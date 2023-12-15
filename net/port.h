/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. 
 * -- VMware Confidential
 * **********************************************************/

/*
 * port.h  --
 *
 *    Interface to network private port API. 
 *
 *    Ports encapsulate the state of a connection to
 *    a virtual network.
 */

#ifndef _PORT_H_
#define _PORT_H_

// our abbrev version of the exported typedef from net_public.h
typedef Net_PortID PortID;

/*
 * PortIDs are used by clients to reference a port and it's parent
 * set.  Encoded in each PortID is an index into the static array
 * of portsets, an index into the portset's port array, and a 
 * generation counter.  The generation counter is used to help 
 * detect stale PortIDs.  
 *
 *      variable per modload           variable per portset config 
 *               |                               |
 *           <-- | -->                       <-- | -->
 *               |                               |
 *               V                               V
 * +---------------------------------------------------------------+
 * |  set index  |         generation            |   port index    |
 * +---------------------------------------------------------------+
 *
 * The global array of portsets is always sized at a power of 2 and an
 * appropriate mask for extracting an index from a PortID based on the
 * size of the array is stored in a global variable.  The size of the 
 * global portset array does not change for the life of the module. The
 * number of portsets may only be changed by reloading the module.
 *
 * Each portset has a power of 2 number of ports, and contains a field
 * with the appropriate mask for extracting the index of the port in
 * the portset's array.  Portsets may be extended by locking them 
 * exclusively, allocating a new array, changing the index mask, and
 * populating the new array based on the old portIDs modulo the new mask.
 * Portsets may not be shrunk since the remasked indices might overlap.
 * We could get around this limitation by creating a reopen action for
 * ports.
 *
 * All the bits left over between the set index and the port index 
 * serve as a generation counter so that portID != port->portID when
 * port is indexed by a stale portID
 */
#define DEFAULT_SET_INDEX_BITS      7
#define DEFAULT_PORT_INDEX_BITS     9

#define MAX_NUM_PORTSETS       1024
#define MAX_NUM_PORTS_PER_SET  1024

/*
 * specific port implementations may define callbacks for
 * events in the life of a port such as enable/disable, etc.
 */
typedef VMK_ReturnStatus (*PortEnable)     (Port *);
typedef VMK_ReturnStatus (*PortDisable)    (Port *, Bool);
typedef VMK_ReturnStatus (*PortDisconnect) (Port *);

typedef struct PortImpl {
   PortEnable       enable;       // called when the port is enabled
   PortDisable      disable;      // called when the port is disabled
   PortDisconnect   disconnect;   // called when the port is disconnected
   void            *data;         // implementation specific data
} PortImpl;

typedef struct PortClientStats {
   uint64  pktsTxOK;   // packets transmitted successfully
   uint64  bytesTxOK;  // bytes transmitted successfully
   uint64  pktsRxOK;   // packets received successfully
   uint64  bytesRxOK;  // bytes received successfully
   uint64  droppedTx;  // transmits dropped 
   uint64  droppedRx;  // receives dropped 
   uint64  interrupts; // number of client virtual interrupts
} PortClientStats;

#define PORT_FLAGS \
   PORT_FLAG(IN_USE,            0x00000001)       \
   PORT_FLAG(ENABLED,           0x00000002)       \
   PORT_FLAG(DISABLE_PENDING,   0x00000004)       \
   PORT_FLAG(WORLD_ASSOC,       0x00000008)       

