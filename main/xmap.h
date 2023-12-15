/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * xmap.h --
 *
 *	This is the header file to the XMap
 *      address space manager.
 */

#ifndef _XMAP_H
#define _XMAP_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"
#include "xmap_dist.h"

extern VMK_ReturnStatus XMap_Init(void);
extern void XMap_LateInit(void);

extern MPN XMap_VA2MPN(VA vaddr);

extern VMK_ReturnStatus XMap_Dump(void);

#endif
