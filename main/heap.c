/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * heap.c --
 *
 *	Manage memory heaps.  Provides poisoning and locking on top of
 *	dlmalloc allocator.  Uses an IRQ lock so it can be called with
 *	interrupts off.
 */

#include "vm_types.h"
#include "vmkernel.h"
#include "memalloc.h"
#include "splock.h"
#include "vm_libc.h"
#include "timer.h"
#include "memmap_dist.h"
#include "dlmalloc_int.h"
#include "heap_int.h"
#include "util.h"
#include "heap_public.h"
#include "heapMgr.h"
#include "histogram.h"
#include "list.h"
#include "proc.h"

#define LOGLEVEL_MODULE Heap
#include "log.h"

#if defined(VMX86_DEBUG)

/* Only zero out this many bytes on regions larger */
#define CLEARMEM_MAX_SIZE       1024

/* Memory poisoning is a way to check for heap corruption. If you enable
 * it you will add some extra space to the end of all allocations, which
 * will be filled with a POISON value. You can then check that these 
 * bytes still have the same value at various places by enabling one of
 * the options below MEM_POISON (and you should enable at least one). */
#define MEM_POISON              (1)

/* Enabling the below (and not the above) will only check for memory
 * corruption on a call to Mem_Free and then only the chunk returned
 * is checked. This has a very low overhead. */
#define POISONCHECK_ON_MEMFREE  (1 && MEM_POISON)

/* Enabling the below will check all heap allocated memory for corruption
 * on regular intervals by adding a timer. You can set the interval by
 * adjusting the POISONCHECK_TIMER_PERIOD defined further down. If you
 * enable this, you should also enable POISONCHECK_ON_MEMFREE, since you
 * could otherwise have guys writing past end of memory, but freeing it
 * before the timer scan can catch it (only memory in use is checked). */
#define POISONCHECK_TIMERCHECKS (0 && MEM_POISON)

#define HEAP_FREE_OWNERSHIP_CHECK (1)

#else

#define CLEARMEM_MAX_SIZE       128

/* Please do ONLY enable these in your private builds!!! */
#define MEM_POISON              (0)
#define POISONCHECK_ON_MEMFREE  (0 && MEM_POISON)
#define POISON_ALLOCCHECKS      (0 && MEM_POISON)
#define POISONCHECK_TIMERCHECKS (0 && MEM_POISON)
#define HEAP_FREE_OWNERSHIP_CHECK (0)

#endif

/* If we run with timer checks, do it every 10 seconds */
#define	POISONCHECK_TIMER_PERIOD		(10*1000)

// poison data/structures/sizes
typedef struct PoisonPrefix {
   uint32 magic;
   uint32 bytes;
   uint32 prefixBytes;
   uint32 callerPC;
} PoisonPrefix;

#define POISON_MAGIC 0x4d474850 // 'MGHP' -- "MaGicHeaP"
#define POISON_PREFIX_SIZE 24 // must be >= PoisonPrefix and a uint32
#define POISON_SUFFIX_SIZE 16
#define POISON_BYTE 0x5A


// defines to control printout from HeapCheckMemoryPressure
#define PRESSURE_FIRST_MSG_PERCENT 20
#define PRESSURE_NTH_MSG_PERCENT   4
#define PRESSURE_LOG_USERS_PERCENT 10
#define PRESSURE_FIRST_DUMP_NTH_CALLER  1024
#define PRESSURE_LATER_LOG_NTH_CALLER   1024
#define PRESSURE_LATER_DUMP_NTH_CALLER  (1024*1024)

#define INIT_LEAST_PERCENT_FREE (PRESSURE_FIRST_MSG_PERCENT + PRESSURE_NTH_MSG_PERCENT)

// The minimum time between successive DumpAllocations (to save disk space)
#define MIN_DUMP_PERIOD_SECONDS 3600

typedef struct CallerList {
   uint32 pc;		// pc of caller of Mem_Alloc()
   int num;		// number of times called from this pc
   uint32 size;		// allocation size of last call
   void *ptr;           // a chunk allocated from this pc
} CallerList;

/* Number of callers to dump when out of memory */
#define MAX_USERS_TO_DUMP 64

/* Maximum number of different ranges a growable "dynamic" heap can support */
#define MAX_RANGES 10

// structure containing management info for the heap, also the heap identifier
#define MAX_PTR_SIZE 12
#define MAX_HEAP_PROC_NAME (MAX_HEAP_NAME + MAX_PTR_SIZE)
typedef struct Heap {
   // links must be first element
   // linked list of heaps
   List_Links links;

   char name[MAX_HEAP_NAME];
   SP_SpinLockIRQ heapLock;

   mstate mallocState;

   uint32 currentSize; 
   uint32 maximumSize;

   Timer_Handle timerCheck;

   // used by MoreCore
   VA rangeStart[MAX_RANGES];
   uint32 rangeLen[MAX_RANGES];
   Bool initialRangeReported; 
   uint8 curRange;

   MemRequestFunc reqFunc;
   MemFreeFunc freeFunc;

   // storage and synchronization for CheckMemoryPressure and dumpAllocations
   Bool loggingUsers;
   Bool stopLogging;
   CallerList memUsers[MAX_USERS_TO_DUMP];

   // used by CheckMemoryPressure
   int leastPercentFree;
   uint32 callCount;

   // following used by dumpAllocations
   Timer_AbsCycles lastDumpTimestamp;

   Bool isDynamic;

   // following used only in ProcRead 
   Histogram_Handle freeHist;
   Histogram_Handle usedHist;
   
   Proc_Entry procStats;

} Heap;

// measured in bytes
static Histogram_Datatype heapBuckets[] = {
   16,
   32,
   64,
   128,
   256,
   512,
   1024,
   2048,
   4096,
   8192,
   16384,
   32768,
   65536
};

typedef struct HeapSetup {
   List_Links heapList;
   Bool lateInitDone;
   Proc_Entry procDir;
   
   // protects info in this struct
   SP_SpinLock lock;

} HeapSetup;

static HeapSetup heapSetup;

#define HEAP_NUM_BUCKETS ((sizeof(heapBuckets) / sizeof(heapBuckets[0]) + 1))

static void* HeapMoreCore(Heap *heap, uint32 size);

static void* HeapPoisonChunk(void *rawMem, uint32 bytes,
                             uint32 prefixBytes, uint32 callerPC);
static void HeapCheckPoisonedChunk(Heap* heap, void *rawMem, uint32 size);

static void HeapDumpAllocations(Heap *heap, Bool throttled);

static void HeapCheckMemoryPressure(Heap *heap, uint32 bytes, uint32 callerPC,
                                    SP_IRQL prevIRQ);

static int HeapProcRead(Proc_Entry *entry, char *buffer, int *len);

static void HeapEnableTimerCheck(Heap *heap);

