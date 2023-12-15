/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * vmkperf.c
 *
 *	This module manages aggregate performance counters.
 */


/*
 * Includes
 */
#include "vm_types.h"
#include "vm_libc.h"
#include "vm_asm.h"
#include "vm_atomic.h"
#include "x86.h"
#include "vmkernel.h"
#include "memmap.h"
#include "world.h"
#include "sched.h"
#include "host.h"
#include "splock.h"
#include "prda.h"
#include "smp.h"
#include "timer.h"
#include "memalloc.h"
#include "nmi.h"
#include "vmkstats.h"
#include "libc.h"
#include "util.h"
#include "config.h"
#include "trace.h"
#include "parse.h"

#include "vmkperf.h"

#define LOGLEVEL_MODULE Vmkperf
#include "log.h"

/*
 * Constants
 *
 */


#define PENTIUM4_NUM_ESCR_ADDRS (PENTIUM4_MAX_ESCR_ADDR - PENTIUM4_MIN_ESCR_ADDR)

#define MAX_PROC_NAMELEN 80
#define PERFCTR_PROC_DIR "vmkperfctr"

#define NUM_PENTIUM4_EVENTS (sizeof(eventInfoPentium4)/(sizeof(VmkperfEventInfo)))   // number of events we've defined
#define NUM_P6_EVENTS (sizeof(eventInfoP6)/(sizeof(VmkperfP6EventInfo))) 
#define HTONLY(expr) (SMP_HTEnabled() ? expr : 0)


#if (vmx86_devel)
#define VMKPERF_UPDATE_TIMER_DELAY (500) // 2x per second
#else
#define VMKPERF_UPDATE_TIMER_DELAY (0)   // don't use update timer
#endif

// from nmi.c
#define PERFCTR_PENTIUM4_VAL_MASK 	(0x000000ffffffffffLL)

#define MIN_TIMER_TIMEOUT (0)
#define PERFCTR_PENTIUM4_OPT_EDGE_DETECT   (PERFCTR_PENTIUM4_CCCR_COMPARE | PERFCTR_PENTIUM4_CCCR_EDGE) 


/*
 * Types
 */

// approximate rate at which we expect a counter to increment
typedef uint32 VmkperfCounterRate;

// Rate reflects the base-10 log of the approximate cycles needed
// for the counter to advance by a single unit. Thus, total_cycles has
// a rate of "0", because the counter advances 1 unit for every cycle.
// A slower event, like itlb misses might occur 10000 times less often,
// so it has a rate of "4"
#define VMKPERF_EVENT_VERYFAST     (0)
#define VMKPERF_EVENT_FAST         (1)
#define VMKPERF_EVENT_MEDIUM       (2)
#define VMKPERF_EVENT_SLOW         (4)

// Basic description of a hardware counter
typedef struct {
   // an array of counter indices valid for this event, terminated by INVALID_COUNTER_SENTRY:
   const uint32* usableCounters;
   const uint32 escrAddr; 
   const uint32 escrIdx;
   uint32 counterNum; // do not statically define
} CounterDesc;

// VmkperfEventInfo describes an event that we know how to count.
// These events are statically predefined in the array eventInfoPentium4.
typedef struct VmkperfEventInfo {
   char *eventName; // short name to appear in /proc
   CounterDesc ctr[SMP_MAX_CPUS_PER_PACKAGE];
   const uint32 eventSel;    // event selection mask
   const uint32 cccrOptions; // extra options, like edge detect, filter, etc.
   const VmkperfCounterRate rate; // how quickly does this counter usually increase?
   const Bool threadIndep;   // can this event be counter on a per HT-lcpu basis?
   const TraceEventID traceEvent;
   
   // do not statically define the following
   uint32 cpusActive;
   Proc_Entry procEnableEntry;
   Proc_Entry procCounterEntry;
   Proc_Entry procWorldCounterEntry;
} VmkperfEventInfo;

typedef struct {
   char *eventName;
   uint32 counter;
   const VmkperfCounterRate rate;
} VmkperfP6EventInfo;

/* Each world will carry an array of these structures, 
 * one per counter on the CPU model (18 on the P4). 
 * They store per-world counter info. */
struct VmkperfWorldCounterInfo {
   uint64 totalCounter;
   uint64 startCounter;
   uint64 totalTime;
   uint64 startTime;
};

typedef struct {
   VmkperfEventInfo* ctrEvent;
   uint64 countSnapshot;
   uint64 snapshotTime;
   uint64 startTime;
   uint64 deltaCount;
} CpuCounterInfo;



/* 
 * Globals
 */

static SP_SpinLockIRQ vmkperfLock; // the main lock for this whole module
static SP_IRQL vmkperfPrevIRQL;  
static Bool vmkperfRunning = FALSE;

/* Basically a bitfield, where an entry is set to TRUE if the 
 * corresponding counter is in use. Indices are based on the
 * pentium4 counter number constants, e.g. PENTIUM4_MSR_BPU_COUNTER0_IDX */
static Bool usedCounters[PERFCTR_PENTIUM4_NUM_PERFCTRS];
static Timer_Handle timerHandles[MAX_PCPUS];

// bitfield of used ESCRs, indexed by (escrAddr - PENTIUM4_MIN_ESCR_ADDR)
static Bool usedESCRs[PENTIUM4_NUM_ESCR_ADDRS];

/* This "cpu counter info" 2-dimensional array tracks per-cpu
 * info on the counters that are currently running (or stopped).
 * For each counter, on each cpu, there is a CpuCounterInfo struct
 * as defined above. The first index into the 2-d array is the 
 * cpu number (from MY_PCPU), the second index is the 
 * counter number (e.g. PENTIUM4_MSR_BPU_COUNTER0_IDX).
 *
 * Design note: The use of a single array element for each counter
 * makes it really easy to implement and easy to run many counters at
 * once. However, it also consumes extra space (500 bytes per world)
 * and takes a little time to scan through the array. As long as this
 * is only in devel builds, it's fine. Could be changed to a linked
 * list for better overall performance.
 */
static CpuCounterInfo  **cpuCounterInfoP4;

static Proc_Entry vmkperfEnableProc;
static Proc_Entry vmkperfRootProc;
static Proc_Entry vmkperfDebugProc;

// define useful sets of related counters
const uint32 PENTIUM4_COUNTERSET_BPU0[] = { PENTIUM4_MSR_BPU_COUNTER0_IDX, 
                                            PENTIUM4_MSR_BPU_COUNTER1_IDX,
                                            INVALID_COUNTER_SENTRY };
const uint32 PENTIUM4_COUNTERSET_BPU1[] = { PENTIUM4_MSR_BPU_COUNTER3_IDX, 
                                            PENTIUM4_MSR_BPU_COUNTER2_IDX, 
                                            INVALID_COUNTER_SENTRY };
const uint32 PENTIUM4_COUNTERSET_FLAME0[] = { PENTIUM4_MSR_FLAME_COUNTER0_IDX, 
                                              PENTIUM4_MSR_FLAME_COUNTER1_IDX,
                                              INVALID_COUNTER_SENTRY };
const uint32 PENTIUM4_COUNTERSET_FLAME1[] = { PENTIUM4_MSR_FLAME_COUNTER2_IDX, 
                                              PENTIUM4_MSR_FLAME_COUNTER3_IDX,
                                              INVALID_COUNTER_SENTRY };
const uint32 PENTIUM4_COUNTERSET_IQ0[]  = { PENTIUM4_MSR_IQ_COUNTER0_IDX, 
                                            PENTIUM4_MSR_IQ_COUNTER1_IDX, 
                                            PENTIUM4_MSR_IQ_COUNTER4_IDX, 
                                            INVALID_COUNTER_SENTRY };
const uint32 PENTIUM4_COUNTERSET_IQ1[]  = { PENTIUM4_MSR_IQ_COUNTER2_IDX, 
                                            PENTIUM4_MSR_IQ_COUNTER3_IDX, 
                                            PENTIUM4_MSR_IQ_COUNTER5_IDX, 
                                            INVALID_COUNTER_SENTRY };

// Macro magic to define a "counterset pair" consisting of two
// CounterDesc structure initializers, each of which represents a set
// of counters and an ESCR that can be used to count the same event(s)
#define GEN_CSET_PAIR01(cset, escr) { \
   { PENTIUM4_COUNTERSET_##cset##0, \
     PENTIUM4_MSR_##escr##_ESCR0_ADDR , \
     PENTIUM4_MSR_##escr##_ESCR0_IDX }, \
   { PENTIUM4_COUNTERSET_##cset##1, \
     PENTIUM4_MSR_##escr##_ESCR1_ADDR, \
     PENTIUM4_MSR_##escr##_ESCR1_IDX } \
  }

// generate a bunch of useful pairs
#define COUNTERSET_PAIR_IQ_CRU01 GEN_CSET_PAIR01(IQ, CRU)
#define COUNTERSET_PAIR_BPU_BSU01 GEN_CSET_PAIR01(BPU, BSU)
#define COUNTERSET_PAIR_BPU_PMH01 GEN_CSET_PAIR01(BPU, PMH)
#define COUNTERSET_PAIR_BPU_ITLB01 GEN_CSET_PAIR01(BPU, ITLB)
#define COUNTERSET_PAIR_BPU_BPU01 GEN_CSET_PAIR01(BPU, BPU)
#define COUNTERSET_PAIR_FLAME_DAC01 GEN_CSET_PAIR01(FLAME, DAC)
#define COUNTERSET_PAIR_BPU_FSB01 GEN_CSET_PAIR01(BPU, FSB)

// odd man out, requires counter numbers other than 0 and 1
#define COUNTERSET_PAIR_IQ_CRU23 { \
        { PENTIUM4_COUNTERSET_IQ0, \
          PENTIUM4_MSR_CRU_ESCR2_ADDR, \
          PENTIUM4_MSR_CRU_ESCR2_IDX }, \
        { PENTIUM4_COUNTERSET_IQ1, \
          PENTIUM4_MSR_CRU_ESCR3_ADDR, \
          PENTIUM4_MSR_CRU_ESCR3_IDX }, \
       }

