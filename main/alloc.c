/* **********************************************************
 * Copyright 2000 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * alloc.c --
 *
 *      Overview
 *
 *	Manage guest main memory allocations. 
 *       
 *      The VMX/VMM/VMkernel get access to the MPN backing the guest main
 *      memory specifying the offset into this pseudo memory file. This offset
 *      is usually specified in pages i.e. PPN.
 *
 *      This file could *really* use some documentation!
 *      Here is a rough stab at an outline.
 *
 *      Forward map/PPN->MPN/PFrames
 *
 *      Anon Memory/Backmap/VPN allocation
 *
 *      Page sharing/Pshare/COW/COWHints 
 *
 *      Migration
 *
 *      Checkpointing
 *
 *      Remapping
 *
 *      Page fault tokens
 *
 *      More?
 */

#include "vm_types.h"
#include "vmcore_types.h"
#include "vm_libc.h"
#include "x86.h"
#include "vmkernel.h"
#include "memmap.h"
#include "world.h"
#include "vmmem.h"
#include "alloc.h"
#include "kseg.h"
#include "kvmap.h"
#include "memalloc.h"
#include "util.h"
#include "config.h"
#include "host.h"
#include "hash.h"
#include "pshare.h"
#include "action.h"
#include "alloc_inline.h"
#include "swap.h"
#include "pagetable.h"
#include "list.h"
#include "libc.h"
#include "timer.h"
#include "vmk_scsi.h"
#include "migrateBridge.h"
#include "numa.h"
#include "memsched.h"
#include "vmkevent.h"
#include "helper.h"
#include "vmnix_syscall.h"
#include "fsSwitch.h"
#include "mpage.h"
#include "user.h"
#include "sharedArea.h"

#define LOGLEVEL_MODULE Alloc
#include "log.h"

/*
 * Compilation flags
 */

// debugging
#define	ALLOC_DEBUG			(vmx86_debug && vmx86_devel)
#define	ALLOC_DEBUG_VERBOSE		(0)

// targeted debugging
#define	ALLOC_PFRAME_DEBUG		(ALLOC_DEBUG)
#define	ALLOC_PFRAME_DEBUG_VERBOSE	(0)
#define	ALLOC_DEBUG_COW			(ALLOC_DEBUG)
#define	ALLOC_DEBUG_COW_VERBOSE		(0)
#define ALLOC_HOST_REF_COUNT_DEBUG      (ALLOC_DEBUG)
#define ALLOC_DEBUG_COS_FAULT           (ALLOC_DEBUG)
#define ALLOC_CPT_SWAP_DEBUG            (ALLOC_DEBUG)
#define	ALLOC_DEBUG_HOST_USE		(0)
#define	ALLOC_DEBUG_UNLOCK_PAGE		(0)
#define	ALLOC_DEBUG_MEM_WAIT		(0)
#define	ALLOC_DEBUG_LAZY_PDIR		(0)
#define	ALLOC_DEBUG_CHECKPOINT		(1)
#define	ALLOC_DEBUG_CHECKPOINT_VERBOSE	(0)
#define	ALLOC_DEBUG_REMAP		(1)
#define	ALLOC_DEBUG_REMAP_VERBOSE	(0)
#define ALLOC_DEBUG_BALLOON             (0)
#define	ALLOC_DEBUG_PSHARE_CONSISTENCY	(vmx86_debug)

/*
 * Constants
 */

// alloc frame pin count related constants
#define ALLOC_PIN_STICKY_COUNT   ((1 << 16) - 1)
#define ALLOC_MAX_PIN_COUNT          (ALLOC_PIN_STICKY_COUNT - 1)

// COW P2M update buffer related constants
#define P2M_SLOTNUM_TO_MPNINDEX(slotNum) ((slotNum)/PSHARE_P2M_BUFFER_SLOTS_PER_MPN)
#define P2M_SLOTNUM_TO_SLOTINDEX(slotNum) ((slotNum) % PSHARE_P2M_BUFFER_SLOTS_PER_MPN)

/*
 * Maximum pages for single PhysMemIO operation.  We choose 64 because
 * 256K seems to be the min size to get the best SCSI bandwidth.
 * Also, the chunk size should not exceed the checkpoint buffer size.
 *
 * For some cards with limited # of scatter-gather entries, vmk_scsi
 * code will break the PhysMemIO scatter-gather req into multiple requests.
 */
#define PHYS_SG_SIZE	MIN(64, ALLOC_CHECKPOINT_BUF_SIZE)

// maximum memory wait time while resuming
#define	ALLOC_RESUME_TIMEOUT_MS		(5000)

// maximum memory wait time while remapping a page to low memory
#define ALLOC_REMAP_LOW_TIMEOUT         (5000)

#define ALLOC_PDIR_ALIGNMENT            (128)

// maximum number of pages to touch when SHARE_COS stress option is set
#define ALLOC_STRESS_COS_PAGES_MAX (400)
#define ALLOC_STRESS_COS_PAGES_SLACK (50)

#define ALLOC_ANON_MPAGE_MAGIC_NUM  (0xa303)
/*
 * Types
 */
typedef struct {
   Bool valid;
   World_ID worldID;
   PPN      ppn;
   PShare_HintStatus status;
} COWHintUpdate;

typedef struct AllocP2MToken {
   KSEG_Pair *pair;
   MPN       mpn;
   PShare_P2MUpdate *ptr;
} AllocP2MToken;

typedef struct AllocPFramePair {
   Alloc_PFrame *pframe;
   KSEG_Pair *kseg;
} AllocPFramePair;

typedef struct AllocAnonMPNNode {
   MPage_Tag tag;
   uint16 magicNum;
   World_ID worldID;
   MPN prevMPN;
   MPN nextMPN;
} AllocAnonMPNNode __attribute__((packed));


/*
 * Local operations
 */

static void AllocDealloc(World_Handle *world);
static void AllocDeallocInt(World_Handle *world);

static void AllocPFTokenInit(World_Handle *world, Alloc_PageFaultToken *token,
                                    Bool isCosToken);
static Bool AllocPFTokenIsStateDone(Alloc_PageFaultToken *token);
static Bool AllocPFTokenIsStateFree(Alloc_PageFaultToken *token);
static VMK_ReturnStatus AllocPFTokenSetStateInUse(World_Handle *world,
                                                  Alloc_PageFaultToken *token,
                                                  PPN ppn, MPN mpn, uint32 slotNr,
                                                  Async_Callback callback);
static void AllocPFTokenSetStateDone(Alloc_PageFaultToken *token);
static void AllocPFTokenRelease(Alloc_PageFaultToken *token);
static void AllocCheckpointCallback(Async_Token *token);
static void AllocAsyncReadCallback(Async_Token *token);

// page fault primitives
static MPN AllocMapPageDir(World_Handle *world, MPN *dirEntry);
static VMK_ReturnStatus AllocPageFault(World_Handle *world, PPN ppn, 
                                       Bool *writeable, MPN *allocMPN, 
                                       Alloc_PageFaultSource source,
                                       Bool cptCaller);
static VMK_ReturnStatus AllocPageFaultInt(World_Handle *world, 
                                          PPN ppn, 
                                          Bool canBlock, 
                                          MPN *allocMPN, 
                                          Bool *sharedCOW, 
                                          Bool cptCaller, 
                                          Alloc_PageFaultSource source);

// copy-on-write operations
static VMK_ReturnStatus AllocCOWCopyPage(World_Handle *world,
                                         PPN ppn,
                                         MPN mpn,
                                         MPN *mpnNew,
                                         Bool fromMonitor);
static VMK_ReturnStatus AllocCOWRemoveHint(World_Handle *world,
                                           PPN ppn,
                                           MPN mpn);
static int AllocCOWCheck(World_Handle *world);
static void AllocCOWUpdateP2MDone(World_Handle *world, MPN mpnShared);
void AllocStressCOSPShare(void *clientData);

// swap operations
static VMK_ReturnStatus AllocGetSwappedPage(World_Handle *world,
                                            uint32 dirMPN,
                                            uint32 pageIndex,
                                            uint32 slotNr,
                                            MPN newMPN,
                                            PPN ppn,
                                            Alloc_PageFaultToken *pfToken);

// remapping operations
static VMK_ReturnStatus AllocRemapPage(World_Handle *world,
                                       PPN ppn,
                                       MPN mpnOld,
                                       MPN mpnNew);
static VMK_ReturnStatus AllocRemapPageLow(World_Handle *world,
                                          PPN ppn, 
                                          MPN mpnOld,
                                          uint32 msTimeout,
                                          MPN *mpnLow);
static VMK_ReturnStatus AllocRemapPageNode(World_Handle *world,
                                           PPN ppn,
                                           uint32 nodeMask,
                                           MPN mpnOld,
                                           uint32 msTimeout,
                                           MPN *mpnNode);

// checkpoint buffer operations
static void AllocCheckpointBufInit(World_Handle *world);
static void AllocCheckpointBufFree(World_Handle *world);
static MPN  AllocCheckpointBufGetPage(World_Handle *world);
static void AllocCheckpointBufRelease(World_Handle *world);
static VMK_ReturnStatus AllocCheckpointBufAlloc(World_Handle *world);
static void AllocCheckpointBufSetStartPPN(World_Handle *world, PPN ppn);
static Bool AllocCheckpointBufCheckPPN(World_Handle *world,
                                       PPN ppn,
                                       PPN *startPPN,
                                       Bool fromVMX);
static void Alloc_RetrySwapIn(void *data, Timer_AbsCycles timestamp);

// procfs and misc operations
static int AllocWorldProcPagesRead(Proc_Entry *e, char *buf, int *len);
static int AllocWorldProcNumaRead(Proc_Entry *e, char *buf, int *len);
static const char * AllocPFrameStateName(AllocPFrameState fState);
static VMK_ReturnStatus AllocAddToAnonMPNList(World_Handle *world, MPN mpn);
static VMK_ReturnStatus AllocRemoveFromAnonMPNList(World_Handle *world, MPN mpn);
static MPN AllocGetNextMPNFromAnonMPNList(World_Handle *world, MPN mpn);


/*
 * Macros
 */

#define PAGE_2_DIRINDEX(pageNum)        ((pageNum) >> ALLOC_PDIR_SHIFT) 
#define PAGE_2_PAGEINDEX(pageNum)       ((pageNum) & ALLOC_PDIR_OFFSET_MASK)


/*
 * utility function to check for a valid MPN
 */
static INLINE Bool
AllocIsValidMPN(MPN mpn, Bool heavyCheck)
{
   if (heavyCheck || ALLOC_PFRAME_DEBUG) {
      return(VMK_IsValidMPN(mpn));
   } else {
      return(mpn <= MemMap_GetLastValidMPN());
   }
}

/*
 * Locking utility operations
 */

// acquire alloc lock for "world"
static INLINE void
AllocLock(World_Handle *world)
{
   return(SP_Lock(&Alloc_AllocInfo(world)->lock));
}

// release alloc lock for "world"
static INLINE void
AllocUnlock(World_Handle *world)
{
   SP_Unlock(&Alloc_AllocInfo(world)->lock);
}

// check alloc lock for "world"
static INLINE Bool
AllocIsLocked(const World_Handle *world)
{
   return(SP_IsLocked(&Alloc_AllocInfo(world)->lock));
}

void
Alloc_Lock(World_Handle *world)
{
   AllocLock(world);
}

void
Alloc_Unlock(World_Handle *world)
{
   AllocUnlock(world);
}

static INLINE void
AllocPFrameResetAll(World_Handle *world, Alloc_PFrame *f)
{
   f->pinCount = 0;
   Alloc_PFrameSetState(f, ALLOC_PFRAME_REGULAR);
   Alloc_PFrameSetInvalid(f);
   f->index = 0;
   f->sharedArea = 0;
}

// mark frame "f" invalid
static INLINE void
AllocPFrameReset(World_Handle *world, Alloc_PFrame *f)
{
   ASSERT(Alloc_PFrameGetPinCount(f) == 0);
   AllocPFrameResetAll(world, f);
}

// set MPN associated with frame "f" to "mpn"
static void
AllocPFrameSetRegular(World_Handle *world,
                      Alloc_PFrame *f, MPN mpn)
{
   if (mpn == INVALID_MPN) {
      AllocPFrameReset(world, f);
   } else {
      // We do not want to modify the vmx use count
      Alloc_PFrameSetValid(f);
      Alloc_PFrameSetState(f, ALLOC_PFRAME_REGULAR);
      f->index = mpn;
   }
}

static INLINE AllocAnonMPNNode *
AllocMapAnonMPNNode(MPN mpn, KSEG_Pair **pair)
{
   return (AllocAnonMPNNode *)MPage_Map(mpn, pair);
}

static INLINE void 
AllocUnmapAnonMPNNode(KSEG_Pair *pair)
{
   MPage_Unmap(pair);
}

/*
 *-----------------------------------------------------------------------------
 *
 * AllocNodeStatsAdd --
 *
 *     Add to stats for world's pages on node of "mpn"
 *
 * Results:
 *     None
 *
 * Side effects:
 *     Modifies world's node stats
 *
 *-----------------------------------------------------------------------------
 */
static void
AllocNodeStatsAdd(World_Handle *world, MPN mpn, uint32 numMPNs)
{
   Atomic_Add(&Alloc_AllocInfo(world)->pagesPerNode[NUMA_MPN2NodeNum(mpn)], numMPNs);
}


/*
 *-----------------------------------------------------------------------------
 *
 * AllocNodeStatsSub --
 *
 *     Subtract from stats for world's pages on node of "mpn"
 *
 * Results:
 *     None
 *
 * Side effects:
 *     Modifies world's node stats
 *
 *-----------------------------------------------------------------------------
 */
static void
AllocNodeStatsSub(World_Handle *world, MPN mpn, uint32 numMPNs)
{
   Atomic_Sub(&Alloc_AllocInfo(world)->pagesPerNode[NUMA_MPN2NodeNum(mpn)], numMPNs);
}

/*
 *-----------------------------------------------------------------------------
 *
 * AllocPShareRemove --
 *
 *     Wrapper for PShare_Remove that properly accounts for stats
 *
 * Results:
 *     Returns result of underlying PShare_Remove
 *
 * Side effects:
 *     Updates node stats
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
AllocPShareRemove(World_Handle *world,
                   uint64 key,
                   MPN mpn,
                   uint32 *count){
   VMK_ReturnStatus status;
   status = PShare_Remove(key, mpn, count);
   if (status == VMK_OK && *count != 0) {
      AllocNodeStatsSub(world, mpn, 1);
   }
   return (status);
}



/*
 *-------------------------------------------------------------
 *
 * Alloc_IsMainMemoryBPN --
 *
 *      Checks if the given bpn belongs to main memory.  
 *
 * Results:
 *      TRUE if the bpn is main memory in this world.
 *
 * Side effects:
 *      none.
 *
 *-------------------------------------------------------------
 */
Bool
Alloc_IsMainMemBPN(World_Handle *world, BPN bpn)
{
   return World_VMMGroup(world)->mainMemHandle == BPN_2_MemHandle(bpn);
}

/*
 *-------------------------------------------------------------
 *
 * Alloc_PPNToBPN --
 *
 *      Convert the ppn corresponding to main memory to a BPN
 *      for this world.  This function assumes page argument
 *      comes from the main memory region.
 *
 * Results:
 *      BPN corresponding to this main memory page.
 *
 * Side effects:
 *      none.
 *
 *-------------------------------------------------------------
 */
BPN
Alloc_PPNToBPN(World_Handle *world, PPN ppn)
{
   return MemPage_2_BPN(World_VMMGroup(world)->mainMemHandle, ppn);
}

/*
 *-------------------------------------------------------------
 *
 * Alloc_BPNToMainMemPPN --
 *
 *      Convert the bpn corresponding to a ppn in the main memory
 *      for this world.  
 *
 * Results:
 *      PPN corresponding to this main memory page.
 *
 * Side effects:
 *      none.
 *
 *-------------------------------------------------------------
 */
PPN
Alloc_BPNToMainMemPPN(World_Handle *world, BPN bpn)
{
   ASSERT(Alloc_IsMainMemBPN(world, bpn));
   return BPN_2_PageNum(bpn);
}

/*
 *-------------------------------------------------------------
 *
 * AllocVMPageInt --
 *
 *      Wrapper to get a free MPN to be used by the VM. 
 *      Function keeps track of MPNs allocated by the VMM while
 *      swap requests are pending to that VMM. 
 *      If "low" is specified we try to allocate a page within 
 *      the first 4GB, waiting for a max of "msTimeout".
 *
 * Results:
 *      MPN if page allocated, INVALID_MPN if out of memory.
 *
 * Side effects:
 *      none.
 *
 *-------------------------------------------------------------
 */
static INLINE MPN
AllocVMPageInt(World_Handle *world,
               PPN ppn,
               MM_NodeMask nodeMask,
               MM_AllocType mmType,
               uint32 msTimeout)
{
   MPN mpn = MemMap_AllocVMPageWait(world, ppn,
                                    nodeMask, MM_COLOR_ANY, mmType,
                                    msTimeout);

   if (mpn != INVALID_MPN) {
      AllocNodeStatsAdd(world, mpn, 1);

      // keep track of number of VALID MPNs used by the VMM
      // while we have a swap request pending
      if (Swap_IsSwapReqPending(&World_VMMGroup(world)->swapInfo)) {
         Swap_AddCurAllocDuringSwap(&World_VMMGroup(world)->swapInfo, 1);
      }
   }
#ifdef VMX86_DEBUG
   if (mpn == INVALID_MPN) {
      MemSched_LogLowStateMPNUsage();
      MemMap_LogState(0);
   }
#endif
   return(mpn);
}


/*
 *-------------------------------------------------------------
 *
 * AllocFreeVMPage --
 *
 *      Free a page of the VM.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      none.
 *
 *-------------------------------------------------------------
 */
static INLINE void
AllocFreeVMPage(World_Handle *world, MPN mpn)
{
   MemMap_FreeVMPage(world, mpn);
}


/*
 *-------------------------------------------------------------
 *
 * AllocVMPage --
 * AllocVMLowPage --
 * AllocVMLowReservedPage --
 *
 *      Wrapper to get a MPN to be used by the VM.
 *
 * Results:
 *      MPN if free MPNs exists, INVALID_MPN if no more 
 *      free MPNs available.
 *
 * Side effects:
 *      none.
 *
 *-------------------------------------------------------------
 */
static INLINE MPN
AllocVMPage(World_Handle *world, PPN ppn)
{
   return AllocVMPageInt(world, ppn, MM_NODE_ANY, MM_TYPE_ANY, 0);
}

static INLINE MPN
AllocVMLowPage(World_Handle *world, PPN ppn, uint32 msTimeout)
{
   return AllocVMPageInt(world, ppn, MM_NODE_ANY, MM_TYPE_LOW, msTimeout);
}

static INLINE MPN
AllocVMLowReservedPage(World_Handle *world, PPN ppn, uint32 msTimeout)
{
   return AllocVMPageInt(world, ppn, MM_NODE_ANY, MM_TYPE_LOWRESERVED, msTimeout);
}


/*
 *-------------------------------------------------------------
 *
 * Alloc_PFrameSetRegular --
 *
 *      Externel interface to call AllocPFrameSetRegular
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      none.
 *
 *-------------------------------------------------------------
 */
void
Alloc_PFrameSetRegular(World_Handle *world, PPN ppn,
                       Alloc_PFrame *f, MPN mpn)
{
   AllocPFrameSetRegular(world, f, mpn);
}

/*
 * COW related Alloc_PFrame operations
 */

// set frame "f" to a shared COW page backed by "mpn"
static INLINE void
AllocPFrameSetCOW(Alloc_PFrame *f, MPN mpn)
{
   Alloc_PFrameSetValid(f);
   Alloc_PFrameSetState(f, ALLOC_PFRAME_COW);
   f->index = mpn;
}

// set frame "f" to COW hint page backed by "mpn"
static INLINE void
AllocPFrameSetCOWHint(Alloc_PFrame *f, MPN mpn)
{
   Alloc_PFrameSetValid(f);
   Alloc_PFrameSetState(f, ALLOC_PFRAME_COW_HINT);
   f->index = mpn;
}

/*
 * COWHintUpdate utility operations
 */

static INLINE void
COWHintUpdateInvalidate(COWHintUpdate *update)
{
   update->valid = FALSE;
}

static INLINE void
COWHintUpdateSet(COWHintUpdate *update,
                 World_ID worldID, PPN ppn, PShare_HintStatus status)
{
   update->worldID = worldID;
   update->ppn = ppn;
   update->status = status;
   update->valid = TRUE;
}

/*
 * Invalidate the frames on Deallocation
 */
static INLINE void
AllocPFrameDeallocInvalidate(World_Handle *world, Alloc_PFrame *f, PPN ppn)
{
   if (UNLIKELY(Alloc_PFrameGetPinCount(f) != 0 && ppn != INVALID_PPN)) {
      Alloc_Info *info = Alloc_AllocInfo(world);
      if (info->throttlePinCountWarnings % 128 == 0) {
         VmWarn(world->worldID, "Deallocating pinned ppn 0x%x, throttle %d.",
                ppn, info->throttlePinCountWarnings++);
      }
   }
   AllocPFrameResetAll(world, f);
}

/*
 *----------------------------------------------------------------------
 *
 * AllocPFTokenInit --
 *      Inititialize the Page fault token
 *
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
AllocPFTokenInit(World_Handle *world,
                 Alloc_PageFaultToken *token,
                 Bool isCosToken)
{
   memset(token, 0, sizeof(*token));
   token->worldID = world->worldID;
   token->token = NULL;
   token->state = FREE;
   token->ppn = INVALID_PPN;
   token->mpn = INVALID_MPN;
   token->nrRetries = 0;
   token->sleepTime = Swap_GetInitSleepTime();
   token->cosToken = isCosToken;
}

/*
 *----------------------------------------------------------------------
 *
 * AllocPFTokenIsStateDone --
 *      Utility function to check if the page fault token is in DONE state
 *
 * Results:
 *      Reutrns TRUE if page fault token's state is DONE else returns FALSE
 *
 * Side effects:
 *      none
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
AllocPFTokenIsStateDone(Alloc_PageFaultToken *token)
{
   return (token->state == DONE);
}

/*
 *----------------------------------------------------------------------
 *
 * AllocPFTokenIsStateFree --
 *      Utility function to check if the page fault token is in FREE state
 *
 *
 * Results:
 *      Reutrns TRUE if page fault token's state is FREE else returns FALSE
 *
 * Side effects:
 *      none
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
AllocPFTokenIsStateFree(Alloc_PageFaultToken *token)
{
   // sanity check
   return (token->state == FREE);
}

/*
 *----------------------------------------------------------------------
 *
 * AllocPFTokenSetStateInUse --
 *      Prepare the page fault token to be used for an async IO
 *
 * Results:
 *      Returns VMK_OK on success.
 *
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
AllocPFTokenSetStateInUse(World_Handle *world,
                          Alloc_PageFaultToken *pfToken,
                          PPN ppn,
                          MPN mpn,
                          uint32 slotNr,
                          Async_Callback callback)
{
   // sanity check
   ASSERT(AllocIsLocked(world));
   ASSERT(pfToken->state == FREE);
   pfToken->state = IN_USE;
   pfToken->worldID = world->worldID;

   ASSERT(pfToken->token == NULL);
   pfToken->token = Async_AllocToken(0);
   ASSERT(pfToken->token);
   if (!pfToken->token) {
      return(VMK_NO_MEMORY);
   }
   pfToken->token->flags = ASYNC_CALLBACK;
   pfToken->token->callback = callback;
   pfToken->token->clientData = pfToken;

   pfToken->ppn = ppn;
   pfToken->slotNr = slotNr;
   pfToken->mpn = mpn;
   pfToken->nrRetries = 0;
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * AllocPFTokenSetStateDone --
 *      Update the state of the page fault token to DONE.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
AllocPFTokenSetStateDone(Alloc_PageFaultToken *token)
{
   // sanity check
   ASSERT(token->state == IN_USE);
   token->state = DONE;
}

/*
 *----------------------------------------------------------------------
 *
 * AllocPFTokenRelease --
 *      Cleanup the page fault token after an async IO
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
AllocPFTokenRelease(Alloc_PageFaultToken *pfToken)
{
   // sanity check
   ASSERT(pfToken->state == IN_USE || pfToken->state == DONE);
   pfToken->state = FREE;
   pfToken->worldID = INVALID_WORLD_ID;
   pfToken->ppn = INVALID_PPN;
   pfToken->mpn = INVALID_MPN;
   pfToken->slotNr = (uint32)-1;
   pfToken->nrRetries = 0;
   if (pfToken->token) {
      Async_ReleaseToken(pfToken->token);
   }
   pfToken->token = NULL;

   // free a token not used by the console os
   if (!pfToken->cosToken) {
      Mem_Free(pfToken);
      pfToken = NULL;
   }
}


/*
 * Convenient wrapper operations
 */

VMK_ReturnStatus
Alloc_PageFaultWrite(World_Handle *world, PPN ppn,
                    MPN *allocMPN, Alloc_PageFaultSource source)
{
   // invoke primitive
   Bool writeable = TRUE;
   return(AllocPageFault(world, ppn, &writeable, allocMPN, source, FALSE));
}

/*
 *-----------------------------------------------------------------------------------
 *
 * AllocP2MInitToken --
 *
 *      Initialize the token before using it to access the P2M buffer.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------------
 */
static INLINE void
AllocP2MInitToken(World_Handle *world, AllocP2MToken *p2mToken)
{
   ASSERT(AllocIsLocked(world));
   p2mToken->pair = NULL;
   p2mToken->mpn = INVALID_MPN;
}

/*
 *-----------------------------------------------------------------------------------
 *
 * AllocGetP2MBufferPtr --
 *
 *      Given a slot return a ptr to the P2M buffer slot. The p2mToken keeps
 *      sufficient information to deduce if a new buffer MPN needs to be
 *      Kseg mapped or if we already have the mpn corresponding slot mapped.
 *
 * Results:
 *      pointer to the P2M buffer slot
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------------
 */
static PShare_P2MUpdate *
AllocGetP2MBufferPtr(World_Handle *world,
                     uint32 slotNum,
                     AllocP2MToken *p2mToken)
{
   uint32 mpnNdx = P2M_SLOTNUM_TO_MPNINDEX(slotNum);
   uint32 slotNdx = P2M_SLOTNUM_TO_SLOTINDEX(slotNum);
   Alloc_Info *info = Alloc_AllocInfo(world);
   MPN reqdMPN;
   ASSERT(AllocIsLocked(world));
   ASSERT(mpnNdx < PSHARE_P2M_BUFFER_MPNS_MAX);
   ASSERT(slotNum < info->numP2MSlots);
   reqdMPN = info->p2mUpdateBuffer[mpnNdx];
   ASSERT(reqdMPN != INVALID_MPN);

   if (p2mToken->pair) {
      ASSERT(p2mToken->mpn != INVALID_MPN);
      if (p2mToken->mpn == reqdMPN) {
         return(&p2mToken->ptr[slotNdx]);
      }
      Kseg_ReleasePtr(p2mToken->pair);
      p2mToken->pair = NULL;
   }
   p2mToken->ptr = Kseg_MapMPN(reqdMPN, &p2mToken->pair);
   p2mToken->mpn = reqdMPN;
   return(&p2mToken->ptr[slotNdx]);
}

/*
 *-----------------------------------------------------------------------------------
 *
 * AllocP2MReleaseToken --
 *
 *      Releases any kseg mapped region pointed to be the p2mToken.
 *
 * Results:
 *      none
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------------
 */
static INLINE void
AllocP2MReleaseToken(World_Handle *world, AllocP2MToken *p2mToken)
{
   ASSERT(AllocIsLocked(world));
   if (p2mToken->pair) {
      Kseg_ReleasePtr(p2mToken->pair);
      p2mToken->pair = NULL;
      p2mToken->mpn = INVALID_MPN;
   }
}

/*
 *-----------------------------------------------------------------------------------
 *
 * AllocGetFrameInfoFromPPN --
 *
 *      Get Frame info like MPN, AllocPFrameState for the given ppn.
 *      Note: frameMPN is set to
 *            shared MPN if frame is COW,
 *            corresponding mpn if frame is COW_HINT or a REGULAR frame
 *            and INVALID_MPN for all other cases
 *
 * Results:
 *      VMK_OK on success, error code otherwise.
 *
 * Side effects:
 *      none.
 *
 *-----------------------------------------------------------------------------------
 */
static VMK_ReturnStatus
AllocGetFrameInfoFromPPN(World_Handle *world,
                         PPN ppn,
                         AllocPFrameState *frameState,
                         MPN *frameMPN)
{
   VMK_ReturnStatus status;
   uint32 dirIndex, pageIndex;
   MPN dirMPN;
   Alloc_PageInfo *pageInfo;
   KSEG_Pair *dirPair;
   Alloc_PFrame *dir;

   // convenient abbrevs, sanity check
   pageInfo = &Alloc_AllocInfo(world)->vmPages;
   ASSERT(AllocIsLocked(world));
   status = Alloc_LookupPPN(world, ppn, &dirIndex, &pageIndex);
   ASSERT(status == VMK_OK);
   if (status != VMK_OK) {
      World_Panic(world, "ppn=0x%x Alloc_LookupPPN failed", ppn);
      return(status);
   }
   // lookup page frame directory, fail if not found
   dirMPN = pageInfo->pages[dirIndex];
   ASSERT(dirMPN != INVALID_MPN);
   if (dirMPN == INVALID_MPN) {
      World_Panic(world, "ppn=0x%x unmapped: dirIndex 0x%x", ppn, dirIndex);
      return(VMK_FAILURE);
   }

   // map page frame, extract state
   dir = Kseg_MapMPN(dirMPN, &dirPair);
   *frameState = Alloc_PFrameGetState(&dir[pageIndex]);
   *frameMPN = Alloc_PFrameGetMPN(&dir[pageIndex]);
   Kseg_ReleasePtr(dirPair);

   return(VMK_OK);
}

/*
 *-----------------------------------------------------------------------------
 *
 * AllocGetPFrameFromPPN --
 *
 *     Looks up the Alloc_PFrame structure for the given "ppn" in "world"
 *     Stores the resulting PFrame and its corresponding KSEG_Pair in
 *     *pair.
 *
 *     Note: You must call AllocPFrameReleasePair when you're done with pair
 *
 * Results:
 *     Returns VMK_OK if the PFrame was found, VMK_FAILURE otherwise
 *
 * Side effects:
 *     KSeg's memory for page directory, so it must be released
 *
 *-----------------------------------------------------------------------------
 */
static INLINE VMK_ReturnStatus
AllocGetPFrameFromPPN(World_Handle *world,
                      PPN ppn,
                      AllocPFramePair *pair)
{
   VMK_ReturnStatus status;
   uint32 dirIndex, pageIndex;
   MPN dirMPN;
   Alloc_PageInfo *pageInfo;
   Alloc_PFrame *dir;

   // convenient abbrevs, sanity check
   pageInfo = &Alloc_AllocInfo(world)->vmPages;
   ASSERT(AllocIsLocked(world));
   status = Alloc_LookupPPN(world, ppn, &dirIndex, &pageIndex);
   ASSERT(status == VMK_OK);
   if (status != VMK_OK) {
      World_Panic(world, "ppn=0x%x Alloc_LookupPPN failed", ppn);
      return(status);
   }
   // lookup page frame directory, fail if not found
   dirMPN = pageInfo->pages[dirIndex];
   if (dirMPN == INVALID_MPN) {
      return(VMK_FAILURE);
   }

   // map page frame, extract state
   dir = Kseg_MapMPN(dirMPN, &pair->kseg);
   pair->pframe = &dir[pageIndex];
   return(VMK_OK);
}

/*
 *-----------------------------------------------------------------------------
 *
 * AllocPFrameReleasePair --
 *
 *     Unmaps the page directory stored in "pair"
 *
 * Results:
 *     None.
 *
 * Side effects:
 *     Unmaps the page directory stored in "pair"
 *
 *-----------------------------------------------------------------------------
 */
void
AllocPFrameReleasePair(AllocPFramePair *pair)
{
   Kseg_ReleasePtr(pair->kseg);
}

/*
 * Operations
 */

void
Alloc_Init(void)
{
   // sanity checks
   ASSERT(sizeof(Alloc_PFrame) == sizeof(uint64));
   ASSERT(sizeof(AllocAnonMPNNode) <= sizeof(MPage));

   // debugging
   if (ALLOC_PFRAME_DEBUG) {
      LOG(0, "Alloc: sizeof(Alloc_PFrame)=%d", sizeof(Alloc_PFrame));
   }
}

/*
 *--------------------------------------------------------------------------
 * AllocAddWorldProcEntries --
 *     This code is called after the /proc/vmware/vm/<ID>/alloc directory
 *     has been set up.   It registers proc entries for page
 *     allocation info specific to each world.
 *
 * Results:
 *     None.
 *
 * Side effects:
 *     None.
 *--------------------------------------------------------------------------
 */
static void
AllocAddWorldProcEntries(World_Handle *world)
{
   Alloc_Info *info = Alloc_AllocInfo(world);

   // "alloc" directory
   Proc_InitEntry(&info->procDir);
   info->procDir.parent = &world->procWorldDir;
   Proc_Register(&info->procDir, "alloc", TRUE);

   // Summarize world page table (/proc/vmware/vm/*/alloc/pages)
   Proc_InitEntry(&info->procPages);
   info->procPages.parent = &info->procDir;
   info->procPages.read = AllocWorldProcPagesRead;
   info->procPages.write = NULL;
   info->procPages.private = (void*)world->worldID;
   Proc_RegisterHidden(&info->procPages, "pages", FALSE);

   // Dump pagesPerNode (/proc/vmware/vm/*/alloc/numa)
   Proc_InitEntry(&info->procNuma);
   info->procNuma.parent = &info->procDir;
   info->procNuma.read   = AllocWorldProcNumaRead;
   info->procNuma.write  = NULL;
   info->procNuma.private = world;
   Proc_Register(&info->procNuma, "numa", FALSE);
}


/*
 *----------------------------------------------------------------
 *
 * Alloc_WorldInit --
 *
 *      Initialize the per world alloc data
 *
 * Results:
 *      VMK_OK
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------
 */
