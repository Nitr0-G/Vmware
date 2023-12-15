/* **********************************************************
 * Copyright 2000 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * alloc.h --
 *	This is the header file for machine memory manager.
 */


#ifndef _ALLOC_H
#define _ALLOC_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "splock.h"
#include "alloc_ext.h"
#include "pshare_ext.h"
#include "return_status.h"
#include "alloc_dist.h"
#include "proc.h"
#include "numa_ext.h"

/*
 * Constants
 */

// p2m lookup flags
#define	ALLOC_READ_ONLY		(0x01)
#define ALLOC_FAST_LOOKUP	(0x02)
#define ALLOC_IO_COPY_HINT	(0x04)

// maximum number of guest main memory pages currently supported.
// i.e. physical memory in pages (mem_size_in_mb * 1024 * 1024 / 4096)
#define ALLOC_MAX_NUM_GUEST_PAGES       (VMMEM_MAX_SIZE_MB * 256) 

// Overhead memory excluding anon memory. i.e. memory that is 
// always mmap'ed in the COS
#define ALLOC_MAX_MAPPED_OVHD_MEM       (1 * 1024 * 1024 * 1024)
#define ALLOC_MAX_NUM_OVHD_PAGES        (ALLOC_MAX_MAPPED_OVHD_MEM / PAGE_SIZE)

// pdir values
#define ALLOC_PDIR_SHIFT	9
#define ALLOC_PDIR_OFFSET_MASK	0x1ff

// AllocPFrame state
typedef enum AllocPFrameState {
   ALLOC_PFRAME_REGULAR = 0,
   ALLOC_PFRAME_COW, 
   ALLOC_PFRAME_COW_HINT, 
   ALLOC_PFRAME_SWAPPED,
   ALLOC_PFRAME_SWAP_OUT,
   ALLOC_PFRAME_SWAP_IN,
   ALLOC_PFRAME_OVERHEAD, /* currently never set, just used for printing
                           * out in AllocWorldProcPagesRead */
   ALLOC_PFRAME_STATE_MAX,
} AllocPFrameState;

typedef enum Alloc_PageFaultSource {
   ALLOC_FROM_INVALID = 0,
   ALLOC_FROM_COS,
   ALLOC_FROM_MONITOR,
   ALLOC_FROM_VMKERNEL,
   ALLOC_FROM_USERWORLD,
} Alloc_PageFaultSource;

/*
 * Data structures to handle page faults from the
 * COS/VMkernel which are executed by doing an async read.
 */
struct Async_Token;
typedef enum {IN_USE, FREE, DONE} Alloc_COSFaultTokenState;
typedef struct Alloc_PageFaultToken {
   World_ID worldID;
   struct Async_Token *token;
   PPN ppn;                // Faulting wpn
   MPN mpn;                // MPN corresponding to the faulting ppn
   Alloc_COSFaultTokenState state; 
   uint32 slotNr;         // Swap file slot from which this page will be read
   uint32 nrRetries;      // Number of retries for the operation
   uint32 sleepTime;      // Current retry sleep time
   Bool cosToken;         // is token used by console os?
} Alloc_PageFaultToken;

/*
 * An Alloc_PFrame is a page frame structure for a world.
 *
 *   For a SWAPPED frame, the index specifies a swap file slot number.
 *   For a SWAP_OUT frame, the index is still the MPN of the page.
 *   For a SWAP_IN frame, the index is the new MPN into which the page is read.
 *   For other frames, the index specifies an MPN.
 *
 *   pinCount is the reference count for the pages used by VMX or Vmkernel
 */
typedef struct Alloc_PFrame {
   uint32 index;
   uint16 pinCount;
   uint8  state;
   uint8  valid: 1;
   uint8  sharedArea: 1;
} Alloc_PFrame;


/*
 * Types
 */

typedef struct {
   MPN  mpn;		// page contents
   Bool inUse;		// page in use?
} Alloc_CheckpointBufPage;

typedef struct {
   Alloc_CheckpointBufPage page[ALLOC_CHECKPOINT_BUF_SIZE];
   int nextPage;
   Bool allocated;
   PPN startPPN;
} Alloc_CheckpointBuf;

