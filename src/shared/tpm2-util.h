/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stdbool.h>

#include "bitfield.h"
#include "io-util.h"
#include "json.h"
#include "macro.h"
#include "sha256.h"

typedef enum TPM2Flags {
        TPM2_FLAGS_USE_PIN = 1 << 0,
} TPM2Flags;

/* As per https://trustedcomputinggroup.org/wp-content/uploads/TCG_PCClient_PFP_r1p05_v23_pub.pdf a
 * TPM2 on a Client PC must have at least 24 PCRs. This hardcodes our expectation of 24. */
#define TPM2_PCRS_MAX 24U
#define TPM2_PCRS_MASK ((UINT32_C(1) << TPM2_PCRS_MAX) - 1)
static inline bool TPM2_PCR_INDEX_VALID(unsigned pcr) {
        return pcr < TPM2_PCRS_MAX;
}
static inline bool TPM2_PCR_MASK_VALID(uint32_t pcr_mask) {
        return pcr_mask <= TPM2_PCRS_MASK;
}

#define FOREACH_PCR_IN_MASK(pcr, mask) BIT_FOREACH(pcr, mask)

#if HAVE_TPM2

#include <tss2/tss2_esys.h>
#include <tss2/tss2_mu.h>
#include <tss2/tss2_rc.h>

int dlopen_tpm2(void);

typedef struct {
        unsigned n_ref;

        void *tcti_dl;
        TSS2_TCTI_CONTEXT *tcti_context;
        ESYS_CONTEXT *esys_context;

        /* Some selected cached capabilities of the TPM */
        TPMS_ALG_PROPERTY *capability_algorithms;
        size_t n_capability_algorithms;
        TPMA_CC *capability_commands;
        size_t n_capability_commands;
        TPML_PCR_SELECTION capability_pcrs;
} Tpm2Context;

int tpm2_context_new(const char *device, Tpm2Context **ret_context);
Tpm2Context *tpm2_context_ref(Tpm2Context *context);
Tpm2Context *tpm2_context_unref(Tpm2Context *context);
DEFINE_TRIVIAL_CLEANUP_FUNC(Tpm2Context*, tpm2_context_unref);

typedef struct {
        Tpm2Context *tpm2_context;
        ESYS_TR esys_handle;

        bool flush;
} Tpm2Handle;

#define _tpm2_handle(c, h) { .tpm2_context = (c), .esys_handle = (h), }
static const Tpm2Handle TPM2_HANDLE_NONE = _tpm2_handle(NULL, ESYS_TR_NONE);

int tpm2_handle_new(Tpm2Context *context, Tpm2Handle **ret_handle);
Tpm2Handle *tpm2_handle_free(Tpm2Handle *handle);
DEFINE_TRIVIAL_CLEANUP_FUNC(Tpm2Handle*, tpm2_handle_free);

typedef struct {
        unsigned index;
        TPMI_ALG_HASH hash;
        TPM2B_DIGEST value;
} Tpm2PCRValue;

#define TPM2_PCR_VALUE_MAKE(i, h, v) (Tpm2PCRValue) { .index = (i), .hash = (h), .value = ((TPM2B_DIGEST) v), }
bool TPM2_PCR_VALUE_VALID(const Tpm2PCRValue *pcr_value);
int tpm2_pcr_value_from_string(const char *arg, Tpm2PCRValue *ret_pcr_value);
char *tpm2_pcr_value_to_string(const Tpm2PCRValue *pcr_value);

bool TPM2_PCR_VALUES_VALID(const Tpm2PCRValue *pcr_values, size_t n_pcr_values);
void tpm2_sort_pcr_values(Tpm2PCRValue *pcr_values, size_t n_pcr_values);
int tpm2_pcr_values_from_mask(uint32_t mask, TPMI_ALG_HASH hash, Tpm2PCRValue **ret_pcr_values, size_t *ret_n_pcr_values);
int tpm2_pcr_values_to_mask(const Tpm2PCRValue *pcr_values, size_t n_pcr_values, TPMI_ALG_HASH hash, uint32_t *ret_mask);
int tpm2_pcr_values_from_string(const char *arg, Tpm2PCRValue **ret_pcr_values, size_t *ret_n_pcr_values);
char *tpm2_pcr_values_to_string(const Tpm2PCRValue *pcr_values, size_t n_pcr_values);
int tpm2_pcr_values_hash_count(const Tpm2PCRValue *pcr_values, size_t n_pcr_values, size_t *ret_count);
int tpm2_tpml_pcr_selection_from_pcr_values(const Tpm2PCRValue *pcr_values, size_t n_pcr_values, TPML_PCR_SELECTION *ret_selection, TPM2B_DIGEST **ret_values, size_t *ret_n_values);

