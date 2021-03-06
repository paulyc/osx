/*
   Unix SMB/CIFS implementation.
   Password and authentication handling
   Copyright (C) Andrew Tridgell              1992-2000
   Copyright (C) Luke Kenneth Casson Leighton 1996-2000
   Copyright (C) Andrew Bartlett              2001

   Copyright (C) 2003-2007 Apple Inc. All Rights Reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include "includes.h"

#undef DBGC_CLASS
#define DBGC_CLASS DBGC_AUTH

#undef u32
#include "opendirectory.h"

#define MODULE_NAME "odsam"

static enum ds_trace_level ds_trace = DS_TRACE_ERRORS;
static int module_debug;

static tDirNodeReference getusernode(struct opendirectory_session *session,
				    const char *userName)
{
    tDirStatus			status			= eDSNoErr;
    UInt32			returnCount		= 0;
    tDataBufferPtr		dataBuffer		= NULL;
    tDataListPtr		searchNodeName		= NULL;
    tDirNodeReference		userNodeRef		= 0;
    tDataListPtr		userNodePath		= NULL;
    char			userNodePathStr[256]	= {0};
    char			recUserName[128]	= {0};
    tDataListPtr		recName			= NULL;
    tDataListPtr		recType			= NULL;
    tDataListPtr		attrType		= NULL;

    tAttributeListRef		attributeListRef	= 0;
    tRecordEntryPtr		outRecordEntryPtr	= NULL;
    tAttributeEntryPtr		attributeInfo		= NULL;
    tAttributeValueListRef	attributeValueListRef	= 0;
    tAttributeValueEntryPtr	attrValue		= NULL;
    UInt32			i			= 0;

    dataBuffer = dsDataBufferAllocate(session->ref, DEFAULT_DS_BUFFER_SIZE);
    if (dataBuffer == NULL) {
	goto cleanup;
    }

    status = opendirectory_searchnode(session);
    LOG_DS_ERROR(ds_trace, status, "opendirectory_searchnode");
    if (status != eDSNoErr) {
	goto cleanup;
    }

    recName = dsBuildListFromStrings(session->ref, userName, NULL);
    recType = dsBuildListFromStrings(session->ref, kDSStdRecordTypeUsers, NULL);
    attrType = dsBuildListFromStrings(session->ref, kDSNAttrMetaNodeLocation,
			kDSNAttrRecordName, NULL);

    status = dsGetRecordList(session->search, dataBuffer, recName, eDSiExact,
			recType, attrType, 0, &returnCount, NULL);
    LOG_DS_ERROR(ds_trace, status, "dsGetRecordList");
    if (status != eDSNoErr) {
	goto cleanup;
    }

    status = dsGetRecordEntry(session->search, dataBuffer, 1,
			&attributeListRef, &outRecordEntryPtr);
    LOG_DS_ERROR(ds_trace, status, "dsGetRecordEntry");
    if (status != eDSNoErr) {
	goto cleanup;
    }

    for (i = 1 ; i <= outRecordEntryPtr->fRecordAttributeCount; i++) {
	status = dsGetAttributeEntry(session->search, dataBuffer,
			attributeListRef, i, &attributeValueListRef,
			&attributeInfo);
	LOG_DS_ERROR(ds_trace, status, "dsGetAttributeEntry");

	status = dsGetAttributeValue(session->search, dataBuffer, 1,
			attributeValueListRef, &attrValue);
	LOG_DS_ERROR(ds_trace, status, "dsGetAttributeValue");

	if (status == eDSNoErr) {
	    if (strncmp(attributeInfo->fAttributeSignature.fBufferData,
			kDSNAttrMetaNodeLocation,
			strlen(kDSNAttrMetaNodeLocation)) == 0) {
		SMB_ASSERT(attrValue->fAttributeValueData.fBufferSize <
			    sizeof(userNodePathStr));
		strncpy(userNodePathStr,
			attrValue->fAttributeValueData.fBufferData,
			attrValue->fAttributeValueData.fBufferSize);
	    } else if (strncmp(attributeInfo->fAttributeSignature.fBufferData,
			kDSNAttrRecordName, strlen(kDSNAttrRecordName)) == 0) {
		SMB_ASSERT(attrValue->fAttributeValueData.fBufferSize <
			    sizeof(recUserName));
		strncpy(recUserName,
			attrValue->fAttributeValueData.fBufferData,
			attrValue->fAttributeValueData.fBufferSize);
	    }
	}

	if (attrValue != NULL) {
	    dsDeallocAttributeValueEntry(session->ref, attrValue);
	    attrValue = NULL;
	}

	if (attributeValueListRef != 0) {
	    dsCloseAttributeValueList(attributeValueListRef);
	    attributeValueListRef = 0;
	}

	if (attributeInfo != NULL) {
	    dsDeallocAttributeEntry(session->ref, attributeInfo);
	    attributeInfo = NULL;
	}
    }

    if (outRecordEntryPtr != NULL) {
	dsDeallocRecordEntry(session->ref, outRecordEntryPtr);
	outRecordEntryPtr = NULL;
    }

    if (strlen(userNodePathStr) != 0 && strlen(recUserName) != 0) {
	userNodePath = dsBuildFromPath(session->ref, userNodePathStr, "/");

	status = dsOpenDirNode(session->ref, userNodePath, &userNodeRef);
	LOG_DS_ERROR(ds_trace, status, "dsOpenDirNode");

	opendirectory_free_list(session, userNodePath);
    }

cleanup:
    DS_CLOSE_NODE(session->search);

    opendirectory_free_buffer(session, dataBuffer);
    opendirectory_free_list(session, searchNodeName);
    opendirectory_free_list(session, recName);
    opendirectory_free_list(session, recType);
    opendirectory_free_list(session, attrType);

    return userNodeRef;
}

/* FIXME: this partial mapping should be completed and hoisted into library
 * code -- jpeach
 */
