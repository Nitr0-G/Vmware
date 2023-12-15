/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. 
 * -- VMware Confidential
 * **********************************************************/

/*
 * vmkernel_exports.c  --
 *
 *   Interface to vmkernel networking for the vmkernel itself.
 */

#include "vmkernel.h"
#include "vm_libc.h"
#include "libc.h"
#include "netDebug.h"
#include "parse.h"

#define LOGLEVEL_MODULE_LEN 0
#define LOGLEVEL_MODULE Net
#include "log.h"

#include "net_int.h"
#include "legacy_esx2.h"

#include "socket_dist.h"
#include "memmap_dist.h"

static VMK_ReturnStatus NetDisconnect(World_ID worldID, Net_PortID portID);

#ifdef ESX3_NETWORKING_NOT_DONE_YET

Net_StackFunctions stackFunctions;
/*
 *----------------------------------------------------------------------------
 *
 * Net_TcpipStackLoaded:
 *   Is the TCP stack currently loaded?
 *
 * Results:
 *   TRUE if the TCP stack is loaded, FALSE otherwise.
 *
 *----------------------------------------------------------------------------
 */
Bool Net_TcpipStackLoaded(void)
{
   Net_StackFunctions nullStackFunctions;

   memset(&nullStackFunctions, 0, sizeof(nullStackFunctions));

   if (memcmp((char *)&stackFunctions, 
              (char *)&nullStackFunctions, 
              sizeof(nullStackFunctions))) {
      return TRUE;
   }

   return FALSE;
}

VMK_ReturnStatus
EtherSwitch_SetMACAddr(Net_PortID portID, MACAddr macAddr)
{
   return VMK_FAILURE;
}
#endif


/*
 *----------------------------------------------------------------------
 *
 * Net_GetIPAddr --
 *
 *      Return the IP address from a string.
 *
 * Results: 
 *	The IP address.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
uint32
Net_GetIPAddr(const char *cp)
{
   char *buf, *h;
   int i;
   uint32 a = 0;

   buf = (char *)Mem_Alloc(strlen(cp) + 1);
   ASSERT(buf != NULL);
   strcpy(buf, cp);
   h = buf;
   for (i = 0; i < 4; i++) {
      uint32 digit;
      const char *s = h;
      while (*h != '.' && *h != 0) {
         h++;
      }
      if (*h == 0 && i != 3) {
	 /*
	  * This is a malformed IP address.
	  */
	 a = 0;
	 break;
      }
      *h = 0;
      h++;
      Parse_Int(s, strlen(s), &digit);
      a = (a << 8) | digit;
   }

   Mem_Free(buf);

   return a;
}


/*
 *----------------------------------------------------------------------
 *
 * Net_EarlyInit --
 *
 *      module load time intialization
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	memory may be early allocated, etc.
 *
 *----------------------------------------------------------------------
 */
void
Net_EarlyInit(void)
{
   Portset_ModEarlyInit();
   Uplink_ModEarlyInit();
}

/*
 *----------------------------------------------------------------------
 *
 * Net_Init --
 *
 *      Initialize networking
 *
 *      XXX needs work for modulization
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	many: locks may be intialized, memory allocated, etc.
 *
 *----------------------------------------------------------------------
 */
void
Net_Init(VMnix_SharedData *sharedData)
{
   VMK_ReturnStatus status;

   LOG(1, "");

   status = ProcNet_ModInit();
   if (status != VMK_OK) {
      goto fail;
   }  

   status = Pkt_ModInit();
   if (status != VMK_OK) {
      goto fail;
   }     

   status = Portset_ModInit(128);
   if (status != VMK_OK) {
      goto fail;
   }

   status = Uplink_ModInit();
   if (status != VMK_OK) {
      goto fail;
   }
   
   NetDebug_Init();

   status = Bond_ModInit();
   if (status != VMK_OK) {
      goto fail;
   }

   NetProc_Init();

   LOG(0, "success");

   return;

  fail:
   Warning("can't intialize networking: %s", 
           VMK_ReturnStatusToString(status));
   Net_Cleanup();
}

/*
 *----------------------------------------------------------------------
 *
 * Net_Cleanup --
 *
 *      Cleanup the networking module.
 *
 * Results: 
 *	Networking resources are released.
 *
 * Side effects:
 *	Many.
 *
 *----------------------------------------------------------------------
 */
