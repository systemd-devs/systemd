/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "dns-type.h"
#include "resolved-dns-question.h"
#include "resolved-dns-rr.h"

#include "log.h"
#include "tests.h"

/* ================================================================
 * dns_question_add()
 * ================================================================ */

TEST(dns_question_add_full) {
        _cleanup_(dns_question_unrefp) DnsQuestion *question = NULL;
        _cleanup_(dns_resource_key_unrefp) DnsResourceKey *key = NULL;

        question = dns_question_new(0);
        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "www.example.com");

        ASSERT_ERROR(dns_question_add(question, key, 0), ENOSPC);

        ASSERT_FALSE(dns_question_contains_key(question, key));

        assert(dns_question_size(question) == 0);
        assert(dns_question_isempty(question) == 1);
}

TEST(dns_question_add_with_space) {
        _cleanup_(dns_question_unrefp) DnsQuestion *question = NULL;
        _cleanup_(dns_resource_key_unrefp) DnsResourceKey *key = NULL;

        assert(dns_question_size(question) == 0);
        assert(dns_question_isempty(question) == 1);

        question = dns_question_new(1);
        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "www.example.com");

        ASSERT_OK(dns_question_add(question, key, 0));

        ASSERT_TRUE(dns_question_contains_key(question, key));

        assert(dns_question_size(question) == 1);
        assert(dns_question_isempty(question) == 0);
}

/* ================================================================
 * dns_question_new_address()
 * ================================================================ */

TEST(dns_question_new_address_ipv4) {
        _cleanup_(dns_question_unrefp) DnsQuestion *question = NULL;
        _cleanup_(dns_resource_key_unrefp) DnsResourceKey *key = NULL;

        ASSERT_OK(dns_question_new_address(&question, AF_INET, "www.example.com", 0));
        ASSERT_NOT_NULL(question);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "www.example.com");

        ASSERT_EQ(dns_question_size(question), 1u);
        ASSERT_TRUE(dns_question_contains_key(question, key));
}

TEST(dns_question_new_address_ipv6) {
        _cleanup_(dns_question_unrefp) DnsQuestion *question = NULL;
        _cleanup_(dns_resource_key_unrefp) DnsResourceKey *key = NULL;

        ASSERT_OK(dns_question_new_address(&question, AF_INET6, "www.example.com", 0));
        ASSERT_NOT_NULL(question);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_AAAA, "www.example.com");

        ASSERT_EQ(dns_question_size(question), 1u);
        ASSERT_TRUE(dns_question_contains_key(question, key));
}

TEST(dns_question_new_address_convert_idna) {
        _cleanup_(dns_question_unrefp) DnsQuestion *question = NULL;
        _cleanup_(dns_resource_key_unrefp) DnsResourceKey *key = NULL;

        ASSERT_OK(dns_question_new_address(&question, AF_INET, "www.\xF0\x9F\x98\xB1.com", 1));
        ASSERT_NOT_NULL(question);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "www.xn--s38h.com");

        ASSERT_EQ(dns_question_size(question), 1u);
        ASSERT_TRUE(dns_question_contains_key(question, key));
}

/* ================================================================
 * dns_question_new_reverse()
 * ================================================================ */

TEST(dns_question_new_reverse_ipv4) {
        _cleanup_(dns_question_unrefp) DnsQuestion *question = NULL;
        _cleanup_(dns_resource_key_unrefp) DnsResourceKey *key = NULL;

        union in_addr_union addr = { .in.s_addr = htobe32(0xc0a8017f) };

        ASSERT_OK(dns_question_new_reverse(&question, AF_INET, &addr));
        ASSERT_NOT_NULL(question);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_PTR, "127.1.168.192.in-addr.arpa");

        ASSERT_EQ(dns_question_size(question), 1u);
        ASSERT_TRUE(dns_question_contains_key(question, key));
}

/* ================================================================
 * dns_question_new_service()
 * ================================================================ */

TEST(dns_question_new_service_no_domain) {
        _cleanup_(dns_question_unrefp) DnsQuestion *question = NULL;

        ASSERT_ERROR(dns_question_new_service(&question, NULL, "_xmpp._tcp", NULL, 0, 0), EINVAL);
        ASSERT_NULL(question);
}

