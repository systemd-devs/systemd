/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "fd-util.h"
#include "openssl-util.h"
#include "alloc-util.h"
#include "hexdecoct.h"

#if HAVE_OPENSSL
int openssl_pkey_from_pem(const void *pem, size_t pem_size, EVP_PKEY **ret) {
        assert(pem);
        assert(ret);

        _cleanup_fclose_ FILE *f = NULL;
        f = fmemopen((void*) pem, pem_size, "r");
        if (!f)
                return log_oom_debug();

        _cleanup_(EVP_PKEY_freep) EVP_PKEY *pkey = PEM_read_PUBKEY(f, NULL, NULL, NULL);
        if (!pkey)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to parse PEM.");

        *ret = TAKE_PTR(pkey);

        return 0;
}

int openssl_digest_size(const char *digest_alg, size_t *ret_digest_size) {
        size_t digest_size;

        assert(digest_alg);
        assert(ret_digest_size);

#if OPENSSL_VERSION_MAJOR >= 3
        _cleanup_(EVP_MD_freep) EVP_MD *md = EVP_MD_fetch(NULL, digest_alg, NULL);
#else
        const EVP_MD *md = EVP_get_digestbyname(digest_alg);
#endif
        if (!md)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to get EVP_MD for '%s'.", digest_alg);

#if OPENSSL_VERSION_MAJOR >= 3
        digest_size = EVP_MD_get_size(md);
#else
        digest_size = EVP_MD_size(md);
#endif
        if (digest_size == 0)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to get Digest size.");

        *ret_digest_size = digest_size;

        return 0;
}

int openssl_digest_many(
                const char *digest_alg,
                const struct iovec data[],
                size_t n_data,
                void **ret_digest,
                size_t *ret_digest_size) {

        assert(digest_alg);
        assert(data || n_data == 0);
        assert(ret_digest);
        /* ret_digest_size is optional, as caller may already know the digest size */

#if OPENSSL_VERSION_MAJOR >= 3
        _cleanup_(EVP_MD_freep) EVP_MD *md = EVP_MD_fetch(NULL, digest_alg, NULL);
        if (!md)
                /* This probably means digest_alg is not supported, but might be OOM. */
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to fetch EVP_MD for '%s'.", digest_alg);
#else
        const EVP_MD *md = EVP_get_digestbyname(digest_alg);
        if (!md)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to get EVP_MD for '%s'.", digest_alg);
#endif

        _cleanup_(EVP_MD_CTX_freep) EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        if (!ctx)
                return log_oom_debug();

        if (!EVP_DigestInit_ex(ctx, md, NULL))
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to initializate EVP_MD_CTX.");

        for (size_t i = 0; i < n_data; i++)
                if (!EVP_DigestUpdate(ctx, data[i].iov_base, data[i].iov_len))
                        return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to update Digest.");

        size_t digest_size;
#if OPENSSL_VERSION_MAJOR >= 3
        digest_size = EVP_MD_CTX_get_size(ctx);
#else
        digest_size = EVP_MD_CTX_size(ctx);
#endif
        if (digest_size == 0)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to get Digest size.");

        _cleanup_free_ void *buf = malloc(digest_size);
        if (!buf)
                return log_oom_debug();

        unsigned int size;
        if (!EVP_DigestFinal_ex(ctx, buf, &size))
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to finalize Digest.");

        assert(size == digest_size);

        *ret_digest = TAKE_PTR(buf);
        if (ret_digest_size)
                *ret_digest_size = size;

        return 0;
}

int openssl_hmac_many(
                const char *digest_alg,
                const void *key,
                size_t key_size,
                const struct iovec data[],
                size_t n_data,
                void **ret_digest,
                size_t *ret_digest_size) {

#if OPENSSL_VERSION_MAJOR >= 3
        _cleanup_(EVP_MAC_freep) EVP_MAC *mac = EVP_MAC_fetch(NULL, "HMAC", NULL);
        if (!mac)
                return log_oom_debug();

        _cleanup_(EVP_MAC_CTX_freep) EVP_MAC_CTX *ctx = EVP_MAC_CTX_new(mac);
        if (!ctx)
                return log_oom_debug();

        _cleanup_(OSSL_PARAM_BLD_freep) OSSL_PARAM_BLD *bld = OSSL_PARAM_BLD_new();
        if (!bld)
                return log_oom_debug();

        if (!OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_MAC_PARAM_DIGEST, (char*) digest_alg, 0))
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to set HMAC OSSL_MAC_PARAM_DIGEST.");

        _cleanup_(OSSL_PARAM_freep) OSSL_PARAM *params = OSSL_PARAM_BLD_to_param(bld);
        if (!params)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to build HMAC OSSL_PARAM.");

        if (!EVP_MAC_init(ctx, key, key_size, params))
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to initializate EVP_MAC_CTX.");
#else
        _cleanup_(HMAC_CTX_freep) HMAC_CTX *ctx = HMAC_CTX_new();
        if (!ctx)
                return log_oom_debug();

        const EVP_MD *digest_md = EVP_get_digestbyname(digest_alg);
        if (!digest_md)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to get EVP_MD for '%s'.", digest_alg);

        if (!HMAC_Init_ex(ctx, key, key_size, digest_md, NULL))
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to initialize HMAC_CTX.");
#endif

        for (size_t i = 0; i < n_data; i++)