VMK_ReturnStatus
Alloc_WorldInit(World_Handle *world, World_InitArgs *args)
{
   Alloc_Info *info = Alloc_AllocInfo(world);
   Alloc_PageInfo *pageInfo = &info->vmPages;
   uint32 i, numMPNs, maxPhysPages;
   Sched_MemClientConfig *memConfig = &args->sched->mem;
   Sched_GroupConfig *groupConfig = &args->sched->group;
   VMK_ReturnStatus status;

   // alloc only relevant for vmm worlds.
   ASSERT(World_IsVMMWorld(world));

   if (!World_IsVmmLeader(world)) {
      return VMK_OK;
   }

   // initialize anon mpn list head
   info->anonMPNHead = INVALID_MPN;

   // Initialize all buffer MPNs to INVALID_MPN
   for (i = 0; i < PSHARE_P2M_BUFFER_MPNS_MAX; i++) {
      info->p2mUpdateBuffer[i] = INVALID_MPN;
   }

   numMPNs = MAX(PSHARE_P2M_BUFFER_MPNS_DEFAULT,
                 CONFIG_OPTION(MEM_NUM_P2M_BUF_MPNS));
   numMPNs = MIN(numMPNs, PSHARE_P2M_BUFFER_MPNS_MAX);
   for (i = 0; i < numMPNs; i++) {
      KSEG_Pair *pair;
      PShare_P2MUpdate *data;
      uint32 j;
      info->p2mUpdateBuffer[i] = MemMap_AllocAnyKernelPage();
      ASSERT(info->p2mUpdateBuffer[i] != INVALID_MPN);
      if (info->p2mUpdateBuffer[i] == INVALID_MPN) {
         VmWarn(world->worldID,
                "Could not allocate mpn for p2mUpdateBuffer[%d]", i);
         return(VMK_NO_RESOURCES);
      }
      MemMap_SetIOProtection(info->p2mUpdateBuffer[i],
                             MMIOPROT_IO_DISABLE);
      data = Kseg_MapMPN(info->p2mUpdateBuffer[i], &pair);
      for (j = 0; j < PSHARE_P2M_BUFFER_SLOTS_PER_MPN; j++) {
         data[j].bpn = INVALID_BPN;
         data[j].mpn = INVALID_MPN;
      }
      Kseg_ReleasePtr(pair);
   }
   info->numP2MSlots = numMPNs * PSHARE_P2M_BUFFER_SLOTS_PER_MPN;
   info->p2mFill = 0;
   info->p2mDrain = 0;
   info->p2mUpdateTotal = 0;
   info->p2mUpdateCur = 0;
   info->p2mUpdatePeak = 0;

   info->cosNextStressPPN = 0;
   info->cosStressInProgress = FALSE;

   // initialize remap requests
   info->remapLowNext = 0;
   info->remapLowPeak = 0;
   for (i = 0; i < ALLOC_REMAP_LOW_REQUESTS_MAX; i++) {
      info->remapLow[i] = INVALID_PPN;
   }
   info->remapLowTotal = 0;

   // initialize hint update buffer
   info->hintUpdateNext = 0;
   info->hintUpdateOverflow = FALSE;
   info->hintUpdatePeak = 0;
   for (i = 0; i < PSHARE_HINT_UPDATES_MAX; i++) {
      info->hintUpdate[i].bpn = INVALID_BPN;
      info->hintUpdate[i].status = PSHARE_HINT_NONE;
   }
   info->hintUpdateTotal = 0;

   // initialize the p2m mapping cache
   for (i = 0; i < ALLOC_P_2_M_CACHE_SIZE; i++) {
      info->p2mCache[i].firstPPN = INVALID_PPN;
      info->p2mCache[i].lastPPN  = INVALID_PPN;
      info->p2mCache[i].maddr = -1;
   }

   // initialize checkpoint state
   info->startingCheckpoint = FALSE;
   info->cptSharesDonated = FALSE;
   info->duringCheckpoint   = FALSE;
   info->dummyMPN = INVALID_MPN;
   info->maxCptPagesToRead = 0;
   info->cptPagesRead = 0;

   // initialize the token to handle page faults from the console os
   // asynchronously
   AllocPFTokenInit(world, &info->cosToken, TRUE);

   // initialize lock
   SP_InitLock("allocLock", &info->lock, SP_RANK_ALLOC);
   // rank check
   ASSERT(SP_RANK_ALLOC < SP_RANK_FILEMAP && 
          SP_RANK_ALLOC < SP_RANK_MEMSCHED);

   // initialize NUMA page stats
   for (i=0; i < NUMA_MAX_NODES; i++) {
      Atomic_Write(&info->pagesPerNode[i], 0);
   }

   // create COW hint update monitor action, fail if unable
   info->hintUpdateAction = Action_Alloc(world, "COWHint");
   if (info->hintUpdateAction == ACTION_INVALID) {
      VmWarn(world->worldID, "unable to allocate COW hint update action");
      return VMK_NO_RESOURCES;
   }

   // create high-priority COW PPN->MPN update monitor action, fail if unable
   info->p2mUpdateAction = Action_Alloc(world, "P2MUpdate");
   if (info->p2mUpdateAction == ACTION_INVALID) {
      VmWarn(world->worldID, "unable to allocate COW update action");
      return VMK_NO_RESOURCES;
   }

   // create page remapping monitor action, fail if unable
   info->remapPickupAction = Action_Alloc(world, "RemapPickup");
   if (info->remapPickupAction == ACTION_INVALID) {
      VmWarn(world->worldID, "unable to allocate page remap pickup action");
      return VMK_NO_RESOURCES;
   }

   // monitor anon memory
   pageInfo->numAnonPages = memConfig->numAnon;

   // COS VMX overhead memory (VMX is COS iff VMM world leader is the group leader.) 
   if (World_IsGroupLeader(world)) {
      pageInfo->cosVmxInfo.numOverheadPages = memConfig->numOverhead;
   } else {
      pageInfo->cosVmxInfo.numOverheadPages = 0;
   }

   // reserve overhead/anon memory 
   status = MemSched_ReserveMem(world, pageInfo->numAnonPages +
                                       pageInfo->cosVmxInfo.numOverheadPages);
   if (status != VMK_OK) {
      return status;
   }

   // guest physical memory
   maxPhysPages = groupConfig->mem.max;
   ASSERT(maxPhysPages <= ALLOC_MAX_NUM_GUEST_PAGES);
   pageInfo->numPhysPages = maxPhysPages;
   pageInfo->valid = TRUE;

   // initialize alloc table for guest physical memory
   pageInfo->numPDirEntries = PAGE_2_DIRINDEX(maxPhysPages) + 1;
   pageInfo->pages = (MPN *) World_Align(world, pageInfo->numPDirEntries * sizeof(MPN),
                                         ALLOC_PDIR_ALIGNMENT);
   ASSERT(pageInfo->pages != NULL);
   for (i = 0; i < pageInfo->numPDirEntries; i++) {
      pageInfo->pages[i] = INVALID_MPN;
   }

   // debugging
   VMLOG(0, world->worldID,
         "numPhysPages=%u, "
         "numOverheadPages=%u, "
         "numAnonPages=%u, ",
         pageInfo->numPhysPages,
         pageInfo->cosVmxInfo.numOverheadPages,
         pageInfo->numAnonPages);

   // initialize checkpoint buffer
   AllocCheckpointBufInit(world);

   AllocAddWorldProcEntries(world);

   return VMK_OK;
}

/*
 *--------------------------------------------------------------------------
 * AllocRemoveWorldProcEntries
 *     This code is called when the procFs nodes are removed from a world.
 *
 * Results:
 *     None.
 *
 * Side effects:
 *     None.
 *--------------------------------------------------------------------------
 */
static void
AllocRemoveWorldProcEntries(World_Handle *world)
{
   Alloc_Info *info = Alloc_AllocInfo(world);
   Proc_Remove(&info->procPages);
   Proc_Remove(&info->procNuma);
   Proc_Remove(&info->procDir);
}


/*
 *--------------------------------------------------------------------------
 *
 * Alloc_WorldCleanup --
 *
 *     Undo Alloc_WorldInit.
 *
 * Results:
 *     None
 *
 * Side effects:
 *     See Alloc_Dealloc
 *
 *--------------------------------------------------------------------------
 */
void
Alloc_WorldCleanup(World_Handle *world)
{
   // alloc only relevant for vmm worlds.  Also POST worlds for ksegPOST
   ASSERT(World_IsVMMWorld(world));

   if (World_IsVmmLeader(world)) {
      Alloc_Info *info = Alloc_AllocInfo(world);
      Alloc_PageInfo *pageInfo = &info->vmPages;
      MemSched_UnreserveMem(world, pageInfo->numAnonPages + 
                                   pageInfo->cosVmxInfo.numOverheadPages);

      info->hintUpdateAction = ACTION_INVALID;
      info->p2mUpdateAction = ACTION_INVALID;
      info->remapPickupAction = ACTION_INVALID;

      AllocRemoveWorldProcEntries(world);

      Alloc_CheckpointCleanup(world);
      AllocDealloc(world);
      SP_CleanupLock(&info->lock);
   }
}

/*
 *--------------------------------------------------------------------------
 *
 * AllocInvalidateCacheEntry --
 *
 *	Invalidates entry "ce" if it contains "ppn".
 *
 * Results:
 *      Returns TRUE if "ce" was invalidated.
 *
 * Side effects:
 *	None.
 *
 *--------------------------------------------------------------------------
 */
static INLINE Bool
AllocInvalidateCacheEntry(Alloc_P2M *ce, PPN ppn)
{
   // invalidate if ppn match in cache entry
   if (ce->firstPPN == ppn || ce->lastPPN == ppn) {
      ce->firstPPN = INVALID_PPN;
      ce->lastPPN  = INVALID_PPN;
      ce->maddr = -1;
      ce->copyHints = 0;
      return(TRUE);
   } else {
      return(FALSE);
   }
}

/*
 *--------------------------------------------------------------------------
 *
 * Alloc_InvalidateCache --
 *
 *	Invalidates any entries containing "ppn" in the
 *      PPN to MPN mapping cache for "world".
 *
 * Results:
 *      Returns TRUE if any cache entries were invalidated.
 *
 * Side effects:
 *	None.
 *
 *--------------------------------------------------------------------------
 */
Bool
Alloc_InvalidateCache(World_Handle *world, PPN ppn)
{
   Bool invalidated = FALSE;
   Alloc_P2M *ce;

   /*
    * Cached mapping spans at most two pages.  The ppn to be
    * invalidated could therefore appear in both the cached
    * entry for ppn and the cached entry for (ppn - 1).
    */

   // invalidate if ppn match in cached entry for [ppn, ppn+1]
   ce = Alloc_CacheEntry(world, ppn);
   if (AllocInvalidateCacheEntry(ce, ppn)) {
      invalidated = TRUE;
   }

   // invalidate if ppn match in cached entry for [ppn-1, ppn]
   ce = Alloc_CacheEntry(world, ppn - 1);
   if (AllocInvalidateCacheEntry(ce, ppn)) {
      invalidated = TRUE;
   }

   return(invalidated);
}

/*
 *--------------------------------------------------------------------------
 *
 * Alloc_IsCached --
 *
 *      Checks if the PPN to MPN mapping cache for "world" has
 *	any entries containing "ppn".
 *
 * Results:
 *      Returns TRUE if cache contains "ppn", otherwise FALSE.
 *
 * Side effects:
 *	None.
 *
 *--------------------------------------------------------------------------
 */
Bool
Alloc_IsCached(World_Handle *world, PPN ppn)
{
   Alloc_P2M *ce;

   // Cached mapping spans at most two pages.  Must check
   // cached entries for both ppn and (ppn - 1).

   // check if ppn match in cached entry for [ppn, ppn+1]
   ce = Alloc_CacheEntry(world, ppn);
   if ((ce->firstPPN == ppn) || (ce->lastPPN == ppn)) {
      return(TRUE);
   }

   // check if ppn match in cached entry for [ppn-1, ppn]
   ce = Alloc_CacheEntry(world, ppn - 1);
   if ((ce->firstPPN == ppn) || (ce->lastPPN == ppn)) {
      return(TRUE);
   }

   // ppn not in cache
   return(FALSE);
}


/*
 *----------------------------------------------------------------------
 *
 * Alloc_LookupPPN --
 *
 *      Lookup the ppn in the pseudo memory file and
 *	compute various corresponding offsets and indexes.
 *
 * Results:
 *      Returns VMK_OK if successful, otherwise error code.
 *	Sets "dirIndex" to the associated page frame directory index.
 *	Sets "pageIndex" to the associated page frame page index.
 *	Sets "ppn" to the associated guest PPN, or INVALID_PPN if none.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Alloc_LookupPPN(World_Handle *world,
                PPN ppn,
                uint32 *dirIndex,
                uint32 *pageIndex)
{
   Alloc_PageInfo *pageInfo;

   // convenient abbrev
   pageInfo = &Alloc_AllocInfo(world)->vmPages;

   // sanity checks
   ASSERT(ppn != INVALID_PPN);
   ASSERT(pageInfo->pages != NULL);

   // lookup PPN, fail if invalid
   if ((ppn >= pageInfo->numPhysPages)) {
      VmWarn(world->worldID, "ppn=0x%x out of range: 0x%x-0x%x",
             ppn, 0, pageInfo->numPhysPages);
      return(VMK_BAD_PARAM);
   }

   // compute page frame location
   *dirIndex  = PAGE_2_DIRINDEX(ppn);
   ASSERT(*dirIndex < pageInfo->numPDirEntries);
   *pageIndex = PAGE_2_PAGEINDEX(ppn);

   // succeed
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * AllocMemWait --
 *
 *      Wait until there is sufficient free memory for "world"
 *	to safely continue execution.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May cause the caller to block until sufficient memory
 *	becomes available.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
AllocMemWaitInt(World_Handle *world, Bool locked)
{
   // block world pending sufficient memory, if necessary
   if (locked) {
      // wait for sufficient memory w/o holding lock
      AllocUnlock(world);
      MemSched_BlockWhileMemLow(world);
      AllocLock(world);
   } else {
      MemSched_BlockWhileMemLow(world);
   }
}

static INLINE void
AllocMemWait(World_Handle *world)
{
   SP_AssertNoLocksHeld();

   // invoke primitive
   AllocMemWaitInt(world, FALSE);
}

static INLINE void
AllocMemWaitLock(World_Handle *world)
{
   // sanity check
   ASSERT(AllocIsLocked(world));

   // invoke primitive
   AllocMemWaitInt(world, TRUE);
}


/*
 *----------------------------------------------------------------------
 *
 * AllocPPNToMPN --
 *
 *      Sets "mpn" to the machine page corresponding to physical page
 *	"ppn" in "world".  If "writeable" is set, ensures that the
 *	returned page is writeable. If "canBlock" is set, may block
 *	(e.g. if page is swapped).  Caller must hold "world" alloc lock.
 *
 * Results:
 *      Returns VMK_OK on success, otherwise returns the error status.
 *	If successful, sets "mpn" to the paged mapped by "ppn".
 *
 * Side effects:
 *      A machine page may be allocated.
 *	The "world" alloc lock may be transiently released.
 *	This function may block if canBlock is set
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
AllocPPNToMPN(World_Handle *world,
              PPN ppn,
              Bool writeable,
              Bool canBlock,
              MPN *mpn)
{
   VMK_ReturnStatus status;
   Bool sharedCOW;

   // block if insufficient memory
   if (canBlock) {
      AllocMemWaitLock(world);
   }

 retry:
   // sanity check
   ASSERT(AllocIsLocked(world));

   // lookup existing PPN->MPN mapping
   status = AllocPageFaultInt(world, ppn,
                              canBlock, mpn, &sharedCOW, 
                              FALSE, ALLOC_FROM_VMKERNEL);
   if (status != VMK_OK) {
      return(status);
   }

   // make private copy of COW page, if necessary
   if (writeable && sharedCOW) {
      MPN copyMPN;

      // attempt to copy page, drop lock for duration of call
      AllocUnlock(world);
      status = AllocCOWCopyPage(world, ppn, *mpn, &copyMPN, FALSE);
      AllocLock(world);

      // retry since lock was dropped
      if ((status == VMK_OK) || (status == VMK_NOT_SHARED)) {
         goto retry;
      }

      // unable to make private copy
      VmWarn(world->worldID, "COW copy failed: ppn=0x%x, mpn=0x%x",
                ppn, *mpn);
      *mpn = INVALID_MPN;
      return(VMK_FAILURE);
   }

   // sanity check
   ASSERT(AllocIsValidMPN(*mpn, TRUE));

   // unshared page
   return(VMK_OK);
}


/*
 *----------------------------------------------------------------------
 *
 * Alloc_TouchPages --
 *
 *      For every set bit in changeMap, fault the page in.
 *
 * Results:
 *      VMK_OK on success, an error otherwise.
 *
 * Side effects:
 *      May cause pages to be paged in from the network [hot migration]
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Alloc_TouchPages(World_Handle *world, uint8 *changeMap, uint32 changeMapLength)
{
   VMK_ReturnStatus status;
   int i,j;

   AllocLock(world);
   for (i = 0; i < changeMapLength; i++) {
      if (changeMap[i] != 0) {
	 for (j = 0; j < 8; j++) {
	    if (changeMap[i] & (1 << j)) {
	       MPN mpn;
	       PPN ppn = i * 8 + j;

             status = AllocPPNToMPN(world, ppn, FALSE, TRUE, &mpn);
             if (status != VMK_OK) {
                  AllocUnlock(world);
                  return status;
               }
             LOG(2, "Paged in page %d", ppn);
          }
       }
      }
   }
   AllocUnlock(world);
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * AllocCleanupPendingP2MUpdates --
 *
 *      Go through all the pending P2M updates and do the necessary
 *      cleanup.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
static void
AllocCleanupPendingP2MUpdates(World_Handle *world)
{
   Alloc_Info *info;
   AllocP2MToken p2mToken;
   uint32 numMPNs, i;
   uint32 throttle = 0;

   info = Alloc_AllocInfo(world);
   AllocLock(world);
   if (vmx86_debug) {
      VMLOG(1, world->worldID,"p2mTotal = %d, p2mPeak = %d, p2mCurr = %d",
            info->p2mUpdateTotal, info->p2mUpdatePeak, info->p2mUpdateCur);
   }

   if (info->p2mDrain != info->p2mFill) {
      VmLog(world->worldID, "P2M buffer is not empty, doing cleanup");
   }
   AllocP2MInitToken(world, &p2mToken);
   while (info->p2mDrain != info->p2mFill) {
      PShare_P2MUpdate *bufferPtr;
      bufferPtr = AllocGetP2MBufferPtr(world, info->p2mDrain, &p2mToken);
      ASSERT(bufferPtr->mpn != INVALID_MPN);
      ASSERT(bufferPtr->bpn != INVALID_BPN);
      throttle++;
      if ((bufferPtr->mpn == INVALID_MPN) ||
          (bufferPtr->bpn == INVALID_BPN)) {
         MPN bufMPN;
         BPN bufPage;
         bufMPN = bufferPtr->mpn;
         bufPage = bufferPtr->bpn;
         AllocP2MReleaseToken(world, &p2mToken);
         AllocUnlock(world);
         World_Panic(world, "CleanupPendingP2MUpdates, inconsistent p2m buffer state,"
                    "drain = %d, fill = %d, mpn = 0x%x, page = 0x%x",
                    info->p2mDrain, info->p2mFill, bufMPN, bufPage);
         return;
      }
      // invoke primitive
      AllocCOWUpdateP2MDone(world, bufferPtr->mpn);
      bufferPtr->bpn = INVALID_BPN;
      bufferPtr->mpn = INVALID_MPN;
      info->p2mDrain = (info->p2mDrain + 1) % info->numP2MSlots;
      if (ALLOC_PFRAME_DEBUG && throttle % 1000 == 0) {
         VmWarn(world->worldID, "throttle = %d", throttle);
      }
   }
   AllocP2MReleaseToken(world, &p2mToken);

   numMPNs = info->numP2MSlots / PSHARE_P2M_BUFFER_SLOTS_PER_MPN;
   for (i = 0; i < numMPNs; i++) {
      ASSERT(info->p2mUpdateBuffer[i] != INVALID_MPN);
      MemMap_FreeKernelPage(info->p2mUpdateBuffer[i]);
      info->p2mUpdateBuffer[i] = INVALID_MPN;
      info->numP2MSlots = 0;
   }
   AllocUnlock(world);
}


/*
 *----------------------------------------------------------------------
 *
 * AllocP2MUpdateExistsForBPN --
 *
 *      Check if a pending p2m update exists for a given bpn.
 *
 * Results:
 *      Returns TRUE if the p2m update exists.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
#ifdef VMX86_DEBUG
static Bool
AllocP2MUpdateExistsForBPN(World_Handle *world, BPN bpn)
{
   AllocP2MToken p2mToken;
   Alloc_Info *info = Alloc_AllocInfo(world);
   uint32 next;

   ASSERT(AllocIsLocked(world));
   AllocP2MInitToken(world, &p2mToken);
   for (next = info->p2mDrain; next != info->p2mFill; 
        next = (next + 1) % info->numP2MSlots) {
      PShare_P2MUpdate *bufferPtr;
      bufferPtr = AllocGetP2MBufferPtr(world, info->p2mDrain, &p2mToken);
      ASSERT(bufferPtr->mpn != INVALID_MPN);
      ASSERT(bufferPtr->bpn != INVALID_BPN);
      if (bufferPtr->bpn == bpn) {
         AllocP2MReleaseToken(world, &p2mToken);
         return TRUE;         
      }
   }
   AllocP2MReleaseToken(world, &p2mToken);
   return FALSE;
}
#endif

/*
 *----------------------------------------------------------------------
 *
 * AllocDealloc --
 *
 *      Deallocate all machine memory allocated for this world.
 *
 * Results:
 *      Returns VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 *      Deallocates memory.
 *
 *----------------------------------------------------------------------
 */
