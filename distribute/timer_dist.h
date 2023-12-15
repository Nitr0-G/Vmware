/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * timer_dist.h --
 *
 *	Timer module.
 */

#ifndef _TIMER_DIST_H
#define _TIMER_DIST_H

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_DISTRIBUTE
#define INCLUDE_ALLOW_VMK_MODULE
#define INCLUDE_ALLOW_USERLEVEL
#include "includeCheck.h"

#include "vmkernel_dist.h"
#include "rateconv.h"
#include "vm_basic_asm.h"
#include "vm_assert.h"

// timer behavior
#define TIMER_ONE_SHOT	0x01
#define TIMER_PERIODIC	0x02
#define TIMER_MIN_PERIOD Timer_USToTC(100)

typedef uint64 Timer_Handle;
#define TIMER_HANDLE_NONE ((Timer_Handle) 0)

#define DEFAULT_GROUP_ID                0

/*
 * Basic types for values in Timer module units.  These are cycles on
 * whatever counter the Timer module is using to implement vmkernel
 * timers.  They are NOT necessarily CPU cycles.
 */
typedef uint64 Timer_Cycles;     // Generic type
typedef uint64 Timer_AbsCycles;  // For absolute times (since vmkernel loaded)
typedef int64  Timer_RelCycles;  // For relative times

// cpu cycles, as measured by the processor's timestamp counter (TSC)
typedef uint64 TSCCycles;
typedef int64 TSCRelCycles;

typedef uint64 TimerGroupID;

typedef void (*Timer_Callback)(void *data, Timer_AbsCycles timestamp);

// for use by inlines below
extern RateConv_Params timerMSToTC, timerUSToTC, timerNSToTC,
   timerTCToNS, timerTCToUS, timerTCToMS;

extern Bool Timer_Remove(Timer_Handle handle);
extern Bool Timer_RemoveSync(Timer_Handle handle);
extern Bool Timer_Pending(Timer_Handle handle);
extern TimerGroupID Timer_CreateGroup(PCPU pcpu);
extern void Timer_RemoveGroup(TimerGroupID groupID);

/*
 *----------------------------------------------------------------------
 *
 * Timer_GetCycles --
 *
 *      Read the fine-grained timer.  On shared-bus SMP machines, this
 *      is the TSC -- we synchronize the TSCs of all processors at
 *      power-up, and they run at the same speed thereafter because
 *      they share a common bus clock.  (Note that the initial
 *      synchronization is not absolutely perfect.  You may observe a
 *      TSC or TSC-based Timer_GetCycles to go backward by a few
 *      cycles when a world migrates between PCPUs.)  On IBM NUMA
 *      machines where the TSCs run at significantly different rates
 *      across nodes, we use the performance event counter (MPMC) in
 *      node 0's Cyclone or Twister chip, set to count bus cycles.
 *      The frequency of the timer is given by Timer_CyclesPerSecond().
 *
 * Results:
 *      64-bit timestamp.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
extern Timer_AbsCycles (*Timer_GetCycles)(void);

extern uint64 Timer_CyclesPerSecond(void);

/*
 *----------------------------------------------------------------------
 *
 * Timer_MSToTC --
 *
 *      Convert time in milliseconds to time in timer cycles.
 *	Note the warning about the precision of converting to/from
 * 	timestamp units at the end of Timer_InitCycles.
 *
 * Results:
 *      Time in timestamp units.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Timer_RelCycles
Timer_MSToTC(int32 ms)      // IN
{
   return Muls64x32s64(ms, timerMSToTC.mult, timerMSToTC.shift);
}

/*
 *----------------------------------------------------------------------
 *
 * Timer_USToTC --
 *
 *      Convert time in microseconds to time in timer cycles.
 *	Note the warning about the precision of converting to/from
 * 	timestamp units at the end of Timer_InitCycles.
 *
 * Results:
 *      Time in timestamp units.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Timer_RelCycles
Timer_USToTC(int64 us)       // IN
{
   return Muls64x32s64(us, timerUSToTC.mult, timerUSToTC.shift);
}

/*
 *----------------------------------------------------------------------
 *
 * Timer_NSToTC --
 *
 *      Convert time in nanoseconds to time in timer cycles.
 *	Note the warning about the precision of converting to/from
 * 	timestamp units at the end of Timer_InitCycles.
 *
 * Results:
 *      Time in timestamp units. 
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Timer_RelCycles
Timer_NSToTC(int64 ns)      // IN
{
   return Muls64x32s64(ns, timerNSToTC.mult, timerNSToTC.shift);
}

/*
 *----------------------------------------------------------------------
 *
 * Timer_TCToNS --
 *
 *      Convert time in timer cycles to time in nanoseconds.
 *	Note the warning about the precision of converting to/from
 * 	timestamp units at the end of Timer_InitCycles.
 *
 * Results:
 *      Time in timestamp units. 
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE int64
Timer_TCToNS(Timer_RelCycles tc)      // IN
{
   return Muls64x32s64(tc, timerTCToNS.mult, timerTCToNS.shift);
}

/*
 *----------------------------------------------------------------------
 *
 * Timer_TCToUS --
 *
 *      Convert time in timer cycles to time in microseconds.
 *	Note the warning about the precision of converting to/from
 * 	timestamp units at the end of Timer_InitCycles.
 *
 * Results:
 *      Time in timestamp units.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE int64
Timer_TCToUS(Timer_RelCycles tc)      // IN
{
   return Muls64x32s64(tc, timerTCToUS.mult, timerTCToUS.shift);
}

/*
 *----------------------------------------------------------------------
 *
 * Timer_TCToMS --
 *
 *      Convert time in timer cycles to time in milliseconds.
 *	Note the warning about the precision of converting to/from
 * 	timestamp units at the end of Timer_InitCycles.
 *
 * Results:
 *      Time in timestamp units.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE int64
Timer_TCToMS(Timer_RelCycles tc)      // IN
{
   return Muls64x32s64(tc, timerTCToMS.mult, timerTCToMS.shift);
}

/*
 *----------------------------------------------------------------------
 *
 * Timer_{MS,US,TSC}To{MS,US,TSC} --
 *
 *      Convert time in TSC units to/from time in milliseconds (MS)
 *	and microseconds (US).
 *
 * Results:
 *      Time in converted units.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE TSCCycles
Timer_MSToTSC(uint64 ms)
{
   return (ms * cpuKhzEstimate);
}
static INLINE TSCCycles
Timer_USToTSC(uint64 us)
{
   return (us * cpuMhzEstimate);
}
static INLINE uint64
Timer_TSCToMS(TSCCycles tscCycles)
{
   return (tscCycles / cpuKhzEstimate);
}
static INLINE uint64
Timer_TSCToUS(TSCCycles tscCycles)
{
   return (tscCycles / cpuMhzEstimate);
}

extern Timer_Handle Timer_AddTC(PCPU pcpu,
                                TimerGroupID groupID,
                                Timer_Callback cb,
                                Timer_AbsCycles deadlineTC,
                                Timer_RelCycles periodTC,
                                void *data);

/*
 *----------------------------------------------------------------------
 *
 * Timer_Add --
 *
 *      Add an new timer with the given parameters.  The timeout is
 *      given in milliseconds.
 *
 * Results:
 *      Handle for the new timer.
 *
 *----------------------------------------------------------------------
 */
