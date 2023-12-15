/* **********************************************************
 * Copyright 2000 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/* 
 * swap.c 
 *
 *      When the vmkernel wants to swap out pages from a VM we post an Action
 *      to the VMM to send us a list of pages to swap, the VMM calls into the
 *      vmkernel with this list of pages and the vmkernel swaps these pages
 *      out in the context of the VMM. Since the work of writing the pages to 
 *      disk is done in the context of the VMM we have 2 different approaches 
 *      depending on how much free memory we have.
 *         o If we have sufficient free memory and do not want to block the
 *           VMM while all the pages are swapped, we try to issue as many 
 *           async write commands as we can without blocking. In this
 *           approach we do not block the VMM
 *         o If we do not have sufficient free memory we block the VMM while
 *           all its pages are swapped out, check if more pages need to be
 *           swapped out for this VMM and basically ask the VMM to continue to 
 *           send us a list of pages until we have sufficient free memory. 
 *      Note: The determination of how much free memory is sufficient is done
 *      by the MemSched module.
 *              
 *      In order to achieve the swapping activity mentioned above the Swapper
 *      maintains a "swap state" with each vm the various "swap states" are
 *         SWAP_WSTATE_INACTIVE - This VM is currently not swapping out pages
 *         SWAP_WSTATE_LIST_REQ - The vmm has processed the swap action and has 
 *                                been informed on the number of pages it has to
 *                                hand us, we are now waiting for the vmm to
 *                                come back with the list of pages.
 *         SWAP_WSTATE_SWAPPING - This VM is currently in the process of
 *                                swapping out pages
 *         SWAP_WSTATE_SWAP_ASYNC - This VM is not actively swapping out pages
 *                                    but is waiting for some Async writes to
 *                                    complete
 *         SWAP_WSTATE_SWAP_DONE - All the pages in the current list of pages 
 *                                 have been written to disk.
 *
 *      The swapper allows the ability to add swap files dynamically, currently
 *      we limit the number of swap files to 8 and each of these 8 swap files 
 *      can be a maximum of 8GB. 
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "vmkernel.h"
#include "tlb.h"
#include "memmap.h"
#include "memalloc.h"
#include "host.h"
#include "splock.h"
#include "proc.h"
#include "alloc.h"
#include "alloc_inline.h"
#include "sched.h"
#include "world.h"
#include "pshare.h"
#include "timer.h"
#include "swap.h"
#include "action.h"
#include "kseg.h"
#include "vmnix_syscall.h"
#include "parse.h"
#include "hash.h"
#include "util.h"
#include "migrateBridge.h"
#include "vmkevent.h"
#include "fsSwitch.h"
#include "user.h"
#include "fsNameSpace.h"
#include "fsClientLib.h"

#define LOGLEVEL_MODULE Swap
#define LOGLEVEL_MODULE_LEN 4
#include "log.h"

// debugging
#if	defined(VMX86_DEBUG) && defined(VMX86_DEVEL)
#define	SWAP_DEBUG		(1)
#define SWAP_DEBUG_ASYNC_READS  (1)
#else
#define	SWAP_DEBUG		(0)
#define SWAP_DEBUG_ASYNC_READS  (0)
#endif

// targeted debugging
#define	SWAP_DEBUG_GENERATE_CANDIDATES	(0)


// Amount of free slots reserved in the swap file
#define SWAP_FREE_PCT           (1)

#define SWAP_DEFAULT_NR_ASYNC_WRITES (5)
#define SWAP_MAX_CLUSTER_SIZE        (16)

#define SWAP_MAX_NR_TOKENS      (16)

#define SWAP_MAX_NR_CPTFILE_OPEN_TRIES  (5)
#define SWAP_CPT_OPEN_SLEEP_PERIOD      (5000)

/* Constants used by the swapfile map */
#define SWAP_SLOT_IN_USE        (0x1)
#define SWAP_BITS_PER_SLOT      (1)           
#define SWAP_DUMMY_MAP_LENGTH   (1)

#define SWAP_NR_SLOTS(len)      ((len)/PAGE_SIZE)

#define SWAP_SLOTS_PER_BYTE     (8/SWAP_BITS_PER_SLOT)
#define SWAP_SLOTS_PER_UINT32   (4 * SWAP_SLOTS_PER_BYTE)
#define SWAP_SLOTS_PER_PAGE     (PAGE_SIZE * SWAP_SLOTS_PER_BYTE)
#define SWAP_NUM_BLOCKS(len)  \
   ((SWAP_NR_SLOTS(len) + SWAP_SLOTS_PER_PAGE - 1) / SWAP_SLOTS_PER_PAGE)

#define	SWAP_WORLDS_MAX         (MAX_WORLDS + 8) // +8 just to be on the safe side 
#define SWAP_DEFAULT_WORLD_ID   (0)
#define SWAP_ALL_BITS_SET       ((uint32)-1)
#define SWAP_INVALID_BLOCK        ((uint32)-1)

#define FOR_ALL_SWAP_FILES(i)  \
   ASSERT(swapGlobalInfo.nrSwapFiles <= SWAP_WORLDS_MAX); \
   for (i = 0 ; i < swapGlobalInfo.nrSwapFiles; ++i)

/* Constants used for storing the Swap file index and Swap file slot number in
 * the index field of the Alloc_PFrame structure 
 */

/*
 * We set SWAP_NUM_FILE_NDX_BITS to 4, even though we allow only 
 * a max of 8 swap files because we want to use the checkpoint
 * file as a special swap file with index of 15 i.e 0xff.
 */
#define SWAP_NUM_FILE_NDX_BITS      (4) 

#define SWAP_MAX_NUM_SWAP_FILES     (8)
#define SWAP_FILE_INVALID_INDEX     ((uint32) -1)

#define SWAP_SLOT_2_OFFSET(_slot) (((uint64)(_slot)) << PAGE_SHIFT)

// Reserved slots to denote pages that are backed by the checkpoint file,
// or a remote vmkernel (hot migration).
#define SWAP_CPT_FILE_INDEX     (14)
#define SWAP_MIGRATED_INDEX     (15)
#if SWAP_MAX_NUM_SWAP_FILES >= SWAP_CPT_FILE_INDEX
#error "Maximum number of swap files must be less than the checkpoint file index"
#endif

// Just make sure that we use only 28 bits
#if ((SWAP_NUM_FILE_NDX_BITS + SWAP_NUM_SLOT_NUM_BITS) > 28)
#error "Swap slot size cannot exceed the number of MPN bits in Alloc_PFrame"
#endif

#if ((SWAP_NUM_FILE_NDX_BITS + SWAP_NUM_SLOT_NUM_BITS) > 32)
#error "Swap slot size should be a max of 32 bits"
#endif

#if ((SWAP_NUM_FILE_NDX_BITS) < 3)
#error "Swap file ndx bits should atleast be 3"
#endif

typedef union SwapFileSlot {
   uint32 value;
   struct {
   unsigned slotNum: SWAP_NUM_SLOT_NUM_BITS;
   unsigned fileNdx: SWAP_NUM_FILE_NDX_BITS;
   unsigned pad    : (32 - (SWAP_NUM_SLOT_NUM_BITS + SWAP_NUM_FILE_NDX_BITS));
   } __attribute__ ((packed)) slot;
} SwapFileSlot;


typedef struct SwapFileBlock {
   MPN mapMPN;          // mpn containing the block map
   uint32 nrSlots;      // total number of slots in this mpn
   uint32 nrFreeSlots;  // number of free slots in this mpn
} SwapFileBlock;


typedef struct SwapFileStats {
   uint32 nrFastSearch;
   uint32 nrSlowSearch;
   uint32 nrPagesWritten; // Total # of pages written to this file
   uint32 nrPagesRead;    // Total # of pages read from this file
   uint32 nrSlotFindRetries;
} SwapFileStats;
/*
 * Structure required for maintaining the swap files 
 * Each swap file is divided into page size slots and this 
 * structure is used to keep track of the empty and full slots in the 
 * swap file. Although just one bit is required to indicate
 * whether the slot is full or empty, we may need to keep more
 * info per slot when/if we start swapping shared pages, hence the
 * SWAP_BITS_PER_SLOT constant defined earlier
 */
typedef struct SwapFileInfo {
   char filePath[FS_MAX_PATH_NAME_LENGTH];  // Swap file path
   FS_FileHandleID fileHandle ; // Swap file handle  
   uint32 fileNdx;              // index as in swapGlobalInfo.swapFileInfo[index]
   uint32 fileID;
   uint32 nrSlots ;             // # of page slots in the file 
   uint32 nrFreeSlots ;         // # of free slots in the file
   uint32 nrReservedSlots;
   SwapFileStats stats;
   uint32 numBlocks;
   SwapFileBlock *blocks;       // Swap file is an array of SwapFileBlocks         
   SP_SpinLock swapFileLock;
   uint32 dbgNrMPNs;
   MPN *dbgSlotContents;
   uint32 lastBlock;            // block from which free slots were found last
} SwapFileInfo ;

/*
 * SwapGlobalInfo: structure to hold data specific to the
 * swapper 
 */
typedef struct SwapGlobalInfo {
   volatile Bool  swapIsEnabled;
   uint32         nrSwapFiles;   // nr of swap files
   SwapFileInfo   *swapFileInfo[SWAP_WORLDS_MAX];
   uint32         totalNrFreeSlots;
   uint32         nextFileNdx;
   SP_SpinLock    swapGlobalLock;
   SP_SpinLock    freeSlotsLock;
   uint32         fileID;        // monotonically increasing IDs for swap files

   uint32         nrAsyncWriteFailures; // Total # of async write failures
   // proc nodes 
   Proc_Entry procDir ;	// procfs node "/proc/vmware/swap"
   Proc_Entry procSwapStats ;
} SwapGlobalInfo;

static SwapGlobalInfo swapGlobalInfo;

/* 
 * Sanity checks are always enabled in obj builds, on other builds 
 * they are enalbed through the config option CONFIG_SWAP_SANITY_CHECKS
 */
#if	defined(VMX86_DEBUG) && defined(VMX86_DEVEL)
static Bool swapDoSanityChecks = TRUE;
#else
static Bool swapDoSanityChecks = FALSE;
#endif

/*
 * Tokens to be used for async writes
 */
typedef struct {
   Async_Token *token;
   World_ID worldID;
   uint32 swapFileNdx;
   uint32 swapPPNNdx; 
   uint32 startSlotNum; 
   uint32 nrSlots; 
} SwapToken;

typedef struct {
   Async_Token *token;
   World_ID worldID;
   PPN ppn;
   MPN mpn;
   uint32 reqNum;
   SwapFileSlot swapFileSlot;
} UWSwapToken;

typedef struct SwapAsyncIOInfo{
   uint32 maxNrIO;
   uint32 nrPendingIO;
   SP_SpinLock lock;
} SwapAsyncIOInfo;

static SwapAsyncIOInfo swapAsyncIOInfo;

/*
 * routines to handle proc file system requests 
 *
 */
void Swap_ProcRegister(void) ;

static int SwapGetStats(Proc_Entry *entry, char *buffer, int *len);

/*
 * Utility methods
 */
static INLINE SwapFileInfo *SwapGetSwapFile(uint32 fileNdx);

/*
 * Function to get the list of pages to swap out
 */
static VMK_ReturnStatus SwapSwapOutPages(World_Handle *world, uint32 nrPagesRecvd, 
                                         BPN *inSwapBPNList, uint32 *nrRequestPages,
                                         Bool *tryCOW);
static Bool SwapCanSwapPFrame(World_Handle *world, PPN ppn, Bool frameValid, 
                              AllocPFrameState frameState, uint16 framePinCount);
/* Routine to read from swap file 
 */
static VMK_ReturnStatus SwapReadFile(World_Handle *world,
				     FS_FileHandleID fileHandle,
                                     MPN mpn, uint64 offset, 
                                     uint32 nrBytes,
                                     Async_Token *token);

/* Routines to write to swap file
 */
static uint32 SwapGetFileSlots(uint32 reqClusterSize,
                               uint32 *swapFileNdx,
                               uint32 *startSlotNum);
static VMK_ReturnStatus SwapClusterWrite(World_Handle *world, 
                                         Swap_VmmInfo *swapInfo,
                                         Bool *allPagesWritten);
static VMK_ReturnStatus SwapAsyncWrite(World_Handle      *world,
                                       FS_FileHandleID   fileHandle,
                                       Swap_VmmInfo       *swapInfo, 
                                       SwapFileInfo    *swapFileInfo,
                                       uint32  swapFileNdx,
                                       uint32  swapPPNNdx,
                                       uint32  startSlotNum, 
                                       uint32  nrSlots,
                                       uint32  *swapPFNextNdx);
static void SwapAsyncWriteCallback(Async_Token *token);


static void SwapFreeFileSlots(uint32 startSlotNum, uint32 nrSlots, 
                                    SwapFileInfo *swapFileInfo);
static void SwapWritePages(World_Handle *world, Swap_VmmInfo *swapInfo);
static VMK_ReturnStatus SwapOpenCptFile(World_Handle *world, 
                                  FS_FileHandleID vmnixFileHandle);
static void SwapCloseCptFile(World_Handle *world);
static void SwapStartSwapping(struct World_Handle *world);


typedef struct SwapSlotInfo {
   World_ID worldID;
   PPN    ppn;
   uint64 hash;
} SwapSlotInfo;

#define DBG_NR_SLOTINFO_PER_PAGE (PAGE_SIZE/sizeof(SwapSlotInfo))
#define DBG_SLOTINFO_OFFSET_MASK (DBG_NR_SLOTINFO_PER_PAGE - 1)
#define DBG_SLOTINFO_INDEX(slotnr) ((slotnr)/DBG_NR_SLOTINFO_PER_PAGE)
#define DBG_SLOTINFO_OFFSET(slotnr) ((slotnr) % DBG_NR_SLOTINFO_PER_PAGE)

// inline functions
static INLINE Swap_VmmInfo*
SwapGetVmmInfo(World_Handle *world)
{
   return &World_VMMGroup(world)->swapInfo;
}

static INLINE Swap_ChkpointFileInfo*
SwapGetCptFile(World_Handle *world)
{
   return &World_VMMGroup(world)->swapCptFile;
}

/*
 * lock functions
 */

// global lock
static INLINE void
SwapGlobalLock(void)
{
   SP_Lock(&swapGlobalInfo.swapGlobalLock);
}

static INLINE void
SwapGlobalUnlock(void)
{
   SP_Unlock(&swapGlobalInfo.swapGlobalLock);
}

static INLINE Bool
SwapGlobalIsLocked(void)
{
   return SP_IsLocked(&swapGlobalInfo.swapGlobalLock);
}

// free slots lock
static INLINE void
SwapFreeSlotsLock(void)
{
   SP_Lock(&swapGlobalInfo.freeSlotsLock);
}

static INLINE void
SwapFreeSlotsUnlock(void)
{
   SP_Unlock(&swapGlobalInfo.freeSlotsLock);
}

static INLINE Bool
SwapFreeSlotsIsLocked(void)
{
   return SP_IsLocked(&swapGlobalInfo.freeSlotsLock);
}

static INLINE void
SwapFreeSlotsWaitLock(void)
{
   ASSERT(SwapFreeSlotsIsLocked());
   CpuSched_Wait((uint32) &swapGlobalInfo.freeSlotsLock,
                 CPUSCHED_WAIT_SWAP_SLOTS, 
                 &swapGlobalInfo.freeSlotsLock); 
   SwapFreeSlotsLock();
}

static INLINE void
SwapFreeSlotsWakeup(void)
{
   ASSERT(SwapFreeSlotsIsLocked());
   CpuSched_Wakeup((uint32) &swapGlobalInfo.freeSlotsLock);
}

// async io lock
static INLINE void
SwapAsyncIOLock(void)
{
   SP_Lock(&swapAsyncIOInfo.lock);
}

static INLINE void
SwapAsyncIOUnlock(void)
{
   SP_Unlock(&swapAsyncIOInfo.lock);
}

static INLINE Bool
SwapAsyncIOIsLocked(void)
{
   return SP_IsLocked(&swapAsyncIOInfo.lock);
}

static INLINE void
SwapAsyncIOWaitLock(void)
{
   ASSERT(SwapAsyncIOIsLocked());
   CpuSched_Wait((uint32) &swapAsyncIOInfo.lock,
                 CPUSCHED_WAIT_SWAP_AIO, 
                 &swapAsyncIOInfo.lock); 
   SP_Lock(&swapAsyncIOInfo.lock);
}

static INLINE void
SwapAsyncIOWakeup(void)
{
   ASSERT(SwapAsyncIOIsLocked());
   CpuSched_Wakeup((uint32) &swapAsyncIOInfo.lock);
}

// file info lock
static INLINE void
SwapFileInfoLock(SwapFileInfo *swapFileInfo)
{
   SP_Lock(&swapFileInfo->swapFileLock);
}

static INLINE void
SwapFileInfoUnlock(SwapFileInfo *swapFileInfo)
{
   SP_Unlock(&swapFileInfo->swapFileLock);
}

// swap info lock
static INLINE void
SwapInfoLock(Swap_VmmInfo *swapInfo)
{
   SP_Lock(&swapInfo->infoLock);
}

static INLINE void
SwapInfoUnlock(Swap_VmmInfo *swapInfo)
{
   SP_Unlock(&swapInfo->infoLock);
}

static INLINE Bool
SwapInfoIsLocked(Swap_VmmInfo *swapInfo)
{
   return SP_IsLocked(&swapInfo->infoLock);
}

static INLINE void
SwapInfoWaitLock(Swap_VmmInfo *swapInfo, CpuSched_WaitState waitType)
{
   ASSERT(SwapInfoIsLocked(swapInfo));
   CpuSched_Wait((uint32) &swapInfo->infoLock, waitType, &swapInfo->infoLock);
   SwapInfoLock(swapInfo);
}

static INLINE void
SwapInfoWakeup(Swap_VmmInfo *swapInfo)
{
   ASSERT(SwapInfoIsLocked(swapInfo));
   CpuSched_Wakeup((uint32) &swapInfo->infoLock);
}

/*
 *--------------------------------------------------
 * SwapIsCptFile
 *
 *      Check if the file is a checkpoint file
 *
 * Returns:
 *      TRUE if the file index corresponds to 
 *      the checkpoint file and this world hasn't
 *	been migrated.
 *
 * Side effects:
 *      None.
 *
 *--------------------------------------------------
 */
static INLINE Bool
SwapIsCptFile(SwapFileSlot *swapFileSlot)
{
   return (swapFileSlot->slot.fileNdx == SWAP_CPT_FILE_INDEX);
}

/*
 *--------------------------------------------------
 * SwapIsMigrated
 *
 *      Check if the swap sources is a remote
 *	machine from which we migrated.
 *
 * Returns:
 *      TRUE if the file index corresponds to 
 *      a remote source.
 *
 * Side effects:
 *      None.
 *
 *--------------------------------------------------
 */
static INLINE Bool
SwapIsMigrated(SwapFileSlot *swapFileSlot)
{
   return (swapFileSlot->slot.fileNdx == SWAP_MIGRATED_INDEX);
}

/*
 *--------------------------------------------------
 * Swap_SetMigPFrame
 *
 *      Set the state of a pframe to indicate that its
 *	source is on a machine that we migrated from.
 *
 * Returns:
 *      None.
 *
 * Side effects:
 *      *pf is set.
 *
 *--------------------------------------------------
 */
void
Swap_SetMigPFrame(Alloc_PFrame *pf, PPN ppn)
{
   SwapFileSlot swapFileSlot;

   memset(&swapFileSlot, 0, sizeof(swapFileSlot));
   swapFileSlot.slot.fileNdx = SWAP_MIGRATED_INDEX;
   swapFileSlot.slot.slotNum = ppn;

   ASSERT(Alloc_PFrameIsRegular(pf));
   Alloc_PFrameSetState(pf, ALLOC_PFRAME_SWAPPED);
   Alloc_PFrameSetIndex(pf, swapFileSlot.value);
}

/*
 *--------------------------------------------------
 * Swap_IsMigPFrame
 *
 *      Return TRUE if this page is supposed to come
 *	from a remote machine.
 *
 * Returns:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *--------------------------------------------------
 */
Bool
Swap_IsMigPFrame(Alloc_PFrame *pf)
{
   uint32 frameIndex = Alloc_PFrameGetIndex(pf);
   SwapFileSlot swapFileSlot;
   swapFileSlot.value = frameIndex;
   return (Alloc_PFrameStateIsSwapped(Alloc_PFrameGetState(pf)) &&
           (swapFileSlot.slot.fileNdx == SWAP_MIGRATED_INDEX));
}

/*
 *--------------------------------------------------
 *
 * SwapResetAllocDuringSwap --
 *
 *      Reset the alloc stats collected while swap request
 *      was pending with the monitor.
 *
 * Returns:
 *      none.
 *
 * Side effects:
 *      updates stats.
 *
 *--------------------------------------------------
 */
