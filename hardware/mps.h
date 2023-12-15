/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * mps.h --
 *
 *	This is the header file for MPS module.
 */


#ifndef _MPS_H
#define _MPS_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"
#include "vmnix_if.h"
#include "chipset.h"

#define MPS_PCI_BUSIRQ(slot, pin)       (((slot)<<2)|(pin))
#define MPS_PCI_SLOT(busIRQ)            ((busIRQ)>>2)
#define MPS_PCI_PIN(busIRQ)             ((busIRQ)&3)

typedef enum MPS_Signatures {
   UNRESOLVED = 0,
   P3_IOAPIC_0X11,
   P3_IOAPIC_0X13,
   IBM_X440,
   IBM_RELENTLESS,
} MPS_Signatures;

extern MPS_Signatures MPS_Signature;

extern Bool MPS_ParseChipset(VMnix_SavedMPS *mps, Chipset_SysInfo *sysInfo);


static INLINE uint32
MPS_BusIRQ2Slot(int type,
                int busIRQ)
{
   return (type == VMK_HW_BUSTYPE_PCI) ? MPS_PCI_SLOT(busIRQ) : busIRQ;
}

static INLINE char
MPS_BusIRQ2Pin(int type,
               int busIRQ)
{
   return (type == VMK_HW_BUSTYPE_PCI) ? 'A'+ MPS_PCI_PIN(busIRQ): ' ';
}

#endif