#if OPENSSL_VERSION_MAJOR >= 3
                if (!EVP_MAC_update(ctx, data[i].iov_base, data[i].iov_len))
#else
                if (!HMAC_Update(ctx, data[i].iov_base, data[i].iov_len))
#endif
                        return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to update HMAC.");

        size_t digest_size;
#if OPENSSL_VERSION_MAJOR >= 3
        digest_size = EVP_MAC_CTX_get_mac_size(ctx);
#else
        digest_size = HMAC_size(ctx);
#endif
        if (digest_size == 0)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to get HMAC digest size.");

        _cleanup_free_ void *buf = malloc(digest_size);
        if (!buf)
                return log_oom_debug();

        size_t size;
#if OPENSSL_VERSION_MAJOR >= 3
        if (!EVP_MAC_final(ctx, buf, &size, digest_size))
#else
        if (!HMAC_Final(ctx, buf, &size))
#endif
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to finalize HMAC.");

        assert(size == digest_size);

        *ret_digest = TAKE_PTR(buf);
        *ret_digest_size = size;

        return 0;
}

/* Symmetric Cipher encryption. */
int openssl_cipher(
                const char *alg,
                size_t bits,
                const char *mode,
                const void *key,
                size_t key_size,
                const void *iv,
                size_t iv_size,
                const struct iovec data[],
                size_t n_data,
                void **ret,
                size_t *ret_size) {

        int r;

        assert(alg);
        assert(bits > 0);
        assert(mode);
        assert(key);
        assert(iv || iv_size == 0);
        assert(data || n_data == 0);
        assert(ret);
        assert(ret_size);

        _cleanup_free_ char *cipher_alg = NULL;
        r = asprintf(&cipher_alg, "%s-%zu-%s", alg, bits, mode);
        if (r < 0)
                return log_oom_debug();

#if OPENSSL_VERSION_MAJOR >= 3
        _cleanup_(EVP_CIPHER_freep) EVP_CIPHER *cipher = EVP_CIPHER_fetch(NULL, cipher_alg, NULL);
#else
        const EVP_CIPHER *cipher = EVP_get_cipherbyname(cipher_alg);
#endif
        if (!cipher)
                /* This probably means cipher_alg is not supported, but (for openssl >= 3) might be OOM. */
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to get EVP_CIPHER for '%s'.", cipher_alg);

        _cleanup_(EVP_CIPHER_CTX_freep) EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
        if (!ctx)
                return log_oom_debug();

        /* Verify enough key data was provided. */
        int cipher_key_length = EVP_CIPHER_key_length(cipher);
        if ((size_t) cipher_key_length > key_size)
                return log_debug_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "Not enough key bytes provided, require %d", cipher_key_length);

        /* Verify enough IV data was provided or, if no IV was provided, use a zeroed buffer for IV data. */
        int cipher_iv_length = EVP_CIPHER_iv_length(cipher);
        _cleanup_free_ void *zero_iv = NULL;
        if (!iv) {
                zero_iv = malloc0(cipher_iv_length);
                if (!zero_iv)
                        return log_oom_debug();

                iv = zero_iv;
                iv_size = (size_t) cipher_iv_length;
        }
        if ((size_t) cipher_iv_length > iv_size)
                return log_debug_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "Not enough IV bytes provided, require %d", cipher_iv_length);

        if (!EVP_EncryptInit(ctx, cipher, key, iv))
                return log_debug_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to initialize EVP_CIPHER_CTX.");

        int cipher_block_size = EVP_CIPHER_CTX_block_size(ctx);

        _cleanup_free_ uint8_t *buf = NULL;
        size_t size = 0;

        uint8_t *tmp;
        for (size_t i = 0; i < n_data; i++) {
                /* Cipher may produce (up to) input length + cipher block size of output. */
                tmp = realloc(buf, size + data[i].iov_len + cipher_block_size);
                if (!tmp)
                        return log_oom_debug();
                buf = TAKE_PTR(tmp);

                int update_size;
                if (!EVP_EncryptUpdate(ctx, &buf[size], &update_size, data[i].iov_base, data[i].iov_len))
                        return log_debug_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE), "Failed to update Cipher.");

                size += update_size;
        }

        tmp = realloc(buf, size + cipher_block_size);
        if (!tmp)
                return log_oom_debug();
        buf = TAKE_PTR(tmp);

        int final_size;
        if (!EVP_EncryptFinal_ex(ctx, &buf[size], &final_size))
                return log_debug_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE), "Failed to finalize Cipher.");

        *ret = TAKE_PTR(buf);
        *ret_size = size + final_size;

        return 0;
}

