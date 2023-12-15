/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * kseg.c -
 *
 * This module manages the kseg vmkernel virtual address range.
 */

#include "vm_types.h"
#include "vmkernel.h"
#include "x86.h"
#include "vmnix_if.h"
#include "vm_asm.h"
#include "splock.h"
#include "kvmap.h"
#include "tlb.h"
#include "memalloc.h"
#include "alloc.h"
#include "alloc_inline.h"
#include "kseg.h"
#include "prda.h"
#include "memmap.h"
#include "vm_libc.h"
#include "post.h"
#include "pagetable.h"
#include "memsched_ext.h"
#include "dump.h"
#include "host_dist.h"
#include "util.h"
#include "nmi.h"

#define LOGLEVEL_MODULE Kseg
#include "log.h"

// compile-time options
#define KSEG_STATS	(vmx86_stats)
#define	KSEG_DEBUG	(vmx86_debug)

// global: the MPN of the kseg page tables for each CPU
MPN ksegPTableMPNs[MAX_PCPUS][VMK_NUM_KSEG_PDES];

#define LRU_ASSOC   4
#include "lru.h"

typedef struct KSEGPtrEntry {
   KSEG_Pair 	pairs[LRU_ASSOC];
   LRUWord	lru;
   uint8	lastWay;
   uint8	pad[30];	// 32-byte align the rest of this entry so the
				//   next entry will start on a 32-byte cache-aligned
				//   boundary.
} KSEGPtrEntry;

/* Address to access the local processor's kseg table */
static KSEGPtrEntry *kseg = (KSEGPtrEntry *)VMK_KSEG_PTR_BASE;

/* Addresses to access the kseg tables of each processor */
static KSEGPtrEntry *ksegs[MAX_PCPUS];

static VMK_PTE *ksegPT = (VMK_PTE *)VMK_KSEG_PTABLE_ADDR;

/* Number of KSEGPtrEntry elements in kseg table for each CPU */
#define NUM_KSEG_PAIRS	((VMK_KSEG_MAP_LENGTH / (2 * PAGE_SIZE)) / LRU_ASSOC)

/* VPN of first virtual address used by kseg */
#define VMK_KSEG_MAP_BASE_VPN	(VA_2_VPN(VMK_KSEG_MAP_BASE))

static Bool KsegPOST(void *clientData, int id, SP_SpinLock *lock, SP_Barrier *barrier);

#define MAX_KSEG_PAGES	8

/*
 * Statistics
 */

typedef struct {
   uint32 paTries;
   uint32 paHits;
   uint32 paHits2;
   uint32 maTries;
   uint32 maHits;
   uint32 maHits2;
   uint32 pad[2];
} KSEGStats;

static KSEGStats ksegStats[MAX_PCPUS];
static Proc_Entry ksegStatsProc;

// forward declarations
static int KsegStatsProcRead(Proc_Entry *entry, char *buf, int *len);
static int KsegStatsProcWrite(Proc_Entry *entry, char *buf, int *len);

/* 
 * Note on Kseg flushing across CPUs:
 *
 * Alloc_UnlockPage needs to remove a MPN from a world.  This means that all
 * PPN to MPN mappings in the ksegs on all CPUs need to be flushed.  Since the
 * kseg is lock free this needs to be done in a lock free yet safe manner.  The
 * algorithm that we use is the following:
 *
 *	1) Alloc_UnlockPage invalidates its copy of the PPN to MPN mapping.  This
 *	   ensures that the now defunct PPN to MPN mapping cannot be entered into
 *	   any kseg caches anymore.
 *	2) Alloc_UnlockPage flushes the kseg mapping on the local CPU.
 *	3) Alloc_UnlockPage flushes the kseg mapping on all other CPUs by calling
 *	   Kseg_FlushRemote.
 *	4) Kseg_FlushRemote zaps the pairs PPN and worldID and then waits for the
 *	   pair reference count to go to zero so that all external references to
 *	   the pair are gone.
 *	
 *	There is a race between someone on the CPU of the kseg looking up a kseg 
 *	entry with the defunct ppn to mpn mapping and us flushing it.  It is possible 
 *	that the other CPU has seen the mapping but has not yet incremented the
 *	reference count.  In this case Kseg_FlushRemote will assume that it has
 *	successfully flushed the entry even though the other CPU still thinks that
 *	the cache entry is valid.  To protect against this we have the kseg
 *	lookup functions do things in the following order:
 *	
 *	1) Check for a cache hit.  If there is one ...
 *	2) Increment the reference count.
 *	3) Check again for a cache hit.  If there isn't one ...
 *	4) Decrement the reference count and go back to step one.
 *
 *	Once the kseg lookup function has incremented the reference count 
 *	it is guaranteed that Kseg_FlushRemote won't return until this count is
 *	decremented.  By checking again after we increment the count we handle
 *	the race condition described earlier.
 */

/*
 * Don't keep retrying finding a kseg entry forever.  We will have to retry if we find
 * that the entry got flushed from under us but if we try an insane number of times
 * then something is wrong and we need to avoid hanging the host.  In reality 
 * MAX_KSEG_RETRIES should always work if it is set to 1.  100 will only happen
 * if something has gone horribly wrong.
 */
#define MAX_KSEG_RETRIES	100
#define KSEG_FLUSH_MAX_US_WAIT	1000000


