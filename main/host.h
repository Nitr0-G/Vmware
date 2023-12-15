/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * host.h --
 *
 *	Host specific functions.
 */

#ifndef _HOST_H
#define _HOST_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"
#include "host_dist.h"
#include "chipset.h"
#include "world.h"

extern void Host_EarlyInit(VMnix_Info *vmnixInfo, VMnix_SharedData *sharedData,
                           VMnix_StartupArgs *startupArgs);
extern void Host_LateInit(void);
extern void Host_InitInterrupts(VMnix_Info *vmnixInfo);
extern void Host_TimerInit(uint64 tscStart, uint64 tscOffset);
extern void Host_SetPendingIRQ(IRQ irq);
extern void Host_SetupIRQ(IRQ irq, uint32 vector, Bool isa, Bool edge);

/*
 * lock rank for hostICPendingLock used in Host_InterruptVMnix
 */
#define SP_RANK_HOSTIC_LOCK (SP_RANK_IRQ_BLOCK)

extern void Host_InterruptVMnix(VMnix_InterruptCause cause);

extern VMK_ReturnStatus Host_Unload(int force);
extern void Host_Broken(void);
extern void Host_RestoreIDT(void);
extern VPN Host_StackMA2VPN(MA maddr); 
extern MPN Host_StackVA2MPN(VA vaddr); 
extern void Host_VMnixVMKDev(VMnix_VMKDevType type, const char *vmkName, 
                             const char *drvName, const char *majorName, 
                             uint64 data, Bool reg);
extern Helper_RequestHandle Host_GetActiveIoctlHandle(void);

extern void Host_SetIDT(MPN idtMPN, Bool newPTable);

extern void Host_SetGDTEntry(int index, LA base, VA limit, uint32 type, 
                             uint32 S, uint32 DPL, uint32 present, uint32 DB, 
                             uint32 gran);

extern void Host_DumpIntrInfo(void);

extern void HostEntryTaskReturn(void);

extern Task *Host_GetVMKTask(void);
extern MA Host_GetVMKPageRoot(void);

extern unsigned statCounters[];

extern uint32 hostCR0;
extern uint32 hostCR4;

extern uint32 hostOpId;

#define STAT_INC(_indx)      statCounters[(_indx)]++ 

extern void CopyFromHostInt(void *dst, const void *src, uint32 length);
extern void CopyToHostInt(void *dst, const void *src, uint32 length);
extern int Host_GetCharDebug(void *addr);

/*
 *----------------------------------------------------------------------
 *
 * CopyFromHost --
 *
 *      A wrapper for CopyFromHostInt with some asserts.  Copies data from
 *      COS virtual address to vmkernel virtual address.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void CopyFromHost(void *dst, const void *src, uint32 length)
{
   DEBUG_ONLY(World_Handle *world = PRDA_GetRunningWorldSafe());
   ASSERT((world == NULL) || World_IsHOSTWorld(world));
   ASSERT((VA)src >= VMNIX_KVA_START && ((VA)(src + length) < VMNIX_KVA_END));
   ASSERT((VA)dst >= VMK_FIRST_ADDR && ((VA)(dst + length) < VMK_VA_END));
   CopyFromHostInt(dst, src, length);
}

/*
 *----------------------------------------------------------------------
 *
 * CopyToHost --
 *
 *      A wrapper for CopyToHostInt with some asserts.  Copies data from
 *      vmkernel virtual address to COS virtual address.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void CopyToHost(void *dst, const void *src, uint32 length)
{
   DEBUG_ONLY(World_Handle *world = PRDA_GetRunningWorldSafe());
   ASSERT((world == NULL) || World_IsHOSTWorld(world));
   ASSERT((VA)dst >= VMNIX_KVA_START && ((VA)(dst + length) < VMNIX_KVA_END));
   ASSERT((VA)src >= VMK_FIRST_ADDR && ((VA)(src + length) < VMK_VA_END));
   CopyToHostInt(dst, src, length);
}

static INLINE World_ID
Host_GetWorldID(void)
{
   return hostWorld->worldID;
}

#endif

