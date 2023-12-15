/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * chipset.c --
 *
 *	This module manages the chipset.
 */


#include "vm_types.h"
#include "vm_libc.h"
#include "vmkernel.h"
#include "chipset_int.h"
#include "ioapic.h"
#include "apic_int.h"
#include "pic.h"
#include "pci_int.h"
#include "parse.h"
#include "mps.h"
#include "host.h"
#include "acpi.h"
#include "memalloc_dist.h"

#define LOGLEVEL_MODULE Chipset
#define LOGLEVEL_MODULE_LEN 7
#include "log.h"


// AMD8131 PCI-X Tunnel Data Sheet, AMD document 24637.pdf (see PR 47757)
#define AMD8131_PCI_DEVICE_ID   0x7450
#define AMD8131_PCI_REG_MISC    0x40
#define AMD8131_NIOAMODE_BIT    0


ICType Chipset_ICType;
Chipset_ICFunctions *Chipset_ICFuncs;
Chipset_ICFunctions_Internal *Chipset_ICFuncs_Internal;
Bool chipsetInitialized;
IRQ Chipset_IrqFromPin[VMK_HW_MAX_ICS][VMK_HW_MAX_PINS_PER_IC];

static Chipset_SysInfo chipsetSysInfo;
static Proc_Entry chipsetProcEntry;

static VMK_ReturnStatus ChipsetSelectIC(ICType hostICType, 
                                        VMnix_SavedMPS *mps,
                                        VMnix_ConfigOptions *vmnixOptions,
                                        VMnix_AcpiInfo *acpiInfo);



/*
 *----------------------------------------------------------------------
 *
 * ChipsetProcRead --
 *
 * 	Callback for read operation on /proc/vmware/chipset
 *
 * Results:
 * 	VMK_OK
 *
 * Side Effects:
 * 	none
 *
 *----------------------------------------------------------------------
 */