int tpm2_create_primary(Tpm2Context *c, const Tpm2Handle *session, const TPM2B_PUBLIC *template, const TPM2B_SENSITIVE_CREATE *sensitive, TPM2B_PUBLIC **ret_public, Tpm2Handle **ret_handle);
int tpm2_create(Tpm2Context *c, const Tpm2Handle *parent, const Tpm2Handle *session, const TPMT_PUBLIC *template, const TPMS_SENSITIVE_CREATE *sensitive, TPM2B_PUBLIC **ret_public, TPM2B_PRIVATE **ret_private);
int tpm2_create_loaded(Tpm2Context *c, const Tpm2Handle *parent, const Tpm2Handle *session, const TPMT_PUBLIC *template, const TPMS_SENSITIVE_CREATE *sensitive, TPM2B_PUBLIC **ret_public, TPM2B_PRIVATE **ret_private, Tpm2Handle **ret_handle);

bool tpm2_supports_alg(Tpm2Context *c, TPM2_ALG_ID alg);
bool tpm2_supports_command(Tpm2Context *c, TPM2_CC command);

bool tpm2_test_parms(Tpm2Context *c, TPMI_ALG_PUBLIC alg, const TPMU_PUBLIC_PARMS *parms);

int tpm2_get_good_pcr_banks(Tpm2Context *c, uint32_t pcr_mask, TPMI_ALG_HASH **ret_banks);
int tpm2_get_good_pcr_banks_strv(Tpm2Context *c, uint32_t pcr_mask, char ***ret);

int tpm2_extend_bytes(Tpm2Context *c, char **banks, unsigned pcr_index, const void *data, size_t data_size, const void *secret, size_t secret_size);

uint32_t tpm2_tpms_pcr_selection_to_mask(const TPMS_PCR_SELECTION *s);
void tpm2_tpms_pcr_selection_from_mask(uint32_t mask, TPMI_ALG_HASH hash, TPMS_PCR_SELECTION *ret);
bool tpm2_tpms_pcr_selection_has_mask(const TPMS_PCR_SELECTION *s, uint32_t mask);
void tpm2_tpms_pcr_selection_add_mask(TPMS_PCR_SELECTION *s, uint32_t mask);
void tpm2_tpms_pcr_selection_sub_mask(TPMS_PCR_SELECTION *s, uint32_t mask);
void tpm2_tpms_pcr_selection_add(TPMS_PCR_SELECTION *a, const TPMS_PCR_SELECTION *b);
void tpm2_tpms_pcr_selection_sub(TPMS_PCR_SELECTION *a, const TPMS_PCR_SELECTION *b);
void tpm2_tpms_pcr_selection_move(TPMS_PCR_SELECTION *a, TPMS_PCR_SELECTION *b);
char *tpm2_tpms_pcr_selection_to_string(const TPMS_PCR_SELECTION *s);
size_t tpm2_tpms_pcr_selection_weight(const TPMS_PCR_SELECTION *s);
#define tpm2_tpms_pcr_selection_is_empty(s) (tpm2_tpms_pcr_selection_weight(s) == 0)

uint32_t tpm2_tpml_pcr_selection_to_mask(const TPML_PCR_SELECTION *l, TPMI_ALG_HASH hash);
void tpm2_tpml_pcr_selection_from_mask(uint32_t mask, TPMI_ALG_HASH hash, TPML_PCR_SELECTION *ret);
bool tpm2_tpml_pcr_selection_has_mask(const TPML_PCR_SELECTION *l, TPMI_ALG_HASH hash, uint32_t mask);
void tpm2_tpml_pcr_selection_add_mask(TPML_PCR_SELECTION *l, TPMI_ALG_HASH hash, uint32_t mask);
void tpm2_tpml_pcr_selection_sub_mask(TPML_PCR_SELECTION *l, TPMI_ALG_HASH hash, uint32_t mask);
void tpm2_tpml_pcr_selection_add_tpms_pcr_selection(TPML_PCR_SELECTION *l, const TPMS_PCR_SELECTION *s);
void tpm2_tpml_pcr_selection_sub_tpms_pcr_selection(TPML_PCR_SELECTION *l, const TPMS_PCR_SELECTION *s);
void tpm2_tpml_pcr_selection_add(TPML_PCR_SELECTION *a, const TPML_PCR_SELECTION *b);
void tpm2_tpml_pcr_selection_sub(TPML_PCR_SELECTION *a, const TPML_PCR_SELECTION *b);
char *tpm2_tpml_pcr_selection_to_string(const TPML_PCR_SELECTION *l);
size_t tpm2_tpml_pcr_selection_weight(const TPML_PCR_SELECTION *l);
#define tpm2_tpml_pcr_selection_is_empty(l) (tpm2_tpml_pcr_selection_weight(l) == 0)

