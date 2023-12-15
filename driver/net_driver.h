/* **********************************************************
 * Portions Copyright 2004 VMware, Inc. 
 * **********************************************************/

/*
 * net_driver.h  --
 *
 *    Exported interface to vmkernel networking for physical
 *    device drivers.
 *
 *    XXX TODO: pull junk out of netdevice.h in console_os
 *
 */

#ifndef _NET_DRIVER_H_
#define _NET_DRIVER_H_

#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMK_MODULE
#define INCLUDE_ALLOW_DISTRIBUTE
#include "includeCheck.h"

#include "vm_basic_types.h"
#include "return_status.h"
#include "idt_dist.h"

#define NETDEV_LINK_DOWN  0
#define NETDEV_LINK_UP    1
#define NETDEV_LINK_UNK  -1

typedef struct Net_Beacon_Stats {
   uint32       rxSuccess;
   uint32       rxTaggedBeacon;
   uint32       rxUmTag;
   uint32       rxUnmatchedLen;
   uint32       rxUnmatchedMagic;
   uint32       rxUnmatchedServer;
   uint32       rxLoopDetected;

   uint32       txSuccess;
   uint32       txTaggedBeacon;
   uint32       txFailure;
   uint32       txLinkDown; // Not applied to bond device beacon xmit.
} Net_Beacon_Stats;

typedef struct Net_Vlan_Stats {
   uint32       xmitSwTagged;
   uint32       xmitHwAccel;
   uint32       recvSwUntagged;
   uint32       recvHwAccel;

   uint32       xmitErrNoCapability;
   uint32       recvErrHandleNoCapability; // vlan handle but vmnic no tagging capability
   uint32       recvErrHandleNoVlan;       // handle does not support vlan
   uint32       recvErrNoTag;
   uint32       recvErrTagMismatch;
   uint32       recvErrOnPlainNic;

   uint32       recvNativeVlan;
   uint32       xmitNativeVlan;
} Net_Vlan_Stats;

typedef struct {
   uint32	interrupts;

   uint32	virtPacketsSent;
   uint64	virtBytesSent;
   uint32	physPacketsSent;
   uint64	physBytesSent;
   uint32	sendOverflowQueue; // put into queue for rexmit
   uint32	sendOverflowDrop;  // being tossed on the floor
   uint32	xmitClusterOn;
   uint32	xmitClusterOff;
   uint32	xmitClusterOffPktPending;   
   uint32	xmitClusteredUntilHalt;
   uint32	xmitClusteredUntilRecv;
   uint32	xmitCalls;
   uint32	xmitQueueLow;
   uint32	xmitStoppedIntr;
   uint32	xmitCompleteIntr;
   uint32	xmitTimeoutIntr;
   uint32	xmitIdleIntr;   
   uint32	xmitNoGoodSlave;   

   uint32	virtPacketsReceived;
   uint64	virtBytesReceived;
   uint32	physPacketsReceived;
   uint64	physBytesReceived;
   uint32	recvClusterOn;
   uint32	recvClusterOff;
   uint32	recvClusterOffPktPending;
   uint32	recvPacketsNoDelay;
   uint32	recvPacketsClustered;   
   uint32	recvPacketsClusteredOverflow;
   uint32	recvPacketsClusteredIdle;
   uint32	recvPacketsClusteredNotRunning;
   uint32	recvPacketsClusteredUntilHalt;
   uint32	receiveOverflow;
   uint32	receiveQueueEmpty;
   uint32       recvInboundLBMismatchDiscard;
   uint32       recvInboundLBMismatchKeep;
   uint32	pktCopiedLow;
   uint32       rxsumOffload;
   uint32       txsumOffload;
   uint32       tcpSegOffloadHw;
   uint32       tcpSegOffloadSw;
   uint32       linkStateChange;
   uint32       beaconStateChange;
   Net_Beacon_Stats beacon;
   Net_Vlan_Stats vlan;
} Net_IndStats;

typedef struct Net_Stats {
   Net_IndStats	local;
   Net_IndStats remote;

   uint32	noReceiver;
} Net_Stats;

struct PktList;
struct UplinkDevice;
struct PktHandle;

typedef VMK_ReturnStatus (*StartTx)(void *, struct PktList *);
typedef int (*NetOpenDev)(void *);
typedef int (*NetCloseDev)(void *);
typedef int (*NetGetPhysicalMACAddr)(void *, uint8 *);
typedef struct Net_Functions {
   StartTx startTx;
   NetOpenDev open;
   NetCloseDev close;
   NetGetPhysicalMACAddr getPhysicalMACAddr;
} Net_Functions;

void Net_PktFree(struct PktHandle *);
struct PktHandle *Net_PktAlloc(size_t, size_t);
VMK_ReturnStatus Net_UplinkDeviceConnected(const char *, void *, int32,
                                           Net_Functions *, size_t, size_t,
                                           void **);
void Net_UplinkSetupIRQ(void *, uint32, IDT_Handler, void *);
void Net_UplinkRegisterCallbacks(struct UplinkDevice *);
void *Net_GetUplinkImpl(const char *name);
void Net_ReceivePkt(void *, struct PktHandle *);
VMK_ReturnStatus Net_IOComplete(struct PktHandle *);
void NetDummyFlushBuffers(void);

#endif // _NET_DRIVER_H_

