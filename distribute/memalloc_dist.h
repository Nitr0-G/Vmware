/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * memalloc_dist.h --
 *
 *	This is the header file for the memory allocator.
 */


#ifndef _MEMALLOC_DIST_H
#define _MEMALLOC_DIST_H

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMK_MODULE
#define INCLUDE_ALLOW_DISTRIBUTE
#include "includeCheck.h"

#include "return_status.h"
#include "heap_public.h"

extern Heap_ID mainHeap;

static INLINE void *Mem_Alloc(uint32 size)
{
   return Heap_Alloc(mainHeap, size);
}
static INLINE void Mem_Free(void *mem)
{
   Heap_Free(mainHeap, mem);
}
static INLINE void *Mem_Align(uint32 size, uint32 alignment)
{
   return Heap_Align(mainHeap, size, alignment);
}

extern VPN Mem_MA2VPN(MA address);

#endif // _MEMALLOC_DIST_H
