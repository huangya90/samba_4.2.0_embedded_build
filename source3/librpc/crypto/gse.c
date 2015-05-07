/*
 *  GSSAPI Security Extensions
 *  RPC Pipe client and server routines
 *  Copyright (C) Simo Sorce 2010.
 *  Copyright (C) Andrew Bartlett 2004-2011.
 *  Copyright (C) Stefan Metzmacher <metze@samba.org> 2004-2005
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

/* We support only GSSAPI/KRB5 here */

#include "includes.h"
#include "gse.h"
#include "libads/kerberos_proto.h"
#include "auth/common_auth.h"
#include "auth/gensec/gensec.h"
#include "auth/gensec/gensec_internal.h"
#include "auth/credentials/credentials.h"
#include "../librpc/gen_ndr/dcerpc.h"

#if defined(HAVE_KRB5)

#include "auth/kerberos/pac_utils.h"
#include "gse_krb5.h"

static char *gse_errstr(TALLOC_CTX *mem_ctx, OM_uint32 maj, OM_uint32 min);

struct gse_context {
	gss_ctx_id_t gssapi_context;
	gss_name_t server_name;
	gss_name_t client_name;
	OM_uint32 gss_want_flags, gss_got_flags;

	gss_cred_id_t delegated_cred_handle;

	NTTIME expire_time;

	/* gensec_gse only */
	krb5_context k5ctx;
	krb5_ccache ccache;
	krb5_keytab keytab;

	gss_OID_desc gss_mech;
	gss_cred_id_t creds;

	gss_OID ret_mech;
};

/* free non talloc dependent contexts */
static int gse_context_destructor(void *ptr)
{
	struct gse_context *gse_ctx;
	OM_uint32 gss_min;

	gse_ctx = talloc_get_type_abort(ptr, struct gse_context);
	if (gse_ctx->k5ctx) {
		if (gse_ctx->ccache) {
			krb5_cc_close(gse_ctx->k5ctx, gse_ctx->ccache);
			gse_ctx->ccache = NULL;
		}
		if (gse_ctx->keytab) {
			krb5_kt_close(gse_ctx->k5ctx, gse_ctx->keytab);
			gse_ctx->keytab = NULL;
		}
		krb5_free_context(gse_ctx->k5ctx);
		gse_ctx->k5ctx = NULL;
	}
	if (gse_ctx->gssapi_context != GSS_C_NO_CONTEXT) {
		(void)gss_delete_sec_context(&gss_min,
						 &gse_ctx->gssapi_context,
						 GSS_C_NO_BUFFER);
	}
	if (gse_ctx->server_name) {
		(void)gss_release_name(&gss_min,
					   &gse_ctx->server_name);
	}
	if (gse_ctx->client_name) {
		(void)gss_release_name(&gss_min,
					   &gse_ctx->client_name);
	}
	if (gse_ctx->creds) {
		(void)gss_release_cred(&gss_min,
					   &gse_ctx->creds);
	}
	if (gse_ctx->delegated_cred_handle) {
		(void)gss_release_cred(&gss_min,
					   &gse_ctx->delegated_cred_handle);
	}

	/* MIT and Heimdal differ as to if you can call
	 * gss_release_oid() on this OID, generated by
	 * gss_{accept,init}_sec_context().  However, as long as the
	 * oid is gss_mech_krb5 (which it always is at the moment),
	 * then this is a moot point, as both declare this particular
	 * OID static, and so no memory is lost.  This assert is in
	 * place to ensure that the programmer who wishes to extend
	 * this code to EAP or other GSS mechanisms determines an
	 * implementation-dependent way of releasing any dynamically
	 * allocated OID */
	SMB_ASSERT(smb_gss_oid_equal(&gse_ctx->gss_mech, GSS_C_NO_OID) ||
		   smb_gss_oid_equal(&gse_ctx->gss_mech, gss_mech_krb5));

	return 0;
}

static NTSTATUS gse_context_init(TALLOC_CTX *mem_ctx,
				 bool do_sign, bool do_seal,
				 const char *ccache_name,
				 uint32_t add_gss_c_flags,
				 struct gse_context **_gse_ctx)
{
	struct gse_context *gse_ctx;
	krb5_error_code k5ret;
	NTSTATUS status;

	gse_ctx = talloc_zero(mem_ctx, struct gse_context);
	if (!gse_ctx) {
		return NT_STATUS_NO_MEMORY;
	}
	talloc_set_destructor((TALLOC_CTX *)gse_ctx, gse_context_destructor);

	gse_ctx->expire_time = GENSEC_EXPIRE_TIME_INFINITY;

	memcpy(&gse_ctx->gss_mech, gss_mech_krb5, sizeof(gss_OID_desc));

	gse_ctx->gss_want_flags = GSS_C_MUTUAL_FLAG |
				GSS_C_DELEG_FLAG |
				GSS_C_DELEG_POLICY_FLAG |
				GSS_C_REPLAY_FLAG |
				GSS_C_SEQUENCE_FLAG;
	if (do_sign) {
		gse_ctx->gss_want_flags |= GSS_C_INTEG_FLAG;
	}
	if (do_seal) {
		gse_ctx->gss_want_flags |= GSS_C_INTEG_FLAG;
		gse_ctx->gss_want_flags |= GSS_C_CONF_FLAG;
	}