static NTSTATUS map_dserr_to_nterr(tDirStatus dirStatus)
{
	switch (dirStatus) {
	case (eDSAuthFailed):
	case (eDSAuthBadPassword):
		return NT_STATUS_WRONG_PASSWORD;
	case (eDSAuthAccountInactive):
		return NT_STATUS_ACCOUNT_DISABLED;
	case (eDSAuthNewPasswordRequired):
	case (eDSAuthPasswordExpired):
		return NT_STATUS_PASSWORD_MUST_CHANGE;
	default:
		return NT_STATUS_WRONG_PASSWORD;
	}
}

static tDirStatus opendirectory_auth_user(
			struct opendirectory_session *session,
			tDirNodeReference userNode,
			const char* user,
			u_int8_t *challenge,
			u_int8_t *password,
			const char *inAuthMethod)
{
	tDirStatus 		status			= eDSAuthServerError;
	size_t			curr			= 0;
	UInt32			len			= 0;
	tDataBufferPtr		authBuff  		= NULL;
	tDataBufferPtr		stepBuff  		= NULL;
	tDataNodePtr		authType		= NULL;

        authBuff = dsDataBufferAllocate( session->ref, 2048 );
        if ( authBuff == NULL ) {
                DEBUG(module_debug, ("*** dsDataBufferAllocate failed\n" ));
		return eMemoryAllocError;
	}

	stepBuff = dsDataBufferAllocate( session->ref, 2048 );
	if ( stepBuff == NULL ) {
		dsDataBufferDeAllocate( session->ref, authBuff );
                DEBUG(module_debug, ("*** dsDataBufferAllocate failed\n" ));
		return eMemoryAllocError;
	}

	authType = dsDataNodeAllocateString( session->ref,  inAuthMethod);
	if ( authType != NULL ) {
		// User Name
		len = (UInt32)strlen( user );
		memcpy( &(authBuff->fBufferData[ curr ]), &len, sizeof(len) );
		curr += sizeof( len );
		memcpy( &(authBuff->fBufferData[ curr ]), user, len );
		curr += len;
		// C8
		len = 8;
		memcpy( &(authBuff->fBufferData[ curr ]), &len, sizeof(len) );
		curr += sizeof (len );
		memcpy( &(authBuff->fBufferData[ curr ]), challenge, len );
		curr += len;
		// P24
		len = 24;
		memcpy( &(authBuff->fBufferData[ curr ]), &len, sizeof(len) );
		curr += sizeof (len );
		memcpy( &(authBuff->fBufferData[ curr ]), password, len );
		curr += len;

		if (curr > UINT32_MAX)
		    smb_panic("Looks like fBufferLength overflowed\n");
		authBuff->fBufferLength = (UInt32)curr;

		status = dsDoDirNodeAuth( userNode, authType, True,
				authBuff, stepBuff, NULL );
		LOG_DS_ERROR(ds_trace, status, "dsDoNodeAuth");
		DEBUG(module_debug, ("User \"%s\" %s ""with \"%s\"\n", user,
			    status == eDSNoErr ? "authenticated successfully"
					    : "failed to authenticate",
			    inAuthMethod));
	}

	opendirectory_free_buffer(session, stepBuff);
	opendirectory_free_buffer(session, authBuff);
	opendirectory_free_node(session, authType);

	return status;
}