void
Net_Cleanup(void)
{
   NetDebug_Shutdown(NULL);
   Bond_ModCleanup();
   Uplink_ModCleanup();
   Portset_ModCleanup();
   Pkt_ModCleanup();
   ProcNet_ModCleanup();

   NetProc_Cleanup();
}


/*
 *----------------------------------------------------------------------
 *
 * Net_WorldInit --
 *
 *      Create net proc entries for the world.
 *      Init halt check info for device handles opened by this world's 
 *      group if the leader.
 *
 * Results: 
 *	VMK_OK.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
Net_WorldInit(struct World_Handle *world, struct World_InitArgs *args)
{
   ASSERT(world);
   ASSERT(!world->netInitialized);

   if (World_IsVmmLeader(world)) {
      World_VMMGroup(world)->netInfo.numPorts = 0;
   }

   LOG(3, "world %s initialized", world->worldName);

   world->netInitialized = TRUE;
   return VMK_OK; // can't fail or double fault in init
}


/*
 *----------------------------------------------------------------------
 *
 * Net_WorldPreCleanup --
 *
 *      Called when the world is about to die.  Normal cleanup funcitons
 *      haven't been called yet.  Should release all references to the
 *      World_Handle pointer before returning from this function.
 *
 *      Here's a description of the little dance we do when a VM exits
 *      and it's VCPU thread worlds die (so far no other type of world 
 *      needs to associate itself explicitly with the ports it connects 
 *      to)
 *
 *           1) when a non-leader VMM world is dying we disassociate
 *              the world from all ports it was associated with which
 *              will:
 *
 *                  a) remove the world from each port's world array
 *
 *                  b) release each port's reference count on the world
 *
 *           2) when a VMM leader world is dying we disconnect all the 
 *              group's ports, since nothing useful can be done with a
 *              port once the leader is gone.  Disconnecting the port 
 *              will:
 *
 *                  a) remove each port from the group's port array.
 *           
 *                  b) remove all the group's worlds from each port's 
 *                     world array
 *
 *                  c) release each port's reference count on each world
 *
 *              now when any remaining non-leader VMM worlds from the 
 *              group begin their death sequence and call into here, 
 *              they find that that the grup has no ports open and we 
 *              don't need to do anything here for them.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	Ports may be disconnected, world references may be released.
 *
 *----------------------------------------------------------------------
 */
void
Net_WorldPreCleanup(World_Handle* world)
{
   unsigned int i, j;
   unsigned int numPorts;
   Net_PortID *portIDs;
   VMK_ReturnStatus status;

   if (!World_IsVMMWorld(world)) {
      // nothing to do
      return;
   }

   /*
    * this lock protects us from colliding with normal disconnects
    * as well as other worlds trying to die.
    */
   Portset_GlobalLock();

   numPorts = World_VMMGroup(world)->netInfo.numPorts;
   portIDs = World_VMMGroup(world)->netInfo.portIDs;

   LOG(0, "worldID %u has %u associated ports", world->worldID, numPorts);

   /*
    * Note that we keep killing the j-th elememnt of the array.
    * That's because NetDisconnect() repacks the array every
    * time it removes an element, so we can't do a normal iteration
    */
   for (i = 0, j = 0; i < numPorts; i++) {
      Net_PortID portID = portIDs[j];

      LOG(1, "portID 0x%x", portID);

      if (World_IsVmmLeader(world)) {
         // can't use the port with the leader gone, so disconnect it
         LOG(1, "portID 0x%x: worldID %u is leader", portID, world->worldID);
         status = NetDisconnect(world->worldID, portID);
         if (status != VMK_OK) {
            Warning("cannot disconnect portID 0x%x for worldID %u pre-cleanup: %s", 
                    world->worldID, portID, VMK_ReturnStatusToString(status));
            /*
             * The portID is always removed from the group's array *unless*
             * NetDisconnect() failed, so we only increment the index for
             * this case
             */
            j++; 
         }
      } else {
         // port *might* still be useful, but we need to release this world
         LOG(1, "portID 0x%x: worldID %u is nonleader", portID, world->worldID);
         status = Port_DisassociateVmmWorld(portID, world);
         if (status != VMK_OK) {
            Warning("cannot disassociate portID 0x%x from worldID %u pre-cleanup: %s", 
                    world->worldID, portID, VMK_ReturnStatusToString(status));
         }
         /*
          * in this case the portID is *never* removed from the group's
          * array so always increment the index
          */
         j++;  
      }
   }

   Portset_GlobalUnlock();
}

