/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * heap_dist.h --
 *
 *	vmkernel heap functionality for the outside world. 
 *
 */ 

#ifndef _HEAP_DIST_H_
#define _HEAP_DIST_H_

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMK_MODULE
#define INCLUDE_ALLOW_DISTRIBUTE
#include "includeCheck.h"

/*
 * Once we have some sort of EXPORT_SYMBOL functionality, these will all be
 * prefaced with that.
 */

#define VMK_INVALID_HEAP_ID NULL

typedef struct Heap* vmk_HeapID;

extern vmk_HeapID vmk_HeapCreateModule(char *name, uint32 initial, uint32 max);
extern void vmk_HeapCleanupModule(vmk_HeapID heap);

extern vmk_HeapID vmk_HeapCreateStatic(char *name, void *start, uint32 len);
extern void vmk_HeapCleanupStatic(vmk_HeapID heap); 

extern void vmk_HeapFree(vmk_HeapID heap, void *mem);
extern void *vmk_HeapAlloc(vmk_HeapID heap, uint32 size);
extern void *vmk_HeapAlign(vmk_HeapID heap, uint32 size, uint32 alignment);

extern void *vmk_HeapAllocWithRA(vmk_HeapID heap, uint32 size, void *ra);
extern void *vmk_HeapAlignWithRA(vmk_HeapID heap, uint32 size, uint32 align, void *ra);

#endif // _HEAP_DIST_H_
