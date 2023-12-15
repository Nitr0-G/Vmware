/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * heapMgr.c --
 *
 *	This file implements a heap manager, which satisfies requests for more
 *	memory from dynamically growable heaps. 
 *
 *      The heap manager uses buddy allocators to manage potentially large
 *      amounts of address space and physical memory. Physical memory is always
 *      allocated from MemMap in large page chunks, making more efficient use of
 *      the TLB cache.
 *
 *      When a dynamic heap requests more memory, the heap manager checks to see
 *      if it can satisfy the request using existing memory managed by its buddy
 *      allocator. If it doesn't, the manager attempts to add more physical
 *      memory and contigous XMap address space to satisfy the request.  Note
 *      that the physical memory for large regions (>2MB) is not at all
 *      guaranteed to be contiguous.
 *
 *	Currently, two buddy allocators are used. One manages physical memory
 *	(and the virtual address space 'attached' to it) that is low memory --
 *	memory whose address is < 4GB. Some device drivers require low memory in
 *	order for the hardware to be able to access it. The other buddy
 *	allocator is the any memory allocator -- this one may have its address
 *	space backed by either high or low memory. If you don't specifically
 *	need low memory, you should use the the more general purpose any memory
 *	allocator.
 *
 *	For debugging purposes, there are currently two defines that can be
 *	enabled/disabled. The HEAPMGR_GUARDPAGE define causes the heap manager
 *	to allocate a page before all requests and fill it with a special value.
 *	That guardpage is verified to be intact when memory is freed by dynamic
 *	heaps. The HEAPMGR_FREE_REGION_CHECK define causes the heap manager to
 *	fill all free memory regions with a special value. That value is checked
 *	whenever memory is allocated to be sure that no stale pointers are in
 *	use in outside code. Since memory is re-used within the heap manager,
 *	the hope is that any stale pointers would be used between the time a
 *	portion of memory is freed and its next reallocation. When using either
 *	check, both of which are currently enabled only in debug builds, the
 *	code panics when a violation is detected.
 *
 *	The heap manager also possesses the ability to release extra memory.
 *	The manner in which it performs this feat is a little obtuse (but
 *	necessarily so). When the manager notices that it has a lot of free
 *	memory (currently inside FreeMem), it sets a bottom half handler to
 *	occur. That bottom half function calls for a helper world to run a
 *	function that goes through both of the heap manager memory allocators 
 *	(the one that manages "low" memory and the one that manages "any" memory)
 *	and releases any extra memory that those allocators have.
 *
 *	The extra steps of a bottom half handler and helper world are necessary
 *	at the moment. A helper world must be used because XMap_Unmap is a
 *	blocking call, and we don't want to perform a blocking call in
 *	HeapMgrFreeMem... However, scheduling a helper world requires holding a
 *	low-level lock, and we don't want to limit the usage of Heap Manager...
 *	So we use a bottom half handler to schedule the helper world.
 *
 *	In order to release memory (both xmap address space and physical pages),
 *	the releasing function calls Buddy_Allocate to be given a range of
 *	memory. This is marked as released for the particular allocator by
 *	flipping a "release bit" -- one bit represents a large page. The
 *	function then goes ahead and unmaps the address space and frees the
 *	physical pages.
 *
 *	Should the manager at some point in the future need more memory and
 *	receive the same address range from an XMap_Map operation as it had
 *	previously released, it notices this, flips the "release bit" back, and
 *	calls Buddy_Free to make the buddy allocator realize it once again has
 *	control of that address space.
 *
 *	To those familiar with the term, this is essentially heapMgr
 *	"ballooning."
 *
 */

#include "vm_types.h"
#include "vmkernel.h"
#include "buddy.h"
#include "heap_int.h"
#include "xmap.h"
#include "vmk_layout.h"
#include "memalloc_dist.h"
#include "memmap.h"
#include "splock.h"
#include "bh_dist.h"
#include "helper.h"

#define LOGLEVEL_MODULE HeapMgr
#include "log.h"

#define KB (1024)
#define MB (KB * 1024)
#define GB (MB * 1024)

#define HEAPMGR_RELEASE_BEGIN (12 * MB)
#define HEAPMGR_RELEASE_END (8 * MB)

/* Min and max sizes should be multiples of PAGE_SIZE */
#define HEAPMGR_MIN_BUF_SIZE (64 * KB)
/* Max size should be multiple of PDE_SIZE */
#define HEAPMGR_MAX_BUF_SIZE (2 * MB)

#define HEAPMGR_MIN_BUF_PAGES (BYTES_2_PAGES(HEAPMGR_MIN_BUF_SIZE))
#define HEAPMGR_MAX_BUF_PAGES (BYTES_2_PAGES(HEAPMGR_MAX_BUF_SIZE))

#define HEAPMGR_XMAP_MAX_ADDR            (VMK_FIRST_XMAP_ADDR + VMK_XMAP_LENGTH)
#define HEAPMGR_XMAP_MAX_LA              (VMK_VA_2_LA(HEAPMGR_XMAP_MAX_ADDR))
#define HEAPMGR_XMAP_MAX_INDEX           (LA_2_LPN(HEAPMGR_XMAP_MAX_LA))
#define HEAPMGR_LARGE_PAGE_INDICES       (HEAPMGR_XMAP_MAX_INDEX / VMK_PTES_PER_PDE)

