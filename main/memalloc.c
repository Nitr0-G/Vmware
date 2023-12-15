/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * memalloc.c --
 *
 *	Manages main vmkernel heap and read-only region.
 */

#include "vm_types.h"
#include "vmkernel.h"
#include "memalloc.h"
#include "splock.h"
#include "vm_libc.h"
#include "timer.h"
#include "host.h"
#include "kseg.h"
#include "pagetable.h"
#include "tlb.h"
#include "kvmap.h"
#include "memmap_dist.h"
#include "hash.h"
#include "heap_int.h"

#define LOGLEVEL_MODULE Mem
#include "log.h"

/*
 * Lock ranks
 */
#define SP_RANK_MEMROLOCK (SP_RANK_STATIC_HEAPLOCK - 1)

// the main vmkernel heap
Heap_ID mainHeap;

/* Base vmkernel address for reserved alloc space 
 * "initAllocBase" - heap start for Mem_AllocEarly() calls.
 * "allocBase" - heap start after Mem_AllocEarly() (for Mem_Alloc() calls). 
 */
static char  *allocBase; 
static VA initAllocBase;

/* MPN of the first page in the vmkernel CODEDATA region */
static MPN vmkFirstMPN;

static Bool memInitialized;

static Bool memROIsWritable = FALSE;

// checksum of main vmkernel code region
static uint64 memROChecksum = 0;

void
Mem_EarlyInit(VMnix_Init *vmnixInit)
{
   allocBase = (char *)VPN_2_VA(vmnixInit->nextVPN);
   initAllocBase = (VA)allocBase;
   vmkFirstMPN = vmnixInit->firstMPN;
}

void *
Mem_AllocEarly(uint32 size, uint32 alignment)
{
   VA ret;

   ASSERT(alignment != 0);
   
   if (size == 0) {
      return NULL;
   }

   ret = ALIGN_UP((VA)(allocBase), alignment);
   allocBase = (char *)(ret + size);

   ASSERT(!memInitialized);

   return (void *)ret;
}

/*
 *----------------------------------------------------------------------
 *
 * Mem_Init --
 *
 *      Initialize the memory allocator.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The internal data structures are initialized.
 *
 *----------------------------------------------------------------------
 */
void
Mem_Init(void)
{
   int reservedMem;

   memInitialized = TRUE;
   
   allocBase = (char *)ALIGN_UP((VA)allocBase, PAGE_SIZE);
   reservedMem = (char *)VMK_FIRST_MAP_ADDR - allocBase;
   ASSERT((reservedMem & PAGE_MASK) == 0);
   LOG(0, "alloc space starts at %p, # pages is %d",
       allocBase, reservedMem / PAGE_SIZE);

   mainHeap = Heap_CreateStatic("mainHeap", allocBase, reservedMem);
}

/*
 *----------------------------------------------------------------------
 *
 * Mem_VA2MPN --
 *
 *      Translate virtual address of allocated memory to machine address.
 *      Will work on vmkernel code+rodata+data+bss
 *
 * Results:
 *      MPN of virtual address.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
MPN
Mem_VA2MPN(VA address)
{
   ASSERT(address >= VMK_FIRST_ADDR);
   ASSERT(address < VMK_FIRST_ADDR + PAGES_2_BYTES(VMK_NUM_CODEHEAP_PAGES));

   return vmkFirstMPN + VA_2_VPN(address - VMK_FIRST_ADDR);
}

/*
 *----------------------------------------------------------------------
 *
 * Mem_MA2VPN --
 *
 *      Translate machine address of allocated memory to virtual address.
 *      Will work on addresses from Mem_AllocEarly().
 *
 * Results:
 *      VPN of machine address, or INVALID_VPN if address outside heap
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VPN
Mem_MA2VPN(MA address)
{
   // Make sure "address" came from MemAlloc'd memory.
   if ((MA_2_MPN(address) < Mem_VA2MPN(initAllocBase)) ||
       (MA_2_MPN(address) > Mem_VA2MPN(VMK_FIRST_MAP_ADDR - 1))) {
      return INVALID_VPN;
   }
   /*
    * Uses the fact that the machine pages of the memory pool are
    * allocated contiguously.
    */
   return VA_2_VPN(VMK_FIRST_ADDR) + (MA_2_MPN(address) - vmkFirstMPN);
}

