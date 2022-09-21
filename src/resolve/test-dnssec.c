/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#if HAVE_GCRYPT
#  include <gcrypt.h>
#endif

#include "alloc-util.h"
#include "hexdecoct.h"
#include "resolved-dns-dnssec.h"
#include "resolved-dns-rr.h"
#include "set.h"
#include "string-util.h"
#include "tests.h"

TEST(dnssec_verify_dns_key) {
        static const uint8_t ds1_fprint[] = {
                0x46, 0x8B, 0xC8, 0xDD, 0xC7, 0xE8, 0x27, 0x03, 0x40, 0xBB, 0x8A, 0x1F, 0x3B, 0x2E, 0x45, 0x9D,
                0x80, 0x67, 0x14, 0x01,
        };
        static const uint8_t ds2_fprint[] = {
                0x8A, 0xEE, 0x80, 0x47, 0x05, 0x5F, 0x83, 0xD1, 0x48, 0xBA, 0x8F, 0xF6, 0xDD, 0xA7, 0x60, 0xCE,
                0x94, 0xF7, 0xC7, 0x5E, 0x52, 0x4C, 0xF2, 0xE9, 0x50, 0xB9, 0x2E, 0xCB, 0xEF, 0x96, 0xB9, 0x98,
        };
        static const uint8_t dnskey_blob[] = {
                0x03, 0x01, 0x00, 0x01, 0xa8, 0x12, 0xda, 0x4f, 0xd2, 0x7d, 0x54, 0x14, 0x0e, 0xcc, 0x5b, 0x5e,
                0x45, 0x9c, 0x96, 0x98, 0xc0, 0xc0, 0x85, 0x81, 0xb1, 0x47, 0x8c, 0x7d, 0xe8, 0x39, 0x50, 0xcc,
                0xc5, 0xd0, 0xf2, 0x00, 0x81, 0x67, 0x79, 0xf6, 0xcc, 0x9d, 0xad, 0x6c, 0xbb, 0x7b, 0x6f, 0x48,
                0x97, 0x15, 0x1c, 0xfd, 0x0b, 0xfe, 0xd3, 0xd7, 0x7d, 0x9f, 0x81, 0x26, 0xd3, 0xc5, 0x65, 0x49,
                0xcf, 0x46, 0x62, 0xb0, 0x55, 0x6e, 0x47, 0xc7, 0x30, 0xef, 0x51, 0xfb, 0x3e, 0xc6, 0xef, 0xde,
                0x27, 0x3f, 0xfa, 0x57, 0x2d, 0xa7, 0x1d, 0x80, 0x46, 0x9a, 0x5f, 0x14, 0xb3, 0xb0, 0x2c, 0xbe,
                0x72, 0xca, 0xdf, 0xb2, 0xff, 0x36, 0x5b, 0x4f, 0xec, 0x58, 0x8e, 0x8d, 0x01, 0xe9, 0xa9, 0xdf,
                0xb5, 0x60, 0xad, 0x52, 0x4d, 0xfc, 0xa9, 0x3e, 0x8d, 0x35, 0x95, 0xb3, 0x4e, 0x0f, 0xca, 0x45,
                0x1b, 0xf7, 0xef, 0x3a, 0x88, 0x25, 0x08, 0xc7, 0x4e, 0x06, 0xc1, 0x62, 0x1a, 0xce, 0xd8, 0x77,
                0xbd, 0x02, 0x65, 0xf8, 0x49, 0xfb, 0xce, 0xf6, 0xa8, 0x09, 0xfc, 0xde, 0xb2, 0x09, 0x9d, 0x39,
                0xf8, 0x63, 0x9c, 0x32, 0x42, 0x7c, 0xa0, 0x30, 0x86, 0x72, 0x7a, 0x4a, 0xc6, 0xd4, 0xb3, 0x2d,
                0x24, 0xef, 0x96, 0x3f, 0xc2, 0xda, 0xd3, 0xf2, 0x15, 0x6f, 0xda, 0x65, 0x4b, 0x81, 0x28, 0x68,
                0xf4, 0xfe, 0x3e, 0x71, 0x4f, 0x50, 0x96, 0x72, 0x58, 0xa1, 0x89, 0xdd, 0x01, 0x61, 0x39, 0x39,
                0xc6, 0x76, 0xa4, 0xda, 0x02, 0x70, 0x3d, 0xc0, 0xdc, 0x8d, 0x70, 0x72, 0x04, 0x90, 0x79, 0xd4,
                0xec, 0x65, 0xcf, 0x49, 0x35, 0x25, 0x3a, 0x14, 0x1a, 0x45, 0x20, 0xeb, 0x31, 0xaf, 0x92, 0xba,
                0x20, 0xd3, 0xcd, 0xa7, 0x13, 0x44, 0xdc, 0xcf, 0xf0, 0x27, 0x34, 0xb9, 0xe7, 0x24, 0x6f, 0x73,
                0xe7, 0xea, 0x77, 0x03,
        };

        _cleanup_(dns_resource_record_unrefp) DnsResourceRecord *dnskey = NULL, *ds1 = NULL, *ds2 = NULL;
        _cleanup_set_free_ Set *digests = NULL;

        /* The two DS RRs in effect for nasa.gov on 2015-12-01. */
        ds1 = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_DS, "nasa.gov");
        assert_se(ds1);

        ds1->ds.key_tag = 47857;
        ds1->ds.algorithm = DNSSEC_ALGORITHM_RSASHA256;
        ds1->ds.digest_type = DNSSEC_DIGEST_SHA1;
        ds1->ds.digest_size = sizeof(ds1_fprint);
        ds1->ds.digest = memdup(ds1_fprint, ds1->ds.digest_size);
        assert_se(ds1->ds.digest);

        log_info("DS1: %s", strna(dns_resource_record_to_string(ds1)));

        ds2 = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_DS, "NASA.GOV");
        assert_se(ds2);

        ds2->ds.key_tag = 47857;
        ds2->ds.algorithm = DNSSEC_ALGORITHM_RSASHA256;
        ds2->ds.digest_type = DNSSEC_DIGEST_SHA256;
        ds2->ds.digest_size = sizeof(ds2_fprint);
        ds2->ds.digest = memdup(ds2_fprint, ds2->ds.digest_size);
        assert_se(ds2->ds.digest);

        log_info("DS2: %s", strna(dns_resource_record_to_string(ds2)));

        dnskey = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_DNSKEY, "nasa.GOV");
        assert_se(dnskey);

        dnskey->dnskey.flags = 257;
        dnskey->dnskey.protocol = 3;
        dnskey->dnskey.algorithm = DNSSEC_ALGORITHM_RSASHA256;
        dnskey->dnskey.key_size = sizeof(dnskey_blob);
        dnskey->dnskey.key = memdup(dnskey_blob, sizeof(dnskey_blob));
        assert_se(dnskey->dnskey.key);

        log_info("DNSKEY: %s", strna(dns_resource_record_to_string(dnskey)));
        log_info("DNSKEY keytag: %u", dnssec_keytag(dnskey, false));

        assert_se(set_ensure_allocated(&digests, NULL) >= 0);


        assert_se(dnssec_verify_dnskey_by_ds(dnskey, ds1, false, digests) == -EOPNOTSUPP);
        assert_se(dnssec_verify_dnskey_by_ds(dnskey, ds2, false, digests) == -EOPNOTSUPP);

        assert_se(set_put(digests, INT_TO_PTR(DNSSEC_DIGEST_SHA256)) > 0);
        assert_se(dnssec_verify_dnskey_by_ds(dnskey, ds1, false, digests) == -EOPNOTSUPP);
        assert_se(dnssec_verify_dnskey_by_ds(dnskey, ds2, false, digests) > 0);

        assert_se(set_put(digests, INT_TO_PTR(DNSSEC_DIGEST_SHA1)) > 0);
        assert_se(dnssec_verify_dnskey_by_ds(dnskey, ds1, false, digests) > 0);
        assert_se(dnssec_verify_dnskey_by_ds(dnskey, ds2, false, digests) > 0);
}