typedef struct {
   // mutual exclusion
   SP_SpinLock          lock;

   // address space sizes and bounds
   Alloc_PageInfo	vmPages;

   // cache of PPN->MPN mappings
   Alloc_P2M		p2mCache[ALLOC_P_2_M_CACHE_SIZE];

   // pending PPN->MPN updates for monitor
   MPN                  p2mUpdateBuffer[PSHARE_P2M_BUFFER_MPNS_MAX];
   uint32               numP2MSlots;
   uint32               p2mFill;
   uint32               p2mDrain;
   uint32               p2mUpdateTotal;
   uint32               p2mUpdateCur;
   uint32               p2mUpdatePeak;
   uint32		p2mUpdateAction;

   // pending hint updates for monitor
   PShare_HintUpdate	hintUpdate[PSHARE_HINT_UPDATES_MAX];
   int                  hintUpdateNext;
   Bool			hintUpdateOverflow;
   int			hintUpdatePeak;
   uint32		hintUpdateAction;
   uint32		hintUpdateTotal;

   // pending page remap requests for monitor
   PPN			remapLow[ALLOC_REMAP_LOW_REQUESTS_MAX];
   int			remapLowNext;
   int			remapLowPeak;
   uint32		remapLowTotal;
   uint32		remapPickupAction;

   // checkpointing state
   Bool			startingCheckpoint; // TRUE anytime during ckpting
   Bool			duringCheckpoint;   // TRUE during SAVE phase of ckpt
   MPN			dummyMPN;

   // pointer to the first MPN in the list of anon mpn 
   MPN                  anonMPNHead;

   // stress console OS breaking COW sharing
   PPN                  cosNextStressPPN;
   Bool                 cosStressInProgress;

   // checkpoint state
   Alloc_CheckpointBuf	checkpointBuf;
   Bool                 cptSharesDonated;
   uint32               maxCptPagesToRead;
   uint32               cptPagesRead;
   uint32               cptInvalidOvhdPages;

   // Async page fault IO token 
   Alloc_PageFaultToken cosToken;

   // ProcFS entries for dumping allocation info
   Proc_Entry           procDir;           // /proc/vmware/vm/*/alloc
   Proc_Entry           procPages;         // /proc/vmware/vm/*/alloc/pages
   Proc_Entry           procNuma;          // /proc/vmware/vm/*/alloc/numa

   // Per-NUMA-node page statistics
   // OPT: we may always hold the allocLock when we update these,
   // so they wouldn't need to be atomic
   Atomic_uint32        pagesPerNode[NUMA_MAX_NODES];
   Atomic_uint32        anonPagesPerNode[NUMA_MAX_NODES];

   // throttle count for warnings on deallocating frame with non 0 pinCount
   uint8                throttlePinCountWarnings;
} Alloc_Info;

struct World_Handle;
struct VMnix_FilePhysMemIOArgs;
struct World_InitArgs;
struct VMnix_SetMPNContents;

typedef struct AllocAnonMPNStore {
   uint32 numMPNs;
   MPN    storeMPN;
   MPN     startMPN;
   MPN     endMPN;
   uint32 initialIndex;
} AllocAnonMPNStore;


/*
 * Inline Alloc_PFrame operations.
 */

static INLINE void
Alloc_PFrameSetValid(Alloc_PFrame *f)
{
   f->valid = 1;
}

static INLINE void
Alloc_PFrameSetInvalid(Alloc_PFrame *f)
{
   f->valid = 0;
}

static INLINE Bool
Alloc_PFrameIsValid(Alloc_PFrame *f)
{
   return(f->valid == 1);
}


// set "flags" associated with frame "f" 
static INLINE void
Alloc_PFrameSetState(Alloc_PFrame *f, AllocPFrameState state)
{
   f->state = state;
}


// get flags associated with frame "f"
static INLINE AllocPFrameState
Alloc_PFrameGetState(Alloc_PFrame *f)
{
   return(f->state);
}

static INLINE Bool
Alloc_PFrameIsRegular(Alloc_PFrame *f)
{
   return(f->state == ALLOC_PFRAME_REGULAR);
}

static INLINE Bool
Alloc_PFrameStateIsRegular(AllocPFrameState s)
{
   return(s == ALLOC_PFRAME_REGULAR);
}