TEST(dns_question_new_service_domain_only) {
        _cleanup_(dns_question_unrefp) DnsQuestion *question = NULL;
        _cleanup_(dns_resource_key_unrefp) DnsResourceKey *key = NULL;

        ASSERT_OK(dns_question_new_service(&question, NULL, NULL, "www.example.com", 0, 0));
        ASSERT_NOT_NULL(question);
        ASSERT_EQ(dns_question_size(question), 1u);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_SRV, "www.example.com");
        ASSERT_TRUE(dns_question_contains_key(question, key));
}

TEST(dns_question_new_service_domain_ignores_idna) {
        _cleanup_(dns_question_unrefp) DnsQuestion *question = NULL;
        _cleanup_(dns_resource_key_unrefp) DnsResourceKey *key = NULL;

        ASSERT_OK(dns_question_new_service(&question, NULL, NULL, "\xF0\x9F\x98\xB1.com", 0, 1));
        ASSERT_NOT_NULL(question);
        ASSERT_EQ(dns_question_size(question), 1u);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_SRV, "\xF0\x9F\x98\xB1.com");
        ASSERT_TRUE(dns_question_contains_key(question, key));
}

TEST(dns_question_new_service_with_type) {
        _cleanup_(dns_question_unrefp) DnsQuestion *question = NULL;
        _cleanup_(dns_resource_key_unrefp) DnsResourceKey *key = NULL;

        ASSERT_OK(dns_question_new_service(&question, NULL, "_xmpp._tcp", "example.com", 0, 0));
        ASSERT_NOT_NULL(question);
        ASSERT_EQ(dns_question_size(question), 1u);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_SRV, "_xmpp._tcp.example.com");
        ASSERT_TRUE(dns_question_contains_key(question, key));
}

TEST(dns_question_new_service_with_type_applies_idna) {
        _cleanup_(dns_question_unrefp) DnsQuestion *question = NULL;
        _cleanup_(dns_resource_key_unrefp) DnsResourceKey *key = NULL;

        ASSERT_OK(dns_question_new_service(&question, NULL, "_xmpp._tcp", "\xF0\x9F\x98\xB1.com", 0, 1));
        ASSERT_NOT_NULL(question);
        ASSERT_EQ(dns_question_size(question), 1u);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_SRV, "_xmpp._tcp.xn--s38h.com");
        ASSERT_TRUE(dns_question_contains_key(question, key));
}

TEST(dns_question_new_service_with_type_with_txt) {
        _cleanup_(dns_question_unrefp) DnsQuestion *question = NULL;
        DnsResourceKey *key = NULL;

        ASSERT_OK(dns_question_new_service(&question, NULL, "_xmpp._tcp", "\xF0\x9F\x98\xB1.com", 1, 1));
        ASSERT_NOT_NULL(question);
        ASSERT_EQ(dns_question_size(question), 2u);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_SRV, "_xmpp._tcp.xn--s38h.com");
        ASSERT_TRUE(dns_question_contains_key(question, key));
        dns_resource_key_unref(key);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_TXT, "_xmpp._tcp.xn--s38h.com");
        ASSERT_TRUE(dns_question_contains_key(question, key));
        dns_resource_key_unref(key);
}

TEST(dns_question_new_service_with_invalid_type) {
        _cleanup_(dns_question_unrefp) DnsQuestion *question = NULL;

        ASSERT_ERROR(dns_question_new_service(&question, NULL, "_xmpp.tcp", "example.com", 0, 0), EINVAL);
        ASSERT_NULL(question);
}

TEST(dns_question_new_service_with_type_too_short) {
        _cleanup_(dns_question_unrefp) DnsQuestion *question = NULL;

        ASSERT_ERROR(dns_question_new_service(&question, NULL, "_xmpp", "example.com", 0, 0), EINVAL);
        ASSERT_NULL(question);
}

TEST(dns_question_new_service_with_type_too_long) {
        _cleanup_(dns_question_unrefp) DnsQuestion *question = NULL;

        ASSERT_ERROR(dns_question_new_service(&question, NULL, "_xmpp._tcp._extra", "example.com", 0, 0), EINVAL);
        ASSERT_NULL(question);
}