TEST(dnssec_verify_rfc8080_ed25519_example1) {
        static const uint8_t dnskey_blob[] = {
                0x97, 0x4d, 0x96, 0xa2, 0x2d, 0x22, 0x4b, 0xc0, 0x1a, 0xdb, 0x91, 0x50, 0x91, 0x47, 0x7d,
                0x44, 0xcc, 0xd9, 0x1c, 0x9a, 0x41, 0xa1, 0x14, 0x30, 0x01, 0x01, 0x17, 0xd5, 0x2c, 0x59,
                0x24, 0xe
        };
        static const uint8_t ds_fprint[] = {
                0xdd, 0xa6, 0xb9, 0x69, 0xbd, 0xfb, 0x79, 0xf7, 0x1e, 0xe7, 0xb7, 0xfb, 0xdf, 0xb7, 0xdc,
                0xd7, 0xad, 0xbb, 0xd3, 0x5d, 0xdf, 0x79, 0xed, 0x3b, 0x6d, 0xd7, 0xf6, 0xe3, 0x56, 0xdd,
                0xd7, 0x47, 0xf7, 0x6f, 0x5f, 0x7a, 0xe1, 0xa6, 0xf9, 0xe5, 0xce, 0xfc, 0x7b, 0xbf, 0x5a,
                0xdf, 0x4e, 0x1b
        };
        static const uint8_t signature_blob[] = {
                0xa0, 0xbf, 0x64, 0xac, 0x9b, 0xa7, 0xef, 0x17, 0xc1, 0x38, 0x85, 0x9c, 0x18, 0x78, 0xbb,
                0x99, 0xa8, 0x39, 0xfe, 0x17, 0x59, 0xac, 0xa5, 0xb0, 0xd7, 0x98, 0xcf, 0x1a, 0xb1, 0xe9,
                0x8d, 0x07, 0x91, 0x02, 0xf4, 0xdd, 0xb3, 0x36, 0x8f, 0x0f, 0xe4, 0x0b, 0xb3, 0x77, 0xf1,
                0xf0, 0x0e, 0x0c, 0xdd, 0xed, 0xb7, 0x99, 0x16, 0x7d, 0x56, 0xb6, 0xe9, 0x32, 0x78, 0x30,
                0x72, 0xba, 0x8d, 0x02
        };

        _cleanup_(dns_resource_record_unrefp) DnsResourceRecord *dnskey = NULL, *ds = NULL, *mx = NULL,
                *rrsig = NULL;
        _cleanup_(dns_answer_unrefp) DnsAnswer *answer = NULL;
        _cleanup_set_free_ Set *algorithms = NULL;
        DnssecResult result;

        dnskey = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_DNSKEY, "example.com.");
        assert_se(dnskey);

        dnskey->dnskey.flags = 257;
        dnskey->dnskey.protocol = 3;
        dnskey->dnskey.algorithm = DNSSEC_ALGORITHM_ED25519;
        dnskey->dnskey.key_size = sizeof(dnskey_blob);
        dnskey->dnskey.key = memdup(dnskey_blob, sizeof(dnskey_blob));
        assert_se(dnskey->dnskey.key);

        log_info("DNSKEY: %s", strna(dns_resource_record_to_string(dnskey)));

        ds = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_DS, "example.com.");
        assert_se(ds);

        ds->ds.key_tag = 3613;
        ds->ds.algorithm = DNSSEC_ALGORITHM_ED25519;
        ds->ds.digest_type = DNSSEC_DIGEST_SHA256;
        ds->ds.digest_size = sizeof(ds_fprint);
        ds->ds.digest = memdup(ds_fprint, ds->ds.digest_size);
        assert_se(ds->ds.digest);

        log_info("DS: %s", strna(dns_resource_record_to_string(ds)));

        mx = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_MX, "example.com.");
        assert_se(mx);

        mx->mx.priority = 10;
        mx->mx.exchange = strdup("mail.example.com.");
        assert_se(mx->mx.exchange);

        log_info("MX: %s", strna(dns_resource_record_to_string(mx)));

        rrsig = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_RRSIG, "example.com.");
        assert_se(rrsig);

        rrsig->rrsig.type_covered = DNS_TYPE_MX;
        rrsig->rrsig.algorithm = DNSSEC_ALGORITHM_ED25519;
        rrsig->rrsig.labels = 2;
        rrsig->rrsig.original_ttl = 3600;
        rrsig->rrsig.expiration = 1440021600;
        rrsig->rrsig.inception = 1438207200;
        rrsig->rrsig.key_tag = 3613;
        rrsig->rrsig.signer = strdup("example.com.");
        assert_se(rrsig->rrsig.signer);
        rrsig->rrsig.signature_size = sizeof(signature_blob);
        rrsig->rrsig.signature = memdup(signature_blob, rrsig->rrsig.signature_size);
        assert_se(rrsig->rrsig.signature);

        log_info("RRSIG: %s", strna(dns_resource_record_to_string(rrsig)));

        assert_se(dnssec_key_match_rrsig(mx->key, rrsig) > 0);
        assert_se(dnssec_rrsig_match_dnskey(rrsig, dnskey, false) > 0);

        answer = dns_answer_new(1);
        assert_se(answer);
        assert_se(dns_answer_add(answer, mx, 0, DNS_ANSWER_AUTHENTICATED, NULL) >= 0);

        assert_se(set_ensure_allocated(&algorithms, NULL) >= 0);
        assert_se(set_put(algorithms, INT_TO_PTR(DNSSEC_ALGORITHM_ED25519)) > 0);

        assert_se(dnssec_verify_rrset(answer, mx->key, rrsig, dnskey,
                                      rrsig->rrsig.inception * USEC_PER_SEC, algorithms, &result) >= 0);
#if PREFER_OPENSSL || GCRYPT_VERSION_NUMBER >= 0x010600
        assert_se(result == DNSSEC_VALIDATED);
#else
        assert_se(result == DNSSEC_UNSUPPORTED_ALGORITHM);
#endif
}

