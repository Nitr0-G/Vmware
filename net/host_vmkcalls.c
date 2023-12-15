/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. 
 * -- VMware Confidential
 * **********************************************************/

/*
 * host_vmkcalls.c  --
 *
 *   Interface to vmkernel networking for the host (aka COS, aka vmnix)
 *
 */

#include "vmkernel.h"
#include "host.h"
#include "kvmap.h"
#include "net_int.h"
#include "cos_vmkdev_public.h"

#define LOGLEVEL_MODULE Net
#define LOGLEVEL_MODULE_LEN 0
#include "log.h"


/*
 *----------------------------------------------------------------------
 *
 * Net_HostConnect --
 *
 *    Connect a virtual adapter in the host to a vmkernel virtual 
 *    network.  Also used to connect VMs' virtual adapters in the 
 *    legacy non-userworld case.
 *
 * Results: 
 *    VMK_ReturnStatus.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
Net_HostConnect(VMnix_NetConnectArgs *hostConnectArgs,
                Net_PortID *hostPortID)
{
   VMK_ReturnStatus status;
   Net_PortID portID;
   VMnix_NetConnectArgs connectArgs;

   CopyFromHost(&connectArgs, hostConnectArgs, sizeof(connectArgs));

   status = Net_Connect(connectArgs.worldID, connectArgs.name, &portID);

   if (status == VMK_OK) {
      CopyToHost(hostPortID, &portID, sizeof(portID));
   }
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Net_HostDisconnect --
 *
 *    Disconnect a virtual adapter in the host from a vmkernel virtual 
 *    network.  Also used to disconnect VMs' virtual adapters in the 
 *    legacy non-userworld case.
 *       
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
Net_HostDisconnect(World_ID worldID, Net_PortID portID)
{
   Net_Disconnect(worldID, portID);
}


/*
 *-----------------------------------------------------------------------------
 *
 * Net_HostPortEnable --
 *
 *    Handle a port enable request for the host. 
 *    Maps in the shared area and sets up the port corresponding to the
 *    interface to which the shared area belongs.
 *    Called from a helper request.
 *     
 * Results:
 *    VMK_ReturnStatus. 
 *      
 * Side effects:
 *    If call was successful, result is returned to the helper module.
 *    Memory allocated by the helper call is freed.
 *
 *-----------------------------------------------------------------------------
 */

VMK_ReturnStatus
Net_HostPortEnable(void *data, void **resultp)
{
   VMnix_NetPortEnableArgs *hostArgs = (VMnix_NetPortEnableArgs *)data;
   VMK_ReturnStatus status = VMK_OK;
   KVMap_MPNRange ranges[COSVMKDEV_MAX_STATE_RANGES];
   unsigned int sharedStateLen = hostArgs->length;
   MA sharedStateMA = (MA)hostArgs->paddr; // COS linearly mapped low
   VA sharedStateVP = 0;
   VA sharedStateVA = 0;
   Port *port = NULL;

   ranges[0].startMPN = MA_2_MPN(sharedStateMA);
   ranges[0].numMPNs =
      MA_2_MPN(sharedStateMA + sharedStateLen -1) - ranges[0].startMPN + 1;

   sharedStateVP = (VPN)KVMap_MapMPNs(ranges[0].numMPNs, ranges, 1, 0);
   LOG(0, "shared state: baseMA = 0x%llx, baseVA = 0x%x, len = 0x%x", sharedStateMA,
       sharedStateVP, sharedStateLen);
   if (sharedStateVP == (VA)0) {
      Warning("Failed to map COS shared driver data");
      status = VMK_NO_RESOURCES;
      goto done;
   }

   sharedStateVA = sharedStateVP + ((VA)sharedStateMA & PAGE_MASK); 

   port = Portset_GetPortExcl(hostArgs->portID);

   if (port == NULL) {
      LOG(0, "Failed to find port for portID 0x%x", hostArgs->portID);
      status = VMK_BAD_PARAM;
      goto done;
   }

   status = COSVMKDev_Enable(port, sharedStateVA, sharedStateLen, sharedStateVP);
   if (status != VMK_OK) {
      LOG(0, "Failed to setup port 0x%x", port->portID);
   }      

   status = Port_Enable(port);

  done:

   if (status != VMK_OK) {
      if (port) {
         port->impl.data = NULL;
         Port_ForceDisable(port);
      }

      if (sharedStateVP != (VA)0) {
         KVMap_FreePages((void *)sharedStateVP);
      }
   }

   Mem_Free(hostArgs);

   if (port) {
      Portset_ReleasePortExcl(port);
   }

   return status;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Net_HostPortDisable --
 *
 *    Handle a port disable request for the host. 
 *    Called from a helper request.
 *
 * Results:
 *    VMK_ReturnStatus.
 *      
 * Side effects:
 *    If call was successful, result is returned to the helper module.
 *    Memory allocated by the helper call is freed.
 *
 *-----------------------------------------------------------------------------
 */

VMK_ReturnStatus
Net_HostPortDisable(void *data, void **resultp)
{
   VMnix_NetPortDisableArgs *hostArgs = (VMnix_NetPortDisableArgs *)data;
   VMK_ReturnStatus status;

   // be nice at first...
   status = Net_PortDisable(hostArgs->portID, FALSE);
   if (status != VMK_OK) {
      // ...but force it if necessary
      status = Net_PortDisable(hostArgs->portID, TRUE);
   }

   Mem_Free(hostArgs);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Net_HostUpdateEthFRP --
 *
 *    Update the ethernet frame routing policy for a virtual adapter 
 *    in the host.  The interface flags, LADRF, and MAC address(es)
 *    are read from the shared memory in order to calculate the new
 *    policy.
 *
 * Results: 
 *    VMK_FAILURE.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
Net_HostUpdateEthFRP(Net_PortID portID)
{
   VMK_ReturnStatus status = VMK_OK;
   Port *port = NULL;

   port = Portset_GetPortExcl(portID);

   if (port == NULL) {
      LOG(0, "Failed to find port for portID 0x%x", portID);
      status = VMK_BAD_PARAM;
      goto done;
   }

   status = COSVMKDev_UpdateEthFRP(port);

  done:

   if (port != NULL) {
      Portset_ReleasePortExcl(port);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Net_HostTx --
 *
 *    Poll the tx ring of a virtual adpater in the host and transmit
 *    any frmaes found.
 *
 * Results: 
 *    VMK_ReturnStatus.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
Net_HostTx(Net_PortID portID)
{
   VMK_ReturnStatus status;
   Port *port = NULL;

   status = Portset_GetPort(portID, &port);
   if (UNLIKELY(status != VMK_OK)) {
      return VMK_BAD_PARAM;
   }

   status = COSVMKDev_Tx(port);
 
   Portset_ReleasePort(port);
      
   return status;
}