int kdf_ss_derive(
                const char *digest,
                const void *key,
                size_t key_size,
                const void *salt,
                size_t salt_size,
                const void *info,
                size_t info_size,
                size_t derive_size,
                void **ret) {

#if OPENSSL_VERSION_MAJOR >= 3
        assert(digest);
        assert(key);
        assert(derive_size > 0);
        assert(ret);

        _cleanup_(EVP_KDF_freep) EVP_KDF *kdf = EVP_KDF_fetch(NULL, "SSKDF", NULL);
        if (!kdf)
                return log_oom_debug();

        _cleanup_(EVP_KDF_CTX_freep) EVP_KDF_CTX *ctx = EVP_KDF_CTX_new(kdf);
        if (!ctx)
                return log_oom_debug();

        _cleanup_(OSSL_PARAM_BLD_freep) OSSL_PARAM_BLD *bld = OSSL_PARAM_BLD_new();
        if (!bld)
                return log_oom_debug();

        _cleanup_free_ void *buf = malloc(derive_size);
        if (!buf)
                return log_oom_debug();

        if (!OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_KDF_PARAM_DIGEST, (char*) digest, 0))
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to add KDF-SS OSSL_KDF_PARAM_DIGEST.");

        if (!OSSL_PARAM_BLD_push_octet_string(bld, OSSL_KDF_PARAM_KEY, (char*) key, key_size))
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to add KDF-SS OSSL_KDF_PARAM_KEY.");

        if (salt)
                if (!OSSL_PARAM_BLD_push_octet_string(bld, OSSL_KDF_PARAM_SALT, (char*) salt, salt_size))
                        return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to add KDF-SS OSSL_KDF_PARAM_SALT.");

        if (info)
                if (!OSSL_PARAM_BLD_push_octet_string(bld, OSSL_KDF_PARAM_INFO, (char*) info, info_size))
                        return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to add KDF-SS OSSL_KDF_PARAM_INFO.");

        _cleanup_(OSSL_PARAM_freep) OSSL_PARAM *params = OSSL_PARAM_BLD_to_param(bld);
        if (!params)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to build KDF-SS OSSL_PARAM.");

        if (EVP_KDF_derive(ctx, buf, derive_size, params) <= 0)
                return log_debug_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Openssl KDF-SS derive failed");

        *ret = TAKE_PTR(buf);

        return 0;
#else
        return log_debug_errno(SYNTHETIC_ERRNO(EOPNOTSUPP), "KDF-SS requires openssl >= 3.");
#endif
}

/* Perform Key-Based HMAC KDF. The mode must be "COUNTER" or "FEEDBACK". The parameter naming is from the
 * Openssl api, and maps to SP800-108 naming as "...key, salt, info, and seed correspond to KI, Label,
 * Context, and IV (respectively)...". The n_derive parameter specifies how many bytes are derived.
 *
 * For more details see: https://www.openssl.org/docs/manmaster/man7/EVP_KDF-KB.html */
