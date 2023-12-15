/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. 
 * -- VMware Confidential
 * **********************************************************/

/*
 * port.c  --
 *
 *   Ports are the vmkernel side of virtual network access
 *   points.  Virtual devices plug in to ports to become
 *   part of a virtual network.  Physical device drivers
 *   plug in to ports to connect physical and virtual networks.
 */

#include "vmkernel.h"
#define LOGLEVEL_MODULE_LEN 0
#define LOGLEVEL_MODULE Net
#include "log.h"

#include "net_int.h"

typedef struct PortFlag {
   uint32  val;
   char   *string;
} PortFlag;

#define PORT_FLAG(f,v) {v,#f},
PortFlag portFlags[] = { PORT_FLAGS {PORT_VALID_FLAGS, "PORT_VALID_FLAGS"} } ;
#undef PORT_FLAG   



/*
 *----------------------------------------------------------------------
 *
 * PortReset --
 *
 *      Reset the given port, making it ready to be (re)connected.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	Port is stripped of any former attachment to a client.
 *
 *----------------------------------------------------------------------
 */
static void
PortReset(Port *port)
{
   ASSERT(Portset_LockedExclHint(port->ps));

   port->portID = NET_INVALID_PORT_ID;
   port->flags &= ~PORT_VALID_FLAGS;
   ASSERT(port->flags == 0);
   port->worldAssc = INVALID_WORLD_ID;
   memset(&port->worldArr, 0, sizeof (port->worldArr));
   memset(&port->clientStats, 0, sizeof (port->clientStats));
   memset(&port->ethFRP, 0, sizeof (port->ethFRP));

   IOChain_Init(&port->outputChain, port->portID);
   IOChain_Init(&port->inputChain, port->portID);
   IOChain_Init(&port->notifyChain, port->portID);
}


/*
 *----------------------------------------------------------------------
 *
 * Port_Init --
 *
 *      Initialize the given port, making it ready to be connected.
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
Port_Init(Port *port, Portset *ps)
{
   memset(port, 0, sizeof(*port));
   port->ps = ps;

   PortReset(port);

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------------
 *
 * PortStatusProcRead --
 *
 *    port status proc read handler.
 *
 * Results:
 *    VMK_OK
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */
 