/*
 *----------------------------------------------------------------------
 *
 * Net_WorldCleanup --
 *
 *      Don't need to do anything here but check to make sure that
 *      Net_WorldPreCleanup did it's job.
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
Net_WorldCleanup(struct World_Handle* targetWorld)
{
   unsigned int numPorts;
   Net_PortID *portIDs;

   /*
    * this lock protects us from colliding with normal disconnects
    * as well as other worlds trying to die.
    */
   Portset_GlobalLock();

   numPorts = World_VMMGroup(targetWorld)->netInfo.numPorts;
   portIDs = World_VMMGroup(targetWorld)->netInfo.portIDs;

   if (World_IsVmmLeader(targetWorld) && (numPorts > 0)) {
      VmWarn(targetWorld->worldID, 
             "killing leader world with %u active network ports", numPorts);
      ASSERT(FALSE);
   } else {
      unsigned int i, j;

      for (i = 0; i < numPorts; i++) {
         Port *port = Portset_GetPortExcl(portIDs[i]);
         if (port != NULL) {
            for (j = 0; j < port->numWorlds; j++) {
               World_Handle *world = port->worldArr[j];
               
               if (world == targetWorld) {
                  VmWarn(targetWorld->worldID, 
                         "killing world still associated with network port 0x%x on %s", 
                         port->portID, port->ps->name);
                  ASSERT(FALSE);
               }
            }
            Portset_ReleasePortExcl(port);
         } else {
            VmWarn(targetWorld->worldID, 
                   "world's group associated with bad network port 0x%x", 
                   port->portID);
            ASSERT(FALSE);            
         }
      }
   }

   Portset_GlobalUnlock();
}


