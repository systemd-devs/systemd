/* SPDX-License-Identifier: LGPL-2.1-or-later
 * This file is generated by src/basic/missing_syscalls.py. Do not edit!
 *
 * Use 'ninja -C build update-syscall-tables' to download new syscall tables,
 * and 'ninja -C build update-syscall-header' to regenerate this file.
 */
#pragma once

/* Note: if this code looks strange, this is because it is derived from the same
 * template as the per-syscall blocks below. */
#  if defined(__aarch64__)
#  elif defined(__alpha__)
#  elif defined(__arc__) || defined(__tilegx__)
#  elif defined(__arm__)
#  elif defined(__i386__)
#  elif defined(__ia64__)
#  elif defined(__loongarch_lp64)
#  elif defined(__m68k__)
#  elif defined(_MIPS_SIM)
#    if _MIPS_SIM == _MIPS_SIM_ABI32
#    elif _MIPS_SIM == _MIPS_SIM_NABI32
#    elif _MIPS_SIM == _MIPS_SIM_ABI64
#    else
#      error "Unknown MIPS ABI"
#    endif
#  elif defined(__hppa__)
#  elif defined(__powerpc__)
#  elif defined(__riscv)
#    if __riscv_xlen == 32
#    elif __riscv_xlen == 64
#    else
#      error "Unknown RISC-V ABI"
#    endif
#  elif defined(__s390__)
#  elif defined(__sparc__)
#  elif defined(__x86_64__)
#    if defined(__ILP32__)
#    else
#    endif
#  elif !defined(missing_arch_template)
#    warning "Current architecture is missing from the template"
#    define missing_arch_template 1
#  endif

#ifndef __IGNORE_bpf
#  if defined(__aarch64__)
#    define systemd_NR_bpf 280
#  elif defined(__alpha__)
#    define systemd_NR_bpf 515
#  elif defined(__arc__) || defined(__tilegx__)
#    define systemd_NR_bpf 280
#  elif defined(__arm__)
#    define systemd_NR_bpf 386
#  elif defined(__i386__)
#    define systemd_NR_bpf 357
#  elif defined(__ia64__)
#    define systemd_NR_bpf 1341
#  elif defined(__loongarch_lp64)
#    define systemd_NR_bpf 280
#  elif defined(__m68k__)
#    define systemd_NR_bpf 354
#  elif defined(_MIPS_SIM)
#    if _MIPS_SIM == _MIPS_SIM_ABI32
#      define systemd_NR_bpf 4355
#    elif _MIPS_SIM == _MIPS_SIM_NABI32
#      define systemd_NR_bpf 6319
#    elif _MIPS_SIM == _MIPS_SIM_ABI64
#      define systemd_NR_bpf 5315
#    else
#      error "Unknown MIPS ABI"
#    endif
#  elif defined(__hppa__)
#    define systemd_NR_bpf 341
#  elif defined(__powerpc__)
#    define systemd_NR_bpf 361
#  elif defined(__riscv)
#    if __riscv_xlen == 32
#      define systemd_NR_bpf 280
#    elif __riscv_xlen == 64
#      define systemd_NR_bpf 280
#    else
#      error "Unknown RISC-V ABI"
#    endif
#  elif defined(__s390__)
#    define systemd_NR_bpf 351
#  elif defined(__sparc__)
#    define systemd_NR_bpf 349
#  elif defined(__x86_64__)
#    if defined(__ILP32__)
#      define systemd_NR_bpf (321 | /* __X32_SYSCALL_BIT */ 0x40000000)
#    else
#      define systemd_NR_bpf 321
#    endif
#  elif !defined(missing_arch_template)
#    warning "bpf() syscall number is unknown for your architecture"
#  endif

/* may be an (invalid) negative number due to libseccomp, see PR 13319 */
#  if defined __NR_bpf && __NR_bpf >= 0
#    if defined systemd_NR_bpf
assert_cc(__NR_bpf == systemd_NR_bpf);
#    endif
#  else
#    if defined __NR_bpf
#      undef __NR_bpf
#    endif
#    if defined systemd_NR_bpf && systemd_NR_bpf >= 0
#      define __NR_bpf systemd_NR_bpf
#    endif
#  endif
#endif

#ifndef __IGNORE_close_range
#  if defined(__aarch64__)
#    define systemd_NR_close_range 436
#  elif defined(__alpha__)
#    define systemd_NR_close_range 546
#  elif defined(__arc__) || defined(__tilegx__)
#    define systemd_NR_close_range 436
#  elif defined(__arm__)
#    define systemd_NR_close_range 436
#  elif defined(__i386__)
#    define systemd_NR_close_range 436
#  elif defined(__ia64__)
#    define systemd_NR_close_range 1460
#  elif defined(__loongarch_lp64)
#    define systemd_NR_close_range 436
#  elif defined(__m68k__)
#    define systemd_NR_close_range 436
#  elif defined(_MIPS_SIM)
#    if _MIPS_SIM == _MIPS_SIM_ABI32
#      define systemd_NR_close_range 4436
#    elif _MIPS_SIM == _MIPS_SIM_NABI32
#      define systemd_NR_close_range 6436
#    elif _MIPS_SIM == _MIPS_SIM_ABI64
#      define systemd_NR_close_range 5436
#    else
#      error "Unknown MIPS ABI"
#    endif
#  elif defined(__hppa__)
#    define systemd_NR_close_range 436
#  elif defined(__powerpc__)
#    define systemd_NR_close_range 436
#  elif defined(__riscv)
#    if __riscv_xlen == 32
#      define systemd_NR_close_range 436
#    elif __riscv_xlen == 64
#      define systemd_NR_close_range 436
#    else
#      error "Unknown RISC-V ABI"
#    endif
#  elif defined(__s390__)
#    define systemd_NR_close_range 436
#  elif defined(__sparc__)
#    define systemd_NR_close_range 436
#  elif defined(__x86_64__)
#    if defined(__ILP32__)
#      define systemd_NR_close_range (436 | /* __X32_SYSCALL_BIT */ 0x40000000)
#    else
#      define systemd_NR_close_range 436
#    endif
#  elif !defined(missing_arch_template)
#    warning "close_range() syscall number is unknown for your architecture"
#  endif