	gse_ctx->gss_want_flags |= add_gss_c_flags;

	/* Initialize Kerberos Context */
	initialize_krb5_error_table();

	k5ret = krb5_init_context(&gse_ctx->k5ctx);
	if (k5ret) {
		DEBUG(0, ("Failed to initialize kerberos context! (%s)\n",
			  error_message(k5ret)));
		status = NT_STATUS_INTERNAL_ERROR;
		goto err_out;
	}

	if (!ccache_name) {
		ccache_name = krb5_cc_default_name(gse_ctx->k5ctx);
	}
	k5ret = krb5_cc_resolve(gse_ctx->k5ctx, ccache_name,
				&gse_ctx->ccache);
	if (k5ret) {
		DEBUG(1, ("Failed to resolve credential cache! (%s)\n",
			  error_message(k5ret)));
		status = NT_STATUS_INTERNAL_ERROR;
		goto err_out;
	}

	/* TODO: Should we enforce a enc_types list ?
	ret = krb5_set_default_tgs_ktypes(gse_ctx->k5ctx, enc_types);
	*/

	*_gse_ctx = gse_ctx;
	return NT_STATUS_OK;

err_out:
	TALLOC_FREE(gse_ctx);
	return status;
}

static NTSTATUS gse_init_client(TALLOC_CTX *mem_ctx,
				bool do_sign, bool do_seal,
				const char *ccache_name,
				const char *server,
				const char *service,
				const char *username,
				const char *password,
				uint32_t add_gss_c_flags,
				struct gse_context **_gse_ctx)
{
	struct gse_context *gse_ctx;
	OM_uint32 gss_maj, gss_min;
	gss_buffer_desc name_buffer = {0, NULL};
	gss_OID_set_desc mech_set;
	NTSTATUS status;

	if (!server || !service) {
		return NT_STATUS_INVALID_PARAMETER;
	}

	status = gse_context_init(mem_ctx, do_sign, do_seal,
				  ccache_name, add_gss_c_flags,
				  &gse_ctx);
	if (!NT_STATUS_IS_OK(status)) {
		return NT_STATUS_NO_MEMORY;
	}

	/* Guess the realm based on the supplied service, and avoid the GSS libs
	   doing DNS lookups which may fail.

	   TODO: Loop with the KDC on some more combinations (local
	   realm in particular), possibly falling back to
	   GSS_C_NT_HOSTBASED_SERVICE
	*/
	name_buffer.value = kerberos_get_principal_from_service_hostname(
					gse_ctx, service, server, lp_realm());
	if (!name_buffer.value) {
		status = NT_STATUS_NO_MEMORY;
		goto err_out;
	}
	name_buffer.length = strlen((char *)name_buffer.value);
	gss_maj = gss_import_name(&gss_min, &name_buffer,
				  GSS_C_NT_USER_NAME,
				  &gse_ctx->server_name);
	if (gss_maj) {
		DEBUG(0, ("gss_import_name failed for %s, with [%s]\n",
			  (char *)name_buffer.value,
			  gse_errstr(gse_ctx, gss_maj, gss_min)));
		status = NT_STATUS_INTERNAL_ERROR;
		goto err_out;
	}

	/* TODO: get krb5 ticket using username/password, if no valid
	 * one already available in ccache */

	mech_set.count = 1;
	mech_set.elements = &gse_ctx->gss_mech;

	gss_maj = gss_acquire_cred(&gss_min,
				   GSS_C_NO_NAME,
				   GSS_C_INDEFINITE,
				   &mech_set,
				   GSS_C_INITIATE,
				   &gse_ctx->creds,
				   NULL, NULL);
	if (gss_maj) {
		DEBUG(0, ("gss_acquire_creds failed for %s, with [%s]\n",
			  (char *)name_buffer.value,
			  gse_errstr(gse_ctx, gss_maj, gss_min)));
		status = NT_STATUS_INTERNAL_ERROR;
		goto err_out;
	}

	*_gse_ctx = gse_ctx;
	TALLOC_FREE(name_buffer.value);
	return NT_STATUS_OK;

err_out:
	TALLOC_FREE(name_buffer.value);
	TALLOC_FREE(gse_ctx);
	return status;
}

static NTSTATUS gse_get_client_auth_token(TALLOC_CTX *mem_ctx,
					  struct gse_context *gse_ctx,
					  const DATA_BLOB *token_in,
					  DATA_BLOB *token_out)
{
	OM_uint32 gss_maj, gss_min;
	gss_buffer_desc in_data;
	gss_buffer_desc out_data;
	DATA_BLOB blob = data_blob_null;
	NTSTATUS status;
	OM_uint32 time_rec = 0;
	struct timeval tv;

	in_data.value = token_in->data;
	in_data.length = token_in->length;

	gss_maj = gss_init_sec_context(&gss_min,
					gse_ctx->creds,
					&gse_ctx->gssapi_context,
					gse_ctx->server_name,
					&gse_ctx->gss_mech,
					gse_ctx->gss_want_flags,
					0, GSS_C_NO_CHANNEL_BINDINGS,
					&in_data, NULL, &out_data,
					&gse_ctx->gss_got_flags, &time_rec);
	switch (gss_maj) {
	case GSS_S_COMPLETE:
		/* we are done with it */
		tv = timeval_current_ofs(time_rec, 0);
		gse_ctx->expire_time = timeval_to_nttime(&tv);

		status = NT_STATUS_OK;
		break;
	case GSS_S_CONTINUE_NEEDED:
		/* we will need a third leg */
		status = NT_STATUS_MORE_PROCESSING_REQUIRED;
		break;
	default:
		DEBUG(0, ("gss_init_sec_context failed with [%s]\n",
			  gse_errstr(talloc_tos(), gss_maj, gss_min)));
		status = NT_STATUS_INTERNAL_ERROR;
		goto done;
	}

	/* we may be told to return nothing */
	if (out_data.length) {
		blob = data_blob_talloc(mem_ctx, out_data.value, out_data.length);
		if (!blob.data) {
			status = NT_STATUS_NO_MEMORY;
		}

		gss_maj = gss_release_buffer(&gss_min, &out_data);
	}

done:
	*token_out = blob;
	return status;
}

