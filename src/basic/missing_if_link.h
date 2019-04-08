/* SPDX-License-Identifier: LGPL-2.1+ */
#pragma once

#if !HAVE_IFLA_INET6_ADDR_GEN_MODE /* linux@bc91b0f07ada5535427373a4e2050877bcc12218 (3.17) */
#define IFLA_INET6_ADDR_GEN_MODE 8

#undef  IFLA_INET6_MAX
#define IFLA_INET6_MAX           8

enum in6_addr_gen_mode {
        IN6_ADDR_GEN_MODE_EUI64,
        IN6_ADDR_GEN_MODE_NONE,
        IN6_ADDR_GEN_MODE_STABLE_PRIVACY,
        IN6_ADDR_GEN_MODE_RANDOM,
};
#else
#if !HAVE_IN6_ADDR_GEN_MODE_STABLE_PRIVACY /* linux@622c81d57b392cc9be836670eb464a4dfaa9adfe (4.1) */
#define IN6_ADDR_GEN_MODE_STABLE_PRIVACY 2
#endif
#if !HAVE_IN6_ADDR_GEN_MODE_RANDOM /* linux@cc9da6cc4f56e05cc9e591459fe0192727ff58b3 (4.5) */
#define IN6_ADDR_GEN_MODE_RANDOM         3
#endif
#endif /* !HAVE_IFLA_INET6_ADDR_GEN_MODE */

#if !HAVE_IFLA_IPVLAN_MODE /* linux@2ad7bf3638411cb547f2823df08166c13ab04269 (3.19) */
enum {
        IFLA_IPVLAN_UNSPEC,
        IFLA_IPVLAN_MODE,
        IFLA_IPVLAN_FLAGS,
        __IFLA_IPVLAN_MAX
};
#define IFLA_IPVLAN_MAX (__IFLA_IPVLAN_MAX - 1)
enum ipvlan_mode {
        IPVLAN_MODE_L2 = 0,
        IPVLAN_MODE_L3,
        IPVLAN_MODE_L3S,
        IPVLAN_MODE_MAX
};
#else
#if !HAVE_IPVLAN_MODE_L3S /* linux@4fbae7d83c98c30efcf0a2a2ac55fbb75ef5a1a5 (4.9) */
#define IPVLAN_MODE_L3S   2
#define IPVLAN_MODE_MAX   3
#endif
#if !HAVE_IFLA_IPVLAN_FLAGS /* linux@a190d04db93710ae166749055b6985397c6d13f5 (4.15) */
#define IFLA_IPVLAN_FLAGS 2

#undef  IFLA_IPVLAN_MAX
#define IFLA_IPVLAN_MAX   2
#endif
#endif /* !HAVE_IFLA_IPVLAN_MODE */

/* linux@a190d04db93710ae166749055b6985397c6d13f5 (4.15) */
#ifndef IPVLAN_F_PRIVATE
#define IPVLAN_F_PRIVATE 0x01
#endif

/* linux@fe89aa6b250c1011ccf425fbb7998e96bd54263f (4.15) */
#ifndef IPVLAN_F_VEPA
#define IPVLAN_F_VEPA    0x02
#endif

