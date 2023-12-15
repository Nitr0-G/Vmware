/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * world_dist.h --
 *
 *	vmkernel world interface
 */

#ifndef _WORLD_DIST_H
#define _WORLD_DIST_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_DISTRIBUTE
#define INCLUDE_ALLOW_VMK_MODULE
#include "includeCheck.h"
#include "sched_dist.h"

//XXX this is ugly (normally defined in world_ext.h)
#ifdef GPLED_CODE
typedef int32 World_ID;
#endif

typedef struct World_Handle World_Handle;
Bool World_CreateKernelThread(CpuSched_StartFunc fn, void *clientData);
void NORETURN World_Exit(VMK_ReturnStatus status);

World_ID World_GetID(World_Handle *world);

#endif
