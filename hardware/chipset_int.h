/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * chipset_int.h --
 *
 *	This is the header file for chipset module.
 */


#ifndef _CHIPSET_INT_H
#define _CHIPSET_INT_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "chipset.h"
#include "vmnix_if.h"
#include "vm_basic_types.h"
#include "pci_ext.h"
#include "hardware_public.h"


#define CHIPSET_ELCR_PORT 0x4d0

#define MAX_BUSES	PCI_NUM_BUSES
#define MAX_BUS_IRQS	256

#define IOAPICID_RANGE  16	// may eventually become 256 with newer IOAPICS


typedef struct Chipset_BusIRQInfo {
   Bool present;
   int  ic;
   int  pin;
   int  trigger;
   int  polarity;
} Chipset_BusIRQInfo;

typedef struct Chipset_BusInfo {
   int type;
   Chipset_BusIRQInfo busIRQ[MAX_BUS_IRQS];
} Chipset_BusInfo;

typedef struct Chipset_IOAPICInfo {
   Bool present;
   PA32 physAddr;
   int  num;
} Chipset_IOAPICInfo;

typedef struct Chipset_SysInfo {
   Chipset_BusInfo	*buses[MAX_BUSES];
   Chipset_IOAPICInfo	ioapic[IOAPICID_RANGE];
} Chipset_SysInfo;


typedef struct Chipset_ICFunctions_Internal {
   VMK_ReturnStatus (*init)(ICType hostICType, VMnix_ConfigOptions *vmnixOptions, VMnix_SharedData *sharedData, Chipset_SysInfo *sysInfo);
   Bool (*hookupBusIRQ)(int busType, int busID, int busIRQ, IRQ isaIRQ, Bool *edge, IRQ *cosIRQ, uint32 *vector);
} Chipset_ICFunctions_Internal;


extern ICType Chipset_ICType;
extern Chipset_ICFunctions_Internal *Chipset_ICFuncs_Internal;
extern IRQ Chipset_IrqFromPin[VMK_HW_MAX_ICS][VMK_HW_MAX_PINS_PER_IC];


EXTERN INLINE Bool
Chipset_HookupBusIRQ(int busType, int busID, int busIRQ, IRQ isaIRQ, Bool *edge, IRQ *cosIRQ, uint32 *vector)
{
   return Chipset_ICFuncs_Internal->hookupBusIRQ(busType, busID, busIRQ, isaIRQ, edge, cosIRQ, vector);
}

void Chipset_GetBusIRQInfo(int busType, int busID, int busIRQ, Chipset_BusIRQInfo *busIRQInfo);
int  Chipset_TriggerType(IRQ isaIRQ);

#endif