static INLINE Bool
Alloc_PFrameStateIsSwapped(AllocPFrameState s)
{
   return (s == ALLOC_PFRAME_SWAPPED);
}

static INLINE Bool
Alloc_PFrameStateIsSwapOut(AllocPFrameState s)
{
   return (s == ALLOC_PFRAME_SWAP_OUT);
}

static INLINE Bool
Alloc_PFrameStateIsSwapIn(AllocPFrameState s)
{
   return (s == ALLOC_PFRAME_SWAP_IN);
}

static INLINE Bool
Alloc_PFrameStateIsSwap(AllocPFrameState s)
{
   return (Alloc_PFrameStateIsSwapped(s) ||
           Alloc_PFrameStateIsSwapOut(s) ||
           Alloc_PFrameStateIsSwapIn(s));
}

static INLINE Bool
Alloc_PFrameStateIsCOW(AllocPFrameState s)
{
   return(s == ALLOC_PFRAME_COW);
}

static INLINE Bool
Alloc_PFrameStateIsCOWHint(AllocPFrameState s)
{
   return(s == ALLOC_PFRAME_COW_HINT);
}

// set "index" associated with frame "f"
static INLINE void
Alloc_PFrameSetIndex(Alloc_PFrame *f, uint32 index)
{
   f->index = index;
}

// get index associated with frame "f"
static INLINE uint32
Alloc_PFrameGetIndex(Alloc_PFrame *f)
{
   return(f->index);
}

// tracks shared area pages for COS only
// XXX delete this along with COS vmx
static INLINE Bool
Alloc_PFrameIsSharedArea(Alloc_PFrame *f)
{
   return f->sharedArea;
}

static INLINE void
Alloc_PFrameSetSharedArea(Alloc_PFrame *f)
{
   f->sharedArea = TRUE;
}

// get MPN associated with "f"
static INLINE MPN
Alloc_PFrameGetMPN(Alloc_PFrame *f)
{
   if (Alloc_PFrameIsValid(f)) {
      // f->index is an MPN, for some types 
      if (Alloc_PFrameIsRegular(f)
          || Alloc_PFrameStateIsCOW(f->state)
          || Alloc_PFrameStateIsCOWHint(f->state)) {
         return(f->index);
      }
   }

   return(INVALID_MPN);
}

static INLINE uint16
Alloc_PFrameGetPinCount(Alloc_PFrame *f)
{
   // we can have valid pinCount even if the the frame is INVALID
   return (f->pinCount);
}

static INLINE void
Alloc_PFrameSetPinCount(Alloc_PFrame *f, uint16 count)
{
   f->pinCount = count;
}

// returns TRUE iff checkpoint in progress 
static INLINE Bool
Alloc_IsCheckpointing(Alloc_Info *info)
{
   return(info->duringCheckpoint || info->startingCheckpoint);
}


/*
 * Operations
 */

extern VMK_ReturnStatus Alloc_WorldInit(struct World_Handle *world, 
                                        struct World_InitArgs *args);
extern void Alloc_WorldCleanup(struct World_Handle *world);
extern VMK_ReturnStatus Alloc_POSTWorldInit(struct World_Handle *world, uint32 numPages);
extern void Alloc_POSTWorldCleanup(struct World_Handle *world);

extern VMK_ReturnStatus Alloc_OverheadMemMap(World_ID worldID, VA start);
extern VMK_ReturnStatus Alloc_COSPhysPageFault(World_ID worldID, PPN ppn, MPN *mpn);
extern VMK_ReturnStatus Alloc_OvhdPageFault(World_ID worldID, VPN vpn, MPN *mpn);
extern VMK_ReturnStatus Alloc_MapSharedAreaPage(struct World_Handle *world, VPN userVPN, MPN mpn);
extern VMK_ReturnStatus Alloc_UserWorldPhysPageFault(World_ID worldID, PPN ppn, MPN *mpn);
extern VMK_ReturnStatus AllocPhysToMachineInt(struct World_Handle *world,
                                              PA      paddr,
                                              uint32  length, 
                                              uint32  flags,
                                              Bool    canBlock,
                                              Alloc_Result *result);
