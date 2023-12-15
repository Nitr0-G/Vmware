/* **********************************************************
 * Copyright 2000 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * swap.h: This is the header file for the swap device
 */

#ifndef _SWAP_H
#define _SWAP_H

#include "swap_ext.h"


typedef enum {
   SWAP_WSTATE_INACTIVE,        // World is inactive 
   SWAP_WSTATE_LIST_REQ,        // Swap page list is requested
   SWAP_WSTATE_SWAPPING,        // VM is in the process of swapping out pages
   SWAP_WSTATE_SWAP_ASYNC,      // VM is no longer swapping out pages, but async writes may
                                // be outstanding
   SWAP_WSTATE_SWAP_DONE,       // Requested pages have all been swapped out
   SWAP_WSTATE_ACTION_POSTED,   // Swap action has been posted
} SwapWorldState;

typedef enum {
   SWAP_CPT_FILE_CLOSED = 0,        // Checkpoint file is closed
   SWAP_CPT_FILE_OPEN,              // Checkpoint file is open
   SWAP_CPT_FILE_OPENING,           // Checkpoint file is being opened
} SwapCptFileState;

struct Alloc_PFrame;
struct VMnix_CreateSwapFileArgs;

/* 
 * Structure to hold the *current* list of pages to write to the swap file.
 * Also keeps track of the number of pages *written* to disk and the number
 * of pages *being* written to disk.
 */
typedef struct {
   uint32       getPgListAction;
   uint32      nrAsyncWrites;	// # of async writes allowed
   uint32      nextWriteNdx;
   uint32      nrPagesWritten;	// # of pages actually written to disk
   uint32      nrPages;		// # of valid pages in the pframe array
   uint32      length;		// maximum size of the pframe array
   MPN         swapMPNList[SWAP_PFRAME_MAX_SIZE];
   PPN         swapPPNList[SWAP_PFRAME_MAX_SIZE];
} SwapPgList;

typedef struct Swap_ChkpointFileInfo {
   FS_FileHandleID fileHandle;
   SwapCptFileState state;
   uint32 nrPagesToRead;
   uint32 nrPagesRead;
   uint32 nrVMMemPages;
} Swap_ChkpointFileInfo;

typedef struct Swap_VMStats {
   uint32 numPagesRead;       // cumulative total of number of pages read
   uint32 numPagesWritten;    // cumulative total of number of pages written
   uint32 numCOWPagesSwapped; // cumulative total of number of COW pages written
} Swap_VMStats;

/*
 * per world group swap information
 * contains information like the state of the world the list of
 * pages to be swapped out for that world and swap statistics 
 * for that world
 */
typedef struct Swap_VmmInfo {
   SwapWorldState worldState; // Swap State of the world
   SwapPgList swapPgList;     // List of pages *currently* being swapped out
   SP_SpinLock infoLock;
   uint32     nrPagesToSwap;  // nr pages to swap for this vm

   Bool       freezeVM;       // Is the vm stuck?
   Proc_Entry swapFreezeVM;   // /proc/vmware/vm/<ID>/swapFreezeVM

   uint32 maxAllocDuringSwap;  // max pages used by VMM while a swap request
                               // was pending
   uint32 lastAllocDuringSwap; // pages used by VMM during last swap request
   uint32 curAllocDuringSwap;  // pages used by VMM since current swap request
   Bool   continueSwap;        // Did we ask VMM to continue swapping?

   Swap_VMStats stats;
} Swap_VmmInfo;


#define SWAP_NUM_SLOT_NUM_BITS      (24)
#define SWAP_MAX_NUM_SLOTS_PER_FILE (1 << SWAP_NUM_SLOT_NUM_BITS)
#define SWAP_FILE_MAX_SIZE_MB       (SWAP_MAX_NUM_SLOTS_PER_FILE >> 8)

/*
 * Number of milliseconds to sleep before retrying whenever
 * we fail a file operation on swap files.
 */
#define SWAP_MIN_RETRY_SLEEP_TIME        50
#define SWAP_MAX_RETRY_SLEEP_TIME        1000

/*
 *
 * Utility function to track the number of pages credited to
 * the VMM while a swap request is pending
 *
 */
static INLINE void
Swap_AddCurAllocDuringSwap(Swap_VmmInfo *swapInfo, uint32 numSmallPages)
{
   ASSERT(swapInfo);
   swapInfo->curAllocDuringSwap += numSmallPages;
}

