/*
 * Copyright (c) 2000-2001 Apple Computer, Inc. All Rights Reserved.
 * 
 * The contents of this file constitute Original Code as defined in and are
 * subject to the Apple Public Source License Version 1.2 (the 'License').
 * You may not use this file except in compliance with the License. Please obtain
 * a copy of the License at http://www.apple.com/publicsource and read it before
 * using this file.
 * 
 * This Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER EXPRESS
 * OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES, INCLUDING WITHOUT
 * LIMITATION, ANY WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 * PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT. Please see the License for the
 * specific language governing rights and limitations under the License.
 */


/*
	File:		appleCdsa.cpp

	Contains:	interface between SSL and CDSA

	Written by:	Doug Mitchell

	Copyright: (c) 1999 by Apple Computer, Inc., all rights reserved.

*/

#include "ssl.h"
#include "sslContext.h"
#include "sslMemory.h"
#include "appleCdsa.h"
#include "sslUtils.h"
#include "sslDebug.h"
#include "sslBER.h"
#include "ModuleAttacher.h"

#ifndef	_SSL_KEYCHAIN_H_
#include "sslKeychain.h"
#endif

#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include <Security/cssm.h>
#include <Security/cssmapple.h>
#include <Security/Security.h>
#include <Security/SecTrustPriv.h>
#include <Security/SecPolicyPriv.h>
#include <Security/SecKeyPriv.h>

/* X.509 includes, from cssmapi */
#include <Security/x509defs.h>         /* x.509 function and type defs */
#include <Security/oidsalg.h>
#include <Security/oidscert.h>

#pragma mark *** Utilities ***

/*
 * Set up a Raw symmetric key with specified algorithm and key bits.
 */
OSStatus sslSetUpSymmKey(
	CSSM_KEY_PTR	symKey,
	CSSM_ALGORITHMS	alg,
	CSSM_KEYUSE		keyUse, 		// CSSM_KEYUSE_ENCRYPT, etc.
	CSSM_BOOL		copyKey,		// true: copy keyData   false: set by reference
	uint8 			*keyData,
	uint32			keyDataLen)		// in bytes
{
	OSStatus serr;
	CSSM_KEYHEADER *hdr;
	
	memset(symKey, 0, sizeof(CSSM_KEY));
	if(copyKey) {
		serr = stSetUpCssmData(&symKey->KeyData, keyDataLen);
		if(serr) {
			return serr;
		}
		memmove(symKey->KeyData.Data, keyData, keyDataLen);
	}
	else {
		symKey->KeyData.Data = keyData;
		symKey->KeyData.Length = keyDataLen;
	}
	
	/* set up the header */
	hdr = &symKey->KeyHeader;
	hdr->BlobType = CSSM_KEYBLOB_RAW;
	hdr->Format = CSSM_KEYBLOB_RAW_FORMAT_OCTET_STRING;
	hdr->AlgorithmId = alg;
	hdr->KeyClass = CSSM_KEYCLASS_SESSION_KEY;
	hdr->LogicalKeySizeInBits = keyDataLen * 8;
	hdr->KeyAttr = CSSM_KEYATTR_MODIFIABLE | CSSM_KEYATTR_EXTRACTABLE;
	hdr->KeyUsage = keyUse;
	hdr->WrapAlgorithmId = CSSM_ALGID_NONE;
	return noErr;
}

/*
 * Free a CSSM_KEY - its CSP resources, KCItemRef, and the key itself.
 */
OSStatus sslFreeKey(
	CSSM_CSP_HANDLE		cspHand,
	CSSM_KEY_PTR		*key,		/* so we can null it out */
	#if		ST_KC_KEYS_NEED_REF
	SecKeychainRef	*kcItem)
	#else	
	void			*kcItem) 
	#endif
{
	assert(key != NULL);
	
	if(*key != NULL) {
		if(cspHand != 0) {
			CSSM_FreeKey(cspHand, NULL, *key, CSSM_FALSE);
		}
		stAppFree(*key, NULL);		// key mallocd by CL using our callback
		*key = NULL;
	}
	#if		ST_KC_KEYS_NEED_REF
	if((kcItem != NULL) && (*kcItem != NULL)) {
		KCReleaseItem(kcItem);		/* does this NULL the referent? */
		*kcItem = NULL;
	}
	#endif
	return noErr;
}

/*
 * Standard app-level memory functions required by CDSA.
 */
void * stAppMalloc (uint32 size, void *allocRef) {
	return( malloc(size) );
}
void stAppFree (void *mem_ptr, void *allocRef) {
	free(mem_ptr);
 	return;
}
void * stAppRealloc (void *ptr, uint32 size, void *allocRef) {
	return( realloc( ptr, size ) );
}
void * stAppCalloc (uint32 num, uint32 size, void *allocRef) {
	return( calloc( num, size ) );
}

/*
 * Ensure there's a connection to ctx->cspHand. If there 
 * already is one, fine.
 * Note that as of 12/18/00, we assume we're connected to 
 * all modules all the time (since we do an attachToAll() in 
 * SSLNewContext()).
 */
OSStatus attachToCsp(SSLContext *ctx)
{
	assert(ctx != NULL);
	if(ctx->cspHand != 0) {
		return noErr;
	}	
	else {
		return errSSLModuleAttach;
	}
}

/* 
 * Connect to TP, CL; reusable.
 */
OSStatus attachToCl(SSLContext *ctx)
{
	assert(ctx != NULL);
	if(ctx->clHand != 0) {
		return noErr;
	}
	else {
		return errSSLModuleAttach;
	}
}

OSStatus attachToTp(SSLContext *ctx)
{
	assert(ctx != NULL);
	if(ctx->tpHand != 0) {
		return noErr;
	}
	else {
		return errSSLModuleAttach;
	}
}

/*
 * Convenience function - attach to CSP, CL, TP. Reusable. 
 */
OSStatus attachToAll(SSLContext *ctx)
{
	CSSM_RETURN crtn;
	
	assert(ctx != NULL);
	crtn = attachToModules(&ctx->cspHand, &ctx->clHand, &ctx->tpHand);
	if(crtn) {
	   return errSSLModuleAttach;
	}
	else {
		return noErr;
	}
}

OSStatus detachFromAll(SSLContext *ctx)
{
	#if	0
	/* No more, attachments are kept on a global basis */
	assert(ctx != NULL);
	if(ctx->cspHand != 0) {
		CSSM_ModuleDetach(ctx->cspHand);
		ctx->cspHand = 0;
	}
	if(ctx->tpHand != 0) {
		CSSM_ModuleDetach(ctx->tpHand);
		ctx->tpHand = 0;
	}
	if(ctx->clHand != 0) {
		CSSM_ModuleDetach(ctx->clHand);
		ctx->clHand = 0;
	}
	#endif	/* 0 */
	return noErr;
}

/*
 * Add a CSSM_ATTRIBUTE_RSA_BLINDING attribute to
 * specified crypto context.
 */