/*
 *----------------------------------------------------------------------
 *
 * Net_Create --
 *
 *      Create a virtual network device of the given type with the 
 *      given name, with the given number of ports.
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
Net_Create(char *name, Net_Type type, unsigned int n)
{
   Portset *ps = NULL;
   VMK_ReturnStatus status;

   LOG(0, "%s: request create", name);

   Portset_GlobalLock();

   status = Portset_Activate(n, name, &ps);
   if (status != VMK_OK) {
      Warning("can't create portset: %s", 
              VMK_ReturnStatusToString(status));
      goto fail;
   }

   switch (type) {
   case NET_TYPE_NULL:
      status = Nulldev_Activate(ps);
      break;
   case NET_TYPE_LOOPBACK:
      status = Loopback_Activate(ps);
      break;
   case NET_TYPE_HUBBED:
      status = Hub_Activate(ps);
      break;
   case NET_TYPE_BOND:
      status = Bond_Activate(ps);
      break;
   case NET_TYPE_ETHER_SWITCHED:
      status = VMK_NOT_IMPLEMENTED;
      break;
   case NET_TYPE_INVALID:
      status = VMK_BAD_PARAM;
      break;
   }


   if (status != VMK_OK) {
      Warning("%s: can't create device: %s", 
              name, VMK_ReturnStatusToString(status));
      goto fail;
   }

   ps->type = type;
   LOG(0, "%s: created", name);
   goto done;

  fail:
   
   if (ps != NULL) {
      Portset_Deactivate(ps);
   }

  done:

   if (ps != NULL) {
      Portset_UnlockExcl(ps);
   }

   Portset_GlobalUnlock();

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * Net_Destroy --
 *
 *      Destroy a virtual network.
 *
 * Results: 
 *	returns a VMK_ReturnStatus.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Net_Destroy(char *name)
{
   Portset *ps;
   VMK_ReturnStatus status;
   unsigned int l;

   LOG(0, "%s: request destroy", name);

   Portset_GlobalLock();

   l = strlen(name);
   if (l > MAX_PORTSET_NAMELEN) {
      Warning("%s: name too long (limit is %u)", name, MAX_PORTSET_NAMELEN);
      status = VMK_BAD_PARAM;
      ASSERT(FALSE);
      goto done;
   }
   
   status = Portset_FindByName(name, &ps);
   if (status != VMK_OK) {
      Warning("%s: not found", name);
      goto done;
   }

   Portset_LockExcl(ps);
   Portset_Deactivate(ps);
   Portset_UnlockExcl(ps);

   LOG(0, "%s: destroyed", name);
   status = VMK_OK;

  done:

   Portset_GlobalUnlock();

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Net_Connect --
 *
 *      Connect to a virtual network by initializing and connecting a
 *      new port on the named network device.
 *
 * Results: 
 *	returns a VMK_ReturnStatus.
 *      if VMK_OK, PortID contains the ID of the new port.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Net_Connect(World_ID worldID, const char *name, Net_PortID *portID)
{
   Portset *ps = NULL;
   Port *port = NULL;
   VMK_ReturnStatus status = VMK_FAILURE;

   Portset_GlobalLock();

   status = Portset_FindByName(name, &ps);
   if (status != VMK_OK) {
      if (Net_CreatePortsetESX2(name) == VMK_OK) {
         status = Portset_FindByName(name, &ps);
      }

      if (status != VMK_OK) {
         ps = NULL;
         goto done;
      }
   }
   Portset_LockExcl(ps);

   status = Portset_ConnectPort(ps, &port);
   if (status != VMK_OK) {
      goto done;
   }
   *portID = port->portID;

   if (worldID != INVALID_WORLD_ID) {
      World_Handle *world = NULL;
      world = World_Find(worldID);
      ASSERT(world);

      if (World_IsVMMWorld(world)) {
         status = Port_AssociateVmmWorldGroup(port, worldID);
      } else if (World_IsHOSTWorld(world)) {
         status = Port_AssociateCOSWorld(port, worldID);
      } else {
         NOT_REACHED();
      }
      World_Release(world);

      if (status != VMK_OK) {
         goto done;
      }
   }

   LOG(0, "connected to net %s, PortID = 0x%x", name, port->portID);

  done:

   if (status != VMK_OK) {
      Log("can't connect device: %s: %s", name, VMK_ReturnStatusToString(status));
      if (port != NULL) {
         ASSERT(ps != NULL);
         Portset_DisconnectPort(ps, *portID);
      }
      *portID = NET_INVALID_PORT_ID;
   }

   if (ps != NULL) {
      Portset_UnlockExcl(ps);
   }

   Portset_GlobalUnlock();

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * NetDisconnect --
 *
 *      Disconnect from a virtual network.
 *
 * Results: 
 *	returns a VMK_ReturnStatus.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
NetDisconnect(World_ID worldID, Net_PortID portID)
{
   VMK_ReturnStatus status;
   Portset *ps;
   Port *port;

   ps = Portset_FindByPortID(portID);
   if (ps == NULL) {
      status = VMK_NOT_FOUND;
      goto done;
   }

   Portset_LockExcl(ps);

   if (!Portset_IsActive(ps)) {
      status = VMK_INVALID_HANDLE;
      Log("0x%x: portset not active", portID);
      goto done;
   }

   status = Portset_GetLockedPort(portID, &port);
   if (status != VMK_OK) {
      Log("0x%x: can't access port", portID);
      goto done;
   }

   if (worldID != INVALID_WORLD_ID) {
      status = Port_CheckWorldAssociation(port, worldID);
      if (status != VMK_OK) {
         Log("0x%x: port doesn't belong to world 0x%x", portID, worldID);
         goto done;
      }
   }

   status = Portset_DisconnectPort(ps, portID);

   if (status == VMK_OK) {
      LOG(0, "disconnected from net %s, PortID = 0x%x", ps->name, portID);
   }

  done:

   if (ps) {
      Portset_UnlockExcl(ps);
   }

   return status;
}



/*
 *----------------------------------------------------------------------
 *
 * Net_Disconnect --
 *
 *      locking wrapper for NetDisconnect()
 *
 * Results: 
 *	returns a VMK_ReturnStatus.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Net_Disconnect(World_ID worldID, Net_PortID portID)
{
   VMK_ReturnStatus status;

   Portset_GlobalLock();

   status = NetDisconnect(worldID, portID);

   Portset_GlobalUnlock();

   return status;
}


/*
 *----------------------------------------------------------------------------
 *
 * Net_ConnectUplinkPort --
 *
 *    External entry point for connecting an uplink port to a portset.
 *    Finds the specified portset and calls the implementation specific 
 *    uplink attach function.
 *
 *  Results:
 *    VMK_OK on success, VMK_FAILURE on failure.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

VMK_ReturnStatus
Net_ConnectUplinkPort(char *portsetName, char *uplinkDevName, Net_PortID *portID)
{
   Portset *ps;
   VMK_ReturnStatus ret;
   ASSERT(uplinkDevName);
   Portset_GlobalLock();
   ret = Portset_FindByName(portsetName, &ps);
   if (ret == VMK_OK) {
      Portset_LockExcl(ps);
      if (Portset_IsActive(ps) && (ps->devImpl.uplinkConnect)) {
         ret = ps->devImpl.uplinkConnect(ps, uplinkDevName, portID);
      } else {
         ret = VMK_FAILURE;
      }
   
      Portset_UnlockExcl(ps);
   } else {
      ret = VMK_FAILURE;
   }
   Portset_GlobalUnlock();

   return ret;
}


/*
 *----------------------------------------------------------------------------
 *
 * Net_DisconnectUplinkPort --
 *
 *    External entry point to disconnect an uplink port from a portset.
 *    Clients must call this function instead PortSet_DisconnectPort() to
 *    ensure that lock ordering semantics are maintained.
 *
 * Results:
 *    VMK_OK if the operation was successful. VMK_FAILURE otherwise.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

VMK_ReturnStatus
Net_DisconnectUplinkPort(char *portsetName, char *uplinkName)
{
   Portset *ps;
   VMK_ReturnStatus ret;
   Portset_GlobalLock();
   ret = Portset_FindByName(portsetName, &ps);
   if (ret == VMK_OK) {
      Portset_LockExcl(ps);
      if (Portset_IsActive(ps) && (ps->devImpl.uplinkDisconnect)) {
         ret = ps->devImpl.uplinkDisconnect(ps, uplinkName);
      } else {
         ret = VMK_FAILURE;
      }

      Portset_UnlockExcl(ps);
   } else {
      ret = VMK_FAILURE;
   }
   Portset_GlobalUnlock();

   return ret;
}

/*
 *----------------------------------------------------------------------------
 *
 * Net_PortEnable --
 *
 *    Enable the port.
 * 
 * Results:
 *    VMK_ReturnStatus.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

VMK_ReturnStatus 
Net_PortEnable(Net_PortID portID)
{
   VMK_ReturnStatus status;
   Port *port = Portset_GetPortExcl(portID);

   if (port == NULL) {
      status = VMK_NOT_FOUND;
   } else {
      status = Port_Enable(port);
      Portset_ReleasePortExcl(port);
   }

   LOG(1, "0x%x %s", portID, VMK_ReturnStatusToString(status));
   return status;
}


/*
 *----------------------------------------------------------------------------
 *
 * Net_PortDisable --
 *
 *    Disable the port.
 * 
 * Results:
 *    VMK_ReturnStatus.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

VMK_ReturnStatus 
Net_PortDisable(Net_PortID portID, Bool force)
{
   VMK_ReturnStatus status;
   Port *port = Portset_GetPortExcl(portID);

   if (port == NULL) {
      status = VMK_NOT_FOUND;
   } else {
      status = Port_Disable(port, force);
      Portset_ReleasePortExcl(port);
   }

   LOG(1, "0x%x %s", portID, VMK_ReturnStatusToString(status));
   return status;
}


/*
 *----------------------------------------------------------------------------
 *
 * Net_GetRawCapabilities --
 *
 *    Return the capabilities associated with the port. For now, we return
 *    zero.
 * 
 * Results:
 *    VMK_OK.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

VMK_ReturnStatus
Net_GetRawCapabilities(Net_PortID portID, uint32 *capabilities)
{
#ifdef ESX3_NETWORKING_NOT_DONE_YET
   ASSERT(capabilities);
   *capabilities = 0;
   return VMK_OK;
#else
#error capabilities not implemented.
#endif
}


/*
 *----------------------------------------------------------------------------
 *
 * Net_FindDevice --
 *
 *    Find the specified logical device(vmnic%d, bond%d, ...).
 *
 * Results:
 *    The requested UplinkDevice on success, NULL on failure.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

void *
Net_FindDevice(const char *name)
{
   return Uplink_GetImpl(name);
}


/*
 *----------------------------------------------------------------------
 *
 * Net_TX --
 *
 *      Transmit packets on port.
 *
 * Results: 
 *	returns a VMK_ReturnStatus.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
Net_Tx(Net_PortID portID, PktList *pktList)
{
   struct Port *port;
   VMK_ReturnStatus status = Portset_GetPort(portID, &port);
   if (status == VMK_OK) {
      status = Port_Input(port, pktList);
      Portset_ReleasePort(port);
   }
   return status;
}



/*
 *----------------------------------------------------------------------------
 *
 * Net_TcpipTx --
 *   Transmits a pktList originating from the vmkernel tcpip stack
 *
 *
 *  Results:
 *    VMK_OK on success, VMK_FAILURE otherwise.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */ 