int tpm2_digest_many(TPMI_ALG_HASH alg, TPM2B_DIGEST *digest, const struct iovec data[], size_t count, bool extend);
static inline int tpm2_digest_buffer(TPMI_ALG_HASH alg, TPM2B_DIGEST *digest, const void *data, size_t len, bool extend) {
        return tpm2_digest_many(alg, digest, &IOVEC_MAKE((void*) data, len), 1, extend);
}
int tpm2_digest_many_digests(TPMI_ALG_HASH alg, TPM2B_DIGEST *digest, const TPM2B_DIGEST data[], size_t count, bool extend);
static inline int tpm2_digest_rehash(TPMI_ALG_HASH alg, TPM2B_DIGEST *digest) {
        return tpm2_digest_many(alg, digest, NULL, 0, true);
}
static inline int tpm2_digest_init(TPMI_ALG_HASH alg, TPM2B_DIGEST *digest) {
        return tpm2_digest_many(alg, digest, NULL, 0, false);
}

void tpm2_log_debug_tpml_pcr_selection(const TPML_PCR_SELECTION *l, const char *msg);
void tpm2_log_debug_pcr_value(const Tpm2PCRValue *pcr_value, const char *msg);
void tpm2_log_debug_buffer(const void *buffer, size_t size, const char *msg);
void tpm2_log_debug_digest(const TPM2B_DIGEST *digest, const char *msg);
void tpm2_log_debug_name(const TPM2B_NAME *name, const char *msg);

int tpm2_calculate_name(const TPMT_PUBLIC *public, TPM2B_NAME *ret_name);
int tpm2_calculate_policy_auth_value(TPM2B_DIGEST *digest);
int tpm2_calculate_policy_authorize(const TPM2B_PUBLIC *public, const TPM2B_DIGEST *policy_ref, TPM2B_DIGEST *digest);
int tpm2_calculate_policy_pcr(const Tpm2PCRValue *pcr_values, size_t n_pcr_values, TPM2B_DIGEST *digest);

int tpm2_seal(const char *device, uint32_t hash_pcr_mask, const void *pubkey, size_t pubkey_size, uint32_t pubkey_pcr_mask, const char *pin, void **ret_secret, size_t *ret_secret_size, void **ret_blob, size_t *ret_blob_size, void **ret_pcr_hash, size_t *ret_pcr_hash_size, uint16_t *ret_pcr_bank, uint16_t *ret_primary_alg, void **ret_srk_buf, size_t *ret_srk_buf_size);
int tpm2_unseal(const char *device, uint32_t hash_pcr_mask, uint16_t pcr_bank, const void *pubkey, size_t pubkey_size, uint32_t pubkey_pcr_mask, JsonVariant *signature, const char *pin, uint16_t primary_alg, const void *blob, size_t blob_size, const void *policy_hash, size_t policy_hash_size, const void *srk_buf, size_t srk_buf_size, void **ret_secret, size_t *ret_secret_size);

/* The tpm2-tss library has many structs that are simply a combination of an array (or object) and
 * size. These macros allow easily initializing or assigning instances of such structs from an existing
 * buffer/object and size, while also checking the size for safety with the struct buffer/object size. If the
 * provided buffer/object is NULL, the resulting struct's buffer/object will be 0s. If the provided size is
 * larger than the struct's buffer/object size, this results in assertion failure; to check the size, use one
 * of the TPM2B_*_CHECK_SIZE() macros. */
