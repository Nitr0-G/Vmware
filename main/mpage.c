/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * Overview
 *
 *   The MPage module allocates and maintains one 'MPage' structure
 *   for each page of machine memory.  The MPage structure holds metadata
 *   describing its corresponding machine page.  MPages are analogous
 *   to Linux's 'struct page' or BSD's 'struct vm_page'.
 *
 * MPage structure 
 *
 *   This structure is currently just opaque data bytes.  In the
 *   future, as modules other than just pshare use this modules, this
 *   will need to be changed.  But for now this works.
 *
 * Interface
 *
 *   Use MPage_Map(..) to map the 'MPage' structure for a given MPN,
 *   and later unmap it with MPage_Unmap(..)
 *
 * Memory ranges
 *
 *   Memmap.c informs this module about ranges of machine memory as
 *   they are added to the system.  The first range is the memory
 *   present at boot.  Subsequent ranges are hot added memory.
 *
 *   A chunk of each memory range is consumed to hold the MPage
 *   structs for that range.  The precise placement within the range
 *   is determined by memmap.c.
 *
 * Synchronization
 *
 *   NB Currently there is no synchronization of the fields of
 *   individual MPage structures.  The only client of this module is
 *   pshare.c, and by the latter's internal locking, accesses to this
 *   module are synchronized.  This will need to be changed as soon as
 *   just one other module starts using this module.
 *
 *   Memmap synchronizes memory hot adds, which makes concurrent calls
 *   to MPage_AssignContMPNs() impossible.  Therefore this module
 *   doesn't need to repeat this synchronization.
 *
 * Memory Overhead
 *
 *   sizeof(MPage) / PAGE_SIZE  -- Currently ~ .39%
 *
 * Future Modifications
 *
 *   1) Store the MPage struct corresponding to a given MPN at a
 *   machine address that is *solely* a function of that MPN.  This
 *   would streamline MPage_Map(), as it wouldn't even have to know
 *   about memory ranges.  Care would need to be taken to balance
 *   cache utilization and to support large (2MB) memory pages.
 *
 *   2) Require clients to pass a valid MPN to MPage_Map(), so that
 *   they don't have to check the return value.  This idea is
 *   analogous to the KSeg mapping routines; they just work and you
 *   never need to check return values.
 *
 *   3) Could the MPage array be permanently mapped into the kernel
 *   virtual address space?  It would be really convenient.  But
 *   doesn't seem feasible.  
 *
 *   Assuming sizeof(MPage) == 16 bytes, 4GB of machine memory would
 *   only need 16MB of VA space to holds its MPage structures.  But,
 *   in reality, you might be required to reserved 256MB of VA space
 *   to cover the case of 64GB of machine memory.  256MB seems too
 *   large, give that the kernel VA space is only 1GB.
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "vm_asm.h"
#include "x86.h"
#include "vmkernel.h"
#include "splock.h"
#include "kseg.h"
#include "libc.h"
#include "memmap.h"
#include "memalloc.h"
#include "util.h"
#include "mpage.h"

#define	LOGLEVEL_MODULE MPage
#include "log.h"

// debugging
#if	defined(VMX86_DEBUG) && defined(VMX86_DEVEL)
#define	MPAGE_DEBUG		(1)
#else
#define	MPAGE_DEBUG		(0)
#endif

