/* **********************************************************
 * Copyright 2000 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * sched_sysacct.h --
 *
 *	Scheduler routines to account for system time.
 */

#ifndef _SCHED_SYSACCT_H
#define _SCHED_SYSACCT_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "sched.h"
#include "world.h"
#include "timer.h"
#include "cpusched.h"
#include "it.h"

/*
 * constants
 */

#define	SCHED_SYS_ACCT_SAMPLE_LG	(3)
#define	SCHED_SYS_ACCT_SAMPLE		(1 << SCHED_SYS_ACCT_SAMPLE_LG)
#define	SCHED_SYS_ACCT_MASK		(SCHED_SYS_ACCT_SAMPLE - 1)
#define	SCHED_SYS_ACCT_SHIFT_MAX	(30 - SCHED_SYS_ACCT_SAMPLE_LG)

/*
 * operations
 */

static INLINE void
SchedSysServiceRandom(void)
{
   uint64 product    = 33614 * (uint64) myPRDA.vmkServiceRandom;
   uint32 product_lo = (uint32) (product & 0xffffffff) >> 1;
   uint32 product_hi = product >> 32;
   int32  test       = product_lo + product_hi;
   myPRDA.vmkServiceRandom = (test > 0) ? test : (test & 0x7fffffff) + 1;
}

static INLINE void
Sched_SysServiceWorld(struct World_Handle *world)
{
   myPRDA.vmkServiceWorld = world;
}

/*
 *-----------------------------------------------------------------------------
 *
 * Sched_SysServiceStart --
 *
 *    Mark the beginning of system work on behalf of the given 'world',
 *    and as a consequence of the given interrupt 'vector'. Used to account
 *    for system time used by the given world in the context of another
 *    world. Caller is responsible for invoking Sched_SysServiceDone() when
 *    it finishes servicing the current world.
 *
 *    In case 'world' cannot be determined at the time of
 *    calling this function, it should be set later by calling
 *    Sched_SysServiceWorld().
 *
 *    Must be called while the current world is not preemptible
 *    to prevent nesting.
 *
 * Results:
 *    Returns TRUE iff accounting was actually started.
 *
 * Side effects:
 *    None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE Bool
Sched_SysServiceStart(struct World_Handle *world, uint32 vector)
{
   int16 rndShift = myPRDA.vmkServiceShift;
   
   ASSERT(!CpuSched_IsPreemptible());

   // Disallow nesting of SysService.
   // Note that it IS possible to nest up until the point where
   // vmkServiceStart is written to memory, but that's ok because
   // we can tolerate stale vmkServiceShift and vmkServiceRandom values
   if (myPRDA.vmkServiceStart != 0) {
      return (FALSE);
   }
   
   // generate new random number when insufficient random bits
   if (rndShift < 0) {
      SchedSysServiceRandom();
      rndShift = SCHED_SYS_ACCT_SHIFT_MAX;
   }
   myPRDA.vmkServiceShift = rndShift - SCHED_SYS_ACCT_SAMPLE_LG;

   // sample on average once per SCHED_SYS_ACCT_SAMPLE service operations
   if (((myPRDA.vmkServiceRandom >> rndShift) & SCHED_SYS_ACCT_MASK) == 0) {
      // Note that the ordering of these writes is very important:
      // we MUST update vmkServiceStart first so that any interrupt that
      // arrives will see vmkServiceStart != 0 and return without updating
      // the PRDA fields.  The COMPILER_MEM_BARRIER() prevents gcc from
      // reordering these writes. Because the readers/writers are only
      // on the same processor, we don't have to worry about the processor
      // reordering the writes either.
      myPRDA.vmkServiceStart = RDTSC();
      COMPILER_MEM_BARRIER();
      myPRDA.vmkServiceWorld = world;
      myPRDA.vmkServiceVector = vector;
      return (TRUE);
   } else {
      // don't actually start a sample this time
      return (FALSE);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * Sched_SysServiceDone --
 *
 *      Account for elapsed service time on the current processor
 *	since the previous call to Sched_SysServiceStart().
 *
 *      Must be called with interrupts disabled.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	May modify global scheduling state.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
Sched_SysServiceDone(void)
{
   ASSERT(!CpuSched_IsPreemptible());

   // special case: ignore if no start time
   if (myPRDA.vmkServiceStart == 0) {
      return;
   }

   CpuSched_SysServiceDoneSample();
}

#endif
