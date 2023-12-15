/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * apic.h --
 *
 *	This is the header file for apic module.
 */

#ifndef _APIC_H
#define _APIC_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "return_status.h"


EXTERN void APIC_SendIPI(PCPU pcpuNum, uint32 vector);
EXTERN void APIC_SendNMI(PCPU pcpuNum);
EXTERN void APIC_BroadcastIPI(uint32 vector);
EXTERN void APIC_BroadcastNMI(void);
EXTERN PCPU APIC_GetPCPU(void);
EXTERN MA   APIC_GetBaseMA(void);
EXTERN void APIC_SetTimer(uint32 inital, uint32 *currentVal);
EXTERN void APIC_SelfInterrupt(uint32 vector);
EXTERN void APIC_HzEstimate(volatile uint64 *cpuHz, volatile uint64 *busHz);
EXTERN void APIC_PerfCtrSetNMI(void);
EXTERN Bool APIC_PerfCtrMask(void);
EXTERN void APIC_PerfCtrUnmask(void);

#endif