int kdf_kb_hmac_derive(
                const char *mode,
                const char *digest,
                const void *key,
                size_t n_key,
                const void *salt,
                size_t n_salt,
                const void *info,
                size_t n_info,
                const void *seed,
                size_t n_seed,
                size_t n_derive,
                void **ret) {

#if OPENSSL_VERSION_MAJOR >= 3
        assert(mode);
        assert(digest);

        _cleanup_(EVP_KDF_freep) EVP_KDF *kdf = EVP_KDF_fetch(NULL, "KBKDF", NULL);
        if (!kdf)
                return log_oom_debug();

        _cleanup_(EVP_KDF_CTX_freep) EVP_KDF_CTX *ctx = EVP_KDF_CTX_new(kdf);
        if (!ctx)
                return log_oom_debug();

        _cleanup_(OSSL_PARAM_BLD_freep) OSSL_PARAM_BLD *bld = OSSL_PARAM_BLD_new();
        if (!bld)
                return log_oom_debug();

        if (!OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_KDF_PARAM_MAC, (char*) "HMAC", 0))
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to add KDF-KB OSSL_KDF_PARAM_MAC.");

        if (!OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_KDF_PARAM_MODE, (char*) mode, 0))
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to add KDF-KB OSSL_KDF_PARAM_MODE.");

        if (!OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_KDF_PARAM_DIGEST, (char*) digest, 0))
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to add KDF-KB OSSL_KDF_PARAM_DIGEST.");

        if (key)
                if (!OSSL_PARAM_BLD_push_octet_string(bld, OSSL_KDF_PARAM_KEY, (char*) key, n_key))
                        return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to add KDF-KB OSSL_KDF_PARAM_KEY.");

        if (salt)
                if (!OSSL_PARAM_BLD_push_octet_string(bld, OSSL_KDF_PARAM_SALT, (char*) salt, n_salt))
                        return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to add KDF-KB OSSL_KDF_PARAM_SALT.");

        if (info)
                if (!OSSL_PARAM_BLD_push_octet_string(bld, OSSL_KDF_PARAM_INFO, (char*) info, n_info))
                        return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to add KDF-KB OSSL_KDF_PARAM_INFO.");

        if (seed)
                if (!OSSL_PARAM_BLD_push_octet_string(bld, OSSL_KDF_PARAM_SEED, (char*) seed, n_seed))
                        return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to add KDF-KB OSSL_KDF_PARAM_SEED.");

        _cleanup_(OSSL_PARAM_freep) OSSL_PARAM *params = OSSL_PARAM_BLD_to_param(bld);
        if (!params)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to build KDF-KB OSSL_PARAM.");

        _cleanup_free_ void *buf = malloc(n_derive);
        if (!buf)
                return log_oom_debug();

        if (EVP_KDF_derive(ctx, buf, n_derive, params) <= 0)
                return log_debug_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Openssl KDF-KB derive failed");

        *ret = TAKE_PTR(buf);

        return 0;
#else
        return log_debug_errno(SYNTHETIC_ERRNO(EOPNOTSUPP), "KDF-KB requires openssl >= 3.");
#endif
}

int rsa_encrypt_bytes(
                EVP_PKEY *pkey,
                const void *decrypted_key,
                size_t decrypted_key_size,
                void **ret_encrypt_key,
                size_t *ret_encrypt_key_size) {

        _cleanup_(EVP_PKEY_CTX_freep) EVP_PKEY_CTX *ctx = NULL;
        _cleanup_free_ void *b = NULL;
        size_t l;

        ctx = EVP_PKEY_CTX_new(pkey, NULL);
        if (!ctx)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to allocate public key context");

        if (EVP_PKEY_encrypt_init(ctx) <= 0)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to initialize public key context");

        if (EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING) <= 0)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to configure PKCS#1 padding");

        if (EVP_PKEY_encrypt(ctx, NULL, &l, decrypted_key, decrypted_key_size) <= 0)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to determine encrypted key size");

        b = malloc(l);
        if (!b)
                return -ENOMEM;

        if (EVP_PKEY_encrypt(ctx, b, &l, decrypted_key, decrypted_key_size) <= 0)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to determine encrypted key size");

        *ret_encrypt_key = TAKE_PTR(b);
        *ret_encrypt_key_size = l;

        return 0;
}

int rsa_oaep_encrypt_bytes(
                const EVP_PKEY *pkey,
                const char *digest_alg,
                const char *label,
                const void *decrypted_key,
                size_t decrypted_key_size,
                void **ret_encrypt_key,
                size_t *ret_encrypt_key_size) {

        assert(pkey);
        assert(digest_alg);
        assert(label);
        assert(decrypted_key);
        assert(decrypted_key_size > 0);
        assert(ret_encrypt_key);
        assert(ret_encrypt_key_size);

        _cleanup_(EVP_PKEY_CTX_freep) EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new((EVP_PKEY*) pkey, NULL);
        if (!ctx)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to allocate public key context");

        if (EVP_PKEY_encrypt_init(ctx) <= 0)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to initialize public key context");

        if (EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) <= 0)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to configure RSA-OAEP padding");

#if OPENSSL_VERSION_MAJOR >= 3
        _cleanup_(EVP_MD_freep) EVP_MD *md = EVP_MD_fetch(NULL, digest_alg, NULL);
#else
        const EVP_MD *md = EVP_get_digestbyname(digest_alg);
