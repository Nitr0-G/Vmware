/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * pagetable.c -
 *
 *   This module provides PAE-mode-independent functions to manage the
 *   pagetables so the rest of the code doesn't need to be aware of
 *   PAE-mode.
 *
 *   PageRoot is the top level of the page table.  The alloc and copy
 *   page root routines allocate both the root and the 4 pagedirs for
 *   the PAE mode page tables.
 *
 *   PageDir is the next level of page table.  These are the page
 *   directory pages, they contain references to the page tables, or
 *   to large pages (2Mb in PAE page tables).
 *
 *   PageTable is the bottom level of the page table.  This contains
 *   references to the 4k pages.
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "x86.h"
#include "vm_asm.h"
#include "vmkernel.h"
#include "kvmap.h"
#include "kseg.h"
#include "memmap.h"
#include "pagetable.h"
#include "util.h"

#define LOGLEVEL_MODULE PT
#include "log.h"

#define PDIR_SHARED    1
#define PDIR_EXCLUSIVE 0

static VMK_PDE* PTAllocPageDir(VMK_PDPTE* pageRoot, LA addr);
static void* PTAllocPage(MPN* outMPN, KSEG_Pair **pair, Bool lowPage);

/*
 * Map the given machine addr and return virtual address.
 * Use Kseg if pair is not NULL, otherwise KVmap.
 */
static INLINE void *
PTMapPage(MA ma, KSEG_Pair **pair)
{
   MPN mpn = MA_2_MPN(ma);

   if (pair) {
      return ((char*)Kseg_MapMPN(mpn, pair)) + ADDR_PGOFFSET_BITS(ma);
   } else {
      return ((char*)KVMap_MapMPN(mpn, TLB_LOCALONLY)) + ADDR_PGOFFSET_BITS(ma);
   }
}