static void
AllocDealloc(World_Handle *world)
{
   Alloc_Info *info = Alloc_AllocInfo(world);

   // sanity check
   ASSERT(World_IsVmmLeader(world));

   // perform page sharing consistency check, if appropriate
   if (ALLOC_DEBUG_COW && PShare_IsEnabled()) {
      int nBad = AllocCOWCheck(world);
      if (nBad > 0) {
         VmWarn(world->worldID, "COWCheck: nBad=%d", nBad);
      }
   }

   // remove any pending COW reference counts *before* deallocating the pagedirs.
   AllocCleanupPendingP2MUpdates(world);

   // deallocate machine memory pages
   AllocDeallocInt(world);

   // deallocate "dummy" page used for checkpoints, if any
   if (info->dummyMPN != INVALID_MPN) {
      MemMap_FreeKernelPage(info->dummyMPN);
      VMLOG(0, world->worldID, "deallocated dummy mpn=0x%x", info->dummyMPN);
      info->dummyMPN = INVALID_MPN;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * AllocDeallocPFrame --
 *
 *      Free machine memory page associated with "f" in "world".
 *	The specified "ppn" is used for pages that are being swapped out.
 *	Caller must hold "world" alloc lock.
 *
 * Results:
 *      Returns VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 *      Memory may be reclaimed.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
AllocDeallocPFrame(World_Handle *world, Alloc_PFrame *f, PPN ppn)
{
   VMK_ReturnStatus status;
   uint32 frameIndex;
   AllocPFrameState frameState;
   World_ID worldID;
   MPN frameMPN;
   uint64 key;

   // convenient abbrev
   worldID = world->worldID;

   // extract pframe values
   frameState = Alloc_PFrameGetState(f);
   frameIndex = Alloc_PFrameGetIndex(f);
   frameMPN   = Alloc_PFrameGetMPN(f);

   // done if page invalid
   if (!Alloc_PFrameIsValid(f)){
      return(VMK_OK);
   }

   // valid copy-on-write page?
   if (Alloc_PFrameStateIsCOW(frameState)) {
      uint32 count;

      // lookup pshare entry
      status = PShare_LookupByMPN(frameMPN, &key, &count);
      if (status != VMK_OK) {
         VmWarn(worldID, "pshare lookup failed: mpn 0x%x", frameMPN);
         return(status);
      }

      // drop pshare reference, reclaim if last one
      status = AllocPShareRemove(world, key, frameMPN, &count);
      ASSERT(status == VMK_OK);
      if (status != VMK_OK) {
         VmWarn(worldID, "pshare remove failed: key 0x%Lx", key);
         return(status);
      }
      if (count == 0) {
         // reclaim unreferenced MPN
         ASSERT(frameMPN != INVALID_MPN);
         AllocFreeVMPage(world, frameMPN);
      }

      // invalidate pframe, succeed
      AllocPFrameDeallocInvalidate(world, f, ppn);
      return(VMK_OK);
   }

   // valid copy-on-write hint page?
   if (Alloc_PFrameStateIsCOWHint(frameState)) {
      World_ID hintWorld;
      PPN hintPPN;

      // lookup hint
      status = PShare_LookupHint(frameMPN, &key, &hintWorld, &hintPPN);
      if (status != VMK_OK) {
         VmWarn(worldID, "hint lookup failed: mpn 0x%x", frameMPN);
         return(status);
      }

      // remove hint
      status = PShare_RemoveHint(frameMPN, hintWorld, hintPPN);
      if (status != VMK_OK) {
         VmWarn(worldID, "hint remove failed: mpn 0x%x", frameMPN);
         return(status);
      }

      // reclaim page
      ASSERT(frameMPN != INVALID_MPN);
      AllocFreeVMPage(world, frameMPN);

      // invalidate pframe, succeed
      AllocPFrameDeallocInvalidate(world, f, ppn);
      return(VMK_OK);
   }

   // if page is being swapped in
   if (Alloc_PFrameStateIsSwapIn(frameState)) {
      // Making this MPN invalid is an indication
      // to the function reading this page that this
      // page is to be deallocated
      Alloc_PFrameSetIndex(f, INVALID_MPN);
      return(VMK_OK);
   }

   // if page is being swapped out
   if (Alloc_PFrameStateIsSwapOut(frameState)) {
      // reclaim page
      ASSERT(frameIndex != INVALID_MPN);
      Alloc_PFrameSetState(f, ALLOC_PFRAME_REGULAR);
      AllocFreeVMPage(world, frameIndex);
      return(VMK_OK);
   }

   // if page already swapped
   if (Alloc_PFrameStateIsSwapped(frameState)) {
      // free the file slot
      Swap_FreeFileSlot(world, frameIndex);
      Alloc_PFrameSetState(f, ALLOC_PFRAME_REGULAR);
      return(VMK_OK);
   }

   // reclaim ordinary page
   ASSERT(Alloc_PFrameIsValid(f));
   ASSERT(Alloc_PFrameIsRegular(f));
   ASSERT(frameMPN != INVALID_MPN);

   // invalidate pframe, succeed
   AllocPFrameDeallocInvalidate(world, f, ppn);

   AllocFreeVMPage(world, frameMPN);
   return(VMK_OK);
}

#define ALLOC_DEALLOC_YIELD_COUNT       1000
static INLINE uint32 AllocDeallocYield(uint32 yieldCount)
{
   yieldCount++;
   if (yieldCount >= ALLOC_DEALLOC_YIELD_COUNT) {
      yieldCount = 0;
      CpuSched_YieldThrottled();
   }
   return yieldCount;
} 

/*
 *----------------------------------------------------------------------
 *
 * AllocDeallocInt --
 *
 *      Free all machine memory pages associated with "world".
 *
 *      The function can only be called during world cleanup by
 *      the vmm leader.  All other VMM worlds should have been
 *      reaped, so no alloc lock is needed.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Memory is reclaimed.
 *
 *----------------------------------------------------------------------
 */
static void
AllocDeallocInt(World_Handle *world)
{
   Alloc_Info *info = Alloc_AllocInfo(world);
   Alloc_PageInfo *pageInfo = &info->vmPages;
   MemSchedVmmUsage *usage = MemSched_ClientVmmUsage(world);
   uint32 yieldCount = 0;

   // sanity check
   ASSERT((World_IsVmmLeader(world) && world->readerCount == 0) ||
          World_IsPOSTWorld(world));

   // deallocate memory
   if (pageInfo->pages != NULL) {
      uint32 i, j;
      uint32 vmNumPages = pageInfo->numPhysPages;

      // for each page frame directory
      for (i = 0; i < pageInfo->numPDirEntries; i++) {
	 if (pageInfo->pages[i] != INVALID_MPN) {
	    MPN dirMPN = pageInfo->pages[i];
            KSEG_Pair *dirPair;
	    Alloc_PFrame *dir;

            // for each page frame
	    dir = Kseg_MapMPN(dirMPN, &dirPair);
	    for (j = 0; j < PAGE_SIZE / sizeof(Alloc_PFrame); j++) {
               uint32 ppn;

               ppn = (i * (PAGE_SIZE / sizeof(Alloc_PFrame))) + j;
               if (ppn >= vmNumPages) {
                  ppn = INVALID_PPN;
               }
               (void) AllocDeallocPFrame(world, &dir[j], ppn);
            }

            // reclaim page frame directory
	    Kseg_ReleasePtr(dirPair);
	    MemMap_FreeKernelPage(dirMPN);
            pageInfo->pages[i] = INVALID_MPN;
	 }
         CpuSched_YieldThrottled(); 
      }

      World_Free(world, pageInfo->pages);
      pageInfo->pages = NULL;
   }

   // deallocate overhead memory
   if (pageInfo->cosVmxInfo.ovhdPages != NULL) {
      uint32 i, j;

      // for each page frame directory
      for (i = 0; i < pageInfo->cosVmxInfo.numOvhdPDirEntries; i++) {
	 if (pageInfo->cosVmxInfo.ovhdPages[i] != INVALID_MPN) {
	    MPN dirMPN = pageInfo->cosVmxInfo.ovhdPages[i];
            KSEG_Pair *dirPair;
	    Alloc_PFrame *dir;

            // for each page frame
	    dir = Kseg_MapMPN(dirMPN, &dirPair);
	    for (j = 0; j < PAGE_SIZE / sizeof(Alloc_PFrame); j++) {
               if (Alloc_PFrameIsValid(&dir[j])) {
                  MPN frameMPN = Alloc_PFrameGetMPN(&dir[j]);
                  if (!Alloc_PFrameIsSharedArea(&dir[j])) {
                     AllocFreeVMPage(world, frameMPN);
                  }
                  AllocPFrameDeallocInvalidate(world, &dir[j], INVALID_PPN);
               }
            }

            // reclaim page frame directory
	    Kseg_ReleasePtr(dirPair);
	    MemMap_FreeKernelPage(dirMPN);
            pageInfo->cosVmxInfo.ovhdPages[i] = INVALID_MPN;
	 }
         CpuSched_YieldThrottled(); 
      }
      World_Free(world, pageInfo->cosVmxInfo.ovhdPages);
      pageInfo->cosVmxInfo.ovhdPages = NULL;
   }

   // Deallocate anon mpns
   while (info->anonMPNHead != INVALID_MPN) {
      VMK_ReturnStatus status;
      MPN mpn = info->anonMPNHead;
      status = AllocRemoveFromAnonMPNList(world, mpn);
      ASSERT(status == VMK_OK);
      if (status != VMK_OK) {
         VmWarn(world->worldID, "Failed to release anon mpn 0x%x", mpn);
         break;
      }
      // release the anon mpn
      AllocFreeVMPage(world, mpn);

      yieldCount = AllocDeallocYield(yieldCount);
   }

   // reset state
   memset(usage, 0, sizeof(*usage));
   memset(pageInfo, 0, sizeof(Alloc_PageInfo));
}

/*
 *----------------------------------------------------------------------
 *
 * AllocUpdateAnonReservedInt --
 *
 *      Attempts to change the number of overhead pages reserved
 *	for the "world" by "pageDelta".
 *
 * Results:
 *	Returns VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 *      May modify overhead memory reservation for current world.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
AllocUpdateAnonReservedInt(World_Handle *world, int32 pageDelta)
{
   int32 nPages ;
   VMK_ReturnStatus status = VMK_OK;
   Alloc_PageInfo *pageInfo = &Alloc_AllocInfo(world)->vmPages;

   if (pageDelta == 0) {
      return VMK_BAD_PARAM;
   }

   AllocLock(world);
   nPages = pageInfo->numAnonPages + pageDelta;
   if (nPages < 0) {
      AllocUnlock(world);
      return(VMK_LIMIT_EXCEEDED);
   }

   // adjust MemSched overhead reservation
   if (pageDelta > 0) {
      status = MemSched_ReserveMem(world, pageDelta);
   } else {
      MemSched_UnreserveMem(world, -pageDelta);
   }
   if (status == VMK_OK) {
      pageInfo->numAnonPages = nPages;
   }
   AllocUnlock(world);

   return(status);
}


/*
 *----------------------------------------------------------------------
 *
 * Alloc_UpdateAnonReserved --
 *
 *      Attempts to change the number of overhead pages reserved
 *	for the current world by "pageDelta".
 *
 * Results:
 *	Returns VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 *      May modify overhead memory reservation for current world.
 *
 *----------------------------------------------------------------------
 */
VMKERNEL_ENTRY
Alloc_UpdateAnonReserved(DECLARE_1_ARG(VMK_UPDATE_ANON_MEM_RESERVED,
                                       int32, pageDelta))
{
   // process args, invoke primitive
   PROCESS_1_ARG(VMK_UPDATE_ANON_MEM_RESERVED, uint32, nPages);
   return(AllocUpdateAnonReservedInt(MY_VMM_GROUP_LEADER, nPages));
}


/*
 *----------------------------------------------------------------------
 *
 * Alloc_OverheadMemMap --
 *
 *      Configure VA range mapped for the overhead memory of world
 *	identified by "worldID".  Only the vmm leader should 
 *      call this function.
 *
 * Results:
 *      Returns VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 *      Initializes Alloc data structures associated with "worldID".
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Alloc_OverheadMemMap(World_ID worldID, VA start)
{
   uint32 i;
   Alloc_PageInfo *pageInfo;
   World_Handle *world;

   // ensure start address page aligned
   if (PAGE_OFFSET(start) != 0) {
      return(VMK_BAD_PARAM);
   }

   // acquire world handle, fail if unable
   if ((world = World_Find(worldID)) == NULL) {
      return VMK_NOT_FOUND;
   }

   // must be called from COS world
   ASSERT(World_IsHOSTWorld(MY_RUNNING_WORLD));
   ASSERT(World_IsVmmLeader(world));

   // convenient abbrevs
   pageInfo = &Alloc_AllocInfo(world)->vmPages;

   // alloc has been initialized
   if (pageInfo->valid != TRUE) {
      return VMK_FAILURE;
   }

   // create alloc page table for overhead memory
   pageInfo->cosVmxInfo.numOvhdPDirEntries = PAGE_2_DIRINDEX(ALLOC_MAX_NUM_OVHD_PAGES);
   pageInfo->cosVmxInfo.ovhdPages = (MPN *) World_Align(world, 
                                             pageInfo->cosVmxInfo.numOvhdPDirEntries * sizeof(MPN),
                                             ALLOC_PDIR_ALIGNMENT);
   ASSERT(pageInfo->cosVmxInfo.ovhdPages != NULL);
   for (i = 0; i < pageInfo->cosVmxInfo.numOvhdPDirEntries; i++) {
      pageInfo->cosVmxInfo.ovhdPages[i] = INVALID_MPN;
   }

   // virtual address space
   pageInfo->cosVmxInfo.vmxOvhdMemVPN = VA_2_VPN(start);

   // debugging
   VMLOG(0, worldID, "vmxOvhdMemVPN=%u, ", pageInfo->cosVmxInfo.vmxOvhdMemVPN);

   World_Release(world);
   return(VMK_OK);
}


/*
 *----------------------------------------------------------------------
 *
 * Alloc_COSPhysPageFault --
 *
 *      Handle a fault from the host for the world identified by
 *	"worldID" for accessing the specified guest physical memory
 *      "ppn". Sets "mpn" to the machine page number corresponding 
 *      to "ppn", or to INVALID_MPN if error.
 *
 * Results:
 *	Returns VMK_OK if successful, otherwise error code.
 *	Returns VMK_WOULD_BLOCK if would block for extended period.
 *      Returns VMK_FAILURE, on failure
 *      Sets "mpn" to MPN for "ppn", or INVALID_MPN if error.
 *
 * Side effects:
 *      A machine page may be allocated.
 *      May block, e.g. if the mpn associated with "ppn" is swapped.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Alloc_COSPhysPageFault(World_ID worldID, PPN ppn, MPN *mpn)
{
   World_Handle *world;
   MPN allocMPN;
   VMK_ReturnStatus status;

   // default to invalid MPN
   allocMPN = INVALID_MPN;

   // acquire world handle, fail if unable
   if ((world = World_Find(worldID)) == NULL) {
      WarnVmNotFound(worldID);
      CopyToHost(mpn, &allocMPN, sizeof(allocMPN));
      return VMK_BAD_PARAM;
   }

   // force nopage handler to retry when memory tight
   if (MemSched_HostShouldWait(world)) {
      /*
       * During checkpoint, we do not block requests for guest
       * physical memory becuase we return a dummy mpn.
       * Hence we do not wait.
       */
      if (!Alloc_AllocInfo(world)->duringCheckpoint) {
         return VMK_WOULD_BLOCK;
      }
   }

   // conservatively assume all accesses are writes,
   status = Alloc_PageFaultWrite(world, ppn, &allocMPN, ALLOC_FROM_COS);
   ASSERT(status == VMK_OK || allocMPN == INVALID_MPN);

   // copyout MPN
   CopyToHost(mpn, &allocMPN, sizeof(allocMPN));

   // release world handle, succeed
   World_Release(world);
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * Alloc_MapSharedAreaPage --
 *
 *      Create a mapping for the shared area overhead memory.  This
 *      comes from a special Mem_Map which needs to use mpns already
 *      allocated by the vmkernel.  This can be deleted once we
 *      kill the console os...
 *
 *
 * Results:
 *	Returns VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Alloc_MapSharedAreaPage(World_Handle *world, VPN userVPN, MPN mpn)
{
   Alloc_PageInfo *pageInfo = &Alloc_AllocInfo(world)->vmPages;
   uint32 pageOffset = userVPN - pageInfo->cosVmxInfo.vmxOvhdMemVPN;
   Alloc_PFrame *dir;
   KSEG_Pair *dirPair;
   MPN dirMPN;
   uint32 dirIndex, pageIndex;
   
   AllocLock(world);
   dirIndex = PAGE_2_DIRINDEX(pageOffset);
   pageIndex = PAGE_2_PAGEINDEX(pageOffset);
   dirMPN = pageInfo->cosVmxInfo.ovhdPages[dirIndex];
   if (dirMPN == INVALID_MPN) {
      // create new page frame directory
      dirMPN = AllocMapPageDir(world, &pageInfo->cosVmxInfo.ovhdPages[dirIndex]);
      ASSERT(dirMPN != INVALID_MPN);
      ASSERT(dirMPN == pageInfo->cosVmxInfo.ovhdPages[dirIndex]);
   }
   dir = Kseg_MapMPN(dirMPN, &dirPair);
   AllocPFrameSetRegular(world, &dir[pageIndex], mpn);
   Alloc_PFrameSetSharedArea(&dir[pageIndex]);
   Kseg_ReleasePtr(dirPair);
   AllocUnlock(world);
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Alloc_OvhdPageFault --
 *
 *      Handle a fault from the host for the world identified by
 *	"worldID", at the specified "vpn" in the virtual address
 *	space of the world.  Sets "mpn" to the machine page number
 *	corresponding to "ppn", or to INVALID_MPN if error.
 *
 * Results:
 *	Returns VMK_OK if successful, otherwise error code.
 *	Returns VMK_WOULD_BLOCK if would block for extended period.
 *      Returns VMK_FAILURE, on failure
 *      Sets "mpn" to MPN for "ppn", or INVALID_MPN if error.
 *
 * Side effects:
 *      A machine page may be allocated.
 *      May block, e.g. if the ppn associated is swapped.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
AllocOvhdPageFault(World_Handle *world, VPN userVPN, MPN *mpn)
{
   VMK_ReturnStatus status = VMK_OK;

   // default to invalid MPN
   *mpn = INVALID_MPN;

   if (MemSched_HostShouldWait(world)) {
      if (Alloc_AllocInfo(world)->duringCheckpoint) {
         /*
          * During checkpoint, we allocate free mpns for overhead
          * memory pages, so we check if have sufficient memory to
          * burn otherwise we do the not so cool thing of terminating
          * the checkpoint, but this case should be extremely rare and
          * if we ever hit it we will atleast not take the whole
          * system down.
          */
          if (MemSched_TerminateCptOnLowMem(world)) {
             // Fail checkpointing.
             status = VMK_FAILURE;
          }
      } else {
          status = VMK_WOULD_BLOCK;
      }
   } else {
      Alloc_PageInfo *pageInfo = &Alloc_AllocInfo(world)->vmPages;
      MemSchedVmmUsage *usage = MemSched_ClientVmmUsage(world);
      uint32 pageOffset = userVPN - pageInfo->cosVmxInfo.vmxOvhdMemVPN;

      ASSERT(pageInfo->cosVmxInfo.ovhdPages);
      if (pageOffset < pageInfo->cosVmxInfo.numOverheadPages) {
         Alloc_PFrame *dir;
         KSEG_Pair *dirPair;
         MPN dirMPN;
         uint32 dirIndex, pageIndex;
         Bool zeroPage = FALSE;

         AllocLock(world);
         dirIndex = PAGE_2_DIRINDEX(pageOffset);
         pageIndex = PAGE_2_PAGEINDEX(pageOffset);
         dirMPN = pageInfo->cosVmxInfo.ovhdPages[dirIndex];
         if (dirMPN == INVALID_MPN) {
            // create new page frame directory
            dirMPN = AllocMapPageDir(world, &pageInfo->cosVmxInfo.ovhdPages[dirIndex]);
            ASSERT(dirMPN == pageInfo->cosVmxInfo.ovhdPages[dirIndex]);
            ASSERT(dirMPN != INVALID_MPN);
         }

         dir = Kseg_MapMPN(dirMPN, &dirPair);

         // extract flags, index, mpn
         *mpn = Alloc_PFrameGetMPN(&dir[pageIndex]);
         if (*mpn != INVALID_MPN) {
            ASSERT(Alloc_PFrameIsValid(&dir[pageIndex]));
            ASSERT(Alloc_PFrameIsRegular(&dir[pageIndex]));
         } else {
            ASSERT(!Alloc_PFrameIsSharedArea(&dir[pageIndex]));
            *mpn = AllocVMPage(world, INVALID_PPN);
            AllocPFrameSetRegular(world, &dir[pageIndex], *mpn);
            zeroPage = TRUE;

            // update overhead memory usage
            usage->overhead++;
         }

         Kseg_ReleasePtr(dirPair);

         if (zeroPage) {
            // zero page contents (required for overhead pages)
            status = Util_ZeroMPN(*mpn);
            if (status != VMK_OK) {
               return status;
            }
         }

         AllocUnlock(world);

         if (*mpn != INVALID_MPN) {
            status = VMK_OK; 
         } else {
            status = VMK_NO_MEMORY;
         }
      } else {
         VmWarn(world->worldID, "Invalid ovhd page request:vpn=0x%x,range:[0x%x,x%x)",
                userVPN, pageInfo->cosVmxInfo.vmxOvhdMemVPN, 
                pageInfo->cosVmxInfo.vmxOvhdMemVPN + pageInfo->cosVmxInfo.numOverheadPages);
         status = VMK_BAD_PARAM;
      }
   }

   return(status);
}


VMK_ReturnStatus
Alloc_OvhdPageFault(World_ID worldID, VPN userVPN, MPN *mpn)
{
   MPN allocMPN;
   VMK_ReturnStatus status;
   World_Handle *world;
   // acquire world handle, fail if unable
   if ((world = World_Find(worldID)) == NULL) {
      WarnVmNotFound(worldID);
      return VMK_NOT_FOUND;
   }

   status = AllocOvhdPageFault(world, userVPN, &allocMPN);
   // copyout MPN
   CopyToHost(mpn, &allocMPN, sizeof(allocMPN));

   World_Release(world);
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * Alloc_UserWorldPhysPageFault --
 *
 *      Handle a fault from the user world for the world identified by
 *	"worldID", at the specified "ppn".
 *	Sets "mpn" to the machine page number
 *	corresponding to "ppn", or to INVALID_MPN if error.
 *
 * Results:
 *	Returns VMK_OK if successful, otherwise error code.
 *      Sets "mpn" to MPN for "ppn", or INVALID_MPN if error.
 *
 * Side effects:
 *      A machine page may be allocated.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Alloc_UserWorldPhysPageFault(World_ID worldID, PPN ppn, MPN *mpn)
{
   World_Handle *world;
   VMK_ReturnStatus status;

   ASSERT(World_IsUSERWorld(MY_RUNNING_WORLD));

   *mpn = INVALID_MPN;
   // acquire world handle, fail if unable
   if ((world = World_Find(worldID)) == NULL) {
      WarnVmNotFound(worldID);
      return(VMK_BAD_PARAM);
   }
   ASSERT(World_IsVMMWorld(world));
   status = Alloc_PageFaultWrite(world, ppn, mpn, ALLOC_FROM_USERWORLD);
   if (status != VMK_OK) {
      VmWarn(worldID, "PhysPageFault failed %s: ppn=0x%x, mpn=0x%x",
             VMK_ReturnStatusToString(status), ppn, *mpn);
   }
   // release world handle, succeed
   World_Release(world);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Alloc_PageFault --
 *
 *      Handle a fault for "world" at the specified PPN "ppn".
 *	If "writeable" is set, ensures that the page is writeable.
 *	The MPN "*mpn" is set to the machine page number
 *	corresponding to "ppn".
 *
 * Results:
 *	Returns VMK_OK if successful, otherwise error code.
 *	Returns VMKBAD_PARAM if the worldID is invalid.
 *	*mpn is set ot INVALID_MPN in the case of a bad world ID.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Alloc_PageFault(World_Handle *world,
		PPN ppn,
                Bool writeable,
		MPN *mpn)
{
   VMK_ReturnStatus status;

   status = AllocPageFault(world, ppn, &writeable, mpn, 
                           ALLOC_FROM_VMKERNEL, FALSE);
   if (!(status == VMK_OK || status == VMK_WOULD_BLOCK)) {
      VMLOG(1, world->worldID, "failed: status=%d", status);
   }

   return(status);
}


/*
 *----------------------------------------------------------------------
 *
 * Alloc_LookupMPN --
 *
 *      Obtain the MPN associated with the specified virtual address
 *	in the specified world. This function should only be called
 *      when COS VMX is used.
 *
 * Results:
 *      The MPN corresponding to "addr", or INVALID_MPN if error.
 *
 * Side effects:
 *      A machine page may be allocated.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Alloc_LookupMPN(World_ID worldID, VPN userVPN, MPN *outMPN)
{
   World_Handle *world;
   VMK_ReturnStatus status;

   // acquire world handle, fail if unable
   if ((world = World_Find(worldID)) == NULL) {
      WarnVmNotFound(worldID);
      return VMK_NOT_FOUND;
   }

   ASSERT(Alloc_AllocInfo(world)->vmPages.cosVmxInfo.ovhdPages != NULL);
   status = AllocOvhdPageFault(world, userVPN, outMPN);

   World_Release(world);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Alloc_LookupMPNFromWorld --
 *
 *      Obtain the MPN associated with the specified virtual address
 *	in the VMX world for the current VMM world. 
 *
 * Results:
 *      The MPN corresponding to "addr", or INVALID_MPN if error.
 *
 * Side effects:
 *      A machine page may be allocated and locked.
 *
 *----------------------------------------------------------------------
 */
VMKERNEL_ENTRY
Alloc_LookupMPNFromWorld(DECLARE_2_ARGS(VMK_LOOKUP_MPN,
                                        VPN, userVPN,
                                        MPN *, mpn))
{
   VMK_ReturnStatus status;
   World_Handle *groupLeader = World_Find(World_GetGroupLeaderID(MY_RUNNING_WORLD));

   PROCESS_2_ARGS(VMK_LOOKUP_MPN, VPN, userVPN, MPN *, mpn);

   if (groupLeader == NULL) {
      return VMK_NOT_FOUND;
   }

   if (World_IsUSERWorld(groupLeader)) {
      status = User_GetPageMPN(groupLeader, userVPN, USER_PAGE_PINNED, mpn);
   } else {
      ASSERT(World_IsVMMWorld(groupLeader));
      // assert that overhead memory is allocated through alloc
      ASSERT(Alloc_AllocInfo(groupLeader)->vmPages.cosVmxInfo.ovhdPages != NULL);
      status = AllocOvhdPageFault(groupLeader, userVPN, mpn);
   }
   if (status != VMK_OK) {
      VmWarn(groupLeader->worldID, "failed: userVPN=0x%x, mpn=0x%x",
             userVPN, *mpn);
   }
   World_Release(groupLeader);

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * AllocPageFault --
 *
 *      Handle a fault for "ppn" in "world".  If "writeable" is
 *	set, ensures that the returned page is writeable.  Sets
 *	"writeable" to FALSE if the returned page is read-only.
 *
 * Results:
 *      Returns MPN for "ppn", or INVALID_MPN if error.
 *
 * Side effects:
 *      This function *will* block if the page is swapped out.
 *      May allocate a page and/or make private copy of COW page.
 *      May cause a page to be swapped in.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
AllocPageFault(World_Handle *world,
               PPN ppn,
               Bool *writeable,
               MPN *allocMPN,
               Alloc_PageFaultSource source,
               Bool cptCaller)
{
   Bool sharedCOW;
   VMK_ReturnStatus status;

   // block if insufficient memory
   AllocMemWait(world);

 retry:
   // invoke primitive
   AllocLock(world);
   status = AllocPageFaultInt(world, ppn, TRUE, allocMPN, &sharedCOW, 
                              cptCaller, source);
   AllocUnlock(world);

   // make private copy of COW page, if necessary
   if (*writeable && sharedCOW) {
      MPN mpnNew;

      // attempt to copy page, retry if no longer shared
      status = AllocCOWCopyPage(world, ppn, *allocMPN, &mpnNew, 
                                source == ALLOC_FROM_MONITOR);
      // retry since lock was dropped
      if ((status == VMK_OK) || (status == VMK_NOT_SHARED)) {
         goto retry;
      }

      // unable to make private copy
      VmWarn(world->worldID, "COW copy failed: ppn=0x%x, mpn=0x%x",
             ppn, *allocMPN);
      *writeable = FALSE;
      return(status);
   }

   if (status == VMK_OK) {
      ASSERT(AllocIsValidMPN(*allocMPN, TRUE));
   }
   // return page, indicate if writeable
   *writeable = (sharedCOW) ? FALSE : TRUE;
   return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * AllocMapPageDir --
 *
 *      Handle a fault for "dirIndex" in "world".
 *
 * Results:
 *      Returns MPN for "dirIndex", or INVALID_MPN if error.
 *
 * Side effects:
 *      May allocate a page.
 *
 *----------------------------------------------------------------------
 */
static MPN
AllocMapPageDir(World_Handle *world, MPN *dirEntry)
{
   MPN dirMPN = INVALID_MPN;
   Alloc_PageInfo *pageInfo;

   // sanity check
   ASSERT(AllocIsLocked(world));

   // convenient abbrev
   pageInfo = &Alloc_AllocInfo(world)->vmPages;

   // create if none exists
   if (*dirEntry == INVALID_MPN) {
      KSEG_Pair *dirPair;
      Alloc_PFrame *dir;
      uint32 i;

      // allocate page, fail if unable
      dirMPN = MemMap_AllocAnyKernelPage();
      ASSERT(dirMPN != INVALID_MPN);
      if (dirMPN == INVALID_MPN) {
         return dirMPN;
      }

      MemMap_SetIOProtection(dirMPN, MMIOPROT_IO_DISABLE);

      // initialize page frame directory
      *dirEntry = dirMPN;
      dir = Kseg_MapMPN(dirMPN, &dirPair);
      for (i = 0; i < PAGE_SIZE / sizeof(Alloc_PFrame); i++) {
         AllocPFrameResetAll(world, &dir[i]);
      }
      Kseg_ReleasePtr(dirPair);

      // debugging
      if (ALLOC_DEBUG_LAZY_PDIR) {
         VMLOG(0, world->worldID, "lazy alloc: dirEntry=0x%p", dirEntry);
      }
   }

   return dirMPN;
}

MPN
Alloc_MapPageDir(World_Handle *world, MPN *dirEntry)
{
   // invoke primitive
   return(AllocMapPageDir(world, dirEntry));
}

/*
 *----------------------------------------------------------------------
 *
 * AllocPageFaultInt --
 *
 *      Handle a fault for page at "ppn" in "world".
 *	If "canBlock" is set, may block to read a swapped page.
 *	Caller must hold "world" alloc lock.
 *    
 *      NOTE: regular pages when first allocated are zeroed out.
 *            large pages on the other hand are never zeroed.
 *
 * Results:
 *      Returns VMK_OK if successful
 *      Returns VMK_WOULD_BLOCK if the ppn is swapped and canBlock is
 *      set to False
 *
 *      Sets "allocMPN" with the mpn for "ppn", or INVALID_MPN if error.
 *	Sets "sharedCOW" TRUE if returned MPN is shared copy-on-write,
 *	otherwise sets "sharedCOW" FALSE.
 *
 * Side effects:
 *      A page may be allocated.
 *	The "world" alloc lock may be transiently released.
 *	If canBlock is set, this function *will block* if the ppn is swapped
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
AllocPageFaultInt(World_Handle *world, PPN ppn, 
                  Bool canBlock, MPN *allocMPN, Bool *sharedCOW, Bool cptCaller,
                  Alloc_PageFaultSource source)
{
   uint32 dirIndex, pageIndex, frameIndex;
   MPN dirMPN, frameMPN;
   KSEG_Pair *dirPair, *dataPair;
   Alloc_PageInfo *pageInfo;
   VMK_ReturnStatus status;
   Alloc_PFrame *dir;
   AllocPFrameState frameState;
   MemSchedVmmUsage *usage;
   World_ID worldID;
   Bool frameValid;
   uint32 *data;
   Bool fromCOS = (source == ALLOC_FROM_COS);
   Bool fromVMX = (source == ALLOC_FROM_COS || source == ALLOC_FROM_USERWORLD);

   // sanity check
   ASSERT(AllocIsLocked(world));
   ASSERT(ppn != INVALID_PPN);

   // default reply value
   *sharedCOW = FALSE;
   *allocMPN  = INVALID_MPN;

   // convenient abbrevs
   worldID = world->worldID;
   pageInfo = &Alloc_AllocInfo(world)->vmPages;
   usage = MemSched_ClientVmmUsage(world);

   // lookup user virtual address, fail if unable
   status = Alloc_LookupPPN(world, ppn, &dirIndex, 
                            &pageIndex);
   if (status != VMK_OK) {
      return(status);
   }

   if (Alloc_AllocInfo(world)->duringCheckpoint) {
      PPN startPPN;
      if (!cptCaller && 
          !AllocCheckpointBufCheckPPN(world, ppn, &startPPN, fromVMX)) {
         // During checkpoint, If we get requests for a page that is
         // not in the current chunk of pages being written to the
         // checkpoint file we return VMK_BUSY and the callers should
         // handle this situation correctly, which might include the
         // VM being killed.
         if (ALLOC_DEBUG_CHECKPOINT) {
            VmWarn(world->worldID, "During checkpoint, received request for "
                   "ppn(0x%x), checkpoint startPPN(0x%x),"
                   "returning VMK_BUSY",
                   ppn, startPPN);
         }
         return (VMK_BUSY);
      }
   }

swap_in_retry:
   // lookup page frame directory
   dirMPN = pageInfo->pages[dirIndex];
   if (dirMPN == INVALID_MPN) {
      // special case: checkpointing guest physical memory
      if (Alloc_AllocInfo(world)->duringCheckpoint) {
	 ASSERT(Alloc_AllocInfo(world)->dummyMPN != INVALID_MPN);
         *allocMPN = Alloc_AllocInfo(world)->dummyMPN;
         ASSERT(AllocIsValidMPN(*allocMPN, FALSE));
         return(VMK_OK);
      }

      // create new page frame directory
      dirMPN = AllocMapPageDir(world, &pageInfo->pages[dirIndex]);
      ASSERT(dirMPN == pageInfo->pages[dirIndex]);
   }
   ASSERT(dirMPN != INVALID_MPN);

   // map existing page directory
   dir = Kseg_MapMPN(dirMPN, &dirPair);

   // extract flags, index, mpn
   frameState = Alloc_PFrameGetState(&dir[pageIndex]);
   frameIndex = Alloc_PFrameGetIndex(&dir[pageIndex]);
   frameMPN = Alloc_PFrameGetMPN(&dir[pageIndex]);
   frameValid = Alloc_PFrameIsValid(&dir[pageIndex]);

   Kseg_ReleasePtr(dirPair);

   // valid copy-on-write page?
   if (Alloc_PFrameStateIsCOW(frameState)) {
      // sanity check
      ASSERT(frameValid);

      // consistency check
      if (ALLOC_DEBUG_PSHARE_CONSISTENCY) {
         uint32 count;
         uint64 key;

         // lookup pshare info, fail if unable
         status = PShare_LookupByMPN(frameMPN, &key, &count);
         ASSERT(status == VMK_OK);
         if (status != VMK_OK) {
            VmWarn(worldID, "pshare lookup failed: mpn=0x%x", frameMPN);
            return(status);
         }
      }

      // special case: checkpointing
      if (Alloc_AllocInfo(world)->duringCheckpoint) {
         // make transient copy if non-VMFS access (i.e. COS fault)
         if (fromVMX) {
            KSEG_Pair *bufPair;
            uint32 *buf;
            MPN bufMPN;

            // optimization: avoid making redundant copy of zero page
            //   simply use zero-filled dummy page instead
            if (PShare_IsZeroMPN(frameMPN)) {
	       ASSERT(Alloc_AllocInfo(world)->dummyMPN != INVALID_MPN);
               *allocMPN = Alloc_AllocInfo(world)->dummyMPN;
               ASSERT(AllocIsValidMPN(*allocMPN, FALSE));
               return(VMK_OK);
            }

            // allocate checkpoint buffer page, fail if unable
            bufMPN = AllocCheckpointBufGetPage(world);
            if ((bufMPN == INVALID_MPN) || (frameMPN == INVALID_MPN)) {
               VmWarn(worldID, "COW checkpoint failed: ppn=0x%x", ppn);
               return(VMK_FAILURE);
            }

            // copy shared page
            data = Kseg_MapMPN(frameMPN, &dataPair);
            buf = Kseg_MapMPN(bufMPN, &bufPair);
            memcpy(buf, data, PAGE_SIZE);
            Kseg_ReleasePtr(bufPair);
            Kseg_ReleasePtr(dataPair);

            // debugging
            if (ALLOC_DEBUG_CHECKPOINT_VERBOSE) {
               VMLOG(0, worldID, "COW ckpt: ppn=0x%x, mpn=0x%x", ppn, bufMPN);
            }

            // set reply value, succeed
            *allocMPN = bufMPN;
            ASSERT(AllocIsValidMPN(*allocMPN, FALSE));
            return(VMK_OK);
         }
      }

      // set reply value, succeed
      *sharedCOW = TRUE;
      *allocMPN = frameMPN;
      ASSERT(AllocIsValidMPN(*allocMPN, FALSE));
      return(VMK_OK);
   }

   // valid copy-on-write hint page?
   if (Alloc_PFrameStateIsCOWHint(frameState)) {
      // sanity check
      ASSERT(frameValid);

      // consistency check
      if (ALLOC_DEBUG_PSHARE_CONSISTENCY) {
         World_ID hintWorld;
         PPN hintPPN;
         uint64 key;

         // lookup pshare info, fail if unable
         status = PShare_LookupHint(frameMPN, &key, &hintWorld, &hintPPN);
         ASSERT(status == VMK_OK);
         if (status != VMK_OK) {
            VmWarn(worldID, "hint lookup failed: mpn=0x%x", frameMPN);
            return(status);
         }
      }

      // succeed
      *allocMPN = frameMPN;
      ASSERT(AllocIsValidMPN(*allocMPN, FALSE));
      return(VMK_OK);
   }

   // is the page already being read from the swap file ?
   if (Alloc_PFrameStateIsSwapIn(frameState)) {
      MPN mpn;

      if (!canBlock) {
         *allocMPN = INVALID_MPN;
         VMLOG(0, worldID, "swapin conflict detected: non-blocking case");
         return(VMK_WOULD_BLOCK);
      }

      // If faulting from the COS, we do not want to block
      // irrespective of the value in canBlock
      if (fromCOS) {
         *allocMPN = INVALID_MPN;
         return(VMK_WOULD_BLOCK);
      }

      dir = Kseg_MapMPN(dirMPN, &dirPair);
      mpn = Alloc_PFrameGetIndex(&dir[pageIndex]);
      Kseg_ReleasePtr(dirPair);

      // Release the allocInfo lock and
      // go to sleep...waiting on the page to be read from
      // the swap file by another kernel thread
      CpuSched_Wait((uint32)mpn,
                    CPUSCHED_WAIT_SWAPIN, 
                    &Alloc_AllocInfo(world)->lock);

      // debugging
      if (ALLOC_PFRAME_DEBUG) {
         VMLOG(0, worldID, "swapin conflict detected: wakeup on mpn=0x%x", mpn);
      }

      // reacquire the alloc lock
      AllocLock(world);

      goto swap_in_retry;
   }

   // is the page *being* swapped ?
   if (Alloc_PFrameStateIsSwapOut(frameState)) {
      // map page directory again for update
      dir = Kseg_MapMPN(dirMPN, &dirPair);

      // Clear the swap out flags
      Alloc_PFrameSetState(&dir[pageIndex], ALLOC_PFRAME_REGULAR);
      Kseg_ReleasePtr(dirPair);
      ASSERT(frameIndex != INVALID_MPN);
      *allocMPN = frameIndex;
      ASSERT(AllocIsValidMPN(*allocMPN, FALSE));
      return(VMK_OK);
   }

   if (Alloc_PFrameStateIsSwapped(frameState)) {
      Alloc_PageFaultToken *pfToken = NULL;

      if (fromCOS) {
         ASSERT(canBlock);
         pfToken = &Alloc_AllocInfo(world)->cosToken;
      } else if (!canBlock) {
         // ok we are not the console os and we dont want to block
         // so we do an async io request before we return back to the
         // caller
         ASSERT_NOT_IMPLEMENTED(!Alloc_AllocInfo(world)->duringCheckpoint);
         pfToken = (Alloc_PageFaultToken *)Mem_Alloc(sizeof(*pfToken));

         // initialize the token, this token is *not* used by the cos
         AllocPFTokenInit(world, pfToken, FALSE);

         if (ALLOC_PFRAME_DEBUG) {
            VMLOG(0, worldID,
                      "cannot block to read swapped ppn=0x%x, slot=0x%x, starting async io",
                      ppn, frameIndex);
         }
         *allocMPN = INVALID_MPN;
      }

      // special case: checkpointing
      if (Alloc_AllocInfo(world)->duringCheckpoint) {
         MPN bufMPN;
         Async_Token *token = NULL;

         if (fromCOS) {
            if (AllocPFTokenIsStateDone(pfToken)) {
               // during checkpointing we should not have
               // received any other page faults while
               // this one was being serviced
               ASSERT(pfToken->ppn == ppn &&
                      pfToken->slotNr == frameIndex);
               if (pfToken->ppn == ppn &&
                   pfToken->slotNr == frameIndex) {
                  *allocMPN = pfToken->mpn;
                  AllocPFTokenRelease(pfToken);
                  ASSERT(AllocIsValidMPN(*allocMPN, FALSE));
                  return(VMK_OK);
               } else {
                  return(VMK_FAILURE);
               }
            }
            if (!AllocPFTokenIsStateFree(pfToken)) {
               return(VMK_WOULD_BLOCK);
            }
         }

         // allocate checkpoint buffer page, fail if unable
         bufMPN = AllocCheckpointBufGetPage(world);
         if (bufMPN == INVALID_MPN) {
            VmWarn(worldID, "swap checkpoint failed: ppn=0x%x", ppn);
            return(VMK_FAILURE);
         }

         if (fromCOS) {
            ASSERT(pfToken);
            status = AllocPFTokenSetStateInUse(world, pfToken,
                                               ppn, bufMPN, frameIndex,
                                               AllocCheckpointCallback);
            if (status != VMK_OK) {
               World_Panic(world, "During checkpoint, could not allocate a Token to read "
                           "swapped out page PPN(0x%x)\n", ppn);
               return(status);
            }
            token = pfToken->token;
            ASSERT(token);
         }

         // read page from swap file into buffer (w/o explicit swapin)
         AllocUnlock(world);

         // XXX: temporarily remove this check.
         // Need to add it back or add a comparable check.
         // ASSERT_HAS_INTERRUPTS();

         status = Swap_GetSwappedPage(world, frameIndex, bufMPN, token, ppn);
         AllocLock(world);

         if (status != VMK_OK) {
            VmWarn(worldID, "swap checkpoint failed: status=%d", status);
            return(status);
         }

         if (fromCOS) {
            ASSERT(pfToken);
            // We did an async read from the swap, so wait till
            // the async read completes
            *allocMPN = INVALID_MPN;
            return(VMK_WOULD_BLOCK);
         }

         // debugging
         if (ALLOC_DEBUG_CHECKPOINT_VERBOSE) {
            VMLOG(0, worldID, "swap ckpt: ppn=0x%x, mpn=0x%x", ppn, bufMPN);
         }

         // set reply value, succeed
         *allocMPN = bufMPN;
         ASSERT(AllocIsValidMPN(*allocMPN, FALSE));
         return(VMK_OK);
      }

      if (pfToken) {
         if (!AllocPFTokenIsStateFree(pfToken)) {
            ASSERT(fromCOS);
            *allocMPN = INVALID_MPN;
            return(VMK_WOULD_BLOCK);
         }
      }

      // allocate new page (with appropriate color)
      *allocMPN = AllocVMPage(world, ppn);
      if (*allocMPN == INVALID_MPN) {
         VmWarn(worldID, "unable to alloc page: ppn=0x%x", ppn);
         // dump swap statistics
         if (vmx86_debug) {
            MemSched_LogSwapStats();
         }
         ASSERT_BUG_DEBUGONLY(21329, *allocMPN != INVALID_MPN);
         return(VMK_NO_MEMORY);
      }

      // read the page contents from disk   
      status = AllocGetSwappedPage(world, dirMPN, pageIndex, 
                                   frameIndex, *allocMPN, ppn,
                                   pfToken);  
      if (pfToken) {
         // We have just issued a async read
         ASSERT(status == VMK_WOULD_BLOCK);
         *allocMPN = INVALID_MPN;
         return status;
      }

      if (status != VMK_OK) {
         // free the newly acquired page
         AllocFreeVMPage(world, *allocMPN);
         *allocMPN = INVALID_MPN;
         return(status);
      }
      // update usage
      usage->locked++;

      // map page directory again for update
      dir = Kseg_MapMPN(dirMPN, &dirPair);

      /*
       * AllocPFrameSetRegular will reset the allocPFrame state to REGULAR and
       * set valid = 1, hence we do not need to
       * explicitly reset the ALLOC_PFRAME_SWAP_IN (set in
       * AllocGetSwappedPage) or ALLOC_PFRAME_SWAPPED flag.
       */
      AllocPFrameSetRegular(world, &dir[pageIndex], *allocMPN);

      Kseg_ReleasePtr(dirPair);

      ASSERT(AllocIsValidMPN(*allocMPN, FALSE));
      // succeed
      return(VMK_OK);
   }

   // done if valid page
   if (frameMPN != INVALID_MPN) {
      // sanity checks: ordinary page
      ASSERT(Alloc_PFrameStateIsRegular(frameState));

      *allocMPN = frameMPN;
      ASSERT(AllocIsValidMPN(*allocMPN, FALSE));
      return(VMK_OK);
   }

   // special case: checkpointing guest physical memory
   if (Alloc_AllocInfo(world)->duringCheckpoint) {
      ASSERT(Alloc_AllocInfo(world)->dummyMPN != INVALID_MPN);
      *allocMPN = Alloc_AllocInfo(world)->dummyMPN;
      ASSERT(AllocIsValidMPN(*allocMPN, FALSE));
      return(VMK_OK);
   }

   // invalid, must allocate new page
   
   // allocate new page (with appropriate color)
   *allocMPN = AllocVMPage(world, ppn);
   ASSERT(*allocMPN != INVALID_MPN);
   if (*allocMPN == INVALID_MPN) {
      VmWarn(worldID, "unable to alloc page: ppn=0x%x", ppn);
      return(VMK_NO_MEMORY);
   }

   // zero page contents (required for security)
   status = Util_ZeroMPN(*allocMPN);
   if (status != VMK_OK) {
      return status;
   }

   // map page directory again for update
   dir = Kseg_MapMPN(dirMPN, &dirPair);

   // update usage info for VM pages
   usage->locked++;
   AllocPFrameSetRegular(world, &dir[pageIndex], *allocMPN);
   Kseg_ReleasePtr(dirPair);

   ASSERT(AllocIsValidMPN(*allocMPN, FALSE));
   // succeed
   return(VMK_OK);
}


/*
 *----------------------------------------------------------------------
 *
 * AllocSwapReadComplete --
 *      Do some post-processing after a page has been read from the
 *      swap file.
 *      - Wakeup all the threads waiting for this page
 *      - Free the swap file slot
 *
 *      Callers must hold the alloc lock
 *
 * Results:
 *      VMK_OK on success.
 *
 * Side effects:
 *
 *----------------------------------------------------------------------
 */
static INLINE VMK_ReturnStatus
AllocSwapReadComplete(World_Handle *world,      // the world
                      Alloc_PFrame *pframe,     // the page frame
                      MPN newMPN,               // the MPN of the page with data
                      uint32 slotNr,            // the slot num in the swap file
                      PPN ppn)                  // the PPN of the page with data
{
   // Assert that Alloc is locked
   ASSERT(AllocIsLocked(world));

   // Wakeup the threads waiting for this swap in operation
   CpuSched_Wakeup((uint32) newMPN);
   LOG(2, "Waking up threads waiting on the mpn (0x%x)", newMPN);

   Swap_DoPageSanityChecks(world, slotNr, newMPN, ppn);

   // Free the swap slot associated with this page
   Swap_FreeFileSlot(world, slotNr);

   /* if the frame has been deallocated while we were reading from disk */
   if (Alloc_PFrameGetIndex(pframe) == INVALID_MPN) {
      ASSERT(Alloc_PFrameStateIsSwapIn(Alloc_PFrameGetState(pframe)));
      VmWarn(world->worldID, "MPN (0x%x) marked invalid while reading swapped page",
             newMPN);
      /* Release this newMPN */
      AllocFreeVMPage(world, newMPN);
      ASSERT_NOT_IMPLEMENTED(ppn != INVALID_PPN);
      AllocPFrameReset(world, pframe);
      return(VMK_FAILURE);
   }

   return(VMK_OK);
}


/*
 *----------------------------------------------------------------------
 *
 * AllocGetSwappedPage --
 *      Read the contents of the swapped page into the newMPN.
 *      If pfToken == NULL do a synchronous/blocking read of the swap file.
 *      Else do an async read of the swap file
 *
 * Results:
 *      VMK_OK : if page was successfully read from the swap file.
 *      VMK_WOULD_BLOCK : if an async read of the swap file is done.
 *
 * Side effects:
 *      Releases the "world" alloc lock for the duration of the read.
 *      Function will *block* to read the page from disk in case of a
 *      synchronous read of the swap file.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
AllocGetSwappedPage(World_Handle *world,        // the world
                    MPN dirMPN,                 // old MPN of the page
                    uint32 pageIndex,           // the page index in the slot
                    uint32 slotNr,              // the slot num in the swap file
                    MPN newMPN,                 // new MPN of the page
                    PPN ppn,                    // PPN of the page
                    Alloc_PageFaultToken *pfToken)      // the token
{
   Alloc_PFrame *dir;
   KSEG_Pair    *dirPair;
   Alloc_PFrame *pframe;
   VMK_ReturnStatus status;
   Async_Token *token = NULL;

   // Assert that Alloc is locked
   ASSERT(AllocIsLocked(world));

   if (pfToken) {
      status = AllocPFTokenSetStateInUse(world, pfToken,
                                         ppn, newMPN, slotNr,
                                         AllocAsyncReadCallback);
      if (status != VMK_OK) {
         World_Panic(world, "Could not allocate a Token to read swapped out page PPN(0x%x)\n",
                     ppn);
         return(status);
      }
      token = pfToken->token;
      ASSERT(token);
   }

   dir = Kseg_MapMPN(dirMPN, &dirPair);
   pframe = &dir[pageIndex];

   // Update the MPN to the newMPN
   AllocPFrameSetRegular(world, pframe, newMPN);

   // Update the flags
   Alloc_PFrameSetState(pframe, ALLOC_PFRAME_SWAP_IN);

   Kseg_ReleasePtr(dirPair);

   // relase the alloc lock
   AllocUnlock(world);
   
   // XXX: temporarily remove this check.
   // Need to add it back or add a comparable check.
   // ASSERT_HAS_INTERRUPTS();

   status = Swap_GetSwappedPage(world, slotNr, newMPN, token, ppn);
   if (status != VMK_OK) {
      VmWarn(world->worldID, "unable to read from slot(0x%x)", slotNr);

      // Wakeup the threads waiting for this swap in operation
      CpuSched_Wakeup((uint32) newMPN);
      World_Panic(world, "Unable to read swapped out page PPN(0x%x) from"
                 "swap slot(0x%x) for VM(%d)\n", ppn, slotNr, world->worldID);
      AllocLock(world);
      return(status);
   }

   // reacquire the alloc lock
   AllocLock(world);

   if (pfToken) {
      // we did an async read so wait till it finishes
      return(VMK_WOULD_BLOCK);
   }

   dir = Kseg_MapMPN(dirMPN, &dirPair);
   pframe = &dir[pageIndex];
   status = AllocSwapReadComplete(world, pframe, newMPN, slotNr, ppn);
   Kseg_ReleasePtr(dirPair);
   return(status);
}

/*
 *-----------------------------------------------------------------------------
 *
 * AllocFreeSwapMPN --
 *      Add the machine page to the list of free pages
 *
 *      Caller must hold alloc lock
 *
 * Results:
 *      none
 *
 * Side effects:
 *      updates the Alloc_PFrame for this page
 *
 *-----------------------------------------------------------------------------
 */
static void
AllocFreeSwapMPN(World_Handle *world,
                 Alloc_PFrame *allocPFrame,
                 uint32 index)
{
   MPN mpn;
   mpn = Alloc_PFrameGetIndex(allocPFrame);

   // Update the alloc frame mpn to contain the slot number 
   Alloc_PFrameSetIndex(allocPFrame, index);

   ASSERT(Alloc_PFrameStateIsSwapOut(Alloc_PFrameGetState(allocPFrame)));

   // Update the alloc frame flags to indicate a swapped page 
   Alloc_PFrameSetValid(allocPFrame);
   Alloc_PFrameSetState(allocPFrame, ALLOC_PFRAME_SWAPPED);

   ASSERT(Alloc_PFrameGetState(allocPFrame) != ALLOC_PFRAME_SWAP_OUT) ;

   // free the page 
   AllocFreeVMPage(world, mpn);
}


/*
 *-----------------------------------------------------------------------------
 *
 * Alloc_MarkSwapPage --
 *      Mark a page "ppn" as swapped in Alloc_PFrame and 
 *      free the associated "mpn".
 *
 * Results:
 *      TRUE if page swapping succeeded.
 *
 * Side effects:
 *      updates the Alloc_PFrame and MPN freed.
 *
 *-----------------------------------------------------------------------------
 */
Bool
Alloc_MarkSwapPage(World_Handle *world,         // the world
                   Bool writeFailed,            // swap file write failed
                   uint32 index,                // slot number in swap file
                   PPN ppn,                     // PPN of the page
                   MPN mpn)                     // MPN of the page
{
   Alloc_PFrame *allocPFrame;
   AllocPFramePair framePair;
   AllocPFrameState frameState;
   VMK_ReturnStatus status;
   Bool swapped;

   // Make sure that this is the only function
   // modifying the flags in the allocPFrame
   Alloc_Lock(world);

   if (Alloc_IsCheckpointing(Alloc_AllocInfo(world))) {
      // if we are checkpointing. Release all the used 
      // swap slots and exit this function after updating
      // the world swap state correctly. This really does not
      // matter if we are doing suspends but for checkpoints it 
      // does matter that we leave the world in a valid checkpoint
      // state.
      //   Setting writeFailed to true should do the right things
      writeFailed = TRUE;
   }

   status = AllocGetPFrameFromPPN(world, ppn, &framePair);
   if (UNLIKELY(status != VMK_OK)) {
      VmWarn(world->worldID,
             "Failed to get (Alloc_PFrame *) for PPN <%d>", ppn);

      Alloc_Unlock(world);
      ASSERT(FALSE);
      return FALSE;
   }
   allocPFrame = framePair.pframe;

   frameState = Alloc_PFrameGetState(allocPFrame);
   if (!writeFailed) {
      // Release the swapped MPN, only if we are in the SWAP_OUT state
      if (Alloc_PFrameStateIsSwapOut(frameState)) {
         MemSchedVmmUsage *usage = MemSched_ClientVmmUsage(world);
         // sanity check
         ASSERT(mpn == Alloc_PFrameGetIndex(allocPFrame));
         Swap_DoPageSanityChecks(world, index, mpn, ppn);

         // reclaim the page and mark the Alloc_PFrame flag as SWAPPED
         AllocFreeSwapMPN(world, allocPFrame, index);

         // update allocInfo statistics
         usage->swapped++;
         usage->locked--;

         swapped = TRUE;
      } else {
         // We had a page fault on this page while we were swapping out.
         // Free the file slot to which this page was written
         // sanity check
         ASSERT(!Alloc_PFrameStateIsSwapped(frameState));
         ASSERT(!Alloc_PFrameStateIsSwapIn(frameState));

         swapped = FALSE;
      }
   } else {
      // if write failed, we just give up and
      // mark this page as a regular page
      if (Alloc_PFrameStateIsSwapOut(frameState)) {
         Alloc_PFrameSetState(allocPFrame, ALLOC_PFRAME_REGULAR);
      }
      swapped = FALSE;
   }

   AllocPFrameReleasePair(&framePair);
   Alloc_Unlock(world);

   return swapped;
}


/*
 *----------------------------------------------------------------------
 *
 * Alloc_PageIsRemote --
 *
 *      Mark this page is to be remotely paged in from a migration
 *	source machine.
 *
 * Results:
 *      VMK_OK upon success, VMK_BAD_PARAM upon failure.
 *
 * Side effects:
 *      The page at ppn gets freed and set to be paged in from a
 *	remote source.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Alloc_PageIsRemote(World_Handle *world, PPN ppn)
{
   VMK_ReturnStatus status;
   uint32 dirIndex, pageIndex;
   Alloc_PageInfo *pageInfo;
   MPN mpn, dirMPN;
   Alloc_PFrame *dir;
   KSEG_Pair *dirPair;

   // lookup ppn, fail if unable
   status = Alloc_LookupPPN(world, ppn, &dirIndex, &pageIndex);
   ASSERT(status == VMK_OK);
   if (status != VMK_OK) {
      return(status);
   }

   AllocLock(world);

   pageInfo = &Alloc_AllocInfo(world)->vmPages;
   dirMPN = pageInfo->pages[dirIndex];
   if (dirMPN == INVALID_MPN) {
      dirMPN = AllocMapPageDir(world, &pageInfo->pages[dirIndex]);
      ASSERT(dirMPN == pageInfo->pages[dirIndex]);
   }

   dir = Kseg_MapMPN(dirMPN, &dirPair);
   if (!Alloc_PFrameIsValid(&dir[pageIndex])) {
      LOG(1, "ppn %d is invalid", ppn);
   } else {
      mpn = Alloc_PFrameGetIndex(&dir[pageIndex]);
      if (mpn == INVALID_MPN) {
	 LOG(1, "MPN for PPN %d is already invalid", ppn);
      } else {
         MemSchedVmmUsage *usage = MemSched_ClientVmmUsage(world);
	 usage->locked--;
	 AllocFreeVMPage(world, mpn);
      }
   }
   Alloc_PFrameSetInvalid(&dir[pageIndex]);
   Swap_SetMigPFrame(&dir[pageIndex], ppn);
   Kseg_ReleasePtr(dirPair);

   AllocUnlock(world);

   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * AllocLockPageInt --
 *
 *      Lock the "ppn" in "world".
 *	If "writeable" is set, ensures that the returned page
 *	is writeable.  Sets "writeable" to FALSE if the returned page
 *	is read-only.
 *
 * Results:
 *      Returns the locked MPN if successful, or INVALID_MPN if error.
 *	Sets "writeable" to TRUE if MPN writeable, FALSE if read-only.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static MPN
AllocLockPageInt(World_Handle *world, PPN ppn, Bool *writeable)
{
   MPN mpn;

   // sanity check
   ASSERT(world == MY_VMM_GROUP_LEADER);

   // invoke primitive
   AllocPageFault(world, ppn, writeable, &mpn, ALLOC_FROM_MONITOR, FALSE);

   // debugging
   if (mpn == INVALID_MPN) {
      VmWarn(world->worldID, "ppn=0x%x failed", ppn);
   }

   return(mpn);
}

/*
 *----------------------------------------------------------------------
 *
 * Alloc_ModeLockPage --
 *
 *      Lock the "ppn".
 *	If "writeable" is set, ensures that the returned page
 *	is writeable.  Sets "writeable" to FALSE if the returned page
 *	is read-only.
 *
 * Results:
 *      Sets "mpn" to the locked MPN if successful, or INVALID_MPN
 *	if error.  Sets "writeable" to TRUE if MPN writeable,
 *	FALSE if read-only.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMKERNEL_ENTRY
Alloc_ModeLockPage(DECLARE_3_ARGS(VMK_MODE_LOCK_PAGE,
                                 PPN, ppn, 
                                 Bool *, writeable,
                                 MPN *, mpn))
{
   PROCESS_3_ARGS(VMK_MODE_LOCK_PAGE,
                  PPN, ppn, 
                  Bool *, writeable,
                  MPN *, mpn);

   // invoke primitive with specified access mode
   *mpn = AllocLockPageInt(MY_VMM_GROUP_LEADER, ppn, writeable);
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * AllocCanBalloonPage --
 *
 *      Check if PPN "ppn" associated with "world" can be ballooned.
 *
 * Results:
 *      Returns TRUE if the page can be ballooned, FALSE otherwise.
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
static Bool
AllocCanBalloonPage(World_Handle *world, PPN ppn)
{
   uint32 dirIndex, pageIndex;
   AllocPFrameState frameState;
   uint16 framePinCount;
   Alloc_PageInfo *pageInfo;
   VMK_ReturnStatus status;
   KSEG_Pair *dirPair;
   World_ID worldID;
   Alloc_PFrame *dir;
   Bool frameValid;
   MPN dirMPN;

   // convenient abbrevs
   worldID = world->worldID;
   pageInfo = &Alloc_AllocInfo(world)->vmPages;

   status = Alloc_LookupPPN(world, ppn, &dirIndex, &pageIndex);
   if (status != VMK_OK) {
      return(FALSE);
   }

   // lookup page frame directory
   dirMPN = pageInfo->pages[dirIndex];
   if (dirMPN == INVALID_MPN) {
      // OK to balloon unmapped pages
      return(TRUE);
   }

   // lookup page frame, extract flags
   dir = Kseg_MapMPN(dirMPN, &dirPair);
   frameValid = Alloc_PFrameIsValid(&dir[pageIndex]);
   framePinCount = Alloc_PFrameGetPinCount(&dir[pageIndex]);
   frameState = Alloc_PFrameGetState(&dir[pageIndex]);
   Kseg_ReleasePtr(dirPair);

   // OK to balloon unmapped pages
   if (!frameValid) {
      return(TRUE);
   }

   // fail if page is pinned
   if (framePinCount > 0) {
      return(FALSE);
   }

   // Fail in the rare case that a page is ballooned while it
   // is in the process of being swapped out (I/O not yet complete).
   // We want to prevent this from happening to eliminate the
   // possibility that the MPN is reclaimed from the first VM,
   // allocated to and modified by a second VM, and then written out
   // (with contents belonging to the second VM) to a swap file
   // associated with the first VM, violating isolation between VMs.
   if (Alloc_PFrameStateIsSwapOut(frameState)) {
      return(FALSE);
   }

   // No need to check if the page is in any other swap or page share
   // states as in these states we can still balloon/unlock it.

   // succeed
   return(TRUE);
}


/*
 *----------------------------------------------------------------------
 *
 * AllocBalloonReleasePage --
 *
 *      Free the mpn assocaited with this "ppn" in "world".
 *
 * Results:
 *      Retruns TRUE if successful, FALSE otherwise.
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
static Bool
AllocBalloonReleasePage(World_Handle *world, PPN ppn)
{
   uint32 dirIndex, pageIndex, frameIndex, retryCount, count;
   uint16 framePinCount;
   Bool reclaimPage;
   Alloc_PageInfo *pageInfo;
   MemSchedVmmUsage *usage;
   VMK_ReturnStatus status;
   MPN unlockMPN, dirMPN;
   PPN hintPPN;
   KSEG_Pair *dirPair;
   World_ID hintWorld, worldID;
   Alloc_PFrame *dir;
   AllocPFrameState frameState;
   Bool frameValid;
   uint64 key;

   ASSERT(ppn != INVALID_PPN);

   // convenient abbrevs
   worldID = world->worldID;
   pageInfo = &Alloc_AllocInfo(world)->vmPages;
   usage = MemSched_ClientVmmUsage(world);

   // sanity check
   ASSERT(world == MY_VMM_GROUP_LEADER);

   // lookup user virtual address, fail if unable
   status = Alloc_LookupPPN(world, ppn, &dirIndex, &pageIndex);
   if (status != VMK_OK) {
      return(FALSE);
   }

   // initialize
   retryCount = 0;

 retry:
   // acquire alloc lock for world
   AllocLock(world);

   if (!AllocCanBalloonPage(world, ppn)) {
      AllocUnlock(world);
      return(FALSE);
   }

   // lookup page frame directory
   dirMPN = pageInfo->pages[dirIndex];
   if (dirMPN == INVALID_MPN) {
      // no work to do if ballooning unmapped page
      AllocUnlock(world);
      return(TRUE);
   }

   // lookup page frame and extract flags, index, mpn
   dir = Kseg_MapMPN(dirMPN, &dirPair);
   frameState = Alloc_PFrameGetState(&dir[pageIndex]);
   frameValid = Alloc_PFrameIsValid(&dir[pageIndex]);
   frameIndex = Alloc_PFrameGetIndex(&dir[pageIndex]);
   unlockMPN = Alloc_PFrameGetMPN(&dir[pageIndex]);
   framePinCount = Alloc_PFrameGetPinCount(&dir[pageIndex]);
   Kseg_ReleasePtr(dirPair);

   // no work to do if ballooning unmapped page
   if (!frameValid) {
      AllocUnlock(world);
      return(TRUE);
   }

   // sanity checks: not possible for page to be pinned or in
   // the process of being swapped out, otherwise couldn't
   // have passed the AllocCanBalloonPage() check above.
   ASSERT(framePinCount == 0);
   ASSERT(!Alloc_PFrameStateIsSwapOut(frameState));

   // find MPN if copy-on-write shared or hint page
   if (Alloc_PFrameStateIsCOW(frameState)) {
      // lookup pshare entry
      status = PShare_LookupByMPN(unlockMPN, &key, &count);
      if (status != VMK_OK) {
         VmWarn(worldID, "pshare lookup failed: mpn 0x%x", unlockMPN);
         AllocUnlock(world);
         return(FALSE);
      }
   } else if (Alloc_PFrameStateIsCOWHint(frameState)) {
      // lookup pshare entry
      status = PShare_LookupHint(unlockMPN,
                                 &key,
                                 &hintWorld,
                                 &hintPPN);
      if (status != VMK_OK) {
         VmWarn(worldID, "hint lookup failed: mpn 0x%x", unlockMPN);
         AllocUnlock(world);
         return(FALSE);         
      }
   } else if (Alloc_PFrameStateIsSwapIn(frameState)) {
      // dont want to complicate the page fault path by
      // invalidating the page here, so we just wait for
      // the page fault to complete and retry again.
      if (ALLOC_PFRAME_DEBUG) {
         VMLOG(1, worldID, "Trying to unlock a page that is being swapped in,"
               "sleeping on mpn (0x%x)", frameIndex);
      }
      // Release the allocInfo lock and
      // go to sleep...waiting on the page to be read from
      // the swap file by another kernel thread
      CpuSched_Wait((uint32)frameIndex,
                    CPUSCHED_WAIT_SWAPIN,
                    &Alloc_AllocInfo(world)->lock);
      if (ALLOC_PFRAME_DEBUG) {
         VMLOG(1, worldID, "Trying to unlock a page that was being swapped in,"
               "woken up on mpn (0x%x)", frameIndex);
      }
      goto retry;
   } else if (Alloc_PFrameStateIsSwapped(frameState)) {
      if (ALLOC_PFRAME_DEBUG_VERBOSE) {
         VMLOG(1, worldID, "swapped out page PPN(0x%x) was unlocked", ppn);
      }
      // a swapped page is already unlocked
      // so free the file slot
      Swap_FreeFileSlot(world, frameIndex);

      dir = Kseg_MapMPN(dirMPN, &dirPair);
      AllocPFrameReset(world, &dir[pageIndex]);
      Kseg_ReleasePtr(dirPair);

      AllocUnlock(world);

      return (TRUE);
   }

   // invalidate PPN to MPN mapping from all caches
   Alloc_InvalidateCache(world, ppn);
   Kseg_InvalidatePtr(world, ppn);

   // reclaim if unshared, drop reference if shared
   reclaimPage = FALSE;
   if (Alloc_PFrameStateIsCOW(frameState)) {
      // reclaim shared page only if last reference
      status = AllocPShareRemove(world, key, unlockMPN, &count);
      ASSERT(status == VMK_OK);
      reclaimPage = (count == 0);
   } else if (Alloc_PFrameStateIsCOWHint(frameState)) {
      // always reclaim if unshared hint page
      status = PShare_RemoveHint(unlockMPN, hintWorld, hintPPN);
      ASSERT(status == VMK_OK);
      reclaimPage = TRUE;
   } else {
      // always reclaim unshared pages
      reclaimPage = TRUE;
   }

   // invalidate page frame
   dir = Kseg_MapMPN(dirMPN, &dirPair);
   ASSERT(Alloc_PFrameGetIndex(&dir[pageIndex]) == frameIndex);
   AllocPFrameReset(world, &dir[pageIndex]);
   Kseg_ReleasePtr(dirPair);

   // update PPN usage
   usage->locked--;
   if (Alloc_PFrameStateIsCOW(frameState)) {
      usage->cow--;
      if (PShare_IsZeroKey(key)) {
         usage->zero--;
      }
   } else if (Alloc_PFrameStateIsCOWHint(frameState)) {
      usage->cowHint--;
   }

   // release alloc lock for world
   AllocUnlock(world);       

   // flush PPN to MPN mapping from the ksegs on all remote cpus
   if (numPCPUs > 1) {
      Kseg_FlushRemote(worldID, ppn);
   }

   // reclaim page, if appropriate
   if (reclaimPage) {
      // free unlocked page
      AllocFreeVMPage(world, unlockMPN);
   }

   if (ALLOC_DEBUG_BALLOON) {
      static uint32 throttle = 0;
      if (throttle++ % 5000 == 0) {
         LOG(0, "successfully unlocked a page, count<%u>",  throttle);
      }
   }
   // succeed
   return(TRUE);
}


/*
 *----------------------------------------------------------------------
 *
 * Alloc_BalloonReleasePages --
 *
 *      Release the MPNs associated with the given list of PageNums.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
VMKERNEL_ENTRY
Alloc_BalloonReleasePages(DECLARE_2_ARGS(VMK_BALLOON_REL_PAGES,
                                         BPN *, bpnList,
                                         uint32, numPages))
{
   World_Handle *world = MY_VMM_GROUP_LEADER;
   uint32 i;
   PROCESS_2_ARGS(VMK_BALLOON_REL_PAGES,
                  BPN *, bpnList,
                  uint32, numPages);

   // release the given pages
   for (i = 0; i < numPages; i++) {
      if (bpnList[i] != INVALID_BPN) {
         if (Alloc_IsMainMemBPN(world, bpnList[i])) {
            AllocBalloonReleasePage(world, 
                                    Alloc_BPNToMainMemPPN(world, bpnList[i]));
         } else {
            return VMK_BAD_PARAM;
         }
      }
   }
   return VMK_OK;
}
/*
 *----------------------------------------------------------------------
 *
 * Alloc_CanBalloonPage --
 *
 *      Determine if this PPN can be ballooned.
 *
 * Results:
 *      returns TRUE if PPN can be ballooned. FALSE otherwise.
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
VMKERNEL_ENTRY
Alloc_CanBalloonPage(DECLARE_1_ARG(VMK_CAN_BALLOON_PAGE,
                                   BPN, bpn))
{
   Bool canBalloon;
   World_Handle *world = MY_VMM_GROUP_LEADER;
   PROCESS_1_ARG(VMK_CAN_BALLOON_PAGE, BPN, bpn);
   // acquire alloc lock for world
   if (!Alloc_IsMainMemBPN(world, bpn)) {
      return VMK_FAILURE;
   }
   AllocLock(world);
   canBalloon = AllocCanBalloonPage(world, Alloc_BPNToMainMemPPN(world, bpn));
   AllocUnlock(world);
   return canBalloon ? VMK_OK : VMK_FAILURE;
}

/*
 *-------------------------------------------------------------------------
 *
 * AllocReleaseAnonPage --
 *
 *    Remove this mpn from the list of anon MPNs and free the mpn.
 *
 * Results:
 *    returns VMK_OK on success,
 *            error code on failure
 *
 * Side effects:
 *      none.
 *
 *-------------------------------------------------------------------------
 */
static VMK_ReturnStatus 
AllocReleaseAnonPage(World_Handle *world, MPN anonMPN)
{
   VMK_ReturnStatus status;

   ASSERT(AllocIsLocked(world));
   status = AllocRemoveFromAnonMPNList(world, anonMPN);
   if (status != VMK_OK) {
      ASSERT(FALSE);
      World_Panic(world, "Anon mpn list is inconsistent\n");
      return status;
   }
   // free the anon mpn
   AllocFreeVMPage(world, anonMPN);
   return status;
}


/*
 *-------------------------------------------------------------------------
 *
 * Alloc_ReleaseAnonPage --
 *
 *      Free the given anonymous MPN. Also takes care of adding the 
 *      userVA corresponding to this MPN back to the list of free
 *      anonymous VAs.
 *
 * Results:
 *    returns VMK_OK on success,
 *            error code on failure
 *
 * Side effects:
 *      none.
 *
 *-------------------------------------------------------------------------
 */
VMKERNEL_ENTRY
Alloc_ReleaseAnonPage(DECLARE_1_ARG(VMK_ALLOC_RELEASE_ANON_PAGE, 
                                    MPN, anonMPN))
{
   VMK_ReturnStatus status;
   World_Handle *world = MY_VMM_GROUP_LEADER;

   PROCESS_1_ARG(VMK_ALLOC_RELEASE_ANON_PAGE,
                 MPN, anonMPN);

   AllocLock(world);
   status = AllocReleaseAnonPage(world, anonMPN);
   if (status == VMK_OK) {
      MemSched_ClientVmmUsage(world)->anon--;
   }
   AllocUnlock(world);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Alloc_ReleaseKernelAnonPage --
 *
 *    Release the anonymous page "mpn" used by the vmkernel.
 *
 * Results:
 *      On success, VMK_OK
 *      On failure, error code
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Alloc_ReleaseKernelAnonPage(World_Handle *world, MPN mpn)
{
   VMK_ReturnStatus status;

   // either a single or a large page
   AllocLock(world);
   status = AllocReleaseAnonPage(world, mpn);
   ASSERT(status == VMK_OK);
   if (status == VMK_OK) {
      MemSched_ClientVmmUsage(world)->anonKern -= 1;
      MemSched_UnreserveMem(world, 1);
   }
   AllocUnlock(world);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * AllocAnonPage --
 *
 *    Allocates 1 anonymous page. "lowMem" requests anonymous pages 
 *    within the first 4GB and it would wait up to 
 *    ALLOC_REMAP_LOW_TIMEOUT ms for getting the low page. 
 *    If "lowMem" is not specified the anon MPN could be any free mpn
 *    and the funciton is non-blocking.
 *
 *    If "lowMem" is specified, currently we always allocate page from
 *    the vmkernel reserved low memory pool. This shouldn't be a problem
 *    because low mem anon pages are only allocated at monitor init time.
 *
 * Results:
 *      On success, VMK_OK 
 *      On failure, returns error code
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
AllocAnonPage(World_Handle *world,
              Bool lowMem,
              MPN *mpnOut)
{
   VMK_ReturnStatus status;
   uint32 msTimeout = (lowMem ? ALLOC_REMAP_LOW_TIMEOUT : 0);

   AllocIsLocked(world);
   if (UNLIKELY(lowMem)) {
      *mpnOut = AllocVMLowReservedPage(world, INVALID_PPN, msTimeout);
   } else {
      *mpnOut = AllocVMPage(world, INVALID_PPN);
   }
   if (*mpnOut == INVALID_MPN) {
      ASSERT(FALSE);
      return(VMK_NO_MEMORY);
   }

   // set IO protection for anon pages
   MemMap_SetIOProtection(*mpnOut, MMIOPROT_IO_DISABLE);

   // Add this mpn to the list of anon pages used by this VM
   status = AllocAddToAnonMPNList(world, *mpnOut);
   ASSERT(status == VMK_OK);
   if (status != VMK_OK) {
      World_Panic(world, "Anon mpn list is inconsistent\n");
   }
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Alloc_AnonPage --
 *
 *       Allocates an anonymous page to be used by the monitor. This is
 *       the external interface used via VMK_Call.
 *
 * Results:
 *      On success, the locked MPN 
 *      On failure, returns INVALID_MPN (insufficient memory to handle fault).
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMKERNEL_ENTRY
Alloc_AnonPage(DECLARE_2_ARGS(VMK_ALLOC_ANON_PAGE, int, lowMem, MPN *, mpn))
{
   PROCESS_2_ARGS(VMK_ALLOC_ANON_PAGE, int, lowMem, MPN *, mpn);
   World_Handle *world = MY_VMM_GROUP_LEADER;
   MemSchedVmmUsage *usage = MemSched_ClientVmmUsage(world);
   Alloc_PageInfo *pageInfo = &Alloc_AllocInfo(world)->vmPages;
   VMK_ReturnStatus status;

   *mpn = INVALID_MPN;

   AllocLock(world);

   // make sure number of anon pages is within reserved limit 
   if (usage->anon + 1 > pageInfo->numAnonPages) {
      AllocUnlock(world);
      return VMK_NO_MEMORY;
   }

   status = AllocAnonPage(world, lowMem, mpn);
   if (status == VMK_OK) {
      usage->anon++;
   }

   AllocUnlock(world);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Alloc_KernelAnonPage --
 *
 *       Allocate an anon VM page for "world" used by vmkernel.
 *
 *       *mpnOut is set to the allocated page.
 *
 * Results:
 *       VMK_OK, on success
 *       error code otherwise, returns VMK_NO_MEMORY if 
 *       pages could not be allocated due to memory pressure
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Alloc_KernelAnonPage(World_Handle *world, Bool lowMem, MPN *mpnOut)
{
   VMK_ReturnStatus status;

   status = MemSched_ReserveMem(world, 1);
   if (status != VMK_OK) {
      return status;
   }

   AllocLock(world);
   status = AllocAnonPage(world, lowMem, mpnOut);
   ASSERT(status == VMK_OK);
   if (status == VMK_OK) {
      MemSched_ClientVmmUsage(world)->anonKern += 1;
   }
   AllocUnlock(world);
   return status; 
}


/*
 *--------------------------------------------------------------------------
 *
 * Alloc_GetNextAnonPage --
 *
 *    Get the next anon page. If "inMPN" is "INVALID_MPN" then get the
 *    first anon page from the anon pages list. If "inMPN" is not "INVALID_MPN"
 *    gets the mpn that is after "inMPN" in the list of anon pages.
 *
 *    NOTE: The anon list can change between calls to this function. It 
 *          is beyond the scope of this function to ensure that this
 *          list does not change.
 *
 * Results:
 *      TRUE on success, FALSE otherwise
 *
 * Side effects:
 *      none.
 *
 *--------------------------------------------------------------------------
 */
VMK_ReturnStatus
Alloc_GetNextAnonPage(World_ID worldID,
                      MPN inMPN,
                      MPN *outMPN)
{
   World_Handle *inWorld;
   World_Handle *world;

   // initialize return values
   *outMPN = INVALID_MPN;

   inWorld = World_Find(worldID);
   if (!inWorld) {
      return VMK_INVALID_HANDLE;
   }
   world = World_GetVmmLeader(inWorld);
   ASSERT(world != NULL);

   AllocLock(world);
   *outMPN = AllocGetNextMPNFromAnonMPNList(world, inMPN);
   AllocUnlock(world);
   World_Release(inWorld);
   return VMK_OK;
}


/*
 *--------------------------------------------------------------------------
 *
 * AllocPhysToMachineInt --
 *
 *      Translate physical address "paddr" in virtual machine "world" to
 *	a corresponding machine	address.
 *      result->length is the number of bytes mapped from paddr to the end
 *      of the last mpn that is mapped, so it can be greater than length.
 *      result->length will be less than length if all of the
 *	physical addresses are not backed by contiguous machine addresses.
 *	Function may block if canBlock is set (e.g. ppn is swapped out).
 *
 * Results:
 *      Returns VMK_OK on success, error status otherwise.
 *      *result contains the machine and possibly virtual addresses.
 *
 * Side effects:
 *      May block if the "canBlock" parameter is set.
 *
 *--------------------------------------------------------------------------
 */
VMK_ReturnStatus
AllocPhysToMachineInt(World_Handle *world,
                      PA      paddr,
                      uint32  length,
                      uint32  flags,
                      Bool    canBlock,
                      Alloc_Result *result)
{
   PPN firstPPN = PA_2_PPN(paddr);
   PPN lastPPN = PA_2_PPN(paddr + length - 1);
   Bool writeable = ((flags & ALLOC_READ_ONLY) == 0);
   Bool contig = FALSE;
   MPN allocMPN, allocMPN1;
   VMK_ReturnStatus status;

 retry:
   // sanity check
   ASSERT(AllocIsLocked(world));

   // map first PPN to MPN, fail if unable
   status = AllocPPNToMPN(world, firstPPN, writeable, canBlock, &allocMPN);
   if (status != VMK_OK) {
      if (ALLOC_PFRAME_DEBUG) {
         VMLOG(0, world->worldID, "failed: ppn=0x%x", firstPPN);
      }
      return(status);
   }

   ASSERT(allocMPN != INVALID_MPN);

   LOG(1, "AllocPhysToMachineInt: AllocPPNToMPN(%d)", firstPPN);
   result->maddr = MPN_2_MA(allocMPN) + (paddr & PAGE_MASK);

   // attempt to map second PPN, if any
   if (firstPPN == lastPPN) {
      result->length = PAGE_SIZE - (paddr & PAGE_MASK);
   } else {
      /*
       * If length is > PAGE_SIZE, determine if the PPNs map to physically
       * contiguous machine pages and set length.
       */
      status = AllocPPNToMPN(world, lastPPN, writeable, canBlock, &allocMPN1);

      // successive PPNToMPN calls not atomic when blocking enabled
      if (canBlock) {
         VMK_ReturnStatus checkStatus;
         MPN checkMPN;

         // check if firstPPN->allocMPN mapping changed
         checkStatus = AllocPPNToMPN(world, firstPPN, writeable,
                                     FALSE, &checkMPN);
         if ((checkStatus != VMK_OK) || (checkMPN != allocMPN)) {
            VMLOG(0, world->worldID,
                     "check failed: status=%d, mpn=0x%x: orig=0x%x: retrying",
                     checkStatus, checkMPN, allocMPN);
            goto retry;
         }
      }

      if ((status == VMK_OK) && ((allocMPN + 1) == allocMPN1)) {
         ASSERT(allocMPN1 != INVALID_MPN);
         contig = TRUE;
         result->length =
            (allocMPN1  - allocMPN) * PAGE_SIZE + (PAGE_SIZE - (paddr & PAGE_MASK));
      } else {
         result->length = PAGE_SIZE - (paddr & PAGE_MASK);
      }
   }

   // update cache
   if (flags & ALLOC_FAST_LOOKUP) {
      Alloc_P2M *ce = Alloc_CacheEntry(world, firstPPN);
      ce->firstPPN = firstPPN;
      ce->lastPPN = (contig) ? lastPPN : firstPPN;
      ce->maddr = result->maddr & ~PAGE_MASK;
      ce->readOnly = (writeable) ? FALSE : TRUE;
      ce->copyHints = 0;
   }

   return(VMK_OK);
}

/*
 *------------------------------------------------------------------------
 *
 * AllocDoCptIO --
 *
 *      Read/Write data synchronously from the checkpoint file. Release
 *      the checkpoint buffers once the IO is completed.
 *
 *      NOTE: Caller should hold  Alloc lock.
 *
 * Results:
 *      VMK_OK if IO is successful, error status otherwise.
 *
 * Side effects:
 *      Alloc lock will be released while IO is in progress
 *
 *------------------------------------------------------------------------
 */
VMK_ReturnStatus
AllocDoCptIO(World_Handle *world,
             FS_FileHandleID fileHandle,
             SG_Array *sgArr,
             Bool isRead,
             uint32 sgLen)
{
   VMK_ReturnStatus status;
   uint32 bytesTransferred = 0;

   ASSERT(AllocIsLocked(world));
   // release lock during potentially long file I/O
   // n.b. OK since monitor/vmx inactive, PPN->MPN mappings static
   AllocUnlock(world);

   ASSERT_HAS_INTERRUPTS();

   // we have now collected together PHYS_SG_SIZE pages,
   // so issue the scatter/gather file IO
   sgArr->length = sgLen;
   sgArr->addrType = SG_MACH_ADDR;
   status = FSS_SGFileIO(fileHandle, sgArr,
                         (isRead) ? FS_READ_OP : FS_WRITE_OP,
                         &bytesTransferred);

   // abort if I/O error or bad transfer size
   if (status != VMK_OK) {
      VmWarn(world->worldID, "error %d: checkpoint I/O failed", status);
   } else if (bytesTransferred != (sgArr->length << PAGE_SHIFT)) {
      status = VMK_IO_ERROR;
      VmWarn(world->worldID,
             "checkpoint I/O xfer size mismatch: expect=%u, actual=%u",
             (sgArr->length << PAGE_SHIFT),
             bytesTransferred);
   }

   // reacquire lock after file I/O complete
   AllocLock(world);

   // recycle checkpoint buffers
   if (Alloc_AllocInfo(world)->duringCheckpoint) {
      AllocCheckpointBufRelease(world);
   }
   return(status);
}

/*
 *-------------------------------------------------------------------
 *
 * Alloc_PhysMemIO --
 *
 * 	Read/write the specified percents of physical memory of the
 * 	specified world to a VMFS file at the specified offset.
 *
 * Results:
 *      Returns VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 *      May allocate machine pages.
 *
 *-------------------------------------------------------------------
 */
VMK_ReturnStatus
Alloc_PhysMemIO(VMnix_FilePhysMemIOArgs *args)
{
   PPN firstPage, lastPage;
   VMK_ReturnStatus status;
   Alloc_PageInfo *pageInfo;
   int i, len, np, worldID;
   World_Handle *world;
   uint64 offset;
   SG_Array *sgArr;
   PPN resumePPN;
   int sgLen;
   Bool sameCptFile;

   ASSERT(!CpuSched_IsHostWorld());
   // Locking notes: In general, a world's alloc lock must be held while
   // manipulating its page mapping data structures.
   // The alloc lock may be temporarily dropped for some operations
   // (such as potentially high-latency file I/O) by the AllocPageFaultInt
   // routine which this function calls.  Dan says this is OK
   // since this function is used only for suspend/resume, during which
   // the monitor and vmx apps are inactive, so all PPN->MPN mappings
   // should be static.  Although we should be able to completely skip
   // locking, there is little cost to holding the lock while possible
   // since there should be no contention for it.

   // convenient abbrev
   worldID = args->worldID;

   // initialize
   status = VMK_OK;

   // acquire world handle, fail if unable
   if ((world = World_Find(worldID)) == NULL) {
      WarnVmNotFound(worldID);
      return(VMK_BAD_PARAM);
   }

   pageInfo = &Alloc_AllocInfo(world)->vmPages;
   ASSERT(pageInfo->pages != NULL);

   firstPage = 0;
   np = (pageInfo->numPhysPages * args->startPercent) / 100;
   resumePPN = np;
   lastPage = firstPage + (pageInfo->numPhysPages * args->endPercent) / 100;
   firstPage += np;
   offset = args->offset + (uint64)np * (uint64)PAGE_SIZE;

   // allocate scatter-gather list, fail if unable
   sgArr = (SG_Array *)World_Alloc(world, SG_ARRAY_SIZE(PHYS_SG_SIZE));
   if (sgArr == NULL) {
      // issue warning, cleanup, fail
      VmWarn(worldID, "SG alloc failed");
      World_Release(world);
      return(VMK_NO_MEMORY);
   }
   sgLen = 0;

   if (args->read && (args->startPercent == 0)) {
      // Collect information about the checkpoint file,
      // to be used later by the swapper
      // Make this blocking call before we acquire the alloc lock
      status = Swap_SetCptFileInfo(world, pageInfo->numPhysPages, args);
      if (status != VMK_OK) {
         Warning("Failed to set checkpoint swap file");
         World_Release(world);
         return status;
      }
   }

   // check if VMX is trying to write to the same checkpoint file
   sameCptFile = Swap_AreCptFilesSame(world, args);

   // acquire world alloc lock
   AllocLock(world);

   if (args->read) {
      if (args->startPercent == 0) {
         // initialize the number of pages read from the checkpoint file
         Alloc_AllocInfo(world)->cptPagesRead = 0;
      }
   } else {
      ASSERT(Alloc_AllocInfo(world)->duringCheckpoint);
   }

   for (i = firstPage; i < lastPage; i += len, sgLen = 0) {
      uint32 dirIndex;
      MPN dirMPN;
      int j;

      len = (1 << ALLOC_PDIR_SHIFT) - (i & ALLOC_PDIR_OFFSET_MASK);
      if (i + len > lastPage) {
	 len = lastPage - i;
      }
      dirIndex = PAGE_2_DIRINDEX(i);

      // lookup page frame directory
      dirMPN = pageInfo->pages[dirIndex];
      if (dirMPN == INVALID_MPN) {
         dirMPN = AllocMapPageDir(world, &pageInfo->pages[dirIndex]);
         ASSERT(dirMPN == pageInfo->pages[dirIndex]);
      }
      ASSERT(dirMPN != INVALID_MPN);

      for (j = 0; j < len; j++, offset += PAGE_SIZE) {
         uint32 pageIndex;
         PPN ppn;
         MPN mpn;
         KSEG_Pair *dirPair;
         Alloc_PFrame *dir, *frame;
         Bool sharedCOW;

         // construct ppn into the memory file
         ppn = i + j;
         if (sgLen == 0) {
            AllocCheckpointBufSetStartPPN(world, ppn);
         }

         if (args->read) {
            Alloc_AllocInfo(world)->cptPagesRead++;
         }

         // get the pframe for the page
         pageIndex = PAGE_2_PAGEINDEX(i + j);
         dir = Kseg_MapMPN(dirMPN, &dirPair);
         frame  = &dir[pageIndex];
         mpn = Alloc_PFrameGetMPN(frame);

         if (!args->read) {
            // handle write case
            if (Alloc_PFrameIsRegular(frame) && mpn == INVALID_MPN) {
               // write out a page of all zeroes for an unallocated page
               VMLOG(1, worldID,  "writing dummy page");
               mpn = Alloc_AllocInfo(world)->dummyMPN;
            } else if (sameCptFile && Swap_IsCptPFrame(frame)) {
               // page is already in the checkpoint file
               continue;
            }
         } else {
            // handle read case

            // frame must be regular and not mapped
            ASSERT(Alloc_PFrameIsRegular(frame) && mpn == INVALID_MPN);

            if (!ALLOC_CPT_SWAP_DEBUG) {
               Alloc_Info *info = Alloc_AllocInfo(world);
               /*
                * If the read count exceeds "maxCptPagesToRead" pages,
                * or the current total free memory is not high, we 
                * leave those pages in the checkpoint file as swapped.
                */
               if (info->cptPagesRead > info->maxCptPagesToRead ||
                   !MemSched_MemoryIsHigh()) {

                  /*
                   * XXX esx20 beta1 hack only.
                   * Force read every 512M page, this is done to warm up
                   * the storage so that the swapper need not block in an 
                   * AsyncIO later on. This would allocate at most 8 extra 
                   * pages per VM resumed,  which should be fine even under
                   * severe memory pressure.
                   */
                  if ((ppn & 0x1ffff) != 0) {
                     // mark this page as being swapped to checkpoint file
                     Swap_SetCptPFrame(world, frame, offset);
                     continue;
                  }
               }
            }
         }
         Kseg_ReleasePtr(dirPair);

         // If MPN does not exist, fault in a new page.
         if (mpn == INVALID_MPN) {
            status = AllocPageFaultInt(world, ppn, 
                                       TRUE, &mpn, &sharedCOW, TRUE, 
                                       ALLOC_FROM_VMKERNEL);
            ASSERT(status == VMK_OK);
            /*
             * If we are reading the page from the CPT file, 
             * assert that the page is not cow shared.
             */
            ASSERT(!args->read || !sharedCOW);
         }

         // sanity check
         ASSERT(mpn != INVALID_MPN);
         if (mpn == INVALID_MPN) {
            VmWarn(worldID, "SG has invalid MPN: index=%d", sgLen);
            status = VMK_FAILURE;
            break;
         }

         // add page to scatter-gather array
	 sgArr->sg[sgLen].offset = offset;
	 sgArr->sg[sgLen].length = PAGE_SIZE;
	 sgArr->sg[sgLen].addr = MPN_2_MA(mpn);
	 sgLen++;
	 if (sgLen < PHYS_SG_SIZE) {
	    continue;
	 }

         if ((status = AllocDoCptIO(world, args->handleID, sgArr, 
                                    args->read, sgLen)) != VMK_OK) {
            break;
         }
         // clear length
         sgLen = 0;
      }
      // abort if any error
      if (status != VMK_OK) {
	 break;
      }
      // Process any read/writes that may be pending
      // due to the use of "continue" in the previous loop.
      if (sgLen != 0) {
         if (AllocDoCptIO(world, args->handleID, sgArr, 
                          args->read, sgLen) != VMK_OK) {
            break;
         }
      }
   }

   // release world alloc lock
   AllocUnlock(world);

   World_Free(world, sgArr);
   World_Release(world);
   return(status);
}

/*
 *-------------------------------------------------------------------------------
 *
 * Alloc_CheckpointCleanup --
 *
 *      Checkpoint info associated with this world is cleaned up. Any
 *      allocated checkpoint buffers are freed up.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      world's checkpoint buffer maybe freed.
 *
 *-------------------------------------------------------------------------------
 */
void
Alloc_CheckpointCleanup(World_Handle *world)
{
   Alloc_CheckpointBuf *buf = &Alloc_AllocInfo(world)->checkpointBuf;
   Alloc_Info *info = Alloc_AllocInfo(world);
   Bool sharesDonated;

   // acquire lock
   AllocLock(world);

   sharesDonated = info->cptSharesDonated;
   // update flag
   info->startingCheckpoint = FALSE;
   info->duringCheckpoint = FALSE;
   info->cptSharesDonated = FALSE;

   if (buf->allocated) {
      // deallocate checkpoint buffer
      AllocCheckpointBufFree(world);
   }

   // release lock
   AllocUnlock(world);
}

/*
 * Mark the phases of a checkpoint.  If wakeup is TRUE, wake up the
 * monitor of this world from a memory wait, if necessary.  Otherwise,
 * the start and end of the saving phase of the checkpoint is marked by
 * called with start as TRUE and FALSE.  In between these calls,
 * return a dummy machine page whenever there is a page fault, and,
 * at the end call, invalidate any entries in the host page tables
 * that have this dummy mpn.
 */
VMK_ReturnStatus
Alloc_MarkCheckpoint(World_ID worldID,
                     Bool wakeup,
                     Bool start)
{
   World_Handle *world;
   Alloc_Info *info;
   MPN mpn;
   VMK_ReturnStatus status;

   // acquire world handle, fail if unable
   if ((world = World_Find(worldID)) == NULL) {
      return VMK_BAD_PARAM;
   }

   // convenient abbrev
   info = Alloc_AllocInfo(world);

   if (wakeup) {
      // get into the startingCheckpoint state
      info->startingCheckpoint = wakeup;
      info->cptSharesDonated = FALSE;
   } else if (start) {
      // acquire lock
      AllocLock(world);

      // allocate dummy zero-filled page on demand;
      //   used for any page faults caused while saving the checkpoint
      if (info->dummyMPN == INVALID_MPN) {
         VMK_ReturnStatus status;
         mpn = MemMap_AllocAnyKernelPage();
         if (mpn == INVALID_MPN) {
            AllocUnlock(world);
            return(VMK_FAILURE);
         }
         info->dummyMPN = mpn;
         status = Util_ZeroMPN(mpn);
         ASSERT(status == VMK_OK);
         VMLOG(0, world->worldID, "allocated dummy mpn=0x%x", info->dummyMPN);
      }

      // release lock
      AllocUnlock(world);

      // allocate fixed-size checkpoint buffer;
      //   used for transient copies of swapped/COW pages during checkpoint
      //   can't hold lock, since interrupts need to be on for call to
      //   MemMap_AllocKernelPageWait().  Lock not needed, since the
      //   buffer will only be used in later phase of checkpoint.
      status = AllocCheckpointBufAlloc(world);
      if (status != VMK_OK) {
	 World_Release(world);
	 return status;
      }

      // Set flag only after checkpoint buffer allocated, preventing
      // a potential race with page faults from userland processes.
      // Although these are never supposed to occur, the checkpoint
      // code to quiesce the system is not perfect.  :-(
      AllocLock(world);
      info->duringCheckpoint = TRUE;
      info->cptSharesDonated = TRUE;
      AllocUnlock(world);
   } else {
      Alloc_CheckpointCleanup(world);
   }

   // release world handle, succeed
   World_Release(world);
   return VMK_OK;
}

/*
 *--------------------------------------------------------------------------
 *
 * Alloc_SetMMapLast --
 *
 *      Set the last address that is being used in the mmap region.
 *
 * Results:
 *      Returns VMK_OK if it could set the last address, error status
 *	if cannot.
 *
 * Side effects:
 *	The last mapped address is set.
 *
 *--------------------------------------------------------------------------
 */
VMK_ReturnStatus
Alloc_SetMMapLast(World_ID worldID, uint32 endMapOffset)
{
   Alloc_PageInfo *pageInfo;
   uint32 endMapPage = BYTES_2_PAGES(endMapOffset) + 1;
   int delta;

   VMK_ReturnStatus status = VMK_OK;

   // Overhead memory excluding anonymous memory is currently
   // not expected to be more than 1GB
   ASSERT(endMapOffset < ALLOC_MAX_MAPPED_OVHD_MEM);
   if (endMapOffset >= ALLOC_MAX_MAPPED_OVHD_MEM) {
      return VMK_NO_MEMORY;
   }

   // lookup world, fail if unable
   World_Handle *world = World_Find(worldID);
   if (world == NULL) {
      return(VMK_BAD_PARAM);
   }

   // convenient abbrev
   pageInfo = &Alloc_AllocInfo(world)->vmPages;

   AllocLock(world);
   delta = endMapPage - pageInfo->cosVmxInfo.numOverheadPages;
   if (delta > 0) {
      VMLOG(1, worldID, 
            "overhead memory exhausted: "
            "curMin=%u, requiredMin=%u, endMapOffset = %u",
            pageInfo->cosVmxInfo.numOverheadPages, endMapPage, endMapOffset);
      status = MemSched_ReserveMem(world, delta);
      if (status == VMK_OK) {
         pageInfo->cosVmxInfo.numOverheadPages = endMapPage;
      }
   }
   AllocUnlock(world);

   World_Release(world);

   return status;
}

/*
 * Transparent page sharing operations
 */

/*
 *----------------------------------------------------------------------
 *
 * AllocCOWHintUpdate --
 *
 *      Process "update", informing world of updated hint status.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May post action for "world".
 *
 *----------------------------------------------------------------------
 */
static void
AllocCOWHintUpdate(COWHintUpdate *update)
{
   World_Handle *world;
   Alloc_Info *info;
   int next;

   // ignore invalid updates
   if (!update->valid) {
      return;
   }

   // acquire world handle
   if ((world = World_Find(update->worldID)) == NULL) {
      /*
       * This could happen when the target world is in the process
       * of cleaning up (so World_Find() won't find it) and it hasn't
       * finished alloc cleanup. (See PR 48890)
       */
      VMLOG(0, update->worldID, "vm not found");
      return;
   }

   // convenient abbrev
   info = Alloc_AllocInfo(world);

   // sanity check
   ASSERT(World_IsVMMWorld(world) && info != NULL);

   // acquire lock
   AllocLock(world);

   if (info->hintUpdateAction != ACTION_INVALID) {
      // buffer hint update
      next = info->hintUpdateNext;
      if (next < PSHARE_HINT_UPDATES_MAX) {
         info->hintUpdate[next].bpn = Alloc_PPNToBPN(world, update->ppn);
         info->hintUpdate[next].status = update->status;
         info->hintUpdateNext++;
         info->hintUpdatePeak = MAX(info->hintUpdatePeak,info->hintUpdateNext);
      } else {
         // set overflow flag
         if (!info->hintUpdateOverflow) {
            info->hintUpdateOverflow = TRUE;
            VMLOG(0, update->worldID, "hint update overflow");
         }
      }

      // post action, update stats
      Action_Post(world, info->hintUpdateAction);
      info->hintUpdateTotal++;
   } else {
      // skip if action no longer valid
      VmWarn(update->worldID, "skip hint update");
   }

   // release lock, handle
   AllocUnlock(world);
   World_Release(world);
}


/*
 *-----------------------------------------------------------------------------
 *
 * AllocCheckPageMatch --
 *
 *     Determines whether the contents of two pages (mpnOrig, mpnNew)
 *      actually match.
 *     "key" is the hash key for mpnNew.
 *
 * Results:
 *     Returns TRUE on a match
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static Bool
AllocCheckPageMatch(uint64 key,
                    MPN mpnOrig,
                    MPN mpnNew)
{
   uint32 *data0, *data1;
   KSEG_Pair *dataPair0, *dataPair1;

   Bool match = TRUE;

   if (PShare_IsZeroKey(key)) {
      // optimization: special-case test for zero page
      data0 = Kseg_MapMPN(mpnOrig, &dataPair0);
      if (!Util_IsZeroPage(data0)) {
         match = FALSE;
      }
      Kseg_ReleasePtr(dataPair0);
   } else {
      // compare contents to ensure identical
      data0 = Kseg_MapMPN(mpnOrig, &dataPair0);
      data1 = Kseg_MapMPN(mpnNew, &dataPair1);
      if (memcmp(data0, data1, PAGE_SIZE) != 0) {
         match = FALSE;
      }
      Kseg_ReleasePtr(dataPair1);
      Kseg_ReleasePtr(dataPair0);
   }

   return match;
}

/*
 *----------------------------------------------------------------------
 *
 * AllocCOWSharePage --
 *
 *      Handle a request to share the MPN "*rtnMPN" at page 
 *	"ppn" in "world".  If "*rtnMPN" is not INVALID_MPN, checks
 *	that it matches the MPN associated with "ppn" in "world".
 *	Sets "*rtnMPN" to the MPN for the shared, read-only,
 *	copy-on-write page with contents identical to the original MPN.
 *	Reclaims page if "*rtnMPN" is not the same as the original MPN.
 *
 * Results:
 *      Returns VMK_OK if page is successfully shared or added as hint,
 *	otherwise returns error code (e.g., VMK_BUSY if page in use
 *	by vmkernel). Sets "rtnMPN" to the MPN for the shared COW page.
 *	Sets "hint" TRUE iff the page was added as a hint only.
 *
 * Side effects:
 *      Updates page mapping data structures for "world".
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
AllocCOWSharePage(World_Handle *world,
                  PPN ppn,
                  MPN *rtnMPN,  // IN/OUT
                  Bool *hint)
{
   uint32 dirIndex, pageIndex, retryCount;
   uint32 countShared;
   KSEG_Pair *dataPair0, *dataPair1, *dirPair;
   MPN frameMPN, dirMPN, hintMPN, mpnShared, mpnOrig;
   MemSchedVmmUsage *usage = MemSched_ClientVmmUsage(world);
   Alloc_PageInfo *pageInfo;
   COWHintUpdate hintUpdate;
   VMK_ReturnStatus status;
   uint32 *data0, *data1;
   World_ID hintWorld, worldID;
   uint64 key, hintKey;
   PPN hintPPN;
   AllocPFrameState frameState;
   Alloc_PFrame *dir;
   Bool hintOnly;
   uint16 framePinCount;
   Bool frameValid;
   MPN mpn = *rtnMPN;

   // initialize
   COWHintUpdateInvalidate(&hintUpdate);
   hintOnly = FALSE;
   retryCount = 0;

   // default reply values
   *rtnMPN = INVALID_MPN;
   *hint = FALSE;

   // sanity check: sharing attempts must originate from monitor
   ASSERT(world == MY_VMM_GROUP_LEADER);

   // convenient abbrevs
   pageInfo = &Alloc_AllocInfo(world)->vmPages;
   worldID = world->worldID;


   // lookup user virtual address, fail if unable
   status = Alloc_LookupPPN(world, ppn, &dirIndex, &pageIndex);
   if (status != VMK_OK) {
      return(status);
   }

   // fail if page not a VM page
   ASSERT(ppn != INVALID_PPN);
   if (ppn == INVALID_PPN) {
      VmWarn(worldID, "Invalid ppn");
      return(VMK_BAD_PARAM);
   }

   // acquire alloc lock for world
   AllocLock(world);

   // fail if page in use by vmkernel
   if ((Alloc_IsCached(world, ppn)) ||
       ((numPCPUs > 1) && Kseg_CheckRemote(worldID, ppn))) {
      AllocUnlock(world);
      return(VMK_BUSY);
   }

   // lookup page frame directory, fail if not found
   dirMPN = pageInfo->pages[dirIndex];
   if (dirMPN == INVALID_MPN) {
      if (mpn != INVALID_MPN) {
         VmWarn(worldID, "ppn=0x%x unmapped: dirIndex 0x%x",
                   ppn, dirIndex);
      }
      AllocUnlock(world);
      return(VMK_NOT_FOUND);
   }

   // map page frame, extract flags and mpn
   dir = Kseg_MapMPN(dirMPN, &dirPair);
   frameState        = Alloc_PFrameGetState(&dir[pageIndex]);
   frameMPN          = Alloc_PFrameGetMPN(&dir[pageIndex]);
   framePinCount  = Alloc_PFrameGetPinCount(&dir[pageIndex]);
   frameValid        = Alloc_PFrameIsValid(&dir[pageIndex]);
   Kseg_ReleasePtr(dirPair);

   // fail if page is pinned
   if (framePinCount > 0 ) {
      AllocUnlock(world);
      return(VMK_BUSY);
   }

   // monitor eliminates duplicates for us
   ASSERT(!Alloc_PFrameStateIsCOW(frameState));
   if (Alloc_PFrameStateIsCOW(frameState)) {
      VMK_ReturnStatus tmpStatus;
      uint64 tmpKey;
      uint32 tmpCount;

      ASSERT(frameValid);
      tmpStatus = PShare_LookupByMPN(frameMPN, &tmpKey, &tmpCount);
      AllocUnlock(world);
      World_Panic(world, "pshare: monitor tried to share cowed page ppn = 0x%x"
                  "mpn = 0x%x count = %d", ppn, frameMPN, tmpCount);
   }

   // lookup MPN if hint, fail if unable
   if (Alloc_PFrameStateIsCOWHint(frameState)) {
      status = PShare_LookupHint(frameMPN, &key, &hintWorld, &hintPPN);
      if (status != VMK_OK) {
         VmWarn(worldID, "ppn=0x%x: hint lookup failed: mpn 0x%x",
                   ppn, frameMPN);
         AllocUnlock(world);
         return(status);
      }
   }

   // fail if page is already swapped or being swapped in/out
   if (Alloc_PFrameStateIsSwap(frameState)) { 
      AllocUnlock(world);
      return(VMK_BUSY);
   }

   // fail if not mapped
   if (frameMPN == INVALID_MPN) {
      if (mpn != INVALID_MPN) {
         VmWarn(worldID, "ppn=0x%x unmapped: pageIndex 0x%x",
                ppn, pageIndex);
      }
      AllocUnlock(world);
      return(VMK_NOT_FOUND);
   }

   // Only REGULAR or COWHINT pages can be turned into COW pages.
   ASSERT(Alloc_PFrameStateIsRegular(frameState) ||
          Alloc_PFrameStateIsCOWHint(frameState));

   // sanity check
   ASSERT((mpn == INVALID_MPN) || (mpn == frameMPN));

   // keep track of original MPN
   mpnOrig = frameMPN;

   // invalidate PPN to MPN mapping from all caches
   // n.b. alloc PPN to MPN cache, remote ksegs checked above

   // invalidate local kseg
   Kseg_InvalidatePtr(world, ppn);

   // OK, nobody should be using this page, and nobody should
   // be able to use it again until the alloc lock is released:
   //   guest: blocked, since invoked from monitor
   //   host:  invalidated from vmx page tables, host TLB
   //   vmk:   not found in alloc cache, or in any kseg cache
   //
   // One caveat: unlike SCSI DMA pages, which are pinned in the
   // monitor, pages involved in network transmits are not pinned.
   // In the extremely unlikely event that this page has contents
   // identical to another page in the system, and it is currently
   // being DMA'd by the network transmit code, and it somehow got
   // evicted quickly from the alloc cache while waiting to be
   // DMA'd by the network card, and the page is reclaimed and then
   // happens to be reallocated quickly, it is possible that the
   // transmitted data could be corrupted.  In this incredibly rare
   // case, the corrupted packet will be detected by the recipient
   // anyway (e.g., bad checksum, as if damaged in transit), and
   // will ultimately cause a retransmit by the guest, if necessary.
   // Note that network receives are DMA'd to a temporary buffer
   // first before being copied into guest memory.

   // remove existing hint, if any
   if (Alloc_PFrameStateIsCOWHint(frameState)) {
      // remove hint, fail if unable
      status = PShare_RemoveHint(mpnOrig, hintWorld, hintPPN);
      ASSERT(status == VMK_OK);
      if (status != VMK_OK) {
         VmWarn(worldID, "hint remove failed: mpn 0x%x", mpnOrig);
         // release lock and fail
         AllocUnlock(world);
         return(status);
      }

      // sanity check
      ASSERT(hintWorld == worldID);

      // update page frame as ordinary page
      dir = Kseg_MapMPN(dirMPN, &dirPair);
      AllocPFrameSetRegular(world, &dir[pageIndex], mpnOrig);
      Kseg_ReleasePtr(dirPair);

      // update usage
      usage->cowHint--;
   }

#ifdef VMX86_DEBUG
   // By this point, we should only be left with REGULAR pages.  All
   // other types have been exclude, and COWHINTs have been coverted
   // into REGULAR pages.
   dir = Kseg_MapMPN(dirMPN, &dirPair);
   ASSERT(Alloc_PFrameIsRegular(&dir[pageIndex]));
   Kseg_ReleasePtr(dirPair);
#endif

   // map page, hash contents
   key = PShare_HashPage(mpnOrig);

   // attempt to share with an existing COW page
   status = PShare_AddIfShared(key, mpnOrig,
                               &mpnShared,
                               &countShared,
                               &hintMPN);


   // consider hint if no direct match
   if ((status != VMK_OK) &&
       (hintMPN != PSHARE_MPN_NULL) &&
       (PShare_LookupHint(hintMPN,
                          &hintKey,
                          &hintWorld,
                          &hintPPN) == VMK_OK)) {
      // 'mpnOrig' matches to the existing hint: 'hintMPN'.
      // First, need to validate hintMPN.  It might be stale
      // since hintMPN is not COW protected.   

      // recompute hash key
      uint64 hintNewKey = PShare_HashPage(hintMPN);
      // hint key match?
      if (hintNewKey == key) {
         // match, so add self unconditionally
         status = PShare_Add(key, mpnOrig, &mpnShared, &countShared);
         if (status != VMK_OK) {
            AllocUnlock(world);
            return(status);
         }

         // prepare "match" hint update
         COWHintUpdateSet(&hintUpdate, hintWorld, 
                          hintPPN, PSHARE_HINT_MATCH);
      } else if (!PShare_HintKeyMatch(hintKey, hintNewKey)) {
         // Contents of hintMPN changed since the time it was added as a
         // hint into pshare
         // prepare "stale" hint update
         COWHintUpdateSet(&hintUpdate, hintWorld, 
                          hintPPN, PSHARE_HINT_STALE);
      }
   }

   // add self as hint (no direct match, no existing hint match)
   if (status != VMK_OK) {
      Bool addHint = TRUE;
      if (VMK_STRESS_RELEASE_OPTION(MEM_SHARE)) {
         // If the stress option is set, we *will* forcibly share
         // the page by copying the contents of the original page
         // to a new page on the same NUMA node.
         MPN mpnNew;
         MM_NodeMask nodeMask = 0x1 << NUMA_MPN2NodeNum(mpnOrig);
         mpnNew = AllocVMPageInt(world, ppn, nodeMask, MM_TYPE_ANY, 0);
         if (mpnNew != INVALID_MPN) {
            data0 = Kseg_MapMPN(mpnOrig, &dataPair0);
            data1 = Kseg_MapMPN(mpnNew, &dataPair1);
            // make a copy of the original page
            memcpy(data1, data0, PAGE_SIZE);

            Kseg_ReleasePtr(dataPair1);
            Kseg_ReleasePtr(dataPair0);

            //add new page unconditionally
            status = PShare_Add(key, mpnNew, &mpnShared, &countShared);
            ASSERT(status == VMK_OK);
            if (mpnShared != mpnNew) {
               AllocFreeVMPage(world, mpnNew);
               ASSERT(mpnOrig != mpnShared);
               ASSERT(countShared > 1);
               VmLog(world->worldID, "mpnNew != mpnShared, freeing mpnNew");
            } else {
               // we're going to account for this new page in the 
               // standard path at the end of the function, so don't
               // double-count it for node stats
               AllocNodeStatsSub(world, mpnNew, 1);
            }
            addHint = FALSE;
         }
      } 
      if (addHint) {
         // add hint, fail if unable
         status = PShare_AddHint(key, mpnOrig, worldID, ppn);
         if (status != VMK_OK) {
            AllocUnlock(world);
            return(status);
         }
         mpnShared = mpnOrig;
         hintOnly = TRUE;
      }
   }


   // check for match if mpn changed
   if (mpnShared != mpnOrig) {
      Bool match;

      // check for match
      match = AllocCheckPageMatch(key, mpnOrig, mpnShared);

      // fail if false match
      if (!match) {
         // should succeed, just added above
         status = AllocPShareRemove(world, key, mpnShared, &countShared);
         ASSERT(status == VMK_OK);
         if ((status == VMK_OK) && (countShared == 0)) {
            // reclaim unreferenced MPN
            AllocFreeVMPage(world, mpnShared);
         }

         AllocUnlock(world);

         // update false match stats
         PShare_ReportCollision(key, worldID, ppn);
         return(VMK_NOT_FOUND);
      }
   }

   // map page frame again, update as shared COW MPN or COW hint
   dir = Kseg_MapMPN(dirMPN, &dirPair);
   frameMPN = Alloc_PFrameGetMPN(&dir[pageIndex]);
   ASSERT(mpnOrig == frameMPN);
   if (hintOnly) {
      AllocPFrameSetCOWHint(&dir[pageIndex], mpnShared);
      usage->cowHint++;
   } else {
      AllocPFrameSetCOW(&dir[pageIndex], mpnShared);
      usage->cow++;
      if (PShare_IsZeroKey(key)) {
         usage->zero++;
      }
   }
   Kseg_ReleasePtr(dirPair);

   // release alloc lock for world
   AllocUnlock(world);

   // process hint update, if any
   AllocCOWHintUpdate(&hintUpdate);

   // free original MPN replaced by shared MPN, if any
   if (mpnShared != mpnOrig) {
      if (VMK_STRESS_RELEASE_OPTION(MEM_SHARE)) {
         // fill the original page with some non-zero values before releasing it
         data0 = Kseg_MapMPN(mpnOrig, &dataPair0);
         memset(data0, 0xff, PAGE_SIZE);
         Kseg_ReleasePtr(dataPair0);
      }
      AllocFreeVMPage(world, mpnOrig);
      AllocNodeStatsAdd(world, mpnShared, 1);

      LOG(1, "Alloc: vm %d: COWSharePage: "
          "shared ppn=0x%x: mpnOrig=0x%x, mpnShared=0x%x",
          worldID, ppn, mpnOrig, mpnShared);
   }

   // successfully shared page or added hint
   *rtnMPN = mpnShared;
   *hint = hintOnly;
   return(VMK_OK);
}



/*
 *----------------------------------------------------------------------
 *
 * AllocCOWCopyPage --
 *
 *      Make a private copy of the shared copy-on-write MPN at
 *	user virtual page "ppn" in "world".  Sets "mpnNew" to
 *	a private writeable page with identical contents.
 *	The parameter "mpnOld" is used for debugging only; it may
 *	be stale since the caller does not hold the "world" alloc lock.
 *
 * Results:
 *	Returns VMK_NOT_SHARED if specified page is not shared.
 *      Returns VMK_OK if page is successfully copied, otherwise
 *	returns an error code.  Sets "mpnNew" to the MPN for the
 *	private writeable page.
 *
 * Side effects:
 *      Updates page mapping data structures for "world".
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
AllocCOWCopyPage(World_Handle *world,
                 PPN ppn,
                 MPN mpnOld,
                 MPN *mpnNew,
                 Bool fromMonitor)
{
   uint32 dirIndex, pageIndex; 
   KSEG_Pair *dataPair0, *dirPair;
   MPN dirMPN, mpnCopy;
   MemSchedVmmUsage *usage;
   Alloc_PageInfo *pageInfo;
   VMK_ReturnStatus status;
   uint32 *data0;
   uint32 countShared;
   uint64 keyShared;
   AllocPFrameState frameState;
   Alloc_PFrame *dir;
   Alloc_Info *info;
   World_ID worldID;
   Bool frameValid;
   Bool stressCOSPShare = FALSE;
   MPN frameMPN;
   Bool ok;

   // default reply value
   *mpnNew = INVALID_MPN;

   // convenient abbrevs, sanity check
   worldID = world->worldID;
   info = Alloc_AllocInfo(world);
   pageInfo = &info->vmPages;
   ASSERT(pageInfo->pages != NULL);

   // acquire alloc lock for world
   AllocLock(world);

   usage = MemSched_ClientVmmUsage(world);

   status = Alloc_LookupPPN(world, ppn, &dirIndex, &pageIndex);
   ASSERT(status == VMK_OK);
   if (status != VMK_OK) {
      VmWarn(worldID, "ppn=0x%x invalid", ppn);
      AllocUnlock(world);
      return(VMK_BAD_PARAM);
   }

   // lookup page frame directory, fail if not found
   dirMPN = pageInfo->pages[dirIndex];
   if (dirMPN == INVALID_MPN) {
      VmWarn(worldID, "ppn 0x%x unmapped: dirIndex 0x%x", ppn, dirIndex);
      AllocUnlock(world);
      return(VMK_NOT_FOUND);
   }

   // map page frame, extract flags and index
   dir = Kseg_MapMPN(dirMPN, &dirPair);
   frameState = Alloc_PFrameGetState(&dir[pageIndex]);
   frameMPN   = Alloc_PFrameGetMPN(&dir[pageIndex]); 
   frameValid = Alloc_PFrameIsValid(&dir[pageIndex]);
   Kseg_ReleasePtr(dirPair);

   // fail if not shared page
   if (!Alloc_PFrameStateIsCOW(frameState)) {
      if (ALLOC_DEBUG_COW_VERBOSE) {
         VMLOG(0, worldID, "ppn=0x%x not shared", ppn);
      }

      // warn if swapped frame
      if (ALLOC_PFRAME_DEBUG && Alloc_PFrameStateIsSwap(frameState)) {
         VmWarn(worldID, "ppn=0x%x is swapped or being swapped", ppn);
      }

      AllocUnlock(world);
      return(VMK_NOT_SHARED);
   }

   // fail if invalid frame
   if (!frameValid) {
      VmWarn(worldID, "ppn=0x%x not valid", ppn);
      AllocUnlock(world);
      return(VMK_NOT_FOUND);
   }

   // valid COW page, lookup pshare info
   if (PShare_LookupByMPN(frameMPN,
                          &keyShared,
                          &countShared) != VMK_OK) {
      VmWarn(worldID, "ppn=0x%x: pshare lookup failed", ppn);
      AllocUnlock(world);
      return(VMK_NOT_FOUND);      
   }

   // check if mpnOld is stale (possible since not locked by caller)
   if (frameMPN != mpnOld) {
      ASSERT(!fromMonitor);
      VMLOG(1, worldID, "ppn=0x%x: mpnOld=0x%x stale, using mpn=0x%x",
            ppn, mpnOld, frameMPN);
   }


   // If we are stressing the system, do not
   // do the PShare_RemoveIfUnshared optimization.
   if (!(VMK_STRESS_RELEASE_OPTION(MEM_SHARE_COS) ||
        VMK_STRESS_RELEASE_OPTION(MEM_SHARE))) {

      // unshare frameMPN if there are no shared references
      status = PShare_RemoveIfUnshared(keyShared, frameMPN);
      if (status == VMK_OK) {
         // no need to copy, since no shared references
         // no need for invalidations, since mpn unchanged
         // future modification:
         //   post action to remove monitor COW trace (unless from Monitor)

         // map page frame again to update as private MPN
         dir = Kseg_MapMPN(dirMPN, &dirPair);
         AllocPFrameSetRegular(world, &dir[pageIndex], frameMPN);
         Kseg_ReleasePtr(dirPair);
         usage->cow--;
         if (PShare_IsZeroKey(keyShared)) {
            usage->zero--;
         }

         AllocUnlock(world);
         *mpnNew = frameMPN;
         return(VMK_OK);
      }
   }


   // invalidate PPN to MPN mapping from all caches
   // n.b. should be uncached or cached read-only, otherwise would
   //      have forced an earlier copy, but we don't depend on this

   // invalidate alloc PPN to MPN cache, local kseg
   Alloc_InvalidateCache(world, ppn);
   Kseg_InvalidatePtr(world, ppn);

   /* We are never required to invalidate the host page tables or TLB
    * Because
    * o If the host use count > 0, it means the host is probably
    *   planning to use this page, and we are executing on behalf of a
    *   page fault originating in the COS, in which case the COS pte
    *   are still not updated
    * o If the host use count > 0 and we are executing this on behalf
    *   of a page fault originating in the MONITOR, it means that the
    *   COS hasnt actually touched this page, so we are fine again.
    * o If the host was actually using this page then we would have
    *   COW copied this page on a page fault from the COS in which
    *   case we would never be exectuing this code.
    */

   // OK, nobody should be writing this page, and nobody should
   // be able to write it again until the alloc lock is released

   // allocate new MPN (with color for ppn), fail if unable
   mpnCopy = AllocVMPage(world, ppn);

   ASSERT(mpnCopy != INVALID_MPN);
   if (mpnCopy == INVALID_MPN) {
      VmWarn(worldID, "unable to alloc page: ppn 0x%x", ppn);
      AllocUnlock(world);
      return(VMK_NO_MEMORY);
   }

   // make private copy
   ok = Util_CopyMA(MPN_2_MA(mpnCopy), MPN_2_MA(frameMPN), PAGE_SIZE);
   ASSERT(ok);

   // map page frame again to update as private MPN
   dir = Kseg_MapMPN(dirMPN, &dirPair);
   AllocPFrameSetRegular(world, &dir[pageIndex], mpnCopy);
   Kseg_ReleasePtr(dirPair);
   usage->cow--;
   if (PShare_IsZeroKey(keyShared)) {
      usage->zero--;
   }

   if (!fromMonitor) {
      if (info->p2mUpdateAction != ACTION_INVALID) {
         // buffer p2m update
         PShare_P2MUpdate *bufferPtr;
         AllocP2MToken p2mToken;
         if (((info->p2mFill + 1) % info->numP2MSlots) == info->p2mDrain) {
            AllocUnlock(world);
            World_Panic(world, "%s", "p2m update buffer full");
            return(VMK_FAILURE);
         }
         AllocP2MInitToken(world, &p2mToken);
         bufferPtr = AllocGetP2MBufferPtr(world, info->p2mFill, &p2mToken);
         ASSERT(bufferPtr);
         ASSERT(bufferPtr->bpn == INVALID_BPN);
         ASSERT(bufferPtr->mpn == INVALID_MPN);
         bufferPtr->bpn = Alloc_PPNToBPN(world, ppn);
         bufferPtr->mpn = frameMPN;
         AllocP2MReleaseToken(world, &p2mToken);
         info->p2mFill = (info->p2mFill + 1) % info->numP2MSlots;
         info->p2mUpdateTotal++;
         info->p2mUpdateCur++;
         info->p2mUpdatePeak = MAX(info->p2mUpdatePeak, info->p2mUpdateCur);

         // post action, update stats
         Action_Post(world, info->p2mUpdateAction);
      } else {
         // skip if action no longer valid
         VmWarn(worldID, "skip p2m update");
      }
   }

   if (VMK_STRESS_RELEASE_OPTION(MEM_SHARE_COS)) {
      uint32 throttle = (uint32) info->cosNextStressPPN;
      if ((throttle % 10000) == 0) {
         uint32 numFreeSlots;
         numFreeSlots = info->numP2MSlots - info->p2mUpdateCur;
         if (!info->cosStressInProgress &&
             (numFreeSlots >
             (ALLOC_STRESS_COS_PAGES_MAX + ALLOC_STRESS_COS_PAGES_SLACK))) {
            info->cosStressInProgress = TRUE;
            stressCOSPShare = TRUE;
         }
      } else {
         info->cosNextStressPPN = (info->cosNextStressPPN + 1)
                                  % pageInfo->numPhysPages;
      }
   }


   // release alloc lock for world
   AllocUnlock(world);

   // Needed as we cant call Helper_Request with IRQ locks held.
   if (stressCOSPShare && (INTERRUPTS_ENABLED())) {
      VMLOG(1, world->worldID, "calling AllocStressCOSPShare");
      Helper_Request(HELPER_MISC_QUEUE, AllocStressCOSPShare, (void *)world->worldID);
   }

   // flush read-only PPN to MPN mappings from ksegs on all remote cpus
   if (numPCPUs > 1) {
      Kseg_FlushRemote(worldID, ppn);
   }

   // if we are *not* from the monitor
   // do not decrement the reference count on the shared page.
   // This will be done in a subsequent call to Alloc_COWP2MUpdatesDone
   // by the monitor. We do this because
   // If we decrement ref count and it drops to 1, another VM
   // could come along and break sharing and it will then
   // be given this shared MPN as a r/w MPN because the ref count drops
   // to 0. It can them actually write
   // to this so called shared MPN because the first VM for which
   // the COS broke sharing is probably still accessing this shared page
   // untill the P2M action gets processed. Thus causing wierd
   // behaviour in the original VM.
   if (fromMonitor) {
      // release shared reference
      // The only possible accesses to the old page are reads by
      // the network transmit code during a very short time window.
      // See comments in AllocCOWSharePage() for full details.
      status = AllocPShareRemove(world, keyShared, frameMPN, &countShared);
      ASSERT(status == VMK_OK);
      if (countShared == 0) {

         // if we are stressing the system fill up the
         // old/shared mpn with non-zero values
         if (VMK_STRESS_RELEASE_OPTION(MEM_SHARE_COS) ||
             VMK_STRESS_RELEASE_OPTION(MEM_SHARE)) {
            // make private copy
            data0 = Kseg_MapMPN(frameMPN, &dataPair0);
            // fill old shared MPN with non-zero values
            memset(data0, 0xff, PAGE_SIZE);
            Kseg_ReleasePtr(dataPair0);
         }

         // reclaim unreferenced MPN
         AllocFreeVMPage(world, frameMPN);
      }
   }

   // successfully copied page
   *mpnNew = mpnCopy;
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * AllocCOWSharePages --
 *
 *      Tries to share the pages in the given list.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      mpnSharedList is updated with the shared mpn
 *      hintList is updated.
 *
 *----------------------------------------------------------------------
 */
static void
AllocCOWSharePages(World_Handle *world,
                   uint32 numPages,          // IN
                   BPN *bpnList,             // IN
                   MPN *mpnList,             // IN/OUT
                   Bool *hintList)           // OUT: is page a hint?
{
   uint32 i;
   VMK_ReturnStatus status; 
   for (i = 0; i < numPages; i++) {
      ASSERT(bpnList[i] != INVALID_BPN && 
             Alloc_IsMainMemBPN(world, bpnList[i]));
      status = AllocCOWSharePage(world, Alloc_BPNToMainMemPPN(world, bpnList[i]),
                                 &mpnList[i], &hintList[i]);
      if (status != VMK_OK) {   
         ASSERT(mpnList[i] == INVALID_MPN && hintList[i] == FALSE);
         if (mpnList[i] != INVALID_MPN || hintList[i] != FALSE) {
            VmWarn(world->worldID, "Invalid COW share state, Killing VM");
            World_Panic(world, "Invalid COW share state");
            return;
         }
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * Alloc_COWSharePages --
 *
 *      Attempt to share the MPNs "mpnList" at pageNums "pageList"
 *      in the current world copy-on-write.  Sets "mpnList" to the
 *	shared, read-only, copy-on-write page with contents identical
 *	to the original mpn.  Reclaims the original mpn if the
 *	new shared mpn is different.
 *
 * Results:
 *      Sets MPNs in "mpnList" to the MPN for the shared copy-on-write page.
 *      Sets hints in "hintOnlyList" for all pages which were marked as
 *      cow hints.
 *
 * Side effects:
 *      Updates page mapping data structures for current world.
 *
 *----------------------------------------------------------------------
 */
VMKERNEL_ENTRY
Alloc_COWSharePages(DECLARE_2_ARGS(VMK_COW_SHARE_PAGES,
                                   uint32, numPages,
                                   MPN, pshareMPN))

{
   World_Handle *world;
   PShare_List *list;
   KSEG_Pair *dirPair;
   PROCESS_2_ARGS(VMK_COW_SHARE_PAGES,
                  uint32, numPages,
                  MPN, pshareMPN);

   world = MY_VMM_GROUP_LEADER;

   ASSERT(PShare_IsEnabled());
   // fail if page sharing disabled
   if (!PShare_IsEnabled()) {
      VmWarn(world->worldID, "called even when sharing is disabled");
      return VMK_NOT_SUPPORTED;
   }
   list = Kseg_MapMPN(pshareMPN, &dirPair);
   ASSERT(list);
   // invoke primitive
   AllocCOWSharePages(world, numPages, list->bpnList, list->mpnList, 
                      list->hintOnlyList);
   Kseg_ReleasePtr(dirPair);
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * AllocCOWUpdateP2MDone --
 *
 *      Decrement the reference count on the sharedMPN. Release the
 *      sharedMPN if the reference count drops to zero.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
static void
AllocCOWUpdateP2MDone(World_Handle *world,
                      MPN mpnShared)
{
   uint32 countShared;
   uint64 key;
   VMK_ReturnStatus status;

   ASSERT(mpnShared != INVALID_MPN);

   ASSERT(AllocIsLocked(world));

   // Compute the key.
   key = PShare_HashPage(mpnShared);

   // Remove the shared reference
   status = AllocPShareRemove(world, key, mpnShared, &countShared);
   ASSERT(status == VMK_OK);
   if (countShared == 0) {
      // reclaim unreferenced MPN
      AllocFreeVMPage(world, mpnShared);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * AllocCOWVerifyP2MDone --
 *
 *      Verify that mpn is no longer in the pshare datastructure.  This should
 *      be true when the vmx broke sharing, didn't tell the monitor, but 
 *      did not change the mpn either.  In this case, the monitor will
 *      lazily find the cow trace but be able to verify that page is no 
 *      longer shared.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
#ifdef VMX86_DEBUG
static Bool
AllocCOWVerifyP2MDone(World_Handle *world,
                      MPN mpn)
{
   uint32 count;
   uint64 key;
   
   ASSERT(AllocIsLocked(world));
   return PShare_LookupByMPN(mpn, &key, &count) == VMK_NOT_FOUND;
}
#endif

/*
 *----------------------------------------------------------------------
 *
 * Alloc_COWCopyPage --
 *
 *      Make a private copy of the shared copy-on-write MPN "mpn" at
 *	"ppn" in the current world.  Sets
 *	"mpnCopy" to the private writeable page with contents identical
 *	to "mpn".
 *
 * Results:
 *      Sets "status" to VMK_OK if successful, otherwise error code.
 *	Sets "mpnCopy" to the MPN for the private writeable copy.
 *
 * Side effects:
 *	Updates page mapping data structures for current world.
 *
 *----------------------------------------------------------------------
 */
VMKERNEL_ENTRY
Alloc_COWCopyPage(DECLARE_3_ARGS(VMK_COW_COPY_PAGE,
                                 BPN, bpn,
                                 MPN, mpn,
                                 MPN *, mpnCopy))
{
   World_Handle *world;
   VMK_ReturnStatus status;
   PPN ppn;
   AllocPFrameState frameState;
   PROCESS_3_ARGS(VMK_COW_COPY_PAGE,
                  BPN, bpn,
                  MPN, mpn,
                  MPN *, mpnCopy);

   // fail if page sharing disabled
   if (!PShare_IsEnabled()) {
      return VMK_NOT_SUPPORTED;
   } 
   world = MY_VMM_GROUP_LEADER;

   if (!Alloc_IsMainMemBPN(world, bpn)) {
      return VMK_BAD_PARAM;
   }

   // block if insufficient memory
   AllocMemWait(world);

   ppn = Alloc_BPNToMainMemPPN(world, bpn);
   
   AllocLock(world);

   status = AllocGetFrameInfoFromPPN(world, ppn, &frameState, mpnCopy);
   if (Alloc_PFrameStateIsCOW(frameState)) {
      // vmx hasn't had a chance to break sharing yet. break now.
      ASSERT(*mpnCopy == mpn);
      AllocUnlock(world);
      status = AllocCOWCopyPage(world, Alloc_BPNToMainMemPPN(world, bpn), mpn,
                                mpnCopy, TRUE);
   } else {
      /*
       * This happens when the vmx breaks sharing.  The monitor finds the page
       * is no longer cowed in the vmkernel.  If the vmx claimed the final 
       * reference to the page, then mpn will not change and the monitor
       * will not be notified.  When it lazily finds the cow trace, we assert 
       * the refcount has been cleaned up properly already by the vmx.  
       * If the vmx claimed a new page, the monitor will get here via a
       * p2mupdate and needs to clean up the reference count on the old mpn. 
       */
      if (mpn != *mpnCopy) {
         // the vmx broke cow already. We can now dec the refcount.
         ASSERT(AllocP2MUpdateExistsForBPN(world, bpn));
         AllocCOWUpdateP2MDone(world, mpn);
      } else {
         // the vmx broke sharing but the mpn did not change.
         ASSERT(AllocCOWVerifyP2MDone(world, mpn));
      }
      AllocUnlock(world);
   }

   if (status != VMK_OK) {
      // report warning only if unexpected failure
      if (status != VMK_NOT_SHARED) {
         VmWarn(world->worldID, "failed: status=%d", status);
      }
      *mpnCopy = INVALID_MPN;
   }
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * Alloc_COWP2MUpdateDone --
 *
 *      Callback from the monitor when the P2M updates have been
 *      processed in the monitor. The vmkernel can now go ahead
 *      and decrement the reference count on the sharedMPNs.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      The shared mpn can be released if the reference count drops to
 *      zero
 *
 *----------------------------------------------------------------------
 */
VMKERNEL_ENTRY
Alloc_COWP2MUpdateDone(DECLARE_1_ARG(VMK_COW_P2M_UPDATE_DONE,
                                      BPN, bpn))
{
   World_Handle *world;
   AllocP2MToken p2mToken;
   Alloc_Info *info;
   PShare_P2MUpdate *bufferPtr;
#ifdef VMX86_DEBUG
   PROCESS_1_ARG(VMK_COW_P2M_UPDATE_DONE,
                 BPN, bpn);
#endif

// fail if page sharing disabled
   if (!PShare_IsEnabled()) {
      return VMK_NOT_SUPPORTED;
   }

   world = MY_VMM_GROUP_LEADER;

   AllocLock(world);
   info = Alloc_AllocInfo(world);

   AllocP2MInitToken(world, &p2mToken);
   bufferPtr = AllocGetP2MBufferPtr(world, info->p2mDrain, &p2mToken);
   ASSERT(info->p2mDrain < info->numP2MSlots);
   ASSERT(info->p2mDrain != info->p2mFill);
   ASSERT(bufferPtr->bpn == bpn);
   bufferPtr->bpn = INVALID_BPN;
   bufferPtr->mpn = INVALID_MPN;
   info->p2mDrain = (info->p2mDrain + 1) % info->numP2MSlots;
   info->p2mUpdateCur--;
   AllocP2MReleaseToken(world, &p2mToken);
   AllocUnlock(world);
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Alloc_COWP2MUpdateGet --
 *
 *      Pickup any pending PPN->MPN updates for current world.
 *
 * Results:
 *      Sets "nUpdates" to the number of updates.
 *	Sets first "nUpdates" elements of "updates" to pending updates.
 *
 * Side effects:
 *      Resets pending updates for current world.
 *
 *----------------------------------------------------------------------
 */
VMKERNEL_ENTRY
Alloc_COWP2MUpdateGet(DECLARE_1_ARG(VMK_COW_P2M_UPDATE,
                                     BPN *, bpn))
{
   World_Handle *world;
   Alloc_Info *info;
   PShare_P2MUpdate *bufferPtr;
   AllocP2MToken p2mToken;
   PROCESS_1_ARG(VMK_COW_P2M_UPDATE,
                 BPN *, bpn);

   world = MY_VMM_GROUP_LEADER;

   // convenient abbrev
   info = Alloc_AllocInfo(world);

   AllocLock(world);

   if (info->p2mDrain == info->p2mFill) {
      *bpn = INVALID_BPN;  // none to fetch
   } else {
      AllocP2MInitToken(world, &p2mToken);
      bufferPtr = AllocGetP2MBufferPtr(world, info->p2mDrain, &p2mToken);
      ASSERT(info->p2mDrain < info->numP2MSlots);
      ASSERT(info->p2mDrain != info->p2mFill);
      ASSERT(bufferPtr->mpn != INVALID_MPN);

      // the monitor actually only cares about the bpn to update
      *bpn = bufferPtr->bpn;
      AllocP2MReleaseToken(world, &p2mToken);
   }
   AllocUnlock(world);
   return VMK_OK;
}
 
/*
 *----------------------------------------------------------------------
 *
 * Alloc_COWGetHintUpdates --
 *
 *      Pickup any pending COW hint updates for current world.
 *
 * Results:
 *      Sets "nUpdates" to the number of updates.
 *	Sets first "nUpdates" elements of "updates" to pending updates.
 *
 * Side effects:
 *      Resets pending updates for current world.
 *
 *----------------------------------------------------------------------
 */
VMKERNEL_ENTRY
Alloc_COWGetHintUpdates(DECLARE_2_ARGS(VMK_COW_HINT_UPDATES,
                                       int *, nUpdates,
                                       PShare_HintUpdate *, updates))
{
   World_Handle *world;
   Alloc_Info *info;
   int i, numHints;
   PROCESS_2_ARGS(VMK_COW_HINT_UPDATES,
                  int *, nUpdates,
                  PShare_HintUpdate *, updates);

   world = MY_VMM_GROUP_LEADER;

   if (*nUpdates < 0) {
      return VMK_BAD_PARAM;
   }
  
   // convenient abbrev
   info = Alloc_AllocInfo(world);

   AllocLock(world);
   
   numHints = MIN(info->hintUpdateNext, *nUpdates);

   // copy pending hint updates
   for (i = 0; i < numHints; i++) {
      // copyout entry
      updates[i].bpn = info->hintUpdate[--info->hintUpdateNext].bpn;
      updates[i].status = info->hintUpdate[info->hintUpdateNext].status;

      // reset entry
      info->hintUpdate[info->hintUpdateNext].bpn = INVALID_BPN;
      info->hintUpdate[info->hintUpdateNext].status = PSHARE_HINT_NONE;
   }
   *nUpdates = numHints;

   if (numHints > 0) {
      // reset overflow flag
      info->hintUpdateOverflow = FALSE;
   }

   AllocUnlock(world);
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * AllocCOWRemoveHint --
 *
 *      Removes COW hint associated with "mpn" at "ppn" 
 *	in the current world, if any.
 *
 * Results:
 *      Returns VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 *	Updates page mapping data structures for current world.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
AllocCOWRemoveHint(World_Handle *world, PPN ppn, MPN mpn)
{
   uint32 dirIndex, pageIndex, frameIndex;
   Alloc_PageInfo *pageInfo;
   MemSchedVmmUsage *usage;
   VMK_ReturnStatus status;
   KSEG_Pair *dirPair;
   Alloc_PFrame *dir;
   AllocPFrameState frameState;
   MPN dirMPN;
   uint64 key;
   PPN hintPPN;
   MPN frameMPN;
   World_ID hintWorld;
   
   // convenient abbrev, sanity check
   pageInfo = &Alloc_AllocInfo(world)->vmPages;
   ASSERT(pageInfo->pages != NULL);

   // acquire alloc lock for world
   AllocLock(world);

   usage = MemSched_ClientVmmUsage(world);

   status = Alloc_LookupPPN(world, ppn, &dirIndex, &pageIndex);
   ASSERT(status == VMK_OK);
   if (status != VMK_OK) {
      VmWarn(world->worldID, "ppn=0x%x invalid", ppn);
      AllocUnlock(world);
      return(VMK_BAD_PARAM);
   }

   // lookup page frame directory, fail if not found
   dirMPN = pageInfo->pages[dirIndex];
   if (dirMPN == INVALID_MPN) {
      VmWarn(world->worldID, "ppn=0x%x unmapped: dirIndex 0x%x",
             ppn, dirIndex);
      AllocUnlock(world);
      return(VMK_NOT_FOUND);
   }

   // map page frame, extract flags and mpn
   dir = Kseg_MapMPN(dirMPN, &dirPair);
   frameState = Alloc_PFrameGetState(&dir[pageIndex]);
   frameIndex = Alloc_PFrameGetIndex(&dir[pageIndex]);
   frameMPN = Alloc_PFrameGetMPN(&dir[pageIndex]);
   Kseg_ReleasePtr(dirPair);

   // fail if not hint frame
   if (!Alloc_PFrameStateIsCOWHint(frameState)) {
      if (ALLOC_DEBUG_COW_VERBOSE) {
         VMLOG(1, world->worldID, "not hint frame: index 0x%x, state = 0x%x",
               frameIndex, frameState);
      }
      // release lock, fail
      AllocUnlock(world);
      return(VMK_NOT_FOUND);
   }

   // lookup hint
   status = PShare_LookupHint(frameMPN, &key, &hintWorld, &hintPPN);
   if (status != VMK_OK) {
      VmWarn(world->worldID, "hint lookup failed: status = 0x%x, mpn = 0x%x"
             " hintWorld = %d, hintPPN = 0x%x, key = 0x%Lx", 
             status, frameMPN, hintWorld, hintPPN, key);
      AllocUnlock(world);
      return(status);
   }
   ASSERT(hintPPN == ppn);
   ASSERT(hintWorld == world->worldID);
   ASSERT((mpn == INVALID_MPN) || (frameMPN == mpn));

   // remove hint
   status = PShare_RemoveHint(frameMPN, hintWorld, hintPPN);
   if (status != VMK_OK) {
      VmWarn(world->worldID, "hint remove failed: hintWorld = %d, status = 0x%x, "
             "frameMPN = 0x%x, hintPPN = 0x%x, ppn = 0x%x", 
             hintWorld, status, frameMPN, hintPPN, ppn);
      // release lock, fail
      AllocUnlock(world);
      return(status);
   }

   // update page frame as ordinary page, preserve vmx-use flag
   dir = Kseg_MapMPN(dirMPN, &dirPair);
   AllocPFrameSetRegular(world, &dir[pageIndex], mpn);
   Kseg_ReleasePtr(dirPair);
   usage->cowHint--;

   // release lock, succeed
   AllocUnlock(world);
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * Alloc_COWRemoveHint --
 *
 *      Removes COW hint associated with "mpn" at guest physical page
 *	"ppn" in the current world, if any.
 *
 * Results:
 *      Sets "status" to VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 *	Updates page mapping data structures for current world.
 *
 *----------------------------------------------------------------------
 */
VMKERNEL_ENTRY
Alloc_COWRemoveHint(DECLARE_2_ARGS(VMK_COW_REMOVE_HINT,
                                   BPN, bpn,
                                   MPN, mpn))
{
   World_Handle *world;
   VMK_ReturnStatus status;
   PPN ppn;
   PROCESS_2_ARGS(VMK_COW_REMOVE_HINT,
                  BPN, bpn, 
                  MPN, mpn);

   // fail if page sharing disabled
   if (!PShare_IsEnabled()) {
      return VMK_NOT_SUPPORTED;
   } 

   world = MY_VMM_GROUP_LEADER;
   if (!Alloc_IsMainMemBPN(world, bpn)) {
      return VMK_BAD_PARAM;
   }

   ppn = Alloc_BPNToMainMemPPN(world, bpn);
   // invoke primitive
   status = AllocCOWRemoveHint(world, ppn, mpn);
   if (status != VMK_OK) {
      VMLOG(1, world->worldID, "failed: status=%d", status);
   }
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * AllocCOWCheck --
 *
 *      Perform consistency check on all COW pages associated
 *	with "world".  Caller must hold alloc lock for "world".
 *
 * Results:
 *	Returns -1 if check not performed, otherwise returns
 *      number of errors found.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
AllocCOWCheck(World_Handle *world)
{
   int badCount, cowCount, worldID;
   MPN *pageDirs;
   uint32 i, j;
   Alloc_PageInfo *pageInfo = &Alloc_AllocInfo(world)->vmPages;

   // convenient abbrevs
   pageDirs = pageInfo->pages;
   worldID = world->worldID;

   // ignore worlds without any VM memory (e.g. POST, helper)
   if (pageDirs == NULL) {
      VMLOG(0, worldID, "ignored (no memory)");
      return(-1);
   }

   // initialize
   cowCount = 0;
   badCount = 0;

   // for each page frame directory
   for (i = 0; i < pageInfo->numPDirEntries; i++) {
      if (pageDirs[i] != INVALID_MPN) {
         MPN dirMPN = pageDirs[i];
         KSEG_Pair *dirPair;
         Alloc_PFrame *dir;

         // for each page frame
         dir = Kseg_MapMPN(dirMPN, &dirPair);
         for (j = 0; j < PAGE_SIZE / sizeof(Alloc_PFrame); j++) {
            AllocPFrameState frameState;
            Bool frameValid;

            // check if valid COW page
            frameState = Alloc_PFrameGetState(&dir[j]);
            frameValid = Alloc_PFrameIsValid(&dir[j]);
            if (frameValid && (Alloc_PFrameStateIsCOW(frameState))) {
               uint32 count;
               VMK_ReturnStatus status;
               uint64 key, keyCheck;
               MPN frameMPN;

               // update stats
               cowCount++;

               // lookup pshare entry
               frameMPN = Alloc_PFrameGetMPN(&dir[j]);
               status = PShare_LookupByMPN(frameMPN, &key, &count);
               if (status != VMK_OK) {
                  VmWarn(worldID, "i=%d, j=%d: pshare lookup failed", i, j);
                  badCount++;
                  continue;
               }

               // check <key, mpn> consistency
               keyCheck = PShare_HashPage(frameMPN);
               if (keyCheck != key) {
                  VmWarn(worldID, "i=%d, j=%d: mpn=0x%x, key=0x%Lx != 0x%Lx",
                            i, j, frameMPN, key, keyCheck);
                  badCount++;
               }
            }
         }

         Kseg_ReleasePtr(dirPair);
      }
   }

   // debugging
   VMLOG(0, worldID, "cowCount=%d, badCount=%d", cowCount, badCount);

   return(badCount);
}



/*
 *----------------------------------------------------------------------
 *
 * AllocCOWCheckPage --
 *
 *      Perform consistency check on page for "world" at "ppn".
 *	Checks correctness of supplied "checkMPN" and "checkCOW" state.
 *	If the page is shared, checks that the hash key is still valid.
 *	Caller must hold alloc lock for "world".
 *
 * Results:
 *	Returns TRUE if passes all consistency checks, otherwise FALSE.
 *      Sets "vmkMPN" to the current MPN.
 *	Sets "vmkCOW" TRUE if the page is shared copy-on-write.
 *	Sets "keyOK" TRUE if the page is a COW page and its hash key
 *      is still valid.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static Bool
AllocCOWCheckPage(World_Handle *world,
                  PPN ppn,
                  MPN checkMPN,
                  Bool checkCOW,
                  MPN *vmkMPN,
                  Bool *vmkCOW,
                  Bool *keyOK)
{
   uint32 dirIndex, pageIndex, frameIndex;
   Alloc_PageInfo *pageInfo;
   MPN dirMPN, frameMPN;
   KSEG_Pair *dirPair;
   Alloc_PFrame *dir;
   AllocPFrameState frameState;
   Bool frameValid;
   World_ID worldID;

   // default reply values
   *vmkMPN = INVALID_MPN;
   *vmkCOW = FALSE;
   *keyOK  = FALSE;

   // convenient abbrevs
   pageInfo = &Alloc_AllocInfo(world)->vmPages;
   worldID = world->worldID;

   // sanity check
   ASSERT(AllocIsLocked(world));

   if (Alloc_LookupPPN(world, ppn, &dirIndex,
                       &pageIndex) != VMK_OK) {
      return(FALSE);
   }

   // lookup page frame directory, fail if none
   dirMPN = pageInfo->pages[dirIndex];
   if (dirMPN == INVALID_MPN) {
      return(FALSE);
   }

   // map frame, extract data
   dir = Kseg_MapMPN(dirMPN, &dirPair);
   frameState = Alloc_PFrameGetState(&dir[pageIndex]);
   frameValid = Alloc_PFrameIsValid(&dir[pageIndex]);
   frameIndex = Alloc_PFrameGetIndex(&dir[pageIndex]);
   frameMPN = Alloc_PFrameGetMPN(&dir[pageIndex]);
   Kseg_ReleasePtr(dirPair);

   // invalid frame?
   if (!frameValid) {
      return(FALSE);
   }

   if (Alloc_PFrameStateIsSwap(frameState)) {
      // set reply values
      *vmkMPN = INVALID_MPN;
      *vmkCOW = FALSE;
      return((checkMPN == frameMPN) && !checkCOW);
   }

   // COW frame?
   if (Alloc_PFrameStateIsCOW(frameState)) {
      uint64 key, checkKey;
      uint32 count;

      // lookup pshare info, fail if unable
      if (PShare_LookupByMPN(frameMPN, &key, &count) != VMK_OK) {
         VmWarn(worldID, "pshare lookup failed: mpn 0x%x", frameMPN);
         *vmkCOW = TRUE;
         return(FALSE);
      }

      // check key consistency (ensure read-only page not modified)
      checkKey = PShare_HashPage(frameMPN);

      // debugging
      if (ALLOC_DEBUG_COW) {
         if (checkKey != key) {
            VmWarn(worldID, "ppn=0x%x, key=0x%Lx != 0x%Lx",
                      ppn, key, checkKey);
         }
         if (checkMPN != frameMPN) {
            VmWarn(worldID,
                   "COW: ppn=0x%x, frameMPN=0x%x != 0x%x, frameState=0x%x",
                   ppn, frameMPN, checkMPN, frameState);
         }
      }

      // set reply values
      *vmkMPN = frameMPN;
      *vmkCOW = TRUE;
      *keyOK  = (checkKey == key);
      return((checkMPN == frameMPN) && checkCOW && (checkKey == key));
   }

   // COW hint frame?
   if (Alloc_PFrameStateIsCOWHint(frameState)) {
      World_ID hintWorld;
      uint64 key;
      PPN ppn;

      // lookup pshare info, fail if unable
      if (PShare_LookupHint(frameMPN, &key, &hintWorld, &ppn) != VMK_OK) {
         VmWarn(worldID, "hint lookup failed: mpn 0x%x", frameMPN);
         *vmkCOW = FALSE;
         return(FALSE);
      }

      // set reply values
      *vmkMPN = frameMPN;
      *vmkCOW = FALSE;
      return((checkMPN == frameMPN) && !checkCOW);
   }

   // ordinary unshared frame
   *vmkMPN = frameMPN;
   *vmkCOW = FALSE;

   // debugging
   if (ALLOC_DEBUG_COW && (checkMPN != frameMPN)) {
      VmWarn(worldID,
             "non-COW: ppn=0x%x, mpn=0x%x != 0x%x, frameState=0x%x, index=0x%x",
             ppn, frameMPN, checkMPN, frameState, frameIndex);
   }

   return((checkMPN == frameMPN) && !checkCOW);
}

/*
 *----------------------------------------------------------------------
 *
 * Alloc_COWCheckPages --
 *
 *      Perform VMK/VMM consistency check on pages listed in the
 *      PShare_COWCheckInfo structure.	Checks correctness of
 *      supplied "check[i].vmmMPN" and "check[i].vmmCOW" state.
 *	If the page is shared, checks that the hash key is still valid.
 *
 * Results:
 *      Sets "check[i].vmkMPN" to the current MPN.
 *	Sets "check[i].vmkCOW" TRUE if the page is shared copy-on-write.
 *	Sets "check[i].keyOK"  TRUE if the hash key is still valid.
 *	Sets "check[i].checkOK" TRUE if passes all consistency checks.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMKERNEL_ENTRY
Alloc_COWCheckPages(DECLARE_2_ARGS(VMK_COW_CHECK_PAGES,
                                   uint32, numPages,
                                   PShare_COWCheckInfo *, check))
{
   uint32 i;
   World_Handle *world;
   PROCESS_2_ARGS(VMK_COW_CHECK_PAGES,
                  uint32, numPages,
                  PShare_COWCheckInfo *, check);

   world = MY_VMM_GROUP_LEADER;

   for (i = 0; i < numPages; i++) {

      ASSERT(check[i].bpn != INVALID_BPN);
      if (!Alloc_IsMainMemBPN(world, check[i].bpn)) {
         return VMK_BAD_PARAM;
      }
      // invoke primitive
      AllocLock(world);
      check[i].checkOK = AllocCOWCheckPage(world, 
                                           Alloc_BPNToMainMemPPN(world, 
                                                                 check[i].bpn),
                                           check[i].vmmMPN, 
                                           check[i].vmmCOW, &check[i].hostMPN, 
                                           &check[i].hostCOW, &check[i].keyOK);
      AllocUnlock(world);
   }
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * AllocCheckpointBufInit --
 *
 *      Initialize checkpoint buffer associated with "world".
 *
 * Results:
 *      Modifies "world".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
AllocCheckpointBufInit(World_Handle *world)
{
   Alloc_CheckpointBuf *buf = &Alloc_AllocInfo(world)->checkpointBuf;
   int i;

   // initialize index, flag
   buf->nextPage = 0;
   buf->allocated = FALSE;

   // initialize pages
   for (i = 0; i < ALLOC_CHECKPOINT_BUF_SIZE; i++) {
      Alloc_CheckpointBufPage *page = &buf->page[i];
      page->mpn = INVALID_MPN;
      page->inUse = FALSE;
   }
}

// Time (in ms) to wait to get each page for the allocInfo checkpointBuf
#define ALLOC_CHECKPOINT_BUF_WAIT 5000

/*
 *----------------------------------------------------------------------
 *
 * AllocCheckpointBufStartPPN --
 *
 *      Checkpoint should be done in chunks of no more than
 *      ALLOC_CHECKPOINT_BUF_SIZE pages. This function will
 *      set the start page Offset of each new chunk.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
static void
AllocCheckpointBufSetStartPPN(World_Handle *world, PPN ppn)
{
   Alloc_CheckpointBuf *buf = &Alloc_AllocInfo(world)->checkpointBuf;
   buf->startPPN = ppn;
}

/*
 *----------------------------------------------------------------------
 *
 * AllocCheckpointBufCheckPPN --
 *
 *      Checks if the given ppn is within the current chunk of
 *      pages that are being written to the checkpoint file
 *
 * Results:
 *      Returns true if the given ppn is within the current chunk. False
 *      otherwise. Also copies the first ppn in the current chunk into
 *      *startPPN.
 *
 * Side effects:
 *      none.
 *
 *
 *----------------------------------------------------------------------
 */
static Bool
AllocCheckpointBufCheckPPN(World_Handle *world,
                           PPN ppn,
                           PPN *startPPN,
                           Bool fromVMX)
{
   *startPPN = Alloc_AllocInfo(world)->checkpointBuf.startPPN;
   if (*startPPN == INVALID_PPN) {
      return FALSE;
   } else {
      if (fromVMX) {
         return ((uint32)(ppn - *startPPN) < 
                 (uint32)ALLOC_CHECKPOINT_BUF_SIZE);
      } else {
         return FALSE;
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * AllocCheckpointBufAlloc --
 *
 *      Allocate checkpoint buffer memory for "world".
 *
 * Results:
 *      Modifies "world".
 *
 * Side effects:
 *      Allocates machine pages.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
AllocCheckpointBufAlloc(World_Handle *world)
{
   Alloc_CheckpointBuf *buf = &Alloc_AllocInfo(world)->checkpointBuf;
   int i;

   // sanity checks
   ASSERT(!buf->allocated);
   ASSERT(!AllocIsLocked(world));

   // debugging
   if (ALLOC_DEBUG_CHECKPOINT) {
      VMLOG(0, world->worldID, "alloc buffer: locked=%u",
               MemSched_ClientVmmUsage(world)->locked);
   }

   AllocCheckpointBufSetStartPPN(world, INVALID_PPN);

   // update index, flag
   buf->nextPage = 0;
   buf->allocated = TRUE;

   // allocate memory
   for (i = 0; i < ALLOC_CHECKPOINT_BUF_SIZE; i++) {
      Alloc_CheckpointBufPage *page = &buf->page[i];
      page->inUse = FALSE;
      page->mpn = INVALID_MPN;
      if (MemSched_MemoryIsLowWait(ALLOC_CHECKPOINT_BUF_WAIT) == VMK_OK) {
	 page->mpn = MemMap_AllocAnyKernelPage();
      }
      if (page->mpn == INVALID_MPN) {
         AllocLock(world);
         // issue warning, deallocate partial buffer, fail
         VmWarn(world->worldID, "insufficient memory");
         AllocCheckpointBufFree(world);
         AllocUnlock(world);
         return(VMK_NO_MEMORY);
      }
   }

   // succeed
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * AllocCheckpointBufFree --
 *
 *      Reclaim checkpoint buffer memory for "world".
 *	Caller must hold "world" alloc lock.
 *
 * Results:
 *      Modifies "world".
 *
 * Side effects:
 *      Deallocates machine pages.
 *
 *----------------------------------------------------------------------
 */
static void
AllocCheckpointBufFree(World_Handle *world)
{
   Alloc_CheckpointBuf *buf = &Alloc_AllocInfo(world)->checkpointBuf;
   int i;

   // sanity checks
   ASSERT(AllocIsLocked(world));
   ASSERT(buf->allocated);

   // debugging
   if (ALLOC_DEBUG_CHECKPOINT) {
      VMLOG(0, world->worldID, "free buffer: locked=%u",
               MemSched_ClientVmmUsage(world)->locked);
   }

   // release any pages still in use
   AllocCheckpointBufRelease(world);

   // reclaim memory
   for (i = 0; i < ALLOC_CHECKPOINT_BUF_SIZE; i++) {
      Alloc_CheckpointBufPage *page = &buf->page[i];
      ASSERT(!page->inUse);
      if (page->mpn != INVALID_MPN) {
         MemMap_FreeKernelPage(page->mpn);
         page->mpn = INVALID_MPN;
      }
   }

   // update index, flag
   buf->nextPage = 0;
   buf->allocated = FALSE;
}

/*
 *----------------------------------------------------------------------
 *
 * AllocCheckpointBufGetPage --
 *
 *      Obtain unused page from checkpoint buffer memory for "world".
 *
 *      In case of Checkpoints to a COS file, the code that
 *      does the checkpoint, maps in physical memory in
 *      ALLOC_CHECKPOINT_BUF_SIZE sized chunks, and unmaps it before
 *      it maps another chunk, we explicitly call
 *      AllocCheckpointBufferRelease when the unmap occurs,
 *      so we should never hit a case where we dont have any free
 *      buffer pages.
 *
 *      In case of Checkpoint to a VMFS file also we call
 *      AllocCheckpointBufferRelease, so even in this case we
 *      should be guranteed of a free buf page.
 *
 *	Caller must hold "world" alloc lock.
 *
 * Results:
 *      Returns MPN from checkpoint buffer pool if successful.
 *	Returns INVALID_MPN if error.
 *
 * Side effects:
 *      May invalidate COS page table entries associated with
 *	checkpoint buffer pages.
 *
 *----------------------------------------------------------------------
 */
static MPN
AllocCheckpointBufGetPage(World_Handle *world)
{
   Alloc_CheckpointBuf *buf = &Alloc_AllocInfo(world)->checkpointBuf;
   Alloc_CheckpointBufPage *page;

   // sanity checks
   ASSERT(AllocIsLocked(world));
   ASSERT(buf->allocated);
   ASSERT(buf->nextPage >= 0);
   ASSERT(buf->nextPage < ALLOC_CHECKPOINT_BUF_SIZE);

   // use next page
   page = &buf->page[buf->nextPage];

   // sanity check
   ASSERT(page->mpn != INVALID_MPN);
   ASSERT(!page->inUse);
   // try to release if still in use
   if (page->inUse) {
      // issue warning, fail
      VmWarn(world->worldID, "overflow: next=%d", buf->nextPage);
      return(INVALID_MPN);
   }

   // update state
   page->inUse = TRUE;

   // debugging
   if (ALLOC_DEBUG_CHECKPOINT_VERBOSE) {
      VMLOG(0, world->worldID,
               "next=%d, mpn=0x%x", buf->nextPage, page->mpn);
   }

   // advance, handle wraparound
   buf->nextPage++;
   if (buf->nextPage >= ALLOC_CHECKPOINT_BUF_SIZE) {
      buf->nextPage = 0;
   }

   // succeed
   return(page->mpn);
}

/*
 *----------------------------------------------------------------------
 *
 * AllocCheckpointBufRelease --
 *
 *	Release all pages in checkpoint buffer pool for "world",
 *	allowing them to be recycled by AllocCheckpointBufGetPage().
 *	Caller must hold "world" alloc lock.
 *
 * Results:
 *      Modifies "world".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
AllocCheckpointBufRelease(World_Handle *world)
{
   Alloc_CheckpointBuf *buf = &Alloc_AllocInfo(world)->checkpointBuf;
   int i;

   // sanity checks
   ASSERT(AllocIsLocked(world));
   ASSERT(buf->allocated);

   AllocCheckpointBufSetStartPPN(world, INVALID_PPN);
   // debugging
   if (ALLOC_DEBUG_CHECKPOINT_VERBOSE) {
      VMLOG(0, world->worldID, "release buffer: next=%d", buf->nextPage);
   }

   // mark all pages unused
   for (i = 0; i < ALLOC_CHECKPOINT_BUF_SIZE; i++) {
      Alloc_CheckpointBufPage *page = &buf->page[i];
      page->inUse = FALSE;
   }

   // reset index
   buf->nextPage = 0;
}

/*
 *----------------------------------------------------------------------
 *
 * AllocIncPinCount --
 *      Increments the pin count of the given allocPFrame.
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
AllocIncPinCount(World_Handle *world, PPN ppn, Alloc_PFrame *allocPFrame)
{
   uint16 curCount = Alloc_PFrameGetPinCount(allocPFrame);
   ASSERT(curCount < ALLOC_MAX_PIN_COUNT);
   if (curCount < ALLOC_MAX_PIN_COUNT) {
      curCount++;
   } else {
      /*
       * In release build, if the pin count exceeds limit, we set it
       * to sticky, i.e. never remove it.
       */
      VmWarn(world->worldID, "allocFrame[0x%x] pin count exceeded", ppn);
      curCount = ALLOC_PIN_STICKY_COUNT;
   }
   Alloc_PFrameSetPinCount(allocPFrame, curCount);
}

/*
 *----------------------------------------------------------------------
 *
 * AllocDecPinCount --
 *      Decrements the pin count of the given allocPFrame.
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
AllocDecPinCount(World_Handle *world, PPN ppn, Alloc_PFrame *allocPFrame)
{
   uint16 curCount = Alloc_PFrameGetPinCount(allocPFrame);
   ASSERT(curCount > 0 && curCount <= ALLOC_MAX_PIN_COUNT);
   if (UNLIKELY(curCount == 0)) {
      VmWarn(world->worldID, "allocFrame[0x%x] count was zero", ppn);
   } else if (curCount <= ALLOC_MAX_PIN_COUNT) {
      Alloc_PFrameSetPinCount(allocPFrame, curCount-1);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * AllocStressCOSPShare --
 *
 *      This function simulates the case where the COS touches a large
 *   number of pages. This is currently used to stress the case
 *   where the COS breaks COW sharing.
 *
 *      NOTE: this function is purely for testing purpose.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
void
AllocStressCOSPShare(void *clientData)
{
   World_ID worldID = (World_ID)clientData;
   World_Handle *world, *groupLeader;
   Alloc_Info *info;
   Alloc_PageInfo *pageInfo;
   uint32 numVMPages;
   PPN startPPN;
   VMK_ReturnStatus status;
   uint32 i;
   world = World_Find(worldID);
   if (!world) {
      return;
   }

   // convenient abbrevs
   groupLeader = World_GetVmmLeader(world);
   info = Alloc_AllocInfo(groupLeader);
   pageInfo = &info->vmPages;

   VMLOG(1, worldID, "%s", "starting");

   numVMPages = MIN(pageInfo->numPhysPages, ALLOC_STRESS_COS_PAGES_MAX);

   startPPN = info->cosNextStressPPN;
   if ((startPPN + numVMPages) >  pageInfo->numPhysPages) {
      startPPN = pageInfo->numPhysPages - numVMPages;
      info->cosNextStressPPN = 0;
   } else {
      info->cosNextStressPPN =
         (info->cosNextStressPPN + numVMPages) % pageInfo->numPhysPages;
   }

   status = Alloc_PhysMemMap(worldID, startPPN, numVMPages * PAGE_SIZE);
   ASSERT(status == VMK_OK);

   for (i = 0; i < numVMPages; i++) {
      MPN mpn;
      status = Alloc_PageFaultWrite(groupLeader, startPPN + i, &mpn, ALLOC_FROM_VMKERNEL);
      ASSERT((status == VMK_OK && mpn != INVALID_MPN) ||
             status == VMK_BUSY);
   }

   status = Alloc_PhysMemUnmap(worldID, startPPN, numVMPages * PAGE_SIZE);
   ASSERT(status == VMK_OK);

   // we can live without acquiring the alloc lock here
   info->cosStressInProgress = FALSE;

   World_Release(world);
   VMLOG(1, worldID, "%s", "finished");
}

/*
 *----------------------------------------------------------------------
 *
 * AllocPhysMemMapInt --
 *      Marks the pages within the specified region as being used
 *      by the VMX or Vmkernel, it does this by incrementing the ppn use
 *      count of every page in this region.
 *
 * Results:
 *      VMK_OK on success.
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
AllocPhysMemMapInt(World_Handle *world, PPN ppn, uint32 numPages)
{
   uint32 i;
   uint32 dirIndex, pageIndex;
   MPN cachedDirMPN = INVALID_MPN;
   MPN dirMPN;
   Alloc_PageInfo *pageInfo;
   Alloc_PFrame *dir = NULL;
   KSEG_Pair *dirPair = NULL;

   ASSERT(AllocIsLocked(world));
   if (ALLOC_HOST_REF_COUNT_DEBUG) {
      static uint32 maxPagesSeen = 0;
      if (numPages > maxPagesSeen) {
         maxPagesSeen = numPages;
         VMLOG(0, world->worldID, " max len = %uK", PagesToKB(numPages));
      }
   }

   pageInfo = &Alloc_AllocInfo(world)->vmPages;

   if (Alloc_AllocInfo(world)->duringCheckpoint) {
      AllocCheckpointBufSetStartPPN(world, ppn);
   }
   for (i = 0; i < numPages; i++) {
      VMK_ReturnStatus status;

      ASSERT(ppn + i != INVALID_PPN);
      // lookup ppn, fail if unable
      status = Alloc_LookupPPN(world, ppn + i, &dirIndex, &pageIndex);
      ASSERT(status == VMK_OK);
      if (status != VMK_OK) {
         return (status);
      }

      // lookup page frame directory
      dirMPN = pageInfo->pages[dirIndex];
      if (dirMPN == INVALID_MPN) {
         // create new page frame directory
         dirMPN = AllocMapPageDir(world, &pageInfo->pages[dirIndex]);
         ASSERT(dirMPN == pageInfo->pages[dirIndex]);
      }
      ASSERT(dirMPN != INVALID_MPN);

      // make sure we kseg the correct dir
      if (cachedDirMPN != dirMPN) {
         if (dirPair != NULL) {
            Kseg_ReleasePtr(dirPair);
         }
         dir = Kseg_MapMPN(dirMPN, &dirPair);
         cachedDirMPN = dirMPN;
      }

      AllocIncPinCount(world, ppn+i, &dir[pageIndex]);
   }

   if (dirPair != NULL) {
      Kseg_ReleasePtr(dirPair);
   }

   return VMK_OK;
}

VMK_ReturnStatus
Alloc_PhysMemMap(World_ID worldID, PPN ppn, uint32 len) 
{
   World_Handle *world;
   VMK_ReturnStatus status;
   // acquire world handle, fail if unable
   if ((world = World_Find(worldID)) == NULL) {
      WarnVmNotFound(worldID);
      return VMK_BAD_PARAM;
   }
   AllocLock(world);
   status = AllocPhysMemMapInt(world, ppn, CEIL(len, PAGE_SIZE));
   AllocUnlock(world);
   // release world handle
   World_Release(world);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * AllocPhysMemUnmapInt --
 *      Decrements the ppn use count of every page in this region.
 *
 * Results:
 *      VMK_OK on success.
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
AllocPhysMemUnmapInt(World_Handle *world, PPN ppn, uint32 numPages)
{
   uint32 i;
   uint32 dirIndex, pageIndex;
   MPN cachedDirMPN = INVALID_MPN;
   MPN dirMPN;
   Alloc_PageInfo *pageInfo;
   Alloc_PFrame *dir = NULL;
   KSEG_Pair *dirPair = NULL;

   ASSERT(AllocIsLocked(world));
   pageInfo = &Alloc_AllocInfo(world)->vmPages;

   for (i = 0; i < numPages; i++) {
      VMK_ReturnStatus status;

      ASSERT(ppn + i != INVALID_PPN);
      // lookup ppn, fail if unable
      status = Alloc_LookupPPN(world, ppn + i, &dirIndex, &pageIndex);
      ASSERT(status == VMK_OK);
      if (status != VMK_OK) {
         return (status);
      }

      // lookup page frame directory
      dirMPN = pageInfo->pages[dirIndex];
      ASSERT(dirMPN != INVALID_MPN);

      // make sure we kseg the correct dir
      if (cachedDirMPN != dirMPN) {
         if (dirPair != NULL) {
            Kseg_ReleasePtr(dirPair);
         }
         dir = Kseg_MapMPN(dirMPN, &dirPair);
         cachedDirMPN = dirMPN;
      }

      AllocDecPinCount(world, ppn + i, &dir[pageIndex]);
   }

   if (dirPair != NULL) {
      Kseg_ReleasePtr(dirPair);
   }

   // recycle checkpoint buffers
   if (Alloc_AllocInfo(world)->duringCheckpoint) {
      AllocCheckpointBufRelease(world);
   }
   return VMK_OK;
}

VMK_ReturnStatus
Alloc_PhysMemUnmap(World_ID worldID, PPN ppn, uint32 len)
{
   World_Handle *world;
   VMK_ReturnStatus status;
   // acquire world handle, fail if unable
   if ((world = World_Find(worldID)) == NULL) {
      return VMK_BAD_PARAM;
   }
   AllocLock(world);
   status = AllocPhysMemUnmapInt(world, ppn, CEIL(len, PAGE_SIZE));
   AllocUnlock(world);
   // release world handle
   World_Release(world);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Alloc_GetPhysMemRange --
 *
 *    Translate a range of guest PPNs of "world" into a list of MPNs.
 *
 * Results:
 *      VMK_OK on success.
 *
 * Side effects:
 *      Guest PPNs will not be able to swap/cow/remap until 
 *      Alloc_ReleasePhysMemRange() is called.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Alloc_GetPhysMemRange(World_Handle *world,      // IN
                      PPN startPPN,             // IN
                      uint32 numPages,          // IN
                      Bool writeable,           // IN
                      Bool canBlock,            // IN
                      MPN *mpnList)             // OUT
{
   uint32 i;
   VMK_ReturnStatus status;

   AllocLock(world);

   status = AllocPhysMemMapInt(world, startPPN, numPages);
   if (UNLIKELY(status != VMK_OK)) {
      return status;
   }

   for (i = 0; i < numPages; i++) {
      status = AllocPPNToMPN(world, startPPN + i, writeable, canBlock, &mpnList[i]);
      if (status != VMK_OK) {
         break;
      }
   }

   if (status != VMK_OK) {
      (void)AllocPhysMemUnmapInt(world, startPPN, numPages);
   }

   AllocUnlock(world);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Alloc_ReleasePhysMemRange --
 *
 *    Undo the side effect of Alloc_GetPhysMemRange().
 *
 * Results:
 *      VMK_OK on success.
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Alloc_ReleasePhysMemRange(World_Handle *world,
                          PPN startPPN,
                          uint32 numPages)
{
   VMK_ReturnStatus status;
   
   AllocLock(world);
   status = AllocPhysMemUnmapInt(world, startPPN, numPages);
   AllocUnlock(world);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * AllocRemapPage --
 *
 *	Change mapping for "ppn" in "world" from "mpnOld" to "mpnNew".
 *	Makes "mpnNew" an identical copy of "mpnOld", and updates page
 *	mapping appropriately.  Caller must hold "world" alloc lock.
 *
 * Results:
 *      Returns VMK_OK if page is successfully remapped, otherwise
 *	returns error code; e.g. VMK_BUSY if page is currently in use.
 *
 * Side effects:
 *      Updates page mapping data structures for "world".
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
AllocRemapPage(World_Handle *world, PPN ppn, MPN mpnOld, MPN mpnNew)
{
   KSEG_Pair *dataPairOld, *dataPairNew, *dirPair;
   uint32 dirIndex, pageIndex, frameIndex;
   uint16 framePinCount;
   AllocPFrameState frameState;
   World_ID hintWorld, worldID;
   uint32 *dataOld, *dataNew;
   VMK_ReturnStatus status;
   MemSchedVmmUsage *usage = MemSched_ClientVmmUsage(world);
   Alloc_PageInfo *pageInfo;
   Alloc_PFrame *dir;
   MPN frameMPN, dirMPN;
   PPN hintPPN;
   uint64 hintKey;

   // sanity checks: lock held, remap attempt from monitor
   ASSERT(AllocIsLocked(world));
   ASSERT(world == MY_VMM_GROUP_LEADER);

   // convenient abbrevs
   pageInfo = &Alloc_AllocInfo(world)->vmPages;
   worldID = world->worldID;

   // fail if invalid parameter
   if ((mpnOld == INVALID_MPN) || (mpnNew == INVALID_MPN)) {
      return(VMK_BAD_PARAM);
   }

   // done if no remapping required
   if (mpnOld == mpnNew) {
      VMLOG(0, worldID, "same MPN: old=new=0x%x", mpnOld);
      return(VMK_OK);
   }

   // lookup user virtual page number, fail if unable
   status = Alloc_LookupPPN(world, ppn, &dirIndex, &pageIndex);
   if (status != VMK_OK) {
      return(status);
   }

   // fail if checkpointing
   if (Alloc_AllocInfo(world)->duringCheckpoint) {
      return(VMK_BUSY);
   }

   // check if PPN currently in use
   if (ppn != INVALID_PPN) {
      // fail if page in use by vmkernel
      if ((Alloc_IsCached(world, ppn)) ||
          ((numPCPUs > 1) && Kseg_CheckRemote(worldID, ppn))) {
         return(VMK_BUSY);
      }
   }

   // lookup page frame directory, fail if not found
   dirMPN = pageInfo->pages[dirIndex];
   if (dirMPN == INVALID_MPN) {
      VmWarn(worldID, "ppn=0x%x dir unmapped", ppn);
      return(VMK_NOT_FOUND);
   }

   // map page frame, extract flags and mpn
   dir = Kseg_MapMPN(dirMPN, &dirPair);
   frameState = Alloc_PFrameGetState(&dir[pageIndex]);
   frameIndex = Alloc_PFrameGetIndex(&dir[pageIndex]);
   frameMPN = Alloc_PFrameGetMPN(&dir[pageIndex]);
   framePinCount = Alloc_PFrameGetPinCount(&dir[pageIndex]);
   Kseg_ReleasePtr(dirPair);

   // fail if page is pinned
   if (framePinCount > 0) {
      VMLOG(0, worldID, "ppn=0x%x pinned", ppn);
      return(VMK_BUSY);
   }

   // fail if page is already swapped or being swapped in/out
   if (Alloc_PFrameStateIsSwap(frameState)) {
      VMLOG(1, worldID, "ppn=0x%x swapped", ppn);
      return(VMK_BUSY);
   }

   // fail if shared
   if (Alloc_PFrameStateIsCOW(frameState)) {
      VMLOG(1, worldID, "ppn=0x%x shared", ppn);
      return(VMK_SHARED);
   }

   // lookup MPN if hint, fail if unable
   if (Alloc_PFrameStateIsCOWHint(frameState)) {
      status = PShare_LookupHint(frameMPN, &hintKey, &hintWorld, &hintPPN);
      if (status != VMK_OK) {
         VmWarn(worldID, "ppn=0x%x: hint lookup failed: mpn=0x%x",
                ppn, frameMPN);
         return(status);
      }
   }

   // fail if not mapped
   if (frameMPN == INVALID_MPN) {
      VmWarn(worldID, "ppn=0x%x unmapped", ppn);
      return(VMK_NOT_FOUND);
   }

   // sanity check
   if (mpnOld != frameMPN) {
      VmWarn(worldID, "unable to remap ppn=0x%x, old mpn=0x%x, frame mpn=0x%x", 
             ppn, mpnOld, frameMPN);
      return(VMK_BUSY);
   }

   // invalidate PPN to MPN mapping from all caches
   // n.b. alloc PPN to MPN cache, remote ksegs checked above

   // invalidate local kseg
   if (ppn != INVALID_PPN) {
      Kseg_InvalidatePtr(world, ppn);
   }

   // OK, nobody should be using this page
   // XXX except possibly network transmit code (sigh)

   // remove existing hint, if any
   if (Alloc_PFrameStateIsCOWHint(frameState)) {
      // remove hint, fail if unable
      status = PShare_RemoveHint(mpnOld, hintWorld, hintPPN);
      ASSERT(status == VMK_OK);
      if (status != VMK_OK) {
         VmWarn(worldID, "hint remove failed: index 0x%x", frameIndex);
         return(status);
      }

      // sanity check
      ASSERT(hintWorld == worldID);

      // update page frame as ordinary page
      dir = Kseg_MapMPN(dirMPN, &dirPair);
      AllocPFrameSetRegular(world, &dir[pageIndex], mpnOld);
      Kseg_ReleasePtr(dirPair);

      // update usage
      usage->cowHint--;
   }

   // copy page contents
   ASSERT(mpnOld != mpnNew);
   dataOld = Kseg_MapMPN(mpnOld, &dataPairOld);
   dataNew = Kseg_MapMPN(mpnNew, &dataPairNew);
   memcpy(dataNew, dataOld, PAGE_SIZE);
   Kseg_ReleasePtr(dataPairNew);
   Kseg_ReleasePtr(dataPairOld);

   // map page frame again, update using mpnNew
   dir = Kseg_MapMPN(dirMPN, &dirPair);
   frameMPN = Alloc_PFrameGetMPN(&dir[pageIndex]);
   ASSERT(mpnOld == frameMPN);
   AllocPFrameSetRegular(world, &dir[pageIndex], mpnNew);
   Kseg_ReleasePtr(dirPair);

   // succeed
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * AllocRemapPageLow --
 *
 *	Attempts to allocate a low memory page "mpnLow", and change
 *	mapping for "ppn" in "world" from "mpnOld" to "mpnLow".
 *	If successful, makes "mpnLow" an identical copy of "mpnOld",
 *	updates page mapping appropriately, and deallocates "mpnOld".
 *      This routine may block for at most "msTimeout" milliseconds.
 *
 * Results:
 *      Returns VMK_OK if page is successfully remapped, otherwise
 *	returns error code.  Sets "mpnLow" to new MPN if successful,
 *	otherwise INVALID_MPN.
 *
 * Side effects:
 *      Updates page mapping data structures for "world".
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
AllocRemapPageLow(World_Handle *world,
                  PPN ppn,
                  MPN mpnOld,
                  uint32 msTimeout,
                  MPN *mpnLow)
{
   VMK_ReturnStatus status;
   MPN mpnNew;

   // default reply value
   *mpnLow = INVALID_MPN;

   ASSERT(ppn != INVALID_PPN);
   // fail if "mpnOld" already in low memory
   if (IsLowMPN(mpnOld) && !(VMK_STRESS_RELEASE_OPTION(MEM_REMAP_LOW))) {
      VMLOG(1, world->worldID, "mpnOld=0x%x already low", mpnOld);
      return(VMK_BAD_PARAM);
   }

   // attempt to allocate page from low memory w/o blocking
   mpnNew = AllocVMLowPage(world, ppn, msTimeout);
   if (mpnNew == INVALID_MPN) {
      return(VMK_NO_MEMORY);
   }

   // invalidate page from alloc cache, remap
   AllocLock(world);
   (void) Alloc_InvalidateCache(world, ppn);
   status = AllocRemapPage(world, ppn, mpnOld, mpnNew);
   AllocUnlock(world);

   // reclaim new page and fail if unable to remap
   if (status != VMK_OK) {
      AllocFreeVMPage(world, mpnNew);
      return(status);
   }

   // reclaim old page, succeed
   *mpnLow = mpnNew;
   AllocFreeVMPage(world, mpnOld);
   return(VMK_OK);
}

/*
 *-----------------------------------------------------------------------------
 *
 * AllocResharePageNode --
 *
 *     Tries to share the page at mpnOld with some already-shared page on a
 *     node within the "nodeMask" memory affinity mask.
 *     Sets "*mpnNode" to be the new, shared MPN, if a sharing candidate is
 *     found, or INVALID_MPN otherwise.
 *
 * Results:
 *     Returns VMK_OK if a sharing match was found, VMK_NOT_SHARED otherwise
 *
 * Side effects:
 *     Updates page sharing data,
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
AllocResharePageNode(World_Handle *world,
                     MPN mpnOld,
                     uint32 nodeMask,
                     PPN ppn,
                     MPN *mpnNode)
{
   NUMA_Node n;
   uint64 key;

   ASSERT(AllocIsLocked(world));

   VMLOG(2, world->worldID, "remap candidate mpnOld:0x%x shared, try reshare", mpnOld);
   key = PShare_HashPage(mpnOld);

   // XXX: biased towards lowest nodes
   NUMA_FORALL_NODES(n) {
      VMK_ReturnStatus status;

      if (nodeMask & MEMSCHED_NODE_AFFINITY(n)) {
         uint32 count, hint;
         uint64 nodeKey;

         // munge the lower bits of the hash to search on destination node
         nodeKey = PShare_HashToNodeHash(key, n);
         status = PShare_AddIfShared(nodeKey, mpnOld, mpnNode, &count, &hint);

         // we found a match on a destination node
         if (status == VMK_OK) {
            AllocPFramePair framePair;
            Bool match;

            // check for false matches
            match = AllocCheckPageMatch(nodeKey, mpnOld, *mpnNode);
            if (!match) {
               status = AllocPShareRemove(world, nodeKey, *mpnNode, &count);
               ASSERT(status == VMK_OK);
               if (count == 0) {
                  // no longer referenced
                  AllocFreeVMPage(world, *mpnNode);
               }
               continue;
            }

            // remove our entry for the old key
            AllocPShareRemove(world, key, mpnOld, &count);
            if (count == 0) {
               // no longer referenced
               AllocFreeVMPage(world, mpnOld);
            }

            // update our COW entry with the proper index
            status = AllocGetPFrameFromPPN(world, ppn, &framePair);
            ASSERT(status == VMK_OK);
            ASSERT(Alloc_PFrameStateIsCOW(Alloc_PFrameGetState(framePair.pframe)));
            AllocPFrameSetCOW(framePair.pframe, *mpnNode);
            AllocPFrameReleasePair(&framePair);

            VMLOG(1, world->worldID, "reshared: mpnShared=0x%x, count=%u", *mpnNode, count);

            // update NUMA stats
            AllocNodeStatsAdd(world, *mpnNode, 1);

            return (VMK_OK);
         }
      }
   }

   *mpnNode = INVALID_MPN;
   return (VMK_NOT_SHARED);
}

/*
 *----------------------------------------------------------------------
 *
 * AllocRemapPageNode --
 *
 *	Attempts to allocate a memory page on a node in "nodeMask"
 *	as "mpnNode", and change mapping for "ppn" in "world" from
 *	"mpnOld" to "mpnNode".   If successful, makes "mpnNode" an
 *	identical copy of "mpnOld", updates page mapping appropriately,
 *	and deallocates "mpnOld".  This routine may block for at most
 *	"msTimeout" milliseconds.
 *
 * Results:
 *      Returns VMK_OK if page is successfully remapped, otherwise
 *	returns error code.  Sets "mpnNode" to new MPN if successful,
 *	otherwise INVALID_MPN.
 *
 * Side effects:
 *      Updates page mapping data structures for "world".
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
AllocRemapPageNode(World_Handle *world,
                   PPN ppn,
                   uint32 nodeMask,
                   MPN mpnOld,
                   uint32 msTimeout,
                   MPN *mpnNode)
{
   VMK_ReturnStatus status;
   NUMA_Node nodeOld;
   int numNodes;
   MPN mpnNew;

   // default reply value
   *mpnNode = INVALID_MPN;

   // fail if not NUMA system
   numNodes = NUMA_GetNumNodes();
   if (numNodes <= 1) {
      VMLOG(1, world->worldID, "system not NUMA");
      return(VMK_BAD_PARAM);
   }

   // fail if "mpnOld" already on node in "nodeMask"
   nodeOld = NUMA_MPN2NodeNum(mpnOld);
   if ((1 << nodeOld) & nodeMask) {
      VMLOG(1, world->worldID, "mpnOld=0x%x already in nodeMask=0x%x",
            mpnOld, nodeMask);
      return(VMK_BAD_PARAM);
   }

   // attempt to allocate page on a node in nodeMask
   mpnNew = AllocVMPageInt(world, ppn, nodeMask, MM_TYPE_ANY, msTimeout);
   if (mpnNew == INVALID_MPN) {
      return(VMK_NO_MEMORY);
   }

   // invalidate page from alloc cache, remap
   AllocLock(world);
   if (ppn != INVALID_PPN) {
      (void) Alloc_InvalidateCache(world, ppn);
   }
   status = AllocRemapPage(world, ppn, mpnOld, mpnNew);

   if (status == VMK_SHARED) {
      // the source page is COW, so try to reshare it onto a destination node
      AllocFreeVMPage(world, mpnNew);
      status = AllocResharePageNode(world, mpnOld, nodeMask, ppn, mpnNode);
   } else if (status != VMK_OK) {
      // reclaim new page and fail if unable to remap
      AllocFreeVMPage(world, mpnNew);
   } else { // VMK_OK case
      // reclaim old page, succeed
      AllocFreeVMPage(world, mpnOld);
      *mpnNode = mpnNew;
      status = VMK_OK;
   }

   AllocUnlock(world);
   return (status);
}

/*
 *----------------------------------------------------------------------
 *
 * Alloc_RemapBatchPages --
 *
 *      Attempt to process the first "batchLen" page remap requests
 *	contained in "batchMPN".  Modifies the contents of "batchMPN",
 *	updating each request appropriately to reflect the remapped
 *	state; any requests that fail are marked invalid.
 *
 * Results:
 *      Modifies contents of "batchMPN".
 *
 * Side effects:
 *      Updates page mapping data structures for "world".
 *
 *----------------------------------------------------------------------
 */
VMKERNEL_ENTRY
Alloc_RemapBatchPages(DECLARE_2_ARGS(VMK_REMAP_BATCH_PAGES,
                                     MPN, batchMPN,
                                     uint32, batchLen))
{
   Alloc_RemapBatch *batch;
   KSEG_Pair *batchPair;
   World_Handle *world;
   uint32 i;

   PROCESS_2_ARGS(VMK_REMAP_BATCH_PAGES,
                  MPN, batchMPN,
                  uint32, batchLen);

   // sanity checks
   ASSERT(batchLen <= ALLOC_REMAP_BATCH_SIZE);
   ASSERT(AllocIsValidMPN(batchMPN, TRUE));

   // convenient abbrev
   world = MY_VMM_GROUP_LEADER;

   // map batch
   batch = Kseg_MapMPN(batchMPN, &batchPair);

   for (i = 0; i < batchLen; i++) {
      Alloc_RemapState *r = &batch->remap[i];

      VMK_ReturnStatus status;
      MPN mpnNew;

      // skip invalid requests
      if (r->op.valid == 0) {
         continue;
      }

      // invalidate bad requests
      if ((r->ppn == INVALID_PPN) || (r->mpnOld == INVALID_MPN)) {
         r->op.valid = 0;
         continue;
      }

      // Note: currently supports only remapLow and remapNode,
      // giving remapLow precedence.  Future expanded support
      // should include remapColor, and general remap combinations
      // (e.g. remap color and node).
      if (r->op.remapLow) {
         // attempt to remap high-to-low, invalidate on failure
         status = AllocRemapPageLow(world, r->ppn, r->mpnOld, 0, &mpnNew);
         if (status == VMK_OK) {
            r->mpnNew = mpnNew;
         } else {
            r->op.valid = 0;
         }
         if (ALLOC_DEBUG_REMAP_VERBOSE) {
            VMLOG(0, world->worldID,
                  "low: ppn=0x%x, mpnOld=0x%x, mpnNew=0x%x, status=%d",
                  r->ppn, r->mpnOld, mpnNew, status);
         }
      } else if (r->op.remapNode) {
         // attempt to migrate page to specified NUMA node, invalidate on failure
         status = AllocRemapPageNode(world, r->ppn, r->op.nodeMask, r->mpnOld, 0, &mpnNew);
         if (status == VMK_OK) {
            r->mpnNew = mpnNew;
         } else {
            r->op.valid = 0;
         }
         if (ALLOC_DEBUG_REMAP_VERBOSE) {
            VMLOG(0, world->worldID,
                  "node: ppn=0x%x, mpnOld=0x%x, mpnNew=0x%x, status=%d",
                  r->ppn, r->mpnOld, mpnNew, status);
         }
      } else {
         // invalidate unexpected requests
         r->op.valid = 0;
         if (ALLOC_DEBUG_REMAP) {
            VMLOG(0, world->worldID,
                  "unexpected: ppn=0x%x, mpnOld=0x%x", r->ppn, r->mpnOld);
         }
      }
   }

   // release batch
   Kseg_ReleasePtr(batchPair);

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Alloc_RemapBatchPickup --
 *
 *      Pickup any pending page remap requests for current world.
 *
 * Results:
 *	Updates contents of "batchMPN" to contain remap requests.
 *      Sets "batchLen" to the number of requests.
 *
 * Side effects:
 *      Resets pending remap requests for current world.
 *
 *----------------------------------------------------------------------
 */
VMKERNEL_ENTRY
Alloc_RemapBatchPickup(DECLARE_2_ARGS(VMK_REMAP_BATCH_PICKUP,
                                      MPN, batchMPN,
                                      uint32 *, batchLen))
{
   Alloc_RemapBatch *batch;
   KSEG_Pair *batchPair;
   World_Handle *world;
   Alloc_Info *info;
   int i;

   PROCESS_2_ARGS(VMK_REMAP_BATCH_PICKUP,
                  MPN, batchMPN,
                  uint32 *, batchLen);

   // sanity checks
   ASSERT(AllocIsValidMPN(batchMPN, TRUE));

   // convenient abbrevs
   world = MY_VMM_GROUP_LEADER;
   info = Alloc_AllocInfo(world);

   // acquire lock, map batch
   AllocLock(world);
   batch = Kseg_MapMPN(batchMPN, &batchPair);

   // copy pending remap requests
   ASSERT(info->remapLowNext < ALLOC_REMAP_BATCH_SIZE);
   for (i = 0; i < info->remapLowNext; i++) {
      Alloc_RemapState *r = &batch->remap[i];

      // generate general page remap request
      memset(r, 0, sizeof(Alloc_RemapState));
      r->op.valid = 1;
      r->op.remapLow = 1;
      r->ppn = info->remapLow[i];

      info->remapLow[i]= INVALID_PPN;
   }
   *batchLen = info->remapLowNext;
   info->remapLowNext = 0;

   // release batch, lock
   Kseg_ReleasePtr(batchPair);
   AllocUnlock(world);

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Alloc_RequestRemapPageLow --
 *
 *      Issue request to remap "ppn" in "world" into low memory.
 *	The "mpn" parameter is currently used for debugging only.
 *	Caller must hold "world" alloc lock.
 *
 * Results:
 *      Returns TRUE if request successfully issued, otherwise
 *	returns FALSE (e.g. request buffer full).
 *
 * Side effects:
 *      May issue request to remap "ppn" in "world" info low memory.
 *
 *----------------------------------------------------------------------
 */
Bool
Alloc_RequestRemapPageLow(World_Handle *world, PPN ppn, MPN mpn)
{
   Alloc_Info *info = Alloc_AllocInfo(world);

   // sanity check
   ASSERT(AllocIsLocked(world));

   if ((info->remapPickupAction != ACTION_INVALID) &&
       (info->remapLowNext < ALLOC_REMAP_LOW_REQUESTS_MAX)) {
      info->remapLow[info->remapLowNext] = ppn;
      info->remapLowNext++;
      info->remapLowPeak = MAX(info->remapLowPeak, info->remapLowNext);
      info->remapLowTotal++;
      Action_Post(world, info->remapPickupAction);

      // debugging
      if (ALLOC_DEBUG_REMAP_VERBOSE) {
         VMLOG(0, world->worldID, "ppn=0x%x, mpn=0x%x, next=%d, total=%u",
               ppn, mpn, info->remapLowNext, info->remapLowTotal);
      }

      // succeed
      return(TRUE);
   }

   // unable to add request
   return(FALSE);
}


/*
 *----------------------------------------------------------------------
 *
 * AllocCheckAsyncReadStatus --
 *
 *      Checks if the Async Read succeeded or failed, In case of a failure
 *      adds a one shot timer callback to retry the operation.
 *
 *      Note: dbgRetry parameter is only useful for obj builds
 *            where we try to simulate some of these read failures.
 *            In other builds dbgRetry will always be FALSE.
 *
 * Results:
 *      VMK_OK if the Async read was successful
 *      VMK_FAILURE if the Async read failed.
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
AllocCheckAsyncReadStatus(World_Handle *world,
                          Alloc_PageFaultToken *pfToken,
                          Bool dbgRetry)
{
   Async_Token *token = pfToken->token;
   uint32 sleepTime;

   if ((((SCSI_Result *)token->result)->status == 0) && (!dbgRetry)) {
      return(VMK_OK);
   }

   // async read failed try again...
   if (pfToken->nrRetries > CONFIG_OPTION(MEM_SWAP_IO_RETRY)) {
      VmWarn(world->worldID, 
                "Could not read swapped out PPN(0x%x) after "
                "%d retries, killing VM", pfToken->ppn, pfToken->nrRetries);
      World_Panic(world, "Alloc: Could not read swapped page\n");
      AllocPFTokenRelease(pfToken);
      return(VMK_FAILURE);
   }
   pfToken->nrRetries++;
   ASSERT(token == pfToken->token);
   sleepTime = pfToken->sleepTime;
   pfToken->sleepTime = Swap_GetNextSleepTime(sleepTime);
   Timer_Add(0, Alloc_RetrySwapIn, sleepTime, TIMER_ONE_SHOT, pfToken);
   return(VMK_FAILURE);
}
/*
 *----------------------------------------------------------------------
 *
 * AllocCheckpointCallback --
 *      Handles callback from async reads that are generated,
 *      when we read a page from the
 *      swap file on behalf of a COS page fault during a checkpoint/resume
 *      of a VM to a console OS file.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
static void
AllocCheckpointCallback(Async_Token *token)
{
   World_Handle *world;
   Alloc_PageFaultToken *pfToken;
   Bool dbgRetry = FALSE;

   pfToken = token->clientData;
   ASSERT(token == pfToken->token);

   world = World_Find(pfToken->worldID);
   ASSERT(world);
   if (!world) {
      WarnVmNotFound(pfToken->worldID);
      return;
   }
   if (ALLOC_DEBUG_COS_FAULT) {
      static uint32 dbgCptSwapIn = 0;
      if (dbgCptSwapIn % 100 == 0) {
         dbgRetry = TRUE;
      }
      dbgCptSwapIn++;
   }
   if (AllocCheckAsyncReadStatus(world, pfToken, dbgRetry) != VMK_OK) {
      World_Release(world);
      return;
   }

   Swap_DoPageSanityChecks(world, pfToken->slotNr, pfToken->mpn, pfToken->ppn);
   AllocLock(world);
   AllocPFTokenSetStateDone(pfToken);
   AllocUnlock(world);
   World_Release(world);   
}

/*
 *----------------------------------------------------------------------
 *
 * AllocAsyncReadCallback --
 *      Handles callback from async reads that are generated,
 *   when we read a page from the swap file on behalf of a
 *   COS page fault.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
static void
AllocAsyncReadCallback(Async_Token *token)
{
   World_Handle *world;
   Alloc_PageFaultToken *pfToken;
   MemSchedVmmUsage *usage;
   PPN ppn;
   uint32 dirIndex;
   uint32 pageIndex;
   MPN dirMPN;
   Alloc_PageInfo *pageInfo;
   KSEG_Pair *dirPair;
   AllocPFrameState frameState;
   uint32 frameIndex;
   VMK_ReturnStatus status;
   Alloc_PFrame *dir;
   World_ID worldID;
   Bool dbgRetry = FALSE;

   pfToken = token->clientData;
   ASSERT(token == pfToken->token);

   world = World_Find(pfToken->worldID);
   ASSERT(world);
   if (!world) {
      WarnVmNotFound(pfToken->worldID);
      AllocPFTokenRelease(pfToken);
      return;
   }
   worldID = world->worldID;
   usage = MemSched_ClientVmmUsage(world);
   ppn = pfToken->ppn;
   status = Alloc_LookupPPN(world, ppn, &dirIndex, &pageIndex);
   if (status != VMK_OK) {
      VmWarn(worldID, "Failure: Lookup PPN(0x%x) failed", ppn);
      AllocPFTokenRelease(pfToken);
      // release world handle
      World_Release(world);
      return;
   }

   if (ALLOC_DEBUG_COS_FAULT) {
      static uint32 dbgCosSwapIn = 0;
      if (dbgCosSwapIn % 100 == 0) {
         dbgRetry = TRUE;
      }
      dbgCosSwapIn++;
   }

   if (AllocCheckAsyncReadStatus(world, pfToken, dbgRetry) != VMK_OK) {
      World_Release(world);
      return;
   }

   AllocLock(world);
   pageInfo = &Alloc_AllocInfo(world)->vmPages;
   // lookup page frame directory
   dirMPN = pageInfo->pages[dirIndex];
   ASSERT(dirMPN != INVALID_MPN);
   if (dirMPN == INVALID_MPN) {
      AllocPFTokenRelease(pfToken);
      AllocUnlock(world);
      World_Panic(world, "Alloc: Could not find a dirMPN\n");
      World_Release(world);   
      return;
   }

   // map existing page directory
   dir = Kseg_MapMPN(dirMPN, &dirPair);

   // extract flags, index, mpn
   frameState = Alloc_PFrameGetState(&dir[pageIndex]);
   frameIndex = Alloc_PFrameGetIndex(&dir[pageIndex]);

   ASSERT(Alloc_PFrameStateIsSwapIn(frameState));
   ASSERT(frameIndex == pfToken->mpn);

   status = AllocSwapReadComplete(world, &dir[pageIndex], frameIndex,
                                  pfToken->slotNr, pfToken->ppn);
   if (status != VMK_OK) {
      AllocPFTokenRelease(pfToken);
      Kseg_ReleasePtr(dirPair);
      AllocUnlock(world);
      // release world handle
      World_Release(world);
      return ;
   }

   // update usage
   usage->locked++;

   /*
    * AllocPFrameSetRegular will reset the state to REGULAR and
    * set valid = 1, hence we do not need to
    * explicitly reset the ALLOC_PFRAME_SWAP_IN (set in
    * AllocGetSwappedPage) or ALLOC_PFRAME_SWAPPED state.
    */
   AllocPFrameSetRegular(world, &dir[pageIndex], pfToken->mpn);

   AllocPFTokenRelease(pfToken);
   Kseg_ReleasePtr(dirPair);
   AllocUnlock(world);
   // release world handle
   World_Release(world);

   if (ALLOC_DEBUG_COS_FAULT) {
      static uint32 cosThrottle = 0;
      if (cosThrottle % 1000 == 0) {
         VMLOG(0, worldID, "called %u times", cosThrottle);
      }
      cosThrottle++;
   }
}


/*
 * ---------------------------------------------
 * AllocPFrameStateName()
 *   returns a constant string describing a PFrame's state.
 * ---------------------------------------------
 */
static INLINE const char *
AllocPFrameStateName(AllocPFrameState fState)
{
   switch (fState) {
   case ALLOC_PFRAME_REGULAR:
      return "Regular";
   case ALLOC_PFRAME_COW:
      return "COW";
   case ALLOC_PFRAME_COW_HINT:
      return "COW hint";
   case ALLOC_PFRAME_SWAPPED:
      return "Swapped";
   case ALLOC_PFRAME_SWAP_OUT:
      return "Swap_Out";
   case ALLOC_PFRAME_SWAP_IN:
      return "Swap_In";
   case ALLOC_PFRAME_OVERHEAD:
      return "Overhead";
   default:
      NOT_REACHED();
   }
}


/*
 *----------------------------------------------------------------------
 *
 * AllocWorldProcPagesRead --
 *
 *      Prints out a summary of a worlds page allocation table, with
 *      MPN range and distribution by NUMA node.
 *      This is a HIDDEN proc node.
 *
 *      NOTE on LOCKING:  World locking is not used in this function.
 *      This creates the possibility that the page tables could be
 *      destroyed or change state while this function is running. Since this
 *      fn traverses the page tables, it could take a while so it was deemed
 *      too expensive to lock the world.  Since
 *      this function just returns statistics, absolutely preserving state during
 *      the scan doesn't seem like a priority.   World_Find() and World_Release()
 *      however is used to guarantee that the world won't be completely destroyed
 *      while the page traversal happens.
 *
 * Results:
 *      Returns VMK_OK.
 *
 * Side effects:
 *	None.
 *
 *---------------------------------------------------------------------- */
static int
AllocWorldProcPagesRead(Proc_Entry  *entry,
                        char        *buffer,
                        int         *len)
{
   World_ID worldID = (World_ID) entry->private;
   World_Handle *world;
   Alloc_Info *info;
   Alloc_PageInfo *pageInfo;
   uint32 frameIndex;
   AllocPFrameState frameState;
   MPN frameMPN;
   uint32 i, j;
   PPN curPgNum = 0;
   /*
    * STACK USAGE WARNING:
    * The Arrays below use (ALLOC_PFRAME_STATE_MAX) * (NUMA_MAX_NODES + 3) * sizeof(int)
    * bytes in the stack. Currently that works out to 8 * (4+3) * (4) = 224 bytes.
    * If either constant changes we could be overflowing the buffer.
    */
   int pageTotals[ALLOC_PFRAME_STATE_MAX];
   uint32 pageLow[ALLOC_PFRAME_STATE_MAX];
   uint32 pageHigh[ALLOC_PFRAME_STATE_MAX];
   int nodeTotals[ALLOC_PFRAME_STATE_MAX][NUMA_MAX_NODES];
   int lowTotal = 0;
   int highTotal = 0;
   PPN vmStartPPN;
   uint32 vmNumPages;

   world = World_Find(worldID);
   if (!world) {
      WarnVmNotFound(worldID);
      return (VMK_NOT_FOUND);
   }
   info = Alloc_AllocInfo(world);
   pageInfo = &info->vmPages;

   ASSERT(world != NULL);
   *len = 0;

   // Quit if there are no page tables to examine
   if (pageInfo->pages == NULL) {
      return(VMK_OK);
   }

   vmStartPPN   = 0;
   vmNumPages   = pageInfo->numPhysPages;

   // initialize statistics arrays
   for (i=0; i < ALLOC_PFRAME_STATE_MAX; i++) {
      pageTotals[i] = 0;
      pageHigh[i] = 0;
      pageLow[i] = (uint32) -1;
      for (j=0; j < NUMA_MAX_NODES; j++) {
	 nodeTotals[i][j] = 0;
      }
   }

   Proc_Printf(buffer, len, "Machine Page Allocation Summary for world ID=%d\n",
	       world->worldID);

   // for each page frame directory
   for (i = 0; i < pageInfo->numPDirEntries; i++) {
      MPN dirMPN;
      KSEG_Pair *dirPair;
      Alloc_PFrame *dir;

      if (pageInfo->pages[i] == INVALID_MPN) {
	 continue;
      }

      dirMPN = pageInfo->pages[i];
      dir = Kseg_MapMPN(dirMPN, &dirPair);

      // for each page frame
      for (j = 0; j < PAGE_SIZE / sizeof(Alloc_PFrame); j++) {
	 PPN ppn;
	 int node;

	 curPgNum = (i * (PAGE_SIZE / sizeof(Alloc_PFrame))) + j;
	 if (curPgNum >= vmStartPPN && (curPgNum < vmStartPPN + vmNumPages)) {
	    ppn = curPgNum - vmStartPPN;
	 } else {
	    ppn = INVALID_PPN;
	 }

	 // Extract PFrame values
	 frameState = Alloc_PFrameGetState(&dir[j]);
	 frameIndex = Alloc_PFrameGetIndex(&dir[j]);
	 frameMPN   = Alloc_PFrameGetMPN(&dir[j]);

	 // Skip page frames with invalid MPN's
	 if (frameMPN == INVALID_MPN) {
	    continue;
	 }

	 // Tag overhead pages (ppn=INVALID_PPN)
	 if (ppn == INVALID_PPN) {
	    frameState = ALLOC_PFRAME_OVERHEAD;
	 }

	 // Collect MPN Range statistics
	 pageTotals[frameState]++;
	 if (pageLow[frameState] > frameMPN) {
	    pageLow[frameState] = frameMPN;
	 }
	 if (pageHigh[frameState] < frameMPN) {
	    pageHigh[frameState] = frameMPN;
	 }

	 // Collect NUMA node statistics
	 node = NUMA_MPN2NodeNum(frameMPN);
	 if (node >= 0) {
	    ASSERT(node < NUMA_MAX_NODES);
	    nodeTotals[frameState][node]++;
	 }

	 if (IsLowMPN(frameMPN)) {
	    lowTotal++;
	 } else {
	    highTotal++;
	 }
      }  // for each page frame

      // Release page from mapping cache
      Kseg_ReleasePtr(dirPair);

   }    // For each page frame table

   World_Release(world);

   // print statistics
   Proc_Printf(buffer, len, "Type     #Pages/ MB  Low  - MPN # - High  ");
   for (j=0; j < NUMA_GetNumNodes(); j++) {
      Proc_Printf(buffer, len, "Node%1d ", j);
   }
   Proc_Printf(buffer, len, "\n");

   for (i=0; i < ALLOC_PFRAME_STATE_MAX; i++) {
      Proc_Printf(buffer, len, "%8.8s %6d/%4d %08x - %08x  ",
		  AllocPFrameStateName(i), pageTotals[i], PagesToMB(pageTotals[i]),
		  pageLow[i], pageHigh[i]);
      for (j=0; j < NUMA_GetNumNodes(); j++) {
	 if ((nodeTotals[i][j] > 0) && (nodeTotals[i][j] < PAGES_PER_MB)) {
	    /* less than 1MB but more than zero, so print symbol */
	    Proc_Printf(buffer, len, "[  <1]");
	 } else {
	    Proc_Printf(buffer, len, "[%4d]", PagesToMB(nodeTotals[i][j]));
	 }
      }
      Proc_Printf(buffer, len, "\n");
   }

   Proc_Printf(buffer, len, "%d Pages below 4GB, %d Pages above 4GB\n",
	       lowTotal, highTotal);

   return(VMK_OK);
}


/*
 *----------------------------------------------------------------------
 *
 * AllocWorldProcNumaRead --
 *
 *      Prints the number of pages allocated per NUMA node for this world.
 *
 * Results:
 *      Returns VMK_OK.
 *
 * Side effects:
 *	None.
 *
 *---------------------------------------------------------------------- */
static int
AllocWorldProcNumaRead(Proc_Entry *entry,
		       char *buffer,
		       int *len)
{
   World_Handle *world = (World_Handle *) entry->private;
   Alloc_Info *info = Alloc_AllocInfo(world);
   int i;

   *len = 0;

   Proc_Printf(buffer, len, "Node#       Pages/MB\n");
   for (i = 0; i < MemMap_GetNumNodes(); i++) {
      Proc_Printf(buffer, len, "%5d     %7d/%-5d\n", i,
		  Atomic_Read(&info->pagesPerNode[i]),
		  PagesToMB(Atomic_Read(&info->pagesPerNode[i])));
   }


   Proc_Printf(buffer, len, "\nNode#   AnonPages/MB\n");
   for (i = 0; i < MemMap_GetNumNodes(); i++) {
      Proc_Printf(buffer, len, "%5d     %7d/%-5d\n", i,
		  Atomic_Read(&info->anonPagesPerNode[i]),
		  PagesToMB(Atomic_Read(&info->anonPagesPerNode[i])));
   }

   return(VMK_OK);
}


/*
 *--------------------------------------------------------------------
 *
 * Alloc_RetrySwapIn --
 *
 *      Timer callback routine, that initiates a async read from the swap file
 *      to read the PPN specified in the pfToken.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      Causes the ppn to be swapped in.
 *
 *--------------------------------------------------------------------
 */
static void
Alloc_RetrySwapIn(void *data, UNUSED_PARAM(Timer_AbsCycles timestamp))
{
   Alloc_PageFaultToken *pfToken = (Alloc_PageFaultToken *)data;
   World_Handle *world;
   VMK_ReturnStatus status;
   ASSERT(pfToken);
   world = World_Find(pfToken->worldID);
   ASSERT(world);
   if (!world) {
      WarnVmNotFound(pfToken->worldID);
      AllocPFTokenRelease(pfToken);
      return;
   }

   AllocLock(world);
   ASSERT(!(AllocPFTokenIsStateFree(pfToken) || 
            AllocPFTokenIsStateDone(pfToken)));
   if (AllocPFTokenIsStateFree(pfToken) ||
       AllocPFTokenIsStateDone(pfToken)) {
      AllocPFTokenRelease(pfToken);
      VmWarn(world->worldID, "pfToken state is invalid");
      AllocUnlock(world);
      World_Panic(world, "pfToken state is invalid\n");
      World_Release(world);
      return;
   }

   AllocUnlock(world);
   VmWarn(world->worldID, 
             "Failed to read PPN(0x%x) from swap file "
             "retrying the operation, attempt number(%d)",
             pfToken->ppn, pfToken->nrRetries);
   ASSERT(pfToken->token);
   status = Swap_GetSwappedPage(world, pfToken->slotNr,
                                pfToken->mpn, pfToken->token, pfToken->ppn);
   ASSERT(status == VMK_OK);
   // release world handle
   World_Release(world);
}

/*
 *--------------------------------------------------------------------
 *
 * Alloc_MigrateRemoteCheck --
 *
 *      Checks to see if any pages are still remote.
 *
 * Results:
 *      TRUE all pages are local, FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *--------------------------------------------------------------------
 */
Bool 
Alloc_MigrateRemoteCheck(World_Handle *world)
{
   PPN ppn;
   int remoteCount = 0;
   Alloc_PageInfo *pageInfo = &Alloc_AllocInfo(world)->vmPages;

   for (ppn = 0; ppn < pageInfo->numPhysPages; ppn++) {
      KSEG_Pair *dirPair;
      uint32 pageIndex, dirIndex;
      MPN dirMPN;
      VMK_ReturnStatus status;

      status = Alloc_LookupPPN(world, ppn, &dirIndex, &pageIndex);
      ASSERT(status == VMK_OK);
      dirMPN = pageInfo->pages[dirIndex];

      if (dirMPN == INVALID_MPN) {
         if (remoteCount++ <= 25) {
            VmWarn(world->worldID, "Missing dir MPN for ppn %d. Assuming remote?", 
                   ppn);
         }
      } else  {
         Alloc_PFrame *dir = Kseg_MapMPN(dirMPN, &dirPair);
         if (Swap_IsMigPFrame(&dir[pageIndex])) {
            if (remoteCount++ <= 25) {
               VmWarn(world->worldID, "ppn %d is remote", ppn);
            }
         }
         Kseg_ReleasePtr(dirPair);
      }
   }

   return (remoteCount == 0);
}

/*
 *--------------------------------------------------------------------
 *
 * Alloc_CheckSum --
 *
 *      Check sum each page in the range.  The checksums are
 *	squashed to 32 bits.
 *
 * Results:
 *      A VMK_ReturnStatus value
 *
 * Side effects:
 *      *csumMap is filled in with check sums.
 *
 *--------------------------------------------------------------------
 */
VMK_ReturnStatus
Alloc_CheckSum(World_Handle *world, uint32 *csumMap, Bool useCheckpointCode)
{
   PPN ppn;
   Alloc_PageInfo *pageInfo = &Alloc_AllocInfo(world)->vmPages;
   VMK_ReturnStatus status = VMK_OK;

   for (ppn = 0; ppn < pageInfo->numPhysPages; ppn++) {
      MPN mpn;
      uint64 hash;
      KSEG_Pair *mpnPair;
      void *data;

      if (csumMap[ppn] == -1) {
	 continue;
      } 

      if (useCheckpointCode) {
         status = Alloc_MigratePagefault(world, ppn, &mpn);
      } else {
         status = Alloc_PageFault(world, ppn, FALSE, &mpn);
      }

      if (status != VMK_OK) {
         VmWarn(world->worldID, "Alloc_PageFault(%d) failed with status %#x", ppn,
                status);
         return status;
      } 

      data = Kseg_MapMPN(mpn, &mpnPair);

      if (data == NULL) {
         return VMK_NO_RESOURCES;
      } 

      hash = Hash_Page(data);
      csumMap[ppn] = (uint32)(hash ^ (hash >> 32));
      LOG(1, "page 0x%x = 0x%x, hash = 0x%"FMT64"x", ppn, csumMap[ppn], hash);

      Kseg_ReleasePtr(mpnPair);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Alloc_GetMPNContents --
 *
 *      Return the contents of the given mpn.
 *
 * Results:
 *      Return the contents of the given mpn.
 *
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
void
Alloc_GetMPNContents(MPN mpn,
                     char *out)
{
   KSEG_Pair *pair;
   char *data;

   ASSERT(VMK_IsValidMPN(mpn));

   data = Kseg_MapMPN(mpn, &pair);
   CopyToHost(out, data, PAGE_SIZE);
   Kseg_ReleasePtr(pair);
}


/*
 *----------------------------------------------------------------------
 *
 * Alloc_SetMPNContents --
 *
 *      write the contents of the buffer into the given mpn.
 *
 * Results:
 *      none
 *
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
Alloc_SetMPNContents(VMnix_SetMPNContents* args)
{
#ifdef DEBUG_STUB

   KSEG_Pair *pair;
   MPN mpn;
   char *data;

   CopyFromHost(&mpn, &args->mpn, sizeof args->mpn);
   if (!VMK_IsValidMPN(mpn)){
      return VMK_BAD_PARAM;
   }

   data = Kseg_MapMPN(mpn, &pair);
   ASSERT(data);

   ASSERT(sizeof args->buf == PAGE_SIZE);
   CopyFromHost(data, args->buf, PAGE_SIZE);
   Kseg_ReleasePtr(pair);
   return VMK_OK;
#else
   return VMK_NOT_SUPPORTED;
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * Alloc_GetMigratedMPN --
 *
 *      Return a MPN for the destination of a migrated VM.  If the 
 *	ppn isn't migrated, then return VMK_EXISTS.  Otherwise allocate
 *	a new mpn.
 *
 * Results:
 *      VMK_EXISTS if the ppn isn't migrated.
 *	VMK_NO_MEMORY if a new MPN can't be allocated.
 *	VMK_OK otherwise.
 *      
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
Alloc_GetMigratedMPN(World_Handle *world, PPN ppn, MPN *mpn)
{
   AllocPFramePair framePair;
   VMK_ReturnStatus status; 

   *mpn = INVALID_MPN;

   /*
    * Wait for up to 1 minute for memory to become available.
    */
   if (MemSched_MemoryIsLowWait(60000) != VMK_OK) {
      return VMK_NO_MEMORY;
   }

   AllocLock(world);

   status = AllocGetPFrameFromPPN(world, ppn, &framePair);
   if (status == VMK_OK) {
      if (Swap_IsMigPFrame(framePair.pframe)) {
         *mpn = AllocVMPage(world, ppn);
	 if (*mpn == INVALID_MPN) {
	    status = VMK_NO_MEMORY;
	 }
      } else {
	 status = VMK_EXISTS;
      }
      AllocPFrameReleasePair(&framePair);
   }

   AllocUnlock(world);

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * Alloc_FreeMigratedMPN --
 *
 *      Free an MPN that was allocated by Alloc_GetMigratedMPN.
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
Alloc_FreeMigratedMPN(World_Handle *world, MPN mpn)
{
   AllocFreeVMPage(world, mpn);
}

/*
 *----------------------------------------------------------------------
 *
 * Alloc_SetMigratedMPN --
 *
 *      Set the MPN for a migrated PPN.  If the MPN is already set,
 *	then free the MPN.
 *
 * Results:
 *      Result from AllocGetPFrameFromPPN.
 *      
 * Side effects:
 *      The pframe for the ppn may be changed.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Alloc_SetMigratedMPN(World_Handle *world, PPN ppn, MPN mpn)
{
   AllocPFramePair framePair;
   VMK_ReturnStatus status; 

   AllocLock(world);

   status = AllocGetPFrameFromPPN(world, ppn, &framePair);
   if (status == VMK_OK) {
      if (Swap_IsMigPFrame(framePair.pframe)) {
         MemSchedVmmUsage *usage = MemSched_ClientVmmUsage(world);
	 AllocPFrameSetRegular(world, framePair.pframe, mpn);
	 usage->locked++;
      } else {
	 AllocFreeVMPage(world, mpn);
      }
      AllocPFrameReleasePair(&framePair);
   }

   AllocUnlock(world);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Alloc_MigratePagefault --
 *
 *      Return an MPN containing the contents of the guest ppn passed in.
 *      Must be called during checkpointing (or migrating).
 *
 * Results:
 *      Result from AllocPageFault.
 *      
 * Side effects:
 *      A page may get swapped in.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Alloc_MigratePagefault(World_Handle *world, PPN ppn, MPN *mpn)
{
   Bool writeable = FALSE;

   ASSERT(Alloc_AllocInfo(world)->duringCheckpoint);

   /*
    * XXX faster to only release once buffer is full.
    */
   AllocLock(world);
   AllocCheckpointBufRelease(world);
   AllocCheckpointBufSetStartPPN(world, ppn);
   AllocUnlock(world);
   return AllocPageFault(world, ppn, &writeable, mpn, ALLOC_FROM_VMKERNEL, TRUE);
}

/*
 *-----------------------------------------------------------------------------
 *
 * AllocSanityCheckAnonNode --
 *
 *    Simple wrapper to perform sanity checks on the given node, produce
 *    a warning if sanity checks fail and PANIC the VM.
 *
 * Results:
 *    TRUE if sanity checks pass, FALSE on failure
 *
 * Side effects:
 *    none.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE Bool
AllocSanityCheckAnonNode(AllocAnonMPNNode *node,
                         World_Handle *world,
                         Bool matchPrevMPN,
                         MPN prevMPN,
                         Bool matchNextMPN,
                         MPN nextMPN)
{
   World_ID worldID = world->worldID;
   if (!node) {
      VmWarn(worldID, "Failed to access node");
      return FALSE;
   }
   ASSERT(node->tag == MPAGE_TAG_ANON_MPN);
   ASSERT(node->worldID == worldID);
   ASSERT(node->magicNum == ALLOC_ANON_MPAGE_MAGIC_NUM);
   ASSERT(!matchPrevMPN || node->prevMPN == prevMPN);
   ASSERT(!matchNextMPN || node->nextMPN == nextMPN);

   if (node->tag != MPAGE_TAG_ANON_MPN ||
       node->worldID != worldID ||
       node->magicNum != ALLOC_ANON_MPAGE_MAGIC_NUM ||
       (matchPrevMPN && node->prevMPN != prevMPN) ||
       (matchNextMPN && node->nextMPN != nextMPN)) {
      VmWarn(worldID, "Anon mpn list is inconsistent: "
             "worldID = %d, magicNum = 0x%x, "
             "tag = 0x%x, nodePrevMPN = 0x%x, nodeNextMPN = 0x%x, "
             "prevMPN = 0x%x, nextMPN = 0x%x",
             worldID, node->magicNum, node->tag, 
             node->prevMPN, node->nextMPN, prevMPN, nextMPN);
      return FALSE;
   }
   return TRUE;
}

/*
 *-----------------------------------------------------------------------------
 *
 * AllocAddToAnonMPNList --
 *
 *    Adds the given "mpn" to the list of anon mpns.
 *
 *    Callers should hold the Alloc lock
 *
 * Results:
 *    VMK_OK on success, error code on failure
 *
 * Side effects:
 *    none.
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus 
AllocAddToAnonMPNList(World_Handle *world,
                      MPN mpn)
{
   Alloc_Info *info = Alloc_AllocInfo(world);
   KSEG_Pair *pair;
   AllocAnonMPNNode *node;
   MPN nextMPN = info->anonMPNHead;

   ASSERT(AllocIsLocked(world));
   info->anonMPNHead = mpn;

   // update the Anon list node of this mpn
   node = AllocMapAnonMPNNode(mpn, &pair);
   ASSERT(node);
   if (!node) {
      return VMK_FAILURE;
   }
   ASSERT(node->magicNum != ALLOC_ANON_MPAGE_MAGIC_NUM);
   node->tag = MPAGE_TAG_ANON_MPN;
   node->worldID = world->worldID;
   node->magicNum = ALLOC_ANON_MPAGE_MAGIC_NUM;  
   node->prevMPN = INVALID_MPN;
   node->nextMPN = nextMPN;
   AllocUnmapAnonMPNNode(pair);

   if (nextMPN != INVALID_MPN) {
      // update the Anon list node of nextMPN
      node = AllocMapAnonMPNNode(nextMPN, &pair);
      ASSERT(node);
      if (!node) {
         return VMK_FAILURE;
      }
      if (!AllocSanityCheckAnonNode(node, world, TRUE, INVALID_MPN,
                                    FALSE, INVALID_MPN)) {
         AllocUnmapAnonMPNNode(pair);
         return VMK_FAILURE;
      }
      node->prevMPN = mpn;
      AllocUnmapAnonMPNNode(pair);
   }
   return VMK_OK;
}


/*
 *-----------------------------------------------------------------------------
 *
 * AllocRemoveFromAnonMPNList --
 *
 *    Removes the given "mpn" from the list of anon mpns.
 *
 *    Callers should hold the Alloc lock, or is single threaded.
 *
 * Results:
 *    VMK_OK on success, error code on failure
 *
 * Side effects:
 *    none.
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
AllocRemoveFromAnonMPNList(World_Handle *world,
                           MPN mpn)
{
   Alloc_Info *info = Alloc_AllocInfo(world);
   MPN prevMPN;
   MPN nextMPN;
   AllocAnonMPNNode *node;
   KSEG_Pair *pair;

   ASSERT(mpn != INVALID_MPN);
   // get prev and next MPNs
   node = AllocMapAnonMPNNode(mpn, &pair);
   ASSERT(node);
   if (!node) {
      return VMK_FAILURE;
   }
   prevMPN = node->prevMPN;
   nextMPN = node->nextMPN;
   // reset the node values
   node->tag = MPAGE_TAG_INVALID;
   node->magicNum = (uint16)~ALLOC_ANON_MPAGE_MAGIC_NUM;  
   node->worldID = 0;
   node->prevMPN = INVALID_MPN;
   node->nextMPN = INVALID_MPN;
   AllocUnmapAnonMPNNode(pair);

   // Adjust the previous node or the head
   if (prevMPN != INVALID_MPN) {
      node = AllocMapAnonMPNNode(prevMPN, &pair);
      ASSERT(node);
      if (!node) {
         return VMK_FAILURE;
      }
      if (!AllocSanityCheckAnonNode(node, world, FALSE, INVALID_MPN,
                                    TRUE, mpn)) {
         AllocUnmapAnonMPNNode(pair);
         return VMK_FAILURE;
      }

      node->nextMPN = nextMPN;
      AllocUnmapAnonMPNNode(pair);
   } else {
      info->anonMPNHead = nextMPN;
   }

   // Adjust the next node
   if (nextMPN != INVALID_MPN) {
      node = AllocMapAnonMPNNode(nextMPN, &pair);
      ASSERT(node);
      if (!node) {
         return VMK_FAILURE;
      }

      if (!AllocSanityCheckAnonNode(node, world, TRUE, mpn,
                                    FALSE, INVALID_MPN)) {
         AllocUnmapAnonMPNNode(pair);
         return VMK_FAILURE;
      }

      node->prevMPN = prevMPN;
      AllocUnmapAnonMPNNode(pair);
   }
   return VMK_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 * AllocGetNextMPNFromAnonMPNList --
 *
 *    Function to travese the list of anon mpns. If "mpn"
 *    is INVALID_MPN returns the first mpn in the list i.e head. If "mpn" 
 *    is not "INVALID_MPN" returns the next anon mpn in the list after "mpn".
 *
 *    Callers should hold the Alloc lock
 *
 *    NOTE: The list of anon MPNs can change between calls to this
 *          function. It is beyond the scope of this function to 
 *          ensure that the list does not change between calls.
 *          
 *
 * Results:
 *    next anon mpn if one exists or INVALID_MPN
 *
 * Side effects:
 *    none.
 *
 *-----------------------------------------------------------------------------
 */
static MPN
AllocGetNextMPNFromAnonMPNList(World_Handle *world,
                               MPN mpn)
{
   Alloc_Info *info = Alloc_AllocInfo(world);
   AllocAnonMPNNode *node;
   ASSERT(AllocIsLocked(world));
   if (mpn == INVALID_MPN) {
      return info->anonMPNHead;
   } else {
      MPN nextMPN;
      KSEG_Pair *pair;
      node = AllocMapAnonMPNNode(mpn, &pair);
      ASSERT(node);
      if (!node) {
         return INVALID_MPN;
      }
      if (!AllocSanityCheckAnonNode(node, world, FALSE, INVALID_MPN,
                                    FALSE, INVALID_MPN)) {
         AllocUnmapAnonMPNNode(pair);
         return INVALID_MPN;
      }
      nextMPN = node->nextMPN;
      AllocUnmapAnonMPNNode(pair);
      return nextMPN;
   }
}


/*
 *----------------------------------------------------------------
 *
 * Alloc_POSTWorldInit --
 *
 *      Initialize the per world alloc data
 *
 * Results:
 *      VMK_OK
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------
 */
VMK_ReturnStatus
Alloc_POSTWorldInit(World_Handle *world, uint32 numPages)
{
   Alloc_Info *info = Alloc_AllocInfo(world);
   Alloc_PageInfo *pageInfo = &info->vmPages;
   uint32 i;

   // alloc for ksegPOST
   ASSERT(World_IsPOSTWorld(world));

   // initialize lock
   SP_InitLock("allocLock", &info->lock, SP_RANK_ALLOC);
   // rank check
   ASSERT(SP_RANK_ALLOC < SP_RANK_FILEMAP && 
          SP_RANK_ALLOC < SP_RANK_MEMSCHED);

   // initialize anon mpn list head
   info->anonMPNHead = INVALID_MPN;

   pageInfo->numPhysPages = numPages;
   pageInfo->valid = TRUE;

   // initialize alloc table for guest physical memory
   pageInfo->numPDirEntries = PAGE_2_DIRINDEX(numPages) + 1;
   pageInfo->pages = (MPN *) World_Align(world, pageInfo->numPDirEntries * sizeof(MPN),
                                         ALLOC_PDIR_ALIGNMENT);
   ASSERT(pageInfo->pages != NULL);
   for (i = 0; i < pageInfo->numPDirEntries; i++) {
      pageInfo->pages[i] = INVALID_MPN;
   }

   return VMK_OK;
}


/*
 *--------------------------------------------------------------------------
 *
 * Alloc_POSTWorldCleanup --
 *
 *     Undo Alloc_WorldInit.
 *
 * Results:
 *     None
 *
 * Side effects:
 *     See Alloc_Dealloc
 *
 *--------------------------------------------------------------------------
 */
void
Alloc_POSTWorldCleanup(World_Handle *world)
{
   Alloc_Info *info = Alloc_AllocInfo(world);

   ASSERT(World_IsPOSTWorld(world));

   // deallocate machine memory pages
   AllocDeallocInt(world);

   SP_CleanupLock(&info->lock);
}