TEST(dnssec_verify_rfc8080_ed25519_example2) {
        static const uint8_t dnskey_blob[] = {
                0xcc, 0xf9, 0xd9, 0xfd, 0x0c, 0x04, 0x7b, 0xb4, 0xbc, 0x0b, 0x94, 0x8f, 0xcf, 0x63, 0x9f,
                0x4b, 0x94, 0x51, 0xe3, 0x40, 0x13, 0x93, 0x6f, 0xeb, 0x62, 0x71, 0x3d, 0xc4, 0x72, 0x4,
                0x8a, 0x3b
        };
        static const uint8_t ds_fprint[] = {
                0xe3, 0x4d, 0x7b, 0xf3, 0x56, 0xfd, 0xdf, 0x87, 0xb7, 0xf7, 0x67, 0x5e, 0xe3, 0xdd, 0x9e,
                0x73, 0xbe, 0xda, 0x7b, 0x67, 0xb5, 0xe5, 0xde, 0xf4, 0x7f, 0xae, 0x7b, 0xe5, 0xad, 0x5c,
                0xd1, 0xb7, 0x39, 0xf5, 0xce, 0x76, 0xef, 0x97, 0x34, 0xe1, 0xe6, 0xde, 0xf3, 0x47, 0x3a,
                0xeb, 0x5e, 0x1c
        };
        static const uint8_t signature_blob[] = {
                0xcd, 0x74, 0x34, 0x6e, 0x46, 0x20, 0x41, 0x31, 0x05, 0xc9, 0xf2, 0xf2, 0x8b, 0xd4, 0x28,
                0x89, 0x8e, 0x83, 0xf1, 0x97, 0x58, 0xa3, 0x8c, 0x32, 0x52, 0x15, 0x62, 0xa1, 0x86, 0x57,
                0x15, 0xd4, 0xf8, 0xd7, 0x44, 0x0f, 0x44, 0x84, 0xd0, 0x4a, 0xa2, 0x52, 0x9f, 0x34, 0x28,
                0x4a, 0x6e, 0x69, 0xa0, 0x9e, 0xe0, 0x0f, 0xb0, 0x10, 0x47, 0x43, 0xbb, 0x2a, 0xe2, 0x39,
                0x93, 0x6a, 0x5c, 0x06
        };

        _cleanup_(dns_resource_record_unrefp) DnsResourceRecord *dnskey = NULL, *ds = NULL, *mx = NULL,
                *rrsig = NULL;
        _cleanup_(dns_answer_unrefp) DnsAnswer *answer = NULL;
        _cleanup_set_free_ Set *algorithms = NULL;
        DnssecResult result;

        dnskey = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_DNSKEY, "example.com.");
        assert_se(dnskey);

        dnskey->dnskey.flags = 257;
        dnskey->dnskey.protocol = 3;
        dnskey->dnskey.algorithm = DNSSEC_ALGORITHM_ED25519;
        dnskey->dnskey.key_size = sizeof(dnskey_blob);
        dnskey->dnskey.key = memdup(dnskey_blob, sizeof(dnskey_blob));
        assert_se(dnskey->dnskey.key);

        log_info("DNSKEY: %s", strna(dns_resource_record_to_string(dnskey)));

        ds = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_DS, "example.com.");
        assert_se(ds);

        ds->ds.key_tag = 35217;
        ds->ds.algorithm = DNSSEC_ALGORITHM_ED25519;
        ds->ds.digest_type = DNSSEC_DIGEST_SHA256;
        ds->ds.digest_size = sizeof(ds_fprint);
        ds->ds.digest = memdup(ds_fprint, ds->ds.digest_size);
        assert_se(ds->ds.digest);

        log_info("DS: %s", strna(dns_resource_record_to_string(ds)));

        mx = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_MX, "example.com.");
        assert_se(mx);

        mx->mx.priority = 10;
        mx->mx.exchange = strdup("mail.example.com.");
        assert_se(mx->mx.exchange);

        log_info("MX: %s", strna(dns_resource_record_to_string(mx)));

        rrsig = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_RRSIG, "example.com.");
        assert_se(rrsig);

        rrsig->rrsig.type_covered = DNS_TYPE_MX;
        rrsig->rrsig.algorithm = DNSSEC_ALGORITHM_ED25519;
        rrsig->rrsig.labels = 2;
        rrsig->rrsig.original_ttl = 3600;
        rrsig->rrsig.expiration = 1440021600;
        rrsig->rrsig.inception = 1438207200;
        rrsig->rrsig.key_tag = 35217;
        rrsig->rrsig.signer = strdup("example.com.");
        assert_se(rrsig->rrsig.signer);
        rrsig->rrsig.signature_size = sizeof(signature_blob);
        rrsig->rrsig.signature = memdup(signature_blob, rrsig->rrsig.signature_size);
        assert_se(rrsig->rrsig.signature);

        log_info("RRSIG: %s", strna(dns_resource_record_to_string(rrsig)));

        assert_se(dnssec_key_match_rrsig(mx->key, rrsig) > 0);
        assert_se(dnssec_rrsig_match_dnskey(rrsig, dnskey, false) > 0);

        answer = dns_answer_new(1);
        assert_se(answer);
        assert_se(dns_answer_add(answer, mx, 0, DNS_ANSWER_AUTHENTICATED, NULL) >= 0);

        assert_se(set_ensure_allocated(&algorithms, NULL) >= 0);
        assert_se(set_put(algorithms, INT_TO_PTR(DNSSEC_ALGORITHM_ED25519)) > 0);

        assert_se(dnssec_verify_rrset(answer, mx->key, rrsig, dnskey,
                                      rrsig->rrsig.inception * USEC_PER_SEC, algorithms, &result) >= 0);
#if PREFER_OPENSSL || GCRYPT_VERSION_NUMBER >= 0x010600
        assert_se(result == DNSSEC_VALIDATED);
#else
        assert_se(result == DNSSEC_UNSUPPORTED_ALGORITHM);
#endif
}

TEST(dnssec_verify_rfc6605_example1) {
        static const uint8_t signature_blob[] = {
                0xab, 0x1e, 0xb0, 0x2d, 0x8a, 0xa6, 0x87, 0xe9, 0x7d, 0xa0, 0x22, 0x93, 0x37, 0xaa, 0x88, 0x73,
                0xe6, 0xf0, 0xeb, 0x26, 0xbe, 0x28, 0x9f, 0x28, 0x33, 0x3d, 0x18, 0x3f, 0x5d, 0x3b, 0x7a, 0x95,
                0xc0, 0xc8, 0x69, 0xad, 0xfb, 0x74, 0x8d, 0xae, 0xe3, 0xc5, 0x28, 0x6e, 0xed, 0x66, 0x82, 0xc1,
                0x2e, 0x55, 0x33, 0x18, 0x6b, 0xac, 0xed, 0x9c, 0x26, 0xc1, 0x67, 0xa9, 0xeb, 0xae, 0x95, 0x0b,
        };

        static const uint8_t ds_fprint[] = {
                0x6f, 0x87, 0x3c, 0x73, 0x57, 0xde, 0xd9, 0xee, 0xf8, 0xef, 0xbd, 0x76, 0xed, 0xbd, 0xbb, 0xd7,
                0x5e, 0x7a, 0xe7, 0xa6, 0x9d, 0xeb, 0x6e, 0x7a, 0x7f, 0x8d, 0xb8, 0xeb, 0x6e, 0x5b, 0x7f, 0x97,
                0x35, 0x7b, 0x6e, 0xfb, 0xd1, 0xc7, 0xba, 0x77, 0xa7, 0xb7, 0xed, 0xd7, 0xfa, 0xd5, 0xdd, 0x7b,
        };

        static const uint8_t dnskey_blob[] = {
                0x1a, 0x88, 0xc8, 0x86, 0x15, 0xd4, 0x37, 0xfb, 0xb8, 0xbf, 0x9e, 0x19, 0x42, 0xa1, 0x92, 0x9f,
                0x28, 0x56, 0x27, 0x06, 0xae, 0x6c, 0x2b, 0xd3, 0x99, 0xe7, 0xb1, 0xbf, 0xb6, 0xd1, 0xe9, 0xe7,
                0x5b, 0x92, 0xb4, 0xaa, 0x42, 0x91, 0x7a, 0xe1, 0xc6, 0x1b, 0x70, 0x1e, 0xf0, 0x35, 0xc3, 0xfe,
                0x7b, 0xe3, 0x00, 0x9c, 0xba, 0xfe, 0x5a, 0x2f, 0x71, 0x31, 0x6c, 0x90, 0x2d, 0xcf, 0x0d, 0x00,
        };

        _cleanup_(dns_resource_record_unrefp) DnsResourceRecord *dnskey = NULL, *ds = NULL, *a = NULL,
                *rrsig = NULL;
        _cleanup_(dns_answer_unrefp) DnsAnswer *answer = NULL;
        _cleanup_set_free_ Set *algorithms = NULL;
        DnssecResult result;

        dnskey = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_DNSKEY, "example.net.");
        assert_se(dnskey);

        dnskey->dnskey.flags = 257;
        dnskey->dnskey.protocol = 3;
        dnskey->dnskey.algorithm = DNSSEC_ALGORITHM_ECDSAP256SHA256;
        dnskey->dnskey.key_size = sizeof(dnskey_blob);
        dnskey->dnskey.key = memdup(dnskey_blob, sizeof(dnskey_blob));
        assert_se(dnskey->dnskey.key);

        log_info("DNSKEY: %s", strna(dns_resource_record_to_string(dnskey)));

        ds = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_DS, "example.net.");
        assert_se(ds);

        ds->ds.key_tag = 55648;
        ds->ds.algorithm = DNSSEC_ALGORITHM_ECDSAP256SHA256;
        ds->ds.digest_type = DNSSEC_DIGEST_SHA256;
        ds->ds.digest_size = sizeof(ds_fprint);
        ds->ds.digest = memdup(ds_fprint, ds->ds.digest_size);
        assert_se(ds->ds.digest);

        log_info("DS: %s", strna(dns_resource_record_to_string(ds)));

        a = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_A, "www.example.net");
        assert_se(a);

        a->a.in_addr.s_addr = inet_addr("192.0.2.1");

        log_info("A: %s", strna(dns_resource_record_to_string(a)));

        rrsig = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_RRSIG, "www.example.net.");
        assert_se(rrsig);

        rrsig->rrsig.type_covered = DNS_TYPE_A;
        rrsig->rrsig.algorithm = DNSSEC_ALGORITHM_ECDSAP256SHA256;
        rrsig->rrsig.labels = 3;
        rrsig->rrsig.expiration = 1284026679;
        rrsig->rrsig.inception = 1281607479;
        rrsig->rrsig.key_tag = 55648;
        rrsig->rrsig.original_ttl = 3600;
        rrsig->rrsig.signer = strdup("example.net.");
        assert_se(rrsig->rrsig.signer);
        rrsig->rrsig.signature_size = sizeof(signature_blob);
        rrsig->rrsig.signature = memdup(signature_blob, rrsig->rrsig.signature_size);
        assert_se(rrsig->rrsig.signature);

        log_info("RRSIG: %s", strna(dns_resource_record_to_string(rrsig)));

        assert_se(dnssec_key_match_rrsig(a->key, rrsig) > 0);
        assert_se(dnssec_rrsig_match_dnskey(rrsig, dnskey, false) > 0);

        answer = dns_answer_new(1);
        assert_se(answer);
        assert_se(dns_answer_add(answer, a, 0, DNS_ANSWER_AUTHENTICATED, NULL) >= 0);

        assert_se(set_ensure_allocated(&algorithms, NULL) >= 0);
        assert_se(set_put(algorithms, INT_TO_PTR(DNSSEC_ALGORITHM_ECDSAP256SHA256)) > 0);

        assert_se(dnssec_verify_rrset(answer, a->key, rrsig, dnskey,
                                      rrsig->rrsig.inception * USEC_PER_SEC, algorithms, &result) >= 0);
        assert_se(result == DNSSEC_VALIDATED);
}

