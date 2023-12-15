/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * netDebug.h --
 *
 *    Network debugging/logging
 */

#ifndef _NETDEBUG_H_
#define _NETDEBUG_H_

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#define NETDEBUG_ENABLE_DEBUG		0x01
#define NETDEBUG_ENABLE_LOG		0x02
#define NETDEBUG_ENABLE_DUMP		0x04
#define NETDEBUG_ENABLE_USERWORLD	0x08

#define SP_RANK_NET_DEBUG		(SP_RANK_LEAF)
#define SP_RANK_NET_LOG_QUEUE		(SP_RANK_LOG)

struct Debug_Context;

extern VMK_ReturnStatus NetDebug_DebugCnxInit(struct Debug_Context* dbgCtx);

extern void NetLog_Queue(int nextLogChar, uint32 length);
extern void NetLog_Send(int nextLogChar, void *data, uint32 length);

extern Bool NetDebug_Start(void);
extern void NetDebug_Stop(void);
extern void NetDebug_Poll(void);
extern Bool NetDebug_Transmit(void* hdr, uint32 hdrLength, void* data,
			      uint32 dataLength, uint32 srcPort,
			      uint8* dstMACAddr, uint32 dstIPAddr,
			      uint32 dstPort, int protocol);
extern Bool NetDebug_ARP(uint32 ipAddr, uint8* macAddr);

void NetDebug_Init(void);
VMK_ReturnStatus NetDebug_Open(char *name, uint32 srcAddr, uint32 flags);
void NetDebug_ProcPrint(char *page, int *len);
int NetDebug_ProcRead(Proc_Entry *entry, char *page, int *len);
int NetDebug_ProcWrite(Proc_Entry *entry, char *page, int *lenp);


// only use this one to forcibly shutdown the network debugger/logger
void NetDebug_Shutdown(struct Debug_Context* dbgCtx);

#endif

