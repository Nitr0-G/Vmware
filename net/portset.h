/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. 
 * -- VMware Confidential
 * **********************************************************/

/*
 * portset.h  --
 *
 *    Interface to network private portset API. 
 *
 *    Portsets are groups of ports which, together with 
 *    policies for frame routing, form virtual networks.
 *
 *    The portset implementaion is a base class for more
 *    useful subclasses, like EtherSwitch.
 */

#ifndef _PORTSET_H_
#define _PORTSET_H_

/*
 * specific device implementations (e.g. loopback, hub, etherswitch, etc)
 * define some or all of the entry points below.
 */
typedef VMK_ReturnStatus (*PortsetDispatch)         (Portset *, 
                                                     struct PktList *, Port *);
typedef VMK_ReturnStatus (*PortsetPortConnect)      (Portset *, Port *);
typedef VMK_ReturnStatus (*PortsetPortDisconnect)   (Portset *, Port *);
typedef VMK_ReturnStatus (*PortsetPortEnable)       (Port *);
typedef VMK_ReturnStatus (*PortsetPortDisable)      (Port *, Bool);
typedef VMK_ReturnStatus (*PortsetPortEthFRPUpdate) (Port*, Eth_FRP *);
typedef VMK_ReturnStatus (*PortsetDeactivate)       (Portset *);
typedef VMK_ReturnStatus (*PortsetConnectUplink)    (Portset *, char *, PortID *portID);
typedef VMK_ReturnStatus (*PortsetDisconnectUplink) (Portset *, char *);

typedef struct UplinkDevice UplinkDev;

typedef struct PortsetDevImpl {
   void                    *data;             // implementation-specific data
   PortsetDispatch          dispatch;         // port has received packets
   PortsetPortConnect       portConnect;      // port is being connected
   PortsetPortDisconnect    portDisconnect;   // port is being disconnected
   PortsetPortEnable        portEnable;       // port is being enabled
   PortsetPortDisable       portDisable;      // port is being disabled
   PortsetPortEthFRPUpdate  portEthFRPUpdate; // port changing ethernet routing policy
   PortsetDeactivate        deactivate;       // portset is being deactivated
   PortsetConnectUplink     uplinkConnect;    // uplink is being connected
   PortsetDisconnectUplink  uplinkDisconnect; // uplink is being disconnected
} PortsetDevImpl;

struct Portset {
   SP_RWLock          lock;            // protects all set structures.
   enum PortsetFlags {                            //
      PORTSET_FLAG_IN_USE          = 0x00000001,  // duh
      PORTSET_VALID_FLAGS          = 0x00000001   // all the flags that are allowed.
   }                  flags;           // combination of the above
   PortsetName        name;            // name of the set
   PortsetDevImpl     devImpl;         // device class specific implementation
   PortID             portgen;         // counter to generate new portIDs
   uint32             portIdxMask;     // mask to convert portIDs to indices
   uint16             numPorts;        // total number of ports available
   uint16             numPortsInUse;   // number of ports in use
   Port              *ports;           // numPorts sized array of ports
   Proc_Entry         procDir;         // proc dir for set-specific nodes
   Proc_Entry         procPortsDir;    // proc dir for set-specific nodes
   Proc_Entry         procNetDebug;    // proc node for setting the debugger
   uint32             uplinkMaxImplSz; // additional buffer space expected in tx pkts
   UplinkDev         *uplinkDev;       // pointer to the uplink device for this portset
   Net_Type           type;            // type of portset
};

extern uint32         numPortsets;
extern uint32         portsetIdxMask;
extern unsigned int   portsetIdxShift;
extern Portset       *portsetArray;
extern SP_SpinLock    portsetGlobalLock;

void             Portset_ModEarlyInit(void);
VMK_ReturnStatus Portset_ModInit(unsigned int num);
VMK_ReturnStatus Portset_FindByName(const char *name, Portset **pps);
VMK_ReturnStatus Portset_Activate(unsigned int numPorts, char *name, Portset **pps);
VMK_ReturnStatus Portset_Deactivate(Portset *ps);
void             Portset_ModCleanup(void);
VMK_ReturnStatus Portset_ConnectPort(Portset *ps, Port **port);
VMK_ReturnStatus Portset_DisconnectPort(Portset *ps, PortID portID);
VMK_ReturnStatus Portset_EnablePort(Port *port);
VMK_ReturnStatus Portset_DisablePort(Port *port, Bool force);
VMK_ReturnStatus Portset_UpdatePortEthFRP(Port *port, Eth_FRP *frp);