/* may be an (invalid) negative number due to libseccomp, see PR 13319 */
#  if defined __NR_close_range && __NR_close_range >= 0
#    if defined systemd_NR_close_range
assert_cc(__NR_close_range == systemd_NR_close_range);
#    endif
#  else
#    if defined __NR_close_range
#      undef __NR_close_range
#    endif
#    if defined systemd_NR_close_range && systemd_NR_close_range >= 0
#      define __NR_close_range systemd_NR_close_range
#    endif
#  endif
#endif

#ifndef __IGNORE_copy_file_range
#  if defined(__aarch64__)
#    define systemd_NR_copy_file_range 285
#  elif defined(__alpha__)
#    define systemd_NR_copy_file_range 519
#  elif defined(__arc__) || defined(__tilegx__)
#    define systemd_NR_copy_file_range 285
#  elif defined(__arm__)
#    define systemd_NR_copy_file_range 391
#  elif defined(__i386__)
#    define systemd_NR_copy_file_range 377
#  elif defined(__ia64__)
#    define systemd_NR_copy_file_range 1347
#  elif defined(__loongarch_lp64)
#    define systemd_NR_copy_file_range 285
#  elif defined(__m68k__)
#    define systemd_NR_copy_file_range 376
#  elif defined(_MIPS_SIM)
#    if _MIPS_SIM == _MIPS_SIM_ABI32
#      define systemd_NR_copy_file_range 4360
#    elif _MIPS_SIM == _MIPS_SIM_NABI32
#      define systemd_NR_copy_file_range 6324
#    elif _MIPS_SIM == _MIPS_SIM_ABI64
#      define systemd_NR_copy_file_range 5320
#    else
#      error "Unknown MIPS ABI"
#    endif
#  elif defined(__hppa__)
#    define systemd_NR_copy_file_range 346
#  elif defined(__powerpc__)
#    define systemd_NR_copy_file_range 379
#  elif defined(__riscv)
#    if __riscv_xlen == 32
#      define systemd_NR_copy_file_range 285
#    elif __riscv_xlen == 64
#      define systemd_NR_copy_file_range 285
#    else
#      error "Unknown RISC-V ABI"
#    endif
#  elif defined(__s390__)
#    define systemd_NR_copy_file_range 375
#  elif defined(__sparc__)
#    define systemd_NR_copy_file_range 357
#  elif defined(__x86_64__)
#    if defined(__ILP32__)
#      define systemd_NR_copy_file_range (326 | /* __X32_SYSCALL_BIT */ 0x40000000)
#    else
#      define systemd_NR_copy_file_range 326
#    endif
#  elif !defined(missing_arch_template)
#    warning "copy_file_range() syscall number is unknown for your architecture"
#  endif

/* may be an (invalid) negative number due to libseccomp, see PR 13319 */
#  if defined __NR_copy_file_range && __NR_copy_file_range >= 0
#    if defined systemd_NR_copy_file_range
assert_cc(__NR_copy_file_range == systemd_NR_copy_file_range);
#    endif
#  else
#    if defined __NR_copy_file_range
#      undef __NR_copy_file_range
#    endif
#    if defined systemd_NR_copy_file_range && systemd_NR_copy_file_range >= 0
#      define __NR_copy_file_range systemd_NR_copy_file_range
#    endif
#  endif
#endif

#ifndef __IGNORE_fchmodat2
#  if defined(__aarch64__)
#    define systemd_NR_fchmodat2 452
#  elif defined(__alpha__)
#    define systemd_NR_fchmodat2 562
#  elif defined(__arc__) || defined(__tilegx__)
#    define systemd_NR_fchmodat2 452
#  elif defined(__arm__)
#    define systemd_NR_fchmodat2 452
#  elif defined(__i386__)
#    define systemd_NR_fchmodat2 452
#  elif defined(__ia64__)
#    define systemd_NR_fchmodat2 1476
#  elif defined(__loongarch_lp64)
#    define systemd_NR_fchmodat2 452
#  elif defined(__m68k__)
#    define systemd_NR_fchmodat2 452
#  elif defined(_MIPS_SIM)
#    if _MIPS_SIM == _MIPS_SIM_ABI32
#      define systemd_NR_fchmodat2 4452
#    elif _MIPS_SIM == _MIPS_SIM_NABI32
#      define systemd_NR_fchmodat2 6452
#    elif _MIPS_SIM == _MIPS_SIM_ABI64
#      define systemd_NR_fchmodat2 5452
#    else
#      error "Unknown MIPS ABI"
#    endif
#  elif defined(__hppa__)
#    define systemd_NR_fchmodat2 452
#  elif defined(__powerpc__)
#    define systemd_NR_fchmodat2 452
#  elif defined(__riscv)
#    if __riscv_xlen == 32
#      define systemd_NR_fchmodat2 452
#    elif __riscv_xlen == 64
#      define systemd_NR_fchmodat2 452
#    else
#      error "Unknown RISC-V ABI"
#    endif
#  elif defined(__s390__)
#    define systemd_NR_fchmodat2 452
#  elif defined(__sparc__)
#    define systemd_NR_fchmodat2 452
#  elif defined(__x86_64__)
#    if defined(__ILP32__)
#      define systemd_NR_fchmodat2 (452 | /* __X32_SYSCALL_BIT */ 0x40000000)
#    else
#      define systemd_NR_fchmodat2 452
#    endif
#  elif !defined(missing_arch_template)
#    warning "fchmodat2() syscall number is unknown for your architecture"
#  endif