#define TPM2B_AUTH_MAKE(b, s) TPM2B_BUF_SIZE_STRUCT_MAKE(b, s, TPM2B_AUTH, buffer, size)
#define TPM2B_DATA_MAKE(b, s) TPM2B_BUF_SIZE_STRUCT_MAKE(b, s, TPM2B_DATA, buffer, size)
#define TPM2B_DIGEST_MAKE(b, s) TPM2B_BUF_SIZE_STRUCT_MAKE(b, s, TPM2B_DIGEST, buffer, size)
#define TPM2B_ECC_PARAMETER_MAKE(b, s) TPM2B_BUF_SIZE_STRUCT_MAKE(b, s, TPM2B_ECC_PARAMETER, buffer, size)
#define TPM2B_ENCRYPTED_SECRET_MAKE(b, s) TPM2B_BUF_SIZE_STRUCT_MAKE(b, s, TPM2B_ENCRYPTED_SECRET, secret, size)
#define TPM2B_MAX_BUFFER_MAKE(b, s) TPM2B_BUF_SIZE_STRUCT_MAKE(b, s, TPM2B_MAX_BUFFER, buffer, size)
#define TPM2B_NAME_MAKE(b, s) TPM2B_BUF_SIZE_STRUCT_MAKE(b, s, TPM2B_NAME, name, size)
#define TPM2B_PRIVATE_MAKE(b, s) TPM2B_BUF_SIZE_STRUCT_MAKE(b, s, TPM2B_PRIVATE, buffer, size)
#define TPM2B_PRIVATE_KEY_RSA_MAKE(b, s) TPM2B_BUF_SIZE_STRUCT_MAKE(b, s, TPM2B_PRIVATE_KEY_RSA, buffer, size)
#define TPM2B_PUBLIC_KEY_RSA_MAKE(b, s) TPM2B_BUF_SIZE_STRUCT_MAKE(b, s, TPM2B_PUBLIC_KEY_RSA, buffer, size)
#define TPM2B_SENSITIVE_DATA_MAKE(b, s) TPM2B_BUF_SIZE_STRUCT_MAKE(b, s, TPM2B_SENSITIVE_DATA, buffer, size)
#define TPM2B_BUF_SIZE_STRUCT_MAKE(buf, size, struct_type, buffer_field, size_field) \
        _TPM2B_BUF_SIZE_STRUCT_MAKE(buf, size, UNIQ, struct_type, buffer_field, size_field)
#define _TPM2B_BUF_SIZE_STRUCT_MAKE(buf, size, uniq, struct_type, buffer_field, size_field) \
        ({                                                              \
                typeof(buf) UNIQ_T(BUF, uniq) = (buf);                  \
                typeof(size) UNIQ_T(SIZE, uniq) = (size);               \
                struct_type UNIQ_T(STRUCT, uniq) = { .size_field = UNIQ_T(SIZE, uniq), }; \
                assert(sizeof(UNIQ_T(STRUCT, uniq).buffer_field) >= (size_t) UNIQ_T(SIZE, uniq)); \
                if (UNIQ_T(BUF, uniq))                                  \
                        memcpy(UNIQ_T(STRUCT, uniq).buffer_field, UNIQ_T(BUF, uniq), UNIQ_T(SIZE, uniq)); \
                UNIQ_T(STRUCT, uniq);                                   \
        })

/* Check if the size will fit in the TPM2B struct buffer. Returns 0 if the size will fit, otherwise this logs
 * a debug message and returns < 0. */
