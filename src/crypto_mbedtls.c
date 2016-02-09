/*
crypto_mbedtls.c
Copyright (C) 2016  Belledonne Communications SARL

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/
#include <stdlib.h>
#include <string.h>
#include "utils.h"
#include <bctoolbox/crypto.h>

#include <mbedtls/ssl.h>
#include <mbedtls/error.h>
#include <mbedtls/net.h>
#include <mbedtls/base64.h>

#include <mbedtls/pem.h>
#include <mbedtls/x509.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/md5.h>
#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>
#include <mbedtls/sha512.h>

#include <mbedtls/debug.h>

#define bctoolbox_error printf

static int bctoolbox_ssl_sendrecv_callback_return_remap(int32_t ret_code) {
	switch (ret_code) {
		case BCTOOLBOX_ERROR_NET_WANT_READ:
			return MBEDTLS_ERR_SSL_WANT_READ;
		case BCTOOLBOX_ERROR_NET_WANT_WRITE:
			return MBEDTLS_ERR_SSL_WANT_WRITE;
		case BCTOOLBOX_ERROR_NET_CONN_RESET:
			return MBEDTLS_ERR_NET_CONN_RESET;
		default:
			return (int)ret_code;
	}
}

void bctoolbox_strerror(int32_t error_code, char *buffer, size_t buffer_length) {
	if (error_code>0) {
		snprintf(buffer, buffer_length, "%s", "Invalid Error code");
		return ;
	}

	/* mbedtls error code are all negatived and bas smaller than 0x0000F000 */
	/* bctoolbox defined error codes are all in format -0x7XXXXXXX */
	if (-error_code<0x00010000) { /* it's a polarssl error code */
		mbedtls_strerror(error_code, buffer, buffer_length);
		return;
	}

	snprintf(buffer, buffer_length, "%s [-0x%x]", "bctoolbox defined error code", -error_code);
	return;
}

int32_t bctoolbox_base64_encode(unsigned char *output, size_t *output_length, const unsigned char *input, size_t input_length) {
	size_t byte_written = 0;
	int ret = mbedtls_base64_encode(output, *output_length, &byte_written, input, input_length);
	*output_length = byte_written;
	if (ret == MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
		return BCTOOLBOX_ERROR_OUTPUT_BUFFER_TOO_SMALL;
	}
	return ret;
}

int32_t bctoolbox_base64_decode(unsigned char *output, size_t *output_length, const unsigned char *input, size_t input_length) {
	size_t byte_written = 0;
	int ret = mbedtls_base64_decode(output, *output_length, &byte_written, input, input_length);
	*output_length = byte_written;
	if (ret == MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
		return BCTOOLBOX_ERROR_OUTPUT_BUFFER_TOO_SMALL;
	}
	if (ret == MBEDTLS_ERR_BASE64_INVALID_CHARACTER) {
		return BCTOOLBOX_ERROR_INVALID_BASE64_INPUT;
	}

	return ret;
}

/*** Random Number Generation ***/
struct bctoolbox_rng_context_struct {
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context ctr_drbg;
};

bctoolbox_rng_context_t *bctoolbox_rng_context_new(void) {
	bctoolbox_rng_context_t *ctx = bctoolbox_malloc0(sizeof(bctoolbox_rng_context_t));
	mbedtls_entropy_init(&(ctx->entropy));
	mbedtls_ctr_drbg_init(&(ctx->ctr_drbg));
	mbedtls_ctr_drbg_seed(&(ctx->ctr_drbg), mbedtls_entropy_func, &(ctx->entropy), NULL, 0);
	return ctx;
}

int32_t bctoolbox_rng_get(bctoolbox_rng_context_t *context, unsigned char*output, size_t output_length) {
	return mbedtls_ctr_drbg_random(&(context->ctr_drbg), output, output_length);
}

void bctoolbox_rng_context_free(bctoolbox_rng_context_t *context) {
	mbedtls_ctr_drbg_free(&(context->ctr_drbg));
	mbedtls_entropy_free(&(context->entropy));
	bctoolbox_free(context);
}

/*** signing key ***/
bctoolbox_signing_key_t *bctoolbox_signing_key_new(void) {
	mbedtls_pk_context *key = bctoolbox_malloc0(sizeof(mbedtls_pk_context));
	mbedtls_pk_init(key);
	return (bctoolbox_signing_key_t *)key;
}

void bctoolbox_signing_key_free(bctoolbox_signing_key_t *key) {
	mbedtls_pk_free((mbedtls_pk_context *)key);
	bctoolbox_free(key);
}

char *bctoolbox_signing_key_get_pem(bctoolbox_signing_key_t *key) {
	char *pem_key;
	if (key == NULL) return NULL;
	pem_key = (char *)bctoolbox_malloc0(4096);
	mbedtls_pk_write_key_pem( (mbedtls_pk_context *)key, (unsigned char *)pem_key, 4096);
	return pem_key;
}

int32_t bctoolbox_signing_key_parse(bctoolbox_signing_key_t *key, const char *buffer, size_t buffer_length, const unsigned char *password, size_t password_length) {
	int err;
	err=mbedtls_pk_parse_key((mbedtls_pk_context *)key, (const unsigned char *)buffer, buffer_length, password, password_length);
	if(err==0 && !mbedtls_pk_can_do((mbedtls_pk_context *)key, MBEDTLS_PK_RSA)) {
		err=MBEDTLS_ERR_PK_TYPE_MISMATCH;
	}
	if (err<0) {
		char tmp[128];
		mbedtls_strerror(err,tmp,sizeof(tmp));
		bctoolbox_error("cannot parse public key because [%s]",tmp);
		return BCTOOLBOX_ERROR_UNABLE_TO_PARSE_KEY;
	}
	return 0;
}

int32_t bctoolbox_signing_key_parse_file(bctoolbox_signing_key_t *key, const char *path, const char *password) {
	int err;
	err=mbedtls_pk_parse_keyfile((mbedtls_pk_context *)key, path, password);
	if(err==0 && !mbedtls_pk_can_do((mbedtls_pk_context *)key,MBEDTLS_PK_RSA)) {
		err=MBEDTLS_ERR_PK_TYPE_MISMATCH;
	}
	if (err<0) {
		char tmp[128];
		mbedtls_strerror(err,tmp,sizeof(tmp));
		bctoolbox_error("cannot parse public key because [%s]",tmp);
		return BCTOOLBOX_ERROR_UNABLE_TO_PARSE_KEY;
	}
	return 0;
}



/*** Certificate ***/
char *bctoolbox_x509_certificates_chain_get_pem(bctoolbox_x509_certificate_t *cert) {
	char *pem_certificate = NULL;
	size_t olen=0;

	pem_certificate = (char *)bctoolbox_malloc0(4096);
	mbedtls_pem_write_buffer("-----BEGIN CERTIFICATE-----\n", "-----END CERTIFICATE-----\n", ((mbedtls_x509_crt *)cert)->raw.p, ((mbedtls_x509_crt *)cert)->raw.len, (unsigned char*)pem_certificate, 4096, &olen );
	return pem_certificate;
}


bctoolbox_x509_certificate_t *bctoolbox_x509_certificate_new(void) {
	mbedtls_x509_crt *cert = bctoolbox_malloc0(sizeof(mbedtls_x509_crt));
	mbedtls_x509_crt_init(cert);
	return (bctoolbox_x509_certificate_t *)cert;
}

void bctoolbox_x509_certificate_free(bctoolbox_x509_certificate_t *cert) {
	mbedtls_x509_crt_free((mbedtls_x509_crt *)cert);
	bctoolbox_free(cert);
}

int32_t bctoolbox_x509_certificate_get_info_string(char *buf, size_t size, const char *prefix, const bctoolbox_x509_certificate_t *cert) {
	return mbedtls_x509_crt_info(buf, size, prefix, (mbedtls_x509_crt *)cert);
}

int32_t bctoolbox_x509_certificate_parse_file(bctoolbox_x509_certificate_t *cert, const char *path) {
	return mbedtls_x509_crt_parse_file((mbedtls_x509_crt *)cert, path);
}

