/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * async_io.h
 *
 *	Asynchronous IO structures.
 */

#ifndef _ASYNC_IO_H
#define _ASYNC_IO_H

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_DISTRIBUTE
#define INCLUDE_ALLOW_VMK_MODULE
#include "includeCheck.h"

#include "splock.h"
#include "memalloc_dist.h"

/* 
 * What to do when cmd associated with token is done
 * ASYNC_ENQUEUE and ASYNC_CALLBACK are mutually exclusive.
 */

#define ASYNC_CALLBACK		0x000001 // Execute fn when cmd is done
#define ASYNC_POST_ACTION	0x000002 // Post action to vmm when cmd is done
#define ASYNC_ENQUEUE		0x000004 // Enqueue result on handle list
#define ASYNC_HOST_INTERRUPT	0x000008 // Interrupt host when cmd is done
#define ASYNC_DUMPING		0x000010
#define ASYNC_CANT_BLOCK	0x000020 // Code issuing request on this token cannot block

#define ASYNC_IO_DONE		0x010000
#define ASYNC_WAITER		0x020000
#define ASYNC_IO_TIMEDOUT	0x040000 // Set when I/O request timesout

#define ASYNC_MAX_RESULT	64
#define ASYNC_MAX_PRIVATE	96

struct Async_Token;

typedef void (*Async_Callback)(struct Async_Token *token);
typedef void (*Async_FrameCallback)(struct Async_Token *token, void *data);

struct SCSI_Command;

typedef struct Async_Token {
   SP_SpinLock		lock;
   int32		refCount;
   struct Async_Token	*nextForCallee;
   VA			freePC;
   uint32 		flags;
   Async_Callback	callback;
   uint32		callbackFrameOffset;
   uint32		originSN;    //serial number of originating command (used by fs)
   uint32		originSN1;   //serial number1 used only by monitor side of scsi code
   int32		originHandleID;
   struct SCSI_Command  *cmd;        //scsi cmd.
   void			*clientData; //for private use of the entity allocating the token
   uint8		result[ASYNC_MAX_RESULT];
   uint8		callerPrivate[ASYNC_MAX_PRIVATE];
   uint32		callerPrivateUsed;
   int                  resID; //resource ID, currently just world ID
   void                 *sgList;
   uint64               startTSC; // TSC when the token is allocated
   uint64               issueTSC; // TSC when the command is sent to driver
   uint64               startVmTime; // CpuSched_VcpuUsage when token alloated 
#ifdef VMX86_DEBUG
   #define ASYNC_DBG_SLOTS 64
   uint32               dbgCurr;
   struct {
      int32 refCount;
      int32 pcpu;
      VA freePC;
   } dbgList[ASYNC_DBG_SLOTS];
#endif
} Async_Token;

extern Async_Token *Async_AllocToken(uint32 flags);
extern void Async_RefToken(Async_Token *token);
extern void Async_ReleaseToken(Async_Token *token);

extern void Async_PrepareToWait(Async_Token *token);
extern void Async_Wait(Async_Token *token);
extern void Async_Wakeup(Async_Token *token);

extern void Async_WaitForIO(Async_Token *token);
extern void Async_IODone(Async_Token *token);
extern void Async_IOTimedOut(Async_Token *token);

extern void *Async_PushCallbackFrame(Async_Token *token,
				     Async_FrameCallback callback,
				     uint8 payload);
extern void Async_PopCallbackFrame(Async_Token *token);
extern void Async_FreeCallbackFrame(Async_Token *token);

static INLINE void 
Async_TokenCallback(Async_Token *token)
{
   Async_Wakeup(token);

   if (token->flags & ASYNC_CALLBACK) {
      ASSERT(token->callback != NULL);
      token->callback(token);
   }
}

#endif