static INLINE void
SwapResetAllocDuringSwap(Swap_VmmInfo *swapInfo) 
{
   // reset cur alloc pages while swap request was pending
   if (swapInfo->curAllocDuringSwap > swapInfo->maxAllocDuringSwap) {
      swapInfo->maxAllocDuringSwap = swapInfo->curAllocDuringSwap;
   }
   swapInfo->lastAllocDuringSwap = swapInfo->curAllocDuringSwap;
   swapInfo->curAllocDuringSwap = 0;
}

/*
 *--------------------------------------------------
 * Functions to help in debugging. Only compiled
 * for obj and beta builds.
 *--------------------------------------------------
 */
static INLINE void
SwapResetSlotInfo(SwapSlotInfo *slotInfo)
{
   slotInfo->worldID = INVALID_WORLD_ID;
   slotInfo->ppn = INVALID_PPN;
   slotInfo->hash = 0;
}
static INLINE Bool
SwapIsSlotFree(SwapSlotInfo *slotInfo) 
{
   return (slotInfo->worldID == INVALID_WORLD_ID &&
           slotInfo->ppn == INVALID_PPN &&
           slotInfo->hash == 0);
}

static INLINE void
SwapInitSlotInfo(SwapFileInfo *swapFileInfo) 
{
   uint32 i, j;
   if (!swapFileInfo->dbgSlotContents) {
      uint32 nrMPNs = ((swapFileInfo->nrSlots - 1)/DBG_NR_SLOTINFO_PER_PAGE) + 1;
      swapFileInfo->dbgSlotContents = Mem_Alloc(nrMPNs * sizeof(MPN));
      swapFileInfo->dbgNrMPNs = nrMPNs;
      for (i = 0; i < nrMPNs; i++) {
         KSEG_Pair   *pair;
         SwapSlotInfo *slotInfo;
         swapFileInfo->dbgSlotContents[i] = MemMap_AllocAnyKernelPage();
         ASSERT(swapFileInfo->dbgSlotContents[i] != INVALID_MPN);
         MemMap_SetIOProtection(swapFileInfo->dbgSlotContents[i], 
                                MMIOPROT_IO_DISABLE);

         slotInfo = (SwapSlotInfo *)Kseg_MapMPN(swapFileInfo->dbgSlotContents[i], &pair);
         for (j = 0; j < DBG_NR_SLOTINFO_PER_PAGE; j++) {
            SwapResetSlotInfo(&slotInfo[j]);
         }
         Kseg_ReleasePtr(pair);
      }
      LOG(0, "Initializing dbgSlotContents for swap file %s, used<%d> MPNs",
          swapFileInfo->filePath, nrMPNs);
   }
}

static INLINE void
SwapDeallocateSlotInfo(SwapFileInfo *swapFileInfo)
{
   if (swapFileInfo->dbgSlotContents) {
      uint32 i; 
      for (i = 0; i < swapFileInfo->dbgNrMPNs; i++) {
         ASSERT(swapFileInfo->dbgSlotContents[i] != INVALID_MPN);
         MemMap_FreeKernelPage(swapFileInfo->dbgSlotContents[i]);
      }
      Mem_Free(swapFileInfo->dbgSlotContents);
      swapFileInfo->dbgSlotContents = NULL;
   }
}


static INLINE MPN
SwapGetSlotInfoMPN(SwapFileInfo *swapFileInfo, uint32 slotNr)
{
   uint32 index = DBG_SLOTINFO_INDEX(slotNr); 
   ASSERT(slotNr < swapFileInfo->nrSlots);
   ASSERT(swapFileInfo->dbgSlotContents);

   ASSERT(swapFileInfo->dbgSlotContents[index] != INVALID_MPN);
   return swapFileInfo->dbgSlotContents[index];
}
static INLINE void
SwapSetSwapInfo(MPN slotMPN, uint32 slotNr, World_ID worldID, PPN ppn, MPN mpn)
{
   uint32 offset = DBG_SLOTINFO_OFFSET(slotNr);
   KSEG_Pair   *pair;
   KSEG_Pair   *hashPair;
   void *mpnPtr;
   SwapSlotInfo *slotInfo;
   ASSERT(slotMPN != INVALID_MPN);
   slotInfo = (SwapSlotInfo *)Kseg_MapMPN(slotMPN, &pair);
   ASSERT(SwapIsSlotFree(&slotInfo[offset]));

   slotInfo[offset].worldID = worldID;
   slotInfo[offset].ppn = ppn;
   mpnPtr = Kseg_MapMPN(mpn, &hashPair);
   slotInfo[offset].hash = Hash_Page(mpnPtr);
   Kseg_ReleasePtr(hashPair);
   Kseg_ReleasePtr(pair);
}
static INLINE void
SwapCheckSwapInfo(MPN slotMPN, uint32 slotNr, World_ID worldID, PPN ppn, MPN mpnToCheck)
{
   uint32 offset;
   KSEG_Pair   *pair;
   KSEG_Pair   *hashPair;
   void *mpnPtr;
   SwapSlotInfo *slotInfo;
   offset = DBG_SLOTINFO_OFFSET(slotNr);
   ASSERT(slotMPN != INVALID_MPN);
   slotInfo = (SwapSlotInfo *)Kseg_MapMPN(slotMPN, &pair);
   ASSERT(!SwapIsSlotFree(&slotInfo[offset]));
   ASSERT(slotInfo[offset].worldID == worldID);
   ASSERT(slotInfo[offset].ppn == ppn);

   mpnPtr = Kseg_MapMPN(mpnToCheck, &hashPair);
   ASSERT(slotInfo[offset].hash == Hash_Page(mpnPtr));
   Kseg_ReleasePtr(hashPair);
   Kseg_ReleasePtr(pair);
}
static INLINE void
SwapFreeSlotInfo(MPN slotMPN, uint32 slotNr) 
{
   uint32 offset = DBG_SLOTINFO_OFFSET(slotNr);
   KSEG_Pair   *pair;
   SwapSlotInfo *slotInfo;
   ASSERT(slotMPN != INVALID_MPN);
   slotInfo = (SwapSlotInfo *)Kseg_MapMPN(slotMPN, &pair);
   ASSERT(!SwapIsSlotFree(&slotInfo[offset]));
   SwapResetSlotInfo(&slotInfo[offset]);
   ASSERT(SwapIsSlotFree(&slotInfo[offset]));
   Kseg_ReleasePtr(pair);
}

/*
 *-----------------------------------------------------------------------------
 *
 * Swap_DoPageSanityChecks --
 *      compares the contents of the page that is read to the contents that were
 *      saved.
 *      NOTE: only used when debugging.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------
 */
