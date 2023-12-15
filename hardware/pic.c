/* **********************************************************
 * Copyright 2002 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * pic.c --
 *
 *	This module manages the pic.
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "vmkernel.h"
#include "util.h"
#include "apic_int.h"
#include "idt.h"
#include "pic.h"
#include "proc.h"
#include "host_dist.h"
#include "hardware_public.h"
#include "pci_dist.h"
#include "splock.h"

#define LOGLEVEL_MODULE PIC
#define LOGLEVEL_MODULE_LEN 3
#include "log.h"

static Bool hookedUp[NUM_ISA_IRQS];
static uint32 cachedIRQMask;


#define __byte(x,y) 	(((unsigned char *)&(y))[x])
#define cached21	(__byte(0,cachedIRQMask))
#define cachedA1	(__byte(1,cachedIRQMask))

static SP_SpinLockIRQ picLock;

static void PICDump(char *buffer, int *len);



/*
 *----------------------------------------------------------------------
 *
 * PICIRQToVector --
 *
 * 	Gives out the equivalent vector of an IRQ
 *
 * Results:
 * 	vector
 *
 * Side Effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
static inline uint32
PICIRQToVector(IRQ irq) {
   ASSERT(irq >= 0 && irq < NUM_ISA_IRQS);
   return irq+IDT_FIRST_EXTERNAL_VECTOR;
}

/*
 *----------------------------------------------------------------------
 *
 * PICVectorToIRQ --
 *
 * 	Gives out the equivalent IRQ of a vector
 *
 * Results:
 * 	IRQ
 *
 * Side Effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
static inline IRQ
PICVectorToIRQ(uint32 vector) {
   IRQ irq = vector-IDT_FIRST_EXTERNAL_VECTOR;
   if (irq >= 0 && irq < NUM_ISA_IRQS) {
      return irq;
   } else {
      return -1;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * PICMaskVector --
 *
 *      Mask the vector's IRQ in the PIC.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The vector's IRQ is masked in the PIC.
 *
 *----------------------------------------------------------------------
 */