static CSSM_RETURN sslAddBlindingAttr(
	CSSM_CC_HANDLE ccHand)
{
	CSSM_CONTEXT_ATTRIBUTE	newAttr;	
	CSSM_RETURN				crtn;
	
	newAttr.AttributeType     = CSSM_ATTRIBUTE_RSA_BLINDING;
	newAttr.AttributeLength   = sizeof(uint32);
	newAttr.Attribute.Uint32  = 1;
	crtn = CSSM_UpdateContextAttributes(ccHand, 1, &newAttr);
	if(crtn) {
		stPrintCdsaError("CSSM_UpdateContextAttributes", crtn);
	}
	return crtn;
}

/* Get CSP, key in CSSM format from a SecKeyRef */
static OSStatus sslGetKeyParts(
	SecKeyRef			keyRef,
	const CSSM_KEY		**cssmKey,
	CSSM_CSP_HANDLE		*cspHand)
{
	OSStatus ortn = SecKeyGetCSSMKey(keyRef, cssmKey);
	if(ortn) {
		sslErrorLog("sslGetKeyParts: SecKeyGetCSSMKey err %d\n",
			(int)ortn);
		return ortn;
	}
	ortn = SecKeyGetCSPHandle(keyRef, cspHand);
	if(ortn) {
		sslErrorLog("sslGetKeyParts: SecKeyGetCSPHandle err %d\n",
			(int)ortn);
	}
	return ortn;
}

#pragma mark -
#pragma mark *** CSSM_DATA routines ***

CSSM_DATA_PTR stMallocCssmData(
	uint32 size)
{
	CSSM_DATA_PTR rtn = (CSSM_DATA_PTR)stAppMalloc(sizeof(CSSM_DATA), NULL);

	if(rtn == NULL) {
		return NULL;
	}
	rtn->Length = size;
	if(size == 0) {
		rtn->Data = NULL;
	}
	else {
		rtn->Data = (uint8 *)stAppMalloc(size, NULL);
	}
	return rtn;
}

void stFreeCssmData(
	CSSM_DATA_PTR data,
	CSSM_BOOL freeStruct)
{
	if(data == NULL) {
		return;
	}
	if(data->Data != NULL) {
		stAppFree(data->Data, NULL);
		data->Data   = NULL;
	}
	data->Length = 0;
	if(freeStruct) {
		stAppFree(data, NULL);
	}
}

/*
 * Ensure that indicated CSSM_DATA_PTR can handle 'length' bytes of data.
 * Malloc the Data ptr if necessary.
 */
OSStatus stSetUpCssmData(
	CSSM_DATA_PTR 	data,
	uint32 			length)
{
	assert(data != NULL);
	if(data->Length == 0) {
		data->Data = (uint8 *)stAppMalloc(length, NULL);
		if(data->Data == NULL) {
			return memFullErr;
		}
	}
	else if(data->Length < length) {
		sslErrorLog("stSetUpCssmData: length too small\n");
		return memFullErr;
	}
	data->Length = length;
	return noErr;
}

static OSStatus sslKeyToSigAlg(
	const CSSM_KEY *cssmKey,
	CSSM_ALGORITHMS &sigAlg)	/* RETURNED */
	
{
	OSStatus ortn = noErr;
	switch(cssmKey->KeyHeader.AlgorithmId) {
		case CSSM_ALGID_RSA:
			sigAlg = CSSM_ALGID_RSA;
			break;
		case CSSM_ALGID_DSA:
			sigAlg = CSSM_ALGID_DSA;
			break;
		default:
			ortn = errSSLBadConfiguration;
			break;
	}
	return ortn;
}

#pragma mark -
#pragma mark *** Public CSP Functions ***

/*
 * Raw RSA/DSA sign/verify.
 */
OSStatus sslRawSign(
	SSLContext			*ctx,
	SecKeyRef			privKeyRef,		
	const UInt8			*plainText,
	UInt32				plainTextLen,
	UInt8				*sig,			// mallocd by caller; RETURNED
	UInt32				sigLen,			// available
	UInt32				*actualBytes)	// RETURNED
{
	CSSM_CC_HANDLE			sigHand = 0;
	CSSM_RETURN				crtn;
	OSStatus				serr;
	CSSM_DATA				sigData;
	CSSM_DATA				ptextData;
	CSSM_CSP_HANDLE			cspHand;
	const CSSM_KEY 			*privKey;
	const CSSM_ACCESS_CREDENTIALS	*creds;
	
	assert(ctx != NULL);
	if((privKeyRef == NULL)	|| 
	   (plainText == NULL)	|| 
	   (sig == NULL)		||
	   (actualBytes == NULL)) {
		sslErrorLog("sslRsaRawSign: bad arguments\n");
		return errSSLInternal;
	}
	*actualBytes = 0;
	
	/* Get CSP, signing key in CSSM format */
	serr = sslGetKeyParts(privKeyRef, &privKey, &cspHand);
	if(serr) {
		return serr;
	}
	assert(privKey->KeyHeader.KeyClass == CSSM_KEYCLASS_PRIVATE_KEY);

	CSSM_ALGORITHMS sigAlg;
	serr = sslKeyToSigAlg(privKey, sigAlg);
	if(serr) {
		return serr;
	}
	
	/* 
	 * Get default creds
	 * FIXME: per 3420180, this needs to allow app-specified creds via
	 * an new API
	 */
	serr = SecKeyGetCredentials(privKeyRef,
		CSSM_ACL_AUTHORIZATION_SIGN,
		kSecCredentialTypeDefault,
		&creds);
	if(serr) {
		sslErrorLog("sslRawSign: SecKeyGetCredentials err %lu\n", serr);
		return serr;
	}
	
	crtn = CSSM_CSP_CreateSignatureContext(cspHand,
		sigAlg,
		creds,	
		privKey,
		&sigHand);
	if(crtn) {
		stPrintCdsaError("CSSM_CSP_CreateSignatureContext (1)", crtn);
		return errSSLCrypto;
	}

	if((ctx->rsaBlindingEnable) &&
	   (privKey->KeyHeader.AlgorithmId == CSSM_ALGID_RSA)) {
		/* 
		 * Turn on RSA blinding to defeat timing attacks 
		 */
		crtn = sslAddBlindingAttr(sigHand);
		if(crtn) {
			return crtn;
		}
	}
	
	ptextData.Data = (uint8 *)plainText;
	ptextData.Length = plainTextLen;
	
	/* caller better get this right, or the SignData will fail */
	sigData.Data = sig;
	sigData.Length = sigLen;
	
	crtn = CSSM_SignData(sigHand,
		&ptextData,
		1,
		CSSM_ALGID_NONE,	// digestAlg for raw sign
		&sigData);
	if(crtn) {
		stPrintCdsaError("CSSM_SignData", crtn);
		serr = errSSLCrypto;
	}
	else {
		*actualBytes = sigData.Length;
		serr = noErr;
	}
	if(sigHand != 0) {
		CSSM_DeleteContext(sigHand);
	}
	return serr;
}