static tDirStatus opendirectory_ntlmv2_auth_user(
			    struct opendirectory_session *session,
			    tDirNodeReference userNode,
			    const char* user,
			    const char* domain,
			    const DATA_BLOB *sec_blob,
			    const DATA_BLOB *ntv2_response,
			    DATA_BLOB *user_sess_key)
{
	static const char const method[] =
			"dsAuthMethodStandard:dsAuthNodeNTLMv2";

	tDirStatus 		status		= eDSAuthServerError;
	size_t			curr		= 0;
	UInt32			len		= 0;
	tDataBufferPtr		authBuff  	= NULL;
	tDataBufferPtr		stepBuff  	= NULL;
	tDataNodePtr		authType	= NULL;

/*


	The auth method constant is: dsAuthMethodStandard:dsAuthNodeNTLMv2
	The format for data in the step buffer is:
	4 byte len + directory-services name
	4 byte len + server challenge
	4 byte len + client "blob" - 16 bytes of client digest + the blob data
	4 byte len + user name used in the digest (usually the same as item #1 in the buffer)
	4 byte len + domain


*/

        authBuff = dsDataBufferAllocate( session->ref, 2048 );
        if ( authBuff == NULL ) {
                DEBUG(module_debug, ("*** dsDataBufferAllocate failed\n" ));
		return eMemoryAllocError;
	}

	stepBuff = dsDataBufferAllocate( session->ref, 2048 );
	if ( stepBuff == NULL ) {
		 dsDataBufferDeAllocate( session->ref, authBuff );
                DEBUG(module_debug, ("*** dsDataBufferAllocate failed\n" ));
		return eMemoryAllocError;
	}

	authType = dsDataNodeAllocateString( session->ref, method );
	if ( authType != NULL ) {
		// directory-services name
		len = (UInt32)strlen( user );
		memcpy( &(authBuff->fBufferData[ curr ]), &len, sizeof(len) );
		curr += sizeof( len );
		memcpy( &(authBuff->fBufferData[ curr ]), user, len );
		curr += len;
		// server challenge
		len = 8;
		memcpy( &(authBuff->fBufferData[ curr ]), &len, sizeof(len) );
		curr += sizeof (len );
		memcpy( &(authBuff->fBufferData[ curr ]), sec_blob->data, len );
		curr += len;
		// client "blob" - 16 bytes of client digest + the blob_data
		len = ntv2_response->length;
		memcpy( &(authBuff->fBufferData[ curr ]), &len, sizeof(len) );
		curr += sizeof (len );
		memcpy( &(authBuff->fBufferData[ curr ]), ntv2_response->data, len );
		curr += len;
		 // user name used in the digest (usually the same as item #1 in the buffer)
		len = (UInt32)strlen( user );
		memcpy( &(authBuff->fBufferData[ curr ]), &len, sizeof(len) );
		curr += sizeof( len );
		memcpy( &(authBuff->fBufferData[ curr ]), user, len );
		curr += len;
		// domain
		len = (UInt32)strlen( domain );
		memcpy( &(authBuff->fBufferData[ curr ]), &len, sizeof(len) );
		curr += sizeof( len );
		memcpy( &(authBuff->fBufferData[ curr ]), domain, len );
		curr += len;

		if (curr > UINT32_MAX)
		    smb_panic("Looks like fBufferLength overflowed\n");
		authBuff->fBufferLength = (UInt32)curr;

		status = dsDoDirNodeAuth( userNode, authType, True,
				authBuff, stepBuff, NULL );
		LOG_DS_ERROR(ds_trace, status, "dsDoNodeAuth");
		DEBUG(module_debug, ("User \"%s\" %s ""with \"%s\"\n", user,
			    status == eDSNoErr ? "authenticated successfully"
					    : "failed to authenticate",
			    method));
	}

	opendirectory_free_buffer(session, stepBuff);
	opendirectory_free_buffer(session, authBuff);
	opendirectory_free_node(session, authType);
	return status;
}