#define TPM2B_AUTH_CHECK_SIZE(s) TPM2B_BUF_SIZE_STRUCT_CHECK_SIZE(s, TPM2B_AUTH, buffer)
#define TPM2B_DATA_CHECK_SIZE(s) TPM2B_BUF_SIZE_STRUCT_CHECK_SIZE(s, TPM2B_DATA, buffer)
#define TPM2B_DIGEST_CHECK_SIZE(s) TPM2B_BUF_SIZE_STRUCT_CHECK_SIZE(s, TPM2B_DIGEST, buffer)
#define TPM2B_ECC_PARAMETER_CHECK_SIZE(s) TPM2B_BUF_SIZE_STRUCT_CHECK_SIZE(s, TPM2B_ECC_PARAMETER, buffer)
#define TPM2B_ENCRYPTED_SECRET_CHECK_SIZE(s) TPM2B_BUF_SIZE_STRUCT_CHECK_SIZE(s, TPM2B_ENCRYPTED_SECRET, buffer)
#define TPM2B_MAX_BUFFER_CHECK_SIZE(s) TPM2B_BUF_SIZE_STRUCT_CHECK_SIZE(s, TPM2B_MAX_BUFFER, buffer)
#define TPM2B_NAME_CHECK_SIZE(s) TPM2B_BUF_SIZE_STRUCT_CHECK_SIZE(s, TPM2B_NAME, name)
#define TPM2B_PRIVATE_CHECK_SIZE(s) TPM2B_BUF_SIZE_STRUCT_CHECK_SIZE(s, TPM2B_PRIVATE, buffer)
#define TPM2B_PRIVATE_KEY_RSA_CHECK_SIZE(s) TPM2B_BUF_SIZE_STRUCT_CHECK_SIZE(s, TPM2B_PRIVATE_KEY_RSA, buffer)
#define TPM2B_PUBLIC_KEY_RSA_CHECK_SIZE(s) TPM2B_BUF_SIZE_STRUCT_CHECK_SIZE(s, TPM2B_PUBLIC_KEY_RSA, buffer)
#define TPM2B_SENSITIVE_DATA_CHECK_SIZE(s) TPM2B_BUF_SIZE_STRUCT_CHECK_SIZE(s, TPM2B_SENSITIVE_DATA, buffer)
#define TPM2B_BUF_SIZE_STRUCT_CHECK_SIZE(size, struct_type, buffer_field) \
        _TPM2B_BUF_SIZE_STRUCT_CHECK_SIZE(size, UNIQ, struct_type, buffer_field)
#define _TPM2B_BUF_SIZE_STRUCT_CHECK_SIZE(size, uniq, struct_type, buffer_field) \
        ({                                                              \
                size_t UNIQ_T(SIZE, uniq) = (size_t) (size);            \
                size_t UNIQ_T(BUFSIZE, uniq) = sizeof_field(struct_type, buffer_field); \
                UNIQ_T(BUFSIZE, uniq) < UNIQ_T(SIZE, uniq) ?            \
                        log_debug_errno(SYNTHETIC_ERRNO(EINVAL),        \
                                        "Size %zu larger than " #struct_type " buffer size %zu.", \
                                        UNIQ_T(SIZE, uniq), UNIQ_T(BUFSIZE, uniq)) : \
                        0;                                              \
        })

#else /* HAVE_TPM2 */
typedef struct {} Tpm2Context;
typedef struct {} Tpm2Handle;
typedef struct {} Tpm2PCRValue;

#define TPM2_PCR_VALUE_MAKE(i, h, v) (Tpm2PCRValue) {}
#endif /* HAVE_TPM2 */

int tpm2_list_devices(void);
int tpm2_find_device_auto(int log_level, char **ret);

int tpm2_make_pcr_json_array(uint32_t pcr_mask, JsonVariant **ret);
int tpm2_parse_pcr_json_array(JsonVariant *v, uint32_t *ret);

int tpm2_make_luks2_json(int keyslot, uint32_t hash_pcr_mask, uint16_t pcr_bank, const void *pubkey, size_t pubkey_size, uint32_t pubkey_pcr_mask, uint16_t primary_alg, const void *blob, size_t blob_size, const void *policy_hash, size_t policy_hash_size, const void *salt, size_t salt_size, const void *srk_buf, size_t srk_buf_size, TPM2Flags flags, JsonVariant **ret);
int tpm2_parse_luks2_json(JsonVariant *v, int *ret_keyslot, uint32_t *ret_hash_pcr_mask, uint16_t *ret_pcr_bank, void **ret_pubkey, size_t *ret_pubkey_size, uint32_t *ret_pubkey_pcr_mask, uint16_t *ret_primary_alg, void **ret_blob, size_t *ret_blob_size, void **ret_policy_hash, size_t *ret_policy_hash_size, void **ret_salt, size_t *ret_salt_size, void **ret_srk_buf, size_t *ret_srk_buf_size, TPM2Flags *ret_flags);

/* Default to PCR 7 only */
#define TPM2_PCR_MASK_DEFAULT (UINT32_C(1) << 7)

/* We want the helpers below to work also if TPM2 libs are not available, hence define these four defines if
 * they are missing. */
#ifndef TPM2_ALG_SHA1
#define TPM2_ALG_SHA1 0x4
#endif

#ifndef TPM2_ALG_SHA256
#define TPM2_ALG_SHA256 0xB
#endif

#ifndef TPM2_ALG_SHA384
#define TPM2_ALG_SHA384 0xC
#endif

#ifndef TPM2_ALG_SHA512
#define TPM2_ALG_SHA512 0xD
#endif