/* 
 * Statically define all of the events that we know about for
 * the Pentium 4.
 */
static VmkperfEventInfo eventInfoPentium4[] = {
   { "cycles",
     .ctr = COUNTERSET_PAIR_IQ_CRU01,
     .eventSel = PERFCTR_PENTIUM4_EVT_CLK_CYCLES,
     .cccrOptions = (PERFCTR_PENTIUM4_CCCR_COMPARE
                     |PERFCTR_PENTIUM4_CCCR_COMPLEMENT
                     |PERFCTR_PENTIUM4_CCCR_THRESHOLD(0xf)),
     .rate = VMKPERF_EVENT_VERYFAST },
   { "instr_retired",
     .ctr = COUNTERSET_PAIR_IQ_CRU01,
     .eventSel = PERFCTR_PENTIUM4_EVT_INSTR_RETIRED,
     .cccrOptions = 0,
     .rate = VMKPERF_EVENT_VERYFAST  },
   { "l1miss",
     .ctr = COUNTERSET_PAIR_BPU_BSU01,
     .eventSel = PERFCTR_PENTIUM4_EVT_L1_MISS,
     .cccrOptions = PERFCTR_PENTIUM4_OPT_EDGE_DETECT,
     .rate = VMKPERF_EVENT_FAST },
   { "l2readhit" ,
     .ctr = COUNTERSET_PAIR_BPU_BSU01,
     .eventSel = PERFCTR_PENTIUM4_EVT_L2_READHIT,
     .cccrOptions = PERFCTR_PENTIUM4_OPT_EDGE_DETECT,
     .rate = VMKPERF_EVENT_FAST   },
   { "l2readmiss",
     .ctr = COUNTERSET_PAIR_BPU_BSU01,
     .eventSel = PERFCTR_PENTIUM4_EVT_L2_READMISS,
     .cccrOptions = PERFCTR_PENTIUM4_OPT_EDGE_DETECT,
     .rate = VMKPERF_EVENT_MEDIUM  },
   { "l2miss",
     .ctr = COUNTERSET_PAIR_BPU_BSU01,
     .eventSel = PERFCTR_PENTIUM4_EVT_L2_MISS,
     .cccrOptions = PERFCTR_PENTIUM4_OPT_EDGE_DETECT,
     .rate = VMKPERF_EVENT_MEDIUM  },
   { "itlb_miss",
     .ctr = COUNTERSET_PAIR_BPU_ITLB01,
     .eventSel = PERFCTR_PENTIUM4_EVT_ITLB_MISS,
     .cccrOptions = 0,
     .rate = VMKPERF_EVENT_SLOW  },
   { "dtlb_page_walk", // thread-independent
     .ctr = COUNTERSET_PAIR_BPU_BSU01,
     .eventSel = PERFCTR_PENTIUM4_EVT_DTLB_PAGE_WALK,
     .cccrOptions = 0,
     .rate = VMKPERF_EVENT_SLOW,
     .threadIndep = TRUE },
   { "itlb_page_walk", // thread-independent
     .ctr = COUNTERSET_PAIR_BPU_BSU01,
     .eventSel = PERFCTR_PENTIUM4_EVT_ITLB_PAGE_WALK,
     .cccrOptions = 0,
     .rate = VMKPERF_EVENT_SLOW,
     .threadIndep = TRUE },
   { "tcache_miss",
      .ctr = COUNTERSET_PAIR_BPU_BPU01,
      .eventSel = PERFCTR_PENTIUM4_EVT_TCACHE_MISS,
     .cccrOptions = 0,
     .rate = VMKPERF_EVENT_FAST  },
   { "branch",
     .ctr = COUNTERSET_PAIR_IQ_CRU23,
     .eventSel = PERFCTR_PENTIUM4_EVT_BRANCH,
     .cccrOptions = 0,
     .rate = VMKPERF_EVENT_VERYFAST  },
   { "branch_taken",
     .ctr = COUNTERSET_PAIR_IQ_CRU23,
     .eventSel = PERFCTR_PENTIUM4_EVT_BRANCH_TAKEN,
     .cccrOptions = 0,
     .rate = VMKPERF_EVENT_VERYFAST },
   { "branch_nottaken",
     .ctr = COUNTERSET_PAIR_IQ_CRU23,
     .eventSel = PERFCTR_PENTIUM4_EVT_BRANCH_NOTTAKEN,
     .cccrOptions = 0,
     .rate = VMKPERF_EVENT_VERYFAST  },
   { "branch_pred",
     .ctr = COUNTERSET_PAIR_IQ_CRU23,
     .eventSel = PERFCTR_PENTIUM4_EVT_BRANCH_PRED,
     .cccrOptions = 0,
     .rate = VMKPERF_EVENT_VERYFAST  },
   { "branch_mispred",
     .ctr = COUNTERSET_PAIR_IQ_CRU23,
     .eventSel = PERFCTR_PENTIUM4_EVT_BRANCH_MISPRED,
     .cccrOptions = 0,
     .rate = VMKPERF_EVENT_VERYFAST  },
   { "branch_nottaken_pred",
     .ctr = COUNTERSET_PAIR_IQ_CRU23,
     .eventSel = PERFCTR_PENTIUM4_EVT_BRANCH_NOTTAKEN_PRED,
     .cccrOptions = 0,
     .rate = VMKPERF_EVENT_VERYFAST  },
   { "branch_nottaken_mispred",
     .ctr = COUNTERSET_PAIR_IQ_CRU23,
     .eventSel = PERFCTR_PENTIUM4_EVT_BRANCH_NOTTAKEN_MISPRED,
     .cccrOptions = 0,
     .rate = VMKPERF_EVENT_VERYFAST  },
   { "branch_taken_pred",
     .ctr = COUNTERSET_PAIR_IQ_CRU23,
     .eventSel = PERFCTR_PENTIUM4_EVT_BRANCH_TAKEN_PRED,
     .cccrOptions = 0,
     .rate = VMKPERF_EVENT_VERYFAST  },
   { "branch_taken_mispred",
     .ctr = COUNTERSET_PAIR_IQ_CRU23,
     .eventSel = PERFCTR_PENTIUM4_EVT_BRANCH_TAKEN_MISPRED,
     .cccrOptions = 0,
     .rate = VMKPERF_EVENT_VERYFAST },
   { "machine_clear_any",
     .ctr = COUNTERSET_PAIR_IQ_CRU23,
     .eventSel = PERFCTR_PENTIUM4_EVT_MACHINE_CLEAR_ANY,
     .cccrOptions = PERFCTR_PENTIUM4_OPT_EDGE_DETECT,
     .rate = VMKPERF_EVENT_SLOW },
   { "machine_clear_order",
     .ctr = COUNTERSET_PAIR_IQ_CRU23,
     .eventSel = PERFCTR_PENTIUM4_EVT_MACHINE_CLEAR_ORDER,
     .cccrOptions = PERFCTR_PENTIUM4_OPT_EDGE_DETECT,
     .rate = VMKPERF_EVENT_SLOW },
   { "machine_clear_selfmod",
     .ctr = COUNTERSET_PAIR_IQ_CRU23,
     .eventSel = PERFCTR_PENTIUM4_EVT_MACHINE_CLEAR_SELFMOD,
     .cccrOptions = PERFCTR_PENTIUM4_OPT_EDGE_DETECT,
     .rate = VMKPERF_EVENT_SLOW },
   { "machine_clear_ot",
     .ctr = COUNTERSET_PAIR_IQ_CRU23,
     .eventSel = PERFCTR_PENTIUM4_EVT_MACHINE_CLEAR_OT,
     .cccrOptions = PERFCTR_PENTIUM4_OPT_EDGE_DETECT,
     .rate = VMKPERF_EVENT_SLOW },
   { "64k_alias",
     .ctr = COUNTERSET_PAIR_FLAME_DAC01 ,
     .eventSel = PERFCTR_PENTIUM4_EVT_MEMORY_CANCEL_64K,
     .cccrOptions = PERFCTR_PENTIUM4_OPT_EDGE_DETECT,
     .rate = VMKPERF_EVENT_SLOW },
   { "unhalted_cycles",
     .ctr = COUNTERSET_PAIR_BPU_FSB01,
     .eventSel = (PENTIUM4_EVTSEL(0x13) | PENTIUM4_EVTMASK_BIT(0)),
     .cccrOptions = 0, /* level-triggered */
     .rate = VMKPERF_EVENT_VERYFAST }
};