/*
 *----------------------------------------------------------------------
 *
 * Portset_GlobalLock/Unlock --
 *
 *      Acquire/release the global portsetGlobalLock, preventing any
 *      destructive access to the global array of portsets, wrapped so
 *      we can easily add debugging code in one place.
 *
 * Results:
 *      portsetGlobalLock is locked/unlocked.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
Portset_GlobalLock(void)
{
   SP_Lock(&portsetGlobalLock);
}

static INLINE void
Portset_GlobalUnlock(void)
{
   SP_Unlock(&portsetGlobalLock);
}


static INLINE Bool
Portset_GlobalLockedHint(void)
{
   return SP_IsLocked(&portsetGlobalLock);
}

/*
 *----------------------------------------------------------------------
 *
 * Portset_Lock(Excl|Nonexcl/Unlock --
 *
 *      Simple wrappers for reader writer locks, so we can easily add
 *      lock debugging code in one place.
 *
 * Results: 
 *	portset is locked/unlocked.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
Portset_LockNonexcl(Portset *ps)
{
   SP_AcqReadLock(&ps->lock);
}

static INLINE void
Portset_UnlockNonexcl(Portset *ps)
{
   SP_RelReadLock(&ps->lock);
}

static INLINE void
Portset_LockExcl(Portset *ps)
{
   SP_AcqWriteLock(&ps->lock);
}

static INLINE void
Portset_UnlockExcl(Portset *ps)
{
   SP_RelWriteLock(&ps->lock);
}

/*
 *----------------------------------------------------------------------
 *
 * Portset_Locked[Excl|Nonexcl]Hint --
 *
 *      Provide a hint about whether the portset is locked.  Suitable
 *      for ASSERTs and the like as there will be no false negatives.
 *      Sometimes gives false positives.
 *
 * Results: 
 *	FALSE if (but *not* only if) the portset is not locked.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
Portset_LockedNonexclHint(Portset *ps)
{
   return (SP_HintReadLocked(&ps->lock));
}

static INLINE Bool
Portset_LockedExclHint(Portset *ps)
{
   return SP_HintWriteLocked(&ps->lock);
}

static INLINE Bool
Portset_LockedHint(Portset *ps)
{
   return (Portset_LockedNonexclHint(ps) || Portset_LockedExclHint(ps));
}

/*
 *----------------------------------------------------------------------
 *
 * Portset_IsActive --
 *
 *      Is the portset activated?
 *
 * Results: 
 *	TRUE iff the portset has been activated.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
Portset_IsActive(Portset *ps)
{
   ASSERT(Portset_LockedHint(ps));

   if (ps->flags & PORTSET_FLAG_IN_USE) {
      return TRUE;
   }

   return FALSE;
}

/*
 *----------------------------------------------------------------------
 *
 * Portset_GetIdx --
 *
 *      Compute the index of the given portset in the global array.
 *
 * Results: 
 *	Returns the index.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE unsigned int
Portset_GetIdx(Portset *ps)
{
   unsigned int idx = (unsigned int)(ps - portsetArray);

   ASSERT(idx < numPortsets);

   return idx;
}

/*
 *----------------------------------------------------------------------
 *
 * Portset_GetPortIdx --
 *
 *      Compute the index of the given port in the portset's array.
 *
 * Results: 
 *	Returns the index.
 *
 * Side effects
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE unsigned int
Portset_GetPortIdx(Port *port)
{
   unsigned int idx = (unsigned int)(port - port->ps->ports);

   ASSERT(idx < port->ps->numPorts);

   return idx;
}


/*
 *----------------------------------------------------------------------
 *
 * Portset_IdxFromPortID --
 *
 *      Extract a portset index from the given PortID.
 *
 * Results: 
 *      Returns the index, based on the current mask.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE unsigned int
Portset_IdxFromPortID(PortID id)
{
   unsigned int idx = ((id >> portsetIdxShift) & portsetIdxMask);
   
   ASSERT(id != NET_INVALID_PORT_ID);
   ASSERT(idx < numPortsets);
   
   return idx;
}

/*
 *----------------------------------------------------------------------
 *
 * Portset_PortIdxFromPortID --
 *
 *      Extract a port index from the given PortID.
 *
 * Results: 
 *      Returns the index, based on the current mask.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE unsigned int
Portset_PortIdxFromPortID(PortID portID, Portset *ps)
{
   unsigned int idx = (portID & ps->portIdxMask);
   
   ASSERT(portID != NET_INVALID_PORT_ID);

   /*
    * no need to check this in a release as the mask used to extract
    * the index from the PortID is based on the size of the array
    */
   ASSERT(idx < ps->numPorts);
   
   return idx;
}