static NTSTATUS gse_init_server(TALLOC_CTX *mem_ctx,
				bool do_sign, bool do_seal,
				uint32_t add_gss_c_flags,
				struct gse_context **_gse_ctx)
{
	struct gse_context *gse_ctx;
	OM_uint32 gss_maj, gss_min;
	krb5_error_code ret;
	NTSTATUS status;

	status = gse_context_init(mem_ctx, do_sign, do_seal,
				  NULL, add_gss_c_flags, &gse_ctx);
	if (!NT_STATUS_IS_OK(status)) {
		return NT_STATUS_NO_MEMORY;
	}

	ret = gse_krb5_get_server_keytab(gse_ctx->k5ctx,
					 &gse_ctx->keytab);
	if (ret) {
		status = NT_STATUS_INTERNAL_ERROR;
		goto done;
	}

#ifdef HAVE_GSS_KRB5_IMPORT_CRED

	/* This creates a GSSAPI cred_id_t with the keytab set */
	gss_maj = gss_krb5_import_cred(&gss_min, NULL, NULL, gse_ctx->keytab, 
				       &gse_ctx->creds);

	if (gss_maj != 0
	    && gss_maj != (GSS_S_CALL_BAD_STRUCTURE|GSS_S_BAD_NAME)) {
		DEBUG(0, ("gss_krb5_import_cred failed with [%s]\n",
			  gse_errstr(gse_ctx, gss_maj, gss_min)));
		status = NT_STATUS_INTERNAL_ERROR;
		goto done;

		/* This is the error the MIT krb5 1.9 gives when it
		 * implements the function, but we do not specify the
		 * principal.  However, when we specify the principal
		 * as host$@REALM the GSS acceptor fails with 'wrong
		 * principal in request'.  Work around the issue by
		 * falling back to the alternate approach below. */
	} else if (gss_maj == (GSS_S_CALL_BAD_STRUCTURE|GSS_S_BAD_NAME))
#endif
	/* FIXME!!!
	 * This call sets the default keytab for the whole server, not
	 * just for this context. Need to find a way that does not alter
	 * the state of the whole server ... */
	{
		const char *ktname;
		gss_OID_set_desc mech_set;

		ret = smb_krb5_keytab_name(gse_ctx, gse_ctx->k5ctx,
				   gse_ctx->keytab, &ktname);
		if (ret) {
			status = NT_STATUS_INTERNAL_ERROR;
			goto done;
		}

		ret = gsskrb5_register_acceptor_identity(ktname);
		if (ret) {
			status = NT_STATUS_INTERNAL_ERROR;
			goto done;
		}

		mech_set.count = 1;
		mech_set.elements = &gse_ctx->gss_mech;

		gss_maj = gss_acquire_cred(&gss_min,
				   GSS_C_NO_NAME,
				   GSS_C_INDEFINITE,
				   &mech_set,
				   GSS_C_ACCEPT,
				   &gse_ctx->creds,
				   NULL, NULL);

		if (gss_maj) {
			DEBUG(0, ("gss_acquire_creds failed with [%s]\n",
				  gse_errstr(gse_ctx, gss_maj, gss_min)));
			status = NT_STATUS_INTERNAL_ERROR;
			goto done;
		}
	}

	status = NT_STATUS_OK;

done:
	if (!NT_STATUS_IS_OK(status)) {
		TALLOC_FREE(gse_ctx);
	}

	*_gse_ctx = gse_ctx;
	return status;
}

