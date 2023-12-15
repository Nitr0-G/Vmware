/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * heap_dist.c --
 *
 *	vmkernel heap functionality for the outside world.
 *
 */ 

/*
 * Note that all include files should be either public or dist header files,
 * except for vmware.h.
 */

/* vmware header */
#include "vmware.h"

/* dist headers */
#include "heap_dist.h"
#include "memalloc_dist.h"

/* public headers */
#include "heap_public.h"

vmk_HeapID 
vmk_HeapCreateModule(char *name, uint32 initial, uint32 max)
{
   return (vmk_HeapID)Heap_CreateDynamicLowMem(name, initial, max);
}

void 
vmk_HeapCleanupModule(vmk_HeapID heap) 
{
   Heap_DestroyWithPanic(heap, FALSE);
}

vmk_HeapID
vmk_HeapCreateStatic(char *name, void *start, uint32 len)
{
   return Heap_CreateStatic(name, start, len);
}

void
vmk_HeapCleanupStatic(vmk_HeapID heap)
{
   Heap_DestroyWithPanic(heap, FALSE);
}

void 
vmk_HeapFree(vmk_HeapID heap, void *mem) 
{
   Heap_Free(heap, mem);
}

void *
vmk_HeapAlloc(vmk_HeapID heap, uint32 size) 
{
   return Heap_AllocWithRA(heap, size, __builtin_return_address(0));
}

void *
vmk_HeapAlign(vmk_HeapID heap, uint32 size, uint32 alignment)
{
   return Heap_AlignWithRA(heap, size, alignment, __builtin_return_address(0));
}

void *
vmk_HeapAllocWithRA(vmk_HeapID heap, uint32 size, void *ra)
{
   if (ra == NULL) {
      return Heap_AllocWithRA(heap, size, __builtin_return_address(0));
   } else {
      return Heap_AllocWithRA(heap, size, ra);
   }
}

void *
vmk_HeapAlignWithRA(vmk_HeapID heap, uint32 size, uint32 align, void *ra)
{
   if (ra == NULL) {
      return Heap_AlignWithRA(heap, size, align, __builtin_return_address(0));
   } else {
      return Heap_AlignWithRA(heap, size, align, ra);
   }
}

