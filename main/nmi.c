/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * nmi.c --
 *
 *	This module manages non-maskable APIC interrupts.
 */

/*
 * Includes
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "x86.h"
#include "vmkernel.h"
#include "vm_asm.h"
#include "kvmap.h"
#include "apic.h"
#include "vmnix_if.h"
#include "host.h"
#include "sched.h"
#include "world.h"
#include "prda.h"
#include "util.h"
#include "x86perfctr.h"
#include "debug.h"
#include "smp.h"
#include "timer.h"
#include "vmkstats.h"
#include "nmi.h"
#include "serial.h"
#include "vmkperf.h"
#include "proc.h"
#include "parse.h"
#include "user.h"

#define LOGLEVEL_MODULE NMI
#include "log.h"

extern Bool vmkernelLoaded;
extern void CommonNMIIret(void);

Bool NMIAllowed = FALSE;
Bool NMI_Pending = FALSE;

// used for measuring average time for an NMI
static uint64 nmiCurrentTotalCyclesSampling = 0;
static uint32 nmiCurrentNumSamples = 0;

/*
 * Constants
 */

#define PERFCTR_PENTIUM4_VAL_MASK 	(0x000000ffffffffffLL)
#define PERFCTR_P6_VAL_MASK             (0x000000ffffffffLL)

// CPU clock rate
#define	PERFCTR_CYCLES_PER_MSEC		(cpuKhzEstimate)

// watchdog default 1s
#define	NMI_WATCHDOG_PERIOD		(1000 * PERFCTR_CYCLES_PER_MSEC)
#define	NMI_WATCHDOG_RESET		PERIOD_TO_RESET(NMI_WATCHDOG_PERIOD)
#define MAX_HANG_COUNTER		(3)

// sampler default 500usec
#define	NMI_SAMPLER_PERIOD		(PERFCTR_CYCLES_PER_MSEC / 2)
#define	NMI_SAMPLER_RESET		PERIOD_TO_RESET(NMI_SAMPLER_PERIOD)

// assign counters, n.b. must always use counter zero on P6 family, so
// we use counter 0 for them both, and watchdog + sampler cannot be on at the
// same time.
#define	P6_NMI_WATCHDOG_CTR		(0)
#define	P6_NMI_SAMPLER_CTR	       	(0)

#define NMI_TRACK_LOST_PERF_EVENTS      (1)
#define NMI_LOST_CYCLES_MAX             (1ll << 32)

#define PERFCTR_VALUE_MASK ((cpuType == CPUTYPE_INTEL_PENTIUM4) ? PERFCTR_PENTIUM4_VAL_MASK : PERFCTR_P6_VAL_MASK)

/*
 * Globals
 */

static PerfCtr_Config sampler;

static VMK_ReturnStatus NMIPentium4MakePCMSRs(PerfCtr_Config *config, const char *event, uint32 period);
static void NMISamplerDisable(void);
static void NMISamplerEnable(void);
static void NMISamplerPerCpuStart(void);

static SP_SpinLockIRQ perfCtrLock;

static PerfCtr_Config watchdog;
static Proc_Entry watchdogProc;

/*
 * Macros
 */

#define	PERIOD_TO_RESET(_period)	(0 - (_period + 1))

/*
 * Performance counter operations
 */

static INLINE uint64 
PerfCtrReadCounter(PerfCtr_Counter *ctr) 
{
   uint64 val;

   val = RDPMC(ctr->index) & PERFCTR_VALUE_MASK;
   return (val);
}


/*
 *----------------------------------------------------------------------
 *
 * NMI_GetPerfCtrConfig --
 *
 *      Return performance counter configuration information.
 *
 * Results:
 *      reference parameter set to perf ctr information.
 *
 * Side effects:
 *      none in addition to returning above info.
 *
 *----------------------------------------------------------------------
 */
extern void
NMI_GetPerfCtrConfig(PerfCtr_Config *ctr)
{
    SP_IRQL prevIRQL;

    prevIRQL = SP_LockIRQ(&perfCtrLock, SP_IRQL_KERNEL);

    *ctr = sampler;

    SP_UnlockIRQ(&perfCtrLock, prevIRQL);
}

static uint32
NMIComputeSamplesPerSec(uint32 period)
{
   return ((uint32)cpuKhzEstimate * 1000) / period;
}

/*
 * NMI sampler operations
 */