static NTSTATUS gse_get_server_auth_token(TALLOC_CTX *mem_ctx,
					  struct gse_context *gse_ctx,
					  const DATA_BLOB *token_in,
					  DATA_BLOB *token_out)
{
	OM_uint32 gss_maj, gss_min;
	gss_buffer_desc in_data;
	gss_buffer_desc out_data;
	DATA_BLOB blob = data_blob_null;
	NTSTATUS status;
	OM_uint32 time_rec = 0;
	struct timeval tv;

	in_data.value = token_in->data;
	in_data.length = token_in->length;

	gss_maj = gss_accept_sec_context(&gss_min,
					 &gse_ctx->gssapi_context,
					 gse_ctx->creds,
					 &in_data,
					 GSS_C_NO_CHANNEL_BINDINGS,
					 &gse_ctx->client_name,
					 &gse_ctx->ret_mech,
					 &out_data,
					 &gse_ctx->gss_got_flags,
					 &time_rec,
					 &gse_ctx->delegated_cred_handle);
	switch (gss_maj) {
	case GSS_S_COMPLETE:
		/* we are done with it */
		tv = timeval_current_ofs(time_rec, 0);
		gse_ctx->expire_time = timeval_to_nttime(&tv);

		status = NT_STATUS_OK;
		break;
	case GSS_S_CONTINUE_NEEDED:
		/* we will need a third leg */
		status = NT_STATUS_MORE_PROCESSING_REQUIRED;
		break;
	default:
		DEBUG(1, ("gss_accept_sec_context failed with [%s]\n",
			  gse_errstr(talloc_tos(), gss_maj, gss_min)));

		if (gse_ctx->gssapi_context) {
			gss_delete_sec_context(&gss_min,
						&gse_ctx->gssapi_context,
						GSS_C_NO_BUFFER);
		}

		status = NT_STATUS_LOGON_FAILURE;
		goto done;
	}

	/* we may be told to return nothing */
	if (out_data.length) {
		blob = data_blob_talloc(mem_ctx, out_data.value, out_data.length);
		if (!blob.data) {
			status = NT_STATUS_NO_MEMORY;
		}
		gss_maj = gss_release_buffer(&gss_min, &out_data);
	}


done:
	*token_out = blob;
	return status;
}

static char *gse_errstr(TALLOC_CTX *mem_ctx, OM_uint32 maj, OM_uint32 min)
{
	OM_uint32 gss_min, gss_maj;
	gss_buffer_desc msg_min;
	gss_buffer_desc msg_maj;
	OM_uint32 msg_ctx = 0;

	char *errstr = NULL;

	ZERO_STRUCT(msg_min);
	ZERO_STRUCT(msg_maj);

	gss_maj = gss_display_status(&gss_min, maj, GSS_C_GSS_CODE,
				     GSS_C_NO_OID, &msg_ctx, &msg_maj);
	if (gss_maj) {
		goto done;
	}
	errstr = talloc_strndup(mem_ctx,
				(char *)msg_maj.value,
					msg_maj.length);
	if (!errstr) {
		goto done;
	}
	gss_maj = gss_display_status(&gss_min, min, GSS_C_MECH_CODE,
				     (gss_OID)discard_const(gss_mech_krb5),
				     &msg_ctx, &msg_min);
	if (gss_maj) {
		goto done;
	}

	errstr = talloc_strdup_append_buffer(errstr, ": ");
	if (!errstr) {
		goto done;
	}
	errstr = talloc_strndup_append_buffer(errstr,
						(char *)msg_min.value,
							msg_min.length);
	if (!errstr) {
		goto done;
	}

done:
	if (msg_min.value) {
		gss_maj = gss_release_buffer(&gss_min, &msg_min);
	}
	if (msg_maj.value) {
		gss_maj = gss_release_buffer(&gss_min, &msg_maj);
	}
	return errstr;
}

static size_t gse_get_signature_length(struct gse_context *gse_ctx,
				       bool seal, size_t payload_size)
{
	OM_uint32 gss_min, gss_maj;
	gss_iov_buffer_desc iov[2];
	int sealed;

	/*
	 * gss_wrap_iov_length() only needs the type and length
	 */
	iov[0].type = GSS_IOV_BUFFER_TYPE_HEADER;
	iov[0].buffer.value = NULL;
	iov[0].buffer.length = 0;
	iov[1].type = GSS_IOV_BUFFER_TYPE_DATA;
	iov[1].buffer.value = NULL;
	iov[1].buffer.length = payload_size;

	gss_maj = gss_wrap_iov_length(&gss_min, gse_ctx->gssapi_context,
					seal, GSS_C_QOP_DEFAULT,
					&sealed, iov, 2);
	if (gss_maj) {
		DEBUG(0, ("gss_wrap_iov_length failed with [%s]\n",
			  gse_errstr(talloc_tos(), gss_maj, gss_min)));
		return 0;
	}

	return iov[0].buffer.length;
}

static NTSTATUS gse_seal(TALLOC_CTX *mem_ctx, struct gse_context *gse_ctx,
			 DATA_BLOB *data, DATA_BLOB *signature)
{
	OM_uint32 gss_min, gss_maj;
	gss_iov_buffer_desc iov[2];
	int req_seal = 1; /* setting to 1 means we request sign+seal */
	int sealed = 1;
	NTSTATUS status;

	/* allocate the memory ourselves so we do not need to talloc_memdup */
	signature->length = gse_get_signature_length(gse_ctx, true, data->length);
	if (!signature->length) {
		return NT_STATUS_INTERNAL_ERROR;
	}
	signature->data = (uint8_t *)talloc_size(mem_ctx, signature->length);
	if (!signature->data) {
		return NT_STATUS_NO_MEMORY;
	}
	iov[0].type = GSS_IOV_BUFFER_TYPE_HEADER;
	iov[0].buffer.value = signature->data;
	iov[0].buffer.length = signature->length;

	/* data is encrypted in place, which is ok */
	iov[1].type = GSS_IOV_BUFFER_TYPE_DATA;
	iov[1].buffer.value = data->data;
	iov[1].buffer.length = data->length;

	gss_maj = gss_wrap_iov(&gss_min, gse_ctx->gssapi_context,
				req_seal, GSS_C_QOP_DEFAULT,
				&sealed, iov, 2);
	if (gss_maj) {
		DEBUG(0, ("gss_wrap_iov failed with [%s]\n",
			  gse_errstr(talloc_tos(), gss_maj, gss_min)));
		status = NT_STATUS_ACCESS_DENIED;
		goto done;
	}

	if (!sealed) {
		DEBUG(0, ("gss_wrap_iov says data was not sealed!\n"));
		status = NT_STATUS_ACCESS_DENIED;
		goto done;
	}

	status = NT_STATUS_OK;

	DEBUG(10, ("Sealed %d bytes, and got %d bytes header/signature.\n",
		   (int)iov[1].buffer.length, (int)iov[0].buffer.length));

done:
	return status;
}