#ifndef TPM2_ALG_ECC
#define TPM2_ALG_ECC 0x23
#endif

#ifndef TPM2_ALG_RSA
#define TPM2_ALG_RSA 0x1
#endif

int tpm2_hash_alg_to_size(uint16_t alg);

const char *tpm2_hash_alg_to_string(uint16_t alg);
int tpm2_hash_alg_from_string(const char *alg);

const char *tpm2_asym_alg_to_string(uint16_t alg);
int tpm2_asym_alg_from_string(const char *alg);

char *tpm2_pcr_mask_to_string(uint32_t mask);

typedef struct {
        uint32_t search_pcr_mask;
        const char *device;
        const char *signature_path;
} systemd_tpm2_plugin_params;

typedef enum Tpm2Support {
        /* NOTE! The systemd-creds tool returns these flags 1:1 as exit status. Hence these flags are pretty
         * much ABI! Hence, be extra careful when changing/extending these definitions. */
        TPM2_SUPPORT_NONE      = 0,       /* no support */
        TPM2_SUPPORT_FIRMWARE  = 1 << 0,  /* firmware reports TPM2 was used */
        TPM2_SUPPORT_DRIVER    = 1 << 1,  /* the kernel has a driver loaded for it */
        TPM2_SUPPORT_SYSTEM    = 1 << 2,  /* we support it ourselves */
        TPM2_SUPPORT_SUBSYSTEM = 1 << 3,  /* the kernel has the tpm subsystem enabled */
        TPM2_SUPPORT_LIBRARIES = 1 << 4,  /* we can dlopen the tpm2 libraries */
        TPM2_SUPPORT_FULL      = TPM2_SUPPORT_FIRMWARE|TPM2_SUPPORT_DRIVER|TPM2_SUPPORT_SYSTEM|TPM2_SUPPORT_SUBSYSTEM|TPM2_SUPPORT_LIBRARIES,
} Tpm2Support;

enum {
        /* The following names for PCRs 0…7 are based on the names in the "TCG PC Client Specific Platform
         * Firmware Profile Specification"
         * (https://trustedcomputinggroup.org/resource/pc-client-specific-platform-firmware-profile-specification/) */
        PCR_PLATFORM_CODE       = 0,
        PCR_PLATFORM_CONFIG     = 1,
        PCR_EXTERNAL_CODE       = 2,
        PCR_EXTERNAL_CONFIG     = 3,
        PCR_BOOT_LOADER_CODE    = 4,
        PCR_BOOT_LOADER_CONFIG  = 5,
        PCR_SECURE_BOOT_POLICY  = 7,
        /* The following names for PCRs 9…15 are based on the "Linux TPM PCR Registry"
        (https://uapi-group.org/specifications/specs/linux_tpm_pcr_registry/) */
        PCR_KERNEL_INITRD       = 9,
        PCR_IMA                 = 10,
        PCR_KERNEL_BOOT         = 11,
        PCR_KERNEL_CONFIG       = 12,
        PCR_SYSEXTS             = 13,
        PCR_SHIM_POLICY         = 14,
        PCR_SYSTEM_IDENTITY     = 15,
        /* As per "TCG PC Client Specific Platform Firmware Profile Specification" again, see above */
        PCR_DEBUG               = 16,
        PCR_APPLICATION_SUPPORT = 23,
        _PCR_INDEX_MAX_DEFINED  = TPM2_PCRS_MAX,
        _PCR_INDEX_INVALID      = -EINVAL,
};

Tpm2Support tpm2_support(void);

int tpm2_parse_pcr_argument(const char *arg, Tpm2PCRValue **ret_pcr_values, size_t *ret_n_pcr_values);
int tpm2_parse_pcr_argument_append(const char *arg, Tpm2PCRValue **ret_pcr_values, size_t *ret_n_pcr_values);
int tpm2_parse_pcr_argument_to_mask(const char *arg, uint32_t *mask);

int tpm2_load_pcr_signature(const char *path, JsonVariant **ret);
int tpm2_load_pcr_public_key(const char *path, void **ret_pubkey, size_t *ret_pubkey_size);

int tpm2_util_pbkdf2_hmac_sha256(const void *pass,
                    size_t passlen,
                    const void *salt,
                    size_t saltlen,
                    uint8_t res[static SHA256_DIGEST_SIZE]);

int pcr_index_from_string(const char *s) _pure_;
const char *pcr_index_to_string(int pcr) _const_;
