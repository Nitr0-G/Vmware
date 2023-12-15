/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * kseg_dist.h --
 *
 *	This is the header file for the kseg vmkernel virtual
 *      address space manager.
 */

#ifndef _KSEG_DIST_H
#define _KSEG_DIST_H

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_DISTRIBUTE
#define INCLUDE_ALLOW_VMK_MODULE
#include "includeCheck.h"

typedef struct KSEG_Pair KSEG_Pair;

extern void *Kseg_GetPtrFromMA(MA maddr, uint32 length, KSEG_Pair **pair);
extern void Kseg_ReleasePtr(KSEG_Pair *pair);

#endif
