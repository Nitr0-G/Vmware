/* **********************************************************
 * Copyright (C) 2004 VMware, Inc. All Rights Reserved.
 * - VMware Confidential.
 * **********************************************************/


/*
 * hub.c --
 *
 *    The hub is one implementation of the portset that broadcasts every packet
 *    to each open port.
 */


#include "vmkernel.h"

#define LOGLEVEL_MODULE Net
#define LOGLEVEL_MODULE_LEN 0
#include "log.h"

#include "net_int.h"

typedef struct Hub {
   int uplinkPort;
   char uplinkDevName[VMNIX_DEVICE_NAME_LENGTH];
   Bool connected;
} Hub;



/*
 *----------------------------------------------------------------------------
 *
 * HubPortConnect --
 *
 *    Port connect Notification handler. This function is called by the portset
 *    whenever a port is connected on the portset associated with this hub.
 *
 * Results:
 *    VMK_OK always.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static VMK_ReturnStatus
HubPortConnect(Portset *ps, Port *port)
{
   LOG(1, "Port connected in portset %s", ps->name);
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------------
 *
 * HubPortDisconnect --
 *
 *    Port disconnect notification handler. Invoked when a port on the portset
 *    associated with this hub is disconnectd.
 *
 * Results:
 *    VMK_OK always.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static VMK_ReturnStatus
HubPortDisconnect(Portset *ps, Port *port)
{
   LOG(1, "Port disconnected. Portset %s", ps->name);
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------------
 *
 * HubPortSwitchingOutFilter --
 *
 *    Port switching output filter.
 *
 * Results:
 *    VMK_OK always.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */
