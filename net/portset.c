/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. 
 * -- VMware Confidential
 * **********************************************************/

/*
 * portset.c  --
 *
 *   Portsets are groups of ports which, together with policies for
 *   frame routing, form virtual networks.  The portset structure and
 *   API form a base class of virtual network device, upon which more
 *   useful classes of device like etherswitches may be built, by
 *   simply implementing a frame routing policy and other device 
 *   specific behavior.
 */

#include "vmkernel.h"
#define LOGLEVEL_MODULE_LEN 0
#define LOGLEVEL_MODULE Net
#include "log.h"

#include "net_int.h"
#include "legacy_esx2.h"


uint32           numPortsets;
Portset         *portsetArray;
uint32           portsetIdxMask;
unsigned int     portsetIdxShift;
Proc_Entry       portsetProcDir;
SP_SpinLock      portsetGlobalLock;


/*
 * XXX find a better home for this, 
 *     also, not that we care for the usage in this file, but
 *     there is probably a better way to implement, this was
 *     just the first I stumbled on and it works.
 */
static INLINE uint32
ceilingPower2(uint32 n)
{
   uint32 x = n - 1;   
   x |= x >> 1;
   x |= x >> 2;
   x |= x >> 4;
   x |= x >> 8;
   x |= x >> 16;
   return x + 1;
}


/*
 *----------------------------------------------------------------------
 *
 * Portset_ModEarlyInit --
 *
 *      Early initialization of the module, called once at load time.
 *
 * Results: 
 *	The module is made ready for run time intialization.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

void
Portset_ModEarlyInit(void)
{
   SP_InitLock("portsetGlobalLock", 
               &portsetGlobalLock, 
               SP_RANK_NET_PORTSET_GLOBAL);
}


/*
 *----------------------------------------------------------------------
 *
 * Portset_ModInit --
 *
 *      Initialization of the module, called at run time.
 *      XXX in the future we may enable recalling this function
 *          to dynamically resize arrays, etc, but an extra layer
 *          of synchronization will be necessary
 *
 * Results: 
 *	VMK_ReturnStatus.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
Portset_ModInit(unsigned int num)
{
   unsigned int sz, i;
   VMK_ReturnStatus status = VMK_OK;

   if (num == 0) {
      Warning("zero portsets specified, networking will not be enabled");
      ASSERT(FALSE);
      status = VMK_BAD_PARAM;
      goto done;
   }
   if (num > MAX_NUM_PORTSETS) {
      Warning("too many portsets: %u, limiting to %u",
              num, MAX_NUM_PORTSETS);
      ASSERT(FALSE);
      num = MAX_NUM_PORTSETS;
   }

   num = ceilingPower2(num);
   numPortsets = num;
   sz = numPortsets * sizeof(Portset);
   portsetArray = Mem_Alloc(sz);
   if (portsetArray == NULL) {
      Warning("cannot allocate  memeory for portset array");
      ASSERT(FALSE);
      status = VMK_NO_RESOURCES;
      goto done;
   }
   memset(portsetArray, 0, sz);
   portsetIdxMask = numPortsets - 1;
   portsetIdxShift = 32 - ffs(numPortsets);
   for (i = 0; i < numPortsets; i++) {
      Portset *ps = &portsetArray[i];
      SP_InitRWLock("Portset.lock", &ps->lock, 
                    SP_RANK_NET_PORTSET | SP_RANK_RECURSIVE_FLAG);
   }

   Proc_InitEntry(&portsetProcDir);
   portsetProcDir.parent = ProcNet_GetRootNode();
   ProcNet_Register(&portsetProcDir, "devices", TRUE);  

  done:
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Portset_ModCleanup --
 *
 *      Cleanup of the module.
 *
 *      XXX Lots to do here for safe unloding of the net module
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

void 
Portset_ModCleanup(void)
{
   unsigned int i;

   /*
    * XXX
    *
    * we take the global lock and the lock on each portset *only* to
    * satisfy ASSERTs in the accessors we call below, they don't
    * really protect us from anything since when we release, the other
    * contender(s) will fall into code which access the resources we
    * release here, and the code itself is possibly open to being
    * overwritten at that point if the module has been unloaded. some
    * external mechanism needs to actually protect us from being
    * entered during or after this call.
    */
   Portset_GlobalLock();

   if (portsetArray != NULL) {
      for (i = 0; i < numPortsets; i++) {
         Portset *ps = &portsetArray[i];
         Portset_LockExcl(ps); //just to satisfy ASSERTs
#ifndef ESX3_NETWORKING_NOT_DONE_YET
         // NetLogger will set this off
         ASSERT(!Portset_IsActive(ps) || !vmkernelLoaded);
#endif
         if (Portset_IsActive(ps)) {
            Portset_Deactivate(ps);
         }
         Portset_UnlockExcl(ps);
#ifdef ESX3_CLEANUP_EVERYTHING
         SP_CleanupRWLock(&ps->lock);
      }
      Mem_Free(portsetArray);
      portsetArray = NULL;
#else
      }
