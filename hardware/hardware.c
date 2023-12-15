/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/



/*
 * hardware.c --
 *
 * 	This module handles hardware-specific issues.
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "vmkernel.h"
#include "hardware.h"
#include "vmnix_if.h"
#include "pci_ext.h"

#define LOGLEVEL_MODULE HARDWARE
#define LOGLEVEL_MODULE_LEN 8
#include "log.h"

/* compile time checks to make sure that the
 * vmkernel and console os are talking the same 
 * language, the VMK_... values are defined
 * in "console-os/include/asm-i386/vmk_hw.h"
 */
#if (VMK_NUM_ISA_IRQS != NUM_ISA_IRQS)
#error "Mismatch between the vmkernel and console os defined value for NUM_ISA_IRQS"
#endif

#if (VMK_PCI_NUM_SLOTS != PCI_NUM_SLOTS)
#error "Mismatch between the vmkernel and console os defined value for PCI_NUM_SLOTS"
#endif

#if (VMK_PCI_NUM_PINS != PCI_NUM_PINS)
#error "Mismatch between the vmkernel and console os defined value for PCI_NUM_PINS"
#endif

#if (VMK_MAX_PCPUS != MAX_PCPUS)
#error "Mismatch between the vmkernel and console os defined value for MAX_PCPUS"
#endif

#if (VMK_MAX_IOAPICS != MAX_IOAPICS)
#error "Mismatch between the vmkernel and console os defined value for MAX_IOAPICS"
#endif

#if (VMK_PCI_NUM_BUSES != PCI_NUM_BUSES)
#error "Mismatch between the vmkernel and console os defined value for PCI_NUM_BUSES"
#endif

#if (VMK_PCI_NUM_FUNCS != PCI_NUM_FUNCS)
#error "Mismatch between the vmkernel and console os defined value for PCI_NUM_FUNCS"
#endif

static Hardware_DMIUUID hardwareDMIUUID;



/*
 *-----------------------------------------------------------------------------
 *
 * Hardware_Init --
 *
 * 	initialize hardware module
 *
 * Results:
 * 	None
 *
 * Side Effects:
 * 	None
 *
 *-----------------------------------------------------------------------------
 */
void
Hardware_Init(VMnix_Init *vmnixInit)
{
   ASSERT(sizeof(hardwareDMIUUID) == sizeof(vmnixInit->savedDMIUUID));
   memcpy(&hardwareDMIUUID, vmnixInit->savedDMIUUID, sizeof(hardwareDMIUUID));
}

/*
 *-----------------------------------------------------------------------------
 *
 * Hardware_GetUUID --
 *
 * 	Return the DMI UUID 
 *
 * Results:
 * 	VMK_OK
 *
 * Side Effects:
 * 	None
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
Hardware_GetUUID(Hardware_DMIUUID *uuid)
{
   *uuid = hardwareDMIUUID;
   return VMK_OK;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Hardware_GetInfo --
 *
 * 	Syscall; return info about hardware
 * 		- DMI UUID
 *
 * Results:
 * 	VMK_OK
 *
 * Side Effects:
 * 	None
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
Hardware_GetInfo(VMnix_HardwareInfoArgs *args,
		 VMnix_HardwareInfoResult *result,
		 unsigned long resultLen)
{
   return Hardware_GetUUID(&result->DMIUUID);

}