void 
Swap_DoPageSanityChecks(World_Handle *world,
                        uint32 slotNr,
                        MPN newMPN,
                        PPN ppn)
{
   if (swapDoSanityChecks) {
      MPN slotMPN;
      SwapFileSlot swapFileSlot;
      SwapFileInfo *swapFileInfo;
      swapFileSlot.value = slotNr;

      if (SwapIsCptFile(&swapFileSlot) || SwapIsMigrated(&swapFileSlot)) {
         return;
      }

      swapFileInfo = SwapGetSwapFile(swapFileSlot.slot.fileNdx);
      ASSERT(swapFileSlot.slot.slotNum < swapFileInfo->nrSlots);

      slotMPN = SwapGetSlotInfoMPN(swapFileInfo, swapFileSlot.slot.slotNum);
      SwapCheckSwapInfo(slotMPN, swapFileSlot.slot.slotNum, 
                        World_GetGroupLeaderID(world), ppn, newMPN);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * SwapMapAllocPFrame --
 *      Helper function to get the Alloc_PFrame * for the given PPN
 *
 * Results:
 *      Returns the Alloc_PFrame * for the given PPN, if successful.
 *      Returns NULL otherwise.
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------
 */
static Alloc_PFrame *
SwapMapAllocPFrame(World_Handle *world,
                   PPN ppn,
                   KSEG_Pair **dirPair)
{
   uint32 dirIndex;
   uint32 pageIndex;
   MPN    dirMPN ;
   Alloc_PageInfo *pageInfo;
   Alloc_PFrame *dir;

   if (Alloc_LookupPPN(world, ppn, &dirIndex, &pageIndex) != VMK_OK) {
      VmWarn(world->worldID,
             "PPN <%d> to MPN lookup failed, This should never happen", ppn);
      ASSERT(0);
      return(NULL);
   }

   pageInfo = &Alloc_AllocInfo(world)->vmPages;
   dirMPN = pageInfo->pages[dirIndex];
   if (dirMPN == INVALID_MPN) {
      dirMPN = Alloc_MapPageDir(world, &pageInfo->pages[dirIndex]);
   }
   ASSERT(dirMPN != INVALID_MPN);
   if (dirMPN == INVALID_MPN) {
      VmWarn(world->worldID, "Invalid dirMPN for a page(0x%x)", ppn);
      return(NULL);
   }

   dir = Kseg_MapMPN(dirMPN, dirPair);
   ASSERT(dir != NULL);
   return(&dir[pageIndex]);
}

/*
 *-----------------------------------------------------------------------------
 *
 * SwapReleaseAllocPFrame --
 *      Release the Alloc_PFrame Kseg mapping.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE void
SwapReleaseAllocPFrame(KSEG_Pair *dirPair)
{
   Kseg_ReleasePtr(dirPair);
}


/*
 *----------------------------------------------------------------------
 *
 * Swap_UpdateDoSanityChecks -- 
 *
 *      Callback for changes to swap sanity check config variables.
 *      Currently we only handle the case where the sanity checks
 *      are enabled and we have to initialize the related data structures.
 *         Handling the case where we switch of sanity checking is
 *      slightly tricky becuase there could be code on the other
 *      cpus which is trying to do Sanity checks while we free up the
 *      related data structures. And since there is no pressing need
 *      to add this functionality i am punting on it for now.
 *
 *      Note: it is safe to call this function multiple times, although
 *      as mentioned above disabling this check is not supported currently
 *
 * Results:
 *      none.
 *      
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Swap_UpdateDoSanityChecks(Bool write, 
                          Bool valueChanged, 
                          UNUSED_PARAM(int ndx))
{
   Bool doSanityChecks;
   VMK_ReturnStatus status = VMK_OK;

   // If VMs already running, quit.
   if (MemSched_TotalSwapReserved() > 0) {
      LOG(0, "Failed to enable swap checks as swap has been reserved");
      return VMK_FAILURE;
   }
   if (swapDoSanityChecks) {
      return VMK_OK;
   }
   if (write && valueChanged) {

      doSanityChecks = CONFIG_OPTION(MEM_SWAP_SANITY_CHECKS);
      if (doSanityChecks) {
         uint32 i;
         // Acquire lock so that no other swap files can be added
         // until we are done.
         SwapGlobalLock();
         FOR_ALL_SWAP_FILES(i) {
            SwapFileInfo *swapFileInfo = swapGlobalInfo.swapFileInfo[i];
            SwapInitSlotInfo(swapFileInfo);
         }
         // If no VMs added since we checked previously, 
         // it is safe to start sanity checking
         if (MemSched_TotalSwapReserved() > 0) {
            swapDoSanityChecks = TRUE;
         } else {
            LOG(0, "Failed to enable swap checks as swap has been reserved");
            status = VMK_FAILURE;
         }
         SwapGlobalUnlock();
      }
   }
   return status;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Swap_ProcRegister --
 *      Adds the swap dir under the /proc/vmware dir and adds
 *      various files to it to query the swap device 
 *      parameters.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------
 */
void 
Swap_ProcRegister(void) 
{
   memset(&swapGlobalInfo.procDir, 0 ,sizeof(swapGlobalInfo.procDir));
   Proc_Register(&swapGlobalInfo.procDir, "swap", TRUE);

   memset(&swapGlobalInfo.procSwapStats, 0, sizeof(swapGlobalInfo.procSwapStats));
   swapGlobalInfo.procSwapStats.read = SwapGetStats;
   swapGlobalInfo.procSwapStats.parent = &swapGlobalInfo.procDir;
   swapGlobalInfo.procSwapStats.canBlock = TRUE;
   Proc_Register(&swapGlobalInfo.procSwapStats, "stats", FALSE);
}


/*
 *-----------------------------------------------------------------------------
 *
 * Swap_ProcUnregister --
 *      Removes the swap dir under the /proc/vmware.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------
 */
void 
Swap_ProcUnregister(void) 
{
   Proc_Remove(&swapGlobalInfo.procSwapStats);
   Proc_Remove(&swapGlobalInfo.procDir);
}


/*
 *----------------------------------------------------------------------
 *
 * SwapWorldStateToString --
 *
 *      Returns human-readable string representation of state "n",
 *	or the string 'unknwn' if "n" is not a valid state.
 *
 * Results:
 *      Returns human-readable string representation of state "n".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static const char *
SwapWorldStateToString(SwapWorldState n)
{
   switch (n) {
   case SWAP_WSTATE_INACTIVE:
      return("inactv");
   case SWAP_WSTATE_LIST_REQ:
      return("lstreq");
   case SWAP_WSTATE_SWAPPING:
      return("swapng");
   case SWAP_WSTATE_SWAP_ASYNC:
      return("swasyc");
   case SWAP_WSTATE_SWAP_DONE:
      return("swpdon");
   default:
      return("unknwn");
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * Swap_VmmGroupStatsHeaderFormat --
 *      
 *      If buffer is NULL, Logs the per vmm group stats header, else
 *      writes the per world stats header to the proc node.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------
 */
void
Swap_VmmGroupStatsHeaderFormat(char *buffer, int *len)
{
   if (buffer) {
      Proc_Printf(buffer, len, "\n%4s %6s %10s %10s "
                  "%10s %10s %10s %4s %10s %10s %10s\n", 
                  "vm", "status", "tgt", "swpd", 
                  "read", "wrtn", "cow", "cont",
                  "alloc-max", "alloc-last", "alloc-cur");
   } else {
      LOG(0, "\n%4s %6s %10s %10s "
          "%10s %10s %10s %4s %10s %10s %10s\n", 
          "vm", "status", "tgt", "swpd", 
          "read", "wrtn", "cow", "cont",
          "alloc-max", "alloc-last", "alloc-cur");
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * Swap_VmmGroupStatsFormat --
 *      
 *      If buffer is NULL, Logs the per vmm group stats, else
 *      writes the per world stats to the proc node.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------
 */
void
Swap_VmmGroupStatsFormat(World_Handle *world, char *buffer, int *len)
{
   MemSchedVmmUsage *usage = MemSched_ClientVmmUsage(world);
   Swap_VmmInfo *info = SwapGetVmmInfo(world);

   if (buffer) {
      Proc_Printf(buffer, len, "%4d %6s %10u %10u "
                  "%10u %10u %10u %4u %10u %10u %10u\n", 
                  world->worldID, 
                  SwapWorldStateToString(info->worldState), 
                  PagesToKB(info->nrPagesToSwap), 
                  PagesToKB(usage->swapped), 
                  PagesToKB(info->stats.numPagesRead), 
                  PagesToKB(info->stats.numPagesWritten), 
                  PagesToKB(info->stats.numCOWPagesSwapped), 
                  info->continueSwap,
                  PagesToKB(info->maxAllocDuringSwap), 
                  PagesToKB(info->lastAllocDuringSwap), 
                  PagesToKB(info->curAllocDuringSwap));
   } else {
      LOG(0, "%4d %6s %10u %10u "
          "%10u %10u %10u %4u %10u %10u %10u\n", 
          world->worldID, 
          SwapWorldStateToString(info->worldState), 
          PagesToKB(info->nrPagesToSwap), 
          PagesToKB(usage->swapped), 
          PagesToKB(info->stats.numPagesRead), 
          PagesToKB(info->stats.numPagesWritten), 
          PagesToKB(info->stats.numCOWPagesSwapped), 
          info->continueSwap,
          PagesToKB(info->maxAllocDuringSwap), 
          PagesToKB(info->lastAllocDuringSwap), 
          PagesToKB(info->curAllocDuringSwap));
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * Swap_GetInfo --
 *
 *       Get the swap file info
 *
 * Results:
 *      VMK_OK on success, error code otherwise
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus 
Swap_GetInfo(VMnix_SwapInfoArgs *args,
             VMnix_SwapInfoResult *result,
             unsigned long resultLen)
{
   const uint32 ndx = args->fileIndex;
   SwapFileInfo **swapFileInfo = swapGlobalInfo.swapFileInfo;
   // initialize return values
   result->valid = FALSE;
   SwapGlobalLock();
   if (ndx >= swapGlobalInfo.nrSwapFiles) {
      SwapGlobalUnlock();
      return VMK_NOT_FOUND;
   }
   SwapFileInfoLock(swapFileInfo[ndx]);
   result->fileID =  swapFileInfo[ndx]->fileID;
   strncpy(result->filePath, swapFileInfo[ndx]->filePath,
           FS_MAX_PATH_NAME_LENGTH);
   result->sizeMB = PAGES_2_MBYTES(swapFileInfo[ndx]->nrSlots);
   result->usedSizeMB = PAGES_2_MBYTES((swapFileInfo[ndx]->nrSlots - 
                                        swapFileInfo[ndx]->nrFreeSlots) - 
                                       swapFileInfo[ndx]->nrReservedSlots); 
   // contents are valid
   result->valid = TRUE;
   SwapFileInfoUnlock(swapFileInfo[ndx]);
   SwapGlobalUnlock();
   return VMK_OK;
}


/*
 *-----------------------------------------------------------------------------
 *
 * SwapGetStats --
 *      Adds the swap statistics to the buffer
 *
 * Results:
 *      none
 *
 * Side effects:
 *	Sets "*len" to number of characters written to "buffer".
 *
 *-----------------------------------------------------------------------------
 */
static int 
SwapGetStats(UNUSED_PARAM(Proc_Entry *entry),
             char *buffer,     
             int  *len) 
{
   uint32 j;
   SwapFileInfo **swapFileInfo = swapGlobalInfo.swapFileInfo;
   uint32 totalSize = 0;
   uint32 totalUsed = 0, totalFree = 0, totalRes = 0, totalWrtn = 0;
   uint32 totalRead = 0, totalFast = 0, totalSlow = 0, totalRetries = 0;

   *len = 0;

   SwapGlobalLock();
   Proc_Printf(buffer, len, "%6s %32s %16s %11s " 
               "%10s %10s %9s %10s %9s %12s %12s %8s\n",
               "fileID", "device", "filename", "Size(MB)",
               "used", "free", "res", "wrtn",
               "read", "fast-search", "slow-search", "retries");
   for (j = 0; j < swapGlobalInfo.nrSwapFiles; ++j) {
      uint32 nrSlots, nrFreeSlots;
      uint32 nrPagesWritten, nrPagesRead;
      uint32 nrFastSearch, nrSlowSearch, nrSlotFindRetries;
      uint32 nrReservedSlots;

      SwapFileInfoLock(swapFileInfo[j]);
      nrFreeSlots = swapFileInfo[j]->nrFreeSlots;
      nrSlots = swapFileInfo[j]->nrSlots;
      nrPagesWritten = swapFileInfo[j]->stats.nrPagesWritten;
      nrReservedSlots = swapFileInfo[j]->nrReservedSlots;
      nrPagesRead = swapFileInfo[j]->stats.nrPagesRead;
      nrFastSearch = swapFileInfo[j]->stats.nrFastSearch;
      nrSlowSearch = swapFileInfo[j]->stats.nrSlowSearch;
      nrSlotFindRetries = swapFileInfo[j]->stats.nrSlotFindRetries;
      SwapFileInfoUnlock(swapFileInfo[j]);

      Proc_Printf(buffer, len, "%6d %48s %11u " 
                  "%10u %10u %9u %10u %9u %12u %12u %8u\n",
                  swapFileInfo[j]->fileID, swapFileInfo[j]->filePath,
                  PAGES_2_MBYTES(swapFileInfo[j]->nrSlots), 
                  PagesToKB((nrSlots - nrFreeSlots) - nrReservedSlots), 
                  PagesToKB(nrFreeSlots), 
                  PagesToKB(nrReservedSlots), 
                  PagesToKB(nrPagesWritten), 
                  PagesToKB(nrPagesRead), 
                  nrFastSearch, nrSlowSearch, nrSlotFindRetries);
      totalSize += PAGES_2_MBYTES(swapFileInfo[j]->nrSlots);
      totalUsed += PagesToKB((nrSlots - nrFreeSlots) - nrReservedSlots);
      totalFree += PagesToKB(nrFreeSlots);
      totalRes  += PagesToKB(nrReservedSlots);
      totalWrtn += PagesToKB(nrPagesWritten);
      totalRead += PagesToKB(nrPagesRead);
      totalFast += nrFastSearch;
      totalSlow += nrSlowSearch;
      totalRetries += nrSlotFindRetries;
   }
   Proc_Printf(buffer, len, "Totals %32s %16s %11u " 
               "%10u %10u %9u %10u %9u %12u %12u %8u\n",
               "", "", totalSize, totalUsed, totalFree,
               totalRes, totalWrtn, totalRead, 
               totalFast, totalSlow, totalRetries);
   SwapGlobalUnlock();

   if (swapDoSanityChecks) {
      Proc_Printf(buffer, len, "\nSanity checking is enabled.\n");
   }
   return (VMK_OK);
}

/*
 *-----------------------------------------------------------------------------
 *
 * SwapAddTotalNumFreeSlots --
 *
 *      Helper function to atomically increment the total num free slots
 *
 * Results:
 *      none
 *
 * Side effects:
 *      Wakeup VMs waiting for free slots
 *
 *-----------------------------------------------------------------------------
 */
static INLINE void
SwapIncTotalNumFreeSlots(uint32 nrSlots) 
{
   SwapFreeSlotsLock();
   swapGlobalInfo.totalNrFreeSlots += nrSlots;
   // Wakeup any VMs waiting for free slots
   SwapFreeSlotsWakeup();
   SwapFreeSlotsUnlock();
}
/*
 *-----------------------------------------------------------------------------
 *
 * SwapSubTotalNumFreeSlots --
 *
 *      Helper function to atomically decrement the total num free slots
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE void
SwapDecTotalNumFreeSlots(uint32 nrSlots)
{
   SwapFreeSlotsLock();
   ASSERT(swapGlobalInfo.totalNrFreeSlots >= nrSlots);
   swapGlobalInfo.totalNrFreeSlots -= nrSlots;
   SwapFreeSlotsUnlock();
}
/*
 *-----------------------------------------------------------------------------
 *
 * SwapTestAndSleepFreeSlots --
 *      If no more free slots are available block the caller waiting for
 *      free slots.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      Caller will be blocked until some swap slots become available.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE void
SwapTestAndSleepFreeSlots(UNUSED_PARAM(World_Handle *world))
{
   ASSERT_HAS_INTERRUPTS();
   SwapFreeSlotsLock();
   while (swapGlobalInfo.totalNrFreeSlots == 0) {
      // Go to sleep and acquire lock again.
      SwapFreeSlotsWaitLock();
   } 
   SwapFreeSlotsUnlock();
}

/*
 *-----------------------------------------------------------------------------
 *
 * Swap_GetTotalNumSlots --
 *      Returns the number of swap slots available.
 *
 * Returns:
 *      Sum of the slots in all the swap files, or zero if swapping is disabled.
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------
 */
uint32
Swap_GetTotalNumSlots(UNUSED_PARAM(World_ID worldID))
{
   uint32 totalNumSlots = 0;
   if (swapGlobalInfo.swapIsEnabled) {
      uint32 i;
      FOR_ALL_SWAP_FILES(i){
         SwapFileInfo *swapFileInfo = swapGlobalInfo.swapFileInfo[i];
         ASSERT(swapFileInfo->nrSlots > swapFileInfo->nrReservedSlots);
         totalNumSlots += (swapFileInfo->nrSlots - 
                           swapFileInfo->nrReservedSlots); 
      }
      return totalNumSlots;
   } else {
      return(0);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * Swap_GetNumFreeSlots --
 *      Gets the number of free slots
 *
 *      Note: Do not trust this number too much as the number of
 *      free slots could increase/decrease depending on the swap in/swap out 
 *      activity in the system
 *
 * Returns:
 *      The number of free slots, or zero if swapping disabled.
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------
 */
uint32
Swap_GetNumFreeSlots(UNUSED_PARAM(World_ID worldID))
{
   if (swapGlobalInfo.swapIsEnabled) {
      return swapGlobalInfo.totalNrFreeSlots;
   } else {
      return(0);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * Swap_Init --
 *      Initialize the swap device 
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      none.
 *-----------------------------------------------------------------------------
 */
void 
Swap_Init(void) 
{
   memset(&swapGlobalInfo, 0, sizeof(swapGlobalInfo));
   memset(&swapAsyncIOInfo, 0, sizeof(swapAsyncIOInfo));
   swapAsyncIOInfo.maxNrIO = SWAP_MAX_NR_TOKENS;
   swapAsyncIOInfo.nrPendingIO = 0;
   swapGlobalInfo.swapIsEnabled = FALSE;

   SP_InitLock("swap", &swapGlobalInfo.swapGlobalLock, SP_RANK_SWAP);
   SP_InitLock("swapFreeSlots", &swapGlobalInfo.freeSlotsLock, SP_RANK_FREESLOTS);
   SP_InitLock("swapAsyncIOInfo", &swapAsyncIOInfo.lock, SP_RANK_SWAPASYNCIO);

   ASSERT(swapGlobalInfo.nrSwapFiles == 0);
   ASSERT(swapGlobalInfo.nrAsyncWriteFailures == 0);
}

/*
 *-----------------------------------------------------------------------------
 *
 * SwapAddNrPagesWritten --
 *      Helper functions to atomically increment the number of pages
 *      written to this file.
 *
 * Results: 
 *      none.
 *
 * Side effects:
 *      none
 *
 *-----------------------------------------------------------------------------
 */
static INLINE void
SwapAddNrPagesWritten(SwapFileInfo *swapFileInfo,
                      uint32 nrPagesWritten) 
{
   SwapFileInfoLock(swapFileInfo);
   swapFileInfo->stats.nrPagesWritten += nrPagesWritten;
   SwapFileInfoUnlock(swapFileInfo);
}

/*
 *-----------------------------------------------------------------------------
 *
 * SwapAddNrPagesRead --
 *      Helper functions to atomically increment the number of pages
 *      read from this file.
 *
 * Results: 
 *      none.
 *
 * Side effects:
 *      none
 *
 *-----------------------------------------------------------------------------
 */
static INLINE void
SwapAddNrPagesRead(SwapFileInfo *swapFileInfo,
                   uint32 nrPagesRead) 
{
   SwapFileInfoLock(swapFileInfo);
   swapFileInfo->stats.nrPagesRead += nrPagesRead;
   SwapFileInfoUnlock(swapFileInfo);
} 

/*
 *-----------------------------------------------------------------------------
 * Swap_ActivateFile --
 *      Activate the specified swap file and add it to the list of swap files.
 *      Create the swap world if required.
 *
 * Results:
 *      VMK_OK on success.
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
Swap_ActivateFile(const char *filePath)
{
   SwapFileInfo *swapFile;
   SwapFileInfo **swapFileInfo = swapGlobalInfo.swapFileInfo;
   VMK_ReturnStatus status = VMK_OK;
   uint32 i;
   FS_FileHandleID fileHandle ;
   FS_FileAttributes attrs;
   uint64 fileLength;

   status = FSS_OpenFilePath(filePath, FILEOPEN_EXCLUSIVE, &fileHandle);
   if (status != VMK_OK) {
      Warning("FSS_OpenFile(\"%s\") failed:status = <0x%x>", filePath, status);
      return status;
   }
   
   status = FSClient_GetFileAttributes(fileHandle, &attrs);
   if (status != VMK_OK) {
      Warning("FSClient_GetFileAttributes failed:status = <0x%x>", status);
      FSS_CloseFile(fileHandle);
      return status;
   }

   if ((attrs.flags & FS_SWAP_FILE) == 0) {
      Warning("file %s is not a swap file, failed to activate",
              filePath);
      FSS_CloseFile(fileHandle);
      return VMK_BAD_PARAM;
   }

   // get the length from the actual file
   fileLength = attrs.length;

   if (fileLength < PAGE_SIZE) {
      Warning("Cannot activate a swap file with size < %d",PAGE_SIZE);
      FSS_CloseFile(fileHandle);
      return VMK_BAD_PARAM;
   }

   if (fileLength > ((uint64)SWAP_FILE_MAX_SIZE_MB << 20)) {
      Warning("Cannot activate a swap file with length greater than %d MB",
              SWAP_FILE_MAX_SIZE_MB);
      FSS_CloseFile(fileHandle);
      return VMK_BAD_PARAM;
   }

   swapFile = (SwapFileInfo *)Mem_Alloc(sizeof(SwapFileInfo));

   ASSERT(swapFile != NULL);
   if (!swapFile) {
      Warning("Insufficient memory: Cannot activate swap file");
      FSS_CloseFile(fileHandle);
      return VMK_NO_MEMORY;
   }

   memset((void *)swapFile, 0, sizeof(SwapFileInfo));

   swapFile->blocks = (SwapFileBlock *)Mem_Alloc(SWAP_NUM_BLOCKS(fileLength) *
                                                 sizeof(SwapFileBlock));

   ASSERT(swapFile->blocks != NULL);
   if (!swapFile->blocks) {
      Mem_Free(swapFile);
      Warning("Insufficient memory: Cannot activate swap file");
      FSS_CloseFile(fileHandle);
      return VMK_NO_MEMORY;
   }

   memset((void *)swapFile->blocks, 0, 
          sizeof(SWAP_NUM_BLOCKS(fileLength) * sizeof(SwapFileBlock)));

   strncpy(swapFile->filePath, filePath, FS_MAX_PATH_NAME_LENGTH);

   swapFile->nrFreeSlots = swapFile->nrSlots = SWAP_NR_SLOTS(fileLength);

   ASSERT(swapFile->nrSlots <= SWAP_MAX_NUM_SLOTS_PER_FILE);

   swapFile->numBlocks = SWAP_NUM_BLOCKS(fileLength);

   {
      // The swap file size may be such that we may ending up using
      // only some of the slots in the last MPN.
      uint32 nrFreeSlotsLeft = swapFile->nrFreeSlots;
      for (i = 0; i < swapFile->numBlocks; ++i) {
         // Allocate MPN's as required
         ASSERT(nrFreeSlotsLeft > 0);
         swapFile->blocks[i].mapMPN = INVALID_MPN;
         swapFile->blocks[i].nrSlots = swapFile->blocks[i].nrFreeSlots = 
                                    MIN(nrFreeSlotsLeft, SWAP_SLOTS_PER_PAGE);
         nrFreeSlotsLeft -= swapFile->blocks[i].nrSlots;
      }
   }

   // reserve some free slots in the swap file
   swapFile->nrReservedSlots = ((swapFile->nrSlots/100) * SWAP_FREE_PCT);
   swapFile->nrFreeSlots -= swapFile->nrReservedSlots;
   swapFile->fileHandle = fileHandle;

   // Acquire the swap lock
   SwapGlobalLock();

   if (swapGlobalInfo.nrSwapFiles >= SWAP_MAX_NUM_SWAP_FILES) {
      SwapGlobalUnlock();
      FSS_CloseFile(swapFile->fileHandle);
      Mem_Free(swapFile->blocks);
      Mem_Free(swapFile);
      Warning("Maximum number of swap files (%d) already active, "
	      "cannot activate any more swap files", SWAP_MAX_NUM_SWAP_FILES);
      return VMK_LIMIT_EXCEEDED;
   }

   /* Increment swapGlobalInfo.nrSwapFiles last, 
    * this is required for correctness as the function 
    * SwapGetSwapFile currently does not use any locks and
    * as long as we increment nrSwapFiles after 
    * swapGlobalInfo.swapFileInfo is properly initialized
    * it should not need locks at all.
    * Also note that swap files can never be removed
    */
   SP_InitLock("swapFileMap", &swapFile->swapFileLock, SP_RANK_FILEMAP);
   swapFile->stats.nrPagesWritten = 0;
   swapFile->stats.nrPagesRead = 0;
   swapFile->fileNdx = swapGlobalInfo.nrSwapFiles;
   swapFile->fileID = swapGlobalInfo.fileID;
   swapGlobalInfo.fileID++;
   swapFileInfo[swapGlobalInfo.nrSwapFiles] = swapFile;
   ASSERT(swapGlobalInfo.nrSwapFiles < SWAP_MAX_NUM_SWAP_FILES);
   swapGlobalInfo.nrSwapFiles++;

   // Increment the total number of free slots after
   // this swap file is added to the list of swap files
   SwapIncTotalNumFreeSlots(swapFile->nrFreeSlots); 

   // Enable swapping
   if (!swapGlobalInfo.swapIsEnabled) {
      swapGlobalInfo.swapIsEnabled = TRUE;
      Swap_ProcRegister();
   }

   if (swapDoSanityChecks) {
      SwapInitSlotInfo(swapFile); 
   }

   // Make new swap space visible to memory scheduler
   MemSched_AddSystemSwap(swapFile->nrFreeSlots);

   // Release the swap lock
   SwapGlobalUnlock();
   Log("Swap file %s activated", filePath);
   return VMK_OK;
}


/*
 *-----------------------------------------------------------------------------
 *
 * SwapGetNdxFromID --
 *
 *    Simple helper function to get the index of the file specified by ID
 *
 * Results:
 *    the index of the file on success,
 *    SWAP_FILE_INVALID_INDEX on failure
 *
 * Side effects:
 *    none.

 *-----------------------------------------------------------------------------
 */
static INLINE uint32
SwapGetNdxFromID(uint32 fileID) 
{
   uint32 i;
   ASSERT(SwapGlobalIsLocked());
   FOR_ALL_SWAP_FILES(i) {
      SwapFileInfo *swapFileInfo = swapGlobalInfo.swapFileInfo[i];
      if (swapFileInfo->fileID == fileID) {
         return i;
      }
   }
   return SWAP_FILE_INVALID_INDEX;
}

/*
 *-----------------------------------------------------------------------------
 *
 * Swap_DeactivateFile --
 *
 *      Close the specified swap file and release all the 
 *      memory allocated to it.
 *
 * Results:
 *      VMK_OK on success, error code on failure.
 *
 * Side effects:
 *       Swap may be disabled if the last remaining swap file is deactivated
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
Swap_DeactivateFile(uint32 fileID)
{
   SwapFileInfo *swapFile;
   uint32 nrFreeSlots;
   uint32 j;
   FS_FileHandleID fileHandle = FS_INVALID_FILE_HANDLE;
   uint32 ndx;
   uint32 nrReserved;

   // protect against new swap files being added
   SwapGlobalLock();

   nrReserved = MemSched_TotalSwapReserved();

   // check for running VMs
   if (nrReserved > 0) {
      Warning("%d pages are still reserved, failed to deactivate swap",
              nrReserved);
      SwapGlobalUnlock();
      return VMK_BUSY;
   }

   // Wait for all pending async writes to complete. We dont
   // have to worry about async reads issued from the alloc.c module
   // as we have already checked that no worlds are currently running
   if (swapAsyncIOInfo.nrPendingIO > 0) {
      Warning("swap io transactions still pending, "
              "cannot deactivate swap, try again later");
      SwapGlobalUnlock();
      return VMK_BUSY;
   }

   ndx = SwapGetNdxFromID(fileID);
   if (ndx == SWAP_FILE_INVALID_INDEX) {
      Warning("specified fileID %d is invalid", fileID);
      SwapGlobalUnlock();
      return VMK_BAD_PARAM;
   }

   ASSERT(ndx < swapGlobalInfo.nrSwapFiles);
   ASSERT(swapGlobalInfo.nrSwapFiles > 0);

   swapFile = swapGlobalInfo.swapFileInfo[ndx];
   ASSERT(swapFile);
   Log("Closing swap file %s", swapFile->filePath);

   nrFreeSlots = swapFile->nrFreeSlots;
   fileHandle = swapFile->fileHandle;

   // release memory used for sanity checking
   if (swapDoSanityChecks) {
      SwapDeallocateSlotInfo(swapFile);
   }

   for (j = 0; j < swapFile->numBlocks; ++j) {
      if (swapFile->blocks[j].mapMPN != INVALID_MPN) {
         MemMap_FreeKernelPage(swapFile->blocks[j].mapMPN);
         swapFile->blocks[j].mapMPN = INVALID_MPN;
      }
   }

   SP_CleanupLock(&swapFile->swapFileLock);
   Mem_Free(swapFile->blocks);
   Mem_Free(swapFile);
   swapGlobalInfo.swapFileInfo[ndx] = NULL;

   // Fill the hole created by closing this file
   swapFile = swapGlobalInfo.swapFileInfo[swapGlobalInfo.nrSwapFiles - 1];
   swapGlobalInfo.swapFileInfo[swapGlobalInfo.nrSwapFiles - 1] = NULL;
   swapGlobalInfo.swapFileInfo[ndx] = swapFile;
   // handle case where we removed the last swap file
   if (swapFile) {
      swapFile->fileNdx = ndx;
   }
   swapGlobalInfo.nrSwapFiles--;
   swapGlobalInfo.nextFileNdx = 0;
   // Decrement the total number of free slots
   ASSERT(swapGlobalInfo.totalNrFreeSlots >= nrFreeSlots);
   swapGlobalInfo.totalNrFreeSlots -= nrFreeSlots;

   if (swapGlobalInfo.nrSwapFiles == 0) {
      Swap_ProcUnregister();
      ASSERT(swapGlobalInfo.totalNrFreeSlots == 0);
      swapGlobalInfo.swapIsEnabled = FALSE;
   }

   // Release the swap lock
   SwapGlobalUnlock();

   if (fileHandle != FS_INVALID_FILE_HANDLE) {
      // close swap file
      FSS_CloseFile(fileHandle);
   }
   Log("Close successful");
   return VMK_OK;
}


/*
 *-----------------------------------------------------------------------------
 *
 * SwapGetSwapFile --
 *      Get the swap file associated with this fileNdx. 
 *
 * Results:
 *      Returns the swap file associated with this ndx. 
 *
 * Side effects:
 *      none.
 *-----------------------------------------------------------------------------
 */
static INLINE SwapFileInfo *
SwapGetSwapFile(uint32 fileNdx)
{
   ASSERT(fileNdx < swapGlobalInfo.nrSwapFiles);
   ASSERT(fileNdx < SWAP_WORLDS_MAX);
   if (fileNdx >= SWAP_WORLDS_MAX) {
      Warning("fileNdx = %d is out of range.", fileNdx);
      ASSERT_NOT_IMPLEMENTED(0);
      return NULL;
   }
   ASSERT(swapGlobalInfo.swapFileInfo[fileNdx]->fileNdx == fileNdx);
   return swapGlobalInfo.swapFileInfo[fileNdx];
}

/*
 *-----------------------------------------------------------------------------
 *
 * SwapAtomicDecFreeSlots --
 *
 *      If exactMatch is set, this function finds a file 
 *      with the required number of slots and atomically 
 *      decrements its free slots count.
 *
 *      If exactMatch is not set, finds the next file with free slots 
 *      and atomically decrements the number of slots that will be used.
 *
 *      Uses a simple round robin algorithm to set the next file to search 
 *      for empty slots, more elaborate algorithms can be tried later.
 *
 * Returns:
 *      TRUE, if required slots were found. False otherwise.
 *      Sets fileNdx to the file containing the free slots.
 *      Sets nrSlotsFound to the number of free slots found.
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------
 */
static Bool 
SwapAtomicDecFreeSlots(uint32 reqNrSlots, 
                       Bool exactMatch,
                       uint32 *fileNdx,
                       uint32 *nrSlotsFound) 
{
   uint32 startFileNdx;
   uint32 i;
   SwapGlobalLock();
   startFileNdx = swapGlobalInfo.nextFileNdx++ % swapGlobalInfo.nrSwapFiles;
   SwapGlobalUnlock();

   FOR_ALL_SWAP_FILES(i) {
      SwapFileInfo *swapFileInfo;
      uint32 nextFileNdx;
      nextFileNdx = (startFileNdx + i) % swapGlobalInfo.nrSwapFiles;
      swapFileInfo = swapGlobalInfo.swapFileInfo[nextFileNdx];
      SwapFileInfoLock(swapFileInfo);
      if ((swapFileInfo->nrFreeSlots > 0) &&
          (swapFileInfo->nrFreeSlots >= reqNrSlots || !exactMatch)) {
         *nrSlotsFound = MIN(reqNrSlots, swapFileInfo->nrFreeSlots);
         *fileNdx = nextFileNdx;
         swapFileInfo->nrFreeSlots -= *nrSlotsFound;

         // Decrement the total number of free slots
         SwapDecTotalNumFreeSlots(*nrSlotsFound);

         SwapFileInfoUnlock(swapFileInfo);
         return TRUE;
      } 
      SwapFileInfoUnlock(swapFileInfo);
   }
   *fileNdx = SWAP_FILE_INVALID_INDEX;
   *nrSlotsFound = 0;
   return FALSE;
}
/*
 *-----------------------------------------------------------------------------
 *
 * SwapGetFreeFile --
 *      Finds a file with the required number of free slots. If no such file
 *      exists finds a file with atleast one free slot. Atomically 
 *      decrements the number of free slots in the file.
 *
 * Returns:
 *      The fileNdx of the file containing the free slots.
 *      Sets nrSlotsFound to the number actual number of free slots found
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------
 */
static uint32
SwapGetFreeFile(uint32 reqNrSlots, 
                uint32 *nrSlotsFound)
{
   uint32 fileNdx = SWAP_FILE_INVALID_INDEX;
   // Try to find the exact number of free slots
   if (SwapAtomicDecFreeSlots(reqNrSlots, TRUE, &fileNdx, nrSlotsFound)) {
      return fileNdx;
   }
   // Ok so none of the swap files have the required number of slots, so
   // lets just use a swap file with atleast one free slot.
   if (SwapAtomicDecFreeSlots(reqNrSlots, FALSE, &fileNdx, nrSlotsFound)) {
      return fileNdx;
   }
   *nrSlotsFound = 0;
   return fileNdx;
}

/*
 *-----------------------------------------------------------------------------
 *
 * Swap_IsEnabled --
 *      Determine if swapping is enabled.
 *
 * Returns:
 *      Returns TRUE iff swapping is enabled. False otherwise.
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------
 */
Bool
Swap_IsEnabled(void)
{
   return(swapGlobalInfo.swapIsEnabled);
}


#ifdef VMX86_DEBUG
/*
 *----------------------------------------------------------------------
 *
 * SwapProcFreezeVMRead --
 *
 *      Callback for reading world's swap freeze information
 *
 * Results:
 *      Returns VMK_OK
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
SwapProcReadFreezeVM(Proc_Entry *entry,
                     char *buffer,
                     int  *len) 
{
   World_Handle *world = (World_Handle *)entry->private;
   *len = 0;

   Proc_Printf(buffer, len,
               "VM:      %d (%s) is %s\n",
               world->worldID, world->worldName,
               (SwapGetVmmInfo(world)->freezeVM ? "frozen" : "not frozen"));
   return(VMK_OK);
}

/*
 *-----------------------------------------------------------------------
 *
 * SwapProcFreezeVMWrite --
 *
 *      Callback for writing to world's swap freeze information
 *
 * Results:
 *      Returns VMK_OK
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
SwapProcWriteFreezeVM(Proc_Entry *entry,
                      char *buffer,
                      int  *len) 
{
   World_Handle *world = (World_Handle *)entry->private;
   if (strncmp(buffer, "1", 1) == 0) {
      SwapGetVmmInfo(world)->freezeVM = TRUE;
   } else {
      SwapGetVmmInfo(world)->freezeVM = FALSE;
   }
   return(VMK_OK);
}
#endif // VMX86_DEBUG


/*
 *-----------------------------------------------------------------------------
 *
 * Swap_WorldInit --
 *
 *      Initializes the swapInfo of this world. 
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      none.
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
Swap_WorldInit(World_Handle *world, UNUSED_PARAM(World_InitArgs *args))
{
   if (World_IsVmmLeader(world)) {
      int i;
      Swap_VmmInfo *swapInfo = SwapGetVmmInfo(world);
      SwapPgList *swapPgList = &swapInfo->swapPgList;

      swapInfo->worldState = SWAP_WSTATE_INACTIVE;
      SP_InitLock("swapInfo", &swapInfo->infoLock, SP_RANK_SWAPINFO);
      swapPgList->nrPagesWritten = 0;
      swapPgList->nextWriteNdx   = 0;
      swapPgList->nrPages        = 0;
      swapPgList->nrAsyncWrites  = SWAP_DEFAULT_NR_ASYNC_WRITES;
      swapPgList->length         = SWAP_PFRAME_MAX_SIZE;

      for (i = 0; i < swapPgList->length; ++i) {
         swapPgList->swapPPNList[i] = INVALID_PPN;
         swapPgList->swapMPNList[i] = INVALID_MPN;
      }

      // setup swap action
      swapPgList->getPgListAction = Action_Alloc(world, "BusMemSwap");
      ASSERT(swapPgList->getPgListAction != ACTION_INVALID);
      if (swapPgList->getPgListAction == ACTION_INVALID) {
         return(VMK_FAILURE);
      }
      // debugging
      VMLOG(1, world->worldID, "action index=%d", swapPgList->getPgListAction);
   }

#ifdef VMX86_DEBUG
   if (World_IsVmmLeader(world)) {
      Swap_VmmInfo *swapInfo = SwapGetVmmInfo(world);
      Proc_InitEntry(&swapInfo->swapFreezeVM);
      swapInfo->swapFreezeVM.parent = &world->procWorldDir;
      swapInfo->swapFreezeVM.private = world;
      swapInfo->swapFreezeVM.read = SwapProcReadFreezeVM;
      swapInfo->swapFreezeVM.write = SwapProcWriteFreezeVM;
      Proc_RegisterHidden(&swapInfo->swapFreezeVM, "swapFreezeVM", FALSE);
   }
#endif
   return VMK_OK;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Swap_WorldCleanup --
 *
 *      Closes the checkpoint file of this world.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      none.
 *-----------------------------------------------------------------------------
 */
void
Swap_WorldCleanup(World_Handle *world)
{
#ifdef VMX86_DEBUG
   if (World_IsVmmLeader(world)) {
      Proc_Remove(&SwapGetVmmInfo(world)->swapFreezeVM);
   }
#endif

   if (World_IsVmmLeader(world)) {
      Swap_VmmInfo *swapInfo = SwapGetVmmInfo(world);

      swapInfo->swapPgList.getPgListAction = ACTION_INVALID;

      VMLOG(2, world->worldID, "closing checkpointing file");
      // close the checkpoint file for this world.
      SwapCloseCptFile(world);

      SP_CleanupLock(&swapInfo->infoLock);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * Swap_GetSwapTarget --
 *
 *      Returns the number of pages that need to be swapped from this world
 *
 * Returns:
 *      Number of pages that will be swapped for this world
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
uint32
Swap_GetSwapTarget(World_Handle *world)
{
   return (SwapGetVmmInfo(world)->nrPagesToSwap);
}

/*
 *----------------------------------------------------------------------
 *
 * Swap_SetSwapTarget --
 *      Sets the number of pages to swap for this world
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      May cause this VM to start swapping pages.
 *
 *----------------------------------------------------------------------
 */
void
Swap_SetSwapTarget(World_Handle *world, uint32 nrPages)
{
   ASSERT(World_IsVmmLeader(world));
   ASSERT(nrPages == 0 || Swap_IsEnabled());
   SwapGetVmmInfo(world)->nrPagesToSwap = nrPages;
   SwapStartSwapping(world);
}

/*
 *-----------------------------------------------------------------------------
 *
 * SwapTestAndIncAsyncIO --
 *      This is the main control mechanism to limit the number of outstanding
 *      async io operations that can be performed by the various VMs.
 *      maxNrIO represents the maximumn number of outstanding
 *      async IO requests that are permitted.
 *
 * Returns:
 *      returns TRUE if async IO can be done.
 *      returns FALSE otherwise.
 *
 * Side effects:
 *      none
 *
 *-----------------------------------------------------------------------------
 */
static INLINE Bool
SwapTestAndIncAsyncIO(void)
{
   Bool rtn;
   SwapAsyncIOLock();
   rtn = (swapAsyncIOInfo.nrPendingIO < swapAsyncIOInfo.maxNrIO); 
   if (rtn) {
      swapAsyncIOInfo.nrPendingIO++;
      ASSERT(swapAsyncIOInfo.nrPendingIO <= swapAsyncIOInfo.maxNrIO);
   }
   SwapAsyncIOUnlock();
   return(rtn);
}

/*
 *-----------------------------------------------------------------------------
 *
 * SwapTestAsyncIO --
 *      Determines if it is ok to do an Async IO.
 *
 * Results:
 *      returns TRUE if atleast one token is free otherwise returns FALSE
 *
 *
 * Side effects:
 *      none
 *
 *-----------------------------------------------------------------------------
 */
static INLINE Bool
SwapTestAsyncIO(void)
{
   Bool rtn;
   SwapAsyncIOLock();
   rtn = (swapAsyncIOInfo.nrPendingIO < swapAsyncIOInfo.maxNrIO);
   SwapAsyncIOUnlock();
   return rtn;
}

/*
 *-----------------------------------------------------------------------------
 *
 * SwapTestAndSleepAsyncIO --
 *      Determines if it is ok to do an Async IO. If no more async ios can be
 *      done sleeps, waiting for async ios to complete.
 *
 *      NOTE:
 *      If force is set then go to sleep irrespective of the number
 *      of outstanding Async IOs.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      Caller will be blocked till more async ios can be done.
 *
 *-----------------------------------------------------------------------------
 */
static void
SwapTestAndSleepAsyncIO(Bool force)
{
   ASSERT_HAS_INTERRUPTS();
   SwapAsyncIOLock();
   if (force) {
      // Go to sleep and acquire lock again
      SwapAsyncIOWaitLock();
   } else {
      while (swapAsyncIOInfo.nrPendingIO >= swapAsyncIOInfo.maxNrIO) {
         // Go to sleep and acquire lock again
         SwapAsyncIOWaitLock();
      } 
   }
   SwapAsyncIOUnlock();
}

/*
 *----------------------------------------------------------------------
 * SwapDecAsyncIO --
 *      Atomically Decrement the number of pending async ios.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      Wakeup VMs waiting for async io to complete
 *      none.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
SwapDecAsyncIO(void)
{
   SwapAsyncIOLock();
   ASSERT(swapAsyncIOInfo.nrPendingIO >= 1);
   swapAsyncIOInfo.nrPendingIO--;
   // Wakeup VMs waiting for pending async io to complete
   SwapAsyncIOWakeup();
   SwapAsyncIOUnlock();
}


/*
 *-----------------------------------------------------------------------------
 *
 * SwapTestAndSleepSwapDone --
 *
 *      Sleep waiting for swap to complete 
 * 
 * Results:
 *      none.
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------
 */
static void
SwapTestAndSleepSwapDone(UNUSED_PARAM(World_ID worldID), Swap_VmmInfo *swapInfo)
{
   volatile Swap_VmmInfo *updatedSwapInfo = (volatile Swap_VmmInfo *)swapInfo;
   ASSERT_HAS_INTERRUPTS();
   SwapInfoLock(swapInfo);
   while (updatedSwapInfo->worldState != SWAP_WSTATE_SWAP_DONE) {
      SwapInfoWaitLock(swapInfo, CPUSCHED_WAIT_SWAP_DONE);
   }
   SwapInfoUnlock(swapInfo);
   ASSERT(updatedSwapInfo->worldState == SWAP_WSTATE_SWAP_DONE);
}

/*
 *----------------------------------------------------------------------
 *
 * SwapRequestPages --
 *
 *	Handler invoked by swapper to post the monitor action. 
 *	Post an action to all vcpus to start the swap out process.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      Will result in call to Swap_SwapGetNumPages() once action
 *      is processed by the monitor.
 *
 *----------------------------------------------------------------------
 */
static void
SwapRequestPages(World_Handle *world)
{
   uint32 i;
   World_VmmGroupInfo *vmmGroup = World_VMMGroup(world);
   SwapPgList *memSwapPgList = &SwapGetVmmInfo(world)->swapPgList;

   ASSERT(memSwapPgList->getPgListAction != ACTION_INVALID);
   ASSERT(World_IsVmmLeader(world));

   if (memSwapPgList->getPgListAction != ACTION_INVALID) {
      for (i = 0; i < vmmGroup->memberCount; i++) {
         World_ID memberID;
         World_Handle *member;

         memberID = vmmGroup->members[i]; 
         member = World_Find(memberID);
         if (member) {
            Action_Post(member, memSwapPgList->getPgListAction); 
         }
         World_Release(member);
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * SwapMoreSwappingReqd
 *      
 *      Find out if more pages need to be swapped.
 *     
 *
 * Results:
 *      TRUE if more swapping is required, FALSE otherwise.
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
SwapMoreSwappingReqd(World_Handle *world, uint32 *nrRequestPages) 
{
   MemSchedVmmUsage *usage = MemSched_ClientVmmUsage(world);
   uint32 swapped = usage->swapped;
   uint32 nrPagesToSwap = SwapGetVmmInfo(world)->nrPagesToSwap;

   // initialize return value
   *nrRequestPages = 0;

   if (Alloc_IsCheckpointing(Alloc_AllocInfo(world))) {
      return(FALSE);
   }
   if (swapped < nrPagesToSwap) {
      *nrRequestPages = MIN((nrPagesToSwap - swapped), SWAP_PFRAME_MAX_SIZE);
      return(TRUE);
   } else {
      *nrRequestPages = 0;
      return(FALSE);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * Swap_BlockUntilReadyToSwap --
 *
 *      Block the VM if we are in the low memory state and we are
 *      waiting for earlier unfinished async io to finish.
 *
 *      Note: This is needed to handle the following scenario.
 *      If we are not in the LOW memory state we will issue 
 *      async ios and return to the monitor. The system then goes into
 *      a LOW memory state, but we wont start swapping from this VM until
 *      the async writes finish, which could potentially be too late as 
 *      this VM can in the mean time consume enough MPNs to take the
 *      whole system down. Hence what we do here is block this VM
 *      until the existing async writes finish and then let it run so
 *      that it can process the swap list request.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      The VM will be blocked till we finish the async IO.
 *
 *-----------------------------------------------------------------------------
 */
void
Swap_BlockUntilReadyToSwap(World_Handle *inWorld) 
{
   uint32 numPagesToSwap;
   World_Handle *world = World_GetVmmLeader(inWorld);
   Swap_VmmInfo *swapInfo = SwapGetVmmInfo(world);
   ASSERT(Swap_IsEnabled());
   if (swapInfo->worldState == SWAP_WSTATE_SWAP_ASYNC &&
       SwapMoreSwappingReqd(world, &numPagesToSwap)) {
      volatile Swap_VmmInfo *volSwapInfo = (volatile Swap_VmmInfo *)swapInfo;
      ASSERT_HAS_INTERRUPTS();
      SwapInfoLock(swapInfo);
      while (volSwapInfo->worldState == SWAP_WSTATE_SWAP_ASYNC) {
         SwapInfoWaitLock(swapInfo, CPUSCHED_WAIT_SWAP_ASYNC);
      }
      if (vmx86_debug) {
         static uint32 throttle = 0;
         if (throttle++ % 10000 == 0) {
            VmLog(inWorld->worldID,"waking after async writes finished");
         }
      }
      SwapInfoUnlock(swapInfo);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * SwapShouldSwapBlock --
 *
 *      Wrapper function to call MemSched_ShouldSwapBlock
 *
 * Results:
 *      TRUE if VM should block until swapTarget is met. FALSE otherwise.
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
SwapShouldSwapBlock(World_Handle *world)
{
   Swap_VmmInfo *swapInfo;
   MemSchedVmmUsage *usage = MemSched_ClientVmmUsage(world);
   swapInfo = SwapGetVmmInfo(world);
   return(MemSched_ShouldSwapBlock(swapInfo->nrPagesToSwap, usage->swapped));
}

/*
 *----------------------------------------------------------------------
 *
 * SwapStartSwapping --
 *
 *      Start swapping pages for this VM, only if more pages need to swapped.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      The swapper engine may get started.
 *
 *----------------------------------------------------------------------
 */
static void
SwapStartSwapping(World_Handle *world)
{
   uint32 nrRequestPages;
   if (SwapMoreSwappingReqd(world, &nrRequestPages)) {
      // note the time when the last swap action was set
      MemSched_SetSwapReqTimeStamp(world, Timer_SysUptime());
      // post monitor action
      SwapRequestPages(world);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * Swap_GetNumPagesToSwap --
 *      
 *      Performs 2 functions for the monitor.
 *      1. To check if some other vcpu has processed this action.
 *      2. To get the number of pages to swap if no other vcpu has processed
 *      this action.
 *
 *      The function sets "numPages" to 0 if another vcpu has processed this
 *      action or if no more pages need to be swapped, else "numPages" is
 *      set to the number of pages to swap.
 *
 * Results:
 *      VMK_OK on success; VMK_FAILURE if swap is not enabled.
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------
 */
VMKERNEL_ENTRY
Swap_GetNumPagesToSwap(DECLARE_2_ARGS(VMK_SWAP_NUM_PAGES,
                                      uint32 *, numPages,
                                      Bool *, tryCOW))
{
   World_Handle *groupLeader;
   Swap_VmmInfo *swapInfo;
   PROCESS_2_ARGS(VMK_SWAP_NUM_PAGES,
                  uint32 *, numPages,
                  Bool *, tryCOW);

   //initialize return values
   *numPages = 0;
   if (CONFIG_OPTION(MEM_SWAP_COW_PAGES)) {
      *tryCOW = TRUE;
   } else {
      *tryCOW = FALSE;
   }

   ASSERT(World_IsVMMWorld(MY_RUNNING_WORLD));
   groupLeader = MY_VMM_GROUP_LEADER;
   swapInfo = SwapGetVmmInfo(groupLeader);

   // If swap is not enabled, this function shouldn't be called
   if (!Swap_IsEnabled()) {
      VmWarn(groupLeader->worldID, "swap not enabled");
      return VMK_FAILURE;
   }

   // inform mem schedular that VM is alive and responding.
   MemSched_SetSwapReqTimeStamp(groupLeader, 0);

   // update world's swap state
   SwapInfoLock(swapInfo);
  
   if (swapInfo->worldState == SWAP_WSTATE_INACTIVE) {
      if (SwapMoreSwappingReqd(groupLeader, numPages)) {
         ASSERT(*numPages > 0); 
         ASSERT(*numPages <= SWAP_PFRAME_MAX_SIZE);
         ASSERT(*numPages <= swapInfo->swapPgList.length);
         swapInfo->worldState = SWAP_WSTATE_LIST_REQ;
         VMLOG(2, groupLeader->worldID, "requested %u pages", *numPages);
         // note the time when the last swap request was sent
         MemSched_SetSwapReqTimeStamp(groupLeader, Timer_SysUptime());
      } else {
         ASSERT(*numPages == 0); 
         // update stats
         SwapResetAllocDuringSwap(swapInfo);
      }
   } else {
      // some other vcpu has already processed this swap action,
      // do not request any more pages
      *numPages = 0;
   }

   SwapInfoUnlock(swapInfo);
   return VMK_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 * Swap_SwapOutPages  --
 *      Wrapper function for SwapSwapOutPages.
 *
 * Results:
 *      nrRequestPages has the number of pages to be swapped out next time
 *
 *      VMK_OK or VMK_CONTINUE_TO_SWAP on success, other error codes mean
 *      FATAL failure for caller world. The caller world should check for
 *      the return code and coredump.
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------
 */
VMKERNEL_ENTRY
Swap_SwapOutPages(DECLARE_4_ARGS(VMK_SWAPOUT_PAGES, 
                                int, nrPagesRecvd,
                                BPN *, bpnList,
                                uint32 *, nrRequestPages,
                                Bool *, tryCOW))
{
   World_Handle *groupLeader;
   VMK_ReturnStatus status;
   
   PROCESS_4_ARGS(VMK_SWAPOUT_PAGES,
                  int, nrPagesRecvd,
                  BPN *, bpnList,
                  uint32 *, nrRequestPages,
                  Bool *, tryCOW);

   ASSERT(World_IsVMMWorld(MY_RUNNING_WORLD));
   groupLeader = MY_VMM_GROUP_LEADER;

   /*
    * Simulate the case where the vm gets stuck. 
    */
   if (SwapGetVmmInfo(groupLeader)->freezeVM && vmx86_debug) {
      volatile Bool *freezeVM = (volatile Bool *)&SwapGetVmmInfo(groupLeader)->freezeVM;
      ASSERT(SwapGetVmmInfo(groupLeader)->worldState == SWAP_WSTATE_LIST_REQ);
      while (*freezeVM) {
         static uint32 throttle = 0;
         if (throttle++ % 100 == 0) {
            VmWarn(groupLeader->worldID, "Simulating blocked vm, sleeping");
         }
         // Sleep, this works even for SMP vms as this function
         // is called in a stop callback in the monitor and
         // thus other vcpus must already be blocked/sleeping
         CpuSched_Sleep(5000);
      }
   }
   status = SwapSwapOutPages(groupLeader, nrPagesRecvd, 
                             bpnList, nrRequestPages, tryCOW);

   if (status == VMK_CONTINUE_TO_SWAP) {
      ASSERT(*nrRequestPages > 0);
      ASSERT(SwapGetVmmInfo(groupLeader)->worldState == SWAP_WSTATE_LIST_REQ);
      // note the time when the last swap request was sent
      MemSched_SetSwapReqTimeStamp(groupLeader, Timer_SysUptime());
   } else {
      ASSERT(*nrRequestPages == 0);
   }

   return status;
}

/*
 *-----------------------------------------------------------------------------
 *
 * SwapCanSwapPFrame --
 *      Check whether the given frame is a candidate to be swapped out
 *      We *do not* swap 
 *        o shared pages
 *        o invalid pages
 *        o pages in the alloc / kseg caches
 *      
 *      Requires that the alloc lock be held by the caller
 *
 * Results:
 *      TRUE if the page can be swapped out.
 *      FALSE if the page cannot be swapped out.
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------
 */
static Bool
SwapCanSwapPFrame(World_Handle *world,
                  PPN ppn, 
                  Bool frameValid, 
                  AllocPFrameState frameState,
                  uint16 framePinCount) 
{
   if (!frameValid) {
      return FALSE ;
   }

   // fail if page is being used by COS
   if (framePinCount > 0) {
      return FALSE;
   }

   if ((Alloc_IsCached(world, ppn)) || 
        ((numPCPUs > 1) && Kseg_CheckRemote(world->worldID, ppn))) {
      return FALSE ;
   }

   // Since we can receive invalid pages from the vmm 
   // these invalid pages could be already in one
   // of the swap states
   if (Alloc_PFrameStateIsSwap(frameState)) {
      return FALSE;
   }

   ASSERT(!Alloc_PFrameStateIsSwap(frameState));
   ASSERT(frameValid);
   return TRUE;
}

/*
 *-----------------------------------------------------------------------------
 *
 * SwapCheckSort --
 *      Checks if the list of SwapPFrames are sorted. Used for debugging
 *      only
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      Will assert fail if the list is not sorted. This is used only for
 *      debugging
 *
 *-----------------------------------------------------------------------------
 */
static INLINE void
SwapCheckSort(BPN *inSwapBPNList,
              uint32 nrPages)
{
   uint32 i;
   for (i = 1; i < nrPages; ++i) {
      if (inSwapBPNList[i] < inSwapBPNList[i-1]) {
         ASSERT(0);
      }
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * SwapShellSort --
 *      implements the shell sort algorithm to sort the list of SwaPFrames
 *
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE void
SwapShellSort(BPN *inSwapBPNList,
              uint32 nrPages)
{
   uint32 inc;

   for (inc = nrPages/2; inc > 0; inc /= 2) {
      uint32 i;
      if ((inc != 0) && ((inc % 2) == 0)) {
         inc++;
      }
      for ( i = inc; i < nrPages; ++i) {
         BPN tmp;
         uint32 j;
         tmp = inSwapBPNList[i];
         for(j = i; j >= inc; j -= inc) {
            if (tmp < inSwapBPNList[j - inc]) {
               inSwapBPNList[j] = inSwapBPNList[j - inc];
            } else {
               break;
            }
         }
         inSwapBPNList[j] = tmp;
      }
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * SwapShouldSwapPPN --
 *
 *      Determine if this PPN can be swapped, if yes then set "rtnMPN" to the MPN
 *      of this PPN.
 *
 * Results:
 *      TRUE iff if the given PPN can be swapped. FALSE otherwise
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------
 */
static Bool
SwapShouldSwapPPN(World_Handle *world,
                  PPN ppn,
                  Alloc_PFrame *allocPFrame,
                  MPN *rtnMPN)
{
   MPN frameMPN;
   uint32 frameIndex;
   uint16 framePinCount;
   AllocPFrameState frameState;
   Bool frameValid;
   MemSchedVmmUsage *usage = MemSched_ClientVmmUsage(world);

   *rtnMPN = INVALID_MPN;

retry:
   ASSERT_NOT_IMPLEMENTED(allocPFrame);

   frameMPN = Alloc_PFrameGetMPN(allocPFrame);
   frameValid = Alloc_PFrameIsValid(allocPFrame);
   frameIndex = Alloc_PFrameGetIndex(allocPFrame);
   frameState = Alloc_PFrameGetState(allocPFrame);
   framePinCount = Alloc_PFrameGetPinCount(allocPFrame);

   // should we swap this page ? 
   if (!SwapCanSwapPFrame(world, ppn, frameValid, frameState, framePinCount)) {
      return(FALSE);
   }
   ASSERT(ppn != INVALID_PPN);

   // If the frame is a cow frame, break sharing and
   // set up the page for swapping
   if (Alloc_PFrameStateIsCOW(frameState)) {
      uint32 count;
      VMK_ReturnStatus status;
      uint64 key;

      status = PShare_LookupByMPN(frameMPN, &key, &count);
      ASSERT(status == VMK_OK);
      if (status != VMK_OK) {
         VmWarn(world->worldID, "pshare lookup failed: mpn=0x%x", frameMPN);
         return(FALSE);
      }
      // Check if the ref count on this shared page is low 
      // enough to qualify for a swap out
      if (count > CONFIG_OPTION(MEM_SWAP_MAX_COW_REF_COUNT)) {
         return(FALSE);
      }
      // break sharing
      //
      /*
       * We do swapping in the context of the monitor, hence it is appropriate
       * that we call Alloc_PageFaultWrite with source == ALLOC_FROM_MONITOR,
       * the other more important reason is that Alloc_PageFaultWrite will try
       * to break sharing and if source != ALLOC_FROM_MONITOR we post an P2M
       * update action. We *do not* want to post this action.
       */
      Alloc_Unlock(world);
      status = Alloc_PageFaultWrite(world, ppn, &frameMPN, ALLOC_FROM_MONITOR);
      Alloc_Lock(world);
      if (status != VMK_OK) {
         VmWarn(world->worldID, "failed to break COW sharing mpn=0x%x", frameMPN);
         return(FALSE);
      }
      // Slightly inaccurate, but should be ok for stats
      SwapGetVmmInfo(world)->stats.numCOWPagesSwapped++;
      // retry because frame state could have changed since we
      // dropped alloc lock
      goto retry;
   } else if (Alloc_PFrameStateIsCOWHint(frameState)) {
      // If the frame is a cow hint frame, remove the cow hint
      // and set up the page for swapping
      VMK_ReturnStatus status;
      uint64    key;
      World_ID  hintWorld;
      PPN       hintPPN;

      // sanity check
      ASSERT(frameMPN != INVALID_MPN);

      // lookup hint
      status = PShare_LookupHint(frameMPN, &key, &hintWorld, &hintPPN);
      ASSERT(status == VMK_OK);
      if (status != VMK_OK) {
         VmWarn(world->worldID, "hint lookup failed: mpn 0x%x", frameMPN);
         return(FALSE);
      }

      // remove hint
      status = PShare_RemoveHint(frameMPN, hintWorld, hintPPN);
      ASSERT(status == VMK_OK);
      if (status != VMK_OK) {
         VmWarn(world->worldID, "hint remove failed: mpn 0x%x", frameMPN);
         return(FALSE);
      }

      Alloc_PFrameSetRegular(world, ppn, allocPFrame, frameMPN);
      // update usage info
      usage->cowHint--;
   } 

   ASSERT(frameMPN != INVALID_MPN);
   if (frameMPN == INVALID_MPN) {
      VmWarn(world->worldID, "** PPN %d does not have a valid MPN **", ppn);
      return(FALSE);
   }

   // success
   *rtnMPN = frameMPN;
   return(TRUE);
}


/*
 *-----------------------------------------------------------------------------
 *
 * SwapContinueAfterEmptyList --
 *      
 *      Determine if we should continue swapping after we received
 *      an empty list from the VM
 *
 * Results:
 *      VMK_CONTINUE_TO_SWAP iff we should continue to swap. VMK_OK otherwise
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
SwapContinueAfterEmptyList(World_Handle *world, 
                           uint32 *nrRequestPages,
                           Bool *tryCOW)
{
   Swap_VmmInfo *swapInfo = SwapGetVmmInfo(world);
   SwapInfoLock(swapInfo);
   *tryCOW = FALSE;
   switch (swapInfo->worldState) {
      case SWAP_WSTATE_LIST_REQ:
         if (SwapShouldSwapBlock(world) && SwapMoreSwappingReqd(world, nrRequestPages)) {
            SwapInfoUnlock(swapInfo);
            // Give the other VMs a chance to run..as we dont
            // seem to be getting any pages to swap 
            CpuSched_YieldThrottled();
            swapInfo->continueSwap = TRUE;
            if (MemSched_MemoryIsLow()) {
               // We dont seem to be getting pages to swap so try COW pages
               // if we are low on memory
               *tryCOW = TRUE;
            }
            return(VMK_CONTINUE_TO_SWAP);
         } else {
            swapInfo->worldState = SWAP_WSTATE_INACTIVE;
            SwapInfoUnlock(swapInfo);
            return(VMK_OK);
         }
         break;
      default:
         // We should not be in any other state.
         ASSERT_NOT_IMPLEMENTED(0);
   }
   NOT_REACHED();
   SwapInfoUnlock(swapInfo);
   return(VMK_OK);
}

/*
 *-----------------------------------------------------------------------------
 *
 * SwapContinueAfterWrite --
 *      
 *      Determine if we should continue swapping after we have swapped
 *      some pages 
 *
 * Results:
 *      VMK_CONTINUE_TO_SWAP iff we should continue to swap. VMK_OK otherwise
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
SwapContinueAfterWrite(World_Handle *world, 
                       uint32 *nrRequestPages)
{
   Swap_VmmInfo *swapInfo = SwapGetVmmInfo(world);
   SwapInfoLock(swapInfo);
   switch (swapInfo->worldState) {
      case SWAP_WSTATE_SWAPPING:
         SwapInfoUnlock(swapInfo);
         ASSERT_NOT_IMPLEMENTED(0);
         return(VMK_OK);
         break;
      case SWAP_WSTATE_SWAP_ASYNC:
         // In this case the AsynWriteCallback function
         // will (if required) start swapping when all the
         // pending async writes finish
         SwapInfoUnlock(swapInfo);
         return(VMK_OK);
         break;
         // we received 0 pages to swap from the VMM
      case SWAP_WSTATE_SWAP_DONE:
         if (SwapShouldSwapBlock(world) && SwapMoreSwappingReqd(world, nrRequestPages)) {
            swapInfo->worldState = SWAP_WSTATE_LIST_REQ;
            SwapInfoUnlock(swapInfo);
            swapInfo->continueSwap = TRUE;
            return(VMK_CONTINUE_TO_SWAP);
         } else {
            swapInfo->worldState = SWAP_WSTATE_INACTIVE;
            SwapInfoUnlock(swapInfo);
            return(VMK_OK);
         }
         break;
      default:
         ASSERT(swapInfo->worldState == SWAP_WSTATE_INACTIVE);
         // It is possible to be in SWAP_WSTATE_INACTIVE state
         // at this point because, consider the scenario where 
         // we returned from SwapWritePages with 
         // SWAP_WSTATE_SWAP_ASYNC and by the time this function 
         // was called AsyncWriteCallback came along and updated
         // the state to SWAP_WSTATE_INACTIVE
         SwapInfoUnlock(swapInfo);
         return(VMK_OK);
   }
   NOT_REACHED();
}

/*
 *-----------------------------------------------------------------------------
 *
 * SwapSwapOutPages --
 *      Goes through the list of pages received from the VMM and creates a new
 *      list consisting of only those pages that are good candidates to be
 *      swapped out. Mark the selected pages as *being* swapped.
 *         The selected pages are removed from the Alloc Cache, Kseg Cache, 
 *      remote Kseg Caches, host page tables and the host TLB.
 *         Write the selected pages to disk
 *
 * Results:
 *      VMK_FAILURE on error.
 *
 * Side effects:
 *      Flushes the TLB on the host if any of the pages to be swapped out is
 *      being used by the host.
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus 
SwapSwapOutPages(World_Handle *world, 
                 uint32 nrPagesRecvd,
                 BPN *inSwapBPNList,
                 uint32 *nrRequestPages,
                 Bool *tryCOW) 
{
   uint32 i, nrPages = 0;
   Swap_VmmInfo *swapInfo;
   PPN *swapPPNList;

   // Initialize return values
   *nrRequestPages = 0;
   *tryCOW = FALSE;

   swapInfo = SwapGetVmmInfo(world);
   swapPPNList = swapInfo->swapPgList.swapPPNList;

   // This is inconsistent with vmkernel swap state: caller should core dump.
   if (UNLIKELY(swapInfo->worldState != SWAP_WSTATE_LIST_REQ)) {
      VmWarn(world->worldID, "swap not in request state %d", swapInfo->worldState);
      return VMK_FAILURE;
   }
   // If swap is not enabled, this function shouldn't be called
   if (!Swap_IsEnabled()) {
      VmWarn(world->worldID, "swap not enabled, pages=%d", nrPagesRecvd);
      return VMK_FAILURE;
   }

   // reset flag
   swapInfo->continueSwap = FALSE;

   // inform mem schedular that VM is alive and responding.
   MemSched_SetSwapReqTimeStamp(world, 0);

   ASSERT(swapInfo->swapPgList.length >= nrPagesRecvd);

   SwapResetAllocDuringSwap(swapInfo); 

   if (Alloc_IsCheckpointing(Alloc_AllocInfo(world))) {
      SwapInfoLock(swapInfo);
      swapInfo->worldState = SWAP_WSTATE_INACTIVE;
      SwapInfoUnlock(swapInfo);
      if (Alloc_AllocInfo(world)->duringCheckpoint) {
         World_Panic(world, "World is in checkpoint state while swapping in progress");
      }
      return(VMK_OK);
   }

   if (nrPagesRecvd == 0) {
      VMLOG(1,world->worldID, "nrPagesRecvd = <%d>", nrPagesRecvd);
      return(SwapContinueAfterEmptyList(world, nrRequestPages, tryCOW));
   }

   SwapShellSort(inSwapBPNList, nrPagesRecvd);
   if (SWAP_DEBUG) {
      SwapCheckSort(inSwapBPNList, nrPagesRecvd);
   }

   VMLOG(1, world->worldID, 
         "Received swap list for world <%d>: nrActual Pages<%d>",
         world->worldID, nrPagesRecvd);

   // Initialize the Swap page list
   swapInfo->swapPgList.nrPages = 0;
   swapInfo->swapPgList.nrPagesWritten = 0;
   swapInfo->swapPgList.nextWriteNdx = 0;

   for (i = 0; i < nrPagesRecvd; ++i) {
      Alloc_PFrame *allocPFrame;
      KSEG_Pair *dirPair;
      MPN frameMPN;
      PPN inPPN;
      if (!Alloc_IsMainMemBPN(world, inSwapBPNList[i])) {
         VmWarn(world->worldID, "Tried to swap non-mainmem bpn %x", 
                inSwapBPNList[i]);
         continue;
      }
      inPPN = Alloc_BPNToMainMemPPN(world, inSwapBPNList[i]);

      // We can get duplicate PPNs from the VMM, make sure
      // that we do not have duplicates in our list.
      // Since we sort inSwapPFrame we need to check only
      // the last PPN that was copied to the swapPgList
      if (nrPages > 0) {
         if (inPPN == swapPPNList[nrPages - 1]) {
            VMLOG(1, world->worldID, "Ignoring duplicate ppn(0x%x)", inPPN);
            inSwapBPNList[i] = INVALID_BPN;
            continue;
         }
      }

      // Make sure that this is the only function
      // modifying the flags in the allocPFrame
      Alloc_Lock(world);

      allocPFrame = SwapMapAllocPFrame(world, inPPN, &dirPair);
      if (!allocPFrame) {
         VmWarn(world->worldID, "Failed to get (Alloc_PFame *) for  PPN <0x%x>", 
                inPPN);
         Alloc_Unlock(world);
         inSwapBPNList[i] = INVALID_BPN;
         ASSERT(0);
         continue;
      }

      if (!SwapShouldSwapPPN(world, inPPN, allocPFrame, &frameMPN)) {
         SwapReleaseAllocPFrame(dirPair);
         Alloc_Unlock(world);
         inSwapBPNList[i] = INVALID_BPN;
         continue;
      }

      ASSERT(frameMPN != INVALID_MPN);

      // It is ok to swap this page
      Alloc_InvalidateCache(world, inPPN);
      Kseg_InvalidatePtr(world, inPPN);

      ASSERT(nrPages < swapInfo->swapPgList.length);

      // Copy the selected pages to the per world SwapInfo structure
      swapPPNList[nrPages] = inPPN;

      // store the mpn for posterity
      swapInfo->swapPgList.swapMPNList[nrPages] = frameMPN; 

      // Update the allocPFrame flags 
      Alloc_PFrameSetValid(allocPFrame);
      Alloc_PFrameSetState(allocPFrame,  ALLOC_PFRAME_SWAP_OUT);

      // count the number of pages to write to disk 
      ++nrPages ;
      swapInfo->swapPgList.nrPages = nrPages;

      SwapReleaseAllocPFrame(dirPair);
      Alloc_Unlock(world);

      if (numPCPUs > 1) {
         // Flush the PPN to MPN mapping on all remote cpus
         Kseg_FlushRemote(world->worldID, inPPN);
      }
   }

   ASSERT(swapInfo->swapPgList.nrPages <= swapInfo->swapPgList.length);
   VMLOG(1, world->worldID, "Received swap list : nrPagesRecvd<%d> , nrPages<%d>", 
         nrPagesRecvd, nrPages);

   if (nrPages > 0) {

      // Write out the pages to disk
      SwapWritePages(world, swapInfo);
      VMLOG(2, world->worldID, "Finished swapping: nrPages<%d>", nrPages);
      return(SwapContinueAfterWrite(world, nrRequestPages));
   } else {
      VMLOG(1,world->worldID, "nrPages = <%d>", nrPages);
      return(SwapContinueAfterEmptyList(world, nrRequestPages, tryCOW));
   }

}

/*
 *-----------------------------------------------------------------------------
 *
 * SwapUWAsyncWriteCallback --
 *
 *      Invoke the userworld memory module to set the page as swapped out.
 *      If the write is not successful or the page shouldn't be swapped out,
 *      free the swap file slot. 
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      free the MPN of the page if it  is untouched.
 *      free the File slot if the page was faulted in or deallocated
 *
 *-----------------------------------------------------------------------------
 */
static void 
SwapUWAsyncWriteCallback(Async_Token *token) 
{
   World_Handle *world;
   World_ID worldID = INVALID_WORLD_ID;
   Bool writeFailed = FALSE, swapped;
   MPN mpn;
   PPN ppn;
   SwapFileInfo *swapFileInfo;
   UWSwapToken *swapToken = (UWSwapToken *)token->clientData;
   SwapFileSlot swapFileSlot;
   uint32 reqNum;

   mpn = swapToken->mpn;
   ppn = swapToken->ppn;
   swapFileSlot = swapToken->swapFileSlot;
   reqNum = swapToken->reqNum;
   ASSERT(swapToken->token == token);
   worldID = swapToken->worldID;

   // Free the memory used by the swapToken
   Mem_Free(swapToken);

   swapFileInfo = SwapGetSwapFile(swapFileSlot.slot.fileNdx);

   world = World_Find(worldID);
   if (!world) {
      WarnVmNotFound(worldID); 
      // free up the file slots 
      SwapFreeFileSlots(swapFileSlot.slot.slotNum, 1, swapFileInfo);
      Async_ReleaseToken(token);

      // Indicate that this async io is complete
      // *DO* only after World_Find, refer to the comment 
      // later in this function for details
      SwapDecAsyncIO();
      return ;
   }
   ASSERT(World_IsGroupLeader(world));

   // Mark that this async io as complete
   // *only* after we have first done a World_Find 
   // otherwise we have to worry about the following race.
   //
   // o  We do SwapDecAsyncIO before World_Find
   // o  Meanwhile Swap_Deactivate sees no outstanding IO and
   //    clears swapFileInfo
   // o  And to make things worse world is destroyed before 
   //    we do a World_Find so we fail and then when we try to do 
   //    SwapFreeFileSlots we get a pretty looking exception 14 PSOD
   SwapDecAsyncIO();

   if (((SCSI_Result *)token->result)->status != 0) {
      VMLOG(1, worldID, "AsynWrite failed for world, scsiStatus = 0x%x", 
            ((SCSI_Result *)token->result)->status);
      writeFailed = TRUE;
   }

   ASSERT(World_IsUSERWorld(world));
   swapped = User_MarkSwapPage(world, reqNum, writeFailed, swapFileSlot.value, ppn, mpn);
   if (!swapped) {
      VMLOG(1, worldID, "not swapped free file slot %x\n", swapFileSlot.slot.slotNum);
      SwapFreeFileSlots(swapFileSlot.slot.slotNum, 1, swapFileInfo);
   }

   World_Release(world);
   Async_ReleaseToken(token);
}


/*
 *-----------------------------------------------------------------------------
 *
 * Swap_UWSwapOutPage --
 *      Allocate slot in the swap file for the page and issue disk 
 *      write request.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      The callback function will be called once the write finishes.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
Swap_UWSwapOutPage(World_Handle *world,  // IN: the world
                   uint32 reqNum,        // IN: req number in the swap req list
                   PPN ppn,              // IN: ppn of the page
                   MPN mpn,              // IN: mpn of the page
                   uint32 *swapSlotNr)   // OUT: swap slot number of the page
{
   uint32 nrSlots;
   VMK_ReturnStatus status = VMK_OK;
   uint32 swapFileNdx, startSlotNum;
   UWSwapToken *swapToken;
   Async_Token *token;
   SG_Array *sgArr;
   SwapFileInfo *swapFileInfo;

   if (!SwapTestAndIncAsyncIO()) {
      return(VMK_MAX_ASYNCIO_PENDING);
   }

   // get a slot from a swap file
   nrSlots = SwapGetFileSlots(1, &swapFileNdx, &startSlotNum);
   if (nrSlots <= 0) {
      SwapDecAsyncIO();
      return(VMK_NOT_ENOUGH_SLOTS);
   }

   swapFileInfo = SwapGetSwapFile(swapFileNdx);

   VMLOG(1, world->worldID, "ppn 0x%x startSlotNum(0x%x)", ppn, startSlotNum);

   sgArr = (SG_Array *)Mem_Alloc(SG_ARRAY_SIZE(nrSlots));
   ASSERT(sgArr != NULL);
   if (!sgArr) {
      VmWarn(world->worldID, "Cannont allocate sgArr");
      SwapDecAsyncIO();
      return VMK_NO_MEMORY;
   }
   sgArr->length = nrSlots;
   sgArr->addrType = SG_MACH_ADDR;

   ASSERT(mpn != INVALID_MPN);
   ASSERT(mpn <= MemMap_GetLastValidMPN()); 

   sgArr->sg[0].length = PAGE_SIZE;
   sgArr->sg[0].addr = MPN_2_MA(mpn);
   sgArr->sg[0].offset = SWAP_SLOT_2_OFFSET(startSlotNum);

   token = Async_AllocToken(0);
   ASSERT(token);
   if (token == NULL) {
      VmWarn(world->worldID, "Alloc token failed");
      Mem_Free(sgArr);
      SwapDecAsyncIO();
      return VMK_NO_MEMORY;
   }

   token->flags = ASYNC_CALLBACK;
   token->callback = SwapUWAsyncWriteCallback;
   token->clientData = swapToken = Mem_Alloc(sizeof(UWSwapToken));
   ASSERT(token->clientData);

   memset(swapToken, 0, sizeof(UWSwapToken));
   swapToken->token = token;
   swapToken->worldID = world->worldID;
   swapToken->ppn = ppn;
   swapToken->mpn = mpn;
   swapToken->reqNum = reqNum;
   swapToken->swapFileSlot.value = 0;
   swapToken->swapFileSlot.slot.fileNdx = swapFileNdx;
   swapToken->swapFileSlot.slot.slotNum = startSlotNum;

   token->resID = World_GetGroupLeaderID(world);
   status = FSS_AsyncFileIO(swapFileInfo->fileHandle, sgArr, token, FS_WRITE_OP);
   ASSERT(status == VMK_OK);
   if (status != VMK_OK) {
      VmWarn(world->worldID, "Write failed - status = %d", status);
      Mem_Free(swapToken);
      Mem_Free(token);
      Mem_Free(sgArr);
      SwapDecAsyncIO();
      return status;
   } 

   Mem_Free(sgArr);
 
   if (swapDoSanityChecks) {
      MPN slotMPN;
      SwapFileInfoLock(swapFileInfo);
      slotMPN = SwapGetSlotInfoMPN(swapFileInfo, startSlotNum);
      SwapSetSwapInfo(slotMPN, startSlotNum, World_GetGroupLeaderID(world), ppn, mpn);
      SwapFileInfoUnlock(swapFileInfo);
   }

   // Add to stats about the total number of pages written to this file
   SwapAddNrPagesWritten(swapFileInfo, nrSlots); 

   *swapSlotNr = startSlotNum;
   return VMK_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 * SwapWritePages --
 *
 *      Write pages from the list of PPNs to the swap file. Depending
 *      on the current memory stress on the system, this function may
 *      block the VM until the async writes finish.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------
 */
static void
SwapWritePages(World_Handle *world,
               Swap_VmmInfo *swapInfo)
{
   // use volatile pointer because the state will be changed by
   // the AsyncWriteCallback function once all writes have 
   // finished
   volatile Swap_VmmInfo *updatedSwapInfo = (volatile Swap_VmmInfo *)swapInfo;
   Bool allPagesWritten = FALSE;

   // update world's swap state
   SwapInfoLock(swapInfo);
   ASSERT_NOT_IMPLEMENTED(swapInfo->worldState == SWAP_WSTATE_LIST_REQ);
   swapInfo->worldState = SWAP_WSTATE_SWAPPING;
   SwapInfoUnlock(swapInfo);

   while(!allPagesWritten) {
      VMK_ReturnStatus status;
      status = SwapClusterWrite(world, swapInfo, &allPagesWritten);

      if (SwapShouldSwapBlock(world)) {
         switch (status) {
            case VMK_NOT_ENOUGH_SLOTS: 
               // Sleep waiting for free slots
               SwapTestAndSleepFreeSlots(world);
               break;
            case VMK_MAX_ASYNCIO_PENDING:
               // Sleep waiting for pending async ios to complete
               SwapTestAndSleepAsyncIO(FALSE);
               break;
            case VMK_OK:
               if (allPagesWritten) {
                  // if all pages are written, wait till the writes finish
                  VMLOG(2, world->worldID, "Doing forced sleep");
                  SwapTestAndSleepSwapDone(world->worldID, swapInfo);
                  break;
               }
               break;
            default:
               break;
         }
      } else {
         if (status != VMK_OK) {
            uint32 nextWriteNdx;
            // We are done 
            SwapInfoLock(swapInfo);
            ASSERT(updatedSwapInfo->worldState == SWAP_WSTATE_SWAPPING);

            nextWriteNdx = updatedSwapInfo->swapPgList.nextWriteNdx;

            if ((updatedSwapInfo->swapPgList.nrPagesWritten == nextWriteNdx) ||
                (nextWriteNdx == 0)) {
               // All outstanding async writes completed or
               // no writes issued at all
               updatedSwapInfo->worldState = SWAP_WSTATE_SWAP_DONE;
            } 
            updatedSwapInfo->swapPgList.nrPages = nextWriteNdx;
            SwapInfoUnlock(swapInfo);

            // Since we updated the number of pages to write 
            // to be the number of pages written
            allPagesWritten = TRUE;
         }
      }
   }

   if (allPagesWritten) {
      // We are done 
      SwapInfoLock(swapInfo);
      // If all outstanding async writes completed
      if (updatedSwapInfo->swapPgList.nrPagesWritten == 
          updatedSwapInfo->swapPgList.nrPages) {
         ASSERT(updatedSwapInfo->worldState == SWAP_WSTATE_SWAP_DONE);
      } else {
         updatedSwapInfo->worldState = SWAP_WSTATE_SWAP_ASYNC;
      }
      SwapInfoUnlock(swapInfo);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * SwapGetSwapMapPg --
 *
 *      Returns the mpn that stores the map for the block specified by 
 *      'blockNum'.  It allocates a MPN if this is the first time this 
 *      block is accessed.
 *
 * Results:
 *      Returns the mpn that stores the map for the specified block. 
 *      Returns INVALID_MPN in case of failure.
 *
 * Side effects:
 *      An mpn will be allocated if required
 *
 *-----------------------------------------------------------------------------
 */
static INLINE MPN
SwapGetSwapMapPg(uint32 blockNum, SwapFileInfo *swapFileInfo)
{
   ASSERT(blockNum < swapFileInfo->numBlocks);

   SwapFileInfoLock(swapFileInfo);
   if (swapFileInfo->blocks[blockNum].mapMPN == INVALID_MPN) {
      VMK_ReturnStatus status;

      // Allocate a new MPN
      swapFileInfo->blocks[blockNum].mapMPN = MemMap_AllocAnyKernelPage();
      ASSERT(swapFileInfo->blocks[blockNum].mapMPN != INVALID_MPN);
      if (swapFileInfo->blocks[blockNum].mapMPN == INVALID_MPN) {
         Warning("Unable to allocate mpn for map of block(%d)", blockNum);
         SwapFileInfoUnlock(swapFileInfo);
         return INVALID_MPN;
      }
      MemMap_SetIOProtection(swapFileInfo->blocks[blockNum].mapMPN, 
                             MMIOPROT_IO_DISABLE);
      status = Util_ZeroMPN(swapFileInfo->blocks[blockNum].mapMPN);
      ASSERT(status == VMK_OK);
      if (SWAP_DEBUG) {
         LOG(0, "*** New page allocated for swap blocks[%d] ***", blockNum);
      }
   }
   SwapFileInfoUnlock(swapFileInfo);
   return swapFileInfo->blocks[blockNum].mapMPN;
}

/*
 *-----------------------------------------------------------------------------
 *
 * SwapDoFastSearch --
 *
 *     Searches the given block map page one byte at a time looking for a free slot
 *     thus making it much faster than doing a bit by bit search of the swap 
 *     map pages.
 *
 * Results:
 *      returns the number of contiguous free slots found. 
 *      swapMapSlotNum is updated with the slot
 *      number within that swap map page which is the first free slot
 *
 *      returns 0 on failure
 *
 * Side effects:
 *      none
 *
 *-----------------------------------------------------------------------------
 */
static uint32 
SwapDoFastSearch(uint32 nrSlotsReq,
                 uint32 blockNum,
                 SwapFileInfo *swapFileInfo,
                 uint32 *swapMapSlotNum)
{
   uint32 maxClusterStart = 0;  // Starting slot number of the maximum cluster
   uint32 maxCluster      = 0;
   uint32 ndx = blockNum;
   MPN mpn;
   uint32 i;
   uint8 *swapSlots;
   KSEG_Pair *dataPair;
   uint32 curCluster      = 0;
   uint32 curClusterStart = SWAP_ALL_BITS_SET;
   uint32 nrSlotsToCheck;
 
   ASSERT(ndx < swapFileInfo->numBlocks);
   ASSERT(nrSlotsReq > 0);
   if (nrSlotsReq == 0) {
      return 0;
   }

   // Map the swap slots page if required
   mpn = SwapGetSwapMapPg(ndx, swapFileInfo);
   if (mpn == INVALID_MPN) {
      return 0;
   }

   // map the mpn
   swapSlots = (uint8 *)Kseg_MapMPN(mpn ,&dataPair);

   //
   // We search for empty slots without acquiring
   // "swapFileInfo->swapFileLock" lock as the calling function
   // SwapGetFileSlots handles all races between multiple VMs looking
   // for empty slots
   //
   // We do not want to check for more slots than the number of slots
   // available in the page, this happens because the size of the swap file
   // dictates the number of slots that are available and hence the number of
   // slots available may be fewer than what a page can hold
   for (i = 0, nrSlotsToCheck = SWAP_SLOTS_PER_BYTE; 
        (i < PAGE_SIZE) && (nrSlotsToCheck <= swapFileInfo->blocks[ndx].nrFreeSlots)  
        && (curCluster < nrSlotsReq); 
        ++i, nrSlotsToCheck += SWAP_SLOTS_PER_BYTE) {
      if (swapSlots[i] != 0) {
         // slot is full 
         if (curCluster > maxCluster) {
            maxCluster = curCluster;
            maxClusterStart = curClusterStart;
         }
         curCluster = 0;
         curClusterStart = SWAP_ALL_BITS_SET;
      } else {
         // slot is empty 
         if (curClusterStart == SWAP_ALL_BITS_SET) {
            // update curClusterStart with the slot number
            curClusterStart = i * SWAP_SLOTS_PER_BYTE;
            ASSERT(curCluster == 0);
         }
         curCluster += SWAP_SLOTS_PER_BYTE;
      }
   }

   if (curCluster > maxCluster) {
      maxCluster = curCluster;
      maxClusterStart = curClusterStart;
   }
   Kseg_ReleasePtr(dataPair);


   // Return the slot number of the first empty file slot in this cluster
   *swapMapSlotNum = maxClusterStart;
   return maxCluster;
}

/*
 *-----------------------------------------------------------------------------
 *
 * SwapDoSlowSearch --
 * 
 *      Searches the given block map page bit by bit looking for the number of
 *      free slots requsted. This function returns the first set of contiguous
 *      free slots that it finds, even if this set may be smaller than the
 *      number of slots requested. This is done because we dont really want to
 *      rely on this routine to get us the reqd number of file slots,
 *      SwapDoFastSearch should be successful most of the times.
 *
 * Results:
 *      returns the number of contiguous free slots found. 
 *      swapMapSlotNum is updated with the slot
 *      number within that swap map page which is the first free slot
 *      returns 0 in case of failure
 *
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------
 */
static uint32 
SwapDoSlowSearch(uint32 nrSlotsReq,
                 uint32 blockNum,     // map page in which  to search for free slots
                 SwapFileInfo *swapFileInfo,
                 uint32 *swapMapSlotNum)
{
   uint32 maxCluster      = 0;
   uint32 maxClusterStart = 0;
   uint32 curCluster      = 0;
   uint32 curClusterStart = (uint32) -1;
   uint32 slotNum;
   uint32 *swapSlots;
   KSEG_Pair *dataPair;
   MPN mpn;
  
   ASSERT(blockNum < swapFileInfo->numBlocks);
   // Map the swap slots page if required
   mpn = SwapGetSwapMapPg(blockNum, swapFileInfo);
   if (mpn == INVALID_MPN) {
      return 0;
   }

   // map the mpn
   swapSlots = (uint32 *)Kseg_MapMPN(mpn ,&dataPair);

   //
   // We search for empty slots without acquiring
   // "swapFileInfo->swapFileLock" lock as the calling function
   // SwapGetFileSlots handles all races between multiple VMs looking
   // for empty slots
   //
   for (slotNum = 0; 
        (slotNum < swapFileInfo->blocks[blockNum].nrSlots) && 
        (curCluster < nrSlotsReq); ++slotNum) {

      uint32 slotNdx;
      uint32 slotOffset;
      uint32 testBits = SWAP_ALL_BITS_SET; // initialize all bits to 1

      // convert slot number to the index number
      slotNdx = slotNum / SWAP_SLOTS_PER_UINT32; 
      // get slot offset within the index 
      slotOffset = slotNum % SWAP_SLOTS_PER_UINT32 ;

      testBits = ~(SWAP_ALL_BITS_SET << SWAP_BITS_PER_SLOT);
      testBits <<= slotOffset * SWAP_BITS_PER_SLOT;

      ASSERT(slotNdx < (PAGE_SIZE/sizeof(uint32)));
      if (swapSlots[slotNdx] & testBits) {
         // slot is full 
         if (curCluster > maxCluster) {
            maxCluster = curCluster;
            maxClusterStart = curClusterStart;
            // we just take the first set of contiguous empty slots that we find
            break;
         }
         curClusterStart = SWAP_ALL_BITS_SET;
      } else {
         // slot is empty 
         if (curClusterStart == SWAP_ALL_BITS_SET) {
            curClusterStart = slotNum;
            ASSERT(curCluster == 0);
         }
         curCluster++;
      }
   }

   Kseg_ReleasePtr(dataPair);

   if (curCluster > maxCluster) {
      maxCluster = curCluster;
      maxClusterStart = curClusterStart;
   }
   *swapMapSlotNum = maxClusterStart;
   return maxCluster;
}

/*
 *-----------------------------------------------------------------------------
 * SwapGetNextBlock --
 * 
 *      Remembers the last swap block that was searched and starts searching
 *      the subsequent blocks for the required number of free slots.
 *          Atomically decrements the number of free slots from the block and
 *      returns the number of slots claimed in the "nrSlotsClaimed" variable.
 *
 * Results:
 *      retruns the index of the page that has the required number of free
 *      slots.
 *      Returns SWAP_INVALID_BLOCK if no such page is found
 *
 * Side effects:
 *      none
 *
 *-----------------------------------------------------------------------------
 */
static uint32
SwapGetNextBlock(uint32 reqFreeSlots,
                  SwapFileInfo *swapFileInfo,
                  uint32 *nrSlotsClaimed) // OUT: num slots claimed
{
   uint32 i;
   uint32 curNrFreeSlots = 0;
   uint32 curNdx = SWAP_INVALID_BLOCK;

   *nrSlotsClaimed = 0;

   ASSERT(reqFreeSlots > 0);
   if (reqFreeSlots == 0) {
      return SWAP_INVALID_BLOCK;
   }

   SwapFileInfoLock(swapFileInfo);
   swapFileInfo->lastBlock = (swapFileInfo->lastBlock + 1) %
                             swapFileInfo->numBlocks;
   ASSERT(swapFileInfo->lastBlock < swapFileInfo->numBlocks);

   for (i = 0; (i < swapFileInfo->numBlocks) && (curNrFreeSlots < reqFreeSlots); ++i) {
      uint32 ndx;
      ndx  = (swapFileInfo->lastBlock + i) % swapFileInfo->numBlocks;
      if (swapFileInfo->blocks[ndx].nrFreeSlots > curNrFreeSlots) {
         curNdx = ndx;
         curNrFreeSlots = swapFileInfo->blocks[ndx].nrFreeSlots;
      }
   }

   // claim the required slots
   if (curNdx != SWAP_INVALID_BLOCK) {
      *nrSlotsClaimed = MIN(reqFreeSlots, swapFileInfo->blocks[curNdx].nrFreeSlots);
      swapFileInfo->blocks[curNdx].nrFreeSlots -= *nrSlotsClaimed;
      ASSERT(swapFileInfo->blocks[curNdx].nrFreeSlots >= 0);

      // next time start searching from curNdx
      swapFileInfo->lastBlock = curNdx;
   }

   SwapFileInfoUnlock(swapFileInfo);
   return curNdx;
}


/*
 *-----------------------------------------------------------------------------
 *
 * SwapGetFileSlots --
 *
 *      Scans the swap file blocks looking for the specified number of  
 *      contiguous empty file slots. 
 *      We have 3 level search hierarchy.
 *              1. Search for a suitable Swap File, we do this
 *                 search under the safety of a lock and reserve
 *                 the required number of free slots in this file.
 *              2. Each file is made up of blocks, so once we have selected 
 *                 a swap file we search for a suitable block in this file.
 *              3. Then we look at the actual slots in the selected block.
 *        Locking Issues and Race conditions:
 *              Steps 1 and 2 mentioned above is carried out with a lock
 *              held. Step 3 does not acquire any locks, hence 
 *              there may be a race between multiple VMs doing step
 *              3, in which case we just retry. Since we atomically 
 *              reserve slots in this file in step 1 and in the map in step 2
 *              we are always guranteed to 
 *              find the required free slots in this file and our retries are
 *              bound to succeed.
 *
 * Results:
 *      returns the size of the cluster with the specified number of continuous
 *      empty slots.
 *      if it cannot find a cluster with the specified number of continuous 
 *      empty slots, it returns the size of the next biggest cluster
 *      
 *      sets "startSlotNum" to the first empty slot in the cluster.
 *
 * Side effects:
 *      marks the selected empty slots as being full.
 *
 *-----------------------------------------------------------------------------
 */
static uint32
SwapGetFileSlots(uint32 reqClusterSize,    // number of continuous slots required
                 uint32 *swapFileNdx,
                 uint32 *startSlotNum)
{
   uint32 blockNum = 0;
   uint32 swapMapSlotNum = 0;
   uint32 nrFreeSlotsClaimed; 
   uint32 nrMapSlotsClaimed;
   uint32 nrSlotsFound;
   uint32 i;
   uint32 *swapSlots;
   KSEG_Pair *dataPair;
   SwapFileInfo *swapFileInfo;
   Bool fastSearch = FALSE;

   // The SwapGetFreeFile function will atomically decrement the
   // number of free slots in the file. When we actually do 
   // do the FastSearch/SlowSearch for continuous slots in this
   // file we *may not* find the required number of continuous
   // free slots and we will end up using only a part of these 
   // free slots. This means that we have to adjust the free slots
   // count later in this function. Another alternative would
   // have been to hold the lock for the entire duration of this function
   // and in that case we would not have to do this free count adjustment,
   // but i dont like the idea of holding an irq lock while we
   // do the Fast/Slow search..
   *swapFileNdx = SwapGetFreeFile(reqClusterSize, &nrFreeSlotsClaimed);
   if (*swapFileNdx == SWAP_FILE_INVALID_INDEX) {
      Warning("All swap files are full, couldnt find any free slots");
      return(0);
   }
   swapFileInfo = SwapGetSwapFile(*swapFileNdx);
   reqClusterSize = MIN(reqClusterSize, nrFreeSlotsClaimed);

retry:
   // Get the swap block in which to search for the free slots 
   blockNum = SwapGetNextBlock(reqClusterSize, swapFileInfo, 
                               &nrMapSlotsClaimed);
   ASSERT(blockNum != SWAP_INVALID_BLOCK);
   if (blockNum == SWAP_INVALID_BLOCK) {
      return(0);
   }

   // adjust required cluster size accordingly
   ASSERT(nrMapSlotsClaimed > 0);
   reqClusterSize = MIN(reqClusterSize, nrMapSlotsClaimed);

   // First try to do a quick search
   nrSlotsFound = SwapDoFastSearch(reqClusterSize, blockNum, swapFileInfo,
                                   &swapMapSlotNum);
   if (nrSlotsFound > 0) {
      // update nrSlotsFound, as FastSearch searches slots in multiples of 8
      nrSlotsFound = MIN(nrSlotsFound, reqClusterSize);
      fastSearch = TRUE;
   } else {
      nrSlotsFound = SwapDoSlowSearch(reqClusterSize, blockNum, swapFileInfo,
                                      &swapMapSlotNum);
      ASSERT(nrSlotsFound > 0);
      if ( nrSlotsFound == 0) {
         // Give back all the claimed free slots
         SwapFileInfoLock(swapFileInfo);
         swapFileInfo->nrFreeSlots += nrFreeSlotsClaimed;
         swapFileInfo->blocks[blockNum].nrFreeSlots += nrMapSlotsClaimed;
         // Increment the total number of free slots
         // and Wakeup VMs waiting for free slots
         SwapIncTotalNumFreeSlots(nrFreeSlotsClaimed); 
         SwapFileInfoUnlock(swapFileInfo);
         return(0);
      }
   } 

   SwapFileInfoLock(swapFileInfo);

   // Collect stats about the number of fast/slow searches
   if (fastSearch) {
      swapFileInfo->stats.nrFastSearch++;
   } else {
      swapFileInfo->stats.nrSlowSearch++;
   }

   // map the mpn
   swapSlots = (uint32 *)Kseg_MapMPN(swapFileInfo->blocks[blockNum].mapMPN,
                                     &dataPair);

   // Check if the slots are free, as there maybe a race since
   // we do our Fast/Slow searches without the "swapFileInfo->swapFileLock" lock held
   for (i = 0; i < nrSlotsFound; ++i) {
      uint32 testBits = SWAP_ALL_BITS_SET; // initialize all bits to 1
      uint32 slotNdx;
      uint32 slotOffset;
      uint32 slotNum = swapMapSlotNum + i;
      ASSERT(slotNum < swapFileInfo->blocks[blockNum].nrSlots);

      // convert the slot number to the index number
      slotNdx = slotNum / SWAP_SLOTS_PER_UINT32; 
      // get the slot offset within the index 
      slotOffset = slotNum % SWAP_SLOTS_PER_UINT32;

      ASSERT(slotNdx < (PAGE_SIZE/sizeof(uint32)));

      testBits = ~(SWAP_ALL_BITS_SET << SWAP_BITS_PER_SLOT);
      testBits <<= slotOffset * SWAP_BITS_PER_SLOT;
      if (swapSlots[slotNdx] & testBits) {
         // Oh, so there was a race in acquiring free slots, retry
         LOG(1, "........Race for free slots, retrying.......");
         swapFileInfo->stats.nrSlotFindRetries++;
         Kseg_ReleasePtr(dataPair);

         // Give back all the claimed map slots
         swapFileInfo->blocks[blockNum].nrFreeSlots += nrMapSlotsClaimed;

         SwapFileInfoUnlock(swapFileInfo);
         goto retry;
      }
   }

   // slots are definately free, start using them...
   for (i = 0; i < nrSlotsFound; ++i) {
      uint32 testBits = SWAP_ALL_BITS_SET; // initialize all bits to 1
      uint32 slotNdx;
      uint32 slotOffset;
      uint32 slotNum = swapMapSlotNum + i;
      ASSERT(slotNum < swapFileInfo->blocks[blockNum].nrSlots);

      // convert the slot number to the index number
      slotNdx = slotNum / SWAP_SLOTS_PER_UINT32; 
      // get the slot offset within the index 
      slotOffset = slotNum % SWAP_SLOTS_PER_UINT32;

      testBits = SWAP_SLOT_IN_USE;
      testBits <<= slotOffset * SWAP_BITS_PER_SLOT;

      ASSERT(slotNdx < (PAGE_SIZE/sizeof(uint32)));

      if (vmx86_debug) {
         uint32 debugBits = SWAP_ALL_BITS_SET; // initialize all bits to 1
         debugBits = ~(SWAP_ALL_BITS_SET << SWAP_BITS_PER_SLOT);
         debugBits <<= slotOffset * SWAP_BITS_PER_SLOT;
         ASSERT(((swapSlots[slotNdx]) & debugBits) == 0);
      }
      // Set the slot as being occupied 
      swapSlots[slotNdx] |= testBits;
   }

   // Give back the number of claimed free slots that are not used
   ASSERT(nrFreeSlotsClaimed >= nrSlotsFound);
   ASSERT(nrMapSlotsClaimed >= nrSlotsFound);
   ASSERT(swapFileInfo->nrFreeSlots >= 0);
   ASSERT(swapFileInfo->blocks[blockNum].nrFreeSlots >= 0);
   swapFileInfo->nrFreeSlots += (nrFreeSlotsClaimed - nrSlotsFound);
   swapFileInfo->blocks[blockNum].nrFreeSlots += 
      (nrMapSlotsClaimed - nrSlotsFound);

   // Increment the total number of free slots
   // and Wakeup VMs waiting for free slots
   SwapIncTotalNumFreeSlots(nrFreeSlotsClaimed - nrSlotsFound); 

   // Calculate the absolute start slot number in the swap file
   *startSlotNum = swapMapSlotNum + (blockNum * SWAP_SLOTS_PER_PAGE);

   //make sure that we are not writing beyond the end of file
   ASSERT((swapMapSlotNum + nrSlotsFound - 1) < swapFileInfo->blocks[blockNum].nrSlots);

   Kseg_ReleasePtr(dataPair);
   SwapFileInfoUnlock(swapFileInfo);
   ASSERT(nrSlotsFound > 0);
   return(nrSlotsFound);
}

/*
 *-----------------------------------------------------------------------------
 *
 * SwapClusterWrite --
 *      Writes the pages to the swap file asynchronously. It tries to cluster 
 *      the writes as much as possible. It starts by looking for a cluster with
 *      either the default cluster size or the number of clusters required 
 *      whichever is the smaller. If the requested cluster size is found 
 *      it writes the pages to that cluster
 *        If the requested cluster size is not found, it writes the number of
 *      pages that will fit in the available cluster and reduces the next cluster
 *      request by half of the current request. Every failed request causes the
 *      cluster size to be cut by half, eventually leading to a cluster size of
 *      1.
 *        The amount of disk bandwidth that can be used by a world is controlled
 *      by the nrAsyncWrites variable.
 *
 * Results:
 *      returns VMK_OK on success, else returns the error status
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
SwapClusterWrite(World_Handle  *world, 
                 Swap_VmmInfo     *swapInfo,
                 Bool *allPagesWritten)
{
   uint32 oldNdx;
   uint32 curClusterSize;
   uint32 nrSlotsReq;
   uint32 nrSlotsFound;
   uint32 startSlotNum;
   uint32 i;
   VMK_ReturnStatus status ;
   SwapPgList *swapPgList = &(swapInfo->swapPgList);
   SwapFileInfo *swapFileInfo;
   uint32 swapFileNdx = SWAP_FILE_INVALID_INDEX;
   Bool canDoAsyncIO;
   // initialize return value
   *allPagesWritten = FALSE;

   nrSlotsReq = (swapPgList->nrPages - swapPgList->nextWriteNdx);
   curClusterSize = MIN(nrSlotsReq, SWAP_MAX_CLUSTER_SIZE); 

   // In case all the pages have been written, but
   // the asynchronous writes havent completed as yet. 
   //
   if (nrSlotsReq == 0){
      *allPagesWritten = TRUE;
      return (VMK_OK);
   }

   ASSERT(nrSlotsReq > 0);
   ASSERT(curClusterSize <= SWAP_MAX_CLUSTER_SIZE);

   for (i = 0; (i < swapPgList->nrAsyncWrites) && (nrSlotsReq > 0); ++i) {
      // check to see if we can do any more async io
      if (!(canDoAsyncIO = SwapTestAndIncAsyncIO())) {
         return(VMK_MAX_ASYNCIO_PENDING);
      }
      nrSlotsFound = SwapGetFileSlots(curClusterSize, 
                                      &swapFileNdx,
                                      &startSlotNum);

      ASSERT(nrSlotsFound <= curClusterSize);
      if (nrSlotsFound == 0) {
         SwapDecAsyncIO();
         return(VMK_NOT_ENOUGH_SLOTS);
      }

      if (nrSlotsFound != curClusterSize) {
         // cut the cluster size by half
         curClusterSize /= 2;
         if (curClusterSize == 0) {
            curClusterSize = 1;
         }
         // it is highly unlikely that we can find any more clusters greater than
         // the nrSlotsFound, so adjust the cluster size if reqd
         if (curClusterSize > nrSlotsFound) {
            curClusterSize = nrSlotsFound;
         }
      }

      swapFileInfo = SwapGetSwapFile(swapFileNdx);

      // Write nrSlotsFound to disk
      oldNdx = swapPgList->nextWriteNdx;
      status = SwapAsyncWrite(world, swapFileInfo->fileHandle, swapInfo, 
                              swapFileInfo, swapFileNdx, 
                              swapPgList->nextWriteNdx, startSlotNum, nrSlotsFound,
                              &(swapPgList->nextWriteNdx));
      ASSERT(status == VMK_OK);
      if (status != VMK_OK){
         if (SWAP_DEBUG) {
            LOG(0, "Asynchrnous write failed! status = %d", status);
         }
         SwapDecAsyncIO();
         return (status);
      }

      if (swapPgList->nextWriteNdx == oldNdx) {
         ASSERT(0);
         if (SWAP_DEBUG) {
            LOG(0, "Did not write any pages to the swap file");
         }
         return (VMK_OK);
      }
      
      nrSlotsReq -= nrSlotsFound;
      if (curClusterSize > nrSlotsReq) {
         curClusterSize = nrSlotsReq;
      }
   }
   return (VMK_OK);
}


/*
 *-----------------------------------------------------------------------------
 *
 * SwapAsyncWrite  --
 *      Asynchronously write the pages to the swap file.
 *
 * Results:
 *      VMK_OK when successful, error code otherwise.
 *
 * Side effects:
 *      will cause the SwapAsyncWriteCallback routine to be called once the
 *      disk io is finished.
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
SwapAsyncWrite(World_Handle *world,
               FS_FileHandleID fileHandle,
               Swap_VmmInfo *swapInfo, 
               SwapFileInfo *swapFileInfo,
               uint32 swapFileNdx,
               uint32 swapPPNNdx,
               uint32 startSlotNum, 
               uint32 nrSlots,
               uint32 *swapPFNextNdx) 
{
   uint32 i = 0;
   uint32 swapNdx = swapPPNNdx;
   SwapToken *swapToken;
   Async_Token *token ;
   SG_Array *sgArr ;
   VMK_ReturnStatus status = VMK_OK;
   SwapPgList *swapPgList = &swapInfo->swapPgList;

   VMLOG(1, world->worldID, "startSlotNum(0x%x),nrSlots(%d)", 
         startSlotNum, nrSlots);

   sgArr = (SG_Array *)Mem_Alloc(SG_ARRAY_SIZE(nrSlots));
   ASSERT(sgArr != NULL);
   if (!sgArr) {
      VmWarn(world->worldID, "Cannont allocate sgArr");
      return VMK_NO_MEMORY;
   }
   sgArr->length = nrSlots;
   sgArr->addrType = SG_MACH_ADDR;

   for (i = 0; i < nrSlots; ++i, ++swapNdx) {
      MPN mpn = swapPgList->swapMPNList[swapNdx];

      // sanity checks
      ASSERT(swapNdx < swapPgList->nrPages);
      ASSERT(swapPgList->swapPPNList[swapNdx] != INVALID_PPN);
      ASSERT(mpn != INVALID_MPN);
      ASSERT(mpn <= MemMap_GetLastValidMPN()); 

      sgArr->sg[i].length = PAGE_SIZE;
      sgArr->sg[i].addr = MPN_2_MA(mpn);
      sgArr->sg[i].offset = SWAP_SLOT_2_OFFSET(startSlotNum + i);

      if (swapDoSanityChecks) {
         MPN slotMPN;
         SwapFileInfoLock(swapFileInfo);
         slotMPN = SwapGetSlotInfoMPN(swapFileInfo, (startSlotNum + i));
         SwapSetSwapInfo(slotMPN, (startSlotNum + i), World_GetGroupLeaderID(world), 
                                   swapPgList->swapPPNList[swapNdx], mpn);
         SwapFileInfoUnlock(swapFileInfo);
      }
   }

   token = Async_AllocToken(0);
   ASSERT(token);
   if (token == NULL) {
      VmWarn(world->worldID, "Alloc token failed");
      Mem_Free(sgArr);
      return VMK_NO_MEMORY;
   }

   token->flags = ASYNC_CALLBACK;
   token->callback = SwapAsyncWriteCallback;
   token->clientData = swapToken = Mem_Alloc(sizeof(SwapToken));
   ASSERT(token->clientData);

   memset(swapToken, 0, sizeof(SwapToken));
   swapToken->token = token;
   swapToken->worldID = world->worldID;
   swapToken->swapFileNdx = swapFileNdx;
   swapToken->swapPPNNdx = swapPPNNdx;
   swapToken->startSlotNum = startSlotNum;
   swapToken->nrSlots = nrSlots;

   token->resID = World_GetVmmLeaderID(world);
   status = FSS_AsyncFileIO(fileHandle, sgArr, token, FS_WRITE_OP);
   ASSERT(status == VMK_OK);
   if (status != VMK_OK) {
      VmWarn(world->worldID, "Write failed - status = %d", status);
      Mem_Free(swapToken);
      Mem_Free(token);
      Mem_Free(sgArr);
      return status;
   } 

   Mem_Free(sgArr);
 
   // Add to stats about the total number of pages written to this file
   SwapAddNrPagesWritten(swapFileInfo, nrSlots); 
   // return the next index for the swapPPNList array in swapPgList
   *swapPFNextNdx = swapNdx;
   VMLOG(1, world->worldID, "startSlotNum(0x%x), nrSlots(%d), swapPFNextNdx = %u", 
         startSlotNum, nrSlots, swapNdx);
   return VMK_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 * SwapAsyncWriteCallback --
 *      For each machine page that has been written to disk. Release the 
 *      machine page and add it to the list of free pages. Update the AllocPFrame 
 *      for the corresponding PPN with the swap file slot num so that this page 
 *      can be located on a subsequent page fault.
 *         This function also needs to correctly handle the cases where the page
 *      being wrritten to disk was deallocated, page faulted, or the world
 *      to which it belonged to was killed or died.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      free the MPN of the page if it  is untouched.
 *      free the File slot if the page was faulted in or deallocated
 *
 *-----------------------------------------------------------------------------
 */
static void 
SwapAsyncWriteCallback(Async_Token *token) 
{
   World_Handle *world;
   World_ID worldID = INVALID_WORLD_ID;
   uint32 swapPPNNdx = (uint32) -1; // initialize to some invalid values to get by
   uint32 startSlotNum = (uint32) -1;  // compiler warnings
   uint32 swapFileNdx = SWAP_FILE_INVALID_INDEX; 
   uint32 nrSlots = (uint32) -1;
   uint32 i;
   uint32 statsNrFreeMPN = 0;
   uint32 statsNrFreeSlots = 0;
   uint32 statsNrTotal = 0;
   volatile Swap_VmmInfo *updatedSwapInfo;
   Bool writeFailed = FALSE;

   Swap_VmmInfo *swapInfo;
   SwapPgList *swapPgList;
   SwapFileInfo *swapFileInfo;
   uint32 swapNdx ;
   SwapToken *swapToken;

   swapToken = (SwapToken *)token->clientData;
   ASSERT(swapToken->token == token);
   worldID = swapToken->worldID;
   swapFileNdx = swapToken->swapFileNdx;
   swapPPNNdx = swapToken->swapPPNNdx;
   startSlotNum = swapToken->startSlotNum;
   nrSlots = swapToken->nrSlots;
   // Free the memory used by the swapToken
   Mem_Free(swapToken);

   statsNrTotal = nrSlots;
   VMLOG(2, worldID, "startSlotNum = 0x%x; nrSlots = %d", startSlotNum, nrSlots);

   swapFileInfo = SwapGetSwapFile(swapFileNdx);

   world = World_Find(worldID);
   if (!world) {
      WarnVmNotFound(worldID); 
      // free up the file slots 
      SwapFreeFileSlots(startSlotNum, nrSlots, swapFileInfo);
      Async_ReleaseToken(token);

      // Indicate that this async io is complete
      // *DO* only after World_Find, refer to the comment 
      // later in this function for details
      SwapDecAsyncIO();
      return ;
   }
   ASSERT(World_IsVmmLeader(world));

   // Mark that this async io as complete
   // *only* after we have first done a World_Find 
   // otherwise we have to worry about the following race.
   //
   // o  We do SwapDecAsyncIO before World_Find
   // o  Meanwhile Swap_Deactivate sees no outstanding IO and
   //    clears swapFileInfo
   // o  And to make things worse world is destroyed before 
   //    we do a World_Find so we fail and then when we try to do 
   //    SwapFreeFileSlots we get a pretty looking exception 14 PSOD
   SwapDecAsyncIO();

   // For debugging in obj builds only, fail
   // after every 5000 write callbacks
   if (SWAP_DEBUG) {
      static uint32 dbgFailCount = 0;
      if (dbgFailCount % 1000 == 0) {
         writeFailed = TRUE;
      }
      dbgFailCount++;
   }

   if (((SCSI_Result *)token->result)->status != 0) {
      VMLOG(0, worldID, "AsynWrite failed for world, scsiStatus = 0x%x", 
            ((SCSI_Result *)token->result)->status);
      writeFailed = TRUE;
   }

   swapInfo = SwapGetVmmInfo(world);
   swapPgList = &swapInfo->swapPgList;
   swapNdx = swapPPNNdx;

   for (i = 0; i < nrSlots; i++, swapNdx++) {
      Bool swapped;
      SwapFileSlot swapFileSlot;
      swapFileSlot.value = 0;
      swapFileSlot.slot.fileNdx = swapFileNdx;
      swapFileSlot.slot.slotNum = startSlotNum + i;

      ASSERT(swapNdx < swapPgList->nrPages);
      ASSERT(swapPgList->swapPPNList[swapNdx] != INVALID_PPN);
      ASSERT(startSlotNum + i < swapFileInfo->nrSlots);

      ASSERT(!World_IsUSERWorld(world));
      swapped = Alloc_MarkSwapPage(world, writeFailed,
                                   swapFileSlot.value,
                                   swapPgList->swapPPNList[swapNdx],
                                   swapPgList->swapMPNList[swapNdx]);
      if (!swapped) {
         // update swap statistics
         swapInfo->stats.numPagesWritten++;
         SwapFreeFileSlots(startSlotNum + i, 1, swapFileInfo);
         statsNrFreeSlots++;
      } else {
         statsNrFreeMPN++;
      }
   }

   // protect against other callbacks 
   SwapInfoLock(swapInfo);
   swapPgList->nrPagesWritten += nrSlots;

   updatedSwapInfo = (volatile Swap_VmmInfo *)swapInfo;
   ASSERT(swapPgList->nrPagesWritten <= swapPgList->nrPages);

   if(writeFailed) {
      swapGlobalInfo.nrAsyncWriteFailures++;
   }

   if (swapPgList->nrPagesWritten != swapPgList->nrPages) {
      SwapInfoUnlock(swapInfo);

      VMLOG(1, world->worldID,
           "Total pages = %d; MPN Released = 0x%x; Slots Released = %d",
           statsNrTotal, statsNrFreeMPN,  statsNrFreeSlots);

      World_Release(world);
      Async_ReleaseToken(token);
      return;
   }

   // we have written all the selected pages 
   
   // mark all the swap PPNs and MPNs as invalid
   ASSERT(swapPgList->nrPages <= swapPgList->length);
   for (i = 0; i < swapPgList->nrPages; ++i) {
      swapPgList->swapPPNList[i] = INVALID_PPN;
      swapPgList->swapMPNList[i] = INVALID_MPN;
   }
   ASSERT(updatedSwapInfo->worldState == SWAP_WSTATE_SWAPPING ||
          updatedSwapInfo->worldState == SWAP_WSTATE_SWAP_ASYNC);

   if (updatedSwapInfo->worldState == SWAP_WSTATE_SWAPPING) {
      updatedSwapInfo->worldState = SWAP_WSTATE_SWAP_DONE;
      SwapInfoWakeup(swapInfo);
      SwapInfoUnlock(swapInfo);
   } else {
      ASSERT(updatedSwapInfo->worldState == SWAP_WSTATE_SWAP_ASYNC);
      // Since we are no longer swapping
      updatedSwapInfo->worldState = SWAP_WSTATE_INACTIVE;

      // Wakeup the VM if it blocked, waiting for async writes to finish
      SwapInfoWakeup(swapInfo);
      SwapInfoUnlock(swapInfo);

      // Start swapping if required
      SwapStartSwapping(world);
   }

   VMLOG(1, world->worldID,
         "Total pages = %d; MPN Released = 0x%x; Slots Released = %d",
         statsNrTotal, statsNrFreeMPN,  statsNrFreeSlots);
   World_Release(world);
   Async_ReleaseToken(token);
}

/*
 *-----------------------------------------------------------------------------
 *
 * Swap_FreeUWFileSlot --
 *      Free the specified slot in the swap file used by a userworld.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------
 */
void
Swap_UWFreeFileSlot(uint32 startSlotNum)
{
   SwapFileSlot swapFileSlot;
   SwapFileInfo *swapFileInfo;
   swapFileSlot.value = startSlotNum;

   swapFileInfo = SwapGetSwapFile(swapFileSlot.slot.fileNdx);
   ASSERT(swapFileSlot.slot.slotNum < swapFileInfo->nrSlots);
   SwapFreeFileSlots(swapFileSlot.slot.slotNum, 1, swapFileInfo);
}

/*
 *-----------------------------------------------------------------------------
 *
 * Swap_FreeFileSlot --
 *      Free the specified slot in the swap file
 *
 *      Caller must hold alloc lock
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------
 */
void
Swap_FreeFileSlot(World_Handle *world,
                  uint32 startSlotNum)
{
   SwapFileSlot swapFileSlot;
   SwapFileInfo *swapFileInfo;
   swapFileSlot.value = startSlotNum;
   MemSchedVmmUsage *usage = MemSched_ClientVmmUsage(world);
   // Do nothing if we are dealing with the checkpoint file
   if (SwapIsCptFile(&swapFileSlot) || SwapIsMigrated(&swapFileSlot)) {
      return;
   }

   swapFileInfo = SwapGetSwapFile(swapFileSlot.slot.fileNdx);
   ASSERT(swapFileSlot.slot.slotNum < swapFileInfo->nrSlots);
   SwapFreeFileSlots(swapFileSlot.slot.slotNum, 1, swapFileInfo);
   // update statistics
   usage->swapped -= 1;
   SwapGetVmmInfo(world)->stats.numPagesRead++;
}

/*
 *-----------------------------------------------------------------------------
 *
 * SwapFreeFileSlots --
 *      Free the specified number of file slots in the swap file
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------
 */
static void
SwapFreeFileSlots(uint32 absStartSlotNum, 
                 uint32 nrSlots, 
                 SwapFileInfo *swapFileInfo)
{
   uint32 offset;
   uint32 slotNdx;
   uint32 resetBits;
   uint32 startSlotNum;
   uint32 i;
   uint32 blockNum;
   uint32 *swapSlots;
   KSEG_Pair *dataPair;

   if (swapDoSanityChecks) {
      for (i = 0; i < nrSlots; ++i) {
         MPN slotMPN;
         slotMPN = SwapGetSlotInfoMPN(swapFileInfo, (absStartSlotNum + i));
         SwapFreeSlotInfo(slotMPN, (absStartSlotNum + i)); 
      }
   }

   SwapFileInfoLock(swapFileInfo);

   // Get the swapMap mpn which contains these slots
   blockNum = absStartSlotNum / SWAP_SLOTS_PER_PAGE;
   startSlotNum = absStartSlotNum % SWAP_SLOTS_PER_PAGE;

   // Currently the way slots are allocated, it is impossible to 
   // get slots belonging from different SwapMap pages, So make sure
   // that all the slots are from the same SwapMap page, This
   // makes our job much simpler
   ASSERT((startSlotNum + nrSlots) <= SWAP_SLOTS_PER_PAGE);
   ASSERT((startSlotNum + nrSlots) <= swapFileInfo->blocks[blockNum].nrSlots);

   // map the mpn
   swapSlots = (uint32 *)Kseg_MapMPN(swapFileInfo->blocks[blockNum].mapMPN, 
                                     &dataPair);

   for (i = 0; i < nrSlots; ++i) {
      uint32 slotNum = startSlotNum + i; 
      ASSERT(slotNum < swapFileInfo->blocks[blockNum].nrSlots);
      slotNdx = slotNum / SWAP_SLOTS_PER_UINT32;
      offset  = slotNum % SWAP_SLOTS_PER_UINT32; 

      resetBits = ~((uint32) -1 << SWAP_BITS_PER_SLOT) ;
      resetBits = ~(resetBits << offset * SWAP_BITS_PER_SLOT);

      ASSERT(slotNdx < (PAGE_SIZE/sizeof(uint32)));

      if (vmx86_debug) {
         // make sure that the slot is actually full
         uint32 testBits = SWAP_ALL_BITS_SET;
         testBits = ~(SWAP_ALL_BITS_SET << SWAP_BITS_PER_SLOT);
         testBits <<= offset * SWAP_BITS_PER_SLOT;
         ASSERT(swapSlots[slotNdx] & testBits); 
      }

      swapSlots[slotNdx] &= resetBits;

      swapFileInfo->blocks[blockNum].nrFreeSlots++;
      swapFileInfo->nrFreeSlots++;
   }
   Kseg_ReleasePtr(dataPair);

   // Increment the total number of free slots
   // and Wakeup VMs waiting for free slots
   SwapIncTotalNumFreeSlots(nrSlots); 

   SwapFileInfoUnlock(swapFileInfo);
}

/*
 *-----------------------------------------------------------------------------
 *
 * Swap_SetCptFileInfo --
 *      
 *      Collect information about the checkpoint file from which the
 *      given world is being resumed.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus 
Swap_SetCptFileInfo(World_Handle *world, 
                    uint32 nrVMMemPages,
                    VMnix_FilePhysMemIOArgs *args)
{
   Swap_ChkpointFileInfo *swapCptFile = SwapGetCptFile(world);

   ASSERT(World_IsVmmLeader(world));
   ASSERT(swapCptFile->state == SWAP_CPT_FILE_CLOSED);
   if (swapCptFile->state != SWAP_CPT_FILE_CLOSED) {
      World_Panic(world, "Inconsistent swap checkpoint state %d", 
                 swapCptFile->state);
      return VMK_FAILURE;
   }
   swapCptFile->nrVMMemPages = nrVMMemPages;
   swapCptFile->nrPagesToRead = 0;
   swapCptFile->nrPagesRead = 0;
   return SwapOpenCptFile(world, args->handleID);
}


/*
 *-----------------------------------------------------------------------------
 *
 * SwapOpenCptFile --
 *
 *      Opens the checkpoit file for use by the swap code. Handles 
 *      race between multiple vcpus trying to open the file at the same time.
 *
 * Results --
 *      VMK_OK if file was openend successfully, otherwise returns VMK_FAILURE
 *
 * Side effects:
 *      May cause the caller to sleep.
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
SwapOpenCptFile(World_Handle *world,
                FS_FileHandleID vmnixFileHandle)
{
   Swap_VmmInfo *swapInfo = SwapGetVmmInfo(world);
   Swap_ChkpointFileInfo *swapCptFile = SwapGetCptFile(world);
   volatile SwapCptFileState *fileState = 
                       (volatile SwapCptFileState *)&swapCptFile->state;
   VMK_ReturnStatus status;

   ASSERT(World_IsVmmLeader(world));
   ASSERT_HAS_INTERRUPTS();

   SwapInfoLock(swapInfo);
   while(1) {
      if (*fileState == SWAP_CPT_FILE_OPEN) {
         break;
      }
      if (*fileState == SWAP_CPT_FILE_OPENING) {
         // Sleep
         SwapInfoWaitLock(swapInfo, CPUSCHED_WAIT_SWAP_CPTFILE_OPEN);
         continue;
      }
      if (*fileState == SWAP_CPT_FILE_CLOSED) {
         uint32 retryCount = 0;
         *fileState = SWAP_CPT_FILE_OPENING;
         SwapInfoUnlock(swapInfo);
retry:
         retryCount++; 
         if (retryCount > SWAP_MAX_NR_CPTFILE_OPEN_TRIES) {
            SwapInfoLock(swapInfo);
            *fileState = SWAP_CPT_FILE_CLOSED;
            SwapInfoUnlock(swapInfo);
            VmWarn(world->worldID, 
                   "Failed to open checkpoint file after %d attempts, killing world",
                   retryCount);
            World_Panic(world, "Failed to open checkpoint file");
            return(VMK_FAILURE);
         }

         status = FSClient_ReopenFile(vmnixFileHandle, FILEOPEN_READ,
				      &swapCptFile->fileHandle);
         if (status != VMK_OK) {
            CpuSched_Sleep(SWAP_CPT_OPEN_SLEEP_PERIOD);
            goto retry;
         }
         VMLOG(1, world->worldID, "Checkpoint file opened successfully"); 

         SwapInfoLock(swapInfo);
         ASSERT(*fileState == SWAP_CPT_FILE_OPENING);
         *fileState = SWAP_CPT_FILE_OPEN;
         // Wakeup theads waiting for file to be opened
         SwapInfoWakeup(swapInfo);
         break;
      }
   }
   SwapInfoUnlock(swapInfo);
   return(VMK_OK);
}
                
/*
 *--------------------------------------------------------------------------
 *
 * SwapCloseCptFile --
 *      
 *      Close the checkpoint file
 *
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      none.
 *
 *--------------------------------------------------------------------------
 */
static void
SwapCloseCptFile(World_Handle *world)
{
   Swap_VmmInfo *swapInfo = SwapGetVmmInfo(world);
   Swap_ChkpointFileInfo *swapCptFile = SwapGetCptFile(world);
   VMK_ReturnStatus status;

   // Return if swap file not open
   if (swapCptFile->state == SWAP_CPT_FILE_CLOSED) {
      return;
   }

   SwapInfoLock(swapInfo);
   ASSERT(swapCptFile->state == SWAP_CPT_FILE_OPEN ||
          swapCptFile->state == SWAP_CPT_FILE_CLOSED);

   if (swapCptFile->state ==  SWAP_CPT_FILE_CLOSED) {
      SwapInfoUnlock(swapInfo);
      return;
   }

   swapCptFile->state = SWAP_CPT_FILE_CLOSED;
   SwapInfoUnlock(swapInfo);

   // Close the file 
   status = FSS_CloseFile(swapCptFile->fileHandle);
   ASSERT(status == VMK_OK);
   VMLOG(1, world->worldID, "Checkpoint file closed");
}

/*
 *-----------------------------------------------------------------------------
 *
 * Swap_GetCptSwappedPage --
 *      read the page from the checkpoint file
 *
 * Results:
 *      returns VMK_OK when successful, errorcode otherwise
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
Swap_GetCptSwappedPage(World_Handle *world,
                       uint32 slotNr, 
                       MPN newMPN,
                       Async_Token *token)
{
   VMK_ReturnStatus status = VMK_OK;
   Swap_ChkpointFileInfo *swapCptFile = SwapGetCptFile(world);

   if (!token) {
      ASSERT_HAS_INTERRUPTS();
   }
   VMLOG(2, world->worldID, "reading checkpoint swapped page slotNum(0x%x)", slotNr);
  
   ASSERT(swapCptFile->state == SWAP_CPT_FILE_OPEN);
   if (UNLIKELY(swapCptFile->state != SWAP_CPT_FILE_OPEN)) {
      return(VMK_FAILURE);
   }

   ASSERT(status == VMK_OK);

   ASSERT(SwapGetCptFile(world)->fileHandle);
   // Read the page contents from the swap file 
   status = SwapReadFile(world, SwapGetCptFile(world)->fileHandle, newMPN, 
                         SWAP_SLOT_2_OFFSET(slotNr), PAGE_SIZE, token);
   ASSERT(status == VMK_OK);
   if (status != VMK_OK) {
      return(status);
   }

   if (!Alloc_AllocInfo(world)->duringCheckpoint) {
      SwapInfoLock(SwapGetVmmInfo(world));
      swapCptFile->nrPagesRead++;
      SwapInfoUnlock(SwapGetVmmInfo(world));
   }

   if (swapCptFile->nrPagesRead >= swapCptFile->nrPagesToRead) {
      ASSERT(swapCptFile->nrPagesRead == swapCptFile->nrPagesToRead);
      SwapCloseCptFile(world);
   }
   return status;
}
/*
 *-----------------------------------------------------------------------------
 *
 * Swap_GetSwappedPage --
 *      read the page from the swap file
 *
 * Results:
 *      returns VMK_OK when successful, errorcode otherwise
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
Swap_GetSwappedPage(World_Handle *world,
                    uint32 slotNr, 
                    MPN newMPN,
                    Async_Token *token,
                    PPN ppn)
{
   SwapFileSlot swapFileSlot;
   VMK_ReturnStatus status;
   SwapFileInfo *swapFileInfo;
   swapFileSlot.value = slotNr;

   // Handle reads from the checkpoint file
   if (SwapIsCptFile(&swapFileSlot)) {
      return(Swap_GetCptSwappedPage(world, swapFileSlot.slot.slotNum, newMPN, token));
   }

   // Handle reads from a remote machine where we migrated from.
   if (SwapIsMigrated(&swapFileSlot)) {
      return Migrate_ReadPage(world, swapFileSlot.slot.slotNum * PAGE_SIZE,
			      newMPN, token);
   }

   VMLOG(2, world->worldID, "reading swapped page file index %d, slotNum(0x%x)", 
         swapFileSlot.slot.fileNdx, swapFileSlot.slot.slotNum);


   swapFileInfo = SwapGetSwapFile(swapFileSlot.slot.fileNdx);

   ASSERT(swapFileSlot.slot.slotNum < swapFileInfo->nrSlots);

   // Read the page contents from the swap file 
   status = SwapReadFile(world, swapFileInfo->fileHandle, newMPN, 
                         SWAP_SLOT_2_OFFSET(swapFileSlot.slot.slotNum), 
                         PAGE_SIZE, token);
   ASSERT(status == VMK_OK);
   if (status != VMK_OK) {
      VmWarn(world->worldID, "SwapReadFile failed, status = %d", status);
      return(status);
   }

   // Collect stats for the number of pages read from the file
   SwapAddNrPagesRead(swapFileInfo, 1); 

   if (swapDoSanityChecks) {
      // do checking only if we are doing synchronous reads
      if (!token) {
         Swap_DoPageSanityChecks(world, slotNr, newMPN, ppn);
      }
   }
   return status;
}

#define SWAP_READ_RETRIES	5

/*
 *-----------------------------------------------------------------------------
 *
 * SwapReadFile --
 *      If token == NULL issue a synchronous read to the swap file
 *      Else issue a async read to the swap file
 *
 * Results:
 *      returns VMK_OK when successful, errorcode otherwise
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus 
SwapReadFile(World_Handle *world,
	     FS_FileHandleID fileHandle,
             MPN mpn, 
             uint64 offset,
             uint32 nrBytes,
             Async_Token *token)
{
   VMK_ReturnStatus status = VMK_OK;
   SG_Array *sgArr;
   uint32 i, bytesRead = 0;
   uint32 nrPagesRead = 0;

   ASSERT((nrBytes % PAGE_SIZE) == 0);

   sgArr = (SG_Array *)Mem_Alloc(SG_ARRAY_SIZE(nrBytes/PAGE_SIZE));
   ASSERT(sgArr != NULL);
   if (!sgArr) {
      Warning("Unable to allocate sgArr");
      return VMK_NO_MEMORY;
   }

   sgArr->length = nrBytes / PAGE_SIZE;
   sgArr->addrType = SG_MACH_ADDR;

   for (i = 0; i < sgArr->length; ++i) {
      sgArr->sg[i].length = PAGE_SIZE;
      sgArr->sg[i].addr = MPN_2_MA(mpn + i);
      sgArr->sg[i].offset = offset + SWAP_SLOT_2_OFFSET(i);

      nrPagesRead++;
   }

   if (token) {
      int i;
      token->resID = World_GetVmmLeaderID(world);
      /* Retry the reading of the checkpoint file if there is a
       * reservation conflict, which could happen if it is on a VMFS
       * accessed by multiple hosts. */
      token->flags |= ASYNC_CANT_BLOCK;
      for (i = 0; i < SWAP_READ_RETRIES; i++) {
	 status = FSS_AsyncFileIO(fileHandle, sgArr, token,
                                  FS_READ_OP | FS_CANTBLOCK);
	 if (status != VMK_RESERVATION_CONFLICT) {
	    break;
	 }
      }
      ASSERT(status == VMK_OK);
      if (status != VMK_OK) {
         Warning( "Async read failed - status = %d", status);
         Mem_Free(sgArr);
         return status;
      } 
      if (SWAP_DEBUG_ASYNC_READS) {
         static uint32 asyncReadThrottle = 0;
         if (asyncReadThrottle % 1000 == 0) {
            LOG(0, "called %u times", asyncReadThrottle);
         }
         asyncReadThrottle++;
      }
      Mem_Free(sgArr);
      return status;
   } else {
      /*
       * Retry synchronous reads for IO connection failures.
       */
      uint32 retryCount = CONFIG_OPTION(MEM_SWAP_IO_RETRY);
      uint32 sleepTime = Swap_GetInitSleepTime();
      while (1) {
         status = FSS_SGFileIO(fileHandle, sgArr, FS_READ_OP, &bytesRead);
         if (status == VMK_OK || retryCount == 0) {
            break;
         }
         ASSERT(bytesRead == 0);
         Warning( "Swap sync read failed - status = %d retry...", status);
         CpuSched_Sleep(sleepTime);
         sleepTime = Swap_GetNextSleepTime(sleepTime);
         retryCount--;
      }
      ASSERT(status == VMK_OK);
      ASSERT(bytesRead == nrBytes);

      if (status != VMK_OK) {
         Warning("Read failed offset <0x%Lx> , MPN <0x%x> , nrBytes <%d>;", 
                 offset, mpn, nrBytes);
      }
      Mem_Free(sgArr);
      return status;
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * SwapAreCptFilesSame --
 *      
 *      Checks if the file used by the swapper is different from the 
 *      new suspend file.
 *
 * Results:
 *      TRUE - if the files are same
 *      FALSE - if the files are different
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------
 */
Bool
Swap_AreCptFilesSame(World_Handle *world,
                     VMnix_FilePhysMemIOArgs *args)
{
   Swap_ChkpointFileInfo *swapCptFile = SwapGetCptFile(world);
   VMK_ReturnStatus status;
   FS_FileAttributes userAttr, swapCptAttr;

   if (!swapCptFile->fileHandle) {
      return FALSE;
   }

   // Now check the descNum on both files
   status = FSClient_GetFileAttributes(args->handleID, &userAttr);
   ASSERT(status == VMK_OK);
   if (status != VMK_OK) {
      return FALSE;
   }

   status = FSClient_GetFileAttributes(swapCptFile->fileHandle, &swapCptAttr);
   ASSERT(status == VMK_OK);
   if (status != VMK_OK) {
      return FALSE;
   }

   if (userAttr.descNum != swapCptAttr.descNum) {
      return FALSE;
   }
   return TRUE;
}

/*
 *-----------------------------------------------------------------------------
 *
 * Swap_IsPageInCptFile --
 *      
 *      Checks if the given page is currently swapped in the checkpoint file.
 *
 * Results:
 *      TRUE - if the page is in the checkpoint file.
 *      FALSE - otherwise.
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------
 */
Bool
Swap_IsCptPFrame(Alloc_PFrame *pf)
{
   uint32 frameIndex = Alloc_PFrameGetIndex(pf);
   SwapFileSlot swapFileSlot;
   swapFileSlot.value = frameIndex;
   return Alloc_PFrameStateIsSwapped(Alloc_PFrameGetState(pf)) &&
          SwapIsCptFile(&swapFileSlot);
}

/*
 *-----------------------------------------------------------------------------
 *
 * Swap_SetCptPFrame --
 *
 *      Sets up the PPN as being swapped to the checkpoint file.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------
 */
void
Swap_SetCptPFrame(World_Handle *world, Alloc_PFrame *pf, uint64 offset)
{
   SwapFileSlot cptFileSlot;
   uint32 pageSizeOffset = offset >> 12;
   ASSERT_NOT_IMPLEMENTED((uint64)(pageSizeOffset << 12) == offset); 
   // Maximum size of the checkpoint file is restricted to
   // 8GB, this is not a problem currently as the max size of
   // a VM is currently restricted to 3.6GB
   ASSERT_NOT_IMPLEMENTED(!(pageSizeOffset & 0xffe00000));
   ASSERT(!Alloc_PFrameIsValid(pf));

   memset(&cptFileSlot, 0, sizeof(cptFileSlot));
   cptFileSlot.slot.slotNum = pageSizeOffset;
   cptFileSlot.slot.fileNdx = SWAP_CPT_FILE_INDEX;
   SwapGetCptFile(world)->nrPagesToRead++;

   Alloc_PFrameSetIndex(pf, cptFileSlot.value);
   Alloc_PFrameSetValid(pf);
   Alloc_PFrameSetState(pf, ALLOC_PFRAME_SWAPPED);
}

/*
 *-----------------------------------------------------------------------------
 *
 * Swap_GetCptStats --
 *      
 *      Get the stats about the # of pages read from the checkpoint file
 *
 * Results:
 *      sets *nrPagesToRead to the total number of pages that need to be
 *      swapped in from the checkpoint file.
 *
 *      sets *nrPagesRead to the number of pages that have already been
 *      swapped in from the checkpoint file
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------
 */
void Swap_GetCptStats(World_Handle *world,
                      uint32 *nrPagesToRead,
                      uint32 *nrPagesRead)
{
   *nrPagesRead = SwapGetCptFile(world)->nrPagesRead;
   *nrPagesToRead = SwapGetCptFile(world)->nrPagesToRead; 
}
