/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * memmap.c --
 *
 *	This is the machine memory manager.  Manages all machine memory
 *      not previously reserved via Mem_AllocEarly() or the VMkernel
 *      internal memory allocator.
 *
 *      On NUMA machines, the memory allocated is the intersection of the
 *      VMNIX memory map and the ACPI SRAT table memory map.  These two
 *      memory maps should be very close, with the VMNIX map excluding
 *      memory for the COS and reserved areas.
 *
 *  Notes on Locking:
 *    There are two important locks held here;  the MemMapInfo lock
 *    protects the free page counters and other system-wide state such as
 *    freeLowNodes/freeHighNodes masks.  The locks in each freeList
 *    protects the page counts in each color. Currently when a page is
 *    freed or allocated, the freeList structures are updated first,
 *    followed by the summary counters. Because two different locks are
 *    held at different times, it's possible for discrepancies between
 *    the freeList page counts and the system-wide summary counters to
 *    develop. However this discrepancy doesn't hurt because the
 *    Policy function just loops and tries another color/node if it
 *    cannot allocate a page due to any reason. 
 *
 *
 *  Notes on IO Protection:
 *    In debug builds, one bit is allocated for each machine page.
 *    That bit controls whether IO operations are permitted to the
 *    corresponding machine page.  Device drivers, for example,
 *    check this "ioable" bit before initiating IO to a page.
 *    Looking ahead, this bit would be a good candidate for getting
 *    folded into the MPage structure.
 *
 *    What is the policy for deciding which parts of memory are
 *    ioable?
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "x86.h"
#include "vmkernel.h"
#include "kvmap.h"
#include "kseg.h"
#include "memmap.h"
#include "memalloc.h"
#include "sched.h"
#include "world.h"
#include "pshare.h"
#include "timer.h"
#include "host.h"
#include "numa.h"
#include "util.h"
#include "proc.h"
#include "parse.h"
#include "mtrr.h"
#include "xmap.h"
#include "vmkevent.h"
#include "buddy.h"
#include "vmmem.h"
#include "mpage.h"
#include "vmnix_syscall.h"
#include "alloc_inline.h"

#define LOGLEVEL_MODULE MemMap
#define LOGLEVEL_MODULE_LEN 6
#include "log.h"

/*
 * Compilation flags
 */

// debugging
#if	defined(VMX86_DEBUG) && defined(VMX86_DEVEL)
#define	MEMMAP_DEBUG_VERBOSE	(0)
#define	MEMMAP_DEBUG		(1)
#else
#define	MEMMAP_DEBUG_VERBOSE	(0)
#define	MEMMAP_DEBUG		(0)
#endif

#ifdef VMX86_DEBUG
#define DEBUG_ONLY(x) x
#else
#define DEBUG_ONLY(x)
#endif

#if MEMMAP_DEBUG
#define MEMMAP_DEBUG_ONLY(x) x
#else
#define MEMMAP_DEBUG_ONLY(x) 
#endif

#define KB (1024)
#define MB (KB * 1024)
#define GB (MB * 1024)
#define MEMMAP_MIN_BUF_SIZE (BYTES_2_PAGES(4 * KB))    // 1 Page i.e 4k
#define MEMMAP_MAX_BUF_SIZE (BYTES_2_PAGES(64 * MB))   // 16K pages i.e. 64M 
#define MEMMAP_MAX_LOW_LEN  (BYTES_2_PAGES(4LL * GB))  // 1M pages i.e. 4GB
#define MEMMAP_MAX_HIGH_LEN (BYTES_2_PAGES(64LL * GB)) // 16M pages i.e. 64GB

/*
 * MEMMAP_MIN_HOTADD_LEN is 
 * chosen randomly but choose wisely because it affects 
 * the memory usage by the buddy allocator, the buddy allocator 
 * divides a given range into blocks and it derives the size of each
 * block from this hotadd length, having too small a value will result
 * in large number of blocks and the buddy requires 8bytes of storage
 * per block.  On the other hand setting this value too big is also not
 * recommended because the buddy allocator will allocate storage for all
 * min sized buffers that fit in a block and on an average it requires
 * 5bytes per min sized buffer
 */
#define MEMMAP_MIN_HOTADD_LEN (1 << 14) // 16K pages i.e. 64M,
/*
 * Constants
 */

// special value
#define	HOST_LINUX_EVIL_MPN	(0x40000)

// policy controls

/* controls how much low memory is reserved for I/O to devices that can't
 * handle memory above 4GB */
#define RESERVE_LOWMEM_PCT  1

/* Threshold at which we start allocating memory above 4GB. See comment
 * before MemMapPolicyLowHigh. */
#define MEMMAP_ALLOC_HIGH_THRESHOLD \
  (CONFIG_OPTION(MEM_ALLOC_HIGH_THRESHOLD) * PAGES_PER_MB)

/*
 * structured logging macros
 */
#define	MemMapWarnNoMemory() SysAlert("out of memory");

/*
 * Alloc flag values
 */
#define MM_ADVISORY_NONE   (0x0)
#define MM_ADVISORY_NICE   (0x1)


/*
 * Types
 */

/* Statistics for the initial memory map received from the BIOS through the COS
 *
 * Out of the 'totalNumPages' received through the bios map
 * Some are
 * 1. Discarded, because of MTRR mismatch, cos evil page, bad page etc.
 * 2. Used by kernel without the actual knowledge of the memmap module
 *    because these allocations happen even before the memmap is fully
 *    initialized, this includes critical mem, early inits and pages used by the
 *    vmkloader for setting up the vmkernel
 *    For HotAdd too the critical mem gets deducted from the bios range before
 *    it is managed by the memmap module.
 * 3. Number of pages actually managed by the memmap.
 *    
 */
typedef struct {
   uint32 totalNumPages;
   uint32 numDiscarded;
   uint32 numKernelUse;
   uint32 numManagedByMemMap;
} BiosMemMapStats;

static BiosMemMapStats biosMemMapStats;

typedef enum {
   POLICY_OK,                      // Policy found a page matching constraints
   POLICY_COLOR_CONFLICT,          // Requested color not avail
   POLICY_NODE_MASK_CONFLICT,      // Requested node mask doesn't match affinity
   POLICY_TYPE_CONFLICT,           // Node mask/affinity and requested type don't agree
   POLICY_NO_PAGES                 // No conflict but no free pages found
} PolicyReturnCode;


/*
 * The freelist of pages is partitioned by NUMA nodes.  Each node may have
 * low or high memory or both.  On UMA systems, or NUMA systems with 
 * compatibility mode turned on, there is only one node partition.
 */
typedef struct {
   int nodeID;	                   // for debugging
   uint32 totalNodePages;          // total # pages in node
   uint32 reservedLowPages;        // MM_TYPE_LOWRESERVED only
   uint32 totalLowPages;	   // needed by HotAdd reapportionment

   uint32 numFreePages;            // # free pages in this node
   uint32 numFreeLowPages;         // # free pages <4GB in this node
   uint32 numKernelPages;

   Buddy_Handle buddyLow;          // <4GB memspace handle
   Buddy_Handle buddyHigh;         // >4GB memspace handle
} MemMapNode;


typedef struct MemMapInfo {
   MPN start;		// Number of first machine page for vmkernel
   MM_Color numColors;	// Cache size divided by page size
   int logNumColors;
   int numNodes;        // 1 if UMA system or compat. mode
   int numLowNodes;
   int numHighNodes;

   DEBUG_ONLY(Bool memMapInitCalled);

   MPN bootTimeMinMPN;          // min MPN available at boot time
   MPN bootTimeMaxMPN;          // max MPN available at boot time

   uint32 totalMemPages;	// total number of memory pages
   uint32 totalLowPages;        // total number of low memory page (<4GB)
   uint32 initFreePages;	// initial number of free pages
   uint32 reservedLowPages;     /* #pages in lowmem(<4GB) that are allocated
                                 * only with MMALLOC_LOWPAGE_RESERVED */
   uint32 numFreePages;	        // number of free pages
   uint32 numFreeLowPages;	// number of free pages < 4GB
   uint32 numKernelPages;       // number of allocated kernel pages
   MemMapNode node[NUMA_MAX_NODES];   // state of each NUMA node's free mem

   MM_NodeMask validNodes;      // node mask of all nodes in system
   MM_NodeMask freeLowNodes;    // node mask of avail. low memory 
   MM_NodeMask freeHighNodes;   // node mask of avail. high memory
   MM_NodeMask freeResNodes;    // node mask of avail. reserved low memory
   NUMA_Node   nextNode;        // round robin allocator next node
   MM_Color    nextKernelColor; // next color for kernel pages
   uint64      totalTypeRetries;   // track # retries for MM_TYPE_ANY
   uint64      totalAffRetries;    // track # retries w/ node affinity off
   uint64      totalGoodAllocs; // track total # of successful allocations
   uint64      totalBadAllocs;  // track total # of failed page allocs
   uint64      totalColorNodeLookups; // total # lookups in Policy fn

   SP_SpinLock hotMemAddLock;
   SP_SpinLockIRQ lock;         // protects stats in this structure
} MemMapInfo;


typedef struct {
   // inputs to the policy function
   World_Handle *world;         // NULL if vmkernel
   PPN           ppn;           // INVALID_PPN if vmkernel
   uint32        numMPNs;       // number of MPNs requested
   MM_NodeMask   nodeMask;      // bitmask of nodes to choose from
   MM_Color      color;         // specific color 0-n or MM_COLOR_ANY
   MM_AllocType  type;          // page type or MM_TYPE_ANY
   Bool          useAffinity;   // use VM's node affinity mask
} PolicyInput;

typedef struct {
   // outputs from the policy function
   int           node;       // specific node to allocate from
   MM_Color      color;
   MM_AllocType  type;
   MPN           mpn;        // the MPN of the page allocated
   uint32        lastNumFreePages;  // # free pages at time of alloc.
   uint32        colorNodeLookups;  // # of color/node lookups done
} PolicyOutput;


// This value may be inflated a little, but we may have to deal
// with NUMA machines which may have interleaved memory. Plus we also
// have to deal with additional ranges which may result due to wierd 
// intersections between the E820 maps and the SRAT tables. Hence leaving
// this value to be slightly on the higher side. 
#define MEMMAP_MAX_NODE_AVAIL_RANGES   (128)

typedef struct {
   uint32 numPages;
   uint32 numRanges;
   NUMA_MemRange nodeRange[MEMMAP_MAX_NODE_AVAIL_RANGES];
} MemMapNodeAvailRange;

// This is a temporary storage of node available ranges. This variable is used
// to store the ranges during boot and is reused on hotadd of memory.
static MemMapNodeAvailRange nodeAvailRange[NUMA_MAX_NODES];

/*
 * Allocation state of pages managed by vmkernel.
 */
static MemMapInfo memMap;

/* the last valid MPN */
static MPN lastValidMPN = 0;

static NUMA_MemRange availMemRange[MAX_AVAIL_MEM_RANGES];

/*
 * We want to switch to kseg to map freelist pages as soon as possible, but
 * have to wait until kseg is initialized.  So, we use this variable, which
 * is set to TRUE after kseg initialization is complete.
 */
static Bool memmapUseKseg = FALSE;

/*
 * Boolean used to ensure that EarlyAllocContiguous is not called after a
 * normal individual page allocation.
 */
#if MEMMAP_DEBUG
static Bool memmapPageAllocated = FALSE;
#endif

/*
 * Functions that need a certain number of continuous MPNs for the proper
 * functioning of the vmkernel are categorized as 'critical' functions. 
 * These functions typically need MPNs that are propotional to the
 * total number of MPNs managed by the system and hence their memory requirement
 * changes when memory is 'hot added'. With the buddy allocator being the
 * backend for the memmap it is conceivable that we have 'max buffers' be 
 * big enough to satisfy these modules. The problem with this approach is that 
 * the buddy system cannot *guarantee* any buffer sizes. This could be because
 * of fragmentation or some other reasons. So the memmap module must take special steps 
 * for assuring that these functions get their memory. We do this by asking the 
 * modules for the number of MPNs they require and reserving these MPNs upfront
 * i.e. before handing them over to the buddy allocator.
 */
typedef uint32 (MemMapGetNumContMPNs)(MPN minMPN, MPN maxMPN, Bool hotAdd);
typedef VMK_ReturnStatus (MemMapAssignContMPNs)(MPN minMPN, 
                                                MPN maxMPN, 
                                                Bool hotAdd,
                                                uint32 size,
                                                MPN firstMPN);

#ifdef VMX86_DEBUG
static uint32
MemMapIOProtGetNumMPNs(MPN minMPN, MPN maxMPN, Bool hotAdd);
static VMK_ReturnStatus MemMapIOProtAssignMPNs(MPN minMPN, MPN maxMPN,
                       Bool hotAdd, uint32 reqSize, MPN startMPN);
#endif

/* format of the following is (name, func1, func2)
 * where func1 is of type MemMapGetNumContMPNs and
 *       func2 is of type MemMapAssignContMPNs
 */
#define MEMMAP_CRITICAL_MEM(_macro) \
        _macro(pshare, PShare_GetNumContMPNs, PShare_AssignContMPNs) \
        _macro(mpage, MPage_GetNumContMPNs, MPage_AssignContMPNs) \
        DEBUG_ONLY(_macro(ioprot, MemMapIOProtGetNumMPNs, MemMapIOProtAssignMPNs)) \

#define MEMMAP_CRITICAL_ENUM(name, f1, f2) _##name,
typedef enum {
   MEMMAP_CRITICAL_MEM_INVALID = -1,
   MEMMAP_CRITICAL_MEM(MEMMAP_CRITICAL_ENUM)
   MEMMAP_CRITICAL_MEM_MAX,
} MemMapCriticalMemEnum;

typedef struct {
   MemMapGetNumContMPNs *getNumMPNs;
   MemMapAssignContMPNs *assignMPNs;
} MemMapCriticalMemFuncs;

#define MEMMAP_CRITICAL_FUNC(name, f1, f2)  {f1, f2},
static MemMapCriticalMemFuncs criticalMemFuncs[MEMMAP_CRITICAL_MEM_MAX] =
               {MEMMAP_CRITICAL_MEM(MEMMAP_CRITICAL_FUNC)};

static Proc_Entry memProcEntry;
static Proc_Entry memDebugProcEntry;

static int MemProcRead(Proc_Entry *entry, char *buffer, int *length);
static int MemDebugProcRead(Proc_Entry *entry, char *buffer, int *length);
static int MemDebugProcWrite(Proc_Entry *entry, char *buffer, int *length);
static VMK_ReturnStatus MemMapAddRange(MemMapInfo *mm, const NUMA_Node node,
                                       const MPN startMPN, 
                                       const uint32 numMPNs,
                                       uint32 *loBuddyOvhd, uint32 *hiBuddyOvhd);
static VMK_ReturnStatus MemMapAllocCriticalMem(MemMapInfo *mm, uint32 numNodes, 
                                         MemMapNodeAvailRange *nodeAvailRange,
                                         uint32 minMPN, uint32 maxMPN, 
                                         Bool hotAdd);
static VMK_ReturnStatus MemMapGetCriticalMPNs(MemMapInfo *mm, uint32 numNodes, 
                                              uint32 numReq, MPN *startMPN, 
                                              NUMA_Node *node, Bool align2M);
static VMK_ReturnStatus MemMapCreateNodeRange(MemMapInfo *mm, 
                                        NUMA_MemRange *availRange, 
                                        MemMapNodeAvailRange *nodeRange,
                                        Bool memCheckEveryWord, MPN *lastMPN);
static VMK_ReturnStatus MemMapAddNodeRangesToBuddy(MemMapInfo *mm,
                                          MemMapNodeAvailRange *nodeAvailRange,
                                          Bool hotAdd);

/*
 * IOProtection table management
 */
#ifdef VMX86_DEBUG
typedef struct IOProtMapArray {
   MPN memRangeMinMPN;
   MPN memRangeMaxMPN;
   MPN metadataMinMPN;
   MPN metadataMaxMPN;
} IOProtMapArray;
static uint32 allocatedIOProtSegments = 0;
static IOProtMapArray IOProtMap[MAX_AVAIL_MEM_RANGES];
#endif

/*
 * Internal Functions 
 */
static MPN MemMapAllocPages(World_Handle *world, PPN ppn, uint32 numMPNs, 
			    MM_NodeMask nodeMask, MM_Color color, 
                            MM_AllocType type, uint32 flags);
static MPN MemMapAllocPageWait(World_Handle *world, PPN ppn, 
			       MM_NodeMask nodeMask, MM_Color color, MM_AllocType type,
			       uint32 msTimeout);
static PolicyReturnCode MemMapPolicyDefault(const PolicyInput *s, PolicyOutput *o);

static uint32 MemMapFreePages(MPN mpn, Bool isKernel);

/*
 * Internal Functions - Debugging and Misc.
 */
static uint32 MemMapNumFreePages(Buddy_Handle handle, MM_Color color);
static uint32 MemMapGetCacheSizeP6(int *assoc);
static uint32 MemMapGetCacheSizeAMD(int *assoc);

#ifdef VMX86_DEBUG
static void MemMapLogFreePages(void);
static void MemMapDumpState(const PolicyInput *s, int level);
static const char * MMTypeString(MM_AllocType type);
#endif

/*
 * Utility operations
 */

/*
 *----------------------------------------------------------------------
 *
 * MemMapResetRange --
 *
 *    Utility function to reset the nodeAvailRange global.
 *
 * Results:
 *    none
 *
 * Side effects:
 *      None.
 *
 *---------------------------------------------------------------------- 
 */
static INLINE void
MemMapResetRange(NUMA_MemRange *range)
{
   range->startMPN = range->endMPN = INVALID_MPN;
}

/*
 *----------------------------------------------------------------------
 *
 * MemMapResetNodeAvailRange --
 *
 *    Utility function to reset the nodeAvailRange global.
 *
 * Results:
 *    none
 *
 * Side effects:
 *      None.
 *
 *---------------------------------------------------------------------- 
 */
