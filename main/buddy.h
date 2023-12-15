/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * buddy_alloc.h --
 *
 *	Interfaces for the buddy allocator.
 */

#ifndef _BUDDY_ALLOC_H
#define _BUDDY_ALLOC_H

#define BUDDY_NO_COLORS          (-1)           
#define BUDDY_MAX_MEMSPACE_NAME  (16)


typedef struct Buddy_AddrRange {
   uint32 start;
   uint32 len;
} Buddy_AddrRange;

typedef struct Buddy_StaticRangeInfo {
   char name[BUDDY_MAX_MEMSPACE_NAME];
   uint32 start;            // start of the address range 
   uint32 len;              // length of the range
   uint32 minSize;          // min buffer size 
   uint32 maxSize;          // max buffer size
   uint32 numColorBits;     // number of bits that determine the color
} Buddy_StaticRangeInfo;

typedef struct Buddy_DynamicRangeInfo {
   // This should be the first element.
   Buddy_StaticRangeInfo rangeInfo;

   uint32 maxLen;           // maximum length to support for hot add
   uint32 minHotAddLenHint; // minimum amount of address length
                            // that may be added using hot add feature,
                            // This is just a *hint* to the buddy so that
                            // it can optimize its internal storage
                            // requirements, the actual amount added 
                            // later on need not be of this precise length. 
} Buddy_DynamicRangeInfo;

typedef struct BuddyMemSpace *Buddy_Handle;

// Functions
void Buddy_Init(void);
void Buddy_LateInit(void);
uint32 Buddy_StaticRangeMemReq(Buddy_StaticRangeInfo *rangeInfo);
VMK_ReturnStatus Buddy_CreateStatic(Buddy_StaticRangeInfo *rangeInfo, 
                                    uint32 memSize, char *mem, 
                                    uint32 numRanges, 
                                    Buddy_AddrRange *addrRange, 
                                    Buddy_Handle *handle);
VMK_ReturnStatus Buddy_CreateDynamic(Buddy_DynamicRangeInfo *dynRange,
                                     uint32 memSize, char *mem, 
                                     uint32 numRanges,
                                     Buddy_AddrRange *addrRange,
                                     Buddy_Handle *handle);
void Buddy_Destroy(Buddy_Handle handle);
uint32 Buddy_DynamicRangeMemReq(Buddy_DynamicRangeInfo *dynRange);
VMK_ReturnStatus Buddy_AllocateColor(Buddy_Handle handle, uint32 size,
                                     uint32 color, World_ID debugWorldID, 
                                     void *debugRA, uint32 *loc);
VMK_ReturnStatus Buddy_Allocate(Buddy_Handle handle, uint32 size, 
                                World_ID debugWorldID, void *debugRA, 
                                uint32 *loc);
uint32 Buddy_Free(Buddy_Handle handle, uint32 loc);
VMK_ReturnStatus Buddy_HotAddMemRequired(Buddy_Handle handle, uint32 start, 
                                         uint32 len, uint32 *memRequired);
VMK_ReturnStatus Buddy_HotAddRange(Buddy_Handle handle, uint32 memSize,
                                   char *mem, uint32 start, uint32 len,
                                   uint32 numRanges, 
                                   Buddy_AddrRange *addrRange);
uint32 Buddy_GetNumFreeBufs(Buddy_Handle handle);
uint32 Buddy_GetNumUsedBufs(Buddy_Handle handle);
uint32 Buddy_GetNumFreeBufsForColor(Buddy_Handle handle, uint32 color);
void Buddy_DumpEntries(Buddy_Handle handle);
uint32 Buddy_GetLocSize(Buddy_Handle handle, uint32 loc);
VMK_ReturnStatus Buddy_AllocRange(Buddy_Handle handle, uint32 *loc, uint32 *size);

#endif
