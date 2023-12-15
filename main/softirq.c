/* **********************************************************
 * Copyright (C) 2004 VMware, Inc. All Rights Reserved -- VMware Confidential.
 * **********************************************************/

/*
 * softirq.c --
 *    Wrappers for softirq.
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "x86.h"
#include "vmkernel.h"
#include "vm_asm.h"
#include "prda.h"
#include "softirq_dist.h"

uint32
Softirq_GetSoftirqFlag(void)
{
   return myPRDA.softirq_pending;
}


void
Softirq_SetSoftirqFlag(uint32 pending)
{
   myPRDA.softirq_pending = pending;
}

uint32
Softirq_GetPCPUNum(void)
{
   return myPRDA.pcpuNum;
}
