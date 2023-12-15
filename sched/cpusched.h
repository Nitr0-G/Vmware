/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * cpusched.h --
 *
 *	World CPU scheduler.
 */

#ifndef _CPUSCHED_H
#define _CPUSCHED_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "timer_dist.h"
#include "sched_dist.h"
#include "sched_ext.h"
#include "prda.h"
#include "proc.h"
#include "list.h"
#include "histogram.h"
#include "numa_ext.h"
#include "numasched.h"

/*
 * constants
 */

// cpu shares
#define	CPUSCHED_SHARES_MIN		(0)
#define	CPUSCHED_SHARES_MAX		(100000)
#define	CPUSCHED_SHARES_IDLE		(0)
#define	CPUSCHED_SHARES_PER_VCPU_LOW	(500)
#define	CPUSCHED_SHARES_PER_VCPU_NORMAL	(1000)
#define	CPUSCHED_SHARES_PER_VCPU_HIGH	(2000)
#define	CPUSCHED_SHARES_LOW(nvcpus)	(CPUSCHED_SHARES_PER_VCPU_LOW * (nvcpus))
#define	CPUSCHED_SHARES_NORMAL(nvcpus)	(CPUSCHED_SHARES_PER_VCPU_NORMAL * (nvcpus))
#define	CPUSCHED_SHARES_HIGH(nvcpus)	(CPUSCHED_SHARES_PER_VCPU_HIGH * (nvcpus))

// limits
#define	CPUSCHED_WORLDS_MAX	(MAX_WORLDS)
#define	CPUSCHED_VSMPS_MAX	(CPUSCHED_WORLDS_MAX)
#define	CPUSCHED_VCPUS_MAX	(CPUSCHED_WORLDS_MAX)
#define	CPUSCHED_VSMP_VCPUS_MAX	(MAX_VCPUS)
#define	CPUSCHED_PCPUS_MAX	(MAX_PCPUS)
#define	CPUSCHED_PACKAGES_MAX	(CPUSCHED_PCPUS_MAX)
#define	CPUSCHED_CELLS_MAX	(CPUSCHED_PCPUS_MAX)

// lock ranks
#define SP_RANK_CPUSCHED_CELL(id)	(SP_RANK_IRQ_CPUSCHED_LO + id)

// maximum rate enforcement
#define	CPUSCHED_ALLOC_MAX_NONE		(0)

// special values
#define	CPUSCHED_EVENT_NONE	    (0)
#define	CPUSCHED_INDEX_NONE	    (-1)

/*
 * types
 */

struct World_Handle;
struct CpuSched_Vsmp;

struct CpuMetrics_LoadHistory;
typedef struct CpuMetrics_LoadHistory CpuMetrics_LoadHistory;


/**
 *  Versioned atomic synchronization:
 *
 *    These synchronization macros allow single-writer/many-reader
 *    access to data, based on Lamport's "A Fast Mutual Exclusion
 *    Algorithm" [1977].
 *
 *    Note that the reader code may execute its body repeatedly
 *    in a loop, so the code in the body must be idempotent.
 *    E.g. do not do "sum += protectedVar", as this may
 *    cause "protectedVar" to be added to the sum several times.
 */
typedef struct {
   volatile uint32 v0;
   volatile uint32 v1;
} CpuSched_AtomicVersions;

static INLINE void
CPUSCHED_VERSIONED_ATOMIC_UPDATE_BEGIN(CpuSched_AtomicVersions *versions)
{
   (versions)->v0++;
   COMPILER_MEM_BARRIER();
}

static INLINE void
CPUSCHED_VERSIONED_ATOMIC_UPDATE_END(CpuSched_AtomicVersions *versions)
{
   COMPILER_MEM_BARRIER();
   (versions)->v1 = (versions)->v0;
}


#define CPUSCHED_VERSIONED_ATOMIC_READ_BEGIN(versions) \
 do { \
   uint32 __lamport_tmp; \
   do { \
      __lamport_tmp = (versions)->v1; \
      COMPILER_MEM_BARRIER(); \


#define CPUSCHED_VERSIONED_ATOMIC_READ_END(versions) \
      COMPILER_MEM_BARRIER(); \
   } while ((versions)->v0 != __lamport_tmp); \
 } while (0)