OSStatus sslRawVerify(
	SSLContext			*ctx,
	const CSSM_KEY		*pubKey,
	CSSM_CSP_HANDLE		cspHand,
	const UInt8			*plainText,
	UInt32				plainTextLen,
	const UInt8			*sig,
	UInt32				sigLen)	
{
	CSSM_CC_HANDLE			sigHand = 0;
	CSSM_RETURN				crtn;
	OSStatus				serr;
	CSSM_DATA				sigData;
	CSSM_DATA				ptextData;
	
	assert(ctx != NULL);
	if((pubKey == NULL) 	|| 
	   (cspHand == 0) 		|| 
	   (plainText == NULL)	|| 
	   (sig == NULL)) {
		sslErrorLog("sslRawVerify: bad arguments\n");
		return errSSLInternal;
	}
	
	CSSM_ALGORITHMS sigAlg;
	serr = sslKeyToSigAlg(pubKey, sigAlg);
	if(serr) {
		return serr;
	}
	crtn = CSSM_CSP_CreateSignatureContext(cspHand,
		sigAlg,
		NULL,				// passPhrase
		pubKey,
		&sigHand);
	if(sigHand == 0) {
		stPrintCdsaError("CSSM_CSP_CreateSignatureContext (2)", crtn);
		return errSSLCrypto;
	}
	
	ptextData.Data = (uint8 *)plainText;
	ptextData.Length = plainTextLen;
	sigData.Data = (uint8 *)sig;
	sigData.Length = sigLen;
	
	crtn = CSSM_VerifyData(sigHand,
		&ptextData,
		1,
		CSSM_ALGID_NONE,		// digestAlg
		&sigData);
	if(crtn) {
		stPrintCdsaError("CSSM_VerifyData", crtn);
		serr = errSSLCrypto;
	}
	else {
		serr = noErr;
	}
	if(sigHand != 0) {
		CSSM_DeleteContext(sigHand);
	}
	return serr;
}

/*
 * Encrypt/Decrypt
 */
OSStatus sslRsaEncrypt(
	SSLContext			*ctx,
	const CSSM_KEY		*pubKey,
	CSSM_CSP_HANDLE		cspHand,
	const UInt8			*plainText,
	UInt32				plainTextLen,
	UInt8				*cipherText,		// mallocd by caller; RETURNED 
	UInt32				cipherTextLen,		// available
	UInt32				*actualBytes)		// RETURNED
{
	CSSM_DATA 		ctextData = {0, NULL};
	CSSM_DATA 		ptextData;
	CSSM_DATA		remData = {0, NULL};
	CSSM_CC_HANDLE 	cryptHand = 0;
	OSStatus		serr = errSSLInternal;
	CSSM_RETURN		crtn;
	uint32			bytesMoved = 0;
	CSSM_ACCESS_CREDENTIALS	creds;
	
	assert(ctx != NULL);
	assert(actualBytes != NULL);
	*actualBytes = 0;
	
	if((pubKey == NULL) || (cspHand == 0)) {
		sslErrorLog("sslRsaEncrypt: bad pubKey/cspHand\n");
		return errSSLInternal;
	}
	assert(pubKey->KeyHeader.KeyClass == CSSM_KEYCLASS_PUBLIC_KEY);
	
	#if		RSA_PUB_KEY_USAGE_HACK
	((CSSM_KEY_PTR)pubKey)->KeyHeader.KeyUsage |= CSSM_KEYUSE_ENCRYPT;
	#endif
	memset(&creds, 0, sizeof(CSSM_ACCESS_CREDENTIALS));
	
	crtn = CSSM_CSP_CreateAsymmetricContext(cspHand,
		CSSM_ALGID_RSA,
		&creds,
		pubKey,
		CSSM_PADDING_PKCS1,
		&cryptHand);
	if(crtn) {
		stPrintCdsaError("CSSM_CSP_CreateAsymmetricContext", crtn);
		return errSSLCrypto;
	}
	ptextData.Data = (uint8 *)plainText;
	ptextData.Length = plainTextLen;
	
	/* 
	 * Have CSP malloc ciphertext 
	 */
	crtn = CSSM_EncryptData(cryptHand,
		&ptextData,
		1,
		&ctextData,
		1,
		&bytesMoved,
		&remData);
	if(crtn == CSSM_OK) {
		/* 
		 * ciphertext in both ctextData and remData; ensure it'll fit
		 * in caller's buf & copy 
		 */
		if(bytesMoved > cipherTextLen) {
			sslErrorLog("sslRsaEncrypt overflow; cipherTextLen %ld bytesMoved %ld\n",
				cipherTextLen, bytesMoved);
			serr = errSSLCrypto;
		}
		else {
			UInt32 toMoveCtext;
			UInt32 toMoveRem;
			
			*actualBytes = bytesMoved;
			/* 
			 * Snag valid data from ctextData - its length or bytesMoved, 
			 * whichever is less
			 */
			if(ctextData.Length > bytesMoved) {
				/* everything's in ctext */
				toMoveCtext = bytesMoved;
				toMoveRem = 0;
			}
			else {
				/* must be some in remData too */
				toMoveCtext = ctextData.Length;
				toMoveRem = bytesMoved - toMoveCtext;		// remainder 
			}
			if(toMoveCtext) {
				memmove(cipherText, ctextData.Data, toMoveCtext);
			}
			if(toMoveRem) {
				memmove(cipherText + toMoveCtext, remData.Data,
					toMoveRem);
			}
			serr = noErr;
		}
	}
	else {
		stPrintCdsaError("CSSM_EncryptData", crtn);
		serr = errSSLCrypto;
	}
	if(cryptHand != 0) {
		CSSM_DeleteContext(cryptHand);
	}

	/* free data mallocd by CSP */
	stFreeCssmData(&ctextData, CSSM_FALSE);
	stFreeCssmData(&remData, CSSM_FALSE);
	return serr;
}

