/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * ioapic.c --
 *
 *	This module manages the ioapic.
 */


#include "vm_types.h"
#include "vm_libc.h"
#include "vmkernel.h"
#include "kvmap.h"
#include "ioapic.h"
#include "apic_int.h"
#include "splock.h"
#include "host.h"
#include "idt.h"
#include "mps.h"
#include "hardware_public.h"
#include "pci_dist.h"

#define LOGLEVEL_MODULE IOAPIC
#define LOGLEVEL_MODULE_LEN 6
#include "log.h"


typedef union IOAPICEntrySettings {
   uint32 regValue;
   struct {
      unsigned vector        : 8;
      unsigned deliveryMode  : 3;
      unsigned destMode      : 1;
      unsigned deliveryStats : 1;
      unsigned polarity      : 1;
      unsigned remoteIRR     : 1;
      unsigned trigger       : 1;
      unsigned mask          : 1;
      unsigned reserved      : 15;
   }      regFields;
} IOAPICEntrySettings;

typedef union IOAPICEntryDestination {
   uint32 regValue;
   struct {
      unsigned reserved      :24;
      unsigned destination   :8;
   }      regFields;
} IOAPICEntryDestination;

typedef struct IOAPICEntry {
   IOAPICEntrySettings    settings;
   IOAPICEntryDestination destination;
} IOAPICEntry;

#define SETTINGS_OFFSET    (offsetof(IOAPICEntry, settings)/sizeof(uint32))
#define DESTINATION_OFFSET (offsetof(IOAPICEntry, destination)/sizeof(uint32))

#define REG_NUM(entryNum)  (IOREDTBL_FIRST + \
				(entryNum)*(sizeof(IOAPICEntry)/sizeof(uint32)))


typedef struct IOAPIC {
   Bool        present;
   int         id;
   PA          physAddr;
   int         numEntries;
   int         version;
   ApicReg     *reg;
   IOAPICEntry hostEntry[VMK_HW_MAX_PINS_PER_IC];
} IOAPIC;

typedef struct IOAPIC_VectorInfo {
   Bool   assigned;
   IOAPIC *ioapic;
   int    entryNum;   
   int    unused;
} IOAPIC_VectorInfo;

static IOAPIC ioapicInfo[MAX_IOAPICS];
static IOAPIC_VectorInfo ioapicVectorInfo[IDT_NUM_VECTORS];

static SP_SpinLockIRQ ioapicLock;


/*
 *----------------------------------------------------------------------
 *
 * IOAPICReadReg --
 *
 *      Read a register in the IOAPIC.
 *
 * Results:
 *      The value of the register.
 *
 * Side effects:
 *      Register is selected and read
 *
 *----------------------------------------------------------------------
 */
static INLINE uint32
IOAPICReadReg(IOAPIC *ioapic,  // IN: the ioapic to read from
              int     regNum)  // IN: the register number to read  
{
   ASSERT(ioapic->present);
   ASSERT(ioapic->reg);
   ioapic->reg[0][0] = regNum;
   return ioapic->reg[1][0];
}

/*
 *----------------------------------------------------------------------
 *
 * IOAPICWriteReg --
 *
 *      Write a register in the IOAPIC.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Register is selected and written
 *
 *----------------------------------------------------------------------
 */
static INLINE void
IOAPICWriteReg(IOAPIC *ioapic,  // IN: the ioapic to write to
               int     regNum,  // IN: the register number to write  
               uint32  regVal)  // IN: the register value to write  
{
   ASSERT(ioapic->present);
   ASSERT(ioapic->reg);
   ioapic->reg[0][0] = regNum;
   ioapic->reg[1][0] = regVal;

   /*
    * XXX Old comment:
    * Sync up the IOAPIC by reading back the value that we last wrote.
    * If we don't do this then the vmnix module won't see our latest
    * ioapic updates.  This is Linux's solution and I don't know why it
    * works.
    * XXX It may no longer be necessary with all the changes made since then.
    */
   regVal = ioapic->reg[1][0];
}

/*
 *----------------------------------------------------------------------
 *
 * IOAPICReadEntrySettings --
 *
 * 	Read the settings of a redirection entry in the IOAPIC.
 *
 * Results:
 * 	None.
 *
 * Side Effects:
 * 	Settings are stored in the out argument
 *
 *----------------------------------------------------------------------
 */
static INLINE void
IOAPICReadEntrySettings(IOAPIC *ioapic,    // IN: the ioapic to read from
			int     entryNum,  // IN: the number of the entry
			IOAPICEntrySettings *entrySettings)
					   // OUT: the entry settings
{
   int regNum = REG_NUM(entryNum);

   ASSERT(ioapic->present);
   ASSERT(entryNum < ioapic->numEntries);
   entrySettings->regValue = IOAPICReadReg(ioapic, regNum + SETTINGS_OFFSET);
}

/*
 *----------------------------------------------------------------------
 *
 * IOAPICWriteEntrySettings --
 *
 * 	Write the settings of a redirection entry in the IOAPIC.
 *
 * Results:
 * 	None.
 *
 * Side Effects:
 * 	Settings are written
 *
 *----------------------------------------------------------------------
 */
static INLINE void
IOAPICWriteEntrySettings(IOAPIC *ioapic,    // IN: the ioapic to write to 
			 int     entryNum,  // IN: the number of the entry
			 IOAPICEntrySettings *entrySettings)
					    // IN: the entry settings
{
   int regNum = REG_NUM(entryNum);

   ASSERT(ioapic->present);
   ASSERT(entryNum < ioapic->numEntries);
   IOAPICWriteReg(ioapic, regNum + SETTINGS_OFFSET, entrySettings->regValue);
}

