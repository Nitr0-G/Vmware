/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * userStatDef.h --
 *
 *	UserWorld statistics struct definitions
 *
 *	See userStat.h for usage macro defintions and prototypes.
 *	This is just the definition of the stats to collect and the
 *	structure they sit in.
 *
 *	OVERVIEW:
 *
 *	There are 4 kinds of stats: COUNTER, ARRAY, HISTOGRAM and
 *	TIMER.  COUNTER is a simple counter, you can add values to it.
 *	ARRAY is an array of counters.  All elements are 64-bit
 *	unsigned ints.  HISTOGRAM stats track the min/max/mean and
 *	number of values inserted into it, plus a histogram of the
 *	values is also recorded (also 64-bit).  TIMER stats start and
 *	stop a timer, and record the delta in a histogram (again,
 *	64-bit cycle counts).
 *
 *	HISTOGRAM and TIMER stats require an initializer array to size
 *	the histogram buckets.  Values in the initializer array should
 *	be monotonically increasing.  Use as many or few as you want.
 *
 *	TIMER records start/stop cycle counts and puts them in a
 *	histogram.  Timers are implicitly thread-private (same thread
 *	starts and stops it), multiple threads can have timers going,
 *	and threads can be involved in multiple timers.  Timers do not
 *	recurse, however.  If STOP isn't invoked, that's okay.  The
 *	next start invocation on that timer will overwrite the
 *	previous start.
 *
 *	Stats are defined by adding a single line to
 *	USERSTAT_STATLIST.  Use the UWSTAT_DECLARE_* macro for the
 *	appropriate type.
 *
 *	Stats are automatically tracked at thread and cartel
 *	granularity.  Stats are propogated to a global level when the
 *	cartels are terminated.  The only exception is stats calls
 *	invoked by helper worlds (e.g., in starting the first cartel).
 *	Those are recorded in the 'other' stats struct.  Thus, a
 *	snapshot of current stats should include all active cartel
 *	stats, plus the 'other' stats, plus the (saved) global stats.
 *	Note that timers invoked by non-userworlds are completely
 *	ignored (there is no good place to save the timer start
 *	timestamp), so startup and shutdown of a cartel thus have
 *	sub-par stat tracking.
 *
 *	To record a stat, use the type-appropriate functions:
 *
 *	Counters:
 *	UWSTAT_ADD(stat, val): add val to counter 'stat'
 *	UWSTAT_INC(stat): add 1 to counter 'stat'
 *
 *	Arrays:
 *	UWSTAT_ARRADD(stat, idx, val): add val to 'stat[idx]'
 *	UWSTAT_ARRINC(stat, idx): add 1 to 'stat[idx]'
 *
 *	Histograms:
 *	UWSTAT_INSERT(stat, val): record val in histogram 'stat'
 *
 *	Timers:
 *	UWSTAT_START(stat): start a timer on stat (per-thread)
 *	UWSTAT_STOP(stat): end timer on stat, record in histogram
 *
 * TODO:
 *	Include a description with each stat.
 *
 *	Generate from and/or with the vmksysinfo interfaces.
 *
 *      Factor out timers so they can be kept on objects (e.g.,
 *      measure inter-poll gaps on fd objs).
 *
 *	Add a sparse array type
 */

#ifndef VMKERNEL_USER_USERSTATDEF_H
#define VMKERNEL_USER_USERSTATDEF_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "vm_basic_types.h"
#include "splock.h"
#include "histogram.h"
#include "proc.h"

#define MILLION (1000 * 1000ULL)
#define BILLION (1000 * MILLION)

/* Useful initialization array for histograms of byte sizes */
#define UWSTAT_SIZES_INIT {8,16,32,64,128,256,512,1024,4096,8192,32768}

/*
 * Can't use actual proxy upcall count because of recursive #includes.
 * However, ASSERTs in stat init will check this.
 */
#define UWSTAT_PROXYUPCALLCT 50

/*
 * UWSTAT_DECLARE_COUNTER(statName)
 * UWSTAT_DECLARE_ARRAY(statName, arraySize)
 * UWSTAT_DECLARE_HISTOGRAM(statName, initializer)
 * UWSTAT_DECLARE_TIMER(statName, initializer)
 */
#define USERSTAT_STATSLIST \
/* general stats */ \
        UWSTAT_DECLARE_ARRAY(linuxSyscallCount, 280) \
        UWSTAT_DECLARE_ARRAY(uwvmkSyscallCount, 60) \
        UWSTAT_DECLARE_COUNTER(exceptions) \
        UWSTAT_DECLARE_COUNTER(userSocketInetPollCallback) \
        UWSTAT_DECLARE_TIMER(US, pageFaultHandleTime, \
                             {1000, 100*1000, MILLION, 10*MILLION, \
                              BILLION, 100*BILLION}) \
        UWSTAT_DECLARE_COUNTER(userCopyFaults) \
        UWSTAT_DECLARE_HISTOGRAM(writevSizes, UWSTAT_SIZES_INIT) \
        UWSTAT_DECLARE_HISTOGRAM(copyInSizes, UWSTAT_SIZES_INIT) \
        UWSTAT_DECLARE_HISTOGRAM(copyOutSizes, UWSTAT_SIZES_INIT) \
        UWSTAT_DECLARE_TIMER(US, waitTimes, \
                             {1000, 10*1000, 100*1000, MILLION, \
                              10*MILLION, BILLION}) \
