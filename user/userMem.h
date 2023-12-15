/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/* 
 * userMem.h - 
 *	UserWorld virtual address space, heap and mmap state and
 *	maintenance
 */

#ifndef VMKERNEL_USER_USERMEM_H
#define VMKERNEL_USER_USERMEM_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "vm_basic_types.h"
#include "vmkernel.h"
#include "linuxAPI.h"
#include "userPTE.h"

struct World_Handle;
struct UserCartelInfo;
struct UserDump_Header;
struct UserObj;
struct UserMemMapInfo;
struct UserDump_DumpData;

/*
 * States of a swap-in/out request.
 *
 * A swap-out request:
 *           |----------------------------------------------|
 *           v                     |                        |
 *   USERMEM_SWAP_NONE --> USERMEM_SWAP_OUT_REQ --> USERMEM_SWAPPING_OUT
 *           ^                                              |
 *           |                                              |
 *           |------------ USERMEM_SWAP_CANCELED <----------|
 *
 * For a swap-in request:
 *           |---------------------|
 *           v                     |
 *   USERMEM_SWAP_NONE --> USERMEM_SWAPPING_IN
 *           ^                     |
 *           |                     v
 *           |------------ USERMEM_SWAP_CANCELED
 *
 */
typedef enum {
   USERMEM_SWAP_NONE,           // swap slot is free
   USERMEM_SWAP_OUT_REQ,        // swap-out requested
   USERMEM_SWAPPING_OUT,        // swap-out in progress
   USERMEM_SWAPPING_IN,         // swap-in in progress
   USERMEM_SWAP_CANCELED,       // swap-in or swap-out canceled
} UserMemSwapState;

/*
 * Each swap slot keeps the state of one swap-in/out request.
 */
typedef struct UserMemSwapReq {
   UserMemSwapState state;
   LPN lpn;
   UserPTE pte;
} UserMemSwapReq ;

/*
 * Maximum number of outstanding swap-in/out requests.
 */
#define USERMEM_NUM_SWAP_REQS          16
#define USERMEM_INVALID_SWAP_REQ       -1

/*
 * Maximum number of pages being swapped in/out.
 */
typedef struct UserMemSwapList {
   int numFreeReqs;
   UserMemSwapReq reqs[USERMEM_NUM_SWAP_REQS];
} UserMemSwapList;

typedef enum {
   MEMMAP_EXEC,
   MEMMAP_NOEXEC,
   MEMMAP_IGNORE,
} UserMemMapExecFlag;

typedef struct UserMem {
   SP_SpinLock	lock;
   
   /*
    * Each cartel has a canonical page root.  Whenever a thread in the
    * cartel has no entry for a needed page table (for a user-mode
    * VA), it gets the page *table* from here.  Any entries added to
    * the page table are thus implicitly added for any other worlds.
    * Exception: each thread has one private page table to map its
    * thread data page.  The Kernel VA tables are all private.
    *
    * Note that if any page table entries are changed or invalidated,
    * all other threads in the cartel will have to flush their TLBs.
    *
    * Note that if you try to drop a page table from a thread, no
    * other threads in the cartel will see that (since the page
    * directories are private).
    *
    * This wastes a minor amount of space (1 page root, plus 4 page
    * directories or 20K) per cartel.  You can think of the duplicates
    * in each world in a cartel as wasted too.  So (N-1)*20K wasted
    * space where N is number of simultaneous threads in the cartel.
    */
   MA		canonicalPageRootMA;

   /* Reference count to page table entries in the cartel */
   uint32       ptRefCount;

   /* Heap and stack state: */
   UserVA	dataStart;	/* VA of first page of heap */
   UserVA	dataEnd;        /* VA of first page after end of heap */

   // pointer to heap mmInfo
   struct UserMemMapInfo *heapInfo;

   // ktext info
   MPN ktextMPN;
   uint32 ktextOffset;

   // mmap state
   List_Links   mmaps;

   MemSchedUser *sched;

   UserMemSwapList swapList;

   // next swap scan address
   LA           swapScanLA;

   // current mmapped reserved memory
   uint32       curReserved;
} UserMem;


typedef struct UserMem_ThreadInfo {
   MPN mpn;     // A per-thread data page (tdata)
   MPN ptMPN;   // A per-thread page table to map tdata
} UserMem_ThreadInfo;