#endif
        if (!md)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to get EVP_MD.");

        if (EVP_PKEY_CTX_set_rsa_oaep_md(ctx, md) <= 0)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to configure RSA-OAEP MD");

        /* openssl takes ownership of the label. */
        char *duplabel = strdup(label);
        if (!duplabel)
                return log_oom_debug();

        if (EVP_PKEY_CTX_set0_rsa_oaep_label(ctx, duplabel, strlen(duplabel) + 1) <= 0) {
                duplabel = mfree(duplabel);
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to configure RSA-OAEP label");
        }

        size_t size = 0;
        if (EVP_PKEY_encrypt(ctx, NULL, &size, decrypted_key, decrypted_key_size) <= 0)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to determine RSA-OAEP encrypted key size");

        _cleanup_free_ void *buf = malloc(size);
        if (!buf)
                return log_oom_debug();

        if (EVP_PKEY_encrypt(ctx, buf, &size, decrypted_key, decrypted_key_size) <= 0)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to RSA-OAEP encrypt");

        *ret_encrypt_key = TAKE_PTR(buf);
        *ret_encrypt_key_size = size;

        return 0;
}

int rsa_pkey_to_suitable_key_size(
                EVP_PKEY *pkey,
                size_t *ret_suitable_key_size) {

        size_t suitable_key_size;
        int bits;

        assert(pkey);
        assert(ret_suitable_key_size);

        /* Analyzes the specified public key and that it is RSA. If so, will return a suitable size for a
         * disk encryption key to encrypt with RSA for use in PKCS#11 security token schemes. */

        if (EVP_PKEY_base_id(pkey) != EVP_PKEY_RSA)
                return log_debug_errno(SYNTHETIC_ERRNO(EBADMSG), "X.509 certificate does not refer to RSA key.");

        bits = EVP_PKEY_bits(pkey);
        log_debug("Bits in RSA key: %i", bits);

        /* We use PKCS#1 padding for the RSA cleartext, hence let's leave some extra space for it, hence only
         * generate a random key half the size of the RSA length */
        suitable_key_size = bits / 8 / 2;

        if (suitable_key_size < 1)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Uh, RSA key size too short?");

        *ret_suitable_key_size = suitable_key_size;
        return 0;
}

/* Generate RSA public key from provided "n" and "e" values. Note that if "e" is a number (e.g. uint32_t), it
 * must be provided here big-endian, e.g. wrap it with htobe32(). */
int rsa_pkey_from_n_e(const void *n, size_t n_size, const void *e, size_t e_size, EVP_PKEY **ret) {
        _cleanup_(EVP_PKEY_freep) EVP_PKEY *pkey = NULL;

        assert(n);
        assert(e);
        assert(ret);

        _cleanup_(EVP_PKEY_CTX_freep) EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
        if (!ctx)
                return log_oom_debug();

        _cleanup_(BN_freep) BIGNUM *bn_n = BN_bin2bn(n, n_size, NULL);
        if (!bn_n)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to create BIGNUM for RSA n.");

        _cleanup_(BN_freep) BIGNUM *bn_e = BN_bin2bn(e, e_size, NULL);
        if (!bn_e)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to create BIGNUM for RSA e.");

#if OPENSSL_VERSION_MAJOR >= 3
        if (EVP_PKEY_fromdata_init(ctx) <= 0)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to initialize EVP_PKEY_CTX.");

        _cleanup_(OSSL_PARAM_BLD_freep) OSSL_PARAM_BLD *bld = OSSL_PARAM_BLD_new();
        if (!bld)
                return log_oom_debug();

        if (!OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_N, bn_n))
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to set RSA OSSL_PKEY_PARAM_RSA_N.");

        if (!OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_E, bn_e))
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to set RSA OSSL_PKEY_PARAM_RSA_E.");

        _cleanup_(OSSL_PARAM_freep) OSSL_PARAM *params = OSSL_PARAM_BLD_to_param(bld);
        if (!params)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to build RSA OSSL_PARAM.");

        if (EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_PUBLIC_KEY, params) <= 0)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to create RSA EVP_PKEY.");
#else
        _cleanup_(RSA_freep) RSA *rsa_key = RSA_new();
        if (!rsa_key)
                return log_oom_debug();

        if (!RSA_set0_key(rsa_key, bn_n, bn_e, NULL))
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to set RSA n/e.");
        /* rsa_key owns these now, don't free */
        bn_n = bn_e = NULL;

        pkey = EVP_PKEY_new();
        if (!pkey)
                return log_oom_debug();

        if (!EVP_PKEY_assign_RSA(pkey, rsa_key))
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to assign RSA key.");
        /* pkey owns this now, don't free */
        rsa_key = NULL;
#endif

        *ret = TAKE_PTR(pkey);

        return 0;
}