OSStatus sslRsaDecrypt(
	SSLContext			*ctx,
	SecKeyRef			privKeyRef,
	const UInt8			*cipherText,
	UInt32				cipherTextLen,		
	UInt8				*plainText,			// mallocd by caller; RETURNED
	UInt32				plainTextLen,		// available
	UInt32				*actualBytes)		// RETURNED
{
	CSSM_DATA 		ptextData = {0, NULL};
	CSSM_DATA 		ctextData;
	CSSM_DATA		remData = {0, NULL};
	CSSM_CC_HANDLE 	cryptHand = 0;
	OSStatus		serr = errSSLInternal;
	CSSM_RETURN		crtn;
	uint32			bytesMoved = 0;
	CSSM_CSP_HANDLE			cspHand;
	const CSSM_KEY 			*privKey;
	const CSSM_ACCESS_CREDENTIALS	*creds;
		
	assert(ctx != NULL);
	assert(actualBytes != NULL);
	*actualBytes = 0;
	
	if(privKeyRef == NULL) {
		sslErrorLog("sslRsaDecrypt: bad privKey\n");
		return errSSLInternal;
	}

	/* Get CSP, signing key in CSSM format */
	serr = sslGetKeyParts(privKeyRef, &privKey, &cspHand);
	if(serr) {
		return serr;
	}
	assert(privKey->KeyHeader.KeyClass == CSSM_KEYCLASS_PRIVATE_KEY);

	/* 
	 * Get default creds
	 * FIXME: per 3420180, this needs to allow app-specified creds via
	 * an new API
	 */
	serr = SecKeyGetCredentials(privKeyRef,
		CSSM_ACL_AUTHORIZATION_DECRYPT,
		kSecCredentialTypeDefault,
		&creds);
	if(serr) {
		sslErrorLog("sslRsaDecrypt: SecKeyGetCredentials err %lu\n", serr);
		return serr;
	}
	crtn = CSSM_CSP_CreateAsymmetricContext(cspHand,
		CSSM_ALGID_RSA,
		creds,
		privKey,
		CSSM_PADDING_PKCS1,
		&cryptHand);
	if(crtn) {
		stPrintCdsaError("CSSM_CSP_CreateAsymmetricContext", crtn);
		return errSSLCrypto;
	}
	ctextData.Data = (uint8 *)cipherText;
	ctextData.Length = cipherTextLen;
	
	if((ctx->rsaBlindingEnable) &&
	   (privKey->KeyHeader.AlgorithmId == CSSM_ALGID_RSA)) {
		/* 
		 * Turn on RSA blinding to defeat timing attacks 
		 */
		crtn = sslAddBlindingAttr(cryptHand);
		if(crtn) {
			return crtn;
		}
	}

	/* 
	 * Have CSP malloc plaintext 
	 */
	crtn = CSSM_DecryptData(cryptHand,
		&ctextData,
		1,
		&ptextData,
		1,
		&bytesMoved,
		&remData);
	if(crtn == CSSM_OK) {
		/* 
		 * plaintext in both ptextData and remData; ensure it'll fit
		 * in caller's buf & copy 
		 */
		if(bytesMoved > plainTextLen) {
			sslErrorLog("sslRsaDecrypt overflow; plainTextLen %ld bytesMoved %ld\n",
				plainTextLen, bytesMoved);
			serr = errSSLCrypto;
		}
		else {
			UInt32 toMovePtext;
			UInt32 toMoveRem;
			
			*actualBytes = bytesMoved;
			/* 
			 * Snag valid data from ptextData - its length or bytesMoved, 
			 * whichever is less
			 */
			if(ptextData.Length > bytesMoved) {
				/* everything's in ptext */
				toMovePtext = bytesMoved;
				toMoveRem = 0;
			}
			else {
				/* must be some in remData too */
				toMovePtext = ptextData.Length;
				toMoveRem = bytesMoved - toMovePtext;		// remainder 
			}
			if(toMovePtext) {
				memmove(plainText, ptextData.Data, toMovePtext);
			}
			if(toMoveRem) {
				memmove(plainText + toMovePtext, remData.Data,
					toMoveRem);
			}
			serr = noErr;
		}
	}
	else {
		stPrintCdsaError("CSSM_DecryptData", crtn);
		serr = errSSLCrypto;
	}
	if(cryptHand != 0) {
		CSSM_DeleteContext(cryptHand);
	}
	
	/* free data mallocd by CSP */
	stFreeCssmData(&ptextData, CSSM_FALSE);
	stFreeCssmData(&remData, CSSM_FALSE);
	return serr;
}

/*
 * Obtain size of key in bytes.
 */
UInt32 sslKeyLengthInBytes(const CSSM_KEY *key)
{
	assert(key != NULL);
	return (((key->KeyHeader.LogicalKeySizeInBits) + 7) / 8);
}

/*
 * Obtain maximum size of signature in bytes. A bit of a kludge; we could
 * ask the CSP to do this but that would be kind of expensive.
 */
OSStatus sslGetMaxSigSize(
	const CSSM_KEY	*privKey,
	UInt32			&maxSigSize)
{	
	OSStatus ortn = noErr;
	assert(privKey != NULL);
	assert(privKey->KeyHeader.KeyClass == CSSM_KEYCLASS_PRIVATE_KEY);
	switch(privKey->KeyHeader.AlgorithmId) {
		case CSSM_ALGID_RSA:
			maxSigSize = sslKeyLengthInBytes(privKey);
			break;
		case CSSM_ALGID_DSA:
		{
			/* DSA sig is DER sequence of two 160-bit integers */
			UInt32 sizeOfOneInt;
			sizeOfOneInt = (160 / 8) +	// the raw contents
							1 +			// possible leading zero
							2;			// tag + length (assume DER, not BER)
			maxSigSize = (2 * sizeOfOneInt) + 5;
			break;
		}
		default:
			ortn = errSSLBadConfiguration;
			break;
	}
	return ortn;
}
/*
 * Get raw key bits from an RSA public key.
 */
OSStatus sslGetPubKeyBits(
	SSLContext			*ctx,
	const CSSM_KEY		*pubKey,
	CSSM_CSP_HANDLE		cspHand,
	SSLBuffer			*modulus,		// data mallocd and RETURNED
	SSLBuffer			*exponent)		// data mallocd and RETURNED
{
	CSSM_KEY			wrappedKey;
	CSSM_BOOL			didWrap = CSSM_FALSE;
	const CSSM_KEYHEADER *hdr;
	SSLBuffer			pubKeyBlob;
	OSStatus			srtn;
	
	assert(ctx != NULL);
	assert(modulus != NULL);
	assert(exponent != NULL);
	assert(pubKey != NULL);
	
	hdr = &pubKey->KeyHeader;
	if(hdr->KeyClass != CSSM_KEYCLASS_PUBLIC_KEY) {
		sslErrorLog("sslGetPubKeyBits: bad keyClass (%ld)\n", hdr->KeyClass);
		return errSSLInternal;
	}
	if(hdr->AlgorithmId != CSSM_ALGID_RSA) {
		sslErrorLog("sslGetPubKeyBits: bad AlgorithmId (%ld)\n", hdr->AlgorithmId);
		return errSSLInternal;
	}

	/* Note currently ALL public keys are raw, obtained from the CL... */
	assert(hdr->BlobType == CSSM_KEYBLOB_RAW);

	/* 
	 * Handle possible reference format - I think it should be in
	 * blob form since it came from the DL, but conversion is 
	 * simple.
	 */
	switch(hdr->BlobType) {
		case CSSM_KEYBLOB_RAW:
			/* easy case */
			CSSM_TO_SSLBUF(&pubKey->KeyData, &pubKeyBlob);
			break;

		case CSSM_KEYBLOB_REFERENCE:
			
			sslErrorLog("sslGetPubKeyBits: bad BlobType (%ld)\n", 
				hdr->BlobType);
			return errSSLInternal;

			#if 0
			/* 
			 * Convert to a blob via "NULL wrap"; no wrapping key, 
			 * ALGID_NONE 
			 */ 
			srtn = attachToCsp(ctx);
			if(srtn) {
				return srtn;
			}
			memset(&creds, 0, sizeof(CSSM_ACCESS_CREDENTIALS));
			crtn = CSSM_CSP_CreateSymmetricContext(ctx->cspHand,
					CSSM_ALGID_NONE,
					CSSM_ALGMODE_NONE,
					&creds,			// creds
					pubKey,
					NULL,			// InitVector
					CSSM_PADDING_NONE,
					0,			// reserved
					&ccHand);
			if(crtn) {
				stPrintCdsaError("sslGetPubKeyBits: CreateSymmetricContext failure", crtn); 
				return errSSLCrypto;
			}
			memset(&wrappedKey, 0, sizeof(CSSM_KEY));
			crtn = CSSM_WrapKey(ccHand,
				&creds,
				pubKey,
				NULL,			// descriptiveData
				&wrappedKey);
			CSSM_DeleteContext(ccHand);
			if(crtn) {
				stPrintCdsaError("CSSM_WrapKey", crtn);
				return errSSLCrypto;
			}
			hdr = &wrappedKey.KeyHeader;
			if(hdr->BlobType != CSSM_KEYBLOB_RAW) {
				sslErrorLog("sslGetPubKeyBits: bad BlobType (%ld) after WrapKey\n", 
					hdr->BlobType);
				return errSSLCrypto;
			}
			didWrap = CSSM_TRUE;
			CSSM_TO_SSLBUF(&wrappedKey.KeyData, &pubKeyBlob);
			break;
			#endif	/* 0 */
			
		default:
			sslErrorLog("sslGetPubKeyBits: bad BlobType (%ld)\n", 
				hdr->BlobType);
			return errSSLInternal;
	
	}	/* switch BlobType */

	assert(hdr->BlobType == CSSM_KEYBLOB_RAW); 
	srtn = sslDecodeRsaBlob(&pubKeyBlob, modulus, exponent);
	if(didWrap) {
		CSSM_FreeKey(ctx->cspHand, NULL, &wrappedKey, CSSM_FALSE);
	}
	return srtn;
}