/****************************************************************************
core of smb password checking routine.
****************************************************************************/
static tDirStatus opendirectory_smb_pwd_check_ntlmv1(
		    struct opendirectory_session *session,
		    tDirNodeReference userNode,
		    const char *user,
		    const char *inAuthMethod,
		    const DATA_BLOB *nt_response,
		    const DATA_BLOB *sec_blob,
		    DATA_BLOB *user_sess_key)
{
	tDirStatus	status	    = eDSAuthFailed;
	tDirStatus	keyStatus   = eDSAuthFailed;
	u_int32_t key_length = 0;


	if (sec_blob->length != 8) {
		DEBUG(module_debug, ("incorrect challenge size (%ld)\n",
			    sec_blob->length));
		return eDSAuthFailed;
	}

	if (nt_response->length != 24) {
		DEBUG(module_debug, ("incorrect password length (%ld)\n",
			    nt_response->length));
		return eDSAuthFailed;
	}

 	if ((user_sess_key != NULL) &&
	    (strcmp(inAuthMethod,kDSStdAuthSMB_NT_Key) == 0) ) {
		*user_sess_key = data_blob(NULL, 16);
		become_root();
		status = opendirectory_user_auth_and_session_key(session,
				    userNode, user, sec_blob->data,
				    nt_response->data, user_sess_key->data,
				    &key_length);
		unbecome_root();

		LOG_DS_ERROR(ds_trace, status,
			"opendirectory_user_auth_and_session_key");
	}

	if (eDSAuthMethodNotSupported == status ||
	    eNotHandledByThisNode == status ||
	    (strcmp(inAuthMethod,kDSStdAuthSMB_LM_Key) == 0)) {

   		status = opendirectory_auth_user(session, userNode, user,
			sec_blob->data, nt_response->data, inAuthMethod);
		LOG_DS_ERROR(ds_trace, status,
			"opendirectory_auth_user");

		if (user_sess_key != NULL) {
			*user_sess_key = data_blob(NULL, 16);
			become_root();
			keyStatus = opendirectory_user_session_key(session,
				userNode, user, user_sess_key->data);
			unbecome_root();
			LOG_DS_ERROR(ds_trace, status,
				"opendirectory_user_session_key");
		}
	}

	return status;
}

/****************************************************************************
 Core of smb password checking routine. (NTLMv2, LMv2)
 Note:  The same code works with both NTLMv2 and LMv2.
****************************************************************************/

static tDirStatus opendirectory_smb_pwd_check_ntlmv2(
			struct opendirectory_session *session,
			tDirNodeReference userNode,
			const DATA_BLOB *ntv2_response,
			const DATA_BLOB *sec_blob,
			const char *user, const char *domain,
			/* should the domain be transformed into upper case? */
			BOOL upper_case_domain,
			DATA_BLOB *user_sess_key)
{
	tDirStatus	status	= eDSAuthFailed;
	tDirStatus	keyStatus = eDSNoErr;

	u_int32_t session_key_len = 0;

	if (sec_blob->length != 8) {
		DEBUG(module_debug, ("incorrect challenge size (%lu)\n",
			  (unsigned long)sec_blob->length));
		return False;
	}

	if (ntv2_response->length < 24) {
		/* We MUST have more than 16 bytes, or the stuff below will go
		   crazy.  No known implementation sends less than the 24 bytes
		   for LMv2, let alone NTLMv2. */
		DEBUG(module_debug, ("incorrect password length (%lu)\n",
			  (unsigned long)ntv2_response->length));
		return False;
	}

	status = opendirectory_ntlmv2_auth_user(session, userNode, user,
		    domain, sec_blob, ntv2_response, user_sess_key );
	LOG_DS_ERROR(ds_trace, status, "opendirectory_ntlmv2_auth_user");

	if (eDSNoErr != status) {
		return status;
	}

	if (user_sess_key != NULL) {
		*user_sess_key = data_blob(NULL, 16);
		become_root();
		keyStatus = opendirectory_ntlmv2user_session_key(user,
			ntv2_response->length, ntv2_response->data,
			domain, &session_key_len, user_sess_key->data);
		unbecome_root();

		LOG_DS_ERROR_MSG(ds_trace, status,
			"opendirectory_ntlmv2user_session_key",
			("%u byte session key (%s)\n",
			 session_key_len,
			 ntv2_response->length == 24 ? "LMv2" : "NTLMv2"));

	}

	 return (status);

}


/**
 * Check a challenge-response password against the value of the NT or
 * LM password hash.
 *
 * @param mem_ctx talloc context
 * @param challenge 8-byte challenge.  If all zero, forces plaintext comparison
 * @param nt_response 'unicode' NT response to the challenge, or unicode password
 * @param lm_response ASCII or LANMAN response to the challenge, or password in DOS code page
 * @param username internal Samba username, for log messages
 * @param client_username username the client used
 * @param client_domain domain name the client used (may be mapped)
 * @param nt_pw MD4 unicode password from our passdb or similar
 * @param lm_pw LANMAN ASCII password from our passdb or similar
 * @param user_sess_key User session key
 * @param lm_sess_key LM session key (first 8 bytes of the LM hash)
 */

