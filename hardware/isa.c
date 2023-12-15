/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * isa.c - ISA related functions
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "vmkernel.h"
#include "isa.h"
#include "chipset_int.h"
#include "host.h"
#include "hardware_public.h"

#define LOGLEVEL_MODULE ISA
#define LOGLEVEL_MODULE_LEN 3
#include "log.h"


// flags
#define ISA_DEVICE_PRESENT	0x01
#define ISA_DEVICE_EDGE		0x02

typedef struct ISA_Device {
   uint8	flags;
   uint8	vector;	// vmkernel interrupt vector
} ISA_Device;

// ISA devices are uniquely identified by their irq == slot
static ISA_Device isaDevices[NUM_ISA_IRQS];

static void ISASetupInterrupt(IRQ isaIRQ);


/*
 *----------------------------------------------------------------------
 *
 * ISA_Init --
 *
 *      Perform initialization of the ISA module.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      vectors are set up for all of the ISA devices.
 *
 *----------------------------------------------------------------------
 */
void
ISA_Init(VMnix_ConfigOptions *vmnixOptions)
{
   IRQ isaIRQ;


   /*
    * Setup legacy ISA devices.
    *
    * NOTE: We rely on the fact that for ISA devices, the irq is fixed
    * whatever IC the host is using and is the same as the ISA slot.
    *
    * NOTE: By legacy ISA devices, we mean everything that is not PCI
    * and uses an irq in the ISA range, e.g. not only something like
    * the floppy controller (irq 6, really ISA, edge-triggered) but
    * also some health agents (irq 13, not really ISA, level-triggered).
    * We make the distinction between PCI and non-PCI because we are
    * never interested in non-PCI devices in the vmkernel.
    */
   Log("Setting up ISA devices interrupts");
   for (isaIRQ = 0; isaIRQ < NUM_ISA_IRQS; isaIRQ++) {

      isaDevices[isaIRQ].flags = 0;

      if (Chipset_TriggerType(isaIRQ) == VMK_HW_INT_LEVEL) {
	 /*
	  * This irq is not ISA. It has been configured for PCI.
	  *
	  * NOTE: Chipset_TriggerType() uses the ELCR register.
	  * While this register claims to report Edge/Level status, it is
	  * not entirely true. For instance irq13 will always be reported as
	  * edge for legacy reason even if it is used as level in the system.
	  * It is however a guarantee that irqs used for PCI will show up
	  * as level.
	  */
	 Log("irq %d is not ISA", isaIRQ);
	 continue;
      }

      if ((isaIRQ == VMNIX_IRQ) ||
	  (isaIRQ == TIMER_IRQ) ||
	  (isaIRQ == CASCADE_IRQ)) {
	 /*
	  * These irqs are not real and are emulated by vmkernel.
	  *
	  * NOTE: The system timer is disabled and we use the local APIC
	  * timer to emulate it. If we ever decide to use the real TIMER_IRQ,
	  * we must take care of machines that do not have it as a normal
	  * interrupt but only as an external interrupt in the MPS table.
	  * This would show up as a failure in ISASetupInterrupt() claiming no
	  * IOAPIC pin found for TIMER_IRQ.
	  */
	 Log("irq %d is emulated by vmkernel", isaIRQ);
	 continue;
      }

      Log("irq %d", isaIRQ);
      ISASetupInterrupt(isaIRQ);
   }

}


/*
 *----------------------------------------------------------------------
 *
 * ISASetupInterrupt --
 *
 *      Set up the IC pin for an ISA device and get its vector
 *
 * Results:
 *      None
 *
 * Side Effects:
 *      IC is setup
 *
 *----------------------------------------------------------------------
 */
static void
ISASetupInterrupt(IRQ isaIRQ)
{
   uint32 vector;
   Bool edge;
   IRQ irq;
   Bool ok;


   ok = Chipset_HookupBusIRQ(VMK_HW_BUSTYPE_ISA,-1,isaIRQ,isaIRQ, 
                             &edge,&irq,&vector);
   if (!ok) {
      Warning("couldn't map ISA irq %d", isaIRQ);
      isaDevices[isaIRQ].flags = 0;
      return;
   }
   ASSERT(irq == isaIRQ);
   isaDevices[isaIRQ].flags = ISA_DEVICE_PRESENT | (edge?ISA_DEVICE_EDGE:0);
   isaDevices[isaIRQ].vector = vector;

   Host_SetupIRQ(irq, vector, TRUE, edge);

   return;
}

/*
 *----------------------------------------------------------------------
 *
 * ISA_GetDeviceVector --
 *
 * 	Return the vmkernel vector associated with an ISA device
 *
 * Results:
 * 	0 if no vector associated, vector otherwise
 *
 * Side Effects:
 * 	None
 *
 *----------------------------------------------------------------------
 */
uint32
ISA_GetDeviceVector(IRQ isaIRQ)
{
   ASSERT(isaIRQ < NUM_ISA_IRQS);

   if (isaDevices[isaIRQ].flags & ISA_DEVICE_PRESENT) {
      return isaDevices[isaIRQ].vector;
   } else {
      return 0;
   }
}