/*
 *----------------------------------------------------------------------
 *
 * Heap(Lock|Unlock|IsLocked) --
 *
 *      Heap locking routines
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE SP_IRQL HeapLock(Heap *heap)
{
   return SP_LockIRQ(&heap->heapLock, SP_IRQL_KERNEL);
}
static INLINE void HeapUnlock(Heap *heap, SP_IRQL prevIRQ)
{
   return SP_UnlockIRQ(&heap->heapLock, prevIRQ);
}
static INLINE Bool HeapIsLocked(Heap *heap)
{
   return SP_IsLockedIRQ(&heap->heapLock);
}

/*
 *---------------------------------------------------------------
 *
 * HeapAddProcNode --
 * 
 *    Adds a proc node for a heap.
 *
 * Results:
 *    None.
 *
 * Side Effects:
 *    None.
 *
 *---------------------------------------------------------------
 */
static void
HeapAddProcNode(Heap *heap)
{
   char name[MAX_HEAP_PROC_NAME];
   snprintf(name, sizeof(name), "%s-%p", heap->name, heap);

   Proc_InitEntry(&heap->procStats);
   heap->procStats.read = HeapProcRead;
   heap->procStats.write = NULL;
   heap->procStats.parent = &heapSetup.procDir;
   heap->procStats.canBlock = FALSE;
   heap->procStats.private = (void *)heap;
   Proc_RegisterHidden(&heap->procStats, name, FALSE);
}


/*
 *----------------------------------------------------------------------
 *
 * Heap_Init
 *
 *	Prepares the heapSetup list so that heaps created early on can have
 *	proc nodes created for them in Heap_LateInit.
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
Heap_Init(void)
{
   SP_InitLock("heapSetup", &heapSetup.lock, SP_RANK_LEAF);
   heapSetup.lateInitDone = FALSE;
   List_Init(&heapSetup.heapList);
}

/*
 *---------------------------------------------------------------
 *
 * Heap_LateInit --
 * 
 *    Initialize the proc nodes.
 *
 * Results:
 *    None.
 *
 * Side Effects:
 *    None.
 *
 *---------------------------------------------------------------
 */
void
Heap_LateInit(void)
{
   Heap *cur;

   SP_Lock(&heapSetup.lock);

   ASSERT(heapSetup.lateInitDone == FALSE);

   Proc_InitEntry(&heapSetup.procDir);
   Proc_RegisterHidden(&heapSetup.procDir, "heaps", TRUE);

   LIST_FORALL(&heapSetup.heapList, (List_Links *)cur) {
      HeapEnableTimerCheck(cur);
      HeapAddProcNode(cur);
   }
   heapSetup.lateInitDone = TRUE;

   SP_Unlock(&heapSetup.lock);
}

/*
 *----------------------------------------------------------------------
 *
 * HeapCreate
 *
 *	Performs basic tasks of heap initialization. Allocates management
 *	overhead memory from the given memory range.
 *
 * Results:
 *	Heap identifier
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
Heap *
HeapCreate(char *name, void *start, uint32 len, SP_Rank lockRank)
{
   Heap *heap;
   uint32 mallocStateSize;
   VA firstAllocatedAddr;
   Bool lateInitDone;

   if (sizeof(Heap) > len) {
      return NULL;
   }
   heap = (Heap*)start;
   memset(heap, 0, sizeof(Heap));

   heap->mallocState = (mstate)(((VA)heap) + sizeof(Heap));
   mallocStateSize = DLM_InitHeap(heap->mallocState, heap, &HeapMoreCore);
   if (mallocStateSize + sizeof(Heap) > len) {
      return NULL;
   }
   firstAllocatedAddr = ALIGN_UP(((VA)heap->mallocState) + mallocStateSize,
                                 MALLOC_ALIGNMENT);

   strncpy(heap->name, name, sizeof(heap->name));
   heap->name[sizeof(heap->name) - 1] = '\0';
   SP_InitLockIRQ("memLck", &heap->heapLock, lockRank);
   
   heap->rangeStart[0] = firstAllocatedAddr;
   heap->rangeLen[0] = (VA)start + len - firstAllocatedAddr;
   heap->currentSize = heap->maximumSize = heap->rangeLen[0];

   heap->leastPercentFree = INIT_LEAST_PERCENT_FREE;

   SP_Lock(&heapSetup.lock);

   List_InitElement(&heap->links);
   List_Insert(&heap->links, LIST_ATFRONT(&heapSetup.heapList));

   lateInitDone = heapSetup.lateInitDone;

   SP_Unlock(&heapSetup.lock);
   
   if (lateInitDone) {
      HeapEnableTimerCheck(heap);
      HeapAddProcNode(heap);
   } 
     
   return heap;
}


/*
 *----------------------------------------------------------------------
 *
 * Heap_CreateStatic --
 * 
 *	Wrapper for HeapCreate that specifies a static heap lock rank.
 *
 * Results:
 *      Heap identifier
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Heap *
Heap_CreateStatic(char *name, void *start, uint32 len)
{
   /*
    * vmkperf needs to use mainHeap, and vmkperf has a lock of rank MEMTIMER... 
    * This warning applies to other code as well which uses the MEMTIMER lock ranking
    * and performs operations involving static heaps.
    *
    */
   ASSERT(SP_RANK_STATIC_HEAPLOCK > SP_RANK_IRQ_MEMTIMER);
   return HeapCreate(name, start, len, SP_RANK_STATIC_HEAPLOCK);
}

/*
 *----------------------------------------------------------------------
 *
 * Heap_CreateCustom --
 *
 *      Create a custom dynamic heap.  Allocate the management overhead
 *      in addition to the initial memory size. Must specify request and free
 *      function pointers.
 *
 * Results:
 *      Heap identifier
 *
 * Side effects:
 *      Requests memory from request function.
 *
 *----------------------------------------------------------------------
 */
