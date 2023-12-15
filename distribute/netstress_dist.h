/* **********************************************************
 * Copyright (C) 2004 VMware, Inc. All Rights Reserved
 * -- VMware Confidential.
 * **********************************************************/


#ifndef _VMK_NET_STRES_DIST_H_
#define _VMK_NET_STRES_DIST_H_

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_DISTRIBUTE
#define INCLUDE_ALLOW_VMK_MODULE
#include "includeCheck.h"

#define VMK_NET_STRESS_RELEASE_OPTIONS_ESX3                                       \

#define VMK_NET_STRESS_DEBUG_OPTIONS_ESX3                                         \
   VMK_STRESS_DECL_OPTION(NET_PKT_ALLOC_FAIL,                                     \
                          "NetFailPktAlloc",                                      \
			  0, -1, 0, 150, 0,                                       \
                          "Fail every nth pkt alloc")                             \
   VMK_STRESS_DECL_OPTION(NET_COPY_TO_SGMA_FAIL,                                  \
                          "NetFailCopyToSGMA",                                    \
			  0, -1, 0, 150, 0,                                       \
                          "Fail every nth copy to SGMA")                          \
   VMK_STRESS_DECL_OPTION(NET_COPY_FROM_SGMA_FAIL,                                \
                          "NetFailCopyFromSGMA",                                  \
			  0, -1, 0, 150, 0,                                       \
                          "Fail every nth copy from SGMA")                        \
   VMK_STRESS_DECL_OPTION(NET_PRIV_HDR_MEM_FAIL,                                  \
                          "NetFailPrivHdr",                                       \
			  0, -1, 0, 150, 0,                                       \
                          "Fail every nth call to create a private header")       \
   VMK_STRESS_DECL_OPTION(NET_PART_COPY_FAIL,                                     \
                          "NetFailPartialCopy",                                   \
			  0, -1, 0, 150, 0,                                       \
                          "Fail every nth partial copy")                          \
   VMK_STRESS_DECL_OPTION(NET_PKT_CLONE_FAIL,                                     \
                          "NetFailPktClone",                                      \
			  0, -1, 0, 150, 0,                                       \
                          "Fail every nth call to clone")                         \
   VMK_STRESS_DECL_OPTION(NET_PKT_FRAME_COPY_FAIL,                                \
                          "NetFailPktFrameCopy",                                  \
			  0, -1, 0, 150, 0,                                       \
                          "Fail every nth call to copy a frame")                  \
   VMK_STRESS_DECL_OPTION(NET_PKT_COPY_BYTES_IN_FAIL,                             \
                          "NetFailPktCopyBytesIn",                                \
                          0, -1, 0, 100, 0,                                       \
                          "Fail every nth call to copy bytes into a handle")      \
   VMK_STRESS_DECL_OPTION(NET_PKT_COPY_BYTES_OUT_FAIL,                            \
                          "NetFailPktCopyBytesOut",                               \
                          0, -1, 0, 100, 0,                                       \
                          "Fail every nth call to copy bytes out of a handle")    \
   VMK_STRESS_DECL_OPTION(NET_PKT_APPEND_FRAG_FAIL,                               \
                          "NetFailPktAppendFrag",                                 \
                          0, -1, 0, 100, 0,                                       \
                          "Fail every nth call to append an SG element to a"      \
                          " handle")                                              \
   VMK_STRESS_DECL_OPTION(NET_PKTLIST_CLONE_FAIL,                                 \
                          "NetFailPktlistClone",                                  \
                          0, -1, 0, 100, 0,                                       \
                          "Fail every nth call to clone a packet list")           \
   VMK_STRESS_DECL_OPTION(NET_PKTLIST_COPY_FAIL,                                  \
                          "NetFailPktlistCopy",                                   \
                          0, -1, 0, 100, 0,                                       \
                          "Fail every nth call to copy a packet list")            \
   VMK_STRESS_DECL_OPTION(NET_IOCHAIN_INSERT_FAIL,                                \
                          "NetFailIOChainInsert",                                 \
                          0, -1, 0, 100, 0,                                       \
                          "Fail every nth call to insert a link in an iochain")   \
   VMK_STRESS_DECL_OPTION(NET_IOCHAIN_GET_START_FAIL,                             \
                          "NetFailIOChainGetStart",                               \
                          0, -1, 0, 100, 0,                                       \
                          "Fail every nth call to get the first active link in "  \
                          "an iochain")                                           \
   VMK_STRESS_DECL_OPTION(NET_IOCHAIN_RESUME_FAIL,                                \
                          "NetFailIOChainResume",                                 \
                          0, -1, 0, 100, 0,                                       \
                          "Fail every nth call to resume an iochain")             \
   VMK_STRESS_DECL_OPTION(NET_PORT_CONNECT_FAIL,                                  \
                          "NetFailPortConnect",                                   \
                          0, -1, 0, 100, 0,                                       \
                          "Fail every nth call to port connect")                  \
   VMK_STRESS_DECL_OPTION(NET_PORT_WORLD_ASSOC_FAIL,                              \
                          "NetFailPortWorldAssoc",                                \
                          0, -1, 0, 100, 0,                                       \
                          "Fail every nth call that associates a world with a "   \
                          "port")                                                 \
   VMK_STRESS_DECL_OPTION(NET_PORT_ENABLE_FAIL,                                   \
                          "NetFailPortEnable",                                    \
                          0, -1, 0, 100, 0,                                       \
                          "Fail every nth call to enable a port")                 \
   VMK_STRESS_DECL_OPTION(NET_PORT_INPUT_RESUME_FAIL,                             \
                          "NetFailPortInputResume",                               \
                          0, -1, 0, 100, 0,                                       \
                          "Fail every nth call to resume input to a port")        \
   VMK_STRESS_DECL_OPTION(NET_PORTSET_PROC_CREATE_FAIL,                           \
                          "NetFailPortsetProcCreate",                             \
                          0, -1, 0, 100, 0,                                       \
                          "Fail every nth call to create proc nodes for "         \
                          "portsets")                                             \
   VMK_STRESS_DECL_OPTION(NET_PORTSET_ACTIVATE_FAIL,                              \
                          "NetFailPortsetActivate",                               \
                          0, -1, 0, 100, 0,                                       \
                          "Fail every nth call to activate a portset")            \
   VMK_STRESS_DECL_OPTION(NET_PORTSET_ACTIVATE_MEM_FAIL,                          \
                          "NetFailPortsetActivateMemAlloc",                       \
                          0, -1, 0, 100, 0,                                       \
                          "Fail every nth call to allocate memory inside portset" \
                          " activate")                                            \
   VMK_STRESS_DECL_OPTION(NET_PORTSET_DISABLE_PORT_FAIL,                          \
                          "NetFailPortsetDisablePort",                            \
                          0, -1, 0, 100, 0,                                       \
                          "Fail every nth call to disable a port in a portset")   \
   VMK_STRESS_DECL_OPTION(NET_PORTSET_ENABLE_PORT_FAIL,                           \
                          "NetFailPortsetEnablePort",                             \
                          0, -1, 0, 100, 0,                                       \
                          "Fail every nth call to enable a port in a portset")    \
   VMK_STRESS_DECL_OPTION(NET_PORTSET_CONNECT_PORT_FAIL,                          \
                          "NetFailPortsetConnectPort",                            \
                          0, -1, 0, 100, 0,                                       \
                          "Fail every nth call to connect to a port in a portset")\
   VMK_STRESS_DECL_OPTION(NET_HUB_PORT_OUTPUT_FAIL,                               \
                          "NetFailHubPortOutput",                                 \
                          0, -1, 0, 100, 0,                                       \
                          "Fail every nth call to send out a packet list on a "   \
                          "port of a hub ")                                       \
   VMK_STRESS_DECL_OPTION(NET_HUB_UPLINK_CONNECT_FAIL,                            \
                          "NetFailHubUplinkConnect",                              \
                          0, -1, 0, 100, 0,                                       \
                          "Fail every nth call to connect a hub to an uplink")    \
   VMK_STRESS_DECL_OPTION(NET_HUB_ACTIVATE_FAIL,                                  \
                          "NetFailHubActivate",                                   \
                          0, -1, 0, 100, 0,                                       \
                          "Fail every nth call to activate a hub")                \
   VMK_STRESS_DECL_OPTION(NET_PORT_INPUT_CORRUPT,                                 \
                          "NetCorruptPortInput",                                  \
                          0, -1, 0, 100, 0,                                       \
                          "Fail a packet in every nth call to input a packet "    \
                          "list to a port")                                       \
   VMK_STRESS_DECL_OPTION(NET_PORT_OUTPUT_CORRUPT,                                \
                          "NetCorruptPortOutput",                                 \
                          0, -1, 0, 100, 0,                                       \
                          "Fail a packet in every nth call to output a packet "   \
                          "list to a port")                                       \
   VMK_STRESS_DECL_OPTION(NET_KSEG_FAIL,                                          \
                          "NetFailKseg",                                          \
                          0, -1, 0, 100, 0,                                       \
                          "Fail every nth request to kseg an MA range")           \


