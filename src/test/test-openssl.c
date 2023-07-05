/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "hexdecoct.h"
#include "openssl-util.h"
#include "tests.h"

TEST(openssl_pkey_from_pem) {
        DEFINE_HEX_PTR(key_ecc, "2d2d2d2d2d424547494e205055424c4943204b45592d2d2d2d2d0a4d466b77457759484b6f5a497a6a3043415159494b6f5a497a6a30444151634451674145726a6e4575424c73496c3972687068777976584e50686a346a426e500a44586e794a304b395579724e6764365335413532542b6f5376746b436a365a726c34685847337741515558706f426c532b7448717452714c35513d3d0a2d2d2d2d2d454e44205055424c4943204b45592d2d2d2d2d0a");
        _cleanup_(EVP_PKEY_freep) EVP_PKEY *pkey_ecc = NULL;
        assert_se(openssl_pkey_from_pem(key_ecc, key_ecc_len, &pkey_ecc) >= 0);

        _cleanup_free_ void *x = NULL, *y = NULL;
        size_t x_len, y_len;
        int curve_id;
        assert_se(ecc_pkey_to_curve_x_y(pkey_ecc, &curve_id, &x, &x_len, &y, &y_len) >= 0);
        assert_se(curve_id == NID_X9_62_prime256v1);

        DEFINE_HEX_PTR(expected_x, "ae39c4b812ec225f6b869870caf5cd3e18f88c19cf0d79f22742bd532acd81de");
        assert_se(x_len == expected_x_len);
        assert_se(memcmp(x, expected_x, x_len) == 0);

        DEFINE_HEX_PTR(expected_y, "92e40e764fea12bed9028fa66b9788571b7c004145e9a01952fad1eab51a8be5");
        assert_se(y_len == expected_y_len);
        assert_se(memcmp(y, expected_y, y_len) == 0);

        DEFINE_HEX_PTR(key_rsa, "2d2d2d2d2d424547494e205055424c4943204b45592d2d2d2d2d0a4d494942496a414e42676b71686b6947397730424151454641414f43415138414d49494243674b4341514541795639434950652f505852337a436f63787045300a6a575262546c3568585844436b472f584b79374b6d2f4439584942334b734f5a31436a5937375571372f674359363170697838697552756a73413464503165380a593445336c68556d374a332b6473766b626f4b64553243626d52494c2f6675627771694c4d587a41673342575278747234547545443533527a373634554650640a307a70304b68775231496230444c67772f344e67566f314146763378784b4d6478774d45683567676b73733038326332706c354a504e32587677426f744e6b4d0a5471526c745a4a35355244436170696e7153334577376675646c4e735851357746766c7432377a7637344b585165616d704c59433037584f6761304c676c536b0a79754774586b6a50542f735542544a705374615769674d5a6f714b7479563463515a58436b4a52684459614c47587673504233687a766d5671636e6b47654e540a65774944415141420a2d2d2d2d2d454e44205055424c4943204b45592d2d2d2d2d0a");
        _cleanup_(EVP_PKEY_freep) EVP_PKEY *pkey_rsa = NULL;
        assert_se(openssl_pkey_from_pem(key_rsa, key_rsa_len, &pkey_rsa) >= 0);

        _cleanup_free_ void *n = NULL, *e = NULL;
        size_t n_len, e_len;
        assert_se(rsa_pkey_to_n_e(pkey_rsa, &n, &n_len, &e, &e_len) >= 0);

        DEFINE_HEX_PTR(expected_n, "c95f4220f7bf3d7477cc2a1cc691348d645b4e5e615d70c2906fd72b2eca9bf0fd5c80772ac399d428d8efb52aeff80263ad698b1f22b91ba3b00e1d3f57bc638137961526ec9dfe76cbe46e829d53609b99120bfdfb9bc2a88b317cc0837056471b6be13b840f9dd1cfbeb85053ddd33a742a1c11d486f40cb830ff8360568d4016fdf1c4a31dc7030487982092cb34f36736a65e493cdd97bf0068b4d90c4ea465b59279e510c26a98a7a92dc4c3b7ee76536c5d0e7016f96ddbbcefef829741e6a6a4b602d3b5ce81ad0b8254a4cae1ad5e48cf4ffb140532694ad6968a0319a2a2adc95e1c4195c29094610d868b197bec3c1de1cef995a9c9e419e3537b");
        assert_se(n_len == expected_n_len);
        assert_se(memcmp(n, expected_n, n_len) == 0);

        DEFINE_HEX_PTR(expected_e, "010001");
        assert_se(e_len == expected_e_len);
        assert_se(memcmp(e, expected_e, e_len) == 0);
}

