/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * xmap_dist.h --
 *
 *	This is the header file for the XMap address space manager.
 */

#ifndef _XMAP_DIST_H
#define _XMAP_DIST_H

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_DISTRIBUTE
#define INCLUDE_ALLOW_VMK_MODULE
#include "includeCheck.h"

typedef struct XMap_MPNRange {
   MPN startMPN;
   int numMPNs;
} XMap_MPNRange;

extern void *XMap_Map(uint32 nPages, XMap_MPNRange *ranges, int numRanges);
extern void XMap_Unmap(uint32 nPages, void *ptr);

#endif
