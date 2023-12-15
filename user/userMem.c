/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * userMem.c --
 *
 *	UserWorld memory (heap, stack, mmap, etc) support functions.
 */

#include "user_int.h"
#include "world.h"
#include "kvmap.h"
#include "pagetable.h"
#include "memmap.h"
#include "userMem.h"
#include "util.h"
#include "vmmem.h"
#include "memalloc.h"
#include "pshare.h"
#include "dump_ext.h"
#include "linuxThread.h"
#include "user_layout.h"
#include "userStat.h"
#include "userPTE.h"

#define LOGLEVEL_MODULE     UserMem
#include "userLog.h"

#define USERMEM_MAPTYPES \
   DEFINE_USERMEM_MAPTYPE(UNUSED) \
   DEFINE_USERMEM_MAPTYPE(ANON) \
   DEFINE_USERMEM_MAPTYPE(FD) \
   DEFINE_USERMEM_MAPTYPE(PHYSMEM) \
   DEFINE_USERMEM_MAPTYPE(KTEXT) \
   DEFINE_USERMEM_MAPTYPE(TDATA) \
   DEFINE_USERMEM_MAPTYPE(MEMTEST) \
   DEFINE_USERMEM_MAPTYPE(END)

// define the enum
#define DEFINE_USERMEM_MAPTYPE(_name) USERMEM_MAPTYPE_ ## _name,
typedef enum {
   USERMEM_MAPTYPES
} UserMemMapType;
#undef DEFINE_USERMEM_MAPTYPE

// string equivalents for the enums
#define DEFINE_USERMEM_MAPTYPE(_name) #_name,
const char *userMemMapTypes[] = {
   USERMEM_MAPTYPES
};
#undef DEFINE_USERMEM_MAPTYPE

// information about an mmap region (see UserMemMapCreate)
typedef struct UserMemMapInfo {
   List_Links links; // must be first entry
   UserVA startAddr;
   uint32 length;
   UserMemMapType type;
   UserObj *obj;
   uint32 refCount;
   uint32 prot;
   Bool   pinned;
   uint32 reservedPages;
   uint64 pgoff;
} UserMemMapInfo;

// stats for user mem
typedef struct UserMemStats {
   Atomic_uint32 pageCount;     // machines pages in use 
   Atomic_uint32 pageShared;    // pages shared 
   Atomic_uint32 pageSwapped;   // pages swapped 
   Atomic_uint32 pagePinned;    // pages pinned

   Proc_Entry procDir;          // procfs node "/proc/vmware/usermem"
   Proc_Entry procStatus;       // procfs node "/proc/vmware/usermem/status"
} UserMemStats;

/*
 * Hash same lpn of different worlds to different uint32.
 */
#define USERMEM_HASH_LPN(cartelID, lpn) ((uint32)(cartelID) | ((uint32)(lpn) << PAGE_SHIFT))

/*
 *----------------------------------------------------------------------
 *
 * USERMEM_FOR_RANGE_DO,USERMEM_FOR_RANGE_END --
 *
 *	Convenience macro for iterating over pagetable entries.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	may hold kseg/kvmap mappings in the middle
 *
 *----------------------------------------------------------------------
 */
#define USERMEM_FOR_RANGE_DO(mem, startVPN, nPages, pte, i)                \
do {                                                                       \
   VMK_PTE *pageTable = NULL;                                              \
   for (i = 0; i < nPages; i++) {                                          \
      VPN vpn = (startVPN)+i;                                              \
      LA laddr = LPN_2_LA(VMK_USER_VPN_2_LPN(vpn));                        \
      MPN pageTableMPN;                                                    \
      if ((pageTable == NULL) || ((vpn % VMK_PTES_PER_PDE) == 0)) {        \
         if (pageTable) {                                                  \
            UserMemReleasePageTable(mem, pageTable);                       \
         }                                                                 \
         pageTable = UserMemCanonicalPageTable(mem, laddr, &pageTableMPN); \
      }                                                                    \
      if (pageTable == NULL) {                                             \
         pte = NULL;                                                       \
      } else {                                                             \
         pte = UserPTE_For(pageTable, laddr);                            \
      }

#define USERMEM_FOR_RANGE_END                   \
   }                                            \
   if (pageTable != NULL) {                     \
      UserMemReleasePageTable(mem, pageTable);  \
   }                                            \
} while (0)


#define USERMEM_FOR_RANGE_BREAK break

#define PTELIST_PTES_PER_NODE 500

typedef struct UserMemPTEListNode {
   int nPages;
   int totalPages;
   UserPTE pteArray[PTELIST_PTES_PER_NODE];
   struct UserMemPTEListNode *next;
} UserMemPTEListNode;

typedef UserMemPTEListNode (*UserMemPTEList);

static UserMemStats userMemStats;

static VMK_PTE* UserMemCanonicalPageTable(UserMem* userMem, const LA laddr,
                                          MPN* outPageTableMPN);
static void UserMemFreeCanonicalPageTable(UserMem* userMem);

static INLINE void UserMemFreePage(World_Handle *world, MPN mpn);

static VMK_ReturnStatus UserMemLookupPageTable(UserMem* userMem,
					       const MA pageRootMA,
					       const LA laddr,
					       VMK_PTE** outPageTable);
static VMK_ReturnStatus UserMemMapCreate(World_Handle *world, UserVA* addr,
                                         Bool overwrite, uint32 length,
                                         uint32 prot, UserMemMapType type, 
                                         Bool pinned, uint32 reservedPages,
                                         UserObj* obj, uint64 pgoff,
                                         UserMemMapExecFlag execFlag,
                                         UserMemMapInfo **outMMInfo);
static VMK_ReturnStatus UserMemMapTryExtending(World_Handle *world,
                                               UserMemMapInfo* mmInfo,
                                               UserVA* addr, uint32 length,
                                               uint32 prot, Bool pinned,
                                               Bool strictAlign);
static VMK_ReturnStatus UserMemMapDestroyMMInfo(World_Handle *world, 
                                                UserMemMapInfo* mmInfo,
                                                UserVA addr, uint32 length,
			                        Bool* freeMe,
                                                UserMemPTEList *pteListPtr);
/*
 *----------------------------------------------------------------------
 *
 * UserMemLock --
 *
 *	Lock the UserMem structure for read or update
 *
 * Results:
 *	None
 *
 * Side effects:
 *	UserMem.lock is locked.
 *
 *----------------------------------------------------------------------
 */
static inline void
UserMemLock(UserMem* mem)
{
   ASSERT(mem);
   SP_Lock(&mem->lock);
}


/*
 *----------------------------------------------------------------------
 *
 * UserMemLock --
 *
 *	Unlock the UserMem structure.  Best to have locked it
 *	beforehand.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	UserMem.lock is unlocked.
 *
 *----------------------------------------------------------------------
 */
static inline void
UserMemUnlock(UserMem* mem)
{
   ASSERT(mem);
   SP_Unlock(&mem->lock);
}


/*
 *----------------------------------------------------------------------
 *
 * UserMemIsLocked --
 *
 *	Test UserMem to see if its locked.
 *
 * Results:
 *	TRUE if locked, false if not
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static inline Bool
UserMemIsLocked(const UserMem* mem)
{
   return SP_IsLocked(&mem->lock);
}

/*
 *----------------------------------------------------------------------
 *
 * UserMemReleasePageTable --
 *
 *      Free up the page table access pointer previously acquired
 *      through UserMemLookupPageTable().
 *
 * Results:
 *      KSeg pointer to the page table released.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
UserMemReleasePageTable(UserMem *mem, VMK_PTE *pageTable)
{
   ASSERT(UserMemIsLocked(mem));
   PT_ReleasePageTable(pageTable, NULL);
   ASSERT(mem->ptRefCount > 0);
   mem->ptRefCount--;
}

/*
 *----------------------------------------------------------------------
 * UserMemUsage --
 *
 *   Wrapper function for accessing memsched usage.
 *
 *   The returned MemSchedUserUsage is protected by UserMem lock.
 *   Readers of this structure do not use a lock, so it's possible
 *   to read inconsistent data. Currently the only reader is memsched,
 *   which doesn't require accurate data.
 *
 *----------------------------------------------------------------------
 */
static INLINE MemSchedUserUsage *
UserMemUsage(const World_Handle *world)
{
   return MemSched_ClientUserUsage(world);
}

/*
 *----------------------------------------------------------------------
 *
 * UserMemVA2PTE --
 *
 *	Converts the given VA into the PTE which maps that VA to
 *	an mpn (if mapped) / mmInfo structure (if not mapped) 
 *
 * Results:
 *	VMK_ReturnStatus
 *
 * Side effects:
 *      pte contains the pte corresponding to the VA.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserMemVA2PTE(UserMem* mem, VA va, VMK_PTE** pageTablePtr, UserPTE** pte)
{
   VMK_ReturnStatus status;
   LA la = VMK_USER_VA_2_LA(va);
   
   ASSERT(UserMemIsLocked(mem));
   status = UserMemLookupPageTable(mem, MY_RUNNING_WORLD->pageRootMA,
                                   la, pageTablePtr);
   if (status == VMK_OK) {
      ASSERT(*pageTablePtr != NULL);
      *pte = UserPTE_For(*pageTablePtr, la);
   }

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * UserMemVA2Mminfo --
 *
 *	Converts the given VA into the mmInfo that covers the region
 *	it's in (and ASSERT fails if its pte is mapped).
 *
 * Results:
 *	VMK_ReturnStatus
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE VMK_ReturnStatus
UserMemVA2MmInfo(World_Handle *world, VA va, UserMemMapInfo** mmInfo)
{
   VMK_ReturnStatus status;
   UserPTE *pte = NULL;
   VMK_PTE *pageTable;
   UserMem *mem = &world->userCartelInfo->mem;

   ASSERT(mmInfo != NULL);

   UserMemLock(mem);
   // lookup PTE, this also fills in this world's pagetable from canonical
   status = UserMemVA2PTE(mem, va, &pageTable, &pte);
   if (status != VMK_OK) {
      DEBUG_ONLY(ASSERT(FALSE));
      UserMemUnlock(mem);
      return status;
   }

   ASSERT(pte != NULL);
   ASSERT(pageTable != NULL);
      
   if (UserPTE_IsMapped(pte)) {
      UWLOG(0, "PTE is mapped, cannot get mmInfo.");
      status = VMK_BAD_PARAM;
      goto done;
   }

   if (UserPTE_IsInUse(pte)) {
      *mmInfo = (UserMemMapInfo *)UserPTE_GetPtr(pte);
   } else {
      UWLOG(0, "PTE somehow cleared while initializing address space.");
      DEBUG_ONLY(ASSERT(FALSE));
      *mmInfo = NULL;
      status = VMK_NO_ADDRESS_SPACE;
      goto done;
   }

done:
   UserMemReleasePageTable(mem, pageTable);
   UserMemUnlock(mem);

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * UserMemMapLenghInPages --
 *
 *      Get number of pages covering [startAddr, startAddr + length).	
 *
 * Results:
 *      Number of pages covering [startAddr, startAddr + length).	
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE int32
UserMemMapLenghInPages(UserVA startAddr, uint32 length)
{
   if (length == 0) {
      return 0;
   } else {
      return VA_2_VPN(startAddr + length - 1) - VA_2_VPN(startAddr) + 1;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * UserMemMapInfoSetRange --
 *
 *      Sets mmap region to address range [startAddr, startAddr + length)
 *      and perform admission check.
 *
 * Results:
 *	VMK_NO_RESOURCES if adding memory, and reservation is exhausted.
 *	Cannot fail if reducing the memory footprint.
 *	
 *      If VMK_OK is returned, mmInfo updated with the new values
 *      and usage changed.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserMemMapInfoSetRange(const World_Handle *world, UserMemMapInfo *mmInfo,
                       UserVA startAddr, uint32 length)
{
   MemSchedUserUsage *usage = UserMemUsage(world);
   int32 oldLength = UserMemMapLenghInPages(mmInfo->startAddr, mmInfo->length);
   int32 newLength = UserMemMapLenghInPages(startAddr, length);
   int32 delta = newLength - oldLength;

   // admission check and record usage.
   switch (mmInfo->type) {
   case USERMEM_MAPTYPE_ANON:
      if (delta > 0 && !MemSched_AdmitUserMapped(world, delta))  {
         UWLOG(0, "User mapped pages %d+%d exceeded limit, mmap region start 0x%x, len %d.",
               usage->virtualPageCount[MEMSCHED_MEMTYPE_MAPPED], delta, 
               startAddr, length);
         return VMK_NO_RESOURCES;
      }
      usage->virtualPageCount[MEMSCHED_MEMTYPE_MAPPED] += delta;
      break;
   case USERMEM_MAPTYPE_KTEXT:
   case USERMEM_MAPTYPE_TDATA:
      usage->virtualPageCount[MEMSCHED_MEMTYPE_KERNEL] += delta;
      break;
   case USERMEM_MAPTYPE_FD:
      usage->virtualPageCount[MEMSCHED_MEMTYPE_SHARED] += delta;
      break;
   case USERMEM_MAPTYPE_PHYSMEM:
   case USERMEM_MAPTYPE_MEMTEST:
      usage->virtualPageCount[MEMSCHED_MEMTYPE_UNCOUNTED] += delta;
      break;
   default:
      ASSERT(FALSE);
      return VMK_BAD_PARAM;
   }

   mmInfo->startAddr = startAddr;
   mmInfo->length = length;

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * UserMemMapInfoSetLength --
 *
 *      Sets new "length" of mmap region without changing
 *      the start address.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE VMK_ReturnStatus
UserMemMapInfoSetLength(const World_Handle *world, UserMemMapInfo *mmInfo, 
                        uint32 length)
{
   return UserMemMapInfoSetRange(world, mmInfo, mmInfo->startAddr,
                                 length);
}

/*
 *----------------------------------------------------------------------
 *
 * UserMemMapInfoSetStart --
 *
 *      Sets new "startAddr" of mmap region without changing
 *      the end address.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE VMK_ReturnStatus
UserMemMapInfoSetStart(const World_Handle *world, UserMemMapInfo *mmInfo, 
                       UserVA startAddr)
{
   ASSERT(mmInfo->startAddr + mmInfo->length >= startAddr);
   return UserMemMapInfoSetRange(world, mmInfo, startAddr,
                                 mmInfo->startAddr + mmInfo->length - startAddr);
}

/*
 *----------------------------------------------------------------------
 *
 * UserMemMapInfoSetEnd --
 *
 *      Sets new "endAddr" of mmap region without changing
 *      the start address.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE VMK_ReturnStatus
UserMemMapInfoSetEnd(const World_Handle *world, UserMemMapInfo *mmInfo, 
                     UserVA endAddr)
{
   ASSERT(endAddr >= mmInfo->startAddr);
   return UserMemMapInfoSetRange(world, mmInfo, mmInfo->startAddr,
                                 endAddr - mmInfo->startAddr);
}


/*
 *----------------------------------------------------------------------
 *
 * UserMemPTEListAdd --
 *
 *	Add the given pte, along with page sharing info to the list of
 *      PTEs. These lists are used for delayed flushing and freeing of
 *      pages backing regions of pagetables. Since remote flush is 
 *      expensive (only want to do it once) and it may block, we 
 *      invalidate the pagetable entries and record the PTEs in these 
 *      lists to be freed later.
 *
 * Results:
 *	VMK_OK if successful, NO_MEMORY otherwise
 *
 * Side effects:
 *	may allocate memory
 *
 *----------------------------------------------------------------------
 */
