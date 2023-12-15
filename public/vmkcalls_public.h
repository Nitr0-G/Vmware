/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * vmkcalls_public.h --
 *
 *	Enumeration of non-monitor vmkernel calls.  The space of calls
 *      is partitioned at VMK_DEV_MIN_FUNCION_ID.  Indices below this
 *      are monitor vmkcalls and indices above this are device vmkcalls.
 */

#ifndef _VMKCALLS_H_
#define _VMKCALLS_H_

#define INCLUDE_ALLOW_VMMEXT
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMCORE
#include "includeCheck.h"

// this needs to be equal to VMK_VMM_MAX_FUNCTION_ID in vmkcallTable_vmcore.h
#define VMK_EXT_MIN_FUNCTION_ID 60

/*
 * Function numbers for VMkernel calls (from VMM)
 */

enum {
#define VMKCALL(name, _ignore...) name ,
   VMK_EXT_START = VMK_EXT_MIN_FUNCTION_ID,
#include "vmkcallTable_public.h"
#undef VMKCALL
};

#endif
