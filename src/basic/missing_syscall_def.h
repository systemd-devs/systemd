/* SPDX-License-Identifier: LGPL-2.1-or-later
 * This file is generated by src/basic/missing_syscalls.py. Do not edit!
 *
 * Use 'ninja -C build update-syscall-tables' to download new syscall tables,
 * and 'ninja -C build update-syscall-header' to regenerate this file.
 */

/* Note: if this code looks strange, this is because it is derived from the same
 * template as the per-syscall blocks below. */
#  if defined(__aarch64__)
#  elif defined(__alpha__)
#  elif defined(__arc__) || defined(__tilegx__)
#  elif defined(__arm__)
#  elif defined(__i386__)
#  elif defined(__ia64__)
#  elif defined(__loongarch64)
#  elif defined(__m68k__)
#  elif defined(_MIPS_SIM)
#    if _MIPS_SIM == _MIPS_SIM_ABI32
#    elif _MIPS_SIM == _MIPS_SIM_NABI32
#    elif _MIPS_SIM == _MIPS_SIM_ABI64
#    else
#      error "Unknown MIPS ABI"
#    endif
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
#  elif defined(__loongarch64)
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
#  elif defined(__loongarch64)
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
#  elif defined(__loongarch64)
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

#ifndef __IGNORE_epoll_pwait2
#  if defined(__aarch64__)
#    define systemd_NR_epoll_pwait2 441
#  elif defined(__alpha__)
#    define systemd_NR_epoll_pwait2 551
#  elif defined(__arc__) || defined(__tilegx__)
#    define systemd_NR_epoll_pwait2 441
#  elif defined(__arm__)
#    define systemd_NR_epoll_pwait2 441
#  elif defined(__i386__)
#    define systemd_NR_epoll_pwait2 441
#  elif defined(__ia64__)
#    define systemd_NR_epoll_pwait2 1465
#  elif defined(__loongarch64)
#    define systemd_NR_epoll_pwait2 441
#  elif defined(__m68k__)
#    define systemd_NR_epoll_pwait2 441
#  elif defined(_MIPS_SIM)
#    if _MIPS_SIM == _MIPS_SIM_ABI32
#      define systemd_NR_epoll_pwait2 4441
#    elif _MIPS_SIM == _MIPS_SIM_NABI32
#      define systemd_NR_epoll_pwait2 6441
#    elif _MIPS_SIM == _MIPS_SIM_ABI64
#      define systemd_NR_epoll_pwait2 5441
#    else
#      error "Unknown MIPS ABI"
#    endif
#  elif defined(__powerpc__)
#    define systemd_NR_epoll_pwait2 441
#  elif defined(__riscv)
#    if __riscv_xlen == 32
#      define systemd_NR_epoll_pwait2 441
#    elif __riscv_xlen == 64
#      define systemd_NR_epoll_pwait2 441
#    else
#      error "Unknown RISC-V ABI"
#    endif
#  elif defined(__s390__)
#    define systemd_NR_epoll_pwait2 441
#  elif defined(__sparc__)
#    define systemd_NR_epoll_pwait2 441
#  elif defined(__x86_64__)
#    if defined(__ILP32__)
#      define systemd_NR_epoll_pwait2 (441 | /* __X32_SYSCALL_BIT */ 0x40000000)
#    else
#      define systemd_NR_epoll_pwait2 441
#    endif
#  elif !defined(missing_arch_template)
#    warning "epoll_pwait2() syscall number is unknown for your architecture"
#  endif

/* may be an (invalid) negative number due to libseccomp, see PR 13319 */
#  if defined __NR_epoll_pwait2 && __NR_epoll_pwait2 >= 0
#    if defined systemd_NR_epoll_pwait2
assert_cc(__NR_epoll_pwait2 == systemd_NR_epoll_pwait2);
#    endif
#  else
#    if defined __NR_epoll_pwait2
#      undef __NR_epoll_pwait2
#    endif
#    if defined systemd_NR_epoll_pwait2 && systemd_NR_epoll_pwait2 >= 0
#      define __NR_epoll_pwait2 systemd_NR_epoll_pwait2
#    endif
#  endif
#endif

