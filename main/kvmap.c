/* **********************************************************
 * Copyright 2000 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * kvmap.c --
 *
 *      This module manages a pool of virtual address space that can
 *	be used to map in machine pages for long periods of time.  Use
 *	the Kseg cache to map machine pages for short periods of time.
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "x86.h"
#include "vm_asm.h"
#include "vmkernel.h"
#include "vmnix_if.h"
#include "kvmap.h"
#include "memmap.h"
#include "tlb.h"
#include "memalloc.h"
#include "post.h"
#include "mtrr.h"
#include "mod_loader.h"
#include "buddy.h"
#include "util.h"

#define LOGLEVEL_MODULE KVMap
#define LOGLEVEL_MODULE_LEN 5
#include "log.h"

/*
 * Allocation information for dynamically allocated pages.
 */

// Info per virtual page frame
typedef struct KVMapFrame {
   MPN       mpn;
   Bool      guarded;
#ifdef VMX86_DEBUG
   Bool      free;
#endif
} KVMapFrame;

#define KVMAP_FRAME_FREE     0x0001
#define KVMAP_FRAME_FIRST    0x0002
#define KVMAP_FRAME_LAST     0x0004

// kvmap manages pages so the sizes are number of pages
#define KVMAP_MIN_SIZE_SHIFT (0)  // 4k 
#define KVMAP_MAX_SIZE_SHIFT (8)  // 1M

/*
 * This is the static state for the kernel virtual address manager.
 */

static struct KVMap {
   VPN           	firstVPN; 
   VPN           	lastVPN;
   uint32          	numVPNs;
   Buddy_Handle         buddyHandle;
   uint32               buddyMemSize;
   VA                   buddyMem;
   KVMapFrame    	*frame;	
} kvMap, *kv;

static unsigned char *postSharedPage; // used by KvmapPost

static Bool KvmapPost(void *clientData, int id, SP_SpinLock *lock, SP_Barrier *barrier);


/*
 *----------------------------------------------------------------------
 *
 * KVMapAddr2FrameIndex --
 *
 *    Utility function to convert the virtual address to a frame index
 *
 * Results:
 *    frame index into the KVMap_Frame
 *
 * Side effects:
 *    none.
 *
 *----------------------------------------------------------------------
 */
static INLINE uint32
KVMapAddr2FrameIndex(VA addr) 
{
   VPN vpn = VA_2_VPN(addr);
   // since first page is never used
   ASSERT(vpn > kv->firstVPN);
   ASSERT(vpn < (kv->firstVPN + kv->numVPNs));
   return (vpn - kv->firstVPN);
}


