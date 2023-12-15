/* **********************************************************
 * Copyright 1999 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * cpusched.c --
 *
 *	Proportional-share CPU scheduler for uniprocessor and
 *	multiprocessor VMs.  Supports near-synchronous co-scheduling
 *	of SMP VM VCPUs while providing rate-based control over CPU
 *	time allocations.  Co-scheduling of SMP VMs is desirable
 *	because the guest OS and applications running within an SMP VM
 *	are given the illusion that they are running on a dedicated
 *	physical multiprocessor.  Synchronous execution may
 *	significantly improve performance (e.g. consider spin locks
 *	within guest), and may even be required for correctness in
 *	some cases where remote guest operations are expected to
 *	complete quickly (e.g. consider TLB shootdowns within guest).
 *
 *	User-specified allocations (min, max, shares) are converted
 *	into internal "base" share allocations (also min, max, shares)
 *	that are used directly by the virtual time scheduling algorithms.
 *	In ESX 2.x, shares were specified using a flat namespace;
 *	starting with ESX 3.0, hierarchical grouping was introduced.
 *
 *	A two-level allocation algorithm is used to guarantee minimum
 *	execution rates and flexibly redistribute any remaining extra
 *	time.  Each VM that is not currently "ahead" of its entitled
 *	allocation competes in a "main" first-level allocation using a
 *	virtual time algorithm in which a VM's virtual time advances 
 *	even when it is not runnable.  When all runnable VMs are "ahead",
 *	they compete in an "extra" second-level allocation using a
 *	virtual time algorithm in which a VM's virtual time advances
 *	only when it runs.  Since a VM's "extra" virtual time remains
 *	constant while it is blocked, this provides a time-averaged
 *	form of fairness that allows VMs which block to "catch up"
 *	when extra time is available, without affecting guaranteed
 *	guaranteed execution rates.  A form of virtual time "aging"
 *	is employed to prevent VMs from monopolizing the consumption
 *	of extra time, e.g. after blocking for long periods of time.
 *
 *	All VCPUs of an SMP VM share a common virtual time and stride.
 *	After one VCPU is scheduled by the local processor, any
 *	remaining VCPUs are mapped to remotely-preemptible processors,
 *	and IPIs are sent to force remote reschedules, causing the
 *	specified VCPUs to start running.  Co-descheduling is
 *	performed by using a "skew timer" to sample the state of all
 *	VCPUs, and descheduling all of the VCPUs in the same VM if
 *	sufficient inter-VCPU skew occurs to violate co-scheduling
 *	constraints.
 *
 *	In ESX 2.0, locking was coarse-grained, with a single CpuSched
 *	lock protecting all scheduler state.  This greatly simplified
 *	scheduling operations that involved multiple processors,
 *	such as co-scheduling, but it could lead to nontrivial lock
 *	contention.  The concept of scheduler "cells" was introduced
 *	with ESX 2.1.  The set of all processors is partitioned into
 *	disjoint subsets called "cells", and at any given time, the
 *	VCPUs of a single SMP VM must reside in a single cell.  Separate
 *	per-cell locks are used to protect per-cell scheduler state.
 *	Finer-grain locking may be desirable, but this approach was
 *	relatively simple to reason about and implement.  Lock ordering
 *	is fairly simple: EventQueue locks must be acquired before any
 *	CpuSched cell locks, cell locks must be acquired in order of
 *	increasing cell id, and all other scheduler locks are leaf-ranked.
 *
 *      Hyperthreading --
 *        On a hyperthreaded system, we have two "logical cpus" per
 *        "package." We still use the PCPU type to refer to a logical
 *        CPU, but this should be renamed eventually. When a logical
 *        processor is halted (in IdleHaltStart) its execution resources
 *        are released to its partner logical processor, so the vcpu
 *        running on the partner executes at double its normal rate. Thus,
 *        we need to charge this VM double for the time when its partner
 *        is halted. To accomplish this, a vcpu keeps track of the change
 *        in the "haltCycles" field of its partner PCPU and adds this delta
 *        onto its charge time. 
 *
 *        A hyperthreaded system has 10000 base shares per package (i.e.
 *        5000 shares per logical processor). However, a 1-vcpu VM can 
 *        receive up to 10000 base shares so that it can occupy an entire
 *        package.
 *
 *	CpuSched supports the following configurations options:
 *          CPU_PCPU_MIGRATE_PERIOD
 *	    CPU_CELL_MIGRATE_PERIOD
 *          CPU_CREDIT_AGE_PERIOD
 *          CPU_COS_WARP_PERIOD
 *          CPU_BOUND_LAG_QUANTA
 *          COS_MIN_CPU
 *          CPU_QUANTUM
 *          CPU_IDLE_SWITCH_OPT
 *	    CPU_IDLE_CONSOLE_OPT
 *          CPU_HALTING_IDLE
 *          CPU_SCHEDULER_DEBUG
 *          CPU_SKEW_SAMPLE_USEC
 *          CPU_SKEW_SAMPLE_THRESHOLD
 *
 *
 *	Remaining Work
 *	==============
 *
 *	Short-Term
 *	  - scan only limited subset of remote pcpus/nodes
 *	  - further reduce migrations, improve cache affinity
 *	  - documentation: add state transition diagram
 *	  - reduce special-case code for BUSY_WAIT
 *	  - reduce locking for remaining non-snapshot procfs handlers
 *	  - renaming: "host" -> "console"
 *        - idle VM detection
 *
 *	Half-Baked
 *	  - vtime-based migration limits
 *	  - preempt vtime as f(running vcpu, ready vcpu w/ min vtime)
 *	  - choose optimization: consider how long vcpu has been on
 *	    remote main queue, help reduce migrations if not behind long
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "vmkernel.h"
#include "world.h"
#include "pagetable.h"
#include "prda.h"
#include "nmi.h"
#include "host.h"
#include "timer.h"
#include "smp.h"
#include "idt.h"
#include "mce.h"
#include "net.h"
#include "memalloc.h"
#include "user.h"
#include "helper.h"
#include "list.h"
#include "config.h"
#include "parse.h"
#include "util.h"
#include "event.h"
#include "memsched.h"
#include "bh.h"
#include "sched_sysacct.h"
#include "histogram.h"
#include "heapsort.h"
#include "numasched.h"
#include "cpuMetrics.h"
#include "cpusched.h"
#include "cpusched_int.h"
#include "sched_int.h"
#include "trace.h"
#include "apic.h"
#define LOGLEVEL_MODULE CpuSched
#include "log.h"

/*
 * Compile-time options
 */

// general debugging
#define	CPUSCHED_DEBUG			(vmx86_debug && vmx86_devel)
#define	CPUSCHED_DEBUG_VERBOSE		(0)
#define CPUSCHED_PREEMPT_STATS          (vmx86_debug)

// targeted debugging
#define	CPUSCHED_DEBUG_REPARENT		(0)
#define CPUSCHED_DEBUG_AGGSTATES        (0)
#define	CPUSCHED_DEBUG_COSTOP		(CPUSCHED_DEBUG)
#define CPUSCHED_DISABLE_INLINING       (CPUSCHED_DEBUG)

// statistics
#define	CPUSCHED_GROUP_CACHE_STATS	(CPUSCHED_DEBUG)


/*
 * Constants
 */

// stride
#define	CPUSCHED_STRIDE1_LG		(24)
#define	CPUSCHED_STRIDE1		(1 << CPUSCHED_STRIDE1_LG)
#define	CPUSCHED_STRIDE_MAX		(CPUSCHED_STRIDE1)
#define	CPUSCHED_STRIDE1_CYCLES_LG	(16)
#define	CPUSCHED_STRIDE1_CYCLES		(1 << CPUSCHED_STRIDE1_CYCLES_LG)

// virtual time
#define	CPUSCHED_VTIME_MAX		(1LL << 62)
#define	CPUSCHED_VTIME_RESET_LG		(61)

// base share conversions
#define	CPUSCHED_BASE_PER_PERCENT	(100)
#define CPUSCHED_BASE_PER_PACKAGE       (10000)
#define	CPUSCHED_BASE_RATIO_MIN		(0)
#define	CPUSCHED_BASE_RATIO_MAX		(1LL << 62)
#define	CPUSCHED_BASE_RATIO_SHIFT	(20)

// round off mhz
#define CPUSCHED_MHZ_ROUNDING           (10)
#define CPUSCHED_MAX_UINT32             ((uint32)-1)

// timeouts (in milliseconds)
#define	CPUSCHED_SWITCH_WAIT_WARN	(10)
#define	CPUSCHED_SWITCH_WAIT_PANIC	(200)

// lock retry limits
#define	CPUSCHED_LOCK_RETRY_PANIC	(1 << 20)
#define	CPUSCHED_LOCK_RETRY_ASSERT	(1000)
#define	CPUSCHED_LOCK_RETRY_DEBUG	(10)

// periodic processing
#define	CPUSCHED_TIMER_PERIOD		(1000)
#define CPUSCHED_HT_EVENT_PERIOD        (1000)
#define PSEUDO_TSC_TIMER_PERIOD_MS      (60000)

// random jitter
#define	CPUSCHED_SMALL_JITTER_USEC	(100)

// credit aging
#define	CPUSCHED_CREDIT_AGE_DIVISOR	(2)

// vtime bounds
#define	CPUSCHED_BOUND_LAG_QUANTA	(8)

// IRQ level
#define	CPUSCHED_IRQL			(SP_IRQL_KERNEL)

// buffer sizes
#define	CPUSCHED_CPUMASK_BUF_LEN	(96)

// skew constants
#define CPUSCHED_IGNORE_SKEW            (0xffffffff)

// service console
#define	CONSOLE_PCPU			(HOST_PCPU)
#define	CONSOLE_WORLD			(hostWorld)
#define CONSOLE_VCPU			(World_CpuSchedVcpu(CONSOLE_WORLD))
#define	CONSOLE_VSMP			(World_CpuSchedVsmp(CONSOLE_WORLD))
#define	CONSOLE_CELL			(CpuSchedPcpu(CONSOLE_PCPU)->cell)

// local cell
#define	MY_CELL				(CpuSchedPcpu(MY_PCPU)->cell)

// if this option is "1", histogram data will be tracked
#define CPUSCHED_STATE_HISTOGRAMS          (1)
#define CPUSCHED_DEFAULT_NUM_HISTO_BUCKETS (11)

// default cell size, in packages
#define	CPUSCHED_CELL_PACKAGES_DEFAULT	(4)

/*
 * Types
 */

typedef struct {
   // aggregate op stats
   Timer_Cycles totalCycles;
   uint32 totalCount;
   uint32 failCount;

   // single op stats
   Timer_AbsCycles start;
   Timer_Cycles cycles;
} CpuSchedOpStats;

typedef struct {
   List_Links queue;
   Bool extra;
   Bool limbo;
} CpuSchedQueue;

typedef struct {
   uint32 yield;
   uint32 dyield;
   uint32 dyieldFailed;
   uint32 preempts;
   uint32 timer;
   uint32 ipi;
   uint32 handoff;
   uint32 switchWait;
   Timer_Cycles haltCycles;
   uint64 groupLookups;
   uint64 groupHits;
   uint64 idleHaltEnd;
   uint64 idleHaltEndIntr;
} CpuSched_PcpuStats;

typedef struct {
   Bool valid;
   CpuSched_VtimeContext vtime;
   Timer_RelCycles vtBonus;
} CpuSchedPcpuPreemption;

typedef struct {
   uint32 generation;
   Sched_GroupID id;

   CpuSchedStride stride;
   CpuSchedVtime vtime;
   CpuSchedVtime vtimeLimit;
} CpuSchedGroupVtimeCacheEntry;

typedef struct {
   uint32 generation;
   CpuSchedGroupVtimeCacheEntry cache[SCHED_GROUPS_MAX];
} CpuSchedGroupVtimeCache;

typedef struct CpuSched_Pcpu {
   // run queues
   CpuSchedQueue queueMain;
   CpuSchedQueue queueExtra;
   CpuSchedQueue queueLimbo;

   CpuSched_Vcpu *handoff;
   CpuSched_Vcpu *directedYield;

   // identity
   PCPU id;

   // scheduler cell
   struct CpuSched_Cell *cell;

   // hypertwin, if any
   struct CpuSched_Pcpu *partner;

   // accounting
   Timer_Cycles usedCycles;
   Timer_Cycles idleCycles;
   Timer_Cycles sysCyclesOverlap;

   // hyperthread halt accounting
   // caller must hold haltLock for the package containing
   // this pcpu to change/read totalHaltCycles or haltStart
   // the halt lock for the package lives the CpuSched_Pcpu
   // structure for lcpu 0 on the package
   SP_SpinLockIRQ haltLock;
   Timer_Cycles totalHaltCycles;
   TSCCycles haltStart;
   Histogram_Handle haltHisto;

   // OPT: cache-align preemption
   // local preemption state
   CpuSchedPcpuPreemption preemption;

   // group vtime cache
   CpuSchedGroupVtimeCache groupVtimes;

   // migration state
   Timer_AbsCycles nextPcpuMigrateAllowed;
   Timer_AbsCycles nextRunnerMoveAllowed;
   Bool recentPcpuMig;
   Bool runnerMoveRequested;

   // yield throttling
   TSCCycles lastYieldTime;

   // for use with CPU_RESCHED_OPT == CPUSCHED_RESCHED_DEFER
   Bool deferredResched;
   
   // per-pcpu skew timer
   Timer_Handle skewTimer;

   // stats
   CpuSched_PcpuStats stats;

   // wait-for-switch state
   Bool switchWaitWarn;
   World_ID switchWaitWorldID;
   TSCCycles switchWaitCycles;
   Histogram_Handle switchWaitHisto;
   
   // debugging support
   uint32 lastYieldCount;
} CpuSched_Pcpu;

typedef struct {
   // identity
   PCPU id;
   NUMA_Node node;
   
   // stats
   CpuSched_PcpuStats stats;

   // hyperthread halt stats
   Timer_Cycles haltCycles;
   Bool halted;

   // derived stats
   World_ID handoffID;
} CpuSchedPcpuSnap;

typedef struct {
   World_ID leaderID;
   uint32 nvcpus;
   Bool groupEnforceMax;
} CpuSchedVsmpNodeSnap;

typedef struct {
   Sched_GroupID groupID;
   struct CpuSchedNodeSnap *members[SCHED_GROUP_MEMBERS_MAX];
   uint32 nMembers;
} CpuSchedGroupNodeSnap;

typedef struct CpuSchedNodeSnap {
   CpuSched_Alloc alloc;
   CpuSched_Alloc base;
   uint64 baseRatio;
   uint32 vsmpCount;

   // tagged union: "vsmp" or "group" node
   Sched_NodeType nodeType;
   union {
      CpuSchedVsmpNodeSnap vsmp;
      CpuSchedGroupNodeSnap group;
   } u;
} CpuSchedNodeSnap;

typedef struct {
   // counts
   uint32 nVsmps;
   uint32 nGroups;

   // nodes
   CpuSchedNodeSnap *nodeRoot;
   CpuSchedNodeSnap nodes[SCHED_NODES_MAX];
   uint32 nNodes;
} CpuSchedReallocSnap;

typedef struct {
   // identity
   World_ID worldID;
   World_ID worldGroupID;
   Sched_GroupID groupID;
   uint32 worldFlags;
   char worldName[WORLD_NAME_LENGTH];
   
   // states
   CpuSched_CoRunState coRunState;
   CpuSched_RunState runState;
   CpuSched_WaitState waitState;

   // allocation parameters
   CpuSched_Alloc alloc;
   CpuMask affinityMask;
   uint32 nvcpus;
   Sched_HTSharing htSharing;

   // allocation state
   CpuSched_Alloc base;
   CpuSched_VtimeContext vtime;
   CpuSchedVtime vtimeLimit;
   PCPU pcpu;

   // usage, statistics
   Timer_Cycles chargeCyclesTotal;
   Timer_Cycles sysCyclesTotal;
   Timer_Cycles readyCycles;
   Timer_Cycles limboCycles;
   Timer_Cycles haltedCycles;
   CpuSched_VcpuStats stats;
   CpuSched_VsmpStats vsmpStats;
   Bool htQuarantine;

   // derived stats
   CpuSchedVtime ahead;
   Timer_Cycles waitCycles;
   Timer_Cycles uptime;
} CpuSchedVcpuSnap;

typedef struct {
   // identity
   Sched_GroupID groupID;
   char groupName[SCHED_GROUP_NAME_LEN];

   // parent identity
   Sched_GroupID parentID;
   char parentName[SCHED_GROUP_NAME_LEN];

   // state
   uint32 members;
   CpuSched_GroupState state;
} CpuSchedGroupSnap;

typedef struct {
   // real time
   Timer_Cycles uptime;

   // virtual time
   CpuSchedStride stride;

   // stats
   uint32 cellCount;
   uint32 vmCount;
   uint32 consoleWarpCount;
   uint32 resetVtimeCount;
} CpuSchedGlobalSnap;

typedef struct {
   // best vcpu, vtime
   CpuSched_Vcpu *min;
   CpuSched_VtimeContext *vtime;
   CpuSched_VtimeContext vtimeData;
   Timer_RelCycles vtBonus;

   // list of vcpus that must be coscheduled
   CpuMask vcpusNeedCosched;

   // hyperthreading
   Bool wholePackage;
   PCPU currentRunnerDest;

   // directed yields
   Bool isDirectedYield;
   
   // flags
   Bool pcpuMigrateAllowed;
   Bool cellMigrateAllowed;
   Bool runnerMoveAllowed;
} CpuSchedChoice;

// CpuSchedConst contains values that are
// written only once during initialization,
// and can therefore be read unlocked.
typedef struct {
   // cycles conversions
   Timer_Cycles cyclesPerSecond;
   Timer_Cycles cyclesPerMinute;

   // randomized jitter
   uint32 smallJitterCycles;

   // base conversions
   uint32 percentPcpu;
   uint32 percentTotal;

   uint32 roundedMhz;
   uint32 unitsPerPkg[SCHED_UNITS_INVALID];
   
   // initialization timestamp
   Timer_AbsCycles uptimeStart;

   // hyperthreading
   struct VmkperfEventInfo *machineClearEvent;
   
   // NUMA configuration
   CpuMask numaNodeMasks[NUMA_MAX_NODES];
   Bool numaSystem;

   // global base shares
   uint32 baseShares;
   CpuSchedStride stride;
   CpuSchedStride nStride;

   // affinity
   CpuMask defaultAffinity;
} CpuSchedConst;

typedef enum {
   CPU_RESCHED_ALWAYS = 0,
   CPU_RESCHED_PREEMPTIBLE = 1,
   CPU_RESCHED_DEFER = 2,
   CPU_RESCHED_NONE = 3
} CpuVcpuReschedOpt;

// CpuSchedConfig contains values that are
// derived from configurable options.
typedef struct {
   // credit aging (millisec)
   uint32 creditAgePeriod;

   // thresholds (cycles) 
   Timer_Cycles quantumCycles;
   Timer_Cycles idleQuantumCycles;
   Timer_Cycles boundLagCycles;
   Timer_Cycles coSchedCacheAffinCycles;
   Timer_RelCycles idleVtimeMsPenaltyCycles;
   Timer_Cycles sysAcctLimitCycles;
   Timer_Cycles intrLevelPenaltyCycles;
   Timer_RelCycles preemptionBonusCycles;
   
   // thresholds (vtime)
   CpuSchedVtime vtAheadThreshold;

   // migration
   Timer_Cycles migPcpuWaitCycles;
   Timer_Cycles migCellWaitCycles;
   Timer_Cycles runnerMoveWaitCycles;
   uint32 migChance;
   TSCCycles idlePackageRebalanceCycles;
   
   // rescheduling aggressiveness
   CpuVcpuReschedOpt vcpuReschedOpt;
   TSCCycles yieldThrottleTSC;
   
   // skew 
   uint32 skewSampleUsec;
   Timer_Cycles skewSampleMinInterval;
   uint32 skewSampleThreshold;
   uint32 intraSkewThreshold;
   Bool relaxCosched;
   
   // COS warping
   Timer_Cycles consoleWarpCycles;

   // quarantining
   Timer_Cycles htEventsUpdateCycles;

   // vtime reset
   CpuSchedVtime vtimeResetThreshold;
   CpuSchedVtime vtimeResetAdjust;
} CpuSchedConfig;

typedef struct {
   uint32 remoteLockSuccess;
   uint32 remoteLockFailure;
   Bool   remoteLockLast;
} CpuSchedCellStats;

typedef struct CpuSched_Cell {
   // mutual exclusion
   SP_SpinLockIRQ lock;

   // identity
   uint32 id;

   // pcpus
   CpuMask pcpuMask;
   PCPU pcpu[CPUSCHED_PCPUS_MAX];
   uint32 nPCPUs;

   // virtual SMPs
   CpuSched_VsmpArray vsmps;

   // real time 
   Timer_AbsCycles now;
   Timer_Cycles lostCycles;
   
   // virtual time
   CpuSchedVtime vtime;
   Timer_Handle vtResetTimer;

   // migration state
   Timer_AbsCycles nextCellMigrateAllowed;

   // statistics
   CpuSchedCellStats stats;

   // configuration info (replicated)
   CpuSchedConfig config;
} CpuSched_Cell;

typedef struct {
   // identity
   uint32 id;

   // pcpus
   CpuMask pcpuMask;
   uint32 nPCPUs;

   // virtual SMPs
   uint32 nVsmps;

   // real time
   Timer_AbsCycles now;
   Timer_Cycles lostCycles;

   // virtual time
   CpuSchedVtime vtime;

   // statistics
   CpuSchedCellStats stats;

   // config info
   CpuSchedConfig config;
} CpuSchedCellSnap;

typedef struct {
   SP_SpinLock lock;
   CpuSchedVcpuSnap vcpu[CPUSCHED_VCPUS_MAX];
   uint32 vcpuSort[CPUSCHED_VCPUS_MAX];
   CpuSchedPcpuSnap pcpu[CPUSCHED_PCPUS_MAX];
   CpuSchedCellSnap cell[CPUSCHED_CELLS_MAX];
   CpuSchedGroupSnap group[SCHED_GROUPS_MAX];
   CpuSchedGlobalSnap global;   
} CpuSchedSnap;

typedef struct {
   // procfs nodes
   Proc_Entry cpu;
   Proc_Entry cpuVerbose;
   Proc_Entry cpuStateTimes;
   Proc_Entry cpuStateCounts;   
   Proc_Entry pcpuRunTimes;
   Proc_Entry idle;
   Proc_Entry ncpus;
   Proc_Entry groups;

   // hidden procfs nodes
   Proc_Entry debug;
   Proc_Entry resetStats;
} CpuSchedProc;

typedef struct {
   // scheduler cells
   CpuSched_Cell cell[CPUSCHED_CELLS_MAX];
   uint32 nCells;

   // processors
   CpuSched_Pcpu pcpu[CPUSCHED_PCPUS_MAX];

   // console warping
   CpuSchedVtime vtConsoleWarpCurrent;
   CpuSchedVtime vtConsoleWarpDelta;

   // stats
   uint32 resetVtimeCount;
   uint32 consoleWarpCount;
   uint32 periodicCount;
   uint32 numIdlePreempts;

   // hyperthreading
   Bool htQuarantineActive;

   // reallocation state
   Bool reallocNeeded;
   Bool reallocInProgress;
   CpuSchedReallocSnap reallocSnap;
   CpuSchedOpStats reallocStats;

   // scheduler dumper control
   Bool stopSchedDumper;

   // procfs snapshot data
   CpuSchedSnap procSnap;

   // procfs nodes
   CpuSchedProc proc;
} CpuSched;


/*
 * Globals
 */

uint32 CpuSched_EIPAfterHLT = 0;

static CpuSchedConst	cpuSchedConst;
static CpuSched		cpuSched;


/*
 * Function inlining
 */

#if (CPUSCHED_DISABLE_INLINING)
#undef	INLINE
#define INLINE
#endif

/*
 * Local functions
 */

static void CpuSchedDispatch(SP_IRQL prevIRQL, Bool updateTime);
static void CpuSchedBusyWait(SP_IRQL prevIRQL);
static void CpuSchedResetVtime(void *ignore);
static void CpuSchedAddWorldProcEntries(World_Handle *world);
static void CpuSchedRemoveWorldProcEntries(World_Handle *world);
static VMK_ReturnStatus CpuSchedRemoveInt(World_Handle *world,
                                          EventQueue *eventQueue);
static void CpuSchedSendReschedIPI(PCPU pcpu);
static void CpuSchedVcpuChargeWait(CpuSched_Vcpu *vcpu,
                                   CpuSchedVtime vtElapsed);

// skew handling
static Bool CpuSchedVsmpStrictSkewOut(const CpuSched_Vsmp *vsmp);

// preemption operations
static Bool CpuSchedPcpuCanPreempt(const CpuSched_Pcpu *schedPcpu,
                                   const CpuSched_Vsmp *vsmp);
static void CpuSchedPcpuPreemptionUpdate(CpuSched_Pcpu *schedPcpu);

static VMK_ReturnStatus CpuSchedVerifyAffinity(int numVcpus, 
                                               CpuMask vcpuMasks[], 
                                               Bool *jointAffinity);
static void CpuSchedWorldPseudoTSCConvCB(void *data, Timer_AbsCycles timestamp);
static void CpuSchedIdleHaltStart(void);

// state names
static const char *CpuSchedRunStateName(CpuSched_RunState runState);
static const char *CpuSchedCoRunStateName(CpuSched_CoRunState coRunState);
static const char *CpuSchedWaitStateName(CpuSched_WaitState waitState);

// procfs operations
static int CpuSchedProcRead(Proc_Entry *e, char *buf, int *len);
static int CpuSchedProcStateTimesRead(Proc_Entry *e, char *buf, int *len);
static int CpuSchedProcStateCountsRead(Proc_Entry *e, char *buf, int *len);
static int CpuSchedProcPcpuRunTimesRead(Proc_Entry *e, char *buf, int *len);
static int CpuSchedProcNcpusRead(Proc_Entry *e, char *buf, int *len);
static int CpuSchedProcIdleRead(Proc_Entry *e, char *buf, int *len);
static int CpuSchedProcWorldDebugRead(Proc_Entry *e, char *buf, int *len);
static int CpuSchedProcWorldDebugWrite(Proc_Entry *e, char *buf, int *len);
static int CpuSchedProcDebugRead(Proc_Entry *e, char *buf, int *len);
static int CpuSchedProcDebugWrite(Proc_Entry *e, char *buf, int *len);
static int CpuSchedProcResetStatsWrite(Proc_Entry *e, char *buf, int *len);
static int CpuSchedProcWorldHyperthreadingRead(Proc_Entry *e, char *buf, int *len);
static int CpuSchedProcWorldHyperthreadingWrite(Proc_Entry *e, char *buf, int *len);
static int CpuSchedProcWorldGroupRead(Proc_Entry *e, char *buf, int *len);
static int CpuSchedProcWorldGroupWrite(Proc_Entry *e, char *buf, int *len);

// affinity
static void CpuSchedVsmpSetAffinityInt(CpuSched_Vsmp *vsmp, CpuMask vcpuMasks[], Bool hardAffinity);
static void CpuSchedVsmpSetSoftAffinity(CpuSched_Vsmp *vsmp, CpuMask affinity);

// action operations
static void CpuSchedVcpuActionNotifyRequest(CpuSched_Vcpu *vcpu, Bool notify);

// debugging operations
static void CpuSchedVsmpAggregateStateCheck(const CpuSched_Vsmp *vsmp);

// histogram operations 
static int CpuSchedProcWaitStatesHistoRead(Proc_Entry *entry, char *buffer, int *len);
static int CpuSchedProcRunStatesHistoRead(Proc_Entry *entry, char *buffer, int *len);
static int CpuSchedProcRunStatesHistoWrite(Proc_Entry *entry, char *buffer, int *len);
static int CpuSchedProcWaitStatesHistoWrite(Proc_Entry *entry, char *buffer, int *len);

static void CpuSchedCoRun(CpuSched_Pcpu *schedPcpu, CpuSched_Vcpu *vcpu, Bool wholePkg,
                          uint32 *vcpusToPlace, uint32 *pcpusForbidden);
static void CpuSchedDumpToLog(void *unused, Timer_AbsCycles timestamp);
static void CpuSchedSetHTSharing(CpuSched_Vsmp *vsmp, Sched_HTSharing newShareType);
static VMK_ReturnStatus CpuSchedHTQuarantineCallback(Bool write, Bool changed, int indx);
static void CpuSchedSetHTQuarantineActive(Bool active);
static Sched_HTSharing CpuSchedVsmpMaxHTConstraint(const CpuSched_Vsmp *vsmp);

// group operations
static void CpuSchedVsmpGroupCharge(const CpuSched_Vsmp *vsmp,
                                    Timer_Cycles cycles);
static void CpuSchedAgeAllGroupVtimes(CpuSchedVtime vtNow);
static void CpuSchedResetAllGroupVtimes(void);
static void CpuSchedGroupSetAllocInt(Sched_Group *group,
                                     const CpuSched_Alloc *alloc);

// admission control operations
static void CpuSchedNodeReservedMin(const Sched_Node *node,
                                    uint32 *reserved,
                                    uint32 *unreserved);
static Bool CpuSchedVsmpAllocAllowed(const CpuSched_Vsmp *vsmp, 
                                     const CpuSched_Alloc *alloc, 
                                     uint8 numVcpus);

/*
 * Macros
 */

// iterator macros
#define	FORALL_SCHED_PCPUS(_schedPcpu) \
	FORALL_PCPUS(_spi) { \
	  CpuSched_Pcpu* _schedPcpu = CpuSchedPcpu(_spi);
#define	SCHED_PCPUS_DONE } PCPUS_DONE

#define	FORALL_CELL_PCPUS(_cell, _pcpu) \
	do { \
	  uint32 _pi; \
	  for (_pi = 0; _pi < (_cell)->nPCPUs; _pi++) { \
	    PCPU _pcpu = (_cell)->pcpu[_pi];
#define	CELL_PCPUS_DONE }} while(0) 

#define	FORALL_CELL_REMOTE_PCPUS(_cell, _localPCPU, _pcpu) \
	FORALL_CELL_PCPUS(_cell, _pcpu) \
	  if (_pcpu != _localPCPU)
#define	CELL_REMOTE_PCPUS_DONE CELL_PCPUS_DONE

#define	FORALL_CELL_PACKAGES(_cell, _pcpu) \
	FORALL_CELL_PCPUS(_cell, _pcpu) \
	  if (SMP_GetHTThreadNum(_pcpu) == 0)
#define	CELL_PACKAGES_DONE CELL_PCPUS_DONE
	  
#define	FORALL_CELL_REMOTE_PACKAGES(_cell, _localPCPU, _pcpu) \
	FORALL_CELL_PACKAGES(_cell, _pcpu) \
	  if (SMP_GetPackageNum(_pcpu) != SMP_GetPackageNum(_localPCPU))
#define	CELL_REMOTE_PACKAGES_DONE CELL_PACKAGES_DONE

#define	FORALL_CELLS_INTERNAL(_cell, _debug) \
	do { \
	  uint32 _ci; \
	  ASSERT(!(_debug) || CpuSchedAllCellsAreLocked()); \
	  for (_ci = 0; _ci < cpuSched.nCells; _ci++) { \
	    CpuSched_Cell* _cell = &cpuSched.cell[_ci];
#define	CELLS_INTERNAL_DONE }} while(0)

#define	FORALL_CELLS(_cell) \
	FORALL_CELLS_INTERNAL(_cell, TRUE)
#define	CELLS_DONE CELLS_INTERNAL_DONE

#define	FORALL_CELLS_UNLOCKED(_cell) \
	FORALL_CELLS_INTERNAL(_cell, FALSE)
#define	CELLS_UNLOCKED_DONE CELLS_INTERNAL_DONE

#define	FORALL_CELL_VSMPS(_cell, _vsmp) \
	do { \
	  uint32 _cvi; \
	  ASSERT(CpuSchedCellIsLocked(_cell)); \
	  for (_cvi = 0; _cvi < (_cell)->vsmps.len; _cvi++) { \
	    CpuSched_Vsmp* _vsmp = (_cell)->vsmps.list[_cvi];
#define	CELL_VSMPS_DONE }} while(0)

#define	FORALL_VSMP_VCPUS(_vsmp, _vcpu) \
	do { \
	  uint32 _vvi; \
	  for (_vvi = 0; _vvi < (_vsmp)->vcpus.len; _vvi++) { \
	    CpuSched_Vcpu* _vcpu = (_vsmp)->vcpus.list[_vvi];
#define	VSMP_VCPUS_DONE }} while(0) 

#define FORALL_NODE_PCPUS(_node, _pcpu) \
	FORALL_PCPUS(_pcpu) \
          if (CPUSCHED_AFFINITY(_pcpu) & cpuSchedConst.numaNodeMasks[_node])
#define	NODE_PCPUS_DONE PCPUS_DONE

#define	FORALL_NODE_PACKAGES(_node, _pcpu) \
	FORALL_NODE_PCPUS(_node, _pcpu) \
	  if (SMP_GetHTThreadNum(_pcpu) == 0)
#define	NODE_PACKAGES_DONE NODE_PCPUS_DONE

#define	FORALL_SNAP_GROUP_MEMBERS(_g, _n) \
	do { \
	  uint32 _gmi; \
	  for (_gmi = 0; _gmi < (_g)->nMembers; _gmi++) { \
	    CpuSchedNodeSnap* _n = (_g)->members[_gmi];
#define	SNAP_GROUP_MEMBERS_DONE }} while(0) 

// structured logging macros (CpuSched)
#define CpuSchedLog(fmt, args...)  LOG(0, fmt , ##args)

// structured logging macros (CpuSchedVcpu)
#define	VcpuWarn(vcpu, fmt, args...) \
 VmWarn(VcpuWorldID(vcpu), fmt , ##args)
#define VcpuLog(vcpu, fmt, args...) \
 VmLog(VcpuWorldID(vcpu), fmt , ##args)
#define	VCPULOG(level, vcpu, fmt, args...) \
 VMLOG(level, VcpuWorldID(vcpu), fmt , ##args)
  
// structured logging macros (CpuSchedVsmp)
#define VsmpWarn(vsmp, fmt, args...) \
 VmWarn(VsmpLeaderID(vsmp), fmt , ##args)
#define VsmpLog(vsmp, fmt, args...) \
 VmLog(VsmpLeaderID(vsmp), fmt , ##args)
#define VSMPLOG(level, vsmp, fmt, args...) \
 VMLOG(level, VsmpLeaderID(vsmp), fmt , ##args)

// helper for proc write handlers
#define CONST_STRNCMP(teststr, conststr) \
 strncmp(teststr, conststr, sizeof(conststr)-1)

// check that PRDA mapped correctly (must have interrupts disabled)
static INLINE void
ASSERT_PRDA_SANITY(void)
{
   if (vmx86_debug) {
      uint32 eflags;
      SAVE_FLAGS(eflags);
      if (eflags & EFLAGS_IF) {
         CLEAR_INTERRUPTS();
      }
      ASSERT_NO_INTERRUPTS();
      ASSERT(APIC_GetPCPU() == MY_PCPU);
      if (eflags & EFLAGS_IF) {
         RESTORE_FLAGS(eflags);
      }
   }
}

/*
 * Simple Wrappers
 */

// vcpu identifier
static INLINE World_ID
VcpuWorldID(const CpuSched_Vcpu *vcpu)
{
   return(World_VcpuToWorld(vcpu)->worldID);
}

// vsmp identifier
static INLINE World_ID
VsmpLeaderID(const CpuSched_Vsmp *vsmp)
{
   return(vsmp->leader->worldID);
}

// random number generation
static INLINE uint32
CpuSchedRandom(void)
{
   ASSERT_NO_INTERRUPTS();
   return(myPRDA.randSeed = Util_FastRand(myPRDA.randSeed));
}

// event logging
static INLINE void
CpuSchedLogEvent(const char *eventName, uint64 eventData)
{
   Log_Event(eventName, eventData, EVENTLOG_CPUSCHED);
}

static INLINE void
VcpuLogEvent(const CpuSched_Vcpu *vcpu, const char *eventName)
{
   CpuSchedLogEvent(eventName, 
                    (vcpu) ? VcpuWorldID(vcpu) : INVALID_WORLD_ID);
}

/*
 * CpuSched Operations
 */

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedOpStatsStart --
 *
 *      Update timed operation statistics "stats" to reflect
 *	start of new operation.
 *
 * Results:
 *      Update timed operation statistics "stats" to reflect
 *	start of new operation.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedOpStatsStart(CpuSchedOpStats *stats)
{
   stats->start = Timer_GetCycles();
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedOpStatsStop --
 *
 *      Update timed operation statistics "stats" to reflect
 *	completion of new operation.
 *
 * Results:
 *      Update timed operation statistics "stats" to reflect
 *	completion of new operation.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedOpStatsStop(CpuSchedOpStats *stats)
{
   Timer_AbsCycles stop = Timer_GetCycles();
   ASSERT(stop >= stats->start);
   stats->cycles = stop - stats->start;
   stats->totalCycles += stats->cycles;
   stats->totalCount++;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedOpStatsAvg --
 *
 *      Returns the average operation time associated with "stats".
 *
 * Results:
 *      Returns the average operation time associated with "stats".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static Timer_Cycles
CpuSchedOpStatsAvg(const CpuSchedOpStats *stats)
{
   Timer_Cycles avg = 0;
   if (stats->totalCount > 0) {
      avg = stats->totalCycles / stats->totalCount;
   }
   return(avg);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedPcpu --
 *
 *      Returns the per-PCPU scheduler structure associated with
 *	the specified "pcpu".
 *
 * Results:
 *      Returns the per-PCPU scheduler structure associated with
 *	the specified "pcpu".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE CpuSched_Pcpu *
CpuSchedPcpu(PCPU pcpu)
{
   ASSERT(pcpu < numPCPUs);
   return(&cpuSched.pcpu[pcpu]);
}


/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedPartnerPcpu --
 *
 *     Returns the per-PCU scheduler structure associated with 
 *     the PARTNER (aka hypertwin) of "pcpu". Do not use on 
 *     non-hyperthreaded systems.
 *
 * Results:
 *     Returns partner pcpu
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE CpuSched_Pcpu *
CpuSchedPartnerPcpu(PCPU pcpu)
{
   ASSERT(pcpu < numPCPUs);
   ASSERT(SMP_HTEnabled());
   return(cpuSched.pcpu[pcpu].partner);
}

/*
 *-----------------------------------------------------------------------------
 *
 * PcpuMask --
 *
 *     Returns the bitmask representing pcpu "p", or pcpu "p" along with
 *     its hypertwin, if "withPartner" is TRUE.
 *
 * Results:
 *     Returns desired bitmask
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE CpuMask
PcpuMask(PCPU p, Bool withPartner)
{
   if (withPartner && SMP_HTEnabled()) {
      return (CPUSCHED_AFFINITY(p) | CPUSCHED_AFFINITY(CpuSchedPartnerPcpu(p)->id));
   } else {
      return (CPUSCHED_AFFINITY(p));
   }
}

// exported version of above
CpuMask
CpuSched_PcpuMask(PCPU p, Bool withPartner)
{
   return PcpuMask(p, withPartner);
}

/*
 *-----------------------------------------------------------------------------
 *
 * VcpuMask --
 *
 *      Returns a cpu mask with the bit set that corresponds to "v's" schedIndex
 *
 * Results:
 *      Returns a cpu mask with the bit set that corresponds to "v's" schedIndex
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE CpuMask
VcpuMask(const CpuSched_Vcpu *v)
{
   return (1 << v->schedIndex);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedCellLock --
 *
 *      Acquire exclusive access to scheduler state for cell "c".
 *
 * Results:
 *      Lock for scheduler cell "c" is acquired.
 *      Returns the caller's IRQL level.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE SP_IRQL
CpuSchedCellLock(CpuSched_Cell *c)
{
   return(SP_LockIRQ(&c->lock, CPUSCHED_IRQL));
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedCellTryLock --
 *
 *      Try once to acquire exclusive access to scheduler state
 *	for cell "c".
 *
 * Results:
 *      Sets "acquired".  If "acquired", lock for scheduler cell has
 *	been successfully acquired. Returns the caller's IRQL level.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE SP_IRQL
CpuSchedCellTryLock(CpuSched_Cell *c, Bool *acquired)
{
   return(SP_TryLockIRQ(&c->lock, CPUSCHED_IRQL, acquired));
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedCellUnlock --
 *
 *      Releases exclusive access to scheduler state for cell "c".
 *      Sets the IRQL level to "prevIRQL".
 *
 * Results:
 *      Lock for scheduler cell "c" is released.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
CpuSchedCellUnlock(CpuSched_Cell *c, SP_IRQL prevIRQL)
{
   ASSERT_NO_INTERRUPTS();
   SP_UnlockIRQ(&c->lock, prevIRQL);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedCellIsLocked --
 *
 *      Returns TRUE iff scheduler cell "c" is locked.
 *
 * Results:
 *      Returns TRUE iff scheduler cell "c" is locked.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
CpuSchedCellIsLocked(const CpuSched_Cell *c)
{
   return(SP_IsLockedIRQ(&c->lock));
}


/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVsmpCellLock --
 *
 *      Acquire exclusive access to the scheduler cell associated
 *	with "vsmp".
 *
 * Results:
 *	Lock for scheduler cell associated with "vsmp" is acquired.
 *	Returns the caller's IRQL level.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static SP_IRQL
CpuSchedVsmpCellLock(volatile CpuSched_Vsmp *vsmp)
{
   const uint32 retryPanic = CPUSCHED_LOCK_RETRY_PANIC;
   uint32 retry;

   for (retry = 0; retry < retryPanic; retry++) {
      CpuSched_Cell *checkCell;
      SP_IRQL checkIRQL;

      // snapshot cell w/o holding lock
      checkCell = vsmp->cell;

      // tentatively lock cell
      checkIRQL = CpuSchedCellLock(checkCell);

      // done if snapshot still valid
      if (vsmp->cell == checkCell) {
         return(checkIRQL);
      }

      // snapshot invalid, so unlock and retry
      CpuSchedCellUnlock(checkCell, checkIRQL);

      // retries should be rare, check for unreasonable values
      ASSERT(retry < CPUSCHED_LOCK_RETRY_ASSERT);
      if (CPUSCHED_DEBUG) {
         ASSERT(retry < CPUSCHED_LOCK_RETRY_DEBUG);
      }
   }

   // shouldn't happen
   Panic("CpuSched: VsmpCellLock: exceeded max retries (%u)\n", retryPanic);
}

// simple wrapper for CpuSchedCellUnlock()
static INLINE void
CpuSchedVsmpCellUnlock(volatile CpuSched_Vsmp *vsmp, SP_IRQL prevIRQL)
{
   CpuSchedCellUnlock(vsmp->cell, prevIRQL);
}

// simple wrapper for CpuSchedCellIsLocked()
static INLINE Bool
CpuSchedVsmpCellIsLocked(const CpuSched_Vsmp *vsmp)
{
   return(CpuSchedCellIsLocked(vsmp->cell));
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedLockAllCells --
 *
 *      Acquire exclusive access to all scheduler cells.
 *
 * Results:
 *	Locks for all scheduler cell are acquired.
 *	Returns the caller's IRQL level.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static SP_IRQL
CpuSchedLockAllCells(void)
{
   SP_IRQL prevIRQL;
   uint32 id;

   // sanity check
   ASSERT(cpuSched.nCells > 0);

   // lock first cell
   prevIRQL = CpuSchedCellLock(&cpuSched.cell[0]);

   // lock remaining cells, ordered by ascending id
   for (id = 1; id < cpuSched.nCells; id++) {
      SP_IRQL checkIRQL;
      checkIRQL = CpuSchedCellLock(&cpuSched.cell[id]);
      ASSERT(checkIRQL == CPUSCHED_IRQL);
   }

   return(prevIRQL);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedUnlockAllCells --
 *
 *      Releases exclusive access to all scheduler cells.
 *	Sets the IRQL level to "prevIRQL".
 *
 * Results:
 *	Locks for all scheduler cell are released.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedUnlockAllCells(SP_IRQL prevIRQL)
{
   uint32 id;

   // sanity checks
   ASSERT_NO_INTERRUPTS();
   ASSERT(cpuSched.nCells > 0);

   // unlock all but first cell, ordered by descending id
   for (id = cpuSched.nCells - 1; id > 0; id--) {
      CpuSchedCellUnlock(&cpuSched.cell[id], CPUSCHED_IRQL);
   }

   // unlock first cell
   CpuSchedCellUnlock(&cpuSched.cell[0], prevIRQL);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedAllCellsAreLocked --
 *
 *      Returns TRUE iff all scheduler cells are locked.
 *
 * Results:
 *      Returns TRUE iff all scheduler cells are locked.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
#ifdef	VMX86_DEBUG
static Bool
CpuSchedAllCellsAreLocked(void)
{
   FORALL_CELLS_UNLOCKED(c) {
      if (!CpuSchedCellIsLocked(c)) {
         return(FALSE);
      }
   } CELLS_UNLOCKED_DONE;
   return(TRUE);
}
#endif

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedSnapLock --
 *
 *      Acquire exclusive access to scheduler procfs snapshot data.
 *
 * Results:
 *      Lock for CpuSched procfs snapshot data is acquired.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
CpuSchedSnapLock(void)
{
   SP_Lock(&cpuSched.procSnap.lock);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedSnapUnlock --
 *
 *      Releases exclusive access to scheduler procfs snapshot data.
 *
 * Results:
 *      Lock for CpuSched procfs snapshot data is released.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
CpuSchedSnapUnlock(void)
{
   SP_Unlock(&cpuSched.procSnap.lock);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedSnapIsLocked --
 *
 *      Returns TRUE iff scheduler procfs snapshot data is locked.
 *
 * Results:
 *      Returns TRUE iff scheduler procfs snapshot data is locked.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
CpuSchedSnapIsLocked(void)
{
   return(SP_IsLocked(&cpuSched.procSnap.lock));
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedPackageHaltLock --
 *
 *     Acquires exclusive access to halt lock for package containing "p"
 *
 * Results:
 *     None.
 *
 * Side effects:
 *     Acquires exclusive access to halt lock for package containing "p"
 *     Disables interrupts
 *
 *-----------------------------------------------------------------------------
 */
static INLINE void
CpuSchedPackageHaltLock(PCPU p)
{
   CpuSched_Pcpu *pcpu = CpuSchedPcpu(SMP_GetPackageInfo(p)->logicalCpus[0]);
   SP_LockIRQ(&pcpu->haltLock, SP_IRQL_KERNEL);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedPackageHaltUnlock --
 *
 *     Relinquishes exclusive access to halt lock for package containing "p"
 *
 * Results:
 *     None.
 *
 * Side effects:
 *     Relinquishes exclusive access to halt lock for package containing "p"
 *     May re-enable interrupts (according to prevIRQL stored in lock)
 *
 *-----------------------------------------------------------------------------
 */

static INLINE void
CpuSchedPackageHaltUnlock(PCPU p)
{
   CpuSched_Pcpu *pcpu = CpuSchedPcpu(SMP_GetPackageInfo(p)->logicalCpus[0]);
   SP_UnlockIRQ(&pcpu->haltLock, SP_GetPrevIRQ(&pcpu->haltLock));
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedPackageHaltIsLocked --
 *
 *     Returns true iff the halt lock on the package containing "p" is locked
 *
 * Results:
 *     Returns true iff the halt lock on the package containing "p" is locked
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE Bool
CpuSchedPackageHaltIsLocked(PCPU p)
{
   CpuSched_Pcpu *pcpu = CpuSchedPcpu(SMP_GetPackageInfo(p)->logicalCpus[0]);
   return SP_IsLockedIRQ(&pcpu->haltLock);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuEventLock --
 *
 *      Acquire exclusive access to both scheduler cell associated
 *	with "vcpu" and to the event queue containing "vcpu" (if any).
 *
 * Results:
 *	Sets "eventQueue" to the event queue containing "vcpu",
 *	or to NULL if no event queue contains "vcpu".  If non-NULL,
 *	sets "eventIRQL" to the caller's IRQL level, and the lock
 *	for "eventQueue" is acquired.  The scheduler cell lock for
 *      "vcpu" is also acquired.  Returns the IRQL level when the
 *	scheduler cell lock was acquired.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static SP_IRQL
CpuSchedVcpuEventLock(volatile CpuSched_Vcpu *vcpu,
                      EventQueue **eventQueue,
                      SP_IRQL *eventIRQL)
{
   const uint32 retryPanic = CPUSCHED_LOCK_RETRY_PANIC;
   uint32 retry;

   // Due to lock ordering, we must acquire the EventQueue lock
   // before the CpuSched lock.  But we need to hold the CpuSched
   // lock to consistently determine which EventQueue to lock.
   // The following code speculatively determines which EventQueue
   // to lock, and then retries in the rare case that an actual
   // race occurs.

   // OPT: consider using alternative Trylock-based implementation

   for (retry = 0; retry < retryPanic; retry++) {
      SP_IRQL checkIRQL, schedIRQL;
      CpuSched_RunState checkState;
      EventQueue *checkQueue;
      uint32 checkEvent;

      // snapshot fields w/o holding lock
      checkState = vcpu->runState;
      checkEvent = vcpu->waitEvent;      

      // tentatively lock event queue, if waiting
      if ((checkState == CPUSCHED_WAIT) ||
          (checkState == CPUSCHED_BUSY_WAIT)) {
         checkQueue = EventQueue_Find(checkEvent);
         checkIRQL = EventQueue_Lock(checkQueue);
      } else {
         checkQueue = NULL;
         checkIRQL = SP_IRQL_NONE;
      }
                  
      // tentatively lock scheduler
      schedIRQL = CpuSchedVsmpCellLock(vcpu->vsmp);
      
      // done if snapshot still valid
      if ((vcpu->runState == checkState) &&
          (vcpu->waitEvent == checkEvent)) {
         *eventQueue = checkQueue;
         *eventIRQL = checkIRQL;
         return(schedIRQL);
      }

      // snapshot invalid, so unlock and retry
      CpuSchedVsmpCellUnlock(vcpu->vsmp, schedIRQL);
      if (checkQueue != NULL) {
         EventQueue_Unlock(checkQueue, checkIRQL);
      }

      // retries should be rare, check for unreasonable values
      ASSERT(retry < CPUSCHED_LOCK_RETRY_ASSERT);
      if (CPUSCHED_DEBUG) {
         ASSERT(retry < CPUSCHED_LOCK_RETRY_DEBUG);
      }
   }

   // shouldn't happen
   Panic("CpuSched: VcpuEventLock: exceeded max retries (%u)\n", retryPanic);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedNumVsmps --
 *
 *      Returns the current number of vsmps managed by all
 *	scheduler cells.
 *
 * Results:
 *      Returns the current number of vsmps managed by all
 *	scheduler cells.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static uint32
CpuSchedNumVsmps(void)
{
   uint32 numVsmps = 0;
   ASSERT(CpuSchedAllCellsAreLocked());
   FORALL_CELLS(c) {
      numVsmps += c->vsmps.len;
   } CELLS_DONE;
   return(numVsmps);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVsmpIsSystemIdle --
 *
 *	Returns TRUE iff "vsmp" is a dedicated system idle world.
 *
 * Results:
 *	Returns TRUE iff "vsmp" is a dedicated system idle world.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
CpuSchedVsmpIsSystemIdle(const CpuSched_Vsmp *vsmp)
{
   return(World_IsIDLEWorld(vsmp->leader));
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedIsMP --
 *
 *      Returns TRUE iff "vsmp" is a multiprocessor VM
 *	(i.e. more than one vcpu).
 *
 * Results:
 *      Returns TRUE iff "vsmp" is a multiprocessor VM.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
CpuSchedIsMP(const CpuSched_Vsmp *vsmp)
{
   return(vsmp->vcpus.len > 1);
}


/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedVsmpStrictCosched --
 *
 *      Returns TRUE iff this vsmp should use strict coscheduling, due to
 *      either a global or per-vsmp strict configuration.
 *
 * Results:
 *      Returns TRUE iff this vsmp should use strict coscheduling.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE Bool
CpuSchedVsmpStrictCosched(const CpuSched_Vsmp *vsmp)
{
   return (!vsmp->cell->config.relaxCosched || vsmp->strictCosched);
}


/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedVcpuNeedsCosched --
 *
 *      Returns TRUE iff "vcpu" must be run in order to run any vcpu in the vsmp.
 *
 *      The vcpu could need coscheduling because we're using strict cosched
 *      or because it's skewed too far behind its partner vcpus.
 *
 * Results:
 *      Returns TRUE iff "vcpu" must be run in order to run any vcpu in the vsmp.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
static Bool
CpuSchedVcpuNeedsCosched(const CpuSched_Vcpu *vcpu)
{
   CpuSchedConfig *conf = &vcpu->vsmp->cell->config;

   if (!CpuSchedIsMP(vcpu->vsmp)
      || vcpu->waitState == CPUSCHED_WAIT_IDLE) {
      return FALSE;
   }

   if (CpuSchedVsmpStrictCosched(vcpu->vsmp)
       || vcpu->intraSkew > conf->intraSkewThreshold) {
      // either we've enabled strict coscheduling, or
      // this vcpu is too far behind its bretheren, so
      // we'd better get around to running it pretty soon
      return (TRUE);
   }

   return (FALSE);
}


/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuRunOrBwait --
 *
 *      Returns TRUE if "vcpu" is currently executing; i.e.
 *	"vcpu" is in the RUN or BUSY_WAIT run states.
 *
 * Results:
 *      Returns TRUE if "vcpu" is currently executing.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
CpuSchedVcpuRunOrBwait(const CpuSched_Vcpu *vcpu)
{
   return((vcpu->runState == CPUSCHED_RUN) || 
          (vcpu->runState == CPUSCHED_BUSY_WAIT)); 
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuIsWaiting --
 *
 *      Returns TRUE if "vcpu" is currently waiting; i.e.
 *	"vcpu" is in the WAIT or BUSY_WAIT run states.
 *
 * Results:
 *      Returns TRUE if "vcpu" is currently waiting.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
CpuSchedVcpuIsWaiting(const CpuSched_Vcpu *vcpu)
{
   return((vcpu->runState == CPUSCHED_WAIT) ||
          (vcpu->runState == CPUSCHED_BUSY_WAIT));
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuIsRunnable --
 *
 *      Returns TRUE iff "vcpu" is currently runnable.
 *
 * Results:
 *      Returns TRUE iff "vcpu" is currently runnable.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
CpuSchedVcpuIsRunnable(const CpuSched_Vcpu *vcpu)
{
   return(((vcpu->runState == CPUSCHED_READY)       ||
           (vcpu->runState == CPUSCHED_READY_CORUN) ||
           (vcpu->runState == CPUSCHED_RUN)) &&
          ((vcpu->vsmp->coRunState == CPUSCHED_CO_NONE)  ||
           (vcpu->vsmp->coRunState == CPUSCHED_CO_READY) ||
           (vcpu->vsmp->coRunState == CPUSCHED_CO_RUN)));
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuIsUnmanaged --
 *
 *      Returns TRUE iff "vcpu" is not currently managed by
 *	the scheduler (e.g. uninitialized or zombie).
 *
 * Results:
 *      Returns TRUE iff "vcpu" is not currently managed by
 *	the scheduler.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
CpuSchedVcpuIsUnmanaged(const CpuSched_Vcpu *vcpu)
{
   return((vcpu->runState == CPUSCHED_NEW) ||
          (vcpu->runState == CPUSCHED_ZOMBIE) ||
          (!World_VcpuToWorld(vcpu)->inUse));
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedRunningVcpu --
 *
 *      Returns the currently-executing vcpu on processor "pcpu".
 *
 * Results:
 *      Returns the currently-executing vcpu on processor "pcpu".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE CpuSched_Vcpu *
CpuSchedRunningVcpu(PCPU pcpu)
{
   return(World_CpuSchedVcpu(prdas[pcpu]->runningWorld));
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedWaitStateDisablesCoDesched --
 *
 *      Returns TRUE iff "waitState" represents a wait event that
 *	may depend on another vcpu to make progress in order to
 *	wakeup the event (e.g. monitor sempahore).  This prevents
 *	co-descheduling of all vcpus.
 *
 * Results:
 *      Returns TRUE iff "waitState" disables co-descheduling.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
CpuSchedWaitStateDisablesCoDesched(CpuSched_WaitState waitState)
{
   // Conservatively assume RPCs disable co-descheduling.  Although
   // this restriction could be relaxed, preliminary analysis by John Z.
   // indicates that most RPCs have very low latency, and it isn't
   // worth the complexity to deal with the infrequent exceptions.
   if ((waitState == CPUSCHED_WAIT_SEMAPHORE) ||
       (waitState == CPUSCHED_WAIT_RPC) ||
       (waitState == CPUSCHED_WAIT_LOCK)) {
      return(TRUE);
   } else {
      return(FALSE);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVtimeAhead --
 *
 *      Returns amount of virtual time by which "vsmp" is ahead of
 *	its entitled allocation.  A positive amount indicates
 *	that "vsmp" is ahead; a negative amount indicates that "vsmp"
 *	is behind.
 *
 * Results:
 *      Returns amount of virtual time by which "vsmp" is ahead of
 *	its entitled allocation.  Result may be negative.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE CpuSchedVtime
CpuSchedVtimeAhead(const CpuSched_Vsmp *vsmp)
{
   // OPT: could precompute this sum, e.g. define "cell->vtAheadLimit"
   CpuSchedVtime vtEligible;
   vtEligible = vsmp->cell->vtime + vsmp->cell->config.vtAheadThreshold;
   return(vsmp->vtime.main - vtEligible);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedEnforceMax --
 *
 *      Returns TRUE iff "alloc" specifies a constrained
 *	maximum execution rate.
 *
 * Results:
 *      Returns TRUE iff "alloc" specifies a constrained
 *	maximum execution rate.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
CpuSchedEnforceMax(const CpuSched_Alloc *alloc)
{
   return(alloc->max != CPUSCHED_ALLOC_MAX_NONE);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVsmpVtimePerVcpu --
 *
 *      Returns "vtime" divided by the number of vcpus in "vsmp".
 *	Optimized to avoid expensive divisions in common cases.
 *
 * Results:
 *      Returns "vtime" divided by the number of vcpus in "vsmp".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE CpuSchedVtime
CpuSchedVsmpVtimePerVcpu(const CpuSched_Vsmp *vsmp, CpuSchedVtime vtime)
{
   // uniprocessor: nothing to do
   if (!CpuSchedIsMP(vsmp)) {
      return(vtime);
   }
   
   // multiprocessor: avoid expensive divisions for common sizes
   if (vsmp->vcpus.len == 2) {
      return(vtime / 2);
   } else if (vsmp->vcpus.len == 4) {
      return(vtime / 4);
   } else {
      return(vtime / vsmp->vcpus.len);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedVtimeToTC --
 *
 *     Converts virtual time the approximate corresponding number
 *     of real-time cpu cycles, using the given stride.
 *     Note that this is only an approximation, so it is often true that:
 *     CpuSchedVtimeToTC(CpuSchedTCToVtime(x)) != x
 *
 * Results:
 *     Returns APPROXIMATE cycles corresponding to virtual time vt for 
 *     specified stride.
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE Timer_Cycles
CpuSchedVtimeToTC(CpuSchedStride stride, CpuSchedVtime vt)
{
   if (LIKELY(stride > 0)) {
      // not adequately tested, but seems simple enough
      return (vt/stride) << CPUSCHED_STRIDE1_CYCLES_LG;
   } else {
      return 0;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * CpuSchedTCToVtime --
 *
 *      Converts real-time timer "cycles" into virtual-time units,
 *	using the specified virtual-time "stride".
 *
 * Results:
 *      Returns amount of virtual time corresponding to "cycles"
 *	and the specified "stride".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE CpuSchedVtime
CpuSchedTCToVtime(CpuSchedStride stride, Timer_RelCycles cycles)
{
   if (cycles == 0) {
      // avoid unnecessary work
      return(0);
   } else if (LIKELY((cycles >> 32) == 0)) {
      // use fast unsigned 32x32-bit multiply when possible
      uint32 cycles32 = (uint32) cycles;
      ASSERT(cycles > 0);
      return(((uint64) cycles32 * stride) >> CPUSCHED_STRIDE1_CYCLES_LG);
   } else {
      // use slower signed 64x32-bit multiply when necessary
      return(Muls64x32s64(cycles, stride, CPUSCHED_STRIDE1_CYCLES_LG));
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVtimeScale --
 *
 *      Returns "vtime" scaled by fraction "numer"/"denom".
 *	Requires that "denom" is non-zero.
 *
 * Results:
 *      Returns "vtime" scaled by fraction "numer"/"denom".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE CpuSchedVtime
CpuSchedVtimeScale(CpuSchedVtime vtime, uint32 numer, uint32 denom)
{
   int32 n, d;
   uint64 vt;

   // sanity check
   ASSERT(denom > 0);

   // carefully avoid signed/unsigned issues
   //   use unsigned vt so that shift works
   //   use signed n,d with signed vtime computations
   vt = (vtime < 0) ? -vtime : vtime;
   n = numer;
   d = denom;

   // carefully avoid overflow
   if (LIKELY((vt >> 32) == 0)) {
      // small vtime: multiply first for better accuracy
      return((vtime * n) / d);
   } else {
      // large vtime: divide first to avoid overflow
      return((vtime / d) * n);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuIsIdle --
 *
 *      Returns TRUE iff "vcpu" is a dedicated or temporary idle vcpu.
 *
 * Results:
 *      Returns TRUE iff "vcpu" is idle.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
CpuSchedVcpuIsIdle(const CpuSched_Vcpu *vcpu)
{
   // dedicated idle vcpu or idling busy-waiting vcpu?
   return(vcpu->idle || (vcpu->runState == CPUSCHED_BUSY_WAIT));
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedPcpuIsIdle --
 *
 *	Returns TRUE iff "p" is currently running an idle world
 *	or busy-waiting.
 *
 * Results:
 *	Returns TRUE iff "p" is currently running an idle world
 *	or busy-waiting.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
CpuSchedPcpuIsIdle(PCPU p)
{
   return(prdas[p]->idle);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedPartnerIsIdle --
 *
 *	Returns TRUE iff the partner pcpu to "p" is currently running
 *	an idle world or busy-waiting.
 *
 * Results:
 *	Returns TRUE iff the partner pcpu to "p" is currently running
 *	an idle world or busy-waiting.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
CpuSchedPartnerIsIdle(PCPU p)
{
   ASSERT(SMP_HTEnabled());
   return(CpuSchedPcpuIsIdle(CpuSchedPcpu(p)->partner->id));
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedPackageIsIdle --
 *
 *	Returns TRUE iff package containing "p" is currently running
 *	only worlds that are idle or busy-waiting.  Identical to
 *	CpuSchedPcpuIsIdle() if hyperthreading not enabled.
 *
 * Results:
 *	Returns TRUE iff package containing "p" is currently running
 *	only worlds that are idle or busy-waiting.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
CpuSchedPackageIsIdle(PCPU p)
{
   if (SMP_HTEnabled()) {
      return(CpuSchedPcpuIsIdle(p) && CpuSchedPartnerIsIdle(p));
   } else {
      return(CpuSchedPcpuIsIdle(p));
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedGetIdleVcpu --
 *
 *     Returns the dedicated idle vcpu for processor "p"
 *
 * Results:
 *     Returns the dedicated idle vcpu for processor "p"
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
CpuSched_Vcpu*
CpuSchedGetIdleVcpu(PCPU p)
{
   return (World_CpuSchedVcpu(World_GetIdleWorld(p)));
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuStateStart --
 *
 *      Updates "m" to start tracking per-state metrics (such as
 *	elapsed time) before starting new vcpu state.
 *
 * Results:
 *      Modifies "m".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
CpuSchedVcpuStateStart(CpuSched_Vcpu *vcpu, CpuSched_StateMeter *m)
{
   ASSERT(CpuSchedVsmpCellIsLocked(vcpu->vsmp));
   m->count++;
   m->start = vcpu->vsmp->cell->now;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuWaitStart --
 *
 *      Updates "m" to start tracking per-state wait metrics (such as
 *	elapsed time and elapsed virtual time) before starting new
 *	vcpu wait state.
 *
 * Results:
 *      Modifies "m".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
CpuSchedVcpuWaitStart(CpuSched_Vcpu *vcpu, CpuSched_StateMeter *m)
{
   ASSERT(CpuSchedVsmpCellIsLocked(vcpu->vsmp));
   CpuSchedVcpuStateStart(vcpu, m);
   m->vtStart = vcpu->vsmp->cell->vtime;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuStateDone --
 *
 *      Updates "m" to maintain per-state metrics after leaving
 *	a vcpu state.  Returns the elapsed time since the
 *	corresponding CpuSchedVcpuStateStart().
 *
 * Results:
 *      Modifies "m".  Returns elapsed time since state started.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Timer_Cycles
CpuSchedVcpuStateDone(CpuSched_Vcpu *vcpu, CpuSched_StateMeter *m)
{
   // sanity checks
   ASSERT(CpuSchedVsmpCellIsLocked(vcpu->vsmp));
   ASSERT(m->start > 0);

   if (LIKELY(m->start > 0)) {
      Timer_AbsCycles now = vcpu->vsmp->cell->now;

      // sanity check
      ASSERT(now >= m->start);
      
      if (LIKELY(now > m->start)) {
         // update elapsed time, reset start time
         Timer_Cycles elapsed = now - m->start;
         m->elapsed += elapsed;
         m->start = 0;

         if (CPUSCHED_STATE_HISTOGRAMS) {
            Histogram_Insert(m->histo, elapsed);
         }

         return(elapsed);
      }
   }

   // no elapsed time
   return(0);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuWaitDone --
 *
 *      Updates "m" to maintain per-state wait metrics (such as
 *	elapsed time and elapsed virtual time) after leaving vcpu
 *	wait state.  Updates "vcpu" virtual time and credit to
 *	account for elapsed wait time.
 *
 * Results:
 *      Modifies "m" and "vcpu".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedVcpuWaitDone(CpuSched_Vcpu *vcpu, CpuSched_StateMeter *m)
{
   // update real time
   CpuSchedVcpuStateDone(vcpu, m);

   // sanity check
   ASSERT(m->vtStart > 0);

   // update virtual time
   if (LIKELY(m->vtStart > 0)) {
      CpuSchedVtime vtime = vcpu->vsmp->cell->vtime;

      // sanity check
      ASSERT(vtime >= m->vtStart);

      if (LIKELY(vtime > m->vtStart)) {
         // update vtime, credit to reflect wait time
         CpuSchedVtime vtElapsed = vtime - m->vtStart;
         CpuSchedVcpuChargeWait(vcpu, vtElapsed);         

         // reset start vtime
         m->vtStart = 0;         
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuWaitUpdate --
 *
 *      Updates per-state wait metrics associated with "vcpu" to 
 *	reflect elapsed portion of in-progress waits.  Updates
 *	"vcpu" virtual time and credit to account for elapsed
 *	wait time.
 *
 * Results:
 *      Modifies "vcpu".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedVcpuWaitUpdate(CpuSched_Vcpu *vcpu)
{
   CpuSched_StateMeter *m;
   uint32 count;

   switch (vcpu->runState) {
   case CPUSCHED_WAIT:
      // update in-progress wait times, preserve count
      m = &vcpu->runStateMeter[CPUSCHED_WAIT];
      count = m->count;
      CpuSchedVcpuWaitDone(vcpu, m);
      CpuSchedVcpuWaitStart(vcpu, m);
      m->count = count;
      break;

   case CPUSCHED_BUSY_WAIT:
      // OK to do nothing since busy-waits should be short
      break;
      
   default:
      // nothing to do
      break;
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedPreemptEnabledStatsUpdate --
 *
 *      Called before re-enabling preemption to update the histograms for
 *      preemption disable time.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Updates preemption disable stats.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE void
CpuSchedPreemptEnabledStatsUpdate(CpuSched_Vcpu *vcpu)
{
   if (CPUSCHED_PREEMPT_STATS && vcpu->disablePreemptTimeHisto
       && vcpu->disablePreemptStartTime != 0) {
      TSCCycles now = RDTSC();
      ASSERT(now >= vcpu->disablePreemptStartTime);
      Histogram_Insert(vcpu->disablePreemptTimeHisto,
                       now - vcpu->disablePreemptStartTime);
      vcpu->disablePreemptStartTime = 0;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * CpuSchedSharesToStride --
 *
 *      Computes proportional-share stride value, which is a
 *      virtual time increment inversely proportional to "shares".
 *
 * Results:
 *      Returns stride inversely proportional to "shares".
 *      Returns CPUSCHED_STRIDE_MAX if "shares" is zero or negative.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE CpuSchedStride
CpuSchedSharesToStride(int32 shares)
{
   // explicit check to avoid division by zero
   return((shares > 0) ? CPUSCHED_STRIDE1 / shares : CPUSCHED_STRIDE_MAX);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedUnitsToBaseShares --
 *
 *      Converts the value "quantity," which is measured in "units,"
 *      to the corresponding number of base shares.
 *
 * Results:
 *      Number of base shares equivalent to quantity.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE uint32
CpuSchedUnitsToBaseShares(uint32 quantity, Sched_Units units)
{
   ASSERT(quantity < CPUSCHED_MAX_UINT32 / CPUSCHED_BASE_PER_PACKAGE);
   return (quantity * CPUSCHED_BASE_PER_PACKAGE) /
      cpuSchedConst.unitsPerPkg[units];
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedBaseSharesToUnits --
 *
 *      Returns the number of "units" that correspond to "bshares".
 *
 * Results:
 *      Returns the number of "units" that correspond to "bshares".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE uint32
CpuSchedBaseSharesToUnits(uint32 bshares, Sched_Units units)
{
   ASSERT(bshares < CPUSCHED_MAX_UINT32 / cpuSchedConst.unitsPerPkg[units]);
   return (bshares * cpuSchedConst.unitsPerPkg[units])
      / CPUSCHED_BASE_PER_PACKAGE;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_BaseSharesToUnits --
 *
 *      Returns the number of "units" that correspond to "bshares".
 *
 * Results:
 *      Returns the number of "units" that correspond to "bshares".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
uint32
CpuSched_BaseSharesToUnits(uint32 bshares, Sched_Units units)
{
   return CpuSchedBaseSharesToUnits(bshares, units);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSched_UsageToSec --
 *
 *     Convert timer "cycles" into seconds and microseconds, assuming that
 *     cycles were measured on a single LCPU if the system is hyperthreaded.
 *     Effectively, this halves the resulting estimate on hyperthreaded
 *     systems.
 *
 * Results:
 *     Sets "sec"  to the number of whole seconds in "cycles".
 *     Sets "usec" to the number of remaining microseconds in "cycles".
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
void
CpuSched_UsageToSec(Timer_Cycles cycles, uint64 *sec, uint32 *usec)
{
   Timer_TCToSec(cycles / SMP_LogicalCPUPerPackage(), sec, usec);
}


/*
 *-----------------------------------------------------------------------------
 *
 * CpuSched_PercentTotal --
 * 
 *     Total percentage of all pcpus in the system.
 *
 * Results:
 *     Return total percentage of all pcpus in the system.
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
uint32
CpuSched_PercentTotal(void)
{
   return cpuSchedConst.percentTotal;
}


/*
 *----------------------------------------------------------------------
 *
 * CpuSched_MarkRescheduleLocal --
 *
 *      Request a new scheduling decision on the local processor.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Updates local scheduling state.
 *
 *----------------------------------------------------------------------
 */
INLINE void
CpuSched_MarkRescheduleLocal(void)
{
   // set local reschedule flag
   myPRDA.reschedule = TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * CpuSchedMarkRescheduleInt --
 *
 *      Request a new scheduling decision on processor "pcpu".
 *      Iff allowIPI is TRUE, this may send an IPI to kick the
 *      remote processor and invoke the scheduler there.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Updates scheduling state for "pcpu".
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedMarkRescheduleInt(PCPU pcpu, Bool allowIPI)
{
   // sanity check
   ASSERT(pcpu < numPCPUs);
   
   if (pcpu == MY_PCPU) {
      // local case: no IPI, just set flag
      CpuSched_MarkRescheduleLocal();
   } else {
      // remote case: set flag and send IPI, if necessary
      volatile PRDA *p = prdas[pcpu];
      if (!p->reschedule) {
         // set remote flag
         p->reschedule = TRUE;
         
         if (allowIPI) {
            // send IPI to ensure remote reschedule flag checked soon;
            //   skip if idle, since idle loop polls flag frequently
            //   unless the idle loop is in a halt state right now
            if (!p->idle || p->halted) {
               CpuSchedSendReschedIPI(pcpu);
            }
         }
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_MarkReschedule --
 *
 *      Exported wrapper for above. Requests reschedule, may send IPI.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Updates scheduling state for "pcpu".
 *
 *----------------------------------------------------------------------
 */
void
CpuSched_MarkReschedule(PCPU pcpu)
{
   CpuSchedMarkRescheduleInt(pcpu, TRUE);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedReschedIntHandler --
 *
 *      Interrupt handler for a resched request.
 *
 * Results: 
 *      None.
 *      
 * Side effects:
 *      Forces reschedule flag to be checked.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedReschedIntHandler(UNUSED_PARAM(void *clientData),
                          UNUSED_PARAM(uint32 vector))
{
   // The sender should have already set our reschedule flag.
   // This is effectively a null handler, and simply forces
   // myPRDA.reschedule to be checked during the interrupt
   // return path.

   // update stats
   CpuSchedPcpu(MY_PCPU)->stats.ipi++;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedSendReschedIPI --
 *
 *      Send an interprocessor interrupt to "pcpu",
 *	causing it to reschedule soon.
 *
 * Results: 
 *      None.
 *      
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedSendReschedIPI(PCPU pcpu)
{
   // ensure not sending to self
   ASSERT(pcpu != MY_PCPU);

   // send interrupt, no ack required
   APIC_SendIPI(pcpu, IDT_RESCHED_VECTOR);

   // debugging
   CpuSchedLogEvent("send-rs-ipi", pcpu);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_Reschedule --
 *
 *      Invoke the scheduler unconditionally.
 *	Must not be called while busy-waiting.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May yield currently-executing world and modify scheduler state.
 *
 *----------------------------------------------------------------------
 */
void 
CpuSched_Reschedule(void)
{
   CpuSched_Vsmp *vsmp = World_CpuSchedVsmp(MY_RUNNING_WORLD);
   SP_IRQL prevIRQL;

   // sanity check: don't reschedule while busy-waiting
   ASSERT(World_CpuSchedRunState(MY_RUNNING_WORLD) != CPUSCHED_BUSY_WAIT);

   // reschedule
   prevIRQL = CpuSchedVsmpCellLock(vsmp);
   CpuSchedDispatch(prevIRQL, TRUE);
}

/*
 * Grouping Operations
 */


/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVsmpUpdateGroup --
 *
 *      Updates scheduler state associated with "vsmp" to 
 *	reflect current group membership.  Caller must hold
 *	scheduler cell lock associated with "vsmp".
 *
 * Results:
 *      Modifies "m".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedVsmpUpdateGroup(CpuSched_Vsmp *vsmp)
{
   World_Handle *world = vsmp->leader;

   // sanity check
   ASSERT(CpuSchedVsmpCellIsLocked(vsmp));

   // initialize vtime path, mark valid
   Sched_TreeLock();
   Sched_GroupPathCopy(&vsmp->vtime.path, &world->sched.group.path);
   world->sched.group.cpuValid = TRUE;
   Sched_TreeUnlock();
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedGroupVtimes --
 *
 *      Sets "vtime" and "vtimeLimit" to consistent snapshots of the
 *	current virtual times associated with the scheduler group
 *	identified by "id".
 *
 * Results:
 *      Sets "vtime" and "vtimeLimit" to the current virtual times
 *	associated with group "id".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedGroupVtimes(Sched_GroupID id,
                    CpuSchedVtime *vtime,
                    CpuSchedVtime *vtimeLimit,
                    CpuSchedStride *stride)
{
   // n.b. lock-free, accesses static storage and uses versioned atomic
   Sched_Group *group = Sched_TreeLookupGroupSlot(id);
   CpuSched_GroupState *cpuGroup = &group->cpu;

   if (LIKELY(group->groupID == id)) {
      CPUSCHED_VERSIONED_ATOMIC_READ_BEGIN(&cpuGroup->vtimeVersion);
      *vtime = cpuGroup->vtime;
      *vtimeLimit = cpuGroup->vtimeLimit;
      *stride = cpuGroup->stride;
      CPUSCHED_VERSIONED_ATOMIC_READ_END(&cpuGroup->vtimeVersion);
   } else {
      if (CPUSCHED_DEBUG) {
         LOG(0, "group slot mismatch: id=%u, slot=%u", id, group->groupID);
      }
      *vtime = CPUSCHED_VTIME_MAX;
      *vtimeLimit = CPUSCHED_VTIME_MAX;
      *stride = CPUSCHED_STRIDE_MAX;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedPcpuGroupVtimeCacheInvalidate --
 *
 *      Invalidates any group virtual times cached for "pcpu".
 *
 * Results:
 *      Invalidates any group virtual times cached for "pcpu".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
CpuSchedPcpuGroupVtimeCacheInvalidate(CpuSched_Pcpu *pcpu)
{
   // advance generation count
   CpuSchedGroupVtimeCache *c = &pcpu->groupVtimes;
   c->generation++;

   // handle generation wraparound
   if ((c->generation == 0) ||
       VMK_STRESS_DEBUG_COUNTER(CPU_GROUP_CACHE_WRAP)) {
      // clear all entries, advance generation count
      memset(c, 0, sizeof(CpuSchedGroupVtimeCache));
      c->generation = 1;
      CpuSchedLog("handled generation wraparound");
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedPcpuGroupVtimeCacheLookup --
 *
 *      Obtains (and, if not already cached, caches for "pcpu") 
 *	current virtual times associated with the scheduler group
 *	identified by "id".  Returns a valid cache entry for "pcpu"
 *	containing the cached virtual times for group "id".
 *
 * Results:
 *	Returns a valid cache entry for "pcpu" containing cached
 *	virtual times for the group identified by "id".
 *
 * Side effects:
 *      May fetch group info into vtime cache.
 *
 *----------------------------------------------------------------------
 */
static INLINE CpuSchedGroupVtimeCacheEntry *
CpuSchedPcpuGroupVtimeCacheLookup(CpuSched_Pcpu *pcpu, Sched_GroupID id)
{
   CpuSchedGroupVtimeCache *c = &pcpu->groupVtimes;
   CpuSchedGroupVtimeCacheEntry *e = &c->cache[id & SCHED_GROUPS_MASK];

   if (CPUSCHED_GROUP_CACHE_STATS) {
      pcpu->stats.groupLookups++;
   }

   // cache hit?
   if (e->generation == c->generation) {
      ASSERT(e->id == id);
      if (e->id == id) {
         if (CPUSCHED_GROUP_CACHE_STATS) {
            pcpu->stats.groupHits++;
         }
         return(e);
      }
   }

   // cache miss
   e->generation = c->generation;
   e->id = id;
   CpuSchedGroupVtimes(id, &e->vtime, &e->vtimeLimit, &e->stride);
   return(e);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedGroupVtime --
 *
 *      Returns the current virtual time associated with the 
 *	scheduler group identified by "id".
 *
 * Results:
 *      Returns the current virtual time associated with the 
 *	scheduler group identified by "id".
 *
 * Side effects:
 *      May fetch group info into vtime cache.
 *
 *----------------------------------------------------------------------
 */
static INLINE CpuSchedVtime
CpuSchedGroupVtime(Sched_GroupID id)
{
   CpuSched_Pcpu *pcpu = CpuSchedPcpu(MY_PCPU);
   return(CpuSchedPcpuGroupVtimeCacheLookup(pcpu, id)->vtime);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedGroupStride --
 *
 *      Returns stride associated with group "id"
 *
 * Results:
 *      Returns stride associated with group "id"
 *
 * Side effects:
 *      May fetch group info into vtime cache.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE CpuSchedVtime
CpuSchedGroupStride(Sched_GroupID id)
{
   CpuSched_Pcpu *pcpu = CpuSchedPcpu(MY_PCPU);
   return(CpuSchedPcpuGroupVtimeCacheLookup(pcpu, id)->stride);
}


/*
 *----------------------------------------------------------------------
 *
 * CpuSchedGroupVtimeLimit --
 *
 *      Returns the current virtual time limit associated with the
 *	scheduler group identified by "id".
 *
 * Results:
 *      Returns the current virtual time limit associated with the
 *	scheduler group identified by "id".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE CpuSchedVtime
CpuSchedGroupVtimeLimit(Sched_GroupID id)
{
   CpuSched_Pcpu *pcpu = CpuSchedPcpu(MY_PCPU);
   return(CpuSchedPcpuGroupVtimeCacheLookup(pcpu, id)->vtimeLimit);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVtimeContextMainCompare --
 *
 *      Compares the main component of the virtual times associated
 *	with "a" and "b".
 *      aBonus is converted to vtime and subtracted from a's vtime
 *      while bBonus is converted and subtraced from b's vtime.
 *      Returns a negative, zero, or positive value 
 *	when "a" < "b", "a" == "b", and "a" > "b", respectively.
 *
 * Results:
 *	Returns a negative, zero, or positive value when
 *	"a" < "b", "a" == "b", and "a" > "b", respectively.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE CpuSchedVtime
CpuSchedVtimeContextMainCompare(const CpuSched_VtimeContext *a,
                                Timer_RelCycles aBonus,
                                const CpuSched_VtimeContext *b,
                                Timer_RelCycles bBonus)
{
   CpuSchedVtime vtBonusA = CpuSchedTCToVtime(a->stride, aBonus);
   CpuSchedVtime vtBonusB = CpuSchedTCToVtime(b->stride, bBonus);

   return (a->main - vtBonusA) - (b->main - vtBonusB);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVtimeContextExtraCompare --
 *
 *      Compares the extra component of the virtual times associated
 *	with "a" and "b", based on group paths and virtual times.
 *      aBonus is converted to vtime and subtracted from a's vtime
 *      while bBonus is converted and subtraced from b's vtime.
 *	Returns a negative, zero, or positive value when
 *	"a" < "b", "a" == "b", and "a" > "b", respectively.
 *
 * Results:
 *	Returns a negative, zero, or positive value when
 *	"a" < "b", "a" == "b", and "a" > "b", respectively.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE CpuSchedVtime
CpuSchedVtimeContextExtraCompare(const CpuSched_VtimeContext *a,
                                 Timer_RelCycles aBonus,
                                 const CpuSched_VtimeContext *b,
                                 Timer_RelCycles bBonus)
{
   uint32 i;

   // sanity checks: all paths start with root group
   ASSERT(a->path.level[0] == SCHED_GROUP_ID_ROOT);
   ASSERT(b->path.level[0] == SCHED_GROUP_ID_ROOT);

   for (i = 1; i < SCHED_GROUP_PATH_LEN; i++) {
      if ((a->path.level[i] != b->path.level[i]) ||
          (a->path.level[i] == SCHED_GROUP_ID_INVALID)) {
         CpuSchedVtime vtA, vtB, vtBonusB, vtBonusA;

         if (a->path.level[i] == SCHED_GROUP_ID_INVALID) {
            vtA = a->extra;
            vtBonusA = CpuSchedTCToVtime(a->stride, aBonus);
         } else {
            vtA = CpuSchedGroupVtime(a->path.level[i]);
            vtBonusA = CpuSchedTCToVtime(CpuSchedGroupStride(a->path.level[i]), aBonus);
         }

         if (b->path.level[i] == SCHED_GROUP_ID_INVALID) {
            vtB = b->extra;
            vtBonusB = CpuSchedTCToVtime(b->stride, bBonus);
         } else {
            vtB = CpuSchedGroupVtime(b->path.level[i]);
            vtBonusB = CpuSchedTCToVtime(CpuSchedGroupStride(b->path.level[i]), bBonus);
         }
         
         CpuSchedLogEvent("msBonusA", Timer_TCToMS(aBonus));
         CpuSchedLogEvent("msBonusB", Timer_TCToMS(bBonus));

         return (vtA - vtBonusA) - (vtB - vtBonusB);
      }
   }

   NOT_REACHED();
   return(0);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVtimeContextCompare --
 *
 *      Compares the specified component of the virtual times
 *	(extra component if "extra" set, otherwise main component)
 *	associated with "a" and "b".  Returns a negative, zero, or
 *	positive value when "a" < "b", "a" == "b", and "a" > "b",
 *	respectively.
 *
 *      aBonus is converted to vtime and subtracted from a's vtime,
 *      and bBbonus is converted to vtime and subtraced from b's
 *      vtime.
 *
 * Results:
 *	Returns a negative, zero, or positive value when
 *	"a" < "b", "a" == "b", and "a" > "b", respectively.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE CpuSchedVtime
CpuSchedVtimeContextCompare(const CpuSched_VtimeContext *a,
                            Timer_RelCycles aBonus,
                            const CpuSched_VtimeContext *b,
                            Timer_RelCycles bBonus,
                            Bool extra)
{
   if (extra) {
      return(CpuSchedVtimeContextExtraCompare(a, aBonus, b, bBonus));
   } else {
      return(CpuSchedVtimeContextMainCompare(a, aBonus, b, bBonus));
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVtimeContextEqual --
 *
 *      Returns TRUE iff all components of vtime contexts
 *	"a" and "b" are identical.
 *
 * Results:
 *      Returns TRUE iff all components of vtime contexts
 *	"a" and "b" are identical.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
CpuSchedVtimeContextEqual(const CpuSched_VtimeContext *a,
                          const CpuSched_VtimeContext *b)
{
   return((a->main == b->main) &&
          (a->extra == b->extra) &&
          Sched_GroupPathEqual(&a->path, &b->path));
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVtimeContextCopy --
 *
 *	Sets vtime context "to" to identical copy of "from".
 *
 * Results:
 *	Sets vtime context "to" to identical copy of "from".
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
CpuSchedVtimeContextCopy(CpuSched_VtimeContext *to,
                         const CpuSched_VtimeContext *from)
{
   // OPT: copy only relevant portion of group path
   memcpy(to, from, sizeof(CpuSched_VtimeContext));
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedChoiceCopy --
 *
 *	Sets choice "to" to identical copy of "from".
 *
 * Results:
 *	Sets choice "to" to identical copy of "from".
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
CpuSchedChoiceCopy(CpuSchedChoice *to, const CpuSchedChoice *from)
{
   // OPT: avoid copying irrelevant fields
   memcpy(to, from, sizeof(CpuSchedChoice));
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVtimeContextSetExtraZero --
 *
 *      Sets extra component of "vtime" to minimum virtual time
 *	in root group.
 *
 * Results:
 *      Sets extra component of "vtime" to minimum virtual time
 *	in root group.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
CpuSchedVtimeContextSetExtraZero(CpuSched_VtimeContext *vtime)
{
   Sched_GroupPathSetRoot(&vtime->path);
   vtime->extra = 0;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVtimeContextSetZero --
 *
 *      Sets "vtime" to minimum virtual time.
 *
 * Results:
 *      Sets "vtime" to minimum virtual time.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
CpuSchedVtimeContextSetZero(CpuSched_VtimeContext *vtime)
{
   CpuSchedVtimeContextSetExtraZero(vtime);
   vtime->main = 0;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVtimeContextSetExtraInfinite --
 *
 *      Sets extra component of "vtime" to maximum virtual time
 *	in root group.
 *
 * Results:
 *      Sets extra component of "vtime" to maximum virtual time
 *	in root group.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
CpuSchedVtimeContextSetExtraInfinite(CpuSched_VtimeContext *vtime)
{
   Sched_GroupPathSetRoot(&vtime->path);
   vtime->extra = CPUSCHED_VTIME_MAX;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVtimeContextSetInfinite --
 *
 *      Sets "vtime" to maximum virtual time.
 *
 * Results:
 *      Sets "vtime" to maximum virtual time.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
CpuSchedVtimeContextSetInfinite(CpuSched_VtimeContext *vtime)
{
   CpuSchedVtimeContextSetExtraInfinite(vtime);
   vtime->main = CPUSCHED_VTIME_MAX;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedGroupExtraEligible --
 *
 *      Returns TRUE iff all enclosing groups associated with "vsmp"
 *	are currently eligible to compete for extra time beyond their
 *	base allocations.
 *
 * Results:
 *      Returns TRUE iff all enclosing groups associated with "vsmp"
 *	are currently eligible to compete for extra time beyond their
 *	base allocations.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
CpuSchedGroupExtraEligible(const CpuSched_Vsmp *vsmp)
{
   const Sched_GroupPath *path = &vsmp->vtime.path;
   CpuSchedVtime vtNow = vsmp->cell->vtime;
   uint32 i;

   // OPT: could start with i=1 if prohibit root max
   // ineligible if any enclosing group would exceed max
   for (i = 0; i < SCHED_GROUP_PATH_LEN; i++) {
      if (path->level[i] == SCHED_GROUP_ID_INVALID) {
         // no enclosing group would exceed max
         return(TRUE);
      }
      if (CpuSchedGroupVtimeLimit(path->level[i]) >= vtNow) {
         // enclosing group would exceed max
         return(FALSE);
      }
   }

   NOT_REACHED();
   return(FALSE);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedExtraEligible --
 *
 *      Returns TRUE iff "vsmp" is currently eligible to compete for
 *	extra time beyond its base allocation.  A vsmp is ineligible
 *	if it has a constrained maximum execution rate, and running
 *	it would violate this constraint.
 *
 * Results:
 *      Returns TRUE iff "vsmp" is eligible to compete for extra time.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
CpuSchedExtraEligible(const CpuSched_Vsmp *vsmp)
{
   // OPT: could avoid enforce-max check by setting vtimeLimit to zero
   // ineligible if would violate vsmp max constraint
   if (CpuSchedEnforceMax(&vsmp->alloc) &&
       (vsmp->vtimeLimit >= vsmp->cell->vtime)) {
      return(FALSE);
   }

   // ineligible if would violate some group max constraint
   if (vsmp->groupEnforceMax && !CpuSchedGroupExtraEligible(vsmp)) {
      return(FALSE);
   }

   // eligible
   return(TRUE);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVtimeContextBetterChoice --
 *
 *      Returns TRUE iff virtual time context "a" is better than
 *	the virtual time context associated with "choice", using
 *	the specified virtual time component (extra component if
 *	"extra" set, otherwise main component).  "aBonus" is converted
 *      from timer cycles to virtual time and subtracted from a's
 *      vtime. Note that any virtual time context is better than an empty
 *      choice.
 *
 * Results:
 *      Returns TRUE iff "a" is better than "choice" given "extra".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
CpuSchedVtimeContextBetterChoice(const CpuSched_VtimeContext *a,
                                 Timer_RelCycles aBonus,
                                 Bool extra,
                                 const CpuSchedChoice *choice)
{
   if (choice->vtime == NULL) {
      // better than nothing
      return(TRUE);
   } else {
      // better if smaller adjusted virtual time
      return(CpuSchedVtimeContextCompare(a, aBonus, choice->vtime, choice->vtBonus, extra) < 0);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * CpuSchedChoiceInit -- 
 *
 *      Initializes "choice" to an empty choice (no vcpu or vtime).
 *
 * Results:
 *      Initializes "choice" to an empty choice (no vcpu or vtime).
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
CpuSchedChoiceInit(CpuSchedChoice *choice)
{
   choice->min = NULL;
   choice->vtime = NULL;
   choice->wholePackage = FALSE;
   choice->currentRunnerDest = INVALID_PCPU;
   choice->isDirectedYield = FALSE;
   choice->vtBonus = 0;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedChoiceUpdate -- 
 *
 *      Updates "choice" to the specified "vcpu" and its associated
 *	virtual time context.
 *
 * Results:
 *      Updates "choice" to the specified "vcpu" and its associated
 *	virtual time context.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
CpuSchedChoiceUpdate(CpuSchedChoice *choice, CpuSched_Vcpu *vcpu, CpuMask vcpusNeedCosched)
{
   // OPT: consider keping track of second-best, move-to-front heuristic
   choice->min = vcpu;
   choice->vcpusNeedCosched = vcpusNeedCosched;
   choice->vtime = &vcpu->vsmp->vtime;
   choice->wholePackage = FALSE;
   choice->vtBonus = 0;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedChoiceUseWholePackage -- 
 *
 *      Updates "choice" to the specify whether or not the choice
 *	is constrained to using a separate hyperthreading package
 *	for each vcpu.
 *
 * Results:
 *      Updates "choice" to the specify whether or not the choice
 *	is constrained to using a separate hyperthreading package
 *	for each vcpu.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
CpuSchedChoiceUseWholePackage(CpuSchedChoice *choice, Bool wholePackage)
{
   choice->wholePackage = wholePackage;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedChoiceUpdateExtended -- 
 *
 *      Updates "choice" to the specified "vcpu", with virtual time 
 *	context "vtime".
 *
 * Results:
 *      Updates "choice" to the specified "vcpu", with virtual time 
 *	context "vtime".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
CpuSchedChoiceUpdateExtended(CpuSchedChoice *choice,
                             CpuSched_Vcpu *vcpu,
                             CpuMask vcpusNeedCosched,
                             const CpuSched_VtimeContext *vtime,
                             Timer_RelCycles bonus)
{
   // update choice
   choice->min = vcpu;
   choice->wholePackage = FALSE;
   choice->vcpusNeedCosched = vcpusNeedCosched;
   // update vtime, handle explicit override
   if (LIKELY(vtime == NULL)) {
      choice->vtime = &vcpu->vsmp->vtime;
   } else {
      CpuSchedVtimeContextCopy(&choice->vtimeData, vtime);
      choice->vtime = &choice->vtimeData;
   }
   // OPT: could precompute bonus-adjusted vtime
   choice->vtBonus = bonus;
}

/*
 * CpuSchedQueue Operations
 */

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedQueueAddInt --
 *
 *      Adds "vcpu" to "q".
 *
 * Results:
 *      Updates "q", "vcpu".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
CpuSchedQueueAddInt(CpuSchedQueue *q, CpuSched_Vcpu *vcpu)
{
   World_Handle *world = World_VcpuToWorld(vcpu);
   List_Insert(&world->sched.links, LIST_ATFRONT(&q->queue));
   if (q->limbo) {
      vcpu->limbo = TRUE;
      CpuSchedVcpuStateStart(vcpu, &vcpu->limboMeter);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedQueueSelect --
 *
 *      Returns the queue associated with "pcpu" where "vcpu"
 *	should be added.
 *
 * Results:
 *      Returns the queue associated with "pcpu" where "vcpu"
 *	should be added.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE CpuSchedQueue *
CpuSchedQueueSelect(CpuSched_Pcpu *pcpu, const CpuSched_Vcpu *vcpu)
{
   if (CpuSchedVtimeAhead(vcpu->vsmp) > 0) {
      if (CpuSchedExtraEligible(vcpu->vsmp)) {
         return(&pcpu->queueExtra);
      } else {
         return(&pcpu->queueLimbo);
      }
   } else {
      return(&pcpu->queueMain);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedQueueAdd --
 *
 *      Adds "vcpu" to appropriate run queue, based on the amount
 *	by which its virtual time is ahead or behind of its
 *	entitled allocation.
 *
 * Results:
 *      Updates "vcpu", scheduler state.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
CpuSchedQueueAdd(CpuSched_Vcpu *vcpu)
{
   // Note: we may not want to requeue vcpu onto "vcpu->pcpu"
   //       if its affinity mask no longer includes "vcpu->pcpu".
   CpuSched_Pcpu *pcpu = CpuSchedPcpu(vcpu->pcpu);
   CpuSchedQueue *q;

   // sanity check: don't queue idle worlds
   ASSERT(!vcpu->idle);

   // add vcpu to proper queue on pcpu
   q = CpuSchedQueueSelect(pcpu, vcpu);
   CpuSchedQueueAddInt(q, vcpu);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedQueueRemove --
 *
 *	Requires that "vcpu" is enqueued on a scheduler run queue.
 *      Removes "vcpu" from the run queue on which it is enqueued.
 *
 * Results:
 *      Updates "vcpu", scheduler state.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
CpuSchedQueueRemove(CpuSched_Vcpu *vcpu)
{
   List_Remove(&World_VcpuToWorld(vcpu)->sched.links);
   if (vcpu->limbo) {
      CpuSchedVcpuStateDone(vcpu, &vcpu->limboMeter);
      vcpu->limbo = FALSE;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuSetRunState --
 *
 *	Sets current scheduling run state for "vcpu" to "state".
 *
 * Results:
 *	Modifies "vcpu".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedVcpuSetRunState(CpuSched_Vcpu *vcpu, CpuSched_RunState state)
{
   CpuSched_Vsmp *vsmp = vcpu->vsmp;
   World_Handle *world = World_VcpuToWorld(vcpu);
   PCPU p;

   // sanity checks
   ASSERT(CpuSchedVsmpCellIsLocked(vsmp));
   ASSERT(state < CPUSCHED_NUM_RUN_STATES);   

   // track current vcpu states
   if (vcpu->runState == CPUSCHED_RUN) {
      vsmp->nRun--;
   }

   // hook transition from busy waiting
   if ((vcpu->runState == CPUSCHED_BUSY_WAIT) && World_IsVMMWorld(world)) {
      // restart vmm action notifications, if requested
      if ((vcpu->actionWakeupMask != 0) && (state == CPUSCHED_WAIT)) {
         CpuSchedVcpuActionNotifyRequest(vcpu, TRUE);
      }
   }

   if (CpuSchedVcpuIsWaiting(vcpu)) {
      vsmp->nWait--;
      CpuSchedVcpuWaitDone(vcpu, &vcpu->runStateMeter[vcpu->runState]);
      
      // transition from WAIT->READY, track latency
      if (state == CPUSCHED_READY) {
         CpuSchedVcpuStateStart(vcpu, &vcpu->wakeupLatencyMeter);
      }
   } else if (vcpu->runState == CPUSCHED_RUN) {
      Timer_Cycles runCycles;
      ASSERT(vcpu->pcpu < numPCPUs);

      runCycles = CpuSchedVcpuStateDone(vcpu, &vcpu->runStateMeter[CPUSCHED_RUN]);
      vcpu->pcpuRunTime[vcpu->pcpu] += runCycles;

      if (CPUSCHED_STATE_HISTOGRAMS) {
         if (state == CPUSCHED_READY) {
            // RUN -> READY transition must be preemption
            Histogram_Insert(vcpu->preemptTimeHisto, runCycles);
         } else if (state == CPUSCHED_WAIT || state == CPUSCHED_BUSY_WAIT) {
            Histogram_Insert(vcpu->runWaitTimeHisto, runCycles);
         }
      }
   } else if (vcpu->runState != CPUSCHED_NEW) {
      (void) CpuSchedVcpuStateDone(vcpu, &vcpu->runStateMeter[vcpu->runState]);
   }

   // state transition
   vcpu->runState = state;

   // track current vcpu states
   if (vcpu->runState == CPUSCHED_RUN) {
      vsmp->nRun++;

      // transition from READY->RUN, track latency
      if (vcpu->wakeupLatencyMeter.start > 0) {
         CpuSchedVcpuStateDone(vcpu, &vcpu->wakeupLatencyMeter);
      } 
   }

   // hook transition to busy waiting
   if ((vcpu->runState == CPUSCHED_BUSY_WAIT) && World_IsVMMWorld(world)) {
      // stop vmm action notifications, if enabled
      if (vcpu->actionWakeupMask != 0) {
         CpuSchedVcpuActionNotifyRequest(vcpu, FALSE);
      }
   }

   if (CpuSchedVcpuIsWaiting(vcpu)) {
      vsmp->nWait++;
      CpuSchedVcpuWaitStart(vcpu, &vcpu->runStateMeter[vcpu->runState]);
   } else {
      CpuSchedVcpuStateStart(vcpu, &vcpu->runStateMeter[vcpu->runState]);
   }

   // idle halt accounting
   if (SMP_HTEnabled() && CpuSchedVcpuIsIdle(vcpu)) {
      vcpu->localHaltStart = CpuSchedPcpu(vcpu->pcpu)->totalHaltCycles;
   } else {
      vcpu->localHaltStart = -1;
   }

   // sanity checks
   ASSERT(vsmp->nRun >= 0);
   ASSERT(vsmp->nRun <= vsmp->vcpus.len);
   ASSERT(vsmp->nWait >= 0);
   ASSERT(vsmp->nWait <= vsmp->vcpus.len);
   if (CPUSCHED_DEBUG_AGGSTATES || CPUSCHED_DEBUG) {
      CpuSchedVsmpAggregateStateCheck(vsmp);
   }

   // trace this state transition
   if (state == CPUSCHED_RUN) {
      p = MY_PCPU;
   } else {
      p = vcpu->pcpu;
   }

   Trace_Event(TRACE_SCHED_STATE_NEW + state, VcpuWorldID(vcpu), p, 0, 0);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuSetWaitState --
 *
 *	Sets current scheduling wait state and wait event for "vcpu"
 *	to "state" and "event", respectively.
 *
 * Results:
 *	Modifies "vcpu".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
CpuSchedVcpuSetWaitState(CpuSched_Vcpu *vcpu,
                         CpuSched_WaitState state,
                         uint32 event)
{
   CpuSched_Vsmp *vsmp = vcpu->vsmp;

   // sanity checks
   ASSERT(CpuSchedVsmpCellIsLocked(vsmp));
   ASSERT(state < CPUSCHED_NUM_WAIT_STATES);

   // track number of idle vcpus
   if (vcpu->waitState == CPUSCHED_WAIT_IDLE) {
      vsmp->nIdle--;
   }

   // track co-descheduling status
   if (CpuSchedIsMP(vsmp) &&
       CpuSchedWaitStateDisablesCoDesched(vcpu->waitState)) {
      vsmp->disableCoDeschedule--;
      ASSERT(vsmp->disableCoDeschedule >= 0);      
   }

   // wait state accounting
   if (vcpu->waitState != CPUSCHED_WAIT_NONE) {
      CpuSchedVcpuStateDone(vcpu, &vcpu->waitStateMeter[vcpu->waitState]);
   }

   // state transition
   vcpu->waitState = state;
   vcpu->waitEvent = event;

   // wait state accounting
   CpuSchedVcpuStateStart(vcpu, &vcpu->waitStateMeter[vcpu->waitState]);

   // track co-descheduling status
   if (CpuSchedIsMP(vsmp) &&
       CpuSchedWaitStateDisablesCoDesched(vcpu->waitState)) {
      ASSERT(vsmp->disableCoDeschedule >= 0);
      vsmp->disableCoDeschedule++;
   }   

   // track number of idle vcpus
   if (vcpu->waitState == CPUSCHED_WAIT_IDLE) {
      vsmp->nIdle++;
   }

   // sanity checks
   ASSERT(vsmp->nIdle >= 0);
   ASSERT(vsmp->nIdle <= vsmp->vcpus.len);
   if (CPUSCHED_DEBUG_AGGSTATES || CPUSCHED_DEBUG) {
      CpuSchedVsmpAggregateStateCheck(vsmp);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuRequeue --
 *
 *	Requires that "vcpu" is enqueued on a scheduler run queue.
 *      Removes "vcpu" from the run queue on which it is enqueued,
 *	and enqueues it on the appropriate run queue, based on the
 *	number of vcpus in its associated vsmp, and the amount by
 *	which its virtual time is ahead or behind of its entitled
 *	allocation.
 *
 * Results:
 *      Updates "vcpu", scheduler state.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
CpuSchedVcpuRequeue(CpuSched_Vcpu *vcpu)
{
   VcpuLogEvent(vcpu, "requeue");
   CpuSchedQueueRemove(vcpu);
   CpuSchedQueueAdd(vcpu);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuMakeReady --
 *
 *      Transitions "vcpu" to READY run state.  Adds "vcpu" to the
 *	appropriate run queue. May request a reschedule, depending
 *      on the value of the vcpuReschedOpt config option.
 *
 * Results:
 *      Modifies "vcpu".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedVcpuMakeReady(CpuSched_Vcpu *vcpu)
{
   CpuVcpuReschedOpt rsOpt = MY_CELL->config.vcpuReschedOpt;
   CpuSched_Pcpu *pcpu = CpuSchedPcpu(vcpu->pcpu);

   ASSERT(vcpu->runState != CPUSCHED_ZOMBIE);
   CpuSchedVcpuSetRunState(vcpu, CPUSCHED_READY);

   // idle vcpus don't go onto the queues
   if (vcpu->idle) {
      return;
   }

   // insert into ready queue
   CpuSchedQueueAdd(vcpu);

   if (rsOpt == CPU_RESCHED_NONE || rsOpt == CPU_RESCHED_DEFER) {
      pcpu->deferredResched = TRUE;
      if (SMP_HTEnabled() && vcpu->pcpu != MY_PCPU && prdas[vcpu->pcpu]->halted) {
         // halted pcpus won't look at this flag until they're interrupted
         CpuSchedSendReschedIPI(vcpu->pcpu);
      }
   } else if (rsOpt == CPU_RESCHED_ALWAYS) {
      // send an IPI to kick the scheduler immediately
      CpuSchedMarkRescheduleInt(vcpu->pcpu, TRUE);
   } else if (rsOpt == CPU_RESCHED_PREEMPTIBLE) {
      // only send IPI if it looks like we can preempt the remote pcpu
      CpuSchedPcpuPreemptionUpdate(pcpu);
      CpuSchedMarkRescheduleInt(vcpu->pcpu,
                                CpuSchedPcpuCanPreempt(pcpu, vcpu->vsmp));
   } else {
      NOT_REACHED();
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuMakeReadyNoResched --
 *
 *      Transitions "vcpu" to READY run state.  Adds "vcpu" to the
 *	appropriate run queue, unless vcpu is a dedicated idle vcpu.
 *	Unlike CpuSchedVcpuMakeReady(), does not request reschedule.
 *
 * Results:
 *      Modifies "vcpu".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
CpuSchedVcpuMakeReadyNoResched(CpuSched_Vcpu *vcpu)
{
   ASSERT(vcpu->runState != CPUSCHED_ZOMBIE);
   CpuSchedVcpuSetRunState(vcpu, CPUSCHED_READY);

   if (!vcpu->idle) {
      CpuSchedQueueAdd(vcpu);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedHTSharing --
 *
 *      Returns the effective HT sharing constraints of vsmp, taking into
 *      account possible quarantining.
 *
 * Results:
 *      Returns HT sharing value for this vsmp
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE Sched_HTSharing
CpuSchedHTSharing(const CpuSched_Vsmp *vsmp)
{
   Sched_HTSharing share = vsmp->htSharing;

   if (UNLIKELY(vsmp->htQuarantine)) {
      // some vcpu in this vsmp does not play well with others
      share = CPUSCHED_HT_SHARE_NONE;
   }

   share = MIN(share, vsmp->maxHTConstraint);   
   // internal sharing only supported for 2-ways
   if (vsmp->vcpus.len != 2 && share == CPUSCHED_HT_SHARE_INTERNALLY) {
      LOG(0, "internal sharing, vcpus.len=%u, true sharing=%u",
          vsmp->vcpus.len,
          vsmp->htSharing);
   }
   ASSERT(vsmp->vcpus.len == 2 || share != CPUSCHED_HT_SHARE_INTERNALLY);
   
   return (share);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedIdleVtime --
 *
 *     Sets "vtime" to adjusted virtual time context for idle world on "pcpu".
 *     Sets *bonus to the bonus (in cycles) that should be applied to this
 *     idle world's vtime.
 *
 * Results:
 *     Sets bonus, vtime.
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static void
CpuSchedIdleVtimeInt(const CpuSched_Pcpu *pcpu,
                     CpuSched_Vsmp *partnerVsmp,
                     CpuSched_VtimeContext *vtime,
                     Timer_RelCycles *bonus)
{
   // sanity check
   ASSERT(CpuSchedCellIsLocked(pcpu->cell));

   *bonus = 0;
   
   // vtime zero if handoff pending
   if (pcpu->handoff != NULL) {
      CpuSchedVtimeContextSetZero(vtime);
      return;
   }

   // vtime based on vsmp running on partner, if any
   if (SMP_HTEnabled() &&
       vmkernelLoaded  &&
       !CpuSchedPartnerIsIdle(pcpu->id)) {
      Sched_HTSharing sharing;

      ASSERT(partnerVsmp != NULL);
      
      // start with partner vtime
      CpuSchedVtimeContextCopy(vtime, &partnerVsmp->vtime);

      // adjust based on sharing
      sharing = CpuSchedHTSharing(partnerVsmp);
      if (sharing == CPUSCHED_HT_SHARE_ANY) {
         // make the idle thread easier to preempt than partner vsmp
         *bonus = -pcpu->cell->config.idleVtimeMsPenaltyCycles;
      } else if (sharing == CPUSCHED_HT_SHARE_INTERNALLY) {
         // slightly increase vtime so that partner vsmp's vcpus can
         // preempt this idle thread
         *bonus = -1LL;
      } else {
         ASSERT(sharing == CPUSCHED_HT_SHARE_NONE || sharing == CPUSCHED_HT_SHARE_ANY);
         // apply the regular preemption penalty, because preempting this idle thread will
         // cause the partner vcpu to be brought down as well
         *bonus = pcpu->cell->config.preemptionBonusCycles;
      }
      
      if (CpuSchedVtimeAhead(partnerVsmp) <= 0) {
         CpuSchedVtimeContextSetExtraZero(vtime);
      }
   } else {
      // vtime infinite if processor (and partner, if any) really idle
      CpuSchedVtimeContextSetInfinite(vtime);
   }

   // adjust idle time based on interrupt load
   if (CONFIG_OPTION(IRQ_ROUTING_POLICY) == IT_IDLE_ROUTING) {
      CpuSchedVtime intrAdj = 0;
      Timer_Cycles adjCycles;
      IT_IntrRate rate, partnerRate = INTR_RATE_NONE;

      rate = IT_GetPcpuIntrRate(pcpu->id);
      if (SMP_HTEnabled()) {
         partnerRate = IT_GetPcpuIntrRate(pcpu->partner->id);
      }

      // Penalize by one unit for every level of interrupts,
      // or one-half unit for every level of partner's interrupts.
      // When interrupts hit our partner lcpu, they'll take away
      // execution resources from the running vcpu on the current lcpu,
      // however, they won't induce any context switching costs, so
      // we shouldn't consider them as significant as IRQs on the local lcpu
      adjCycles = pcpu->cell->config.intrLevelPenaltyCycles
                  * (partnerRate + (2*rate));
      intrAdj = CpuSchedTCToVtime(cpuSchedConst.nStride, adjCycles);
      *bonus += intrAdj;

      if (vtime->main <
          pcpu->cell->vtime + pcpu->cell->config.vtAheadThreshold) {
         CpuSchedVtimeContextSetExtraZero(vtime);
      } else {
         *bonus += intrAdj;
      }
   }

   // ensure non-negative vtimes
   vtime->main  = MAX(vtime->main, 0);
   vtime->extra = MAX(vtime->extra, 0);
}

// wrapper for above, uses default partnerVsmp
static INLINE void
CpuSchedIdleVtime(const CpuSched_Pcpu *pcpu,
                  CpuSched_VtimeContext *context, // out
                  Timer_RelCycles *bonus)         // out
{
   CpuSched_Vsmp *partnerVsmp;
   if (!SMP_HTEnabled()) {
      partnerVsmp = NULL;
   } else if (pcpu->partner->handoff != NULL) {
      partnerVsmp = pcpu->partner->handoff->vsmp;
   } else {
      partnerVsmp = CpuSchedRunningVcpu(pcpu->partner->id)->vsmp;
   }
   CpuSchedIdleVtimeInt(pcpu, partnerVsmp, context, bonus);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedGetPartnerHaltedDelta --
 *
 *     Returns the number of cycles for which the partner pcpu of vcpu->pcpu
 *     has been halted since vcpu started running
 *
 * Results:
 *     Returns the number of cycles for which the partner pcpu of vcpu->pcpu
 *     has been halted since vcpu started running
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE Timer_Cycles
CpuSchedGetPartnerHaltedDelta(CpuSched_Vcpu *vcpu)
{
   Timer_Cycles partnerHaltedCycles, haltDelta;
   CpuSched_Pcpu *partnerPcpu;

   if (!SMP_HTEnabled()) {
      return (0);
   }

   ASSERT(CpuSchedPackageHaltIsLocked(vcpu->pcpu));

   partnerPcpu = CpuSchedPartnerPcpu(vcpu->pcpu);
   partnerHaltedCycles = partnerPcpu->totalHaltCycles;

   if (partnerHaltedCycles == 0 
       || vcpu->phaltStart == 0) {
      return (0);
   }

   haltDelta = (partnerHaltedCycles - vcpu->phaltStart);
      
   if (UNLIKELY(haltDelta > cpuSchedConst.cyclesPerSecond ||
                vcpu->phaltStart > partnerHaltedCycles)) {
      uint32 hdeltaUsec;
      uint64 hdeltaSec;
      Timer_TCToSec(haltDelta, &hdeltaSec, &hdeltaUsec);
      
      VCPULOG(1, vcpu, 
              "invalid partner halt time: delta=%Lu, deltaSec=%Lu.%06u", 
              haltDelta,
              hdeltaSec,
              hdeltaUsec);
      return (0);
   } else {
      return (haltDelta);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedPcpuPreemptionInvalidate --
 *
 *	Invalidates preemption virtual times associated with "pcpu".
 *
 * Results:
 *      Modifies "pcpu".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
CpuSchedPcpuPreemptionInvalidate(CpuSched_Pcpu *pcpu)
{
   pcpu->preemption.valid = FALSE;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedCellPreemptionInvalidate --
 *
 *	Invalidates preemption virtual times associated with
 *	all pcpus in "cell".
 *
 * Results:
 *      Modifies "cell".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedCellPreemptionInvalidate(CpuSched_Cell *cell)
{
   FORALL_CELL_PCPUS(cell, p) {
      CpuSchedPcpuPreemptionInvalidate(CpuSchedPcpu(p));
   } CELL_PCPUS_DONE;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedPcpuPreemptionUpdate --
 *
 *	Updates preemption virtual times associated with "schedPcpu".
 *	The processor specified by "schedPcpu" can be remotely
 *	preempted only by a vsmp with a smaller virtual time.
 *
 * Results:
 *      Modifies "schedPcpu".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedPcpuPreemptionUpdate(CpuSched_Pcpu *schedPcpu)
{
   CpuSchedPcpuPreemption *preempt = &schedPcpu->preemption;
   CpuSched_VtimeContext *vtPreempt;
   CpuSched_Vcpu *vcpu;
   CpuSched_Vsmp *vsmp;
   Bool isAhead;

   // sanity checks
   ASSERT(CpuSchedCellIsLocked(schedPcpu->cell));

   // avoid unnecessary work
   if (preempt->valid) {
      return;
   }
   
   vtPreempt = &preempt->vtime;
   vcpu = CpuSchedRunningVcpu(schedPcpu->id);
   vsmp = vcpu->vsmp;
   ASSERT(CpuSchedVsmpCellIsLocked(vsmp));
   
   // disable preemption if handoff or still loading
   if (UNLIKELY(!vmkernelLoaded) || schedPcpu->handoff) {
      CpuSchedVtimeContextSetZero(vtPreempt);
      preempt->vtBonus = 0;
      preempt->valid = TRUE;
      return;
   }

   // special vtime for idle world
   if (CpuSchedVcpuIsIdle(vcpu)) {
      CpuSchedIdleVtime(schedPcpu, vtPreempt, &preempt->vtBonus);
      preempt->valid = TRUE;
      return;
   }

   // preemptibility based on running vsmp
   CpuSchedVtimeContextCopy(vtPreempt, &vsmp->vtime);
   
   // prevent extra from preempting main
   isAhead = (CpuSchedVtimeAhead(vsmp) > 0);
   if (!isAhead) {
      CpuSchedVtimeContextSetExtraZero(vtPreempt);
   }
   
   // add a configurable bonus to make it harder to preempt a running world
   preempt->vtBonus = schedPcpu->cell->config.preemptionBonusCycles;
   
   // preeemption state up-to-date
   preempt->valid = TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedPackagePreemptionUpdate --
 *
 *      Determines and stores preemptiblity info for PCPU p and its partner
 *      (if hyperthreaded). Safe to call on both HT and non HT systems.
 *      preemption.valid flag will be TRUE for both LCPUS after this returns.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Updates stored preemptibility info, valid flags.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE void
CpuSchedPackagePreemptionUpdate(PCPU p)
{
   CpuSched_Pcpu *pcpu = CpuSchedPcpu(p);
   
   CpuSchedPcpuPreemptionUpdate(pcpu);
   if (SMP_HTEnabled()) {
      CpuSchedPcpuPreemptionUpdate(CpuSchedPartnerPcpu(p));
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedPcpuCanPreemptMain --
 *
 *	Returns TRUE iff "vsmp" can preempt the currently-executing
 *	world on "pcpu".  Requires that "vsmp" is not ahead of
 *	schedule.
 *
 * Results:
 *      Returns TRUE iff "vsmp" can preempt "pcpu".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
CpuSchedPcpuCanPreemptMain(const CpuSched_Pcpu *pcpu,
                           const CpuSched_Vsmp *vsmp)
{
   ASSERT(pcpu->preemption.valid);
   return(CpuSchedVtimeContextMainCompare(&vsmp->vtime,
                                          0,
                                          &pcpu->preemption.vtime,
                                          pcpu->preemption.vtBonus) < 0);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedPcpuCanPreemptExtra --
 *
 *	Returns TRUE iff "vsmp" can preempt the currently-executing
 *	world on "pcpu".  Requires that "vsmp" is ahead of
 *	schedule.
 *
 * Results:
 *      Returns TRUE iff "vsmp" can preempt "pcpu".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
CpuSchedPcpuCanPreemptExtra(const CpuSched_Pcpu *pcpu,
                            const CpuSched_Vsmp *vsmp)
{
   ASSERT(pcpu->preemption.valid);
   return(CpuSchedVtimeContextExtraCompare(&vsmp->vtime,
                                           0,
                                           &pcpu->preemption.vtime,
                                           pcpu->preemption.vtBonus) < 0);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedPcpuCanPreempt --
 *
 *	Returns TRUE iff "vsmp" can preempt the currently-executing
 *	world on "pcpu".
 *
 * Results:
 *      Returns TRUE iff "vsmp" can preempt "pcpu".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static Bool
CpuSchedPcpuCanPreempt(const CpuSched_Pcpu *pcpu,
                       const CpuSched_Vsmp *vsmp)
{
   if (CpuSchedVtimeAhead(vsmp) > 0) {
      return(CpuSchedPcpuCanPreemptExtra(pcpu, vsmp));
   } else {
      return(CpuSchedPcpuCanPreemptMain(pcpu, vsmp));
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedCanPreempt --
 *
 *     Returns TRUE if "vsmp" can preempt the currently-executing world
 *     on schedPcpu and wholePackage is FALSE, or if "vsmp" can preempt
 *     the currently-executing worlds on both hypertwins of schedPcpu's package
 *     and wholePackage is TRUE.
 *
 * Results:
 *     Returns TRUE if vsmp can preempt schedPcpu or schedPcpu and its partner
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE Bool
CpuSchedCanPreempt(const CpuSched_Pcpu *schedPcpu,
                   const CpuSched_Vsmp *vsmp,
                   Bool wholePackage)
{
   if (!wholePackage || !SMP_HTEnabled()) {
      // standard case: not hyperthreaded or just trying
      // to preempt a single LCPU
      return CpuSchedPcpuCanPreempt(schedPcpu, vsmp);
   } else {
      // hyperthreaded, whole-package preempt case
      return (CpuSchedPcpuCanPreempt(schedPcpu, vsmp) &&
              CpuSchedPcpuCanPreempt(schedPcpu->partner, vsmp));
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * CpuSched_WorldHasHardAffinity --
 *
 *     Determine whether this world has "hard" (user-set) affinity constraints
 *
 * Results:
 *     Returns TRUE iff world has hard affinity
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
Bool
CpuSched_WorldHasHardAffinity(const World_Handle *world)
{
   return (World_CpuSchedVsmp(world)->hardAffinity);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedVcpuSetAffinityMask --
 *
 *     Actually sets the hard or soft affinity field in the vcpu structure.
 *
 * Results:
 *     None
 *
 * Side effects:
 *     Modifies vcpu
 *
 *-----------------------------------------------------------------------------
 */
static INLINE void
CpuSchedVcpuSetAffinityMask(CpuSched_Vcpu *vcpu, 
                            CpuMask affinMask,
                            Bool hard)
{
   ASSERT(hard || !vcpu->vsmp->hardAffinity);
   vcpu->affinityMask = affinMask;

   if (hard && (affinMask & cpuSchedConst.defaultAffinity) != cpuSchedConst.defaultAffinity) {
      vcpu->vsmp->hardAffinity = TRUE;
      VCPULOG(1, vcpu, "set hard affinity");
   } else {
      VCPULOG(1, vcpu, "set soft afffinity");
      vcpu->vsmp->hardAffinity = FALSE;
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedVcpuAffinityPermitsPcpu --
 *
 *	Determine whether "vcpu" is allowed to run on "p", according to
 *	affinity, with the pcpus in the "forbiddenPcpus" mask considered
 *	to be disallowed.
 *
 * Results:
 *	Returns TRUE if "vcpu"'s affinity and forbiddenPcpus allows it
 *	to run on "p".
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE Bool
CpuSchedVcpuAffinityPermitsPcpu(const CpuSched_Vcpu *vcpu,
                                PCPU p,
                                CpuMask forbiddenPcpus)
{   
   return ((vcpu->affinityMask & CPUSCHED_AFFINITY(p) & (~forbiddenPcpus)) != 0);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedVcpuHardAffinity --
 *
 *     Returns a CpuMask representing the "hard" (user-set and user-visible)
 *     affinity of the vcpu
 *
 * Results:
 *     Returns the hard affinity CpuMask
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE CpuMask
CpuSchedVcpuHardAffinity(const CpuSched_Vcpu *vcpu)
{
   if (vcpu->vsmp->hardAffinity) {
      return vcpu->affinityMask;
   } else {
      return cpuSchedConst.defaultAffinity;
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedCanCoScheduleMig --
 *
 *	Returns TRUE iff all "needCoSchedCount" vcpus in "vcpuNeedCosched"
 *	can be scheduled concurrently, assuming that "myVcpu" will be
 *	scheduled on "local", and that inter-pcpu migration is allowed.
 *	If "wholePackage" is TRUE, this function will only return TRUE
 *	if each vcpu in "vsmp" can be coscheduled onto its own package.
 *
 * Results:
 *	Returns TRUE if needed vcpus can be coscheduled.
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */
static Bool
CpuSchedCanCoScheduleMig(const CpuSched_Vsmp *vsmp, 
                         PCPU local, 
                         const CpuSched_Vcpu *myVcpu,
                         Bool wholePackage,
                         CpuMask vcpusNeedCosched,
                         uint32 needCoschedCount)
{
   CpuSched_Cell *localCell = CpuSchedPcpu(local)->cell;
   CpuMask pcpuTakenMask = 0;

   ASSERT(CpuSchedIsMP(vsmp));

   pcpuTakenMask |= PcpuMask(local, wholePackage);
   if (vsmp->jointAffinity) {
      // handling the joint affinity case is straightforward
      CpuSched_Vcpu* vcpuExample = vsmp->vcpus.list[0];
      uint32 need = needCoschedCount;
      
      FORALL_CELL_REMOTE_PCPUS(localCell, local, p) {
         if (CpuSchedVcpuAffinityPermitsPcpu(vcpuExample, p, pcpuTakenMask) &&
             CpuSchedCanPreempt(CpuSchedPcpu(p), vsmp, wholePackage)) {
            need--;
            pcpuTakenMask |= PcpuMask(p, wholePackage);
            if (need == 0) {
               return TRUE;
            }
         }
      } CELL_REMOTE_PCPUS_DONE;

      // tried all cpus, couldn't fill our need
      return FALSE;
   } else {
      // disjoint affinity handling... 
      FORALL_VSMP_VCPUS(vsmp, vcpu) {
         if (VcpuMask(vcpu) & vcpusNeedCosched) {
            // find a home for this vcpu
            FORALL_CELL_REMOTE_PCPUS(localCell, local, p) {
               if (CpuSchedVcpuAffinityPermitsPcpu(vcpu, p, pcpuTakenMask) && 
                   CpuSchedCanPreempt(CpuSchedPcpu(p), vsmp, wholePackage)) {
                  pcpuTakenMask |= PcpuMask(p, wholePackage);
                  // continue to the next iteration of FORALL_VSMP_VCPUS loop
                  goto nextVcpu;
               }
            } CELL_REMOTE_PCPUS_DONE;
            // couldn't find a home for this vcpu
            return FALSE;
         }
        nextVcpu:
         continue;
      } VSMP_VCPUS_DONE;
      // successfully placed every cosched-mandatory vcpu
      return TRUE;
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedPcpuCheckMigrationAllowed --
 *
 *	Determine whether inter-pcpu or inter-cell migrations to
 *	"schedPcpu" are currently permitted. 
 *
 * Results:
 *	Sets "allowPcpuMigrate" TRUE if the next reschedule on "schedPcpu"
 *	should allow migration from another pcpu, otherwise FALSE.
 *	Sets "allowCellMigrate" TRUE if the next reschedule on "schedPcpu"
 *	should allow migration from another cell, otherwise FALSE.
 *	Sets "allowRunnerMove" TRUE if the next reschedule on "schedPcpu"
 *	should allow the currently-running vcpu to remotely preempt an idle pcpu.
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE void
CpuSchedPcpuCheckMigrationAllowed(const CpuSched_Pcpu *schedPcpu,
                                  Bool *allowPcpuMigrate,
                                  Bool *allowCellMigrate,
                                  Bool *allowRunnerMove)
{
   const CpuSched_Cell *cell = schedPcpu->cell;

   // allow inter-pcpu migration if sufficient elapsed time
   *allowPcpuMigrate = (schedPcpu->runnerMoveRequested ||
                        (cell->config.migPcpuWaitCycles == 0) ||
                        (cell->now > schedPcpu->nextPcpuMigrateAllowed));
   // if we haven't actually had a migration within this migrate interval,
   // randomly (one out of every migChance times) allow remote scanning
   if (cell->config.migChance != 0 &&
       !*allowPcpuMigrate &&
       !schedPcpu->recentPcpuMig) {
      uint32 rnd = CpuSchedRandom();
      if ((rnd % cell->config.migChance) == 0) {
         *allowPcpuMigrate = TRUE;
      }
   }

   // allow inter-cell migration based if sufficient elapsed time,
   // but only if inter-pcpu migration is also allowed
   *allowCellMigrate = ((*allowPcpuMigrate) &&
                        (cpuSched.nCells > 1) &&
                        ((cell->config.migCellWaitCycles == 0) ||
                         (cell->now > cell->nextCellMigrateAllowed)));

   *allowRunnerMove = ((*allowPcpuMigrate) &&
                        (schedPcpu->runnerMoveRequested ||
                         (cell->config.runnerMoveWaitCycles == 0) ||
                         (cell->now > schedPcpu->nextRunnerMoveAllowed)));
}


/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedRandomJitter --
 *
 *      Returns "cycles" +/- a random value in [0, "maxJitter").
 *	The returned value is bounded from below by zero.
 *	The value of "maxJitter" must be a power of 2.
 *
 * Results:
 *      Returns "cycles" +/- a random value in [0, "maxJitter").
 *	The returned value is bounded from below by zero.
 *
 * Side effects:
 *      Updates global cpusched random seed.
 *
 *-----------------------------------------------------------------------------
 */
static Timer_Cycles
CpuSchedRandomJitter(Timer_Cycles cycles, uint32 maxJitter)
{
   uint32 rnd, jitter, jitterMask;

   // sanity check
   ASSERT(Util_IsPowerOf2(maxJitter));
   if (UNLIKELY(!Util_IsPowerOf2(maxJitter))) {
      // simply return unjittered value
      return(cycles);
   }

   // compute mask
   jitterMask = maxJitter - 1;
   
   // generate random number
   //   use low-order lg(maxJitter) bits (mask=maxJitter-1) as random value
   //   use next higher bit (mask=maxJitter) as random sign bit
   rnd = CpuSchedRandom();
   jitter = rnd & jitterMask;
   ASSERT(jitter < maxJitter);

   if (rnd & maxJitter) {
      // subtract jitter, avoid going negative
      if (LIKELY(cycles > jitter)) {
         return(cycles - jitter);
      } else {
         return(0);
      }
   } else {
      // add jitter
      return(cycles + jitter);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedPcpuUpdateMigrationAllowed --
 *
 *	Updates "schedPcpu" state that determines when inter-pcpu and
 *	inter-cell migrations to "schedPcpu" are permitted. "choice" should
 *      be the current choose result, used to determine what migation types
 *      were allowed and which actually occurred.
 *
 * Results:
 *	Modifies "schedPcpu".
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */
static void
CpuSchedPcpuUpdateMigrationAllowed(CpuSched_Pcpu *schedPcpu,
                                   CpuSchedChoice *choice)
{
   const uint32 maxJitter = cpuSchedConst.smallJitterCycles;
   CpuSched_Cell *cell = schedPcpu->cell;

   // sanity check
   ASSERT(CpuSchedCellIsLocked(cell));

   // update next pcpu migration time if timer expired
   if ((cell->config.migPcpuWaitCycles > 0) &&
       (schedPcpu->nextPcpuMigrateAllowed < cell->now)) {
      schedPcpu->nextPcpuMigrateAllowed = cell->now +
         CpuSchedRandomJitter(cell->config.migPcpuWaitCycles, maxJitter);
      ASSERT(schedPcpu->nextPcpuMigrateAllowed <=
             cell->now + cell->config.migPcpuWaitCycles + maxJitter);
      schedPcpu->recentPcpuMig = FALSE;
   }

   // update flag to indicate if we recently migrated a vcpu here
   if (choice->min != NULL && choice->min->pcpu != schedPcpu->id) {
      schedPcpu->recentPcpuMig = TRUE;
   }
   
   // n.b. don't update if pcpu migration was disallowed, or
   //      if last remote try-lock failed
   if (choice->cellMigrateAllowed &&
       (cell->stats.remoteLockLast)) {
      cell->nextCellMigrateAllowed = cell->now + 
         CpuSchedRandomJitter(cell->config.migCellWaitCycles, maxJitter);
      ASSERT(cell->nextCellMigrateAllowed <=
             cell->now + cell->config.migCellWaitCycles + maxJitter);
   }

   // update next current runner move time
   if (choice->runnerMoveAllowed) {
      schedPcpu->nextRunnerMoveAllowed = cell->now +
         CpuSchedRandomJitter(cell->config.runnerMoveWaitCycles, maxJitter);
      ASSERT(schedPcpu->nextRunnerMoveAllowed <=
             cell->now + cell->config.runnerMoveWaitCycles + maxJitter);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedCanCoScheduleNoMig --
 *
 *     Determine if we can coschedule all vcpus in "vcpusNeedCosched"
 *     with myVcpu on "local" WITHOUT migrating anybody. If "wholePackage" is
 *     TRUE, then coscheduling will only be permitted if the vsmp can claim an
 *     entire physical package for each vcpu.
 *
 * Results:
 *     Returns TRUE if we can coschedule the vsmp
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static Bool
CpuSchedCanCoScheduleNoMig(const CpuSched_Vsmp *vsmp, 
                           PCPU local, 
                           const CpuSched_Vcpu *myVcpu, 
                           Bool wholePackage,
                           CpuMask vcpusNeedCosched)
{
   CpuMask pcpuTakenMask = PcpuMask(local, wholePackage);

   // fail if unexpectedly trying to schedule vsmp from remote cell
   ASSERT(vsmp->cell == CpuSchedPcpu(local)->cell);
   if (vsmp->cell != CpuSchedPcpu(local)->cell) {
      return(FALSE);
   }

   FORALL_VSMP_VCPUS(vsmp, vcpu) {
      CpuSched_Pcpu *curPcpu = CpuSchedPcpu(vcpu->pcpu);

      if (!(VcpuMask(vcpu) & vcpusNeedCosched)) {
         // we know that myVcpu can run and we don't need to
         // worry about vcpus with discretionary coscheduling
         continue;
      }

      // in the nomig case we don't update all the remote preemption times
      // up front, so we may have to update it here
      CpuSchedPackagePreemptionUpdate(vcpu->pcpu);
      
      // make sure the vcpu in question can preempt its local pcpu
      // and that its pcpu is already taken
      if (CpuSchedVcpuAffinityPermitsPcpu(vcpu, vcpu->pcpu, pcpuTakenMask) &&
          CpuSchedCanPreempt(curPcpu, vsmp, wholePackage)) {
         pcpuTakenMask |= PcpuMask(vcpu->pcpu, wholePackage);
      } else {
         return FALSE;
      }
   } VSMP_VCPUS_DONE;
   return TRUE;
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedVcpusNeedCosched --
 *
 *      Returns the number of vcpus in "vsmp" that must be coscheduled,
 *      excluding "myVcpu" from the count.
 *      Bits corresponding to vcpus in vsmp that need coscheduling are set
 *      to 1 in "*vcpuMask"
 * 
 * Results:
 *      See above.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE uint8
CpuSchedVcpusNeedCosched(const CpuSched_Vsmp *vsmp,
                         const CpuSched_Vcpu *myVcpu,
                         CpuMask *vcpuMask) // out
{
   uint8 need = 0;
   FORALL_VSMP_VCPUS(vsmp, vcpu) {
      if (vcpu != myVcpu && CpuSchedVcpuNeedsCosched(vcpu)) {
         need++;
         *vcpuMask |= VcpuMask(vcpu);
      }
   } VSMP_VCPUS_DONE;

   return (need);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedCanCoSchedule --
 *
 *	Returns TRUE iff all cosched-mandatory vcpus in vsmp can be scheduled
 *	concurrently in the scheduler cell associated with "local", assuming
 *	that "myVcpu" will be scheduled on the processor specified by "local",
 *	and considering the global preemption * state. If "wholePackage" is
 *	TRUE, this function will only return TRUE if each vcpu of the vsmp can
 *	be coscheduled on its own package.
 *
 *      "vcpusCoSchedNeeded" will be filled in with a mask that indicates which
 *      
 *
 * Results:
 *      Returns TRUE iff all cosched-mandatory vcpus in vsmp can be co-scheduled.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
CpuSchedCanCoSchedule(const CpuSched_Vsmp *vsmp, 
                      PCPU local, 
                      const CpuSched_Vcpu *myVcpu, 
                      Bool wholePackage,
                      Bool migrateAllowed,
                      CpuMask *vcpusCoSchedNeeded)  // out
{
   uint32 need;

   // initialize
   *vcpusCoSchedNeeded = 0;

   // no coscheduling in vsmp CO_RUN state
   if (vsmp->coRunState == CPUSCHED_CO_RUN) {
      return(TRUE);
   }

   // number of vcpus to co-schedule
   ASSERT(vsmp->vcpus.len > 0);
   need = CpuSchedVcpusNeedCosched(vsmp, myVcpu, vcpusCoSchedNeeded);

   // uniprocessor VM or no cosched-mandatory vcpus
   if (need == 0) {
      return(TRUE);
   }

   // general case, with or without migration
   if (migrateAllowed) {
      return CpuSchedCanCoScheduleMig(vsmp, local, myVcpu,
                                      wholePackage, *vcpusCoSchedNeeded, need);
   } else {
      return CpuSchedCanCoScheduleNoMig(vsmp, local, myVcpu,
                                        wholePackage, *vcpusCoSchedNeeded);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedPcpuCoRun --
 *
 *	Arrange for "vcpu" to be scheduled next on "pcpu", and
 *	initiate reschedule.
 *
 * Results:
 *      Modifies "vcpu" and "pcpu".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedPcpuCoRun(CpuSched_Pcpu *pcpu, CpuSched_Vcpu *vcpu)
{
   // sanity checks
   ASSERT(pcpu->handoff == NULL);
   ASSERT(vcpu->runState == CPUSCHED_READY);

   if (vcpu->runState == CPUSCHED_READY) {
      // transition vcpu to READY_CORUN
      if (!vcpu->idle) {
         CpuSchedQueueRemove(vcpu);
      }
      CpuSchedVcpuSetRunState(vcpu, CPUSCHED_READY_CORUN);
      vcpu->pcpuHandoff = pcpu->id;

      // update pcpu handoff state
      pcpu->handoff = vcpu;

      // immediately update our preemptibility
      CpuSchedPcpuPreemptionInvalidate(pcpu);
      CpuSchedPcpuPreemptionUpdate(pcpu);

      // immediately update preemptibility for our partner
      if (SMP_HTEnabled() && CpuSchedPartnerIsIdle(pcpu->id)) {
         CpuSchedPcpuPreemptionInvalidate(pcpu->partner);
         CpuSchedPcpuPreemptionUpdate(pcpu->partner);
      }

      CpuSched_MarkReschedule(pcpu->id);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedCoRun --
 *
 *     Arrange for "vcpu" to be scheduled next on "schedPcpu", and
 *     initiate reschedule. If wholePkg is TRUE, the idle world will
 *     be scheduled on "schedPcpu"'s hypertwin, so that vcpu can 
 *     consume the execution resources of the whole package.
 *
 *     Also removes "vcpu->schedIndex" from the vcpusToPlace mask and
 *     adds schedPcpu (and possibly its partner) to the mask *pcpusForbidden.
 *
 * Results:
 *     Modifies "vcpu" and "schedPcpu", may also modify "schedPcpu"'s twin
 *     Modifies *vcpusToPlace and *pcpusForbidden.
 *
 * Side effects:
 *     Will set a handoff and corun the vcpu.
 *
 *-----------------------------------------------------------------------------
 */
static void
CpuSchedCoRun(CpuSched_Pcpu *schedPcpu,
              CpuSched_Vcpu *vcpu,
              Bool wholePkg,
              uint32 *vcpusToPlace,
              uint32 *pcpusForbidden)
{
   ASSERT(schedPcpu->handoff == NULL);
   
   *vcpusToPlace &= ~VcpuMask(vcpu);
   *pcpusForbidden |= PcpuMask(schedPcpu->id, wholePkg);
   
   CpuSchedPcpuCoRun(schedPcpu, vcpu);
   if (wholePkg) {
      // handoff to idle world on partner pcpu
      ASSERT(SMP_HTEnabled());
      if (!CpuSchedPartnerIsIdle(schedPcpu->id)
          && schedPcpu->partner->handoff == NULL) {
         CpuSchedPcpuCoRun(schedPcpu->partner, 
                           CpuSchedGetIdleVcpu(schedPcpu->partner->id));
      }
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedCoSchedSubset --
 *
 *      Try to coschedule the vcpus in the mask *vcpusToPlace.
 *      Bits set in *vcpusToPlace correspond to the sched indices
 *      of vcpus in the vsmp "choice->min->vsmp." No vcpus are permitted to be
 *      placed on pcpus in the mask *pcpusForbidden. If "migrateAllowed" is
 *      false, no vcpus will be migrated.
 *
 *      Each successfully-placed vcpu is removed from *vcpusToPlace and
 *      *pcpusForbidden is updated to reflect pcpus that are no longer available.
 *
 * Results:
 *      Modifies vcpusToPlace and pcpusForbidden
 *
 * Side effects:
 *      Modifies scheduler state to coschedule vcpus.
 *
 *-----------------------------------------------------------------------------
 */
static void
CpuSchedCoSchedSubset(const CpuSchedChoice *choice,
                      Bool migrateAllowed,
                      CpuMask *vcpusToPlace, // in/out
                      CpuMask *pcpusForbidden) // in/out
{
   CpuSched_Vcpu *myVcpu = choice->min;
   CpuSched_Vsmp *myVsmp = myVcpu->vsmp;
   CpuSched_Pcpu *myPcpu = CpuSchedPcpu(myVcpu->pcpu);
   Bool ahead = (CpuSchedVtimeAhead(myVsmp) > 0);
   
   if (*vcpusToPlace == 0) {
      // nothing to do
      return;
   }
   
   FORALL_VSMP_VCPUS(myVsmp, vcpu) {
      uint32 vcpuMask = VcpuMask(vcpu);
      // pcpu where last executed
      CpuSched_Pcpu *pcpu = CpuSchedPcpu(vcpu->pcpu);
      
      // only try to cosched vcpus in the "toPlace" list
      if (!(vcpuMask & *vcpusToPlace)) {
         continue;
      }

      // sanity checks: runnable, but not already running
      ASSERT(CpuSchedVcpuIsRunnable(vcpu));
      ASSERT(!CpuSchedVcpuRunOrBwait(vcpu));            

      if (!migrateAllowed) {
         // in the nomig case, we didn't update all preemption data up-front,
         // so we might need to now
         CpuSchedPackagePreemptionUpdate(vcpu->pcpu);

         // consider pcpu where last executed, and its hypertwin, if any
         if (CpuSchedVcpuAffinityPermitsPcpu(vcpu, pcpu->id, *pcpusForbidden) &&
             CpuSchedCanPreempt(pcpu, myVsmp, choice->wholePackage)) {
            // pcpu where last executed
            CpuSchedCoRun(pcpu, vcpu, choice->wholePackage,
                          vcpusToPlace, pcpusForbidden);
         } else if (SMP_HTEnabled() &&
                    !choice->wholePackage &&
                    CpuSchedVcpuAffinityPermitsPcpu(vcpu, pcpu->partner->id, *pcpusForbidden) &&
                    CpuSchedCanPreempt(pcpu->partner, myVsmp, choice->wholePackage)) {
            // hypertwin of pcpu where last executed
            CpuSchedCoRun(pcpu->partner, vcpu, choice->wholePackage,
                          vcpusToPlace, pcpusForbidden);
         }
      } else {
         // migration allowed
         CpuSched_Pcpu *bestPcpu = NULL;
         
         // find most preemptible remote pcpu
         FORALL_CELL_REMOTE_PCPUS(myVsmp->cell, myPcpu->id, p) {
            CpuSched_Pcpu *pcpu = CpuSchedPcpu(p);
            if (CpuSchedVcpuAffinityPermitsPcpu(vcpu, pcpu->id, *pcpusForbidden) &&
                pcpu->handoff == NULL &&
                CpuSchedCanPreempt(pcpu, myVsmp, choice->wholePackage)) {
               if ((bestPcpu == NULL) ||
                   (CpuSchedVtimeContextCompare(&pcpu->preemption.vtime,
                                                pcpu->preemption.vtBonus,
                                                &bestPcpu->preemption.vtime,
                                                bestPcpu->preemption.vtBonus,
                                                ahead) < 0)) {
                  bestPcpu = pcpu;
               }
            }
         } CELL_REMOTE_PCPUS_DONE;

         if (bestPcpu != NULL) {
            CpuSchedCoRun(bestPcpu, vcpu, choice->wholePackage,
                          vcpusToPlace, pcpusForbidden);
            
         }         
      }
   } VSMP_VCPUS_DONE;
}


/*
 *----------------------------------------------------------------------
 *
 * CpuSchedCoSchedule --
 *
 *	Maps all siblings of "choice->min" to remote processors, and
 *	initiates remote reschedules.  One vcpu will be scheduled
 *	on the local processor.  Requires the vsmp associated
 *	with "myVcpu" to be in the CO_RUN state.  Caller must hold
 *	scheduler cell lock for "choice->min".
 *
 * Results:
 *      Modifies scheduler state.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedCoSchedule(const CpuSchedChoice *choice)
{
   CpuSched_Vcpu *myVcpu = choice->min;
   CpuSched_Vsmp *myVsmp = myVcpu->vsmp;
   CpuSched_Pcpu *myPcpu = CpuSchedPcpu(myVcpu->pcpu);
   uint32 mandatoryVcpuMask, optionalVcpuMask=0;
   CpuMask forbidMask;

   // sanity checks
   ASSERT(CpuSchedVsmpCellIsLocked(myVsmp));
   ASSERT(myVsmp->coRunState == CPUSCHED_CO_RUN);

   // special case: internal-sharing
   // must share with partner pcpu, unless taking whole packages
   if (SMP_HTEnabled() &&
       !choice->wholePackage && 
       CpuSchedHTSharing(myVsmp) == CPUSCHED_HT_SHARE_INTERNALLY) {
      CpuSched_Vcpu *otherVcpu;
      ASSERT(myVsmp->vcpus.len == 2);
      ASSERT(myVcpu->schedIndex <= 1);
      otherVcpu = myVsmp->vcpus.list[1 - myVcpu->schedIndex];
      CpuSchedPcpuCoRun(myPcpu->partner, otherVcpu);
      return;
   }

   // initialize mask of forbidden pcpus
   forbidMask = PcpuMask(myPcpu->id, choice->wholePackage);

   // n.b. after this point, can no longer depend on sort order for
   //      remote preemptibility, since invalidated/updated by
   //      CpuSchedPcpuCoRun()

   mandatoryVcpuMask = choice->vcpusNeedCosched;
   FORALL_VSMP_VCPUS(myVsmp, vcpu) {
      if (vcpu != myVcpu &&
          !(VcpuMask(vcpu) & mandatoryVcpuMask)) {
         optionalVcpuMask |= VcpuMask(vcpu);
      }
   } VSMP_VCPUS_DONE;

   // first pass -- make sure we get all mandatory vcpus placed,
   // ideally leaving them on their current pcpus to preserve cache warmth
   CpuSchedCoSchedSubset(choice, FALSE, &mandatoryVcpuMask, &forbidMask);
   if (choice->pcpuMigrateAllowed) {
      CpuSchedCoSchedSubset(choice, TRUE, &mandatoryVcpuMask, &forbidMask);
   }
   ASSERT(mandatoryVcpuMask == 0);

   // second pass -- opportunistically try to schedule all optional
   // vcpus, also trying to preserve cache warmth
   CpuSchedCoSchedSubset(choice, FALSE, &optionalVcpuMask, &forbidMask);
   if (choice->pcpuMigrateAllowed) {
      CpuSchedCoSchedSubset(choice, TRUE, &optionalVcpuMask, &forbidMask);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedWarpConsole --
 *
 *	Temporarily warps Console OS world backwards in virtual time
 *	to reduce the latency until it is next scheduled.
 *	Caller must hold scheduler cell lock for console.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Modifies scheduler state.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedWarpConsole(void)
{
   // sanity check
   ASSERT(CpuSchedCellIsLocked(CONSOLE_CELL));

   if (cpuSched.vtConsoleWarpCurrent == 0) {
      if (CONSOLE_WORLD != NULL) {
         // adjust virtual time
         cpuSched.vtConsoleWarpCurrent = cpuSched.vtConsoleWarpDelta;
         ASSERT(cpuSched.vtConsoleWarpCurrent >= 0);
         CONSOLE_VSMP->vtime.main -= cpuSched.vtConsoleWarpCurrent;
         CONSOLE_VSMP->vtime.extra -= cpuSched.vtConsoleWarpCurrent;

         // update stats
         cpuSched.consoleWarpCount++;

         // requeue to handle main/extra queue movement
         if (CONSOLE_VCPU->runState == CPUSCHED_READY) {
            CpuSchedVcpuRequeue(CONSOLE_VCPU);
         }
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedUnwarpConsole --
 *
 *	Restores Console OS virtual time to unwarped value.
 *	Caller must hold scheduler cell lock for console.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Modifies scheduler state.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedUnwarpConsole(void)
{
   // sanity check
   ASSERT(CpuSchedCellIsLocked(CONSOLE_CELL));

   if (cpuSched.vtConsoleWarpCurrent > 0) {
      ASSERT(CONSOLE_WORLD != NULL);
      if (CONSOLE_WORLD != NULL) {
         // adjust virtual time
         CONSOLE_VSMP->vtime.main += cpuSched.vtConsoleWarpCurrent;
         CONSOLE_VSMP->vtime.extra += cpuSched.vtConsoleWarpCurrent;         
         cpuSched.vtConsoleWarpCurrent = 0;
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedPcpuVcpuChoose --
 *
 *	Examines "vcpu" as a candidate to run on "schedPcpu".
 *	If "vcpu" is runnable and has a smaller adjusted virtual
 *	time than "choice", updates "choice" appropriately. 
 *	Caller must hold scheduler cell lock for "schedPcpu".
 *
 * Results:
 *      Updates "choice".
 *
 * Side effects:
 *      Modifies scheduler state.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedPcpuVcpuChoose(const CpuSched_Pcpu *schedPcpu,
                       CpuSched_Vcpu *vcpu,
                       Bool extra,
                       CpuSchedChoice *choice,
                       Timer_RelCycles vcpuBonus)
{
   PCPU pcpu = schedPcpu->id;
   const CpuSched_Vsmp *vsmp = vcpu->vsmp;
   CpuMask vcpusNeedCosched;
   Bool ahead = (CpuSchedVtimeAhead(vsmp) > 0), needPackage = FALSE;   
   Sched_HTSharing sharing;
   
   // sanity check
   ASSERT(CpuSchedCellIsLocked(schedPcpu->cell));

   // don't choose if extra but vcpu would be on main queue
   if (!ahead && extra) {
      return;
   }

   // don't choose if main but vcpu would be on extra queue
   if (ahead && (!extra || !CpuSchedExtraEligible(vsmp))) {
      return;
   }

   // vcpu must be runnable
   if (CpuSchedVcpuIsIdle(vcpu) ||
       !CpuSchedVcpuIsRunnable(vcpu)) {
      return;
   }

   if (!CpuSchedVcpuAffinityPermitsPcpu(vcpu, pcpu, 0)) {
      return;
   }

   // only choose this vcpu if it beats the previous-best choice
   if (!CpuSchedVtimeContextBetterChoice(&vsmp->vtime, vcpuBonus, extra, choice)) {
      return;
   }

   // make sure we can get the whole package if we're sharing constrained
   sharing = CpuSchedHTSharing(vcpu->vsmp);
   if (sharing != CPUSCHED_HT_SHARE_ANY && !CpuSchedPartnerIsIdle(pcpu)) {
      ASSERT(SMP_HTEnabled());
      CpuSched_Vsmp *partnerVsmp = CpuSchedRunningVcpu(schedPcpu->partner->id)->vsmp;
      
      if (sharing == CPUSCHED_HT_SHARE_NONE ||
          (sharing == CPUSCHED_HT_SHARE_INTERNALLY && partnerVsmp != vsmp)) {
         if ((!extra && !CpuSchedPcpuCanPreemptMain(schedPcpu->partner, vsmp)) ||
             (extra && !CpuSchedPcpuCanPreemptExtra(schedPcpu->partner, vsmp))) {
            // bail, because we can't preempt the partner pcpu
            return;
         }
         
         needPackage = TRUE;
      }
   }
   
   if (!CpuSchedCanCoSchedule(vsmp, pcpu, NULL, needPackage, choice->pcpuMigrateAllowed, &vcpusNeedCosched)) {
      return;
   }
   
   CpuSchedChoiceUpdateExtended(choice, vcpu, vcpusNeedCosched, &vsmp->vtime, vcpuBonus);
   CpuSchedChoiceUseWholePackage(choice, needPackage);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedPcpuChoose --
 *
 *	Examines the best candidate vcpu in the scheduler run
 *	queue specified by "q" on "schedPcpu".  If a candidate
 *	is found with a smaller adjusted virtual time than
 *	"choice", updates "choice" appropriately.  If "choice"
 *	is updated with a candidate from a remote cell, its
 *	associated remote scheduler cell is locked appropriately.
 *	Caller must hold scheduler cell lock for "schedPcpu".
 *
 * Results:
 *      Updates "choice".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedPcpuChoose(const CpuSched_Pcpu *schedPcpu,      // in
                   Bool extra,				// in
                   CpuSchedChoice *choice)		// in+out
{
   const CpuSchedQueue *q = extra ? &schedPcpu->queueExtra : &schedPcpu->queueMain;
   Bool mig = choice->pcpuMigrateAllowed;
   PCPU myPCPU = MY_PCPU;
   List_Links *elt;

   // sanity check
   ASSERT(CpuSchedCellIsLocked(schedPcpu->cell));

   LIST_FORALL(&q->queue, elt) {
      CpuSched_Vcpu *vcpu = World_CpuSchedVcpu((World_Handle *) elt);
      CpuSched_Vsmp *vsmp = vcpu->vsmp;
      CpuMask vcpusNeedCosched;

      // skip vcpus in "limbo" on main queue that are
      // ineligible for extra time
      if (!extra && (CpuSchedVtimeAhead(vsmp) > 0)) {
         continue;
      }

      // skip if can't run on this pcpu
      if (!CpuSchedVcpuAffinityPermitsPcpu(vcpu, myPCPU, 0)) {
         continue;
      }

      // skip if no better than existing choice
      // currently no bonus applied to vsmp, but could apply migration penalty
      if (!CpuSchedVtimeContextBetterChoice(&vsmp->vtime, 0, extra, choice)) {
         continue;
      }

      // uni VM without HT constraints => no co-scheduling
      if (!CpuSchedIsMP(vsmp) && CpuSchedHTSharing(vsmp) == CPUSCHED_HT_SHARE_ANY) {
         CpuSchedChoiceUpdate(choice, vcpu, 0);
         continue;
      }

      // SMP VM => co-scheduling
      if (!SMP_HTEnabled()) {
         // no sharing constraints
         if (CpuSchedCanCoSchedule(vsmp, myPCPU, vcpu, FALSE, mig, &vcpusNeedCosched)) {
            CpuSchedChoiceUpdate(choice, vcpu, vcpusNeedCosched);
         }
      } else {
         // HT sharing constraints
         CpuSched_Pcpu *partner = CpuSchedPartnerPcpu(myPCPU);
         Bool canPreemptPartner = CpuSchedPcpuCanPreempt(partner, vsmp);
         Sched_HTSharing sharing = CpuSchedHTSharing(vsmp);
         
         // first check if able to get whole packages,
         // then fallback to sharing, if permitted
         if (canPreemptPartner &&
             CpuSchedCanCoSchedule(vsmp, myPCPU, vcpu, TRUE, mig, &vcpusNeedCosched)) {
            CpuSchedChoiceUpdate(choice, vcpu, vcpusNeedCosched);
            CpuSchedChoiceUseWholePackage(choice, TRUE);
         } else if (sharing == CPUSCHED_HT_SHARE_INTERNALLY) {
            ASSERT(vsmp->vcpus.len == 2);
            if (canPreemptPartner) {
               CpuSchedChoiceUpdate(choice, vcpu, vcpusNeedCosched);
            }
         } else if ((sharing != CPUSCHED_HT_SHARE_NONE) &&
                    CpuSchedCanCoSchedule(vsmp, myPCPU, vcpu, FALSE, mig, &vcpusNeedCosched)) {
            CpuSchedChoiceUpdate(choice, vcpu, vcpusNeedCosched);
         }
      }
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedShouldMoveCurrentRunner --
 *
 *	Returns the pcpu to which the current runner "vcpu" on "pcpu"
 *	should be moved, or "INVALID_PCPU" if "vcpu" should not move.
 *
 * Results:
 *	Returns new pcpu destination or INVALID_PCPU.
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */
static PCPU 
CpuSchedShouldMoveCurrentRunner(CpuSched_Pcpu *pcpu, CpuSched_Vcpu *vcpu)
{
   CpuSched_Cell *cell = pcpu->cell;
   CpuSched_VtimeContext vtIdle;
   Timer_RelCycles idleBonus;
   CpuSched_Pcpu *bestRemote;
   CpuMask partnerMask;
   Bool ahead;
   
   if (!CONFIG_OPTION(CPU_MOVE_CURRENT_RUNNER)
      || !vmkernelLoaded) {
      return INVALID_PCPU;
   }
   
   // initialize
   if (SMP_HTEnabled()) {
      partnerMask = PcpuMask(pcpu->partner->id, TRUE);
   } else {
      partnerMask = 0;
   }

   // compute idle virtual times
   CpuSchedIdleVtime(pcpu, &vtIdle, &idleBonus);

   // use the main or extra vtime of the idle world, as appropriate
   ahead = (vtIdle.main - (cell->vtime + cell->config.vtAheadThreshold)) > 0;

   // Basically we test to see if the idle world on this pcpu
   // could preempt some remote pcpu. If it could, we want to run
   // the idle world locally (since it can't move) and push our
   // current runner onto the preemptible remote processor
   bestRemote = NULL;
   FORALL_CELL_REMOTE_PCPUS(cell, pcpu->id, p) {
      CpuSched_Pcpu *r = CpuSchedPcpu(p);
      if (CpuSchedVcpuAffinityPermitsPcpu(vcpu, r->id, partnerMask)) {
         if (CpuSchedVtimeContextCompare(&vtIdle,
                                         idleBonus,
                                         &r->preemption.vtime,
                                         r->preemption.vtBonus,
                                         ahead) < 0) {
            if ((bestRemote == NULL) ||
                (CpuSchedVtimeContextCompare(&r->preemption.vtime,
                                             r->preemption.vtBonus,
                                             &bestRemote->preemption.vtime,
                                             bestRemote->preemption.vtBonus,
                                             ahead) < 0)) {
               bestRemote = r;
            }
         }
      }
   } CELL_REMOTE_PCPUS_DONE;

   if (bestRemote != NULL) {
      ASSERT(bestRemote->handoff == NULL);
      return(bestRemote->id);
   } else {
      return(INVALID_PCPU);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedCellSyncTime --
 *
 *	Synchronizes current real time and virtual time between
 *	"localCell" and "remoteCell".  The should be almost no
 *	drift across cells, except for the precise time of the last
 *	CpuSchedCellUpdateTime() performed on each cell, and
 *	potential rounding error accumulated during vtime
 *	computations.  Caller must hold scheduler cell locks for
 *	both "cellLocal" and "cellRemote".
 *
 * Results:
 *      Updates current real time and virtual time values
 *	associated with "localCell" and "remoteCell".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
CpuSchedCellSyncTime(CpuSched_Cell *localCell, CpuSched_Cell *remoteCell)
{
   CpuSchedVtime vtime;
   Timer_AbsCycles now;

   // sanity checks
   ASSERT(CpuSchedCellIsLocked(localCell));
   ASSERT(CpuSchedCellIsLocked(remoteCell));

   now = MAX(localCell->now, remoteCell->now);
   localCell->now = now;
   remoteCell->now = now;

   vtime = MAX(localCell->vtime, remoteCell->vtime);
   localCell->vtime = vtime;
   remoteCell->vtime = vtime;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedCellRemoteChoose --
 *
 *	Examines "cellRemote" for candidate vsmps to migrate to
 *	"cellLocal".  If a candidate is found with a smaller adjusted
 *	virtual time than "choice", updates "choice" appropriately.
 *	Caller must hold scheduler cell locks for both "cellLocal"
 *	and "cellRemote".
 *
 * Results:
 *      Updates "choice".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedCellRemoteChoose(const CpuSched_Cell *cellLocal,
                         const CpuSched_Cell *cellRemote,
                         Bool extra,
                         CpuSchedChoice *choice)
{
   // sanity checks
   ASSERT(cpuSched.nCells > 1);
   ASSERT(CpuSchedCellIsLocked(cellLocal));
   ASSERT(CpuSchedCellIsLocked(cellRemote));
   ASSERT(choice->cellMigrateAllowed);

   FORALL_CELL_PCPUS(cellRemote, i) {
      CpuSchedPcpuChoose(CpuSchedPcpu(i), extra, choice);
   } CELL_PCPUS_DONE;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedCellCanMigrateVsmp --
 *
 *	Returns TRUE if "vsmp" can be migrated to "cell".
 *	Caller must hold scheduler cell locks for "cell" and "vsmp".
 *
 * Results:
 *	Returns TRUE if "vsmp" is able to migrate to "cell".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static Bool
CpuSchedCellCanMigrateVsmp(const CpuSched_Cell *cell,
                           const CpuSched_Vsmp *vsmp)
{
   // sanity checks
   ASSERT(CpuSchedCellIsLocked(cell));
   ASSERT(CpuSchedVsmpCellIsLocked(vsmp));

   // XXX future: support "heavyweight" migration
   //     e.g. set vsmp "migrateToCell" field,
   //     change CanBusyWait() to return false if migrateToCell set
   //     co-stop and/or mark reschedule, last one down moves cell

   // can't migrate if any vcpus currently running or
   // in the middle of a handoff
   FORALL_VSMP_VCPUS(vsmp, vcpu) {
      if (CpuSchedVcpuRunOrBwait(vcpu)
         || vcpu->runState == CPUSCHED_READY_CORUN) {
         return(FALSE);
      }
   } VSMP_VCPUS_DONE;
   
   return(TRUE);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedUpdateRemotePreemption --
 *
 *	Updates preemptibility of all remote pcpus in the same
 *	cell as "pcpu".
 *
 * Results:
 *	Modifies preemption state associated with all remote
 *	pcpus in the same cell as "pcpu".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedUpdateRemotePreemption(CpuSched_Pcpu *pcpu)
{
   // update remote preemption state
   FORALL_CELL_REMOTE_PCPUS(pcpu->cell, pcpu->id, p) {
      CpuSched_Pcpu *remotePcpu = CpuSchedPcpu(p);
      CpuSchedPcpuPreemptionUpdate(remotePcpu);
      ASSERT(remotePcpu->preemption.valid);
   } CELL_REMOTE_PCPUS_DONE;
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedAcceptDirectedYield --
 *
 *      Returns TRUE iff "schedPcpu" should yield to yieldVcpu.
 *
 *      If it returns TRUE and yieldVcpu is in a different cell, the
 *      cell lock for that remote cell will still be held upon returning.
 *
 * Results:
 *      Returns TRUE iff "schedPcpu" should yield to yieldVcpu.
 *
 * Side effects:
 *      May acquire remote cell lock.
 *
 *-----------------------------------------------------------------------------
 */
Bool
CpuSchedAcceptDirectedYield(CpuSched_Pcpu *schedPcpu, CpuSched_Vcpu *yieldVcpu)
{
   CpuSched_Vsmp *yieldVsmp = yieldVcpu->vsmp;
   CpuSched_Cell *yieldCell = yieldVsmp->cell;
   SP_IRQL prevIRQL = SP_IRQL_NONE;
   CpuMask dummyMask;

   // TODO should only allow yielding within scheduler group to
   // avoid possible unfairness/gaming of the system
   if (yieldCell != schedPcpu->cell) {
      Bool remoteLocked;

      prevIRQL = CpuSchedCellTryLock(yieldCell, &remoteLocked);
      if (!remoteLocked) {
         return (FALSE);
      }
      if (yieldVsmp->cell != yieldCell
          || !CpuSchedCellCanMigrateVsmp(MY_CELL, yieldVcpu->vsmp)) {
         // if vsmp can't migrate to this cell, give up
         // also, if a rare race hits where the vsmp has already migrated
         // to a different cell, give up
         CpuSchedCellUnlock(yieldCell, prevIRQL);
         return (FALSE);
      }
   }

   if (yieldVcpu->runState != CPUSCHED_READY
       || !CpuSchedVcpuAffinityPermitsPcpu(yieldVcpu, schedPcpu->id, 0)
       || CpuSchedVcpusNeedCosched(yieldVsmp, yieldVcpu, &dummyMask) != 0
       || (CpuSchedVtimeAhead(yieldVsmp) > 0
           && !CpuSchedExtraEligible(yieldVsmp))) {
      // can only migrate a runnable vcpu with right affinity
      // that doesn't need other vcpus to be coscheduled
      if (yieldCell != schedPcpu->cell) {
         CpuSchedCellUnlock(yieldCell, prevIRQL);
      }
      return (FALSE);
   }

   // leave remote cell (if any) locked
   return (TRUE);
}
/*
 *----------------------------------------------------------------------
 *
 * CpuSchedChoose --
 *
 *	Selects a runnable vcpu for execution on processor myPcpu,
 *	using the two-level virtual-time scheduling algorithm, given
 *      that myVcpu was running there previously.
 *	Note that the currently-running vcpu is eligible to be selected.
 *	Caller must hold scheduler cell lock for "myPcpu".
 *
 * Results:
 *      Vcpu that should run next is stored in "choice->min", or NULL
 *      is stored there if no runnable world found.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedChoose(PCPU myPcpu,                    // in
               CpuSched_Vcpu *myVcpu,          // in
               CpuSched_Vcpu *directedYield,   // in
               CpuSchedChoice *choice)	       // out
{
   CpuSched_Cell *myCell, *remoteCell;
   CpuSched_VtimeContext idleVtime;
   CpuSched_Pcpu *schedPcpu;
   CpuSched_Vsmp *myVsmp;
   Timer_RelCycles idleBonus;
   
   // debugging
   CpuSchedLogEvent("choose", MY_RUNNING_WORLD->worldID);

   // convenient abbrevs
   schedPcpu = CpuSchedPcpu(myPcpu);
   myCell = schedPcpu->cell;
   myVsmp = myVcpu->vsmp;
   
   // sanity checks
   ASSERT(CpuSchedCellIsLocked(myCell));
   ASSERT(myVsmp->cell == myCell);

   // initialize "out" structure
   CpuSchedChoiceInit(choice);

   // forcibly co-scheduled by remote scheduler invocation?
   if (schedPcpu->handoff != NULL) {
      VcpuLogEvent(schedPcpu->handoff, "choose-hand");
      ASSERT(schedPcpu->handoff->pcpuHandoff == schedPcpu->id);
      ASSERT(schedPcpu->handoff->runState == CPUSCHED_READY_CORUN);
      ASSERT(CpuSchedVcpuIsRunnable(schedPcpu->handoff));
      schedPcpu->stats.handoff++;
      CpuSchedChoiceUpdate(choice, schedPcpu->handoff, 0);
      return;
   }

   // should we do a directed yield?
   if (directedYield != NULL) {
      if (CpuSchedAcceptDirectedYield(schedPcpu, directedYield)) {
         ASSERT(CpuSchedCellIsLocked(directedYield->vsmp->cell));
         schedPcpu->stats.dyield++;
         LOG(2, "directed yield from %u to %u",
             VcpuWorldID(myVcpu), VcpuWorldID(directedYield));
         CpuSchedChoiceUpdate(choice, directedYield, 0);
         choice->isDirectedYield = TRUE;
         return;
      } else {
         schedPcpu->stats.dyieldFailed++;
      }
   }

   // determine where we're allowed to look
   CpuSchedPcpuCheckMigrationAllowed(schedPcpu,
                                     &choice->pcpuMigrateAllowed,
                                     &choice->cellMigrateAllowed,
                                     &choice->runnerMoveAllowed);
   // reset runner move request
   schedPcpu->runnerMoveRequested = FALSE;
   
   // invalidate group vtime cache
   CpuSchedPcpuGroupVtimeCacheInvalidate(schedPcpu);
   
   // update remote preemption state
   if (choice->pcpuMigrateAllowed) {
      // only need to update all remote processor information if
      // we're allowed to migrate
      CpuSchedUpdateRemotePreemption(schedPcpu);
   } else if (SMP_HTEnabled()) {
      // even in the nomig case, we may still need to know if we
      // can preempt our partner
      CpuSchedPcpuPreemptionUpdate(schedPcpu->partner);
   }

   // Possibly move current runner to another pcpu.
   // There are two cases where a remote pcpu would be preferrable
   // to the current one:
   //  (a) If we're hyperthreaded and our partner LCPU is busy
   //      while some remote package is truly idle
   //  (b) If our current processor is taking a lot of interrupts
   //      while some remote processor isn't (this could happen
   //      on either a hyperthreaded or non-hyperthreaded box.
   // In either case, CpuSchedChoose() we'll return the idle world
   // as our choice and the current vcpu will soon be CoRun elsewhere
   if (choice->runnerMoveAllowed &&
       CpuSchedVcpuIsRunnable(myVcpu) &&
       myVcpu->affinityMask != CPUSCHED_AFFINITY(schedPcpu->id)) {
      choice->currentRunnerDest = 
         CpuSchedShouldMoveCurrentRunner(schedPcpu, myVcpu);         
      if (choice->currentRunnerDest != INVALID_PCPU) {
         // run dedicated idle world here
         CpuSchedChoiceUpdate(choice, CpuSchedGetIdleVcpu(myPcpu), 0);
         CpuSchedLogEvent("pcpu-move", choice->currentRunnerDest);
         cpuSched.numIdlePreempts++;
         return;
      }
   }

   // on a hyperthreaded system, we must always consider the possibility
   // that the idle world is the optimal choice to run here next
   CpuSchedIdleVtime(schedPcpu, &idleVtime, &idleBonus);
   CpuSchedChoiceUpdateExtended(choice, NULL, 0, &idleVtime, idleBonus);
   CpuSchedLogEvent("idle-main", idleVtime.main);

   // consider running main
   CpuSchedPcpuVcpuChoose(schedPcpu,
                          myVcpu,
                          FALSE,
                          choice,
                          myCell->config.preemptionBonusCycles);

   // consider local main queue first
   CpuSchedPcpuChoose(schedPcpu, FALSE, choice);
   VcpuLogEvent(choice->min, "choose-lmain");

   // OPT: we may not want to scan remote queues if someone on the local main
   // queue is runnable (min != NULL). Or we may want to scan remote queues
   // only occasionally in this case. This optimization formerly existed, but
   // it runs into problems when you have a cpu-burning VM or testworld that
   // is always running in the main queue. This was especially a problem when
   // a VM on a remote processor had its affinity changed so that it could now
   // only run on schedPcpu. If schedPcpu was running a CPU-burning task off
   // the main queue, the optimization would prevent it from ever picking up
   // that vcpu, so the vcpu would remain stuck forever.
   if (choice->pcpuMigrateAllowed) {
      FORALL_CELL_REMOTE_PCPUS(myCell, myPcpu, i) {
         CpuSchedPcpuChoose(CpuSchedPcpu(i), FALSE, choice);
      } CELL_REMOTE_PCPUS_DONE;
      VcpuLogEvent(choice->min, "choose-rmain");
   } else if (SMP_HTEnabled()) {
      // on a hyperthreaded system, always scan our partner hypertwin,
      // even if we're not allowed to do "remote" migration
      CpuSchedPcpuChoose(CpuSchedPartnerPcpu(myPcpu), FALSE, choice);
   }
   
   remoteCell = NULL;
   if (choice->cellMigrateAllowed) {
      CpuSched_Cell *rndCell;
      SP_IRQL prevIRQL;
      Bool rndLocked;
      uint32 rnd;

      // sanity check
      ASSERT(choice->pcpuMigrateAllowed);

      // choose remote cell randomly
      ASSERT(cpuSched.nCells > 1);
      rnd = CpuSchedRandom() % cpuSched.nCells;
      if (rnd == myCell->id) {
         rnd = (rnd + 1) % cpuSched.nCells;
      }
      rndCell = &cpuSched.cell[rnd];
      ASSERT(rndCell != myCell);

      prevIRQL = CpuSchedCellTryLock(rndCell, &rndLocked);
      if (rndLocked) {
         CpuSchedChoice prevChoice;
         CpuSchedChoiceCopy(&prevChoice, choice);
         ASSERT(CpuSchedCellIsLocked(rndCell));
         ASSERT(prevIRQL == CPUSCHED_IRQL);
         CpuSchedCellRemoteChoose(myCell, rndCell, FALSE, choice);
         if ((choice->min == NULL) ||
             ((choice->min != prevChoice.min) &&
              CpuSchedCellCanMigrateVsmp(myCell, choice->min->vsmp))) {
            // keep remoteCell locked
            remoteCell = rndCell;
            ASSERT((choice->min == NULL) ||
                   (choice->min->vsmp->cell == remoteCell));
         } else {
            // revert to intra-cell choice, unlock
            CpuSchedChoiceCopy(choice, &prevChoice);
            CpuSchedCellUnlock(rndCell, prevIRQL);
         }
         myCell->stats.remoteLockLast = TRUE;
         myCell->stats.remoteLockSuccess++;
      } else {
         myCell->stats.remoteLockLast = FALSE;
         myCell->stats.remoteLockFailure++;
      }
   }

   // consider extra queues, if necessary
   if (choice->min == NULL) {
      // consider idle vcpu
      ASSERT(CpuSchedVtimeContextEqual(choice->vtime, &idleVtime));
      CpuSchedLogEvent("idle-extra", idleVtime.extra);

      // consider running extra
      CpuSchedPcpuVcpuChoose(schedPcpu,
                             myVcpu,
                             TRUE,
                             choice,
                             myCell->config.preemptionBonusCycles);

      // consider local extra queues
      CpuSchedPcpuChoose(schedPcpu, TRUE, choice);
      VcpuLogEvent(choice->min, "choose-lextra");

      if (choice->pcpuMigrateAllowed) {
         // consider remote extra queues
         FORALL_CELL_REMOTE_PCPUS(myCell, myPcpu, i) {
            CpuSchedPcpuChoose(CpuSchedPcpu(i), TRUE, choice);
         } CELL_REMOTE_PCPUS_DONE;
         VcpuLogEvent(choice->min, "choose-rextra");
      } else if (SMP_HTEnabled()) {
         CpuSchedPcpuChoose(CpuSchedPartnerPcpu(myPcpu), TRUE, choice);
      }

      if (remoteCell != NULL) {
         CpuSchedChoice prevChoice;
         CpuSchedChoiceCopy(&prevChoice, choice);
         ASSERT(choice->cellMigrateAllowed);
         CpuSchedCellRemoteChoose(myCell, remoteCell, TRUE, choice);
         if ((choice->min != prevChoice.min) &&
             CpuSchedCellCanMigrateVsmp(myCell, choice->min->vsmp)) {
            // keep remoteCell locked
            ASSERT(choice->min->vsmp->cell == remoteCell);
         } else {
            // revert to intra-cell choice, unlock
            CpuSchedChoiceCopy(choice, &prevChoice);
            CpuSchedCellUnlock(remoteCell, CPUSCHED_IRQL);
            remoteCell = NULL;
         }
      }
   }
   
   // sanity checks
   ASSERT((remoteCell == NULL) || CpuSchedCellIsLocked(remoteCell));
   ASSERT((remoteCell == NULL) || (choice->min != NULL));
   ASSERT(CpuSchedCellIsLocked(myCell));

   // "choice" contains best candidate, if any
   return;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuMapPcpu --
 *
 *	Prepare "vcpu" to execute on processor "pcpu".  Maps PRDA and
 *	KSEG memory pages for "pcpu" into page tables for "vcpu", and
 *	marks "vcpu" as currently assigned to "pcpu".  Caller must
 *	hold scheduler cell lock for "pcpu".
 *
 * Results:
 *	Modifies "vcpu".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedVcpuMapPcpu(CpuSched_Vcpu *vcpu, PCPU pcpu)
{
   World_Handle *world;
   VMK_ReturnStatus status;

   // convenient abbrev
   world = World_VcpuToWorld(vcpu);

   // sanity check
   ASSERT(world != CONSOLE_WORLD);

   // special case: console mapped early and can't migrate
   if (World_IsHOSTWorld(world)) {
      VCPULOG(0, vcpu, "pcpu=%u: skipping console", pcpu);
      ASSERT(pcpu == CONSOLE_PCPU);
      vcpu->pcpuMapped = pcpu;
      return;
   }

   // update stats
   vcpu->stats.migrate++;
   if (vcpu->pcpuMapped != SMP_GetPartnerPCPU(pcpu)) {
      vcpu->stats.pkgMigrate++;
   }

   // map PRDA, Kseg regions
   status = PRDA_MapRegion(pcpu, world->pageRootMA);
   ASSERT_NOT_IMPLEMENTED(status == VMK_OK);
   status = Kseg_MapRegion(pcpu, world->pageRootMA);
   ASSERT_NOT_IMPLEMENTED(status == VMK_OK);

   // update mapped pcpu
   vcpu->pcpuMapped = pcpu;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedSwitch --
 *
 *	Switch contexts from the currently-executing vcpu "prev" to
 *	the specified vcpu "next", saving the state of "prev" and
 *	restoring the state of "next".  Caller must hold scheduler
 *	cell lock for local processor.
 *
 * Results:
 *      Returns the previous vcpu.  That is, return the value of
 *      "prev" in the pre-switch context.
 *      Modifies "prev" and "next".
 *
 * Side effects:
 *      Switches the currently-executing vcpu.
 *
 *----------------------------------------------------------------------
 */
static INLINE CpuSched_Vcpu *
CpuSchedSwitch(CpuSched_Vcpu *next, CpuSched_Vcpu *prev)
{
   World_Handle *prevWorld;

   // sanity check
   ASSERT_NO_INTERRUPTS();

   // switch world contexts
   prevWorld = World_Switch(World_VcpuToWorld(next), World_VcpuToWorld(prev));

   // sanity checks: interrupts disabled, PRDA mapped correctly
   ASSERT_NO_INTERRUPTS();
   ASSERT_PRDA_SANITY();

   // memory debugging support
   Watchpoint_Update();

   return World_CpuSchedVcpu(prevWorld);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedUpdateTime --
 *
 *	Updates current real-time and virtual-time for scheduler "cell".
 *	Ensures that times increase monotonically.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      Modifies "cell".
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedCellUpdateTime(CpuSched_Cell *cell)
{
   // obtain current time
   Timer_AbsCycles now = Timer_GetCycles();

   // debugging
   CpuSchedLogEvent("update-time", MY_PCPU);

   // sanity check
   ASSERT(CpuSchedCellIsLocked(cell));

   // ensure time increases monotonically
   if (LIKELY(now > cell->now)) {
      Timer_Cycles elapsed = now - cell->now;
      CpuSchedVtime vtElapsed;

      // sanity check
      if (UNLIKELY(elapsed > cpuSchedConst.cyclesPerMinute)) {
         uint32 deltaUsec;
         uint64 deltaSec;
         Timer_TCToSec(elapsed, &deltaSec, &deltaUsec);
         Warning("excessive time: elapsed=%Lu, elapsedSec=%Lu.%06d", 
                 elapsed, deltaSec, deltaUsec);
      }

      vtElapsed = CpuSchedTCToVtime(cpuSchedConst.nStride, elapsed);
      cell->vtime += vtElapsed;
      cell->now = now;

      // proactively prevent overflow
      if (UNLIKELY(cell->vtime > cell->config.vtimeResetThreshold)) {
         // reset vtime from timer callback (need to lock all cells)
         if (cell->vtResetTimer == TIMER_HANDLE_NONE) {
            cell->vtResetTimer = Timer_Add(MY_PCPU,
                                           (Timer_Callback) CpuSchedResetVtime,
                                           0, TIMER_ONE_SHOT, NULL);
         }
      }
   } else {
      // track amount that time went backwards
      Timer_Cycles lost = cell->now - now;
      cell->lostCycles += lost;

      // n.b. Timer_GetCycles() isn't guaranteed to be strictly
      // monotonically increasing across processors, due to small
      // potential skew in "synchronized" TSC values across
      // processors.  See PR 52137 for more details.  Log in
      // non-release builds if skew seems unreasonably large
      // (more than 10 microseconds).
      if (lost > Timer_USToTC(10)) {
         LOG(0, "time went backwards by %Ld usec", Timer_TCToUS(lost));
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVsmpSetBaseAlloc --
 *
 *	Sets the base allocation parameters for "vsmp" to "alloc".
 *	Caller must hold scheduler cell lock for "vsmp".
 *
 * Results:
 *	Modifies "vsmp".
 *
 * Side effects:
 *      Modifies scheduler state.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedVsmpSetBaseAlloc(CpuSched_Vsmp *vsmp, const CpuSched_Alloc *base)
{
   uint32 oldStride, oldStrideLimit;
   CpuSchedVtime vtime;

   // sanity check
   ASSERT(CpuSchedVsmpCellIsLocked(vsmp));

   // convenient abbrev
   vtime = vsmp->cell->vtime;

   // remember old values
   oldStride = vsmp->vtime.stride;
   oldStrideLimit = vsmp->strideLimit;

   // update rate bounds
   vsmp->base.min = base->min;
   vsmp->base.max = base->max;
   vsmp->strideLimit = CpuSchedSharesToStride(vsmp->base.max);

   // update shares
   vsmp->base.shares = base->shares;
   vsmp->vtime.stride = CpuSchedSharesToStride(vsmp->base.shares);
   
   // nStride represents the greatest amount that the vsmp's vtime
   // could advance in one unit of time. Because a vsmp on a hyperthreaded
   // system could occupy nvcpus packages, rather than nvcpus pcpus,
   // we must multiply by logicalPerPackage to compensate.
   vsmp->vtime.nStride = vsmp->vtime.stride * vsmp->vcpus.len * SMP_LogicalCPUPerPackage();

   // sanity checks
   ASSERT(vsmp->base.min <= vsmp->base.max);
   ASSERT(vsmp->base.shares <= vsmp->base.max);
   ASSERT(vsmp->vtime.stride >= vsmp->strideLimit);

   // adjust vtimes to account for update
   if (!CpuSchedVsmpIsSystemIdle(vsmp)) {
      Bool updated = FALSE;
      CpuSchedVtime delta;

      // adjust main and extra vtimes, if necessary
      if ((oldStride > 0) && (oldStride != vsmp->vtime.stride)) {
         delta = vsmp->vtime.main - vtime;
         vsmp->vtime.main =
            vtime + CpuSchedVtimeScale(delta, vsmp->vtime.stride, oldStride);
         delta = vsmp->vtime.extra - vtime;
         vsmp->vtime.extra =
            vtime + CpuSchedVtimeScale(delta, vsmp->vtime.stride, oldStride);
         updated = TRUE;
      }

      // adjust limit vtime, if necessary
      if ((oldStrideLimit > 0) && (oldStrideLimit != vsmp->strideLimit)) {
         delta = vsmp->vtimeLimit - vtime;
         vsmp->vtimeLimit =
            vtime + CpuSchedVtimeScale(delta, vsmp->strideLimit, oldStrideLimit);
         updated = TRUE;
      }

      // requeue vcpus if updated
      if (updated) {
         FORALL_VSMP_VCPUS(vsmp, vcpu) {
            if (vcpu->runState == CPUSCHED_READY) {
               CpuSchedVcpuRequeue(vcpu);
            }
         } VSMP_VCPUS_DONE;
      }
   }

   // special case: adjust warp when COS updated
   if (World_IsHOSTWorld(vsmp->leader)) {
      ASSERT(vsmp->cell == CONSOLE_CELL);
      cpuSched.vtConsoleWarpDelta =
         CpuSchedTCToVtime(vsmp->vtime.stride, vsmp->cell->config.consoleWarpCycles);
      // debugging
      if (CPUSCHED_DEBUG_VERBOSE) {
         VsmpLog(vsmp, "vtConsoleWarpDelta=%Ld", cpuSched.vtConsoleWarpDelta);
      }
   }

   // debugging
   if (CPUSCHED_DEBUG_VERBOSE) {
      VsmpLog(vsmp,
              "min=%u, max=%u, shares=%u, "
              "stride=%u, nstride=%u, nvcpus=%d",
              vsmp->base.min, vsmp->base.max, vsmp->base.shares,
              vsmp->vtime.stride, vsmp->vtime.nStride, vsmp->vcpus.len);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedGroupSetBaseAlloc --
 *
 *	Sets the base allocation parameters for "cpuGroup" to "alloc",
 *	and updates vsmp count to "vsmpCount".
 *	Caller must hold scheduler tree lock.
 *
 * Results:
 *	Modifies "cpuGroup".
 *
 * Side effects:
 *      Modifies scheduler state.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedGroupSetBaseAlloc(CpuSched_GroupState *cpuGroup,
                          const CpuSched_Alloc *base,
                          uint32 vsmpCount)
{
   CpuSchedVtime vtime, delta, updateVtime, updateVtimeLimit;
   uint32 oldStride, oldStrideLimit, oldVsmpCount;
   Bool update;

   // sanity check
   ASSERT(Sched_TreeIsLocked());

   // convenient abbrev
   vtime = MY_CELL->vtime;

   // remember old values
   oldVsmpCount = cpuGroup->vsmpCount;
   oldStride = cpuGroup->stride;
   oldStrideLimit = cpuGroup->strideLimit;

   // update vsmp count
   cpuGroup->vsmpCount = vsmpCount;

   // update rate bounds
   cpuGroup->base.min = base->min;
   cpuGroup->base.max = base->max;
   cpuGroup->strideLimit = CpuSchedSharesToStride(cpuGroup->base.max);

   // update shares
   cpuGroup->base.shares = base->shares;
   cpuGroup->stride = CpuSchedSharesToStride(cpuGroup->base.shares);
   
   // sanity checks
   ASSERT(cpuGroup->base.min <= cpuGroup->base.max);
   ASSERT(cpuGroup->base.shares <= cpuGroup->base.max);
   ASSERT(cpuGroup->stride >= cpuGroup->strideLimit);

   // adjust vtimes to account for update
   updateVtime = cpuGroup->vtime;
   updateVtimeLimit = cpuGroup->vtimeLimit;
   update = FALSE;

   if ((oldVsmpCount == 0) && (vsmpCount > 0)) {
      // special case: added first vsmp to group
      updateVtime = vtime;
      updateVtimeLimit = vtime;
      update = TRUE;
   } else {
      // adjust vtime, if necessary
      if ((oldStride > 0) && (oldStride != cpuGroup->stride)) {
         delta = cpuGroup->vtime - vtime;
         updateVtime = vtime +
            CpuSchedVtimeScale(delta, cpuGroup->stride, oldStride);
         update = TRUE;
      }

      // adjust limit vtime, if necessary
      if ((oldStrideLimit > 0) && (oldStrideLimit != cpuGroup->strideLimit)) {
         delta = cpuGroup->vtimeLimit - vtime;
         updateVtimeLimit = vtime + 
            CpuSchedVtimeScale(delta, cpuGroup->strideLimit, oldStrideLimit);
         update = TRUE;
      }
   }

   // perform updates, if necessary
   if (update) {
      // versioned atomic update protected by scheduler tree lock
      CPUSCHED_VERSIONED_ATOMIC_UPDATE_BEGIN(&cpuGroup->vtimeVersion);
      cpuGroup->vtime = updateVtime;
      cpuGroup->vtimeLimit = updateVtimeLimit;
      CPUSCHED_VERSIONED_ATOMIC_UPDATE_END(&cpuGroup->vtimeVersion);      
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVsmpNode --
 *
 *	Returns the scheduler tree node associated with "vsmp", or
 *	NULL if no such association exists.  Caller must hold the
 *	scheduler tree lock.
 *
 * Results:
 *	Returns the scheduler tree node associated with "vsmp", or
 *	NULL if no such association exists.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Sched_Node *
CpuSchedVsmpNode(const CpuSched_Vsmp *vsmp)
{
   ASSERT(Sched_TreeIsLocked());
   return(vsmp->leader->sched.group.node);   
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVsmpSetAllocInt --
 *
 *	Sets the external allocation parameters for "vsmp" to "alloc".
 *	Caller must hold scheduler cell lock for "vsmp" and the
 *	scheduler tree lock.
 *
 * Results:
 *	Modifies "vsmp".
 *
 * Side effects:
 *      Modifies scheduler state.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedVsmpSetAllocInt(CpuSched_Vsmp *vsmp, const CpuSched_Alloc *alloc)
{
   uint32 shares;

   // sanity checks
   ASSERT(CpuSchedVsmpCellIsLocked(vsmp));
   ASSERT(Sched_TreeIsLocked());
   ASSERT((alloc->max == CPUSCHED_ALLOC_MAX_NONE) ||
          (alloc->max >= alloc->min));

   // ensure shares within valid range
   shares = alloc->shares;
   if (shares < CPUSCHED_SHARES_MIN) {
      shares = CPUSCHED_SHARES_MIN;
   } else if (shares > CPUSCHED_SHARES_MAX) {
      shares = CPUSCHED_SHARES_MAX;
   }

   // update external allocation
   vsmp->alloc = *alloc;
   vsmp->alloc.shares = shares;

   // convert trivial cpu max into MAX_NONE
   if (CpuSchedUnitsToBaseShares(alloc->max, alloc->units) ==
       CPUSCHED_BASE_PER_PACKAGE * vsmp->vcpus.len) {
      if (CPUSCHED_DEBUG) {
         VSMPLOG(1, vsmp, "convert trivial max=%u to max=none", alloc->max);
      }
      vsmp->alloc.max = CPUSCHED_ALLOC_MAX_NONE;
   }

   // request reallocation
   CpuSched_RequestReallocate();
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVsmpSetAllocSpecial --
 *
 *	Attempts to set the external allocation parameters for "vsmp"
 *	with "numVcpus" vcpus to "alloc". Caller must hold all
 *	scheduler cell locks.
 *
 * Results:
 *	Modifies "vsmp".  Returns VMK_OK if successful, otherwise
 *	error code.
 *
 * Side effects:
 *      Modifies scheduler state.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
CpuSchedVsmpSetAllocSpecial(CpuSched_Vsmp *vsmp,
                            const CpuSched_Alloc *alloc,
                            uint8 numVcpus)
{
   ASSERT(CpuSchedAllCellsAreLocked());

   Sched_TreeLock();
   if (!CpuSchedVsmpAllocAllowed(vsmp, alloc, numVcpus)) {
      Sched_TreeUnlock();
      return(VMK_BAD_PARAM);
   }
   CpuSchedVsmpSetAllocInt(vsmp, alloc);
   Sched_TreeUnlock();

   return(VMK_OK);
}

// simple wrapper
static INLINE VMK_ReturnStatus
CpuSchedVsmpSetAlloc(CpuSched_Vsmp *vsmp, const CpuSched_Alloc *alloc)
{
   return(CpuSchedVsmpSetAllocSpecial(vsmp, alloc, vsmp->vcpus.len));
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedAllocInit --
 *
 *	Initializes "alloc" using specified "min", "max", "units", 
 *	and "shares".
 *
 * Results:
 *	Initializes "alloc" using specified parameters.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
CpuSchedAllocInit(CpuSched_Alloc *alloc, 
		  uint32 min, 
		  uint32 max, 
		  Sched_Units units, 
		  uint32 shares)
{
   // sanity check for supported units
   ASSERT(units == SCHED_UNITS_PERCENT ||
          units == SCHED_UNITS_MHZ ||
          units == SCHED_UNITS_BSHARES); 
   alloc->min = min;
   alloc->max = max;
   alloc->shares = shares;
   alloc->units = units;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedAllocEqual --
 *
 *	Returns TRUE iff "a" and "b" have identical contents.
 *
 * Results:
 *	Returns TRUE iff "a" and "b" have identical contents.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
CpuSchedAllocEqual(const CpuSched_Alloc *a, const CpuSched_Alloc *b)
{
   return((a->min == b->min) &&
          (a->max == b->max) &&
          (a->units == b->units) &&
          (a->shares == b->shares));
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVsmpRevokeAlloc --
 *
 *	Revokes external and internal allocation for "vsmp".
 *	Used when "vsmp" is terminating.
 *
 * Results:
 *	Modifies "vsmp".
 *
 * Side effects:
 *      Modifies scheduler state.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedVsmpRevokeAlloc(CpuSched_Vsmp *vsmp)
{
   CpuSched_Alloc alloc;

   // set allocations to zero
   CpuSchedAllocInit(&alloc, 0, 0, SCHED_UNITS_BSHARES, 0);
   Sched_TreeLock();
   CpuSchedVsmpSetAllocInt(vsmp, &alloc);
   Sched_TreeUnlock();

   CpuSchedVsmpSetBaseAlloc(vsmp, &alloc);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuSetAffinityUni --
 *
 *	Sets the hard affinity for "vcpu" to "affinity".  The affinity
 *	mask specifies the set of processor on which "vcpu" is allowed
 *	to execute. Applies to uniprocessor VMs only.
 *      Caller must hold scheduler cell lock for "vcpu".
 *
 * Results:
 *	Modifies "vcpu".
 *
 * Side effects:
 *      Modifies scheduler state.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
CpuSchedVcpuSetAffinityUni(CpuSched_Vcpu *vcpu, CpuMask affinity)
{
   ASSERT(!CpuSchedIsMP(vcpu->vsmp));
   ASSERT(CpuSchedVsmpCellIsLocked(vcpu->vsmp));
   
   // fail if invalid mask
   if (affinity == 0) {
      return(VMK_BAD_PARAM);
   }

   // fail if dedicated idle world or COS
   if (vcpu->idle || World_IsHOSTWorld(World_VcpuToWorld(vcpu))) {
      return(VMK_BAD_PARAM);
   }
   
   CpuSchedVcpuSetAffinityMask(vcpu, affinity, TRUE);
   
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVtimeResetAdjust --
 *
 *	Updates "vtime" to reflect global virtual time reset.
 *	No effect if "vtime" is the special value CPUSCHED_VTIME_MAX.
 *
 * Results:
 *	Modifies "vtime".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
CpuSchedVtimeResetAdjust(CpuSchedVtime *vtime)
{
   CpuSchedVtime vtimeResetAdjust = MY_CELL->config.vtimeResetAdjust;

   // sanity check
   ASSERT(*vtime <= CPUSCHED_VTIME_MAX);
   
   // adjust vtime, unless special max value
   if (*vtime != CPUSCHED_VTIME_MAX) {
      if (*vtime > vtimeResetAdjust) {
         *vtime -= vtimeResetAdjust;
      } else {
         *vtime = 0;
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedResetVtime --
 *
 *	Updates all virtual times associated with scheduler to reflect
 *	global virtual time reset.  Subtracts fixed amount of virtual
 *	time from global scheduler, all vsmps, and per-vcpu wait
 *	meters.  Does not modify any virtual times with the special
 *	value CPUSCHED_VTIME_MAX.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      Modifies scheduler state.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedResetVtime(UNUSED_PARAM(void *ignore))
{
   SP_IRQL prevIRQL;
   uint32 needReset;

   prevIRQL = CpuSchedLockAllCells();

   needReset = 0;
   FORALL_CELLS(c) {
      // determine if reset needed
      if (c->vtime > c->config.vtimeResetThreshold) {
         needReset++;
      }

      // prevent redundant callbacks
      Timer_Remove(c->vtResetTimer);
      c->vtResetTimer = TIMER_HANDLE_NONE;
   } CELLS_DONE;

   // avoid unnecessary work
   if (needReset == 0) {
      // release locks, done
      CpuSchedUnlockAllCells(prevIRQL);
      CpuSchedLog("already reset, count=%u", cpuSched.resetVtimeCount);
      return;
   }

   // update stats
   cpuSched.resetVtimeCount++;

   // debugging
   CpuSchedLog("reset vtime: count=%u, vtime=%Ld, needReset=%u",
               cpuSched.resetVtimeCount, MY_CELL->vtime, needReset);

   FORALL_CELLS(c) {
      // adjust cell vtime
      CpuSchedVtimeResetAdjust(&c->vtime);

      // adjust vm vtimes
      FORALL_CELL_VSMPS(c, vsmp) {
         // adjust vsmp virtual times
         CpuSchedVtimeResetAdjust(&vsmp->vtime.main);
         CpuSchedVtimeResetAdjust(&vsmp->vtime.extra);
         CpuSchedVtimeResetAdjust(&vsmp->vtimeLimit);

         // adjust vcpu virtual times (meters)
         FORALL_VSMP_VCPUS(vsmp, vcpu) {
            // only wait meters use vtStart
            CpuSched_StateMeter *m;
            m = &vcpu->runStateMeter[CPUSCHED_WAIT];
            CpuSchedVtimeResetAdjust(&m->vtStart);
            m = &vcpu->runStateMeter[CPUSCHED_BUSY_WAIT];
            CpuSchedVtimeResetAdjust(&m->vtStart);
         } VSMP_VCPUS_DONE;
      } CELL_VSMPS_DONE;
   } CELLS_DONE;

   // adjust group vtimes
   CpuSchedResetAllGroupVtimes();

   CpuSchedUnlockAllCells(prevIRQL);
}


// locked wrapper for CpuSched_VcpuArrayRemove
static INLINE void
CpuSchedVcpuArrayRemove(CpuSched_Vsmp *vsmp,
                        CpuSched_Vcpu *vcpu)
{
   SP_SpinLockIRQ *lock = &vsmp->vcpuArrayLock;
   SP_LockIRQ(lock, SP_IRQL_KERNEL);
   CpuSched_VcpuArrayRemove(&vsmp->vcpus, vcpu);
   SP_UnlockIRQ(lock, SP_GetPrevIRQ(lock));   
}

// locked wrapper for CpuSched_VcpuArrayAdd
static INLINE void
CpuSchedVcpuArrayAdd(CpuSched_Vsmp *vsmp,
                     CpuSched_Vcpu *vcpu)
{
   SP_SpinLockIRQ *lock = &vsmp->vcpuArrayLock;
   SP_LockIRQ(lock, SP_IRQL_KERNEL);
   CpuSched_VcpuArrayAdd(&vsmp->vcpus, vcpu);
   SP_UnlockIRQ(lock, SP_GetPrevIRQ(lock));
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVsmpCanDeschedule --
 *
 *	Returns TRUE iff "vsmp" is allowed to be co-descheduled.
 *
 * Results:
 *	Returns TRUE iff "vsmp" is allowed to be co-descheduled.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
CpuSchedVsmpCanDeschedule(const CpuSched_Vsmp *vsmp)
{
   return vsmp->disableCoDeschedule == 0;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVsmpCoStopSanityCheck --
 *
 *	Debugging code to perform sanity checks related to
 *	co-descheduling.  Issues warning with backtrace if
 *	any sanity checks fail.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedVsmpCoStopSanityCheck(const CpuSched_Vsmp *vsmp)
{
   uint32 *p = (uint32 *) &vsmp;
   uint32 nReady = 0, nIdle = 0, nOther = 0;

   // sanity checks
   ASSERT(CpuSchedVsmpCellIsLocked(vsmp));

   // nothing to check if not co-stopped
   if (vsmp->coRunState != CPUSCHED_CO_STOP) {
      return;
   }

   // count vcpus that aren't ready or idle
   FORALL_VSMP_VCPUS(vsmp, vcpu) {
      if (vcpu->runState == CPUSCHED_READY_COSTOP) {
         nReady++;
      } else if (CpuSchedVcpuIsWaiting(vcpu) &&
                 (vcpu->waitState == CPUSCHED_WAIT_IDLE)) {
         nIdle++;
      } else {
         nOther++;
      }
   } VSMP_VCPUS_DONE;

   // check: shouldn't co-deschedule if all vcpus ready or idle
   if (nOther == 0) {
      VsmpWarn(vsmp,
               "all vcpus ready or idle:"
               "nReadyCoStop=%u, nIdle=%u: "
               "vsmp nRun=%d, nWait=%d, nIdle=%d",
               nReady, nIdle,
               vsmp->nRun, vsmp->nWait, vsmp->nIdle);
      VsmpWarn(vsmp,
               "cur world state: %s",
               CpuSchedRunStateName(World_CpuSchedVcpu(MY_RUNNING_WORLD)->runState));
      
      Util_Backtrace(*(p - 1), *(p - 2), _Log, TRUE);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedVsmpMixedPkgs --
 *
 *      Returns TRUE iff at least one vcpu in "vsmp" has a full package and
 *      at least one vcpu in "vsmp" has a half package.
 *
 * Results:
 *      Returns TRUE iff at least one vcpu in "vsmp" has a full package and
 *      at least one vcpu in "vsmp" has a half package.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE Bool
CpuSchedVsmpMixedPkgs(const CpuSched_Vsmp *vsmp)
{
   int numWhole = 0, numHalf = 0;

   if (!SMP_HTEnabled()) {
      return (FALSE);
   }
   
   // first, find out if any vcpu has a full package
   FORALL_VSMP_VCPUS(vsmp, v) {
      if (CpuSchedPartnerIsIdle(v->pcpu)) {
         numWhole++;
      } else {
         numHalf++;
      }
   } VSMP_VCPUS_DONE;
   
   return (numWhole > 0 && numHalf > 0);
}


/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedVsmpIntraSkewOut --
 *
 *      Returns TRUE if "vsmp" should skew out because one if its vcpus has
 *      not made enough forward progress recently.
 *
 * Results:
 *      Returns TRUE if "vsmp" should skew out for intraskew.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE Bool
CpuSchedVsmpIntraSkewOut(const CpuSched_Vsmp *vsmp)
{
   ASSERT(SP_IsLockedIRQ(&vsmp->vcpuArrayLock) || CpuSchedVsmpCellIsLocked(vsmp));
   
   // we're skewed if some vcpu isn't running that should be
   FORALL_VSMP_VCPUS(vsmp, v) {
      if (CpuSchedVcpuNeedsCosched(v) &&
          v->runState != CPUSCHED_RUN &&
          v->waitState != CPUSCHED_WAIT_IDLE) {
         return (TRUE);
      }
   } VSMP_VCPUS_DONE;

   return (FALSE);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedVsmpHTSkewOut --
 *
 *      Returns TRUE if "vsmp" should skew out because it is running with
 *      "mixed" packages (some vcpu has a whole package, some has a half
 *      package) and one of the half-package vcpus needs coscheduling.
 *
 * Results:
 *      Returns TRUE if "vsmp" should skew out from HT.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE Bool
CpuSchedVsmpHTSkewOut(const CpuSched_Vsmp *vsmp)
{
   ASSERT(SP_IsLockedIRQ(&vsmp->vcpuArrayLock) || CpuSchedVsmpCellIsLocked(vsmp));
   
   if (!SMP_HTEnabled()) {
      return (FALSE);
   }
   
   if (!CpuSchedVsmpMixedPkgs(vsmp)) {
      return (FALSE);
   }

   FORALL_VSMP_VCPUS(vsmp, v) {
      if (CpuSchedVcpuNeedsCosched(v) &&
          v->runState == CPUSCHED_RUN &&
          !CpuSchedPartnerIsIdle(v->pcpu)) {
         // this vcpu needs to be coscheduled, but it
         // only has half a package right now, so skew out
         return (TRUE);
      }
   } VSMP_VCPUS_DONE;
   
   return (FALSE);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedVsmpStrictSkewOut --
 *
 *      Returns TRUE if this vsmp should be stopped due to strict skew
 *      violations. Strict skew violated if the sum of all vcpu intraSkews
 *      exceeds the configured threshold.
 *
 * Results:
 *      Returns TRUE if this vsmp has strict-skewed out
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
static Bool
CpuSchedVsmpStrictSkewOut(const CpuSched_Vsmp *vsmp)
{
   int totalSkew = 0;
   
   ASSERT(SP_IsLockedIRQ(&vsmp->vcpuArrayLock) || CpuSchedVsmpCellIsLocked(vsmp));

   // skew disabled
   if (MY_CELL->config.skewSampleThreshold == CPUSCHED_IGNORE_SKEW) {
      return (FALSE);
   }
   
   if (vsmp->nRun + vsmp->nIdle == vsmp->vcpus.len) {
      // all vcpus running or idle, so not skewed
      return (FALSE);
   }

   // skew out if sum of skew (over all vcpus) exceeds threshold
   FORALL_VSMP_VCPUS(vsmp, v) {
      totalSkew += v->intraSkew;
   } VSMP_VCPUS_DONE;

   return (totalSkew > MY_CELL->config.skewSampleThreshold);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedVsmpSkewedOut --
 *
 *      Returns TRUE if this vsmp is skewed out due to hyperthreading, strict
 *      skew, or relaxed skew, whichever is/are applicable.
 *
 * Results:
 *      Returns TRUE if this vsmp is skewed out.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE Bool
CpuSchedVsmpSkewedOut(const CpuSched_Vsmp *vsmp)
{
   // no skew for unis
   if (!CpuSchedIsMP(vsmp)) {
      return (FALSE);
   }

   // check for HT skew
   if (SMP_HTEnabled() && CpuSchedVsmpHTSkewOut(vsmp)) {
      return (TRUE);
   }

   if (CpuSchedVsmpStrictCosched(vsmp)) {
      // handle strict skew
      return (CpuSchedVsmpStrictSkewOut(vsmp));
   } else {
      // handle relaxed skew
      return (CpuSchedVsmpIntraSkewOut(vsmp));
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVsmpAggregateStateCheck --
 *
 *	Performs a consistency check on the aggregate state counts
 *	assoicated with "vsmp", checking against its current vcpu states.
 *	Intended for debugging.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      May assert-fail or warn if inconsistency detected.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedVsmpAggregateStateCheck(const CpuSched_Vsmp *vsmp)
{
   int nRun = 0, nIdle = 0, nOther = 0, nWait = 0, nDisableCoDesched = 0;

   // sanity check
   ASSERT(CpuSchedVsmpCellIsLocked(vsmp));

   // inspect current state of all vcpus
   FORALL_VSMP_VCPUS(vsmp, vcpu) {
      if (vcpu->runState == CPUSCHED_RUN) {
         nRun++;
      } else if (vcpu->waitState == CPUSCHED_WAIT_IDLE) {
         nIdle++;
      } else {
         nOther++;
      }

      if (CpuSchedWaitStateDisablesCoDesched(vcpu->waitState)) {
         ASSERT(CpuSchedVcpuIsWaiting(vcpu));
         nDisableCoDesched++;
      }

      if (CpuSchedVcpuIsWaiting(vcpu)) {
         nWait++;
      }
   } VSMP_VCPUS_DONE;

   // valid nRun?
   ASSERT(vsmp->nRun == nRun);
   if (vsmp->nRun != nRun) {
      VsmpWarn(vsmp, "inconsistent: nRun=%d, vsmp.nRun=%d",
               nRun, vsmp->nRun);
   }

   // valid nIdle?
   ASSERT(vsmp->nIdle == nIdle);
   if (vsmp->nIdle != nIdle) {
      VsmpWarn(vsmp, "inconsistent: nIdle=%d, vsmp.nIdle=%d",
               nIdle, vsmp->nIdle);
   }

   // valid nWait?
   ASSERT(vsmp->nWait == nWait);
   if (vsmp->nWait != nWait) {
      VsmpWarn(vsmp, "inconsistent: nWait=%d, vsmp.nWait=%d",
               nWait, vsmp->nWait);
   }

   // valid discod?
   if (CpuSchedIsMP(vsmp)) {
      if (vsmp->disableCoDeschedule != nDisableCoDesched) {
         Warning("vsmp->disableCoDeschedule = %d, nDisable=%d",
             vsmp->disableCoDeschedule, nDisableCoDesched);
         FORALL_VSMP_VCPUS(vsmp, vcpu) {
            VcpuWarn(vcpu,
                     "runState=%s, waitState=%s, coState=%s",
                     CpuSchedRunStateName(vcpu->runState),
                     CpuSchedWaitStateName(vcpu->waitState),
                     CpuSchedCoRunStateName(vsmp->coRunState));
         } VSMP_VCPUS_DONE;
      }
      ASSERT(vsmp->disableCoDeschedule == nDisableCoDesched);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedSampleIntraSkew --
 *
 *      Takes a new sample of the intra-skew state of "vsmp," updating
 *      each vcpu's intraSkew to reflect whether or not it is running in
 *      sync with its partners.
 *
 * Results:
 *      Returns TRUE if "vsmp" should be co-stopped due to intra-skew
 *
 * Side effects:
 *      Updates intr-skew for all vsmp vcpus, updates stats.
 *
 *-----------------------------------------------------------------------------
 */
static Bool
CpuSchedSampleIntraSkew(CpuSched_Vsmp *vsmp)
{
   Bool intraSkewOut = FALSE, mixedPkg;
   
   vsmp->skew.stats.intraSkewSamples++;

   mixedPkg = CpuSchedVsmpMixedPkgs(vsmp);

   ASSERT(SMP_HTEnabled() || !mixedPkg);
   
   // increment intraskew for vcpus that are neither
   // running nor idle
   FORALL_VSMP_VCPUS(vsmp, v) {
      if (v->runState != CPUSCHED_RUN
          && v->waitState != CPUSCHED_WAIT_IDLE) {
         // being descheduled is twice as bad as having only half
         // a package, so accumulate skew at twice the rate
         v->intraSkew += SMP_LogicalCPUPerPackage();
         
         // somebody in this vsmp should be running, but isn't
         // (it might be non-idle waiting, but that's still bad)
         if (CpuSchedVcpuNeedsCosched(v)) {
            Log_Event("intra-skew", 0, EVENTLOG_CPUSCHED_COSCHED);
            vsmp->skew.stats.intraSkewOut++;
            intraSkewOut = TRUE;
         }
      } else if (mixedPkg && !CpuSchedPartnerIsIdle(v->pcpu)) {
         // hyperthread skew, just gets one point
         v->intraSkew++;
      } else if (v->intraSkew > 0) {
         // this vcpu is running fine, decrement skew count,
         // but don't go below 0
         v->intraSkew = MAX(0, v->intraSkew - SMP_LogicalCPUPerPackage());
      }

      Trace_Event(TRACE_SCHED_INTRASKEW, VcpuWorldID(v), v->pcpu, 0, v->intraSkew);
      if (vmx86_debug || vmx86_devel) {
         Histogram_Insert(v->intraSkewHisto, v->intraSkew);
      }
      ASSERT(v->intraSkew >= 0);
   } VSMP_VCPUS_DONE;

   if (vmx86_debug && intraSkewOut) {
      FORALL_VSMP_VCPUS(vsmp, v) {
         Trace_Event(TRACE_SCHED_INTRASKEW_OUT, VcpuWorldID(v), v->pcpu, 0, v->intraSkew);
      } VSMP_VCPUS_DONE;
   }
   
   return (intraSkewOut);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedSampleSkew --
 *
 *   Timer callback to sample skew status for vsmp associated with
 *   the current running world on this PCPU. Each PCPU gets its own
 *   periodic timer that calls this function every CPU_SKEW_SAMPLE_USEC.
 *
 *   To avoid oversampling large SMPs, we rate-limit the skew sampling
 *   so that at least skewSampleMinInterval cycles pass between samples
 *   for a given vsmp.
 *
 *   Note that this function grabs a per-vsmp skew sample lock, but
 *   does not grab the cpuSched lock in the common case. The scheduler
 *   cell lock is only acquired if this vsmp has exceeded its skew threshold
 *   and we want to skew it out immediately.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      Modifies skew state for vsmp running on current processor.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedSampleSkew(UNUSED_PARAM(void *data), Timer_AbsCycles timestamp)
{
   World_Handle *world;
   CpuSched_Vsmp *vsmp;
   Timer_AbsCycles now;
   Bool acquired;
   Bool skewOut;
   SP_SpinLockIRQ *lock;
   SP_IRQL prevArrayIRQL;
   
   ASSERT(!CpuSched_IsPreemptible());
   world = MY_RUNNING_WORLD;

   // convenient abbrev
   vsmp = World_CpuSchedVsmp(world);
   
   if (!CpuSchedIsMP(vsmp) ||
       World_CpuSchedVcpu(world)->runState != CPUSCHED_RUN) {
      // we don't need to deal with skew, because this is a uni
      // or it's busy waiting
      return;
   }

   // lock the vcpu array state (which includes skew information).
   // This also protects us from addition or removal of vcpus in the vsmp.
   // Note that skew stats readers do not always acquire this lock,
   // so they may observe stale or inconsistent data.
   lock = &vsmp->vcpuArrayLock;
   prevArrayIRQL = SP_TryLockIRQ(lock, SP_IRQL_KERNEL, &acquired);
   if (!acquired) {
      // somebody else is updating these skew stats, so
      // clearly we don't need to take a new sample
      return;
   }
   now = timestamp;
   if (now - vsmp->skew.lastUpdate < 
       vsmp->cell->config.skewSampleMinInterval) {
      // our last sample was too recent and we don't want to overcount
      // skew samples for this vsmp, so just bail
      SP_UnlockIRQ(lock, prevArrayIRQL);
      return;
   } else {
      vsmp->skew.lastUpdate = now;
   }
   
   // update stats
   vsmp->skew.stats.samples++;

   // update intraSkew counters, needed for both strict and relaxed
   skewOut = CpuSchedSampleIntraSkew(vsmp);
   if (CpuSchedVsmpStrictCosched(vsmp)) {
      // the return value from SampleIntraSkew (above) only applies to the
      // relaxed case, so here we clobber it with the strict notion of skew out
      skewOut = CpuSchedVsmpStrictSkewOut(vsmp);
   }
   SP_UnlockIRQ(lock, prevArrayIRQL);

   // check if want to co-deschedule (due to strict or relaxed skew)
   if (skewOut && CpuSchedVsmpCanDeschedule(vsmp)) {
      SP_IRQL prevIRQL = CpuSchedVsmpCellLock(vsmp);
      
      // check if deschedulable co-running MP
      if ((vsmp->coRunState == CPUSCHED_CO_RUN
           || vsmp->coRunState == CPUSCHED_CO_NONE) &&
          CpuSchedVsmpCanDeschedule(vsmp)) {
         // reschedule pcpus where vcpus running      
         FORALL_VSMP_VCPUS(vsmp, vcpu) {
            if (CpuSchedVcpuRunOrBwait(vcpu)) {
               CpuSched_MarkReschedule(vcpu->pcpu);
            }
         } VSMP_VCPUS_DONE;

         // update stats
         vsmp->skew.stats.resched++;
      }

      CpuSchedVsmpCellUnlock(vsmp, prevIRQL);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVsmpSetState --
 *
 *	Sets current co-scheduling state for "vsmp" to "state".
 *
 * Results:
 *	Modifies "vsmp".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
CpuSchedVsmpSetState(CpuSched_Vsmp *vsmp, CpuSched_CoRunState state)
{
   // sanity checks
   ASSERT(CpuSchedVsmpCellIsLocked(vsmp));

   switch (state) {
   case CPUSCHED_CO_READY:
      // no waiters on transition to CO_READY
      ASSERT(vsmp->nWait == 0);

      break;
   case CPUSCHED_CO_STOP:
      // co-descheduling enabled on transition to CO_STOP
      ASSERT(CpuSchedVsmpCanDeschedule(vsmp));
      break;
   case CPUSCHED_CO_RUN:
      // reset skew counter when entering CO_RUN
      SP_LockIRQ(&vsmp->vcpuArrayLock, SP_IRQL_KERNEL);
      // just decrement intraSkew so it partially pesists across CO_STOPs
      FORALL_VSMP_VCPUS(vsmp, v) {
         if (!CpuSchedVsmpStrictCosched(vsmp) && v->intraSkew > 0) {
            v->intraSkew--;
         } else {
            // in strict cosched, always reset the counter
            v->intraSkew = 0;
         }
         Trace_Event(TRACE_SCHED_INTRASKEW, VcpuWorldID(v), v->pcpu, 0, v->intraSkew);
      } VSMP_VCPUS_DONE;
      SP_UnlockIRQ(&vsmp->vcpuArrayLock, SP_GetPrevIRQ(&vsmp->vcpuArrayLock));
      break;
   default:
      // no checks
      break;
   }

   // state transition
   vsmp->coRunState = state;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuCoRunAbort --
 *
 *	Cancel pending co-schedule of "vcpu".
 *
 * Results:
 *	Modifies "vcpu".
 *
 * Side effects:
 *      Modifies scheduler state.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedVcpuCoRunAbort(CpuSched_Vcpu *vcpu)
{
   CpuSched_Pcpu *schedPcpu = CpuSchedPcpu(vcpu->pcpuHandoff);
   ASSERT(schedPcpu->handoff == vcpu);
   if (schedPcpu->handoff == vcpu) {
      schedPcpu->handoff = NULL;
      vcpu->pcpuHandoff = INVALID_PCPU;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedCoStop --
 *
 *	Initiate co-deschedule of all running and runnable siblings of
 *	"myVcpu", causing them to transition to the READY_COSTOP state.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      Modifies scheduler state.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedCoStop(CpuSched_Vcpu *myVcpu)
{
   CpuSched_Vsmp *vsmp = myVcpu->vsmp;

   // sanity checks
   ASSERT(CpuSchedVsmpCellIsLocked(vsmp));
   ASSERT(vsmp->coRunState == CPUSCHED_CO_STOP);
   ASSERT(CpuSchedVsmpCanDeschedule(vsmp));
   ASSERT(!myVcpu->idle);

   // debugging
   VcpuLogEvent(myVcpu, "costop");

   // XXX Design Issue:
   // Should reconsider the issue of how time in READY_COSTOP is
   // charged to a vcpu.  Previously, when a separate COWAIT state
   // still existed, a vcpu was charged for its wasted time via
   // CpuSchedVcpuChargeWait().  Since COWAIT was absorbed into
   // COSTOP, this is no longer the case.  Initial fairness tests
   // seem to indicate that it doesn't really matter, but this
   // isn't terribly convincing.

   FORALL_VSMP_VCPUS(vsmp, vcpu) {
      if (vcpu != myVcpu) {
         switch (vcpu->runState) {
         case CPUSCHED_RUN:
            // reschedule
            CpuSched_MarkReschedule(vcpu->pcpu);
            break;

         case CPUSCHED_READY:
            // change state, remove from queue
            CpuSchedQueueRemove(vcpu);
            CpuSchedVcpuSetRunState(vcpu, CPUSCHED_READY_COSTOP);
            break;

         case CPUSCHED_READY_CORUN:
            // abort coschedule, keep off queue
            CpuSchedVcpuCoRunAbort(vcpu);
            CpuSchedVcpuSetRunState(vcpu, CPUSCHED_READY_COSTOP);
            break;

         case CPUSCHED_READY_COSTOP:
         case CPUSCHED_WAIT:
         case CPUSCHED_BUSY_WAIT:
         case CPUSCHED_ZOMBIE:
            // nothing to do
            break;

         case CPUSCHED_NEW:
         default:
            NOT_REACHED();
         }
      }
   } VSMP_VCPUS_DONE;

   // debugging
   if (CPUSCHED_DEBUG_COSTOP) {
      CpuSchedVsmpCoStopSanityCheck(vsmp);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedCoStopAbort --
 *
 *	Cancel co-deschedule of all running and runnable siblings of
 *	"myVcpu", causing them to transition to the READY state.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      Modifies scheduler state.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedCoStopAbort(CpuSched_Vcpu *myVcpu)
{
   CpuSched_Vsmp *vsmp = myVcpu->vsmp;

   // debugging
   VcpuLogEvent(myVcpu, "stop-abort");

   // sanity check
   ASSERT(vsmp->coRunState == CPUSCHED_CO_RUN);

   FORALL_VSMP_VCPUS(vsmp, vcpu) {
      if (vcpu != myVcpu) {
         switch (vcpu->runState) {
         case CPUSCHED_READY_COSTOP:
            // make ready, put back on queue
            CpuSchedVcpuMakeReady(vcpu);
            break;

         case CPUSCHED_READY_CORUN:
            // abort coschedule, put back on queue
            CpuSchedVcpuCoRunAbort(vcpu);
            CpuSchedVcpuMakeReady(vcpu);
            break;

         case CPUSCHED_RUN:
         case CPUSCHED_READY:
         case CPUSCHED_WAIT:
         case CPUSCHED_BUSY_WAIT:
         case CPUSCHED_ZOMBIE:
            // nothing to do
            break;

         case CPUSCHED_NEW:
         default:
            NOT_REACHED();
         }
      }
   } VSMP_VCPUS_DONE;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuRequeueSiblings --
 *
 *	Update all siblings of "vcpu" to account for changes to
 *	common virtual time.  May move sibling vcpus in the READY
 *	state to different run queues.
 *
 * Results:
 *	Modifies "vcpu".
 *
 * Side effects:
 *      Modifies scheduler state.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
CpuSchedVcpuRequeueSiblings(CpuSched_Vcpu *vcpu)
{
   CpuSched_Vsmp *vsmp = vcpu->vsmp;
   FORALL_VSMP_VCPUS(vsmp, v) {
      if (v != vcpu) {
         // remove and reinsert to ensure proper queue and sort order
         if (v->runState == CPUSCHED_READY) {
            CpuSchedVcpuRequeue(v);
         }
      }
   } VSMP_VCPUS_DONE;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuChargeWait --
 *
 *	Charge "vcpu" after it finished blocking for "vtElapsed"
 *	units of global virtual time.  Advances virtual time for
 *	"vcpu" to current global virtual time, and credits "vcpu"
 *	to compensate for this lost virtual time.
 *
 * Results:
 *	Modifies "vcpu".
 *
 * Side effects:
 *      Modifies scheduler state.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedVcpuChargeWait(CpuSched_Vcpu *vcpu, CpuSchedVtime vtElapsed)
{
   CpuSched_Vsmp *vsmp = vcpu->vsmp;
   CpuSchedVtime vt;

   // sanity check
   ASSERT(vtElapsed >= 0);

   // compute vtime "lost" while waiting, pro-rated for single vcpu
   vt = CpuSchedVsmpVtimePerVcpu(vsmp, vtElapsed);

   // update vtime, credit
   vsmp->vtime.main += vt;

   // requeue siblings, if any
   if (CpuSchedIsMP(vsmp)) {
      CpuSchedVcpuRequeueSiblings(vcpu);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVsmpCoStart --
 *
 *	Attempt to transition "vsmp" out of CO_STOP state.
 *	If possible, transitions "vsmp" to CO_RUN or CO_READY state,
 *	and transitions all vcpus in READY_COSTOP to READY state.
 *	When a vcpu is transitioned to the READY state, a reschedule
 *	is requested on its associated pcpu, unless it matches
 *	"pcpuNoResched".  Caller must hold scheduler cell lock
 *	for "vsmp".
 *
 * Results:
 *	May modify "vsmp".
 *
 * Side effects: 
 *      Modifies scheduler state.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedVsmpCoStart(CpuSched_Vsmp *vsmp, PCPU pcpuNoResched)
{
   // sanity check
   ASSERT(CpuSchedVsmpCellIsLocked(vsmp));

   // nothing to do if vsmp not stopped
   if (vsmp->coRunState != CPUSCHED_CO_STOP) {
      return;
   }

   // remain stopped if non-idle waiters
   if (vsmp->nWait != vsmp->nIdle) {
      return;
   }

   // transition vsmp to CO_RUN or CO_READY
   if (vsmp->nRun + vsmp->nIdle > 0) {
      CpuSchedVsmpSetState(vsmp, CPUSCHED_CO_RUN);
   } else {
      ASSERT(vsmp->nWait == 0);
      CpuSchedVsmpSetState(vsmp, CPUSCHED_CO_READY);      
   }

   // transition any stopped vcpus to ready
   FORALL_VSMP_VCPUS(vsmp, vcpu) {
      if (vcpu->runState == CPUSCHED_READY_COSTOP) {
         // transition vcpu to READY with optional reschedule
         if (vcpu->pcpu == pcpuNoResched) {
            CpuSchedVcpuMakeReadyNoResched(vcpu);
         } else {
            CpuSchedVcpuMakeReady(vcpu);
         }
      }
   } VSMP_VCPUS_DONE;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuWakeupMigrateIdle --
 *
 *	Attempt to migrate waiting "vcpu" to an idle package.
 *	Caller must hold scheduler cell lock for "vcpu".
 *
 * Results:
 *	Modifies "vcpu".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedVcpuWakeupMigrateIdle(CpuSched_Vcpu *vcpu)
{
   CpuSched_Vsmp *vsmp = vcpu->vsmp;
   CpuMask avoidPcpus;
   uint32 rnd, i;

   // sanity checks
   ASSERT(CpuSchedVsmpCellIsLocked(vsmp));
   ASSERT(vcpu->runState == CPUSCHED_WAIT);

   // done if already on idle package
   if (CpuSchedPackageIsIdle(vcpu->pcpu)) {
      return;
   }

   // avoid migrating where our vcpus already running or queued,
   // otherwise co-wakeups might place all vcpus on same package
   avoidPcpus = PcpuMask(vcpu->pcpu, TRUE);
   FORALL_VSMP_VCPUS(vsmp, v) {
      if (v != vcpu) {
         if (CpuSchedVcpuRunOrBwait(v) || CpuSchedVcpuIsRunnable(v)) {
            avoidPcpus |= PcpuMask(v->pcpu, TRUE);
         }
      }
   } VSMP_VCPUS_DONE;

   // search for permissible idle package
   // start at random index to avoid bias
   rnd = CpuSchedRandom();
   for (i = 0; i < vsmp->cell->nPCPUs; i++) {
      uint32 index = (rnd + i) % vsmp->cell->nPCPUs;
      PCPU p = vsmp->cell->pcpu[index];
      if (CpuSchedVcpuAffinityPermitsPcpu(vcpu, p, avoidPcpus) &&
          CpuSchedPackageIsIdle(p)) {
         // found one: migrate to package on transition to ready
         // randomize LCPU selection to avoid bias on HT systems
         vcpu->pcpu = p;
         if (SMP_HTEnabled() && (rnd & 1)) {
            vcpu->pcpu = CpuSchedPartnerPcpu(p)->id;
         }
         vcpu->stats.wakeupMigrateIdle++;
         return;
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuWakeup --
 *
 *	Wakeup "vcpu" from wait state.  If appropriate, initiates
 *	wakeups for all siblings of "vcpu".  If the vsmp associated
 *	with "vcpu" is runnable, transitions "vcpu" to READY state,
 *	otherwise transitions "vcpu" to the READY_COSTOP state.
 *	Caller must hold scheduler cell lock for "vcpu".
 *
 * Results:
 *	Modifies "vcpu".
 *
 * Side effects:
 *      Modifies scheduler state.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedVcpuWakeup(CpuSched_Vcpu *vcpu)
{
   CpuSched_Vsmp *vsmp = vcpu->vsmp;

   // sanity checks
   ASSERT(CpuSchedVsmpCellIsLocked(vsmp));
   ASSERT(CpuSchedVcpuIsWaiting(vcpu));
   ASSERT(vsmp->nWait > 0);

   // debugging
   VcpuLogEvent(vcpu, "wakeup");

   // update global time
   CpuSchedCellUpdateTime(vsmp->cell);

   // stop vmm action notifications, if enabled
   if (vcpu->actionWakeupMask != 0) {
      SP_IRQL prevIRQL = SP_LockIRQ(&vcpu->actionWakeupLock, SP_IRQL_KERNEL);
      vcpu->actionWakeupMask = 0;
      CpuSchedVcpuActionNotifyRequest(vcpu, FALSE);
      SP_UnlockIRQ(&vcpu->actionWakeupLock, prevIRQL);
   }

   // reset wait state
   CpuSchedVcpuSetWaitState(vcpu, CPUSCHED_WAIT_NONE, CPUSCHED_EVENT_NONE);

   // special case: busy-wait
   if (vcpu->runState == CPUSCHED_BUSY_WAIT) {
      // will transition to RUN on next reschedule
      VcpuLogEvent(vcpu, "wake-bwait");
      CpuSched_MarkReschedule(vcpu->pcpu);
      return;
   }

   // simple case: uniprocessor
   if (!CpuSchedIsMP(vsmp)) {
      // wakeup lone vcpu, make ready
      if (CONFIG_OPTION(CPU_WAKEUP_MIGRATE_IDLE)) {
         CpuSchedVcpuWakeupMigrateIdle(vcpu);
      }
      CpuSchedVcpuMakeReady(vcpu);
      ASSERT(vsmp->nWait == 0);
      return;
   }

   // general case: multiprocessor
   switch (vsmp->coRunState) {
   case CPUSCHED_CO_RUN:
      // make ready
      if (CONFIG_OPTION(CPU_WAKEUP_MIGRATE_IDLE)) {
         CpuSchedVcpuWakeupMigrateIdle(vcpu);
      }
      CpuSchedVcpuMakeReady(vcpu);
      break;

   case CPUSCHED_CO_STOP:
      // ready but co-descheduled, keep off queue
      CpuSchedVcpuSetRunState(vcpu, CPUSCHED_READY_COSTOP);

      // try to restart vsmp, if possible
      CpuSchedVsmpCoStart(vsmp, INVALID_PCPU);
      if (CPUSCHED_DEBUG_COSTOP) {
         CpuSchedVsmpCoStopSanityCheck(vsmp);
      }
      break;

   case CPUSCHED_CO_READY:
   default:
      NOT_REACHED();
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedUpdateVtimeLimit --
 *
 *	Updates credit limit state (used for max rate enforcement)
 *	associated with "vsmp" to reflect the consumption of "charge"
 *	cycles.
 *
 * Results:
 *	Modifies "vsmp".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
CpuSchedUpdateVtimeLimit(CpuSched_Vsmp *vsmp, Timer_Cycles charge)
{
   if (CpuSchedEnforceMax(&vsmp->alloc)) {
      vsmp->vtimeLimit += CpuSchedTCToVtime(vsmp->strideLimit, charge);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedHTQuarantineUpdate --
 *
 *      If this vcpu has run for at least CPU_MACHINE_CLEAR_THRESH ms
 *      since we last checked, update its count of "machine_clear_any" events.
 *      If any vcpu in this vsmp has an aged event count exceeding the
 *      specified threshold, "quarantine" the vsmp so that its vcpus are not
 *      allowed to share packages with anybody. If no vcpu in the vsmp
 *      exceeds the threshold, the vsmp may be de-quarantined.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May flip the htQuarantine setting of the vsmp.
 *
 *-----------------------------------------------------------------------------
 */
static void
CpuSchedHTQuarantineUpdateInt(CpuSched_Vcpu *vcpu)
{
   uint64 clearsNow, clearsDiff;
   struct VmkperfEventInfo *event;
   CpuSched_HTEventCount *eventCount;
   CpuSched_Cell *cell;
   CpuSched_Vsmp *vsmp;
   Bool needQuarantine;
   World_Handle *world;
   // slow decays 5% each time, fast by 33% each time
   static const uint32 slowDenom = 20, slowNum = 19;
   static const uint32 fastDenom = 3, fastNum = 2;

   cell = vcpu->vsmp->cell;
   ASSERT(CpuSchedCellIsLocked(cell));
   
   eventCount = &vcpu->htEvents;

   ASSERT(SMP_HTEnabled());
   ASSERT(cpuType == CPUTYPE_INTEL_PENTIUM4);

   // local aliases
   event = cpuSchedConst.machineClearEvent;
   vsmp = vcpu->vsmp;
   world = World_VcpuToWorld(vcpu);

   // get most recent performance counter info
   Vmkperf_WorldSave(world);

   // start a new counting interval
   eventCount->nextUpdate = eventCount->nextUpdate + cell->config.htEventsUpdateCycles;

   // obtain current event counts for this vcpu
   clearsNow = Vmkperf_GetWorldEventCount(world, event);
   
   if (clearsNow < eventCount->prevCount) {
      // counter has gone backwards (probably reset), so update and bail out
      eventCount->prevCount = clearsNow;
      return;
   }
   
   clearsDiff = clearsNow - eventCount->prevCount;
   eventCount->prevCount = clearsNow;

   // update moving average info for this vcpu
   ASSERT(slowDenom > slowNum);
   eventCount->agedCountSlow = ((eventCount->agedCountSlow * slowNum)
                                + (clearsDiff * (slowDenom - slowNum))) / slowDenom;
   ASSERT(fastDenom > fastNum);
   eventCount->agedCountFast = ((eventCount->agedCountFast * fastNum)
                                + (clearsDiff * (fastDenom - fastNum))) / fastDenom;

   // if any vcpu exceeds our threshold, quarantine the whole vsmp
   needQuarantine = FALSE;
   FORALL_VSMP_VCPUS(vsmp, v) {
      uint32 clearsPerMil, clearsPerMilFast, clearsPerMilSlow;

      // take the MAX of the slow and fast moving averages
      clearsPerMilFast  = v->htEvents.agedCountFast
         / (cell->config.htEventsUpdateCycles / 1000000ul);
      clearsPerMilSlow = v->htEvents.agedCountSlow
         / (cell->config.htEventsUpdateCycles / 1000000ul);
      clearsPerMil = MAX(clearsPerMilFast, clearsPerMilSlow);

      VCPULOG(2, v, "clearsDiff=%Lu, clearsNow=%Lu, slow=%u, fast=%u",
              clearsDiff, clearsNow, clearsPerMilSlow, clearsPerMilFast);
      
      if (clearsPerMil > CONFIG_OPTION(CPU_MACHINE_CLEAR_THRESH)) {
         VCPULOG(1, v,
                 "should quarantine vcpu due to high machine clear count: "
                 "%u per million cycles",
                 clearsPerMil);
         needQuarantine = TRUE;
      } else {
         VCPULOG(2, v, "no quarantine needed: countPerMil = %u", clearsPerMil);
      }
   } VSMP_VCPUS_DONE;

   if (!vsmp->htQuarantine && needQuarantine) {
      vsmp->htQuarantine = TRUE;
      vsmp->numQuarantines++;
      CpuSchedPcpuPreemptionInvalidate(CpuSchedPartnerPcpu(vcpu->pcpu));
   } else if (vsmp->htQuarantine && !needQuarantine) {
      vsmp->htQuarantine = FALSE;
      CpuSchedPcpuPreemptionInvalidate(CpuSchedPartnerPcpu(vcpu->pcpu));
   }
   
   if (vsmp->htQuarantine) {
      vsmp->quarantinePeriods++;
   }
}

// inline "fast path" for above function,
// handles case where we don't really need to update quaratining
static INLINE void
CpuSchedHTQuarantineUpdate(CpuSched_Vcpu *vcpu)
{
   // update after every CPUSCHED_HT_EVENT_PERIOD ms of used time
   if (LIKELY(!cpuSched.htQuarantineActive ||
              vcpu->idle ||
              vcpu->htEvents.nextUpdate > vcpu->chargeCyclesTotal)) {
      return;
   } else {
      CpuSchedHTQuarantineUpdateInt(vcpu);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuChargeCyclesTotalSet --
 *
 *	Sets total cpu consumption for "vcpu" to "n" using
 *      versioned-atomic primitives.  Caller must hold scheduler
 *	cell lock associated with "vcpu".
 *
 * Results:
 *	Sets total cpu consumption for "vcpu" to "n".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
CpuSchedVcpuChargeCyclesTotalSet(CpuSched_Vcpu *vcpu, Timer_Cycles n)
{
   // cell lock ensures single-writer for versioned atomic update
   ASSERT(CpuSchedVsmpCellIsLocked(vcpu->vsmp));
   CPUSCHED_VERSIONED_ATOMIC_UPDATE_BEGIN(&vcpu->chargeCyclesVersion);
   vcpu->chargeCyclesTotal = n;
   CPUSCHED_VERSIONED_ATOMIC_UPDATE_END(&vcpu->chargeCyclesVersion);
}

// trivial wrapper
static INLINE void
CpuSchedVcpuChargeCyclesTotalAdd(CpuSched_Vcpu *vcpu, Timer_Cycles n)
{
   CpuSchedVcpuChargeCyclesTotalSet(vcpu, vcpu->chargeCyclesTotal + n);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuChargeCyclesTotalGet --
 *
 *	Returns consistent snapshot of total cpu consumption for "vcpu"
 *	using versioned-atomic primitives.  Caller is *not* required
 *	to hold any scheduler locks.
 *
 * Results:
 *	Returns consistent snapshot of total cpu consumption for "vcpu".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Timer_Cycles
CpuSchedVcpuChargeCyclesTotalGet(const CpuSched_Vcpu *vcpu)
{
   // obtain consistent snapshot 
   Timer_Cycles n;
   CPUSCHED_VERSIONED_ATOMIC_READ_BEGIN(&vcpu->chargeCyclesVersion);
   n = vcpu->chargeCyclesTotal;
   CPUSCHED_VERSIONED_ATOMIC_READ_END(&vcpu->chargeCyclesVersion);
   return(n);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuChargeStartSet --
 *
 *	Sets start of cpu charging period for "vcpu" to "n" using
 *      versioned-atomic primitives.  Caller must hold scheduler
 *	cell lock associated with "vcpu".
 *
 * Results:
 *	Sets start of cpu charging period for "vcpu" to "n".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
CpuSchedVcpuChargeStartSet(CpuSched_Vcpu *vcpu, Timer_AbsCycles n)
{
   // cell lock ensures single-writer for versioned atomic update
   ASSERT(CpuSchedVsmpCellIsLocked(vcpu->vsmp));
   CPUSCHED_VERSIONED_ATOMIC_UPDATE_BEGIN(&vcpu->chargeStartVersion);
   vcpu->chargeStart = n;
   CPUSCHED_VERSIONED_ATOMIC_UPDATE_END(&vcpu->chargeStartVersion);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuChargeStartGet --
 *
 *	Returns consistent snapshot of cpu charging period start
 *	for "vcpu" using versioned-atomic primitives.  Caller is
 *	*not* required to hold any scheduler locks.
 *
 * Results:
 *	Returns consistent snapshot of cpu charging period start
 *	for "vcpu".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Timer_AbsCycles
CpuSchedVcpuChargeStartGet(const CpuSched_Vcpu *vcpu)
{
   // obtain consistent snapshot 
   Timer_AbsCycles n;
   CPUSCHED_VERSIONED_ATOMIC_READ_BEGIN(&vcpu->chargeStartVersion);
   n = vcpu->chargeStart;
   CPUSCHED_VERSIONED_ATOMIC_READ_END(&vcpu->chargeStartVersion);
   return(n);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuChargeUsage --
 *
 *	Charges vsmp associated with "vcpu" for its recent cpu
 *	consumption, updating its virtual times.
 *
 * Results:
 *	Modifies "vcpu" and its associated vsmp.
 *
 * Side effects:
 *      Modifies scheduler state.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedVcpuChargeUsage(CpuSched_Vcpu *vcpu)
{
   Timer_Cycles charge, delta, sysCycles, sysCyclesOverlap;
   Timer_AbsCycles now, chargeStart;
   CpuSched_Pcpu *schedPcpu;
   CpuSched_Vsmp *vsmp;
   CpuSchedConfig *config;
   uint32 sysKCycles;

   // convenient abbrevs
   schedPcpu = CpuSchedPcpu(vcpu->pcpu);
   now = schedPcpu->cell->now;
   config = &schedPcpu->cell->config;
   chargeStart = vcpu->chargeStart;
   vsmp = vcpu->vsmp;

   // sanity checks
   ASSERT(CpuSchedVsmpCellIsLocked(vsmp));
   ASSERT(vcpu->pcpu == MY_PCPU);
   ASSERT(chargeStart > 0);
   ASSERT(now >= chargeStart);

   // done if no uncharged elapsed time
   if ((chargeStart == 0) || (chargeStart >= now)) {
      return;
   }

   // compute elapsed run time
   delta = now - chargeStart;
   charge = delta;

   // update charge start time
   CpuSchedVcpuChargeStartSet(vcpu, now);

   // hyperthreaded halt accounting
   if (SMP_HTEnabled() && vmkernelLoaded) {
      Timer_Cycles phaltDelta;
      CpuSched_Pcpu *partnerPcpu; 

      // we need this lock to read and manipulate "pcpu->totalHaltCycles"
      CpuSchedPackageHaltLock(vcpu->pcpu);

      // "double-charge" this vcpu for time when we
      // ran and our partner PCPU was halted
      partnerPcpu = schedPcpu->partner;
      phaltDelta = CpuSchedGetPartnerHaltedDelta(vcpu);
      charge += phaltDelta;

      // the idle world should NOT be charged
      // for time when it was actually halted
      if (CpuSchedVcpuIsIdle(vcpu)) {
         Timer_Cycles localHaltDelta, total;
         CpuSched_Vcpu *idleVcpu;

         ASSERT(vcpu->localHaltStart != -1);
         if (LIKELY(vcpu->localHaltStart != -1)) {
            // read and reset local pcpu's halt info
            ASSERT(vcpu->localHaltStart <= schedPcpu->totalHaltCycles);
            localHaltDelta = schedPcpu->totalHaltCycles - vcpu->localHaltStart;
            vcpu->localHaltStart = schedPcpu->totalHaltCycles;
            
            // charge both the pcpu and idle vcpu, but don't send them negative
            schedPcpu->idleCycles -= MIN(localHaltDelta, schedPcpu->idleCycles);
            idleVcpu = CpuSchedGetIdleVcpu(schedPcpu->id);
            total = idleVcpu->chargeCyclesTotal;
            total -= MIN(localHaltDelta, idleVcpu->chargeCyclesTotal);
            CpuSchedVcpuChargeCyclesTotalSet(idleVcpu, total);
         }
      } else {
         schedPcpu->usedCycles += phaltDelta;
      }

      // reset phalt start
      vcpu->phaltStart = partnerPcpu->totalHaltCycles;

      CpuSchedPackageHaltUnlock(vcpu->pcpu);
   }

   // sanity check
   if (UNLIKELY(delta > cpuSchedConst.cyclesPerMinute)) {
      uint32 deltaUsec;
      uint64 deltaSec;
      Timer_TCToSec(delta, &deltaSec, &deltaUsec);
      VcpuWarn(vcpu, "excessive time: delta=%Lu, deltaSec=%Lu.%06u", 
               delta, deltaSec, deltaUsec);
   }

   // account for system cycles attributed to vcpu,
   //   use atomic ops since may be updated concurrently
   sysKCycles = Atomic_Read(&vcpu->sysKCycles);
   sysCycles = ((Timer_Cycles) sysKCycles) << 10;

   if (UNLIKELY(sysCycles > config->sysAcctLimitCycles)) {
      if (CONFIG_OPTION(CPU_SCHEDULER_DEBUG)) {
         VcpuWarn(vcpu, "excessive sysCycles: %Lu", sysCycles);
      }
      sysCycles = config->sysAcctLimitCycles;
   }

   if (sysKCycles > 0) {
      Atomic_Sub(&vcpu->sysKCycles, sysKCycles);
      vcpu->sysCyclesTotal += sysCycles;
      charge += sysCycles;
   }

   // account for system cycles that overlapped vcpu execution,
   //   no need for atomic ops since only updated locally
   //   automatically handles hyperthread accounting for syscycles too
   if (UNLIKELY(vcpu->sysCyclesOverlap > config->sysAcctLimitCycles)) {
      if (CONFIG_OPTION(CPU_SCHEDULER_DEBUG)) {
         VcpuWarn(vcpu, "excessive sysOverlap: %Lu", vcpu->sysCyclesOverlap);
      }
      vcpu->sysCyclesOverlap = config->sysAcctLimitCycles;
   }

   // offset current charge with unused system overlap cycles
   if (charge >= vcpu->sysCyclesOverlap) {
      charge -= vcpu->sysCyclesOverlap;
      sysCyclesOverlap = vcpu->sysCyclesOverlap;
      vcpu->sysOverlapTotal += vcpu->sysCyclesOverlap;
      vcpu->sysCyclesOverlap = 0;
   } else {
      vcpu->sysOverlapTotal += charge;
      vcpu->sysCyclesOverlap -= charge;
      sysCyclesOverlap = charge;
      charge = 0;
   }

   if (vcpu->runState == CPUSCHED_BUSY_WAIT) {
      // charge vcpu only for directly-attributed system time
      CpuSchedVcpuChargeCyclesTotalAdd(vcpu, sysCycles);

      // charge idle world for remaining time
      if (charge > sysCycles) {
         // update cpu consumption
         CpuSched_Vcpu *idleVcpu = CpuSchedGetIdleVcpu(MY_PCPU);
         CpuSchedVcpuChargeCyclesTotalAdd(idleVcpu, charge - sysCycles);
      }
   } else {
      // update accounting
      CpuSchedVcpuChargeCyclesTotalAdd(vcpu, charge);
   }

   // accumulate pcpu usage
   if (CpuSchedVcpuIsIdle(vcpu)) {
      schedPcpu->idleCycles += delta;
   } else {
      schedPcpu->usedCycles += delta;
   }
   schedPcpu->usedCycles += sysCyclesOverlap;
   schedPcpu->sysCyclesOverlap += sysCyclesOverlap;

   // sanity check
   if (UNLIKELY(charge > cpuSchedConst.cyclesPerMinute)) {
      uint32 chargeUsec;
      uint64 chargeSec;
      Timer_TCToSec(charge, &chargeSec, &chargeUsec);
      VcpuWarn(vcpu, "excessive time: charge=%Lu, chargeSec=%Lu.%06u", 
               charge, chargeSec, chargeUsec);
   }

   // debugging
   CpuSchedLogEvent("charge", charge);

   // adjust charge for non-idle worlds
   if ((charge > 0) && !CpuSchedVcpuIsIdle(vcpu)) {
      CpuSchedVtime vtCharge, vtQuantum, vtLimit;

      // charge by updating vtimes
      vtCharge = CpuSchedTCToVtime(vsmp->vtime.stride, charge);
      vsmp->vtime.main += vtCharge;
      vsmp->vtime.extra += vtCharge;
      CpuSchedVsmpGroupCharge(vsmp, charge);
      CpuSchedUpdateVtimeLimit(vsmp, charge);
      
      // compensate for "extra" consumption;
      //    limit main vtime advance to single quantum beyond global vtime
      // OPT: could store precomputed "vtQuantum" value
      vtQuantum = CpuSchedTCToVtime(vsmp->vtime.nStride, config->quantumCycles);
      vtLimit = vsmp->cell->vtime + vtQuantum;
      if (vsmp->vtime.main > vtLimit) {
         // XXX bonus computation likely wrong (was locally negative credit)
         // XXX maybe want f(vtime.main - vtime.extra)?
         CpuSchedVtime vtBonus = vsmp->vtime.main - vtLimit;
         Timer_Cycles bonus = CpuSchedVtimeToTC(vsmp->vtime.stride, vtBonus);
         vsmp->vtime.main = vtLimit;
         vsmp->stats.bonusCyclesTotal += bonus;
      }
         
      // requeue siblings, if any
      if (CpuSchedIsMP(vsmp)) {
         CpuSchedVcpuRequeueSiblings(vcpu);
      } 
   }

   CpuSchedHTQuarantineUpdate(vcpu);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuQuantumStart --
 *
 *	Updates per-quantum scheduling state associated with "vcpu".
 *	May update quantum expiration for vsmp associated with "vcpu".
 *
 * Results:
 *	Modifies "vcpu", vsmp associated with "vcpu".
 *
 * Side effects:
 *      Modifies scheduler state.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedVcpuQuantumStart(CpuSched_Vcpu *vcpu, CpuSched_Vcpu *yieldingVcpu)
{
   CpuSched_Vsmp *vsmp = vcpu->vsmp;
   CpuSched_Cell *cell = vsmp->cell;

   CpuSchedVcpuChargeStartSet(vcpu, cell->now);

   if (SMP_HTEnabled()) {
      CpuSched_Pcpu *partnerPcpu = CpuSchedPartnerPcpu(vcpu->pcpu);

      // need this lock to read totalHaltCycles
      CpuSchedPackageHaltLock(vcpu->pcpu);

      vcpu->phaltStart = partnerPcpu->totalHaltCycles;   
      if (CpuSchedVcpuIsIdle(vcpu)) {
         CpuSched_Pcpu *schedPcpu = CpuSchedPcpu(vcpu->pcpu);
         vcpu->localHaltStart = schedPcpu->totalHaltCycles;
      }

      CpuSchedPackageHaltUnlock(vcpu->pcpu);
   }

   // start new vsmp quantum if reset or expired
   if ((vsmp->quantumExpire == 0) || (cell->now > vsmp->quantumExpire)) {
      if (vcpu->idle) {
         vsmp->quantumExpire = cell->now + cell->config.idleQuantumCycles;
      } else if (yieldingVcpu != NULL) {
         vsmp->quantumExpire = yieldingVcpu->quantumExpire;
      } else {
         vsmp->quantumExpire = cell->now + cell->config.quantumCycles;
      }
   }

   // vcpu uses joint vsmp quantum
   vcpu->quantumExpire = vsmp->quantumExpire;
   if (TRACE_MODULE_ACTIVE) {
      int32 qremain = 0;
      if (vsmp->quantumExpire > cell->now) {
         qremain = Timer_TCToMS(vsmp->quantumExpire - cell->now);
      }
      Trace_Event(TRACE_SCHED_QUANTUM_REMAIN, VcpuWorldID(vcpu), vcpu->pcpu, 0, qremain);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * CpuSchedQueueRequeue --
 *
 *	Examines each vcpu enqueued on scheduler queue "q"
 *	associated with "pcpu", and moves any vcpus that
 *	should no longer belong there to the proper queue
 *	associated with "pcpu".
 *
 * Results:
 *	May modify "pcpu", "q".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedQueueRequeue(CpuSched_Pcpu *pcpu, CpuSchedQueue *q)
{
   List_Links *elt;

   elt = List_First(&q->queue);
   while (!List_IsAtEnd(elt, &q->queue)) {
      List_Links *next = List_Next(elt);
      CpuSched_Vcpu *vcpu = World_CpuSchedVcpu((World_Handle *) elt);
      CpuSchedQueue *qNew = CpuSchedQueueSelect(pcpu, vcpu);
      if (qNew != q) {
         VcpuLogEvent(vcpu, "qrequeue");
         CpuSchedQueueRemove(vcpu);
         CpuSchedQueueAddInt(qNew, vcpu);
      }
      elt = next;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedPcpuUpdateQueues --
 *
 *	Moves vcpus that are no longer ahead of their entitled
 *	allocation from "extra" queue to "main" queue on "pcpu",
 *	and moves vcpus that are no longer max-limited from the
 *	"limbo" queue to the appropriate "main" or "extra" queue
 *	on "pcpu".
 *
 * Results:
 *	Modifies "pcpu".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
CpuSchedPcpuUpdateQueues(CpuSched_Pcpu *pcpu)
{
   // main  -> extra: handled during enqueue (or explicit requeue)
   // extra -> main:  simple brute-force approach for now
   CpuSchedQueueRequeue(pcpu, &pcpu->queueExtra);

   // other -> limbo: handled during enqueue (or explicit requeue)
   // limbo -> other: simple brute-force approach for now
   CpuSchedQueueRequeue(pcpu, &pcpu->queueLimbo);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuCanBusyWait --
 *
 *	Returns TRUE if "vcpu" is in the WAIT state and able to busy
 *	wait.  Busy-waiting is prohibited for some vcpu types,
 *	and may be completely disabled explicitly via the hidden config
 *	option "CPU_IDLE_SWITCH_OPT".  It may also be disabled only for
 *	the service console via the hidden config option
 *	"CPU_IDLE_CONSOLE_OPT".
 *
 * Results:
 *	Returns TRUE if "vcpu" is able to busy wait.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
CpuSchedVcpuCanBusyWait(const CpuSched_Vcpu *vcpu)
{
   // busy-wait only if enabled and vcpu is waiting in vmkernel
   World_Handle *world = World_VcpuToWorld(vcpu);
   if (CONFIG_OPTION(CPU_IDLE_SWITCH_OPT) &&
       (vcpu->runState == CPUSCHED_WAIT) &&
       world->preemptionDisabled &&
       (World_IsVMMWorld(world)
        || World_IsTESTWorld(world)
        || World_IsUSERWorld(world)
        || (World_IsHOSTWorld(world) && CONFIG_OPTION(CPU_IDLE_CONSOLE_OPT)))) {
      return(TRUE);
   } else {
      return(FALSE);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuBusyWaitDone --
 *
 *	Terminates busy-wait, transitioning "vcpu" to the RUN state
 *	if "vcpu" is no longer waiting, or to the WAIT state if "vcpu"
 *	is still waiting.
 *
 * Results:
 *	Modifies "vcpu".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedVcpuBusyWaitDone(CpuSched_Vcpu *vcpu)
{
   // sanity checks
   ASSERT(CpuSchedVsmpCellIsLocked(vcpu->vsmp));
   ASSERT(vcpu->runState == CPUSCHED_BUSY_WAIT);

   // transition non-busy-waiting state
   if (vcpu->runState == CPUSCHED_BUSY_WAIT) {
      CpuSched_Vsmp *vsmp = vcpu->vsmp;

      if (vcpu->waitState == CPUSCHED_WAIT_NONE) {
         // no longer waiting, transition vcpu to RUN
         VcpuLogEvent(vcpu, "bwait-to-run");

         if (Vmkperf_TrackPerWorld()) {
            // start counting events for this world again
            Vmkperf_WorldRestore(World_VcpuToWorld(vcpu));
         }
         
         CpuSchedVcpuSetRunState(vcpu, CPUSCHED_RUN);

         // try to restart vsmp, if appropriate
         if (CpuSchedIsMP(vsmp)) {
            CpuSchedVsmpCoStart(vsmp, vcpu->pcpu);
            if (CPUSCHED_DEBUG_COSTOP) {
               CpuSchedVsmpCoStopSanityCheck(vsmp);
            }
         }
      } else {
         // still waiting, transition vcpu to WAIT
         VcpuLogEvent(vcpu, "bwait-to-wait");
         CpuSchedVcpuSetRunState(vcpu, CPUSCHED_WAIT);
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuDescheduleMP --
 *
 *	Deschedules "prev" in preparation for running "next" on
 *	the local processor.  Requires that "prev" is associated 
 *	with a multiprocessor VM.  If this vcpu is required for coscheduling,
 *      this will costop a vsmp in CO_RUN unless continueCoRun is TRUE.
 *      Caller must hold scheduler cell lock for "prev".
 *	
 * Results:
 *	None.
 *
 * Side effects:
 *      Modifies scheduler state.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedVcpuDescheduleMP(CpuSched_Vcpu *prev,
                         const CpuSched_Vcpu *next,
                         Bool continueCoRun)
{
   const CpuSched_Vsmp *nextVsmp = next->vsmp;
   CpuSched_Vsmp *prevVsmp = prev->vsmp;

   // sanity checks
   ASSERT(CpuSchedVsmpCellIsLocked(prevVsmp));
   ASSERT(CpuSchedVsmpCellIsLocked(nextVsmp));
   ASSERT(CpuSchedIsMP(prevVsmp));

   switch (prev->runState) {
   case CPUSCHED_RUN: 
      switch (prevVsmp->coRunState) {
      case CPUSCHED_CO_RUN:
         // if this vcpu requires coscheduling (due to strict settings or
         // excessive intra-skew), we should co-stop unless switching to
         // sibling, or all siblings halted or we've been told not to co-stop
         // (due to a currentRunnerMove)
         if (CpuSchedVcpuNeedsCosched(prev) &&
             !continueCoRun &&
             (nextVsmp != prevVsmp) &&
             (prevVsmp->nIdle < prevVsmp->vcpus.len - 1) &&
             CpuSchedVsmpCanDeschedule(prevVsmp)) {
            // transition vsmp to CO_STOP
            CpuSchedVsmpSetState(prevVsmp, CPUSCHED_CO_STOP);
            CpuSchedCoStop(prev);

            // update state, keep off queue
            CpuSchedVcpuSetRunState(prev, CPUSCHED_READY_COSTOP);
         } else {
            // make ready, don't reschedule
            CpuSchedVcpuMakeReadyNoResched(prev);
         }
         break;

      case CPUSCHED_CO_STOP:
         // update state, keep off queue
         CpuSchedVcpuSetRunState(prev, CPUSCHED_READY_COSTOP);
         break;

      case CPUSCHED_CO_READY:
      default:
         NOT_REACHED();
      }
      break;

   case CPUSCHED_WAIT:
      // keep off queue
      switch (prevVsmp->coRunState) {
      case CPUSCHED_CO_RUN:
         if ((nextVsmp != prevVsmp) &&
             (prevVsmp->nRun == 0) &&
             (prevVsmp->nIdle == 0) &&
             CpuSchedVsmpCanDeschedule(prevVsmp)) {
            // no vcpus running, transition vsmp to CO_STOP
            CpuSchedVsmpSetState(prevVsmp, CPUSCHED_CO_STOP);
            CpuSchedCoStop(prev);
         }
         break;
         
      case CPUSCHED_CO_STOP:
         // vcpu entered no-desched WAIT w/ CO_STOP pending
         if (!CpuSchedVsmpCanDeschedule(prevVsmp)) {
            // transition vsmp back to CO_RUN state
            CpuSchedVsmpSetState(prevVsmp, CPUSCHED_CO_RUN);
            CpuSchedCoStopAbort(prev);
         }
         break;
            
      case CPUSCHED_CO_READY:
      default:
         NOT_REACHED();
      }
      break;

   case CPUSCHED_ZOMBIE:
      // nothing to do
      break;

   case CPUSCHED_BUSY_WAIT:
   case CPUSCHED_READY:
   case CPUSCHED_READY_COSTOP:
   case CPUSCHED_READY_CORUN:
   case CPUSCHED_NEW:
   default:
      NOT_REACHED();
   }

   // OPT: (CO_RUN -> CO_STOP -> CO_READY) => (CO_RUN -> CO_READY)
   if (prevVsmp->coRunState == CPUSCHED_CO_STOP) {
      PCPU skipReschedPcpu = prev->pcpu;
      
      // try to restart vsmp, if possible
      if (CpuSchedVcpuIsIdle(next)) {
         // allow reschedule here if MY_PCPU is going idle
         skipReschedPcpu = INVALID_PCPU;
      }
      CpuSchedVsmpCoStart(prevVsmp, skipReschedPcpu);
   }

   // debugging
   if (CPUSCHED_DEBUG_COSTOP) {
      CpuSchedVsmpCoStopSanityCheck(prevVsmp);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuDescheduleUP --
 *
 *	Deschedules "vcpu".  Requires that "vcpu" is associated with
 *	a uniprocessor VM.  Caller must hold scheduler cell lock
 *	for "vcpu".
 *	
 * Results:
 *	None.
 *
 * Side effects:
 *      Modifies scheduler state.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedVcpuDescheduleUP(CpuSched_Vcpu *vcpu)
{
   // sanity checks
   ASSERT(!CpuSchedIsMP(vcpu->vsmp));
   ASSERT(CpuSchedVsmpCellIsLocked(vcpu->vsmp));   

   switch (vcpu->runState) {
   case CPUSCHED_RUN:
      // make ready, don't reschedule
      CpuSchedVcpuMakeReadyNoResched(vcpu);
      break;
      
   case CPUSCHED_WAIT:
   case CPUSCHED_ZOMBIE:
      // nothing to do, keep off queue
      break;

   case CPUSCHED_BUSY_WAIT:
   case CPUSCHED_READY:
   case CPUSCHED_READY_COSTOP:
   case CPUSCHED_READY_CORUN:
   case CPUSCHED_NEW:
   default:
      NOT_REACHED();
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuDeschedule --
 *
 *	Deschedules "prev" in preparation for running "next" on
 *	the local processor. prev vsmp may be costopped, unless
 *      "continueCoRun" is TRUE.  Caller must hold scheduler
 *      cell lock for "prev".
 *	
 * Results:
 *	None.
 *
 * Side effects:
 *      Modifies scheduler state.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
CpuSchedVcpuDeschedule(CpuSched_Vcpu *prev, 
                       const CpuSched_Vcpu *next,
                       Bool continueCoRun)
{
   // invoke appropriate primitive
   if (CpuSchedIsMP(prev->vsmp)) {
      CpuSchedVcpuDescheduleMP(prev, 
                               next,
                               continueCoRun);
   } else {
      CpuSchedVcpuDescheduleUP(prev);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuWaitForSwitch --
 *
 *	Spins until "switchInProgress" flag associated with "vcpu"
 *	is clear.  Panics if timeout is reached first.
 *	
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedVcpuWaitForSwitch(const CpuSched_Vcpu *vcpu)
{
   TSCCycles start, elapsed, timeout, deltaWarn, deltaPanic;

   // compute timeout thresholds
   deltaWarn = Timer_MSToTSC(CPUSCHED_SWITCH_WAIT_WARN);
   deltaPanic = Timer_MSToTSC(CPUSCHED_SWITCH_WAIT_PANIC);

   // spin until done or timeout
   start = RDTSC();
   timeout = start + deltaPanic;
   while (vcpu->switchInProgress && (RDTSC() < timeout)) {
      PAUSE();
   }
   elapsed = RDTSC() - start;
   
   // debugging
   if (vmx86_debug) {
      Histogram_Insert(CpuSchedPcpu(MY_PCPU)->switchWaitHisto,
                       Timer_TSCToUS(elapsed));
   }

   // setup deferred warning if exceeded threshold
   // n.b. avoid slow logging here, else likely to make things worse
   if (elapsed > deltaWarn) {
      CpuSched_Pcpu *schedPcpu = CpuSchedPcpu(MY_PCPU);
      schedPcpu->switchWaitWarn = TRUE;
      schedPcpu->switchWaitWorldID = VcpuWorldID(vcpu);
      schedPcpu->switchWaitCycles = elapsed;
   }

   // panic if not yet complete
   if (vcpu->switchInProgress) {
      Panic("CpuSched: VcpuWaitForSwitch: timed out\n");
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedAfterSwitch --
 *
 *	Clears "switch in progress" flag and updates other state
 *	associated with local processor after completing a
 *	world switch.
 *	
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedAfterSwitch(CpuSched_Vcpu *desched)
{
   CpuSched_Pcpu *schedPcpu = CpuSchedPcpu(MY_PCPU);

   // sanity checks
   ASSERT_NO_INTERRUPTS();
   ASSERT(desched != NULL);

   // clear "switch in progress" flag
   desched->switchInProgress = FALSE;

   // issue deferred warning, if any
   if (schedPcpu->switchWaitWarn) {
      VmWarn(schedPcpu->switchWaitWorldID,
             "VcpuWaitForSwitch: %Lu cycles, %Lu msec",
             schedPcpu->switchWaitCycles,
             Timer_TSCToMS(schedPcpu->switchWaitCycles));
      schedPcpu->switchWaitWarn = FALSE;
      schedPcpu->switchWaitWorldID = INVALID_WORLD_ID;
      schedPcpu->switchWaitCycles = 0;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * CpuSchedDoCellMigrate --
 *
 *	Transfers "nextVcpu", the vcpu selected to execute next on the
 *	local processor "myPcpu", and its sibings (if any) to "myCell".
 *	Caller must hold scheduler cell locks for "myCell" and
 *	the cell associated with "nextVcpu".
 *	
 * Results:
 *	Modifies "myCell", "nextVcpu", and its associated cell.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedDoCellMigrate(PCPU myPcpu,
                      CpuSched_Cell *myCell,
                      CpuSched_Vcpu *nextVcpu)
{
   CpuSched_Vsmp *nextVsmp = nextVcpu->vsmp;
   CpuSched_Cell *remoteCell = nextVsmp->cell;
   
   // sanity checks
   ASSERT(CpuSchedCellIsLocked(myCell));
   ASSERT(CpuSchedCellIsLocked(remoteCell));
   ASSERT(myCell != remoteCell);

   // move vsmp between cells
   CpuSched_VsmpArrayRemove(&remoteCell->vsmps, nextVsmp);
   nextVsmp->cell = myCell;
   CpuSched_VsmpArrayAdd(&myCell->vsmps, nextVsmp);
   nextVsmp->stats.cellMigrate++;
   VCPULOG(1, nextVcpu, "inter-cell mig: from=%u, to=%u",
           remoteCell->id, myCell->id);

   // move siblings to local pcpu
   FORALL_VSMP_VCPUS(nextVsmp, migVcpu) {
      // prevent rare race from making time appear non-monotonic
      CpuSched_StateMeter *m = &migVcpu->runStateMeter[migVcpu->runState];
      if (UNLIKELY(m->start > myCell->now)) {
         VCPULOG(0, migVcpu, "adjusted start by %Lu",
                 m->start - myCell->now);
         m->start = myCell->now;
      }
      if (UNLIKELY(m->vtStart > myCell->vtime)) {
         VCPULOG(0, migVcpu, "adjusted vtStart by %Lu",
                 m->vtStart - myCell->vtime);
         m->vtStart = myCell->vtime;
      }

      if (migVcpu != nextVcpu) {
         DEBUG_ONLY(PCPU migPcpu = migVcpu->pcpu);
         ASSERT(!CpuSchedVcpuRunOrBwait(migVcpu));
         ASSERT(migPcpu != myPcpu);
         migVcpu->pcpu = myPcpu;
         if (migVcpu->runState == CPUSCHED_READY) {
            CpuSchedVcpuRequeue(migVcpu);
         }
      }
   } VSMP_VCPUS_DONE;
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedSkewCheck --
 *
 *      Determines if the previous vcpu has skewed out.
 *      If the previous vsmp's skew counter exceeds the currently-configured
 *      threshold, we'll 
 *
 * Results:
 *      May put the previous vsmp into CO_STOP.
 *
 * Side effects:
 *      May put the previous vsmp into CO_STOP.
 *
 *-----------------------------------------------------------------------------
 */
static void
CpuSchedSkewCheck(CpuSched_Vcpu *prev)
{
   CpuSched_Vsmp *prevVsmp = prev->vsmp;

   // no need to stop if we're not running
   if (!prev->runState != CPUSCHED_RUN) {
      return;
   }

   if (!(CpuSchedVsmpSkewedOut(prevVsmp) &&
         CpuSchedVsmpCanDeschedule(prevVsmp))) {
      // we're not skewed
      return;
   }
   
   if ((prevVsmp->coRunState == CPUSCHED_CO_RUN) &&
       // avoid co-descheduling immediately before disabling it
       ((prev->runState != CPUSCHED_WAIT) ||
        (!CpuSchedWaitStateDisablesCoDesched(prev->waitState)))) {
      // deschedule vsmp
      VcpuLogEvent(prev, "skew-out");
      CpuSchedVsmpSetState(prevVsmp, CPUSCHED_CO_STOP);
      CpuSchedCoStop(prev);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedDispatch --
 *
 *	Invokes the scheduler, choosing the next vcpu to run on the
 *	local processor, and co-scheduling other vcpus in the same vsmp
 *	on remote processors when necessary.  Note that the currently-
 *	executing vcpu may be selected to run for another quantum.
 *	Updates global CpuSched time if "updateTime" is set.
 *	Caller must hold scheduler cell lock for local processor.
 *	
 * Results:
 *	None.
 *
 * Side effects:
 *      Modifies scheduler state, including currently-executing vcpu.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedDispatch(SP_IRQL prevIRQL, Bool updateTime)
{
   CpuSched_Vsmp *prevVsmp, *nextVsmp;
   Bool afterBusyWait;
   CpuSched_Vcpu *prev, *next;
   CpuSched_Cell *myCell, *remoteCell;
   CpuSched_Pcpu *schedPcpu;
   CpuSchedChoice choice;
   PCPU myPCPU;
   CpuSched_Vcpu *directedYield;

   // sanity checks: can't reschedule while busy-waiting or marked halted
   //                see BH_Check() for details
   ASSERT(World_CpuSchedRunState(MY_RUNNING_WORLD) != CPUSCHED_BUSY_WAIT);
   ASSERT(!myPRDA.halted);

   // initialize
   afterBusyWait = FALSE;
   
 dispatch_after_busy_wait:

   // sanity checks
   ASSERT_NO_INTERRUPTS();
   ASSERT_PRDA_SANITY();

   // convenient abbrevs
   myPCPU = MY_PCPU;
   schedPcpu = CpuSchedPcpu(myPCPU);
   myCell = schedPcpu->cell;
   prev = World_CpuSchedVcpu(MY_RUNNING_WORLD);
   prevVsmp = prev->vsmp;

   // sanity checks
   ASSERT(CpuSchedCellIsLocked(myCell));
   ASSERT(prevVsmp->cell == myCell);

   // debugging
   VcpuLogEvent(prev, "dispatch");

   // clear reschedule flag
   myPRDA.reschedule = FALSE;
   schedPcpu->deferredResched = FALSE;

   // copy dyield to local and reset it
   directedYield = schedPcpu->directedYield;
   schedPcpu->directedYield = NULL;
   
   // update stats
   schedPcpu->stats.yield++;

   // accounting
   if (updateTime || afterBusyWait) {
      CpuSchedCellUpdateTime(myCell);
   }
   CpuSchedVcpuChargeUsage(prev);
   
   // update quantum expiration stats
   if (myCell->now > prev->quantumExpire) {
      prev->stats.quantumExpire++;
      // if our quantum has expired, ignore any directed yield
      schedPcpu->directedYield = NULL;
   }

   // convert busy-waiting vcpu back to non-busy-waiting state
   if (prev->runState == CPUSCHED_BUSY_WAIT) {
      CpuSchedVcpuBusyWaitDone(prev);
      ASSERT(prev->runState != CPUSCHED_BUSY_WAIT);
   }

   // check for skew timeout due to coscheduling or HT
   if (CpuSchedIsMP(prevVsmp)) {
      CpuSchedSkewCheck(prev);
   }
   
   // promote clients from extra -> main queues
   CpuSchedPcpuUpdateQueues(schedPcpu);

   // make scheduling decision
   CpuSchedChoose(myPCPU, prev, directedYield, &choice);
   
   next = choice.min;
   ASSERT(SMP_HTEnabled() || !choice.wholePackage);

   // invalidate preemption vtimes for my pcpu
   // n.b. defer HT partner invalidate until co-schedule complete
   CpuSchedPcpuPreemptionInvalidate(schedPcpu);

   // update next migrate times
   CpuSchedPcpuUpdateMigrationAllowed(schedPcpu, &choice);

   if (next == NULL) {
      // enter generic busy-wait idle loop, if possible
      if ((prevIRQL == SP_IRQL_NONE)
          && CpuSchedVcpuCanBusyWait(prev)) {
         ASSERT(prev->runState == CPUSCHED_WAIT);
         CpuSchedVcpuSetRunState(prev, CPUSCHED_BUSY_WAIT);
         Trace_Event(TRACE_SCHED_PCPU_BWAIT, VcpuWorldID(prev), MY_PCPU, 0, 0);
         
         // invalidate preemption vtime for HT partner
         if (SMP_HTEnabled()) {
            CpuSchedPcpuPreemptionInvalidate(schedPcpu->partner);
         }

         // busy wait loop
         CpuSchedBusyWait(prevIRQL);

         // otherwise set flag, reschedule
         afterBusyWait = TRUE;
         goto dispatch_after_busy_wait;
      }

      // run dedicated idle vcpu
      next = CpuSchedGetIdleVcpu(myPCPU);
   }
   nextVsmp = next->vsmp;
   ASSERT(CpuSchedVcpuIsRunnable(next));
   ASSERT(CpuSchedVsmpCellIsLocked(nextVsmp));

   if (choice.wholePackage
       && !CpuSchedPartnerIsIdle(schedPcpu->id)
       && schedPcpu->partner->handoff == NULL) {
      // handoff to the idle world on our partner,
      // because we've chosen the "whole package" route
      ASSERT(CpuSchedIsMP(nextVsmp) || CpuSchedHTSharing(nextVsmp) != CPUSCHED_HT_SHARE_ANY);
      CpuSchedPcpuCoRun(schedPcpu->partner, 
                        CpuSchedGetIdleVcpu(schedPcpu->partner->id));
   }
   
   // update preemptiblity stats
   // Need to do this even in yield-same case, so we properly consider this
   // scheduler entry to be a preemption-enabling point, e.g. in the case
   // of a nonpreemptible world that does explicit yields.
   if (CPUSCHED_PREEMPT_STATS) {
      // departing world shouldn't keep accumulating preempt-disabled time
      CpuSchedPreemptEnabledStatsUpdate(prev);
      // incoming world SHOULD start tracking time
      next->disablePreemptStartTime = RDTSC();
   }

   // continue running same vcpu?
   if (next == prev) {
      // sanity checks
      ASSERT(next->runState == CPUSCHED_RUN);
      ASSERT(next->pcpu == myPCPU);
      ASSERT(World_VcpuToWorld(next) == MY_RUNNING_WORLD);
      ASSERT(schedPcpu->handoff == NULL);
      ASSERT(nextVsmp->cell == myCell);

      // debugging
      VcpuLogEvent(next, "yield-same");

      // start new quantum (or continue current quantum)
      CpuSchedVcpuSetRunState(next, CPUSCHED_RUN);
      CpuSchedVcpuQuantumStart(next, NULL);

      Trace_Event(next->idle ? TRACE_SCHED_PCPU_IDLE : TRACE_SCHED_PCPU_RUN,
                     VcpuWorldID(prev), MY_PCPU, 0, 0);   
      
      // invalidate preemption vtime for HT partner
      if (SMP_HTEnabled()) {
         CpuSchedPcpuPreemptionInvalidate(schedPcpu->partner);
      }

      // unlock
      CpuSchedCellUnlock(myCell, prevIRQL);

      // run any pending bottom-half handlers w/o reschedule, if enabled
      if (vmkernelLoaded && MY_RUNNING_WORLD->preemptionDisabled) {
         BH_Check(FALSE);
      }
      
      return;
   }

   // deschedule prev
   CpuSchedVcpuDeschedule(prev, 
                          next, 
                          (choice.currentRunnerDest != INVALID_PCPU));

   if (prev->runState == CPUSCHED_READY) {
      // RUN -> READY transition is a preemption
      schedPcpu->stats.preempts++;
   }
   
   // reset vsmp quantum
   CpuSchedVcpuChargeStartSet(prev, 0);
   prev->phaltStart = 0;
   if ((prevVsmp->nRun == 0) && (nextVsmp != prevVsmp)) {
      prevVsmp->quantumExpire = 0;
   }

   // remove from run queue, if any
   if (next->runState == CPUSCHED_READY) {
      if (!next->idle) {
         CpuSchedQueueRemove(next);
      }
   }

   // reset "handoff"
   if (next == schedPcpu->handoff) {
      ASSERT(next->runState == CPUSCHED_READY_CORUN);
      schedPcpu->handoff = NULL;
      next->pcpuHandoff = INVALID_PCPU;
   }

   // state transition
   CpuSchedVcpuSetRunState(next, CPUSCHED_RUN);
   Trace_Event(next->idle ? TRACE_SCHED_PCPU_IDLE : TRACE_SCHED_PCPU_RUN,
               VcpuWorldID(next), MY_PCPU, 0, 0);   
   
   // update current pcpu
   next->pcpu = myPCPU;

   // inter-cell migration
   if (nextVsmp->cell != myCell) {
      ASSERT(choice.cellMigrateAllowed || choice.isDirectedYield);
      remoteCell = nextVsmp->cell;
      CpuSchedDoCellMigrate(myPCPU, myCell, next);
   } else {
      remoteCell = NULL;
   }

   // co-schedule next, if necessary
   if (CpuSchedIsMP(nextVsmp)) {
      switch (nextVsmp->coRunState) {
      case CPUSCHED_CO_RUN:
         // nothing to do; vcpus scheduled independently
         break;

      case CPUSCHED_CO_READY:
         // transition vsmp to CO_RUN, co-schedule
         ASSERT(nextVsmp->nWait == 0);
         ASSERT(nextVsmp->nRun == 1);
         CpuSchedVsmpSetState(nextVsmp, CPUSCHED_CO_RUN);
         // don't coschedule if we're coming from a directed yield
         if (!choice.isDirectedYield) {
            if (SMP_HTEnabled() &&
                CpuSchedPartnerIsIdle(myPCPU)) {
               // our partner is idle, so it should have an idle vtime
               // based on nextVsmp's vtime, not whoever was running here before
               CpuSched_Pcpu *partnerPcpu = CpuSchedPartnerPcpu(myPCPU);
               CpuSchedIdleVtimeInt(partnerPcpu,
                                    nextVsmp,
                                    &partnerPcpu->preemption.vtime,
                                    &partnerPcpu->preemption.vtBonus);
               if (CpuSchedHTSharing(nextVsmp) != CPUSCHED_HT_SHARE_NONE) {
                  // we want to guarantee that we can still preempt our idle partner
                  // during our coscheduling phase
                  partnerPcpu->preemption.vtBonus = MIN(-1, partnerPcpu->preemption.vtBonus);
                  ASSERT(partnerPcpu->handoff != NULL ||
                         CpuSchedPcpuCanPreempt(partnerPcpu, nextVsmp));
               }
            }
            CpuSchedCoSchedule(&choice);
         } else {
            DEBUG_ONLY(CpuMask dummyMask);
            // we shouldn't be here if we NEED coscheduling
            ASSERT(CpuSchedVcpusNeedCosched(nextVsmp, next, &dummyMask) == 0);
         }
         break;

      case CPUSCHED_CO_STOP:
      default:
         NOT_REACHED();
      }
   }

   // invalidate preemption vtime for HT partner
   if (SMP_HTEnabled()) {
      CpuSchedPcpuPreemptionInvalidate(schedPcpu->partner);
   }

   // Handle the "currentRunnerDest" case in which we want to 
   // move the currently-running vcpu to a remote pcpu while running
   // the idle world locally
   if (choice.currentRunnerDest != INVALID_PCPU) {
      ASSERT(next != prev);
      ASSERT(CpuSchedVcpuIsIdle(next));
      ASSERT(CpuSchedVcpuAffinityPermitsPcpu(prev,choice.currentRunnerDest,0));
      CpuSchedPcpuCoRun(CpuSchedPcpu(choice.currentRunnerDest), prev);
   }

   if (SMP_HTEnabled() &&
       !CpuSchedVcpuIsIdle(next) &&
       !CpuSchedPartnerIsIdle(myPCPU) &&
       schedPcpu->partner->handoff == NULL) {
      CpuSched_Vsmp *partnerVsmp = CpuSchedRunningVcpu(schedPcpu->partner->id)->vsmp;
      Sched_HTSharing partnerShare = CpuSchedHTSharing(partnerVsmp);

      // if our partner doesn't want to share packages, mark reschedule on it
      // so that it can either run the idle world or pick a vcpu that does allow sharing
      if (partnerShare == CPUSCHED_HT_SHARE_NONE ||
          (partnerShare == CPUSCHED_HT_SHARE_INTERNALLY && partnerVsmp != nextVsmp)) {
         CpuSched_MarkReschedule(schedPcpu->partner->id);
      }
   }
       
   
   // start a new quantum, or reuse the quantum from the vcpu that yielded to us
   CpuSchedVcpuQuantumStart(next, choice.isDirectedYield ? prev : NULL);
   
   // debugging
   VcpuLogEvent(next, "yield-switch");

   // reset warp if dispatching host world
   if (World_IsHOSTWorld(World_VcpuToWorld(next))) {
      CpuSchedUnwarpConsole();
   }

   // update stats
   prev->stats.worldSwitch++;

   // disable NMIs immediately before switch
   NMI_Disable();

   // sanity checks
   ASSERT(CpuSchedVcpuIsRunnable(next));
   ASSERT(next->chargeStart > 0);
   ASSERT(next != prev);

   // update current world in PRDA
   myPRDA.runningWorld = World_VcpuToWorld(next);
   myPRDA.idle = next->idle;
   
   // set "switch in progress" flag
   ASSERT(!prev->switchInProgress);
   prev->switchInProgress = TRUE;

   // release locks, but keep interrupts disabled
   if (remoteCell != NULL) {
      // sync per-cell "now" and "vtime" first
      CpuSchedCellSyncTime(myCell, remoteCell);
      CpuSchedCellUnlock(remoteCell, CPUSCHED_IRQL);
   }
   CpuSchedCellUnlock(myCell, CPUSCHED_IRQL);
   ASSERT_NO_INTERRUPTS();

   // avoid unlikely potential race:
   //   switching to "next" on local processor while
   //   switching from "next" on remote processor
   if (UNLIKELY(next->switchInProgress)) {
      schedPcpu->stats.switchWait++;
      CpuSchedVcpuWaitForSwitch(next);
   }

   if (next->pcpu != next->pcpuMapped) {
      World_Handle *world = World_VcpuToWorld(next);

      // map local pages 
      ASSERT(next->pcpu == myPCPU);
      CpuSchedVcpuMapPcpu(next, next->pcpu);

      // update pseudo-TSC parameters if needed
      if (cpuSchedConst.numaSystem) {
         Timer_UpdateWorldPseudoTSCConv(world, Timer_GetCycles());
      }
   }

   // world switch
   prev = CpuSchedSwitch(next, prev);

   // Post-switch operations (clear "switch in progress" flag).
   // Note that we're on a different stack now.
   CpuSchedAfterSwitch(prev);

   // enable NMIs immediately after switch
   NMI_Enable();

   // restore original interrupt level
   SP_RestoreIRQ(prevIRQL);

   // Check whether the descheduled one was panicking.
   if (prev->runState == CPUSCHED_ZOMBIE) {
      World_Handle *prevWorld = World_VcpuToWorld(prev);
      if (World_IsVMMWorld(prevWorld) && World_VMM(prevWorld)->inVMMPanic) {
         World_AfterPanic(prevWorld);
      }
   }

   // run any pending bottom-half handlers w/o reschedule, if enabled
   if (vmkernelLoaded && MY_RUNNING_WORLD->preemptionDisabled) {
      BH_Check(FALSE);
   }

   // If we have a deathPending with unconditional kill, don't run anymore
   if (UNLIKELY(MY_RUNNING_WORLD->deathPending) &&
       (MY_RUNNING_WORLD->killLevel == WORLD_KILL_UNCONDITIONAL)) {
      VMLOG(0, MY_RUNNING_WORLD->worldID, "Exiting world on deathPending");
      World_Exit(VMK_OK);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_TimerInterrupt --
 *
 *	Handle an interrupt from the timer.  Requests a reschedule
 *	if the currently-executing vcpu has an expired quantum
 *	or skew timeout.
 *	
 * Results:
 *	None.
 *
 * Side effects:
 *      Modifies scheduler state.
 *
 *----------------------------------------------------------------------
 */
void
CpuSched_TimerInterrupt(Timer_AbsCycles now)
{
   CpuSched_Pcpu *pcpu = CpuSchedPcpu(MY_PCPU);
   CpuSched_Vcpu *vcpu;

   // sanity checks 
   ASSERT(MY_RUNNING_WORLD != NULL);
   ASSERT_NO_INTERRUPTS();

   // debugging
   CpuSchedLogEvent("timer-int", MY_PCPU);

   // periodic timer interrupt:
   //   CpuSched lock not required, since read-only access to
   //   running world on local processor with interrupts disabled.
   //   n.b. due to lock ordering, Timer cannot acquire CpuSched lock
   vcpu = World_CpuSchedVcpu(MY_RUNNING_WORLD);

   // debugging
   if (CONFIG_OPTION(CPU_SCHEDULER_DEBUG)) {
      if ((pcpu->stats.timer & 255) == 0) {
         if (pcpu->stats.yield == pcpu->lastYieldCount) {
            VcpuWarn(vcpu, "yield count on pcpu %u is still %u",
                     MY_PCPU, pcpu->stats.yield);
         }         
         pcpu->lastYieldCount = pcpu->stats.yield;
      }
   }

   // update stats w/o locking (protected by Timer module lock)
   vcpu->stats.timer++;
   pcpu->stats.timer++;
   
   // check for quantum expiration
   if (TRACE_MODULE_ACTIVE) {
      int32 qremain = 0;
      if (vcpu->vsmp->quantumExpire > now) {
         qremain = Timer_TCToMS(vcpu->vsmp->quantumExpire - now);
      }
      Trace_Event(TRACE_SCHED_QUANTUM_REMAIN, VcpuWorldID(vcpu), vcpu->pcpu, 0, qremain);
   }
   if (now > vcpu->quantumExpire) {
      CpuSchedLogEvent("qntm-expire", now - vcpu->quantumExpire);
      CpuSched_MarkRescheduleLocal();
   } else if (pcpu->deferredResched
              && CONFIG_OPTION(CPU_RESCHED_OPT) == CPU_RESCHED_DEFER
              && pcpu->stats.timer % CONFIG_OPTION(CPU_RESCHED_DEFER_TIME) == 0) {
      CpuSched_MarkRescheduleLocal();
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_AsyncCheckActions --
 *
 *      Wakeup or interrupt the specified "world".
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
CpuSched_AsyncCheckActions(World_Handle *world)
{
   CpuSched_Vcpu *vcpu = World_CpuSchedVcpu(world);
   SP_IRQL prevIRQL;
   Bool needWakeup;

   // ignore non-VMM worlds, unmanaged vcpus
   if (!World_IsVMMWorld(world) || CpuSchedVcpuIsUnmanaged(vcpu)) {
      return;
   }

   // skip if world running locally (not waiting, not remote)
   if (world == MY_RUNNING_WORLD) {
      return;
   }

   // case #1: wakeup world if waiting (must wakeup for correctness)
   // prevent wait/async-check-actions race 
   prevIRQL = SP_LockIRQ(&vcpu->actionWakeupLock, SP_IRQL_KERNEL);
   needWakeup = Action_PendingInMask(world, vcpu->actionWakeupMask);
   vcpu->stats.actionWakeupCheck++;
   SP_UnlockIRQ(&vcpu->actionWakeupLock, prevIRQL);
   if (needWakeup) {
      CpuSched_ForceWakeup(world);
      return;
   }
   
   // case #2: interrupt world if running remotely (performance only)
   // check state w/o holding lock (occassional missing/stray IPI OK)
   if (vcpu->runState == CPUSCHED_RUN) {
      PCPU pcpu = vcpu->pcpu;
      if ((pcpu != MY_PCPU) && (pcpu < numPCPUs)) {
         APIC_SendIPI(pcpu, IDT_MONITOR_IPI_VECTOR);
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_AsyncCheckActionsByID --
 *
 *      Wakeup or interrupt the world specified by "worldID".
 *
 * Results:
 *      Returns VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
CpuSched_AsyncCheckActionsByID(World_ID worldID)
{
   World_Handle *world = World_Find(worldID);
   if (world == NULL) {
      return(VMK_NOT_FOUND);
   } else {
      CpuSched_AsyncCheckActions(world);
      World_Release(world);
      return(VMK_OK);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_ActionNotifyVcpu --
 *
 *      Wakeup or interrupt the specified "vcpuid" associated
 *	with the current world.
 *
 * Results:
 *      Returns VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMKERNEL_ENTRY
CpuSched_ActionNotifyVcpu(DECLARE_1_ARG(VMK_ACTION_NOTIFY_VCPU, Vcpuid, v))
{
   World_Handle *world;
   World_ID worldID;

   PROCESS_1_ARG(VMK_ACTION_NOTIFY_VCPU, Vcpuid, v);

   // map vcpu id to world, fail if unable
   worldID = World_VcpuidToWorldID(MY_RUNNING_WORLD, v);
   if (worldID == INVALID_WORLD_ID) {
      return(VMK_BAD_PARAM);
   }

   world = World_Find(worldID);
   if (world != NULL) {
      CpuSched_Vcpu *vcpu = World_CpuSchedVcpu(world);

      // sanity check
      ASSERT(World_VMM(world)->vcpuid == v);

      // check actions, update stats
      CpuSched_AsyncCheckActions(world);
      vcpu->stats.actionNotify++;

      World_Release(world);
   }

   // everything OK
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuActionNotifyRequest --
 *
 *      Set action notification hint associated with "vcpu".
 *	If "notify" is TRUE, requests that monitor performs
 *	VMK_ACTION_NOTIFY_VCPU vmkcall whenever it posts an
 *	action to "vcpu".
 *
 * Results:
 *      Returns VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedVcpuActionNotifyRequest(CpuSched_Vcpu *vcpu, Bool notify)
{
   World_Handle *world = World_VcpuToWorld(vcpu);

   // sanity check
   ASSERT(CpuSchedVsmpCellIsLocked(vcpu->vsmp));

   if (World_IsVMMWorld(world)) {
      CpuSched_Vsmp *vsmp = vcpu->vsmp;
      Vcpuid v;

      // lookup vmm vcpuid associated w/ vcpu
      v = World_VMM(world)->vcpuid;
      ASSERT(v < MAX_VCPUS);

      // update hints replicated in each world's shared area
      FORALL_VSMP_VCPUS(vsmp, vcpu) {
         World_Handle *hintWorld = World_VcpuToWorld(vcpu);
         Action_MonitorNotifyHint(hintWorld, v, notify);
      } VSMP_VCPUS_DONE;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVsmpBoundLag --
 *
 *	Bounds the virtual time associated with "vsmp" to be within
 *	a limited distance around the global virtual time.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Modifies scheduler state.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedVsmpBoundLag(CpuSched_Vsmp *vsmp)
{
   CpuSchedVtime globalBound, localBound, vtime;
   CpuSchedConfig *config;

   // sanity check
   ASSERT(CpuSchedVsmpCellIsLocked(vsmp));

   // avoid bounding dedicated idle vsmps
   if (CpuSchedVsmpIsSystemIdle(vsmp)) {
      return;
   }

   // convenient abbrev
   config = &vsmp->cell->config;
   vtime = vsmp->cell->vtime;

   // bound a VM if it's more than CONFIG_CPU_BOUND_LAG_QUANTA
   // away from the global vtime (judging by the global stride)
   // AND at least one quantum behind/ahead, judging by its own stride
   globalBound = CpuSchedTCToVtime(cpuSchedConst.nStride, config->boundLagCycles);
   localBound = CpuSchedTCToVtime(vsmp->vtime.nStride, config->quantumCycles);
   
   if ((vsmp->vtime.main < vtime - globalBound) &&
       (vsmp->vtime.main < vtime - localBound)) {
      // bound lag behind
      CpuSchedVtime behind = vtime - vsmp->vtime.main;
      CpuSchedVtime warp = behind / 2;
      VSMPLOG(1, vsmp, "boundlag-behind");

      vsmp->vtime.main += warp;
      vsmp->stats.boundLagBehind++;
      vsmp->stats.boundLagTotal += warp;
      if (CONFIG_OPTION(CPU_SCHEDULER_DEBUG)) {
         VsmpLog(vsmp, "behind-aged by %u vtMsec",
                 (uint32)Timer_TCToMS(CpuSchedVtimeToTC(vsmp->vtime.nStride, warp)));
      }
   } else if ((vsmp->vtime.main > vtime + globalBound) &&
              (vsmp->vtime.main > vtime + localBound)) {
      // bound lag ahead
      CpuSchedVtime ahead = vsmp->vtime.main - vtime;
      CpuSchedVtime warp = ahead / 2;
      VSMPLOG(1, vsmp, "boundlag-ahead");

      vsmp->vtime.main -= warp;
      vsmp->stats.boundLagAhead++;
      if (CONFIG_OPTION(CPU_SCHEDULER_DEBUG)) {
         VsmpLog(vsmp, "ahead-aged by %u vtMsec",
                 (uint32)Timer_TCToMS(CpuSchedVtimeToTC(vsmp->vtime.nStride, warp)));
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedAgeVtime --
 *
 *	Updates "vtime" to reduces its distance from "vtNow" by a
 *	multiplicative aging factor.
 *
 * Results:
 *	Updates "vtime" to reduces its distance from "vtNow" by a
 *	multiplicative aging factor.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
CpuSchedAgeVtime(CpuSchedVtime vtNow, CpuSchedVtime *vtime)
{
   if (*vtime < vtNow) {
      CpuSchedVtime behind = vtNow - *vtime;
      behind /= CPUSCHED_CREDIT_AGE_DIVISOR;
      *vtime = vtNow - behind;
   } else if (*vtime > vtNow) {
      CpuSchedVtime ahead = *vtime - vtNow;
      ahead /= CPUSCHED_CREDIT_AGE_DIVISOR;
      *vtime = vtNow + ahead;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVsmpAgeVtimes --
 *
 *	Reduces the distance of the virtual time associated with "vsmp"
 *	from the global virtual time  by a multiplicative aging factor.
 *	Caller must hold scheduler cell lock for "vsmp".
 *
 * Results:
 *      Modifies virtual times associated with "vsmp".
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedVsmpAgeVtimes(CpuSched_Vsmp *vsmp)
{
   // sanity check
   ASSERT(CpuSchedVsmpCellIsLocked(vsmp));

   // avoid aging dedicated idle vsmps
   if (!CpuSchedVsmpIsSystemIdle(vsmp)) {
      CpuSchedVtime vtExtra = vsmp->vtime.extra;
      CpuSchedAgeVtime(vsmp->cell->vtime, &vsmp->vtime.extra);
      vsmp->stats.vtimeAged += (vsmp->vtime.extra - vtExtra);
      if (CpuSchedEnforceMax(&vsmp->alloc)) {
         CpuSchedAgeVtime(vsmp->cell->vtime, &vsmp->vtimeLimit);
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedCellAgeVtimes --
 *
 *	Reduces the distance from the global virtual time associated
 *	with all vsmps in "cell" by a multiplicative aging factor
 *	to prevent monopolization of extra time consumption.
 *	Caller must hold scheduler "cell" lock.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Modifies "cell".
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedCellAgeVtimes(CpuSched_Cell *cell)
{
   // sanity check
   ASSERT(CpuSchedCellIsLocked(cell));

   // update global times
   CpuSchedCellUpdateTime(cell);

   // age credit for all vsmps
   FORALL_CELL_VSMPS(cell, vsmp) {
      // update "pending" credit for waiting vcpus
      FORALL_VSMP_VCPUS(vsmp, vcpu) {
         CpuSchedVcpuWaitUpdate(vcpu);
      } VSMP_VCPUS_DONE;

      // bound lag, if necessary
      CpuSchedVsmpBoundLag(vsmp);

      // age vtimes, if appropriate
      CpuSchedVsmpAgeVtimes(vsmp);
   } CELL_VSMPS_DONE;

   // invalidate preemption vtimes 
   CpuSchedCellPreemptionInvalidate(cell);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedConfigInit --
 *
 *      Checks for changes to configurable scheduler parameters, and
 *	updates scheduler state to reflect any changes.
 *
 * Results:
 *      Modifies "config".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedConfigInit(CpuSchedConfig *c)
{
   uint32 lgVtimeReset;

   // credit aging period (millisec)
   c->creditAgePeriod = CONFIG_OPTION(CPU_CREDIT_AGE_PERIOD);

   // thresholds
   c->quantumCycles = Timer_MSToTC(CONFIG_OPTION(CPU_QUANTUM));
   c->idleQuantumCycles = Timer_MSToTC(CONFIG_OPTION(CPU_IDLE_QUANTUM));
   c->boundLagCycles = CONFIG_OPTION(CPU_BOUND_LAG_QUANTA) * c->quantumCycles;
   c->sysAcctLimitCycles = SCHED_SYS_ACCT_SAMPLE * c->quantumCycles;
   c->yieldThrottleTSC = Timer_USToTSC(CONFIG_OPTION(CPU_YIELD_THROTTLE_USEC));

   // thresholds (vtime)
   // derivation of "ahead" threshold:
   //   want:  cvt' <= gvt' + cstride
   //   where: cvt' = cvt + cstride
   //          gvt' = gvt + n*gstride
   //      =>  cvt + cstride <= gvt + n*gstride + cstride
   //	   =>  cvt <= gvt +n*gstride
   c->vtAheadThreshold = CpuSchedTCToVtime(cpuSchedConst.nStride, c->quantumCycles);
   c->preemptionBonusCycles = Timer_MSToTC(CONFIG_OPTION(CPU_PREEMPTION_BONUS));
   
   // migration
   c->migPcpuWaitCycles = Timer_MSToTC(CONFIG_OPTION(CPU_PCPU_MIGRATE_PERIOD));
   c->migCellWaitCycles = Timer_MSToTC(CONFIG_OPTION(CPU_CELL_MIGRATE_PERIOD));
   c->runnerMoveWaitCycles = Timer_MSToTC(CONFIG_OPTION(CPU_RUNNER_MOVE_PERIOD));
   c->migChance = CONFIG_OPTION(CPU_MIGRATE_CHANCE);
   c->vcpuReschedOpt = (CpuVcpuReschedOpt) CONFIG_OPTION(CPU_RESCHED_OPT);

   // need to measure this in tsc cycles, not TC cycles (see IdlePackageRebalanceCheck)
   c->idlePackageRebalanceCycles =
      Timer_MSToTSC(CONFIG_OPTION(CPU_IDLE_PACKAGE_REBALANCE_PERIOD));
   
   // cache affinity for coscheduling
   c->coSchedCacheAffinCycles = Timer_MSToTC(CONFIG_OPTION(CPU_COSCHED_CACHE_AFFINITY_BONUS));

   // added to idle vtime
   c->idleVtimeMsPenaltyCycles = Timer_MSToTC(CONFIG_OPTION(CPU_HALTING_IDLE_MS_PENALTY));

   // idle vtime interrupt penalties
   c->intrLevelPenaltyCycles = Timer_MSToTC(CONFIG_OPTION(CPU_IDLE_VTIME_INTERRUPT_PENALTY));
      
   // skew 
   c->skewSampleUsec = CONFIG_OPTION(CPU_SKEW_SAMPLE_USEC);
   c->skewSampleMinInterval = Timer_USToTC(c->skewSampleUsec) / 2;
   // on a hyperthreaded system, each skew sample counts as "logicalPerPhysical" points
   // of skew, so we multiply the threshold accordingly
   c->skewSampleThreshold = CONFIG_OPTION(CPU_SKEW_SAMPLE_THRESHOLD)
      * SMP_LogicalCPUPerPackage();
   if (c->skewSampleThreshold == 0) {
      c->skewSampleThreshold = CPUSCHED_IGNORE_SKEW;
      CpuSchedLog("ignoring skew from now on");
   }
   c->intraSkewThreshold = CONFIG_OPTION(CPU_INTRASKEW_THRESHOLD) * SMP_LogicalCPUPerPackage();
   c->relaxCosched = CONFIG_OPTION(CPU_RELAXED_COSCHED);
   
   // console warping
   c->consoleWarpCycles = Timer_MSToTC(CONFIG_OPTION(CPU_COS_WARP_PERIOD));

   // quarantining
   c->htEventsUpdateCycles = Timer_MSToTC(CPUSCHED_HT_EVENT_PERIOD);

   // vtime reset
   lgVtimeReset = CONFIG_OPTION(CPU_VTIME_RESET_LG);
   ASSERT(lgVtimeReset <= CPUSCHED_VTIME_RESET_LG);
   c->vtimeResetThreshold = (1LL << lgVtimeReset);
   c->vtimeResetAdjust = c->vtimeResetThreshold / 2;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedPeriodic --
 *
 *      Timer-based callback to perform periodic scheduler computations,
 *	such as configuration updates and base share reallocations.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	May update scheduler state.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedPeriodic(UNUSED_PARAM(void *ignore),
                 UNUSED_PARAM(Timer_AbsCycles timestamp))
{
   // update stats
   cpuSched.periodicCount++;
   
   // periodic processing
   CpuSched_Reallocate();
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedPeriodicAgeVtimes --
 *
 *      Timer-based callback to perform periodic virtual time aging.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Updates scheduler state.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedPeriodicAgeVtimes(UNUSED_PARAM(void *ignore),
                          UNUSED_PARAM(Timer_AbsCycles timestamp))
{
   CpuSchedVtime vtNow = 0;
   uint32 period = 0;

   // age vsmps
   FORALL_CELLS_UNLOCKED(c) {
      SP_IRQL prevIRQL = CpuSchedCellLock(c);
      CpuSchedCellAgeVtimes(c);
      period = c->config.creditAgePeriod;
      vtNow = c->vtime;
      CpuSchedCellUnlock(c, prevIRQL);
   } CELLS_DONE;

   // age groups
   ASSERT(vtNow > 0);
   CpuSchedAgeAllGroupVtimes(vtNow);

   // arrange for next invocation
   ASSERT(period != 0);
   Timer_Add(MY_PCPU, CpuSchedPeriodicAgeVtimes, period, TIMER_ONE_SHOT, NULL);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedQueueInit --
 *
 *      Initializes "q" based on "extra" attribute.
 *
 * Results:
 *	Initializes "q".
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedQueueInit(CpuSchedQueue *q, Bool extra, Bool limbo)
{
   List_Init(&q->queue);
   q->extra = extra;
   q->limbo = limbo;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedPcpuInit --
 *
 *      Initializes "schedPcpu", associating it with processor "pcpu"
 *	and scheduler cell "cell".
 *
 * Results:
 *	Initializes "schedPcpu".
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedPcpuInit(CpuSched_Pcpu *schedPcpu, PCPU pcpu, CpuSched_Cell *cell)
{
   char buf[20];
   uint64 histoBuckets[] = { 1, 10, 100, 500, 1000, 5000, 15000, 30000, 50000, 100000 };
   uint32 numBuckets = sizeof(histoBuckets) / sizeof(uint64);
                             
   // zero state
   memset(schedPcpu, 0, sizeof(CpuSched_Pcpu));

   // initialize identity
   schedPcpu->id = pcpu;

   // associate cell
   schedPcpu->cell = cell;
   Log("pcpu %u: cell %u", schedPcpu->id, schedPcpu->cell->id);

   // cache hypertwin, if any
   if (SMP_HTEnabled()) {
      schedPcpu->partner = CpuSchedPcpu(SMP_GetPartnerPCPU(schedPcpu->id));
      Log("partner of pcpu %u is pcpu %u at address 0x%x",
          schedPcpu->id, SMP_GetPartnerPCPU(schedPcpu->id), 
          (uint32)CpuSchedPcpu(SMP_GetPartnerPCPU(schedPcpu->id)));
   } else {
      schedPcpu->partner = NULL;
      Log("null partner for pcpu %u", pcpu);
   }

   // initialize queues
   CpuSchedQueueInit(&schedPcpu->queueMain, FALSE, FALSE);
   CpuSchedQueueInit(&schedPcpu->queueExtra, TRUE, FALSE);
   CpuSchedQueueInit(&schedPcpu->queueLimbo, FALSE, TRUE);

   // initialize vtime cache
   CpuSchedPcpuGroupVtimeCacheInvalidate(schedPcpu);

   // initialize histograms
   schedPcpu->switchWaitHisto = Histogram_New(mainHeap, numBuckets, histoBuckets);
   schedPcpu->haltHisto = Histogram_New(mainHeap, numBuckets, histoBuckets);
   
   // setup lock
   snprintf(buf, 19, "CpuHalt.%02u", pcpu);
   SP_InitLockIRQ(buf, &schedPcpu->haltLock, SP_RANK_IRQ_LEAF);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedCellInit --
 *
 *      Initializes "cell" with the specified cell "id" and 
 *	"config" info, managing the set of processors associated
 *	with "pcpuMask".
 *
 * Results:
 *	Initializes "cell".
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedCellInit(CpuSched_Cell *cell,
                 uint32 id,
                 CpuMask pcpuMask,
                 const CpuSchedConfig *config)
{
   char nameBuf[32];
   uint32 i;

   // sanity checks
   ASSERT(id < CPUSCHED_CELLS_MAX);
   ASSERT(pcpuMask != 0);

   // zero state
   memset(cell, 0, sizeof(CpuSched_Cell));

   // initialize lock
   snprintf(nameBuf, sizeof(nameBuf), "CpuSchedCell.%u", id);
   SP_InitLockIRQ(nameBuf, &cell->lock, SP_RANK_CPUSCHED_CELL(id));

   // initialize identity
   cell->id = id;

   // initialize managed pcpus
   cell->pcpuMask = pcpuMask;
   cell->nPCPUs = 0;
   FORALL_PCPUS(p) {
      if (pcpuMask & CPUSCHED_AFFINITY(p)) {
         cell->pcpu[cell->nPCPUs++] = p;
      }
   } PCPUS_DONE;
   for (i = cell->nPCPUs; i < CPUSCHED_PCPUS_MAX; i++) {
      cell->pcpu[i] = INVALID_PCPU;
   }

   // initialize real time, virtual time
   cell->now = Timer_GetCycles();
   cell->vtime = 0;
   cell->vtResetTimer = TIMER_HANDLE_NONE;

   // initialize configuration info
   cell->config = *config;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedConstInit --
 *
 *      Initializes "c" to contain values that are written only
 *	once during initialization, and can therefore be read
 *	without holding any locks.
 *
 * Results:
 *	Initializes "c".
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedConstInit(CpuSchedConst *c)
{
   int i;

   // initialize timestamp
   c->uptimeStart = Timer_GetCycles();
   
   // cycles conversions
   c->cyclesPerSecond = Timer_CyclesPerSecond();
   c->cyclesPerMinute = 60 * c->cyclesPerSecond;

   // randomized jitter
   c->smallJitterCycles = Timer_USToTC(CPUSCHED_SMALL_JITTER_USEC);
   c->smallJitterCycles = Util_RoundupToPowerOfTwo(c->smallJitterCycles);
   ASSERT(Util_IsPowerOf2(c->smallJitterCycles));
   CpuSchedLog("jitter: cycles=%u, usec=%u",
               c->smallJitterCycles,
               (uint32)Timer_TCToUS(c->smallJitterCycles));

   // round up mhz to our desired accuracy
   if (cpuMhzEstimate % CPUSCHED_MHZ_ROUNDING == 0) {
      c->roundedMhz = cpuMhzEstimate;
   } else {
      c->roundedMhz = cpuMhzEstimate - (cpuMhzEstimate % CPUSCHED_MHZ_ROUNDING)
         + CPUSCHED_MHZ_ROUNDING;
   }
   
   // base conversions
   c->percentPcpu = 100 / SMP_LogicalCPUPerPackage();
   c->percentTotal = c->percentPcpu * numPCPUs;
   
   c->unitsPerPkg[SCHED_UNITS_BSHARES] = CPUSCHED_BASE_PER_PACKAGE;
   c->unitsPerPkg[SCHED_UNITS_PERCENT] = 100;
   c->unitsPerPkg[SCHED_UNITS_MHZ] = c->roundedMhz;

   // initialize base allocation
   c->baseShares = CpuSchedUnitsToBaseShares(c->percentTotal, SCHED_UNITS_PERCENT);
   c->stride = CpuSchedSharesToStride(c->baseShares);
   c->nStride = c->stride * numPCPUs;

   // compute default (unconstrained) affinity mask 
   c->defaultAffinity = 0;
   FORALL_PCPUS(p) {
      c->defaultAffinity |= CPUSCHED_AFFINITY(p);
   } PCPUS_DONE;
   Log("defaultAffinity=0x%x", c->defaultAffinity);

   // NUMA system?
   if (NUMA_GetNumNodes() > 1) {
      c->numaSystem = TRUE;
   }

   // precompute a bitmask for each NUMA node
   FORALL_PCPUS(p) {
      c->numaNodeMasks[NUMA_PCPU2NodeNum(p)] |= CPUSCHED_AFFINITY(p);
   } PCPUS_DONE;
   if (NUMA_GetNumNodes() <= 1) {
      ASSERT(c->numaNodeMasks[0] == c->defaultAffinity);
   }
   for (i=0; i < NUMA_GetNumNodes(); i++) {
      Log("node %d: mask=0x%x", i, c->numaNodeMasks[i]);
   }

   c->machineClearEvent = Vmkperf_GetEventInfo("machine_clear_any");
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedProcInit --
 *
 *      Initializes "proc", setting up and registering scheduler
 *	procfs nodes with "dir" as a parent.
 *
 * Results:
 *	Initializes "proc".
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedProcInit(CpuSchedProc *proc, Proc_Entry *dir)
{
   // register "sched/cpu" proc node
   Proc_InitEntry(&proc->cpu);
   proc->cpu.parent = dir;
   proc->cpu.read = CpuSchedProcRead;
   proc->cpu.private = (void *) FALSE;
   Proc_Register(&proc->cpu, "cpu", FALSE);

   // register "sched/cpu-verbose" proc node
   Proc_InitEntry(&proc->cpuVerbose);
   proc->cpuVerbose.parent = dir;
   proc->cpuVerbose.read = CpuSchedProcRead;
   proc->cpuVerbose.private = (void *) TRUE;
   Proc_Register(&proc->cpuVerbose, "cpu-verbose", FALSE);   

   // register "sched/cpu-state-times" proc node
   Proc_InitEntry(&proc->cpuStateTimes);
   proc->cpuStateTimes.parent = dir;
   proc->cpuStateTimes.read = CpuSchedProcStateTimesRead;
   Proc_Register(&proc->cpuStateTimes, "cpu-state-times", FALSE);

   // register "sched/cpu-state-counts" proc node
   Proc_InitEntry(&proc->cpuStateCounts);
   proc->cpuStateCounts.parent = dir;
   proc->cpuStateCounts.read = CpuSchedProcStateCountsRead;
   Proc_Register(&proc->cpuStateCounts, "cpu-state-counts", FALSE);

   // register "sched/cpu-run-times" proc node
   Proc_InitEntry(&proc->pcpuRunTimes);
   proc->pcpuRunTimes.parent = dir;
   proc->pcpuRunTimes.read = CpuSchedProcPcpuRunTimesRead;
   Proc_Register(&proc->pcpuRunTimes, "cpu-run-times", FALSE);

   // register "sched/idle" proc node
   Proc_InitEntry(&proc->idle);
   proc->idle.parent = dir;
   proc->idle.read = CpuSchedProcIdleRead;
   Proc_Register(&proc->idle, "idle", FALSE);

   // register "sched/ncpus" entry
   Proc_InitEntry(&proc->ncpus);
   proc->ncpus.parent = dir;
   proc->ncpus.read = CpuSchedProcNcpusRead;
   Proc_Register(&proc->ncpus, "ncpus", FALSE);

   // register "sched/cpu-debug" entry
   Proc_InitEntry(&proc->debug);
   proc->debug.parent = dir;
   proc->debug.read = CpuSchedProcDebugRead;
   proc->debug.write = CpuSchedProcDebugWrite;
   Proc_RegisterHidden(&proc->debug, "cpu-debug", FALSE);

   // register "sched/reset-stats" entry
   Proc_InitEntry(&proc->resetStats);
   proc->resetStats.parent = dir;
   proc->resetStats.write = CpuSchedProcResetStatsWrite;
   Proc_RegisterHidden(&proc->resetStats, "reset-stats", FALSE);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedCellSizeInit --
 *
 *      Computes acceptable scheduler cell size based on configured 
 *	value "configSize" and various cell size constraints,
 *	including: all cells must have same size, cannot split
 *	packages across cells, and cannot split NUMA nodes across
 *	cells.  Some of these constraints could be relaxed, but
 *	the additional complexity probably isn't justified.  If
 *	unable to meet constraints, returns size for single cell
 *	containing all pcpus.
 *
 * Results:
 *	Returns acceptable number of pcpus per scheduler cell.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static uint32
CpuSchedCellSizeInit(uint32 configSize)
{
   uint32 cellSize, singleCellSize, pkgSize;

   // initialize
   cellSize = configSize;
   singleCellSize = numPCPUs;
   pkgSize = SMP_LogicalCPUPerPackage();

   // special case: no cell size => default
   if (cellSize == 0) {
      cellSize = CPUSCHED_CELL_PACKAGES_DEFAULT * pkgSize;
      Log("no specified cell size, trying default size %u (%u packages)",
          cellSize, CPUSCHED_CELL_PACKAGES_DEFAULT);
   }

   // ensure cell size is a multiple of HT package size
   if (cellSize % pkgSize != 0) {
      Log("cell size %u not multiple of HT package size %u, using single cell",
          cellSize, pkgSize);
      return(singleCellSize);
   }

   // ensure cell size evenly divides numPCPUs
   if (numPCPUs % cellSize != 0) {
      Log("%u pcpus not multiple of cell size %u, using single cell",
          numPCPUs, cellSize);
      return(singleCellSize);
   }

   // ensure cell size doesn't result in too many cells
   if (numPCPUs / cellSize > CPUSCHED_CELLS_MAX) {
      Log("%u pcpus with cell size %u exceeds max cells %u, using single cell",
          numPCPUs, cellSize, CPUSCHED_CELLS_MAX);
      return(singleCellSize);
   }

   // ensure reasonable cell size constraints for NUMA systems
   if (cpuSchedConst.numaSystem) {
      uint32 minNodeSize, maxNodeSize, nodeSize;

      minNodeSize = CPUSCHED_PCPUS_MAX;
      maxNodeSize = 0;
      FORALL_NUMA_NODES(n) {
         minNodeSize = MIN(minNodeSize, NUMA_GetNumNodeCpus(n));
         maxNodeSize = MAX(maxNodeSize, NUMA_GetNumNodeCpus(n));
      } NUMA_NODES_DONE;

      if (minNodeSize != maxNodeSize) {
         Log("node size varies (min=%u, max=%u), using single cell",
             minNodeSize, maxNodeSize);
         return(singleCellSize);
      }

      nodeSize = minNodeSize;
      if ((nodeSize < cellSize) && (cellSize % nodeSize != 0)) {
         Log("cell size %u not multiple of node size %u, using single cell",
             cellSize, nodeSize);
         return(singleCellSize);
      }
         
      if ((nodeSize > cellSize) && (nodeSize % cellSize != 0)) {
         Log("node size %u not multiple of cell size %u, using single cell",
             nodeSize, cellSize);
         return(singleCellSize);
      }
   }

   // cell size meets all constraints
   LOG(0, "cellSize=%u", cellSize);
   return(cellSize);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedCellsInit --
 *
 *      Initializes all scheduler cells with the initial
 *	configuration info specified by "config".  Attempts
 *	to partition pcpus into scheduler cells based on 
 *	the configured scheduler cell size "configSize".
 *
 * Results:
 *	Initializes global scheduler state.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedCellsInit(const CpuSchedConfig *config, uint32 configSize)
{
   CpuMask pcpuMask[CPUSCHED_CELLS_MAX];
   uint32 id, cellSize, nCells, pkgSize;

   // initialize
   pkgSize = SMP_LogicalCPUPerPackage();
   for (id = 0; id < CPUSCHED_CELLS_MAX; id++) {
      pcpuMask[id] = 0;
   }

   // compute acceptable cell size
   cellSize = CpuSchedCellSizeInit(configSize);

   // convert cell size into number of cells
   nCells = numPCPUs / cellSize;
   Log("partitioning %u pcpus into %u cells", numPCPUs, nCells);

   // sanity checks
   ASSERT(cellSize % pkgSize == 0);
   ASSERT(numPCPUs % cellSize == 0);
   ASSERT(nCells > 0);
   ASSERT(nCells <= numPCPUs);

   if (nCells == 1) {
      // use single cell
      pcpuMask[0] = cpuSchedConst.defaultAffinity;      
   } else {
      // use multiple cells
      uint32 pkgPerCell = cellSize / pkgSize;
      uint32 pkgCount = 0;
      
      if (cpuSchedConst.numaSystem) {
         // assign node packages to cells
         FORALL_NUMA_NODES(n) {
            FORALL_NODE_PACKAGES(n, p) {
               const SMP_PackageInfo *pkg = SMP_GetPackageInfo(p);
               int i;
                  
               id = pkgCount / pkgPerCell;
               for (i = 0; i < pkg->numLogical; i++) {
                  pcpuMask[id] |= CPUSCHED_AFFINITY(pkg->logicalCpus[i]);
               }
               pkgCount++;
            } NODE_PACKAGES_DONE;
         } NUMA_NODES_DONE;
      } else {
         // assign packages to cells
         FORALL_PACKAGES(p) {
            const SMP_PackageInfo *pkg = SMP_GetPackageInfo(p);
            int i;

            id = pkgCount / pkgPerCell;
            for (i = 0; i < pkg->numLogical; i++) {
               pcpuMask[id] |= CPUSCHED_AFFINITY(pkg->logicalCpus[i]);
            }
            pkgCount++;
         } PACKAGES_DONE;
      }
   }

   // initialize cells
   cpuSched.nCells = nCells;
   Log("ncells=%u", cpuSched.nCells);
   for (id = 0; id < cpuSched.nCells; id++) {
      Log("cell %u: pcpuMask=0x%x", id, pcpuMask[id]);
      CpuSchedCellInit(&cpuSched.cell[id], id, pcpuMask[id], config);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * CpuSched_Init --
 *
 *      Initializes global cpu scheduler state.  Attempts to use
 *	specified "cellSize" to configure scheduler cells.
 *	Roots scheduler procfs nodes at "procSchedDir",
 *	registers timer callbacks, initializes root group.
 *
 * Results:
 *	Initializes remaining global scheduler state.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
void
CpuSched_Init(Proc_Entry *procSchedDir, uint32 cellSize)
{
   extern char *CpuSchedAfterHLTLabel;
   CpuSchedConfig config;
   Bool success;

   // sanity checks
   ASSERT(CPUSCHED_IRQL == EVENTQUEUE_IRQL);
   ASSERT(World_VcpuToWorld(World_CpuSchedVcpu(MY_RUNNING_WORLD)) == MY_RUNNING_WORLD);

   // initialize exported globals
   CpuSched_EIPAfterHLT = (uint32) &CpuSchedAfterHLTLabel;
   LOG(0, "CpuSched_EIPAfterHLT=0x%x", CpuSched_EIPAfterHLT);

   // initialize resched interrupt handler
   success = IDT_VectorAddHandler(IDT_RESCHED_VECTOR,
                                  CpuSchedReschedIntHandler,
                                  NULL, FALSE, "resched", 0);
   ASSERT_NOT_IMPLEMENTED(success);

   // initialize constant values
   CpuSchedConstInit(&cpuSchedConst);

   // initialize configuration values
   CpuSchedConfigInit(&config);

   // zero state
   memset(&cpuSched, 0, sizeof(CpuSched));

   // initialize cells
   CpuSchedCellsInit(&config, cellSize);

   // initialize per-PCPU state
   FORALL_PCPUS(p) {
      CpuSched_Cell *cell = NULL;

      // find cell responsible for p
      FORALL_CELLS_UNLOCKED(c) {
         if ((c->pcpuMask & CPUSCHED_AFFINITY(p)) != 0) {
            cell = c;
            break;
         }
      } CELLS_UNLOCKED_DONE;

      CpuSchedPcpuInit(CpuSchedPcpu(p), p, cell);
   } PCPUS_DONE;

   // initialize snapshot state
   SP_InitLock("CpuSnapshot", &cpuSched.procSnap.lock, SP_RANK_LEAF);

   // register periodic callbacks
   Timer_Add(MY_PCPU, CpuSchedPeriodic,
             CPUSCHED_TIMER_PERIOD, TIMER_PERIODIC, NULL);
   Timer_Add(MY_PCPU, CpuSchedPeriodicAgeVtimes,
             MY_CELL->config.creditAgePeriod, TIMER_ONE_SHOT, NULL);
   
   // register a skew timer on each pcpu
   if (MY_CELL->config.skewSampleUsec != CPUSCHED_IGNORE_SKEW) {
      FORALL_SCHED_PCPUS(p) {
         p->skewTimer = Timer_AddHiRes(p->id,
                                       CpuSchedSampleSkew,
                                       MY_CELL->config.skewSampleUsec,
                                       TIMER_PERIODIC,
                                       NULL);
         ASSERT(p->skewTimer != TIMER_HANDLE_NONE);
      } SCHED_PCPUS_DONE;
   }
   
   // initialize procfs nodes
   CpuSchedProcInit(&cpuSched.proc, procSchedDir);

   // initialize cpu metrics module
   CpuMetrics_Init(procSchedDir);

   // verbose debug logging
   if (CPUSCHED_DEBUG_VERBOSE) {
      Log_EventLogSetTypeActive(EVENTLOG_CPUSCHED, TRUE);
   }

   // machine-clear HT quarantining
   if (SMP_HTEnabled()) {
      ASSERT(cpuSchedConst.machineClearEvent != NULL);
      if (CONFIG_OPTION(CPU_MACHINE_CLEAR_THRESH) > 0) {
         CpuSchedSetHTQuarantineActive(TRUE);
      }
   }
   Config_RegisterCallback(CONFIG_CPU_MACHINE_CLEAR_THRESH,
                           CpuSchedHTQuarantineCallback);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_Wakeup --
 *
 *      Wakeup any worlds waiting on the specified "event".
 *
 * Results:
 *      Returns TRUE iff one or more worlds was awakened.
 *
 * Side effects:
 *      May modify global scheduler state.
 *
 *----------------------------------------------------------------------
 */
Bool
CpuSched_Wakeup(uint32 event)
{
   EventQueue *eventQueue;
   World_Handle *world;
   SP_IRQL prevIRQL;
   int nWakeup = 0;

   // convenient abbrevs
   eventQueue = EventQueue_Find(event);   

   // wakeup all worlds waiting on event
   prevIRQL = EventQueue_Lock(eventQueue);
   world = (World_Handle *) List_First(&eventQueue->queue);
   while (!List_IsAtEnd(&eventQueue->queue, (List_Links *) world)) {
      CpuSched_Vcpu *vcpu = World_CpuSchedVcpu(world);
      World_Handle *next = (World_Handle *) List_Next((List_Links *) world);

      // ensure matching event
      if (vcpu->waitEvent == event) {
         SP_IRQL schedIRQL;

         // acquire lock
         schedIRQL = CpuSchedVsmpCellLock(vcpu->vsmp);

         // remove from eventQueue, perform wakeup
         List_Remove(&world->sched.links);
         if (CpuSchedVcpuIsWaiting(vcpu)) {
            CpuSchedVcpuWakeup(vcpu);
         } else {
            ASSERT(CpuSchedVcpuIsUnmanaged(vcpu));
         }

         // release lock
         CpuSchedVsmpCellUnlock(vcpu->vsmp, schedIRQL);
	 nWakeup++;
      }

      // advance to next element
      world = next;
   }
   EventQueue_Unlock(eventQueue, prevIRQL);

   return(nWakeup > 0);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_ForceWakeup --
 *
 *      Wakeup "world", if it is sleeping. This function will
 *      wakeup the world irrespective of the event on which it is 
 *      sleeping.
 *
 * Results:
 *      TRUE if "world" was woken, FALSE otherwise. 
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Bool 
CpuSched_ForceWakeup(World_Handle *world)
{
   Bool res = FALSE;
   SP_IRQL schedIRQL, eventIRQL;
   EventQueue *eventQueue;
   CpuSched_Vcpu *vcpu = World_CpuSchedVcpu(world);

   schedIRQL = CpuSchedVcpuEventLock(vcpu, &eventQueue, &eventIRQL);
   
   // if the world is actually waiting, do the wakeup
   if (CpuSchedVcpuIsWaiting(vcpu)) {
      ASSERT(eventQueue != NULL);
      EventQueue_Remove(eventQueue, world);
      ASSERT(vcpu->waitState < CPUSCHED_NUM_WAIT_STATES);
      vcpu->stats.forceWakeup[vcpu->waitState]++;
      CpuSchedVcpuWakeup(vcpu);
      res = TRUE;
   }

   CpuSchedVsmpCellUnlock(vcpu->vsmp, schedIRQL);
   if (eventQueue != NULL) {
      EventQueue_Unlock(eventQueue, eventIRQL);
   }

   return (res);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedWait --
 *
 *	The running world is put to sleep pending a wakeup on "event",
 *	or the arrival of an action in "actionWakeupMask".  Note 
 *	that a spurious wakeup may still be delivered even when
 *	"actionWakeupMask" is empty.  If "lock" or "lockIRQ" is not
 *	NULL, then it is released before the world is put to sleep.
 *	Caller must hold "eventQueue" lock and scheduler cell lock
 *	for local processor, acquired in that order.
 *
 * Results:
 *      VMK_DEATH_PENDING if the world is supposed to die,
 *	VMK_OK otherwise.
 *
 * Side effects:
 *	Updates "eventQueue", adding current world.
 *	Releases "lock" and "lockIRQ", if any.
 *	Releases "eventQueue" lock.
 *      Modifies currently-executing world.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
CpuSchedWait(EventQueue *eventQueue,
             uint32 event,
             CpuSched_WaitState waitType,
             uint32 actionWakeupMask,
             SP_SpinLockIRQ *lockIRQ,
             SP_SpinLock *lock,
             SP_IRQL prevIRQL)
{
   World_Handle *myWorld = MY_RUNNING_WORLD;
   CpuSched_Vcpu *vcpu;
   CpuSched_Vsmp *vsmp;
   SP_IRQL awIRQL;
   Bool doWait;

   // sanity checks
   ASSERT(myWorld != NULL);
   ASSERT(EventQueue_IsLocked(eventQueue));

   // sanity check: only VMM worlds need action-based wakeups
   ASSERT((actionWakeupMask == 0) || World_IsVMMWorld(myWorld));

   // convenient abbrevs
   vcpu = World_CpuSchedVcpu(myWorld);
   vsmp = vcpu->vsmp;

   // debugging
   VcpuLogEvent(vcpu, "wait");

   // sanity checks
   ASSERT(CpuSchedVcpuRunOrBwait(vcpu));
   ASSERT(CpuSchedVsmpCellIsLocked(vsmp));
   ASSERT(!vcpu->idle);

   // initialize
   doWait = TRUE;

   // prevent wait/async-check-actions race
   if (actionWakeupMask != 0) {
      CpuSchedVcpuActionNotifyRequest(vcpu, TRUE);
   }
   awIRQL = SP_LockIRQ(&vcpu->actionWakeupLock, SP_IRQL_KERNEL);
   vcpu->actionWakeupMask = actionWakeupMask;
   if (vcpu->actionWakeupMask != 0) {
      if (World_IsVMMWorld(myWorld) &&
          Action_PendingInMask(myWorld, vcpu->actionWakeupMask)) {
         doWait = FALSE;
         vcpu->actionWakeupMask = 0;
         CpuSchedVcpuActionNotifyRequest(vcpu, FALSE);
         ASSERT(waitType < CPUSCHED_NUM_WAIT_STATES);
         vcpu->stats.actionPreventWait[waitType]++;
      }
   }
   SP_UnlockIRQ(&vcpu->actionWakeupLock, awIRQL);

   if (doWait) {
      // update global time
      CpuSchedCellUpdateTime(vsmp->cell);

      // update state
      CpuSchedVcpuSetRunState(vcpu, CPUSCHED_WAIT);
      CpuSchedVcpuSetWaitState(vcpu, waitType, event);

      // add world as waiter on event queue
      EventQueue_Insert(eventQueue, myWorld);
   }

   // release event queue lock
   EventQueue_Unlock(eventQueue, CPUSCHED_IRQL);

   // release caller lock here, not earlier (prevent wait/wakeup race)
   if (lockIRQ != NULL) {
      SP_UnlockIRQSpecial(lockIRQ, CPUSCHED_IRQL);
   }   
   if (lock != NULL) {
      SP_UnlockSpecial(lock);
   }

   if (doWait) {
      // reschedule w/o redundantly updating time
      CpuSchedDispatch(prevIRQL, FALSE);
   } else {
      // release cell lock
      CpuSchedVsmpCellUnlock(vsmp, prevIRQL);
   }

   if (UNLIKELY(MY_RUNNING_WORLD->deathPending) && 
       (MY_RUNNING_WORLD->killLevel == WORLD_KILL_DEMAND)) {
      return VMK_DEATH_PENDING;
   } else {
      return VMK_OK;
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedSetDirectedYield --
 *
 *      Simple helper function to store vcpu corresponding to "directedYield"
 *      in the directedYield field of MY_PCPU;
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Updates "directedYield" field of current pcpu.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE void
CpuSchedSetDirectedYield(World_ID directedYield)
{
   World_Handle *yieldWorld = World_Find(directedYield);

   if (yieldWorld != NULL) {
      CpuSchedPcpu(MY_PCPU)->directedYield = World_CpuSchedVcpu(yieldWorld);
      World_Release(yieldWorld);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedDoWaitDirectedYield --
 *
 *     Works like the standard CpuSched_Wait function, but tries to ensure
 *     that the world "directedYield" is scheduled next on the current pcpu.
 *     Can be called with both SP_SpinLock "lock" and SP_SpinLockIRQ "lockIRQ"
 *     held.
 *
 * Results:
 *      CpuSchedWait() return status
 *
 * Side effects:
 *       Puts current world to sleep waiting for "event", modifies scheduler
 *       state, may run the "directedYield" world.
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
CpuSchedDoWaitDirectedYield(uint32 event,
                            CpuSched_WaitState waitType,	
                            uint32 actionWakeupMask,
                            SP_SpinLockIRQ *lockIRQ,
                            SP_SpinLock *lock,
                            World_ID directedYield,
                            SP_IRQL callerPrevIRQL)
{
   CpuSched_Vsmp *vsmp = World_CpuSchedVsmp(MY_RUNNING_WORLD);   
   EventQueue *eventQueue;
   SP_IRQL prevIRQL;
   
   // should use IsSafeToBlock, but COS world calls this from Host_Idle in
   // a safe manner. Checking IsIDLEWorld as well as that is checked inside
   // IsSafeToBlock
   
   if (lockIRQ !=NULL) {
      ASSERT(!World_IsIDLEWorld(MY_RUNNING_WORLD));
      ASSERT(World_IsSafeToDescheduleWithLock(NULL, lockIRQ));
   }
   if (lock != NULL) {
      ASSERT(World_IsSafeToBlockWithLock(lock,NULL));
   }
   // acquire locks
   eventQueue = EventQueue_Find(event);
   prevIRQL = EventQueue_Lock(eventQueue);
   (void) CpuSchedVsmpCellLock(vsmp);

   
   CpuSchedSetDirectedYield(directedYield);
   
   if (lockIRQ != NULL) {
      // n.b. unlock performed in CpuSchedWait()
      prevIRQL = callerPrevIRQL;
   }

   // invoke primitive (n.b. eventQueue and cpuSched unlocked internally)
   return CpuSchedWait(eventQueue, event, waitType, actionWakeupMask, lockIRQ, lock, prevIRQL);   
}
/*
 *-----------------------------------------------------------------------------
 *
 * CpuSched_WaitDirectedYield --
 *
 *      Works like the standard CpuSched_Wait function, but tries to ensure
 *      that the world "directedYield" is scheduled next on the current pcpu.
 *	Also, will wakeup when an action in "actionWakeupMask" is pending.
 *
 *      A directed yield is simply a strong hint to the scheduler, so there
 *      are many reasons why it may fail. The yieldVcpu may be running already
 *      or not currently runnable, its affinity settings may disallow the
 *      current pcpu, or it may be in a remote cell whose lock we are unable
 *      to acquire.
 *
 *      Note that the scheduler quantum is shared between the yielding vcpu
 *      and the destination vcpu to prevent processor monopolization when
 *      two worlds rapidly yield back-and-forth. If a vcpu tries to do a
 *      directed yield when its quantum has expired, the yield hint will be
 *      ignored.
 *
 * Results:
 *      CpuSchedDoWaitDirectedYield return value.
 *
 * Side effects:
 *      Puts current world to sleep waiting for "event", modifies scheduler
 *      state, may run the "directedYield" world.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
CpuSched_WaitDirectedYield(uint32 event,
                           CpuSched_WaitState waitType,
                           uint32 actionWakeupMask,
                           SP_SpinLock *lock,
                           World_ID directedYield)
{
   return CpuSchedDoWaitDirectedYield(event, waitType, actionWakeupMask, NULL,
                               lock, directedYield, SP_IRQL_NONE);
}


/*
 *----------------------------------------------------------------------
 *
 * CpuSched_Wait --
 *
 *      The running world is put to sleep pending a wakeup on "event".
 *	Note that spurious wakeups are possible.  If "lock" is not NULL,
 *	then it is released before the world is put to sleep.
 *
 * Results:
 *      CpuSched_WaitDirectedYield return value
 *
 * Side effects:
 *      Modifies global scheduler state.
 *	Modifies event table, updating wait queue for "event".
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
CpuSched_Wait(uint32 event,
              CpuSched_WaitState waitType,
              SP_SpinLock *lock)
{
   return CpuSched_WaitDirectedYield(event, waitType, 0, lock, INVALID_WORLD_ID);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedSleepTimeout --
 *
 *      Callback for sleep timer timeout.  Performs wakeup on
 *	event encoded by "data".
 *
 * Results:
 *      None. 
 *
 * Side effects:
 *      Modifies global scheduler state.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedSleepTimeout(void *data, UNUSED_PARAM(Timer_AbsCycles timestamp))
{
   CpuSched_Wakeup((uint32) data);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_TimedWait --
 *
 *      The running world is put to sleep pending a wakeup on "event".
 *	or expiration of the specified timeout.  Note that spurious 
 *      wakeups are possible.  If "lock" is not NULL, then it is
 *	released before the world is put to sleep.
 *
 * Results:
 *      CpuSchedWait return value.
 *
 * Side effects:
 *      Modifies global scheduler state.
 *	Modifies event table, updating wait queue for "event".
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
CpuSched_TimedWait(uint32 event,
                   CpuSched_WaitState waitType,
 		   SP_SpinLock *lock,
 		   uint32 msecs)
{
   VMK_ReturnStatus status;
   Timer_Handle th;
 
   th = Timer_Add(MY_PCPU, CpuSchedSleepTimeout, msecs, TIMER_ONE_SHOT,
 		  (void *)event);
   status = CpuSched_Wait(event, waitType, lock);
   Timer_RemoveSync(th);

   return status;
}
 

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSched_WaitIRQDirectedYield --
 *
 *      Same as CpuSched_WaitIRQ (below) but tries to ensure that
 *	"directedYield" is scheduled next on the current processor.
 *	Also, will wakeup when an action in "actionWakeupMask" is pending.
 *	See CpuSched_WaitDirectedYield header doc for caveats.
 *
 * Results:
 *
 *      CpuSchedDoWaitDirectedYield return value
 *
 *
 * Side effects:
 *      Modifies scheduler state, puts current world to sleep.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
CpuSched_WaitIRQDirectedYield(uint32 event,
                              CpuSched_WaitState waitType,	
                              uint32 actionWakeupMask,
                              SP_SpinLockIRQ *lockIRQ,
                              SP_IRQL callerPrevIRQL,
                              World_ID directedYield)
{
   return CpuSchedDoWaitDirectedYield(event, waitType, actionWakeupMask,
                                      lockIRQ,
                                      NULL, directedYield, callerPrevIRQL);
}
                              
/*
 *----------------------------------------------------------------------
 *
 * CpuSched_WaitIRQ --
 *
 *      The running world is put to sleep pending a wakeup on "event".
 *      If "lockIRQ" is not NULL, then it is released before the world
 *      is put to sleep.
 *
 * Results:
 *      CpuSchedWait return value.
 *
 * Side effects:
 *      Modifies global scheduler state.
 *	Modifies event table, updating wait queue for "event".
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
CpuSched_WaitIRQ(uint32 event,
                 CpuSched_WaitState waitType,	
                 SP_SpinLockIRQ *lockIRQ,
                 SP_IRQL callerPrevIRQL)
{
   return CpuSched_WaitIRQDirectedYield(event,
                                        waitType,
                                        0,
                                        lockIRQ,
                                        callerPrevIRQL,
                                        INVALID_WORLD_ID);
}


/*
 *----------------------------------------------------------------------
 *
 * CpuSchedRWWait --
 *      Similar to CpuSchedWait(), but works with reader/writer locks
 *      "rwlock" and "rwlockIRQ".  (See PR #47980)
 *
 * Results:
 *    VMK_DEATH_PENDING if the world is supposed to die, VMK_OK otherwise
 *
 * Side effects:
 *    	Updates "eventQueue", adding current world.
 *      Updates cell time
 *	Releases "lock" and "lockIRQ", if any.
 *	Releases "eventQueue" lock.
 *      Modifies currently-executing world.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
CpuSchedRWWait(EventQueue *eventQueue,
               uint32 event,
               CpuSched_WaitState waitType,
               SP_RWLockIRQ *rwlockIRQ,
               CpuSched_RWWaitLockType rwlockIRQType,
               SP_RWLock *rwlock,
               CpuSched_RWWaitLockType rwlockType,
               SP_IRQL prevIRQL)
         
{
   World_Handle *myWorld = MY_RUNNING_WORLD;
   CpuSched_Vcpu *vcpu;
   CpuSched_Vsmp *vsmp;

   // sanity checks
   ASSERT(myWorld != NULL);
   ASSERT(EventQueue_IsLocked(eventQueue));

   // convenient abbrevs
   vcpu = World_CpuSchedVcpu(myWorld);
   vsmp = vcpu->vsmp;

   // debugging
   VcpuLogEvent(vcpu, "wait");

   // sanity checks
   ASSERT(CpuSchedVcpuRunOrBwait(vcpu));
   ASSERT(CpuSchedVsmpCellIsLocked(vsmp));

   // can't put idle world to sleep
   ASSERT(!vcpu->idle);

   // update global time
   CpuSchedCellUpdateTime(vsmp->cell);

   // update state
   CpuSchedVcpuSetRunState(vcpu, CPUSCHED_WAIT);
   CpuSchedVcpuSetWaitState(vcpu, waitType, event);

   // add world as waiter on event queue, release lock
   EventQueue_Insert(eventQueue, myWorld);
   EventQueue_Unlock(eventQueue, CPUSCHED_IRQL);

   // release caller lock here, not earlier (prevent wait/wakeup race)
   if (rwlockIRQ != NULL) {
      ASSERT(rwlockIRQType != CPUSCHED_RWWAIT_NONE);
      //check whether lock was write locked or read locked
      if (rwlockIRQType == CPUSCHED_RWWAIT_WRITE) {
         SP_RelWriteLockIRQ(rwlockIRQ,CPUSCHED_IRQL);
      } else {
         SP_RelReadLockIRQ(rwlockIRQ,CPUSCHED_IRQL);
      }
   }   
   if (rwlock != NULL) {
      ASSERT(rwlockType != CPUSCHED_RWWAIT_NONE);
      if (rwlockType == CPUSCHED_RWWAIT_WRITE) {
         SP_RelWriteLockSpecial(rwlock);
      } else {
         SP_RelReadLockSpecial(rwlock);
      }
      
   }
   
   // reschedule w/o redundantly updating time
   CpuSchedDispatch(prevIRQL, FALSE);
   
   if (UNLIKELY(MY_RUNNING_WORLD->deathPending) && 
       (MY_RUNNING_WORLD->killLevel == WORLD_KILL_DEMAND)) {
      return VMK_DEATH_PENDING;
   } else {
      return VMK_OK;
   }
}
/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedDoRWWait --
 *       The running world is put to sleep pending a wakeup on event.
 *       The locks "rwlockIRQ" and "rwlock" are reader/writer locks acquired
 *       by the world. They are released before the world is put to sleep.
 *       (See PR #47980)
 *
 * Results:
 *      CpuSchedRWWait() return status
 *
 * Side effects:
 *      Puts current world to sleep waiting for "event", modifies scheduler
 *      state, may run the "directedYield" world.
 *
 *-----------------------------------------------------------------------------
 */

static VMK_ReturnStatus
CpuSchedDoRWWait(uint32 event,
                 CpuSched_WaitState waitType,
                 SP_RWLockIRQ *rwlockIRQ,
                 CpuSched_RWWaitLockType rwlockIRQType,
                 SP_RWLock *rwlock,
                 CpuSched_RWWaitLockType rwlockType,
                 SP_IRQL callerPrevIRQL)
     
{
   CpuSched_Vsmp *vsmp = World_CpuSchedVsmp(MY_RUNNING_WORLD);   
   EventQueue *eventQueue;
   SP_IRQL prevIRQL;

   // if there is a lock, then its type should not be CPUSCHED_RWWAIT_NONE
   ASSERT( (!rwlockIRQ) || (rwlockIRQType != CPUSCHED_RWWAIT_NONE));

   // check if "rwlockIRQ" is write locked
   if (rwlockIRQ && rwlockIRQType == CPUSCHED_RWWAIT_WRITE){
      ASSERT(World_IsSafeToBlockWithLock(NULL,&rwlockIRQ->write));
   }

   // if there is a lock, then its type should not be CPUSCHED_RWWAIT_NONE
   ASSERT( (!rwlock) || (rwlockType != CPUSCHED_RWWAIT_NONE));

   // check if "rwlock" is write locked
   if (rwlock && rwlockType == CPUSCHED_RWWAIT_WRITE) {
      ASSERT(World_IsSafeToBlockWithLock(&rwlock->write, NULL));
   }

   // acquire locks
   eventQueue = EventQueue_Find(event);
   prevIRQL = EventQueue_Lock(eventQueue);
   (void) CpuSchedVsmpCellLock(vsmp);

   if (rwlockIRQ != NULL) {
      prevIRQL = callerPrevIRQL;
   }
   
   // invoke primitive (n.b. eventQueue and cpuSched unlocked internally)
   return CpuSchedRWWait(eventQueue, event, waitType,
                         rwlockIRQ, rwlockIRQType, rwlock,
                         rwlockType, prevIRQL);
}



/*
 *----------------------------------------------------------------------
 *
 * CpuSched_RWWait --
 *
 *      Similar to  CpuSched_Wait, but takes a reader/writer lock "rwlock"
 *      instead of a standard spinlock. 
 *      (See PR #47980)
 *
 * Results:
 *      CpuSchedDoRWWait() return status
 *
 * Side effects:
 *      Modifies global scheduler state.
 *	Modifies event table, updating wait queue for "event".
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
CpuSched_RWWait(uint32 event,
                CpuSched_WaitState waitType,
                SP_RWLock *rwlock,
                CpuSched_RWWaitLockType rwlockType)
{
   return CpuSchedDoRWWait(event, waitType,
                           NULL, CPUSCHED_RWWAIT_NONE,
                           rwlock, rwlockType,
                           SP_IRQL_NONE);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_TimedRWWait --
 *      Similar to CpuSched_TimedWait but takes a reader/writer lock
 *      "rwlock" instead of a standard spinlock.
 *      (See PR #47980)
 *
 * Results:
 *      CpuSched_RWWait() return status
 *
 * Side effects:
 *      Modifies global scheduler state.
 *	Modifies event table, updating wait queue for "event".
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
CpuSched_TimedRWWait(uint32 event, CpuSched_WaitState waitType,
                     SP_RWLock *rwlock,
                     CpuSched_RWWaitLockType rwlockType,
                     uint32 msecs)
{
   Timer_Handle th;
   VMK_ReturnStatus status;
   th = Timer_Add(MY_PCPU, CpuSchedSleepTimeout, msecs, TIMER_ONE_SHOT,
 		  (void *)event);
 
   status = CpuSched_RWWait(event, waitType,
                            rwlock, rwlockType);
 
   Timer_RemoveSync(th);

   return status;
}

/*
 *------------------------------------------------------------------------
 *
 * CpuSched_RWWaitIRQ --
 *
 *      Similar to  CpuSched_WaitIRQ, but takes a reader/writer lock
 *      "rwlockIRQ" instead of a standard spinlock.
 *      (See PR #47980)
 *
 * Results:
 *      CpuSchedDoRWWait() return status 
 *
 * Side effects:
 *      Modifies global scheduler state.
 *	Modifies event table, updating wait queue for "event".
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
CpuSched_RWWaitIRQ(uint32 event,
                   CpuSched_WaitState waitType,
                   SP_RWLockIRQ *rwlockIRQ,
                   CpuSched_RWWaitLockType rwlockIRQType,
                   SP_IRQL callerPrevIRQL)
{
   return CpuSchedDoRWWait(event, waitType,
                           rwlockIRQ, rwlockIRQType,
                           NULL, CPUSCHED_RWWAIT_NONE,
                           callerPrevIRQL);
}
/*
 *----------------------------------------------------------------------
 *
 * CpuSched_Sleep --
 *
 *      The running world is put to sleep, and will be awakened
 *	automatically after "msec" milliseconds.
 *
 * Results:
 *      CpuSchedWait return value
 *
 * Side effects:
 *      Modifies global scheduler state.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
CpuSched_Sleep(uint32 msec)
{
   CpuSched_Vcpu *vcpu;
   uint64 now, target;

   // debugging
   CpuSchedLogEvent("sleep", msec);

   // sanity check
   ASSERT(MY_RUNNING_WORLD != NULL);

   vcpu = World_CpuSchedVcpu(MY_RUNNING_WORLD);   
   now = Timer_SysUptime();
   target = now + msec;

   while (now < target) {
      SP_IRQL prevIRQL;
      Timer_Handle th;
      VMK_ReturnStatus status;

      // n.b. lock prevents timer from firing prior to wait
      prevIRQL = SP_LockIRQ(&vcpu->sleepLock, SP_IRQL_KERNEL);
      th = Timer_Add(MY_PCPU, CpuSchedSleepTimeout, target - now,
                     TIMER_ONE_SHOT, (void *) vcpu->sleepEvent);
      status = CpuSched_WaitIRQ(vcpu->sleepEvent, CPUSCHED_WAIT_SLEEP, 
                                &vcpu->sleepLock, prevIRQL);
      Timer_Remove(th);
      if (status != VMK_OK) {
         return status;
      }

      now = Timer_SysUptime();
   }

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedYield --
 *
 *      Invokes the scheduler, choosing the next world to run on
 *	the current processor.	Note that the currently-executing
 *	world may be selected to run for another quantum.
 *
 *      Because this function will grab the scheduler lock, you should not
 *      call it repeatedly from a tight loop. Use CpuSched_YieldThrottled
 *      for that purpose.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Modifies scheduler state, currently-executing world.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedYield(void)
{
   CpuSched_Vsmp *myVsmp = World_CpuSchedVsmp(MY_RUNNING_WORLD);   
   SP_IRQL prevIRQL;

   ASSERT(World_IsSafeToDeschedule());

   // debugging
   CpuSchedLogEvent("exp-yield", MY_RUNNING_WORLD->worldID);

   // invoke primitive
   prevIRQL = CpuSchedVsmpCellLock(myVsmp);
   CpuSchedDispatch(prevIRQL, TRUE);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSched_YieldThrottled --
 *
 *      Same as CpuSched_Yield (above) but with built in throttling so that
 *      this function can be safely called from within a tight loop.
 *      If we have recently gone through a YieldThrottled, this function
 *      simply becomes a no-op.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May modify scheduler state, currently-executing world.
 *
 *-----------------------------------------------------------------------------
 */
void
CpuSched_YieldThrottled(void)
{
   Bool preempt, doYield = FALSE;
   CpuSched_Pcpu *pcpu;
   TSCCycles now;
   
   // don't want to migrate to a different processor
   preempt = CpuSched_DisablePreemption();
   
   // no need for Timer_GetCycles since this is pcpu-local
   now = RDTSC();
   pcpu = CpuSchedPcpu(MY_PCPU);
   if (now > pcpu->lastYieldTime + pcpu->cell->config.yieldThrottleTSC) {
      // enough time has passed since the last yield
      pcpu->lastYieldTime = now;
      doYield = TRUE;
   }
   CpuSched_RestorePreemption(preempt);
   
   if (doYield) {
      CpuSchedYield();
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_YieldToHost --
 *
 *      If the running world is not the console and is executing on
 *	the console processor, invokes the scheduler via
 *	CpuSchedYield().  Otherwise has no effect.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	May modify currently-executing world, scheduler state.
 *
 *----------------------------------------------------------------------
 */
void 
CpuSched_YieldToHost(void)
{
   // OPT: consider eliminating, only call site is Host_Broken()
   if ((MY_PCPU == CONSOLE_PCPU) &&
       (MY_RUNNING_WORLD != NULL) &&
       (MY_RUNNING_WORLD != CONSOLE_WORLD)) {
      CpuSchedYield();
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_StartWorld --
 *
 *      Startup function for a world.
 *      This function is declared with __attribute__((regparm(1)))
 *      (see cpusched.h).
 *
 *      Either by luck or because the GNU folks are incredibly
 *      smart (hi Richard), the first argument of a function
 *      declared with __attribute__ ((regparms(1))) is passed
 *      in %eax.  This is also the register where return values
 *      are placed.  The WorldDoSwitch assembly code places a
 *      pointer to the previous world in %eax.  When WorldDoSwitch
 *      returns to World_Switch, the return value is the previous
 *      world.  When it "returns" to CpuSched_StartWorld, the
 *      first argument is the previous world.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Enables interrupts.
 *
 *----------------------------------------------------------------------
 */
void
CpuSched_StartWorld(World_Handle *previous)
{
   World_Handle *current = MY_RUNNING_WORLD;
   uint32 eflags;

   // sanity checks
   ASSERT(current != NULL);
   ASSERT(current->sched.cpu.startFunc != NULL);
   ASSERT(current->sched.cpu.vcpu.pcpu == MY_PCPU);
   ASSERT(!CpuSched_IsPreemptible());

   /*
    * new worlds inherit previous world's eflags, but we don't want the NT
    * flag, otherwise iret will try to do a task switch.
    */
   SAVE_FLAGS(eflags);
   if(eflags & EFLAGS_NT) {
      eflags &= ~EFLAGS_NT;
      RESTORE_FLAGS(eflags);
   }

   NMI_Enable();

   /*
    * World is running, no longer safe to read registers (without jumping 
    * through the hoops in World_Panic).
    */
   current->okToReadRegs = FALSE;

   // run post-switch code
   CpuSchedAfterSwitch(World_CpuSchedVcpu(previous));

   // debugging support
   Watchpoint_WorldInit(current);
   Watchpoint_Enable(FALSE);

   // start with interrupts enabled (first yield to world)
   SP_RestoreIRQ(SP_IRQL_NONE);

   if (World_IsVMMWorld(current)) {
      VMK_MonitorInitArgs vmkArgs;
      /* 
       * Must enter vmm world with interrupts disabled. Cannot use 
       * CLEAR_INTERRUPTS() because it does a builtin_return_address, which
       * really confuses gcc at this point (because we don't have a caller).
       */
      __asm__ __volatile__ ("cli");
      vmkArgs.call = VMKCall;
      vmkArgs.stackTop = World_GetVMKStackTop(current);
      vmkArgs.vmkIDTPTE = IDT_GetVMKIDTPTE();
      IDT_GetDefaultIDT(&vmkArgs.vmkIDTR);
      VMLOG(0, current->worldID, "VMK IDT offset = 0x%x, pte = %Lx,"
            " stackTop = 0x%x", vmkArgs.vmkIDTR.offset, 
            vmkArgs.vmkIDTPTE, vmkArgs.stackTop);
      vmkArgs.vmkCR3 = current->savedState.CR[3];
      vmkArgs.worldID = current->worldID;
      
      // enter monitor with NMIs disabled
      NMI_Disable();
      ASSERT(current->nmisInMonitor == FALSE);
      
      // enter monitor with preemption enabled
      CpuSched_EnablePreemption();

      // prepare monitor arguments
      current->sched.cpu.startData = &vmkArgs;
   }

   if (World_IsVMMWorld(current) || World_IsUSERWorld(current)) {
      // initialize pseudo-TSC parameters
      Timer_UpdateWorldPseudoTSCConv(current, Timer_GetCycles());
      if (cpuSchedConst.numaSystem) {
         current->pseudoTSCTimer =
            Timer_Add(MY_PCPU, CpuSchedWorldPseudoTSCConvCB,
                      PSEUDO_TSC_TIMER_PERIOD_MS, TIMER_PERIODIC,
                      (void *) current->worldID);
      }
   }

   // invoke world's start function
   (*current->sched.cpu.startFunc)(current->sched.cpu.startData);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_Die --
 *
 *      Deschedule the current world by changing its state to zombie.
 *	This world will never be run again.  
 *
 *	Should only be called by World_Exit().
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Run state is set to SCHED_ZOMBIE.
 *
 *----------------------------------------------------------------------
 */
void
CpuSched_Die(void)
{
   SP_IRQL schedIRQL, eventIRQL;
   EventQueue *eventQueue;
   CpuSched_Vcpu *myVcpu;
   World_Handle *myWorld;

   // convenient abbrevs
   myWorld = MY_RUNNING_WORLD;
   myVcpu = World_CpuSchedVcpu(myWorld);

   // acquire necessary locks
   schedIRQL = CpuSchedVcpuEventLock(myVcpu, &eventQueue, &eventIRQL);
   
   // sanity checks
   ASSERT(CpuSchedVcpuRunOrBwait(myVcpu));

   // debugging
   VCPULOG(1, myVcpu, "zombifying");

   // wakeup if busy-waiting
   if (myVcpu->runState == CPUSCHED_BUSY_WAIT) {
      if (EventQueue_Remove(eventQueue, myWorld)) {
         CpuSchedVcpuWakeup(myVcpu);
      }
   }

   // release eventQueue lock, if any
   //   but keep scheduler lock w/o re-enabling interrupts
   if (eventQueue != NULL) {
      EventQueue_Unlock(eventQueue, CPUSCHED_IRQL);
   }

   // zombify world
   CpuSchedVcpuSetRunState(myVcpu, CPUSCHED_ZOMBIE);

   // remove from scheduler
   CpuSchedRemoveInt(myWorld, NULL);

   // yield processor
   CpuSchedDispatch(schedIRQL, TRUE);

   // convince compiler that we really don't return;
   // use Panic() since NOT_REACHED() not in all build types
   Panic("CpuSched: Die: unexpected return\n");
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedAddFirstVcpu --
 *
 *      Finish adding "world" to CPU scheduler, with an initial
 *      allocation of "config->shares"
 * 	Requires "world" is vsmp leader (i.e. first vcpu in vsmp).
 *	Caller must hold all scheduler cell locks.
 *
 * Results:
 *      Updates scheduler state, "world" scheduling state.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedAddFirstVcpu(World_Handle *world,
                     const Sched_CpuClientConfig *config,
                     Bool running)
{
   CpuSched_Vcpu *vcpu = World_CpuSchedVcpu(world);
   CpuSched_Vsmp *vsmp = World_CpuSchedVsmp(world);
   CpuSched_Pcpu *schedPcpu = CpuSchedPcpu(vcpu->pcpu);
   CpuSched_Cell *cell = schedPcpu->cell;
   CpuSched_Alloc base;

   // sanity checks
   ASSERT(CpuSched_IsVsmpLeader(world));
   ASSERT(CpuSchedAllCellsAreLocked());

   // setup per-vsmp locking
   SP_InitLockIRQ("vsmp-members", &vsmp->vcpuArrayLock, SP_RANK_IRQ_LEAF);
   
   // initialize vsmp state
   vsmp->numa.homeNode = INVALID_NUMANODE;

   // add to list of VCPUs associated with VSMP
   CpuSchedVcpuArrayAdd(vsmp, vcpu);

   // add to list of cell's managed vsmps
   CpuSched_VsmpArrayAdd(&cell->vsmps, vsmp);

   // initialize coscheduling state
   //   uniprocessor vsmp, or first vcpu in multiprocessor vsmp
   CpuSchedVsmpSetState(vsmp, CPUSCHED_CO_NONE);

   if (running) {
      // sanity checks
      ASSERT(World_VcpuToWorld(vcpu) == MY_RUNNING_WORLD);
      ASSERT(!CpuSchedIsMP(vsmp));

      // already running
      CpuSchedVcpuSetRunState(vcpu, CPUSCHED_RUN);
      ASSERT(vsmp->nRun > 0);
      CpuSchedVcpuQuantumStart(vcpu, NULL);
   } else {
      // ready, add to run queue
      CpuSchedVcpuMakeReady(vcpu);
   }

   // validate vsmp group associatation
   CpuSchedVsmpUpdateGroup(vsmp);

   // initialize internal base allocation
   //   guarantee minimal initial 1% allocation until next realloc
   CpuSchedAllocInit(&base,
                     CpuSchedUnitsToBaseShares(1, SCHED_UNITS_PERCENT),
                     CpuSchedUnitsToBaseShares(100, SCHED_UNITS_PERCENT),
                     SCHED_UNITS_BSHARES,
                     CpuSchedUnitsToBaseShares(1, SCHED_UNITS_PERCENT));
   CpuSchedVsmpSetBaseAlloc(vsmp, &base);

   // initialize virtual time
   if (vcpu->idle) {
      CpuSchedVtimeContextSetInfinite(&vsmp->vtime);
      vsmp->vtimeLimit = 0;
   } else {
      vsmp->vtime.main = cell->vtime;
      vsmp->vtime.extra = cell->vtime;
      vsmp->vtimeLimit = cell->vtime;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedAddSecondVcpu --
 *
 *      Finish adding "world" to CPU scheduler, with an initial
 *      allocation of "config->shares"
 * 	Requires "world" is the second vcpu in its vsmp.
 *	Caller must hold all scheduler cell locks.
 *
 * Results:
 *      Updates scheduler state, "world" scheduling state.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedAddSecondVcpu(World_Handle *world,
                      const Sched_CpuClientConfig *config)
{
   CpuSched_Vcpu *vcpu = World_CpuSchedVcpu(world);
   CpuSched_Vsmp *vsmp = World_CpuSchedVsmp(world);
   CpuSched_Vcpu *vcpu0;

   // sanity checks
   ASSERT(!CpuSched_IsVsmpLeader(world));
   ASSERT(vsmp->coRunState == CPUSCHED_CO_NONE);
   ASSERT(vsmp->vcpus.len == 1);
   ASSERT(vcpu != NULL);
   ASSERT(CpuSchedAllCellsAreLocked());

   // add to list of VCPUs associated with VSMP
   CpuSchedVcpuArrayAdd(vsmp, vcpu);

   // add second vcpu in multiprocessor vsmp
   ASSERT(vsmp->vcpus.len == 2);
   vcpu0 = vsmp->vcpus.list[0];

   if (vsmp->nWait > 0) {
      // first vcpu already waiting => start in CO_RUN state
      ASSERT(vsmp->nWait == 1);
      VCPULOG(0, vcpu, "already nWait=%d => CO_RUN", vsmp->nWait);

      ASSERT(vcpu0 != NULL);
      ASSERT(CpuSchedVcpuIsWaiting(vcpu0));
      if (CpuSchedWaitStateDisablesCoDesched(vcpu0->waitState)) {
         vsmp->disableCoDeschedule++;
         VCPULOG(0, vcpu, "wait=%s, disableCoDeschedule=%d",
                 CpuSchedWaitStateName(vcpu0->waitState),
                 vsmp->disableCoDeschedule);
         ASSERT(vsmp->disableCoDeschedule == 1);
      }

      CpuSchedVcpuMakeReady(vcpu);
      CpuSchedVsmpSetState(vsmp, CPUSCHED_CO_RUN);
   } else if (vsmp->nRun > 0 || vcpu0->runState == CPUSCHED_READY_CORUN) {
      // first vcpu already running => start in CO_RUN state
      VCPULOG(0, vcpu, "already nRun=%d => CO_RUN", vsmp->nRun);

      CpuSchedVcpuMakeReady(vcpu);
      CpuSchedVsmpSetState(vsmp, CPUSCHED_CO_RUN);
   } else {
      // first vcpu ready => start in CO_READY state
      ASSERT((vsmp->nRun == 0) && (vsmp->nWait == 0));
      VCPULOG(0, vcpu, "nRun=%d, nWait=%d => CO_READY",
              vsmp->nRun, vsmp->nWait);
      
      CpuSchedVcpuMakeReady(vcpu);
      CpuSchedVsmpSetState(vsmp, CPUSCHED_CO_READY);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedAddNthVcpu --
 *
 *      Finish adding "world" to CPU scheduler, with an initial
 *      allocation of "config->shares"
 * 	Requires "world" is the Nth vcpu in its vsmp, where N > 2.
 *	Caller must hold all scheduler cell locks.
 *
 * Results:
 *      Updates scheduler state, "world" scheduling state.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedAddNthVcpu(World_Handle *world,
                   const Sched_CpuClientConfig *config)
{
   CpuSched_Vcpu *vcpu = World_CpuSchedVcpu(world);
   CpuSched_Vsmp *vsmp = World_CpuSchedVsmp(world);

   // sanity checks
   ASSERT(!CpuSched_IsVsmpLeader(world));
   ASSERT(vsmp->coRunState != CPUSCHED_CO_NONE);
   ASSERT(vsmp->vcpus.len > 1);
   ASSERT(CpuSchedAllCellsAreLocked());

   // add to list of VCPUs associated with VSMP
   CpuSchedVcpuArrayAdd(vsmp, vcpu);

   // add Nth vcpu in existing multiprocessor vsmp
   ASSERT(vsmp->vcpus.len > 2);
   switch (vsmp->coRunState) {
      case CPUSCHED_CO_RUN:
      case CPUSCHED_CO_READY:
         // make ready, place on queue
         CpuSchedVcpuMakeReady(vcpu);
         break;

      case CPUSCHED_CO_STOP:
         // make ready but co-descheduled, keep off queue
         CpuSchedVcpuSetRunState(vcpu, CPUSCHED_READY_COSTOP);
         break;

      case CPUSCHED_CO_NONE:
      default:
         NOT_REACHED();
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedVcpuInitialPlacement --
 *
 *     Determines the pcpu on which vcpu should start running.
 *     Caller must hold scheduler cell lock for "vcpu".
 *
 * Results:
 *     Returns a valid pcpu placement for "vcpu" or INVALID_PCPU
 *     if the vcpu could not be placed.
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static PCPU
CpuSchedVcpuInitialPlacement(const CpuSched_Vcpu* vcpu)
{
   CpuSched_Cell *cell = vcpu->vsmp->cell;
   uint32 start, i;

   // sanity check
   ASSERT(CpuSchedCellIsLocked(cell));

   // find first acceptable cell pcpu, starting at random point
   start = CpuSchedRandom() % cell->nPCPUs;
   for (i = 0; i < cell->nPCPUs; i++) {
      PCPU p = cell->pcpu[(start + i) % cell->nPCPUs];
      if (CpuSchedVcpuAffinityPermitsPcpu(vcpu, p, 0)) {
         return(p);
      }
   }

   // no valid placement
   return(INVALID_PCPU);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedAffinityPermitsCell --
 *
 *	Determine whether a VM with "nVcpus" can be coscheduled in "cell",
 *	given the per-vcpu affinity specified by "vcpuMasks".
 *
 * Results:
 *	Returns TRUE iff VM with "nVcpus" and "vcpuMasks" constraints
 *	can be coscheduled in "cell".
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */
static Bool
CpuSchedAffinityPermitsCell(const CpuSched_Cell *cell,
                            uint8 nVcpus,
                            const CpuMask *vcpuMasks)
{
   Bool jointAffinity = TRUE;
   uint32 v;

   // check if each vcpu can run on some pcpu in cell
   for (v = 0; v < nVcpus; v++) {
      // fail if vcpu affinity doesn't permit cell
      if ((cell->pcpuMask & vcpuMasks[v]) == 0) {
         return(FALSE);
      }

      // all masks identical => joint affinity
      if (vcpuMasks[v] != vcpuMasks[0]) {
         jointAffinity = FALSE;
      }
   }

   // check if sufficient pcpus in cell to coschedule all vcpus
   if (jointAffinity) {
      uint8 bitsSet = Util_BitPopCount(vcpuMasks[0] & cell->pcpuMask);
      if (bitsSet < nVcpus) {
         return(FALSE);
      }
   }

   return(TRUE);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedCellInitialPlacement --
 *
 *	Determines the cell on which a new vcpu with scheduling
 *	parameters specified by "config" should start running.
 *
 * Results:
 *	Returns a valid cell placement for "config", or NULL
 *	if no placement was possible.
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */
static CpuSched_Cell *
CpuSchedCellInitialPlacement(const Sched_CpuClientConfig *config)
{
   CpuSched_Cell *bestCell = NULL;
   uint32 start, i;

   // sanity checks
   ASSERT(CpuSchedAllCellsAreLocked());
   ASSERT(cpuSched.nCells > 0);

   // Initial placement isn't terribly important since vcpus
   // migrate dynamically.  Simply find first acceptable cell,
   // starting at random point, and try to avoid console cell.
   start = CpuSchedRandom() % cpuSched.nCells;
   for (i = 0; i < cpuSched.nCells; i++) {
      uint32 id = (start + i) % cpuSched.nCells;
      CpuSched_Cell *cell = &cpuSched.cell[id];

      if (CpuSchedAffinityPermitsCell(cell,
                                      config->numVcpus,
                                      config->vcpuAffinity)) {
         if ((bestCell == NULL) || (bestCell == CONSOLE_CELL)) {
            bestCell = cell;
         }
      }
   }

   return(bestCell);
}

static void
CpuSchedInitHistograms(CpuSched_Vcpu *vcpu)
{
   Heap_ID groupHeap = World_VcpuToWorld(vcpu)->group->heap;
   int64 skewBucketLimits[] = {0, 1, 2, 3, 5, 10, 15, 20, 25, 50, 100 };

   vcpu->intraSkewHisto = Histogram_New(
      groupHeap,
      sizeof(skewBucketLimits) / sizeof(skewBucketLimits[0] + 1),
      skewBucketLimits);
   
   ASSERT_NOT_IMPLEMENTED(vcpu->intraSkewHisto != NULL);

   if (CPUSCHED_STATE_HISTOGRAMS) {
      int numBuckets = CPUSCHED_DEFAULT_NUM_HISTO_BUCKETS;
      int64 bucketLimits[CPUSCHED_DEFAULT_NUM_HISTO_BUCKETS - 1];
      CpuSched_WaitState w;
      CpuSched_RunState r;

      // initialize default bucket limits
      bucketLimits[0] = Timer_USToTC(2);
      bucketLimits[1] = Timer_USToTC(10);
      bucketLimits[2] = Timer_USToTC(30);
      bucketLimits[3] = Timer_USToTC(100);
      bucketLimits[4] = Timer_USToTC(300);
      bucketLimits[5] = Timer_USToTC(1000);
      bucketLimits[6] = Timer_USToTC(5000);
      bucketLimits[7] = Timer_USToTC(10000);
      bucketLimits[8] = Timer_USToTC(25000);
      bucketLimits[9] = Timer_USToTC(60000);
   
      // init run state histos
      for (r = 0; r < CPUSCHED_NUM_RUN_STATES; r++) {
         CpuSched_StateMeter *m = &vcpu->runStateMeter[r];
         m->histo = Histogram_New(groupHeap, numBuckets, bucketLimits);
         ASSERT(m->histo != NULL);
      }
   
      // init wait state meters
      for (w = 0; w < CPUSCHED_NUM_WAIT_STATES; w++) {
         CpuSched_StateMeter *m = &vcpu->waitStateMeter[w];
         m->histo = Histogram_New(groupHeap, numBuckets, bucketLimits);
         ASSERT(m->histo != NULL);
      }
      
      // init other histos
      vcpu->limboMeter.histo = Histogram_New(groupHeap, numBuckets, bucketLimits);
      vcpu->wakeupLatencyMeter.histo = Histogram_New(groupHeap, numBuckets, bucketLimits);
      vcpu->preemptTimeHisto = Histogram_New(groupHeap, numBuckets, bucketLimits);
      vcpu->runWaitTimeHisto = Histogram_New(groupHeap, numBuckets, bucketLimits);
      vcpu->disablePreemptTimeHisto = Histogram_New(groupHeap, numBuckets, bucketLimits);
   }
}


static void
CpuSchedDeleteHistograms(CpuSched_Vcpu *vcpu)
{
   Heap_ID groupHeap = World_VcpuToWorld(vcpu)->group->heap;
   if (CPUSCHED_STATE_HISTOGRAMS) {
      CpuSched_RunState r;
      CpuSched_WaitState w;

      // free run state histos
      for (r = 0; r < CPUSCHED_NUM_RUN_STATES; r++) {
         CpuSched_StateMeter *m = &vcpu->runStateMeter[r];
         Histogram_Delete(groupHeap, m->histo);
      }
   
      // free wait state meters
      for (w = 0; w < CPUSCHED_NUM_WAIT_STATES; w++) {
         CpuSched_StateMeter *m = &vcpu->waitStateMeter[w];
         Histogram_Delete(groupHeap, m->histo);
      }
      
      // free other histos
      Histogram_Delete(groupHeap, vcpu->limboMeter.histo);
      Histogram_Delete(groupHeap, vcpu->wakeupLatencyMeter.histo);
      Histogram_Delete(groupHeap, vcpu->preemptTimeHisto);
      Histogram_Delete(groupHeap, vcpu->runWaitTimeHisto);
      Histogram_Delete(groupHeap, vcpu->disablePreemptTimeHisto);
   }

   Histogram_Delete(groupHeap, vcpu->intraSkewHisto);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedAddInt --
 *
 *      Add "world", which may already be "running", to CPU scheduler,
 *	with initial configuration specified by "config".
 *	Caller must hold all scheduler cell locks.
 *
 * Results:
 *	Returns VMK_OK if successful, otherwise error code.
 *      Updates scheduler state, "world" scheduling state.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
CpuSchedAddInt(World_Handle *world,
               Sched_CpuClientConfig *config,
               Bool running)
{
   CpuSched_Vcpu *vcpu;
   CpuSched_Vsmp *vsmp;
   CpuSched_Alloc alloc;
   Bool jointAffinity;
   int i;
   World_Handle *smpLeader = CpuSched_GetVsmpLeader(world);

   // sanity check
   ASSERT(CpuSchedAllCellsAreLocked());

   // convenient abbrevs
   vcpu = World_CpuSchedVcpu(world);
   vsmp = &smpLeader->sched.cpu.vsmpData;

   // debugging
   VMLOG(1, world->worldID,
         "name='%s', min=%d, max=%d, unit=%s, shares=%d, affinity[0]=0x%x",
         world->worldName,
         config->alloc.min,
         config->alloc.max,
         Sched_UnitsToString(config->alloc.units),
         config->alloc.shares,
         config->vcpuAffinity[0]);

   // zero VCPU state, VSMP data area
   memset(vcpu, 0, sizeof(CpuSched_Vcpu));
   memset(&world->sched.cpu.vsmpData, 0, sizeof(CpuSched_Vsmp));

   // initialize special event numbers
   vcpu->sleepEvent  = (uint32) &vcpu->sleepEvent;
   vcpu->actionEvent = (uint32) &vcpu->actionEvent;
   vcpu->haltEvent   = (uint32) &vcpu->haltEvent;

   // initialize start time
   vcpu->stats.uptimeStart = MY_CELL->now;

   // initialize pcpu mapping
   vcpu->pcpuMapped = INVALID_PCPU;

   // verify that our affinity masks are reasonable
   if (config->numVcpus > 1) {
      if (CpuSchedVerifyAffinity(config->numVcpus, 
                                 config->vcpuAffinity,
                                 &jointAffinity) == VMK_OK) {
         vsmp->jointAffinity = jointAffinity;
      } else {
         VmWarn(world->worldID, "invalid affinity settings, ignored");
         // unset all affinities
         for (i=0; i < config->numVcpus; i++) {
            config->vcpuAffinity[i] = cpuSchedConst.defaultAffinity;
         }
         // unset affinity for all previously-added vcpus
         if (vsmp->vcpus.len > 0) {
            CpuSchedVsmpSetAffinityInt(vsmp, config->vcpuAffinity, TRUE);
         }
      }
   }

   // initialize vsmp handle
   vcpu->vsmp = vsmp;
   if (CpuSched_IsVsmpLeader(world)) {
      ASSERT(&world->sched.cpu.vsmpData == vsmp);
      // assign vsmp leader
      vsmp->leader = world;

      // initial cell placement
      vsmp->cell = CpuSchedCellInitialPlacement(config);
      if (vsmp->cell == NULL) {
         VmWarn(world->worldID, "no valid cell assignment");
         return(VMK_NOT_SUPPORTED);
      }
   } else {
      // simple feasibility check: #vcpus <= #pcpus
      // n.b. assumes world group leader starts first
      if (vsmp->vcpus.len == numPCPUs) {
         VmWarn(world->worldID, "nVCPUs=%d > nPCPUs=%d",
                vsmp->vcpus.len + 1, numPCPUs);
         return(VMK_NOT_SUPPORTED);
      }
   }

   // sanity check
   ASSERT(vsmp->cell != NULL);

   // initialize state histograms
   CpuSchedInitHistograms(vcpu);

   // initialize load history
   vcpu->loadHistory = CpuMetrics_LoadHistoryNew();

   // initial placement, respecting affinity
   vcpu->pcpuHandoff = INVALID_PCPU;
   CpuSchedVcpuSetAffinityMask(vcpu, config->vcpuAffinity[vsmp->vcpus.len], TRUE);
       
   if (vsmp->hardAffinity && config->numVcpus > 1) {
      vsmp->affinityConstrained = TRUE;
   } 

   vsmp->numa.lastMonMigMask = 0;

   vcpu->pcpu = CpuSchedVcpuInitialPlacement(vcpu);
   VCPULOG(1, vcpu, "initial placement (affinity=0x%x): pcpu %d",
           vcpu->affinityMask, vcpu->pcpu);

   // initial placement failed, ignore affinity for all vcpus in this vsmp
   if (vcpu->pcpu == INVALID_PCPU) {
      VmWarn(world->worldID, "invalid affinity 0x%x, ignored", vcpu->affinityMask);
      for (i=0; i < config->numVcpus; i++) {
         config->vcpuAffinity[i] = cpuSchedConst.defaultAffinity;
      }

      if (vsmp->vcpus.len > 1) {
         CpuSchedVsmpSetAffinityInt(vsmp, config->vcpuAffinity, TRUE);
      } else {
         CpuSchedVcpuSetAffinityMask(vcpu, cpuSchedConst.defaultAffinity, TRUE);
      }
      vcpu->pcpu = 0;
   }
   if (CPUSCHED_DEBUG) {
      VCPULOG(1, vcpu, "initial pcpu=%u", vcpu->pcpu);
   }

   // initialize per-pcpu memory region mappings (kseg, prda)
   CpuSchedVcpuMapPcpu(vcpu, vcpu->pcpu);

   // initialize idle flag
   vcpu->idle = World_IsIDLEWorld(world);

   // alloc should be same for all vcpus,
   //  but update on each add anyway since some state depends on nvcpus
   CpuSchedAllocInit(&alloc, 
                     config->alloc.min,
                     config->alloc.max,
                     config->alloc.units,
                     config->alloc.shares);
   if (CpuSchedVsmpSetAllocSpecial(vsmp, &alloc, config->numVcpus) != VMK_OK) {
      VcpuWarn(vcpu,
               "could not add vm: min=%u, max=%u, shares=%u, numVcpus=%u",
               config->alloc.min, config->alloc.max, 
               config->alloc.shares, config->numVcpus);
      return(VMK_NO_RESOURCES);
   }

   // update VSMP state
   if (CpuSched_IsVsmpLeader(world)) {
      // n.b. assumes world group leader starts first
      CpuSchedAddFirstVcpu(world, config, running);

      // We don't setup HT sharing for real until all vcpus in this
      // vsmp have come up. This may lead to a tiny window in which your
      // HT sharing isn't respected, but since no guest code is allowed
      // to run until all vcpus are up, it seems unlikely that anyone
      // would mind.
      vsmp->htSharing = CPUSCHED_HT_SHARE_ANY;
   } else {
      // sanity checks: can't add running or idle multiprocessor vsmp
      ASSERT(!running);
      ASSERT(!vcpu->idle);

      // add additional vcpu to existing vsmp
      if (vsmp->vcpus.len == 1) {
         CpuSchedAddSecondVcpu(world, config);
      } else {
         CpuSchedAddNthVcpu(world, config);
      }
   }

   // if we're the last world in the vsmp,
   // initialize HT sharing
   if (SMP_HTEnabled()
       && vsmp->vcpus.len == config->numVcpus) {
      vsmp->maxHTConstraint = CpuSchedVsmpMaxHTConstraint(vsmp);
      CpuSchedSetHTSharing(vsmp, config->htSharing);
   }
   
   // vcpu addition will succeed

   // initialize wait state
   CpuSchedVcpuSetWaitState(vcpu, CPUSCHED_WAIT_NONE, CPUSCHED_EVENT_NONE);

   // initialize sleep, action check locks
   SP_InitLockIRQ("Sleep", &vcpu->sleepLock, SP_RANK_IRQ_BLOCK);
   SP_InitLockIRQ("ActionWakeup", &vcpu->actionWakeupLock, SP_RANK_IRQ_LEAF);

   // everything OK
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_Add --
 *
 *      Add "world", which may already be "running", to CPU scheduler,
 *	with initial configuration specified by "config".
 *
 * Results:
 *	Returns VMK_OK if successful, otherwise error code.
 *      Updates scheduler state, "world" scheduling state.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
CpuSched_Add(World_Handle *world,
             Sched_CpuClientConfig *config,
             Bool running)
{
   VMK_ReturnStatus status;
   SP_IRQL prevIRQL;
   
   // invoke primitive holding appropriate locks
   prevIRQL = CpuSchedLockAllCells();
   status = CpuSchedAddInt(world, config, running);
   CpuSchedUnlockAllCells(prevIRQL);

   // add proc handler, if successful
   if (status == VMK_OK) {
      CpuSchedAddWorldProcEntries(world);
   }

   return(status);
}


/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedIdleShouldHalt --
 *
 *     Returns TRUE iff the idle loop on PCPU "P" should halt now.
 *     We only halt if we're on a hyperthreaded system and our
 *     partner logical cpu is busy.
 *
 * Results:
 *     Returns TRUE iff the idle loop on PCPU "P" should halt now.
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE Bool
CpuSchedIdleShouldHalt(PCPU p)
{
   if (!SMP_HTEnabled() || !CONFIG_OPTION(CPU_HALTING_IDLE) || !vmkernelLoaded) {
      return (FALSE);
   } else {
      return (!CpuSchedPartnerIsIdle(p));
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * CpuSched_IdleHaltEnd --
 *
 *	Cleanup after a halt, updating pcpu statistics.  The
 *	parameter "fromIntrHandler" is used to maintain stats
 *	on the number of halts ended from an interrupt context.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */
void
CpuSched_IdleHaltEnd(Bool fromIntrHandler)
{
   PCPU p = MY_PCPU;
   CpuSched_Pcpu *myPcpu = CpuSchedPcpu(p);
   Timer_Cycles haltDiff;

   // don't want to get interrupted and do nested IdleHaltEnd here
   CpuSchedPackageHaltLock(p);
   if (!myPRDA.halted) {
      CpuSchedPackageHaltUnlock(p);
      CpuSchedLogEvent("spur-endhlt", 0);
      return;
   }

   ASSERT(myPcpu->haltStart != 0);

   // clear flag
   myPRDA.halted = FALSE;

   // update stats
   myPcpu->stats.idleHaltEnd++;
   if (fromIntrHandler) {
      myPcpu->stats.idleHaltEndIntr++;
   }

   // update total halted time
   haltDiff = RDTSC() - myPcpu->haltStart;
   if (UNLIKELY(!RateConv_IsIdentity(&myPRDA.tscToTC))) {
      haltDiff = RateConv_Unsigned(&myPRDA.tscToTC, haltDiff);
   }

   // debugging
   if (vmx86_debug) {
      Histogram_Insert(myPcpu->haltHisto, Timer_TCToUS(haltDiff));
   }

   if (haltDiff > cpuSchedConst.cyclesPerSecond) {
      // if we were 'halted' for an incredibly long time, something
      // is probably broken on the scheduler side (e.g time went backward)
      if (haltDiff > cpuSchedConst.cyclesPerMinute) {
         SysAlert("processor apparently halted for %Lu ms", Timer_TCToMS(haltDiff));
         // we could experience this condition if the debugger broke in
         ASSERT(Debug_EverInDebugger());
      } else {
         // If we were 'halted' for a few seconds, it could be that
         // an interrupt handler or bottom half went nuts and took ages.
         // Don't ASSERT but continue and hope we get an ASSERT somewhere
         // closer to the root of the problem
         Warning("processor apparently halted for %Lu ms", Timer_TCToMS(haltDiff));
      }
      
      // sanitize this value and continue in release builds
      haltDiff = 0;
   }

   // track total halted cycles and stats
   // stats can be reset, so they're kept separately
   myPcpu->totalHaltCycles += haltDiff;
   myPcpu->stats.haltCycles += haltDiff;

   // reset halt start time
   myPcpu->haltStart = 0;

   CpuSchedPackageHaltUnlock(p);

   CpuSchedLogEvent("end-hlt", haltDiff > 0xffffffffull ? 0 : (uint32)haltDiff);

   // sanity check
   ASSERT(p == MY_PCPU);
}


/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedIdleHaltStart --
 *
 *     Setup halt-related stats and issue the "hlt" instructions.
 *
 * Results:
 *     None.
 *
 * Side effects:
 *     Halts the logical processor until the next interrupt. 
 *     Modifies per-pcpu halt stats
 *
 *-----------------------------------------------------------------------------
 */
static void
CpuSchedIdleHaltStart(void)
{
   PCPU p = MY_PCPU;
   CpuSched_Pcpu *myPcpu = CpuSchedPcpu(p);

   CpuSchedPackageHaltLock(p);

   // don't halt if our partner is halted
   if (MY_PARTNER_PRDA->halted) {
      CpuSchedPackageHaltUnlock(p);
      return;
   }
   
   // don't do two halts at the same time!
   ASSERT(!myPRDA.halted);
   
   myPcpu->haltStart = RDTSC();
   myPRDA.halted = TRUE;

   CpuSchedPackageHaltUnlock(p);

   // sleep until the next interrupt comes in
   __asm__ __volatile__("hlt\n\t"
                        "CpuSchedAfterHLTLabel:\n\t"
                        "nop\n\t");

   // sanity check
   ASSERT(p == MY_PCPU);   
}


/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedIdlePackageRebalanceCheck --
 *
 *      Called from an idle context, this checks to see if we need to tell
 *      a remote, busy package that it should move its currently running
 *      vcpu in order to improve inter-package rebalance.
 *
 *      Should only be called on hyper-threaded systems.
 *
 * Results:
 *      Updates lastSchedCheck.
 *
 * Side effects:
 *      May tell a remote cpu to reschedule and move its current runner.
 *
 *-----------------------------------------------------------------------------
 */
static void
CpuSchedIdlePackageRebalanceCheck(TSCCycles *lastSchedCheck /* in/out */)
{
   Bool preempt;
   TSCCycles timeNow;
   PCPU myPcpu;

   ASSERT(SMP_HTEnabled());

   // only the primarly lcpu does this check
   // config value of 0 means to disable idle rebalance checking
   if (MY_CELL->config.idlePackageRebalanceCycles == 0 ||
       SMP_GetHTThreadNum(MY_PCPU) != 0) {
      return;
   }

   preempt = CpuSched_DisablePreemption();

   timeNow = RDTSC();
   myPcpu= MY_PCPU;

   if (timeNow - *lastSchedCheck > MY_CELL->config.idlePackageRebalanceCycles) {
      FORALL_CELL_REMOTE_PACKAGES(MY_CELL, myPcpu, p) {
         PCPU partner = CpuSchedPcpu(p)->partner->id;
         if (!CpuSchedPcpuIsIdle(p) && !CpuSchedPcpuIsIdle(partner)) {
            // this whole package is busy, so resched somebody
            // randomly choose either lcpu 0 or lcpu 1 on the identified package
            // use tsc low-bit as random bit, since CpuSchedRandom requires
            // that you disable interrupts first
            PCPU pcpuToFlag = (timeNow & 1) ? p : partner;

            CpuSchedPcpu(pcpuToFlag)->runnerMoveRequested = TRUE;
            CpuSchedMarkRescheduleInt(pcpuToFlag, TRUE);
            break;
         }
      } CELL_REMOTE_PACKAGES_DONE;
   }

   *lastSchedCheck = timeNow; // processor local, so tsc is valid
   CpuSched_RestorePreemption(preempt);
}


/*
 *----------------------------------------------------------------------
 *
 * CpuSched_IdleLoop --
 *
 *      Executes idle loop in dedicated idle world context on
 *	this processor.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Never terminates.
 *
 *----------------------------------------------------------------------
 */
void
CpuSched_IdleLoop(void)
{
   uint32 myPCPUState = myPRDA.pcpuState;
   PCPU myPCPU = myPRDA.pcpuNum;
   CpuSched_Pcpu *pcpu = CpuSchedPcpu(myPCPU);
   TSCCycles lastSchedCheck = RDTSC();
   
   // idle loop
   while (TRUE) {
      /*
       * the following two asserts are couple of the most frequently checked
       * asserts, so disabling them when running with assert stress.
       */
      if (!VMK_STRESS_DEBUG_OPTION(ASSERT_STRESS)) {
         // sanity checks
         ASSERT_HAS_INTERRUPTS();
         ASSERT(CpuSched_IsPreemptible());
         ASSERT(myPCPU == MY_PCPU);
      }
      
      // reschedule, if needed
      if (myPRDA.reschedule || pcpu->deferredResched) {
         Bool preemptible = CpuSched_DisablePreemption();
         ASSERT(preemptible);
	 CpuSched_Reschedule();
         CpuSched_RestorePreemption(preemptible);
      }
      
      // perform halt-check on slaves
      if (myPCPUState == PCPU_AP) {
         SMP_SlaveHaltCheck(myPCPU);
      }

      if (SMP_HTEnabled()) {
         CpuSchedIdlePackageRebalanceCheck(&lastSchedCheck);
      }
      
      if (CpuSchedIdleShouldHalt(myPCPU)) {
         ASSERT(CpuSched_IsPreemptible());
         CpuSchedIdleHaltStart();
         if (UNLIKELY(myPRDA.halted)) {
            // interrupt handler normally terminates HLT,
            // so this should only happen after an NMI
            CpuSched_IdleHaltEnd(FALSE);
         }
      } else {
         // On P4, improves power+perf; REPZ-NOP on non-P4
         PAUSE();
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedBusyWait --
 *
 * 	Executes idle busy-wait loop in current world context on the
 * 	local processor until an action or reschedule is pending for
 * 	the current world.  Resource consumption is charged
 * 	appropriately to the dedicated idle world for the current
 * 	processor.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Modifies scheduler state.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedBusyWait(SP_IRQL prevIRQL)
{
   World_Handle *myWorld = MY_RUNNING_WORLD;
   CpuSched_Vcpu *myVcpu = World_CpuSchedVcpu(myWorld);
   CpuSched_Vsmp *myVsmp = myVcpu->vsmp;
   Bool preemptible = CpuSched_IsPreemptible();
   PCPU myPcpu = MY_PCPU;
   CpuSched_Pcpu *pcpu = CpuSchedPcpu(myPcpu);
   TSCCycles lastSchedCheck = RDTSC();
   SP_IRQL checkIRQL;

   // sanity checks
   ASSERT(CpuSchedVsmpCellIsLocked(myVsmp));
   ASSERT(!preemptible);
   ASSERT(myVcpu->runState == CPUSCHED_BUSY_WAIT);

   // debugging
   VcpuLogEvent(myVcpu, "bwait-loop");

   // mark idle in prda
   myPRDA.idle = 1;

   if (Vmkperf_TrackPerWorld()) {
      Vmkperf_WorldSave(myWorld);
   }
   
   // release lock
   CpuSchedVsmpCellUnlock(myVsmp, prevIRQL);

   // run any pending bottom-half handlers w/o reschedule
   BH_Check(FALSE);

   // allow preemption, bottom-half handlers
   preemptible = CpuSched_EnablePreemption();
   ASSERT(!preemptible);

   // busy-wait idle loop
   while (TRUE) {
      // The following asserts are some of the most frequently checked
      // asserts, so disable them when running with assert stress.
      if (!VMK_STRESS_DEBUG_OPTION(ASSERT_STRESS)) {
         // sanity checks
         ASSERT_HAS_INTERRUPTS();
         ASSERT(CpuSched_IsPreemptible());
         ASSERT(myPcpu == MY_PCPU);
      }

      // terminate loop if done waiting or pending reschedule
      if ((myVcpu->runState != CPUSCHED_BUSY_WAIT)  ||
          (myVcpu->waitState == CPUSCHED_WAIT_NONE) ||
          myPRDA.reschedule ||
          pcpu->deferredResched) {
         break;
      }   
      
      // terminate loop if pending action should wakeup
      if ((myVcpu->actionWakeupMask != 0) &&
          World_IsVMMWorld(myWorld) &&
          Action_PendingInMask(myWorld, myVcpu->actionWakeupMask)) {
         break;
      }

      // perform halt-check on slaves
      if (myPRDA.pcpuState == PCPU_AP) {
         SMP_SlaveHaltCheck(myPcpu);
      }
      
      if (SMP_HTEnabled()) {
         CpuSchedIdlePackageRebalanceCheck(&lastSchedCheck);
      }

      // check if should halt
      if (CpuSchedIdleShouldHalt(myPcpu)) {
         // sleep until the next interrupt comes in
         CpuSchedIdleHaltStart();
         if (UNLIKELY(myPRDA.halted)) {
            // interrupt handler normally terminates HLT,
            // so this should only happen after an NMI
            CpuSched_IdleHaltEnd(FALSE);
         }
      } else {
         // On P4, improves power+perf; REPZ-NOP on non-P4
         PAUSE();
      }
   }

   // sanity checks
   if (CPUSCHED_DEBUG) {
      ASSERT_PRDA_SANITY();
   }
   ASSERT(myWorld == MY_RUNNING_WORLD);

   // restore original preemption state (disabled)
   ASSERT(!preemptible);
   CpuSched_RestorePreemption(preemptible);

   // wakeup if pending action (like CpuSched_AsyncCheckActions())
   if ((myVcpu->actionWakeupMask != 0) &&
       World_IsVMMWorld(myWorld) &&
       Action_PendingInMask(myWorld, myVcpu->actionWakeupMask) &&
       (myVcpu->waitState != CPUSCHED_WAIT_NONE)) {
      SP_IRQL eventIRQL, schedIRQL;
      EventQueue *eventQueue;

      VcpuLogEvent(myVcpu, "bwait-action");

      eventQueue = EventQueue_Find(myVcpu->waitEvent);
      eventIRQL = EventQueue_Lock(eventQueue);
      ASSERT(eventIRQL == prevIRQL);
      schedIRQL = CpuSchedVsmpCellLock(myVsmp);
      ASSERT(schedIRQL == CPUSCHED_IRQL);

      if (EventQueue_Remove(eventQueue, myWorld)) {
         CpuSchedVcpuWakeup(myVcpu);
      }

      // keep scheduler lock, release eventQueue lock
      EventQueue_Unlock(eventQueue, CPUSCHED_IRQL);
      VcpuLogEvent(myVcpu, "bwait-exita");
   } else {
      // reacquire lock
      VcpuLogEvent(myVcpu, "bwait-exitb");
      checkIRQL = CpuSchedVsmpCellLock(myVsmp);
      ASSERT(checkIRQL == prevIRQL);
   }

   // clear idle flag in prda
   myPRDA.idle = 0;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_VcpuHalt --
 *
 *      Halt the running vcpu.  The vcpu will be awakened if an
 *	action is posted to it.  If the specified "timeOutUsec"
 *	is non-zero, then the vcpu will also be awakened if
 *	the timeout expires.
 *
 * Results:
 *      None. 
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedHaltCallback(void *data, UNUSED_PARAM(Timer_AbsCycles timestamp))
{
   uint32 event = (uint32) data;
   CpuSched_Wakeup(event);
}

VMKERNEL_ENTRY
CpuSched_VcpuHalt(DECLARE_1_ARG(VMK_VCPU_HALT, int64, timeOutUsec))
{
   CpuSched_Vcpu *vcpu = World_CpuSchedVcpu(MY_RUNNING_WORLD);
   uint32 event = vcpu->haltEvent;
   Timer_Handle th;

   PROCESS_1_ARG(VMK_VCPU_HALT, int64, timeOutUsec);

#ifndef ESX3_NETWORKING_NOT_DONE_YET
   if (CONFIG_OPTION(NET_CLUSTER_HALT_CHECK) && Net_HaltCheck()) {
      // world had some delayed receives pending so we shouldn't halt it
      return VMK_OK;
   }
#endif // ESX3_NETWORKING_NOT_DONE_YET

   // sanity check
   ASSERT_HAS_INTERRUPTS();

   // update stats (unlocked but safe; per-vcpu field updated only here)
   vcpu->stats.halt++;

   // setup timeout callback, if any
   if (timeOutUsec > 0) {
      th = Timer_AddHiRes(MY_PCPU, CpuSchedHaltCallback, timeOutUsec,
                          TIMER_ONE_SHOT, (void *) event);
   } else {
      th = TIMER_HANDLE_NONE;
   }

   // wait until timeout or any action pending
   CpuSched_WaitDirectedYield(event, CPUSCHED_WAIT_IDLE,
                              0xffffffff, NULL, INVALID_WORLD_ID);

   // remove callback (avoid redundant callbacks)
   if (th != TIMER_HANDLE_NONE) {
      Timer_Remove(th);
   }

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_HostIsRunning --
 *
 *      Returns TRUE if the host world is currently executing.
 *
 * Results:
 *      Returns TRUE if the host world is currently executing.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Bool
CpuSched_HostIsRunning(void)
{
   return(prdas[CONSOLE_PCPU]->runningWorld == CONSOLE_WORLD);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_IsHostWorld --
 *
 *      Returns TRUE if the currently-executing world is the host world.
 *
 * Results:
 *      Returns TRUE if host is the running world.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Bool
CpuSched_IsHostWorld(void)
{
   // host runs early initialization code
   if (CONSOLE_WORLD == NULL) {
      return(TRUE);
   }
   
   // check for match
   return(MY_RUNNING_WORLD == CONSOLE_WORLD);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSched_ForallGroupMembersDo --
 *
 *     Executes "fn" on each world in the group led by "leader", passing
 *     "data" as second param to "fn".  Does a World_Find on each world,
 *     so that it won't disappear.  Acquires appropriate scheduler cell
 *     lock, but "fn" is invoked WITHOUT holding any scheduler locks.
 *
 * Results:
 *     Always returns VMK_OK.
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
CpuSched_ForallGroupMembersDo(struct World_Handle *leader, 
                              WorldForallFn fn, 
                              void *data)
{
   uint32 v, numMembers = 0;
   SP_IRQL prevIRQL;
   CpuSched_Vsmp *vsmp = World_CpuSchedVsmp(leader);
   World_ID worldList[MAX_VCPUS];

   prevIRQL = CpuSchedVsmpCellLock(vsmp);

   FORALL_VSMP_VCPUS(vsmp, vcpu) {
      worldList[numMembers] = VcpuWorldID(vcpu);
      numMembers++;
   } VSMP_VCPUS_DONE;

   CpuSchedVsmpCellUnlock(vsmp, prevIRQL);

   for (v=0; v < numMembers; v++) {
      World_Handle *world = World_Find(worldList[v]);
      if (world) {
         fn(world, data);
         World_Release(world);
      }
   }

   return (VMK_OK);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSched_ForallGroupLeadersDo --
 *
 *     Executes "fn" on the world leader of each vsmp in the system, passing
 *     "data" as second param to "fn".  Does a World_Find on each world,
 *     so that it won't disappear.  Acquires all scheduler cell locks,
 *     but "fn" is invoked WITHOUT holding any scheduler locks.
 *     
 *
 * Results:
 *     Returns VMK_OK on success, VMK_NO_MEMORY if heap alloc fails.
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
CpuSched_ForallGroupLeadersDo(WorldForallFn fn, void *data)
{
   World_ID *allWorlds;
   uint32 v, numVsmps;
   SP_IRQL prevIRQL;

   prevIRQL = CpuSchedLockAllCells();

   // allocate storage 
   numVsmps = CpuSchedNumVsmps();
   allWorlds = Mem_Alloc(sizeof(World_ID) * numVsmps);
   if (!allWorlds) {
      CpuSchedUnlockAllCells(prevIRQL);
      return (VMK_NO_MEMORY);
   }

   // collect all leaders
   v = 0;
   FORALL_CELLS(c) {
      FORALL_CELL_VSMPS(c, vsmp) {
         allWorlds[v++] = vsmp->leader->worldID;
      } CELL_VSMPS_DONE;
   } CELLS_DONE;
   ASSERT(v == numVsmps);

   CpuSchedUnlockAllCells(prevIRQL);

   // invoke function on all leaders
   for (v=0; v < numVsmps; v++) {
      World_Handle *world = World_Find(allWorlds[v]);
      if (world) {
         fn(world, data);
         World_Release(world);
      }
   }

   // reclaim storage, succeed
   Mem_Free(allWorlds);
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_HostWorldCmp --
 *
 *      Returns TRUE if "world" is the host world.
 *
 * Results:
 *      Returns TRUE if "world" is the host world.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Bool
CpuSched_HostWorldCmp(World_Handle *world)
{
   return(world == CONSOLE_WORLD);
}


/*
 *----------------------------------------------------------------------
 *
 * CpuSched_ProcessorIdleTime --
 *
 *      Return a snapshot of the cumulative elapsed idle time
 *	in cycles for processor "pcpu".  If "locked" is set,
 *	performs appropriate locking to ensure snapshot is
 *	consistent.
 *
 * Results:
 *	Returns cumulative idle time in cycles for "pcpu".
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
Timer_Cycles
CpuSched_ProcessorIdleTime(PCPU pcpu, Bool locked)
{
   CpuSched_Pcpu *schedPcpu = CpuSchedPcpu(pcpu);
   Timer_Cycles snapshot;

   // OPT: use "versioned atomic" macros instead of locking
   //      and eliminate "locked" parameter

   if (locked) {
      // snapshot holding lock (slower, but consistent)
      SP_IRQL prevIRQL = CpuSchedCellLock(schedPcpu->cell);
      snapshot = schedPcpu->idleCycles;
      CpuSchedCellUnlock(schedPcpu->cell, prevIRQL);
   } else {
      // snapshot w/o locking (faster, but not atomic)
      snapshot = schedPcpu->idleCycles;
   }
   
   return(snapshot);
}


/*
 *-----------------------------------------------------------------------------
 *
 * CpuSched_PcpuUsageStats --
 *
 *      Fills the arrays idle, used, and sysOverlap with cumulative
 *      per-pcpu idle, used, and sysOverlap cycle counts.
 *      These arrays should have enough space for numPCPUs entries.
 *      Acquires the cpusched lock.
 *
 * Results:
 *      Fills idle, used, and sysOverlap
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
void
CpuSched_PcpuUsageStats(Timer_Cycles* idle,
                        Timer_Cycles* used,
                        Timer_Cycles* sysOverlap)
{
   SP_IRQL prevIRQL;

   prevIRQL = CpuSchedLockAllCells();
   
   FORALL_PCPUS(p) {
      idle[p] = CpuSchedPcpu(p)->idleCycles;
      used[p] = CpuSchedPcpu(p)->usedCycles;
      sysOverlap[p] = CpuSchedPcpu(p)->sysCyclesOverlap;
   } PCPUS_DONE;
   
   CpuSchedUnlockAllCells(prevIRQL);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_VcpuUsageUsec -- 
 *      
 *      Returns a snapshot of the cumulative cpu consumption by the
 *	vcpu associated with "world", in cpu-package microseconds.  
 *	Does not acquire CpuSched lock, instead using versioned
 *	atomic accesses to ensure consistent snapshot.
 *
 * Results:
 *      Returns snapshot of total cpu usec consumed by "world".
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
uint64
CpuSched_VcpuUsageUsec(World_Handle *world)
{
   CpuSched_Vcpu *vcpu = World_CpuSchedVcpu(world);
   Timer_Cycles used;

   // snapshot cumulative charged usage
   used = CpuSchedVcpuChargeCyclesTotalGet(vcpu);

   // add estimated uncharged usage from current quantum
   if (vcpu->runState == CPUSCHED_RUN) {
      Timer_Cycles quantum = vcpu->vsmp->cell->config.quantumCycles;
      Timer_AbsCycles now = Timer_GetCycles();
      Timer_AbsCycles start;

      // snapshot start of current charging period
      start = CpuSchedVcpuChargeStartGet(vcpu);

      // add uncharged elapsed time, if any
      // n.b. the "2 * quantum" bound may be unnecessary, but
      //      it's a useful sanity check/guard nonetheless
      if ((start > 0) && (now > start) && (now - start < 2 * quantum)) {
         used += now - start;
      }
   }

   // HT accounting measures usage in LCPU cycles
   used /= SMP_LogicalCPUPerPackage();

   // return usage in microseconds
   return(Timer_TCToUS(used));
}

/*
 * Driver Interface Operations
 */

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_IsPreemptible --
 *
 *	Return whether the current world is preemptible
 *
 * Results:
 *	TRUE if preemptible, FALSE otherwise
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
Bool
CpuSched_IsPreemptible(void)
{
   if (!PRDA_IsInitialized() || (MY_RUNNING_WORLD == NULL)) {
      // early init
      return FALSE;
   }
   return !(MY_RUNNING_WORLD->preemptionDisabled);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_DisablePreemption --
 *
 *	Disable preemption for the current world, and also return the
 *      previous preemptible status
 *
 * Results:
 *	TRUE if world was preemptible, FALSE otherwise
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
Bool
CpuSched_DisablePreemption(void)
{
   Bool preemptible = CpuSched_IsPreemptible();

   if (PRDA_IsInitialized() && MY_RUNNING_WORLD) {
      MY_RUNNING_WORLD->preemptionDisabled = TRUE;
      if (CPUSCHED_PREEMPT_STATS && preemptible) {
         // track preemption disable time stats
         World_CpuSchedVcpu(MY_RUNNING_WORLD)->disablePreemptStartTime = RDTSC();
      }
   }
   
   return preemptible;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_EnablePreemption --
 *
 *	Enable preemption for the current world, and also return the
 *	previous preemptible status.  This should be used carefully since
 *	the current context may not be preemptible.
 *
 * Results:
 *	TRUE if world was preemptible, FALSE otherwise
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
Bool
CpuSched_EnablePreemption(void)
{
   Bool preemptible = CpuSched_IsPreemptible();

   if (PRDA_IsInitialized() && MY_RUNNING_WORLD) {
      ASSERT(World_IsSafeToDeschedule());
      if (CPUSCHED_PREEMPT_STATS && !preemptible) {
         // track stats when we switch from nonpreemptible->preemptible
         CpuSchedPreemptEnabledStatsUpdate(World_CpuSchedVcpu(MY_RUNNING_WORLD)); 
      }
      MY_RUNNING_WORLD->preemptionDisabled = FALSE;
   }
   return preemptible;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_RestorePreemption --
 *
 *	Restore preemption status to what it used to be (the old state is
 *	passed in as a parameter)
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
CpuSched_RestorePreemption(Bool preemptible)
{
   if (PRDA_IsInitialized() && MY_RUNNING_WORLD) {
      CpuSched_Vcpu *myVcpu = World_CpuSchedVcpu(MY_RUNNING_WORLD);
      Bool prevPreemptible = CpuSched_IsPreemptible();
      
      if (preemptible) {
         ASSERT(World_IsSafeToDeschedule());
      }
      if (preemptible && !prevPreemptible) {
         // track nonpreemptible->preemptible switch stats
         CpuSchedPreemptEnabledStatsUpdate(myVcpu);
      }

      // actually change our preemptibility
      MY_RUNNING_WORLD->preemptionDisabled = !preemptible;

      if (CPUSCHED_PREEMPT_STATS && !preemptible && prevPreemptible) {
         // track preemptible->nonpreemptible switch stats
         myVcpu->disablePreemptStartTime = RDTSC();
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_GetCurrentWorld --
 *      
 *      Mainly for use in linux drivers where the myPRDA struct isn't
 *      exported.
 *
 * Results:
 *      Returns a pointer World_Hanldle struct of the current world
 *      (or NULL if vmkernel isn't loaded)
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
World_Handle *
CpuSched_GetCurrentWorld(void)
{
   return vmkernelLoaded ? MY_RUNNING_WORLD : NULL;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CpuSched_CurrentWorldSwitchCount --
 *
 *     Returns the number of world switches that this world has undergone
 *
 * Results:
 *     Returns the number of world switches that this world has undergone
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
uint32 CpuSched_WorldSwitchCount(World_Handle *world)
{
   if (vmkernelLoaded) {
      return World_CpuSchedVcpu(world)->stats.worldSwitch;
   } else {
      return 0;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_MyPCPU --
 *      
 *      Mainly for use in linux drivers where the myPRDA struct
 *      isn't exported.
 *
 * Results:
 *      Returns the current PCPU (or 0 if vmkernel isn't loaded)
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
PCPU
CpuSched_MyPCPU(void)
{
   return PRDA_GetPCPUNumSafe();
}

/*
 *----------------------------------------------------------------------
 *
 * Sched_{Driver|SCSI}WaitIRQ --
 *      
 *      Mainly for use in linux drivers where the wait states aren't
 *      exported.
 *
 * Results:
 *      CpuSchedWait return value
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
CpuSched_DriverWaitIRQ(uint32 event,
                       SP_SpinLockIRQ *lock,
                       SP_IRQL callerPrevIRQL)
{
   return CpuSched_WaitIRQ(event, CPUSCHED_WAIT_DRIVER, lock, callerPrevIRQL);
}

VMK_ReturnStatus
CpuSched_SCSIWaitIRQ(uint32 event,
                     SP_SpinLockIRQ *lock,
                     SP_IRQL callerPrevIRQL)
{
   return CpuSched_WaitIRQ(event, CPUSCHED_WAIT_SCSI, lock, callerPrevIRQL);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedRemoveLastVcpu --
 *
 *      Requires that "vcpu" has no siblings.  Removes "vcpu", its
 *	associated vsmp, and its remaining shares from the scheduler.
 *      Caller must hold scheduler cell lock for "vcpu".
 *
 * Results:
 *      Updates "vcpu" scheduling state.
 *
 * Side effects:
 *	Updates scheduler state.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedRemoveLastVcpu(CpuSched_Vcpu *vcpu)
{
   CpuSched_Vsmp *vsmp = vcpu->vsmp;
   
   // sanity checks
   ASSERT(CpuSchedVsmpCellIsLocked(vsmp));
   ASSERT(vsmp->vcpus.len == 1);

   // remove remaining shares
   if (vsmp->base.shares > 0) {
      VCPULOG(1, vcpu, "removing shares: alloc=%d, base=%d",
              vsmp->alloc.shares, vsmp->base.shares);
      CpuSchedVsmpRevokeAlloc(vsmp);
   }

   // remove from vsmp list of vcpus
   CpuSchedVcpuArrayRemove(vsmp, vcpu);
   
   ASSERT(vsmp->vcpus.len == 0);

   // remove last vcpu from vsmp list
   CpuSched_VsmpArrayRemove(&vsmp->cell->vsmps, vsmp);

   // cleanup skew lock
   SP_CleanupLockIRQ(&vsmp->vcpuArrayLock);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedRemoveSecondVcpu --
 *
 *      Requires that "vcpu" has exactly one sibling.  Removes "vcpu"
 *	from the scheduler.  Transitions its associated vsmp from a
 *	multiprocessor to a uniprocessor.  Caller must hold scheduler
 *      cell lock for "vcpu".
 *
 * Results:
 *      Updates "vcpu" scheduling state.
 *
 * Side effects:
 *	Updates scheduler state.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedRemoveSecondVcpu(CpuSched_Vcpu *vcpu)
{
   CpuSched_Vsmp *vsmp = vcpu->vsmp;
   CpuSched_Vcpu *vcpu0;

   // sanity checks
   ASSERT(CpuSchedVsmpCellIsLocked(vsmp));
   ASSERT(vsmp->vcpus.len == 2);

   // remove from vsmp list of vcpus
   CpuSchedVcpuArrayRemove(vsmp, vcpu);
   ASSERT(vsmp->vcpus.len == 1);

   // transition vsmp to CO_NONE (uniprocessor)
   CpuSchedVsmpSetState(vsmp, CPUSCHED_CO_NONE);
   vsmp->affinityConstrained = FALSE;

   // obtain handle to last remaining vcpu
   vcpu0 = vsmp->vcpus.list[0];
   ASSERT(vcpu0 != NULL);

   // no longer allowed to use internal sharing
   if (vsmp->htSharing == CPUSCHED_HT_SHARE_INTERNALLY) {
      vsmp->htSharing = CPUSCHED_HT_SHARE_NONE;
   }
   vsmp->maxHTConstraint = CpuSchedVsmpMaxHTConstraint(vsmp);
      
   
   // transition remaining vcpu to uni state
   switch (vcpu0->runState) {
   case CPUSCHED_RUN:
   case CPUSCHED_READY:
   case CPUSCHED_WAIT:
   case CPUSCHED_BUSY_WAIT:
   case CPUSCHED_ZOMBIE:
      // nothing to do
      break;

   case CPUSCHED_READY_COSTOP:
      // make ready, place on queue
      CpuSchedVcpuMakeReady(vcpu0);
      VCPULOG(0, vcpu0, "COSTOP => READY");
      break;

   case CPUSCHED_READY_CORUN:
      // abort coschedule, put back on queue
      CpuSchedVcpuCoRunAbort(vcpu0);
      CpuSchedVcpuMakeReady(vcpu0);
      VCPULOG(0, vcpu0, "CORUN => READY");
      break;

   case CPUSCHED_NEW:
   default:
      NOT_REACHED();
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedVcpuLogStats --
 *
 *      Dumps basic utilization stats about this vcpu to the log.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Writes to log.
 *
 *-----------------------------------------------------------------------------
 */
static void
CpuSchedVcpuLogStats(const CpuSched_Vcpu *vcpu)
{
   CpuSched_RunState s;
   uint32 usecState[CPUSCHED_NUM_RUN_STATES], usecCharge;
   uint64 secState[CPUSCHED_NUM_RUN_STATES], secCharge;
      
   for (s = 0; s < CPUSCHED_NUM_RUN_STATES; s++) {
      const CpuSched_StateMeter *m = &vcpu->runStateMeter[s];
      Timer_TCToSec(m->elapsed, &secState[s], &usecState[s]);
   }
   CpuSched_UsageToSec(vcpu->chargeCyclesTotal, &secCharge, &usecCharge);
   VcpuLog(vcpu,
           "charged: %9Lu.%03u run: %9Lu.%03u  wait: %9Lu.%03u"
           " bwait: %9Lu.%03u ready: %9Lu.%03u",
           secCharge, usecCharge / 1000,
           secState[CPUSCHED_RUN], usecState[CPUSCHED_RUN] / 1000,
           secState[CPUSCHED_WAIT], usecState[CPUSCHED_WAIT] / 1000,
           secState[CPUSCHED_BUSY_WAIT], usecState[CPUSCHED_BUSY_WAIT] / 1000,
           secState[CPUSCHED_READY], usecState[CPUSCHED_READY] / 1000);
   VcpuLog(vcpu,
           "switch: %u  migrate:  %u  halt:  %u   qexp:  %u",
           vcpu->stats.worldSwitch,
           vcpu->stats.pkgMigrate,
           vcpu->stats.halt,
           vcpu->stats.quantumExpire);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedRemoveInt --
 *
 *      Remove "world" from scheduler.
 *	Remove "world" from any list it may be on.
 *	Caller must hold lock for "eventQueue", if non-NULL.
 *      Caller must hold scheduler cell lock for "world".
 *
 * Results:
 *      Returns VMK_OK if successful, otherwise error code.
 *	May update "eventQueue", removing "world".
 *      Updates "world" scheduling state.
 *
 * Side effects:
 *	Updates scheduler state.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
CpuSchedRemoveInt(World_Handle *world, EventQueue *eventQueue)
{
   CpuSched_Vcpu *vcpu = World_CpuSchedVcpu(world);
   CpuSched_Vsmp *vsmp = vcpu->vsmp;
   Bool removed;

   // sanity check
   ASSERT(CpuSchedVsmpCellIsLocked(vsmp));

   // debugging
   VCPULOG(1, vcpu, "state=%s", CpuSchedRunStateName(vcpu->runState));

   // unable to remove running world
   if (CpuSchedVcpuRunOrBwait(vcpu)) {
      return(VMK_BUSY);
   }

   // dump some basic stats when the world exits
   if (vmx86_devel) {
      CpuSchedVcpuLogStats(vcpu);
   }
   
   // unwind current vcpu state
   switch (vcpu->runState) {
   case CPUSCHED_READY:
      // remove from run queue, unless idle
      if (!vcpu->idle) {
         CpuSchedQueueRemove(vcpu);
      }
      break;
   case CPUSCHED_READY_CORUN:
      // abort coschedule, keep off queue
      CpuSchedVcpuCoRunAbort(vcpu);
      break;
   case CPUSCHED_READY_COSTOP:
      // nothing to do, keep off queue
      break;
   case CPUSCHED_WAIT:
      ASSERT(eventQueue != NULL);
      // remove world from event wait queue
      removed = EventQueue_Remove(eventQueue, world);
      ASSERT(removed);
      // clear wait state
      CpuSchedVcpuSetWaitState(vcpu, CPUSCHED_WAIT_NONE, CPUSCHED_EVENT_NONE);
      break;
   case CPUSCHED_ZOMBIE:
      // nothing to do
      break;
   case CPUSCHED_RUN:
   case CPUSCHED_BUSY_WAIT:
   case CPUSCHED_NEW:
   default:
      // unexpected state
      VmWarn(world->worldID, "invalid state: %s",
             CpuSchedRunStateName(vcpu->runState));
      NOT_REACHED();
      return(VMK_BAD_PARAM);
   }

   // zombify vcpu
   VcpuLog(vcpu, "%s -> ZOMBIE", CpuSchedRunStateName(vcpu->runState));
   CpuSchedVcpuSetRunState(vcpu, CPUSCHED_ZOMBIE);

   // remove non-idle vcpu from scheduler lists
   if (!vcpu->idle) {
      if (vsmp->vcpus.len == 1) {
         // special case: uni
         CpuSchedRemoveLastVcpu(vcpu);
      } else if (vsmp->vcpus.len == 2) {
         // special case: multi -> uni
         CpuSchedRemoveSecondVcpu(vcpu);
      } else {
         // simply remove from vsmp list of vcpus
         ASSERT(vsmp->vcpus.len > 2);
         CpuSchedVcpuArrayRemove(vsmp, vcpu);
      }

      // debugging
      VCPULOG(0, vcpu, "remain vcpus=%d", vsmp->vcpus.len);
   }

   // cleanup locks
   SP_CleanupLockIRQ(&vcpu->sleepLock);
   SP_CleanupLockIRQ(&vcpu->actionWakeupLock);

   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_Remove --
 *
 *      Attempts to remove "world" from scheduler.  If "world" is
 *	currently running, then an error is returned.
 *
 * Results:
 *      Returns VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 *      Updates scheduler state associated with "world".
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
CpuSched_Remove(World_Handle *world)
{
   CpuSched_Vcpu *vcpu = World_CpuSchedVcpu(world);
   SP_IRQL eventIRQL, schedIRQL;
   VMK_ReturnStatus status;
   EventQueue *eventQueue;
   CpuSched_Cell *cell;

   schedIRQL = CpuSchedLockAllCells();
   if (vcpu->removeInProgress) {
      // somebody else is removing this vcpu already
      CpuSchedUnlockAllCells(schedIRQL);
      // we return OK, rather than BUSY to let the caller
      // know that this world is being destroyed and we don't
      // need to schedule a reap
      return (VMK_OK);
   }
   vcpu->removeInProgress = TRUE;

   // special case: remove unmanaged vcpu
   if (CpuSchedVcpuIsUnmanaged(vcpu)) {      
      CpuSched_RunState oldRunState = vcpu->runState;

      // zombify unmanaged world
      // n.b. deliberately bypassing CpuSchedVcpuSetRunState(),
      //      which assumes the scheduler is managing the vcpu
      vcpu->runState = CPUSCHED_ZOMBIE;
      
      // release all scheduler locks
      CpuSchedUnlockAllCells(schedIRQL);
      
      // debugging (log after locks released)
      VmLog(world->worldID, "zombified unscheduled world: runState=%s",
            CpuSchedRunStateName(oldRunState));
      
      return (VMK_OK);
   }

   // although we drop the locks here, we know that nobody else
   // will being removing the vcpu, since we're protected by
   // the "removeInProgress" flag (above)
   CpuSchedUnlockAllCells(schedIRQL);
   
   // acquire necessary locks
   schedIRQL = CpuSchedVcpuEventLock(vcpu, &eventQueue, &eventIRQL);
   cell = vcpu->vsmp->cell;

   // sanity check
   ASSERT(vcpu->runState != CPUSCHED_NEW);
   ASSERT(cell != NULL);

   // detach world from cpu scheduler
   status = CpuSchedRemoveInt(world, eventQueue);
   
   // release locks
   CpuSchedCellUnlock(cell, schedIRQL);
   if (eventQueue != NULL) {
      EventQueue_Unlock(eventQueue, eventIRQL);
   }

   if (status != VMK_OK) {
      schedIRQL = CpuSchedLockAllCells();
      vcpu->removeInProgress = FALSE;
      CpuSchedUnlockAllCells(schedIRQL);
   }
   return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_WorldCleanup --
 *
 *      Cleanup various scheduling state associated with "world",
 *	including removal of scheduler procfs nodes for "world".
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
CpuSched_WorldCleanup(World_Handle *world)
{
   CpuSched_Vcpu *vcpu = World_CpuSchedVcpu(world);

   // debugging
   VmLog(world->worldID, "state=%s", CpuSchedRunStateName(vcpu->runState));

   // remove world-specific proc entries
   CpuSchedRemoveWorldProcEntries(world);

   // remove pseudo-TSC timer
   if (world->pseudoTSCTimer != TIMER_HANDLE_NONE) {
      Bool found;
      found = Timer_Remove(world->pseudoTSCTimer);
      ASSERT(found);
    }

   // reclaim heap-allocated histograms
   CpuSchedDeleteHistograms(vcpu);

   // reclaim heap-allocated load history
   CpuMetrics_LoadHistoryDelete(vcpu->loadHistory);
   vcpu->loadHistory = NULL;   
}

/*
 * ProcFS Operations
 */

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedRunStateName --
 *
 *      Returns constant string name for specified "runState".
 *
 * Results: 
 *      Returns constant string name for "runState".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static const char *
CpuSchedRunStateName(CpuSched_RunState runState)
{
   switch (runState) {
   case CPUSCHED_NEW:
      return("NEW");
   case CPUSCHED_ZOMBIE:
      return("ZOMBIE");
   case CPUSCHED_RUN:
      return("RUN");
   case CPUSCHED_READY:
      return("READY");
   case CPUSCHED_READY_CORUN:
      return("CORUN");
   case CPUSCHED_READY_COSTOP:
      return("COSTOP");      
   case CPUSCHED_WAIT:
      return("WAIT");
   case CPUSCHED_BUSY_WAIT:
      return("WAITB");
   default:
      return("UNKNOWN");
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedCoRunStateName --
 *
 *      Returns constant string name for specified "coRunState".
 *
 * Results: 
 *      Returns constant string name for "coRunState".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static const char *
CpuSchedCoRunStateName(CpuSched_CoRunState coRunState)
{
   switch (coRunState) {
   case CPUSCHED_CO_NONE:
      return("NONE");
   case CPUSCHED_CO_RUN:
      return("RUN");
   case CPUSCHED_CO_READY:
      return("READY");
   case CPUSCHED_CO_STOP:
      return("STOP");
   default:
      return("UNKNOWN");
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedWaitStateName --
 *
 *      Returns constant string name for specified "waitState".
 *
 * Results: 
 *      Returns constant string name for "waitState".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static const char *
CpuSchedWaitStateName(CpuSched_WaitState waitState)
{
   switch (waitState) {
   case CPUSCHED_WAIT_NONE:
      return("NONE");
   case CPUSCHED_WAIT_ACTION:
      return("ACTN");
   case CPUSCHED_WAIT_AIO:
      return("AIO");
   case CPUSCHED_WAIT_DRIVER:
      return("DRVR");
   case CPUSCHED_WAIT_FS:
      return("FS");
   case CPUSCHED_WAIT_IDLE:
      return("IDLE");
   case CPUSCHED_WAIT_LOCK:
      return("LOCK");
   case CPUSCHED_WAIT_SEMAPHORE:
      return("SEMA");
   case CPUSCHED_WAIT_MEM:
      return("MEM");
   case CPUSCHED_WAIT_NET:
      return("NET");
   case CPUSCHED_WAIT_REQUEST:
      return("RQ");
   case CPUSCHED_WAIT_RPC:
      return("RPC");
   case CPUSCHED_WAIT_RTC:
      return("RTC");
   case CPUSCHED_WAIT_SCSI:
      return("SCSI");
   case CPUSCHED_WAIT_SLEEP:
      return("SLP");
   case CPUSCHED_WAIT_TLB:
      return("TLB");
   case CPUSCHED_WAIT_WORLDDEATH:
      return("WRLD");
   case CPUSCHED_WAIT_SWAP_AIO:
      return("SWPA");
   case CPUSCHED_WAIT_SWAP_SLOTS:
      return("SWPS");
   case CPUSCHED_WAIT_SWAP_DONE:
      return("SWPD");
   case CPUSCHED_WAIT_SWAP_CPTFILE_OPEN:
      return("SCOP");
   case CPUSCHED_WAIT_SWAP_ASYNC:
      return("SWAC");
   case CPUSCHED_WAIT_UW_SIGWAIT:
      return("USIG");
   case CPUSCHED_WAIT_UW_PIPEREADER:
      return("UPRD");
   case CPUSCHED_WAIT_UW_PIPEWRITER:
      return("UPWR");
   case CPUSCHED_WAIT_UW_EXITCOLLECT:
      return("UJN");
   case CPUSCHED_WAIT_UW_SLEEP:
      return("USLP");
   case CPUSCHED_WAIT_UW_POLL:
      return("UPOL");
   case CPUSCHED_WAIT_UW_PROCDEBUG:
      return("UPROC");
   default:
      return("UNK");
   }      
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedHTSharingName --
 *
 *     Returns a constant string describing the HT sharing constraint "sharing"
 *
 * Results:
 *     Returns a constant string describing the HT sharing constraint "sharing"
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static const char*
CpuSchedHTSharingName(Sched_HTSharing sharing)
{
   if (sharing == CPUSCHED_HT_SHARE_NONE) {
      return "none";
   } else if (sharing == CPUSCHED_HT_SHARE_INTERNALLY) {
      return "internal";
   } else {
      return "any";
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedDumpToLog --
 *
 *     Used as a timer callback to output the current runners and handoffs
 *     of all pcpus to the log.
 *     Grabs cpuSched lock.
 *
 * Results:
 *     None.
 *
 * Side effects:
 *     Prints to log.
 *
 *-----------------------------------------------------------------------------
 */
static void
CpuSchedDumpToLog(void *timeParam, UNUSED_PARAM(Timer_AbsCycles timestamp))
{   
   int dumpTime = (int) timeParam;

   SP_IRQL prevIRQL = CpuSchedLockAllCells();

   _Log("pcpu:     | ");
   FORALL_PCPUS(p) {
      _Log("%4u | ", p);
   } PCPUS_DONE;

   _Log("\nrunning:  | ");
   FORALL_PCPUS(p) {
      CpuSched_Vcpu *vcpu = CpuSchedRunningVcpu(p);
      if (CpuSchedVcpuIsIdle(vcpu)) {
         _Log("     | ");
      } else {
         _Log("%4u | ", VcpuWorldID(vcpu));
      }
   } PCPUS_DONE;

   _Log("\nhandoff:  | ");
   FORALL_PCPUS(p) {
      CpuSched_Vcpu *vcpu = CpuSchedPcpu(p)->handoff;
      if (vcpu == NULL) {
         _Log("     | ");
      } else {
         _Log("%4u | ", VcpuWorldID(vcpu));
      }
   } PCPUS_DONE;

   _Log("\n\n");

   CpuSchedUnlockAllCells(prevIRQL);

   // re-add the timer
   if (!cpuSched.stopSchedDumper) {
      Timer_Add((MY_PCPU + 1) % numPCPUs,
                CpuSchedDumpToLog,
                dumpTime,
                TIMER_ONE_SHOT,
                (void*)dumpTime);
   }
   
}

/*
 *----------------------------------------------------------------------
 *
 * CpuMaskFormat --
 *
 *      Writes processor numbers represented by "mask" into "buf",
 *	using the specified "separator" character between numbers.
 *
 * Results:
 *      Returns the number of characters written to "buf".
 *
 * Side effects:
 *      Modifies "buf".
 *
 *----------------------------------------------------------------------
 */
static int
CpuMaskFormat(CpuMask mask, char *buf, int maxlen, char separator)
{
   Bool first;
   int len;

   // initialize
   first = TRUE;
   buf[0] = '\0';
   len = 0;

   // format each pcpu in mask
   FORALL_PCPUS(i) {
      if (mask & (1 << i)) {
         if (first) {
	    len += snprintf(buf+len, maxlen-len, "%u", i);
            first = FALSE;
         } else {
	    len += snprintf(buf+len, maxlen-len, "%c%u", separator, i);
         }
	 len = MIN(len, maxlen);
      }
   } PCPUS_DONE;

   return(len);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuStateTime --
 *
 *      Returns the cumulative elapsed time spent by "vcpu"
 *	in the specified run "state", including any elapsed
 *	not yet reflected by state meters if "state" is the
 *	current state for "vcpu".  Caller must hold scheduler
 *	cell lock associated with "vcpu".
 *
 * Results:
 *      Returns the cumulative elapsed time spent by "vcpu"
 *	in the specified run "state".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Timer_Cycles
CpuSchedVcpuStateTime(const CpuSched_Vcpu *vcpu, CpuSched_RunState state)
{
   // obtain total from appropriate state meter
   const CpuSched_StateMeter *m = &vcpu->runStateMeter[state];
   Timer_Cycles total = m->elapsed;

   // sanity check
   ASSERT(CpuSchedVsmpCellIsLocked(vcpu->vsmp));

   // update total with elapsed time part-way into current state
   if (vcpu->runState == state) {
      Timer_AbsCycles now = vcpu->vsmp->cell->now;
      if (LIKELY((m->start > 0) && (now > m->start))) {
         total  += (now - m->start);
      }
   }
   return(total);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuReadyTime --
 *
 *      Returns the cumulative elapsed time spent by "vcpu"
 *	in the CPUSCHED_READY run state, including any elapsed
 *	not yet reflected by the ready state meter if "vcpu" is
 *	currently ready.  Note that ready time includes time
 *	spent on the scheduler limbo queues.
 *
 * Results:
 *      Returns the cumulative elapsed time spent by "vcpu"
 *	in the CPUSCHED_READY run state.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Timer_Cycles
CpuSchedVcpuReadyTime(const CpuSched_Vcpu *vcpu)
{
   return(CpuSchedVcpuStateTime(vcpu, CPUSCHED_READY));
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuWaitTime --
 *
 *      Returns the cumulative elapsed time spent by "vcpu"
 *	in the CPUSCHED_WAIT and CPUSCHED_BUSY_WAIT run states,
 *	including any elapsed not yet reflected by the wait
 *	state meters if "vcpu" is currently waiting.
 *
 * Results:
 *      Returns the cumulative elapsed time spent by "vcpu"
 *	in the CPUSCHED_WAIT and CPUSCHED_BUSY_WAIT run states.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Timer_Cycles
CpuSchedVcpuWaitTime(const CpuSched_Vcpu *vcpu)
{
   return(CpuSchedVcpuStateTime(vcpu, CPUSCHED_WAIT) +
          CpuSchedVcpuStateTime(vcpu, CPUSCHED_BUSY_WAIT));
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuLimboTime --
 *
 *      Returns the cumulative elapsed time spent by "vcpu"
 *	on scheduler limbo queues (i.e. deliberately delayed
 *	to enforce specified maximum execution rates).
 *
 * Results:
 *      Returns the cumulative elapsed time spent by "vcpu"
 *	on scheduler limbo queues.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Timer_Cycles
CpuSchedVcpuLimboTime(const CpuSched_Vcpu *vcpu)
{
   const CpuSched_StateMeter *m = &vcpu->limboMeter;
   Timer_Cycles total = m->elapsed;
   if (vcpu->limbo) {
      Timer_AbsCycles now = vcpu->vsmp->cell->now;
      if (LIKELY((m->start > 0) && (now > m->start))) {
         total  += (now - m->start);
      }
   }
   return(total);   
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuSnapshot --
 *
 *      Snapshot current status information from "vcpu" into "s".
 *
 * Results:
 *      Modifies "s" to contain current status information from "vcpu".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedVcpuSnapshot(const CpuSched_Vcpu *vcpu, CpuSchedVcpuSnap *s)
{
   World_Handle *world = World_VcpuToWorld(vcpu);
   const CpuSched_Vsmp *vsmp = vcpu->vsmp;

   // sanity check
   ASSERT(CpuSchedVsmpCellIsLocked(vsmp));

   // identity
   s->worldID = world->worldID;
   s->worldFlags = world->typeFlags;
   s->worldGroupID = VsmpLeaderID(vsmp);
   (void) strncpy(s->worldName, world->worldName, WORLD_NAME_LENGTH);
   
   // group
   s->groupID = CpuSched_GetVsmpLeader(world)->sched.group.groupID;

   // states
   s->coRunState = vsmp->coRunState;
   s->runState = vcpu->runState;
   s->waitState = vcpu->waitState;

   // allocation parameters
   s->alloc = vsmp->alloc;
   s->affinityMask = CpuSchedVcpuHardAffinity(vcpu);
   s->nvcpus = vsmp->vcpus.len;

   // allocation state
   s->base = vsmp->base;
   CpuSchedVtimeContextCopy(&s->vtime, &vsmp->vtime);
   s->vtimeLimit = vsmp->vtimeLimit;
   s->pcpu = vcpu->pcpu;
   s->htSharing = vcpu->vsmp->htSharing;

   // usage, statistics
   s->chargeCyclesTotal = vcpu->chargeCyclesTotal;
   s->sysCyclesTotal = vcpu->sysCyclesTotal;
   s->stats = vcpu->stats;
   s->vsmpStats = vsmp->stats;
   s->htQuarantine = vsmp->htQuarantine;

   // derived stats
   s->ahead = CpuSchedVtimeAhead(vsmp);
   s->waitCycles = CpuSchedVcpuWaitTime(vcpu);
   s->readyCycles = CpuSchedVcpuReadyTime(vcpu);
   s->limboCycles = CpuSchedVcpuLimboTime(vcpu);
   s->haltedCycles = vcpu->waitStateMeter[CPUSCHED_WAIT_IDLE].elapsed;
   s->uptime = vsmp->cell->now - vcpu->stats.uptimeStart;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedPcpuSnapshot --
 *
 *      Snapshot current status information from "pcpu" into "s".
 *
 * Results:
 *      Modifies "s" to contain current status information from "pcpu".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedPcpuSnapshot(CpuSched_Pcpu *pcpu, CpuSchedPcpuSnap *s)
{
   // sanity check
   ASSERT(CpuSchedCellIsLocked(pcpu->cell));

   // identity
   s->id = pcpu->id;
   s->node = NUMA_PCPU2NodeNum(pcpu->id);

   // stats
   s->stats = pcpu->stats;
   CpuSchedPackageHaltLock(pcpu->id);
   s->haltCycles = pcpu->stats.haltCycles;
   s->halted = prdas[pcpu->id]->halted;
   CpuSchedPackageHaltUnlock(pcpu->id);

   // derived stats
   if (pcpu->handoff == NULL) {
      s->handoffID = INVALID_WORLD_ID;
   } else {
      s->handoffID = VcpuWorldID(pcpu->handoff);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedCellSnapshot --
 *
 *      Snapshot current status information for cell "c" into "s".
 *
 * Results:
 *      Modifies "s" to contain current cell status data for "c".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedCellSnapshot(CpuSched_Cell *c, CpuSchedCellSnap *s)
{
   // sanity check
   ASSERT(CpuSchedCellIsLocked(c));

   // identity
   s->id = c->id;

   // pcpus
   s->pcpuMask = c->pcpuMask;
   s->nPCPUs = c->nPCPUs;

   // virtual SMPs
   s->nVsmps = c->vsmps.len;

   // real time
   s->now = c->now;
   s->lostCycles = c->lostCycles;

   // virtual time
   s->vtime = c->vtime;

   // statistics
   s->stats = c->stats;

   // config info
   s->config = c->config;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedGlobalSnapshot --
 *
 *      Snapshot current CpuSched global status information into "s".
 *
 * Results:
 *      Modifies "s" to contain current global CpuSched status data.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedGlobalSnapshot(CpuSchedGlobalSnap *s)
{
   // sanity check
   ASSERT(CpuSchedAllCellsAreLocked());

   // real time
   s->uptime = MY_CELL->now - cpuSchedConst.uptimeStart;

   // virtual time
   s->stride = cpuSchedConst.stride;

   // stats
   s->cellCount = cpuSched.nCells;
   s->vmCount = CpuSchedNumVsmps();
   s->consoleWarpCount = cpuSched.consoleWarpCount;
   s->resetVtimeCount = cpuSched.resetVtimeCount;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedProcNcpusRead --
 *
 *      Callback for read operation on /proc/vmware/sched/ncpus
 *	procfs node.
 *
 * Results:
 *      Returns VMK_OK.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
CpuSchedProcNcpusRead(UNUSED_PARAM(Proc_Entry *entry),
                      char *buf,
                      int *len)
{
   // format pcpu count
   *len = 0;
   Proc_Printf(buf, len,
               "%2u logical\n"
               "%2u physical\n",
               numPCPUs,
               numPCPUs / SMP_LogicalCPUPerPackage());
   return(VMK_OK);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedProcDebugRead --
 *
 *     Proc read handler for /proc/vmware/sched/cpu-debug node
 *
 * Results:
 *     Returns VMK_OK
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static int 
CpuSchedProcDebugRead(UNUSED_PARAM(Proc_Entry *e), char *buf, int *len)
{
   // initialize
   *len = 0;
   
   // format debugging info
   Proc_Printf(buf, len,
               "IdlePreempts:    %u\n", 
               cpuSched.numIdlePreempts);

   Proc_Printf(buf, len,
               "\n"
               "pcpu    dyield  dyieldFail     failPct  "
               " idleHalts idleHaltIntr"
               "\n");
   FORALL_SCHED_PCPUS(p) {
      uint32 failed, failPct;
      failed = p->stats.dyieldFailed;
      if (failed > 0) {
         failPct = (failed * 100) / (p->stats.dyield + failed);
      } else {
         failPct = 0;
      }

      Proc_Printf(buf, len,
                  "  %2u  %8u    %8u        %3u%%"
                  "%12Lu %12Lu"
                  "\n",
                  p->id,
                  p->stats.dyield,
                  p->stats.dyieldFailed,
                  failPct,
                  p->stats.idleHaltEnd,
                  p->stats.idleHaltEndIntr);
   } SCHED_PCPUS_DONE;

   if (vmx86_debug) {
      FORALL_SCHED_PCPUS(p) {
         Proc_Printf(buf, len, "\npcpu %u vcpuWaitFor switch times:\n", p->id);
         Histogram_ProcFormat(p->switchWaitHisto, "  ", buf, len);
      } SCHED_PCPUS_DONE;

      FORALL_SCHED_PCPUS(p) {
         Proc_Printf(buf, len, "\npcpu %u halt times:\n", p->id);
         Histogram_ProcFormat(p->haltHisto, "  ", buf, len);
      } SCHED_PCPUS_DONE;
   }

   return(VMK_OK);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedProcDebugWrite --
 *
 *     Proc write handler for /proc/vmware/sched/cpu-debug node
 *
 * Results:
 *     Returns VMK_OK or VMK_BAD_PARAM if command not understood
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static int 
CpuSchedProcDebugWrite(UNUSED_PARAM(Proc_Entry *e), char *buf, int *len)
{
   int argc;
   char *argv[2];
   
   argc = Parse_Args(buf, argv, 2);
   if (argc < 1) {
      Warning("no arguments found");
      return (VMK_BAD_PARAM);
   }

   if (!strcmp(argv[0], "dump")) {
      int dumpTime;
      if (argc < 2 ||
          Parse_Int(argv[1], strlen(argv[1]), &dumpTime) != VMK_OK) {
         Warning("second argument invalid");
         return (VMK_BAD_PARAM);
      }

      cpuSched.stopSchedDumper = FALSE;
      Log("starting scheduler dumper");
      Timer_Add(MY_PCPU,
                CpuSchedDumpToLog,
                dumpTime,
                TIMER_ONE_SHOT,
                (void*)dumpTime);
      return (VMK_OK);
   } else if (!strcmp(argv[0], "stop")) {
      Log("stopping scheduler dumper");
      cpuSched.stopSchedDumper = TRUE;
      return (VMK_OK);
   } else {
      Warning("command not understood");
      return (VMK_BAD_PARAM);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedProcIdleRead --
 *
 *      Callback for read operation on /proc/vmware/sched/idle
 *	procfs node.
 *
 * Results:
 *      Returns VMK_OK.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
CpuSchedProcIdleRead(UNUSED_PARAM(Proc_Entry *entry),
                     char *buf,
                     int  *len)
{
   // initialize
   *len = 0;

   // format header
   Proc_Printf(buf, len, "cpu        idlesec        usedsec\n");

   // iterate over processors
   FORALL_SCHED_PCPUS(schedPcpu) {
      uint32 usecIdle, usecUsed;
      uint64 secIdle, secUsed;
      SP_IRQL prevIRQL;

      // compute cumulative idle, nonidle time
      prevIRQL = CpuSchedCellLock(schedPcpu->cell);
      CpuSched_UsageToSec(schedPcpu->idleCycles, &secIdle, &usecIdle);
      CpuSched_UsageToSec(schedPcpu->usedCycles, &secUsed, &usecUsed);
      CpuSchedCellUnlock(schedPcpu->cell, prevIRQL);

      // format per-scheduler data
      Proc_Printf(buf, len,
		  "%3d  %9Ld.%03d  %9Ld.%03d\n",
		  schedPcpu->id,
		  secIdle,
		  usecIdle / 1000,
		  secUsed,
		  usecUsed / 1000);
   } SCHED_PCPUS_DONE;

   // everything OK
   return(VMK_OK);
}

static void
CpuSchedGlobalSnapFormat(const CpuSchedGlobalSnap *s, char *buf, int *len)
{
   uint32 usecUptime;
   uint64 secUptime;

   // sanity check
   ASSERT(CpuSchedSnapIsLocked());

   // format header
   Proc_Printf(buf, len,
               "cells vms        uptime     stride    warps\n");

   // convert cycles to seconds
   Timer_TCToSec(s->uptime, &secUptime, &usecUptime);

   // format data
   Proc_Printf(buf, len,
               "%5d %3d %9Lu.%03u %10d %8u\n",
               // cell, vm counts
               s->cellCount,
               s->vmCount,
               // uptime
               secUptime,
               usecUptime / 1000,
               // alloc
               s->stride,
               // console warps
               s->consoleWarpCount);

   // separator
   Proc_Printf(buf, len, "\n");
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedCellSnapHeader --
 *
 *      Writes cell status header into "buf".
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
CpuSchedCellSnapHeader(char *buf, int *len)
{
   // format header
   Proc_Printf(buf, len,
               "cell "
               "pcpus managed           "
               "vms "
               "                 now "
               "         lost "
               "               vtime "
               "remotetry try%% "
               "\n");
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedCellSnapFormat --
 *
 *      Writes status information for "s" into "buf".
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
CpuSchedCellSnapFormat(const CpuSchedCellSnap *s, char *buf, int *len)
{
   char pcpuBuf[CPUSCHED_CPUMASK_BUF_LEN];
   uint32 usecLost, tryCount, tryPct;
   uint64 secLost;

   // conversions
   CpuMaskFormat(s->pcpuMask, pcpuBuf, CPUSCHED_CPUMASK_BUF_LEN, ',');
   Timer_TCToSec(s->lostCycles, &secLost, &usecLost);

   // compute remote cell lock success rate
   tryCount = s->stats.remoteLockSuccess + s->stats.remoteLockFailure;
   if (tryCount > 0) {
      tryPct = (100 * s->stats.remoteLockSuccess) / tryCount;
   } else {
      tryPct = 0;
   }

   // format data
   Proc_Printf(buf, len,
               "%4u "
               "%5u %-16s "
               "%4u "
               "%20Lu "
               "%9Lu.%03u "
               "%20Ld "
               "%9u %4u"
               "\n",
               s->id,
               s->nPCPUs,
               pcpuBuf,
               s->nVsmps,
               s->now,
               secLost,
               usecLost / 1000,
               s->vtime,
               tryCount,
               tryPct);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedPcpuSnapHeader --
 *
 *      Writes pcpu status header into "buf".
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
CpuSchedPcpuSnapHeader(char *buf, int *len)
{
   // format header
   Proc_Printf(buf, len,
               "cpu rnext      sched      preempt    "
               "timer        hltsec     rsipi  "
               "handoff   waitsw   halted node    "
               "gvclookup hit%%"
               "\n");
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedPcpuSnapFormat --
 *
 *      Writes status information for "s" into "buf".
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
CpuSchedPcpuSnapFormat(const CpuSchedPcpuSnap *s, char *buf, int *len)
{
   uint32 haltedUsec, groupHitPct;
   uint64 haltedSec;

   // sanity check
   ASSERT(CpuSchedSnapIsLocked());
   Timer_TCToSec(s->haltCycles, &haltedSec, &haltedUsec);

   if (s->stats.groupLookups > 0) {
      groupHitPct = (100 * s->stats.groupHits) / s->stats.groupLookups;
   } else {
      groupHitPct = 0;
   }

   // format status
   Proc_Printf(buf, len,
               "%3d %5d %10u %10u "
               "%10u %10Lu.%02u  %8u "
               "%8u %8u      %3s   %2u "
               "%12Lu %4u"
               "\n",
               s->id,
               s->handoffID,
               s->stats.yield,
               s->stats.preempts,
               s->stats.timer,
               haltedSec,
               haltedUsec / 10000,
               s->stats.ipi,
               s->stats.handoff,
               s->stats.switchWait,
               s->halted ? "YES" : "NO",
               s->node,
               s->stats.groupLookups,
               groupHitPct);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuSnapHeader --
 *
 *      Writes vcpu status header into "buf".  If "verbose" is set,
 *	the header includes additional detail fields.
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
CpuSchedVcpuSnapHeader(Bool verbose, char *buf, int *len)
{
   // basic header
   Proc_Printf(buf, len,
               "vcpu   vm type name         "
               "       uptime "
               "status   costatus       "
               "usedsec     syssec "
               "wait           waitsec "
               "      idlesec      readysec "
               "cpu affinity         "
               "htsharing  "
               "  min    max    units shares group        "
               "emin      extrasec ");

   // verbose header
   if (verbose) {
      Proc_Printf(buf, len,
                  " | "
                  "  bmin   bmax   base "
                  "   maxlimited "
                  "   switch  pmigs  imigs  cmigs htquar"
                  "           vtime            ahead "
                  "         vtextra "
                  "         vtlimit "
                  "          vtaged");
   }

   // newline
   Proc_Printf(buf, len, "\n");
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuSnapFormat --
 *
 *      Writes status information for "s" into "buf".  If "verbose"
 *	is set, the status information includes additional details.
 *	Caller must hold CpuSched snapshot lock.
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
CpuSchedVcpuSnapFormat(const CpuSchedVcpuSnap *s,
                       Bool verbose,
                       char *buf,
                       int *len)
{
   char affinityBuf[CPUSCHED_CPUMASK_BUF_LEN], worldTypeBuf[8];
   char groupNameBuf[SCHED_GROUP_NAME_LEN];
   uint32 usecCharge, usecWait, usecUptime, usecSys, usecBonus, usecHalted, usecReady, usecLimbo;
   uint64 secCharge, secWait, secUptime, secSys, secBonus, secHalted, secReady, secLimbo;
   Timer_Cycles nonLimboReady;
   uint32 allocMax;

   // sanity check
   ASSERT(CpuSchedSnapIsLocked());

   // compute non-limbo ready time
   nonLimboReady = 0;
   if (LIKELY(s->readyCycles > s->limboCycles)) {
      nonLimboReady = s->readyCycles - s->limboCycles;
   }

   // format world affinity, type
   CpuMaskFormat(s->affinityMask, affinityBuf, CPUSCHED_CPUMASK_BUF_LEN, ',');
   World_FormatTypeFlags(s->worldFlags, worldTypeBuf, sizeof(worldTypeBuf));

   // format group name
   if (Sched_GroupIDToName(s->groupID,
                           groupNameBuf,
                           SCHED_GROUP_NAME_LEN) != VMK_OK) {
      strcpy(groupNameBuf, "unknown");
   }

   // convert cpu consumption cycles to seconds
   CpuSched_UsageToSec(s->chargeCyclesTotal, &secCharge, &usecCharge);
   CpuSched_UsageToSec(s->sysCyclesTotal, &secSys, &usecSys);
   CpuSched_UsageToSec(s->vsmpStats.bonusCyclesTotal, &secBonus, &usecBonus);

   // convert elapsed time cycles to seconds
   Timer_TCToSec(s->waitCycles, &secWait, &usecWait);
   Timer_TCToSec(nonLimboReady, &secReady, &usecReady);
   Timer_TCToSec(s->limboCycles, &secLimbo, &usecLimbo);
   Timer_TCToSec(s->haltedCycles, &secHalted, &usecHalted);
   Timer_TCToSec(s->uptime, &secUptime, &usecUptime);

   // compute effective max
   if (CpuSchedEnforceMax(&s->alloc)) {
      allocMax = s->alloc.max;
   } else {
      allocMax = cpuSchedConst.unitsPerPkg[s->alloc.units] * s->nvcpus;
   }

   // basic status
   Proc_Printf(buf, len,
               "%4d %4d %-4.4s %-12.12s " // identity
               "%9Lu.%03u "               // uptime
               "%-8s %-8s "               // run/co-run states
               "%9Lu.%03u %6Lu.%03u "     // charged and sys
               "%-8s %9Lu.%03u "          // wait state and wait time
               "%9Lu.%03u %9Lu.%03u "     // halted and ready time
               "%3d %-16s "               // pcpu and affinity
               "%-9s "                    // htsharing
               "%6u %6u %8s %6u "	  // min, max, shares
               "%-12.12s "  		  // scheduler group
               "%4u %9Lu.%03u ",          // emin, extrasec
               // identity
               s->worldID,
               s->worldGroupID,
               worldTypeBuf,
               s->worldName,
               // uptime
               secUptime,
               usecUptime / 1000,
               // run status
               CpuSchedRunStateName(s->runState),
               CpuSchedCoRunStateName(s->coRunState),
               // run stats
               secCharge,
               usecCharge / 1000,
               secSys,
               usecSys / 1000,
               // wait stats
               CpuSchedWaitStateName(s->waitState),
               secWait,
               usecWait / 1000,
               secHalted,
               usecHalted / 1000,
               secReady,
               usecReady / 1000,
               // placement stats
               s->pcpu,
               affinityBuf,
               // external allocation state
               CpuSchedHTSharingName(s->htSharing),
               s->alloc.min,
               allocMax,
               Sched_UnitsToString(s->alloc.units),
               s->alloc.shares,
               groupNameBuf,
               // entitled min, usage above entitlement
               CpuSchedBaseSharesToUnits(s->base.shares, SCHED_UNITS_PERCENT),
               secBonus,
               usecBonus / 1000);

   // verbose status
   if (verbose) {
      // format status
      Proc_Printf(buf, len,
                  " | " 
                  "%6u %6u %6u "
                  "%9Lu.%03u  "
                  "%8u %6u %6u %6u %6u",
                  // internal allocation state
                  s->base.min,
                  s->base.max,
                  s->base.shares,
                  // limbo time
                  secLimbo,
                  usecLimbo / 1000,
                  // misc stats
                  s->stats.worldSwitch,
                  s->stats.pkgMigrate,
                  s->stats.wakeupMigrateIdle,
                  s->vsmpStats.cellMigrate,
                  s->htQuarantine);

      // format vtimes, special-case max to reduce output width
      if (s->vtime.main == CPUSCHED_VTIME_MAX) {
         Proc_Printf(buf, len, "%16s %16s ", "max", "max");
      } else {
         Proc_Printf(buf, len, "%16Ld %16Ld ", s->vtime.main, s->ahead);
      }
      if (s->vtime.extra == CPUSCHED_VTIME_MAX) {
         Proc_Printf(buf, len, "%16s ", "max");
      } else {
         Proc_Printf(buf, len, "%16Ld ", s->vtime.extra);
      }
      Proc_Printf(buf, len, "%16Ld %16Ld",
                  s->vtimeLimit,
                  s->vsmpStats.vtimeAged);
   }

   // format newline
   Proc_Printf(buf, len, "\n");
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedVcpuSnapIndexCompare --
 *
 *	Compares the two CpuSchedVcpuSnap structures associated
 *	with the indexes represented by "a" and "b" in the global
 *	vcpu snapshot area.  Uses group id as the primary key,
 *	and world id as the secondary key.  Returns 1 if "a" is
 *	greater, -1 if "b" is greater, and 0 if they are equal.
 *
 * Results:
 *	Returns -1, 0, or 1 depending on comparison
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */
static int
CpuSchedVcpuSnapIndexCompare(const void *a, const void *b)
{
   CpuSchedVcpuSnap *aSnap, *bSnap;
   uint32 aIndex, bIndex;
   
   aIndex = *((uint32 *) a);
   bIndex = *((uint32 *) b);
   ASSERT(aIndex < CPUSCHED_VCPUS_MAX);
   ASSERT(bIndex < CPUSCHED_VCPUS_MAX);

   aSnap = &cpuSched.procSnap.vcpu[aIndex];
   bSnap = &cpuSched.procSnap.vcpu[bIndex];

   if (aSnap->worldGroupID < bSnap->worldGroupID) {
      return -1;
   } else if (aSnap->worldGroupID > bSnap->worldGroupID) {
      return 1;
   } else {
      if (aSnap->worldID < bSnap->worldID) {
         return -1;
      } else if (aSnap->worldID > bSnap->worldID) {
         return 1;
      } else {
         return 0;
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedProcRead --
 *
 *      Callback for read operation on /proc/vmware/sched/cpu
 *	procfs node.
 *
 * Results:
 *      Returns VMK_OK.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
CpuSchedProcRead(Proc_Entry *entry, char *buf, int *len)
{
   Bool verbose = (entry->private == 0) ? FALSE : TRUE;
   uint32 i, nVcpus, nPcpus, nCells, tmpSort;
   SP_IRQL prevIRQL;

   // initialize
   *len = 0;
   nPcpus = 0;
   nVcpus = 0;
   nCells = 0;
   
   // acquire snap, sched locks
   CpuSchedSnapLock();
   prevIRQL = CpuSchedLockAllCells();

   // snapshot global status
   if (verbose) {
      CpuSchedGlobalSnapshot(&cpuSched.procSnap.global);
   }

   // snapshot cell status
   if (verbose) {
      FORALL_CELLS(c) {
         CpuSchedCellSnapshot(c, &cpuSched.procSnap.cell[nCells]);
         nCells++;
      } CELLS_DONE;
   }

   // snapshot pcpu status
   if (verbose) {
      FORALL_SCHED_PCPUS(schedPcpu) {
         CpuSchedPcpuSnap *s = &cpuSched.procSnap.pcpu[nPcpus];
         CpuSchedPcpuSnapshot(schedPcpu, s);
         nPcpus++;
      } SCHED_PCPUS_DONE;
   }

   // snapshot vcpu status
   FORALL_CELLS(c) {
      FORALL_CELL_VSMPS(c, vsmp) {
         FORALL_VSMP_VCPUS(vsmp, vcpu) {
            CpuSchedVcpuSnap *s = &cpuSched.procSnap.vcpu[nVcpus];
            CpuSchedVcpuSnapshot(vcpu, s);
            nVcpus++;
         } VSMP_VCPUS_DONE;
      } CELL_VSMPS_DONE;
   } CELLS_DONE;

   // release sched locks
   CpuSchedUnlockAllCells(prevIRQL);   

   // format global status
   if (verbose) {
      CpuSchedGlobalSnapFormat(&cpuSched.procSnap.global, buf, len);
   }

   // format cell status
   if (verbose) {
      CpuSchedCellSnapHeader(buf, len);
      for (i = 0; i < nCells; i++) {
         CpuSchedCellSnap *s = &cpuSched.procSnap.cell[i];
         CpuSchedCellSnapFormat(s, buf, len);
      }

      // format separator
      Proc_Printf(buf, len, "\n");
   }

   // format pcpu status
   if (verbose) {
      CpuSchedPcpuSnapHeader(buf, len);
      for (i = 0; i < nPcpus; i++) {
         CpuSchedPcpuSnap *s = &cpuSched.procSnap.pcpu[i];
         CpuSchedPcpuSnapFormat(s, buf, len);
      }

      // format separator
      Proc_Printf(buf, len, "\n");      
   }

   // sort vcpus by id
   for (i = 0; i < nVcpus; i++) {
      cpuSched.procSnap.vcpuSort[i] = i;
   }
   heapsort(cpuSched.procSnap.vcpuSort,
            nVcpus,
            sizeof(uint32),
            CpuSchedVcpuSnapIndexCompare,
            &tmpSort);

   // format vcpu status
   CpuSchedVcpuSnapHeader(verbose, buf, len);
   for (i = 0; i < nVcpus; i++) {
      uint32 vcpuIndex = cpuSched.procSnap.vcpuSort[i];
      CpuSchedVcpuSnap *s = &cpuSched.procSnap.vcpu[vcpuIndex];
      CpuSchedVcpuSnapFormat(s, verbose, buf, len);
   }

   // release snap lock
   CpuSchedSnapUnlock();

   // everything OK
   return(VMK_OK);
}


/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVsmpStateTimesFormatHeader --
 *
 *      Writes vsmp state times header into "buf".
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
CpuSchedVsmpStateTimesFormatHeader(char *buf, int *len)
{
   // basic header
   Proc_Printf(buf, len,
               "vcpu   vm name         "
               "       uptime "
               "      charged "
               "          sys "
               "   sysoverlap "
               "          run "
               "         wait "
               "        waitb "
               "        ready "
               "       costop "
               "        corun "
               "   maxlimited"
               "\n");
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuStateTimesFormat --
 *
 *      Writes information about elapsed time spent in various
 *	scheduler states for "vcpu" into "buf".
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
CpuSchedVcpuStateTimesFormat(const CpuSched_Vcpu *vcpu, char *buf, int *len)
{
   uint32 usecUptime, usecCharge, usecSys, usecOverlap, usecLimbo;
   uint64 secUptime, secCharge, secSys, secOverlap, secLimbo;
   uint32 usecState[CPUSCHED_NUM_RUN_STATES];
   uint64 secState[CPUSCHED_NUM_RUN_STATES];
   CpuSched_RunState s;
   Timer_Cycles uptime;

   // sanity check
   ASSERT(CpuSchedVsmpCellIsLocked(vcpu->vsmp));

   // compute uptime
   uptime = vcpu->vsmp->cell->now - vcpu->stats.uptimeStart;

   // convert cpu consumption cycles to seconds
   CpuSched_UsageToSec(vcpu->sysCyclesTotal, &secSys, &usecSys);   
   CpuSched_UsageToSec(vcpu->sysOverlapTotal, &secOverlap, &usecOverlap);
   CpuSched_UsageToSec(vcpu->chargeCyclesTotal, &secCharge, &usecCharge);

   // convert elapsed time cycles to seconds
   for (s = 0; s < CPUSCHED_NUM_RUN_STATES; s++) {
      const CpuSched_StateMeter *m = &vcpu->runStateMeter[s];
      Timer_TCToSec(m->elapsed, &secState[s], &usecState[s]);
   }
   Timer_TCToSec(uptime, &secUptime, &usecUptime);
   Timer_TCToSec(vcpu->limboMeter.elapsed, &secLimbo, &usecLimbo);

   // format states data
   Proc_Printf(buf, len,
               "%4d %4d %-12.12s "
               "%9Lu.%03u %9Lu.%03u %9Lu.%03u %9Lu.%03u %9Lu.%03u "
               "%9Lu.%03u %9Lu.%03u %9Lu.%03u %9Lu.%03u %9Lu.%03u "
               "%9Lu.%03u"
               "\n",
               // identity
               VcpuWorldID(vcpu),
               VsmpLeaderID(vcpu->vsmp),
               World_VcpuToWorld(vcpu)->worldName,
               // uptime
               secUptime,
               usecUptime / 1000,
               // charged
               secCharge,
               usecCharge / 1000,
               // sys
               secSys,
               usecSys / 1000,
               // sys overlap
               secOverlap,
               usecOverlap / 1000,
               // run
               secState[CPUSCHED_RUN],
               usecState[CPUSCHED_RUN] / 1000,
               // wait
               secState[CPUSCHED_WAIT],
               usecState[CPUSCHED_WAIT] / 1000,
               // busy-wait
               secState[CPUSCHED_BUSY_WAIT],
               usecState[CPUSCHED_BUSY_WAIT] / 1000,
               // ready
               secState[CPUSCHED_READY],
               usecState[CPUSCHED_READY] / 1000,
               // co-stop
               secState[CPUSCHED_READY_COSTOP],
               usecState[CPUSCHED_READY_COSTOP] / 1000,
               // co-run
               secState[CPUSCHED_READY_CORUN],
               usecState[CPUSCHED_READY_CORUN] / 1000,
               // limbo
               secLimbo,
               usecLimbo / 1000);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVsmpStateTimesFormat --
 *
 *      Writes scheduler states information for "vsmp" into "buf".
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
CpuSchedVsmpStateTimesFormat(const CpuSched_Vsmp *vsmp, char *buf, int *len)
{
   FORALL_VSMP_VCPUS(vsmp, vcpu) {
      CpuSchedVcpuStateTimesFormat(vcpu, buf, len);
   } VSMP_VCPUS_DONE;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedProcStateTimesRead --
 *
 *      Callback for read operation on /proc/vmware/sched/cpu-state-times
 *	procfs node.
 *
 * Results:
 *      Returns VMK_OK.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
CpuSchedProcStateTimesRead(UNUSED_PARAM(Proc_Entry *entry),
                           char *buf,
                           int *len)
{
   // initialize
   *len = 0;
   
   // format header
   CpuSchedVsmpStateTimesFormatHeader(buf, len);

   FORALL_CELLS_UNLOCKED(c) {
      SP_IRQL prevIRQL;

      // OPT: locking here is expensive
      prevIRQL = CpuSchedCellLock(c);

      FORALL_CELL_VSMPS(c, vsmp) {
         // format basic scheduling status
         CpuSchedVsmpStateTimesFormat(vsmp, buf, len);
      } CELL_VSMPS_DONE;

      CpuSchedCellUnlock(c, prevIRQL);
   } CELLS_UNLOCKED_DONE;
   
   // everything OK
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVsmpStateCountsFormatHeader --
 *
 *      Writes vsmp state counts header into "buf".
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
CpuSchedVsmpStateCountsFormatHeader(char *buf, int *len)
{
   // basic header
   Proc_Printf(buf, len,
               "vcpu   vm name         "
               "    switch "
               "   migrate "
               "      halt "
               "       run "
               "      wait "
               "     waitb "
               "     ready "
               "    costop "
               "     corun "
               "maxlimited"
               "\n");
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuStateCountsFormat --
 *
 *      Writes information about counts associated with various
 *	scheduler states for "vcpu" into "buf".
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
CpuSchedVcpuStateCountsFormat(const CpuSched_Vcpu *vcpu, char *buf, int *len)
{
   // format states data
   Proc_Printf(buf, len,
               "%4d %4d %-12.12s "
               "%10u %10u %10u "
               "%10u %10u %10u %10u %10u %10u %10u\n",
               // identity
               VcpuWorldID(vcpu),
               VsmpLeaderID(vcpu->vsmp),
               World_VcpuToWorld(vcpu)->worldName,
               // event counts
               vcpu->stats.worldSwitch,
               vcpu->stats.migrate,
               vcpu->stats.halt,
               // state counts
               vcpu->runStateMeter[CPUSCHED_RUN].count,
               vcpu->runStateMeter[CPUSCHED_WAIT].count,
               vcpu->runStateMeter[CPUSCHED_BUSY_WAIT].count,
               vcpu->runStateMeter[CPUSCHED_READY].count,
               vcpu->runStateMeter[CPUSCHED_READY_COSTOP].count,
               vcpu->runStateMeter[CPUSCHED_READY_CORUN].count,
               vcpu->limboMeter.count);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVsmpStateCountsFormat --
 *
 *      Writes scheduler state counts information for "vsmp"
 *	into "buf".
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
CpuSchedVsmpStateCountsFormat(const CpuSched_Vsmp *vsmp, char *buf, int *len)
{
   int i;

   for (i = 0; i < vsmp->vcpus.len; i++) {
      CpuSched_Vcpu *vcpu = vsmp->vcpus.list[i];
      CpuSchedVcpuStateCountsFormat(vcpu, buf, len);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedProcStateCountsRead --
 *
 *      Callback for read operation on /proc/vmware/sched/cpu-state-counts
 *	procfs node.
 *
 * Results:
 *      Returns VMK_OK.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
CpuSchedProcStateCountsRead(UNUSED_PARAM(Proc_Entry *entry),
                            char *buf,
                            int *len)
{
   // initialize
   *len = 0;
   
   // format header
   CpuSchedVsmpStateCountsFormatHeader(buf, len);

   FORALL_CELLS_UNLOCKED(c) {
      SP_IRQL prevIRQL;

      // OPT: locking here is expensive
      prevIRQL = CpuSchedCellLock(c);

      // basic scheduling status
      FORALL_CELL_VSMPS(c, vsmp) {
         CpuSchedVsmpStateCountsFormat(vsmp, buf, len);
      } CELL_VSMPS_DONE;
   
      CpuSchedCellUnlock(c, prevIRQL);
   } CELLS_UNLOCKED_DONE;

   // everything OK
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedRunTimesFormatHeader --
 *
 *      Writes vcpu run times header into "buf".
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
CpuSchedRunTimesFormatHeader(char *buf, int *len)
{
   Proc_Printf(buf, len, "vcpu   vm name        ");
   FORALL_PCPUS(p) {
      Proc_Printf(buf, len, "         cpu%02u", p);
   } PCPUS_DONE;
   Proc_Printf(buf, len, "\n");
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVcpuRunTimesFormat --
 *
 *      Writes per-pcpu run times for "vcpu" into "buf".
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
CpuSchedVcpuRunTimesFormat(const CpuSched_Vcpu *vcpu, char *buf, int *len)
{
   CpuSched_Vsmp *vsmp = vcpu->vsmp;

   // sanity check
   ASSERT(CpuSchedVsmpCellIsLocked(vsmp));

   // format identity
   Proc_Printf(buf, len,
               "%4d %4d %-12.12s",
               VcpuWorldID(vcpu),
               VsmpLeaderID(vsmp),
               World_VcpuToWorld(vcpu)->worldName);

   // format pcpu run times
   FORALL_PCPUS(p) {
      uint64 sec;
      uint32 usec;
      Timer_TCToSec(vcpu->pcpuRunTime[p], &sec, &usec);
      Proc_Printf(buf, len, " %9Lu.%03u", sec, usec / 1000);
   } PCPUS_DONE;

   // format newline
   Proc_Printf(buf, len, "\n");
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedProcPcpuRunTimesRead --
 *
 *      Callback for read operation on /proc/vmware/sched/cpu-run-times
 *	procfs node.
 *
 * Results:
 *      Returns VMK_OK.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
CpuSchedProcPcpuRunTimesRead(UNUSED_PARAM(Proc_Entry *entry),
                             char *buf,
                             int *len)
{
   // initialize
   *len = 0;
   
   // format header
   CpuSchedRunTimesFormatHeader(buf, len);

   FORALL_CELLS_UNLOCKED(c) {
      SP_IRQL prevIRQL;

      // OPT: locking here is expensive
      prevIRQL = CpuSchedCellLock(c);

      // format per-pcpu runtimes
      FORALL_CELL_VSMPS(c, vsmp) {
         FORALL_VSMP_VCPUS(vsmp, vcpu) {
            CpuSchedVcpuRunTimesFormat(vcpu, buf, len);
         } VSMP_VCPUS_DONE;
      } CELL_VSMPS_DONE;

      CpuSchedCellUnlock(c, prevIRQL);
   } CELLS_UNLOCKED_DONE;

   // everything OK
   return(VMK_OK);
}


/*
 *----------------------------------------------------------------------
 *
 * CpuSchedProcWorldStatusRead --
 *
 *      Callback for read operation on /proc/vmware/vm/<id>/cpu/status
 *	procfs node.
 *
 * Results:
 *      Returns VMK_OK.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
CpuSchedProcWorldStatusRead(Proc_Entry *entry, char *buf, int *len)
{
   World_Handle *world = (World_Handle *) entry->private;
   CpuSched_Vcpu *vcpu = World_CpuSchedVcpu(world);
   CpuSched_Vsmp *vsmp = vcpu->vsmp;
   CpuSchedVcpuSnap *s = &cpuSched.procSnap.vcpu[0];
   SP_IRQL prevIRQL;

   // initialize
   *len = 0;

   // format header
   CpuSchedVcpuSnapHeader(FALSE, buf, len);

   // acquire snap, sched locks
   CpuSchedSnapLock();
   prevIRQL = CpuSchedVsmpCellLock(vsmp);

   // snapshot status
   CpuSchedVcpuSnapshot(vcpu, s);

   // release sched lock
   CpuSchedVsmpCellUnlock(vsmp, prevIRQL);

   // format status
   CpuSchedVcpuSnapFormat(s, FALSE, buf, len);

   // release snap lock
   CpuSchedSnapUnlock();

   // everything OK
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedProcWorldStateTimesRead --
 *
 *      Callback for read operation on procfs node
 *	/proc/vmware/vm/<id>/cpu/state-times.
 *
 * Results:
 *      Returns VMK_OK.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
CpuSchedProcWorldStateTimesRead(Proc_Entry *entry, char *buf, int *len)
{
   World_Handle *world = (World_Handle *) entry->private;
   CpuSched_Vcpu *vcpu = World_CpuSchedVcpu(world);
   CpuSched_Vsmp *vsmp = vcpu->vsmp;
   SP_IRQL prevIRQL;

   // initialize
   *len = 0;

   // format header
   CpuSchedVsmpStateTimesFormatHeader(buf, len);

   // format vcpu status
   prevIRQL = CpuSchedVsmpCellLock(vsmp);
   CpuSchedVcpuStateTimesFormat(vcpu, buf, len);
   CpuSchedVsmpCellUnlock(vsmp, prevIRQL);

   // everything OK
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedProcWorldStateCountsRead --
 *
 *      Callback for read operation on procfs node
 *	/proc/vmware/vm/<id>/cpu/state-counts.
 *
 * Results:
 *      Returns VMK_OK.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
CpuSchedProcWorldStateCountsRead(Proc_Entry *entry, char *buf, int *len)
{
   World_Handle *world = (World_Handle *) entry->private;
   CpuSched_Vcpu *vcpu = World_CpuSchedVcpu(world);
   CpuSched_Vsmp *vsmp = vcpu->vsmp;
   SP_IRQL prevIRQL;

   // initialize
   *len = 0;

   // format header
   CpuSchedVsmpStateCountsFormatHeader(buf, len);

   // format vcpu status
   prevIRQL = CpuSchedVsmpCellLock(vsmp);
   CpuSchedVcpuStateCountsFormat(vcpu, buf, len);
   CpuSchedVsmpCellUnlock(vsmp, prevIRQL);

   // everything OK
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedProcWorldRunTimesRead --
 *
 *      Callback for read operation on procfs node
 *	/proc/vmware/vm/<id>/cpu/run-times.
 *
 * Results:
 *      Returns VMK_OK.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
CpuSchedProcWorldRunTimesRead(Proc_Entry *entry, char *buf, int *len)
{
   World_Handle *world = (World_Handle *) entry->private;
   CpuSched_Vcpu *vcpu = World_CpuSchedVcpu(world);
   CpuSched_Vsmp *vsmp = vcpu->vsmp;
   SP_IRQL prevIRQL;

   // initialize
   *len = 0;

   // format header
   CpuSchedRunTimesFormatHeader(buf, len);

   // format stats
   prevIRQL = CpuSchedVsmpCellLock(vsmp);
   CpuSchedVcpuRunTimesFormat(vcpu, buf, len);
   CpuSchedVsmpCellUnlock(vsmp, prevIRQL);

   // everything OK
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedProcWorldWaitStatsRead --
 *
 *      Callback for read operation on procfs node
 *	/proc/vmware/vm/<id>/cpu/wait-stats.
 *
 * Results:
 *      Returns VMK_OK.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
CpuSchedProcWorldWaitStatsRead(Proc_Entry *entry, char *buf, int *len)
{
   World_Handle *world = (World_Handle *) entry->private;
   CpuSched_Vcpu *vcpu = World_CpuSchedVcpu(world);
   CpuSched_Vsmp *vsmp = vcpu->vsmp;
   CpuSched_WaitState s;
   SP_IRQL prevIRQL;

   // initialize
   *len = 0;

   // format header
   Proc_Printf(buf, len,
               "type         count       elapsed    prevent     force\n");

   prevIRQL = CpuSchedVsmpCellLock(vsmp);

   // format stats (skip CPUSCHED_WAIT_NONE=0)
   for (s = 1; s < CPUSCHED_NUM_WAIT_STATES; s++) {
      const CpuSched_StateMeter *m = &vcpu->waitStateMeter[s];

      // report non-zero states
      if (m->count > 0) {
         uint32 usec;
         uint64 sec;
         Timer_TCToSec(m->elapsed, &sec, &usec);
         Proc_Printf(buf, len, "%-8s  %8d  %9Lu.%03u  %8d  %8d\n",
                     CpuSchedWaitStateName(s),
                     m->count,
                     sec, usec / 1000,
                     vcpu->stats.actionPreventWait[s],
                     vcpu->stats.forceWakeup[s]);
      }
   }

   CpuSchedVsmpCellUnlock(vsmp, prevIRQL);

   // everything OK
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedParseShares --
 *
 *      Parses "buf" as a cpu shares value.  The special values
 *	"high", "normal", and "low" are converted into appropriate
 *	corresponding numeric values based on "nvcpus".
 *
 * Results:
 *      Returns VMK_OK and stores value in "shares" on success.
 *	Returns VMK_BAD_PARAM if "buf" could not be parsed.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
CpuSchedParseShares(const char *buf, uint32 nvcpus, uint32 *shares)
{
   // sanity check
   ASSERT(nvcpus <= CPUSCHED_VSMP_VCPUS_MAX);

   // parse special values: high, normal, low
   if (strcmp(buf, "high") == 0) {
      *shares = CPUSCHED_SHARES_HIGH(nvcpus);
      return(VMK_OK);
   } else if (strcmp(buf, "normal") == 0) {
      *shares = CPUSCHED_SHARES_NORMAL(nvcpus);
      return(VMK_OK);
   } else if (strcmp(buf, "low") == 0) {
      *shares = CPUSCHED_SHARES_LOW(nvcpus);
      return(VMK_OK);
   }

   // parse numeric value
   return(Parse_Int(buf, strlen(buf), shares));
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedProcWorldSharesRead --
 *
 *      Callback for read operation on /proc/vmware/vm/<id>/cpu/shares
 *	procfs node.
 *
 * Results:
 *      Returns VMK_OK.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
CpuSchedProcWorldSharesRead(Proc_Entry *entry, char *buf, int *len)
{
   World_Handle *world = (World_Handle *) entry->private;
   CpuSched_Vsmp *vsmp = World_CpuSchedVsmp(world);
   SP_IRQL prevIRQL;
   int32 shares;

   // initialize
   *len = 0;

   // snapshot shares
   prevIRQL = CpuSchedVsmpCellLock(vsmp);
   shares = vsmp->alloc.shares;
   CpuSchedVsmpCellUnlock(vsmp, prevIRQL);

   // format shares
   Proc_Printf(buf, len, "%d\n", shares);
   return(VMK_OK);
}


/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedVsmpSetShares --
 *
 *     Changes the cpu share allocation of "vsmp" to "shares"
 *     Caller must hold all scheduler cell locks.
 *
 * Results:
 *     Returns VMK_OK on success, or VMK_BAD_PARAM if shares was invalid.
 *
 * Side effects:
 *     shares updated, requests realloc.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
CpuSchedVsmpSetShares(CpuSched_Vsmp *vsmp, uint32 shares)
{
   CpuSched_Alloc alloc;

   ASSERT(CpuSchedAllCellsAreLocked());

   CpuSchedAllocInit(&alloc, vsmp->alloc.min, 
                     vsmp->alloc.max, vsmp->alloc.units, shares);
   return(CpuSchedVsmpSetAlloc(vsmp, &alloc));
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedProcWorldSharesWrite --
 *
 *      Callback for write operation on /proc/vmware/vm/<id>/cpu/shares
 *	procfs node.
 *
 * Results:
 *      Returns VMK_OK iff successful, otherwise error code.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
CpuSchedProcWorldSharesWrite(Proc_Entry *entry, char *buf, int *len)
{
   World_Handle *world = (World_Handle *) entry->private;
   CpuSched_Vcpu *vcpu = World_CpuSchedVcpu(world);
   CpuSched_Vsmp *vsmp = vcpu->vsmp;
   VMK_ReturnStatus status;
   uint32 nvcpus, shares;
   SP_IRQL prevIRQL;
   char *argv[2];
   int argc;

   // fail if dedicated idle world
   if (vcpu->idle) {
      return(VMK_BAD_PARAM);
   }

   // parse buf into args (assumes OK to overwrite)
   argc = Parse_Args(buf, argv, 2);
   if (argc != 1) {
      VcpuWarn(vcpu, "invalid shares: unable to parse");
      return(VMK_BAD_PARAM);
   }

   prevIRQL = CpuSchedLockAllCells();

   // snapshot number of vcpus for vsmp
   nvcpus = vsmp->vcpus.len;

   // parse shares
   if (CpuSchedParseShares(argv[0], nvcpus, &shares) != VMK_OK) {
      CpuSchedUnlockAllCells(prevIRQL);
      VcpuWarn(vcpu, "invalid shares: unable to parse");
      return(VMK_BAD_PARAM);
   }

   // update alloc
   status = CpuSchedVsmpSetShares(vsmp, shares);

   CpuSchedUnlockAllCells(prevIRQL);
   return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedProcWorldMinRead --
 *
 *      Callback for read operation on /proc/vmware/vm/<id>/cpu/min
 *	procfs node.
 *
 * Results:
 *      Returns VMK_OK.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
CpuSchedProcWorldMinRead(Proc_Entry *entry, char *buf, int *len)
{
   World_Handle *world = (World_Handle *) entry->private;
   CpuSched_Vsmp *vsmp = World_CpuSchedVsmp(world);
   SP_IRQL prevIRQL;
   uint32 min;

   // initialize
   *len = 0;

   // snapshot shares
   prevIRQL = CpuSchedVsmpCellLock(vsmp);
   min = vsmp->alloc.min;
   CpuSchedVsmpCellUnlock(vsmp, prevIRQL);

   // format shares
   Proc_Printf(buf, len, "%u\n", min);
   return(VMK_OK);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedVsmpSetMin --
 *
 *     Changes the cpu min allocation of "vsmp" to "min"
 *     Caller must hold all scheduler cell locks.
 *
 * Results:
 *     Returns VMK_OK on success, or VMK_BAD_PARAM if min was invalid
 *
 * Side effects:
 *     Min updated, requests realloc.
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
CpuSchedVsmpSetMin(CpuSched_Vsmp *vsmp, uint32 min)
{
   CpuSched_Alloc alloc;

   ASSERT(CpuSchedAllCellsAreLocked());
   CpuSchedAllocInit(&alloc, min, vsmp->alloc.max, 
		     vsmp->alloc.units, vsmp->alloc.shares);
   return(CpuSchedVsmpSetAlloc(vsmp, &alloc));
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedProcWorldMinWrite --
 *
 *      Callback for write operation on /proc/vmware/vm/<id>/cpu/min
 *	procfs node.
 *
 * Results:
 *      Returns VMK_OK iff successful, otherwise error code.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
CpuSchedProcWorldMinWrite(Proc_Entry *entry, char *buf, int *len)
{
   World_Handle *world = (World_Handle *) entry->private;
   CpuSched_Vcpu *vcpu = World_CpuSchedVcpu(world);
   CpuSched_Vsmp *vsmp = vcpu->vsmp;
   SP_IRQL prevIRQL;
   uint32 min;
   VMK_ReturnStatus res;
   

   // fail if dedicated idle world
   if (vcpu->idle) {
      return(VMK_BAD_PARAM);
   }

   // parse min
   if (Parse_Int(buf, *len, &min) != VMK_OK) {
      return(VMK_BAD_PARAM);
   }

   prevIRQL = CpuSchedLockAllCells();
   res = CpuSchedVsmpSetMin(vsmp, min);
   CpuSchedUnlockAllCells(prevIRQL);

   return (res);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedProcWorldMaxRead --
 *
 *      Callback for read operation on /proc/vmware/vm/<id>/cpu/max
 *	procfs node.
 *
 * Results:
 *      Returns VMK_OK.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
CpuSchedProcWorldMaxRead(Proc_Entry *entry, char *buf, int *len)
{
   World_Handle *world = (World_Handle *) entry->private;
   CpuSched_Vsmp *vsmp = World_CpuSchedVsmp(world);
   SP_IRQL prevIRQL;
   uint32 max;

   // initialize
   *len = 0;

   // snapshot effective max
   prevIRQL = CpuSchedVsmpCellLock(vsmp);
   if (CpuSchedEnforceMax(&vsmp->alloc)) {
      max = vsmp->alloc.max;
   } else {
      max = CpuSchedBaseSharesToUnits(CPUSCHED_BASE_PER_PACKAGE * vsmp->vcpus.len,
                                      vsmp->alloc.units);
   }
   CpuSchedVsmpCellUnlock(vsmp, prevIRQL);

   // format effective max
   Proc_Printf(buf, len, "%u\n", max);
   return(VMK_OK);
}


/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedVsmpSetMax --
 *
 *     Changes the cpu max allocation of "vsmp" to "max"
 *     Caller must hold all scheduler cell locks.
 *
 * Results:
 *     Returns VMK_OK on success, or VMK_BAD_PARAM if min was invalid
 *
 * Side effects:
 *     Max updated, requests realloc.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
CpuSchedVsmpSetMax(CpuSched_Vsmp *vsmp, uint32 max)
{
   CpuSched_Alloc alloc;

   ASSERT(CpuSchedAllCellsAreLocked());

   CpuSchedAllocInit(&alloc, vsmp->alloc.min, max, 
		     vsmp->alloc.units, vsmp->alloc.shares);
   return(CpuSchedVsmpSetAlloc(vsmp, &alloc));
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedProcWorldMaxWrite --
 *
 *      Callback for write operation on /proc/vmware/vm/<id>/cpu/max
 *	procfs node.
 *
 * Results:
 *      Returns VMK_OK iff successful, otherwise error code.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
CpuSchedProcWorldMaxWrite(Proc_Entry *entry, char *buf, int *len)
{
   World_Handle *world = (World_Handle *) entry->private;
   CpuSched_Vcpu *vcpu = World_CpuSchedVcpu(world);
   CpuSched_Vsmp *vsmp = vcpu->vsmp;
   SP_IRQL prevIRQL;
   uint32 max;
   VMK_ReturnStatus status;

   // fail if dedicated idle world
   if (vcpu->idle) {
      return(VMK_BAD_PARAM);
   }

   // parse max
   if (Parse_Int(buf, *len, &max) != VMK_OK) {
      return(VMK_BAD_PARAM);
   }

   prevIRQL = CpuSchedLockAllCells();
   status = CpuSchedVsmpSetMax(vsmp, max);
   CpuSchedUnlockAllCells(prevIRQL);
   
   return(status);
}


/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedProcWorldUnitsWrite --
 *
 *      Callback for proc write operations on /proc/vmware/vm/<vmid>/cpu/units.
 *
 *      By writing a unit type (e.g. "mhz") to this proc node, a user can
 *      change the way that mins and maxes are specified for this vsmp.
 *      Any existing min or max will be auto-converted into the new units.
 *
 * Results:
 *      Returns VMK_BAD_PARAM or VMK_OK
 *
 * Side effects:
 *      May change allocation on vsmp.
 *
 *-----------------------------------------------------------------------------
 */
static int
CpuSchedProcWorldUnitsWrite(Proc_Entry *entry, char *buf, int *len)
{
   World_Handle *world = (World_Handle *) entry->private;
   CpuSched_Vcpu *vcpu = World_CpuSchedVcpu(world);
   CpuSched_Vsmp *vsmp = vcpu->vsmp;
   SP_IRQL prevIRQL;
   int argc;
   char *argv[1];
   Sched_Units newUnits, oldUnits;
   VMK_ReturnStatus status = VMK_OK;
   
   argc = Parse_Args(buf, argv, 1);
   if (argc != 1) {
      Warning("no argument supplied");
      return (VMK_BAD_PARAM);
   }

   newUnits = Sched_StringToUnits(argv[0]);
   if (newUnits == SCHED_UNITS_INVALID) {
      Warning("unknown units type (%s), supported are: 'mhz', 'pct'",
              argv[0]);
      return (VMK_BAD_PARAM);
   }
   
   prevIRQL = CpuSchedLockAllCells();

   oldUnits = vsmp->alloc.units;

   if (oldUnits == newUnits) {
      VSMPLOG(0, vsmp, "no change in units");
   } else {
      uint32 newMin, newMax;
      CpuSched_Alloc newAlloc;
      
      VSMPLOG(0, vsmp, "changing units to %s from %s",
              Sched_UnitsToString(newUnits),
              Sched_UnitsToString(vsmp->alloc.units));
      
      newMin = vsmp->alloc.min * cpuSchedConst.unitsPerPkg[newUnits]
         / cpuSchedConst.unitsPerPkg[vsmp->alloc.units];
      if (CpuSchedEnforceMax(&vsmp->alloc)) {
         newMax = vsmp->alloc.max * cpuSchedConst.unitsPerPkg[newUnits]
            / cpuSchedConst.unitsPerPkg[vsmp->alloc.units];
      } else {
         newMax = cpuSchedConst.unitsPerPkg[newUnits] * vsmp->vcpus.len;
      }

      CpuSchedAllocInit(&newAlloc, newMin, newMax, newUnits, 
		        vsmp->alloc.shares);
      status = CpuSchedVsmpSetAlloc(vsmp, &newAlloc);
      if (status != VMK_OK) {
         Warning("could not change units to %s, possibly due to min constraints",
                 Sched_UnitsToString(newUnits));
      }
   }
   
   CpuSchedUnlockAllCells(prevIRQL);

   return (status);
}


/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedProcWorldUnitsRead --
 *
 *      Callback for proc read operations on /proc/vmware/vm/<vmid>/cpu/units
 *
 * Results:
 *      Returns VMK_OK.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
static int
CpuSchedProcWorldUnitsRead(Proc_Entry *entry, char *buf, int *len)
{
   World_Handle *world = (World_Handle *) entry->private;
   CpuSched_Vsmp *vsmp = World_CpuSchedVsmp(world);
   SP_IRQL prevIRQL;

   // initialize
   *len = 0;

   prevIRQL = CpuSchedVsmpCellLock(vsmp);
   Proc_Printf(buf, len, "%s\n", Sched_UnitsToString(vsmp->alloc.units));
   CpuSchedVsmpCellUnlock(vsmp, prevIRQL);

   return (VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedProcWorldAffinityRead --
 *
 *      Callback for read operation on /proc/vmware/vm/<id>/cpu/affinity
 *	procfs node.
 *
 * Results:
 *      Returns VMK_OK.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
CpuSchedProcWorldAffinityRead(Proc_Entry *entry, char *buf, int *len)
{
   World_Handle *world = (World_Handle *) entry->private;
   CpuSched_Vcpu *vcpu = World_CpuSchedVcpu(world);
   CpuSched_Vsmp *vsmp = vcpu->vsmp;
   char affinityBuf[CPUSCHED_CPUMASK_BUF_LEN];
   CpuMask affinity;
   SP_IRQL prevIRQL;

   // initialize
   *len = 0;

   // snapshot affinity
   prevIRQL = CpuSchedVsmpCellLock(vsmp);
   affinity = CpuSchedVcpuHardAffinity(vcpu);
   CpuSchedVsmpCellUnlock(vsmp, prevIRQL);

   // format affinity
   (void) CpuMaskFormat(affinity, affinityBuf, CPUSCHED_CPUMASK_BUF_LEN, ',');
   Proc_Printf(buf, len, "%s\n", affinityBuf);
   return(VMK_OK);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedVerifyAffinity --
 *
 *     Confirms that the affinity settings specified by "vcpuMasks" are valid
 *     for a vsmp with "numVcpus" vcpus. This only guarantees that all vcpus
 *     have valid masks (i.e. nonzero), that the vsmp has either fully
 *     joint or disjoint affinity, and that the vsmp is capable of being
 *     coscheduled within some cell.
 *
 * Results:
 *     Returns VMK_OK if the affinity settings are valid, else VMK_BAD_PARAM
 *     "*jointAffinity" is set to TRUE is all vcpu affinity masks are 
 *     identical, FALSE otherwise.
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
CpuSchedVerifyAffinity(int numVcpus, CpuMask vcpuMasks[], Bool *jointAffinity)
{
   Bool verifiedCell;
   int i, j;

   *jointAffinity = TRUE;

   // uni always has valid affinity if mask is non-null
   if (numVcpus == 1 && vcpuMasks[0] != 0) {
      return (VMK_OK);
   }

   // make sure that we have some valid mask for each vcpu
   for (i=0; i < numVcpus; i++) {
      if (vcpuMasks[i] == 0) {
         LOG(0, "no valid mask for vcpu %d", i);
         return (VMK_BAD_PARAM);
      }
      // iff all masks are identical, we have joint affinity
      if (vcpuMasks[i] != vcpuMasks[0]) {
         *jointAffinity = FALSE;
      }
   }

   if ((*jointAffinity)) {
      // need enough bits in the mask to coschedule all vcpus
      uint8 bitsSet = Util_BitPopCount(vcpuMasks[0] & cpuSchedConst.defaultAffinity);
      if (bitsSet < numVcpus) {
         Warning("affinity set contains only %u pcpus, need at least %u for SMP VM",
                 bitsSet, numVcpus);
         return (VMK_BAD_PARAM);
      }
   } else {
      // ensure that we don't have any overlapping masks if we have disjoint affinity
      for (i=0; i < numVcpus - 1; i++) {
         for (j=i+1; j < numVcpus; j++) {
            if ((vcpuMasks[i] & vcpuMasks[j]) != 0) {
               LOG(0,"vcpus %d and %d have overlapping affinity masks, invalid",
                   i, j);
               return (VMK_BAD_PARAM);
            } 
         }
      }
   }

   // ensure able to coschedule in at least one cell
   // n.b. cell masks are write-once, so OK to read unlocked
   verifiedCell = FALSE;
   FORALL_CELLS_UNLOCKED(c) {
      if (CpuSchedAffinityPermitsCell(c, numVcpus, vcpuMasks)) {
         verifiedCell = TRUE;
      }
   } CELLS_UNLOCKED_DONE;
   if (!verifiedCell) {
      LOG(0, "affinity constraints not compatible with any cell");
      return(VMK_BAD_PARAM);
   }

   return (VMK_OK);
}


/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedParseStandardAffinity --
 *
 *     Parses a UP-style affinity string, like: "1,2,3" and stores
 *     the resulting mask in *mask if successful.
 *
 * Results:
 *     Returns VMK_OK and stores mask in *mask on success.
 *     Returns VMK_BAD_PARAM if buf could not be parsed.
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
CpuSchedParseStandardAffinity(int numVcpus, char* buf, CpuMask *mask)
{
   VMK_ReturnStatus status;
   char *badToken;
   char affinBuf[CPUSCHED_CPUMASK_BUF_LEN];
   
   strncpy(affinBuf, buf, CPUSCHED_CPUMASK_BUF_LEN);

   status = Parse_IntMask(affinBuf, numPCPUs, mask, &badToken);
   if (status != VMK_OK) {
      if (badToken == NULL) {
         return(status);
      } else if ((strcmp(badToken, "default") == 0) ||
                 (strcmp(badToken, "all") == 0)) {
         // special case: single argument "default" or "all"
         *mask = cpuSchedConst.defaultAffinity;
      } else {
         return(status);
      }
   }

   return (VMK_OK);
}


/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedParseMPAffinity --
 *
 *     Parse the string in "buf" as an MP affinity specification. Store the
 *     resulting vcpu affinity masks in the vcpuMasks array. Set
 *     "*jointAffinity" to true if the specification describes joint affinity
 *     (all vcpus have same affinity).
 *  
 *     Specifications have the following format:
 *     "vcpunum:pcpulist;vcpunum:pcpulist;...;" where "pcpulist" is a
 *     comma-separated list of pcpu numbers. All lists are 0-indexed. "all"
 *     is an acceptable alias to set affinity for all vcpus in a vsmp at
 *     once. The trailing semicolon is mandatory.
 *
 * Examples:
 *     - bind all vcpus to a NUMA node: "all:4,5,6,7;"
 *     - bind each vcpu in a 2-way to a different cpu: "0:2;1:3;"
 *       (in the above example, vcpu0 is bound to pcpu2 and vcpu1 is bound to pcpu3)
 *     - allow both vcpus to run anywhere on a 4-way box: "0:0,1,2,3;1:0,1,2,3;"
 *
 * Results:
 *     Returns VMK_OK if parsing succeeded, VMK_BAD_PARAM otherwise.
 *
 * Side effects:
 *     May overwrite parts of the "buf" string.
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
CpuSchedParseMPAffinity(int numVcpus, char *buf, CpuMask vcpuMasks[], Bool *jointAffinity)
{
   VMK_ReturnStatus res;
   char *badToken;
   int i=0, v;
   int colonIndex = 0, semiColonIndex = 0;
   CpuMask mask;

   *jointAffinity = TRUE;

   memset(vcpuMasks, 0, sizeof(CpuMask) * numVcpus);

   // first, try to parse this as UP-style "0,1,2,.." affinity
   if (CpuSchedParseStandardAffinity(numVcpus, buf, &mask) == VMK_OK) {
      uint8 bitsSet = Util_BitPopCount(mask & cpuSchedConst.defaultAffinity);
      // make sure this mask covers enough PCPUs for us to coschedule
      if (bitsSet < numVcpus) {
         Warning("affinity set contains only %u pcpus, need at least %u for SMP VM",
                 bitsSet, numVcpus);
         return (VMK_BAD_PARAM);
      }

      // this mask is fine, use it for all vcpus
      for (i=0; i < numVcpus; i++) {
         vcpuMasks[i] = mask;
      }

      // verify affinity
      return(CpuSchedVerifyAffinity(numVcpus, vcpuMasks, jointAffinity));
   }

   while (1) {
      Bool allVcpus = FALSE;
      int thisVcpu;
      

      // start by finding the first colon
      colonIndex = i;
      while (buf[colonIndex] != '\0' && buf[colonIndex] != ':') { colonIndex++; }
      if (buf[colonIndex] != ':') { break; }
      
      // the colon should be preceeded by a vcpu number or "all"
      if (strncmp(buf + i, "all", 3) == 0) {
         allVcpus = TRUE;
         thisVcpu = -1;
      } else {
         res = Parse_Int(buf + i, colonIndex - i, &thisVcpu);
         if (res != VMK_OK) {
            LOG(0, "bad vcpu num: %s", buf + i);
            return (VMK_BAD_PARAM);
         }
         if (thisVcpu >= numVcpus) {
            LOG(0, "vcpu num %d too high (only %d vcpus)", thisVcpu, numVcpus);
            return (VMK_BAD_PARAM);
         }
      }
      
      // find the terminating semicolon
      semiColonIndex = colonIndex + 1;
      while (buf[semiColonIndex] != '\0' && buf[semiColonIndex] != ';') {
         semiColonIndex++;
      }

      // bail if no semicolon found
      if (buf[semiColonIndex] != ';') {
         LOG(0, "missing semicolon in affinity specification");
         return (VMK_BAD_PARAM);
      }
      buf[semiColonIndex] = '\0';

      // look for special token
      if (!strcmp(buf + colonIndex + 1, "all")
          || !strcmp(buf + colonIndex + 1, "default")) {
         mask = cpuSchedConst.defaultAffinity;
      } else {
         // actually parse the mask
         res = Parse_IntMask(buf + colonIndex + 1, numPCPUs, &mask, &badToken);
         if (res != VMK_OK) {
            LOG(0, "parse mask failed: %s", buf + colonIndex + 1);
            return (res);
         }
      }

      // now that we have a valid mask, save it in the in/out buffer
      if (allVcpus) {
         for (v=0; v < numVcpus; v++) {
            vcpuMasks[v] = mask;
         }
         // we've set all vcpus, so we're done
         *jointAffinity = TRUE;
	break;
      } else {
         vcpuMasks[thisVcpu] = mask;
      }

      i = semiColonIndex + 1;
   }

   // verify affinity
   return(CpuSchedVerifyAffinity(numVcpus, vcpuMasks, jointAffinity));
}


/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedVsmpSetAffinity --
 *
 *     Sets the vsmps's affinity to the affinity spec provided by "buf."
 *     See CpuSchedParseMPAffinity for details of affinity specs.
 *  
 *     Note that we only support fully "joint" or "disjoint" affinity settings
 *     for MP vsmps. That is, either ALL vcpus have the same affinity mask, OR
 *     no vcpus have overlapping affinity masks.
 *
 * Results:
 *     Returns VMK_OK if we successfully set affinity to the new mask, 
 *     or VMK_BAD_PARAM if we failed due to a bad parameter.
 *
 * Side effects:
 *     Changes vsmp affinity settings if all goes well.
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
CpuSchedVsmpSetAffinity(CpuSched_Vsmp *vsmp, char *buf)
{
   CpuMask vcpuMasks[CPUSCHED_VSMP_VCPUS_MAX];
   Bool jointAffinity;
   VMK_ReturnStatus res;
   SP_IRQL prevIRQL;

   prevIRQL = CpuSchedLockAllCells();

   res = CpuSchedParseMPAffinity(vsmp->vcpus.len, buf, vcpuMasks, &jointAffinity);
   if (res != VMK_OK) {
      VsmpWarn(vsmp, "invalid affinity specification ignored: %s", buf);
      CpuSchedUnlockAllCells(prevIRQL);
      return (res);
   }
   CpuSchedVsmpSetAffinityInt(vsmp, vcpuMasks, TRUE);
   
   CpuSchedUnlockAllCells(prevIRQL);
   return (VMK_OK);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedVsmpSetAffinityInt --
 *
 *     Sets affinity of vcpus in vsmp to "vcpuMasks"
 *     "hardAffinity" indicates that this affinity has been manually set by 
 *     a user, as opposed to soft affinity, which is used only internally.
 *     Caller must hold all scheduler cell locks.
 * 
 *     See CpuSchedVsmpSetAffinity above for details. 
 *
 * Results:
 *     None.
 *
 * Side effects:
 *     Modifies per-vcpu and per-vsmp affinity data.
 *
 *-----------------------------------------------------------------------------
 */
static void
CpuSchedVsmpSetAffinityInt(CpuSched_Vsmp *vsmp, CpuMask vcpuMasks[], Bool hardAffinity)
{
   int i;
   Bool jointAffinity = TRUE;
   Bool affinityConstrained = FALSE;

   ASSERT(CpuSchedAllCellsAreLocked());
   // can't set soft affinity on a vsmp with hard affinity already
   ASSERT(hardAffinity || !vsmp->hardAffinity);
   
   // set affinity mask of all vcpus
   for (i=0; i < vsmp->vcpus.len; i++) {
      CpuSchedVcpuSetAffinityMask(vsmp->vcpus.list[i], vcpuMasks[i], hardAffinity);
      if (vcpuMasks[i] != vcpuMasks[0]) {
         jointAffinity = FALSE;
      }
      if ((cpuSchedConst.defaultAffinity & vcpuMasks[i]) != cpuSchedConst.defaultAffinity) {
         affinityConstrained = TRUE;
      }
   }

   vsmp->affinityConstrained = affinityConstrained;
   vsmp->jointAffinity = jointAffinity;
   vsmp->maxHTConstraint = CpuSchedVsmpMaxHTConstraint(vsmp);
   
   // warn if a user-specified affinity setting screws up HT sharing config
   if (vsmp->maxHTConstraint < vsmp->htSharing
       && hardAffinity) {
      Warning("based on new affinity, configured HT sharing type of %s"
              " is not allowed, %s will be used instead",
              CpuSchedHTSharingName(vsmp->htSharing),
              CpuSchedHTSharingName(vsmp->maxHTConstraint));
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * CpuSched_WorldSetAffinity --
 *
 *      Sets "world"'s affinity to the provided affinMask. This joint affinity
 *      will be applied to all vcpus of a vsmp.
 *
 * Results:
 *      Returns VMK_OK on success, VMK_BAD_PARAM or VMK_NOT_FOUND otherwise
 *
 * Side effects:
 *      Modifies affinity of the "world."
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
CpuSched_WorldSetAffinity(World_ID world, CpuMask affinMask)
{
   CpuMask vcpuMasks[MAX_VCPUS], effectiveAffin;
   unsigned i;
   SP_IRQL prevIRQL;
   World_Handle *thisWorld;
   CpuSched_Vsmp *vsmp;

   // ignore pcpus in the mask that don't actually exist
   effectiveAffin = affinMask & cpuSchedConst.defaultAffinity;

   thisWorld = World_Find(world);
   if (thisWorld == NULL) {
      return (VMK_NOT_FOUND);
   }
   vsmp = World_CpuSchedVsmp(thisWorld);
   
   // always provide joint affinity for smps
   for (i=0; i < MAX_VCPUS; i++) {
      vcpuMasks[i] = affinMask;
   }
   
   // we need at least one pcpu in the mask for each vcpu
   if (Util_BitPopCount(effectiveAffin) < vsmp->vcpus.len) {
      return (VMK_BAD_PARAM);
   }
   
   prevIRQL = CpuSchedLockAllCells();
   
   CpuSchedVsmpSetAffinityInt(vsmp, vcpuMasks, TRUE);

   CpuSchedUnlockAllCells(prevIRQL);
   World_Release(thisWorld);
   
   return (VMK_OK);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedVsmpSetSoftAffinity --
 *
 *     Sets "soft" cpu affinity for the vsmp to the provided mask.
 *     The vsmp should NOT have hard affinity already. 
 *
 * Results:
 *     None
 *
 * Side effects:
 *     Changes cpu affinity masks for vcpus in vsmp
 *
 *-----------------------------------------------------------------------------
 */
static void
CpuSchedVsmpSetSoftAffinity(CpuSched_Vsmp *vsmp, CpuMask affinity)
{
   int i;
   CpuMask vcpuMasks[CPUSCHED_VSMP_VCPUS_MAX];

   ASSERT(!vsmp->hardAffinity);
   
   for (i=0; i < vsmp->vcpus.len; i++) {
      vcpuMasks[i] = affinity;
   }

   CpuSchedVsmpSetAffinityInt(vsmp, vcpuMasks, FALSE);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedProcWorldAffinityWrite --
 *
 *      Callback for write operation on /proc/vmware/vm/<id>/cpu/affinity
 *	procfs node.
 *
 * Results:
 *      Returns VMK_OK iff successful, otherwise error code.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
CpuSchedProcWorldAffinityWrite(Proc_Entry *entry, char *buf, int *len)
{
   World_Handle *world = (World_Handle *) entry->private;
   CpuSched_Vcpu *vcpu = World_CpuSchedVcpu(world);
   VMK_ReturnStatus status;
   CpuMask affinity;
   World_ID worldID;
   SP_IRQL prevIRQL;
   char *localBuf;
   Bool mpStyleParam = FALSE;

   // convenient abbrev
   worldID = world->worldID;

   if (CpuSchedIsMP(vcpu->vsmp)) {
      int res = CpuSchedVsmpSetAffinity(vcpu->vsmp, buf);
      return res;
   }

   // handle uniprocessor affinity

   // permit the use of the smp-compatible "0:1,2,3" format for unis too
   if (!strncmp(buf, "0:", 2)) {
      localBuf = buf + 2;
      mpStyleParam = TRUE;
   } else if (!strncmp(buf, "all:", 4)) {
      localBuf = buf + 4;
      mpStyleParam = TRUE;
   } else {
      localBuf = buf;
   }
   
   if (mpStyleParam) {
      int i = 0;

      // find the semicolon and null it out;
      for (i=0; localBuf[i] != '\0'; i++) {
         if (localBuf[i] == ';') {
            localBuf[i] = '\0';
         }
      }
   }

   // parse buffer as bitmask of PCPUs
   status = CpuSchedParseStandardAffinity(1, localBuf, &affinity);

   // sanity check: ensure non-zero mask
   if (status != VMK_OK || affinity == 0) {
      VmWarn(worldID, "invalid affinity setting=%s", buf);
      return(VMK_BAD_PARAM);
   }

   // update affinity
   prevIRQL = CpuSchedLockAllCells();
   status = CpuSchedVcpuSetAffinityUni(vcpu, affinity);
   CpuSchedUnlockAllCells(prevIRQL);

   return(status);
}


/*
 *-----------------------------------------------------------------------------
 *
 * CpuSched_NumAffinityPackages --
 *
 *      Returns the number of physical packages covered by the "mask"
 *
 * Results:
 *      Returns the number of physical packages covered by the "mask"
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
uint8
CpuSched_NumAffinityPackages(CpuMask mask)
{
   uint8 numPackages = 0;
   
   FORALL_PCPUS(p) {
      CpuMask pkgMask;

      if (!(mask & CPUSCHED_AFFINITY(p))
         || SMP_GetHTThreadNum(p) != 0) {
         continue;
      }
      pkgMask = CpuSched_PcpuMask(p, TRUE);
      if (mask & pkgMask) {
         numPackages++;
      }
   } PCPUS_DONE;

   return (numPackages);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedAddWorldProcEntries --
 *
 *      Add world specific proc entries exported by the cpu scheduler.
 *
 * Results:
 *      None. 
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedAddWorldProcEntries(World_Handle *world)
{
   CpuSched_Client *c = &world->sched.cpu;
   Bool idle = c->vcpu.idle;

   // "cpu" directory
   Proc_InitEntry(&c->procDir);
   c->procDir.parent = &world->procWorldDir;
   Proc_Register(&c->procDir, "cpu", TRUE);   

   // "cpu/status" entry
   Proc_InitEntry(&c->procStatus);
   c->procStatus.parent = &c->procDir;
   c->procStatus.read = CpuSchedProcWorldStatusRead;
   c->procStatus.private = world;   
   Proc_Register(&c->procStatus, "status", FALSE);

   // "cpu/state-times" entry
   Proc_InitEntry(&c->procStateTimes);
   c->procStateTimes.parent = &c->procDir;
   c->procStateTimes.read = CpuSchedProcWorldStateTimesRead;
   c->procStateTimes.private = world;
   Proc_Register(&c->procStateTimes, "state-times", FALSE);

   // "cpu/state-counts" entry
   Proc_InitEntry(&c->procStateCounts);
   c->procStateCounts.parent = &c->procDir;
   c->procStateCounts.read = CpuSchedProcWorldStateCountsRead;
   c->procStateCounts.private = world;
   Proc_Register(&c->procStateCounts, "state-counts", FALSE);

   // setup run and wait histogram proc nodes
   if (CPUSCHED_STATE_HISTOGRAMS) {
      Proc_InitEntry(&c->procRunStatesHisto);
      c->procRunStatesHisto.parent = &c->procDir;
      c->procRunStatesHisto.read = CpuSchedProcRunStatesHistoRead;
      c->procRunStatesHisto.write = CpuSchedProcRunStatesHistoWrite;
      c->procRunStatesHisto.private = World_CpuSchedVcpu(world);
      Proc_Register(&c->procRunStatesHisto, "run-state-histo", FALSE);

      Proc_InitEntry(&c->procWaitStatesHisto);
      c->procWaitStatesHisto.parent = &c->procDir;
      c->procWaitStatesHisto.read = CpuSchedProcWaitStatesHistoRead;
      c->procWaitStatesHisto.write = CpuSchedProcWaitStatesHistoWrite;
      c->procWaitStatesHisto.private = World_CpuSchedVcpu(world);
      Proc_Register(&c->procWaitStatesHisto, "wait-state-histo", FALSE);
   }

   // "cpu/run-times" entry
   Proc_InitEntry(&c->procPcpuRunTimes);
   c->procPcpuRunTimes.parent = &c->procDir;
   c->procPcpuRunTimes.read = CpuSchedProcWorldRunTimesRead;
   c->procPcpuRunTimes.private = world;
   Proc_Register(&c->procPcpuRunTimes, "run-times", FALSE);

   // "cpu/wait-stats" entry
   Proc_InitEntry(&c->procWaitStats);
   c->procWaitStats.parent = &c->procDir;
   c->procWaitStats.read = CpuSchedProcWorldWaitStatsRead;
   c->procWaitStats.private = world;
   Proc_Register(&c->procWaitStats, "wait-stats", FALSE);

   // "cpu/group" entry (non-idle worlds only)
   if (!idle) {
      Proc_InitEntry(&c->procGroup);
      c->procGroup.parent = &c->procDir;
      c->procGroup.read = CpuSchedProcWorldGroupRead;
      c->procGroup.write = CpuSchedProcWorldGroupWrite;
      c->procGroup.private = world;
      Proc_Register(&c->procGroup, "group", FALSE);
   }

   // "cpu/shares" entry (non-idle worlds only)
   Proc_InitEntry(&c->procShares);
   if (!idle) {
      c->procShares.parent = &c->procDir;
      c->procShares.read  = CpuSchedProcWorldSharesRead;
      c->procShares.write = CpuSchedProcWorldSharesWrite;
      c->procShares.private = world;   
      Proc_Register(&c->procShares, "shares", FALSE);
   }

   // "cpu/min" entry (non-idle worlds only)
   Proc_InitEntry(&c->procMin);
   if (!idle) {
      c->procMin.parent = &c->procDir;      
      c->procMin.read = CpuSchedProcWorldMinRead;
      c->procMin.write = CpuSchedProcWorldMinWrite;
      c->procMin.private = world;   
      Proc_Register(&c->procMin, "min", FALSE);
   }

   // "cpu/max" entry (non-idle worlds only)
   Proc_InitEntry(&c->procMax);
   if (!idle) {
      c->procMax.parent = &c->procDir;      
      c->procMax.read = CpuSchedProcWorldMaxRead;
      c->procMax.write = CpuSchedProcWorldMaxWrite;
      c->procMax.private = world;   
      Proc_Register(&c->procMax, "max", FALSE);
   }

   // "cpu/units" entry (non-idle worlds only)
   Proc_InitEntry(&c->procUnits);
   if (!idle) {
      c->procUnits.parent = &c->procDir;
      c->procUnits.read = CpuSchedProcWorldUnitsRead;
      c->procUnits.write = CpuSchedProcWorldUnitsWrite;
      c->procUnits.private = world;
      Proc_Register(&c->procUnits, "units", FALSE);
   }

   // "cpu/affinity" entry (non-idle worlds only)
   Proc_InitEntry(&c->procAffinity);
   if (!idle) {
      c->procAffinity.parent = &c->procDir;
      c->procAffinity.read  = CpuSchedProcWorldAffinityRead;
      c->procAffinity.write = CpuSchedProcWorldAffinityWrite;
      c->procAffinity.private = world;   
      Proc_Register(&c->procAffinity, "affinity", FALSE);
   }

   // "cpu/debug" entry
   Proc_InitEntry(&c->procDebug);
   c->procDebug.parent = &c->procDir;
   c->procDebug.read  = CpuSchedProcWorldDebugRead;
   c->procDebug.write = CpuSchedProcWorldDebugWrite;
   c->procDebug.private = world;   
   Proc_RegisterHidden(&c->procDebug, "debug", FALSE);

   // "cpu/hyperthreading" entry
   if (SMP_HTEnabled()) {
      Proc_InitEntry(&c->procHyperthreading);
      c->procHyperthreading.parent = &c->procDir;
      c->procHyperthreading.read  = CpuSchedProcWorldHyperthreadingRead;
      c->procHyperthreading.write  = CpuSchedProcWorldHyperthreadingWrite;
      c->procHyperthreading.private = (void*)world->worldID;
      Proc_Register(&c->procHyperthreading, "hyperthreading", FALSE);
   }

   NUMASched_AddWorldProcEntries(world, &c->procDir);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedRemoveWorldProcEntries --
 *
 *      Remove world specific proc entries exported by the cpu scheduler.
 *
 * Results:
 *      None. 
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedRemoveWorldProcEntries(World_Handle *world)
{
   CpuSched_Client *c = &world->sched.cpu;

   // remove all entries (ignored if don't exist)
   Proc_Remove(&c->procDebug);
   if (!c->vcpu.idle) {
      Proc_Remove(&c->procAffinity);
      Proc_Remove(&c->procUnits);
      Proc_Remove(&c->procMax);
      Proc_Remove(&c->procMin);
      Proc_Remove(&c->procShares);
      Proc_Remove(&c->procGroup);
   }
   Proc_Remove(&c->procWaitStats);
   Proc_Remove(&c->procPcpuRunTimes);
   Proc_Remove(&c->procStateCounts);
   Proc_Remove(&c->procStateTimes);
   Proc_Remove(&c->procStatus);

   if (CPUSCHED_STATE_HISTOGRAMS) {
      Proc_Remove(&c->procWaitStatesHisto);
      Proc_Remove(&c->procRunStatesHisto);
   }
   if (SMP_HTEnabled()) {
      Proc_Remove(&c->procHyperthreading);
   }

   NUMASched_RemoveWorldProcEntries(world);

   Proc_Remove(&c->procDir);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_HostInterrupt --
 *
 *      Inform scheduler that the host world has a pending interrupt.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May reduce scheduling latency for host world.
 *
 *----------------------------------------------------------------------
 */
void
CpuSched_HostInterrupt(void)
{
   // warp COS for faster dispatch
   //   try to avoid unnecessary work if COS already running or warped
   //   n.b. may occasionally skip useful warp (race during checks)
   if (!CpuSched_HostIsRunning() && (cpuSched.vtConsoleWarpCurrent == 0)) {
      CpuSched_Cell *c = CONSOLE_CELL;
      SP_IRQL prevIRQL = CpuSchedCellLock(c);
      CpuSchedWarpConsole();
      CpuSchedCellUnlock(c, prevIRQL);
   }

   // debugging
   CpuSchedLogEvent("host-intr", MY_PCPU);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedProcWorldDebugWrite --
 *
 *      Callback for write operation on /proc/vmware/vm/<id>/cpu/debug
 *	procfs node.
 *
 * Results:
 *      Returns VMK_OK iff successful, otherwise error code.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
CpuSchedProcWorldDebugWrite(Proc_Entry *entry, char *buf, int *len)
{
   World_Handle *world = (World_Handle *) entry->private;
   CpuSched_Vcpu *vcpu = World_CpuSchedVcpu(world);
   CpuSched_Vsmp *vsmp = vcpu->vsmp;
   uint32 val;
   char *argv[8];
   char *cmd;
   int argc;

   // parse buf into args (assumes OK to overwrite buffer)
   argc = Parse_Args(buf, argv, 8);
   if (argc < 1) {
      return(VMK_BAD_PARAM);
   }

   // convenient abbrev
   cmd = argv[0];

   // "sync" command
   if (strcmp(cmd, "sync") == 0) {
      // sync client vtime to global vtime
      SP_IRQL prevIRQL = CpuSchedVsmpCellLock(vsmp);
      CpuSchedVtime vtime = vsmp->cell->vtime;
      VcpuLog(vcpu, "sync vtime, delta=%Ld", vtime - vsmp->vtime.main);
      vsmp->vtime.main = vtime;
      CpuSchedVsmpCellUnlock(vsmp, prevIRQL);
      return(VMK_OK);
   } else if (argc == 2
              && strcmp(cmd, "strictcosched") == 0
              && Parse_Int(argv[1], strlen(argv[1]), &val) == VMK_OK) {
      SP_IRQL prevIRQL = CpuSchedVsmpCellLock(vsmp);
      vsmp->strictCosched = val ? TRUE : FALSE;
      CpuSchedVsmpCellUnlock(vsmp, prevIRQL);
      return(VMK_OK);
   }

   LOG(0, "valid world debug commands are 'sync' and 'strictcosched [1 or 0]'");
   Warning("invalid command: \"%s\"", buf);
   return(VMK_BAD_PARAM);
}


/*
 *----------------------------------------------------------------------
 *
 * CpuSchedProcWorldDebugRead --
 *
 *      Callback for read operation on procfs node
 *	/proc/vmware/vm/<id>/cpu/debug.
 *
 * Results:
 *      Returns VMK_OK.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
CpuSchedProcWorldDebugRead(Proc_Entry *entry, char *buf, int *len)
{
   World_Handle *world = (World_Handle *) entry->private;
   CpuSched_Vcpu *vcpu = World_CpuSchedVcpu(world);
   CpuSched_Vsmp *vsmp = vcpu->vsmp;
   uint64 avgLatencyUsec;
   char affinityBuf[CPUSCHED_CPUMASK_BUF_LEN];   
   SP_IRQL prevIRQL;
   Timer_Cycles uptime;
   Vcpuid vcpuid;

   // initialize
   *len = 0;

   prevIRQL = CpuSchedVsmpCellLock(vsmp);

   // compute uptime
   uptime = vsmp->cell->now - vcpu->stats.uptimeStart;

   // compute average wakeup latency
   if (vcpu->wakeupLatencyMeter.count > 0) {
      avgLatencyUsec = Timer_TCToUS(vcpu->wakeupLatencyMeter.elapsed
                                    / vcpu->wakeupLatencyMeter.count);
   } else {
      avgLatencyUsec = 0;
   }

   // lookup vmm vcpuid, if any
   if (World_IsVMMWorld(world)) {
      vcpuid = World_VMM(world)->vcpuid;
   } else {
      vcpuid = 0;
   }
   
   // format state and time data
   Proc_Printf(buf, len,
               "vmmvcpu         %u\n"
               "actwakechk      %u\n"
               "actntfy         %u\n"
               "\n"
               "costate         %s\n"
               "state           %s\n"
               "wait            %s\n"
               "event           %u\n"
               "discod          %d\n"
               "\n"
               "wswitch         %8u\n"
               "migrate         %8u\n"
               "pkgMigrate      %8u\n"
               "wakeupMigIdle   %8u\n"
               "halts           %8u\n"
               "qexpire         %8u\n"
               "\n"
               "skewSmp         %8u\n"
               "skewOK          %8u\n"
               "skewBad         %8u\n"
               "skewRes         %8u\n"
               "skewIgn         %8u\n"
               "\n"
               "nRun            %2d\n"
               "nWait           %2d\n"
               "nIdle           %2d\n"
               "\n"
               "wakeups         %12u\n"
               "avgLtcy         %12Lu usec\n"
               "\n"
               "nblbhd          %8u\n"
               "nblahd          %8u\n"
               "\n"
               "vtLimit         %16Ld\n"
               "noPreempt       %d\n"
               "affn            %x\n"
               "\n"
               "cellMigs        %8u\n"
               "\n"
               "agedCountSlow       %16Lu\n"
               "agedPerMilSlow      %16Lu\n"
               "agedCountFast       %16Lu\n"
               "agedPerMilFast      %16Lu\n"
               "quarantine?         %16u\n"
               "numQuarantines      %16u\n"
               "quarantinePeriods   %16u\n"
               "\n"
               "coschedPolicy       %s\n"
               "intraSkew           %8u\n"
               "needsCosched        %s\n"
               "intraSkewSamp       %8u\n"
               "intraSkewOut        %8u\n",
               vcpuid,
               vcpu->stats.actionWakeupCheck,
               vcpu->stats.actionNotify,
               CpuSchedCoRunStateName(vsmp->coRunState),
               CpuSchedRunStateName(vcpu->runState),
               CpuSchedWaitStateName(vcpu->waitState),
               vcpu->waitEvent,
               vsmp->disableCoDeschedule,
               vcpu->stats.worldSwitch,
               vcpu->stats.migrate,
               vcpu->stats.pkgMigrate,
               vcpu->stats.wakeupMigrateIdle,
               vcpu->stats.halt,
               vcpu->stats.quantumExpire,
               vsmp->skew.stats.samples,
               vsmp->skew.stats.good,
               vsmp->skew.stats.bad,
               vsmp->skew.stats.resched,
               vsmp->skew.stats.ignore,
               vsmp->nRun,
               vsmp->nWait,
               vsmp->nIdle,
               vcpu->wakeupLatencyMeter.count,
               avgLatencyUsec,
               vsmp->stats.boundLagBehind,
               vsmp->stats.boundLagAhead,
               vsmp->vtimeLimit,
               world->preemptionDisabled,
               CpuSchedVcpuHardAffinity(vcpu),
               vsmp->stats.cellMigrate,
               vcpu->htEvents.agedCountSlow,
               vcpu->htEvents.agedCountSlow
               / (vsmp->cell->config.htEventsUpdateCycles / 1000000ul),
               vcpu->htEvents.agedCountFast,
               vcpu->htEvents.agedCountFast
               / (vsmp->cell->config.htEventsUpdateCycles / 1000000ul),
               vsmp->htQuarantine,
               vsmp->numQuarantines,
               vsmp->quarantinePeriods,
               vsmp->strictCosched ? "strict" : "relaxed",
               vcpu->intraSkew,
               CpuSchedVcpuNeedsCosched(vcpu) ? "yes" : "no",
               vsmp->skew.stats.intraSkewSamples,
               vsmp->skew.stats.intraSkewOut);

   Proc_Printf(buf, len, "\nIntraSkew values histogram:\n\n");
   Histogram_ProcFormat(vcpu->intraSkewHisto, "", buf, len);
                        
   // format affinity data
   (void) CpuMaskFormat(CpuSchedVcpuHardAffinity(vcpu),
                        affinityBuf, CPUSCHED_CPUMASK_BUF_LEN, ',');

   CpuSchedVsmpCellUnlock(vsmp, prevIRQL);

   // everything OK
   return(VMK_OK);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedCellStatsReset --
 *
 *	Resets scheduler stats for "cell".
 *      Caller must hold scheduler lock for "cell".
 *
 * Results:
 *	Modifies stats values in "cell".
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */
static void 
CpuSchedCellStatsReset(CpuSched_Cell *cell)
{
   // sanity check
   ASSERT(CpuSchedCellIsLocked(cell));

   // reset per-cell stats
   memset(&cell->stats, 0, sizeof(CpuSchedCellStats));
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedStateMeterReset --
 *
 *      Resets "m" by zeroing count, elapsed time, and
 *	histogram statistics.
 *
 * Results:
 *      Resets "m" by zeroing count, elapsed time, and
 *	histogram statistics.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedStateMeterReset(CpuSched_StateMeter *m)
{
   m->count = 0;
   m->elapsed = 0;
   if (CPUSCHED_STATE_HISTOGRAMS) {
      Histogram_Reset(m->histo);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedVsmpStatsReset --
 *
 *	Resets scheduler stats for "vsmp".
 *      Caller must hold scheduler cell lock for "vsmp".
 *
 * Results:
 *	Modifies stats values in "vsmp".
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */
static void 
CpuSchedVsmpStatsReset(CpuSched_Vsmp* vsmp)
{
   ASSERT(CpuSchedVsmpCellIsLocked(vsmp));

   // reset per-VSMP stats
   memset(&vsmp->stats, 0, sizeof(CpuSched_VsmpStats));
   vsmp->htQuarantine = FALSE;

   // reset per-VCPU stats
   FORALL_VSMP_VCPUS(vsmp, vcpu) {
      CpuSched_WaitState w;
      CpuSched_RunState r;

      // reset run state meters
      for (r = 0; r < CPUSCHED_NUM_RUN_STATES; r++) {
         CpuSched_StateMeter *m = &vcpu->runStateMeter[r];
         CpuSchedStateMeterReset(m);
      }

      // reset wait state meters
      for (w = 0; w < CPUSCHED_NUM_WAIT_STATES; w++) {
         CpuSched_StateMeter *m = &vcpu->waitStateMeter[w];
         CpuSchedStateMeterReset(m);
      }
      
      // reset other meters
      CpuSchedStateMeterReset(&vcpu->wakeupLatencyMeter);
      CpuSchedStateMeterReset(&vcpu->limboMeter);
      
      // reset other histograms
      if (CPUSCHED_STATE_HISTOGRAMS) {
         Histogram_Reset(vcpu->runWaitTimeHisto);
         Histogram_Reset(vcpu->preemptTimeHisto);
      }

      // reset hyperthreaded event count stats
      memset(&vcpu->htEvents, 0, sizeof(CpuSched_HTEventCount));
      vsmp->numQuarantines = 0;
      vsmp->quarantinePeriods = 0;

      // reset pcpu run times
      FORALL_PCPUS(p) {
         vcpu->pcpuRunTime[p] = 0;
      } PCPUS_DONE;

      // reset stats
      memset(&vcpu->stats, 0, sizeof(CpuSched_VcpuStats));
      vcpu->stats.uptimeStart = vsmp->cell->now;

      // reset previous load history values
      if (vcpu->loadHistory != NULL) {
         CpuMetrics_LoadHistoryReset(vcpu->loadHistory);
      }

      // reset other aggregate stats
      vcpu->sysCyclesTotal = 0;   
      vcpu->sysOverlapTotal = 0;
      CpuSchedVcpuChargeCyclesTotalSet(vcpu, 0);
   } VSMP_VCPUS_DONE;
}


/*
 *----------------------------------------------------------------------
 *
 * CpuSchedGroupStatsReset --
 *
 *	Callback function used by CpuSchedProcResetStatsWrite().
 *	Resets scheduler stats for scheduler group "g".
 *
 * Results:
 *	Modifies stats values for scheduler group "g".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedGroupStatsReset(Sched_Group *g, UNUSED_PARAM(void *ignore))
{
   CpuSched_GroupState *cpuGroup = &g->cpu;
   cpuGroup->chargeCyclesTotal = 0;
   cpuGroup->vtimeAged = 0;
   if (cpuGroup->loadHistory != NULL) {
      CpuMetrics_LoadHistoryReset(cpuGroup->loadHistory);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedProcResetStatsWrite --
 *
 *	Proc write handler: echo "reset" here to reset all
 *	scheduler stats.
 *
 * Results:
 *	Returns VMK_OK or VMK_BAD_PARAM.
 *
 * Side effects:
 *      Resets scheduler statistics.
 *
 *----------------------------------------------------------------------
 */
static int 
CpuSchedProcResetStatsWrite(UNUSED_PARAM(Proc_Entry *e), char *buf, int *len)
{
   if (strncmp(buf, "reset", 5) == 0) {
      SP_IRQL prevIRQL;

      prevIRQL = CpuSchedLockAllCells();

      FORALL_PCPUS(p) {
         CpuSched_Pcpu* pcpu = CpuSchedPcpu(p);
         CpuSchedPackageHaltLock(p);
         memset(&pcpu->stats, 0, sizeof(CpuSched_PcpuStats));         
         pcpu->usedCycles = 0;
         pcpu->idleCycles = 0;
         CpuSchedPackageHaltUnlock(p);
      } PCPUS_DONE;

      FORALL_CELLS(c) {
         FORALL_CELL_VSMPS(c, vsmp) {
            CpuSchedVsmpStatsReset(vsmp);
         } CELL_VSMPS_DONE;

         CpuSchedCellStatsReset(c);
      } CELLS_DONE;

      Sched_ForAllGroupsDo(CpuSchedGroupStatsReset, NULL);

      CpuSchedUnlockAllCells(prevIRQL);

      Log("Reset scheduler statistics");
      return (VMK_OK);
   } else {
      Log("Command not understood");
      return (VMK_BAD_PARAM);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSched_UpdateConfig --
 *
 *     Callback for changes to cpusched-related config variables.
 *
 * Results:
 *     None
 *
 * Side effects:
 *     Updates global scheduler configuration
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus 
CpuSched_UpdateConfig(Bool write, Bool valueChanged, int indx)
{
   if (write && valueChanged) {
      CpuSchedConfig config;
      SP_IRQL prevIRQL;

      // obtain current config values
      CpuSchedConfigInit(&config);

      prevIRQL = CpuSchedLockAllCells();

      // replicate to all cells
      FORALL_CELLS(c) {
         c->config = config;
      } CELLS_DONE;

      // remove and re-install all skew timers if the
      // sample interval has been changed
      if (indx == CONFIG_CPU_SKEW_SAMPLE_USEC) {
         FORALL_SCHED_PCPUS(p) {
            Timer_Remove(p->skewTimer);
            p->skewTimer = Timer_AddHiRes(p->id,
                                          CpuSchedSampleSkew,
                                          config.skewSampleUsec,
                                          TIMER_PERIODIC,
                                          NULL);
            ASSERT(p->skewTimer != TIMER_HANDLE_NONE);
         } SCHED_PCPUS_DONE;
      }
      
      // update console warp amount
      cpuSched.vtConsoleWarpDelta =
         CpuSchedTCToVtime(CONSOLE_VSMP->vtime.stride, config.consoleWarpCycles);

      CpuSchedUnlockAllCells(prevIRQL);
   }
   return VMK_OK;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CpuSched_UpdateCOSMin --
 *
 *     Config callback handler to change the cpu "min" of the COS world.
 *
 * Results:
 *     None.
 *
 * Side effects:
 *     May update COS min, may acquire scheduler cell locks.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus 
CpuSched_UpdateCOSMin(Bool write, Bool valueChanged, UNUSED_PARAM(int indx))
{
   VMK_ReturnStatus status = VMK_OK;
   if (write && valueChanged) {
      SP_IRQL prevIRQL = CpuSchedLockAllCells();
      status = CpuSchedVsmpSetMin(CONSOLE_VSMP, CONFIG_OPTION(CPU_COS_MIN_CPU));
      CpuSchedUnlockAllCells(prevIRQL);
   }
   return status;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedSetHTSharing --
 *
 *     Sets the hyperthread sharing constraints for "vsmp" to "shareType"
 *     Caller must hold scheduler cell lock for "vsmp".
 *
 * Results:
 *     Sets the hyperthread sharing constraints for "vsmp" to "shareType"
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static void
CpuSchedSetHTSharing(CpuSched_Vsmp *vsmp, 
                     Sched_HTSharing newShareType)
{
   Sched_HTSharing shareType = newShareType;
   ASSERT(SMP_HTEnabled());
   ASSERT(CpuSchedVsmpCellIsLocked(vsmp));

   if (vsmp->vcpus.len != 2
       && shareType == CPUSCHED_HT_SHARE_INTERNALLY) {
      VsmpWarn(vsmp,
               "internal sharing is only permitted on 2-way SMP VMs,"
               " changing sharing type to %s",
               CpuSchedHTSharingName(CPUSCHED_HT_SHARE_NONE));
      shareType = CPUSCHED_HT_SHARE_NONE;
   }
   
   if (vsmp->htSharing != shareType) {
      Sched_HTSharing maxShare = CpuSchedVsmpMaxHTConstraint(vsmp);
      vsmp->htSharing = shareType;
      
      if (maxShare < shareType) {
         Warning("based on current affinity, HT sharing type of %s"
                 " is not allowed, %s will be used instead",
                 CpuSchedHTSharingName(shareType),
                 CpuSchedHTSharingName(maxShare));
      }
      FORALL_VSMP_VCPUS(vsmp, vcpu) {
         CpuSchedPcpuPreemptionInvalidate(CpuSchedPcpu(vcpu->pcpu));
         CpuSchedPcpuPreemptionInvalidate(CpuSchedPartnerPcpu(vcpu->pcpu));
      } VSMP_VCPUS_DONE;
   }
}


// cpusched NUMA support
/*
 *-----------------------------------------------------------------------------
 *
 * CpuSched_SetHomeNode --
 *
 *     Changes the home node of "world's" vsmp to "nodeNum,"
 *     setting soft memory and cpu affinity to that node as well.
 *  
 *     Grabs the CpuSched lock
 *
 * Results:
 *     None
 *
 * Side effects:
 *     Updates cpu/mem affinity for all vcpus in vsmp
 *
 *-----------------------------------------------------------------------------
 */
void
CpuSched_SetHomeNode(World_Handle *world, NUMA_Node nodeNum)
{
   SP_IRQL prevIRQL;
   CpuSched_Vsmp *vsmp;

   // currently only vmm and user worlds are supproted form NUMA scheduling.
   if (UNLIKELY(!(World_IsVMMWorld(world) || World_IsUSERWorld(world)))) {
      VmWarn(world->worldID, "skip setting home node for non-vmm/user world");
      return;
   }

   prevIRQL = CpuSchedLockAllCells();

   vsmp = World_CpuSchedVsmp(world);

   vsmp->numa.homeNode = nodeNum;
   vsmp->numa.lastMigrateTime = Timer_GetCycles();
   vsmp->numa.nextMigrateAllowed = vsmp->numa.lastMigrateTime
      + CONFIG_OPTION(NUMA_MIN_MIGRATE_INTERVAL)
      * cpuSchedConst.cyclesPerSecond;

   if (nodeNum != INVALID_NUMANODE) {
      // hard affinity takes precedence, so don't clobber it with soft affinity
      if (!vsmp->hardAffinity) {
         CpuSchedVsmpSetSoftAffinity(vsmp, cpuSchedConst.numaNodeMasks[nodeNum]);
      } else {
         // shouldn't be trying to set a home node that conflicts with hard affinity
         ASSERT((vsmp->vcpus.list[0]->affinityMask & cpuSchedConst.numaNodeMasks[nodeNum]) 
                == vsmp->vcpus.list[0]->affinityMask);
      }
      CpuSchedUnlockAllCells(prevIRQL);

      MemSched_SetNodeAffinity(vsmp->leader, MEMSCHED_NODE_AFFINITY(nodeNum), FALSE);
   } else {
      if (!vsmp->hardAffinity) {
         // unset cpu affinity
         CpuSchedVsmpSetSoftAffinity(vsmp, cpuSchedConst.defaultAffinity);
      }
      CpuSchedUnlockAllCells(prevIRQL);

      if (!MemSched_HasNodeHardAffinity(vsmp->leader)) {
         // unset memory affinity
         MemSched_SetNodeAffinity(vsmp->leader, MEMSCHED_NODE_AFFINITY_NONE, FALSE);
      }
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSched_NUMASnap --
 *
 *     Saves information about all vsmps in "info" for use in NUMA rebalancing.
 *
 * Results:
 *     Stores scheduler data in "info"
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
void
CpuSched_NUMASnap(NUMASched_Snap *snap) 
{
   SP_IRQL prevIRQL;
   uint32 i;

   prevIRQL = CpuSchedLockAllCells();

   // save node idle times
   // find per-node idle time
   FORALL_NUMA_NODES(n) {
      snap->nodeIdleTotal[n] = 0;
      FORALL_NODE_PCPUS(n, p) {
         snap->nodeIdleTotal[n] += CpuSchedPcpu(p)->idleCycles;
      } NODE_PCPUS_DONE;
   } NUMA_NODES_DONE;

   snap->totalShares = 0;
   snap->numVsmps = 0;
   i = 0;
   FORALL_CELLS(c) {
      FORALL_CELL_VSMPS(c, vsmp) {
         NUMASched_VsmpNUMASnap(vsmp, &snap->vsmps[i++]);
         snap->numVsmps++;

         // only consider non-system shares for now (seems to work much better)
         if (World_IsVMMWorld(vsmp->leader) ||
             World_IsTESTWorld(vsmp->leader)) {
            snap->totalShares += vsmp->base.shares;
         }
      } CELL_VSMPS_DONE;
   } CELLS_DONE;

   CpuSchedUnlockAllCells(prevIRQL);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSched_ResetNUMAStats --
 *
 *     Resets NUMASched stats for all vsmps.
 *     Grabs all scheduler cell locks.
 *
 * Results:
 *     None.
 *
 * Side effects:
 *     Resets NUMASched stats for all vsmps.
 *
 *-----------------------------------------------------------------------------
 */
void
CpuSched_ResetNUMAStats(void)
{
   SP_IRQL prevIRQL = CpuSchedLockAllCells();
   FORALL_CELLS(c) {
      FORALL_CELL_VSMPS(c, vsmp) {
         memset(&vsmp->numa.stats, 0, sizeof(NUMASched_Stats));
         vsmp->numa.lastMigrateTime = 0;
      } CELL_VSMPS_DONE;
   } CELLS_DONE;
   CpuSchedUnlockAllCells(prevIRQL);
}


/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedProcWaitStatesHistoRead --
 *
 *     Proc callback to display the histogram of run state times for
 *     the given vcpu. ("/proc/vmware/<vmid>/cpu/wait-state-histo")
 *
 * Results:
 *     Returns VMK_OK
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static int
CpuSchedProcWaitStatesHistoRead(Proc_Entry *entry, char *buffer, int *len)
{
   CpuSched_Vcpu *vcpu = entry->private;
   CpuSched_Vsmp *vsmp = vcpu->vsmp;
   int i, j;
   SP_IRQL prevIRQL;
   World_Handle *world;
   uint32 numBuckets;
   Bool showHisto[CPUSCHED_NUM_WAIT_STATES];

   *len = 0;

   world = World_Find(World_VcpuToWorld(vcpu)->worldID);
   prevIRQL = CpuSchedVsmpCellLock(vsmp);

   // make sure this world isn't disappearing right now
   if (vcpu->runState == CPUSCHED_ZOMBIE) {
      LOG(0, "Can't read histogram from ZOMBIE world");
      CpuSchedVsmpCellUnlock(vsmp, prevIRQL);
      World_Release(world);
      return VMK_BUSY;
   }

   numBuckets = Histogram_NumBuckets(vcpu->waitStateMeter[0].histo);

   Proc_Printf(buffer, len, "bucket             ");
   for (j=0; j < CPUSCHED_NUM_WAIT_STATES; j++) {
      if (Histogram_Count(vcpu->waitStateMeter[j].histo) != 0) {
         showHisto[j] = TRUE;
         Proc_Printf(buffer, len, " %11s ", CpuSchedWaitStateName(j));
      } else {
         showHisto[j] = FALSE;
      }
   }

   Proc_Printf(buffer, len, "     WakeLat\n");

   // print out each bucket for each state
   for (i=0; i < numBuckets; i++) {
      if (i != numBuckets - 1) {
         Proc_Printf(buffer, len, "(<  %8Ld us)   ", 
                     Timer_TCToUS(Histogram_BucketLimit(vcpu->waitStateMeter[0].histo, i)));
      } else {
         Proc_Printf(buffer, len, "(>= %8Ld us)   ", 
                     Timer_TCToUS(Histogram_BucketLimit(vcpu->waitStateMeter[0].histo, i - 1)));
      }

      for (j=0; j < CPUSCHED_NUM_WAIT_STATES; j++) {
         if (showHisto[j]) {
            Proc_Printf(buffer, len, " %11Ld ", 
                        Histogram_BucketCount(vcpu->waitStateMeter[j].histo, i));
         }
      }
      
      // Display the synthetic state histograms
      Proc_Printf(buffer, len, " %11Ld ", 
                  Histogram_BucketCount(vcpu->wakeupLatencyMeter.histo, i));
      Proc_Printf(buffer, len, "\n");
   }
   
   CpuSchedVsmpCellUnlock(vsmp, prevIRQL);
   World_Release(world);

   return VMK_OK;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedProcRunStatesHistoRead --
 *
 *     Proc callback to display the histogram of run state times for
 *     the given vcpu. ("/proc/vmware/<vmid>/cpu/wait-state-histo")
 *
 * Results:
 *     Returns VMK_OK, or VMK_BUSY if you try to print the histogram
 *     for a world that is currently being removed.
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static int
CpuSchedProcRunStatesHistoRead(Proc_Entry *entry, char *buffer, int *len)
{
   CpuSched_Vcpu *vcpu = entry->private;
   CpuSched_Vsmp *vsmp = vcpu->vsmp;
   int i, j;
   SP_IRQL prevIRQL;
   uint32 numBuckets;
   World_Handle *world;
   
   *len = 0;

   // format header
   Proc_Printf(buffer, len, "bucket            ");
   for (j=0; j < CPUSCHED_NUM_RUN_STATES; j++) {
      Proc_Printf(buffer, len, " %11s ", CpuSchedRunStateName(j));
   }
   Proc_Printf(buffer, len,
               "     runWait      preempt      disablePreempt   maxLimited"
               "\n");

   world = World_Find(World_VcpuToWorld(vcpu)->worldID);
   prevIRQL = CpuSchedVsmpCellLock(vsmp);

   // make sure this world isn't disappearing right now
   if (vcpu->runState == CPUSCHED_ZOMBIE) {
      CpuSchedVsmpCellUnlock(vsmp, prevIRQL);
      LOG(0, "Can't read histogram from ZOMBIE world");
      World_Release(world);
      return VMK_BUSY;
   }

   numBuckets = Histogram_NumBuckets(vcpu->runStateMeter[0].histo);

   // print out each bucket for each state
   for (i=0; i < numBuckets; i++) {
      if (i != numBuckets - 1) {
         Proc_Printf(buffer, len, "(<  %8Ld us)  ", 
                     Timer_TCToUS(Histogram_BucketLimit(vcpu->runStateMeter[0].histo, i)));
      } else {
         Proc_Printf(buffer, len, "(>= %8Ld us)  ", 
                     Timer_TCToUS(Histogram_BucketLimit(vcpu->runStateMeter[0].histo, i - 1)));
      }

      for (j=0; j < CPUSCHED_NUM_RUN_STATES; j++) {
         Proc_Printf(buffer, len, " %11Ld ", 
                     Histogram_BucketCount(vcpu->runStateMeter[j].histo, i));
      }
      Proc_Printf(buffer, len, " %11Ld ", 
                  Histogram_BucketCount(vcpu->runWaitTimeHisto, i));
      Proc_Printf(buffer, len, " %11Ld ", 
                  Histogram_BucketCount(vcpu->preemptTimeHisto, i));
      Proc_Printf(buffer, len, "        %11Ld ", 
                  CPUSCHED_PREEMPT_STATS ? Histogram_BucketCount(vcpu->disablePreemptTimeHisto, i) : 0);
      Proc_Printf(buffer, len, " %11Ld",
                  Histogram_BucketCount(vcpu->limboMeter.histo, i));

      Proc_Printf(buffer, len, "\n");
   }

   Proc_Printf(buffer, len, "       mean (us)  ");
   for (j=0; j < CPUSCHED_NUM_RUN_STATES; j++) {
      Proc_Printf(buffer, len, " %11Ld ",
                  Timer_TCToUS(Histogram_Mean(vcpu->runStateMeter[j].histo)));
   }
   Proc_Printf(buffer, len, " %11Ld ", 
               Timer_TCToUS(Histogram_Mean(vcpu->runWaitTimeHisto)));
   Proc_Printf(buffer, len, " %11Ld ", 
               Timer_TCToUS(Histogram_Mean(vcpu->preemptTimeHisto)));
   Proc_Printf(buffer, len, "        %11Ld ", 
               CPUSCHED_PREEMPT_STATS ?
               Timer_TCToUS(Histogram_Mean(vcpu->disablePreemptTimeHisto)) : 0);
   Proc_Printf(buffer, len, " %11Ld\n",
               Timer_TCToUS(Histogram_Mean(vcpu->limboMeter.histo)));

   CpuSchedVsmpCellUnlock(vsmp, prevIRQL);
   World_Release(world);

   return VMK_OK;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedParseHistoLimits --
 *
 *     Utility function to parse a list of bucket limits (int64's).
 *     Resulting limits are stored in "newLimits" and number of buckets
 *     stored in "numBuckets"
 *
 * Results:
 *     Returns VMK_OK on success, VMK_BAD_PARAM if the limits were invalid
 *
 * Side effects:
 *     May modify "buffer"
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
CpuSchedParseHistoLimits(char *buffer, int64 newLimits[], uint32 *numBuckets)
{
   int argc, i;
   char *argv[HISTOGRAM_BUCKETS_MAX];
   
   argc = Parse_Args(buffer, argv, HISTOGRAM_BUCKETS_MAX);

   if (argc < 1) {
      LOG(0, "Failed to reconfigure histogram, invalid bucket limits");
      return VMK_BAD_PARAM;
   } else if (argc >= HISTOGRAM_BUCKETS_MAX - 1) {
      LOG(0, "Too many buckets for histogram: %d, max=%d", 
          argc, HISTOGRAM_BUCKETS_MAX);
      return VMK_BAD_PARAM;
   }

   // parse the limits and store them in the input array.
   for (i=0; i < argc; i++) {
      if (Parse_Int64(argv[i], strlen(argv[i]), &newLimits[i]) != VMK_OK) {
         LOG(1, "parsed limit: %Ld", newLimits[i]);
         LOG(0, "invalid integer format: %s", argv[i]);
         return VMK_BAD_PARAM;
      }
      newLimits[i] = Timer_USToTC(newLimits[i]);
      if (i > 0 && newLimits[i] <= newLimits[i - 1]) {
         LOG(0, "invalid limits -- must be monotonically increasing");
      }
   }

   *numBuckets = argc + 1;

   return VMK_OK;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedProcRunStatesHistoWrite --
 *
 *     Proc write callback to reconfigure bucket limits of run states histo.
 *
 * Results:
 *     Returns VMK_OK on success, VMK_BAD_PARAM otherwise
 *
 * Side effects:
 *     Resets the run states histogram.
 *
 *-----------------------------------------------------------------------------
 */
static int 
CpuSchedProcRunStatesHistoWrite(Proc_Entry *entry, char *buffer, int *len)
{
   CpuSched_Vcpu *vcpu = entry->private;
   CpuSched_Vsmp *vsmp = vcpu->vsmp;
   int64 newLimits[HISTOGRAM_BUCKETS_MAX];
   uint32 numBuckets;
   VMK_ReturnStatus res;
   CpuSched_RunState r;
   SP_IRQL prevIRQL;
   Heap_ID groupHeap = World_VcpuToWorld(vcpu)->group->heap;
   
   res = CpuSchedParseHistoLimits(buffer, newLimits, &numBuckets);
   if (res != VMK_OK) {
      return res;
   }

   prevIRQL = CpuSchedVsmpCellLock(vsmp);

   for (r = 0; r < CPUSCHED_NUM_RUN_STATES; r++) {
      Histogram_Reconfigure(groupHeap, vcpu->runStateMeter[r].histo, numBuckets, newLimits);
   }
   
   CpuSchedVsmpCellUnlock(vsmp, prevIRQL);

   return VMK_OK;
}



/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedProcWaitStatesHistoWrite --
 *
 *     Proc write callback to reconfigure bucket limits of wait states histo.
 *
 * Results:
 *     Returns VMK_OK on success, VMK_BAD_PARAM otherwise
 *
 * Side effects:
 *     Resets the wait states histogram.
 *
 *-----------------------------------------------------------------------------
 */
static int 
CpuSchedProcWaitStatesHistoWrite(Proc_Entry *entry, char *buffer, int *len)
{
   CpuSched_Vcpu *vcpu = entry->private;
   CpuSched_Vsmp *vsmp = vcpu->vsmp;
   int64 newLimits[HISTOGRAM_BUCKETS_MAX];
   uint32 numBuckets;
   VMK_ReturnStatus res;
   CpuSched_WaitState w;
   SP_IRQL prevIRQL;
   Heap_ID groupHeap = World_VcpuToWorld(vcpu)->group->heap;

   res = CpuSchedParseHistoLimits(buffer, newLimits, &numBuckets);
   if (res != VMK_OK) {
      return res;
   }

   prevIRQL = CpuSchedVsmpCellLock(vsmp);

   for (w = 0; w < CPUSCHED_NUM_WAIT_STATES; w++) {
      Histogram_Reconfigure(groupHeap, vcpu->waitStateMeter[w].histo, numBuckets, newLimits);
   }

   Histogram_Reconfigure(groupHeap, vcpu->limboMeter.histo, numBuckets, newLimits);
   Histogram_Reconfigure(groupHeap, vcpu->wakeupLatencyMeter.histo, numBuckets, newLimits);
   Histogram_Reconfigure(groupHeap, vcpu->preemptTimeHisto, numBuckets, newLimits);
   Histogram_Reconfigure(groupHeap, vcpu->runWaitTimeHisto, numBuckets, newLimits);
   
   CpuSchedVsmpCellUnlock(vsmp, prevIRQL);

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedWorldPseudoTSCConvCB --
 *
 *      Periodic update of a world's TSC to pseudo-TSC conversion
 *      parameters.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	New conversion parameters stored in world->vmkSharedData.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedWorldPseudoTSCConvCB(void *data, Timer_AbsCycles timestamp)
{
   World_Handle *world = World_Find((World_ID)data);
   CpuSched_Vcpu *vcpu;
   CpuSched_Vsmp *vsmp;
   SP_IRQL prevIRQL;
   PCPU pcpu;

   if (!world) {
      // Orphan call; world has been cleaned up
      return;
   }

   // convenient abbrevs
   vcpu = World_CpuSchedVcpu(world);
   vsmp = vcpu->vsmp;

   prevIRQL = CpuSchedVsmpCellLock(vsmp);
   pcpu = vcpu->pcpu;
   if (pcpu == MY_PCPU) {
      // update pseudo-TSC parameters
      Timer_UpdateWorldPseudoTSCConv(world, timestamp);
   }
   CpuSchedVsmpCellUnlock(vsmp, prevIRQL);

   if (pcpu != MY_PCPU) {
      // Move the timer to the new PCPU
      Bool found;
      found = Timer_Remove(world->pseudoTSCTimer);
      ASSERT(found);
      world->pseudoTSCTimer =
         Timer_Add(pcpu, CpuSchedWorldPseudoTSCConvCB,
                   PSEUDO_TSC_TIMER_PERIOD_MS, TIMER_PERIODIC,
                   (void *) world->worldID);
   }

   World_Release(world);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_GetAlloc --
 *
 *      Fills in the alloc struct with the current cpu allocation
 *      parameters.
 *
 * Results:
 *	A CpuSched_Alloc alloc struct
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
CpuSched_GetAlloc(World_Handle *world, CpuSched_Alloc *alloc)
{
   CpuSched_Vsmp *vsmp = World_CpuSchedVsmp(world);
   SP_IRQL prevIRQL;

   prevIRQL = CpuSchedVsmpCellLock(vsmp);
   *alloc = vsmp->alloc;
   CpuSchedVsmpCellUnlock(vsmp, prevIRQL);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedVsmpAllocAllowed --
 *
 *     Returns TRUE iff it is permissible to set "vsmp"'s alloc to "alloc"
 *     Includes cpu min admission control check and general sanity
 *     checks on min/max/shares.
 *
 *     The size of the vsmp is taken from the "numVcpus" parameter, rather
 *     than from "vsmp" so that this function can be used when the vsmp
 *     is being created and not all of its vcpus have been added yet.
 *     Caller must hold all scheduler cell locks and scheduler tree lock.
 *
 * Results:
 *     Returns TRUE iff it is permissible to set "vsmp"'s alloc to "alloc".
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static Bool
CpuSchedVsmpAllocAllowed(const CpuSched_Vsmp *vsmp, 
                         const CpuSched_Alloc *alloc, 
                         uint8 numVcpus)
{
   const CpuSched_Alloc *oldAlloc = &vsmp->alloc;
   uint32 usedMinBase, unusedMinBase, maxLimit, oldMinBase, newMinBase;
   Sched_Node *parent;

   // sanity checks
   ASSERT(CpuSchedAllCellsAreLocked());
   ASSERT(Sched_TreeIsLocked());

   // initialize
   maxLimit = cpuSchedConst.unitsPerPkg[alloc->units] * numVcpus;

   // fail if min outside valid range
   if (alloc->min > maxLimit) {
      VsmpWarn(vsmp, "invalid min=%u", alloc->min);
      return(FALSE);
   }

   // fail if max outside valid range
   if (alloc->max > maxLimit) {
      VsmpWarn(vsmp, "invalid max=%u", alloc->max);
      return(FALSE);
   }

   // fail if min exceeds max
   if (CpuSchedEnforceMax(alloc) && (alloc->min > alloc->max)) {
      VsmpWarn(vsmp, "invalid min=%u > max=%u", alloc->min, alloc->max);
      return(FALSE);
   }

   // fail if shares outside valid range
   if ((alloc->shares < CPUSCHED_SHARES_MIN) ||
       (alloc->shares > CPUSCHED_SHARES_MAX)) {
      VsmpWarn(vsmp, "invalid shares=%u", alloc->shares);
      return(FALSE);
   }

   // lookup reservations allocated to parent
   parent = CpuSchedVsmpNode(vsmp)->parent;
   CpuSchedNodeReservedMin(parent, &usedMinBase, &unusedMinBase);

   // convert to common units
   oldMinBase = CpuSchedUnitsToBaseShares(oldAlloc->min, oldAlloc->units);
   newMinBase = CpuSchedUnitsToBaseShares(alloc->min, alloc->units);

   // perform admission control check if increasing min
   if (newMinBase > oldMinBase) {
      uint32 needMinBase = newMinBase - oldMinBase;
      if (needMinBase > unusedMinBase) {
         VsmpWarn(vsmp, "invalid min %u %s: "
                  "parent min reserved=%u, unreserved=%u, need=%u",
                  alloc->min,
		  Sched_UnitsToString(alloc->units),
		  CpuSchedBaseSharesToUnits(usedMinBase, alloc->units),
		  CpuSchedBaseSharesToUnits(unusedMinBase, alloc->units),
		  CpuSchedBaseSharesToUnits(needMinBase, alloc->units));
         return(FALSE);
      }
   }

   // XXX Note that affinity checks are no longer performed, so 
   //     admission control is somewhat broken for affinity-constrained
   //     VMs with specified mins.  We might want to fix this, or maybe
   //     restrict some user-specified resource configurations instead.

   return(TRUE);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedProcWorldHyperthreadingRead --
 *
 *     Proc read handler for /proc/vmware/vm/<vmid>/cpu/hyperthreading.
 *     Displays hyperthreading-related stats and config.
 *
 * Results:
 *     Returns VMK_OK on success
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static int 
CpuSchedProcWorldHyperthreadingRead(Proc_Entry *e, char *buf, int *len)
{
   CpuSched_Vsmp *vsmp;
   CpuSched_Vcpu *vcpu;
   World_Handle *world;

   if (!SMP_HTEnabled()) {
      Proc_Printf(buf, len, "HT not enabled\n");
      return (VMK_OK);
   }

   world = World_Find((World_ID)e->private);
   if (!world) {
      WarnVmNotFound((World_ID)e->private);
      return (VMK_NOT_FOUND);
   }

   vcpu = World_CpuSchedVcpu(world);
   vsmp = vcpu->vsmp;
   
   *len = 0;
   Proc_Printf(buf, len, 
               "htSharing:             %s\n"
               "maxSharing:            %s\n"
               "\n"
               "vcpuTotalSamples:      %u\n"
               "vcpuWholePkgSamples:   %u\n"
               "\n"
               "vsmpTotalSamples:      %u\n"
               "vsmpAllWhole:          %u\n"
               "vsmpAllHalf:           %u\n"
               "vsmpMixed:             %u\n",
               CpuSchedHTSharingName(vsmp->htSharing),
               CpuSchedHTSharingName(vsmp->maxHTConstraint),
               vcpu->stats.htTotalSamples,
               vcpu->stats.htWholePackageSamples,
               vsmp->stats.htTotalSamples,
               vsmp->stats.htAllWholeSamples,
               vsmp->stats.htAllHalfSamples,
               vsmp->stats.htMixedRunSamples);
   World_Release(world);
   return (VMK_OK);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedProcWorldHyperthreadingWrite --
 *
 *     Proc write handler for /proc/vmware/vm/<vmid>/cpu/hyperthreading.
 *     Currently support "reset" command to reset stats
 *
 * Results:
 *     Returns 
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static int
CpuSchedProcWorldHyperthreadingWrite(Proc_Entry *e, char *buffer, int *len)
{
   World_Handle *world;
   SP_IRQL prevIRQL;
   CpuSched_Vcpu *vcpu;
   CpuSched_Vsmp *vsmp;
   VMK_ReturnStatus res;

   if (!SMP_HTEnabled()) {
      return (VMK_NOT_SUPPORTED);
   }

   world = World_Find((World_ID)e->private);
   if (!world) {
      WarnVmNotFound((World_ID)e->private);
      return (VMK_NOT_FOUND);
   }

   vcpu = World_CpuSchedVcpu(world);
   vsmp = vcpu->vsmp;

   prevIRQL = CpuSchedVsmpCellLock(vsmp);

   if (CONST_STRNCMP(buffer, "reset") == 0) {
      VmLog((uint32)e->private, "resetting world hyperthreading stats");

      vcpu->stats.htTotalSamples = 0;
      vcpu->stats.htWholePackageSamples = 0;
      vsmp->stats.htTotalSamples = 0;
      vsmp->stats.htAllWholeSamples = 0;
      vsmp->stats.htAllHalfSamples = 0;
      vsmp->stats.htMixedRunSamples = 0;
      res = VMK_OK;
   } else if (CONST_STRNCMP(buffer, "any") == 0) {
      VcpuLog(vcpu, "allow any HT sharing");
      CpuSchedSetHTSharing(vsmp, CPUSCHED_HT_SHARE_ANY);
      res = VMK_OK;
   } else if (CONST_STRNCMP(buffer, "internal") == 0) {
      VcpuLog(vcpu, "allow internal HT sharing");
      CpuSchedSetHTSharing(vsmp, CPUSCHED_HT_SHARE_INTERNALLY);
      res = VMK_OK;
   } else if (CONST_STRNCMP(buffer, "none") == 0) {
      VcpuLog(vcpu, "disallow HT sharing");
      CpuSchedSetHTSharing(vsmp, CPUSCHED_HT_SHARE_NONE);
      res = VMK_OK;
   } else {
      Log("unknown command");
      res = VMK_BAD_PARAM;
   }

   CpuSchedVsmpCellUnlock(vsmp, prevIRQL);
   World_Release(world);
   return (res);   
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSched_SysServiceDoneSample --
 *
 *     Accounts for system service time on the current processor in the
 *     case where we're doing a real sample (vmkServiceStart != 0).
 *     Must be called while the current world is not preemptible.
 *
 * Results:
 *     None.
 *
 * Side effects:
 *     Modifies global scheduler state.
 *
 *-----------------------------------------------------------------------------
 */
void
CpuSched_SysServiceDoneSample(void)
{
   TSCCycles nowTSC, startTSC, elapsedTSC;
   Timer_Cycles elapsed;
   uint32 elapsedKTC;
   PCPU localPCPU;
   World_Handle *vmkWorld;

   ASSERT(!CpuSched_IsPreemptible());

   // convenient abbrevs
   startTSC = myPRDA.vmkServiceStart;
   vmkWorld  = myPRDA.vmkServiceWorld;
   localPCPU = MY_PCPU;

   // compute elapsed cycles, adjust for statistical sampling rate
   nowTSC = RDTSC();
   ASSERT(nowTSC > startTSC);
   elapsedTSC = nowTSC - startTSC;
   elapsedTSC *= SCHED_SYS_ACCT_SAMPLE;
   if (UNLIKELY(!RateConv_IsIdentity(&myPRDA.tscToTC))) {
      elapsed = RateConv_Unsigned(&myPRDA.tscToTC, elapsedTSC);
   } else {
      elapsed = elapsedTSC;
   }
   
   // track system time overlapped with interrupted world
   myPRDA.runningWorld->sched.cpu.vcpu.sysCyclesOverlap += elapsed;

   // double charge responsible VM if our partner is halted
   if (SMP_HTEnabled()) {
      if (MY_PARTNER_PRDA->halted) {
         elapsed *= 2;
      }
   }

   // track system time for specified world,
   //   but charge idle world instead if unspecified
   if (vmkWorld == NULL) {
      vmkWorld = World_GetIdleWorld(localPCPU);
   }

   // atomically update elapsed system time
   elapsedKTC = (uint32) (elapsed >> 10);
   if (elapsedKTC > 0) {
      // OPT: possibly use Lamport atomic versioning here instead
      Atomic_Add(&vmkWorld->sched.cpu.vcpu.sysKCycles, elapsedKTC);
   }
   
   // account on a per-pcpu, per-vector basis too (in TC units)
   IT_AccountSystime(myPRDA.vmkServiceVector, elapsed);
   
   // clear start time
   myPRDA.vmkServiceStart = 0;
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSched_GetLoadMetrics --
 *
 *	Sets "m" to reflect current cpu load metrics.  These
 *	metrics include the number of active VCPUs, the number
 *	of active VMs, and the number of active base shares,
 *	where active is defined as "running or ready to run".
 *
 * Results:
 *	Modifies "m".
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */
void
CpuSched_GetLoadMetrics(CpuSched_LoadMetrics *m)
{
   SP_IRQL prevIRQL;

   // initialize
   memset(m, 0, sizeof(CpuSched_LoadMetrics));

   prevIRQL = CpuSchedLockAllCells();

   // examine all vcpus in all cells
   FORALL_CELLS(c) {
      // update global times
      CpuSchedCellUpdateTime(c);
      
      FORALL_CELL_VSMPS(c, vsmp) {
         Bool vsmpActive = FALSE;

         // update stats for runnable, non-limbo, non-idle vcpus
         FORALL_VSMP_VCPUS(vsmp, vcpu) {
            if (CpuSchedVcpuIsRunnable(vcpu) &&
                !vcpu->limbo &&
                !CpuSchedVcpuIsIdle(vcpu)) {
               m->vcpus++;
               vsmpActive = TRUE;               
            }
         } VSMP_VCPUS_DONE;

         if (vsmpActive) {
            m->vms++;
            m->baseShares += vsmp->base.shares;
         }
      } CELL_VSMPS_DONE;
   } CELLS_DONE;

   CpuSchedUnlockAllCells(prevIRQL);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedIsValidNode --
 *
 *      Returns TRUE iff the scheduler state is consistent with
 *	respect to the current state of tree node "n".
 *
 * Results:
 *      Returns TRUE iff the scheduler state is consistent with
 *	respect to the current state of tree node "n".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
CpuSchedIsValidNode(const Sched_Node *n)
{
   ASSERT(Sched_TreeIsLocked());
   switch (n->nodeType) {
   case SCHED_NODE_TYPE_VM:
      return(n->u.world->sched.group.cpuValid);
   case SCHED_NODE_TYPE_GROUP:
      return(TRUE);
   default:
      NOT_REACHED();
      return(FALSE);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedSampleLoadHistoryNodes --
 *
 *	Recursively samples the incremental run and ready times for all
 *	nodes in the scheduler tree rooted at "n" since last invocation.  
 *	Sets "nodeRun" and "nodeReady" to the incremental run and
 *	ready times for node "n", respectively.  Caller must hold
 *	all scheduler cell locks and the scheduler tree lock.
 *
 * Results:
 *	Sets "nodeRun" and "nodeReady" to the incremental run and
 *	ready times for node "n", respectively.
 *
 * Side effects:
 *      Updates load history state for all nodes in the scheduler
 *	tree rooted at "n".
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedSampleLoadHistoryNodes(const Sched_Node *n,
                               Timer_Cycles *nodeRun,
                               Timer_Cycles *nodeReady)
{
   Timer_Cycles run, ready;

   // sanity checks
   ASSERT(CpuSchedAllCellsAreLocked());
   ASSERT(Sched_TreeIsLocked());
   ASSERT(CpuSchedIsValidNode(n));

   // initialize
   *nodeRun = 0;
   *nodeReady = 0;

   switch (n->nodeType) {
   case SCHED_NODE_TYPE_VM: {
      const CpuSched_Vsmp *vsmp = World_CpuSchedVsmp(n->u.world);
      if (!CpuSchedVsmpIsSystemIdle(vsmp)) {
         FORALL_VSMP_VCPUS(vsmp, vcpu) {
            CpuMetrics_LoadHistory *h = vcpu->loadHistory;
            if (h != NULL) {
               // compute ready time excluding max-limited limbo time
               Timer_Cycles totalReady = CpuSchedVcpuReadyTime(vcpu);
               Timer_Cycles totalLimbo = CpuSchedVcpuLimboTime(vcpu);
               Timer_Cycles nonLimboReady = 0;
               if (LIKELY(totalReady > totalLimbo)) {
                  nonLimboReady = totalReady - totalLimbo;
               }
               CpuMetrics_LoadHistorySampleCumulative(h,
                                                      vcpu->chargeCyclesTotal,
                                                      nonLimboReady,
                                                      &run,
                                                      &ready);
               *nodeRun += run;
               *nodeReady += ready;
            }
         } VSMP_VCPUS_DONE;
      }
      break;
   }

   case SCHED_NODE_TYPE_GROUP: {
      Sched_Group *group = n->u.group;
      CpuSched_GroupState *cpuGroup = &group->cpu;
      CpuMetrics_LoadHistory *h = cpuGroup->loadHistory;
      uint32 i;

      for (i = 0; i < group->members.len; i++) {
         const Sched_Node *m = group->members.list[i];
         if (CpuSchedIsValidNode(m)) {
            CpuSchedSampleLoadHistoryNodes(m, &run, &ready);
            *nodeRun += run;
            *nodeReady += ready;
         }
      }
      if (h != NULL) {
         CpuMetrics_LoadHistorySampleDelta(h, *nodeRun, *nodeReady);
      }
      break;
   }
      
   default:
      NOT_REACHED();
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_SampleLoadHistory --
 *
 *	Samples the incremental run and ready times for all nodes in
 *	the global scheduler tree since the last invocation.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      Updates load history state for all nodes in the global 
 *	scheduler tree.
 *
 *----------------------------------------------------------------------
 */
void
CpuSched_SampleLoadHistory(void)
{
   Timer_Cycles totalRun, totalReady;
   SP_IRQL prevIRQL;

   // invoke primitive holding appropriate locks
   prevIRQL = CpuSchedLockAllCells();
   Sched_TreeLock();
   CpuSchedSampleLoadHistoryNodes(Sched_TreeRootNode(),
                                  &totalRun,
                                  &totalReady);
   Sched_TreeUnlock();
   CpuSchedUnlockAllCells(prevIRQL);

   // debugging
   if (CPUSCHED_DEBUG_VERBOSE) {
      uint32 usecRun, usecReady;
      uint64 secRun, secReady;
      CpuSched_UsageToSec(totalRun, &secRun, &usecRun);
      Timer_TCToSec(totalReady, &secReady, &usecReady);
      LOG(0, "totals: run: %Lu.%03u, ready: %Lu.%03u",
          secRun, usecRun / 1000, secReady, usecReady / 1000);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedSetHTQuarantineActive --
 *
 *      Activates or deactives HT "quarantine" depending on value
 *      of "active."
 *
 *      Vsmp quarantining was motivated by the findings in
 *      "Microarchitectural Denial of Service: Insuring Microarchitectural
 "      Fairness," by Grunwald and Ghiasi. The paper pointed out that
 *      certain processor events, particularly machine clears generated
 *      by self-modifying code, can have a devastating impact on the execution
 *      of a thread running on another logical processor on the same package.
 *
 *      Our approach in vmkernel monitors the "machine_clear_any" event,
 *      using the standard vmkperf module, to track self-modifying code
 *      and memory ordering violations. If the moving average of machine
 *      clear events per million cycles exceeds the threshold specified
 *      by CONFIG_CPU_MACHINE_CLEAR_THRESH, the vsmp will be quarantined,
 *      that is, its htsharing will be changed to CPUSCHED_HT_SHARE_NONE,
 *      until this weighted moving average drops below the threshold.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Activates or deactivates quarantining.
 *
 *-----------------------------------------------------------------------------
 */
static void
CpuSchedSetHTQuarantineActive(Bool active)
{
   SP_IRQL prevIRQL;
   VMK_ReturnStatus res;
   struct VmkperfEventInfo *event;
   ASSERT(SMP_HTEnabled());

   prevIRQL = CpuSchedLockAllCells();

   if (cpuSched.htQuarantineActive != active) {
      event = cpuSchedConst.machineClearEvent;
      res = Vmkperf_SetEventActive(event, active);
      if (res != VMK_OK) {
         Log("unable to configure HT quarantining");
         CpuSchedUnlockAllCells(prevIRQL);
         return;
      }
   }

   if (active && !cpuSched.htQuarantineActive) {
      cpuSched.htQuarantineActive = TRUE;
      Log("beginning to track events for HT quarantine");
   } else if (!active && cpuSched.htQuarantineActive) {
      cpuSched.htQuarantineActive = FALSE;
      Log("HT quarantine tracking deactivated");
      // de-quarantine everybody and reset
      // all relevant stats
      FORALL_CELLS(c) {
         FORALL_CELL_VSMPS(c, vsmp) {
            vsmp->htQuarantine = FALSE;
            FORALL_VSMP_VCPUS(vsmp, v) {
               memset(&v->htEvents, 0, sizeof(CpuSched_HTEventCount));
            } VSMP_VCPUS_DONE;
         } CELL_VSMPS_DONE;
      } CELLS_DONE;
   }

   CpuSchedUnlockAllCells(prevIRQL);
}

/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedHTQuarantineCallback --
 *
 *      Handles changes to the CPU_MACHINE_CLEAR_THRESH config option.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May enable/disable HT quarantining.
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
CpuSchedHTQuarantineCallback(Bool write, Bool changed, int indx)
{
   VMK_ReturnStatus status = VMK_OK;
   if (write && changed) {
      CpuSchedSetHTQuarantineActive(
         CONFIG_OPTION(CPU_MACHINE_CLEAR_THRESH) > 0);

      // update config structure too
      status = CpuSched_UpdateConfig(write, changed, indx);
   }
   return status;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CpuSchedVsmpMaxHTConstraint --
 *
 *      Returns the strongest constraint (INTERNALLY or SHARE_NONE)
 *      that this vsmp could support, taking into account affinity
 *      and vsmp size.
 *
 * Results:
 *      Returns CPUSCHED_HT_SHARE_NONE or CPUSCHED_HT_SHARE_INTERNALLY
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
static Sched_HTSharing
CpuSchedVsmpMaxHTConstraint(const CpuSched_Vsmp *vsmp)
{
   Sched_HTSharing sharing = CPUSCHED_HT_SHARE_NONE;
   
   if (vsmp->vcpus.len < 2) {
      sharing = CPUSCHED_HT_SHARE_NONE;
   } else if (vsmp->affinityConstrained && !vsmp->jointAffinity) {
      // we're deleting disjoint affinity soon, so
      // don't try to do anything smart here
      sharing = CPUSCHED_HT_SHARE_INTERNALLY;
   } else {
      // in order to forbid sharing entirely, we must have affinity to
      // at least "numVcpus" packages (not pcpus!) in each cell
      FORALL_CELLS_UNLOCKED(c) {
         CpuMask mask;
         uint8 numPackages;
      
         // just look at affinity of vcpu0, because we know they're all the same
         mask = vsmp->vcpus.list[0]->affinityMask & c->pcpuMask;
         numPackages = CpuSched_NumAffinityPackages(mask);
         if (numPackages != 0 && numPackages < vsmp->vcpus.len) {
            // not enough room, so we'll need to share internally
            // to guarantee that we have room for coscheduling
            VSMPLOG(2,  vsmp, "insufficient room, share internally");
            sharing = CPUSCHED_HT_SHARE_INTERNALLY;
            break;
         }
      } CELLS_UNLOCKED_DONE;
   }

   if (vsmp->vcpus.len > 2 && sharing == CPUSCHED_HT_SHARE_INTERNALLY) {
      // internal sharing not supported for >2way vsmps
      sharing = CPUSCHED_HT_SHARE_ANY;
   }
   
   return (sharing);
}

/*
 * Grouping Operations
 */

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedNodeSnapUpdateRatio --
 *
 *	Updates the base:alloc share ratio for node snapshot "n".
 *
 * Results:
 *      Updates "n".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
CpuSchedNodeSnapUpdateRatio(CpuSchedNodeSnap *n)
{
   if (n->alloc.shares == 0) {
      n->baseRatio = CPUSCHED_BASE_RATIO_MAX;
   } else if (n->base.shares >= n->base.max) {
      n->baseRatio = CPUSCHED_BASE_RATIO_MAX;
   } else if (n->base.shares <= n->base.min) {
      n->baseRatio = CPUSCHED_BASE_RATIO_MIN;
   } else {
      n->baseRatio = ((uint64) n->base.shares) << CPUSCHED_BASE_RATIO_SHIFT;
      n->baseRatio /= n->alloc.shares;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedNodeSnapMinRatio --
 *
 *	Returns the child of "node" with the minimum base:alloc 
 *	share ratio that is still eligible to receive more 
 *	base shares, or NULL if none exists.
 *
 * Results:
 *      Returns eligible child of "node" with the minimum base:alloc
 *	ratio, or NULL if none exists.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static CpuSchedNodeSnap *
CpuSchedNodeSnapMinRatio(CpuSchedNodeSnap *node)
{
   CpuSchedNodeSnap *min = NULL;

   ASSERT(SCHED_NODE_IS_GROUP(node));
   if (SCHED_NODE_IS_GROUP(node)) {
      CpuSchedGroupNodeSnap *g = &node->u.group;

      // OPT: maintain list sorted by baseRatio
      FORALL_SNAP_GROUP_MEMBERS(g, n) {
         if (n->baseRatio < CPUSCHED_BASE_RATIO_MAX) {
            if ((min == NULL) || (n->baseRatio < min->baseRatio)) {
               min = n;
            }
         }
      } SNAP_GROUP_MEMBERS_DONE;
   }
   
   return(min);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedSnapshotNodes --
 *
 *	Updates snapshot "s" by traversing the allocation node tree
 *	rooted at node "n", and constructing a corresponding tree
 *	of snapshot nodes in "s".  Caller must hold all scheduler
 *	cell locks and the scheduler tree lock.
 *
 * Results:
 *      Updates "s" with snapshot of current scheduler
 *	allocation state.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static CpuSchedNodeSnap *
CpuSchedSnapshotNodes(CpuSchedReallocSnap *s,
                      const Sched_Node *n,
                      Bool groupEnforceMax)
{
   uint32 baseMin, baseMax, vsmpCount;
   CpuSchedNodeSnap *snapNode;

   // sanity checks
   ASSERT(CpuSchedAllCellsAreLocked());
   ASSERT(Sched_TreeIsLocked());
   ASSERT(CpuSchedIsValidNode(n));

   // assign snapshot node
   // n.b. rely on same traversal order for correspondence
   ASSERT(s->nNodes < SCHED_NODES_MAX);
   snapNode = &s->nodes[s->nNodes++];

   // initialize
   baseMin = 0;
   baseMax = 0;
   vsmpCount = 0;

   switch (n->nodeType) {
   case SCHED_NODE_TYPE_VM: {
      const CpuSched_Vsmp *vsmp = World_CpuSchedVsmp(n->u.world);
      CpuSchedVsmpNodeSnap *snapVsmp;
      uint32 baseLimit;
      
      // debugging
      if (CPUSCHED_DEBUG_VERBOSE) {
         LOG(0, "vsmp id=%u", VsmpLeaderID(vsmp));
      }

      // avoid infeasible alloc
      baseLimit = CPUSCHED_BASE_PER_PACKAGE * vsmp->vcpus.len;
      baseMin = MIN(CpuSchedUnitsToBaseShares(vsmp->alloc.min, vsmp->alloc.units),
                   baseLimit);
      if (CpuSchedEnforceMax(&vsmp->alloc)) {
         baseMax = MIN(CpuSchedUnitsToBaseShares(vsmp->alloc.max, vsmp->alloc.units),
                       baseLimit);
      } else {
         baseMax = baseLimit;
      }

      // special-case: dedicated idle world
      if (CpuSchedVsmpIsSystemIdle(vsmp)) {
         baseMax = 0;
      }

      // node covers single vsmp
      vsmpCount = 1;

      // snapshot state
      snapNode->nodeType = SCHED_NODE_TYPE_VM;
      snapNode->alloc = vsmp->alloc;
      snapVsmp = &snapNode->u.vsmp;
      snapVsmp->leaderID = VsmpLeaderID(vsmp);
      snapVsmp->nvcpus = vsmp->vcpus.len;
      snapVsmp->groupEnforceMax = groupEnforceMax;
      break;
   }

   case SCHED_NODE_TYPE_GROUP: {
      const Sched_Group *group = n->u.group;
      const CpuSched_GroupState *cpuGroup = &group->cpu;
      CpuSchedGroupNodeSnap *snapGroup;
      uint32 i, groupBaseMin, groupBaseMax;
      Bool enforceMax;

      // debugging
      if (CPUSCHED_DEBUG_VERBOSE) {
         LOG(0, "id=%u, name=%s", group->groupID, group->groupName);
      }

      // propagate group max enforcement to children
      enforceMax = groupEnforceMax || CpuSchedEnforceMax(&cpuGroup->alloc);

      // snapshot state
      snapNode->nodeType = SCHED_NODE_TYPE_GROUP;
      snapNode->alloc = cpuGroup->alloc;
      snapGroup = &snapNode->u.group;
      snapGroup->groupID = group->groupID;
      snapGroup->nMembers = 0;
      for (i = 0; i < group->members.len; i++) {
         const Sched_Node *m = group->members.list[i];
         if (CpuSchedIsValidNode(m)) {
            // recursive call results in depth-first traversal
            CpuSchedNodeSnap *mSnap = CpuSchedSnapshotNodes(s, m, enforceMax);
            ASSERT(snapGroup->nMembers < SCHED_GROUP_MEMBERS_MAX);
            snapGroup->members[snapGroup->nMembers++] = mSnap;
            baseMin += mSnap->base.min;
            baseMax += mSnap->base.max;
            vsmpCount += mSnap->vsmpCount;
         }
      }

      // enforce group max
      if (CpuSchedEnforceMax(&cpuGroup->alloc)) {
         groupBaseMax = CpuSchedUnitsToBaseShares(cpuGroup->alloc.max,
                                                  cpuGroup->alloc.units);
      } else {
         groupBaseMax = cpuSchedConst.baseShares;
      }
      baseMax = MIN(baseMax, groupBaseMax);

      // enforce group min
      groupBaseMin = CpuSchedUnitsToBaseShares(cpuGroup->alloc.min,
                                               cpuGroup->alloc.units);
      baseMin = MAX(baseMin, groupBaseMin);

      // enforce min <= max
      baseMin = MIN(baseMin, baseMax);
      break;
   }

   default:
      NOT_REACHED();
   }

   // count vsmps covered by node
   snapNode->vsmpCount = vsmpCount;

   // initialize base, update node base:alloc ratio
   ASSERT(baseMin <= baseMax);
   CpuSchedAllocInit(&snapNode->base, baseMin, baseMax, 
		     SCHED_UNITS_BSHARES, baseMin);
   CpuSchedNodeSnapUpdateRatio(snapNode);

   // debugging
   if (CPUSCHED_DEBUG_VERBOSE) {
      switch (snapNode->nodeType) {
      case SCHED_NODE_TYPE_VM:
         LOG(0,
             "vsmp snap: type=%u, id=%u, "
             "bmin=%u, bmax=%u, bshares=%u",
             snapNode->nodeType,
             snapNode->u.vsmp.leaderID,
             snapNode->base.min,
             snapNode->base.max,
             snapNode->base.shares);
         break;
      case SCHED_NODE_TYPE_GROUP:
         LOG(0,
             "group snap: type=%u, id=%u, "
             "bmin=%u, bmax=%u, bshares=%u",
             snapNode->nodeType,
             snapNode->u.group.groupID,
             snapNode->base.min,
             snapNode->base.max,
             snapNode->base.shares);
         break;
      default:
         NOT_REACHED();
      }
   }

   return(snapNode);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedReallocSnapshot --
 *
 *	Updates "s" with a snapshot of the current allocation
 *	state for all vsmps managed by the cpu scheduler.
 *	Caller must hold all scheduler cell locks and the
 *	scheduler tree lock.
 *
 * Results:
 *      Updates "s" with snapshot of current scheduler
 *	allocation state.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedReallocSnapshot(CpuSchedReallocSnap *s)
{
   // sanity checks
   ASSERT(CpuSchedAllCellsAreLocked());
   ASSERT(Sched_TreeIsLocked());

   // snapshot counts
   s->nVsmps = CpuSchedNumVsmps();
   s->nGroups = Sched_TreeGroupCount();

   // snapshot nodes
   s->nNodes = 0;
   s->nodeRoot = CpuSchedSnapshotNodes(s, Sched_TreeRootNode(), FALSE);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedSnapshotNodesConsistent --
 *
 *	Returns TRUE iff the allocation node tree snapshot rooted
 *	at "snapNode" is consistent with the current scheduler
 *	node tree rooted at "node".  Caller must hold all scheduler
 *	cell locks and the scheduler tree lock.
 *
 * Results:
 *      Returns TRUE if "s" is consistent with scheduler state.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static Bool
CpuSchedSnapshotNodesConsistent(const CpuSchedNodeSnap *snapNode,
                                const Sched_Node *node)
{
   // sanity check
   ASSERT(CpuSchedIsValidNode(node));

   // same node type?
   if (snapNode->nodeType != node->nodeType) {
      return(FALSE);
   }

   switch (snapNode->nodeType) {
   case SCHED_NODE_TYPE_VM: {
      const CpuSchedVsmpNodeSnap *snapVsmp = &snapNode->u.vsmp;
      const CpuSched_Vsmp *vsmp = World_CpuSchedVsmp(node->u.world);

      // same snapshot data?
      if ((snapVsmp->leaderID != VsmpLeaderID(vsmp)) ||
          (snapVsmp->nvcpus != vsmp->vcpus.len)) {
         return(FALSE);
      }

      // same external alloc?
      if (!CpuSchedAllocEqual(&snapNode->alloc, &vsmp->alloc)) {
         return(FALSE);
      }

      break;
   }

   case SCHED_NODE_TYPE_GROUP: {
      const CpuSchedGroupNodeSnap *snapGroup = &snapNode->u.group;
      const Sched_Group *group = node->u.group;
      uint32 i, iSnap;

      // same snapshot data?
      if (snapGroup->groupID != group->groupID) {
         return(FALSE);
      }

      // same external alloc?
      if (!CpuSchedAllocEqual(&snapNode->alloc, &group->cpu.alloc)) {
         return(FALSE);
      }

      // check consistency of valid member nodes recursively
      for (i = 0, iSnap = 0; i < group->members.len; i++) {
         const Sched_Node *m = group->members.list[i];
         ASSERT(iSnap <= snapGroup->nMembers);
         if (CpuSchedIsValidNode(m)) {
            const CpuSchedNodeSnap *mSnap = snapGroup->members[iSnap++];
            if (!CpuSchedSnapshotNodesConsistent(mSnap, m)) {
               return(FALSE);
            }
         }
      }

      break;
   }

   default:
      NOT_REACHED();
      return(FALSE);
   }

   // everything consistent
   return(TRUE);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedSnapshotConsistent --
 *
 *	Returns TRUE iff the allocation state snapshot in "s"
 *	is consistent with the current scheduler allocation state.
 *	Caller must hold all scheduler cell locks and the
 *	scheduler tree lock.
 *
 * Results:
 *      Returns TRUE if "s" is consistent with scheduler state.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static Bool
CpuSchedSnapshotConsistent(const CpuSchedReallocSnap *s)
{
   // sanity checks
   ASSERT(CpuSchedAllCellsAreLocked());
   ASSERT(Sched_TreeIsLocked());

   // quick checks: same aggregate counts?
   if (s->nVsmps != CpuSchedNumVsmps()) {
      return(FALSE);
   }
   if (s->nGroups != Sched_TreeGroupCount()) {
      return(FALSE);
   }

   // full check: all node data identical?
   if (!CpuSchedSnapshotNodesConsistent(s->nodeRoot, Sched_TreeRootNode())) {
      return(FALSE);
   }

   return(TRUE);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedSnapshotCommitNodes --
 *
 *	Makes updated allocations specified by the scheduler node
 *	tree snapshot rooted at "snapNode" effective for the scheduler
 *	node tree rooted at "node".  Caller must hold all scheduler
 *	cell locks and the scheduler tree lock.
 *
 * Results:
 *      Updates state for scheduler tree rooted at "node".
 *
 * Side effects:
 *      Modifies scheduler state.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedSnapshotCommitNodes(const CpuSchedNodeSnap *snapNode,
                            Sched_Node *node)
{
   // sanity checks
   ASSERT(snapNode != NULL);
   ASSERT(snapNode->nodeType == node->nodeType);
   ASSERT(snapNode->base.min <= snapNode->base.max);
   ASSERT(snapNode->base.shares <= snapNode->base.max);

   switch (snapNode->nodeType) {
   case SCHED_NODE_TYPE_VM: {
      const CpuSchedVsmpNodeSnap *snapVsmp = &snapNode->u.vsmp;
      CpuSched_Vsmp *vsmp = World_CpuSchedVsmp(node->u.world);

      // sanity check
      ASSERT(snapVsmp->leaderID == VsmpLeaderID(vsmp));

      // debugging
      if (CPUSCHED_DEBUG_VERBOSE) {
         VmLog(snapVsmp->leaderID,
               "old=%d, base: min=%u, max=%u, shares=%u, ratio=%Lu",
               vsmp->base.shares,
               snapNode->base.min,
               snapNode->base.max,
               snapNode->base.shares,
               snapNode->baseRatio);
      }

      // update base allocation
      CpuSchedVsmpSetBaseAlloc(vsmp, &snapNode->base);
      vsmp->groupEnforceMax = snapVsmp->groupEnforceMax;
      break;
   }

   case SCHED_NODE_TYPE_GROUP: {
      const CpuSchedGroupNodeSnap *snapGroup = &snapNode->u.group;
      Sched_Group *group = node->u.group;
      CpuSched_GroupState *cpuGroup = &group->cpu;
      uint32 i, iSnap;

      // sanity check
      ASSERT(snapGroup->groupID == group->groupID);

      // debugging
      if (CPUSCHED_DEBUG_VERBOSE) {
         Log("group %s: old=%d, base: min=%u, max=%u, shares=%u, ratio=%Lu",
             group->groupName,
             cpuGroup->base.shares,
             snapNode->base.min,
             snapNode->base.max,
             snapNode->base.shares,
             snapNode->baseRatio);
      }

      // update base allocation, vsmp count
      CpuSchedGroupSetBaseAlloc(cpuGroup,
                                &snapNode->base,
                                snapNode->vsmpCount);

      // update valid member nodes recursively
      for (i = 0, iSnap = 0; i < group->members.len; i++) {
         Sched_Node *m = group->members.list[i];
         ASSERT(iSnap <= snapGroup->nMembers);
         if (CpuSchedIsValidNode(m)) {
            const CpuSchedNodeSnap *mSnap = snapGroup->members[iSnap++];
            CpuSchedSnapshotCommitNodes(mSnap, m);            
         }
      }

      break;
   }

   default:
      NOT_REACHED();
   }
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedSnapshotCommit --
 *
 *	Makes updated allocations specified by "s" effective
 *	for all vsmps.  Caller must hold all scheduler cell locks
 *	and the scheduler tree lock.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Modifies scheduler state.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedSnapshotCommit(const CpuSchedReallocSnap *s)
{
   // sanity checks
   ASSERT(CpuSchedAllCellsAreLocked());
   ASSERT(Sched_TreeIsLocked());

   // update shares for all nodes
   CpuSchedSnapshotCommitNodes(s->nodeRoot, Sched_TreeRootNode());

   // invalidate all preemption vtimes
   FORALL_CELLS(c) {
      CpuSchedCellPreemptionInvalidate(c);
   } CELLS_DONE;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedBalanceNodes --
 *
 *	Recomputes internal base share allocations for all nodes in
 *	the tree rooted at "node", based on external allocation
 *	parameters.  Note that the current implementation is somewhat
 *	inefficient, but numerous optimizations are possible.
 *
 * Results:
 *      Updates "node".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedBalanceNodes(CpuSchedNodeSnap *node)
{
   uint32 totalBase, reservedBase, simpleBase, totalAlloc, totalMin;
   CpuSchedGroupNodeSnap *group;

   // sanity check
   ASSERT(node != NULL);

   // done if leaf node (vsmp)
   if (SCHED_NODE_IS_VM(node)) {
      if (CPUSCHED_DEBUG_VERBOSE) {
         LOG(0, "vsmp %u: bshares=%u",
             node->u.vsmp.leaderID,
             node->base.shares);
      }
      return;
   }

   ASSERT(SCHED_NODE_IS_GROUP(node));
   group = &node->u.group;

   // initialize 
   totalBase = node->base.shares;
   reservedBase = 0;
   
   if (CPUSCHED_DEBUG_VERBOSE) {
      LOG(0, "group %u: bshares=%u", group->groupID, totalBase);
   }

   // compute sum of member alloc shares, min base shares
   totalAlloc = 0;
   totalMin = 0;
   FORALL_SNAP_GROUP_MEMBERS(group, n) {
      totalAlloc += n->alloc.shares;
      totalMin += n->base.min;
   } SNAP_GROUP_MEMBERS_DONE;

   // compute non-min base shares (for lower bound #2 below)
   ASSERT(totalMin <= totalBase);
   simpleBase = totalBase - totalMin;

   // initialize base shares to lower bound
   FORALL_SNAP_GROUP_MEMBERS(group, n) {
      // lower bound #1: guaranteed min computed during snapshot      
      n->base.shares = n->base.min;

      // lower bound #2: simple "alloc fraction" of non-min base shares
      if (totalAlloc > 0) {
         uint64 simpleNumer = ((uint64) simpleBase * (uint64) n->alloc.shares);
         uint32 simpleShare = simpleNumer / totalAlloc;
         simpleShare = MIN(simpleShare, n->base.max);
         n->base.shares = MAX(n->base.shares, simpleShare);
      }

      reservedBase += n->base.shares;
      CpuSchedNodeSnapUpdateRatio(n);

      // debugging
      if (CPUSCHED_DEBUG_VERBOSE) {
         LOG(0, "group %u: member: min=%u, bshares=%u",
             group->groupID,
             n->base.min,
             n->base.shares);
      }
   } SNAP_GROUP_MEMBERS_DONE;

   ASSERT(reservedBase <= totalBase);

   // OPT: algorithm below is simple but slow
   // parcel out excess capacity in chunks
   // start w/ larger chunks, make progressively smaller
   if (reservedBase < totalBase) {
      uint32 chunkLarge, chunkMedium, chunkSmall, chunkTiny, chunkOnePct;
      uint32 remainBase, remainMedium, remainSmall, remainTiny;

      // initialize remaining
      remainBase = totalBase - reservedBase;
      if (CPUSCHED_DEBUG_VERBOSE) {
         LOG(0, "totalBase: %u, reservedBase: %u, remainBase: %u",
             totalBase, reservedBase, remainBase);
      }

      // initialize remain thresholds: 100%, 50%, 25%, 2%
      remainMedium = totalBase / 2;
      remainSmall  = totalBase / 4;
      remainTiny   = totalBase / 50;

      // initialize corresponding chunk sizes: 1%, 0.5%, 0.25%, 0.05%
      // OPT: improve number of chunks, chunk sizes
      chunkOnePct = CpuSchedUnitsToBaseShares(1, SCHED_UNITS_PERCENT);
      chunkLarge  = chunkOnePct;
      chunkMedium = MAX(chunkOnePct / 2, 1);
      chunkSmall  = MAX(chunkOnePct / 4, 1);
      chunkTiny   = MAX(chunkOnePct / 20, 1);
      if (CPUSCHED_DEBUG_VERBOSE) {
         LOG(0, "chunks: large=%u, med=%u, small=%u, tiny=%u",
             chunkLarge, chunkMedium, chunkSmall, chunkTiny);
      }

      // try to distribute remaining base shares
      while (remainBase > 0) {
         uint32 deltaBase, deltaMax;
         CpuSchedNodeSnap *min;
         uint32 chunk;

         // choose node most eligible for more shares
         min = CpuSchedNodeSnapMinRatio(node);
         if (min == NULL) {
            break;
         }
         
         // parcel out base shares in chunks
         if (remainBase > remainMedium) {
            chunk = chunkLarge;
         } else if (remainBase > remainSmall) {
            chunk = chunkMedium;
         } else if (remainBase > remainTiny) {
            chunk = chunkSmall;
         } else {
            chunk = chunkTiny;
         }
         deltaBase = MIN(chunk, remainBase);

         // avoid exceeding client max
         ASSERT(min->base.max > min->base.shares);
         deltaMax = min->base.max - min->base.shares;
         deltaBase = MIN(deltaBase, deltaMax);

         // update client, remaining shares
         min->base.shares += deltaBase;
         CpuSchedNodeSnapUpdateRatio(min);                         
         remainBase -= deltaBase;
      }
   }

   // balance children recursively
   FORALL_SNAP_GROUP_MEMBERS(group, n) {
      CpuSchedBalanceNodes(n);
   } SNAP_GROUP_MEMBERS_DONE;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedBalance --
 *
 *	Recomputes internal base share allocations for all vsmps
 *	in "s", based on external allocation parameters.
 *
 * Results:
 *      Updates "s".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedBalance(CpuSchedReallocSnap *s)
{
   CpuSchedNodeSnap *root = s->nodeRoot;
   uint32 totalBase = cpuSchedConst.baseShares;

   // enforce root max, if any
   if (CpuSchedEnforceMax(&root->alloc)) {
      totalBase = MIN(totalBase, CpuSchedUnitsToBaseShares(root->alloc.max, root->alloc.units));
   }
   CpuSchedAllocInit(&root->base, totalBase, totalBase, 
		     SCHED_UNITS_BSHARES, totalBase);

   CpuSchedBalanceNodes(s->nodeRoot);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_RequestReallocate --
 *
 *	Indicates that a reallocation operation is needed to reflect
 *	changes in vsmp allocations managed by the cpu scheduler.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      Modifies scheduler allocation state.
 *
 *----------------------------------------------------------------------
 */
void
CpuSched_RequestReallocate(void)
{
   cpuSched.reallocNeeded = TRUE;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedReallocateInt --
 *
 *	Reallocates base shares among vsmps managed by cpu scheduler.
 *	Optimistically assumes that external allocations remain the
 *	same during rebalancing; fails with VMK_BUSY if this assumption
 *	is violated.
 *
 * Results:
 *	Returns VMK_OK if successful, otherwise error code.
 *	Modifies allocation and scheduler state.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
CpuSchedReallocateInt(void)
{
   VMK_ReturnStatus status;
   CpuSchedReallocSnap *s;
   CpuSchedOpStats *stats;
   SP_IRQL prevIRQL;

   // convenient abbrevs
   s = &cpuSched.reallocSnap;
   stats = &cpuSched.reallocStats;

   // snapshot current alloc state
   prevIRQL = CpuSchedLockAllCells();
   Sched_TreeLock();
   if (cpuSched.reallocInProgress) {
      Sched_TreeUnlock();
      CpuSchedUnlockAllCells(prevIRQL);
      return(VMK_BUSY);
   }
   cpuSched.reallocInProgress = TRUE;
   CpuSchedReallocSnapshot(s);
   if (CPUSCHED_DEBUG) {
      ASSERT(CpuSchedSnapshotConsistent(s));
   }
   Sched_TreeUnlock();
   CpuSchedUnlockAllCells(prevIRQL);

   // rebalance base shares w/o holding locks, track cost
   CpuSchedOpStatsStart(stats);
   CpuSchedBalance(s);
   CpuSchedOpStatsStop(stats);

   // commit changes if snapshot still valid
   prevIRQL = CpuSchedLockAllCells();
   Sched_TreeLock();
   if (CpuSchedSnapshotConsistent(s)) {
      CpuSchedSnapshotCommit(s);
      cpuSched.reallocNeeded = FALSE;
      status = VMK_OK;
   } else {
      stats->failCount++;
      status = VMK_BUSY;
   }
   cpuSched.reallocInProgress = FALSE;
   Sched_TreeUnlock();
   CpuSchedUnlockAllCells(prevIRQL);
   return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_Reallocate --
 *
 *	Reallocates base shares among vsmps managed by cpu scheduler.
 *
 * Results:
 *	Returns VMK_OK if successful, otherwise error code.
 *	Modifies allocation and scheduler state.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
CpuSched_Reallocate(void)
{
   VMK_ReturnStatus status;

   // avoid unnecessary work
   if (!cpuSched.reallocNeeded) {
      return(VMK_OK);
   }

   // invoke primitive
   status = CpuSchedReallocateInt();

   // debugging
   if (CPUSCHED_DEBUG) {
      const CpuSchedOpStats *stats = &cpuSched.reallocStats;
      Log("%s: total=%u, failed=%u: balance last=%Lu usec, avg=%Lu usec",
          VMK_ReturnStatusToString(status),
          stats->totalCount,
          stats->failCount,
          Timer_TCToUS(stats->cycles),
          Timer_TCToUS(CpuSchedOpStatsAvg(stats)));
   }

   return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedNodeReservedMin --
 *
 *	Sets "reserved" and "unreserved" to the amounts of guaranteed
 *	min capacity associated with group "node" that are currently
 *	reserved and unreserved, respectively, measured in base shares.
 *
 * Results:
 *	Sets "reserved" and "unreserved" as described above.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedNodeReservedMin(const Sched_Node *node,
                        uint32 *reserved,
                        uint32 *unreserved)
{
   uint32 usedMin, unusedMin;

   // sanity checks
   ASSERT(Sched_TreeIsLocked());
   ASSERT(node != NULL);
   ASSERT(SCHED_NODE_IS_GROUP(node));

   // initialize
   usedMin = 0;
   unusedMin = 0;

   if (SCHED_NODE_IS_GROUP(node)) {
      const Sched_Group *group = node->u.group;
      uint32 totalMin, i;

      // compute min already reserved
      for (i = 0; i < group->members.len; i++) {
         const Sched_Node *member = group->members.list[i];

         // skip invalid (e.g. unadmitted) nodes
         if (!CpuSchedIsValidNode(member)) {
            continue;
         }

         switch (member->nodeType) {
         case SCHED_NODE_TYPE_VM: {
            const CpuSched_Vsmp *vsmp = World_CpuSchedVsmp(member->u.world);
            usedMin += CpuSchedUnitsToBaseShares(vsmp->alloc.min, vsmp->alloc.units);
            break;
         }
         case SCHED_NODE_TYPE_GROUP: {
            const CpuSched_GroupState *cpuGroup = &member->u.group->cpu;
            usedMin += CpuSchedUnitsToBaseShares(cpuGroup->alloc.min, cpuGroup->alloc.units);
            break;
         }
         default:
            NOT_REACHED();
         }
      }

      // compute min available
      totalMin = CpuSchedUnitsToBaseShares(group->cpu.alloc.min, group->cpu.alloc.units);
      ASSERT(totalMin >= usedMin);
      if (totalMin >= usedMin) {
         unusedMin = totalMin - usedMin;
      }
   }

   // set reply values
   *reserved = usedMin;
   *unreserved = unusedMin;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedGroupSetAllocInt --
 *
 *	Sets the external allocation parameters for "group" to "alloc".
 *	Caller must hold scheduler tree lock.
 *
 * Results:
 *	Modifies "group" scheduler state.
 *
 * Side effects:
 *      Modifies scheduler state.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedGroupSetAllocInt(Sched_Group *group, const CpuSched_Alloc *alloc)
{
   CpuSched_GroupState *cpuGroup = &group->cpu;
   uint32 shares;

   // sanity checks
   ASSERT(Sched_TreeIsLocked());
   ASSERT((alloc->max == CPUSCHED_ALLOC_MAX_NONE) ||
          (alloc->max >= alloc->min));
   
   // ensure shares within valid range
   shares = alloc->shares;
   shares = MAX(shares, CPUSCHED_SHARES_MIN);
   shares = MIN(shares, CPUSCHED_SHARES_MAX);

   // update external allocation
   cpuGroup->alloc = *alloc;
   cpuGroup->alloc.shares = shares;

   CpuSched_RequestReallocate();
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedGroupAllocAllowed --
 *
 *	Returns TRUE if it is permissible to set the external
 *	allocation parameters for "group" to "alloc".
 *	Caller must hold scheduler tree lock.
 *
 * Results:
 *	Modifies "group" scheduler state.
 *
 * Side effects:
 *      Modifies scheduler state.
 *
 *----------------------------------------------------------------------
 */
static Bool
CpuSchedGroupAllocAllowed(const Sched_Group *group,
                          const CpuSched_Alloc *alloc)
{
   const CpuSched_Alloc *oldAlloc = &group->cpu.alloc;
   uint32 usedMinBase, unusedMinBase, oldMinBase, newMinBase, newMaxBase;

   // sanity check
   ASSERT(Sched_TreeIsLocked());

   // fail if min exceeds max
   if (CpuSchedEnforceMax(alloc) && (alloc->min > alloc->max)) {
      Warning("group '%s': invalid min=%u > max=%u",
              group->groupName, alloc->min, alloc->max);
      return(FALSE);
   }

   // fail if shares outside valid range
   if ((alloc->shares < CPUSCHED_SHARES_MIN) ||
       (alloc->shares > CPUSCHED_SHARES_MAX)) {
      Warning("group '%s': invalid shares=%u",
              group->groupName, alloc->shares);
      return(FALSE);
   }

   // convert to common units
   oldMinBase = CpuSchedUnitsToBaseShares(oldAlloc->min, oldAlloc->units);
   newMinBase = CpuSchedUnitsToBaseShares(alloc->min, alloc->units);
   newMaxBase = CpuSchedUnitsToBaseShares(alloc->max, alloc->units);

   // lookup reservations allocated to children
   CpuSchedNodeReservedMin(group->node, &usedMinBase, &unusedMinBase);
   if (CPUSCHED_DEBUG_REPARENT) {
      LOG(0, "group '%s': members: base min reserved=%u, unreserved=%u",
          group->groupName, usedMinBase, unusedMinBase);
   }
   
   // perform admission control check on min
   if (newMinBase < usedMinBase) {
      Warning("group '%s': invalid min %u %s: "
              "members already reserved min=%u",
              group->groupName,
              alloc->min,
              Sched_UnitsToString(alloc->units),
              CpuSchedBaseSharesToUnits(usedMinBase, alloc->units));
      return(FALSE);
   }
   
   // perform admission control check on max
   if (CpuSchedEnforceMax(alloc) && (newMaxBase < usedMinBase)) {
      Warning("group '%s': invalid max %u %s: "
              "members already reserved min=%u",
              group->groupName,
              alloc->max,
              Sched_UnitsToString(alloc->units),
              CpuSchedBaseSharesToUnits(usedMinBase, alloc->units));
      return(FALSE);
   }

   // special case: no parent checks when modifying root
   if (group->node == Sched_TreeRootNode()) {
      ASSERT(group->node->parent == NULL);
      return(TRUE);
   }

   // lookup reservations allocated to parent
   CpuSchedNodeReservedMin(group->node->parent, &usedMinBase, &unusedMinBase);
   if (CPUSCHED_DEBUG_REPARENT) {
      LOG(0, "group '%s': parent: base min reserved=%u, unreserved=%u",
          group->groupName, usedMinBase, unusedMinBase);
   }

   // perform admission control check if increasing min
   if (newMinBase > oldMinBase) {
      uint32 needMinBase = newMinBase - oldMinBase;
      if (needMinBase > unusedMinBase) {
         Warning("group '%s': invalid min %u %s: "
                 "parent min reserved=%u, unreserved=%u, need=%u",
                 group->groupName,
                 alloc->min,
                 Sched_UnitsToString(alloc->units),
                 CpuSchedBaseSharesToUnits(usedMinBase, alloc->units),
                 CpuSchedBaseSharesToUnits(unusedMinBase, alloc->units),
                 CpuSchedBaseSharesToUnits(needMinBase, alloc->units));
         return(FALSE);
      }
   }

   return(TRUE);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_GroupSetAllocLocked --
 *
 *	Sets the external allocation parameters for "group" to "alloc". 
 *	Implements functionality for CpuSched_GroupSetAlloc() and may 
 *	also be directly invoked by callers residing within the scheduler 
 *	module. Caller must hold scheduler tree lock.
 *
 * Results:
 *	Returns VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 *      Modifies scheduler state associated with "group."
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
CpuSched_GroupSetAllocLocked(Sched_Group *group, const Sched_Alloc *alloc)
{
   CpuSched_Alloc cpuAlloc;

   ASSERT(Sched_TreeIsLocked());
   ASSERT(SCHED_NODE_IS_GROUP(group->node));

   CpuSchedAllocInit(&cpuAlloc, alloc->min, alloc->max, 
                     alloc->units, alloc->shares);

   // Check if new allocations are permissible
   if (!CpuSchedGroupAllocAllowed(group, &cpuAlloc)) {
      return(VMK_BAD_PARAM);
   }

   // Assign new allocations to group
   CpuSchedGroupSetAllocInt(group, &cpuAlloc);

   return (VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_GroupSetAlloc --
 *
 *	Sets the external allocation parameters for the group
 *	identified by "id" to "alloc".
 *
 * Results:
 *	Returns VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 *	Modifies scheduler state associated with group "id".
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
CpuSched_GroupSetAlloc(Sched_GroupID id, const Sched_Alloc *alloc)
{
   Sched_Group *group;
   VMK_ReturnStatus status;

   Sched_TreeLock();

   group = Sched_TreeLookupGroup(id);
   if (group == NULL) {
      status = VMK_NOT_FOUND;
   } else {
      status = CpuSched_GroupSetAllocLocked(group, alloc);
   }

   Sched_TreeUnlock();

   return (status);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_AdmitGroup --
 * 	Performs the necessary cpu resource related admission 
 * 	control checks when adding a scheduler group under a specified 
 * 	parent scheduler group. Caller must hold scheduler tree lock.
 *
 * Results: 
 * 	Returns VMK_OK if admission check passed, otherwise error code.
 *
 * Side effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
CpuSched_AdmitGroup(const Sched_Group *group,
		    const Sched_Group *newParentGroup)
{
   const CpuSched_Alloc *alloc;
   uint32 groupMinBase, newParentUsedMinBase, newParentUnusedMinBase;

   ASSERT(Sched_TreeIsLocked());
   ASSERT(SCHED_NODE_IS_GROUP(group->node));
   ASSERT(SCHED_NODE_IS_GROUP(newParentGroup->node));

   /*
    * Perform admission control checks to see if group can be
    * added under specified parent group.
    */
   alloc = &group->cpu.alloc;
   groupMinBase = CpuSchedUnitsToBaseShares(alloc->min, alloc->units);

   CpuSchedNodeReservedMin(newParentGroup->node, 
		           &newParentUsedMinBase, 
			   &newParentUnusedMinBase);

   if (groupMinBase > newParentUnusedMinBase) {
      return (VMK_CPU_ADMIT_FAILED);
   }

   return (VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_GroupStateInit -- 
 *
 *	Initialize scheduler group state "s" with zero min/max.
 *
 * Results:
 *	Initialize scheduler group state "s".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
CpuSched_GroupStateInit(CpuSched_GroupState *s)
{
   memset(s, 0, sizeof(*s));
   // check constant: units/min/max must be initialized
   ASSERT(s->alloc.units == SCHED_UNITS_PERCENT);
   ASSERT(s->alloc.min == 0);
   ASSERT(s->alloc.max == CPUSCHED_ALLOC_MAX_NONE);
   s->loadHistory = CpuMetrics_LoadHistoryNew();
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_GroupStateCleanup -- 
 *
 *	Cleanup scheduler group state "s", reclaiming any heap memory.
 *
 * Results:
 *	Cleanup scheduler group state "s", reclaiming any heap memory.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
CpuSched_GroupStateCleanup(CpuSched_GroupState *s)
{
   CpuMetrics_LoadHistoryDelete(s->loadHistory);
   s->loadHistory = NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_GroupChanged -- 
 *
 *	Updates scheduler state associated with "world" to
 *	be consistent with respect to its current group
 *	membership.
 *
 * Results:
 *	Modifies scheduler state associated with "world".
 *
 * Side effects:
 *      Requests reallocation.
 *
 *----------------------------------------------------------------------
 */
void
CpuSched_GroupChanged(World_Handle *world)
{
   CpuSched_Vsmp *vsmp = World_CpuSchedVsmp(world);
   SP_IRQL prevIRQL;

   // validate vsmp group
   prevIRQL = CpuSchedVsmpCellLock(vsmp);
   CpuSchedVsmpUpdateGroup(vsmp);
   CpuSchedVsmpCellUnlock(vsmp, prevIRQL);

   // debugging
   if (CPUSCHED_DEBUG) {
      VSMPLOG(0, vsmp, "updated group");
   }

   // request reallocation
   CpuSched_RequestReallocate();
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_MoveVmAllocToGroup --
 *
 *	Sets allocation for group "id" to be identical to allocation
 *	for VM associated with "world".  Removes guaranteed min
 *	allocation for VM associated with "world".
 *
 * Results:
 *	Returns VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 *      Modifies scheduler state.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
CpuSched_MoveVmAllocToGroup(World_Handle *world, Sched_GroupID id)
{
   CpuSched_Alloc origAlloc;
   CpuSched_Vsmp *vsmp;
   Sched_Group *group;
   SP_IRQL prevIRQL;   

   prevIRQL = CpuSchedLockAllCells();
   Sched_TreeLock();   

   // lookup group, fail if not found
   group = Sched_TreeLookupGroup(id);
   if (group == NULL) {
      Sched_TreeUnlock();
      CpuSchedUnlockAllCells(prevIRQL);
      return(VMK_NOT_FOUND);
   }

   // remove guaranteed alloc (min) from vsmp
   vsmp = World_CpuSchedVsmp(world);
   origAlloc = vsmp->alloc;
   vsmp->alloc.min = 0;

   // admission control check, restore original alloc if fails
   if (!CpuSchedGroupAllocAllowed(group, &origAlloc)) {
      vsmp->alloc.min = origAlloc.min;
      ASSERT(CpuSchedAllocEqual(&vsmp->alloc, &origAlloc));
      Sched_TreeUnlock();
      CpuSchedUnlockAllCells(prevIRQL);
      return(VMK_NO_RESOURCES);
   }

   // set group alloc identical to original vsmp alloc
   CpuSchedGroupSetAllocInt(group, &origAlloc);

   Sched_TreeUnlock();
   CpuSchedUnlockAllCells(prevIRQL);

   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_MoveGroupAllocToVm --
 *
 *	Sets allocation for VM associated with "world" to be identical
 *	to allocation for group "id".  Removes guaranteed min
 *	allocation for group "id".  Fails if group "id" has
 *	any members.
 *
 * Results:
 *	Returns VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 *      Modifies scheduler state.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
CpuSched_MoveGroupAllocToVm(Sched_GroupID id, World_Handle *world)
{
   CpuSched_GroupState *cpuGroup;
   CpuSched_Alloc origAlloc;
   CpuSched_Vsmp *vsmp;
   Sched_Group *group;
   SP_IRQL prevIRQL;   

   prevIRQL = CpuSchedLockAllCells();
   Sched_TreeLock();   

   // lookup group, fail if not found
   group = Sched_TreeLookupGroup(id);
   if (group == NULL) {
      Sched_TreeUnlock();
      CpuSchedUnlockAllCells(prevIRQL);
      return(VMK_NOT_FOUND);
   }

   // fail if group has any members
   if (group->members.len > 0) {
      Sched_TreeUnlock();
      CpuSchedUnlockAllCells(prevIRQL);
      return(VMK_BAD_PARAM);
   }

   // convenient abbrevs
   cpuGroup = &group->cpu;
   vsmp = World_CpuSchedVsmp(world);

   // remove guaranteed alloc (min) from group
   origAlloc = cpuGroup->alloc;
   cpuGroup->alloc.min = 0;

   // admission control check, restore original alloc if fails
   if (!CpuSchedVsmpAllocAllowed(vsmp, &origAlloc, vsmp->vcpus.len)) {
      cpuGroup->alloc.min = origAlloc.min;
      ASSERT(CpuSchedAllocEqual(&cpuGroup->alloc, &origAlloc));
      Sched_TreeUnlock();
      CpuSchedUnlockAllCells(prevIRQL);
      return(VMK_NO_RESOURCES);
   }

   // set vsmp alloc identical to original group alloc
   CpuSchedVsmpSetAllocInt(vsmp, &origAlloc);

   Sched_TreeUnlock();
   CpuSchedUnlockAllCells(prevIRQL);

   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedAgeGroupVtimes --
 *
 *	Callback function used by CpuSchedAgeAllGroupVtimes().
 *	Reduces the distance of the virtual time associated with
 *	group "g" from the virtual time specified by "data",
 *	by a multiplicative aging factor.  Caller must hold
 *	scheduler tree lock.
 *
 * Results:
 *      Modifies virtual times associated with "g".
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedAgeGroupVtimes(Sched_Group *g, void *data)
{
   CpuSchedVtime vtNow = *((CpuSchedVtime *) data);
   CpuSched_GroupState *cpuGroup = &g->cpu;
   CpuSchedVtime vtGroup = cpuGroup->vtime;
   CpuSchedVtime vtLimit = cpuGroup->vtimeLimit;

   // sanity check
   ASSERT(Sched_TreeIsLocked());

   CpuSchedAgeVtime(vtNow, &vtGroup);
   cpuGroup->vtimeAged += (vtGroup - cpuGroup->vtime);
   if (CpuSchedEnforceMax(&cpuGroup->alloc)) {
      CpuSchedAgeVtime(vtNow, &vtLimit);
   }

   // versioned atomic update protected by scheduler tree lock
   CPUSCHED_VERSIONED_ATOMIC_UPDATE_BEGIN(&cpuGroup->vtimeVersion);
   cpuGroup->vtime = vtGroup;
   cpuGroup->vtimeLimit = vtLimit;
   CPUSCHED_VERSIONED_ATOMIC_UPDATE_END(&cpuGroup->vtimeVersion);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedAgeAllGroupVtimes --
 *
 *	Reduces the distance from the global virtual time associated
 *	with all scheduler groups by a multiplicative aging factor
 *	to prevent monopolization of extra time consumption.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Modifies scheduler group state.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedAgeAllGroupVtimes(CpuSchedVtime vtNow)
{
   Sched_ForAllGroupsDo(CpuSchedAgeGroupVtimes, &vtNow);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedResetGroupVtimes --
 *
 *	Callback function used by CpuSchedResetAllGroupVtimes().
 *	Updates virtual times associated with scheduler group "g"
 *	to reflect global virtual time reset.  Caller must hold
 *	scheduler tree lock.
 *
 * Results:
 *      Modifies virtual times associated with "g".
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedResetGroupVtimes(Sched_Group *g, void *ignore)
{
   CpuSched_GroupState *cpuGroup = &g->cpu;
   CpuSchedVtime vtGroup = cpuGroup->vtime;
   CpuSchedVtime vtLimit = cpuGroup->vtimeLimit;

   // sanity check
   ASSERT(Sched_TreeIsLocked());

   CpuSchedVtimeResetAdjust(&vtGroup);
   CpuSchedVtimeResetAdjust(&vtLimit);

   // versioned atomic update protected by scheduler tree lock
   CPUSCHED_VERSIONED_ATOMIC_UPDATE_BEGIN(&cpuGroup->vtimeVersion);
   cpuGroup->vtime = vtGroup;
   cpuGroup->vtimeLimit = vtLimit;
   CPUSCHED_VERSIONED_ATOMIC_UPDATE_END(&cpuGroup->vtimeVersion);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedResetAllGroupVtimes --
 *
 *	Updates virtual times associated with all scheduler groups
 *	to reflect global virtual time reset.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Modifies scheduler group state.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedResetAllGroupVtimes(void)
{
   Sched_ForAllGroupsDo(CpuSchedResetGroupVtimes, NULL);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedVsmpGroupCharge --
 *
 *	Charges all enclosing scheduler groups associated with
 *	"vsmp" for the consumption of "cycles", updating their
 *	virtual times and other statistics appropriately.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      Modifies scheduler state.
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedVsmpGroupCharge(const CpuSched_Vsmp *vsmp, Timer_Cycles cycles)
{
   Sched_Node *node;

   // sanity checks
   ASSERT(vsmp != NULL);
   ASSERT(CpuSchedVsmpCellIsLocked(vsmp));

   Sched_TreeLock();

   // lookup vsmp node, group node
   node = CpuSchedVsmpNode(vsmp);
   if (node == NULL) {
      // possible if vsmp terminating
      Sched_TreeUnlock();
      VsmpLog(vsmp, "no group: skip");
      return;
   }

   ASSERT(node != NULL);
   ASSERT(SCHED_NODE_IS_VM(node));
   node = node->parent;

   // charge by updating vtime along leaf-to-root path
   while (node != NULL) {
      CpuSchedVtime vtGroup, vtLimit;
      CpuSched_GroupState *cpuGroup;
      Sched_Group *group;

      // sanity check
      ASSERT(SCHED_NODE_IS_GROUP(node));

      // convenient abbrevs
      group = node->u.group;
      cpuGroup = &group->cpu;
      vtGroup = cpuGroup->vtime;
      vtLimit = cpuGroup->vtimeLimit;

      // compute new vtimes
      cpuGroup->chargeCyclesTotal += cycles;
      vtGroup += CpuSchedTCToVtime(cpuGroup->stride, cycles);
      if (CpuSchedEnforceMax(&cpuGroup->alloc)) {
         vtLimit += CpuSchedTCToVtime(cpuGroup->strideLimit, cycles);
      }

      // versioned atomic update protected by scheduler tree lock
      CPUSCHED_VERSIONED_ATOMIC_UPDATE_BEGIN(&cpuGroup->vtimeVersion);
      cpuGroup->vtime = vtGroup;
      cpuGroup->vtimeLimit = vtLimit;
      CPUSCHED_VERSIONED_ATOMIC_UPDATE_END(&cpuGroup->vtimeVersion);

      node = node->parent;
   }

   Sched_TreeUnlock();
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedGroupSnapshot --
 *
 *	Callback function used by CpuSched_ProcGroupsRead().
 *	Snapshots current state of group "g" into CpuSched
 *	snapshot area, and increments counter specified by
 *	the "data" parameter.
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
CpuSchedGroupSnapshot(Sched_Group *g, void *data)
{
   Sched_Group *parent = Sched_TreeGroupParent(g);
   CpuSched_GroupState *c = &g->cpu;
   uint32 *snapCount = (uint32 *) data;
   CpuSchedGroupSnap *s;

   // find slot for snapshot
   s = &cpuSched.procSnap.group[*snapCount];

   // snapshot identity
   s->groupID = g->groupID;
   strncpy(s->groupName, g->groupName, SCHED_GROUP_NAME_LEN);

   // snapshot parent identity
   if (parent == NULL) {
      s->parentID = 0;
      strncpy(s->parentName, "none", SCHED_GROUP_NAME_LEN);
   } else {
      s->parentID = parent->groupID;
      strncpy(s->parentName, parent->groupName, SCHED_GROUP_NAME_LEN);
   }

   // snapshot group state
   s->members = g->members.len;
   s->state = *c;

   // update count
   (*snapCount)++;
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedGroupSnapFormat --
 *
 *	Formats and writes status information for scheduler
 *	group snapshot "s" into "buf".
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Increments "len" by number of characters written to "buf".
 *
 *----------------------------------------------------------------------
 */
static void
CpuSchedGroupSnapFormat(const CpuSchedGroupSnap *s, char *buf, int *len)
{
   const CpuSched_GroupState *c = &s->state;
   uint32 usecCharge;
   uint64 secCharge;

   // convert cpu consumption cycles to seconds
   CpuSched_UsageToSec(c->chargeCyclesTotal, &secCharge, &usecCharge);

   Proc_Printf(buf, len,
               "%5u %-12s "
               "%5u %-12s "
               "%4u %5u "
               "%9Lu.%03u "
               "%6u %6u %8s %7u "
               "%6u %6u %7u %4u "
               "%16Ld %16Ld %16Ld\n",
               s->groupID,
               s->groupName,
               s->parentID,
               s->parentName,
               s->members,
               c->vsmpCount,
               secCharge,
               usecCharge / 1000,
               c->alloc.min,
               c->alloc.max,
               Sched_UnitsToString(c->alloc.units),
               c->alloc.shares,
               c->base.min,
               c->base.max,
               c->base.shares,
               CpuSchedBaseSharesToUnits(c->base.shares, SCHED_UNITS_PERCENT),
               c->vtime,
               c->vtimeLimit,
               c->vtimeAged);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedProcWorldGroupRead --
 *
 *      Callback for read operation on /proc/vmware/vm/<id>/cpu/group
 *	procfs node.
 *
 * Results:
 *      Returns VMK_OK.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
CpuSchedProcWorldGroupRead(Proc_Entry *entry, char *buf, int *len)
{
   World_Handle *world = (World_Handle *) entry->private;
   CpuSched_Vsmp *vsmp = World_CpuSchedVsmp(world);
   char groupName[SCHED_GROUP_NAME_LEN];
   Sched_GroupID groupID;
   SP_IRQL prevIRQL;

   // initialize
   *len = 0;

   // snapshot group
   prevIRQL = CpuSchedVsmpCellLock(vsmp);
   groupID = CpuSched_GetVsmpLeader(world)->sched.group.groupID;
   CpuSchedVsmpCellUnlock(vsmp, prevIRQL);

   if (Sched_GroupIDToName(groupID, groupName, sizeof(groupName)) != VMK_OK) {
      strcpy(groupName, "unknown");
   }

   // format group
   Proc_Printf(buf, len, "%s\n", groupName);
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSchedProcWorldGroupWrite --
 *
 *	Callback for write operation on /proc/vmware/vm/<id>/cpu/group
 *	procfs node.
 *
 * Results:
 *	Returns VMK_OK iff successful, otherwise error code.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
CpuSchedProcWorldGroupWrite(Proc_Entry *entry, char *buf, int *len)
{
   World_Handle *world = (World_Handle *) entry->private;
   CpuSched_Vsmp *vsmp = World_CpuSchedVsmp(world);   
   VMK_ReturnStatus status;
   char *argv[2], *groupName;
   Sched_GroupID groupID;
   int argc;

   // fail if dedicated idle world
   if (CpuSchedVsmpIsSystemIdle(vsmp)) {
      return(VMK_BAD_PARAM);
   }

   // parse buf into args (assumes OK to overwrite)
   argc = Parse_Args(buf, argv, 2);
   if (argc != 1) {
      VsmpWarn(vsmp, "invalid group: unable to parse");
      return(VMK_BAD_PARAM);
   }
   groupName = argv[0];

   // convert group name to id
   groupID = Sched_GroupNameToID(groupName);
   if (groupID == SCHED_GROUP_ID_INVALID) {
      VsmpWarn(vsmp, "invalid group name: %s not found", groupName);
      return(VMK_NOT_FOUND);
   }

   // attempt to reparent
   status = Sched_ChangeGroup(world, groupID);
   if (status != VMK_OK) {
      VsmpWarn(vsmp, "unable to change group to %s", groupName);
      return(status);
   }

   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * CpuSched_ProcGroupsRead --
 *
 *	Callback for read operation on /proc/vmware/sched/groups
 *	procfs node.
 *
 * Results:
 * 	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
void
CpuSched_ProcGroupsRead(char *buf, int *len)
{
   uint32 i, snapCount;

   // format header
   Proc_Printf(buf, len,
	       "CPU Resource Related Info:"
	       "\n"
               "vmgid name         "
               " pgid pname        "
               "size vsmps "
               "      usedsec "
               "  amin   amax    units ashares "
               "  bmin   bmax bshares emin "
               "           vtime "
               "         vtlimit "
               "          vtaged\n");

   CpuSchedSnapLock();

   // snapshot groups 
   snapCount = 0;
   Sched_ForAllGroupsDo(CpuSchedGroupSnapshot, &snapCount);
   ASSERT(snapCount <= SCHED_GROUPS_MAX);

   // format groups
   for (i = 0; i < snapCount; i++) {
      CpuSchedGroupSnap *s = &cpuSched.procSnap.group[i];
      CpuSchedGroupSnapFormat(s, buf, len);
   }

   CpuSchedSnapUnlock();
}