static NTSTATUS
opendirectory_opendirectory_ntlm_password_check(
			    struct opendirectory_session *session,
			    tDirNodeReference userNode,
			    TALLOC_CTX *mem_ctx,
			    const DATA_BLOB *challenge,
			    const DATA_BLOB *lm_response,
			    const DATA_BLOB *nt_response,
			    const DATA_BLOB *lm_interactive_pwd,
			    const DATA_BLOB *nt_interactive_pwd,
			    const char *username,
			    const char *client_username,
			    const char *client_domain,
			    DATA_BLOB *user_sess_key,
			    DATA_BLOB *lm_sess_key)
{
	tDirStatus dirStatus = eDSAuthFailed;

	if (nt_response->length > 24) {
		/* We have the NT MD4 hash challenge available - see if we can
		   use it
		*/
		DEBUG(module_debug,("Checking NTLMv2 password with domain [%s]\n",
			    client_domain));
		dirStatus = opendirectory_smb_pwd_check_ntlmv2(session,
					  userNode, nt_response,
					  challenge,
					  client_username,
					  client_domain,
					  False,
					  user_sess_key);
		LOG_DS_ERROR(ds_trace, dirStatus,
			"opendirectory_smb_pwd_check_ntlmv2");

		if (dirStatus == eDSNoErr) {

#if DEBUG_LMv2
			DEBUG(module_debug,("Checking LMv2 password with domain [%s]\n",
				    client_domain));
			dirStatus = opendirectory_smb_pwd_check_ntlmv2(session,
						  userNode, lm_response,
						  challenge,
						  client_username,
						  client_domain,
						  False,
						  user_sess_key);
			LOG_DS_ERROR(ds_trace, dirStatus,
				"opendirectory_smb_pwd_check_ntlmv2");
#endif
			return NT_STATUS_OK;
		}

		DEBUG(module_debug,("Checking NTLMv2 password with uppercase domain [%s]\n",
			    client_domain));

		dirStatus = opendirectory_smb_pwd_check_ntlmv2(session,
					  userNode, nt_response,
					  challenge,
					  client_username,
					  client_domain,
					  True,
					  user_sess_key);
		LOG_DS_ERROR(ds_trace, dirStatus,
			"opendirectory_smb_pwd_check_ntlmv2");

		if (dirStatus == eDSNoErr) {
			return NT_STATUS_OK;
		}

		DEBUG(module_debug,("Checking NTLMv2 password without a domain\n"));
		dirStatus = opendirectory_smb_pwd_check_ntlmv2(session,
					  userNode, nt_response,
					  challenge,
					  client_username,
					  "",
					  False,
					  user_sess_key);
		LOG_DS_ERROR(ds_trace, dirStatus,
			"opendirectory_smb_pwd_check_ntlmv2");

		if (dirStatus == eDSNoErr) {

			return NT_STATUS_OK;
		}

		return map_dserr_to_nterr(dirStatus);
	}

	if (nt_response->length == 24) {

		/* Why are we checking nt_interactive_pwd but not actually
		 * using it? -- jpeach
		 */
		if (lp_ntlm_auth() ||
		    (nt_interactive_pwd && nt_interactive_pwd->length)) {
			/* We have the NT MD4 hash challenge available - see
			 * if we can use it (ie. does it exist in the smbpasswd
			 * file).
			 */
			DEBUG(module_debug,("Checking NT MD4 password\n"));
			dirStatus =
			    opendirectory_smb_pwd_check_ntlmv1(session,
						userNode, username,
						kDSStdAuthSMB_NT_Key,
						nt_response,
						challenge,
						user_sess_key);
			LOG_DS_ERROR(ds_trace, dirStatus,
				"opendirectory_smb_pwd_check_ntlmv1");

			if (dirStatus == eDSNoErr) {
				return NT_STATUS_OK;
			}
			DEBUG(module_debug,("NT MD4 password check failed for user %s\n",
				 username));
			return map_dserr_to_nterr(dirStatus);
		}

		/* No return, because we might pick up LMv2 in the LM field. */
		DEBUG(module_debug,("NTLMv1 passwords NOT PERMITTED for user %s\n",
				 username));

	}


	if (lm_response->length == 0) {
		DEBUG(module_debug,("NEITHER LanMan nor NT password supplied for user %s\n",
			 username));
		return NT_STATUS_ILL_FORMED_PASSWORD;
	}

	if (lm_response->length < 24) {
		DEBUG(module_debug,("invalid LanMan password length (%lu) for user %s\n",
			 (unsigned long)nt_response->length, username));
		return NT_STATUS_ILL_FORMED_PASSWORD;
	}

	if (lp_lanman_auth()) {
		DEBUG(module_debug,("Checking LM password\n"));
		dirStatus =  opendirectory_smb_pwd_check_ntlmv1(session,
					userNode, username,
					kDSStdAuthSMB_LM_Key,
					lm_response,
					 challenge,
					 NULL);
		LOG_DS_ERROR(ds_trace, dirStatus,
			"opendirectory_smb_pwd_check_ntlmv1");

		if (dirStatus == eDSNoErr) {
			return NT_STATUS_OK;
		}

		DEBUG(module_debug,("LM password check failed for user %s\n",username));
	}

	/* This is for 'LMv2' authentication.  almost NTLMv2 but limited to
	 * 24 bytes - related to Win9X, legacy NAS pass-though authentication
	 */
	DEBUG(module_debug,("Checking LMv2 password with domain %s\n", client_domain));
	dirStatus = opendirectory_smb_pwd_check_ntlmv2(session, userNode,
				  lm_response,
				  challenge,
				  client_username,
				  client_domain,
				  False,
				  user_sess_key);
	LOG_DS_ERROR(ds_trace, dirStatus,
		"opendirectory_smb_pwd_check_ntlmv2");

	if (dirStatus== eDSNoErr) {
		return NT_STATUS_OK;
	}

	DEBUG(module_debug,("Checking LMv2 password with upper-cased domain %s\n",
		    client_domain));
	dirStatus = opendirectory_smb_pwd_check_ntlmv2(session, userNode,
				  lm_response,
				  challenge,
				  client_username,
				  client_domain,
				  True,
				  user_sess_key);
	LOG_DS_ERROR(ds_trace, dirStatus,
		"opendirectory_smb_pwd_check_ntlmv2");

	if (dirStatus== eDSNoErr) {
		return NT_STATUS_OK;
	}

	DEBUG(module_debug,("Checking LMv2 password without a domain\n"));
	dirStatus = opendirectory_smb_pwd_check_ntlmv2(session, userNode,
			          lm_response,
				  challenge,
				  client_username,
				  "",
				  False,
				  user_sess_key);
	LOG_DS_ERROR(ds_trace, dirStatus,
		"opendirectory_smb_pwd_check_ntlmv2");

	if (dirStatus== eDSNoErr) {
		return NT_STATUS_OK;
	}

	/* Apparently NT accepts NT responses in the LM field
	   - I think this is related to Win9X pass-though authentication
	*/
	DEBUG(module_debug,("Checking NT MD4 password in LM field\n"));
	if (lp_ntlm_auth()) {
		dirStatus =  opendirectory_smb_pwd_check_ntlmv1(session,
					userNode, username,
					kDSStdAuthSMB_NT_Key,
					lm_response,
					 challenge,
					 user_sess_key);
		LOG_DS_ERROR(ds_trace, dirStatus,
			"opendirectory_smb_pwd_check_ntlmv1");

		if (dirStatus== eDSNoErr) {
			return NT_STATUS_OK;
		}
		DEBUG(module_debug,("LM password, NT MD4 password in LM field "
			    "and LMv2 failed for user %s\n",username));
	}

	DEBUG(module_debug,("LM password and LMv2 failed for user %s, "
		    "and NT MD4 password in LM field not permitted\n",username));
	return NT_STATUS_WRONG_PASSWORD;
}


