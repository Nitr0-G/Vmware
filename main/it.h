/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * it.h --
 *
 *	Interrupt balancing functions.
 */

#ifndef _IT_H_
#define _IT_H_

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "idt.h"
#include "prda.h"
#include "cpusched.h"
#include "world.h"
#include "it_dist.h"

// turn on IT debugging in OBJ and BETA builds by default
#define IT_DEBUG (vmx86_debug)

// interrupt rates describe the expected frequency/expense
// of interrupts on a given processor
typedef enum {
   INTR_RATE_NONE = 0,
   INTR_RATE_LOW,
   INTR_RATE_MEDIUM,
   INTR_RATE_HIGH,
   INTR_RATE_EXCESSIVE,
   INTR_RATE_MAX
} IT_IntrRate;

// specified via CONFIG_IRQ_ROUTING_POLICY
typedef enum {
   IT_NO_ROUTING = 0,
   IT_IDLE_ROUTING = 1,
   IT_RANDOM_ROUTING = 2
} IT_RoutingPolicy;

typedef struct IT_VectorInfo {
   struct IT_VectorInfo *next;
   uint32 vector;
   PCPU pcpuNum;
   int32  refCount;
   uint32 remoteInterrupts;
   
   Timer_RelCycles sysCycles[MAX_PCPUS];
   CpuSched_AtomicVersions sysCyclesVersions[MAX_PCPUS];

   Bool inList;
   Bool skip;
   Bool isFake;

   // data protected by itLock
   Timer_RelCycles agedSysCycles; 
   Timer_AbsCycles prevSysCycles; 
   uint64 agedInterrupts;
   uint64 prevInterrupts;
   
   Timer_Handle followTimer;
   Timer_Handle rebalTimer;
   
#if (IT_DEBUG)
   uint64 remoteForwards;
   uint64 idleCount;
#endif
} IT_VectorInfo;

EXTERN IT_VectorInfo *itInfo[IDT_NUM_VECTORS];
EXTERN IT_IntrRate pcpuIntrRates[MAX_PCPUS];

EXTERN void IT_Init(void);
EXTERN void IT_Enable(void);
EXTERN void IT_NotifyHostSharing(uint32 vector, Bool shared);
EXTERN IT_IntrRate IT_GetPcpuIntrRate(PCPU p);

/*
 *-----------------------------------------------------------------------------
 *
 * IT_ShouldTrackInterrupts --
 *
 *     Returns TRUE iff interrupt handlers should call IT_Count to keep
 *     track of interrupts.
 *
 * Results:
 *     Returns TRUE iff interrupt handlers should call IT_Count to keep
 *     track of interrupts.
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE Bool
IT_ShouldTrackInterrupts(void)
{
   return (CONFIG_OPTION(IRQ_ROUTING_POLICY) != IT_NO_ROUTING);
}


/*
 *-----------------------------------------------------------------------------
 *
 * IT_AccountSystime --
 *
 *     Updates the counter to reflect that the current PCPU spent 
 *     "cycles" processor cycles processing an interrupt
 *     (bottom half or handler) corresponding to "vector"
 *
 * Results:
 *     None.
 *
 * Side effects:
 *     Updates internal counters.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE void
IT_AccountSystime(uint32 vector, Timer_RelCycles cycles)
{
   if (IT_ShouldTrackInterrupts() && vector != 0) {
      IT_VectorInfo *info;
      ASSERT(vector >= IDT_FIRST_EXTERNAL_VECTOR
             && vector < IDT_NUM_VECTORS);
      // survive this condition in release builds;
      // we'll just miss one sample for the interrupt tracker
      if (UNLIKELY(vector < IDT_FIRST_EXTERNAL_VECTOR
                   || vector >= IDT_NUM_VECTORS)) {
         return;
      }
      info = itInfo[vector];
      if (info) {
         PCPU p = MY_PCPU;
         CPUSCHED_VERSIONED_ATOMIC_UPDATE_BEGIN(&info->sysCyclesVersions[p]);
         info->sysCycles[p] += cycles;
         CPUSCHED_VERSIONED_ATOMIC_UPDATE_END(&info->sysCyclesVersions[p]);
      }
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * IT_Count --
 *
 *     Updates the counter to reflect that the interrupt "vector" has been
 *     forwarded to a world on "pcpuNum"
 *     Only used for debugging and stats.
 *
 * Results:
 *     None.
 *
 * Side effects:
 *     Updates internal counters.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE void
IT_Count(uint32 vector, PCPU pcpuNum)
{
#if (IT_DEBUG)
   static int numMisses = 0;
   IT_VectorInfo *info = itInfo[vector];
   if (info == NULL) {
      if (numMisses++ < 10) {
         // throttle this logging after 10 failures, so we don't fill up the log
         extern void _Log(const char *fmt, ...);
         _Log("IT: counting interrupt for vector 0x%x, which has not been registered with tracker",
              vector);
      }
      return;
   }
   if (info->pcpuNum != pcpuNum) {
      info->remoteForwards++;
   }
   if (prdas[MY_PCPU]->idle) {
      info->idleCount++;
   }
#endif
}

/*
 *-----------------------------------------------------------------------------
 *
 * IT_GetCurPCPU --
 *
 *     Returns the pcpu to which "vector" is currently routed
 *
 * Results:
 *     Returns the pcpu to which "vector" is currently routed
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE PCPU
IT_GetCurPCPU(uint32 vector)
{
   if(!itInfo[vector]) {
      // this call is sometimes made before IT_RegisterVector()
      return 0;
   }
   return itInfo[vector]->pcpuNum;
}


#endif // ifdef _IT_H_