TEST(dns_question_new_service_with_service_and_type) {
        _cleanup_(dns_question_unrefp) DnsQuestion *question = NULL;
        _cleanup_(dns_resource_key_unrefp) DnsResourceKey *key = NULL;

        ASSERT_OK(dns_question_new_service(&question, "service", "_xmpp._tcp", "example.com", 0, 0));
        ASSERT_NOT_NULL(question);
        ASSERT_EQ(dns_question_size(question), 1u);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_SRV, "service._xmpp._tcp.example.com");
        ASSERT_TRUE(dns_question_contains_key(question, key));
}

TEST(dns_question_new_service_with_service_no_type) {
        _cleanup_(dns_question_unrefp) DnsQuestion *question = NULL;

        ASSERT_ERROR(dns_question_new_service(&question, "service", NULL, "example.com", 0, 0), EINVAL);
        ASSERT_NULL(question);
}

/* ================================================================
 * dns_question_matches_rr()
 * ================================================================ */

TEST(dns_question_matches_rr_first) {
        _cleanup_(dns_question_unrefp) DnsQuestion *question = NULL;
        DnsResourceKey *key = NULL;
        _cleanup_(dns_resource_record_unrefp) DnsResourceRecord *rr = NULL;

        question = dns_question_new(2);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "www.example.com");
        dns_question_add(question, key, 0);
        dns_resource_key_unref(key);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "mail.example.com");
        dns_question_add(question, key, 0);
        dns_resource_key_unref(key);

        rr = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_A, "www.example.com");
        ASSERT_TRUE(dns_question_matches_rr(question, rr, NULL));
}

TEST(dns_question_matches_rr_second) {
        _cleanup_(dns_question_unrefp) DnsQuestion *question = NULL;
        DnsResourceKey *key = NULL;
        _cleanup_(dns_resource_record_unrefp) DnsResourceRecord *rr = NULL;

        question = dns_question_new(2);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "www.example.com");
        dns_question_add(question, key, 0);
        dns_resource_key_unref(key);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "mail.example.com");
        dns_question_add(question, key, 0);
        dns_resource_key_unref(key);

        rr = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_A, "mail.example.com");
        ASSERT_TRUE(dns_question_matches_rr(question, rr, NULL));
}

TEST(dns_question_matches_rr_fail) {
        _cleanup_(dns_question_unrefp) DnsQuestion *question = NULL;
        DnsResourceKey *key = NULL;
        _cleanup_(dns_resource_record_unrefp) DnsResourceRecord *rr = NULL;

        question = dns_question_new(2);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "www.example.com");
        dns_question_add(question, key, 0);
        dns_resource_key_unref(key);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "mail.example.com");
        dns_question_add(question, key, 0);
        dns_resource_key_unref(key);

        rr = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_CNAME, "mail.example.com");
        ASSERT_FALSE(dns_question_matches_rr(question, rr, NULL));
}

/* ================================================================
 * dns_question_matches_cname_or_dname()
 * ================================================================ */

TEST(dns_question_matches_cname) {
        _cleanup_(dns_question_unrefp) DnsQuestion *question = NULL;
        _cleanup_(dns_resource_key_unrefp) DnsResourceKey *key = NULL;
        _cleanup_(dns_resource_record_unrefp) DnsResourceRecord *rr = NULL;

        question = dns_question_new(1);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "www.example.com");
        dns_question_add(question, key, 0);

        rr = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_CNAME, "www.example.com");
        ASSERT_TRUE(dns_question_matches_cname_or_dname(question, rr, NULL));
}

TEST(dns_question_matches_dname) {
        _cleanup_(dns_question_unrefp) DnsQuestion *question = NULL;
        _cleanup_(dns_resource_key_unrefp) DnsResourceKey *key = NULL;
        _cleanup_(dns_resource_record_unrefp) DnsResourceRecord *rr = NULL;

        question = dns_question_new(1);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "www.example.com");
        dns_question_add(question, key, 0);

        rr = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_DNAME, "example.com");
        ASSERT_TRUE(dns_question_matches_cname_or_dname(question, rr, NULL));
}

TEST(dns_question_matches_cname_or_dname_fail) {
        _cleanup_(dns_question_unrefp) DnsQuestion *question = NULL;
        _cleanup_(dns_resource_key_unrefp) DnsResourceKey *key = NULL;
        _cleanup_(dns_resource_record_unrefp) DnsResourceRecord *rr = NULL;

        question = dns_question_new(1);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "www.example.com");
        dns_question_add(question, key, 0);

        rr = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_A, "www.example.com");
        ASSERT_FALSE(dns_question_matches_cname_or_dname(question, rr, NULL));
}

