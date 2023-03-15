/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "efi.h"

bool is_direct_boot(EFI_HANDLE device);
EFI_STATUS vmm_open(EFI_HANDLE *ret_qemu_dev, EFI_FILE **ret_qemu_dir);

bool in_hypervisor(void);

EFI_STATUS smbios_find_oem_string_join(const char *name, char16_t **ret);