#define PORT_FLAG(f,v) PORT_FLAG_##f = v,
struct Port {
   Portset          *ps;              // pointer to parent portset
   enum {                             //
      PORT_FLAGS
#undef PORT_FLAG
#define PORT_FLAG(f,v) v|
#define _PORT_VALID_FLAGS (PORT_FLAGS 0)
      PORT_VALID_FLAGS = _PORT_VALID_FLAGS
   }                  flags;          // combination of the above
   PortID             portID;         // check for stale PortIDs with this
   IOChain            outputChain;    // call chain for output
   IOChain            inputChain;     // call chain for input
   IOChain            notifyChain;    // call chain for io completions
   PortClientStats    clientStats;    // stats for the virtual nic attached to the port
   World_ID           worldAssc;      // world association (sortof ownership)
   World_Handle      *worldArr[MAX_VCPUS]; // world(s) we bill for time
                                                         // and send intrs to
   unsigned int       numWorlds;      // number of worlds in the above array
   Eth_FRP            ethFRP;         // ethernet frame routing policy
   Proc_Entry         procDir;        // proc dir for port-specific nodes
   Proc_Entry         procStatus;     // port status
   PortImpl           impl;           // port type specific calls and data
};
#undef PORT_FLAG


VMK_ReturnStatus  Port_Init(Port *port, Portset *ps);
VMK_ReturnStatus  Port_Connect(Port *port, PortID portID);
VMK_ReturnStatus  Port_Disconnect(Port *port);
VMK_ReturnStatus  Port_Enable(Port *port);
VMK_ReturnStatus  Port_Disable(Port *port, Bool force);
Port             *Port_BlockUntilDisabled(Port *port);
VMK_ReturnStatus  Port_UpdateEthFRP(Port *port, Eth_FRP *frp);
VMK_ReturnStatus  Port_AssociateVmmWorldGroup(Port *port, World_ID worldID);
VMK_ReturnStatus  Port_AssociateCOSWorld(Port *port, World_ID worldID);
VMK_ReturnStatus  Port_DisassociateVmmWorld(PortID portID, World_Handle *world);
VMK_ReturnStatus  Port_InputResume(Port* port, IOChainLink *start, PktList *pktList);


/*
 *----------------------------------------------------------------------
 *
 * Port_InitImpl --
 *
 *      Initialize the port's implementation hooks.
 *
 * Results: 
 *	port's implementation specific data is cleared.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
Port_InitImpl(Port *port)
{
   memset(&port->impl, 0, sizeof(port->impl));
}

/*
 *----------------------------------------------------------------------------
 *
 * Port_ClientStatInc --
 *
 *    Increment the stat by inc.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */
 
static INLINE void
Port_ClientStatInc(uint64 *stat, unsigned int inc)
{
   *stat += inc;
}


