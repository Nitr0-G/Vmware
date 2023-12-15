/* **********************************************************
 * Copyright 2000 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * memsched.c --
 *
 *	Memory scheduling policies to manage allocation of machine
 *	memory to worlds. 
 *
 *	Two separate mechanisms are available for reclaiming memory
 *	allocated to VMs: ballooning and swapping.  The "balloon"
 *	mechanism relies upon a "vmmemctl" driver loaded into the
 *	guest.  The vmkernel can direct this driver to allocate or
 *	deallocate physical memory within the guest OS.  Allocating
 *	physical pages places the guest under memory pressure, forcing
 *	it to invoke its own native memory management algorithms to
 *	decide which of its own pages should be reclaimed (and
 *	possibly swapped to its own virtual disk).  The "swap"
 *	mechanism forcibly pages memory from VMs to a vmkernel disk
 *	device without any involvement by the guest.  The balloon
 *	mechanism is best viewed as a common-case optimization that is
 *	used whenever possible for optimum performace.  The swapping
 *	mechanism is best viewed as a reliable mechanism of last
 *	resort that can be used to reclaim memory when ballooning is
 *	not feasible.  An additional content-based transparent page
 *	sharing mechanism may be used to reduce system-wide memory
 *	consumption (see the PShare module for more details).
 *
 *      A higher-level proportional-share memory management policy is
 *      used to determine overall allocations.  Statistical sampling
 *      is employed to estimate the fraction of pages actively used by
 *      each VM. This fraction is combined with the specified share
 *      allocation to determine the target memory size, which is
 *      achieved via mechanisms described above.
 *
 * 	The on-going effort to make the memory scheduler operate within 
 * 	the scheduler tree hierarchical framework has resulted in certain 
 * 	new concepts. Each group in the hierarchy, which is not a MemSChed 
 * 	Client is defiend by the following parameters w.r.t. to the memory 
 * 	resource:
 *
 * 	min	: Guaranteed minimum allocation for the group.
 * 	minLimit: Upper bound for total minimum allocations for the group..
 * 	max	: Upper bound for total storage (memory + swap) available 
 * 		  to the group.
 * 	hardMax	: Guranteed storage available to the group.
 *	shares	: Specifiies relative importance of the group w.r.t other 
 *		  groups under the same parent group.
 *
 * 	where the following conditions hold:
 *
 * 		min <= minLimit <= max
 * 		min <= hardMax  <= max
 *
 *	MemSched clients (VMs, UserWorlds, etc.) continue to be defined
 *	by "min," "max" and "shares." If "min" is not specified, the
 *	existing autoMin implementation will be used to determine a 
 *	suitable minimum allocation for the memory client. Because 
 *	"minLimit" and "hardMax" are meaningful only for non memsched 
 *	client groups, in the case of memsched client groups "minLimit" 
 *	is always be equal to "min" and "hardMax" is always be equal
 *	to "max."
 *
 *	The memsched implementation computes and stores the following
 *	internal representations of "min," "max" and "shares" for each
 *	group.
 *
 * 	baseShares: Normalized shares across all groups on the system. 
 *	baseMin   : Total "baseMins" of all immediate child groups. 
 *	baseMax   : Total "baseMaxs" of all immediate child groups.
 *	eMin      : Total "eMins" of all immediate child groups, but
 *	            never less than own "min."
 *	eMax      : Total "eMaxs" of all immediate child groups, but
 *	            never less than own "hardMax."
 *
 * 	"baseMin" and "baseMax" are used for memory allocation purposes.
 * 	"eMin" and eMax" are used for admission control. In the case
 * 	of a memsched client group "baseMin" and "eMin" is always equal
 * 	to "min" and "baseMax" and "eMAx" is always equal to "max."
 *
 *	Supports the following configuration options:
 *	  CONFIG_MEM_BALANCE_PERIOD,
 *	  CONFIG_MEM_SAMPLE_PERIOD,
 *	  CONFIG_MEM_SAMPLE_SIZE,
 *	  CONFIG_MEM_SAMPLE_HISTORY,
 *	  CONFIG_MEM_IDLE_TAX,
 *	  CONFIG_MEM_CTL_MAX_{WINNT4,WINNT5,LINUX,BSD},
 *	  CONFIG_MEM_CTL_MAX_PERCENT,
 *	  CONFIG_MEM_SHARE_SCAN_{VM,TOTAL},
 *	  CONFIG_MEM_SHARE_CHECK_{VM,TOTAL},
 *	  CONFIG_MEM_ADMIT_HEAP_MIN.
 *	  CONFIG_STRESS_REMAP_NODE.
 */

#include "vm_types.h"
#include "vm_asm.h"
#include "vm_libc.h"
#include "vmkernel.h"
#include "action.h"
#include "timer.h"
#include "host.h"
#include "config.h"
#include "libc.h"
#include "parse.h"
#include "pshare.h"
#include "memmap.h"
#include "alloc.h"
#include "alloc_inline.h"
#include "memalloc.h"
#include "kvmap.h"
#include "numa.h"
#include "migrateBridge.h"
#include "user.h"
#include "util.h"
#include "sharedArea.h"
#include "bh.h"

#include "balloon_def.h"
#include "memsched.h"
#include "memsched_int.h"
#include "sched_int.h"
#include "memMetrics.h"
#include "vmmem.h"

#define LOGLEVEL_MODULE MemSched
#include "log.h"

/*
 * Compilation flags
 */

// general debugging
#define	MEMSCHED_DEBUG			(vmx86_debug && vmx86_devel)
#define	MEMSCHED_DEBUG_VERBOSE		(0)

// targeted debugging
#define	MEMSCHED_DEBUG_PERIODIC		(0)
#define	MEMSCHED_DEBUG_BALANCE		(0)
#define	MEMSCHED_DEBUG_ENFORCE		(0)
#define	MEMSCHED_DEBUG_SWAP_STRESS	(0)
#define	MEMSCHED_DEBUG_DISABLE_BALLOON	(0)
#define	MEMSCHED_DEBUG_TRIGGER		(1)
#define	MEMSCHED_DEBUG_RESUME		(1)
#define	MEMSCHED_DEBUG_TAX		(1)
#define	MEMSCHED_DEBUG_LOW_WAIT		(1)
#define	MEMSCHED_DEBUG_EARLY_WAIT	(1)
#define	MEMSCHED_DEBUG_AUTO_MIN		(0)
#define	MEMSCHED_DEBUG_RESUME_SWAP	(0)
#define MEMSCHED_DEBUG_BALLOON_STATS    (1)


/*
 * Constants
 */

// pshare parameter ranges
#define	MEM_PSHARE_SCAN_RATE_MIN	(0)
#define	MEM_PSHARE_SCAN_RATE_MAX	(1000)
#define	MEM_PSHARE_CHECK_RATE_MIN	(0)
#define	MEM_PSHARE_CHECK_RATE_MAX	(1000)

// timeouts (in milliseconds)
#define	MEMSCHED_EARLY_TIMEOUT		(5000)
#define	MEMSCHED_HOST_WAIT_SKIP_TIMEOUT	(500)

// allocation constants
#define	MEMSCHED_SHARES_INV_MAX		(1000000)
#define	MEMSCHED_PPS_MIN		(0)
#define	MEMSCHED_PPS_MAX		(1LL << 62)
#define	MEMSCHED_BALANCE_THRESHOLD	(PAGES_PER_MB / 4)
#define	MEMSCHED_MIN_TARGET_DELTA	(PAGES_PER_MB)

// min alloction for "UW Nursery" system scheduler group (unit: MB)
#define MEMSCHED_UW_NURSERY_ALLOC_MIN	(32)

// overcommitted resume constants
#define	MEMSCHED_RESUME_SWAP_DELTA	(PAGES_PER_MB)
#define	MEMSCHED_RESUME_MIN_RESERVE	(2 * MEMSCHED_RESUME_SWAP_DELTA)

// cost ratio scaling factors
#define	MEMSCHED_COST_SCALE_SHIFT	(8)

// future: possibly expose as config option
// rebalancing threshold (%mem)
#define	MEMSCHED_BALANCE_DELTA_PCT	(5)

// future: possibly expose as config options
// default state thresholds (%mem)
#define	MEMSCHED_FREE_HIGH_PCT		(6)
#define	MEMSCHED_FREE_SOFT_PCT		(4)
#define	MEMSCHED_FREE_HARD_PCT		(2)
#define	MEMSCHED_FREE_LOW_PCT		(1)

#define MEMSCHED_MAX_SWAP_SLACK         (PAGES_PER_MB)
#define MEMSCHED_BALLOON_BONUS_PAGES    (PAGES_PER_MB)

// names
#define	MEMSCHED_BALLOON_NAME		"vmmemctl"

// non-existent list index
#define	MEMSCHED_INDEX_NONE		(-1)

// buffer sizes
#define	MEMSCHED_AFFINITY_BUF_LEN	(64)
#define	MEMSCHED_XFER_LOG_BUF_SIZE	(64)

// Min pages to swap if config option MemSwap stress option is set
#define MEMSCHED_SWAP_STRESS_MIN        (50)

// page migration stress parameter
#define	MEMSCHED_NODE_STRESS_RATE	(50)


/*
 * Types
 */

#define	MEMSCHED_STATES_MAX		(4)
typedef enum {
   MEMSCHED_STATE_HIGH,
   MEMSCHED_STATE_SOFT,
   MEMSCHED_STATE_HARD,
   MEMSCHED_STATE_LOW
} MemSchedState;

typedef struct {
   MemSchedState state;	        // state
   MemSchedState lowState;	// transition to low 
   uint32 lowPages;	        //   when free < lowPages
   uint32 lowCount;
   MemSchedState highState;	// transition to high
   uint32 highPages;	        //   when free > highPages
   uint32 highCount;
} MemSchedStateTransition;

typedef struct {
   MemSchedStateTransition table[MEMSCHED_STATES_MAX];
   MemSchedState state;
   SP_SpinLockIRQ lock;	        // for protecting transition state
   uint32 highThreshold;        // high memory threshold (in pages)
   uint32 lowThreshold;	        // low  memory threshold (in pages)
   uint32 triggerCount;	        // stats: normal trigger callbacks
} MemSchedFreeState;

typedef struct {
   // identity
   Sched_GroupID groupID;
   char groupName[SCHED_GROUP_NAME_LEN];

   // parent identity
   Sched_GroupID parentID;
   char parentName[SCHED_GROUP_NAME_LEN];

   // state
   uint32 members;
   uint32 clients;
   MemSched_GroupState state;
} MemSchedGroupSnap;

typedef struct MemSched {
   SP_SpinLock    lock;		// for mutual exclusion
   List_Links     schedQueue;   // list of current mem sched clients
   int            numScheds;    // number of managed clients

   uint32	totalSystemSwap; // total swap visible to memsched

   Bool           allClientsResponsive; // any unresponsive clients?

   MemSchedFreeState freeState;	// memory level state transitions
   uint32       bhNum;          // memory reallocation bh handler
   uint32       reallocWaitCount;  // count of worlds waiting for reallocation 

   uint32	idleCost;	// scaled cost ratio idle:used pages

   uint32	idleTax;	// config: idle memory tax race (in percent)
   uint32	samplePeriod;	// config: usage sampling period (in sec)
   uint32	sampleSize;	// config: sample set size (in pages)
   uint32	sampleHistory;	// config: usage sampling history (in periods)
   uint32	balancePeriod;	// config: balancing period (in msec)

   uint32	shareScanVM;	// config: per-VM scan rate  (in pages/sec)
   uint32	shareScanTotal;	// config: total  scan rate  (in pages/sec)
   uint32	shareCheckVM;	// config: per-VM check rate (in pages/sec)
   uint32	shareCheckTotal;// config: total  check rate (in pages/sec)
   uint32	shareScanRate;	// current per-VM scan  rate (in pages/sec)
   uint32	shareCheckRate;	// current per-VM check rate (in pages/sec)
   Bool		shareEnable;	// scanning or checking currently enabled?

   uint32       reallocGen;	// realloc: reallocation generation counter 
   uint32       reallocFastCount; // realloc: total fast bh-handler reallocs
   uint32       reallocSlowCount; // realloc: total slow memsched world reallocs
   uint32	reallocPages;	// realloc: change in free pages threshold

   uint32       maxCptInvalidOvhdPages; // Maximum number of invalid overhead
                                        // pages accessed during checkpoint of
                                        // a VM.

   uint32       defaultNodeAffinity;  // mask for all present nodes

   uint32	nodeStressCount;// stress: periodic page migration stress
   uint32	nodeStressSeed;	// stress: rng state 

   MemSchedGroupSnap group[SCHED_GROUPS_MAX];

   Proc_Entry	procMem;	// procfs: /proc/vmware/sched/mem
   Proc_Entry	procMemVerbose;	// procfs: /proc/vmware/sched/mem-verbose
} MemSched;

/*
 * Globals
 */

static MemSched memSched;

/*
 * Local functions
 */

// low-level allocation operations
static void MemSchedAllocInit(MemSched_Alloc *memAlloc,
		              const Sched_Alloc *alloc);
static void MemSchedUpdateAutoMins(void);
static void MemSchedUpdateTotals(void);
static void MemSchedUpdateTargets(void);
static void MemSchedUpdateAllocs(void);
static void MemSchedCommitAllocs(Bool canBlock);
static void MemSchedClientBalloonSet(MemSched_Client *c, uint32 nPages);
static void MemSchedClientBalloonUpdateMax(MemSched_Client *c);
static void MemSchedClientSwapSet(MemSched_Client *c, uint32 nPages);
static void MemSchedClientUpdateSwap(MemSched_Client *c);

// admission control operations
static VMK_ReturnStatus MemSchedAdmit(const World_Handle *world, 
		                      Bool vmResuming,
                                      MemSched_Alloc *alloc);
static void MemSchedReservedMem(Bool swapEnabled,
                                int32 *avail,
                                int32 *reserved,
                                int32 *autoMinReserved);
static void MemSchedReservedSwap(Bool swapEnabled,
                                 int32 *avail,
                                 int32 *reserved);

// high-level allocation operations
static void MemSchedReallocate(Bool canBlock);
static void MemSchedReallocBHHandler(void *clientData);
static void MemSchedReallocReqSlow(void);
static void MemSchedReallocReqFast(void);

// parameter reconfiguration operations
static void MemSchedUpdatePShareRate(void);

// free state operations
static void MemSchedFreeStateInit(MemSchedFreeState *s, uint32 nPages);
static const char *MemSchedStateToString(MemSchedState n);

// other operations
static INLINE int32 MemSchedFreePages(void);
static void MemSchedUpdateThreshold(uint32 nPages);
static Bool MemSchedSetNodeAffinityInt(MemSched_Client *c, uint32 affinMask, Bool hard);

// formatting and parsing operations
static void MemSchedStatusHeaderFormat(Bool verbose, char *buf, int *len);
static void MemSchedClientStatusFormat(MemSched_Client *c, Bool verbose, char *buf, int *len);
static void MemSchedFreeStateFormat(MemSchedFreeState *s, char *buf, int *len);
static MemSched_ColorVec *MemSchedParseColorList(World_Handle *world, const char *colorList);
static int MemSchedColorListFormat(const MemSched_ColorVec *colors,char *buf,int maxLen);

// procfs operations
static int MemSchedClientProcStatusRead(Proc_Entry *e, char *buf, int *len);
static int MemSchedClientProcMinRead(Proc_Entry *e, char *buf, int *len);
static int MemSchedClientProcMinWrite(Proc_Entry *e, char *buf, int *len);
static int MemSchedClientProcSharesRead(Proc_Entry *e, char *buf, int *len);
static int MemSchedClientProcSharesWrite(Proc_Entry *e, char *buf, int *len);
static int MemSchedClientProcAffinityRead(Proc_Entry *e, char *buf, int *len);
static int MemSchedClientProcAffinityWrite(Proc_Entry *e, char *buf, int *len);
static int MemSchedClientProcPShareRead(Proc_Entry *e, char *buf, int *len);
static int MemSchedClientProcRemapRead(Proc_Entry *e, char *buf, int *len);
static int MemSchedClientProcMigRateRead(Proc_Entry *e, char *buf, int *len);
static int MemSchedClientProcMigRateWrite(Proc_Entry *e, char *buf, int *len);
static int MemSchedProcRead(Proc_Entry *e, char *buf, int *len);
static int MemSchedProcWrite(Proc_Entry *e, char *buf, int *len);

// Scheduler group(s) operations
static INLINE Bool MemSchedNodeIsGroup(const Sched_Node *n);
static INLINE Bool MemSchedNodeIsMemClient(const Sched_Node *n);
static void MemSchedGroupSnapshot(Sched_Group *g, void *data);
static void MemSchedGroupSnapFormat(const MemSchedGroupSnap *s, 
				    char *buf, int *len);
static void MemSchedGroupSetAllocInt(Sched_Group *group,
				     const MemSched_Alloc *alloc);
static VMK_ReturnStatus MemSchedAdmitGroupInt(const Sched_Group *parentGroup,
					      uint32 minReqPages,
					      uint32 maxReqPages);
static VMK_ReturnStatus MemSchedIncClientGroupSize(Sched_Group *group,
						   uint32 minSize,
						   uint32 maxSize);
static void MemSchedDecClientGroupSize(Sched_Group *group, 
				       uint32 minSize, uint32 maxSize);

/*
 * Macros
 */

