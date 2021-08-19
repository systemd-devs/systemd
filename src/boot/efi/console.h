/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "missing_efi.h"

#define EFI_CONTROL_PRESSED             (EFI_RIGHT_CONTROL_PRESSED|EFI_LEFT_CONTROL_PRESSED)
#define EFI_ALT_PRESSED                 (EFI_RIGHT_ALT_PRESSED|EFI_LEFT_ALT_PRESSED)
#define KEYPRESS(keys, scan, uni) ((((UINT64)keys) << 32) | (((UINT64)scan) << 16) | (uni))
#define KEYCHAR(k) ((k) & 0xffff)
#define CHAR_CTRL(c) ((c) - 'a' + 1)

enum console_mode_special {
        CONSOLE_MODE_KEEP = INT32_MAX + 1UL,
        CONSOLE_MODE_AUTO,
        CONSOLE_MODE_NEXT,
        CONSOLE_MODE_MAX,
};

EFI_STATUS console_key_read(UINT64 *key, UINT64 timeout_usec);
EFI_STATUS console_set_mode(UINT64 mode);
EFI_STATUS console_query_mode(UINTN *x_max, UINTN *y_max);
