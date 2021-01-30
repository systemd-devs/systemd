/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <efi.h>
#include <efilib.h>

#define ELEMENTSOF(x) (sizeof(x)/sizeof((x)[0]))
#define OFFSETOF(x,y) __builtin_offsetof(x,y)

/*
 * Allocated random UUID, intended to be shared across tools that implement
 * the (ESP)\loader\entries\<vendor>-<revision>.conf convention and the
 * associated EFI variables.
 */
#define LOADER_GUID &(const EFI_GUID) { 0x4a67b082, 0x0a4c, 0x41cf, {0xb6, 0xc7, 0x44, 0x0b, 0x29, 0xbb, 0x8c, 0x4f} }
#define GLOBAL_GUID &(const EFI_GUID) EFI_GLOBAL_VARIABLE

static inline UINTN ALIGN_TO(UINTN l, UINTN ali) {
        return ((l + ali - 1) & ~(ali - 1));
}

static inline const CHAR16 *yes_no(BOOLEAN b) {
        return b ? L"yes" : L"no";
}

EFI_STATUS parse_boolean(const CHAR8 *v, BOOLEAN *b);

UINT64 ticks_read(void);
UINT64 ticks_freq(void);
UINT64 time_usec(void);

EFI_STATUS efivar_set(const EFI_GUID *vendor, const CHAR16 *name, const CHAR16 *value, BOOLEAN persistent);
EFI_STATUS efivar_set_raw(const EFI_GUID *vendor, const CHAR16 *name, const VOID *buf, UINTN size, BOOLEAN persistent);
EFI_STATUS efivar_set_int(const EFI_GUID *vendor, CHAR16 *name, UINTN i, BOOLEAN persistent);
VOID efivar_set_time_usec(const EFI_GUID *vendor, CHAR16 *name, UINT64 usec);

EFI_STATUS efivar_get(const EFI_GUID *vendor, const CHAR16 *name, CHAR16 **value);
EFI_STATUS efivar_get_raw(const EFI_GUID *vendor, const CHAR16 *name, CHAR8 **buffer, UINTN *size);
EFI_STATUS efivar_get_int(const EFI_GUID *vendor, const CHAR16 *name, UINTN *i);

CHAR8 *strchra(CHAR8 *s, CHAR8 c);
CHAR16 *stra_to_path(CHAR8 *stra);
CHAR16 *stra_to_str(CHAR8 *stra);

EFI_STATUS file_read(EFI_FILE_HANDLE dir, const CHAR16 *name, UINTN off, UINTN size, CHAR8 **content, UINTN *content_size);

static inline void FreePoolp(void *p) {
        void *q = *(void**) p;

        if (!q)
                return;

        FreePool(q);
}

#define _cleanup_(x) __attribute__((__cleanup__(x)))
#define _cleanup_freepool_ _cleanup_(FreePoolp)

static inline void FileHandleClosep(EFI_FILE_HANDLE *handle) {
        if (!*handle)
                return;

        uefi_call_wrapper((*handle)->Close, 1, *handle);
}

#define UINTN_MAX (~(UINTN)0)
#define INTN_MAX ((INTN)(UINTN_MAX>>1))

#define TAKE_PTR(ptr)                           \
        ({                                      \
                typeof(ptr) _ptr_ = (ptr);      \
                (ptr) = NULL;                   \
                _ptr_;                          \
        })

EFI_STATUS log_oom(void);