static VmkperfP6EventInfo eventInfoP6[] = {
   { "cycles",	PERFCTR_P6_CPU_CLK_UNHALTED, VMKPERF_EVENT_VERYFAST},
   { "instret",	PERFCTR_P6_INST_RETIRED, VMKPERF_EVENT_VERYFAST},
   { "i1miss",	PERFCTR_P6_L2_IFETCH, VMKPERF_EVENT_FAST},
   { "l1miss",	PERFCTR_PENTIUM4_EVT_L1_MISS, VMKPERF_EVENT_FAST},
   { "l2miss",	PERFCTR_P6_L2_LINES_IN, VMKPERF_EVENT_FAST},
   { "dmissout",PERFCTR_P6_DCU_MISS_OUTSTANDING, VMKPERF_EVENT_FAST},
   { "iftchstl",PERFCTR_P6_IFU_MEM_STALL, VMKPERF_EVENT_FAST},
   { "malign",	PERFCTR_P6_MISALIGN_MEM_REF, VMKPERF_EVENT_MEDIUM},
   { "breqout",	PERFCTR_P6_BUS_REQ_OUTSTANDING, VMKPERF_EVENT_MEDIUM},
   { "blckany",	PERFCTR_P6_BUS_LOCK_CLOCKS_ANY, VMKPERF_EVENT_SLOW},
   { "itlb",	PERFCTR_P6_ITLB_MISS, VMKPERF_EVENT_SLOW},
};

#define LIST_ITEM_TYPE VmkperfEventInfo*
#define LIST_NAME Vmkperf_EventList
#define LIST_SIZE NUM_PENTIUM4_EVENTS
#include "staticlist.h"

static Vmkperf_EventList activeEvents[MAX_PCPUS];

/* 
 * Function prototypes 
 */
static void VmkperfEnablePentium4(void* data, Timer_AbsCycles timestamp);
static void VmkperfDisableCtrPentium4(void* data, Timer_AbsCycles timestamp);
static void VmkperfSetupCounter(VmkperfEventInfo* eventInfo, uint64 escrVal, uint64 cccrVal);
static int  VmkperfProcWorldsReadCounters(Proc_Entry* entry, char *page, int *lenp);
static void VmkperfResetWorldCounters(uint32 counterNum);

// simple exponentiation function
static uint32
POW(uint32 base, uint8 power)
{
   int i;
   uint32 res = 1;
   for (i=0; i < power; i++) {
      res *= base;
   }
   return res;
}

static INLINE void
VmkperfLock(void) 
{
   vmkperfPrevIRQL = SP_LockIRQ(&vmkperfLock, SP_IRQL_KERNEL);
}

static INLINE void
VmkperfUnlock(void) 
{
   SP_UnlockIRQ(&vmkperfLock, vmkperfPrevIRQL);
}

/*
 *-----------------------------------------------------------------------------
 *
 * VmkperfSetESCRLocked --
 *
 *     Marks the ESCR at "escrAddr" locked or unlocked, according to "newState"
 *
 * Results:
 *     None.
 *
 * Side effects:
 *     Marks the ESCR at "escrAddr" locked or unlocked, according to "newState"
 *
 *-----------------------------------------------------------------------------
 */
static INLINE void
VmkperfSetESCRLocked(uint32 escrAddr, Bool newState)
{
   uint32 escrIndex = escrAddr - PENTIUM4_MIN_ESCR_ADDR;
   if (newState == TRUE) {
      ASSERT(!usedESCRs[escrIndex]);
   }
   usedESCRs[escrIndex] = newState;
}

/*
 *-----------------------------------------------------------------------------
 *
 * VmkperfESCRValid --
 *
 *     Returns TRUE iff this is a "known" ESCR, that is, 
 *     one which is used by an event that we understand.
 *
 * Results:
 *     Returns TRUE iff this is a "known" ESCR
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE Bool
VmkperfESCRValid(uint32 thisAddr)
{
   int i;
   for (i=0; i < NUM_PENTIUM4_EVENTS; i++) {
      if (eventInfoPentium4[i].ctr[0].escrAddr == thisAddr
         || eventInfoPentium4[i].ctr[1].escrAddr == thisAddr) {
         return (TRUE);
      }
   }

   // didn't find any possible consumers of this escrAddr
   return (FALSE);
}

/*
 *-----------------------------------------------------------------------------
 *
 * VmkperfESCRUsed --
 *
 *     Returns TRUE iff "escrAddr" is currently being used by a counter
 *
 * Results:
 *     Returns TRUE iff "escrAddr" is currently being used by a counter
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE Bool
VmkperfESCRUsed(uint32 escrAddr)
{
   return (usedESCRs[escrAddr - PENTIUM4_MIN_ESCR_ADDR]);
}


/*
 *-----------------------------------------------------------------------------
 *
 * Vmkperf_FindCounter --
 *
 *  Find a free performance counter from the provided "usableCounters" array, 
 *  marking it "used" before returning. 
 *  "usableCounters" should end with an INVALID_COUNTER_SENTRY.
 *
 * Results:
 *  Returns the index of the counter that was found and claimed. If no valid
 *  counter could be found, it returns INVALID_COUNTER_SENTRY..
 *
 * Side effects:
 *  Marks the counter as "used" before returning it (in usedCounters)
 *
 *-----------------------------------------------------------------------------
 */
uint32 
Vmkperf_FindCounter(const uint32 *usableCounters)
{
   int i;

   ASSERT(cpuType == CPUTYPE_INTEL_PENTIUM4);
   ASSERT(SP_IsLockedIRQ(&vmkperfLock));
   
   // otherwise loop through the array of usable counters, trying to find a free one
   for (i=0; usableCounters[i] != INVALID_COUNTER_SENTRY; i++) {
      uint32 counterNum = usableCounters[i];
      
      LOG(2, "Trying counter %d", counterNum);

      if (usedCounters[counterNum] == FALSE) {
         // this counter is free, so use it
         usedCounters[counterNum] = TRUE;         
         LOG(1, "Returning counter %d", counterNum);
         break;
      }
   }

   return usableCounters[i];
}

/*
 *-----------------------------------------------------------------------------
 *
 * VmkperfSaveCtr --
 *
 *  Internal utility function: read counter "counterNum" from hardware
 *  and save it to the appropriate slot in this CPU's stores counter data.
 *
 * Results:
 *     Returns void
 *
 * Side effects:
 *     Updates counter data for MY_PCPU
 *
 *-----------------------------------------------------------------------------
 */
static void
VmkperfSaveCtr(uint32 counterNum)
{
   uint64 val, timenow, prevVal, deltaCount, prevTime;
   
   val = RDPMC(counterNum) & PERFCTR_PENTIUM4_VAL_MASK;
   timenow = RDTSC();

   prevVal = cpuCounterInfoP4[MY_PCPU][counterNum].countSnapshot;
   cpuCounterInfoP4[MY_PCPU][counterNum].countSnapshot = val;
   prevTime = cpuCounterInfoP4[MY_PCPU][counterNum].snapshotTime;
   cpuCounterInfoP4[MY_PCPU][counterNum].snapshotTime = timenow;

   if (TRACE_MODULE_ACTIVE) {
      // insert event into trace buffer too
      uint32 ctag = (uint32)cpuCounterInfoP4[MY_PCPU][counterNum].ctrEvent;
      // note that we use the START time of this recording interval
      // as the timestamp for the trace event so that the bar
      // covers the right area in the perfviz gui
      deltaCount = val - prevVal;
      Trace_EventWithTimestamp(
         TRACE_VMKPERF_SAMPLE,
         MY_RUNNING_WORLD->worldID,
         MY_PCPU,
         ctag,
         deltaCount,
         prevTime
         );
      
      cpuCounterInfoP4[MY_PCPU][counterNum].deltaCount = deltaCount;
   }
      
   LOG(1, "saved counter %u on pcpu %u has val %Lu", counterNum, MY_PCPU, val);
}

/*
 *-----------------------------------------------------------------------------
 *
 * VmkperfDoCtrRead
 *
 *      Used as a timer callback to read and store the per-cpu performance
 *      counters.
 *
 * Results:
 *      Void return.
 *
 * Side effects:
 *	Stores current time and counter snapshot into cpuCounterInfoP4 entry
 *
 *-----------------------------------------------------------------------------
 */
static void
VmkperfDoCtrRead(void* data, UNUSED_PARAM(Timer_AbsCycles timestamp)) 
{
   uint32 counterNum = (uint32) data;

   LOG(1, "Execute VmkperfPerfCtrRead timer handler on CPU %d", MY_PCPU);

   VmkperfLock();
   VmkperfSaveCtr(counterNum);
   VmkperfUnlock();  
}


/*
 *-----------------------------------------------------------------------------
 *
 * VmkperfPcpuSnapshotAllCounters --
 *
 *  Used as a timer callback. Saves all running (global) counters on 
 *  this PCPU.
 *
 * Results:
 *     Returns 
 *
 * Side effects:
 *     Updates saved global counter values
 *
 *-----------------------------------------------------------------------------
 */
static void
VmkperfPcpuSnapshotAllCounters(void* data, UNUSED_PARAM(Timer_AbsCycles timestamp))
{
   int i;
   uint8 threadNum;
   PCPU myPcpu;

   if (!vmkperfRunning) return;

   VmkperfLock();
   
   myPcpu = MY_PCPU;
   threadNum = SMP_GetHTThreadNum(myPcpu);

   for (i=0; i < activeEvents[myPcpu].len; i++) {
      VmkperfEventInfo* event;

      event = activeEvents[myPcpu].list[i];
      if (event->ctr[threadNum].counterNum == INVALID_COUNTER_SENTRY) {
         continue;
      }
      VmkperfSaveCtr(event->ctr[threadNum].counterNum);
   }

   Vmkperf_WorldSave(MY_RUNNING_WORLD);
   VmkperfUnlock();
}

