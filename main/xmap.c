/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * xmap.c --
 *
 *      This module manages a LARGE pool of virtual address space that can
 *      be used to map in lots of machine pages for long periods of time.
 *      The main difference between XMap and KVMap is that XMap is meant
 *      for big virtual address ranges and therefore, is too big to dump on
 *      a PSOD. Also currently KVMap is present in the COS pagetable, while
 *      XMap is not, but this might be changing soon.
 *
 *      Because the page directory containing the XMap is shared by all
 *      vmkernel worlds, any changes to the page directory page (such as
 *      large page mappings) show up immediately on all worlds.
 *
 *      There is one lock for the alloc and free routines. The hope is
 *      that these functions are not called frequently.
 *
 *      Use of xmap:
 *        memmap.c
 *          Depends on the size of RAM in a machine, ~150MB for 64GB
 *	  world.c
 *	    Per process GDTs = 5 pages per world
 *	    Per-world NMI stack = 1 pages per world
 *	  sharedArea.c
 *	    32 pages per VM
 *	  vmkstats.c
 *	    Per-PCPU sample buffers plus global data structures.
 *	    Typically < 16MB, but may grow with planned extensions,
 *	    such as hierarchical vmm profiling, tagging, etc.
 *        TOE(vmk_impl.c ...)
 *          8-12 MB per TOE instance
 *          Given a maximum of 4 TOE instance, max usage = 48 MB
 *        user.c
 *          Per-cartel heaps.  At most 128cartels for now each using 128KB,
 *          so a total of 16MB.
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "x86.h"
#include "vm_asm.h"
#include "vmkernel.h"
#include "vmnix_if.h"
#include "kvmap.h"
#include "memmap.h"
#include "splock.h"
#include "post.h"
#include "libc.h"
#include "xmap.h"
#include "util.h"
#include "memalloc.h"
#include "buddy.h"
#include "dump.h"

#define LOGLEVEL_MODULE XMap
#include "log.h"


/*
 * The pagetables for the XMap region need to be mapped somewhere, so
 * using the first n entries of the XMap itself instead of creating
 * another mapping region just for XMap pagetables.
 *
 * XMap region size is laid out as following:
 *
 * page index:                           Description
 * 0:                                    Guard page
 * 1 -> VMK_NUM_XMAP_PDES:               XMap page tables
 * VMK_NUM_XMAP_PDES+1:                  Guard page
 * VMK_NUM_XMAP_PDES+2 -> XMAP_PTES:  by others
 *
 */

#define XMAP_INDEX_PTABLES_START 1 // 0 is guard page
#define XMAP_INDEX_USER_START   (VMK_NUM_XMAP_PDES + 2) // 2 guards
#define XMAP_PTES               (VMK_NUM_XMAP_PDES*VMK_PTES_PER_PDE)

/*
 * For large pages we store three things in the xmapPTables.  At the first
 * index, a marker indicating large page mapping: XMAP_LARGEPAGE_SENTINEL
 * (which can be anything as long as PTE_P is false).
 * Then we store the large page mpn and the original pagetable mpn at the
 * offsets defined below.
 */
#define XMAP_LARGEPAGE_SENTINEL    PTE_PS
#define XMAP_LARGEPAGE_OFFSET      1
#define XMAP_LARGEPAGETABLE_OFFSET 2

#define XMAP_MIN_ALLOCATION_SIZE (2*PAGE_SIZE) // data + guard
// max set at 256MB to accomodate memmap buddy allocator
// running on machines with 64GB RAM, see PR 43372 for details
#define XMAP_MAX_ALLOCATION_SIZE (256*1024*1024)

static VMK_PDE *xmapPTables;
static Buddy_Handle xmapBuddyHandle;

static Bool XMapPost(void *clientData, int id, SP_SpinLock *lock, SP_Barrier *barrier);
static void *XMapMapAtIndex(uint32 startIndex, uint32 nPages,
                            XMap_MPNRange *ranges, int numRanges);