static INLINE void
PTUnmapPage(void *ptr, KSEG_Pair *pair)
{
   if (pair) {
      Kseg_ReleasePtr(pair);
   } else {
      KVMap_FreePages(ptr);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * PTAllocPage --
 *
 *	Allocate a machine page to be used for part of the page table
 *	hierarchy.  Disable IO permissions on it, map it, and zero it.
 *	Must be released with the appropriate PT_Release call.  outMPN
 *	is required.  Allocates a low page if lowPage is TRUE
 *
 * Results:
 *      Pointer to a new page or NULL. outMPN is the MPN of the page
 *      or INVALID_MPN.
 *
 * Side effects:
 *      Page is allocated, IO permissions on page are disabled, page
 *      is zero'd.
 *
 *----------------------------------------------------------------------
 */
static INLINE void*
PTAllocPage(MPN* outMPN, KSEG_Pair **pair, Bool lowPage)
{
   void* page;
   MPN tableMPN;

   tableMPN = MemMap_AllocKernelPage(MM_NODE_ANY, MM_COLOR_ANY,
                                     lowPage ? MM_TYPE_LOWRESERVED : MM_TYPE_ANY);

   if (tableMPN != INVALID_MPN) {
      MemMap_SetIOProtection(tableMPN, MMIOPROT_IO_DISABLE);
      page = PTMapPage(MPN_2_MA(tableMPN), pair);
      if (page != NULL) {
         Util_ZeroPage(page);
      } else {
         MemMap_FreeKernelPage(tableMPN);
         tableMPN = INVALID_MPN;
      }
   } else {
      page = NULL;
   }

   ASSERT(outMPN);
   *outMPN = tableMPN;

   /* page and tableMPN must be consistent */
   ASSERT(((page != NULL) && (tableMPN != INVALID_MPN))
          || ((page == NULL) && (tableMPN == INVALID_MPN)));

   return page;
}


/*
 *----------------------------------------------------------------------
 *
 * PT_AllocPageRoot --
 *
 *      Create a new pageroot and return a pointer to it.  Also setup the
 *      VMK_NUM_PDPTES (4) pagedirs for the new root.  If the MPN for the
 *      first page dir is given, use it instead of allocating a new one.
 *
 * Results:
 *      Pointer to pageroot, which must be released using PT_ReleasePageRoot
 *	Provides MA of the page root in "pPTRootMA" if its not NULL.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_PDPTE *
PT_AllocPageRoot(MA *pPTRootMA, MPN firstPageDir)
{
   MPN pageRootMPN;
   MA pageRootMA;
   VMK_PDPTE *pageRoot;
   int i;

   pageRoot = PTAllocPage(&pageRootMPN, NULL, TRUE);
   if (pageRoot == NULL) {
      return NULL;
   }

   ASSERT(IsLowMPN(pageRootMPN));
   pageRootMA = MPN_2_MA(pageRootMPN);

   for (i = 0; i < VMK_NUM_PDPTES; i++) {
      if ((i == 0) && (firstPageDir != INVALID_MPN)) {
         PT_SET(&pageRoot[0], MAKE_PDPTE(firstPageDir, PDIR_SHARED, PDPTE_FLAGS));
      } else {
         VMK_PDE *pageDir = PTAllocPageDir(pageRoot,
                                           PTBITS_ADDR(i, 0, 0));
         if (pageDir == NULL) {
            PTUnmapPage(pageRoot, NULL);
            PT_FreePageRoot(pageRootMA);
            return NULL;
         }
         PT_ReleasePageDir(pageDir, NULL);
      }
   }

   if (pPTRootMA) {
      *pPTRootMA = pageRootMA;
   }
   return pageRoot;
}

/*
 *----------------------------------------------------------------------
 *
 * PT_ReleasePageRoot --
 *
 *      Release resources that were being used for mapping the pageRoot
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
PT_ReleasePageRoot(VMK_PDPTE *pageRoot)
{
   PTUnmapPage(pageRoot, NULL);
}

/*
 *----------------------------------------------------------------------
 *
 * PT_CopyPageRoot --
 *
 *      Create a new pageroot and deep copy the contents of the old
 *      page root (i.e., copy the page directories).
 *      If firstPageDir is specified, then use that as the first page
 *      directory MPN.
 *
 * Results:
 *      Pointer to the new pageroot, which must be released using
 *      PT_ReleasePageRoot.  NULL if an error occurred.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_PDPTE *
PT_CopyPageRoot(MA srcPageRootMA, MA *pDestPageRootMA, MPN firstPageDir)
{
   MA destPageRootMA;
   VMK_PDPTE *destPageRoot;
   VMK_PDE *dest, *src;
   int i;

   destPageRoot = (VMK_PDPTE*)PT_AllocPageRoot(&destPageRootMA, firstPageDir);
   if (destPageRoot == NULL) {
      return NULL;
   }

   for (i = 0; i < VMK_PDES_PER_PDPTE; i++) {
      if ((i == 0) && (firstPageDir != INVALID_MPN)) {
         continue;
      }
      src = PT_GetPageDir(srcPageRootMA, PTBITS_ADDR(i, 0, 0), NULL);
      if (src == NULL) {
         PT_ReleasePageRoot(destPageRoot);
         PT_FreePageRoot(destPageRootMA);
         return NULL;
      }
      dest = PT_GetPageDir(destPageRootMA, PTBITS_ADDR(i, 0, 0), NULL);
      if (dest == NULL) {
         PT_ReleasePageDir(src, NULL);
         PT_ReleasePageRoot(destPageRoot);
         PT_FreePageRoot(destPageRootMA);
         return NULL;
      }
      memcpy(dest, src, PAGE_SIZE);
      PT_ReleasePageDir(src, NULL);
      PT_ReleasePageDir(dest, NULL);
   }

   *pDestPageRootMA = destPageRootMA;
   return destPageRoot;
}

/*
 *----------------------------------------------------------------------
 *
 * PT_FreePageRoot --
 *
 *      Free the pageroot page including 4 pdirs (except the first page
 *      directory if it's marked shared).
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
PT_FreePageRoot(MA pageRootMA)
{
   int i;
   VMK_PDPTE *pageRoot = (VMK_PDPTE*)PTMapPage(pageRootMA, NULL);
   if (pageRoot == NULL) {
      return;
   }

   for (i = 0; i < VMK_NUM_PDPTES; i++) {
      if (PTE_PRESENT(pageRoot[i])) {
         if (PTE_AVAIL(pageRoot[i]) == PDIR_SHARED) {
            // first page directory page is shared with all vmkernel
            // pagetables so don't free it.
            ASSERT(i == 0);
            continue;
         }
         MemMap_FreeKernelPage(VMK_PTE_2_MPN(pageRoot[i]));
      }
   }
   PTUnmapPage(pageRoot, NULL);
   MemMap_FreeKernelPage(MA_2_MPN(pageRootMA));
}


/*
 *----------------------------------------------------------------------
 *
 * PT_GetPageDir --
 *
 *      Get a pointer to the pagedir that maps the given linear address
 *      Use kseg to map the pagedir if pair is not NULL, otherwise KVMap
 *
 * Results:
 *      Pointer to pagedir, which must be released using ReleasePageDir
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_PDE *
PT_GetPageDir(MA pageRootMA, LA laddr, KSEG_Pair **pair)
{
   VMK_PDPTE *pageRoot;
   VMK_PDE *pageDir = NULL;
   KSEG_Pair *pairRoot = NULL;
   
   if (pageRootMA == 0) {
      return NULL;
   }

   pageRoot = (VMK_PDPTE *)PTMapPage(pageRootMA, pair);
   // we just used pair to map the root instead of the dir, so move
   // the pair info to pairRoot
   if (pair) {
      pairRoot = *pair;
      *pair = NULL;
   }
   if (pageRoot == NULL) {
      return NULL;
   }
   if (PTE_PRESENT(pageRoot[ADDR_PDPTE_BITS(laddr)])) {
      pageDir = (VMK_PDE *)PTMapPage(MPN_2_MA(VMK_PTE_2_MPN(pageRoot[ADDR_PDPTE_BITS(laddr)])),
				     pair);
   }
   PTUnmapPage(pageRoot, pairRoot);

   return pageDir;
}

/*
 *----------------------------------------------------------------------
 *
 * PT_ReleasePageDir --
 *
 *      Release resources that were being used for mapping the pageDir
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
PT_ReleasePageDir(VMK_PDE *pageDir, KSEG_Pair *pair)
{
   PTUnmapPage(pageDir, pair);
}


/*
 *----------------------------------------------------------------------
 *
 * PTAllocPageDir --
 *
 *      Create a new pagedir to map the given linear address and return
 *      a pointer to it.  The old page dir value is overwritten.
 *      Use kseg to map the pagedir if pair is not NULL, otherwise KVMap
 *
 * Results:
 *      Pointer to pagedir, which must be released using ReleasePageDir
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_PDE *
PTAllocPageDir(VMK_PDPTE* pageRoot, LA addr)
{
   VMK_PDE *pageDir;
   MPN pageDirMPN;

   ASSERT(pageRoot);

   pageDir = PTAllocPage(&pageDirMPN, NULL, FALSE);
   if (pageDir != NULL) {
      PT_SET(&pageRoot[ADDR_PDPTE_BITS(addr)], 
             MAKE_PDPTE(pageDirMPN, PDIR_EXCLUSIVE, PDPTE_FLAGS));
   }

   return pageDir;
}


/*
 *----------------------------------------------------------------------
 *
 * PT_GetPageTableInDir --
 *
 *      Get a pointer to the pagetable page that maps the given linear
 *      address in the given pageDirectory.  The given laddr is
 *      assumed to already map to pageDir, of course.  Use kseg to map
 *      the page if pair is not NULL, otherwise use KVMap.
 *
 * Results:
 *      Pointer to pagetable which must be released using
 *      PT_ReleasePageTable, or NULL. 
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_PTE*
PT_GetPageTableInDir(VMK_PDE* pageDir, LA laddr, KSEG_Pair **pair)
{
   VMK_PTE *pageTable = NULL;
   VMK_PTE pte;
   
   ASSERT(pageDir != NULL);
   
   pte = pageDir[ADDR_PDE_BITS(laddr)];
   // Return entry only if present bit is set, and large page bit is not.
   if (PTE_PRESENT(pte)
       && (! (pte & PTE_PS))) {
         pageTable = (VMK_PTE *)PTMapPage(MPN_2_MA(VMK_PTE_2_MPN(pte)),
                                          pair);
   }

   return pageTable;
}


/*
 *----------------------------------------------------------------------
 *
 * PT_GetPageTable --
 *
 *      Get a pointer to the pagetable page that maps the given
 *      linear address.
 *      Use kseg to map the page if pair is not NULL, otherwise KVMap
 *
 * Results:
 *      Pointer to pagetable which must be released using
 *      PT_ReleasePageTable, or NULL. 
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_PTE *
PT_GetPageTable(MA pageRootMA, LA laddr, KSEG_Pair **pair)
{
   VMK_PDE *pageDir;
   KSEG_Pair *tablePair = NULL;
   VMK_PTE *pageTable;

   pageDir = PT_GetPageDir(pageRootMA, laddr,
                           (pair == NULL) ? NULL : &tablePair);

   if (pageDir != NULL) {
      pageTable = PT_GetPageTableInDir(pageDir, laddr, pair);
      PT_ReleasePageDir(pageDir, tablePair);
   } else {
      pageTable = NULL;
   }

   return pageTable;
}

/*
 *----------------------------------------------------------------------
 *
 * PT_ReleasePageTable --
 *
 *      Release resources that were being used for mapping the pageTable
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
PT_ReleasePageTable(VMK_PTE *pageTable, KSEG_Pair *pair)
{
   PTUnmapPage(pageTable, pair);
}

/*
 *----------------------------------------------------------------------
 *
 * PT_AllocPageTableInDir --
 *
 *	Allocate a new, empty page for the given laddr (and implicitly
 * 	lots of its neighbors) and register it in the given page
 * 	directory.
 *
 *	Caller must call PT_ReleasePageTable on returned page table.
 *
 * Results:
 *	Page table pointer (outPTableMPN will be the MPN of the page
 *	table).  Or NULL (and INVALID_MPN) if there was an error.
 *
 * Side effects:
 *	A page is allocated 
 *
 *----------------------------------------------------------------------
 */
VMK_PTE*
PT_AllocPageTableInDir(VMK_PDE* pageDir,
                       LA laddr,
                       int flags,
                       KSEG_Pair **pair,
                       MPN *outPTableMPN)
{
   MPN pTableMPN;
   VMK_PTE* pageTable;

   ASSERT(pageDir != NULL);

   pageTable = PTAllocPage(&pTableMPN, pair, FALSE);
   if (pageTable != NULL) {
      ASSERT(pTableMPN != INVALID_MPN);
      /*
       * Just overwrite whatever was in the table at this point.
       * Caller is responsible for knowing if a tlb flush is required.
       */
      PT_SET(&pageDir[ADDR_PDE_BITS(laddr)], 
             VMK_MAKE_PDE(pTableMPN, 0, PTE_KERNEL|flags));
   }

   if (outPTableMPN) {
      *outPTableMPN = pTableMPN;
   }

   return pageTable;
}

/*
 *----------------------------------------------------------------------
 *
 * PT_AllocPageTable --
 *
 *      Create a new pagetable page to map the given linear address
 *      and return a pointer to it.  The old pagedir entry is overwritten.
 *      Use kseg to map the pagedir if pair is not NULL, otherwise KVMap
 *      Also return the MPN of the new page in *pPTableMPN.
 *
 * Results:
 *      Pointer to pagedir, which must be released using PT_ReleasePageDir,
 *	or NULL.
 *
 * Side effects:
 *      Page directory is modified for addr
 *
 *----------------------------------------------------------------------
 */
VMK_PTE *
PT_AllocPageTable(MA pageRootMA,
                  LA addr,
                  int flags,
                  KSEG_Pair **pair,
                  MPN *pPTableMPN)
{
   KSEG_Pair *tablePair = NULL;
   VMK_PDE *pageDir;
   VMK_PTE *pageTable;

   pageDir = PT_GetPageDir(pageRootMA, addr,
                           (pair == NULL) ? NULL : &tablePair);

   if (pageDir != NULL) {
      pageTable = PT_AllocPageTableInDir(pageDir, addr, flags, pair, pPTableMPN);
      PT_ReleasePageDir(pageDir, tablePair);
   } else {
      pageTable = NULL;
   }

   return pageTable;
}   


/*
 *----------------------------------------------------------------------
 *
 * PT_CheckPageRoot --
 *
 *      ASSERT check that the given pageRootMA probably addresses a
 *      valid page root page.  Simple check of the 4 PDPTE entries,
 *      too.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
PT_CheckPageRoot(MA pageRootMA)
{
   int i;
   VMK_PDPTE *root;

   ASSERT(pageRootMA != 0);
   ASSERT(IsLowMA(pageRootMA));
   // bottom five bits must be 0
   ASSERT((pageRootMA & 31) == 0);

   root = PTMapPage(pageRootMA, NULL);
   ASSERT(root != NULL);
      
   for (i = 0; i < VMK_NUM_PDPTES; i++) {
      ASSERT((root[i]>>36) == 0);
      ASSERT((root[i] & 0xffe) == 0);
      ASSERT((root[i] & 1) == 1);
   }      

   PTUnmapPage(root, NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * PT_AddPageTable --
 *
 *      Maps the given page table mpn at the given linear address in the
 *      currently installed pageroot (cr3)
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
PT_AddPageTable(LA laddr, MPN pageTableMPN)
{
   MA cr3;
   VMK_PDE *pageDir;

   GET_CR3(cr3);
   pageDir = PT_GetPageDir(cr3, laddr, NULL);
   ASSERT_NOT_IMPLEMENTED(pageDir);

   PT_SET(&pageDir[ADDR_PDE_BITS(laddr)], 
          VMK_MAKE_PDE(pageTableMPN, 0, PTE_KERNEL));
   PT_ReleasePageDir(pageDir, NULL);

   TLB_Flush(TLB_LOCALONLY);
}

/*
 *----------------------------------------------------------------------
 *
 * PT_LogPageRoot --
 *
 *	Debugging utility function.  Dumps the relevant parts of the
 *	given page table for the given range of linear addresses.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Potentially a lot of stuff is dumped to the Log.
 *
 *----------------------------------------------------------------------
 */
void
PT_LogPageRoot(MA pageRootMA, LA start, LA end)
{
   VMK_PDPTE* pageRoot;
   uint32 rooti;
   
   pageRoot = PTMapPage(pageRootMA, NULL);
   VMLOG(0, MY_RUNNING_WORLD->worldID,
         "pageRootMA=%#Lx, start=la:%#x end=la:%#x",
       pageRootMA, start, end);
   /* Dump each relevant page directory pointer page entry. */
   for (rooti = ADDR_PDPTE_BITS(start); rooti <= ADDR_PDPTE_BITS(end); rooti++) {
      VMK_PDPTE rentry = pageRoot[rooti];
      if (PTE_PRESENT(rentry)) {
         uint32 dirStart = (rooti == ADDR_PDPTE_BITS(start)) ?
            ADDR_PDE_BITS(start) : 0;
         uint32 dirEnd = (rooti == ADDR_PDPTE_BITS(end)) ?
            ADDR_PDE_BITS(end) : VMK_PDES_PER_PDPTE;
         uint32 diri;
         MA pageDirMA = MPN_2_MA(VMK_PTE_2_MPN(rentry));
         VMK_PDE* pageDir;
         pageDir = PTMapPage(pageDirMA, NULL);
         VMLOG(0, MY_RUNNING_WORLD->worldID,
               "  [%3d] pdpte = %#Lx (%#Lx): LA:%#x-%#x",
               rooti, pageDirMA, rentry,
               PTBITS_ADDR(rooti, 0, 0),
               PTBITS_ADDR(rooti+1, 0, 0) - 1
            );
         /* Dump each relevant page directory entry. */
         for (diri = dirStart; diri <= dirEnd; diri++) {
            VMK_PDE pdentry = pageDir[diri];
            if (PTE_PRESENT(pdentry)) {
               MA pageTableMA = MPN_2_MA(VMK_PTE_2_MPN(pdentry));
               VMK_PTE* pageTable;
               pageTable = PTMapPage(pageTableMA, NULL);
               VMLOG(0, MY_RUNNING_WORLD->worldID,
                     "    [%3d] pde = %#Lx (%#Lx): LA:%#x-%#x%s",
                     diri, pageDirMA, pdentry,
                     PTBITS_ADDR(rooti, diri, 0),
                     PTBITS_ADDR(rooti, diri+1, 0) - 1,
                     (pdentry & PTE_PS) ? " (super page)" : ""
                  );
               /* Don't parse large page entries as a page table */
               if (! (pdentry & PTE_PS)) {
                  int pti;
                  int tStart = (rooti == ADDR_PDPTE_BITS(start))
                     ? ADDR_PTE_BITS(start) : 0;
                  int tEnd = (rooti == ADDR_PDPTE_BITS(end))
                     ? ADDR_PTE_BITS(end) : VMK_PTES_PER_PDE;

                  /* Dump each relevant page table entry. */
                  for (pti = tStart; pti <= tEnd; pti++) {
                     VMK_PTE ptentry = pageTable[pti];
                     if (PTE_PRESENT(ptentry)) {
                        VMLOG(0, MY_RUNNING_WORLD->worldID,
                              "      [%3d] pte = %#Lx: LA:%#x-%#x",
                              pti, ptentry, 
                              PTBITS_ADDR(rooti, diri, pti),
                              PTBITS_ADDR(rooti, diri, pti+1) - 1
                           );
                     }
                  }
               }
               PTUnmapPage(pageTable, NULL);
            } else {
               VMLOG(0, MY_RUNNING_WORLD->worldID,
                     "    [%3d] pde = <not present> (%#Lx) LA::%#x-%#x",
                     diri, pdentry,
                     PTBITS_ADDR(rooti, diri, 0),
                     PTBITS_ADDR(rooti, diri+1, 0) - 1
                  );
            }
         }
         PTUnmapPage(pageDir, NULL);
      } else {
         VMLOG(0, MY_RUNNING_WORLD->worldID,
               "  [%3d] pdpte = <not present> (%#Lx): LA:%#x-%#x",
               rooti, rentry,
               PTBITS_ADDR(rooti, 0, 0),
               PTBITS_ADDR(rooti+1, 0, 0) - 1
            );
      }
   }
   PTUnmapPage(pageRoot, NULL);
}
   