VMK_ReturnStatus
Net_TcpipTx(PktList *pktList)
{
   VMK_ReturnStatus status = VMK_OK;
   Net_PortID portID;
   Port *port = NULL;
   
   PktHandle *pkt = NULL;

   
   pkt = PktList_GetHead(pktList);
   
   if (UNLIKELY(pkt == NULL)) {
      return VMK_OK;
   }
   portID = PktGetSrcPort(pkt);

   status = Portset_GetPort(portID, &port);

   if (UNLIKELY(status != VMK_OK)) {
      return status;
   }

   status = Port_Input(port, pktList);
   
   if (port) {
      Portset_ReleasePort(port);
   }
   
   return status;
}

/*
 *----------------------------------------------------------------------------
 *
 * Net_TxOne --
 *
 *    Handle one buffer coming in on the specified port. A master packet handle
 *    is created for the specified buffer and an SG_MA is built to describe it.
 *    The input chain for the port is invoked before being dispatched by the
 *    portset.
 *
 *  Results:
 *    VMK_OK on success, VMK_FAILURE otherwise.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */ 

VMK_ReturnStatus
Net_TxOne(Net_PortID portID, void *srcBuf, uint32 srcBufLen, uint32 flags)
{
   VMK_ReturnStatus ret = VMK_OK;
   Port *port;
   ret = Portset_GetPort(portID, &port);
   if (ret == VMK_OK) {
      // XXX: alignment required??
      PktHandle *pkt = NULL;
      PktHandle *tmpPkt = Pkt_Alloc(0, 0);
      if (tmpPkt) {
         PktSetBufType(tmpPkt, NET_SG_MACH_ADDR);
         PktSetSrcPort(tmpPkt, portID);

         /* build the tmpPkt sgMA to describe the buffer */
         ret = Pkt_AppendFrag(VMK_VA2MA((VA)srcBuf), srcBufLen, tmpPkt);
         if (ret == VMK_OK) {
            uint32 padLen = MAX(srcBufLen, MIN_TX_FRAME_LEN) - srcBufLen;
            if (UNLIKELY(padLen > 0)) {
               ret = Pkt_PadWithZeroes(tmpPkt, padLen);
            }

            if (ret == VMK_OK) {
               PktSetFrameLen(tmpPkt, srcBufLen + padLen);
               pkt = Pkt_PartialCopy(tmpPkt, Portset_GetMaxUplinkImplSz(port->ps),
                                     INFINITY);
               if (pkt) {
                  ret = Port_InputOne(port, pkt);
               } else {
                  ret = VMK_NO_RESOURCES;
               }
            }
         }
         Pkt_Release(tmpPkt);
      } else {
         ret = VMK_NO_RESOURCES;
      }
      Portset_ReleasePort(port);
   } else {
      ret = VMK_NOT_FOUND;
   }

   return ret;
}