/*
 *----------------------------------------------------------------------
 *
 * Mem_SetIOProtection --
 *
 *      Mark the entire heap to allow I/O to it
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
Mem_SetIOProtection(void)
{
   MPN mpn;

   for (mpn = Mem_VA2MPN(initAllocBase);
        mpn <= Mem_VA2MPN(VMK_FIRST_MAP_ADDR - 1);
        mpn++) {
      MemMap_SetIOProtection(mpn, MMIOPROT_IO_ENABLE);
   }
}

/* ------------------------------------
 * Read-only area management
 * ------------------------------------
 */
static SP_SpinLockIRQ memROLock;
typedef struct MemRODesc {
   struct MemRODesc 	*next;
   VA			data;
   uint32		length;
} MemRODesc;

static MemRODesc *freeReadOnlyData, *inuseReadOnlyData;

/*
 *-----------------------------------------------------------------------------
 *
 * MemRO_EarlyInit -- 
 *
 *      Initialize the checksum of the read-only memory
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
void
MemRO_EarlyInit(void)
{
   memROChecksum = MemRO_CalcChecksum();
}

/*
 *-----------------------------------------------------------------------------
 *
 * MemRO_Init -- 
 *
 *      Initialize the read-only memory management
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
void
MemRO_Init(VMnix_StartupArgs *startupArgs)
{
   ASSERT(SP_RANK_MEMROLOCK > SP_RANK_IRQ_MEMTIMER);
   SP_InitLockIRQ("MemReadOnly", &memROLock, SP_RANK_MEMROLOCK);
   freeReadOnlyData = (MemRODesc *)Mem_Alloc(sizeof(MemRODesc));
   ASSERT(freeReadOnlyData != NULL);

   freeReadOnlyData->data = ALIGN_UP(startupArgs->endReadOnly, PAGE_SIZE);
   freeReadOnlyData->length = 
      VMK_FIRST_ADDR + VMK_NUM_CODE_PAGES * PAGE_SIZE - freeReadOnlyData->data;

   Log("endReadOnly=0x%x data=0x%x length=0x%x", 
       startupArgs->endReadOnly, freeReadOnlyData->data, freeReadOnlyData->length);
}

/*
 *-----------------------------------------------------------------------------
 *
 * MemRO_ChangeProtection --
 *
 *      Change the protection of this world's code to writable or read-only.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The protection on this world's code is changed.
 *
 *-----------------------------------------------------------------------------
 */