/*
 *----------------------------------------------------------------------
 *
 * Portset_FindByPortID --
 *
 *      Simple accessor for the portsetArray
 *
 * Results: 
 *	Returns a pointer to the portset which contains the given portID.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Portset *
Portset_FindByPortID(PortID portID)
{
   ASSERT(portID != NET_INVALID_PORT_ID);

   // no lock needed for read access to this array
   return &portsetArray[Portset_IdxFromPortID(portID)];
}


/*
 *----------------------------------------------------------------------------
 *
 *  Portset_GetPortExcl --
 *      Find the port indicated by the given ID and return a pointer
 *      to it, with exclusive access to the parent set.  
 *
 * Results: 
 *	Pointer to Port on success, NULL on failure.
 *
 * Side effects:
 *	The parent Portset is write locked.
 *
 *----------------------------------------------------------------------------
 */

static INLINE Port *
Portset_GetPortExcl(PortID portID)
{
   Portset *ps;
   Port *port;

   ASSERT(portID != NET_INVALID_PORT_ID);

   ps = Portset_FindByPortID(portID);
   Portset_LockExcl(ps);
   
   if (Portset_IsActive(ps)) {
      unsigned int idx = Portset_PortIdxFromPortID(portID, ps);
      
      port = &ps->ports[idx];
      
      /*
       * test all 32 bits of the ID (including the generation and set
       * index) so that old portIDs don't map to newer ones after we
       * wrap modulo the mask.
       */
      if (port->portID == portID) {
         return port;
      }
   }
      
   // failure

   Portset_UnlockExcl(ps);
   return NULL;
}


/*
 *----------------------------------------------------------------------------
 *
 *  Portset_ReleasePortExcl --
 *
 *      Release the reference to a port obtained from Portset_GetPortExcl(). 
 *
 * Results: 
 *	The port reference is released.
 *
 * Side effects:
 *	The parent Portset's write lock is released.
 *
 *----------------------------------------------------------------------------
 */

static INLINE void
Portset_ReleasePortExcl(Port* port)
{
   Portset_UnlockExcl(port->ps);
}


/*
 *----------------------------------------------------------------------------
 *
 * Portset_GetLockedPort --
 *
 *    Get the port corresponding to portID. This function must be used only if
 *    the caller is sure that the portset's lock is already held.
 *
 * Results:
 *    VMK_OK on success, VMK_NOT_FOUND on failure
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE VMK_ReturnStatus
Portset_GetLockedPort(PortID portID, Port **pport)
{
   Portset *ps = Portset_FindByPortID(portID);

   ASSERT(portID != NET_INVALID_PORT_ID);
   ASSERT(pport);
   ASSERT(ps);
   ASSERT(Portset_LockedHint(ps));

   *pport = NULL;

   if (Portset_LockedHint(ps)) {
      if (Portset_IsActive(ps)) {
         unsigned int idx = Portset_PortIdxFromPortID(portID, ps);
         Port *port = &ps->ports[idx];
         if (port->portID == portID) {
            *pport = port;
            return VMK_OK;
         }
      }
      return VMK_NOT_FOUND;
   } else {
      return VMK_FAILURE;
   }
}

   
/*
 *----------------------------------------------------------------------
 *
 * Portset_GetPort --
 *
 *      Find the port indicated by the given ID and return a pointer
 *      to it, with nonexclusive access to the parent set.  
 *
 * Results: 
 *	Pointer to Port on success, NULL on failure.
 *
 * Side effects:
 *	The parent Portset is read locked.
 *
 *----------------------------------------------------------------------
 */