static void
PICMaskVector(uint32 vector)
{
   IRQ irq = PICVectorToIRQ(vector);

   if (irq == -1) {
      return;
   }

   cachedIRQMask |= 1 << irq;
   if (irq & 0x8) {
      OUTB(0xA1, cachedA1);
   } else {
      OUTB(0x21, cached21);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * PICMaskAll --
 *
 *      Mask all IRQs in the PIC.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      All IRQs are masked in the PIC.
 *
 *----------------------------------------------------------------------
 */
static void
PICMaskAll(void)
{
   // The cascade IRQ must stay unmasked.
   cachedIRQMask = 0xffff & ~(1 << CASCADE_IRQ);
   OUTB(0x21, cached21);
   OUTB(0xA1, cachedA1);
}


/*
 *----------------------------------------------------------------------
 *
 * PICUnmaskVector --
 *
 *      Unmask the vector's IRQ in the PIC.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The vector's IRQ is unmasked in the PIC.
 *
 *----------------------------------------------------------------------
 */
static void
PICUnmaskVector(uint32 vector)
{
   IRQ irq = PICVectorToIRQ(vector);

   if (irq == -1) {
      return;
   }

   cachedIRQMask &= ~(1 << irq);
   if (irq & 0x8) {
      OUTB(0xA1, cachedA1);
   } else {
      OUTB(0x21, cached21);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * PICAckVector --
 *
 *      Ack the vector's IRQ for the PIC.
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
PICAckVector(uint32 vector)
{ 
   IRQ irq = PICVectorToIRQ(vector);

   if (irq == -1) {
      // This must be an APIC interrupt
      APIC_AckVector(vector);
      return;
   }

   if (irq & 0x8) {
      OUTB(0x20, 0x62);
      OUTB(0xA0, 0x20);
   } else {
      OUTB(0x20, 0x20);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * PICMaskAndAckVector --
 *
 *      Mask and ack the vector's IRQ for the PIC.
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
PICMaskAndAckVector(uint32 vector)
{
   IRQ irq = PICVectorToIRQ(vector);

   if (irq == -1) {
      // This must be an APIC interrupt but we should never mask any
      ASSERT(FALSE);
      return;
   }

   cachedIRQMask |= 1 << irq;
   if (irq & 0x8) {
      INB(0xA1);
      OUTB(0xA1, cachedA1);
      OUTB(0x20, 0x62);
      OUTB(0xA0, 0x20);
   } else {
      INB(0x21);
      OUTB(0x21, cached21);
      OUTB(0x20, 0x20);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * PICGetInServiceLocally --
 *
 * 	Get the currently in service vector if any on the current pcpu.
 *
 * Results:
 * 	TRUE if a vector was in service.
 *
 * SideEffects:
 * 	vector is returned.
 * 
 *----------------------------------------------------------------------
 */
static Bool
PICGetInServiceLocally(uint32 *vector)
{
#define NUM_ISR	2
   Bool fromAPIC, fromPIC;
   char ISR[NUM_ISR];
   int i;
   IRQ irq[NUM_ISR];
   Bool multiple = FALSE;
   SP_IRQL prevIRQL;


   /*
    * Internal interrupts like APIC timer interrupts and IPIs are stored
    * in the APIC ISR but not PIC interrupts. We have to check both.
    */

   /* 
    * Get the ISR from the APIC.
    */
   fromAPIC = APIC_GetInServiceVector(vector);

   /*
    * Get the ISR from the PIC.
    *
    * NOTE: It's important to reset the ports to preserve the default PIC
    * setting of returning the IRR when reading from ports 0x20 or 0xA0.
    * We have to lock as this may be called from any pcpu.
    */
   prevIRQL = SP_LockIRQ(&picLock, SP_IRQL_KERNEL);
   OUTB(0x20, 0x0B);
   ISR[0] = INB(0x20);
   OUTB(0xA0, 0x0B);
   ISR[1] = INB(0xA0);

   OUTB(0x20, 0x0A);
   OUTB(0xA0, 0x0A);
   SP_UnlockIRQ(&picLock, prevIRQL);

   /*
    * Parse the ISR for the currently in service vector.
    * From highest priority to lowest prority: 0, 1, (2), 8, 9, 10, 11, 12,
    * 13, 14, 15, 3, 4, 5, 6, 7.
    * APIC has a higher priority than PIC.
    */
   for (i = 0; i < NUM_ISR; i++) {
      irq[i] = -1;
      if (ISR[i]) {
	 irq[i] = i*sizeof(ISR[0])*8;
	 while ((ISR[i] & 1) == 0) {
	    irq[i]++;
	    ISR[i] >>= 1;
	 }
	 if (ISR[i] != 1) {
	    multiple = TRUE;
	 }
      }
   }
   if (irq[0] != -1) {
      fromPIC = TRUE;
      if (fromAPIC) {
	 multiple = TRUE;
      } else if (irq[1] != -1) {
	 multiple = TRUE;
	 if (irq[0] < 2) {
	    *vector = PICIRQToVector(irq[0]);
 	 } else {
	    *vector = PICIRQToVector(irq[1]);
	 }
      } else {
	 *vector = PICIRQToVector(irq[0]);
      }
   } else if (irq[1] != -1) {
      fromPIC = TRUE;
      if (fromAPIC) {
	 multiple = TRUE;
      } else {
	 *vector = PICIRQToVector(irq[1]);
      }
   } else {
      fromPIC = FALSE;
   }

   if (multiple) {
      SysAlert("Several interrupts are in service at once");
      PICDump(NULL, NULL);
   }

   return (fromAPIC || fromPIC);
}


/*
 *----------------------------------------------------------------------
 *
 * PICSteerVector --
 *
 *      Remap the destination of the vector to be the callers pcpu.
 *      This is a noop with a PIC.
 *
 * Results:
 *      success
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static Bool
PICSteerVector(uint32 vector, uint32 pcpuNum)
{
   return (pcpuNum == HOST_PCPU);
}


/*
 *----------------------------------------------------------------------
 *
 * PICReinitialize --
 *
 *      Reinitialize the PIC
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      PIC is reset
 *
 *----------------------------------------------------------------------
 */
#ifdef notdef
static void
PICReinitialize(void)
{
   OUTB(0x21, 0xff);	/* mask all IRQs */
   OUTB(0xA1, 0xff);	/* mask all IRQs */

   OUTB(0x20, 0x11);	/* ICW1: select master 8259A */
   SLOW_DOWN_IO;
   OUTB(0x21, IDT_FIRST_EXTERNAL_VECTOR); /* ICW2: map IRQ0-IRQ7 to 0x20-0x27 */
   SLOW_DOWN_IO;
   OUTB(0x21, 0x04);	/* slave casaced onto IRQ2 */
   SLOW_DOWN_IO;
   OUTB(0x21, 0x01);	/* use normal EOI */
   SLOW_DOWN_IO;

   OUTB(0xA0, 0x11);	/* ICW1: select slave 8259A */
   SLOW_DOWN_IO;
   OUTB(0xA1, IDT_FIRST_EXTERNAL_VECTOR+8); /* ICW2: map IRQ8-15 to 0x28-0x2f */
   SLOW_DOWN_IO;
   OUTB(0xA1, 0x02);	/* slave cascaded onto master's IRQ2 */
   SLOW_DOWN_IO;
   OUTB(0xA1, 0x01);	/* use normal EOI */
   SLOW_DOWN_IO;

   Util_Udelay(100);	/* wait for 8259A to initialize */

   OUTB(0x21, cached21);/* restore original IRQ masks */
   OUTB(0xA1, cachedA1);
}
#endif


/*
 *----------------------------------------------------------------------
 *
 * PICInit --
 *
 *      Initialize the PIC.
 *
 * Results:
 *      VMK_OK if successful
 *
 * Side effects:
 *      PIC and APIC are initialized
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
PICInit(ICType hostICType,
	 VMnix_ConfigOptions *vmnixOptions,
	 VMnix_SharedData *sharedData,
	 Chipset_SysInfo *sysInfo)
{
   /*
    * The COS PIC setup is quasi identical to ours: The major difference is
    * 	- COS sets the PIC in AEOI mode when it uses an IOAPIC.
    * 
    * NOTE: Those assumptions need to be checked each time COS or the
    * vmkernel changes its way of using the PIC. In particular COS and vmkernel
    * use the same vectors for isaIRQs, but nothing should depend on it.
    *
    * NOTE: We could call PICReinitialize() to get to a known state.
    */

   SP_InitLockIRQ("picLck", &picLock, SP_RANK_IRQ_LEAF);

   PICMaskAll();

   SHARED_DATA_ADD(sharedData->cachedIRQMask, unsigned long *, &cachedIRQMask);

   return APIC_Init(hostICType, vmnixOptions, sharedData);
}


/*
 *----------------------------------------------------------------------
 *
 * PICRestoreHostSetup --
 *
 *      Restore the PIC state for COS before unloading the vmkernel.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      PIC and APIC are set up for COS.
 *
 *----------------------------------------------------------------------
 */
static void
PICRestoreHostSetup(void)
{
   /*
    * We did not change the PIC setup so there is nothing to undo for it.
    */

   APIC_RestoreHostSetup();
}


/*
 *----------------------------------------------------------------------
 *
 * PICHookupBusIRQ --
 *
 *      Get a vector and set the PIC to hookup that vector to the bus IRQ
 *
 * Results:
 * 	Success
 *
 * Side effects:
 *      *edge is TRUE if the interrupt is edge-triggered, FALSE otherwise
 *      *irq contains the irq as understood by COS
 *      *vector contains the allocated vector
 *
 *----------------------------------------------------------------------
 */
static Bool
PICHookupBusIRQ(int busType, int busID, int busIRQ, IRQ isaIRQ,
		Bool *edge, IRQ *cosIRQ, uint32 *vector)
{
   ASSERT(busType == VMK_HW_BUSTYPE_PCI || busIRQ == isaIRQ);

   if (isaIRQ < 0 || isaIRQ >= NUM_ISA_IRQS) {
      Warning("out of bound ISA IRQ %d", isaIRQ);
      return FALSE;
   }

   // PIC is always 0 and its pins == isaIRQs
   ASSERT(0 < VMK_HW_MAX_ICS && isaIRQ < VMK_HW_MAX_PINS_PER_IC);
   *cosIRQ = Chipset_IrqFromPin[0][isaIRQ];
   if (*cosIRQ == PCI_IRQ_NONE && busType == VMK_HW_BUSTYPE_ISA) {
      // ignore ISA irqs not used by the console os
      return FALSE;
   }
   *vector = PICIRQToVector(isaIRQ);

   if (Chipset_TriggerType(isaIRQ) == VMK_HW_INT_EDGE) {
      /*
       * If this interrupt is edge triggered it can be hooked up only once.
       */
      if (hookedUp[isaIRQ]) {
         Warning("edge triggered ISA irq %d can't be shared", isaIRQ);
         return FALSE;
      }
      *edge = TRUE;
   } else {
      *edge = FALSE;
   }

   /*
    * Vectors have been programmed for all isaIRQs already during PIC init,
    * nothing to do in the PIC redirection table.
    */

   hookedUp[isaIRQ] = TRUE;
   return TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * PICDump --
 *
 * 	Output the state of the PIC and APIC to the log or to a proc node
 * 	if buffer is not NULL
 *
 * Results:
 * 	None.
 *
 * Side effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
static void
PICDump(char *buffer, int *len)
{
   char IRR[2], ISR[2], IMR[2], ELCR[2];
   SP_IRQL prevIRQL;
   Bool acq;

   if (buffer) {
      Proc_Printf(buffer, len, "PIC interrupt state:\n");
   }

   ELCR[0] = INB(CHIPSET_ELCR_PORT);
   ELCR[1] = INB(CHIPSET_ELCR_PORT+1);

   // important to read the ISR before the IRR to preserve the default PIC
   // setting of returning the IRR when reading from ports 0x20 or 0xA0
   prevIRQL = SP_TryLockIRQ(&picLock, SP_IRQL_KERNEL, &acq);
   if (acq) {
      OUTB(0x20, 0x0B);
      ISR[0] = INB(0x20);
      OUTB(0xA0, 0x0B);
      ISR[1] = INB(0xA0);

      OUTB(0x20, 0x0A);
      OUTB(0xA0, 0x0A);
      SP_UnlockIRQ(&picLock, prevIRQL);
   } else {
      // cannot modify PIC so cannot read ISR
      ISR[0] = 0;
      ISR[1] = 0;
   }
   IRR[0] = INB(0x20);
   IRR[1] = INB(0xA0);

   IMR[0] = INB(0x21);
   IMR[1] = INB(0xA1);

   if (buffer) {
      if (!acq) {
	 Proc_Printf(buffer, len, "Couldn't read ISR\n");
      }
      Proc_Printf(buffer, len, "IRR=0x%x, ISR=0x%x, IMR=0x%x, ELCR=0x%x\n",
		  *((uint16 *)&IRR[0]), *((uint16 *)&ISR[0]),
		  *((uint16 *)&IMR[0]), *((uint16 *)&ELCR[0]));
      if (*((uint16 *)&IMR[0]) != cachedIRQMask) {
	 Proc_Printf(buffer, len, "cachedIRQMask=0x%x\n", cachedIRQMask);
      }
   } else {
      if (!acq) {
         Log("Couldn't read ISR");
      }
      Log("IRR=0x%x, ISR=0x%x, IMR=0x%x, ELCR=0x%x",
	  *((uint16 *)&IRR[0]), *((uint16 *)&ISR[0]),
	  *((uint16 *)&IMR[0]), *((uint16 *)&ELCR[0]));
      if (*((uint16 *)&IMR[0]) != cachedIRQMask) {
	 Warning("cachedIRQMask=0x%x", cachedIRQMask);
      }
   }

   APIC_Dump(buffer, len);
}


/*
 *----------------------------------------------------------------------
 *
 * PICPosted --
 *
 *      Check if a vector has been posted by PIC to a pcpu.
 *
 * Results:
 *      TRUE if posted
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static Bool
PICPosted(uint32 vector)
{
   IRQ irq = PICVectorToIRQ(vector);
   Bool posted;
   unsigned int mask;
   SP_IRQL prevIRQL;

   if (irq == -1) {
      // This must be an APIC interrupt but we should never check any
      ASSERT(FALSE);
      return FALSE;
   }
   mask = 1<<irq;

   /*
    * When a vector has been posted, its IRQ bit is set in the ISR.
    *
    * NOTE: It's important to reset the ports to preserve the default PIC
    * setting of returning the IRR when reading from ports 0x20 or 0xA0.
    * We have to lock as this may be called from any pcpu.
    */
   prevIRQL = SP_LockIRQ(&picLock, SP_IRQL_KERNEL);
   if (irq < 8) {
      OUTB(0x20, 0x0B);
      posted = (INB(0x20) & mask) != 0;
      OUTB(0x20, 0x0A);
   } else {
      OUTB(0xA0, 0x0B);
      posted = (INB(0xA0) & (mask >> 8)) != 0;
      OUTB(0xA0, 0x0A);
   }
   SP_UnlockIRQ(&picLock, prevIRQL);

   return posted;
}


/*
 *----------------------------------------------------------------------
 *
 * PICPendingLocally --
 *
 * 	Check if a vector is waiting to be serviced by the current pcpu
 *
 * Results:
 * 	TRUE if pending locally
 *
 * Side effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
static Bool
PICPendingLocally(uint32 vector)
{
   IRQ irq = PICVectorToIRQ(vector);

   if (irq == -1) {
      // This must be an APIC interrupt but we should never check any
      ASSERT(FALSE);
      return FALSE;
   }

   return FALSE; // vectors are not queued at the pcpu when using PIC
}


/*----------------------------------------------------------------------
 *
 * PICSpurious --
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
PICSpurious(uint32 vector)
{
   IRQ irq = PICVectorToIRQ(vector);

   // PIC may generate legitimate spurious interrupts on IRQ7 or IRQ15
   if ((irq == 7) || (irq == 15)) {
      static int count = 0; // warning throttle
      if ((count++ & 0x3FF) == 0) {
	 Log("%d spurious IRQ %d", count, irq);
      }
      PICMaskVector(vector);
      return TRUE;
   }
   return FALSE;
}


/*
 *----------------------------------------------------------------------
 *
 * PICGoodTrigger --
 *
 *      Check if a vector just received was triggered in the expected way.
 *
 * Results:
 *      TRUE if triggered as expected
 *
 * Side Effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
static Bool
PICGoodTrigger(uint32 vector, Bool edge)
{
   /*
    * The trigger type does not matter for PIC.
    */
   return TRUE;
}

/*
 * Setup function pointers
 */
Chipset_ICFunctions PIC_Functions = {
   PICMaskAndAckVector,
   PICUnmaskVector,
   PICMaskVector,
   PICAckVector,
   PICGetInServiceLocally,
   PICRestoreHostSetup,
   PICSteerVector,
   PICMaskAll,
   PICDump,
   PICPosted,
   PICPendingLocally,
   PICSpurious,
   PICGoodTrigger,
};

Chipset_ICFunctions_Internal PIC_Functions_Internal = {
   PICInit,
   PICHookupBusIRQ,
};