static INLINE void
MemMapResetNodeAvailRange(MemMapNodeAvailRange *nodeAvailRange)
{
   uint32 i;
   for (i = 0; i < NUMA_MAX_NODES; i++) {
      uint32 j;
      nodeAvailRange[i].numPages = 0;
      nodeAvailRange[i].numRanges = 0;
      for (j = 0; j < MEMMAP_MAX_NODE_AVAIL_RANGES; j++) {
         MemMapResetRange(&nodeAvailRange[i].nodeRange[j]);
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * MemMapAddRangeToNode --
 *    
 *    Utility function to add the given range of mpns to this nodes 
 *    available range.
 *
 * Results:
 *    VMK_OK on success, VMK_FAILURE otherwise.
 *
 * Side effects:
 *    None.
 *
 *---------------------------------------------------------------------- 
 */
static INLINE VMK_ReturnStatus
MemMapAddRangeToNode(MemMapNodeAvailRange *availRange, 
                     MPN startMPN,
                     MPN endMPN)
{
   const uint32 numRanges = availRange->numRanges;
   ASSERT(numRanges < MEMMAP_MAX_NODE_AVAIL_RANGES);
   if (numRanges >= MEMMAP_MAX_NODE_AVAIL_RANGES) {
      SysAlert("insufficient number of ranges, failure to "
               "allocate contiguous memory");
      return VMK_FAILURE;
   }
   if (startMPN > endMPN) {
      return VMK_OK;
   }
   availRange->nodeRange[numRanges].startMPN = startMPN;
   availRange->nodeRange[numRanges].endMPN = endMPN;
   availRange->numRanges++;
   availRange->numPages += endMPN - startMPN + 1;
   return VMK_OK;
}




/*
 *-----------------------------------------------------------------------------
 *
 * MemMap_MPN2Color --
 *
 *      Returns the cache color of the page at "mpn"
 *
 * Results:
 *      Returns the cache color of the page at "mpn"
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
MM_Color
MemMap_MPN2Color(const MPN mpn)
{
   return mpn & (memMap.numColors-1);
}

/*
 * When looking at MemMap freelist or IOProtect table, need to map the
 * pages containing freelist nodes.  Normally we want to use Kseg for these
 * mappings, but early on in the boot process kseg hasn't been setup, so we
 * need use kvmap during that period.
 */

static INLINE void*
MemMapMapPage(MPN mpn, KSEG_Pair **pair)
{
   if (memmapUseKseg) {
      return Kseg_MapMPN(mpn, pair);
   } else {
      return KVMap_MapMPN(mpn, TLB_LOCALONLY);
   }
}

static INLINE void
MemMapUnmapPage(void *ptr, KSEG_Pair *pair)
{
   if (memmapUseKseg) {
      Kseg_ReleasePtr(pair);
   } else {
      KVMap_FreePages(ptr);
   }
}

static INLINE Bool
MemMapIsSystemNUMA(void)
{
   return (memMap.numNodes > 1);
}

/*
 * Returns the average # of (color, node) combinations the
 * PolicyDefault function has to go through. A measure of the latency
 * of the Policy function.
 * NB: It returns the average PER 100 Calls.  This allows for
 * fixed-point display of the average with 2 digit precision.
 */
static INLINE uint64
MemMapAvgLookups(const MemMapInfo *mm)
{
   if (mm->totalGoodAllocs) {
      return ((100 * mm->totalColorNodeLookups) /
              (mm->totalGoodAllocs + mm->totalBadAllocs
               + mm->totalTypeRetries + mm->totalAffRetries));
   } else {
      return 0;
   }
}

/*
 * Conditions defined to clarify code readability in policy fns
 */
static INLINE Bool
IsKernelPage(const PolicyInput *s)
{
   return(s->world == NULL);
}

static INLINE Bool
IsVMPhysicalPage(const PolicyInput *s)
{
   return(s->world != NULL && s->ppn != INVALID_PPN);
}

static INLINE Bool
IsVMOverheadPage(const PolicyInput *s)
{
   return(s->world != NULL && s->ppn == INVALID_PPN);
}

/* Find the appropriate freeList given the mpn */
static INLINE Buddy_Handle 
MemMapMPN2BuddyHandle(const MemMapInfo *mm, const MPN mpn)
{
   NUMA_Node node  = NUMA_MPN2NodeNum(mpn);

   ASSERT(node != INVALID_NUMANODE);
   ASSERT(VMK_IsValidMPN(mpn));

   if (IsLowMPN(mpn)) {
      return  mm->node[node].buddyLow;
   } else {
      return  mm->node[node].buddyHigh;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * MemMapDecFreePages --
 *
 *      Record num free pages decreased.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE  void
MemMapDecFreePages(NUMA_Node node, int numPages, Bool isLowMPN, Bool isKernel)
{
   MemMapInfo *mm = &memMap;

   ASSERT(SP_IsLockedIRQ(&mm->lock));

   mm->node[node].numFreePages -= numPages;
   mm->numFreePages -= numPages;
   if (isLowMPN) {
      mm->node[node].numFreeLowPages -= numPages;
      mm->numFreeLowPages -= numPages;
   }
   if (isKernel) {
      mm->node[node].numKernelPages += numPages;
      mm->numKernelPages += numPages;
   }

   // update freeLowNodes/freeHighNodes
   if ((mm->node[node].numFreeLowPages <= mm->node[node].reservedLowPages)) {
      mm->freeLowNodes &= ~(1 << node);
   } else if ((mm->node[node].numFreePages <= mm->node[node].numFreeLowPages)) {
      mm->freeHighNodes &= ~(1 << node);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * MemMapIncFreePages --
 *
 *      Record num free pages increased.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE  void
MemMapIncFreePages(NUMA_Node node, int numPages, Bool isLowMPN, Bool isKernel)
{
   MemMapInfo *mm = &memMap;

   ASSERT(SP_IsLockedIRQ(&mm->lock));

   mm->node[node].numFreePages += numPages;
   mm->numFreePages += numPages;
   if (isLowMPN) {
      mm->node[node].numFreeLowPages += numPages;
      mm->numFreeLowPages += numPages;
      if (mm->node[node].numFreeLowPages > mm->node[node].reservedLowPages) {
	 mm->freeLowNodes |= (1 << node);
      }
   } else {
      mm->freeHighNodes |= (1 << node);
   }
   if (isKernel) {
      mm->node[node].numKernelPages -= numPages;
      mm->numKernelPages -= numPages;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * MemMapNumFreeHighPages --
 *
 *      Return the number of free high pages (>4GB).  We don't explicitly
 *      track high pages, so get it by subtracting free low pages from
 *      total free low pages.
 *
 * Results:
 *      number of free high pages (>4GB)
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE uint32
MemMapNumFreeHighPages(const MemMapInfo *mm)
{
   uint32 numFreePages = mm->numFreePages;
   uint32 numFreeLowPages = mm->numFreeLowPages;

   if (numFreePages > numFreeLowPages) {
      return(numFreePages - numFreeLowPages);
   } else {
      return(0);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * MemMapPolicyLowHigh
 *
 *      Policy to decide when to allocate from memory below or above 4GB.
 *      Allocate low pages until low page count drops below threshold
 *      MEMMAP_ALLOC_HIGH_THRESHOLD, and then allocate high.  This way,
 *      there won't be a performance degradation when someone decides to
 *      upgrade a machine beyond 4GB, but doesn't actually use the extra
 *      memory.
 *
 * Results:
 *      The type of memory to allocate
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
#define MEMMAP_MIN_FREE_HIGH_PAGES (128)
static INLINE MM_AllocType
MemMapPolicyLowHigh(const MemMapInfo *mm)
{
   uint32 numFreeLowPages  = mm->numFreeLowPages;
   uint32 numFreeHighPages = MemMapNumFreeHighPages(mm);

   // See bug - 31069 to find out why we need to enforce the following
   ASSERT(MEMMAP_ALLOC_HIGH_THRESHOLD > mm->reservedLowPages);

   if ((numFreeLowPages > MEMMAP_ALLOC_HIGH_THRESHOLD) &&
       (numFreeLowPages > mm->reservedLowPages)) {
      return MM_TYPE_LOW;
   }

   if (numFreeHighPages < MEMMAP_MIN_FREE_HIGH_PAGES) {
      return MM_TYPE_ANY;
   }

   return MM_TYPE_HIGH;
}

/*
 *----------------------------------------------------------------------
 *
 * MemMapUnusedPages --
 *
 *      Returns current total free memory (both below and above 4GB),
 *      but not including the low pages reserved for I/O (for devices that
 *      can't DMA into high memory).
 *
 * Results:
 *      See above
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE uint32
MemMapUnusedPages(const MemMapInfo *mm)
{
   // copying numfreepages to get an atomic snap shot so that the
   // comparison and the subtraction use the same value.
   uint32 numFreePages = mm->numFreePages;
   if (numFreePages >= mm->reservedLowPages) {
      return numFreePages - mm->reservedLowPages;
   } else {
      return 0;
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * MemMap_GetCacheSize --
 *
 *      Returns the size and associativity of the processor's cache in
 *      "assoc" and "size."
 *
 * Results:
 *      Returns VMK_OK on success, VMK_NOT_FOUND if cache size unknown.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
MemMap_GetCacheSize(uint32 *assoc, uint32 *size)
{
   switch (cpuType) {
   case CPUTYPE_INTEL_P6:
   case CPUTYPE_INTEL_PENTIUM4:
      *size = MemMapGetCacheSizeP6(assoc);
      break;
   case CPUTYPE_AMD_ATHLON:
   case CPUTYPE_AMD_DURON:
      *size = MemMapGetCacheSizeAMD(assoc);
      break;
   default:
      return (VMK_NOT_FOUND);
   }
   
   return (VMK_OK);
}

static uint32
MemMapGetCacheSizeP6(int *assoc)
{
   uint32 desc[4];
   int count, times;
   int i, j;
   uint8 d;
   MA size;
   uint32 L2size, L2assoc;
   uint32 L3size, L3assoc;

   L2size = 0;
   L2assoc = 1; 
   L3size = 0;
   L3assoc = 1;

   for (count=0, times=1; count<times; count++) {
      ASM("cpuid" :
          "=a" (desc[0]),
          "=b" (desc[1]),
          "=c" (desc[2]),
          "=d" (desc[3]) :
          "a" (2)
          );

      // indicates how many entries to read
      times = LOBYTE(desc[0]);

      // mask off low byte of entry
      desc[0] &= ~0x000000ff;

      // for each of the 4 word entries returned by the CPUID instruction
      for (i=0; i<4; i++) {
         if (desc[i] & 0x80000000) {
	    // upper bit set means reserved entry
            continue;
         }

	 // for each byte in the word.  We only care about the L2/L3 cache sizes
	 // at this time.  TLB and L1 caches not relevant
         for (j=0; j<4; j++) {
            d = (desc[i] >> (j*8)) & 0x000000ff;
            switch (d) {
            case 0x00:
               break;
            case 0x01:
               LOG(1, "iTLB: 4K page, 4-way, 32ent");
               break;
            case 0x02:
               LOG(1, "iTLB: 4M page, 4-way, 4ent");
               break;
            case 0x03:
               LOG(1, "dTLB: 4K page, 4-way, 64ent");
               break;
            case 0x04:
               LOG(1, "dTLB: 4M page, 4-way, 8ent");
               break;
            case 0x06:
               LOG(1, "iL1: 8KB, 4-way, 32bl");
               break;
            case 0x08:
               LOG(1, "iL1: 16KB, 4-way, 32bl");
               break;
            case 0x0a:
               LOG(1, "dL1: 8KB, 2-way, 32bl");
               break;
            case 0x0c:
               LOG(1, "dL1: 16KB, 2-way, 32bl");
               break;
	    case 0x22:
               LOG(1, "L3: 512KB, 4-way, 64bl");
	       L3assoc = 4;
	       L3size = 512*1024;
	       break;
	    case 0x23:
               LOG(1, "L3: 1MB, 8-way, 64bl");
               L3assoc = 8;
               L3size = 1*1024*1024;
	       break;
	    case 0x25:
               LOG(1, "L3: 2MB, 8-way, 64bl");
               L3assoc = 8;
               L3size = 2*1024*1024;
	       break;
	    case 0x29:
               LOG(1, "L3: 4MB, 8-way, 64bl");
               L3assoc = 8;
               L3size = 4*1024*1024;
	       break;
            case 0x40:
	       LOG(1, "no L2 (P6) or L3 cache (Pentium 4)");
	       break;
            case 0x41:
               LOG(1, "L2: 128KB, 4-way, 32bl");
               L2assoc = 4;
               L2size = 128*1024;
               break;
            case 0x42:
               LOG(1, "L2: 256KB, 4-way, 32bl");
               L2assoc = 4;
               L2size = 256*1024;
               break;
            case 0x43:
               LOG(1, "L2: 512KB, 4-way, 32bl");
               L2assoc = 4;
               L2size = 512*1024;
               break;
            case 0x44:
               LOG(1, "L2: 1024KB, 4-way, 32bl");
               L2assoc = 4;
               L2size = 1024*1024;
               break;
            case 0x45:
               LOG(1, "L2: 2048KB, 4-way, 32bl");
               L2assoc = 4;
               L2size = 2*1024*1024;
               break;
	    case 0x50:
	       LOG(1, "iTLB: 4K/2M/4M page, fully associative, 64ent");
	       break;
	    case 0x51:
	       LOG(1, "iTLB: 4K/2M/4M page, fully associative, 128ent");
	       break;
	    case 0x52:
	       LOG(1, "iTLB: 4K/2M/4M page, fully associative, 256ent");
	       break;
	    case 0x5b:
	       LOG(1, "dTLB: 4K/4M page, fully associative, 64ent");
	       break;
	    case 0x5c:
	       LOG(1, "dTLB: 4K/4M page, fully associative, 128ent");
	       break;
	    case 0x5d:
	       LOG(1, "dTLB: 4K/4M page, fully associative, 256ent");
	       break;
	    case 0x66:
	       LOG(1, "dL1: 8KB, 4-way, 64bl");
	       break;
	    case 0x67:
	       LOG(1, "dL1: 16KB, 4-way, 64bl");
	       break;
	    case 0x68:
	       LOG(1, "dL1: 32KB, 4-way, 64bl");
	       break;
	    case 0x70:
	       LOG(1, "iTrace: 12k uops, 8-way");
	       break;
	    case 0x71:
	       LOG(1, "iTrace: 16k uops, 8-way");
	       break;
	    case 0x72:
	       LOG(1, "iTrace: 32k uops, 8-way");
	       break;
	    case 0x79:
	       LOG(1, "L2: 128KB, 8-way, 64bl");
               L2assoc = 8;
	       L2size = 128*1024;
	       break;
	    case 0x7a:
	       LOG(1, "L2: 256KB, 8-way, 64bl");
               L2assoc = 8;
	       L2size = 256*1024;
	       break;
	    case 0x7b:
	       LOG(1, "L2: 512KB, 8-way, 64bl");
               L2assoc = 8;
	       L2size = 512*1024;
	       break;
	    case 0x7c:
	       LOG(1, "L2: 1MB, 8-way, 64bl");
               L2assoc = 8;
	       L2size = 1024*1024;
	       break;
            case 0x82:
               LOG(1, "L2: 256KB, 8-way, 32bl");
               L2assoc = 8;
               L2size = 256*1024;
               break;
            case 0x83:
               LOG(1, "L2: 512KB, 8-way, 32bl");
               L2assoc = 8;
               L2size = 512*1024;
               break;
            case 0x84:
               LOG(1, "L2: 1MB, 8-way, 32bl");
               L2assoc = 8;
               L2size = 1024*1024;
               break;
            case 0x85:
               LOG(1, "L2: 2MB, 8-way, 32bl");
               L2assoc = 8;
               L2size = 2*1024*1024;
               break;
            default:
               LOG(0, "unknown cache size: 0x%x", d);
               break;
            }
         }
      }
   }

   if ((L3size != 0) && (L3size > L2size)) {
      /* use L3 size if exists and greater than L2 */
      size = L3size;
      *assoc = L3assoc;
   } else if (L2size != 0) {
      /* use L2 size */
      size = L2size;
      *assoc = L2assoc;
   } else {
      /* assume default of 2MB direct mapped cache */
      size = 2048*1024;
      *assoc = 1;
   }

   return size;
}


static uint32
MemMapGetCacheSizeAMD(int *assoc)
{
   uint32 version;
   uint32 reg, size, dummy;

   /*
    * Check the cache size (need dummy, or else gcc 2.9.2 complains)
    */
   ASM("cpuid" :
       "=a" (dummy),
       "=c" (reg) :
       "a" (0x80000006) :
       "ebx", "edx");

   size = reg >> 16;
   LOG(1, "L2: %d KB, %d-way, %d lines/tag, %d bl",
	   size, (reg >> 12) & 0xF, (reg >> 8) & 0xF, reg & 0xFF);
   *assoc = (reg >> 12) & 0xf;

   if (cpuType == CPUTYPE_AMD_DURON) {
      // Rev A0 Duron has a buggy cache size field
      ASM("cpuid" :
	  "=a" (version) :
	  "a" (1) :
	  "ebx", "ecx", "edx");
      if ((version & 0xFFF) == 0x630) {
	 LOG(1, "AMD Duron Rev 0, L2 size = 64KB");
	 size = 64;
      }
   }

   // see AMD Athlon/Duron documentation
   ASSERT(size <= 8192);

   return size * 1024;
}


/*
 *----------------------------------------------------------------------
 *
 * MemMapNextColor --
 *
 *      Returns the optimal next page color. 
 *      First check for the color farthest away (half the cache size
 *      away) as that will protect against interference from closeby data.
 *      Then check 1/4 away, then 3/4, 1/8, 5/8, 3/8, 7/8, then
 *      1/16, 9/16, and so on...  Another way to look at it is that it's a
 *      counter with all the bits in reverse order.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE int
MemMapNextColor(const MemMapInfo *mm, int n)
{
   int b = 1 << (mm->logNumColors - 1);

   for (;;) {
      if (((n ^= b) & b) != 0) {
	 break;
      }
      if ((b >>= 1) == 0) {
	 break;
      }
   }

   return n;
}


static INLINE uint32
Rotate(uint32 origVal, int origShift)
{
   int sizeInBits = sizeof(uint32) * 8;
   int shift = origShift % sizeInBits;
   
   return (origVal << shift) | (origVal >> (sizeInBits - shift));
}

/*
 *----------------------------------------------------------------------
 *
 * CheckMemoryPage --
 *
 * 	Check to see if the given page is good by writing some value and
 *      verifying that it got written.  If checkEveryWord is FALSE, only
 *      check the first word on the page, otherwise check every word.
 *
 * Results:
 *      TRUE if good, FALSE otherwise
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static Bool
CheckMemoryPage(MPN mpn, Bool checkEveryWord)
{
   Bool retval;
   volatile uint32 *va = (volatile uint32*)KVMap_MapMPN(mpn, TLB_LOCALONLY
                                                        | TLB_UNCACHED);

   if (va == NULL) {
      return FALSE;
   }

   *va = 0x12345678;

   retval = (*va == 0x12345678);

   if (retval && checkEveryWord) {
      uint32 i;
      /* write a rotating bit pattern to check for stuck at bits */
      for (i = 0; i < PAGE_SIZE/sizeof(uint32); i+= 2) {
         va[i] = Rotate(0x01234567, i/2);
         va[i+1] = Rotate(0x89abcdef, i/2);
      }
      for (i = 0; i < PAGE_SIZE/sizeof(uint32); i+= 2) {
         if ((va[i] != Rotate(0x01234567, i/2)) || 
             (va[i+1] != Rotate(0x89abcdef, i/2))) {
            retval = FALSE;
            break;
         }
      }
   }

   KVMap_FreePages((void*)va);

   return retval;
}

/*
 *----------------------------------------------------------------------
 *
 * CheckMemoryRange --
 *
 * 	Check to see if the given range of pages is good.
 *      If checkEveryWord is FALSE, do a quick check by checking a word
 *      every megabyte until failure and then check a word every page for
 *      the last MB.  Otherwise, check every single word in the range.
 *
 * Results:
 *      The last good MPN of the given range. If the whole range is bad,
 *      returns INVALID_MPN
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static MPN
CheckMemoryRange(MPN startMPN, MPN endMPN, Bool checkEveryWord)
{
   MPN mpn, lastGoodMPN = INVALID_MPN;

   ASSERT(startMPN <= endMPN);

   if (checkEveryWord) {
      // skip the quick checks. Actually check every single page
      lastGoodMPN = startMPN - 1;
   } else {
      /* first do a quick scan by checking a page every megabyte until we
       * get to near the end or find a bad page */
      for (mpn = startMPN; mpn < endMPN - PAGES_PER_MB; mpn+= PAGES_PER_MB) {
         if (!CheckMemoryPage(mpn, checkEveryWord)) {
            break;
         }
         lastGoodMPN = mpn;
      }
      if (lastGoodMPN == INVALID_MPN) {
         return INVALID_MPN;
      }
   }

   /* now do a slow page-by-page scan from the last known good page till
    * the end of range or a bad page */
   for (mpn = lastGoodMPN+1; mpn <= endMPN; mpn++) {
      if (checkEveryWord && ((mpn % 8192) == 0)) {
         LOG(0, "at mpn=0x%x of range (0x%x:0x%x]", mpn, startMPN, endMPN);
      }
      if (!CheckMemoryPage(mpn, checkEveryWord)) {
         break;
      }
      lastGoodMPN = mpn;
   }

   if (lastGoodMPN < startMPN) {
      // happens if checkEveryWord was set, and none of the pages were valid
      lastGoodMPN = INVALID_MPN;
   }

   return lastGoodMPN;
}

/*
 *----------------------------------------------------------------------
 *
 * MemMap_HotAdd --
 *
 *      Adds free pages to the MemMap page pool while the system is running.
 *	MemMap_HotAdd can be invoked on systems which support the insertion
 *	of physical memory while power is on.  MemMap_HotAdd is invoked
 *	either directly or indirectly via the HAM or HotAddModule.
 *
 *	"startAddress" is the 64 bit address denoting the beginning of the
 *	region of newly available ram.  "size" is a 64 bit value denoting
 *	the amount in bytes of the ram being made available.
 *
 *
 * Results:
 *      Returns VMK_OK on success, an error number otherwise.
 *
 * Side effects:
 *      None.
 *
 *---------------------------------------------------------------------- */
VMK_ReturnStatus
MemMap_HotAdd(MA startAddress,
	      uint64 size,
	      Bool memCheckEveryWord,
	      unsigned char attrib,
	      VMnix_Init *vmnixInit)
{
   MemMapInfo *mm = &memMap;
   int i;
   int newRange;
   MPN startMPN, endMPN;
   SP_IRQL prevIRQL;
   VMK_ReturnStatus status;

   startMPN = MA_2_MPN(startAddress);
   endMPN = (MA_2_MPN(startAddress + size)) - 1;

   if((MA_2_MPN(size)) <= PAGES_PER_MB) {
	memCheckEveryWord = TRUE;
   }

   SP_Lock(&mm->hotMemAddLock);
   // Find the first available mem range slot
   for (newRange = 0; newRange < MAX_VMNIX_MEM_RANGES; newRange++) {
      if (availMemRange[newRange].startMPN == 0) {
         break;
      }
   }
   if(newRange >= MAX_VMNIX_MEM_RANGES) {
      // Out of space to record new ranges
      SP_Unlock(&mm->hotMemAddLock);
      return VMK_NO_MEMORY;
   }

   // Check to make sure this is not an overlapping region 
   // check if 1: old range overlaps the beginning of new one 
   // 2: old range overlaps the end of the new one, or 3: old 
   // range is encapsulated by new one. 
   for (i=0; i<newRange; i++)  {
      if(((startMPN >= availMemRange[i].startMPN) && 
          (startMPN < availMemRange[i].endMPN)) ||
	  ((endMPN >= availMemRange[i].startMPN) && 
           (endMPN < availMemRange[i].endMPN)) ||
	  (startMPN <= availMemRange[i].startMPN && 
          (endMPN >= availMemRange[i].endMPN))) {
         SP_Unlock(&mm->hotMemAddLock);
         return VMK_BAD_PARAM;
      }
   }

   // Update the vmnixInit structure since that is consulted
   // for valid MPN ranges.
   for(i = 0; i < MAX_VMNIX_MEM_RANGES; i++) {
      if(vmnixInit->vmkMem[i].startMPN == 0) {
         vmnixInit->vmkMem[i].startMPN = startMPN;
         vmnixInit->vmkMem[i].endMPN = endMPN;
         break;
      }
   }
   if(i == MAX_VMNIX_MEM_RANGES) {
      SP_Unlock(&mm->hotMemAddLock);
      return VMK_NO_MEMORY;
   }

   // Set the fields for the new memory range
   availMemRange[newRange].startMPN = startMPN;
   availMemRange[newRange].endMPN = endMPN;

   // Add to the total number of pages received by the COS
   biosMemMapStats.totalNumPages += (endMPN - startMPN) + 1;

   // reset per node data
   MemMapResetNodeAvailRange(nodeAvailRange);

   // create a per node range from the current avail range
   status = MemMapCreateNodeRange(mm, &availMemRange[newRange], 
                                  nodeAvailRange, memCheckEveryWord, 
                                  &lastValidMPN);
   ASSERT(status == VMK_OK);
   if (UNLIKELY(status != VMK_OK)) {
      SP_Unlock(&mm->hotMemAddLock);
      return status;
   }

   // Allocate memory for critical vmkernel functions
   status = MemMapAllocCriticalMem(mm, mm->numNodes, nodeAvailRange, 
                                   startMPN, endMPN, TRUE);
   ASSERT(status == VMK_OK);
   if (UNLIKELY(status != VMK_OK)) {
      SP_Unlock(&mm->hotMemAddLock);
      return status;
   }

   // Acquire the memmap lock so that the addition of the new ranges
   // to the buddy allocator and the subsequent adjustment of the
   // free page counters are atomic.
   prevIRQL = SP_LockIRQ(&mm->lock, SP_IRQL_KERNEL);

   // Add the new range to the buddy allocator
   status = MemMapAddNodeRangesToBuddy(mm, nodeAvailRange, TRUE);
   ASSERT(status == VMK_OK);
   if (UNLIKELY(status != VMK_OK)) {
      SP_UnlockIRQ(&mm->lock, prevIRQL);
      SP_Unlock(&mm->hotMemAddLock);
      return status;
   }

   // Reserve low pages for I/O if there is high memory
   if (mm->numHighNodes) {
      uint32 reservedIO=0; 
      uint32 reservedRequest;
      
      reservedRequest = mm->initFreePages/100 * RESERVE_LOWMEM_PCT;
      // The amount of low memory reserved per node will be proportional
      // to how much low memory that node has.
      for (i = 0; i < mm->numNodes; i++) {
         mm->node[i].reservedLowPages =
	    ((uint64) reservedRequest *
	    (uint64)  mm->node[i].totalLowPages) /
	    (uint64)  mm->totalLowPages;
         reservedIO += mm->node[i].reservedLowPages;
      }
      // update memMap reserved count
      mm->reservedLowPages = reservedIO;
   }
   SP_UnlockIRQ(&mm->lock, prevIRQL);
   SP_Unlock(&mm->hotMemAddLock);

#if 0
   /* XXX see bug 45139 */

   /* Signal serverd that physical memory size has changed */
   {  
      VmkEvent_VmkLoadModArgs eventArgs;
      eventArgs.load = 1;
      strncpy(eventArgs.name, "PhysicalMemoryAdd", VMNIX_MODULE_NAME_LENGTH);
      VMKUC_EventRPC(VMKEVENT_MODULE_LOAD, (char *)&eventArgs, sizeof(eventArgs));
   }
#endif
   
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * MemMapCreateNodeRange --
 *
 *    Find the intersection of MPNs between the mpns specified in availRange
 *    and MPNs within each numa node and create a list of available MPNs
 *    per node.
 *
 * Results:
 *    VMK_OK on success, error code otherwise.
 *
 * Side effects:
 *    Per node list of MPNs is created
 *
 *---------------------------------------------------------------------- 
 */
static VMK_ReturnStatus 
MemMapCreateNodeRange(MemMapInfo *mm,
                      NUMA_MemRange *availRange,
                      MemMapNodeAvailRange *nodeRange,
                      Bool memCheckEveryWord,
                      MPN *lastMPN)
{
   uint32 n;
   MPN mpn;
   uint32 rangePages;
   NUMA_MemRange nodeMem;

   // Test memory range and shrink or skip it if bad
   mpn = CheckMemoryRange(availRange->startMPN, availRange->endMPN,
                          memCheckEveryWord);
   if (mpn == INVALID_MPN) {
      biosMemMapStats.numDiscarded += 
            (availRange->endMPN - availRange->startMPN) + 1;

      Warning("ignoring bad memory range[%x:%x]",
	    availRange->startMPN, availRange->endMPN);
      availRange->endMPN = availRange->startMPN - 1;

      return VMK_BAD_ADDR_RANGE;
   } else if (availRange->endMPN != mpn) {
      biosMemMapStats.numDiscarded += (availRange->endMPN - mpn);

      Warning("shrinking memory range[%x:%x] to mpn=%x",
	    availRange->startMPN, availRange->endMPN, mpn);
      availRange->endMPN = mpn;
   }

   if (*lastMPN < availRange->endMPN) {
      *lastMPN = availRange->endMPN;
   }
   rangePages = availRange->endMPN - availRange->startMPN + 1;
   ASSERT(availRange->endMPN >= availRange->startMPN);

   // Now find NUMA nodes pertaining to this mem range and initialize data
   for (n = 0; n < mm->numNodes; n++) {
      nodeMem.startMPN = INVALID_MPN;

       // For each NUMA node, loop through all possible intersections. 
      while (1) {
         MPN curStartMPN;
         uint32 curNumMPNs;
         uint32 k;
         uint32 skippedPages = 0;
         // Find the next intersection of mem range and this node's mem
         if (MemMapIsSystemNUMA()) {
            if (NUMA_MemRangeIntersection(n, availRange, &nodeMem)) {
               LOG(2, "Found intersection of NUMA Node %d and [%x-%x] = %x - %x",
                   n, availRange->startMPN, availRange->endMPN,
                   nodeMem.startMPN, nodeMem.endMPN);
               ASSERT(nodeMem.startMPN >= availRange->startMPN);
               ASSERT(nodeMem.endMPN   <= availRange->endMPN);
            } else {
               break;
            }
         } else {
            // Not a NUMA system.  Add the entire VMNIX mem range.
            nodeMem.startMPN = availRange->startMPN;
            nodeMem.endMPN   = availRange->endMPN;
         }

         curNumMPNs = 0;
         curStartMPN = INVALID_MPN;
         for (k = nodeMem.startMPN; k <= nodeMem.endMPN; k++) {
            /*
             * Don't add the page to nodeRange 
             * o if it's not write-back cachable. 
             *   We've seen this on a DL760.
             * o if mpn is at 1GB boundary (HOST_LINUX_EVIL_MPN), this is 
             *   to avoid bug in host devworld "nopage" handler due to the way that
             *   Linux uses PAGE_OFFSET.  Otherwise (mpn << PAGE_SHIFT) + PAGE_OFFSET
             *   evaluates to zero, which is misinterpreted as an error code.
             */
            if (!MTRR_IsWBCachedMPN(k) || (k == HOST_LINUX_EVIL_MPN)) {
               biosMemMapStats.numDiscarded++;

               if (k != HOST_LINUX_EVIL_MPN) {
                  skippedPages++;
               }
               // Add to node range, all the pages before this mpn
               if (curStartMPN != INVALID_MPN) {
                  MemMapAddRangeToNode(&nodeRange[n], 
                                       curStartMPN,
                                       (curStartMPN + curNumMPNs - 1));
                  curStartMPN = INVALID_MPN;
                  curNumMPNs = 0;
               }
               continue;
            }
            if (curStartMPN == INVALID_MPN) {
               curStartMPN = k;
            }
            curNumMPNs++;
         }
         if (curStartMPN != INVALID_MPN) {
            // Add mem range to this node
            MemMapAddRangeToNode(&nodeRange[n], curStartMPN, 
                                 (curStartMPN + curNumMPNs - 1));
         }
         if (nodeMem.endMPN >= nodeMem.startMPN) {
            rangePages -= nodeMem.endMPN - nodeMem.startMPN + 1;
         }

         if (skippedPages) {
            Warning("skipped %d pages (MTRR not writeback cached)",
                    skippedPages);
         }
         if (!MemMapIsSystemNUMA()) {
            break;   // UMA systems don't have to deal with SRATs
         }
      } /* while (1)     */
   } /* For each node */

   /*
    * By this point, every page in the node range should have been added,
    * claimed by one NUMA node or another.  So rangePages should be 0.
    * If there are pages left unadded, those must be pages outside of the 
    * SRAT memory map.  This signals a memory map mismatch (VMNIX != SRAT)
    * and is a BIOS error or corruption error.
    * An overlap in NUMA memory ranges could also lead to this.
    */
   if (rangePages) { 
      SysAlert("0x%x pages in range [%x:%x] not added.  Memory map "
               "mismatch due to BIOS/SRAT error.  Try upgrading BIOS.",
               rangePages, availRange->startMPN, availRange->endMPN);
      return VMK_INVALID_MEMMAP;
   }
   return VMK_OK;
}



/*
 *----------------------------------------------------------------------
 *
 * MemMap_EarlyInit --
 *
 *       This function more or less performs sanity checks on the 
 *       the range of memory we get from the VMNIX(BIOS-e820) by checking it
 *       against the SRAT tables and also checking for bad memory.
 *       As a side effect of doing these sanity checks we also populate 
 *       the per node "nodeAvailRange" to be used by MemMap_Init so that
 *       it does not have to do the same work again.
 *
 *      - On NUMA machines, the memory allocated is the intersection of
 *        the VMNIX memory map and the ACPI SRAT table memory map.
 *        These two memory maps should be very close, with the VMNIX 
 *        map excluding memory for the COS and reserved areas.
 *      - If the SRAT memory ranges are smaller than the VMNIX ranges,
 *        then a warning is issued and the VMNIX range is clipped to 
 *        ensure that early page allocations from the top of memory 
 *        continue to work
 *      - If the VMNIX (BIOS-e820) memory ranges are much shorter than
 *        the SRAT ones, such that one or more nodes do not have any pages
 *        this function will exit with an error VMK_INVALID_MEMMAP,
 *        leading to abortion of the VMkernel load.
 *      - The previous two conditions are likely to be BIOS errors 
 *        correctable by a BIOS upgrade.
 *
 * Results:
 *      Returns VMK_OK on success, otherwise:
 *      VMK_INVALID_MEMMAP:  Mismatch between SRAT and VMNIX memory map
 *
 * Side effects:
 *      None.
 *
 *---------------------------------------------------------------------- 
 */
VMK_ReturnStatus
MemMap_EarlyInit(VMnix_Init *vmnixInit,
                 Bool memCheckEveryWord)
{
   MemMapInfo *mm = &memMap;

   uint32 cacheSize, assoc;
   int i;
   MPN initPage, startPage;

   // initialize memMap
   memset(mm, 0, sizeof(MemMapInfo));

   SP_InitLock("HotMemAddLock", &mm->hotMemAddLock, SP_RANK_HOTMEMADD);

   if (MemMap_GetCacheSize(&assoc, &cacheSize) != VMK_OK) {
      // default to 2MB, 4-way assoc cache if no information
      Warning("unknown cache size, using 2MB default");
      assoc = 4;
      cacheSize = 2048*1024;
   }
   
   mm->numColors = cacheSize / assoc / PAGE_SIZE;

   // if there are too many, the freelistNode may trash the kseg, so 
   // reduce the number of colors to 1/2 the kseg entries.
   while (mm->numColors > VMK_KSEG_MAP_LENGTH / PAGE_SIZE / 2) {
      Warning("reducing colors(%d) to avoid kseg(%Ld) thrashing",
              mm->numColors, VMK_KSEG_MAP_LENGTH / PAGE_SIZE / 2);
      mm->numColors /= 2;
   }
   
   if (mm->numColors == 0) {
      mm->numColors = 1;
   }

   if (mm->numColors & (mm->numColors-1)) {
      Warning("number of colors is not a power of two: %d", mm->numColors);
      return VMK_FAILURE;
   }

   for (i = 0; i < 32; i++) {
      if ((mm->numColors >> i) & 0x1) {
         break;
      }
   }

   mm->logNumColors = i;
   ASSERT((1 << mm->logNumColors) == mm->numColors);

   // Figure out correct number of nodes
   mm->numNodes = NUMA_GetNumNodes();

   ASSERT(0 < mm->numNodes);
   ASSERT(mm->numNodes <= NUMA_MAX_NODES);

   LOG(1, "cacheSize=%d numColors=%d logNumColors=%d numNodes=%d",
       cacheSize, mm->numColors, mm->logNumColors,
       mm->numNodes);

   mm->bootTimeMinMPN = vmnixInit->vmkMem[0].startMPN;
   mm->bootTimeMaxMPN = vmnixInit->vmkMem[0].endMPN;
   // Retrieve VMNIX's list of memory ranges
   ASSERT(MAX_AVAIL_MEM_RANGES == MAX_VMNIX_MEM_RANGES);
   for (i = 0; i < MAX_VMNIX_MEM_RANGES; i++) {
      availMemRange[i].startMPN = vmnixInit->vmkMem[i].startMPN;
      availMemRange[i].endMPN = vmnixInit->vmkMem[i].endMPN;
      if(availMemRange[i].endMPN != 0) {
         if(availMemRange[i].startMPN < mm->bootTimeMinMPN) {
            mm->bootTimeMinMPN = availMemRange[i].startMPN;
         }
         if(availMemRange[i].endMPN > mm->bootTimeMaxMPN) {
            mm->bootTimeMaxMPN = availMemRange[i].endMPN;
         }
         // Count the total number of pages received from the COS 
         // supplied BIOS map
         if (availMemRange[i].startMPN <= availMemRange[i].endMPN) {
            biosMemMapStats.totalNumPages += 
               (availMemRange[i].endMPN - availMemRange[i].startMPN) + 1;
         }
      }
   }

   // Go through each NUMA node and initialize variables
   for (i=0; i < mm->numNodes; i++) {
      mm->node[i].nodeID = i;
      mm->node[i].totalNodePages = 0;
      mm->node[i].totalLowPages = 0;
      mm->node[i].buddyLow = NULL;
      mm->node[i].buddyHigh = NULL;
   }

   mm->initFreePages = 0;
   initPage = vmnixInit->firstMPN;
   startPage = vmnixInit->nextMPN;

   // reset per node data
   MemMapResetNodeAvailRange(nodeAvailRange);

   // Go through each VMNIX memory range and split it into per node range
   for (i = 0; i < MAX_AVAIL_MEM_RANGES; i++) {
      VMK_ReturnStatus status;
      if (availMemRange[i].startMPN == 0) {
         break;
      }

      // Make sure this range is above AllocEarly and COS memory
      if (availMemRange[i].endMPN < startPage) {
         if (availMemRange[i].startMPN <= availMemRange[i].endMPN) {
            // pages below 'startPage' are essentially used by the vmkloader
            // for vmkernel setup, hence charge these to the 'early' kernel use
            biosMemMapStats.numKernelUse += 
                  (availMemRange[i].endMPN - availMemRange[i].startMPN) + 1;
         }
         availMemRange[i].startMPN = startPage;
         availMemRange[i].endMPN = startPage - 1;
         continue;
      }

      if (availMemRange[i].startMPN < startPage) {
         // pages below 'startPage' are essentially used by the vmkloader
         // for vmkernel setup, hence charge these to the 'early' kernel use
         biosMemMapStats.numKernelUse += 
                  (startPage - availMemRange[i].startMPN);
         availMemRange[i].startMPN = startPage;
      }

      // create a per node range for the current avail range
      status = MemMapCreateNodeRange(mm, &availMemRange[i], nodeAvailRange, 
                                     memCheckEveryWord, &lastValidMPN);
      ASSERT(status == VMK_OK || status == VMK_BAD_ADDR_RANGE);
      if (UNLIKELY(status != VMK_OK && status != VMK_BAD_ADDR_RANGE)) {
         return status;
      }
   }

   for (i=0; i < mm->numNodes; i++) {
      // Check that this node has pages allocated.
      if (!nodeAvailRange[i].numPages) {
	 SysAlert("No pages allocated to Node %d -- big mismatch between "
                  "BIOS and SRAT memory maps, or MTRR error, "
                  "or user removed all memory from a Node. "
                  "Try checking memory or upgrading BIOS.", i);
         return VMK_INVALID_MEMMAP;
      }
   }
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * MemMapAddNodeRangesToBuddy --
 *
 *    Iterate through the nodes and the range of MPNs within each
 *    node and add the mpns in each range it to the free lists 
 *    maintained by the buddy allocator, also initialize the
 *    various memap counters. 
 *
 *
 * Results:
 *      Returns VMK_OK on success, error code otherwise:
 *
 * Side effects:
 *    MPNs are handed over to the buddy allocator.
 *
 *---------------------------------------------------------------------- 
 */
static VMK_ReturnStatus
MemMapAddNodeRangesToBuddy(MemMapInfo *mm,
                           MemMapNodeAvailRange *nodeAvailRange,
                           Bool hotAdd)
{
   uint32 i;
   MPN startPage = INVALID_MPN;
   for (i = 0; i < mm->numNodes; i++) {
      uint32 j;
      uint32 numPagesAdded = 0;
      uint32 numLowPagesAdded = 0;
      uint32 loBuddyOvhd = 0;
      uint32 hiBuddyOvhd = 0;
      uint32 totLoBuddyOvhd = 0;
      uint32 totHiBuddyOvhd = 0;
      MemMapNodeAvailRange *avail = &nodeAvailRange[i];

      if (avail->numPages <= 0) {
         continue;
      }

      if (!hotAdd) {
         ASSERT(mm->node[i].buddyLow == NULL);
         ASSERT(mm->node[i].buddyHigh == NULL);
      }
      ASSERT(avail->numRanges <= MEMMAP_MAX_NODE_AVAIL_RANGES);

      for (j = 0; j < avail->numRanges; j++) {
         NUMA_MemRange *nodeMem = &avail->nodeRange[j];
         uint32 rangeNumMPNs;
         MPN rangeStartMPN;
         VMK_ReturnStatus status;

         if (nodeMem->startMPN > nodeMem->endMPN) {
            continue;
         }
	 // Insert pages into freelist
	 LOG(1, "inserting Node %d pages from %x to %x", i,
	     nodeMem->startMPN, nodeMem->endMPN);
         if (UNLIKELY(startPage == INVALID_MPN)) {
            startPage = nodeMem->startMPN;
         }
         rangeStartMPN = nodeMem->startMPN;
         rangeNumMPNs = nodeMem->endMPN - nodeMem->startMPN + 1;
         // Add to free list, all the pages in this range.
         status = MemMapAddRange(mm, i, rangeStartMPN, rangeNumMPNs, 
                                 &loBuddyOvhd, &hiBuddyOvhd);
         ASSERT(status == VMK_OK);
         if (UNLIKELY(status != VMK_OK)) {
            Warning("Failed to add %d pages starting with 0x%x, status = %d",
                    rangeNumMPNs, rangeStartMPN, status);
            continue;
         }
         if (IsLowMPN(nodeMem->endMPN)) {
            numLowPagesAdded += rangeNumMPNs;
         } else if (IsLowMPN(nodeMem->startMPN)) {
            numLowPagesAdded += (FOUR_GB_MPN - nodeMem->startMPN);
         }
         numPagesAdded += rangeNumMPNs; 
         totLoBuddyOvhd += loBuddyOvhd;
         totHiBuddyOvhd += hiBuddyOvhd;

	 DEBUG_ONLY( {
            MPN mpn;
            for (mpn = nodeMem->startMPN;  mpn <= nodeMem->endMPN; mpn++) {
               ASSERT(MTRR_IsWBCachedMPN(mpn));
               ASSERT(mpn != HOST_LINUX_EVIL_MPN);
            }
         });
      }
      /*
       * NOTE: If we are 'booting' we cannot use SP_LockIRQ here to protect 
       * free page counters, 
       * since the SP module hasn't been initialized yet.  However this
       * shouldn't be a problem since there should only be one VMkernel_Init
       * thread running.
       */
      if (hotAdd) {
         ASSERT(SP_IsLockedIRQ(&mm->lock));
      }
      mm->node[i].totalNodePages += numPagesAdded;
      mm->node[i].numFreePages += (numPagesAdded - totLoBuddyOvhd) - 
                                   totHiBuddyOvhd;
      mm->totalMemPages += numPagesAdded;
      mm->numFreePages += (numPagesAdded - totLoBuddyOvhd) - totHiBuddyOvhd;
      mm->initFreePages += (numPagesAdded - totLoBuddyOvhd) - totHiBuddyOvhd;

      mm->node[i].totalLowPages += numLowPagesAdded;
      mm->node[i].numFreeLowPages += numLowPagesAdded - totLoBuddyOvhd;
      mm->totalLowPages += numLowPagesAdded;
      mm->numFreeLowPages += numLowPagesAdded - totLoBuddyOvhd;

      mm->node[i].numKernelPages += (totLoBuddyOvhd + totHiBuddyOvhd);
      mm->numKernelPages += totLoBuddyOvhd + totHiBuddyOvhd;

      biosMemMapStats.numManagedByMemMap += numPagesAdded;

      // if this is the first node reset low/high node stats
      if (i == 0) {
         mm->numLowNodes = 0;
         mm->freeLowNodes = 0;
         mm->numHighNodes = 0;
         mm->freeHighNodes = 0;
         mm->validNodes = 0;
         mm->freeResNodes = 0;
      }
      // Check that this node has pages allocated. Init node masks.
      if (mm->node[i].totalNodePages) {
	 ASSERT(mm->node[i].buddyLow != NULL ||
		mm->node[i].buddyHigh != NULL);
	 if (mm->node[i].buddyLow) {
            if (!hotAdd) {
               ASSERT(mm->node[i].numFreeLowPages > 0);
            }
	    mm->numLowNodes++;
	    mm->freeLowNodes |= (1 << i);
	 }
	 if (mm->node[i].buddyHigh) {
            if (!hotAdd) {
               ASSERT(mm->node[i].numFreePages - mm->node[i].numFreeLowPages > 0);
            }
	    mm->numHighNodes++;
	    mm->freeHighNodes |= (1 << i);
	 }
         mm->validNodes = mm->freeLowNodes | mm->freeHighNodes;
         mm->freeResNodes = mm->freeLowNodes;
      } else {
         ASSERT(mm->node[i].buddyLow == NULL &&
                mm->node[i].buddyHigh == NULL);
         ASSERT(!hotAdd);
         SysAlert("No pages allocated to Node %d -- big mismatch between "
                  "BIOS and SRAT memory maps, or MTRR error, "
                  "or user removed all memory from a Node. "
                  "Try checking memory or upgrading BIOS.", i);
         return VMK_INVALID_MEMMAP;
      }
   }
   if (!hotAdd) {
      mm->start = startPage;
   }
   ASSERT(mm->numFreeLowPages > 0);
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * MemMap_Init --
 *
 *    Initialize the memmap module.
 *    Go through the available range of MPNs and set up the memmap
 *    structures needed to manage them.
 *
 * Results:
 *      Returns VMK_OK on success, otherwise:
 *       VMK_NO_MEMORY: could not allocate heap memory, or no low memory
 *       VMK_INVALID_MEMMAP:  Mismatch between SRAT and VMNIX memory map
 *
 * Side effects:
 *      None.
 *
 *---------------------------------------------------------------------- 
 */
VMK_ReturnStatus
MemMap_Init(void)
{
   MemMapInfo *mm = &memMap;

   uint32 totalAllNodePages;
   uint32 reservedLowPages;
   DEBUG_ONLY(MPN startPage = INVALID_MPN);
   int i;
   VMK_ReturnStatus status;

   DEBUG_ONLY(mm->memMapInitCalled = TRUE);
   ASSERT(0 < mm->numNodes);
   ASSERT(mm->numNodes <= NUMA_MAX_NODES);

   mm->initFreePages = 0;
   mm->numFreeLowPages = 0;
   totalAllNodePages = 0;
   mm->numLowNodes = mm->numHighNodes = 0;
   mm->freeLowNodes = mm->freeHighNodes = 0;
   
   // Allocate memory for critical vmkernel functions
   status = MemMapAllocCriticalMem(mm, mm->numNodes, nodeAvailRange, 
                                   mm->bootTimeMinMPN, mm->bootTimeMaxMPN, 
                                   FALSE);
   ASSERT(status == VMK_OK);
   if (UNLIKELY(status != VMK_OK)) {
      return status;
   }

   // Add the ranges to the buddy allocator
   status = MemMapAddNodeRangesToBuddy(mm, nodeAvailRange, FALSE);
   ASSERT(status == VMK_OK);
   if (UNLIKELY(status != VMK_OK)) {
      return status;
   }

   ASSERT(biosMemMapStats.totalNumPages == (biosMemMapStats.numDiscarded +
                                            biosMemMapStats.numKernelUse +
                                            biosMemMapStats.numManagedByMemMap));

   // sanity checks
   if (mm->numLowNodes < 1) {
      SysAlert("No low memory available -- vmkernel cannot continue");
      return VMK_NO_MEMORY;
   }

   // debugging
   mm->totalTypeRetries = 0;
   mm->totalAffRetries = 0;
   mm->totalGoodAllocs = 0;
   mm->totalBadAllocs  = 0;
   mm->totalColorNodeLookups = 0;

   // Reserve low pages for I/O if there is high memory
   mm->reservedLowPages = 0;
   if (mm->numHighNodes) {
      reservedLowPages = mm->initFreePages/100 * RESERVE_LOWMEM_PCT;
   } else {
      reservedLowPages = 0;
   }

   // The amount of low memory reserved per node will be proportional
   // to how much low memory that node has.
   for (i = 0; i < mm->numNodes; i++) {
      mm->node[i].reservedLowPages =
	 ((uint64) reservedLowPages *
	 (uint64)  mm->node[i].numFreeLowPages) /
	 (uint64)  mm->numFreeLowPages;
      mm->reservedLowPages += mm->node[i].reservedLowPages;
   }

   // Dump out MemMapInfo and MemMapNode contents
   DEBUG_ONLY ({
      LOG(1, "start=0x%x, totalMemPages=0x%x, totalAllNodePages=0x%x, "
	  "initFreePages=0x%x, reservedPages=0x%x, pagesPerColor=0x%x",
	  startPage,
	  mm->totalMemPages,
	  totalAllNodePages,
	  mm->initFreePages,
	  mm->reservedLowPages,
	  CEIL(mm->initFreePages, mm->numColors));
      LOG(1, "numLowNodes=%d, numHighNodes=%d, freeLowNodes=0x%x, "
	  "freeHighNodes=0x%x, validNodes=0x%x, reservedLowPages=%d",
	  mm->numLowNodes,
	  mm->numHighNodes,
	  mm->freeLowNodes,
	  mm->freeHighNodes,
          mm->validNodes,
	  reservedLowPages);
      MemMap_LogState(1);
      MemMapLogFreePages();
   });

   // Initialize state of policy function
   mm->nextKernelColor = mm->numColors / 2;
   mm->nextNode = 0;

   MEMMAP_DEBUG_ONLY(MemMapLogFreePages());
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * MemMap_LateInit --
 *
 *    Register proc nodes
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *---------------------------------------------------------------------- 
 */
void
MemMap_LateInit(void)
{
   memmapUseKseg = TRUE; // ok to use kseg now
   // rank check
   ASSERT(SP_RANK_MEMMAP < SP_RANK_BUDDY_HOTADD &&
          SP_RANK_MEMMAP < SP_RANK_MEMSCHED_STATE);
   SP_InitLockIRQ("MemMapLock", &memMap.lock, SP_RANK_MEMMAP);

   Proc_InitEntry(&memProcEntry);
   memProcEntry.read = MemProcRead;
   Proc_Register(&memProcEntry, "mem", FALSE);

   Proc_InitEntry(&memDebugProcEntry);
   memDebugProcEntry.read = MemDebugProcRead;
   memDebugProcEntry.write = MemDebugProcWrite;
   Proc_RegisterHidden(&memDebugProcEntry, "memDebug", FALSE);
}

/*
 *----------------------------------------------------------------------
 *
 * MemMapAllocCriticalMem --
 *    
 *    Allocate memory for all the critical functions statically registered
 *    with memmap.
 *
 * Results:
 *    VMK_OK on success, error code otherwise.
 *
 * Side effects:
 *    None.
 *
 *---------------------------------------------------------------------- 
 */
static VMK_ReturnStatus
MemMapAllocCriticalMem(MemMapInfo *mm, 
                       uint32 numNodes, 
                       MemMapNodeAvailRange *nodeAvailRange,
                       uint32 minMPN,
                       uint32 maxMPN,
                       Bool hotAdd)
{
   uint32 i;
   for (i = 0; i < MEMMAP_CRITICAL_MEM_MAX; i++) {
      MPN startMPN;
      VMK_ReturnStatus status;
      NUMA_Node node;
      Bool align2M = FALSE;
      uint32 numReq = criticalMemFuncs[i].getNumMPNs(minMPN, maxMPN, hotAdd);
      LOG(0, "%d needs %d pages", i, numReq);
      if (UNLIKELY(numReq <= 0)) {
         Warning("region %d requires no memory for minMPN 0x%x "
                 " maxMPN 0x%x, hotAdd %d", i, minMPN, maxMPN, hotAdd);
         continue;
      }
      if (numReq >= BYTES_2_PAGES(2 * MB)) {
         align2M = TRUE;
      }
      // First try to get MPNs that are 2M aligned, if required
      status = MemMapGetCriticalMPNs(mm, numNodes, numReq, &startMPN, 
                                     &node, align2M);
      if (status != VMK_OK && align2M) {
         // forget the alignment, just get the critical MPNs
         Warning("Unable to allocate 2M aligned pages for region %d, "
                 "num of pages required = %d ", i, numReq);
         status = MemMapGetCriticalMPNs(mm, numNodes, numReq, &startMPN, 
                                        &node, FALSE);
      }
      ASSERT(status == VMK_OK);
      if (UNLIKELY(status != VMK_OK)) {
         return status;
      }

      LOG(0, "%d is assigned [%x %x]", i, startMPN, startMPN + numReq - 1);
      criticalMemFuncs[i].assignMPNs(minMPN, maxMPN, hotAdd, numReq, startMPN);

      // Charge the Critical memory usage to 'early' vmkernel usage as 
      // these uses happen even before the memmap module is fully initialized
      biosMemMapStats.numKernelUse += numReq;
   }
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * MemMapGet2MegAlignedPage --
 *
 *    Get numReq pages with the start page aligned at 2M.
 *    If required pages are found, 'outRange' is created 
 *    by removing the  selected pages from 'inRange'. 
 *
 * Results:
 *    2M aligned MPN on success, 
 *    INVALID_MPN on failure
 *
 * Side effects:
 *    none.
 *
 *---------------------------------------------------------------------- 
 */
static MPN
MemMapGet2MegAlignedPage(MemMapInfo *mm,          // IN
                         const uint32 numReq,     // IN
                         NUMA_MemRange *inRange,  // IN
                         NUMA_MemRange *outRange) // OUT
{
   const MPN start = inRange->startMPN;
   const MPN end = inRange->endMPN;
   MPN chosenMPN = end - numReq + 1;

   // initialize the return range
   MemMapResetRange(&outRange[0]);
   MemMapResetRange(&outRange[1]);

   chosenMPN = ROUNDDOWN(chosenMPN, BYTES_2_PAGES(2 * MB));
   if (chosenMPN < start) {
      return INVALID_MPN;
   }
   
   // we have the 2M aligned MPN, now split the
   // given range to remove the required number of
   // MPNs from it

   // if required MPNs are found at the begining
   if (chosenMPN == start) {
      outRange[0].startMPN = chosenMPN + numReq;
      outRange[0].endMPN = end;
      return chosenMPN;
   }

   // if required MPNs are found at the end
   if ((chosenMPN + numReq - 1) == end) {
      outRange[0].startMPN = start;
      outRange[0].endMPN = chosenMPN - 1;
      return chosenMPN;
   }

   // required MPNs are found in the middle
   ASSERT(chosenMPN > start);
   ASSERT((chosenMPN + numReq - 1) < end);

   outRange[0].startMPN = start;
   outRange[0].endMPN = chosenMPN - 1;
   outRange[1].startMPN = chosenMPN + numReq;
   outRange[1].endMPN = end;

   return chosenMPN;
}


/*
 *----------------------------------------------------------------------
 *
 * MemMapGetCriticalMPNs --
 *
 *    Find the requested number of MPNs by looking at all the nodes and
 *    rearranging the ranges in the nodes if required.
 *
 * Results:
 *    VMK_OK on success, error code otherwise
 *    Set startMPN and node to the memory allocated
 *
 * Side effects:
 *    The range of available MPNs in a node may be changed/rearranged
 *
 *---------------------------------------------------------------------- 
 */
static VMK_ReturnStatus
MemMapGetCriticalMPNs(MemMapInfo *mm,
                      uint32 numNodes,
                      uint32 numReq,
                      MPN *startMPN,   //OUT
                      NUMA_Node *node, //OUT
                      Bool align2M)
{
   int j;
   ASSERT(numReq > 0);
   *startMPN = INVALID_MPN;
   if (numReq == 0) {
      return VMK_FAILURE;
   }
   // start looking for those MPNs in the available nodes
   // Look for highest range and highest mpns first, to avoid low page usage
   for (j = (numNodes - 1); j >= 0; j--) {
      MemMapNodeAvailRange *availRange = &nodeAvailRange[j];
      int k;
      if (availRange->numRanges <= 0) {
         continue;
      }

      for (k = (availRange->numRanges - 1); k >= 0;  k--) {
         VMK_ReturnStatus status = VMK_OK;
         NUMA_MemRange *curRange = &availRange->nodeRange[k];
         NUMA_MemRange splitRange[2];
         const MPN start = curRange->startMPN;
         const MPN end = curRange->endMPN;
         ASSERT(start != INVALID_MPN);
         ASSERT(end != INVALID_MPN);
         if ((end < start) || (numReq > (end - start + 1))) {
            continue;
         }
         if (align2M) {
            // try and get 2M aligned pages
            *startMPN = MemMapGet2MegAlignedPage(mm, numReq, curRange, splitRange);
            if (*startMPN == INVALID_MPN) {
               continue;
            }
         } else {
            // pick MPNs from end, no alignment requested
            *startMPN = end - numReq + 1;
            // adjust the given range
            splitRange[0].startMPN = start;
            splitRange[0].endMPN = *startMPN - 1;
            MemMapResetRange(&splitRange[1]);
         }

         // reduce num pages in this node
         availRange->numPages -= numReq;

         ASSERT(splitRange[0].startMPN != INVALID_MPN);
         ASSERT(splitRange[0].endMPN != INVALID_MPN);

         // Adjust the cur range
         curRange->startMPN = splitRange[0].startMPN;
         curRange->endMPN = splitRange[0].endMPN;

         // if MPNs were found in the middle, the original range is split,
         // add the split part back to this node
         if (splitRange[1].startMPN != INVALID_MPN) {

            // decrement these pages from this node as they 
            // will be added back in the following call to 
            // MemMapAddRangeToNode
            availRange->numPages -= splitRange[1].endMPN - 
                                    splitRange[1].startMPN + 1;
            // add the split range to the end of this node 
            status = MemMapAddRangeToNode(availRange, splitRange[1].startMPN,
                                          splitRange[1].endMPN);
            ASSERT(status == VMK_OK);
         }
         *node = j;
         return status;
      }
   } // for all nodes
   return VMK_FAILURE;
}


/*
 *----------------------------------------------------------------------
 *
 * MemMapHandOverToBuddy --
 *
 *    Submit the given range to be managed by the buddy allocator.
 *    If '*handle' is NULL a new memspace is created otherwise
 *    we hot add the range to the memspace represented by the handle
 *
 * Results:
 *    VMK_OK on success, Failure code otherwise
 *
 * Side effects:
 *    none.
 *
 *---------------------------------------------------------------------- 
 */
static VMK_ReturnStatus
MemMapHandOverToBuddy(MemMapInfo *mm, 
                      Buddy_Handle *handle,  // IN (IN/OUT when creating buddy)
                      Buddy_DynamicRangeInfo *dynRange,
                      const NUMA_Node node,
                      const NUMA_MemRange *range,
                      const uint32 numOvhdMPNs,
                      const MPN buddyOvhdMPN,
                      const Bool low)
{
   char *buddyMem;
   Buddy_AddrRange addrRange;
   const Bool create = (*handle == NULL);
   XMap_MPNRange xmapRange;
   VMK_ReturnStatus status;

   // for hot add numOvhdMPNs can sometimes be 0 because hot add
   // may be adding a region which is completely consumed by regions
   // already added, for such regions the buddy does not need additional memory
   if (numOvhdMPNs) {
      // Use XMap to map these MPNs into vmkernel virtual address space
      xmapRange.startMPN = buddyOvhdMPN;
      xmapRange.numMPNs = numOvhdMPNs;
      buddyMem = XMap_Map(numOvhdMPNs, &xmapRange, 1);
      ASSERT(buddyMem);
   } else {
      ASSERT(!create);
      buddyMem = NULL;
   }

   ASSERT(range->endMPN >= range->startMPN);
   ASSERT(range->endMPN <= lastValidMPN);
   addrRange.start = range->startMPN;
   addrRange.len = range->endMPN - range->startMPN + 1; 

   if (create) {
      ASSERT(dynRange);
      ASSERT(dynRange->rangeInfo.start <= addrRange.start);
      if (dynRange->rangeInfo.start < addrRange.start) {
         // if the start of this range has increased because of
         // overhead memory allocations then adjust the
         // maxLen accordingly.
         ASSERT((addrRange.start - dynRange->rangeInfo.start) ==
                 numOvhdMPNs);
         dynRange->maxLen = dynRange->maxLen - numOvhdMPNs; 
      }
      dynRange->rangeInfo.start = addrRange.start;
      dynRange->rangeInfo.len = addrRange.len;
      status = Buddy_CreateDynamic(dynRange, PAGES_2_BYTES(numOvhdMPNs), 
                                   buddyMem, 1, &addrRange, handle);
      LOG(0, "creating buddy, startMPN = 0x%x, numMPNs = %d, buddy requires "
          "%Ld bytes, numColorBits = %d", addrRange.start, addrRange.len, 
          PAGES_2_BYTES(numOvhdMPNs), dynRange->rangeInfo.numColorBits);
   } else {
      status = Buddy_HotAddRange(*handle, PAGES_2_BYTES(numOvhdMPNs), buddyMem,
                                 addrRange.start, addrRange.len, 1, &addrRange);
      LOG(0, "hot adding buddy, startMPN = 0x%x, numMPNs = %d, "
          "buddy requires %Ld bytes", addrRange.start, addrRange.len,
          PAGES_2_BYTES(numOvhdMPNs));
   }
   ASSERT(status == VMK_OK);
   if (UNLIKELY(status != VMK_OK)) {
      SysAlert("Failed to %s range for node %d, low %d, "
               "startMPN = 0x%x, numMPNs = %d, status = %d", 
               (dynRange ? "create" : "hot add"), 
               node, low, addrRange.start, addrRange.len, status);
   }
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * MemMapAddToBuddy --
 *
 *    Find the amount of overhead memory required for adding this range
 *    of MPNs. Allocate this overhead memory smartly (try to 2M align it)
 *    and handover the range (or ranges if current range is split) to the
 *    buddy allocator.
 * 
 *    NOTE: handle is IN/OUT parameter when creating a region
 *          and IN parameter for hot adding.
 *
 * Results:
 *    VMK_OK on success, Failure code otherwise
 *
 * Side effects:
 *    Some memory of the given range will be set aside for the use
 *    of the buddy allocator
 *
 *---------------------------------------------------------------------- 
 */
static VMK_ReturnStatus
MemMapAddToBuddy(MemMapInfo *mm, 
                 const NUMA_Node node,
                 Buddy_Handle *handle,   // IN (IN/OUT when creating buddy)
                 const Bool low,
                 const MPN startMPN,
                 const uint32 numMPNs,
                 uint32 *numBuddyOvhdMPN)
{
   uint32 memReq;
   VMK_ReturnStatus status;
   const Bool create = (*handle == NULL);
   Buddy_DynamicRangeInfo dynRange;
   NUMA_MemRange splitRange[2];
   MPN buddyOvhdStartMPN = INVALID_MPN;

   // initialize return value
   *numBuddyOvhdMPN = 0;

   if (create) { 
      // create the memspace
      snprintf(dynRange.rangeInfo.name, BUDDY_MAX_MEMSPACE_NAME,
                 "memmap-%.2d-%s", node, (low ? "lo" : "hi"));
      dynRange.rangeInfo.start = startMPN;
      dynRange.rangeInfo.len = numMPNs;

      dynRange.rangeInfo.minSize = MEMMAP_MIN_BUF_SIZE;
      dynRange.rangeInfo.maxSize = MEMMAP_MAX_BUF_SIZE;
      dynRange.rangeInfo.numColorBits = fls(mm->numColors) - 1;
      if (low) {
         dynRange.maxLen = MEMMAP_MAX_LOW_LEN;
      } else {
         dynRange.maxLen = MEMMAP_MAX_HIGH_LEN;
      }
      dynRange.minHotAddLenHint = MEMMAP_MIN_HOTADD_LEN;
      memReq = Buddy_DynamicRangeMemReq(&dynRange);
      ASSERT(memReq > 0);
   } else {
      // hot add the range
      status = Buddy_HotAddMemRequired(*handle, startMPN, numMPNs, &memReq);
      ASSERT(status == VMK_OK);
      if (UNLIKELY(status != VMK_OK)) {
         SysAlert("Failed to hot add range for node %d, low %d, "
                  "startMPN = 0x%x, numMPNs = %d, status = %d", 
                  node, low, startMPN, numMPNs, status);
         return status;
      }
   }

   *numBuddyOvhdMPN = CEILING(memReq, PAGE_SIZE);
   if (*numBuddyOvhdMPN >= numMPNs) {
      Warning("range too small ignoring, startMPN = 0x%x, numMPNs = %d, "
              "buddy requires %d bytes, numColorBits = %d",
              startMPN, numMPNs, memReq, dynRange.rangeInfo.numColorBits);
      // account this ignored range as buddy overhead
      *numBuddyOvhdMPN = numMPNs;
      return VMK_OK;
   }

   // If the amount of overhead memory required is greater than 2M, try
   // and align the buddy overhead memory on a 2M boundary so that
   // XMap can later optimize by using large pages i.e 2M pages.
   if (*numBuddyOvhdMPN >= BYTES_2_PAGES(2 * MB)) {
      NUMA_MemRange curRange;
      curRange.startMPN = startMPN;
      curRange.endMPN = startMPN + numMPNs - 1;
      buddyOvhdStartMPN = MemMapGet2MegAlignedPage(mm, *numBuddyOvhdMPN, 
                                                   &curRange, splitRange);
   }

   // Either numBuddyOvhdMPNs are less than 2M or we failed to get 
   // a 2M aligned page, in either case allocate the required
   // overhead pages
   if (buddyOvhdStartMPN == INVALID_MPN) {
      buddyOvhdStartMPN = startMPN + numMPNs - *numBuddyOvhdMPN;
      splitRange[0].startMPN = startMPN;
      splitRange[0].endMPN = buddyOvhdStartMPN - 1;
      MemMapResetRange(&splitRange[1]);
      // for hot add numBuddyOvhdMPN can sometimes be 0 because hot add
      // may be adding a region which is completely consumed by regions
      // already added, for such regions the buddy has already allocated
      // the overhead memory while adding/creating the other regions and
      // no more additional overhead memory is required
      if (*numBuddyOvhdMPN == 0) {
         buddyOvhdStartMPN = INVALID_MPN;
      }
   }

   // Hand over this range of memory to the buddy for management
   status = MemMapHandOverToBuddy(mm, handle, &dynRange, node, &splitRange[0],
                                  *numBuddyOvhdMPN, buddyOvhdStartMPN, low);

   ASSERT(status == VMK_OK);
   if (status == VMK_OK &&
       splitRange[1].startMPN != INVALID_MPN) {
      // No additional overhead memory is required by the buddy 
      // for this split range because all the required overhead 
      // memory has already been allocated and passed on to the 
      // buddy in the previous call.
      status = MemMapHandOverToBuddy(mm, handle, NULL, node, &splitRange[1],
                                     0, INVALID_MPN, low);
      ASSERT(status == VMK_OK);
   }
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * MemMapAddRange --
 *
 *    Add this range of the MPNs to be managed by MemMap.
 *
 * Results:
 *    VMK_OK on success, Failure code otherwise
 *
 * Side effects:
 *    none.
 *
 *---------------------------------------------------------------------- 
 */
static VMK_ReturnStatus 
MemMapAddRange(MemMapInfo *mm,
               const NUMA_Node node,
               const MPN startMPN, 
               const uint32 numMPNs,
               uint32 *loBuddyOvhd,
               uint32 *hiBuddyOvhd)
{
   const MPN endMPN = startMPN + numMPNs - 1;
   VMK_ReturnStatus status;
   // Initialize return values
   *loBuddyOvhd = 0;
   *hiBuddyOvhd = 0;
   if (endMPN < FOUR_GB_MPN) {
      // range is low , add to low memspace
      status = MemMapAddToBuddy(mm, node, &mm->node[node].buddyLow, 
                                TRUE, startMPN, numMPNs, loBuddyOvhd);
   } else if (startMPN >= FOUR_GB_MPN) {
      // range is high, add to high memspace
      status = MemMapAddToBuddy(mm, node, &mm->node[node].buddyHigh,
                                FALSE, startMPN, numMPNs, hiBuddyOvhd);
   } else {
      // range straddles high low boundary
      uint32 num;
      // Add to low memspace, startMPN to (FOUR_GB_MPN - 1) 
      num = FOUR_GB_MPN - startMPN;
      status = MemMapAddToBuddy(mm, node, &mm->node[node].buddyLow,
                                TRUE, startMPN, num, loBuddyOvhd);

      // Add to high memspace, FOUR_GB_MPN to endMPN
      num = endMPN - FOUR_GB_MPN + 1;
      status = MemMapAddToBuddy(mm, node, &mm->node[node].buddyHigh,
                                FALSE, FOUR_GB_MPN, num,
                                hiBuddyOvhd);
   }
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * MemMapNumFreePages
 *
 *    Get the number of free pages for the given color
 *
 * Results:
 *      Number of pages on the freelists, or 0 if freeList is NULL.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
uint32
MemMapNumFreePages(Buddy_Handle handle, MM_Color color)
{
   if (handle == NULL) {
      return 0;
   }

   if (color == MM_COLOR_ANY) {
      return Buddy_GetNumFreeBufs(handle);
   } else {
      return Buddy_GetNumFreeBufsForColor(handle, color);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * MemMapAllocPageWait --
 *
 *      Wait for at most "msTimeout" to get a machine page of
 *      (node, color, type).  This is basically a wait loop around
 *      MemMapAllocPages().  The wait loop terminates if a checkpoint
 *      is starting or a world death is pending.
 *
 * Results:
 *      Allocated mpn if low memory is available;
 *      INVALID_MPN if msTimeout milliseconds elapsed. 
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static MPN
MemMapAllocPageWait(World_Handle *world,
                    PPN ppn, 
                    MM_NodeMask nodeMask,
                    MM_Color color,
                    MM_AllocType type,
                    uint32 msTimeout)
{
   MPN mpn = INVALID_MPN;
   uint64 startTime;
   Bool waited = FALSE;

   // no wait 
   if (msTimeout == 0) {
      return MemMapAllocPages(world, ppn, 1, nodeMask, color, type,
                              MM_ADVISORY_NONE);
   }

   startTime = Timer_SysUptime();
   mpn = MemMapAllocPages(world, ppn, 1, nodeMask, color, type,
                          MM_ADVISORY_NONE);
   while ((mpn == INVALID_MPN)
          && (Timer_SysUptime() < (startTime + msTimeout))) {

      waited = TRUE;

      // prematurely terminate wait if necessary
      if (world != NULL && 
          (Alloc_AllocInfo(world)->startingCheckpoint || world->deathPending)) {
         break;
      }
      
      // wait for memory to free up
      CpuSched_Sleep(1);
      mpn = MemMapAllocPages(world, ppn, 1, nodeMask, color, 
                             type, MM_ADVISORY_NONE);
   }

   MEMMAP_DEBUG_ONLY( if (waited) { 
      LOG(5, "%d waited %"FMT64"d ms (%s).",
          world == NULL ? -1 : world->worldID, 
          Timer_SysUptime() - startTime, 
          (mpn == INVALID_MPN) ? "failed" : "success");
   })

   return mpn;
}

/*
 *----------------------------------------------------------------------
 *
 * MemMap_AllocKernelPages --
 *
 *      Allocates the requested number of machine pages to be used
 *    by the vmkernel. 
 *
 *    Note: pages are always aligned at the 
 *          specified size i.e (numPages * PAGE_SIZE).
 *
 * Results:
 *      Returns the first mpn of the 'numPages' contiguous MPNs.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
MPN
MemMap_AllocKernelPages(uint32 numPages, 
                        MM_NodeMask nodeMask, 
                        MM_Color color, 
                        MM_AllocType type)
{
   return MemMapAllocPages(NULL, INVALID_PPN, numPages, nodeMask, 
                           color, type, MM_ADVISORY_NONE);
}

/*
 *----------------------------------------------------------------------
 *
 * MemMap_NiceAllocKernelPages --
 *
 *      Allocates the requested number of machine pages to be used
 *    by the vmkernel, only if we are not in a memory crunch, i.e be NICE
 *
 *    Note: pages are always aligned at the 
 *          specified size i.e (numPages * PAGE_SIZE).
 *
 * Results:
 *      Returns the first mpn of the 'numPages' contiguous MPNs.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
MPN
MemMap_NiceAllocKernelPages(uint32 numPages, 
                            MM_NodeMask nodeMask, 
                            MM_Color color, 
                            MM_AllocType type)
{
   return MemMapAllocPages(NULL, INVALID_PPN, numPages, nodeMask, 
                           color, type, MM_ADVISORY_NICE);
}


/*
 *----------------------------------------------------------------------
 *
 * MemMap_AllocKernelLargePage --
 *
 *      Allocates a large (2M) page.
 *
 * Results:
 *      Returns a large page.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
MPN
MemMap_AllocKernelLargePage(MM_NodeMask nodeMask, 
                            MM_Color color, 
                            MM_AllocType type)
{
   uint32 numPages = VM_PAE_LARGE_2_SMALL_PAGES;
   return MemMapAllocPages(NULL, INVALID_PPN, numPages, nodeMask, 
                           color, type, MM_ADVISORY_NONE);
}

/*
 *----------------------------------------------------------------------
 *
 * MemMap_NiceAllocKernelLargePage --
 *
 *      Allocates a large (2M) page.
 *      only if we are not in a memory crunch, i.e be NICE
 *
 * Results:
 *      Returns a large page.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
MPN
MemMap_NiceAllocKernelLargePage(MM_NodeMask nodeMask, 
                                MM_Color color, 
                                MM_AllocType type)
{
   uint32 numPages = VM_PAE_LARGE_2_SMALL_PAGES;
   return MemMapAllocPages(NULL, INVALID_PPN, numPages, nodeMask, 
                           color, type, MM_ADVISORY_NICE);
}


/*
 *----------------------------------------------------------------------
 *
 * MemMap_AllocKernelPage --
 *
 *      Allocate a machine page to be used by the kernel.
 *
 * Results:
 *      Returns the mpn allocated, or INVALID_MPN if unable.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
MPN
MemMap_AllocKernelPage(MM_NodeMask nodeMask, MM_Color color, MM_AllocType type)
{
   return MemMapAllocPages(NULL, INVALID_PPN, 1, nodeMask, color, 
                           type, MM_ADVISORY_NONE);
}

/*
 *----------------------------------------------------------------------
 *
 * MemMap_NiceAllocKernelPage --
 *
 *      Allocate a machine page to be used by the kernel 
 *      only if we are not in a memory crunch, i.e be NICE
 *
 * Results:
 *      Returns the mpn allocated, or INVALID_MPN if unable.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
MPN
MemMap_NiceAllocKernelPage(MM_NodeMask nodeMask, MM_Color color, 
                           MM_AllocType type)
{
   return MemMapAllocPages(NULL, INVALID_PPN, 1, nodeMask, color, 
                           type, MM_ADVISORY_NICE);
}

/*
 *----------------------------------------------------------------------
 *
 * MemMap_AllocKernelPageWait --
 *
 *      This function attempts to allocate kernel memory within constraints
 *      (nodeMask, color, type) every millisecond until msTimeout has
 *      elapsed, or when a world death or checkpoint is pending.
 *      This is a potentially blocking call.
 *
 * Results:
 *      Returns the mpn allocated, or INVALID_MPN if msTimeout has elapsed,
 *      an early exit condition reached, or the constraints can't be met.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
MPN
MemMap_AllocKernelPageWait(MM_NodeMask nodeMask, MM_Color color, MM_AllocType type,
                           uint32 msTimeout)
{
   return MemMapAllocPageWait(NULL, INVALID_PPN, nodeMask, color, type, msTimeout); 
}

/*
 *----------------------------------------------------------------------
 *
 * MemMap_FreeKernelPages --
 *
 *      Free previously allocated machine pages.
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
MemMap_FreeKernelPages(MPN firstMPN)
{
   MemMapFreePages(firstMPN, TRUE);
}


/*
 *----------------------------------------------------------------------
 *
 * MemMap_AllocVMPage --
 *
 *      Allocate a page that will appear at the given ppn in the
 *      VM.
 *
 * Results:
 *      mpn allocated by MemMapAllocPages() (could be INVALID_MPN).
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
MPN
MemMap_AllocVMPage(World_Handle *world, PPN ppn,
                   MM_NodeMask nodeMask, MM_Color color, MM_AllocType type)
{
   MPN mpn;
   mpn = MemMapAllocPages(world, ppn, 1, nodeMask, color, 
                          type, MM_ADVISORY_NONE);

#ifdef VMX86_DEBUG
   if (MemSched_MemoryIsLow()) {
      MemSched_IncLowStateMPNAllocated(world, ppn == INVALID_PPN);
   }
#endif

   return mpn;
}

/*
 *----------------------------------------------------------------------
 *
 * MemMap_AllocVMPageWait --
 *
 *      This function attempts to allocate a VM page within constraints
 *      (nodeMask, color, type) every second until msTimeout has elapsed, or
 *      when a world death or checkpoint is pending.
 *      This is a potentially blocking call.
 *
 * Results:
 *      Returns the mpn allocated, or INVALID_MPN if msTimeout has elapsed,
 *      an early exit condition reached, or the constraints can't be met.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
MPN
MemMap_AllocVMPageWait(World_Handle *world, PPN ppn,
                       MM_NodeMask nodeMask, MM_Color color, MM_AllocType type,
                       uint32 msTimeout)
{
   ASSERT (world != NULL);
   return MemMapAllocPageWait(world, ppn, nodeMask, color, type, msTimeout); 
}


/*
 *----------------------------------------------------------------------
 *
 * MemMap_AllocUserWorldPage --
 *
 *      Allocate a machine page to be used by the given UserWorld
 * 	within the given constraints (nodeMask, color, type).  If no
 *	pages are immediately available, return INVALID_MPN.
 *
 * Results:
 *      Returns the mpn allocated, or INVALID_MPN if the constraints
 * 	can't be met.
 *
 * Side effects:
 *      A machine page is allocated
 *
 *----------------------------------------------------------------------
 */
MPN
MemMap_AllocUserWorldPage(World_Handle* world,
                          MM_NodeMask nodeMask,
                          MM_Color color,
                          MM_AllocType type)
{
   ASSERT(World_IsUSERWorld(world));
   return MemMapAllocPages(world, INVALID_PPN, 1, 
                          nodeMask, color, type, MM_ADVISORY_NONE);
}


/*
 *----------------------------------------------------------------------
 *
 * MemMap_AllocUserWorldPageWait --
 *
 *      Allocate a machine page to be used by the given UserWorld
 * 	within the given constraints (nodeMask, color, type).  If no
 *	pages are immediately available, wait for up to msTimeout 
 * 	milliseconds have elapsed (or a world death or a checkpoint
 *	pending, etc) before giving up and failing. This is a
 *  	potentially blocking call.
 *
 * Results:
 *      Returns the mpn allocated, or INVALID_MPN if msTimeout has
 *	elapsed, an early exit condition reached, or the constraints
 * 	can't be met.
 *
 * Side effects:
 *      A machine page is allocated
 *
 *----------------------------------------------------------------------
 */
MPN
MemMap_AllocUserWorldPageWait(World_Handle* world,
                              MM_NodeMask nodeMask,
                              MM_Color color,
                              MM_AllocType type,
                              uint32 msTimeout)
{
   ASSERT(World_IsUSERWorld(world));
   return MemMapAllocPageWait(world, INVALID_PPN,
                              nodeMask, color, type,
                              msTimeout);
}


/*
 *----------------------------------------------------------------------
 *
 * MemMap_AllocPageRange --
 *
 *      Allocate a range of machine pages containing *startMPN.
 *      If successful, a block of MPNs are allocated. The size
 *      of the allocated block depends on the free memory block
 *      size in the buddy list.
 *
 * Results:
 *      On success, *startMPN is the starting MPN of the allocated
 *      block; *numPages is the number of pages in the block.
 *      
 *      On failure, *startMPN is the next possible free MPN. If the
 *      input *startMPN is larger than all MPN ranges, the output
 *      *startMPN is the smallest MPN of all MPN ranges.
 *
 * Side effects:
 *      A block of machine pages is allocated
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
MemMap_AllocPageRange(World_Handle *world,      // IN
                      MPN *startMPN,            // IN/OUT
                      uint32 *numPages)         // OUT
{
   uint32 i;
   VMK_ReturnStatus status = VMK_FAILURE;
   MemMapInfo *mm = &memMap;
   Buddy_Handle handle;
   MPN mpn = *startMPN;
   MPN nextMPN, minMPN;

   *numPages = 0;

   SP_Lock(&mm->hotMemAddLock);
   for (i = 0; i < MAX_VMNIX_MEM_RANGES; i++) {
      if (availMemRange[i].startMPN == 0) {
         continue;
      }
      if (mpn >= availMemRange[i].startMPN &&
          mpn <= availMemRange[i].endMPN) {
         status = VMK_OK;
         break;
      }
   }
   SP_Unlock(&mm->hotMemAddLock);

   if (status == VMK_OK) {
      handle = MemMapMPN2BuddyHandle(mm, *startMPN);
      status = Buddy_AllocRange(handle, startMPN, numPages);

      if (status == VMK_OK) {
         uint32 numMPNs = *numPages;

         if (mm->numFreePages - numMPNs < mm->reservedLowPages) {
            // if low memory, free memory and return error.
            Buddy_Free(handle, *startMPN);
            *numPages = 0;
            status = VMK_NO_MEMORY;
         } else {
            NUMA_Node node = NUMA_MPN2NodeNum(*startMPN);
            SP_IRQL prevIRQL = SP_LockIRQ(&mm->lock, SP_IRQL_KERNEL);
            MemMapDecFreePages(node, numMPNs, IsLowMPN(*startMPN), FALSE);
            SP_UnlockIRQ(&mm->lock, prevIRQL);
         }
         return status;
      } else if (*startMPN != mpn) {
         // next MPN has been assigned by buddy
         return status;
      }
   }

   ASSERT(status != VMK_OK);

   /*
    * Search for the next available nextMPN > mpn.
    * If non available, set nextMPN to be the minimum MPN.
    */
   nextMPN = INVALID_MPN;
   minMPN = INVALID_MPN;

   SP_Lock(&mm->hotMemAddLock);
   for (i = 0; i < MAX_VMNIX_MEM_RANGES; i++) {
      MPN rangeStartMPN = availMemRange[i].startMPN;
      if (rangeStartMPN == 0) {
         continue;
      }
      if (rangeStartMPN > mpn &&
          (nextMPN == INVALID_MPN || nextMPN > rangeStartMPN)) {
         nextMPN = rangeStartMPN;
      }
      if (minMPN == INVALID_MPN || minMPN > rangeStartMPN) {
         minMPN = rangeStartMPN;
      }
   }
   SP_Unlock(&mm->hotMemAddLock);

   *startMPN = ((nextMPN != INVALID_MPN) ? nextMPN : minMPN);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * MemMap_FreePageRange --
 *
 *      Free a range of pages.
 *
 * Results:
 *      number of pages freed
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
uint32
MemMap_FreePageRange(MPN startMPN, uint32 numPages)
{
   uint32 numMPNs = MemMapFreePages(startMPN, FALSE);
   ASSERT(numMPNs == numPages);
   return numMPNs;
}


/*
 *----------------------------------------------------------------------
 *
 * MemMap_FreeVMPage --
 *
 *      Free the page for the VM.
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
MemMap_FreeVMPage(World_Handle *world, MPN mpn)
{
   uint32 numMPNs;
   DEBUG_ONLY(NUMA_Node node = NUMA_MPN2NodeNum(mpn);)
   ASSERT(node != INVALID_NUMANODE);
   numMPNs = MemMapFreePages(mpn, FALSE);
   ASSERT(numMPNs == 1);

#ifdef VMX86_DEBUG
   if (MemSched_MemoryIsLow()) {
      MemSched_IncLowStateMPNReleased(world, numMPNs);
   }
#endif
}

/*
 *----------------------------------------------------------------------
 *
 * MemMap_FreeUserWorldPage --
 *
 *      Free the page for the userworld
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
MemMap_FreeUserWorldPage(MPN mpn)
{
   MemMapFreePages(mpn, FALSE);
}


#ifdef VMX86_DEBUG

#define MEMMAP_FREE_PAGE_SLACK        (4)
#define MEMMAP_FREE_PAGE_SLACK_ASSERT (16)

/*
 *----------------------------------------------------------------------
 * MemMapCheckNoPages() --
 *   This function checks that when PolicyDefault() returns NO_PAGES, it is indeed
 *   because no pages could be found to meet the constraints specified.  Between the
 *   time the Policy function returned NO_PAGES and this function was called, pages
 *   could have been freed. Thus, a tolerance, or delta, is used in the comparisons;
 *   it is based on the difference in system-wide free pages.
 *
 *   In order for the unsigned comparisons to work, the right hand side (ie
 *    reservedLowPages + Delta) must not overflow.
 *
 *   The constant MEMMAP_FREE_PAGE_SLACK accounts for any remaining discrepancies
 *   or free pages that could have been inserted, such as when the page statistics in
 *   a color have been updated but the summary counters in node or memMap
 *   haven't. Note that while we hold the mm->lock, no FreePage operations will finish.
 *
 * Side Effects:
 *   Will ASSERT if the FREE_PAGE_SLACK_ASSERT delta is broken.
 *   Otherwise print a Warning if delta of FREE_PAGE_SLACK is broken.
 *----------------------------------------------------------------------
 */
static void
MemMapCheckNoPages(const PolicyInput *pIn, PolicyOutput *pOut)
{
   int i;
   uint32 mask = pIn->nodeMask;
   MemMapInfo *mm = &memMap;
   uint32 deltaW = MEMMAP_FREE_PAGE_SLACK;
   uint32 deltaA = MEMMAP_FREE_PAGE_SLACK_ASSERT;
   SP_IRQL prevIRQL;
   
   // skip checking for large page requests.
   if (pIn->numMPNs > 1) {
      return;
   }
   
   prevIRQL = SP_LockIRQ(&mm->lock, SP_IRQL_KERNEL);

   // if pages have been added, account for them
   if (mm->numFreePages > pOut->lastNumFreePages) {
      deltaW += (mm->numFreePages - pOut->lastNumFreePages);
      deltaA += (mm->numFreePages - pOut->lastNumFreePages);
   }

   switch(pIn->type) {
   case MM_TYPE_HIGH:
      mask &= mm->freeHighNodes;
      for (i=0; mask; i++, mask>>=1) {
	 if (mask & 0x01) {
            uint32 freeHiPages = MemMapNumFreePages(mm->node[i].buddyLow, 
                                                    pIn->color);
            if (freeHiPages > deltaW) {
               Warning("[hi]NO_PAGES returned for node=%d color=%d, "
                       "but freeHiPages=%u, "
                       "numFreePages=%u, numFreeLowPages=%u",
                       i, pIn->color, freeHiPages,
                       mm->node[i].numFreePages, mm->node[i].numFreeLowPages);
               MemMapDumpState(pIn, 0);
            }
            ASSERT(freeHiPages < deltaA);

	    if (pIn->color == MM_COLOR_ANY) {
	       ASSERT(mm->node[i].numFreePages <= (mm->node[i].numFreeLowPages
		      + deltaA) );
	    }
	 }
      }
      break;
   case MM_TYPE_LOW:
      mask &= mm->freeLowNodes;
      for (i=0; mask; i++, mask>>=1) {
	 if (mask & 0x01) {
            // for explanation see notes in MM_TYPE_ANY
            if (mm->node[i].buddyLow) {
               if (mm->node[i].numFreeLowPages > (mm->node[i].reservedLowPages
                                                  + deltaW)) {
                  uint32 freePages = MemMapNumFreePages(mm->node[i].buddyLow,
                                                        pIn->color);
                  if (pIn->color == MM_COLOR_ANY) {
                     Warning("[lo]NO_PAGES in node %d ANY color, but "
                             "freePages=%u, numFreeLowPages=%u > "
                             "reservedLowPages=%u", i, freePages,
                             mm->node[i].numFreeLowPages, mm->node[i].reservedLowPages);
                     MemMapDumpState(pIn, 0);
                  } else if (freePages > deltaW) {
                     Warning("[lo]NO_PAGES returned in node=%d color=%d, but "
                             "# low pages = %u",
                             i, pIn->color, freePages);
                     MemMapDumpState(pIn, 0);
                  }
               }
               if (mm->node[i].numFreeLowPages > (mm->node[i].reservedLowPages
                                                  + deltaA)) {
                  ASSERT(pIn->color != MM_COLOR_ANY);
                  ASSERT(MemMapNumFreePages(mm->node[i].buddyLow, pIn->color)
                         < deltaA);
               }
            }     // if buddyLow
	 }
      }
      break;
   case MM_TYPE_LOWRESERVED:
      mask &= mm->freeLowNodes;
      for (i=0; mask; i++, mask>>=1) {
	 if (mask & 0x01) {
            uint32 freePages = MemMapNumFreePages(mm->node[i].buddyLow, pIn->color);
            if (freePages > deltaW) {
               Warning("[lowreserved] NO_PAGES returned for node=%d color=%d, but "
                       "NumFreeListPages(buddyLow)=%u, numFreeLowPages=%u",
                       i, pIn->color,
                       freePages, mm->node[i].numFreeLowPages);
               MemMapDumpState(pIn, 0);
            }
            ASSERT(freePages < deltaA);
	    if (pIn->color == MM_COLOR_ANY) {
	       ASSERT(mm->node[i].numFreeLowPages < deltaA);
	    }
	 }
      }
      break;

      /* A chosen type of MM_TYPE_ANY means the recommended type by
       * PolicyLowHigh() must have been overriden due to a lack of
       * free pages.
       */
   case MM_TYPE_ANY:
      for (i=0; i<mm->numNodes; i++) {
	 if (mask & (1 << i)) {
            //  This node has HIGH mem only, #freeHighPages==0
            //  This node has HIGH mem only, #freeHighPages>0 but not in pIn->color
            uint32 freeHiPages = MemMapNumFreePages(mm->node[i].buddyHigh,
                                                    pIn->color);
            if (freeHiPages > deltaW) {
               Warning("[any]NO_PAGES returned for node=%d color=%d, but freeHiPages=%u, "
                       "numFreePages=%u, numFreeLowPages=%u",
                       i, pIn->color, freeHiPages,
                       mm->node[i].numFreePages, mm->node[i].numFreeLowPages);
               MemMapDumpState(pIn, 0);
            }
            ASSERT(freeHiPages < deltaA);

            //  This node has only LOW mem, and #freeLowPages < #reservedPages, color=ANY
            //  This node has only LOW mem and #freeLowPages > #reservedPages
            //      but not in pIn->color
            //                                  |           |       color != ANY
            //                                  | color=ANY | colorPages=0 | colorPages>0
            //   #freeLowPages < #reservedPages |     True  |    True      |   True
            //   #freeLowPages > #reservedPages |    False  |    True      |  False
            //
            //  This node has both L and H, it's a combo of the above two schemes
            if (mm->node[i].buddyLow) {
               if (mm->node[i].numFreeLowPages > (mm->node[i].reservedLowPages
                                                  + deltaW)) {
                  uint32 freePages = MemMapNumFreePages(mm->node[i].buddyLow,
                                                        pIn->color);
                  if (pIn->color == MM_COLOR_ANY) {
                     Warning("[any]NO_PAGES in node %d ANY color, but "
                             "freePages=%u, numFreeLowPages=%u > "
                             "reservedLowPages=%u", i, freePages,
                             mm->node[i].numFreeLowPages, mm->node[i].reservedLowPages);
                     MemMapDumpState(pIn, 0);
                  } else if (freePages > deltaW) {
                     Warning("[any]NO_PAGES returned in node=%d color=%d, but "
                             "# low pages = %u",
                             i, pIn->color, freePages);
                     MemMapDumpState(pIn, 0);
                  }
               }
               if (mm->node[i].numFreeLowPages > (mm->node[i].reservedLowPages
                                                  + deltaA)) {
                  ASSERT(pIn->color != MM_COLOR_ANY);
                  ASSERT(MemMapNumFreePages(mm->node[i].buddyLow, pIn->color) < deltaA);
               }
            }     // if buddyLow

	 }
      }   // for each node
   }     // switch type

   SP_UnlockIRQ(&mm->lock, prevIRQL);
}

#endif   // VMX86_DEBUG

/*
 *----------------------------------------------------------------------
 *
 * MemMap_EarlyAllocPage --
 *
 *    During early init, memmap's internal mpn managing structures havent
 *    been initialized as yet, so get the required MPNs directly from the
 *    available ranges.
 *
 * Results:
 *      Returns the mpn allocated.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
MPN
MemMap_EarlyAllocPage(MM_AllocType type)
{
   MemMapInfo *mm = &memMap;
   uint32 i, j;
   MPN mpn;
   const Bool low = (type == MM_TYPE_LOW || type == MM_TYPE_LOWRESERVED) ?
                     TRUE : FALSE;
   ASSERT(!mm->memMapInitCalled);
   for (i = 0; i < mm->numNodes; i++) {
      MemMapNodeAvailRange *avail = &nodeAvailRange[i];
      if (nodeAvailRange[i].numPages <= 0) {
         continue;
      }
      ASSERT(avail->numRanges <= MEMMAP_MAX_NODE_AVAIL_RANGES);
      for (j = 0; j < avail->numRanges; j++) {
         NUMA_MemRange *nodeRange = &avail->nodeRange[j];
         for (mpn = nodeRange->startMPN; 
              mpn <= nodeRange->endMPN; mpn++) {
            if (low && !IsLowMPN(mpn)) {
               break;
            }
            ASSERT(MTRR_IsWBCachedMPN(mpn));
            nodeRange->startMPN = mpn + 1;
            avail->numPages--;
            // track the early allocations, all early allocations are made by
            // the kernel 
            biosMemMapStats.numKernelUse++;
            return mpn;
         }
      }
   }
   ASSERT_NOT_IMPLEMENTED(0);
}


/*
 *----------------------------------------------------------------------
 *
 * MemMapAllocPages --
 *
 *      Allocate the requested number of pages for the 
 *      given world (vmkernel if world == NULL).
 *      Statistics are updated and a memory warning is issued if needed.
 *      The internal policy function decides on node/color/memtype.
 *      By default:
 *      ppn is used for selecting the proper pagecolor.
 *      nodeMask allows caller to limit page placement to specific nodes.
 *      color allows caller to specify desired page color or ANY.
 *      type indicates if the page needs to be allocated below 4GB.
 *
 * Results:
 *      Returns the mpn allocated.
 *      INVALID_MPN if out of memory or unable to satisfy the constraints.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static MPN
MemMapAllocPages(World_Handle *world, PPN ppn, uint32 numMPNs, 
		 MM_NodeMask nodeMask, MM_Color color, MM_AllocType type,
                 uint32 flags)
{
   MemMapInfo *mm = &memMap;
   PolicyInput pIn;
   PolicyOutput pOut;
   PolicyReturnCode status;
   uint32 typeRetry = 0;
   uint32 affRetry = 0;

   ASSERT(nodeMask != 0);

   // if we are in the early init part, get early pages
   if (vmkernelInEarlyInit) {
      MPN rtnMPN;
      ASSERT(numMPNs == 1);
      rtnMPN = MemMap_EarlyAllocPage(type);
      ASSERT(rtnMPN <= lastValidMPN);
      return rtnMPN;
   }

   // when we are adviced to be nice, allocate pages only if no memory crunch
   if ((flags & MM_ADVISORY_NICE) && MemSched_MemoryIsLow()) {
      return INVALID_MPN;
   }

   MEMMAP_DEBUG_ONLY(memmapPageAllocated = TRUE);

   pIn.world = world;
   pIn.ppn = ppn;
   pIn.nodeMask = nodeMask;
   pIn.color = color;
   pIn.type = (type == MM_TYPE_ANY) ? MemMapPolicyLowHigh(mm) : type;
   pIn.numMPNs = numMPNs;
   pOut.colorNodeLookups = 0;

   // First try finding a free page with node affinity enabled and
   // use PolicyLowHigh() recommendation
   pIn.useAffinity = TRUE;
   status = MemMapPolicyDefault(&pIn, &pOut);

   // If above failed, try MM_TYPE_ANY instead of recommendation
   // - exception: if PolicyLowHigh recommended MM_TYPE_ANY
   if (status != POLICY_OK && type != pIn.type) {
      pIn.type = MM_TYPE_ANY;
      status = MemMapPolicyDefault(&pIn, &pOut);
      typeRetry++;
   }

   // If above fails, try again without node affinity
   // and use default policy here for widest possible search
   if (status != POLICY_OK  && mm->numNodes > 1) {
      pIn.useAffinity = FALSE;
      status = MemMapPolicyDefault(&pIn, &pOut);
      affRetry++;
   }

   /*
    * handle conflicts.  These indicate that the given constraints are 
    * unreasonable - no pages of specified type in specified nodes.
    * Note that a NODE_MASK_CONFLICT is not possible here, those only
    * happen when the node mask disagrees with node affinity, which we
    * turn off on the second call to the policy function.
    */
   ASSERT(status != POLICY_NODE_MASK_CONFLICT);

   // bump page counters atomically
   // this method of locking then bumping en mass saves cycles
   // over using individual Atomic_Inc/Dec's 
   if (status == POLICY_OK) {
      SP_IRQL prevIRQL = SP_LockIRQ(&mm->lock, SP_IRQL_KERNEL);

      MemMapDecFreePages(pOut.node, numMPNs, IsLowMPN(pOut.mpn), IsKernelPage(&pIn));

      mm->totalGoodAllocs++;
      mm->totalTypeRetries += typeRetry;
      mm->totalAffRetries  += affRetry;
      mm->totalColorNodeLookups += pOut.colorNodeLookups;

      MemSched_UpdateFreePages(MemMapUnusedPages(mm));

      SP_UnlockIRQ(&mm->lock, prevIRQL);

      ASSERT(pOut.mpn >= mm->start);
      ASSERT(pOut.mpn <= lastValidMPN);

      if (MemMap_MPN2Color(pOut.mpn) == 0) {
         LOG(1, "alloc color 0, mpn=0x%x, ppn=0x%x, nummpn=%u, wantColor=%u",
             pOut.mpn, ppn, numMPNs, color);
      }
      return pOut.mpn;

   } else {
      // Page allocation failure.
      // Update stats
      SP_IRQL prevIRQL = SP_LockIRQ(&mm->lock, SP_IRQL_KERNEL);

      mm->totalBadAllocs++;
      mm->totalTypeRetries += typeRetry;
      mm->totalAffRetries  += affRetry;
      mm->totalColorNodeLookups += pOut.colorNodeLookups;

      SP_UnlockIRQ(&mm->lock, prevIRQL);

      LOG(1, "vm %d: Constraints cannot be met",
          (world == NULL ? 0 : world->worldID));
      DEBUG_ONLY({ MemMapDumpState(&pIn, 1); })
      MemMap_LogState(1);

      DEBUG_ONLY( MemMapCheckNoPages(&pIn, &pOut); )

      // warn out of memory
      if (mm->numFreePages < mm->reservedLowPages) {
	 MemMapWarnNoMemory();
	 MEMMAP_DEBUG_ONLY( {
	    MemMapDumpState(&pIn, 0);
	    MemMapLogFreePages();
	 })
      }
      return INVALID_MPN;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * MemMapFreePages --
 *
 *      Free a previously allocated large machine page(s).
 *
 * Results:
 *      returns the number of pages released.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static uint32
MemMapFreePages(MPN mpn, Bool isKernel)
{
   MemMapInfo *mm = &memMap;
   SP_IRQL prevIRQL;
   NUMA_Node node = NUMA_MPN2NodeNum(mpn);
   uint32 numMPNs = 0;
   Buddy_Handle handle;

   // We dont expect any Free's during early init
   ASSERT(!vmkernelInEarlyInit);
   // We dont expect mpn's allocated during early init to be freed.
   ASSERT(mpn >= mm->start);
      
   // sanity check
   ASSERT(mpn != INVALID_MPN);
   ASSERT(node != INVALID_NUMANODE);
   ASSERT(mpn <= lastValidMPN);

   LOG(5, "Freeing %s node %d mpn %x ", (isKernel) ? "kernel" : "", node, mpn);

   handle = MemMapMPN2BuddyHandle(mm, mpn);
   numMPNs = Buddy_GetLocSize(handle, mpn);
   ASSERT(numMPNs > 0);
   MemMap_SetIOProtectionRange(MPN_2_MA(mpn), PAGES_2_BYTES(numMPNs), 
                               MMIOPROT_IO_DISABLE);

   Buddy_Free(handle, mpn);

   // Update memMap-level counters atomically
   prevIRQL = SP_LockIRQ(&mm->lock, SP_IRQL_KERNEL);
   MemMapIncFreePages(node, numMPNs, IsLowMPN(mpn), isKernel);
   MemSched_UpdateFreePages(MemMapUnusedPages(mm));
   SP_UnlockIRQ(&mm->lock, prevIRQL);
   return numMPNs;
}

/*
 *----------------------------------------------------------------------
 *
 * MemMap_UnusedPages --
 *
 *      Returns total number of free pages currently available.
 *
 * Results:
 *      Returns total number of free pages currently available.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
uint32
MemMap_UnusedPages(void)
{
   MemMapInfo *mm = &memMap;
   return MemMapUnusedPages(mm);
}


/*
 *----------------------------------------------------------------------
 *
 * MemMap_KernelPages --
 *
 *      Returns total number of kernel pages currently allocated.
 *
 * Results:
 *      Returns total number of kernel pages currently allocated.
 *
 * Side effects:
 *      None.
 *
 *---------------------------------------------------------------------- */
uint32
MemMap_KernelPages(void)
{
   MemMapInfo *mm = &memMap;
   return(mm->numKernelPages);
}


/*
 *----------------------------------------------------------------------
 *
 * MemGetMaxNewVMSize --
 *      Calculates the maximum size in MB for a new VM depending on the
 *      available memory and number of specified vcpus
 *
 * Results:
 *      returns the maximum size in MB for a new VM
 *
 * Side effects:
 *      none
 *
 *----------------------------------------------------------------------
 */
static uint32
MemGetMaxNewVMSize(int numVCPUs, uint32 newMem, uint32 newSwap) 
{
   uint32 overheadMem;
   uint32 newSize, newSizeMB;

   // compute maximum size for new VM
   overheadMem = VMMem_OverheadSize(VMMEM_DEFAULT_OVERHEAD_MB, numVCPUs) / PAGE_SIZE;
   if (newMem < overheadMem) {
      newSize = 0;
   } else {
      newSize = (newMem - overheadMem) + newSwap;
   }
   
   // handle expanded overhead requirements for large VMs
   if (newSize > (uint32) MBToPages(VMMEM_SIZE_MB_FOR_DEFAULT_OVERHEAD)) {
      uint32 newOverheadMem;

      // compute total overhead
      newSizeMB = MIN((uint32) PagesToMB(newSize), VMMEM_MAX_SIZE_MB);
      newOverheadMem = VMMem_OverheadSize(newSizeMB, numVCPUs) / PAGE_SIZE;
      ASSERT(newOverheadMem >= overheadMem);

      // ensure sufficient memory for additional overhead
      if (newMem < newOverheadMem) {
         // conservatively report smaller VM size
         newSize = MBToPages(VMMEM_SIZE_MB_FOR_DEFAULT_OVERHEAD);
      } else {
         // reduce size by additional overhead
         newSize -= (newOverheadMem - overheadMem);
      }
   }

   // limit maximum VM size
   newSizeMB = PagesToMB(newSize);
   newSizeMB = MIN(newSizeMB, VMMEM_MAX_SIZE_MB);

   // restrict VM size to multiple of 4MB
   newSizeMB -= (newSizeMB & 0x3);
   return(newSizeMB);
}

/*
 *----------------------------------------------------------------------
 *
 * MemProcRead() --
 * 	Provide info in /proc file system on amount of machine memory
 * 	available to start up new VMs.
 *
 * Results:
 *      VMK_OK
 *
 * Side Effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
static int
MemProcRead(Proc_Entry *entry, char *buffer, int *len)
{
   MemMapInfo *mm = &memMap;

   int32 availMem, reservedMem, autoMinMem, availSwap, reservedSwap;
   int32 reclaimMem, newMem, newSwap, newOneVCPUSizeMB, newTwoVCPUSizeMB;
   int32 loMem, hiMem, kMem, sumHigh, sumLow;
   uint32 nCOW, nCOW1, nUsed, nHint;
   int heapFreeBytes;
   int i;

   // initialize
   *len = 0;

   // obtain current reserve levels
   MemSched_CheckReserved(&availMem, &reservedMem, &autoMinMem,
                          &availSwap, &reservedSwap);

   // can consume auto-min memory, if sufficient swap
   reclaimMem = MIN(autoMinMem, availSwap);
   newMem = availMem + reclaimMem;
   newSwap = availSwap - reclaimMem;
   // compute maximum size for new 1-vcpu VM
   newOneVCPUSizeMB = MemGetMaxNewVMSize(1, MAX(newMem, 0), MAX(newSwap, 0)); 

   // compute maximum size for new 2-vcpu VM
   newTwoVCPUSizeMB = MemGetMaxNewVMSize(2, MAX(newMem, 0), MAX(newSwap, 0)); 

   // obtain current sharing stats
   PShare_TotalShared(&nCOW, &nCOW1, &nUsed, &nHint);

   // obtain current heap stats
   heapFreeBytes = Mem_Avail();

   // format statistics
   Proc_Printf(buffer, len,
               "Unreserved machine memory: %d Mbytes/%d Mbytes\n"
               "Unreserved swap space: %d Mbytes/%d Mbytes\n"    
               "Reclaimable reserved memory: %d Mbytes\n"
	       "Machine memory free: %d Mbytes/%d Mbytes\n"
               "Shared memory (shared/common): %d Kbytes/%d Kbytes\n"
	       "Maximum new 1-vcpu VM size: %d Mbytes\n"
	       "Maximum new 2-vcpu VM size: %d Mbytes\n"
               "System heap size: %d Kbytes (%Ld bytes)\n"
               "System heap free: %d Kbytes (%d bytes)\n"
               "System map entries free: %d\n"
               "System code size: %d Kbytes\n" 
               "System memory usage: %d Kbytes\n",
               MAX(0, PagesToMB(availMem)),
               PagesToMB(availMem + reservedMem),
               MAX(0, PagesToMB(availSwap)),
               PagesToMB(availSwap + reservedSwap),
               MAX(0, PagesToMB(reclaimMem)),
               PagesToMB(MemMap_UnusedPages()),
               PagesToMB(mm->totalMemPages) +
               PagesToMB(biosMemMapStats.numDiscarded) +
               PagesToMB(biosMemMapStats.numKernelUse),
               PagesToKB(nCOW),
               PagesToKB(nUsed),
	       newOneVCPUSizeMB,
	       newTwoVCPUSizeMB,
               PagesToKB(VMK_NUM_CODEHEAP_PAGES - VMK_NUM_CODE_PAGES), 
               PAGES_2_BYTES(VMK_NUM_CODEHEAP_PAGES - VMK_NUM_CODE_PAGES), 
               heapFreeBytes / 1024,
               heapFreeBytes,
               KVMap_NumEntriesFree(),
               PagesToKB(VMK_NUM_CODE_PAGES),
               PagesToKB(MemMap_KernelPages()) +
                  PagesToKB(biosMemMapStats.numKernelUse));


   // memory status per node:  total free high low reserved kernel
   Proc_Printf(buffer, len, "Node -Total-/MB    -FreeHi/MB    FreeLow/MB   Reserved/MB   "
	       " Kernel/MB\n");
   sumHigh = sumLow = 0;
   for (i=0; i < mm->numNodes; i++) {
      loMem = mm->node[i].numFreeLowPages;
      hiMem = mm->node[i].numFreePages - loMem;
      kMem  = mm->node[i].numKernelPages;
      Proc_Printf(buffer, len, "%2d   %7d/%-5d %7d/%-5d %7d/%-5d %7d/%-4d %7d/%-4d\n", 
		  i,
		  mm->node[i].totalNodePages, PagesToMB(mm->node[i].totalNodePages),
		  hiMem, PagesToMB(hiMem),
		  loMem, PagesToMB(loMem),
		  mm->node[i].reservedLowPages, PagesToMB(mm->node[i].reservedLowPages),
		  kMem, PagesToMB(kMem) );
      sumHigh += hiMem;
      sumLow += loMem;
   }
   Proc_Printf(buffer, len, "TOTALS            %7d/%-5d %7d/%-5d\n",
	       sumHigh, PagesToMB(sumHigh),
	       sumLow,  PagesToMB(sumLow) );

   // everything OK
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * MemDebugProcRead() --
 * 	Provide debugging info in /proc file system on state of memmap module
 * 	and the free pages in each color.
 *
 * Results:
 *      VMK_OK
 *
 * Side Effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
static int
MemDebugProcRead(Proc_Entry *entry,
		 char *buffer,
		 int *len)
{
   int i;
   MM_Color color;
   MemMapInfo *mm = &memMap;
   uint64 intAvgLookups, avgLookups;
   uint32 fracAvgLookups;

   // initialize
   *len = 0;
   avgLookups = MemMapAvgLookups(mm);
   intAvgLookups = avgLookups / 100;
   fracAvgLookups = avgLookups % 100;

   // format table of free pages by color/node

   // header
   Proc_Printf(buffer, len,
               "color   ");
   for (i=0; i < mm->numNodes; i++) {
   	 Proc_Printf(buffer, len, "node%2dH node%2dL ", i, i);
   }
   Proc_Printf(buffer, len, "\n");
   
   // per-color per-node info
   for (color = 0; color < mm->numColors; color++) {
      Proc_Printf(buffer, len, "%5d  ", color);
      for (i=0; i < mm->numNodes; i++) {
	 Proc_Printf(buffer, len, "%7d %7d ", 
		     (mm->node[i].buddyHigh ? 
                      MemMapNumFreePages(mm->node[i].buddyHigh, color) : 0),
		     (mm->node[i].buddyLow ?
                      MemMapNumFreePages(mm->node[i].buddyLow, color) : 0));
      }
      Proc_Printf(buffer, len, "\n");
   }
   
   // format other debugging statistics
   Proc_Printf(buffer, len,
	       "Retried allocs due to lack of mem type: %Lu\n"
               "Retried allocs due to affinity: %Lu\n"
	       "System total good/failed allocs: %Lu/%Lu\n"
               "Total node/color lookups: %Lu\n"
               "Avg # lookups per Policy call: %Lu.%02u\n"
	       "Next kernel color allocated: %u\n"
	       "Next NUMA node allocated   : %u\n",
	       mm->totalTypeRetries,
               mm->totalAffRetries,
	       mm->totalGoodAllocs, mm->totalBadAllocs,
               mm->totalColorNodeLookups,
               intAvgLookups, fracAvgLookups,
	       mm->nextKernelColor,
	       mm->nextNode );

   // everything OK
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * MemDebugProcWrite() --
 * 	for debugging commands only.
 * 	check vmkernel log output for MPN #'s
 * 	it might help to turn off page sharing / migration / remapping
 *     
 * 	alloc 0x<nodeMask> <color> <type>
 * 	free 0x<MPN>
 *
 * Results:
 *      VMK_OK
 *
 * Side Effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
static int
MemDebugProcWrite(Proc_Entry *entry,     // IN: unused in this case
		  char *buffer,          // IN: chars written to proc node
		  int *len)              // IN: # of chars written
{
   MemMapInfo *mm = &memMap;
   char *argv[4];
   int argc;
   uint32 val;

   argc = Parse_Args(buffer, argv, 4);
   if ((argc == 4) && strncmp(argv[0], "alloc", 5) == 0) {
      MM_Color color;
      int type;
      MPN mpn;
      if (Parse_Hex(argv[1], strlen(argv[1]), &val) != VMK_OK) {
	 Log("Invalid nodeMask arg %s", argv[1]);
	 return(VMK_BAD_PARAM);
      }
      if (Parse_Int(argv[2], strlen(argv[2]), &color) != VMK_OK ||
	  (color >= mm->numColors && color < MM_COLOR_ANY)) {
	 Log("Invalid color # '%s'", argv[2]);
	 return(VMK_BAD_PARAM);
      }
      if (Parse_Int(argv[3], strlen(argv[3]), &type) != VMK_OK) {
	 Log("Invalid type # '%s'", argv[3]);
	 return(VMK_BAD_PARAM);
      }
      mpn = MemMap_AllocVMPage(NULL, 0, (MM_NodeMask) val, color, type);
      Log("MemMap_AllocVMPage returned MPN = 0x%08x", mpn);
   } else if ((argc == 2) && strncmp(argv[0], "free", 4) == 0) {
      // get MPN to be freed
      if (Parse_Hex(argv[1], 8, &val) != VMK_OK) {
	 Log("Invalid MPN arg %s", argv[1]);
	 return(VMK_BAD_PARAM);
      }
      MemMap_FreeKernelPage((MPN) val);
   } else if ((argc == 1) && strncmp(argv[0], "kvmap", 5) == 0)  {
      KVMap_DumpEntries();
   } 

   return VMK_OK;
}



uint32
MemMap_ManagedPages(void)
{
   MemMapInfo *mm = &memMap;
   ASSERT(mm->initFreePages > mm->reservedLowPages);
   return mm->initFreePages - mm->reservedLowPages;
}

MPN
MemMap_GetLastValidMPN(void)
{
   return lastValidMPN;
}

uint32
MemMap_GetNumNodes(void)
{
   return memMap.numNodes;
}

#ifdef VMX86_DEBUG
static const char *
MMTypeString(MM_AllocType type)
{
   const char * typestr;

   switch(type) {
   case MM_TYPE_ANY:
      typestr = "ANY";
      break;
   case MM_TYPE_HIGH:
      typestr = "HIGH";
      break;
   case MM_TYPE_LOW:
      typestr = "LOW";
      break;
   case MM_TYPE_LOWRESERVED:
      typestr = "LOW RESERVED";
      break;
   default:
      typestr = "INVALID";
   }
   return typestr;
}

/*
 *----------------------------------------------------------------------
 *
 * MemMapDumpState --
 *
 *      Debugging routine to log the inputs to the policy function:
 *      node/color/type 
 *
 * Results:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
MemMapDumpState(const PolicyInput *s, int level)
{
   char sourceStr[24];
   char nodeStr[11];
   char colorStr[5];

   if (s->world == NULL) {
      snprintf(sourceStr, sizeof sourceStr, "VMkernel");
   } else {
      if (s->ppn != INVALID_PPN) {
	 snprintf(sourceStr, sizeof sourceStr, "world %d  ppn=%x", s->world->worldID, s->ppn);
      } else {
	 snprintf(sourceStr, sizeof sourceStr, "world %d  overhead", s->world->worldID);
      }
   }
   if (s->nodeMask == MM_NODE_ANY) {
      snprintf(nodeStr, sizeof nodeStr, "ANY");
   } else {
      snprintf(nodeStr, sizeof nodeStr,  "0x%x", s->nodeMask);
   }
   if (s->color == MM_COLOR_ANY) {
      snprintf(colorStr, sizeof colorStr, "ANY");
   } else {
      snprintf(colorStr, sizeof colorStr, "%d", s->color);
   }

   LOG(level, "%s: nodeMask=%s color=%s type=%s",
       sourceStr, nodeStr, colorStr, MMTypeString(s->type) );
}


/*
 *----------------------------------------------------------------------
 *
 * MemMapLogFreePages --
 *
 * 	Debugging routine to log the total number of free pages,
 * 	obtained both from the aggregate total value that is
 * 	maintained by the system, and separately by summing the
 *	values across all color free lists.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Generates logging output.
 *
 *----------------------------------------------------------------------
 */
static void
MemMapLogFreePages(void)
{
   MemMapInfo *mm = &memMap;
   MM_Color color;
   int i;
   char line[200];
   int len;

   // header
   len = 0;
   len += snprintf(line+len, sizeof(line)-len, "color   ");
   len = MIN(len, sizeof(line));
   for (i=0; i < mm->numNodes; i++) {
      len += snprintf(line+len, sizeof(line)-len, "node%2dH node%2dL ", i, i);
      len = MIN(len, sizeof(line));
   }
   Log("%s", line);
      
   // per-color per-node info
   for (color = 0; color < mm->numColors; color++) {
      len = 0;
      len += snprintf(line+len, sizeof(line)-len, "%5d  ", color);
      len = MIN(len, sizeof(line));
      for (i=0; i < mm->numNodes; i++) {
	 len += snprintf(line+len, sizeof(line)-len, "%7d %7d ", 
                         (mm->node[i].buddyHigh ? 
                         MemMapNumFreePages(mm->node[i].buddyHigh, color) : 0),
                         (mm->node[i].buddyLow ?
                         MemMapNumFreePages(mm->node[i].buddyLow, color) : 0));
         len = MIN(len, sizeof(line));
      }
      Log("%s", line);
   }
 
   // print out next kernel color & next node
   Log("Next kernel color allocated: %u", mm->nextKernelColor);
   Log("Next NUMA node allocated   : %u", mm->nextNode );

}
#endif /* VMX86_DEBUG */



/*
 *----------------------------------------------------------------------
 *
 * MemMap_LogState --
 *
 * 	Debugging routine to log the free page counters for each node,
 * 	the important nodeMasks and summary information.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Generates logging output.
 *
 *----------------------------------------------------------------------
 */
void
MemMap_LogState(int level)
{
   MemMapInfo *mm = &memMap;
   int i;

   // title
   LOG(level, "Node freeHiLoRes totalPages freePages freeLoPages freeHiPages  reserved    kernel");

   // for each node
   for (i=0; i < mm->numNodes; i++) {
      LOG(level, " %3d     %2u%2u%2u   %9u %9u   %9u   %9u %9u %9u",
          i,
          (mm->freeHighNodes & (1<<i) ? 1 : 0),
          (mm->freeLowNodes & (1<<i) ? 1 : 0),
          (mm->freeResNodes & (1<<i) ? 1 : 0),
          mm->node[i].totalNodePages,
          mm->node[i].numFreePages,
          mm->node[i].numFreeLowPages,
          mm->node[i].numFreePages - mm->node[i].numFreeLowPages,
          mm->node[i].reservedLowPages,
          mm->node[i].numKernelPages);
   }
   // summary
   LOG(level, "Combined ------   %9u %9u   %9u   %9u %9u %9u",
       mm->totalMemPages,
       mm->numFreePages,
       mm->numFreeLowPages,
       mm->numFreePages - mm->numFreeLowPages,
       mm->reservedLowPages,
       mm->numKernelPages);

   // debugging stuff 
   LOG(level, "  AffinityRetries [%Lu] TypeRetries [%Lu]"
       "  Bad Allocs [%Lu]  Avg Lookups [%Lu.%02Lu]",
       mm->totalAffRetries, mm->totalTypeRetries,
       mm->totalBadAllocs, MemMapAvgLookups(mm)/100,
       MemMapAvgLookups(mm) % 100);
}

/*
 * MemMap_AllocDriverPage and MemMap_FreeDriverPage exist so that we 
 * don't have to distribute definitions for MM_AllocType, etc..
 */
MPN
MemMap_AllocDriverPage(Bool lowPage)
{
   if (lowPage) {
      return MemMap_AllocKernelPage(MM_NODE_ANY, MM_COLOR_ANY,
                                    MM_TYPE_LOWRESERVED);
   } else {
      return MemMap_AllocAnyKernelPage();
   }
}
void
MemMap_FreeDriverPage(MPN mpn)
{
   MemMap_FreeKernelPage(mpn);
}


/*
 *----------------------------------------------------------------------
 *
 * MemMap_DefaultColor --
 *
 *      Returns the default color for page allocation.
 *      For VM's, the default color is based on world ID and PPN #.
 *
 * Results:
 *      A page color number.
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
MM_Color
MemMap_DefaultColor(World_Handle *world,
		    PPN ppn)
{
   MM_Color color;
   MemMapInfo *mm = &memMap;

   if (world != NULL) {
      uint32 numColors, offset;
      MemSched_ColorVec *colorList = MemSched_AllowedColors(world);

      // if we have cache color restrictions, use them
      if (colorList != MEMSCHED_COLORS_ALL) {
         numColors = colorList->nColors;
      } else {
         numColors = mm->numColors;
      }

      if (ppn == INVALID_PPN) {
         offset = mm->nextKernelColor % numColors;
      } else {
         offset = (ppn + world->worldID) % numColors;
      }

      if (colorList != MEMSCHED_COLORS_ALL) {
         color = (MM_Color)colorList->colors[offset];
      } else {
         color = offset;
      }
      
      LOG(9, "Color: %d ppn: %x", color, ppn);

      return color;
   }

   color = mm->nextKernelColor;

   return color;
}


/*
 *----------------------------------------------------------------------
 *
 * MemMap_GetNumColors --
 *
 *      Returns the number of colors used by the page allocator.
 *
 * Results:
 *      The number of colors.
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
MM_Color
MemMap_GetNumColors(void)
{
   return memMap.numColors;
}


/*
 *----------------------------------------------------------------------
 *
 * MemMapPolicyDefault --
 *
 *      All policies:
 *      Given constraints, finds the optimal (node, color, type) combo for
 *      the next page allocation using allocation policy algorithm.  Then
 *      the policy function allocates a page from the chosen free list.
 *
 *      This Policy:
 *      Note that this default policy is designed to loop through all
 *      colors and nodes if necessary to satisfy the constraints - it
 *      does a complete search.
 *      - If not specified, existing algorithms are used to
 *        compute the color for VM's (ppn-based) and VMkernel.
 *      - Fail if VM node affinity settings and passed in node mask don't
 *        agree.
 *      - Outer loop to iterate through the colors.  For each color,
 *      iterate through all nodes with the selected memory type till free
 *      page is found.  In other words, color has precedence over node.
 *
 *     LOG 1: To see why OK not returned;
 *
 * Results:
 *      If constraints satisfied: returns POLICY_OK and sets PolicyOutput
 *      to reflect page allocated;
 *      else, returns one of POLICY_* error codes
 *
 * Side effects:
 *      May update policy state variables (nextNode, nextKernelColor)
 *
 *----------------------------------------------------------------------
 */
static PolicyReturnCode
MemMapPolicyDefault(const PolicyInput *s, PolicyOutput *o)
{
   MemMapInfo *mm = &memMap;
   MM_AllocType recType;              // the recommended mem type
   MM_NodeMask  recMask;              // mask of nodes to search
   MM_NodeMask  affMask;              // VM or kernel affinity mask
   MM_Color     color, initColor;
   int          i, n;
   NUMA_Node    node;
   Bool         allocated = FALSE;

   recType = s->type;
   o->mpn = INVALID_MPN;

   // TYPE:  if specific memory type requested, limit mask to
   //        those nodes with that type of mem
   if (recType == MM_TYPE_ANY) {
      recMask = MM_NODE_ANY;
   } else if (recType == MM_TYPE_HIGH) {
      recMask = mm->freeHighNodes;
   } else if (recType == MM_TYPE_LOW) {
      recMask = mm->freeLowNodes;
   } else {
      recMask = mm->freeResNodes;
   }

   // Get Node Affinity if its enabled
   if (s->useAffinity &&
      (IsVMPhysicalPage(s) || IsVMOverheadPage(s))) {
      ASSERT(s->world != NULL);
      affMask = MemSched_NodeAffinityMask(s->world);
      if ((affMask & mm->validNodes) == 0) {
         // override affinity mask if its invalid
	 affMask = MM_NODE_ANY;
      }
   } else {
      affMask = MM_NODE_ANY;
   }

   DEBUG_ONLY( {
      LOG(6, "recType=%s recMask=0x%x affMask=0x%x",
	  MMTypeString(recType), recMask, affMask);
      MemMapDumpState(s, 6);
   })

   if (s->color != MM_COLOR_ANY) {
      ASSERT(s->color < mm->numColors);
   }

   // Fail if affinity and passed in node mask conflict
   // If useAffinity == false, don't fail, use node mask
   if ((affMask & s->nodeMask) != 0) {
      affMask &= s->nodeMask;
   } else {
      ASSERT(s->useAffinity == TRUE);
      LOG(1, "Affinity mask %x and passed in node mask %x don't agree",
	      affMask, s->nodeMask);
      return POLICY_NODE_MASK_CONFLICT;
   }

   // Resolve conflicts between node mask and type
   //       -If node mask and specific type don't agree, return failure
   //       -we could also be out of pages in requested type
   if ((affMask & recMask) == 0) {
      LOG(1, "Node/Affinity mask 0x%x disagrees with type mask 0x%x, type=%d",
          affMask, recMask, s->type);
      o->type = recType;
      o->lastNumFreePages = mm->numFreePages;
      return POLICY_NO_PAGES;
   } else {
      recMask &= affMask;
   }

   ASSERT(recMask != 0);

   DEBUG_ONLY(LOG(6, "final recType=%s recMask=0x%x affMask=0x%x",
		  MMTypeString(recType), recMask, affMask));

   // COLOR: Pick a starting color based on VM PPN rule or next
   //        kernel color, or use specific color parm
   if (s->color == MM_COLOR_ANY) {
      initColor = MemMap_DefaultColor(s->world, s->ppn);
   } else {
      initColor = s->color;
   }
   n = 0;

   // Iterate through freelists, colors to find a free page
   while (1) {
      color = (initColor + n) % mm->numColors;

      // Optimizations:
      // 1) if recMask & colorNodeMask[] == 0, skip this color

      // For each color, cycle through the nodes selected in the Mask
      node = mm->nextNode;
      for (i = 0; i < mm->numNodes; i++) {
         VMK_ReturnStatus status;
	 node = (mm->nextNode + i) % mm->numNodes;

	 if (recMask & (1 << node)) {
            o->colorNodeLookups++;

	    // For each node, check that the desired memory type is available.
	    // Maintain old policy: if type constraint is ANY, and policy
	    // recommends high page but not avail., try low page instead
	    if ((recType == MM_TYPE_HIGH || recType == MM_TYPE_ANY) &&
		mm->node[node].buddyHigh) {
               void *ra = __builtin_return_address(2);
               const World_ID wid = PRDA_GetRunningWorldIDSafe();
               status = Buddy_AllocateColor(mm->node[node].buddyHigh, 
                                            s->numMPNs, color, wid, ra, 
                                            &o->mpn);
               if (status == VMK_OK) {
                  // got high page
                  LOG(6, "Allocated node %d, color %d, HIGH mpn = 0x%x",
                      node, color, o->mpn);
                  o->type = MM_TYPE_HIGH;
                  allocated = TRUE;
               }
	    }
            if (!allocated && (recType != MM_TYPE_HIGH) &&
                mm->node[node].buddyLow) {
               void *ra = __builtin_return_address(2);
               const World_ID wid = PRDA_GetRunningWorldIDSafe();

	       // Keep low reserved pages strictly for reserved use.
	       if (recType != MM_TYPE_LOWRESERVED && 
		   (mm->node[node].numFreeLowPages <=
		    mm->node[node].reservedLowPages)) {
		  continue;
	       }

               status = Buddy_AllocateColor(mm->node[node].buddyLow, 
                                            s->numMPNs, color, wid, ra, 
                                            &o->mpn);
               if (status != VMK_OK) {
                  continue;
               }

	       // got low page
	       LOG(6, "Allocated node %d, color %d, LOW mpn = 0x%x",
                   node, color, o->mpn);
	       o->type = (recType == MM_TYPE_ANY) ? MM_TYPE_LOW : recType;	       
	       allocated = TRUE;
	    }

	    if (allocated) {
               ASSERT(o->mpn != INVALID_MPN);
               MemMap_SetIOProtectionRange(MPN_2_MA(o->mpn), 
                                           PAGES_2_BYTES(s->numMPNs),
                                           MMIOPROT_IO_ENABLE);
	       // Round robin: calculate next node or color
	       if (IsVMPhysicalPage(s)) {
		  mm->nextNode = (mm->nextNode + 1) % mm->numNodes;
	       } else {
		  mm->nextKernelColor = (mm->nextKernelColor + 1) % mm->numColors;
		  if (mm->nextKernelColor == 0) {
		     mm->nextNode = (mm->nextNode + 1) % mm->numNodes;
		  }
	       }

               o->lastNumFreePages = mm->numFreePages;
	       o->node = node;
	       o->color = color;
	       return POLICY_OK;
	    }
	    // Reach this point should mean no free pages of this color & node.
	 }
      }

      LOG(7, "Out of color: %d for ppn: %x freePages: %d",
	      color, s->ppn, mm->numFreePages);
      if (s->color == MM_COLOR_ANY) {
	 n = MemMapNextColor(mm, n);
      }
      if (!n) {
         o->lastNumFreePages = mm->numFreePages;

         LOG(1, "policy failed -- cannot meet constraints");
	 DEBUG_ONLY({
	    MemMapDumpState(s, 1);
         })
         LOG(1, "recType=%s recMask=0x%x affMask=0x%x",
             MMTypeString(recType), recMask, affMask);
         MemMap_LogState(1);

         MEMMAP_DEBUG_ONLY({
            MemMapLogFreePages();
         })

         o->type = recType;
	 return POLICY_NO_PAGES;
      }
   }

   NOT_REACHED();
   return POLICY_NO_PAGES;
}


/*
 *-----------------------------------------------------------------------------
 *
 * MemMap_NodeFreePages --
 *
 *     Returns the number of free pages on node "n"
 *
 * Results:
 *     Returns the number of free pages on node "n"
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
uint32
MemMap_NodeFreePages(NUMA_Node n)
{
   return memMap.node[n].numFreePages;
}

/*
 *-----------------------------------------------------------------------------
 *
 * MemMap_NodeTotalPages --
 *
 *     Returns the number of total pages on node "n"
 *
 * Results:
 *     Returns the number of total pages on node "n"
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
uint32
MemMap_NodeTotalPages(NUMA_Node n)
{
   return memMap.node[n].totalNodePages;
}

/*
 *-----------------------------------------------------------------------------
 *
 * MemMap_NodePctMemFree --
 *
 *     Returns the percentage of memory free on node "n" as an unsigned int
 *     (not a decimal, obviously)
 *
 * Results:
 *     Returns percent memory free on node "n"
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
uint32
MemMap_NodePctMemFree(NUMA_Node n)
{
   uint32 numFree = memMap.node[n].numFreePages;
   if (numFree > 0) {
      return (100 * (memMap.node[n].totalNodePages - numFree)) / numFree;
   } else {
      return 0;
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * MemMap_GetInfo --
 *
 * 	return info about memmap
 *
 * Results:
 * 	VMK_OK
 *
 * Side Effects:
 * 	None
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
MemMap_GetInfo(VMnix_MemMapInfoArgs *args,
	       VMnix_MemMapInfoResult *result,
               unsigned long resultLen)
{
   MemMapInfo *mm = &memMap;
   result->totalPages = mm->totalMemPages + biosMemMapStats.numDiscarded +
                        biosMemMapStats.numKernelUse;

   result->totalKernelPages = MemMap_KernelPages() +
                              biosMemMapStats.numKernelUse;
   result->totalLowReservedPages = mm->reservedLowPages;
   result->totalFreePages = MemMapUnusedPages(mm);
   return VMK_OK;
}

#if VMX86_DEBUG

#define IOPROT_MPNS_PER_WORD  (sizeof(Atomic_uint32)*8)
#define IOPROT_WORDS_PER_PAGE (PAGE_SIZE/sizeof(Atomic_uint32))
#define IOPROT_MPNS_PER_PAGE  (IOPROT_MPNS_PER_WORD*IOPROT_WORDS_PER_PAGE)

/*
 *----------------------------------------------------------------------
 *
 * MemMapIOProtGetNumMPNs --
 *
 *    Get the number of MPNs required to set up the IO protect tables for
 *    the given range of MPNs.
 *
 * Results:
 *    Number of contiguous MPNs required.
 *
 * Side effects:
 *    none.
 *
 *----------------------------------------------------------------------
 */
static uint32
MemMapIOProtGetNumMPNs(MPN minMPN,
                       MPN maxMPN,
                       UNUSED_PARAM(Bool hotAdd))
{
   uint32 nPages = maxMPN - minMPN + 1;
   return CEILING(nPages, IOPROT_MPNS_PER_PAGE);
}

/*
 *----------------------------------------------------------------------
 *
 * MemMapIOProtAssignMPNs --
 *
 *    Initialize the IOProtect table
 *
 * Results:
 *    VMK_OK on success, VMK_FAILURE otherwise
 *
 * Side effects:
 *    none.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
MemMapIOProtAssignMPNs(MPN minMPN,
                       MPN maxMPN,
                       Bool hotAdd,
                       uint32 reqSize,
                       MPN startMPN)
{
   MPN mpn;
   IOProtMapArray *seg;

   // sanity check
   ASSERT(reqSize == MemMapIOProtGetNumMPNs(minMPN, maxMPN, hotAdd));

   if (allocatedIOProtSegments == MAX_AVAIL_MEM_RANGES) {
      Panic("IOProtMapArray is full");
      return VMK_FAILURE;
   }

   seg = &IOProtMap[allocatedIOProtSegments];

   // Init segment's descriptor
   seg->memRangeMinMPN = minMPN;
   seg->memRangeMaxMPN = maxMPN;
   seg->metadataMinMPN = startMPN;
   seg->metadataMaxMPN = startMPN + reqSize - 1;

   // sanity
   ASSERT(seg->memRangeMinMPN <= seg->memRangeMaxMPN);
   ASSERT(seg->metadataMinMPN <= seg->metadataMaxMPN);
   // metadata min is in [mem range min, mem range max]
   ASSERT(seg->memRangeMinMPN <= seg->metadataMinMPN &&
          seg->metadataMinMPN <= seg->memRangeMaxMPN);
   // metadata max is in [mem range min, mem range max]
   ASSERT(seg->memRangeMinMPN <= seg->metadataMaxMPN &&
          seg->metadataMaxMPN <= seg->memRangeMaxMPN);

   // sanity: successive ranges are fully above the boot range
   // IOProtMetadataMap() relies on this assumption to set *cosMemory.
   if (allocatedIOProtSegments > 0) {
      IOProtMapArray *bootseg = &IOProtMap[0];
      ASSERT(bootseg->memRangeMaxMPN < seg->memRangeMinMPN);
   }

   // zero all metadata pages
   for (mpn = seg->metadataMinMPN;  mpn <= seg->metadataMaxMPN; mpn++) {
      // vmkernel pages are not allowed for I/O until they get allocated
      Util_ZeroMPN(mpn);
   }

   // Don't make this new segment visible until
   // after it has been fully initialized.
   allocatedIOProtSegments++;

   // allow I/O to heap
   Mem_SetIOProtection();

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * IOProtMetadataMap --
 *
 *      Finds and maps the IO metadata corresponding to 'mpn'.
 *
 * Results:
 *      Return NULL on failure, otherwise a pointer to the IO metadata
 *      for 'mpn'
 *
 *      *inRange -- TRUE when 'mpn' is a valid MPN, otherwise FALSE   
 *      *cosMemory -- TRUE when 'mpn' is a cos MPN, otherwise FALSE   
 *      *pair -- on success, used to map the metadata page
 *      *bitOffset -- on success, the bit index within the return value
 *                   *which holds * IO metadata for 'mpn'. 
 *                   otherwise, undefined.
 *
 * Side effects:
 * Possibly maps a page, which should be unmapped w/MemMapUnmapPage.  */
Atomic_uint32 *
IOProtMetadataMap(MPN mpn,
                  Bool *inRange,      // OUT
                  Bool *cosMemory,    // OUT
                  uint32 *bitOffset,  // OUT
                  KSEG_Pair **pair)   // OUT
{
   int i;
   IOProtMapArray *seg;
   Atomic_uint32 *ptr;

   // Cos memory is defined as the memory below all the memory ranges.
   // In boundary case where there are no memory ranges, there is no
   // cos memory.  (This follows the convention of the old code).
   // Also, the boot time segment (IOProtMap[0]) is assumed be to the
   // lowest memory range in the system.  This assumption is verified
   // in MemMapIOProtAssignMPNs().
   *cosMemory = FALSE;   
   if (allocatedIOProtSegments > 0 && mpn < IOProtMap[0].memRangeMinMPN) {
      *cosMemory = TRUE;
   }

   // there really is no good default value
   *bitOffset = 0;

   // find memory range containing 'mpn'
   for (seg = NULL, i = 0; i < allocatedIOProtSegments; i++) {
      seg = &IOProtMap[i];
      if (seg->memRangeMinMPN <= mpn && mpn <= seg->memRangeMaxMPN) {
         break;
      }
      seg = NULL;
   }

   // is in range if we found a mem range containing 'mpn' 
   *inRange = seg ? TRUE : FALSE;

   if (!seg) {
      return NULL;
   } else {
      // calculate MA of the page of metadata which contains the IO bit for 'mpn'
      uint32 mpnOffset = mpn - seg->memRangeMinMPN;
      MPN metadataMPN = seg->metadataMinMPN + mpnOffset/IOPROT_MPNS_PER_PAGE;

      // sanity: only access memory we allocated
      ASSERT(seg->metadataMinMPN <= metadataMPN &&
             metadataMPN <= seg->metadataMaxMPN);
      ptr = MemMapMapPage(metadataMPN, pair);
      if (!ptr) {
         //  mapping failed
         return NULL;
      } else {
         // Note: there is one subtlety here.  The VA returned by
         // MemMapMapPage() is not going to be the same one passed to
         // back MemMapUnmapPage(), as would be expected.  The former
         // is page aligned, but the later is some where within that
         // page.  Just wanted to point this out.  It really is not
         // problem, since KVMap can deal with this.
         uint32 wordOffset = (mpnOffset % IOPROT_MPNS_PER_PAGE) / IOPROT_MPNS_PER_WORD;
         *bitOffset = (mpnOffset % IOPROT_MPNS_PER_WORD);
         return &ptr[wordOffset];
      }
   }
}



/*
 *----------------------------------------------------------------------
 *
 * MemMap_SetIOProtection --
 *
 *      Mark the given mpn either usable or unusable for I/O
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
void
MemMap_SetIOProtection(MPN mpn, Bool ioAble)
{
   Bool inRange, cosMemory;
   Atomic_uint32 *metadata;
   uint32 bitOffset;
   KSEG_Pair *pair;
   
   metadata = IOProtMetadataMap(mpn, &inRange, &cosMemory, &bitOffset, &pair);

   // sanity check.
   ASSERT(cosMemory == FALSE);

   if (allocatedIOProtSegments > 0) {
      // Ideally, the 'if' wouldn't be necessary.  But during early
      // init there are a few calls to this function before memmap.c
      // has been informed about existing memory ranges.  We should
      // eliminate this so that all memory pages get the correct IO
      // permissions.  And so that the ASSERT can stand alone.
      ASSERT(inRange == TRUE);
   }

   if (metadata == NULL) {
      // can't map array, so not much to do here...
      // This is not so bad because IsIOAble will return TRUE if it can't
      // map either.  This case happens when PSODing and out of kseg
      // entries.
      return;
   }

   if (ioAble) {
      Atomic_Or(metadata, (1 << bitOffset));
   } else {
      Atomic_And(metadata, ~(1 << bitOffset));
   }
   MemMapUnmapPage(metadata, pair);
}

/*
 *----------------------------------------------------------------------
 *
 * MemMap_SetIOProtectionRange --
 *
 *      Mark the given address range either usable or unusable for I/O
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
void
MemMap_SetIOProtectionRange(MA maddr, uint64 len, Bool ioAble)
{
   MA ma;

   for (ma = maddr; ma < maddr + len; ma += PAGE_SIZE) {
      MemMap_SetIOProtection(MA_2_MPN(ma), ioAble);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * MemMap_IsIOAble --
 *
 *      Check if the given MPN is allowed to be used for I/O
 *
 * NOTE:
 *	This is a first cut at an IOAble check.
 *	It would be cleaner to separate out all of memory into three regions:
 *	    COS
 *	    vmkernel
 *	    rest	   
 *      Rest should always return FALSE.
 *
 * Results:
 *      TRUE if mpn is allowed, FALSE otherwise
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
Bool
MemMap_IsIOAble(MPN mpn)
{
   Bool inRange, cosMemory, ioAble;
   Atomic_uint32 *metadata;
   uint32 bitOffset;
   KSEG_Pair *pair;

   metadata = IOProtMetadataMap(mpn, &inRange, &cosMemory, &bitOffset, &pair);
   if (cosMemory) {
      // this is a COS address, I/O is allowed
      return TRUE;
   }
   if (!inRange) {
      // this is not a vmkernel or a COS address, I/O is not allowed
      return FALSE;
   }
   if (metadata == NULL) {
      // mapping can fail if all four of the kseg entries are used.
      // this is a rare case, but .... assume the page is I/O able
      return TRUE;
   }

   // read metadata
   ioAble = (Atomic_Read(metadata) & (1 << bitOffset)) ? TRUE : FALSE;

   MemMapUnmapPage(metadata, pair); 
   return ioAble;
}

/*
 *----------------------------------------------------------------------
 *
 * MemMap_IsIOAbleRange --
 *
 *      Check if the given address range is allowed to be used for I/O
 *
 * Results:
 *      TRUE if range is allowed, FALSE otherwise
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
Bool
MemMap_IsIOAbleRange(MA maddr, uint64 len)
{
   MA ma;

   for (ma = maddr; ma < maddr + len; ma += PAGE_SIZE) {
      if (!MemMap_IsIOAble(MA_2_MPN(ma))) {
         return FALSE;
      }
   }
   return TRUE;
}

#endif