/*
 *-----------------------------------------------------------------------------
 *
 * VmkperfProcReadCounter --
 *
 *  Used as a read handler by the proc system. Outputs the most recent
 *  performance counter snapshot from this set of cpu perfcounters.
 *  (see docs to read about snapshots, etc.).
 *
 * Results:
 *  Returns VMK_OK
 *
 * Side effects:
 *  Performance counter data written to buffer in "page", "lenp" set to 
 *  length of data written to page.
 *
 *-----------------------------------------------------------------------------
 */
static int
VmkperfProcReadCounter(Proc_Entry* entry,
		       char *page,
		       int *lenp) 
{
   int cpu;
  
   VmkperfEventInfo* event = (VmkperfEventInfo*) entry->private;

   // XXX: Should lock on per-cpu basis instead
   VmkperfLock();
  
   *lenp = 0;

   // write the snapshotted results from each cpu to output page
   for (cpu=0; cpu < numPCPUs; cpu++) {
      uint64 val, cycleDiff, avgPerMillion;
      uint32 counterNum;
      CounterDesc *ctr = &event->ctr[SMP_GetHTThreadNum(cpu)];

      // print the data from the correct hypertwin
      counterNum = ctr->counterNum;
      
      val = cpuCounterInfoP4[cpu][counterNum].countSnapshot;
      cycleDiff = (cpuCounterInfoP4[cpu][counterNum].snapshotTime
                   - cpuCounterInfoP4[cpu][counterNum].startTime) / 1000000ul;

      if (cycleDiff == 0) {
         avgPerMillion = 0;
      } else {
         avgPerMillion = val / cycleDiff;
      }

      Proc_Printf(page, lenp,
                       "Cpu %d:\t%12Lu\t\t%Lu per million cycles avg\n",
                       cpu,
                       val,
                       avgPerMillion );
   }
    
   VmkperfUnlock();

   return VMK_OK;
}


/*
 *-----------------------------------------------------------------------------
 *
 * VmkperfProcSnapshotCounter --
 *
 *  Sets up timers to tell each CPU to read and store its performance counters
 *  for the performance event of interest (the one stored in entry->private).
 *  The timers fire more-or-less immediately (timeout of 1), and after they're done
 *  you can read the results from the proc node.
 *
 * Results:
 *  Returns VMK_OK
 *
 * Side effects:
 *  Timers set to run VmkperfDoCtrRead as soon as possible
 *
 *-----------------------------------------------------------------------------
 */
