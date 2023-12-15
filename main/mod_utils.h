/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * mod.h --
 *
 *      Monitor module support.
 */

#ifndef _MOD_H_
#define _MOD_H_

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "monitorAction.h"

struct VM;

EXTERN void Mod_Init(struct VM *vm);
EXTERN void *Mod_AllocData(uint32 size, uint32 alignment);
EXTERN void Mod_Load(ModuleAction ma);

#endif