int32_t bctoolbox_x509_certificate_parse_path(bctoolbox_x509_certificate_t *cert, const char *path) {
	return mbedtls_x509_crt_parse_path((mbedtls_x509_crt *)cert, path);
}

int32_t bctoolbox_x509_certificate_parse(bctoolbox_x509_certificate_t *cert, const char *buffer, size_t buffer_length) {
	char buff[256];
	int ret = mbedtls_x509_crt_parse((mbedtls_x509_crt *)cert, (const unsigned char *)buffer, buffer_length);
	if (ret<0) {
		mbedtls_strerror(ret, buff, 255);
	}
	return ret;
}

int32_t bctoolbox_x509_certificate_get_der_length(bctoolbox_x509_certificate_t *cert) {
	if (cert!=NULL) {
		return ((mbedtls_x509_crt *)cert)->raw.len;
	}
	return 0;
}

int32_t bctoolbox_x509_certificate_get_der(bctoolbox_x509_certificate_t *cert, unsigned char *buffer, size_t buffer_length) {
	if (cert==NULL) {
		return BCTOOLBOX_ERROR_INVALID_CERTIFICATE;
	}
	if (((mbedtls_x509_crt *)cert)->raw.len>buffer_length-1) { /* check buffer size is ok, +1 for the NULL termination added at the end */
		return BCTOOLBOX_ERROR_OUTPUT_BUFFER_TOO_SMALL;
	}
	memcpy(buffer, ((mbedtls_x509_crt *)cert)->raw.p, ((mbedtls_x509_crt *)cert)->raw.len);
	buffer[((mbedtls_x509_crt *)cert)->raw.len] = '\0'; /* add a null termination char */

	return 0;
}

int32_t bctoolbox_x509_certificate_get_subject_dn(bctoolbox_x509_certificate_t *cert, char *dn, size_t dn_length) {
	if (cert==NULL) {
		return BCTOOLBOX_ERROR_INVALID_CERTIFICATE;
	}

	return mbedtls_x509_dn_gets(dn, dn_length, &(((mbedtls_x509_crt *)cert)->subject));
}

int32_t bctoolbox_x509_certificate_generate_selfsigned(const char *subject, bctoolbox_x509_certificate_t *certificate, bctoolbox_signing_key_t *pkey, char * pem, size_t pem_length) {
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context ctr_drbg;
	int ret;
	mbedtls_mpi serial;
	mbedtls_x509write_cert crt;
	char file_buffer[8192];
	size_t file_buffer_len = 0;
	char formatted_subject[512];

	/* subject may be a sip URL or linphone-dtls-default-identity, add CN= before it to make a valid name */
	memcpy(formatted_subject, "CN=", 3);
	memcpy(formatted_subject+3, subject, strlen(subject)+1); /* +1 to get the \0 termination */

	mbedtls_entropy_init( &entropy );
	mbedtls_ctr_drbg_init(&ctr_drbg);
	if( ( ret = mbedtls_ctr_drbg_seed( &ctr_drbg, mbedtls_entropy_func, &entropy, NULL, 0 ) )   != 0 )
	{
		bctoolbox_error("Certificate generation can't init ctr_drbg: [-0x%x]", -ret);
		return BCTOOLBOX_ERROR_CERTIFICATE_GENERATION_FAIL;
	}

	/* generate 3072 bits RSA public/private key */
	if ( (ret = mbedtls_pk_setup( (mbedtls_pk_context *)pkey, mbedtls_pk_info_from_type( MBEDTLS_PK_RSA ) )) != 0) {
		bctoolbox_error("Certificate generation can't init pk_ctx: [-0x%x]", -ret);
		return BCTOOLBOX_ERROR_CERTIFICATE_GENERATION_FAIL;
	}

	if ( ( ret = mbedtls_rsa_gen_key( mbedtls_pk_rsa( *(mbedtls_pk_context *)pkey ), mbedtls_ctr_drbg_random, &ctr_drbg, 3072, 65537 ) ) != 0) {
		bctoolbox_error("Certificate generation can't generate rsa key: [-0x%x]", -ret);
		return BCTOOLBOX_ERROR_CERTIFICATE_GENERATION_FAIL;
	}

	/* if there is no pem pointer, don't save the key in pem format */
	if (pem!=NULL) {
		mbedtls_pk_write_key_pem((mbedtls_pk_context *)pkey, (unsigned char *)file_buffer, 4096);
		file_buffer_len = strlen(file_buffer);
	}

	/* generate the certificate */
	mbedtls_x509write_crt_init( &crt );
	mbedtls_x509write_crt_set_md_alg( &crt, MBEDTLS_MD_SHA256 );

	mbedtls_mpi_init( &serial );

	if ( (ret = mbedtls_mpi_read_string( &serial, 10, "1" ) ) != 0 ) {
		bctoolbox_error("Certificate generation can't read serial mpi: [-0x%x]", -ret);
		return BCTOOLBOX_ERROR_CERTIFICATE_GENERATION_FAIL;
	}

	mbedtls_x509write_crt_set_subject_key( &crt, (mbedtls_pk_context *)pkey);
	mbedtls_x509write_crt_set_issuer_key( &crt, (mbedtls_pk_context *)pkey);

	if ( (ret = mbedtls_x509write_crt_set_subject_name( &crt, formatted_subject) ) != 0) {
		bctoolbox_error("Certificate generation can't set subject name: [-0x%x]", -ret);
		return BCTOOLBOX_ERROR_CERTIFICATE_GENERATION_FAIL;
	}

	if ( (ret = mbedtls_x509write_crt_set_issuer_name( &crt, formatted_subject) ) != 0) {
		bctoolbox_error("Certificate generation can't set issuer name: -%x", -ret);
		return BCTOOLBOX_ERROR_CERTIFICATE_GENERATION_FAIL;
	}

	if ( (ret = mbedtls_x509write_crt_set_serial( &crt, &serial ) ) != 0) {
		bctoolbox_error("Certificate generation can't set serial: -%x", -ret);
		return BCTOOLBOX_ERROR_CERTIFICATE_GENERATION_FAIL;
	}
	mbedtls_mpi_free(&serial);

	if ( (ret = mbedtls_x509write_crt_set_validity( &crt, "20010101000000", "20300101000000" ) ) != 0) {
		bctoolbox_error("Certificate generation can't set validity: -%x", -ret);
		return BCTOOLBOX_ERROR_CERTIFICATE_GENERATION_FAIL;
	}

	/* store anyway certificate in pem format in a string even if we do not have file to write as we need it to get it in a x509_crt structure */
	if ( (ret = mbedtls_x509write_crt_pem( &crt, (unsigned char *)file_buffer+file_buffer_len, 4096, mbedtls_ctr_drbg_random, &ctr_drbg ) ) != 0) {
		bctoolbox_error("Certificate generation can't write crt pem: -%x", -ret);
		return BCTOOLBOX_ERROR_CERTIFICATE_WRITE_PEM;
	}

	mbedtls_x509write_crt_free(&crt);
	mbedtls_ctr_drbg_free(&ctr_drbg);
	mbedtls_entropy_free(&entropy);

	/* copy the key+cert in pem format into the given buffer */
	if (pem != NULL) {
		if (strlen(file_buffer)+1>pem_length) {
			bctoolbox_error("Certificate generation can't copy the certificate to pem buffer: too short [%ld] but need [%ld] bytes", (long)pem_length, (long)strlen(file_buffer));
			return BCTOOLBOX_ERROR_OUTPUT_BUFFER_TOO_SMALL;
		}
		strncpy(pem, file_buffer, pem_length);
	}

	/* +1 on strlen as crt_parse in PEM format, length must include the final \0 */
	if ( (ret = mbedtls_x509_crt_parse((mbedtls_x509_crt *)certificate, (unsigned char *)file_buffer, strlen(file_buffer)+1) ) != 0) {
		bctoolbox_error("Certificate generation can't parse crt pem: -%x", -ret);
		return BCTOOLBOX_ERROR_CERTIFICATE_PARSE_PEM;
	}

	return 0;
}


