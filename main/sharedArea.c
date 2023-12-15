/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

#include "vm_types.h"
#include "vm_libc.h"
#include "vmkernel.h"
#include "vmnix_if.h"
#include "memmap.h"
#include "alloc.h"
#include "xmap.h"
#include "world.h"
#include "sharedArea.h"
#include "host.h"
#include "simplemalloc.h"
#include "memalloc.h"
#include "user.h"
#include "vmnix_syscall.h"

#define LOGLEVEL_MODULE SharedArea
#include "log.h"

/*
 * Implements shared memory between a VM & the VMkernel. The memory is
 * shared 3way, between vmm, vmx, and vmkernel. The memory is shared
 * per VM and not per world i.e the naming domain is the VM.
 *
 * Since we have a console os vmx and a userworld vmx, this file has 
 * two separate mechanisms for managing shared area.
 *
 * For userworld, the vmx can do a mmap prior SharedArea_Init and the 
 * vmkernel can read its page tables to find mpns for the shared area.
 * 
 * For console os vmx, the vmkernel cooks up its own mpns for shared area
 * and when the vmx can later do Mem_Map, it calls back to the vmkernel
 * place these mpns in the correct structure in alloc.c.  Since no worlds
 * fully exist yet to charge these initial pages, they are taken from the
 * kernel pool.  Hopefully we can get rid of this code one day...
 *
 */ 

typedef struct SharedAreaInfo {
   void              *vmkbase;           // sharedarea mapped in vmkernel
   VA                vmxbase;            // vmx addr for shared area (userworld)
   SharedAreaDesc    *sharedAreaDescs;   // description of shared area allocs
   uint32            numSharedAreaDescs; // number of shared area allocations
   uint32            numSharedAreaPages; // number of pages for shared area
   XMap_MPNRange     *ranges;            // mpns used for shared area
} SharedAreaInfo;



/*
 *----------------------------------------------------------------------
 *
 *  SharedArea_Map --
 *
 *      For the COS based vmx, we get this callback to map the sharedArea
 *      mpns into the vmx address space.
 *
 *  Results:
 *      Page frames for vmx may be updated.
 * 
 *  Side effects:
 *      None     
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
SharedArea_Map(VMnix_MapSharedArea *args)
{
   VMnix_MapSharedArea map;
   World_Handle *w;
   uint32 i, numPages;
   VPN startVPN;
   VMK_ReturnStatus status = VMK_OK;
   SharedAreaInfo *sai;
   CopyFromHost(&map, args, sizeof(map));
   w = World_Find(map.worldID);
   if (w == NULL) {
      return VMK_BAD_PARAM;
   }
   sai = World_VMMGroup(w)->sai;
   numPages = sai->numSharedAreaPages;
   if (map.length / PAGE_SIZE != numPages) {
      VmWarn(w->worldID, "SharedArea_Map Failed due to page size mismatch");
      return VMK_BAD_PARAM;
   }
   startVPN = VA_2_VPN(map.startUserVA);
   for (i = 0; i < numPages; i++) {
      status = Alloc_MapSharedAreaPage(w, startVPN + i, 
                                       sai->ranges[i].startMPN);
      if (status != VMK_OK) {
         break;
      }
   }
   World_Release(w);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 *  SharedArea_Init --
 *
 *      Initialize per-vm shared area.
 *
 *  Results:
 *      VMK_OK on success
 * 
 *  Side effects:
 *      None     
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
SharedArea_Init(World_Handle *world, World_InitArgs *args)
{
   uint32 i;
   SharedAreaInfo *sai = NULL;
   XMap_MPNRange *ranges = NULL;
 
   ASSERT(World_IsVMMWorld(world));
   ASSERT(world && world->group);

   if (World_VMMGroup(world)->sai != NULL) {
      // already allocated by another vmm world
      return VMK_OK; 
   }
   if ((sai = World_Alloc(world, sizeof(SharedAreaInfo))) == NULL) {
      goto free_and_exit;
   }
   sai->numSharedAreaDescs = args->sharedAreaArgs->numDescs;
   sai->numSharedAreaPages = args->sharedAreaArgs->numPages;
   sai->vmxbase = args->sharedAreaArgs->userVA;
   if ((sai->sharedAreaDescs = World_Alloc(world, sizeof(SharedAreaDesc) *
                                         sai->numSharedAreaDescs)) == NULL) {
      goto free_and_exit;
   }
   memcpy(sai->sharedAreaDescs,  args->sharedAreaArgs->descs, 
          sizeof(SharedAreaDesc) * sai->numSharedAreaDescs);

   if ((ranges = World_Alloc(world, sizeof(XMap_MPNRange) * sai->numSharedAreaPages)) ==
       NULL) {
      goto free_and_exit;
   }

   /*
    * Console os implementation starts out with no address space in the vmx.
    */
   if (sai->vmxbase == 0) {
      // initialize ranges to cope with failure
      for (i = 0; i < sai->numSharedAreaPages; i++) {
         ranges[i].startMPN = INVALID_MPN;
         ranges[i].numMPNs = 0;
      }
      for (i = 0; i < sai->numSharedAreaPages; i++) {
         ranges[i].startMPN = MemMap_AllocKernelPage(MM_NODE_ANY, 
                                                     MM_COLOR_ANY,
                                                     MM_TYPE_ANY);
         ranges[i].numMPNs = 1;
         if (ranges[i].startMPN == INVALID_MPN) {            
            goto free_and_exit;
         }
      }
   } else {
      /* 
       * Userworld implementation starts with mmap in the vmx to 
       * reserve the necessary va and backing.
       */
      for (i = 0; i < sai->numSharedAreaPages; i++) {
         VMK_ReturnStatus status;
         MPN mpn;
         VPN userVPN = VA_2_VPN((VA)sai->vmxbase);
         status = User_GetPageMPN(MY_RUNNING_WORLD, userVPN + i, USER_PAGE_PINNED, &mpn);
         if (status != VMK_OK) {
            VMLOG(0, world->worldID, "Lookup failed: 0x%x", userVPN + i);
            goto free_and_exit;
         }
         ranges[i].startMPN = mpn;
         ranges[i].numMPNs = 1;
      }
   }
   sai->vmkbase = XMap_Map(sai->numSharedAreaPages, ranges, 
                           sai->numSharedAreaPages);
   if (sai->vmkbase == NULL) {
      goto free_and_exit;
   }
   memset(sai->vmkbase, 0, sai->numSharedAreaPages * PAGE_SIZE);
   World_VMMGroup(world)->sai = sai;
   World_VMMGroup(world)->sai->ranges = ranges;
   return VMK_OK;
