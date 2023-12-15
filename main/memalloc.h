/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * memalloc.h --
 *
 *	This is the header file for the memory allocator.
 */


#ifndef _MEMALLOC_H
#define _MEMALLOC_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"
#include "memalloc_dist.h"

struct VMnix_Init;

extern void Mem_EarlyInit(struct VMnix_Init *init);
extern void *Mem_AllocEarly(uint32 size, uint32 alignment);
extern void Mem_Init(void);
extern MPN Mem_VA2MPN(VA address);
extern void Mem_SetIOProtection(void);

extern uint32 Heap_Avail(Heap_ID heap);

static INLINE int Mem_Avail(void)
{
   return Heap_Avail(mainHeap);
}
extern void Heap_EnableTimerCheck(Heap_ID heap);


// read-only area management functions
struct VMnix_StartupArgs;
extern void MemRO_EarlyInit(void);
extern void MemRO_Init(struct VMnix_StartupArgs *startupArgs);
extern void *MemRO_Alloc(uint32 size);
extern void MemRO_Free(void *ptr);
extern uint64 MemRO_CalcChecksum(void);
extern uint64 MemRO_GetChecksum(void);
#define MEMRO_WRITABLE TRUE
#define MEMRO_READONLY FALSE
extern void MemRO_ChangeProtection(Bool writable);
extern Bool MemRO_IsWritable(void);

#endif