int32_t bctoolbox_x509_certificate_get_signature_hash_function(const bctoolbox_x509_certificate_t *certificate, bctoolbox_md_type_t *hash_algorithm) {

	mbedtls_x509_crt *crt;
	if (certificate == NULL) return BCTOOLBOX_ERROR_INVALID_CERTIFICATE;

	crt = (mbedtls_x509_crt *)certificate;

	switch (crt->sig_md) {
		case MBEDTLS_MD_SHA1:
			*hash_algorithm = BCTOOLBOX_MD_SHA1;
		break;

		case MBEDTLS_MD_SHA224:
			*hash_algorithm = BCTOOLBOX_MD_SHA224;
		break;

		case MBEDTLS_MD_SHA256:
			*hash_algorithm = BCTOOLBOX_MD_SHA256;
		break;

		case MBEDTLS_MD_SHA384:
			*hash_algorithm = BCTOOLBOX_MD_SHA384;
		break;

		case MBEDTLS_MD_SHA512:
			*hash_algorithm = BCTOOLBOX_MD_SHA512;
		break;

		default:
			*hash_algorithm = BCTOOLBOX_MD_UNDEFINED;
			return BCTOOLBOX_ERROR_UNSUPPORTED_HASH_FUNCTION;
		break;
	}

	return 0;

}

/* maximum length of returned buffer will be 7(SHA-512 string)+3*hash_length(64)+null char = 200 bytes */
int32_t bctoolbox_x509_certificate_get_fingerprint(const bctoolbox_x509_certificate_t *certificate, char *fingerprint, size_t fingerprint_length, bctoolbox_md_type_t hash_algorithm) {
	unsigned char buffer[64]={0}; /* buffer is max length of returned hash, which is 64 in case we use sha-512 */
	size_t hash_length = 0;
	const char *hash_alg_string=NULL;
	size_t fingerprint_size = 0;
	mbedtls_x509_crt *crt;
	mbedtls_md_type_t hash_id;
	if (certificate == NULL) return BCTOOLBOX_ERROR_INVALID_CERTIFICATE;

	crt = (mbedtls_x509_crt *)certificate;

	/* if there is a specified hash algorithm, use it*/
	switch (hash_algorithm) {
		case BCTOOLBOX_MD_SHA1:
			hash_id = MBEDTLS_MD_SHA1;
			break;
		case BCTOOLBOX_MD_SHA224:
			hash_id = MBEDTLS_MD_SHA224;
			break;
		case BCTOOLBOX_MD_SHA256:
			hash_id = MBEDTLS_MD_SHA256;
			break;
		case BCTOOLBOX_MD_SHA384:
			hash_id = MBEDTLS_MD_SHA384;
			break;
		case BCTOOLBOX_MD_SHA512:
			hash_id = MBEDTLS_MD_SHA512;
			break;
		default: /* nothing specified, use the hash algo used in the certificate signature */
			hash_id = crt->sig_md;
			break;
	}

	/* fingerprint is a hash of the DER formated certificate (found in crt->raw.p) using the same hash function used by certificate signature */
	switch (hash_id) {
		case MBEDTLS_MD_SHA1:
			mbedtls_sha1(crt->raw.p, crt->raw.len, buffer);
			hash_length = 20;
			hash_alg_string="SHA-1";
		break;

		case MBEDTLS_MD_SHA224:
			mbedtls_sha256(crt->raw.p, crt->raw.len, buffer, 1); /* last argument is a boolean, indicate to output sha-224 and not sha-256 */
			hash_length = 28;
			hash_alg_string="SHA-224";
		break;

		case MBEDTLS_MD_SHA256:
			mbedtls_sha256(crt->raw.p, crt->raw.len, buffer, 0);
			hash_length = 32;
			hash_alg_string="SHA-256";
		break;

		case MBEDTLS_MD_SHA384:
			mbedtls_sha512(crt->raw.p, crt->raw.len, buffer, 1); /* last argument is a boolean, indicate to output sha-384 and not sha-512 */
			hash_length = 48;
			hash_alg_string="SHA-384";
		break;

		case MBEDTLS_MD_SHA512:
			mbedtls_sha512(crt->raw.p, crt->raw.len, buffer, 1); /* last argument is a boolean, indicate to output sha-384 and not sha-512 */
			hash_length = 64;
			hash_alg_string="SHA-512";
		break;

		default:
			return BCTOOLBOX_ERROR_UNSUPPORTED_HASH_FUNCTION;
		break;
	}

	if (hash_length>0) {
		int i;
		int fingerprint_index = strlen(hash_alg_string);
		char prefix=' ';

		fingerprint_size=fingerprint_index+3*hash_length+1;
		/* fingerprint will be : hash_alg_string+' '+HEX : separated values: length is strlen(hash_alg_string)+3*hash_lenght + 1 for null termination */
		if (fingerprint_length<fingerprint_size) {
			return BCTOOLBOX_ERROR_OUTPUT_BUFFER_TOO_SMALL;
		}

		snprintf(fingerprint, fingerprint_size, "%s", hash_alg_string);
		for (i=0; i<hash_length; i++, fingerprint_index+=3) {
			snprintf((char*)fingerprint+fingerprint_index, fingerprint_size-fingerprint_index, "%c%02X", prefix,buffer[i]);
			prefix=':';
		}
		*(fingerprint+fingerprint_index) = '\0';
	}

	return (int32_t)fingerprint_size;
}

