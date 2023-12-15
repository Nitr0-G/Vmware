/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * timer.c --
 *
 *	This module provides a time-delayed callback service for the
 *	vmkernel. See Timer_AddTC, Timer_GetTimeoutTC,
 *	Timer_ModifyTimeoutTC, Timer_Pending, and Timer_Remove for the
 *	client interface. The module also implements periodic
 *	VMK_ACTION_TIMER_INTR actions to help drive guest timers in
 *	VMs, timer-related /proc nodes, and some other timer-related
 *	code.
 *
 *      The implementation uses the concepts of timer handles, timer
 *      wheels [1] and soft timers [2].
 * 
 *      [1] George Varghese and Tony Lauck. Hashed and Hierarchical
 *      Timing Wheels: Efficient Data Structures for Implementing a
 *      Timer Facility. http://citeseer.nj.nec.com/varghese96hashed.html
 * 
 *      [2] Mohit Aron and Peter Druschel. Soft timers: efficient
 *      microsecond software timer support for network processing.
 *      http://www.cs.rice.edu/CS/Systems/Soft-timers/
 *
 *      A timer handle is a soft pointer to a timer; it contains
 *      enough information to find the timer data structure directly,
 *      plus a generation count to allow stale handles to be detected.
 *      We use 64-bit handles so that we don't have to worry about a
 *      stale handle being reused and becoming valid again. Handles
 *      allow an O(1) implementation of Timer_Remove,
 *      Timer_ModifyTimeout, and Timer_Pending.
 *
 *      A timer wheel is roughly a hash table, where timers are
 *      assigned to buckets (or *spokes*) based on some low-order or
 *      middle-order bits of their next deadline. The hashing keeps
 *      the timers roughly sorted by deadline at low cost, and the
 *      wheel structure makes it efficient to find the next timer due
 *      to fire. There are several variants of timer wheels; we use
 *      one with sorted spokes (scheme 5 in [1]) because it integrates
 *      better with soft timers than the more common variant with
 *      unsorted spokes (scheme 6 in [1], used in BSD Unix). With
 *      sorted spokes, insertion is no longer O(1) as with unsorted
 *      spokes, but it should be O(1) for practical purposes if the
 *      wheel is made large enough that each spoke typically contains
 *      about 1 timer.
 *
 *      Soft timers are timers that are checked to see if they are due
 *      to fire not only on a hardware timer interrupt, but also at
 *      other convenient points -- typically on every exit from the
 *      kernel, i.e., whenever bottom halves are run. Soft timers are
 *      useful when you need to set timers with short time periods but
 *      can tolerate sometimes having them go off late. For example,
 *      they are good for implementing pacing timers in network
 *      protocols. We implement soft timers simply by checking the
 *      timer wheel whenever bottom halves are due to run, not just on
 *      hard interrupts. Because the timer wheel spokes are sorted,
 *      each soft poll typically checks the head of only one spoke, so
 *      soft polls are cheap in the common case where there is no
 *      timer to fire, and are no more expensive when firing a timer
 *      than the usual poll on hard interrupt.
 *
 *      (Note: Aron and Druschel seem to have missed the insight about
 *      sorted spokes integrating best with soft timers. Their soft
 *      timers implementation uses a timer wheel with unsorted spokes,
 *      plus an extra pointer to the timer with the next deadline.
 *      Keeping this extra pointer up to date blows the advantages of
 *      timer wheels and timer handles; for example, updating it after
 *      a Timer_Remove can take O(n) time.)
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "vm_asm.h"
#include "x86.h"
#include "util.h"
#include "vmkernel.h"
#include "world.h"
#include "sched.h"
#include "idt.h"
#include "sched.h"
#include "vmnix_if.h"
#include "host.h"
#include "bh.h"
#include "proc.h"
#include "rateconv.h"
#include "apic.h"
#include "numa.h"
#include "../hardware/summit.h"
#include "timer.h"
#include "world.h"
#include "user.h"

#define LOGLEVEL_MODULE Timer
#include "log.h"

#include "post.h"

/*
 * Compilation flags
 */
// put all guest timers on pcpu 0
#define TIMERON0 0
// move guest timer if world moves
#define TIMERMIGRATE 1
// Timer_RemoveSync panics after multiple spinout warnings
#define SPIN_OUT_CYCLES 4000000000ul
#define SPIN_OUTS_BEFORE_PANIC 5

/*
 * Constants
 * XXX Perhaps should make these tunable at power-up.
 */

/*
 * Maximum number of timers that can be scheduled at once, and format
 * of a Timer_Handle.
 *
 * The maximum number of timers needed depends on number of worlds expected
 * on each CPU assuming we want to allow each world to sleep for some time
 * duration.  We limit to 8-10 VM per CPU, and each VM can have up to 8-10
 * worlds with all the VMX/VMM threads.  Plus random system worlds
 * (idle/helper, etc).  So, that's about 100 worlds per CPU if worlds
 * equally balanced across CPUs.  Assuming worst misbalancing is 4x, 512
 * timers should be plenty.
 *
 * MAX_TIMERS_BITS must be at least the base 2 log of MAX_TIMERS.
 *
 * A Timer_Handle is the 64-bit concatenation ABC of three bit strings:
 *
 *    A = generation counter, nonzero if valid.
 *    B = timer number (MAX_TIMERS_BITS wide).
 *    C = physical CPU number (MAX_PCPUS_BITS wide).
 */
#define MAX_TIMERS		 512
#define MAX_TIMERS_BITS          9
#define MAX_TIMERS_MASK          ((1 << MAX_TIMERS_BITS) - 1)

/*
 * Number of spokes in timer wheel and width (in CPU cycles) of each spoke.
 *
 * To determine which spoke a timer goes into, we take bits from the middle
 * of its deadline. That is, we can look at the deadline as the 64-bit
 * concatenation DEF of three bit strings:
 *
 *    D = high order bits
 *    E = spoke number (TIMER_NUM_SPOKES_BITS wide)
 *    F = low order bits (TIMER_SPOKE_WIDTH_BITS wide)
 *
 * The number of bits in F determines the *spoke width*, and the
 * number of bits in E determines the *number of spokes*. Ideally, we
 * would like to check about one spoke per timer poll, so the spoke
 * width should be somewhere around the frequency with which we expect
 * to do timer polls. We've initially set this to 2**18 CPU cycles, or
 * about 262 us on a 1 GHz machine. We are currently doing a hard
 * timer interrupt at a fixed period of 1000 us, but we haven't yet
 * measured the frequency with which soft polls end up happening. The
 * number of spokes is a simple space/time tradeoff, as with sizing
 * any hash table. As mentioned above, we'd like the number of spokes
 * to be about the maximum number of timers that are typically
 * outstanding at once. We've initially set this arbitrarily to 2**6 =
 * 64.
 */
#define TIMER_NUM_SPOKES_BITS           6
#define TIMER_NUM_SPOKES                (1 << TIMER_NUM_SPOKES_BITS)
#define TIMER_NUM_SPOKES_MASK           (TIMER_NUM_SPOKES - 1)
#define TIMER_SPOKE_WIDTH_BITS          18
#define TIMER_SPOKE_WIDTH               (1 << TIMER_SPOKE_WIDTH_BITS)
#define TIMER_SPOKE_WIDTH_MASK          (TIMER_SPOKE_WIDTH_TS - 1)

#define SCHED_PERIOD_US                 1000
#define JIFFY_PERIOD_US                 10000
#define STATS_PERIOD_US                 10000
#define PSEUDO_TSC_UPDATE_MS            60000

#define MAX_GROUP_ID_BITS               (64 - MAX_PCPUS_BITS)

#define	TIMER_FAKE_NUMA_DIVISOR		(20)

/*
 * Types
 */

/*
 * A scheduled timer callback.
 */
typedef struct Timer {
   List_Links		links;		// must be first

   Timer_Callback	function;
   uint32		flags;		// one-shot, periodic, etc.
   Timer_AbsCycles	deadlineTC;     // next time to fire
   Timer_RelCycles	periodTC;
   void			*data;		// private client data
   TimerGroupID         groupID;        // Class this timer belongs to(Optional)
   Timer_Handle         handle;
} Timer;

/*
 * Main timer structure.  One per CPU.
 */
typedef struct TimerWheel {
   Timer		timer[MAX_TIMERS];  // timer storage

   SP_SpinLockIRQ	lock;		// for mutual exclusion
   PCPU			pcpu;		// processor number
   List_Links		freeList;	// unallocated timer objects

   List_Links           wheel[TIMER_NUM_SPOKES];  // timer wheel
   Timer_AbsCycles	curTC;          // when wheel was last checked
   uint32               curSpoke;       // spoke last checked
   uint32		periodUS;	// hard interrupt period in us
   uint32		newPeriodUS;	// desired hard interrupt period in us

   /*
    * Stats
    */
   uint64		interruptCount;	// number of interrupts
   uint64		periodSetCount;	// dynamic period adjusts
   uint64		lostBusCycles;	// bus cycles lost when adjust
   uint64		overdueDropped; // overdue periodic callbacks dropped

   /*
    * Special case timer that doesn't wait until bottom halves run.
    */
   Timer_RelCycles      schedPeriodTC;    // size of a scheduler tick in cycles
   Timer_AbsCycles      schedDeadlineTC;  // time for next scheduler tick

   /*
    * Special timer for STATS/KSTATS
    */
   Timer_RelCycles      statsPeriodTC;
   Timer_AbsCycles      statsDeadlineTC;
} TimerWheel;

/*
 * Globals
 */


// Timer group ID
static TimerGroupID nextGroupID = 0;

// Increments every JIFFY_PERIOD_US microseconds
unsigned long jiffies;

// size of a jiffy tick in cycles
Timer_RelCycles jiffyPeriodTC = 0;

// time for next jiffy tick
Timer_AbsCycles jiffyDeadlineTC;  

// offset used by Timer_GetTimeOfDay
Atomic_uint64 timeOfDayOffset;

static TimerWheel localTimerWheel[MAX_PCPUS];
#if !SOFTTIMERS
static uint32 timerBHNum;
#endif

static Proc_Entry timerProcEntry;
static Proc_Entry timerUptimeProcEntry;

// Computed in init.c
uint64 cpuHzEstimate;
uint32 cpuKhzEstimate;
uint64 busHzEstimate = 100000000ull;
uint32 busKhzEstimate = 100000;

// Computed in Timer_InitTSCAdjustment
static RateConv_Params tcToPseudoTSC;
static const RateConv_Params rateConvIdentity = RATE_CONV_IDENTITY;
static Bool moduleInitialized = FALSE;

// offset to make Timer_GetCycles start at 0 as of Timer_InitCycles call
static TSCRelCycles shiftTC = 0;

static uint64 mpmcHzEstimate;
static volatile uint32 mpmcExtension;
uint64 (*Timer_GetCycles)(void);

RateConv_Params timerMSToTC, timerUSToTC, timerNSToTC,
   timerTCToNS, timerTCToUS, timerTCToMS;