/*
 *----------------------------------------------------------------------------
 *
 * Net_RawTXOneLocked --
 *
 *    Handle one buffer coming in on the specified port. A master packet handle
 *    is created for the specified buffer and an SG_MA is built to describe it.
 *    The input chain for the port is invoked before being dispatched by the
 *    portset.
 *
 *  Results:
 *    VMK_OK on success, VMK_FAILURE otherwise.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */
 
VMK_ReturnStatus
Net_RawTXOneLocked(Net_PortID portID, void *srcBuf, uint32 srcBufLen, uint32 flags)
{
   VMK_ReturnStatus ret = VMK_OK;
   Port *port;
   ret = Portset_GetLockedPort(portID, &port);
   if (ret == VMK_OK) {
      // XXX: alignment required??
      PktHandle *pkt = Pkt_Alloc(Portset_GetMaxUplinkImplSz(port->ps), srcBufLen);
      if (pkt) {
         PktSetSrcPort(pkt, portID);
         Pkt_AppendBytes(srcBuf, srcBufLen, pkt);

         ret = Port_InputOne(port, pkt);
      } else {
         LOG(0, "Failed to allocate memory for tx on port 0x%x(%p)", portID,
             port);
         ret = VMK_FAILURE;
      }
   } else {
      LOG(0, "Couldn't get port for portID 0x%x", portID);
      ret = VMK_FAILURE;
   }

   return ret;
}


