/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * it.c --
 *
 *	This module provides interrupt balancing functions.
 *
 * XXX It assumes it is the only entity steering interrupts away from HOST_PCPU
 *
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "x86.h"
#include "vmkernel.h"
#include "splock.h"
#include "it.h"
#include "memalloc.h"
#include "timer.h"
#include "host.h"
#include "config.h"
#include "parse.h"
#include "sched_sysacct.h"
#include "util.h"

#define LOGLEVEL_MODULE IT
#include "log.h"

IT_VectorInfo *itInfo[IDT_NUM_VECTORS];

// static data 
typedef struct {
   SP_SpinLockIRQ itLock;
      
   // starting point, in cycles, of each rate level
   Timer_RelCycles intrThresh[INTR_RATE_MAX];
   
   Timer_RelCycles pcpuPrevIdle[MAX_PCPUS];
   Timer_RelCycles pcpuAgedIdle[MAX_PCPUS];

   Timer_RelCycles vecCacheAffin;
   Timer_RelCycles rebalancePeriodCycles;
   Timer_RelCycles pcpuMaxIntrLoad;
   uint32 lastRand;

   Proc_Entry itProcEnt;
   IT_IntrRate pcpuIntrRates[MAX_PCPUS];

   // approximate cost of vmkcall roundtrip
   Timer_RelCycles intrCycleWeight;
   
   uint32 intrOverflows;
} IT;

static IT it;

typedef struct {
   Timer_RelCycles pcpuIntrTaken[MAX_PCPUS];
   Timer_RelCycles newIdle[MAX_PCPUS];
   Timer_RelCycles newUsed[MAX_PCPUS];
   Timer_RelCycles newOverlap[MAX_PCPUS];
} ITDataBuffer;


#define IT_PCPU_INTR_TAKEN_MAX (4)
#define IT_LOW_PCT       (4)
#define IT_MEDIUM_PCT    (12)
#define IT_HIGH_PCT      (30)
#define IT_EXCESSIVE_PCT (65)

// TODO: research this number in more detail
// approximate cost in cycles of vmkcall roundtrip
#define IT_INTR_CYCLE_WEIGHT (10000)

// fake interrupts in OBJ and OPT builds only
#define IT_ALLOW_FAKE_INTERRUPTS (vmx86_devel)

