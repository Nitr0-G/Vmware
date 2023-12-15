/* **********************************************************
 * Copyright 2000 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * memsched.h --
 *
 *	World memory scheduler.
 */

#ifndef _MEMSCHED_H
#define _MEMSCHED_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "memsched_ext.h"
#include "proc.h"

/*
 * constants
 */

// memory shares
#define	MEMSCHED_SHARES_MIN		(0)
#define	MEMSCHED_SHARES_MAX		(100000)
#define	MEMSCHED_SHARES_PER_MB_LOW	(5)
#define	MEMSCHED_SHARES_PER_MB_NORMAL	(10)
#define	MEMSCHED_SHARES_PER_MB_HIGH	(20)
#define	MEMSCHED_SHARES_LOW(mb)		(MEMSCHED_SHARES_PER_MB_LOW * (mb))
#define	MEMSCHED_SHARES_NORMAL(mb)	(MEMSCHED_SHARES_PER_MB_NORMAL * (mb))
#define	MEMSCHED_SHARES_HIGH(mb)	(MEMSCHED_SHARES_PER_MB_HIGH * (mb))

// page migration rate
#define	MEMSCHED_MIGRATE_RATE_MAX	(200)

// useful constants
#define	PAGES_PER_MB			(1 << (20 - PAGE_SHIFT))
#define	KB_PER_PAGE			(PAGE_SIZE >> 10)

// node affinity
#define	MEMSCHED_NODE_AFFINITY_NONE		(0xffffffff)
#define	MEMSCHED_NODE_AFFINITY(node)		(1 << (node))

// color affinity
#define MEMSCHED_COLORS_ALL                     (NULL)
#define MEMSCHED_MAX_SUPPORTED_COLORS           (256)

/*
 * types
 */

struct World_Handle;
struct World_InitArgs;

typedef struct {
   int nColors;
   uint8 colors[MEMSCHED_MAX_SUPPORTED_COLORS];
} MemSched_ColorVec;

typedef struct {
   uint32 locked;	// snapshot: locked pages
   uint32 overhead;	// snapshot: overhead pages
   uint32 cow;		// snapshot: copy-on-write pages
   uint32 zero;		// snapshot: zero pages
   uint32 shared;	// snapshot: locked, but not consumed (approx)
   uint32 swapped;      // snapshot: swapped pages
   uint32 balloon;      // snapshot: balloon size
   uint32 touched;	// snapshot: estimated working set size (in pages)
} MemSched_Snapshot;

typedef struct {
   uint32 target;	// target    size (in pages)
   uint32 alloc;	// allocated size (in pages)
   uint32 minTarget;	// minimum allowed target size (in pages)

   uint32 invShares;	// inversely proportional to shares
   uint32 charged;	// effective pages (touched + idleCost*idle)
   uint64 pps;		// effective pages per share
} MemSched_Eval;

typedef struct {
   uint32 min;
   uint32 max;
   uint32 shares;
   uint32 minLimit;
   uint32 hardMax;
   Bool autoMin;

   // XXX fields used by current memsched algorithm,
   //     possibly to be changed with the new implementation
   uint32 adjustedMin;  // adjusted min to account for un-responsive VMs
} MemSched_Alloc; 

typedef struct {
   uint32 min;
   uint32 max;
   uint32 shares;
   uint32 emin;
   uint32 emax;
} MemSched_AllocInt;

typedef enum {
   MEMSCHED_MEMTYPE_MAPPED,     // total mapped memory (limited by max)
   MEMSCHED_MEMTYPE_KERNEL,     // kernel memory (currently unlimited)
   MEMSCHED_MEMTYPE_SHARED,     // pshared memory (currently unlimited)
   MEMSCHED_MEMTYPE_UNCOUNTED,  // special uncounted memory (unlimited)
   MEMSCHED_NUM_MEMTYPES
} MemSchedType;

typedef struct {
   uint32 pageable;     // pageable memory
   uint32 cow;          // cowed usermem memory 
   uint32 swapped;      // swapped memory
   uint32 pinned;       // currently pinned memory 
   uint32 virtualPageCount[MEMSCHED_NUM_MEMTYPES];  // virtual memory count
} MemSchedUserUsage;

typedef struct MemSchedUser {
   Bool   valid;

   // memory control
   uint32 reserved;     // total memory for locked mmap regions
   uint32 mapped;       // total memory for unlocked mmap regions

   // memory usage
   MemSchedUserUsage usage;

   uint32 swapTarget;
} MemSchedUser;

typedef struct MemSchedVmmUsage {
   uint32 locked; 
   uint32 cow;
   uint32 zero;
   uint32 cowHint;
   uint32 swapped;
   uint32 overhead;     // VMX overhead (COS VMX only)
   uint32 anon;
   uint32 anonKern;
} MemSchedVmmUsage;