/* may be an (invalid) negative number due to libseccomp, see PR 13319 */
#  if defined __NR_fchmodat2 && __NR_fchmodat2 >= 0
#    if defined systemd_NR_fchmodat2
assert_cc(__NR_fchmodat2 == systemd_NR_fchmodat2);
#    endif
#  else
#    if defined __NR_fchmodat2
#      undef __NR_fchmodat2
#    endif
#    if defined systemd_NR_fchmodat2 && systemd_NR_fchmodat2 >= 0
#      define __NR_fchmodat2 systemd_NR_fchmodat2
#    endif
#  endif
#endif

#ifndef __IGNORE_mount_setattr
#  if defined(__aarch64__)
#    define systemd_NR_mount_setattr 442
#  elif defined(__alpha__)
#    define systemd_NR_mount_setattr 552
#  elif defined(__arc__) || defined(__tilegx__)
#    define systemd_NR_mount_setattr 442
#  elif defined(__arm__)
#    define systemd_NR_mount_setattr 442
#  elif defined(__i386__)
#    define systemd_NR_mount_setattr 442
#  elif defined(__ia64__)
#    define systemd_NR_mount_setattr 1466
#  elif defined(__loongarch_lp64)
#    define systemd_NR_mount_setattr 442
#  elif defined(__m68k__)
#    define systemd_NR_mount_setattr 442
#  elif defined(_MIPS_SIM)
#    if _MIPS_SIM == _MIPS_SIM_ABI32
#      define systemd_NR_mount_setattr 4442
#    elif _MIPS_SIM == _MIPS_SIM_NABI32
#      define systemd_NR_mount_setattr 6442
#    elif _MIPS_SIM == _MIPS_SIM_ABI64
#      define systemd_NR_mount_setattr 5442
#    else
#      error "Unknown MIPS ABI"
#    endif
#  elif defined(__hppa__)
#    define systemd_NR_mount_setattr 442
#  elif defined(__powerpc__)
#    define systemd_NR_mount_setattr 442
#  elif defined(__riscv)
#    if __riscv_xlen == 32
#      define systemd_NR_mount_setattr 442
#    elif __riscv_xlen == 64
#      define systemd_NR_mount_setattr 442
#    else
#      error "Unknown RISC-V ABI"
#    endif
#  elif defined(__s390__)
#    define systemd_NR_mount_setattr 442
#  elif defined(__sparc__)
#    define systemd_NR_mount_setattr 442
#  elif defined(__x86_64__)
#    if defined(__ILP32__)
#      define systemd_NR_mount_setattr (442 | /* __X32_SYSCALL_BIT */ 0x40000000)
#    else
#      define systemd_NR_mount_setattr 442
#    endif
#  elif !defined(missing_arch_template)
#    warning "mount_setattr() syscall number is unknown for your architecture"
#  endif

/* may be an (invalid) negative number due to libseccomp, see PR 13319 */
#  if defined __NR_mount_setattr && __NR_mount_setattr >= 0
#    if defined systemd_NR_mount_setattr
assert_cc(__NR_mount_setattr == systemd_NR_mount_setattr);
#    endif
#  else
#    if defined __NR_mount_setattr
#      undef __NR_mount_setattr
#    endif
#    if defined systemd_NR_mount_setattr && systemd_NR_mount_setattr >= 0
#      define __NR_mount_setattr systemd_NR_mount_setattr
#    endif
#  endif
#endif

#ifndef __IGNORE_move_mount
#  if defined(__aarch64__)
#    define systemd_NR_move_mount 429
#  elif defined(__alpha__)
#    define systemd_NR_move_mount 539
#  elif defined(__arc__) || defined(__tilegx__)
#    define systemd_NR_move_mount 429
#  elif defined(__arm__)
#    define systemd_NR_move_mount 429
#  elif defined(__i386__)
#    define systemd_NR_move_mount 429
#  elif defined(__ia64__)
#    define systemd_NR_move_mount 1453
#  elif defined(__loongarch_lp64)
#    define systemd_NR_move_mount 429
#  elif defined(__m68k__)
#    define systemd_NR_move_mount 429
#  elif defined(_MIPS_SIM)
#    if _MIPS_SIM == _MIPS_SIM_ABI32
#      define systemd_NR_move_mount 4429
#    elif _MIPS_SIM == _MIPS_SIM_NABI32
#      define systemd_NR_move_mount 6429
#    elif _MIPS_SIM == _MIPS_SIM_ABI64
#      define systemd_NR_move_mount 5429
#    else
#      error "Unknown MIPS ABI"
#    endif
#  elif defined(__hppa__)
#    define systemd_NR_move_mount 429
#  elif defined(__powerpc__)
#    define systemd_NR_move_mount 429
#  elif defined(__riscv)
#    if __riscv_xlen == 32
#      define systemd_NR_move_mount 429
#    elif __riscv_xlen == 64
#      define systemd_NR_move_mount 429
#    else
#      error "Unknown RISC-V ABI"
#    endif
#  elif defined(__s390__)
#    define systemd_NR_move_mount 429
#  elif defined(__sparc__)
#    define systemd_NR_move_mount 429
#  elif defined(__x86_64__)
#    if defined(__ILP32__)
#      define systemd_NR_move_mount (429 | /* __X32_SYSCALL_BIT */ 0x40000000)
#    else
#      define systemd_NR_move_mount 429
#    endif
#  elif !defined(missing_arch_template)
#    warning "move_mount() syscall number is unknown for your architecture"
#  endif