/*
 * Given raw RSA key bits, cook up a CSSM_KEY_PTR. Used in 
 * Server-initiated key exchange. 
 */
OSStatus sslGetPubKeyFromBits(
	SSLContext			*ctx,
	const SSLBuffer		*modulus,	
	const SSLBuffer		*exponent,	
	CSSM_KEY_PTR		*pubKey,		// mallocd and RETURNED
	CSSM_CSP_HANDLE		*cspHand)		// RETURNED
{
	CSSM_KEY_PTR		key = NULL;
	OSStatus				serr;
	SSLBuffer			blob;
	CSSM_KEYHEADER_PTR	hdr;
	CSSM_KEY_SIZE		keySize;
	CSSM_RETURN			crtn;
	
	assert((ctx != NULL) && (modulus != NULL) && (exponent != NULL));
	assert((pubKey != NULL) && (cspHand != NULL));
	
	*pubKey = NULL;
	*cspHand = 0;
	
	serr = attachToCsp(ctx);
	if(serr) {
		return serr;
	}
	serr = sslEncodeRsaBlob(modulus, exponent, &blob);
	if(serr) {
		return serr;
	}
	
	/* the rest is boilerplate, cook up a good-looking public key */
	key = (CSSM_KEY_PTR)sslMalloc(sizeof(CSSM_KEY));
	if(key == NULL) {
		return memFullErr;
	}
	memset(key, 0, sizeof(CSSM_KEY));
	hdr = &key->KeyHeader;
	
    hdr->HeaderVersion = CSSM_KEYHEADER_VERSION;
    /* key_ptr->KeyHeader.CspId is unknown (remains 0) */
    hdr->BlobType = CSSM_KEYBLOB_RAW;
    hdr->AlgorithmId = CSSM_ALGID_RSA;
    hdr->Format = CSSM_KEYBLOB_RAW_FORMAT_PKCS1;
    hdr->KeyClass = CSSM_KEYCLASS_PUBLIC_KEY;
    /* comply with ASA requirements */
    hdr->KeyUsage = CSSM_KEYUSE_VERIFY;
    hdr->KeyAttr = CSSM_KEYATTR_EXTRACTABLE;
    /* key_ptr->KeyHeader.StartDate is unknown  (remains 0) */
    /* key_ptr->KeyHeader.EndDate is unknown  (remains 0) */
    hdr->WrapAlgorithmId = CSSM_ALGID_NONE;
    hdr->WrapMode = CSSM_ALGMODE_NONE;

	/* blob->data was mallocd by sslEncodeRsaBlob, pass it over to 
	 * actual key */
	SSLBUF_TO_CSSM(&blob, &key->KeyData);
	
	/*
	 * Get keySizeInBits. This also serves to validate the key blob
	 * we just cooked up.
	 */
    crtn = CSSM_QueryKeySizeInBits(ctx->cspHand, CSSM_INVALID_HANDLE, key, &keySize);
	if(crtn) {	
    	stPrintCdsaError("sslGetPubKeyFromBits: QueryKeySizeInBits\n", crtn);
		serr = errSSLCrypto;
    	goto abort;
	}
	
	/* success */
    hdr->LogicalKeySizeInBits = keySize.EffectiveKeySizeInBits;
    *pubKey = key;
    *cspHand = ctx->cspHand;
	return noErr;
	
abort:
	/* note this frees the blob */
	sslFreeKey(ctx->cspHand, &key, NULL);
	return serr;
}

#pragma mark -
#pragma mark *** Public Certificate Functions ***

/*
 * Given a DER-encoded cert, obtain its public key as a CSSM_KEY_PTR.
 * Caller must CSSM_FreeKey and free the CSSM_KEY_PTR itself. 
 *
 * For now, the returned cspHand is a copy of ctx->cspHand, so it
 * doesn't have to be detached later - this may change.
 *
 * Update: since CSSM_CL_CertGetKeyInfo() doesn't provide a means for
 * us to tell the CL what CSP to use, we really have no way of knowing 
 * what is going on here...we return the process-wide (bare) cspHand,
 * which is currently always able to deal with this raw public key. 
 */
OSStatus sslPubKeyFromCert(
	SSLContext 			*ctx,
	const SSLBuffer		&derCert,
	CSSM_KEY_PTR		*pubKey,		// RETURNED
	CSSM_CSP_HANDLE		*cspHand)		// RETURNED
{
	OSStatus 			serr;
	CSSM_DATA		certData;
	CSSM_RETURN		crtn;
	
	assert(ctx != NULL);
	assert(pubKey != NULL);
	assert(cspHand != NULL);
	
	*pubKey = NULL;
	*cspHand = 0;
	
	serr = attachToCl(ctx);
	if(serr) {
		return serr;
	}
	serr = attachToCsp(ctx);
	if(serr) {
		return serr;
	}
	SSLBUF_TO_CSSM(&derCert, &certData);
	crtn = CSSM_CL_CertGetKeyInfo(ctx->clHand, &certData, pubKey);
	if(crtn) {
		return errSSLBadCert;
	}
	else {
		*cspHand = ctx->cspHand; 
		return noErr;
	}
}