/*
 *-----------------------------------------------------------------------------
 *
 * NMIPentium4MakePCMSRs --
 *
 *  Configure "ctr" to contain the config info to monitor "event"
 *  with the given "period"
 *  Note that this only works on the Pentium IV.
 *
 * Results:
 *     Fills in "ctr" with the appropriate configuration.
 *     Returns VMK_OK on success, VMK_FAILURE otherwise
 *
 * Side effects:
 *     May allocate a performance counter from the vmkperf module
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
NMIPentium4MakePCMSRs(PerfCtr_Config *config, const char *event, uint32 period)
{
   VMK_ReturnStatus res;

   // if "config" isn't already initialized to this event, set it up
   if (!config->eventName || strcmp(config->eventName, event) != 0) {
      res = Vmkperf_PerfCtrConfig(event, config);
      if (res != VMK_OK) {
         Warning("failed to configure vmkstats event");
         return (VMK_FAILURE);
      }
      Log("configured sampler");
   }

   config->counters[0].cccrVal |= PERFCTR_PENTIUM4_CCCR_REQRSVD      |
                                  PERFCTR_PENTIUM4_CCCR_ENABLE       |
                                  PERFCTR_PENTIUM4_CCCR_OVF_PMI_T0;
   
   config->counters[1].cccrVal |= PERFCTR_PENTIUM4_CCCR_REQRSVD      |
                                  PERFCTR_PENTIUM4_CCCR_ENABLE       |
                                  PERFCTR_PENTIUM4_CCCR_OVF_PMI_T1;


   config->resetLo = PERIOD_TO_RESET(period);
   config->resetHi = 0x000000ff; // no sign extension done by Pentium4
   config->valid = TRUE;
   return (VMK_OK);
}


/*
 *----------------------------------------------------------------------
 *
 * NMI_SamplerSetConfig --
 *
 *      Initialize performance counter values associated with
 *	NMI-based sampling to generate an interrupt after "period"
 *	events of type "event" occur.
 *
 * Results:
 *      Returns VMK_OK on success, VMK_FAILURE otherwise.
 *
 * Side effects:
 *	Updates sampler globals.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
NMI_SamplerSetConfig(const char *event, uint32 period)
{
   SP_IRQL prevIRQL;
   PerfCtr_Config *config = &sampler;
   PerfCtr_Counter *counter;
   VMK_ReturnStatus res = VMK_FAILURE;

   prevIRQL = SP_LockIRQ(&perfCtrLock, SP_IRQL_KERNEL);

   if (period == NMI_SAMPLER_DEFAULT_PERIOD) {
      period = Vmkperf_GetDefaultPeriod(event);
   }
   
   switch (cpuType) {
   
   case CPUTYPE_INTEL_P6: {
      uint32 p6Event = Vmkperf_GetP6Event(event);

      if (p6Event == INVALID_COUNTER_SENTRY) {
         Warning("unknown event: %s", event);
         res = VMK_BAD_PARAM;
         break;
      }
      counter = &config->counters[0]; // no hypertwins on P6

      counter->index = P6_NMI_SAMPLER_CTR;
      counter->addr = MSR_PERFCTR0 + P6_NMI_SAMPLER_CTR;
      counter->escrAddr = MSR_EVNTSEL0 + P6_NMI_SAMPLER_CTR;
      counter->cccrAddr = 0; // Pentium4-only MSR
      counter->escrVal = p6Event |
                  PERFCTR_P6_USER_MODE   | 
                  PERFCTR_P6_KERNEL_MODE |
                  PERFCTR_P6_ENABLE      |
                  PERFCTR_P6_APIC_INTR;              
      counter->cccrVal = 0; // Pentium4-only MSR
      
      config->resetLo = PERIOD_TO_RESET(period);
      config->resetHi = 0; // sign extended in the MSR for P6 family
      config->valid = TRUE;
      res = VMK_OK;
      break;
   }

   case CPUTYPE_INTEL_PENTIUM4:
      if (config->valid) {
         Vmkperf_FreePerfCtr(config);
         config->eventName = NULL;
         config->valid = FALSE;
      }

      res = NMIPentium4MakePCMSRs(config, event, period);
      break;

   case CPUTYPE_AMD_ATHLON:
   case CPUTYPE_AMD_DURON:
   case CPUTYPE_OTHER:
   case CPUTYPE_UNSUPPORTED:
      // hush gcc (case never happens - NMI_Init() tests the CPU type)
      res = VMK_FAILURE;
      ASSERT_NOT_IMPLEMENTED(TRUE);
   }

   if (res == VMK_OK) {
      config->eventName = Vmkperf_GetCanonicalEventName(event);
      config->samplesPerSec = NMIComputeSamplesPerSec(period);
      config->period = period;
      config->config++;
   }

   SP_UnlockIRQ(&perfCtrLock, prevIRQL);
   return (res);
}


/*
 *-----------------------------------------------------------------------------
 *
 * NMI_SamplerGetEventName --
 *
 *     Returns the name of the currently-configured sampler event
 *
 * Results:
 *     Returns the name of the currently-configured sampler event
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
const char*
NMI_SamplerGetEventName(void)
{
   return sampler.eventName;
}


/*
 *-----------------------------------------------------------------------------
 *
 * NMI_SamplerGetPeriod --
 *
 *     Returns the sampling period of the currently-configured sampler event
 *
 * Results:
 *     Returns the sampling period of the currently-configured sampler event
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
uint32
NMI_SamplerGetPeriod(void)
{
   return sampler.period;
}


/*
 *-----------------------------------------------------------------------------
 *
 * NMISamplerChangeCallback --
 *
 *     Make any sampler configuration changes take effect.
 *     Will briefly disable NMIs if they were enabled, but will
 *     put them in the proper state before returning. May enable
 *     NMIs if they were previously disabled.
 *
 * Results:
 *     None.
 *
 * Side effects:
 *     NMIs temporarily disabled. 
 *     New sampler configuration takes effect.
 *
 *-----------------------------------------------------------------------------
 */