Heap *
Heap_CreateCustom(char *name, uint32 initial, uint32 maximum, 
                  MemRequestFunc reqFunc, MemFreeFunc freeFunc)
{
   Heap *heap;
   void *firstAddr;
   uint32 len;
   uint32 heapManageMem;

   ASSERT(reqFunc);
   ASSERT(freeFunc);
   ASSERT(maximum >= initial);

   heapManageMem = ALIGN_UP(sizeof(Heap) + DLM_GetStateSize(), MALLOC_ALIGNMENT);

   if (reqFunc(initial + heapManageMem, &firstAddr, &len) != VMK_OK) {
      Warning("Could not allocate %d bytes of initial memory for dynamic heap %s", 
              initial + heapManageMem, name);
      return NULL;
   }

   ASSERT(len >= initial + heapManageMem);
  
   ASSERT(SP_RANK_DYNAMIC_HEAPLOCK > SP_RANK_IRQ_MEMTIMER);

   heap = HeapCreate(name, firstAddr, MIN(len, maximum + heapManageMem), SP_RANK_DYNAMIC_HEAPLOCK);

   /*
    * This ASSERT is necessary to ensure that we can accurately keep track of the heap's
    * current size. Look at the comment in MoreCore regarding subtracting memory for
    * fenceposts for more information.
    */
   ASSERT((MIN(len, maximum + heapManageMem) == maximum + heapManageMem) 
          || (len % MALLOC_ALIGNMENT == 0));

   if (heap == NULL) {
      Warning("Could not create dynamic heap %s", name);
      freeFunc(firstAddr, len);
      return NULL;
   }
      
   heap->maximumSize = maximum;
   heap->reqFunc = reqFunc;
   heap->freeFunc = freeFunc;
   heap->isDynamic = TRUE;

   LOG(1, "Dynamic heap %s successfully created.", name);

   return heap;
}

/*
 *----------------------------------------------------------------------
 *
 * Heap_CreateDynamic --
 *
 *	Wrapper for Heap_CreateCustom. Specifies the use of
 *	HeapMgr_RequestAnyMem and FreeAnyMem functions, so this heap can be
 *	backed by both high and low physical memory.
 *
 * Results:
 *      Heap identifier
 *
 * Side effects:
 *
 *----------------------------------------------------------------------
 */
Heap *
Heap_CreateDynamic(char *name, uint32 initial, uint32 maximum)
{
   return Heap_CreateCustom(name, initial, maximum,
   			    HeapMgr_RequestAnyMem,
			    HeapMgr_FreeAnyMem);
}

/*
 *----------------------------------------------------------------------
 *
 * Heap_CreateDynamicLowMem --
 *
 *	Wrapper for Heap_CreateCustom. Specifies the use of
 *	HeapMgr_RequestLowMem and FreeLowMem functions, so this heap can be
 *	backed by only low physical memory.
 *
 * Results:
 *      Heap identifier
 *
 * Side effects:
 *
 *----------------------------------------------------------------------
 */
Heap *
Heap_CreateDynamicLowMem(char *name, uint32 initial, uint32 maximum)
{
   return Heap_CreateCustom(name, initial, maximum,
   			    HeapMgr_RequestLowMem,
			    HeapMgr_FreeLowMem);
}

/*
 *----------------------------------------------------------------------
 *
 * HeapCurrentAvail
 *
 *      Return the amount of memory currently available in the heap. For dynamic heaps, 
 *	this does not include the amount that the heap can be grown. Heap lock must 
 *	already be held.
 *
 * Results:
 *      Available memory, excluding growable amount
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static uint32
HeapCurrentAvail(Heap *heap)
{
   ASSERT(HeapIsLocked(heap));
   
   uint32 avail = DLM_Avail(heap->mallocState);

   /* 
    * If a heap has already been assigned some initial memory, but no allocations have
    * yet been made, dlmalloc doesn't know about it. 
    */
   if (!heap->initialRangeReported) {
      avail += heap->rangeLen[0];
   }

   return avail;
}

/*
 *----------------------------------------------------------------------
 *
 * HeapAvail
 *
 *      Return the amount of memory available in the heap. For dynamic heaps, this
 *      includes the amount that the heap can be grown. Heap lock must already be held.
 *
 * Results:
 *      Available memory, including growable amount
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static uint32
HeapAvail(Heap *heap)
{
   ASSERT(HeapIsLocked(heap));
   
   return HeapCurrentAvail(heap) + heap->maximumSize - heap->currentSize;
}

/*
 *----------------------------------------------------------------------
 *
 * Heap_Avail
 *
 *	Wrapper for HeapAvail that locks the heap.
 *
 * Results:
 *      Available memory, including possible growth
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
uint32
Heap_Avail(Heap *heap)
{
   SP_IRQL prevIRQL;
   uint32 avail;

   prevIRQL = HeapLock(heap);

   avail = HeapAvail(heap);
   
   HeapUnlock(heap, prevIRQL);

   return avail;
}

/*
 *----------------------------------------------------------------------
 *
 * Heap_CurrentAvail
 *
 *	Wrapper for HeapCurrentAvail that locks the heap.
 *
 * Results:
 *      Available memory, excluding future growth
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
uint32
Heap_CurrentAvail(Heap *heap)
{
   SP_IRQL prevIRQL;
   uint32 avail;

   prevIRQL = HeapLock(heap);

   avail = HeapCurrentAvail(heap);
   
   HeapUnlock(heap, prevIRQL);

   return avail;
}

/*
 *----------------------------------------------------------------------
 *
 * Heap_DestroyWithPanic --
 *
 *      Destroy the given heap.
 *
 * Results:
 *      VMK_OK if heap was empty and was destroyed, VMK_FAILURE if the heap 
 *	was not empty and was still destroyed.
 *
 * Side effects:
 *      Returns memory via the specified freeFunc.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Heap_DestroyWithPanic(Heap *heap, Bool nonEmptyPanic)
{
   SP_IRQL prevIRQ; 
   VMK_ReturnStatus status = VMK_OK;
   int retryLoops;
   int rangeLoops;

   ASSERT(heap != NULL);

   prevIRQ = HeapLock(heap);
  
   if (heap->timerCheck != TIMER_HANDLE_NONE) {
      Bool removed = Timer_Remove(heap->timerCheck);
      if (!removed) {
         Warning("Could not remove timer for poison check.");
      }
      ASSERT(removed);
      heap->timerCheck = TIMER_HANDLE_NONE;
   }

   heap->stopLogging = TRUE;
   // wait up to 1 second (1 million us) for memUsers output to drain
   for (retryLoops = 1000*1000; retryLoops > 0; retryLoops--) {
      if (!heap->loggingUsers) {
         break;
      }
      HeapUnlock(heap, prevIRQ);
      Util_Udelay(1);
      PAUSE();
      prevIRQ = HeapLock(heap);
   }
  
   // if memUsers output still draining, return failure
   if (heap->loggingUsers) {
      Warning("Heap %s busy logging heap usage, cannot destroy heap.",
      heap->name);
      return VMK_FAILURE;
   }

   heap->stopLogging = FALSE;

   HeapUnlock(heap, prevIRQ);

   SP_Lock(&heapSetup.lock);
   List_Remove(&heap->links);
   SP_Unlock(&heapSetup.lock);

   Proc_Remove(&heap->procStats);

   if (Heap_CurrentAvail(heap) < heap->currentSize) {
      Warning("Non-empty heap (%s) being destroyed (avail is %d, should be %d).",
              heap->name, Heap_CurrentAvail(heap), heap->currentSize);
      if (MEM_POISON) {
	 HeapDumpAllocations(heap, FALSE);
      }
      if (vmx86_debug && nonEmptyPanic) {
         Panic("Non-empty heap (%s) being destroyed (avail is %d, should be %d).\n",
               heap->name, Heap_CurrentAvail(heap), heap->currentSize);
      }
      status = VMK_FAILURE;
      /*
       * Fall through and clean up the lock and heap metadata anyway.
       */
   } else {
      LOG(1, "Heap %s is empty and is being destroyed.", heap->name);
   }

   /*
    * If the heap is dynamic, all of its memory was allocated within heap.c and
    * thus this file is also responsible for freeing it. As such, the following
    * code steps through all the alllocated ranges and calls the specified
    * freeFunc. 
    */
   
   SP_CleanupLockIRQ(&heap->heapLock);
   
   if (heap->isDynamic) {
   
      MemFreeFunc freeFuncCopy = heap->freeFunc;
      uint32 initialLen = heap->rangeStart[0] + heap->rangeLen[0] - (VA)heap; 

      for (rangeLoops = 1; rangeLoops <= heap->curRange; rangeLoops++) {
	 if (heap->freeFunc((void*)heap->rangeStart[rangeLoops], 
		  heap->rangeLen[rangeLoops]) != VMK_OK) {
	    Warning("Unable to free memory at %p in heap %s.",
		  (void*)heap->rangeStart[rangeLoops], heap->name);
	    status = VMK_FAILURE;
	 }
      }
 
      memset(heap, 0, sizeof(Heap));

      if (freeFuncCopy(heap, initialLen) != VMK_OK) {
	 Warning("Unable to free memory at %p.", (void*)heap);
	 status = VMK_FAILURE;
      }
   } else {
      memset(heap, 0, sizeof(Heap));
   }
  
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * Heap_Destroy --
 *
 *      Wrapper for Heap_DestroyWithPanic.
 *
 * Results:
 *      VMK_OK if heap was empty and was destroyed, panics if the heap 
 *	was not empty and running debug code.
 *
 * Side effects:
 *      Returns memory via the specified freeFunc.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Heap_Destroy(Heap *heap)
{
   return Heap_DestroyWithPanic(heap, TRUE);
}

