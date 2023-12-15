/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * memMetrics.c --
 *
 *	Load metrics for memory resources.
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "vmkernel.h"
#include "timer.h"
#include "sched.h"
#include "memsched.h"
#include "memMetrics.h"
#include "schedMetrics.h"

#define	LOGLEVEL_MODULE	MemMetrics
#include "log.h"

/*
 * Compile-time options
 */

#define	MEMMETRICS_DEBUG		(vmx86_debug && vmx86_devel)
#define	MEMMETRICS_DEBUG_VERBOSE	(0)

/*
 * Constants
 */

// moving-average constants
//   MEMMETRICS_EXP_m = 2^p / 2^((s lg e) / 60 m)
//   where s = inter-sample period, in seconds
//         m = load averaging period, in minutes
//         p = precision, in bits
// See <http://www.teamquest.com/html/gunther/ldavg1.shtml> for
// a detailed explanation of this formula and other magic.
#define	MEMMETRICS_PERIOD_MS	(2000)
#define	MEMMETRICS_EXP_1	(3962)
#define	MEMMETRICS_EXP_5	(4069)
#define	MEMMETRICS_EXP_15	(4087)

/*
 * Types
 */

typedef struct {
   FixedAverages overcommit;
   FixedAverages free;
   FixedAverages reclaim;
   FixedAverages balloon;
   FixedAverages swap;
} LoadAverages;

typedef struct {
   SP_SpinLock lock;
   Timer_Handle timer;
   LoadAverages averages;
   FixedAverageDecays decays;
   Proc_Entry proc;
} MemMetrics;

/*
 * Globals
 */

MemMetrics memMetrics;


/*
 * Operations
 */

/*
 *-----------------------------------------------------------------------------
 *
 * MemMetricsPeriodic --
 *
 *      Timer-based callback to perform periodic load metrics computations,
 *	such as maintaining moving averages.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Modifies global "memMetrics" state.
 *
 *-----------------------------------------------------------------------------
 */
static void
MemMetricsPeriodic(UNUSED_PARAM(void *ignore),
                   UNUSED_PARAM(Timer_AbsCycles timestamp))
{
   LoadAverages *load = &memMetrics.averages;
   FixedAverageDecays *decays = &memMetrics.decays;
   MemSched_LoadMetrics m;

   // snapshot current load metrics
   MemSched_GetLoadMetrics(&m);

   // update averages
   SP_Lock(&memMetrics.lock);
   FixedAveragesUpdate(&load->overcommit, decays, m.overcommit);
   FixedAveragesUpdate(&load->free, decays, m.free);
   FixedAveragesUpdate(&load->reclaim, decays, m.reclaim);
   FixedAveragesUpdate(&load->balloon, decays, m.balloon);
   FixedAveragesUpdate(&load->swap, decays, m.swap);
   SP_Unlock(&memMetrics.lock);
}


/*
 *-----------------------------------------------------------------------------
 *
 * MemMetricsProcRead --
 *
 *	Formats and writes load information for the memory load
 *	metric with the specified "name" and data "f" into "buf".
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Increments "len" by number of characters written to "buf".
 *
 *-----------------------------------------------------------------------------
 */
static void
MemMetricsFormatLoad(char *buf, int *len,
                     const char *name, FixedAverages *f)
{
   DecimalAverages pct;
   
   FixedAveragesToDecimal(f, &pct);
   Proc_Printf(buf, len,
               "%-10s %6u.%03u %6u.%03u %6u.%03u %6u.%03u\n",
               name,
               pct.value.whole, pct.value.milli,
               pct.avg1.whole,  pct.avg1.milli,
               pct.avg5.whole,  pct.avg5.milli,
               pct.avg15.whole, pct.avg15.milli);
}

/*
 *-----------------------------------------------------------------------------
 *
 * MemMetricsProcRead --
 *
 *     Proc read handler for /proc/vmware/sched/mem-load.
 *
 * Results:
 *     Returns VMK_OK.
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static int
MemMetricsProcRead(Proc_Entry *entry, char *buf, int *len)
{
   LoadAverages load;

   // initialize
   *len = 0;

   // snapshot current metrics
   SP_Lock(&memMetrics.lock);
   load = memMetrics.averages;
   SP_Unlock(&memMetrics.lock);

   // format header
   Proc_Printf(buf, len,
               "percent       current       1min       5min      15min\n");

   // format load metrics
   MemMetricsFormatLoad(buf, len, "overcommit", &load.overcommit);
   MemMetricsFormatLoad(buf, len, "free", &load.free);
   MemMetricsFormatLoad(buf, len, "reclaim", &load.reclaim);
   MemMetricsFormatLoad(buf, len, "balloon", &load.balloon);
   MemMetricsFormatLoad(buf, len, "swap", &load.swap);

   return(VMK_OK);
}

/*
 * Exported operations
 */

/*
 *----------------------------------------------------------------------
 *
 * MemMetrics_Init --
 *
 *	Initializes MemMetrics module.
 *
 * Results:
 *      None. 
 *
 * Side effects:
 *      Modifies global state, registers timer-based callback,
 *	registers proc node.
 *
 *----------------------------------------------------------------------
 */
void
MemMetrics_Init(Proc_Entry *dir)
{
   // zero state
   memset(&memMetrics, 0, sizeof(MemMetrics));

   // initalize exponential weighted moving average decays
   memMetrics.decays.exp1  = MEMMETRICS_EXP_1;
   memMetrics.decays.exp5  = MEMMETRICS_EXP_5;
   memMetrics.decays.exp15 = MEMMETRICS_EXP_15;

   // initialize lock
   SP_InitLock("MemMetrics", &memMetrics.lock, SP_RANK_LEAF);

   // register periodic callback
   memMetrics.timer = Timer_Add(MY_PCPU, MemMetricsPeriodic,
                                MEMMETRICS_PERIOD_MS, TIMER_PERIODIC, NULL);
   
   // register "sched/cpu-load" entry
   Proc_InitEntry(&memMetrics.proc);
   memMetrics.proc.parent = dir;
   memMetrics.proc.read = MemMetricsProcRead;
   Proc_Register(&memMetrics.proc, "mem-load", FALSE);

   // debugging
   LOG(0, "initialized");
}