static int
PortStatusProcRead(Proc_Entry  *entry, 
                   char        *page, 
                   int         *len)
{
   *len = 0;
   PortFlag *flag = portFlags;
   Port *port = (Port *)entry->private;
   int i;
   List_Links *curEntry;
   IOChainLink *link;

   Proc_Printf(page, len, "\nPort flags:   ");
   while (flag->val != PORT_VALID_FLAGS) {
      if (port->flags & flag->val) {
         Proc_Printf(page, len, "%s   ", flag->string);
      }
      flag++;
   }
   Proc_Printf(page, len, "\n\n");

   Proc_Printf(page, len, "Port ethernet frame routing:\n\n");
   Proc_Printf(page, len, "%15s %20s %20s %20s %20s %20s %20s   %-17s   %-18s   %s\n",
               "",
               "unicastPassed",
               "multicastPassed",
               "broadcastPassed",
               "unicastBlocked",
               "multicastBlocked",
               "broadcastBlocked",
               "unicastAddr",
               "LADRF",
               "flags");

   Proc_Printf(page, len, "%15s %20Lu %20Lu %20Lu %20Lu %20Lu %20Lu   "
               ETH_ADDR_FMT_STR "   0x%08x%08x " ETH_FILTER_FLAG_FMT_STR "\n",
               "input:",
               port->ethFRP.inputFilter.passed.unicastFrames,
               port->ethFRP.inputFilter.passed.multicastFrames,
               port->ethFRP.inputFilter.passed.broadcastFrames,
               port->ethFRP.inputFilter.blocked.unicastFrames,
               port->ethFRP.inputFilter.blocked.multicastFrames,
               port->ethFRP.inputFilter.blocked.broadcastFrames,
               ETH_ADDR_FMT_ARGS(&port->ethFRP.inputFilter.unicastAddr),
               port->ethFRP.inputFilter.ladrf[0],
               port->ethFRP.inputFilter.ladrf[1],
               ETH_FILTER_FLAG_FMT_ARGS(port->ethFRP.inputFilter.flags));

   Proc_Printf(page, len, "%15s %20Lu %20Lu %20Lu %20Lu %20Lu %20Lu   "
               ETH_ADDR_FMT_STR "   0x%08x%08x " ETH_FILTER_FLAG_FMT_STR "\n\n",
               "output:",
               port->ethFRP.outputFilter.passed.unicastFrames,
               port->ethFRP.outputFilter.passed.multicastFrames,
               port->ethFRP.outputFilter.passed.broadcastFrames,
               port->ethFRP.outputFilter.blocked.unicastFrames,
               port->ethFRP.outputFilter.blocked.multicastFrames,
               port->ethFRP.outputFilter.blocked.broadcastFrames,
               ETH_ADDR_FMT_ARGS(&port->ethFRP.outputFilter.unicastAddr),
               port->ethFRP.outputFilter.ladrf[0],
               port->ethFRP.outputFilter.ladrf[1],
               ETH_FILTER_FLAG_FMT_ARGS(port->ethFRP.outputFilter.flags));

   Proc_Printf(page, len, "Port iochains:\n\n");
   Proc_Printf(page, len, "%15s %20s %20s %20s %20s %20s %20s %20s %20s   %s",
               "", 
               "starts", 
               "resumes", 
               "errors",
               "pktstarted", 
               "pktsPassed", 
               "pktsFiltered", 
               "pktsQueued", 
               "pktsDropped",
               "callChain");
   Proc_Printf(page, len, "\n%15s %20Lu %20Lu %20Lu %20Lu %20Lu %20Lu %20Lu %20Lu   ", 
               "input:",
               port->inputChain.stats.starts, 
               port->inputChain.stats.resumes, 
               port->inputChain.stats.errors, 
               port->inputChain.stats.pktsStarted,
               port->inputChain.stats.pktsPassed,
               port->inputChain.stats.pktsFiltered,
               port->inputChain.stats.pktsQueued,
               port->inputChain.stats.pktsDropped);
   for (i = 0; i < MAX_CHAIN_RANKS; i++) {
      LIST_FORALL(&port->inputChain.chainHeads[i], curEntry) {
         link = (IOChainLink *)curEntry;
         Proc_Printf(page, len, " -> %u:%s", link->rank, link->ioChainFnName);
      }
   }
   Proc_Printf(page, len, "\n%15s %20Lu %20Lu %20Lu %20Lu %20Lu %20Lu %20Lu %20Lu   ", 
               "output:",
               port->outputChain.stats.starts, 
               port->outputChain.stats.resumes, 
               port->outputChain.stats.errors, 
               port->outputChain.stats.pktsStarted,
               port->outputChain.stats.pktsPassed,
               port->outputChain.stats.pktsFiltered,
               port->outputChain.stats.pktsQueued,
               port->outputChain.stats.pktsDropped);
   for (i = 0; i < MAX_CHAIN_RANKS; i++) {
      LIST_FORALL(&port->outputChain.chainHeads[i], curEntry) {
         link = (IOChainLink *)curEntry;
         Proc_Printf(page, len, " -> %u:%s", link->rank, link->ioChainFnName);
      }
   }
   Proc_Printf(page, len, "\n%15s %20Lu %20Lu %20Lu %20Lu %20Lu %20Lu %20Lu %20Lu   ", 
               "iocomplete:",
               port->notifyChain.stats.starts, 
               port->notifyChain.stats.resumes, 
               port->notifyChain.stats.errors, 
               port->notifyChain.stats.pktsStarted,
               port->notifyChain.stats.pktsPassed,
               port->notifyChain.stats.pktsFiltered,
               port->notifyChain.stats.pktsQueued,
               port->notifyChain.stats.pktsDropped);
   for (i = 0; i < MAX_CHAIN_RANKS; i++) {
      LIST_FORALL(&port->notifyChain.chainHeads[i], curEntry) {
         link = (IOChainLink *)curEntry;
         Proc_Printf(page, len, " -> %u:%s", link->rank, link->ioChainFnName);
      }
   }
   Proc_Printf(page, len, "\n\n");

   Proc_Printf(page, len, "Peer adapter statistics:\n\n");
   Proc_Printf(page, len, "%20s %20s %20s %20s %20s %20s %20s\n",
               "pktsTxOK",
               "bytesTxOK",
               "pktsRxOK",
               "bytesRxOK",
               "droppedTx",
               "droppedRx",
               "interrupts");
   Proc_Printf(page, len, "%20Lu %20Lu %20Lu %20Lu %20Lu %20Lu %20Lu\n",
               port->clientStats.pktsTxOK,
               port->clientStats.bytesTxOK,
               port->clientStats.pktsRxOK,
               port->clientStats.bytesRxOK,
               port->clientStats.droppedTx,
               port->clientStats.droppedRx,
               port->clientStats.interrupts);

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * PortProcCreate --
 *
 *      Create a proc dir and populate it for the given port.
 *
 * Results: 
 *	VMK_ReturnStatus and proc nodes are created for the port.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
PortProcCreate(Port *port)
{
   char name[5];

   snprintf(name, sizeof(name) - 1, "%u", Portset_GetPortIdx(port));

   Proc_InitEntry(&port->procDir);
   port->procDir.parent = &port->ps->procPortsDir;
   ProcNet_Register(&port->procDir, name, TRUE);
   Proc_InitEntry(&port->procStatus);
   port->procStatus.parent = &port->procDir;
   port->procStatus.read = PortStatusProcRead;
   port->procStatus.private = port;
   ProcNet_Register(&port->procStatus, "status", FALSE);

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * PortProcDestroy --
 *
 *      Cleanup the proc nodes for a portset.
 *
 * Results: 
 *	Proc nodes for the port are deleted.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
PortProcDestroy(Port *port)
{
   ProcNet_Remove(&port->procStatus);
   ProcNet_Remove(&port->procDir);
}

/*
 *----------------------------------------------------------------------
 *
 * Port_Connect --
 *
 *      Connect a given port, making it ready to be enabled.
 *
 * Results: 
 *	VMK_ReturnStatus.
 *
 * Side effects:
 *	Proc nodes created.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Port_Connect(Port *port, PortID portID)
{
   VMK_ReturnStatus status = VMK_OK;

   ASSERT(Portset_LockedExclHint(port->ps));

   if (UNLIKELY(VMK_STRESS_DEBUG_COUNTER(NET_PORT_CONNECT_FAIL))) {
      return VMK_FAILURE;
                  
   }

   if (port->flags & PORT_FLAG_IN_USE) {
      ASSERT(FALSE);
      return VMK_BUSY;
   }

   port->flags |= PORT_FLAG_IN_USE;
   ASSERT(Portset_GetPortIdx(port) == 
          Portset_PortIdxFromPortID(portID, port->ps));
   port->portID = portID;

   IOChain_Init(&port->outputChain, port->portID);
   IOChain_Init(&port->inputChain, port->portID);
   IOChain_Init(&port->notifyChain, port->portID);

   PortProcCreate(port);

   return status;
}


/*
 *----------------------------------------------------------------------------
 *
 * Port_AssociateCOSWorld --
 *
 *    Associate a COS world with the given port.
 *
 * Results:
 *    VMK_OK on success. Error code on failure.
 *
 * Side effects:
 *    None
 *
 *----------------------------------------------------------------------------
 */

VMK_ReturnStatus
Port_AssociateCOSWorld(Port *port, World_ID worldID)
{
   VMK_ReturnStatus status = VMK_OK;
   World_Handle *world = NULL;

   ASSERT(worldID != INVALID_WORLD_ID);

   world = World_Find(worldID);
   if (world) {
      ASSERT(World_IsHOSTWorld(world));

      port->worldAssc = worldID;
      port->worldArr[0] = world;
      port->numWorlds   = 1;
      port->flags |= PORT_FLAG_WORLD_ASSOC;

      World_Release(world); // not a problem with the COS world
   } else {
      LOG(0, "Couldn't find world associated with world id 0x%x", worldID);
      return VMK_NOT_FOUND;
   }
   return status;
}


/*
 *----------------------------------------------------------------------------
 *
 * PortDisassociateCOSWorld --
 * 
 *    Remove the port's association with the host world.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */
 
static void
PortDisassociateCOSWorld(Port *port)
{
   ASSERT(port);
   ASSERT(World_IsHOSTWorld(port->worldArr[0]));

   port->worldAssc = INVALID_WORLD_ID;
   port->worldArr[0] = NULL;
   port->numWorlds   = 0;
   port->flags &= ~PORT_FLAG_WORLD_ASSOC;
}



/*
 *----------------------------------------------------------------------
 *
 * Port_AssociateVmmWorldGroup --
 *
 *      Associate a port with a given world (as well as its vcpu 
 *      siblings, if any) for accounting and interrupt delivery 
 *      purposes.
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
Port_AssociateVmmWorldGroup(Port *port, World_ID worldID)
{
   World_Handle *world = NULL;
   VMK_ReturnStatus status = VMK_OK;

   if (UNLIKELY(VMK_STRESS_DEBUG_COUNTER(NET_PORT_WORLD_ASSOC_FAIL))) {
      status = VMK_FAILURE;
      goto done;
   }
   
   memset(port->worldArr, 0, sizeof(port->worldArr));
   port->worldAssc = worldID;
   if (worldID == INVALID_WORLD_ID) {      
      /*
       * non vmm clients don't need a world associated
       */
      status = VMK_OK; 
      goto done;
   }

   world = World_Find(worldID);

   ASSERT(World_IsVMMWorld(world));

   if (world == NULL) {
      LOG(0, "couldn't find world %x", worldID);
      status = VMK_NOT_FOUND;
      goto done;
   }

   /*
    * add this port to the world group's array of ports
    */
   if (World_VMMGroup(world)->netInfo.numPorts >= MAX_VMM_GROUP_NET_PORTS) {
      VmWarn(worldID, "too many ports open on world VMM group");
      status = VMK_LIMIT_EXCEEDED;
      goto done;
   }
   World_VMMGroup(world)->netInfo.portIDs[World_VMMGroup(world)->netInfo.numPorts] = 
      port->portID;
   World_VMMGroup(world)->netInfo.numPorts++;
   LOG(1, "numPorts %u", World_VMMGroup(world)->netInfo.numPorts);

   /*
    * fill up the port's array of worlds
    */
   port->numWorlds = 
      World_GetVmmMembers(world, port->worldArr);
   ASSERT(port->numWorlds > 0);

   port->flags |= PORT_FLAG_WORLD_ASSOC;

   LOG(0, "world %u %s ---> port 0x%x on %s", 
       worldID, world->worldName, port->portID, port->ps->name);

  done:

   if (world != NULL) {
      World_Release(world);
   }
   
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * PortDisassociateVmmWorldGroup --
 *
 *      Disassociate a port from the given VMM group.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
PortDisassociateVmmWorldGroup(Port *port, World_Handle *world)
{
   unsigned int i,j;
   unsigned int numPorts = World_VMMGroup(world)->netInfo.numPorts;
   Net_PortID *portIDs = World_VMMGroup(world)->netInfo.portIDs;

   LOG(0, "world %u %s -X-> port 0x%x on %s", 
       world->worldID, world->worldName, port->portID, port->ps->name);

   for (i = 0, j = 0; i < numPorts; i++) {
      portIDs[j] = portIDs[i];
      if (port->portID == portIDs[i]) {
         World_VMMGroup(world)->netInfo.numPorts--;
      } else {
         j++;
      }
   }

   // we should have removed one and only one entry
   ASSERT((numPorts - 1) == World_VMMGroup(world)->netInfo.numPorts);
}

/*
 *----------------------------------------------------------------------
 *
 * PortDisassociateVmmWorld --
 *
 *      Disassociate a port from the given VMM world (used when a world is
 *      in the process of dying) If the targetWorld argument is NULL,
 *      then the port is disassociated with all worlds (used when the
 *      port is being disconnected).
 *
 * Results: 
 *	VMK_ReturnStatus.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
PortDisassociateVmmWorld(Port *port, World_Handle *targetWorld)
{
   unsigned int i,j;
   unsigned int numWorlds = port->numWorlds;

   ASSERT(Portset_LockedExclHint(port->ps));

   for (i = 0, j = 0; i < numWorlds; i++) {
      World_Handle *world = port->worldArr[i];
      ASSERT(World_IsVMMWorld(world));
      port->worldArr[j] = port->worldArr[i];
      if ((targetWorld == NULL) || (world == targetWorld)) {
         LOG(0, "world %u %s -X-> port 0x%x on %s", 
             world->worldID, world->worldName, port->portID, port->ps->name);
         ASSERT(port->numWorlds > 0);
         if (port->numWorlds == 1) {
            // last one out shut off the lights
            PortDisassociateVmmWorldGroup(port, world);
         }
         World_Release(world);
         port->numWorlds--;
      } else {
         j++;
      }
   }

   if (targetWorld != NULL) {
      // we should have removed one and only one entry
      ASSERT((numWorlds - 1) == port->numWorlds);
   }

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Port_DisassociateVmmWorld --
 *
 *      wrapper for PortDisassociateVmmWorld()
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
Port_DisassociateVmmWorld(PortID portID, World_Handle *world)
{
   VMK_ReturnStatus status;
   Port *port = Portset_GetPortExcl(portID);

   if (port != NULL) {
      status = PortDisassociateVmmWorld(port, world);
      Portset_ReleasePortExcl(port);
   } else {
      status = VMK_NOT_FOUND;
   }

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * Port_Disconnect --
 *
 *      Disconnect the given port, making it available for reuse.
 *
 * Results: 
 *	VMK_ReturnStatus.  
 *
 * Side effects:
 *	Proc nodes are destroyed.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Port_Disconnect(Port *port)
{
   ASSERT(Portset_LockedExclHint(port->ps));

   LOG(3, "0x%x", port->portID);

   PortProcDestroy(port);

   if (port->worldArr[0] && World_IsHOSTWorld(port->worldArr[0])) {
      PortDisassociateCOSWorld(port);
   } else {
      // drop our association with all VMM worlds (if any)
      PortDisassociateVmmWorld(port, NULL);
   }
   ASSERT(port->numWorlds == 0);
   /*
    * LOOKOUT: can't fail after here bc Net_WorldPreCleanup()
    *          depends on it.
    */

   PortReset(port);

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------------
 *
 * Port_Enable --
 *
 *    Enable the given port making it ready to send and recieve frames.
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
Port_Enable(Port *port)
{
   VMK_ReturnStatus status = VMK_OK;

   ASSERT(port->flags & PORT_FLAG_IN_USE);
   ASSERT(Portset_LockedExclHint(port->ps));

   if (UNLIKELY(VMK_STRESS_DEBUG_COUNTER(NET_PORT_ENABLE_FAIL))) {
      return VMK_FAILURE;
   }

   if (port->impl.enable) {
      status = port->impl.enable(port);
   }
   
   if (status == VMK_OK) {
      status = Portset_EnablePort(port);
      Portset_EnablePort(port);
   } else {
      LOG(0, "Impl specific enable failed for port 0x%x: %s", port->portID,
          VMK_ReturnStatusToString(status));
   }

   if (status == VMK_OK) {
      port->flags |= PORT_FLAG_ENABLED;
   } else {
      LOG(0, "Failed to enable port 0x%x on portset %s: %s", port->portID,
          port->ps->name, VMK_ReturnStatusToString(status));
   }

   return status;
}


/*
 *----------------------------------------------------------------------------
 *
 * Port_Disable --
 *
 *    Disable the given port.
 *
 * Results:
 *    VMK_ReturnStatus, usually VMK_OK, sometimes VMK_BUSY if force is FALSE,
 *    and there are still transmitted packets outstanding for the port.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */
VMK_ReturnStatus
Port_Disable(Port *port, Bool force)
{
   VMK_ReturnStatus status = VMK_OK;

   ASSERT(port->flags & PORT_FLAG_IN_USE);
   ASSERT(Portset_LockedExclHint(port->ps));

   port->flags |= PORT_FLAG_DISABLE_PENDING;
   port->flags &= ~PORT_FLAG_ENABLED;

   if (port->impl.disable) {
      status = port->impl.disable(port, force);
      // make sure they protect from being called again
      ASSERT((port->impl.disable == NULL) || !((status == VMK_OK) || force));
   }
   
   if ((status == VMK_OK) || force) {
      status = Portset_DisablePort(port, force);
   }

   if ((status == VMK_OK) || force) {
      IOChain_ReleaseChain(&port->notifyChain);
      IOChain_ReleaseChain(&port->outputChain);
      IOChain_ReleaseChain(&port->inputChain);

      port->flags &= ~PORT_FLAG_DISABLE_PENDING;
   }

   return status;
}

/*
 *----------------------------------------------------------------------------
 *
 * Port_BlockUntilDisabled --
 *
 *    Deschedule the current thread until the given port is disabled.
 *
 * Results:
 *    VMK_ReturnStatus.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */
Port *
Port_BlockUntilDisabled(Port *port)
{
#ifndef ESX3_NETWORKING_NOT_DONE_YET
#error implement me
#endif

   return port;
}


/*
 *----------------------------------------------------------------------------
 *
 * Port_UpdateEthFRP --
 *
 *    Update the ethernet frame routing policy for the port, and notify
 *    the parent portset if it cares.
 *
 * Results:
 *    VMK_ReturnStatus, usually VMK_OK, sometimes VMK_BUSY if force is FALSE,
 *    and there are still transmitted packets outstanding for the port.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */
VMK_ReturnStatus
Port_UpdateEthFRP(Port *port, Eth_FRP *frp)
{
   VMK_ReturnStatus status = VMK_OK;

   ASSERT(port->flags & PORT_FLAG_IN_USE);
   ASSERT(Portset_LockedExclHint(port->ps));

   /*
    * XXX here is where we will check with the security policy 
    *     to see if the requested changes are allowed, as well 
    *     as craft an input filter to enforce the tx restrictions 
    *     of the policy.  For now just allow whatever RX filter
    *     they want and don't apply a a TX filter at all. 
    *     (remember that "input" and "output" are wrt the portset,
    *     so their sense is reversed wrt to "rx" and "tx" here)
    */
#ifdef ESX3_NETWORKING_NOT_DONE_YET
   frp->inputFilter.flags |= ETH_FILTER_PROMISC;
#else
#error "implement security policy"
#endif // ESX3_NETWORKING_NOT_DONE_YET

   status = Portset_UpdatePortEthFRP(port, frp);

   if (status == VMK_OK) {
      memcpy (&port->ethFRP, frp, sizeof(*frp));
   }
      
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * Port_InputResume --
 *
 *      Input a list of packets to a port, starting after the indicated
 *      iochain link.  The list will be emptied on success or failure.
 *
 * Results: 
 *      VMK_ReturnStatus.
 *
 * Side effects:
 *	Other ports on the portset may receive packets.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
Port_InputResume(Port* port, IOChainLink *prev, PktList *pktList)
{
   VMK_ReturnStatus status = VMK_OK;

   Pkt_DbgOnInput(pktList); // nop on release builds

   pktList->mayModify = TRUE;
      
   if (Port_IsInputActive(port) &&
       !VMK_STRESS_DEBUG_COUNTER(NET_PORT_INPUT_RESUME_FAIL)) {

      status = IOChain_Resume(port, &port->inputChain, prev, pktList);
      
      if (status == VMK_OK) {
         status = Portset_Input(port, pktList);
      }
   }   

   /*
    * the portset will prune anything it wanted to keep from the 
    * list, so we complete anything else left over here
    */
   Port_IOComplete(port, pktList);

   return status;
}