typedef struct MemSchedVmm {
   Bool           valid;
   // identity
   struct World_Handle *world;	// vmm world group lead

   // monitor started?
   Bool		  vmmStarted;	// monitor ready to process actions?
   uint32	  startAction;	// vmm start action index

   // memsched client group configuration state
   int32	  minVmm;	// min increase when starting VM
   int32	  maxVmm;	// max increase when starting VM
   int32	  oldShares;	// original shares when starting VM

   // memsched alloc before vmm is started
   MemSched_Alloc preAlloc;

   // memory usage
   MemSchedVmmUsage     usage;  // memory usage for the vmm.

   // swap state
   SP_SpinLockIRQ swapLock;	    // for protecting swap fields
   uint32         swapTarget;	    // target swap size (in pages)
   uint64         swapReqTimeStamp; // time stamp of the last swap request
   Bool           vmResponsive;     // is the VM responding to swap requests?

   // balloon state
   uint32	  balloonAction;  // balloon action index
   uint32	  balloonTarget;  // target  balloon size (in pages)
   int32	  balloonMaxCfg;  // configured max balloon size (in pages)
   uint32	  balloonMax;	  // maximum balloon size (in pages)

   // usage sampling state
   uint32	  sampleAction;	  // sampling action index
   uint32	  samplePeriod;	  // sampling period (in virtual seconds)
   uint32	  sampleSize;	  // sampling size   (in pages)
   uint32	  sampleHistory;  // sampling history (in periods)

   // page sharing state
   uint32         pshareAction;	// pshare action index
   Bool           pshareEnable; // is page sharing enabled?

   // page remapping state
   uint32	  remapConfigAction;

   // host wait state
   uint64	  hostWaitSkip;	// timestamp after which can skip wait
   // action for initiating vmm numa migration.
   uint32         numaMigrateAction;

#ifdef VMX86_DEBUG
   // low state mem allocation
   uint32         lowStateMPNAllocated; // # MPNs allocated while in low state
   uint32         lowStateOvhdMPNAllocated; // # MPNs allocated while in low state
   uint32         lowStateMPNReleased;  // # MPNs released while in low state
   uint32         lowStateOvhdMPNReleased;  // # MPNs released while in low state
   uint32         lowStateSwapped;
   uint32         lowStateSwapTarget;
   uint32         lowStateAlloc;
   uint32         lowStateLocked;
   uint32         lowStateFree;
   uint32         lowStateFreeAmt;
#endif

   // shared area data 
   MemSched_Info  *memschedInfo;
} MemSchedVmm;

typedef struct MemSched_Client {
   List_Links     link;         // link in the list

   MemSchedVmm    vmm;          // vmm world specific mem scheduling info
   MemSchedUser   user;         // user world specific mem scheduling info

   // allocation state
   uint32 overhead;	        // maximum overhead size (in pages)
   MemSched_Alloc alloc;	// specified allocation
   MemSched_Snapshot snapshot;   // snapshot of memory usage
   MemSched_Eval   commit;	// committed eval state
   MemSched_Eval   update;	// transient eval state

   uint32 nodeAffinityMask;	// memory node bitmask
   Bool   hardAffinity; // was affinity set by user?
   MemSched_ColorVec* colorsAllowed;    // which cache colors can client use

   // procfs nodes
   Proc_Entry	  procMemDir;	// /proc/vmware/vm/<ID>/mem/
   Proc_Entry	  procStatus;	// /proc/vmware/vm/<ID>/mem/status
   Proc_Entry	  procSwap;	// /proc/vmware/vm/<ID>/mem/swap
   Proc_Entry	  procMin;	// /proc/vmware/vm/<ID>/mem/min
   Proc_Entry	  procShares;	// /proc/vmware/vm/<ID>/mem/shares
   Proc_Entry	  procAffinity; // /proc/vmware/vm/<ID>/mem/affinity
   Proc_Entry	  procPShare;	// /proc/vmware/vm/<ID>/mem/pshare
   Proc_Entry	  procRemap;	// /proc/vmware/vm/<ID>/mem/remap
   Proc_Entry	  procMigRate;	// /proc/vmware/vm/<ID>/mem/mig-rate
   Proc_Entry	  procDebug;	// /proc/vmware/vm/<ID>/mem/debug
} MemSched_Client;

typedef struct {
   uint32 overcommit;	// % managed memory overcommitted
   uint32 free;		// % managed memory free
   uint32 reclaim;	// % VM memory reclaimed
   uint32 balloon;	// % VM memory ballooned
   uint32 swap;		// % VM memory swapped
} MemSched_LoadMetrics;

typedef struct {
   MemSched_Alloc alloc;
   MemSched_AllocInt base;
   // Additional fields to be added soon
} MemSched_GroupState;

/*
 * inline operations
 */

static INLINE int32
PagesToKB(int32 n)
{
   return(n * KB_PER_PAGE);
}

static INLINE int32
PagesToMB(int32 n)
{
   return(n / PAGES_PER_MB);
}

/*
 * operations
 */

// initialization
void MemSched_EarlyInit(void);
void MemSched_Init(Proc_Entry *procSchedDir);
void MemSched_SchedWorldInit(void);


