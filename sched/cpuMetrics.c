/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * cpuMetrics.c --
 *
 *	Load metrics for CPU resources.
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "vmkernel.h"
#include "timer.h"
#include "world.h"
#include "heap_public.h"
#include "heapsort.h"
#include "cpusched.h"
#include "cpusched_int.h"
#include "sched_int.h"
#include "cpuMetrics.h"
#include "schedMetrics.h"

#define	LOGLEVEL_MODULE	CpuMetrics
#include "log.h"

/*
 * Compile-time options
 */

#define	CPUMETRICS_DEBUG		(vmx86_debug && vmx86_devel)
#define	CPUMETRICS_DEBUG_VERBOSE	(0)

/*
 * Constants
 */

// moving-average constants
//   CPUMETRICS_EXP_m = 2^p / 2^((s lg e) / 60 m)
//   where s = inter-sample period, in seconds
//         m = load averaging period, in minutes
//         p = precision, in bits
// See <http://www.teamquest.com/html/gunther/ldavg1.shtml> for
// a detailed explanation of this formula and other magic.
#define	CPUMETRICS_PERIOD_MS	(2000)
#define	CPUMETRICS_EXP_1	(3962)
#define	CPUMETRICS_EXP_5	(4069)
#define	CPUMETRICS_EXP_15	(4087)

// heap size
#define	CPUMETRICS_HEAP_SIZE_MIN	( 512 * 1024)
#define	CPUMETRICS_HEAP_SIZE_MAX	(2048 * 1024)

// load history name len
#define	LOAD_HISTORY_NAME_LEN	(MAX(WORLD_NAME_LENGTH, SCHED_GROUP_NAME_LEN))

// load history max samples
#define	LOAD_HISTORY_SAMPLES_MAX	(180)

// samples for 1, 5, 15 min at default rate
#define	LOAD_HISTORY_TIMESCALES		(3)
#define	LOAD_HISTORY_TS0		(10)
#define	LOAD_HISTORY_TS1		(50)
#define	LOAD_HISTORY_TS2		(150)

/*
 * Types
 */

typedef struct {
   FixedAverages vcpus;
   FixedAverages vms;
   FixedAverages baseShares;
   FixedAverages eminPct;
   FixedAverages eminMhz;
} LoadAverages;

typedef struct {
   uint32 run;
   uint32 ready;
} LoadHistorySample;

struct CpuMetrics_LoadHistory {
   LoadHistorySample samples[LOAD_HISTORY_SAMPLES_MAX];
   uint32 nSamples;
   Timer_Cycles prevRun;
   Timer_Cycles prevReady;
};

typedef struct {
   uint32 activeQuintile[5];
   uint32 activeMin;
   uint32 activeMax;
   uint32 activeAvg;
   uint32 runAvg;
} LoadHistorySummary;

typedef struct {
   // identity
   World_ID worldID;
   World_ID worldGroupID;
   Sched_GroupID groupID;
   char name[LOAD_HISTORY_NAME_LEN];

   // summary stats
   LoadHistorySummary timeScale[LOAD_HISTORY_TIMESCALES];
   uint32 nSamples;
} CpuMetricsLoadHistorySnap;

typedef struct {
   // module heap
   Heap_ID heap;

   // load average state
   SP_SpinLock loadLock;
   LoadAverages averages;
   FixedAverageDecays decays;
   Proc_Entry procLoad;

   // load history state
   SP_SpinLock loadHistoryLock;
   uint32 loadHistoryIndex;
   Proc_Entry procLoadHistoryDir;
   Proc_Entry procLoadHistoryVcpus;
   Proc_Entry procLoadHistoryVcpusPct;   
   Proc_Entry procLoadHistoryGroups;
   Proc_Entry procLoadHistoryGroupsPct;
   Proc_Entry procDrmStats;
} CpuMetrics;

typedef struct {
   Sched_Group *group[SCHED_GROUPS_MAX];
   uint32 nGroups;
} CpuMetricsAllGroups;

/*
 * Globals
 */

CpuMetrics cpuMetrics;


/*
 * Operations
 */

/*
 *-----------------------------------------------------------------------------
 *
 * CpuMetricsLoadAveragePeriodic --
 *
 *      Timer-based callback to perform periodic load metrics computations,
 *	such as maintaining moving averages.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Modifies global "cpuMetrics" state.
 *
 *-----------------------------------------------------------------------------
 */