// utility functions to convert from xmap index to VA, LA and vice versa
static INLINE VA
XMapIndexToVA(uint32 index)
{
   return VMK_FIRST_XMAP_ADDR + index*PAGE_SIZE;
}

static INLINE LA
XMapIndexToLA(uint32 index)
{
   return VMK_VA_2_LA(XMapIndexToVA(index));
}

static INLINE uint32
XMapVAToIndex(VA vaddr)
{
   return (vaddr - VMK_FIRST_XMAP_ADDR)/PAGE_SIZE;
}

/*
 *----------------------------------------------------------------------
 *
 * XMap_Init --
 *
 *      Initialize the XMap region and add it to the current page table.
 *
 * Results: 
 *      VMK_OK on success
 *      
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
XMap_Init(void)
{
   uint32 i;
   Buddy_StaticRangeInfo rangeInfo;
   Buddy_AddrRange addrRange; 
   uint32 buddyMemSize, buddyMemPages;
   XMap_MPNRange *buddyMPNs;
   char *buddyMemAddr;
   VMK_ReturnStatus status;

   /*
    * Since XMap pagetables reside in the XMap itself, the bootstrapping
    * problem is addressed by using KVMap for the first XMap pagetable.  As
    * soon as the first XMap pde is initialized, we switch over to using
    * XMap to map the pagetables.
    */
   for (i = 0; i < VMK_NUM_XMAP_PDES; i++) {
      MPN mpn;
      VMK_PDE *pTable;

      mpn = MemMap_EarlyAllocPage(MM_TYPE_ANY);
      ASSERT_NOT_IMPLEMENTED(mpn != INVALID_MPN);

      pTable = KVMap_MapMPN(mpn, TLB_LOCALONLY);
      ASSERT_NOT_IMPLEMENTED(pTable != NULL);
      Util_ZeroPage(pTable);

      LOG(2, "mapping page 0x%x at index %d", mpn, XMAP_INDEX_PTABLES_START + i);
      // map this page in the part of xmap region used to manage xmap page tables
      if (i == 0) {
         // first page special since xmapPTables hasn't been initialized yet
         PT_SET(&pTable[XMAP_INDEX_PTABLES_START], VMK_MAKE_PTE(mpn, 0, PTE_KERNEL));
         xmapPTables = (VMK_PDE *) XMapIndexToVA(XMAP_INDEX_PTABLES_START);
      } else {
         PT_SET(&xmapPTables[XMAP_INDEX_PTABLES_START + i], 
                VMK_MAKE_PTE(mpn, 0, PTE_KERNEL));
      }
      TLB_INVALIDATE_PAGE(XMapIndexToVA(XMAP_INDEX_PTABLES_START + i));

      KVMap_FreePages(pTable);

      // add pde for this pagetable page to all pagetables
      PT_AddPageTable(XMapIndexToLA(i*VMK_PTES_PER_PDE), mpn);
   }

   memset(&rangeInfo, 0, sizeof rangeInfo);
   strcpy(rangeInfo.name, "xmap"); 
   rangeInfo.minSize = BYTES_2_PAGES(XMAP_MIN_ALLOCATION_SIZE);
   rangeInfo.maxSize = BYTES_2_PAGES(XMAP_MAX_ALLOCATION_SIZE);
   rangeInfo.start = ROUNDUP(XMAP_INDEX_USER_START, rangeInfo.minSize);
   rangeInfo.len = XMAP_PTES - rangeInfo.start;
   rangeInfo.numColorBits = BUDDY_NO_COLORS;

   buddyMemSize = Buddy_StaticRangeMemReq(&rangeInfo);
   ASSERT_NOT_IMPLEMENTED(buddyMemSize > 0);

   Log("Allocating %d bytes for allocator [%x,%x]", buddyMemSize,
       rangeInfo.start, rangeInfo.start + rangeInfo.len);

   // we allocate buddy overhead memory from memmap and map it using Xmap itself
   buddyMemPages = CEIL(buddyMemSize, PAGE_SIZE);
   buddyMPNs = (XMap_MPNRange *) Mem_Alloc(buddyMemPages * sizeof (XMap_MPNRange));
   ASSERT_NOT_IMPLEMENTED(buddyMPNs != NULL);
   for (i = 0; i < buddyMemPages; i++) {
      buddyMPNs[i].startMPN = MemMap_EarlyAllocPage(MM_TYPE_ANY);
      buddyMPNs[i].numMPNs = 1;
   }
   buddyMemAddr = XMapMapAtIndex(XMAP_INDEX_USER_START, 
                                 buddyMemPages, buddyMPNs, buddyMemPages);
   ASSERT_NOT_IMPLEMENTED(buddyMemAddr != NULL);

   Mem_Free(buddyMPNs);

   /*
    * we need to skip over the xmap entries that are used for buddy
    * overhead.  This slightly reduces the amount of memory buddy needs,
    * but probably not worth recalculating buddy overhead.
    * Also, leave 1 guard page after buddy overhead mapping.
    */
   rangeInfo.start = ROUNDUP(XMAP_INDEX_USER_START + buddyMemPages + 1,
                             rangeInfo.minSize);
   rangeInfo.len =  XMAP_PTES - rangeInfo.start;
   Log("Range reduced to [%x,%x]", rangeInfo.start, rangeInfo.start + rangeInfo.len);

   addrRange.start = rangeInfo.start;
   addrRange.len = rangeInfo.len;
   status = Buddy_CreateStatic(&rangeInfo, buddyMemSize, buddyMemAddr, 
                               1, &addrRange, &xmapBuddyHandle); 
   if (status != VMK_OK) {
      return status;
   }

   POST_Register("xmap", XMapPost, NULL);
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * XMap_LateInit --
 *
 *      Protect the XMap page tables from random IO to them
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
XMap_LateInit(void)
{
   uint32 i;
   for (i = 0; i < VMK_NUM_XMAP_PDES; i++) {
      MPN mpn;
      mpn = VMK_PTE_2_MPN(xmapPTables[i + XMAP_INDEX_PTABLES_START]);
      LOG(2, "mapping page 0x%x at index %d", mpn, i);
      MemMap_SetIOProtection(mpn, MMIOPROT_IO_DISABLE);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * XMapAllocate --
 *
 *      Find a free contiguous region nPages long and allocate it
 *
 * Results: 
 *      The starting index of the newly allocated region
 *      
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
XMapAllocate(int nPages, uint32 *index)
{
   VMK_ReturnStatus status;
   void *ra = __builtin_return_address(1);
   uint32 wid = PRDA_GetRunningWorldIDSafe();

   status = Buddy_Allocate(xmapBuddyHandle, nPages, 
                           wid, ra, index);
   LOG(1, "index = %x, len=%x  status=%d ra=%p",
       *index, nPages, status, ra);

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * XMapFree --
 *
 *      Free the given contiguous nPages long region
 *
 * Results: 
 *      None
 *      
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
XMapFree(uint32 nPages, uint32 index)
{
   uint32 freeSize;
   LOG(1, "index = %x, len=%x  ra=%p",
       index, nPages, __builtin_return_address(1));

   freeSize = Buddy_Free(xmapBuddyHandle, index);
   ASSERT(freeSize >= nPages);
}

/*
 *----------------------------------------------------------------------
 *
 * XMapMapLargePage --
 *
 *      Maps the given large page at the given XMap index, and returns the
 *      MPN of the pagetable page that used to be there
 *
 * Results:
 *      MPN of the pagetable page that was at this index, or INVALID_MPN
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static MPN
XMapMapLargePage(uint32 index, MPN largePageMPN)
{
   MA cr3;
   VMK_PDE *pageDir, *pdePtr;
   LA laddr = XMapIndexToLA(index);
   MPN pageTable;

   ASSERT((laddr % PDE_SIZE) == 0);
   ASSERT((largePageMPN % VMK_PTES_PER_PDE) == 0);

   GET_CR3(cr3);
   pageDir = PT_GetPageDir(cr3, laddr, NULL);
   if (pageDir == NULL) {
      return INVALID_MPN;
   }

   pdePtr = &pageDir[ADDR_PDE_BITS(laddr)];
   ASSERT(PTE_PRESENT(*pdePtr));
   ASSERT(!(*pdePtr & PTE_PS));
   pageTable = VMK_PDE_2_MPN(*pdePtr);
   PT_SET(pdePtr, VMK_MAKE_PDE(largePageMPN, 0, PTE_KERNEL|PTE_PS));
   PT_ReleasePageDir(pageDir, NULL);

   return pageTable;
}

#ifdef VMX86_DEBUG
/*
 *----------------------------------------------------------------------
 *
 * XMapGetLargePage --
 *
 *      Get the large page mapped at given XMap Index
 *
 * Results:
 *      MPN of the large page, or INVALID_MPN
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static MPN
XMapGetLargePage(uint32 index)
{
   MA cr3;
   VMK_PDE *pageDir, *pdePtr;
   LA laddr = XMapIndexToLA(index);
   MPN largePageMPN = INVALID_MPN;

   ASSERT((laddr % PDE_SIZE) == 0);

   GET_CR3(cr3);
   pageDir = PT_GetPageDir(cr3, laddr, NULL);
   if (pageDir == NULL) {
      return INVALID_MPN;
   }

   pdePtr = &pageDir[ADDR_PDE_BITS(laddr)];

   ASSERT(PTE_PRESENT(*pdePtr));
   ASSERT(*pdePtr & PTE_PS);

   largePageMPN = VMK_PTE_2_MPN(*pdePtr);
   PT_ReleasePageDir(pageDir, NULL);

   return largePageMPN;
}
#endif

/*
 *----------------------------------------------------------------------
 *
 * XMapMapAtIndex
 *
 *      Map the given list of MPNs at the given xmap index
 *
 * Results: 
 *      virtual address for the region
 *      
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void *
XMapMapAtIndex(uint32 startIndex, uint32 nPages, XMap_MPNRange *ranges, int numRanges)
{
   uint32 index;
   int pageInRange = 0;
   XMap_MPNRange *curRange = ranges;

   if (vmx86_debug) {
      uint32 i;
      for (i = 0; i < nPages; i++) {
         ASSERT(!PTE_PRESENT(xmapPTables[startIndex + i]));
      }
   }

   index = startIndex;
   while (index < startIndex + nPages) {
      uint32 mappedPages;
      MPN mpn = curRange->startMPN + pageInRange;

      ASSERT(curRange - ranges < numRanges);
      // check if we can use a large page
      if (((index + VMK_PTES_PER_PDE) <= (startIndex + nPages)) &&
          ((pageInRange + VMK_PTES_PER_PDE) <= curRange->numMPNs) &&
          ((index % VMK_PTES_PER_PDE) == 0) &&
          ((mpn % VMK_PTES_PER_PDE) == 0)) {
         MPN pageTableMPN;

         pageTableMPN = XMapMapLargePage(index, mpn);
         if (pageTableMPN == INVALID_MPN) {
            return NULL;
         }
         /*
          * xmapPTables[index..index+VMK_PTES_PER_PDE] are no longer PTEs
          * because we just mapped a large page where they used to map
          * small pages.  So I can store whatever I want in there.  So,
          * storing indicator that we're using large pages, the large page
          * MPN and the page table MPN.
          *
          * XMap_VA2MPN checks the present bit in xmapPTables to check if
          * small or large page, so can't have the present bit be true for
          * the values stored in XMAP_LARGEPAGE_OFFSET and
          * XMAP_LARGEPAGETABLE_OFFSET, so instead of directly storing MPN,
          * we store mpn shifted into a pte.
          */
         xmapPTables[index] = XMAP_LARGEPAGE_SENTINEL;
         xmapPTables[index+XMAP_LARGEPAGE_OFFSET] = VMK_MAKE_PTE(mpn, 0, 0);
         xmapPTables[index+XMAP_LARGEPAGETABLE_OFFSET] = VMK_MAKE_PTE(pageTableMPN, 0, 0);
         mappedPages = VMK_PTES_PER_PDE;
         LOG(3, "mapping large page 0x%x at index %d", mpn, index);
      } else {
         PT_SET(&xmapPTables[index], VMK_MAKE_PTE(mpn, 0, PTE_KERNEL));
         mappedPages = 1;
         LOG(3, "mapping page 0x%x at index %d", mpn, index);
      }

      pageInRange += mappedPages;
      ASSERT(pageInRange <= curRange->numMPNs);
      if (pageInRange == curRange->numMPNs) {
         curRange++;
         pageInRange = 0;
      }

      index += mappedPages;
      ASSERT(index <= startIndex + nPages);
   }
   ASSERT(!PTE_PRESENT(xmapPTables[startIndex + nPages])); // guard page

   /*
    * We don't need to flush any TLB entries here because we flush when
    * removing a mapping, and TLB can't cache a not-present mapping.  We
    * invalidate on unmap instead of map to catch use-after-free.
    */

   return (void*)XMapIndexToVA(startIndex);
}

