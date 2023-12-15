/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * kvmap_dist.h --
 *
 *	This is the header file to the kernel virtual
 *      address space manager.
 */

#ifndef _KVMAP_DIST_H
#define _KVMAP_DIST_H

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_DISTRIBUTE
#define INCLUDE_ALLOW_VMK_MODULE
#include "includeCheck.h"

/*
 *  Flags for overriding the default behavior of validate, invalidate and flushing.
 *  The defaults are a cached mapping and wait for all hardware tlbs to be invalidated.
 */

#define TLB_UNCACHED   0x01
#define TLB_LOCALONLY  0x02

typedef struct KVMap_MPNRange {
   MPN startMPN;
   uint32 numMPNs;
} KVMap_MPNRange;


void *KVMap_MapMPNs(uint32 numPages, KVMap_MPNRange *ranges, uint32 numRanges, uint32 flags);
void KVMap_FreePages(void *ptr);

#endif