/* signal stats */ \
        UWSTAT_DECLARE_COUNTER(pendingSigsInt) \
        UWSTAT_DECLARE_ARRAY(signalsSent, 64) \
/* fdobj stats */ \
        UWSTAT_DECLARE_ARRAY(userObjCreated, 32) \
        UWSTAT_DECLARE_ARRAY(userObjDestroyed, 32) \
        UWSTAT_DECLARE_HISTOGRAM(pollFdCount, {2,4,8,16,32,64,256,512}) \
/* pipe stats */ \
        UWSTAT_DECLARE_HISTOGRAM(pipeReadSizes, UWSTAT_SIZES_INIT) \
        UWSTAT_DECLARE_HISTOGRAM(pipeWriteSizes, UWSTAT_SIZES_INIT) \
/* proxy stats */ \
        UWSTAT_DECLARE_COUNTER(proxyRPCSleepMs) \
        UWSTAT_DECLARE_HISTOGRAM(proxyRPCSendLoopCt, {0,1,2,4,8,16,32,64}) \
        UWSTAT_DECLARE_ARRAY(proxySyscallCount, UWSTAT_PROXYUPCALLCT) \
        UWSTAT_DECLARE_ARRAY(proxyBytesSent, UWSTAT_PROXYUPCALLCT) \
        UWSTAT_DECLARE_ARRAY(proxyBytesRecv, UWSTAT_PROXYUPCALLCT) \
        UWSTAT_DECLARE_COUNTER(proxyObjFindMissCt) \
        UWSTAT_DECLARE_COUNTER(proxyCancelMsgCt) \
        UWSTAT_DECLARE_HISTOGRAM(proxyObjFindHitCt, {4,8,16,24,32,64,128,256}) \
        UWSTAT_DECLARE_HISTOGRAM(proxyCopyInVMK, UWSTAT_SIZES_INIT) \
        UWSTAT_DECLARE_HISTOGRAM(proxyCopyInUser, UWSTAT_SIZES_INIT) \
        UWSTAT_DECLARE_HISTOGRAM(proxyCopyOutVMK, UWSTAT_SIZES_INIT) \
        UWSTAT_DECLARE_HISTOGRAM(proxyCopyOutUser, UWSTAT_SIZES_INIT) \
        UWSTAT_DECLARE_HISTOGRAM(proxyRPCsPerMessage, \
                                 {1,2,4,8,16,32,64,128,256,512,1024,2048,4096})\
        UWSTAT_DECLARE_TIMER(US, proxyCallTime, \
                             {1000, 10*1000, 100*1000, MILLION, \
                              10*MILLION, BILLION, 100*BILLION}) \
/* mem/paging */ \
        UWSTAT_DECLARE_COUNTER(userMemCartelFlushes) \
        UWSTAT_DECLARE_COUNTER(mmapExtendHitCount) \
        UWSTAT_DECLARE_COUNTER(mmapExtendMissCount) \
        UWSTAT_DECLARE_COUNTER(mmapSplitCount) \
/* end of list */


#if defined(VMX86_DEBUG) || defined(VMX86_DEVEL) || defined(VMX86_STATS)
#define USERSTAT_ENABLED 1
#else
#undef USERSTAT_ENABLED
#endif


#if defined(USERSTAT_ENABLED) // {

#define UWSTAT_ONLY(x) x

/*
 * The type used for tracking TIMER stats.
 */
typedef struct UserStat_Timer {
   Timer_AbsCycles  start; // only used in thread stats
   Histogram_Handle results;
} UserStat_Timer;

/*
 * Setup Hutchins Macros to define type declarations for each type of
 * stat.
 */
#define UWSTAT_DECLARE_ARRAY(NAME, SIZE) \
	uint64 NAME[SIZE];
#define UWSTAT_DECLARE_COUNTER(NAME) \
	uint64 NAME;
#define UWSTAT_DECLARE_HISTOGRAM(NAME, init... ) \
	Histogram_Handle NAME;
#define UWSTAT_DECLARE_TIMER(units, NAME, init... ) \
	UserStat_Timer NAME;

/*
 * Record (noun, not the verb) of stats.  The same struct is used
 * for global, other, ignored, cartel, and thread stats.
 */
typedef struct UserStat_Record {
   SP_SpinLock lock;            // global and other only

   /* Expand Hutchins Macro to fill out the typedef with each stat. */
   USERSTAT_STATSLIST

   Proc_Entry  procDir;         // global and cartel only
   Proc_Entry  procEntry;
} UserStat_Record;

#undef UWSTAT_DECLARE_ARRAY
#undef UWSTAT_DECLARE_COUNTER
#undef UWSTAT_DECLARE_HISTOGRAM
#undef UWSTAT_DECLARE_TIMER

#else
/*
 * UserStat not enabled.
 */

typedef void* UserStat_Record;

#define UWSTAT_ONLY(x)

#endif /* USERSTAT_ENABLED */

#endif /* VMKERNEL_USER_USERSTATDEF_H */