// affinity
uint32 MemSched_NodeAffinityMask(struct World_Handle *world);
Bool MemSched_HasNodeHardAffinity(struct World_Handle *world);
void MemSched_SetNodeAffinity(struct World_Handle *world, uint32 affinMask,
                          Bool forced);
MemSched_ColorVec* MemSched_AllowedColors(struct World_Handle *world);

// page migration
VMK_ReturnStatus MemSched_SetMigRate(struct World_Handle *world, uint32 rate);
uint32 MemSched_GetMigRate(const struct World_Handle *world);

VMK_ReturnStatus MemSched_NumaMigrateVMM(struct World_Handle *world);


// world join and leave operations
void MemSched_WorldGroupInit(struct World_Handle *world, 
                             struct World_InitArgs *args);
void MemSched_WorldGroupCleanup(struct World_Handle *world);
VMK_ReturnStatus MemSched_WorldInit(struct World_Handle *world, 
                                    struct World_InitArgs *args);
void MemSched_WorldCleanup(struct World_Handle *world);
Bool MemSched_IsManaged(const struct World_Handle *world);

// usage access 
MemSchedVmmUsage * MemSched_ClientVmmUsage(const struct World_Handle *world);
MemSchedUserUsage * MemSched_ClientUserUsage(const struct World_Handle *world);

// memory status operations
int32 MemSched_FreePages(void);
void  MemSched_Reallocate(void);
Bool  MemSched_MemoryIsLow(void);
Bool  MemSched_MemoryIsHigh(void);
void  MemSched_CheckReserved(int32 *availPages,
                                    int32 *reservedPages,
                                    int32 *autoMinPages,
                                    int32 *availSwap,
                                    int32 *reservedSwap);
uint32 MemSched_TotalVMPagesUsed(void);
void MemSched_UpdateFreePages(uint32 nPages);
uint32 MemSched_TotalSwapReserved(void);

// memsched config 
VMK_ReturnStatus MemSched_Reconfig(Bool write, Bool valueChanged, int indx);
VMK_ReturnStatus MemSched_ReconfigPShare(Bool write, Bool valueChanged, int indx);
VMK_ReturnStatus MemSched_ReconfigSample(Bool write, Bool valueChanged, int indx);
VMK_ReturnStatus MemSched_UpdateMinFree(Bool write, Bool valueChanged, int ndx);

// load statistics
void MemSched_GetLoadMetrics(MemSched_LoadMetrics *load);

// memory reservation and admission
VMK_ReturnStatus MemSched_ReserveMem(struct World_Handle *world,
                                     uint32 pageDelta);
void MemSched_UnreserveMem(struct World_Handle *world, uint32 pageDelta);
VMK_ReturnStatus MemSched_SetUserOverhead(struct World_Handle* world, uint32 numOverhead);
Bool MemSched_AdmitUserOverhead(const struct World_Handle *world, uint32 incPages);
Bool MemSched_AdmitUserMapped(const struct World_Handle *world, uint32 incPages);

// memory wait operations
Bool MemSched_HostShouldWait(struct World_Handle *world);
Bool MemSched_UserWorldShouldBlock(const struct World_Handle *world);
VMK_ReturnStatus MemSched_UserWorldBlock(void);
void MemSched_BlockWhileMemLow(struct World_Handle *world);
VMK_ReturnStatus MemSched_MemoryIsLowWait(uint32 msTimeout);
Bool MemSched_ShouldSwapBlock(uint32 swapTarget, uint32 swapped);

// checkpointing operations
Bool MemSched_TerminateCptOnLowMem(struct World_Handle *world);
void MemSched_SetMaxCptInvalidPages(uint32 numPages);

// VMK entry points
VMKERNEL_ENTRY MemSched_MonitorStarted(DECLARE_0_ARGS(VMK_MEM_VMM_STARTED));
void MemSched_SetSwapReqTimeStamp(struct World_Handle *world, uint64 mSec);
VMK_ReturnStatus MemSched_SetMemMin(struct World_Handle *world, 
                                    uint32 allocMinPages, Bool autoMin);

// Scheduler group(s) operations
void MemSched_GroupStateInit(MemSched_GroupState *s);
void MemSched_GroupStateCleanup(MemSched_GroupState *s);
VMK_ReturnStatus MemSched_GroupSetAlloc(Sched_GroupID groupID, 
					const Sched_Alloc *alloc);
void MemSched_GroupChanged(struct World_Handle *world);

void MemSched_AddSystemSwap(uint32 numPages);
VMK_ReturnStatus MemSched_RemoveSystemSwap(uint32 numPages);

// callbacks for sched/groups procfs node
void MemSched_ProcGroupsRead(char *buf, int *len);

// debugging
void MemSched_LogSwapStats(void);
#ifdef VMX86_DEBUG
void MemSched_IncLowStateMPNAllocated(struct World_Handle *world, Bool ovhdPage);
void MemSched_IncLowStateMPNReleased(struct World_Handle *world, uint32 numPages);
void MemSched_LogLowStateMPNUsage(void);
#endif

#endif