/*
 *----------------------------------------------------------------------
 *
 * Port_IsAvailable --
 *
 *      Is the port not reserved or in use?
 *
 * Results: 
 *	TRUE iff the port is available for use.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
Port_IsAvailable(Port *port)
{
   if (!(port->flags & PORT_FLAG_IN_USE)) {
      return TRUE;
   }

   return FALSE;
}

/*
 *----------------------------------------------------------------------
 *
 * Port_IsInputActive --
 *
 *      Is the port activated for input (ie should it accept packets
 *      from its client)?
 *
 * Results: 
 *	TRUE iff the port is activated.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
Port_IsInputActive(Port *port)
{
   if (port->flags & (PORT_FLAG_ENABLED | PORT_FLAG_DISABLE_PENDING)) {
      ASSERT(port->flags & PORT_FLAG_IN_USE);
      return TRUE;
   }

   return FALSE;
}


/*
 *----------------------------------------------------------------------
 *
 * Port_IsOutputActive --
 *
 *      Is the port activated for output (ie should it pass packets
 *      to its client)?
 *
 * Results: 
 *	TRUE iff the port is activated.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
Port_IsOutputActive(Port *port)
{
   if (port->flags & PORT_FLAG_ENABLED) {
      ASSERT(port->flags & PORT_FLAG_IN_USE);
      return TRUE;
   }

   return FALSE;
}


/*
 *----------------------------------------------------------------------------
 *
 * Port_IsEnabled --
 *
 *    Is the given port enabled.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE Bool
Port_IsEnabled(Port *port)
{
   if (port->flags & PORT_FLAG_ENABLED) {
      ASSERT(port->flags & PORT_FLAG_IN_USE);
      return TRUE;
   }

   return FALSE;
}


/*
 *----------------------------------------------------------------------------
 *
 * Port_CheckWorldAssociation --
 *
 *    Check that the given port is associated with the given world (or is
 *    not associated with any world)
 *
 * Results:
 *    VMK_ReturnStatus.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE VMK_ReturnStatus
Port_CheckWorldAssociation(Port *port, World_ID worldID)
{
   if (port->worldAssc == worldID) {
      return VMK_OK;
   }

   if (!(port->flags & PORT_FLAG_WORLD_ASSOC)) {
      // this port is a free agent
      return VMK_OK;
   }

   return VMK_INVALID_HANDLE;
}


/*
 *----------------------------------------------------------------------------
 *
 * Port_GetWorldGroupLeader --
 *
 *    Get the world group leader for the group associated with the port
 *    if any.
 *
 * Results:
 *    Returns a World_Handle, NULL on failure.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE World_Handle *
Port_GetWorldGroupLeader(Port *port)
{
   if (port->flags & PORT_FLAG_WORLD_ASSOC) {
      ASSERT(port->worldAssc != INVALID_WORLD_ID);
      ASSERT(port->worldArr[0] != NULL);
      return port->worldArr[0]->group->vmm.vmmLeader;
   }

   return NULL;
}

/*
 *----------------------------------------------------------------------------
 *
 * Port_IOComplete --
 *
 *    Handle IO Complete request.
 *
 * Results:
 *    VMK_ReturnStatus.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE VMK_ReturnStatus
Port_IOComplete(Port *port, PktList *pktList)
{
   VMK_ReturnStatus status;
   PktList completionList[1];
   PktHandle *pkt = PktList_GetHead(pktList);
   
   PktList_Init(completionList);

   while (pkt != NULL) {
      PktList_Remove(pktList, pkt);
      pkt = Pkt_ReleaseOrComplete(pkt);
      if (pkt != NULL) {
         PktList_AddToTail(completionList, pkt);
      }
      pkt = PktList_GetHead(pktList);
    }

   Pkt_DbgOnNotify(completionList); // nop on release builds
   status = IOChain_Start(port, &port->notifyChain, completionList);
   
   ASSERT(PktList_IsEmpty(pktList));

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * PortOutput --
 *
 *      Output a list of packets to a port.
 *
 * Results: 
 *      VMK_ReturnStatus.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static INLINE VMK_ReturnStatus
PortOutput(Port* port, IOChainLink *prev, PktList *pktList)
{
   Pkt_DbgOnOutput(pktList); // nop on release builds
   if (UNLIKELY(VMK_STRESS_DEBUG_COUNTER(NET_PORT_OUTPUT_CORRUPT))) {
      uint32 bufOffset = 0;
      PktHandle *handle = PktList_GetHead(pktList);
      char* buf = (char *)handle->frameVA;
      if (buf) {
         buf[0] = Util_RandSeed()%0x7E + 1;
         for(bufOffset = 1; bufOffset < 40; bufOffset++) {
            buf[bufOffset] = Util_FastRand(buf[bufOffset-1])%0x7E + 1;
         }
      }
   }
   return IOChain_Resume(port, &port->outputChain, prev, pktList);
}

/*
 *----------------------------------------------------------------------
 *
 * Port_Output --
 *
 *      Output a list of packets to a port.
 *
 * Results: 
 *      VMK_ReturnStatus.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static INLINE VMK_ReturnStatus
Port_Output(Port* port, PktList *pktList)
{
   return PortOutput(port, NULL, pktList);
}

/*
 *----------------------------------------------------------------------
 *
 * Port_OutputResume --
 *
 *      Resume output of a list of packets to a port.
 *
 * Results: 
 *      VMK_ReturnStatus.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static INLINE VMK_ReturnStatus
Port_OutputResume(Port* port, IOChainLink *prev, PktList *pktList)
{
   VMK_ReturnStatus status;
   status = PortOutput(port, prev, pktList);

   /*
    * Since we are not called in the context of Port_InputXXX() we
    * need to complete any packets we have here.  PktList_CompleteAll()
    * will iterate the list and return them to the apropriate port(s)
    * from completion.
    */
   PktList_CompleteAll(pktList);

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * Port_Input --
 *
 *      Input a list of packets to a port.  The list will be emptied 
 *      on success or failure.
 *
 * Results: 
 *      VMK_ReturnStatus.
 *
 * Side effects:
 *      Other ports on the portset may receive packets.
 *
 *----------------------------------------------------------------------
 */