// iterator macros
#define	FORALL_PCPUS(_pcpu) \
	do { \
	  PCPU _pcpu; \
	  for (_pcpu = 0; _pcpu < numPCPUs; _pcpu++) 
#define	PCPUS_DONE } while(0)

#define	FORALL_REMOTE_PCPUS(_localPCPU, _pcpu) \
        FORALL_PCPUS(_pcpu) \
	  if (_pcpu != _localPCPU)
#define	REMOTE_PCPUS_DONE PCPUS_DONE

#define	FORALL_PACKAGES(_pcpu) \
	FORALL_PCPUS(_pcpu) \
          if (SMP_GetHTThreadNum(_pcpu) == 0)
#define	PACKAGES_DONE PCPUS_DONE

#define	FORALL_REMOTE_PACKAGES(_localPCPU, _pcpu) \
        FORALL_PACKAGES(_pcpu) \
	  if (SMP_GetPackageNum(_pcpu) != SMP_GetPackageNum(_localPCPU))
#define	REMOTE_PACKAGES_DONE PACKAGES_DONE

#define FORALL_NUMA_NODES(_node) \
	do { \
	  NUMA_Node _node; \
	  for (_node = 0; _node < NUMA_GetNumNodes(); _node++)
#define	NUMA_NODES_DONE } while(0)


typedef enum {
   CPUSCHED_NEW,		// unscheduled, freshly-allocated
   CPUSCHED_ZOMBIE,		// descheduled, in process of dying
   CPUSCHED_RUN,		// running
   CPUSCHED_READY,		// ready
   CPUSCHED_READY_CORUN,	// ready, pending co-schedule
   CPUSCHED_READY_COSTOP,	// ready, co-descheduled
   CPUSCHED_WAIT,		// blocked waiting for event
   CPUSCHED_BUSY_WAIT,		// busy-waiting for event
   CPUSCHED_NUM_RUN_STATES
} CpuSched_RunState;

typedef enum {
   CPUSCHED_CO_NONE,
   CPUSCHED_CO_RUN,
   CPUSCHED_CO_READY,
   CPUSCHED_CO_STOP
} CpuSched_CoRunState;

typedef enum {
   CPUSCHED_WAIT_NONE,
   CPUSCHED_WAIT_ACTION,
   CPUSCHED_WAIT_AIO,
   CPUSCHED_WAIT_DRIVER,
   CPUSCHED_WAIT_FS,
   CPUSCHED_WAIT_IDLE,
   CPUSCHED_WAIT_LOCK,
   CPUSCHED_WAIT_SEMAPHORE,
   CPUSCHED_WAIT_MEM,
   CPUSCHED_WAIT_NET,
   CPUSCHED_WAIT_REQUEST,
   CPUSCHED_WAIT_RPC,
   CPUSCHED_WAIT_RTC,
   CPUSCHED_WAIT_SCSI,
   CPUSCHED_WAIT_SLEEP,
   CPUSCHED_WAIT_TLB,
   CPUSCHED_WAIT_WORLDDEATH,
   CPUSCHED_WAIT_RWLOCK,
   CPUSCHED_WAIT_SWAPIN,
   CPUSCHED_WAIT_SWAP_AIO,
   CPUSCHED_WAIT_SWAP_SLOTS,
   CPUSCHED_WAIT_SWAP_DONE,
   CPUSCHED_WAIT_SWAP_CPTFILE_OPEN,
   CPUSCHED_WAIT_SWAP_ASYNC,
   CPUSCHED_WAIT_UW_SIGWAIT,
   CPUSCHED_WAIT_UW_PIPEREADER,
   CPUSCHED_WAIT_UW_PIPEWRITER,
   CPUSCHED_WAIT_UW_EXITCOLLECT,
   CPUSCHED_WAIT_UW_SLEEP,
   CPUSCHED_WAIT_UW_POLL,
   CPUSCHED_WAIT_UW_DEBUGGER,
   CPUSCHED_WAIT_UW_PROCDEBUG,
   CPUSCHED_WAIT_UW_UNIX_CONNECT,
   CPUSCHED_WAIT_UW_TERM,
   CPUSCHED_NUM_WAIT_STATES
} CpuSched_WaitState;

typedef struct {
   uint32 count;
   Timer_Cycles elapsed;
   Timer_Cycles start;
   CpuSchedVtime vtStart;
   Histogram_Handle histo;
} CpuSched_StateMeter;

typedef struct {
   uint32 min;
   uint32 max;
   uint32 shares;
   Sched_Units units;
} CpuSched_Alloc;

typedef struct {
   // uptime
   Timer_Cycles uptimeStart;

   // migrations and switches
   uint32 worldSwitch;
   uint32 migrate;
   uint32 pkgMigrate;
   uint32 wakeupMigrateIdle;

   // event counters
   uint32 timer;
   uint32 halt;
   uint32 quantumExpire;

   // action processing
   uint32 actionWakeupCheck;
   uint32 actionNotify;
   uint32 actionPreventWait[CPUSCHED_NUM_WAIT_STATES];

   // forced wakeups
   uint32 forceWakeup[CPUSCHED_NUM_WAIT_STATES];

   // ht debugging
   uint32 htWholePackageSamples;
   uint32 htTotalSamples;
} CpuSched_VcpuStats;

typedef struct {
   Timer_Cycles nextUpdate;
   uint64 prevCount;
   uint64 agedCountFast;
   uint64 agedCountSlow;
} CpuSched_HTEventCount;

typedef struct {
   CpuSchedVtime main;
   CpuSchedVtime extra;
   Sched_GroupPath path;
   CpuSchedStride stride;
   CpuSchedStride nStride;
} CpuSched_VtimeContext;

typedef struct CpuSched_Vcpu {
   // enclosing object
   struct CpuSched_Vsmp *vsmp;

   // for array-based lists
   int schedIndex;

   // state
   CpuSched_RunState runState;
   CpuSched_WaitState waitState;
   uint32 waitEvent;
   Bool limbo;

   // action wakeup
   // n.b. update mask => must hold this lock and sched cell lock
   //      read mask => must hold this lock or sched cell lock
   SP_SpinLockIRQ actionWakeupLock;
   uint32 actionWakeupMask;

   // in-progress flags
   volatile Bool switchInProgress;
   Bool removeInProgress;

   // special events
   uint32 sleepEvent;
   uint32 actionEvent;
   uint32 haltEvent;

   // sleep event lock
   SP_SpinLockIRQ sleepLock;   

   // placement
   CpuMask affinityMask;
   PCPU pcpuMapped;
   PCPU pcpu;
   PCPU pcpuHandoff;

   // idle flags
   Bool idle;

   // current accounting
   CpuSched_AtomicVersions chargeStartVersion;
   Timer_Cycles chargeStart;
   Timer_Cycles phaltStart;
   Timer_Cycles localHaltStart;
   Timer_Cycles sysCyclesOverlap;
   Atomic_uint32 sysKCycles;

   // intra-skew information
   int intraSkew;
   Histogram_Handle intraSkewHisto;
   
   // cumulative accounting
   CpuSched_AtomicVersions chargeCyclesVersion;
   Timer_Cycles chargeCyclesTotal;
   Timer_Cycles sysCyclesTotal;
   Timer_Cycles sysOverlapTotal;

   // per-state accounting
   CpuSched_StateMeter runStateMeter[CPUSCHED_NUM_RUN_STATES];
   CpuSched_StateMeter limboMeter;
   CpuSched_StateMeter waitStateMeter[CPUSCHED_NUM_WAIT_STATES];
   CpuSched_StateMeter wakeupLatencyMeter;
   Histogram_Handle runWaitTimeHisto;
   Histogram_Handle preemptTimeHisto;
   Histogram_Handle disablePreemptTimeHisto;
   TSCCycles disablePreemptStartTime;
   
   // per-pcpu accounting
   Timer_Cycles pcpuRunTime[CPUSCHED_PCPUS_MAX];

   // per-vcpu load
   CpuMetrics_LoadHistory *loadHistory;

   // quantum state
   Timer_AbsCycles quantumExpire;

   // statistics
   CpuSched_VcpuStats stats;

   // hyperthreading
   CpuSched_HTEventCount htEvents;
} CpuSched_Vcpu;

#define LIST_ITEM_TYPE CpuSched_Vcpu*
#define LIST_NAME CpuSched_VcpuArray
#define LIST_SIZE CPUSCHED_VSMP_VCPUS_MAX
#define LIST_IDX_FIELD schedIndex
#include "staticlist.h"

typedef struct {
   uint32 samples;
   uint32 good;
   uint32 bad;
   uint32 resched;
   uint32 ignore;
   uint32 intraSkewSamples;
   uint32 intraSkewOut;
} CpuSched_SkewStats;

typedef struct {
   Timer_AbsCycles lastUpdate;
   CpuSched_SkewStats stats;
} CpuSched_SkewState;

typedef struct {
   // cumulative vtime aged
   CpuSchedVtime vtimeAged;

   // consumption in excess of entitlement
   Timer_Cycles bonusCyclesTotal;

   // lag-bounding stats
   uint32 boundLagBehind;
   uint32 boundLagAhead;
   CpuSchedVtime boundLagTotal;

   // inter-cell migration
   uint32 cellMigrate;

   // ht debugging
   uint32 htAllWholeSamples;
   uint32 htAllHalfSamples;
   uint32 htMixedRunSamples;
   uint32 htTotalSamples;
} CpuSched_VsmpStats;

typedef struct CpuSched_Vsmp {
   // for array-based lists
   int schedIndex;

   // world group leader
   struct World_Handle *leader;

   // scheduler cell
   struct CpuSched_Cell *cell;

   // virtual cpus   
   CpuSched_VcpuArray vcpus;
   
   // protects vcpuArray AND skew state
   SP_SpinLockIRQ vcpuArrayLock;

   // co-scheduling state
   int disableCoDeschedule;
   CpuSched_SkewState skew;

   // aggregate state
   CpuSched_CoRunState coRunState;
   int nRun;
   int nWait;
   int nIdle;

   NUMASched_VsmpInfo numa;

   // external allocation state
   CpuSched_Alloc alloc;

   // internal allocation state
   CpuSched_Alloc base;
   CpuSched_VtimeContext vtime;
   Bool groupEnforceMax;

   // max rate enforcement
   CpuSchedStride strideLimit;
   CpuSchedVtime vtimeLimit;

   // quantum state
   Timer_AbsCycles quantumExpire;

   // co-scheding configuration
   Bool strictCosched;
   
   // aggregate affinity state
   Bool affinityConstrained;	// any vcpus have affinity set?
   Bool jointAffinity;		// do all vcpus have same affin mask?
   Bool hardAffinity;		// was affinity set by user?

   // hyperthreading constraints
   Sched_HTSharing htSharing;
   Bool htQuarantine;
   Sched_HTSharing maxHTConstraint;
   uint32 quarantinePeriods;
   uint32 numQuarantines;

   // stats
   CpuSched_VsmpStats stats;
} CpuSched_Vsmp;

#define LIST_ITEM_TYPE CpuSched_Vsmp*
#define LIST_NAME CpuSched_VsmpArray
#define LIST_SIZE CPUSCHED_VSMPS_MAX
#define LIST_IDX_FIELD schedIndex
#include "staticlist.h"

typedef struct {
   // group allocation
   CpuSched_Alloc alloc;
   CpuSched_Alloc base;

   // vsmps covered by group
   uint32 vsmpCount;

   // group virtual time, limit
   // must hold scheduler tree lock to update
   CpuSched_AtomicVersions vtimeVersion;
   CpuSchedVtime vtime;
   CpuSchedVtime vtimeLimit;

   // group stride, limit
   CpuSchedStride stride;
   CpuSchedStride strideLimit;

   // stats
   Timer_Cycles chargeCyclesTotal;
   CpuSchedVtime vtimeAged;

   // per-group load
   CpuMetrics_LoadHistory *loadHistory;
} CpuSched_GroupState;

typedef struct {
   // scheduling state
   CpuSched_Vcpu vcpu;			// per-VCPU state
   CpuSched_Vsmp vsmpData;		// per-VSMP state
 					// (used if group leader, else unused)
   // initial entry point
   CpuSched_StartFunc startFunc;	// initial function
   void	*startData;			// initial argument

   // nodes in /proc/vmware/vm/<id>/cpu
   Proc_Entry procDir;			// "/"
   Proc_Entry procStatus;		// "status"
   Proc_Entry procStateTimes;		// "state-times"
   Proc_Entry procStateCounts;		// "state-counts"
   Proc_Entry procPcpuRunTimes;		// "run-times"
   Proc_Entry procWaitStats;		// "wait-stats"
   Proc_Entry procMin;			// "min"
   Proc_Entry procMax;			// "max"
   Proc_Entry procUnits;		// "units"
   Proc_Entry procShares;		// "shares"
   Proc_Entry procGroup;		// "group"
   Proc_Entry procAffinity; 		// "affinity"
   Proc_Entry procDebug;		// "debug"
   Proc_Entry procHyperthreading;	// "hyperthreading"
   Proc_Entry procRunStatesHisto;	// "run-state-histo"
   Proc_Entry procWaitStatesHisto;      // "wait-state-histo"
} CpuSched_Client;

typedef struct {
   uint32 vcpus;	// active virtual cpus
   uint32 vms;		// active VMs
   uint32 baseShares;	// active base shares
} CpuSched_LoadMetrics;


typedef enum {
   CPUSCHED_RWWAIT_READ,
   CPUSCHED_RWWAIT_WRITE,
   CPUSCHED_RWWAIT_NONE
} CpuSched_RWWaitLockType;

/*
 * exported variables
 */

extern uint32 CpuSched_EIPAfterHLT;

/*
 * operations
 */

void CpuSched_IdleHaltEnd(Bool fromIntrHandler);

void CpuSched_UsageToSec(Timer_Cycles usage, uint64 *sec, uint32 *usec);

void CpuSched_Init(Proc_Entry *procSchedDir, uint32 cellSize);

void CpuSched_StartWorld(struct World_Handle *previous)
     __attribute__((regparm(1)));

void CpuSched_MarkReschedule(PCPU pcpu);
void CpuSched_MarkRescheduleLocal(void);
void CpuSched_Reschedule(void);

void CpuSched_YieldToHost(void);
void CpuSched_IdleLoop(void);
void CpuSched_HostInterrupt(void);

VMK_ReturnStatus CpuSched_Wait(uint32 event,
                               CpuSched_WaitState waitType,
                               SP_SpinLock *lock);

VMK_ReturnStatus CpuSched_TimedWait(uint32 event,
                                    CpuSched_WaitState waitType,
                                    SP_SpinLock *lock,
                                    uint32 msecs);

VMK_ReturnStatus CpuSched_WaitDirectedYield(uint32 event,
                                            CpuSched_WaitState waitType,
                                            uint32 actionWakeupMask,
                                            SP_SpinLock *lock,
                                            World_ID directedYield);

VMK_ReturnStatus CpuSched_WaitIRQ(uint32 event,
                                  CpuSched_WaitState waitType,
                                  SP_SpinLockIRQ *lock,
                                  SP_IRQL callerPrevIRQL);

VMK_ReturnStatus CpuSched_WaitIRQDirectedYield(uint32 event,
                                               CpuSched_WaitState waitType,
                                               uint32 actionWakeupMask,
                                               SP_SpinLockIRQ *lock,
                                               SP_IRQL callerPrevIRQL,
                                               World_ID directedYield);

VMK_ReturnStatus CpuSched_RWWait(uint32 event,
                                 CpuSched_WaitState waitType,
                                 SP_RWLock *rwlock,
                                 CpuSched_RWWaitLockType rwlockType);

VMK_ReturnStatus CpuSched_RWWaitIRQ(uint32 event,
                                    CpuSched_WaitState waitType,
                                    SP_RWLockIRQ *rwlockIRQ,
                                    CpuSched_RWWaitLockType rwlockIRQType,
                                    SP_IRQL callerPrevIRQL);

VMK_ReturnStatus CpuSched_TimedRWWait(uint32 event,
                                      CpuSched_WaitState waitType,
                                      SP_RWLock *rwlock,
                                      CpuSched_RWWaitLockType rwlockType,
                                      uint32 msecs);

VMK_ReturnStatus CpuSched_Add(struct World_Handle *world,
                              Sched_CpuClientConfig *config,
                              Bool running);
VMK_ReturnStatus CpuSched_Remove(struct World_Handle *world);
void CpuSched_WorldCleanup(struct World_Handle *world);

Bool CpuSched_Wakeup(uint32 event);
Bool CpuSched_ForceWakeup(struct World_Handle *world);

void CpuSched_AsyncCheckActions(struct World_Handle *world);
VMK_ReturnStatus CpuSched_AsyncCheckActionsByID(World_ID worldID);

void CpuSched_TimerInterrupt(Timer_AbsCycles now);
Timer_Cycles CpuSched_ProcessorIdleTime(PCPU pcpu, Bool locked);
void CpuSched_PcpuUsageStats(Timer_Cycles* idle,
                             Timer_Cycles* used,
                             Timer_Cycles* sysOverlap);
uint64 CpuSched_VcpuUsageUsec(struct World_Handle *world);

Bool CpuSched_IsHostWorld(void);
Bool CpuSched_HostWorldCmp(struct World_Handle *world);
Bool CpuSched_HostIsRunning(void);

typedef void (*WorldForallFn)(struct World_Handle*, void*);
VMK_ReturnStatus CpuSched_ForallGroupMembersDo(struct World_Handle *leader, WorldForallFn fn, void *data);
VMK_ReturnStatus CpuSched_ForallGroupLeadersDo(WorldForallFn fn, void *data);

uint32 CpuSched_WorldSwitchCount(struct World_Handle *world);
Bool CpuSched_WorldHasHardAffinity(const struct World_Handle *world);
uint8 CpuSched_NumAffinityPackages(CpuMask mask);
CpuMask CpuSched_PcpuMask(PCPU p, Bool withPartner);
void CpuSched_SysServiceDoneSample(void);

// reallocations
void CpuSched_RequestReallocate(void);
VMK_ReturnStatus CpuSched_Reallocate(void);

// min/max/shares querying
void CpuSched_GetAlloc(struct World_Handle *world, CpuSched_Alloc *alloc);
uint32 CpuSched_BaseSharesToUnits(uint32 bshares, Sched_Units units);

// load statistics
void CpuSched_GetLoadMetrics(CpuSched_LoadMetrics *m);
void CpuSched_SampleLoadHistory(void);

// NUMA support
struct NUMASched_SnapInfo;
void CpuSched_NUMASnap(struct NUMASched_Snap *info);
void CpuSched_SetHomeNode(struct World_Handle *leader, NUMA_Node nodeNum);
void CpuSched_ResetNUMAStats(void);

// affinity
VMK_ReturnStatus CpuSched_WorldSetAffinity(World_ID world, CpuMask affinMask);

// scheduler groups
void CpuSched_GroupChanged(struct World_Handle *world);
VMK_ReturnStatus CpuSched_GroupSetAlloc(Sched_GroupID id,
                                        const Sched_Alloc *alloc);
VMK_ReturnStatus CpuSched_MoveVmAllocToGroup(struct World_Handle *world,
                                             Sched_GroupID id);
VMK_ReturnStatus CpuSched_MoveGroupAllocToVm(Sched_GroupID id,
                                             struct World_Handle *world);

// vmkernel config change callbacks
VMK_ReturnStatus CpuSched_UpdateConfig(Bool write, Bool valueChanged, int indx);
VMK_ReturnStatus CpuSched_UpdateCOSMin(Bool write, Bool valueChanged, int indx);

/*
 * VMK Entry Points
 */

VMKERNEL_ENTRY CpuSched_VcpuHalt(DECLARE_1_ARG(VMK_VCPU_HALT, int64, timeOutUsec));
VMKERNEL_ENTRY CpuSched_ActionNotifyVcpu(DECLARE_1_ARG(VMK_ACTION_NOTIFY_VCPU, Vcpuid, v));

#endif
