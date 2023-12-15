/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

#include "vm_types.h"
#include "vm_libc.h"
#include "vmkernel.h"
#include "pci_int.h"
#include "chipset_int.h"
#include "host.h"
#include "splock.h"
#include "memalloc.h"
#include "proc.h"
#include "helper.h"
#include "list.h"
#include "mod_loader.h"
#include "mps.h"
#include "hardware_public.h"

#define LOGLEVEL_MODULE PCI
#define LOGLEVEL_MODULE_LEN 3
#include "log.h"


/*
 * PCI configuration space header types.
 */
#define PCI_HEADER_MULTIFUNC(hdrType)	(((hdrType) & 0x80) != 0)
#define PCI_HEADER_NORMAL(hdrType)	(((hdrType) & 0x7F) == 0)
#define PCI_HEADER_PCI_BRIDGE(hdrType)	(((hdrType) & 0x7F) == 1)

/*
 * PCI configuration space registers common to all header types.
 */
#define PCI_REG_VENDOR_ID		0x00
#define PCI_REG_DEVICE_ID		0x02
#define PCI_REG_CLASS_REVISION		0x08
#define PCI_REG_HEADER_TYPE		0x0e
#define PCI_REG_INTERRUPT_LINE		0x3c
#define PCI_REG_INTERRUPT_PIN		0x3d

/*
 * PCI configuration space registers specific to normal header type.
 */
#define PCI_REG_SUBSYSTEM_VENDOR_ID	0x2c
#define PCI_REG_SUBSYSTEM_DEVICE_ID	0x2e

/*
 * PCI configuration space registers specific to PCI bridge header type.
 */
#define PCI_REG_PRIMARY_BUS         	0x18
#define PCI_REG_SECONDARY_BUS       	0x19

/*
 * PCI device class codes
 */
#define PCI_CLASSCODE_IDE		0x0101
#define PCI_CLASSCODE_PCI_BRIDGE	0x0604


/*
 * For each bus (identified by number), we keep track of the bridge that
 * spawns it.
 *
 * NOTE: It seems to be impossible for root buses to find the corresponding
 * host bridge, so only PCI-to-PCI bridges are tracked, so a NULL value in
 * the array denotes either a non-existing bus or a root bus.
 */
static PCI_Device *pciBridge[PCI_NUM_BUSES];


/*
 * A circular list of devices is kept using the list module.
 */
struct PCI_DeviceElt {
   List_Links	links;
   PCI_Device	device;
};

static List_Links pciDevices;

#define DEV_FROM_LINKS(links)	(&((struct PCI_DeviceElt *)(links))->device);
#define LINKS_FROM_DEV(dev)	(List_Links *) \
			((char *)(dev) - offsetof(struct PCI_DeviceElt, device))


static void PCIScan(void);
static void PCIDoDeviceInsertedCallbacks(PCI_Device *dev, Bool hotplug);
static void PCIDoDeviceRemovedCallbacks(PCI_Device *dev, Bool hotplug);

/*
 * A compatibility module (such as vmklinux) can request
 * callbacks on PCI events.
 */
#define PCI_NUM_COMPAT_MODULES	4

typedef struct PCICompatModule {
   int		moduleID;
   PCI_Callback	insert;
   PCI_Callback	remove;
} PCICompatModule;

static PCICompatModule pciCompatModule[PCI_NUM_COMPAT_MODULES];
static SP_SpinLock pciCompatModuleLock;

static Proc_Entry pciProcEntry;

static SP_SpinLockIRQ pciConfigLock;



static int PCIProcRead(Proc_Entry *entry, char *buffer, int *len);

static void PCISetupInterrupt(PCI_Device *dev);