void
NMISamplerChangeCallback(UNUSED_PARAM(void *data),
                         UNUSED_PARAM(Timer_AbsCycles timestamp))
{
   // if we're running this callback, then we're processing
   // bottom halves and it's obviously safe to turn on NMIs
   // very briefly, as long as we don't switch away
   ASSERT(!CpuSched_IsPreemptible());

   if (myPRDA.nmisEnabled) {
      NMISamplerDisable();
      NMISamplerEnable();
   } else if (myPRDA.configNMI == NMI_USING_SAMPLER ||
              myPRDA.configNMI == NMI_SETUP_SAMPLER) {
      NMISamplerEnable();
   }
}

/*
 *----------------------------------------------------------------------
 *
 * NMI_SamplerChange --
 *
 *      Turns on or off the sampler by setting per-pcpu flags and
 *      firing off timers on each pcpu to make them take effect.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Timers fired on all pcpus, prdas[*].configNMI changed
 *
 *----------------------------------------------------------------------
 */
void 
NMI_SamplerChange(Bool turnOn) {
   PCPU i;
   
   if ((cpuType != CPUTYPE_INTEL_P6) && (cpuType != CPUTYPE_INTEL_PENTIUM4)) {
      Warning("Can't do NMI tracing on non Intel P6/Pentium4 processors");
      return;
   }

   if (myPRDA.configNMI && turnOn) {
      Warning("error, either watchdog or sampler is already running");
      return;
   } else if (!turnOn && myPRDA.configNMI != NMI_USING_SAMPLER) {
      Warning("error, sampler is not on, so it cannot be disabled");
      return;
   }

   for (i=0; i<numPCPUs; i++) {
      if (turnOn) {
         // tell pcpu i to turn ON its own sampler
         prdas[i]->configNMI = NMI_SETUP_SAMPLER;
      } else {
         // tell pcpu i to turn OFF its own sampler
         prdas[i]->configNMI = NMI_DISABLING_SAMPLER;
      }

      // fire a timer to make change take effect ASAP (will be slight lag)
      Timer_AddHiRes(i, 
                     NMISamplerChangeCallback,
                     1,
                     TIMER_ONE_SHOT,
                     NULL);
   }
}

static void
NMISamplerPRDAConfig(volatile PRDA *p)
{
   uint8 threadNum = SMP_GetHTThreadNum(p->pcpuNum);
   // configure per-processor VMKStats state
   p->vmkstatsPerfCtrReset = sampler.resetLo;
   p->vmkstatsPerfCtrValue = sampler.resetLo;
   p->vmkstatsPerfCtrEvent = sampler.counters[threadNum].escrVal;
   p->vmkstatsMissingEvents = FALSE;
   p->vmkstatsConfig = sampler.config;
}