#if !HAVE_IFLA_PHYS_PORT_ID /* linux@66cae9ed6bc46b8cc57a9693f99f69926f3cc7ef (3.12) */
#define IFLA_PHYS_PORT_ID       34
#endif
#if !HAVE_IFLA_CARRIER_CHANGES /* linux@2d3b479df41a10e2f41f9259fcba775bd34de6e4 (3.15) */
#define IFLA_CARRIER_CHANGES    35
#endif
#if !HAVE_IFLA_PHYS_SWITCH_ID /* linux@82f2841291cfaf4d225aa1766424280254d3e3b2 (3.19) */
#define IFLA_PHYS_SWITCH_ID     36
#endif
#if !HAVE_IFLA_LINK_NETNSID /* linux@d37512a277dfb2cef8a578e25a3246f61399a55a (4.0) */
#define IFLA_LINK_NETNSID       37
#endif
#if !HAVE_IFLA_PHYS_PORT_NAME /* linux@db24a9044ee191c397dcd1c6574f56d67d7c8df5 (4.1) */
#define IFLA_PHYS_PORT_NAME     38
#endif
#if !HAVE_IFLA_PROTO_DOWN /* linux@88d6378bd6c096cb8440face3ae3f33d55a2e6e4 (4.3) */
#define IFLA_PROTO_DOWN         39
#endif
#if !HAVE_IFLA_GSO_MAX_SIZE /* linux@c70ce028e834f8e51306217dbdbd441d851c64d3 (4.6) */
#define IFLA_GSO_MAX_SEGS       40
#define IFLA_GSO_MAX_SIZE       41
#endif
#if !HAVE_IFLA_PAD /* linux@18402843bf88c2e9674e1a3a05c73b7d9b09ee05 (4.7) */
#define IFLA_PAD                42
#endif
#if !HAVE_IFLA_XDP /* linux@d1fdd9138682e0f272beee0cb08b6328c5478b26 (4.8) */
#define IFLA_XDP                43
#endif
#if !HAVE_IFLA_EVENT /* linux@3d3ea5af5c0b382bc9d9aed378fd814fb5d4a011 (4.13) */
#define IFLA_EVENT              44
#endif
#if !HAVE_IFLA_IF_NETNSID /* linux@6621dd29eb9b5e6774ec7a9a75161352fdea47fc, 79e1ad148c844f5c8b9d76b36b26e3886dca95ae (4.15) */
#define IFLA_IF_NETNSID         45
#define IFLA_NEW_NETNSID        46
#endif
#if !HAVE_IFLA_TARGET_NETNSID /* linux@19d8f1ad12fd746e60707a58d954980013c7a35a (4.20) */
#define IFLA_TARGET_NETNSID IFLA_IF_NETNSID
#endif
#if !HAVE_IFLA_NEW_IFINDEX /* linux@b2d3bcfa26a7a8de41f358a6cae8b848673b3c6e, 38e01b30563a5b5ade7b54e5d739d16a2b02fe82 (4.16) */
#define IFLA_CARRIER_UP_COUNT   47
#define IFLA_CARRIER_DOWN_COUNT 48
#define IFLA_NEW_IFINDEX        49
#endif
#if !HAVE_IFLA_MAX_MTU /* linux@3e7a50ceb11ea75c27e944f1a01e478fd62a2d8d (4.19) */
#define IFLA_MIN_MTU            50
#define IFLA_MAX_MTU            51

#undef  IFLA_MAX
#define IFLA_MAX                51
#endif

#if !HAVE_IFLA_BOND_MODE /* linux@90af231106c0b8d223c27d35464af95cb3d9cacf (3.13) */
#define IFLA_BOND_MODE              1
#endif
#if !HAVE_IFLA_BOND_ACTIVE_SLAVE /* linux@ec76aa49855f6d6fea5e01de179fb57dd47c619d (3.13) */
#define IFLA_BOND_ACTIVE_SLAVE      2
#endif
#if !HAVE_IFLA_BOND_AD_INFO /* linux@4ee7ac7526d4a9413cafa733d824edfe49fdcc46 (3.14) */
#define IFLA_BOND_MIIMON            3
#define IFLA_BOND_UPDELAY           4
#define IFLA_BOND_DOWNDELAY         5
#define IFLA_BOND_USE_CARRIER       6
#define IFLA_BOND_ARP_INTERVAL      7
#define IFLA_BOND_ARP_IP_TARGET     8
#define IFLA_BOND_ARP_VALIDATE      9
#define IFLA_BOND_ARP_ALL_TARGETS   10
#define IFLA_BOND_PRIMARY           11
#define IFLA_BOND_PRIMARY_RESELECT  12
#define IFLA_BOND_FAIL_OVER_MAC     13
#define IFLA_BOND_XMIT_HASH_POLICY  14
#define IFLA_BOND_RESEND_IGMP       15
#define IFLA_BOND_NUM_PEER_NOTIF    16
#define IFLA_BOND_ALL_SLAVES_ACTIVE 17
#define IFLA_BOND_MIN_LINKS         18
#define IFLA_BOND_LP_INTERVAL       19
#define IFLA_BOND_PACKETS_PER_SLAVE 20
#define IFLA_BOND_AD_LACP_RATE      21
#define IFLA_BOND_AD_SELECT         22
#define IFLA_BOND_AD_INFO           23
#endif
#if !HAVE_IFLA_BOND_AD_ACTOR_SYSTEM /* linux@171a42c38c6e1a5a076d6276e94e55a0b5b7868c (4.2) */
#define IFLA_BOND_AD_ACTOR_SYS_PRIO 24
#define IFLA_BOND_AD_USER_PORT_KEY  25
#define IFLA_BOND_AD_ACTOR_SYSTEM   26
#endif
#if !HAVE_IFLA_BOND_TLB_DYNAMIC_LB /* linux@0f7bffd9e512b77279bbce704fad3cb1d6887958 (4.3) */
#define IFLA_BOND_TLB_DYNAMIC_LB    27