#define MPageDebug(fmt, args...) \
 if (MPAGE_DEBUG) LOG(0, fmt , ##args)

// This structure describes 1) a range of machine memory and 2) the
// mpage array.  The array is stored at a contiguous chunk of machine
// memory within the range itself.  The array is sized so that one
// 'MPage' structure exists for each machine page in the entire range.
typedef struct {
   // the range proper
   MPN minMPN;              // first MPN in the range
   MPN maxMPN;              // last MPN in the range
   uint32 nMPNs;            // maxMPN - minMPN + 1

   MA mpageArray;	    // machine address of the beginning
                            // of the array of Mpages
   uint32 mpageArraySize;   // array size (in pages)
} MPageMachineMemoryRange;


// Machine memory is composed of a number of non-overlapping, and
// (almost certainly) non-contigous, memory ranges.  range[0] is the
// boot time memory, and range[i], for i>0, are hot add memory ranges.
typedef struct {
   uint32 nRanges;   
   MPageMachineMemoryRange range[MAX_AVAIL_MEM_RANGES];
   uint32 totalMemoryPages;     // in pages (sum over range[i].nMPNs)
   uint32 totalOverheadPages;   // in pages (sum over range[i].mpageArraySize)
} MPageMachineMemory;


// Container for all the state of the MPage Module. 
typedef struct {
   MPageMachineMemory mem;
} MPageModule;


// Module instance
static MPageModule mpagemodule;


#define	LOW24(x)		((x) & 0xffffff)


/*
 *----------------------------------------------------------------------
 *
 * MPage_GetNumContMPNs --
 *    
 *    Memmap.c calls this function to ask MPage how much
 *    memory it desires out of the range [minPMN, maxMPN].
 *
 *    'hotAdd' is used to say if this range is hot added,
 *    or was simply present at boot; it's currently ignored.
 *
 * Results:
 *    Number of contiguous MPNs desired
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------
 */
uint32
MPage_GetNumContMPNs(MPN minMPN,
                     MPN maxMPN,
                     UNUSED_PARAM(Bool hotAdd))
{
   uint32 nPages = maxMPN - minMPN + 1;
   uint32 nBytesNeeded, nPagesNeeded;

   // need one MPage structure per machine page
   nBytesNeeded = sizeof(MPage) * nPages;

   // round up to nearest page
   nPagesNeeded = CEILING(nBytesNeeded, PAGE_SIZE);

   return nPagesNeeded;
}



/*
 *----------------------------------------------------------------------
 *
 * MPage_AssignContMPNs --
 *    
 *    Initializes the MPage data structures for a memory range.
 *
 *    Memmap.c calls this function to inform MPage that it can use the
 *    MPNs [startMPN, startMPN + reqSize - 1].  This range is
 *    sub-range of [minMPN, maxMPN].
 *  
 * Results:
 *    VMK_OK
 *
 * Side effects:
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
MPage_AssignContMPNs(MPN minMPN,
                     MPN maxMPN,
                     UNUSED_PARAM(Bool hotAdd),
                     uint32 reqSize,
                     MPN startMPN)
{
   MPageModule *m = &mpagemodule;
   MPageMachineMemoryRange *r;
   int i;

   MPageDebug("minMPN 0x%x, maxMPN 0x%x, hotAdd %u, reqSize 0x%x, startMPN 0x%x",
              minMPN, maxMPN, hotAdd, reqSize, startMPN);

   // sanity
   ASSERT(m->mem.nRanges < MAX_AVAIL_MEM_RANGES);

   // initialize descriptor for new memory range
   r = &m->mem.range[m->mem.nRanges];
   r->minMPN = minMPN;
   r->maxMPN = maxMPN;
   r->nMPNs = maxMPN - minMPN + 1;
   r->mpageArray = MPN_2_MA(startMPN);
   r->mpageArraySize = reqSize;

   // zero out each byte of the mpage array
   for (i = 0; i < r->mpageArraySize; i++) {
      Util_ZeroMPN(startMPN + i);
   }

   // update totals
   m->mem.totalMemoryPages += r->nMPNs;
   m->mem.totalOverheadPages += r->mpageArraySize;

   // Important: The order is important here!  We don't want
   // MPage_Map() to see partially initialized ranges nor do we want
   // to resort to locking.  Therefore, the range is fully initialized
   // before its made visible by incrementing 'nRanges'.
   m->mem.nRanges++;

   // success
   return(VMK_OK);
}



/*
 *----------------------------------------------------------------------
 *
 * MPage_GetNumMachinePages --
 *
 *    Accessor.
 *    
 * Results:
 *
 *    Returns the number total number machine pages which are present
 *    in the system.
 *
 * Side effects:
 *
 *----------------------------------------------------------------------
 */
uint32
MPage_GetNumMachinePages(void)
{
   MPageModule *m = &mpagemodule;
   return m->mem.totalMemoryPages;
}


/*
 *----------------------------------------------------------------------
 *
 * MPage_GetNumOverheadPages --
 *
 *    Accessor.
 *    
 * Results:
 *
 *    Returns the number of machine pages which are used to store
 *    MPage structure.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------
 */
uint32
MPage_GetNumOverheadPages(void)
{
   MPageModule *m = &mpagemodule;
   return m->mem.totalOverheadPages;
}



/*
 *----------------------------------------------------------------------
 *
 * MPage_Map --
 *    
 *    Maps the MPage structure for 'mpn'.
 *
 * Results:
 *    Pointer to the MPage structure.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------
 */
MPage *
MPage_Map(MPN mpn, KSEG_Pair **pair)
{
   MPageModule *m = &mpagemodule;
   MPageMachineMemoryRange *r;
   uint32 i;

   if (mpn == INVALID_MPN) {
      return(NULL);
   }

   // sanity check
   ASSERT(LOW24(mpn) == mpn);

   // Find mem range containing mpn
   r = NULL;
   for (i = 0; i < m->mem.nRanges; i++) {
      r = &m->mem.range[i];
      if (r->minMPN <= mpn && mpn <= r->maxMPN) {
         break;
      }
      r = NULL;
   }


   if (!r) {
      // no range contains mpn    
      return(NULL);
   } else {
      MPage *vaddr;
      MA maddr;
      // sanity
      ASSERT(r->minMPN <= mpn && mpn <= r->maxMPN);
      // compute machine address of mpn's MPage struct
      maddr = r->mpageArray + (mpn - r->minMPN) * sizeof(MPage);
      // sanity: don't access memory we didn't allocate 
      ASSERT(maddr >= r->mpageArray &&
             maddr < r->mpageArray + PAGE_SIZE * r->mpageArraySize);
      // map using kseg
      vaddr = Kseg_GetPtrFromMA(maddr, sizeof(MPage), pair);
      return(vaddr);
   }
}



/*
 *----------------------------------------------------------------------
 *
 * MPage_Unmap --
 *    
 *    Unmaps the an MPage structure.
 *
 * Results:
 *    None
 *
 * Side effects:
 *    None
 *
 *----------------------------------------------------------------------
 */
void
MPage_Unmap(KSEG_Pair *pair)
{
   Kseg_ReleasePtr(pair);
}