TEST(dnssec_verify_rfc6605_example2) {
        static const uint8_t signature_blob[] = {
                0xfc, 0xbe, 0x61, 0x0c, 0xa2, 0x2f, 0x18, 0x3c, 0x88, 0xd5, 0xf7, 0x00, 0x45, 0x7d, 0xf3, 0xeb,
                0x9a, 0xab, 0x98, 0xfb, 0x15, 0xcf, 0xbd, 0xd0, 0x0f, 0x53, 0x2b, 0xe4, 0x21, 0x2a, 0x3a, 0x22,
                0xcf, 0xf7, 0x98, 0x71, 0x42, 0x8b, 0xae, 0xae, 0x81, 0x82, 0x79, 0x93, 0xaf, 0xcc, 0x56, 0xb1,
                0xb1, 0x3f, 0x06, 0x96, 0xbe, 0xf8, 0x85, 0xb6, 0xaf, 0x44, 0xa6, 0xb2, 0x24, 0xdb, 0xb2, 0x74,
                0x2b, 0xb3, 0x59, 0x34, 0x92, 0x3d, 0xdc, 0xfb, 0xc2, 0x7a, 0x97, 0x2f, 0x96, 0xdd, 0x70, 0x9c,
                0xee, 0xb1, 0xd9, 0xc8, 0xd1, 0x14, 0x8c, 0x44, 0xec, 0x71, 0xc0, 0x68, 0xa9, 0x59, 0xc2, 0x66,

        };

        static const uint8_t ds_fprint[] = {
                0xef, 0x67, 0x7b, 0x6f, 0xad, 0xbd, 0xef, 0xa7, 0x1e, 0xd3, 0xae, 0x37, 0xf1, 0xef, 0x5c, 0xd1,
                0xb7, 0xf7, 0xd7, 0xdd, 0x35, 0xdd, 0xc7, 0xfc, 0xd3, 0x57, 0xf4, 0xf5, 0xe7, 0x1c, 0xf3, 0x86,
                0xfc, 0x77, 0xb7, 0xbd, 0xe3, 0xde, 0x5f, 0xdb, 0xb7, 0xb7, 0xd3, 0x97, 0x3a, 0x6b, 0xd6, 0xf4,
                0xe7, 0xad, 0xda, 0xf5, 0xbe, 0x5f, 0xe1, 0xdd, 0xbc, 0xf3, 0x8d, 0x39, 0x73, 0x7d, 0x34, 0xf1,
                0xaf, 0x78, 0xe9, 0xd7, 0xfd, 0xf3, 0x77, 0x7a,
        };

        static const uint8_t dnskey_blob[] = {
                0xc4, 0xa6, 0x1a, 0x36, 0x15, 0x9d, 0x18, 0xe7, 0xc9, 0xfa, 0x73, 0xeb, 0x2f, 0xcf, 0xda, 0xae,
                0x4c, 0x1f, 0xd8, 0x46, 0x37, 0x30, 0x32, 0x7e, 0x48, 0x4a, 0xca, 0x8a, 0xf0, 0x55, 0x4a, 0xe9,
                0xb5, 0xc3, 0xf7, 0xa0, 0xb1, 0x7b, 0xd2, 0x00, 0x3b, 0x4d, 0x26, 0x1c, 0x9e, 0x9b, 0x94, 0x42,
                0x3a, 0x98, 0x10, 0xe8, 0xaf, 0x17, 0xd4, 0x34, 0x52, 0x12, 0x4a, 0xdb, 0x61, 0x0f, 0x8e, 0x07,
                0xeb, 0xfc, 0xfe, 0xe5, 0xf8, 0xe4, 0xd0, 0x70, 0x63, 0xca, 0xe9, 0xeb, 0x91, 0x7a, 0x1a, 0x5b,
                0xab, 0xf0, 0x8f, 0xe6, 0x95, 0x53, 0x60, 0x17, 0xa5, 0xbf, 0xa9, 0x32, 0x37, 0xee, 0x6e, 0x34,
        };


        _cleanup_(dns_resource_record_unrefp) DnsResourceRecord *dnskey = NULL, *ds = NULL, *a = NULL,
                *rrsig = NULL;
        _cleanup_(dns_answer_unrefp) DnsAnswer *answer = NULL;
        _cleanup_set_free_ Set *algorithms = NULL;
        DnssecResult result;

        dnskey = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_DNSKEY, "example.net.");
        assert_se(dnskey);

        dnskey->dnskey.flags = 257;
        dnskey->dnskey.protocol = 3;
        dnskey->dnskey.algorithm = DNSSEC_ALGORITHM_ECDSAP384SHA384;
        dnskey->dnskey.key_size = sizeof(dnskey_blob);
        dnskey->dnskey.key = memdup(dnskey_blob, sizeof(dnskey_blob));
        assert_se(dnskey->dnskey.key);

        log_info("DNSKEY: %s", strna(dns_resource_record_to_string(dnskey)));

        ds = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_DS, "example.net.");
        assert_se(ds);

        ds->ds.key_tag = 10771;
        ds->ds.algorithm = DNSSEC_ALGORITHM_ECDSAP384SHA384;
        ds->ds.digest_type = DNSSEC_DIGEST_SHA384;
        ds->ds.digest_size = sizeof(ds_fprint);
        ds->ds.digest = memdup(ds_fprint, ds->ds.digest_size);
        assert_se(ds->ds.digest);

        log_info("DS: %s", strna(dns_resource_record_to_string(ds)));

        a = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_A, "www.example.net");
        assert_se(a);

        a->a.in_addr.s_addr = inet_addr("192.0.2.1");

        log_info("A: %s", strna(dns_resource_record_to_string(a)));

        rrsig = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_RRSIG, "www.example.net.");
        assert_se(rrsig);

        rrsig->rrsig.type_covered = DNS_TYPE_A;
        rrsig->rrsig.algorithm = DNSSEC_ALGORITHM_ECDSAP384SHA384;
        rrsig->rrsig.labels = 3;
        rrsig->rrsig.expiration = 1284027625;
        rrsig->rrsig.inception = 1281608425;
        rrsig->rrsig.key_tag = 10771;
        rrsig->rrsig.original_ttl = 3600;
        rrsig->rrsig.signer = strdup("example.net.");
        assert_se(rrsig->rrsig.signer);
        rrsig->rrsig.signature_size = sizeof(signature_blob);
        rrsig->rrsig.signature = memdup(signature_blob, rrsig->rrsig.signature_size);
        assert_se(rrsig->rrsig.signature);

        log_info("RRSIG: %s", strna(dns_resource_record_to_string(rrsig)));

        assert_se(dnssec_key_match_rrsig(a->key, rrsig) > 0);
        assert_se(dnssec_rrsig_match_dnskey(rrsig, dnskey, false) > 0);

        answer = dns_answer_new(1);
        assert_se(answer);
        assert_se(dns_answer_add(answer, a, 0, DNS_ANSWER_AUTHENTICATED, NULL) >= 0);

        assert_se(set_ensure_allocated(&algorithms, NULL) >= 0);
        assert_se(set_put(algorithms, INT_TO_PTR(DNSSEC_ALGORITHM_ECDSAP384SHA384)) > 0);

        assert_se(dnssec_verify_rrset(answer, a->key, rrsig, dnskey,
                                      rrsig->rrsig.inception * USEC_PER_SEC, algorithms, &result) >= 0);
        assert_se(result == DNSSEC_VALIDATED);
}