static NTSTATUS gse_unseal(TALLOC_CTX *mem_ctx, struct gse_context *gse_ctx,
			   DATA_BLOB *data, const DATA_BLOB *signature)
{
	OM_uint32 gss_min, gss_maj;
	gss_iov_buffer_desc iov[2];
	int sealed;
	NTSTATUS status;

	iov[0].type = GSS_IOV_BUFFER_TYPE_HEADER;
	iov[0].buffer.value = signature->data;
	iov[0].buffer.length = signature->length;

	/* data is decrypted in place, which is ok */
	iov[1].type = GSS_IOV_BUFFER_TYPE_DATA;
	iov[1].buffer.value = data->data;
	iov[1].buffer.length = data->length;

	gss_maj = gss_unwrap_iov(&gss_min, gse_ctx->gssapi_context,
				 &sealed, NULL, iov, 2);
	if (gss_maj) {
		DEBUG(0, ("gss_unwrap_iov failed with [%s]\n",
			  gse_errstr(talloc_tos(), gss_maj, gss_min)));
		status = NT_STATUS_ACCESS_DENIED;
		goto done;
	}

	if (!sealed) {
		DEBUG(0, ("gss_unwrap_iov says data is not sealed!\n"));
		status = NT_STATUS_ACCESS_DENIED;
		goto done;
	}

	status = NT_STATUS_OK;

	DEBUG(10, ("Unsealed %d bytes, with %d bytes header/signature.\n",
		   (int)iov[1].buffer.length, (int)iov[0].buffer.length));

done:
	return status;
}

static NTSTATUS gse_sign(TALLOC_CTX *mem_ctx, struct gse_context *gse_ctx,
			 DATA_BLOB *data, DATA_BLOB *signature)
{
	OM_uint32 gss_min, gss_maj;
	gss_buffer_desc in_data = { 0, NULL };
	gss_buffer_desc out_data = { 0, NULL};
	NTSTATUS status;

	in_data.value = data->data;
	in_data.length = data->length;

	gss_maj = gss_get_mic(&gss_min, gse_ctx->gssapi_context,
			      GSS_C_QOP_DEFAULT,
			      &in_data, &out_data);
	if (gss_maj) {
		DEBUG(0, ("gss_get_mic failed with [%s]\n",
			  gse_errstr(talloc_tos(), gss_maj, gss_min)));
		status = NT_STATUS_ACCESS_DENIED;
		goto done;
	}

	*signature = data_blob_talloc(mem_ctx,
					out_data.value, out_data.length);
	if (!signature->data) {
		status = NT_STATUS_NO_MEMORY;
		goto done;
	}

	status = NT_STATUS_OK;

done:
	if (out_data.value) {
		gss_maj = gss_release_buffer(&gss_min, &out_data);
	}
	return status;
}

static NTSTATUS gse_sigcheck(TALLOC_CTX *mem_ctx, struct gse_context *gse_ctx,
			     const DATA_BLOB *data, const DATA_BLOB *signature)
{
	OM_uint32 gss_min, gss_maj;
	gss_buffer_desc in_data = { 0, NULL };
	gss_buffer_desc in_token = { 0, NULL};
	NTSTATUS status;

	in_data.value = data->data;
	in_data.length = data->length;
	in_token.value = signature->data;
	in_token.length = signature->length;

	gss_maj = gss_verify_mic(&gss_min, gse_ctx->gssapi_context,
				 &in_data, &in_token, NULL);
	if (gss_maj) {
		DEBUG(0, ("gss_verify_mic failed with [%s]\n",
			  gse_errstr(talloc_tos(), gss_maj, gss_min)));
		status = NT_STATUS_ACCESS_DENIED;
		goto done;
	}

	status = NT_STATUS_OK;

done:
	return status;
}