/*
 *----------------------------------------------------------------------
 *
 * KVMap_Init --
 *
 *      Initialize the kernel virtual address manager module. This
 *      module dynamically allocates vmkernel virtual
 *      address space and maps machine pages to virtual pages.
 *
 * Results: 
 *    VMK_OK on success, error code on failure
 *      
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
KVMap_Init(VA startAddr, uint32 length)
{
   uint32 i;
   VMK_ReturnStatus status;
   VPN startVPN;
   uint32 numVPNs;
   Buddy_StaticRangeInfo rangeInfo;
   Buddy_AddrRange addrRange; 

   kv = &kvMap;
   kv->firstVPN = VA_2_VPN(startAddr);
   kv->lastVPN = VA_2_VPN(startAddr + length - 1);

   kv->numVPNs = kv->lastVPN - kv->firstVPN + 1;

   /*
    * Do not use the first page.  This will prevent wayward loops 
    * iterating on heap pointers from reaching into sensitive kvmap 
    * entries like APIC and memory mapped IOs.
    */
   startVPN = kv->firstVPN + 1;
   numVPNs = kv->numVPNs - 1;

   strcpy(rangeInfo.name, "kvmap"); 
   rangeInfo.start = startVPN;
   rangeInfo.len = numVPNs;
   rangeInfo.minSize = 1 << KVMAP_MIN_SIZE_SHIFT;
   rangeInfo.maxSize = 1 << KVMAP_MAX_SIZE_SHIFT;
   rangeInfo.numColorBits = BUDDY_NO_COLORS;

   // Get mem required by the buddy allocator
   kv->buddyMemSize = Buddy_StaticRangeMemReq(&rangeInfo);
   ASSERT(kv->buddyMemSize > 0);
   if (UNLIKELY(kv->buddyMemSize == 0)) {
      Warning("failed to initialize, memory size is 0");
      return VMK_FAILURE;
   }

   // Allocate memory to be used by the buddy allocator
   kv->buddyMem = (VA)Mem_AllocEarly(kv->buddyMemSize, 4);

   addrRange.start = startVPN;
   addrRange.len = numVPNs;
   status = Buddy_CreateStatic(&rangeInfo, kv->buddyMemSize, (void *)kv->buddyMem, 
                               1, &addrRange, &kv->buddyHandle); 
   ASSERT(status == VMK_OK);
   if (UNLIKELY(status != VMK_OK)) {
      Warning("failed to initialize, status 0x%x", status);
      return status;
   }

   kv->frame = (KVMapFrame *)Mem_AllocEarly(kv->numVPNs * sizeof(KVMapFrame), 4);
   ASSERT(kv->frame);
   for (i = 0; i < kv->numVPNs; i++) {
      kv->frame[i].mpn = INVALID_MPN;
      DEBUG_ONLY(kv->frame[i].free = TRUE;)
   }

   POST_Register("kvmap", KvmapPost, NULL);
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * KVMapMapMPNsInt --
 *
 *      Map the requested pages into the vmkernel's address space.
 *	The machine pages are specified as a set of ranges.  The
 *	previous mapping is flushed on all CPUs.
 *
 * Results: 
 *      The virtual address where the pages were mapped in.
 *      
 * Side effects:
 *      The page table and other data structures are updated.
 *
 *----------------------------------------------------------------------
 */
static void *
KVMapMapMPNsInt(uint32 numMPNs, 
                 KVMap_MPNRange *ranges, 
                 uint32 numRanges, 
                 uint32 flags,
                 void* ra)
{
   VA addr;
   VPN allocPage;
   uint32 startNdx;
   uint32 i;
   uint32 pageInRange;
   KVMap_MPNRange *curRange;
   VMK_ReturnStatus status;
   World_ID wid = PRDA_GetRunningWorldIDSafe();
   Bool guarded = FALSE;
   uint32 numVPNs;

   // guard uncached mappings with unmapped pages on both sides
   if (CONFIG_OPTION(KVMAP_GUARD_UNCACHED) && (flags & TLB_UNCACHED)) {
      guarded = TRUE;
   }
   numVPNs = numMPNs + (guarded ? 2 : 0);

   LOG(1, "mapping %d MPNs %d VPNs flags=%d ra=%p",
       numMPNs, numVPNs, flags, ra);

   status = Buddy_Allocate(kv->buddyHandle, numVPNs,
                           wid, ra, &allocPage);
   if (status != VMK_OK) {
      // this should never happen...
      SysAlert("Out of kvmap entries (numMPNs=%d)", numMPNs);
      if (vmx86_debug) {
         Panic("resize kvmap\n");
      }
      return NULL;
   }
   addr = VPN_2_VA(allocPage);
   startNdx = KVMapAddr2FrameIndex(addr); 
   curRange = ranges;
   pageInRange = 0;

   for (i = 0; i < numVPNs; i++) {
      const uint32 ndx = startNdx + i;
      const VPN vpn = kv->firstVPN + ndx;
      uint32 curFlags;

      ASSERT(ndx < kv->numVPNs);
      ASSERT(kv->frame[ndx].free);
      ASSERT(kv->frame[ndx].mpn == INVALID_MPN);

      DEBUG_ONLY(kv->frame[ndx].free = FALSE);
      kv->frame[ndx].guarded = guarded;

      if (guarded && ((i == 0) || (i == numVPNs - 1))) {
         // guard page
         continue;
      }

      ASSERT(curRange - ranges < numRanges);
      ASSERT(pageInRange < curRange->numMPNs);

      kv->frame[ndx].mpn = curRange->startMPN + pageInRange;

      /*
       * If the MTRRs say this page is supposed to be uncached, even if
       * the caller is asking for a cached mapping, mark the page
       * uncached.  This indicates to vmkcore that it shouldn't dump this
       * page, and it shouldn't hurt anything.
       */
      curFlags = flags | (MTRR_IsUncachedMPN(kv->frame[ndx].mpn) ?  
                          TLB_UNCACHED : 0);
      if (UNLIKELY(numMPNs > 1)) {
         /*
          * Only validate locally because we are going to do a global 
          * TLB flush below.  I can't remember why we do it this way 
          * except that it is probably cheaper to flush the entire TLB 
          * once then send IPIs for each page and do mutiple page 
          * invalidates.  This has never been quantified. This function 
          * is not called very often anyway so performance doesn't 
          * matter.  -Mike
          */
         curFlags |= TLB_LOCALONLY;
      }

      TLB_Validate(vpn, kv->frame[ndx].mpn, curFlags);

      pageInRange++;
      if (pageInRange == curRange->numMPNs) {
         curRange++;
         pageInRange = 0;
      }
   }

   if (UNLIKELY((numMPNs > 1) && (numPCPUs > 1) && !(flags & TLB_LOCALONLY))) {
      /*
       * Flush the TLBs on all other CPUs 
       */   
      TLB_Flush(0);
   }

   if (guarded) {
      // skip over guard page
      addr += PAGE_SIZE;
   }
   ASSERT(((uint32) addr & (PAGE_SIZE - 1)) == 0);
   return (void*)addr;
}