/*
 * Release each element in a CFArray.
 */
static void sslReleaseArray(
	CFArrayRef a)
{
	CFIndex num = CFArrayGetCount(a);
	for(CFIndex dex=0; dex<num; dex++) {
		CFTypeRef elmt = (CFTypeRef)CFArrayGetValueAtIndex(a, dex);
		secdebug("sslcert", "Freeing cert %p", elmt);
		CFRelease(elmt);
	}
}

/*
 * Verify a chain of DER-encoded certs.
 * First cert in a chain is root; this must also be present
 * in ctx->trustedCerts. 
 *
 * If arePeerCerts is true, host name verification is enabled and we
 * save the resulting SecTrustRef in ctx->peerSecTrust. Otherwise
 * we're just validating our own certs; no host name checking and 
 * peerSecTrust is transient.
 */
 OSStatus sslVerifyCertChain(
	SSLContext				*ctx,
	const SSLCertificate	&certChain,
	bool					arePeerCerts /* = true */) 
{
	UInt32 						numCerts;
	int 						i;
	OSStatus					serr;
	SSLCertificate				*c = (SSLCertificate *)&certChain;
	CSSM_RETURN					crtn;
	CSSM_APPLE_TP_SSL_OPTIONS	sslOpts;
	CSSM_APPLE_TP_ACTION_DATA	tpActionData;
	SecPolicyRef				policy = NULL;
	SecPolicySearchRef			policySearch = NULL;
	CFDataRef					actionData = NULL;
	CSSM_DATA					sslOptsData;
	CFMutableArrayRef			anchors = NULL;
	SecCertificateRef 			cert;			// only lives in CFArrayRefs
	SecTrustResultType			secTrustResult;
	CFMutableArrayRef			kcList = NULL;
	SecTrustRef					theTrust = NULL;
	
	if(ctx->peerSecTrust && arePeerCerts) {
		/* renegotiate - start with a new SecTrustRef */
		CFRelease(ctx->peerSecTrust);
		ctx->peerSecTrust = NULL;
	}
	
	numCerts = SSLGetCertificateChainLength(&certChain);
	if(numCerts == 0) {
		/* nope */
		return errSSLBadCert;
	}
	
	/* 
	 * SSLCertificate chain --> CFArrayRef of SecCertificateRefs.
	 * TP Cert group has root at the end, opposite of 
	 * SSLCertificate chain. 
	 */
	CFMutableArrayRef certGroup = CFArrayCreateMutable(NULL, numCerts, 
		&kCFTypeArrayCallBacks);
	if(certGroup == NULL) {
		return memFullErr;
	}
	/* subsequent errors to errOut: */
	
	for(i=numCerts-1; i>=0; i--) {
		CSSM_DATA cdata;
		SSLBUF_TO_CSSM(&c->derCert, &cdata);
		serr = SecCertificateCreateFromData(&cdata,	CSSM_CERT_X_509v3,
			CSSM_CERT_ENCODING_DER, &cert);
		if(serr) {
			goto errOut;
		}
		/*
		 * Can't set a value at index i when there is an empty element
		 * at i=1!
		 */
		secdebug("sslcert", "Adding cert %p", cert);
		CFArrayInsertValueAtIndex(certGroup, 0, cert);
		c = c->next;
	}
	
	/* 
	 * Cook up an SSL-specific SecPolicyRef. This will persists as part
	 * of the SecTrustRef object we'll be creating.
	 */
	serr = SecPolicySearchCreate(CSSM_CERT_X_509v3,
		&CSSMOID_APPLE_TP_SSL,
		NULL,
		&policySearch);
	if(serr) {
		sslErrorLog("***sslVerifyCertChain: SecPolicySearchCreate rtn %d\n",
			(int)serr);
		goto errOut;
	}
	serr = SecPolicySearchCopyNext(policySearch, &policy);
	if(serr) {
		sslErrorLog("***sslVerifyCertChain: SecPolicySearchCopyNext rtn %d\n",
			(int)serr);
		goto errOut;
	}
	sslOpts.Version = CSSM_APPLE_TP_SSL_OPTS_VERSION;
	if(arePeerCerts) {
		sslOpts.ServerNameLen = ctx->peerDomainNameLen;
		sslOpts.ServerName = ctx->peerDomainName;
	}
	else {
		sslOpts.ServerNameLen = 0;
		sslOpts.ServerName = NULL;
	}
	sslOpts.Flags = 0;
	if(ctx->protocolSide == SSL_ServerSide) {
		/* we're evaluating a client cert */
		sslOpts.Flags |= CSSM_APPLE_TP_SSL_CLIENT;
	}
	sslOptsData.Data = (uint8 *)&sslOpts;
	sslOptsData.Length = sizeof(sslOpts);
	serr = SecPolicySetValue(policy, &sslOptsData);
	if(serr) {
		sslErrorLog("***sslVerifyCertChain: SecPolicySetValue rtn %d\n",
			(int)serr);
		goto errOut;
	}
	
	/* now a SecTrustRef */
	serr = SecTrustCreateWithCertificates(certGroup, policy, &theTrust);
	if(serr) {
		sslErrorLog("***sslVerifyCertChain: SecTrustCreateWithCertificates "
			"rtn %d\n",	(int)serr);
		goto errOut;
	}
	
	/* anchors - default, or ours? */
	if(ctx->numTrustedCerts != 0) {
		anchors = CFArrayCreateMutable(NULL, ctx->numTrustedCerts, 
			&kCFTypeArrayCallBacks);
		if(anchors == NULL) {
			serr = memFullErr;
			goto errOut;
		}
		for(i=0; i<(int)ctx->numTrustedCerts; i++) {
			serr = SecCertificateCreateFromData(&ctx->trustedCerts[i],
				CSSM_CERT_X_509v3, CSSM_CERT_ENCODING_DER, &cert);
			if(serr) {
				goto errOut;
			}
			secdebug("sslcert", "Adding cert %p", cert);
			CFArraySetValueAtIndex(anchors, i, cert);
		}
		serr = SecTrustSetAnchorCertificates(theTrust, anchors);
		if(serr) {
			sslErrorLog("***sslVerifyCertChain: SecTrustSetAnchorCertificates "
				"rtn %d\n",	(int)serr);
			goto errOut;
		}
	}
	tpActionData.Version = CSSM_APPLE_TP_ACTION_VERSION;
	tpActionData.ActionFlags = 0;
	if(ctx->allowExpiredCerts) {
		tpActionData.ActionFlags |= CSSM_TP_ACTION_ALLOW_EXPIRED;
	}
	if(ctx->allowExpiredRoots) {
		tpActionData.ActionFlags |= CSSM_TP_ACTION_ALLOW_EXPIRED_ROOT;
	}
	actionData = CFDataCreate(NULL, (UInt8 *)&tpActionData, sizeof(tpActionData));
	
	serr = SecTrustSetParameters(theTrust, CSSM_TP_ACTION_DEFAULT,
		actionData);
	if(serr) {
		sslErrorLog("***sslVerifyCertChain: SecTrustSetParameters rtn %d\n",
			(int)serr);
		goto errOut;
	}

	#if 0
	/* Disabled for Radar 3421314 */
	/*
	 * Avoid searching user keychains for intermediate certs by specifying
	 * an empty array of keychains
	 */
	kcList = CFArrayCreateMutable(NULL, 0, NULL);
	if(kcList == NULL) {
		sslErrorLog("***sslVerifyCertChain: error creating null kcList\n");
		serr = memFullErr;
		goto errOut;
	}
	serr = SecTrustSetKeychains(theTrust, kcList);
	if(serr) {
		sslErrorLog("***sslVerifyCertChain: SecTrustSetKeychains rtn %d\n",
			(int)serr);
		goto errOut;
	}
	#endif
	
	/* 
	 * Save this no matter what if we're evaluating peer certs.
	 * We do a retain here so we can unconditionally release theTrust
	 * at the end of this routine in case of previous error or 
	 * !arePeerCerts.
	 */ 
	if(arePeerCerts) {
		ctx->peerSecTrust = theTrust;
		CFRetain(theTrust);
	}

	if(!ctx->enableCertVerify) {
		/* trivial case, this is caller's responsibility */
		serr = noErr;
		goto errOut;
	}
	
	/*
	 * Here we go; hand it over to SecTrust/TP. 
	 */
	serr = SecTrustEvaluate(theTrust, &secTrustResult);
	if(serr) {
		sslErrorLog("***sslVerifyCertChain: SecTrustEvaluate rtn %d\n",
			(int)serr);
		goto errOut;
	}
	switch(secTrustResult) {
		case kSecTrustResultUnspecified:
			/* cert chain valid, no special UserTrust assignments */
		case kSecTrustResultProceed:
			/* cert chain valid AND user explicitly trusts this */
			crtn = CSSM_OK;
			break;
		case kSecTrustResultDeny:
		case kSecTrustResultConfirm:
			/*
			 * Cert chain may well have verified OK, but user has flagged
			 * one of these certs as untrustable.
			 */
			crtn = CSSMERR_TP_NOT_TRUSTED;
			break;
		default:
		{
			OSStatus osCrtn;
			serr = SecTrustGetCssmResultCode(theTrust, &osCrtn);
			if(serr) {
				sslErrorLog("***sslVerifyCertChain: SecTrustGetCssmResultCode"
					" rtn %d\n", (int)serr);
				goto errOut;
			}
			crtn = osCrtn;
		}
	}
	if(crtn) {	
		/* get some detailed error info */
		switch(crtn) {
			case CSSMERR_TP_INVALID_ANCHOR_CERT: 
				/* root found but we don't trust it */
				if(ctx->allowAnyRoot) {
					serr = noErr;
					sslErrorLog("***Warning: accepting unknown root cert\n");
				}
				else {
					serr = errSSLUnknownRootCert;
				}
				break;
			case CSSMERR_TP_NOT_TRUSTED:
				/* no root, not even in implicit SSL roots */
				if(ctx->allowAnyRoot) {
					sslErrorLog("***Warning: accepting unverified cert chain\n");
					serr = noErr;
				}
				else {
					serr = errSSLNoRootCert;
				}
				break;
			case CSSMERR_TP_CERT_EXPIRED:
				assert(!ctx->allowExpiredCerts);
				serr = errSSLCertExpired;
				break;
			case CSSMERR_TP_CERT_NOT_VALID_YET:
				serr = errSSLCertNotYetValid;
				break;
			case CSSMERR_APPLETP_HOSTNAME_MISMATCH:
				serr = errSSLHostNameMismatch;
				break;
			default:
				stPrintCdsaError("sslVerifyCertChain: SecTrustEvaluate returned", 
						crtn);
				serr = errSSLXCertChainInvalid;
				break;
		}
	} 	/* SecTrustEvaluate error */

errOut:
	/* 
	 * Free up resources - certGroup, policy, etc. Note that most of these
	 * will actually persist as long as the current SSLContext does since
	 * peerSecTrust holds references to these.
	 */
	if(policy) {
		CFRelease(policy);
	}
	if(policySearch) {
		CFRelease(policySearch);
	}
	if(actionData) {
		CFRelease(actionData);
	}
	if(anchors) {	
		sslReleaseArray(anchors);
		CFRelease(anchors);
	}
	if(certGroup) {	
		sslReleaseArray(certGroup);
		CFRelease(certGroup);
	}
	if(kcList) {
		/* empty, no contents to release */
		CFRelease(kcList);
	}	
	if(theTrust) {
		CFRelease(theTrust);
	}
	return serr;
}