static NTSTATUS gensec_gse_client_start(struct gensec_security *gensec_security)
{
	struct gse_context *gse_ctx;
	struct cli_credentials *creds = gensec_get_credentials(gensec_security);
	NTSTATUS nt_status;
	OM_uint32 want_flags = 0;
	bool do_sign = false, do_seal = false;
	const char *hostname = gensec_get_target_hostname(gensec_security);
	const char *service = gensec_get_target_service(gensec_security);
	const char *username = cli_credentials_get_username(creds);
	const char *password = cli_credentials_get_password(creds);

	if (!hostname) {
		DEBUG(1, ("Could not determine hostname for target computer, cannot use kerberos\n"));
		return NT_STATUS_INVALID_PARAMETER;
	}
	if (is_ipaddress(hostname)) {
		DEBUG(2, ("Cannot do GSE to an IP address\n"));
		return NT_STATUS_INVALID_PARAMETER;
	}
	if (strcmp(hostname, "localhost") == 0) {
		DEBUG(2, ("GSE to 'localhost' does not make sense\n"));
		return NT_STATUS_INVALID_PARAMETER;
	}

	if (gensec_security->want_features & GENSEC_FEATURE_SIGN) {
		do_sign = true;
	}
	if (gensec_security->want_features & GENSEC_FEATURE_SEAL) {
		do_seal = true;
	}
	if (gensec_security->want_features & GENSEC_FEATURE_DCE_STYLE) {
		want_flags |= GSS_C_DCE_STYLE;
	}

	nt_status = gse_init_client(gensec_security, do_sign, do_seal, NULL,
				    hostname, service,
				    username, password, want_flags,
				    &gse_ctx);
	if (!NT_STATUS_IS_OK(nt_status)) {
		return nt_status;
	}
	gensec_security->private_data = gse_ctx;
	return NT_STATUS_OK;
}

static NTSTATUS gensec_gse_server_start(struct gensec_security *gensec_security)
{
	struct gse_context *gse_ctx;
	NTSTATUS nt_status;
	OM_uint32 want_flags = 0;
	bool do_sign = false, do_seal = false;

	if (gensec_security->want_features & GENSEC_FEATURE_SIGN) {
		do_sign = true;
	}
	if (gensec_security->want_features & GENSEC_FEATURE_SEAL) {
		do_seal = true;
	}
	if (gensec_security->want_features & GENSEC_FEATURE_DCE_STYLE) {
		want_flags |= GSS_C_DCE_STYLE;
	}

	nt_status = gse_init_server(gensec_security, do_sign, do_seal, want_flags,
				    &gse_ctx);
	if (!NT_STATUS_IS_OK(nt_status)) {
		return nt_status;
	}
	gensec_security->private_data = gse_ctx;
	return NT_STATUS_OK;
}

/**
 * Next state function for the GSE GENSEC mechanism
 *
 * @param gensec_gse_state GSE State
 * @param mem_ctx The TALLOC_CTX for *out to be allocated on
 * @param in The request, as a DATA_BLOB
 * @param out The reply, as an talloc()ed DATA_BLOB, on *mem_ctx
 * @return Error, MORE_PROCESSING_REQUIRED if a reply is sent,
 *                or NT_STATUS_OK if the user is authenticated.
 */

static NTSTATUS gensec_gse_update(struct gensec_security *gensec_security,
				  TALLOC_CTX *mem_ctx,
				  struct tevent_context *ev,
				  const DATA_BLOB in, DATA_BLOB *out)
{
	NTSTATUS status;
	struct gse_context *gse_ctx =
		talloc_get_type_abort(gensec_security->private_data,
		struct gse_context);

	switch (gensec_security->gensec_role) {
	case GENSEC_CLIENT:
		status = gse_get_client_auth_token(mem_ctx, gse_ctx,
						   &in, out);
		break;
	case GENSEC_SERVER:
		status = gse_get_server_auth_token(mem_ctx, gse_ctx,
						   &in, out);
		break;
	}
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	return NT_STATUS_OK;
}

static NTSTATUS gensec_gse_wrap(struct gensec_security *gensec_security,
				TALLOC_CTX *mem_ctx,
				const DATA_BLOB *in,
				DATA_BLOB *out)
{
	struct gse_context *gse_ctx =
		talloc_get_type_abort(gensec_security->private_data,
		struct gse_context);
	OM_uint32 maj_stat, min_stat;
	gss_buffer_desc input_token, output_token;
	int conf_state;
	input_token.length = in->length;
	input_token.value = in->data;

	maj_stat = gss_wrap(&min_stat,
			    gse_ctx->gssapi_context,
			    gensec_have_feature(gensec_security, GENSEC_FEATURE_SEAL),
			    GSS_C_QOP_DEFAULT,
			    &input_token,
			    &conf_state,
			    &output_token);
	if (GSS_ERROR(maj_stat)) {
		DEBUG(0, ("gensec_gse_wrap: GSS Wrap failed: %s\n",
			  gse_errstr(talloc_tos(), maj_stat, min_stat)));
		return NT_STATUS_ACCESS_DENIED;
	}

	*out = data_blob_talloc(mem_ctx, output_token.value, output_token.length);
	gss_release_buffer(&min_stat, &output_token);

	if (gensec_have_feature(gensec_security, GENSEC_FEATURE_SEAL)
	    && !conf_state) {
		return NT_STATUS_ACCESS_DENIED;
	}
	return NT_STATUS_OK;
}

