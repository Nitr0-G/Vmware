/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * mtrr.h --
 *
 *      MTRRs (Memory Type Range Registers)
 */

#ifndef _MTRR_H_
#define _MTRR_H_

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

extern VMK_ReturnStatus MTRR_Init(PCPU pcpu);

extern Bool MTRR_IsWBCachedMPN(MPN mpn);
extern Bool MTRR_IsUncachedMPN(MPN mpn);

#endif