TEST(dns_question_matches_cname_or_dname_all_must_redirect) {
        _cleanup_(dns_question_unrefp) DnsQuestion *question = NULL;
        DnsResourceKey *key = NULL;
        _cleanup_(dns_resource_record_unrefp) DnsResourceRecord *rr = NULL;

        question = dns_question_new(2);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_CNAME, "example.com");
        dns_question_add(question, key, 0);
        dns_resource_key_unref(key);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "www.example.com");
        dns_question_add(question, key, 0);
        dns_resource_key_unref(key);

        rr = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_CNAME, "www.example.com");
        ASSERT_FALSE(dns_question_matches_cname_or_dname(question, rr, NULL));
}

/* ================================================================
 * dns_question_is_valid_for_query()
 * ================================================================ */

TEST(dns_question_is_valid_for_query_empty) {
        _cleanup_(dns_question_unrefp) DnsQuestion *question = NULL;

        question = dns_question_new(0);
        ASSERT_FALSE(dns_question_is_valid_for_query(question));
}

TEST(dns_question_is_valid_for_query_single) {
        _cleanup_(dns_question_unrefp) DnsQuestion *question = NULL;
        _cleanup_(dns_resource_key_unrefp) DnsResourceKey *key = NULL;

        question = dns_question_new(1);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "www.example.com");
        dns_question_add(question, key, 0);

        ASSERT_TRUE(dns_question_is_valid_for_query(question));
}

TEST(dns_question_is_valid_for_query_invalid_type) {
        _cleanup_(dns_question_unrefp) DnsQuestion *question = NULL;
        _cleanup_(dns_resource_key_unrefp) DnsResourceKey *key = NULL;

        question = dns_question_new(1);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_OPT, "www.example.com");
        dns_question_add(question, key, 0);

        ASSERT_FALSE(dns_question_is_valid_for_query(question));
}

TEST(dns_question_is_valid_for_query_multi_same_name) {
        _cleanup_(dns_question_unrefp) DnsQuestion *question = NULL;
        DnsResourceKey *key = NULL;

        question = dns_question_new(2);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "www.example.com");
        dns_question_add(question, key, 0);
        dns_resource_key_unref(key);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_AAAA, "www.EXAMPLE.com");
        dns_question_add(question, key, 0);
        dns_resource_key_unref(key);

        ASSERT_TRUE(dns_question_is_valid_for_query(question));
}

TEST(dns_question_is_valid_for_query_multi_different_names) {
        _cleanup_(dns_question_unrefp) DnsQuestion *question = NULL;
        DnsResourceKey *key = NULL;

        question = dns_question_new(2);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "www.example.com");
        dns_question_add(question, key, 0);
        dns_resource_key_unref(key);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_AAAA, "www.example.org");
        dns_question_add(question, key, 0);
        dns_resource_key_unref(key);

        ASSERT_FALSE(dns_question_is_valid_for_query(question));
}

/* ================================================================
 * dns_question_is_equal()
 * ================================================================ */

TEST(dns_question_is_equal_same_pointer) {
        _cleanup_(dns_question_unrefp) DnsQuestion *a = NULL;

        a = dns_question_new(0);
        ASSERT_TRUE(dns_question_is_equal(a, a));
}

TEST(dns_question_is_equal_both_empty) {
        _cleanup_(dns_question_unrefp) DnsQuestion *a = NULL, *b = NULL;

        a = dns_question_new(0);
        b = dns_question_new(0);

        ASSERT_TRUE(dns_question_is_equal(a, b));
}

TEST(dns_question_is_equal_single) {
        _cleanup_(dns_question_unrefp) DnsQuestion *a = NULL, *b = NULL;
        DnsResourceKey *key = NULL;

        a = dns_question_new(1);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "www.example.com");
        dns_question_add(a, key, 0);
        dns_resource_key_unref(key);

        b = dns_question_new(1);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "www.EXAMPLE.com");
        dns_question_add(b, key, 0);
        dns_resource_key_unref(key);

        ASSERT_TRUE(dns_question_is_equal(a, b));
}

