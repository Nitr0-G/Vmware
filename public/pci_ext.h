/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * pci_ext.h --
 *
 *	External definitions for the pci module.
 */


#ifndef _PCI_EXT_H
#define _PCI_EXT_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMK_MODULE
#define INCLUDE_ALLOW_DISTRIBUTE
#include "includeCheck.h"


#define PCI_NUM_BUSES   256		// per system
#define PCI_NUM_SLOTS   32		// per bus
#define PCI_NUM_FUNCS   8		// per slot
#define PCI_NUM_PINS    4               // per slot


#define PCI_DEVICE_BUS_ADDRESS      "%03d:%02d.%01d"      // bus,slot,func
#define PCI_DEVICE_VENDOR_SIGNATURE "%04x:%04x %04x:%04x" // ven,dev, subV,subD

#endif
