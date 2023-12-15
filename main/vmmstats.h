/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * vmmstats.h -- 
 *
 *	Allow access to monitor stats via /proc nodes
 *
 */

#ifndef	_VMMSTATS_H
#define	_VMMSTATS_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "vm_basic_types.h"
#include "vmkernel.h"
#include "world.h"

extern VMK_ReturnStatus VMMStats_WorldInit(struct World_Handle *world, World_InitArgs *args);
extern void VMMStats_WorldCleanup(struct World_Handle *world);

#endif