#define BCTOOLBOX_MAX_CERTIFICATE_FLAGS_STRING_LENGTH 256
int32_t bctoolbox_x509_certificate_flags_to_string(char *buffer, size_t buffer_size, uint32_t flags) {
	size_t i=0;
	char outputString[BCTOOLBOX_MAX_CERTIFICATE_FLAGS_STRING_LENGTH];

	if (flags & MBEDTLS_X509_BADCERT_EXPIRED)
		i+=snprintf(outputString+i, BCTOOLBOX_MAX_CERTIFICATE_FLAGS_STRING_LENGTH-i, "expired ");
	if (flags & MBEDTLS_X509_BADCERT_REVOKED)
		i+=snprintf(outputString+i, BCTOOLBOX_MAX_CERTIFICATE_FLAGS_STRING_LENGTH-i, "revoked ");
	if (flags & MBEDTLS_X509_BADCERT_CN_MISMATCH)
		i+=snprintf(outputString+i, BCTOOLBOX_MAX_CERTIFICATE_FLAGS_STRING_LENGTH-i, "CN-mismatch ");
	if (flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED)
		i+=snprintf(outputString+i, BCTOOLBOX_MAX_CERTIFICATE_FLAGS_STRING_LENGTH-i, "not-trusted ");
	if (flags & MBEDTLS_X509_BADCERT_MISSING)
		i+=snprintf(outputString+i, BCTOOLBOX_MAX_CERTIFICATE_FLAGS_STRING_LENGTH-i, "missing ");
	if (flags & MBEDTLS_X509_BADCERT_SKIP_VERIFY)
		i+=snprintf(outputString+i, BCTOOLBOX_MAX_CERTIFICATE_FLAGS_STRING_LENGTH-i, "skip-verify ");
	if (flags & MBEDTLS_X509_BADCERT_OTHER)
		i+=snprintf(outputString+i, BCTOOLBOX_MAX_CERTIFICATE_FLAGS_STRING_LENGTH-i, "other-reason ");
	if (flags & MBEDTLS_X509_BADCERT_FUTURE)
		i+=snprintf(outputString+i, BCTOOLBOX_MAX_CERTIFICATE_FLAGS_STRING_LENGTH-i, "future-validity ");
	if (flags & MBEDTLS_X509_BADCERT_KEY_USAGE)
		i+=snprintf(outputString+i, BCTOOLBOX_MAX_CERTIFICATE_FLAGS_STRING_LENGTH-i, "keyUsage-mismatch");
	if (flags & MBEDTLS_X509_BADCERT_EXT_KEY_USAGE)
		i+=snprintf(outputString+i, BCTOOLBOX_MAX_CERTIFICATE_FLAGS_STRING_LENGTH-i, "extendedKeyUsage-mismatch ");
	if (flags & MBEDTLS_X509_BADCERT_NS_CERT_TYPE)
		i+=snprintf(outputString+i, BCTOOLBOX_MAX_CERTIFICATE_FLAGS_STRING_LENGTH-i, "nsCertType-mismatch ");
	if (flags & MBEDTLS_X509_BADCERT_BAD_MD)
		i+=snprintf(outputString+i, BCTOOLBOX_MAX_CERTIFICATE_FLAGS_STRING_LENGTH-i, "unacceptable-hash ");
	if (flags & MBEDTLS_X509_BADCERT_BAD_PK)
		i+=snprintf(outputString+i, BCTOOLBOX_MAX_CERTIFICATE_FLAGS_STRING_LENGTH-i, "unacceptable-PK-alg ");
	if (flags & MBEDTLS_X509_BADCERT_BAD_KEY)
		i+=snprintf(outputString+i, BCTOOLBOX_MAX_CERTIFICATE_FLAGS_STRING_LENGTH-i, "unacceptable-key ");

	if (flags & MBEDTLS_X509_BADCRL_NOT_TRUSTED)
		i+=snprintf(outputString+i, BCTOOLBOX_MAX_CERTIFICATE_FLAGS_STRING_LENGTH-i, "crl-not-trusted ");
	if (flags & MBEDTLS_X509_BADCRL_EXPIRED)
		i+=snprintf(outputString+i, BCTOOLBOX_MAX_CERTIFICATE_FLAGS_STRING_LENGTH-i, "crl-expired ");

	if (flags & MBEDTLS_X509_BADCRL_FUTURE)
		i+=snprintf(outputString+i, BCTOOLBOX_MAX_CERTIFICATE_FLAGS_STRING_LENGTH-i, "crl-future ");
	if (flags & MBEDTLS_X509_BADCRL_BAD_MD)
		i+=snprintf(outputString+i, BCTOOLBOX_MAX_CERTIFICATE_FLAGS_STRING_LENGTH-i, "crl-unacceptable-hash ");
	if (flags & MBEDTLS_X509_BADCRL_BAD_PK)
		i+=snprintf(outputString+i, BCTOOLBOX_MAX_CERTIFICATE_FLAGS_STRING_LENGTH-i, "crl-unacceptable-PK-alg ");
	if (flags & MBEDTLS_X509_BADCRL_BAD_KEY)
		i+=snprintf(outputString+i, BCTOOLBOX_MAX_CERTIFICATE_FLAGS_STRING_LENGTH-i, "crl-unacceptable-key ");

	outputString[i] = '\0'; /* null terminate the string */

	if (i+1>buffer_size) {
		return BCTOOLBOX_ERROR_OUTPUT_BUFFER_TOO_SMALL;
	}

	strncpy(buffer, outputString, buffer_size);

	return 0;
}

int32_t bctoolbox_x509_certificate_set_flag(uint32_t *flags, uint32_t flags_to_set) {
	if (flags_to_set & BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_EXPIRED)
		*flags |= MBEDTLS_X509_BADCERT_EXPIRED;
	if (flags_to_set & BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_REVOKED)
		*flags |= MBEDTLS_X509_BADCERT_REVOKED;
	if (flags_to_set & BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_CN_MISMATCH)
		*flags |= MBEDTLS_X509_BADCERT_CN_MISMATCH;
	if (flags_to_set & BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_NOT_TRUSTED)
		*flags |= MBEDTLS_X509_BADCERT_NOT_TRUSTED;
	if (flags_to_set & BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_MISSING)
		*flags |= MBEDTLS_X509_BADCERT_MISSING;
	if (flags_to_set & BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_SKIP_VERIFY)
		*flags |= MBEDTLS_X509_BADCERT_SKIP_VERIFY;
	if (flags_to_set & BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_OTHER)
		*flags |= MBEDTLS_X509_BADCERT_OTHER;
	if (flags_to_set & BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_FUTURE)
		*flags |= MBEDTLS_X509_BADCERT_FUTURE;
	if (flags_to_set & BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_KEY_USAGE)
		*flags |= MBEDTLS_X509_BADCERT_KEY_USAGE;
	if (flags_to_set & BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_EXT_KEY_USAGE)
		*flags |= MBEDTLS_X509_BADCERT_EXT_KEY_USAGE;
	if (flags_to_set & BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_NS_CERT_TYPE)
		*flags |= MBEDTLS_X509_BADCERT_NS_CERT_TYPE;
	if (flags_to_set & BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_BAD_MD)
		*flags |= MBEDTLS_X509_BADCERT_BAD_MD;
	if (flags_to_set & BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_BAD_PK)
		*flags |= MBEDTLS_X509_BADCERT_BAD_PK;
	if (flags_to_set & BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_BAD_KEY)
		*flags |= MBEDTLS_X509_BADCERT_BAD_KEY;

	if (flags_to_set & BCTOOLBOX_CERTIFICATE_VERIFY_BADCRL_NOT_TRUSTED)
		*flags |= MBEDTLS_X509_BADCRL_NOT_TRUSTED;
	if (flags_to_set & BCTOOLBOX_CERTIFICATE_VERIFY_BADCRL_EXPIRED)
		*flags |= MBEDTLS_X509_BADCRL_EXPIRED;
	if (flags_to_set & BCTOOLBOX_CERTIFICATE_VERIFY_BADCRL_FUTURE)
		*flags |= MBEDTLS_X509_BADCRL_FUTURE;
	if (flags_to_set & BCTOOLBOX_CERTIFICATE_VERIFY_BADCRL_BAD_MD)
		*flags |= MBEDTLS_X509_BADCRL_BAD_MD;
	if (flags_to_set & BCTOOLBOX_CERTIFICATE_VERIFY_BADCRL_BAD_PK)
		*flags |= MBEDTLS_X509_BADCRL_BAD_PK;
	if (flags_to_set & BCTOOLBOX_CERTIFICATE_VERIFY_BADCRL_BAD_KEY)
		*flags |= MBEDTLS_X509_BADCRL_BAD_KEY;

	return 0;
}

uint32_t bctoolbox_x509_certificate_remap_flag(uint32_t flags) {
	uint32_t ret = 0;
	if (flags & MBEDTLS_X509_BADCERT_EXPIRED)
		ret |= BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_EXPIRED;
	if (flags & MBEDTLS_X509_BADCERT_REVOKED)
		ret |= BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_REVOKED;
	if (flags & MBEDTLS_X509_BADCERT_CN_MISMATCH)
		ret |= BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_CN_MISMATCH;
	if (flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED)
		ret |= BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_NOT_TRUSTED;
	if (flags & MBEDTLS_X509_BADCERT_MISSING)
		ret |= BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_MISSING;
	if (flags & MBEDTLS_X509_BADCERT_SKIP_VERIFY)
		ret |= BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_SKIP_VERIFY;
	if (flags & MBEDTLS_X509_BADCERT_FUTURE)
		ret |= BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_FUTURE;
	if (flags & MBEDTLS_X509_BADCERT_OTHER)
		ret |= BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_OTHER;
	if (flags & MBEDTLS_X509_BADCERT_KEY_USAGE)
		ret |= BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_KEY_USAGE;
	if (flags & MBEDTLS_X509_BADCERT_EXT_KEY_USAGE)
		ret |= BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_EXT_KEY_USAGE;
	if (flags & MBEDTLS_X509_BADCERT_NS_CERT_TYPE)
		ret |= BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_NS_CERT_TYPE;
	if (flags & MBEDTLS_X509_BADCERT_BAD_MD)
		ret |= BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_BAD_MD;
	if (flags & MBEDTLS_X509_BADCERT_BAD_PK)
		ret |= BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_BAD_PK;
	if (flags & MBEDTLS_X509_BADCERT_BAD_KEY)
		ret |= BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_BAD_KEY;

	if (flags & MBEDTLS_X509_BADCRL_NOT_TRUSTED)
		ret |= BCTOOLBOX_CERTIFICATE_VERIFY_BADCRL_NOT_TRUSTED;
	if (flags & MBEDTLS_X509_BADCRL_EXPIRED)
		ret |= BCTOOLBOX_CERTIFICATE_VERIFY_BADCRL_EXPIRED;
	if (flags & MBEDTLS_X509_BADCRL_FUTURE)
		ret |= BCTOOLBOX_CERTIFICATE_VERIFY_BADCRL_FUTURE;
	if (flags & MBEDTLS_X509_BADCRL_BAD_MD)
		ret |= BCTOOLBOX_CERTIFICATE_VERIFY_BADCRL_BAD_MD;
	if (flags & MBEDTLS_X509_BADCRL_BAD_PK)
		ret |= BCTOOLBOX_CERTIFICATE_VERIFY_BADCRL_BAD_PK;
	if (flags & MBEDTLS_X509_BADCRL_BAD_KEY)
		ret |= BCTOOLBOX_CERTIFICATE_VERIFY_BADCRL_BAD_KEY;

	return ret;
}

