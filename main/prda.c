/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * prda.c: vmkernel per-cpu data area initialization.
 */

#include "vm_types.h"
#include "vm_asm.h"
#include "x86.h"
#include "vm_libc.h"
#include "vmkernel.h"
#include "splock.h" 
#include "prda.h"
#include "kvmap.h"
#include "memalloc.h"
#include "memmap.h"
#include "pagetable.h"
#include "util.h"
#include "host_dist.h"

PRDA **prdas;
MPN *prdaMPNs;
MPN prdaPTableMPNs[MAX_PCPUS][VMK_NUM_PRDA_PDES];

/*
 * We keep this non-static so that when entering the debugger, the code in
 * debugAsm.S can ascertain whether it can safely access the PRDA (calling
 * PRDA_IsInitialized() would scribble on the stack, which is something we don't
 * want to do when entering the debugger).  Note that there is no extern
 * definition for this variable though, so for .c files it's effectively static.
 */
Bool prdaInitialized = FALSE;

/*
 *----------------------------------------------------------------------
 *
 * PRDA_Init --
 *
 *      Initialize the PRDAs for all the pcpus. 
 *
 * Results: 
 *      None.
 *      
 * Side effects:
 *      All of the prda data structures are allocated and initialized.
 *
 *----------------------------------------------------------------------
 */
void
PRDA_Init(VMnix_Init *vmnixInit)
{
   PCPU i;
   int j;

   prdas = (PRDA **)Mem_Alloc(numPCPUs * sizeof(PRDA *));
   ASSERT(prdas != NULL);
   
   prdaMPNs = (MPN *)Mem_Alloc(numPCPUs * sizeof(MPN));
   ASSERT(prdaMPNs != NULL);

   for (i = 0; i < numPCPUs; i++) {
      VMK_PTE *prdaPTable;

      // allocate and initialize the prda pagetables
      for (j = 0; j < VMK_NUM_PRDA_PDES; j++) {
         VMK_ReturnStatus status;
         prdaPTableMPNs[i][j] = MemMap_AllocKernelPage(MemMap_PCPU2NodeMask(i),
                                                       MM_COLOR_ANY, MM_TYPE_ANY);
         ASSERT_NOT_IMPLEMENTED(prdaPTableMPNs[i][j] != INVALID_MPN);
         MemMap_SetIOProtection(prdaPTableMPNs[i][j], MMIOPROT_IO_DISABLE);

         status = Util_ZeroMPN(prdaPTableMPNs[i][j]);
         ASSERT(status == VMK_OK);

         if (i == HOST_PCPU) {
            PT_AddPageTable(VMK_FIRST_PRDA_ADDR + j*PDE_SIZE - VMK_FIRST_ADDR,
                            prdaPTableMPNs[i][j]);
         }
      }

      // allocate and initialize the prda page
      prdaMPNs[i] = MemMap_AllocKernelPage(MemMap_PCPU2NodeMask(i),
                                           MM_COLOR_ANY, MM_TYPE_ANY);
      ASSERT_NOT_IMPLEMENTED(prdaMPNs[i] != INVALID_MPN);
      MemMap_SetIOProtection(prdaMPNs[i], MMIOPROT_IO_DISABLE);

      prdas[i] = (PRDA *)KVMap_MapMPN(prdaMPNs[i], 0);
      memset(prdas[i], 0, sizeof(PRDA));
      prdas[i]->pcpuNum = i;
      if (i == HOST_PCPU) {
	 prdas[i]->pcpuState = PCPU_BSP;
      }
      prdas[i]->currentTicks = 1;
      prdas[i]->vmkServiceRandom = i;
      prdas[i]->vmkServiceShift = 0;
      prdas[i]->randSeed = ( (RDTSC() * (i + 1)) %
                             (UTIL_FASTRAND_SEED_MAX - 1) ) + 1;
      ASSERT(0 < prdas[i]->randSeed &&
             prdas[i]->randSeed < UTIL_FASTRAND_SEED_MAX);

      // map the PRDA page into the PRDA region
      prdaPTable = KVMap_MapMPN(prdaPTableMPNs[i][0], TLB_LOCALONLY);
      PT_SET(&prdaPTable[0], VMK_MAKE_PDE(prdaMPNs[i], 0, PTE_KERNEL));
      KVMap_FreePages(prdaPTable);

      if (i == HOST_PCPU) {
         TLB_Flush(TLB_LOCALONLY);
      }
   }
   prdaInitialized = TRUE;
}

/*
 *----------------------------------------------------------------------
 *
 * PRDA_GetRunningWorldSafe
 *
 *      Get the world pointer for the currently running world on this CPU,
 *      but do it in a safe manner such that we don't take a fault.
 *
 * Results: 
 *      World pointer
 *      
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
struct World_Handle *
PRDA_GetRunningWorldSafe(void)
{
   if (prdaInitialized) {
      return MY_RUNNING_WORLD;
   } else {
      return NULL;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * PRDA_GetRunningWorldIDSafe
 *
 *      Get the worldID for the currently running world on this CPU,
 *      but do it in a safe manner such that we don't take a fault.
 *
 * Results: 
 *      World_ID
 *      
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
World_ID
PRDA_GetRunningWorldIDSafe(void)
{
   World_ID worldID = INVALID_WORLD_ID;

   if (PRDA_GetRunningWorldSafe() != NULL) {
      worldID = MY_RUNNING_WORLD->worldID;
   }

   return worldID;
}

/*
 *----------------------------------------------------------------------
 *
 * PRDA_GetRunningWorldNameSafe
 *
 *      Get the world name for the currently running world on this CPU,
 *      but do it in a safe manner such that we don't take a fault.
 *
 * Results: 
 *      Pointer to world's name
 *      
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
const char*
PRDA_GetRunningWorldNameSafe(void)
{
   char *worldName = "unknown";

   if (PRDA_GetRunningWorldSafe() != NULL) {
      worldName = MY_RUNNING_WORLD->worldName;
   }

   return worldName;
}

/*
 *----------------------------------------------------------------------
 *
 * PRDA_GetPCPUNumSafe
 *
 *      Get the current cpu number in a safe manner
 *
 * Results: 
 *      CPU number
 *      
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
uint32
PRDA_GetPCPUNumSafe(void)
{
   if (prdaInitialized) {
      return myPRDA.pcpuNum;
   } else {
      return 0;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * PRDA_IsInitialized
 *
 *      Return TRUE if the PRDA region setup and initialized
 *
 * Results: 
 *      TRUE if PRDA initialized
 *      
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Bool
PRDA_IsInitialized(void)
{
   return prdaInitialized;
}

/*
 *----------------------------------------------------------------------
 *
 * PRDA_MapRegion
 *
 *      Map the PRDA region for the given CPU into the given page table root
 *
 * Results: 
 *      VMK_ReturnStatus.
 *      
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
PRDA_MapRegion(PCPU pcpu, MA pageRoot)
{
   int i;
   KSEG_Pair *dirPair;
   LA laddr = VMK_VA_2_LA(VMK_FIRST_PRDA_ADDR);

   VMK_PDE *pageDir = PT_GetPageDir(pageRoot, laddr, &dirPair);
   if (pageDir == NULL) {
      return VMK_NO_RESOURCES;
   }
   for (i = 0; i < VMK_NUM_PRDA_PDES; i++) {
      PT_SET(&pageDir[ADDR_PDE_BITS(laddr)],
             VMK_MAKE_PDE(prdaPTableMPNs[pcpu][i], 0, PTE_KERNEL));
      laddr += PDE_SIZE;
   }
   PT_ReleasePageDir(pageDir, dirPair);

   return VMK_OK;
}