#ifndef __IGNORE_getrandom
#  if defined(__aarch64__)
#    define systemd_NR_getrandom 278
#  elif defined(__alpha__)
#    define systemd_NR_getrandom 511
#  elif defined(__arc__) || defined(__tilegx__)
#    define systemd_NR_getrandom 278
#  elif defined(__arm__)
#    define systemd_NR_getrandom 384
#  elif defined(__i386__)
#    define systemd_NR_getrandom 355
#  elif defined(__ia64__)
#    define systemd_NR_getrandom 1339
#  elif defined(__loongarch64)
#    define systemd_NR_getrandom 278
#  elif defined(__m68k__)
#    define systemd_NR_getrandom 352
#  elif defined(_MIPS_SIM)
#    if _MIPS_SIM == _MIPS_SIM_ABI32
#      define systemd_NR_getrandom 4353
#    elif _MIPS_SIM == _MIPS_SIM_NABI32
#      define systemd_NR_getrandom 6317
#    elif _MIPS_SIM == _MIPS_SIM_ABI64
#      define systemd_NR_getrandom 5313
#    else
#      error "Unknown MIPS ABI"
#    endif
#  elif defined(__powerpc__)
#    define systemd_NR_getrandom 359
#  elif defined(__riscv)
#    if __riscv_xlen == 32
#      define systemd_NR_getrandom 278
#    elif __riscv_xlen == 64
#      define systemd_NR_getrandom 278
#    else
#      error "Unknown RISC-V ABI"
#    endif
#  elif defined(__s390__)
#    define systemd_NR_getrandom 349
#  elif defined(__sparc__)
#    define systemd_NR_getrandom 347
#  elif defined(__x86_64__)
#    if defined(__ILP32__)
#      define systemd_NR_getrandom (318 | /* __X32_SYSCALL_BIT */ 0x40000000)
#    else
#      define systemd_NR_getrandom 318
#    endif
#  elif !defined(missing_arch_template)
#    warning "getrandom() syscall number is unknown for your architecture"
#  endif

/* may be an (invalid) negative number due to libseccomp, see PR 13319 */
#  if defined __NR_getrandom && __NR_getrandom >= 0
#    if defined systemd_NR_getrandom
assert_cc(__NR_getrandom == systemd_NR_getrandom);
#    endif
#  else
#    if defined __NR_getrandom
#      undef __NR_getrandom
#    endif
#    if defined systemd_NR_getrandom && systemd_NR_getrandom >= 0
#      define __NR_getrandom systemd_NR_getrandom
#    endif
#  endif
#endif

#ifndef __IGNORE_memfd_create
#  if defined(__aarch64__)
#    define systemd_NR_memfd_create 279
#  elif defined(__alpha__)
#    define systemd_NR_memfd_create 512
#  elif defined(__arc__) || defined(__tilegx__)
#    define systemd_NR_memfd_create 279
#  elif defined(__arm__)
#    define systemd_NR_memfd_create 385
#  elif defined(__i386__)
#    define systemd_NR_memfd_create 356
#  elif defined(__ia64__)
#    define systemd_NR_memfd_create 1340
#  elif defined(__loongarch64)
#    define systemd_NR_memfd_create 279
#  elif defined(__m68k__)
#    define systemd_NR_memfd_create 353
#  elif defined(_MIPS_SIM)
#    if _MIPS_SIM == _MIPS_SIM_ABI32
#      define systemd_NR_memfd_create 4354
#    elif _MIPS_SIM == _MIPS_SIM_NABI32
#      define systemd_NR_memfd_create 6318
#    elif _MIPS_SIM == _MIPS_SIM_ABI64
#      define systemd_NR_memfd_create 5314
#    else
#      error "Unknown MIPS ABI"
#    endif
#  elif defined(__powerpc__)
#    define systemd_NR_memfd_create 360
#  elif defined(__riscv)
#    if __riscv_xlen == 32
#      define systemd_NR_memfd_create 279
#    elif __riscv_xlen == 64
#      define systemd_NR_memfd_create 279
#    else
#      error "Unknown RISC-V ABI"
#    endif
#  elif defined(__s390__)
#    define systemd_NR_memfd_create 350
#  elif defined(__sparc__)
#    define systemd_NR_memfd_create 348
#  elif defined(__x86_64__)
#    if defined(__ILP32__)
#      define systemd_NR_memfd_create (319 | /* __X32_SYSCALL_BIT */ 0x40000000)
#    else
#      define systemd_NR_memfd_create 319
#    endif
#  elif !defined(missing_arch_template)
#    warning "memfd_create() syscall number is unknown for your architecture"
#  endif

/* may be an (invalid) negative number due to libseccomp, see PR 13319 */
#  if defined __NR_memfd_create && __NR_memfd_create >= 0
#    if defined systemd_NR_memfd_create
assert_cc(__NR_memfd_create == systemd_NR_memfd_create);
#    endif
#  else
#    if defined __NR_memfd_create
#      undef __NR_memfd_create
#    endif
#    if defined systemd_NR_memfd_create && systemd_NR_memfd_create >= 0
#      define __NR_memfd_create systemd_NR_memfd_create
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
#  elif defined(__loongarch64)
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
#  elif defined(__loongarch64)
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

