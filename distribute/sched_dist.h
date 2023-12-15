/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * sched_dist.h --
 *
 *	World scheduler.
 */

#ifndef _SCHED_DIST_H
#define _SCHED_DIST_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_DISTRIBUTE
#define INCLUDE_ALLOW_VMK_MODULE
#include "includeCheck.h"

#include "splock.h"

typedef void (*CpuSched_StartFunc)(void *data);
struct World_Handle;

EXTERN Bool CpuSched_Wakeup(uint32 event);
EXTERN NORETURN void CpuSched_Die(void);
EXTERN void CpuSched_YieldThrottled(void);

EXTERN Bool CpuSched_IsPreemptible(void);
EXTERN Bool CpuSched_DisablePreemption(void);
EXTERN Bool CpuSched_EnablePreemption(void);
EXTERN void CpuSched_RestorePreemption(Bool preemptible);

EXTERN struct World_Handle *CpuSched_GetCurrentWorld(void);

EXTERN uint32 CpuSched_MyPCPU(void);

EXTERN VMK_ReturnStatus CpuSched_DriverWaitIRQ(uint32 event,
                                               SP_SpinLockIRQ *lock,
                                               SP_IRQL callerPrevIRQL);
EXTERN VMK_ReturnStatus CpuSched_SCSIWaitIRQ(uint32 event,
                                             SP_SpinLockIRQ *lock,
                                             SP_IRQL callerPrevIRQL);

EXTERN VMK_ReturnStatus CpuSched_Sleep(uint32 msec);

#endif