/*
 *----------------------------------------------------------------------
 *
 * NMISamplerPerCpuStart --
 *
 *      Called for each pcpu to start the sampler.  Sets up the hardware
 *	performance counters for sampling.
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
NMISamplerPerCpuStart(void)
{
   uint8 threadNum = SMP_GetHTThreadNum(MY_PCPU);

   ASSERT(sampler.valid);
   
   // specify NMI interrupt mode for performance counters
   APIC_PerfCtrSetNMI();

   // initialize PRDA configuration
   NMISamplerPRDAConfig(&myPRDA);

   // sample using appropriate counter
   myPRDA.vmkstatsMissingEvents = FALSE;
   PerfCtrWriteEvtSel(&sampler.counters[threadNum], 0, 0);
   PerfCtrWriteCounter(&sampler.counters[threadNum], sampler.resetLo, sampler.resetHi);
}

/*
 *----------------------------------------------------------------------
 *
 * NMISamplerEnable --
 *
 *      Called from NMIEnableInt to turn the sampler on after it has been
 *	disabled.
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
NMISamplerEnable(void)
{
   uint32 event, value;
   uint8 threadNum;
   PerfCtr_Counter *counter;
   volatile PRDA *p = &myPRDA;

   threadNum = SMP_GetHTThreadNum(MY_PCPU);
   counter = &sampler.counters[threadNum];

   // determine sampling params
   if (vmkernelLoaded) {
      // configure, if necessary
      if (p->vmkstatsConfig != sampler.config) {
         NMISamplerPRDAConfig(p);
      }
      
      if (cpuType == CPUTYPE_INTEL_PENTIUM4
          && NMI_TRACK_LOST_PERF_EVENTS
          && p->configNMI == NMI_USING_SAMPLER
          && p->vmkstatsMissingEvents) {
         // read old value to compute lost event count
         // (number of events that transpired while sample disabled)
         uint64 lostCount = PerfCtrReadCounter(counter);
         Log_Event("lost-count", lostCount, EVENTLOG_VMKSTATS);
         
         if (lostCount > NMI_LOST_CYCLES_MAX) {
            Log("lost too many counts! %Lu", lostCount);
         } else {
            p->vmkstatsMissedEvents += lostCount;
         }
      }

      // use saved restart value, event
      value = p->vmkstatsPerfCtrValue;
      event = p->vmkstatsPerfCtrEvent;
   } else {
      // use default value, event
      value = sampler.resetLo;
      event = counter->escrVal;
   }

   // ensure value consistent with period
   // (e.g. recover from reading value immediately after wraparound)
   if (value < sampler.resetLo) {
      value = sampler.resetLo;
   }

   // possibly clear missed events
   if (p->vmkstatsClearStats) {
      p->vmkstatsMissedEvents = 0;
      p->vmkstatsClearStats = FALSE;
   }

   // enable sampling
   if (sampler.valid) {
      p->vmkstatsMissingEvents = FALSE;
      PerfCtrWriteCounter(counter, value, sampler.resetHi);
      PerfCtrWriteEvtSel(counter, event, counter->cccrVal);
      memcpy((PerfCtr_Counter*) &myPRDA.samplerCounter, counter, sizeof(PerfCtr_Counter));
   }
}

/*
 *----------------------------------------------------------------------
 *
 * NMISamplerDisable --
 *
 *      Disables the sampler (prevents it from generating any interrupts),
 *	and records the current value to be restored when NMISamplerEnable
 *	is called.
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
NMISamplerDisable(void)
{
   volatile PRDA *p = &myPRDA;
   uint32 escr, cccr;
   PerfCtr_Counter *ctr = (PerfCtr_Counter*) &myPRDA.samplerCounter;

   // preserve sampling params
   if (vmkernelLoaded) {
      uint64 value;
      
      // preserve current counter value      
      value = PerfCtrReadCounter(ctr);
      p->vmkstatsPerfCtrValue = (uint32) (value & 0xffffffff);
   }

   // possibly track events that happen while the sampler is disabled
   if (vmkernelLoaded
       && cpuType == CPUTYPE_INTEL_PENTIUM4
       && NMI_TRACK_LOST_PERF_EVENTS
       && p->configNMI == NMI_USING_SAMPLER
       && sampler.valid) {
      // tell this counter not to generate PMIs any more
      // by masking off "PMI on overflow" bits
      cccr = ctr->cccrVal & ~(
         PERFCTR_PENTIUM4_CCCR_OVF_PMI_T1 |
         PERFCTR_PENTIUM4_CCCR_OVF_PMI_T0);
      escr = p->vmkstatsPerfCtrEvent;
      PerfCtrWriteEvtSel(ctr, escr, cccr);

      // reset value counter to 0
      PerfCtrWriteCounter(ctr, 0, 0);
      p->vmkstatsMissingEvents = TRUE;
   } else {
      PerfCtrWriteEvtSel(ctr, 0, 0);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * NMISamplerInterrupt --
 *
 *      Records the current sample.
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
NMISamplerInterrupt(NMIContext *nmiContext)
{
   Bool configChanged = FALSE;
   uint32 reset, event;
   uint8 threadNum;
   PerfCtr_Counter *counter;

   threadNum = SMP_GetHTThreadNum(MY_PCPU);
   counter = &sampler.counters[threadNum];

   // process sample, if possible
   if (vmkernelLoaded) {
      volatile PRDA *p = &myPRDA;

      // initialize PRDA, if necessary
      if (p->vmkstatsConfig != sampler.config) {
         NMISamplerPRDAConfig(p);
         configChanged = TRUE;
      }

      // sampling action: update vmkstats
      VMKStats_Sample(nmiContext);

      // obtain event, reset value
      reset = p->vmkstatsPerfCtrReset;
      event = p->vmkstatsPerfCtrEvent;
   } else {
      // use default event, reset
      reset = sampler.resetLo;
      event = counter->escrVal;
   }

   // used to keep average execution times of the sampler handler.  Only done
   // on pcpu 0 to avoid races.
   if (MY_PCPU == 0) {
      nmiCurrentTotalCyclesSampling += PerfCtrReadCounter(counter);
      nmiCurrentNumSamples++;
   }
      
   /*
    * reset counter and the control regs if necessary. 
    */

   // On P4, clear OVF in cccr each time, else hang from apparent repeated NMIs
   if (configChanged || (cpuType == CPUTYPE_INTEL_PENTIUM4)) {
      PerfCtrWriteEvtSel(counter, event, counter->cccrVal);
   }
   myPRDA.vmkstatsMissingEvents = FALSE;
   PerfCtrWriteCounter(counter, reset, sampler.resetHi);
}

/*
 *----------------------------------------------------------------------
 *
 * NMI_GetAverageHandlerCycles --
 *
 * Results:
 *      The average number of cycles that the NMI interrupt handler takes.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
uint32 
NMI_GetAverageSamplerCycles()
{
   if (nmiCurrentNumSamples == 0) {
      return 0;
   } else {
      return (uint32) (nmiCurrentTotalCyclesSampling / nmiCurrentNumSamples);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * NMI_ResetAverageSamplerCycles --
 *
 *      Resets the counters that keep track of the average NMI handler 
 *	execution time.
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
NMI_ResetAverageSamplerCycles()
{
   nmiCurrentNumSamples = 0;
   nmiCurrentTotalCyclesSampling = 0;
}


/*
 * NMI watchdog operations
 */

