
/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * sharedArea.h --
 *
 *	Shared area between vmm, vmk, and vmx.
 */

#ifndef _SHAREDAREA_H_
#define _SHAREDAREA_H_

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "vm_types.h"
#include "vm_libc.h"
#include "vmnix_if.h"
#include "world.h"
#include "sharedAreaDesc.h"


VMK_ReturnStatus SharedArea_Init(World_Handle *world, World_InitArgs *args);
VMK_ReturnStatus SharedArea_Map(VMnix_MapSharedArea *args);
void* SharedArea_Alloc(World_Handle *world, char *name, uint32 size);
void SharedArea_Cleanup(World_Handle *world);
void *SharedArea_GetBase(World_Handle *world);

#endif
