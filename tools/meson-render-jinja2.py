#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

import ast
import os
import re
import sys

import jinja2

def parse_config_h(filename):
    # Parse config.h file generated by meson.
    ans = {}
    for line in open(filename):
        m = re.match(r'#define\s+(\w+)\s+(.*)', line)
        if not m:
            continue
        a, b = m.groups()
        if b and b[0] in '0123456789"':
            b = ast.literal_eval(b)
        ans[a] = b
    return ans

def render(filename, defines):
    text = open(filename).read()
    template = jinja2.Template(text, trim_blocks=True, undefined=jinja2.StrictUndefined)
    return template.render(defines)

if __name__ == '__main__':
    defines = parse_config_h(sys.argv[1])
    output = render(sys.argv[2], defines)
    with open(sys.argv[3], 'w') as f:
        f.write(output)
        f.write('\n')
    info = os.stat(sys.argv[2])
    os.chmod(sys.argv[3], info.st_mode)