/*
 *----------------------------------------------------------------------
 *
 * KVMap_MapMPNs --
 *
 *	See KVMapMapMPNsInt.  This just adds the appropriate return
 *	address.
 *
 * Results: 
 *      The virtual address where the pages were mapped in.
 *      
 * Side effects:
 *      The page table and other data structures are updated.
 *
 *----------------------------------------------------------------------
 */
void *
KVMap_MapMPNs(uint32 numMPNs, 
              KVMap_MPNRange *ranges, 
              uint32 numRanges, 
              uint32 flags)
{
   void* ra = __builtin_return_address(0);
   return KVMapMapMPNsInt(numMPNs, ranges, numRanges, flags, ra);
}


/*
 *----------------------------------------------------------------------
 *
 * KVMap_MapMPN --
 *
 *      Allocate one virtual page and use mpn as backing.
 *
 * Results: 
 *      On success a kernel virtual address, NULL on failure.
 *      
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void *
KVMap_MapMPN(MPN mpn, uint32 flags)
{
   void* ra = __builtin_return_address(0);
   KVMap_MPNRange range;
   range.startMPN = mpn;
   range.numMPNs = 1;
   /*
    * MPNs must be in the range 0 - 0xFFFF.FFFF>>PAGE_SHIFT,
    * (0xF.FFFF.FFFF>>PAGE_SHIFT if PAE is enabled)
    */

   ASSERT((mpn & 0xFF000000) == 0);
   return KVMapMapMPNsInt(1, &range, 1, flags, ra);
}