#undef  IFLA_BOND_MAX
#define IFLA_BOND_MAX               27
#endif

#if !HAVE_IFLA_VXLAN_UDP_ZERO_CSUM6_RX /* linux@359a0ea9875ef4f32c8425bbe1ae348e1fd2ed2a (3.16) */
#define IFLA_VXLAN_UDP_CSUM          18
#define IFLA_VXLAN_UDP_ZERO_CSUM6_TX 19
#define IFLA_VXLAN_UDP_ZERO_CSUM6_RX 20
#endif
#if !HAVE_IFLA_VXLAN_REMCSUM_NOPARTIAL /* linux@dfd8645ea1bd91277f841e74c33e1f4dbbede808..0ace2ca89cbd6bcdf2b9d2df1fa0fa24ea9d1653 (4.0) */
#define IFLA_VXLAN_REMCSUM_TX        21
#define IFLA_VXLAN_REMCSUM_RX        22
#define IFLA_VXLAN_GBP               23
#define IFLA_VXLAN_REMCSUM_NOPARTIAL 24
#endif
#if !HAVE_IFLA_VXLAN_COLLECT_METADATA /* linux@f8a9b1bc1b238eed9987da747a0e52f5bb009980 (4.3) */
#define IFLA_VXLAN_COLLECT_METADATA  25
#endif
#if !HAVE_IFLA_VXLAN_LABEL /* linux@e7f70af111f086a20800ad2e17f544b2e3e0f375 (4.6) */
#define IFLA_VXLAN_LABEL             26
#endif
#if !HAVE_IFLA_VXLAN_GPE /* linux@e1e5314de08ba6003b358125eafc9ad9e75a950c (4.7) */
#define IFLA_VXLAN_GPE               27
#endif
#if !HAVE_IFLA_VXLAN_TTL_INHERIT /* linux@72f6d71e491e6ce269b564865b21fab0a4402dd3 (4.18) */
#define IFLA_VXLAN_TTL_INHERIT       28

#undef  IFLA_VXLAN_MAX
#define IFLA_VXLAN_MAX               28
#endif

#if !HAVE_IFLA_GENEVE_TOS /* linux@2d07dc79fe04a43d82a346ced6bbf07bdb523f1b..d89511251f6519599b109dc6cda87a6ab314ed8c (4.2) */
enum {
        IFLA_GENEVE_UNSPEC,
        IFLA_GENEVE_ID,
        IFLA_GENEVE_REMOTE,
        IFLA_GENEVE_TTL,
        IFLA_GENEVE_TOS,
        IFLA_GENEVE_PORT,        /* destination port */
        IFLA_GENEVE_COLLECT_METADATA,
        IFLA_GENEVE_REMOTE6,
        IFLA_GENEVE_UDP_CSUM,
        IFLA_GENEVE_UDP_ZERO_CSUM6_TX,
        IFLA_GENEVE_UDP_ZERO_CSUM6_RX,
        IFLA_GENEVE_LABEL,
        IFLA_GENEVE_TTL_INHERIT,
        __IFLA_GENEVE_MAX
};
#define IFLA_GENEVE_MAX        (__IFLA_GENEVE_MAX - 1)
#else
#if !HAVE_IFLA_GENEVE_COLLECT_METADATA /* linux@e305ac6cf5a1e1386aedce7ef9cb773635d5845c (4.3) */
#define IFLA_GENEVE_PORT              5
#define IFLA_GENEVE_COLLECT_METADATA  6
#endif
#if !HAVE_IFLA_GENEVE_REMOTE6 /* linux@8ed66f0e8235118a31720acdab3bbbe9debd0f6a (4.4) */
#define IFLA_GENEVE_REMOTE6           7
#endif
#if !HAVE_IFLA_GENEVE_UDP_ZERO_CSUM6_RX /* linux@abe492b4f50c3ae2ebcfaa2f5c16176aebaa1c68 (4.5) */
#define IFLA_GENEVE_UDP_CSUM          8
#define IFLA_GENEVE_UDP_ZERO_CSUM6_TX 9
#define IFLA_GENEVE_UDP_ZERO_CSUM6_RX 10
#endif
#if !HAVE_IFLA_GENEVE_LABEL /* linux@8eb3b99554b82da968d1fbc00df9f3156c5e2d63 (4.6) */
#define IFLA_GENEVE_LABEL             11
#endif
#if !HAVE_IFLA_GENEVE_TTL_INHERIT /* linux@52d0d404d39dd9eac71a181615d6ca15e23d8e38 (4.20) */
#define IFLA_GENEVE_TTL_INHERIT       12