/* Get the "n" and "e" values from the pkey. The values are returned in "bin" format, i.e. BN_bn2bin(). */
int rsa_pkey_to_n_e(
                const EVP_PKEY *pkey,
                void **ret_n,
                size_t *ret_n_size,
                void **ret_e,
                size_t *ret_e_size) {

        assert(pkey);
        assert(ret_n);
        assert(ret_n_size);
        assert(ret_e);
        assert(ret_e_size);

#if OPENSSL_VERSION_MAJOR >= 3
        _cleanup_(BN_freep) BIGNUM *bn_n = NULL;
        if (!EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_N, &bn_n))
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to get RSA n.");

        _cleanup_(BN_freep) BIGNUM *bn_e = NULL;
        if (!EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_E, &bn_e))
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to get RSA e.");
#else
        const RSA *rsa = EVP_PKEY_get0_RSA((EVP_PKEY*) pkey);
        if (!rsa)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to get RSA key from public key.");

        const BIGNUM *bn_n = RSA_get0_n(rsa);
        if (!bn_n)
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to get RSA n.");

        const BIGNUM *bn_e = RSA_get0_e(rsa);
        if (!bn_e)
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to get RSA e.");
#endif

        size_t n_size = BN_num_bytes(bn_n), e_size = BN_num_bytes(bn_e);
        _cleanup_free_ void *n = malloc(n_size), *e = malloc(e_size);
        if (!n || !e)
                return log_oom_debug();

        assert(BN_bn2bin(bn_n, n) == (int) n_size);
        assert(BN_bn2bin(bn_e, e) == (int) e_size);

        *ret_n = TAKE_PTR(n);
        *ret_n_size = n_size;
        *ret_e = TAKE_PTR(e);
        *ret_e_size = e_size;

        return 0;
}

/* Generate a new RSA key with the specified number of bits. */
int rsa_pkey_new(size_t bits, EVP_PKEY **ret) {
        assert(ret);

        _cleanup_(EVP_PKEY_CTX_freep) EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
        if (!ctx)
                return log_oom_debug();

        if (EVP_PKEY_keygen_init(ctx) <= 0)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to initialize EVP_PKEY_CTX.");

        if (EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, (int) bits) <= 0)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to set RSA bits to %zu.", bits);

        _cleanup_(EVP_PKEY_freep) EVP_PKEY *pkey = NULL;
        if (EVP_PKEY_keygen(ctx, &pkey) <= 0)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to generate ECC key.");

        *ret = TAKE_PTR(pkey);

        return 0;
}

/* Generate ECC public key from provided curve ID and x/y points. */
int ecc_pkey_from_curve_x_y(
                int curve_id,
                const void *x,
                size_t x_size,
                const void *y,
                size_t y_size,
                EVP_PKEY **ret) {

        assert(x);
        assert(y);
        assert(ret);

        _cleanup_(EVP_PKEY_CTX_freep) EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
        if (!ctx)
                return log_oom_debug();

        _cleanup_(BN_freep) BIGNUM *bn_x = BN_bin2bn(x, x_size, NULL), *bn_y = BN_bin2bn(y, y_size, NULL);
        if (!bn_x || !bn_y)
                return log_oom_debug();

        _cleanup_(EC_GROUP_freep) EC_GROUP *group = EC_GROUP_new_by_curve_name(curve_id);
        if (!group)
                return log_debug_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                                       "ECC curve id %d not supported.", curve_id);

        _cleanup_(EC_POINT_freep) EC_POINT *point = EC_POINT_new(group);
        if (!point)
                return log_oom_debug();

        if (!EC_POINT_set_affine_coordinates(group, point, bn_x, bn_y, NULL))
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to set ECC coordinates.");

#if OPENSSL_VERSION_MAJOR >= 3
        if (EVP_PKEY_fromdata_init(ctx) <= 0)
                return log_debug_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to initialize EVP_PKEY_CTX.");

        _cleanup_(OSSL_PARAM_BLD_freep) OSSL_PARAM_BLD *bld = OSSL_PARAM_BLD_new();
        if (!bld)
                return log_oom_debug();

        if (!OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME, (char*) OSSL_EC_curve_nid2name(curve_id), 0))
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to add ECC OSSL_PKEY_PARAM_GROUP_NAME.");

        _cleanup_(OPENSSL_freep) void *pbuf = NULL;
        size_t pbuf_len = 0;
        pbuf_len = EC_POINT_point2buf(group, point, POINT_CONVERSION_UNCOMPRESSED, (unsigned char**) &pbuf, NULL);
        if (pbuf_len == 0)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to convert ECC point to buffer.");

        if (!OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY, pbuf, pbuf_len))
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to add ECC OSSL_PKEY_PARAM_PUB_KEY.");

        _cleanup_(OSSL_PARAM_freep) OSSL_PARAM *params = OSSL_PARAM_BLD_to_param(bld);
        if (!params)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to build ECC OSSL_PARAM.");

        _cleanup_(EVP_PKEY_freep) EVP_PKEY *pkey = NULL;
        if (EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_PUBLIC_KEY, params) <= 0)
                return log_debug_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                                       "Failed to create ECC EVP_PKEY.");