// iterator macros
#define IT_FORALL_VECTORS_BEGIN(_info) do { \
   uint32 _vec; \
   ASSERT(SP_IsLockedIRQ(&it.itLock)); \
   for (_vec = 0; _vec < IDT_NUM_VECTORS; _vec++) { \
      IT_VectorInfo *_info = itInfo[_vec]; \
      if (!_info) { \
         continue; \
      }

#define IT_FORALL_VECTORS_END } } while (0)

static void ITRebalanceTimer(void *data, Timer_AbsCycles timestamp);
static void ITSetupThresholds(uint32 lowPct,
                              uint32 medPct,
                              uint32 highPct,
                              uint32 excessivePct);
#if IT_ALLOW_FAKE_INTERRUPTS
static VMK_ReturnStatus ITRemoveFakeInterrupt(uint32 vector);
static VMK_ReturnStatus ITAddFakeInterrupt(uint32 vector, uint32 microRun, uint32 microWait);
#endif


/*
 *-----------------------------------------------------------------------------
 *
 * ITComputeIntrRate --
 *
 *      Returns the interrupt rate corresponding to "agedTotalCycles"
 *
 * Results:
 *      Returns the interrupt rate corresponding to "agedTotalCycles"
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE IT_IntrRate
ITComputeIntrRate(Timer_RelCycles agedTotalCycles)
{
   int rate;
   uint32 timePct;

   if (cpuKhzEstimate == 0
       || it.rebalancePeriodCycles == 0) {
      return 0;
   }

   timePct = (100 * agedTotalCycles) / it.rebalancePeriodCycles;
   LOG(3, "timePct=%u, agedTotal=%Lu, rebalCycl=%Lu",
       timePct, agedTotalCycles, it.rebalancePeriodCycles);
   Log_Event("rate-pct", timePct, EVENTLOG_OTHER);

   for (rate=0; rate < INTR_RATE_MAX; rate++) {
      if (agedTotalCycles < it.intrThresh[rate]) {
         break;
      }
   }

   ASSERT(rate > 0);
   return (IT_IntrRate) (rate - 1);
}

/*
 *-----------------------------------------------------------------------------
 *
 * ITComputePcpuIntrRate --
 *
 *      Computes and returns the interrupt rate of processor "p"
 *      Does not use the cached interrupt rate data
 *
 * Results:
 *      Returns the interrupt rate of processor "p"
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */
static IT_IntrRate
ITComputePcpuIntrRate(PCPU p)
{
   Timer_RelCycles agedSysCycles = 0;
   uint64 agedInterrupts = 0;

   IT_IntrRate rate;

   IT_FORALL_VECTORS_BEGIN(info) {
      if (info->pcpuNum == p) {
         agedSysCycles += info->agedSysCycles;
         agedInterrupts += info->agedInterrupts;
       }
   } IT_FORALL_VECTORS_END;

   rate = ITComputeIntrRate(agedSysCycles + (agedInterrupts * it.intrCycleWeight));

   return (rate);
}


/*
 *-----------------------------------------------------------------------------
 *
 * IT_GetPcpuIntrRate --
 *
 *      Returns the cached interrupt rate of the given processor "p"
 *
 * Results:
 *      Returns the cached interrupt rate of the given processor "p"
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
IT_IntrRate
IT_GetPcpuIntrRate(PCPU p)
{
   return it.pcpuIntrRates[p];
}


// locked internal helper for IT_RegisterVector below
static void 
ITRegisterVectorInt(uint32 vector, Bool fake)
{
   IT_VectorInfo *info;

   ASSERT(SP_IsLockedIRQ(&it.itLock));
   ASSERT(IDT_VectorIsDevInterrupt(vector));
   
   info = itInfo[vector];   
   if (itInfo[vector] == NULL) {
      info = (IT_VectorInfo *)Mem_Alloc(sizeof(IT_VectorInfo));
      ASSERT_NOT_IMPLEMENTED(info != NULL);
      memset(info, 0, sizeof(IT_VectorInfo));
      // all vectors sent to HOST_PCPU to start
      // TODO: add a function to query chipset for
      // this info, just in case
      info->pcpuNum = HOST_PCPU;
      info->vector = vector;
      info->skip = FALSE;
      itInfo[vector] = info;
   }
   
   info->refCount++;

   LOG(0, "vector 0x%x refCount=%d", vector, info->refCount);

   info->isFake = fake;
}

/*
 *----------------------------------------------------------------------
 *
 * IT_RegisterVector --
 *
 *      Register a device on this vector that will need to use us
 *	for balancing.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Update the vector table and list.
 *
 *----------------------------------------------------------------------
 */
void 
IT_RegisterVector(uint32 vector)
{
   SP_IRQL prevIRQL = SP_LockIRQ(&it.itLock, SP_IRQL_KERNEL);
   ITRegisterVectorInt(vector, FALSE);
   SP_UnlockIRQ(&it.itLock, prevIRQL);
}


/*
 *----------------------------------------------------------------------
 *
 * IT_UnregisterVector --
 *
 *      Unregister this vector as needing rebalancing.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The corresponding vector entry is modified.
 *
 *----------------------------------------------------------------------
 */
void
IT_UnregisterVector(uint32 vector)
{
   Bool ok = TRUE;
   IT_VectorInfo *info;
   SP_IRQL prevIRQL = SP_LockIRQ(&it.itLock, SP_IRQL_KERNEL);
   info = itInfo[vector];
   
   ASSERT(SP_IsLockedIRQ(&it.itLock));
   ASSERT(IDT_VectorIsDevInterrupt(vector));
   ASSERT(info != NULL);
   ASSERT(info->refCount > 0);

   // if this is really a "double-unregister" of a vector, this shouldn't
   // lead to fatal errors down the line, so don't panic in a release build
   if (info == NULL || info->refCount <= 0) {
      Warning("unregistering unknown vector: 0x%x", vector);
      SP_UnlockIRQ(&it.itLock, prevIRQL);
      return;
   }
   
   info->refCount--;

   LOG(2, "refCount=%d", info->refCount);

   if (info->refCount == 0) {
      info->skip = TRUE;
      info->pcpuNum = HOST_PCPU;
      if (!info->isFake) {
         ok = IDT_VectorSetDestination(vector, HOST_PCPU);
      }
      ASSERT(ok);
      itInfo[vector] = NULL;
      Mem_Free(info);
   }
   SP_UnlockIRQ(&it.itLock, prevIRQL);
}


/*
 *-----------------------------------------------------------------------------
 *
 * ITGetVectorSystime --
 *
 *      Returns the total systime associated with "vector" in timer cycles
 *
 * Results:
 *      Returns the total systime associated with "vector" in timer cycles
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE Timer_RelCycles
ITGetVectorSystime(uint32 vector)
{
   PCPU p;
   Timer_RelCycles totalTime = 0;
   IT_VectorInfo *info = itInfo[vector];

   for (p=0; p < numPCPUs; p++) {
      Timer_RelCycles thisPcpuTime;
      CPUSCHED_VERSIONED_ATOMIC_READ_BEGIN(&info->sysCyclesVersions[p]);
      thisPcpuTime = info->sysCycles[p];
      CPUSCHED_VERSIONED_ATOMIC_READ_END(&info->sysCyclesVersions[p]);
      
      totalTime += thisPcpuTime;
   }

   return (totalTime);
}

/*
 *-----------------------------------------------------------------------------
 *
 * ITComputePcpuIdleTimes --
 *
 *      Based on recent pcpu usage data from the scheduler (newIdle, newUsed,
 *      newSysOver), updates global per-pcpu idle time stats.
 *      The arrays passed in should have room for at least numPCPUs entries.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Updates global aged idle time stats
 *
 *-----------------------------------------------------------------------------
 */
static void
ITComputePcpuIdleTimes(Timer_RelCycles *newIdle,
                       Timer_RelCycles *newUsed,
                       Timer_RelCycles *newSysOver)
{
   PCPU p;
   ASSERT(SP_IsLockedIRQ(&it.itLock));
   
   for (p=0; p < numPCPUs; p++) {
      Timer_RelCycles idleUnused, diff;

      // note that we need to use "idle - used" as our metric,
      // because on a hyperthread system, an idle time of 0 could
      // imply that the logical processor was halted for the whole
      // interval, giving its resources to its hypertwin. In that
      // case, (used - idle) will be 0 for the halted hypertwin
      // positive for the busy hypertwin, which is the distinction we want.
      idleUnused = newIdle[p] - newUsed[p] + newSysOver[p];
      diff = idleUnused - it.pcpuPrevIdle[p];
      it.pcpuAgedIdle[p] /= 2;
      it.pcpuAgedIdle[p] += (diff / 2);
      it.pcpuPrevIdle[p] = idleUnused;
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * ITComputeVectorCycles --
 *
 *      Updates stored systime and interrupt counts associated with
 *      all registered vectors.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Updates aged systime and interrupt count data in vecInfo
 *
 *-----------------------------------------------------------------------------
 */
static void
ITComputeVectorCycles(void)
{
   ASSERT(SP_IsLockedIRQ(&it.itLock));

   // compute aged systime for each vector
   IT_FORALL_VECTORS_BEGIN(info) {
      Timer_RelCycles sysTimeNow, timeDiff;
      uint64 interruptsNow, intrDiff;

      sysTimeNow = ITGetVectorSystime(info->vector);
      timeDiff = sysTimeNow - info->prevSysCycles;

      interruptsNow = intrCounts[info->pcpuNum][info->vector];
      if (interruptsNow < info->prevInterrupts) {
         // in the incredibly rare overflow race case, just
         // don't update the averages and totals for this vector
         it.intrOverflows++;
         continue;
      }
      intrDiff = interruptsNow - info->prevInterrupts;

      if (vmx86_debug) {
         uint32 intrPct, sysPct;
         intrPct = (100 * intrDiff * it.intrCycleWeight)
            / it.rebalancePeriodCycles;
         sysPct = (100 * timeDiff)
            / it.rebalancePeriodCycles;
         Log_Event("intr-pct", intrPct, EVENTLOG_OTHER);
         Log_Event("sys-pct", sysPct, EVENTLOG_OTHER);
      }
      
      info->agedSysCycles += timeDiff;
      info->agedInterrupts += intrDiff;
      info->agedSysCycles /= 2;
      info->agedInterrupts /= 2;
      
      info->prevSysCycles = sysTimeNow;
      info->prevInterrupts = interruptsNow;
   } IT_FORALL_VECTORS_END;
}


/*
 *-----------------------------------------------------------------------------
 *
 * ITGetIntrRateName --
 *
 *      Returns a descriptive string for "rate"
 *
 * Results:
 *      Returns a descriptive string for "rate"
 *
 * Side effects:
 *      Side effects
 *
 *-----------------------------------------------------------------------------
 */
static const char*
ITGetIntrRateName(IT_IntrRate rate)
{
   switch (rate) {
   case INTR_RATE_NONE:
      return "none";
   case INTR_RATE_LOW:
      return "low";
   case INTR_RATE_MEDIUM:
      return "medium";
   case INTR_RATE_HIGH:
      return "high";
   case INTR_RATE_EXCESSIVE:
      return "excessive";
   case INTR_RATE_MAX:
      return "max";
   default:
      return "unknown";
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * ITRebalanceVector --
 *
 *      Attempts to move the vector specified by "info" to a pcpu with
 *      lots of idle time, preferring to stay put if possible.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May re-vector interrupt.
 *
 *-----------------------------------------------------------------------------
 */
static void
ITRebalanceVector(IT_VectorInfo *info, Timer_RelCycles *pcpuIntrTaken)
{
   PCPU curBest, p;
   Timer_RelCycles vectorCycles;
   int64 bestCycles;
   IT_IntrRate rate;

   vectorCycles = info->agedSysCycles +
      (info->agedInterrupts * it.intrCycleWeight);
   rate = ITComputeIntrRate(vectorCycles);
   LOG(2, "vector 0x%x rate = %s (%u), agedSys=%Lu, agedIntrTime=%Lu",
       info->vector,
       ITGetIntrRateName(rate),
       rate,
       info->agedSysCycles,
       info->agedInterrupts * it.intrCycleWeight);

   if (pcpuIntrTaken[info->pcpuNum] < it.pcpuMaxIntrLoad) {
      // add a cache affinity bonus to the current location,
      // unless it's already overloaded
      curBest = info->pcpuNum;
      bestCycles = it.pcpuAgedIdle[curBest] + it.vecCacheAffin;
   } else {
      curBest = INVALID_PCPU;
      bestCycles = -it.rebalancePeriodCycles;
   }

   for (p=0; p < numPCPUs; p++) {
      // find the most-idle pcpu, but don't overload a pcpu
      // with too many interrupts
      if (curBest == INVALID_PCPU ||
          (it.pcpuAgedIdle[p] > bestCycles
           && pcpuIntrTaken[p] < it.pcpuMaxIntrLoad)) {
         curBest = p;
         bestCycles = it.pcpuAgedIdle[p];
      }
      LOG(3, "pcpu %u agedidlecycles=%Lu", p, it.pcpuAgedIdle[p]);
   }

   // if we have INVALID_PCPU, it means that all pcpus are overloaded,
   // so we just give up and leave things where they are
   if (curBest != INVALID_PCPU) {
      // actually move the interrupt to its new location
      if (curBest != info->pcpuNum) {
         LOG(2, "move vector 0x%x to pcpu %u", info->vector, curBest);         
         if ((IT_ALLOW_FAKE_INTERRUPTS && info->isFake)
             || IDT_VectorSetDestination(info->vector, curBest)) {
            info->pcpuNum = curBest;
            info->prevInterrupts = intrCounts[info->pcpuNum][info->vector];
         } else {
            info->skip = TRUE;
            LOG(0, "failed to move vector 0x%x, will skip in future", info->vector);
         }
      }
      
      pcpuIntrTaken[curBest] += vectorCycles;
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * ITIdleRebalanceAll --
 *
 *      Revectors interrupts based on available idle time
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May re-vector interrupts. May update various accounting data.
 *
 *-----------------------------------------------------------------------------
 */
static void
ITIdleRebalanceAll(void)
{
   SP_IRQL prevIRQL;
   PCPU p;
   uint32 idleRebalancePeriodMS = CONFIG_OPTION(IRQ_REBALANCE_PERIOD);
   ITDataBuffer *buf = Mem_Alloc(sizeof(*buf));

   if (buf == NULL) {
      LOG(0, "insufficient memory to rebalance interrupts");
      return;
   }
   
   ASSERT(!CpuSched_IsPreemptible());
      
   // grab data without it.itLock held due to lock ordering
   CpuSched_PcpuUsageStats(buf->newIdle, buf->newUsed, buf->newOverlap);
   
   prevIRQL = SP_LockIRQ(&it.itLock, SP_IRQL_KERNEL);

   it.rebalancePeriodCycles = Timer_MSToTC(idleRebalancePeriodMS);
   it.vecCacheAffin = (it.rebalancePeriodCycles / 100)
      * CONFIG_OPTION(IRQ_VECTOR_CACHE_BONUS_PCT);
   it.pcpuMaxIntrLoad = (it.rebalancePeriodCycles / 100)
      * CONFIG_OPTION(IRQ_MAX_LOAD_PCT)
      / SMP_LogicalCPUPerPackage();
   memset(buf->pcpuIntrTaken, 0, sizeof(*buf->pcpuIntrTaken));
   ITComputePcpuIdleTimes(buf->newIdle, buf->newUsed, buf->newOverlap);
   ITComputeVectorCycles();

   IT_FORALL_VECTORS_BEGIN(info) {
      if (!info->skip) {
         ITRebalanceVector(info, buf->pcpuIntrTaken);
      }
   } IT_FORALL_VECTORS_END;

   // update current rates -- must be done after any revectoring happens
   // so that these are based on the current vectors of interrupts
   for (p=0; p < numPCPUs; p++) {
      it.pcpuIntrRates[p] = ITComputePcpuIntrRate(p);
   }
   SP_UnlockIRQ(&it.itLock, prevIRQL);
   
   Mem_Free(buf);
}


/*
 *-----------------------------------------------------------------------------
 *
 * ITRandomRebalanceAll --
 *
 *      Randomly revectors all known vectors to pcpus
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Randomly revectors all known vectors to pcpus
 *
 *-----------------------------------------------------------------------------
 */
void
ITRandomRebalanceAll(void)
{
   SP_IRQL prevIRQL;
   
   prevIRQL = SP_LockIRQ(&it.itLock, SP_IRQL_KERNEL);
   IT_FORALL_VECTORS_BEGIN(info) {
      PCPU newDest;
      if (!info->skip) {
         // re-vector to a random destination
         it.lastRand = Util_FastRand(it.lastRand);
         newDest = it.lastRand % numPCPUs;
         if ((IT_ALLOW_FAKE_INTERRUPTS && info->isFake)
             || IDT_VectorSetDestination(info->vector, newDest)) {
            LOG(1, "moved vector 0x%x to pcpu %u", info->vector, newDest);
            info->pcpuNum = newDest;
         } else {
            info->skip = TRUE;
            LOG(0, "failed to move vector 0x%x, will skip in future", info->vector);
         }
      }
   } IT_FORALL_VECTORS_END;
   
   SP_UnlockIRQ(&it.itLock, prevIRQL);
}

/*
 *-----------------------------------------------------------------------------
 *
 * ITRebalanceTimer --
 *
 *      Timer callback to rebalance interrupt vectors based on 
 *      current policy
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May re-vector interrupts. May update various accounting data.
 *
 *-----------------------------------------------------------------------------
 */
static void
ITRebalanceTimer(void *data, UNUSED_PARAM(Timer_AbsCycles timestamp))
{
   if (CONFIG_OPTION(IRQ_ROUTING_POLICY) == IT_IDLE_ROUTING) {
      ITIdleRebalanceAll();
   } else if (CONFIG_OPTION(IRQ_ROUTING_POLICY) == IT_RANDOM_ROUTING) {
      ITRandomRebalanceAll();
   }
   
   Timer_Add((MY_PCPU + 1) % numPCPUs,
             ITRebalanceTimer,
             CONFIG_OPTION(IRQ_REBALANCE_PERIOD),
             TIMER_ONE_SHOT, NULL);
}

/*
 *-----------------------------------------------------------------------------
 *
 * ITAutoManageVector --
 *
 *     Allows the interrupt tracker to manage this vector again.
 *     Used after the vector had been manually moved.
 *
 * Results:
 *     Returns VMK_OK on success, VMK_FAILURE if we couldn't manage this vector
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
ITAutoManageVector(uint32 vector)
{
   SP_IRQL prevIRQL;
   IT_VectorInfo *info;
   VMK_ReturnStatus res = VMK_FAILURE;

   LOG(0, "restoring automatic managment for vector 0x%x", vector);

   ASSERT(vector >= IDT_FIRST_EXTERNAL_VECTOR && 
          vector < IDT_NUM_VECTORS);

   prevIRQL = SP_LockIRQ(&it.itLock, SP_IRQL_KERNEL);
   
   info = itInfo[vector];
   if (info && !info->inList && info->refCount > 0) {
      // once this vector is back in the list, we'll
      // automatically manage it again on the next pass
      info->skip = FALSE;
      res = VMK_OK;
   } else {
      Warning("Vector 0x%x could not be auto-managed", vector);
   }
   SP_UnlockIRQ(&it.itLock, prevIRQL);

   return (res);
}


/*
 *-----------------------------------------------------------------------------
 *
 * ITManualVectorMove --
 *
 *     Redirects "vector" to processor "destPcpu" and prevents it from being
 *     managed by the interrupt tracker in the future (until ITAutoManageVector
 *     is called for it).
 *
 * Results:
 *     Returns VMK_OK on success, VMK_FAILURE if interrupt could not be redirected
 *
 * Side effects:
 *     Redirects vector and removes vector from it.infoList on success
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
ITManualVectorMove(uint32 vector, PCPU destPcpu)
{
   SP_IRQL prevIRQL;
   IT_VectorInfo *info;
   VMK_ReturnStatus res = VMK_FAILURE;

   LOG(0, "moving vector 0x%x to pcpu %u", vector, destPcpu);

   ASSERT(vector >= IDT_FIRST_EXTERNAL_VECTOR && 
          vector < IDT_NUM_VECTORS);

   if (destPcpu >= numPCPUs) {
      Warning("destination pcpu %u is invalid", destPcpu);
      return (VMK_BAD_PARAM);
   }

   prevIRQL = SP_LockIRQ(&it.itLock, SP_IRQL_KERNEL);
   
   info = itInfo[vector];
   if (info) {
      if (IDT_VectorSetDestination(vector, destPcpu)) {
         info->pcpuNum = destPcpu;
         // don't automanage this in the future
         info->skip = TRUE;
         res = VMK_OK;
      } else {
         Warning("failed to move vector 0x%x to pcpu %u", vector, destPcpu);
      }
   } else {
      Warning("vector 0x%x not found", vector);
   }
   
   SP_UnlockIRQ(&it.itLock, prevIRQL);

   return (res);
}

/*
 *-----------------------------------------------------------------------------
 *
 * ITProcRead --
 *
 *     Read handler for /proc/vmware/intr-tracker
 *
 * Results:
 *     Returns VMK_OK, writes proc output to buf and len
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static int
ITProcRead(Proc_Entry *e, char *buf, int *len)
{
   PCPU p;
   SP_IRQL prevIRQL;

   *len = 0;

   prevIRQL = SP_LockIRQ(&it.itLock, SP_IRQL_KERNEL);

   // display current vector destination
   Proc_Printf(buf, len, "\n\ncurrent vector destination:\n\n");
   Proc_Printf(buf, len, "Dest  ");
   IT_FORALL_VECTORS_BEGIN(info) {
      Proc_Printf(buf, len, "      0x%2x ", info->vector);
   } IT_FORALL_VECTORS_END;
   Proc_Printf(buf, len, "\n");

   Proc_Printf(buf, len, "      ");
   IT_FORALL_VECTORS_BEGIN(info) {
      if (info->pcpuNum == INVALID_PCPU) {
         Proc_Printf(buf, len, "%10s ", "Unk.");
      } else {
         Proc_Printf(buf, len, "%10u ", info->pcpuNum);
      }
   } IT_FORALL_VECTORS_END;

   // display systime per-pcpu, per vector
   Proc_Printf(buf, len, "\n\nvector systime per pcpu (and overall rate):\n\n");
   Proc_Printf(buf, len, "PCPU  ");
   IT_FORALL_VECTORS_BEGIN(info) {
      Proc_Printf(buf, len, "      0x%2x ", info->vector);
   } IT_FORALL_VECTORS_END;
   Proc_Printf(buf, len, "\n");
   for (p=0; p < numPCPUs; p++) {
      const char *rateName;
      IT_IntrRate pcpuRate;
      
      Proc_Printf(buf, len, "  %2u  ", p);

      IT_FORALL_VECTORS_BEGIN(info) {
         Timer_RelCycles sysCycles;
         uint64 sec;
         uint32 usec;
         
         CPUSCHED_VERSIONED_ATOMIC_READ_BEGIN(&info->sysCyclesVersions[p]);
         sysCycles = info->sysCycles[p];
         CPUSCHED_VERSIONED_ATOMIC_READ_END(&info->sysCyclesVersions[p]);
         Timer_TCToSec(sysCycles, &sec, &usec);
         
         Proc_Printf(buf, len, "%6Lu.%03u ", sec, usec / 1000);
      } IT_FORALL_VECTORS_END;

      pcpuRate = it.pcpuIntrRates[p];
      rateName = ITGetIntrRateName(pcpuRate);
      Proc_Printf(buf, len, " (%s)\n", rateName);
   }

   // display IT debugging counts per-pcpu
#if (IT_DEBUG)
   Proc_Printf(buf, len, "\nremote/idle/total per vector\n\n");
   IT_FORALL_VECTORS_BEGIN(info) {
      uint64 remote = 0, idle = 0, total = 0; 

      // sum debugging values across all pcpus
      for (p=0; p < numPCPUs; p++) {
         total += intrCounts[p][info->vector];
      }
      remote += info->remoteForwards;
      idle += info->idleCount;

      Proc_Printf(buf, len,"0x%2x  %10Lu/%10Lu/%10Lu\n",
                  info->vector,
                  remote,
                  idle,
                  total);
   } IT_FORALL_VECTORS_END;

   // display aged usage information per-pcpu
   Proc_Printf(buf, len, "\n\nPcpu idle - used + sys time: \n");
   for (p=0; p < numPCPUs; p++) {
      Bool negative = FALSE;
      uint64 sec;
      uint32 usec;
      int64 time = it.pcpuAgedIdle[p];
      
      if (time < 0) {
         time *= -1;
         negative = TRUE;
      }
      CpuSched_UsageToSec(time, &sec, &usec);
      Proc_Printf(buf, len, "PCPU %2u:  %c%Ld.%03u\n",
                  p,
                  negative ? '-' : ' ',
                  sec,
                  usec / 1000);
   }
#endif
   
   Proc_Printf(buf, len,
               "\n\ninterrupt counter overflows: %u",
               it.intrOverflows);

   SP_UnlockIRQ(&it.itLock, prevIRQL);

   Proc_Printf(buf, len, 
               "\n\nSupported commands: \n"
               "move <hexVector> <destPcpu> -- \n"
               "       Manually moves the specified vector to destPcpu\n"
               "       and no longer rebalances it automatically\n\n"
               "automate <hexVector> -- \n"
               "       Reinstates automatic rebalancing for the specified\n"
               "       vector\n"
               "thresh <low> <medium> <high> <excessive>\n"
               "       Configures interrupt-rebalancing thresholds,\n"
               "       measured in %% of a processor consumed by interrrupts\n"
#if IT_ALLOW_FAKE_INTERRUPTS
               "fake  <hexVector> <runUsec> <waitUsec>\n"
               "       Creates a new fake interrupt vector\n"
               "stop  <hexVector>\n"
               "       Removes the specified fake interrupt vector\n"
#endif
      );

   return (VMK_OK);
}

/*
 *-----------------------------------------------------------------------------
 *
 * ITProcWrite --
 *
 *     Write handler for /proc/vmware/intr-tracker.
 *     Allows manual movement of interrupts.
 *
 * Results:
 *     Returns VMK_OK on success, VMK_BAD_PARAM on invalid parameters
 *
 * Side effects:
 *     Will move an interrupt to the desired processor if successful.
 *
 *-----------------------------------------------------------------------------
 */
static int
ITProcWrite(Proc_Entry *e, char *buf, int *len)
{
   int argc;
   uint32 vector, destPcpu;
   char *argv[5];
   
   argc = Parse_Args(buf, argv, 5);
   if (argc < 2
       || Parse_Hex(argv[1], strlen(argv[1]), &vector) != VMK_OK) {
      Warning("command not understood");
      return (VMK_BAD_PARAM);
   }
   if (vector < IDT_FIRST_EXTERNAL_VECTOR
       || vector >= IDT_NUM_VECTORS) {
      Warning("vector 0x%x is invalid", vector);
      return (VMK_BAD_PARAM);
   }

   if (argc == 5 && strcmp(argv[0], "thresh") == 0) {
      uint32 high, medium, low, excess;
      SP_IRQL prevIRQL;

      if (Parse_Int(argv[1], strlen(argv[1]), &low) != VMK_OK
          || Parse_Int(argv[2], strlen(argv[2]), &medium) != VMK_OK
          || Parse_Int(argv[3], strlen(argv[3]), &high) != VMK_OK
          || Parse_Int(argv[4], strlen(argv[4]), &excess) != VMK_OK) {
         Warning("could not parse thresholds");
         return (VMK_BAD_PARAM);
      }
      if (low > 100 || medium > 100 || high > 100 || excess > 100
         || low > medium || medium > high || high > excess) {
         Warning("invalid thresholds");
         return (VMK_BAD_PARAM);
      }

      prevIRQL = SP_LockIRQ(&it.itLock, SP_IRQL_KERNEL);
      ITSetupThresholds(low, medium, high, excess);
      SP_UnlockIRQ(&it.itLock, prevIRQL);
      return (VMK_OK);
   }
   
   if (argc == 3 
       && strcmp(argv[0], "move") == 0
       && Parse_Int(argv[2], strlen(argv[2]), &destPcpu) == VMK_OK) {
      return ITManualVectorMove(vector, (PCPU)destPcpu);
   } else if (argc == 2 
              && strcmp(argv[0], "automate") == 0) {
      return ITAutoManageVector(vector);
   }

#if (IT_ALLOW_FAKE_INTERRUPTS)
   if (argc == 4 && !strcmp(argv[0], "fake")) {
      uint32 microRun, microWait, vector;
      
      if (Parse_Hex(argv[1], strlen(argv[1]), &vector) != VMK_OK ||
          Parse_Int(argv[2], strlen(argv[2]), &microRun) != VMK_OK ||
          Parse_Int(argv[3], strlen(argv[3]), &microWait) != VMK_OK) {
         Warning("invalid number format");
         return (VMK_BAD_PARAM);
      }
      
      return ITAddFakeInterrupt(vector, microRun, microWait);
   }
      
   if (argc == 2 && !strcmp(argv[0], "stop")) {
      uint32 vector;
      if (Parse_Hex(argv[1], strlen(argv[1]), &vector) != VMK_OK) {
         Warning("invalid vector");
         return (VMK_BAD_PARAM);
      }
      
      return ITRemoveFakeInterrupt(vector);
   }
#endif
   
   Warning("command not understood");
   return (VMK_BAD_PARAM);
}


/*
 *-----------------------------------------------------------------------------
 *
 * ITSetupThresholds --
 *
 *      Configures basic global thresholds
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Configures it.intrThresh array
 *
 *-----------------------------------------------------------------------------
 */
static void
ITSetupThresholds(uint32 lowPct,
                  uint32 medPct,
                  uint32 highPct,
                  uint32 excessivePct)
{
   Timer_RelCycles onePct = it.rebalancePeriodCycles / 100;

   memset(it.pcpuIntrRates, 0, sizeof(IT_IntrRate) * MAX_PCPUS);
   memset(it.pcpuPrevIdle, 0, sizeof(Timer_RelCycles) * MAX_PCPUS);
   memset(it.pcpuAgedIdle, 0, sizeof(Timer_RelCycles) * MAX_PCPUS);

   it.intrThresh[INTR_RATE_NONE] = 0;
   it.intrThresh[INTR_RATE_LOW] = lowPct * onePct;
   it.intrThresh[INTR_RATE_MEDIUM] = medPct * onePct;
   it.intrThresh[INTR_RATE_HIGH] = highPct * onePct;
   it.intrThresh[INTR_RATE_EXCESSIVE] = excessivePct * onePct;
}


/*
 *----------------------------------------------------------------------
 *
 * IT_Init --
 *
 *      Initialize the IT module.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The lock is initialized.
 *
 *----------------------------------------------------------------------
 */
void
IT_Init(void)
{
   SP_IRQL prevIRQL;

   memset(&it, 0, sizeof(it));

   it.intrCycleWeight = RateConv_Unsigned(&myPRDA.tscToTC, IT_INTR_CYCLE_WEIGHT);
   it.lastRand = Util_RandSeed();
   it.rebalancePeriodCycles = Timer_MSToTC(CONFIG_OPTION(IRQ_REBALANCE_PERIOD));
   SP_InitLockIRQ("itLck", &it.itLock, SP_RANK_IRQ_MEMTIMER);

   prevIRQL = SP_LockIRQ(&it.itLock, SP_IRQL_KERNEL);
   ITSetupThresholds(IT_LOW_PCT, IT_MEDIUM_PCT, IT_HIGH_PCT, IT_EXCESSIVE_PCT);
   SP_UnlockIRQ(&it.itLock, prevIRQL);

   Proc_InitEntry(&it.itProcEnt);
   it.itProcEnt.read = ITProcRead;
   it.itProcEnt.write = ITProcWrite;
   Proc_Register(&it.itProcEnt, "intr-tracker", FALSE);

   Timer_Add(0, ITRebalanceTimer, CONFIG_OPTION(IRQ_REBALANCE_PERIOD),
             TIMER_ONE_SHOT, NULL);

}

/*
 *----------------------------------------------------------------------
 *
 * IT_NotifyHostSharing --
 *
 *    Notify whether the host has started or stopped sharing a vector.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    Update the hostShared and pcpuNum fields.
 *
 *----------------------------------------------------------------------
 */
void
IT_NotifyHostSharing(uint32 vector, Bool shared)
{
   SP_IRQL prevIRQL = SP_LockIRQ(&it.itLock, SP_IRQL_KERNEL);

   ASSERT(IDT_VectorIsDevInterrupt(vector));

   if (itInfo[vector] != NULL) {
      // We can no longer assume we know where we are
      itInfo[vector]->pcpuNum = INVALID_PCPU;
      if (shared) {
         // We let balancing fail on its own if needs be
	 /*
	  * NOTE: We don't set skip to TRUE on purpose.
	  * This routine has to be called from idt.c without the idtLock
	  * so there is a possibility that the vector is no longer shared.
	  * Balancing will implicitly recheck under the idtLock when
	  * calling IDT_VectorSetDestination.
	  */
      } else {
         // We let balancing start up again
         itInfo[vector]->skip = FALSE;
      }
   }

   SP_UnlockIRQ(&it.itLock, prevIRQL);
}

#if IT_ALLOW_FAKE_INTERRUPTS

typedef struct {
   uint32 microRun;
   uint32 microWait;
   uint32 vector;
   Bool stop;
   Timer_Handle timer;
   uint32 lastRand;
} ITFakeIntrConfig;

static ITFakeIntrConfig fakeInterrupts[IDT_NUM_VECTORS];

/*
 *-----------------------------------------------------------------------------
 *
 * ITFakeIntrCB --
 *
 *      Callback function for fake interrupt processing;
 *      burns some "systime" and reinstalls itself
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Burns time, re-registers timer.
 *
 *-----------------------------------------------------------------------------
 */
static void
ITFakeIntrCB(void *data, UNUSED_PARAM(Timer_AbsCycles timestamp))
{
   ITFakeIntrConfig *config = (ITFakeIntrConfig*) data;
   uint32 eflags;
   uint32 thisMicroRun, thisMicroWait;
   IT_VectorInfo *info = itInfo[config->vector];

   // add +/- 10% random jitter to run and wait times
   config->lastRand = Util_FastRand(config->lastRand);
   thisMicroRun = (config->lastRand % (config->microRun / 5))
      + (9 * config->microRun / 10);
   config->lastRand = Util_FastRand(config->lastRand);
   thisMicroWait = ((config->lastRand % (config->microWait / 5)))
      + (9 * config->microWait / 10);

   // we need to disable interrupts, because SysService expects it
   SAVE_FLAGS(eflags);
   CLEAR_INTERRUPTS();

   LOG(2, "firing callback for vector 0x%x, microRun=%u",
       config->vector,
       thisMicroRun);
   IT_Count(config->vector, info->pcpuNum);
   
   // pretend we were a top-half, even though this
   // code will run out of a timer handler in a bottom half
   intrCounts[info->pcpuNum][info->vector]++;
   
   // pretend we're an interrupt by burning time in "SysService" region
   Sched_SysServiceStart(NULL, config->vector);
   Util_Udelay(thisMicroRun);
   Sched_SysServiceDone();

   RESTORE_FLAGS(eflags);

   if (!config->stop && vmkernelLoaded) {
      LOG(2, "re-register interrupt with wait time %u", thisMicroWait);
      config->timer = Timer_AddHiRes(info->pcpuNum,
                                     ITFakeIntrCB,
                                     thisMicroWait,
                                     TIMER_ONE_SHOT,
                                     (void*) config);
   } else {
      // we're supposed to unregister, so don't re-add the timer
      LOG(0, "unregistering callback for vector 0x%x", info->vector);
      IT_UnregisterVector(info->vector);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * ITAddFakeInterrupt --
 *
 *      Installs a new fake interrupt source with interrupt vector "vector"
 *      which will run for "microRun" microseconds on each firing, with
 *      microWait milliseconds between firings.
 *
 * Results:
 *      Returns VMK_OK on success, VMK_BAD_PARAM or VMK_NO_RESOURCES otherwise
 *
 * Side effects:
 *      Installs new fake interrupt source
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
ITAddFakeInterrupt(uint32 vector, uint32 microRun, uint32 microWait)
{
   ITFakeIntrConfig *config;
   SP_IRQL prevIRQL;
   VMK_ReturnStatus result;
   
   if (vector >= IDT_NUM_VECTORS) {
      Warning("vector too large, limit is 0x%x", IDT_NUM_VECTORS);
      return (VMK_BAD_PARAM);
   }

   prevIRQL = SP_LockIRQ(&it.itLock, SP_IRQL_KERNEL);
   
   config = &fakeInterrupts[vector];
   if (itInfo[vector] == NULL) {
      config->microRun = microRun;
      config->microWait = microWait;
      config->vector = vector;
      config->lastRand = Util_RandSeed();
      config->stop = FALSE;
      config->timer = Timer_AddHiRes(MY_PCPU,
                                     ITFakeIntrCB,
                                     microWait,
                                     TIMER_ONE_SHOT,
                                     (void*) config);
      ITRegisterVectorInt(config->vector, TRUE);
      result = VMK_OK;
   } else {
      Warning("vector already in use");
      result = VMK_NO_RESOURCES;
   }
   
   SP_UnlockIRQ(&it.itLock, prevIRQL);
   
   return (result);
}

/*
 *-----------------------------------------------------------------------------
 *
 * ITRemoveFakeInterrupt --
 *
 *      Unregisters the fake interrupt source corresponding to "vector"
 *      after the next firing of its timer.
 *
 * Results:
 *      Returns VMK_OK on success
 *
 * Side effects:
 *      Eliminates the fake interrupt source.
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
ITRemoveFakeInterrupt(uint32 vector)
{
   SP_IRQL prevIRQL;
   ITFakeIntrConfig *config;
   
   prevIRQL = SP_LockIRQ(&it.itLock, SP_IRQL_KERNEL);
   config = &fakeInterrupts[vector];
   config->stop = TRUE;
   SP_UnlockIRQ(&it.itLock, prevIRQL);
   
   return (VMK_OK);
}


#endif // IT_ALLOW_FAKE_INTERRUPTS