/*
 *----------------------------------------------------------------------------
 *
 * Net_SetRawTxCompletionCB --
 *
 *    Set the tcp/ip tx callback handler for the given port. Inserts a generic
 *    handler at edge of the port's output function. 
 *
 * Results:
 *    The outcome of the IOChain_InsertCall operation.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

VMK_ReturnStatus
Net_SetRawTxCompleteCB(Net_PortID portID, NetRawCBData *cbArg)
{
   VMK_ReturnStatus ret;
   Port *port = Portset_GetPortExcl(portID);
   if (port != NULL) {
      Log("Setting Tx-complete cb for port");
      ret = IOChain_InsertCall(&port->notifyChain, 
                               IO_CHAIN_RANK_TERMINAL, 
                               cbArg->routine,
                               NULL, NULL,
                               cbArg->data, FALSE, NULL);
      Portset_ReleasePortExcl(port);
   } else {
      ret = VMK_FAILURE;
   }
   return ret;
}


/*
 *----------------------------------------------------------------------------
 *
 * Net_SetRawCB --
 *
 *    Set the tcp/ip callback handler for the given port. Inserts a generic
 *    handler at edge of the port's output function. 
 *
 * Results:
 *    The outcome of the IOChain_InsertCall operation.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

VMK_ReturnStatus
Net_SetRawCB(Net_PortID portID, NetRawCBData *cbArg)
{
   VMK_ReturnStatus ret;
   Port *port = Portset_GetPortExcl(portID);
   if (port != NULL) {
      Log("Setting cb for port");
      ret = IOChain_InsertCall(&port->outputChain, 
                               IO_CHAIN_RANK_TERMINAL, 
                               cbArg->routine,
                               NULL, NULL,
                               cbArg->data, TRUE, NULL);
      Portset_ReleasePortExcl(port);
   } else {
      ret = VMK_FAILURE;
   }
   return ret;
}


/*
 *----------------------------------------------------------------------------
 *
 * Net_Sum --
 *
 *    Compute the sum of the bytes in the buffer.
 *
 *
 * Results:
 *    The sum of the bytes in the buffer. carry is set if the len is odd.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

void
Net_Sum(void *src, int len, uint32 *sum, int *carry)
{
   int nleft = len;
   uint16 *srcw = (uint16 *)src;
   uint32 acc = 0;

   while (nleft > 1)  {
      acc += *srcw++;
      nleft -= 2;
   }

   // deal with the case where we finished on an odd byte
   if (nleft == 1) {
      acc += *(unsigned char *)srcw;
   }

   // deal with the case where we started on an odd byte
   if (*carry) {
      // fold in the high 16 bits of the sum before byteswap
      while (acc >> 16) {
         acc = (acc >> 16) + (acc & 0xffff);
      }
      *sum += (acc >> 8 & 0xff) | (acc << 8 & 0xff00);
   } else {
      *sum += acc;
   }

   *carry = nleft;
}

/*
 *----------------------------------------------------------------------------
 *
 * Net_GetProcRoot --
 *
 *    Get the networking proc root node.
 *
 * Results:
 *    The root proc node for networking is returned.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

Proc_Entry *
Net_GetProcRoot(void)
{
   return ProcNet_GetRootNode();
}


#ifdef ESX2_NET_SUPPORT

/*
 *----------------------------------------------------------------------------
 *
 * Net_ConnectBondUplinkPort --
 *
 *    External entry point for connecting an uplink port to a portset.
 *    Finds the specified portset and calls the implementation specific 
 *    uplink attach function.
 *
 *  Results:
 *    VMK_OK on success, VMK_FAILURE on failure.
 *
 *  Side effects:
 *    None.
 *
 *  Remarks:
 *    DO NOT USE THIS FUNCTION. IT IS PRESENT TO SERVE SOME LEGACY PATHS.
 *
 *----------------------------------------------------------------------------
 */

