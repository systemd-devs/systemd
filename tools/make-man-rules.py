#!/usr/bin/python3
#  -*- Mode: python; coding: utf-8; indent-tabs-mode: nil -*- */
#
#  This file is part of systemd.
#
#  Copyright 2013 Zbigniew Jędrzejewski-Szmek
#
#  systemd is free software; you can redistribute it and/or modify it
#  under the terms of the GNU Lesser General Public License as published by
#  the Free Software Foundation; either version 2.1 of the License, or
#  (at your option) any later version.
#
#  systemd is distributed in the hope that it will be useful, but
#  WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
#  Lesser General Public License for more details.
#
#  You should have received a copy of the GNU Lesser General Public License
#  along with systemd; If not, see <http://www.gnu.org/licenses/>.

from __future__ import print_function
import collections
import sys
import os.path
from xml_helper import *

SECTION = '''\
MANPAGES += \\
	{manpages}
MANPAGES_ALIAS += \\
	{aliases}
{rules}
{htmlrules}
'''

CONDITIONAL = '''\
if {conditional}
''' \
+ SECTION + \
'''\
endif
'''

HEADER = '''\
# Do not edit. Generated by make-man-rules.py.
# To regenerate:
#   1. Create, update, or remove source .xml files in man/
#   2. Run 'make update-man-list'
#   3. Run 'make man' to generate manpages
#
# To make a man page conditional on a configure switch add
# attribute conditional="ENABLE_WHAT" or conditional="WITH_WHAT"
# to <refentry> element.
'''

HTML_ALIAS_RULE = '''\
{}.html: {}.html
	$(html-alias)
'''

FOOTER = '''\

# Really, do not edit this file.

EXTRA_DIST += \\
	{dist_files}
'''

def man(page, number):
    return 'man/{}.{}'.format(page, number)

def xml(file):
    return 'man/{}'.format(os.path.basename(file))

def add_rules(rules, name):
    xml = xml_parse(name)
    # print('parsing {}'.format(name), file=sys.stderr)
    if xml.getroot().tag != 'refentry':
        return
    conditional = xml.getroot().get('conditional') or ''
    rulegroup = rules[conditional]
    refmeta = xml.find('./refmeta')
    title = refmeta.find('./refentrytitle').text
    number = refmeta.find('./manvolnum').text
    refnames = xml.findall('./refnamediv/refname')
    target = man(refnames[0].text, number)
    if title != refnames[0].text:
        raise ValueError('refmeta and refnamediv disagree: ' + name)
    for refname in refnames:
        assert all(refname not in group
                   for group in rules.values()), "duplicate page name"
        alias = man(refname.text, number)
        rulegroup[alias] = target
        # print('{} => {} [{}]'.format(alias, target, conditional), file=sys.stderr)

def create_rules(xml_files):
    " {conditional => {alias-name => source-name}} "
    rules = collections.defaultdict(dict)
    for name in xml_files:
        try:
            add_rules(rules, name)
        except Exception:
            print("Failed to process", name, file=sys.stderr)
            raise
    return rules

def mjoin(files):
    return ' \\\n\t'.join(sorted(files) or '#')

def make_makefile(rules, dist_files):
    return HEADER + '\n'.join(
        (CONDITIONAL if conditional else SECTION).format(
            manpages=mjoin(set(rulegroup.values())),
            aliases=mjoin(k for k,v in rulegroup.items() if k != v),
            rules='\n'.join('{}: {}'.format(k,v)
                            for k,v in sorted(rulegroup.items())
                            if k != v),
            htmlrules='\n'.join(HTML_ALIAS_RULE.format(k[:-2],v[:-2])
                                for k,v in sorted(rulegroup.items())
                                if k != v),
            conditional=conditional)
        for conditional,rulegroup in sorted(rules.items())
        ) + FOOTER.format(dist_files=mjoin(sorted(dist_files)))

if __name__ == '__main__':
    rules = create_rules(sys.argv[1:])
    dist_files = (xml(file) for file in sys.argv[1:]
                  if not file.endswith(".directives.xml") and
                     not file.endswith(".index.xml"))
    print(make_makefile(rules, dist_files), end='')