/*
 *----------------------------------------------------------------------
 *
 * NMIWatchdogPerCpuStart --
 *
 *      Called for each pcpu to start the watchdog.  Sets up the hardware
 *	performance counters.
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
NMIWatchdogPerCpuStart(void)
{
   PerfCtr_Config *config = &watchdog;
   PerfCtr_Counter *ctr = &watchdog.counters[SMP_GetHTThreadNum(MY_PCPU)];

   // specify NMI interrupt mode for performance counters
   APIC_PerfCtrSetNMI();

   PerfCtrWriteEvtSel(ctr, 0, 0);
   PerfCtrWriteCounter(ctr, config->resetLo, config->resetHi);
   Log("activated watchdog, resetlo=0xx%x, resethi=0x%x", 
       config->resetLo, 
       config->resetHi);
}

static void
NMIWatchdogDisable(void)
{
   // disable watchdog counter
   PerfCtr_Counter *ctr = &watchdog.counters[SMP_GetHTThreadNum(MY_PCPU)];
   PerfCtrWriteEvtSel(ctr, 0, 0);
}

static void
NMIWatchdogEnable(void)
{
   // enable watchdog counter
   PerfCtr_Counter *ctr = &watchdog.counters[SMP_GetHTThreadNum(MY_PCPU)];
   PerfCtrWriteEvtSel(ctr, ctr->escrVal, ctr->cccrVal);
}

/*
 *----------------------------------------------------------------------
 *
 * NMIWatchdogInterrupt --
 *
 *      Process an NMI interrupt.  If the timer has stopped going off
 *	then enter the debugger.  If another CPU has detected a hang
 *	then backtrace the stack and spin waiting for the other CPU to
 *	exit the debugger.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      State in the prda is updated.
 *
 *----------------------------------------------------------------------
 */
static void
NMIWatchdogInterrupt(uint32 cs, uint32 eip, uint32 esp, uint32 ebp)
{
   static Bool hangPanic;
   PerfCtr_Counter *ctr = &watchdog.counters[SMP_GetHTThreadNum(MY_PCPU)];

   if (vmkernelLoaded) {
      unsigned char ch;   
      int id = APIC_GetPCPU();
      PRDA *p = prdas[id];

      if (p->currentTicks == p->previousTicks) {
	 p->hungCount++;
	 if (!hangPanic && p->hungCount >= MAX_HANG_COUNTER) {
	    hangPanic = TRUE;
	    Panic("CPU %d not responding: "
                  "cs=0x%x eip=0x%x esp=0x%x ebp=0x%x, ticks=%d\n",
                  id, (uint32)cs, eip, esp, ebp, p->currentTicks);
	    // Debug_Break();
	    hangPanic = FALSE;
	    p->hungCount = 0;
	 } else {
	    Warning("CPU %d is not taking timer interrupts (%d)"
		    "\tcs=0x%x eip=0x%x esp=0x%x ebp=0x%x, TSC=0x%Lx",
                    id, p->hungCount, 
                    (uint32)cs, eip, esp, ebp, RDTSC());
	 }
      } else {
	 p->previousTicks = p->currentTicks;
	 p->hungCount = 0;
      }
      p->perfCounterInts++;
      if (hangPanic || p->perfCounterInts % 10 == 0) {
         int backtracePeriod = CONFIG_OPTION(WATCHDOG_BACKTRACE);
	 LOG(0, "id %d: %d %d cs=0x%x eip=0x%x esp=0x%x ebp=0x%x", 
	     id, p->perfCounterInts, p->currentTicks,
             (uint32)cs, eip, esp, ebp);
#ifdef notdef
	 if (id == HOST_PCPU) {
	    Host_DumpIntrInfo(0);
	 }
#endif
         if ((cs == DEFAULT_CS) && backtracePeriod &&
             ((p->perfCounterInts / 10 % backtracePeriod) == 0)) {
            Util_Backtrace(eip, ebp, _Log, FALSE);
         }
      }
      p->lastEIP = eip;
      p->lastESP = esp;
      p->lastEBP = ebp;
      if (hangPanic) {
	 Warning("CPU %d spinning waiting for other CPUs to resume: eip=0x%x ebp=0x%x",
                 id, eip, ebp);
         if (cs == DEFAULT_CS) {
            Util_Backtrace(eip, ebp, _Log, FALSE);
         }
	 while (hangPanic) {
	 }
	 p->hungCount = 0;
      } else if (Debug_InDebugger()) {
	 Warning("a CPU is in the debugger - CPU %d waiting to resume", id);
         if (cs == DEFAULT_CS) {
            Util_Backtrace(eip, ebp, _Log, FALSE);
         }
	 while (Debug_InDebugger()) {
	 }
      }

      ch = Serial_PollChar();
      if (ch == 3) {
	 Log("Entering debugger with cs=0x%x eip=0x%x esp=0x%x ebp=0x%x", 
	     (uint32)cs, eip, esp, ebp);
         if (cs == DEFAULT_CS) {
            Util_Backtrace(eip, ebp, _Log, FALSE);
         }
	 Debug_Break();
      }
   }

   // On P4, clear OVF in cccr each time, else hang from apparent repeated NMIs
   if (cpuType == CPUTYPE_INTEL_PENTIUM4) {
      PerfCtrWriteEvtSel(ctr, ctr->escrVal, ctr->cccrVal);
   }

   PerfCtrWriteCounter(ctr, watchdog.resetLo, watchdog.resetHi);
}