static INLINE Timer_Handle
Timer_Add(PCPU pcpu,
          Timer_Callback cb,
          int32 timeoutMS,
          uint32 flags,
          void *data)
{
   Timer_RelCycles timeoutTC = Timer_MSToTC(timeoutMS);
   return Timer_AddTC(pcpu, DEFAULT_GROUP_ID, cb,
                      Timer_GetCycles() + timeoutTC,
                      flags == TIMER_PERIODIC ? timeoutTC : 0, data);
}

/*
 *----------------------------------------------------------------------
 *
 * Timer_AddToGroup --
 *
 *      Add an new timer with the given parameters.  The timeout is
 *      given in milliseconds.
 *
 * Results:
 *      Handle for the new timer.
 *
 *----------------------------------------------------------------------
 */
static INLINE Timer_Handle
Timer_AddToGroup(PCPU pcpu,
                 TimerGroupID groupID,
                 Timer_Callback cb,
                 int32 timeoutMS,
                 uint32 flags,
                 void *data)
{
   Timer_RelCycles timeoutTC = Timer_MSToTC(timeoutMS);
   return Timer_AddTC(pcpu, groupID, cb,
                      Timer_GetCycles() + timeoutTC,
                      flags == TIMER_PERIODIC ? timeoutTC : 0, data);
}

/*
 *----------------------------------------------------------------------
 *
 * Timer_AddHiRes --
 *
 *      Add an new timer with the given parameters.  The timeout is
 *      given in microseconds.
 *
 * Results:
 *      Handle for the new timer.
 *
 *----------------------------------------------------------------------
 */
static INLINE Timer_Handle
Timer_AddHiRes(PCPU pcpu,
               Timer_Callback cb,
	       int64 timeoutUS,
               uint32 flags,
               void *data)
{
   Timer_RelCycles timeoutTC = Timer_USToTC(timeoutUS);
   return Timer_AddTC(pcpu, DEFAULT_GROUP_ID, cb,
                      Timer_GetCycles() + timeoutTC,
                      flags == TIMER_PERIODIC ? timeoutTC : 0, data);
}
#endif