/*
 *----------------------------------------------------------------------
 *
 * IOAPICReadEntryDestination --
 *
 * 	Read the destination of a redirection entry in the IOAPIC.
 *
 * Results:
 * 	None.
 *
 * Side Effects:
 *  	Destination is stored in the out argument
 *
 *----------------------------------------------------------------------
 */
static INLINE void
IOAPICReadEntryDestination(IOAPIC *ioapic,    // IN: the ioapic to read from
			   int     entryNum,  // IN: the number of the entry
			   IOAPICEntryDestination *entryDestination)
					      // OUT: the entry destination
{
   int regNum = REG_NUM(entryNum);

   ASSERT(ioapic->present);
   ASSERT(entryNum < ioapic->numEntries);
   entryDestination->regValue = IOAPICReadReg(ioapic, regNum + DESTINATION_OFFSET);
}

/*
 *----------------------------------------------------------------------
 *
 * IOAPICWriteEntryDestination --
 *
 * 	Write the destination of a redirection entry in the IOAPIC.
 *
 * Results:
 * 	None.
 *
 * Side Effects:
 * 	Destination is written
 *
 *----------------------------------------------------------------------
 */
static INLINE void
IOAPICWriteEntryDestination(IOAPIC *ioapic,    // IN: the ioapic to write to
			    int     entryNum,  // IN: the number of the entry
			    IOAPICEntryDestination *entryDestination)
					       // IN: the entry destination
{
   int regNum = REG_NUM(entryNum);

   ASSERT(ioapic->present);
   ASSERT(entryNum < ioapic->numEntries);
   IOAPICWriteReg(ioapic, regNum + DESTINATION_OFFSET, entryDestination->regValue);
}

/*
 *----------------------------------------------------------------------
 *
 * IOAPICReadEntry --
 *
 *      Read a redirection entry in the IOAPIC.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Entry is stored in the out argument
 *
 *----------------------------------------------------------------------
 */
static INLINE void
IOAPICReadEntry(IOAPIC      *ioapic,    // IN: the ioapic to read from
                int          entryNum,  // IN: the number of the entry  
                IOAPICEntry *entry)     // OUT: the entry itself
{
   IOAPICReadEntrySettings(ioapic, entryNum, &entry->settings);
   IOAPICReadEntryDestination(ioapic, entryNum, &entry->destination);
}

/*
 *----------------------------------------------------------------------
 *
 * IOAPICWriteEntry --
 *
 *      Write a redirection entry in the IOAPIC.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Entry is written
 *
 *----------------------------------------------------------------------
 */
static INLINE void
IOAPICWriteEntry(IOAPIC      *ioapic,    // IN: the ioapic to read from
                 int          entryNum,  // IN: the number of the entry  
                 IOAPICEntry *entry)     // IN: the entry itself
{
   IOAPICWriteEntrySettings(ioapic, entryNum, &entry->settings);
   IOAPICWriteEntryDestination(ioapic, entryNum, &entry->destination);
}