#define VMK_NET_STRESS_RELEASE_OPTIONS_COMMON                                     \

#ifdef ESX3_NETWORKING_NOT_DONE_YET

/*
 * The difference between the ESX3_NETWORKING_NOT_DONE_YET version of the stress
 * options and the non-ESX3_NETWORKING_NOT_DONE_YET ones is that the former set
 * has the defaults for its members set to '0'. This prevents these options from
 * getting turned on by default. Once, sufficient QA'ing has been done on the new
 * architecture and a reasonable measure of confidence in the stability of the
 * code attained, non-zero defaults can be reverted to.
 */

#define VMK_NET_STRESS_DEBUG_OPTIONS_COMMON                                       \
   VMK_STRESS_DECL_OPTION(NET_HW_FAIL_HARD_TX,                                    \
                          "NetHwFailHardTx",                                      \
			  0, -1, 0, 10, 1,                                        \
			  "Cause every Nth packet to be dropped during hard "     \
                          "transmit.")                                            \
   VMK_STRESS_DECL_OPTION(NET_HW_CORRUPT_TX,                                      \
                          "NetHwCorruptTx",                                       \
			  0, -1, 0, 10, 1,                                        \
			  "Corrupt every Nth transmit frame in the hardware "     \
                          "driver before transmitting it.")                       \
   VMK_STRESS_DECL_OPTION(NET_HW_CORRUPT_RX,                                      \
                          "NetHwCorruptRx",                                       \
			  0, -1, 0, 10, 1,                                        \
			  "Corrupt every Nth Rx frame in the hardware driver "    \
                          "before sending it up the stack.")                      \
   VMK_STRESS_DECL_OPTION(NET_HW_FAIL_RX,                                         \
                          "NetHwFailRx",                                          \
			  0, -1, 0, 10, 1,                                        \
			  "Refuse to pass every Nth packet up the stack.")        \
   VMK_STRESS_DECL_OPTION(NET_HW_INTR_RATE,                                       \
                          "NetHwIntrRate",                                        \
			  0, 0x64, 0, 10, 0,                                      \
			  "Modify the hardware interrupt rate dynamically. "      \
                          "This feature can be used to control the clustering "   \
                          "of interrupts. A low value indicates a very high "     \
                          "interrupt rate. Setting it to 0 currently has no "     \
                          "effect (it retains the last settings.)")               \
   VMK_STRESS_DECL_OPTION(NET_HW_STOP_QUEUE,                                      \
                          "NetHwStopQueue",                                       \
			  0, -1, 0, 10, 1,                                        \
			  "Stop driver queue.")                                   \
   VMK_STRESS_DECL_OPTION(NET_HW_RETAIN_BUF,                                      \
			  "NetHwRetainBuffer",                                    \
			  0, -1, 0, 10, 0,                                        \
			  "Avoid freeing every Nth Tx and Rx buffer. The "        \
                          "buffers are put in a free list and freed "             \
                          "periodically")                                         \
   VMK_STRESS_DECL_OPTION(NET_IF_CORRUPT_RX_TCP_UDP,                              \
                          "NetIfCorruptRxTcpUdp",                                 \
			  0, -1, 0, 10, 0,                                        \
			  "Corrupt the first UDP/TCP/IP headers of every Nth "    \
                          "receive buffer at the interface.")                     \
   VMK_STRESS_DECL_OPTION(NET_IF_CORRUPT_RX_DATA,                                 \
			  "NetIfCorruptRxData",                                   \
			  0, -1, 0, 500, 1,                                       \
                          "Corrupt the data in every Nth frame.")                 \
   VMK_STRESS_DECL_OPTION(NET_IF_CORRUPT_ETHERNET_HDR,                            \
                          "NetIfCorruptEthHdr",                                   \
                          0, -1, 0, 100, 1,                                       \
                          "Corrupt the Ethernet header of every Nth frame")       \
   VMK_STRESS_DECL_OPTION(NET_IF_FAIL_RX,                                         \
                          "NetIfFailRx",                                          \
			  0, -1, 0, 10, 0,                                        \
			  "Refuse to push every Nth packet up the stack.")        \
   VMK_STRESS_DECL_OPTION(NET_IF_CORRUPT_TX,                                      \
                          "NetIfCorruptTx",                                       \
			  0, -1, 0, 10, 1,                                        \
			  "Corrupt every Nth transmit frame at the interface.")   \
   VMK_STRESS_DECL_OPTION(NET_IF_FAIL_HARD_TX,                                    \
                          "NetIfFailHardTx",                                      \
			  0, -1, 0, 10, 1,                                        \
			  "Fail to call the device's hard xmit function.")        \