static Bool
PCIType1(void)
{
   Bool status;
   uint32 eflags;
   uint32 tmp;

   SAVE_FLAGS(eflags);
   CLEAR_INTERRUPTS();

   OUTB(0xCFB, 0x01);
   tmp = IN32(0xCF8);
   OUT32(0xCF8, 0x80000000);
   if (IN32 (0xCF8) == 0x80000000) {
      status = TRUE;
   } else {
      status = FALSE;
   }

   OUT32(0xCF8, tmp);   
   RESTORE_FLAGS(eflags);   

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * PCI_Init --
 *
 * 	Initialize the PCI module
 *
 * Results:
 * 	None.
 *
 * Side Effects:
 * 	The PCI buses are scanned and the list of devices is built.
 * 	Devices not seen by COS are assigned a vector.
 *
 *----------------------------------------------------------------------
 */
void
PCI_Init(VMnix_Info *vmnixInfo)
{
   int i;
   List_Links *links;
   PCI_Device *dev;   

   SP_InitLock("PCICompat", &pciCompatModuleLock, SP_RANK_LOWEST);
   SP_InitLockIRQ("PCIConfig", &pciConfigLock, SP_RANK_IRQ_LEAF);

   for (i = 0; i < PCI_NUM_COMPAT_MODULES; i++) {
      pciCompatModule[i].moduleID = -1;
   }

   if (!PCIType1()) {
      Log("doesn't look like PCI type 1 configuration space");
   } else {
      PCIScan();
   }

   LIST_FORALL(&pciDevices, links) {
      dev = DEV_FROM_LINKS(links);

      /*
       * By default, all devices when discovered are assumed to be visible
       * to the host. We need to reset that based on the info we got when
       * the vmkernel was loaded.
       */
      ASSERT(dev->flags & PCI_DEVICE_HOST);
      if (vmnixInfo->hostFuncs[dev->bus][dev->slot] & (1<<dev->func)) {
         dev->flags |= PCI_DEVICE_HOST;
	 continue;
      } else {
	 dev->flags &= ~PCI_DEVICE_HOST;
      }

      // Skip IDE because it is a special PCI device that runs in ISA mode
      if (dev->flags & PCI_DEVICE_IDE) {
	 SysAlert("%s IDE device, wrongly assigned to the vmkernel",
			 dev->busAddress);
	 continue;
      }

      // Skip devices that do not have interrupts
      if (!(dev->flags & PCI_DEVICE_INTERRUPTIVE)) {
	 SysAlert("%s No interrupt, wrongly assigned to the vmkernel",
			 dev->busAddress);
	 continue;
      }
   }

   pciProcEntry.read = PCIProcRead;

   Proc_Register(&pciProcEntry, "pci", FALSE);
}

#define CONFIG_CMD(bus, slotFunc, reg)   (0x80000000 | (bus << 16) | (slotFunc << 8) | (reg & ~3))

void
PCI_ReadConfig8(uint32 bus, uint32 slotFunc, uint32 reg, uint8 *value)
{
   SP_IRQL prevIRQL = SP_LockIRQ(&pciConfigLock, SP_IRQL_KERNEL);
   OUT32(0xCF8, CONFIG_CMD(bus, slotFunc, reg));
   *value = INB(0xCFC + (reg & 3));
   SP_UnlockIRQ(&pciConfigLock, prevIRQL);
}

void
PCI_ReadConfig16(uint32 bus, uint32 slotFunc, uint32 reg, uint16 *value)
{
   SP_IRQL prevIRQL = SP_LockIRQ(&pciConfigLock, SP_IRQL_KERNEL);
   OUT32(0xCF8, CONFIG_CMD(bus, slotFunc, reg));
   *value = INW(0xCFC + (reg & 2));
   SP_UnlockIRQ(&pciConfigLock, prevIRQL);
}

void 
PCI_ReadConfig32(uint32 bus, uint32 slotFunc, uint32 reg, uint32 *value)
{
   SP_IRQL prevIRQL = SP_LockIRQ(&pciConfigLock, SP_IRQL_KERNEL);
   OUT32(0xCF8, CONFIG_CMD(bus, slotFunc, reg));
   *value = IN32(0xCFC);
   SP_UnlockIRQ(&pciConfigLock, prevIRQL);
}

void
PCI_WriteConfig8(uint32 bus, uint32 slotFunc, uint32 reg, uint8 value)
{
   SP_IRQL prevIRQL = SP_LockIRQ(&pciConfigLock, SP_IRQL_KERNEL);
   OUT32(0xCF8, CONFIG_CMD(bus, slotFunc, reg));
   OUTB(0xCFC + (reg & 3), value);
   SP_UnlockIRQ(&pciConfigLock, prevIRQL);
}

void
PCI_WriteConfig16(uint32 bus, uint32 slotFunc, uint32 reg, uint16 value)
{
   SP_IRQL prevIRQL = SP_LockIRQ(&pciConfigLock, SP_IRQL_KERNEL);
   OUT32(0xCF8, CONFIG_CMD(bus, slotFunc, reg));
   OUTW(0xCFC + (reg & 2), value);
   SP_UnlockIRQ(&pciConfigLock, prevIRQL);
}

void
PCI_WriteConfig32(uint32 bus, uint32 slotFunc, uint32 reg, uint32 value)
{
   SP_IRQL prevIRQL = SP_LockIRQ(&pciConfigLock, SP_IRQL_KERNEL);
   OUT32(0xCF8, CONFIG_CMD(bus, slotFunc, reg));
   OUT32(0xCFC, value);
   SP_UnlockIRQ(&pciConfigLock, prevIRQL);
}


/*
 *----------------------------------------------------------------------
 *
 * PCIListDevicesInSlot --
 *
 * 	Return the devices already discovered in bus:slot. If onlyFunc is
 * 	specified, only the specific function is checked, all others will
 * 	be returned as NULL.
 *
 * Results:
 * 	TRUE if something exists, FALSE otherwise
 * 	The devices array is filled.
 *
 * Side Effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static Bool
PCIListDevicesInSlot(uint32 bus, uint32 slot, uint32 onlyFunc, PCI_Device **devices)
{
   Bool something = FALSE;
   uint32 func;
   List_Links *links;
   PCI_Device *dev;

   ASSERT(bus < PCI_NUM_BUSES);
   ASSERT(slot < PCI_NUM_SLOTS);
   ASSERT(onlyFunc <= PCI_NUM_FUNCS);

   for (func = 0; func < PCI_NUM_FUNCS; func++) {
      devices[func] = NULL;
   }

   LIST_FORALL(&pciDevices, links) {
      dev = DEV_FROM_LINKS(links);

      if ((dev->bus != bus) || (dev->slot != slot)) {
	 continue;
      }
      
      func = dev->func;
      if ((onlyFunc < PCI_NUM_FUNCS) && (func != onlyFunc)) {
	 continue;
      }

      ASSERT(devices[func] == NULL);
      devices[func] = dev;
      something = TRUE;
   }

   return something;
}

/*
 *----------------------------------------------------------------------
 *
 * PCIScanSlot --
 *
 *      Updates list with devices present in bus:slot. If func is given,
 *      only the specific function is checked. IF SO, oldDevices MUST
 *      have NULL entries for all other functions.
 *      
 *      oldDevices input represents the currently known devices for the
 *      slot.
 *      
 *      Devices no longer present are removed from the device list and
 *      their structure freed. New devices are added to the device list
 *      and their structure is allocated.
 *      
 *      newDevices output represents the updated known devices for the
 *      slot.
 *
 * Results:
 *      TRUE if something exists, FALSE otherwise
 *      newDevices array is filled.
 *
 * Side effects:
 * 	pciDevices list is updated
 *      PCI_Device structures are freed/allocated.
 *
 *----------------------------------------------------------------------
 */
static Bool
PCIScanSlot(uint32 bus, uint32 slot, uint32 onlyFunc, PCI_Device **oldDevices, PCI_Device **newDevices)
{
   Bool something = FALSE;
   uint32 func;
   uint32 slotfunc;
   Bool multiFunction = FALSE;
   uint32 classRevision;
   PCI_Device pcidev;
   PCI_Device *dev = &pcidev;
   struct PCI_DeviceElt *devElt;

   ASSERT(bus < PCI_NUM_BUSES);
   ASSERT(slot < PCI_NUM_SLOTS);
   ASSERT(onlyFunc <= PCI_NUM_FUNCS);
   ASSERT(newDevices != NULL);
   ASSERT(oldDevices != NULL);

   for (func = 0; func < PCI_NUM_FUNCS; func++) {

      memset(dev, 0, sizeof(*dev));
      newDevices[func] = NULL;

      if ((onlyFunc < PCI_NUM_FUNCS) && (func != onlyFunc)) {
	 ASSERT(oldDevices[func] == NULL);
      }

      /*
       * If it's not a multifunction card, any func above 0 is invalid.
       */
      if (!multiFunction && (func != 0)) {
	 continue;
      }

      /*
       * If func is given and it is not the current one, skip it,
       * except for 0 which is always checked for multifunction consistency
       * (i.e. if func is given and is not 0, the slot must have
       * a multifunction card).
       */
      if ((onlyFunc < PCI_NUM_FUNCS) && (func != onlyFunc) && (func != 0)) {
	 continue;
      }

      /*
       * Query the function presence.
       */
      slotfunc = PCI_SLOTFUNC(slot, func);
      PCI_ReadConfig16(bus, slotfunc, PCI_REG_VENDOR_ID, &dev->vendorID);
      PCI_ReadConfig16(bus, slotfunc, PCI_REG_DEVICE_ID, &dev->deviceID);
      if ((dev->vendorID == 0xffff) || (dev->vendorID == 0) ||
	  (dev->deviceID == 0xffff) || (dev->deviceID == 0)) {
	 continue;
      }

      /*
       * Determine multifunction-ness.
       */
      PCI_ReadConfig8(bus, slotfunc, PCI_REG_HEADER_TYPE, &dev->hdrType);
      if (func == 0) {
	 multiFunction = PCI_HEADER_MULTIFUNC(dev->hdrType);
         /*
          * If func is given and it is not 0, we can now skip 0.
          */
	 if ((onlyFunc < PCI_NUM_FUNCS) && (onlyFunc != 0)) {
	    continue;
	 }
      }

      /*
       * Check against an existing device already found at the same spot.
       */
      if (PCI_HEADER_NORMAL(dev->hdrType)) {
         PCI_ReadConfig16(bus, slotfunc, PCI_REG_SUBSYSTEM_VENDOR_ID,
			 &dev->subVendorID);
         PCI_ReadConfig16(bus, slotfunc, PCI_REG_SUBSYSTEM_DEVICE_ID,
			 &dev->subDeviceID);
      }
      if ((oldDevices[func] != NULL) &&
	  (oldDevices[func]->vendorID == dev->vendorID) &&
	  (oldDevices[func]->deviceID == dev->deviceID) &&
	  (oldDevices[func]->subVendorID == dev->subVendorID) &&
	  (oldDevices[func]->subDeviceID == dev->subDeviceID)) {
	 newDevices[func] = oldDevices[func];
	 continue;
      }

      /*
       * Fill out the PCI_Device structure.
       */
      dev->bus = bus;
      dev->slot = slot;
      dev->func = func;
      dev->slotFunc = slotfunc;

      PCI_ReadConfig32(bus, slotfunc, PCI_REG_CLASS_REVISION, &classRevision);
      dev->classCode = classRevision >> 16;
      dev->progIFRevID = classRevision & 0xFFFF;

      dev->intPin = PCI_INTPIN_NONE;
      dev->intLine = PCI_INTLINE_NONE;
      dev->irq = PCI_IRQ_NONE;

      snprintf(dev->busAddress, sizeof(dev->busAddress),
				PCI_DEVICE_BUS_ADDRESS, bus, slot, func);
      snprintf(dev->vendorSignature, sizeof(dev->vendorSignature),
				PCI_DEVICE_VENDOR_SIGNATURE,
	dev->vendorID, dev->deviceID, dev->subVendorID, dev->subDeviceID);

      /*
       * Check the device.
       */
      if (PCI_HEADER_NORMAL(dev->hdrType)) {

	 // PCI interrupt pin (INTA, INTB, INTC INTD mapped to 1, 2, 3, 4)
	 PCI_ReadConfig8(bus, slotfunc, PCI_REG_INTERRUPT_PIN, &dev->intPin);

	 // If the device has a pin, it is capable of interrupting
	 if (dev->intPin) {
	    if (dev->intPin > 4) {
	       Warning("%s %s bad PCI intPin %d",
			dev->busAddress, dev->vendorSignature, dev->intPin);
	       continue;
	    }
	    // It's easier if we keep the pin as 0-3 instead of 1-4
	    dev->intPin--;
	    PCI_ReadConfig8(bus,slotfunc,PCI_REG_INTERRUPT_LINE, &dev->intLine);
	    if ((dev->intLine != PCI_INTLINE_NONE) &&
			    (dev->intLine >= NUM_ISA_IRQS)) {
	       Log("%s %s intLine contains %d, cf. PR 26655",
			dev->busAddress, dev->vendorSignature, dev->intLine);
	    }
	    dev->flags |= PCI_DEVICE_INTERRUPTIVE;
	 } else {
	    dev->intPin = PCI_INTPIN_NONE;
	 }
	 // Take note if it is an IDE device
	 if (dev->classCode == PCI_CLASSCODE_IDE) {
	    dev->flags |= PCI_DEVICE_IDE;
	 }

      } else if (PCI_HEADER_PCI_BRIDGE(dev->hdrType)) {

	 uint8 primary, secondary;

	 if (dev->classCode != PCI_CLASSCODE_PCI_BRIDGE) {
	    Warning("%s %s bad class %04x for bridge",
			dev->busAddress, dev->vendorSignature, dev->classCode);
	    continue;
	 }
	 PCI_ReadConfig8(bus, slotfunc, PCI_REG_PRIMARY_BUS, &primary);
	 PCI_ReadConfig8(bus, slotfunc, PCI_REG_SECONDARY_BUS, &secondary);
	 if ((primary == 0xff) || (secondary == 0xff)) {
	    Warning("%s %s bad primary %d or secondary %d",
			dev->busAddress, dev->vendorSignature,
		    	primary, secondary);
	    continue;
	 }
	 ASSERT(primary == bus);
	 ASSERT(secondary != 0);
	 dev->spawnedBus = secondary;
	 dev->flags |= PCI_DEVICE_PCI_BRIDGE;

      } else {

	 Warning("%s %s unsupported header type %02x",
			dev->busAddress, dev->vendorSignature, dev->hdrType);
	 continue;

      }

      dev->flags |= PCI_DEVICE_HOST; // newly discovered has to be owned by host

      devElt = (struct PCI_DeviceElt *)Mem_Alloc(sizeof(struct PCI_DeviceElt));
      ASSERT_NOT_IMPLEMENTED(devElt != NULL);
      List_InitElement(&devElt->links)
      newDevices[func] = &devElt->device;
      memcpy(newDevices[func], dev, sizeof(*dev));

   }

   /*
    * Update the list.
    */
   for (func = 0; func < PCI_NUM_FUNCS; func++) {

      if (newDevices[func] != NULL) {
	 something = TRUE;
      }

      if ((newDevices[func] == oldDevices[func]) && (oldDevices[func] != NULL)){

	 // device existed and was unchanged

	 dev = oldDevices[func];
	 Log("%s %s unchanged", dev->busAddress, dev->vendorSignature);

      } else {

	 // device inexistant or changed: removed, added or replaced
	 // (removed and added)

         if (oldDevices[func] != NULL) {

	    // old device vanished
	    dev = oldDevices[func];
	    Log("%s %s removed", dev->busAddress, dev->vendorSignature);

	    List_Remove(LINKS_FROM_DEV(dev));
	    if (dev->flags & PCI_DEVICE_PCI_BRIDGE) {
	       ASSERT(pciBridge[dev->spawnedBus] == dev);
	       Log("  Removing bus %d with parent %d",dev->spawnedBus,dev->bus);
	       pciBridge[dev->spawnedBus] = NULL;
	    }
	    ASSERT(dev->flags & PCI_DEVICE_HOST);
            Mem_Free(LINKS_FROM_DEV(dev)); // XXX more correctly PCI_DeviceElt

         }

         if (newDevices[func] != NULL) {

	    // new device appeared
	    dev = newDevices[func];
	    Log("%s %s added", dev->busAddress, dev->vendorSignature);
	    Log("  classCode %04x progIFRevID %04x",
			dev->classCode, dev->progIFRevID);
	    if (dev->flags & PCI_DEVICE_INTERRUPTIVE) {
	       Log("  intPIN %c intLine %d", 'A'+dev->intPin, dev->intLine);
	       PCISetupInterrupt(dev);
	       Log("  irq %d vector 0x%02x", dev->irq, dev->vector);
	       Host_SetupIRQ(dev->irq, dev->vector, FALSE, FALSE);
	    }

	    List_Insert(LINKS_FROM_DEV(dev), LIST_ATREAR(&pciDevices));
	    if (dev->flags & PCI_DEVICE_PCI_BRIDGE) {
	       ASSERT(pciBridge[dev->spawnedBus] == NULL);
	       Log("  Adding bus %d with parent %d", dev->spawnedBus, dev->bus);
	       pciBridge[dev->spawnedBus] = dev;
	    }

         }
      }

   }

   return something;
}

static void
PCIScan(void)
{
   uint32 bus;
   uint32 slot;
   PCI_Device *oldDevices[PCI_NUM_FUNCS] =
			{NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};
   PCI_Device *newDevices[PCI_NUM_FUNCS];

   Log("Building PCI devices list");
   List_Init(&pciDevices);

   for (bus = 0; bus < PCI_NUM_BUSES; bus++) {
      for (slot = 0; slot < PCI_NUM_SLOTS; slot++) {
	 PCIScanSlot(bus, slot, PCI_NUM_FUNCS, oldDevices, newDevices);
      }
   }
}



PCI_Device *
PCI_GetFirstDevice(void)
{
   if (List_IsEmpty(&pciDevices)) {
      return NULL;
   } else {
      return DEV_FROM_LINKS(List_First(&pciDevices));
   }
}

PCI_Device *
PCI_GetNextDevice(PCI_Device *dev)
{
   List_Links *links = List_Next(LINKS_FROM_DEV(dev));

   if (List_IsAtEnd(&pciDevices, links)) {
      return NULL;
   } else {
      return DEV_FROM_LINKS(links);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * PCI_IsSharedDevice
 *
 *      Check if the given bus/function device is shared between the ConsoleOS 
 *      and the vmkernel. 
 *
 * Results: 
 *      TRUE is device is shared, FALSE otherwise
 *
 * Side effects:
 *      None. 
 *
 *----------------------------------------------------------------------
 */

Bool
PCI_IsSharedDevice(uint32 bus, uint32 slotFunc)
{
   PCI_Device *devices[PCI_NUM_FUNCS];
   uint32 slot = PCI_SLOT(slotFunc);
   uint32 func = PCI_FUNC(slotFunc);

   PCIListDevicesInSlot(bus, slot, func, devices);

   if (devices[func] == NULL) {
      Warning(PCI_DEVICE_BUS_ADDRESS "not found",
		   bus, slot, func);
      return FALSE;
   }

   return (devices[func]->flags & PCI_DEVICE_SHARED) != 0;
}


static volatile Bool devOwnershipBeingChanged = FALSE;

/*
 *----------------------------------------------------------------------
 *
 * PCI_ChangeDevOwnership
 *
 * 	Change ownership of a device between COS and vmkernel
 *
 * Results:
 *      VMK_OK	success (if hotplug, relates only to the synchronous part)
 *      VMK_BUSY	another change is ongoing
 *      VMK_NOT_FOUND	no such device
 *      VMK_NOT_SUPPORTED	ownership cannot be changed
 * 
 * Side effects:
 * 	if this is a dynamic change (hotplug), the callback to the
 * 	compatibility module (vmklinux) is handled by a helper world.
 * 	
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
PCI_ChangeDevOwnership(VMnix_DevArgs *hostArgs)
{
   VMnix_DevArgs args;
   PCI_Device *devices[PCI_NUM_FUNCS];
   PCI_Device *dev;
   uint32 func;
   Bool deviceFound;
   Bool bridgeFound;
   VMK_ReturnStatus status;


   CopyFromHost(&args, hostArgs, sizeof(args));
   Log(PCI_DEVICE_BUS_ADDRESS " to %s %s", args.bus, args.slot, args.func,
		   args.toVMKernel ? "vmkernel" : "console",
		   args.hotplug ? "(HOTPLUG)" : "(SHARING)");

   /*
    * Only one change at a time.
    */
   if (devOwnershipBeingChanged) {
      Warning("Can only change one slot at once");
      return VMK_BUSY;
   }
   devOwnershipBeingChanged = TRUE; 
   // XXX m->useCount++; //don't allow unload while probing/removing

   /*
    * Get a list of the devices present in the slot.
    */
   deviceFound = PCIListDevicesInSlot(args.bus, args.slot, args.func, devices);

   /*
    * If the slot is empty, there is nothing to do
    */
   if (!deviceFound) {
      devOwnershipBeingChanged = FALSE;
      return VMK_NOT_FOUND;
   }

   /*
    * Check if there is a bridge in the slot, as they should remain
    * the property of COS.
    *
    * Some cards have a bridge as function 0 and actual devices as
    * other functions, like some RAID controllers on Dell machines.
    *
    * As a convenience, to avoid having to change the device function
    * by function, the whole slot can be changed but the bridge
    * function will remain unchanged.
    */
   bridgeFound = FALSE;
   for (func = 0; func < PCI_NUM_FUNCS; func++) {
      dev = devices[func];
      if ((dev != NULL) && (dev->flags & PCI_DEVICE_PCI_BRIDGE)) {
	 bridgeFound = TRUE;
	 break;
      }
   }
   if (bridgeFound) {
      ASSERT(dev->flags & PCI_DEVICE_HOST);
      if ((args.func < PCI_NUM_FUNCS)) {
	 if (func == args.func) {
	    /*
	     * Attempting to change ownership of a bridging function.
	     */
	    Warning("Won't change bridge ownership");
	    devOwnershipBeingChanged = FALSE;
	    return VMK_NOT_SUPPORTED;
	 } else {
	    /*
	     * Attempting to change ownership of a single function which
	     * is not the bridging function, this is fine.
	     */
	 }
      } else {
	 /*
	  * Attempting to change ownership of a whole slot which contains
	  * a bridging function.
	  */
	 Warning("Changing ownership of a whole slot that contains a bridge");
	 if (func != 0) {
	    Warning("The bridge is not function 0, not changing anything");
	    devOwnershipBeingChanged = FALSE;
	    return VMK_NOT_SUPPORTED;
	 } else {
	    deviceFound = FALSE;
	    for (func = 1; func < PCI_NUM_FUNCS; func++) {
	       dev = devices[func];
	       if (dev != NULL) {
		  deviceFound = TRUE;
		  break;
	       }
	    }
	    if (!deviceFound) {
	       Warning("Won't change bridge ownership, no other functions");
	       devOwnershipBeingChanged = FALSE;
	       return VMK_NOT_SUPPORTED;
	    } else {
	       Log("Won't change bridge ownership, changing other functions");
	    }
	 }
      }
   }

   /*
    * Go over the functions.
    */
   status = VMK_NOT_FOUND;
   for (func = 0; func < PCI_NUM_FUNCS; func++) {

      /*
       * If a function was specified, it must match too.
       */
      if ((args.func < PCI_NUM_FUNCS) && (func != args.func)) {
	 continue;
      }

      /*
       * If the function does not exist, skip it.
       */
      dev = devices[func];
      if (dev == NULL) {
	 continue;
      }

      /*
       * If it is a PCI bridge, its ownership is not changed.
       */
      if (dev->flags & PCI_DEVICE_PCI_BRIDGE) {
         continue;
      }

      /*
       * Do the change.
       */
      if (args.toVMKernel && (dev->flags & PCI_DEVICE_HOST)) {

         // host -> vmkernel
	 dev->flags &= ~PCI_DEVICE_HOST;

	 if (!args.hotplug) {
	    dev->flags |= PCI_DEVICE_SHARED;
	 }

	 PCIDoDeviceInsertedCallbacks(dev, args.hotplug);

	 status = VMK_OK;

      } else if (!args.toVMKernel && !(dev->flags & PCI_DEVICE_HOST)) {

         // vmkernel -> host 
	 dev->flags |= PCI_DEVICE_HOST;

	 if (!args.hotplug) {
	    dev->flags &= ~PCI_DEVICE_SHARED;
	 }

	 PCIDoDeviceRemovedCallbacks(dev, args.hotplug);

	 status = VMK_OK;

      }
   }

   if (!args.hotplug) {
      devOwnershipBeingChanged = FALSE;
   }

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * PCI_ChangeDevOwnershipProbe --
 *
 * 	Probe whether a change ownership operation is done
 *
 * Results:
 * 	VMK_OK if done
 * 	VMK_STATUS_PENDING if not
 *
 * Side Effects:
 * 	None.
 * 
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
PCI_ChangeDevOwnershipProbe(VMnix_DevArgs *hostArgs)
{
   return devOwnershipBeingChanged ? VMK_STATUS_PENDING : VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * PCI_SetDevName --
 *
 * 	Set the name of device
 *
 * Results:
 * 	VMK_OK if done
 * 	VMK_NOT_FOUND no such device
 * 	VMK_BAD_PARAM name is empty or func is not specified
 *
 * Side Effects:
 * 	the device name field is updated (if the device is already loaded,
 * 	its external published name remains the old one)
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
PCI_SetDevName(VMnix_DevArgs *hostArgs)
{
   VMnix_DevArgs args;
   PCI_Device *devices[PCI_NUM_FUNCS];
   PCI_Device *dev;
   Bool wasNameless = FALSE;


   CopyFromHost(&args, hostArgs, sizeof(args));
   Log(PCI_DEVICE_BUS_ADDRESS " to %s", args.bus, args.slot, args.func, args.name);
   if (args.func >= PCI_NUM_FUNCS) {
      return VMK_BAD_PARAM;
   }

   /*
    * Get a list of the devices present in the slot.
    */
   PCIListDevicesInSlot(args.bus, args.slot, args.func, devices);

   /*
    * Update the name if the device exists and the new name is not empty
    */
   dev = devices[args.func];
   if (dev == NULL) {
      Warning("No such device");
      return VMK_NOT_FOUND;
   }
   if (args.name[0] == '\0') {
      Warning("New name is empty");
      return VMK_BAD_PARAM;
   }
   wasNameless = (dev->name[0] == '\0');
   Log("Previous name was %s", dev->name);
   snprintf(dev->name, sizeof(dev->name), "%s", args.name);

   /*
    * If the device was nameless and can be seen and used by vmkernel, if must
    * now be made available.
    */
   if (wasNameless && !(dev->flags & PCI_DEVICE_HOST) &&
		   (dev->flags & PCI_DEVICE_INTERRUPTIVE)) {
      PCIDoDeviceInsertedCallbacks(dev, TRUE);
   }

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * PCI_GetDevName --
 *
 * 	Get the name of a device
 *
 * Results:
 * 	VMK_OK if done
 *      VMK_NOT_FOUND no such device
 *      VMK_BAD_PARAM func is not specified
 *
 * Side Effects:
 * 	None
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
PCI_GetDevName(VMnix_DevArgs *hostArgs)
{
   VMnix_DevArgs args;
   PCI_Device *devices[PCI_NUM_FUNCS];
   PCI_Device *dev;


   CopyFromHost(&args, hostArgs, sizeof(args));
   if (args.func >= PCI_NUM_FUNCS) {
      return VMK_BAD_PARAM;
   }

   /*
    * Get a list of the devices present in the slot.
    */
   PCIListDevicesInSlot(args.bus, args.slot, args.func, devices);

   /*
    * Return the name if the device exists
    */
   dev = devices[args.func];
   if (dev == NULL) {
      Log("No such device");
      return VMK_NOT_FOUND;
   } else {
      snprintf(args.name, sizeof(args.name), "%s", dev->name);
      CopyToHost(hostArgs, &args, sizeof(args));
      return VMK_OK;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * PCI_CheckDevName --
 *
 * 	Check that a device is named
 *
 * 	Note that once a device has a name it cannot lose it
 *
 * Results:
 * 	VMK_OK if so
 * 	VMK_FAILURE otherwise
 *
 * Side Effects:
 * 	None
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
PCI_CheckDevName(VMnix_DevArgs *hostArgs)
{
   VMnix_DevArgs args;
   PCI_Device *devices[PCI_NUM_FUNCS];
   PCI_Device *dev;
   uint32 func;


   CopyFromHost(&args, hostArgs, sizeof(args));

   /*
    * Get a list of the devices present in the slot.
    */
   PCIListDevicesInSlot(args.bus, args.slot, args.func, devices);

   for (func = 0; func < PCI_NUM_FUNCS; func++) {
      if ((args.func < PCI_NUM_FUNCS) && (func != args.func)) {
         continue;
      }
      dev = devices[func];
      if (dev == NULL) {
         continue;
      }
      if (dev->flags & PCI_DEVICE_PCI_BRIDGE) {
         continue;
      }
      if (dev->name[0] == '\0') {
	  Log("%s is nameless", dev->busAddress);
          return VMK_FAILURE;
      }
   }

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * PCI_ScanDev --
 *
 * 	Scan a device range and update device list
 *
 * Results:
 * 	VMK_OK always
 *
 * Side Effects:
 * 	pciDevices list updated
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
PCI_ScanDev(VMnix_DevArgs *hostArgs)
{
   VMnix_DevArgs args;
   uint32 bus;
   uint32 slot;
   PCI_Device *oldDevices[PCI_NUM_FUNCS];
   PCI_Device *newDevices[PCI_NUM_FUNCS];


   CopyFromHost(&args, hostArgs, sizeof(args));


   for (bus = 0; bus < PCI_NUM_BUSES; bus++) {

      /*
       * If a bus has been specified, skip all others.
       */
      if ((args.bus < PCI_NUM_BUSES) && (bus != args.bus)) {
	 continue;
      }

      for (slot = 0; slot < PCI_NUM_SLOTS; slot++) {

	 /*
	  * If a slot has been specified, skip all others.
	  */
	 if ((args.slot < PCI_NUM_SLOTS) && (slot != args.slot)) {
	    continue;
	 }

	 /*
	  * Get a list of the devices present in the slot and
	  * rescan it for changes.
	  */
	 PCIListDevicesInSlot(bus, slot, args.func, oldDevices);
         PCIScanSlot(bus, slot, args.func, oldDevices, newDevices);

      }

   }

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * PCI_GetDeviceName
 *
 *    Return the name that should be published (e.g. vmnic0)
 *
 *
 * Results:
 *    NULL if the device does not exist or is nameless, the name otherwise
 *
 * Side Effects:
 *    None
 *
 *----------------------------------------------------------------------
 */
const char *
PCI_GetDeviceName(uint32 bus, uint32 slotFunc)
{
   PCI_Device *devices[PCI_NUM_FUNCS];
   PCI_Device *dev;
   uint32 slot = PCI_SLOT(slotFunc);
   uint32 func = PCI_FUNC(slotFunc);

   PCIListDevicesInSlot(bus, slot, func, devices);

   /*
    * Return the name if the device exists
    */
   dev = devices[func];

   if (dev == NULL) {
      Warning(PCI_DEVICE_BUS_ADDRESS "not found", bus, slot, func);
      return NULL;
   }

   ASSERT(dev->name[0] != '\0');
   if (dev->name[0] == '\0') {
      SysAlert("%s is nameless", dev->busAddress);
      return NULL;
   }

   return dev->name;
}

/*
 *----------------------------------------------------------------------
 *
 * PCI_RegisterCallback --
 *
 * 	Register callback functions of a compatibility module that will
 * 	be called when a device is inserted/removed
 *
 * Results:
 * 	None.
 *
 * Side Effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
void 
PCI_RegisterCallback(int moduleID, PCI_Callback insert, PCI_Callback remove)
{
   int i;
   PCICompatModule *cm = NULL;

   Log("for module %d", moduleID);

   ASSERT(moduleID != -1);

   SP_Lock(&pciCompatModuleLock);

   // A compatibility module can only register callbacks once
   for (i = 0; i < PCI_NUM_COMPAT_MODULES; i++) {
      ASSERT(pciCompatModule[i].moduleID != moduleID);
   }

   // Find an empty spot
   for (i = 0; i < PCI_NUM_COMPAT_MODULES; i++) {
      if (pciCompatModule[i].moduleID == -1) {
	 cm = &pciCompatModule[i];
	 break;
      }
   }
   ASSERT_NOT_IMPLEMENTED(cm != NULL);

   cm->moduleID = moduleID;
   cm->insert = insert;
   cm->remove = remove;

   SP_Unlock(&pciCompatModuleLock);
}

/*
 *----------------------------------------------------------------------
 *
 * PCI_UnregisterCallback --
 *
 * 	Unregister callback functions of a compatibility module that would
 * 	have been called when a device is inserted/removed
 *
 * Results:
 * 	None.
 *
 * Side Effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
void 
PCI_UnregisterCallback(int moduleID, PCI_Callback insert, PCI_Callback remove)
{
   int i;
   PCICompatModule *cm = NULL;

   Log("for module %d", moduleID);

   ASSERT(moduleID != -1);

   SP_Lock(&pciCompatModuleLock);

   // Find the corresponding spot
   for (i = 0; i < PCI_NUM_COMPAT_MODULES; i++) {
      if (pciCompatModule[i].moduleID == moduleID) {
	 cm = &pciCompatModule[i];
	 break;
      }
   }
   ASSERT_NOT_IMPLEMENTED(cm != NULL);

   ASSERT(cm->insert == insert);
   ASSERT(cm->remove == remove);

   cm->moduleID = -1;

   SP_Unlock(&pciCompatModuleLock);
}

/*
 *----------------------------------------------------------------------
 *
 * PCIHelpDeviceInsertedCallbacks --
 *
 * 	Execute all insert callbacks (helper world environment)
 *
 * Results:
 * 	None.
 *
 * Side Effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
static void
PCIHelpDeviceInsertedCallbacks(void *data)
{
   int i;
   PCICompatModule *cm;
   PCICompatModule *cms[PCI_NUM_COMPAT_MODULES];
   int numCMs = 0;

   /*
    * Make sure interrupts are enabled because we may execute driver code
    * that depends on it.
    */
   ASSERT_HAS_INTERRUPTS();

   /*
    * Lock down compatibility modules that requested callbacks so that they
    * do not disappear while we are calling them.
    */
   SP_Lock(&pciCompatModuleLock);
   for (i = 0; i < PCI_NUM_COMPAT_MODULES; i++) {
      cm = &pciCompatModule[i];
      if ((cm->moduleID != -1) && (cm->insert != NULL)) {
	 if (Mod_IncUseCount(cm->moduleID) == VMK_OK) {
	    cms[numCMs++] = cm;
	 } else {
	    Warning("Module %d cannot be locked down", cm->moduleID);
	 }
      }
   }
   SP_Unlock(&pciCompatModuleLock);

   /*
    * Invoke insert callback of modules we could lock down and release them.
    */
   for (i = 0; i < numCMs; i++) {
      ASSERT(cms[i]->moduleID != -1);
      ASSERT(cms[i]->insert != NULL);
      (*cms[i]->insert)((PCI_Device *)data, TRUE);
      Mod_DecUseCount(cms[i]->moduleID);
   }

   devOwnershipBeingChanged = FALSE;
}
 
/*
 *----------------------------------------------------------------------
 *
 * PCIHelpDeviceRemovedCallbacks --
 *
 * 	Execute all remove callbacks (helper world environment)
 *
 * Results:
 * 	None.
 *
 * Side Effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
static void
PCIHelpDeviceRemovedCallbacks(void *data)
{
   int i;
   PCICompatModule *cm;
   PCICompatModule *cms[PCI_NUM_COMPAT_MODULES];
   int numCMs = 0;

   /*
    * Make sure interrupts are enabled because we may execute driver code
    * that depends on it.
    */
   ASSERT_HAS_INTERRUPTS();

   /*
    * Lock down compatibility modules that requested callbacks so that they
    * do not disappear while we are calling them.
    */
   SP_Lock(&pciCompatModuleLock);
   for (i = 0; i < PCI_NUM_COMPAT_MODULES; i++) {
      cm = &pciCompatModule[i];
      if ((cm->moduleID != -1) && (cm->remove != NULL)) {
	 if (Mod_IncUseCount(cm->moduleID) == VMK_OK) {
	    cms[numCMs++] = cm;
	 } else {
	    Warning("Module %d cannot be locked down", cm->moduleID);
	 }
      }
   }
   SP_Unlock(&pciCompatModuleLock);

   /*
    * Invoke remove callback of modules we could lock down and release them.
    */
   for (i = 0; i < numCMs; i++) {
      ASSERT(cms[i]->moduleID != -1);
      ASSERT(cms[i]->remove != NULL);
      (*cms[i]->remove)((PCI_Device *)data, TRUE);
      Mod_DecUseCount(cms[i]->moduleID);
   }

   devOwnershipBeingChanged = FALSE;
}

/*
 *----------------------------------------------------------------------
 *
 * PCIDoDeviceInsertedCallbacks --
 *
 * 	Execute all insert callbacks (possibly in a helper world)
 *
 * Results:
 * 	None.
 *
 * Side Effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
static void
PCIDoDeviceInsertedCallbacks(PCI_Device *dev, Bool hotplug)
{
   int i;
   PCICompatModule *cm;
   PCICompatModule *cms[PCI_NUM_COMPAT_MODULES];
   int numCMs = 0;

   if (hotplug) {
      Helper_Request(HELPER_MISC_QUEUE, PCIHelpDeviceInsertedCallbacks, dev);
      return;
   }

   /*
    * Lock down compatibility modules that requested callbacks so that they
    * do not disappear while we are calling them.
    */
   SP_Lock(&pciCompatModuleLock);
   for (i = 0; i < PCI_NUM_COMPAT_MODULES; i++) {
      cm = &pciCompatModule[i];
      if ((cm->moduleID != -1) && (cm->insert != NULL)) {
	 if (Mod_IncUseCount(cm->moduleID) == VMK_OK) {
	    cms[numCMs++] = cm;
	 } else {
	    Warning("Module %d cannot be locked down", cm->moduleID);
	 }
      }
   }
   SP_Unlock(&pciCompatModuleLock);

   /*
    * Invoke insert callback of modules we could lock down and release them.
    */
   for (i = 0; i < numCMs; i++) {
      ASSERT(cms[i]->moduleID != -1);
      ASSERT(cms[i]->insert != NULL);
      (*cms[i]->insert)(dev, FALSE);
      Mod_DecUseCount(cms[i]->moduleID);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * PCIDoDeviceRemovedCallbacks --
 *
 * 	Execute all remove callbacks (possibly in a helper world)
 *
 * Results:
 * 	None.
 *
 * Side Effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
static void
PCIDoDeviceRemovedCallbacks(PCI_Device *dev, Bool hotplug)
{
   int i;
   PCICompatModule *cm;
   PCICompatModule *cms[PCI_NUM_COMPAT_MODULES];
   int numCMs = 0;

   if (hotplug) {
      Helper_Request(HELPER_MISC_QUEUE, PCIHelpDeviceRemovedCallbacks, dev);
      return;
   }

   /*
    * Lock down compatibility modules that requested callbacks so that they
    * do not disappear while we are calling them.
    */
   SP_Lock(&pciCompatModuleLock);
   for (i = 0; i < PCI_NUM_COMPAT_MODULES; i++) {
      cm = &pciCompatModule[i];
      if ((cm->moduleID != -1) && (cm->remove != NULL)) {
	 if (Mod_IncUseCount(cm->moduleID) == VMK_OK) {
	    cms[numCMs++] = cm;
	 } else {
	    Warning("Module %d cannot be locked down", cm->moduleID);
	 }
      }
   }
   SP_Unlock(&pciCompatModuleLock);

   /*
    * Invoke remove callback of modules we could lock down and release them.
    */
   for (i = 0; i < numCMs; i++) {
      ASSERT(cms[i]->moduleID != -1);
      ASSERT(cms[i]->remove != NULL);
      (*cms[i]->remove)(dev, FALSE);
      Mod_DecUseCount(cms[i]->moduleID);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * PCISetupInterrupt --
 *
 * 	Set up the IC pin for a device and get its vector
 *
 * Results:
 * 	None
 *
 * Side Effects:
 * 	IC is setup
 * 	dev->vector is updated
 * 	dev->irq is updated
 *
 *----------------------------------------------------------------------
 */
static void
PCISetupInterrupt(PCI_Device *dev)
{
   Bool edge;
   IRQ irq;
   uint32 vector;
   int pin;
   PCI_Device *bridge;


   ASSERT_BUG(15463, dev->intPin < 4);

   if (Chipset_HookupBusIRQ(VMK_HW_BUSTYPE_PCI, dev->bus,
			MPS_PCI_BUSIRQ(dev->slot, dev->intPin),
			dev->intLine, &edge, &irq, &vector)) {
      dev->vector = vector;
      dev->irq = irq;
      return;
   }

   Log("No direct hookup for %s", dev->busAddress);

   /*
    * If there was no direct hookup possible, then it is likely a bridged
    * device and we should try to hook it up through the bridges until
    * we find a hookup or reach a root bus.
    *
    * Pins of PCI slots are connected to the pins of the bridge in a
    * staggered manner (barber pole):
    * slot 0 pin A is connected to bridge pin A
    * slot 0 pin B is connected to bridge pin B
    * slot 0 pin C is connected to bridge pin C
    * slot 0 pin D is connected to bridge pin D
    * slot 1 pin A is connected to bridge pin B
    * slot 1 pin B is connected to bridge pin C
    * slot 1 pin C is connected to bridge pin D
    * slot 1 pin D is connected to bridge pin A
    * and so on
    */
   pin = (dev->intPin + dev->slot) % 4;
   bridge = pciBridge[dev->bus];
   while (bridge) {
      ASSERT(bridge->flags & PCI_DEVICE_PCI_BRIDGE);
      Log("Trying through bridge at %s", bridge->busAddress);

      if (Chipset_HookupBusIRQ(VMK_HW_BUSTYPE_PCI, bridge->bus,
			MPS_PCI_BUSIRQ(bridge->slot, pin),
			dev->intLine, &edge, &irq, &vector)) {
	 dev->vector = vector;
	 dev->irq = irq;
	 return;
      }

      pin = (pin + bridge->slot) % 4;
      bridge = pciBridge[bridge->bus];
   }

   /*
    * If the device was not bridged or we could not hook it up through its
    * bridge, we should try to hook it up through the ISA irq it was assigned
    * by BIOS if it is level-triggered.
    */
   Log("Trying through ISA irq %d", dev->intLine);
   if (Chipset_HookupBusIRQ(VMK_HW_BUSTYPE_ISA, -1,
				dev->intLine,
				dev->intLine, &edge, &irq, &vector) && !edge) {
      dev->vector = vector;
      dev->irq = irq;
      return;
   }

   SysAlert("failed for %s", dev->busAddress);
   dev->vector = 0;
   dev->irq = PCI_IRQ_NONE;

   return;
}


typedef struct PCIDescriptor {
   int class;
   char *description;
} PCIDescriptor;

static PCIDescriptor pciDescriptors[] = {
   { 0x100, "SCSI" },
   { 0x101, "IDE" },
   { 0x104, "RAID" },
   { 0x180, "Storage" },
   { 0x200, "Ethernet" },
   { 0x300, "Display" },
   { 0x400, "Video" },
   { 0x401, "Audio" },
   { 0x480, "Multimed" },
   { 0x600, "Host/PCI" },
   { 0x601, "PCI/ISA" },
   { 0x604, "PCI/PCI" },
   { 0x804, "PCI HotP" },
   { 0xc03, "USB" },
   { 0xc04, "FC" },
   { 0xc05, "SMBus" },
   { 0x000, NULL }
};

typedef struct VendorDescriptor {
   int vendor;
   char *name;
} VendorDescriptor;

static VendorDescriptor vendors[] = {
   { PCI_VENDOR_ID_INTEL,       "Intel" },
   { PCI_VENDOR_ID_3COM,        "3Com" },
   { PCI_VENDOR_ID_ADAPTEC,     "Adaptec" },
   { PCI_VENDOR_ID_ADAPTEC_2,   "Adaptec" },
   { PCI_VENDOR_ID_DEC,         "DEC" },
   { PCI_VENDOR_ID_SYMBIOS,     "Symbios" },
   { PCI_VENDOR_ID_COMPAQ,      "Compaq" },
   { PCI_VENDOR_ID_LITEON,      "Lite-On" },
   { PCI_VENDOR_ID_BUSLOGIC,    "BusLogic" },
   { PCI_VENDOR_ID_DELL,        "Dell" },
   { PCI_VENDOR_ID_IBM,         "IBM" },
   { PCI_VENDOR_ID_BROADCOM,    "Broadcom" },
   { PCI_VENDOR_ID_NVIDIA,      "NVidia" },
   { PCI_VENDOR_ID_QLOGIC,      "QLogic" },
   { PCI_VENDOR_ID_ATI,         "ATI" },
   { PCI_VENDOR_ID_SERVERWORKS, "SrvrWrks" },
   { PCI_VENDOR_ID_EMULEX,      "Emulex" },
   { PCI_VENDOR_ID_AMD,         "AMD" },
   { 0, NULL },
};

/*
 *----------------------------------------------------------------------
 *
 * PCIProcRead --
 *
 *      Callback for read operation on the pci space.
 *
 * Results: 
 *      VMK_OK
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
PCIProcRead(Proc_Entry *entry,
           char       *buffer,
           int        *len)
{
   List_Links *links;
   PCI_Device *dev;
   *len = 0;

/*
 * Format used contains:
 * 	PCI bus address (bus:slot.func)
 * 	PCI vendor and device IDs
 * 	PCI subsystem vendor and device IDs
 * 	Type of device
 * 	Vendor spelled out
 * 	ISA pin
 * 	irq for COS
 * 	vector for vmkernel
 * 	PCI interrupt pin
 * 	Mode of operation (Console, Vmkernel, Shared)
 * 	Module handling the device if owned by vmkernel
 * 	External name of the device if owned by vmkernel
 */
#define FORMAT	"%-8s %-19s %-8s %-8s %-13s %-1s %-8s %-8s\n"

   Proc_Printf(buffer, len, FORMAT,
	"Bus:Sl.F", "Vend:Dvid Subv:Subd", "Type", "Vendor", "ISA/irq/Vec P",
	"M", "Module", "Name");
   Proc_Printf(buffer, len, FORMAT, "", "", "", "", "Spawned bus", "", "", "");

   LIST_FORALL(&pciDevices, links) {
      int i;
      char typeBuf[8];
      char *type = NULL;
      char vendorBuf[8];
      char *vendor = NULL;
      char intBuf[16];
      char modName[VMNIX_MODULE_NAME_LENGTH];

      dev = DEV_FROM_LINKS(links);

      for (i = 0; pciDescriptors[i].description != NULL; i++) {
	 if (pciDescriptors[i].class == dev->classCode) {
	    type = pciDescriptors[i].description;
	    break;
	 }
      }

      if (type == NULL) {
	 type = typeBuf;
	 snprintf(typeBuf, sizeof typeBuf, "0x%x", dev->classCode);
      }

      for (i = 0; vendors[i].name != NULL; i++) {
	 if (vendors[i].vendor == dev->vendorID) {
	    vendor = vendors[i].name;
	    break;
	 }
      }

      if (vendor == NULL) {
	 vendor = vendorBuf;
	 snprintf(vendorBuf, sizeof vendorBuf, "0x%x", dev->vendorID);
      }

      if (dev->flags & PCI_DEVICE_INTERRUPTIVE) {
	 if (dev->vector == 0) {
	    snprintf(intBuf, sizeof intBuf, "%2d/   /     %c",
		        dev->intLine, 'A'+dev->intPin);
	 } else if (dev->irq == PCI_IRQ_NONE) {
	    snprintf(intBuf, sizeof intBuf, "%2d/   /0x%02x %c",
			dev->intLine, dev->vector, 'A'+dev->intPin);
	 } else {
	    snprintf(intBuf, sizeof intBuf, "%2d/%3d/0x%02x %c",
			dev->intLine, dev->irq, dev->vector, 'A'+dev->intPin);
	 }
      } else {
	 if (dev->flags & PCI_DEVICE_PCI_BRIDGE) {
	    snprintf(intBuf, sizeof intBuf, "    %03d", dev->spawnedBus);
	 } else {
	    intBuf[0] = 0;
	 }
      }

      Proc_Printf(buffer, len, FORMAT,
	dev->busAddress, dev->vendorSignature, type, vendor, intBuf,
	dev->flags & PCI_DEVICE_HOST ? "C" :
		dev->flags & PCI_DEVICE_SHARED ? "S" : "V",
	dev->moduleID == MOD_ID_NONE ? "" :
		dev->moduleID == MOD_ID_UNKNOWN ? "unknown" :
		Mod_GetName(dev->moduleID, modName) ? modName : "error",
	dev->name);
   }

   return VMK_OK;
}