/*
 *----------------------------------------------------------------------
 *
 * HeapHistAddChunkInfo --
 * 
 *	Used by ProcRead to create used and free histograms to print out.
 *
 *      Requires the heap spinlock held
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
HeapHistAddChunkInfo(Heap *heap, Bool inUse, void *rawMem, uint32 rawBytes)
{
   ASSERT(HeapIsLocked(heap));

   if (inUse) {
      Histogram_Insert(heap->usedHist, (Histogram_Datatype)rawBytes);
   } else {
      Histogram_Insert(heap->freeHist, (Histogram_Datatype)rawBytes);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * HeapForEachChunk --
 *
 *	Steps through all the heap's ranges, calling DLM_ForEachChunk on each range.
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
HeapForEachChunk(Heap *heap, Bool inUseOnly, Heap_ChunkCallback callback) 
{
   int range;

   if (!heap->initialRangeReported) {
      return;
   }

   /*
    * Initial range is special. Because the heap struct is created out of the first range
    * of memory, no MALLOC_ALIGNMENT-sized buffer is needed here.
    */
   DLM_ForEachChunk(heap->mallocState, inUseOnly, callback,
                    (void *)heap->rangeStart[0], heap->rangeLen[0]);

   for (range = 1; range <= heap->curRange; range++) {
      DLM_ForEachChunk(heap->mallocState, inUseOnly, callback, 
		       (char *)heap->rangeStart[range] + MALLOC_ALIGNMENT, 
		       heap->rangeLen[range] - MALLOC_ALIGNMENT);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * HeapProcRead --
 *
 *	Writes out information about a particular heap. Allocates new histograms
 *	and deletes them after info is printed out.
 *
 * Results:
 *	None. 
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
HeapProcRead(Proc_Entry *entry, char *buffer, int *len) 
{
   SP_IRQL prevIRQL;
   Heap *heap = (Heap *)entry->private;
   *len = 0;
   Histogram_Handle tempFreeHist;
   Histogram_Handle tempUsedHist;
   uint32 avail, curAvail;
   uint32 percentFree, curPercentFree;

   tempFreeHist = Histogram_New(mainHeap, HEAP_NUM_BUCKETS, heapBuckets);
   tempUsedHist = Histogram_New(mainHeap, HEAP_NUM_BUCKETS, heapBuckets);

   if (tempFreeHist == NULL || tempUsedHist == NULL) {
      // Yes, Histogram_Delete is ok with NULL
      Histogram_Delete(mainHeap, tempUsedHist);
      Histogram_Delete(mainHeap, tempFreeHist);
      Proc_Printf(buffer, len, "<failed to allocate memory for %s>\n",
                  heap->name);
      return VMK_OK;
   }

   prevIRQL = HeapLock(heap);

   avail = HeapAvail(heap);
   percentFree = (100 * avail) / heap->maximumSize;

   if (heap->isDynamic) {
     
      curAvail = HeapCurrentAvail(heap);
      curPercentFree = (100 * curAvail) / heap->currentSize;
      
      Proc_Printf(buffer, len, "Dynamic heap: %s\n", heap->name);
      Proc_Printf(buffer, len, "Grown: %d times\n", heap->curRange);
      Proc_Printf(buffer, len, "Max grows: %d\n", MAX_RANGES - 1);
      Proc_Printf(buffer, len, "Current size: %d bytes\n", heap->currentSize);
      Proc_Printf(buffer, len, "Current available: %d bytes\n", curAvail);
      Proc_Printf(buffer, len, "Current percent free: %d%%\n", curPercentFree); 
      Proc_Printf(buffer, len, "Maximum size: %d bytes\n", heap->maximumSize);
      Proc_Printf(buffer, len, "Maximum available: %d bytes\n", avail);
      Proc_Printf(buffer, len, "Maximum percent free: %d%%\n", percentFree);

   } else {
      Proc_Printf(buffer, len, "Static heap: %s\n", heap->name);
      Proc_Printf(buffer, len, "Maximum size: %d bytes\n", heap->maximumSize);
      Proc_Printf(buffer, len, "Available: %d bytes\n", avail);
      Proc_Printf(buffer, len, "Percent free: %d%%\n", percentFree);
   }

   if (heap->leastPercentFree == INIT_LEAST_PERCENT_FREE) {
      Proc_Printf(buffer, len, "Least percent free: >= %d%%\n", INIT_LEAST_PERCENT_FREE);
   } else { 
      Proc_Printf(buffer, len, "Least percent free: %d%%\n", heap->leastPercentFree);
   }

   heap->freeHist = tempFreeHist;
   heap->usedHist = tempUsedHist;

   HeapForEachChunk(heap, FALSE, HeapHistAddChunkInfo);

   Proc_Printf(buffer, len, "\nAllocated Regions (in bytes): \n\n");
   
   Histogram_ProcFormat(heap->usedHist, "   ", buffer, len);

   Proc_Printf(buffer, len, "\n\nFree Regions (in bytes): \n\n");

   Histogram_ProcFormat(heap->freeHist, "   ", buffer, len);

   heap->freeHist = NULL;
   heap->usedHist = NULL;

   HeapUnlock(heap, prevIRQL);

   Histogram_Delete(mainHeap, tempFreeHist);
   Histogram_Delete(mainHeap, tempUsedHist);

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * HeapMoreCore --
 *
 *      Called by the dlmalloc allocator to get more memory for the given
 *      heap.
 *
 *	MALLOC_ALIGNMENT is added and subtracted in a lot of places throughout this file
 *	so that ranges given to dlmalloc can be guaranteed never to combine. This is done
 *	by purposefully reporting the start of a new region to be MALLOC_ALIGNMENT past
 *	what has actually been allocated. All of this is necessary for DLM_ForEachChunk
 *	to work.
 *
 * Results:
 *      Pointer to start of new memory region if size !=0, otherwise the
 *      last valid address of the last region returned.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void*
HeapMoreCore(Heap *heap, uint32 size)
{
   VA ptr = 0;
   void *newRangeAddr;
   uint32 newRangeLen, request;
   VMK_ReturnStatus status;
   uint32 recognizedSize;

   ASSERT(HeapIsLocked(heap));

   LOG(1, "%s: size=%u", heap->name, size);

   if (size == 0) {
      /* If size is 0, just report end of current range. */
      ASSERT(heap->rangeLen[heap->curRange] != 0);
      
      ptr = heap->rangeStart[heap->curRange] + heap->rangeLen[heap->curRange];
   } else if (!heap->initialRangeReported) {
      /*
       * Otherwise, if we haven't yet reported the initial range of memory that
       * we guaranteed was allocated back in the create function, then we should
       * report that here.
       */
      if (size <= heap->rangeLen[0]) {
         ptr = heap->rangeStart[0];
	 heap->initialRangeReported = TRUE;
      } else {
         /* 
	  * Note that code should really never get here. Initial sizes should be
	  * big enough to support the first call to MoreCore...
	  */
         Warning("Initial range too small for MoreCore. Heap %s, %d bytes",
		 heap->name, heap->rangeLen[0]);
      }
   } else if (heap->currentSize == heap->maximumSize) {
      /* 
       * Otherwise more memory is being requested. If we're already at or over our max
       * size, report a warning and eventually (at the bottom) return NULL.
       */
     
      Warning("Heap %s already at its maximumSize. Cannot expand.",
              heap->name);
   }  else if (heap->curRange + 1 < MAX_RANGES) {
      /* 
       * Otherwise, if we haven't exhausted our limited number of ranges (which
       * we really should not ever do), then calculate the amount of memory that
       * we should add to this heap, see if it will cause us to exceed our max
       * size. If it doesn't, go ahead and try to allocate it.
       */
      
      request = MAX(heap->rangeLen[heap->curRange], 
                    heap->maximumSize / MAX_RANGES);
      request = MAX(request, size + MALLOC_ALIGNMENT);
      request = MIN(request, heap->maximumSize - heap->currentSize + MALLOC_ALIGNMENT); 

      if (request < size + MALLOC_ALIGNMENT) {
         Warning("Request for more memory would exceed maximum size of heap %s",
	         heap->name);
      } else if ((status = heap->reqFunc(request, &newRangeAddr, &newRangeLen)) != 
                 VMK_OK) {
	 Warning("Could not allocate %d bytes for dynamic heap %s. Request returned %s", 
	          request, heap->name, VMK_ReturnStatusToString(status));
      } else {
	 ASSERT(newRangeLen >= size + MALLOC_ALIGNMENT);

         /*
	  * The recognizedSize is used essentially to "lie" to a dynamic heap so
	  * that the currentSize can never exceed the maximumSize. Allowing such
	  * a contradiction would raise a need for lots of extra logic... We can
	  * get a way with having a smaller recognizedSize than the actual
	  * length of the region because upon freeing the region of memory, the
	  * actual freed memory portion is just checked to make sure that it's
	  * at least as large as it's expected to be.
	  */
         recognizedSize = MIN(newRangeLen, 
	                      heap->maximumSize - heap->currentSize + MALLOC_ALIGNMENT);

         ASSERT((newRangeLen == heap->maximumSize - heap->currentSize + MALLOC_ALIGNMENT)
	        || (newRangeLen % MALLOC_ALIGNMENT == 0));

	 heap->curRange++; 
	 heap->rangeStart[heap->curRange] = (VA)newRangeAddr;
	 heap->rangeLen[heap->curRange] = recognizedSize;
 
	 /*
	  * Everytime the heap is grown, dlmalloc uses a "fencepost" to mark the end of a
	  * range. This fencepost allocation subtracts 3 * SIZE_SZ from the end of
	  * the top portion of memory, and then aligns the length of the region with
	  * MALLOC_ALIGNMENT. Since we ASSERT that we're either on the last "grow" or are
	  * passing in a region that is a multiple of MALLOC_ALIGNMENT in length, the
	  * following subtraction of - DLM_GetFencepostSize() should always work, as we
	  * try to accurately set our "current size" of the heap.
	  */
	 heap->currentSize += recognizedSize - MALLOC_ALIGNMENT - DLM_GetFencepostSize(); 

	 ptr = heap->rangeStart[heap->curRange] + MALLOC_ALIGNMENT;
      }
   } else {
      Warning("Heap %s could not be grown to accomodate the memory request",
	    heap->name);
   }

   ASSERT(heap->currentSize <= heap->maximumSize);
   
   return (void*)ptr;
}

/*
 *----------------------------------------------------------------------
 *
 * Heap_AlignWithRA --
 *
 *      Allocate aligned memory and store poison data around it if poisoning
 *      is enabled. Also takes the return address as an argument so that allocations made
 *      through multiple function calls (like a kmalloc from a device driver with a
 *      private heap) can have the location of the allocation accurately describe in dump
 *      allocation printouts.
 *
 * Results:
 *      Allocated object.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void *
Heap_AlignWithRA(Heap *heap, uint32 bytes, uint32 alignment, void *ra)
{
   void *rawMem, *mem;
   SP_IRQL prevIRQL;
   uint32 rawBytes = bytes;
   uint32 prefixBytes = 0;
   uint32 callerPC = (uint32)ra;

   ASSERT(heap != NULL);
   ASSERT(heap->mallocState != NULL);
   ASSERT(alignment != 0);

   if (bytes == 0) {
      return NULL;
   }

   if (ra == NULL) {
      callerPC = (uint32)__builtin_return_address(0);
   }

   prevIRQL = HeapLock(heap);

   if (MEM_POISON) {
      prefixBytes = ALIGN_UP(POISON_PREFIX_SIZE, alignment);
      rawBytes += prefixBytes + POISON_SUFFIX_SIZE;
   }
   
   rawMem = DLM_memalign(heap->mallocState, alignment, rawBytes);

   if (MEM_POISON && (rawMem != NULL)) {
      mem = HeapPoisonChunk(rawMem, bytes, prefixBytes, callerPC);
   } else {
      mem = rawMem;
   }

   if (mem != NULL) {
      // HeapCheckMemoryPressure will release the heap lock
      HeapCheckMemoryPressure(heap, bytes, callerPC, prevIRQL);
   } else {
      HeapUnlock(heap, prevIRQL);
      Warning("Heap_Align(%s, %d/%d bytes, %d align) failed."
              "  caller: %#x",
              heap->name, bytes, rawBytes, alignment, callerPC);
      if (MEM_POISON) {
         HeapDumpAllocations(heap, TRUE);
      }
   }
   
   ASSERT((((VA)mem) & (alignment - 1)) == 0);
   LOG(2, "%s: %p %d bytes %d alignment", heap->name, mem, bytes, alignment);

   return mem;
}


/*
 *----------------------------------------------------------------------
 *
 * HeapManagesAddr --
 *
 *	Returns TRUE if mem is an addr managed by heap, and FALSE if not.
 *
 *	Just checks heap bounds, doesn't check that mem is actually a
 *	valid object start.
 *
 * Results:
 *      TRUE if mem is managed by heap
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
HeapManagesAddr(Heap* heap, void* mem)
{
   int range;

   ASSERT(heap != NULL);
   ASSERT(HeapIsLocked(heap));

   for (range = 0; range <= heap->curRange; range++) {
      const VA hMin = heap->rangeStart[range] + MALLOC_ALIGNMENT;
      const VA hMax = hMin + heap->rangeLen[range] - MALLOC_ALIGNMENT;
      if ((VA)mem >= hMin && (VA)mem < hMax) { 
         return TRUE;
      }
   }

   return FALSE;
}


/*
 *----------------------------------------------------------------------
 *
 * Heap_Free --
 *
 *      Free memory allocated by Heap_Align.  If poison check are enabled,
 *      verify that the poison area is still consistent.
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
Heap_Free(Heap *heap, void* mem)
{
   SP_IRQL prevIRQL;
   void *rawMem = mem;
      
   LOG(2, "%s: %p", heap->name, mem);

   prevIRQL = HeapLock(heap);
   ASSERT(mem!=NULL);

   if (HEAP_FREE_OWNERSHIP_CHECK) {
      ASSERT(HeapManagesAddr(heap, mem));
   }

   if (MEM_POISON) {
      PoisonPrefix *prefix = (PoisonPrefix*)((uint8*)mem - sizeof *prefix);
      ASSERT(prefix->magic == POISON_MAGIC);
      rawMem = (uint8*)mem - prefix->prefixBytes;

      if (POISONCHECK_ON_MEMFREE) {
         HeapCheckPoisonedChunk(heap, rawMem, 0);
      }

      memset(rawMem, 0xff,
             MIN(prefix->prefixBytes + prefix->bytes, CLEARMEM_MAX_SIZE));
   }
   DLM_free(heap->mallocState, rawMem);
   HeapUnlock(heap, prevIRQL);
}

static INLINE void
HeapLogUser(Heap *heap, int index)
{
   Log("%s: %d bytes (ptr=%p) allocated from caller %p in at least %d calls.",
       heap->name, heap->memUsers[index].size,  heap->memUsers[index].ptr,
       (void *) heap->memUsers[index].pc, heap->memUsers[index].num);
}

/*
 *----------------------------------------------------------------------
 *
 * HeapCheckMemoryPressure --
 *
 *      Check whether we are running low on memory and warn user every time
 *      we cross PRESSURE_FIRST_MSG_PERCENT boundary. If we cross
 *      PRESSURE_LOG_USERS_PERCENT threshold, also inform the user of the
 *      return addresses from where the most recent allocations occurred.
 *
 *      This function also releases the heap lock
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Updates the heap's callers array, releases heap lock
 *
 *----------------------------------------------------------------------
 */
void
HeapCheckMemoryPressure(Heap *heap, uint32 bytes, uint32 callerPC, SP_IRQL prevIRQL)
{
   int percentFree;
   
   percentFree = (DLM_FastAvail(heap->mallocState) + heap->maximumSize -
                  heap->currentSize) / (heap->maximumSize / 100);
   if (percentFree < heap->leastPercentFree - PRESSURE_NTH_MSG_PERCENT) {
      /* This is actually warning about the available "top" memory *
       * that has not been allocated at all.  For this quick
       * check, we're not bothering to count the chunks of memory
       * that have been returned to the allocator. */
      LOG(0, "%s: heap below %d%% -- %d bytes free", heap->name, percentFree,
          HeapAvail(heap));
      heap->leastPercentFree = percentFree;
   }
   if ((percentFree < PRESSURE_LOG_USERS_PERCENT)  && !heap->loggingUsers) {
      /*
       * If memory is really low, start tracing the frequent callers to
       * Mem_Alloc() and print them out periodically.  Initially, we track
       * every caller and print out at the PRESSURE_FIRST_DUMP_NTH_CALLER
       * caller.  But, then we slow down to avoid log spew (see PR
       * 20935). So, afterwards we track only every
       * PRESSURE_LATER_LOG_NTH_CALLER caller, and print out on every
       * PRESSURE_LATER_DUMP_NTH_CALLER caller.
       */
      int i;
      int free, min;
	
      if (++heap->callCount <= PRESSURE_FIRST_DUMP_NTH_CALLER) {
         if (heap->callCount == PRESSURE_FIRST_DUMP_NTH_CALLER) {
            heap->loggingUsers = TRUE;
         }
      } else {
         if ((heap->callCount % PRESSURE_LATER_LOG_NTH_CALLER) != 0) {
            HeapUnlock(heap, prevIRQL);
            return;
         } else if ((heap->callCount % PRESSURE_LATER_DUMP_NTH_CALLER) == 0) {
            heap->loggingUsers = TRUE;
         }
      }

      min = -1;
      free = -1;
      for (i = 0; i < ARRAYSIZE(heap->memUsers); i++) {
         if (heap->memUsers[i].pc == callerPC) {
            break;
         }
         if (min == -1 || heap->memUsers[i].num < min) {
            free = i;
            min = heap->memUsers[i].num;
         }
      }
      if (i == ARRAYSIZE(heap->memUsers)) {
         /* If this is a caller that's not in the table, replace the
          * entry with the minimum 'num' count. */
         heap->memUsers[free].pc = callerPC;
         heap->memUsers[free].num = 1;
         heap->memUsers[free].size = bytes;
      } else {
         heap->memUsers[i].pc = callerPC;
         heap->memUsers[i].num++;
         heap->memUsers[i].size = bytes;
      }


      if (heap->loggingUsers) {
      
         Log("%s: heap below %d%% -- %d bytes free", heap->name, percentFree, 
	    HeapAvail(heap));

         /*
          * Release lock while dumping to serial to avoid lock spinouts.
          * We've already set loggingUsers, so heap can't be destroyed
          * and no one will modify memUsers array while we're in here.
          */
         HeapUnlock(heap, prevIRQL);
  
	 for (i = 0; i < ARRAYSIZE(heap->memUsers); i++) {
            if (heap->stopLogging) {
               Log("%s: requested to stop logging", heap->name);
               break;
            }
            if (heap->memUsers[i].num != 0) {
               HeapLogUser(heap, i);
            }
            heap->memUsers[i].num = 0;
            heap->memUsers[i].pc = 0;
         }

         prevIRQL = HeapLock(heap);
         heap->loggingUsers = FALSE;
      }
   }
   HeapUnlock(heap, prevIRQL);
}

/*
 *----------------------------------------------------------------------
 *
 * HeapAddChunkInfo --
 *
 *      Used by DumpAllocations to get memory usage information.
 *      Add the size of of a chunk together with info on who
 *      allocated the memory and how much memory was allocated to
 *      a list of users using the most memory..
 *
 *      Requires the heap spinlock held
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
HeapAddChunkInfo(Heap *heap, Bool inUse, void *rawMem, uint32 rawBytes)
{
   int i;
   uint32 callerPC = 1;

   ASSERT(inUse);

   if (MEM_POISON) {
      PoisonPrefix *prefix = (PoisonPrefix*)(*(void**)rawMem);
      ASSERT(prefix->magic == POISON_MAGIC);
      callerPC = prefix->callerPC;
   }

   for (i = 0; i < ARRAYSIZE(heap->memUsers); i++) {
      if (MEM_POISON && (heap->memUsers[i].pc == callerPC)) {
         heap->memUsers[i].size += rawBytes;
         heap->memUsers[i].num++;
         break;
      }
      if (heap->memUsers[i].pc == 0) {
         heap->memUsers[i].pc = callerPC;
         heap->memUsers[i].size = rawBytes;
         heap->memUsers[i].num = 1;
         heap->memUsers[i].ptr = rawMem;
         break;
      }
   }
   if (i == ARRAYSIZE(heap->memUsers)) {
      /* OK, this caller cannot make it on the list, so dump
       * the info on him right here */
      LOG(0, "%s: %d bytes allocated from caller %p.",
          heap->name, rawBytes, (void *) callerPC);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * HeapDumpAllocations --
 *
 *      Dump the size of allocated chunks together with info on who
 *      allocated the memory and how much memory was allocated for the
 *      most memory hogging callers.  If 'throttled' is TRUE, don't dump
 *      data if its already happened recently.
 *
 *      Optimally this would be a dynamic list (or hash table), but
 *      since we only call this function when we have run out of
 *      memory, we use a statically allocated array instead.
 *
 *      Requires the heapLock spinlock held
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
HeapDumpAllocations(Heap *heap, Bool throttled)
{
   int i;
   SP_IRQL prevIRQ;
   
   /* Ensure that we are doing memory poisoning if we call this */
   ASSERT(MEM_POISON);
   
   prevIRQ = HeapLock(heap);
   
   /*
    * don't dump too frequently, or we'll fill up disk space with logs
    * and crash the system (PR 20935).
    */
   if (throttled && heap->lastDumpTimestamp != 0) {
      Timer_AbsCycles curTimestamp = Timer_GetCycles();
      if (heap->lastDumpTimestamp > curTimestamp) {
         // time not fully synched between CPUS
         HeapUnlock(heap, prevIRQ);
         return;
      }
      if ((Timer_TCToMS(curTimestamp - heap->lastDumpTimestamp)/1000) <
          MIN_DUMP_PERIOD_SECONDS) {
         HeapUnlock(heap, prevIRQ);
         return;
      }
   }
   if (heap->loggingUsers || heap->stopLogging) {
      HeapUnlock(heap, prevIRQ);
      LOG(0, "%s: log busy", heap->name);
      return;
   }

   heap->lastDumpTimestamp = Timer_GetCycles();

   /* Clear the list */
   for (i = 0; i < ARRAYSIZE(heap->memUsers); i++) {
      heap->memUsers[i].pc = 0;
      heap->memUsers[i].size = 0;
      heap->memUsers[i].num = 0;
      heap->memUsers[i].ptr = NULL;
   }
   
   HeapForEachChunk(heap, TRUE, HeapAddChunkInfo);

   /*
    * Release lock while dumping to serial to avoid lock spinouts.  We've
    * already set loggingUsers, so heap can't be destroyed and no one will
    * modify memUsers array while we're in here.
    */
   heap->loggingUsers = TRUE;
   HeapUnlock(heap, prevIRQ);

   /* Dump the list */
   Log("Contents of %s:", heap->name);
   for (i = 0; i < ARRAYSIZE(heap->memUsers); i++) {
      if (heap->stopLogging) {
         Log("%s: requested to stop logging", heap->name);
         break;
      }
      if (heap->memUsers[i].pc != 0) {
         HeapLogUser(heap, i);
      }
   }

   prevIRQ = HeapLock(heap);
   heap->loggingUsers = FALSE;
   HeapUnlock(heap, prevIRQ);
}

/*
 *----------------------------------------------------------------------
 *
 * HeapPoisonChunk --
 *
 *      Fill out poison areas with POISON_BYTE and tracking information.
 *      Requires the heapLock spinlock held
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void *
HeapPoisonChunk(void *rawMem, uint32 bytes, uint32 prefixBytes, uint32 callerPC)
{
   PoisonPrefix *prefix;
   void *mem = ((uint8*)rawMem) + prefixBytes;

   ASSERT(MEM_POISON);
   memset(rawMem, POISON_BYTE, prefixBytes);

   ASSERT(prefixBytes >= ((sizeof *prefix) + sizeof (uint32)));
   prefix = (PoisonPrefix*)((uint8*)mem - sizeof *prefix);
   ASSERT(((VA)prefix & (sizeof (uint32) - 1)) == 0);
   prefix->magic = POISON_MAGIC;
   prefix->bytes = bytes;
   prefix->prefixBytes = prefixBytes;
   prefix->callerPC = callerPC;
   ASSERT((VA)rawMem < (VA)prefix);
   *(void**)rawMem = prefix;

   memset((uint8*)mem + bytes, POISON_BYTE, POISON_SUFFIX_SIZE);

   return mem;
}

/*
 *----------------------------------------------------------------------
 *
 * HeapDumpChunk --
 *
 *      Dump the poison areas of the given chunk.
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
HeapDumpChunk(void *rawMem)
{
   PoisonPrefix *prefix = (PoisonPrefix*)(*(void**)rawMem);
   int i;
   uint32 prefixBytes;

   if ((VA)prefix > VMK_FIRST_ADDR) {
      Log("raw=%p prefix=%p pb=%x b=%x pc=%x", rawMem, prefix,
          prefix->prefixBytes, prefix->bytes, prefix->callerPC);
      prefixBytes = prefix->prefixBytes;
   } else {
      prefixBytes = 128;
   }
   for (i = 0; i < prefixBytes; i+= 16) {
      uint32 *p = (uint32*)(((VA)rawMem) + i);
      Log("%p: 0x%08x 0x%08x 0x%08x 0x%08x", p,
          p[0], p[1], p[2], p[3]);
   }

   for (i = ALIGN_DOWN(prefix->prefixBytes + prefix->bytes, sizeof (uint32));
        i < prefix->prefixBytes;
        i+= 16) {
      uint32 *p = (uint32*)(((VA)rawMem) + i);
      Log("%p: 0x%08x 0x%08x 0x%08x 0x%08x", p,
          p[0], p[1], p[2], p[3]);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * HeapCheckPoisonedChunk --
 *
 *      Check that the poison bytes written by HeapPoisonChunk haven't
 *      been modified.
 *
 *      Requires the heapLock spinlock held
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Will ASSERT fail if poison bytes are not there.
 *
 *----------------------------------------------------------------------
 */
void
HeapCheckPoisonedChunk(Heap* heap, void *rawMem, uint32 size)
{
   PoisonPrefix *prefix = (PoisonPrefix*)(*(void**)rawMem);
   uint8 *p;
   int i;

   ASSERT(MEM_POISON);
   ASSERT(HeapManagesAddr(heap, rawMem));
   ASSERT(prefix->magic == POISON_MAGIC);

   if (size) {
      ASSERT(size >= prefix->prefixBytes + prefix->bytes + POISON_SUFFIX_SIZE);
   }

   ASSERT(*(void**)rawMem == prefix);
   for (p = ((uint8*)rawMem) + sizeof(uint32); p < (uint8*)prefix; p++) {
      if (*p != POISON_BYTE) {
         HeapDumpChunk(rawMem);
         Panic("prefix poison overwritten: heap=%s raw=%p size=%x\n",
               heap->name, rawMem, size);
      }
   }

   p = (uint8*)rawMem + prefix->prefixBytes + prefix->bytes;
   for (i = 0; i < POISON_SUFFIX_SIZE; i++, p++) {
      if (*p != POISON_BYTE) {
         HeapDumpChunk(rawMem);
         Panic("suffix poison [%d] overwritten: heap=%s raw=%p size=%x\n",
               i, heap->name, rawMem, size);
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 *  HeapCheckPoisonHelper --
 *
 *      Helper function invoked by Heap_CheckPoison on each element of the
 *      heap that is in use.
 *
 * Results:
 *      None. 
 *
 * Side effects:
 *      Checks the poison fenceposts in the given item on the heap.
 *
 *----------------------------------------------------------------------
 */
static void
HeapCheckPoisonHelper(Heap *heap, Bool inUse, void *ptr, uint32 size)
{
   ASSERT(inUse);
   HeapCheckPoisonedChunk(heap, ptr, size);
}


/*
 *----------------------------------------------------------------------
 *
 *  Heap_CheckPoison --
 *
 *      Iterate over all the allocated chunks in the given heap and check
 *      the integrity of their poison fenceposts.  Should only be invoked
 *      if MEM_POISON is enabled (otherwise its pretty useless).
 *
 * Results:
 *      None. 
 *
 * Side effects:
 *      Checks all allocated memory in the given heap.
 *
 *----------------------------------------------------------------------
 */
void 
Heap_CheckPoison(Heap* heap)
{
   if (MEM_POISON) {
      SP_IRQL prevIRQL = HeapLock(heap);
      HeapForEachChunk(heap, TRUE, HeapCheckPoisonHelper);
      HeapUnlock(heap, prevIRQL);
   } else {
      LOG(0, "Disabled (MEM_POISON not enabled)");
   }
}


/*
 *----------------------------------------------------------------------
 *
 *  HeapCheckPoisonCB --
 *
 *      Timer callback used by HeapEnableTimerCheck.
 *
 * Results:
 *      None. 
 *
 * Side effects:
 *      Calls Heap_CheckPoison on given heap.
 *
 *----------------------------------------------------------------------
 */
static void 
HeapCheckPoisonCB(void *data, UNUSED_PARAM(Timer_AbsCycles timestamp))
{
   /* Ensure that we are doing memory poisoning if we call this */
   ASSERT(MEM_POISON);

   Heap_CheckPoison((Heap*)data);
}   


/*
 *----------------------------------------------------------------------
 *
 *  HeapEnableTimerCheck
 *
 *      Start a periodic timer callback to check poison info
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
HeapEnableTimerCheck(Heap *heap)
{
   if (POISONCHECK_TIMERCHECKS && heap->timerCheck == TIMER_HANDLE_NONE) {
      heap->timerCheck = Timer_Add(PRDA_GetPCPUNumSafe(),
	    			   HeapCheckPoisonCB,
				   POISONCHECK_TIMER_PERIOD,
				   TIMER_PERIODIC,
				   heap);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * Heap_Dump
 *
 *	Dump the contents of a given heap.  The exact dumping method
 *	is taken care of by the callback function.  Simply iterate
 *	over the range of heap regions, calling the callback function
 *	with the start address and length of each.
 *
 *	Note that we drop the lock between calls to the callback.  This
 *	is okay since a heap region is never removed unless the whole
 *	heap itself is removed.  It is possible for the heap itself to
 *	be removed, so this function should only be called on heaps
 *	that the current world is actually using (thus negating the
 *	possibility of the heap being removed out from under us).
 *
 * Results:
 *	VMK_OK on success, otherwise on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Heap_Dump(Heap *heap, HeapDumpCallback callback, void *cookie)
{
   VMK_ReturnStatus status = VMK_OK;
   SP_IRQL prevIRQL;
   VA start;
   uint32 len;
   int i;

   prevIRQL = HeapLock(heap);
   for (i = 0; i <= heap->curRange && status == VMK_OK; i++) {
      start = heap->rangeStart[i];
      len = heap->rangeLen[i];

      HeapUnlock(heap, prevIRQL);
      status = callback(cookie, start, len);
      prevIRQL = HeapLock(heap);
   }
   HeapUnlock(heap, prevIRQL);

   return status;
}