/*
 * Macros
 */

#define	PCPU_TIMER_WHEEL(pcpu)		(&localTimerWheel[pcpu])
#define	MY_TIMER_WHEEL			(PCPU_TIMER_WHEEL(MY_PCPU))

// Additional timer flags values
#define TIMER_FREE    0x0200  // timer is on free list
#define TIMER_FIRING  0x0400  // timer is currently firing
#define TIMER_EXPIRED 0x0800  // timer should be freed after firing

/*
 * Local functions
 */

static void TimerInit(TimerWheel *t, PCPU pcpu);
static int TimerProcRead(Proc_Entry *entry, char *buffer, int *length);
static int TimerUptimeProcRead(Proc_Entry *entry, char *buffer, int *length);
static void TimerComputeRateConv(uint64 x0, uint64 xrate,
                                 uint64 y0, uint64 yrate,
                                 RateConv_Params *conv);
static INLINE SP_IRQL TimerLock(TimerWheel *t);
static INLINE void TimerUnlock(TimerWheel *t, SP_IRQL prevIRQL);
static INLINE Bool TimerIsLocked(TimerWheel *t);
static void TimerSetPeriod(TimerWheel *t, uint32 period);
static Bool TimerPOST(void *, int, SP_SpinLock *, SP_Barrier *);
static INLINE uint32 TimerTCToSpoke(Timer_AbsCycles tc);
static INLINE uint32 TimerNextSpoke(uint32 spoke);
static uint64 TimerGetMPMCCycles(void);
static uint64 TimerGetFakeNUMACycles(void);
static void TimerUpdatePseudoTSCConv(void *unused, Timer_AbsCycles timestamp);
static void TimerGuestTimeCB(void *data, Timer_AbsCycles timestamp);

#define GetMPMCCycles32() Summit_GetCycloneCycles32(0)

/*
 *----------------------------------------------------------------------
 *
 * TimerGetMPMCCycles --
 *
 *      Read the fine-grained timer on an IBM NUMA machine.  We use
 *      the performance event counter (MPMC) in node 0's Cyclone or
 *      Twister chip, set to count bus cycles, because the TSC's run
 *      at noticeably different speeds in different nodes.
 *
 *      The MPMC counter is 40 bits wide and counts at 100 or 200 MHz.
 *      At the 200 MHz rate, it will wrap in just over 1.5 hours.  We
 *      need more range than that to use it as a global clock to
 *      replace the TSC.
 *      
 *      We use the following lock-free algorithm to extend the
 *      counter.  (The algorithm is my own invention.  --mann)
 *
 *      First, we use only the low 32 bits of the hardware counter,
 *      not all 40.  We can fetch the low 32 bits atomically with a
 *      single mov instruction, but it would require a more expensive
 *      instruction or instruction sequence to fetch all 40 bits.
 *      Since we're extending the counter in software anyway, we can
 *      afford to ignore the high 8 bits that the hardware gives us.
 *
 *      We keep a 32-bit extension to the counter, with the low-order
 *      bit of the extension being normally a *copy* of b31 of the
 *      hardware counter.  This gives us 63 bits in all.  (It would be
 *      nicer to have a full 64, but at 200 MHz, a 63-bit counter
 *      takes 1461 years to overflow, so let's not worry about that.)
 *      
 *                  +----+-----+-----+
 *      extension = | 62 | ... | 31' |
 *                  +----+-----+-----+
 *                             +-----+----+-----+---+
 *      hardware =             |  31 | 30 | ... | 0 |
 *                             +-----+----+-----+---+
 *      
 *      To read the full counter, you usually just read the two parts
 *      and combine them, but you need to take special action when
 *      bits b31' and b31 differ.  Usually what's needed is to
 *      propagate a carry into the extension word.  We allow any
 *      thread to update the extension.  We don't even care if more
 *      than one thread makes updates at once, because we ensure that
 *      if that happens, everyone is writing the same value.
 *
 *      We ensure that all threads make the same updates by having a
 *      thread update the extension only when it sees b31' != b31 and
 *      b30 == 0.  If a thread sees b31' != b31 and b30 == 1, it has
 *      hit a rare race case where after it read the hardware counter,
 *      b31 rolled over and another thread updated b31' before this
 *      thread read it.  I don't think we can avoid that by reading
 *      the software extension before reading the hardware counter,
 *      because I doubt there are ordering guarantees between the MPMC
 *      counter and normal memory.  Even if we could force ordering
 *      with a read fence, this would cost performance.  In the race
 *      case, using extension-1 as the high-order part will give the
 *      thread that lost the race a consistent reading.
 *
 *      The algorithm works without locks or disabling interrupts.
 *      However, we do need to be nonpreemptible to ensure that there
 *      isn't a pause of many minutes while we're running it.  Also,
 *      whenever b31 changes, the counter must be read before b30
 *      changes.  This is ensured because we read the counter from
 *      every hard timer interrupt.  At the 200 MHz rate, b30 changes
 *      only every 2**30 / 2*10**8 = 5.368 seconds.
 *
 * Results:
 *      63-bit timestamp.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static Timer_AbsCycles
TimerGetMPMCCycles(void)
{
   // We must be nonpreemptible while reading the two parts of the
   // timestamp, but we don't need to lock or disable interrupts.

   Bool preemptible = CpuSched_DisablePreemption();
   uint32 eTemp = mpmcExtension;
   uint32 hTemp = GetMPMCCycles32();
   CpuSched_RestorePreemption(preemptible);

   if (UNLIKELY((eTemp ^ (hTemp >> 31)) & 1)) {
      /* Bits 31' and 31 disagree */
      if (UNLIKELY(hTemp & (1ull<<30))) {
         /* Bit 30 is set: we lost a rare race. */
         --eTemp;
      } else {
         /* Bit 30 is clear: need to carry into the extension */
         mpmcExtension = ++eTemp;
      }
   }

   return (((uint64)eTemp << 31) | hTemp) + shiftTC;
}

/*
 *----------------------------------------------------------------------
 *
 * TimerGetTSCCycles --
 *
 *      Read the timer on a default system.
 *
 * Results:
 *      Returns TSC + cycle shift.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static Timer_AbsCycles
TimerGetTSCCycles(void)
{
   return RDTSC() + shiftTC;
}

/*
 *----------------------------------------------------------------------
 *
 * TimerGetFakeNUMACycles --
 *
 *      Read the fake high-precision timer on a fake NUMA system,
 *	implemented as TSC divided by TIMER_FAKE_NUMA_DIVISOR.
 *
 * Results:
 *      Returns TSC divided by TIMER_FAKE_NUMA_DIVISOR.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static Timer_AbsCycles
TimerGetFakeNUMACycles(void)
{
   return RDTSC() / TIMER_FAKE_NUMA_DIVISOR + shiftTC;
}

/*
 *----------------------------------------------------------------------
 *
 * Timer_InitCycles --
 *
 *      Set up to use either the TSC or the MPMC timer for
 *      Timer_GetCycles.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	Sets the Timer_GetCycles function pointer.
 *
 *----------------------------------------------------------------------
 */
void
Timer_InitCycles(void)
{
   if (NUMA_GetSystemType() == NUMA_SYSTEM_IBMX440 &&
       NUMA_GetNumNodes() > 1) {
      uint32 beginMPMC, endMPMC;
      
      /*
       * Measure the frequency of the MPMC cycle counter.
       */
      HZ_ESTIMATE_BEGIN(4);
      beginMPMC = GetMPMCCycles32();
      HZ_ESTIMATE_DELAY;
      endMPMC = GetMPMCCycles32();
      mpmcHzEstimate = HZ_ESTIMATE_COMPUTE(endMPMC - beginMPMC);
      HZ_ESTIMATE_END;

      Log("measured mpmc speed is %Lu Hz", mpmcHzEstimate);

      mpmcExtension = (GetMPMCCycles32() >> 31) & 1;
      Timer_GetCycles = TimerGetMPMCCycles;
   } else if (NUMA_GetSystemType() == NUMA_SYSTEM_FAKE_NUMA &&
              NUMA_GetNumNodes() > 1) {
      Timer_GetCycles = TimerGetFakeNUMACycles;
      Log("fake numa timer speed is %Lu Hz", Timer_CyclesPerSecond());
   } else {
      Timer_GetCycles = TimerGetTSCCycles;
   }

   /*
    * Make Timer_GetCycles start at 0.
    */
   shiftTC = -Timer_GetCycles();

   /*
    * Warning: conversion factors computed below are precise to only
    * 32 bits.  This is mostly OK, since we don't know the
    * relationship between TS and seconds to more than 32 bits of
    * precision anyway, so bits beyond that aren't really meaningful
    * and we might as well set them all to 0.  But if you convert a
    * number to TS *and back* (or vice versa), only the first 32 bits
    * (counting from the highest order nonzero bit) of the result will
    * equal the initial value.  This is because, if we call the first
    * conversion factor x, you're multiplying by a 32-bit
    * approximation of x and then multiplying by an independent 32-bit
    * approximation of 1/x to go back, not multiplying by a 32-bit
    * approximation of x and then dividing by the same approximation
    * to go back.  If you convert only small numbers, or you convert
    * only in one direction, this problem doesn't arise.
    */

   /* For converting to Timer_Cycles: */
   TimerComputeRateConv(0, 1000, 0, Timer_CyclesPerSecond(), &timerMSToTC);
   TimerComputeRateConv(0, 1000000, 0, Timer_CyclesPerSecond(), &timerUSToTC);
   TimerComputeRateConv(0, 1000*1000*1000, 0, Timer_CyclesPerSecond(), &timerNSToTC);
   /* For converting from Timer_Cycles: */
   TimerComputeRateConv(0, Timer_CyclesPerSecond(), 0, 1000*1000*1000, &timerTCToNS);
   TimerComputeRateConv(0, Timer_CyclesPerSecond(), 0, 1000000, &timerTCToUS);
   TimerComputeRateConv(0, Timer_CyclesPerSecond(), 0, 1000, &timerTCToMS);
}