int32_t bctoolbox_x509_certificate_unset_flag(uint32_t *flags, uint32_t flags_to_unset) {
	if (flags_to_unset & BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_EXPIRED)
		*flags &= ~MBEDTLS_X509_BADCERT_EXPIRED;
	if (flags_to_unset & BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_REVOKED)
		*flags &= ~MBEDTLS_X509_BADCERT_REVOKED;
	if (flags_to_unset & BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_CN_MISMATCH)
		*flags &= ~MBEDTLS_X509_BADCERT_CN_MISMATCH;
	if (flags_to_unset & BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_NOT_TRUSTED)
		*flags &= ~MBEDTLS_X509_BADCERT_NOT_TRUSTED;
	if (flags_to_unset & BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_MISSING)
		*flags &= ~MBEDTLS_X509_BADCERT_MISSING;

	if (flags_to_unset & BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_SKIP_VERIFY)
		*flags &= ~MBEDTLS_X509_BADCERT_SKIP_VERIFY;
	if (flags_to_unset & BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_OTHER)
		*flags &= ~MBEDTLS_X509_BADCERT_OTHER;
	if (flags_to_unset & BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_FUTURE)
		*flags &= ~MBEDTLS_X509_BADCERT_FUTURE;
	if (flags_to_unset & BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_KEY_USAGE)
		*flags &= ~MBEDTLS_X509_BADCERT_KEY_USAGE;
	if (flags_to_unset & BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_EXT_KEY_USAGE)
		*flags &= ~MBEDTLS_X509_BADCERT_EXT_KEY_USAGE;
	if (flags_to_unset & BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_NS_CERT_TYPE)
		*flags &= ~MBEDTLS_X509_BADCERT_NS_CERT_TYPE;
	if (flags_to_unset & BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_BAD_MD)
		*flags &= ~MBEDTLS_X509_BADCERT_BAD_MD;
	if (flags_to_unset & BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_BAD_PK)
		*flags &= ~MBEDTLS_X509_BADCERT_BAD_PK;
	if (flags_to_unset & BCTOOLBOX_CERTIFICATE_VERIFY_BADCERT_BAD_KEY)
		*flags &= ~MBEDTLS_X509_BADCERT_BAD_KEY;


	if (flags_to_unset & BCTOOLBOX_CERTIFICATE_VERIFY_BADCRL_NOT_TRUSTED)
		*flags &= ~MBEDTLS_X509_BADCRL_NOT_TRUSTED;
	if (flags_to_unset & BCTOOLBOX_CERTIFICATE_VERIFY_BADCRL_EXPIRED)
		*flags &= ~MBEDTLS_X509_BADCRL_EXPIRED;

	if (flags_to_unset & BCTOOLBOX_CERTIFICATE_VERIFY_BADCRL_FUTURE)
		*flags &= ~MBEDTLS_X509_BADCRL_FUTURE;
	if (flags_to_unset & BCTOOLBOX_CERTIFICATE_VERIFY_BADCRL_BAD_MD)
		*flags &= ~MBEDTLS_X509_BADCRL_BAD_MD;
	if (flags_to_unset & BCTOOLBOX_CERTIFICATE_VERIFY_BADCRL_BAD_PK)
		*flags &= ~MBEDTLS_X509_BADCRL_BAD_PK;
	if (flags_to_unset & BCTOOLBOX_CERTIFICATE_VERIFY_BADCRL_BAD_KEY)
		*flags &= ~MBEDTLS_X509_BADCRL_BAD_KEY;

	return 0;
}

/*** SSL Client ***/
/** context **/
struct bctoolbox_ssl_context_struct {
	mbedtls_ssl_context ssl_ctx;
	int(*callback_cli_cert_function)(void *, bctoolbox_ssl_context_t *, unsigned char *, size_t); /**< pointer to the callback called to update client certificate during handshake
													callback params are user_data, ssl_context, certificate distinguished name, name length */
	void *callback_cli_cert_data; /**< data passed to the client cert callback */
	int(*callback_send_function)(void *, const unsigned char *, size_t); /* callbacks args are: callback data, data buffer to be send, size of data buffer */
	int(*callback_recv_function)(void *, unsigned char *, size_t); /* args: callback data, data buffer to be read, size of data buffer */
	void *callback_sendrecv_data; /**< data passed to send/recv callbacks */
};

bctoolbox_ssl_context_t *bctoolbox_ssl_context_new(void) {
	bctoolbox_ssl_context_t *ssl_ctx = bctoolbox_malloc0(sizeof(bctoolbox_ssl_context_t));
	mbedtls_ssl_init(&(ssl_ctx->ssl_ctx));
	ssl_ctx->callback_cli_cert_function = NULL;
	ssl_ctx->callback_cli_cert_data = NULL;
	ssl_ctx->callback_send_function = NULL;
	ssl_ctx->callback_recv_function = NULL;
	ssl_ctx->callback_sendrecv_data = NULL;
	return ssl_ctx;
}

void bctoolbox_ssl_context_free(bctoolbox_ssl_context_t *ssl_ctx) {
	mbedtls_ssl_free(&(ssl_ctx->ssl_ctx));
	bctoolbox_free(ssl_ctx);
}

int32_t bctoolbox_ssl_close_notify(bctoolbox_ssl_context_t *ssl_ctx) {
	return mbedtls_ssl_close_notify(&(ssl_ctx->ssl_ctx));
}

int32_t bctoolbox_ssl_session_reset(bctoolbox_ssl_context_t *ssl_ctx) {
	return mbedtls_ssl_session_reset(&(ssl_ctx->ssl_ctx));
}

int32_t bctoolbox_ssl_write(bctoolbox_ssl_context_t *ssl_ctx, const unsigned char *buf, size_t buf_length) {
	int ret = mbedtls_ssl_write(&(ssl_ctx->ssl_ctx), buf, buf_length);
	/* remap some output code */
	if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
		ret = BCTOOLBOX_ERROR_NET_WANT_WRITE;
	}
	return ret;
}

int32_t bctoolbox_ssl_read(bctoolbox_ssl_context_t *ssl_ctx, unsigned char *buf, size_t buf_length) {
	int ret = mbedtls_ssl_read(&(ssl_ctx->ssl_ctx), buf, buf_length);
	/* remap some output code */
	if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
		ret = BCTOOLBOX_ERROR_SSL_PEER_CLOSE_NOTIFY;
	}
	if (ret == MBEDTLS_ERR_SSL_WANT_READ) {
		ret = BCTOOLBOX_ERROR_NET_WANT_READ;
	}
	return ret;
}