extern void Alloc_Init(void);
extern VMK_ReturnStatus Alloc_LookupMPN(World_ID worldID,
					VPN userVPN,
					MPN *outMPN);
extern VMK_ReturnStatus Alloc_GetNextAnonPage(World_ID worldID, 
                                              MPN inMPN, 
                                              MPN *outMPN);
extern VMK_ReturnStatus Alloc_MarkCheckpoint(World_ID worldID, Bool wakeup, Bool start);
extern void Alloc_CheckpointCleanup(struct World_Handle *world);

extern VMK_ReturnStatus Alloc_PhysMemIO(struct VMnix_FilePhysMemIOArgs *args);
extern VMK_ReturnStatus Alloc_SetMMapLast(World_ID worldID, uint32 endMapOffset);

extern void Alloc_Lock(struct World_Handle *);
extern void Alloc_Unlock(struct World_Handle *);
extern VMK_ReturnStatus Alloc_LookupPPN(struct World_Handle *world,
                                        PPN ppn, 
                                        uint32 *dirIndex,
                                        uint32 *pageIndex);
extern MPN Alloc_MapPageDir(struct World_Handle *world, MPN *dirEntry);
extern Bool Alloc_IsCached(struct World_Handle *world, PPN ppn);

extern Bool Alloc_InvalidateCache(struct World_Handle *world, PPN ppn);

extern VMK_ReturnStatus Alloc_PhysMemMap(World_ID worldID, PPN ppn, uint32 len);
extern VMK_ReturnStatus Alloc_PhysMemUnmap(World_ID worldID, PPN ppn, uint32 len);
extern VMK_ReturnStatus Alloc_GetPhysMemRange(struct World_Handle *world,
                                              PPN startPPN,
                                              uint32 numPages,
                                              Bool writeable,
                                              Bool canBlock,
                                              MPN *mpnList);
extern VMK_ReturnStatus Alloc_ReleasePhysMemRange(struct World_Handle *world,
                                                  PPN startPPN, uint32 numPages);

extern Bool Alloc_RequestRemapPageLow(struct World_Handle *world, PPN ppn, MPN mpn);

extern Bool Alloc_MarkSwapPage(struct World_Handle *world, Bool writeFailed,
                               uint32 index, PPN ppn, MPN mpn);

extern VMK_ReturnStatus Alloc_PageIsRemote(struct World_Handle *world, PPN ppn);
extern Bool Alloc_MigrateRemoteCheck(struct World_Handle *world);
extern VMK_ReturnStatus Alloc_CheckSum(struct World_Handle *world, uint32 *csumMap, 
                                       Bool useCheckpointCode);
extern void Alloc_LateMemInit(uint32 MPNSeg);
extern void Alloc_PFrameSetRegular(struct World_Handle *world, PPN ppn, 
                                   Alloc_PFrame *f, MPN mpn);
extern void Alloc_GetMPNContents(MPN mpn, char *out);
VMK_ReturnStatus Alloc_SetMPNContents(struct VMnix_SetMPNContents* args);
extern VMK_ReturnStatus Alloc_PageFault(struct World_Handle *world,
                                        PPN ppn,
                                        Bool writeable,
                                        MPN *mpn);
extern VMK_ReturnStatus Alloc_PageFaultWrite(struct World_Handle *world,
                                             PPN ppn, 
                                             MPN *allocMPN,
                                             Alloc_PageFaultSource source);

extern BPN Alloc_PPNToBPN(struct World_Handle *world, PPN ppn);
extern PPN Alloc_BPNToMainMemPPN(struct World_Handle *world, BPN bpn);
extern Bool Alloc_IsMainMemBPN(struct World_Handle *world, BPN bpn);

extern VMK_ReturnStatus Alloc_TouchPages(struct World_Handle *world, 
                                         uint8 *changeMap, uint32 changeMapLength);
extern VMK_ReturnStatus Alloc_GetMigratedMPN(struct World_Handle *world, 
					     PPN ppn, MPN *mpn);
extern void Alloc_FreeMigratedMPN(struct World_Handle *world, MPN mpn);
extern VMK_ReturnStatus Alloc_SetMigratedMPN(struct World_Handle *world, 
				             PPN ppn, MPN mpn);