#undef  IFLA_GENEVE_MAX
#define IFLA_GENEVE_MAX               12
#endif
#endif

#if !HAVE_IFLA_BR_MAX_AGE /* linux@e5c3ea5c668033b303e7ac835d7d91da32d97958 (3.18) */
enum {
        IFLA_BR_UNSPEC,
        IFLA_BR_FORWARD_DELAY,
        IFLA_BR_HELLO_TIME,
        IFLA_BR_MAX_AGE,
        IFLA_BR_AGEING_TIME,
        IFLA_BR_STP_STATE,
        IFLA_BR_PRIORITY,
        IFLA_BR_VLAN_FILTERING,
        IFLA_BR_VLAN_PROTOCOL,
        IFLA_BR_GROUP_FWD_MASK,
        IFLA_BR_ROOT_ID,
        IFLA_BR_BRIDGE_ID,
        IFLA_BR_ROOT_PORT,
        IFLA_BR_ROOT_PATH_COST,
        IFLA_BR_TOPOLOGY_CHANGE,
        IFLA_BR_TOPOLOGY_CHANGE_DETECTED,
        IFLA_BR_HELLO_TIMER,
        IFLA_BR_TCN_TIMER,
        IFLA_BR_TOPOLOGY_CHANGE_TIMER,
        IFLA_BR_GC_TIMER,
        IFLA_BR_GROUP_ADDR,
        IFLA_BR_FDB_FLUSH,
        IFLA_BR_MCAST_ROUTER,
        IFLA_BR_MCAST_SNOOPING,
        IFLA_BR_MCAST_QUERY_USE_IFADDR,
        IFLA_BR_MCAST_QUERIER,
        IFLA_BR_MCAST_HASH_ELASTICITY,
        IFLA_BR_MCAST_HASH_MAX,
        IFLA_BR_MCAST_LAST_MEMBER_CNT,
        IFLA_BR_MCAST_STARTUP_QUERY_CNT,
        IFLA_BR_MCAST_LAST_MEMBER_INTVL,
        IFLA_BR_MCAST_MEMBERSHIP_INTVL,
        IFLA_BR_MCAST_QUERIER_INTVL,
        IFLA_BR_MCAST_QUERY_INTVL,
        IFLA_BR_MCAST_QUERY_RESPONSE_INTVL,
        IFLA_BR_MCAST_STARTUP_QUERY_INTVL,
        IFLA_BR_NF_CALL_IPTABLES,
        IFLA_BR_NF_CALL_IP6TABLES,
        IFLA_BR_NF_CALL_ARPTABLES,
        IFLA_BR_VLAN_DEFAULT_PVID,
        IFLA_BR_PAD,
        IFLA_BR_VLAN_STATS_ENABLED,
        IFLA_BR_MCAST_STATS_ENABLED,
        IFLA_BR_MCAST_IGMP_VERSION,
        IFLA_BR_MCAST_MLD_VERSION,
        IFLA_BR_VLAN_STATS_PER_PORT,
        __IFLA_BR_MAX,
};

