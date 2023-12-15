/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * heapMgr.h --
 *
 *	This is the header file for the heap manager.
 */


#ifndef _HEAPMGR_H
#define _HEAPMGR_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "return_status.h"

extern VMK_ReturnStatus 
HeapMgr_RequestAnyMem(uint32 size, void **startAddr, uint32 *regionLength);

extern VMK_ReturnStatus
HeapMgr_RequestLowMem(uint32 size, void **startAddr, uint32 *regionLength);

extern VMK_ReturnStatus HeapMgr_FreeAnyMem(void *addr, uint32 size);
extern VMK_ReturnStatus HeapMgr_FreeLowMem(void *addr, uint32 size);

extern void HeapMgr_Init(void);

#endif // _HEAPMGR_H