TEST(rsa_pkey_n_e) {
        DEFINE_HEX_PTR(n, "e3975a2124a7c9fe57752d106314ff62f6da731632eac221f1c0255bdcf2a34eeb21e3ab89ba8759ddad3b68be99463c7f03f3d004028a35e6f7c6596aeab2558d490f1e1c38aed2ff796bda8d6d55704eefb6ac55842dd6e606bb707f66acc02f0db2aed0dabab885bd0c850f1bdc8ac4b6bc1f74858db8ca2ab57a3d4217c091e9cd78727a2e36b8126ea629e81fecc69b0bea601000a6c0b749c5be16f53f4fa9f208a581d804234eb6526ba3fee9822d58d1ab9cac2761d7f630eb7ad6054dff0856d41aea219e1adfd87256aa1532202a070f4b1044e718d1f38bbc5a4b1fcb024f04afaafda5edeacfdf0d0bdf35c359acd059e3edb5024e588458f9b5");
        uint32_t e = htobe32(0x10001);

        _cleanup_(EVP_PKEY_freep) EVP_PKEY *pkey = NULL;
        assert_se(rsa_pkey_from_n_e(n, n_len, &e, sizeof(e), &pkey) >= 0);

        _cleanup_(EVP_PKEY_CTX_freep) EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new((EVP_PKEY*) pkey, NULL);
        assert_se(ctx);
        assert_se(EVP_PKEY_verify_init(ctx) == 1);

        const char *msg = "this is a secret";
        DEFINE_HEX_PTR(sig, "14b53e0c6ad99a350c3d7811e8160f4ae03ad159815bb91bddb9735b833588df2eac221fbd3fc4ece0dd63bfaeddfdaf4ae67021e759f3638bc194836413414f54e8c4d01c9c37fa4488ea2ef772276b8a33822a53c97b1c35acfb4bc621cfb8fad88f0cf7d5491f05236886afbf9ed47f9469536482f50f74a20defa59d99676bed62a17b5eb98641df5a2f8080fa4b24f2749cc152fa65ba34c14022fcb27f1b36f52021950d7b9b6c3042c50b84cfb7d55a5f9235bfd58e1bf1f604eb93416c5fb5fd90cb68f1270dfa9daf67f52c604f62c2f2beee5e7e672b0e6e9833dd43dba99b77668540c850c9a81a5ea7aaf6297383e6135bd64572362333121fc7");
        assert_se(EVP_PKEY_verify(ctx, sig, sig_len, (unsigned char*) msg, strlen(msg)) == 1);

        DEFINE_HEX_PTR(invalid_sig, "1234");
        assert_se(EVP_PKEY_verify(ctx, invalid_sig, invalid_sig_len, (unsigned char*) msg, strlen(msg)) != 1);

        _cleanup_free_ void *n2 = NULL, *e2 = NULL;
        size_t n2_size, e2_size;
        assert_se(rsa_pkey_to_n_e(pkey, &n2, &n2_size, &e2, &e2_size) >= 0);
        assert_se(memcmp_nn(n, n_len, n2, n2_size) == 0);
        assert_se(e2_size <= sizeof(uint32_t));
        assert_se(memcmp(&((uint8_t*) &e)[sizeof(uint32_t) - e2_size], e2, e2_size) == 0);
}

TEST(ecc_pkey_curve_x_y) {
        int curveid = NID_X9_62_prime256v1;
        DEFINE_HEX_PTR(x, "2830d2c8f65d3efbef12303b968b91692f8bd04045dcb8a9656374e4ae61d818");
        DEFINE_HEX_PTR(y, "8a80750f76729defdcc2a4bc1a91c22e60109dd6e1ffde634a650a20bab172e9");

        _cleanup_(EVP_PKEY_freep) EVP_PKEY *pkey = NULL;
        assert_se(ecc_pkey_from_curve_x_y(curveid, x, x_len, y, y_len, &pkey) >= 0);

        _cleanup_(EVP_PKEY_CTX_freep) EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new((EVP_PKEY*) pkey, NULL);
        assert_se(ctx);
        assert_se(EVP_PKEY_verify_init(ctx) == 1);

        const char *msg = "this is a secret";
        DEFINE_HEX_PTR(sig, "3045022100f6ca10f7ed57a020679899b26dd5ac5a1079265885e2a6477f527b6a3f02b5ca02207b550eb3e7b69360aff977f7f6afac99c3f28266b6c5338ce373f6b59263000a");
        assert_se(EVP_PKEY_verify(ctx, sig, sig_len, (unsigned char*) msg, strlen(msg)) == 1);

        DEFINE_HEX_PTR(invalid_sig, "1234");
        assert_se(EVP_PKEY_verify(ctx, invalid_sig, invalid_sig_len, (unsigned char*) msg, strlen(msg)) != 1);

        _cleanup_free_ void *x2 = NULL, *y2 = NULL;
        size_t x2_size, y2_size;
        int curveid2;
        assert_se(ecc_pkey_to_curve_x_y(pkey, &curveid2, &x2, &x2_size, &y2, &y2_size) >= 0);
        assert_se(curveid == curveid2);
        assert_se(memcmp_nn(x, x_len, x2, x2_size) == 0);
        assert_se(memcmp_nn(y, y_len, y2, y2_size) == 0);
}