/*
 *----------------------------------------------------------------------
 *
 * Timer_CorrectForTSCShift --
 *
 *      Update the cycle shift used in calculating the timestamp to
 *      compensate for the TSC being reset.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
void
Timer_CorrectForTSCShift(TSCRelCycles tscShift)
{
   ASSERT(Timer_GetCycles != NULL);
   if (Timer_GetCycles == TimerGetTSCCycles) {
      shiftTC += tscShift;
   } else if (Timer_GetCycles == TimerGetFakeNUMACycles) {
      shiftTC += tscShift / TIMER_FAKE_NUMA_DIVISOR;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * Timer_CyclesPerSecond --
 *
 *      Return the frequency of Timer_Cycles in Hz.  That is, the
 *      value of Timer_GetCycles advances at Timer_CyclesPerSecond
 *      counts per second.
 *
 * Results:
 *      Hz.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
uint64
Timer_CyclesPerSecond(void)
{
   if (Timer_GetCycles == TimerGetMPMCCycles) {
      return mpmcHzEstimate;
   } else if (Timer_GetCycles == TimerGetFakeNUMACycles) {
      return cpuHzEstimate / TIMER_FAKE_NUMA_DIVISOR;
   } else {
      ASSERT(Timer_GetCycles != NULL);
      return cpuHzEstimate;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * TimerLock --
 *
 *      Acquire exclusive access to timer "t".
 *
 * Results:
 *      Lock for "t" is acquired.
 *      Returns the caller's IRQL level.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE SP_IRQL
TimerLock(TimerWheel *t)  // IN/OUT
{
   return(SP_LockIRQ(&t->lock, SP_IRQL_KERNEL));
}

/*
 *----------------------------------------------------------------------
 *
 * TimerUnlock --
 *
 *      Releases exclusive access to timer "t".
 *	Sets the IRQL level to "prevIRQL".
 *
 * Results:
 *      Lock for "t" is released.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
TimerUnlock(TimerWheel *t,     // IN/OUT
            SP_IRQL prevIRQL)  // IN
{
   SP_UnlockIRQ(&t->lock, prevIRQL);
}

/*
 *----------------------------------------------------------------------
 *
 * TimerIsLocked --
 *
 *      Check that timer "t" is locked.
 *
 * Results:
 *      TRUE if locked.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
TimerIsLocked(TimerWheel *t)   // IN
{
   return SP_IsLockedIRQ(&t->lock);
}


/*
 *----------------------------------------------------------------------
 *
 * TimerTCToSpoke --
 *
 *      Find the correct spoke for the given timeout.
 *
 * Results:
 *      Spoke number.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE uint32
TimerTCToSpoke(Timer_AbsCycles tc)  // IN
{
   return (tc >> TIMER_SPOKE_WIDTH_BITS) & TIMER_NUM_SPOKES_MASK;
}


/*
 *----------------------------------------------------------------------
 *
 * TimerNextSpoke --
 *
 *      Step to the next spoke.
 *
 * Results:
 *      (Spoke number + 1) mod (number of spokes).
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE uint32
TimerNextSpoke(uint32 spoke)  // IN
{
   return (spoke + 1) & TIMER_NUM_SPOKES_MASK;
}


/*
 *----------------------------------------------------------------------
 *
 * TimerInit --
 *
 *      Timer initialization.
 *
 * Results:
 *      Initializes timer "t" for processor "pcpu".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
TimerInit(TimerWheel *t,  // OUT
          PCPU pcpu)	  // IN
{
   char nameBuf[32];
   int i;

   // zero everything
   memset(t, 0, sizeof(TimerWheel));

   // initialize lock
   ASSERT(SP_RANK_IRQ_LEAF > SP_RANK_IRQ_MEMTIMER);
   snprintf(nameBuf, sizeof(nameBuf), "Timer.%02u", pcpu);
   SP_InitLockIRQ(nameBuf, &t->lock, SP_RANK_IRQ_LEAF);

   // initialize processor
   t->pcpu = pcpu;

   // initialize free list
   List_Init(&t->freeList);
   for (i = 0; i < MAX_TIMERS; i++) {
      t->timer[i].handle =
         (1ull << (MAX_TIMERS_BITS + MAX_PCPUS_BITS)) +
         (i << MAX_PCPUS_BITS) + pcpu;
      t->timer[i].flags = TIMER_FREE;
      List_Insert(&t->timer[i].links, LIST_ATREAR(&t->freeList));
   }

   // initialize wheel 
   for (i = 0; i < TIMER_NUM_SPOKES; i++) {
      List_Init(&t->wheel[i]);
   }
   t->curTC = Timer_GetCycles();
   t->curSpoke = TimerTCToSpoke(t->curTC);

   // initialize period
   t->periodUS = 0; // conservatively assume unknown, though apic.c sets it
   t->newPeriodUS = CONFIG_OPTION(TIMER_HARD_PERIOD);

   // initialize special-case timers handled at interrupt level

   // scheduler
   t->schedPeriodTC = Timer_USToTC(SCHED_PERIOD_US);
   t->schedDeadlineTC = t->curTC;

   /*
    * some linux drivers poll jiffies to do delays and such, so we
    * need to update it on hard interrupts keep them from hanging
    * if they go into a loop on CPU 0 waiting for jiffies to increase
    */
   if (t->pcpu == 0) {
      jiffyPeriodTC = Timer_USToTC(JIFFY_PERIOD_US);
      jiffyDeadlineTC = t->curTC;
   }

   // stats
   t->statsPeriodTC = Timer_USToTC(STATS_PERIOD_US);
   t->statsDeadlineTC = t->curTC;

   // initialize stats
   t->interruptCount = 0;
   t->periodSetCount = 0;
   t->lostBusCycles = 0;
   t->overdueDropped = 0;
}


/*
 *----------------------------------------------------------------------
 *
 * Timer_Init --
 *
 *      Initialize timer module.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Initializes all global module state, including
 *	per-processor state for each local timer.
 *
 *----------------------------------------------------------------------
 */