void
MemRO_ChangeProtection(Bool writable)
{
   int i;
   MA cr3;
   LA firstLA;
   KSEG_Pair *pair = NULL;
   KSEG_Pair **pPair = NULL;

   if (writable) {
      /*
       * Check for corruption in main vmkernel code region unless multiple
       * ChangeProtection(MEMRO_WRITAABLE)'s have been called in a row. This
       * exception should only happen in rare occasions, like when you're in a
       * debugging loop.
       */
      if (!memROIsWritable) {
	 uint64 checksum = MemRO_CalcChecksum();
	 if (checksum != memROChecksum) {
	    Panic("VMKernel: checksum BAD: 0x%Lx 0x%Lx",
		  checksum, memROChecksum);
	 }
	 memROIsWritable = TRUE;
      }
   } else {
      memROChecksum = MemRO_CalcChecksum();
      memROIsWritable = FALSE;
   }

   if (vmkernelLoaded) {
      pPair = &pair;
   }

   if (vmkernelInEarlyInit) {
      firstLA = VMNIX_VMK_FIRST_LINEAR_ADDR;
   } else {
      firstLA = VMK_FIRST_LINEAR_ADDR;
   }
   GET_CR3(cr3);

   for (i = 0; i < VMK_NUM_CODE_PDES; i++) {
      VMK_PDE *pdir;
      LA laddr = firstLA + i * PDE_SIZE;

      pdir = PT_GetPageDir(cr3, laddr, pPair);
      if (pdir != NULL) {
	 if (pdir[ADDR_PDE_BITS(laddr)] & PTE_PS) {
	    if (writable) {
	       pdir[ADDR_PDE_BITS(laddr)] |= PTE_RW;
	    } else {
	       pdir[ADDR_PDE_BITS(laddr)] &= ~PTE_RW;
	    }
	    PT_ReleasePageDir(pdir, pair);
	    continue;
	 }
	 PT_ReleasePageDir(pdir, pair);

	 for (; laddr < firstLA + (i + 1) * PDE_SIZE; laddr += PAGE_SIZE) {
	    VMK_PTE *pTable = PT_GetPageTable(cr3, laddr, pPair);
	    if (pTable) {
	       if (writable) {
		  pTable[ADDR_PTE_BITS(laddr)] |= PTE_RW;
	       } else {
		  pTable[ADDR_PTE_BITS(laddr)] &= ~PTE_RW;
	       }
	       PT_ReleasePageTable(pTable, pair);
	    } else {
	       Log("PT NULL, cr3=0x%"FMT64"x, laddr=%x", cr3, laddr);
	       break;
	    }
	 }
      } else {
	 Log("pdir NULL, cr3=0x%"FMT64"x, laddr=%x", cr3, laddr);
	 break;
      }      
   }

   TLB_Flush(TLB_LOCALONLY);
}

/*
 *-----------------------------------------------------------------------------
 *
 * MemRO_IsWritable --
 *
 *      Returns true if the MemRO region is writable.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
Bool
MemRO_IsWritable(void)
{
   return memROIsWritable;
}

/*
 *----------------------------------------------------------------------
 *
 * MemRODump --
 *
 *      Print out the read-only free and inuse lists.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
MemRODump(char *str)
{
   MemRODesc *mdd;

   ASSERT(SP_IsLockedIRQ(&memROLock));

   Log("%s: FREE READ-ONLY LIST:", str);
   for (mdd = freeReadOnlyData; mdd != NULL; mdd = mdd->next) {
      Log("0x%-10x for 0x%x bytes", mdd->data, mdd->length);
   }

   Log("%s: INUSE READ-ONLY LIST:", str);
   for (mdd = inuseReadOnlyData; mdd != NULL; mdd = mdd->next) {
      Log("0x%-10x for 0x%x bytes", mdd->data, mdd->length);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * MemROAllocLocked --
 *
 *      Allocate a chunk of data from the read-only data pool.
 *
 * Results: 
 *	Pointer to allocated read-only data.
 *
 * Side effects:
 *	The freeReadOnlyData and inuseReadOnlyData lists are modified.
 *
 *----------------------------------------------------------------------
 */