static INLINE VMK_ReturnStatus
Portset_GetPort(PortID portID, Port **pport)
{
   Portset *ps;
   Port *port;
   VMK_ReturnStatus status = VMK_FAILURE;

   ASSERT(portID != NET_INVALID_PORT_ID);

   ps = Portset_FindByPortID(portID);
   ASSERT(ps);
   Portset_LockNonexcl(ps);
   
   if (Portset_IsActive(ps)) {
      unsigned int idx = Portset_PortIdxFromPortID(portID, ps);
      
      port = &ps->ports[idx];
      
      /*
       * test all 32 bits of the ID (including the generation and set
       * index) so that old portIDs don't map to newer ones after we
       * wrap modulo the mask.
       */
      if (port->portID == portID) {
         *pport = port;
         return VMK_OK;
      }
   } else {
      status = VMK_NOT_FOUND;
   }
      
   // failure

   *pport = NULL;
   Portset_UnlockNonexcl(ps);
   return status;
} 

/*
 *----------------------------------------------------------------------
 *
 * Portset_ReleasePort --
 *
 *      Release the reference to a port obtained from Portset_GetPort(). 
 *
 * Results: 
 *	The port reference is released.
 *
 * Side effects:
 *	The parent Portset's read lock is released.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
Portset_ReleasePort(Port* port)
{
   Portset_UnlockNonexcl(port->ps);
}

/*
 *----------------------------------------------------------------------
 *
 * Portset_Input --
 *
 *      Input a list of packets to a portset.  Some or all of the 
 *      packets may be removed from the list and held by the portset
 *      to be completed later.
 *
 * Results: 
 *      VMK_ReturnStatus.
 *
 * Side effects:
 *	Ports on the portset may receive packets.
 *
 *----------------------------------------------------------------------
 */
static INLINE VMK_ReturnStatus
Portset_Input(Port* port, PktList *pktList)
{
   VMK_ReturnStatus status;

   ASSERT(port->ps->devImpl.dispatch);
   status = port->ps->devImpl.dispatch(port->ps, pktList, port);
   return status;
}


/*
 *----------------------------------------------------------------------------
 *
 * Portset_SetUplinkImplSz --
 *
 *    Set the amount of additional buffer space expected by the uplink 
 *    implementation in each packet sent to it for tx. This space is typically
 *    used for maintaining the implementation's data structures.
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
Portset_SetUplinkImplSz(Portset *ps, uint32 uplinkImplSz)
{
   ASSERT(ps);
   if (ps->uplinkMaxImplSz < uplinkImplSz) {
      ps->uplinkMaxImplSz = uplinkImplSz;
   }
}


/*
 *----------------------------------------------------------------------------
 *
 * Portset_GetMaxUplinkImplSz --
 *
 *    Get the amount of additional buffer space the uplink implementation
 *    requires in a packet sent to it for tx.
 *
 * Results:
 *    The buffer space required by the implementation.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE uint32
Portset_GetMaxUplinkImplSz(Portset *ps)
{
   ASSERT(ps);
   return ps->uplinkMaxImplSz;
}

/*
 *----------------------------------------------------------------------------
 *
 * Portset_GetNameFromPortID --
 *
 *    Given a port, returns the name of the portset to which the port belongs.
 *    This function is used extensively by the uplink layer.
 *
 * Results:
 *    The name of the portset to which the given port belongs.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE void
Portset_GetNameFromPortID(PortID portID, char *buf, int bufLen)
{
   Portset *ps = Portset_FindByPortID(portID);
   ASSERT(buf);
   buf[0] = '\0';
   if (ps && strlen(ps->name) < bufLen) {
      strncpy(buf, ps->name, bufLen);
   }
}

#endif // __PORTSET_H_

