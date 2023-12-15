/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * it.h --
 *
 *	Interrupt tracker.
 */

#ifndef _IT_DIST_H
#define _IT_DIST_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_DISTRIBUTE
#define INCLUDE_ALLOW_VMK_MODULE
#include "includeCheck.h"

EXTERN void IT_UnregisterVector(uint32 vector);
EXTERN void IT_RegisterVector(uint32 vector);

#endif
