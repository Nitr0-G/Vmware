/* **********************************************************
 * Copyright (C) 2004 VMware, Inc.
 * All Rights Reserved
 * **********************************************************/


/*
 * vmklinux_exports.c --
 *
 *    Interface between the vmkernel and vmklinux.
 */

#include "net_int.h"
#define LOGLEVEL_MODULE_LEN 0
#define LOGLEVEL_MODULE Net
#include "log.h"

extern Proc_Entry portsetProcDir;

/*
 *----------------------------------------------------------------------------
 *
 *  Net_PktFree --
 *
 *    Call necessary IO completion handlers and free the PktHandle.
 *
 *  Results:
 *    None.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

void
Net_PktFree(PktHandle *pkt)
{
   Net_IOComplete(pkt);
}


/*
 *----------------------------------------------------------------------------
 *
 *  Net_PktAlloc --
 *
 *    Wrapper for allocating PktHandles.
 *
 *  Results:
 *    Pointer to the allocated memory.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

PktHandle *
Net_PktAlloc(size_t headroom, size_t size)
{
   return Pkt_Alloc(headroom, size);
}


/*
 *----------------------------------------------------------------------------
 *
 *  Net_UplinkDeviceConnected --
 *
 *    Handler for device connect notifications.
 *
 *  Results:
 *    None.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

VMK_ReturnStatus
Net_UplinkDeviceConnected(const char *devName, void *device, int32 moduleID,
                          Net_Functions *functions, size_t pktHdrSize,
                          size_t maxSGLength, void **uplinkDev)
{
   return(Uplink_DeviceConnected(devName, device, moduleID, functions,
                                 pktHdrSize, maxSGLength, uplinkDev));
}


/*
 *----------------------------------------------------------------------------
 *
 *  Net_UplinkSetupIRQ --
 *
 *    Setup IRQ paramaters for the device.
 *
 *  Results:
 *    None.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

void
Net_UplinkSetupIRQ(void *d, uint32 vector, IDT_Handler h, void *handlerData)
{
   Uplink_SetupIRQ(d, vector, h, handlerData);
}


/*
 *----------------------------------------------------------------------------
 *
 *  Net_UplinkRegisterCallbacks --
 *
 *    Register PCI callback notifications for the specified device.
 *
 *  Results:
 *    None.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

void
Net_UplinkRegisterCallbacks(UplinkDevice *dev)
{
   Uplink_RegisterCallbacks(dev);
}



/*
 *----------------------------------------------------------------------------
 *
 * Net_GetUplinkImpl --
 *
 *    Get the implementation field associated with the device having the given
 *    name.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

void *
Net_GetUplinkImpl(const char *name)
{
   return Uplink_GetImpl(name);
}


/*
 *----------------------------------------------------------------------------
 *
 * Net_ReceivePkt --
 *
 *    Receive a packet from an uplink port. The packet is forwarded to the
 *    portset for further processing. For now, only one packet is received at
 *    at time.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

void
Net_ReceivePkt(void *uplinkDev, PktHandle *pkt)
{
   UplinkDevice *uplink = uplinkDev;
   Port *port;

   ASSERT(uplinkDev);

   if (uplink->uplinkPort != NET_INVALID_PORT_ID) {
      Portset_GetPort(uplink->uplinkPort, &port);
      if (port != NULL) {
         LOG(3, "%s uplink = %p uplinkPort = 0x%x port->ps->name = %s port->ps = %p",
             uplink->devName, uplink, (int)uplink->uplinkPort, port->ps->name, port->ps);
         Port_InputOne(port, pkt);
         Portset_ReleasePort(port);
      } else {
         Log("Port is NULL\n");
         Pkt_Release(pkt);
      }
   } else {
      LOG(1, "uplinkPort is not defined for %s, pkt = %p", uplink->devName, pkt);
      Pkt_Release(pkt);
   }
}


/*
 *----------------------------------------------------------------------------
 *
 * Net_IOComplete --
 *
 *    Handle an IOComplete packet.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */
 
VMK_ReturnStatus
Net_IOComplete(PktHandle *pkt)
{
   Port *port;
   PktList tmpList;
   Net_PortID srcPortID = pkt->pktDesc->srcPortID;

   ASSERT(pkt);
   ASSERT(pkt->pktDesc);

   if (IS_SET(pkt->pktDesc->flags, PKTDESC_FLAG_NOTIFY_COMPLETE)) {
      ASSERT(srcPortID != NET_INVALID_PORT_ID);
      PktList_Init(&tmpList);
      PktList_AddToTail(&tmpList, pkt);
      Portset_GetPort(srcPortID, &port);
      if (port != NULL) {
         VMK_ReturnStatus ret = Port_IOComplete(port, &tmpList);
         Portset_ReleasePort(port);
         return ret;
      }

      // just toss it since the sending port is gone
      LOG(0, "sending port 0x%x not available for completion of pkt %p",
          srcPortID, pkt);
      pkt = Pkt_ReleaseOrComplete(pkt);
      if (pkt != NULL) {
         PktClearIOCompleteData(pkt);
         Pkt_Release(pkt);
      }
      return VMK_NOT_FOUND;
   }

   Pkt_Release(pkt);
   return VMK_OK;
}