#endif
   }
   ProcNet_Remove(&portsetProcDir);

   Portset_GlobalUnlock();

#ifdef ESX3_CLEANUP_EVERYTHING
   SP_CleanupLock(&portsetGlobalLock);
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * Portset_FindByName --
 *
 *      Find the named portset.  
 *
 *      XXX This doesn't need to be fast for any current usage, 
 *          please implement a hash or something if that changes.
 *
 * Results:
 *      VMK_ReturnStatus 
 *	Pointer to requested Portset on success, NULL on failure.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
Portset_FindByName(const char *name, Portset **pps)
{
   unsigned int i;
   VMK_ReturnStatus status = VMK_NOT_FOUND;

   ASSERT(SP_IsLocked(&portsetGlobalLock));

   *pps = NULL;

   for (i = 0; i < numPortsets; i++) {
      if (strncmp(name, portsetArray[i].name, MAX_PORTSET_NAMELEN) == 0) {
         *pps = &portsetArray[i];
         status = VMK_OK;
         break;
      }
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * PortsetProcCreate --
 *
 *      Create a proc dir and populate it for the given portset.
 *
 * Results: 
 *	proc nodes are created.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
PortsetProcCreate(Portset *ps)
{
   if (UNLIKELY(VMK_STRESS_DEBUG_COUNTER(NET_PORTSET_PROC_CREATE_FAIL))) {
      return VMK_FAILURE;
   }

   Proc_InitEntry(&ps->procDir);
   ps->procDir.parent = &portsetProcDir;
   ProcNet_Register(&ps->procDir, ps->name, TRUE);

   Proc_InitEntry(&ps->procPortsDir);
   ps->procPortsDir.parent = &ps->procDir;
   ProcNet_Register(&ps->procPortsDir, "ports", TRUE);

   Proc_InitEntry(&ps->procNetDebug);
   ps->procNetDebug.read = NetDebug_ProcRead;
   ps->procNetDebug.write = NetDebug_ProcWrite; 
   ps->procNetDebug.parent = &ps->procDir;
   ps->procNetDebug.private = ps->name;
   ProcNet_Register(&ps->procNetDebug, "config", FALSE);

   NetProc_AddPortset(ps);
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * PortsetProcDestroy --
 *
 *      Cleanup the proc nodes for a portset.
 *
 * Results: 
 *	proc nodes are deleted.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
PortsetProcDestroy(Portset *ps)
{
   ProcNet_Remove(&ps->procPortsDir);
   ProcNet_Remove(&ps->procDir);
   ProcNet_Remove(&ps->procNetDebug);

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Portset_Deactivate --
 *
 *      Free the resources associated with a portset.
 *
 * Results: 
 *      VMK_ReturnStatus.
 *	The Portset array entry passed in is made available for use
 *      by a new virtual network and resources are released.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
Portset_Deactivate(Portset *ps)
{
   ASSERT(SP_IsLocked(&portsetGlobalLock));
   ASSERT(Portset_LockedExclHint(ps));

   if (ps->numPortsInUse != 0) {
      int i;
      LOG(0, "killing portset %s with %u connected ports", 
          ps->name, ps->numPortsInUse);
      ASSERT(ps->ports != NULL);
      for (i = 0; i < ps->numPorts; i++) {
         Port *port = &ps->ports[i];
         if (Port_IsEnabled(port)) {
            LOG(0, "%s: port 0x%x still enabled", ps->name, port->portID);
            Port_ForceDisable(port);
         }
         if (!Port_IsAvailable(port)) {
            LOG(0, "%s: port 0x%x still active", ps->name, port->portID);
            Port_Disconnect(port);
         }
      }
   }

   if (ps->devImpl.deactivate) {
      ps->devImpl.deactivate(ps);
   }

   NetProc_RemovePortset(ps);

   ps->flags = 0;
   // we do not reset ps->portgen here (so we can detect stale handles)
   ps->name[0] = 0;
   ps->portIdxMask = 0;
   ps->numPorts = 0;
   ps->numPortsInUse = 0;
   if (ps->ports != NULL) {
      Mem_Free(ps->ports);
      ps->ports = NULL;
   }
   PortsetProcDestroy(ps);

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Portset_Activate --
 *
 *      Find a free slot in the array of Portsets and initialize it
 *      for use as a virtual network.
 *
 * Results: 
 *      VMK_ReturnStatus.
 *	Pointer to initialized, and exclusively locked Portset in pps 
 *      on success.
 *
 * Side effects:
 *	The portset is returned with it's exclusive lock held, the
 *      caller must release the lock with PortsetExclusiveRelease()
 *      when finished.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
Portset_Activate(unsigned int numPorts, char *name, Portset **pps)
{
   Portset *ps = NULL;
   unsigned int l, i;
   VMK_ReturnStatus status;

   ASSERT(SP_IsLocked(&portsetGlobalLock));

   l = strlen(name);
   if (l > MAX_PORTSET_NAMELEN) {
      Warning("%s: name too long (limit is %u)", name, MAX_PORTSET_NAMELEN);
      status = VMK_BAD_PARAM;
      goto fail;
   }

   if (numPorts == 0) {
      Warning("%s: numPorts is zero", name);
      status = VMK_BAD_PARAM;
      goto fail;
   }

   if (numPorts > MAX_NUM_PORTS_PER_SET) {
      Warning("%s: too many ports (%u, limit is %u)", 
              name, numPorts, MAX_NUM_PORTS_PER_SET);
      status = VMK_BAD_PARAM;
      goto fail;
   }

   if (Portset_FindByName(name, &ps) != VMK_NOT_FOUND) {
      Warning("%s: already exists", name);
      ps = NULL;
      status = VMK_EXISTS;
      goto fail;
   }

   if (UNLIKELY(VMK_STRESS_DEBUG_COUNTER(NET_PORTSET_ACTIVATE_FAIL))) {
      status = VMK_FAILURE;
      goto fail;
   }

   status = Portset_FindByName("", &ps); // grab the first empty slot

   if (status == VMK_OK) {
      /*
       * Take the exclusive lock since it's possible that a stale reference
       * could map to this slot in the array while it is in an inconsistant
       * state below.
       */
      Portset_LockExcl(ps);
   } else {
      Warning("%s: no empty slots", name);
      status = VMK_NO_RESOURCES;
      goto fail;
   }

   ps->flags = PORTSET_FLAG_IN_USE;
   // do not reset ps->portgen (to detect stale handles)
   strcpy(ps->name, name);
   memset(&ps->devImpl, 0, sizeof(ps->devImpl));
   ps->numPorts = ceilingPower2(numPorts);
   ps->portIdxMask = ps->numPorts - 1;
   ps->numPortsInUse = 0;
   ps->devImpl.data = NULL;
   ps->ports = Mem_Alloc(ps->numPorts * sizeof(Port));

   if (UNLIKELY(VMK_STRESS_DEBUG_COUNTER(NET_PORTSET_ACTIVATE_MEM_FAIL))) {
      if (ps->ports) {
         Mem_Free(ps->ports);
         ps->ports = NULL;
      }
   }
   
   if (ps->ports == NULL) {
      Warning("%s: can't allocate port array", name);
      status = VMK_NO_RESOURCES;
      goto fail;
   }

   for (i = 0; i < ps->numPorts; i++) {
      Port_Init(&ps->ports[i], ps);
   }


   status = PortsetProcCreate(ps);
   if (status != VMK_OK) {
      Warning("%s: can't create proc nodes", name);
      goto fail;
   }
      
   Log("activated portset #%u as %s with %u %s, index mask is 0x%x", 
       Portset_GetIdx(ps), ps->name, ps->numPorts, 
       ps->numPorts > 1 ? "ports" : "port", ps->portIdxMask);

   status = VMK_OK;
   goto done;
   
  fail:
   
   if (ps != NULL) {
      Portset_Deactivate(ps);
      Portset_UnlockExcl(ps);
      ps = NULL;
   }
   
  done:    

   *pps = ps;
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * PortsetGeneratePortID --
 *
 *      Encode the set index, a generation count, and a port index into
 *      a new PortID and increment the generation counter.
 *
 * Results: 
 *	returns the next portID to be used for the given set.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static PortID
PortsetGeneratePortID(Portset *ps) 
{
   unsigned int setIndex = Portset_GetIdx(ps);
   PortID newPortID = NET_INVALID_PORT_ID;

   // skip the 1/2^32 case where we wrap to NET_INVALID_PORT_ID
   while (newPortID == NET_INVALID_PORT_ID) {
      ps->portgen++;
      newPortID = ps->portgen & ~(portsetIdxMask << portsetIdxShift);
      newPortID |= (setIndex & portsetIdxMask) << portsetIdxShift;
   }
   
   LOG(3, "%s: new PortID: 0x%x", ps->name, newPortID);

   return newPortID;
}


/*
 *----------------------------------------------------------------------
 *
 * PortsetFindPortByID --
 *
 *      Find the port referenced by the given ID.
 *
 * Results: 
 *	Returns a pointer to the port on success, NULL on failure.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static Port *
PortsetFindPortByID(Portset *ps, PortID portID)
{
   ASSERT(Portset_IdxFromPortID(portID) == Portset_GetIdx(ps));
   ASSERT(Portset_LockedExclHint(ps));

   if (portID == NET_INVALID_PORT_ID) {
      return NULL;
   }

   return &ps->ports[portID & ps->portIdxMask];
}


/*
 *----------------------------------------------------------------------
 *
 * Portset_UpdatePortEthFRP --
 *
 *      Notify the portset of and ethernet frame routing policy update.
 *
 * Results: 
 *	VMK_ReturnStatus
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus 
Portset_UpdatePortEthFRP(Port *port, Eth_FRP *frp)
{
   VMK_ReturnStatus status = VMK_OK;

   ASSERT(Portset_LockedExclHint(port->ps));

   if (port->ps->devImpl.portEthFRPUpdate) {
      status = port->ps->devImpl.portEthFRPUpdate(port, frp);
      if (status != VMK_OK) {
         LOG(0, "port 0x%x on portset %s: %s", port->portID, port->ps->name,
             VMK_ReturnStatusToString(status));
      }
   }
   
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Portset_DisablePort --
 *
 *      Disable a port on its parent portset.
 *
 * Results: 
 *	VMK_ReturnStatus
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus 
Portset_DisablePort(Port *port, Bool force)
{
   VMK_ReturnStatus status = VMK_OK;

   ASSERT(Portset_LockedExclHint(port->ps));

   if (UNLIKELY(VMK_STRESS_DEBUG_COUNTER(NET_PORTSET_DISABLE_PORT_FAIL))) {
      if (!force) {
         return VMK_FAILURE;
      }
   }

   if (port->ps->devImpl.portDisable) {
      status = port->ps->devImpl.portDisable(port, force);
      if (status != VMK_OK) {
         LOG(0, "port 0x%x on portset %s: %s", port->portID, port->ps->name,
             VMK_ReturnStatusToString(status));
      }
      // make sure they protect from being called again
      ASSERT((port->ps->devImpl.portDisable == NULL) || 
             !((status == VMK_OK) || force));
   }
   
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Portset_EnablePort --
 *
 *      Enable a port on its parent portset.
 *
 * Results: 
 *	VMK_ReturnStatus
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus 
Portset_EnablePort(Port *port)
{
   VMK_ReturnStatus status = VMK_OK;

   ASSERT(Portset_LockedExclHint(port->ps));

   if (UNLIKELY(VMK_STRESS_DEBUG_COUNTER(NET_PORTSET_ENABLE_PORT_FAIL))) {
      status =VMK_FAILURE;
      goto fail;
   }

   if (port->ps->devImpl.portEnable) {
      status = port->ps->devImpl.portEnable(port);
      if (status != VMK_OK) {
         goto fail;
      }
   }

   return status;

  fail:

   Log("port 0x%x on portset %s: %s", port->portID, port->ps->name,
       VMK_ReturnStatusToString(status));

   Port_Disable(port, TRUE);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Portset_ConnectPort --
 *
 *      Connect to a port on the given portset.
 *
 * Results: 
 *	VMK_ReturnStatus, and on success *port gets a pointer to the
 *      port which was connected.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus 
Portset_ConnectPort(Portset *ps, Port **port)
{
   unsigned int i;
   VMK_ReturnStatus status;

   ASSERT(SP_IsLocked(&portsetGlobalLock));
   ASSERT(Portset_LockedExclHint(ps));

   *port = NULL;

   if (UNLIKELY(VMK_STRESS_DEBUG_COUNTER(NET_PORTSET_CONNECT_PORT_FAIL))) {
      status = VMK_FAILURE;
      goto fail;
   }

   // find an empty slot
   for (i = 0; i < ps->numPorts; i++) {
      PortID newID; 
      Port *newPort; 

      newID = PortsetGeneratePortID(ps);

      newPort = &ps->ports[newID & ps->portIdxMask];

      if (Port_IsAvailable(newPort)) {
         status = Port_Connect(newPort, newID);
         if (status != VMK_OK) {
            goto fail;
         }
         *port = newPort;
         if (ps->devImpl.portConnect != NULL) {
            status = ps->devImpl.portConnect(ps, *port);
            if (status != VMK_OK) {
               goto fail;
            }            
         }

         ASSERT(ps->numPortsInUse < ps->numPorts);
         ps->numPortsInUse++;

         LOG(0, "newID 0x%x, newIDIdx 0x%x, psMask 0x%x, newPort %p, portsInUse %u", 
             newID, newID & ps->portIdxMask, ps->portIdxMask, newPort, 
             ps->numPortsInUse);

         return VMK_OK;
      }
   }
   /*
    * no empty slots
    * XXX here we could resize the portset and try again
    */
   status = VMK_NO_RESOURCES;

  fail:

   if (*port) {
      Portset_DisconnectPort(ps, (*port)->portID);
      *port = NULL;
   }
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Portset_DisconnectPort --
 *
 *      Disconnect the given port, making it available for reuse.
 *
 * Results: 
 *	VMK_ReturnStatus.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
Portset_DisconnectPort(Portset *ps, PortID portID)
{
   VMK_ReturnStatus status;
   Port *port;

   ASSERT(SP_IsLocked(&portsetGlobalLock));
   ASSERT(Portset_LockedExclHint(ps));

   LOG(3, "0x%x", portID);

   if (portID == NET_INVALID_PORT_ID) {
      Log("invalid PortID");      
      status = VMK_INVALID_HANDLE;
      goto done;
   }

   if (!Portset_IsActive(ps)) {
      Log("PortID %x stale or garbage, portset not in use", portID);
      ASSERT(FALSE);
      status = VMK_BAD_PARAM;
      goto done;
   }

   port = PortsetFindPortByID(ps, portID);
   if (port == NULL) {
      LOG(0, "no such port: 0x%x", portID);
      status = VMK_IS_DISCONNECTED;
      goto done;
   }

   if (Port_IsAvailable(port)) {
      LOG(0, "port not connected: 0x%x", portID);
      status = VMK_IS_DISCONNECTED;
      goto done;
   }

   if (Port_IsEnabled(port)) {
      Port_ForceDisable(port);
   }

   if (ps->devImpl.portDisconnect != NULL) {
      status = ps->devImpl.portDisconnect(ps, port);
      if (status != VMK_OK) {
         goto done;
      }            
   }

   status = Port_Disconnect(port);
   /*
    * LOOKOUT: can't fail after here bc Net_WorldPreCleanup()
    *          depends on it.
    */

  done:

   if (status == VMK_OK) {
      ps->numPortsInUse--;
   }

   return status;
}