/****************************************************************************
 Do a specific test for an smb password being correct, given a smb_password and
 the lanman and NT responses.
****************************************************************************/
static NTSTATUS opendirectory_password_ok(
			    struct opendirectory_session *session,
			    tDirNodeReference userNode,
			    const struct auth_context *auth_context,
			    TALLOC_CTX *mem_ctx,
			    struct samu *sampass,
			    const auth_usersupplied_info *user_info,
			    DATA_BLOB *user_sess_key,
			    DATA_BLOB *lm_sess_key)
{
	uint16 acct_ctrl;
	const char *username = pdb_get_username(sampass);

	acct_ctrl = pdb_get_acct_ctrl(sampass);
	if (acct_ctrl & ACB_PWNOTREQ) {
		DEBUG(module_debug,("Account for user '%s' has no password "
			    "and null passwords %s allowed.\n",
			    lp_null_passwords() ? "are" : "are NOT",
			    username));
		return lp_null_passwords() ? NT_STATUS_OK
					   : NT_STATUS_LOGON_FAILURE;
	}

	return opendirectory_opendirectory_ntlm_password_check(session,
			    userNode, mem_ctx, &auth_context->challenge,
			    &user_info->lm_resp, &user_info->nt_resp,
			    &user_info->lm_interactive_pwd,
			    &user_info->nt_interactive_pwd,
			    username,
			    user_info->smb_name,
			    user_info->client_domain,
			    user_sess_key, lm_sess_key);
}