VMK_ReturnStatus 
HubPortSwitchingOutFilter(Port *port, IOChainData iocd, PktList *pktList)
{
   Eth_FRP *frp = (Eth_FRP *)iocd;
   PktList filteredList;

   PktList_Init(&filteredList);

   Eth_DestinationFilter(&frp->outputFilter, pktList, &filteredList);

   // use this bc we don't know to which port(s) the pkt(s) belong
   PktList_CompleteAll(&filteredList);

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------------
 *
 * HubPortSwitchingInFilter --
 *
 *    Port switching input filter.
 *
 * Results:
 *    VMK_OK always.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */
VMK_ReturnStatus 
HubPortSwitchingInFilter(Port *port, IOChainData iocd, PktList *pktList)
{
   Eth_FRP *frp = (Eth_FRP *)iocd;
   PktList filteredList;

   PktList_Init(&filteredList);

   Eth_SourceFilter(&frp->inputFilter, pktList, &filteredList);

   // we can use Port_IOComplete bc we know all the pkts belong to this port
   Port_IOComplete(port, &filteredList);

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------------
 *
 * HubPortEthFRPUpdate --
 *
 *    Invoked when a port's ethernet frame routing policy is updated.
 *
 * Results:
 *    VMK_ReturnStatus.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static VMK_ReturnStatus
HubPortEthFRPUpdate(Port *port, Eth_FRP *frp)
{
   VMK_ReturnStatus status = VMK_OK;

   // first wipe out the old policy, if any
   IOChain_RemoveCall(&port->outputChain, HubPortSwitchingOutFilter);
   IOChain_RemoveCall(&port->inputChain, HubPortSwitchingInFilter);

   // add the new policy (promisc mode means no filter needed)
   if (frp->outputFilter.flags & ETH_FILTER_PROMISC) {
      LOG(0, "port 0x%x on %s: promiscuous mode enabled", 
          port->portID, port->ps->name);
   } else {
      status = IOChain_InsertCall(&port->outputChain,
                                  IO_CHAIN_RANK_FILTER,
                                  HubPortSwitchingOutFilter,
                                  NULL,
                                  NULL,
                                  &port->ethFRP,
                                  TRUE,
                                  NULL);
   }

   
   
   if (status == VMK_OK) {
      if (frp->inputFilter.flags & ETH_FILTER_PROMISC) {
         LOG(0, "port 0x%x on %s: no input filter", 
             port->portID, port->ps->name);
      } else {
         status = IOChain_InsertCall(&port->inputChain,
                                     IO_CHAIN_RANK_FILTER,
                                     HubPortSwitchingInFilter,
                                     NULL,
                                     NULL,
                                     &port->ethFRP,
                                     TRUE,
                                     NULL);
         
         LOG(0, "port 0x%x on %s: install input filter: %s", 
             port->portID, port->ps->name, VMK_ReturnStatusToString(status));
      }
   }

   return status;
}


/*
 *----------------------------------------------------------------------------
 *
 * HubPortOutput --
 *
 *    Send the packet list to the specified port's output chain. If the port
 *    isn't active, nothing is done.
 *
 *  Results:
 *
 *    Returns whatever the IOChain returns.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE VMK_ReturnStatus
HubPortOutput(Port *port, PktList *pktList)
{
   if (UNLIKELY(VMK_STRESS_DEBUG_COUNTER(NET_HUB_PORT_OUTPUT_FAIL))) {
      return VMK_FAILURE;
   }

   if (Port_IsOutputActive(port)) {
      LOG(2, "Sending packet list to port 0x%x output chain", port->portID);
      return Port_Output(port, pktList);
   }
   return VMK_FAILURE;
}


/*
 *----------------------------------------------------------------------------
 *
 * HubPortDispatch --
 *
 *    Send the packet list to every connect port in the hub. The onus of cloning
 *    the packet list lies solely with the modifier.
 *
 *  Results:
 *    VMK_OK always.
 *
 *  Side effects:
 *    The packet list is emptied by the time the call is done.
 *
 *----------------------------------------------------------------------------
 */

static VMK_ReturnStatus
HubPortDispatch(Portset *ps, struct PktList *pktList, Port *srcPort)
{
   int i;
   int srcPortIdx = Portset_GetPortIdx(srcPort);
   Hub *hub = ps->devImpl.data;
   int uplinkPortIdx = 0;
   int firstExcludedPort;
   int secondExcludedPort;
   Bool mayModify;

   ASSERT(hub);

   if (hub->connected) {
      uplinkPortIdx = Portset_PortIdxFromPortID(hub->uplinkPort, ps);
   } else {
      uplinkPortIdx = ps->numPorts;
   }

   /*
    * skip
    *    (1) the srcPort to avoid reflecting packets
    *    (2) the uplink port. This port needs to be handled the last.
    */
   firstExcludedPort = MIN(srcPortIdx, uplinkPortIdx);
   secondExcludedPort = srcPortIdx ^ uplinkPortIdx ^ firstExcludedPort;

   ASSERT(ps);
   ASSERT(srcPort);
   ASSERT(pktList);

   LOG(1, "First excluded port = 0x%x, second excluded port = 0x%x",
       firstExcludedPort, secondExcludedPort);

   /*
    * don't let any of the normal ports change the list because
    * we want everyone to see the same packets
    */
   mayModify = pktList->mayModify;
   pktList->mayModify = FALSE;

   for (i = 0; i < firstExcludedPort; i++) {
      HubPortOutput(&ps->ports[i], pktList);
   }
   for (i = firstExcludedPort + 1; i < secondExcludedPort; i++) {
      HubPortOutput(&ps->ports[i], pktList);
   }
   for (i = secondExcludedPort + 1; i < ps->numPorts; i++) {
      HubPortOutput(&ps->ports[i], pktList);
   }

   /*
    * do the uplink last and let it modify the list (if the caller 
    * allowed it) since it's usually the only one that wants to.
    */
   pktList->mayModify = mayModify;

   // now transmit to the uplink port
   if (hub->connected) {
      if (uplinkPortIdx != srcPortIdx) {
         LOG(2, "Sending packet list to the uplink port");
         HubPortOutput(&ps->ports[uplinkPortIdx], pktList);
      }
   } else {
      LOG(2, "uplink port disconnected for portset %s", ps->name);
   }
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------------
 *
 * HubUplinkNotify --
 *
 *    Handles notifications from the uplink layer.
 *
 * Results:
 *    VMK_OK on success, VMK_FAILURE on failure.
 *
 * Side effects:
 *    The hub's uplink characteristics may be modified.
 *
 *----------------------------------------------------------------------------
 */

static VMK_ReturnStatus
HubUplinkNotify(PortID uplinkPortID, UplinkData *uplinkData,
                UplinkStatus status)
{
   VMK_ReturnStatus ret = VMK_OK;
   Port *port;
   /*
    * notification is always done with both the Portset_GlobalLock and the
    * portset's exclusive lock held.
    */
   if (Portset_GetLockedPort(uplinkPortID, &port) == VMK_OK) {
      ASSERT(port);
      Hub *hub = port->ps->devImpl.data;
      ASSERT(hub);
      ASSERT(hub->uplinkPort != NET_INVALID_PORT_ID);
      ASSERT(hub->uplinkPort == uplinkPortID);
      ASSERT(hub->uplinkDevName[0] != '\0');
      switch (status) {
         case UPLINK_UP:
            ASSERT(hub->connected == FALSE);
            ASSERT(uplinkData);
            LOG(1, "Hub %s's uplink is up.", port->ps->name);
            hub->connected = TRUE;
            Portset_SetUplinkImplSz(port->ps, uplinkData->pktHdrSize);
            break;
         case UPLINK_DOWN:
            ASSERT(hub->connected == TRUE);
            LOG(1, "Hub %s's uplink is down", port->ps->name);
            hub->connected = FALSE;
            Portset_SetUplinkImplSz(port->ps, 0);
            break;
         default:
            LOG(0, "Notification message not recognized.");
            ret = VMK_FAILURE;
      }
   }
   return ret;
}


/*
 *----------------------------------------------------------------------------
 *
 * HubUplinkConnect --
 *
 *    Connects a port on the specified portset and connectes it to the specified
 *    uplink device.
 *
 * Results:
 *    VMK_OK on success, VMK_FAILURE otherwise.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static VMK_ReturnStatus
HubUplinkConnect(Portset *ps, char *uplinkDevName, PortID *portID)
{
   VMK_ReturnStatus ret;
   Port *port;
   Hub *hub = ps->devImpl.data;
   ASSERT(hub);
   ASSERT(uplinkDevName);
   ASSERT(portID);
   *portID = 0;
   if (hub->connected) {
      ASSERT(hub->uplinkPort != NET_INVALID_PORT_ID);
      ASSERT(hub->uplinkDevName[0] != '\0');
      Log("Uplink port is already connected to a device.");
      return VMK_FAILURE;
   }

   if (UNLIKELY(VMK_STRESS_DEBUG_COUNTER(NET_HUB_UPLINK_CONNECT_FAIL))) {
      return VMK_FAILURE;
   }

   ret = Portset_ConnectPort(ps, &port);
   if (ret == VMK_OK) {
      UplinkData *dummy;
      ret = Uplink_Register(port->portID, uplinkDevName,
                            DEVICE_TYPE_PORTSET_TOPLEVEL,
                            HubUplinkNotify,
                            &dummy);
      if (ret == VMK_OK) {
         hub->connected = TRUE;
         hub->uplinkPort = port->portID;
         Portset_SetUplinkImplSz(port->ps, dummy->pktHdrSize);
         memcpy(hub->uplinkDevName, uplinkDevName, sizeof(hub->uplinkDevName));
         Port_Enable(port);
         *portID = port->portID;
         LOG(1, "Hub %s connected to uplink port", ps->name);
      } else if (ret == VMK_NOT_FOUND) {
         hub->connected = FALSE;
         hub->uplinkPort = port->portID;
         memcpy(hub->uplinkDevName, uplinkDevName, sizeof(hub->uplinkDevName));
         *portID = port->portID;
         LOG(1, "Hub %s's uplink yet to come up", ps->name);
         ret = VMK_OK;
      } else {
         Portset_DisconnectPort(ps, port->portID);
         LOG(0, "Hub %s failed to claim uplink device", ps->name);
         ret = VMK_FAILURE;
      }
   }
   return ret;
}


/*
 *----------------------------------------------------------------------------
 *
 * HubUplinkDisconnect --
 *
 *    Disconnect the uplink port from the specified device. Calls into the uplink
 *    layer to request the disconnect.
 *
 * Results:
 *    VMK_OK always.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static VMK_ReturnStatus
HubUplinkDisconnect(Portset *ps, char *uplinkName)
{
   Hub *hub = ps->devImpl.data;

   ASSERT(hub);

   if (strcmp(hub->uplinkDevName, uplinkName)) {
      Warning("cannot disconnect %s on %s", uplinkName, ps->name);
      return VMK_FAILURE;
   }

   if (hub->uplinkPort != NET_INVALID_PORT_ID) {
      ASSERT(hub->uplinkDevName[0] != '\0');
      Uplink_Unregister(hub->uplinkPort, hub->uplinkDevName);
      Portset_DisconnectPort(ps, hub->uplinkPort);
      hub->uplinkPort = NET_INVALID_PORT_ID;
      hub->uplinkDevName[0] = '\0';
      hub->connected = FALSE;
      LOG(1, "Hub %s's uplink detached", ps->name);
   } else {
      LOG(0, "Did not find an active uplink for hub %s", ps->name);
   }
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------------
 *
 * HubDeactivate --
 *
 *    Deactivation handler for the hub. Frees up the hub associated with the
 *    portset. If the uplink port happens to be connect, it disconnects it.
 *
 * Results:
 *    VMK_OK always.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static VMK_ReturnStatus
HubDeactivate(Portset *ps)
{
   Hub *hub = ps->devImpl.data;
   if (hub) {
      if (hub->uplinkPort != NET_INVALID_PORT_ID) {
         ASSERT(hub->uplinkDevName[0] != '\0');
         LOG(1, "Closing uplink port for hub %s", ps->name);
         Uplink_Unregister(hub->uplinkPort, hub->uplinkDevName);
         Portset_DisconnectPort(ps, hub->uplinkPort);
      }
      Mem_Free(hub);
   }
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------------
 *
 * Hub_Activate --
 *
 *    External entry point. Sets up the portset's dispatch table.
 *
 * Results:
 *    VMK_OK on success. VMK_NO_RESOURCES if memory couldn't be allocated for
 *    the hub.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

VMK_ReturnStatus
Hub_Activate(Portset *ps)
{
   VMK_ReturnStatus ret = VMK_OK;
   Hub *hub = Mem_Alloc(sizeof(Hub));

   if (UNLIKELY(VMK_STRESS_DEBUG_COUNTER(NET_HUB_ACTIVATE_FAIL))) {
      if (hub) {
         Mem_Free(hub);
         hub = NULL;
      }
   }

   if (hub) {
      ASSERT(ps);
      ps->devImpl.dispatch = HubPortDispatch;
      ps->devImpl.deactivate = HubDeactivate;
      ps->devImpl.portConnect = HubPortConnect;
      ps->devImpl.portDisconnect = HubPortDisconnect;
      ps->devImpl.portEthFRPUpdate = HubPortEthFRPUpdate;
      ps->devImpl.uplinkConnect = HubUplinkConnect;
      ps->devImpl.uplinkDisconnect = HubUplinkDisconnect;
      hub->uplinkPort = NET_INVALID_PORT_ID;
      hub->uplinkDevName[0] = '\0';
      hub->connected = FALSE;
      ps->devImpl.data = hub;
      LOG(2, "Hub %s activated", ps->name);
   } else {
      LOG(0, "Hub %s couldnt be created", ps->name);
      ret = VMK_NO_RESOURCES;
   }
   return ret;
}