int32_t bctoolbox_ssl_handshake(bctoolbox_ssl_context_t *ssl_ctx) {

	int ret = 0;
	while( ssl_ctx->ssl_ctx.state != MBEDTLS_SSL_HANDSHAKE_OVER )
	{
		ret = mbedtls_ssl_handshake_step(&(ssl_ctx->ssl_ctx));
		if( ret != 0 ) {
			break;
		}

		/* insert the callback function for client certificate request */
		if (ssl_ctx->callback_cli_cert_function != NULL) { /* check we have a callback function */
			/* when in state SSL_CLIENT_CERTIFICATE - which means, next call to ssl_handshake_step will send the client certificate to server -
			 * and the client_auth flag is set - which means the server requested a client certificate - */
			if (ssl_ctx->ssl_ctx.state == MBEDTLS_SSL_CLIENT_CERTIFICATE && ssl_ctx->ssl_ctx.client_auth > 0) {
				/* note: polarssl 1.3 is unable to retrieve certificate dn during handshake from server certificate request
				 * so the dn params in the callback are set to NULL and 0(dn string length) */
				if (ssl_ctx->callback_cli_cert_function(ssl_ctx->callback_cli_cert_data, ssl_ctx, NULL, 0)!=0) {
					if((ret=mbedtls_ssl_send_alert_message(&(ssl_ctx->ssl_ctx), MBEDTLS_SSL_ALERT_LEVEL_FATAL, MBEDTLS_SSL_ALERT_MSG_HANDSHAKE_FAILURE)) != 0 )
						return( ret );
				}
			}
		}

	}

	/* remap some output codes */
	if (ret == MBEDTLS_ERR_SSL_WANT_READ) {
		ret = BCTOOLBOX_ERROR_NET_WANT_READ;
	} else if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
		ret = BCTOOLBOX_ERROR_NET_WANT_WRITE;
	}

	return(ret);
}

int32_t bctoolbox_ssl_set_hs_own_cert(bctoolbox_ssl_context_t *ssl_ctx, bctoolbox_x509_certificate_t *cert, bctoolbox_signing_key_t *key) {
	/* WARNING Dirty-Dirty trick : in mbedtls ssl_set_hs_own_cert shall be caller on server side only for SNI, there is no real equivalent on client side yet */
	/* If we are server call the regular function */
	if (ssl_ctx->ssl_ctx.conf->endpoint == MBEDTLS_SSL_IS_SERVER ) {
		return mbedtls_ssl_set_hs_own_cert(&(ssl_ctx->ssl_ctx) , (mbedtls_x509_crt *)cert , (mbedtls_pk_context *)key);
	} else { /* if we are client, call the conf function on the function in use inside the ssl_context, dirty but it works for now */
		return mbedtls_ssl_conf_own_cert((mbedtls_ssl_config *)ssl_ctx->ssl_ctx.conf , (mbedtls_x509_crt *)cert , (mbedtls_pk_context *)key);
	}
}

int bctoolbox_ssl_send_callback(void *data, const unsigned char *buffer, size_t buffer_length) {
	int ret = 0;
	/* data is the ssl_context which contains the actual callback and data */
	bctoolbox_ssl_context_t *ssl_ctx = (bctoolbox_ssl_context_t *)data;

	ret = ssl_ctx->callback_send_function(ssl_ctx->callback_sendrecv_data, buffer, buffer_length);

	return bctoolbox_ssl_sendrecv_callback_return_remap(ret);
}

int bctoolbox_ssl_recv_callback(void *data, unsigned char *buffer, size_t buffer_length) {
	int ret = 0;
	/* data is the ssl_context which contains the actual callback and data */
	bctoolbox_ssl_context_t *ssl_ctx = (bctoolbox_ssl_context_t *)data;

	ret = ssl_ctx->callback_recv_function(ssl_ctx->callback_sendrecv_data, buffer, buffer_length);

	return bctoolbox_ssl_sendrecv_callback_return_remap(ret);
}

void bctoolbox_ssl_set_io_callbacks(bctoolbox_ssl_context_t *ssl_ctx, void *callback_data,
		int(*callback_send_function)(void *, const unsigned char *, size_t), /* callbacks args are: callback data, data buffer to be send, size of data buffer */
		int(*callback_recv_function)(void *, unsigned char *, size_t)){ /* args: callback data, data buffer to be read, size of data buffer */

	if (ssl_ctx==NULL) {
		return;
	}

	ssl_ctx->callback_send_function = callback_send_function;
	ssl_ctx->callback_recv_function = callback_recv_function;
	ssl_ctx->callback_sendrecv_data = callback_data;

	mbedtls_ssl_set_bio(&(ssl_ctx->ssl_ctx), ssl_ctx, bctoolbox_ssl_send_callback, bctoolbox_ssl_recv_callback, NULL);
}

const bctoolbox_x509_certificate_t *bctoolbox_ssl_get_peer_certificate(bctoolbox_ssl_context_t *ssl_ctx) {
	return (const bctoolbox_x509_certificate_t *)mbedtls_ssl_get_peer_cert(&(ssl_ctx->ssl_ctx));
}

/** DTLS SRTP functions **/
#ifdef HAVE_DTLS_SRTP
uint8_t bctoolbox_dtls_srtp_supported(void) {
	return 1;
}

static bctoolbox_dtls_srtp_profile_t bctoolbox_srtp_profile_polarssl2bctoolbox(enum DTLS_SRTP_protection_profiles polarssl_profile) {
	switch (polarssl_profile) {
		case SRTP_AES128_CM_HMAC_SHA1_80:
			return BCTOOLBOX_SRTP_AES128_CM_HMAC_SHA1_80;
		case SRTP_AES128_CM_HMAC_SHA1_32:
			return BCTOOLBOX_SRTP_AES128_CM_HMAC_SHA1_32;
		case SRTP_NULL_HMAC_SHA1_80:
			return BCTOOLBOX_SRTP_NULL_HMAC_SHA1_80;
		case SRTP_NULL_HMAC_SHA1_32:
			return BCTOOLBOX_SRTP_NULL_HMAC_SHA1_32;
		default:
			return BCTOOLBOX_SRTP_UNDEFINED;
	}
}

static enum DTLS_SRTP_protection_profiles bctoolbox_srtp_profile_bctoolbox2polarssl(bctoolbox_dtls_srtp_profile_t bctoolbox_profile) {
	switch (bctoolbox_profile) {
		case BCTOOLBOX_SRTP_AES128_CM_HMAC_SHA1_80:
			return SRTP_AES128_CM_HMAC_SHA1_80;
		case BCTOOLBOX_SRTP_AES128_CM_HMAC_SHA1_32:
			return SRTP_AES128_CM_HMAC_SHA1_32;
		case BCTOOLBOX_SRTP_NULL_HMAC_SHA1_80:
			return SRTP_NULL_HMAC_SHA1_80;
		case BCTOOLBOX_SRTP_NULL_HMAC_SHA1_32:
			return SRTP_NULL_HMAC_SHA1_32;
		default:
			return SRTP_UNSET_PROFILE;
	}
}

bctoolbox_dtls_srtp_profile_t bctoolbox_ssl_get_dtls_srtp_protection_profile(bctoolbox_ssl_context_t *ssl_ctx) {
	if (ssl_ctx==NULL) {
		return BCTOOLBOX_ERROR_INVALID_SSL_CONTEXT;
	}

	return bctoolbox_srtp_profile_polarssl2bctoolbox(ssl_get_dtls_srtp_protection_profile(&(ssl_ctx->ssl_ctx)));
};


int32_t bctoolbox_ssl_get_dtls_srtp_key_material(bctoolbox_ssl_context_t *ssl_ctx, char *output, size_t *output_length) {
	if (ssl_ctx==NULL) {
		return BCTOOLBOX_ERROR_INVALID_SSL_CONTEXT;
	}

	/* check output buffer size */
	if (*output_length<ssl_ctx->ssl_ctx.dtls_srtp_keys_len) {
		return BCTOOLBOX_ERROR_OUTPUT_BUFFER_TOO_SMALL;
	}

	memcpy(output, ssl_ctx->ssl_ctx.dtls_srtp_keys, ssl_ctx->ssl_ctx.dtls_srtp_keys_len);
	*output_length = ssl_ctx->ssl_ctx.dtls_srtp_keys_len;

	return 0;
}
#else /* HAVE_DTLS_SRTP */
/* dummy DTLS api when not available */
uint8_t bctoolbox_dtls_srtp_supported(void) {
	return 0;
}

bctoolbox_dtls_srtp_profile_t bctoolbox_ssl_get_dtls_srtp_protection_profile(bctoolbox_ssl_context_t *ssl_ctx) {
	return BCTOOLBOX_SRTP_UNDEFINED;
}

