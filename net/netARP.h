/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * netARP.h --
 *
 *    ARP Interface
 */

#ifndef _NETARP_H_
#define _NETARP_H_

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "net.h"

#define ARP_CACHE_LENGTH	16

typedef struct NetARP_Entry {
   uint32	ipAddr;
   uint8	macAddr[ETHER_ADDR_LENGTH];
} NetARP_Entry;

typedef struct NetARP_State {
   NetARP_Entry	cache[ARP_CACHE_LENGTH];
} NetARP_State;

extern VMK_ReturnStatus NetARP_SendARP(Net_PortID portID, uint32 srcIPAddr, 
                                       uint8 *srcMACAddr, uint32 dstIPAddr);
extern VMK_ReturnStatus NetARP_SendRARP(Net_PortID portID, uint8 *srcMACAddr);
extern VMK_ReturnStatus NetARP_ProcessARP(Net_PortID portID,
					  NetARP_State* arpState, uint32 ipAddr,
					  uint8* macAddr, EtherHdr* eh);
extern VMK_ReturnStatus NetARP_GetMACFromIP(Net_PortID portID,
					    NetARP_State* arpState,
					    uint32 ipAddr, uint8* macAddr,
					    uint32 srcIPAddr, uint8* srcMACAddr);

#endif
