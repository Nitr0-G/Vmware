/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * pci_int.h --
 *
 *      This is the internal header file for pci module.
 */

#ifndef _PCI_INT_H_
#define _PCI_INT_H_

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "pci.h"

/*
 * PCI Vendor IDs
 */
#define PCI_VENDOR_ID_COMPAQ            0x0e11
#define PCI_VENDOR_ID_SYMBIOS           0x1000
#define PCI_VENDOR_ID_ATI               0x1002
#define PCI_VENDOR_ID_DEC               0x1011
#define PCI_VENDOR_ID_IBM               0x1014
#define PCI_VENDOR_ID_AMD               0x1022
#define PCI_VENDOR_ID_DELL              0x1028
#define PCI_VENDOR_ID_BUSLOGIC          0x104b
#define PCI_VENDOR_ID_QLOGIC            0x1077
#define PCI_VENDOR_ID_3COM              0x10b7
#define PCI_VENDOR_ID_NVIDIA            0x10de
#define PCI_VENDOR_ID_EMULEX            0x10df
#define PCI_VENDOR_ID_SERVERWORKS       0x1166
#define PCI_VENDOR_ID_BROADCOM          0x14e4
#define PCI_VENDOR_ID_INTEL             0x8086
#define PCI_VENDOR_ID_ADAPTEC           0x9004
#define PCI_VENDOR_ID_ADAPTEC_2         0x9005
#define PCI_VENDOR_ID_LITEON            0xc001

#endif
