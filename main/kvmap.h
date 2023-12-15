/* **********************************************************
 * Copyright 2000 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * kvmap.h --
 *
 *	This is the header file to the kernel virtual
 *      address space manager.
 */

#ifndef _KVMAP_H
#define _KVMAP_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"
#include "tlb.h"
#include "kvmap_dist.h"


VMK_ReturnStatus KVMap_Init(VA startAddr, uint32 length);
void *KVMap_MapMPN(MPN mpn, uint32 flags);
void *KVMap_AllocVA(uint32 numPages);
MPN KVMap_VA2MPN(VA vaddr);

int KVMap_NumEntriesUsed(void);
int KVMap_NumEntriesFree(void);
void KVMap_DumpEntries(void);

#endif