#ifndef	NDEBUG
void stPrintCdsaError(const char *op, CSSM_RETURN crtn)
{
	cssmPerror(op, crtn);
}

char *stCssmErrToStr(CSSM_RETURN err)
{
#if 1
	return const_cast<char *>("error");
#else
	string errStr = cssmErrorString(err);
	return const_cast<char *>(errStr.c_str());
#endif
}
#endif

#pragma mark -
#pragma mark *** Diffie-Hellman support ***

/*
 * Generate a Diffie-Hellman key pair. Algorithm parameters always
 * come from the server, so on client side we have the parameters
 * as two SSLBuffers. On server side we have the pre-encoded block
 * which comes from ServerDhParams.
 */
OSStatus sslDhGenKeyPairClient(
	SSLContext		*ctx,
	const SSLBuffer	&prime,
	const SSLBuffer	&generator,
	CSSM_KEY_PTR	publicKey,		// RETURNED
	CSSM_KEY_PTR	privateKey)		// RETURNED
{
	assert((prime.data != NULL) && (generator.data != NULL));
	if(prime.data && !generator.data) {
		return errSSLProtocol;
	}
	if(!prime.data && generator.data) {
		return errSSLProtocol;
	}
	
	SSLBuffer sParam;
	OSStatus ortn = sslEncodeDhParams(&prime, &generator, &sParam);
	if(ortn) {
		sslErrorLog("***sslDhGenerateKeyPairClient: DH param error\n");
		return ortn;
	}
	ortn = sslDhGenerateKeyPair(ctx, sParam, prime.length * 8, publicKey, privateKey);
	SSLFreeBuffer(sParam, ctx);
	return ortn;
}

