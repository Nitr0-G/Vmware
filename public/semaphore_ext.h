/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * semaphore.h
 *
 *	Semaphore support.
 */

#ifndef _SEMAPHORE_EXT_H
#define _SEMAPHORE_EXT_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMK_MODULE
#define INCLUDE_ALLOW_DISTRIBUTE
#include "includeCheck.h"

#include "return_status.h"
#include "splock.h"
#include "list.h"

#define SEMA_RANK_UNRANKED 0x10000
#define SEMA_RANK_MAX      0xffff

#define SEMA_RANK_LEAF     SEMA_RANK_MAX
#define SEMA_RANK_STORAGE  0x8000
#define SEMA_RANK_FS       0x7000

#define SEMA_RANK_MIN      0

typedef uint32 SemaRank;

typedef struct Semaphore {
   List_Links nextHeldSema; // must be first item
   int32 count;
   int32 waiters;
   SP_SpinLock lock;
   SemaRank rank;
} Semaphore;

typedef struct RWSemaphore {
   int32 exclusiveWaiters;
   int32 sharedWaiters;
   int32 exclusiveAccess;
   int32 sharedAccess;
   Bool upgradeWaiter;
   SP_SpinLock lock;
} RWSemaphore;

extern void Semaphore_Init(char *name, Semaphore *sema, int32 value, SemaRank rank);
extern void Semaphore_Cleanup(Semaphore *sema);
extern void Semaphore_Lock(Semaphore *sema);
extern void Semaphore_Unlock(Semaphore *sema);
extern Bool Semaphore_IsLocked(Semaphore *sema);

extern void Semaphore_RWInit(char *name, RWSemaphore *sema);
extern void Semaphore_RWCleanup(RWSemaphore *sema);
extern void Semaphore_BeginRead(RWSemaphore *sema);
extern void Semaphore_EndRead(RWSemaphore *sema);
extern void Semaphore_BeginWrite(RWSemaphore *sema);
extern void Semaphore_EndWrite(RWSemaphore *sema);
extern Bool Semaphore_IsShared(RWSemaphore *sema);
extern Bool Semaphore_IsExclusive(RWSemaphore *sema);

extern VMK_ReturnStatus Semaphore_UpgradeFromShared(RWSemaphore *sema);
extern void Semaphore_DowngradeToShared(RWSemaphore *sema);

#endif