TEST(dns_question_is_equal_different_names) {
        _cleanup_(dns_question_unrefp) DnsQuestion *a = NULL, *b = NULL;
        DnsResourceKey *key = NULL;

        a = dns_question_new(1);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "www.example.com");
        dns_question_add(a, key, 0);
        dns_resource_key_unref(key);

        b = dns_question_new(1);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "www.example.org");
        dns_question_add(b, key, 0);
        dns_resource_key_unref(key);

        ASSERT_FALSE(dns_question_is_equal(a, b));
}

TEST(dns_question_is_equal_different_types) {
        _cleanup_(dns_question_unrefp) DnsQuestion *a = NULL, *b = NULL;
        DnsResourceKey *key = NULL;

        a = dns_question_new(1);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "www.example.com");
        dns_question_add(a, key, 0);
        dns_resource_key_unref(key);

        b = dns_question_new(1);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_AAAA, "www.example.com");
        dns_question_add(b, key, 0);
        dns_resource_key_unref(key);

        ASSERT_FALSE(dns_question_is_equal(a, b));
}

TEST(dns_question_is_equal_first_larger) {
        _cleanup_(dns_question_unrefp) DnsQuestion *a = NULL, *b = NULL;
        DnsResourceKey *key = NULL;

        a = dns_question_new(2);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "www.example.com");
        dns_question_add(a, key, 0);
        dns_resource_key_unref(key);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_AAAA, "www.example.com");
        dns_question_add(a, key, 0);
        dns_resource_key_unref(key);

        b = dns_question_new(1);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "www.example.com");
        dns_question_add(b, key, 0);
        dns_resource_key_unref(key);

        ASSERT_FALSE(dns_question_is_equal(a, b));
}

TEST(dns_question_is_equal_second_larger) {
        _cleanup_(dns_question_unrefp) DnsQuestion *a = NULL, *b = NULL;
        DnsResourceKey *key = NULL;

        a = dns_question_new(1);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "www.example.com");
        dns_question_add(a, key, 0);
        dns_resource_key_unref(key);

        b = dns_question_new(2);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "www.example.com");
        dns_question_add(b, key, 0);
        dns_resource_key_unref(key);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_AAAA, "www.example.com");
        dns_question_add(b, key, 0);
        dns_resource_key_unref(key);

        ASSERT_FALSE(dns_question_is_equal(a, b));
}

TEST(dns_question_is_equal_different_order) {
        _cleanup_(dns_question_unrefp) DnsQuestion *a = NULL, *b = NULL;
        DnsResourceKey *key = NULL;

        a = dns_question_new(2);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_AAAA, "www.example.com");
        dns_question_add(a, key, 0);
        dns_resource_key_unref(key);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "www.example.com");
        dns_question_add(a, key, 0);
        dns_resource_key_unref(key);

        b = dns_question_new(2);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "www.example.com");
        dns_question_add(b, key, 0);
        dns_resource_key_unref(key);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_AAAA, "www.example.com");
        dns_question_add(b, key, 0);
        dns_resource_key_unref(key);

        ASSERT_TRUE(dns_question_is_equal(a, b));
}

/* ================================================================
 * dns_question_cname_redirect()
 * ================================================================ */

TEST(dns_question_cname_redirect_empty) {
        _cleanup_(dns_question_unrefp) DnsQuestion *question = NULL, *ret = NULL;
        _cleanup_(dns_resource_record_unrefp) DnsResourceRecord *rr = NULL;

        question = dns_question_new(0);

        rr = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_CNAME, "www.example.com");
        rr->cname.name = strdup("example.com");

        ASSERT_FALSE(dns_question_cname_redirect(question, rr, &ret));
        ASSERT_NULL(ret);
}

TEST(dns_question_cname_redirect_single_cname_match) {
        _cleanup_(dns_question_unrefp) DnsQuestion *question = NULL, *ret = NULL;
        DnsResourceKey *key = NULL;
        _cleanup_(dns_resource_record_unrefp) DnsResourceRecord *rr = NULL;

        question = dns_question_new(1);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "www.example.com");
        dns_question_add(question, key, 0);
        dns_resource_key_unref(key);

        rr = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_CNAME, "www.example.com");
        rr->cname.name = strdup("example.com");

        ASSERT_TRUE(dns_question_cname_redirect(question, rr, &ret));

        ASSERT_NOT_NULL(ret);
        ASSERT_TRUE(question != ret);
        ASSERT_FALSE(dns_question_is_equal(question, ret));

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "www.example.com");
        ASSERT_TRUE(dns_question_contains_key(question, key));
        dns_resource_key_unref(key);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "example.com");
        ASSERT_TRUE(dns_question_contains_key(ret, key));
        dns_resource_key_unref(key);
}