/*
 *----------------------------------------------------------------------
 *
 * XMap_Map --
 *
 *      Allocate some XMap space, map the given list of MPNs, and return
 *      the virtual address
 *
 * Results: 
 *      virtual address for the region
 *      
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void *
XMap_Map(uint32 nPages, XMap_MPNRange *ranges, int numRanges)
{
   uint32 startIndex;

   LOG(1, "%d pages in %d ranges from ra %p",
       nPages, numRanges, __builtin_return_address(0));

   /*
    * Allocating space for one guard page to leave a hole to catch out of
    * bounds accesses.
    */
   if (XMapAllocate(nPages + 1, &startIndex) != VMK_OK) {
      ASSERT(FALSE);
      return NULL;
   }

   return XMapMapAtIndex(startIndex, nPages, ranges, numRanges);
}

/*
 *----------------------------------------------------------------------
 *
 * XMap_Unmap --
 *
 *      Unmap the given virtual address mapping.  nPages specifies the
 *      length of the region and it must match the number of pages given
 *      when the range was mapped.
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
XMap_Unmap(uint32 nPages, void *ptr)
{
   uint32 index, startIndex;
   VA vaddr = (VA)ptr;

   ASSERT(World_IsSafeToBlock());

   LOG(1, "%d pages from ra %p",
       nPages, __builtin_return_address(0));

   ASSERT(PAGE_OFFSET(vaddr) == 0);

   startIndex = XMapVAToIndex(vaddr);

   index = startIndex;
   while (index < startIndex + nPages) {
      uint32 handledPages;

      // vaddr can't be first_addr because first page is never allocated
      ASSERT(vaddr > VMK_FIRST_XMAP_ADDR);
      ASSERT(vaddr < VMK_FIRST_XMAP_ADDR + VMK_NUM_XMAP_PDES*PDE_SIZE);

      if (!PTE_PRESENT(xmapPTables[index])) {
         MPN pageTableMPN;

         // this must be a large page
         ASSERT(xmapPTables[index] == XMAP_LARGEPAGE_SENTINEL);
         ASSERT((index % VMK_PTES_PER_PDE) == 0);
         ASSERT((index + VMK_PTES_PER_PDE) <= startIndex + nPages);
         ASSERT(XMapGetLargePage(index) == 
                VMK_PTE_2_MPN(xmapPTables[index+XMAP_LARGEPAGE_OFFSET]));
         LOG(3, "unmapping large page 0x%Lx at index %d", 
             VMK_PTE_2_MPN(xmapPTables[index+XMAP_LARGEPAGE_OFFSET]), index);
         pageTableMPN = VMK_PTE_2_MPN(xmapPTables[index+XMAP_LARGEPAGETABLE_OFFSET]);

         /*
          * Map the original page table that used to be there instead of
          * the large page.  But first, need to make
          * xmapPTables[index..index+XMAP_LARGEPAGETABLE_OFFSET] look like
          * PTEs again.
          */
         PT_INVAL(&xmapPTables[index]);
         PT_INVAL(&xmapPTables[index+XMAP_LARGEPAGE_OFFSET]);
         PT_INVAL(&xmapPTables[index+XMAP_LARGEPAGETABLE_OFFSET]);
         PT_AddPageTable(XMapIndexToLA(index), pageTableMPN);

         handledPages = VMK_PTES_PER_PDE;
      } else {
         ASSERT(!(xmapPTables[index] & PTE_PS));
         LOG(3, "unmapping page 0x%x at index %d", 
             (MPN)VMK_PTE_2_MPN(xmapPTables[index]), index);
         PT_INVAL(&xmapPTables[index]);

         handledPages = 1;
      }

      index += handledPages;
      ASSERT(index <= startIndex + nPages);
      vaddr += handledPages*PAGE_SIZE;
   }
   ASSERT(!PTE_PRESENT(xmapPTables[startIndex + nPages])); //guard page

   /*
    * XMap users usually map big ranges that indual invalpg is not useful
    * so doing a global flush.  Currently map/unmaps are infrequent, but if
    * this changes, we could remove the TLB_Flush by not reusing XMap
    * entries until we use up the entire range, and then flush once.
    */
   TLB_Flush(0);

   XMapFree(nPages + 1, startIndex);
}

