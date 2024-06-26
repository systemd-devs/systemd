/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "efi.h"

/* This is a subset of the full PE section header structure, with validated values, and without
 * the noise. */
typedef struct PeSectionVector {
        size_t size;
        size_t file_offset;     /* Offset on disk, relative to beginning of file */
        size_t memory_offset;   /* Offfset in memory, relative to base address */
} PeSectionVector;

static inline bool PE_SECTION_VECTOR_IS_SET(const PeSectionVector *v) {
        return v && v->size != 0;
}

EFI_STATUS pe_memory_locate_sections(
                const void *base,
                const char * const sections[],
                PeSectionVector *ret_sections);

EFI_STATUS pe_file_locate_sections(
                EFI_FILE *dir,
                const char16_t *path,
                const char * const sections[],
                PeSectionVector *ret_sections);

EFI_STATUS pe_kernel_info(const void *base, uint32_t *ret_compat_address);