/*
 *----------------------------------------------------------------------
 *
 * NMI_WatchdogTurnOn --
 *
 *      Turns on the watchdog by setting per-cpu flags that will be checked
 *	next time NMI_Enable is called.
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
NMIWatchdogTurnOn(void)
{
   int i;
   SP_IRQL prevIRQL;
   PerfCtr_Config *config = &watchdog;
   PerfCtr_Counter *ctr = &watchdog.counters[0];

   if (myPRDA.configNMI) {
      Warning("error, either watchdog or sampler is already running");
      return;
   } 
   
   // disable watchdog counter, set default period
   switch (cpuType) {
   case CPUTYPE_INTEL_P6:
      ctr->index = P6_NMI_WATCHDOG_CTR;
      ctr->addr = MSR_PERFCTR0 + P6_NMI_WATCHDOG_CTR;
      ctr->escrVal = PERFCTR_P6_CPU_CLK_UNHALTED |
	 PERFCTR_P6_USER_MODE        | 
	 PERFCTR_P6_KERNEL_MODE      |
	 PERFCTR_P6_ENABLE           |
	 PERFCTR_P6_APIC_INTR;
      ctr->cccrVal = 0; // Pentium4-only MSR
      ctr->escrAddr = MSR_EVNTSEL0 + P6_NMI_WATCHDOG_CTR;
      ctr->cccrAddr = 0; // Pentium4-only MSR
      
      config->resetLo = NMI_WATCHDOG_RESET; 
      config->resetHi = 0; // sign extended in the MSR for P6 family
      break;
      
   case CPUTYPE_INTEL_PENTIUM4:
      prevIRQL = SP_LockIRQ(&perfCtrLock, SP_IRQL_KERNEL);
      if (NMIPentium4MakePCMSRs(config, 
				"cycles", 
				NMI_WATCHDOG_PERIOD) != VMK_OK) {
	 Warning("failed to configure watchdog properly!");
      }

      PERFCTR_PENTIUM4_CCCR_SET_THRESHOLD(watchdog.counters[0].cccrVal, 0xf);
      PERFCTR_PENTIUM4_CCCR_SET_THRESHOLD(watchdog.counters[1].cccrVal, 0xf);
      
      watchdog.counters[0].cccrVal |= PERFCTR_PENTIUM4_CCCR_COMPARE |
	 PERFCTR_PENTIUM4_CCCR_COMPLEMENT;
      watchdog.counters[1].cccrVal |= PERFCTR_PENTIUM4_CCCR_COMPARE |
	 PERFCTR_PENTIUM4_CCCR_COMPLEMENT;
      
      SP_UnlockIRQ(&perfCtrLock, prevIRQL);
      Log("setup watchdog counter");
      break;

   case CPUTYPE_AMD_ATHLON:
   case CPUTYPE_AMD_DURON:
   case CPUTYPE_OTHER:
   case CPUTYPE_UNSUPPORTED:
      Warning("Can't do NMI tracing on non Intel P6/Pentium4 processors");
      return;
   }

   config->samplesPerSec = NMIComputeSamplesPerSec(NMI_WATCHDOG_PERIOD);
   config->period = NMI_WATCHDOG_PERIOD;
   config->config = 1;

   for (i=0; i<numPCPUs; i++) {
      prdas[i]->configNMI = NMI_SETUP_WATCHDOG;
   }
}

static int
NMIWatchdogProcRead(Proc_Entry *entry, char *buffer, int *len)
{
   *len = 0;
   if (myPRDA.configNMI == NMI_SETUP_WATCHDOG ||
       myPRDA.configNMI == NMI_USING_WATCHDOG) {
      Proc_Printf(buffer, len, "watchdog enabled and running.\n");
   } else {
      Proc_Printf(buffer, len, 
		  "watchdog is off. To turn it on run:\n"
		  "  echo start > /proc/vmware/watchdog\n");
   }
   return(0);
}

static int
NMIWatchdogProcWrite(Proc_Entry *entry, char *buffer, int *len)
{
   char *argv[2];
   int argc;

   // parse buffer into args (assumes OK to overwrite buffer)
   argc = Parse_Args(buffer, argv, 2);
   if ((argc == 1) && (strcmp(argv[0], "start") == 0)) {
      NMIWatchdogTurnOn();
   } else {
      Warning("invalid argument.");
      return(VMK_BAD_PARAM);
   }

   return(VMK_OK);
}


/*
 * NMI operations
 */


/*
 *----------------------------------------------------------------------
 *
 * NMI_TaskToNMIContext --
 *
 *      Fills in an NMIContext struct from a task struct
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	modifies nmiContext
 *
 *----------------------------------------------------------------------
 */
void
NMI_TaskToNMIContext(const Task *task,      //IN
		     NMIContext *nmiContext) //OUT
{
   nmiContext->eip = task->eip;
   nmiContext->cs = task->cs;
   nmiContext->esp = task->esp;
   nmiContext->ss = task->ss;
   nmiContext->ebp = task->ebp;
   nmiContext->eflags = task->eflags;
}


/*
 *----------------------------------------------------------------------
 *
 * NMI_Init --
 *
 *      Initialize NMIs for the current processor.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	Starts performance counter sampling, if configured.
 *      Starts watchdog timer, if configured.
 *
 *----------------------------------------------------------------------
 */