/* may be an (invalid) negative number due to libseccomp, see PR 13319 */
#  if defined __NR_move_mount && __NR_move_mount >= 0
#    if defined systemd_NR_move_mount
assert_cc(__NR_move_mount == systemd_NR_move_mount);
#    endif
#  else
#    if defined __NR_move_mount
#      undef __NR_move_mount
#    endif
#    if defined systemd_NR_move_mount && systemd_NR_move_mount >= 0
#      define __NR_move_mount systemd_NR_move_mount
#    endif
#  endif
#endif

#ifndef __IGNORE_open_tree
#  if defined(__aarch64__)
#    define systemd_NR_open_tree 428
#  elif defined(__alpha__)
#    define systemd_NR_open_tree 538
#  elif defined(__arc__) || defined(__tilegx__)
#    define systemd_NR_open_tree 428
#  elif defined(__arm__)
#    define systemd_NR_open_tree 428
#  elif defined(__i386__)
#    define systemd_NR_open_tree 428
#  elif defined(__ia64__)
#    define systemd_NR_open_tree 1452
#  elif defined(__loongarch_lp64)
#    define systemd_NR_open_tree 428
#  elif defined(__m68k__)
#    define systemd_NR_open_tree 428
#  elif defined(_MIPS_SIM)
#    if _MIPS_SIM == _MIPS_SIM_ABI32
#      define systemd_NR_open_tree 4428
#    elif _MIPS_SIM == _MIPS_SIM_NABI32
#      define systemd_NR_open_tree 6428
#    elif _MIPS_SIM == _MIPS_SIM_ABI64
#      define systemd_NR_open_tree 5428
#    else
#      error "Unknown MIPS ABI"
#    endif
#  elif defined(__hppa__)
#    define systemd_NR_open_tree 428
#  elif defined(__powerpc__)
#    define systemd_NR_open_tree 428
#  elif defined(__riscv)
#    if __riscv_xlen == 32
#      define systemd_NR_open_tree 428
#    elif __riscv_xlen == 64
#      define systemd_NR_open_tree 428
#    else
#      error "Unknown RISC-V ABI"
#    endif
#  elif defined(__s390__)
#    define systemd_NR_open_tree 428
#  elif defined(__sparc__)
#    define systemd_NR_open_tree 428
#  elif defined(__x86_64__)
#    if defined(__ILP32__)
#      define systemd_NR_open_tree (428 | /* __X32_SYSCALL_BIT */ 0x40000000)
#    else
#      define systemd_NR_open_tree 428
#    endif
#  elif !defined(missing_arch_template)
#    warning "open_tree() syscall number is unknown for your architecture"
#  endif

/* may be an (invalid) negative number due to libseccomp, see PR 13319 */
#  if defined __NR_open_tree && __NR_open_tree >= 0
#    if defined systemd_NR_open_tree
assert_cc(__NR_open_tree == systemd_NR_open_tree);
#    endif
#  else
#    if defined __NR_open_tree
#      undef __NR_open_tree
#    endif
#    if defined systemd_NR_open_tree && systemd_NR_open_tree >= 0
#      define __NR_open_tree systemd_NR_open_tree
#    endif
#  endif
#endif

#ifndef __IGNORE_openat2
#  if defined(__aarch64__)
#    define systemd_NR_openat2 437
#  elif defined(__alpha__)
#    define systemd_NR_openat2 547
#  elif defined(__arc__) || defined(__tilegx__)
#    define systemd_NR_openat2 437
#  elif defined(__arm__)
#    define systemd_NR_openat2 437
#  elif defined(__i386__)
#    define systemd_NR_openat2 437
#  elif defined(__ia64__)
#    define systemd_NR_openat2 1461
#  elif defined(__loongarch_lp64)
#    define systemd_NR_openat2 437
#  elif defined(__m68k__)
#    define systemd_NR_openat2 437
#  elif defined(_MIPS_SIM)
#    if _MIPS_SIM == _MIPS_SIM_ABI32
#      define systemd_NR_openat2 4437
#    elif _MIPS_SIM == _MIPS_SIM_NABI32
#      define systemd_NR_openat2 6437
#    elif _MIPS_SIM == _MIPS_SIM_ABI64
#      define systemd_NR_openat2 5437
#    else
#      error "Unknown MIPS ABI"
#    endif
#  elif defined(__hppa__)
#    define systemd_NR_openat2 437
#  elif defined(__powerpc__)
#    define systemd_NR_openat2 437
#  elif defined(__riscv)
#    if __riscv_xlen == 32
#      define systemd_NR_openat2 437
#    elif __riscv_xlen == 64
#      define systemd_NR_openat2 437
#    else
#      error "Unknown RISC-V ABI"
#    endif
#  elif defined(__s390__)
#    define systemd_NR_openat2 437
#  elif defined(__sparc__)
#    define systemd_NR_openat2 437
#  elif defined(__x86_64__)
#    if defined(__ILP32__)
#      define systemd_NR_openat2 (437 | /* __X32_SYSCALL_BIT */ 0x40000000)
#    else
#      define systemd_NR_openat2 437
#    endif
#  elif !defined(missing_arch_template)
#    warning "openat2() syscall number is unknown for your architecture"
#  endif