/*
 *----------------------------------------------------------------------
 *
 * XMap_VA2MPN --
 *
 *      Find and return the machine page for the given virtual address
 *
 * Results: 
 *      MPN corresponding to the given vaddr
 *      
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
MPN
XMap_VA2MPN(VA vaddr)
{
   uint32 index;
   MPN mpn;

   ASSERT(vaddr > VMK_FIRST_XMAP_ADDR);
   ASSERT(vaddr < VMK_FIRST_XMAP_ADDR + VMK_NUM_XMAP_PDES*PDE_SIZE);

   index = XMapVAToIndex(vaddr);

   // present bit tells us if it's part of a large or small mapping
   if (PTE_PRESENT(xmapPTables[index])) {
      //small page map
      mpn = VMK_PTE_2_MPN(xmapPTables[index]);
   } else {
      // large page map
      uint32 offset = (vaddr & (PDE_SIZE - 1)) >> PAGE_SHIFT;
      index = XMapVAToIndex((vaddr & ~(PDE_SIZE - 1)));
      ASSERT(PTE_LARGEPAGE(xmapPTables[index]));
      mpn = (VMK_PTE_2_MPN(xmapPTables[index+XMAP_LARGEPAGE_OFFSET]) + offset);
   }

   ASSERT(VMK_IsValidMPN(mpn));
   return mpn;
}

/*
 *----------------------------------------------------------------------
 *
 * XMapPost --
 *
 *      A quick self-test for XMap module.  This test allocates a page,
 *      maps it using XMap and write a string to it.  Then, maps the same
 *      page using KVmap and verifies it gets the same string back.
 *
 * Results: 
 *      TRUE on successful test, FALSE otherwise
 *      
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static Bool
XMapPost(UNUSED_PARAM(void *clientData),
         UNUSED_PARAM(int id),
         UNUSED_PARAM(SP_SpinLock *lock),
         UNUSED_PARAM(SP_Barrier *barrier))
{
#define XMAP_POST_TESTPAGES 2
   XMap_MPNRange xRange[XMAP_POST_TESTPAGES];
   KVMap_MPNRange kvRange[XMAP_POST_TESTPAGES];
   Bool success = TRUE;
   uint32 *ptr;
   uint32 i;

   for (i = 0; i < XMAP_POST_TESTPAGES; i++) {
      xRange[i].numMPNs = 1;
      xRange[i].startMPN = MemMap_AllocAnyKernelPage();
      kvRange[i].numMPNs = 1;
      kvRange[i].startMPN = xRange[i].startMPN;
   }

   ptr = XMap_Map(XMAP_POST_TESTPAGES, xRange, XMAP_POST_TESTPAGES);
   for (i = 0;
        i < (XMAP_POST_TESTPAGES*PAGE_SIZE)/sizeof(uint32);
        i++) {
      ptr[i]=i;
   }
   XMap_Unmap(XMAP_POST_TESTPAGES, ptr);

   ptr = KVMap_MapMPNs(XMAP_POST_TESTPAGES, kvRange,
                       XMAP_POST_TESTPAGES, TLB_LOCALONLY);
   for (i = 0;
        i < (XMAP_POST_TESTPAGES*PAGE_SIZE)/sizeof(uint32);
        i++) {
      ASSERT(ptr[i] == i);
   }
   KVMap_FreePages(ptr);

   for (i = 0; i < XMAP_POST_TESTPAGES; i++) {
      MemMap_FreeKernelPage(xRange[i].startMPN);
   }

   return success;
}

/*
 *----------------------------------------------------------------------
 *
 * XMapDumpPDE --
 *
 *      Dumps the given xmap pde.  XMap is a large area that is usually
 *      unmapped, so to conserve dump space we only dump the regions that
 *      are actully mapped.  We note down which pages are dumped in a
 *      bitmap per 2MB region (PDE).
 *
 * Results:
 *      VMK_ReturnStatus
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
XMapDumpPDE(int pdeNum)
{
   static Bool bitmap[VMK_PTES_PER_PDE];
   int index = pdeNum*VMK_PTES_PER_PDE;
   int i;
   Bool dumpingLargePage;
   VA va;
   VMK_ReturnStatus status;

   if ((xmapPTables[index] == XMAP_LARGEPAGE_SENTINEL) &&
       VMK_IsValidMPN(VMK_PTE_2_MPN(xmapPTables[index+XMAP_LARGEPAGE_OFFSET]))) {
      dumpingLargePage = TRUE;
   } else {
      dumpingLargePage = FALSE;
   }

   // generate and dump the bitmap
   for (i = 0; i < VMK_PTES_PER_PDE; i++) {
      index = pdeNum*VMK_PTES_PER_PDE + i;

      if (dumpingLargePage ||
          (PTE_PRESENT(xmapPTables[index]) &&
           VMK_IsValidMPN(VMK_PTE_2_MPN(xmapPTables[index])))) {
         bitmap[i] = TRUE;
      } else {
         bitmap[i] = FALSE;
      }
   }

   status = Dump_Range((VA)bitmap, sizeof bitmap, "XMap bitmap");
   if (status != VMK_OK) {
      return status;
   }

   // dump the pages that are mapped
   for (i = 0; i < VMK_PTES_PER_PDE; i++) {
      index = pdeNum*VMK_PTES_PER_PDE + i;
      va = XMapIndexToVA(index);

      if (bitmap[i]) {
         status = Dump_Page(va, "XMap");
         if (status != VMK_OK) {
            return status;
         }
      }
   }

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * XMap_Dump --
 *
 *      Dumps the xmap region to the coredump.
 *
 * Results:
 *      VMK_ReturnStatus
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
XMap_Dump(void)
{
   VMK_ReturnStatus status;
   int i;

   for (i = 0; i < VMK_NUM_XMAP_PDES; i++) {
      status = XMapDumpPDE(i);
      if (status != VMK_OK) {
         return status;
      }
   }

   return VMK_OK;
}
