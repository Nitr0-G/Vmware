/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * smp_int.h --
 *
 *	Internal SMP specific functions.
 */

#ifndef _SMP_INT_H
#define _SMP_INT_H

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_USERLEVEL

#include "includeCheck.h"

#include "smp.h"

EXTERN PCPU SMP_GetPCPUNum(int apicID);
EXTERN int SMP_GetApicID(PCPU pcpuNum);


#endif

