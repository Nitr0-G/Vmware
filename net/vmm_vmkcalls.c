/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. 
 * -- VMware Confidential
 * **********************************************************/

/*
 * vmm_vmkcalls.c  --
 *
 *   vmk_call interface to vmkernel networking for the monitor.
 */

#include "vmkernel.h"
#define LOGLEVEL_MODULE_LEN 0
#define LOGLEVEL_MODULE Net
#include "log.h"

#include "net_int.h"
#include "vmxnet_def.h"
#include "vmxnet1_def.h"
#include "vmxnet2_def.h"
#include "alloc_inline.h"
#include "kvmap.h"

/*
 *----------------------------------------------------------------------
 *
 * Net_VMMDisconnect --
 *
 *      Disconnect a virtual device from a virtual network.
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
Net_VMMDisconnect(DECLARE_ARGS(VMK_NET_VMM_DISCONNECT))
{
   PROCESS_1_ARG(VMK_NET_VMM_DISCONNECT, Net_PortID, portID);
   World_ID worldID = World_GetVmmLeaderID(MY_RUNNING_WORLD);

   LOG(0, "port 0x%x from world %u", portID, worldID);

   return Net_Disconnect(worldID, portID);
}


/*
 *----------------------------------------------------------------------------
 *
 * Net_VMMPortEnableVlance --
 * 
 *    Enable the specified port for a vlance virtual device connnection.
 *
 * Results:
 *    VMK_ReturnStatus indicating the outcome.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

VMK_ReturnStatus
Net_VMMPortEnableVlance(DECLARE_ARGS(VMK_NET_VMM_PORT_ENABLE_VLANCE))
{
   PROCESS_2_ARGS(VMK_NET_VMM_PORT_ENABLE_VLANCE, 
                  Net_PortID, portID, 
                  uint32, vmkChannelPending);

   World_Handle *world = World_GetVmmLeader(MY_RUNNING_WORLD);
   VMK_ReturnStatus status = VMK_OK;
   Port *port = Portset_GetPortExcl(portID);

   if (port == NULL) {
      status = VMK_BAD_PARAM;
      goto done;
   }

   // only one of the "owner" worlds should access the handle
   status = Port_CheckWorldAssociation(port, world->worldID);
   if (status != VMK_OK) {
      goto done;
   }
   
   status = VlanceVMKDev_Enable(port, vmkChannelPending);
   if (status != VMK_OK) {
      goto done;
   }

   status = Port_Enable(port);

  done:

   // clean up from failure if necessary
   if (status != VMK_OK) {
      VmWarn(world->worldID, "cannot enable port 0x%x: %s", 
             portID, VMK_ReturnStatusToString(status));
      if (port == NULL) {
         port = Portset_GetPortExcl(portID);
      }
      if (port != NULL) {
         Port_ForceDisable(port);
      }
   }

   if (port != NULL) {
      Portset_ReleasePortExcl(port);
   }

   return status;
}


/*
 *----------------------------------------------------------------------------
 *
 * Net_VMMPortEnableVmxnet --
 * 
 *    Enable the specified port for a vmxnet style virtual device connnection.
 *
 * Results:
 *    VMK_ReturnStatus indicating the outcome.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

VMK_ReturnStatus
Net_VMMPortEnableVmxnet(DECLARE_ARGS(VMK_NET_VMM_PORT_ENABLE_VMXNET))
{
   PROCESS_4_ARGS(VMK_NET_VMM_PORT_ENABLE_VMXNET, 
                  Net_PortID, portID, 
                  PA32, ddPA, uint32, ddLen, uint32, intrActionIdx);
   PA pa;
   uint32 paLen;
   uint32 numPages = 0;
   KVMap_MPNRange ranges[VMXNET_MAX_SHARED_PAGES];
   uint32 numRanges = 0;
   VA ddMapped = (VA)0;
   uint32 ddOffset;
   Vmxnet_DDMagic *ddMagic;
   World_Handle *world = World_GetVmmLeader(MY_RUNNING_WORLD);
   VMK_ReturnStatus status = VMK_OK;
   Port *port = NULL; 
    
   /*
    * map in the driver data and figure out what kind of vmxnet driver
    * we are dealing with here.
    */

   pa = ddPA;
   paLen = ddLen;
   while (1) {
      Alloc_Result r;
      status = Alloc_PhysToMachine(world, pa, paLen, 0, TRUE, &r);
      if (status != VMK_OK) {
         goto done;
      }
      if (r.length > paLen) {
         r.length = paLen;
      }
      ranges[numRanges].startMPN = MA_2_MPN(r.maddr);
      ranges[numRanges].numMPNs = 
         MA_2_MPN(r.maddr + r.length - 1) - ranges[numRanges].startMPN + 1;
      numPages += ranges[numRanges].numMPNs;
      numRanges++;
      pa += r.length;
      paLen -= r.length;
      if (paLen == 0) {
         break;
      }
      if (numRanges == VMXNET_MAX_SHARED_PAGES) {
         Warning("Driver data too big: %u", numRanges);
         status = VMK_LIMIT_EXCEEDED;
         goto done;
      }   
   }
    
   ddMapped = (VA)KVMap_MapMPNs(numPages, ranges, numRanges, 0);
   if (ddMapped == (VA)0) {
      Warning("Failed to map vmxnet shared driver data");
      status = VMK_NO_RESOURCES;
      goto done;
   }
   ddOffset = ((uint32)ddPA & PAGE_MASK);
   ddMagic = (Vmxnet_DDMagic *) (ddMapped + ddOffset); 

   port = Portset_GetPortExcl(portID);

   if (port == NULL) {
      status = VMK_BAD_PARAM;
      goto done;
   }

   // only one of the "owner" worlds should access the handle
   status = Port_CheckWorldAssociation(port, world->worldID);
   if (status != VMK_OK) {
      goto done;
   }

   switch (*ddMagic) {
   case VMXNET1_MAGIC:
      VmWarn(world->worldID, "ESX 1.x vmxnet guest drivers no longer supported");
      status = VMK_NOT_SUPPORTED;
      break;

   case VMXNET2_MAGIC:
      status = Vmxnet2VMKDev_Enable(port, ddMapped, ddLen, ddOffset, intrActionIdx);
      break;

   default:
      VmWarn(world->worldID, "bad dd magic: 0x%x", *ddMagic);
      status = VMK_BAD_PARAM;
   }

   if (status != VMK_OK) {
      goto done;
   }

   status = Port_Enable(port);

  done:

   // clean up from failure if necessary
   if (status != VMK_OK) {
      if (ddMapped != (VA)0) {      
         /*
          * The various implementations would normally do this 
          * in their .disable call, but it's hard to know here 
          * if they registered one or even had a chance to, so
          * we special case this.
          */
         KVMap_FreePages((char *)ddMapped);
      }

      VmWarn(world->worldID, "cannot enable port 0x%x: %s", 
             portID, VMK_ReturnStatusToString(status));
      if (port == NULL) {
         port = Portset_GetPortExcl(portID);
      }
      if (port != NULL) {
         Port_ForceDisable(port);
      }
   }

   if (port != NULL) {
      Portset_ReleasePortExcl(port);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Net_VMMVmxnetUpdateEthFRP --
 *
 *      vmxnet monitor emul calls this to get the switch to update
 *      its ethernet frame routing policy for the port.  The adapter's
 *      current MAC addr is passed in, and the LADRF and IFF are 
 *      pulled from the driverData struct.
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
Net_VMMVmxnetUpdateEthFRP(DECLARE_2_ARGS(VMK_NET_VMM_VMXNET_UPDATE_ETH_FRP, 
                                         Net_PortID, portID, 
                                         uint8 *, addr))
{
   PROCESS_2_ARGS(VMK_NET_VMM_VMXNET_UPDATE_ETH_FRP, 
                  Net_PortID, portID, uint8 *, addr);
   Eth_Address *macAddr = (Eth_Address *)addr;
   VMK_ReturnStatus status = VMK_OK;
   World_ID worldID = World_GetVmmLeaderID(MY_RUNNING_WORLD);
   Port *port = Portset_GetPortExcl(portID);

   LOG(0, "port 0x%x from world %u", portID, worldID);

   if (port) {
      status = Port_CheckWorldAssociation(port, worldID);
      if (status == VMK_OK) {
         // XXX switch based on vmxnet proto here once we have vmxnet3
         status = Vmxnet2VMKDev_UpdateEthFRP(port, macAddr);
      }
      Portset_ReleasePortExcl(port);
   } else {
      VmWarn(worldID, "port 0x%x not found", portID);
      status = VMK_BAD_PARAM;
   }

   return status;
}


/*
 *----------------------------------------------------------------------------
 *
 * Net_VMMPortDisable --
 * 
 *    Disable the specified port.
 *
 * Results:
 *    VMK_ReturnStatus indicating the outcome.
 *
 * Side effects:
 *    May transmit packets.
 *    May block for a long time if a physical device is wedged.
 *    May force packet completion even if device is unresponsive.
 *
 *----------------------------------------------------------------------------
 */

VMK_ReturnStatus
Net_VMMPortDisable(DECLARE_ARGS(VMK_NET_VMM_PORT_DISABLE))
{
   PROCESS_1_ARG(VMK_NET_VMM_PORT_DISABLE, Net_PortID, portID);
   VMK_ReturnStatus status = VMK_OK;
   World_ID worldID = World_GetVmmLeaderID(MY_RUNNING_WORLD);
   Port *port = Portset_GetPortExcl(portID);

   LOG(0, "port 0x%x from world %u", portID, worldID);

   if (port) {
      status = Port_CheckWorldAssociation(port, worldID);
      if (status == VMK_OK) {
         status = Port_TryDisable(port);
         if (status == VMK_BUSY) {
            LOG(0, "port 0x%x busy, blocking world %u", portID, worldID);
            port = Port_BlockUntilDisabled(port);
            if (port == NULL) {
               ASSERT(FALSE);
               VmWarn(worldID, "port 0x%x disappeared waiting for disable", portID);
               return status;
            }
            if (status != VMK_OK) {
               VmWarn(worldID, "cannot disable port 0x%x on %s: %s",
                      portID, port->ps->name, VMK_ReturnStatusToString(status));
               status = Port_ForceDisable(port);
               if (status != VMK_OK) {
                  VmWarn(worldID, "cannot force disable port 0x%x on %s: %s",
                         portID, port->ps->name, VMK_ReturnStatusToString(status));
               }
            }
         }
      } else {
         VmWarn(worldID, "0x%x: port doesn't belong to world", portID);
      }
      Portset_ReleasePortExcl(port);
   } else {
      VmWarn(worldID, "port 0x%x not found", portID);
      status = VMK_NOT_FOUND;
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Net_VMMGetPortCapabilities --
 *
 *      Return the capabilities supported by the port.
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
Net_VMMGetPortCapabilities(DECLARE_2_ARGS(VMK_NET_VMM_GET_PORT_CAPABILITIES, 
                                          Net_PortID, portID, 
                                          uint32 *, capabilities))
{
   PROCESS_2_ARGS(VMK_NET_VMM_GET_PORT_CAPABILITIES, 
                  Net_PortID, portID, 
                  uint32 *, capabilities);

   LOG(0, "0x%x", portID);
   if (portID == NET_INVALID_PORT_ID) {
      VmWarn(MY_RUNNING_WORLD->worldID, "bad port 0x%x", portID);
   }

   *capabilities = 0;
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Net_VMMVlancetTx --
 *
 *      Called to transmit a packet from a vlance device.
 *
 * Results: 
 *	VMK_ReturnStatus.
 *
 * Side effects:
 *	May block.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus 
Net_VMMVlanceTx(DECLARE_ARGS(VMK_NET_VMM_VLANCE_TX))
{
   PROCESS_2_ARGS(VMK_NET_VMM_VLANCE_TX, 
                  Net_PortID, portID, 
                  NetSG_Array *, sg);

   Port *port;
   VMK_ReturnStatus status;
   World_ID worldID = World_GetVmmLeaderID(MY_RUNNING_WORLD);

   LOG(20, "0x%x: %p", portID, sg);

   status = Portset_GetPort(portID, &port);
   if (UNLIKELY(status != VMK_OK)) {
      return status;
   }
      
   status = Port_CheckWorldAssociation(port, worldID);
   if (status == VMK_OK) {
      if (Port_IsOutputActive(port)) {
         status = VlanceVMKDev_Tx(port, sg);
      } else {
         LOG(0, "port 0x%x not enabled for output", portID);
         status = VMK_ENETDOWN;
      }     
   }

   Portset_ReleasePort(port);
   
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Net_VMMVlancetRxDMA --
 *
 *      Called to receive a packet on a vlance device.
 *
 * Results: 
 *	VMK_ReturnStatus.
 *
 * Side effects:
 *	May block.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus 
Net_VMMVlanceRxDMA(DECLARE_ARGS(VMK_NET_VMM_VLANCE_RXDMA))
{
   PROCESS_3_ARGS(VMK_NET_VMM_VLANCE_RXDMA, 
                  Net_PortID,  portID, 
                  NetSG_Array *, sg,
                  uint32 *, byteCount);

   Port *port;
   VMK_ReturnStatus status;
   World_ID worldID = World_GetVmmLeaderID(MY_RUNNING_WORLD);

   LOG(20, "0x%x: %p", portID, sg);

   status = Portset_GetPort(portID, &port);
   if (UNLIKELY(status != VMK_OK)) {
      return status;
   }
    
   status = Port_CheckWorldAssociation(port, worldID);
   if (status == VMK_OK) {
      if (Port_IsInputActive(port)) {
         status = VlanceVMKDev_RxDMA(port, sg, byteCount);
      } else {
         LOG(0, "port 0x%x not enabled for input", portID);
         status = VMK_ENETDOWN;
      }     
   }

   Portset_ReleasePort(port);
   
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Net_VMMVmxnetTx --
 *
 *      Called to flush the transmit ring of a vmxnet device.
 *
 * Results: 
 *	VMK_ReturnStatus.
 *
 * Side effects:
 *	Attempts to flush the device's tx ring.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus 
Net_VMMVmxnetTx(DECLARE_ARGS(VMK_NET_VMM_VMXNET_TX))
{
   PROCESS_1_ARG(VMK_NET_VMM_VMXNET_TX, Net_PortID, portID);
   // XXX switch based on vmxnet proto here once we have vmxnet3
   return Vmxnet2VMKDev_Tx(portID);
}


/*
 *----------------------------------------------------------------------
 *
 * Net_VMMPinVmxnetTxBuffers --
 *
 *      Pin down the tx buffers from the guest into the vmkernel
 *      so that we don't have to translate/pin them over and over.
 *
 * Results: 
 *	VMK_ReturnStatus.
 *
 * Side effects:
 *	Pages are pinned, MAs and VAs are stored by the client impl.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
Net_VMMPinVmxnetTxBuffers(DECLARE_1_ARG(VMK_NET_PIN_VMXNET_TX_BUFFERS,
                                        Net_PortID, portID))
{
   PROCESS_1_ARG(VMK_NET_PIN_VMXNET_TX_BUFFERS, Net_PortID, portID);
   // XXX switch based on vmxnet proto here once we have vmxnet3
   return Vmxnet2VMKDev_PinTxBuffers(portID);
}


/*
 *----------------------------------------------------------------------
 *
 * Net_VMMVlanceUpdateIFF --
 *
 *      empty stub, please implement me
 *
 * Results: 
 *	VMK_FAILURE.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
Net_VMMVlanceUpdateIFF(DECLARE_2_ARGS(VMK_NET_VMM_VLANCE_UPDATE_IFF, 
                                      Net_PortID, portID,
                                      uint32, ifflags))
{
   PROCESS_2_ARGS(VMK_NET_VMM_VLANCE_UPDATE_IFF, 
                  Net_PortID, portID,
                  uint32, ifflags);
   World_ID worldID = World_GetVmmLeaderID(MY_RUNNING_WORLD);
   Port *port = Portset_GetPortExcl(portID);
   VMK_ReturnStatus status;
   Eth_FRP frp;

   if (port == NULL) {
      VmWarn(worldID, "port 0x%x not found", portID);
      status = VMK_NOT_FOUND;
      goto done;
   }

   // only one of the "owner" worlds should access the handle
   status = Port_CheckWorldAssociation(port, worldID);
   if (status != VMK_OK) {
      goto done;
   }

   memcpy(&frp, &port->ethFRP, sizeof (frp));

   frp.outputFilter.flags &= ~(ETH_FILTER_UNICAST    |
                               ETH_FILTER_MULTICAST  |
                               ETH_FILTER_BROADCAST  |
                               ETH_FILTER_PROMISC);
   if (ifflags & IFF_UP) {
      frp.outputFilter.flags |= ETH_FILTER_UNICAST;
   }
   if (ifflags & IFF_MULTICAST) {
      frp.outputFilter.flags |= ETH_FILTER_MULTICAST;
   }
   if (ifflags & IFF_BROADCAST) {
      frp.outputFilter.flags |= ETH_FILTER_BROADCAST;
   }
   if (ifflags & IFF_PROMISC) {
      frp.outputFilter.flags |= ETH_FILTER_PROMISC;
   }

   status = Port_UpdateEthFRP(port, &frp);

  done:

   if (port != NULL) {
      Portset_ReleasePortExcl(port);
   }
   
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * Net_VMMVlanceUpdateLADRF --
 *
 *      Update the logical address filter for multicast frames.
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
Net_VMMVlanceUpdateLADRF(DECLARE_2_ARGS(VMK_NET_VMM_VLANCE_UPDATE_LADRF, 
                                        Net_PortID, portID,
                                        uint32 *, ladrf))
{
   PROCESS_2_ARGS(VMK_NET_VMM_VLANCE_UPDATE_LADRF, 
                  Net_PortID, portID,
                  uint32 *, ladrf);
   World_ID worldID = World_GetVmmLeaderID(MY_RUNNING_WORLD);
   Port *port = Portset_GetPortExcl(portID);
   VMK_ReturnStatus status;
   Eth_FRP frp;

   if (port == NULL) {
      VmWarn(worldID, "port 0x%x not found", portID);
      status = VMK_NOT_FOUND;
      goto done;
   }

   // only one of the "owner" worlds should access the handle
   status = Port_CheckWorldAssociation(port, worldID);
   if (status != VMK_OK) {
      goto done;
   }

   memcpy(&frp, &port->ethFRP, sizeof (frp));
   memcpy(&frp.outputFilter.ladrf, ladrf, sizeof(frp.inputFilter.ladrf));
   frp.outputFilter.flags |= ETH_FILTER_USE_LADRF;

   status = Port_UpdateEthFRP(port, &frp);

  done:

   if (port != NULL) {
      Portset_ReleasePortExcl(port);
   }
   
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Net_VMMVlanceUpdateMAC --
 *
 *      Update the MAC address.
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
Net_VMMVlanceUpdateMAC(DECLARE_2_ARGS(VMK_NET_VMM_VLANCE_UPDATE_MAC, 
                                      Net_PortID, portID,
                                      uint8 *, addr))
{
   PROCESS_2_ARGS(VMK_NET_VMM_VLANCE_UPDATE_MAC, 
                  Net_PortID, portID,
                  uint8 *, addr);
   World_ID worldID = World_GetVmmLeaderID(MY_RUNNING_WORLD);
   Port *port = Portset_GetPortExcl(portID);
   VMK_ReturnStatus status;
   Eth_FRP frp;

   if (port == NULL) {
      VmWarn(worldID, "port 0x%x not found", portID);
      status = VMK_NOT_FOUND;
      goto done;
   }

   // only one of the "owner" worlds should access the handle
   status = Port_CheckWorldAssociation(port, worldID);
   if (status != VMK_OK) {
      goto done;
   }

   memcpy(&frp, &port->ethFRP, sizeof (frp));
   memcpy(&frp.outputFilter.unicastAddr, addr, sizeof(frp.inputFilter.unicastAddr));
   frp.outputFilter.flags |= ETH_FILTER_USE_LADRF;

   status = Port_UpdateEthFRP(port, &frp);

  done:

   if (port != NULL) {
      Portset_ReleasePortExcl(port);
   }
   
   return status;
}