static void *
MemROAllocLocked(uint32 length)
{
   MemRODesc *freeMDD, *prev;
   ASSERT(SP_IsLockedIRQ(&memROLock));

   length = ALIGN_UP(length, PAGE_SIZE);
   for (prev = NULL, freeMDD = freeReadOnlyData; 
        freeMDD != NULL; 
	prev = freeMDD, freeMDD = freeMDD->next) {
      if (length <= freeMDD->length) {
	 void *retVal;

         MemRODesc *inuseMDD;

         inuseMDD = (MemRODesc *)Mem_Alloc(sizeof(MemRODesc));
         if (inuseMDD == NULL) {
            Warning("Couldn't allocated inuse descriptor");
            return NULL;
         }

	 inuseMDD->data = freeMDD->data;
	 inuseMDD->length = length;
	 inuseMDD->next = inuseReadOnlyData;
	 inuseReadOnlyData = inuseMDD;

	 retVal = (void *)freeMDD->data;
	 if (length == freeMDD->length) {
	    if (prev == NULL) {
	       freeReadOnlyData = freeMDD->next;
	    } else {
	       prev->next = freeMDD->next;
	    }
	    Mem_Free(freeMDD);
	 } else {
	    freeMDD->data += length;
	    freeMDD->length -= length;
	 }

	 return retVal;
      }
   }

   return NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * MemRO_Alloc --
 *
 *      Allocate a chunk of data from the read-only data pool.
 *
 * Results: 
 *	Pointer to allocated read-only data.
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
void *
MemRO_Alloc(uint32 length)
{
   SP_IRQL prevIRQL;
   void *ptr;
  
   if (length == 0) {
      return NULL;
   }

   prevIRQL = SP_LockIRQ(&memROLock, SP_IRQL_KERNEL);
   ptr = MemROAllocLocked(length);
   if (ptr == NULL) {
      MemRODump("MemRO_Alloc");
   }
   SP_UnlockIRQ(&memROLock, prevIRQL);

   return ptr;
}

/*
 *----------------------------------------------------------------------
 *
 * MemRO_Free --
 *
 *      Free a chunk of data from the read-only data pool.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	The freeReadOnlyData and inuseReadOnlyData lists are modified.
 *
 *----------------------------------------------------------------------
 */
void
MemRO_Free(void *ptr)
{
   MemRODesc *inuseMDD, *freeMDD, *prev;
   SP_IRQL prevIRQL;
   
   prevIRQL = SP_LockIRQ(&memROLock, SP_IRQL_KERNEL);

   for (prev = NULL, inuseMDD = inuseReadOnlyData; 
        inuseMDD != NULL && inuseMDD->data != (VA)ptr; 
	prev = inuseMDD, inuseMDD = inuseMDD->next) {
   }

   ASSERT_NOT_IMPLEMENTED(inuseMDD != NULL);

   if (prev == NULL) {
      inuseReadOnlyData = inuseMDD->next;
   } else {
      prev->next = inuseMDD->next;
   }

   for (prev = NULL, freeMDD = freeReadOnlyData;
        freeMDD != NULL && freeMDD->data < inuseMDD->data + inuseMDD->length;
	prev = freeMDD, freeMDD = freeMDD->next) {
   }

   if (freeMDD == NULL || freeMDD->data > inuseMDD->data + inuseMDD->length) {
      if (prev == NULL) {
	 freeReadOnlyData = inuseMDD;
      } else {
	 prev->next = inuseMDD;
      }
      inuseMDD->next = freeMDD;
   } else {
      freeMDD->data = inuseMDD->data;
      freeMDD->length += inuseMDD->length;
      Mem_Free(inuseMDD);
   }
   SP_UnlockIRQ(&memROLock, prevIRQL);
}

/*
 *----------------------------------------------------------------------
 *
 * MemRO_CalcChecksum --
 *
 *      Calculate 64-bit checksum for the entire vmkernel code region.
 *
 * Results:
 *      Returns 64-bit checksum for the entire vmkernel code region.
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
uint64
MemRO_CalcChecksum(void)
{
   const uint32 vmkTextStart = VMK_CODE_START;

   /*
    * VMK_CODE_LENGTH should divide evenly so we can use Hash_Quads.
    */
   ASSERT((VMK_CODE_LENGTH % sizeof(uint64)) == 0);
   const uint32 vmkTextSizeQuads = VMK_CODE_LENGTH / sizeof(uint64);

   return(Hash_Quads((uint64*)vmkTextStart, vmkTextSizeQuads));
}

/*
 *----------------------------------------------------------------------
 *
 * MemRO_GetChecksum --
 *
 *      Returns the current expected checksum for the vmkernel code region.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
uint64
MemRO_GetChecksum(void)
{
   return memROChecksum;
}

