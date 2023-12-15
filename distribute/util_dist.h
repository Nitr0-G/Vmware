/* **********************************************************
 * Copyright 2002 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * util_dist.h --
 *
 *	Utilities.
 */

#ifndef _UTIL_DIST_H
#define _UTIL_DIST_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMK_MODULE
#define INCLUDE_ALLOW_DISTRIBUTE
#include "includeCheck.h"

#include "scattergather.h"

typedef enum {
   UTIL_COPY_TO_SG,
   UTIL_COPY_FROM_SG
} Util_CopySGDir;

extern Bool Util_CopySGData(void *data, SG_Array *sgArr, Util_CopySGDir dir,
	                    int index, int offset, int length);
extern Bool Util_CopyToLinuxUser(void *dst, const void *src, unsigned long l);
extern Bool Util_CopyFromLinuxUser(void *dst, const void *src, unsigned long l);

extern uint32 Util_FastRand(uint32 seed);
extern uint32 Util_RandSeed(void);

#endif