TEST(dnssec_verify_rrset) {
        static const uint8_t signature_blob[] = {
                0x7f, 0x79, 0xdd, 0x5e, 0x89, 0x79, 0x18, 0xd0, 0x34, 0x86, 0x8c, 0x72, 0x77, 0x75, 0x48, 0x4d,
                0xc3, 0x7d, 0x38, 0x04, 0xab, 0xcd, 0x9e, 0x4c, 0x82, 0xb0, 0x92, 0xca, 0xe9, 0x66, 0xe9, 0x6e,
                0x47, 0xc7, 0x68, 0x8c, 0x94, 0xf6, 0x69, 0xcb, 0x75, 0x94, 0xe6, 0x30, 0xa6, 0xfb, 0x68, 0x64,
                0x96, 0x1a, 0x84, 0xe1, 0xdc, 0x16, 0x4c, 0x83, 0x6c, 0x44, 0xf2, 0x74, 0x4d, 0x74, 0x79, 0x8f,
                0xf3, 0xf4, 0x63, 0x0d, 0xef, 0x5a, 0xe7, 0xe2, 0xfd, 0xf2, 0x2b, 0x38, 0x7c, 0x28, 0x96, 0x9d,
                0xb6, 0xcd, 0x5c, 0x3b, 0x57, 0xe2, 0x24, 0x78, 0x65, 0xd0, 0x9e, 0x77, 0x83, 0x09, 0x6c, 0xff,
                0x3d, 0x52, 0x3f, 0x6e, 0xd1, 0xed, 0x2e, 0xf9, 0xee, 0x8e, 0xa6, 0xbe, 0x9a, 0xa8, 0x87, 0x76,
                0xd8, 0x77, 0xcc, 0x96, 0xa0, 0x98, 0xa1, 0xd1, 0x68, 0x09, 0x43, 0xcf, 0x56, 0xd9, 0xd1, 0x66,
        };

        static const uint8_t dnskey_blob[] = {
                0x03, 0x01, 0x00, 0x01, 0x9b, 0x49, 0x9b, 0xc1, 0xf9, 0x9a, 0xe0, 0x4e, 0xcf, 0xcb, 0x14, 0x45,
                0x2e, 0xc9, 0xf9, 0x74, 0xa7, 0x18, 0xb5, 0xf3, 0xde, 0x39, 0x49, 0xdf, 0x63, 0x33, 0x97, 0x52,
                0xe0, 0x8e, 0xac, 0x50, 0x30, 0x8e, 0x09, 0xd5, 0x24, 0x3d, 0x26, 0xa4, 0x49, 0x37, 0x2b, 0xb0,
                0x6b, 0x1b, 0xdf, 0xde, 0x85, 0x83, 0xcb, 0x22, 0x4e, 0x60, 0x0a, 0x91, 0x1a, 0x1f, 0xc5, 0x40,
                0xb1, 0xc3, 0x15, 0xc1, 0x54, 0x77, 0x86, 0x65, 0x53, 0xec, 0x10, 0x90, 0x0c, 0x91, 0x00, 0x5e,
                0x15, 0xdc, 0x08, 0x02, 0x4c, 0x8c, 0x0d, 0xc0, 0xac, 0x6e, 0xc4, 0x3e, 0x1b, 0x80, 0x19, 0xe4,
                0xf7, 0x5f, 0x77, 0x51, 0x06, 0x87, 0x61, 0xde, 0xa2, 0x18, 0x0f, 0x40, 0x8b, 0x79, 0x72, 0xfa,
                0x8d, 0x1a, 0x44, 0x47, 0x0d, 0x8e, 0x3a, 0x2d, 0xc7, 0x39, 0xbf, 0x56, 0x28, 0x97, 0xd9, 0x20,
                0x4f, 0x00, 0x51, 0x3b,
        };

        _cleanup_(dns_resource_record_unrefp) DnsResourceRecord *a = NULL, *rrsig = NULL, *dnskey = NULL;
        _cleanup_(dns_answer_unrefp) DnsAnswer *answer = NULL;
        _cleanup_set_free_ Set *algorithms = NULL;
        DnssecResult result;

        a = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_A, "nAsA.gov");
        assert_se(a);

        a->a.in_addr.s_addr = inet_addr("52.0.14.116");

        log_info("A: %s", strna(dns_resource_record_to_string(a)));

        rrsig = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_RRSIG, "NaSa.GOV.");
        assert_se(rrsig);

        rrsig->rrsig.type_covered = DNS_TYPE_A;
        rrsig->rrsig.algorithm = DNSSEC_ALGORITHM_RSASHA256;
        rrsig->rrsig.labels = 2;
        rrsig->rrsig.original_ttl = 600;
        rrsig->rrsig.expiration = 0x5683135c;
        rrsig->rrsig.inception = 0x565b7da8;
        rrsig->rrsig.key_tag = 63876;
        rrsig->rrsig.signer = strdup("Nasa.Gov.");
        assert_se(rrsig->rrsig.signer);
        rrsig->rrsig.signature_size = sizeof(signature_blob);
        rrsig->rrsig.signature = memdup(signature_blob, rrsig->rrsig.signature_size);
        assert_se(rrsig->rrsig.signature);

        log_info("RRSIG: %s", strna(dns_resource_record_to_string(rrsig)));

        dnskey = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_DNSKEY, "nASA.gOV");
        assert_se(dnskey);

        dnskey->dnskey.flags = 256;
        dnskey->dnskey.protocol = 3;
        dnskey->dnskey.algorithm = DNSSEC_ALGORITHM_RSASHA256;
        dnskey->dnskey.key_size = sizeof(dnskey_blob);
        dnskey->dnskey.key = memdup(dnskey_blob, sizeof(dnskey_blob));
        assert_se(dnskey->dnskey.key);

        log_info("DNSKEY: %s", strna(dns_resource_record_to_string(dnskey)));
        log_info("DNSKEY keytag: %u", dnssec_keytag(dnskey, false));

        assert_se(dnssec_key_match_rrsig(a->key, rrsig) > 0);
        assert_se(dnssec_rrsig_match_dnskey(rrsig, dnskey, false) > 0);

        answer = dns_answer_new(1);
        assert_se(answer);
        assert_se(dns_answer_add(answer, a, 0, DNS_ANSWER_AUTHENTICATED, NULL) >= 0);

        assert_se(set_ensure_allocated(&algorithms, NULL) >= 0);
        assert_se(set_put(algorithms, INT_TO_PTR(DNSSEC_ALGORITHM_RSASHA256)) > 0);

        /* Validate the RR as it if was 2015-12-2 today */
        assert_se(dnssec_verify_rrset(answer, a->key, rrsig, dnskey, 1449092754*USEC_PER_SEC, algorithms, &result) >= 0);
        assert_se(result == DNSSEC_VALIDATED);
}