static void verify_digest(const char *digest_alg, const struct iovec *data, size_t n_data, const char *expect) {
        _cleanup_free_ void *digest = NULL;
        size_t digest_size;

        assert_se(openssl_digest_many(digest_alg, data, n_data, &digest, &digest_size) == 0);

        DEFINE_HEX_PTR(e, expect);
        assert_se(memcmp_nn(e, e_len, digest, digest_size) == 0);
}

#define _DEFINE_DIGEST_TEST(uniq, alg, expect, ...)                     \
        const struct iovec UNIQ_T(i, uniq)[] = { __VA_ARGS__ };         \
        verify_digest(alg,                                              \
                      UNIQ_T(i, uniq),                                  \
                      ELEMENTSOF(UNIQ_T(i, uniq)),                      \
                      expect);
#define DEFINE_DIGEST_TEST(alg, expect, ...) _DEFINE_DIGEST_TEST(UNIQ, alg, expect, __VA_ARGS__)
#define DEFINE_SHA1_TEST(expect, ...) DEFINE_DIGEST_TEST("SHA1", expect, __VA_ARGS__)
#define DEFINE_SHA256_TEST(expect, ...) DEFINE_DIGEST_TEST("SHA256", expect, __VA_ARGS__)
#define DEFINE_SHA384_TEST(expect, ...) DEFINE_DIGEST_TEST("SHA384", expect, __VA_ARGS__)
#define DEFINE_SHA512_TEST(expect, ...) DEFINE_DIGEST_TEST("SHA512", expect, __VA_ARGS__)

TEST(digest_many) {
        const struct iovec test = IOVEC_MAKE_STRING("test");

        /* Empty digests */
        DEFINE_SHA1_TEST("da39a3ee5e6b4b0d3255bfef95601890afd80709");
        DEFINE_SHA256_TEST("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        DEFINE_SHA384_TEST("38b060a751ac96384cd9327eb1b1e36a21fdb71114be07434c0cc7bf63f6e1da274edebfe76f65fbd51ad2f14898b95b");
        DEFINE_SHA512_TEST("cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");

        DEFINE_SHA1_TEST("a94a8fe5ccb19ba61c4c0873d391e987982fbbd3", test);
        DEFINE_SHA256_TEST("9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08", test);
        DEFINE_SHA384_TEST("768412320f7b0aa5812fce428dc4706b3cae50e02a64caa16a782249bfe8efc4b7ef1ccb126255d196047dfedf17a0a9", test);
        DEFINE_SHA512_TEST("ee26b0dd4af7e749aa1a8ee3c10ae9923f618980772e473f8819a5d4940e0db27ac185f8a0e1d5f84f88bc887fd67b143732c304cc5fa9ad8e6f57f50028a8ff", test);

        DEFINE_HEX_PTR(h1, "e9ff2b6dfbc03b8dd0471a0f23840334e3ef51c64a325945524563c0375284a092751eca8d084fae22f74a104559a0ee8339d1845538481e674e6d31d4f63089");
        DEFINE_HEX_PTR(h2, "5b6e809933a1b8d5a4a6bb62e20b36ae82d9408141e7479d0aa067273bd2d04007fb1977bad549d54330a49ed98f82b495ba");
        DEFINE_HEX_PTR(h3, "d2aeef94d7ba2a");
        DEFINE_HEX_PTR(h4, "1557db45ded3e38c79b5bb25c83ade42fa7d13047ef1b9a0b21a3c2ab2d4eee5c75e2927ce643163addbda65331035850a436c0acffc723f419e1d1cbf04c9064e6d850580c0732a12600f9feb");

        const struct iovec i1 = IOVEC_MAKE(h1, h1_len);
        const struct iovec i2 = IOVEC_MAKE(h2, h2_len);
        const struct iovec i3 = IOVEC_MAKE(h3, h3_len);
        const struct iovec i4 = IOVEC_MAKE(h4, h4_len);

        DEFINE_SHA1_TEST("8e7c659a6331508b06adf98b430759dafb92fc43", i1, i2, i3, i4);
        DEFINE_SHA256_TEST("4d6be38798786a5500651c1a02d96aa010e9d7b2bece1695294cd396d456cde8", i1, i2, i3, i4);
        DEFINE_SHA384_TEST("82e6ec14f8d90f1ae1fd4fb7f415ea6fdb674515b13092e3e548a8d37a8faed30cda8ea613ec2a015a51bc578dacc995", i1, i2, i3, i4);
        DEFINE_SHA512_TEST("21fe5beb15927257a9143ff59010e51d4c65c7c5237b0cd9a8db3c3fabe429be3a0759f9ace3cdd70f6ea543f998bec9bc3308833d70aa1bd380364de872a62c", i1, i2, i3, i4);

        DEFINE_SHA256_TEST("0e0ed67d6717dc08dd6f472f6c35107a92b8c2695dcba344b884436f97a9eb4d", i1, i1, i1, i4);

        DEFINE_SHA256_TEST("8fe8b8d1899c44bfb82e1edc4ff92642db5b2cb25c4210ea06c3846c757525a8", i1, i1, i1, i4, i4, i4, i4, i3, i3, i2);
}

DEFINE_TEST_MAIN(LOG_DEBUG);
