/* **********************************************************
 * Copyright (C) 2004 VMware, Inc. All Rights Reserved -- VMware Copyright.
 * **********************************************************/

#ifndef _SOFTIRQ_DIST_H_
#define _SOFTIRQ_DIST_H_

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_DISTRIBUTE
#define INCLUDE_ALLOW_VMK_MODULE
#include "includeCheck.h"

EXTERN uint32 Softirq_GetSoftirqFlag(void);
EXTERN void Softirq_SetSoftirqFlag(uint32);
EXTERN uint32 Softirq_GetPCPUNum(void);

#endif // _SOFTIRQ_DIST_H_
