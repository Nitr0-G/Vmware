/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * acpi.c --
 *
 *	This module handles the ACPI information
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "vmkernel_dist.h"
#include "vmkernel.h"
#include "chipset_int.h"
#include "chipset.h"
#include "memalloc_dist.h"
#include "mps.h"

#define LOGLEVEL_MODULE ACPI
#include "log.h"

/*
 *----------------------------------------------------------------------
 *
 * ACPIParseIOAPIC  --
 *
 *    Collect the ioapic information from the ACPI tables
 *
 *
 * Results:
 *    TRUE on success, FALSE on failure
 *
 * Side effects:
 *    none.
 *
 *----------------------------------------------------------------------
 */
static Bool
ACPIParseIOAPIC(Chipset_IOAPICInfo *chipsetIOAPIC,
                VMnix_AcpiIOAPIC *acpiIOAPIC)
{
   uint32 numIOAPICS = 0;
   uint32 i;
   // Initialize the chipset IOAPICs
   for (i = 0; i < IOAPICID_RANGE; i++) {
      chipsetIOAPIC[i].present = FALSE;
   }

   // Go through the acpi IOAPICs and copy them into 
   // the chipset IOAPICs
   for (i = 0; i < VMK_HW_MAX_ICS; i++) {
      if (!acpiIOAPIC[i].present) {
         continue;
      }
      if (acpiIOAPIC[i].id >= IOAPICID_RANGE) {
         Warning("acpi IOAPIC id %d, is greater than %d",
                 acpiIOAPIC[i].id , IOAPICID_RANGE);
         return FALSE;
      }
      chipsetIOAPIC[acpiIOAPIC[i].id].present = TRUE;
      chipsetIOAPIC[acpiIOAPIC[i].id].physAddr = acpiIOAPIC[i].physAddr;
      chipsetIOAPIC[acpiIOAPIC[i].id].num = numIOAPICS++;
   }
   return TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * ACPISetChipsetBusIRQ  --
 *
 *    Set the bus irq values for the chipset.
 *
 *
 * Results:
 *    none
 *
 * Side effects:
 *    none.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
ACPISetChipsetBusIRQ(uint32 busIRQ,
                     Chipset_SysInfo *chipsetInfo,
                     Chipset_BusInfo *chipsetBus, 
                     VMnix_AcpiDevInt *devInt,
                     VMnix_AcpiIOAPIC *acpiIOAPICs)
{
   VMnix_AcpiINTIn *infoINTIn;
   chipsetBus->busIRQ[busIRQ].present = TRUE;
   chipsetBus->busIRQ[busIRQ].ic = 
            chipsetInfo->ioapic[devInt->ioapicID].num;
   chipsetBus->busIRQ[busIRQ].pin = devInt->intIn;

   // Find out the trigger/polarity for this interrupt   
   infoINTIn = VMnixAcpiGetINTInInfo(acpiIOAPICs,
                                     devInt->ioapicID, 
                                     devInt->intIn);
   ASSERT(infoINTIn && infoINTIn->present);
   chipsetBus->busIRQ[busIRQ].trigger = infoINTIn->trigger;
   chipsetBus->busIRQ[busIRQ].polarity = infoINTIn->polarity;
}

/*
 *----------------------------------------------------------------------
 *
 * ACPI_ParseChipset --
 *
 *      Parses the ACPI information passed from the
 *    console os for chipset information
 *
 * Results:
 *      TRUE if successful, FALSE if the ACPI information is unusable
 *
 * Side effects:
 *      A bunch of info structures.
 *
 *----------------------------------------------------------------------
 */
Bool 
ACPI_ParseChipset(VMnix_AcpiInfo *acpiInfo, 
                  Chipset_SysInfo *chipsetInfo)
{
   uint32 i;
   uint32 isaBusID, maxBusID = 0;
   Chipset_BusInfo *chipsetBus;

   if (!strncmp(acpiInfo->oemID, "IBM", 3) &&
       !strncmp(acpiInfo->productID, "SERVIGIL", 8)) {
      Log("resolved as IBM_X440");
      // TODO: replace this MPS_Signature with a more generic term
      MPS_Signature = IBM_X440;
   }
   // XXX find out signature and add Relentless esx130

   /*
    * IOAPIC info
    */
   if (!ACPIParseIOAPIC(chipsetInfo->ioapic, acpiInfo->ioapics)) {
      return FALSE;
   }

   /*
    * PCI bus info 
    */ 

   // Go through the PCI buses and collect information about
   // 1. The bus itself
   // 2. The interrupts for the slots/pins on the bus
   for (i = 0; i < VMK_PCI_NUM_BUSES; i++) {
      uint32 slot;
      VMnix_AcpiPCIBus *bus = acpiInfo->busInfo.buses[i];
      if (!bus) {
         continue;
      }

      chipsetBus = chipsetInfo->buses[bus->busID];
      if (chipsetBus) {
         Warning("Bus %d is already defined", bus->busID);
         continue;
      }
      if (bus->busID >= MAX_BUSES) {
         Warning("Bus %d is not within range, max buses is %d", 
                 bus->busID, MAX_BUSES);
         return FALSE;
      }

      chipsetBus = (Chipset_BusInfo *)Mem_Alloc(sizeof(*chipsetBus));
      if (!chipsetBus) {
         Warning("Failed to allocate memory for acpi bus info");
         return FALSE;
      }

      chipsetInfo->buses[bus->busID] = chipsetBus;
      chipsetBus->type = VMK_HW_BUSTYPE_PCI;

      // keep track of the maxBusID
      if (maxBusID < bus->busID) {
         maxBusID = bus->busID;
      }

      for (slot = 0; slot < PCI_NUM_SLOTS; slot++) {
         uint32 pin; 
         for (pin = 0; pin < PCI_NUM_PINS; pin++) {
            VMnix_AcpiDevInt *devInt;
            uint32 busIRQ;
            devInt = &bus->devInt[slot][pin];
            if (!devInt->present) {
               continue;
            }
            // Convert the slot/pin into busIRQ
            busIRQ = MPS_PCI_BUSIRQ(slot, pin);
            if (busIRQ >= MAX_BUS_IRQS) {
               Warning("slot %d, pin %d, busIRQ %d, "
                       "greater than max bus irq %d",
                       slot, pin, busIRQ, MAX_BUS_IRQS);
               return FALSE;
            }
            // fill in the irq details
            ACPISetChipsetBusIRQ(busIRQ, chipsetInfo, 
                                 chipsetBus, devInt, 
                                 acpiInfo->ioapics);
         }
      }
   }

   /*
    * ISA bus info 
    */ 

   // ACPI does not provide ISA bus info as such, but we can
   // can create one on the fly here.

   // dummy bus id for the ISA bus
   isaBusID = maxBusID + 1;   
   if (isaBusID >= MAX_BUSES) {
      Warning("Bus %d is not within range, max buses is %d", 
              isaBusID, MAX_BUSES);
      return FALSE;
   }
   chipsetBus = (Chipset_BusInfo *)Mem_Alloc(sizeof(*chipsetBus));
   if (!chipsetBus) {
      Warning("Failed to allocate memory for acpi isa bus info");
      return FALSE;
   }
   ASSERT(!chipsetInfo->buses[isaBusID]);
   chipsetInfo->buses[isaBusID] = chipsetBus;
   chipsetBus->type = VMK_HW_BUSTYPE_ISA;

   for (i = 0; i < NUM_ISA_IRQS; i++){
      VMnix_AcpiDevInt *devInt = &acpiInfo->legacyIRQ.irq[i];
      if (!devInt->present) {
         ASSERT(!acpiInfo->legacyIRQ.overrides[i]);
         continue;
      }
      ACPISetChipsetBusIRQ(i, chipsetInfo, chipsetBus, devInt, 
                           acpiInfo->ioapics);
      // If these were not ACPI overrides, then they were just
      // generated by the vmnix module. We generate
      // these ISA irq entries even if some of these IRQs may map to 
      // an intIn pin which is used by another device (PCI or ISA).
      // We rely on the actual interrupt hookup function to resolve this
      // conflict.
      if (!acpiInfo->legacyIRQ.overrides[i]) {
         chipsetBus->busIRQ[i].trigger = VMK_HW_ISA_INT_DEFAULT_TRIGGER;
         chipsetBus->busIRQ[i].polarity = VMK_HW_ISA_INT_DEFAULT_POLARITY;
      }
   }
   return TRUE;
}