static void
CpuMetricsLoadAveragePeriodic(UNUSED_PARAM(void *ignore),
                              UNUSED_PARAM(Timer_AbsCycles timestamp))
{
   LoadAverages *load = &cpuMetrics.averages;
   FixedAverageDecays *decays = &cpuMetrics.decays;
   CpuSched_LoadMetrics m;
   uint32 eminPct, eminMhz;

   // snapshot current load metrics
   CpuSched_GetLoadMetrics(&m);
   eminPct = CpuSched_BaseSharesToUnits(m.baseShares, SCHED_UNITS_PERCENT);
   eminMhz = CpuSched_BaseSharesToUnits(m.baseShares, SCHED_UNITS_MHZ);

   // update averages
   SP_Lock(&cpuMetrics.loadLock);
   FixedAveragesUpdate(&load->vcpus, decays, m.vcpus);
   FixedAveragesUpdate(&load->vms, decays, m.vms);
   FixedAveragesUpdate(&load->baseShares, decays, m.baseShares);
   FixedAveragesUpdate(&load->eminPct, decays, eminPct);
   FixedAveragesUpdate(&load->eminMhz, decays, eminMhz);
   SP_Unlock(&cpuMetrics.loadLock);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuMetricsFixedAveragesFormat --
 *
 *	Converts load averages associated with "f" to decimal values,
 *	and formats "name" and decimal load averages into "buf".
 *
 * Results:
 *	Increments "len" by number of characters written into "buf".
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */
static void
CpuMetricsFixedAveragesFormat(const FixedAverages *f,
                              const char *name,
                              char *buf,
                              int *len)
{
   DecimalAverages d;

   // convert units
   FixedAveragesToDecimal(f, &d);
   
   // format output
   Proc_Printf(buf, len,
               "%-8s %6u.%03u %6u.%03u %6u.%03u %6u.%03u\n",
               name,
               d.value.whole, d.value.milli,
               d.avg1.whole,  d.avg1.milli,
               d.avg5.whole,  d.avg5.milli,
               d.avg15.whole, d.avg15.milli);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuMetricsProcLoadRead --
 *
 *     Proc read handler for /proc/vmware/sched/cpu-load.
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
CpuMetricsProcLoadRead(Proc_Entry *entry, char *buf, int *len)
{
   LoadAverages load;

   // initialize
   *len = 0;

   // snapshot current metrics
   SP_Lock(&cpuMetrics.loadLock);
   load = cpuMetrics.averages;
   SP_Unlock(&cpuMetrics.loadLock);

   // format header
   Proc_Printf(buf, len,
               "active      current       1min       5min      15min\n");

   // format output
   CpuMetricsFixedAveragesFormat(&load.vcpus, "vcpus", buf, len);
   CpuMetricsFixedAveragesFormat(&load.vms, "vms", buf, len);
   CpuMetricsFixedAveragesFormat(&load.eminPct, "eminPct", buf, len);
   CpuMetricsFixedAveragesFormat(&load.eminMhz, "eminMhz", buf, len);
   CpuMetricsFixedAveragesFormat(&load.baseShares, "bshares", buf, len);

   return(VMK_OK);
}

// load history ring buffer indexing
static INLINE uint32
LoadHistoryIndexNext(uint32 index)
{
   return((index + 1) % LOAD_HISTORY_SAMPLES_MAX);
}
static INLINE uint32
LoadHistoryIndexPrev(uint32 index)
{
   return((index == 0) ? LOAD_HISTORY_SAMPLES_MAX - 1 : index - 1);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuMetricsActiveSampleCompare --
 *
 *      Comparison function for use with heapsort().  Compares the
 *	32-bit activity samples "a" and "b".  Returns a negative, zero,
 *	or positive value when "a" > "b", "a" == "b", and "a" < "b",
 *	respectively.  Note that this is a descending sort order.
 *
 * Results:
 *	Returns a negative, zero, or positive value when
 *	"a" > "b", "a" == "b", and "a" < "b", respectively.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
CpuMetricsActiveSampleCompare(const void *a, const void *b)
{
   int aValue = *((uint32 *) a);
   int bValue = *((uint32 *) b);
   return(bValue - aValue);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuMetricsActiveQuintiles --
 *
 *      Updates load history summary "s" with quintile statistics
 *	for the first "nSamples" elements of "samples".
 *
 * Results:
 *      Updates load history summary "s" with quintile statistics
 *	for the first "nSamples" elements of "samples".
 *
 * Side effects:
 *      Sorts first "nSamples" elements of "samples".
 *
 *----------------------------------------------------------------------
 */
static void
CpuMetricsActiveQuintiles(uint32 *samples,
                          uint32 nSamples,
                          LoadHistorySummary *s)
{
   uint32 tmpSort;
   uint32 i;

   // sanity check: sufficient samples to compute quintiles
   ASSERT(nSamples >= 5);

   // sort samples in descending order
   heapsort(samples, nSamples, sizeof(uint32),
            CpuMetricsActiveSampleCompare, &tmpSort);

   // obtain min, max values
   s->activeMax = samples[0];
   s->activeMin = samples[nSamples - 1];

   // index into sorted samples to compute quintiles
   for (i = 0; i < 5; i++) {
      uint32 sampleIndex = (((i + 1) * nSamples) / 5) - 1;
      ASSERT(sampleIndex < nSamples);
      s->activeQuintile[i] = samples[sampleIndex];
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuMetricsLoadHistorySnapshotStats --
 *
 *      Snapshots a summary of the load history statistics associated
 *	with "h", including both simple averages and percentile
 *	statistics, into "s".  The "index" specifies the most recent
 *	sample in the load history ring buffer associated with "h".
 *
 * Results:
 *      Snapshots a summary of the load history statistics associated
 *	with "h" into "s".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuMetricsLoadHistorySnapshotStats(const CpuMetrics_LoadHistory *h,
                                   uint32 index,
                                   CpuMetricsLoadHistorySnap *s)
{
   uint32 nSamples;
   uint32 *active;

   // done if no samples
   if ((h == NULL) || (h->nSamples == 0)) {
      return;
   }

   // determine available samples
   nSamples = MIN(h->nSamples, LOAD_HISTORY_SAMPLES_MAX);
   s->nSamples = nSamples;
   ASSERT(nSamples > 0);

   // avoid copying samples we don't need
   nSamples = MIN(nSamples, LOAD_HISTORY_TS2);

   // allocate sample buffer
   active = Heap_Alloc(cpuMetrics.heap, nSamples * sizeof(uint32));

   // process sample buffer
   if (active != NULL) {
      uint32 i, count, activeSum, runSum;

      // initialize
      activeSum = 0;
      runSum = 0;
      count = 0;
      i = index;

      // copy samples, compute stats
      while (count < nSamples) {
         LoadHistorySummary *ts = NULL;

         // copy samples
         active[count] = h->samples[i].run + h->samples[i].ready;
         activeSum += active[count];
         runSum += h->samples[i].run;
         i = LoadHistoryIndexPrev(i);
         count++;

         // compute quintiles
         if (count == LOAD_HISTORY_TS0) {
            ts = &s->timeScale[0];
            CpuMetricsActiveQuintiles(active, count, ts);
         } else if (count == LOAD_HISTORY_TS1) {
            ts = &s->timeScale[1];
            CpuMetricsActiveQuintiles(active, count, ts);
         } else if (count == LOAD_HISTORY_TS2) {
            ts = &s->timeScale[2];
            CpuMetricsActiveQuintiles(active, count, ts);
         }

         // compute averages
         if (ts != NULL) {
            ts->runAvg = runSum / count;
            ts->activeAvg = activeSum / count;
         }
      }

      // reclaim sample buffer
      Heap_Free(cpuMetrics.heap, active);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuMetricsVcpuLoadHistorySnapshot --
 *
 *      Snapshots the identity and a summary of the load history
 *	statistics associated with "vcpu" into "s".  The "index"
 *	specifies the most recent sample in the load history ring
 *	buffer associated with "vcpu".
 *
 * Results:
 *      Snapshots the identity and a summary of the load history
 *	statistics associated with "vcpu" into "s".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuMetricsVcpuLoadHistorySnapshot(const CpuSched_Vcpu *vcpu,
                                  uint32 index,
                                  CpuMetricsLoadHistorySnap *s)
{
   World_Handle *world = World_VcpuToWorld(vcpu);

   // initialize
   memset(s, 0, sizeof(*s));

   // identity
   s->worldID = world->worldID;
   s->worldGroupID = CpuSched_GetVsmpLeader(world)->worldID;
   s->groupID = CpuSched_GetVsmpLeader(world)->sched.group.groupID;
   (void) strncpy(s->name, world->worldName, LOAD_HISTORY_NAME_LEN);

   // statistics
   CpuMetricsLoadHistorySnapshotStats(vcpu->loadHistory, index, s);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuMetricsGroupLoadHistorySnapshot --
 *
 *      Snapshots the identity and a summary of the load history
 *	statistics associated with "group" into "s".  The "index"
 *	specifies the most recent sample in the load history ring
 *	buffer associated with "group".
 *
 * Results:
 *      Snapshots the identity and a summary of the load history
 *	statistics associated with "group" into "s".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuMetricsGroupLoadHistorySnapshot(const Sched_Group *group,
                                   uint32 index,
                                   CpuMetricsLoadHistorySnap *s)
{
   // initialize
   memset(s, 0, sizeof(*s));

   // identity
   s->groupID = group->groupID;
   (void) strncpy(s->name, group->groupName, LOAD_HISTORY_NAME_LEN);

   // statistics
   CpuMetricsLoadHistorySnapshotStats(group->cpu.loadHistory, index, s);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuMetricsLoadHistorySnapStatsHeader --
 *
 *      Writes load history stats header into "buf".
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Increments "len" by number of characters written to "buf".
 *
 *----------------------------------------------------------------------
 */
static void
CpuMetricsLoadHistorySnapStatsHeader(char *buf, int *len)
{
   Proc_Printf(buf, len,
               "count   "
               "avgrun  avgact       80      60      40      20       0     "
               "avgrun  avgact       80      60      40      20       0     "
               "avgrun  avgact       80      60      40      20       0"
               "\n");
}

/*
 *----------------------------------------------------------------------
 *
 * CpuMetricsLoadHistorySnapStatsFormat --
 *
 *      Writes load history snapshot statistics for "s" into "buf".
 *	Stats are formatted as percentages if "formatPct" is TRUE.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Increments "len" by number of characters written to "buf".
 *
 *----------------------------------------------------------------------
 */
static void
CpuMetricsLoadHistorySnapStatsFormat(const CpuMetricsLoadHistorySnap *s,
                                     Bool formatPct, 
                                     char *buf, int *len)
{
   uint32 i, period;

   // sample period, in millisec
   period = CONFIG_OPTION(CPU_LOAD_HISTORY_SAMPLE_PERIOD);

   // sample count
   Proc_Printf(buf, len, "%5u  ", s->nSamples);
   
   // average and quintile statistics
   for (i = 0; i < LOAD_HISTORY_TIMESCALES; i++) {
      const LoadHistorySummary *a = &s->timeScale[i];
      if (formatPct) {
         // format values as percentages
         Proc_Printf(buf, len,
                     "%7u %7u  "
                     "%7u %7u %7u %7u %7u    ",
                     (100 * a->runAvg) / period,
                     (100 * a->activeAvg) / period,
                     (100 * a->activeQuintile[0]) / period,
                     (100 * a->activeQuintile[1]) / period,
                     (100 * a->activeQuintile[2]) / period,
                     (100 * a->activeQuintile[3]) / period,
                     (100 * a->activeQuintile[4]) / period);
      } else {
         // format raw values
         Proc_Printf(buf, len,
                     "%3u.%03u %3u.%03u  "
                     "%3u.%03u %3u.%03u %3u.%03u %3u.%03u %3u.%03u    ",
                     a->runAvg / 1000, a->runAvg % 1000,
                     a->activeAvg / 1000, a->activeAvg % 1000,
                     a->activeQuintile[0] / 1000, a->activeQuintile[0] % 1000,
                     a->activeQuintile[1] / 1000, a->activeQuintile[1] % 1000,
                     a->activeQuintile[2] / 1000, a->activeQuintile[2] % 1000,
                     a->activeQuintile[3] / 1000, a->activeQuintile[3] % 1000,
                     a->activeQuintile[4] / 1000, a->activeQuintile[4] % 1000);
      }
   }

   Proc_Printf(buf, len, "\n");
}

/*
 *----------------------------------------------------------------------
 *
 * CpuMetricsVcpuLoadHistorySnapFormat --
 *
 *      Writes load history snapshot information for "s" into "buf".
 *	Stats are formatted as percentages if "formatPct" is TRUE.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Increments "len" by number of characters written to "buf".
 *
 *----------------------------------------------------------------------
 */
static void
CpuMetricsVcpuLoadHistorySnapFormat(const CpuMetricsLoadHistorySnap *s,
                                    Bool formatPct,
                                    char *buf, int *len)
{
   // identity, sample count
   Proc_Printf(buf, len,
               "%4u %4u %-12.12s %5u ",
               s->worldID, s->worldGroupID, s->name, s->groupID);

   // statistics
   CpuMetricsLoadHistorySnapStatsFormat(s, formatPct, buf, len);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuMetricsGroupLoadHistorySnapFormat --
 *
 *      Writes load history snapshot information for "s" into "buf".
 *	Stats are formatted as percentages if "formatPct" is TRUE.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Increments "len" by number of characters written to "buf".
 *
 *----------------------------------------------------------------------
 */
static void
CpuMetricsGroupLoadHistorySnapFormat(const CpuMetricsLoadHistorySnap *s,
                                     Bool formatPct,
                                     char *buf, int *len)
{
   // identity, sample count
   Proc_Printf(buf, len,
               "%5u %-12.12s ",
               s->groupID, s->name);

   // statistics
   CpuMetricsLoadHistorySnapStatsFormat(s, formatPct, buf, len);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuMetricsProcLoadHistoryVcpusRead --
 *
 *      Callback for read operation on procfs node
 *	/proc/vmware/sched/cpu-load-history/vcpus.
 *
 * Results:
 *      Returns VMK_OK.
 *
 * Side effects:
 *      Increments "len" by number of characters written to "buf".
 *
 *----------------------------------------------------------------------
 */
static int
CpuMetricsProcLoadHistoryVcpusRead(Proc_Entry *entry, char *buf, int *len)
{
   Bool formatPct = (entry->private == 0) ? FALSE : TRUE;
   CpuMetricsLoadHistorySnap *loadSnaps;
   uint32 nWorlds, nSnaps, i, index;
   World_ID *allWorlds;

   // initialize
   *len = 0;

   // snapshot global index
   index = LoadHistoryIndexPrev(cpuMetrics.loadHistoryIndex);

   // allocate id storage
   allWorlds = Heap_Alloc(cpuMetrics.heap,
                          CPUSCHED_WORLDS_MAX * sizeof(World_ID));
   if (allWorlds == NULL) {
      return(VMK_NO_MEMORY);
   }

   // obtain worlds ids
   nWorlds = CPUSCHED_WORLDS_MAX;
   World_AllWorlds(allWorlds, &nWorlds);

   // allocate snap storage
   loadSnaps = Heap_Alloc(cpuMetrics.heap,
                          nWorlds * sizeof(CpuMetricsLoadHistorySnap));
   if (loadSnaps == NULL) {
      Heap_Free(cpuMetrics.heap, allWorlds);
      return(VMK_NO_MEMORY);
   }

   // summarize load histories
   nSnaps = 0;
   for (i = 0; i < nWorlds; i++) {
      World_Handle *world = World_Find(allWorlds[i]);
      if (world != NULL) {
         if (!World_IsIDLEWorld(world)) {
            const CpuSched_Vcpu *vcpu = World_CpuSchedVcpu(world);
            CpuMetricsVcpuLoadHistorySnapshot(vcpu, index, &loadSnaps[nSnaps]);
            nSnaps++;
         }
         World_Release(world);
      }
   }

   // format header
   Proc_Printf(buf, len, "vcpu   vm name         vmgid ");
   CpuMetricsLoadHistorySnapStatsHeader(buf, len);

   // format load history data
   for (i = 0; i < nSnaps; i++) {
      CpuMetricsVcpuLoadHistorySnapFormat(&loadSnaps[i], formatPct, buf, len);
   }

   // reclaim storage, succeed
   Heap_Free(cpuMetrics.heap, allWorlds);
   Heap_Free(cpuMetrics.heap, loadSnaps);
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuMetricsFindGroups -- 
 *
 *      Callback for Sched_ForAllGroupsDo() used to snapshot list
 *	of all groups into CpuMetricsAllGroups "data".
 *
 * Results:
 *      Adds "g" to list of all groups associated with "data".
 *
 * Side effects:
 *      Increments "len" by number of characters written to "buf".
 *
 *----------------------------------------------------------------------
 */
static void
CpuMetricsFindGroups(Sched_Group *g, void *data)
{
   CpuMetricsAllGroups *allGroups = (CpuMetricsAllGroups *) data;

   // add reference to prevent group deallocation
   Sched_TreeGroupAddReference(g);

   // add to list of all groups
   allGroups->group[allGroups->nGroups] = g;
   allGroups->nGroups++;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuMetricsAllGroupsNew -- 
 *
 *      Allocates, initializes and returns a new heap-allocated 
 *	structure for snapshotting the current list of all groups.
 *
 * Results:
 *      Returns initialized, heap-allocated CpuMetricsAllGroups.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static CpuMetricsAllGroups *
CpuMetricsAllGroupsNew(void)
{
   CpuMetricsAllGroups *allGroups;
   allGroups = Heap_Alloc(cpuMetrics.heap, sizeof(*allGroups));
   if (allGroups != NULL) {
      memset(allGroups, 0, sizeof(*allGroups));
   }
   return(allGroups);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuMetricsAllGroupsDelete -- 
 *
 *      Removes references to all groups in "allGroups", and
 *	reclaims all stroage associated with "allGroups".
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Reclaims heap-allocated storage.
 *
 *----------------------------------------------------------------------
 */
static void
CpuMetricsAllGroupsDelete(CpuMetricsAllGroups *allGroups)
{
   uint32 i;

   // drop group references
   Sched_TreeLock();
   for (i = 0; i < allGroups->nGroups; i++) {
      Sched_TreeGroupRemoveReference(allGroups->group[i]);
   }
   Sched_TreeUnlock();

   // reclaim storage
   Heap_Free(cpuMetrics.heap, allGroups);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuMetricsProcLoadHistoryGroupsRead --
 *
 *      Callback for read operation on procfs node
 *	/proc/vmware/sched/cpu-load-history/groups.
 *
 * Results:
 *      Returns VMK_OK.
 *
 * Side effects:
 *      Increments "len" by number of characters written to "buf".
 *
 *----------------------------------------------------------------------
 */
static int
CpuMetricsProcLoadHistoryGroupsRead(Proc_Entry *entry, char *buf, int *len)
{
   Bool formatPct = (entry->private == 0) ? FALSE : TRUE;
   CpuMetricsLoadHistorySnap *loadSnaps;
   CpuMetricsAllGroups *allGroups;
   uint32 index, i;

   // initialize
   *len = 0;

   // snapshot global index
   index = LoadHistoryIndexPrev(cpuMetrics.loadHistoryIndex);

   // allocate storage
   allGroups = CpuMetricsAllGroupsNew();
   if (allGroups == NULL) {
      return(VMK_NO_MEMORY);
   }

   // obtain group ids, increment reference counts
   Sched_ForAllGroupsDo(CpuMetricsFindGroups, allGroups);

   // allocate snap storage
   loadSnaps = Heap_Alloc(cpuMetrics.heap,
                          allGroups->nGroups * sizeof(CpuMetricsLoadHistorySnap));
   if (loadSnaps == NULL) {
      CpuMetricsAllGroupsDelete(allGroups);
      return(VMK_NO_MEMORY);
   }

   // summarize load histories
   for (i = 0; i < allGroups->nGroups; i++) {
      CpuMetricsGroupLoadHistorySnapshot(allGroups->group[i], index, &loadSnaps[i]);
   }

   // format header
   Proc_Printf(buf, len, "vmgid name         ");
   CpuMetricsLoadHistorySnapStatsHeader(buf, len);

   // format load history data
   for (i = 0; i < allGroups->nGroups; i++) {
      CpuMetricsGroupLoadHistorySnapFormat(&loadSnaps[i], formatPct, buf, len);
   }

   // reclaim storage, succeed
   CpuMetricsAllGroupsDelete(allGroups);
   Heap_Free(cpuMetrics.heap, loadSnaps);
   return(VMK_OK);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuMetricsLoadHistoryPeriodic --
 *
 *      Timer-based callback to periodically update load history data
 *	for vcpus and groups.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Modifies global "cpuMetrics" state.
 *
 *-----------------------------------------------------------------------------
 */
void
CpuMetricsLoadHistoryPeriodic(UNUSED_PARAM(void *ignore),
                              UNUSED_PARAM(Timer_AbsCycles timestamp))
{
   uint32 period;

   SP_Lock(&cpuMetrics.loadHistoryLock);

   // arrange for next invocation
   period = CONFIG_OPTION(CPU_LOAD_HISTORY_SAMPLE_PERIOD);
   ASSERT(period != 0);
   Timer_Add(MY_PCPU, CpuMetricsLoadHistoryPeriodic,
             period, TIMER_ONE_SHOT, NULL);

   // sample load history
   CpuSched_SampleLoadHistory();

   // advance ring buffer index
   cpuMetrics.loadHistoryIndex =
      LoadHistoryIndexNext(cpuMetrics.loadHistoryIndex);
   
   SP_Unlock(&cpuMetrics.loadHistoryLock);
}

/*
 * Exported operations
 */

/*
 *----------------------------------------------------------------------
 *
 * CpuMetrics_LoadHistoryNew --
 *
 *      Returns a new heap-allocated load history object, or
 *	NULL if unable to allocate sufficient memory.
 *
 * Results:
 *      Returns a new heap-allocated load history object, or
 *	NULL if unable to allocate sufficient memory.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
CpuMetrics_LoadHistory *
CpuMetrics_LoadHistoryNew(void)
{
   CpuMetrics_LoadHistory *h;

   // allocate storage, fail if unable
   h = Heap_Alloc(cpuMetrics.heap, sizeof(*h));
   if (h == NULL) {
      return(NULL);
   }

   // return initialized storage
   memset(h, 0, sizeof(*h));
   return(h);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuMetrics_LoadHistoryDelete --
 *
 *      Reclaims memory associated with an existing heap-allocated
 *	load history object "h".
 *
 * Results:
 *      Reclaims memory associated with an existing heap-allocated
 *	load history object "h".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
CpuMetrics_LoadHistoryDelete(CpuMetrics_LoadHistory *h)
{
   if (h != NULL) {
      Heap_Free(cpuMetrics.heap, h);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuMetrics_LoadHistoryReset --
 *
 *      Resets state of "h" to empty history.
 *
 * Results:
 *      Resets state of "h" to empty history.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
CpuMetrics_LoadHistoryReset(CpuMetrics_LoadHistory *h)
{
   h->prevRun = 0;
   h->prevReady = 0;
   h->nSamples = 0;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuMetrics_LoadHistorySampleDelta --
 *
 *	Updates load history "h" by adding a sample defined by
 *	the incremental values "runCycles" and "readyCycles".
 *
 * Results:
 *	Updates load history "h".
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
void
CpuMetrics_LoadHistorySampleDelta(CpuMetrics_LoadHistory *h,
                                  Timer_Cycles runCycles,
                                  Timer_Cycles readyCycles)
{
   uint32 run = Timer_TCToMS(runCycles / SMP_LogicalCPUPerPackage());
   uint32 ready = Timer_TCToMS(readyCycles);

   // sanity check
   ASSERT(SP_IsLocked(&cpuMetrics.loadHistoryLock));

   h->samples[cpuMetrics.loadHistoryIndex].run = run;
   h->samples[cpuMetrics.loadHistoryIndex].ready = ready;
   h->nSamples++;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuMetrics_LoadHistorySampleCumulative--
 *
 *	Updates load history "h" by adding a sample defined by
 *	the cumulative values "totalRun" and "totalReady".
 *	The incremental values "deltaRun" and "deltaReady"
 *	are set to reflect the change in the cumulative values
 *	since the last update.
 *
 * Results:
 *	Sets "deltaRun" and "deltaReady" to the change in the
 *	cumulative run and ready values since the last update.
 *	Updates load history "h".
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
void
CpuMetrics_LoadHistorySampleCumulative(CpuMetrics_LoadHistory *h,
                                       Timer_Cycles totalRun,
                                       Timer_Cycles totalReady,
                                       Timer_Cycles *deltaRun,
                                       Timer_Cycles *deltaReady)
{
   *deltaRun = totalRun - h->prevRun;
   *deltaReady = totalReady - h->prevReady;
   CpuMetrics_LoadHistorySampleDelta(h, *deltaRun, *deltaReady);
   h->prevRun = totalRun;
   h->prevReady = totalReady;
}

// period and sample info are both in millisec, so multiplying
// by 100 and dividing by period yields the percentage of the period
#define TOPCT(A) ((100 * (A)) / period)

/*
 *-----------------------------------------------------------------------------
 *
 * CpuMetricsGroupDrmStatsFormat --
 *
 *      Prints DRM stats for group "g" with the load history "snap" into "buf."
 *
 * Results:
 *      Returns VMK_OK.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
static void
CpuMetricsGroupDrmStatsFormat(Sched_Group *g, CpuMetricsLoadHistorySnap *snap, char *buf, int *len)
{
   char *cfgPath = NULL;
   CpuSched_Alloc *alloc = &g->cpu.alloc;
   uint32 period = CONFIG_OPTION(CPU_LOAD_HISTORY_SAMPLE_PERIOD);
   
   if ((g->flags & SCHED_GROUP_IS_VM) && g->members.len != 0) {
      Sched_Node *node = g->members.list[0];
      ASSERT(node->nodeType == SCHED_NODE_TYPE_VM);
      cfgPath = node->u.world->group->vmm.cfgPath;
   } else {
      cfgPath = "default";
   }

   if (cfgPath == NULL) {
      cfgPath = "n/a";
   }

   Proc_Printf(buf, len,
               "%4u  %7u  %5u  %5u  "       // gid, alloc info
               "%6u %7u   %6u "             // run avg 1
               "%6u %7u   %6u   "           // run avg 5
               "%6u  %7u    %6u "           // run avg 15
               "       %12s    (%36s)\n",   // name, cfg path
               g->groupID,
               // alloc info
               alloc->shares,
               alloc->min,
               alloc->max,               
               // run avgs
               TOPCT(snap->timeScale[0].activeAvg),
               TOPCT(snap->timeScale[0].activeQuintile[1]),
               TOPCT(snap->timeScale[0].runAvg),
               TOPCT(snap->timeScale[1].activeAvg),
               TOPCT(snap->timeScale[1].activeQuintile[1]),
               TOPCT(snap->timeScale[1].runAvg),
               TOPCT(snap->timeScale[2].activeAvg),
               TOPCT(snap->timeScale[2].activeQuintile[1]),
               TOPCT(snap->timeScale[2].runAvg),
               // config file
               g->groupName,
               cfgPath);
}


/*
 *-----------------------------------------------------------------------------
 *
 * CpuMetricsProcDrmStatsRead --
 *
 *      Read handler to print summary stats useful for DRM.
 *
 * Results:
 *      Returns VMK_OK.
 *
 * Side effects:
 *      Writes to buf.
 *
 *-----------------------------------------------------------------------------
 */
static int
CpuMetricsProcDrmStatsRead(Proc_Entry *entry, char *buf, int *len)
{
   CpuMetricsLoadHistorySnap *loadSnaps;
   CpuMetricsAllGroups *allGroups;
   uint32 index, i;

   *len = 0;
   
   // snapshot global index
   index = LoadHistoryIndexPrev(cpuMetrics.loadHistoryIndex);

   // allocate storage
   allGroups = CpuMetricsAllGroupsNew();
   if (allGroups == NULL) {
      return(VMK_NO_MEMORY);
   }

   // obtain group ids, increment reference counts
   Sched_ForAllGroupsDo(CpuMetricsFindGroups, allGroups);

   // allocate snap storage
   loadSnaps = Heap_Alloc(cpuMetrics.heap,
                          allGroups->nGroups * sizeof(CpuMetricsLoadHistorySnap));
   if (loadSnaps == NULL) {
      CpuMetricsAllGroupsDelete(allGroups);
      return(VMK_NO_MEMORY);
   }

   for (i=0; i < allGroups->nGroups; i++) {
      CpuMetricsGroupLoadHistorySnapshot(allGroups->group[i], index, &loadSnaps[i]);
   }
   
   Proc_Printf(buf, len,
               " gid   shares    min    max  "
               "actAv1  actPk1   runAv1  "
               "actAv5  actPk5   runAv5 "
               "actAv15  actPk15   runAv15"
               "        %12s     %36s\n",
               "name",
               "cfgPath");
   
   for (i = 0; i < allGroups->nGroups; i++) {
      Sched_Group *g = allGroups->group[i];
      if (g->flags & SCHED_GROUP_IS_VM || g->groupID == SCHED_GROUP_ID_ROOT) {
         CpuMetricsGroupDrmStatsFormat(g, &loadSnaps[i], buf, len);
      }
   }

   // reclaim storage, succeed
   CpuMetricsAllGroupsDelete(allGroups);
   Heap_Free(cpuMetrics.heap, loadSnaps);
   
   return (VMK_OK);
}


/*
 *----------------------------------------------------------------------
 *
 * CpuMetrics_Init --
 *
 *	Initializes CpuMetrics module.
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
CpuMetrics_Init(Proc_Entry *dir)
{
   CpuMetrics *m = &cpuMetrics;
   uint32 period;

   // sanity check: ring buffer larger than active history;
   // n.b. proc handlers snapshot global load history index w/o
   // locking, which is safe because they don't access the
   // entire buffer, and therefore won't observe inconsistencies
   // due to race with elements being overwritten while the
   // handler executes.
   ASSERT(LOAD_HISTORY_SAMPLES_MAX > LOAD_HISTORY_TS2);

   // zero state
   memset(m, 0, sizeof(CpuMetrics));

   // create heap
   m->heap = Heap_CreateDynamic("CpuMetrics",
                                CPUMETRICS_HEAP_SIZE_MIN,
                                CPUMETRICS_HEAP_SIZE_MAX);
   ASSERT_NOT_IMPLEMENTED(m->heap != INVALID_HEAP_ID);

   // initialize locks
   SP_InitLock("CpuMetricsLoad", &m->loadLock, SP_RANK_LEAF);
   SP_InitLock("CpuMetricsHistory", &m->loadHistoryLock, SP_RANK_LEAF);

   // initalize exponential weighted moving average decays
   m->decays.exp1  = CPUMETRICS_EXP_1;
   m->decays.exp5  = CPUMETRICS_EXP_5;
   m->decays.exp15 = CPUMETRICS_EXP_15;

   // periodic load average sampling
   Timer_Add(MY_PCPU, CpuMetricsLoadAveragePeriodic,
             CPUMETRICS_PERIOD_MS, TIMER_PERIODIC, NULL);
   
   // register "sched/cpu-load" entry
   Proc_InitEntry(&m->procLoad);
   m->procLoad.parent = dir;
   m->procLoad.read = CpuMetricsProcLoadRead;
   Proc_Register(&m->procLoad, "cpu-load", FALSE);

   // periodic load history sampling
   period = CONFIG_OPTION(CPU_LOAD_HISTORY_SAMPLE_PERIOD);
   ASSERT(period != 0);
   Timer_Add(MY_PCPU, CpuMetricsLoadHistoryPeriodic,
             period, TIMER_ONE_SHOT, NULL);

   // register "sched/cpu-load-history" directory
   Proc_InitEntry(&m->procLoadHistoryDir);
   m->procLoadHistoryDir.parent = dir;
   Proc_Register(&m->procLoadHistoryDir, "cpu-load-history", TRUE);

   // register "sched/cpu-load-history/vcpus" entry
   Proc_InitEntry(&m->procLoadHistoryVcpus);
   m->procLoadHistoryVcpus.parent = &m->procLoadHistoryDir;
   m->procLoadHistoryVcpus.read = CpuMetricsProcLoadHistoryVcpusRead;
   m->procLoadHistoryVcpusPct.private = (void *) FALSE;
   Proc_Register(&m->procLoadHistoryVcpus, "vcpus", FALSE);

   // register "sched/cpu-load-history/vcpus-pct" entry
   Proc_InitEntry(&m->procLoadHistoryVcpusPct);
   m->procLoadHistoryVcpusPct.parent = &m->procLoadHistoryDir;
   m->procLoadHistoryVcpusPct.read = CpuMetricsProcLoadHistoryVcpusRead;
   m->procLoadHistoryVcpusPct.private = (void *) TRUE;
   Proc_Register(&m->procLoadHistoryVcpusPct, "vcpus-pct", FALSE);

   // register "sched/cpu-load-history/groups" entry
   Proc_InitEntry(&m->procLoadHistoryGroups);
   m->procLoadHistoryGroups.parent = &m->procLoadHistoryDir;
   m->procLoadHistoryGroups.read = CpuMetricsProcLoadHistoryGroupsRead;
   m->procLoadHistoryGroups.private = (void *) FALSE;
   Proc_Register(&m->procLoadHistoryGroups, "groups", FALSE);

   // register "sched/cpu-load-history/groups-pct" entry
   Proc_InitEntry(&m->procLoadHistoryGroupsPct);
   m->procLoadHistoryGroupsPct.parent = &m->procLoadHistoryDir;
   m->procLoadHistoryGroupsPct.read = CpuMetricsProcLoadHistoryGroupsRead;
   m->procLoadHistoryGroupsPct.private = (void *) TRUE;
   Proc_Register(&m->procLoadHistoryGroupsPct, "groups-pct", FALSE);   

   // register drm entry
   Proc_InitEntry(&m->procDrmStats);
   m->procDrmStats.parent = dir;
   m->procDrmStats.read = CpuMetricsProcDrmStatsRead;
   Proc_Register(&m->procDrmStats, "drm-stats", FALSE);
   
   // debugging
   LOG(0, "initialized");
}