static INLINE VMK_ReturnStatus
Port_Input(Port* port, PktList *pktList)
{
   if (UNLIKELY(VMK_STRESS_DEBUG_COUNTER(NET_PORT_INPUT_CORRUPT))) {
      uint32 bufOffset = 0;
      PktHandle *handle = PktList_GetHead(pktList);
      char* buf = (char *)handle->frameVA;
      if (buf) {
         buf[0] = Util_RandSeed()%0x7E + 1;
         for(bufOffset = 1; bufOffset < 40; bufOffset++) {
            buf[bufOffset] = Util_FastRand(buf[bufOffset-1])%0x7E + 1;
         }
      }
   }

   return Port_InputResume(port, NULL, pktList);
}


/*
 *----------------------------------------------------------------------------
 *
 * Port_InputOne --
 *
 *    Send one packet to the input chain. Creates a packet list for this 
 *    packet and sends it on its way.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE VMK_ReturnStatus
Port_InputOne(Port *port, PktHandle *pkt)
{
   PktList tmpList;

   ASSERT(port);
   ASSERT(pkt);

   PktList_Init(&tmpList);
   PktList_AddToTail(&tmpList, pkt);
   return Port_Input(port, &tmpList);
}


/*
 *----------------------------------------------------------------------------
 *
 * Port_ChooseWorldForIntr --
 *
 *    Chooses the best world to interrupt.
 *
 * Results:
 *    returns the best world to interrupt.
 *    XXX currently always chooses the first one in the array
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE World_Handle *
Port_ChooseWorldForIntr(Port *port)
{
   ASSERT(port->worldArr[0] != NULL);
   return port->worldArr[0];
}
#ifndef ESX3_NETWORKING_NOT_DONE_YET
#error implement me
#endif


/*
 *----------------------------------------------------------------------------
 *
 * Port_GetLeaderWorld --
 *
 *    Returns the leader world associated with this port.
 *
 * Results:
 *    Returns the leader world associated with this port.
 *    XXX currently just chooses the first one in the array, need to
 *        integrate with Mike's world group cleanup.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE World_Handle *
Port_GetLeaderWorld(Port *port)
{
   ASSERT(port->worldArr[0] != NULL);
   return port->worldArr[0];
}
#ifndef ESX3_NETWORKING_NOT_DONE_YET
#error implement me better
#endif


/*
 *----------------------------------------------------------------------
 *
 * Port_TryDisable --
 *
 *      Attempts to disable the port.
 *
 * Results: 
 *      VMK_ReturnStatus.
 *
 * Side effects:
 *      Other ports on the portset may receive packets which are 
 *      transmitted when we flush the port.
 *
 *----------------------------------------------------------------------
 */

static INLINE VMK_ReturnStatus
Port_TryDisable(Port* port)
{
   return Port_Disable(port, FALSE);
}


/*
 *----------------------------------------------------------------------
 *
 * Port_ForceDisable --
 *
 *      Attempts to forcefully disable the port.
 *
 * Results: 
 *      VMK_ReturnStatus.
 *
 * Side effects:
 *      Other ports on the portset may receive packets which are 
 *      transmitted when we flush the port.
 *
 *----------------------------------------------------------------------
 */

static INLINE VMK_ReturnStatus
Port_ForceDisable(Port* port)
{
   return Port_Disable(port, TRUE);
}


#endif // _PORT_H_

