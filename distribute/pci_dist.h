/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * pci_dist.h --
 *
 *      PCI device support.
 */

#ifndef _PCI_DIST_H_
#define _PCI_DIST_H_

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_DISTRIBUTE
#define INCLUDE_ALLOW_VMK_MODULE
#include "includeCheck.h"

#include "pci_ext.h"
#include "vmnix_if_dist.h"

/*
 * Flags for a device
 */
#define PCI_DEVICE_INTERRUPTIVE		0x0001	// device can interrupt
#define PCI_DEVICE_PCI_BRIDGE		0x0002	// device is a PCI bridge
#define	PCI_DEVICE_IDE			0x0004	// device is an IDE controller
#define PCI_DEVICE_HOST			0x0100	// device is handled by COS
#define PCI_DEVICE_SHARED		0x0200	// device is shared with COS

#define PCI_IRQ_NONE			255
#define PCI_INTLINE_NONE		255
#define PCI_INTPIN_NONE			255


typedef struct PCI_Device {
   // bus address
   uint8	bus;		// bus number
   uint8	slot;		// slot number
   uint8	func;		// function number
   uint8	slotFunc;	// synthetic slot/func number

   //
   uint16	flags;		// device features
   uint8	irq;		// COS irq
   uint8	vector;		// vmkernel interrupt vector
   int		moduleID;	// module handling the device if handled by vmk

   // cache of useful PCI registers
   uint16	vendorID;
   uint16	deviceID;
   uint16	classCode;
   uint16	progIFRevID;
   uint16	subVendorID;
   uint16	subDeviceID;
   uint8	hdrType;
   uint8	intLine;	// PCI intline, usually BIOS-assigned ISA int
   uint8	intPin;		// PCI int pin A,B,C or D (mapped to 0,1,2 or 3)
   uint8	spawnedBus;	// spawned bus number if device is a PCI bridge

   // cross-references
   void 	*linuxDev;	// device as seen by vmklinux

   // handy tags
   char		busAddress[12];      // formated bus,slot,function numbers
   char		vendorSignature[20]; // formated venID,devID, subVenID,subDevID

   // external name, such as vmnic0
   char		name[VMNIX_DEVICE_NAME_LENGTH];
} PCI_Device;



EXTERN void PCI_ReadConfig8(uint32 bus,uint32 devFn,uint32 reg, uint8 *value);
EXTERN void PCI_ReadConfig16(uint32 bus,uint32 devFn,uint32 reg, uint16 *value);
EXTERN void PCI_ReadConfig32(uint32 bus,uint32 devFn,uint32 reg, uint32 *value);
EXTERN void PCI_WriteConfig8(uint32 bus,uint32 devFn,uint32 reg, uint8 value);
EXTERN void PCI_WriteConfig16(uint32 bus,uint32 devFn,uint32 reg, uint16 value);
EXTERN void PCI_WriteConfig32(uint32 bus,uint32 devFn,uint32 reg, uint32 value);

EXTERN PCI_Device *PCI_GetFirstDevice(void);
EXTERN PCI_Device *PCI_GetNextDevice(PCI_Device *dev);

EXTERN const char *PCI_GetDeviceName(uint32 bus, uint32 slotFunc);

typedef void (*PCI_Callback)(PCI_Device *dev, Bool hotplug);

EXTERN void PCI_RegisterCallback(int moduleID, PCI_Callback insert, PCI_Callback remove);
EXTERN void PCI_UnregisterCallback(int moduleID, PCI_Callback insert, PCI_Callback remove);

EXTERN Bool PCI_IsSharedDevice(uint32 bus, uint32 slotFunc);
#endif