static NTSTATUS gensec_gse_unwrap(struct gensec_security *gensec_security,
				     TALLOC_CTX *mem_ctx,
				     const DATA_BLOB *in,
				     DATA_BLOB *out)
{
	struct gse_context *gse_ctx =
		talloc_get_type_abort(gensec_security->private_data,
		struct gse_context);
	OM_uint32 maj_stat, min_stat;
	gss_buffer_desc input_token, output_token;
	int conf_state;
	gss_qop_t qop_state;
	input_token.length = in->length;
	input_token.value = in->data;

	maj_stat = gss_unwrap(&min_stat,
			      gse_ctx->gssapi_context,
			      &input_token,
			      &output_token,
			      &conf_state,
			      &qop_state);
	if (GSS_ERROR(maj_stat)) {
		DEBUG(0, ("gensec_gse_unwrap: GSS UnWrap failed: %s\n",
			  gse_errstr(talloc_tos(), maj_stat, min_stat)));
		return NT_STATUS_ACCESS_DENIED;
	}

	*out = data_blob_talloc(mem_ctx, output_token.value, output_token.length);
	gss_release_buffer(&min_stat, &output_token);

	if (gensec_have_feature(gensec_security, GENSEC_FEATURE_SEAL)
	    && !conf_state) {
		return NT_STATUS_ACCESS_DENIED;
	}
	return NT_STATUS_OK;
}

static NTSTATUS gensec_gse_seal_packet(struct gensec_security *gensec_security,
				       TALLOC_CTX *mem_ctx,
				       uint8_t *data, size_t length,
				       const uint8_t *whole_pdu, size_t pdu_length,
				       DATA_BLOB *sig)
{
	struct gse_context *gse_ctx =
		talloc_get_type_abort(gensec_security->private_data,
		struct gse_context);
	DATA_BLOB payload = data_blob_const(data, length);
	return gse_seal(mem_ctx, gse_ctx, &payload, sig);
}

static NTSTATUS gensec_gse_unseal_packet(struct gensec_security *gensec_security,
					 uint8_t *data, size_t length,
					 const uint8_t *whole_pdu, size_t pdu_length,
					 const DATA_BLOB *sig)
{
	struct gse_context *gse_ctx =
		talloc_get_type_abort(gensec_security->private_data,
		struct gse_context);
	DATA_BLOB payload = data_blob_const(data, length);
	return gse_unseal(talloc_tos() /* unused */, gse_ctx, &payload, sig);
}

static NTSTATUS gensec_gse_sign_packet(struct gensec_security *gensec_security,
				       TALLOC_CTX *mem_ctx,
				       const uint8_t *data, size_t length,
				       const uint8_t *whole_pdu, size_t pdu_length,
				       DATA_BLOB *sig)
{
	struct gse_context *gse_ctx =
		talloc_get_type_abort(gensec_security->private_data,
		struct gse_context);
	DATA_BLOB payload = data_blob_const(data, length);
	return gse_sign(mem_ctx, gse_ctx, &payload, sig);
}

static NTSTATUS gensec_gse_check_packet(struct gensec_security *gensec_security,
					const uint8_t *data, size_t length,
					const uint8_t *whole_pdu, size_t pdu_length,
					const DATA_BLOB *sig)
{
	struct gse_context *gse_ctx =
		talloc_get_type_abort(gensec_security->private_data,
		struct gse_context);
	DATA_BLOB payload = data_blob_const(data, length);
	return gse_sigcheck(NULL, gse_ctx, &payload, sig);
}

/* Try to figure out what features we actually got on the connection */
static bool gensec_gse_have_feature(struct gensec_security *gensec_security,
				    uint32_t feature)
{
	struct gse_context *gse_ctx =
		talloc_get_type_abort(gensec_security->private_data,
		struct gse_context);

	if (feature & GENSEC_FEATURE_SIGN) {
		return gse_ctx->gss_got_flags & GSS_C_INTEG_FLAG;
	}
	if (feature & GENSEC_FEATURE_SEAL) {
		return gse_ctx->gss_got_flags & GSS_C_CONF_FLAG;
	}
	if (feature & GENSEC_FEATURE_SESSION_KEY) {
		/* Only for GSE/Krb5 */
		if (smb_gss_oid_equal(gse_ctx->ret_mech, gss_mech_krb5)) {
			return true;
		}
	}
	if (feature & GENSEC_FEATURE_DCE_STYLE) {
		return gse_ctx->gss_got_flags & GSS_C_DCE_STYLE;
	}
	if (feature & GENSEC_FEATURE_NEW_SPNEGO) {
		NTSTATUS status;
		uint32_t keytype;

		if (!(gse_ctx->gss_got_flags & GSS_C_INTEG_FLAG)) {
			return false;
		}

		status = gssapi_get_session_key(talloc_tos(), 
						gse_ctx->gssapi_context, NULL, &keytype);
		/* 
		 * We should do a proper sig on the mechListMic unless
		 * we know we have to be backwards compatible with
		 * earlier windows versions.  
		 * 
		 * Negotiating a non-krb5
		 * mech for example should be regarded as having
		 * NEW_SPNEGO
		 */
		if (NT_STATUS_IS_OK(status)) {
			switch (keytype) {
			case ENCTYPE_DES_CBC_CRC:
			case ENCTYPE_DES_CBC_MD5:
			case ENCTYPE_ARCFOUR_HMAC:
			case ENCTYPE_DES3_CBC_SHA1:
				return false;
			}
		}
		return true;
	}
	/* We can always do async (rather than strict request/reply) packets.  */
	if (feature & GENSEC_FEATURE_ASYNC_REPLIES) {
		return true;
	}
	return false;
}

