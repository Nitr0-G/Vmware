/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * bh.c --
 *
 *	This module manages bottom-half handlers.
 */


#include "vm_types.h"
#include "vm_libc.h"
#include "x86.h"
#include "vmkernel.h"
#include "vm_asm.h"
#include "host.h"
#include "sched.h"
#include "prda.h"
#include "bh.h"
#include "world.h"
#include "action.h"
#include "vmkstats.h"
#include "timer.h"
#include "eventhisto.h"

#define LOGLEVEL_MODULE BH
#include "log.h"

#define MAX_BH_HANDLERS		32
typedef struct BHInfo {
   BH_Handler 	handler;
   void		*clientData;
} BHInfo;

uint32 currentBH = 0;
static BHInfo bhInfo[MAX_BH_HANDLERS];

static SP_SpinLock bhLock;

/*
 * bhPending flags of the global bh should be read cached on all CPUs.
 * Adding the padding on both sides to make sure it gets its own cacheline
 * so that it doesn't get evicted due to other data.
 */
struct {
   char pad0[64];
   volatile Atomic_uint32 bhPending;
   char pad1[64];
} bhGlobal;

/*
 * BHPendingXXX provide wrappers for Atomic_XXX operations on bhPending
 * bitmask.  Didn't want to litter the references with casts to remove the
 * inherent volatile in myPRDA reference.
 */

static INLINE void
BHPendingOr(volatile Atomic_uint32 *pending, uint32 val)
{
   Atomic_Or((Atomic_uint32*)pending, val);
}

static INLINE uint32
BHPendingRead(volatile Atomic_uint32 *pending)
{
   return Atomic_Read((Atomic_uint32*)pending);
}

static INLINE uint32
BHPendingReadWrite(volatile Atomic_uint32 *pending, uint32 val)
{
   return Atomic_ReadWrite((Atomic_uint32*)pending, val);
}

void
BH_Init(void)
{
   SP_InitLock("bhLock", &bhLock, SP_RANK_LEAF); 
   Atomic_Write((Atomic_uint32*)&bhGlobal.bhPending, 0);
}

/*
 *----------------------------------------------------------------------
 *
 * BH_Register
 *
 * 	Register a BH handler and return the bit index identifying it
 *
 * Results:
 *	Bit index representing the handler
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
uint32 
BH_Register(BH_Handler handler, void *clientData)
{
   uint32 retVal;

   ASSERT(currentBH < MAX_BH_HANDLERS);

   DEBUG_ONLY(EventHisto_Register((uint32)handler));
   SP_Lock(&bhLock);

   bhInfo[currentBH].handler = handler;
   bhInfo[currentBH].clientData = clientData;

   retVal = currentBH;

   currentBH++;

   SP_Unlock(&bhLock);

   return retVal;
}

/*
 *----------------------------------------------------------------------
 *
 * BHAssertValidIndex
 *
 *      Assert that the given bh index is valid
 *
 * Results: 
 *	None
 *
 * Side effects:
 *	none.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
BHAssertValidIndex(uint32 bhNum)
{
   ASSERT(bhNum < MAX_BH_HANDLERS);
   ASSERT(bhInfo[bhNum].handler != NULL);
   ASSERT(bhNum < currentBH);
}

/*
 *----------------------------------------------------------------------
 *
 * BH_SetOnPCPU
 *
 *      Schedule a bottom half on the given PCPU
 *
 * Results: 
 *	None
 *
 * Side effects:
 *	none.
 *
 *----------------------------------------------------------------------
 */
void
BH_SetOnPCPU(PCPU pcpu, uint32 bhNum)
{
   BHAssertValidIndex(bhNum);
   ASSERT(pcpu < numPCPUs);

   BHPendingOr(&prdas[pcpu]->bhPending, 1 << bhNum);
}

/*
 *----------------------------------------------------------------------
 *
 * BH_SetLocalPCPU
 *
 *      Schedule a bottom half on the local PCPU
 *
 * Results: 
 *	None
 *
 * Side effects:
 *	none.
 *
 *----------------------------------------------------------------------
 */
void
BH_SetLocalPCPU(uint32 bhNum)
{
   /*
    * NO logging, warning, etc. allowed in this call because 
    * netlogger calls it to avoid calling anything else.
    */

   BH_SetOnPCPU(MY_PCPU, bhNum);
}

/*
 *----------------------------------------------------------------------
 *
 * BH_SetOnWorld
 *
 *      Schedule a bottom half on the given world
 *
 * Results: 
 *	None
 *
 * Side effects:
 *	none.
 *
 *----------------------------------------------------------------------
 */