/*
 *----------------------------------------------------------------------
 *
 * IOAPICInit --
 *
 *      Initialize the ioapic module.
 *
 * Results:
 *      VMK_OK if successful
 *
 * Side effects:
 *      PIC, APIC and IOAPIC are initialized.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
IOAPICInit(ICType hostICType,		 // IN: the type of IC used by COS
	   VMnix_ConfigOptions *vmnixOptions, // IN: the options from vmnix
	   VMnix_SharedData *sharedData, // OUT: data to be shared with vmnix
	   Chipset_SysInfo *sysInfo)	 // IN: data from sysinfo tables
{
   VMK_ReturnStatus status;
   Chipset_IOAPICInfo *csIOAPIC;
   IOAPIC *ioapic;
   VA vAddr;
   uint32 reg;
   IOAPICEntry masked;
   int i;
   int id;
   int actualId;
   uint8 ioapicVer = 0;
   
   /*
    * We reinitialize everything but we keep a copy of the current
    * state to restore to later.
    */

   status = APIC_Init(hostICType, vmnixOptions, sharedData);
   if (status != VMK_OK) {
      return status;
   }

   SP_InitLockIRQ("ioapicLck", &ioapicLock, SP_RANK_IRQ_LEAF);
   SHARED_DATA_ADD(sharedData->ioapicLock, volatile uint32 *, SP_GetLockAddrIRQ(&ioapicLock));

   for (i=0; i<MAX_IOAPICS; i++) {
      ioapicInfo[i].present = FALSE;
   }
      
   for (i=0; i<IDT_NUM_VECTORS; i++) {
      ioapicVectorInfo[i].assigned = FALSE;
   }
   
   for (id = 0; id < IOAPICID_RANGE; id++) {
      csIOAPIC = &sysInfo->ioapic[id];
      if (!csIOAPIC->present) {
	 continue;
      }

      ASSERT_NOT_IMPLEMENTED(csIOAPIC->num < MAX_IOAPICS);
      ioapic = &ioapicInfo[csIOAPIC->num];

      ASSERT(csIOAPIC->physAddr != 0);
      /* first map the registers */
      vAddr = (VA)KVMap_MapMPN(MA_2_MPN(csIOAPIC->physAddr), TLB_UNCACHED);
      if (!vAddr) {
         return VMK_NO_RESOURCES;
      }
      
      ASSERT(!(vAddr & PAGE_MASK));
      ioapic->reg = (ApicReg *)(vAddr | (csIOAPIC->physAddr & PAGE_MASK));

      /* everythings OK, fill in the vitals */

      Log("found %d (id %02d @ %08x)", csIOAPIC->num, id, csIOAPIC->physAddr);
      
      ioapic->id = id;
      ioapic->physAddr = csIOAPIC->physAddr;
      ioapic->present = TRUE;

      /* id must have been set in the hardware by COS */
      
      actualId = IOAPICReadReg(ioapic, IOAPICID) >> APIC_ICRHI_DEST_OFFSET;
      if (actualId != id) {
	 /*
	  * COS must have detected an ID conflict between IOAPICs and CPUs and
	  * found a way around.
	  *
	  * Note: We leave ioapic->id set to the original id because it is
	  * only used in log and it refers to the original MPS id which is
	  * easier to track.
	  */
	 SysAlert("ID used (%d) is not the one reported by MPS table",actualId);
      }

      /* read the version information */
      
      reg = IOAPICReadReg(ioapic, IOAPICVER);
      ioapic->version = reg & 0xff;
      ioapic->numEntries = ((reg >> 16) & 0xff)+1;
      ASSERT_NOT_IMPLEMENTED(ioapic->numEntries <= VMK_HW_MAX_PINS_PER_IC);

      // Warn if the ioapic versions differ
      if (!ioapicVer) {
         ioapicVer = ioapic->version;
      } else {
         if (ioapicVer != ioapic->version) {
            Warning("ioapic %d, version %d does not match ioapic0 version %d",
                    csIOAPIC->num, ioapic->version, ioapicVer);
         }
      }
      Log("version 0x%x, number of entries %d", 
          ioapic->version, ioapic->numEntries);
      
      /* mask all entries but keep a record if the host was using them */

      memset((char*)&masked, 0, sizeof(IOAPICEntry));
      masked.settings.regFields.mask = 1;
      
      ASSERT(hostICType == ICTYPE_IOAPIC);
      for (i=0; i<ioapic->numEntries; i++) {
         IOAPICReadEntry(ioapic, i, &ioapic->hostEntry[i]);
	 ASSERT(ioapic->hostEntry[i].settings.regFields.mask);
         IOAPICWriteEntry(ioapic, i, &masked);
      }
   }

   // Set the MPS_Signature
   // TODO: Clearly the wrong place to set MPS_Signature
   //       But the whole idea of MPS_Signature needs
   //       to be re-evaluated
   if (ioapicVer == 0x11 &&
       cpuType == CPUTYPE_INTEL_P6) {
      Log("resolved as P3_IOAPIC_0X11");
      MPS_Signature = P3_IOAPIC_0X11;
   } else if (ioapicVer == 0x13 &&
       cpuType == CPUTYPE_INTEL_P6) {
      Log("resolved as P3_IOAPIC_0X13");
      MPS_Signature = P3_IOAPIC_0X13;
   }

   /*
    * Check that there is no hole in the IOAPIC Nums.
    */
   for (i = 0; i < MAX_IOAPICS; i++) {
      if (!ioapicInfo[i].present) {
	 break;
      }
   }
   for (;i < MAX_IOAPICS; i++) {
      ASSERT(!ioapicInfo[i].present);
      // For release builds which don't have ASSERT compiled in
      if (ioapicInfo[i].present) {
         SysAlert("IOAPIC Num %d is missing - check BIOS settings", i-1);
	 break;
      }
   }

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * IOAPICRestoreHostSetup --
 *
 *      Restore the IOAPIC state for COS before unloading the vmkernel.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      APIC and IOAPIC are set up for COS.
 *
 *----------------------------------------------------------------------
 */
static void
IOAPICRestoreHostSetup(void)
{
   IOAPICEntry entry;
   IOAPIC *ioapic;
   int i;
   int pin;

   memset((char*)&entry, 0, sizeof(IOAPICEntry));
   entry.settings.regFields.mask = 1;

   /*
    * The COS IOAPIC setup was completely overwritten but we kept a copy,
    * so we simply need to restore it.
    *
    * NOTE: There may have been interrupts in flight whose vectors are
    * already posted in the CPU. The host may warn it has received a
    * spurious interrupt for an unknown vector.
    *
    * NOTE: See PR 20628.
    * When a device sends an edge interrupt, the IOAPIC delivers it to the
    * APIC and forgets it (the APIC EOI does not notify the IOAPIC back).
    * When a device sends a level interrupt, the process is more complicated
    * since the level is continuous. The IOAPIC delivers the vector to the
    * APIC and remembers it has done so, ignoring the level from now on
    * (virtual masking). The APIC delivers the vector to the CPU. Eventually
    * the CPU issues an APIC EOI. The APIC keeps track of the vector it last
    * sent and notifies the IOAPIC back that the vector was processed. The
    * IOAPIC then checks which pin has that vector and removes the virtual
    * masking. If the level is still present, the vector is again delivered
    * and so on.
    * It is easy to see that for that scheme to work, vectors have to be
    * immutable for an active pin, otherwise when the APIC EOI comes back
    * with a vector that is no longer the one programmed for the pin, the
    * IOAPIC won't match it and won't remove the virtual masking and the pin
    * is basically hung.
    * Since we are changing all vectors when restoring the host setup we
    * can therefore end up with hung pins. There is no official way to reset
    * an IOAPIC pin but it seems that zeroing out the entry does it.
    */

   // Reset the IOAPICs
   for (i = 0; i < MAX_IOAPICS; i++) {
      ioapic = &ioapicInfo[i];
      if (!ioapic->present) {
         break;
      }
      for (pin = 0; pin < ioapic->numEntries; pin++) {
         IOAPICWriteEntry(ioapic, pin, &entry);
      }
   }

   // Restore host values into the IOAPICs
   for (i = 0; i < MAX_IOAPICS; i++) {
      ioapic = &ioapicInfo[i];
      if (!ioapic->present) {
	 break;
      }
      for (pin = 0; pin < ioapic->numEntries; pin++) {
	 int dest;

	 entry = ioapic->hostEntry[pin];

	 /*
	  * COS uses flat logical, lowest-priority delivery mode with a
	  * target set of all CPUs. Even though we shut down the APs before
	  * returning to COS, the chipset may have cached target info and
	  * may send interrupts to now dormant APs. Modifying COS to restrict
	  * the target set to the BSP solves the issue in most cases (see PR
	  * 42410).
	  * However on IBM NUMA machines, it does not seem to be enough. It
	  * should be noted that the mode COS uses can only discriminate
	  * among 8 CPUs, so it is dubious to use that mode on such machines
	  * to begin with. Modifying COS to use physical, fixed delivery mode
	  * with BSP as target solves the issue (see PR 44198).
	  *
	  * Since as long as no APs were ever started there is no problem,
	  * COS is fine before vmkernel is loaded. It's also fine when vmkernel
	  * is running since vmkernel handles the interrupts. It's only when
	  * vmkernel is unloaded that COS can fail. So we can avoid modifying
	  * COS itself and instead restore here what we need as we know that
	  * COS never looks back on the IOAPICs once it has set them up.
	  */
	 entry.settings.regFields.deliveryMode = APIC_DELMODE_FIXED;
	 entry.settings.regFields.destMode = APIC_DESTMODE_PHYS;
	 dest = APIC_FindID(HOST_PCPU);
	 ASSERT_NOT_IMPLEMENTED(dest != -1);
	 entry.destination.regFields.destination = dest;

         IOAPICWriteEntry(ioapic, pin, &entry);
      }
   }

   APIC_RestoreHostSetup();
}