VMK_ReturnStatus
Net_ConnectBondUplinkPort(char *portsetName, char *uplinkDevName, Net_PortID *portID)
{
   Portset *ps = NULL, *legacyPs = NULL;
   VMK_ReturnStatus ret;
   ASSERT(uplinkDevName);
   ASSERT(!strncmp(portsetName, "bond", 4));

   Portset_GlobalLock();

   ret = Portset_FindByName(portsetName, &ps);
   if (ret != VMK_OK) {
      goto done;
   }

   Portset_LockExcl(ps);
   if (Portset_IsActive(ps)){
      UplinkDev *dev = ps->uplinkDev;
      if (dev) {
         ret = Portset_FindByName(dev->devName, &legacyPs);
         if (ret != VMK_OK) {
            goto done;
         }
         Portset_LockExcl(legacyPs);
         ASSERT(!strncmp(legacyPs->name, "legacyBond", 10));
         if (Portset_IsActive(legacyPs) && legacyPs->devImpl.uplinkConnect) {
            ret = legacyPs->devImpl.uplinkConnect(legacyPs, uplinkDevName,
                                                  portID);
         } else {
            Log("%s not active", legacyPs->name);
            ret = VMK_FAILURE;
         }
      } else {
         Log("%s not yet connected to any bond portsets", portsetName);
         ret = VMK_FAILURE;
      }
   } else {
      Log("%s not yet active", portsetName);
      ret = VMK_FAILURE;
   }

done:
   if (legacyPs) {
      Portset_UnlockExcl(legacyPs);
      
   }
   if (ps) {
      Portset_UnlockExcl(ps);
   }
   Portset_GlobalUnlock();

   return ret;
}

VMK_ReturnStatus
Net_DisconnectBondUplinkPort(char *portsetName, char *uplinkDevName)
{
   Portset *ps = NULL, *legacyPs = NULL;
   VMK_ReturnStatus ret;
   ASSERT(uplinkDevName);
   ASSERT(!strncmp(portsetName, "bond", 4));

   Portset_GlobalLock();

   ret = Portset_FindByName(portsetName, &ps);
   if (ret != VMK_OK) {
      goto done;
   }

   Portset_LockExcl(ps);
   if (Portset_IsActive(ps)){
      UplinkDev *dev = ps->uplinkDev;
      if (dev) {
         ret = Portset_FindByName(dev->devName, &legacyPs);
         if (ret != VMK_OK) {
            goto done;
         }
         Portset_LockExcl(legacyPs);
         ASSERT(!strncmp(legacyPs->name, "legacyBond", 10));
         if (Portset_IsActive(legacyPs) && legacyPs->devImpl.uplinkDisconnect) {
            ret = legacyPs->devImpl.uplinkDisconnect(legacyPs, uplinkDevName);
         } else {
            Log("%s not active", legacyPs->name);
            ret = VMK_FAILURE;
         }
      } else {
         Log("%s not yet connected to any bond portsets", portsetName);
         ret = VMK_FAILURE;
      }
   } else {
      Log("%s not yet active", portsetName);
      ret = VMK_FAILURE;
   }

done:
   if (legacyPs) {
      Portset_UnlockExcl(legacyPs);
      
   }
   if (ps) {
      Portset_UnlockExcl(ps);
   }
   Portset_GlobalUnlock();

   return ret;
}
#endif