#define IFLA_BR_MAX        (__IFLA_BR_MAX - 1)
#else
#if !HAVE_IFLA_BR_PRIORITY /* linux@af615762e972be0c66cf1d156ca4fac13b93c0b0 (4.1) */
#define IFLA_BR_AGEING_TIME                4
#define IFLA_BR_STP_STATE                  5
#define IFLA_BR_PRIORITY                   6
#endif
#if !HAVE_IFLA_BR_VLAN_PROTOCOL /* linux@a7854037da006a7472c48773e3190db55217ec9b, d2d427b3927bd7a0348fc7f323d0e291f79a2779 (4.3) */
#define IFLA_BR_VLAN_FILTERING             7
#define IFLA_BR_VLAN_PROTOCOL              8
#endif
#if !HAVE_IFLA_BR_VLAN_DEFAULT_PVID /* linux@7910228b6bb35f3c8e0bc72a8d84c29616cb1b90..0f963b7592ef9e054974b6672b86ec1edd84b4bc (4.4) */
#define IFLA_BR_GROUP_FWD_MASK             9
#define IFLA_BR_ROOT_ID                    10
#define IFLA_BR_BRIDGE_ID                  11
#define IFLA_BR_ROOT_PORT                  12
#define IFLA_BR_ROOT_PATH_COST             13
#define IFLA_BR_TOPOLOGY_CHANGE            14
#define IFLA_BR_TOPOLOGY_CHANGE_DETECTED   15
#define IFLA_BR_HELLO_TIMER                16
#define IFLA_BR_TCN_TIMER                  17
#define IFLA_BR_TOPOLOGY_CHANGE_TIMER      18
#define IFLA_BR_GC_TIMER                   19
#define IFLA_BR_GROUP_ADDR                 20
#define IFLA_BR_FDB_FLUSH                  21
#define IFLA_BR_MCAST_ROUTER               22
#define IFLA_BR_MCAST_SNOOPING             23
#define IFLA_BR_MCAST_QUERY_USE_IFADDR     24
#define IFLA_BR_MCAST_QUERIER              25
#define IFLA_BR_MCAST_HASH_ELASTICITY      26
#define IFLA_BR_MCAST_HASH_MAX             27
#define IFLA_BR_MCAST_LAST_MEMBER_CNT      28
#define IFLA_BR_MCAST_STARTUP_QUERY_CNT    29
#define IFLA_BR_MCAST_LAST_MEMBER_INTVL    30
#define IFLA_BR_MCAST_MEMBERSHIP_INTVL     31
#define IFLA_BR_MCAST_QUERIER_INTVL        32
#define IFLA_BR_MCAST_QUERY_INTVL          33
#define IFLA_BR_MCAST_QUERY_RESPONSE_INTVL 34
#define IFLA_BR_MCAST_STARTUP_QUERY_INTVL  35
#define IFLA_BR_NF_CALL_IPTABLES           36
#define IFLA_BR_NF_CALL_IP6TABLES          37
#define IFLA_BR_NF_CALL_ARPTABLES          38
#define IFLA_BR_VLAN_DEFAULT_PVID          39
#endif
#if !HAVE_IFLA_BR_VLAN_STATS_ENABLED /* linux@12a0faa3bd76157b9dc096758d6818ff535e4586, 6dada9b10a0818ba72c249526a742c8c41274a73 (4.7) */
#define IFLA_BR_PAD                        40
#define IFLA_BR_VLAN_STATS_ENABLED         41
#endif
#if !HAVE_IFLA_BR_MCAST_STATS_ENABLED /* linux@1080ab95e3c7bdd77870e209aff83c763fdcf439 (4.8) */
#define IFLA_BR_MCAST_STATS_ENABLED        42
#endif
#if !HAVE_IFLA_BR_MCAST_MLD_VERSION /* linux@5e9235853d652a295d5f56cb8652950b6b5bf56b, aa2ae3e71c74cc00ec22f133dc900b3817415785 (4.10) */
#define IFLA_BR_MCAST_IGMP_VERSION         43
#define IFLA_BR_MCAST_MLD_VERSION          44
#endif
#if !HAVE_IFLA_BR_VLAN_STATS_PER_PORT /* linux@9163a0fc1f0c0980f117cc25f4fa6ba9b0750a36 (4.20) */
#define IFLA_BR_VLAN_STATS_PER_PORT        45