TEST(dnssec_verify_rrset2) {
        static const uint8_t signature_blob[] = {
                0x48, 0x45, 0xc8, 0x8b, 0xc0, 0x14, 0x92, 0xf5, 0x15, 0xc6, 0x84, 0x9d, 0x2f, 0xe3, 0x32, 0x11,
                0x7d, 0xf1, 0xe6, 0x87, 0xb9, 0x42, 0xd3, 0x8b, 0x9e, 0xaf, 0x92, 0x31, 0x0a, 0x53, 0xad, 0x8b,
                0xa7, 0x5c, 0x83, 0x39, 0x8c, 0x28, 0xac, 0xce, 0x6e, 0x9c, 0x18, 0xe3, 0x31, 0x16, 0x6e, 0xca,
                0x38, 0x31, 0xaf, 0xd9, 0x94, 0xf1, 0x84, 0xb1, 0xdf, 0x5a, 0xc2, 0x73, 0x22, 0xf6, 0xcb, 0xa2,
                0xe7, 0x8c, 0x77, 0x0c, 0x74, 0x2f, 0xc2, 0x13, 0xb0, 0x93, 0x51, 0xa9, 0x4f, 0xae, 0x0a, 0xda,
                0x45, 0xcc, 0xfd, 0x43, 0x99, 0x36, 0x9a, 0x0d, 0x21, 0xe0, 0xeb, 0x30, 0x65, 0xd4, 0xa0, 0x27,
                0x37, 0x3b, 0xe4, 0xc1, 0xc5, 0xa1, 0x2a, 0xd1, 0x76, 0xc4, 0x7e, 0x64, 0x0e, 0x5a, 0xa6, 0x50,
                0x24, 0xd5, 0x2c, 0xcc, 0x6d, 0xe5, 0x37, 0xea, 0xbd, 0x09, 0x34, 0xed, 0x24, 0x06, 0xa1, 0x22,
        };

        static const uint8_t dnskey_blob[] = {
                0x03, 0x01, 0x00, 0x01, 0xc3, 0x7f, 0x1d, 0xd1, 0x1c, 0x97, 0xb1, 0x13, 0x34, 0x3a, 0x9a, 0xea,
                0xee, 0xd9, 0x5a, 0x11, 0x1b, 0x17, 0xc7, 0xe3, 0xd4, 0xda, 0x20, 0xbc, 0x5d, 0xba, 0x74, 0xe3,
                0x37, 0x99, 0xec, 0x25, 0xce, 0x93, 0x7f, 0xbd, 0x22, 0x73, 0x7e, 0x14, 0x71, 0xe0, 0x60, 0x07,
                0xd4, 0x39, 0x8b, 0x5e, 0xe9, 0xba, 0x25, 0xe8, 0x49, 0xe9, 0x34, 0xef, 0xfe, 0x04, 0x5c, 0xa5,
                0x27, 0xcd, 0xa9, 0xda, 0x70, 0x05, 0x21, 0xab, 0x15, 0x82, 0x24, 0xc3, 0x94, 0xf5, 0xd7, 0xb7,
                0xc4, 0x66, 0xcb, 0x32, 0x6e, 0x60, 0x2b, 0x55, 0x59, 0x28, 0x89, 0x8a, 0x72, 0xde, 0x88, 0x56,
                0x27, 0x95, 0xd9, 0xac, 0x88, 0x4f, 0x65, 0x2b, 0x68, 0xfc, 0xe6, 0x41, 0xc1, 0x1b, 0xef, 0x4e,
                0xd6, 0xc2, 0x0f, 0x64, 0x88, 0x95, 0x5e, 0xdd, 0x3a, 0x02, 0x07, 0x50, 0xa9, 0xda, 0xa4, 0x49,
                0x74, 0x62, 0xfe, 0xd7,
        };

        _cleanup_(dns_resource_record_unrefp) DnsResourceRecord *nsec = NULL, *rrsig = NULL, *dnskey = NULL;
        _cleanup_(dns_answer_unrefp) DnsAnswer *answer = NULL;
        _cleanup_set_free_ Set *algorithms = NULL;
        DnssecResult result;

        nsec = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_NSEC, "nasa.gov");
        assert_se(nsec);

        nsec->nsec.next_domain_name = strdup("3D-Printing.nasa.gov");
        assert_se(nsec->nsec.next_domain_name);

        nsec->nsec.types = bitmap_new();
        assert_se(nsec->nsec.types);
        assert_se(bitmap_set(nsec->nsec.types, DNS_TYPE_A) >= 0);
        assert_se(bitmap_set(nsec->nsec.types, DNS_TYPE_NS) >= 0);
        assert_se(bitmap_set(nsec->nsec.types, DNS_TYPE_SOA) >= 0);
        assert_se(bitmap_set(nsec->nsec.types, DNS_TYPE_MX) >= 0);
        assert_se(bitmap_set(nsec->nsec.types, DNS_TYPE_TXT) >= 0);
        assert_se(bitmap_set(nsec->nsec.types, DNS_TYPE_RRSIG) >= 0);
        assert_se(bitmap_set(nsec->nsec.types, DNS_TYPE_NSEC) >= 0);
        assert_se(bitmap_set(nsec->nsec.types, DNS_TYPE_DNSKEY) >= 0);
        assert_se(bitmap_set(nsec->nsec.types, 65534) >= 0);

        log_info("NSEC: %s", strna(dns_resource_record_to_string(nsec)));

        rrsig = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_RRSIG, "NaSa.GOV.");
        assert_se(rrsig);

        rrsig->rrsig.type_covered = DNS_TYPE_NSEC;
        rrsig->rrsig.algorithm = DNSSEC_ALGORITHM_RSASHA256;
        rrsig->rrsig.labels = 2;
        rrsig->rrsig.original_ttl = 300;
        rrsig->rrsig.expiration = 0x5689002f;
        rrsig->rrsig.inception = 0x56617230;
        rrsig->rrsig.key_tag = 30390;
        rrsig->rrsig.signer = strdup("Nasa.Gov.");
        assert_se(rrsig->rrsig.signer);
        rrsig->rrsig.signature_size = sizeof(signature_blob);
        rrsig->rrsig.signature = memdup(signature_blob, rrsig->rrsig.signature_size);
        assert_se(rrsig->rrsig.signature);

        log_info("RRSIG: %s", strna(dns_resource_record_to_string(rrsig)));

        dnskey = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_DNSKEY, "nASA.gOV");
        assert_se(dnskey);

        dnskey->dnskey.flags = 256;
        dnskey->dnskey.protocol = 3;
        dnskey->dnskey.algorithm = DNSSEC_ALGORITHM_RSASHA256;
        dnskey->dnskey.key_size = sizeof(dnskey_blob);
        dnskey->dnskey.key = memdup(dnskey_blob, sizeof(dnskey_blob));
        assert_se(dnskey->dnskey.key);

        log_info("DNSKEY: %s", strna(dns_resource_record_to_string(dnskey)));
        log_info("DNSKEY keytag: %u", dnssec_keytag(dnskey, false));

        assert_se(dnssec_key_match_rrsig(nsec->key, rrsig) > 0);
        assert_se(dnssec_rrsig_match_dnskey(rrsig, dnskey, false) > 0);

        answer = dns_answer_new(1);
        assert_se(answer);
        assert_se(dns_answer_add(answer, nsec, 0, DNS_ANSWER_AUTHENTICATED, NULL) >= 0);

        assert_se(set_ensure_allocated(&algorithms, NULL) >= 0);

        /* Try to validate the RR as it if was 2015-12-11 today and verify that process fails because set of supported algorithms is empty. */
        assert_se(dnssec_verify_rrset(answer, nsec->key, rrsig, dnskey, 1449849318*USEC_PER_SEC, algorithms, &result) == 0);
        assert_se(result == DNSSEC_UNSUPPORTED_ALGORITHM);

        /* Validate the RR again as it if was 2015-12-11 today but now with populated set of supported algorithms. */
        assert_se(set_put(algorithms, INT_TO_PTR(DNSSEC_ALGORITHM_RSASHA256)) > 0);
        assert_se(dnssec_verify_rrset(answer, nsec->key, rrsig, dnskey, 1449849318*USEC_PER_SEC, algorithms, &result) >= 0);
        assert_se(result == DNSSEC_VALIDATED);

}

