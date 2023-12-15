/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * common.h
 *
 *	Prototypes for main/common.S assembly routines, needed to
 * call out from C.
 *
 * TODO: "common" is a useless name.  This is all wacky, uncommon,
 * hand-coded assembly.
 */

#ifndef VMKERNEL_COMMON_H
#define VMKERNEL_COMMON_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

extern NORETURN void CommonIntr(void);
extern NORETURN void CommonRet(VMKExcFrame *regs);
extern NORETURN void CommonRetDebug(void *handler, VMKExcFrame *regs);
extern void CommonTrap(void);
extern NORETURN void StartUserWorld(VMKUserExcFrame *initialRegs, uint32 segSel);

#endif