TEST(dns_question_cname_redirect_single_cname_no_change) {
        _cleanup_(dns_question_unrefp) DnsQuestion *question = NULL, *ret = NULL;
        DnsResourceKey *key = NULL;
        _cleanup_(dns_resource_record_unrefp) DnsResourceRecord *rr = NULL;

        question = dns_question_new(1);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "example.com");
        dns_question_add(question, key, 0);
        dns_resource_key_unref(key);

        rr = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_CNAME, "www.example.com");
        rr->cname.name = strdup("example.com");

        ASSERT_FALSE(dns_question_cname_redirect(question, rr, &ret));
        ASSERT_NULL(ret);
}

TEST(dns_question_cname_redirect_single_cname_no_match) {
        _cleanup_(dns_question_unrefp) DnsQuestion *question = NULL, *ret = NULL;
        DnsResourceKey *key = NULL;
        _cleanup_(dns_resource_record_unrefp) DnsResourceRecord *rr = NULL;

        question = dns_question_new(1);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "mail.example.com");
        dns_question_add(question, key, 0);
        dns_resource_key_unref(key);

        rr = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_CNAME, "www.example.com");
        rr->cname.name = strdup("example.com");

        ASSERT_TRUE(dns_question_cname_redirect(question, rr, &ret));

        ASSERT_NOT_NULL(ret);
        ASSERT_TRUE(question != ret);
        ASSERT_FALSE(dns_question_is_equal(question, ret));

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "mail.example.com");
        ASSERT_TRUE(dns_question_contains_key(question, key));
        dns_resource_key_unref(key);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "example.com");
        ASSERT_TRUE(dns_question_contains_key(ret, key));
        dns_resource_key_unref(key);
}

TEST(dns_question_cname_redirect_single_dname_match) {
        _cleanup_(dns_question_unrefp) DnsQuestion *question = NULL, *ret = NULL;
        DnsResourceKey *key = NULL;
        _cleanup_(dns_resource_record_unrefp) DnsResourceRecord *rr = NULL;

        question = dns_question_new(1);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "www.example.com");
        dns_question_add(question, key, 0);
        dns_resource_key_unref(key);

        rr = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_DNAME, "example.com");
        rr->cname.name = strdup("v2.example.com");

        ASSERT_TRUE(dns_question_cname_redirect(question, rr, &ret));

        ASSERT_NOT_NULL(ret);
        ASSERT_TRUE(question != ret);
        ASSERT_FALSE(dns_question_is_equal(question, ret));

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "www.example.com");
        ASSERT_TRUE(dns_question_contains_key(question, key));
        dns_resource_key_unref(key);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "www.v2.example.com");
        ASSERT_TRUE(dns_question_contains_key(ret, key));
        dns_resource_key_unref(key);
}

TEST(dns_question_cname_redirect_multi_dname_match) {
        _cleanup_(dns_question_unrefp) DnsQuestion *question = NULL, *ret = NULL;
        DnsResourceKey *key = NULL;
        _cleanup_(dns_resource_record_unrefp) DnsResourceRecord *rr = NULL;

        question = dns_question_new(2);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "www.example.com");
        dns_question_add(question, key, 0);
        dns_resource_key_unref(key);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "mail.example.com");
        dns_question_add(question, key, 0);
        dns_resource_key_unref(key);

        rr = dns_resource_record_new_full(DNS_CLASS_IN, DNS_TYPE_DNAME, "example.com");
        rr->cname.name = strdup("v2.example.com");

        ASSERT_TRUE(dns_question_cname_redirect(question, rr, &ret));

        ASSERT_NOT_NULL(ret);
        ASSERT_TRUE(question != ret);
        ASSERT_FALSE(dns_question_is_equal(question, ret));

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "www.v2.example.com");
        ASSERT_TRUE(dns_question_contains_key(ret, key));
        dns_resource_key_unref(key);

        key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, "mail.v2.example.com");
        ASSERT_TRUE(dns_question_contains_key(ret, key));
        dns_resource_key_unref(key);
}

DEFINE_TEST_MAIN(LOG_DEBUG);