static int
ChipsetProcRead(Proc_Entry *entry, char *buffer, int *len)
{
   ASSERT(buffer && len);
   *len = 0;
   Chipset_ICFuncs->dump(buffer, len);
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ChipsetProcWrite --
 *
 * 	Callback for write operation on /proc/vmware/chipset
 *
 * Results:
 *	VMK_BAD_PARAM if bad request
 * 	VMK_OK otherwise
 *
 * Side Effects:
 *  	IOAPIC pins may be reset
 *
 *----------------------------------------------------------------------
 */
static int
ChipsetProcWrite(Proc_Entry *entry, char *buffer, int *len)
{
   char *argv[3];
   int argc = Parse_Args(buffer, argv, 3);
   PCPU pcpuNum;
   IRQ irq;

   if (argc == 0) {
      LOG(0, "Not enough arguments");
      return VMK_BAD_PARAM;
   }

   if (!strcmp(argv[0], "ResetPins")) {

      Bool levelOnly;

      if (Chipset_ICType != ICTYPE_IOAPIC) {
         LOG(0, "IC is not IOAPIC, nothing to do");
         return VMK_OK;
      }

      levelOnly = (argc > 1) && !strcmp(argv[1], "LevelOnly");
      Warning("ResetPins %s", levelOnly ? "LevelOnly" : "");
      IOAPIC_ResetPins(levelOnly);
      return VMK_OK;

   }

   if (!strcmp(argv[0], "SendNMI")) {

      if (argc != 2 || Parse_Int(argv[1], strlen(argv[1]), &pcpuNum) != VMK_OK){
	 LOG(0, "Incorrect arguments");
	 return VMK_BAD_PARAM;
      }

      Warning("SendNMI %d", pcpuNum);
      APIC_SendNMI(pcpuNum);
      return VMK_OK;

   }

   if (!strcmp(argv[0], "SetHostIRQ")) {

      if (argc != 2 || Parse_Int(argv[1], strlen(argv[1]), &irq) != VMK_OK){
	 LOG(0, "Incorrect arguments");
	 return VMK_BAD_PARAM;
      }

      Warning("SetHostIRQ %d", irq);
      Host_SetPendingIRQ(irq);
      return VMK_OK;

   }

   LOG(0, "Unknown command <%s>", argv[0]);
   return VMK_BAD_PARAM;
}

/*
 *----------------------------------------------------------------------
 *
 * Chipset_Init --
 *
 * 	Initialize the Chipset module
 *
 * Results:
 * 	error if initialization fails
 *
 * Side Effects:
 * 	Chipset_ICType is set up
 * 	Chipset_Funcs is set up
 * 	Chipset_IrqFromPin is set up
 * 	IC is initialized
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Chipset_Init(VMnix_Init *vmnixInit,
	     VMnix_Info *vmnixInfo,
	     VMnix_ConfigOptions *vmnixOptions,
	     VMnix_SharedData *sharedData,
             VMnix_AcpiInfo *acpiInfo)
{
   VMK_ReturnStatus status;
   int ic;
   int pin;
   IRQ irq;


   /*
    * Select the type of IC the vmkernel is going to use.
    * It has to be the same one the host is using.
    */
   status = ChipsetSelectIC(vmnixInfo->ICType, &vmnixInit->savedMPS, 
                            vmnixOptions, acpiInfo);
   if (status != VMK_OK) {
      return status;
   }

   /*
    * Initialize the IC.
    */
   Proc_InitEntry(&chipsetProcEntry);
   chipsetProcEntry.parent = NULL;
   chipsetProcEntry.read = ChipsetProcRead;
   chipsetProcEntry.write = ChipsetProcWrite;
   chipsetProcEntry.private = NULL;
   Proc_Register(&chipsetProcEntry, "chipset", FALSE);
   status = Chipset_ICFuncs_Internal->init(vmnixInfo->ICType, vmnixOptions, sharedData, &chipsetSysInfo);
   if (status != VMK_OK) {
      return status;
   }
   chipsetInitialized = TRUE;


   /*
    * Build the Chipset_IrqFromPin array depending on IC used by COS.
    */
   for (ic = 0; ic < VMK_HW_MAX_ICS; ic++) {
      for (pin = 0; pin < VMK_HW_MAX_PINS_PER_IC; pin++) {
	 Chipset_IrqFromPin[ic][pin] = PCI_IRQ_NONE;
      }
   }

   switch (vmnixInfo->ICType) {
   case ICTYPE_PIC:
      for (irq = 0; irq < vmnixInfo->numirqs; irq++) {
	 ic = vmnixInfo->irq[irq].ic;
	 ASSERT(ic == 0);
	 pin = vmnixInfo->irq[irq].pin;
	 ASSERT(pin >= 0);
	 ASSERT(pin < NUM_ISA_IRQS);
	 ASSERT(pin == irq);
	 Chipset_IrqFromPin[ic][pin] = irq;
      }
      break;
   case ICTYPE_IOAPIC:
      for (irq = 0; irq < vmnixInfo->numirqs; irq++) {
	 pin = vmnixInfo->irq[irq].pin;
	 if (irq == CASCADE_IRQ) {
	    // CASCADE_IRQ is invisible so it should not be connected to any pin
	    // but some machines report it in their MPS table nevertheless.
	    // ASSERT(pin == -1);
            continue;
	 }
	 if (pin == -1) {
	    // This is an unusable irq as it is not connected to any pin.
	    if (irq == TIMER_IRQ) {
	       // TIMER_IRQ may not have a pin if it's an external (through PIC)
	       // or local (through LVT0 on local APIC) interrupt.
	       continue;
	    }
	    if (!(vmnixInfo->irq[irq].used & IRQ_COS_USED)) {
	       // It's not used by COS, so all is well
	       continue;
	    } else {
	       // It's used by COS. Either there is a BIOS bug and COS is 
	       // led to use an irq for which there is no information in the
	       // MPS table (we've seen a faulty Compaq BIOS that had COS
	       // use irq 7 for on board USB, yet did not describe irq 7 in
	       // its MPS table, see PR 26263 or PR 38318)
	       // or the irq is for a device on a secondary
	       // bus whose bridge COS cannot see because it has been
	       // mistakenly assigned to vmkernel and thus COS falls back
	       // to the ISA irq which may not be described in the MPS table.
	       SysAlert("irq %d has no pin (COS vector is %02x)\n"
			"Make sure PCI bridges are assigned to COS",
			irq, vmnixInfo->irq[irq].vector);
	       // If no vector was assigned, it's the first case and
	       // we can safely ignore it.
	       if (vmnixInfo->irq[irq].vector) {
		  return VMK_FAILURE;
	       } else {
	          continue;
	       }
	    }
	 }
         ic = vmnixInfo->irq[irq].ic;
         ASSERT(ic >= 0);
         ASSERT_NOT_IMPLEMENTED(ic < VMK_HW_MAX_ICS);
	 ASSERT(pin >= 0);
         ASSERT_NOT_IMPLEMENTED(pin < VMK_HW_MAX_PINS_PER_IC);
         Chipset_IrqFromPin[ic][pin] = irq;
      }
      break;
   default:
      NOT_IMPLEMENTED();
      break;
   }

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Chipset_LateInit --
 *
 *      Late initialization of chipset
 *
 * Results:
 *      None
 *
 * Side Effects:
 *      On AMD-8131 PCI-X Tunnel chip, bouncing of masked interrupts to
 *      the legacy IOAPIC is disabled. See PR 47757.
 *
 *----------------------------------------------------------------------
 */
void
Chipset_LateInit(void)
{
   PCI_Device *dev;
   uint32 reg;

   /*
    * Traverse the list of PCI devices to find AMD-8131 PCI-X Tunnel chips.
    *
    * NOTE: no need for read/write atomicity since at this time, we are UP
    * with interrupts disabled.
    */
   for (dev = PCI_GetFirstDevice(); dev != NULL; dev = PCI_GetNextDevice(dev)) {
      if ((dev->vendorID == PCI_VENDOR_ID_AMD) &&
          (dev->deviceID == AMD8131_PCI_DEVICE_ID)) {
         Log("Found AMD-8131 at %s, disabling NIOAMODE", dev->busAddress);
         PCI_ReadConfig32(dev->bus, dev->slotFunc, AMD8131_PCI_REG_MISC, &reg);
         LOG(0, "MISC reg is 0x%08x", reg);
         reg &= ~(1<<AMD8131_NIOAMODE_BIT);
         PCI_WriteConfig32(dev->bus, dev->slotFunc, AMD8131_PCI_REG_MISC, reg);
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * Chipset_TriggerType --
 *
 *      Determines if an ISA interrupt is edge or level triggered
 *
 * Results:
 *      VMK_HW_INT_LEVEL or VMK_HW_INT_EDGE
 *
 * Side effects:
 *      none
 *
 *----------------------------------------------------------------------
 */
int
Chipset_TriggerType(IRQ isaIRQ)
{
   Bool levelTriggered;
   uint32 port;

   ASSERT(isaIRQ < NUM_ISA_IRQS);

   port = CHIPSET_ELCR_PORT + (isaIRQ >> 3);
   levelTriggered = (INB(port) >> (isaIRQ & 7)) & 1;

   if (levelTriggered) {
      return VMK_HW_INT_LEVEL;
   } else {
      return VMK_HW_INT_EDGE;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * Chipset_PrintSysInfo --
 *
 *    Print the contents of 'chipseSysInfo'
 *
 * Results:
 *    none
 *
 * Side effects:
 *    none
 *
 *----------------------------------------------------------------------
 */
static void
Chipset_PrintSysInfo(Chipset_SysInfo *chipsetSysInfo)
{
   uint32 i;
   // print IOAPIC info
   for (i = 0; i < IOAPICID_RANGE; i++) {
      Chipset_IOAPICInfo *ioapic = &chipsetSysInfo->ioapic[i];
      if (!ioapic->present) {
         continue;
      }
      Log("IOAPIC id %d, num %d, physAddr 0x%x",
          i, ioapic->num, ioapic->physAddr);
   }

   // Print bus irq info
   for (i = 0; i < MAX_BUSES; i++) {
      uint32 j;
      Chipset_BusInfo *busInfo = chipsetSysInfo->buses[i];
      if (!busInfo) {
         continue;
      }
      Log("%s, busID %d", 
          busInfo->type == VMK_HW_BUSTYPE_ISA ? "isa" : "pci",
          i);
      for (j = 0; j < MAX_BUS_IRQS; j++) {
         Chipset_BusIRQInfo *irqInfo = &busInfo->busIRQ[j];
         if (!irqInfo->present) {
            continue;
         }
         Log("%s, busId:slot:pin (%d:%d:%c), busIRQ %d, ic %d, pin %d, trigger %d, polarity %d",
             busInfo->type == VMK_HW_BUSTYPE_ISA ? "isa" : "pci",
             i, MPS_BusIRQ2Slot(busInfo->type, j), 
             MPS_BusIRQ2Pin(busInfo->type, j),
             j, irqInfo->ic, irqInfo->pin, irqInfo->trigger,
             irqInfo->polarity);
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * Chipset_CompareMPSAndACPI --
 *
 *    This is function is used purely for debugging. 
 * 
 *    Compares the data in 'mpsSyInfo' to 'acpiSysInfo'. 
 *    The IOAPICs comparison is fairly straightforward.
 *    The comparision for the data in the busInfo gets
 *    a little fuzzy because
 *    1. ACPI does not contain any ISA bus info so 
 *       the acpi struct contains bogus bus ids for ISA
 *       buses.
 *    2. The mps structure sometimes has empty entries for
 *       pci buses
 *    Nevertheless, we try to print Warnings where ever we can
 *    and give 'Logs' in other cases
 *
 * Results:
 *    none
 *
 * Side effects:
 *    none
 *
 *----------------------------------------------------------------------
 */
static void
Chipset_CompareMPSAndACPI(Chipset_SysInfo *mpsSysInfo, 
                          Chipset_SysInfo *acpiSysInfo)
{
   uint32 i;
   Chipset_BusInfo *mpsISABus = NULL;
   Chipset_BusInfo *acpiISABus = NULL;
   // Compare IOAPICs
   for (i = 0; i < IOAPICID_RANGE; i++) {
      Chipset_IOAPICInfo *mpsIOAPIC = &mpsSysInfo->ioapic[i];
      DEBUG_ONLY(Chipset_IOAPICInfo *acpiIOAPIC = &acpiSysInfo->ioapic[i]);
      ASSERT((mpsIOAPIC->present && acpiIOAPIC->present) ||
             (!mpsIOAPIC->present && !acpiIOAPIC->present));
      if (!mpsIOAPIC->present) {
         continue;
      }
      ASSERT(mpsIOAPIC->physAddr == acpiIOAPIC->physAddr);
   }

   // Compare pci buses
   for (i = 0; i < MAX_BUSES; i++) {
      uint32 j;
      Chipset_BusInfo *mpsBus = mpsSysInfo->buses[i];
      Chipset_BusInfo *acpiBus = acpiSysInfo->buses[i];
      if (acpiBus) {
         if (acpiBus->type == VMK_HW_BUSTYPE_ISA) {
            if (acpiISABus) {
               Warning("More than one acpi ISA bus found");
            }
            acpiISABus = acpiBus;
         }
      } 
      if (mpsBus) {
         if (mpsBus->type == VMK_HW_BUSTYPE_ISA) {
            if (mpsISABus) {
               Warning("More than one mps ISA bus found");
            }
            mpsISABus = mpsBus;
         }
      }
      if (!mpsBus && !acpiBus) {
         continue;
      }
      if (mpsBus && mpsBus->type == VMK_HW_BUSTYPE_ISA) {
         continue;
      }
      if (acpiBus && acpiBus->type == VMK_HW_BUSTYPE_ISA) {
         continue;
      }
      for (j = 0; j < MAX_BUS_IRQS; j++) {
         Chipset_BusIRQInfo *mpsIRQ = (mpsBus ? &mpsBus->busIRQ[j] : NULL);
         Chipset_BusIRQInfo *acpiIRQ = (acpiBus ? &acpiBus->busIRQ[j] : NULL);

         if (mpsIRQ && mpsIRQ->present) {
            if (!acpiIRQ || !acpiIRQ->present) {
               SysAlert("Missing acpi entry for PCI bus:slot:pin (%d:%d:%c), busIRQ %d is absent in acpi",
                       i, MPS_BusIRQ2Slot(VMK_HW_BUSTYPE_PCI, j),
                       MPS_BusIRQ2Pin(VMK_HW_BUSTYPE_PCI, j), j);
            }
         }
         if (acpiIRQ && acpiIRQ->present) {
            if (!mpsIRQ || !mpsIRQ->present) {
               Warning("Missing mps entry for PCI bus:slot:pin (%d:%d:%c), busIRQ %d is absent in mps",
                   i, MPS_BusIRQ2Slot(VMK_HW_BUSTYPE_PCI, j),
                   MPS_BusIRQ2Pin(VMK_HW_BUSTYPE_PCI, j), j);
            }
         }

         if (!mpsIRQ || !acpiIRQ) {
            continue;
         }
         if (!mpsIRQ->present || !acpiIRQ->present) {
            continue;
         }

         if (mpsIRQ->ic != acpiIRQ->ic ||
             mpsIRQ->pin != acpiIRQ->pin ||
             mpsIRQ->trigger != acpiIRQ->trigger ||
             mpsIRQ->polarity != acpiIRQ->polarity) {
            SysAlert("MISMATCH between mps and acpi for PCI busID:slot:pin (%d:%d:%c), busIRQ %d, "
                    "mps: ic:pin:trig:pol (%d:%d:%d:%d), "
                    "acpi: ic:pin:trig:pol (%d:%d:%d:%d)",
                    i, MPS_BusIRQ2Slot(VMK_HW_BUSTYPE_PCI, j),
                    MPS_BusIRQ2Pin(VMK_HW_BUSTYPE_PCI,j), 
                    j, 
                    mpsIRQ->ic, mpsIRQ->pin, mpsIRQ->trigger, mpsIRQ->polarity,
                    acpiIRQ->ic, acpiIRQ->pin, acpiIRQ->trigger, acpiIRQ->polarity);
         }
      }
   }

   // Compare isa buses
   for (i = 0; i < MAX_BUS_IRQS; i++) {
      Chipset_BusIRQInfo *mpsIRQ = &mpsISABus->busIRQ[i];
      Chipset_BusIRQInfo *acpiIRQ = &acpiISABus->busIRQ[i];

      if (!mpsIRQ->present) {
         continue;
      }

      if (!acpiIRQ->present) {
         Warning("ISA busIRQ %d, missing in acpi, "
                 "mps: %d:%d:%d",
                 i, mpsIRQ->pin, mpsIRQ->trigger, mpsIRQ->polarity);
         continue;
      }
      if (mpsIRQ->pin != acpiIRQ->pin ||
          mpsIRQ->trigger != acpiIRQ->trigger ||
          mpsIRQ->polarity != acpiIRQ->polarity) {
         if (i == 0) {
            // IRQ 0 is usually enumerated incorrectly in MPS
            // so make this just a warning
            Warning("ISA busIRQ %d, mps and acpi mismatch "
                     "mps: %d:%d:%d, acpi = %d:%d:%d",
                     i, mpsIRQ->pin, mpsIRQ->trigger, mpsIRQ->polarity,
                     acpiIRQ->pin, acpiIRQ->trigger, acpiIRQ->polarity);
         } else {
            SysAlert("ISA busIRQ %d, mps and acpi mismatch "
                     "mps: %d:%d:%d, acpi = %d:%d:%d",
                     i, mpsIRQ->pin, mpsIRQ->trigger, mpsIRQ->polarity,
                     acpiIRQ->pin, acpiIRQ->trigger, acpiIRQ->polarity);
         }
      }
      if (mpsIRQ->ic != acpiIRQ->ic) {
         SysAlert("ISA: busIRQ %d, mpsIC %d != acpiIC %d",
                  i, mpsIRQ->ic, acpiIRQ->ic);
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * ChipsetSelectIC --
 *
 *      Selects the IC the vmkernel is going to use. First tries to 
 *    use the ACPI info and if that does not exists falls back on 
 *    using the MPS info.
 *
 * Results:
 *      VMK_NO_RESOURCES if vmkernel cannot use the IC the host is using
 *
 * Side effects:
 *      A bunch of info structures.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
ChipsetSelectIC(ICType hostICType, 
                VMnix_SavedMPS *mps,
                VMnix_ConfigOptions *vmnixOptions,
                VMnix_AcpiInfo *acpiInfo)
{
   Chipset_ICType = ICTYPE_PIC;
   Chipset_ICFuncs = &PIC_Functions;
   Chipset_ICFuncs_Internal = &PIC_Functions_Internal;
   Bool ok;

   // Try the ACPI info first
   if (vmnixOptions->acpiIntRouting) {
      if (acpiInfo && acpiInfo->intRoutingValid) {
         ASSERT(hostICType == acpiInfo->icType);
         if (hostICType == ICTYPE_PIC) {
            // It is most likely  that 'noapic'
            // was used. Since the host is using PIC, we have to.
            SysAlert("ACPI found but host is using PIC");
            SysAlert("Make sure that if 'noapic' is used, it is on purpose");
            return VMK_OK;
         }
         Log("Using ACPI");
         if (acpiInfo->icType == ICTYPE_IOAPIC) {
            // The host is using the IOAPIC and we can too.
            Chipset_ICType = ICTYPE_IOAPIC;
            Chipset_ICFuncs = &IOAPIC_Functions;
            Chipset_ICFuncs_Internal = &IOAPIC_Functions_Internal;
         }
         ok = ACPI_ParseChipset(acpiInfo, &chipsetSysInfo);

         // For now make sure that we atleast do as well as the MPS tables. 
         // Assume that MPS table is reliable (which probably wont be the
         // case moving forward)
         if (vmx86_debug) {
            Chipset_SysInfo *mpsSysInfo;
            mpsSysInfo = (Chipset_SysInfo *)Mem_Alloc(sizeof(*mpsSysInfo));
            memset(mpsSysInfo, 0, sizeof(*mpsSysInfo));
            if (MPS_ParseChipset(mps, mpsSysInfo)) {
               Chipset_CompareMPSAndACPI(mpsSysInfo, &chipsetSysInfo);
            }
            Mem_Free(mpsSysInfo);
         }
         // Print the sysinfo collected so far
         Chipset_PrintSysInfo(&chipsetSysInfo);
         return (ok ? VMK_OK : VMK_FAILURE);
      } else {
         Warning("Ignoring acpi irq routing as acpi information is not valid");
      }
   } 

   Log("Using MPS");
   // Looks like ACPI information is not available.
   // Try the MPS table
   if (!mps->present) {
      switch (hostICType) {
      case ICTYPE_PIC:
	 // Either the system is an older UP machine or the BIOS did not set
	 // the MPS table correctly. We can run in PIC mode.
	 SysAlert("No MPS found, check BIOS if system is not UP");
         return VMK_OK;
      case ICTYPE_IOAPIC:
	 // This should not be possible. Even if it was, the host would be
	 // using the IOAPIC but we could not, this not supported.
	 SysAlert("No MPS found, yet host is using IOAPIC!");
	 return VMK_NO_RESOURCES;
      default:
	 NOT_IMPLEMENTED();
	 break;
      }
   } else if (mps->mpf.feature1) {
      // System uses one of the default configurations.
      // See Intel MP Spec Chapter 4.
      switch (hostICType) {
      case ICTYPE_PIC:
	 // The host does not support the default configuration either or
	 // 'noapic' was used in the lilo command line. We can run in PIC mode.
	 SysAlert("default MPS found");
	 return VMK_OK;
      case ICTYPE_IOAPIC:
	 // The host supports the default configuration and thus sould be using
	 // the IOAPIC but we could not, this not supported. The user has to
	 // check the BIOS for a way to generate a normal MPS table or use
	 // 'noapic' in the lilo command line to avoid MPS parsing by the host.
	 SysAlert("default MPS found, check BIOS or use 'noapic'");
	 return VMK_NO_RESOURCES;
      default:
	 NOT_IMPLEMENTED();
	 break;
      }
   }

   if (mps->mpf.feature2) {
      Log("mpf feature2 = 0x%x", mps->mpf.feature2);
   }
   
   switch (hostICType) {
   case ICTYPE_PIC:
      // Either the host did not find the MPS table or more likely 'noapic'
      // was used. Since the host is using PIC, we have to.
      SysAlert("MPS found but host is using PIC");
      SysAlert("Make sure that if 'noapic' is used, it is on purpose");
      return VMK_OK;
   case ICTYPE_IOAPIC:
      // The host is using the IOAPIC and we can too.
      Chipset_ICType = ICTYPE_IOAPIC;
      Chipset_ICFuncs = &IOAPIC_Functions;
      Chipset_ICFuncs_Internal = &IOAPIC_Functions_Internal;
      ok = MPS_ParseChipset(mps, &chipsetSysInfo);
      // Print the sysinfo collected so far
      Chipset_PrintSysInfo(&chipsetSysInfo);

      if (!ok) {
	 return VMK_BAD_MPS;
      } else {
         return VMK_OK;
      }
   default:
      NOT_IMPLEMENTED();
      break;
   }

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Chipset_GetBusIRQInfo --
 *
 *      Return information about a bus IRQ
 *
 * Results:
 *      The IOAPIC and INTIN line for this busIRQ
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
Chipset_GetBusIRQInfo(int busType, int busID, int busIRQ, Chipset_BusIRQInfo *busIRQInfo)
{
   Chipset_BusInfo **buses = chipsetSysInfo.buses;


   ASSERT(Chipset_ICType == ICTYPE_IOAPIC);

   busIRQInfo->present = FALSE;

   switch (busType) {
   case VMK_HW_BUSTYPE_ISA:
   case VMK_HW_BUSTYPE_EISA:
      /*
       * As a convenience for the caller, when the busType is ISA/EISA,
       * since there is only one, the busID parameter is a dummy and we find
       * the real one directly.
       */
      ASSERT(busID == -1);
      for (busID = 0; busID < MAX_BUSES; busID++) {
         if (buses[busID] != NULL && ((buses[busID]->type == VMK_HW_BUSTYPE_ISA) ||
					(buses[busID]->type == VMK_HW_BUSTYPE_EISA))) {
            break;
	 }
      }
      if (busID == MAX_BUSES) {
         Warning("Couldn't find ISA or EISA bus");
	 return;
      } else {
         *busIRQInfo = buses[busID]->busIRQ[busIRQ];
      }
      break;
   case VMK_HW_BUSTYPE_PCI:
      if (busID >= MAX_BUSES || buses[busID] == NULL) {
         Warning("bus %d isn't present", busID);
	 return;
      } else if (buses[busID]->type != VMK_HW_BUSTYPE_PCI) {
         Warning("bus type mismatch (%d != %d) for bus %d busIRQ %d",
		    busType, buses[busID]->type, busID, busIRQ);
	 return;
      } else {
         *busIRQInfo = buses[busID]->busIRQ[busIRQ];
      }
      break;
   default:
      Warning("Unknown bus type %d for bus %d",
	         busType, busID);
      return;
   }

   if (busIRQInfo->present) {
      Log("%03d:%02d %c busIRQ=%3d on %02d-%02d",
	   busID, MPS_BusIRQ2Slot(busType, busIRQ),
           MPS_BusIRQ2Pin(busType, busIRQ),
	   busIRQ, busIRQInfo->ic, busIRQInfo->pin);
   } else {
      Log("%03d:%02d %c busIRQ=%3d not connected",
	   busID, MPS_BusIRQ2Slot(busType, busIRQ),
           MPS_BusIRQ2Pin(busType, busIRQ),
	   busIRQ);
   }
}
