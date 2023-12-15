/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * heap_public.h --
 *
 *	This is the header file for the heap memory allocator.
 */


#ifndef _HEAP_PUBLIC_H
#define _HEAP_PUBLIC_H

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMK_MODULE
#define INCLUDE_ALLOW_DISTRIBUTE
#include "includeCheck.h"

#include "return_status.h"
#include "heap_dist.h"

typedef vmk_HeapID Heap_ID;
typedef VMK_ReturnStatus (*HeapDumpCallback)(void *data, VA start, uint32 len);

#define INVALID_HEAP_ID VMK_INVALID_HEAP_ID 
#define MAX_HEAP_NAME 32

extern Heap_ID Heap_CreateStatic(char *name, void *start, uint32 len);
extern VMK_ReturnStatus Heap_DestroyWithPanic(Heap_ID heap, Bool nonEmptyPanic);
extern VMK_ReturnStatus Heap_Destroy(Heap_ID heap);
extern void Heap_Free(Heap_ID heap, void *mem);
extern void *Heap_AlignWithRA(Heap_ID heap, uint32 size, uint32 alignment, void *ra);
extern void Heap_CheckPoison(Heap_ID heap);

static INLINE void *Heap_Align(Heap_ID heap, uint32 size, uint32 alignment)
{
   return Heap_AlignWithRA(heap, size, alignment, NULL);
}

static INLINE void *Heap_AllocWithRA(Heap_ID heap, uint32 size, void *ra)
{
   return Heap_AlignWithRA(heap, size, sizeof (void*), ra);
}

static INLINE void *Heap_Alloc(Heap_ID heap, uint32 size)
{
   return Heap_AllocWithRA(heap, size, NULL);
}

/* Callback function that a growable "dynamic" heap uses to request more memory */
/* Params: IN - requested size. OUT - region start address, region length */ 
typedef VMK_ReturnStatus (*MemRequestFunc)(uint32,void**,uint32*);

/* Callback function that a growable "dynamic" heap uses to free up memory */
/* Takes address as first arg, size in bytes as second */
typedef VMK_ReturnStatus (*MemFreeFunc)(void*,uint32);

extern Heap_ID Heap_CreateCustom(char *name, uint32 initial,
				 uint32 maximum, MemRequestFunc reqFunc,
				 MemFreeFunc freeFunc);

extern Heap_ID Heap_CreateDynamic(char *name, uint32 initial, uint32 maximum);
extern Heap_ID Heap_CreateDynamicLowMem(char *name, uint32 initial, uint32 maximum);

extern VMK_ReturnStatus Heap_Dump(Heap_ID heap, HeapDumpCallback callback,
				  void *data);


#endif // _HEAP_PUBLIC_H
