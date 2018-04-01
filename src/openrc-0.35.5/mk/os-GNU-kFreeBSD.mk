# Copyright (c) 2013-2015 The OpenRC Authors.
# See the Authors file at the top-level directory of this distribution and
# https://github.com/OpenRC/openrc/blob/master/AUTHORS
#
# This file is part of OpenRC. It is subject to the license terms in
# the LICENSE file found in the top-level directory of this
# distribution and at https://github.com/OpenRC/openrc/blob/master/LICENSE
# This file may not be copied, modified, propagated, or distributed
# except according to the terms contained in the LICENSE file.

# Generic definitions

SFX=		.GNU-kFreeBSD.in
PKG_PREFIX?=	/usr

CPPFLAGS+=	-D_BSD_SOURCE
LIBDL=		-Wl,-Bdynamic -ldl
LIBKVM?=
