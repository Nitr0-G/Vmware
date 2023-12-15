/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * acpi_public.h --
 *
 */
#ifndef _ACPI_PUBLIC_H
#define _ACPI_PUBLIC_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

/*
 *-------------------------------------------------------------------------
 *
 * ACPI_CopyAcpiInfo --
 *
 *    Console OS acpi info is copied into the vmkernel space. 
 *
 * Results:
 *    VMK_OK on success, error code on failure
 *
 * Side effects:
 *    none.
 *
 *-------------------------------------------------------------------------
 */
static INLINE VMK_ReturnStatus 
ACPI_CopyAcpiInfo(VMnix_AcpiInfo **vmkAcpi,
             VMnix_AcpiInfo *vmnixAcpi)
{
   uint32 i;

   *vmkAcpi = Mem_Alloc(sizeof(**vmkAcpi));
   if (!*vmkAcpi) {
      return VMK_NO_MEMORY;
   }

   // Copy the Acpi info into the vmkernel allocated space
   CopyFromHost(*vmkAcpi, vmnixAcpi, sizeof(**vmkAcpi));

   for (i = 0; i < VMK_PCI_NUM_BUSES; i++) {
      VMnix_AcpiPCIBus *vmnixBus = (*vmkAcpi)->busInfo.buses[i];
      VMnix_AcpiPCIBus *vmkBus;
      if (!vmnixBus) {
         continue;
      }
      vmkBus = Mem_Alloc(sizeof(*vmkBus));
      if (!vmkBus) {
         return VMK_NO_MEMORY;
      }
      CopyFromHost(vmkBus, vmnixBus, sizeof(*vmkBus));
      (*vmkAcpi)->busInfo.buses[i] = vmkBus;
   }
   return VMK_OK;
}


/*
 *-------------------------------------------------------------------------
 *
 * ACPI_DestroyAcpiInfo --
 *
 *    Free the vmkernel heap memory allocated when creating this acpi info.
 *
 * Results: 
 *    none.
 *
 * Side effects:
 *    none.
 *
 *-------------------------------------------------------------------------
 */
static INLINE void 
ACPI_DestroyAcpiInfo(VMnix_AcpiInfo *vmkAcpi)
{
   if (vmkAcpi) {
      uint32 i;
      for (i = 0; i < VMK_PCI_NUM_BUSES; i++) {
         if (vmkAcpi->busInfo.buses[i]) {
            Mem_Free(vmkAcpi->busInfo.buses[i]);
         }
      }

      Mem_Free(vmkAcpi);
   }
}
#endif