#else
        _cleanup_(EC_KEY_freep) EC_KEY *eckey = EC_KEY_new();
        if (!eckey)
                return log_oom_debug();

        if (!EC_KEY_set_group(eckey, group))
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to set ECC group.");

        if (!EC_KEY_set_public_key(eckey, point))
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to set ECC point.");

        _cleanup_(EVP_PKEY_freep) EVP_PKEY *pkey = EVP_PKEY_new();
        if (!pkey)
                return log_oom_debug();

        if (!EVP_PKEY_assign_EC_KEY(pkey, eckey))
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to assign ECC key.");
        /* pkey owns this now, don't free */
        eckey = NULL;
#endif

    *ret = TAKE_PTR(pkey);

    return 0;
}

int ecc_pkey_to_curve_x_y(
                const EVP_PKEY *pkey,
                int *ret_curve_id,
                void **ret_x,
                size_t *ret_x_size,
                void **ret_y,
                size_t *ret_y_size) {

        _cleanup_(BN_freep) BIGNUM *bn_x = NULL, *bn_y = NULL;
        int curve_id;

        assert(pkey);

#if OPENSSL_VERSION_MAJOR >= 3
        size_t name_size;
        if (!EVP_PKEY_get_utf8_string_param(pkey, OSSL_PKEY_PARAM_GROUP_NAME, NULL, 0, &name_size))
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to get ECC group name size.");

        _cleanup_free_ char *name = malloc(name_size + 1);
        if (!name)
                return log_oom_debug();

        if (!EVP_PKEY_get_utf8_string_param(pkey, OSSL_PKEY_PARAM_GROUP_NAME, name, name_size + 1, NULL))
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to get ECC group name.");

        curve_id = OBJ_sn2nid(name);
        if (curve_id == NID_undef)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to get ECC curve id.");

        if (!EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_EC_PUB_X, &bn_x))
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to get ECC point x.");

        if (!EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_EC_PUB_Y, &bn_y))
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to get ECC point y.");
#else
        const EC_KEY *eckey = EVP_PKEY_get0_EC_KEY((EVP_PKEY*) pkey);
        if (!eckey)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to get EC_KEY.");

        const EC_GROUP *group = EC_KEY_get0_group(eckey);
        if (!group)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to get EC_GROUP.");

        curve_id = EC_GROUP_get_curve_name(group);
        if (curve_id == NID_undef)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to get ECC curve id.");

        const EC_POINT *point = EC_KEY_get0_public_key(eckey);
        if (!point)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to get EC_POINT.");

        bn_x = BN_new();
        bn_y = BN_new();
        if (!bn_x || !bn_y)
                return log_oom_debug();

        if (!EC_POINT_get_affine_coordinates(group, point, bn_x, bn_y, NULL))
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to get ECC x/y.");
#endif

        size_t x_size = BN_num_bytes(bn_x), y_size = BN_num_bytes(bn_y);
        _cleanup_free_ void *x = malloc(x_size), *y = malloc(y_size);
        if (!x || !y)
                return log_oom_debug();

        assert(BN_bn2bin(bn_x, x) == (int) x_size);
        assert(BN_bn2bin(bn_y, y) == (int) y_size);

        if (ret_curve_id)
                *ret_curve_id = curve_id;
        if (ret_x)
                *ret_x = TAKE_PTR(x);
        if (ret_x_size)
                *ret_x_size = x_size;
        if (ret_y)
                *ret_y = TAKE_PTR(y);
        if (ret_y_size)
                *ret_y_size = y_size;

        return 0;
}

/* Generate a new ECC key for the specified ECC curve id. */
int ecc_pkey_new(int curve_id, EVP_PKEY **ret) {
        assert(ret);

        _cleanup_(EVP_PKEY_CTX_freep) EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
        if (!ctx)
                return log_oom_debug();

        if (EVP_PKEY_keygen_init(ctx) <= 0)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to initialize EVP_PKEY_CTX.");

        if (EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, curve_id) <= 0)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to set ECC curve %d.", curve_id);

        _cleanup_(EVP_PKEY_freep) EVP_PKEY *pkey = NULL;
        if (EVP_PKEY_keygen(ctx, &pkey) <= 0)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to generate ECC key.");

        *ret = TAKE_PTR(pkey);

        return 0;
}

