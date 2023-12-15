/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * timer.h --
 *
 *	Timer module.  See also timer_dist.h.
 */

#ifndef _TIMER_H
#define _TIMER_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"
#include "timer_dist.h"
#include "splock.h"

/*
 * Exported globals
 */
extern uint64 cpuHzEstimate;
extern uint32 cpuKhzEstimate;
extern uint64 busHzEstimate;
extern uint32 busKhzEstimate;

/*
 * Compilation options
 */
// enable soft timer polls
#define SOFTTIMERS 1

/*
 * Exported functions
 */
extern void Timer_Init(void);
extern void Timer_LateInit(void);
extern void Timer_CorrectForTSCShift(TSCRelCycles tscShift);
extern Bool Timer_ModifyTimeoutTC(Timer_Handle handle,
                                  Timer_AbsCycles deadlineTC,
                                  Timer_RelCycles periodTC);
extern Bool Timer_GetTimeoutTC(Timer_Handle handle,
                               Timer_AbsCycles *deadlineTC,
                               Timer_RelCycles *periodTC);
extern Bool Timer_Initialized(void);

extern VMK_ReturnStatus Timer_UpdateConfig(Bool write, Bool valueChanged, int indx);


/*
 *----------------------------------------------------------------------
 *
 * Timer_ModifyTimeoutHiRes --
 *
 *      Changes the period and deadline of a timer if the timer is
 *      still pending.  The timeout is given in microseconds.
 *
 * Results:
 *      Returns TRUE if the timer was successfully changed; FALSE
 *      otherwise.  FALSE means that either the timer was previously
 *      removed, or it was a one-shot that has already fired or is in
 *      the process of firing.
 *
 * Side effects:
 *      The timer wheel is modified.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
Timer_ModifyTimeoutHiRes(Timer_Handle handle, // IN
                         int64 timeoutUS)     // IN
{
   Timer_RelCycles timeoutTC = Timer_USToTC(timeoutUS);
   return Timer_ModifyTimeoutTC(handle, Timer_GetCycles() + timeoutTC,
                                timeoutTC);
}


/*
 *----------------------------------------------------------------------
 *
 * Timer_ModifyTimeout --
 *
 *      Changes the period and deadline of a timer if the timer is
 *      still pending.  The timeout is given in milliseconds.
 *
 * Results:
 *      Returns TRUE if the timer was successfully changed; FALSE
 *      otherwise.  FALSE means that either the timer was previously
 *      removed, or it was a one-shot that has already fired or is in
 *      the process of firing.
 *
 * Side effects:
 *      The timer wheel is modified.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
Timer_ModifyTimeout(Timer_Handle handle, // IN
                    int32 timeoutMS)     // IN
{
   Timer_RelCycles timeoutTC = Timer_MSToTC(timeoutMS);
   return Timer_ModifyTimeoutTC(handle, Timer_GetCycles() + timeoutTC,
                                timeoutTC);
}


extern void Timer_Interrupt(void);
extern VMKERNEL_ENTRY Timer_Info(DECLARE_1_ARG(VMK_TIMER_INFO,
                                               uint32 *, info));
extern uint64 Timer_SysUptime(void);

extern void Timer_BHHandler(void *ignore);

struct World_Handle;
struct World_InitArgs;
extern void Timer_WorldCleanup(struct World_Handle *world);
extern VMK_ReturnStatus Timer_WorldInit(struct World_Handle *world, struct World_InitArgs *args);

extern void Timer_InitCycles(void);

extern void Timer_InitPseudoTSC(void);
extern TSCCycles Timer_PseudoTSC(void);
extern void Timer_UpdateWorldPseudoTSCConv(struct World_Handle *world,
                                           Timer_AbsCycles timestamp);

extern int64 Timer_GetTimeOfDay(void);
extern void Timer_SetTimeOfDay(int64 tod);

extern void Timer_TCToSec(Timer_Cycles tc, uint64 *sec, uint32 *usec);
extern void Timer_TSCToSec(TSCCycles tc, uint32 *sec, uint32 *usec);


/*
 * Code fragments for use in measuring the frequency of other timers
 * against the PIT, which runs at a known frequency.
 */

void Timer_HzEstimateInit(void);

#define SPEAKER_PORT	 0x61
#define CLICKS_PER_SEC   1193182ull  // PIT timer input clock frequency in Hz
#define CLICKS_PER_LOOP  (1<<16)

/*
 * This code goes just before you record the beginning value of the
 * timer being measured.  The argument is the approximate number of
 * seconds to run the test.
 */
#define HZ_ESTIMATE_BEGIN(testSecs) \
{ \
   uint32 _testLoops = ((CLICKS_PER_SEC * (testSecs) + CLICKS_PER_LOOP / 2) \
                          / CLICKS_PER_LOOP); \
   uint32 _flags, _i = _testLoops + 1; \
   \
   if (Timer_GetCycles != NULL) { \
      Timer_GetCycles(); /* must be called at least every 5.368 seconds */ \
      ASSERT(testSecs <= 5); \
   } \
   SAVE_FLAGS(_flags); \
   CLEAR_INTERRUPTS(); \
   while (~INB(SPEAKER_PORT) & 0x20) { PAUSE(); } \
   while (INB(SPEAKER_PORT) & 0x20) { PAUSE(); }

/* 
 * This code goes after you record the starting value.  It is tweaked
 * to get the best output from gcc.  It could still be improved a
 * little by coding in asm, but that doesn't much matter when running
 * the test for several seconds.
 */
#define HZ_ESTIMATE_DELAY \
   while (--_i) { \
      while (~INB(SPEAKER_PORT) & 0x20) { PAUSE(); } \
      while (INB(SPEAKER_PORT) & 0x20) { PAUSE(); } \
   }

/* This code computes the frequency estimate in Hz.  The argument is
 * the number of cycles that passed on the timer being measured
 */
#define HZ_ESTIMATE_COMPUTE(count) \
   ((count) * CLICKS_PER_SEC / (_testLoops * CLICKS_PER_LOOP))

/*
 * This code goes at the end
 */
#define HZ_ESTIMATE_END \
   RESTORE_FLAGS(_flags); \
   if (Timer_GetCycles != NULL) { \
      Timer_GetCycles(); /* must be called at least every 5.368 seconds */ \
   } \
}

extern uint64 Timer_CPUHzEstimate(void);

#endif