static int 
VmkperfProcSnapshotCounter(Proc_Entry* entry,
			   char *page,
			   int *lenp) 
{
   int i;
   VmkperfEventInfo* event = (VmkperfEventInfo*) entry->private;

   VmkperfLock(); 

   ASSERT(event != NULL);
   LOG(1, "Snapshotting proc data");

   for (i=0; i < numPCPUs; i++) {
      uint8 threadNum = SMP_GetHTThreadNum(i);

      LOG(2, "Adding timer for pcpu %d", i);

      ASSERT(event->ctr[threadNum].counterNum != INVALID_COUNTER_SENTRY);
    
      Timer_Add(i,
                VmkperfDoCtrRead,
                MIN_TIMER_TIMEOUT,
                TIMER_ONE_SHOT,
                (void*) event->ctr[threadNum].counterNum);
   }
  
   VmkperfUnlock();
  
   return VMK_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 * VmkperfReset --
 *
 *      Resets all state for the counter corresponding to "event"
 *
 * Results:
 *      Returns VMK_OK on success, or return code if counter could not be reset.
 *
 * Side effects:
 *      Resets counter stats.
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
VmkperfReset(VmkperfEventInfo *event)
{
   unsigned i;
   VMK_ReturnStatus ret = VMK_OK;
   
   VmkperfLock();
   // reset global counters
   for (i=0; i < numPCPUs; i++) {
      
      uint32 counterNum = event->ctr[SMP_GetHTThreadNum(i)].counterNum;
      CpuCounterInfo *info;
      
      if (counterNum == INVALID_COUNTER_SENTRY) {
         ret = VMK_NOT_FOUND;
         LOG(1, "counter not enabled on pcpu %u", i);
         continue;
      }
      info = &cpuCounterInfoP4[i][counterNum];
      
      info->countSnapshot = 0;
      info->snapshotTime = 0;
      info->startTime = 0;
      info->deltaCount = 0;
   }
   VmkperfUnlock();
   
   // reset per world counters (acquires lock internally)
   for (i=0; i < SMP_LogicalCPUPerPackage(); i++) {
      uint32 counterNum = event->ctr[SMP_GetHTThreadNum(i)].counterNum;
      VmkperfResetWorldCounters(counterNum);
   }

   return (ret);
}

/*
 *-----------------------------------------------------------------------------
 *
 * Vmkperf_SetEventActive --
 *
 *      Activates the counters for "event" if "active" is TRUE, or
 *      deactivates them if "active" is FALSE.
 *
 * Results:
 *      Returns VMK_OK on success, VMK_NO_RESOURCES if counter unavailable
 *
 * Side effects:
 *      Activates or deactivates specified counter.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
Vmkperf_SetEventActive(VmkperfEventInfo *event, Bool active)
{
   uint8 i;
   PCPU cpu;
   Timer_Callback callback;

   VmkperfLock();

   if (active) {
      if (VmkperfESCRUsed(event->ctr[0].escrAddr)
          || (SMP_HTEnabled() 
              && VmkperfESCRUsed(event->ctr[1].escrAddr))) {
         // can't find a valid ESCR for this 
         VmkperfUnlock();
         return (VMK_NO_RESOURCES);
      }

      // claim one counter for each hyperthread
      for (i=0; i < SMP_LogicalCPUPerPackage(); i++) {
         uint32 counterNum;
         CounterDesc *ctr = &event->ctr[i];
         
         counterNum = Vmkperf_FindCounter(ctr->usableCounters);
         ctr->counterNum = counterNum;
         // if we couldn't find a valid counter, bail out
         if (counterNum == INVALID_COUNTER_SENTRY) {
            Log("Unable to find free counter for %s", event->eventName);
            VmkperfUnlock();
            return (VMK_NO_RESOURCES);
         } else {
            // we just claimed a counter, now claim an ESCR to go with it
            ASSERT(!VmkperfESCRUsed(ctr->escrAddr));
            VmkperfSetESCRLocked(ctr->escrAddr, TRUE);
         }
      }

      callback = VmkperfEnablePentium4;
   } else {      
      callback = VmkperfDisableCtrPentium4;
   }

   VmkperfUnlock();

   for (cpu = 0; cpu < numPCPUs; cpu++) {
      Timer_Add(cpu, callback,
                MIN_TIMER_TIMEOUT, TIMER_ONE_SHOT, event);
      LOG(2, "Added callback for cpu %d", cpu);
   }

   return (VMK_OK);
}

/*
 *-----------------------------------------------------------------------------
 *
 * VmkperfProcEnableCounter --
 *
 *  Called by proc system to disable/enable the counter specified in entry->private.
 *  If function receives "1" as input, it starts or resets the counter.
 *  If it receives "0" as input, it disables the counter.
 *
 * Results:
 *  Returns VMK_OK on success, VMK_BAD_PARAM otherwise
 *
 * Side effects:
 *  Counter started or stopped as appropriate.
 *  Proc nodes for counter appear or disappear, as appropriate.
 *
 *-----------------------------------------------------------------------------
 */
static int
VmkperfProcEnableCounter(Proc_Entry* entry,
                         char* page,
                         int* lenp) 
{
   VmkperfEventInfo* event;
   char *argv[1];
   int argc;

   argc = Parse_Args(page, argv, 1);

   if (argc < 1) {
      Warning("invalid command");
      return (VMK_BAD_PARAM);
   }
   
   LOG(1, "command: %s", argv[0]);

   event = (VmkperfEventInfo*) entry->private;

   if (!strcmp(argv[0], "start")) {
      return Vmkperf_SetEventActive(event, TRUE);
   } else if (!strcmp(argv[0], "stop")) {
      return Vmkperf_SetEventActive(event, FALSE);
   } else if (!strcmp(argv[0], "reset")) {
      return VmkperfReset(event);
   } else {
      // Other, unknown command received
      Log("Unknown command: %s", argv[0]);
      return (VMK_BAD_PARAM);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * VmkperfDisableCtrPentium4 --
 *
 *  Used as a timer callback. Deactivates the performance counter
 *  specified in "data" (as a VmkperfEventInfo*) and removes the
 *  associated proc nodes, if they still exist.
 *
 *
 * Results:
 *  Returns void.
 *
 * Side effects:
 *  Sets specified performance counter to 0 and deactivates it.
 *  May remove proc nodes for the counter.
 *
 *-----------------------------------------------------------------------------
 */
static void
VmkperfDisableCtrPentium4(void* data, UNUSED_PARAM(Timer_AbsCycles timestamp))
{
   VmkperfEventInfo* eventInfo;
   CpuCounterInfo* ctInfo;
   uint8 threadNum;
   uint32 counterNum;
   Bool procRemove = FALSE;
  
   VmkperfLock();

   eventInfo = (VmkperfEventInfo*) data;

   if (eventInfo->cpusActive == 0) {
      LOG(0, "Tried to disable inactive counter.");
      VmkperfUnlock();
      return;
   }

   threadNum = SMP_GetHTThreadNum(MY_PCPU);
   counterNum = eventInfo->ctr[threadNum].counterNum;

   if (counterNum == INVALID_COUNTER_SENTRY) {
      LOG(0, "Tried to disable invalid counter.");
      VmkperfUnlock();
      return;
   }

   ctInfo = &cpuCounterInfoP4[MY_PCPU][counterNum];   
   if (ctInfo->ctrEvent == NULL) {
      LOG(0, "Counter to disable (%x) not running!", counterNum);
      VmkperfUnlock();
      return;  
   }

   ASSERT(ctInfo->ctrEvent == eventInfo);
   LOG(1, "disabling p4 counter");
  
   VmkperfSetupCounter(eventInfo, 0, 0);


   ASSERT(counterNum != INVALID_COUNTER_SENTRY);

   // if we're the first cpu to get the callback to disable the counter
   // go ahead and remove the proc entry
   if ((eventInfo->cpusActive)-- == numPCPUs) {
      procRemove = TRUE;
   }

   if (eventInfo->cpusActive == 0) {
      // if this event is done, we can declare the counter to be free at last
      LOG(2, "Setting counter %d to FREE", counterNum);

      // note that we're done with counters for both hypertwins now
      usedCounters[eventInfo->ctr[0].counterNum] = FALSE;
      usedCounters[eventInfo->ctr[1].counterNum] = FALSE;
      VmkperfSetESCRLocked(eventInfo->ctr[0].escrAddr, FALSE);
      VmkperfSetESCRLocked(eventInfo->ctr[1].escrAddr, FALSE);
      eventInfo->ctr[0].counterNum = INVALID_COUNTER_SENTRY;
      eventInfo->ctr[1].counterNum = INVALID_COUNTER_SENTRY;
   }

   ctInfo->ctrEvent = NULL;
   ctInfo->countSnapshot = 0;
   ctInfo->startTime = 0;

   // this event is no longer officially active
   Vmkperf_EventListRemoveByData(&activeEvents[MY_PCPU], eventInfo);

   VmkperfUnlock();

   // must do this after dropping the vmkperf lock
   if (procRemove) {
      Proc_Remove(&eventInfo->procCounterEntry);
      Proc_Remove(&eventInfo->procWorldCounterEntry);
   }
}



/*
 *-----------------------------------------------------------------------------
 *
 * VmkperfSetupCounter --
 *
 *  Sets a counter (in the eventInfo struct) to 0 and starts it counting,
 *  according to the escrVal and cccrVal specified. If these are set to 0,
 *  the counter stops counting.
 *
 * Results:
 *  Returns void.
 *
 * Side effects:
 *  Changes the model-specific ESCR and CCCR registers and sets the counter to 0.
 *
 *-----------------------------------------------------------------------------
 */
static void VmkperfSetupCounter(VmkperfEventInfo* eventInfo, uint64 escrVal, uint64 cccrVal)
{
   uint32 ctr_addr;
   uint32 cccr_addr;
   uint8 threadNum;
   CounterDesc *ctr;

   ASSERT_NO_INTERRUPTS();
   
   threadNum = SMP_GetHTThreadNum(MY_PCPU);
   ctr = &eventInfo->ctr[threadNum];

   cccr_addr = PERFCTR_PENTIUM4_CCCR_BASE_ADDR + ctr->counterNum;
   ctr_addr = ctr->counterNum
      + PERFCTR_PENTIUM4_COUNTER_BASEADDR;
  

   // first, select no event and set the counter to zero
   __SET_MSR(ctr->escrAddr, 0ul);
   __SET_MSR(cccr_addr, 0ul);
   __SET_MSR(ctr_addr, 0ul);
  
   // now activate the event for real
   __SET_MSR(ctr->escrAddr, escrVal);
   __SET_MSR(cccr_addr, cccrVal);

   Log("set escr_val = 0x%Lx for thread num %u", escrVal, threadNum);
}


/*
 *-----------------------------------------------------------------------------
 *
 * VmkperfResetWorldCounters --
 *
 *     Clears the per-world counters for ALL worlds in the system.
 *
 * Results:
 *     Returns 
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static void
VmkperfResetWorldCounters(uint32 counterNum)
{
   World_ID *allWorlds;
   int numWorlds, i;

   allWorlds = (World_ID *)Mem_Alloc(MAX_WORLDS * sizeof allWorlds[0]);
   if (allWorlds == NULL) {
      return;
   }
   World_AllWorlds(allWorlds, &numWorlds);
   for (i=0; i < numWorlds; i++) {
      World_Handle *world = World_Find(allWorlds[i]);
      if (world) {
         Vmkperf_ResetCounter(world, counterNum);
         World_Release(world);
      }
   }

   Mem_Free(allWorlds);
}

/*
 * -----------------------------------------------------------------------------
 *
 * VmkperfEnablePentium4 --
 *
 *  Used as a timer callback. Activates the counter specified in "data"
 *  (as a VmkperfEventInfo*) on this particular CPU. If the counter is
 *  already in use, the function simply resets it.
 *
 * Results:
 *   Returns void.
 *
 *
 * Side effects:
 *  Sets the performance counter to 0 and activates it with the
 *  specified event.
 *
 * -----------------------------------------------------------------------------
 */
static void
VmkperfEnablePentium4(void* data, UNUSED_PARAM(Timer_AbsCycles timestamp))
{
   uint64 cccr_val;
   uint64 escr_val;
   VmkperfEventInfo* eventInfo;
   CpuCounterInfo* ctInfo;
   uint8 threadNum;
   uint32 counterNum;
   CounterDesc *ctr;
   Bool doReset = FALSE;
   
   ASSERT(data != NULL);
  
   VmkperfLock();
  
   eventInfo = (VmkperfEventInfo*) data;
   threadNum = SMP_GetHTThreadNum(MY_PCPU);
   ctr = &eventInfo->ctr[threadNum];
   counterNum = ctr->counterNum;

   LOG(0, "Setting up counter: %s, event=%x, counterNum=%u...", 
       eventInfo->eventName, eventInfo->eventSel, counterNum);

   ctInfo = &cpuCounterInfoP4[MY_PCPU][counterNum];

   if (ctInfo->ctrEvent == eventInfo) {
      // This event is already being counted, so just reset it
      uint64 oldCccr, oldEscr;
      
      oldCccr = __GET_MSR(counterNum + PERFCTR_PENTIUM4_CCCR_BASE_ADDR);
      oldEscr = __GET_MSR(ctr->escrAddr);
      
      VmkperfSetupCounter(eventInfo, oldEscr, oldCccr);
      
      ctInfo->startTime = RDTSC();      
      LOG(0, "Reset counter");
      VmkperfUnlock();
      return;
   } else if (ctInfo->ctrEvent != NULL) {
      // another event hasn't quite finished running. This is a race condition,
      // should never get here
      LOG(0, "Attempted to enable %s while %s still running on same counter.",
          eventInfo->eventName, ctInfo->ctrEvent->eventName);
      ASSERT(0);
      
   }

   ctInfo->ctrEvent = eventInfo;

   cccr_val = 0;
   PERFCTR_PENTIUM4_CCCR_SET_ESCR(cccr_val, 
                                  ctr->escrIdx);
   cccr_val |= PERFCTR_PENTIUM4_CCCR_REQRSVD;
   cccr_val |= PERFCTR_PENTIUM4_CCCR_ENABLE;
   cccr_val |= eventInfo->cccrOptions;

   escr_val = 0;
   if (threadNum == 0) {
      escr_val |= PERFCTR_PENTIUM4_ESCR_USER_MODE_T0;
      escr_val |= PERFCTR_PENTIUM4_ESCR_KERNEL_MODE_T0;
   } else {
      escr_val |= PERFCTR_PENTIUM4_ESCR_USER_MODE_T1;
      escr_val |= PERFCTR_PENTIUM4_ESCR_KERNEL_MODE_T1;
   }

   escr_val |= eventInfo->eventSel;

   // set the model-specific registers.for this event
   VmkperfSetupCounter(eventInfo, escr_val, cccr_val);

   ctInfo->startTime = RDTSC();

   // this event is now officially active on this pcpu
   Vmkperf_EventListAdd(&activeEvents[MY_PCPU], eventInfo);
   
   // if we're the last counter to be activated, add a proc entry
   if (++(eventInfo->cpusActive) == numPCPUs) {
      char procCtrNameBuffer[MAX_PROC_NAMELEN], procWorldNameBuffer[MAX_PROC_NAMELEN];

      snprintf(procCtrNameBuffer,  sizeof(procCtrNameBuffer), 
               "counter_%s", eventInfo->eventName);
      eventInfo->procCounterEntry.parent = &vmkperfRootProc;
      eventInfo->procCounterEntry.read = VmkperfProcReadCounter;
      eventInfo->procCounterEntry.write = VmkperfProcSnapshotCounter;
      eventInfo->procCounterEntry.private = eventInfo;
    

      snprintf(procWorldNameBuffer, sizeof(procWorldNameBuffer),
               "worlds_%s", eventInfo->eventName);
      eventInfo->procWorldCounterEntry.parent = &vmkperfRootProc;
      eventInfo->procWorldCounterEntry.read = VmkperfProcWorldsReadCounters;
      eventInfo->procWorldCounterEntry.private = eventInfo;

      doReset = TRUE;
      VmkperfUnlock();
      Proc_Register(&eventInfo->procCounterEntry, procCtrNameBuffer, FALSE);
      Proc_Register(&eventInfo->procWorldCounterEntry, procWorldNameBuffer, FALSE);
   } else {
      VmkperfUnlock();
   }

   // after setting up the last pcpu, reset stats for this counter
   if (doReset) {
      VmkperfReset(eventInfo);
   }
   
   return;
}

/*
 *-----------------------------------------------------------------------------
 *
 * Vmkperf_FreePerfCtr --
 *
 *     Frees resources associated with the perfcounter or counters
 *     in "config," including the counter and the ESCR.
 *
 * Results:
 *     None.
 *
 * Side effects:
 *     Marks the associated counter and ESCR unused.
 *
 *-----------------------------------------------------------------------------
 */
void
Vmkperf_FreePerfCtr(PerfCtr_Config *config)
{
   int i;
   PerfCtr_Counter *ctrs = config->counters;

   ASSERT(cpuType == CPUTYPE_INTEL_PENTIUM4);

   VmkperfLock();

   for (i=0; i < SMP_MAX_CPUS_PER_PACKAGE; i++) {
      if (ctrs[i].index != INVALID_COUNTER_SENTRY) {
         ASSERT(usedCounters[ctrs[i].index]);
         ASSERT(VmkperfESCRUsed(ctrs[i].escrAddr));

         usedCounters[ctrs[i].index] = FALSE;
         VmkperfSetESCRLocked(ctrs[i].escrAddr, FALSE);
      }
   }
   VmkperfUnlock();
}


/*
 *-----------------------------------------------------------------------------
 *
 * Vmkperf_PerfCtrConfig --
 *
 *     Initializes "config" to contain the performance counter settings
 *     corresponding to the event "eventName".
 *     "eventName" should match a string in the vmkperf eventInfo table.
 *     Does not modify the "valid" bit of "config."
 *
 * Results:
 *     Returns VMK_OK on success, VMK_FAILURE if the event could
 *     not be allocated. After VMK_FAILURE, "config" is in an undefined state.
 *
 * Side effects:
 *     May claim a global performance counter or counters.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
Vmkperf_PerfCtrConfig(const char *eventName,            // in 
                      PerfCtr_Config *config)     // out
{
   VmkperfEventInfo *event;
   CounterDesc *desc;
   uint32 counterNum;
   uint8 threadNum;

   ASSERT(cpuType == CPUTYPE_INTEL_PENTIUM4);

   event = Vmkperf_GetEventInfo(eventName);
   if (event == NULL) {
      Warning("event type %s unknown", eventName);
      return (VMK_NOT_FOUND);
   }
   VmkperfLock();

   // allocate and configure one counter per hypertwin
   // Note that even on non-HT processors we grab two counters.
   // This is wasteful of counter resources, but simplifies testing/coding.

   for (threadNum=0; threadNum < SMP_MAX_CPUS_PER_PACKAGE; threadNum++) {
      PerfCtr_Counter *ctr = &config->counters[threadNum];
      desc = &event->ctr[threadNum];

      LOG(0, "eventName: %s", event->eventName);
      
      // reserve the counter
      counterNum = Vmkperf_FindCounter(desc->usableCounters);
      if (counterNum == INVALID_COUNTER_SENTRY) {
         VmkperfUnlock();
         LOG(0, "failed to find unused counter");
         return (VMK_FAILURE);
      } else if (VmkperfESCRUsed(desc->escrAddr)) {
         // clean up after ourselves before returning
         usedCounters[counterNum] = FALSE;
         if (threadNum == 1) {
            // free previous thread's counter too
            usedCounters[config->counters[0].index] = FALSE;
            VmkperfSetESCRLocked(config->counters[0].escrAddr, FALSE);
         }
         VmkperfUnlock();
         LOG(0, "Failed to find available ESCR");
         return (VMK_FAILURE);
      }

      ctr->escrVal = 0;
      ctr->cccrVal = 0;
      ctr->index = counterNum;
      ctr->addr = counterNum + PERFCTR_PENTIUM4_COUNTER_BASEADDR;
      ctr->cccrAddr = ctr->index + PERFCTR_PENTIUM4_CCCR_BASE_ADDR;
      PERFCTR_PENTIUM4_CCCR_SET_ESCR(ctr->cccrVal, desc->escrIdx);
      ctr->cccrVal |= event->cccrOptions;
      ctr->escrVal |= event->eventSel;
      ctr->escrAddr = desc->escrAddr;

      VmkperfSetESCRLocked(ctr->escrAddr, TRUE);

      // set thread and kernel/user masks
      if (threadNum == 1) {
         ctr->escrVal |= PERFCTR_PENTIUM4_ESCR_USER_MODE_T1     | 
            PERFCTR_PENTIUM4_ESCR_KERNEL_MODE_T1;
      } else {
         // works for both HT thread 0 and for non-HT processors
         ctr->escrVal |= PERFCTR_PENTIUM4_ESCR_USER_MODE_T0     |
            PERFCTR_PENTIUM4_ESCR_KERNEL_MODE_T0;
      }
   }
   
   VmkperfUnlock();
   return (VMK_OK);
}

/*
 *-----------------------------------------------------------------------------
 *
 * VmkperfSetupEnableProcs --
 *
 *  Installs the entries in the /proc/vmware/vmkperf/enable dir
 *
 * Results:
 *  Returns void
 *
 * Side effects:
 *  Creates one proc node for each event type we support
 *
 *-----------------------------------------------------------------------------
 */
static void
VmkperfSetupEnableProcs(void) 
{
   int i;
   int numEntries;
   VmkperfEventInfo* eventArray;
   Proc_Write enableFunction;
  
   eventArray = eventInfoPentium4;
   enableFunction = VmkperfProcEnableCounter;
   numEntries = NUM_PENTIUM4_EVENTS;

   for (i = 0; i < numEntries; i++) {      
      eventArray[i].procEnableEntry.write = enableFunction;
      eventArray[i].procEnableEntry.private = &eventArray[i];
      eventArray[i].procEnableEntry.parent = &vmkperfEnableProc;
      Proc_Register(&eventArray[i].procEnableEntry, eventArray[i].eventName, FALSE);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * Vmkperf_WorldSave --
 *  Store the data from the recent execution of the world specified in
 *  "save"
 *
 * Results:
 *  Returns void
 *
 * Side effects:
 *  Updates all per-world performance counters for this world with new
 *  aggregate counter data
 *
 *-----------------------------------------------------------------------------
 */
void 
Vmkperf_WorldSave(World_Handle* save) 
{
   int i;
   uint8 threadNum;
   PCPU myPcpu;

   ASSERT_NO_INTERRUPTS();

   myPcpu = MY_PCPU;
   
   // If we haven't set up counters for this world, abort
   // we should only initialize when a world is activated/restored
   if (!vmkperfRunning
       || save->vmkperfInfo == NULL
       || World_CpuSchedVcpu(save)->pcpu != myPcpu) {
      return;
   }

   ASSERT(cpuType == CPUTYPE_INTEL_PENTIUM4);

   
   // note that we only save counters that are running
   // on our current logical cpu
   threadNum = SMP_GetHTThreadNum(myPcpu);
  
   for (i=0; i < activeEvents[myPcpu].len; i++) {
      uint64 curtime;
      uint64 val;
      VmkperfEventInfo* event;
      struct VmkperfWorldCounterInfo* saveEv;
      CounterDesc *cdesc;

      event = activeEvents[myPcpu].list[i];
      cdesc = &event->ctr[threadNum];

      saveEv = &save->vmkperfInfo[cdesc->counterNum];

      if (event->cpusActive == numPCPUs) {
         uint64 oldStartTime, oldStartCounter;

         oldStartTime = saveEv->startTime;
         oldStartCounter = saveEv->startCounter;

         val = RDPMC(cdesc->counterNum) & PERFCTR_PENTIUM4_VAL_MASK;
         curtime = RDTSC();

         saveEv->startTime = curtime;
         saveEv->startCounter = val;

         // watch out for cases in which the counter just got reset
         // and only update the total counts if none of these values is 0
         // or rolling backwards
         if (oldStartCounter != 0
             && oldStartTime != 0
             && val > oldStartCounter) {
            saveEv->totalTime += (curtime - oldStartTime);
            saveEv->totalCounter += (val - oldStartCounter);
         }
      }
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * Vmkperf_WorldRestore --
 *
 *  Prepare the performance counters for this world, which is about to
 *  execute. Sets up the vmkperfInfo of this world for the first time,
 *  if necessary.
 *
 * Results:
 *  Returns void
 *
 * Side effects:
 *  Sets the current counter and timestamp values in this world's
 *  vmkperfInfo array
 *
 *-----------------------------------------------------------------------------
 */
void Vmkperf_WorldRestore(World_Handle* restore) 
{
   int i;
   uint8 threadNum;

   ASSERT_NO_INTERRUPTS();
   
   if (!vmkperfRunning) {
      return;
   }

   if (restore->vmkperfInfo == NULL) {
      // We lazily initialize the per-world vmkperf data when
      // it's first restored. This is important, because the user
      // may boot with per-world profiling disabled, then enable it
      // while a world is already running
      Vmkperf_InitWorld(restore);
   }

   threadNum = SMP_GetHTThreadNum(MY_PCPU);

   ASSERT(restore->vmkperfInfo != NULL);

   for (i=0; i < activeEvents[MY_PCPU].len; i++) {
      uint64 curtime;
      uint64 val;
      struct VmkperfWorldCounterInfo* restoreEv;
      VmkperfEventInfo* event;
      uint32 counterNum;

      event = activeEvents[MY_PCPU].list[i];
      counterNum = event->ctr[threadNum].counterNum;
      if (counterNum == INVALID_COUNTER_SENTRY) {
         continue;
      }
      restoreEv = &restore->vmkperfInfo[counterNum];

      if (event->cpusActive == numPCPUs) {
         ASSERT(restoreEv != NULL);

         val = RDPMC(counterNum) & PERFCTR_PENTIUM4_VAL_MASK;
         curtime = RDTSC();
       
         restoreEv->startTime = curtime;
         restoreEv->startCounter = val;
      } 
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * Vmkperf_InitWorld --
 *
 *  Initializes the performance counters for this world for the first
 *  time. 
 *
 * Results:
 *  Returns void.
 *
 * Side effects:
 *  Allocates a vmkperfInfo array for this world and inits it to 0.
 *
 *-----------------------------------------------------------------------------
 */
void
Vmkperf_InitWorld(World_Handle* world) 
{
   if (cpuType != CPUTYPE_INTEL_PENTIUM4) {
      return;
   }
   world->vmkperfInfo = World_Alloc(world, sizeof(struct VmkperfWorldCounterInfo)
                                           * PERFCTR_PENTIUM4_NUM_PERFCTRS);
   ASSERT(world->vmkperfInfo != NULL);
  
   memset(world->vmkperfInfo, 0, sizeof(struct VmkperfWorldCounterInfo) 
                                        * PERFCTR_PENTIUM4_NUM_PERFCTRS);

   LOG(1, "Initialized performance counter info for world %d", world->worldID);
}


/*
 *-----------------------------------------------------------------------------
 *
 * Vmkperf_CleanupWorld --
 *
 *     Called at world death to clean up per-world data structures.
 *     Dumps per-world data to log file.
 *
 * Results:
 *     None.
 *
 * Side effects:
 *     May dump to log, free memory for data structures.
 *
 *-----------------------------------------------------------------------------
 */
void 
Vmkperf_CleanupWorld(World_Handle* world) 
{
   int i;

   if (world->vmkperfInfo == NULL) {
      return;
   }
   ASSERT(cpuType == CPUTYPE_INTEL_PENTIUM4);

   for (i = 0; i < NUM_PENTIUM4_EVENTS; i++) {
      uint32 counterNum0, counterNum1;
      VmkperfEventInfo *event = &eventInfoPentium4[i];

      counterNum0 = event->ctr[0].counterNum;
      counterNum1 = event->ctr[1].counterNum;

      if (counterNum0 == INVALID_COUNTER_SENTRY || 
          (SMP_HTEnabled() && counterNum1 == INVALID_COUNTER_SENTRY)) {
         continue;
      }

      uint64 val = (world->vmkperfInfo[counterNum0].totalCounter
                    + HTONLY(world->vmkperfInfo[counterNum1].totalCounter));
      uint64 divtime =  (world->vmkperfInfo[counterNum0].totalTime 
                         + HTONLY(world->vmkperfInfo[counterNum1].totalTime)) / 1000000ul;

      // If we're really tracking per world events, log the
      // counts when we clean up a world. Otherwise, there would be
      // no way to obtain this data.
      if (divtime > 0ul
         && CONFIG_OPTION(VMKPERF_PER_WORLD)) {
         VmLog(world->worldID, "%s:\t%12Lu\t\t%Lu per million cycles avg\n",
             event->eventName,
             val,
             val / divtime);
      }
   }

   World_Free(world, world->vmkperfInfo);
   world->vmkperfInfo = NULL;
}


/*
 *-----------------------------------------------------------------------------
 *
 * VmkperfProcWorldsReadCounters --
 *
 *  Handler for read operation on /proc/vmware/vmkperf/worlds_XXX.
 *  Writes per-world counters for the event specified in
 *  entry->private to "page". 
 *
 * Results:
 *  Returns VMK_OK
 *
 * Side effects:
 *  Writes output to "page". Stores length of output in "lenp"
 *
 *-----------------------------------------------------------------------------
 */
static int
VmkperfProcWorldsReadCounters(Proc_Entry* entry,
                              char *page,
                              int *lenp) 
{
   World_ID *worldIds;
   int count, i, n;
   uint32 counter0, counter1;

   VmkperfEventInfo* event = (VmkperfEventInfo*) entry->private;

   LOG(1, "Read proc worlds counters");

   *lenp = 0;

   if (!Vmkperf_TrackPerWorld()) {
      Proc_Printf(page, lenp, "Not recording per-world data. "
                  "Use VmkperfPerWorld config option to enable.\n");
      return VMK_OK;
   }

   n = MAX_WORLDS;
   *lenp = 0;

   counter0 = event->ctr[0].counterNum;
   counter1 = event->ctr[1].counterNum;

   worldIds = (World_ID *)Mem_Alloc(MAX_WORLDS * sizeof worldIds[0]);
   if (worldIds == NULL) {
      return VMK_NO_MEMORY;
   }
   // iterate over all worlds, printing each one's vmkperf information
   count = World_AllWorlds(worldIds, &n);
   for (i=0; i < count; i++) {
      World_Handle* world = World_Find(worldIds[i]);
      if (world != NULL) {
         if (world->vmkperfInfo != NULL) {
            uint64 val = world->vmkperfInfo[counter0].totalCounter 
               + HTONLY(world->vmkperfInfo[counter1].totalCounter);
            uint64 divtime =  (world->vmkperfInfo[counter0].totalTime 
                               + HTONLY(world->vmkperfInfo[counter1].totalTime)) / 1000000ul;

            if (divtime > 0ul) {
               Proc_Printf(page, lenp,
                           "%-4d (%-16.16s):\t%12Lu\t\t%Lu per million cycles avg\n",
                           world->worldID,
                           world->worldName,
                           val,
                           val / divtime);
            }
         }
         World_Release(world);
      }
   }

   Mem_Free(worldIds);
   return VMK_OK;
}

uint64
Vmkperf_GetWorldEventCount(World_Handle *world, VmkperfEventInfo *info)
{
   uint64 total;
   uint32 counter0, counter1;
   
   if (cpuType != CPUTYPE_INTEL_PENTIUM4) {
      return 0;
   }

   VmkperfLock();

   counter0 = info->ctr[0].counterNum;
   counter1 = info->ctr[1].counterNum;

   if (world->vmkperfInfo == NULL
       || counter0 == INVALID_COUNTER_SENTRY) {
      total = 0;
   } else {
      total = world->vmkperfInfo[counter0].totalCounter 
         + HTONLY(world->vmkperfInfo[counter1].totalCounter);
   }

   VmkperfUnlock();

   return total;
}

/*
 *-----------------------------------------------------------------------------
 *
 * Vmkperf_ResetCounter --
 *  Simply sets the specified counter to 0.
 *
 * Results:
 *  Returns void.
 *
 * Side effects:
 *  Clears per-world counters for specified world/counter combo.
 *
 *-----------------------------------------------------------------------------
 */
void
Vmkperf_ResetCounter(World_Handle* world, uint32 counterNum) 
{
   if (cpuType != CPUTYPE_INTEL_PENTIUM4) {
      return;
   }
   LOG(2, "Reset vmkperf counters for counter %u, world %u", 
       counterNum, world->worldID);

   ASSERT(counterNum != INVALID_COUNTER_SENTRY);
   
   VmkperfLock();

   // if we haven't set up this world yet, just return
   if (world->vmkperfInfo == NULL) {
      VmkperfUnlock();
      return;
   }

   world->vmkperfInfo[counterNum].totalCounter = 0ul;
   world->vmkperfInfo[counterNum].totalTime = 0ul;

   world->vmkperfInfo[counterNum].startTime = 0ul;
   world->vmkperfInfo[counterNum].startCounter = 0ul;

   VmkperfUnlock();
}


/*
 *-----------------------------------------------------------------------------
 *
 * VmkperfDebugProcRead --
 *
 *     Proc read handler to display internal info about status
 *     of performance counters.
 *
 * Results:
 *     Returns VMK_OK.
 *
 * Side effects:
 *     Writes to proc buffer.
 *
 *-----------------------------------------------------------------------------
 */
static int
VmkperfDebugProcRead(Proc_Entry *entry, char *buffer, int *len)
{
   int i;
   *len = 0;
   
   VmkperfLock();
               
   // show counter usage
   Proc_Printf(buffer, len, "ctr used?\n");
   for (i=0; i < PERFCTR_PENTIUM4_NUM_PERFCTRS; i++) {
      Proc_Printf(buffer, len, "%2d     %1d\n", i, usedCounters[i]);
   }

   // show ESCR usage, ignoring unknown ESCR addrs
   Proc_Printf(buffer, len, "\nescr used?\n");
   for (i=0; i < PENTIUM4_NUM_ESCR_ADDRS; i++) {
      uint32 thisAddr = i + PENTIUM4_MIN_ESCR_ADDR;
      if (VmkperfESCRValid(thisAddr)) {
         Proc_Printf(buffer, len, "0x%2x     %1d\n", thisAddr, usedESCRs[i]);
      }
   }

   // show per-event counter utilization
   Proc_Printf(buffer, len, "\n      event               ctr0  ctr1\n");
   for (i=0; i < NUM_PENTIUM4_EVENTS; i++) {
      Proc_Printf(buffer, len, "%24s    %2d    %2d\n", 
                  eventInfoPentium4[i].eventName,
                  eventInfoPentium4[i].ctr[0].counterNum,
                  eventInfoPentium4[i].ctr[1].counterNum);
   }

   VmkperfUnlock();
   return(VMK_OK);
}

/*
 *-----------------------------------------------------------------------------
 *
 * Vmkperf_SetSamplerRate --
 *
 *      Changes the per-pcpu sampling rate of vmkperf counters to
 *      "sampleMs" milliseconds. If sampleMs is 0, disables sampling.
 *      If sampleMs is -1, reverts to default.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Removes old timer handlers, adds new ones.
 *
 *-----------------------------------------------------------------------------
 */
void
Vmkperf_SetSamplerRate(uint32 sampleMs)
{
   PCPU i;
   uint32 ms = sampleMs;

   // use default
   if (ms == -1) {
      ms = VMKPERF_UPDATE_TIMER_DELAY;
   }
   for (i=0; i < numPCPUs; i++) {
      if (timerHandles[i] != 0) {
         Timer_Remove(timerHandles[i]);
         timerHandles[i] = 0;
      }
      if (ms != 0) {
         timerHandles[i] = Timer_Add(i, VmkperfPcpuSnapshotAllCounters,
                                     ms, TIMER_PERIODIC, NULL);
      }
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * Vmkperf_Init --
 *
 *  Initialize the vmkperf module.
 *
 * Results:
 *  Return void
 *
 * Side effects:
 *  Installs proc nodes, allocates counters
 *
 *-----------------------------------------------------------------------------
 */
void 
Vmkperf_Init(void) 
{
   int i, j;
   i = j = 0; // make gcc not complain about unused variables when vmkperf off

   // don't let any non-p4 cpus through here!
   if (cpuType != CPUTYPE_INTEL_PENTIUM4) {
      LOG(0, "Cputype unsupported by vmkperf -- not initializing");
      return;
   }

   // set up some basics that are used by both vmkstats and vmkperf
   memset(usedCounters, 0, sizeof(usedCounters));

   // All of the following is only used when we have vmkperf counters on
   LOG(0, "Initializing vmkperf, lock rank = %u", SP_RANK_VMKPERF_USEDCOUNTER - 1);
   SP_InitLockIRQ("vmkperfLock", &vmkperfLock, SP_RANK_VMKPERF_USEDCOUNTER - 1); 

   VmkperfLock();

   cpuCounterInfoP4 = (CpuCounterInfo**) Mem_Alloc(sizeof(CpuCounterInfo*) * numPCPUs);
   ASSERT(cpuCounterInfoP4 != NULL);
   
   for (i=0; i < numPCPUs; i++) {
      cpuCounterInfoP4[i] = (CpuCounterInfo*) Mem_Alloc(sizeof(CpuCounterInfo) * PERFCTR_PENTIUM4_NUM_PERFCTRS);
      ASSERT(cpuCounterInfoP4[i] != NULL);
      memset(cpuCounterInfoP4[i], 0, sizeof(CpuCounterInfo) * PERFCTR_PENTIUM4_NUM_PERFCTRS);

      for (j=0; j < PERFCTR_PENTIUM4_NUM_PERFCTRS; j++) {
         cpuCounterInfoP4[i][j].ctrEvent = NULL;
      }
   }
      
   for (i=0; i < NUM_PENTIUM4_EVENTS; i++) {
      eventInfoPentium4[i].ctr[0].counterNum = INVALID_COUNTER_SENTRY;
      eventInfoPentium4[i].ctr[1].counterNum = INVALID_COUNTER_SENTRY;
      eventInfoPentium4[i].cpusActive = 0;

      Trace_RegisterCustomTag(TRACE_VMKPERF,
                              (uint32)&eventInfoPentium4[i],
                              eventInfoPentium4[i].eventName);
   }

   VmkperfUnlock();

   // setup proc directories
   Proc_Register(&vmkperfRootProc, "vmkperf", TRUE);
   vmkperfEnableProc.parent = &vmkperfRootProc; 
   Proc_Register(&vmkperfEnableProc, "enable", TRUE);

   // setup debug proc node
   vmkperfDebugProc.parent = &vmkperfRootProc;
   vmkperfDebugProc.read = VmkperfDebugProcRead;
   Proc_RegisterHidden(&vmkperfDebugProc, "debug", FALSE);

   VmkperfSetupEnableProcs();

   // add a timer on each CPU to update the saved counter values periodically
   Vmkperf_SetSamplerRate(VMKPERF_UPDATE_TIMER_DELAY);

   vmkperfRunning = TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * VmkperfFindP6EventInfo --
 *
 *     Returns a pointer the the eventInfo struct for eventName.
 *     Works on Pentium III only.
 *
 * Results:
 *     Returns a pointer the the eventInfo struct for eventName
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */

static VmkperfP6EventInfo*
VmkperfFindP6EventInfo(const char *eventName)
{
   int i;

   for (i=0; i < NUM_P6_EVENTS; i++) {
      if (!strcmp(eventInfoP6[i].eventName, eventName)) {
         return (&eventInfoP6[i]);
      }
   }

   return (NULL);
}

/*
 *-----------------------------------------------------------------------------
 *
 * Vmkperf_GetEventInfo --
 *
 *     Returns a pointer the the eventInfo struct for eventName.
 *     Works on Pentium IV only.
 *
 * Results:
 *     Returns a pointer the the eventInfo struct for eventName
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
VmkperfEventInfo*
Vmkperf_GetEventInfo(const char *eventName)
{
   int i;

   // find the event
   for (i=0; i < NUM_PENTIUM4_EVENTS; i++) {
      if (!strcmp(eventInfoPentium4[i].eventName, eventName)) {
         return (&eventInfoPentium4[i]);
      }
   }
   
   return (NULL);
}

/*
 *-----------------------------------------------------------------------------
 *
 * Vmkperf_GetP6Event --
 *
 *     Returns the performance counter event corresponding to
 *     eventName, or INVALID_COUNTER_SENTRY if eventName is not found
 *
 * Results:
 *     Returns the performance counter event corresponding to
 *     eventName, or INVALID_COUNTER_SENTRY if eventName is not found
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
uint32
Vmkperf_GetP6Event(const char *eventName)
{
   VmkperfP6EventInfo *info = VmkperfFindP6EventInfo(eventName);
   if (info != NULL) {
      return (info->counter);
   } else {
      return (INVALID_COUNTER_SENTRY);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * Vmkperf_GetDefaultPeriod --
 *
 *     Returns the default sampling period for the "eventName" event,
 *     or -1 if eventName is not found
 *
 * Results:
 *     Returns sampling period or -1
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
uint32
Vmkperf_GetDefaultPeriod(const char *eventName)
{
   if (cpuType == CPUTYPE_INTEL_PENTIUM4) {
      VmkperfEventInfo *info = Vmkperf_GetEventInfo(eventName);
      if (info != NULL) {
         return (cpuKhzEstimate / POW(10, info->rate));
      } else {
         return (-1);
      }
   } else if  (cpuType == CPUTYPE_INTEL_P6) {
      VmkperfP6EventInfo *info = VmkperfFindP6EventInfo(eventName);

      if (info != NULL) {
         return (cpuKhzEstimate / POW(10, info->rate));
      } else {
         return (-1);
      }
   } else {
      Warning("unsupported cpu type");
      return (-1);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * Vmkperf_GetCanonicalEventName --
 *
 *     Returns a pointer to the canonical event name for the specified event
 *
 * Results:
 *     Returns a pointer to the canonical event name for the specified event
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
const char*
Vmkperf_GetCanonicalEventName(const char *eventName)
{
   if (cpuType == CPUTYPE_INTEL_PENTIUM4) {
      VmkperfEventInfo *info = Vmkperf_GetEventInfo(eventName);
      if (info != NULL) {
         return (info->eventName);
      } else {
         return (NULL);
      }
   } else if  (cpuType == CPUTYPE_INTEL_P6) {
      VmkperfP6EventInfo *info = VmkperfFindP6EventInfo(eventName);

      if (info != NULL) {
         return (info->eventName);
      } else {
         return (NULL);
      }
   } else {
      Warning("unsupported cpu type");
      return (NULL);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * Vmkperf_PrintCounterList --
 *
 *     Writes the list of available counters to "buffer"
 *
 * Results:
 *     Writes the list of available counters to "buffer"
 *
 * Side effects:
 *     *len increased by length of written data
 *
 *-----------------------------------------------------------------------------
 */
void
Vmkperf_PrintCounterList(char *buffer, int *len)
{
   int i;

   if (cpuType == CPUTYPE_INTEL_PENTIUM4) {
      for (i=0; i < NUM_PENTIUM4_EVENTS; i++) {
         VmkperfEventInfo *info = &eventInfoPentium4[i];
         Proc_Printf(buffer, len, "%-28s    %10u\n",
                     info->eventName,
                     Vmkperf_GetDefaultPeriod(info->eventName));
      }
   } else {
      for (i=0; i < NUM_P6_EVENTS; i++) {
         VmkperfP6EventInfo *info = &eventInfoP6[i];
         Proc_Printf(buffer, len, "%-28s    %10u\n",
                     info->eventName,
                     Vmkperf_GetDefaultPeriod(info->eventName));
      }
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * Vmkperf_ReadLocalCounter --
 *
 *      Reads the current value of the counter specified by "event" on the
 *      local package. Returns the value for the current logical cpu if "hypertwin"
 *      is FALSE or the current logical cpu's partner if "hypertwin" is TRUE.
 *
 * Results:
 *      Returns current value of counter.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
uint64
Vmkperf_ReadLocalCounter(VmkperfEventInfo *event, Bool hypertwin)
{
   uint64 val;
   PCPU myPcpu = MY_PCPU;
   uint32 counterNum, threadNum;
   ASSERT(!CpuSched_IsPreemptible());

   if (hypertwin) {
      ASSERT(SMP_HTEnabled());
      threadNum = SMP_GetHTThreadNum(SMP_GetPartnerPCPU(myPcpu));
   } else {
      threadNum = SMP_GetHTThreadNum(myPcpu);
   }

   counterNum = event->ctr[threadNum].counterNum;
   val = RDPMC(counterNum) & PERFCTR_PENTIUM4_VAL_MASK;

   return (val);
}