void
NMI_Init(void)
{
   Proc_InitEntry(&watchdogProc);
   watchdogProc.read = NMIWatchdogProcRead;
   watchdogProc.write = NMIWatchdogProcWrite;
   Proc_RegisterHidden(&watchdogProc, "watchdog", FALSE);
   NMIAllowed = TRUE;

   SP_InitLockIRQ("perfCtrLock", &perfCtrLock, SP_RANK_VMKPERF_USEDCOUNTER - 2);

   // initialize global sampler configuration
   memset(&sampler, 0, sizeof(sampler));
   sampler.counters[0].index = INVALID_COUNTER_SENTRY;
   sampler.counters[1].index = INVALID_COUNTER_SENTRY;
}

/*
 *----------------------------------------------------------------------
 *
 * NMI_IsEnabled --
 *
 *
 * Results:
 *      Returns true if NMIs are currently enabled.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
Bool
NMI_IsEnabled(void)
{
   return (myPRDA.nmisEnabled);
}

/*
 *----------------------------------------------------------------------
 *
 * NMIEnableInt --
 *
 *      Enable NMIs on the current processor.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	Enables performance counter sampling interrupts, if configured.
 *      Enables watchdog timer interrupts, if configured.
 *
 *----------------------------------------------------------------------
 */
void
NMIEnableInt(void)
{
   // ASSERT(!myPRDA.nmisEnabled);
   if (myPRDA.nmisEnabled) {
      return;
   }

   if (!NMIAllowed) {
      return;
   }

   myPRDA.nmisEnabled = TRUE;

   if (myPRDA.configNMI == NMI_USING_WATCHDOG) {
      NMIWatchdogEnable();
   } else if (myPRDA.configNMI == NMI_SETUP_WATCHDOG) {
      myPRDA.configNMI = NMI_USING_WATCHDOG;
      NMIWatchdogPerCpuStart();
      NMIWatchdogEnable();
   } else if (myPRDA.configNMI == NMI_USING_SAMPLER) {
      NMISamplerEnable();
   } else if (myPRDA.configNMI == NMI_SETUP_SAMPLER) {
      myPRDA.configNMI = NMI_USING_SAMPLER;
      NMISamplerPerCpuStart();
      NMISamplerEnable();
   }

   // need to unmask the so-called "non maskable interrupts"
   NMI_Unmask();
}

/*
 *----------------------------------------------------------------------
 *
 * NMIDisableInt --
 *
 *      Disable NMIs on the current processor.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	Disables performance counter sampling interrupts.
 *      Disables watchdog timer interrupts.
 *
 *----------------------------------------------------------------------
 */
void
NMIDisableInt(void)
{
   /*
    * Need to mask the so-called "non maskable interrupts".
    *
    * This should be the first thing we do in this function so that we
    * don't get NMIs in the middle of trying to disable them.
    */
   
   NMI_Mask();
   myPRDA.nmisEnabled = FALSE;

#if 0
   //make sure that nmis were enabled before calling disable 
   if((!Debug_InDebugger()) && !(myPRDA.configNMI & NMI_CURRENTLY_ON_MASK)) {
      Log("last caller of disable: 0x%x",lastDisableCaller);
      ASSERT(FALSE);
   }
   lastDisableCaller = (uint32)__builtin_return_address(0);
#endif

   if (myPRDA.configNMI == NMI_USING_WATCHDOG) {
      // disable watchdog timer
      NMIWatchdogDisable();
   } else if (myPRDA.configNMI == NMI_DISABLING_SAMPLER) {
      // transition to "OFF" state
      NMISamplerDisable();
      myPRDA.configNMI = NMI_OFF;
      Log("disabled sampler");
   } else if (myPRDA.configNMI == NMI_USING_SAMPLER) {
      // disable sample collection
      NMISamplerDisable();
   }
}

/*
 *----------------------------------------------------------------------
 *
 * NMI_Disallow --
 *
 * 	Disallow (perf) NMIs.
 *
 * Results:
 * 	None.
 *
 * Side Effects:
 * 	Perf NMIs can no longer be enabled
 *
 *----------------------------------------------------------------------
 */
void
NMI_Disallow(void)
{
   NMIAllowed = FALSE;
   NMI_Disable();
}

/*
 *----------------------------------------------------------------------
 *
 * NMI_Interrupt --
 *
 *      Handle NMI interrupt on the current processor.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	Processes performance counter sampling interrupts, if any.
 *      Processes watchdog timer interrupts, if any.
 *
 *----------------------------------------------------------------------
 */