static NTTIME gensec_gse_expire_time(struct gensec_security *gensec_security)
{
	struct gse_context *gse_ctx =
		talloc_get_type_abort(gensec_security->private_data,
		struct gse_context);

	return gse_ctx->expire_time;
}

/*
 * Extract the 'sesssion key' needed by SMB signing and ncacn_np
 * (for encrypting some passwords).
 *
 * This breaks all the abstractions, but what do you expect...
 */
static NTSTATUS gensec_gse_session_key(struct gensec_security *gensec_security,
				       TALLOC_CTX *mem_ctx,
				       DATA_BLOB *session_key)
{
	struct gse_context *gse_ctx =
		talloc_get_type_abort(gensec_security->private_data,
		struct gse_context);

	return gssapi_get_session_key(mem_ctx, gse_ctx->gssapi_context, session_key, NULL);
}

/* Get some basic (and authorization) information about the user on
 * this session.  This uses either the PAC (if present) or a local
 * database lookup */
static NTSTATUS gensec_gse_session_info(struct gensec_security *gensec_security,
					TALLOC_CTX *mem_ctx,
					struct auth_session_info **_session_info)
{
	struct gse_context *gse_ctx =
		talloc_get_type_abort(gensec_security->private_data,
		struct gse_context);
	NTSTATUS nt_status;
	TALLOC_CTX *tmp_ctx;
	struct auth_session_info *session_info = NULL;
	OM_uint32 maj_stat, min_stat;
	DATA_BLOB pac_blob, *pac_blob_ptr = NULL;

	gss_buffer_desc name_token;
	char *principal_string;

	tmp_ctx = talloc_named(mem_ctx, 0, "gensec_gse_session_info context");
	NT_STATUS_HAVE_NO_MEMORY(tmp_ctx);

	maj_stat = gss_display_name(&min_stat,
				    gse_ctx->client_name,
				    &name_token,
				    NULL);
	if (GSS_ERROR(maj_stat)) {
		DEBUG(1, ("GSS display_name failed: %s\n",
			  gse_errstr(talloc_tos(), maj_stat, min_stat)));
		talloc_free(tmp_ctx);
		return NT_STATUS_FOOBAR;
	}

	principal_string = talloc_strndup(tmp_ctx,
					  (const char *)name_token.value,
					  name_token.length);

	gss_release_buffer(&min_stat, &name_token);

	if (!principal_string) {
		talloc_free(tmp_ctx);
		return NT_STATUS_NO_MEMORY;
	}

	nt_status = gssapi_obtain_pac_blob(tmp_ctx,  gse_ctx->gssapi_context,
					   gse_ctx->client_name,
					   &pac_blob);

	/* IF we have the PAC - otherwise we need to get this
	 * data from elsewere
	 */
	if (NT_STATUS_IS_OK(nt_status)) {
		pac_blob_ptr = &pac_blob;
	}
	nt_status = gensec_generate_session_info_pac(tmp_ctx,
						     gensec_security,
						     NULL,
						     pac_blob_ptr, principal_string,
						     gensec_get_remote_address(gensec_security),
						     &session_info);
	if (!NT_STATUS_IS_OK(nt_status)) {
		talloc_free(tmp_ctx);
		return nt_status;
	}

	nt_status = gensec_gse_session_key(gensec_security, session_info,
					   &session_info->session_key);
	if (!NT_STATUS_IS_OK(nt_status)) {
		talloc_free(tmp_ctx);
		return nt_status;
	}

	*_session_info = talloc_move(mem_ctx, &session_info);
	talloc_free(tmp_ctx);

	return NT_STATUS_OK;
}

static size_t gensec_gse_sig_size(struct gensec_security *gensec_security,
				  size_t data_size)
{
	struct gse_context *gse_ctx =
		talloc_get_type_abort(gensec_security->private_data,
		struct gse_context);

	return gse_get_signature_length(gse_ctx,
					gensec_security->want_features & GENSEC_FEATURE_SEAL,
					data_size);
}

static const char *gensec_gse_krb5_oids[] = {
	GENSEC_OID_KERBEROS5_OLD,
	GENSEC_OID_KERBEROS5,
	NULL
};

const struct gensec_security_ops gensec_gse_krb5_security_ops = {
	.name		= "gse_krb5",
	.auth_type	= DCERPC_AUTH_TYPE_KRB5,
	.oid            = gensec_gse_krb5_oids,
	.client_start   = gensec_gse_client_start,
	.server_start   = gensec_gse_server_start,
	.magic  	= gensec_magic_check_krb5_oid,
	.update 	= gensec_gse_update,
	.session_key	= gensec_gse_session_key,
	.session_info	= gensec_gse_session_info,
	.sig_size	= gensec_gse_sig_size,
	.sign_packet	= gensec_gse_sign_packet,
	.check_packet	= gensec_gse_check_packet,
	.seal_packet	= gensec_gse_seal_packet,
	.unseal_packet	= gensec_gse_unseal_packet,
	.wrap           = gensec_gse_wrap,
	.unwrap         = gensec_gse_unwrap,
	.have_feature   = gensec_gse_have_feature,
	.expire_time    = gensec_gse_expire_time,
	.enabled        = true,
	.kerberos       = true,
	.priority       = GENSEC_GSSAPI
};

#endif /* HAVE_KRB5 */