void
BH_SetOnWorld(World_Handle *world, uint32 bhNum)
{
   BHAssertValidIndex(bhNum);
   ASSERT(world != NULL);

   BHPendingOr(&world->bhPending, 1 << bhNum);
}

/*
 *----------------------------------------------------------------------
 *
 * BH_SetGlobal
 *
 * 	Set the BH pending bit for the global BH
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
  *
 *----------------------------------------------------------------------
 */
void
BH_SetGlobal(uint32 bhNum)
{
   BHAssertValidIndex(bhNum);
   BHPendingOr(&bhGlobal.bhPending, 1 << bhNum);
}


/*
 *----------------------------------------------------------------------
 *
 * BHCallHandlers
 *
 * 	Helper function that calls the handlers that are set in the given
 * 	BH pending flags
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
BHCallHandlers(volatile Atomic_uint32 *pendingPtr)
{
   uint32 pending;

   // first check with non-locking instruction to handle common case
   if (BHPendingRead(pendingPtr) == 0) {
      // nothing to do
      return;
   }

   while ((pending = BHPendingReadWrite(pendingPtr, 0)) != 0) {
      uint32 i;

      LOG(1, "pending=0x%x", pending);
      for (i = 0; i < currentBH && pending; i++) {
         if (pending & (1 << i)) {
            DEBUG_ONLY(uint64 startTsc);
            pending &= ~(1 << i);

            LOG(2, "calling %d:%p", i, bhInfo[i].handler);
            ASSERT(bhInfo[i].handler != NULL);
            DEBUG_ONLY(startTsc = EventHisto_StartSample());
            bhInfo[i].handler(bhInfo[i].clientData);
            DEBUG_ONLY(EventHisto_EndSample((uint32)bhInfo[i].handler,
                                            startTsc));
         }
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * BH_Check --
 *
 * 	Execute any pending bottom-half handlers on the local pcpu.
 *      After running bottom-half handlers, invokes scheduler if a
 *	reschedule is pending and "canReschedule" is TRUE.
 *	The running world must be non-preemptible.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Transiently enables interrupts
 *
 *----------------------------------------------------------------------
 */
void
BH_Check(Bool canReschedule)
{
   volatile PRDA *p = &myPRDA;
   uint32 eflags;
   World_Handle *w = MY_RUNNING_WORLD;
   if (World_IsVMMWorld(w) && 
       VMK_STRESS_DEBUG_COUNTER(WORLD_PANIC)) {
      World_Panic(w, "PanicStress (%x)\n", w->worldID);
   }

   /*
    * must not be preemptible otherwise running world could migrate and
    * access wrong PRDA.
    */
   ASSERT(!CpuSched_IsPreemptible());
   SAVE_FLAGS(eflags);

   DEBUG_ONLY({
      p->bhCheck++;
      p->bhCheckResched += canReschedule;
   });
   if (!p->bhInProgress) {
      p->bhInProgress = TRUE;

      ENABLE_INTERRUPTS();
      BHCallHandlers(&bhGlobal.bhPending);
      BHCallHandlers(&p->bhPending);
      BHCallHandlers(&MY_RUNNING_WORLD->bhPending);

#if SOFTTIMERS
      Timer_BHHandler(NULL);
#endif
      RESTORE_FLAGS(eflags);
      p->bhInProgress = FALSE;      
   } 

   // check reschedule flag, if allowed
   if (canReschedule && p->reschedule) {
      // prevent rescheduling while busy-waiting, 
      //   since the busy-wait loop will notice the reschedule flag itself,
      //   and this avoids complexity from migrating at inconvenient points
      // prevent rescheduling while marked halted,
      //   which is only possible if interrupted during brief window
      //   in CpuSchedIdleHaltStart() between setting flag and HLT;
      //   the idle loop will notice the reschedule flag itself,
      //   and this reduces the complexity of halt time accounting
      if ((World_CpuSchedRunState(w) != CPUSCHED_BUSY_WAIT) &&
          !p->halted) {
         CpuSched_Reschedule();
      }
      /*
       * Note that if we get rescheduled, we don't need to rerun
       * BHCallHandlers because cpusched.c calls BH_Check after reschedule.
       */
   }
}

/*
 *----------------------------------------------------------------------
 *
 * BH_*LinuxBHList --
 *
 *      Accessor fuction for use in linux drivers / vmklinux where the
 *      myPRDA struct isn't exported.
 *
 * Results: 
 *	
 *
 * Side effects:
 *	none.
 *
 *----------------------------------------------------------------------
 */
void 
BH_SetLinuxBHList(void *data)
{
   myPRDA.linuxBHList = data;
}

void *
BH_GetLinuxBHList(void)
{
   return myPRDA.linuxBHList;
}