int32_t bctoolbox_ssl_get_dtls_srtp_key_material(bctoolbox_ssl_context_t *ssl_ctx, char *output, size_t *output_length) {
	*output_length = 0;
	return BCTOOLBOX_ERROR_UNAVAILABLE_FUNCTION;
}
#endif /* HAVE_DTLS_SRTP */

/** DTLS SRTP functions **/

/** config **/
struct bctoolbox_ssl_config_struct {
	mbedtls_ssl_config *ssl_config; /**< actual config structure */
	uint8_t ssl_config_externally_provided; /**< a flag, on when the ssl_config was provided by callers and not created threw the new function */
	int(*callback_cli_cert_function)(void *, bctoolbox_ssl_context_t *, unsigned char *, size_t); /**< pointer to the callback called to update client certificate during handshake
													callback params are user_data, ssl_context, certificate distinguished name, name length */
	void *callback_cli_cert_data; /**< data passed to the client cert callback */
};

bctoolbox_ssl_config_t *bctoolbox_ssl_config_new(void) {
	bctoolbox_ssl_config_t *ssl_config = bctoolbox_malloc0(sizeof(bctoolbox_ssl_config_t));
	/* allocate and init anyway a ssl_config, it may be then crashed by an externally provided one */
	ssl_config->ssl_config = bctoolbox_malloc0(sizeof(mbedtls_ssl_config));
	ssl_config->ssl_config_externally_provided = 0;
	mbedtls_ssl_config_init(ssl_config->ssl_config);

	ssl_config->callback_cli_cert_function = NULL;
	ssl_config->callback_cli_cert_data = NULL;

	return ssl_config;
}

int32_t bctoolbox_ssl_config_set_crypto_library_config(bctoolbox_ssl_config_t *ssl_config, void *internal_config) {
	if (ssl_config == NULL) {
		return BCTOOLBOX_ERROR_INVALID_SSL_CONFIG;
	}

	/* free already existing structure */
	if (ssl_config->ssl_config != NULL && ssl_config->ssl_config_externally_provided==0) {
		mbedtls_ssl_config_free(ssl_config->ssl_config);
		bctoolbox_free(ssl_config->ssl_config);
	}

	/* set the given pointer as the ssl_config context */
	ssl_config->ssl_config = (mbedtls_ssl_config *)internal_config;

	/* set the flag in order to not free the mbedtls config when freing bctoolbox_ssl_config */
	ssl_config->ssl_config_externally_provided = 1;

	return 0;
}

void bctoolbox_ssl_config_free(bctoolbox_ssl_config_t *ssl_config) {
	if (ssl_config == NULL) {
		return;
	}

	/* free mbedtls_ssl_config only when created internally */
	if (ssl_config->ssl_config_externally_provided==0) {
		mbedtls_ssl_config_free(ssl_config->ssl_config);
		bctoolbox_free(ssl_config->ssl_config);
	}

	bctoolbox_free(ssl_config);
}

int32_t bctoolbox_ssl_config_defaults(bctoolbox_ssl_config_t *ssl_config, int endpoint, int transport) {
	int mbedtls_endpoint, mbedtls_transport;

	if (ssl_config == NULL) {
		return BCTOOLBOX_ERROR_INVALID_SSL_CONFIG;
	}

	/* remap input arguments */
	switch (endpoint) {
		case BCTOOLBOX_SSL_IS_CLIENT:
			mbedtls_endpoint = MBEDTLS_SSL_IS_CLIENT;
			break;
		case BCTOOLBOX_SSL_IS_SERVER:
			mbedtls_endpoint = MBEDTLS_SSL_IS_SERVER;
			break;
		default:
			return BCTOOLBOX_ERROR_INVALID_INPUT_DATA;
	}

	switch (transport) {
		case BCTOOLBOX_SSL_TRANSPORT_STREAM:
			mbedtls_transport = MBEDTLS_SSL_TRANSPORT_STREAM;
			break;
		case BCTOOLBOX_SSL_TRANSPORT_DATAGRAM:
			mbedtls_transport = MBEDTLS_SSL_TRANSPORT_DATAGRAM;
			break;
		default:
			return BCTOOLBOX_ERROR_INVALID_INPUT_DATA;
	}

	return mbedtls_ssl_config_defaults(ssl_config->ssl_config, mbedtls_endpoint, mbedtls_transport, MBEDTLS_SSL_PRESET_DEFAULT);

	return 0;
}

int32_t bctoolbox_ssl_config_set_endpoint(bctoolbox_ssl_config_t *ssl_config, int endpoint) {
	int mbedtls_endpoint;
	if (ssl_config == NULL) {
		return BCTOOLBOX_ERROR_INVALID_SSL_CONFIG;
	}

	/* remap input arguments */
	switch (endpoint) {
		case BCTOOLBOX_SSL_IS_CLIENT:
			mbedtls_endpoint = MBEDTLS_SSL_IS_CLIENT;
			break;
		case BCTOOLBOX_SSL_IS_SERVER:
			mbedtls_endpoint = MBEDTLS_SSL_IS_SERVER;
			break;
		default:
			return BCTOOLBOX_ERROR_INVALID_INPUT_DATA;
	}

	mbedtls_ssl_conf_endpoint(ssl_config->ssl_config, mbedtls_endpoint);

	return 0;
}

int32_t bctoolbox_ssl_config_set_transport (bctoolbox_ssl_config_t *ssl_config, int transport) {
	int mbedtls_transport;
	if (ssl_config == NULL) {
		return BCTOOLBOX_ERROR_INVALID_SSL_CONFIG;
	}

	/* remap input arguments */
	switch (transport) {
		case BCTOOLBOX_SSL_TRANSPORT_STREAM:
			mbedtls_transport = MBEDTLS_SSL_TRANSPORT_STREAM;
			break;
		case BCTOOLBOX_SSL_TRANSPORT_DATAGRAM:
			mbedtls_transport = MBEDTLS_SSL_TRANSPORT_DATAGRAM;
			break;
		default:
			return BCTOOLBOX_ERROR_INVALID_INPUT_DATA;
	}

	mbedtls_ssl_conf_transport(ssl_config->ssl_config, mbedtls_transport);

	return 0;
}

int32_t bctoolbox_ssl_config_set_authmode(bctoolbox_ssl_config_t *ssl_config, int authmode) {
	int mbedtls_authmode;
	if (ssl_config == NULL) {
		return BCTOOLBOX_ERROR_INVALID_SSL_CONFIG;
	}

	/* remap input arguments */
	switch (authmode) {
		case BCTOOLBOX_SSL_VERIFY_NONE:
			mbedtls_authmode = MBEDTLS_SSL_VERIFY_NONE;
			break;
		case BCTOOLBOX_SSL_VERIFY_OPTIONAL:
			mbedtls_authmode = MBEDTLS_SSL_VERIFY_OPTIONAL;
			break;
		case BCTOOLBOX_SSL_VERIFY_REQUIRED:
			mbedtls_authmode = MBEDTLS_SSL_VERIFY_REQUIRED;
			break;
		default:
			return BCTOOLBOX_ERROR_INVALID_SSL_AUTHMODE;
			break;
	}

	mbedtls_ssl_conf_authmode(ssl_config->ssl_config, mbedtls_authmode);

	return 0;
}

int32_t bctoolbox_ssl_config_set_rng(bctoolbox_ssl_config_t *ssl_config, int(*rng_function)(void *, unsigned char *, size_t), void *rng_context) {
	if (ssl_config == NULL) {
		return BCTOOLBOX_ERROR_INVALID_SSL_CONFIG;
	}

	mbedtls_ssl_conf_rng(ssl_config->ssl_config, rng_function, rng_context);

	return 0;
}

