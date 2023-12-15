/* **********************************************************
 * Copyright 2000 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * alloc_inline.h --
 *
 *	Inline functions exported by the alloc module.
 */


#ifndef _ALLOC_INLINE_H
#define _ALLOC_INLINE_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "alloc.h"
#include "world.h"
#include "memmap.h"
#include "config.h"

static INLINE Alloc_Info *
Alloc_AllocInfo(const World_Handle *world)
{
   return &World_VMMGroup(world)->allocInfo;
}

static INLINE Bool
AllocCacheShouldRemapLow(const Alloc_P2M *ce)
{
   // simple policy: remap high pages copied more than specified count
   if (!IsLowMA(ce->maddr)) {
      uint32 nCopies = CONFIG_OPTION(NET_COPIES_BEFORE_REMAP);
      if ((nCopies > 0) &&
          (ce->copyHints > nCopies) &&
          (ce->firstPPN == ce->lastPPN)) {
         return(TRUE);
      }
   }
   
   return(FALSE);
}

static INLINE Alloc_P2M *
Alloc_CacheEntry(World_Handle *world, PPN ppn)
{
   return(&Alloc_AllocInfo(world)->p2mCache[ppn % ALLOC_P_2_M_CACHE_SIZE]);
}

static INLINE VMK_ReturnStatus
Alloc_PhysToMachine(World_Handle *world,
                    PA paddr,
                    uint32 length, 
                    uint32 flags,
                    Bool canBlock, 
                    Alloc_Result *result)
{
   VMK_ReturnStatus status;
   Alloc_Info *allocInfo = Alloc_AllocInfo(world);

   if (canBlock) {
      SP_AssertNoLocksHeld();
   }

   ASSERT(!CpuSched_HostWorldCmp(world));
   SP_Lock(&allocInfo->lock);

   if (flags & ALLOC_FAST_LOOKUP) {
      PPN firstPPN = PA_2_PPN(paddr);
      PPN lastPPN = PA_2_PPN(paddr + length - 1);   
      Alloc_P2M *ce = Alloc_CacheEntry(world, firstPPN);
      if (ce->firstPPN == firstPPN && ce->lastPPN >= lastPPN) {
         if (!ce->readOnly || (flags & ALLOC_READ_ONLY)) {
            result->maddr = ce->maddr + (paddr & PAGE_MASK);
            result->length = length;

            // update automatic page remapping state
            if ((flags & ALLOC_IO_COPY_HINT) ||
                VMK_STRESS_RELEASE_OPTION(MEM_REMAP_LOW)) {
               // update copy hint count
               ce->copyHints++;
               
               // consider remapping "hot" pages to low memory
               if (AllocCacheShouldRemapLow(ce) || 
                   VMK_STRESS_RELEASE_COUNTER(MEM_REMAP_LOW)) {
                  MPN mpn = MA_2_MPN(ce->maddr);
                  (void) Alloc_RequestRemapPageLow(world, firstPPN, mpn);
                  ce->copyHints = 0;
               }
            }

            SP_Unlock(&allocInfo->lock);
            return(VMK_OK);
         }
      }
   }

   status = AllocPhysToMachineInt(world, paddr, length, flags, 
                                  canBlock, result);

   SP_Unlock(&allocInfo->lock);

   return status;
}

#endif