OSStatus sslDhGenerateKeyPair(
	SSLContext		*ctx,
	const SSLBuffer	&paramBlob,
	UInt32			keySizeInBits,
	CSSM_KEY_PTR	publicKey,		// RETURNED
	CSSM_KEY_PTR	privateKey)		// RETURNED
{
	CSSM_RETURN		crtn;
	CSSM_CC_HANDLE 	ccHandle;
	CSSM_DATA		labelData = {8, (uint8 *)"tempKey"};
	OSStatus		ortn = noErr;
	CSSM_DATA		cParamBlob;

	assert(ctx != NULL);
	assert(ctx->cspHand != 0);
	
	memset(publicKey, 0, sizeof(CSSM_KEY));
	memset(privateKey, 0, sizeof(CSSM_KEY));
	SSLBUF_TO_CSSM(&paramBlob, &cParamBlob);
	
	crtn = CSSM_CSP_CreateKeyGenContext(ctx->cspHand,
		CSSM_ALGID_DH,
		keySizeInBits,
		NULL,					// Seed
		NULL,					// Salt
		NULL,					// StartDate
		NULL,					// EndDate
		&cParamBlob,
		&ccHandle);
	if(crtn) {
		stPrintCdsaError("DH CSSM_CSP_CreateKeyGenContext", crtn);
		return errSSLCrypto;
	}
	
	crtn = CSSM_GenerateKeyPair(ccHandle,
		CSSM_KEYUSE_DERIVE,		// only legal use of a Diffie-Hellman key 
		CSSM_KEYATTR_RETURN_DATA | CSSM_KEYATTR_EXTRACTABLE,
		&labelData,
		publicKey,
		/* private key specification */
		CSSM_KEYUSE_DERIVE,
		CSSM_KEYATTR_RETURN_REF,
		&labelData,				// same labels
		NULL,					// CredAndAclEntry
		privateKey);
	if(crtn) {
		stPrintCdsaError("DH CSSM_GenerateKeyPair", crtn);
		ortn = errSSLCrypto;
	}
	CSSM_DeleteContext(ccHandle);
	return ortn;
}

/*
 * Perform Diffie-Hellman key exchange. 
 * Valid on entry:
 *    	ctx->dhPrivate
 *		ctx->dhPeerPublic
 *
 * This generates deriveSizeInBits of key-exchanged data. 
 */
 
/* the alg isn't important; we just want to be able to cook up lots of bits */
#define DERIVE_KEY_ALG			CSSM_ALGID_RC5
#define DERIVE_KEY_MAX_BYTES	255

OSStatus sslDhKeyExchange(
	SSLContext		*ctx,
	uint32			deriveSizeInBits,
	SSLBuffer		*exchanged)
{
	CSSM_RETURN 			crtn;
	CSSM_ACCESS_CREDENTIALS	creds;
	CSSM_CC_HANDLE			ccHandle;
	CSSM_DATA				labelData = {8, (uint8 *)"tempKey"};
	CSSM_KEY				derivedKey;
	OSStatus				ortn = noErr;
	
	assert(ctx != NULL);
	assert(ctx->cspHand != 0);
	assert(ctx->dhPrivate != NULL);
	if(ctx->dhPeerPublic.length == 0) {
		/* comes from peer, don't panic */
		sslErrorLog("cdsaDhKeyExchange: null peer public key\n");
		return errSSLProtocol;
	}
	if(deriveSizeInBits > (DERIVE_KEY_MAX_BYTES * 8)) {
		sslErrorLog("cdsaDhKeyExchange: deriveSizeInBits %u bits\n",
			(unsigned)deriveSizeInBits);
		return errSSLProtocol;
	}
	
	memset(&creds, 0, sizeof(CSSM_ACCESS_CREDENTIALS));
	memset(&derivedKey, 0, sizeof(CSSM_KEY));
	
	crtn = CSSM_CSP_CreateDeriveKeyContext(ctx->cspHand,
		CSSM_ALGID_DH,
		DERIVE_KEY_ALG,
		deriveSizeInBits,
		&creds,
		ctx->dhPrivate,	// BaseKey
		0,				// IterationCount
		0,				// Salt
		0,				// Seed
		&ccHandle);
	if(crtn) {
		stPrintCdsaError("DH CSSM_CSP_CreateDeriveKeyContext", crtn);
		return errSSLCrypto;
	}
	
	/* public key passed in as CSSM_DATA *Param */
	CSSM_DATA theirPubKeyData;
	SSLBUF_TO_CSSM(&ctx->dhPeerPublic, &theirPubKeyData);
	
	crtn = CSSM_DeriveKey(ccHandle,
		&theirPubKeyData,
		CSSM_KEYUSE_ANY, 
		CSSM_KEYATTR_RETURN_DATA | CSSM_KEYATTR_EXTRACTABLE,
		&labelData,
		NULL,				// cread/acl
		&derivedKey);
	if(crtn) {
		stPrintCdsaError("DH CSSM_DeriveKey", crtn);
		ortn = errSSLCrypto;
	}
	else {
		CSSM_TO_SSLBUF(&derivedKey.KeyData, exchanged);
	}
	CSSM_DeleteContext(ccHandle);
	return ortn;
}

/*
 * After ciphersuite negotiation is complete, verify that we have
 * the capability of actually performing the negotiated cipher.
 * Currently we just verify that we have a cert and private signing 
 * key, if needed, and that the signing key's algorithm matches the
 * expected key exchange method.
 * This is currnetly only called from FindCipherSpec(), after
 * it sets ctx->selectedCipherSpec to a (supposedly) valid value.
 */
OSStatus sslVerifyNegotiatedCipher(
	SSLContext *ctx)
{
	if(ctx->protocolSide == SSL_ClientSide) {
		return noErr;
	}
	CSSM_ALGORITHMS requireAlg = CSSM_ALGID_NONE;
	
    switch (ctx->selectedCipherSpec->keyExchangeMethod) {
		case SSL_RSA:
        case SSL_RSA_EXPORT:
		case SSL_DH_RSA:
		case SSL_DH_RSA_EXPORT:
		case SSL_DHE_RSA:
		case SSL_DHE_RSA_EXPORT:
			requireAlg = CSSM_ALGID_RSA;
			break;
 		case SSL_DHE_DSS:
		case SSL_DHE_DSS_EXPORT:
 		case SSL_DH_DSS:
		case SSL_DH_DSS_EXPORT:
			requireAlg = CSSM_ALGID_DSA;
			break;
		case SSL_DH_anon:
		case SSL_DH_anon_EXPORT:
			/* CSSM_ALGID_NONE, no signing key */
			break;
		default:
			/* needs update per cipherSpecs.cpp */
			assert(0);
			return errSSLInternal;
    }
	if(requireAlg == CSSM_ALGID_NONE) {
		return noErr;
	}
	
	/* private signing key required */
	if(ctx->signingPrivKeyRef == NULL) {
		sslErrorLog("sslVerifyNegotiatedCipher: no signing key\n");
		return errSSLBadConfiguration;
	}
	{
		const CSSM_KEY *cssmKey;
		OSStatus ortn = SecKeyGetCSSMKey(ctx->signingPrivKeyRef, &cssmKey);
		if(ortn) {
			sslErrorLog("sslVerifyNegotiatedCipher: SecKeyGetCSSMKey err %d\n",
			(int)ortn);
			return ortn;
		}
		if(cssmKey->KeyHeader.AlgorithmId != requireAlg) {
			sslErrorLog("sslVerifyNegotiatedCipher: signing key alg mismatch\n");
			return errSSLBadConfiguration;
		}
	}
	return noErr;
}