TEST(dnssec_verify_rrset3) {
        static const uint8_t signature_blob[] = {
                0x41, 0x09, 0x08, 0x67, 0x51, 0x6d, 0x02, 0xf2, 0x17, 0x1e, 0x61, 0x03, 0xc6, 0x80, 0x7a, 0x82,
                0x8f, 0x6c, 0x8c, 0x4c, 0x68, 0x6f, 0x1c, 0xaa, 0x4a, 0xe0, 0x9b, 0x72, 0xdf, 0x7f, 0x15, 0xfa,
                0x2b, 0xc5, 0x63, 0x6f, 0x52, 0xa2, 0x60, 0x59, 0x24, 0xb6, 0xc3, 0x43, 0x3d, 0x47, 0x38, 0xd8,
                0x0c, 0xcc, 0x6c, 0x10, 0x49, 0x92, 0x97, 0x6c, 0x7d, 0x32, 0xc2, 0x62, 0x83, 0x34, 0x96, 0xdf,
                0xbd, 0xf9, 0xcc, 0xcf, 0xd9, 0x4d, 0x8b, 0x8a, 0xa9, 0x3c, 0x1f, 0x89, 0xc4, 0xad, 0xd5, 0xbb,
                0x74, 0xf8, 0xee, 0x60, 0x54, 0x7a, 0xec, 0x36, 0x45, 0xf2, 0xec, 0xb9, 0x73, 0x66, 0xae, 0x57,
                0x2d, 0xd4, 0x91, 0x02, 0x99, 0xcd, 0xba, 0xbd, 0x6e, 0xfb, 0xa6, 0xf6, 0x34, 0xce, 0x4c, 0x44,
                0x0b, 0xd2, 0x66, 0xdb, 0x4e, 0x5e, 0x00, 0x72, 0x1b, 0xe5, 0x2f, 0x24, 0xd2, 0xc8, 0x72, 0x37,
                0x97, 0x2b, 0xd0, 0xcd, 0xa9, 0x6b, 0x84, 0x32, 0x56, 0x7a, 0x89, 0x6e, 0x3d, 0x8f, 0x03, 0x9a,
                0x9d, 0x6d, 0xf7, 0xe5, 0x13, 0xd7, 0x4b, 0xbc, 0xe2, 0x6c, 0xd1, 0x18, 0x60, 0x0e, 0x1a, 0xe3,
                0xf9, 0xc0, 0x34, 0x4b, 0x1c, 0x82, 0x17, 0x5e, 0xdf, 0x81, 0x32, 0xd7, 0x5b, 0x30, 0x1d, 0xe0,
                0x29, 0x80, 0x6b, 0xb1, 0x69, 0xbf, 0x3f, 0x12, 0x56, 0xb0, 0x80, 0x91, 0x22, 0x1a, 0x31, 0xd5,
                0x5d, 0x3d, 0xdd, 0x70, 0x5e, 0xcb, 0xc7, 0x2d, 0xb8, 0x3e, 0x54, 0x34, 0xd3, 0x50, 0x89, 0x77,
                0x08, 0xc1, 0xf7, 0x11, 0x6e, 0x57, 0xd7, 0x09, 0x94, 0x20, 0x03, 0x38, 0xc3, 0x3a, 0xd3, 0x93,
                0x8f, 0xd0, 0x65, 0xc5, 0xa1, 0xe0, 0x69, 0x2c, 0xf6, 0x0a, 0xce, 0x01, 0xb6, 0x0d, 0x95, 0xa0,
                0x5d, 0x97, 0x94, 0xc3, 0xf1, 0xcd, 0x49, 0xea, 0x20, 0xd3, 0xa9, 0xa6, 0x67, 0x94, 0x64, 0x17
        };

        static const uint8_t dnskey_blob[] = {
                0x03, 0x01, 0x00, 0x01, 0xbf, 0xdd, 0x24, 0x95, 0x21, 0x70, 0xa8, 0x5b, 0x19, 0xa6, 0x76, 0xd3,
                0x5b, 0x37, 0xcf, 0x59, 0x0d, 0x3c, 0xdb, 0x0c, 0xcf, 0xd6, 0x19, 0x02, 0xc7, 0x8e, 0x56, 0x4d,
                0x14, 0xb7, 0x9d, 0x71, 0xf4, 0xdd, 0x24, 0x36, 0xc8, 0x32, 0x1c, 0x63, 0xf7, 0xc0, 0xfc, 0xe3,
                0x83, 0xa6, 0x22, 0x8b, 0x6a, 0x34, 0x41, 0x72, 0xaa, 0x95, 0x98, 0x06, 0xac, 0x03, 0xec, 0xc3,
                0xa1, 0x6d, 0x8b, 0x1b, 0xfd, 0xa4, 0x05, 0x72, 0xe6, 0xe0, 0xb9, 0x98, 0x07, 0x54, 0x7a, 0xb2,
                0x55, 0x30, 0x96, 0xa3, 0x22, 0x3b, 0xe0, 0x9d, 0x61, 0xf6, 0xdc, 0x31, 0x2b, 0xc9, 0x2c, 0x12,
                0x06, 0x7f, 0x3c, 0x5d, 0x29, 0x76, 0x01, 0x62, 0xe3, 0x41, 0x41, 0x4f, 0xa6, 0x07, 0xfa, 0x2d,
                0x0c, 0x64, 0x88, 0xd1, 0x56, 0x18, 0x4b, 0x2b, 0xc2, 0x19, 0x7e, 0xd0, 0x1a, 0x8c, 0x2d, 0x8d,
                0x06, 0xdf, 0x4d, 0xaf, 0xd9, 0xe3, 0x31, 0x59, 0xbc, 0xc3, 0x36, 0x22, 0xe7, 0x15, 0xf9, 0xb2,
                0x44, 0x8a, 0x33, 0xd7, 0x6c, 0xf1, 0xcc, 0x37, 0x05, 0x69, 0x32, 0x71, 0x76, 0xd8, 0x50, 0x06,
                0xae, 0x27, 0xed, 0x3b, 0xdb, 0x1a, 0x97, 0x9b, 0xa3, 0x3e, 0x40, 0x42, 0x29, 0xaf, 0x75, 0x1c,
                0xff, 0x1d, 0xaf, 0x85, 0x02, 0xb3, 0x2e, 0x99, 0x67, 0x08, 0x13, 0xd5, 0xda, 0x6d, 0x65, 0xb2,
                0x36, 0x6f, 0x2f, 0x64, 0xe0, 0xfa, 0xd3, 0x81, 0x86, 0x6b, 0x41, 0x3e, 0x91, 0xaa, 0x0a, 0xd3,
                0xb2, 0x92, 0xd9, 0x42, 0x36, 0x8a, 0x11, 0x0b, 0x5b, 0xb0, 0xea, 0xad, 0x76, 0xd5, 0xb4, 0x81,
                0x30, 0xca, 0x5c, 0x4f, 0xd9, 0xea, 0xe7, 0x4b, 0x10, 0x0a, 0x09, 0x4b, 0x73, 0x66, 0xed, 0x8e,
                0x84, 0xa2, 0x4f, 0x93, 0x7e, 0x29, 0xdc, 0x6a, 0xbd, 0x12, 0xa1, 0x3d, 0xd2, 0xd6, 0x2a, 0x67,
                0x99, 0x4d, 0xf3, 0x43
        };

        _cleanup_(dns_resource_record_unrefp) DnsResourceRecord *mx1 = NULL, *mx2 = NULL, *mx3 = NULL, *mx4 = NULL, *rrsig = NULL, *dnskey = NULL;
        _cleanup_(dns_answer_unrefp) DnsAnswer *answer = NULL;
        _cleanup_set_free_ Set *algorithms = NULL;
        DnssecResult result;

        mx1 = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_MX, "kodapan.se");
        assert_se(mx1);

        mx1->mx.priority = 1;
        mx1->mx.exchange = strdup("ASPMX.L.GOOGLE.COM");
        assert_se(mx1->mx.exchange);

        log_info("MX: %s", strna(dns_resource_record_to_string(mx1)));

        mx2 = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_MX, "kodapan.se");
        assert_se(mx2);

        mx2->mx.priority = 5;
        mx2->mx.exchange = strdup("ALT2.ASPMX.L.GOOGLE.COM");
        assert_se(mx2->mx.exchange);

        log_info("MX: %s", strna(dns_resource_record_to_string(mx2)));

        mx3 = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_MX, "kodapan.se");
        assert_se(mx3);

        mx3->mx.priority = 10;
        mx3->mx.exchange = strdup("ASPMX2.GOOGLEMAIL.COM");
        assert_se(mx3->mx.exchange);

        log_info("MX: %s", strna(dns_resource_record_to_string(mx3)));

        mx4 = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_MX, "kodapan.se");
        assert_se(mx4);

        mx4->mx.priority = 10;
        mx4->mx.exchange = strdup("ASPMX3.GOOGLEMAIL.COM");
        assert_se(mx4->mx.exchange);

        log_info("MX: %s", strna(dns_resource_record_to_string(mx4)));

        rrsig = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_RRSIG, "kodapan.se");
        assert_se(rrsig);

        rrsig->rrsig.type_covered = DNS_TYPE_MX;
        rrsig->rrsig.algorithm = DNSSEC_ALGORITHM_RSASHA256;
        rrsig->rrsig.labels = 2;
        rrsig->rrsig.original_ttl = 900;
        rrsig->rrsig.expiration = 0x5e608a84;
        rrsig->rrsig.inception = 0x5e4e1584;
        rrsig->rrsig.key_tag = 44028;
        rrsig->rrsig.signer = strdup("kodapan.se.");
        assert_se(rrsig->rrsig.signer);
        rrsig->rrsig.signature_size = sizeof(signature_blob);
        rrsig->rrsig.signature = memdup(signature_blob, rrsig->rrsig.signature_size);
        assert_se(rrsig->rrsig.signature);

        log_info("RRSIG: %s", strna(dns_resource_record_to_string(rrsig)));

        dnskey = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_DNSKEY, "kodapan.se");
        assert_se(dnskey);

        dnskey->dnskey.flags = 256;
        dnskey->dnskey.protocol = 3;
        dnskey->dnskey.algorithm = DNSSEC_ALGORITHM_RSASHA256;
        dnskey->dnskey.key_size = sizeof(dnskey_blob);
        dnskey->dnskey.key = memdup(dnskey_blob, sizeof(dnskey_blob));
        assert_se(dnskey->dnskey.key);

        log_info("DNSKEY: %s", strna(dns_resource_record_to_string(dnskey)));
        log_info("DNSKEY keytag: %u", dnssec_keytag(dnskey, false));

        assert_se(dnssec_key_match_rrsig(mx1->key, rrsig) > 0);
        assert_se(dnssec_key_match_rrsig(mx2->key, rrsig) > 0);
        assert_se(dnssec_key_match_rrsig(mx3->key, rrsig) > 0);
        assert_se(dnssec_key_match_rrsig(mx4->key, rrsig) > 0);
        assert_se(dnssec_rrsig_match_dnskey(rrsig, dnskey, false) > 0);

        answer = dns_answer_new(4);
        assert_se(answer);
        assert_se(dns_answer_add(answer, mx1, 0, DNS_ANSWER_AUTHENTICATED, NULL) >= 0);
        assert_se(dns_answer_add(answer, mx2, 0, DNS_ANSWER_AUTHENTICATED, NULL) >= 0);
        assert_se(dns_answer_add(answer, mx3, 0, DNS_ANSWER_AUTHENTICATED, NULL) >= 0);
        assert_se(dns_answer_add(answer, mx4, 0, DNS_ANSWER_AUTHENTICATED, NULL) >= 0);

        assert_se(set_ensure_allocated(&algorithms, NULL) >= 0);
        assert_se(set_put(algorithms, INT_TO_PTR(DNSSEC_ALGORITHM_RSASHA256)) > 0);

        /* Validate the RR as it if was 2020-02-24 today */
        assert_se(dnssec_verify_rrset(answer, mx1->key, rrsig, dnskey, 1582534685*USEC_PER_SEC, algorithms, &result) >= 0);
        assert_se(result == DNSSEC_VALIDATED);
}