/* may be an (invalid) negative number due to libseccomp, see PR 13319 */
#  if defined __NR_openat2 && __NR_openat2 >= 0
#    if defined systemd_NR_openat2
assert_cc(__NR_openat2 == systemd_NR_openat2);
#    endif
#  else
#    if defined __NR_openat2
#      undef __NR_openat2
#    endif
#    if defined systemd_NR_openat2 && systemd_NR_openat2 >= 0
#      define __NR_openat2 systemd_NR_openat2
#    endif
#  endif
#endif

#ifndef __IGNORE_pidfd_open
#  if defined(__aarch64__)
#    define systemd_NR_pidfd_open 434
#  elif defined(__alpha__)
#    define systemd_NR_pidfd_open 544
#  elif defined(__arc__) || defined(__tilegx__)
#    define systemd_NR_pidfd_open 434
#  elif defined(__arm__)
#    define systemd_NR_pidfd_open 434
#  elif defined(__i386__)
#    define systemd_NR_pidfd_open 434
#  elif defined(__ia64__)
#    define systemd_NR_pidfd_open 1458
#  elif defined(__loongarch_lp64)
#    define systemd_NR_pidfd_open 434
#  elif defined(__m68k__)
#    define systemd_NR_pidfd_open 434
#  elif defined(_MIPS_SIM)
#    if _MIPS_SIM == _MIPS_SIM_ABI32
#      define systemd_NR_pidfd_open 4434
#    elif _MIPS_SIM == _MIPS_SIM_NABI32
#      define systemd_NR_pidfd_open 6434
#    elif _MIPS_SIM == _MIPS_SIM_ABI64
#      define systemd_NR_pidfd_open 5434
#    else
#      error "Unknown MIPS ABI"
#    endif
#  elif defined(__hppa__)
#    define systemd_NR_pidfd_open 434
#  elif defined(__powerpc__)
#    define systemd_NR_pidfd_open 434
#  elif defined(__riscv)
#    if __riscv_xlen == 32
#      define systemd_NR_pidfd_open 434
#    elif __riscv_xlen == 64
#      define systemd_NR_pidfd_open 434
#    else
#      error "Unknown RISC-V ABI"
#    endif
#  elif defined(__s390__)
#    define systemd_NR_pidfd_open 434
#  elif defined(__sparc__)
#    define systemd_NR_pidfd_open 434
#  elif defined(__x86_64__)
#    if defined(__ILP32__)
#      define systemd_NR_pidfd_open (434 | /* __X32_SYSCALL_BIT */ 0x40000000)
#    else
#      define systemd_NR_pidfd_open 434
#    endif
#  elif !defined(missing_arch_template)
#    warning "pidfd_open() syscall number is unknown for your architecture"
#  endif

/* may be an (invalid) negative number due to libseccomp, see PR 13319 */
#  if defined __NR_pidfd_open && __NR_pidfd_open >= 0
#    if defined systemd_NR_pidfd_open
assert_cc(__NR_pidfd_open == systemd_NR_pidfd_open);
#    endif
#  else
#    if defined __NR_pidfd_open
#      undef __NR_pidfd_open
#    endif
#    if defined systemd_NR_pidfd_open && systemd_NR_pidfd_open >= 0
#      define __NR_pidfd_open systemd_NR_pidfd_open
#    endif
#  endif
#endif

#ifndef __IGNORE_pidfd_send_signal
#  if defined(__aarch64__)
#    define systemd_NR_pidfd_send_signal 424
#  elif defined(__alpha__)
#    define systemd_NR_pidfd_send_signal 534
#  elif defined(__arc__) || defined(__tilegx__)
#    define systemd_NR_pidfd_send_signal 424
#  elif defined(__arm__)
#    define systemd_NR_pidfd_send_signal 424
#  elif defined(__i386__)
#    define systemd_NR_pidfd_send_signal 424
#  elif defined(__ia64__)
#    define systemd_NR_pidfd_send_signal 1448
#  elif defined(__loongarch_lp64)
#    define systemd_NR_pidfd_send_signal 424
#  elif defined(__m68k__)
#    define systemd_NR_pidfd_send_signal 424
#  elif defined(_MIPS_SIM)
#    if _MIPS_SIM == _MIPS_SIM_ABI32
#      define systemd_NR_pidfd_send_signal 4424
#    elif _MIPS_SIM == _MIPS_SIM_NABI32
#      define systemd_NR_pidfd_send_signal 6424
#    elif _MIPS_SIM == _MIPS_SIM_ABI64
#      define systemd_NR_pidfd_send_signal 5424
#    else
#      error "Unknown MIPS ABI"
#    endif
#  elif defined(__hppa__)
#    define systemd_NR_pidfd_send_signal 424
#  elif defined(__powerpc__)
#    define systemd_NR_pidfd_send_signal 424
#  elif defined(__riscv)
#    if __riscv_xlen == 32
#      define systemd_NR_pidfd_send_signal 424
#    elif __riscv_xlen == 64
#      define systemd_NR_pidfd_send_signal 424
#    else
#      error "Unknown RISC-V ABI"
#    endif
#  elif defined(__s390__)
#    define systemd_NR_pidfd_send_signal 424
#  elif defined(__sparc__)
#    define systemd_NR_pidfd_send_signal 424
#  elif defined(__x86_64__)
#    if defined(__ILP32__)
#      define systemd_NR_pidfd_send_signal (424 | /* __X32_SYSCALL_BIT */ 0x40000000)
#    else
#      define systemd_NR_pidfd_send_signal 424
#    endif
#  elif !defined(missing_arch_template)
#    warning "pidfd_send_signal() syscall number is unknown for your architecture"
#  endif