#define HEAPMGR_LARGE_PAGES_TO_ADD       (HEAPMGR_MAX_BUF_PAGES / VMK_PTES_PER_PDE)
#define HEAPMGR_ADD_PAGE_LEN             (HEAPMGR_MAX_BUF_PAGES)

/* 
 * RelInt is the released integer type -- the base type that is used in the
 * array that describes which "large pages" of address space have been released
 * or not. Changing this type should propagate everywhere that matters.
 */
typedef uint32 RelInt;

#define HEAPMGR_RELINT_BITS (sizeof(RelInt) * 8)

#define HEAPMGR_RELEASED_SLOTS (HEAPMGR_LARGE_PAGE_INDICES / HEAPMGR_RELINT_BITS)

typedef struct HeapMgrAllocator {
   char *name;
   Buddy_Handle handle;
   RelInt released[HEAPMGR_RELEASED_SLOTS];
   SP_SpinLockIRQ lock;
   MM_AllocType type;
} HeapMgrAllocator;

static char *anyMemName = "HeapMgrAnyMem";
static char *lowMemName = "HeapMgrLowMem";

static HeapMgrAllocator allocatorAnyMem;
static HeapMgrAllocator allocatorLowMem;

static Bool releaseScheduled;
static SP_SpinLockIRQ releaseLock;
static uint32 releaseBHNum;

#if defined(VMX86_DEBUG)

#define HEAPMGR_GUARDPAGE (1)
#define HEAPMGR_FREE_REGION_CHECK (1)

#else

#define HEAPMGR_GUARDPAGE (0)
#define HEAPMGR_FREE_REGION_CHECK (0)

#endif

#define HEAPMGR_GUARDPAGE_VALUE (0xA5FF00A5)
#define HEAPMGR_FREE_REGION_VALUE (0xA5CC33A5)

/*
 *----------------------------------------------------------------------
 *
 * HeapMgrCheckPage --
 *
 *      Checks a page of memory that should be filled with the specified value.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Panics if any values in memory have been overwritten.
 *
 *----------------------------------------------------------------------
 */