// structured logging macros
#define	ClientWarn(c, fmt, args...) \
   VmWarn(ClientGroupID(c), fmt ,  ##args)
#define ClientLOG(c, fmt, args...) \
   VMLOG(0, ClientGroupID(c), fmt , ##args)

#define MemSchedDebug(vmID, fmt, args...) \
 if (MEMSCHED_DEBUG) VMLOG(0, vmID, fmt , ##args)

#define ClientDebug(c, fmt, args...) \
 if (MEMSCHED_DEBUG) ClientLOG(c, fmt , ##args)

#define FORALL_MEMSCHED_CLIENTS(queue, _c)                      \
do {                                                            \
   List_Links *_itemPtr;                                        \
   LIST_FORALL(queue, _itemPtr) {                               \
      MemSched_Client *_c = (MemSched_Client *)_itemPtr;        \
     
#define MEMSCHED_CLIENTS_DONE                                   \
   }                                                            \
} while (0)

#define FORALL_MEMSCHED_VMM_CLIENTS(queue, _c, _vmm)            \
   FORALL_MEMSCHED_CLIENTS(queue, _c)                           \
      MemSchedVmm *_vmm = &_c->vmm;                             \
      if (!_vmm->valid) {                                       \
         continue;                                              \
      } else {                                                  \

#define MEMSCHED_VMM_CLIENTS_DONE                               \
      }                                                         \
   MEMSCHED_CLIENTS_DONE



/*
 * Utility operations
 */

static INLINE void
MemSchedLock(void)
{
   SP_Lock(&memSched.lock);
}

static INLINE void
MemSchedUnlock(void)
{
   SP_Unlock(&memSched.lock);
}

static INLINE Bool
MemSchedIsLocked(void)
{
   return SP_IsLocked(&memSched.lock);
}

static INLINE void
MemSchedTimedWaitLock(uint32 msecs)
{
   ASSERT(MemSchedIsLocked());
   if (msecs > 0) {
      CpuSched_TimedWait((uint32) &memSched.lock,
                         CPUSCHED_WAIT_MEM,
                         &memSched.lock, msecs);
   } else {
      CpuSched_Wait((uint32) &memSched.lock,
                     CPUSCHED_WAIT_MEM,
                     &memSched.lock);
   }
   SP_Lock(&memSched.lock);
}

static INLINE void
MemSchedWakeup(void)
{
   ASSERT(MemSchedIsLocked());
   CpuSched_Wakeup((uint32) &memSched.lock);
}

static INLINE VMK_ReturnStatus
MemSchedReallocWaitLock(void)
{
   VMK_ReturnStatus status;
   ASSERT(MemSchedIsLocked());
   memSched.reallocWaitCount++;
   status = CpuSched_Wait((uint32) &memSched.reallocWaitCount, 
                          CPUSCHED_WAIT_MEM, &memSched.lock);
   MemSchedLock();
   return status;
}

static INLINE void
MemSchedReallocWakeup(void)
{
   ASSERT(MemSchedIsLocked());
   if (memSched.reallocWaitCount > 0) {
      CpuSched_Wakeup((uint32) &memSched.reallocWaitCount);
      memSched.reallocWaitCount = 0;
   }
}

static INLINE SP_IRQL
MemSchedFreeStateLock(void)
{
   return SP_LockIRQ(&memSched.freeState.lock, SP_IRQL_KERNEL);
}

static INLINE void
MemSchedFreeStateUnlock(SP_IRQL prevIRQL)
{
   SP_UnlockIRQ(&memSched.freeState.lock, prevIRQL);
}

static INLINE Bool
MemSchedFreeStateIsLocked(void)
{
   return SP_IsLockedIRQ(&memSched.freeState.lock);
}

static INLINE MemSchedState
MemSchedCurrentState(void)
{
   return memSched.freeState.state;
}

static INLINE MemSchedStateTransition *
MemSchedCurrentStateTransition(void)
{
   return &memSched.freeState.table[memSched.freeState.state];
}

static Bool MemSchedIsDefaultAffinity(uint32 mask)
{
   return (mask & memSched.defaultNodeAffinity) == memSched.defaultNodeAffinity;
}

static INLINE MemSched_Client *
ClientFromWorld(const World_Handle *world)
{
   return &world->group->memsched;
}
     
static INLINE MemSchedVmm *
VmmClientFromWorld(const World_Handle *world)
{
   return &world->group->memsched.vmm;
}

static INLINE World_GroupInfo *
ClientToWorldGroup(const MemSched_Client *c)
{
   World_GroupInfo *group;
   group = (World_GroupInfo *)
           ((uintptr_t)c - offsetof(World_GroupInfo, memsched));
   ASSERT(&group->memsched == c);
   return group;
}

static INLINE World_GroupID
ClientGroupID(const MemSched_Client *c)
{
   return ClientToWorldGroup(c)->groupID;
}

static INLINE Bool
ClientResponsive(const MemSched_Client *c)
{
   return (c->vmm.valid && c->vmm.vmResponsive) || c->user.valid;
}

static INLINE Bool
ClientBalloonActive(const MemSched_Client *c)
{
   if (MEMSCHED_DEBUG_DISABLE_BALLOON) {
      return(FALSE);
   }
   return (c->vmm.valid && 
           c->vmm.memschedInfo->balloon.nOps > 0);
}

static INLINE uint32
ClientCurrentSize(const MemSched_Client *c)
{
   if (c->vmm.valid) {
      return c->vmm.usage.locked;
   } else {
      return c->user.usage.pageable;
   }
}

static INLINE uint32
ClientCurrentOverhead(const MemSched_Client *c)
{
   if (c->vmm.valid) {
      return c->vmm.usage.anon + c->vmm.usage.anonKern + 
             c->vmm.usage.overhead + c->user.usage.pageable + 
             c->user.usage.pinned;
   } else {
      return c->user.usage.pinned;
   }
}

static INLINE MemSchedVmmUsage *
VmmClientCurrentUsage(MemSched_Client *c)
{
   return &c->vmm.usage;
}

static INLINE MemSchedUserUsage *
UserClientCurrentUsage(MemSched_Client *c)
{
   return &c->user.usage;
}

static INLINE MemSched_Info *
VmmClientSharedData(const MemSchedVmm *vmm)
{
   ASSERT(vmm->memschedInfo != NULL);
   return vmm->memschedInfo;
}

static INLINE Bool
ClientEarlyShouldWait(const MemSched_Client *c)
{
   const MemSchedVmm *vmm = &c->vmm;
   // wait if memory low and vmm not yet started
   if (vmm->valid && !vmm->vmmStarted) {
      return(MemSched_MemoryIsLow());
   } else {
      return(FALSE);
   }
}

static INLINE Bool
ClientCanWait(const MemSched_Client *c)
{
   // don't block console OS
   if (CpuSched_IsHostWorld()) {
      return(FALSE);
   }

   if (!c->vmm.valid) {
      return(FALSE);
   }

   // OK to block
   return(TRUE);
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedReallocReqFast --
 *
 *      Requests a new memory reallocation on bottom half.
 *
 * Results:
 *      None. 
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
MemSchedReallocReqFast(void)
{
   BH_SetGlobal(memSched.bhNum);
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedReallocReqSlow --
 *
 *      Requests a new memory reallocation by memsched world.
 *      Caller must hold memsched lock.
 *
 * Results:
 *      None. 
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
MemSchedReallocReqSlow(void)
{
   ASSERT(MemSchedIsLocked());
   memSched.reallocGen++;
   MemSchedWakeup();
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedFindNonResponsiveClients --
 *
 *      There are times when VMs are un-responsive to our swap requests.
 *      We ignore these clients as they may cause us to reallocate more
 *      physical memory than we can reclaim.
 *              Caller must hold MemSched lock
 *
 * Results:
 *      none.
 *
 * Side effects:        
 *      none.
 *
 *----------------------------------------------------------------------
 */
#define MEMSCHED_MAX_SWAP_REQ_TIME_MSEC (15000)  // 15 sec
static void
MemSchedFindNonResponsiveClients(void)
{
   MemSched *m = &memSched;
   ASSERT(MemSchedIsLocked());
   m->allClientsResponsive = TRUE;
   FORALL_MEMSCHED_VMM_CLIENTS(&m->schedQueue, c, vmm) {
      uint64 curTimeStamp = Timer_SysUptime();
      vmm->vmResponsive = TRUE;
      if (vmm->swapReqTimeStamp != 0 && (vmm->swapReqTimeStamp < curTimeStamp)) {
         uint64 mSecDiff = curTimeStamp - vmm->swapReqTimeStamp;
         if ( mSecDiff > MEMSCHED_MAX_SWAP_REQ_TIME_MSEC) {
            vmm->vmResponsive = FALSE;
            m->allClientsResponsive = FALSE;
         }
      }
   } MEMSCHED_VMM_CLIENTS_DONE;
}


/*
 *----------------------------------------------------------------------
 *
 * MemSchedTaxToCost --
 *
 *      Returns the scaled idle:used cost ratio corresponding to
 *	the specified "taxRate" percentage.  Requires "taxRate"
 *	to be in the range [0, 99].
 *
 * Results:
 *      Returns the scaled idle:used cost ratio corresponding to
 *	the specified "taxRate" percentage.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static uint32
MemSchedTaxToCost(uint32 taxRate)
{
   uint32 costScaled;

   // sanity check
   ASSERT(taxRate <= 99);

   // convert tax% to scaled cost factor
   costScaled = (100 << MEMSCHED_COST_SCALE_SHIFT) / (100 - taxRate);

   // debugging
   if (MEMSCHED_DEBUG_TAX) {
      LOG(0, "tax=%u%%, costScaled=%u, costInt=%u",
         taxRate, costScaled, costScaled >> MEMSCHED_COST_SCALE_SHIFT);
   }

   return(costScaled);
}

/*
 *----------------------------------------------------------------------
 * MemSchedMinFree --
 *
 * Results:
 *      Returns the number of pages should be kept free in the system.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE uint32
MemSchedMinFree(void)
{
   return memSched.freeState.table[MEMSCHED_STATE_SOFT].highPages;
}

/*
 *----------------------------------------------------------------------
 * MemSchedLowFree --
 *
 * Results:
 *      Returns the number of free pages when the system is considered
 *      low in memory.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE uint32
MemSchedLowFree(void)
{
   return memSched.freeState.table[MEMSCHED_STATE_HARD].lowPages;
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedFreePages --
 *
 * Results:
 *      Returns the number of pages currently available for allocation.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE int32
MemSchedFreePages(void)
{
   return MemMap_UnusedPages() - MemSchedMinFree();
}

int32
MemSched_FreePages(void)
{
   return MemSchedFreePages();
}


/*
 * MemSched operations
 */

/*
 *----------------------------------------------------------------------
 *
 * MemSchedClientProcDebugRead --
 *
 *      Callback for read operation on "mem/debug" procfs node.
 *
 * Results: 
 *      Returns VMK_OK.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
MemSchedClientProcDebugRead(Proc_Entry *entry,
                            char       *buffer,
                            int        *len)
{
   MemSched_Client *c = (MemSched_Client *) entry->private;
   
   *len = 0;

   if (c->vmm.valid) {
      MemSchedVmmUsage *vmmUsage = VmmClientCurrentUsage(c);
      Alloc_PageInfo *pageInfo = &Alloc_AllocInfo(c->vmm.world)->vmPages;
      // format vmm memory subsystem info
      Proc_Printf(buffer, len,
                  "\n"
                  "alloc: phys=%u overhead=%u anon=%u\n"
                  "vmm usage:\n"
                  "  locked=%u cow=%u zero=%u cowHint=%u swapped=%u\n"
                  "  overhead=%u anon=%u anonKern=%u\n",
                  PagesToKB(pageInfo->numPhysPages),
                  PagesToKB(pageInfo->cosVmxInfo.numOverheadPages),
                  PagesToKB(pageInfo->numAnonPages),
                  PagesToKB(vmmUsage->locked),
                  PagesToKB(vmmUsage->cow),
                  PagesToKB(vmmUsage->zero),
                  PagesToKB(vmmUsage->cowHint),
                  PagesToKB(vmmUsage->swapped),
                  PagesToKB(vmmUsage->overhead),
                  PagesToKB(vmmUsage->anon),
                  PagesToKB(vmmUsage->anonKern));
   }

   if (c->user.valid) {
      MemSchedUserUsage *userUsage = UserClientCurrentUsage(c);
      // format userworld memory subsystem info
      Proc_Printf(buffer, len,
                  "uw reservation : reserved(min)=%u mapped(max)=%u\n"
                  "uw usage: pageable=%u cow=%u swapped=%u pinned=%u\n",
                  PagesToKB(c->user.reserved),
                  PagesToKB(c->user.mapped),
                  PagesToKB(userUsage->pageable),
                  PagesToKB(userUsage->cow),
                  PagesToKB(userUsage->swapped),
                  PagesToKB(userUsage->pinned));
      Proc_Printf(buffer, len,
                  "uw va space: mapped=%u kernel=%u shared=%u uncounted=%u\n",
                  PagesToKB(userUsage->virtualPageCount[MEMSCHED_MEMTYPE_MAPPED]),
                  PagesToKB(userUsage->virtualPageCount[MEMSCHED_MEMTYPE_KERNEL]),
                  PagesToKB(userUsage->virtualPageCount[MEMSCHED_MEMTYPE_SHARED]),
                  PagesToKB(userUsage->virtualPageCount[MEMSCHED_MEMTYPE_UNCOUNTED]));
   }

   if (c->vmm.valid) {
      char affinityBuf[MEMSCHED_AFFINITY_BUF_LEN];
      MemSched_Info *info = VmmClientSharedData(&c->vmm);
      int i;

      // format balloon info
      Proc_Printf(buffer, len,
                  "balloon:\n"
                  "  target(vmk)=%u target(vmm)=%u size=%u nOps=%u guestType=%u\n",
                  c->vmm.balloonTarget,
                  info->balloon.target,
                  info->balloon.size,
                  info->balloon.nOps,
                  info->balloon.guestType);

      // format mem sampling info
      Proc_Printf(buffer, len,
                  "\n"
                  "sampling: period    size  history nextEst nextAvg\n"
                  "         %7u %7u %7u %7u %7u\n",
                  info->sample.period,
                  info->sample.size,
                  info->sample.history,
                  info->sample.stats.nextEstimate,
                  info->sample.stats.nextAvg);

      Proc_Printf(buffer, len,
                  "history fastAvg slowAvg estimate\n");

      for (i = 0; i < info->sample.history; i++) {
         Proc_Printf(buffer, len,
                     "%7u %7u %7u  %7u\n",
                     i,
                     info->sample.stats.fastAvg[i],
                     info->sample.stats.slowAvg[i],
                     info->sample.stats.estimate[i]);
      }
                       
      MemSchedColorListFormat(c->colorsAllowed,
                              affinityBuf,
                              MEMSCHED_AFFINITY_BUF_LEN);
      Proc_Printf(buffer, len, "\n\nColorsAllowed:  %s\n", affinityBuf);
   }
   
   // everything OK
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedClientProcStatusRead --
 *
 *      Callback for read operation on "mem/status" procfs node.
 *
 * Results: 
 *      Returns VMK_OK.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
MemSchedClientProcStatusRead(Proc_Entry *entry,
                             char       *buffer,
                             int        *len)
{
   MemSched_Client *c = (MemSched_Client *) entry->private;
   *len = 0;

   // format header, data, message
   MemSchedStatusHeaderFormat(FALSE, buffer, len);
   MemSchedClientStatusFormat(c, FALSE, buffer, len);

   // everything OK
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedClientProcMinRead --
 *
 *      Callback for read operation on "mem/min" procfs node.
 *
 * Results: 
 *      Returns VMK_OK.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
MemSchedClientProcMinRead(Proc_Entry *entry,
                          char       *buffer,
                          int        *len)
{
   MemSched_Client *c = (MemSched_Client *) entry->private;
   *len = 0;

   // format buffer
   Proc_Printf(buffer, len, "%u\n", PagesToMB(c->alloc.min));

   // everything OK
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedSetMemMinInt --
 *
 *      Sets the min memory for the memsched client.
 *
 *      - Decreasing the min can fail if there isn't enough swap
 *      - Increasing the min can fail is there isn't enough unreserved
 *        memory
 *
 * Results: 
 *      Returns a VMK_ReturnStatus value
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
MemSchedSetMemMinInt(MemSched_Client *c, uint32 allocPages, Bool autoMin)
{
   int32 availMem, reservedMem, autoMinMem, availSwap, reservedSwap;
   Bool swapEnabled;
   
   // enforce valid allocation range
   if (allocPages > c->alloc.max) {
      ClientWarn(c, "invalid min: %u pages", allocPages);
      return(VMK_BAD_PARAM);
   }

   // acquire lock
   MemSchedLock();

   // obtain reserved memory, swap totals
   swapEnabled = Swap_IsEnabled();
   MemSchedReservedMem(swapEnabled, &availMem, &reservedMem, &autoMinMem);
   MemSchedReservedSwap(swapEnabled, &availSwap, &reservedSwap);
   availSwap = MAX(0, availSwap);
   if (c->alloc.autoMin) {
      ASSERT(autoMinMem >= c->alloc.min);
      autoMinMem -= c->alloc.min;
   }

   // perform admission control check
   if (allocPages > c->alloc.min) {
      // increasing min: ensure sufficient unreserved memory
      int32 deltaReserveMem;

      // determine pages needed, available
      deltaReserveMem = allocPages - c->alloc.min;

      // debugging
      ClientDebug(c, "check memory: avail=%dK, automin=%dK, need=%dK",
                  PagesToKB(availMem),
                  PagesToKB(autoMinMem),
                  PagesToKB(deltaReserveMem));

      // perform check
      if (availMem < deltaReserveMem) {
         int32 needMem, reclaimMem;

         // can reclaim other auto-min pages, limited by swap space
         needMem = deltaReserveMem - availMem;
         reclaimMem = MIN(autoMinMem, availSwap);

         if (reclaimMem < needMem) {
            // fail: unlock, warn, return error
            MemSchedUnlock();
            ClientWarn(c, "insufficient memory: "
                       "avail=%dK (%dK + %dK), need=%dK",
                       PagesToKB(availMem + reclaimMem),
                       PagesToKB(availMem),
                       PagesToKB(reclaimMem),
                       PagesToKB(deltaReserveMem));
            return(VMK_NO_MEMORY);
         }
      }
   } else if (allocPages < c->alloc.min) {
      // decreasing min: ensure sufficient unreserved swap
      int32 deltaReserveSwap;

      // determine swap needed, available
      deltaReserveSwap = c->alloc.min - allocPages;

      // debugging
      ClientDebug(c, "check swap: avail=%dK, need=%dK",
                  PagesToKB(availSwap), PagesToKB(deltaReserveSwap));

      // perform check
      if (availSwap < deltaReserveSwap) {
         // fail: unlock, warn, return error
         MemSchedUnlock();
         ClientWarn(c, "insufficient swap: avail=%dK, need=%dK",
                    PagesToKB(availSwap), PagesToKB(deltaReserveSwap));
         return(VMK_NO_MEMORY);
      }
   }

   // update min alloc
   c->alloc.min = allocPages;
   c->alloc.autoMin = autoMin;

   // request reallocation
   MemSchedReallocReqSlow();

   // release lock
   MemSchedUnlock();

   // debugging
   ClientDebug(c, "set min=%u pages", allocPages);

   // everything OK
   return(VMK_OK);
}


/*
 *----------------------------------------------------------------------
 *
 * MemSched_SetMemMin --
 *
 *      Sets the min memory for the memsched client of the world.
 *
 * Results: 
 *      Returns a VMK_ReturnStatus value
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
MemSched_SetMemMin(World_Handle *world, uint32 allocPages, Bool autoMin)
{
   return MemSchedSetMemMinInt(ClientFromWorld(world), allocPages, autoMin);
}


/*
 *----------------------------------------------------------------------
 *
 * MemSchedClientProcMinWrite --
 *
 *      Callback for write operation on "mem/min" procfs node.
 *
 * Results: 
 *      Returns VMK_OK if successful, otherwise VMK_BAD_PARAM.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
MemSchedClientProcMinWrite(Proc_Entry *entry,
                           char       *buffer,
                           int        *len)
{
   MemSched_Client *c = (MemSched_Client *) entry->private;
   uint32 allocMB, allocPages;

   // parse value from buffer
   if (Parse_Int(buffer, *len, &allocMB) != VMK_OK) {
      return(VMK_BAD_PARAM);
   }

   // convert MB to pages
   allocPages = MBToPages(allocMB);

   return MemSchedSetMemMinInt(c, allocPages, FALSE);
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedParseShares --
 *
 *      Parses "buf" as a memory shares value.  The special values
 *	"high", "normal", and "low" are converted into appropriate
 *	corresponding numeric values based on "sizeMB".
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
MemSchedParseShares(const char *buf, uint32 sizeMB, uint32 *shares)
{
   // sanity check
   ASSERT(sizeMB <= VMMEM_MAX_SIZE_MB);

   // parse special values: high, normal, low
   if (strcmp(buf, "high") == 0) {
      *shares = MEMSCHED_SHARES_HIGH(sizeMB);
      return(VMK_OK);
   } else if (strcmp(buf, "normal") == 0) {
      *shares = MEMSCHED_SHARES_NORMAL(sizeMB);
      return(VMK_OK);
   } else if (strcmp(buf, "low") == 0) {
      *shares = MEMSCHED_SHARES_LOW(sizeMB);
      return(VMK_OK);
   }

   // parse numeric value
   return(Parse_Int(buf, strlen(buf), shares));
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedClientProcSharesRead --
 *
 *      Callback for read operation on world's memory shares.
 *
 * Results: 
 *      Returns VMK_OK.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
MemSchedClientProcSharesRead(Proc_Entry *entry,
                             char       *buffer,
                             int        *len)
{
   MemSched_Client *c = (MemSched_Client *) entry->private;
   *len = 0;
   
   // format buffer
   Proc_Printf(buffer, len, "%u\n", c->alloc.shares);
   
   // everything OK
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedClientProcSharesWrite --
 *
 *      Callback for write operation on world's memory shares.
 *
 * Results: 
 *      Returns VMK_OK if successful, otherwise VMK_BAD_PARAM.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
MemSchedClientProcSharesWrite(Proc_Entry *entry,
                              char       *buffer,
                              int        *len)
{
   MemSched_Client *c = (MemSched_Client *) entry->private;
   uint32 sizeMB, shares;
   char *argv[2];
   int argc;
   
   // parse buffer into args (assumes OK to overwrite)
   argc = Parse_Args(buffer, argv, 2);
   if (argc != 1) {
      ClientWarn(c, "invalid shares: unable to parse");
      return(VMK_BAD_PARAM);
   }

   // snapshot client memory size
   MemSchedLock();
   sizeMB = PagesToMB(c->alloc.max);
   MemSchedUnlock();

   // parse value from buffer
   if (MemSchedParseShares(buffer, sizeMB, &shares) != VMK_OK) {
      ClientWarn(c, "invalid shares: unable to parse");
      return(VMK_BAD_PARAM);
   }   

   // ensure value within legal range
   if ((shares < MEMSCHED_SHARES_MIN) || (shares > MEMSCHED_SHARES_MAX)) {
      ClientWarn(c, "invalid shares: %u", shares);
      return(VMK_BAD_PARAM);
   }

   // update shares, request reallocation
   MemSchedLock();
   c->alloc.shares = shares;
   MemSchedReallocReqSlow();   
   MemSchedUnlock();
   
   // everything OK
   return(VMK_OK);
}


/*
 *----------------------------------------------------------------------
 *
 * MemSchedClientProcSwapRead --
 *
 *      Callback for read operation on "mem/swap" procfs node.
 *
 * Results: 
 *      Returns VMK_OK.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
MemSchedClientProcSwapRead(Proc_Entry *entry,
                           char       *buffer,
                           int        *len)
{
   MemSched_Client *c = (MemSched_Client *) entry->private;
   *len = 0;
   if (c->vmm.valid) {
      // format header, data, message
      Swap_VmmGroupStatsHeaderFormat(buffer, len);
      Swap_VmmGroupStatsFormat(c->vmm.world, buffer, len);
   }

   // everything OK
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * MemSched_NodeAffinityMask --
 *
 *      Returns the current memory node affinity mask for "world".
 *
 * Results:
 *      Returns the current memory node affinity mask for "world".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
uint32
MemSched_NodeAffinityMask(World_Handle *world)
{
   return ClientFromWorld(world)->nodeAffinityMask;
}


/*
 *-----------------------------------------------------------------------------
 *
 * MemSched_HasNodeHardAffinity --
 *
 *     Returns TRUE iff the world has "hard" memory affinity set
 *
 * Results:
 *     Returns TRUE iff the world has "hard" memory affinity set
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
Bool
MemSched_HasNodeHardAffinity(World_Handle *world)
{
   MemSched_Client *c = ClientFromWorld(world);
   return (c->hardAffinity);
}

/*
 *-----------------------------------------------------------------------------
 *
 * MemSched_AllowedColors --
 *
 *      Returns the list of cache colors allowed for this world
 *
 * Results:
 *      Returns the list of cache colors allowed for this world
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
MemSched_ColorVec*
MemSched_AllowedColors(World_Handle *world)
{
   MemSched_Client *c = ClientFromWorld(world);
   return (c->colorsAllowed);
}

/*
 *----------------------------------------------------------------------
 *
 * MemMaskFormat --
 *
 *      Writes memory node numbers represented by "mask" into "buf",
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
MemMaskFormat(uint32 mask, char *buf, int maxLen, char separator)
{
   int i, len, nNodes;
   Bool first;

   // initialize
   nNodes = NUMA_GetNumNodes();
   first = TRUE;
   buf[0] = '\0';
   len = 0;

   // format each bit in mask
   for (i = 0; i < nNodes; i++) {
      if (mask & (1 << i)) {
         if (first) {
	    len += snprintf(buf+len, maxLen-len, "%d", i);
            first = FALSE;
         } else {
	    len += snprintf(buf+len, maxLen-len, "%c%d", separator, i);
         }
	 len = MIN(len, maxLen);
      }
   }

   // ensure that the mask is always null-terminated
   buf[maxLen-1] = '\0';
   return(len);
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedClientProcAffinityRead --
 *
 *      Callback for read operation on world's memory affinity.
 *
 * Results: 
 *      Returns VMK_OK.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
MemSchedClientProcAffinityRead(Proc_Entry *entry,
                               char       *buffer,
                               int        *len)
{
   MemSched_Client *c = (MemSched_Client *) entry->private;
   MemSched *m = &memSched;
   char affinityBuf[MEMSCHED_AFFINITY_BUF_LEN];
   uint32 affinity;

   // initialize
   *len = 0;
   
   // snapshot affinity
   if (c->hardAffinity) {
      affinity = c->nodeAffinityMask;
   } else {
      // should not show soft affinity to user
      affinity = m->defaultNodeAffinity;
   }

   // format affinity
   (void) MemMaskFormat(affinity, affinityBuf,  MEMSCHED_AFFINITY_BUF_LEN, ',');
   Proc_Printf(buffer, len, "%s\n", affinityBuf);

   // everything OK
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedClientProcAffinityWrite --
 *
 *      Callback for write operation on world's memory affinity.
 *
 * Results: 
 *      Returns VMK_OK if successful, otherwise VMK_BAD_PARAM.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
MemSchedClientProcAffinityWrite(Proc_Entry *entry,
                                char       *buffer,
                                int        *len)
{
   MemSched_Client *c = (MemSched_Client *) entry->private;
   MemSchedVmm *vmm = &c->vmm;
   VMK_ReturnStatus status;
   MemSched *m = &memSched;
   uint32 affinity;
   char *badToken;
   int nNodes;
   
   // sanity check
   if (!vmm->valid) {
      return(VMK_BAD_PARAM);
   }

   // handle changes to cache color affinity
   if (!strncmp(buffer, "colors", sizeof("colors")-1)) {
      char *argv[2];
      int argc = Parse_Args(buffer, argv, 2);
      if (argc < 2) {
         Warning("invalid affinity command");
         return (VMK_BAD_PARAM);
      }

      MemSchedLock();
      if (c->colorsAllowed != MEMSCHED_COLORS_ALL) {
         World_Free(c->vmm.world, c->colorsAllowed);
      }
      c->colorsAllowed = MemSchedParseColorList(c->vmm.world, argv[1]);
      MemSchedUnlock();
      
      LOG(1, "set new cache color affinity: (%s)", argv[1]);
      return (VMK_OK);
   }
   
   // parse buffer as bitmask of memory nodes
   nNodes = NUMA_GetNumNodes();
   status = Parse_IntMask(buffer, nNodes, &affinity, &badToken);
   if (status != VMK_OK) {
      if (badToken == NULL) {
         ClientWarn(c, "invalid set affinity");
         return(status);
      } else if ((strcmp(badToken, "default") == 0) ||
                 (strcmp(badToken, "all") == 0)) {
         // special case: single argument "default" or "all"
         affinity = m->defaultNodeAffinity;
      } else if (strcmp(badToken, "migrate") == 0) {
         MemSched_NumaMigrateVMM(vmm->world);
      } else {
         ClientWarn(c, "invalid set affinity: arg=%s", badToken);
         return(status);
      }
   }

   // sanity check: ensure non-zero mask
   if (affinity == 0 || (affinity & m->defaultNodeAffinity) == 0) {
      ClientWarn(c, "invalid affinity mask=0x%x", affinity);
      return(VMK_BAD_PARAM);
   }

   MemSched_SetNodeAffinity(vmm->world, affinity, TRUE);

   // request configuration update
   Action_Post(vmm->world, vmm->remapConfigAction);

   
   // everything OK
   return(VMK_OK);
}


/*
 *-----------------------------------------------------------------------------
 *
 * MemSchedSetNodeAffinityInt --
 *
 *     Assigns a new memory affinity to the world group of "world."
 *     If "forced" is TRUE, we recompute the memory affinity.
 *     If "forced" is FALSE, we update the affinity only if it's not hard.
 *
 *     Meaning of "hardAffinity":
 *        If TRUE, then this is user-set and user-visible affinity, 
 *        otherwise it is internal "soft" affinity.
 * 
 * Results:
 *     Return TRUE if affinMask is updated, FALSE otherwise.
 *
 * Side effects:
 *     Changes memsched alloc affinity assocated with the world group. 
 *
 *-----------------------------------------------------------------------------
 */
static Bool
MemSchedSetNodeAffinityInt(MemSched_Client *c, uint32 affinMask, Bool forced)
{
   ASSERT(MemSchedIsLocked());
   ASSERT((affinMask & memSched.defaultNodeAffinity) != 0);

   /*
    * If the change is forced, we update the affinity mask and 
    * recompute hardAffinity.
    */
   if (UNLIKELY(forced)) {
      c->nodeAffinityMask = affinMask;
      c->hardAffinity = !MemSchedIsDefaultAffinity(affinMask);
   } else {
      // update affinMask if it's not already hard
      if (!c->hardAffinity) {
         c->nodeAffinityMask = affinMask;
      } else {
         return FALSE;
      }
   }
   return TRUE;
}

/*
 *-----------------------------------------------------------------------------
 *
 * MemSched_SetNodeAffinity --
 *
 *     Assigns a new memory affinity to the world group of "world."
 *     Call MemSchedSetNodeAffinityInt() to set affinity.
 * 
 * Results:
 *     None
 *
 * Side effects:
 *     Changes memsched alloc affinity assocated with the world group. 
 *
 *-----------------------------------------------------------------------------
 */
void
MemSched_SetNodeAffinity(struct World_Handle *world, uint32 affinMask, Bool forced)
{
   MemSched_Client *c = ClientFromWorld(world);

   MemSchedLock();

   if (!MemSchedSetNodeAffinityInt(c, affinMask, forced)) {
      VMLOG(0, world->worldID, 
            "cannot set soft affinity on world with hard affinity set");
   }

   if (World_IsVMMWorld(world)) {
      MemSchedVmm *vmm = &c->vmm;
      ASSERT(vmm->valid);
      if (vmm->vmmStarted) {
         MemSched_RemapInfo *info = &VmmClientSharedData(vmm)->remap;
         info->migrateNodeMask = MemSched_NodeAffinityMask(world);
      }
   }

   MemSchedUnlock();
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedClientProcPShareRead --
 *
 *      Callback for read operation for world's page sharing status.
 *
 * Results: 
 *      Returns VMK_OK.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
MemSchedClientProcPShareRead(Proc_Entry *entry,
                             char       *buffer,
                             int        *len)
{
   MemSched_Client *c = (MemSched_Client *) entry->private;
   MemSchedVmm *vmm = &c->vmm;
   MemSched_PShareInfo *info;
   PShare_MonitorStats *stats;
   Alloc_Info *allocInfo;
   *len = 0;
  
   if (!vmm->valid) {
      return VMK_BAD_PARAM;
   }

   info = &VmmClientSharedData(vmm)->pshare;
   stats = &info->stats;

   allocInfo = Alloc_AllocInfo(vmm->world);
 
   // format info
   Proc_Printf(buffer, len,
               "enable     %6d\n"
               "debug      %6d\n"
               "scanRate   %6u\n"
               "checkRate  %6u\n"
               "\n"
               "nScan      %6u\n"
               "nAttempt   %6u\n"
               "nCOW       %6u\n"
               "nHint      %6u\n"
               "nShare     %6u\n"
               "nCopy      %6u\n"
               "\n"
               "nCheck     %6u\n"
               "nBad       %6u\n"
               "nBadCOW    %6u\n"
               "nBadKey    %6u\n"
               "nBadMPN    %6u\n"
               "\n"
               "p2mTotal   %6u\n"
               "p2mPeak    %6u\n"
               "hintTotal  %6u\n"
               "hintPeak   %6u\n",
               info->enable,
               info->debug,
               info->scanRate,
               info->checkRate,
               stats->nScan,
               stats->nAttempt,
               stats->nCOW,
               stats->nHint,
               stats->nShare,
               stats->nCopy,
               stats->nCheck,
               stats->nCheckBad,
               stats->nCheckBadCOW,
               stats->nCheckBadKey,
               stats->nCheckBadMPN,
               allocInfo->p2mUpdateTotal,
               allocInfo->p2mUpdatePeak,
               allocInfo->hintUpdateTotal,
               allocInfo->hintUpdatePeak);
   
   // everything OK
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedClientProcRemapRead --
 *
 *      Callback for read operation for world's page remapping status.
 *
 * Results: 
 *      Returns VMK_OK.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
MemSchedClientProcRemapRead(Proc_Entry *entry,
                            char       *buffer,
                            int        *len)
{
   MemSched_Client *c = (MemSched_Client *) entry->private;
   MemSchedVmm *vmm = &c->vmm;
   MemSched_RemapInfo *info;
   MemSched_RemapStats *stats;
   *len = 0;
  
   if (!vmm->valid) {
      return VMK_BAD_PARAM;
   }
 
   info = &VmmClientSharedData(vmm)->remap;
   stats = &info->stats;

   // format info
   Proc_Printf(buffer, len,
               "type       remapped  attempts\n"
               "low        %8u  %8u\n"
               "migrate    %8u  %8u\n"
               "recolor    %8u  %8u\n"
               "\n"
               "periods    %8u\n"
               "pickups    %8u\n"
               "scans      %8u\n"
               "stops      %8u\n",
               stats->vmkRemap, stats->vmkAttempt,
               stats->migrateRemap, stats->migrateAttempt,
               stats->recolorRemap, stats->recolorAttempt,
               stats->period,
               stats->pickup,
               stats->scan,
               stats->stop);
   
   // everything OK
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedClientProcMigRateRead --
 *
 *      Callback for read operation for world's page migration rate.
 *
 * Results: 
 *      Returns VMK_OK.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
MemSchedClientProcMigRateRead(Proc_Entry *entry,
                              char       *buffer,
                              int        *len)
{
   MemSched_Client *c = (MemSched_Client *) entry->private;
   MemSchedVmm *vmm = &c->vmm;
   MemSched_RemapInfo *info;
   *len = 0;
  
   if (!vmm->valid) {
      return VMK_BAD_PARAM;
   }

   info = &VmmClientSharedData(vmm)->remap;
 
   // format info
   Proc_Printf(buffer, len, "%u\n", info->migrateScanRate);
   
   // everything OK
   return(VMK_OK);
}


/*
 *-----------------------------------------------------------------------------
 *
 * MemSchedSetMigRateInt --
 *
 *     Actually assigns a new migration rate to the world whose memsched
 *     client is "c"
 *
 * Results:
 *     Returns VMK_OK on success, VMK_BAD_PARAM if client or rate invalid
 *
 * Side effects:
 *     Changes page migration rate
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
MemSchedSetMigRateInt(MemSched_Client *c, uint32 rate)
{
   MemSchedVmm *vmm = &c->vmm;
   MemSched_RemapInfo *info;

   // sanity check
   if (!vmm->valid) {
      return(VMK_BAD_PARAM);
   }

   info = &VmmClientSharedData(vmm)->remap;

   if (rate > MEMSCHED_MIGRATE_RATE_MAX) {
      ClientWarn(c, "invalid rate: %u", rate);
      return(VMK_BAD_PARAM);
   }

   // OK to write unlocked (uint32)
   info->migrateScanRate = rate;

   // request configuration update
   Action_Post(vmm->world, vmm->remapConfigAction);

   return (VMK_OK);
}

// exported wrapper for above function
VMK_ReturnStatus
MemSched_SetMigRate(World_Handle *world, uint32 rate)
{
   return MemSchedSetMigRateInt(ClientFromWorld(world), rate);
}



/*
 *-----------------------------------------------------------------------------
 *
 * MemSched_GetMigRate --
 *
 *     Returns the current page migration rate for this world
 *
 * Results:
 *     Returns the current page migration rate for this world
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
uint32
MemSched_GetMigRate(const World_Handle *world)
{
   MemSchedVmm *vmm = VmmClientFromWorld(world);
   return VmmClientSharedData(vmm)->remap.migrateScanRate;
}

                    
/*
 *----------------------------------------------------------------------
 *
 * MemSchedClientProcMigRateWrite --
 *
 *      Callback for write operation on world's page migration rate.
 *
 * Results: 
 *      Returns VMK_OK if successful, otherwise VMK_BAD_PARAM.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
MemSchedClientProcMigRateWrite(Proc_Entry *entry,
                               char       *buffer,
                               int        *len)
{
   MemSched_Client *c = (MemSched_Client *) entry->private;
   uint32 rate;
   VMK_ReturnStatus res;
   
   if (!c->vmm.valid) {
      return VMK_BAD_PARAM;
   }
 
   // parse value from buffer
   if (Parse_Int(buffer, *len, &rate) != VMK_OK) {
      return(VMK_BAD_PARAM);
   }

   res = MemSchedSetMigRateInt(c, rate);
   if (res != VMK_OK) {
      LOG(0, "failed to set migrate rate");
   }
   return (res);
}


/*
 *----------------------------------------------------------------------
 *
 * MemSchedClientAddProcEntries --
 *
 *      Add client-specific proc entries exported by the memory
 *	scheduler.
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
MemSchedClientAddProcEntries(World_Handle *world)
{
   MemSched_Client *c = ClientFromWorld(world);
   // "mem" directory
   Proc_InitEntry(&c->procMemDir);
   c->procMemDir.parent = &world->procWorldDir;
   Proc_Register(&c->procMemDir, "mem", TRUE);

   // "mem/status" entry
   Proc_InitEntry(&c->procStatus);
   c->procStatus.parent = &c->procMemDir;
   c->procStatus.read = MemSchedClientProcStatusRead;
   c->procStatus.private = c;
   Proc_Register(&c->procStatus, "status", FALSE);
   
   // "mem/min" entry
   Proc_InitEntry(&c->procMin);
   c->procMin.parent = &c->procMemDir;
   c->procMin.read = MemSchedClientProcMinRead;
   c->procMin.write = MemSchedClientProcMinWrite;
   c->procMin.private = c;
   Proc_Register(&c->procMin, "min", FALSE);

   // "mem/shares" entry
   Proc_InitEntry(&c->procShares);
   c->procShares.parent = &c->procMemDir;
   c->procShares.read = MemSchedClientProcSharesRead;
   c->procShares.write = MemSchedClientProcSharesWrite;
   c->procShares.private = c;
   Proc_Register(&c->procShares, "shares", FALSE);
   
   // "mem/affinity" entry
   Proc_InitEntry(&c->procAffinity);
   c->procAffinity.parent = &c->procMemDir;
   c->procAffinity.read = MemSchedClientProcAffinityRead;
   c->procAffinity.write = MemSchedClientProcAffinityWrite;
   c->procAffinity.private = c;
   Proc_Register(&c->procAffinity, "affinity", FALSE);

   // "mem/pshare" entry, if enabled
   Proc_InitEntry(&c->procPShare);
   if (PShare_IsEnabled()) {
      c->procPShare.parent = &c->procMemDir;
      c->procPShare.read = MemSchedClientProcPShareRead;
      c->procPShare.private = c;
      Proc_Register(&c->procPShare, "pshare", FALSE);
   }

   // "mem/remap" entry
   Proc_InitEntry(&c->procRemap);
   c->procRemap.parent = &c->procMemDir;
   c->procRemap.read = MemSchedClientProcRemapRead;
   c->procRemap.private = c;
   Proc_Register(&c->procRemap, "remap", FALSE);

   // "mem/migrate-rate" entry, if NUMA
   Proc_InitEntry(&c->procMigRate);
   if (NUMA_GetNumNodes() > 1) {
      c->procMigRate.parent = &c->procMemDir;
      c->procMigRate.read = MemSchedClientProcMigRateRead;
      c->procMigRate.write = MemSchedClientProcMigRateWrite;
      c->procMigRate.private = c;
      Proc_Register(&c->procMigRate, "migrate-rate", FALSE);
   }

   // "mem/swap" entry
   Proc_InitEntry(&c->procSwap);
   c->procSwap.parent = &c->procMemDir;
   c->procSwap.read = MemSchedClientProcSwapRead;
   c->procSwap.private = c;
   Proc_Register(&c->procSwap, "swap", FALSE);
   
   // "mem/debug" hidden entry
   Proc_InitEntry(&c->procDebug);
   c->procDebug.parent = &c->procMemDir;
   c->procDebug.read = MemSchedClientProcDebugRead;
   c->procDebug.private = c;
   Proc_RegisterHidden(&c->procDebug, "debug", FALSE);
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedClientRemoveProcEntries --
 *
 *      Remove client-specific proc entries exported by the memory
 *	scheduler.
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
MemSchedClientRemoveProcEntries(MemSched_Client *c)
{
   // remove "debug" entry
   Proc_Remove(&c->procDebug);

   // remove main entries
   Proc_Remove(&c->procSwap);
   Proc_Remove(&c->procMigRate);
   Proc_Remove(&c->procRemap);
   Proc_Remove(&c->procPShare);
   Proc_Remove(&c->procAffinity);
   Proc_Remove(&c->procShares);
   Proc_Remove(&c->procMin);   
   Proc_Remove(&c->procStatus);

   // remove "mem" directory
   Proc_Remove(&c->procMemDir);
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedClientBalloonUpdateMax -- 
 *
 *      Update maximum balloon size for "c" based on guest OS limits.
 *
 * Results:
 *	May modify "c".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
MemSchedClientBalloonUpdateMax(MemSched_Client *c)
{
   MemSchedVmm *vmm = &c->vmm;
   uint32 max;

   // initialize
   max = 0;

   // enforce limits if driver active
   if (ClientBalloonActive(c)) {
      if (vmm->balloonMaxCfg >= 0) {
         // use explicitly-configured limit
         max = MIN(c->alloc.max, vmm->balloonMaxCfg);
      } else {
         // compute limit based on global config options
         MemSched_BalloonInfo *b = &VmmClientSharedData(vmm)->balloon;
         uint32 percent, limit;

         // compute percentage-based limit
         percent = CONFIG_OPTION(MEM_CTL_MAX_PERCENT);
         ASSERT(percent < 100);
         max = (percent * c->alloc.max) / 100;

         // enforce OS-specific limit, if any
         switch (b->guestType) {
         case BALLOON_GUEST_WINDOWS_NT4:
            // enforce NT4 limit
            limit = MBToPages(CONFIG_OPTION(MEM_CTL_MAX_NT4));
            max = MIN(max, limit);
            break;
         case BALLOON_GUEST_WINDOWS_NT5:
            // enforce NT5 limit
            limit = MBToPages(CONFIG_OPTION(MEM_CTL_MAX_NT5));
            max = MIN(max, limit);
            break;
         case BALLOON_GUEST_LINUX:
            // enforce Linux limit
            limit = MBToPages(CONFIG_OPTION(MEM_CTL_MAX_LINUX));
            max = MIN(max, limit);
            break;
         case BALLOON_GUEST_BSD:
            // enforce BSD limit
            limit = MBToPages(CONFIG_OPTION(MEM_CTL_MAX_BSD));
            max = MIN(max, limit);
            break;
         default:
            // no known limit
            break;
         }
      }
   }

   // update max
   if (vmm->balloonMax != max) {
      ClientLOG(c, "updated maxmemctl %uM -> %uM",
                PagesToMB(vmm->balloonMax),
                PagesToMB(max));
      vmm->balloonMax = max;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedClientBalloonSet -- 
 *
 *      Sets target size of balloon associated with client "c"
 *	to "nPages".
 *
 * Results:
 *      None. 
 *
 * Side effects:
 *      Updates shared area, posts action to inform monitor of change.
 *
 *----------------------------------------------------------------------
 */
static void
MemSchedClientBalloonSet(MemSched_Client *c, uint32 nPages)
{
   MemSchedVmm *vmm = &c->vmm;
   MemSched_Info *info = VmmClientSharedData(vmm);

   // debugging
   if (MEMSCHED_DEBUG_VERBOSE) {
      ClientDebug(c, "old=%u: balloon=%u", vmm->balloonTarget, nPages);
   }

   // done if target unchanged
   if (vmm->balloonTarget == nPages) {
      return;
   }

   // update world balloon target
   vmm->balloonTarget = nPages;

   // reflect update in shared area
   info->balloon.target = vmm->balloonTarget;

   // post action to inform monitor
   Action_Post(vmm->world, vmm->balloonAction);
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedClientSwapSet -- 
 *
 *      Sets target number of swapped pages for client "c" to "nPages".
 *      Caller must hold memsched lock.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      Updates shared area, informs swap daemon of change.
 *
 *----------------------------------------------------------------------
 */
static void
MemSchedClientSwapSet(MemSched_Client *c, uint32 nPages)
{
   MemSchedVmm *vmm = &c->vmm;

   ASSERT(MemSchedIsLocked());

   if (VMK_STRESS_RELEASE_OPTION(MEM_SWAP) && Swap_IsEnabled()) {
      // choose atleast MEMSCHED_SWAP_STRESS_MIN pages to swap.
      vmm->swapTarget = MAX(MEMSCHED_SWAP_STRESS_MIN, 
                            ((int)c->snapshot.locked - (int)c->alloc.min));
   } else {
      // update world swap target
      vmm->swapTarget = nPages;
   }

   // inform swapper
   Swap_SetSwapTarget(vmm->world, vmm->swapTarget);
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedClientUpdateSwap -- 
 *
 *      Adjusts target number of swapped pages for client "c"
 *	to reflect any adjustments needed as a result of
 *	COW or overhead changes between reallocations.
 *
 * Results:
 *      None. 
 *
 * Side effects:
 *      Updates shared area, informs swap daemon of change.
 *
 *----------------------------------------------------------------------
 */
static void
MemSchedClientUpdateSwap(MemSched_Client *c)
{
   MemSchedVmm *vmm = &c->vmm;
   MemSchedVmmUsage *vmmUsage = VmmClientCurrentUsage(c);
   uint32 swapAdjusted = vmmUsage->swapped;
   uint32 cowUsage = vmmUsage->cow;
   uint32 lockedUsage = vmmUsage->locked;
   uint32 curOverhead = ClientCurrentOverhead(c);

   ASSERT(Swap_IsEnabled());

   // extra swap required for COW copies
   if (cowUsage < c->snapshot.cow) {
      swapAdjusted += c->snapshot.cow - cowUsage;
   }

   // extra swap required for overhead allocations
   if (curOverhead > c->snapshot.overhead) {
      swapAdjusted += curOverhead - c->snapshot.overhead;
   }

   // extra swap required for locked pages 
   // +1 is for the page which is being requested.
   if ((lockedUsage + 1 ) > c->commit.alloc) {
      swapAdjusted += lockedUsage - c->commit.alloc;
   }

   // optimization: perform initial check w/o locking
   if (swapAdjusted > vmm->swapTarget) {
      // acquire lock
      MemSchedLock();
      if (swapAdjusted > vmm->swapTarget) {
         vmm->swapTarget = swapAdjusted;
         // inform swapper
         Swap_SetSwapTarget(vmm->world, swapAdjusted);
      }
      
      // release lock
      MemSchedUnlock();

      // debugging, causes excess spew when low on memory
      if (!VMK_STRESS_RELEASE_OPTION(MEM_SWAP) && MEMSCHED_DEBUG_VERBOSE) {
         ClientDebug(c, " target=%dK", PagesToKB(vmm->swapTarget));
      }
   }
}



static void
MemSchedClientConfigNodeAffinity(MemSched_Client *c, uint32 affinityMask)
{
   MemSched *m = &memSched;

   if (affinityMask == 0) {
      return;
   }

   if (c->hardAffinity == TRUE) {
      if (c->nodeAffinityMask != affinityMask) {
         Warning("memory affinity mask config failed: old 0x%x new 0x%x", 
                 c->nodeAffinityMask, affinityMask);
      }
      return;
   }
  
   if ((affinityMask & m->defaultNodeAffinity) == 0) {
      Warning("memory affinity mask 0x%x is invalid, using default affinity instead", 
              affinityMask);
      return;
   }
   if (!MemSchedIsDefaultAffinity(affinityMask)) {
      c->nodeAffinityMask = affinityMask;
      c->hardAffinity = TRUE;
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * MemSchedParseColorList --
 *
 *      Converts the string "colorList" to its corresponding MemSched_ColorVec
 *      representation as an array of permissible colors. Allocates memory
 *      for the color vec, unless all colors are permitted, in which case
 *      MEMSCHED_COLORS_ALL is returned.
 *
 * Results:
 *      Returns MemSched_ColorVec representation of "colorList"
 *
 * Side effects:
 *      May allocate memory.
 *
 *-----------------------------------------------------------------------------
 */
static MemSched_ColorVec *
MemSchedParseColorList(World_Handle *world, const char *colorStr)
{
   MemSched_ColorVec *vec;
   uint32 numColors, i;
   char *colorList;

   if (strcmp(colorStr, "all") == 0) {
      return (MEMSCHED_COLORS_ALL);
   }
   if (MemMap_GetNumColors() > MEMSCHED_MAX_SUPPORTED_COLORS) {
      LOG(0, "processor has more colors than memsched supports, ignoring affinity");
      return (MEMSCHED_COLORS_ALL);
   }

   colorList = Mem_Alloc(SCHED_COLORAFFINITY_LEN+2);
   if (colorList == NULL) {
      Warning("no memory to parse color affinity");
      return (MEMSCHED_COLORS_ALL);
   }
   
   // semicolon-terminate the list (for Parse_RangeList's sake)
   if (snprintf(colorList, SCHED_COLORAFFINITY_LEN+2, "%s;", colorStr) >= SCHED_COLORAFFINITY_LEN+2) {
      Log("color list (%s) too long", colorStr);
      Mem_Free(colorList);
      return (MEMSCHED_COLORS_ALL);
   }

   vec = World_Alloc(world, sizeof(MemSched_ColorVec));
   if (vec == NULL) {
      Warning("no memory to parse color affinity");
      Mem_Free(colorList);
      return (MEMSCHED_COLORS_ALL);
   }
   memset(vec, 0, sizeof(vec));

   // use a really inefficient way of parsing this list of colors   
   numColors = MemMap_GetNumColors();
   for (i=0; i < numColors; i++) {
      if (Parse_RangeList(colorList, i)) {
         vec->colors[vec->nColors] = i;
         vec->nColors++;
      }
   }

   Mem_Free(colorList);

   if (vec->nColors == 0) {
      Log("no valid colors in mask: (%s)", colorList);
      Mem_Free(vec);
      return (MEMSCHED_COLORS_ALL);
   }
   
   // more efficient to return "all" if every color covered
   if (vec->nColors >= numColors) {
      Mem_Free(vec);
      return (MEMSCHED_COLORS_ALL);
   }

   return (vec);
}

/*
 *-----------------------------------------------------------------------------
 *
 * MemSchedColorListFormat --
 *
 *      Fills in "buf" with a string representation of "colors," up
 *      to "maxLen" characters.
 *
 * Results:
 *      Returns the number of characters written.
 *
 * Side effects:
 *      Writes to buf.
 *
 *-----------------------------------------------------------------------------
 */
static int
MemSchedColorListFormat(const MemSched_ColorVec *cvec,
                         char *buf,
                         int maxLen)
{
   int c=0;
   unsigned i=0;
   uint8 prevColor, rangeStart;

   if (cvec == MEMSCHED_COLORS_ALL) {
      c = snprintf(buf, maxLen, "%s", "all");
      if (c >= maxLen) {
         buf[maxLen-1] = '\0';
      }
      return c;
   }
   
   // invalid to have 0 colors in mask
   ASSERT(cvec->nColors != 0);

   rangeStart = cvec->colors[0];
   prevColor = cvec->colors[0];

   // print out a list of color ranges allowed
   for (i=1; i < cvec->nColors+1; i++) {
      uint8 thisColor=0;
      if (i < cvec->nColors) {
         thisColor = cvec->colors[i];
      }
      if (thisColor != prevColor + 1 || i == cvec->nColors) {
         // this range has ended, so display it
         if (rangeStart != prevColor) {
            c += snprintf(buf+c,maxLen-c,"%u-%u,", rangeStart, prevColor);
         } else {
            c += snprintf(buf+c,maxLen-c, "%u,", prevColor);
         }
         
         rangeStart = thisColor;
      }
      
      prevColor = thisColor;
   }
   

   // null-terminate the string
   if (c < maxLen) {
      buf[c] = 0;
      c++;
   } else {
      buf[maxLen-1] = 0;
   }
   
   return c;
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedInitVmmWorld --
 *
 *      Perform vmm specific operation when adding "world" to set of 
 *      worlds managed by memory scheduler.
 *
 *	Sets memory allocation parameters to values specified in
 *	"config"; default values are used for parameters specified
 *	as SCHED_CONFIG_NONE.
 *
 * Results:
 *      Returns TRUE iff world added to memory scheduler.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
MemSchedInitVmmWorld(World_Handle *world, 
		     const Sched_MemClientConfig *memConfig,
		     const Sched_GroupConfig *groupConfig)
{
   MemSched *m = &memSched;
   MemSched_Client *c = ClientFromWorld(world);
   MemSchedVmm *vmm = &c->vmm;
   MemSched_Info *info = VmmClientSharedData(vmm);
   MemSched_Alloc vmmAlloc;
   uint32 memShares;
   World_ID worldID = world->worldID;
   VMK_ReturnStatus status;

   ASSERT(World_IsVmmLeader(world));

   // create memory start action, fail if unable
   vmm->startAction = Action_Alloc(world, "MemMonStarted");
   if (vmm->startAction == ACTION_INVALID) {
      VmWarn(worldID, "unable to allocate memory start action");
      return VMK_NO_RESOURCES;
   }
   
   // create numa migrate action, fail if unable
   vmm->numaMigrateAction = Action_Alloc(world, "NumaMig");
   if (vmm->numaMigrateAction == ACTION_INVALID) {
      VmWarn(worldID, "unable to allocate numa migrate action");
      return VMK_NO_RESOURCES;
   }

   // create balloon monitor action, fail if unable
   vmm->balloonAction = Action_Alloc(world, "MemBalloon");
   if (vmm->balloonAction == ACTION_INVALID) {
      VmWarn(worldID, "unable to allocate balloon action");
      return VMK_NO_RESOURCES;
   }

   // create memory sampling monitor action, fail if unable   
   vmm->sampleAction = Action_Alloc(world, "MemSample");
   if (vmm->sampleAction == ACTION_INVALID) {
      VmWarn(worldID, "unable to allocate sampling action");
      return VMK_NO_RESOURCES;
   }

   // create page sharing monitor action, fail if unable
   vmm->pshareAction = Action_Alloc(world, "COWConfig");
   if (vmm->pshareAction == ACTION_INVALID) {
      VmWarn(worldID, "unable to allocate page sharing action");
      return VMK_NO_RESOURCES;
   }

   // create remap config monitor action, fail if unable
   vmm->remapConfigAction = Action_Alloc(world, "RemapConfig");
   if (vmm->remapConfigAction == ACTION_INVALID) {
      VmWarn(worldID, "unable to allocate remap config action");
      return VMK_NO_RESOURCES;
   }

   MemSchedLock();

   MemSchedAllocInit(&vmmAlloc, &groupConfig->mem);

   status = MemSchedAdmit(world, memConfig->resuming, &vmmAlloc);
   if (status != VMK_OK) {
       MemSchedUnlock();
       return status;
   }

   // save old memory allocation 
   vmm->preAlloc = c->alloc;
   // set new memory allocation
   c->alloc = vmmAlloc;

   // operation will succeed

   // associate enclosing world
   vmm->world = world;

   // Is page sharing enabled for this world?
   vmm->pshareEnable = memConfig->pShare;

   // default page sharing parameters
   info->pshare.enable = (vmm->pshareEnable && m->shareEnable);
   info->pshare.scanRate = m->shareScanRate;
   info->pshare.checkRate = m->shareCheckRate;
   info->pshare.debug = FALSE;

   // memory sampling parameters
   vmm->samplePeriod = m->samplePeriod;
   vmm->sampleSize = m->sampleSize;
   vmm->sampleHistory = m->sampleHistory;
   info->sample.period = vmm->samplePeriod;
   info->sample.size = vmm->sampleSize;
   info->sample.history = vmm->sampleHistory;

   // set shares alloc parameter, handle missing/special values
   if (SCHED_CONFIG_SHARES_SPECIAL(groupConfig->mem.shares)) {
      switch (groupConfig->mem.shares) {
      case SCHED_CONFIG_SHARES_LOW:
         memShares = MEMSCHED_SHARES_LOW(PagesToMB(c->alloc.max));
         break;
      case SCHED_CONFIG_SHARES_HIGH:
         memShares = MEMSCHED_SHARES_HIGH(PagesToMB(c->alloc.max));
         break;
      case SCHED_CONFIG_SHARES_NORMAL:
      default:
         memShares = MEMSCHED_SHARES_NORMAL(PagesToMB(c->alloc.max));
      }
   } else {
      memShares = groupConfig->mem.shares;
   }
   c->alloc.shares = MIN(memShares, MEMSCHED_SHARES_MAX);

   // set affinityMask alloc parameter
   MemSchedClientConfigNodeAffinity(c, memConfig->nodeAffinity);

   // handle memory color affinity
   c->colorsAllowed = MemSchedParseColorList(world, memConfig->colorAffinity);
   
   // initial page remapping parameters
   info->remap.migrateNodeMask = c->nodeAffinityMask;
   info->remap.migrateScanRate = 0;

   // configured max balloon size, if any
   if ((memConfig->maxBalloon == SCHED_CONFIG_NONE) ||
       (memConfig->maxBalloon < 0)) {
      // no explicitly-specified value
      vmm->balloonMaxCfg = SCHED_CONFIG_NONE;
   } else {
      // use explicitly-specified value
      vmm->balloonMaxCfg = MBToPages(memConfig->maxBalloon);
   }

   // actual max balloon size, zero until active
   vmm->balloonMax = 0;

   vmm->valid = TRUE;

   MemSchedUpdatePShareRate();
   MemSchedReallocReqSlow();

   MemSchedUnlock();

   // post initial actions
   Action_Post(world, vmm->startAction);
   Action_Post(world, vmm->sampleAction);
   Action_Post(world, vmm->pshareAction);
   Action_Post(world, vmm->remapConfigAction);

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedInitUserWorld --
 *
 *      Perform userworld specific operation when adding "world" to 
 *      set of worlds managed by memory scheduler.
 *
 *	Sets memory allocation parameters to values specified in
 *	"config"; default values are used for parameters specified
 *	as SCHED_CONFIG_NONE.
 *
 * Results:
 *      Returns TRUE iff world added to memory scheduler.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
MemSchedInitUserWorld(World_Handle *world, 
		      const Sched_MemClientConfig *memConfig,
		      const Sched_GroupConfig *groupConfig)
{
   MemSched_Client *c = ClientFromWorld(world);
   MemSchedUser *user = &c->user;

   MemSchedLock();

   MemSchedAllocInit(&c->alloc, &groupConfig->mem);
   // no autoMin for userworld programs
   c->alloc.autoMin = FALSE;

   // set total mapped memory limit
   user->mapped = c->alloc.max;

   // XXX need to perform admission control

   // set affinityMask alloc parameter
   MemSchedClientConfigNodeAffinity(c, memConfig->nodeAffinity);

   user->valid = TRUE;

   // request reallocation
   MemSchedReallocReqSlow();

   MemSchedUnlock();

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * MemSched_WorldGroupInit --
 *
 *      Assign memory schedule client to the world group.
 *      Initialize memsched data for the world group.
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
MemSched_WorldGroupInit(World_Handle *world, World_InitArgs *args)
{
   MemSched *m = &memSched;
   MemSched_Client *c = ClientFromWorld(world);

   if (!(World_IsVMMWorld(world) || 
         World_IsUSERWorld(world))) {
      return;
   }

   // initialize to default data
   c->hardAffinity = FALSE;
   c->nodeAffinityMask = memSched.defaultNodeAffinity;
   c->colorsAllowed = MEMSCHED_COLORS_ALL;
   
   MemSchedLock();
   List_Insert(&c->link, &m->schedQueue);
   m->numScheds++;
   MemSchedUnlock();

   // initialize userworld group leader data
   if (World_IsUSERWorld(world)) {
      MemSchedInitUserWorld(world, &args->sched->mem, &args->sched->group);
   }

   MemSchedClientAddProcEntries(world);
}

/*
 *----------------------------------------------------------------------
 *
 * MemSched_WorldGroupCleanup --
 *
 *      Cleanup world group memsched data.
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
MemSched_WorldGroupCleanup(World_Handle *world)
{
   MemSched *m = &memSched;
   MemSched_Client *c = ClientFromWorld(world);

   if (!(World_IsVMMWorld(world) || 
         World_IsUSERWorld(world))) { 
      return;
   }

   MemSchedClientRemoveProcEntries(c);

   MemSchedLock();

   // remove from scheduler queue
   List_Remove(&c->link);
   m->numScheds--;
   ASSERT(m->numScheds >= 0);
   MemSchedUpdatePShareRate();
   MemSchedReallocReqSlow();

   MemSchedUnlock();
}


/*
 *----------------------------------------------------------------------
 *
 * MemSched_WorldInit --
 *
 *      Initialize "world" specific memsched client structure.
 *
 * Results:
 *      Returns TRUE iff world initialization successful.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
MemSched_WorldInit(World_Handle *world, World_InitArgs *args)
{
   MemSched_Client *c = ClientFromWorld(world);

   if (World_IsVmmLeader(world)) {
      c->vmm.memschedInfo = SharedArea_Alloc(world,
                                             "memschedInfo",
                                             sizeof(MemSched_Info));
      return MemSchedInitVmmWorld(world, 
		                  &args->sched->mem, 
				  &args->sched->group);
   }

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * MemSchedCleanupVmmWorld --
 *
 *      Remove vmm "world" from set of worlds managed by memory scheduler.
 *
 * Results:
 *      Returns TRUE iff world removed from memory scheduler.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
MemSchedCleanupVmmWorld(World_Handle *world)
{
   MemSched_Client *c = ClientFromWorld(world);
   MemSchedVmm *vmm = &c->vmm;

   ASSERT(World_IsVmmLeader(world));
   ASSERT(vmm->valid);

   // acquire lock
   MemSchedLock();

   /*
    * Restore old memsched allocation.
    */ 
   c->alloc = vmm->preAlloc;

   // mark invalid, request reallocation
   vmm->valid = FALSE;
   MemSchedUpdatePShareRate();
   MemSchedReallocReqSlow();

   if (c->colorsAllowed != MEMSCHED_COLORS_ALL) {
      World_Free(c->vmm.world, c->colorsAllowed);
   }
   
   // release lock
   MemSchedUnlock();

   // successful
   return;
}


/*
 *----------------------------------------------------------------------
 *
 * MemSched_WorldCleanup --
 *
 *      Cleanup world specific memsched data.
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
MemSched_WorldCleanup(World_Handle *world)
{
   if (World_IsVmmLeader(world)) {
      MemSchedCleanupVmmWorld(world);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * MemSched_ClientVmmUsage --
 *
 *      Retrieve the VMM usage structure.
 *
 *      This function must be called from worlds managed by memory
 *      scheduler.
 *
 * Results: 
 *      Returns the vmm usage structure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
MemSchedVmmUsage * 
MemSched_ClientVmmUsage(const World_Handle *world)
{
   MemSched_Client *c = ClientFromWorld(world);
   return VmmClientCurrentUsage(c);
}


/*
 *----------------------------------------------------------------------
 *
 * MemSched_ClientUserUsage --
 *
 *      Retrieve the userworld usage structure. 
 *
 * Results: 
 *      Returns the userworld usage structure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
MemSchedUserUsage * 
MemSched_ClientUserUsage(const World_Handle *world)
{
   MemSched_Client *c = ClientFromWorld(world);
   return UserClientCurrentUsage(c);
}


/*
 *----------------------------------------------------------------------
 *
 * MemSchedReserveMemInt --
 * MemSched_ReserveMem --
 *
 *      Attempts to increase the number of anon/overhead pages
 *      reserved for "world" by "pageDelta".
 *
 * Results: 
 *      Returns VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
MemSchedReserveMemInt(World_Handle *world, uint32 pageDelta)
{
   Bool autoMinReclaim = FALSE;
   Bool swapEnabled = Swap_IsEnabled();
   MemSched_Client *c = ClientFromWorld(world);
   int32 avail, reserved, autoMin;
   Sched_Group *group;

   ASSERT(MemSchedIsLocked());

   // obtain reserved memory totals
   MemSchedReservedMem(swapEnabled, &avail, &reserved, &autoMin);

   // not enough memory available
   if (avail < pageDelta) {
      // check if we can reserve memory by reclaiming automin
      if (avail + autoMin > pageDelta) {
         autoMinReclaim = TRUE;
         VMLOG(1, world->worldID, 
               "reclaim automin: avail=%u requested=%d autoMin=%d\n", 
               avail, pageDelta, autoMin);
      } else {
         return(VMK_NO_MEMORY);
      }
   }

   // Increase owning sched group size by amount of overhead growth
   Sched_TreeLock();
   group = Sched_TreeLookupGroup(world->group->schedGroupID);
   if (MemSchedIncClientGroupSize(group, pageDelta, pageDelta) != VMK_OK) {
      Sched_TreeUnlock();
      return (VMK_NO_MEMORY);
   }
   Sched_TreeUnlock();

   // update overhead allocation
   c->overhead += pageDelta;

   if (autoMinReclaim) {
      // request to adjust memory allocation 
      MemSchedReallocReqFast();
   }

   // everything OK
   return(VMK_OK);
}

VMK_ReturnStatus
MemSched_ReserveMem(World_Handle *world, uint32 pageDelta)
{
   VMK_ReturnStatus status;

   MemSchedLock();
   status = MemSchedReserveMemInt(world, pageDelta);
   MemSchedUnlock();

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * MemSchedUnreserveMemInt --
 * MemSched_UnreserveMem --
 *
 *      Reduce the number of anon/overhead pages reserved for "world" 
 *      by "pageDelta".
 *
 * Results: 
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
MemSchedUnreserveMemInt(World_Handle *world, uint32 pageDelta)
{
   Sched_Group *group;
   MemSched_Client *c = ClientFromWorld(world);

   ASSERT(MemSchedIsLocked());

   // Decrease owning sched group size by amount of overhead shrinkage
   Sched_TreeLock();
   group = Sched_TreeLookupGroup(world->group->schedGroupID);
   MemSchedDecClientGroupSize(group, pageDelta, pageDelta);
   Sched_TreeUnlock();

   // update overhead allocation
   ASSERT(c->overhead >= pageDelta);
   c->overhead -= pageDelta;
}

void
MemSched_UnreserveMem(World_Handle *world, uint32 pageDelta)
{
   MemSchedLock();
   MemSchedUnreserveMemInt(world, pageDelta);
   MemSchedUnlock();
}


/*
 *----------------------------------------------------------------------
 *
 * MemSched_SetUserOverhead --
 *
 *      Configure the overhead limit for the given userworld.
 *
 * Results:
 *      VMK_OK upon success, otherwise upon failure.
 *
 * Side effects:
 *      More or less memory reserved.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
MemSched_SetUserOverhead(World_Handle* world, uint32 numOverhead)
{
   MemSched_Client *c = ClientFromWorld(world);
   VMK_ReturnStatus status = VMK_OK;
   int32 memDelta;

   ASSERT(World_IsUSERWorld(world));
   MemSchedLock();

   // calculate the reserved memory difference
   memDelta = numOverhead - c->user.reserved;

   // reserve or unreserve the difference.
   if (memDelta > 0) {
      status = MemSchedReserveMemInt(world, memDelta);
   } else {
      MemSchedUnreserveMemInt(world, -memDelta);
   }

   // commit reserved memory change
   if (status == VMK_OK) {
      c->user.reserved = numOverhead;
      c->user.mapped += memDelta;
   }

   MemSchedUnlock();

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * MemSched_AdmitUserOverhead --
 *
 *      Check if we can add more pages to overhead memory.
 *
 * Results:
 *      TRUE upon success, FALSE upon failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Bool
MemSched_AdmitUserOverhead(const World_Handle *world, uint32 incPages)
{
   MemSched_Client *c = ClientFromWorld(world);
   const MemSchedUserUsage *userUsage = UserClientCurrentUsage(c);
   ASSERT(World_IsUSERWorld(world));
   /*
    * Either user reserved memory hasn't been configured or 
    * it's below limit.
    */ 
   return (c->user.reserved == 0 || 
           userUsage->pinned + incPages <= c->user.reserved);
}


/*
 *----------------------------------------------------------------------
 *
 * MemSched_AdmitUserMapped --
 *
 *      Check if we can add more pages to mapped memory.
 *
 * Results:
 *      TRUE upon success, FALSE upon failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Bool
MemSched_AdmitUserMapped(const World_Handle *world, uint32 incPages)
{
   MemSched_Client *c = ClientFromWorld(world);
   const MemSchedUserUsage *userUsage = UserClientCurrentUsage(c);
   ASSERT(World_IsUSERWorld(world));
   return (userUsage->virtualPageCount[MEMSCHED_MEMTYPE_MAPPED] + incPages <=
           c->user.mapped);
}


/*
 *----------------------------------------------------------------------
 *
 * MemSchedStatusHeaderFormat --
 *
 *      Writes client status header into "buf".  If "verbose" is set,
 *	the header includes additional detail fields.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Increments *len by number of characters written to "buf"
 *
 *----------------------------------------------------------------------
 */
static void
MemSchedStatusHeaderFormat(Bool verbose, char *buf, int *len)
{
   // basic header
   Proc_Printf(buf, len,
	       "   vm mctl? shares     min     max  active    size/sizetgt"
               "  memctl/mctltgt swapped/swaptgt    swapin"
               "    swapout cptread/cpt-tgt"
               "  shared   uwovhd/overhd/ovhdmax affinity");

   // verbose header
   if (verbose) {
      Proc_Printf(buf, len,
                  " |  mintgt  adjmin amin? rspd?  target mctlmax "
                  "    cow    zero xshared    hint "
		  "  charged cowSwapped     mpps est/slo/fst/nxt");
   }

   // newline
   Proc_Printf(buf, len, "\n");
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedClientStatusFormat --
 *
 *      Writes status information for client "c" into "buf".  If
 *	"verbose" is set, the status information includes additional
 *	details.
 *
 * Results:
 *      Increments "*len" by number of characters written to "buf".
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
MemSchedClientStatusFormat(MemSched_Client *c,
                           Bool verbose,
                           char *buf,
                           int *len)
{
   MemSchedVmm *vmm = &c->vmm;
   MemSchedUser *user = &c->user;
   MemSchedVmmUsage *vmmUsage = VmmClientCurrentUsage(c);
   MemSchedUserUsage *userUsage = UserClientCurrentUsage(c);
   uint32 cptRead = 0, cptTgt = 0, affinMask;
   uint32 numSwapPagesRead = 0, numSwapPagesWritten = 0, numCOWPagesSwapped = 0;
   char affinStr[MEMSCHED_AFFINITY_BUF_LEN];
   uint32 currentSize = ClientCurrentSize(c);
   Bool balloonActive = ClientBalloonActive(c);
      
   if (vmm->valid) {
      Swap_VMStats *swapVmStats = &ClientToWorldGroup(c)->vmm.swapInfo.stats;
      numSwapPagesRead = swapVmStats->numPagesRead;
      numSwapPagesWritten = swapVmStats->numPagesWritten;
      numCOWPagesSwapped = swapVmStats->numCOWPagesSwapped;
      Swap_GetCptStats(vmm->world, &cptTgt, &cptRead);
   }

   affinMask = c->hardAffinity ? c->nodeAffinityMask : memSched.defaultNodeAffinity;
   MemMaskFormat(affinMask, affinStr, MEMSCHED_AFFINITY_BUF_LEN, ',');

   // basic data
   Proc_Printf(buf, len,
               "%5d %-5s %6u %7u %7u %7u %7u/%7u "
               "%7u/%7u %7u/%7u %9u  %9u %7u/%7u "
               "%7u %7u/%7u/%7u %8s",
               ClientGroupID(c),
               balloonActive ? "yes" : "no",
               c->alloc.shares,
               PagesToKB(c->alloc.min),
               PagesToKB(c->alloc.max),
               PagesToKB(c->snapshot.touched),
               PagesToKB(currentSize),
               PagesToKB(c->commit.alloc),
               PagesToKB(c->snapshot.balloon),
               PagesToKB(vmm->balloonTarget),
               PagesToKB(c->snapshot.swapped),
               PagesToKB(vmm->swapTarget + user->swapTarget),
               PagesToKB(numSwapPagesRead),
               PagesToKB(numSwapPagesWritten),
               PagesToKB(cptRead),
               PagesToKB(cptTgt),
               PagesToKB(c->snapshot.cow),
               vmm->valid? PagesToKB(userUsage->pageable) : 0,
               PagesToKB(ClientCurrentOverhead(c)),
               PagesToKB(c->overhead),
               affinStr);

   // verbose data
   if (verbose) {
      Proc_Printf(buf, len,
                  " | %7u %7u %-5s %-5s %7u %7u "
                  "%7u %7u %7u %7u %8u %10u ",
                  PagesToKB(c->commit.minTarget),
                  PagesToKB(c->alloc.adjustedMin),
                  c->alloc.autoMin ? "yes" : "no",
                  ClientResponsive(c) ? "yes" : "no", 
                  PagesToKB(c->commit.target),
                  PagesToKB(vmm->balloonMax),
                  PagesToKB(c->snapshot.cow),
                  PagesToKB(c->snapshot.zero),
                  PagesToKB(c->snapshot.shared),
                  PagesToKB(vmmUsage->cowHint),
                  PagesToKB(c->commit.charged),
                  PagesToKB(numCOWPagesSwapped));
       
      if (c->commit.pps == MEMSCHED_PPS_MAX) {
         Proc_Printf(buf, len, "%8s ", "max");
      } else {
         Proc_Printf(buf, len, "%8Lu ", c->commit.pps / 1000);
      }

      // vm specific
      if (vmm->valid) {
         MemSched_Info *info = VmmClientSharedData(vmm);
         Proc_Printf(buf, len,
                     "%3u/%3u/%3u/%3u ",
                     info->sample.stats.estimate[0],
                     info->sample.stats.slowAvg[0],
                     info->sample.stats.fastAvg[0],
                     info->sample.stats.nextAvg);
      }
   } 

   // newline
   Proc_Printf(buf, len, "\n");
}


/*
 *----------------------------------------------------------------------
 *
 * MemSchedFreeStateFormat --
 *
 *      Writes free state status information for "s" into "buf".
 *
 * Results: 
 *      Increments "*len" by number of characters written to "buf".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
MemSchedFreeStateFormat(MemSchedFreeState *s, char *buf, int *len)
{
   int i;

   // format header
   Proc_Printf(buf, len,
               "state   "
               "  free<  low      count "
               "  free>  high     count\n");

   // format state transition table
   for (i = 0; i < MEMSCHED_STATES_MAX; i++) {
      MemSchedStateTransition *t = &s->table[i];
      Proc_Printf(buf, len,
                  "%-6s  "
                  "%7u  %-6s  %6u "
                  "%7u  %-6s  %6u\n",
                  MemSchedStateToString(t->state),
                  PagesToKB(t->lowPages),
                  MemSchedStateToString(t->lowState),
                  t->lowCount,
                  PagesToKB(t->highPages),
                  MemSchedStateToString(t->highState),
                  t->highCount);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * MemSchedPrintSwapStats --
 *      
 *      If buffer is NULL ans len is NULL, Logs swap stats for all memsched 
 *      clients, else writes the swap stats to the proc node and saves the 
 *      buffer length used into "*len".
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------
 */
static void
MemSchedPrintSwapStats(char *buffer, int *len)
{
   MemSched *m = &memSched;
   Swap_VmmGroupStatsHeaderFormat(buffer, len);

   FORALL_MEMSCHED_VMM_CLIENTS(&m->schedQueue, c, vmm) {
      Swap_VmmGroupStatsFormat(vmm->world, buffer, len);
   } MEMSCHED_VMM_CLIENTS_DONE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * MemSched_LogSwapStats --
 *      
 *      Logs swap stats for all memsched clients.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------
 */
void
MemSched_LogSwapStats(void)
{
   MemSchedPrintSwapStats(NULL, NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * MemSchedProcRead --
 *
 *      Callback for read operation on "/proc/vmware/sched/mem"
 *	procfs node.
 *
 * Results: 
 *      Returns VMK_OK.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
MemSchedProcRead(Proc_Entry *entry,
                 char       *buffer,
                 int        *len)
{
   Bool verbose = (entry->private == 0) ? FALSE : TRUE;
   MemSched *m = &memSched;
   int32 availSwap, reservedSwap, availMem, reservedMem, autoMinMem;
   Bool swapEnabled = Swap_IsEnabled();

   uint32 totalAlloc = 0, totalTarget = 0;
   uint32 totalBalloonTarget = 0, totalSwapTarget = 0, totalMinTarget = 0;
   uint32 totalMin = 0, totalAdjustedMin = 0, totalMax = 0;
   uint32 totalOverhead = 0;
   uint32 totalSize = 0, totalShared = 0, totalCharged = 0, totalSampled = 0;
   uint32 totalSwapped = 0, totalTouched = 0;
   uint32 totalBalloon = 0, totalBalloonMax = 0;
   uint32 totalCptRead = 0, totalCptTgt = 0;
   uint32 totalCow = 0, totalCowHint = 0, totalZero = 0;
   uint32 totalVMOvhd = 0, totalUWOvhd = 0;
   uint32 totalSwapRead = 0, totalSwapWritten = 0, totalCOWSwapped = 0;

   // initialize buffer length
   *len = 0;

   // acquire lock
   MemSchedLock();

   // obtain reserved memory, swap totals
   MemSchedReservedMem(swapEnabled, &availMem, &reservedMem, &autoMinMem);
   MemSchedReservedSwap(swapEnabled, &availSwap, &reservedSwap);

   // verbose info
   if (verbose) {
      Proc_Printf(buffer, len,
		  "%8u ReallocFastCount\n"
		  "%8u ReallocSlowCount\n"
                  "%8u TriggerCount\n"
                  "%8u CptMaxOvhd\n\n",
		  m->reallocFastCount,
		  m->reallocSlowCount,
                  m->freeState.triggerCount,
                  m->maxCptInvalidOvhdPages);
   }

   // format totals
   Proc_Printf(buffer, len,
               "%8d Managed\n"
               "%8d Kernel\n"
               "%8d Free\n"
	       "%8d MinFree\n"
	       "%8d Excess\n"
               "%8s Status\n\n"
               "%8d MemReserved\n"
               "%8d MemAvailable\n"
               "%8d MemAutoMin\n"
               "%8d SwapReserved\n"
               "%8d SwapAvailable\n\n",
               PagesToKB(MemMap_ManagedPages()),
               PagesToKB(MemMap_KernelPages()),
               PagesToKB(MemMap_UnusedPages()),
	       PagesToKB(MemSchedMinFree()),
               PagesToKB(MemSchedFreePages()),
               MemSchedStateToString(MemSchedCurrentState()),
               PagesToKB(reservedMem),
               PagesToKB(availMem),
               PagesToKB(autoMinMem),
               PagesToKB(reservedSwap),
               PagesToKB(availSwap));
   
   // format header
   MemSchedStatusHeaderFormat(verbose, buffer, len);

   // format client data
   FORALL_MEMSCHED_CLIENTS(&m->schedQueue, c) {
      MemSchedVmm *vmm  = &c->vmm;
      MemSchedUser *user = &c->user;
      
      if (vmm->valid) {
         MemSchedVmmUsage *vmmUsage = VmmClientCurrentUsage(c);
         MemSchedUserUsage *userUsage = UserClientCurrentUsage(c);
         Swap_VMStats *swapStats = &ClientToWorldGroup(c)->vmm.swapInfo.stats;
         uint32 cptRead, cptTgt;

         // update totals
         totalBalloonMax += vmm->balloonMax;
         totalSampled += (c->alloc.max - c->snapshot.balloon);
         totalBalloonTarget += vmm->balloonTarget;
         Swap_GetCptStats(vmm->world, &cptTgt, &cptRead);
         totalCptRead += cptRead;
         totalCptTgt += cptTgt;
         totalSwapRead += swapStats->numPagesRead;
         totalSwapWritten += swapStats->numPagesWritten;
         totalCOWSwapped += swapStats->numCOWPagesSwapped;
         totalCowHint += vmmUsage->cowHint;
         totalVMOvhd += ClientCurrentOverhead(c);
         totalUWOvhd += userUsage->pageable;
      }

      // update totals
      totalMin += c->alloc.min;
      totalMax += c->alloc.max;
      totalAdjustedMin += c->alloc.adjustedMin;
      totalOverhead += c->overhead;

      totalSize += c->snapshot.locked;
      totalBalloon += c->snapshot.balloon;
      totalCow += c->snapshot.cow;
      totalZero += c->snapshot.zero;
      totalShared  += c->snapshot.shared;
      totalBalloon += c->snapshot.balloon;
      totalSwapped += c->snapshot.swapped;
      totalTouched += c->snapshot.touched;

      totalAlloc += c->commit.alloc;
      totalTarget += c->commit.target;
      totalCharged += c->commit.charged;
      totalMinTarget += c->commit.minTarget;
      totalSwapTarget += vmm->swapTarget + user->swapTarget;
         
      MemSchedClientStatusFormat(c, verbose, buffer, len);
   } MEMSCHED_CLIENTS_DONE;

   // standard totals
   Proc_Printf(buffer, len,
 	       "TOTAL    NA     NA %7u %7u %7u %7u/%7u "
               "%7u/%7u %7u/%7u %9u  %9u %7u/%7u "
               "%7u %7u/%7u/%7u       NA",
	       PagesToKB(totalMin),
	       PagesToKB(totalMax),
               PagesToKB(totalTouched),
	       PagesToKB(totalSize),
               PagesToKB(totalAlloc),
               PagesToKB(totalBalloon),
               PagesToKB(totalBalloonTarget),
               PagesToKB(totalSwapped),
               PagesToKB(totalSwapTarget),
               PagesToKB(totalSwapRead),
               PagesToKB(totalSwapWritten),
               PagesToKB(totalCptRead),
               PagesToKB(totalCptTgt),
               PagesToKB(totalCow),
               PagesToKB(totalUWOvhd),
               PagesToKB(totalVMOvhd),
               PagesToKB(totalOverhead));

   // verbose totals
   if (verbose) {
      Proc_Printf(buffer, len,
                  " | %7u %7u  NA   %3s   %7u %7u "
                  "%7u %7u %7u %7u "
                  " NA/ NA/ NA/ NA %8u %10u "
                  "      NA",
                  PagesToKB(totalMinTarget),
                  PagesToKB(totalAdjustedMin),
                  ((m->allClientsResponsive) ? "yes" : "no"),
                  PagesToKB(totalTarget),
                  PagesToKB(totalBalloonMax),
                  PagesToKB(totalCow),
                  PagesToKB(totalZero),
                  PagesToKB(totalShared),
                  PagesToKB(totalCowHint),
                  PagesToKB(totalCharged),
                  PagesToKB(totalCOWSwapped));
   }

   // newline
   Proc_Printf(buffer, len, "\n");

   // format state transition info
   if (verbose) {
      Proc_Printf(buffer, len, "\n");      
      MemSchedFreeStateFormat(&m->freeState, buffer, len);
   }

   // format swap stats
   if (verbose) {
      Proc_Printf(buffer, len, "\n");      
      MemSchedPrintSwapStats(buffer, len);
   }

   // release lock
   MemSchedUnlock();

   // everything OK
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedProcWrite --
 *
 *      Callback for write operation on "/proc/vmware/sched/mem"
 *	procfs node.  Any write causes a reallocation to be
 *	performed.
 *
 * Results: 
 *      Returns VMK_OK.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
MemSchedProcWrite(UNUSED_PARAM(Proc_Entry *entry),
                  char *buffer,
                  int  *len)
{
   // debugging
   if (MEMSCHED_DEBUG) {
      LOG(0, "realloc initiated");
   }

   // "realloc" => force reallocation 
   if (strncmp(buffer, "realloc", 7) == 0) {
      MemSchedLock();
      MemSchedReallocReqSlow();
      MemSchedUnlock();
      return(VMK_OK);
   }

   // invalid command
   Warning("invalid command: %s", buffer);
   return(VMK_BAD_PARAM);
}

/*
 *----------------------------------------------------------------------
 *
 * MemSched_EarlyInit --
 *
 *      Partially initialize memory scheduler module so that
 *      it can be called from other modules.
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
MemSched_EarlyInit(void)
{
   MemSched *m = &memSched;
   MemSchedFreeState *s = &memSched.freeState;

   // zero global state
   memset(m, 0, sizeof(MemSched));

   // initialize non-zero values
   SP_InitLock("MemSchedLock", &m->lock, SP_RANK_MEMSCHED);

   // initialize mem sched clients and queues
   List_Init(&m->schedQueue);

   // initialize rng state
   m->nodeStressSeed = 42;

   // set state transition threshold to max range
   s->lowThreshold = 0;
   s->highThreshold = -1U;
}

/*
 *----------------------------------------------------------------------
 *
 * MemSched_Init --
 *
 *      Initializes the memory scheduler module.
 *
 * Results:
 *      None. 
 *
 * Side effects:
 *      Internal state is initialized.
 *
 *----------------------------------------------------------------------
 */
void
MemSched_Init(Proc_Entry *procSchedDir)
{
   MemSched *m = &memSched;
   uint32 managedPages;
   NUMA_Node n;

   // register memsched commit BH handler
   m->bhNum = BH_Register(MemSchedReallocBHHandler, NULL);

   // initialize total system swap
   m->totalSystemSwap = 0;		// start off with nothing!

   // initialize free state
   managedPages = MemMap_ManagedPages();
   MemSchedFreeStateInit(&m->freeState, managedPages);

   // initialize reallocation threshold
   m->reallocPages = (managedPages / 100) * MEMSCHED_BALANCE_DELTA_PCT;
   if (MEMSCHED_DEBUG) {
      LOG(0, "reallocPages=%d (%dK)",
          m->reallocPages,
          PagesToKB(m->reallocPages));
   }

   // initial configuration options
   m->balancePeriod = CONFIG_OPTION(MEM_BALANCE_PERIOD) * 1000;
   m->samplePeriod = CONFIG_OPTION(MEM_SAMPLE_PERIOD);
   m->sampleSize = CONFIG_OPTION(MEM_SAMPLE_SIZE);
   m->sampleHistory = CONFIG_OPTION(MEM_SAMPLE_HISTORY);
   m->idleTax = CONFIG_OPTION(MEM_IDLE_TAX);
   
   // convert idle tax rate to cost factor
   m->idleCost = MemSchedTaxToCost(m->idleTax);

   // initial page-sharing config options
   m->shareScanVM = CONFIG_OPTION(MEM_SHARE_SCAN_VM);
   m->shareScanTotal = CONFIG_OPTION(MEM_SHARE_SCAN_TOTAL);
   m->shareCheckVM = CONFIG_OPTION(MEM_SHARE_CHECK_VM);
   m->shareCheckTotal = CONFIG_OPTION(MEM_SHARE_CHECK_TOTAL);
   m->shareScanRate = 0;
   m->shareCheckRate = 0;
   m->shareEnable = FALSE;

   // register "/proc/vmware/sched/mem"
   Proc_InitEntry(&m->procMem);
   m->procMem.parent = procSchedDir;
   m->procMem.read = MemSchedProcRead;
   m->procMem.write = MemSchedProcWrite;
   m->procMem.private = (void *) FALSE;
   Proc_Register(&m->procMem, "mem", FALSE);

   // register "/proc/vmware/sched/mem-verbose"
   Proc_InitEntry(&m->procMemVerbose);
   m->procMemVerbose.parent = procSchedDir;
   m->procMemVerbose.read = MemSchedProcRead;   
   m->procMemVerbose.write = MemSchedProcWrite;
   m->procMemVerbose.private = (void *) TRUE;
   Proc_Register(&m->procMemVerbose, "mem-verbose", FALSE);
  
   // set default affinity 
   for (n=0; n < NUMA_GetNumNodes(); n++) {
      m->defaultNodeAffinity |= MEMSCHED_NODE_AFFINITY(n);
   }

   // initialize memory metrics module
   MemMetrics_Init(procSchedDir);

   // log initialization message
   LOG(0, "initialized");
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedSnapshotClient --
 *
 *      Take a snapshot on the usage and stats for the memory client.
 *      The snapshot is then used for calculatiing memory allocations.
 *
 * Results:
 *      None. 
 *
 * Side effects:
 *      MemSched client usage/stats snapshot updated.
 *
 *----------------------------------------------------------------------
 */
static void
MemSchedSnapshotClient(MemSched_Client *c)
{
   MemSchedUser *user = &c->user;
   MemSchedVmm *vmm = &c->vmm;

   memset(&c->snapshot, 0, sizeof(c->snapshot));

   if (vmm->valid) {
      MemSched_Info *info = VmmClientSharedData(vmm);
      MemSchedVmmUsage *vmmUsage = VmmClientCurrentUsage(c);
      uint32 nSampled, usedPct;

      // snapshot current client memory usage
      c->snapshot.locked = vmmUsage->locked;
      c->snapshot.overhead = ClientCurrentOverhead(c);
      c->snapshot.balloon = info->balloon.size;
      c->snapshot.swapped = vmmUsage->swapped;
      c->snapshot.cow = vmmUsage->cow;
      c->snapshot.zero = vmmUsage->zero;
      c->snapshot.shared = c->snapshot.zero;
      if (c->snapshot.cow > c->snapshot.zero) {
         // approximation: assume 50% sharing for non-zero COW pages
         c->snapshot.shared += (c->snapshot.cow - c->snapshot.zero) / 2;
      }
      ASSERT(c->snapshot.locked >= c->snapshot.shared);

      // update estimated number of touched pages
      nSampled = c->alloc.max - c->snapshot.balloon;
      // take max over the fast intra-period average
      usedPct = info->sample.stats.nextAvg;
      // ... and over the most recent completed sample period 
      usedPct = MAX(usedPct, info->sample.stats.slowAvg[0]);
      usedPct = MAX(usedPct, info->sample.stats.fastAvg[0]);
      // ... and over the oldest completed sample period 
      ASSERT(info->sample.history >= 1);
      usedPct = MAX(usedPct, info->sample.stats.slowAvg[info->sample.history - 1]);
      usedPct = MAX(usedPct, info->sample.stats.fastAvg[info->sample.history - 1]);

      c->snapshot.touched = (nSampled * usedPct) / 100;
      c->snapshot.touched = MIN(c->snapshot.touched, c->snapshot.locked);
   } else if (user->valid) {
      MemSchedUserUsage *userUsage = UserClientCurrentUsage(c);
      c->snapshot.locked = userUsage->pageable;
      c->snapshot.swapped = userUsage->swapped;
      c->snapshot.touched = userUsage->pageable;
      c->snapshot.cow = userUsage->cow;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * MemSchedUpdateTotals --
 *
 *	Computes the current memory sizes associated with each
 *	client, and updates totals related to available memory.
 *      Caller must hold MemSched lock.
 *
 * Results:
 *      Modifies memSched and per-client memory scheduling state.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
MemSchedUpdateTotals(void)
{
   MemSched *m = &memSched;
   int32 totalMin, totalTargetMin;  
   Bool reduceMin = FALSE;

   totalMin = 0;
   totalTargetMin = 0;

   // if we have vms which are not responding
   if (!m->allClientsResponsive) {
      int32 reservedMem, autoMinMem, availMem;
      Bool swapEnabled;
      // check existing reserved memory level
      swapEnabled = Swap_IsEnabled();
      MemSchedReservedMem(swapEnabled, &availMem, 
                          &reservedMem, &autoMinMem);
      if (availMem < 0) {
         // reduce the min allocation to each vm
         reduceMin = TRUE;
         FORALL_MEMSCHED_CLIENTS(&m->schedQueue, c) {
            if (ClientResponsive(c)) {
               totalMin += c->alloc.min;
            }
         } MEMSCHED_CLIENTS_DONE;
         // totalMin - (-availMem) -> totalMin + availMem
         totalTargetMin = MAX((totalMin + availMem), 0);
      }
   }

   // snapshot, update per-world and aggregate sizes
   FORALL_MEMSCHED_CLIENTS(&m->schedQueue, c) {
      MemSchedVmm *vmm = &c->vmm;

      // snapshot clients usage
      MemSchedSnapshotClient(c);

      // initialize
      memset(&c->update, 0, sizeof(c->update));

      if (ClientResponsive(c)) {
         if (reduceMin) {
            if (totalMin <= 0) {
               c->alloc.adjustedMin = 0;
            } else {
               // sanity check
               ASSERT(totalTargetMin < totalMin);

               // adjust min to account for non-responsive VMs
               c->alloc.adjustedMin = 
                     ((int64)c->alloc.min * (int64)totalTargetMin)/totalMin;
            }
         } else {
            c->alloc.adjustedMin = c->alloc.min;
         }

         // lazy alloc: below min despite lack of memory pressure?
         c->update.minTarget = c->alloc.adjustedMin;
         if (c->snapshot.locked < c->alloc.adjustedMin &&
             vmm->swapTarget + vmm->balloonTarget == 0) {
            c->update.minTarget =
                  MIN(c->alloc.adjustedMin, c->snapshot.locked + MEMSCHED_MIN_TARGET_DELTA);
         }
      }
   } MEMSCHED_CLIENTS_DONE;
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedClientUpdatePPS --
 *
 *	Updates the adjusted pages-per-share ratio for memory scheduler
 *	client "c", counting each idle page as "idleCost" pages.
 *
 * Results:
 *      Updates "c".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
MemSchedClientUpdatePPS(MemSched_Client *c, uint32 idleCost)
{
   uint32 consume, idle;

   // memory consumption (shared pages don't consume memory)
   if (c->update.target > c->snapshot.shared) {
      consume = c->update.target - c->snapshot.shared;
   } else {
      consume = 0;
   }

   // inactive memory
   if (consume > c->snapshot.touched) {
      idle = consume - c->snapshot.touched;
   } else {
      idle = 0;
   }

   // compute adjusted pages per share
   if (c->update.target < c->update.minTarget) {
      // force min pps if below min target
      c->update.charged = consume;
      c->update.pps = MEMSCHED_PPS_MIN;
   } else {
      // impose idle memory tax
      if (idle > 0) {
         uint64 idleCharge;

         // carefully avoid 32-bit overflow (guests <= 64GB = 16M pages)
         idleCharge = idleCost * idle;
         idleCharge = idleCharge >> MEMSCHED_COST_SCALE_SHIFT;
         ASSERT(idleCharge < (1LL << 32));

         ASSERT(idle <= consume);
         c->update.charged = (consume - idle) + ((uint32) idleCharge);
      } else {
         c->update.charged = consume;
      }

      // compute pps (charged pages per share)
      if (c->update.invShares > MEMSCHED_SHARES_INV_MAX) {
         // special case: no shares => 1/shares infinite => infinite pps
         c->update.pps = MEMSCHED_PPS_MAX;
      } else {
         // normal case
         c->update.pps = c->update.charged * c->update.invShares;
         ASSERT(c->update.pps < MEMSCHED_PPS_MAX);
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedImbalancedClients --
 *
 *	Find the clients with the minimum and maximum adjusted
 *	pages-per-share ratios that can have at least "threshold"
 *	pages reallocated to them (for min) or from them (for max).
 *	Caller must hold MemSched lock.
 *
 * Results:
 *      Sets "lo" to the min PPS client, or NULL if none meets criteria.
 *	Sets "hi" to the max PPS client, or NULL if none meets criteria.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
MemSchedImbalancedClients(uint32 threshold,
                          MemSched_Client **lo,
                          MemSched_Client **hi)
{
   MemSched *m = &memSched;
   MemSched_Client *min, *max;

   // initialize
   min = NULL;
   max = NULL;
   
   // examine all clients
   FORALL_MEMSCHED_CLIENTS(&m->schedQueue, c) {
      if (ClientResponsive(c)) {
         // find client with min PPS that is under max alloc by threshold
         if (((c->update.target + threshold) < c->alloc.max) && 
             ((c->update.target < c->alloc.min) || (c->alloc.shares > 0)) &&
             ((min == NULL) || (c->update.pps < min->update.pps))) {
            min = c;
         }

         // find client with max PPS that is over min alloc by threshold
         if ((c->update.target > (c->update.minTarget + threshold)) &&
             ((max == NULL) || (c->update.pps > max->update.pps))) {
            max = c;
         }
      }
   } MEMSCHED_CLIENTS_DONE;

   // set reply values
   *lo = min;
   *hi = max;
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedXferLog --
 *
 *	Format and log message indicating transfer of "nPages"
 *	between memory scheduler clients "to" and "from".
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      Writes messages to log.
 *
 *----------------------------------------------------------------------
 */
static void
MemSchedXferLog(const char *prefix,
                const MemSched_Client *from,
                const MemSched_Client *to,
                int nPages)
{
   char fromName[MEMSCHED_XFER_LOG_BUF_SIZE];
   char toName[MEMSCHED_XFER_LOG_BUF_SIZE];

   // format "from" client
   if (from == NULL) {
      (void) snprintf(fromName, MEMSCHED_XFER_LOG_BUF_SIZE, "VMK");
   } else {
      (void) snprintf(fromName,
                      MEMSCHED_XFER_LOG_BUF_SIZE,
                      "%3d (tgt %5d, pps %Lu)",
                      ClientGroupID(from),
                      from->update.target,
                      from->update.pps);
   }

   // format "to" client
   if (to == NULL) {
      (void) snprintf(toName, MEMSCHED_XFER_LOG_BUF_SIZE, "VMK");
   } else {
      (void) snprintf(toName,
                      MEMSCHED_XFER_LOG_BUF_SIZE,
                      "%3d (tgt %5d, pps %Lu)",
                      ClientGroupID(to),
                      to->update.target,
                      to->update.pps);
   }

   // log transfer
   Log("%s: xfer %4d: %s => %s", prefix, nPages, fromName, toName);
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedBalanceClients --
 *
 *	Attempts to reduce the PPS imbalance between clients by
 *	by reallocating memory from "hi" to "lo".  Requires that
 *	"lo" has a lower initial PPS value than "hi".
 *	Caller must hold MemSched lock.
 *
 * Results:
 *	Returns the number of pages transferred from "hi" to "lo".
 *      Updates "lo", "hi".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int32
MemSchedBalanceClients(uint32 threshold,
                       MemSched_Client *lo,
                       MemSched_Client *hi)
{
   MemSched *m = &memSched;
   int32 loMax, hiMax, xferMin, xferMax, xfer, xferCount;
   uint64 delta, deltaOrig;
   uint32 loOrig, hiOrig;

   // sanity checks
   ASSERT(lo != hi);
   ASSERT(lo->update.pps <= hi->update.pps);

   // remember original state
   loOrig = lo->update.target;
   hiOrig = hi->update.target;
   deltaOrig = hi->update.pps - lo->update.pps;

   // done if already balanced
   if (deltaOrig == 0) {
      return(0);
   }

   // compute maximum transfer size
   loMax = lo->alloc.max - lo->update.target;
   ASSERT(loMax > 0);
   hiMax = hi->update.target - hi->update.minTarget;   
   ASSERT(hiMax > 0);

   // initialize
   xferMin = 0;
   xferMax = MIN(loMax, hiMax);

   // attempt to balance allocations
   // binary search for optimum transfer size
   xferCount = 0;
   for (xfer = xferMax / 2;
        (xferMax - xferMin) > threshold;
        xfer = (xferMin + xferMax) / 2) {
      // track iteration count
      xferCount++;

      // sanity check (no system has 2^30 pages)
      ASSERT(xferCount < 30);

      lo->update.target = loOrig + xfer;
      hi->update.target = hiOrig - xfer;
   
      MemSchedClientUpdatePPS(lo, m->idleCost);
      MemSchedClientUpdatePPS(hi, m->idleCost);

      // adjust transfer size
      if (lo->update.pps < hi->update.pps) {
         // increase size
         xferMin = xfer;
      } else {
         // decrease size
         xferMax = xfer;
      }
   }

   // compute new PPS difference
   if (hi->update.pps > lo->update.pps) {
      delta = hi->update.pps - lo->update.pps;
   } else {
      delta = lo->update.pps - hi->update.pps;
   }

   // revert to original values if no forward progress
   if (delta > deltaOrig) {
      lo->update.target = loOrig;
      hi->update.target = hiOrig;
      MemSchedClientUpdatePPS(lo, m->idleCost);
      MemSchedClientUpdatePPS(hi, m->idleCost);      
   }

   // debugging
   if (MEMSCHED_DEBUG_BALANCE) {
      MemSchedXferLog("MemSched: BC", hi, lo, lo->update.target - loOrig);
   }

   return(lo->update.target - loOrig);
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedUpdateTargets --
 *
 *	Computes the long-term target memory allocation for each 
 *	client, as a function of its share allocation and usage.
 *      Caller must hold MemSched lock.
 *
 * Results:
 *      Modifies memSched and per-client memory scheduling state.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
MemSchedUpdateTargets(void)
{
   MemSched *m = &memSched;
   int32 excessPages;
   int nBalance;

   // initialize totals
   excessPages = MemSchedFreePages();

   // debugging
   if (MEMSCHED_DEBUG_BALANCE) {
      LOG(0, "realloc %d: nclients=%d, excess=%d",
          m->reallocFastCount, m->numScheds, excessPages);
   }

   // compute initial allocations based on current usage
   FORALL_MEMSCHED_CLIENTS(&m->schedQueue, c) {
      if (!ClientResponsive(c)) {
         continue;
      }

      if (c->alloc.shares > 0) {
         // current usage, set invShares proportional to 1/shares
         c->update.target = c->snapshot.locked;
         c->update.invShares = MEMSCHED_SHARES_INV_MAX / c->alloc.shares;
      } else {
         // no shares: current usage, but don't exceed min; 1/shares infinite
         c->update.target = MIN(c->snapshot.locked, c->alloc.min);
         c->update.invShares = MEMSCHED_SHARES_INV_MAX + 1;
      }

      MemSchedClientUpdatePPS(c, m->idleCost);
   } MEMSCHED_CLIENTS_DONE;

   // handle changes in free pages
   for (nBalance = 0; excessPages != 0; nBalance++) {
      MemSched_Client *low, *high;
      int32 xfer;

      // sanity check
      ASSERT(nBalance <= m->numScheds);
      
      // find clients with extreme PPS values
      MemSchedImbalancedClients(0, &low, &high);

      // crudely adjust initial allocations
      if (excessPages < 0) {
         // reclaim pages from client with max PPS
         if (high == NULL) {
            break;
         }
         xfer = MIN(high->update.target - high->update.minTarget, -excessPages);
         high->update.target -= xfer;
         excessPages += xfer;
         MemSchedClientUpdatePPS(high, m->idleCost);
         if (MEMSCHED_DEBUG_BALANCE) {
            MemSchedXferLog("MemSched: UT: pre", high, NULL, xfer);
         }
      } else {
         uint32 lowMax;

         // give pages to client with min PPS
         if (low == NULL) {
            break;
         }

         // allow adjustment up to max size, or min if no shares
         if (low->alloc.shares > 0) {
            lowMax = low->alloc.max;
         } else {
            lowMax = low->alloc.min;
         }
         ASSERT(low->update.target <= lowMax);

         xfer = MIN(lowMax - low->update.target, excessPages);
         low->update.target += xfer;
         excessPages -= xfer;
         MemSchedClientUpdatePPS(low, m->idleCost);
         if (MEMSCHED_DEBUG_BALANCE) {
            MemSchedXferLog("MemSched: UT: pre", NULL, low, xfer);
         }
      }
   }

   // balance allocations using pairwise operations,
   //   restrict total number of pairwise transfers to limit overhead
   for (nBalance = 0; nBalance < (2 * m->numScheds); nBalance++) {
      // future modification: adaptively lower threshold
      const uint32 threshold = MEMSCHED_BALANCE_THRESHOLD;
      MemSched_Client *low, *high;
      
      // find greatest imbalance, done if none above threshold
      MemSchedImbalancedClients(threshold, &low, &high);
      if ((low == NULL) || (high == NULL) || (low == high)) {
         break;
      }

      // don't attempt to balance if PPS imbalance backwards;
      //   rare condition, but possible due to thresholding
      if (low->update.pps > high->update.pps) {
         LOG(0, "balance skipped: low=%d (mpps=%Lu), high=%d (mpps=%Lu)",
             ClientGroupID(low),
             low->update.pps / 1000,
             ClientGroupID(high),
             high->update.pps / 1000);
         break;
      }

      // reduce imbalance, done if no reduction
      if (MemSchedBalanceClients(threshold, low, high) == 0) {
         break;
      }
   }

   // debugging
   if (MEMSCHED_DEBUG_BALANCE) {
      LOG(0, "nBalance=%d", nBalance);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * MemSchedUpdateAllocs --
 *
 *	Computes the short-term memory allocation for each client,
 *	proportional to its entitled share of immediately-available
 *	memory pages, moving toward its long-term target allocation.
 *      Caller must hold MemSched lock.
 *
 * Results:
 *      Modifies memSched and per-client memory scheduling state.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
MemSchedUpdateAllocs(void)
{
   MemSched *m = &memSched;
   int32 totalOwed, totalFree;

   // initialize totals
   totalFree = MemSchedFreePages();
   totalOwed = 0;

   // compute total amount owed to each world
   FORALL_MEMSCHED_CLIENTS(&m->schedQueue, c) {
      int32 owed;

      if (!ClientResponsive(c)) {
         continue;
      }
      if (MemSched_MemoryIsLow()) {
         c->update.alloc = MIN(c->snapshot.locked, c->commit.alloc);
      } else {
         c->update.alloc = c->snapshot.locked;
      }

      owed = c->update.target - c->update.alloc;
      if (owed > 0) {
         // if client unresponsive, we ignore memory
         // owed to it so that we can grant a larger percent of the
         // free memory to responsive clients
         totalOwed += owed;
      }
   } MEMSCHED_CLIENTS_DONE;

   // grant immediate alloc proportional to amount owed
   FORALL_MEMSCHED_CLIENTS(&m->schedQueue, c) {
      int32 owed = c->update.target - c->update.alloc;

      if (!ClientResponsive(c)) {
         continue;
      }

      // decreasing alloc => reclaim memory immediately
      if (owed < 0) {
         c->update.alloc = c->update.target;
      }

      // increasing alloc => grant share of available pages (if any)
      if ((owed > 0) && (totalFree > 0)) {
         int64 grant = ((int64) owed * (int64) totalFree) / totalOwed;
         grant = MIN(grant, owed);
         c->update.alloc += (int32) grant;
#ifdef VMX86_DEBUG
         if (MemSched_MemoryIsLow() && c->vmm.valid) {
            c->vmm.lowStateFree++;
            c->vmm.lowStateFreeAmt += grant;
         }
#endif
      }
   } MEMSCHED_CLIENTS_DONE;
}

/*
 *----------------------------------------------------------------------
 *
 * MemSched_ShouldSwapBlock --
 *
 *      Determine if we should block if the swap target has not
 *      been reached.
 *
 * Results:
 *      Returns TRUE if we should block during swapping, FALSE otherwise
 *
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
Bool 
MemSched_ShouldSwapBlock(uint32 swapTarget, 
                         uint32 swapped)
{
   // current state
   MemSchedState curState = MemSchedCurrentState();

   // swap is not enabled, so not possible to block
   if (!Swap_IsEnabled()) {
      return FALSE;
   }

   // debugging: stress blocking/swapping
   if (MEMSCHED_DEBUG_SWAP_STRESS) {
      return TRUE;
   }

   if (curState == MEMSCHED_STATE_LOW) {
      return TRUE;
   } else if (curState == MEMSCHED_STATE_HARD) {
      if (swapTarget > (swapped + MEMSCHED_MAX_SWAP_SLACK)) {
         return TRUE;
      }
   }
   return FALSE;
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedClientCommitAllocVmm --
 *
 *	Makes most-recently updated allocations effective client "c"
 *      to vmm worlds, setting its balloon and swap target appropriately.
 *      Caller must hold MemSched lock.
 *
 * Results:
 *      Modifies per-client memory scheduling state for "c".
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
MemSchedClientCommitAllocVmm(MemSched_Client *c, UNUSED_PARAM(Bool canBlock))
{
   MemSchedVmm *vmm = &c->vmm;
   int32 balloonTarget, swapTarget;
   int32 ballooned, swapped, delta;
   MemSchedState freeState;
   Bool swapBlock;
   uint32 balloonBonusPages = 0;

   // current state
   freeState = MemSchedCurrentState();

   // compute total reclamation target
   delta = c->commit.alloc - c->snapshot.locked;

   // debugging
   if (MEMSCHED_DEBUG_VERBOSE) {
      ClientDebug(c, "delta=%d", delta);
   }

   // compute current amount reclaimed
   ballooned = c->snapshot.balloon;
   swapped = c->snapshot.swapped;

   if (delta >= 0) {
      // decrease memory pressure, reduce swap first
      if (delta <= swapped) {
         swapTarget = swapped - delta;
         balloonTarget = ballooned;
      } else {
         swapTarget = 0;
         balloonTarget = ballooned - (delta - swapped);
         balloonTarget = MAX(balloonTarget, 0);
      }
   } else {
      // increase memory pressure
      int32 reclaimDelta;

      reclaimDelta = -delta;

      if (ClientBalloonActive(c)) {
         // balloon+swap if active balloon driver
         balloonTarget = ballooned;
         swapTarget = swapped;
            
         switch (freeState) {
         case MEMSCHED_STATE_HIGH:
            balloonTarget += reclaimDelta;
            break;
         case MEMSCHED_STATE_SOFT:
            // future mod: balloon x%, swap (N - x)%, where N >= 100
            balloonTarget += reclaimDelta;
            break;
         case MEMSCHED_STATE_HARD:
         case MEMSCHED_STATE_LOW:
            swapTarget += reclaimDelta;
            balloonBonusPages = MEMSCHED_BALLOON_BONUS_PAGES;
            break;
         default:
            NOT_REACHED();
            break;
         }
      } else {
         // swap if no active balloon driver
         balloonTarget = 0;
         ASSERT(ballooned >= 0);
         swapTarget = (ballooned + swapped + reclaimDelta);
      }
   }
      
   // enforce maximum balloon size
   MemSchedClientBalloonUpdateMax(c);
   if (balloonTarget > vmm->balloonMax) {
      uint32 balloonDelta = balloonTarget - vmm->balloonMax;
      if (MEMSCHED_DEBUG_ENFORCE) {
         ClientDebug(c, "enforced balloon max: %u -> %u",
                     balloonTarget, vmm->balloonMax);
      }
      balloonTarget -= balloonDelta;
      swapTarget += balloonDelta;
   }

   // see if the balloon driver can pleasantly surprise us
   // with some bonus pages
   if (LIKELY((balloonTarget + balloonBonusPages) <= vmm->balloonMax)) {
      balloonTarget += balloonBonusPages;
   }

   if (!Swap_IsEnabled() && swapTarget > 0) {
      ASSERT(FALSE);
      Warning("swapTarget %d with no swap enabled", swapTarget);
      // In release builds, we try balloon again.
      balloonTarget += swapTarget;
      swapTarget = 0;
   }

   // sanity checks
   ASSERT(balloonTarget >= 0);
   ASSERT(balloonTarget <= vmm->balloonMax);
   ASSERT(swapTarget >= 0);

   // block if low on memory or far from target
   swapBlock = MemSched_ShouldSwapBlock(swapTarget, swapped);

   // debugging
   if (MEMSCHED_DEBUG_ENFORCE) {
      ClientDebug(c, "use/tgt: lock=%u/%u/(%u), balloon=%u/%u, swap=%u/%u/%d",
                  VmmClientCurrentUsage(c)->locked, 
                  c->commit.alloc, c->snapshot.locked,
                  ballooned, balloonTarget,
                  swapped, swapTarget, swapBlock);
   }

   // make allocations effective
   MemSchedClientBalloonSet(c, balloonTarget);
   MemSchedClientSwapSet(c, swapTarget);
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedClientCommitAllocUser --
 *
 *	Makes most-recently updated allocations effective client "c"
 *      to user worlds. Swap out memory if required.
 *      Caller must hold memsched lock.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
MemSchedClientCommitAllocUser(MemSched_Client *c, Bool canBlock)
{
   MemSchedUserUsage *userUsage = &c->user.usage;
   int target, delta;

   if (c->vmm.valid) {
      return;
   }

   if (canBlock) {
      if (!Swap_IsEnabled()) {
         LOG(2, "swap not enabled");
         return;
      }

      // calculate swap target based on committed allocation
      target = c->snapshot.swapped + c->snapshot.locked - c->commit.alloc;

      if (target > 0) {
         LOG(2, "World %d swap-target %d used pages %d swapped %d pinned %d", 
             ClientGroupID(c), target, c->snapshot.locked, 
             c->snapshot.swapped, userUsage->pinned);

         c->user.swapTarget = target;
      } else {
         c->user.swapTarget = 0;
      }

      // get difference between swap target and currently swapped pages
      delta = c->user.swapTarget - userUsage->swapped;

      if (delta > 0) {
         World_Handle *world = World_Find(ClientGroupID(c));
         if (world != NULL) { 
            MemSchedUnlock();
            User_SwapOutPages(world, delta);
            MemSchedLock();
            World_Release(world);
         }
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedCommitAllocs --
 *
 *	Makes most-recently updated allocations effective for each
 *	client by setting its balloon and swap target appropriately.
 *      Caller must hold MemSched lock.
 *      If "canBlock == TRUE", we may release MemSched lock and acquire
 *      it again.
 *
 * Results:
 *      Modifies memSched and per-client memory scheduling state.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
MemSchedCommitAllocs(Bool canBlock)
{
   MemSched *m = &memSched;
   uint32 reallocGen = m->reallocGen;

   // use ballooning or swapping to implement allocations
   FORALL_MEMSCHED_CLIENTS(&m->schedQueue, c) {
      MemSchedVmm *vmm = &c->vmm;
      MemSchedUser *user = &c->user;

      // commit updated state
      c->commit = c->update;

      if (vmm->valid &&
          vmm->vmResponsive) {
         MemSchedClientCommitAllocVmm(c, canBlock);
      }
      if (reallocGen == m->reallocGen && 
          user->valid) {
         MemSchedClientCommitAllocUser(c, canBlock);
      }

      /*
       * If the generation counter is changed while we don't hold the 
       * MemSched lock, we skip committing rest of the memsched clients. 
       */
      if (UNLIKELY(reallocGen != m->reallocGen)) {
         ASSERT(canBlock);
         return;
      }
   } MEMSCHED_CLIENTS_DONE;
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedReallocate --
 *
 *      Reallocates memory among worlds managed by memory scheduler.
 *	Caller must hold MemSched lock.
 *      If "canBlock == TRUE", we may release MemSched lock
 *      and perform blocking operations in commit stage.
 *      If "canBlock == FALSE", we will hold MemSched lock all through.
 *
 * Results:
 *      Modifies memSched and per-client memory scheduling state.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
MemSchedReallocate(Bool canBlock)
{
   MemSched *m = &memSched;

   ASSERT(MemSchedIsLocked());
   // update stats
   if (canBlock) {
      m->reallocSlowCount++;
   } else {
      m->reallocFastCount++;
   }

   // find non-responsive clients
   MemSchedFindNonResponsiveClients();

   // update auto-min allocations
   MemSchedUpdateAutoMins();

   // compute updated allocation
   MemSchedUpdateTotals();
   MemSchedUpdateTargets();
   MemSchedUpdateAllocs();

   // commit updated allocation
   MemSchedCommitAllocs(canBlock);
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedReallocBHHandler --
 *
 *      BH handler for executing MemSchedReallocate().
 *
 * Results:
 *      See MemSchedReallocate().
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
MemSchedReallocBHHandler(UNUSED_PARAM(void * clientData))
{
   MemSchedLock();
   // reallocate memory non-block
   MemSchedReallocate(FALSE);

   // issue another reallocation request to memsched world
   MemSchedReallocReqSlow();

   MemSchedUnlock();
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedUpdatePShareRate --
 *
 *	Update page sharing rate according to page sharing parameters
 *	and reconfigure all clients of memSched.
 *	Caller must hold MemSched lock.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      Reconfigures page sharing parameters for all clients of memsched.
 *
 *----------------------------------------------------------------------
 */
static void
MemSchedUpdatePShareRate(void)
{
   MemSched *m = &memSched;
   uint32 scanRate, checkRate;
   Bool enable;
   uint32 numVMs = 0;

   // compute per-VM rates based on per-VM and aggregate limits
   scanRate = m->shareScanVM;
   checkRate = m->shareCheckVM;

   FORALL_MEMSCHED_VMM_CLIENTS(&m->schedQueue, c, vmm) {
      if (vmm->pshareEnable) {
         numVMs++; 
      }
   } MEMSCHED_VMM_CLIENTS_DONE;
   if (numVMs > 0) {
      scanRate = MIN(scanRate, m->shareScanTotal / numVMs);
      checkRate = MIN(checkRate, m->shareCheckTotal / numVMs);
   }
   enable = (scanRate + checkRate > 0) ? TRUE : FALSE;

   // forcibly disable page sharing if not supported
   if (!PShare_IsEnabled()) {
      scanRate = 0;
      checkRate = 0;
      enable = FALSE;
   }

   // reconfigure if rates changed
   if ((m->shareScanRate != scanRate)   ||
       (m->shareCheckRate != checkRate) ||
       (m->shareEnable != enable)) {

      // update rates
      m->shareScanRate = scanRate;
      m->shareCheckRate = checkRate;
      m->shareEnable = enable;

      // update all clients
      FORALL_MEMSCHED_VMM_CLIENTS(&m->schedQueue, c, vmm) {
         MemSched_PShareInfo *info = &VmmClientSharedData(vmm)->pshare;
         if (vmm->pshareEnable) {
            // update shared area, post config action
            info->enable = enable;
            info->scanRate = scanRate;
            info->checkRate = checkRate;
            Action_Post(vmm->world, vmm->pshareAction);
         }
      } MEMSCHED_VMM_CLIENTS_DONE;

      // debugging
      if (MEMSCHED_DEBUG) {
         LOG(0, "enable=%d, scanRate=%u, checkRate=%u",
             enable, scanRate, checkRate);
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * MemSched_Reconfig --
 *
 *	Reconfigure the memory scheduling parameter and request for
 *      memory reallocation by the memsched world.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
MemSched_Reconfig(Bool write, Bool valueChanged, int indx)
{
   MemSched *m = &memSched;
   if (write && valueChanged) {
      MemSchedLock();

      switch (indx) {
      case CONFIG_MEM_IDLE_TAX:
         m->idleTax = CONFIG_OPTION(MEM_IDLE_TAX);
         break;
      case CONFIG_MEM_BALANCE_PERIOD:
         m->balancePeriod = CONFIG_OPTION(MEM_BALANCE_PERIOD) * 1000;
         break;
      default:
         Warning("config change %s not handled", Config_GetStringOption(indx));
         break;
      }
     
      MemSchedReallocReqSlow();
      MemSchedUnlock();
   }
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * MemSched_ReconfigPShare --
 *
 *	Reconfigure page sharing parameters and update
 *      page sharing scan and check rate.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
MemSched_ReconfigPShare(Bool write, Bool valueChanged, int indx)
{
   MemSched *m = &memSched;

   if (write && valueChanged) {
      MemSchedLock();
      switch (indx) {
      case CONFIG_MEM_SHARE_SCAN_VM:
         m->shareScanVM = CONFIG_OPTION(MEM_SHARE_SCAN_VM);
         break;
      case CONFIG_MEM_SHARE_SCAN_TOTAL:
         m->shareScanTotal = CONFIG_OPTION(MEM_SHARE_SCAN_TOTAL);
         break;
      case CONFIG_MEM_SHARE_CHECK_VM:
         m->shareCheckVM = CONFIG_OPTION(MEM_SHARE_CHECK_VM);
         break;
      case CONFIG_MEM_SHARE_CHECK_TOTAL:
         m->shareCheckTotal = CONFIG_OPTION(MEM_SHARE_CHECK_TOTAL);
         break;
      default:
         Warning("pshare config change %s not handled", Config_GetStringOption(indx));
         break;
      }

      if (PShare_IsEnabled()) {
         MemSchedUpdatePShareRate();
      }
      MemSchedUnlock();
   }
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * MemSched_ReconfigSample --
 *
 *	Reconfigure memory sampling parameters and update
 *      sampling rate for clients.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
MemSched_ReconfigSample(Bool write, Bool valueChanged, int indx)
{
   MemSched *m = &memSched;

   if (write && valueChanged) {
      MemSchedLock();
      switch (indx) {
      case CONFIG_MEM_SAMPLE_PERIOD:
         m->samplePeriod  = CONFIG_OPTION(MEM_SAMPLE_PERIOD);
         break;
      case CONFIG_MEM_SAMPLE_SIZE:
         m->sampleSize    = CONFIG_OPTION(MEM_SAMPLE_SIZE);
         break;
      case CONFIG_MEM_SAMPLE_HISTORY:
         m->sampleHistory = CONFIG_OPTION(MEM_SAMPLE_HISTORY);
         break;
      default:
         Warning("sample config change %s not handled", Config_GetStringOption(indx));
         break;
      } 
        
      // update all clients
      FORALL_MEMSCHED_VMM_CLIENTS(&m->schedQueue, c, vmm) {
         MemSched_SampleInfo *info = &VmmClientSharedData(vmm)->sample;
         
         vmm->samplePeriod = m->samplePeriod;
         vmm->sampleSize = m->sampleSize;
         vmm->sampleHistory = m->sampleHistory;
         info->period = vmm->samplePeriod;            
         info->size = vmm->sampleSize;            
         info->history = vmm->sampleHistory;            
         Action_Post(vmm->world, vmm->sampleAction);
      } MEMSCHED_VMM_CLIENTS_DONE;

      MemSchedUnlock();
   }
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * MemSchedRemapNodeStress --
 *
 *      Stress page migration code on NUMA systems by periodically
 *	altering memory node affinity and page migration rates.
 *	Caller must hold MemSched lock.
 *
 * Results:
 *      None. 
 *
 * Side effects:
 *      Modifies global and per-client memory scheduling state.
 *
 *----------------------------------------------------------------------
 */
static void
MemSchedRemapNodeStress(void)
{
   MemSched *m = &memSched;
   uint32 numaNodes;

   // sanity check
   ASSERT(MemSchedIsLocked());

   // nothing to do unless NUMA
   numaNodes = NUMA_GetNumNodes();
   if (numaNodes <= 1) {
      return;
   }

   // nothing to do until node stress period elapses
   if (m->nodeStressCount < VMK_STRESS_RELEASE_VALUE(MEM_REMAP_NODE)) {
      m->nodeStressCount++;
      return;
   } else {
      m->nodeStressCount = 0;
   }

   // induce stress by altering affinity, migration rate configs
   FORALL_MEMSCHED_VMM_CLIENTS(&m->schedQueue, c, vmm) {
      MemSchedVmm *vmm = &c->vmm;
      MemSched_Info *info = VmmClientSharedData(vmm);
      NUMA_Node rndNode;

      // set affinity to random node
      m->nodeStressSeed = Util_FastRand(m->nodeStressSeed);
      rndNode = m->nodeStressSeed % numaNodes;
      MemSchedSetNodeAffinityInt(c, MEMSCHED_NODE_AFFINITY(rndNode), TRUE);

      // set page migration rate
      info->remap.migrateScanRate = MEMSCHED_NODE_STRESS_RATE;

      // signal vmm to pickup new config
      Action_Post(vmm->world, vmm->remapConfigAction);

      // debugging
      ClientDebug(c, "set node=%u, mask=0x%x, rate=%u",
                  rndNode,
                  info->remap.migrateNodeMask,
                  info->remap.migrateScanRate);
   } MEMSCHED_VMM_CLIENTS_DONE;
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedLoop --
 *
 *      To be executed by memsched world.
 *
 *      Wait for realloc request and reallocate memory among 
 *      managed worlds.
 *
 * Results: 
 *      Never returns.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
MemSchedLoop(UNUSED_PARAM(void * clientData))
{
   uint32 reallocGen;
   MemSched *m = &memSched;

   MemSchedLock();

   // get the current generation counter
   reallocGen = m->reallocGen;

   while(1) {
      // check if the generation counter is the same
      if (reallocGen == m->reallocGen) {
         // wake up on event or next time-out
         MemSchedTimedWaitLock(m->balancePeriod);
      }

      // get the current generation counter
      reallocGen = m->reallocGen;

      // reallocate memory
      MemSchedReallocate(TRUE);
      ASSERT(MemSchedIsLocked());
     
      if (reallocGen == m->reallocGen) { 
         // wake up anyone waiting for reschedule to finish
         MemSchedReallocWakeup();
      }

      // stress page migration, if specified
      if (VMK_STRESS_RELEASE_OPTION(MEM_REMAP_NODE)) {
         MemSchedRemapNodeStress();
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * MemSched_SchedWorldInit --
 *
 *      Create a new memsched daemon world
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
MemSched_SchedWorldInit(void)
{
   World_Handle *world;
   VMK_ReturnStatus status;
   World_InitArgs args;
   Sched_ClientConfig sched;

   Sched_ConfigInit(&sched, SCHED_GROUP_NAME_SYSTEM);
   World_ConfigArgs(&args, "memsched", 
                    WORLD_SYSTEM, WORLD_GROUP_DEFAULT, &sched);
   status = World_New(&args, &world);
   if (status == VMK_OK) {
      status = Sched_Add(world, MemSchedLoop, NULL);
   }
   ASSERT_NOT_IMPLEMENTED(status == VMK_OK);
}


/*
 *----------------------------------------------------------------------
 *
 * MemSched_TotalVMPagesUsed --
 *
 *      Obtain a snapshot of the current memory usage by VMs.
 *
 * Results:
 *      Returns the total current usage, in pages, for all VMs.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
uint32
MemSched_TotalVMPagesUsed(void)
{
   MemSched *m = &memSched;   
   uint32 total;

   // acquire lock
   MemSchedLock();   

   // accumulate total across all clients
   total = 0;
   FORALL_MEMSCHED_CLIENTS(&m->schedQueue, c) {
      total += ClientCurrentSize(c);
   } MEMSCHED_CLIENTS_DONE;

   // release lock
   MemSchedUnlock();

   return(total);
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedReservedMem --
 *
 *      Sets "reserved" to the total number of pages that have already
 *	been reserved, and sets "avail" to the total number of remaining
 *	pages that are available for admitting new clients.
 *	Caller must hold MemSched lock.
 *
 * Results:
 *	Sets "reserved" to the total number of reserved pages.
 *	Sets "avail" to the total number of unreserved pages.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
MemSchedReservedMem(Bool swapEnabled,
                    int32 *avail,
                    int32 *reserved,
                    int32 *autoMinReserved)
{
   MemSched *m = &memSched;
   int32 min, max, overhead, autoMin, total, unReclaimable;

   // initialize
   min = 0;
   max = 0;
   overhead = 0;
   autoMin = 0;
   unReclaimable = 0;
   *reserved = 0;

   // sum total client memory already reserved
   FORALL_MEMSCHED_CLIENTS(&m->schedQueue, c) {
      MemSchedVmm *vmm = &c->vmm;
      MemSchedUser *user = &c->user;
      min += c->alloc.min;
      max += c->alloc.max;
      overhead += c->overhead;
      if (c->alloc.autoMin && swapEnabled) {
         autoMin += c->alloc.min;
      }
      if (vmm->valid) {
         /*
          * If client is unresponsive factor in the 
          * amount of memory it has locked as 'unReclaimable' memory.
          * This memory is not available for use until the VM becomes active
          * again. In the case where swapping is disabled this does not 
          * matter as we reserve max memory for every client.
          */
         if (!vmm->vmResponsive && swapEnabled) {
            MemSchedVmmUsage *vmmUsage = VmmClientCurrentUsage(c);
            // Only add the number of locked pages more than alloc.min
            unReclaimable += (MAX(vmmUsage->locked, c->alloc.min) - c->alloc.min);
         }
         if (user->valid) {
            // assume we don't swap out userworld VMX pages
            unReclaimable += UserClientCurrentUsage(c)->pageable;
         }
      }
   } MEMSCHED_CLIENTS_DONE;

   // reserve min, or max if swapping disabled
   if (swapEnabled) {
      *reserved = min + overhead;
   } else {
      *reserved = max + overhead;
   }

   // add memory reserved for pending admits, min free
   *reserved += MemSchedMinFree();

   // total client memory reserved for auto-min sizes
   *autoMinReserved = autoMin;

   // compute remaining memory
   total = MemMap_ManagedPages() - MemMap_KernelPages();
   // deduct unreclaimable memory 
   total -= unReclaimable;
   // deduct reserved memory
   *avail = total - *reserved;
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedReservedSwap --
 *
 *      Sets "reserved" to the total number of swap files pages that
 *	have already been reserved, and sets "avail" to the total
 *	number of swap file pages that have not already been reserved,
 *	and are therefore available for admitting new clients.
 *	Caller must hold MemSched lock.
 *
 * Results:
 *	Sets "reserved" to the total number of reserved swap file pages.
 *	Sets "avail" to the total number of unreserved swap file pages.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
MemSchedReservedSwap(Bool swapEnabled,
                     int32 *avail,
                     int32 *reserved)
{
   MemSched *m = &memSched;
   // no swap space if swapping disabled
   if (!swapEnabled) {
      *reserved = 0;
      *avail = 0;
      return;
   }

   // initialize
   *reserved = 0;

   // sum total client swap already reserved
   FORALL_MEMSCHED_CLIENTS(&m->schedQueue, c) {
      *reserved += (c->alloc.max - c->alloc.min);
   } MEMSCHED_CLIENTS_DONE;

   // compute remaining memory
   // XXX n.b. use invalid worldID since no per-world swap files yet
   *avail = Swap_GetTotalNumSlots(INVALID_WORLD_ID) - *reserved;
}


/*
 *----------------------------------------------------------------------
 *
 * MemSched_TotalSwapReserved --
 *
 * Results:
 *      Return total number of reserved swap pages by all existing 
 *      memsched clients.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
uint32
MemSched_TotalSwapReserved(void)
{
   int32 avail, reserved;
   MemSchedLock();
   MemSchedReservedSwap(Swap_IsEnabled(), &avail, &reserved);
   ASSERT(reserved >= 0);
   MemSchedUnlock();

   return reserved;
}


/*
 *----------------------------------------------------------------------
 *
 * MemSchedUpdateAutoMins --
 *
 *	Updates minimum size allocations for each client without
 *	an explicitly-specified "min" size.
 *      Caller must hold MemSched lock.
 *
 * Results:
 *      Modifies memSched and per-client memory scheduling state.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
MemSchedUpdateAutoMins(void)
{
   MemSched *m = &memSched;
   int32 totalMin, totalMax, totalAlloc, totalLimit, totalCount;
   int32 availMem, reservedMem, autoMinMem;
   Bool swapEnabled = Swap_IsEnabled();

   // if swap is not enabled, automin should not change
   if (!swapEnabled) {
      return;
   }

   // initialize
   totalCount = 0;
   totalMin = 0;
   totalMax = 0;

   // compute totals for auto-min clients
   FORALL_MEMSCHED_VMM_CLIENTS(&m->schedQueue, c, vmm) {
      if (c->alloc.autoMin && vmm->vmResponsive) {
         totalCount++;
         totalMin += c->alloc.min;
         totalMax += c->alloc.max;
      }
   } MEMSCHED_VMM_CLIENTS_DONE;
   
   // sanity checks
   ASSERT(totalMin >= 0);
   ASSERT(totalMax >= 0);

   // done if no auto-min clients
   if (totalCount == 0) {
      return;
   }

   // check existing reserved memory level
   MemSchedReservedMem(swapEnabled, &availMem, &reservedMem, &autoMinMem);
   ASSERT(autoMinMem >= totalMin);

   // can use additional unreserved memory, up to limit
   totalLimit = totalMax / 2;
   totalAlloc = MIN(totalMin + availMem, totalLimit);

   // check if sufficient unreserved swap to decrease total min
   if (totalAlloc < totalMin) {
      int32 availSwap, reservedSwap, needSwap;

      needSwap = totalMin - totalAlloc;
      MemSchedReservedSwap(swapEnabled, &availSwap, &reservedSwap);

      if (needSwap > availSwap) {
         totalAlloc += (needSwap - availSwap);
         totalAlloc = MIN(totalAlloc, totalMin + availMem);
      }
   }
   totalAlloc = MAX(0, totalAlloc);

   // debugging
   if (MEMSCHED_DEBUG_AUTO_MIN) {
      LOG(0, "n=%d, min=%dK, avail=%dK, limit=%dK, alloc=%dK",
          totalCount,
          PagesToKB(totalMin),
          PagesToKB(availMem),
          PagesToKB(totalLimit),
          PagesToKB(totalAlloc));
   }

   // rebalance auto-min sizes
   if (totalMax > 0) {
      int64 totalAlloc64;
      int32 totalGrant;

      // initialize
      totalAlloc64 = (int64) totalAlloc;
      totalGrant = 0;

      FORALL_MEMSCHED_VMM_CLIENTS(&m->schedQueue, c, vmm) {
         if (c->alloc.autoMin && vmm->vmResponsive) {
            int64 grant;

            // compute min as common fraction of max
            grant = (totalAlloc64 * (int64) c->alloc.max) / totalMax;

            // debugging
            if (MEMSCHED_DEBUG_AUTO_MIN && (grant != c->alloc.min)) {
               VMLOG(0, ClientGroupID(c), "min: %dK -> %dK",
                     PagesToKB(c->alloc.min),
                     PagesToKB((int32) grant));
            }

            // sanity check
            ASSERT(grant >= 0);

            // update min
            c->alloc.min = (uint32) grant;
            totalGrant += c->alloc.min;
         }
      } MEMSCHED_VMM_CLIENTS_DONE;

      // sanity check
      ASSERT(totalGrant <= totalAlloc);

      // debugging
      if (MEMSCHED_DEBUG_AUTO_MIN && (totalGrant != totalMin)) {
         LOG(0, "total: %dK -> %dK",
             PagesToKB(totalMin),
             PagesToKB(totalGrant));
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * MemSched_CheckReserved --
 *
 *	Sets "reservedMem" to the total number of pages that have
 *	already been reserved, and sets "availMem" to the total
 *	number of remaining pages available for admitting new clients.
 *	Sets "autoMinMem" to the total number of reserved pages that
 *	are associated with VMs without explicitly-specified min sizes.
 *      Sets "reservedSwap" to the total number of swap files pages that
 *	have already been reserved, and sets "availSwap" to the total
 *	number of swap file pages that have not already been reserved,
 *	and are therefore available for admitting new clients.
 *
 * Results:
 *	Sets "reservedMem", "availMem", "autoMinMem", "reservedSwap",
 *	and "availSwap" values based on current reservations.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
MemSched_CheckReserved(int32 *availMem,
                       int32 *reservedMem,
                       int32 *autoMinMem,
                       int32 *availSwap,
                       int32 *reservedSwap)
{
   Bool swapEnabled;

   // check if swapping enabled
   swapEnabled = Swap_IsEnabled();

   // acquire lock
   MemSchedLock();

   // invoke primitives
   MemSchedReservedMem(swapEnabled, availMem, reservedMem, autoMinMem);
   MemSchedReservedSwap(swapEnabled, availSwap, reservedSwap);

   // release lock
   MemSchedUnlock();
}


/*
 *----------------------------------------------------------------------
 *
 * MemSchedAdmit --
 *
 *      Performs admission control check for "world" using client's
 *      memory configuration.
 *
 *	Checks that unreserved machine memory and unreserved swap space 
 *	are sufficient to accept "world" into system.
 *
 *      Caller must hold MemSched lock.
 *
 * Results:
 *	Returns VMK_OK if successfully admitted, otherwise error code.
 *	Modifies memsched alloc passed in.
 *
 * Side effects:
 *      Memsched alloc "min" and "autoMin" may be modified.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
MemSchedAdmit(const World_Handle *world,      // IN
              Bool vmResuming,                // IN
              MemSched_Alloc *alloc)          // IN/OUT
{
   MemSched_Client *c = ClientFromWorld(world);
   int32 availMem, reservedMem, autoMinMem, availSwap, reservedSwap;
   int32 clientReserveMem;
   int availHeap, needHeap;
   int minKVMapEntries, availKVMapEntries;
   Bool swapEnabled;
    
   ASSERT(World_IsVMMWorld(world));
   ASSERT(MemSchedIsLocked());

   if (alloc->max > MBToPages(VMMEM_MAX_SIZE_MB)) {
      VmWarn(world->worldID, "does not support guest more than %d MB, guest mem: %d MB",
             VMMEM_MAX_SIZE_MB, PagesToMB(alloc->max));
      return VMK_LIMIT_EXCEEDED;
   }

   // admission control check without lock:
   //   ensure sufficient vmkernel heap space
   needHeap = CONFIG_OPTION(MEM_ADMIT_HEAP_MIN);
   availHeap = Mem_Avail() / 1024;
   if (availHeap < needHeap) {
      // fail: return error
      VmWarn(world->worldID,
             "insufficient heap: avail=%dK, need=%dK",
             availHeap, needHeap);
      return(VMK_NO_MEMORY);
   }

   // debugging
   VMLOG(0, world->worldID,
         "heap OK: avail=%dK, need=%dK",
         availHeap, needHeap);

   //   ensure sufficient KVMap entries
   minKVMapEntries = CONFIG_OPTION(KVMAP_ENTRIES_MIN);
   availKVMapEntries = KVMap_NumEntriesFree();
   if (availKVMapEntries < minKVMapEntries) {
      // fail: return error
      VmWarn(world->worldID,
             "insufficient system map entries: avail=%d, need=%d",
             availKVMapEntries, minKVMapEntries);
      return(VMK_NO_MEMORY);
   }

   // obtain reserved memory, swap totals
   swapEnabled = Swap_IsEnabled();
   MemSchedReservedMem(swapEnabled, &availMem, &reservedMem, &autoMinMem);
   MemSchedReservedSwap(swapEnabled, &availSwap, &reservedSwap);   
   availSwap = MAX(0, availSwap);

   // issue warnings if min overridden
   if (!swapEnabled && alloc->min < alloc->max) {
      VmWarn(world->worldID,
             "swap disabled: reserve max size=%dK",
             PagesToKB(alloc->max));
      alloc->min = alloc->max;
      alloc->autoMin = FALSE;
      ASSERT(availSwap == 0);
   }

   // admission control check:

   // subtract non-reclaimable memory currently used by vmx
   availMem -= UserClientCurrentUsage(c)->pageable;

   // ensure sufficient memory
   clientReserveMem = alloc->min;

   // handle extra memory reservation when resuming
   if (vmResuming) {
      // ensure minimal amount of memory reserved when resuming,
      //   since some locked pages may not be immediately swappable
      if (clientReserveMem < MEMSCHED_RESUME_MIN_RESERVE) {
         clientReserveMem = MIN(MEMSCHED_RESUME_MIN_RESERVE, alloc->max);
         VMLOG(0, world->worldID,
               "resuming: require non-overhead reserved=%dK",
               PagesToKB(clientReserveMem));
      }

      // reserve more memory while resuming, if available
      //   done to reduce/eliminate swapping for undercommitted resumes
      if (availMem > clientReserveMem) {
         clientReserveMem = MIN(alloc->max, availMem);
         // debugging
         VMLOG(0, world->worldID,
               "resuming: reserved=%dK/%dK",
               PagesToKB(clientReserveMem), PagesToKB(alloc->max));
      }

      Alloc_AllocInfo(world)->maxCptPagesToRead = clientReserveMem;
   }

   if (availMem < clientReserveMem) {
      int32 needMem, reclaimMem;

      // can reclaim existing auto-min pages, limited by swap space
      needMem = clientReserveMem - availMem;
      reclaimMem = MIN(autoMinMem, availSwap);
      
      if (reclaimMem < needMem) {
         // fail: return error
         VmWarn(world->worldID,
                "insufficient memory: avail=%dK (%dK + %dK), need=%dK",
                PagesToKB(availMem + reclaimMem),
                PagesToKB(availMem),
                PagesToKB(reclaimMem),
                PagesToKB(clientReserveMem));
         return(VMK_NO_MEMORY);
      }
   }

   // ensure sufficient total memory and swap space for max
   if (alloc->max > availMem + availSwap) {
      // fail: return error
      VmWarn(world->worldID,
             "insufficient swap: enabled=%d avail=%dK, need=%dK",
             swapEnabled,
             PagesToKB(availSwap),
             PagesToKB(alloc->max - availMem));
      return(VMK_NO_MEMORY);
   }

   // debugging
   VMLOG(0, world->worldID,
         "admitted: min=%dK reserved mem=%dK swap=%dK",
         PagesToKB(alloc->min),
         PagesToKB(clientReserveMem),
         PagesToKB(alloc->max - clientReserveMem));

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedStateToString --
 *
 *      Returns human-readable string representation of state "n",
 *	or the string 'unknown' if "n" is not a valid state.
 *
 * Results:
 *      Returns human-readable string representation of state "n".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static const char *
MemSchedStateToString(MemSchedState n)
{
   switch (n) {
   case MEMSCHED_STATE_HIGH:
      return("high");
   case MEMSCHED_STATE_SOFT:
      return("soft");
   case MEMSCHED_STATE_HARD:
      return("hard");
   case MEMSCHED_STATE_LOW:
      return("low");
   default:
      return("unknown");
   }
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedFreeStateInit --
 *
 *      Initializes free state "s", setting up state transition table
 *	based on compile-time memory percentage parameters (may use
 *	config options in the future).
 *
 * Results:
 *      Modifies "s".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
MemSchedFreeStateInit(MemSchedFreeState *s, uint32 nPages)
{
   uint32 highPages, softPages, hardPages, lowPages, onePct;
   MemSchedStateTransition *t;

   // useful fraction
   onePct = nPages / 100;

   // threshold levels in pages
   highPages = MEMSCHED_FREE_HIGH_PCT * onePct;
   softPages = MEMSCHED_FREE_SOFT_PCT * onePct;
   hardPages = MEMSCHED_FREE_HARD_PCT * onePct;
   lowPages  = MEMSCHED_FREE_LOW_PCT  * onePct;

   // initialize
   s->state = MEMSCHED_STATE_HIGH;
   s->lowThreshold= softPages;
   s->highThreshold = nPages;
   SP_InitLockIRQ("MemSchedStateLock", &s->lock, SP_RANK_MEMSCHED_STATE);

   // initialize state transition table

   // HIGH: no memory reclamation
   t = &s->table[MEMSCHED_STATE_HIGH];
   t->state = MEMSCHED_STATE_HIGH;
   t->lowState = MEMSCHED_STATE_SOFT;
   t->lowPages = softPages;
   t->highState = MEMSCHED_STATE_HIGH;
   t->highPages = nPages;

   // SOFT: preferentially use ballooning
   t = &s->table[MEMSCHED_STATE_SOFT];
   t->state = MEMSCHED_STATE_SOFT;
   t->lowState = MEMSCHED_STATE_HARD;
   t->lowPages = hardPages;
   t->highState = MEMSCHED_STATE_HIGH;
   t->highPages = highPages;

   // HARD: preferentially use swapping
   t = &s->table[MEMSCHED_STATE_HARD];
   t->state = MEMSCHED_STATE_HARD;
   t->lowState = MEMSCHED_STATE_LOW;
   t->lowPages = lowPages;
   t->highState = MEMSCHED_STATE_SOFT;
   t->highPages = (hardPages + softPages) / 2;
   
   // LOW: swap, block VM until reaches target
   t = &s->table[MEMSCHED_STATE_LOW];
   t->state = MEMSCHED_STATE_LOW;   
   t->lowState = MEMSCHED_STATE_LOW;
   t->lowPages = 0;
   t->highState = MEMSCHED_STATE_HARD;
   t->highPages = (lowPages + hardPages) / 2;
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedUpdateThreshold --
 *
 *      Update the low and high threshold for the trigger.
 *      Caller must hold MemSchedFreeState lock.
 *
 * Results:
 *	low and high are set at the end of the function.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
MemSchedUpdateThreshold(uint32 free)
{
   MemSched *m = &memSched;
   MemSchedStateTransition *t;
   uint32 lowPages, highPages;

   ASSERT(MemSchedFreeStateIsLocked());
   t = MemSchedCurrentStateTransition();

   // adjust thresholds based on min realloc delta
   highPages = MIN(t->highPages, free + m->reallocPages);
   if (free > m->reallocPages) {
      lowPages = MAX(t->lowPages, free - m->reallocPages);
   } else {
      lowPages = t->lowPages;
   }

   // adjust low threshold in low state 
   if (free <= MemSchedLowFree()) {
      // force callback each time free space drops in half
      uint32 halfFree = free / 2;
      if (halfFree > lowPages) {
         // debugging
         if (MEMSCHED_DEBUG_TRIGGER) {
            LOG(0, "adjusted low threshold: %dK -> %dK",
                PagesToKB(lowPages), PagesToKB(halfFree));
         }

         // adjust threshold
         lowPages = halfFree;
      }
   }

   m->freeState.lowThreshold = lowPages;
   m->freeState.highThreshold = highPages;
}

/*
 *----------------------------------------------------------------------
 *
 * MemSched_UpdateFreePages --
 *
 *	Callback function invoked from MemMap module when the
 *	number of free pages crosses a threshold specified
 *	when the callback was registered.  The current number
 *	of free pages is supplied as "nPages".
 *
 * Results:
 *	*lowPages and *highPages are updated with new thresholds.
 *
 * Side effects:
 *      Reallocates memory among worlds managed by memory scheduler.
 *
 *----------------------------------------------------------------------
 */
void
MemSched_UpdateFreePages(uint32 nPages)
{
   MemSched *m = &memSched;
   MemSchedFreeState *s = &memSched.freeState;
   MemSchedStateTransition *t;
   MemSchedState prevState;
   SP_IRQL prevIRQL;

   // handle common case: check threshold w/o lock
   if (nPages >= s->lowThreshold && nPages <= s->highThreshold) {
      return;
   }

   prevIRQL = MemSchedFreeStateLock();

   // check threshold again with lock
   if (UNLIKELY(nPages >= s->lowThreshold && nPages <= s->highThreshold)) {
      MemSchedFreeStateUnlock(prevIRQL);
      return;
   }

   // perform state transition
   prevState = m->freeState.state;
   t = &m->freeState.table[prevState];
   if (nPages < t->lowPages) {
      m->freeState.state = t->lowState;
      t->lowCount++;
   } else if (nPages > t->highPages) {
      m->freeState.state = t->highState;
      t->highCount++;
   }

   // update stats
   m->freeState.triggerCount++;

   // debugging
   if (MEMSCHED_DEBUG_TRIGGER) {
      LOG(1, "%s -> %s: %dK free",
          MemSchedStateToString(prevState),
          MemSchedStateToString(MemSchedCurrentState()),
          PagesToKB(nPages));
   }

   // warn if memory below half of low threshold
   if (nPages <= MemSchedLowFree() / 2) {
      SysAlert("memory low: %dK free", PagesToKB(nPages));
   }

#ifdef VMX86_DEBUG
   // Are we entering the low state?
   if (prevState != m->freeState.state &&
       m->freeState.state == MEMSCHED_STATE_LOW) {
      // Reset the num of MPNs alloced and released in low state.
      FORALL_MEMSCHED_VMM_CLIENTS(&m->schedQueue, c, vmm) {
         vmm->lowStateMPNAllocated = 0;
         vmm->lowStateOvhdMPNAllocated = 0;
         vmm->lowStateMPNReleased = 0;
         vmm->lowStateSwapTarget = Swap_GetSwapTarget(vmm->world);
         vmm->lowStateSwapped = c->snapshot.swapped;
         vmm->lowStateAlloc = c->commit.alloc;
         vmm->lowStateLocked = c->snapshot.locked;
         vmm->lowStateFree = 0;
         vmm->lowStateFreeAmt = 0;
      } MEMSCHED_VMM_CLIENTS_DONE;
   }
#endif

   // reallocate
   if (m->freeState.state == MEMSCHED_STATE_LOW) {
      MemSchedReallocReqFast();
   }

   // update threshold 
   MemSchedUpdateThreshold(nPages);

   MemSchedFreeStateUnlock(prevIRQL);
}


/*
 *----------------------------------------------------------------------
 *
 * MemSched_HostShouldWait --
 *
 *      Checks if host operations on behalf of "world" should wait
 *	by polling/retrying until enough memory has been reclaimed to
 *	continue executing.  
 *
 * Results:
 *	Returns TRUE iff host operations on behalf of "world"
 *	should wait by polling/retrying pending memory.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Bool
MemSched_HostShouldWait(World_Handle *world)
{
   // wait if memory is low, but avoid rare potential deadlock
   if (MemSched_MemoryIsLow()) {
      MemSched_Client *c = ClientFromWorld(world);
      MemSchedVmm *vmm = &c->vmm;

      // The system may need this VM to swap in order to free up memory
      // and resolve the low memory condition.  Check if the world is
      // currently waiting for a blocking RPC to the COS to complete;
      // in this case, spinning/blocking indefinitely risks deadlock.
      // So we allow the world to allocate memory at a very slow rate
      // (approximately one page per MEMSCHED_HOST_WAIT_SKIP_TIMEOUT).

      if ((world->sched.cpu.vcpu.runState  == CPUSCHED_WAIT) &&
          (world->sched.cpu.vcpu.waitState == CPUSCHED_WAIT_RPC)) {
         uint64 now = Timer_SysUptime();
         
         // RPC case: permit infrequent allocations
         if (now > vmm->hostWaitSkip) {
            // update next wait skip time
            MemSchedDebug(world->worldID, "in RPC wait: skip");
            vmm->hostWaitSkip = now + MEMSCHED_HOST_WAIT_SKIP_TIMEOUT;
            return(FALSE);
         }
      } else {
         // non-RPC case: reset next wait skip time
         vmm->hostWaitSkip = 0;
      }

      return(TRUE);
   }
   
   // sufficient memory
   return(FALSE);
}


/*
 *----------------------------------------------------------------------
 *
 * MemSched_UserWorldShouldBlock --
 *
 *      Checks if user world should block on memory resource.
 *
 * Results:
 *	Returns TRUE iff the user world should block on memory resource.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Bool
MemSched_UserWorldShouldBlock(const World_Handle *world)
{
   MemSched_Client *c = ClientFromWorld(world);
   MemSchedUserUsage *userUsage = UserClientCurrentUsage(c);

   ASSERT(c->user.valid);
   // wait if memory is low and we are overcommitted
   return (MemSched_MemoryIsLow() && 
           !c->vmm.valid && userUsage->pageable > c->commit.alloc);
}


/*
 *----------------------------------------------------------------------
 *
 * MemSched_UserWorldBlock --
 *
 *      Block until the current world can allocate more memory.
 *
 * Results:
 *      VMK_OK if successful and VMK_DEATH_PENDING 
 *      if the current world is dying.
 *
 * Side effects:
 *      Wait for memsched daemon to reschedule.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
MemSched_UserWorldBlock(void)
{
   VMK_ReturnStatus status;

   MemSchedLock();
   do {
      // request for memory reschedule
      MemSchedReallocReqSlow();
      // wait for memory reschedule to finish
      status = MemSchedReallocWaitLock();
      if (status != VMK_OK) {
         break;
      }
   } while (MemSched_UserWorldShouldBlock(MY_RUNNING_WORLD));
   MemSchedUnlock();

   ASSERT(status == VMK_OK || status == VMK_DEATH_PENDING);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * MemSched_BlockWhileMemLow --
 *
 *      Checks if specified "world" should enter a blocking memory
 *	wait state. If yes, block the current world in a memory wait 
 *	state until sufficient memory is available to safely continue 
 *	execution or the VM is ready to start swapping pages.
 *
 * Results:
 *	Returns TRUE iff "world" should block pending memory.
 *
 * Side effects:
 *      May cause pages from this VM to be swapped.
 *
 *----------------------------------------------------------------------
 */
void
MemSched_BlockWhileMemLow(World_Handle *inWorld)
{
   World_Handle *world;
   MemSched_Client *c;
   MemSchedVmm *vmm;
   MemSchedVmmUsage *vmmUsage;

   if (!World_IsVMMWorld(inWorld)) {
      return;
   }
   world = World_GetVmmLeader(inWorld);
   ASSERT(world);

   // don't wait if checkpoint or resume in progress
   if (Alloc_AllocInfo(world)->duringCheckpoint) {
      return;
   }

   c = ClientFromWorld(world);
   vmm = &c->vmm;

   // don't wait if unable to block
   if (!ClientCanWait(c)) {
      return;
   }

   vmmUsage = VmmClientCurrentUsage(c);
   // handle early waits
   while (ClientEarlyShouldWait(c)) {
      // debugging
      if (MEMSCHED_DEBUG_EARLY_WAIT) {
         MemSchedDebug(world->worldID,
                       "early wait: "
                       "valid=%d, vmmStarted=%d, "
                       "lock=%u/%u, overhd=%u",
                       vmm->valid, vmm->vmmStarted,
                       vmmUsage->locked, c->commit.alloc, ClientCurrentOverhead(c));
      }

      // terminate wait unless simply timed out
      //   note that MEMSCHED_EARLY_TIMEOUT value is essentially arbitrary,
      //   only really changes frequency of debugging output above
      if (MemSched_MemoryIsLowWait(MEMSCHED_EARLY_TIMEOUT) != VMK_TIMEOUT) {
         return;
      }
   }

   // See if the swapper needs to block this VM
   if (MemSched_ShouldSwapBlock(vmm->swapTarget, vmmUsage->swapped) ||
       (VMK_STRESS_RELEASE_OPTION(MEM_SWAP) && Swap_IsEnabled())) {
      // block until vm is ready to swap
      Swap_BlockUntilReadyToSwap(world);
      // Update swap targets for VM, since it is now ready to swap
      MemSchedClientUpdateSwap(c);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * MemSched_MemoryIsLow --
 *
 *      Returns TRUE iff the system is currently low on memory.
 *
 * Results:
 *	Returns TRUE iff the system is currently low on memory.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Bool
MemSched_MemoryIsLow(void)
{
   MemSchedState state;

   // poll current state without locking
   state = MemSchedCurrentState();
   return(state == MEMSCHED_STATE_LOW);
}

/*
 *----------------------------------------------------------------------
 *
 * MemSched_MemoryIsHigh --
 *
 *      Returns TRUE iff the system is currently high on memory.
 *
 * Results:
 *      Returns TRUE iff the system is currently high on memory.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Bool
MemSched_MemoryIsHigh(void)
{
   MemSchedState state;

   // poll current state without locking
   state = MemSchedCurrentState();
   return(state == MEMSCHED_STATE_HIGH);
}

/*
 *----------------------------------------------------------------------
 *
 * MemSched_MemoryIsLowWait --
 *
 *      Wait while system is low on memory.
 *
 * Results:
 *	Returns VMK_OK if system is not currently low on memory, or
 *	VMK_TIMEOUT if exceeded timeout waiting for memory.
 *
 * Side effects:
 *      May block caller.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
MemSched_MemoryIsLowWait(uint32 msTimeout)
{
   World_Handle *world = MY_RUNNING_WORLD;
   uint64 startTime;

   // done if system not low on memory
   if (!MemSched_MemoryIsLow()) {
      return(VMK_OK);
   }

   // track start time
   startTime = Timer_SysUptime();

   // poll memory state
   while (MemSched_MemoryIsLow()) {
      // prematurely terminate wait, if necessary
      if (Alloc_AllocInfo(world)->startingCheckpoint || world->deathPending) {
         MemSchedDebug(world->worldID,
                       "premature termination: sc=%d, dp=%d",
                       Alloc_AllocInfo(world)->startingCheckpoint,
                       world->deathPending);
         break;
      }
      
      // fail if exceed timeout
      if (Timer_SysUptime() >= (startTime + msTimeout)) {
         return(VMK_TIMEOUT);
      }

      if (vmx86_debug && !INTERRUPTS_ENABLED()) {
         VmWarn(world->worldID, "sleeping with interrupts disabled");
      }

      // wait for memory to free up
      CpuSched_Sleep(1);
   }

   // debugging
   if (MEMSCHED_DEBUG_LOW_WAIT) {
      MemSchedDebug(MY_RUNNING_WORLD->worldID,
                    "waited %Lu ms",
                    Timer_SysUptime() - startTime);
   }
   
   // succeed
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * MemSched_MonitorStarted --
 *
 *      Callback in response to VMK_ACTION_MEM_VMM_START action,
 *	indicating that the monitor associated with the current world
 *	is ready to process memory actions.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      Updates state associated with current world to indicate that
 *	its associated monitor is ready to process memory actions.
 *
 *----------------------------------------------------------------------
 */
VMKERNEL_ENTRY
MemSched_MonitorStarted(DECLARE_0_ARGS(VMK_MEM_VMM_STARTED))
{
   World_Handle *world;
   MemSched_Client *c;
   MemSchedVmm *vmm;
   
   PROCESS_0_ARGS(VMK_MEM_VMM_STARTED);

   world = MY_VMM_GROUP_LEADER;

   // update "monitor started" flag
   c = ClientFromWorld(world);
   vmm = &c->vmm;
   vmm->vmmStarted = TRUE;

   LOG_ONLY(
         // debugging
         if (MEMSCHED_DEBUG_EARLY_WAIT) {
         MemSchedVmmUsage *vmmUsage = VmmClientCurrentUsage(c);      
         MemSchedDebug(world->worldID,
            "valid=%d, vmmStarted=%d, "
            "lock=%u/%u, overhd=%u",
            vmm->valid, vmm->vmmStarted,
            vmmUsage->locked, c->commit.alloc, ClientCurrentOverhead(c));
         };
   )
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * MemSched_TerminateCptOnLowMem --
 *
 *      Decide if we should terminate the checkpoint/suspend operation.
 *
 * Results:
 *      TRUE if the number of free pages is less than a very low threshold,
 *      FALSE otherwise
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
#define MEMSCHED_CPT_LOWMEM_THRESHOLD (256)     // 1M
Bool
MemSched_TerminateCptOnLowMem(World_Handle *world)
{
   return(MemMap_UnusedPages() < MEMSCHED_CPT_LOWMEM_THRESHOLD);
}

/*
 *----------------------------------------------------------------------
 *
 * MemSched_SetMaxCptInvalidPages --
 *
 *      Wrapper to set the maximum number of invalid overhead pages
 *      touched by a VM during checkpoint.
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
MemSched_SetMaxCptInvalidPages(uint32 numPages)
{
   if (numPages > memSched.maxCptInvalidOvhdPages) {
      memSched.maxCptInvalidOvhdPages = numPages;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * MemSched_NumaMigrateVMMCallback --
 *
 *     Tells "world"'s monitor to flush all its caches so that they
 *     can be reallocated on the world's current home node
 *
 * Results:
 *     none.
 *
 * Side effects:
 *     Queues action to world.
 *     Brutal performance impact from flushing TC and MMU caches,
 *     so use sparingly.
 *
 *----------------------------------------------------------------------
 */
static void 
MemSchedMigrateVMMCallback(World_Handle *world, UNUSED_PARAM(void *ignored))
{
   MemSched_Client *c = ClientFromWorld(world);
   MemSchedVmm *vmm = &c->vmm;

   ASSERT(World_IsVMMWorld(world));

   if (!vmm->valid || !vmm->vmmStarted) {
      VMLOG(0, world->worldID, "cannot remap vmm for invalid client");
      return;
   }

   Action_Post(world, vmm->numaMigrateAction);
   VMLOG(0, world->worldID, "migrated vmm to new node");
}

/*
 *-----------------------------------------------------------------------------
 *
 * MemSched_NumaMigrateVMM --
 *
 *     Intitates monitor migration for all worlds in group led by "leader"
 *
 * Results:
 *     Returns VMK_OK on success, VMK_BUSY if migration already in progress
 *
 * Side effects:
 *     Updates vsmp numa structure. Also, see NumaMigrateVMMCallback above.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
MemSched_NumaMigrateVMM(World_Handle *leader)
{
   CpuSched_Vsmp *vsmp = World_CpuSchedVsmp(leader);
   Timer_AbsCycles now;
   VMK_ReturnStatus res;

   if (!World_IsVMMWorld(leader)) {
      VMLOG(0, leader->worldID, "only a vmm world can remap its vmm");
      return (VMK_BAD_PARAM);
   }

   now = Timer_GetCycles();
   if (now < vsmp->numa.nextMigrateAllowed) {
      return (VMK_BUSY);
   }

   res = CpuSched_ForallGroupMembersDo(leader, MemSchedMigrateVMMCallback, NULL);
   ASSERT(res == VMK_OK);

   vsmp->numa.nextMigrateAllowed = now +
      (CONFIG_OPTION(NUMA_MONMIG_TIME) * Timer_CyclesPerSecond());
   vsmp->numa.lastMonMigMask = MemSched_NodeAffinityMask(leader);
   
   return (VMK_OK);
}


/*
 *----------------------------------------------------------------------
 *
 * MemSched_SetSwapReqTimeStamp --
 *      
 *      Wrapper to set the time stamp when the swap request was issued to 
 *      the monitor.
 *              
 *      Note: Caller must serialze the operations to set the time stamp
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
MemSched_SetSwapReqTimeStamp(World_Handle *world, 
                             uint64 mSec)
{
   MemSchedVmm *vmm = VmmClientFromWorld(world);
   // No need to hold any locks here because the caller
   // serializes multiple writes
   vmm->swapReqTimeStamp = mSec;
}


/*
 *----------------------------------------------------------------------
 *
 * MemSched_UpdateMinFree --
 *      
 *    Update minFree to the new value only if 
 *    o *no* VMs are currently running, as we don't want to deal with 
 *      the effects this may have on the mins of running VMs.
 *    o the new new value is greater than MEMSCHED_FREE_HIGH_PCT
 *              
 * Results:
 *    none.
 *
 * Side effects:
 *    If minFree is increased VMs may feel more memory pressure 
 *    and vice versa.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
MemSched_UpdateMinFree(Bool write,
                       Bool valueChanged,
                       UNUSED_PARAM(int ndx))
{
   MemSched *m = &memSched;
   // acquire lock
   MemSchedLock();   

   if (write) {
      uint32 newPct = CONFIG_OPTION(MEM_MIN_FREE);
      uint32 pct;
      uint32 onePct;
      MemSchedStateTransition *t = &m->freeState.table[MEMSCHED_STATE_SOFT];
      SP_IRQL prevIRQL;

      if (m->numScheds != 0) {
         Warning("Minimum free memory can only be changed when no "
                 "VMs are running");
         MemSchedUnlock();
         return VMK_NOT_SUPPORTED;
      }

      onePct = MemMap_ManagedPages()/100;
      pct = MAX(newPct, MEMSCHED_FREE_HIGH_PCT);

      prevIRQL = MemSchedFreeStateLock();
      /*
       * Update high pages for the 'soft state' for 
       * correct transition between high and soft states.
       */
      t->highPages = pct * onePct;
      ASSERT(t->highPages == MemSchedMinFree());
      MemSchedFreeStateUnlock(prevIRQL);
   }

   // release lock
   MemSchedUnlock();
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * MemSched_GetLoadMetrics --
 *
 *      Sets "load" to reflect current memory load metrics.
 *
 * Results:
 *      Modifies "load".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
MemSched_GetLoadMetrics(MemSched_LoadMetrics *load)
{
   MemSched *m = &memSched;
   uint64 managed, maxSize, maxOverhead, balloon, swap;

   // initialize
   memset(load, 0, sizeof(MemSched_LoadMetrics));
   maxSize = 0;
   maxOverhead = 0;
   balloon = 0;
   swap = 0;

   // snapshot current stats
   MemSchedLock();
   FORALL_MEMSCHED_VMM_CLIENTS(&m->schedQueue, c, vmm) {
      MemSched_Info *info = VmmClientSharedData(vmm);
      MemSchedVmmUsage *vmmUsage = VmmClientCurrentUsage(c);

      maxSize += c->alloc.max;
      maxOverhead += c->overhead;
      balloon += info->balloon.size;
      swap += vmmUsage->swapped;
   } MEMSCHED_VMM_CLIENTS_DONE;
   MemSchedUnlock();

   // metrics expressed as percentage of VM memory
   if (maxSize > 0) {
      load->balloon = (100 * balloon) / maxSize;
      load->swap = (100 * swap) / maxSize;
      load->reclaim = (100 * (balloon + swap)) / maxSize;
   }
   
   // metrics  expressed as percentage of managed memory
   managed = MemMap_ManagedPages();
   if (managed > 0) {
      uint64 maxVm = maxSize + maxOverhead;
      load->free = (100 * MemMap_UnusedPages()) / managed;
      if (maxVm > managed) {
         load->overcommit = (100 * (maxVm - managed)) / managed;
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * MemSched_GroupStateInit --
 *
 * 	Initializes memory scheduler state for the group
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
MemSched_GroupStateInit(MemSched_GroupState *s)
{
   memset(s, 0, sizeof(*s));
}

/*
 *----------------------------------------------------------------------
 *
 * MemSched_GroupStateCleanup --
 * 
 *	Cleans up memory scheduler state for the group.
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
MemSched_GroupStateCleanup(MemSched_GroupState *s)
{
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedNodeIsGroup --
 * 
 *	Checks if the specified node is a scheduler group. Caller 
 *	must hold scheduler tree lock.
 *
 * Results: 
 *      TRUE if node is a scheduler group; FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
MemSchedNodeIsGroup(const Sched_Node *n)
{
   ASSERT(Sched_TreeIsLocked());

   if (n->nodeType == SCHED_NODE_TYPE_GROUP) {
         return (TRUE);
   }
   return (FALSE);
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedNodeIsMemClient --
 * 
 *	Checks if the specified node is a memory scheduler client
 *	group. Caller must hold scheduler tree lock.
 *
 * Results: 
 *      TRUE if node represents a memory scheduler client group; 
 *      FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
MemSchedNodeIsMemClient(const Sched_Node *n)
{
   ASSERT(Sched_TreeIsLocked());

   if (n->nodeType == SCHED_NODE_TYPE_GROUP) {
      Sched_Group *group = n->u.group;

      if (group->flags & SCHED_GROUP_IS_MEMSCHED_CLIENT) {
         return (TRUE);
      }
   }
   return (FALSE);
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedGroupSnapshot --
 *
 * 	Snapshots current memory resource related state of group "g"
 * 	into memSched snapshot area, and increments counter specified
 * 	by the "data" parameter. Caller must hold scheduler tree lock.
 *
 * Results: 
 *      None.
 *
 * Side effects:
 *      Updates memSched snapshot area and increments counter specified
 *      by "data."
 *
 *----------------------------------------------------------------------
 */
static void
MemSchedGroupSnapshot(Sched_Group *g, void *data)
{
   Sched_Group *parent = Sched_TreeGroupParent(g);
   uint32 *snapCount = (uint32 *)data;
   MemSchedGroupSnap *s;

   ASSERT(Sched_TreeIsLocked());

   // Find correct slot to store snapshot
   s = &memSched.group[*snapCount];

   // Snapshot group identity
   s->groupID = g->groupID;
   strncpy(s->groupName, g->groupName, SCHED_GROUP_NAME_LEN);

   // Snapshot parent identity
   if (parent == NULL) {
      s->parentID = 0;
      strncpy(s->parentName, "none", SCHED_GROUP_NAME_LEN);
   } else {
      s->parentID = parent->groupID;
      strncpy(s->parentName, parent->groupName, SCHED_GROUP_NAME_LEN);
   }

   // Snapshot relevant group state
   s->members = s->clients = 0;
   FORALL_GROUP_MEMBER_NODES(g, node) {
      if (MemSchedNodeIsGroup(node)) {
         s->members++;				// found a member
         if (MemSchedNodeIsMemClient(node)) {
            s->clients++;			// member is mem client
         }
      }
   } GROUP_MEMBER_NODES_DONE;
   ASSERT(s->members <= g->members.len);
   
   s->state = *(&g->mem);

   ASSERT(*snapCount < SCHED_GROUPS_MAX);
   (*snapCount)++;				// update count
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedGroupSnapFormat --
 * 
 *	Formats and writes memory resource related information for
 *	scheduler group snapshot "s" into "buf."
 *
 * Results: 
 *      None.
 *
 * Side effects:
 *      Increments "len" by the number of characters written to "buf."
 *
 *----------------------------------------------------------------------
 */
static void
MemSchedGroupSnapFormat(const MemSchedGroupSnap *s, char *buf, int *len)
{
   const MemSched_GroupState *m = &s->state;

   Proc_Printf(buf, len,
	       "%5u %-12s "
	       "%5u %-12s "
	       "%4u %7u"
	       "%6u %6u %7u   %7u %7u "
	       "%6u %6u %6u %6u %7u "
	       "\n",
	       s->groupID,
	       s->groupName,
	       s->parentID,
	       s->parentName,
	       s->members,
	       s->clients,
	       PagesToMB(m->alloc.min),
	       PagesToMB(m->alloc.max),
	       m->alloc.shares,
	       PagesToMB(m->alloc.minLimit),
	       PagesToMB(m->alloc.hardMax),
	       PagesToMB(m->base.min),
	       PagesToMB(m->base.max),
	       PagesToMB(m->base.emin),
	       PagesToMB(m->base.emax),
	       m->base.shares);
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedTotalChildBaseValues --
 *
 * 	Sums up the base mins, base maxs, effective mins and effective
 * 	maxs respectively for every group that is an immediate child of 
 * 	the specified group. Caller must hold scheduler tree lock.
 *
 * Results: 
 * 	Results returned in output parameter "totalChildBase."
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
MemSchedTotalChildBaseValues(const Sched_Group *group, 
		             MemSched_AllocInt *totalChildBase)
{
   uint32 i;

   ASSERT(Sched_TreeIsLocked());
   ASSERT(SCHED_NODE_IS_GROUP(group->node));

   totalChildBase->min = 0;
   totalChildBase->max = 0;
   totalChildBase->emin = 0;
   totalChildBase->emax = 0;
   for (i = 0; i < group->members.len; i++) {
      Sched_Node *node = group->members.list[i];

      if (SCHED_NODE_IS_GROUP(node)) {
         Sched_Group *childGroup = node->u.group;
         MemSched_GroupState *childGroupState = &childGroup->mem;

         totalChildBase->min += childGroupState->base.min;
         totalChildBase->max += childGroupState->base.max;
         totalChildBase->emin += childGroupState->base.emin;
         totalChildBase->emax += childGroupState->base.emax;
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedComputeBaseMinMax --
 * 
 *	This routine computes the base min, base max, effective min 
 *	and effective max parameters for the specified group and all 
 *	affected parents up to the "root" node of the scheduler tree. 
 *	The caller must hold scheduler tree lock.
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
MemSchedComputeBaseMinMax(Sched_Group *group)
{
   Sched_Group *g = group;			// working copy
   MemSched_GroupState *groupState;

   ASSERT(Sched_TreeIsLocked());
   ASSERT(SCHED_NODE_IS_GROUP(g->node));

   if (MemSchedNodeIsMemClient(g->node)) {
      /*
       * The specified group is a memory scheduler client and 
       * consequently its base min and max are the same as 
       * its allocated min and max respectively.
       */
      groupState = &g->mem;

      groupState->base.min = groupState->alloc.min;
      groupState->base.max = groupState->alloc.max;
      groupState->base.emin = groupState->alloc.min;
      groupState->base.emax = groupState->alloc.max;

      // climb up the  tree to parent group.
      g = Sched_TreeGroupParent(g);
      ASSERT(g != NULL);
   }

   do {
      MemSched_AllocInt totalChildBase;

      ASSERT(!MemSchedNodeIsMemClient(g->node));

      groupState = &g->mem;

      // Sum up base values for all immediate children
      MemSchedTotalChildBaseValues(g, &totalChildBase);

      /*
       * The base min for the group is the sum of the base mins of
       * all its immediate children.
       */
      groupState->base.min = totalChildBase.min;
      ASSERT(groupState->base.min <= groupState->alloc.minLimit);

      /*
       * The base max for the group is the sum of the base maxs of
       * all its immediate children.
       */
      groupState->base.max = totalChildBase.max;
      ASSERT(groupState->base.max <= groupState->alloc.max);

      /*
       * The effective min for the group is the sum of the effective mins 
       * of all its immediate children, but never less than its own
       * allocated min.
       */
      groupState->base.emin = MAX(groupState->alloc.min, totalChildBase.emin);
      ASSERT(groupState->base.emin <= groupState->alloc.minLimit);

      /*
       * The effective max for the group is the sum of the effective maxs 
       * of all its immediate children, but never less than its own
       * allocated hard max.
       */
      groupState->base.emax = MAX(groupState->alloc.hardMax, 
		                  totalChildBase.emax);
      ASSERT(groupState->base.emax <= groupState->alloc.max);

   } while ((g = Sched_TreeGroupParent(g)) != NULL);
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedComputeBaseShares --
 *
 *	Recursively descends down the scheduler groups tree, starting
 *	at the specified group, and computes memory resource related 
 *	base shares for each group that is encountered. The caller must 
 *	hold scheduler tree lock.
 *
 *	Note: Base shares are not currently used by the memory scheduler.
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
MemSchedComputeBaseShares(Sched_Group *group)
{
   int i;
   uint32 totalShares;
   Sched_Node *node;
   Sched_Group *childGroup;
   MemSched_GroupState *groupState;
   MemSched_GroupState *childGroupState;

   ASSERT(Sched_TreeIsLocked());
   ASSERT(SCHED_NODE_IS_GROUP(group->node));

   groupState = &group->mem;

   // Seed base shares iff "root" of scheduler tree
   if (group == Sched_TreeRootGroup()) {
      groupState->base.shares = groupState->alloc.shares;
   }

   // Add up the allocation shares for all immediate child groups.
   totalShares = 0;
   for (i = 0; i < group->members.len; i++) {
      node = group->members.list[i];

      if (SCHED_NODE_IS_GROUP(node)) {
         childGroup = node->u.group;
         childGroupState = &childGroup->mem;

         totalShares += childGroupState->alloc.shares;
      } 
   }

   /* 
    * Compute base shares for all immediate child groups. Each child
    * receives a portion of the parent's base shares, based on its 
    * contribution to the total allocated shares obtained above.
    */
   for (i = 0; i < group->members.len; i++) {
      uint32 childBaseShares;

      node = group->members.list[i];

      if (SCHED_NODE_IS_GROUP(node)) {
         childGroup = node->u.group;

         childGroupState = &childGroup->mem;

         if (totalShares == 0) {
            childBaseShares = 0;
         } else {
            childBaseShares = ((uint64)groupState->base.shares * 
		               (uint64)childGroupState->alloc.shares) / 
		              totalShares;
         }
         childGroupState->base.shares = childBaseShares;

	 // Continue with the recursive descent...
	 MemSchedComputeBaseShares(childGroup);
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * MemSched_SubTreeChanged --
 *
 * 	Recomputes the base min and max values for the specified
 * 	group and all affected parents leading up to the "root" group 
 * 	of the scheduler tree. Also recomputes the base shares for the 
 * 	sub-tree rooted at the specified node. The caller must hold 
 * 	scheduler tree lock.
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
MemSched_SubTreeChanged(Sched_Group *group)
{
   Sched_Group *parentGroup;

   ASSERT(Sched_TreeIsLocked());
   ASSERT(SCHED_NODE_IS_GROUP(group->node));

   // Re-compute base "min" and "max" for group and all affected parents
   MemSchedComputeBaseMinMax(group);
   
   // Re-compute base shares for sub-tree rooted at parent group
   if ((parentGroup = Sched_TreeGroupParent(group)) == NULL) {
      ASSERT(group == Sched_TreeRootGroup());
   } else {
      group = parentGroup;
   }
   MemSchedComputeBaseShares(group);
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedAllocInit --
 *
 * 	Initializes a MemSched_Alloc structure using data contained
 * 	in the specified Sched_Alloc structure.
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
MemSchedAllocInit(MemSched_Alloc *memAlloc, const Sched_Alloc *alloc)
{
   /*
    * XXX TODO:
    *     We should manage total memory and total storage inside MemSched
    *     and let swap/memmap modules increase/decrease it.
    */
   uint32 totalMemory = MemMap_ManagedPages();
   uint32 totalStorage = totalMemory + memSched.totalSystemSwap;
   uint32 multiplier;

   ASSERT((alloc->min >= 0) || (alloc->min == SCHED_CONFIG_NONE));
   ASSERT((alloc->max >= 0) || (alloc->max == SCHED_CONFIG_NONE));
   ASSERT((alloc->minLimit >= 0) || (alloc->minLimit == SCHED_CONFIG_NONE));
   ASSERT((alloc->hardMax >= 0) || (alloc->hardMax == SCHED_CONFIG_NONE));

   // converts unit to multiplier
   switch (alloc->units) {
   case SCHED_UNITS_PAGES:
      multiplier = 1;
      break;
   case SCHED_UNITS_MB:
      multiplier = MBYTES_2_PAGES(1);
      break;
   case SCHED_UNITS_PERCENT:
      // we round down if "pct" is used as config unit
      multiplier = totalMemory / 100;
      break;
   default:
      Warning("Invalid mem alloc units: %s\n", 
              Sched_UnitsToString(alloc->units));
      ASSERT(FALSE);
      multiplier = 0;
   }

   // set min, max, minLimit and hardMax
   if (alloc->min == SCHED_CONFIG_NONE) {
      memAlloc->autoMin = TRUE;
      memAlloc->min = 0;
   } else {
      memAlloc->autoMin = FALSE;
      memAlloc->min = alloc->min * multiplier;
   }

   if (alloc->max == SCHED_CONFIG_NONE) {
      memAlloc->max = totalStorage;
   } else {
      memAlloc->max = alloc->max * multiplier;
   }

   if (alloc->minLimit == SCHED_CONFIG_NONE) {
      memAlloc->minLimit = memAlloc->max;
   } else {
      memAlloc->minLimit = alloc->minLimit * multiplier;
   }

   if (alloc->hardMax == SCHED_CONFIG_NONE) {
      memAlloc->hardMax = memAlloc->max;
   } else {
      memAlloc->hardMax = alloc->hardMax * multiplier;
   }

   // boundary checks
   memAlloc->max = MIN(memAlloc->max, totalStorage);
   memAlloc->min = MIN(memAlloc->min, totalMemory);
   memAlloc->min = MIN(memAlloc->min, memAlloc->max);
   memAlloc->minLimit = MIN(memAlloc->minLimit, totalMemory);
   memAlloc->minLimit = MIN(memAlloc->minLimit, memAlloc->max);
   memAlloc->minLimit = MAX(memAlloc->minLimit, memAlloc->min);
   memAlloc->hardMax = MIN(memAlloc->hardMax, memAlloc->max);
   memAlloc->hardMax = MAX(memAlloc->hardMax, memAlloc->min);

   // set shares alloc parameter, handle missing/special values
   if (SCHED_CONFIG_SHARES_SPECIAL(alloc->shares)) {
      switch (alloc->shares) {
      case SCHED_CONFIG_SHARES_LOW:
         memAlloc->shares = MEMSCHED_SHARES_LOW(PagesToMB(memAlloc->max));
         break;
      case SCHED_CONFIG_SHARES_HIGH:
         memAlloc->shares = MEMSCHED_SHARES_HIGH(PagesToMB(memAlloc->max));
         break;
      case SCHED_CONFIG_SHARES_NORMAL:
      default:
         memAlloc->shares = MEMSCHED_SHARES_NORMAL(PagesToMB(memAlloc->max));
      }
   } else {
      memAlloc->shares = alloc->shares;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedGroupAllocAllowed --
 *
 * 	Checks if it is permissible to set the external allocation 
 * 	parameters for "group" to "alloc." Caller must hold scheduler 
 * 	tree lock.
 *
 * Results: 
 * 	Returns TRUE if ok to change external allocations; FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static Bool
MemSchedGroupAllocAllowed(const Sched_Group *group, const MemSched_Alloc *alloc)
{
   MemSched_AllocInt childBase;
   Sched_Group *parentGroup;
   const MemSched_GroupState *groupState = &group->mem;

   ASSERT(Sched_TreeIsLocked());

   // Sum up base values for all immediate child groups
   MemSchedTotalChildBaseValues(group, &childBase);

   /*
    * Check if new allocations are less than what is required to support 
    * the sub-tree under the group.
    */
   if ((alloc->minLimit < childBase.emin) || (alloc->max < childBase.emax)) {
      return (FALSE);
   }

   /*
    * If more min/max is being requested over current min/max consumption
    * carry out admission control check against parent group 
    */
   parentGroup = Sched_TreeGroupParent(group);
   if (parentGroup == NULL) {
      // Nothing needs to be done for "root" group
      ASSERT(group == Sched_TreeRootGroup());
   } else {
      uint32 minReqPages, maxReqPages;
      uint32 hardMax;

      if (alloc->min > groupState->base.emin) {
         minReqPages = alloc->min - groupState->base.emin;
      } else {
         minReqPages = 0;
      }

      hardMax = (MemSchedNodeIsMemClient(group->node)) ? alloc->max
	      					       : alloc->hardMax;
      if (hardMax > groupState->base.emax) {
         maxReqPages = hardMax - groupState->base.emax;
      } else {
         maxReqPages = 0;
      }

      if ((minReqPages > 0) || (maxReqPages > 0)) {
         if (MemSchedAdmitGroupInt(parentGroup, 
   			           minReqPages, 
				   maxReqPages) != VMK_OK) {
            return (FALSE);
         }
      }
   }

   return (TRUE);
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedGroupSetAllocInt --
 *
 * 	Sets the external allocation parameters for "group" to "alloc".
 * 	Caller must hold scheduler tree lock.
 *
 * Results: 
 * 	Modifies "group" state.
 *
 * Side effects:
 *      Modifies scheduler state.
 *
 *----------------------------------------------------------------------
 */
static void
MemSchedGroupSetAllocInt(Sched_Group *group, const MemSched_Alloc *alloc)
{
   MemSched_GroupState *memGroup = &group->mem;
   uint32 shares;

   // Sanity Checks
   ASSERT(Sched_TreeIsLocked());
   ASSERT(alloc->min <= alloc->minLimit);
   ASSERT(alloc->min <= alloc->hardMax);
   ASSERT(alloc->hardMax <= alloc->max);
   ASSERT(alloc->minLimit <= alloc->max);

   // Ensure shares are within valid range
   shares = alloc->shares;
   shares = MAX(shares, MEMSCHED_SHARES_MIN);
   shares = MIN(shares, MEMSCHED_SHARES_MAX);
 
   // Update external allocation parameters
   memGroup->alloc = *alloc;
   memGroup->alloc.shares = shares;

   // Update internal memory resource related state in the scheduler tree
   MemSched_SubTreeChanged(group);

   // XXX Issue realloction request
}

/*
 *----------------------------------------------------------------------
 *
 * MemSched_GroupGetAllocLocked --
 *
 * 	Returns the external allocation parameters for "group" in "alloc". 
 * 	Caller must hold scheduler tree lock.
 *
 * Results: 
 * 	None.
 *
 * Side effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
void
MemSched_GroupGetAllocLocked(Sched_Group *group, Sched_Alloc *alloc)
{
   MemSched_GroupState *groupState;

   ASSERT(Sched_TreeIsLocked());
   ASSERT(SCHED_NODE_IS_GROUP(group->node));

   groupState = &group->mem;

   alloc->min = groupState->alloc.min;
   alloc->max = groupState->alloc.max;
   alloc->shares = groupState->alloc.shares;
   alloc->minLimit = groupState->alloc.minLimit;
   alloc->hardMax = groupState->alloc.hardMax;
   alloc->units = SCHED_UNITS_PAGES;
}

/*
 *----------------------------------------------------------------------
 *
 * MemSched_GroupSetAllocLocked --
 *
 * 	Sets the external allocation parameters for "group" to "alloc". 
 * 	Implements functionality for MemSched_GroupSetAlloc() and may 
 * 	also be directly invoked by callers residing within the scheduler 
 * 	module. Caller must hold scheduler tree lock.
 *
 * Results: 
 *      Returns VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 *      Modifies scheduler state associated with "group."
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
MemSched_GroupSetAllocLocked(Sched_Group *group, const Sched_Alloc *alloc)
{
   MemSched_Alloc memAlloc;

   ASSERT(Sched_TreeIsLocked());
   ASSERT(SCHED_NODE_IS_GROUP(group->node));

   MemSchedAllocInit(&memAlloc, alloc);

   // Check if new allocations are permissible
   if (!MemSchedGroupAllocAllowed(group, &memAlloc)) {
      return (VMK_BAD_PARAM);
   }

   // Assign new allocations to group
   MemSchedGroupSetAllocInt(group, &memAlloc);

   return (VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * MemSched_GroupSetAlloc --
 *
 * 	Sets the external allocation parameters for the group 
 * 	identified by "id" to "alloc".
 *
 * Results: 
 *      Returns VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 *      Modifies scheduler state associated with group "id".
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
MemSched_GroupSetAlloc(Sched_GroupID groupID, const Sched_Alloc *alloc)
{
   Sched_Group *group;
   VMK_ReturnStatus status;

   Sched_TreeLock();

   group = Sched_TreeLookupGroup(groupID);
   if (group == NULL) {
      status = VMK_NOT_FOUND;
   } else {
      status = MemSched_GroupSetAllocLocked(group, alloc);
   }

   Sched_TreeUnlock();

   return (status);
}

/*
 *----------------------------------------------------------------------
 *
 * MemSched_SetupVmGroup --
 *
 * 	Sets  up the VM container group's memory resource allocations.
 * 	The total allocations for the group are the sum of the existing 
 * 	allocations and the newly specified allocations.
 *
 * Results: 
 * 	VMK_OK on success; error otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
MemSched_SetupVmGroup(World_Handle *world,
		      Sched_Group *group, 
		      const Sched_Alloc *alloc)
{
   Sched_Alloc curAlloc, newAlloc;
   VMK_ReturnStatus status;
   MemSchedVmm *vmm = VmmClientFromWorld(world);

   Sched_TreeIsLocked();
   ASSERT(MemSchedNodeIsMemClient(group->node));

   // Extract current group allocations
   MemSched_GroupGetAllocLocked(group, &curAlloc);

   // Assign new allocations to the group
   status = MemSched_GroupSetAllocLocked(group, alloc);
   if (status != VMK_OK) {
      return (status);
   }

   // Save state necessary for restoring group state when VM terminates
   MemSched_GroupGetAllocLocked(group, &newAlloc);
   vmm->minVmm = newAlloc.min;
   vmm->maxVmm = newAlloc.max;
   vmm->oldShares = curAlloc.shares;

   // Increase group so that existing consumers (vmx, etc.) are accomodated
   status = MemSchedIncClientGroupSize(group, curAlloc.min, curAlloc.max);
   if (status != VMK_OK) {
      MemSched_GroupSetAllocLocked(group, &curAlloc);
   }

   return (status);
}

/*
 *----------------------------------------------------------------------
 *
 * MemSched_CleanupVmGroup --
 *
 * 	Restores memory resource allocations for the VM container
 * 	group to the state before the VM was powered on.
 *
 * Results: 
 * 	VMK_OK on success; error otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
MemSched_CleanupVmGroup(World_Handle *world, Sched_Group *group)
{
   VMK_ReturnStatus status;
   Sched_Alloc curAlloc, newAlloc;
   MemSchedVmm *vmm = VmmClientFromWorld(world);

   Sched_TreeIsLocked();
   ASSERT(MemSchedNodeIsMemClient(group->node));

   // Extract current group allocations
   MemSched_GroupGetAllocLocked(group, &curAlloc);

   // Make adjustments to bring group back to state before VM was started
   newAlloc = curAlloc;
   newAlloc.min -= vmm->minVmm;
   newAlloc.max -= vmm->maxVmm;
   newAlloc.shares = vmm->oldShares;
   newAlloc.minLimit = newAlloc.min;
   newAlloc.hardMax = newAlloc.max;

   // Assign new allocations to group
   status = MemSched_GroupSetAllocLocked(group, &newAlloc);
   ASSERT(status == VMK_OK);

   vmm->minVmm = 0;
   vmm->maxVmm = 0;
   vmm->oldShares = 0;
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedAdmitGroupInt --
 * 	Internal routine that performs the necessary memory resource 
 * 	related admission control checks when either placing a scheduler 
 * 	group under a specified parent scheduler group or modifying an
 * 	existing scheduler group's memory resource allocations. The caller 
 * 	must hold scheduler tree lock.
 *
 * Results: 
 * 	Returns VMK_OK if admission check passed, 
 * 	otherwise VMK_MEM_ADMIT_FAILED.
 *
 * Side effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
MemSchedAdmitGroupInt(const Sched_Group *parentGroup,
		      uint32 minReqPages,
		      uint32 maxReqPages)
{
   const Sched_Group *parGroup = parentGroup;		// working copy
   uint32 requiredMin, requiredMax;

   ASSERT(Sched_TreeIsLocked());
   ASSERT(SCHED_NODE_IS_GROUP(parGroup->node));

   requiredMin = minReqPages;
   requiredMax = maxReqPages;

   do {
      MemSched_AllocInt totalChildBase;
      const MemSched_GroupState *groupState = &parGroup->mem;

      // Sum up the base values for all immediate child groups
      MemSchedTotalChildBaseValues(parGroup, &totalChildBase);

      requiredMin += totalChildBase.emin;
      requiredMax += totalChildBase.emax;

      // Check against group's minLimit
      if (requiredMin > groupState->alloc.minLimit) {
         return (VMK_MEM_ADMIT_FAILED);
      }
      if (requiredMin <= groupState->base.emin) {
         requiredMin = 0;
      } else {
	 requiredMin -= groupState->base.emin;
      }

      // Check against group's max size
      if (requiredMax > groupState->alloc.max) {
         return (VMK_MEM_ADMIT_FAILED);
      }
      if (requiredMax <= groupState->base.emax) {
         requiredMax = 0;
      } else {
	 requiredMax -= groupState->base.emax;
      }

      if ((requiredMin == 0) && (requiredMax == 0)) {
         return (VMK_OK);		// admission check completed
      } 

      /*
       * There is enough allowable room to grow autoMin and/or autoMax
       * for this group. Check if parent has sufficient min and/or max
       * to allow for this autoMin and/or autoMax growth.
       */
   } while ((parGroup = Sched_TreeGroupParent(parGroup)) != NULL);

   NOT_REACHED();
}

/*
 *----------------------------------------------------------------------
 *
 * MemSched_AdmitGroup --
 * 	Performs the necessary memory resource related admission 
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
MemSched_AdmitGroup(const Sched_Group *group, const Sched_Group *newParentGroup)
{
   uint32 minReqPages, maxReqPages;

   ASSERT(Sched_TreeIsLocked());
   ASSERT(SCHED_NODE_IS_GROUP(group->node));
   ASSERT(SCHED_NODE_IS_GROUP(newParentGroup->node));

   minReqPages = group->mem.base.emin;
   maxReqPages = group->mem.base.emax;

   return (MemSchedAdmitGroupInt(newParentGroup, minReqPages, maxReqPages));
}

/*
 *----------------------------------------------------------------------
 *
 * MemSched_GroupChanged --
 *	Updates scheduler state associated with "world" to be 
 *	consistent with respect to its current group membership.
 *
 * Results: 
 * 	None.
 *
 * Side effects:
 * 	Might issue realloction request.
 *
 *----------------------------------------------------------------------
 */
void
MemSched_GroupChanged(World_Handle *world)
{
   ASSERT(world != NULL);

   // XXX Actual code goes here
}

/*
 *----------------------------------------------------------------------
 *
 * MemSched_ProcGroupsRead --
 *
 * 	Callback for read operation on /proc/vmware/sched/groups
 * 	procfs node.
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
MemSched_ProcGroupsRead(char *buf, int  *len)
{
   uint32 i, snapCount;

   // format header
   Proc_Printf(buf, len,
		"\n"
		"Memory Resource Related Info:"
		"\n"
	        "vmgid name         "
	        " pgid pname        "
		"size clients"
		"  amin   amax ashares  minlimit hardmax "
		"  bmin   bmax   emin   emax bshares "
		"\n");

   MemSchedLock();

   // Snapshot group information
   snapCount = 0;
   Sched_ForAllGroupsDo(MemSchedGroupSnapshot, &snapCount);
   ASSERT(snapCount <= SCHED_GROUPS_MAX);

   // Format output 
   for (i = 0; i < snapCount; i++) {
      MemSchedGroupSnap *s = &memSched.group[i];
      MemSchedGroupSnapFormat(s, buf, len);
   }

   MemSchedUnlock();
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedIncClientGroupSize --
 *
 * 	Increases min and max for the memsched client group by the 
 * 	specified amounts. Caller must hold scheduler tree lock.
 *
 * Results: 
 * 	VMK_OK on success; error otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
MemSchedIncClientGroupSize(Sched_Group *group, uint32 minSize, uint32 maxSize)
{
   Sched_Alloc alloc;

   ASSERT(Sched_TreeIsLocked());
   ASSERT(MemSchedNodeIsMemClient(group->node));

   MemSched_GroupGetAllocLocked(group, &alloc);

   alloc.min += minSize;
   alloc.max += maxSize;
   alloc.minLimit = alloc.min;
   alloc.hardMax = alloc.max;

   return (MemSched_GroupSetAllocLocked(group, &alloc));
}

/*
 *----------------------------------------------------------------------
 *
 * MemSchedDecClientGroupSize --
 *
 * 	Decreases min and max for the memsched client group by the
 * 	specified amounts. Caller must hold scheduler tree lock.
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
MemSchedDecClientGroupSize(Sched_Group *group, uint32 minSize, uint32 maxSize)
{
   Sched_Alloc alloc;
   VMK_ReturnStatus status;

   ASSERT(Sched_TreeIsLocked());
   ASSERT(MemSchedNodeIsMemClient(group->node));

   MemSched_GroupGetAllocLocked(group, &alloc);

   ASSERT(alloc.min > minSize);
   ASSERT(alloc.max > maxSize);

   alloc.min -= minSize;
   alloc.max -= maxSize;
   alloc.minLimit = alloc.min;
   alloc.hardMax = alloc.max;

   status = MemSched_GroupSetAllocLocked(group, &alloc);
   ASSERT(status == VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * MemSched_AddSystemSwap --
 *
 * 	Increases amount of system swap visible to the memory scheduler.
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
MemSched_AddSystemSwap(uint32 numPages)
{
   VMK_ReturnStatus status;
   Sched_Alloc alloc;
   Sched_Group *group;

   Sched_TreeLock();

   // Extract current allocations for the "root" group
   group = Sched_TreeRootGroup();
   MemSched_GroupGetAllocLocked(group, &alloc);

   ASSERT(alloc.max == alloc.hardMax);
   ASSERT(alloc.max > memSched.totalSystemSwap);

   // Increase max allocations by the amount of swap being added.
   alloc.max += numPages;
   alloc.hardMax += numPages;

   // Increase total swap seen by memsched.
   memSched.totalSystemSwap += numPages;	

   // Set new max allocations for the "root" group.
   status = MemSched_GroupSetAllocLocked(group, &alloc);
   ASSERT(status == VMK_OK);

   Sched_TreeUnlock();
}

/*
 *----------------------------------------------------------------------
 *
 * MemSched_RemoveSystemSwap --
 *
 * 	Decreases amount of system swap visible to the memory scheduler.
 *
 * Results: 
 *      VMK_OK on success; error code otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
MemSched_RemoveSystemSwap(uint32 numPages)
{
   VMK_ReturnStatus status;
   Sched_Alloc alloc;
   Sched_Group *group;

   Sched_TreeLock();

   if (numPages > memSched.totalSystemSwap) {
      Sched_TreeUnlock();
      return (VMK_BAD_PARAM);
   }

   // Extract current allocations for the "root" group.
   group = Sched_TreeRootGroup();
   MemSched_GroupGetAllocLocked(group, &alloc);

   ASSERT(alloc.max == alloc.hardMax);
   ASSERT(alloc.max > memSched.totalSystemSwap);

   // Decrease max allocations by the amount of swap being removed.
   alloc.max -= numPages;
   alloc.hardMax -= numPages;

   // Set new max allocations for the "root" group.
   status = MemSched_GroupSetAllocLocked(group, &alloc);
   if (status == VMK_OK) {
      // Decrease total swap seen by memsched.
      memSched.totalSystemSwap -= numPages;
   }

   Sched_TreeUnlock();

   return (status);
}

#ifdef VMX86_DEBUG
/*
 *----------------------------------------------------------------------
 *
 * MemSched_IncLowStateMPNAllocated --
 *
 *      Increment the number of MPNs allocated in Low state.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
MemSched_IncLowStateMPNAllocated(World_Handle *world, Bool ovhdPage)
{
   MemSchedVmm *vmm = VmmClientFromWorld(world);
   vmm->lowStateMPNAllocated++;
   if (ovhdPage) {
      vmm->lowStateOvhdMPNAllocated++;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * MemSched_IncLowStateMPNReleased --
 *
 *      Increment the number of MPNs released in Low state.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
MemSched_IncLowStateMPNReleased(World_Handle *world,
                                uint32 numPages)
{
   MemSchedVmm *vmm = VmmClientFromWorld(world);
   vmm->lowStateMPNReleased += numPages;
}

/*
 *----------------------------------------------------------------------
 *
 * MemSched_LogLowStateMPNUsage --
 *
 *      Increment the number of MPNs released in Low state.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
MemSched_LogLowStateMPNUsage(void)
{
   MemSched *m = &memSched;

   // Reset the num of MPNs alloced and released in low state.
   FORALL_MEMSCHED_VMM_CLIENTS(&m->schedQueue, c, vmm) {
      Log("world<%d>, mpn used = %u, released = %u, ovhdMPN used = %u", 
          vmm->world->worldID, vmm->lowStateMPNAllocated, vmm->lowStateMPNReleased,
          vmm->lowStateOvhdMPNAllocated);
      Log("world<%d>, lowAlloc = %u, lowLocked = %u, curAlloc = %u, curLocked = %u",
          vmm->world->worldID, vmm->lowStateAlloc, vmm->lowStateLocked, 
          c->commit.alloc, c->snapshot.locked);
      Log("world<%d>, swapTarget = %u, swapped = %u, numLowFree = %u, lowFreeAmt = %u\n",
          vmm->world->worldID, vmm->lowStateSwapTarget, vmm->lowStateSwapped, 
          vmm->lowStateFree, vmm->lowStateFreeAmt);
   } MEMSCHED_VMM_CLIENTS_DONE;
}

#endif