static INLINE Bool
Swap_IsSwapReqPending(const Swap_VmmInfo *swapInfo)
{
   ASSERT(swapInfo);
   return (swapInfo->worldState == SWAP_WSTATE_LIST_REQ ||
           swapInfo->worldState == SWAP_WSTATE_ACTION_POSTED);
}

/*
 * Initial I/O retry sleep interval.
 */
static INLINE uint32
Swap_GetInitSleepTime(void)
{
   return SWAP_MIN_RETRY_SLEEP_TIME;
}

/*
 * Increase swap I/O retry sleep interval.
 */
static INLINE uint32
Swap_GetNextSleepTime(uint32 sleepTime)
{
   if (sleepTime <= SWAP_MAX_RETRY_SLEEP_TIME / 2) {
      return (sleepTime * 2);
   } else {
      return SWAP_MAX_RETRY_SLEEP_TIME;
   }
}


/*
 * operations
 */
struct Async_Token;
struct World_InitArgs;
VMKERNEL_ENTRY Swap_GetNumPagesToSwap(DECLARE_2_ARGS(VMK_SWAP_NUM_PAGES,
                                      uint32 *, numPages, Bool *, tryCOW));
VMKERNEL_ENTRY Swap_SwapOutPages(DECLARE_4_ARGS(VMK_SWAPOUT_PAGES , 
                                 int , nrActualPages ,
                                 BPN *, bpnList,
                                 uint32 *, nrRequestPages,
                                 Bool *, tryCOW));
VMK_ReturnStatus Swap_UWSwapOutPage(struct World_Handle *world, uint32 reqNum,
                                    PPN ppn, MPN mpn, uint32 *swapSlotNr);
VMK_ReturnStatus Swap_GetSwappedPage(struct World_Handle *world, 
                                     uint32 slotNr, MPN newMPN, 
                                     struct Async_Token *token, PPN ppn);
void Swap_FreeFileSlot(struct World_Handle *world, uint32 startSlotNum);
void Swap_UWFreeFileSlot(uint32 startSlotNum);
void Swap_ProcRegister(void);
void Swap_Init(void);
VMK_ReturnStatus Swap_WorldAdd(struct World_Handle *world);
VMK_ReturnStatus Swap_WorldRemove(struct World_Handle *world);
void Swap_SetSwapTarget(struct World_Handle *world, uint32 nrPages);
uint32 Swap_GetSwapTarget(struct World_Handle *world);
uint32 Swap_GetTotalNumSlots(World_ID worldID);
uint32 Swap_GetNumFreeSlots(World_ID worldID);
Bool Swap_IsEnabled(void);
void Swap_DoPageSanityChecks(struct World_Handle *world, uint32 slotNr, 
                                    MPN newMPN, PPN ppn);
VMK_ReturnStatus Swap_SetCptFileInfo(struct World_Handle *world, uint32 nrVMMemPages,
                                VMnix_FilePhysMemIOArgs *args);
Bool Swap_AreCptFilesSame(struct World_Handle *world, VMnix_FilePhysMemIOArgs *args);
void Swap_SetCptPFrame(struct World_Handle *world, Alloc_PFrame *pf, uint64 offset);
Bool Swap_IsCptPFrame(Alloc_PFrame *pf);
void Swap_GetCptStats(struct World_Handle *world, uint32 *nrPagesToRead, 
                      uint32 *nrPagesRead);
void Swap_SetMigPFrame(Alloc_PFrame *pf, PPN ppn);
Bool Swap_IsMigPFrame(Alloc_PFrame *pf);
void Swap_WorldCleanup(struct World_Handle *world);
VMK_ReturnStatus Swap_WorldActionInit(struct World_Handle *world, 
                                      struct World_InitArgs *args);
void Swap_WorldActionCleanup(struct World_Handle *world);
VMK_ReturnStatus Swap_WorldInit(struct World_Handle *world, struct World_InitArgs *args);
void Swap_LogPerWorldStats(void);
void Swap_BlockUntilReadyToSwap(struct World_Handle *inWorld);
VMK_ReturnStatus Swap_ActivateFile(const char *filePath);
VMK_ReturnStatus Swap_DeactivateFile(uint32 fileNum);
VMK_ReturnStatus Swap_GetInfo(VMnix_SwapInfoArgs *args, VMnix_SwapInfoResult *result, 
                              unsigned long resultLen);
void Swap_VmmGroupStatsHeaderFormat(char *buffer, int *len);
void Swap_VmmGroupStatsFormat(struct World_Handle *world, char *buffer, int *len);
VMK_ReturnStatus Swap_UpdateDoSanityChecks(Bool write, Bool valueChanged, int ndx);

#endif