TEST(dnssec_nsec3_hash) {
        static const uint8_t salt[] = { 0xB0, 0x1D, 0xFA, 0xCE };
        static const uint8_t next_hashed_name[] = { 0x84, 0x10, 0x26, 0x53, 0xc9, 0xfa, 0x4d, 0x85, 0x6c, 0x97, 0x82, 0xe2, 0x8f, 0xdf, 0x2d, 0x5e, 0x87, 0x69, 0xc4, 0x52 };
        _cleanup_(dns_resource_record_unrefp) DnsResourceRecord *rr = NULL;
        uint8_t h[DNSSEC_HASH_SIZE_MAX];
        _cleanup_free_ char *b = NULL;
        int k;

        /* The NSEC3 RR for eurid.eu on 2015-12-14. */
        rr = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_NSEC3, "PJ8S08RR45VIQDAQGE7EN3VHKNROTBMM.eurid.eu.");
        assert_se(rr);

        rr->nsec3.algorithm = DNSSEC_DIGEST_SHA1;
        rr->nsec3.flags = 1;
        rr->nsec3.iterations = 1;
        rr->nsec3.salt = memdup(salt, sizeof(salt));
        assert_se(rr->nsec3.salt);
        rr->nsec3.salt_size = sizeof(salt);
        rr->nsec3.next_hashed_name = memdup(next_hashed_name, sizeof(next_hashed_name));
        assert_se(rr->nsec3.next_hashed_name);
        rr->nsec3.next_hashed_name_size = sizeof(next_hashed_name);

        log_info("NSEC3: %s", strna(dns_resource_record_to_string(rr)));

        k = dnssec_nsec3_hash(rr, "eurid.eu", &h);
        assert_se(k >= 0);

        b = base32hexmem(h, k, false);
        assert_se(b);
        assert_se(strcasecmp(b, "PJ8S08RR45VIQDAQGE7EN3VHKNROTBMM") == 0);
}

DEFINE_TEST_MAIN(LOG_INFO);