static INLINE VMK_ReturnStatus
UserMemPTEListAdd(User_CartelInfo* uci, UserMemPTEList *listPtr,
                  UserPTE *pte)
{
   UserMemPTEList list = *listPtr;

   UWLOG(4, "pte %"FMT64"x", pte->u.raw);
   // allocate a new list node if necessary
   if ((list == NULL) || (list->nPages == PTELIST_PTES_PER_NODE)) {
      UserMemPTEList newList = (UserMemPTEList)User_HeapAlloc(uci, sizeof *list);
      if (newList == NULL) {
         return VMK_NO_MEMORY_RETRY;
      }
      newList->nPages = 0;
      if (list) {
         newList->totalPages = list->totalPages;
      } else {
         newList->totalPages = 0;
      }
      newList->next = list;
      *listPtr = newList;
      list = newList;
      UWLOG(2, "new list %p", list);
   }

   // insert the new mpn in list node
   ASSERT(list->nPages < PTELIST_PTES_PER_NODE);
   list->pteArray[list->nPages] = *pte;
   list->nPages++;
   list->totalPages++;

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * UserMemPTEListRemove --
 *
 *	Remove an PTE from the PTE list.
 *
 * Results:
 *      Return the remaining PTE list. 
 *      If there is no PTE to be removed, return NULL.
 *
 * Side effects:
 *	may free memory
 *
 *----------------------------------------------------------------------
 */
static INLINE UserMemPTEList 
UserMemPTEListRemove(User_CartelInfo *uci,      // IN
                     UserMemPTEList  list,      // IN
                     UserPTE *pte)           // OUT 
{
   ASSERT(list != NULL);

   // If list is empty, free it.
   if (list->nPages == 0) {
      UserMemPTEList nextList = list->next;
      User_HeapFree(uci, list);
      UWLOG(2, "free list %p", list);

      // nothing left, return NULL
      if (nextList == NULL) {
         UserPTE_Clear(pte);
         return NULL;
      }
      // cdr list 
      list = nextList;
   }

   ASSERT(list->nPages <= PTELIST_PTES_PER_NODE);
   ASSERT(list->nPages > 0);

   // remove PTE from list node
   *pte = list->pteArray[list->nPages-1];
   list->nPages--;
   list->totalPages--;

   UWLOG(4, "pte %"FMT64"x", pte->u.raw);

   return list;
}

/*
 *----------------------------------------------------------------------
 *
 * UserMemNextFreeSwapReq --
 *
 *	Find the next free req in the swap list.
 *
 * Results:
 *      On success, return the next free req.
 *	On failure, return USERMEM_INVALID_SWAP_REQ.
 *
 *----------------------------------------------------------------------
 */
static int
UserMemNextFreeSwapReq(UserMemSwapList *swapList, uint32 curReqNum)
{
   int i;

   for (i = curReqNum; i < USERMEM_NUM_SWAP_REQS; i++) {
      if (swapList->reqs[i].state == USERMEM_SWAP_NONE) {
         return i;
      }
   }
   return USERMEM_INVALID_SWAP_REQ;
}

/*
 *----------------------------------------------------------------------
 *
 * UserMemNextFreeSwapReq --
 *
 *	Find the first free req in the swap list.
 *
 * Results:
 *      On success, return the next free req.
 *	On failure, return USERMEM_INVALID_SWAP_REQ.
 *
 *----------------------------------------------------------------------
 */
static INLINE int
UserMemFirstFreeSwapReq(UserMemSwapList *swapList)
{
   return UserMemNextFreeSwapReq(swapList, 0);
}

/*
 *----------------------------------------------------------------------
 *
 * UserMemFreeSwapReq --
 *
 *	Free a swap req.
 *
 * Results:
 *      Swap req state changes to USERMEM_SWAP_NONE.
 *
 * Side effects:
 *      Wake up threads waiting for free reqs.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
UserMemFreeSwapReq(UserMemSwapList *swapList, int reqNum)
{
   ASSERT(reqNum >= 0 && reqNum < USERMEM_NUM_SWAP_REQS);
   ASSERT(swapList->reqs[reqNum].state != USERMEM_SWAP_NONE);
   swapList->reqs[reqNum].state = USERMEM_SWAP_NONE;
   swapList->numFreeReqs++;
   if (swapList->numFreeReqs == 1) {
      CpuSched_Wakeup((uint32)(uintptr_t)swapList);
   }
   ASSERT(swapList->numFreeReqs <= USERMEM_NUM_SWAP_REQS);
}

/*
 *----------------------------------------------------------------------
 *
 * UserMemCancelSwapOut --
 *
 *	Cancel all pending swap-out requests on a given lpn.
 *
 * Results:
 *      Swap reqs freed or states change to USERMEM_SWAP_CANCELED.
 *
 *----------------------------------------------------------------------
 */
static void
UserMemCancelSwapOut(UserMemSwapList *swapList, LPN lpn)
{
   int i;
   for (i = 0; i < USERMEM_NUM_SWAP_REQS; i++) {
      UserMemSwapReq *req = &swapList->reqs[i];
      if (req->lpn == lpn) {
         if (req->state == USERMEM_SWAP_OUT_REQ) {
            UserMemFreeSwapReq(swapList, i);
         } else if (req->state == USERMEM_SWAPPING_OUT) {
            req->state = USERMEM_SWAP_CANCELED;
         }
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * UserMemCancelSwapOut --
 *
 *	Cancel all pending swap-in and swap-out requests on a given lpn.
 *
 * Results:
 *      Swap reqs freed or states change to USERMEM_SWAP_CANCELED.
 *
 *----------------------------------------------------------------------
 */
static void
UserMemCancelSwapping(UserMemSwapList *swapList, LPN lpn)
{
   int i;
   for (i = 0; i < USERMEM_NUM_SWAP_REQS; i++) {
      UserMemSwapReq *req = &swapList->reqs[i];
      if (req->lpn == lpn) {
         if (req->state == USERMEM_SWAP_OUT_REQ) {
            UserMemFreeSwapReq(swapList, i);
         } else if (req->state == USERMEM_SWAPPING_OUT ||
                  req->state == USERMEM_SWAPPING_IN) {
            req->state = USERMEM_SWAP_CANCELED;
         }
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * UserMemInitSwapReq --
 *
 *	Initialize a swap req for a swap request.
 *
 * Results:
 *      Swap req state changes to the requested state.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
UserMemInitSwapReq(UserMemSwapList *swapList, int reqNum,
                    UserMemSwapState state, LPN lpn, UserPTE *pte)
{
   ASSERT(reqNum >= 0 && reqNum < USERMEM_NUM_SWAP_REQS);
   ASSERT(swapList->reqs[reqNum].state == USERMEM_SWAP_NONE);
   ASSERT(state == USERMEM_SWAP_OUT_REQ || state == USERMEM_SWAPPING_IN);

   swapList->reqs[reqNum].state = state;
   swapList->reqs[reqNum].lpn = lpn;
   swapList->reqs[reqNum].pte = *pte;

   swapList->numFreeReqs--;
   ASSERT(swapList->numFreeReqs >= 0);
}

/*
 *----------------------------------------------------------------------
 *
 * UserMemMapRangeCheckEmpty
 *
 *	Check if the PTEs are in use for a range of VPNs.
 *
 * Results:
 *      VMK_OK if emtpy, VMK_EXISTS if not, or VMK_NO_MEMORY
 *
 * Side effects:
 *	none
 *
 *----------------------------------------------------------------------
 */
static INLINE VMK_ReturnStatus 
UserMemMapRangeCheckEmpty(User_CartelInfo* uci,
                          VPN startVPN,
                          uint32 nPages)
{
   UserMem *mem = &uci->mem;
   UserPTE *pte;
   int i;
   VMK_ReturnStatus status = VMK_OK;
 
   ASSERT(UserMemIsLocked(mem));
   USERMEM_FOR_RANGE_DO(mem, startVPN, nPages, pte, i) {
      if (pte == NULL) {
         status = VMK_NO_MEMORY;
         USERMEM_FOR_RANGE_BREAK;
      }
      if (UserPTE_IsMapped(pte) || UserPTE_IsInUse(pte)) {
         status = VMK_EXISTS;
         USERMEM_FOR_RANGE_BREAK;
      }
   } USERMEM_FOR_RANGE_END;

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * UserMemSetPTEInUseRange --
 *
 *	Mark a range of PTEs in use.
 *
 *      NOTE: given prot is ignored if mmInfoOnly is TRUE
 *
 * Results:
 *      VMK_NO_MEMORY if page table mapping fails because of no kernel
 *      memory.
 *
 * Side effects:
 *	none
 *
 *----------------------------------------------------------------------
 */
static INLINE VMK_ReturnStatus
UserMemSetPTEInUseRange(User_CartelInfo* uci,
                        VPN startVPN,
                        uint32 nPages,
			uint32 prot, 
                        UserMemMapInfo* mmInfo,
			Bool mmInfoOnly)
{
   UserMem *mem = &uci->mem;
   UserPTE *pte;
   int i;
   VMK_ReturnStatus status = VMK_OK;
 
   ASSERT(UserMemIsLocked(mem));
   USERMEM_FOR_RANGE_DO(mem, startVPN, nPages, pte, i) {
      if (pte == NULL) {
         status = VMK_NO_MEMORY;
         USERMEM_FOR_RANGE_BREAK;
      }
      if (mmInfoOnly) {
         /*
          * When creating a new mmInfo to cover a region previously
          * covered by a different mmInfo (see the split code in
          * UserMemMapInfoSplit), we want to leave mapped PTEs unchanged,
          * and just want to update the mmInfo portion of the unmapped
          * PTEs (keep the prot bits and other aspects of the PTE
          * unchanged).
          */
         if (!UserPTE_IsMapped(pte)) {
	    prot = UserPTE_GetProt(pte);
	    UserPTE_SetInUse(pte, prot, mmInfo);
	 }
         // else, if the pte is already mapped, just leave it be.
      } else {
         UserPTE_SetInUse(pte, prot, mmInfo);
      }
   } USERMEM_FOR_RANGE_END;

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * UserMemMapClearRange --
 *
 *	Clear a range of PTEs.
 *
 * Results:
 *      VMK_ReturnStatus, and list of PTEs are added into pteListPtr for removal.
 *
 * Side effects:
 *	none
 *
 *----------------------------------------------------------------------
 */
static INLINE VMK_ReturnStatus
UserMemMapClearRange(User_CartelInfo* uci,        // IN
                     Bool isRegionPinned,         // IN
                     VPN startVPN,                // IN
                     uint32 nPages,               // IN
                     UserMemPTEList *pteListPtr)  // INOUT
{
   UserMem *mem = &uci->mem;
   UserPTE *pte;
   int i;
   VMK_ReturnStatus status = VMK_OK;

   ASSERT(UserMemIsLocked(mem));
   USERMEM_FOR_RANGE_DO(mem, startVPN, nPages, pte, i) {
      if (pte == NULL) {
         UWLOG(0, "NULL pte when clearing %#x (%d pages)", VPN_2_VA(startVPN), nPages);
         status = VMK_NO_MEMORY_RETRY;
         USERMEM_FOR_RANGE_BREAK;
      }
      if (UserPTE_IsMapped(pte)) {
         if (!isRegionPinned && UserPTE_IsPinned(pte)) {
            mem->curReserved--;
         }
         if (UserPTE_IsSwapping(pte)) {
            UserMemCancelSwapping(&mem->swapList, LA_2_LPN(laddr));
            UWLOG(1, "ClearRange: cancel swapping lpn %x", LA_2_LPN(laddr));
            // For swapable,  make sure that we always pass in pteListPtr.
            ASSERT(pteListPtr);
         }
         if (pteListPtr) {
            // add pten to list for delayed flush/free
            status = UserMemPTEListAdd(uci, pteListPtr, pte);
            if (status != VMK_OK) {
               ASSERT(status == VMK_NO_MEMORY_RETRY);
               USERMEM_FOR_RANGE_BREAK;
            }
         }
         UserPTE_Clear(pte);
      }
      if (UserPTE_IsInUse(pte)) {
         UserPTE_Clear(pte);
      }
   } USERMEM_FOR_RANGE_END;

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserMemAllocPage --
 *
 *	Allocate a machine page.  If no page is available, returns
 *	VMK_NO_MEMORY and sets *mpn to INVALID_MPN.  If possible, you 
 *
 * Results:
 *	VMK_ReturnStatus and MPN
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE VMK_ReturnStatus
UserMemAllocPage(World_Handle *world, MPN *mpn)
{
   /* XXX add stress option to occasionally claim we're out of memory. */

   *mpn = MemMap_AllocUserWorldPage(world,
                                    MM_NODE_ANY, MM_COLOR_ANY, MM_TYPE_ANY);
   if (*mpn == INVALID_MPN) {
      return VMK_NO_MEMORY;
   }

   Atomic_Inc(&userMemStats.pageCount);
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * UserMemFreePage --
 *
 *	Free a page allocated by UserMemAllocPage.  Maybe good to pass in
 *	the world pointer if we want memmap to track node breakdown for
 *	pages used by this world.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
UserMemFreePage(World_Handle *world, MPN mpn)
{
   Atomic_Dec(&userMemStats.pageCount);
   MemMap_FreeUserWorldPage(mpn);
}

/*
 *----------------------------------------------------------------------
 *
 * UserMemFreePSharedPage --
 *
 *	Free a pshared page.
 *
 * Results:
 *	TRUE if a real MPN has been freed, 
 *      FALSE if the page count is decrimented.
 *
 * Side effects:
 *	Panic.
 *
 *----------------------------------------------------------------------
 */
static Bool
UserMemFreePSharedPage(World_Handle *world, MPN mpn)
{
   uint64 key;
   uint32 count;
   VMK_ReturnStatus status;

   status = PShare_LookupByMPN(mpn, &key, &count);
   if (status != VMK_OK || count <= 0) {
      Panic("UserMemFreePSharedPage: try to free an invalid page %#x\n", mpn);
   }
   status = PShare_Remove(key, mpn, &count);
   if (LIKELY(status == VMK_OK)) {
      if (count == 0) {
         UserMemFreePage(world, mpn);
         return TRUE;
      } else {
         Atomic_Dec(&userMemStats.pageShared);
         return FALSE;
      }
   } else {
      ASSERT(FALSE);
      return FALSE;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * UserMemCartelFlush --
 *
 *	Flush the tlb on all CPUs that are currently running a world that
 *	belongs to the given cartel.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
UserMemCartelFlush(User_CartelInfo* uci)
{
   UWSTAT_INC(userMemCartelFlushes);
   // XXX currently flushes out tlb on all CPUS.  Should fix this to only
   // do the CPUs running this cartel's worlds instead of all CPUs
   TLB_Flush(0);
}

/*
 *----------------------------------------------------------------------
 *
 * UserMemFlushAndFreePages --
 *
 *	Flush all cartel CPUs and free the given list of PTEs 
 *
 * Results:
 *	Total PTE entries freed.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static uint32
UserMemFlushAndFreePages(World_Handle *world, UserMemPTEList list)
{
   User_CartelInfo *uci = world->userCartelInfo;
   UserPTE pte;
   uint32 totalDecCount = 0;
   uint32 pageableDecCount = 0, pshareDecCount = 0;
   uint32 swapDecCount = 0, pinnedDecCount = 0;

   if (list != NULL) {
      UserMemCartelFlush(uci);
   
      UWLOG(1, "total pages = %d", list->totalPages);
      while ((list = UserMemPTEListRemove(uci, list, &pte)) != NULL) {
         if (UserPTE_IsPresent(&pte)) {
            MPN mpn = UserPTE_GetMPN(&pte);
            if (UserPTE_IsPShared(&pte)) {
               UserMemFreePSharedPage(world, mpn);
               pshareDecCount++;
            } else {
               UserMemFreePage(world, mpn);
               if (UserPTE_IsPinned(&pte)) {
                  pinnedDecCount++;
               } else {
                  pageableDecCount++;
               }
            }
         } else if (UserPTE_IsSwapping(&pte)) {
            /*
             * We don't need to free swap file slots for the pages being swapped
             * because they will be freed in the swap-in and swap-out paths.
             */
            MPN mpn = UserPTE_GetMPN(&pte);
            if (mpn != INVALID_MPN) {
               /*
                * Free the page that is being swapped out.
                * It's safe to do so because the swap request will eventually
                * be canceled so the data in the swap file becomes invalid.
                */
               UserMemFreePage(world, mpn);
               pageableDecCount++;
            } else {
               swapDecCount++;
            }
         } else if (UserPTE_IsSwapped(&pte)) {
            uint32 swapFileSlot = UserPTE_GetSwapSlot(&pte);
            // free swap file slot
            Swap_UWFreeFileSlot(swapFileSlot);
            swapDecCount++;
         } else {
            // Only present/swapped/swapping entries are added to PTEList.
            ASSERT(FALSE);
         }
         totalDecCount++;
      }
      // discount all freed pages
      if (totalDecCount > 0) {
         UserMemLock(&uci->mem);
         UserMemUsage(world)->pageable -= pageableDecCount;
         UserMemUsage(world)->cow -= pshareDecCount;
         UserMemUsage(world)->swapped -= swapDecCount;
         UserMemUsage(world)->pinned -= pinnedDecCount;
         Atomic_Sub(&userMemStats.pageSwapped, swapDecCount);
         Atomic_Sub(&userMemStats.pagePinned, pinnedDecCount);
         UserMemUnlock(&uci->mem);
      }
   }
   return totalDecCount;
}

/*
 *----------------------------------------------------------------------
 *
 * UserMemCleanupAndFreeMMInfos --
 *
 *	Cleanup and free each mmInfo in the given list of mmInfos.
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
UserMemCleanupAndFreeMMInfos(World_Handle *world, List_Links* mmInfosToFree)
{
   User_CartelInfo *uci = world->userCartelInfo;

   while (!List_IsEmpty(mmInfosToFree)) {
      UserMemMapInfo *mmInfo = (UserMemMapInfo *)List_First(mmInfosToFree);
      VMK_ReturnStatus status;

      ASSERT(mmInfo->refCount == 0);
      List_Remove(&mmInfo->links);

      switch(mmInfo->type) {
         case USERMEM_MAPTYPE_ANON:
	 case USERMEM_MAPTYPE_KTEXT:
	 case USERMEM_MAPTYPE_TDATA:
	    // Nothing to do here.
	    break;
	 case USERMEM_MAPTYPE_FD:
	    ASSERT(mmInfo->obj != NULL);
	    status = UserObj_Release(uci, mmInfo->obj);
            if (status != VMK_OK) {
               UWWarn("UserObj release failed %s", 
                      UWLOG_ReturnStatusToString(status));
            }
	    break;
	 case USERMEM_MAPTYPE_PHYSMEM:
            UserMemCartelFlush(uci);
	    status = Alloc_PhysMemUnmap(World_GetVmmLeaderID(world), mmInfo->pgoff,
                                        mmInfo->length);
            /*
             * It's possible that the VMM world has been cleaned up when we reach 
             * here, in which case, the physMem has already been freed.
             */
            ASSERT(status == VMK_OK || status == VMK_BAD_PARAM);
            if (status != VMK_OK) {
               UWWarn("free physMem error %s, off 0x%#Lx, length %d", 
                      UWLOG_ReturnStatusToString(status), mmInfo->pgoff, mmInfo->length);
            }
	    break;
         case USERMEM_MAPTYPE_MEMTEST:
            UserMemCartelFlush(uci);
            MemMap_FreePageRange(mmInfo->pgoff, mmInfo->length / PAGE_SIZE);
            UserMemLock(&uci->mem);
            UserMemUsage(world)->pinned -= mmInfo->length / PAGE_SIZE;
            UserMemUnlock(&uci->mem);
            break;
	 default:
	    Panic("Invalid mmInfo struct type: %d", mmInfo->type);
      }

      UserMemLock(&uci->mem);
      if (mmInfo->reservedPages > 0) {
         uci->mem.curReserved -= mmInfo->reservedPages;
      }
      // reduce reserved memory
      status = UserMemMapInfoSetRange(world, mmInfo, 0, 0); 
      ASSERT(status == VMK_OK);
      UserMemUnlock(&uci->mem);

      User_HeapFree(uci, mmInfo);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * UserMem_CartelInit --
 *
 *	Setup cartel-wide memory tracking state.
 *      XXX The way we deal with partial failures could use some cleanup.
 *
 * Results:
 *	VMK_OK
 *
 * Side effects:
 *	Lock is allocated, canonical root is allocated
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserMem_CartelInit(User_CartelInfo* uci, World_Handle* world)
{
   UserMem *mem = &uci->mem;
   int i;
   VMK_ReturnStatus status = VMK_OK;
   VMK_PDPTE *canonRoot;
   ASSERT(mem != NULL);
   UserVA addr;
   VMK_PTE* pageTable;
   LA laddr;

   SP_InitLock("UserMem", &mem->lock, UW_SP_RANK_USERMEM);
   List_Init(&mem->mmaps);

   /*
    * Allocate Page Directory page and the 4 Page Roots.  Leave it
    * empty (it will eventually hold the canonical mappings for
    * user-mode addresses).
    */
   canonRoot = PT_AllocPageRoot(&mem->canonicalPageRootMA, INVALID_MPN);
   if (canonRoot == NULL) {
      status = VMK_NO_MEMORY;
      // Fall through and hit status check below
   } else {
      PT_ReleasePageRoot(canonRoot);

      mem->dataStart = VMK_USER_FIRST_TEXT_VADDR;
      mem->dataEnd = VMK_USER_FIRST_TEXT_VADDR;
   }
   mem->ptRefCount = 0;

   /*
    * Allocate the ktext page.
    */
   ASSERT(VMK_USER_MAX_KTEXT_PAGES == 1);
   mem->ktextMPN = INVALID_MPN;
   ASSERT(mem->ktextOffset == 0);
   mem->ktextOffset = 0;
   if (status == VMK_OK) {
      status = UserMemAllocPage(world, &mem->ktextMPN);

      if (status == VMK_OK) {
         // Don't let users see random bits of kernel data.
         status = Util_ZeroMPN(mem->ktextMPN);
      }
   }

   // initialize swap list
   mem->swapList.numFreeReqs = USERMEM_NUM_SWAP_REQS;
   for (i = 0; i < USERMEM_NUM_SWAP_REQS; i++) {
      mem->swapList.reqs[i].state = USERMEM_SWAP_NONE;
   }

   mem->swapScanLA = 0;

   /*
    * XXX hack: Later we should invoke mem scheduler function 
    * to update sched data and userMem.sched can be deleted.
    */
   mem->sched = &world->group->memsched.user;

   if (status != VMK_OK) {
      (void) UserMem_CartelCleanup(uci, world);
      return status;
   }

   /*
    * Create vaddr mapping for the per-thread data page.  (Each thread
    * will fault in a different MPN for this address.)
    */
   addr = VMK_USER_FIRST_KTEXT_VADDR;
   UserMemLock(mem);
   status = UserMemMapCreate(world, &addr, FALSE, PAGE_SIZE,
                             PTE_P, USERMEM_MAPTYPE_KTEXT, TRUE,
                             0, NULL, 0, MEMMAP_IGNORE, NULL);
   UserMemUnlock(mem);

   if (status != VMK_OK) {
      (void) UserMem_CartelCleanup(uci, world);
      return status;
   }

   /*
    * Set the ktext pte immediately so that it doesn't have to get
    * faulted in on first reference.
    */
   laddr = VMK_USER_VA_2_LA(VMK_USER_FIRST_KTEXT_VADDR);
   UserMemLock(mem);
   pageTable = UserMemCanonicalPageTable(mem, laddr, NULL);
   if (pageTable != NULL) {
      UserPTE_Set(UserPTE_For(pageTable, laddr),
                  mem->ktextMPN, PTE_P | PTE_US, TRUE, FALSE);
      UserMemReleasePageTable(mem, pageTable);
   } else {
      status = VMK_NO_MEMORY;
   }
   UserMemUnlock(mem);

   if (status != VMK_OK) {
      (void) UserMem_CartelCleanup(uci, world);
      return status;
   }

   /*
    * Map in the per-thread data page.
    */
   addr = VMK_USER_FIRST_TDATA_VADDR;
   UserMemLock(mem);
   status = UserMemMapCreate(world, &addr, FALSE, PAGE_SIZE,
                             PTE_P, USERMEM_MAPTYPE_TDATA, TRUE,
                             0, NULL, 0, MEMMAP_IGNORE, NULL);
   UserMemUnlock(mem);

   if (status != VMK_OK) {
      (void) UserMem_CartelCleanup(uci, world);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserMem_CartelCleanup --
 *
 *	Undo UserMem_CartelInit.  Cleanup cartel-wide memory tracking
 *	state.
 *
 * Results:
 *	VMK_OK
 *
 * Side effects:
 *	Lock is destroyed.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserMem_CartelCleanup(User_CartelInfo *uci, World_Handle *world)
{
   VMK_ReturnStatus status;
   UserMem *mem = &uci->mem;

   ASSERT(mem != NULL);

   // free mmaps
   status = UserMem_Unmap(world, (UserVA)0, VMK_USER_LAST_VADDR);
   if (status != VMK_OK) {
      UWWarn("Failed to cleanly unmap user address space: %s",
             VMK_ReturnStatusToString(status));
   }

   UserMemLock(mem);

   // free ktext MPN
   if (mem->ktextMPN != INVALID_MPN) {
      UserMemFreePage(world, mem->ktextMPN);
   }

   // free canonical page table 
   UserMemFreeCanonicalPageTable(mem);
   UserMemUnlock(mem);

   SP_CleanupLock(&mem->lock);

   if (vmx86_debug) {
      int i;
      /* If you hit this assert, you probably forgot to free some
       * PTEs somewhere
       */
      if (UserMemUsage(world)->pageable != 0 ||
          UserMemUsage(world)->cow != 0 ||
          UserMemUsage(world)->swapped != 0 ||
          UserMemUsage(world)->pinned != 0) {
         UWWarn("Failed to cleanup: pageable %d cow %d swapped %d pinned %d",
                UserMemUsage(world)->pageable, UserMemUsage(world)->cow,
                UserMemUsage(world)->swapped, UserMemUsage(world)->pinned);
         ASSERT(FALSE);
      }

      for (i = 0; i < MEMSCHED_NUM_MEMTYPES; i++) {
         if (UserMemUsage(world)->virtualPageCount[i] != 0) {
            UWWarn("Failed to cleanup: virtualPageCount[%d]=%d", 
                   i, UserMemUsage(world)->virtualPageCount[i]);
            ASSERT(FALSE);
         }
      }

      memset(mem, 0xad, sizeof(UserMem));
   }

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserMem_ThreadInit --
 *
 *	Set up per-thread memory state, namely one page for per-thread
 *	data (tdata) and one page table to hold its pte.  For
 *	simplicity, we do not place any other ptes in this page table.
 *	All other user-space pages and page tables are per-cartel.
 *
 * Results:
 *	VMK_OK or error
 *
 * Side effects:
 *	Thread data page and its page table are allocated.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserMem_ThreadInit(User_ThreadInfo *uti, World_Handle *world)
{
   VMK_PDE *pageDir;
   VMK_PTE *pageTable;
   KSEG_Pair *ksegDir;
   KSEG_Pair *ksegPT;
   const LA laddr = VMK_USER_VA_2_LA(VMK_USER_FIRST_TDATA_VADDR);
   VMK_ReturnStatus status;
   User_ThreadData *tdata;

   ASSERT(VMK_USER_MAX_TDATA_PAGES == 1);

   /*
    * In case of failure...
    */
   uti->mem.mpn = uti->mem.ptMPN = INVALID_MPN;

   /*
    * Allocate a private MPN to hold the thread data.
    */
   status = UserMemAllocPage(world, &uti->mem.mpn);
   if (status != VMK_OK) {
      UWWarn("Failed to allocate tdata page");
      goto failure;
   }

   /*
    * Allocate a private page table MPN to hold its pte.
    */
   pageDir = PT_GetPageDir(world->pageRootMA, laddr, &ksegDir);
   pageTable = PT_AllocPageTableInDir(pageDir, laddr, PTE_US, &ksegPT,
                                      &uti->mem.ptMPN);
   PT_ReleasePageDir(pageDir, ksegDir);
   if (pageTable == NULL) {
      UWWarn("Failed to allocate tdata page table");
      status = VMK_NO_MEMORY;
      goto failure;
   }

   /*
    * Set the tdata pte immediately so that it doesn't have to get
    * faulted it in on first reference.
    */
   UserPTE_Set(UserPTE_For(pageTable, laddr),
               uti->mem.mpn, PTE_P | PTE_US, TRUE, FALSE);

   /*
    * Unmap the page table.
    */
   PT_ReleasePageTable(pageTable, ksegPT);

   /*
    * Fill in page contents.
    */
   tdata = KVMap_MapMPN(uti->mem.mpn, TLB_LOCALONLY);
   if (tdata == NULL) {
      UWLOG(0, "KVMap_MapMPN failed");
      status = VMK_NO_ADDRESS_SPACE;
      goto failure;
   }
   Util_ZeroPage(tdata);
   tdata->magic = USER_THREADDATA_MAGIC;
   tdata->minorVersion = USER_THREADDATA_MINOR_VERSION;
   tdata->majorVersion = USER_THREADDATA_MAJOR_VERSION;
   tdata->tid = LinuxThread_PidForWorldID(world->worldID);
   tdata->pseudoTSCGet =
      (uint64 (*)(void)) world->userCartelInfo->time.pseudoTSCGet;
   KVMap_FreePages(tdata);

   return VMK_OK;

  failure:
   UserMem_ThreadCleanup(uti, world);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserMem_ThreadCleanup --
 *
 *	Clean up per-thread memory state.
 *
 * Results:
 *	VMK_OK
 *
 * Side effects:
 *	Thread data page and its page table are freed.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserMem_ThreadCleanup(User_ThreadInfo *uti, World_Handle* world)
{
   ASSERT(VMK_USER_MAX_TDATA_PAGES == 1);

   // free private page table page
   if (uti->mem.ptMPN != INVALID_MPN) {
      MemMap_FreeKernelPage(uti->mem.ptMPN);
      uti->mem.ptMPN = INVALID_MPN;
   }

   // free private thread data page
   if (uti->mem.mpn != INVALID_MPN) {
      UserMemFreePage(world, uti->mem.mpn);
      uti->mem.mpn = INVALID_MPN;
   }

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserMem_SetDataStart --
 *
 *	Sets the va for the start of the heap.
 *
 * Results:
 *	VMK_OK if successful, VMK_BAD_PARAM if the value was out of bounds.
 *	*start is rounded down to a page-aligned value if its not aligned.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserMem_SetDataStart(World_Handle *world, UserVA* start)
{
   UserMem *mem = &world->userCartelInfo->mem;
   /*
    * Do a small sanity check.
    */
   if (*start < VMK_USER_FIRST_TEXT_VADDR) {
      return VMK_BAD_PARAM;
   }
   
   if (*start > VMK_USER_FIRST_MMAP_TEXT_VADDR - PAGE_SIZE) {
      return VMK_LIMIT_EXCEEDED;
   }

   if ((*start & PAGE_MASK) != 0) {
      *start = (*start & ~PAGE_MASK) + PAGE_SIZE;
   }

   UserMemLock(mem);
   mem->dataStart = *start;
   UserMemUnlock(mem);

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserMem_GetDataStart --
 *
 *      Gets the va for the start of the heap.
 *
 * Results:
 *      mem->dataStart
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
UserVA
UserMem_GetDataStart(World_Handle *world)
{
   UserMem *mem = &world->userCartelInfo->mem;
   UserVA start;

   UserMemLock(mem);
   start = mem->dataStart;
   UserMemUnlock(mem);

   return start;
}


/*
 *----------------------------------------------------------------------
 *
 * UserMem_GetDataEnd --
 *
 *      Get the end of a user world's data segment.  This is the
 *	address of the first page after the valid data segment.
 *
 * Results:
 *	End of a user world's data segment.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
UserVA
UserMem_GetDataEnd(World_Handle *world)
{
   UserMem *mem = &world->userCartelInfo->mem;
   UserVA end;

   UserMemLock(mem);
   end = mem->dataEnd;
   ASSERT(end >= mem->dataStart);
   ASSERT(end <= VMK_USER_FIRST_MMAP_TEXT_VADDR);
   UserMemUnlock(mem);

   return end;
}


/*
 *----------------------------------------------------------------------
 *
 * UserMem_SetDataEnd --
 *
 *      Send the end of a user world's data segment.  The world is
 *	allowed to access data up to this point.
 *
 * Results:
 *	VMK_OK if brk is set, VMK_BAD_PARAM or VMK_LIMIT_EXCEEDED if
 *	brk goes below or above hard limits, respectively.
 *
 * Side effects:
 *      Current world's data end increased.  No pages actually allocated.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserMem_SetDataEnd(World_Handle *world, UserVA dataEnd)
{
   User_CartelInfo *uci = world->userCartelInfo;
   UserMem *mem = &uci->mem;
   UserMemMapInfo *heapMap;
   VMK_ReturnStatus status = VMK_OK;
   UserMemPTEList pteList = NULL;
   List_Links mmInfosToFree;
   List_Init(&mmInfosToFree);

   UserMemLock(mem);

   UWLOG(1, "Old end 0x%x  Requested end 0x%x", mem->dataEnd, dataEnd);
   heapMap = mem->heapInfo;

   if (dataEnd < mem->dataStart) {
      status = VMK_BAD_PARAM;
   } else if (dataEnd >= (VMK_USER_FIRST_MMAP_TEXT_VADDR - PAGE_SIZE)) {
      status = VMK_LIMIT_EXCEEDED;
   }

   if (status != VMK_OK) {
      UserMemUnlock(mem);
      return status;
   }
   
   /*
    * Heap mmap region hasn't been created, create one if heap size 
    * is not  zero.
    */
   if (heapMap == NULL) {
      mem->dataEnd = dataEnd;
      /*
       * This is the first time we move dataEnd beyond dataStart,
       * so create the heap mmap region.
       */
      if (dataEnd > mem->dataStart) {
         // Map in the heap.
         status = UserMemMapCreate(world, &mem->dataStart, FALSE,
                                   dataEnd - mem->dataStart,
                                   PTE_P | PTE_RW, USERMEM_MAPTYPE_ANON,
                                   FALSE, 0, NULL, 0, MEMMAP_IGNORE, &mem->heapInfo);
         if (status != VMK_OK) {
            UWWarn("Unable to get heap's mmInfo: %s start 0x%x end 0x%x",
                   UWLOG_ReturnStatusToString(status), mem->dataStart, mem->dataEnd);
            ASSERT(FALSE);
         } else {
            mem->dataEnd = dataEnd;
            ASSERT(mem->heapInfo != NULL);
         }
      }
      UserMemUnlock(mem);
      return status;
   }

   /*
    * Adjust the heap mmap region to the new heap size.
    */
   ASSERT(mem->dataStart == heapMap->startAddr);
   ASSERT(mem->dataEnd == heapMap->startAddr + heapMap->length);

   if (dataEnd < mem->dataEnd) {
      uint32 len = mem->dataEnd - dataEnd;
      Bool freeMe = FALSE;
      status = UserMemMapDestroyMMInfo(world, heapMap, dataEnd, len,
                                       &freeMe, &pteList);
      ASSERT(status == VMK_OK);
      if (freeMe) {
         ASSERT(mem->dataStart == dataEnd);
         List_Remove(&heapMap->links);
	 List_Insert(&heapMap->links, LIST_ATFRONT(&mmInfosToFree));
         mem->heapInfo = NULL;
      }
   } else if (dataEnd > mem->dataEnd) {
      UserVA oldEnd = mem->dataEnd;
      status = UserMemMapTryExtending(world, heapMap, &oldEnd,
                                      dataEnd - oldEnd, heapMap->prot,
                                      heapMap->pinned, FALSE);
   }
   if (status == VMK_OK) {
      mem->dataEnd = dataEnd;
   } else {
      UWWarn("set data end %s: old end 0x%x new end 0x%x",
             UWLOG_ReturnStatusToString(status), mem->dataEnd, dataEnd);
   }
   UserMemUnlock(mem);

   UserMemFlushAndFreePages(world, pteList);
   UserMemCleanupAndFreeMMInfos(world, &mmInfosToFree);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserMemFreeCanonicalPageTable --
 *
 *	Free the canonical page tables, dirs, and root.
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
UserMemFreeCanonicalPageTable(UserMem* mem)
{
   VMK_PDE *pageDir;
   KSEG_Pair *ksegDir;
   int i, j;

   ASSERT(mem->canonicalPageRootMA != 0);
   ASSERT(UserMemIsLocked(mem));

   // make sure there are no pending references to page table pages.
   ASSERT(mem->ptRefCount == 0);
   
   // free page table pages
   for (i = 0; i < VMK_NUM_PDPTES; i++) {
      pageDir = PT_GetPageDir(mem->canonicalPageRootMA,
                              PTBITS_ADDR(i, 0, 0), &ksegDir);
      if (pageDir != NULL) {
         for (j = 0; j < VMK_PDES_PER_PDPTE; j++) {
            VMK_PDE *pde = &pageDir[j];
            if (PTE_PRESENT(*pde)) {
               MPN mpn = VMK_PDE_2_MPN(*pde);
               MemMap_FreeKernelPage(mpn);
               PT_INVAL(pde);
            }
         }

         PT_ReleasePageDir(pageDir, ksegDir);
      }
   }

   // free page dir + page root
   PT_FreePageRoot(mem->canonicalPageRootMA);
   mem->canonicalPageRootMA = 0;
}

/*
 *----------------------------------------------------------------------
 *
 * UserMemCanonicalPageTable --
 *
 *	From the given UserMem get the cartel-wide (canonical) page
 *	table for the given linear address.  The page table itself is
 *	shared among all threads in the cartel expressly so any
 *	updates to it will be visible to those worlds.
 *
 *      Once the page table is allocated, the page table cannot
 *      be freed from the canonical page table until all threads
 *      have cleaned up their root page tables.
 *
 *	Caller must have the cartel-wide UserMem lock.
 *
 *	Caller must call UserMemReleasePageTable on the returned table.
 *
 * Results:
 *	Returns virtual address of the PageTable (must be released
 *	with a call to UserMemReleasePageTable).  Sets outPageTableMPN to
 *	the MPN of the page table.  Returns NULL if some part of the
 *	page table couldn't be mapped or a new table couldn't be
 *	allocated.
 *
 * Side effects:
 *	May allocate a page table if the corresponding entry in the
 *	page directory is empty.
 *
 *----------------------------------------------------------------------
 */
static VMK_PTE*
UserMemCanonicalPageTable(UserMem* userMem, const LA laddr, MPN* outPageTableMPN)
{
   VMK_PDE *pageDir;
   VMK_PTE *pageTable;
   KSEG_Pair *ksegDir;

   ASSERT(userMem->canonicalPageRootMA != 0);
   ASSERT(UserMemIsLocked(userMem));
   
   pageDir = PT_GetPageDir(userMem->canonicalPageRootMA, laddr, &ksegDir);
   if (pageDir) {
      pageTable = PT_GetPageTableInDir(pageDir, laddr, NULL);
      if (pageTable == NULL) {
         if (PTE_PRESENT(pageDir[ADDR_PDE_BITS(laddr)])) {
            UWLOG(0, "mapping failure");
         } else {
            pageTable = PT_AllocPageTableInDir(pageDir, laddr, PTE_US, 
                                               NULL, /* KSEG_Pair */
                                               outPageTableMPN);
            UWLOG(3, "   No table, alloc'd one (mapped at %p).",
                  pageTable);
            if (pageTable == NULL) {
               UWWarn("Failed to alloc page table for cartel");
            }
         }
      } else {
         if (outPageTableMPN) {
            *outPageTableMPN = VMK_PTE_2_MPN(pageDir[ADDR_PDE_BITS(laddr)]);
         }
      }

      UWLOG(5, "la=%#x -> pte=%#Lx pt=%p",
            laddr, pageDir[ADDR_PDE_BITS(laddr)], pageTable);
      PT_ReleasePageDir(pageDir, ksegDir);
   } else {
      if (outPageTableMPN) {
         *outPageTableMPN = INVALID_MPN;
      }
      pageTable = NULL;
      UWWarn("Failed to get canon pageDir! That's bad.  canonRootMA=%#Lx",
             userMem->canonicalPageRootMA);
   }

   if (pageTable != NULL) {
      userMem->ptRefCount++;
   }
   return pageTable;
}


/*
 *----------------------------------------------------------------------
 *
 * UserMemLookupPageTable --
 *
 *	Get a Page Table for the given laddr.  Generally gets the
 *	table out of the given pageRoot.  But if no table is in that
 *	root, it looks in the canonical root (and may allocate a new
 *	table if the canonical root doesn't have the appropriate
 *	table).
 *
 * Results:
 * 	VMK_OK if *outPageTable is set to the appropriate page table.
 *      VMK_NO_RESOURCES if no kernel address space to map page directory.
 *      VMK_NO_ADDRESS_SPACE if no address space to create page table.
 * 	VMK_NO_MEMORY if no memory to create page table.
 * 	Returned Page Table must be released with UserMemReleasePageTable.
 *
 * Side effects:
 *	Page tables may be allocated.  Pages directories and tables
 *	are mapped in.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserMemLookupPageTable(UserMem* userMem,
                       const MA pageRootMA,
                       const LA laddr,
                       VMK_PTE** outPageTable)
{
   const uint32 pdIndex = ADDR_PDE_BITS(laddr);
   VMK_ReturnStatus status;
   VMK_PDE* pageDir = NULL;
   VMK_PTE* pageTable = NULL;
   KSEG_Pair *ksegDir;

   ASSERT(userMem != NULL);
   ASSERT(outPageTable != NULL);
   ASSERT(UserMemIsLocked(userMem));

   /* Get the Page Directory for laddr. */
   pageDir = PT_GetPageDir(pageRootMA, laddr, &ksegDir);
   if (pageDir == NULL) {
      status = VMK_NO_RESOURCES;
      UWLOG(0, "VMK_NO_RESOURCES: Couldn't get page directory!");
   } else {
      /* Get existing or new Page Table for laddr. */
      if (PTE_PRESENT(pageDir[pdIndex])) {
         pageTable = PT_GetPageTableInDir(pageDir, laddr, NULL);
         if (pageTable != NULL) {
            userMem->ptRefCount++;
            status = VMK_OK;
         } else {
            UWLOG(0, "VMK_NO_ADDRESS_SPACE: Couldn't map page table!");
            status = VMK_NO_ADDRESS_SPACE;
         }
      } else {
         MPN pageTableMPN = INVALID_MPN;
         
         /*
          * The appropriate Page Table isn't in current world's
          * private Page Directory, look up the canonical Page Table
          * and update current world's private Page Directory.  The
          * canonical Page Table is shared.
          */
         pageTable = UserMemCanonicalPageTable(userMem, laddr, &pageTableMPN);
         
         if (pageTable != NULL) {
            /* Add the shared Page Table to our private Page Directory */
            ASSERT(pageTableMPN != INVALID_MPN);
            PT_SET(&pageDir[pdIndex], VMK_MAKE_PDE(pageTableMPN, 0, PTE_KERNEL|PTE_US));
            status = VMK_OK;
         } else {
            status = VMK_NO_MEMORY;
            UWLOG(0, "VMK_NO_MEMORY: Couldn't get canonical page table!");
         }
      }

      PT_ReleasePageDir(pageDir, ksegDir);
   }

   *outPageTable = pageTable;
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * UserMemAddPageToTable --
 *
 *	Add the given mpn to the given pageTable at the offset for the
 *	given laddr.  If the pageTable already has an entry, return
 *	VMK_BUSY and leave the entry alone.  Sets outMPN to the MPN only
 *	on success (VMK_OK or VMK_BUSY).
 *
 * Results:
 * 	VMK_BUSY means page is already mapped for laddr in pageTable.
 * 	That is NOT necessarily a total failure.  Returns VMK_FAILURE
 * 	if an existing PTE is incompatible with the readOnly flag.
 *
 * Side effects:
 *	A page table entry in pageTable may be twiddled.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserMemAddPageToTable(UserMem *userMem,
                      VMK_PTE* pageTable,
                      const LA laddr,
                      Bool pinned,
                      Bool isWrite,
                      const MPN mpnToAdd,
		      void* data)
{
   VMK_ReturnStatus status;
   UserPTE *pte = UserPTE_For(pageTable, laddr);

   ASSERT(mpnToAdd != INVALID_MPN);
   ASSERT(pageTable != NULL);
   ASSERT(UserMemIsLocked(userMem));

   /*
    * The page table may already have the given laddr mapped in it.
    * (Other threads, first fault in that page directory, the caller
    * was mistaken about the fault).
    */
   if (UserPTE_IsMapped(pte)) {
     
      /* Let caller know page was already available (if they care) */
      status = VMK_BUSY;
      UWLOG(1, "VMK_BUSY: Page already present (pte=%#Lx)", pte->u.raw);
   } else {
      uint32 prot;
      void* curData;

      if (UserPTE_IsInUse(pte)) {
         prot = UserPTE_GetProt(pte);
         curData = UserPTE_GetPtr(pte);
      } else {
	 /*
	  * Looks like someone removed the mapping out from under us while we
	  * dropped the UserMem lock.
	  */
	 return VMK_INVALID_ADDRESS;
      }

      /*
       * If they specified a data value, it means we should make sure the pte
       * we're about to add a page to is in use and has the same value as they
       * provided.
       */
      if (data != curData) {
	 /*
	  * Looks like the old mapping was replaced by a new one while we
	  * dropped the UserMem lock.
	  */
	 return VMK_INVALID_ADDRESS;
      }

      /*
       * prot should not be 0, otherwise we shouldn't have gotten this far while
       * faulting in a page.
       */
      ASSERT(prot != 0);
      /*
       * Update the PTE entry. If the instruction is write, don't delay 
       * setting PTE_RW.
       */
      UserPTE_Set(pte, mpnToAdd, prot | PTE_US, pinned, !isWrite);
      status = VMK_OK;
      UWLOG(4, "VMK_OK: Added mapping to page table (pt=%p pte=%#Lx mpn=%#x)",
            pageTable, pte->u.raw, mpnToAdd);
   }


   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * UserMemPSharePTE --
 *
 *	Try to page share the page given the corresponding PTE.
 *      If a page with the same content is found, change the PTE
 *      to map the new MPN and return the old MPN for removal.
 *      Otherwise, register the current page in pshare hashtable.
 *
 * Results:
 *      MPN to be removed if an existing page is found.
 *
 * Side effects:
 *	PTE will be changed.
 *
 *----------------------------------------------------------------------
 */
static INLINE_SINGLE_CALLER MPN
UserMemPSharePTE(World_Handle *world,   // IN: the world
                 UserMem *userMem,      // IN: the usermem
                 UserPTE *pte,       // IN/OUT: pte of the page
                 Bool *needFlush)       // OUT: need to flush TLB
{
   MPN mpnRemove = INVALID_MPN;
   *needFlush = FALSE;

   ASSERT(&world->userCartelInfo->mem == userMem);
   ASSERT(UserMemIsLocked(userMem));
   /*
    * If the page exists, is not pinned, is readonly and has not been shared,
    * try to pshare it.
    */
   if (UserPTE_IsPresent(pte) && !UserPTE_IsPinned(pte) &&
       !UserPTE_HdWriteEnabled(pte) && !UserPTE_IsPShared(pte)) {
      VMK_ReturnStatus status;
      MPN mpn = UserPTE_GetMPN(pte);
      MPN mpnShared;
      uint32 count;
      uint64 key = PShare_HashPage(mpn);

      status = PShare_Add(key, mpn, &mpnShared, &count);
      if (status != VMK_OK) {
         return INVALID_MPN;
      }

      /*
       * Set to free the old mpn.
       */
      if (mpnShared != mpn) {
         ASSERT(count > 1);
         mpnRemove = mpn;
         Atomic_Inc(&userMemStats.pageShared);
         *needFlush = TRUE;
      }

      UserMemUsage(world)->cow++;
      UserMemUsage(world)->pageable--;
      UserPTE_SetPShare(pte, mpnShared);
   }

   return mpnRemove;
}


/*
 *----------------------------------------------------------------------
 *
 * UserMem_PSharePage  --
 *
 *      Try to share the page content with other worlds.
 *
 *      This function is be called when the content of a user world page
 *      could be shared. This would be most suitable for code pages, but
 *      can also be applies to data pages for aggressive page sharing.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	The original mpn is freed if a match is found.
 *
 *----------------------------------------------------------------------
 */
void
UserMem_PSharePage(World_Handle* world, 
                   const VPN vpn)
{
   const LA laddr = LPN_2_LA(VMK_USER_VPN_2_LPN(vpn));
   UserMem* userMem = &world->userCartelInfo->mem;
   MPN mpnRemove;
   Bool needFlush;
   VMK_PTE *pageTable = NULL;
   UserPTE *pte;
   VMK_ReturnStatus status;

   UserMemLock(userMem);
   status = UserMemLookupPageTable(userMem, world->pageRootMA,
                                   laddr, &pageTable);
   ASSERT(status == VMK_OK);

   pte = UserPTE_For(pageTable, laddr);

   mpnRemove = UserMemPSharePTE(world, userMem, pte, &needFlush);
   UWLOGFor(3, world, "PShare: vpn=%#x mpnShared=%#x mpnRemove=%#x",
            vpn, UserPTE_GetMPN(pte), mpnRemove);

   UserMemReleasePageTable(userMem, pageTable);
   UserMemUnlock(userMem);

   if (needFlush) {
      // flush TLB
      UserMemCartelFlush(world->userCartelInfo);
   }

   if (mpnRemove != INVALID_MPN) { 
      // free the mpn
      UserMemFreePage(world, mpnRemove);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * UserMemSwapOut --
 *
 *	Swap out userworld pages in the swapList.  Issue requests to 
 *      the swap module.
 *
 * Results:
 *	VMK_ReturnStatus
 *
 * Side effects:
 *	PTE entry in the page table gets changed.
 *
 *----------------------------------------------------------------------
 */
static void
UserMemSwapOut(UserMem *mem, World_Handle *world)
{
   uint32 i;
   UserMemSwapList *swapList = &mem->swapList;

   ASSERT(&world->userCartelInfo->mem == mem);
   UserMemLock(mem);

   for (i = 0; i < USERMEM_NUM_SWAP_REQS; i++) {
      VMK_ReturnStatus status;
      UserMemSwapReq *req = &swapList->reqs[i];
      LPN lpn;
      MPN mpn;
      uint32 swapFileSlot;

      // skip reqs that do not have swap-out request pending
      if (req->state != USERMEM_SWAP_OUT_REQ) {
         continue;
      }

      // set the status to swapping out
      req->state = USERMEM_SWAPPING_OUT;

      lpn = req->lpn;
      ASSERT(UserPTE_IsPresent(&req->pte));
      mpn = UserPTE_GetMPN(&req->pte);

      // Issue swap-out request for the page.
      UserMemUnlock(mem);
      status = Swap_UWSwapOutPage(world, i, (PPN)lpn, mpn, &swapFileSlot);
      UserMemLock(mem);

      if (status != VMK_OK) {
         UWLOGFor(0, world, "swap-out failed lpn %x mpn %x status %s",
                  lpn, mpn, VMK_ReturnStatusToString(status));

         if (req->state == USERMEM_SWAPPING_OUT) {
            LA la = LPN_2_LA(lpn);
            VMK_PTE *pageTable;

            // restore the original pte value
            status = UserMemLookupPageTable(mem, world->pageRootMA, la, &pageTable);
            if (status == VMK_OK) {
               UserPTE *pte = UserPTE_For(pageTable, la);
               // assert that mpn hasn't been changed in req
               ASSERT(UserPTE_IsSwapping(pte) && UserPTE_GetMPN(&req->pte) == mpn);
               UserPTE_SetImmed(pte, req->pte.u.raw);
               UserMemReleasePageTable(mem, pageTable);
            } else {
               ASSERT(FALSE);
            }
         } else {
            // If the swap-out request has been canceled, do nothing.
            ASSERT(req->state == USERMEM_SWAP_CANCELED);
         }

         // release the swap req
         UserMemFreeSwapReq(swapList, i); 
         continue;
      }

      UWLOGFor(2, world, "swap-out swapFileSlot %x lpn %x mpn %x status %s",
               swapFileSlot, lpn, mpn, VMK_ReturnStatusToString(status));
   }

   UserMemUnlock(mem);
}

/*
 *-----------------------------------------------------------------------------
 *
 * UserMem_MarkSwapPage --
 *      Callback function after swap-out finishes.
 *      Mark a page "lpn" as swapped out in the page table and 
 *      free the associated "mpn". 
 *
 * Results:
 *      TRUE if page swapping succeeded.  The caller is responsible
 *      for freeing up the swapSlot when returning FALSE.
 *
 * Side effects:
 *      updates the page table and MPN freed.
 *
 *-----------------------------------------------------------------------------
 */
Bool
UserMem_MarkSwapPage(World_Handle *world,       // the world
                     uint32 reqNum,             // req number in swap list
                     Bool writeFailed,          // swap file write failed
                     uint32 swapFileSlot,       // slot number in swap file
                     LPN lpn,                   // LPN of the page
                     MPN mpn)                   // MPN of the page
{
   VMK_PTE *pageTable;
   VMK_ReturnStatus status;
   User_CartelInfo *uci = world->userCartelInfo;
   UserMem *mem = &uci->mem;
   UserMemSwapList *swapList = &mem->swapList;
   UserMemSwapReq *req;
   LA la = LPN_2_LA(lpn);
   Bool swapSucceed;

   UWLOG(2, "Mark page req %d swapFileSlot %x lpn %x mpn %x state %d", 
         reqNum, swapFileSlot, lpn, mpn, swapList->reqs[reqNum].state);
   UserMemLock(mem);

   req = &swapList->reqs[reqNum]; 
   if (req->state != USERMEM_SWAPPING_OUT) {
      // the swap-out request has been canceled
      UserMemFreeSwapReq(swapList, reqNum); 
      UWLOG(1, "swap-out request canceled, lpn %x mpn %x", lpn, mpn);
      UserMemUnlock(mem);
      return FALSE;
   }

   status = UserMemLookupPageTable(mem, world->pageRootMA, la, &pageTable);
   ASSERT(status == VMK_OK);
   if (UNLIKELY(status != VMK_OK)) {
      swapSucceed = FALSE;
   } else {
      UserPTE *pte = UserPTE_For(pageTable, la);
      uint32 pteFlags = UserPTE_GetFlags(pte);
      // the pte shouldn't change
      ASSERT(UserPTE_IsSwapping(pte) && UserPTE_GetMPN(pte) == mpn);

      if (!writeFailed) {
         // set the page as swapped
         UserPTE_SetSwap(pte, swapFileSlot, pteFlags);
         UserMemUsage(world)->swapped++;
         UserMemUsage(world)->pageable--;
         Atomic_Inc(&userMemStats.pageSwapped);
         // free the machine page
         UserMemFreePage(world, mpn);
         swapSucceed = TRUE;
      } else {
         // restore the old pte.
         UserPTE_Set(pte, mpn, pteFlags, FALSE, TRUE);
         swapSucceed = FALSE;
      }
      UserMemReleasePageTable(mem, pageTable);
   }

   // release the swap req
   UserMemFreeSwapReq(swapList, reqNum); 
   UWLOG(2, "Mark page finished lpn %x succeed %d", lpn, swapSucceed);
   UserMemUnlock(mem);

   return swapSucceed;
}

/*
 *-----------------------------------------------------------------------------
 *
 * UserMemOverlapUserLARange --
 *      Test whether an address range overlaps with the user address range.
 *
 * Results:
 *      TRUE if there is an overlap.
 *
 * Side effects:
 *      none
 *
 *-----------------------------------------------------------------------------
 */
static INLINE Bool
UserMemOverlapUserLARange(LA startLA, uint32 length)
{
   // sanity check for wrapping around
   ASSERT(startLA <= startLA + (length - 1));

   if (startLA + (length - 1) < VMK_USER_FIRST_LADDR ||
       startLA > VMK_USER_LAST_LADDR) {
      return FALSE;
   }
   return TRUE;
}

/*
 *-----------------------------------------------------------------------------
 *
 * UserMemSwapScan --
 *      Scan the user cartel's page table. Try to swap out "numPages" pages.
 *      Find swap-out candidate pages using a pseudo LRU algorithm by walking
 *      through the page table in round-robbin and swap out pages that do
 *      not have access bit (PTE_A) set. Meanwhile, for pages that have the 
 *      access bits, clear the access bits during the scan.
 *
 * Results:
 *      Number of pages starting being swapped.
 *
 * Side effects:
 *      Page table could be modified.
 *
 *-----------------------------------------------------------------------------
 */
static uint32
UserMemSwapScan(User_CartelInfo *uci, uint32 numPages)
{
   uint32 pgFreed = 0;
   uint32 pdpteStart, pdeStart, pteStart;
   uint32 i, j, k;
   UserMem *mem = &uci->mem;
   UserMemSwapList *swapList = &mem->swapList;
   uint32 curReqNum;
   Bool keepSwapping = TRUE;

   ASSERT(UserMemIsLocked(mem));

   if (swapList->numFreeReqs == 0) {
      return 0;
   }
   curReqNum = UserMemFirstFreeSwapReq(swapList);
   ASSERT(curReqNum != USERMEM_INVALID_SWAP_REQ);

   pdpteStart = ADDR_PDPTE_BITS(mem->swapScanLA);
   pdeStart = ADDR_PDPTE_BITS(mem->swapScanLA);
   pteStart = ADDR_PTE_BITS(mem->swapScanLA);

   for (i = 0; i < VMK_NUM_PDPTES && keepSwapping; i++) {
      // for each page table dir
      VMK_PDE *pageDir;
      uint32 pdpteIndex = (i + pdpteStart) % VMK_NUM_PDPTES;
      pageDir = PT_GetPageDir(mem->canonicalPageRootMA,
                              PTBITS_ADDR(pdpteIndex, 0, 0), NULL);
      if (pageDir != NULL) {
         for (j = 0; j < VMK_PDES_PER_PDPTE && keepSwapping; j++) {
            // for each page table
            VMK_PTE *pageTable;
            uint32 pdeIndex = (j + pdeStart) % VMK_PDES_PER_PDPTE;
            LA pdeLA = PTBITS_ADDR(pdpteIndex, pdeIndex, 0);

            // check for address range
            if (!UserMemOverlapUserLARange(pdeLA, PDE_SIZE)) {
               continue;
            }
            pageTable = PT_GetPageTableInDir(pageDir, 
                                             PTBITS_ADDR(pdpteIndex, pdeIndex, 0), NULL);
            if (pageTable != NULL) {
               mem->ptRefCount++;
               for (k = 0; k < VMK_PTES_PER_PDE && keepSwapping; k++) {
                  // for each pte
                  uint32 pteIndex = (k + pteStart) % VMK_PTES_PER_PDE;
                  LA pteLA = PTBITS_ADDR(pdpteIndex, pdeIndex, pteIndex);
                  UserPTE *pte = UserPTE_For(pageTable, pteLA);

                  // check for address range
                  if (!UserMemOverlapUserLARange(pteLA, PAGE_SIZE)) { 
                     continue;
                  }
                  // if the page is not shared or pinned, we may try to swap it out
                  if (UserPTE_IsPresent(pte) && 
                      !UserPTE_IsPShared(pte) && !UserPTE_IsPinned(pte)) {
                     if (PTE_ACCESS(pte->u.raw)) {
                        // clear the access bit
                        UserPTE_SetImmed(pte, VMK_PTE_CLEAR_ACCESS(pte->u.raw));
                     } else {
                        uint32 pteFlags = UserPTE_GetFlags(pte);
                        MPN mpn = UserPTE_GetMPN(pte);
                        // setup the swap req
                        UserMemInitSwapReq(swapList, curReqNum, 
                                           USERMEM_SWAP_OUT_REQ, LA_2_LPN(pteLA), pte);
                        // set the PTE in swapping
                        UserPTE_SetSwapBusy(pte, mpn, pteFlags);

                        if (++pgFreed >= numPages) {
                           // swapped out enough pages
                           mem->swapScanLA = pteLA;
                           keepSwapping = FALSE;
                        } else {
                           curReqNum = UserMemNextFreeSwapReq(swapList, curReqNum);
                           if (curReqNum == USERMEM_INVALID_SWAP_REQ) {
                              keepSwapping = FALSE;
                           }
                        }
                     }
                  }
               }
               UserMemReleasePageTable(mem, pageTable);
            }
            pteStart = 0;
         }
         PT_ReleasePageDir(pageDir, NULL);
      }
      pdeStart = 0;
   }
   return pgFreed;
}


/*
 *-----------------------------------------------------------------------------
 *
 * UserMem_SwapOutPages --
 *      Try to swap out "numPages" from "world".
 *
 * Results:
 *      actual number of pages to swap out
 *
 * Side effects:
 *      pages swapped and TLB flushed
 *
 *-----------------------------------------------------------------------------
 */
uint32
UserMem_SwapOutPages(World_Handle *world, uint32 numPages)
{
   User_CartelInfo *uci = world->userCartelInfo;
   UserMem *mem = &uci->mem;
   uint32 numToSwap = 0;

   if (!Swap_IsEnabled()) {
      UWLOG(2, "swap not enabled");
      return numToSwap;
   }

   UserMemLock(mem);
   numToSwap = UserMemSwapScan(uci, numPages);
   UWLOG(2, "tagged %d pages for swap out", numToSwap);
   UserMemUnlock(mem);

   if (numToSwap > 0) {
      UserMemCartelFlush(uci);
      UserMemSwapOut(mem, world);
   }

   return numToSwap;
}


/*
 *-----------------------------------------------------------------------------
 *
 * UserMemSwapInPage --
 *      Swap in a page and update the PTE.
 *
 *      Called with userMem lock held. This call may block
 *      and the userMem lock will be released and re-acquired.
 *
 * Results:
 *      If the page is mapped at the LPN, the PTE becomes present.
 *      The state of the PTE is protected by the userMem lock.
 *
 * Side effects:
 *      The page could be swapped in and PTE could be changed.
 *      The userMem lock will be released and re-acquired.
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserMemSwapInPage(World_Handle *world, UserPTE *pte, LPN lpn)
{
   User_CartelInfo *uci = world->userCartelInfo;
   UserMem* mem = &uci->mem;
   UserMemSwapList *swapList = &mem->swapList;
   MPN mpn;
   VMK_ReturnStatus status;
   uint32 curReqNum, swapFileSlot;

   ASSERT(!UserPTE_IsPresent(pte));

   while (1) {
      ASSERT(UserMemIsLocked(mem));

      // handle the case when the page is being swapped, either in or out.
      while (UserPTE_IsSwapping(pte)) {
         mpn = UserPTE_GetMPN(pte);
         /*
          * If the page is being swapped out, restore the page and 
          * cancel swap-out operation, otherwise wait for swap-in to finish.
          */
         if (mpn != INVALID_MPN) {
            uint32 pteFlags = UserPTE_GetFlags(pte);

            UWLOG(1, "UserMemSwapIn: skip swap-out for page %x mpn %x", lpn, mpn);
            UserPTE_Set(pte, mpn, pteFlags | PTE_A, FALSE, TRUE);
            UserMemCancelSwapOut(swapList, lpn);
            return VMK_OK;
         } else {
            UWLOG(1, "UserMemSwapIn: wait for page %x", lpn);
            status = CpuSched_Wait(USERMEM_HASH_LPN(uci->cartelID, lpn),
                                   CPUSCHED_WAIT_SWAPIN, &mem->lock);
            if (status != VMK_OK) {
               return status;
            }
            UserMemLock(mem);
         }
      }

      /*
       * If the page has become present or no longer swapped out, do nothing.
       */
      if (UserPTE_IsPresent(pte) || !UserPTE_IsSwapped(pte)) {
         return VMK_OK;
      }

      if (swapList->numFreeReqs > 0) {
         // There is a free swap req, start swapping in the page. 
         break;
      } else {
         /*
          * Try to grab a free swap req. 
          * If none is available, wait for one to become available.
          */
         UWLOG(1, "wait for a free swap req %p", swapList);
         status = CpuSched_Wait((uint32)(uintptr_t)swapList, CPUSCHED_WAIT_SWAPIN, 
                                &mem->lock);
         if (status != VMK_OK) {
            return status;
         }
         UserMemLock(mem);
         // Since we released the lock, start all over.
      }
   }

   curReqNum = UserMemFirstFreeSwapReq(swapList);
   ASSERT(curReqNum != USERMEM_INVALID_SWAP_REQ);

   swapFileSlot = UserPTE_GetSwapSlot(pte);

   // setup the swap req for swap in
   UserMemInitSwapReq(swapList, curReqNum, USERMEM_SWAPPING_IN, lpn, pte);

   // mark pte in the process of swap-in.
   UserPTE_SetSwapBusy(pte, INVALID_MPN, UserPTE_GetFlags(pte));

   /*
    * From now on, we hold mpn and swapFileSlot.
    * If swap-in is successful, we need to free swapFileSlot.
    * If swap-in failed, we need to free mpn.
    * If swap-in is canceled, we need to free both.
    */
   UserMemUnlock(mem);

   // allocate a machine page
   status = UserMemAllocPage(world, &mpn);

   // copy the page content to the machine page
   if (status == VMK_OK) {
      UWLOG(2, "swap-in lpn 0x%x swapFileSlot %d new mapn 0x%x", lpn, swapFileSlot, mpn);
      status = Swap_GetSwappedPage(world, swapFileSlot, mpn, NULL, (PPN)lpn);
      ASSERT(status == VMK_OK);
      if (status != VMK_OK) {
         // swap-in failed, free up mpn
         UserMemFreePage(world, mpn);
      } else {
         Swap_DoPageSanityChecks(world, swapFileSlot, mpn, (PPN)lpn);
         // swap-in done, free up the swap slot
         Swap_UWFreeFileSlot(swapFileSlot);
      }
   }

   // By now, we have either freed the swap file slot (VMK_OK) or the mpn (!VMK_OK).

   UserMemLock(mem);

   if (UNLIKELY(swapList->reqs[curReqNum].state != USERMEM_SWAPPING_IN)) {
      // If the swap-in request has been canceled, free all resources held.
      ASSERT(swapList->reqs[curReqNum].state == USERMEM_SWAP_CANCELED);
      UWLOG(1, "UserMemSwapIn: canceled %x", lpn);
      if (status == VMK_OK) {
         UserMemFreePage(world, mpn);
      } else {
         Swap_UWFreeFileSlot(swapFileSlot);
      }
   } else {
      uint32 pteFlags = UserPTE_GetFlags(pte);

      ASSERT(UserPTE_IsSwapping(pte));
      if (UNLIKELY(status != VMK_OK)) {
         // swap-in failed, restore the old PTE
         UserPTE_SetSwap(pte, swapFileSlot, pteFlags);
      } else {
         // set the new PTE with access bit
         UserPTE_Set(pte, mpn, pteFlags | PTE_A, FALSE, TRUE);
         UserMemUsage(world)->swapped--;
         UserMemUsage(world)->pageable++;
         Atomic_Dec(&userMemStats.pageSwapped);
      }
   }

   // wake up all waiting on this page
   UWLOG(2, "UserMemSwapIn: wake up page %x", lpn);
   CpuSched_Wakeup(USERMEM_HASH_LPN(uci->cartelID, lpn));
   // free the swap req
   UserMemFreeSwapReq(swapList, curReqNum); 

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserMemMapInfoInsert --
 *
 *      Inserts the given mmInfo structure into the mmaps list
 *      for the UserMem.
 *
 * Results:
 *      None. 
 *
 * Side effects:
 *      mmaps list is altered.
 *
 *----------------------------------------------------------------------
 */
static void
UserMemMapInfoInsert(UserMem* mem, UserMemMapInfo* mmInfoToInsert)
{
   List_Links* itemPtr;
   UserMemMapInfo* tempMMInfo;
   Bool inserted = FALSE;
   ASSERT(UserMemIsLocked(mem));

   // insert in ascending order.
   LIST_FORALL(&mem->mmaps, itemPtr) {
      tempMMInfo = (UserMemMapInfo*)itemPtr;
      
      if (mmInfoToInsert->startAddr < tempMMInfo->startAddr) {
         ASSERT(mmInfoToInsert->startAddr + mmInfoToInsert->length <=
               tempMMInfo->startAddr);
         List_Insert(&mmInfoToInsert->links, LIST_BEFORE(&tempMMInfo->links));
         inserted = TRUE;
         break;
      }
   }
   if (!inserted) {
      List_Insert(&mmInfoToInsert->links, LIST_ATREAR(&mem->mmaps));
   }
}

/*
 *----------------------------------------------------------------------
 *
 * UserMemMapAllocRange --
 *
 *	Search the address space for either the given address range or find
 *	a new range big enough to hold given mmap request. Store the given
 *	arg pointer in each PTE.  If overwrite is TRUE, it will overwrite a
 *	current mapping, skipping those PTEs that already have a page mapped.
 *
 * Results:
 *	VMK_OK on success, VMK_EXISTS if range already allocated, or other status
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserMemMapAllocRange(User_CartelInfo *uci,
                     UserVA *addr,         // INOUT
		     Bool overwrite,
                     uint32 length,
		     uint32 prot,
                     UserMemMapInfo* mmInfo,
                     UserMemMapExecFlag execFlag)
{
   UserMem* mem = &uci->mem;
   VMK_ReturnStatus status = VMK_OK;
   VPN startVPN = VA_2_VPN(*addr);
   uint32 nPages = UserMemMapLenghInPages(*addr, length);
   UserPTE *pte;
   int i;
 
   ASSERT(UserMemIsLocked(mem));
   ASSERT(PAGE_OFFSET(*addr) == 0);
   ASSERT(length != 0);

   if (startVPN) {
      /* 
       * Requesting a specific address range.
       */
      if (!overwrite) {
	 /* 
	  * They didn't specify MAP_FIXED, so let's check for availability.
	  */
         status = UserMemMapRangeCheckEmpty(uci, startVPN, nPages);
         if (status != VMK_OK) {
	    return status;
	 }
      } else {
         /*
          * Give a warning if they passed MMAP_FIXED (overwrite) 
          * with startVPN in code segment and it moved past the segment
          */
         if (startVPN < VMK_USER_MAX_CODE_SEG_PAGES) {
            if((startVPN + nPages) > VMK_USER_MAX_CODE_SEG_PAGES) {
               UWWarn("mmap region extends beyond code segment\n");
            }
         }
      }

      /* 
       * If they specified MAP_FIXED or if the range specified is empty,
       * ignore execFlag and allocate the  requested address range. 
       * Mark new pages as in use
       */
      status = UserMemSetPTEInUseRange(uci, startVPN, nPages, prot, mmInfo,
				       overwrite);
   } else {
      /*
       * Address not specified, so go find a big enough hole in the address
       * space to map this region.  This currently uses a slow linear
       * search, we might need to upgrade to using a binary tree or something.
       */
      uint32 searchPages;
      VPN freeVPN = INVALID_VPN;
      /*
       * If execFlag is MEMMAP_EXEC, then we need to allocate memory from the mmap
       * region in the CS, otherwise we allocate from the DS
       */
      ASSERT(execFlag != MEMMAP_IGNORE);

      if (execFlag == MEMMAP_EXEC) {
         startVPN = VA_2_VPN(VMK_USER_FIRST_MMAP_TEXT_VADDR);
         searchPages = VA_2_VPN(VMK_USER_LAST_MMAP_TEXT_VADDR) - startVPN + 1;
      } else {
         startVPN = VA_2_VPN(VMK_USER_FIRST_MMAP_DATA_VADDR);
         searchPages = VA_2_VPN(VMK_USER_LAST_MMAP_DATA_VADDR) - startVPN + 1;
      }
         

      ASSERT(!overwrite);

      /* XXX look through the mmInfo list */

      status = VMK_NO_RESOURCES;
      USERMEM_FOR_RANGE_DO(mem, startVPN, searchPages, pte, i) {
         if (pte == NULL) {
            status = VMK_NO_MEMORY;
            USERMEM_FOR_RANGE_BREAK;
         }
         if ((!UserPTE_IsMapped(pte)) && (!UserPTE_IsInUse(pte))) {
            if (freeVPN == INVALID_VPN) {
               freeVPN = startVPN+i;
            }
            ASSERT (startVPN+i-freeVPN+1 <= nPages);
            if (startVPN+i-freeVPN+1 == nPages) {
               status = VMK_OK;
               USERMEM_FOR_RANGE_BREAK;
            }
         } else {
            freeVPN = INVALID_VPN;
         }
      } USERMEM_FOR_RANGE_END;
      if (status == VMK_OK) {
         // we found a big enough hole, so mark it in use
         status = UserMemSetPTEInUseRange(uci, freeVPN, nPages, prot, mmInfo,
					  FALSE);
         *addr = VPN_2_VA(freeVPN);
      }
   }
   
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * UserMemMapCreate --
 *
 *	Create new mmap region: allocate virtual addresses for given mmap
 *	request and store the mmap info in the cartel's mmaps list.  If
 *	overwrite is TRUE, it will overwrite a current mapping.
 *
 * Results:
 *	VMK_OK on success, 
 *      VMK_EXISTS if range already allocated
 *      VMK_NO_RESOURCES if no usermem heap available
 *      or other status
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserMemMapCreate(World_Handle *world,
                 UserVA *addr,          // INOUT 
		 Bool overwrite,
                 uint32 length,
		 uint32 prot,
                 UserMemMapType type,
                 Bool pinned,
                 uint32 reservedPages,
                 UserObj *obj,
                 uint64 pgoff,
                 UserMemMapExecFlag execFlag,
                 UserMemMapInfo **outMMInfo)
{
   VMK_ReturnStatus status;
   UserMemMapInfo *mmInfo;
   User_CartelInfo *uci = world->userCartelInfo;
   UserMem *mem = &uci->mem;

   ASSERT(UserMemIsLocked(mem));

   UWLOG(1, "addr=0x%x overwrite=%d len=0x%x type=%d", *addr, overwrite, length, type);
   mmInfo = (UserMemMapInfo*)User_HeapAlloc(uci, sizeof *mmInfo);
   if (mmInfo == NULL) {
      return VMK_NO_RESOURCES;
   }

   List_InitElement(&mmInfo->links);
   // initialize start address and length to zero
   mmInfo->startAddr = 0;
   mmInfo->length = 0;
   mmInfo->type = type;
   mmInfo->obj = obj;
   mmInfo->pgoff = pgoff;
   mmInfo->refCount = 0;
   mmInfo->prot = prot;
   mmInfo->pinned = pinned;
   mmInfo->reservedPages = reservedPages;

   // set start address and length
   status = UserMemMapInfoSetRange(world, mmInfo, *addr, length);

   if (status == VMK_OK) {
      status = UserMemMapAllocRange(uci, addr, overwrite, length, prot, mmInfo,
                                    execFlag);

      /*
       * If overwrite is set, we shouldn't fail if something was already mapped at
       * the addr requested.
       */
      ASSERT(!overwrite || status != VMK_EXISTS);
      if (status == VMK_OK) {
         if (mmInfo->startAddr == 0) {
            ASSERT(PAGE_OFFSET(*addr) == 0);
            // fix start address
            mmInfo->startAddr = *addr;
         }
         UserMemMapInfoInsert(mem, mmInfo);

         // Return to caller if they care to see it
         if (outMMInfo) {
            *outMMInfo = mmInfo;
         }
      }
   }

   if (status != VMK_OK) {
      User_HeapFree(uci, mmInfo);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserMemMapInfoSplit --
 *
 *      Split given mmInfo at splitAddr. Given splitAddr must be
 *      page-aligned.  A new mmInfo is created to cover from splitAddr up,
 *      while given mmInfo is shrunk to cover up to splitAddr.
 *
 * Results:
 *	VMK_OK if split succeeded, otherwise on failure.  *outMMInfo will
 *	point to the new mmInfo, if not NULL.
 *
 * Side effects:
 *	MemMapInfo created and inserted in mminfo list.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserMemMapInfoSplit(World_Handle *world,
                    UserMemMapInfo* mmInfo,
                    UserVA splitAddr,
                    UserMemMapInfo** outMMInfo)
{
   /* | --- left ---- <splitAddr> --- right --- | */
   VMK_ReturnStatus status;
   const uint32 leftLen = splitAddr - mmInfo->startAddr;
   const uint32 rightLen = mmInfo->startAddr + mmInfo->length - splitAddr;
   uint64 newPgoff;
   const uint32 oldLen = mmInfo->length;
   
   ASSERT(splitAddr > mmInfo->startAddr);
   ASSERT(splitAddr < mmInfo->startAddr + mmInfo->length);
   ASSERT(PAGE_OFFSET(splitAddr) == 0);
   ASSERT(UserMemIsLocked(&world->userCartelInfo->mem));
   
   newPgoff = 0;
   if (mmInfo->type == USERMEM_MAPTYPE_FD) {
      newPgoff = mmInfo->pgoff + BYTES_2_PAGES(leftLen);
   }

   UWLOG(1, "Splitting mminfo %p: {%#x, %#x} at %#x (+%#x)",
         mmInfo, mmInfo->startAddr, mmInfo->length, splitAddr, rightLen);
   
   // Shrink original mapping.  Do this first to avoid double accounting
   status = UserMemMapInfoSetEnd(world, mmInfo, splitAddr);
   ASSERT(status == VMK_OK); // shrinking cannot fail

   status = UserMemMapCreate(world, &splitAddr, TRUE, rightLen,
                             mmInfo->prot, mmInfo->type, mmInfo->pinned, 0,
                             mmInfo->obj, newPgoff, MEMMAP_IGNORE, outMMInfo);
   ASSERT(status != VMK_EXISTS); // overwrite=TRUE prevents this

   if (status != VMK_OK) {
      UWLOG(0, "split of mmInfo{%#x, %#x} failed: %s",
            mmInfo->startAddr, mmInfo->length,
            UWLOG_ReturnStatusToString(status));

      // un-shrink mmInfo
      status = UserMemMapInfoSetLength(world, mmInfo, oldLen);
      ASSERT(status == VMK_OK);

      return status;
   }
   
   UWSTAT_INC(mmapSplitCount);

   UWLOG(2, "Successfully split: {%#x, %#x} -> {%#x, %#x} and {%#x, %#x}",
         mmInfo->startAddr, mmInfo->length,
         mmInfo->startAddr, leftLen,
         splitAddr, rightLen);

   /*
    * If we have a file-backed mapping, don't forget to up the refcount.
    */
   if (mmInfo->type == USERMEM_MAPTYPE_FD) {
      ASSERT(mmInfo->obj != NULL);
      UserObj_Acquire(mmInfo->obj);
   }

   ASSERT(status == VMK_OK);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserMemMapDestroyMMInfo --
 *
 *	Destroy given region specified by addr and length within the
 *	given mmInfo.  Free the virtual and machine pages for this region
 *	and mark the mmInfo struct to be cleaned up, freed, and removed
 *	from the cartel's mmaps list.  Also, return the list of MPNs to
 *	be freed (caller is responsible for flushing TLB and freeing
 *	the pages).
 *
 * Results:
 *	VMK_OK if successful, or VMK_ReturnStatus.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserMemMapDestroyMMInfo(World_Handle *world, 
                        UserMemMapInfo* mmInfo,
                        UserVA addr,
                        uint32 length,
			Bool* freeMe,
                        UserMemPTEList *pteListPtr)
{
   User_CartelInfo* uci = world->userCartelInfo;
   uint32 lengthPA;
   uint32 mmLengthPA;
   VPN startVPN = VA_2_VPN(addr);
   uint32 nPages = CEIL(length, PAGE_SIZE);
   Bool exactMatch;

   *freeMe = FALSE;

   // addr must be page aligned or the aligned lengths are not
   // particularly useful in comparisons.
   ASSERT(PAGE_OFFSET(addr) == 0);

   // Given addr+length must be a subset of mmInfo
   ASSERT(addr >= mmInfo->startAddr);
   ASSERT(length <= mmInfo->length);
   
   /*
    * Although mmap works at the granularity of pages, an mmap'd region's
    * length is specified in bytes.  Of course, the mmap'ed region
    * actually extends to the next page boundary.  So to simplify
    * calculations, we just round up these lengths to the next page
    * boundaries.
    */
   lengthPA = ALIGN_UP(length, PAGE_SIZE);
   mmLengthPA = ALIGN_UP(mmInfo->length, PAGE_SIZE);

   /*
    * Do some basic checks on this change first.  Note that the
    * 'exactMatch'-ness of this destroy will not change after the split.
    */
   exactMatch = (addr == mmInfo->startAddr && lengthPA == mmLengthPA);
   if (exactMatch) {
      /*
       * Don't let them delete the whole thing if the refCount isn't 0.
       *
       * XXX should be able to force this during CartelShutdown, if
       * necessary.
       */
      if (mmInfo->refCount != 0) {
	 UWWarn("mmap refcount (%d) not zero", mmInfo->refCount);
	 return VMK_BUSY;
      }
   } else {
      /*
       * Partial unmapping.  Since we only support partial unmaps on
       * anonymous and file-backed regions, we need to check that first.
       */
      if (mmInfo->type != USERMEM_MAPTYPE_ANON &&
          mmInfo->type != USERMEM_MAPTYPE_FD) {
	 UWWarn("Trying to partially unmap a region that's not anonymous or file-backed!");
	 DEBUG_ONLY(ASSERT(FALSE));
	 return VMK_BAD_PARAM;
      }
   }

   /*
    * Split the mmInfo into two pieces if the region being destroyed
    * doesn't touch either end of the mmInfo.  This will give us a single
    * mmInfo that can then be resized by just reducing its length.
    */
   if ((addr > mmInfo->startAddr)
       && (addr + lengthPA < mmInfo->startAddr + mmLengthPA)) {
      VMK_ReturnStatus status;

      ASSERT(!exactMatch);  // won't become one after the split, either.

      /*
       * Split at the end of the deleted area, so mmInfo will have to be
       * sized down, and we can just ignore the new mmInfo.
       */
      status = UserMemMapInfoSplit(world, mmInfo, addr + lengthPA, NULL);
      if (status != VMK_OK) {
         UWLOG(0, "Implicit creation of new mapping failed: %s",
               UWLOG_ReturnStatusToString(status));
         return status;
      }

      // mmInfo probably changed, so update mmLengthPA:
      mmLengthPA = ALIGN_UP(mmInfo->length, PAGE_SIZE);
   }

   // deleted region must touch one end or the other of the mmInfo
   ASSERT((addr == mmInfo->startAddr)
          || (addr + lengthPA == mmInfo->startAddr + mmLengthPA));

   /*
    * Clear the PTEs in the destroyed area.
    */
   {
      VMK_ReturnStatus status;
      
      if (mmInfo->type == USERMEM_MAPTYPE_PHYSMEM ||
          mmInfo->type == USERMEM_MAPTYPE_MEMTEST ||
          mmInfo->type == USERMEM_MAPTYPE_KTEXT ||
          mmInfo->type == USERMEM_MAPTYPE_TDATA) {
         // physmem/memtest/ktext/tdata pages are freed seperately
         status = UserMemMapClearRange(uci, mmInfo->pinned, startVPN, nPages, NULL);
      } else {
         status = UserMemMapClearRange(uci, mmInfo->pinned, startVPN, nPages, pteListPtr);
      }
      
      if (status != VMK_OK) {
         UWLOG(0, "ClearRange(startVPN=%#x, nPages=%d): %s",
               startVPN, nPages, VMK_ReturnStatusToString(status));
         // XXX undo split mmInfos
         return status;
      }
   }

   /*
    * Now that PTEs are cleared, shrink the mmInfo.  This step cannot
    * fail, so we won't have to undo the *ClearRange.  We do this after
    * *ClearRange because this is hard to undo if there is a failure there.
    */
   if (exactMatch) {
      *freeMe = TRUE;
   } else if (addr == mmInfo->startAddr) {
      VMK_ReturnStatus status;
      /*
       * The front part of the mapping is gone.  Push startAddr up.
       */
      status = UserMemMapInfoSetStart(world, mmInfo, addr + lengthPA);
      ASSERT(status == VMK_OK); // Cannot fail on shrink
   } else if ((addr + lengthPA) == (mmInfo->startAddr + mmLengthPA)) {
      VMK_ReturnStatus status;
      /*
       * The back part of the mapping is gone.  Shorten length.
       */
      status = UserMemMapInfoSetEnd(world, mmInfo, addr);
      ASSERT(status == VMK_OK); // Cannot fail on shrink
   } else {
      ASSERT(FALSE); // cannot happen
   }

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * UserMemMapDestroyRegion --
 *
 *	Destroy given mmap'ed region.  Addr must be page-aligned.  Length
 *	must not be zero.
 *
 * Results:
 *	VMK_OK if successful, or VMK_ReturnStatus.
 *
 * Side effects:
 *	mmInfos and MPNs are freed and TLB is flushed.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserMemMapDestroyRegion(World_Handle *world, UserVA addr, uint32 length)
{
   User_CartelInfo *uci = world->userCartelInfo;
   UserMemMapInfo *mmInfo = NULL;
   List_Links *item;
   VMK_ReturnStatus status = VMK_OK;
   UserMem* mem = &uci->mem;
   UserVA endAddr = addr + length;
   UserVA mmEndAddr;
   UserVA addrToUnmap;
   uint32 lengthToUnmap;
   Bool freeMMInfo;
   uint32 ptesFreed;

   ASSERT(!UserMemIsLocked(mem));
   ASSERT(length != 0);
   ASSERT(PAGE_OFFSET(addr) == 0);

   do {
      UserMemPTEList pteList = NULL;
      List_Links mmInfosToFree;
      List_Init(&mmInfosToFree);

      UserMemLock(mem);

      for (item = List_First(&mem->mmaps); !List_IsAtEnd(&mem->mmaps, item); ) {
         mmInfo = (UserMemMapInfo *)item;
         mmEndAddr = mmInfo->startAddr + mmInfo->length;

         /*
          * Get a pointer to the next item immediately.
          */
         item = List_Next(item);

         /*
          * If this mmInfo ends at or before the region to be unmapped starts, we
          * know we can skip this mmInfo.
          */
         if (mmEndAddr <= addr) {
            continue;
         }

         /*
          * If the region to be unmapped ends before this mmInfo starts, we know
          * we're done.
          */
         if (endAddr <= mmInfo->startAddr) {
            break;
         }

         /*
          * Now we know this mmInfo is somehow affected by this unmap.  So figure
          * out how much of this mmInfo to unmap.
          */
         addrToUnmap = MAX(mmInfo->startAddr, addr);
         lengthToUnmap = MIN(mmEndAddr - addrToUnmap, endAddr - addrToUnmap);
         ASSERT(lengthToUnmap != 0);

         status = UserMemMapDestroyMMInfo(world, mmInfo, addrToUnmap, lengthToUnmap,
                                          &freeMMInfo, &pteList);
         if (status != VMK_OK) {
            UWLOG(0, "UserMemMapDestroyMMInfo failed: %s",
                  VMK_ReturnStatusToString(status));
	    break;
         }
     
         /*
          * If freeMMInfo is set, the region this mmInfo represented is completely
          * gone, so remove it from the mem->mmaps list and add it to the list of
          * mmInfos to be cleaned up and freed.
          *
          * Note, because of locking, we cannot clean mmInfo here. We call the
          * mmInfo cleanup function after releasing the usermem lock.
          * 
          */
         if (freeMMInfo) {
            List_Remove(&mmInfo->links);
	    List_Insert(&mmInfo->links, LIST_ATFRONT(&mmInfosToFree));
         }
      }

      UserMemUnlock(mem);

      // Now free PTEs and mmInfos.
      ptesFreed = UserMemFlushAndFreePages(world, pteList);
      UserMemCleanupAndFreeMMInfos(world, &mmInfosToFree);

      if (status == VMK_NO_MEMORY_RETRY) {
         ASSERT(pteList != NULL);
      }

      /*
       * Repeat as long as status equals to VMK_NO_MEMORY_RETRY 
       * and we have freed PTEs.
       */
   } while (status == VMK_NO_MEMORY_RETRY && ptesFreed > 0);

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * UserMem_Map --
 *
 *	Map length bytes of given file or anonymous memory into the
 *	current cartel's address space.
 *
 * Results:
 *	VMK_ReturnStatus
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserMem_Map(World_Handle *world,
            UserVA *addr,               // INOUT 
            uint32 length,
	    uint32 prot,
            uint32 flags,
            LinuxFd fd,
            uint64 pgoff)
{
   User_CartelInfo *uci = world->userCartelInfo;
   VMK_ReturnStatus status = VMK_OK;
   UserObj *obj = NULL;
   UserMemMapType type = (flags & LINUX_MMAP_ANONYMOUS) ? USERMEM_MAPTYPE_ANON :
      USERMEM_MAPTYPE_FD;

   UWLOG(1, "addr=%#x len=%#x flags=%#x fd=%d pgoff=%#Lx",
         *addr, length, flags, fd, pgoff);

   // if mmap'ing a file, check for file and get a refcount on the object
   if (type == USERMEM_MAPTYPE_FD) {
      status = UserObj_Find(uci, fd, &obj);
      if (status != VMK_OK) {
         UWLOG(0, "Failed because invalid file descriptor");
         return status;
      }
      ASSERT(obj != NULL);

      /*
       * Note that USEROBJ_TYPE_PROXY_FILE is a bit too permissive.  You'll
       * be able to mmap silly things like directories or whatnot.
       */
      if ((obj->type != USEROBJ_TYPE_FILE)
          && (obj->type != USEROBJ_TYPE_PROXY_FILE)
	  && (obj->type != USEROBJ_TYPE_PROXY_CHAR)) {
         UWLOG(0, "Failed because fd is not a file or proxy object");
         (void) UserObj_Release(uci, obj);
         return VMK_INVALID_HANDLE;
      }
   }

   /*
    * Make sure they're requesting a valid range.
    */
   if ((*addr != 0) && (*addr < VMK_USER_FIRST_MMAP_TEXT_VADDR)) {
      UWLOG(0, "Failed because requested address (%x) not in map range", *addr);
      status = VMK_BAD_PARAM;
   } else if ((*addr != 0) && (*addr >= VMK_USER_LAST_MMAP_DATA_VADDR)) {
      UWLOG(0, "Failed because requested address (%x) not in map range", *addr);
      status = VMK_BAD_PARAM;
   }

   if (status != VMK_OK) {
      if (type == USERMEM_MAPTYPE_FD) {
         (void) UserObj_Release(uci, obj);
      }
      return status;
   }

   status = UserMem_MapObj(world, addr, length, prot, flags, obj, pgoff, FALSE);
   if (status == VMK_OK) {
      // Must only return aligned addrs
      ASSERT(PAGE_OFFSET(*addr) == 0);
   }
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserMemMapTryExtending --
 *
 *	Try to extend the given mmInfo to incorporate the given
 *	addr/length/prot.  
 *
 *	strictAlign: if true, require that region end on a page-aligned
 *	boundary.  This is required if we're handing back a new start addr
 *	(i.e., for gluing an new mmap region onto an exisiting one).
 *	However, if we're extending a existing region (e.g., the heap),
 *	then we can extend an unaligned region.
 *
 * Results:
 *	VMK_OK: if extended successfully
 *      VMK_NOT_FOUND: if not possible to extend
 *      VMK_NO_MEMORY (or others): if some error happened during extension
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserMemMapTryExtending(World_Handle *world,
                       UserMemMapInfo* mmInfo,
                       UserVA* addr,
                       uint32 length,
                       uint32 prot,
                       Bool pinned,
                       Bool strictAlign)
{
   User_CartelInfo* uci = world->userCartelInfo;
   const UserMemMapInfo* nextMMInfo = (UserMemMapInfo*)(mmInfo->links.nextPtr);
   VMK_ReturnStatus status = VMK_NOT_FOUND;
   UserVA maxAddr = VMK_USER_LAST_MMAP_DATA_VADDR;
      
   // assume mmInfo list is sorted
   if (nextMMInfo != NULL) {
      maxAddr = nextMMInfo->startAddr;
   }

   UWLOG(3, "Trying mmInfo=%p {type=%d, addr=%#x prot=%#x, length=%#x %spinned}, max=%#x",
         mmInfo, mmInfo->type, mmInfo->startAddr, mmInfo->prot, mmInfo->length,
         (mmInfo->pinned ? "" : "!"), maxAddr);

   /* Must be anonymous, with matching prots/pinned and no funny business */
   if ((mmInfo->type == USERMEM_MAPTYPE_ANON)
       && (mmInfo->prot == prot)
       && (mmInfo->pinned == pinned)
       && ((!strictAlign) || PAGE_OFFSET(mmInfo->length) == 0)
       && (mmInfo->reservedPages == 0)) {
      const UserVA oldEnd = mmInfo->startAddr + mmInfo->length;
      
      /* XXX only try to grow up (no changing start down) */
      if ((oldEnd + length > oldEnd) // watch out for overflow
          && (oldEnd + length) <= maxAddr) {
         VPN oldEndVPN = VA_2_VPN(oldEnd - 1);
         VPN newEndVPN = VA_2_VPN(oldEnd + length - 1);

         ASSERT(newEndVPN >= oldEndVPN);
         ASSERT(*addr == 0 || *addr == oldEnd);
         *addr = oldEnd;

         status = UserMemMapInfoSetEnd(world, mmInfo, oldEnd + length);

         if (status == VMK_OK && oldEndVPN < newEndVPN) {
            status = UserMemSetPTEInUseRange(uci, oldEndVPN+1,
                                             newEndVPN - oldEndVPN,
                                             prot, mmInfo, FALSE);
            ASSERT(status == VMK_OK);
         }

         if (status == VMK_OK) {
            UWLOG(2, "Found mmInfo=%p {newlength=%#x, +%d pages}, *addr=%#x",
                  mmInfo, mmInfo->length, newEndVPN - oldEndVPN, *addr);
         }
      }
   }
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserMemMapExtendExisting --
 *
 *	Search high and low for an mmInfo to extend with the given
 *	allocation addr/length/prot.  Assumes only ANON mappings will be
 *	passed in.
 *
 * Results:
 *	VMK_OK: if extended an existing one
 *      VMK_NOT_FOUND: if nothing found
 *      VMK_NO_MEMORY (or others): if some error happened
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserMemMapExtendExisting(World_Handle *world,
                         UserVA *addr,
                         uint32 length,
                         uint32 prot,
                         Bool pinned,
                         UserMemMapExecFlag execFlag)
{
   User_CartelInfo* uci = world->userCartelInfo;
   UserMem* mem = &uci->mem;
   List_Links* itemPtr;
   VMK_ReturnStatus status = VMK_NOT_FOUND;
   UserVA minAddr;
   UserVA maxAddr;

   ASSERT(UserMemIsLocked(mem));

   UWLOG(4, "addr=%#x length=%d", *addr, length);

   return VMK_NOT_FOUND;

   /*
    * Support the execFlag by disallowing mmInfos in a particular region.
    */
   switch (execFlag) {
   case MEMMAP_EXEC:
      minAddr = VMK_USER_FIRST_MMAP_TEXT_VADDR;
      maxAddr = VMK_USER_LAST_MMAP_TEXT_VADDR;
      break;
   case MEMMAP_NOEXEC:
      minAddr = VMK_USER_FIRST_MMAP_DATA_VADDR;
      maxAddr = VMK_USER_LAST_MMAP_DATA_VADDR;
      break;
   default:
   case MEMMAP_IGNORE:
      minAddr = VMK_USER_FIRST_MMAP_TEXT_VADDR;
      maxAddr = VMK_USER_LAST_MMAP_DATA_VADDR;
      break;
   }
   ASSERT(minAddr < maxAddr);


   /* XXX only for ANON mappings */
   if (*addr == 0) {
      LIST_FORALL(&mem->mmaps, itemPtr) {
         UserMemMapInfo* mmInfo = (UserMemMapInfo*)itemPtr;

         if (mmInfo->startAddr < minAddr) {
            continue;
         }

         if (mmInfo->startAddr + mmInfo->length + length > maxAddr) {
            status = VMK_NOT_FOUND;
            break;
         }

         status = UserMemMapTryExtending(world, mmInfo, addr, length,
                                         prot, pinned, TRUE);
         if (status != VMK_NOT_FOUND) {
            UWLOG(2, "for MAP_ANY: %s",
                  VMK_ReturnStatusToString(status));
            ASSERT(*addr != 0);
            break;
         }
      }
   } else {
      if ((*addr > minAddr)
          && (*addr + length < maxAddr)) {
         // for a particular address
         LIST_FORALL(&mem->mmaps, itemPtr) {
            UserMemMapInfo* mmInfo = (UserMemMapInfo*)itemPtr;
            
            if (mmInfo->startAddr + mmInfo->length == *addr) {
               status = UserMemMapTryExtending(world, mmInfo, addr, length,
                                               prot, pinned, TRUE);
               UWLOG(2, "for specific addr (%#x): %s",
                     *addr, VMK_ReturnStatusToString(status));
               break;
            }
            
            if (mmInfo->startAddr > *addr) {
               status = VMK_NOT_FOUND;
               break;
            }
         }
      }
   }

   if (status == VMK_NOT_FOUND) {
      UWLOG(4, "addr=%#x length=%d: not found", *addr, length);
   }
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserMem_MapObj --
 *
 *	Map length bytes of given file or anonymous memory into the current
 *	cartel's address space.  This function should be used by the vmkernel
 *	when it needs to mmap something in that's outside of the mmap region or
 *	when it only has a UserObj, but not a LinuxFD.
 *
 * Results:
 *	VMK_ReturnStatus
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserMem_MapObj(World_Handle *world,
	       UserVA *addr,            // INOUT 
               uint32 length,
	       uint32 linuxProt,
               uint32 flags,
               UserObj* obj,
               uint64 pgoff,
	       Bool incRefcount)
{
   const Bool forced = (flags & LINUX_MMAP_FIXED) != 0;
   VMK_ReturnStatus status = VMK_OK;
   User_CartelInfo *uci = world->userCartelInfo;
   UserMem *mem = &uci->mem;
   UserMemMapType type = (flags & LINUX_MMAP_ANONYMOUS) ? USERMEM_MAPTYPE_ANON :
      USERMEM_MAPTYPE_FD;
   Bool pinned = (flags & LINUX_MMAP_LOCKED) ? TRUE : FALSE;
   uint32 numReservedPages = 0;
   uint32 prot;
   UserMemMapExecFlag execFlag = MEMMAP_IGNORE;

   UWLOG(1, "addr=%#x len=%#x flags=%#x obj=%p pgoff=%#Lx %spinned",
         *addr, length, flags, obj, pgoff, (pinned ? "" : "!"));

   if (incRefcount && type == USERMEM_MAPTYPE_FD) {
      ASSERT(obj != NULL);
      UserObj_Acquire(obj);
   }

   if (PAGE_OFFSET(*addr) != 0) {
      UWLOG(0, "Failed because addr not page aligned %x", *addr);
      status = VMK_BAD_PARAM;
   } else if (length == 0) {
      UWLOG(0, "Failed because length is 0");
      status = VMK_BAD_PARAM;
   } else if ((*addr == 0) && forced) {
      // Linux allows fixed mmap at 0, but it would break User_CopyIn
      // if we allowed that.
      UWLOG(0, "Failed because addr is 0, and flags include FIXED.");
      status = VMK_BAD_PARAM;
   } else if (linuxProt & ~(LINUX_MMAP_PROT_ALL)) {
      UWLOG(0, "Invalid protection flags: %#x", linuxProt);
      status = VMK_BAD_PARAM;
   } else if (linuxProt == LINUX_MMAP_PROT_WRITE) {
      UWLOG(0, "Can't mmap a file with write only permission");
      status = VMK_NO_ACCESS;
   } else if (obj != NULL && obj->openFlags & USEROBJ_OPEN_WRONLY) {
      UWLOG(0, "Can't mmap a file opened with O_WRONLY");
      status = VMK_NO_ACCESS;
   }

   /*
    * Check memory reservation limit if the mmap region is locked.
    */
   if (status == VMK_OK && pinned) {
      numReservedPages = length / PAGE_SIZE;
      UserMemLock(mem);
      /*
       * Verify reserved memory limit if it has been initialized.
       *
       * This can happen when some client (such as SharedArea_LayoutPowerOn())
       * does mmap before VMM admission control is invoked.
       */
      if (!MemSched_AdmitUserOverhead(world, numReservedPages)) {
         UWWarn("VMX reserved memory exceeded: required %d",
                mem->curReserved + numReservedPages);
         status = VMK_LIMIT_EXCEEDED;
      } else {
         mem->curReserved += numReservedPages;
      }
      UserMemUnlock(mem);
   }

   if (status == VMK_OK) {
      /*
       * Convert the protections from Linux to UserMem format.
       */
      prot = 0;
      if (linuxProt & LINUX_MMAP_PROT_READ) {
	 prot |= PTE_P;
      }
      if (linuxProt & LINUX_MMAP_PROT_WRITE) {
	 prot |= PTE_RW;
      }
      /*
       * Check if the requested portion of memory needs to be executable
       */
      if (linuxProt & LINUX_MMAP_PROT_EXEC) {
         /*
          * This flag will be checked in userMemMapAllocRange to see if the
          * mmap region should be in the code or data segment.
          */
         execFlag = MEMMAP_EXEC;
      } else {
         execFlag = MEMMAP_NOEXEC;
      }

      if (forced) {
         do {
            status = UserMemMapDestroyRegion(world, *addr, length);

            if (status == VMK_OK) {
               UserMemLock(mem);
               status = UserMemMapCreate(world, addr, FALSE, length, prot, type, pinned,
                                         numReservedPages, obj, pgoff, MEMMAP_IGNORE,
                                         NULL);
               UserMemUnlock(mem);
            }
            /*
             * It's possible that multiple threads trying to map the 
             * same region. So we try to map the region again if it failed.
             *
             * We assert that this shouldn't happen becaused forced mmap 
             * is only used during initialization.
             */
            ASSERT(status != VMK_EXISTS);
         } while(status == VMK_EXISTS);
      } else {
         Bool extended;
         UserMemLock(mem);

         /*
          * Special-case extension of an mmap region because implementing
          * general coalescing of mmap regions is currently too
          * compilcated.  Just for anonymous regions (pinned or unpinned),
          * too.
          * 
          * See if this mmap can just extend a pre-existing region.
          */
         extended = FALSE;
         if (type == USERMEM_MAPTYPE_ANON) {
            status = UserMemMapExtendExisting(world, addr, length, prot, pinned, execFlag);
            if (status == VMK_OK) {
               UWSTAT_INC(mmapExtendHitCount);
               ASSERT(*addr != 0);
               extended = TRUE;
            } else if (status == VMK_NOT_FOUND) {
               UWSTAT_INC(mmapExtendMissCount);
               extended = FALSE;
            } else {
               /*
                * May be VMK_NO_RESOURCES, etc
                */
               extended = TRUE; // lie, and bail
            }
         }

         /*
          * If hacks for finding and extending a pre-existing region fail,
          * try to create a new region.
          */
         if (!extended) {
            status = UserMemMapCreate(world, addr, FALSE, length, prot, type, pinned,
                                      numReservedPages, obj, pgoff, execFlag, NULL);
            if (status == VMK_EXISTS) {
               // if failed to allocate at given address range hint, try any address
               *addr = 0;
               status = UserMemMapCreate(world, addr, FALSE, length, prot, type, pinned,
                                         numReservedPages, obj, pgoff, execFlag, NULL);
            }
         }
         UserMemUnlock(mem);
      }

      if (status != VMK_OK && numReservedPages > 0) {
         UserMemLock(mem);
         mem->curReserved += numReservedPages;
         UserMemUnlock(mem);
      }
   }

   /*
    * We only want to release the obj here if we failed.  If we succeeded, then
    * a mmInfo will be holding a pointer to the obj, and thus we don't want to
    * drop the refcount.
    */
   if ((status != VMK_OK) &&
       (type == USERMEM_MAPTYPE_FD)) {
      ASSERT(obj != NULL);
      (void) UserObj_Release(uci, obj);
   }

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * UserMem_Unmap --
 *
 *	Unmap a region previously mapped with UserMem_Map
 *
 * Results:
 *	VMK_ReturnStatus
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserMem_Unmap(World_Handle *world,
              UserVA addr,
              uint32 length)
{
   VMK_ReturnStatus status;

   if (length == 0) {
      UWLOG(0, "zero length.  No unmap.");
      return VMK_OK;
   }

   if (PAGE_OFFSET(addr) != 0) {
      UWLOG(0, "addr %#x is not page-aligned.  Cannot unmap.", addr);
      return VMK_BAD_PARAM;
   }

   status = UserMemMapDestroyRegion(world, addr, length);

   UWLOG(1, "addr=%#x len=%#x status=%#x", addr, length, status);

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * UserMem_InitAddrSpace --
 *
 *	Sets up the stack for the initial world in the cartel.
 *
 * Results:
 *	VMK_OK on success, appropriate error code otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserMem_InitAddrSpace(World_Handle *world, UserVA* userStackEnd)
{
   VMK_ReturnStatus status;
   UserVA start;

   /*
    * Map in the first thread's stack.
    */
   *userStackEnd = VMK_USER_LAST_VADDR + 1;
   start = VMK_USER_MIN_STACK_VADDR;
   ASSERT(PAGE_OFFSET(start) == 0);
   status = UserMem_MapObj(world, &start, *userStackEnd - start,
			   LINUX_MMAP_PROT_READ | LINUX_MMAP_PROT_WRITE,
                           LINUX_MMAP_PRIVATE | LINUX_MMAP_ANONYMOUS |
                           LINUX_MMAP_FIXED, NULL, 0, FALSE);
   if (status != VMK_OK) {
      return status;
   }

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * UserMemChangeProtection --
 *
 *	Sets the protection bits for the given mmInfo's pte's.
 *
 * Results:
 *	VMK_ReturnStatus
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserMemChangeProtection(UserMem* mem, UserMemMapInfo* mmInfo, UserVA startAddr,
			uint32 length, uint32 prot, Bool* needFlush)
{
   VMK_ReturnStatus status = VMK_OK;
   UserPTE *pte;
   VPN startVPN;
   uint32 nPages;
   int i;

   *needFlush = FALSE;
   startVPN = VA_2_VPN(startAddr);
   nPages = CEIL(length, PAGE_SIZE);

   ASSERT(UserMemIsLocked(mem));
   USERMEM_FOR_RANGE_DO(mem, startVPN, nPages, pte, i) {
      if (pte == NULL) {
         status = VMK_NO_MEMORY;
	 USERMEM_FOR_RANGE_BREAK;
      }

      if (UserPTE_IsMapped(pte)) {
         if (prot == 0) {
	    /*
	     * Note: If we hit this, it's likely that we've already set the no
	     * access permissions on pte's from other mmInfos.  While we should
	     * go back and clear them up, we don't support clearing all
	     * protections on mapped pte's, so we're ok.
	     */
	    UWLOG(0, "Can't protect a memory region with no permissions if a "
		     "page of that region is already faulted in!");
	    status = VMK_BUSY;
	    USERMEM_FOR_RANGE_BREAK;
	 } else {
	    /*
	     * This pte must have at least read access.
	     */
	    ASSERT(prot & PTE_P);

	    if (prot & PTE_RW) {
               *needFlush |= UserPTE_EnableWrite(pte);
	    } else {
               *needFlush |= UserPTE_DisableWrite(pte);
	    }
	    status = VMK_OK;
         }
      } else {
         // change prot
         void* mmInfo = UserPTE_GetPtr(pte);
	 UserPTE_SetInUse(pte, prot, mmInfo);
      }
   } USERMEM_FOR_RANGE_END;

   if (status == VMK_OK) {
         // change prot field in the mmInfo structure as well
         mmInfo->prot = prot;
   }
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * UserMemProtectRange --
 *
 *	Finds all affected mmap regions and calls UserMemChangeProtection
 *	for each region.
 *
 * Results:
 *	VMK_ReturnStatus
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserMemProtectRange(UserMem *mem, UserVA addr, uint32 length, uint32 prot,
		    Bool verifyOnly, Bool* needFlush)
{
   VMK_ReturnStatus status = VMK_OK;
   List_Links *item;
   UserMemMapInfo* mmInfo;
   UserVA endAddr = addr + length;
   UserVA mmEndAddr;
   UserVA addrToChange;
   uint32 lengthToChange;

   *needFlush = FALSE;

   LIST_FORALL(&mem->mmaps, item) {
      mmInfo = (UserMemMapInfo *)item;
      mmEndAddr = mmInfo->startAddr + mmInfo->length;

      /*
       * If this mmInfo ends at or before the region to be changed starts, we
       * know we can skip this mmInfo.
       */
      if (mmEndAddr <= addr) {
         continue;
      }

      /*
       * If the region to be changed ends before this mmInfo starts, we know
       * we're done.
       */
      if (endAddr <= mmInfo->startAddr) {
         break;
      }

      /*
       * Now we know this mmInfo is somehow affected by this change.  So figure
       * out how much of this mmInfo to change.
       */
      addrToChange = MAX(mmInfo->startAddr, addr);
      lengthToChange = MIN(mmEndAddr - addrToChange, endAddr - addrToChange);
      ASSERT(lengthToChange != 0);

      if (verifyOnly) {
         /*
	  * Verify that this mmInfo will allow this type of access.
	  */
	 if (prot == 0 || (prot & PTE_P) != 0) {
	    /*
	     * Always allow no permissions or only read permission.
	     */
	    status = VMK_OK;
	 } else {
	    ASSERT(prot & PTE_RW);
	    /*
	     * Only allow write permission if this is an anonymous mapping or if
	     * the file-backing is opened for read-write (write only won't work
	     * with mmap).
	     */
	    if (mmInfo->obj == NULL ||
	        mmInfo->obj->openFlags & USEROBJ_OPEN_RDWR) {
	       status = VMK_OK;
	    } else {
	       status = VMK_NO_ACCESS;
	    }
	 }
      } else {
         Bool tmpNeedFlush;

         /*
	  * Make the change.
	  */
	 status = UserMemChangeProtection(mem, mmInfo, addrToChange,
					  lengthToChange, prot, &tmpNeedFlush);
	 if (tmpNeedFlush) {
	    *needFlush = TRUE;
	 }
      }
      if (status != VMK_OK) {
	 break;
      }
   }

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * UserMem_Protect --
 *
 *	Sets the protection bits for the given addr range.
 *
 * Results:
 *	VMK_ReturnStatus
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserMem_Protect(World_Handle *world, UserVA addr, uint32 length, uint32 linuxProt)
{
   VMK_ReturnStatus status;
   User_CartelInfo *uci = world->userCartelInfo;
   UserMem *mem = &uci->mem;
   Bool needFlush;
   uint32 prot;

   UWLOG(1, "addr=%#x len=%#x linuxProt=%#x", addr, length, linuxProt);

   /*
    * Make sure address is page aligned.
    */
   if (addr & (PAGE_SIZE - 1)) {
      return VMK_BAD_PARAM;
   }
   
   /*
    * Make sure addr + length doesn't wrap around.
    */
   if (addr + length < addr) {
      return VMK_BAD_PARAM;
   }

   /*
    * Make sure addr is within a valid range.
    */
   if (addr < VMK_USER_FIRST_TEXT_VADDR || addr > VMK_USER_LAST_VADDR) {
      return VMK_BAD_PARAM;
   }

   /*
    * Make sure addr + length is valid.
    */
   if (addr + length > VMK_USER_LAST_VADDR) {
      return VMK_BAD_PARAM;
   }
   
   /*
    * Make sure they passed in a valid set of protections.
    */
   if (linuxProt & ~(LINUX_MMAP_PROT_ALL)) {
      return VMK_BAD_PARAM;
   }

   /*
    * They can't set just write permission.
    */
   if (linuxProt == LINUX_MMAP_PROT_WRITE) {
      return VMK_BAD_PARAM;
   }

   /*
    * If length is 0, just return 0.
    */
   if (length == 0) {
      return VMK_OK;
   }
 
   /*
    * Convert the protections from Linux to UserMem format.
    */
   prot = 0;
   if (linuxProt & LINUX_MMAP_PROT_READ) {
      prot |= PTE_P;
   }
   if (linuxProt & LINUX_MMAP_PROT_WRITE) {
      prot |= PTE_RW;
   }

   UserMemLock(mem);
   status = UserMemProtectRange(mem, addr, length, prot, TRUE, &needFlush);
   if (status == VMK_OK) {
      status = UserMemProtectRange(mem, addr, length, prot, FALSE, &needFlush);
   }
   UserMemUnlock(mem);
   if (needFlush) {
      UserMemCartelFlush(uci);
   }

   UWLOG(1, "addr=%#x len=%#x status=%#x", addr, length, status);

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * UserMemMapFilePage --
 *
 *	Allocate a mpn and map it to the content from the file.
 *
 * Results:
 *	VMK_OK if the page is allocated.  Something else otherwise.
 *
 * Side effects:
 *	Panics.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserMemMapFilePage(World_Handle *world, VA va, UserMemMapInfo *mmInfo, MPN *mpn)
{
   VMK_ReturnStatus status;
   VPN vpn = VA_2_VPN(va);
   uint32 bytesRead;

   UWLOG(3, "userMem=%p va=%#x start=%#x len=%#x",
         &world->userCartelInfo->mem, va, mmInfo->startAddr, mmInfo->length);
   
   status = UserMemAllocPage(world, mpn);
   if (status != VMK_OK) {
      UWLOG(0, "Failed to alloc page: %s", UWLOG_ReturnStatusToString(status));
      return status;
   }

   ASSERT(mmInfo->obj != NULL);
   ASSERT(mmInfo->startAddr % PAGE_SIZE == 0);
   status = UserObj_ReadMPN(mmInfo->obj, *mpn, VPN_2_VA(vpn) - mmInfo->startAddr +
			    VPN_2_VA(mmInfo->pgoff), &bytesRead);
   if (status != VMK_OK) {
      UWLOG(0, "ReadMPN failed: %s", UWLOG_ReturnStatusToString(status));
      UserMemFreePage(world, *mpn);
      return status;
   }

   if (bytesRead != PAGE_SIZE) {
      UWLOG(1, "ReadMPN (va=%#x) returned partial read (%d bytes)",
            va, bytesRead);

      uint8 *ptr;
      ASSERT(bytesRead < PAGE_SIZE);
      ptr = KVMap_MapMPN(*mpn, TLB_LOCALONLY);
      if (ptr == NULL) {
         UWLOG(0, "Failed to mapmpn");
         UserMemFreePage(world, *mpn);
         return VMK_NO_ADDRESS_SPACE;
      }
      memset(ptr+bytesRead, 0, PAGE_SIZE - bytesRead);
      KVMap_FreePages(ptr);
   }

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserMem_SetupPhysMemMap --
 *
 *      Map length bytes worth of guest physical memory into cartel.
 *
 * Results:
 *      VMK_OK upon success, otherwise upon failure.
 *
 * Side effects:
 *      Beginning address is copied to userOut
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserMem_SetupPhysMemMap(World_Handle *world,
                        PPN startPPN,
                        uint32 length,
                        UserVA userOut)
{
   VMK_ReturnStatus status;
   UserVA addr;
   User_CartelInfo *uci = world->userCartelInfo;
   UserMem *mem = &uci->mem;
   Bool freePhysMem = TRUE;

   status = Alloc_PhysMemMap(World_GetVmmLeaderID(world), startPPN, length);
   if (status != VMK_OK) {
      UWLOG(0, "failed physmem map status=%x", status);
      return status;
   }

   addr = 0; // any address is fine
   UserMemLock(mem);
   status = UserMemMapCreate(world, &addr, FALSE, length, 
                             PTE_P | PTE_RW, USERMEM_MAPTYPE_PHYSMEM, TRUE,
                             0, NULL, startPPN, MEMMAP_NOEXEC, NULL);
   UserMemUnlock(mem);
   if (status == VMK_OK) {
      freePhysMem = FALSE;
      status = User_CopyOut(userOut, &addr, sizeof addr);
      if (status != VMK_OK) {
         UWLOG(0, "failed copyout status=%x", status);
         status = UserMemMapDestroyRegion(world, addr, length);
      }
   }

   if (status != VMK_OK && freePhysMem) {
      VMK_ReturnStatus cleanupStatus;
      cleanupStatus = Alloc_PhysMemUnmap(World_GetVmmLeaderID(world), startPPN, length);
      ASSERT(cleanupStatus == VMK_OK);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserMem_MemTestMap --
 *
 *      Allocate a block of MPNs containing the input mpn (from mpnInOut).
 *      If MPNs are allocated successfully, allocate a mmap region to
 *      map it into address space.
 *
 * Results:
 *      VMK_OK upon success, otherwise upon failure.
 *
 *      On success, set addrOut to the start of a new mmap region, 
 *      set mpnInout to the starting MPN of the MPN block and 
 *      set numPageOut to the size of the MPN block.
 *
 *      On failure, set addrOut to NULL,
 *      set numPageOut the 0 and set mpnInOut to the next MPN to try.
 *
 * Side effects:
 *      Beginning address is copied to userOut
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserMem_MemTestMap(World_Handle *world,
                   UserVA mpnInOut,
                   UserVA numPagesOut,
                   UserVA addrOut)
{
   VMK_ReturnStatus copyStatus, status;
   User_CartelInfo *uci = world->userCartelInfo;
   UserMem *mem = &uci->mem;
   MPN startMPN;
   uint32 numPages = 0;
   UserVA addr = 0;

   // read startMPN from user param
   copyStatus = User_CopyIn(&startMPN, mpnInOut, sizeof(startMPN));
   if (copyStatus != VMK_OK) {
      return copyStatus; 
   }

   // set user param mmap address to 0 
   copyStatus = User_CopyOut(addrOut, &addr, sizeof(addr));
   if (copyStatus != VMK_OK) {
      return copyStatus; 
   }

   // allocate the memory range
   status = MemMap_AllocPageRange(world, &startMPN, &numPages);
   if (status != VMK_OK) {
      // write next startMPN to user param
      copyStatus = User_CopyOut(mpnInOut, &startMPN, sizeof(startMPN));
      return (copyStatus == VMK_OK ? status : copyStatus);
   }

   ASSERT(numPages > 0);
   UserMemLock(mem);
   status = UserMemMapCreate(world, &addr, FALSE, numPages * PAGE_SIZE,
                             PTE_P | PTE_RW | PTE_PCD, 
                             USERMEM_MAPTYPE_MEMTEST, TRUE,
                             0, NULL, startMPN, MEMMAP_NOEXEC, NULL);
   if (status != VMK_OK) {
      MemMap_FreePageRange(startMPN, numPages);
   } else {
      UserMemUsage(world)->pinned += numPages;
   }
   UserMemUnlock(mem);

   if (status == VMK_OK) {
      copyStatus = User_CopyOut(mpnInOut, &startMPN, sizeof(startMPN));
      if (copyStatus == VMK_OK) {
         copyStatus = User_CopyOut(numPagesOut, &numPages, sizeof(numPages));
      }
      if (copyStatus == VMK_OK) {
         copyStatus = User_CopyOut(addrOut, &addr, sizeof(addr));
      }
      if (copyStatus != VMK_OK) {
         status = UserMemMapDestroyRegion(world, addr, numPages * PAGE_SIZE);
         ASSERT(status == VMK_OK);
         return copyStatus;
      }
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserMemMapPhysMemPage --
 *
 *	Return mpn mapped to the guest physical page.
 *
 * Results:
 *	Something from alloc module.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserMemMapPhysMemPage(World_Handle *world, VA va, UserMemMapInfo *mmInfo, MPN *mpn)
{
   PPN ppn = VA_2_VPN(va - mmInfo->startAddr) + mmInfo->pgoff;
   return Alloc_UserWorldPhysPageFault(World_GetVmmLeaderID(world), ppn, mpn);
}


/*
 *----------------------------------------------------------------------
 *
 * UserMem_AddToKText --
 *
 *      Copy the given code into the ktext page and return its user
 *      address.  This routine does no locking and should be called
 *      only during cartel initialization.
 *
 * Results:
 *	VMK_OK or error.  
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserMem_AddToKText(UserMem* mem,       // IN/OUT
                   const void* code,   // IN: address of code to copy in
                   size_t size,        // IN: size of code to copy in
                   UserVA* uva)        // OUT: UserVA where code landed
{
   void* ktext;

   ASSERT(VMK_USER_MAX_KTEXT_PAGES == 1);
   ASSERT(mem->ktextMPN != 0);
   ASSERT(mem->ktextMPN != INVALID_MPN);

   // Size check
   if (mem->ktextOffset + size > VMK_USER_MAX_KTEXT_PAGES * PAGE_SIZE) {
      UWLOG(0, "size (%u) too big for remaining ktext (offset=%u)",
            size, mem->ktextOffset);
      return VMK_NO_ADDRESS_SPACE;
   }

   // Map in the ktext so we can write to it.
   ktext = KVMap_MapMPN(mem->ktextMPN, TLB_LOCALONLY);
   if (ktext == NULL) {
      UWLOG(0, "KVMap_MapMPN failed");
      return VMK_NO_RESOURCES;
   }

   // Copy in and return offset
   memcpy(ktext + mem->ktextOffset, code, size);
   *uva = VMK_USER_FIRST_KTEXT_VADDR + mem->ktextOffset;
   mem->ktextOffset += size;

   // Unmap ktext
   KVMap_FreePages(ktext);
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserMemEnableHdWrite --
 *
 *      Enable RW bit in the PTE of a writable page.
 *
 * Results:
 *	VMK_OK if write enabled. Something else otherwise.
 *      If a global TLB flush is required, *globalFlush = TRUE.
 *
 * Side effects:
 *	Panic.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserMemEnableHdWrite(World_Handle *world, UserPTE *pte, VA va, Bool *globalFlush)
{
   VMK_ReturnStatus status;
   *globalFlush = FALSE;

   ASSERT(UserMemIsLocked(&world->userCartelInfo->mem));
   ASSERT(UserPTE_IsPresent(pte));

   if (UserPTE_IsWritable(pte)) {
      uint32 pteFlags = UserPTE_GetFlags(pte);
      Bool pinned = UserPTE_IsPinned(pte);
      MPN mpn = UserPTE_GetMPN(pte);
      uint64 key = PShare_HashPage(mpn);
      MPN mpnCopy;
      uint32 count;
      
      if (UserPTE_IsPShared(pte)) {
         // Remove from PShare hashtable.
         status = PShare_Remove(key, mpn, &count);
         if (status != VMK_OK) {
            Panic("UserMem_HandlePShareFault: invalid shared mpn %#x\n", mpn);
         }

         // If more than one shared page left, create a copy.
         if (count > 0) {
            Bool ok;
            status = UserMemAllocPage(world, &mpnCopy);
            if (status != VMK_OK) {
               UWLOG(0, "Failed. Alloc failed: %s",
                     VMK_ReturnStatusToString(status));
               return status;
            }

            // Make private copy of the page.
            UWLOG(3, "copy mpn from %#x to %#x", mpn, mpnCopy);
            ok = Util_CopyMA(MPN_2_MA(mpnCopy), MPN_2_MA(mpn), PAGE_SIZE);
            ASSERT(ok);
 
            Atomic_Dec(&userMemStats.pageShared);
            mpn = mpnCopy;
            *globalFlush = TRUE;
         }
         UserMemUsage(world)->cow--;
         UserMemUsage(world)->pageable++;
      }
     
      // Update the page table. 
      UserPTE_Set(pte, mpn, pteFlags, pinned, FALSE);
      if (!*globalFlush) {
         TLB_INVALIDATE_PAGE(va);
      }
      status = VMK_OK;
   } else {
      // a true protection violation or page fault
      UWLOG(1, "protection violation: va %#x pte %"FMT64"x", va, pte->u.raw);
      status = VMK_NO_ACCESS;
   }

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * UserMem_HandleMapFault --
 *
 *	Handle a fault in the mmap region given la/va
 *
 * Results:
 *	VMK_OK if the fault was handled.  Something else otherwise.
 *
 * Side effects:
 *	Panics.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserMem_HandleMapFault(World_Handle *world, LA la, VA va, Bool isWrite)
{
   VMK_PTE *pageTable;
   VMK_ReturnStatus status;
   User_CartelInfo *uci = world->userCartelInfo;
   UserMem *mem = &world->userCartelInfo->mem;
   UserMemMapInfo *mmInfo;
   UserPTE *pte;
   Bool globalFlush = FALSE;

   UWLOG(3, "userMem=%p va=%#x", mem, va);
   
   ASSERT((va >= 0) && (va <= VMK_USER_LAST_VADDR));

   /*
    * Some asserts so you might have faith in the memory layout
    * implied by the following tests.
    */
   ASSERT(VMK_USER_FIRST_KTEXT_VADDR < VMK_USER_LAST_KTEXT_VADDR);
   ASSERT(VMK_USER_LAST_KTEXT_VADDR < VMK_USER_FIRST_TDATA_VADDR);
   ASSERT(VMK_USER_FIRST_TDATA_VADDR < VMK_USER_LAST_TDATA_VADDR);
   ASSERT(VMK_USER_LAST_TDATA_VADDR <= VMK_USER_LAST_TDATA_PT_VADDR);
   ASSERT(VMK_USER_LAST_TDATA_PT_VADDR < VMK_USER_FIRST_TEXT_VADDR);
   ASSERT(VMK_USER_FIRST_TEXT_VADDR < VMK_USER_FIRST_MMAP_TEXT_VADDR);
   ASSERT(VMK_USER_FIRST_MMAP_TEXT_VADDR < VMK_USER_FIRST_MMAP_DATA_VADDR);
   ASSERT(VMK_USER_FIRST_MMAP_DATA_VADDR < VMK_USER_MIN_STACK_VADDR);
   ASSERT(VMK_USER_MIN_STACK_VADDR < VMK_USER_LAST_VADDR);
   ASSERT(mem->dataEnd >= mem->dataStart);
   ASSERT(mem->dataEnd <= VMK_USER_FIRST_MMAP_TEXT_VADDR);

   /*
    * Notes on modifying the code:
    *
    * UserMem lock will be dropped in the middle of the function,
    * so we use a while loop to repeat the operation if we detect
    * the PTE has been modified.
    */
   UserMemLock(mem);

   // lookup PTE, this also fills in this world's pagetable from canonical
   status = UserMemLookupPageTable(mem, world->pageRootMA,
                                   la, &pageTable);
   if (status != VMK_OK) {
      UserMemUnlock(mem);
      return status;
   }
   ASSERT(pageTable != NULL);
   pte = UserPTE_For(pageTable, la);

   UWLOG(3, "va=%#x pte=%#Lx", va, pte->u.raw);

   while (1) {
      MPN mpn;
      Bool mpnAllocated;

      mmInfo = NULL;

      // Swap in the page if it has been swapped out or in swap process.
      if (UserPTE_InSwap(pte)) {
         status = UserMemSwapInPage(world, pte, LA_2_LPN(la));
         if (status != VMK_OK) {
            UWLOG(0, "failed to swap in page la %#x status %d", la, status);
            break;
         }
      }
      ASSERT(!UserPTE_InSwap(pte));

      // Check to see if the page is already present.
      if (UserPTE_IsPresent(pte)) {
         // The page is already present.
         if (isWrite && !UserPTE_HdWriteEnabled(pte)) {
            status = UserMemEnableHdWrite(world, pte, va, &globalFlush);
         } else {
            status = VMK_OK;
         }
         break;
      } else if (UserPTE_IsInUse(pte)) {
         uint32 prot = UserPTE_GetProt(pte);
         mmInfo = UserPTE_GetPtr(pte);
         ASSERT(mmInfo != NULL);
         // check for page protection
         if ((prot & PTE_P) == 0) {
            status = VMK_NO_ACCESS;
            break;
         }
         if (isWrite && ((prot & PTE_RW) == 0)) {
            status = VMK_NO_ACCESS;
            break;
         }
      } else {
         // The page is not valid.
         status = VMK_INVALID_ADDRESS;
         break;
      }

      /*
       * We've verified that the page needs to be paged in
       * with mmInfo containing the type of the page.
       */
      mmInfo->refCount++;
      UserMemUnlock(mem);

      /*
       * Get an mpn containing the content of the page.
       */
      switch (mmInfo->type) {
      case USERMEM_MAPTYPE_ANON:
         status = UserMemAllocPage(world, &mpn);
         if (status == VMK_OK) {
            status = Util_ZeroMPN(mpn);
            ASSERT(status == VMK_OK); /* XXX fix me */
         }
         mpnAllocated = TRUE;
         break;
      case USERMEM_MAPTYPE_FD:
         status = UserMemMapFilePage(world, va, mmInfo, &mpn);
         mpnAllocated = TRUE;
         break;
      case USERMEM_MAPTYPE_PHYSMEM:
         status = UserMemMapPhysMemPage(world, va, mmInfo, &mpn);
         mpnAllocated = FALSE;
         break;
      case USERMEM_MAPTYPE_MEMTEST:
         mpn = VA_2_VPN(va - mmInfo->startAddr) + mmInfo->pgoff;
         mpnAllocated = FALSE;
         break;
      case USERMEM_MAPTYPE_KTEXT:
      case USERMEM_MAPTYPE_TDATA:
      default:
         mpn = INVALID_MPN;
         mpnAllocated = FALSE;
         Panic("UserMem_HandleMapFault: unexpected type = %d at la=%x va=%x\n",
               mmInfo->type, la, va);
      }

      UserMemLock(mem);
      mmInfo->refCount--;

      if (UNLIKELY(status != VMK_OK)) {
         break;
      }

      /*
       * Insert the new page into the page table.
       */
      status = UserMemAddPageToTable(mem, pageTable, la, mmInfo->pinned, isWrite, mpn, mmInfo);
      if (UNLIKELY(status != VMK_OK)) {
         /*
          * This happens when the PTE was modified while we were not holding
          * the userMem lock.  Free the mpn and restart.
          */
         if (mpnAllocated) {
            UserMemFreePage(world, mpn);
         }
      } else {
         // Page-in finished.
         ASSERT(UserPTE_IsPresent(pte));
         if (mpnAllocated) {
            if (mmInfo->pinned) {
               ASSERT(UserPTE_IsPinned(pte));
               Atomic_Inc(&userMemStats.pagePinned); 
               UserMemUsage(world)->pinned++;
            } else {
               UserMemUsage(world)->pageable++;
            }
         }
         break;
      }
   }

   UserMemReleasePageTable(mem, pageTable);

   UserMemUnlock(mem);

   // Do a global TLB flush if required.
   if (globalFlush) {
      UserMemCartelFlush(uci);
   }

   if (status == VMK_OK) {
      if (mmInfo != NULL && mmInfo->type == USERMEM_MAPTYPE_FD) {
         UserMem_PSharePage(world, VA_2_VPN(va));
      }
   }

   UWLOG(3, "userMem=%p va=%#x: %s", mem, va, UWLOG_ReturnStatusToString(status));

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserMem_LookupMPN --
 *
 *	Converts a userland VPN to a MPN.
 *
 * Results:
 *	INVALID_MPN if there is no mapping for this va, the MPN otherwise.
 *
 * Side effects:
 *	If pinPage is TRUE, the page will be pinned.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserMem_LookupMPN(World_Handle *world, VPN vpn, UserPageType pageType, MPN *mpnOut)
{
   LA laddr;
   VMK_PTE *pageTable = NULL;                                              
   UserPTE *pte;
   UserMem *mem = &world->userCartelInfo->mem;
   VMK_ReturnStatus status = VMK_OK;

   ASSERT(pageType == USER_PAGE_NOT_PINNED || pageType == USER_PAGE_PINNED);
   ASSERT(vpn <= VMK_USER_LAST_VPN);
   *mpnOut = INVALID_MPN;

   UserMemLock(mem);

   laddr = LPN_2_LA(VMK_USER_VPN_2_LPN(vpn));

   pageTable = UserMemCanonicalPageTable(mem, laddr, NULL);
   if (pageTable == NULL) {
      UWWarn("Pagetable not found for laddr %#x.", laddr);
      status = VMK_NOT_FOUND;
   } else {
      pte = UserPTE_For(pageTable, laddr);

      if (UserPTE_InSwap(pte)) {
         VMK_ReturnStatus status;
         status = UserMemSwapInPage(world, pte, LA_2_LPN(laddr));
      }

      ASSERT(status == VMK_OK);
      if (status == VMK_OK) {
         *mpnOut = UserPTE_GetMPN(pte);
         if (*mpnOut != INVALID_MPN) {
            /*
             * If we need to pin the page and it has not been pinned,
             * check reservation and pin it.
             */
            if (pageType == USER_PAGE_PINNED && !UserPTE_IsPinned(pte)) {
               if (MemSched_AdmitUserOverhead(world, 1)) {
                  mem->curReserved++;
                  Atomic_Inc(&userMemStats.pagePinned); 
                  UserMemUsage(world)->pinned++;
                  UserMemUsage(world)->pageable--;
                  UserPTE_SetPinned(pte);
               } else {
                  UWWarn("VMX pinned page num %d exceeded reserved limit, vpn %#x.",
                         mem->curReserved, vpn);
                  *mpnOut = INVALID_MPN;
                  status = VMK_LIMIT_EXCEEDED;
               }
            }
         } else {
            status = VMK_NOT_FOUND;
         }
      }
   }
   UserMemReleasePageTable(mem, pageTable);

   UserMemUnlock(mem);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserMem_DumpMapTypes --
 *
 *	Dumps out the possible mmap types in string representation.
 *
 * Results:
 *	VMK_OK on success, appropriate failure status otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserMem_DumpMapTypes(UserDump_Header *header, UserDump_DumpData *dumpData)
{
   VMK_ReturnStatus status;
   int i, len;

   for (i = 0; i <= USERMEM_MAPTYPE_END; i++) {
      len = strlen(userMemMapTypes[i]) + 1;

      status = UserDump_Write(dumpData, (void*)userMemMapTypes[i], len);
      if (status != VMK_OK) {
         return status;
      }

      header->mapTypesSize += len;
   }

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserMemGetNextFdMap --
 *
 *	Searches through the list of mmap regions starting from mmapIdx
 *	regions into the list, looking for the first object-backed
 *	region.
 *
 * Results:
 *	A pointer to the region if found, or NULL if the list is
 *	exhausted.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static UserMemMapInfo*
UserMemGetNextFdMap(UserMem* mem, int *mmapIdx)
{
   UserMemMapInfo *mmInfo;
   List_Links* cur;
   int i = 0;
   
   UserMemLock(mem);
   LIST_FORALL(&mem->mmaps, cur) {
      if (i < *mmapIdx) {
         i++;
         continue;
      }

      mmInfo = (UserMemMapInfo*)cur;
      if (mmInfo->type == USERMEM_MAPTYPE_FD) {
         *mmapIdx = i + 1;
         UserMemUnlock(mem);
         return mmInfo;
      }

      i++;
   }
   UserMemUnlock(mem);

   return NULL;
}


/*
 *----------------------------------------------------------------------
 *
 * UserMem_DumpMMapObjects --
 *
 *	Dumps out the UserObj info for objects that back mmap regions
 *	but aren't in the file descriptor table.
 *
 * Results:
 *	VMK_OK on success, appropriate failure code otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserMem_DumpMMapObjects(UserDump_Header *header, UserDump_DumpData *dumpData)
{
   User_CartelInfo *uci = MY_USER_CARTEL_INFO;
   UserMem *mem = &uci->mem;
   VMK_ReturnStatus status = VMK_OK;
   UserDump_ObjEntry *objEntry;
   UserMemMapInfo *mmInfo;
   int mmapIdx = 0;
   int numObjs = 0;

   objEntry = User_HeapAlloc(uci, sizeof *objEntry);
   if (objEntry == NULL) {
      return VMK_NO_MEMORY;
   }

   /*
    * Find the next object-backed mmap.
    *
    * Note: We grab and release the lock within UserMemGetNextFdMap because
    * UserObj_FdForObj grabs the UserObj lock, and UserObj_ToString and
    * UserDump_Write may block.
    */
   while ((mmInfo = UserMemGetNextFdMap(mem, &mmapIdx)) != NULL) {
      int fd;

      /*
       * Check if this object is also in the fd table, in which case we don't
       * need to dump it here as well.
       */
      status = UserObj_FdForObj(uci, mmInfo->obj, &fd);
      if (status == VMK_NOT_FOUND) {
         memset(objEntry, 0, sizeof *objEntry);

	 objEntry->obj = (uint32)mmInfo->obj;
	 objEntry->fd = USEROBJ_INVALID_HANDLE;
         objEntry->type = mmInfo->obj->type;
	 status = UserObj_ToString(mmInfo->obj, objEntry->description,
				   sizeof objEntry->description);
	 if (status != VMK_OK) {
	    goto done;
	 }

	 status = UserDump_Write(dumpData, (uint8*)objEntry, sizeof *objEntry);
	 if (status != VMK_OK) {
	    goto done;
	 }

	 numObjs++;
      }
      ASSERT(status == VMK_OK);
   }

   header->objEntries += numObjs;

done:
   User_HeapFree(uci, objEntry);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserMemCopyDumpHeaders --
 *
 *	Copy as many mmap dump headers onto page as we can fit.  Start
 *	at 'restartIdx' in the list.
 *
 * Results:
 *	TRUE if all mmaps have been written, FALSE if we didn't get to
 *	the end of the list.  *restartIdx is updated to the next idx
 *	to be written, *regionsOffset is updated to the next offset
 *	the mmap should have in the file, and *totalLength is the
 *	total byte length of the mmap regions (for consistency checking).
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static Bool
UserMemCopyDumpHeaders(UserMem* mem,
                       unsigned* restartIdx,    // IN/OUT
                       unsigned* regionsOffset, // IN/OUT
                       unsigned* totalLength,   // IN/OUT
                       uint8* page,
		       unsigned* outBufferOffset) // IN/OUT
{
   unsigned bufferOffset = 0;
   List_Links* cur;
   int i = 0;

   ASSERT(mem);
   ASSERT(UserMemIsLocked(mem));

   Util_ZeroPage(page);

   LIST_FORALL(&mem->mmaps, cur) {
      const UserMemMapInfo* mmInfo = (UserMemMapInfo*)cur;
      UserDump_MMap* dumpMMap = (UserDump_MMap*)(page + bufferOffset);
      
      // Skip maps already dumped (in a previous call)
      if (i++ < *restartIdx) {
         continue;
      }
      
      // Fill in the dump-specific mmap info
      ASSERT(bufferOffset <= (PAGE_SIZE - sizeof *dumpMMap));
      ASSERT(mmInfo->startAddr % PAGE_SIZE == 0);
      dumpMMap->type   = mmInfo->type;
      dumpMMap->va     = mmInfo->startAddr;
      dumpMMap->length = mmInfo->length;
      dumpMMap->offset = *regionsOffset; // offset in core dump file
      dumpMMap->flags  = 0;
      if (mmInfo->prot & PTE_P) {
         dumpMMap->flags |= USERDUMPMMAP_FLAGS_PROT_READ;
      }
      if (mmInfo->prot & PTE_RW) {
         dumpMMap->flags |= USERDUMPMMAP_FLAGS_PROT_WRITE;
      }
      if (mmInfo->prot & PTE_PCD) {
         dumpMMap->flags |= USERDUMPMMAP_FLAGS_PCD;
      }
      if (mmInfo->pinned) {
         dumpMMap->flags |= USERDUMPMMAP_FLAGS_PINNED;
      }
      dumpMMap->filePgOffset = mmInfo->pgoff;
      dumpMMap->obj = (uint32)mmInfo->obj;

      UWLOG(4, "%#x [%d] @ %d", mmInfo->startAddr, mmInfo->length,
	    *regionsOffset);

      // Update pointers and offsets
      bufferOffset   += sizeof *dumpMMap;
      *totalLength   += mmInfo->length;
      *regionsOffset += ALIGN_UP(mmInfo->length, PAGE_SIZE);

      // Return if we've filled 'page' (assumed to be page sized)
      if (bufferOffset > (PAGE_SIZE - sizeof *dumpMMap)) {
         ASSERT(bufferOffset == PAGE_SIZE); // should fit exactly
	 *outBufferOffset = PAGE_SIZE;

         UWLOG(2, "Filled page. *idx=%d *totalLen=%u", *restartIdx, *totalLength);

         *restartIdx = i;

         // Done if we just squeezed the last entry on the page
         return (cur == List_Last(&mem->mmaps));
      }
   }

   ASSERT(bufferOffset < PAGE_SIZE);
   *outBufferOffset = bufferOffset;

   // Done if we fell off the end of the list
   UWLOG(2, "Finished list. *idx=%d *totalLen=%u", *restartIdx, *totalLength);

   *restartIdx = i;
   return TRUE;
}



/*
 *----------------------------------------------------------------------
 *
 * UserMemDumpGetMap --
 *
 *	Get the 'mmapIdx'th mmap object.
 *
 * Results:
 *	Nth UserMemMapInfo object, or NULL if the list is finished.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static UserMemMapInfo*
UserMemDumpGetMap(UserMem* mem,
                  int mmapIdx)
{
   List_Links* cur;
   int i = 0;
   
   ASSERT(UserMemIsLocked(mem));
   
   LIST_FORALL(&mem->mmaps, cur) {
      if (i == mmapIdx) {
         return (UserMemMapInfo*)cur;
      }
      i++;
   }

   return NULL;
}


/*
 *----------------------------------------------------------------------
 *
 * UserMem_DumpMMap --
 *
 *      Writes out the mmap'ed regions of this cartel to the core
 *      file.  Metadata first, then the raw mmap pages.  
 *
 *	*---  <current offset when called> (page-aligned)
 *      | 
 *      | Headers, one UserDump_MMap per mmap in LIST_FORALL order
 *      | 
 *	*---  <regionsOffset> (page-aligned)
 *	|
 *      | MMap data, full-page-sized per mmap in LIST_FORALL order
 *      | 
 *	*---
 *
 * Results:
 *      VMK_OK on success, appropriate failure code otherwise.
 *
 * Side effects:
 *      UserObj state is modified.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserMem_DumpMMap(UserDump_Header* dumpHeader, // IN/OUT
		 UserDump_DumpData *dumpData) // IN/OUT
{
   World_Handle *world = MY_RUNNING_WORLD;
   UserMem *mem = &world->userCartelInfo->mem;
   VMK_ReturnStatus status;
   unsigned regionsOffset;        // Offset for the mmap'ed regions (ie, the actual data)
   unsigned mmapIdx;
   unsigned totalLength;
   unsigned dataLength;
   unsigned bufferOffset;
   Bool allHeaders;
   UserMemMapInfo* mmInfo;
   uint8 *page;

   dumpHeader->mmapElements = 0;

   /*
    * Note that we acquire and drop the usermem lock a bunch in here.
    * We can do that because we know all the other threads in this
    * cartel are quiet (we force them to be).  However, we don't do
    * anything really stupid (like keeping a mmInfo pointer across a
    * lock release), worst case is an inconsistent core dump, not a
    * crashed kernel.
    */
   UserMemLock(mem);

   if (List_IsEmpty(&mem->mmaps)) {
      UWLOG(0, "No mmaps.");
      UserMemUnlock(mem);
      return VMK_OK;
   }

   page = User_HeapAlloc(world->userCartelInfo, PAGE_SIZE);
   if (page == NULL) {
      return VMK_NO_RESOURCES;
   }

   /*
    * First dump the metadata for all the mmaps.
    */
   UWLOG(1, "Dumping headers.");

   regionsOffset = 0;  
   mmapIdx = 0;
   totalLength = 0;

   do {
      DEBUG_ONLY(const unsigned prevIdx = mmapIdx);

      // Clears 'page' before copying any headers to it
      allHeaders = UserMemCopyDumpHeaders(mem, &mmapIdx, &regionsOffset,
					  &totalLength, page, &bufferOffset);

      ASSERT(prevIdx < mmapIdx); // Always make progress.

      UserMemUnlock(mem);
      status = UserDump_Write(dumpData, page, bufferOffset);
      UserMemLock(mem);

      if (status != VMK_OK) {
         UWLOG(0, "Failed to dump mmap metadata (@ idx=%d): %s",
               mmapIdx, UWLOG_ReturnStatusToString(status));
         goto out;
      }
   } while (! allHeaders);

   UWLOG(1, "Done with headers.  mmapIdx=%d, regionsOffset=%#x, totalLength=%d",
         mmapIdx, regionsOffset, totalLength);

   // record total number of mmap elements (needed for parsing the core file)
   dumpHeader->mmapElements = mmapIdx;

   /*
    * Write out mmap region data.
    */
   regionsOffset = 0;
   dataLength = 0;
   mmapIdx = 0;

   /*
    * Don't use LIST_FORALL to traverse the mmap list, because we're
    * going to drop the mem lock in the middle of the loop.  We just use
    * an integer "index" to track where we are in the list.
    */
   while ((mmInfo = UserMemDumpGetMap(mem, mmapIdx)) != NULL) {
      UWLOG(2, "%#x/%d @ %d", mmInfo->startAddr, mmInfo->length, regionsOffset);

      UserMemUnlock(mem);
      status = UserDump_WriteUserRange(world, dumpData, mmInfo->startAddr,
				       mmInfo->startAddr + mmInfo->length);
      UserMemLock(mem);

      if (status != VMK_OK) {
	 UWLOG(0, "Failed to dump mmap region (idx=%d, va=%#x, length=%d): %s",
               mmapIdx, mmInfo->startAddr, mmInfo->length,
	       UWLOG_ReturnStatusToString(status));
         goto out;
      }
      
      mmapIdx++;
      dataLength += mmInfo->length;
      regionsOffset += ALIGN_UP(mmInfo->length, PAGE_SIZE);
   }
         
   /*
    * Sanity check that dump is consistent.
    */
   if (mmapIdx != dumpHeader->mmapElements) {
      /* XXX dumpHeader->corrupt = TRUE */
      UWWarn("Probably corrupt core dump (2nd mmapCt (%d) != 1st mmapCt (%d)).",
             mmapIdx, dumpHeader->mmapElements);
   }

   if (dataLength != totalLength) {
      /* XXX dumpHeader->corrupt = TRUE */
      UWWarn("Probably corrupt core dump (totalLength (%d) != dataLength(%d)).",
             totalLength, dataLength);
   }

   UWLOG(1, "Completed mmap data regions.");
   status = VMK_OK;

  out:
   if (page != NULL) {
      User_HeapFree(world->userCartelInfo, page);
   }
   UserMemUnlock(mem);
   return status;
}   


/*
 *----------------------------------------------------------------------
 *
 * UserMemProcStatusRead --
 *
 *      Callback for read operation on "usermem/status" procfs node.
 *
 * Results: 
 *      Returns VMK_OK.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
static int
UserMemProcStatusRead(UNUSED_PARAM(Proc_Entry *entry),
                      char *buffer,
                      int  *len)
{
   UserMemStats *stats = &userMemStats;
   uint32 pageCount;
   uint32 pageShared;
   uint32 pageSwapped;
   uint32 pagePinned;

   /*
    * We could't access all stats atomically, so the reported
    * stats could be inconsistent.
    */
   pageCount = Atomic_Read(&stats->pageCount);
   pageShared = Atomic_Read(&stats->pageShared);
   pageSwapped = Atomic_Read(&stats->pageSwapped);
   pagePinned = Atomic_Read(&stats->pagePinned);

   *len = 0;
   Proc_Printf(buffer, len,
               "%10s %6d %8s %6d %8s %6d %8s %6d\n",
               "pages used", pageCount, "pshared", pageShared, 
               "swapped", pageSwapped, "pinned", pagePinned);
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * UserMem_Init --
 *
 *      Initialization userMem data structures and 
 *      registers procfs nodes.
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
UserMem_Init(void)
{
   UserMemStats *stats = &userMemStats;

   // register "usermem" directory
   Proc_InitEntry(&stats->procDir);
   Proc_Register(&stats->procDir, "usermem", TRUE);

   // register "usermem/status" entry
   Proc_InitEntry(&stats->procStatus);
   stats->procStatus.parent = &stats->procDir;
   stats->procStatus.read = UserMemProcStatusRead;
   Proc_Register(&stats->procStatus, "status", FALSE);
}

/*
 *----------------------------------------------------------------------
 *
 * UserMemGetMMInfoExecFlag --
 *
 *      Checks to see if the given MMInfo points to a mapped region
 *      in the code / data segment
 *
 * Results:
 *      Returns MEMMAP_EXEC if the address is in code segment.
 *      Returns MEMMAP_NOEXEC otherwise.
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
static UserMemMapExecFlag
UserMemGetMMInfoExecFlag(UserMemMapInfo* mmInfo)
{

   if (mmInfo->startAddr <= VMK_USER_LAST_MMAP_TEXT_VADDR) {
      return MEMMAP_EXEC;
   } else {
      return MEMMAP_NOEXEC;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * UserMemMoveMmap --
 *
 *      Creates a new MMInfo structure and Moves a mapped region to
 *      the new one. 
 *
 * Results:
 *      VMK_OK upon success , or error
 *
 * Side effects:
 *      newAddr contains newly mapped address
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserMemMoveMmap(World_Handle *world, UserMemMapInfo* oldMMInfo, UserVA* newAddr,
                int newLen, uint32 prot, List_Links *mmInfosToFree) 
{
   VMK_ReturnStatus status = VMK_OK;
   VMK_ReturnStatus copyStatus = VMK_OK;
   UserMemMapInfo* newMMInfo;
   UserMemMapInfo* mmInfoToDestroy;
   User_CartelInfo *uci = world->userCartelInfo;
   UserMem *mem = &world->userCartelInfo->mem;
   VPN startVPN;
   uint32 nPages;
   UserPTE *pte;
   UserPTE* newPte;
   Bool freeMe = FALSE;
   Bool swapIn = FALSE;
   MPN mpn;
   int i;
   uint32 pteFlags;
   UserVA tempAddr;
   VMK_PTE* tempPageTable;
   UserMemMapExecFlag execFlag = MEMMAP_IGNORE;
   
   ASSERT(UserMemIsLocked(mem));
         
   newMMInfo = (UserMemMapInfo*)User_HeapAlloc(uci, sizeof *newMMInfo);
   if (newMMInfo == NULL) {
      return VMK_NO_RESOURCES;
   }
   List_InitElement(&newMMInfo->links);
        
   // copy old mminfo contents into the new one
   *newMMInfo = *oldMMInfo;
   // clear addr and length
   newMMInfo->startAddr = 0;
   newMMInfo->length = 0;

   /*
    * See what segment the old mmaped region was in. 
    * Allocate new region in the same segment as the old one.
    */
   execFlag = UserMemGetMMInfoExecFlag(oldMMInfo);
   ASSERT(execFlag != MEMMAP_IGNORE);   

   status = UserMemMapInfoSetRange(world, newMMInfo, *newAddr, newLen);
   if (status == VMK_OK) {
      status = UserMemMapAllocRange(uci, newAddr, FALSE, newLen, prot, newMMInfo,
                                    execFlag);
   }
   if (status == VMK_OK) {
      if (newMMInfo->startAddr == 0) {
         ASSERT(PAGE_OFFSET(*newAddr) == 0);
         newMMInfo->startAddr = *newAddr;
      }
      // Insert into mmaps list
      UserMemMapInfoInsert(mem, newMMInfo);
   } else {
      User_HeapFree(uci, newMMInfo);
      return status;
   }
      
   /*
    * Increment the ref count if this is a file backed mapping
    * (because unmapping it will reduce the refcount later)
    */
   if (newMMInfo->obj != NULL) {   
      UserObj_Acquire(newMMInfo->obj);
   }

   // copy all old ptes to the new ones and then fix broken ones
   startVPN = VA_2_VPN(oldMMInfo->startAddr);
   nPages = CEIL(oldMMInfo->length, PAGE_SIZE);
   tempAddr = newMMInfo->startAddr;
   USERMEM_FOR_RANGE_DO(mem, startVPN, nPages, pte, i) {
      ASSERT(pte != NULL); // XXX runtime check

      tempPageTable = NULL;
      
      status = UserMemVA2PTE(mem, tempAddr, &tempPageTable, &newPte);
      ASSERT(status == VMK_OK);
      
      ASSERT(newPte != NULL);
      ASSERT(tempPageTable != NULL);
      *newPte = *pte;

      if (!UserPTE_IsPresent(pte)) {
         ASSERT(UserPTE_IsInUse(pte));
         if(UserPTE_InSwap(pte)) {
            if (UserPTE_IsSwapping(pte)) {
               mpn = UserPTE_GetMPN(pte);
               if (mpn != INVALID_MPN) {
                  // if page is being swapped out, cancel swapping
                  UserMemCancelSwapping(&mem->swapList, LA_2_LPN(laddr));
                  UWLOG(1, "UserMemMapMove: cancel swapping out lpn %x", 
                        LA_2_LPN(laddr));
                  // Restore the original state of the pte before swap-out began
                  pteFlags = UserPTE_GetFlags(pte);
                  UserPTE_Set(pte, mpn, pteFlags, FALSE, FALSE);
                  *newPte = *pte;

               } else {
                  /*
                   * XXX If a page was being swapped in at this time, then
                   * moving the pte is disastrous, since the function to swap-in
                   * a page will try to allocate a new MPN for it. Hence 
                   * returning E_BUSY if this is the case.
                   */
                  swapIn = TRUE;
                  copyStatus = VMK_BUSY;
                  UWLOG(1, "UserMemMapMove: Page being swapped in: lpn %x", 
                        LA_2_LPN(laddr));
               }
            }
         } else {
            /* 
             * if pte entry is not mapped or is not swapped out
             * then retain  newMMInfo structure
             */
              newPte->u.cachedPte.data = (uint32)newMMInfo;
         }
      }
            
      UserMemReleasePageTable(mem, tempPageTable);
      tempAddr += PAGE_SIZE;
      if (swapIn) {
         USERMEM_FOR_RANGE_BREAK;
      }
   }USERMEM_FOR_RANGE_END;
      
   //if copying of ptes went well, destroy oldMMInfo else destroy newMMInfo
   if (copyStatus == VMK_OK) {
      mmInfoToDestroy = oldMMInfo;
   } else {
      mmInfoToDestroy = newMMInfo;
   }
  
   // destroy MMInfo 
   status = UserMemMapDestroyMMInfo(world, mmInfoToDestroy, mmInfoToDestroy->startAddr,
         mmInfoToDestroy->length, &freeMe, NULL); 
   ASSERT(status == VMK_OK);
   // freeMe should be set to true because the entire MMInfo was destroyed
   ASSERT(freeMe == TRUE); 

   // remove the destroyed mmInfo from mmaps and free it
   List_Remove(&mmInfoToDestroy->links);
   List_Insert(&mmInfoToDestroy->links, LIST_ATFRONT(mmInfosToFree));

   return copyStatus;
}
                                                                                       
/*
 * --------------------------------------------------------------------
 * 
 * UserMem_Remap --
 *      Function to mremap a given region of memory.  The only thing that
 *      changes with this function are the start and/or length of a
 *      region.  The flags associated with a region are not changed with
 *      this function.
 *
 * Results:
 *      VMK_OK or error      
 *
 * Side effects:
 *      newAddr contains the newly (re)mapped address
 *    
 * ---------------------------------------------------------------------
 */

VMK_ReturnStatus
UserMem_Remap(World_Handle *world,
              UserVA addr,
              size_t oldLen,
              size_t newLen,
              int flags,
              UserVA *newAddr)
{
   VMK_ReturnStatus status = VMK_OK;
   User_CartelInfo *uci = world->userCartelInfo;
   UserMem* mem = &uci->mem;
   uint32 oldnPages;
   uint32 newnPages;
   uint32 oldLenPgAligned;
   uint32 newLenPgAligned;
   uint32 mmLenPgAligned;
   List_Links* itemPtr;
   UserMemPTEList pteList = NULL;
   UserMemMapInfo* curMMInfo = NULL;
   List_Links mmInfosToFree;
   List_Init(&mmInfosToFree);

   ASSERT(newAddr != NULL);

   UWLOG(1, "addr=%#x oldLen=%#x newLen=%#x flags=%#x",
         addr, oldLen, newLen, flags);
   
   UserMemLock(mem);
   
   // Find the region that is being remapped
   LIST_FORALL(&mem->mmaps, itemPtr) {
      curMMInfo = (UserMemMapInfo*)itemPtr;
      // Stop at first entry at or beyond addr.
      if ((curMMInfo->startAddr + curMMInfo->length) > addr) {
         break;
      }
   }

   // No region, no progress
   if (List_IsAtEnd(&mem->mmaps, itemPtr) || (curMMInfo->startAddr > addr)) {
      UWLOG(0, "(%#x) is not a valid mapped address", addr);
      UserMemUnlock(mem);
      return VMK_BAD_PARAM;
   }

   // Not allowed to overlap multiple (e.g., different prots) regions
   if ((curMMInfo->startAddr + curMMInfo->length) < (addr + oldLen)) {
      UWLOG(0, "(%#x + %#x) is not within a single mmap region (%#x + %#x)",
            addr, oldLen, curMMInfo->startAddr, curMMInfo->length);
      UserMemUnlock(mem);
      return VMK_BAD_PARAM;
   }

   /*
    * If the target area to be remapped isn't at the beginning of the
    * current mmap object, split the current object so addr *is* at the
    * beginning.
    */
   if (addr != curMMInfo->startAddr) {
      UserMemMapInfo* secondHalf;
      status = UserMemMapInfoSplit(world, curMMInfo, addr, &secondHalf);
      if (status == VMK_OK) {
         // Remap will happen with some portion of secondHalf:
         curMMInfo = secondHalf;
      } else {
         UWLOG(0, "split failed: %s", UWLOG_ReturnStatusToString(status));
         UserMemUnlock(mem);
         return status;
      }
   }

   oldLen = MAX(oldLen, curMMInfo->length);

   // old and new number of pages
   oldnPages = CEIL(oldLen, PAGE_SIZE);
   newnPages = CEIL(newLen, PAGE_SIZE);

   // oldLen and newLen page aligned
   oldLenPgAligned = ALIGN_UP(oldLen, PAGE_SIZE);
   newLenPgAligned = ALIGN_UP(newLen, PAGE_SIZE);
   mmLenPgAligned = ALIGN_UP(curMMInfo->length, PAGE_SIZE);
   
   if (newLen < oldLen) {
      Bool freeMe = FALSE;
      // Destroy the pages which are no longer part of the mapping
      if (newnPages < oldnPages) {
         UWLOG(2, "nuking subset of mminfo %p: (%#x +%#x pages)",
               curMMInfo, addr+newLenPgAligned, oldnPages - newnPages);
         status = UserMemMapDestroyMMInfo(world, curMMInfo, addr+newLenPgAligned,
                                          oldnPages - newnPages, &freeMe, &pteList);
      } else {
         UWLOG(2, "just trimming length to %#x from %#x",
               newLen, curMMInfo->length);
         status = UserMemMapInfoSetLength(world, curMMInfo, newLen);
         ASSERT(status == VMK_OK);
      }
      
      if (status == VMK_OK) {
         if (freeMe) {
             List_Remove(&curMMInfo->links);
             List_Insert(&curMMInfo->links, LIST_ATFRONT(&mmInfosToFree));
         } else {
            *newAddr = curMMInfo->startAddr;
         }
      } else {
         UWLOG(0, "Failed to lop a chunk off mminfo %p: %s",
               curMMInfo, UWLOG_ReturnStatusToString(status));
      }
   } else {
      const uint32 reqLen = newLenPgAligned - oldLenPgAligned;
      const uint32 prot = curMMInfo->prot;
      UserVA tempUserVA = addr + oldLenPgAligned;

      if (reqLen == 0) {
         // No-op resize.  UserMemMapAllocRange doesn't like a reqLen of 0.
         status = VMK_OK;
      } else {
         // See if the existing mapping can be extended  
         status = UserMemMapAllocRange(uci, &tempUserVA, FALSE, reqLen, prot,
                                       curMMInfo, MEMMAP_IGNORE);
      }

      if (status == VMK_OK) {
         // space exists. Try to update mminfo and fall out
         status = UserMemMapInfoSetLength(world, curMMInfo, newLen);
         if (status == VMK_OK) {
            *newAddr = curMMInfo->startAddr;
            UWLOG(2, "grew existing mmInfo %p to %#x bytes, addr=%#x, len=%#x",
                  curMMInfo, reqLen, *newAddr, curMMInfo->length);
         }
      } else if (status == VMK_EXISTS) {
         /*
          * Find another region of memory that is large enough and move 
          * the mapped region, if LINUX_MREMAP_MAYMOVE flag is set
          */
         if (flags & LINUX_MREMAP_MAYMOVE) {
            UWLOG(2, "finding a new mapping to replace current");
            status = UserMemMoveMmap(world, curMMInfo, newAddr,
                                     newLen, prot, &mmInfosToFree);
         } else {
            status = VMK_NO_RESOURCES;
            UWLOG(2, "mmInfo %p not movable, not growable: %s",
                  curMMInfo, UWLOG_ReturnStatusToString(status));
         }
      } else {
         // Failed.  Fall through and return error.
         UWLOG(0, "Failure trying to extend mmInfo %p: %s",
               curMMInfo, UWLOG_ReturnStatusToString(status));
      }
   }   

   UserMemUnlock(mem);

   // Must cleanup without the usermem lock
   if (pteList) {
      UserMemFlushAndFreePages(world, pteList);
   } else {
      UserMemCartelFlush(uci);
   }

   UserMemCleanupAndFreeMMInfos(world, &mmInfosToFree);

   UWLOG(1, "status=%s, *addr=%#x",
         UWLOG_ReturnStatusToString(status), *newAddr);
   return status;
}