#else //!ESX3_NETWORKING_NOT_DONE_YET

#define VMK_NET_STRESS_DEBUG_OPTIONS_COMMON                                       \
   VMK_STRESS_DECL_OPTION(NET_COPY_PACKET,                                        \
                          "NetVmkCopyPacket",                                     \
                          0, -1, 1000, 50, 1,                                     \
                          "Force vmkernel to copy at least every Nth packet.")    \
   VMK_STRESS_DECL_OPTION(NET_CORRUPT_TX,                                         \
		          "NetVmkCorruptTxFrame",                                 \
			  0, -1, 0, 100, 1,                                       \
			  "Force data corruption in every Nth transmit frame.")   \
   VMK_STRESS_DECL_OPTION(NET_CORRUPT_RX,                                         \
		          "NetVmkCorruptRxFrame",                                 \
			  0, -1, 0, 100, 1,                                       \
			  "Force data corruption in every Nth receive frame.")    \
   VMK_STRESS_DECL_OPTION(NET_FAIL_RX,                                            \
                          "NetVmkFailRx",                                         \
			  0, -1, 10000, 100, 1,                                   \
			  "Fail every Nth packet after grabbing ring entry.")     \
   VMK_STRESS_DECL_OPTION(NET_BIG_RX,                                             \
                          "NetVmkBigRx",                                          \
			  0, -1, 0, 100, 0,                                       \
			  "Artificially enlarge every Nth packet received.")      \
   VMK_STRESS_DECL_OPTION(NET_FAIL_LOCAL_TX,                                      \
                          "NetVmkFailLocalTX",                                    \
			  0, -1, 10000, 10, 1,                                    \
			  "Fail to queue every Nth local transmit for receive.")  \
   VMK_STRESS_DECL_OPTION(NET_INTR_RATE,                                          \
                          "NetVmkIntrRate",                                       \
			  0, -1, 0, 1, 0,                                         \
			  "Post interrupt actions at a rate proportional to N.")  \
   VMK_STRESS_DECL_OPTION(NET_KSEG_RX,                                            \
                          "NetVmkFailKsegRx",                                     \
			  0, -1, 10000, 100, 1,                                   \
			  "Fail to kseg guest buffers for every Nth receive.")    \
   VMK_STRESS_DECL_OPTION(NET_KSEG_TX,                                            \
                          "NetVmkFailKsegTx",                                     \
			  0, -1, 10000, 10, 1,                                    \
			  "Fail to kseg guest buffers for every Nth transmit.")   \
   VMK_STRESS_DECL_OPTION(NET_HW_FAIL_HARD_TX,                                    \
                          "NetHwFailHardTx",                                      \
			  0, -1, 10000, 10, 1,                                    \
			  "Cause every Nth packet to be dropped during hard "     \
                          "transmit.")                                            \
   VMK_STRESS_DECL_OPTION(NET_HW_CORRUPT_TX,                                      \
                          "NetHwCorruptTx",                                       \
			  0, -1, 0, 10, 1,                                        \
			  "Corrupt every Nth transmit frame in the hardware "     \
                          "driver before transmitting it.")                       \
   VMK_STRESS_DECL_OPTION(NET_HW_CORRUPT_RX,                                      \
                          "NetHwCorruptRx",                                       \
			  0, -1, 10000, 10, 1,                                    \
			  "Corrupt every Nth Rx frame in the hardware driver "    \
                          "before sending it up the stack.")                      \
   VMK_STRESS_DECL_OPTION(NET_HW_FAIL_RX,                                         \
                          "NetHwFailRx",                                          \
			  0, -1, 10000, 10, 1,                                    \
			  "Refuse to pass every Nth packet up the stack.")        \
   VMK_STRESS_DECL_OPTION(NET_HW_INTR_RATE,                                       \
                          "NetHwIntrRate",                                        \
			  0, 0x64, 0, 10, 0,                                      \
			  "Modify the hardware interrupt rate dynamically. "      \
                          "This feature can be used to control the clustering "   \
                          "of interrupts. A low value indicates a very high "     \
                          "interrupt rate. Setting it to 0 currently has no "     \
                          "effect (it retains the last settings.)")               \
   VMK_STRESS_DECL_OPTION(NET_HW_STOP_QUEUE,                                      \
                          "NetHwStopQueue",                                       \
			  0, -1, 0, 10, 1,                                        \
			  "Stop driver queue.")                                   \
   VMK_STRESS_DECL_OPTION(NET_HW_RETAIN_BUF,                                      \
			  "NetHwRetainBuffer",                                    \
			  0, -1, 0, 10, 0,                                        \
			  "Avoid freeing every Nth Tx and Rx buffer. The "        \
                          "buffers are put in a free list and freed "             \
                          "periodically")                                         \
   VMK_STRESS_DECL_OPTION(NET_IF_CORRUPT_RX_TCP_UDP,                              \
                          "NetIfCorruptRxTcpUdp",                                 \
			  0, -1, 10000, 10, 0,                                    \
			  "Corrupt the first UDP/TCP/IP headers of every Nth "    \
                          "receive buffer at the interface.")                     \
   VMK_STRESS_DECL_OPTION(NET_IF_CORRUPT_RX_DATA,                                 \
			  "NetIfCorruptRxData",                                   \
			  0, -1, 10000, 500, 1,                                   \
                          "Corrupt the data in every Nth frame.")                 \
   VMK_STRESS_DECL_OPTION(NET_IF_CORRUPT_ETHERNET_HDR,                            \
                          "NetIfCorruptEthHdr",                                   \
                          0, -1, 10000, 100, 1,                                   \
                          "Corrupt the Ethernet header of every Nth frame")       \
   VMK_STRESS_DECL_OPTION(NET_IF_FAIL_RX,                                         \
                          "NetIfFailRx",                                          \
			  0, -1, 10000, 10, 0,                                    \
			  "Refuse to push every Nth packet up the stack.")        \
   VMK_STRESS_DECL_OPTION(NET_IF_CORRUPT_TX,                                      \
                          "NetIfCorruptTx",                                       \
			  0, -1, 0, 10, 1,                                        \
			  "Corrupt every Nth transmit frame at the interface.")   \
   VMK_STRESS_DECL_OPTION(NET_IF_FAIL_HARD_TX,                                    \
                          "NetIfFailHardTx",                                      \
			  0, -1, 10000, 10, 1,                                    \
			  "Fail to call the device's hard xmit function.")        \

#endif //ESX3_NETWORKING_NOT_DONE_YET

#define VMK_NET_STRESS_DEBUG_OPTIONS                                              \
   VMK_NET_STRESS_DEBUG_OPTIONS_COMMON                                            \
   VMK_NET_STRESS_DEBUG_OPTIONS_ESX3                                              \

#define VMK_NET_STRESS_RELEASE_OPTIONS                                            \
   VMK_NET_STRESS_RELEASE_OPTIONS_COMMON                                          \
   VMK_NET_STRESS_RELEASE_OPTIONS_ESX3                                            \

#endif //_VMK_NET_STRES_DIST_H_