/*
 *----------------------------------------------------------------------
 *
 * KVMap_AllocVA -
 *
 *      Allocate numPages pages of virtual address space.
 *
 * Results: 
 *      A pointer to the allocated range of pages, NULL if 
 *      
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void *
KVMap_AllocVA(uint32 numPages)
{
   VA addr;
   VPN allocPage;
   uint32 startNdx;
   uint32 i;
   VMK_ReturnStatus status;
   void *ra = __builtin_return_address(0);
   World_ID wid = PRDA_GetRunningWorldIDSafe();

   LOG(1, "allocating %d pages of virtual address space, ra=%p",
       numPages, ra);

   status = Buddy_Allocate(kv->buddyHandle, numPages, 
                           wid, ra, &allocPage);
   if (status != VMK_OK) {
      Warning("Out of kvmap entries");
      return NULL;
   }
   addr = VPN_2_VA(allocPage);
   startNdx = KVMapAddr2FrameIndex(addr); 

   for (i = 0; i < numPages; i++) {
      DEBUG_ONLY(const uint32 ndx = startNdx + i);
      ASSERT(ndx < kv->numVPNs);
      ASSERT(kv->frame[ndx].free);
      ASSERT(kv->frame[ndx].mpn == INVALID_MPN);
      DEBUG_ONLY(kv->frame[ndx].free = FALSE);
   }
   return (void*)addr;
}

/*
 *----------------------------------------------------------------------
 *
 * KVMap_FreePages --
 *
 *      Free pages allocated with one of the following calls:
 *         KVMap_MapMPN or KVMap_MapMPNs or KVMap_AllocVA.
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
KVMap_FreePages(void *ptr)
{
   uint32 i;
   int count = 0;
   uint32 size;
   uint32 numPages;
   uint32 startNdx;
   VPN allocPage = VA_2_VPN((VA)ptr);

   // vaddr can't be first_addr because first page is never allocated
   ASSERT((VA)ptr > VMK_KVMAP_BASE);
   ASSERT((VA)ptr < VMK_KVMAP_BASE + VMK_KVMAP_LENGTH);

   startNdx = KVMapAddr2FrameIndex((VA)ptr);
   if (kv->frame[startNdx].guarded) {
      // for guarded mappings, move index back to include guard page
      allocPage--;
      startNdx--;
   }

   // Get number of pages to free
   size = Buddy_GetLocSize(kv->buddyHandle, allocPage);
   ASSERT(size);
   numPages = size;

   for (i = 0; i < numPages; i++) {
      const uint32 ndx = startNdx + i;
      const VPN vpn = kv->firstVPN + ndx;

      ASSERT(ndx < kv->numVPNs);
      kv->frame[ndx].mpn = INVALID_MPN;
      kv->frame[ndx].guarded = FALSE;
      ASSERT(kv->frame[ndx].free == FALSE);
      DEBUG_ONLY(kv->frame[ndx].free = TRUE);
      /* 
       * Invalidate from local TLB, other cpus will pick up the change at
       * the next TLB flush (vmm<->vmk or cos<->vmk boundary)
       */
      TLB_Invalidate(vpn, TLB_LOCALONLY);

      count++;
   }

   // Free the actual pages only after we have marked
   // our frame as free, see bug 31979 
   (void)Buddy_Free(kv->buddyHandle, allocPage);

   // we must have mapped at least 1 page
   ASSERT(count > 0);

   LOG(1, "freed %d pages, ra = %p", count, __builtin_return_address(0));
}


MPN
KVMap_VA2MPN(VA vaddr)
{
   MPN mpn;
   int i = KVMapAddr2FrameIndex(vaddr);

   /*
    * No need for locking as we know this virtual address has been
    * allocated and can't be stolen.
    */
   DEBUG_ONLY(ASSERT_BUG(7115, !(kv->frame[i].free)));

   mpn = kv->frame[i].mpn;
   ASSERT(mpn != INVALID_MPN);

   return mpn;
}


/*
 *----------------------------------------------------------------------
 *
 * KvmapPOSTCachedvsUnCached
 *
 *      Make sure that cached memory accesses are substantially faster
 *      than uncached.
 *
 * Results:
 *      FALSE if cached are not SPEED_FACTOR times faster than uncached.
 *
 * Side effects:
 * 	none.
 *
 *----------------------------------------------------------------------
 */

#define SPEED_FACTOR 2
Bool 
KvmapPOSTCachedvsUnCached(void)
{
   MPN mpn;
   char *cached,*uncached;
   TSCCycles start, end;
   int total_cached, total_uncached,i;
   volatile char dummy;

   //check caching
   mpn = MemMap_AllocAnyKernelPage();

   // use local mappings to reduce global invalidates
   cached = KVMap_MapMPN(mpn, TLB_LOCALONLY);
   ASSERT(cached != NULL);
   uncached = KVMap_MapMPN(mpn, TLB_UNCACHED | TLB_LOCALONLY);
   ASSERT(uncached != NULL);

   Util_ZeroPage(cached);

   for(i=0;i < PAGE_SIZE;i++) {
      dummy = cached[i];
   } 
   start = RDTSC();
   for(i=0;i < PAGE_SIZE;i++) {
      dummy = cached[i];
   }
   end = RDTSC();
   total_cached = end - start;

   for(i=0;i < PAGE_SIZE;i++) {
      dummy = uncached[i];
   }
   start = RDTSC();
   for(i=0;i < PAGE_SIZE;i++) {
      dummy = uncached[i];
   }
   end = RDTSC();
   total_uncached = end - start;

//   Log("*******cached = %d, uncached = %d",total_cached,total_uncached);


   KVMap_FreePages(cached);
   KVMap_FreePages(uncached);
   MemMap_FreeKernelPage(mpn);

   //uncached reads should be at least SPEED_FACTOR times slower than cached reads
   //(determined empirically.).
   return total_cached * SPEED_FACTOR < total_uncached;
}