static void
HeapMgrCheckPage(VA pageAddr, uint32 value)
{
   uint32 *pageAddrCopy = (uint32 *)pageAddr;
   uint32 *pageAddrEnd = (uint32 *)(pageAddr + PAGE_SIZE);

   for (; pageAddrCopy < pageAddrEnd; pageAddrCopy++) {
      if( (*pageAddrCopy) != value) {
         Panic("Heap manager page at %x has been overwritten at location %p.", 
	    pageAddr, pageAddrCopy);
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * HeapMgrCheckGuardPage --
 *
 *      Wrapper for HeapMgrCheckPage that specifies HEAPMGR_GUARDPAGE_VALUE.
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
HeapMgrCheckGuardPage(VA pageAddr)
{
   ASSERT(HEAPMGR_GUARDPAGE);
   HeapMgrCheckPage(pageAddr, HEAPMGR_GUARDPAGE_VALUE);
}

/*
 *----------------------------------------------------------------------
 *
 * HeapMgrCheckFreeRegion --
 *
 *      Wrapper for HeapMgrCheckPage that specifies HEAPMGR_FREE_REGION_VALUE and
 *      iterates over all the pages in a region.
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
HeapMgrCheckFreeRegion(VA pageAddr, uint32 nPages)
{
   VA pageAddrEnd = pageAddr + nPages * PAGE_SIZE;

   ASSERT(HEAPMGR_FREE_REGION_CHECK);
   
   for (; pageAddr < pageAddrEnd; pageAddr += PAGE_SIZE) {
      HeapMgrCheckPage(pageAddr, HEAPMGR_FREE_REGION_VALUE);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * HeapMgrFillPage --
 *
 *      Fills a page of memory with specified value. Note: Filling in uint32
 *      sized chunks was tested to be faster than a memset, a fill of uint64's,
 *      and a memcpy from an already filled page.
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
HeapMgrFillPage(void *pageAddr, uint32 value)
{
   uint32 *pageAddrCopy, *pageAddrEnd = pageAddr + PAGE_SIZE;

   for (pageAddrCopy = pageAddr; pageAddrCopy < pageAddrEnd; pageAddrCopy++) {
      *pageAddrCopy = value;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * HeapMgrFillGuardPage --
 *
 *      Wrapper for HeapMgrFillPage that specifies HEAPMGR_GUARDPAGE_VALUE. 
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
HeapMgrFillGuardPage(void *pageAddr)
{
   ASSERT(HEAPMGR_GUARDPAGE);
   HeapMgrFillPage(pageAddr, HEAPMGR_GUARDPAGE_VALUE);
}

/*
 *----------------------------------------------------------------------
 *
 * HeapMgrFillFreeRegion --
 *
 *      Wrapper for HeapMgrFillPage that specifies HEAPMGR_FREE_REGION_VALUE and
 *      iteratres over all the pages in a region. 
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
HeapMgrFillFreeRegion(void *pageAddr, uint32 nPages)
{
   void *pageAddrEnd = pageAddr + nPages * PAGE_SIZE;

   ASSERT(HEAPMGR_FREE_REGION_CHECK);
   
   for (; pageAddr < pageAddrEnd; pageAddr += PAGE_SIZE) {
      HeapMgrFillPage(pageAddr, HEAPMGR_FREE_REGION_VALUE);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * HeapMgrGetSlotAndBitFlag --
 *
 *	This function determines which slot a particular address of a large page
 *	corresponds to in an allocator's released table... It then fills out a
 *	bit flag for future operations on the particular bit that corresponds to
 *	the particular large page in question.
 *
 * 	Steps to converting an address to a released bit:
 * 	   1. convert VA to LA so we're in the range of 0 - 1024 MB
 * 	   2. convert that to an index, or page number
 * 	   3. convert that to a large page index... should be 0 - 511, inclusive
 * 	   4. divide that index by number of bits in a slot to get correct released slot
 * 	   5. mod that index by number of bits in a slot to get correct bit
 *
 * Results:
 *      Sets the slot and bitflag variables passed in.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
HeapMgrGetSlotAndBitFlag(HeapMgrAllocator *allocator, VA addr, 
                         RelInt **slotAddr, RelInt *bitFlag)
{
   uint32 largePageIndex = LA_2_LPN(VMK_VA_2_LA(addr)) / VMK_PTES_PER_PDE;
   uint32 bitToFlip = largePageIndex % HEAPMGR_RELINT_BITS;
   uint32 slot = largePageIndex / HEAPMGR_RELINT_BITS;

   ASSERT(allocator != NULL);
   ASSERT(slotAddr != NULL);
   ASSERT(bitFlag != NULL);
   ASSERT(SP_IsLockedIRQ(&allocator->lock));
   ASSERT((VA)addr % PDE_SIZE == 0);
   ASSERT(largePageIndex < HEAPMGR_LARGE_PAGE_INDICES);
   ASSERT(slot < HEAPMGR_RELEASED_SLOTS); 
  
   *bitFlag = 1 << bitToFlip;
   *slotAddr = &allocator->released[slot];
}

/*
 *----------------------------------------------------------------------
 *
 * HeapMgrMarkReleased --
 *
 *	Marks a particular large page as released within a HeapMgrAllocator.
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
HeapMgrMarkReleased(HeapMgrAllocator *allocator, VA addr)
{
   RelInt *releasedSlot;
   RelInt bitFlag;

   ASSERT(allocator != NULL);
   ASSERT(SP_IsLockedIRQ(&allocator->lock));
 
   HeapMgrGetSlotAndBitFlag(allocator, addr, &releasedSlot, &bitFlag);

   ASSERT(((*releasedSlot) & bitFlag) == 0);

   LOG(2, "Marking %x released. SlotAddr = %p, bitFlag = %d.",
       addr, releasedSlot, bitFlag);
   
   *releasedSlot = (*releasedSlot) | bitFlag;
}

/*
 *----------------------------------------------------------------------
 *
 * HeapMgrCheckReleased --
 *
 *	Checks if a particular large page is marked as released within a
 *	HeapMgrAllocator.
 *
 * Results:
 *      TRUE if page has been released from the allocator back to the system
 *      FALSE if page has never been assigned to allocator or is currently assigned
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static Bool
HeapMgrCheckReleased(HeapMgrAllocator *allocator, VA addr)
{
   RelInt *releasedSlot;
   RelInt bitFlag;

   ASSERT(allocator != NULL);
   ASSERT(SP_IsLockedIRQ(&allocator->lock));

   HeapMgrGetSlotAndBitFlag(allocator, addr, &releasedSlot, &bitFlag);

   LOG(2, "Checking %x. SlotAddr = %p, bitFlag = %d.", addr, releasedSlot, bitFlag);

   /* 
    * Note that the != 0 is necessary because the RelInt type could be bigger than
    * the Bool type (and probably is).
    */
   return (((*releasedSlot) & bitFlag) != 0);
}

/*
 *----------------------------------------------------------------------
 *
 * HeapMgrMarkInUse --
 *
 *	Marks a particular large page as in use within a HeapMgrAllocator.
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
HeapMgrMarkInUse(HeapMgrAllocator *allocator, VA addr)
{
   RelInt *releasedSlot;
   RelInt bitFlag;

   ASSERT(allocator != NULL);
   ASSERT(SP_IsLockedIRQ(&allocator->lock));
 
   HeapMgrGetSlotAndBitFlag(allocator, addr, &releasedSlot, &bitFlag);

   LOG(2, "Marking %x in use. SlotAddr = %p, bitFlag = %d", addr, releasedSlot, bitFlag);
   
   *releasedSlot = (*releasedSlot) & ~bitFlag;
}

/*
 *----------------------------------------------------------------------
 *
 * HeapMgrFreePageArray --
 *
 *      Helper function to loop through and free an array of MPNs.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Sets array elements to INVALID_MPN.
 *
 *----------------------------------------------------------------------
 */
static void
HeapMgrFreePageArray(uint32 nEntries, MPN *mpnArray)
{
   uint32 cur;

   for (cur = 0; cur < nEntries; cur++ ) {
      MemMap_FreeKernelPages(mpnArray[cur]);
      mpnArray[cur] = INVALID_MPN;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * HeapMgrDeallocateLargePages --
 *
 *      Cleanup function to deallocate large pages when errors occur.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
HeapMgrDeallocateLargePages(uint32 nLargePages, MPN *mpnArray, void **vaddr)
{
   LOG(1, "nLargePages = %d, vaddr = %p", nLargePages, *vaddr);
   XMap_Unmap(VMK_PTES_PER_PDE * nLargePages, *vaddr);
   HeapMgrFreePageArray(nLargePages, mpnArray);
   *vaddr = NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * HeapMgrAllocatorReleaseMemory --
 *
 *	This function attempts to release extra memory that an allocator
 *	currently "owns." It does this by first requesting memory in large
 *	chunks, then marking those pages as "released" in the allocator,
 *	and then unmapping them in both XMap and MemMap.
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
HeapMgrAllocatorReleaseMemory(HeapMgrAllocator *allocator)
{
   SP_IRQL prevIRQL;
   uint32 index, counter;
   Buddy_Handle handle;
   uint32 wid = PRDA_GetRunningWorldIDSafe();
   void *ra = __builtin_return_address(1);
   MPN mpnArray[HEAPMGR_LARGE_PAGES_TO_ADD];
   VA releaseVA, indexVA;

   ASSERT(allocator != NULL);
   
   handle = allocator->handle;
   
   while (TRUE) {
      prevIRQL = SP_LockIRQ(&allocator->lock, SP_IRQL_KERNEL); 
     
      /* 
       * Check to see if we've free'd enough or can't get a contiguous
       * HEAPMGR_MAX_BUF_SIZE region to free. Order here is important.
       */
      if (Buddy_GetNumFreeBufs(handle) * HEAPMGR_MIN_BUF_SIZE <= HEAPMGR_RELEASE_END ||
          Buddy_Allocate(handle, HEAPMGR_MAX_BUF_PAGES, wid, ra, &index) != VMK_OK) {
	  
         SP_UnlockIRQ(&allocator->lock, prevIRQL);
	 break;
      }
      
      ASSERT(index % HEAPMGR_MAX_BUF_PAGES == 0);
    
      releaseVA = indexVA = VPN_2_VA(index);

      if (HEAPMGR_FREE_REGION_CHECK) {
         HeapMgrCheckFreeRegion(indexVA, HEAPMGR_MAX_BUF_PAGES); 
      }

      for (counter = 0; counter < HEAPMGR_LARGE_PAGES_TO_ADD; counter++) {
         releaseVA = indexVA + counter * PDE_SIZE; 

	 HeapMgrMarkReleased(allocator, releaseVA);
         mpnArray[counter] = XMap_VA2MPN(releaseVA);
      }

      SP_UnlockIRQ(&allocator->lock, prevIRQL);

      HeapMgrDeallocateLargePages(HEAPMGR_LARGE_PAGES_TO_ADD, mpnArray, 
                                  (void **)&indexVA);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * HeapMgrReleaseExtraMemory --
 *
 *	This is run by a helper world. It asks both allocators (any and low) to
 *	release any extra memory they have.
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
HeapMgrReleaseExtraMemory(void *clientData)
{
   SP_IRQL prevIRQL;
   
   HeapMgrAllocatorReleaseMemory(&allocatorAnyMem);
   HeapMgrAllocatorReleaseMemory(&allocatorLowMem);
 
   prevIRQL = SP_LockIRQ(&releaseLock, SP_IRQL_KERNEL);
   
   releaseScheduled = FALSE;

   SP_UnlockIRQ(&releaseLock, prevIRQL);
}

/*
 *----------------------------------------------------------------------
 *
 * HeapMgrReleaseExtraMemoryBH --
 *
 *      Bottom half function that will use a helper world to call the above
 *      ReleaseExtraMemory function. This bottom half is necessary because
 *      registering a helper world function requires locking a really low level
 *      lock, and nobody wants to limit the usage of heapMgr by doing that
 *      inside a typical heapMgr function.
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
HeapMgrReleaseExtraMemoryBH(void *clientData)
{
   VMK_ReturnStatus status;
   SP_IRQL prevIRQL;
   
   status = Helper_Request(HELPER_MISC_QUEUE, HeapMgrReleaseExtraMemory, NULL);

   if (status != VMK_OK) {
      prevIRQL = SP_LockIRQ(&releaseLock, SP_IRQL_KERNEL);
      releaseScheduled = FALSE;
      SP_UnlockIRQ(&releaseLock, prevIRQL);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * HeapMgrAllocateLargePages --
 *
 *      Allocate large pages to add to the manager. MemMaps physical memory
 *      and acquires virtual address space from XMap.
 *
 * Results:
 *      Returns MPNs of large pages and the vaddr to the whole range.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
HeapMgrAllocateLargePages(uint32 nLargePages, MM_AllocType type, 
                          MPN *mpnArray, void **vaddr)
{
   uint32 curLargePage;
   XMap_MPNRange *largePageRanges;
 
   ASSERT(mpnArray != NULL);
   ASSERT(vaddr != NULL);

   /*
    * Bunch of code calls VMK_VA2MA on the start of heap allocated chunk
    * and assumes that the whole chunk is physically contiguous.
    * Allocating multiple separate large pages and making a single xmap
    * mapping for them would violate that assumption.  Two quick solutions:
    * requests 4MB contiguous from memmap or drop heap manager chunk size
    * to 2MB.  Given PR 50411, switching to 2MB for now.
    */
   ASSERT(nLargePages == 1);

   /* Allocate space to hold range information. */
   largePageRanges = Mem_Alloc(nLargePages * sizeof(XMap_MPNRange));
   
   if (UNLIKELY(largePageRanges == NULL)) {
      HeapMgrFreePageArray(nLargePages, mpnArray); 
      Warning("Could not allocate memory for xmap ranges.");
      return VMK_NO_MEMORY;
   }

   /* MemMap the requested large pages, one at a time. */
   for (curLargePage = 0; curLargePage < nLargePages; curLargePage++) {
  
      mpnArray[curLargePage] = MemMap_NiceAllocKernelLargePage(MM_NODE_ANY,
  							       MM_COLOR_ANY,
							       type);
      if (UNLIKELY(mpnArray[curLargePage] == INVALID_MPN)) {
	 HeapMgrFreePageArray(curLargePage, mpnArray); 
         Mem_Free(largePageRanges);
	 largePageRanges = NULL;
	 Warning("Could not allocate large pages.");
	 return VMK_NO_MEMORY;
      }
      
      largePageRanges[curLargePage].startMPN = mpnArray[curLargePage]; 
      largePageRanges[curLargePage].numMPNs = VMK_PTES_PER_PDE;
   }

   /* Allocate continuous XMap address space for (perhaps discontinuous) large pages */
   *vaddr = XMap_Map(VMK_PTES_PER_PDE * nLargePages, 
                     largePageRanges,
		     nLargePages);

   Mem_Free(largePageRanges);
   largePageRanges = NULL;

   if (UNLIKELY(*vaddr == NULL)) {
      HeapMgrFreePageArray(nLargePages, mpnArray); 
      Warning("Could not allocate xmap address space for large pages.");
      return VMK_NO_ADDRESS_SPACE;
   }

   LOG(1, "nLargePages = %d, vaddr = %p", nLargePages, *vaddr);

   return VMK_OK;
}
/*
 *----------------------------------------------------------------------
 *
 * HeapMgrBuddyHotAdd --
 *
 *      Add memory to existing buddy allocator. Memory must be a
 *      HEAPMGR_LARGE_PAGES_TO_ADD chunk. 
 *
 * Results:
 *      Returns VMK_OK if additional memory successfully added to buddy allocator.
 *
 * Side effects:
 *      May reclaim released address space.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
HeapMgrBuddyHotAdd(HeapMgrAllocator *allocator, void *memVaddr, VPN memVPN,
                   void *manageVaddr, uint32 manageBytes, Buddy_AddrRange *addrRange)
{
   VMK_ReturnStatus status = VMK_OK;
   VA releasedVA, memVA;
   Buddy_Handle handle;

   ASSERT(allocator != NULL);
   ASSERT(memVaddr != NULL);
   ASSERT(addrRange != NULL);

   handle = allocator->handle;
   releasedVA = memVA = (VA)memVaddr;

   /* 
    * Check to see if initial address has ever been released... If it has, the
    * entire HEAPMGR_MAX_BUF_SIZE region needs to be marked in use again 
    */
   if (HeapMgrCheckReleased(allocator, releasedVA)) {
      LOG(2, "Pages at %p were previously released; buddy freeing.", memVaddr);
      
      Buddy_Free(handle, memVPN);
    
      for (; releasedVA < memVA + HEAPMGR_MAX_BUF_SIZE; releasedVA += PDE_SIZE) {
	 HeapMgrMarkInUse(allocator, releasedVA);
      }
   } else {
      /* 
       * Only perform actual Buddy_HotAddRange if this is first time region has
       * been added to buddy allocator. Otherwise, the Buddy_Free above is all
       * you need 
       */
      status = Buddy_HotAddRange(handle, manageBytes, manageVaddr, addrRange->start,
	                         addrRange->len, 1, addrRange);
   }

   return status;
}



/*
 *----------------------------------------------------------------------
 *
 * HeapMgrSetupDynRange --
 *
 *	This just initializes the dynamic range info for a buddy allocator.
 *	
 * 	dynRange.rangeInfo should have the following info: 
 *	 - start is first possible address, which is the first XMap VPN
 *	 - len will be just enough to cover this primary allocation 
 *	 - min/max sizes determine smallest and largest possible allocations 
 *	 - maxLen should encompass all possible hot-added vpn's
 *	 - minHotAddLenHint must be at least as big as maxSize
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
 HeapMgrSetupDynRange(HeapMgrAllocator *allocator, Buddy_DynamicRangeInfo *dynRange,
                      uint32 memVPN, uint32 memPageLength) 
{
   ASSERT(dynRange != NULL);
   ASSERT(allocator != NULL);
 
   dynRange->rangeInfo.start = VMK_FIRST_XMAP_VPN;
   dynRange->rangeInfo.len = memVPN + memPageLength - VMK_FIRST_XMAP_VPN; 
   dynRange->rangeInfo.minSize = HEAPMGR_MIN_BUF_PAGES; 
   dynRange->rangeInfo.maxSize = HEAPMGR_MAX_BUF_PAGES; 
   dynRange->rangeInfo.numColorBits = BUDDY_NO_COLORS;
   snprintf(dynRange->rangeInfo.name, BUDDY_MAX_MEMSPACE_NAME, allocator->name);
   dynRange->maxLen = VMK_NUM_XMAP_PDES * VMK_PTES_PER_PDE;
   dynRange->minHotAddLenHint = HEAPMGR_MAX_BUF_PAGES;
}

/*
 *----------------------------------------------------------------------
 *
 * HeapMgrAddMem --
 *
 *      Add memory to buddy allocator. Memory added in HEAPMGR_LARGE_PAGES_TO_ADD 
 *	chunks. If initial parameter is true, creates the buddy allocator.
 *
 * Results:
 *      Returns VMK_OK if additional memory successfully added to buddy allocator.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
HeapMgrAddMem(HeapMgrAllocator *allocator, Bool initial)
{  
   Buddy_Handle *handle;
   uint32 memVPN, manageBytes;
   MPN mpnArray[HEAPMGR_LARGE_PAGES_TO_ADD];
   void *memVaddr, *manageVaddr = NULL;
   VMK_ReturnStatus status;
   Buddy_AddrRange addrRange;
   Buddy_DynamicRangeInfo dynRange;
   
   ASSERT(allocator != NULL);

   handle = &allocator->handle;

   if (!initial) { 
      ASSERT(SP_IsLockedIRQ(&allocator->lock));
      ASSERT(*handle != NULL);
   }
   
   /* 
    * Allocate required number of physical large pages and xmap them into
    * continuous address space. 
    */
   status = HeapMgrAllocateLargePages(HEAPMGR_LARGE_PAGES_TO_ADD, 
                                      allocator->type, mpnArray, &memVaddr);
   
   if (UNLIKELY(VMK_OK != status)) {
      Warning("Failed to allocate/xmap large pages.");
      return status;
   }

   memVPN = VA_2_VPN((VA)memVaddr);
   addrRange.start = memVPN;
   addrRange.len = HEAPMGR_ADD_PAGE_LEN;

   /* Determine how much, if any, management memory is needed */
   if (initial) {
      HeapMgrSetupDynRange(allocator, &dynRange, memVPN, HEAPMGR_ADD_PAGE_LEN);
      manageBytes = Buddy_DynamicRangeMemReq(&dynRange);
   } else {
      status = Buddy_HotAddMemRequired(*handle, memVPN, HEAPMGR_ADD_PAGE_LEN, 
                                       &manageBytes);

      if (UNLIKELY(VMK_OK != status)) {
	 HeapMgrDeallocateLargePages(HEAPMGR_LARGE_PAGES_TO_ADD, mpnArray, &memVaddr);
	 Warning("Failed to calculate required managment memory for add.");
	 return status;
      }
   }

   LOG(1,"%d management bytes required for adding %d large pages to allocator %s.",
       manageBytes, HEAPMGR_LARGE_PAGES_TO_ADD, allocator->name);

   /* Allocate the management memory if needed */
   if (manageBytes) {
      manageVaddr = Mem_Alloc(manageBytes);

      if (UNLIKELY(manageVaddr == NULL)) {
	 HeapMgrDeallocateLargePages(HEAPMGR_LARGE_PAGES_TO_ADD, mpnArray, &memVaddr);
	 Warning("Failed to allocate management memory.");
	 return status;
      }
   }

   /* Perform free region check if region checks on */
   if (HEAPMGR_FREE_REGION_CHECK) {
      HeapMgrFillFreeRegion(memVaddr, HEAPMGR_ADD_PAGE_LEN);
   }

   /* 
    * Either create or add memory to allocator. If adding, HeapMgrBuddyHotAdd
    * takes care of all the magic necessary for "ballooning" 
    */
   if (initial) {
      status = Buddy_CreateDynamic(&dynRange, manageBytes, manageVaddr, 1, &addrRange, 
                                   handle);
   } else {
      status = HeapMgrBuddyHotAdd(allocator, memVaddr, memVPN, manageVaddr,
      				  manageBytes, &addrRange);
   }

   if (UNLIKELY(status != VMK_OK)) {
      HeapMgrDeallocateLargePages(HEAPMGR_LARGE_PAGES_TO_ADD, mpnArray, &memVaddr);
      Mem_Free(manageVaddr);  
      Warning("Failed to add memory to allocator %s.", allocator->name);
      return status;
   }

   LOG(1, "Successfully added %d bytes to allocator.", HEAPMGR_MAX_BUF_SIZE);

   ASSERT(status == VMK_OK); 
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * HeapMgr_Init --
 *
 *      Initializes low and any memory allocators. Initializes spin locks 
 *	and registers the ReleaseExtraMem bottom half handler. 
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
HeapMgr_Init(void)
{ 
   ASSERT(HEAPMGR_MIN_BUF_SIZE == HEAPMGR_MIN_BUF_PAGES * PAGE_SIZE);
   ASSERT(HEAPMGR_MAX_BUF_SIZE == HEAPMGR_MAX_BUF_PAGES * PAGE_SIZE);
   ASSERT(HEAPMGR_MAX_BUF_SIZE % PDE_SIZE == 0);

   SP_InitLockIRQ("HeapMgrReleaseLock", &releaseLock, SP_RANK_HEAPMGR);
   releaseBHNum = BH_Register(HeapMgrReleaseExtraMemoryBH, NULL);

   allocatorAnyMem.name = anyMemName;
   SP_InitLockIRQ("HeapMgrAnyMemLock", &allocatorAnyMem.lock, SP_RANK_HEAPMGR_HEAP);
   allocatorAnyMem.type = MM_TYPE_ANY;

   allocatorLowMem.name = lowMemName;
   SP_InitLockIRQ("HeapMgrLowMemLock", &allocatorLowMem.lock, SP_RANK_HEAPMGR_HEAP);
   allocatorLowMem.type = MM_TYPE_LOW;

   /*
    * The allocators are initialized here because the lock protecting the buddy
    * systems as a whole (the list of all the buddies, etc.), is fairly low --
    * lower than SP_RANK_HEAPMGR_HEAP, at least. Initializing them here
    * shouldn't hurt anything, and we can leave the buddy lock low.
    */

   HeapMgrAddMem(&allocatorAnyMem, TRUE);
   HeapMgrAddMem(&allocatorLowMem, TRUE);
}

/*
 *----------------------------------------------------------------------
 *
 * HeapMgrRequestMem --
 *
 *      Make a request for memory to the heap. 
 *
 * Results:
 *      Returns VMK_OK on success, error otherwise. 
 *      Sets startAddr and regionLength on success to describe the allocated
 *      region.
 *
 * Side effects:
 *      If buddy allocator cannot service initial request, add memory to the
 *      buddy allocator.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
HeapMgrRequestMem(HeapMgrAllocator *allocator, uint32 size, void **startAddr, 
                  uint32 *regionLength)
{
   Buddy_Handle *handle;
   VMK_ReturnStatus status;
   uint32 index, nPages, returnedPageLength; 
   void *ra = __builtin_return_address(1);
   uint32 wid = PRDA_GetRunningWorldIDSafe();
   uint32 paddedSize = size;
   SP_IRQL prevIRQL;
   VA indexVA;

   ASSERT(allocator != NULL);
   
   if (HEAPMGR_GUARDPAGE) { 
      paddedSize = size + PAGE_SIZE;
   }

   LOG(1, "Request received for %d bytes. Asking for %d bytes.", 
           size, paddedSize);

   nPages = CEIL(paddedSize, PAGE_SIZE);
   
   if (nPages > HEAPMGR_MAX_BUF_PAGES) {
      Warning("Request for heap allocation larger than max buffer size.");
      return VMK_BAD_PARAM;
   }

   prevIRQL = SP_LockIRQ(&allocator->lock, SP_IRQL_KERNEL);
   handle = &allocator->handle;

   ASSERT(*handle != NULL);

   /* Attempt to allocate memory to satisfy request */
   status = Buddy_Allocate(*handle, nPages, wid, ra, &index);

   if (status != VMK_OK) {
      /* Since first attempt failed, add memory to manager */
      status = HeapMgrAddMem(allocator, FALSE);
      
      if (status != VMK_OK) {
         Warning("Could not add memory to heap allocator %s.", allocator->name);
	 goto RequestMemEnd;
      }

      /* Attempt allocation again */
      status = Buddy_Allocate(*handle, nPages, wid, ra, &index);
      
      if (status != VMK_OK) {
         Warning("Could not satisfy request after adding memory to allocator %s.", 
	         allocator->name);
         goto RequestMemEnd;
      }
   }

   indexVA = VPN_2_VA(index);
   returnedPageLength = Buddy_GetLocSize(*handle, index);

   if (HEAPMGR_FREE_REGION_CHECK) {
      HeapMgrCheckFreeRegion(indexVA, returnedPageLength); 
   }

   if (HEAPMGR_GUARDPAGE) {
      /* Adjust length and startAddr to hide use of guardpages */ 
      *regionLength = PAGES_2_BYTES(returnedPageLength - 1); 
      *startAddr = (void *)(indexVA + PAGE_SIZE);

      HeapMgrFillGuardPage(*startAddr - PAGE_SIZE);

   } else {
      *regionLength = PAGES_2_BYTES(returnedPageLength); 
      *startAddr = (void *)indexVA;
   }

   LOG(1, "Satisfied request with %d bytes at %p", 
       *regionLength, *startAddr);

RequestMemEnd:

   SP_UnlockIRQ(&allocator->lock, prevIRQL);
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * HeapMgrFreeMem --
 *
 *      Free memory via the heap manager. Size can be either equal to or smaller
 *      than the size of the region that will actually be freed by the buddy
 *      allocator.
 *
 * Results:
 *      Returns VMK_OK if all went well, VMK_FAILURE otherwise.
 *
 * Side effects:
 *      Memory returned to buddy allocator.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
HeapMgrFreeMem(HeapMgrAllocator *allocator, void *addr, uint32 size)
{  
   uint32 freeSize, nPages;
   Buddy_Handle handle;
   VA adjustedAddr = (VA)addr;
   SP_IRQL prevIRQL;

   ASSERT(allocator != NULL);
   
   if (HEAPMGR_GUARDPAGE) {
      adjustedAddr -= PAGE_SIZE;
      HeapMgrCheckGuardPage(adjustedAddr);
   }

   prevIRQL = SP_LockIRQ(&allocator->lock, SP_IRQL_KERNEL);
   handle = allocator->handle;
   ASSERT(handle != NULL);
   
   /*
    * It is necessary to query the buddy allocator for the actual size of the
    * region when free region checks are on because it is possible that the size
    * passed into this function is smaller than that of the actual region. As
    * such, to properly fill the entire region that will be freed with the free
    * region magic value, we must get the actual size of that region.
    */
   if (HEAPMGR_FREE_REGION_CHECK) {
      nPages = Buddy_GetLocSize(handle, VA_2_VPN(adjustedAddr));
      HeapMgrFillFreeRegion((void *)adjustedAddr, nPages);
   } else {
      nPages = CEIL(size,PAGE_SIZE);
      if (HEAPMGR_GUARDPAGE) {
         nPages++;
      }
   }

   /* freeSize here is what is being freed by the buddy operation */
   freeSize = Buddy_Free(handle, VA_2_VPN(adjustedAddr));
   ASSERT(freeSize >= nPages);

   /* freeSize here is overall free memory managed by buddy */
   freeSize = Buddy_GetNumFreeBufs(handle) * HEAPMGR_MIN_BUF_SIZE;
   
   SP_UnlockIRQ(&allocator->lock, prevIRQL);
   
   prevIRQL = SP_LockIRQ(&releaseLock, SP_IRQL_KERNEL);

   if (!releaseScheduled && freeSize > HEAPMGR_RELEASE_BEGIN) {
      releaseScheduled = TRUE; 
      BH_SetLocalPCPU(releaseBHNum);
   } 

   SP_UnlockIRQ(&releaseLock, prevIRQL);

   LOG(1, "address=%x, len=%x  ra=%p",
       adjustedAddr, nPages, __builtin_return_address(1));

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * HeapMgr_RequestAnyMem --
 *
 *      Wrapper for HeapMgrRequestMem to specify high or low memory.
 *
 * Results:
 *      Returns VMK_OK on success, error otherwise. 
 *
 * Side effects:
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
HeapMgr_RequestAnyMem(uint32 size, void **startAddr, uint32 *regionLength)
{
   return HeapMgrRequestMem(&allocatorAnyMem, size, startAddr, regionLength);
}

/*
 *----------------------------------------------------------------------
 *
 * HeapMgr_RequestLowMem --
 *
 *      Wrapper for HeapMgrRequestMem to specify low memory.
 *
 * Results:
 *      Returns VMK_OK on success, error otherwise. 
 *
 * Side effects:
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
HeapMgr_RequestLowMem(uint32 size, void **startAddr, uint32 *regionLength)
{
   return HeapMgrRequestMem(&allocatorLowMem, size, startAddr, regionLength);
}

/*
 *----------------------------------------------------------------------
 *
 * HeapMgr_FreeAnyMem --
 *
 *      Wrapper for HeapMgrFree to specify "Any" allocator.
 *
 *      NOTE: Do NOT use this to free memory allocated with
 *      HeapMgr_RequestLowMem. RequestLowMem should be paired with FreeLowMem;
 *      RequestAnyMem should be paired with FreeAnyMem calls.
 *
 * Results:
 *      Returns VMK_OK if all went well, VMK_FAILURE otherwise.
 *
 * Side effects:
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
HeapMgr_FreeAnyMem(void *addr, uint32 size)
{
   return HeapMgrFreeMem(&allocatorAnyMem, addr, size);
}

/*
 *----------------------------------------------------------------------
 *
 * HeapMgr_FreeLowMem --
 *
 *      Wrapper for HeapMgrFree to specify "Low" allocator.
 *
 * Results:
 *      Returns VMK_OK if all went well, VMK_FAILURE otherwise.
 *
 * Side effects:
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
HeapMgr_FreeLowMem(void *addr, uint32 size)
{
   return HeapMgrFreeMem(&allocatorLowMem, addr, size);
}