/****************************************************************************
 Do a specific test for a struct samu being vaild for this connection
 (ie not disabled, expired and the like).
****************************************************************************/
static NTSTATUS opendirectory_account_ok(TALLOC_CTX *mem_ctx,
			       struct samu *sampass,
			       const auth_usersupplied_info *user_info)
{
	uint16	acct_ctrl = pdb_get_acct_ctrl(sampass);
	char *workstation_list;
	time_t kickoff_time;

	DEBUG(module_debug,("Checking SMB password for user %s\n",
		    pdb_get_username(sampass)));

	/* Quit if the account was disabled. */
	if (acct_ctrl & ACB_DISABLED) {
		DEBUG(module_debug,("Account for user '%s' was disabled.\n",
			    pdb_get_username(sampass)));
		return NT_STATUS_ACCOUNT_DISABLED;
	}

	/* Test account expire time */

	kickoff_time = pdb_get_kickoff_time(sampass);
	if (kickoff_time != 0 && time(NULL) > kickoff_time) {
		DEBUG(module_debug,("Account for user '%s' has expired.\n",
			    pdb_get_username(sampass)));
		DEBUG(module_debug,("Account expired at '%ld' unix time.\n",
			    (long)kickoff_time));
		return NT_STATUS_ACCOUNT_EXPIRED;
	}

	if (!(pdb_get_acct_ctrl(sampass) & ACB_PWNOEXP)) {
		time_t must_change_time = pdb_get_pass_must_change_time(sampass);
		time_t last_set_time = pdb_get_pass_last_set_time(sampass);

		/* check for immediate expiry "must change at next logon" */
		if (must_change_time == 0 && last_set_time != 0) {
			DEBUG(module_debug,("Password for user '%s' must change!\n",
				    pdb_get_username(sampass)));
			return NT_STATUS_PASSWORD_MUST_CHANGE;
		}

		/* check for expired password */
		if (must_change_time < time(NULL) && must_change_time != 0) {
			DEBUG(module_debug,("Password for user '%s' has expired!\n",
				    pdb_get_username(sampass)));
			DEBUG(module_debug,("Password expired at '%s' (%ld) unix time.\n",
				    http_timestring(must_change_time),
				    (long)must_change_time));
			return NT_STATUS_PASSWORD_EXPIRED;
		}
	}

	/* Test workstation. Workstation list is comma separated. */

	workstation_list = talloc_strdup(mem_ctx, pdb_get_workstations(sampass));

	if (!workstation_list) return NT_STATUS_NO_MEMORY;

	if (*workstation_list) {
		BOOL invalid_ws = True;
		const char *s = workstation_list;

		fstring tok;

		while (next_token(&s, tok, ",", sizeof(tok))) {
			DEBUG(module_debug,("checking for workstation match %s and %s\n",
				  tok, user_info->wksta_name));
			if(strequal(tok, user_info->wksta_name)) {
				invalid_ws = False;
				break;
			}
		}

		if (invalid_ws)
			return NT_STATUS_INVALID_WORKSTATION;
	}

	if (acct_ctrl & ACB_DOMTRUST) {
		DEBUG(module_debug,("Domain trust account %s denied by server\n",
			    pdb_get_username(sampass)));
		return NT_STATUS_NOLOGON_INTERDOMAIN_TRUST_ACCOUNT;
	}

	if (acct_ctrl & ACB_SVRTRUST) {
		DEBUG(module_debug,("Server trust account %s denied by server\n",
			    pdb_get_username(sampass)));
		return NT_STATUS_NOLOGON_SERVER_TRUST_ACCOUNT;
	}

	if (acct_ctrl & ACB_WSTRUST) {
		DEBUG(module_debug,("Wksta trust account %s denied by server\n",
			    pdb_get_username(sampass)));
		return NT_STATUS_NOLOGON_WORKSTATION_TRUST_ACCOUNT;
	}

	return NT_STATUS_OK;
}


/****************************************************************************
check if a username/password is OK assuming the password is a 24 byte
SMB hash supplied in the user_info structure
return an NT_STATUS constant.
****************************************************************************/

