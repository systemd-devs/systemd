/* SPDX-License-Identifier: GPL-2.0+ WITH Linux-syscall-note */
#ifndef _MPTCP_H
#define _MPTCP_H

#include <linux/const.h>
#include <linux/types.h>

#define MPTCP_SUBFLOW_FLAG_MCAP_REM		_BITUL(0)
#define MPTCP_SUBFLOW_FLAG_MCAP_LOC		_BITUL(1)
#define MPTCP_SUBFLOW_FLAG_JOIN_REM		_BITUL(2)
#define MPTCP_SUBFLOW_FLAG_JOIN_LOC		_BITUL(3)
#define MPTCP_SUBFLOW_FLAG_BKUP_REM		_BITUL(4)
#define MPTCP_SUBFLOW_FLAG_BKUP_LOC		_BITUL(5)
#define MPTCP_SUBFLOW_FLAG_FULLY_ESTABLISHED	_BITUL(6)
#define MPTCP_SUBFLOW_FLAG_CONNECTED		_BITUL(7)
#define MPTCP_SUBFLOW_FLAG_MAPVALID		_BITUL(8)

enum {
        MPTCP_SUBFLOW_ATTR_UNSPEC,
        MPTCP_SUBFLOW_ATTR_TOKEN_REM,
        MPTCP_SUBFLOW_ATTR_TOKEN_LOC,
        MPTCP_SUBFLOW_ATTR_RELWRITE_SEQ,
        MPTCP_SUBFLOW_ATTR_MAP_SEQ,
        MPTCP_SUBFLOW_ATTR_MAP_SFSEQ,
        MPTCP_SUBFLOW_ATTR_SSN_OFFSET,
        MPTCP_SUBFLOW_ATTR_MAP_DATALEN,
        MPTCP_SUBFLOW_ATTR_FLAGS,
        MPTCP_SUBFLOW_ATTR_ID_REM,
        MPTCP_SUBFLOW_ATTR_ID_LOC,
        MPTCP_SUBFLOW_ATTR_PAD,
        __MPTCP_SUBFLOW_ATTR_MAX
};

#define MPTCP_SUBFLOW_ATTR_MAX (__MPTCP_SUBFLOW_ATTR_MAX - 1)

/* netlink interface */
#define MPTCP_PM_NAME		"mptcp_pm"
#define MPTCP_PM_CMD_GRP_NAME	"mptcp_pm_cmds"
#define MPTCP_PM_VER		0x1

/*
 * ATTR types defined for MPTCP
 */
enum {
        MPTCP_PM_ATTR_UNSPEC,

        MPTCP_PM_ATTR_ADDR,				/* nested address */
        MPTCP_PM_ATTR_RCV_ADD_ADDRS,			/* u32 */
        MPTCP_PM_ATTR_SUBFLOWS,				/* u32 */

        __MPTCP_PM_ATTR_MAX
};

#define MPTCP_PM_ATTR_MAX (__MPTCP_PM_ATTR_MAX - 1)

enum {
        MPTCP_PM_ADDR_ATTR_UNSPEC,

        MPTCP_PM_ADDR_ATTR_FAMILY,			/* u16 */
        MPTCP_PM_ADDR_ATTR_ID,				/* u8 */
        MPTCP_PM_ADDR_ATTR_ADDR4,			/* struct in_addr */
        MPTCP_PM_ADDR_ATTR_ADDR6,			/* struct in6_addr */
        MPTCP_PM_ADDR_ATTR_PORT,			/* u16 */
        MPTCP_PM_ADDR_ATTR_FLAGS,			/* u32 */
        MPTCP_PM_ADDR_ATTR_IF_IDX,			/* s32 */

        __MPTCP_PM_ADDR_ATTR_MAX
};

#define MPTCP_PM_ADDR_ATTR_MAX (__MPTCP_PM_ADDR_ATTR_MAX - 1)

#define MPTCP_PM_ADDR_FLAG_SIGNAL			(1 << 0)
#define MPTCP_PM_ADDR_FLAG_SUBFLOW			(1 << 1)
#define MPTCP_PM_ADDR_FLAG_BACKUP			(1 << 2)

enum {
        MPTCP_PM_CMD_UNSPEC,

        MPTCP_PM_CMD_ADD_ADDR,
        MPTCP_PM_CMD_DEL_ADDR,
        MPTCP_PM_CMD_GET_ADDR,
        MPTCP_PM_CMD_FLUSH_ADDRS,
        MPTCP_PM_CMD_SET_LIMITS,
        MPTCP_PM_CMD_GET_LIMITS,

        __MPTCP_PM_CMD_AFTER_LAST
};

#define MPTCP_INFO_FLAG_FALLBACK		_BITUL(0)
#define MPTCP_INFO_FLAG_REMOTE_KEY_RECEIVED	_BITUL(1)

struct mptcp_info {
        __u8	mptcpi_subflows;
        __u8	mptcpi_add_addr_signal;
        __u8	mptcpi_add_addr_accepted;
        __u8	mptcpi_subflows_max;
        __u8	mptcpi_add_addr_signal_max;
        __u8	mptcpi_add_addr_accepted_max;
        __u32	mptcpi_flags;
        __u32	mptcpi_token;
        __u64	mptcpi_write_seq;
        __u64	mptcpi_snd_una;
        __u64	mptcpi_rcv_nxt;
};

#endif /* _MPTCP_H */
