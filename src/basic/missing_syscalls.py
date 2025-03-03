#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

import sys
import functools

# We only generate numbers for a dozen or so syscalls
SYSCALLS = [
    'close_range',   # defined in glibc header since glibc-2.33
    'fchmodat2',     # defined in glibc header since glibc-2.39
    'mount_setattr', # defined in glibc header since glibc-2.34
    'openat2',       # defined in glibc header since glibc-2.32
    'quotactl_fd',   # defined in glibc header since glibc-2.35
    'removexattrat',
    'setxattrat',
]

def dictify(f):
    def wrap(*args, **kwargs):
        return dict(f(*args, **kwargs))
    return functools.update_wrapper(wrap, f)

@dictify
def parse_syscall_table(filename):
    print(f'Reading {filename}…')
    for line in open(filename):
        items = line.split()
        if len(items) >= 2:
            yield items[0], int(items[1])

def parse_syscall_tables(filenames):
    return {filename.split('-')[-1][:-4]: parse_syscall_table(filename)
            for filename in filenames}

DEF_TEMPLATE_A = '''\

#ifndef __IGNORE_{syscall}
'''

DEF_TEMPLATE_B = '''\
#  if defined(__aarch64__)
#    define systemd_NR_{syscall} {nr_arm64}
#  elif defined(__alpha__)
#    define systemd_NR_{syscall} {nr_alpha}
#  elif defined(__arc__) || defined(__tilegx__)
#    define systemd_NR_{syscall} {nr_arc}
#  elif defined(__arm__)
#    define systemd_NR_{syscall} {nr_arm}
#  elif defined(__i386__)
#    define systemd_NR_{syscall} {nr_i386}
#  elif defined(__ia64__)
#    define systemd_NR_{syscall} {nr_ia64}
#  elif defined(__loongarch_lp64)
#    define systemd_NR_{syscall} {nr_loongarch64}
#  elif defined(__m68k__)
#    define systemd_NR_{syscall} {nr_m68k}
#  elif defined(_MIPS_SIM)
#    if _MIPS_SIM == _MIPS_SIM_ABI32
#      define systemd_NR_{syscall} {nr_mipso32}
#    elif _MIPS_SIM == _MIPS_SIM_NABI32
#      define systemd_NR_{syscall} {nr_mips64n32}
#    elif _MIPS_SIM == _MIPS_SIM_ABI64
#      define systemd_NR_{syscall} {nr_mips64}
#    else
#      error "Unknown MIPS ABI"
#    endif
#  elif defined(__hppa__)
#    define systemd_NR_{syscall} {nr_parisc}
#  elif defined(__powerpc__)
#    define systemd_NR_{syscall} {nr_powerpc}
#  elif defined(__riscv)
#    if __riscv_xlen == 32
#      define systemd_NR_{syscall} {nr_riscv32}
#    elif __riscv_xlen == 64
#      define systemd_NR_{syscall} {nr_riscv64}
#    else
#      error "Unknown RISC-V ABI"
#    endif
#  elif defined(__s390__)
#    define systemd_NR_{syscall} {nr_s390}
#  elif defined(__sparc__)
#    define systemd_NR_{syscall} {nr_sparc}
#  elif defined(__x86_64__)
#    if defined(__ILP32__)
#      define systemd_NR_{syscall} ({nr_x86_64} | /* __X32_SYSCALL_BIT */ 0x40000000)
#    else
#      define systemd_NR_{syscall} {nr_x86_64}
#    endif
#  elif !defined(missing_arch_template)
%s
#  endif
'''

DEF_TEMPLATE_C = '''\

/* may be an (invalid) negative number due to libseccomp, see PR 13319 */
#  if defined __NR_{syscall} && __NR_{syscall} >= 0
#    if defined systemd_NR_{syscall}
assert_cc(__NR_{syscall} == systemd_NR_{syscall});
#    endif
#  else
#    if defined __NR_{syscall}
#      undef __NR_{syscall}
#    endif
#    if defined systemd_NR_{syscall} && systemd_NR_{syscall} >= 0
#      define __NR_{syscall} systemd_NR_{syscall}
#    endif
#  endif
#endif'''

DEF_TEMPLATE = (DEF_TEMPLATE_A +
                DEF_TEMPLATE_B % '#    warning "{syscall}() syscall number is unknown for your architecture"' +
                DEF_TEMPLATE_C)

ARCH_CHECK = '''\
/* Note: if this code looks strange, this is because it is derived from the same
 * template as the per-syscall blocks below. */
''' + '\n'.join(line for line in DEF_TEMPLATE_B.splitlines()
                       if ' define ' not in line) % '''\
#    warning "Current architecture is missing from the template"
#    define missing_arch_template 1'''

def print_syscall_def(syscall, tables, out):
    mappings = {f'nr_{arch}':t.get(syscall, -1)
                for arch, t in tables.items()}
    print(DEF_TEMPLATE.format(syscall=syscall, **mappings),
          file=out)

def print_syscall_defs(syscalls, tables, out):
    print('''\
/* SPDX-License-Identifier: LGPL-2.1-or-later
 * This file is generated by src/basic/missing_syscalls.py. Do not edit!
 *
 * Use 'ninja -C build update-syscall-tables' to download new syscall tables,
 * and 'ninja -C build update-syscall-header' to regenerate this file.
 */
#pragma once
''',
          file=out)
    print(ARCH_CHECK, file=out)
    for syscall in syscalls:
        print_syscall_def(syscall, tables, out)

if __name__ == '__main__':
    output_file = sys.argv[1]
    arch_files = sys.argv[2:]
    out = open(output_file, 'wt')

    tables = parse_syscall_tables(arch_files)
    print_syscall_defs(SYSCALLS, tables, out)

    print(f'Wrote {output_file}')