/*
 *----------------------------------------------------------------------
 *
 * IOAPIC_ResetPins --
 *
 *      Reset IOAPIC pins.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      IOAPIC pins are reset.
 *
 *----------------------------------------------------------------------
 */
void
IOAPIC_ResetPins(Bool levelOnly)
{
   IOAPICEntry masked;
   IOAPICEntry entry;
   IOAPIC *ioapic;
   int i;
   SP_IRQL prevIRQL;
   int pin;

   Warning("%s", levelOnly ? "level-triggered pins only" : "all pins");

   memset((char*)&masked, 0, sizeof(IOAPICEntry));
   masked.settings.regFields.mask = 1;

   for (i = 0; i < MAX_IOAPICS; i++) {
      ioapic = &ioapicInfo[i];
      if (!ioapic->present) {
         break;
      }
      prevIRQL = SP_LockIRQ(&ioapicLock, SP_IRQL_KERNEL);
      for (pin = 0; pin < ioapic->numEntries; pin++) {
	 // save current entry content
	 IOAPICReadEntry(ioapic, pin, &entry);
	 if (levelOnly && entry.settings.regFields.trigger==APIC_TRIGGER_EDGE) {
	    continue;
	 }
	 // reset entry
         IOAPICWriteEntry(ioapic, pin, &masked);
	 // sanitize saved entry content (paranoid as they are R/O fields)
	 entry.settings.regFields.deliveryStats = 0;
	 entry.settings.regFields.remoteIRR = 0;
	 entry.settings.regFields.reserved = 0;
	 entry.destination.regFields.reserved = 0;
	 // load saved entry content back
	 IOAPICWriteEntry(ioapic, pin, &entry);
      }
      SP_UnlockIRQ(&ioapicLock, prevIRQL);
   }

}


/*
 *----------------------------------------------------------------------
 *
 * IOAPICAllocateVector --
 *
 *      Allocate the next available vector for a device interrupt
 *      (max of 2 vectors per priority group if possible to work around
 *      a pentium III limitation).
 *
 * Results:
 *      vector if successful, 0 otherwise
 *
 * Side effects:
 *      none
 *
 *----------------------------------------------------------------------
 */