/* may be an (invalid) negative number due to libseccomp, see PR 13319 */
#  if defined __NR_pidfd_send_signal && __NR_pidfd_send_signal >= 0
#    if defined systemd_NR_pidfd_send_signal
assert_cc(__NR_pidfd_send_signal == systemd_NR_pidfd_send_signal);
#    endif
#  else
#    if defined __NR_pidfd_send_signal
#      undef __NR_pidfd_send_signal
#    endif
#    if defined systemd_NR_pidfd_send_signal && systemd_NR_pidfd_send_signal >= 0
#      define __NR_pidfd_send_signal systemd_NR_pidfd_send_signal
#    endif
#  endif
#endif

#ifndef __IGNORE_pkey_mprotect
#  if defined(__aarch64__)
#    define systemd_NR_pkey_mprotect 288
#  elif defined(__alpha__)
#    define systemd_NR_pkey_mprotect 524
#  elif defined(__arc__) || defined(__tilegx__)
#    define systemd_NR_pkey_mprotect 288
#  elif defined(__arm__)
#    define systemd_NR_pkey_mprotect 394
#  elif defined(__i386__)
#    define systemd_NR_pkey_mprotect 380
#  elif defined(__ia64__)
#    define systemd_NR_pkey_mprotect 1354
#  elif defined(__loongarch_lp64)
#    define systemd_NR_pkey_mprotect 288
#  elif defined(__m68k__)
#    define systemd_NR_pkey_mprotect 381
#  elif defined(_MIPS_SIM)
#    if _MIPS_SIM == _MIPS_SIM_ABI32
#      define systemd_NR_pkey_mprotect 4363
#    elif _MIPS_SIM == _MIPS_SIM_NABI32
#      define systemd_NR_pkey_mprotect 6327
#    elif _MIPS_SIM == _MIPS_SIM_ABI64
#      define systemd_NR_pkey_mprotect 5323
#    else
#      error "Unknown MIPS ABI"
#    endif
#  elif defined(__hppa__)
#    define systemd_NR_pkey_mprotect 351
#  elif defined(__powerpc__)
#    define systemd_NR_pkey_mprotect 386
#  elif defined(__riscv)
#    if __riscv_xlen == 32
#      define systemd_NR_pkey_mprotect 288
#    elif __riscv_xlen == 64
#      define systemd_NR_pkey_mprotect 288
#    else
#      error "Unknown RISC-V ABI"
#    endif
#  elif defined(__s390__)
#    define systemd_NR_pkey_mprotect 384
#  elif defined(__sparc__)
#    define systemd_NR_pkey_mprotect 362
#  elif defined(__x86_64__)
#    if defined(__ILP32__)
#      define systemd_NR_pkey_mprotect (329 | /* __X32_SYSCALL_BIT */ 0x40000000)
#    else
#      define systemd_NR_pkey_mprotect 329
#    endif
#  elif !defined(missing_arch_template)
#    warning "pkey_mprotect() syscall number is unknown for your architecture"
#  endif

/* may be an (invalid) negative number due to libseccomp, see PR 13319 */
#  if defined __NR_pkey_mprotect && __NR_pkey_mprotect >= 0
#    if defined systemd_NR_pkey_mprotect
assert_cc(__NR_pkey_mprotect == systemd_NR_pkey_mprotect);
#    endif
#  else
#    if defined __NR_pkey_mprotect
#      undef __NR_pkey_mprotect
#    endif
#    if defined systemd_NR_pkey_mprotect && systemd_NR_pkey_mprotect >= 0
#      define __NR_pkey_mprotect systemd_NR_pkey_mprotect
#    endif
#  endif
#endif

#ifndef __IGNORE_quotactl_fd
#  if defined(__aarch64__)
#    define systemd_NR_quotactl_fd 443
#  elif defined(__alpha__)
#    define systemd_NR_quotactl_fd 553
#  elif defined(__arc__) || defined(__tilegx__)
#    define systemd_NR_quotactl_fd 443
#  elif defined(__arm__)
#    define systemd_NR_quotactl_fd 443
#  elif defined(__i386__)
#    define systemd_NR_quotactl_fd 443
#  elif defined(__ia64__)
#    define systemd_NR_quotactl_fd 1467
#  elif defined(__loongarch_lp64)
#    define systemd_NR_quotactl_fd 443
#  elif defined(__m68k__)
#    define systemd_NR_quotactl_fd 443
#  elif defined(_MIPS_SIM)
#    if _MIPS_SIM == _MIPS_SIM_ABI32
#      define systemd_NR_quotactl_fd 4443
#    elif _MIPS_SIM == _MIPS_SIM_NABI32
#      define systemd_NR_quotactl_fd 6443
#    elif _MIPS_SIM == _MIPS_SIM_ABI64
#      define systemd_NR_quotactl_fd 5443
#    else
#      error "Unknown MIPS ABI"
#    endif
#  elif defined(__hppa__)
#    define systemd_NR_quotactl_fd 443
#  elif defined(__powerpc__)
#    define systemd_NR_quotactl_fd 443
#  elif defined(__riscv)
#    if __riscv_xlen == 32
#      define systemd_NR_quotactl_fd 443
#    elif __riscv_xlen == 64
#      define systemd_NR_quotactl_fd 443
#    else
#      error "Unknown RISC-V ABI"
#    endif
#  elif defined(__s390__)
#    define systemd_NR_quotactl_fd 443
#  elif defined(__sparc__)
#    define systemd_NR_quotactl_fd 443
#  elif defined(__x86_64__)
#    if defined(__ILP32__)
#      define systemd_NR_quotactl_fd (443 | /* __X32_SYSCALL_BIT */ 0x40000000)
#    else
#      define systemd_NR_quotactl_fd 443
#    endif
#  elif !defined(missing_arch_template)
#    warning "quotactl_fd() syscall number is unknown for your architecture"
#  endif