static NTSTATUS check_opendirectory_security(
			const struct auth_context *auth_context,
		        void *my_private_data,
		        TALLOC_CTX *mem_ctx,
		        const auth_usersupplied_info *user_info,
		        auth_serversupplied_info **server_info)
{
	struct samu *sampass=NULL;
	BOOL ret = False;
	NTSTATUS nt_status = NT_STATUS_OK;
	DATA_BLOB user_sess_key = data_blob(NULL, 0);
	DATA_BLOB lm_sess_key = data_blob(NULL, 0);
	tDirStatus		dirStatus	= eDSNoErr;
	tDirNodeReference	userNodeRef	= 0;
	struct opendirectory_session session;

	if (!user_info || !auth_context) {
		return NT_STATUS_UNSUCCESSFUL;
	}

	sampass = samu_new(NULL);
	if (!sampass) {
		return NT_STATUS_NO_MEMORY;
	}

	/* get the account information */

	if (user_info->internal_username &&
	    strlen(user_info->internal_username)) {
		become_root();
		ret = pdb_getsampwnam(sampass, user_info->internal_username);
		unbecome_root();
	}

	if (ret == False) {
		DEBUG(module_debug, ("user '%s' is not in the password database\n",
			    user_info->internal_username));
		TALLOC_FREE(sampass);
		return NT_STATUS_NO_SUCH_USER;
	}

	/* FIXME: this would be a good place to check the SMB service ACL.
	 * Unfortunately, we don't ever get here if the client authenticated
	 * with Kerberos.
	 */

	nt_status = opendirectory_account_ok(mem_ctx, sampass, user_info);

	if (!NT_STATUS_IS_OK(nt_status)) {
		TALLOC_FREE(sampass);
		return nt_status;
	}

	dirStatus = opendirectory_connect(&session);
	LOG_DS_ERROR(ds_trace, dirStatus, "dsOpenDirService");
	if (dirStatus != eDSNoErr) {
		TALLOC_FREE(sampass);
		return NT_STATUS_UNSUCCESSFUL;
	}

	userNodeRef = getusernode(&session, pdb_get_username(sampass));
	if (userNodeRef == 0) {
		opendirectory_disconnect(&session);
		TALLOC_FREE(sampass);
		return NT_STATUS_NO_SUCH_USER;
	}

	nt_status = opendirectory_password_ok(&session, userNodeRef,
			auth_context, mem_ctx, sampass, user_info,
			&user_sess_key, &lm_sess_key);
	DS_CLOSE_NODE( userNodeRef );
	opendirectory_disconnect(&session);

	if (!NT_STATUS_IS_OK(nt_status)) {
		TALLOC_FREE(sampass);
		return nt_status;
	}

	nt_status = make_server_info_sam(server_info, sampass);
	if (!NT_STATUS_IS_OK(nt_status)) {
		TALLOC_FREE(sampass);
		DEBUG(module_debug, ("make_server_info_sam() failed with '%s'\n",
			    nt_errstr(nt_status)));
		return nt_status;
	}

	/* NOTE: There is no need to deal specially with
	 * (*server_info)->sam_account since server_info_dtor() cleans it
	 * up on a talloc_free().
	 */

	(*server_info)->user_session_key = user_sess_key;
	(*server_info)->lm_session_key = lm_sess_key;

	return nt_status;
}

/* module initialisation */
static NTSTATUS auth_init_opendirectory(struct auth_context *auth_context,
	const char *param, auth_methods **auth_method)
{
	if (!make_auth_methods(auth_context, auth_method)) {
		return NT_STATUS_NO_MEMORY;
	}

	(*auth_method)->auth = check_opendirectory_security;
	(*auth_method)->name = MODULE_NAME;
	return NT_STATUS_OK;
}

/* module initialisation */
static NTSTATUS auth_init_od_historic(struct auth_context *auth_context,
	const char *param, auth_methods **auth_method)
{
	if (!make_auth_methods(auth_context, auth_method)) {
		return NT_STATUS_NO_MEMORY;
	}

	(*auth_method)->auth = check_opendirectory_security;
	(*auth_method)->name = "opendirectory";
	return NT_STATUS_OK;
}

NTSTATUS auth_ods_init(void)
{
	/* Use "odsam:traceall = yes" to turn on OD query tracing. */
	if (lp_parm_bool(GLOBAL_SECTION_SNUM,
		    MODULE_NAME, "traceall", False)) {
		ds_trace = DS_TRACE_ALL;
	}

	module_debug = lp_parm_int(GLOBAL_SECTION_SNUM,
					MODULE_NAME, "msglevel", 100);

	/* Register with the current module name. */
	smb_register_auth(AUTH_INTERFACE_VERSION, MODULE_NAME,
			auth_init_opendirectory);

	/* Register the historic name for compatibility with 10.4 configs. */
	smb_register_auth(AUTH_INTERFACE_VERSION, "opendirectory",
			auth_init_od_historic);

	return NT_STATUS_OK;
}
