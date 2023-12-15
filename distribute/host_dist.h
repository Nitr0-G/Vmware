/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * host_dist.h --
 *
 *	Host specific functions.
 */

#ifndef _HOST_DIST_H
#define _HOST_DIST_H

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_DISTRIBUTE
#define INCLUDE_ALLOW_VMK_MODULE
#include "includeCheck.h"

// XXX  could / should be accessor functions
extern struct World_Handle *hostWorld;

#define HOST_PCPU	0
#endif