/* may be an (invalid) negative number due to libseccomp, see PR 13319 */
#  if defined __NR_quotactl_fd && __NR_quotactl_fd >= 0
#    if defined systemd_NR_quotactl_fd
assert_cc(__NR_quotactl_fd == systemd_NR_quotactl_fd);
#    endif
#  else
#    if defined __NR_quotactl_fd
#      undef __NR_quotactl_fd
#    endif
#    if defined systemd_NR_quotactl_fd && systemd_NR_quotactl_fd >= 0
#      define __NR_quotactl_fd systemd_NR_quotactl_fd
#    endif
#  endif
#endif

#ifndef __IGNORE_removexattrat
#  if defined(__aarch64__)
#    define systemd_NR_removexattrat 466
#  elif defined(__alpha__)
#    define systemd_NR_removexattrat 576
#  elif defined(__arc__) || defined(__tilegx__)
#    define systemd_NR_removexattrat 466
#  elif defined(__arm__)
#    define systemd_NR_removexattrat 466
#  elif defined(__i386__)
#    define systemd_NR_removexattrat 466
#  elif defined(__ia64__)
#    define systemd_NR_removexattrat -1
#  elif defined(__loongarch_lp64)
#    define systemd_NR_removexattrat 466
#  elif defined(__m68k__)
#    define systemd_NR_removexattrat 466
#  elif defined(_MIPS_SIM)
#    if _MIPS_SIM == _MIPS_SIM_ABI32
#      define systemd_NR_removexattrat 4466
#    elif _MIPS_SIM == _MIPS_SIM_NABI32
#      define systemd_NR_removexattrat 6466
#    elif _MIPS_SIM == _MIPS_SIM_ABI64
#      define systemd_NR_removexattrat 5466
#    else
#      error "Unknown MIPS ABI"
#    endif
#  elif defined(__hppa__)
#    define systemd_NR_removexattrat 466
#  elif defined(__powerpc__)
#    define systemd_NR_removexattrat 466
#  elif defined(__riscv)
#    if __riscv_xlen == 32
#      define systemd_NR_removexattrat 466
#    elif __riscv_xlen == 64
#      define systemd_NR_removexattrat 466
#    else
#      error "Unknown RISC-V ABI"
#    endif
#  elif defined(__s390__)
#    define systemd_NR_removexattrat 466
#  elif defined(__sparc__)
#    define systemd_NR_removexattrat 466
#  elif defined(__x86_64__)
#    if defined(__ILP32__)
#      define systemd_NR_removexattrat (466 | /* __X32_SYSCALL_BIT */ 0x40000000)
#    else
#      define systemd_NR_removexattrat 466
#    endif
#  elif !defined(missing_arch_template)
#    warning "removexattrat() syscall number is unknown for your architecture"
#  endif

/* may be an (invalid) negative number due to libseccomp, see PR 13319 */
#  if defined __NR_removexattrat && __NR_removexattrat >= 0
#    if defined systemd_NR_removexattrat
assert_cc(__NR_removexattrat == systemd_NR_removexattrat);
#    endif
#  else
#    if defined __NR_removexattrat
#      undef __NR_removexattrat
#    endif
#    if defined systemd_NR_removexattrat && systemd_NR_removexattrat >= 0
#      define __NR_removexattrat systemd_NR_removexattrat
#    endif
#  endif
#endif

#ifndef __IGNORE_renameat2
#  if defined(__aarch64__)
#    define systemd_NR_renameat2 276
#  elif defined(__alpha__)
#    define systemd_NR_renameat2 510
#  elif defined(__arc__) || defined(__tilegx__)
#    define systemd_NR_renameat2 276
#  elif defined(__arm__)
#    define systemd_NR_renameat2 382
#  elif defined(__i386__)
#    define systemd_NR_renameat2 353
#  elif defined(__ia64__)
#    define systemd_NR_renameat2 1338
#  elif defined(__loongarch_lp64)
#    define systemd_NR_renameat2 276
#  elif defined(__m68k__)
#    define systemd_NR_renameat2 351
#  elif defined(_MIPS_SIM)
#    if _MIPS_SIM == _MIPS_SIM_ABI32
#      define systemd_NR_renameat2 4351
#    elif _MIPS_SIM == _MIPS_SIM_NABI32
#      define systemd_NR_renameat2 6315
#    elif _MIPS_SIM == _MIPS_SIM_ABI64
#      define systemd_NR_renameat2 5311
#    else
#      error "Unknown MIPS ABI"
#    endif
#  elif defined(__hppa__)
#    define systemd_NR_renameat2 337
#  elif defined(__powerpc__)
#    define systemd_NR_renameat2 357
#  elif defined(__riscv)
#    if __riscv_xlen == 32
#      define systemd_NR_renameat2 276
#    elif __riscv_xlen == 64
#      define systemd_NR_renameat2 276
#    else
#      error "Unknown RISC-V ABI"
#    endif
#  elif defined(__s390__)
#    define systemd_NR_renameat2 347
#  elif defined(__sparc__)
#    define systemd_NR_renameat2 345
#  elif defined(__x86_64__)
#    if defined(__ILP32__)
#      define systemd_NR_renameat2 (316 | /* __X32_SYSCALL_BIT */ 0x40000000)
#    else
#      define systemd_NR_renameat2 316
#    endif
#  elif !defined(missing_arch_template)
#    warning "renameat2() syscall number is unknown for your architecture"
#  endif