/*
 *----------------------------------------------------------------------
 *
 * KvmapPost
 *
 *      Perform a power on test of Kvmap.
 *      Notes:  KVMap_AllocVA not tested (appears to be unused)
 *      	KVMap_MapMPNs not tested (probably should be)
 *
 * Results:
 *      FALSE if error detected, TRUE otherwise
 *
 * Side effects:
 *
 *----------------------------------------------------------------------
 */

static MPN sharedMPN;

static Bool
KvmapPost(UNUSED_PARAM(void *clientData),
          int id,
          UNUSED_PARAM(SP_SpinLock *lock),
          SP_Barrier *barrier)
{
   Bool success = TRUE;
   uint32 i;


   //time cached vs uncached memory access
   if(!KvmapPOSTCachedvsUnCached()) {
      Warning("Cached vs uncached reads failed");
      success = FALSE;
   }

   /*
    * Now map the same mpn on each processor using global mapping request
    * from CPU 0.  And make sure that we do get the same memory.
    */
   if(id == 0) {
      sharedMPN = MemMap_AllocAnyKernelPage();
      ASSERT(sharedMPN != INVALID_MPN);
      postSharedPage = (char *)KVMap_MapMPN(sharedMPN, 0);
   }
   SP_SpinBarrier(barrier);
   ASSERT(postSharedPage != NULL);
   for(i=id; i< PAGE_SIZE;i+= numPCPUs) {
      postSharedPage[i] = id;
   }
   SP_SpinBarrier(barrier);

   for(i=0; i < PAGE_SIZE; i++) {
      if(postSharedPage[i] != i % numPCPUs) {
	 Warning("Shared page test failed (i=%d)", i);
	 success = FALSE;
         break;
      }
   }

   //Check VA2MPN in a simple manner
   if(KVMap_VA2MPN((VA)postSharedPage) != sharedMPN) {
      Warning("VA2MPN test 1 failed");
      success=FALSE;
   }
   if(KVMap_VA2MPN((VA)(postSharedPage + PAGE_SIZE/2)) != sharedMPN) {
      Warning("VA2MPN test 2 failed");
      success=FALSE;
   }

   SP_SpinBarrier(barrier);

   if(id == 0)
   {
      KVMap_FreePages(postSharedPage);
      postSharedPage = NULL;
      MemMap_FreeKernelPage(sharedMPN);
   }

   return success;
}

/*
 *----------------------------------------------------------------------
 *
 * KVMap_NumEntriesUsed --
 *
 *      Get the number of KVMap entries that are currently being used.
 *
 * Results:
 *      Number of used kvmap entries
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
int
KVMap_NumEntriesUsed(void)
{
   return Buddy_GetNumUsedBufs(kv->buddyHandle);
}

/*
 *----------------------------------------------------------------------
 *
 * KVMap_NumEntriesFree --
 *
 *      Get the number of free KVMap entries.
 *
 * Results:
 *      Number of free kvmap entries
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
int 
KVMap_NumEntriesFree(void)
{
   return Buddy_GetNumFreeBufs(kv->buddyHandle);
}

/*
 *----------------------------------------------------------------------
 *
 * KVMap_DumpEntries --
 *
 *      Dumps the contents of the kvmap to the log.
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
KVMap_DumpEntries(void)
{
   Buddy_DumpEntries(kv->buddyHandle);
}