void
Timer_Init(void)
{
   PCPU pcpu;

   // note: APIC timer setup handled in "apic.c"

   // sanity check
   ASSERT(MAX_PCPUS <= (1 << MAX_PCPUS_BITS));

   // initialize jiffies
   jiffies = 0;

   // initialize per-processor timers
   for (pcpu = 0; pcpu < MAX_PCPUS; pcpu++) {
      TimerInit(&localTimerWheel[pcpu], pcpu);
   }

#if !SOFTTIMERS
   // register bottom half handler to run callbacks
   timerBHNum = BH_Register(TimerBHHandler, NULL);

   // debugging
   LOG(1, "timerBHNum=%d", timerBHNum);
#endif

   POST_Register("Timer", TimerPOST, NULL);
   moduleInitialized = TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * Timer_LateInit --
 *
 *      Late initialization of timer module.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      proc entry for the timer
 *
 *----------------------------------------------------------------------
 */
void
Timer_LateInit(void)
{
   // register top-level "timers" procfs entry
   Proc_InitEntry(&timerProcEntry);
   timerProcEntry.read = TimerProcRead;
   Proc_Register(&timerProcEntry, "timers", FALSE);

   // register top-level "uptime" procfs entry
   Proc_InitEntry(&timerUptimeProcEntry);
   timerUptimeProcEntry.read = TimerUptimeProcRead;
   Proc_Register(&timerUptimeProcEntry, "uptime", FALSE);
}


/*
 *----------------------------------------------------------------------
 *
 * TimerUSToBusCycles --
 *
 *      Convert time in microseconds to time in bus cycles for the
 *      current CPU.
 *
 * Results:
 *      Time in bus cycles.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE uint32
TimerUSToBusCycles(uint32 us)  // IN
{
   return (uint64) us * myPRDA.busHzEstimate / 1000000;
}


/*
 *----------------------------------------------------------------------
 *
 * TimerSetPeriod --
 *
 *      Reprogram local APIC to interrupt every "periodUS"
 *      microseconds.  Caller must hold timer lock for "t" and must be
 *      running on t->pcpu.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Updates local timer state, changes local APIC timer
 *      interrupt rate.
 *
 *----------------------------------------------------------------------
 */
static void
TimerSetPeriod(TimerWheel *t,    // IN
               uint32 periodUS)  // IN
{
   uint32 cyclesPeriod, cyclesLeft, cyclesPrev;
   
   ASSERT(TimerIsLocked(t));
   ASSERT(t->pcpu == MY_PCPU);

   // configure APIC timer period
   cyclesPeriod = TimerUSToBusCycles(periodUS);
   APIC_SetTimer(cyclesPeriod, &cyclesLeft);

   // Keep track of bus cycles "lost" during adjustment.  If we change
   // periods only right after an interrupt (as a previous
   // implementation did), the loss is typically negligible.  Changing
   // periods at an arbitrary time would lose half the old period on
   // average.  However, because we are just using the the APIC as a
   // source of hard interrupts to make sure we check the wheel often
   // enough, not as our timebase, losing these cycles is not really a
   // problem anyway, so the code to keep this statistic could be
   // removed at some point.
   cyclesPrev = TimerUSToBusCycles(t->periodUS);
   if (cyclesLeft <= cyclesPrev) {
      // accumulate "lost" bus cycles
      t->lostBusCycles += (cyclesPrev - cyclesLeft);
   }
                     
   // update interrupt period
   t->periodUS = periodUS;

   // update stats
   t->periodSetCount++;
}


/*
 *----------------------------------------------------------------------
 *
 * TimerInsert --
 *
 *	Insert timer into the wheel for t.  Timer lock must be held.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      modifies wheel
 *
 *----------------------------------------------------------------------
 */
static void
TimerInsert(TimerWheel *t,  // IN/OUT
            Timer *timer)   // IN/OUT
{
   uint32 spoke;
   List_Links *list;
   List_Links *next;
   
   ASSERT(TimerIsLocked(t));

   if (COMPARE_TS(timer->deadlineTC, >, t->curTC)) {
      spoke = TimerTCToSpoke(timer->deadlineTC);
   } else {
      // already overdue; fire as soon as possible
      spoke = t->curSpoke;
   }

   list = &t->wheel[spoke];
   next = List_First(list); 

   for (;;) {
      if (List_IsAtEnd(list, next) ||
          COMPARE_TS(timer->deadlineTC, <=,
                     ((Timer *) next)->deadlineTC)) {
         List_Insert((List_Links *) timer, LIST_BEFORE(next));
         break;
      }
      next = List_Next(next);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * TimerFree --
 *
 *	Mark the timer's current handle as invalid and add the timer
 *	to the free list for t.  Timer lock must be held.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Modifies free list.
 *
 *----------------------------------------------------------------------
 */
static void
TimerFree(TimerWheel *t,  // IN/OUT
          Timer *timer)   // IN/OUT
{
   ASSERT(TimerIsLocked(t));

   // assign new handle, invalidating the old one
   timer->handle += 1 << (MAX_TIMERS_BITS + MAX_PCPUS_BITS);

   // avoid using the reserved null handle
   if (timer->handle == TIMER_HANDLE_NONE) {
      timer->handle += 1 << (MAX_TIMERS_BITS + MAX_PCPUS_BITS);
   }

   // mark as free
   ASSERT(!(timer->flags & TIMER_FREE));
   timer->flags = TIMER_FREE;
   List_Insert((List_Links *) timer, LIST_ATREAR(&t->freeList));   
}


/*
 *----------------------------------------------------------------------
 *
 * TimerStats --
 *
 *      Called every 10ms to handle counters for STATS and KSTATS gathering
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Increments counters in MY_RUNNING_WORLD->vmkSharedData
 *
 *----------------------------------------------------------------------
 */
static INLINE void
TimerStats(void)
{
   World_Handle *myWorld = MY_RUNNING_WORLD;
   if (myWorld != NULL && myWorld->vmkSharedData != NULL) {
      myWorld->vmkSharedData->statsTicks++;

      if (World_CpuSchedRunState(myWorld) == CPUSCHED_BUSY_WAIT) {
        myWorld->vmkSharedData->statsTotalWaitTicks++;
      } else {
        // this will be decremented by the monitor if the timer interrupt came
        // from the monitor's IDT, and thus the time will not be counted for
        // the vmkernel if the interrupt happened while in the monitor
        myWorld->vmkSharedData->statsTotalBusyTicks++;
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * Timer_Interrupt --
 *
 *      Handle interrupt-time processing.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Updates local timer state, schedules bottom half handler.
 *
 *----------------------------------------------------------------------
 */
void
Timer_Interrupt(void)
{
   TimerWheel *t = MY_TIMER_WHEEL;
   SP_IRQL prevIRQL;
#if !SOFTTIMERS
   uint32 spoke, lastSpoke;
#endif

   prevIRQL = TimerLock(t);

   // update current time, stats
   t->curTC = Timer_GetCycles();
   t->interruptCount++;

#if !SOFTTIMERS
   // set BH if any timers are due to fire
   spoke = t->curSpoke;
   lastSpoke = TimerTCToSpoke(t->curTC);
   for (;;) {
      // check spoke
      if (!List_IsEmpty(&t->wheel[spoke])) {
         Timer *timer = (Timer *) List_First(&t->wheel[spoke]);
         if (COMPARE_TS(t->curTC, >=, timer->deadlineTC)) {
            BH_SetLocalPCPU(timerBHNum);
            break;
         }
      }
      if (spoke == lastSpoke) break;
      spoke = TimerNextSpoke(spoke);
   }
#endif

   // deliver scheduler interrupts
   if (COMPARE_TS(t->curTC, >=, t->schedDeadlineTC)) {
      CpuSched_TimerInterrupt(t->curTC);
      t->schedDeadlineTC += t->schedPeriodTC;
   }

   // update jiffies if we're on CPU 0
   if ((t->pcpu == 0) && (COMPARE_TS(t->curTC, >=, jiffyDeadlineTC))) {
      jiffies++;
      jiffyDeadlineTC += jiffyPeriodTC;
   }

   // do stats stuff
   if (COMPARE_TS(t->curTC, >=, t->statsDeadlineTC)) {
      TimerStats();
      t->statsDeadlineTC += t->statsPeriodTC;
   }
   

   // update hard rate if needed
   if (t->newPeriodUS != t->periodUS) {
      TimerSetPeriod(t, t->newPeriodUS);
   }

   TimerUnlock(t, prevIRQL);
}


/*
 *----------------------------------------------------------------------
 *
 * Timer_BHHandler
 *
 *      Bottom half handling - check and fire timer callbacks
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Perform callbacks for each timer that has reached its timeout.
 *
 *----------------------------------------------------------------------
 */
void
Timer_BHHandler(UNUSED_PARAM(void *ignore))
{
   TimerWheel *t = MY_TIMER_WHEEL;
   SP_IRQL prevIRQL;
   uint32 lastSpoke;
   Timer_AbsCycles curTC;

   // acquire lock
   prevIRQL = TimerLock(t);

   // update current time
   t->curTC = curTC = Timer_GetCycles();

   // loop through spokes
   lastSpoke = TimerTCToSpoke(curTC);
   for (;;) { 

      // loop through timers in spoke
      for (;;) { 
         List_Links *list = &t->wheel[t->curSpoke];
         Timer *timer;
         
         // done with this spoke?
         if (List_IsEmpty(list)) {
            break;
         }
         timer = (Timer *) List_First(list);
         if (COMPARE_TS(curTC, <, timer->deadlineTC)) {
            break;
         }

         // remove/reinsert timer
         List_Remove((List_Links *) timer);
         if (timer->flags & TIMER_PERIODIC) {
            // insert timer in new position
            timer->deadlineTC += timer->periodTC;
            if (UNLIKELY(COMPARE_TS(timer->deadlineTC, <, curTC))) {
               // Next deadline is already in the past.  Punt and
               // reschedule for curTS + period instead.  PR 48501.
               t->overdueDropped++;
               timer->deadlineTC = curTC + timer->periodTC;
            }
            TimerInsert(t, timer);
         } else {
            // mark timer to be freed
            timer->flags |= TIMER_EXPIRED;
         }
         
         // do callback, not holding lock
         timer->flags |= TIMER_FIRING;
         TimerUnlock(t, prevIRQL);
         timer->function(timer->data, curTC);
         prevIRQL = TimerLock(t);
         timer->flags &= ~TIMER_FIRING;

         // free timer if needed
         if (timer->flags & TIMER_EXPIRED) {
            TimerFree(t, timer);
         }
      }

      if (t->curSpoke == lastSpoke) break;
      t->curSpoke = TimerNextSpoke(t->curSpoke);
   }

   // release lock
   TimerUnlock(t, prevIRQL);
}


/*
 *----------------------------------------------------------------------
 *
 * Timer_CreateGroup --
 *
 *      Create a new timer group for pcpu
 *      
 *
 * Results:
 *      Returns new timer group ID
 *
 * Side effects:
 *      nextGroupID is incremented
 *
 *----------------------------------------------------------------------
 */
TimerGroupID
Timer_CreateGroup(PCPU pcpu)
{
   TimerGroupID groupID;
   SP_IRQL prevIRQL;
   TimerWheel *t = PCPU_TIMER_WHEEL(pcpu);

   prevIRQL = TimerLock(t);
   nextGroupID++;
   groupID = nextGroupID;
   TimerUnlock(t, prevIRQL);      
   groupID = groupID | ((TimerGroupID)pcpu << MAX_GROUP_ID_BITS);
   return groupID;
}


/*
 *----------------------------------------------------------------------
 *
 * Timer_RemoveGroup --
 *
 *      Removes all timers belonging to groupID
 *      
 *
 * Results:
 *      None
 *
 * Side effects:
 *      The timer wheel is modified
 *
 *----------------------------------------------------------------------
 */
void
Timer_RemoveGroup(TimerGroupID groupID)
{
   SP_IRQL prevIRQL;
   int i;
   PCPU pcpu = groupID >> MAX_GROUP_ID_BITS;
   TimerWheel *t = PCPU_TIMER_WHEEL(pcpu);

   ASSERT(groupID != DEFAULT_GROUP_ID);
   // default group never gets deleted
   if (groupID == DEFAULT_GROUP_ID) {
      return; 
   }

   prevIRQL = TimerLock(t);
   
   for (i=0; i< TIMER_NUM_SPOKES; i++) {
      List_Links *elt = List_First(&t->wheel[i]);
      while (!List_IsAtEnd(elt, &t->wheel[i])) {
         Timer *timer = (Timer *)elt;
         List_Links *next = List_Next(elt);
         if (timer->groupID == groupID) {
            LOG(1, "removing timer = %p from group\n", timer);
            timer->groupID = 0;
            List_Remove(elt);
            ASSERT(!(timer->flags & TIMER_FREE));
            if (timer->flags & TIMER_FIRING) {
               // not safe to free yet; TimerCheckWheel must handle that
               timer->flags |= TIMER_EXPIRED;
            } else {
               TimerFree(t, timer);
            }
         }
         elt = next;
      }
   }
   TimerUnlock(t, prevIRQL);      
}

/*
 *----------------------------------------------------------------------
 *
 * Timer_AddTC --
 *
 *      Add a new timer with the given parameters.  The deadline and
 *      period are given in Timer_Cycles units.  If periodTC is 0, the
 *      timer is one-shot; otherwise it is periodic.
 *
 * Results:
 *      Handle for the new timer.
 *
 * Side effects:
 *      The timer wheel is modified.
 *
 *----------------------------------------------------------------------
 */
Timer_Handle
Timer_AddTC(PCPU pcpu,                  // IN
            TimerGroupID groupID,       // IN
            Timer_Callback cb,          // IN
            Timer_AbsCycles deadlineTC, // IN
            Timer_RelCycles periodTC,   // IN
            void *data)                 // IN
{
   TimerWheel *t = PCPU_TIMER_WHEEL(pcpu);
   Timer *timer;
   SP_IRQL prevIRQL;
   Timer_Handle handle;

   // check that module is initialized
   ASSERT(jiffyPeriodTC != 0);
   ASSERT(groupID == DEFAULT_GROUP_ID || pcpu == groupID >> MAX_GROUP_ID_BITS);

   // sanity check: a periodic timer's period must be at least 100 us
   if (periodTC) {
      ASSERT(periodTC >= TIMER_MIN_PERIOD);
   }

   prevIRQL = TimerLock(t);

   // allocate timer from free list
   ASSERT_BUG(7132, !List_IsEmpty(&t->freeList));
   timer = (Timer *) List_First(&t->freeList);
   List_Remove(&timer->links);
   ASSERT(timer->flags & TIMER_FREE);
   handle = timer->handle;

   // initialize using specified parameters
   timer->function   = cb;
   timer->deadlineTC = deadlineTC;
   timer->flags      = periodTC ? TIMER_PERIODIC : TIMER_ONE_SHOT;
   timer->data       = data;
   timer->periodTC   = periodTC;
   timer->groupID    = groupID;

   // insert into wheel
   TimerInsert(t, timer);

   // update hard rate if needed and possible
   if (t->pcpu == MY_PCPU && t->newPeriodUS != t->periodUS) {
      TimerSetPeriod(t, t->newPeriodUS);
   }

   TimerUnlock(t, prevIRQL);   

   return handle;
}


/*
 *----------------------------------------------------------------------
 *
 * Timer_Remove --
 *
 *      Remove the timer with the given handle.  Does not wait for the
 *      timer callback to finish if it is already firing.  It is OK
 *      to call this routine from the timer callback itself.
 *
 * Results: 
 *      Returns TRUE if the timer was successfully removed; FALSE
 *      otherwise.  For one-shot timers, TRUE indicates that the timer
 *      has not fired (and now never will), while FALSE indicates that
 *      the timer either was previously removed, has already fired, or
 *      is in the process of firing on another CPU.  For periodic
 *      timers, TRUE indicates that the timer was successfully
 *      removed, while FALSE indicates that the timer was previously
 *      removed; in either case, its final occurrence might still be
 *      in the process of firing on another CPU.
 *
 * Side effects:
 *      The timer wheel and free list associated with the timer are
 *      modified.
 *
 *----------------------------------------------------------------------
 */
Bool
Timer_Remove(Timer_Handle handle) // IN
{   
   SP_IRQL prevIRQL;
   Bool found;
   TimerWheel *t = PCPU_TIMER_WHEEL(handle & MAX_PCPUS_MASK);
   Timer *timer;

   // acquire lock
   prevIRQL = TimerLock(t);

   // look up handle to find timer; ignore expired and freed timers
   timer = &t->timer[(handle >> MAX_PCPUS_BITS) & MAX_TIMERS_MASK];
   found = timer->handle == handle
      && !(timer->flags & (TIMER_FREE|TIMER_EXPIRED));

   // remove timer from wheel
   if (found) {
      List_Remove((List_Links *) timer);
      if (timer->flags & TIMER_FIRING) {
         // not safe to free yet; TimerCheckWheel must handle that
         timer->flags |= TIMER_EXPIRED;
      } else {
         TimerFree(t, timer);
      }
   }
   
   // release lock
   TimerUnlock(t, prevIRQL);   

   return found;
}


/*
 *----------------------------------------------------------------------
 *
 * Timer_RemoveSync --
 *
 *      Remove the timer with the given handle.  Spins waiting for the
 *      timer callback to finish if it is already firing.  (Therefore
 *      this routine must not be called from within the timer callback
 *      itself!)
 *
 * Results: 
 *      Returns TRUE if the timer was successfully removed; FALSE
 *      otherwise.  For one-shot timers, TRUE indicates that the timer
 *      has not fired (and now never will), while FALSE indicates that
 *      the timer either was previously removed or has already fired.
 *      For periodic timers, TRUE indicates that the timer was
 *      successfully removed, while FALSE indicates that the timer was
 *      previously removed.
 *
 * Side effects:
 *      The timer wheel and free list associated with the timer are
 *      modified.
 *
 *----------------------------------------------------------------------
 */
Bool
Timer_RemoveSync(Timer_Handle handle) // IN
{   
   SP_IRQL prevIRQL;
   Bool found;
   TimerWheel *t = PCPU_TIMER_WHEEL(handle & MAX_PCPUS_MASK);
   Timer *timer;

   LOG(2, "invoked");
   // retry loop...
   for (;;) {
      uint32 warn = SPIN_OUT_CYCLES, fail = SPIN_OUTS_BEFORE_PANIC;

      // acquire lock
      prevIRQL = TimerLock(t);

      // look up handle to find timer; include expired but not freed timers
      timer = &t->timer[(handle >> MAX_PCPUS_BITS) & MAX_TIMERS_MASK];
      found = timer->handle == handle
         && !(timer->flags & TIMER_FREE);

      // if not firing, proceed
      if (!(found && (timer->flags & TIMER_FIRING))) {
         break;
      }
         
      // firing; need to spin until it's done
      ASSERT((handle & MAX_PCPUS_MASK) != MY_PCPU); // caller error
      TimerUnlock(t, prevIRQL);
      LOG(1, "timer is firing; spinning...");
      while ((*(volatile uint32*) &timer->flags) & TIMER_FIRING) {
         if (--warn == 0) {
            if (--fail == 0) {
               Panic("Spin count exceeded - possible timer deadlock");
            } else {
               Warning("Spin count exceeded - possible timer deadlock");
               warn = SPIN_OUT_CYCLES;
            }
         }
         PAUSE();
      }
      LOG(1, "...done");
   }

   // timers that have expired are removed by TimerCheckWheel
   found = found && !(timer->flags & TIMER_EXPIRED);

   // remove timer
   if (found) {
      // ASSERT(!(timer->flags & TIMER_FIRING));
      List_Remove((List_Links *) timer);
      TimerFree(t, timer);
   }
   
   // release lock
   TimerUnlock(t, prevIRQL);   

   return found;
}


/*
 *----------------------------------------------------------------------
 *
 * Timer_ModifyTimeoutTC --
 *
 *      Changes the period and deadline of a timer if the timer is
 *      still pending.  The parameters are given in Timer_Cycles
 *      units.  The periodTC parameter is ignored for one-shot timers.
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
Bool
Timer_ModifyTimeoutTC(Timer_Handle handle,        // IN
                      Timer_AbsCycles deadlineTC, // IN
                      Timer_RelCycles periodTC)   // IN
{   
   SP_IRQL prevIRQL;
   Bool found;
   TimerWheel *t = PCPU_TIMER_WHEEL(handle & MAX_PCPUS_MASK);
   Timer *timer;

   // acquire lock
   prevIRQL = TimerLock(t);

   // look up handle to find timer; ignore expired and freed timers
   timer = &t->timer[(handle >> MAX_PCPUS_BITS) & MAX_TIMERS_MASK];
   found = timer->handle == handle
      && !(timer->flags & (TIMER_FREE|TIMER_EXPIRED));

   // remove/reinsert it
   if (found) {
      List_Remove((List_Links *) timer);
      timer->deadlineTC = deadlineTC;
      if (timer->flags & TIMER_PERIODIC) {
         ASSERT(periodTC >= TIMER_MIN_PERIOD);
         timer->periodTC = periodTC;
      }
      TimerInsert(t, timer);
   }
   
   // release lock
   TimerUnlock(t, prevIRQL);   

   return found;
}


/*
 *----------------------------------------------------------------------
 *
 * Timer_GetTimeoutTC --
 *
 *      Gets the deadline and period of a timer.  The values are given
 *      in Timer_Cycles units.
 *
 * Results:
 *      If the timer handle is valid, returns TRUE and sets
 *      *deadlineTC and *periodTC.  Otherwise returns FALSE.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Bool
Timer_GetTimeoutTC(Timer_Handle handle,         // IN
                   Timer_AbsCycles *deadlineTC, // OUT
                   Timer_RelCycles *periodTC)   // OUT
{   
   SP_IRQL prevIRQL;
   Bool found;
   TimerWheel *t = PCPU_TIMER_WHEEL(handle & MAX_PCPUS_MASK);
   Timer *timer;

   // acquire lock
   prevIRQL = TimerLock(t);

   // look up handle to find timer; ignore expired and freed timers
   timer = &t->timer[(handle >> MAX_PCPUS_BITS) & MAX_TIMERS_MASK];
   found = timer->handle == handle
      && !(timer->flags & (TIMER_FREE|TIMER_EXPIRED));

   // extract values
   if (found) {
      *deadlineTC = timer->deadlineTC;
      *periodTC = timer->periodTC;
   }
   
   // release lock
   TimerUnlock(t, prevIRQL);   

   return found;
}


/*
 *----------------------------------------------------------------------
 *
 * Timer_Pending --
 *
 *      Check whether the timer with the specified handle is still
 *      pending.  Note that this is not a stable property, although
 *      the result will never change from FALSE back to TRUE.
 *
 * Results:
 *      Returns TRUE if the timer is still pending; FALSE if not.
 *      FALSE means that either the timer was previously removed, or
 *      it was a one-shot that has already fired or is in the process
 *      of firing.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Bool
Timer_Pending(Timer_Handle handle)  // IN
{   
   SP_IRQL prevIRQL;
   Bool found;
   TimerWheel *t = PCPU_TIMER_WHEEL(handle & MAX_PCPUS_MASK);
   Timer *timer;

   // acquire lock
   prevIRQL = TimerLock(t);

   // look up handle to find timer; ignore expired and freed timers
   timer = &t->timer[(handle >> MAX_PCPUS_BITS) & MAX_TIMERS_MASK];
   found = timer->handle == handle
      && !(timer->flags & (TIMER_FREE|TIMER_EXPIRED));

   // release lock
   TimerUnlock(t, prevIRQL);   

   return found;
}


/*
 *----------------------------------------------------------------------
 *
 * TimerProcRead --
 *
 *      Timer procfs status routine.
 *
 * Results:
 *      Writes human-readable status information about all timers
 *	into "buffer".  Sets "len" to number of bytes written.
 *	Returns 0 iff successful.  
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
TimerProcRead(UNUSED_PARAM(Proc_Entry *entry), char *buffer, int *len)
{
   PCPU pcpu;

   // initialize
   *len = 0;

   // report status for each timer
   for (pcpu = 0; pcpu < numPCPUs;  pcpu++) {
      // convenient abbrev
      TimerWheel *t = PCPU_TIMER_WHEEL(pcpu);
      SP_IRQL prevIRQL;
      uint32 freeCount;
      List_Links *elt;
      Timer *timer;

      // acquire lock
      prevIRQL = TimerLock(t);

      // count free queue entries
      freeCount = 0;
      LIST_FORALL(&t->freeList, elt) {
         freeCount++;
      }

      // format status
      Proc_Printf(buffer, len,
		  "timer.%d:\n"
                  "  %16Lu TC frequency\n"
                  "  %16Lu curTC\n"
                  "  %16u curSpoke\n"
                  "  %16u hardPeriodUS\n"
		  "  %16Lu interruptCount\n"
		  "  %16Lu periodSetCount\n"
		  "  %16Lu lostBusCycles\n"
		  "  %16Lu overdueDropped\n"
		  "  %16u freeSlots\n"
                  "  %16Lu schedPeriodTC\n"
                  "  %16Lu schedDeadlineTC\n"
		  "  %16lu jiffies\n\n",
		  pcpu,
                  Timer_CyclesPerSecond(),
		  t->curTC,
		  t->curSpoke,
		  t->periodUS,
		  t->interruptCount,
		  t->periodSetCount,
		  t->lostBusCycles,
                  t->overdueDropped,
		  freeCount,
                  t->schedPeriodTC,
                  t->schedDeadlineTC,
		  jiffies);

      Proc_Printf(buffer, len, "%16s %12s  %8s  %8s  %8s  %8s\n",
                  "deadlineTS", "periodTS", "periodUS",
                  "function", "data", "flags");
      for (timer = &t->timer[0];
           timer < &t->timer[MAX_TIMERS]; timer++) {

         if (!(timer->flags & TIMER_FREE)) {

            Proc_Printf(buffer, len, "%16Lu %12Ld  %8Ld  %8p  %8p  ",
                        timer->deadlineTC, timer->periodTC,
                        (timer->periodTC * 1000000 +
                         Timer_CyclesPerSecond() / 2) / Timer_CyclesPerSecond(),
                        timer->function, timer->data);

            if (timer->flags & TIMER_ONE_SHOT) {
               Proc_Printf(buffer, len, "one-shot");
            } else {
               ASSERT(timer->flags & TIMER_PERIODIC);
               Proc_Printf(buffer, len, "periodic");
            }
            if (timer->function == TimerGuestTimeCB) {
               Proc_Printf(buffer, len, ", guest %d", (World_ID) timer->data);
            }
            Proc_Printf(buffer, len, "\n");
         }
      }
      Proc_Printf(buffer, len, "\n");      	 

      // release lock
      TimerUnlock(t, prevIRQL);
   }
   // success
   return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * TimerUptimeProcRead --
 *
 *      Timer uptime procfs status routine.
 *
 * Results:
 *      Writes uptime in seconds into buffer.
 *	Sets "len" to number of bytes written.
 *	Returns 0 iff successful.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
TimerUptimeProcRead(UNUSED_PARAM(Proc_Entry *entry), char *buffer, int *len)
{
   uint32 secUptime, msUptime;
   uint64 tmp;
   *len = 0;

   // determine uptime
   tmp = Timer_SysUptime();
   secUptime = tmp / 1000;
   msUptime  = tmp % 1000;

   // format uptime
   Proc_Printf(buffer, len, "%u.%03u\n", secUptime, msUptime);

   // success
   return 0;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Timer_UpdateConfig --
 *
 *     Callback for changes to timer-related config variables.
 *
 * Results:
 *     None
 *
 * Side effects:
 *     Updates global timer configuration
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus 
Timer_UpdateConfig(Bool write, Bool valueChanged, UNUSED_PARAM(int indx))
{
   if (write && valueChanged) {
      PCPU pcpu;
      for (pcpu = 0; pcpu < numPCPUs; pcpu++) {
         TimerWheel *t = PCPU_TIMER_WHEEL(pcpu);
         SP_IRQL prevIRQL;

         prevIRQL = TimerLock(t);
         t->newPeriodUS = CONFIG_OPTION(TIMER_HARD_PERIOD);
         TimerUnlock(t, prevIRQL);
      }
   }
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * TimerGuestTimeCB --
 *
 *      Timer callback that sets an timer update action for the Guest
 *      The data argument to the callback is the worldID.
 *
 * Results:
 *      Posts a timer update action for the guest.
 *
 *----------------------------------------------------------------------
 */
void
TimerGuestTimeCB(void *data, UNUSED_PARAM(Timer_AbsCycles timestamp))
{
   World_Handle *world = World_Find((World_ID)data);
   World_TimerInfo *wti;
   PCPU pcpu;
   SP_IRQL irql;

   if (!world) {
      return;
   }

   wti = &World_VMMGroup(world)->timerInfo;
   irql = SP_LockIRQ(&wti->lock, SP_IRQL_KERNEL);
  
   Action_Post(world, wti->action);

   // If world seems to have changed pcpu, move the timer
   pcpu = world->sched.cpu.vcpu.pcpu;
   if (TIMERMIGRATE && MY_PCPU != pcpu) {
      Bool found;

      LOG(1, "moving guest %d timer from %d to %d",
          (World_ID) data, MY_PCPU, pcpu);
      found = Timer_Remove(wti->handle);
      ASSERT(found);
      wti->handle =
         Timer_AddHiRes(pcpu, TimerGuestTimeCB, wti->interval, 
                        TIMER_PERIODIC, (void *)world->worldID);
   }

   SP_UnlockIRQ(&wti->lock, irql);
   World_Release(world);
}


/*
 *----------------------------------------------------------------------
 *
 * Timer_Info --
 *
 *      Routine to set host timer info for a world group.
 *
 * Results:
 *      The first time, initialize the timer info for the world group.
 *      On subsequent calls, change the timer interval.
 *
 * Side effects:
 *      Change the timer interval.
 *
 *----------------------------------------------------------------------
 */
VMKERNEL_ENTRY
Timer_Info(DECLARE_1_ARG(VMK_TIMER_INFO, uint32 *, info))
{
   SP_IRQL irql;
   World_Handle *world = MY_VMM_GROUP_LEADER; 
   World_TimerInfo *wti = &World_VMMGroup(world)->timerInfo;
   PROCESS_1_ARG(VMK_TIMER_INFO, uint32 *, info);
   PCPU pcpu;
   uint32 newInterval = *info;

   /*
    * Sanity check: make sure a guest that asks for a ridiculously short
    * virtual timer interrupt period can't get the vmkernel to spend
    * all its time calling TimerGuestTimeCB.  Running VPC 2004 in a
    * guest does this; see PR 39462.
    *
    * We can also dial this minimum period higher to reduce the
    * frequency of context switches caused by waking up VMs that need
    * timer interrupts.  Doing this will likely make apparent time run
    * slowly/erratically in VMs that need timer interrupts closer
    * together than the minimum period, however.
    */
   newInterval = MAX(newInterval, CONFIG_OPTION(TIMER_MIN_GUEST_PERIOD));

   if (wti->action == ACTION_INVALID) {
      // first time, initialize the timer info
      wti->action = Action_Alloc(MY_RUNNING_WORLD, "TimerHandler");
      ASSERT(wti->action != ACTION_INVALID);
      wti->interval = newInterval;
      if (TIMERON0) {
         pcpu = 0;
      } else {
         pcpu = world->sched.cpu.vcpu.pcpu;
      }

      wti->handle = Timer_AddHiRes(pcpu, TimerGuestTimeCB, wti->interval, 
                                   TIMER_PERIODIC, (void *)world->worldID);
   } else if (wti->interval != newInterval) {
      // timer interval change request
      Bool found;

      LOG(1, "interval change from %d to %d for world %d",
          wti->interval, newInterval, world->worldID);

      irql = SP_LockIRQ(&wti->lock, SP_IRQL_KERNEL);
      wti->interval = newInterval;
      found = Timer_ModifyTimeoutHiRes(wti->handle, wti->interval);
      SP_UnlockIRQ(&wti->lock, irql);
      ASSERT(found);
   }

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Timer_WorldInit --
 *
 *      Initializes the timerInfo struct in the World_VmmGroupInfo.
 *
 * Results:
 *      VMK_OK
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Timer_WorldInit(World_Handle *world, UNUSED_PARAM(World_InitArgs *args))
{
   if (World_IsVmmLeader(world)) {
      World_TimerInfo *wti = &World_VMMGroup(world)->timerInfo;

      wti->action = ACTION_INVALID;

      // lock rank must be low enough to be able to call Action_Post
      SP_InitLockIRQ("GuestTimerLock", &wti->lock, SP_RANK_IRQ_BLOCK);
   }
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Timer_WorldCleanup --
 *
 *      Clean up guest timer state when a world is destroyed.
 *      Removes the timer callback and frees the action structure.
 *
 * Results:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void 
Timer_WorldCleanup(World_Handle *world)
{
   if (World_IsVmmLeader(world)) {
      Bool found;
      World_TimerInfo *wti = &World_VMMGroup(world)->timerInfo;

      if (wti->action == ACTION_INVALID) {
         /*
          * Timer_Info() was never called, so nothing
          * but the lock to cleanup.
          */
         SP_CleanupLockIRQ(&wti->lock);
         return;
      }

      found = Timer_RemoveSync(wti->handle);
      ASSERT(found);

      /* 
       * Must cleanup lock _after_ we remove the timer.
       * [lock is used in the scheduled timer callback]
       */
      SP_CleanupLockIRQ(&wti->lock);

      wti->action = ACTION_INVALID;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * TimerComputeRateConv --
 *
 *      Compute parameters to convert from xrate to yrate,
 *      with x0 and y0 as the initial point.  That is,
 *
 *      y = y0 + (x - x0) * yrate / xrate
 *        = y0 + ((x - x0) * conv->mult) >> conv->shift 
 *        = conv->add + (x * conv->mult) >> conv->shift.
 *
 * Results:
 *      Returned in *conv.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
TimerComputeRateConv(uint64 x0,             // IN
                     uint64 xrate,          // IN
                     uint64 y0,             // IN
                     uint64 yrate,          // IN
                     RateConv_Params *conv) // OUT
{
   /*
    * This would be simpler if we could do floating-point arithmetic
    * in the kernel.  See RateConv_ComputeParams, which has a much
    * longer comment explaining what it's doing but much shorter code.
    */
   uint64 mult, div;
   uint32 shift;

   if (x0 == y0 && xrate == yrate) {
      *conv = rateConvIdentity;
      return;
   }

   shift = 0;
   mult = yrate;
   ASSERT(mult != 0);
   while ((mult & (1ull << 63)) == 0) {
      mult <<= 1;
      shift++;
   }
   div = xrate;
   while (div >= (1ull << 32)) {
      div >>= 1;
      shift++;
   }
   mult = mult / div;
   while (mult >= (1ull << 32)) {
      mult >>= 1;
      shift--;
   }

   conv->mult = (uint32) mult;
   conv->shift = shift;
   conv->add = y0 - Mul64x3264(x0, conv->mult, conv->shift);
}

/*
 *----------------------------------------------------------------------
 *
 * ApproximatelyEqual --
 *
 *      Check whether two values are approximately equal (to about 1.6%).
 *
 * Results:
 *      True if abs(a - b) < b/64.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static Bool
ApproximatelyEqual(uint64 a, uint64 b)
{
   int64 diff = a - b;

   if (diff < 0) {
      diff = -diff;
   }
   return diff < (b >> 6);
}

/*
 *----------------------------------------------------------------------
 *
 * Timer_InitPseudoTSC --
 *
 *      1) Tweak all the cpuHzEstimate, busHzEstimate, and
 *      mpmcHzEstimate values (both global and in the PRDAs) to make
 *      them consistent.  
 *
 *      2) Compute parameters for converting a real TSC or a
 *      Timer_GetCycles value to a pseudo-TSC.  The pseudo-TSC runs at
 *      the same rate as PCPU 0 on the real machine and is
 *      approximately synchronized across PCPUs.  On SMP (as opposed
 *      to NUMA) machines where all TSCs run at the same rate and are
 *      synced up at vmkernel load time, the conversion is the
 *      identity function.  Set timer callbacks to update the
 *      parameters periodically on machines where this is needed.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Modifies Hz estimates as listed above.
 *      Fills in tcToPseudoTSC and prdas[]->tscToPseudoTSC.
 *
 *----------------------------------------------------------------------
 */
void
Timer_InitPseudoTSC()
{
   PCPU pcpu;
   Bool tweak = TRUE;

   /*
    * Tweak speed estimates to make them consistent, assuming the
    * following hardware properties.  If any of these properties are
    * violated, we log a warning but let the vmkernel try to run on
    * the machine anyway.
    *
    * 1) Within a shared-bus SMP machine or a single NUMA node, the
    * bus clock rate must be the same for all processors.
    *
    * 2) For each processor, the ratio (cpu clock rate) / (bus clock
    * rate) is known as the *clock multiplier*.  The clock multiplier
    * must be of the form n/2 for some small integer n.  (Historically
    * there have been a very few processors with multipliers of the
    * form n/4, but we don't need to support them.)
    *
    * 3) Within an shared-bus SMP machine or a single NUMA node, all
    * processors should have the same clock multiplier.
    *
    * 4) The bus clock rate and clock multiplier should be
    * approximately the same across all nodes of a NUMA machine.
    * I.e., being 1% off should be tolerable, but we may get confused
    * if the drift is greater.
    *
    * Properties 1 and 2 are unlikely to ever be violated.  It's
    * possible to build machines that violate 3, and it's possible to
    * put together IBM x440 configurations that violate 4.  Both Intel
    * and IBM recommend against doing this, and we officially do not
    * support such machines.  We could probably handle both cases by
    * using the pseudoTSC in more places, but this is unexplored.
    */
   // Outer loop through all PCPUs
   pcpu = 0;
   while (pcpu < numPCPUs) {
      PCPU firstPCPUInNode, lastPCPUInNode, i;
      NUMA_Node node;
      uint64 accum;
      uint32 count;
      static char *fraction[] = { ".0", ".5" };

      // Special processing for first PCPU in a node
      firstPCPUInNode = pcpu;
      node = NUMA_PCPU2NodeNum(pcpu);
      accum = count = 0;

      // General processing of all PCPUs in a node
      do {
         // Check that bus speed measurements are consistent (rule 1).
         if (!ApproximatelyEqual(prdas[firstPCPUInNode]->busHzEstimate,
                                 prdas[pcpu]->busHzEstimate)) {
            SysAlert("cpus %u and %u: measured bus speeds conflict",
                    firstPCPUInNode, pcpu);
            ASSERT_BUG(34866, FALSE);
            tweak = FALSE;
         }

         // Determine the clock multiplier and check that it's of the
         // form n/2 (rule 2).  (We add the busHzEstimate/2 to the
         // numerator in order to round rather than truncating.)
         prdas[pcpu]->clockMultiplierX2 = (prdas[pcpu]->cpuHzEstimate * 2 +
                             prdas[pcpu]->busHzEstimate / 2)
            / prdas[pcpu]->busHzEstimate;

         if (!ApproximatelyEqual(prdas[pcpu]->cpuHzEstimate * 2,
                                 prdas[pcpu]->busHzEstimate *
                                 prdas[pcpu]->clockMultiplierX2)) {
            SysAlert("cpu %u: measured cpu and bus speeds conflict", pcpu);
            ASSERT_BUG(34866, FALSE);
            tweak = FALSE;
         }
         Log("cpu %u: measured clock multiplier is %u%s",
             pcpu, prdas[pcpu]->clockMultiplierX2 / 2,
             fraction[prdas[pcpu]->clockMultiplierX2 % 2]);

         // Check that multipliers are the same across the node (rule 3).
         if (prdas[firstPCPUInNode]->clockMultiplierX2
             != prdas[pcpu]->clockMultiplierX2) {
            SysAlert("cpus %u and %u: clock multipliers conflict",
                    firstPCPUInNode, pcpu);
            ASSERT_BUG(34866, FALSE);
         }

         // Average the speed measurements across the node.
         // Weight both bus and cpu measurements equally.
         accum += prdas[pcpu]->busHzEstimate * prdas[pcpu]->clockMultiplierX2 +
            prdas[pcpu]->cpuHzEstimate * 2;
         count += prdas[pcpu]->clockMultiplierX2 * 2;

      } while (++pcpu < numPCPUs && NUMA_PCPU2NodeNum(pcpu) == node);

      // Special processing for last PCPU in a node
      lastPCPUInNode = pcpu - 1;

      // Check that bus speeds are approximately the same across all
      // nodes (rule 4).
      if (!ApproximatelyEqual(prdas[0]->busHzEstimate,
                              prdas[firstPCPUInNode]->busHzEstimate)) {
         SysAlert("nodes 0 and %u: measured bus speeds conflict", node);
         ASSERT_BUG(34866, FALSE);
      }

      if (tweak) {
         // Compute the average bus speed estimate for the node and
         // base the tweaked estimates on that.
         uint64 busHz = accum / count;
         Log("node %u (cpus %u-%u): consensus bus speed is %Lu Hz",
             node, firstPCPUInNode, lastPCPUInNode, busHz);
         
         for (i = firstPCPUInNode; i <= lastPCPUInNode; i++) {
            prdas[i]->busHzEstimate = busHz;
            prdas[i]->cpuHzEstimate = busHz * prdas[i]->clockMultiplierX2 / 2;
            Log("cpu %u: consensus cpu speed is %Lu Hz",
                i, prdas[i]->cpuHzEstimate);
         }
      }
   }

   if (tweak) {
      // Update the global estimates to match the tweaked PCPU 0 estimates
      cpuHzEstimate = prdas[0]->cpuHzEstimate;
      cpuKhzEstimate = (cpuHzEstimate + 500) / 1000;
      busHzEstimate = prdas[0]->busHzEstimate;
      busKhzEstimate = (busHzEstimate + 500) / 1000;
   }

   if (NUMA_GetSystemType() == NUMA_SYSTEM_IBMX440) {
      // Determine the MPMC clock multiplier and check that 
      // it's an integer.
      uint32 multiplier = (mpmcHzEstimate + busHzEstimate / 2) / busHzEstimate;

      Log("measured mpmc clock multiplier is %u", multiplier);

      if (!ApproximatelyEqual(mpmcHzEstimate, busHzEstimate * multiplier)) {
         SysAlert("measured mpmc and bus speeds conflict");
         ASSERT_BUG(34866, FALSE);
      } else {
         // Tweak mpmcHzEstimate to be an exact multiple of the bus clock
         mpmcHzEstimate = busHzEstimate * multiplier;
         Log("consensus mpmc speed is %Lu Hz", mpmcHzEstimate);
      }
   }

   /*
    * Compute the parameters for the unit conversion from
    * Timer_CyclesPerSecond to cpuHzEstimate.
    */
   TimerComputeRateConv(Timer_GetCycles(), Timer_CyclesPerSecond(),
                        RDTSC(), cpuHzEstimate, &tcToPseudoTSC);
   Log("tcToPseudoTSC mult=%#x, shift=%u, add=%#Lx",
       tcToPseudoTSC.mult, tcToPseudoTSC.shift, tcToPseudoTSC.add);

   /*
    * Compute the parameters for conversion from each PCPU's local
    * cpuHzEstimate rate to the global cpuHzEstimate.
    */
   for (pcpu = 0; pcpu < numPCPUs; pcpu++) {
      TimerComputeRateConv(0, prdas[pcpu]->cpuHzEstimate, 0, cpuHzEstimate,
                           &prdas[pcpu]->tscToPseudoTSC);
      Log("tscToPseudoTSC[%u] mult=%#x, shift=%u, add=%#Lx", pcpu,
          prdas[pcpu]->tscToPseudoTSC.mult,
          prdas[pcpu]->tscToPseudoTSC.shift,
          prdas[pcpu]->tscToPseudoTSC.add);

      // we can tolerate a little drift here, so just compute at init time
      TimerComputeRateConv(0, prdas[pcpu]->cpuHzEstimate, 0, 
                           Timer_CyclesPerSecond(), &prdas[pcpu]->tscToTC);
      Log("tscToTS[%u] mult=%#x, shift=%u, add=%#Lx", pcpu,
          prdas[pcpu]->tscToTC.mult,
          prdas[pcpu]->tscToTC.shift,
          prdas[pcpu]->tscToTC.add);

      if (NUMA_PCPU2NodeNum(pcpu) > 0) {
         // Set a timer to update the parameters periodically
         Timer_Add(pcpu, TimerUpdatePseudoTSCConv,
                   PSEUDO_TSC_UPDATE_MS, TIMER_PERIODIC, NULL);
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * Timer_PseudoTSC --
 *
 *      Convert a real TSC value on the current PCPU to a pseudo-TSC
 *      that is approximately consistent across all PCPUs, for
 *      vmkernel internal use.  The pseudo-TSC runs at approximately
 *      the rate of PCPU 0's TSC.  On machines where the hardware TSCs
 *      can get out of sync, Timer_Pseudo TSC is periodically resynced
 *      to Timer_GetCycles, which is a real global timer.
 *
 *      XXXmann No one uses this routine yet.
 *
 * Results:
 *      64-bit timestamp.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
TSCCycles
Timer_PseudoTSC()
{
   Bool preemptible = CpuSched_DisablePreemption();
   TSCCycles tsc = RDTSC();

   if (!RateConv_IsIdentity(&myPRDA.tscToPseudoTSC)) {
      tsc = RateConv_Unsigned(&myPRDA.tscToPseudoTSC, tsc);
   }

   CpuSched_RestorePreemption(preemptible);
   return tsc;
}


/*
 *----------------------------------------------------------------------
 *
 * Timer_UpdateWorldPseudoTSCConv --
 *
 *      Update a world's parameters for converting a real TSC value on
 *      the current PCPU to a pseudo-TSC that is approximately
 *      consistent across all PCPUs, for use by the monitor and the
 *      VMX.  The pseudo-TSC runs at approximately the rate of PCPU
 *      0's TSC.
 *
 *      This routine is called from the scheduler when a VCPU world or
 *      userworld first starts or migrates to a different PCPU, and
 *      periodically from a timer callback.  It preserves consistency
 *      by resynchronizing the pseudo-TSC to Timer_GetCycles().
 *
 *      Reading the pseudo-TSC (PTSC_Get) is implemented in user
 *      space.  There is a critical section in the userspace code
 *      where the TSC and the conversion parameters for the current
 *      PCPU must all be read atomically.
 *   
 *      In the userworld case, we implement this atomicity by having
 *      the vmkernel supply the implementation of PTSC_Get, placing it
 *      in the ktext page.  (See vmkernel/user/pseudoTSC.S.)  If the
 *      userspace EIP is inside the critical section when the
 *      userworld is interrupted, User_InterruptCheck backs it up to
 *      the beginning of the critical section.  This requires the
 *      critical section to be restartable.
 *
 *      In the VMM world case, the monitor implements the atomicity
 *      itself, using its concept of uninterruptible regions.  The
 *      monitor rolls the critical section forward to the end before
 *      passing control to the vmkernel.  (See vmcore/vmm32/platform/
 *      vmkernel/pseudoTSC.S.)
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	Updates parameters as noted above.
 *
 *----------------------------------------------------------------------
 */
void
Timer_UpdateWorldPseudoTSCConv(World_Handle *world, Timer_AbsCycles timestamp)
{
   RateConv_Params *conv;
   RateConv_Params tmpConv;

   if (World_IsVMMWorld(world)) {
      /* Parameters are in the vmkSharedData area */
      conv = &world->vmkSharedData->pseudoTSCConv;
   } else if (World_IsUSERWorld(world)) {
      /* Parameters are in the tdata page */
      conv = &tmpConv;
   } else {
      return;
   }

   if (RateConv_IsIdentity(&myPRDA.tscToPseudoTSC)) {
      *conv = rateConvIdentity;
   } else {
      uint64 pseudoTSC = RateConv_Unsigned(&tcToPseudoTSC, timestamp);
      uint64 realTSC = RDTSC();

      conv->mult = myPRDA.tscToPseudoTSC.mult;
      conv->shift = myPRDA.tscToPseudoTSC.shift;
      conv->add = pseudoTSC - Mul64x3264(realTSC, conv->mult, conv->shift);
   }

   if (conv == &tmpConv) {
      User_UpdatePseudoTSCConv(world, conv);
   }

   LOG(2, "mult=%#x, shift=%u, add=%#Lx", conv->mult, conv->shift, conv->add);
}


/*
 *----------------------------------------------------------------------
 *
 * TimerUpdatePseudoTSCConv --
 *
 *      Update parameters for converting a real TSC value on the
 *      current PCPU to Timer_PseudoTSC, a pseudo-TSC that is
 *      approximately consistent across all PCPUs, for use by the
 *      vmkernel.  The pseudo-TSC runs at approximately the rate of
 *      PCPU 0's TSC.  It is resynchronized to Timer_GetCycles() each
 *      time this routine is called.
 *
 *      This routine is called periodically as a timer callback on
 *      PCPUs that need it.
 *
 * Results:
 *      None.
 *
 * Side effects: 
 *     Updates myPRDA.tscToPseudoTSC.
 * 
 *     We leave the rate constant (mult and shift fields) and change
 *     only the offset (add field).  We could perhaps be more
 *     sophisticated and adjust the mult field too.  This would give
 *     us scope to gradually learn the rate more accurately than the
 *     measurement we made at vmkernel load time or to track changes
 *     in the rate.  It would also allow us to make the value
 *     monotonic within a particular PCPU.  That is, instead of doing
 *     a step forward or back to correct the value, we could keep the
 *     value constant but set the rate a little faster or slower than
 *     we think it ought to be so that we converge on the right value.
 *     It's not clear how worthwhile all that would be, as we can't
 *     prevent there being steps forward or back when a world moves
 *     from one PCPU to another.
 *
 *----------------------------------------------------------------------
 */
void
TimerUpdatePseudoTSCConv(UNUSED_PARAM(void *unused), Timer_AbsCycles timestamp)
{
   uint64 pseudoTSC = RateConv_Unsigned(&tcToPseudoTSC, timestamp);
   uint64 realTSC = RDTSC();
   volatile RateConv_Params *conv = &myPRDA.tscToPseudoTSC;

   conv->add = pseudoTSC - Mul64x3264(realTSC, conv->mult, conv->shift);

   LOG(2, "mult=%#x, shift=%u, add=%#Lx", conv->mult, conv->shift, conv->add);
}


/*
 *----------------------------------------------------------------------
 *
 * TimerPOSTCB --
 *
 *      Timer POST Callback function. 
 *
 * Results:
 *      none
 *
 * Side effects:
 *      updates timerCount
 *
 *----------------------------------------------------------------------
 */
#define POST_CB_DELAY 1000 // usecs
#define MAX_CB_TIME   200  // usecs

static Atomic_uint32 timerCount = {0};
static uint32 lowerBound;
static uint32 upperBound;
static Bool POSTFailed = FALSE;

void
TimerPOSTCB(void *data, UNUSED_PARAM(Timer_AbsCycles timestamp)) 
{
   uint32 start = (uint32)data;
   uint32 end = (uint32)RDTSC();
   uint32 elapsed = end-start;

   if (start != 0 && end > start) {
      if ((elapsed < lowerBound) || (elapsed > upperBound)) {
	 Warning("\tmissed deadline of %u <= %u <= %u\n",
             lowerBound, elapsed, upperBound);
      }
   }
   Atomic_Inc(&timerCount);
}

/*
 *----------------------------------------------------------------------
 *
 * TimerPOST
 *
 *      Perform a power on test of timer callbacks
 *
 * Results:
 *      FALSE if error detected, TRUE otherwise
 *
 * Side effects:
 *      none
 *
 *----------------------------------------------------------------------
 */
Bool
TimerPOST(UNUSED_PARAM(void *clientData),
          int id,
          UNUSED_PARAM(SP_SpinLock *lock),
          SP_Barrier *barrier)
{
   int i, numCallbacks;
   uint64 expire;
   int expire_usecs;
   Bool preemptible;
   Timer_Handle handle;

   if (id == 0) {
      timerCount.value = 0;
   }
   SP_SpinBarrier(barrier);

   // set up a periodic timer callback per cpu
   handle = Timer_Add(MY_PCPU, TimerPOSTCB,
                      POST_CB_DELAY/1000, TIMER_PERIODIC, 0);

   // wait for callbacks to fire
   preemptible = CpuSched_EnablePreemption();

   expire_usecs = 100*POST_CB_DELAY;
   expire = RDTSC() + cpuMhzEstimate*expire_usecs;
   while (RDTSC() < expire) {
      if (timerCount.value >= 10) {
	 break;
      }
   }
   CpuSched_RestorePreemption(preemptible);

   // remove the periodic timer callback
   if (!Timer_Remove(handle)) {
      Warning("\tTimer_Remove failed");
      POSTFailed = TRUE;
   }

   // check if periodic timer callbacks fired
   if (timerCount.value < 10) {
      Warning("\tTime expired before completing all periodic callbacks");
      POSTFailed = TRUE;
   }
   SP_SpinBarrier(barrier);
   if (POSTFailed) {
      return FALSE;
   }

   // set up for one-shot timer callbacks
   numCallbacks = MAX_TIMERS/numPCPUs;
   numCallbacks = numCallbacks/2;
   if (id == 0) {
      timerCount.value = 0;
      lowerBound = POST_CB_DELAY * cpuMhzEstimate;
      upperBound = (lowerBound*10) + (numCallbacks*MAX_CB_TIME*cpuMhzEstimate);
   }
   SP_SpinBarrier(barrier);

   // prime the timer for one-shot callback
   Timer_Add(MY_PCPU, TimerPOSTCB, POST_CB_DELAY/1000, TIMER_ONE_SHOT, 0);
   Util_Udelay(2000);

   // launch the one-shot timer callback requests
   for (i = 0; i < numCallbacks-1; i++) {
      uint32 start = (uint32) RDTSC();
      Timer_Add(MY_PCPU, TimerPOSTCB, POST_CB_DELAY/1000,
		TIMER_ONE_SHOT, (void *)start);
   }

   // wait for callbacks to complete firing, or time to expire
   preemptible = CpuSched_EnablePreemption();

   expire_usecs = 500*POST_CB_DELAY;
   expire = RDTSC() + cpuMhzEstimate*expire_usecs;
   while (RDTSC() < expire) {
      if (timerCount.value == numCallbacks*numPCPUs) {
	 break;
      }
   }
   CpuSched_RestorePreemption(preemptible);

   // check if all the one-shot callbacks fired
   if (timerCount.value != numCallbacks*numPCPUs) {
      Warning("\tTime expired before completing all one-shot callbacks");
      POSTFailed = TRUE;
   }
   SP_SpinBarrier(barrier);

   return (! POSTFailed);
}


/*
 *----------------------------------------------------------------------
 *
 * Timer_SysUptime --
 *
 *      Read the system uptime.
 *
 * Results:
 *      Returns the cumulative system uptime, in milliseconds.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
uint64
Timer_SysUptime(void)
{
   if (Timer_GetCycles != NULL) {
      return Timer_TCToMS(Timer_GetCycles());
   } else {
      return 0;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * Timer_GetTimeOfDay --
 *
 *      Get the time of day in microseconds since 1970.
 *
 * Results:
 *      Time of day.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
int64
Timer_GetTimeOfDay()
{
   int64 offset = Atomic_Read64(&timeOfDayOffset);
   return offset + Timer_TCToUS(Timer_GetCycles());
}


/*
 *----------------------------------------------------------------------
 *
 * Timer_SetTimeOfDay --
 *
 *      Set the time of day in microseconds since 1970.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Updates the timeOfDayOffset.
 *
 *----------------------------------------------------------------------
 */
void
Timer_SetTimeOfDay(int64 tod)
{
   int64 offset = tod - Timer_TCToUS(Timer_GetCycles());
   Atomic_Write64(&timeOfDayOffset, offset);
}

/*
 *----------------------------------------------------------------------
 *
 * Timer_TCToSec --
 *
 *      Converts "tc" in timer cycles to seconds and microseconds
 *
 * Results:
 *      Sets "sec"  to the number of whole seconds in "tc".
 *	Sets "usec" to the number of remaining microseconds in "tc".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
Timer_TCToSec(Timer_Cycles tc, uint64 *sec, uint32 *usec)
{
   uint64 seconds, uSeconds;

   // convert units
   uSeconds = Timer_TCToUS(tc);
   seconds = uSeconds / 1000000;
   uSeconds -= seconds * 1000000;

   // set reply values
   *sec = seconds;
   *usec = (uint32) uSeconds;
}

/*
 *----------------------------------------------------------------------
 *
 * Timer_TSCToSec --
 *              
 *      Convert TSC to seconds and microseconds with ULP rounding.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
void
Timer_TSCToSec(TSCCycles tsc, uint32 *sec, uint32 *usec)
{
   uint64 remainder, rounded, seconds, uSeconds;
   uint64 hzEstimate = cpuKhzEstimate * 1000;

   seconds   = tsc / hzEstimate;
   remainder = tsc - (seconds * hzEstimate);
   rounded   = remainder * 1000 + (cpuKhzEstimate >> 1);
   uSeconds  = rounded / cpuKhzEstimate;

   *sec  = (uint32)seconds;
   *usec = (uint32)uSeconds;
}

/*
 *----------------------------------------------------------------------
 *
 * Timer_HzEstimateInit --
 *
 *      Set up the PIT2 timer for use in estimating the frequency of
 *      other timers.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      As above.
 *
 *----------------------------------------------------------------------
 */
void
Timer_HzEstimateInit(void)
{
   uint8 byte;

   ASSERT(CLICKS_PER_LOOP <= (1<<16));

   /* Enable gate on PIT2 timer, disable speaker output, set timer to
    * square wave mode, and select period.  Counting starts at next
    * click after period is written.  The period register is only 16
    * bits wide, but a period of 0 can be used to mean 2**16. */
   byte = INB(SPEAKER_PORT);
   byte = (byte & ~0x2) | 0x1;
   OUTB(SPEAKER_PORT, byte);
   OUTB(0x43, 0xb6);
   OUTB(0x42, CLICKS_PER_LOOP & 0xff);
   OUTB(0x42, (CLICKS_PER_LOOP >> 8) & 0xff);
}


/*
 *----------------------------------------------------------------------
 *
 * Timer_CPUHzEstimate
 *
 *      Return an estimate of the processor's speed in Hz, based on
 *      the ratio of the cycle counter and the PIT timer.  The
 *      estimate seems to be good to about +/- 200 Hz.
 *
 * Results:
 *      Estimated CPU speed in Hz.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
uint64
Timer_CPUHzEstimate(void)
{
   uint64 beginTSC, endTSC, hz;

   HZ_ESTIMATE_BEGIN(4);
   beginTSC = RDTSC();
   HZ_ESTIMATE_DELAY;
   endTSC = RDTSC();
   hz = HZ_ESTIMATE_COMPUTE(endTSC - beginTSC);
   HZ_ESTIMATE_END;
   return hz;
}


/*
 *----------------------------------------------------------------------
 *
 * Timer_Initialized
 *
 *      Returns TRUE if the timer module has been initialized (ie safe 
 *      to call Timer_GetCycles, Timer_TCTo<foo>), and FALSE 
 *      otherwise.
 *
 *      The timer module has a number of initialization functions,
 *      which are called in the following order (see init.c):
 *
 *      Timer_HzEstimateInit();
 *      Timer_InitCycles();
 *      Timer_Init();            // this one sets Timer_Initialized
 *      Timer_InitPseudoTSC();
 *      Timer_LateInit();
 *
 * Results:
 *      See above.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Bool
Timer_Initialized(void)
{
   return moduleInitialized;
}