static uint32
IOAPICAllocateVector(void)
{
#define NUM_VECTORS_PER_PRIORITY 16

   static int lastVector = -1;
   static int offset = 1;	// x.0 and x.8 are used by monitor, see idt.h

   if (lastVector == -1) {
      lastVector = IDT_FIRST_EXTERNAL_VECTOR + offset;
   } else {
      lastVector += NUM_VECTORS_PER_PRIORITY/2;
   }

   while (offset < NUM_VECTORS_PER_PRIORITY/2) {
      if (lastVector < IDT_LAST_DEVICE_VECTOR) {//IDT_LAST_DEVICE_VECTOR is used
	 Log("0x%x", lastVector);
	 ASSERT((lastVector & IDT_MONITOR_VECTOR_MASK) != 0);
         return lastVector;
      }
      offset++;
      lastVector = IDT_FIRST_EXTERNAL_VECTOR + offset;
   }

   SysAlert("Out of interrupt vectors");
   return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * IOAPICMapBusIRQ --
 *
 *      Map the chipset interrupt to a vector. 
 *
 * Results:
 *      On success: sets *vector and returns true
 *      On failure: returns false
 *
 * Side effects:
 *      The IOAPIC redirection table entry is programmed
 *
 *----------------------------------------------------------------------
 */
static Bool
IOAPICMapBusIRQ(Chipset_BusIRQInfo *busIRQInfo, uint32 *vector)
{
   IOAPICEntry entry;
   IOAPIC *ioapic;
   SP_IRQL prevIRQL;
   Bool ok;
   uint32 dest;
   uint32 destMode;


   ASSERT(busIRQInfo->ic < VMK_HW_MAX_ICS);
   ioapic = &ioapicInfo[busIRQInfo->ic];
   ASSERT(ioapic->present);

   if (busIRQInfo->pin >= ioapic->numEntries) {
      // happens on HP LP-1000/2000r
      Warning("intIn (%d) >= ioapic entries (%d)",
              busIRQInfo->pin, ioapic->numEntries);
      return FALSE;
   }

   prevIRQL = SP_LockIRQ(&ioapicLock, SP_IRQL_KERNEL);

   /*
    * If the entry is already initialized, it means 2 devices share the
    * same interrupt line.
    */
   IOAPICReadEntrySettings(ioapic, busIRQInfo->pin, &entry.settings);
   if (entry.settings.regFields.vector) {
      int trigger = (entry.settings.regFields.trigger == APIC_TRIGGER_EDGE ? 
                     VMK_HW_INT_EDGE : VMK_HW_INT_LEVEL);
      int polarity = (entry.settings.regFields.polarity == APIC_POLARITY_HIGH ?
                      VMK_HW_INT_ACTIVE_HIGH : VMK_HW_INT_ACTIVE_LOW);
      // Check the following
      //    1. If the existing entry is 'edge' the new one is also 'edge'
      //    2. If the existing entry is 'level' the new one is also 'level'
      //    3. Also make sure that the polarities match
      ASSERT(trigger == busIRQInfo->trigger);
      ASSERT(polarity == busIRQInfo->polarity);

      if ((trigger != busIRQInfo->trigger) ||
          (polarity != busIRQInfo->polarity)) {
         Warning("conflicting types for ioapic %d, pin %d "
                 "current trigger = %d, new trigger = %d, "
                 "current polarity = %d, new polarity = %d", 
                 busIRQInfo->ic, busIRQInfo->pin, trigger, busIRQInfo->trigger,
                 polarity, busIRQInfo->polarity);
         SP_UnlockIRQ(&ioapicLock, prevIRQL);
         return FALSE;
      }
      SP_UnlockIRQ(&ioapicLock, prevIRQL);
      *vector = entry.settings.regFields.vector;
      return TRUE;
   }

   /* get a vector */
   *vector = IOAPICAllocateVector();
   if (*vector == 0) {
      Warning("failed to allocate vector");
      SP_UnlockIRQ(&ioapicLock, prevIRQL);
      return FALSE;
   }
   
   /* record mapping */
   Log("vector 0x%x to %02d-%02d",
			*vector, busIRQInfo->ic, busIRQInfo->pin);
   ASSERT(!ioapicVectorInfo[*vector].assigned);

   /*
    * Initialize ioapic entry, making sure to zero read-only bits 
    */
   memset((char*)&entry, 0, sizeof(IOAPICEntry));

   /* interrupt vector */
   entry.settings.regFields.vector = *vector;

   /* Active high or active low */
   if (busIRQInfo->polarity == VMK_HW_INT_ACTIVE_HIGH) {
      entry.settings.regFields.polarity = APIC_POLARITY_HIGH;
   } else if (busIRQInfo->polarity == VMK_HW_INT_ACTIVE_LOW) {
      entry.settings.regFields.polarity = APIC_POLARITY_LOW;
   } else {
      Warning("bad polarity %d", busIRQInfo->polarity);
      SP_UnlockIRQ(&ioapicLock, prevIRQL);
      return FALSE;
   }

   /* Level or edge triggered */
   if (busIRQInfo->trigger == VMK_HW_INT_EDGE) {
      entry.settings.regFields.trigger = APIC_TRIGGER_EDGE;
   } else if (busIRQInfo->trigger == VMK_HW_INT_LEVEL) {
      entry.settings.regFields.trigger = APIC_TRIGGER_LEVEL;
   } else {
      Warning("bad trigger %d", busIRQInfo->trigger);
      SP_UnlockIRQ(&ioapicLock, prevIRQL);
      return FALSE;
   }

   /* deliver interrupt to the BSP, i.e. HOST_PCPU by default */
   entry.settings.regFields.deliveryMode = APIC_DELMODE_FIXED;
   ok = APIC_GetDestInfo(HOST_PCPU, &dest, &destMode);
   ASSERT_NOT_IMPLEMENTED(ok);
   entry.settings.regFields.destMode = destMode;
   entry.destination.regFields.destination = dest;

   /* need to mask until all APs are booted, per Intel Book 3 */
   entry.settings.regFields.mask = 1;


   IOAPICWriteEntry(ioapic, busIRQInfo->pin, &entry);

   ioapicVectorInfo[*vector].assigned = TRUE;
   ioapicVectorInfo[*vector].ioapic = ioapic;
   ioapicVectorInfo[*vector].entryNum = busIRQInfo->pin;

   SP_UnlockIRQ(&ioapicLock, prevIRQL);

   return TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * IOAPICSteerVector --
 *
 *      Changes the destination of a given vector
 *
 * Results:
 *      Success
 *
 * Side effects:
 *      The IOAPIC
 *
 *----------------------------------------------------------------------
 */
static Bool
IOAPICSteerVector(uint32 vector, uint32 pcpuNum)
{
   IOAPIC *ioapic;
   SP_IRQL prevIRQL;
   int entryNum;
   DEBUG_ONLY(IOAPICEntrySettings entrySettings);
   DEBUG_ONLY(IOAPICEntryDestination oldEntryDestination);
   IOAPICEntryDestination entryDestination;
   Bool ok;
   uint32 dest;
   uint32 destMode;

   if (!ioapicVectorInfo[vector].assigned) {
      return FALSE;
   }
   ioapic = ioapicVectorInfo[vector].ioapic;
   entryNum = ioapicVectorInfo[vector].entryNum;

   // Get the destination based on pcpu.
   ok = APIC_GetDestInfo(pcpuNum, &dest, &destMode);
   ASSERT_NOT_IMPLEMENTED(ok);

   prevIRQL = SP_LockIRQ(&ioapicLock, SP_IRQL_KERNEL);

   /*
    * Note that since destMode does not change over the life of the system,
    * we do not need to update it in the entry.
    */
   DEBUG_ONLY(IOAPICReadEntrySettings(ioapic, entryNum, &entrySettings));
   DEBUG_ONLY(ASSERT(entrySettings.regFields.destMode == destMode));

   DEBUG_ONLY(IOAPICReadEntryDestination(ioapic,entryNum,&oldEntryDestination));
   DEBUG_ONLY(ASSERT(oldEntryDestination.regFields.reserved == 0));

   entryDestination.regValue = 0;
   entryDestination.regFields.destination = dest;
   IOAPICWriteEntryDestination(ioapic, entryNum, &entryDestination);

   SP_UnlockIRQ(&ioapicLock, prevIRQL);

   LOG(1, "changed destination for vector 0x%x to %d (oldreg 0x%x, reg 0x%x)",
      vector, pcpuNum, oldEntryDestination.regValue, entryDestination.regValue);

   return TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * IOAPICDoMaskVector --
 *
 *      Mask the vector (maybe virtually only). Using force causes it to be
 *      masked no matter what.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The vector is masked in the IOAPIC unless it's for an edge-triggered
 *      interrupt and force has not been used in which case it is left
 *      unmasked.
 *
 *----------------------------------------------------------------------
 */
static void
IOAPICDoMaskVector(uint32 vector, Bool force)
{
   IOAPIC *ioapic;
   SP_IRQL prevIRQL;
   int entryNum;
   IOAPICEntrySettings entrySettings;

   if (!ioapicVectorInfo[vector].assigned) {
      return;
   }
   ioapic = ioapicVectorInfo[vector].ioapic;
   entryNum = ioapicVectorInfo[vector].entryNum;

   prevIRQL = SP_LockIRQ(&ioapicLock, SP_IRQL_KERNEL);

   IOAPICReadEntrySettings(ioapic, entryNum, &entrySettings);

   // entries are set up once so it's safe to rely on their content
   if (!force && (entrySettings.regFields.trigger == APIC_TRIGGER_EDGE)) {

      /*
       * IOAPIC does not latch edge-triggered interrupts happening while
       * masked so we cannot mask for fear of losing one interrupt. We leave
       * it unmasked and if it occurs, we'll catch it in IOAPICSpurious()
       * and mask it then. So if an edge-triggered interrupt is masked, this
       * is the clue that it happened, this serves as latch.
       */

   } else {

      entrySettings.regFields.mask = 1;
      IOAPICWriteEntrySettings(ioapic, entryNum, &entrySettings);

   }

   SP_UnlockIRQ(&ioapicLock, prevIRQL);
}


/*
 *----------------------------------------------------------------------
 *
 * IOAPICMaskVector --
 *
 * 	Mask the vector (maybe virtually only).
 *
 * Results:
 * 	None.
 *
 * Side Effects:
 * 	The vector is masked in the IOAPIC unless it's for an edge-triggered
 * 	interrupt in which case it is left unmasked.
 *
 *----------------------------------------------------------------------
 */
static void
IOAPICMaskVector(uint32 vector)
{
   IOAPICDoMaskVector(vector, FALSE);
}


/*
 *----------------------------------------------------------------------
 *
 * IOAPICMaskAll --
 *
 * 	Mask all vectors.
 *
 * Results:
 * 	None.
 *
 * Side effects:
 * 	All vectors are masked in the IOAPIC.
 * 	Be careful that this function may cause phantom edge-triggered
 * 	interrupts when corresponding vectors are unmasked.
 * 
 *----------------------------------------------------------------------
 */
static void
IOAPICMaskAll(void)
{
   int vector;

   for (vector=IDT_FIRST_EXTERNAL_VECTOR; vector < IDT_NUM_VECTORS; vector++) {
      if (ioapicVectorInfo[vector].assigned) {
         IOAPICDoMaskVector(vector, TRUE);
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * IOAPICUnmaskVector --
 *
 *      Unmask the vector.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The vector is unmasked in the IOAPIC. If it is for an edge-triggered
 *      interrupt and the vector was masked, the interrupt is retriggered
 *      as mask serves as a latch.
 *
 *----------------------------------------------------------------------
 */
static void
IOAPICUnmaskVector(uint32 vector)
{
   IOAPIC *ioapic;
   SP_IRQL prevIRQL;
   int entryNum;
   IOAPICEntrySettings entrySettings;
   Bool retrigger = FALSE;

   if (!ioapicVectorInfo[vector].assigned) {
      return;
   }
   ioapic = ioapicVectorInfo[vector].ioapic;
   entryNum = ioapicVectorInfo[vector].entryNum;

   prevIRQL = SP_LockIRQ(&ioapicLock, SP_IRQL_KERNEL);

   IOAPICReadEntrySettings(ioapic, entryNum, &entrySettings);

   // entries are set up once so it's safe to rely on their content
   if ((entrySettings.regFields.trigger == APIC_TRIGGER_EDGE) &&
		   entrySettings.regFields.mask) {

      /*
       * Edge-triggered interrupts are only masked when an interrupt
       * occurs while masking was deferred. We need to retrigger it.
       */
      retrigger = TRUE;

   }

   entrySettings.regFields.mask = 0;
   IOAPICWriteEntrySettings(ioapic, entryNum, &entrySettings);

   SP_UnlockIRQ(&ioapicLock, prevIRQL);

   if (retrigger) {
      // Self interrupts are fortuitously sent as edge-triggered interrupts
      Log("0x%x retriggerred", vector);
      APIC_SelfInterrupt(vector);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * IOAPICDump --
 *
 *      Output the state of the IOAPIC, PIC and APIC to the log or to a
 *      proc node if buffer is not NULL
 *
 * Results:
 *      none
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

// Format description:
// pin number
// vector
// delivery mode (should be 0)
// 	0 fixed (always to destination)
// 	1 lowest priority
// 	2 SMI
// 	4 NMI
// 	5 INIT
// 	7 ExtINT
// destination mode (should be 0)
// 	0 physical (destination is APIC ID)
// 	1 logical (destination is LDR)
// polarity (should be 0 for ISA, 1 for PCI)
// 	0 high
// 	1 low
// trigger (should be 0 for ISA, 1 for PCI)
// 	0 edge
// 	1 level
// status (should not stay at 1)
// 	0 idle
// 	1 in the process of delivering to a local APIC
// remote IRR (should not stay at 1)
// 	0 not posted
// 	1 posted in the IRR of a local APIC
// destination (should be BSP except for vmkernel NICs)
// mask (should not stay at 1 except for unused interrupts)
// 	0 unmasked
// 	1 masked
// content of the settings half of the entry
//
#define IOAPIC_ENTRY_FMT \
"%2d [vec 0x%02x delm %d dstm %d pol %d trg %d stat %d remIRR %d dst 0x%02x mask %d] %08X"

static void
IOAPICDump(char *buffer, int *len)
{
   IOAPIC *ioapic;   
   int i, j;
   SP_IRQL prevIRQL;
   Bool acq;
   IOAPICEntry entry;


   if (buffer) {
      Proc_Printf(buffer, len, "IOAPIC interrupt state:\n");
   }

   prevIRQL = SP_TryLockIRQ(&ioapicLock, SP_IRQL_KERNEL, &acq);
   if (acq) {
      for (i = 0; i < MAX_IOAPICS; i++) {
	 ioapic = &ioapicInfo[i];
	 if (!ioapic->present) {
	    break;
	 }
	 if (buffer) {
	    Proc_Printf(buffer, len, "IOAPICId %d:\n", ioapic->id);
	 } else {
	    Log("IOAPICId %d:", ioapic->id);
	 }
	 for (j = 0; j < ioapic->numEntries; j++) {
	    IOAPICReadEntry(ioapic, j, &entry);
	    if (entry.settings.regFields.vector) {
	       if (buffer) {
		  Proc_Printf(buffer, len, IOAPIC_ENTRY_FMT "\n",
			j,
			entry.settings.regFields.vector,
			entry.settings.regFields.deliveryMode,
			entry.settings.regFields.destMode,
			entry.settings.regFields.polarity,
			entry.settings.regFields.trigger,
			entry.settings.regFields.deliveryStats,
			entry.settings.regFields.remoteIRR,
			entry.destination.regFields.destination,
			entry.settings.regFields.mask,
			entry.settings.regValue);
	       } else {
	          Log(IOAPIC_ENTRY_FMT,
			j,
			entry.settings.regFields.vector,
			entry.settings.regFields.deliveryMode,
			entry.settings.regFields.destMode,
			entry.settings.regFields.polarity,
			entry.settings.regFields.trigger,
			entry.settings.regFields.deliveryStats,
			entry.settings.regFields.remoteIRR,
			entry.destination.regFields.destination,
			entry.settings.regFields.mask,
			entry.settings.regValue);
	       }
	    }
	 }      
      }
      SP_UnlockIRQ(&ioapicLock, prevIRQL);
   }

   APIC_Dump(buffer, len);
}


/*
 *----------------------------------------------------------------------
 *
 * IOAPICHookupBusIRQ --
 *
 *      Get a vector and set the IOAPIC to hookup that vector to the bus IRQ
 *
 * Results:
 *      Success
 *
 * Side effects:
 *      *edge is TRUE if the interrupt is edge-triggered, FALSE otherwise
 *      *irq contains the irq as understood by COS
 *      *vector contains the allocated vector
 *
 *----------------------------------------------------------------------
 */
static Bool
IOAPICHookupBusIRQ(int busType, int busID, int busIRQ, IRQ isaIRQ,
		   Bool *edge, IRQ *cosIRQ, uint32 *vector)
{
   Chipset_BusIRQInfo busIRQInfo;

   ASSERT(busType == VMK_HW_BUSTYPE_PCI || busIRQ == isaIRQ);

   Chipset_GetBusIRQInfo(busType, busID, busIRQ, &busIRQInfo);
   if (!busIRQInfo.present) {
      return FALSE;
   }

   ASSERT(busIRQInfo.ic < VMK_HW_MAX_ICS && busIRQInfo.pin < VMK_HW_MAX_PINS_PER_IC);
   *cosIRQ = Chipset_IrqFromPin[busIRQInfo.ic][busIRQInfo.pin];
   if (*cosIRQ == PCI_IRQ_NONE && busType == VMK_HW_BUSTYPE_ISA) {
      // ignore ISA irqs not used by the console os
      return FALSE;
   }
   *edge = (busIRQInfo.trigger == VMK_HW_INT_EDGE);

   return IOAPICMapBusIRQ(&busIRQInfo, vector);
}


/*
 *----------------------------------------------------------------------
 *
 * IOAPICMaskAndAckVector --
 *
 *      Mask and ack the vector's IRQ for the IOAPIC.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The vector's IRQ is acked and masked.
 *
 *----------------------------------------------------------------------
 */
static void
IOAPICMaskAndAckVector(uint32 vector)
{
   IOAPICDoMaskVector(vector, FALSE);
   APIC_AckVector(vector);   
}


/*
 *----------------------------------------------------------------------
 *
 * IOAPICAckVector --
 *
 *      Ack the vector's IRQ for the IOAPIC.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The vector's IRQ is acked.
 *
 *----------------------------------------------------------------------
 */
static void
IOAPICAckVector(uint32 vector)
{
   APIC_AckVector(vector);   
}


/*
 *----------------------------------------------------------------------
 *
 * IOAPICGetInServiceLocally --
 *
 * 	Get the currently in service vector if any on the current pcpu.
 *
 * Results:
 * 	TRUE if a vector was in service.
 *
 * Side Effects:
 * 	vector is returned.
 *
 *----------------------------------------------------------------------
 */
static Bool
IOAPICGetInServiceLocally(uint32 *vector)
{
   return APIC_GetInServiceVector(vector);
}


/*
 *----------------------------------------------------------------------
 *
 * IOAPICPosted --
 *
 * 	Check if a vector has been posted by IOAPIC to a pcpu.
 *
 * Results:
 * 	TRUE if posted
 *
 * Side Effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
static Bool
IOAPICPosted(uint32 vector)
{
   IOAPIC *ioapic;
   SP_IRQL prevIRQL;
   int entryNum;
   IOAPICEntrySettings entrySettings;

   if (!ioapicVectorInfo[vector].assigned) {
      return FALSE;
   }
   ioapic = ioapicVectorInfo[vector].ioapic;
   entryNum = ioapicVectorInfo[vector].entryNum;
   
   prevIRQL = SP_LockIRQ(&ioapicLock, SP_IRQL_KERNEL);

   IOAPICReadEntrySettings(ioapic, entryNum, &entrySettings);

   SP_UnlockIRQ(&ioapicLock, prevIRQL);
   
   // Remote IRR set
   return entrySettings.regFields.remoteIRR != 0;
}


/*
 *----------------------------------------------------------------------
 *
 * IOAPICPendingLocally --
 *
 * 	Check if a vector is waiting to be serviced on the cuurent pcpu
 *
 * Results:
 * 	TRUE if pending
 *
 * Side Effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
static Bool
IOAPICPendingLocally(uint32 vector)
{
   return APIC_IsPendingVector(vector);
}


/*
 *----------------------------------------------------------------------
 *
 * IOAPICSpurious --
 *
 * 	Check if a vector just received is due to a spurious interrupt,
 * 	i.e. can be safely ignored.
 *
 * Results:
 * 	TRUE if spurious
 *
 * Side Effects:
 * 	If spurious, the vector will be masked
 *
 *----------------------------------------------------------------------
 */
static Bool
IOAPICSpurious(uint32 vector)
{
   IOAPIC *ioapic;
   SP_IRQL prevIRQL;
   int entryNum;
   IOAPICEntrySettings entrySettings;
   Bool spurious = FALSE;

   /*
    * IOAPIC does not generate spurious interrupts but we may have
    * deferred masking for an edge-triggered interrupt.
    */

   if (!ioapicVectorInfo[vector].assigned) {
      return FALSE;
   }
   ioapic = ioapicVectorInfo[vector].ioapic;
   entryNum = ioapicVectorInfo[vector].entryNum;

   prevIRQL = SP_LockIRQ(&ioapicLock, SP_IRQL_KERNEL);

   IOAPICReadEntrySettings(ioapic, entryNum, &entrySettings);

   // entries are set up once so it's safe to rely on their content
   if (entrySettings.regFields.trigger == APIC_TRIGGER_EDGE) {

      /*
       * If we get an edge-triggered interrupt and it's checked for
       * spuriousness then its masking must have been deferred. So
       * it's legitimate and we need to mask it for good now.
       */
      spurious = TRUE;
      entrySettings.regFields.mask = 1;
      IOAPICWriteEntrySettings(ioapic, entryNum, &entrySettings);

   }

   SP_UnlockIRQ(&ioapicLock, prevIRQL);

   return spurious;
}


/*
 *----------------------------------------------------------------------
 *
 * IOAPICGoodTrigger --
 *
 * 	Check if a vector just received was triggered in the expected way.
 *
 * Results:
 * 	TRUE if triggered as expected
 *
 * Side Effects:
 * 	None
 *
 *----------------------------------------------------------------------
 */
static Bool
IOAPICGoodTrigger(uint32 vector, Bool edge)
{
   uint32 tmr;

   /* 
    * When the IOAPIC delivers a vector, its trigger mode is recorded
    * in the TMR register of the local APIC. If the corresponding bit is set,
    * the trigger was a level, otherwise it was an edge.
    */
   tmr = apic->reg[APICR_TMR+vector/0x20][0];
   if (tmr & (1 << (vector & 0x1F))) {
      return !edge;
   } else {
      return edge;
   }
}


/*
 * Setup function pointers
 */
Chipset_ICFunctions IOAPIC_Functions = {
   IOAPICMaskAndAckVector,
   IOAPICUnmaskVector,
   IOAPICMaskVector,
   IOAPICAckVector,
   IOAPICGetInServiceLocally,
   IOAPICRestoreHostSetup,
   IOAPICSteerVector,
   IOAPICMaskAll,
   IOAPICDump,
   IOAPICPosted,
   IOAPICPendingLocally,
   IOAPICSpurious,
   IOAPICGoodTrigger,
};

Chipset_ICFunctions_Internal IOAPIC_Functions_Internal = {
   IOAPICInit,
   IOAPICHookupBusIRQ,
};