/*
 *----------------------------------------------------------------------
 *
 * KsegInitPCPU --
 *
 *      Initialize the kseg data structures on the given cpu.
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
KsegInitPCPU(int pcpu, MPN prdaPTableMPN, int ksegPages)
{
   int i;
   KVMap_MPNRange ranges[MAX_KSEG_PAGES];
   VMK_PTE *prdaPTable = KVMap_MapMPN(prdaPTableMPN, TLB_LOCALONLY);

   ASSERT(prdaPTable != NULL);
   ASSERT(MAX_KSEG_PAGES >= ksegPages);

   for (i = 0; i < ksegPages; i++) {
      /* Allocate machine pages for the kseg table itself. */
      MPN mpn = MemMap_AllocKernelPage(MemMap_PCPU2NodeMask(pcpu),
                                       MM_COLOR_ANY, MM_TYPE_ANY);
      VPN vpn = VA_2_VPN(VMK_KSEG_PTR_BASE - VMK_FIRST_PRDA_ADDR) + i;

      ASSERT_NOT_IMPLEMENTED(mpn != INVALID_MPN);
      MemMap_SetIOProtection(mpn, MMIOPROT_IO_DISABLE);
      PT_SET(&prdaPTable[vpn], VMK_MAKE_PDE(mpn, 0, PTE_KERNEL));
      if (pcpu == 0) {
	 TLB_INVALIDATE_PAGE(VMK_KSEG_PTR_BASE + i * PAGE_SIZE);
      } else {
	 void *ksegDst = KVMap_MapMPN(mpn, TLB_LOCALONLY);
         ASSERT(ksegDst != NULL);
	 /* Initialize kseg table of other processors from proc 0. */
	 memcpy(ksegDst, (void *)(((VA)kseg) + i * PAGE_SIZE), PAGE_SIZE);
	 KVMap_FreePages(ksegDst);
      }
      ranges[i].startMPN = mpn;
      ranges[i].numMPNs = 1;
   }
   KVMap_FreePages(prdaPTable);

   if (pcpu == HOST_PCPU) {
      for (i = 0; i < NUM_KSEG_PAIRS; i++) {
         int j;
         LRU_Init(&kseg[i].lru);
         /* Make sure kseg entries are cache-line  aligned */
         ASSERT_NOT_IMPLEMENTED(((uint32)&kseg[i] & 0x1f) == 0);      
         for (j = 0; j < LRU_ASSOC; j++) {
            ASSERT_NOT_IMPLEMENTED(((uint32)(&kseg[i].pairs[j]) & 0x1f) == 0);      
            kseg[i].pairs[j].pageNum = 0xffffffff;
            kseg[i].pairs[j].count = 0;
            kseg[i].pairs[j].vaddr = 
               VPN_2_VA(VMK_KSEG_MAP_BASE_VPN + 2 * (i * LRU_ASSOC) + 2 * j);
            kseg[i].lastWay = 0;
         }
      }
   }

   /* Also map kseg table so every processor can see it. */
   ksegs[pcpu] = KVMap_MapMPNs(ksegPages, ranges, ksegPages, 0);
   ASSERT(ksegs[pcpu] != NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * Kseg_Init --
 *
 *      Setup the kseg for all processors.  Kseg data structures are
 *      allocated and mapped into the proper places in the kseg and prda
 *      regions.
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
Kseg_Init(void)
{
   PCPU pcpu;
   int j;
   int ksegPages =
      (NUM_KSEG_PAIRS * sizeof(KSEGPtrEntry) + PAGE_SIZE - 1) / PAGE_SIZE;

   for (pcpu = 0; pcpu < numPCPUs; pcpu++) {
      for (j = 0; j < VMK_NUM_KSEG_PDES; j++) {
         VA addr;
         VMK_PTE *prdaPTable;
         VMK_ReturnStatus status;

         // allocate/intialize the kseg pagetables
         ksegPTableMPNs[pcpu][j] = MemMap_AllocKernelPage(MemMap_PCPU2NodeMask(pcpu),
                                                          MM_COLOR_ANY, MM_TYPE_ANY);
         ASSERT_NOT_IMPLEMENTED(ksegPTableMPNs[pcpu][j] != INVALID_MPN);
         MemMap_SetIOProtection(ksegPTableMPNs[pcpu][j], MMIOPROT_IO_DISABLE);

         status = Util_ZeroMPN(ksegPTableMPNs[pcpu][j]);
         ASSERT(status == VMK_OK);

         /*
          * map the pagetables as regular pages at VMK_KSEG_PTABLE_ADDR in the
          * PRDA region.
          */
         prdaPTable = KVMap_MapMPN(prdaPTableMPNs[pcpu][0], TLB_LOCALONLY);
         ASSERT(prdaPTable != NULL);
         addr = VMK_KSEG_PTABLE_ADDR + j * PAGE_SIZE;
         PT_SET(&prdaPTable[VA_2_VPN(addr - VMK_FIRST_PRDA_ADDR)],
                VMK_MAKE_PTE(ksegPTableMPNs[pcpu][j], 0, PTE_KERNEL));
         KVMap_FreePages(prdaPTable);

         /*
          * Map the pagetables at the KSEG region in the current world/CPU.
          * Other worlds will pick it up when they are initialized.
          */
         if (pcpu == HOST_PCPU) {
            PT_AddPageTable(VMK_FIRST_KSEG_ADDR + j*PDE_SIZE - VMK_FIRST_ADDR,
                            ksegPTableMPNs[pcpu][j]);
         }
      }

      // allocate/initialize the kseg data structures
      KsegInitPCPU(pcpu, prdaPTableMPNs[pcpu][0], ksegPages);
   }

   // we've added bunch of things to our cr3, let's make it usable now
   TLB_Flush(TLB_LOCALONLY);

   POST_Register("Kseg", KsegPOST, NULL);

   // register stats proc entry
   Proc_InitEntry(&ksegStatsProc);
   if (KSEG_STATS) {
      ksegStatsProc.read = KsegStatsProcRead;
      ksegStatsProc.write = KsegStatsProcWrite;
      Proc_RegisterHidden(&ksegStatsProc, "kseg", FALSE);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * KsegValidate --
 *
 *      Validate this vpn and mpn on this cpu.
 *
 * Results: 
 *      None.
 *      
 * Side effects:
 *      A page table entry is added for this cpu and the TLB entry for 
 *	the virtual address is flushed.
 *
 *----------------------------------------------------------------------
 */
static void
KsegValidate(VPN vpn, MPN mpn)
{
   VMK_PTE pte;

   ASSERT(vpn >= VA_2_VPN(VMK_KSEG_MAP_BASE) && 
          vpn <= VA_2_VPN(VMK_KSEG_MAP_BASE + VMK_KSEG_MAP_LENGTH - 1));

   pte = VMK_MAKE_PTE(mpn, 0, PTE_KERNEL);	 

   PT_SET(&ksegPT[vpn - VMK_KSEG_MAP_BASE_VPN], pte);
   TLB_INVALIDATE_PAGE(VPN_2_VA(vpn));
}

static VMK_PTE ksegSavedPTE;

/*
 *----------------------------------------------------------------------
 *
 * Kseg_DebugMap --
 *
 *      Create a temporary mapping for debugging purposes.  This should
 *	only be called when we know that nothing can interrupt the 
 *	calling thread between this call and the call to 
 *	Kseg_DebugMapRestore.
 *
 * Results: 
 *      A pointer to the mapped page.
 *      
 * Side effects:
 *      The first mapping of the kseg is saved and overwritten.
 *
 *----------------------------------------------------------------------
 */
void *
Kseg_DebugMap(MPN mpn)
{
   ksegSavedPTE = ksegPT[0];
   KsegValidate(VMK_KSEG_MAP_BASE_VPN, mpn);

   ASSERT(!NMI_IsCPUInNMI() || Panic_IsSystemInPanic());

   return (void *)VMK_KSEG_MAP_BASE;
}

/*
 *----------------------------------------------------------------------
 *
 * Kseg_DebugMapRestore --
 *
 *      Restore the mapping that was saved and overwritten by
 *	Kseg_DebugMap.
 *
 * Results: 
 *      None.
 *      
 * Side effects:
 *      The first mapping of the kseg is restored.
 *
 *----------------------------------------------------------------------
 */
void
Kseg_DebugMapRestore(void)
{
   ASSERT(!NMI_IsCPUInNMI() || Panic_IsSystemInPanic());

   PT_SET(&ksegPT[0], ksegSavedPTE);
   TLB_INVALIDATE_PAGE(VMK_KSEG_MAP_BASE);
}

/*
 *----------------------------------------------------------------------
 *
 * KsegPairInvalidate --
 *
 *      Set contents of "pair" to invalid values.
 *
 * Results: 
 *      Invalidates "pair".
 *      
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
KsegPairInvalidate(volatile KSEG_Pair *pair)
{
   pair->pageNum = INVALID_MPN;
   pair->worldID = INVALID_WORLD_ID;
   pair->maxAddr = -1;
}

/*
 *----------------------------------------------------------------------
 *
 * KsegPairIsInvalid --
 *
 *      Query if "pair" is invalid.
 *
 * Results: 
 *      Returns TRUE if "pair" is invalid, otherwise FALSE.
 *      
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
KsegPairIsInvalid(volatile KSEG_Pair *pair)
{
   return((pair->pageNum == INVALID_MPN) && (pair->worldID == INVALID_WORLD_ID));
}

/*
 *----------------------------------------------------------------------
 *
 * KsegPairIncCount --
 *
 *      Increment the usecount on this pair and total use count for this CPU.
 *      Also store the return address for debugging. 
 *
 * Results: 
 *      None
 *      
 * Side effects:
 *      Warning.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
KsegPairIncCount(KSEG_Pair *pair)
{
   pair->count++;
   if (KSEG_DEBUG) {
      pair->ra = __builtin_return_address(0);
      myPRDA.ksegActiveMaps++;
      /* if the total active maps is greateer than the associativity, we
       * could potentially fail a kseg map if they all ended up on the same
       * line in the kseg cache. */
      ASSERT((myPRDA.ksegActiveMaps <= LRU_ASSOC) ||
             Panic_IsSystemInPanic()); //one assert fail is enough!
   }
}

/*
 *----------------------------------------------------------------------
 *
 * KsegPairDecCount --
 *
 *      Decrement the usecount on this pair and total use count for this CPU.
 *
 * Results: 
 *      None
 *      
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
static INLINE void
KsegPairDecCount(KSEG_Pair *pair)
{
   ASSERT(pair->count > 0);
   pair->count--;
   if (KSEG_DEBUG) {
      ASSERT(myPRDA.ksegActiveMaps > 0);
      myPRDA.ksegActiveMaps--;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * KsegIncPair --
 *
 *      The count on this pair is incremented and the 2nd virtual page
 *	is mapped if necessary. This function will fail if the ppn 
 *	corresponding to the 2nd page is swapped out.
 *
 * Results: 
 *      The virtual address for this pair.
 *      In case of a failure the virtual address is NULL and retStatus
 *      contains the error status.
 *      
 * Side effects:
 *      The count on this pair is incremented and another virtual page
 *	is mapped if necessary.
 *
 *----------------------------------------------------------------------
 */
static inline void *
KsegIncPair(World_Handle *world, World_ID worldID, 
            KSEG_Pair *pair, MPN pageNum, uint64 maxAddr, 
            KSEG_Pair **resultPair, VMK_ReturnStatus *retStatus)
{
   // default return value 
   *retStatus = VMK_OK;

   if (pair->maxAddr < maxAddr) {

      VMLOG(1, pair->worldID, "0x%"FMT64"x < 0x%"FMT64"x",
           pair->maxAddr, maxAddr);
      if (pair->worldID == INVALID_WORLD_ID) {
         KsegValidate(VA_2_VPN(pair->vaddr + PAGE_SIZE), pageNum + 1);      
         pair->maxAddr = MPN_2_MA(pageNum + 2);
      } else {
         MPN mpn;
         Alloc_Result result;

         ASSERT(world != NULL);
         *retStatus = Alloc_PhysToMachine(world, PPN_2_PA(pageNum + 1), PAGE_SIZE,
                                      ALLOC_FAST_LOOKUP, FALSE, &result);
         if (*retStatus != VMK_OK) {
            return NULL;
         }

         mpn = MA_2_MPN(result.maddr);
         KsegValidate(VA_2_VPN(pair->vaddr + PAGE_SIZE), mpn);
         pair->maxAddr = PPN_2_PA(pageNum + 2);
      }
   }

   KsegPairIncCount(pair);

   {
      /*
       * Check for the match again because this entry could have gotten flushed 
       * after we checked it earlier.  Use a volatile pointer to make sure that
       * it rereads the pair entry from memory.
       */   
      volatile KSEG_Pair *p2 = (volatile KSEG_Pair *)pair;

      if (p2->pageNum != pageNum || p2->worldID != worldID) {
         KsegPairDecCount(pair);
         *retStatus  = VMK_KSEG_PAIR_FLUSHED ;
         return NULL;
      } else {
         *resultPair = pair;
         return (void *)pair->vaddr;
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * KsegGetNewPair --
 *
 *      Find a kseg pair in the specified entry (one that is not
 *      currently in use and was least recently used)
 *
 * Results: 
 *      The pair.
 *      
 * Side effects:
 *      LRU information is updated.
 *
 *----------------------------------------------------------------------
 */
static inline KSEG_Pair *
KsegGetNewPair(struct KSEGPtrEntry *entry)
{
   int i, count;
   KSEG_Pair *pair;

   for (count = 0; count < LRU_ASSOC; count++) {
      i = LRU_Get(entry->lru);
      LRU_Touch(&entry->lru, i);
      ASSERT(i >= 0 && i < LRU_ASSOC);

      pair = &entry->pairs[i];

      if (pair->count == 0) {
         entry->lastWay = i;
         return pair;
      }
   }
   return NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * KsegGetAndInitNewPair --
 *
 *      Get a kseg pair and initialize it with the given values
 *
 *
 * Results:
 *      The pair. 
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
static inline KSEG_Pair *
KsegGetAndInitNewPair(World_ID worldID, MPN pageNum, uint64 maxAddr,
                      struct KSEGPtrEntry *entry)
{
   MPN lastPageNum;
   struct KSEG_Pair *pair;
   pair = KsegGetNewPair(entry);
   if (pair == NULL) {
      Panic("Kseg: GetPairFromPN: no entries available for %s 0x%x\n",
            (worldID != INVALID_WORLD_ID) ? "ppn" : "mpn", pageNum);
   }

   lastPageNum = MA_2_MPN(maxAddr - 1);

   ASSERT(lastPageNum <= pageNum + 1);

   pair->pageNum = pageNum;
   pair->maxAddr = MPN_2_MA(lastPageNum + 1);
   pair->worldID = worldID;

   return pair;
}

/*
 *----------------------------------------------------------------------
 *
 * KsegGetPairFromPN --
 *
 *      Search the kseg entry for a matching kseg pair, else grab
 *      an available pair and establish a mapping between MPN and the
 *      associated virtual address. 
 *         This function will fail if the PPN is swapped as we do not
 *      want to do any blocking operations in this function. 
 *      Alloc_PhysToMachine returns VMK_WOULD_BLOCK in this case
 *
 * Results:
 *      Returns a valid kseg pair for the MPN or PPN.
 *      In case of failures returns NULL, retStatus contains
 *      the error status
 *
 * Side effects:
 *      Increases the use count on the page
 *      May evict a page with count==0 from the kseg cache.
 *
 *----------------------------------------------------------------------
 */
static inline void *
KsegGetPairFromPN(World_Handle *world, MPN pageNum, uint64 maxAddr,
                  KSEGPtrEntry *entry, KSEG_Pair **resultPair , 
                  VMK_ReturnStatus *retStatus)
{
   int i;
   MPN lastPageNum;
   struct KSEG_Pair *pair;
   World_ID worldID;
   int retryCount = 0;   

   //default return value
   *retStatus = VMK_OK;

again:

   i = 0;
   pair = &entry->pairs[0];
   worldID = (world != NULL) ? world->worldID : INVALID_WORLD_ID;

   do {
      if (pair->pageNum == pageNum && pair->worldID == worldID) {
         void *result;

         LRU_Touch(&entry->lru, i);
         entry->lastWay = i;	 
         if (KSEG_STATS) {
	    if (world == NULL) {
	       ksegStats[MY_PCPU].maHits2++;
	    } else {
	       ksegStats[MY_PCPU].paHits2++;
	    }
         }

         result = KsegIncPair(world, worldID, pair, pageNum, maxAddr, 
                         resultPair, retStatus );
         if (result == NULL) {
            if (*retStatus  == VMK_KSEG_PAIR_FLUSHED) { 
               VMLOG(0, worldID, "page flushed out from under us (1)");
               retryCount++;
               if (retryCount <= MAX_KSEG_RETRIES) {
                  goto again;
               } else {
                  VmWarn(worldID, "page %d yanked too many times (1)",
                            pageNum);
               }
            }
         }
         return result;
      }
      pair++;
      i++;
   } while (i < LRU_ASSOC);

   lastPageNum = MA_2_MPN(maxAddr - 1);
   ASSERT(lastPageNum <= pageNum + 1);

   /*
    * If worldID is INVALID_WORLD_ID, pageNum is an MPN.
    * Otherwise, pageNum is a VM PPN.
    */
   if (worldID == INVALID_WORLD_ID) {
      pair = KsegGetAndInitNewPair(worldID, pageNum, maxAddr, entry); 
      KsegValidate(VA_2_VPN(pair->vaddr), pageNum);
      if (lastPageNum != pageNum) {
         KsegValidate(VA_2_VPN(pair->vaddr + PAGE_SIZE), pageNum + 1);
      }
   } else {
      MPN mpn;
      Alloc_Result result;
      uint32 len = PAGE_SIZE;

      if (lastPageNum != pageNum) {
         len = 2 * PAGE_SIZE;
      }

      /*
       * Obtain the MPN associated with the VMs PPN.  The returned length
       * could be identical to "len" if the machine pages are contiguous.
       */
      *retStatus = Alloc_PhysToMachine(world, PPN_2_PA(pageNum), len,
                                   ALLOC_FAST_LOOKUP, FALSE, &result);
      if (*retStatus != VMK_OK) {
         return NULL ;
      }

      mpn = PA_2_PPN(result.maddr);

      if (lastPageNum != pageNum) {
         MPN mpn1;

         /*
          * If the machine pages associated with paddr and paddr + len are
          * not contiguous, obtain the MPN associated with "lastPageNum".
          */
         if (result.length < len) {
            *retStatus = Alloc_PhysToMachine(world, PPN_2_PA(lastPageNum), 
                                             PAGE_SIZE, ALLOC_FAST_LOOKUP, 
                                             FALSE, &result);
            if (*retStatus != VMK_OK) {
               return NULL;
            }
            mpn1 = PA_2_PPN(result.maddr);
         } else {
            mpn1 = mpn + 1;
         }
         pair = KsegGetAndInitNewPair(worldID, pageNum, maxAddr, entry); 
         KsegValidate(VA_2_VPN(pair->vaddr), mpn);
         KsegValidate(VA_2_VPN(pair->vaddr + PAGE_SIZE), mpn1);
      } else {
         pair = KsegGetAndInitNewPair(worldID, pageNum, maxAddr, entry); 
         KsegValidate(VA_2_VPN(pair->vaddr), mpn);
      }
   }

   KsegPairIncCount(pair);

   {
      /*
       * Check for the match again because this entry could have gotten flushed 
       * after we checked it earlier.  Use a volatile pointer to make sure that
       * it rereads the pair entry from memory.
       */ 
      volatile KSEG_Pair *p2 = (volatile KSEG_Pair *)pair;
      if (p2->pageNum != pageNum || p2->worldID != worldID) {
         VMLOG(0, worldID, "page flushed out from under us (2)");
         KsegPairDecCount(pair);
         retryCount++;
         if (retryCount <= MAX_KSEG_RETRIES) {
            goto again;
         } else {
            VmWarn(worldID, "page %d yanked too many times (2)",
                   pageNum);
            *retStatus = VMK_KSEG_PAIR_FLUSHED ;
            return NULL;
         }
      }
   }

   *resultPair = pair;
   return (void *)pair->vaddr;
}


/*
 *----------------------------------------------------------------------
 *
 * KsegGetPtrFromAddr --
 *      Returns a dereferencable pointer for a given machine or VM physical
 *      address.   maddr is a physical address if world is non-NULL,
 *      else a machine address. This function will return NULL if the
 *      ppn corresponding to the VM physical address is swapped, the
 *      retStatus in this case is VMK_WOULD_BLOCK.
 *
 * Results:
 *      Virtual address pointer in the vmkernel address space.
 *      Returns NULL in case of a failure, retStatus contains the
 *      error status.
 *
 * Side effects:
 *      increases the use count on the page
 *      may evict a page with count==0 from the kseg cache.
 *
 *----------------------------------------------------------------------
 */
static inline void *
KsegGetPtrFromAddr(World_Handle *world, MA maddr, uint64 maxAddr,
                   KSEG_Pair **resultPair, 
                   VMK_ReturnStatus *retStatus)
{
   void *result;
   struct KSEG_Pair *pair;
   MPN pageNum = MA_2_MPN(maddr);
   World_ID worldID = (world) ? world->worldID : INVALID_WORLD_ID;

   struct KSEGPtrEntry *entry = &kseg[pageNum & (NUM_KSEG_PAIRS - 1)];   

   ASSERT(!NMI_IsCPUInNMI() || Panic_IsSystemInPanic());
   ASSERT(!CpuSched_IsPreemptible() || Panic_IsSystemInPanic());

   if (KSEG_STATS) {
      if (world == NULL) {
	 ksegStats[MY_PCPU].maTries++;
      } else {
	 ksegStats[MY_PCPU].paTries++;
      }
   }

   pair = &entry->pairs[entry->lastWay];
   if (pair->pageNum == pageNum && pair->worldID == worldID) {
      if (KSEG_STATS) {
	 if (world == NULL) {
	    ksegStats[MY_PCPU].maHits++;
	 } else {
	    ksegStats[MY_PCPU].paHits++;
	 }
      }
      LOG(2, "HIT for %s 0x%x @ 0x%x",
          (worldID != INVALID_WORLD_ID) ? "PPN" : "MPN", 
          pageNum, pair->vaddr);

      result = KsegIncPair(world, worldID, pair, pageNum, maxAddr, 
                           resultPair, retStatus);

      if (result != NULL) {
         ASSERT(*retStatus == VMK_OK);
         return result + (maddr & PAGE_MASK);
      }

      if (*retStatus == VMK_WOULD_BLOCK) {
         return NULL;
      }
   }
   result = KsegGetPairFromPN(world, pageNum, maxAddr, entry,
                              resultPair, retStatus);
   if (result != NULL) {
      result += (maddr & PAGE_MASK);
   }
   return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Kseg_GetPtrFromMA --
 *
 *      Maps the given machine address and returns a dereferencable pointer.
 *
 * Results:
 *      Virtual address pointer in the vmkernel address space.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void *
Kseg_GetPtrFromMA(MA maddr, uint32 length, KSEG_Pair **pair)
{
   uint32 eflags;
   void *vaddr;   
   VMK_ReturnStatus vmkStatus;

   /*
    * MPNs must be in the range 0 - 0xFFFF.FFFF>>PAGE_SHIFT,
    * (0xF.FFFF.FFFF>>PAGE_SHIFT if PAE is enabled).
    */
   ASSERT((MA_2_MPN(maddr) & 0xFF000000) == 0);
   ASSERT(length <= PAGE_SIZE);

   SAVE_FLAGS(eflags);
   if (eflags & EFLAGS_IF) {
      CLEAR_INTERRUPTS();
      vaddr = KsegGetPtrFromAddr(NULL, maddr, maddr + length,
                                 pair, &vmkStatus);
      RESTORE_FLAGS(eflags);
   } else {
      vaddr = KsegGetPtrFromAddr(NULL, maddr, maddr + length,
                                 pair, &vmkStatus);
   }

   if (vaddr == NULL) {
      LOG(0, "error mapping maddr = %#Lx: %#x", maddr, vmkStatus);
      ASSERT(FALSE);
   }

   LOG(3, "mapping machine address %#Lx at virtual addr 0x%p", maddr, vaddr);

   return vaddr;
}


/*
 *----------------------------------------------------------------------
 *
 * Kseg_GetPtrIRQFromMA --
 *
 *      Maps the given machine address and returns a dereferencable pointer.
 *      Requires that interrupts are disabled and at most a page is mapped.
 *
 * Results:
 *      Virtual address pointer in the vmkernel address space.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void *
Kseg_GetPtrIRQFromMA(MA maddr, uint32 length, KSEG_Pair **pair)
{
   void *vaddr;
   VMK_ReturnStatus vmkStatus ;

   ASSERT(length <= PAGE_SIZE);
   ASSERT_NO_INTERRUPTS();

   vaddr = KsegGetPtrFromAddr(NULL, maddr, maddr + length, 
                              pair, &vmkStatus);

   ASSERT(vaddr != NULL);

   LOG(3, "mapping machine address %#Lx at virtual addr 0x%p", maddr, vaddr);

   return vaddr;
}


/*
 *----------------------------------------------------------------------
 *
 * Kseg_ReleasePtr --
 *
 *      Decrement the count on this kseg ptr.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The count is decremented on this pair. 
 *
 *----------------------------------------------------------------------
 */
void
Kseg_ReleasePtr(KSEG_Pair *pair)
{
   uint32 eflags;
   SAVE_FLAGS(eflags);
   if (eflags & EFLAGS_IF) {
      CLEAR_INTERRUPTS();
      KsegPairDecCount(pair);
      RESTORE_FLAGS(eflags);
   } else {
      KsegPairDecCount(pair);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * Kseg_Flush --
 *
 *      Flush every entry from the kseg that has a count of 0.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The count is decremented on this pair. 
 *
 *----------------------------------------------------------------------
 */
void
Kseg_Flush(void)
{
   int i;
   for (i = 0; i < NUM_KSEG_PAIRS; i++) {
      int j;
      for (j = 0; j < LRU_ASSOC; j++) {
	 if (kseg[i].pairs[j].count == 0) {
	    kseg[i].pairs[j].pageNum = 0xffffffff;
	 }
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * Kseg_GetPtrFromPA --
 *         Maps the given VM physical address and returns a dereferencable
 *      pointer. If canBlock is set this mapping could result in a blocking 
 *      operation, if the PPN corresponding to the VM physical address
 *      is swapped to disk.
 *         The canBlock parameter should be set to TRUE if the calling function
 *      can afford to block. Otherwise this parameter should be set to FALSE.
 *
 * Results:
 *      Virtual address pointer in the vmkernel address space.
 *      Returns NULL on failure. 
 *      In case of a failure, retStatus contains the error status. VMK_WOULD_BLOCK
 *      is the most likely error status, which indicates that the ppn is 
 *      swapped and a blocking read is required.
 *
 * Side effects:
 *      May block if canBlock is set.
 *
 *----------------------------------------------------------------------
 */
void *
Kseg_GetPtrFromPA(World_Handle *world, PA paddr, uint32 length, Bool canBlock,
                  KSEG_Pair **pair, VMK_ReturnStatus *retStatus)
{
   void *vaddr;   
   uint32 eflags;
   World_Handle *leader;

   if (World_IsPOSTWorld(world)) {
      leader = world;
   } else {
      ASSERT(World_IsVMMWorld(world));
      leader = World_GetVmmLeader(world);
   }
   ASSERT(leader);

   ASSERT(length <= (2 * PAGE_SIZE));
retry:
   // Default return value
   *retStatus = VMK_OK;

   SAVE_FLAGS(eflags);
   if (eflags & EFLAGS_IF) {
      CLEAR_INTERRUPTS();
      vaddr = KsegGetPtrFromAddr(leader, paddr, paddr + length, 
                                 pair, retStatus);
      RESTORE_FLAGS(eflags);
   } else {
      vaddr = KsegGetPtrFromAddr(leader, paddr, paddr + length, 
                                 pair, retStatus);
   }

   if ((*retStatus == VMK_WOULD_BLOCK) && canBlock) {
      Alloc_Result result;

      if (!(eflags & EFLAGS_IF)) {
         SysAlert("Cannot block with interrupts disabled");
         ASSERT(0);
         return NULL;
      }
      // It is ok to block here and try the kseg mapping again if we succeed.
      *retStatus = Alloc_PhysToMachine(leader, paddr, length, ALLOC_FAST_LOOKUP, 
                                       canBlock, &result);
      if (*retStatus == VMK_OK) {
         if (result.length < length) {
            // get the mapping for the second page
            *retStatus = Alloc_PhysToMachine(leader, paddr+PAGE_SIZE, PAGE_SIZE,
                                          ALLOC_FAST_LOOKUP, canBlock, &result);
         }
         if (*retStatus == VMK_OK) {
            LOG(1, "Retrying to map PA(0x%llx after doing Alloc_PhysToMachine", 
                paddr);
            goto retry;
         }
      } 
      if (*retStatus != VMK_OK) {
         Warning("Alloc_PhysToMachine failed status %d", *retStatus);
         return NULL;
      }
   }

   LOG(3, "mapping VM physical address %#Lx at virtual addr 0x%p", paddr, vaddr);
   return vaddr;
}

/*
 *----------------------------------------------------------------------
 *
 * KsegInvalidatePtrInt --
 *
 *      Internal routine for invalidating the dereferencable pointer
 *	associated with the VM PPN "ppn" for "world" if it extends
 *	past "maxAddr".  Note that the MMU translation is updated when
 *	the kseg pair is re-used.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Modifies global "kseg" table for local PCPU.
 *
 *----------------------------------------------------------------------
 */
static void
KsegInvalidatePtrInt(World_Handle *world, PPN ppn, uint64 maxAddr)
{
   struct KSEGPtrEntry *entry = &kseg[ppn & (NUM_KSEG_PAIRS - 1)];
   struct KSEG_Pair *pair;

   // sanity checks
   ASSERT_NO_INTERRUPTS();
   ASSERT(world != NULL);

   /*
    * Use the hint to obtain the kseg pair.
    */
   pair = &entry->pairs[entry->lastWay];
   if ((pair->pageNum == ppn) &&
       (pair->worldID == world->worldID) &&
       (pair->maxAddr > maxAddr)) {
      ASSERT(pair->count == 0);
      KsegPairInvalidate(pair);
      pair->lastWay = (entry->lastWay + 1) % LRU_ASSOC;
      VMLOG(1, world->worldID, 
            "local invalidate mappg PPN 0x%x at vaddr 0x%x", 
            ppn, pair->vaddr);
   } else {
      int i = 0;

      /*
       * Check all kseg pairs.
       */
      pair = &entry->pairs[0];
      do {
         if ((pair->pageNum == ppn) &&
             (pair->worldID == world->worldID) &&
             (pair->maxAddr > maxAddr)) {
            ASSERT(pair->count == 0);
            KsegPairInvalidate(pair);
            pair->lastWay = (i + 1) % LRU_ASSOC;
            VMLOG(1, world->worldID, 
                  "local invalidate mappg PPN 0x%x at vaddr 0x%x",
                  ppn, pair->vaddr);
            break;
         }
         pair++;
         i++;
      } while (i < LRU_ASSOC);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * Kseg_InvalidatePtr --
 *
 *      Invalidate the dereferencable pointer associated with the
 *      given VM PPN.  The MMU translation will be updated when the
 *      kseg pair gets re-used.
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
Kseg_InvalidatePtr(World_Handle *world, PPN ppn)
{
   uint32 eflags;

   // sanity checks
   ASSERT(world != NULL);

   // disable interrupts, if necessary
   SAVE_FLAGS(eflags);
   if (eflags & EFLAGS_IF) {
      CLEAR_INTERRUPTS();
   }

   // A kseg mapping spans at most two pages.  To invalidate ppn,
   // need to invalidate both the mapping starting at ppn, and also
   // any mapping starting at (ppn - 1) that spans two pages.
   KsegInvalidatePtrInt(world, ppn, 0);
   KsegInvalidatePtrInt(world, ppn - 1, PPN_2_PA(ppn));

   // restore interrupt state
   if (eflags & EFLAGS_IF) {
      RESTORE_FLAGS(eflags);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * KsegFlushRemoteInt --
 *
 *	Internal routine for invalidating any remote kseg entries
 *	associated with the VM PPN "ppn" for "world" if it extends
 *	past "maxAddr".  The "canWait" parameter specifies whether
 *	the caller can wait for active users of a kseg pair to finish.
 *	If waiting is required to complete the flush and "canWait"
 *	is FALSE, the operation aborts and returns FALSE.
 *
 * Results:
 *      Returns TRUE iff flush successful.
 *
 * Side effects:
 *      The kseg entry for this PPN is invalidated on remote PCPUs.
 *
 *----------------------------------------------------------------------
 */
static Bool
KsegFlushRemoteInt(World_ID worldID, PPN ppn, uint64 maxAddr, Bool canWait)
{
   PCPU pcpu;

   ASSERT(!CpuSched_IsPreemptible() || Panic_IsSystemInPanic());
   for (pcpu = 0; pcpu < numPCPUs; pcpu++) {
      volatile KSEG_Pair *pair;
      KSEGPtrEntry *thisKseg;
      KSEGPtrEntry *entry;
      int i;      

      if (pcpu == myPRDA.pcpuNum)  {
	 continue;
      }

      thisKseg = ksegs[pcpu];
      entry = &thisKseg[ppn & (NUM_KSEG_PAIRS - 1)];

      i = 0;

      /*
       * Check all kseg pairs.  Note that all associative ways must
       * be checked, including entries with invalid pairs.  This is
       * because a previous call may have aborted without waiting for
       * an invalidated pair's users to finish.  
       */
      pair = &entry->pairs[0];
      do {
         if (KsegPairIsInvalid(pair) ||
             ((pair->pageNum == ppn) &&
              (pair->worldID == worldID) &&
              (pair->maxAddr > maxAddr))) {
            VMLOG(1, worldID, 
                  "remote invalidate mapping PPN 0x%x at vaddr 0x%x on cpu %d",
                  ppn, pair->vaddr, pcpu);
            KsegPairInvalidate(pair);
	    if (pair->count > 0) {
	       /* 
		* Wait for any users of this kseg pair to finish.  They are 
		* guaranteed to be on a remote CPU so spin waiting here is OK.
		*/	    
	       uint64 maxTSC = RDTSC() + Timer_USToTSC(KSEG_FLUSH_MAX_US_WAIT);
   
               /*
                * Fail if unable to wait for users of kseg pair to finish.
                */
               if (!canWait) {
                  return(FALSE);
               }

	       while (pair->count > 0 && pair->pageNum == INVALID_MPN &&
                      RDTSC() < maxTSC) {
                  PAUSE();
	       }
	       if (pair->count > 0 && pair->pageNum == INVALID_MPN) {
		  Panic("Kseg: vm %d: remote invalidate timeout "
                        "(%d usec) for PPN 0x%x on cpu %d\n",
                        worldID, KSEG_FLUSH_MAX_US_WAIT, ppn, pcpu);
	       }
	    }
         }
         pair++;
         i++;
      } while (i < LRU_ASSOC);
   }

   return(TRUE);
}

/*
 *----------------------------------------------------------------------
 *
 * Kseg_FlushRemote --
 *
 *      Flush "ppn" from the kseg caches on remote pcpus.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      All kseg entries for "ppn" are flushed on other pcpus.
 *
 *----------------------------------------------------------------------
 */
void
Kseg_FlushRemote(World_ID worldID, PPN ppn)
{
   // A kseg mapping spans at most two pages.  To invalidate ppn,
   // need to invalidate both the mapping starting at ppn, and also
   // any mapping starting at (ppn - 1) that spans two pages.
   KsegFlushRemoteInt(worldID, ppn, 0, TRUE);
   KsegFlushRemoteInt(worldID, ppn - 1, PPN_2_PA(ppn), TRUE);
}

/*
 *----------------------------------------------------------------------
 *
 * Kseg_CheckRemote --
 *
 *      Check if "ppn" is in use by any kseg caches on remote pcpus.
 *	Flushes any inactive (i.e., no current users) matching kseg
 *	entries	that are found.  Returns TRUE if active kseg entries
 *	are found, otherwise FALSE.
 *
 * Results:
 *      Returns TRUE if any active kseg entries containing "ppn" are
 *	found on remote pcpus, otherwise FALSE.
 *
 * Side effects:
 *      Kseg entries for "ppn" may be flushed on other pcpus.
 *
 *----------------------------------------------------------------------
 */
Bool
Kseg_CheckRemote(World_ID worldID, PPN ppn)
{
   // A kseg mapping spans at most two pages.  To check for ppn,
   // need to check both the mapping starting at ppn, and also
   // any mapping starting at (ppn - 1) that spans two pages.
   if (KsegFlushRemoteInt(worldID, ppn, 0, FALSE) &&
       KsegFlushRemoteInt(worldID, ppn - 1, PPN_2_PA(ppn), FALSE)) {
      // no remote kseg contains ppn
      return(FALSE);
   }

   // remote kseg contains ppn
   return(TRUE);
}

/*
 *----------------------------------------------------------------------
 *
 * KsegPOST --
 *
 *      Performs simple tests of Kseg_GetPtrFrom(MA|PA): 
 *      writes & reads from pages distributed across a VMs address
 *      space.  
 *      
 *
 * Results:
 *      FALSE if error detected, TRUE otherwise
 *
 * Side effects:
 *
 *----------------------------------------------------------------------
 */

#define POST_KSEG_VA_START 0x10000
#define POST_KSEG_PAIRS 32

/* 
 * There is a race condition when trying to allocate all available
 * memory, with each POST world grabbing an equal portion.  The
 * available memory could change in between calculating each portion
 * and the actual allocation.  Thus first subtract out 1 MB of "slop"
 * before before calculating each processor's portion.  Also, avoid
 * allocating more than 2GB per world to prevent problems with max
 * world size and other possible 32-bit overflow conditions.
 */
#define	POST_PAGES_MB	(256)
#define POST_PAGES_SLOP	(POST_PAGES_MB)
#define	POST_PAGES_MAX	(2048 * POST_PAGES_MB)

static Bool
KsegPOST(UNUSED_PARAM(void *clientData), int id,
         UNUSED_PARAM(SP_SpinLock *lock), SP_Barrier *barrier)
{
   VMK_ReturnStatus status;
   World_ID worldID = MY_RUNNING_WORLD->worldID;
   KSEG_Pair *pairPA, *pairMA;
   uint32 vaPages, vaSize;
   Bool postFailed = FALSE;
   int32 freePages;
   int i;
   int32 reservedMem, autoMinMem, availSwap, reservedSwap;
   World_ID *data;

   // determine currently-available free pages from the
   // mem sched module
   MemSched_CheckReserved(&freePages, &reservedMem, &autoMinMem, 
                          &availSwap, &reservedSwap);
   freePages -=  POST_PAGES_SLOP;
   ASSERT(freePages > 0);

   // allocate equal portions for each POST world (one per processor)
   vaPages = (freePages - POST_PAGES_SLOP) / numPCPUs;
   vaPages = MIN(vaPages, POST_PAGES_MAX);
   vaSize = PAGES_2_BYTES(vaPages);

   SP_SpinBarrier(barrier);

   status = Alloc_POSTWorldInit(MY_RUNNING_WORLD, vaPages);
   if (status != VMK_OK) { 
      Warning("Alloc_MemMap failed on post cpu %d", id);
      postFailed = TRUE;
   }

   SP_SpinBarrier(barrier);
   if (postFailed) {
      // OK to return only after all POST threads have reached the same barrier
      return(FALSE);
   }

   // Now test POST_KSEG_PAIRS physical addresses (evenly distributed 
   // across whole address range)
   ASSERT(POST_KSEG_PAIRS < BYTES_2_PAGES(vaSize));
   for (i = 0; i < POST_KSEG_PAIRS; i++) {
      VMK_ReturnStatus retStatus;
      data = Kseg_GetPtrFromPA(MY_RUNNING_WORLD,
                               i*(vaSize/POST_KSEG_PAIRS),PAGE_SIZE, TRUE,
                               &pairPA, &retStatus);
      ASSERT(data != NULL);
      *data = worldID + i;
      Kseg_ReleasePtr(pairPA);
   } 

   SP_SpinBarrier(barrier);

   // Now test same addresses using MAs
   for (i = 0; i < POST_KSEG_PAIRS; i++) {
      PPN ppn = PA_2_PPN(i * (vaSize / POST_KSEG_PAIRS));
      MPN mpn;
      VMK_ReturnStatus status;
      
      status = Alloc_PageFault(MY_RUNNING_WORLD, ppn, TRUE, &mpn);
      ASSERT(status == VMK_OK);

      data = Kseg_GetPtrFromMA(MPN_2_MA(mpn),PAGE_SIZE, &pairMA) + 
	 i * (vaSize/POST_KSEG_PAIRS) % PAGE_SIZE;
      if(*data != worldID +i) {
	 postFailed = TRUE;
	 Warning("PA/MA lookup value mismatch: id=%d, mpn=0x%x, *data=%d",
                 id, mpn, *data);
      }
     Kseg_ReleasePtr(pairMA);
   }

   SP_SpinBarrier(barrier);

   // TODO: Now try timing --make sure that cache is working correctly

   SP_SpinBarrier(barrier);

   // cleanup alloc for this world -- generally system worlds don't use alloc
   Alloc_POSTWorldCleanup(MY_RUNNING_WORLD);

   return !postFailed;
}

/* TODO
 *	-more stressful tests
 *	-check that caching works / is safe.
 * 	-check that caching actually speeds up secondary accesses.
 */


/*
 *----------------------------------------------------------------------
 *
 * Kseg_Dump --
 *
 *      Dumps the kseg structure to the coredump.  First the kseg page table
 *      pages, then the kseg data structure pages, and finally the
 *      valid mappings in the kseg region itself.
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
Kseg_Dump(void)
{
   VA va;
   int i, ksegPages;
   VMK_ReturnStatus status = VMK_OK;

   // gap between PRDA page and kseg pagetable pages
   for (va = VMK_FIRST_PRDA_ADDR + PAGE_SIZE;
        va < VMK_KSEG_PTABLE_ADDR; va += PAGE_SIZE) {
      status = Dump_Page(0, "Kseg zero");
      if (status != VMK_OK) {
         return status;
      }
   }

   // kseg pagetable pages
   for (i = 0; i < VMK_NUM_KSEG_PDES; i++) {
      status = Dump_Page(va, "Kseg ptable page");
      if (status != VMK_OK) {
         return status;
      }
      va += PAGE_SIZE;
   }

   // gap between kseg pagetable pages and kseg data structure pages
   for (; va < VMK_KSEG_PTR_BASE; va += PAGE_SIZE) {
      status = Dump_Page(0, "Kseg zero");
      if (status != VMK_OK) {
         return status;
      }
   }

   // kseg data structure pages
   ksegPages = (NUM_KSEG_PAIRS * sizeof(KSEGPtrEntry) + PAGE_SIZE - 1) / PAGE_SIZE;
   for (i = 0; i < ksegPages; i++) {
      VA kvmapKseg = (VA)ksegs[myPRDA.pcpuNum];
      MPN mpn = TLB_GetMPN(kvmapKseg + i*PAGE_SIZE);
      if (VMK_IsValidMPN(mpn)) {
         status = Dump_Page(va, "Kseg pages page");
      } else {
         status = Dump_Page(0, "Kseg pages page");
      }
      if (status != VMK_OK) {
         return status;
      }
      va += PAGE_SIZE;
   }

   // gap till the end of prda region
   for (; va < VMK_FIRST_PRDA_ADDR + VMK_NUM_PRDA_PDES*PDE_SIZE; va += PAGE_SIZE) {
      status = Dump_Page(0, "Kseg zero");
      if (status != VMK_OK) {
         return status;
      }
   }

   // the kseg itself
   ASSERT(va == VMK_KSEG_MAP_BASE);
   for (; va < VMK_KSEG_MAP_BASE + VMK_KSEG_MAP_LENGTH; va += PAGE_SIZE) {
      VPN vpn = VA_2_VPN(va);
      VMK_PTE pte = ksegPT[vpn - VMK_FIRST_KSEG_VPN];
      if (PTE_PRESENT(pte) && VMK_IsValidMPN(VMK_PTE_2_MPN(pte))) {
         status = Dump_Page(va, "Kseg page");
      } else {
         status = Dump_Page(0, "Kseg page");
      }
      if (status != VMK_OK) {
         return status;
      }
   }

   return VMK_OK;
}


/*
 *-----------------------------------------------------------------------------
 *
 * KsegStatsProcRead --
 *
 *	Callback for read operation on /proc/vmware/kseg procfs node.
 *	Formats per-PCPU Kseg statistics.
 *
 * Results:
 *	Returns VMK_OK.
 *
 * Side effects:
 *	Resets stats.
 *
 *-----------------------------------------------------------------------------
 */
static int
KsegStatsProcRead(UNUSED_PARAM(Proc_Entry *entry), char *buf, int *len)
{
   uint32 totalHits, totalTries;
   PCPU p;

   // initialize
   totalHits = 0;
   totalTries = 0;
   *len = 0;

   // format header
   Proc_Printf(buf, len,
               "cpu  type "
               "     hits (     hit1 +      hit2) "
               "   access hit%%\n");

   for (p = 0; p < numPCPUs; p++) {
      KSEGStats *stats = &ksegStats[p];
      uint32 paTries, maTries, tries;
      uint32 paHits, maHits, hits;

      // snapshot hits
      paHits = stats->paHits + stats->paHits2;
      maHits = stats->maHits + stats->maHits2;
      hits  = paHits + maHits;

      // snapshot tries
      paTries = stats->paTries;
      maTries = stats->maTries;
      tries = paTries + maTries;

      // update totals
      totalHits += hits;
      totalTries += tries;
      
      // format stats
      Proc_Printf(buf, len,
                  "%3u     P %9u (%9u + %9u) %9u %4u\n"
                  "%3u     M %9u (%9u + %9u) %9u %4u\n"
                  "%3u   P+M %9u (%9u + %9u) %9u %4u\n",
                  // PA
                  p,
                  paHits,
                  stats->paHits,
                  stats->paHits2,
                  paTries,
                  (paTries > 0) ? ((100 * paHits) / paTries) : 0,
                  // MA
                  p,
                  maHits,
                  stats->maHits,
                  stats->maHits2,
                  maTries,
                  (maTries > 0) ? ((100 * maHits) / maTries) : 0,
                  // PA + MA
                  p,
                  hits,
                  stats->paHits + stats->maHits,
                  stats->paHits2 + stats->maHits2,
                  tries,
                  (tries > 0) ? ((100 * hits) / tries) : 0);
   }
   
   // format totals
   Proc_Printf(buf, len,
               "TOT   P+M %9u                         %9u %4u\n",
               totalHits,
               totalTries,
               (totalTries > 0) ? ((100 * totalHits) / totalTries) : 0);

   return(VMK_OK);
}

/*
 *-----------------------------------------------------------------------------
 *
 * KsegStatsProcWrite --
 *
 *	Callback for write operation on /proc/vmware/kseg procfs node.
 *	Resets all Kseg stats when "reset" written to node.
 *
 * Results:
 *	Returns VMK_OK or VMK_BAD_PARAM.
 *
 * Side effects:
 *	Resets stats.
 *
 *-----------------------------------------------------------------------------
 */
static int
KsegStatsProcWrite(UNUSED_PARAM(Proc_Entry *entry), char *buf, UNUSED_PARAM(int *len))
{
   if (strncmp(buf, "reset", 5) == 0) {
      PCPU p;
      for (p = 0; p < numPCPUs; p++) {
         KSEGStats *stats = &ksegStats[p];
         memset(stats, 0, sizeof(KSEGStats));
      }
      Log("Reset Kseg statistics");
   } else {
      Log("Command not understood");
      return(VMK_BAD_PARAM);
   }

   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * Kseg_MapRegion
 *
 *      Map the Kseg region for the given CPU into the given page table root
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
Kseg_MapRegion(PCPU pcpu, MA pageRoot)
{
   int i;
   KSEG_Pair *dirPair;
   LA laddr = VMK_VA_2_LA(VMK_FIRST_KSEG_ADDR);
   VMK_PDE *pageDir;

   pageDir = PT_GetPageDir(pageRoot, laddr, &dirPair);
   if (pageDir == NULL) {
      return VMK_NO_RESOURCES;
   }
   for (i = 0; i < VMK_NUM_KSEG_PDES; i++) {
      PT_SET(&pageDir[ADDR_PDE_BITS(laddr)],
             VMK_MAKE_PDE(ksegPTableMPNs[pcpu][i], 0, PTE_KERNEL));
      laddr += PDE_SIZE;
   }
   PT_ReleasePageDir(pageDir, dirPair);

   return VMK_OK;
}