/* Perform ECDH to derive a ECC shared secret. */
int ecc_ecdh(const EVP_PKEY *peerkey,
             EVP_PKEY **ret_pkey,
             void **ret_shared_secret,
             size_t *ret_shared_secret_size) {

        int curve_id, r;

        assert(peerkey);
        assert(ret_pkey);
        assert(ret_shared_secret);
        assert(ret_shared_secret_size);

        r = ecc_pkey_to_curve_x_y(peerkey, &curve_id, NULL, NULL, NULL, NULL);
        if (r < 0)
                return r;

        _cleanup_(EVP_PKEY_freep) EVP_PKEY *pkey = NULL;
        r = ecc_pkey_new(curve_id, &pkey);
        if (r < 0)
                return r;

        _cleanup_(EVP_PKEY_CTX_freep) EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(pkey, NULL);
        if (!ctx)
                return log_oom_debug();

        if (EVP_PKEY_derive_init(ctx) <= 0)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to initialize EVP_PKEY_CTX.");

        if (EVP_PKEY_derive_set_peer(ctx, (EVP_PKEY*) peerkey) <= 0)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to set ECC derive peer.");

        size_t shared_secret_size;
        if (EVP_PKEY_derive(ctx, NULL, &shared_secret_size) <= 0)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to get ECC shared secret size.");

        _cleanup_free_ void *shared_secret = malloc(shared_secret_size);
        if (!shared_secret)
                return log_oom_debug();

        if (EVP_PKEY_derive(ctx, (unsigned char*) shared_secret, &shared_secret_size) <= 0)
                return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Failed to derive ECC shared secret.");

        *ret_pkey = TAKE_PTR(pkey);
        *ret_shared_secret = TAKE_PTR(shared_secret);
        *ret_shared_secret_size = shared_secret_size;

        return 0;
}

int pubkey_fingerprint(EVP_PKEY *pk, const EVP_MD *md, void **ret, size_t *ret_size) {
        _cleanup_(EVP_MD_CTX_freep) EVP_MD_CTX* m = NULL;
        _cleanup_free_ void *d = NULL, *h = NULL;
        int sz, lsz, msz;
        unsigned umsz;
        unsigned char *dd;

        /* Calculates a message digest of the DER encoded public key */

        assert(pk);
        assert(md);
        assert(ret);
        assert(ret_size);

        sz = i2d_PublicKey(pk, NULL);
        if (sz < 0)
                return log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "Unable to convert public key to DER format: %s",
                                       ERR_error_string(ERR_get_error(), NULL));

        dd = d = malloc(sz);
        if (!d)
                return log_oom_debug();

        lsz = i2d_PublicKey(pk, &dd);
        if (lsz < 0)
                return log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "Unable to convert public key to DER format: %s",
                                       ERR_error_string(ERR_get_error(), NULL));

        m = EVP_MD_CTX_new();
        if (!m)
                return log_oom_debug();

        if (EVP_DigestInit_ex(m, md, NULL) != 1)
                return log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "Failed to initialize %s context.", EVP_MD_name(md));

        if (EVP_DigestUpdate(m, d, lsz) != 1)
                return log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "Failed to run %s context.", EVP_MD_name(md));

        msz = EVP_MD_size(md);
        assert(msz > 0);

        h = malloc(msz);
        if (!h)
                return log_oom_debug();

        umsz = msz;
        if (EVP_DigestFinal_ex(m, h, &umsz) != 1)
                return log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "Failed to finalize hash context.");

        assert(umsz == (unsigned) msz);

        *ret = TAKE_PTR(h);
        *ret_size = msz;

        return 0;
}

#  if PREFER_OPENSSL
int string_hashsum(
                const char *s,
                size_t len,
                const char *md_algorithm,
                char **ret) {

        int r;

        assert(s);
        assert(md_algorithm);
        assert(ret);

        _cleanup_free_ void *digest = NULL;
        size_t digest_size;
        r = openssl_digest(md_algorithm, s, len, &digest, &digest_size);
        if (r < 0)
                return r;

        *ret = hexmem(digest, digest_size);
        if (!*ret)
                return -ENOMEM;

        return 0;
}
#  endif
#endif

int x509_fingerprint(X509 *cert, uint8_t buffer[static SHA256_DIGEST_SIZE]) {
#if HAVE_OPENSSL
        _cleanup_free_ uint8_t *der = NULL;
        int dersz;

        assert(cert);

        dersz = i2d_X509(cert, &der);
        if (dersz < 0)
                return log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "Unable to convert PEM certificate to DER format: %s",
                                       ERR_error_string(ERR_get_error(), NULL));

        sha256_direct(der, dersz, buffer);
        return 0;
#else
        return log_debug_errno(SYNTHETIC_ERRNO(EOPNOTSUPP), "openssl is not supported, cannot calculate X509 fingerprint: %m");
#endif
}
