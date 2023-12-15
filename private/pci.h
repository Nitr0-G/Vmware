/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * pci.h --
 *
 *      PCI device support.
 */

#ifndef _PCI_H_
#define _PCI_H_

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"
#include "pci_dist.h"
#include "chipset.h"
#include "vmnix_syscall.h"


EXTERN void PCI_Init(VMnix_Info *vmnixInfo);

EXTERN VMK_ReturnStatus PCI_ChangeDevOwnership(VMnix_DevArgs *hostArgs);
EXTERN VMK_ReturnStatus PCI_ChangeDevOwnershipProbe(VMnix_DevArgs *hostArgs);
EXTERN VMK_ReturnStatus PCI_SetDevName(VMnix_DevArgs *hostArgs);
EXTERN VMK_ReturnStatus PCI_GetDevName(VMnix_DevArgs *hostArgs);
EXTERN VMK_ReturnStatus PCI_CheckDevName(VMnix_DevArgs *hostArgs);
EXTERN VMK_ReturnStatus PCI_ScanDev(VMnix_DevArgs *hostArgs);

#define PCI_SLOTFUNC(slot, func)	((((slot) & 0x1f) << 3)|((func) & 0x07))
#define PCI_SLOT(slotFunc)         	(((slotFunc) >> 3) & 0x1f)
#define PCI_FUNC(slotFunc)		((slotFunc) & 0x07)
#endif
