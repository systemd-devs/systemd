/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stdbool.h>

#include "bitfield.h"
#include "json.h"
#include "macro.h"
#include "sha256.h"

typedef enum TPM2Flags {
        TPM2_FLAGS_USE_PIN = 1 << 0,
} TPM2Flags;


typedef enum Tpm2SRKTemplateFlags {
        TPM2_SRK_TEMPLATE_ECC       = 1 << 0,
        TPM2_SRK_TEMPLATE_NEW_STYLE = 1 << 1,
        _TPM2_SRK_TEMPLATE_MAX      = TPM2_SRK_TEMPLATE_NEW_STYLE|TPM2_SRK_TEMPLATE_ECC,
} Tpm2SRKTemplateFlags;

/* As per https://trustedcomputinggroup.org/wp-content/uploads/TCG_PCClient_PFP_r1p05_v23_pub.pdf a
 * TPM2 on a Client PC must have at least 24 PCRs. This hardcodes our expectation of 24. */
#define TPM2_PCRS_MAX 24U
#define TPM2_PCRS_MASK ((UINT32_C(1) << TPM2_PCRS_MAX) - 1)
static inline bool TPM2_PCR_VALID(unsigned pcr) {
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

extern TSS2_RC (*sym_Esys_Create)(ESYS_CONTEXT *esysContext, ESYS_TR parentHandle, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPM2B_SENSITIVE_CREATE *inSensitive, const TPM2B_PUBLIC *inPublic, const TPM2B_DATA *outsideInfo, const TPML_PCR_SELECTION *creationPCR, TPM2B_PRIVATE **outPrivate, TPM2B_PUBLIC **outPublic, TPM2B_CREATION_DATA **creationData, TPM2B_DIGEST **creationHash, TPMT_TK_CREATION **creationTicket);
extern TSS2_RC (*sym_Esys_CreatePrimary)(ESYS_CONTEXT *esysContext, ESYS_TR primaryHandle, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPM2B_SENSITIVE_CREATE *inSensitive, const TPM2B_PUBLIC *inPublic, const TPM2B_DATA *outsideInfo, const TPML_PCR_SELECTION *creationPCR, ESYS_TR *objectHandle, TPM2B_PUBLIC **outPublic, TPM2B_CREATION_DATA **creationData, TPM2B_DIGEST **creationHash, TPMT_TK_CREATION **creationTicket);
extern void (*sym_Esys_Finalize)(ESYS_CONTEXT **context);
extern TSS2_RC (*sym_Esys_FlushContext)(ESYS_CONTEXT *esysContext, ESYS_TR flushHandle);
extern void (*sym_Esys_Free)(void *ptr);
extern TSS2_RC (*sym_Esys_GetCapability)(ESYS_CONTEXT *esysContext, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, TPM2_CAP capability, UINT32 property, UINT32 propertyCount, TPMI_YES_NO *moreData, TPMS_CAPABILITY_DATA **capabilityData);
extern TSS2_RC (*sym_Esys_GetRandom)(ESYS_CONTEXT *esysContext, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, UINT16 bytesRequested, TPM2B_DIGEST **randomBytes);
extern TSS2_RC (*sym_Esys_Initialize)(ESYS_CONTEXT **esys_context,  TSS2_TCTI_CONTEXT *tcti, TSS2_ABI_VERSION *abiVersion);
extern TSS2_RC (*sym_Esys_Load)(ESYS_CONTEXT *esysContext, ESYS_TR parentHandle, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPM2B_PRIVATE *inPrivate, const TPM2B_PUBLIC *inPublic, ESYS_TR *objectHandle);
extern TSS2_RC (*sym_Esys_LoadExternal)(ESYS_CONTEXT *esysContext, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPM2B_SENSITIVE *inPrivate, const TPM2B_PUBLIC *inPublic, ESYS_TR hierarchy, ESYS_TR *objectHandle);
extern TSS2_RC (*sym_Esys_PCR_Extend)(ESYS_CONTEXT *esysContext, ESYS_TR pcrHandle, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPML_DIGEST_VALUES *digests);
extern TSS2_RC (*sym_Esys_PCR_Read)(ESYS_CONTEXT *esysContext, ESYS_TR shandle1,ESYS_TR shandle2, ESYS_TR shandle3, const TPML_PCR_SELECTION *pcrSelectionIn, UINT32 *pcrUpdateCounter, TPML_PCR_SELECTION **pcrSelectionOut, TPML_DIGEST **pcrValues);
extern TSS2_RC (*sym_Esys_PolicyAuthorize)(ESYS_CONTEXT *esysContext, ESYS_TR policySession, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPM2B_DIGEST *approvedPolicy, const TPM2B_NONCE *policyRef, const TPM2B_NAME *keySign, const TPMT_TK_VERIFIED *checkTicket);
extern TSS2_RC (*sym_Esys_PolicyAuthValue)(ESYS_CONTEXT *esysContext, ESYS_TR policySession, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3);
extern TSS2_RC (*sym_Esys_PolicyGetDigest)(ESYS_CONTEXT *esysContext, ESYS_TR policySession, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, TPM2B_DIGEST **policyDigest);
extern TSS2_RC (*sym_Esys_PolicyPCR)(ESYS_CONTEXT *esysContext, ESYS_TR policySession, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPM2B_DIGEST *pcrDigest, const TPML_PCR_SELECTION *pcrs);
extern TSS2_RC (*sym_Esys_StartAuthSession)(ESYS_CONTEXT *esysContext, ESYS_TR tpmKey, ESYS_TR bind, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPM2B_NONCE *nonceCaller, TPM2_SE sessionType, const TPMT_SYM_DEF *symmetric, TPMI_ALG_HASH authHash, ESYS_TR *sessionHandle);
extern TSS2_RC (*sym_Esys_Startup)(ESYS_CONTEXT *esysContext, TPM2_SU startupType);
extern TSS2_RC (*sym_Esys_TRSess_GetAttributes)(ESYS_CONTEXT *esysContext, ESYS_TR session, TPMA_SESSION *flags);
extern TSS2_RC (*sym_Esys_TRSess_SetAttributes)(ESYS_CONTEXT *esysContext, ESYS_TR session, TPMA_SESSION flags, TPMA_SESSION mask);
extern TSS2_RC (*sym_Esys_TR_GetName)(ESYS_CONTEXT *esysContext, ESYS_TR handle, TPM2B_NAME **name);
extern TSS2_RC (*sym_Esys_TR_SetAuth)(ESYS_CONTEXT *esysContext, ESYS_TR handle, TPM2B_AUTH const *authValue);
extern TSS2_RC (*sym_Esys_Unseal)(ESYS_CONTEXT *esysContext, ESYS_TR itemHandle, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, TPM2B_SENSITIVE_DATA **outData);
extern TSS2_RC (*sym_Esys_VerifySignature)(ESYS_CONTEXT *esysContext, ESYS_TR keyHandle, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPM2B_DIGEST *digest, const TPMT_SIGNATURE *signature, TPMT_TK_VERIFIED **validation);

extern const char* (*sym_Tss2_RC_Decode)(TSS2_RC rc);

extern TSS2_RC (*sym_Tss2_MU_TPM2_CC_Marshal)(TPM2_CC src, uint8_t buffer[], size_t buffer_size, size_t *offset);
extern TSS2_RC (*sym_Tss2_MU_TPM2B_PRIVATE_Marshal)(TPM2B_PRIVATE const *src, uint8_t buffer[], size_t buffer_size, size_t *offset);
extern TSS2_RC (*sym_Tss2_MU_TPM2B_PRIVATE_Unmarshal)(uint8_t const buffer[], size_t buffer_size, size_t *offset, TPM2B_PRIVATE  *dest);
extern TSS2_RC (*sym_Tss2_MU_TPM2B_PUBLIC_Marshal)(TPM2B_PUBLIC const *src, uint8_t buffer[], size_t buffer_size, size_t *offset);
extern TSS2_RC (*sym_Tss2_MU_TPM2B_PUBLIC_Unmarshal)(uint8_t const buffer[], size_t buffer_size, size_t *offset, TPM2B_PUBLIC *dest);
extern TSS2_RC (*sym_Tss2_MU_TPML_PCR_SELECTION_Marshal)(TPML_PCR_SELECTION const *src, uint8_t buffer[], size_t buffer_size, size_t *offset);
extern TSS2_RC (*sym_Tss2_MU_TPMT_HA_Marshal)(TPMT_HA const *src, uint8_t buffer[], size_t buffer_size, size_t *offset);
extern TSS2_RC (*sym_Tss2_MU_TPMT_PUBLIC_Marshal)(TPMT_PUBLIC const *src, uint8_t buffer[], size_t buffer_size, size_t *offset);

int dlopen_tpm2(void);

int tpm2_digest_hash_buffers(TPMI_ALG_HASH alg, TPM2B_DIGEST *digest, const uint8_t *data[], const size_t len[], size_t count, bool extend);
#define tpm2_digest_extend_buffers(alg, digest, data, len, count) tpm2_digest_hash_buffers(alg, digest, data, len, count, true)
#define tpm2_digest_init_buffers(alg, digest, data, len, count) tpm2_digest_hash_buffers(alg, digest, data, len, count, false)
static inline int tpm2_digest_hash_buffer(TPMI_ALG_HASH alg, TPM2B_DIGEST *digest, const uint8_t *data, size_t len, bool extend) {
        return tpm2_digest_hash_buffers(alg, digest, &data, &len, 1, extend);
}
#define tpm2_digest_extend_buffer(alg, digest, data, len) tpm2_digest_hash_buffer(alg, digest, data, len, true)
#define tpm2_digest_init_buffer(alg, digest, data, len) tpm2_digest_hash_buffer(alg, digest, data, len, false)
int tpm2_digest_hash_digests(TPMI_ALG_HASH alg, TPM2B_DIGEST *digest, const TPM2B_DIGEST srcdigests[], size_t count, bool extend);
#define tpm2_digest_extend_digests(alg, digest, srcdigests, count) tpm2_digest_hash_digests(alg, digest, srcdigests, count, true)
#define tpm2_digest_init_digests(alg, digest, srcdigests, count) tpm2_digest_hash_digests(alg, digest, srcdigests, count, false)
#define tpm2_digest_hash_digest(alg, digest, srcdigest, extend) tpm2_digest_hash_digests(alg, digest, srcdigest, 1, extend)
#define tpm2_digest_extend_digest(alg, digest, srcdigest) tpm2_digest_hash_digest(alg, digest, srcdigest, true)
#define tpm2_digest_init_digest(alg, digest, srcdigest) tpm2_digest_hash_digest(alg, digest, srcdigest, false)
#define tpm2_digest_rehash(alg, digest) tpm2_digest_hash_buffers(alg, digest, NULL, NULL, 0, true)
#define tpm2_digest_init(alg, digest) tpm2_digest_hash_buffers(alg, digest, NULL, NULL, 0, false)

int tpm2_calculate_name(const TPMT_PUBLIC *public, TPM2B_NAME *ret_name);
int tpm2_calculate_policy_auth_value(TPM2B_DIGEST *digest);
int tpm2_calculate_policy_pcr(const TPML_PCR_SELECTION *pcr_selection, const TPM2B_DIGEST pcr_values[], size_t pcr_values_count, TPM2B_DIGEST *digest);

int tpm2_seal(const char *device, uint32_t hash_pcr_mask, const void *pubkey, size_t pubkey_size, uint32_t pubkey_pcr_mask, const char *pin, void **ret_secret, size_t *ret_secret_size, void **ret_blob, size_t *ret_blob_size, void **ret_pcr_hash, size_t *ret_pcr_hash_size, uint16_t *ret_pcr_bank, uint16_t *ret_primary_alg, void **ret_srk_buf, size_t *ret_srk_buf_size);
int tpm2_unseal(const char *device, uint32_t hash_pcr_mask, uint16_t pcr_bank, const void *pubkey, size_t pubkey_size, uint32_t pubkey_pcr_mask, JsonVariant *signature, const char *pin, uint16_t primary_alg, const void *blob, size_t blob_size, const void *policy_hash, size_t policy_hash_size, const void *srk_buf, size_t srk_buf_size, void **ret_secret, size_t *ret_secret_size);

typedef struct {
        unsigned n_ref;

        void *tcti_dl;
        TSS2_TCTI_CONTEXT *tcti_context;
        ESYS_CONTEXT *esys_context;
} Tpm2Context;

int tpm2_context_new(const char *device, Tpm2Context **ret_context);
Tpm2Context *tpm2_context_ref(Tpm2Context *context);
Tpm2Context *tpm2_context_unref(Tpm2Context *context);
DEFINE_TRIVIAL_CLEANUP_FUNC(Tpm2Context*, tpm2_context_unref);
#define _cleanup_tpm2_context_ _cleanup_(tpm2_context_unrefp)

typedef struct {
        Tpm2Context *tpm2_context;
        ESYS_TR esys_handle;
        bool keep;
} Tpm2Handle;

#define _tpm2_handle(c, h) { .tpm2_context = (c), .esys_handle = (h), }
static const Tpm2Handle TPM2_HANDLE_NONE = _tpm2_handle(NULL, ESYS_TR_NONE);

int tpm2_handle_new(Tpm2Context *context, Tpm2Handle **ret_handle);
Tpm2Handle *tpm2_handle_free(Tpm2Handle *handle);
DEFINE_TRIVIAL_CLEANUP_FUNC(Tpm2Handle*, tpm2_handle_free);
#define _cleanup_tpm2_handle_ _cleanup_(tpm2_handle_freep)

static inline void Esys_Freep(void *p) {
        if (*(void**) p)
                sym_Esys_Free(*(void**) p);
}

int tpm2_get_good_pcr_banks(Tpm2Context *c, uint32_t pcr_mask, TPMI_ALG_HASH **ret_banks);
int tpm2_get_good_pcr_banks_strv(Tpm2Context *c, uint32_t pcr_mask, char ***ret);

int tpm2_extend_bytes(Tpm2Context *c, char **banks, unsigned pcr_index, const void *data, size_t data_size, const void *secret, size_t secret_size);

void tpm2_tpms_pcr_selection_to_mask(const TPMS_PCR_SELECTION *s, uint32_t *ret);
void tpm2_tpms_pcr_selection_from_mask(uint32_t mask, TPMI_ALG_HASH hash, TPMS_PCR_SELECTION *ret);
void tpm2_tpms_pcr_selection_add(TPMS_PCR_SELECTION *a, const TPMS_PCR_SELECTION *b);
void tpm2_tpms_pcr_selection_sub(TPMS_PCR_SELECTION *a, const TPMS_PCR_SELECTION *b);
void tpm2_tpms_pcr_selection_move(TPMS_PCR_SELECTION *a, TPMS_PCR_SELECTION *b);
char *tpm2_tpms_pcr_selection_to_string(const TPMS_PCR_SELECTION *s);
size_t tpm2_tpms_pcr_selection_weight(const TPMS_PCR_SELECTION *s);
#define tpm2_tpms_pcr_selection_is_empty(s) (tpm2_tpms_pcr_selection_weight(s) == 0)

int tpm2_tpml_pcr_selection_to_mask(const TPML_PCR_SELECTION *l, TPMI_ALG_HASH hash, uint32_t *ret);
void tpm2_tpml_pcr_selection_from_mask(uint32_t mask, TPMI_ALG_HASH hash, TPML_PCR_SELECTION *ret);
void tpm2_tpml_pcr_selection_add_tpms_pcr_selection(TPML_PCR_SELECTION *l, const TPMS_PCR_SELECTION *s);
void tpm2_tpml_pcr_selection_sub_tpms_pcr_selection(TPML_PCR_SELECTION *l, const TPMS_PCR_SELECTION *s);
void tpm2_tpml_pcr_selection_add(TPML_PCR_SELECTION *a, const TPML_PCR_SELECTION *b);
void tpm2_tpml_pcr_selection_sub(TPML_PCR_SELECTION *a, const TPML_PCR_SELECTION *b);
char *tpm2_tpml_pcr_selection_to_string(const TPML_PCR_SELECTION *l);
size_t tpm2_tpml_pcr_selection_weight(const TPML_PCR_SELECTION *l);
#define tpm2_tpml_pcr_selection_is_empty(l) (tpm2_tpml_pcr_selection_weight(l) == 0)

const TPM2B_PUBLIC *tpm2_get_primary_template(Tpm2SRKTemplateFlags flags);

#else /* HAVE_TPM2 */
typedef struct {} Tpm2Context;
typedef struct {} Tpm2Handle;
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

const char *tpm2_hash_alg_to_string(uint16_t alg);
int tpm2_hash_alg_from_string(const char *alg);

const char *tpm2_asym_alg_to_string(uint16_t alg);
int tpm2_asym_alg_from_string(const char *alg);

char *tpm2_pcr_mask_to_string(uint32_t mask);
int tpm2_pcr_mask_from_string(const char *arg, uint32_t *mask);

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
        TPM2_SUPPORT_FULL      = TPM2_SUPPORT_FIRMWARE|TPM2_SUPPORT_DRIVER|TPM2_SUPPORT_SYSTEM|TPM2_SUPPORT_SUBSYSTEM,
} Tpm2Support;

typedef enum PcrIndex {
/* The following names for PCRs 0…7 are based on the names in the "TCG PC Client Specific Platform Firmware Profile Specification" (https://trustedcomputinggroup.org/resource/pc-client-specific-platform-firmware-profile-specification/) */
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
} PcrIndex;

Tpm2Support tpm2_support(void);

int tpm2_parse_pcr_argument(const char *arg, uint32_t *mask);

int tpm2_load_pcr_signature(const char *path, JsonVariant **ret);
int tpm2_load_pcr_public_key(const char *path, void **ret_pubkey, size_t *ret_pubkey_size);

int tpm2_util_pbkdf2_hmac_sha256(const void *pass,
                    size_t passlen,
                    const void *salt,
                    size_t saltlen,
                    uint8_t res[static SHA256_DIGEST_SIZE]);

int pcr_index_from_string(const char *s);
