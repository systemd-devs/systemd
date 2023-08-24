/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "macro-fundamental.h"

/* The various TPM PCRs we measure into from sd-stub and sd-boot. */

enum {
        /* The following names for PCRs 0…7 are based on the names in the "TCG PC Client Specific Platform
         * Firmware Profile Specification"
         * (https://trustedcomputinggroup.org/resource/pc-client-specific-platform-firmware-profile-specification/) */
        TPM2_PCR_PLATFORM_CODE       = 0,
        TPM2_PCR_PLATFORM_CONFIG     = 1,
        TPM2_PCR_EXTERNAL_CODE       = 2,
        TPM2_PCR_EXTERNAL_CONFIG     = 3,
        TPM2_PCR_BOOT_LOADER_CODE    = 4,
        TPM2_PCR_BOOT_LOADER_CONFIG  = 5,
        TPM2_PCR_HOST_PLATFORM       = 6,
        TPM2_PCR_SECURE_BOOT_POLICY  = 7,

        /* The following names for PCRs 9…15 are based on the "Linux TPM PCR Registry"
        (https://uapi-group.org/specifications/specs/linux_tpm_pcr_registry/) */
        TPM2_PCR_KERNEL_INITRD       = 9,
        TPM2_PCR_IMA                 = 10,

        /* systemd: This TPM PCR is where we extend the sd-stub "payloads" into, before using them. i.e. the kernel
         * ELF image, embedded initrd, and so on. In contrast to PCR 4 (which also contains this data, given
         * the whole surrounding PE image is measured into it) this should be reasonably pre-calculatable,
         * because it *only* consists of static data from the kernel PE image. */
        TPM2_PCR_KERNEL_BOOT         = 11,

        /* systemd: This TPM PCR is where sd-stub extends the kernel command line and any passed credentials into. */
        TPM2_PCR_KERNEL_CONFIG       = 12,

        /* systemd: This TPM PCR is where we extend the initrd sysext images into which we pass to the booted kernel */
        TPM2_PCR_SYSEXTS             = 13,
        TPM2_PCR_SHIM_POLICY         = 14,

        /* systemd: This TPM PCR is where we measure the root fs volume key (and maybe /var/'s) if it is split off */
        TPM2_PCR_SYSTEM_IDENTITY     = 15,

        /* As per "TCG PC Client Specific Platform Firmware Profile Specification" again, see above */
        TPM2_PCR_DEBUG               = 16,
        TPM2_PCR_APPLICATION_SUPPORT = 23,
};

/* List of PE sections that have special meaning for us in unified kernels. This is the canonical order in
 * which we measure the sections into TPM PCR 11 (see above). PLEASE DO NOT REORDER! */
typedef enum UnifiedSection {
        UNIFIED_SECTION_LINUX,
        UNIFIED_SECTION_OSREL,
        UNIFIED_SECTION_CMDLINE,
        UNIFIED_SECTION_INITRD,
        UNIFIED_SECTION_SPLASH,
        UNIFIED_SECTION_DTB,
        UNIFIED_SECTION_UNAME,
        UNIFIED_SECTION_SBAT,
        UNIFIED_SECTION_PCRSIG,
        UNIFIED_SECTION_PCRPKEY,
        _UNIFIED_SECTION_MAX,
} UnifiedSection;

extern const char* const unified_sections[_UNIFIED_SECTION_MAX + 1];

static inline bool unified_section_measure(UnifiedSection section) {
        /* Don't include the PCR signature in the PCR measurements, since they sign the expected result of
         * the measurement, and hence shouldn't be input to it. */
        return section >= 0 && section < _UNIFIED_SECTION_MAX && section != UNIFIED_SECTION_PCRSIG;
}