free_and_exit:
   if (ranges != NULL) {
      ASSERT(sai);
      if (sai->vmxbase == 0) {
         // Console os page cleanup
         for (i = 0; i < sai->numSharedAreaDescs; i++) {
            if (ranges[i].startMPN != INVALID_MPN) {
               MemMap_FreeKernelPage(ranges[i].startMPN);
            }
         }
      }
      World_Free(world, ranges);
   }
   if (sai->sharedAreaDescs != NULL) {
      World_Free(world, sai->sharedAreaDescs);
   }
   if (sai != NULL) {
      World_Free(world, sai);
   }
   World_VMMGroup(world)->sai = NULL;
   return VMK_NO_MEMORY;
}


/*
 *----------------------------------------------------------------------
 *
 *  SharedArea_Cleanup --
 *
 *      Reap function to cleanup per-vm sharedArea. 
 *
 *  Results:
 *      VMK_OK on success
 * 
 *  Side effects:
 *       Free memory, XMap_Unmap
 *
 *----------------------------------------------------------------------
 */

void
SharedArea_Cleanup(World_Handle *world)
{
   SharedAreaInfo *sai = World_VMMGroup(world)->sai;
   uint32 i;
   ASSERT(world && world->group);
   if (!World_IsVmmLeader(world) || sai == NULL) {
      return;
   }
   ASSERT(sai->vmkbase);
   XMap_Unmap(sai->numSharedAreaPages, sai->vmkbase);
   if (sai->vmxbase == 0) {
      for (i = 0; i < sai->numSharedAreaPages; i++) {
         MemMap_FreeKernelPage(sai->ranges[i].startMPN);
      }
   }
   World_Free(world, sai->sharedAreaDescs);
   World_Free(world, sai->ranges);
   World_Free(world, sai);
   World_VMMGroup(world)->sai = NULL;
}

/*
 *----------------------------------------------------------------------
 *
 *  SharedArea_Alloc --
 *
 *      Alloc per-vm shared memory between vmkernel, vmx, and vmm. VMK
 *      entrypoint.
 *
 *  Results:
 *      Pointer to memory in sharedarea or null.
 * 
 *  Side effects:
 *      None     
 *----------------------------------------------------------------------
 */

void *
SharedArea_Alloc(World_Handle *world, char *name, uint32 size)
{
   void *p;
   int i;
   SharedAreaInfo *sai = World_VMMGroup(world)->sai;
   ASSERT(sai != NULL);
   ASSERT(name != NULL);

   for (i = 0; i < sai->numSharedAreaDescs; i++) {
      if (strncmp(name, sai->sharedAreaDescs[i].name,
                  sizeof(sai->sharedAreaDescs[i].name)) == 0) {
         ASSERT(sai->sharedAreaDescs[i].size == size);
         p = sai->vmkbase + sai->sharedAreaDescs[i].offs;
         return p;
      }
   }
   Panic("vmm<->vmkernel version mismatch: Failed to find %s "\
         "in the sharedArea.\n", name);
}


/*
 *----------------------------------------------------------------------
 *
 *  SharedArea_GetBase --
 *
 *	Returns base address of the shared area.  
 *
 *  Results:
 *      Pointer to base address of shared area.
 * 
 *  Side effects:
 *      None     
 *----------------------------------------------------------------------
 */

void *
SharedArea_GetBase(World_Handle *world)
{
   SharedAreaInfo *sai = World_VMMGroup(world)->sai;
   ASSERT(sai != NULL);

   return (sai->vmkbase);
}