#ifndef __IGNORE_name_to_handle_at
#  if defined(__aarch64__)
#    define systemd_NR_name_to_handle_at 264
#  elif defined(__alpha__)
#    define systemd_NR_name_to_handle_at 497
#  elif defined(__arc__) || defined(__tilegx__)
#    define systemd_NR_name_to_handle_at 264
#  elif defined(__arm__)
#    define systemd_NR_name_to_handle_at 370
#  elif defined(__i386__)
#    define systemd_NR_name_to_handle_at 341
#  elif defined(__ia64__)
#    define systemd_NR_name_to_handle_at 1326
#  elif defined(__loongarch64)
#    define systemd_NR_name_to_handle_at 264
#  elif defined(__m68k__)
#    define systemd_NR_name_to_handle_at 340
#  elif defined(_MIPS_SIM)
#    if _MIPS_SIM == _MIPS_SIM_ABI32
#      define systemd_NR_name_to_handle_at 4339
#    elif _MIPS_SIM == _MIPS_SIM_NABI32
#      define systemd_NR_name_to_handle_at 6303
#    elif _MIPS_SIM == _MIPS_SIM_ABI64
#      define systemd_NR_name_to_handle_at 5298
#    else
#      error "Unknown MIPS ABI"
#    endif
#  elif defined(__powerpc__)
#    define systemd_NR_name_to_handle_at 345
#  elif defined(__riscv)
#    if __riscv_xlen == 32
#      define systemd_NR_name_to_handle_at 264
#    elif __riscv_xlen == 64
#      define systemd_NR_name_to_handle_at 264
#    else
#      error "Unknown RISC-V ABI"
#    endif
#  elif defined(__s390__)
#    define systemd_NR_name_to_handle_at 335
#  elif defined(__sparc__)
#    define systemd_NR_name_to_handle_at 332
#  elif defined(__x86_64__)
#    if defined(__ILP32__)
#      define systemd_NR_name_to_handle_at (303 | /* __X32_SYSCALL_BIT */ 0x40000000)
#    else
#      define systemd_NR_name_to_handle_at 303
#    endif
#  elif !defined(missing_arch_template)
#    warning "name_to_handle_at() syscall number is unknown for your architecture"
#  endif

/* may be an (invalid) negative number due to libseccomp, see PR 13319 */
#  if defined __NR_name_to_handle_at && __NR_name_to_handle_at >= 0
#    if defined systemd_NR_name_to_handle_at
assert_cc(__NR_name_to_handle_at == systemd_NR_name_to_handle_at);
#    endif
#  else
#    if defined __NR_name_to_handle_at
#      undef __NR_name_to_handle_at
#    endif
#    if defined systemd_NR_name_to_handle_at && systemd_NR_name_to_handle_at >= 0
#      define __NR_name_to_handle_at systemd_NR_name_to_handle_at
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
#  elif defined(__loongarch64)
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
#  elif defined(__loongarch64)
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
#  elif defined(__loongarch64)
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
#  elif defined(__loongarch64)
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
#  elif defined(__loongarch64)
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
#  elif defined(__loongarch64)
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
#  elif defined(__loongarch64)
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

#ifndef __IGNORE_statx
#  if defined(__aarch64__)
#    define systemd_NR_statx 291
#  elif defined(__alpha__)
#    define systemd_NR_statx 522
#  elif defined(__arc__) || defined(__tilegx__)
#    define systemd_NR_statx 291
#  elif defined(__arm__)
#    define systemd_NR_statx 397
#  elif defined(__i386__)
#    define systemd_NR_statx 383
#  elif defined(__ia64__)
#    define systemd_NR_statx 1350
#  elif defined(__loongarch64)
#    define systemd_NR_statx 291
#  elif defined(__m68k__)
#    define systemd_NR_statx 379
#  elif defined(_MIPS_SIM)
#    if _MIPS_SIM == _MIPS_SIM_ABI32
#      define systemd_NR_statx 4366
#    elif _MIPS_SIM == _MIPS_SIM_NABI32
#      define systemd_NR_statx 6330
#    elif _MIPS_SIM == _MIPS_SIM_ABI64
#      define systemd_NR_statx 5326
#    else
#      error "Unknown MIPS ABI"
#    endif
#  elif defined(__powerpc__)
#    define systemd_NR_statx 383
#  elif defined(__riscv)
#    if __riscv_xlen == 32
#      define systemd_NR_statx 291
#    elif __riscv_xlen == 64
#      define systemd_NR_statx 291
#    else
#      error "Unknown RISC-V ABI"
#    endif
#  elif defined(__s390__)
#    define systemd_NR_statx 379
#  elif defined(__sparc__)
#    define systemd_NR_statx 360
#  elif defined(__x86_64__)
#    if defined(__ILP32__)
#      define systemd_NR_statx (332 | /* __X32_SYSCALL_BIT */ 0x40000000)
#    else
#      define systemd_NR_statx 332
#    endif
#  elif !defined(missing_arch_template)
#    warning "statx() syscall number is unknown for your architecture"
#  endif

/* may be an (invalid) negative number due to libseccomp, see PR 13319 */
#  if defined __NR_statx && __NR_statx >= 0
#    if defined systemd_NR_statx
assert_cc(__NR_statx == systemd_NR_statx);
#    endif
#  else
#    if defined __NR_statx
#      undef __NR_statx
#    endif
#    if defined systemd_NR_statx && systemd_NR_statx >= 0
#      define __NR_statx systemd_NR_statx
#    endif
#  endif
#endif