void
NMI_Interrupt(NMIContext *nmiContext)
{
   PerfCtr_Counter *watchdogCounter = &watchdog.counters[SMP_GetHTThreadNum(MY_PCPU)];

   if (myPRDA.configNMI == NMI_DISABLING_SAMPLER) {
      return;
   }

   LOG(1, "Yo! eip = %d", nmiContext->eip);
   if (myPRDA.configNMI != NMI_USING_SAMPLER &&
       myPRDA.configNMI != NMI_USING_WATCHDOG) {
      uint8 nmiReason = INB(0x61);
      
      NMI_Pending = TRUE;
      
      if (nmiReason & 0x80) {
	 SysAlert("Interrupt @ 0x%x:0x%x Memory Parity Error (0x%x)",
		  nmiContext->cs, nmiContext->eip, nmiReason);
      } else if (nmiReason & 0x40) {
	 SysAlert("Interrupt @ 0x%x:0x%x IO Check Error (0x%x)",
		  nmiContext->cs, nmiContext->eip, nmiReason);
      } else if (nmiReason) {
	 SysAlert("Interrupt @ 0x%x:0x%x Unknown Error (0x%x)",
		  nmiContext->cs, nmiContext->eip, nmiReason);
      }
      if (nmiContext->cs == DEFAULT_CS) {
         Util_Backtrace(nmiContext->eip, nmiContext->ebp, _Log, FALSE);
      }
      
      return;
   } else {
      Bool gotNMIMatch = FALSE;

      ASSERT(myPRDA.nmisEnabled);

      if (myPRDA.configNMI == NMI_USING_WATCHDOG) {
	 if (PERFCTR_CHECK_OVERFLOW(watchdogCounter->index)) {
	    NMIWatchdogInterrupt(nmiContext->cs, nmiContext->eip, 
                                 nmiContext->esp, nmiContext->ebp);
	    if (myPRDA.configNMI == NMI_USING_SAMPLER) {
	       gotNMIMatch = TRUE;
	    }
	 }
      }

      if (myPRDA.configNMI == NMI_USING_SAMPLER) {
	 uint32 samplerIndex;
         PerfCtr_Counter *samplerCounter = 
            &sampler.counters[SMP_GetHTThreadNum(MY_PCPU)];

         samplerIndex = samplerCounter->index;

	 if (PERFCTR_CHECK_OVERFLOW(samplerIndex)) {
	    NMISamplerInterrupt(nmiContext);
	    gotNMIMatch = TRUE;
	 }

	 /*
	  * On the P3, the perf ctrs used for watchdog & sampling do
	  * not change at runtime, so getting an unaccounted for NMI
	  * is odd & we issue a warning.  On the P4, the perf ctr used
	  * for watchdog does not change at runtime, but the perf ctr
	  * used for sampling may, since different events are
	  * restricted to different perf ctrs.  Should the sampling
	  * perf ctr change, there is a transition period during which
	  * a cpu may deliver an NMI for a perf ctr that is no longer
	  * the one stored in the sampler data structure & if the OVF
	  * flag of this orphan perf ctr is not cleared, the repeated
	  * NMIs hang problem [mentioned in NMISamplerInterrupt] will
	  * occur.
	  */
	 if (gotNMIMatch == FALSE) {
	    if (cpuType == CPUTYPE_INTEL_PENTIUM4) {
	       uint32 pCtrInd;

	       for (pCtrInd = 0; pCtrInd < PERFCTR_PENTIUM4_NUM_PERFCTRS; 
                    pCtrInd++) {
                  // how is this a concern when we can't have watchdog
                  // and sampler at the same time?? --JRZ
		  if ((pCtrInd != samplerIndex) && 
                      ((myPRDA.configNMI == NMI_USING_WATCHDOG) && 
                       (pCtrInd != watchdogCounter->index))) {
		     WRMSR(PERFCTR_PENTIUM4_CCCR_BASE_ADDR + pCtrInd, 0, 0);
		  }
	       }
	    } else {
	       Warning("Unexplained NMI Interrupt @ 0x%x:0x%x", 
                       nmiContext->cs, nmiContext->eip);
	    }
	 }
      }
      NMI_Unmask();
   }
}


/*
 *----------------------------------------------------------------------
 *
 * NMI_Unmask --
 *
 * 	Unmask perfcounter NMIs
 *      NOTE: this function is called with COS addrspace, so no kseg/prda
 *
 * Results:
 * 	None.
 *
 * Side Effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
void
NMI_Unmask(void)
{
   APIC_PerfCtrUnmask();
}

/*
 *----------------------------------------------------------------------
 *
 * NMI_Mask --
 *
 * 	Mask perfcounter NMIs and return whether they were enabled
 *      NOTE: this function is called with COS addrspace, so no kseg/prda
 *
 * Results:
 * 	TRUE if NMIs were enabled
 *
 * Side Effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */

Bool
NMI_Mask(void)
{
   return APIC_PerfCtrMask();
}


/*
 *----------------------------------------------------------------------
 *
 * NMI_PatchTask --
 *
 *      Patches the interrupted task to execute a little bit of code to
 *      clear TS.
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *      Alters the execution path of the interrupted task.
 *
 *----------------------------------------------------------------------
 */

void
NMI_PatchTask(Task *task)
{
   int stackIdx = NMI_PATCH_STACK_SIZE - 1;

   ASSERT(User_SegInUsermode(task->cs));

   /* We interrupted a task at CPL3 - use IRET to switch stacks after
      we CLTS at CPL0. */

   myPRDA.nmiPatchStack[stackIdx--] = task->ss;
   myPRDA.nmiPatchStack[stackIdx--] = task->esp;
   myPRDA.nmiPatchStack[stackIdx--] = task->eflags;
   myPRDA.nmiPatchStack[stackIdx--] = task->cs;
   myPRDA.nmiPatchStack[stackIdx]   = task->eip;
   ASSERT(stackIdx >= 0);
   
   task->ss  = DEFAULT_SS;
   task->esp = (uint32)&myPRDA.nmiPatchStack[stackIdx];
   task->cs  = DEFAULT_CS;
   task->eip = (uint32)CommonNMIIret;
   task->eflags = 0;
}