extern void UserMem_Init(void);
extern VMK_ReturnStatus UserMem_CartelInit(struct User_CartelInfo *uci,
                                           struct World_Handle* world);
extern VMK_ReturnStatus UserMem_CartelCleanup(struct User_CartelInfo* uci,
                                              struct World_Handle* world);
extern VMK_ReturnStatus UserMem_ThreadInit(struct User_ThreadInfo *uti,
                                           struct World_Handle* world);
extern VMK_ReturnStatus UserMem_ThreadCleanup(struct User_ThreadInfo *uti,
                                              struct World_Handle* world);

extern UserVA UserMem_GetDataStart(struct World_Handle *world);
extern UserVA UserMem_GetDataEnd(struct World_Handle *world);
extern VMK_ReturnStatus UserMem_SetDataStart(struct World_Handle *world,
                                             UserVA* start);
extern VMK_ReturnStatus UserMem_SetDataEnd(struct World_Handle *world,
                                           UserVA dataEnd);

extern VMK_ReturnStatus UserMem_Map(struct World_Handle *world,
                                    UserVA *addr, uint32 length, uint32 prot,
                                    uint32 flags, LinuxFd fd, uint64 offset);
extern VMK_ReturnStatus UserMem_MapObj(struct World_Handle *world,
                                       UserVA *addr, uint32 length, uint32 prot,
                                       uint32 flags, struct UserObj* obj,
				       uint64 offset, Bool incRefcount);
extern VMK_ReturnStatus UserMem_Unmap(struct World_Handle *world,
                                      UserVA addr, uint32 length);

extern VMK_ReturnStatus UserMem_InitAddrSpace(struct World_Handle *world,
                                              UserVA* userStackEnd);

extern VMK_ReturnStatus UserMem_Protect(struct World_Handle *world,
					UserVA addr, uint32 length,
					uint32 prot);

extern VMK_ReturnStatus UserMem_SetupPhysMemMap(struct World_Handle* world,
                                                PPN startPPN,
                                                uint32 length,
                                                UserVA userOut);

extern VMK_ReturnStatus UserMem_HandleMapFault(struct World_Handle* world, 
                                               LA la, 
                                               VA va, 
                                               Bool isWrite);

extern VMK_ReturnStatus UserMem_LookupMPN(struct World_Handle *world,
                                          VPN vpn,
                                          UserPageType pageType,
                                          MPN *mpn);
extern VMK_ReturnStatus UserMem_DumpMapTypes(struct UserDump_Header *header,
					     struct UserDump_DumpData *dumpData);
extern VMK_ReturnStatus UserMem_DumpMMap(struct UserDump_Header* info,
					 struct UserDump_DumpData *dumpData);
extern VMK_ReturnStatus UserMem_DumpMMapObjects(struct UserDump_Header *header,
						struct UserDump_DumpData
								     *dumpData);

extern Bool UserMem_MarkSwapPage(World_Handle *world, uint32 reqNum, 
                                 Bool writeFailed, uint32 swapFileSlot, 
                                 LPN lpn, MPN mpn);

extern void UserMem_PSharePage(struct World_Handle *world, const VPN vpn);

extern VMK_ReturnStatus UserMem_AddToKText(UserMem* mem, const void* code,
                                           size_t size, UserVA* uva);

extern VMK_ReturnStatus UserMem_Remap(struct World_Handle *world,
                                      UserVA addr,
                                      LinuxSizeT oldLen,
                                      LinuxSizeT newLen,
                                      int flags,
                                      UserVA* newAddr);

extern VMK_ReturnStatus UserMem_MemTestMap(struct World_Handle *world,
                                           UserVA mpnInOut,
                                           UserVA numPagesOut,
                                           UserVA addrOut);

extern uint32 UserMem_SwapOutPages(struct World_Handle *world, uint32 numPages);


static INLINE VMK_ReturnStatus
UserMem_Probe(struct World_Handle *world, VPN vpn, MPN *mpnOut)
{
   return UserMem_LookupMPN(world, vpn, USER_PAGE_NOT_PINNED, mpnOut);
}

#endif /* VMKERNEL_USER_USERMEM_H */
