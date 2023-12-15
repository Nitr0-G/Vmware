/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * mps.c --
 *
 *	This module handles the MPS table
 */


#include "vm_types.h"
#include "vm_libc.h"
#include "vmkernel.h"
#include "chipset_int.h"
#include "memalloc.h"
#include "libc.h"
#include "mps.h"
#include "hardware_public.h"

#define LOGLEVEL_MODULE MPS
#define LOGLEVEL_MODULE_LEN 3
#include "log.h"

#define MPS_INTTYPE_INT    0
#define MPS_INTTYPE_NMI    1
#define MPS_INTTYPE_SMI    2
#define MPS_INTTYPE_ExtINT 3

MPS_Signatures MPS_Signature = UNRESOLVED;



/*
 *----------------------------------------------------------------------
 *
 * MPS_ParseChipset --
 *
 *      Parses the MPS table for chipset information
 *
 * Results:
 *      TRUE if successful, FALSE if the MPS table is unusable
 *
 * Side effects:
 *      A bunch of info structures.
 *
 *----------------------------------------------------------------------
 */
Bool
MPS_ParseChipset(VMnix_SavedMPS *mps, Chipset_SysInfo *sysInfo)
{
   Chipset_IOAPICInfo *ioapic = sysInfo->ioapic;
   Chipset_BusInfo **buses = sysInfo->buses;
   MPConfigTable *mpc;
   uint8 *ptr;
   int i;
   int pciIRQs = 0;
   int entrySize;
   int numIOAPIC = 0;
   int numINT = 0;
   int ioapic_version = 0;
   char oemId[9];
   char productId[13];


   ASSERT(mps->present);
   ASSERT(mps->mpf.feature1 == 0);
   ASSERT(Chipset_ICType == ICTYPE_IOAPIC);
   for (i=0; i < IOAPICID_RANGE; i++) {
      ioapic[i].present = FALSE;
   }

   /*
    *  scan the MPC table for interrupts
    */
   mpc = (MPConfigTable*)&mps->mpc;

   for (ptr=(uint8*)(mpc+1), i=0; i<mpc->count; i++) {
      switch (*ptr) {
      case PROC_ENTRY:
	 entrySize = sizeof(MPProcessorEntry);
	 break;
      case BUS_ENTRY: {
	 MPBusEntry *mpb = (MPBusEntry*)ptr;

	 entrySize = sizeof(MPBusEntry);

	 if (buses[mpb->busID] != NULL) {
	    Warning("Bus %d is already defined", mpb->busID);
	 } else {
	    buses[mpb->busID] = (Chipset_BusInfo *)Mem_Alloc(sizeof(Chipset_BusInfo));
	    ASSERT_NOT_IMPLEMENTED(buses[mpb->busID] != NULL);
	    memset(buses[mpb->busID], 0, sizeof(Chipset_BusInfo));
	 }

	 if (!memcmp("ISA", mpb->busTypeStr, 3)) {
            Log("bus %03d ISA", mpb->busID);
	    buses[mpb->busID]->type = VMK_HW_BUSTYPE_ISA;
	 } else if (!memcmp("EISA", mpb->busTypeStr, 4)) {
            Log("bus %03d EISA", mpb->busID);         
	    buses[mpb->busID]->type = VMK_HW_BUSTYPE_EISA;
	 } else if (!memcmp("PCI", mpb->busTypeStr, 3)) {
            Log("bus %03d PCI", mpb->busID);
	    buses[mpb->busID]->type = VMK_HW_BUSTYPE_PCI;
	 } else {
	    Warning("bus %03d unexpected type %s", mpb->busID, mpb->busTypeStr);
	    buses[mpb->busID]->type = VMK_HW_BUSTYPE_NONE;
	 }
	 break;
      }
      case IOAPIC_ENTRY: {
	 MPAPICEntry *mpapic = (MPAPICEntry*)ptr;

	 entrySize = sizeof(MPAPICEntry);
	 if (mpapic->id >= IOAPICID_RANGE) {
	    Warning("IOAPICid %d is too big", mpapic->id);
	 } else {
	    if (mpapic->flags & MPS_APIC_ENABLED) {
	       Log("ioapic %02d (%d) @ %08x version 0x%x", 
		   mpapic->id, numIOAPIC, mpapic->physAddr, mpapic->version);
	       ioapic[mpapic->id].present = TRUE;
	       ioapic[mpapic->id].physAddr = mpapic->physAddr;
	       ioapic[mpapic->id].num = numIOAPIC++;
	       if (numIOAPIC == 1) {
		  ioapic_version = mpapic->version;
	       } else if (mpapic->version != ioapic_version) {
		  Warning("version is not the same as that of ioapic 0");
	       }
	    }
	 }
	 break;
      }
      case IOINT_ENTRY: {
	 IOInterEntry *ioi = (IOInterEntry*)ptr;
	 Chipset_BusInfo *busInfo;

	 entrySize = sizeof(IOInterEntry);

	 busInfo = buses[ioi->srcBusID];
	 if (busInfo == NULL) {
	    Warning("No bus ID %d for int entry", ioi->srcBusID);
	    break;
	 }

	 if (ioi->destIOAPICID >= IOAPICID_RANGE) {
	    Warning("IOAPIC ID %d too big for int entry", ioi->destIOAPICID);
	    break;
	 }

	 if (!ioapic[ioi->destIOAPICID].present) {
	    Warning("No IOAPIC ID %d for int entry", ioi->destIOAPICID);
	    break;
	 }
         Log("%s %03d:%02d %c busIRQ=%3d on %02d-%02d (%x)",
	     ioi->interType == MPS_INTTYPE_INT ? "int" :
	     ioi->interType == MPS_INTTYPE_NMI ? "nmi" :
	     ioi->interType == MPS_INTTYPE_SMI ? "smi" :
	     ioi->interType == MPS_INTTYPE_ExtINT ? "ext" : "it?",
	     ioi->srcBusID,
             MPS_BusIRQ2Slot(busInfo->type, ioi->srcBusIRQ),
             MPS_BusIRQ2Pin(busInfo->type, ioi->srcBusIRQ),
	     ioi->srcBusIRQ,
	     ioapic[ioi->destIOAPICID].num, ioi->destIOAPICIntIN,
	     ioi->flags);


	 /* we don't use ExtINT, NMI, or SMI IOAPIC entries */
	 if (ioi->interType != MPS_INTTYPE_INT) {
	    break;
	 }

	 if (busInfo->busIRQ[ioi->srcBusIRQ].present) {
	    Warning("ignoring duplicate int for bus %d slot %d, busIRQ %d",
		    ioi->srcBusID, MPS_BusIRQ2Slot(busInfo->type, ioi->srcBusIRQ), 
                    ioi->srcBusIRQ);
	    break;
	 }

	 busInfo->busIRQ[ioi->srcBusIRQ].present = TRUE;
	 busInfo->busIRQ[ioi->srcBusIRQ].ic = ioapic[ioi->destIOAPICID].num;
	 busInfo->busIRQ[ioi->srcBusIRQ].pin = ioi->destIOAPICIntIN;

	 if (busInfo->type == VMK_HW_BUSTYPE_PCI) {
	    pciIRQs++;
	 }
	 numINT++;

	 switch (ioi->flags & MPS_POLARITY_MASK) {
	 case MPS_POLARITY_BUS: {
	    switch (busInfo->type) {
	    case VMK_HW_BUSTYPE_ISA:
	    case VMK_HW_BUSTYPE_EISA:
	       busInfo->busIRQ[ioi->srcBusIRQ].polarity = VMK_HW_INT_ACTIVE_HIGH;
	       break;
	    case VMK_HW_BUSTYPE_PCI:
	       busInfo->busIRQ[ioi->srcBusIRQ].polarity = VMK_HW_INT_ACTIVE_LOW;
	       break;
	    default:
	       NOT_IMPLEMENTED();
	    }
	    break;
	 }
	 case MPS_POLARITY_ACTIVE_HIGH:
	    busInfo->busIRQ[ioi->srcBusIRQ].polarity = VMK_HW_INT_ACTIVE_HIGH;
	    break;
	 case MPS_POLARITY_ACTIVE_LOW:
	    busInfo->busIRQ[ioi->srcBusIRQ].polarity = VMK_HW_INT_ACTIVE_LOW;
	    break;
	 default:
	    NOT_IMPLEMENTED();
	 }
	 
	 switch (ioi->flags & MPS_TRIGGER_MASK) {
	 case MPS_TRIGGER_BUS: {
	    switch (busInfo->type) {
	    case VMK_HW_BUSTYPE_ISA:
	       busInfo->busIRQ[ioi->srcBusIRQ].trigger = VMK_HW_INT_EDGE;
	       break;
	    case VMK_HW_BUSTYPE_EISA:
	       busInfo->busIRQ[ioi->srcBusIRQ].trigger =
		  Chipset_TriggerType(ioi->srcBusIRQ);
	       break;
	    case VMK_HW_BUSTYPE_PCI:
	       busInfo->busIRQ[ioi->srcBusIRQ].trigger = VMK_HW_INT_LEVEL;
	       break;
	    default:
	       NOT_IMPLEMENTED();
	    }
	    break;
	 }
	 case MPS_TRIGGER_EDGE:
	    busInfo->busIRQ[ioi->srcBusIRQ].trigger = VMK_HW_INT_EDGE;
	    break;
	 case MPS_TRIGGER_LEVEL:
	    busInfo->busIRQ[ioi->srcBusIRQ].trigger = VMK_HW_INT_LEVEL;
	    break;
	 default:
	    NOT_IMPLEMENTED();
	 }	    
	 break;
      }
      case LOCALINT_ENTRY:
	 entrySize = sizeof(IOInterEntry);
	 break;
      default:
	 NOT_IMPLEMENTED();
	 break;
      }

      ptr += entrySize;
   }

   if (!pciIRQs) {
      /*
       * There was no PCI entries in the MPS table. It may be because it
       * is an older motherboard or some DOS compatibility mode was selected
       * in the BIOS.
       *
       * We will try using ISA entries even for PCI routing.
       */
      SysAlert("no PCI entries in MPS table - check BIOS settings");
   }

   memcpy(oemId, mpc->oem, 8);
   oemId[8]=0;
   memcpy(productId, mpc->productid, 12);
   productId[12]=0;
   Log("<%s> <%s>", oemId, productId);
   Log("IOAPIC Version 0x%02x, CPU type %d (%s)", ioapic_version, cpuType,
		cpuType == CPUTYPE_INTEL_P6 ? "P3":
		cpuType == CPUTYPE_INTEL_PENTIUM4 ? "P4":
		cpuType == CPUTYPE_AMD_ATHLON ? "Athlon":
		cpuType == CPUTYPE_AMD_DURON ? "Duron": "Unknown");

   if (ioapic_version == 0x11 &&
       cpuType == CPUTYPE_INTEL_P6) {
      Log("resolved as P3_IOAPIC_0X11");
      MPS_Signature = P3_IOAPIC_0X11;
   } else if (ioapic_version == 0x13 &&
       cpuType == CPUTYPE_INTEL_P6) {
      Log("resolved as P3_IOAPIC_0X13");
      MPS_Signature = P3_IOAPIC_0X13;
   } else if (!strncmp(mpc->oem, "IBM ENSW", 8) &&
	      !strncmp(mpc->productid, "VIGIL SMP", 9)) {
      Log("resolved as IBM_X440");
      MPS_Signature = IBM_X440;
   } else if (!strncmp(mpc->oem, "IBM ENSW", 8) &&
	      !strncmp(mpc->productid, "RELENTLE SMP", 12)) {
      Log("resolved as IBM_RELENTLESS");
      MPS_Signature = IBM_RELENTLESS;
   } else {
      Log("left unresolved");
   }

   if ((numIOAPIC == 0) || (numINT == 0)) {
      /*
       * There was no usable IOAPIC or INT entries in the MPS table.
       */
      SysAlert("no IOAPIC or INT entries in MPS table (%d,%d)",
		      numIOAPIC, numINT);
      return FALSE;
   } else {
      return TRUE;
   }
}
