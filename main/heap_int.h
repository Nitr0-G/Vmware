/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * heap_int.h --
 *
 *	This is the internal usage header file for the heap module
 */


#ifndef _HEAP_INT_H
#define _HEAP_INT_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"
#include "splock.h"

#define SP_RANK_STATIC_HEAPLOCK  (SP_RANK_IRQ_LEAF)
#define SP_RANK_HEAPMGR          (SP_RANK_MEMMAP - 1)
#define SP_RANK_HEAPMGR_HEAP     (SP_RANK_HEAPMGR - 1)
#define SP_RANK_DYNAMIC_HEAPLOCK (SP_RANK_HEAPMGR_HEAP - 1)

extern void Heap_Init(void);
extern void Heap_LateInit(void);

#endif //_HEAP_INT_H