#undef  IFLA_BR_MAX
#define IFLA_BR_MAX                        45
#endif
#endif

#if !HAVE_IFLA_BRPORT_LEARNING_SYNC /* linux@958501163ddd6ea22a98f94fa0e7ce6d4734e5c4, efacacdaf7cb5a0592ed772e3731636b2742e34a (3.19)*/
#define IFLA_BRPORT_PROXYARP            10
#define IFLA_BRPORT_LEARNING_SYNC       11
#endif
#if !HAVE_IFLA_BRPORT_PROXYARP_WIFI /* linux@842a9ae08a25671db3d4f689eed68b4d64be15b5 (4.1) */
#define IFLA_BRPORT_PROXYARP_WIFI       12
#endif
#if !HAVE_IFLA_BRPORT_MULTICAST_ROUTER /* linux@4ebc7660ab4559cad10b6595e05f70562bb26dc5..5d6ae479ab7ddf77bb22bdf739268581453ff886 (4.4) */
#define IFLA_BRPORT_ROOT_ID             13
#define IFLA_BRPORT_BRIDGE_ID           14
#define IFLA_BRPORT_DESIGNATED_PORT     15
#define IFLA_BRPORT_DESIGNATED_COST     16
#define IFLA_BRPORT_ID                  17
#define IFLA_BRPORT_NO                  18
#define IFLA_BRPORT_TOPOLOGY_CHANGE_ACK 19
#define IFLA_BRPORT_CONFIG_PENDING      20
#define IFLA_BRPORT_MESSAGE_AGE_TIMER   21
#define IFLA_BRPORT_FORWARD_DELAY_TIMER 22
#define IFLA_BRPORT_HOLD_TIMER          23
#define IFLA_BRPORT_FLUSH               24
#define IFLA_BRPORT_MULTICAST_ROUTER    25
#endif
#if !HAVE_IFLA_BRPORT_PAD /* linux@12a0faa3bd76157b9dc096758d6818ff535e4586 (4.7) */
#define IFLA_BRPORT_PAD                 26
#endif
#if !HAVE_IFLA_BRPORT_MCAST_FLOOD /* linux@b6cb5ac8331b6bcfe9ce38c7f7f58db6e1d6270a (4.9) */
#define IFLA_BRPORT_MCAST_FLOOD         27
#endif
#if !HAVE_IFLA_BRPORT_VLAN_TUNNEL /* linux@6db6f0eae6052b70885562e1733896647ec1d807, b3c7ef0adadc5768e0baa786213c6bd1ce521a77 (4.11) */
#define IFLA_BRPORT_MCAST_TO_UCAST      28
#define IFLA_BRPORT_VLAN_TUNNEL         29
#endif
#if !HAVE_IFLA_BRPORT_BCAST_FLOOD /* linux@99f906e9ad7b6e79ffeda30f45906a8448b9d6a2 (4.12) */
#define IFLA_BRPORT_BCAST_FLOOD         30
#endif
#if !HAVE_IFLA_BRPORT_NEIGH_SUPPRESS /* linux@5af48b59f35cf712793badabe1a574a0d0ce3bd3, 821f1b21cabb46827ce39ddf82e2789680b5042a (4.15) */
#define IFLA_BRPORT_GROUP_FWD_MASK      31
#define IFLA_BRPORT_NEIGH_SUPPRESS      32
#endif
#if !HAVE_IFLA_BRPORT_ISOLATED /* linux@7d850abd5f4edb1b1ca4b4141a4453305736f564 (4.18) */
#define IFLA_BRPORT_ISOLATED            33
#endif
#if !HAVE_IFLA_BRPORT_BACKUP_PORT /* linux@2756f68c314917d03eb348084edb08bb929139d9 (4.19) */
#define IFLA_BRPORT_BACKUP_PORT         34

#undef  IFLA_BRPORT_MAX
#define IFLA_BRPORT_MAX                 34
#endif

#if !HAVE_IFLA_VRF_TABLE /* linux@4e3c89920cd3a6cfce22c6f537690747c26128dd (4.3) */
enum {
        IFLA_VRF_UNSPEC,
        IFLA_VRF_TABLE,
        __IFLA_VRF_MAX
};
#define IFLA_VRF_MAX (__IFLA_VRF_MAX - 1)
#endif