int32_t bctoolbox_ssl_config_set_callback_verify(bctoolbox_ssl_config_t *ssl_config, int(*callback_function)(void *, bctoolbox_x509_certificate_t *, int, uint32_t *), void *callback_data) {
	if (ssl_config == NULL) {
		return BCTOOLBOX_ERROR_INVALID_SSL_CONFIG;
	}

	mbedtls_ssl_conf_verify(ssl_config->ssl_config, (int(*)(void *, mbedtls_x509_crt*, int, uint32_t *))callback_function, callback_data);

	return 0;
}

int32_t bctoolbox_ssl_config_set_callback_cli_cert(bctoolbox_ssl_config_t *ssl_config, int(*callback_function)(void *, bctoolbox_ssl_context_t *, unsigned char *, size_t), void *callback_data) {
	if (ssl_config == NULL) {
		return BCTOOLBOX_ERROR_INVALID_SSL_CONFIG;
	}
	ssl_config->callback_cli_cert_function = callback_function;
	ssl_config->callback_cli_cert_data = callback_data;

	return 0;
}

int32_t bctoolbox_ssl_config_set_ca_chain(bctoolbox_ssl_config_t *ssl_config, bctoolbox_x509_certificate_t *ca_chain, char *peer_cn) {
	if (ssl_config == NULL) {
		return BCTOOLBOX_ERROR_INVALID_SSL_CONFIG;
	}
	/* ca_crl (arg 3) is always set to null, add the functionnality if needed */
	mbedtls_ssl_conf_ca_chain(ssl_config->ssl_config, (mbedtls_x509_crt *)ca_chain, NULL);

	return 0;
}

int32_t bctoolbox_ssl_config_set_own_cert(bctoolbox_ssl_config_t *ssl_config, bctoolbox_x509_certificate_t *cert, bctoolbox_signing_key_t *key) {
	if (ssl_config == NULL) {
		return BCTOOLBOX_ERROR_INVALID_SSL_CONFIG;
	}
	return mbedtls_ssl_conf_own_cert(ssl_config->ssl_config, (mbedtls_x509_crt *)cert, (mbedtls_pk_context *)key);
}


/** DTLS SRTP functions **/
#ifdef HAVE_DTLS_SRTP
int32_t bctoolbox_ssl_config_set_dtls_srtp_protection_profiles(bctoolbox_ssl_config_t *ssl_config, const bctoolbox_dtls_srtp_profile_t *profiles, size_t profiles_number) {
	int i;

	if (ssl_config == NULL) {
		return BCTOOLBOX_ERROR_INVALID_SSL_CONFIG;
	}

	/* convert the profiles array into a polarssl profiles array */
	for (i=0; i<profiles_number && i<4; i++) { /* 4 profiles defined max */
		ssl_config->dtls_srtp_profiles[i] = bctoolbox_srtp_profile_bctoolbox2polarssl(profiles[i]);
	}
	for (;i<4; i++) { /* make sure to have harmless values in the rest of the array */
		ssl_config->dtls_srtp_profiles[i] = SRTP_UNSET_PROFILE;
	}

	ssl_config->dtls_srtp_profiles_number = profiles_number;

	return 0;
}

#else /* HAVE_DTLS_SRTP */
int32_t bctoolbox_ssl_config_set_dtls_srtp_protection_profiles(bctoolbox_ssl_config_t *ssl_config, const bctoolbox_dtls_srtp_profile_t *profiles, size_t profiles_number) {
	return BCTOOLBOX_ERROR_UNAVAILABLE_FUNCTION;
}
#endif /* HAVE_DTLS_SRTP */
/** DTLS SRTP functions **/

int32_t bctoolbox_ssl_context_setup(bctoolbox_ssl_context_t *ssl_ctx, bctoolbox_ssl_config_t *ssl_config) {
	/* Check validity of context and config */
	if (ssl_config == NULL) {
		return BCTOOLBOX_ERROR_INVALID_SSL_CONFIG;
	}

	if (ssl_ctx == NULL) {
		return BCTOOLBOX_ERROR_INVALID_SSL_CONTEXT;
	}

	/* apply all valids settings to the ssl_context */
	if (ssl_config->callback_cli_cert_function != NULL) {
		ssl_ctx->callback_cli_cert_function = ssl_config->callback_cli_cert_function;
		ssl_ctx->callback_cli_cert_data = ssl_config->callback_cli_cert_data;
	}

#ifdef HAVE_DTLS_SRTP
	if (ssl_config->dtls_srtp_profiles_number > 0) {
		ssl_set_dtls_srtp_protection_profiles(&(ssl_ctx->ssl_ctx), ssl_config->dtls_srtp_profiles, ssl_config->dtls_srtp_profiles_number );
	}

	/* We do not use DTLS SRTP cookie, so we must set to NULL the callbacks. Cookies are used to prevent DoS attack but our server is on only when during a brief period so we do not need this */
	mbedtls_ssl_conf_dtls_cookies(ssl_config->ssl_config, NULL, NULL, NULL);
#endif /* HAVE_DTLS_SRTP */

	return mbedtls_ssl_setup(&(ssl_ctx->ssl_ctx), ssl_config->ssl_config);
}

/*****************************************************************************/
/***** Hashing                                                           *****/
/*****************************************************************************/

/*
 * HMAC-SHA-256 wrapper
 * @param[in] 	key			HMAC secret key
 * @param[in] 	keyLength	HMAC key length
 * @param[in]	input 		Input data buffer
 * @param[in]   inputLength	Input data length
 * @param[in]	hmacLength	Length of output required in bytes, HMAC output is truncated to the hmacLength left bytes. 32 bytes maximum
 * @param[out]	output		Output data buffer.
 *
 */
void bctoolbox_hmacSha256(const uint8_t *key,
		size_t keyLength,
		const uint8_t *input,
		size_t inputLength,
		uint8_t hmacLength,
		uint8_t *output)
{
	uint8_t hmacOutput[32];
	mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, keyLength, input, inputLength, hmacOutput);

	/* check output length, can't be>32 */
	if (hmacLength>32) {
		memcpy(output, hmacOutput, 32);
	} else {
		memcpy(output, hmacOutput, hmacLength);
	}
}

/*
 * @brief SHA256 wrapper
 * @param[in]	input 		Input data buffer
 * @param[in]   inputLength	Input data length in bytes
 * @param[in]	hmacLength	Length of output required in bytes, HMAC output is truncated to the hmacLength left bytes. 32 bytes maximum
 * @param[out]	output		Output data buffer.
 *
 */
void bctoolbox_sha256(const uint8_t *input,
		size_t inputLength,
		uint8_t hashLength,
		uint8_t *output)
{
	uint8_t hashOutput[32];
	mbedtls_sha256(input, inputLength, hashOutput, 0); /* last param to zero to select SHA256 and not SHA224 */

	/* check output length, can't be>32 */
	if (hashLength>32) {
		memcpy(output, hashOutput, 32);
	} else {
		memcpy(output, hashOutput, hashLength);
	}
}

/*
 * @brief HMAC-SHA1 wrapper
 * @param[in] 	key			HMAC secret key
 * @param[in] 	keyLength	HMAC key length
 * @param[in]	input 		Input data buffer
 * @param[in]   inputLength	Input data length
 * @param[in]	hmacLength	Length of output required in bytes, HMAC output is truncated to the hmacLength left bytes. 20 bytes maximum
 * @param[out]	output		Output data buffer.
 *
 */
void bctoolbox_hmacSha1(const uint8_t *key,
		size_t keyLength,
		const uint8_t *input,
		size_t inputLength,
		uint8_t hmacLength,
		uint8_t *output)
{
	uint8_t hmacOutput[20];

	mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), key, keyLength, input, inputLength, hmacOutput);

	/* check output length, can't be>20 */
	if (hmacLength>20) {
		memcpy(output, hmacOutput, 20);
	} else {
		memcpy(output, hmacOutput, hmacLength);
	}
}

/**
 * @brief MD5 wrapper
 * output = md5(input)
 * @param[in]	input 		Input data buffer
 * @param[in]   inputLength	Input data length in bytes
 * @param[out]	output		Output data buffer.
 *
 */
void bctoolbox_md5(const uint8_t *input,
		size_t inputLength,
		uint8_t output[16])
{
	mbedtls_md5(input, inputLength, output);
}
