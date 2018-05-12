/* SPDX-License-Identifier: LGPL-2.1+ */
#pragma once

/***
  This file is part of systemd.

  Copyright 2014 Lennart Poettering
***/

#include <inttypes.h>

#include "time-util.h"

#define SD_RESOLVED_DNS           (UINT64_C(1) << 0)
#define SD_RESOLVED_LLMNR_IPV4    (UINT64_C(1) << 1)
#define SD_RESOLVED_LLMNR_IPV6    (UINT64_C(1) << 2)
#define SD_RESOLVED_MDNS_IPV4     (UINT64_C(1) << 3)
#define SD_RESOLVED_MDNS_IPV6     (UINT64_C(1) << 4)
#define SD_RESOLVED_NO_CNAME      (UINT64_C(1) << 5)
#define SD_RESOLVED_NO_TXT        (UINT64_C(1) << 6)
#define SD_RESOLVED_NO_ADDRESS    (UINT64_C(1) << 7)
#define SD_RESOLVED_NO_SEARCH     (UINT64_C(1) << 8)
#define SD_RESOLVED_AUTHENTICATED (UINT64_C(1) << 9)

#define SD_RESOLVED_LLMNR         (SD_RESOLVED_LLMNR_IPV4|SD_RESOLVED_LLMNR_IPV6)
#define SD_RESOLVED_MDNS          (SD_RESOLVED_MDNS_IPV4|SD_RESOLVED_MDNS_IPV6)

#define SD_RESOLVED_PROTOCOLS_ALL (SD_RESOLVED_MDNS|SD_RESOLVED_LLMNR|SD_RESOLVED_DNS)

#define SD_RESOLVED_QUERY_TIMEOUT_USEC (120 * USEC_PER_SEC)

/* 127.0.0.53 in native endian */
#define INADDR_DNS_STUB ((in_addr_t) 0x7f000035U)