/* may be an (invalid) negative number due to libseccomp, see PR 13319 */
#  if defined __NR_renameat2 && __NR_renameat2 >= 0
#    if defined systemd_NR_renameat2
assert_cc(__NR_renameat2 == systemd_NR_renameat2);
#    endif
#  else
#    if defined __NR_renameat2
#      undef __NR_renameat2
#    endif
#    if defined systemd_NR_renameat2 && systemd_NR_renameat2 >= 0
#      define __NR_renameat2 systemd_NR_renameat2
#    endif
#  endif
#endif

#ifndef __IGNORE_setns
#  if defined(__aarch64__)
#    define systemd_NR_setns 268
#  elif defined(__alpha__)
#    define systemd_NR_setns 501
#  elif defined(__arc__) || defined(__tilegx__)
#    define systemd_NR_setns 268
#  elif defined(__arm__)
#    define systemd_NR_setns 375
#  elif defined(__i386__)
#    define systemd_NR_setns 346
#  elif defined(__ia64__)
#    define systemd_NR_setns 1330
#  elif defined(__loongarch_lp64)
#    define systemd_NR_setns 268
#  elif defined(__m68k__)
#    define systemd_NR_setns 344
#  elif defined(_MIPS_SIM)
#    if _MIPS_SIM == _MIPS_SIM_ABI32
#      define systemd_NR_setns 4344
#    elif _MIPS_SIM == _MIPS_SIM_NABI32
#      define systemd_NR_setns 6308
#    elif _MIPS_SIM == _MIPS_SIM_ABI64
#      define systemd_NR_setns 5303
#    else
#      error "Unknown MIPS ABI"
#    endif
#  elif defined(__hppa__)
#    define systemd_NR_setns 328
#  elif defined(__powerpc__)
#    define systemd_NR_setns 350
#  elif defined(__riscv)
#    if __riscv_xlen == 32
#      define systemd_NR_setns 268
#    elif __riscv_xlen == 64
#      define systemd_NR_setns 268
#    else
#      error "Unknown RISC-V ABI"
#    endif
#  elif defined(__s390__)
#    define systemd_NR_setns 339
#  elif defined(__sparc__)
#    define systemd_NR_setns 337
#  elif defined(__x86_64__)
#    if defined(__ILP32__)
#      define systemd_NR_setns (308 | /* __X32_SYSCALL_BIT */ 0x40000000)
#    else
#      define systemd_NR_setns 308
#    endif
#  elif !defined(missing_arch_template)
#    warning "setns() syscall number is unknown for your architecture"
#  endif

/* may be an (invalid) negative number due to libseccomp, see PR 13319 */
#  if defined __NR_setns && __NR_setns >= 0
#    if defined systemd_NR_setns
assert_cc(__NR_setns == systemd_NR_setns);
#    endif
#  else
#    if defined __NR_setns
#      undef __NR_setns
#    endif
#    if defined systemd_NR_setns && systemd_NR_setns >= 0
#      define __NR_setns systemd_NR_setns
#    endif
#  endif
#endif

#ifndef __IGNORE_setxattrat
#  if defined(__aarch64__)
#    define systemd_NR_setxattrat 463
#  elif defined(__alpha__)
#    define systemd_NR_setxattrat 573
#  elif defined(__arc__) || defined(__tilegx__)
#    define systemd_NR_setxattrat 463
#  elif defined(__arm__)
#    define systemd_NR_setxattrat 463
#  elif defined(__i386__)
#    define systemd_NR_setxattrat 463
#  elif defined(__ia64__)
#    define systemd_NR_setxattrat -1
#  elif defined(__loongarch_lp64)
#    define systemd_NR_setxattrat 463
#  elif defined(__m68k__)
#    define systemd_NR_setxattrat 463
#  elif defined(_MIPS_SIM)
#    if _MIPS_SIM == _MIPS_SIM_ABI32
#      define systemd_NR_setxattrat 4463
#    elif _MIPS_SIM == _MIPS_SIM_NABI32
#      define systemd_NR_setxattrat 6463
#    elif _MIPS_SIM == _MIPS_SIM_ABI64
#      define systemd_NR_setxattrat 5463
#    else
#      error "Unknown MIPS ABI"
#    endif
#  elif defined(__hppa__)
#    define systemd_NR_setxattrat 463
#  elif defined(__powerpc__)
#    define systemd_NR_setxattrat 463
#  elif defined(__riscv)
#    if __riscv_xlen == 32
#      define systemd_NR_setxattrat 463
#    elif __riscv_xlen == 64
#      define systemd_NR_setxattrat 463
#    else
#      error "Unknown RISC-V ABI"
#    endif
#  elif defined(__s390__)
#    define systemd_NR_setxattrat 463
#  elif defined(__sparc__)
#    define systemd_NR_setxattrat 463
#  elif defined(__x86_64__)
#    if defined(__ILP32__)
#      define systemd_NR_setxattrat (463 | /* __X32_SYSCALL_BIT */ 0x40000000)
#    else
#      define systemd_NR_setxattrat 463
#    endif
#  elif !defined(missing_arch_template)
#    warning "setxattrat() syscall number is unknown for your architecture"
#  endif

/* may be an (invalid) negative number due to libseccomp, see PR 13319 */
#  if defined __NR_setxattrat && __NR_setxattrat >= 0
#    if defined systemd_NR_setxattrat
assert_cc(__NR_setxattrat == systemd_NR_setxattrat);
#    endif
#  else
#    if defined __NR_setxattrat
#      undef __NR_setxattrat
#    endif
#    if defined systemd_NR_setxattrat && systemd_NR_setxattrat >= 0
#      define __NR_setxattrat systemd_NR_setxattrat
#    endif
#  endif
#endif
