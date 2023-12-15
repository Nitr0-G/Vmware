/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * vmkcalls_vmm.h --
 *
 *	Enumeration of vmkernel calls made by the monitor.
 */

#ifndef _VMKCALLS_VMCORE_H
#define _VMKCALLS_VMCORE_H

#define INCLUDE_ALLOW_VMCORE
#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

/*
 * Function numbers for VMkernel calls (from VMM).
 */
enum {
#define VMKCALL(name, _ignore...) name ,
#include "vmkcallTable_vmcore.h"
#undef VMKCALL
};

#endif
