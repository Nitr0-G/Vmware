/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * rpc.h --
 *
 *	RPC functions.
 */

#ifndef _RPC_H
#define _RPC_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "rpc_types.h"
#include "proc.h"
#include "util.h"
#include "heap_public.h"

#define RPC_POLL_GET_MSG		0x01
#define RPC_POLL_GET_REPLY		0x02
#define RPC_POLL_SEND_MSG		0x04
#define RPC_POLL_POST_REPLY		0x08
#define RPC_POLL_CALL			0x10

#define RPC_NUM_USER_RPC_CALLS          (80)

typedef struct RPC_UserRPCStats {
   uint32 callCnt[RPC_NUM_USER_RPC_CALLS];
   Timer_RelCycles maxTime[RPC_NUM_USER_RPC_CALLS];
   Timer_RelCycles totTime[RPC_NUM_USER_RPC_CALLS];
   Proc_Entry procUserRPC; // /proc/vmware/vm/<ID>/userRPC
} RPC_UserRPCStats;

extern void RPC_Init(VMnix_SharedData *);
extern VMK_ReturnStatus RPC_Register(const char *name, Bool isSemaphore, Bool notifyCOS,
                                     World_ID associatedWorld,
                                     int numBuffers, uint32 bufferLength,
                                     Heap_ID heap, RPC_Connection *resultCnxID);
extern VMK_ReturnStatus RPC_Unregister(RPC_Connection cnxID);
extern VMK_ReturnStatus RPC_Connect(const char *name, RPC_Connection *cnx);
extern void RPC_Disconnect(RPC_Connection cnx);

extern VMK_ReturnStatus RPC_Send(RPC_Connection cnx, int32 function,
                                 uint32 flags, const char *argBuffer,
                                 uint32 argLength, Util_BufferType bufType,
                                 RPC_Token *token);

extern VMK_ReturnStatus RPC_GetReply(RPC_Connection cnx, RPC_Token token, uint32 flags,
                                     char *outArgBuffer, uint32 *outArgLength,
                                     Util_BufferType bufType, World_ID switchToWorldID);

extern VMK_ReturnStatus RPC_GetMsg(RPC_Connection cnx, uint32 flags,
                                   RPC_MsgInfo *msgInfo, uint32 timeout,
                                   Util_BufferType bufType,
                                   World_ID switchToWorldID);
extern VMK_ReturnStatus RPC_GetMsgInterruptible(RPC_Connection cnxID, uint32 flags,
                                                RPC_MsgInfo *msgInfo, uint32 timeout,
                                                Util_BufferType bufType,
                                                World_ID switchToWorldID);

extern VMK_ReturnStatus RPC_PostReply(RPC_Connection cnx, RPC_Token token, 
                                const char *buffer, uint32 bufferLength,
                                Util_BufferType bufType);
                                

extern VMK_ReturnStatus RPC_Call(RPC_Connection cnx, int32 function, 
                                 World_ID switchToWorldID,
                                 char *inArgBuffer, uint32 inArgLength,
                                 char *outArgBuffer, uint32 *outArgLength);

extern int RPC_CheckPendingMsgs(RPC_CnxList *cnxList);

extern void RPC_InterruptHost(void);

extern VMK_ReturnStatus RPC_Poll(RPC_Connection cnx, uint32 inEvents,
                                 uint32 *outEvents, Bool notify);
extern VMK_ReturnStatus RPC_PollCleanup(RPC_Connection cnx);

struct World_Handle;
struct World_InitArgs;
VMK_ReturnStatus RPC_WorldInit(struct World_Handle *world, 
                               struct World_InitArgs *args);
extern void RPC_WorldCleanup(struct World_Handle *world);
extern void RPC_StatsDisable(void);
extern void RPC_StatsEnable(void);

#endif

