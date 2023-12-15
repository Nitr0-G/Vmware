/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * bh.h --
 *
 *	Bottom half handlers.
 */


#ifndef _BH_H
#define _BH_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "net.h"
#include "action.h"
#include "vm_asm.h"
#include "bh_dist.h"

void BH_Init(void);
void BH_SetOnWorld(World_Handle *world, uint32 bhNum);
void BH_SetOnPCPU(PCPU pcpu, uint32 bhNum);
void BH_SetGlobal(uint32 bhNum);

#endif