extern VMK_ReturnStatus Alloc_MigratePagefault(struct World_Handle *world, PPN ppn, MPN *mpn);
/*
 * VMK Entry Points
 */

extern VMKERNEL_ENTRY
Alloc_AnonPage(DECLARE_2_ARGS(VMK_ALLOC_ANON_PAGE,
                              Bool, lowMem,
                              MPN *, mpn));

extern VMKERNEL_ENTRY
Alloc_ModeLockPage(DECLARE_3_ARGS(VMK_MODE_LOCK_PAGE,
                                  PPN, ppn, 
                                  Bool *, writeable,
                                  MPN *, mpn));
extern VMKERNEL_ENTRY
Alloc_ReleaseAnonPage(DECLARE_1_ARG(VMK_ALLOC_RELEASE_ANON_PAGE,
                                     MPN, mpn));
extern VMKERNEL_ENTRY 
Alloc_BalloonReleasePages(DECLARE_2_ARGS(VMK_BALLOON_REL_PAGES,
                                         BPN *, bpnList,
                                         uint32, numPages));
extern VMKERNEL_ENTRY 
Alloc_CanBalloonPage(DECLARE_1_ARG(VMK_CAN_BALLOON_PAGE, BPN, bpn));

extern VMKERNEL_ENTRY
Alloc_COWSharePages(DECLARE_2_ARGS(VMK_COW_SHARE_PAGES,
                                   uint32, numPages, 
                                   MPN, pshareMPN));
extern VMKERNEL_ENTRY
Alloc_COWCopyPage(DECLARE_3_ARGS(VMK_COW_COPY_PAGE,
                                 BPN, bpn,
                                 MPN, mpn,
                                 MPN *, mpnCopy));

extern VMKERNEL_ENTRY
Alloc_COWP2MUpdateGet(DECLARE_1_ARG(VMK_COW_P2M_UPDATES, BPN *, bpn));

extern VMKERNEL_ENTRY
Alloc_COWP2MUpdateDone(DECLARE_1_ARG(VMK_COW_P2M_UPDATE_DONE, BPN, bpn));

extern VMKERNEL_ENTRY
Alloc_COWGetHintUpdates(DECLARE_2_ARGS(VMK_COW_HINT_UPDATES,
                                       int *, nUpdates,
                                       PShare_HintUpdate *, updates));

extern VMKERNEL_ENTRY
Alloc_COWRemoveHint(DECLARE_2_ARGS(VMK_COW_REMOVE_HINT,
                                   BPN, bpn,
                                   MPN, mpn));
extern VMKERNEL_ENTRY
Alloc_COWCheckPages(DECLARE_2_ARGS(VMK_COW_CHECK_PAGES,
                                   uint32, numPages,
                                   PShare_COWCheckInfo *, check));

extern VMKERNEL_ENTRY
Alloc_RemapBatchPages(DECLARE_2_ARGS(VMK_REMAP_BATCH_PAGES,
                                     MPN, batchMPN,
                                     uint32, batchLen));

extern VMKERNEL_ENTRY
Alloc_RemapBatchPickup(DECLARE_2_ARGS(VMK_REMAP_BATCH_PICKUP,
                                      MPN, batchMPN,
                                      uint32 *, batchLen));

extern VMKERNEL_ENTRY
Alloc_GetAnonReserved(DECLARE_3_ARGS(VMK_GET_ANON_MEM_RESERVED,
                                     uint32 *, nReservedPages,
                                     uint32 *, nUsedPages,
                                     uint32 *, nAvailPages));

extern VMKERNEL_ENTRY
Alloc_UpdateAnonReserved(DECLARE_1_ARG(VMK_UPDATE_ANON_MEM_RESERVED,
                                       int32, pageDelta));

extern VMK_ReturnStatus 
Alloc_KernelAnonPage(struct World_Handle *world, Bool lowMem,
                     MPN *mpnOut);
extern VMK_ReturnStatus 
Alloc_ReleaseKernelAnonPage(struct World_Handle *world, MPN mpn);
extern VMKERNEL_ENTRY
Alloc_LookupMPNFromWorld(DECLARE_2_ARGS(VMK_LOOKUP_MPN,
                                        VPN, userVPN,
                                        MPN *, mpn));

#endif
