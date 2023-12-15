/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. 
 * -- VMware Confidential
 * **********************************************************/

/* *****************************************************************
 *
 * bond.c -- Implements the ESX 3.x NIC Teaming feature.
 *
 *   Bond is a special portset with one or multiple uplinks.
 *
 *   Bond is not visible to the guest directly. Guest VMs would have to
 *   open a port to a regular portset, whose uplink connect to a vmnic or a bond.
 *   Bond is visible to the vmnic and the regular portset only.
 *
 *   Stage 1 implementation makes sure that the existing ESX Servers that
 *   have bond configured in the /etc/vmware/hwconfig will still be able to use Bond.
 *
 *   next stage todos:
 *   1) Failover and advanced teaming policy; 
 *   2) I intentially not use any INLINE in this round to make debugging easier
 *      in this and next stage.
 *   3) I might want to split bond.c into:
 *          bond.c (bond module open/register etc. routine code),
 *          bondBeacon.c (beacon protocol), and
 *          bondHash.c (calculates the hash based on MAC/IP address and check
 *                      and link/beacon state)
 *
 *****************************************************************
 *
 */
#define LOGLEVEL_MODULE Bond
#define LOGLEVEL_MODULE_LEN 0
#include "log_dist.h"

#include "net_int.h"
#include "parse.h"

static BondList bondList;

size_t vmklinuxPktHdrSize = 0;
size_t vmklinuxMaxSGLength = 0;

static VMK_ReturnStatus  BondDeactivate(Portset *ps);
static void              BondXConfigProcCreate(Bond *bond);
static void              BondXConfigProcRemove(Bond *bond);
static VMK_ReturnStatus  BondUplinkDisconnect(Portset *ps, char *uplinkName);
static VMK_ReturnStatus  BondUplinkConnect(Portset *ps, char *uplinkName, 
                                           PortID *portID);

/*
 *-----------------------------------------------------------------------------
 *
 * BondXProcRead --
 *
 *      Display the bond device config (/proc/vmware/net/devices/bond/config)
 *      
 * Results:
 *      int
 *
 * Side effects:
 *      None 
 *
 *-----------------------------------------------------------------------------
 */

static int
BondXProcRead(Proc_Entry *entry, char *page, int *len)
{
   Bond *bond = entry->private;   
   int i = 0;
   *len = 0;

   ASSERT(bond);
   ASSERT(bond->totalSlaveCount <= NICTEAMING_MAX_SLAVE_NUM);
   while (i < bond->totalSlaveCount) {
      Proc_Printf(page, len, "slave[%d] = %s, status = %s\n", 
                  bond->slave[i].index, bond->slave[i].uplinkName,
                  bond->slave[i].connected ? "connected":"disconnected");
      i++;
   }
   return VMK_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 * BondFindBondByName --
 *
 *     Look up the bond device by name 
 *
 * Results:
 *     The bond ptr found 
 *
 * Side effects:
 *     None. 
 *
 *-----------------------------------------------------------------------------
 */

Bond *
Bond_FindBondByName(char *bondName)
{
   List_Links *e = NULL;
   Bond *bond = NULL;
   ASSERT(SP_IsLocked(&portsetGlobalLock));
   LIST_FORALL(&bondList.bondList, e) {
      bond = (Bond *)e;
      if (strcmp(bondName, bond->devName) == 0) {
         return bond;
      }
   }
   return NULL;
}

/*
 *-----------------------------------------------------------------------------
 *
 * BondFindSlaveByName --
 *
 *     Look up the slave device by name given the bond 
 *
 * Results:
 *     Ptr to the slave found 
 *
 * Side effects:
 *     None. 
 *
 *-----------------------------------------------------------------------------
 */

static Slave *
BondFindSlaveByName(Bond *bond, char *slaveName)
{
   int i;
   for (i = 0; i < bond->totalSlaveCount; i++) {
      if (strcmp(bond->slave[i].uplinkName, slaveName) == 0) {
         ASSERT(bond->slave[i].uplinkPort != bond->portset->numPorts);
         return &(bond->slave[i]);
      }
   }
   return NULL;
}

/*
 *-----------------------------------------------------------------------------
 *
 * BondValidSlave --
 *
 *     Check if the slave name is valid. 
 *     XXX:  next stage todo
 *     1. sanity check on whether a slave is already a slave of another bond, etc.
 *     2. possibly remove the requirement that a vmnic has to be named as "vmnicX"
 *
 * Results:
 *     TRUE if it is a good slave candiate; FALSE otherwise.  
 *
 * Side effects:
 *     None 
 *
 *-----------------------------------------------------------------------------
 */

static Bool
BondValidSlave(char *slaveName)
{
   return(((strncmp(slaveName, "bond", 4) == 0)) 
       || ((strncmp(slaveName, "vmnic", 5)) == 0));
}

/*
 *-----------------------------------------------------------------------------
 *
 * BondUpdateHandleSlaveIndex --
 *
 *     XXX: stage 2 todo  
 *     go through each handle to update the primary slave choice
 *
 * Results:
 *      
 *
 * Side effects:
 *      
 *
 *-----------------------------------------------------------------------------
 */

static void
BondUpdateHandleSlaveIndex(Portset *ps)
{
   LOG(1, "updating %s handle slave hash", ps->name);
   return;
}

/*
 *-----------------------------------------------------------------------------
 *
 * BondXProcWrite --
 *
 *     Let the user configure the bondX settings.
 *
 * Results:
 *     int 
 *
 * Side effects:
 *     None 
 *
 *-----------------------------------------------------------------------------
 */

static int 
BondXProcWrite(Proc_Entry *entry, char *page, int *len)
{
   char *argv[3];
   Bond *bond = NULL;
   Portset *ps = NULL;
   int argc = Parse_Args(page, argv, 3);
   VMK_ReturnStatus ret = VMK_FAILURE;

   if (argc != 2) {
      LOG(0, "wrong number of args: %u, expected 2", argc);
      return VMK_FAILURE;
   }

   Portset_GlobalLock();

   bond = entry->private;   
   ASSERT(bond);
   ps = bond->portset;
   ASSERT(ps);
   Portset_LockExcl(ps);

   LOG(0, "%s %s in %s", argv[0], argv[1], bond->devName);
   if ((strcmp(argv[0], "add") == 0) && BondValidSlave(argv[1])) {
      PortID uplinkPortID;
      ret = BondUplinkConnect(bond->portset, argv[1], &uplinkPortID);
   } else if ((strcmp(argv[0], "delete") == 0) && BondValidSlave(argv[1])) {
      ret = BondUplinkDisconnect(bond->portset, argv[1]);
   } else {
      Warning("%s: command not supported", argv[1]);
   }

   if (ret == VMK_OK) {
      BondUpdateHandleSlaveIndex(bond->portset);
   }

   Portset_UnlockExcl(ps);

   Portset_GlobalUnlock();

   return VMK_OK;
}


/*
 *-----------------------------------------------------------------------------
 *
 * BondRxPkt --
 *
 *     Send the frames to the bond's upper (VM visible) portset
 *
 * Results:
 *     VMK_ReturnStatus 
 *
 * Side effects:
 *     The pktList are released.
 *
 *-----------------------------------------------------------------------------
 */

static VMK_ReturnStatus 
BondRxPkt(void *uplinkDev, struct PktList *pktList)
{
   UplinkDevice *uplink = uplinkDev;
   Port *port;

   ASSERT(uplinkDev);

   if (uplink->uplinkPort != NET_INVALID_PORT_ID) {
      LOG(3, "uplink = %p uplinkPort = 0x%x", uplink, uplink->uplinkPort);
      Portset_GetPort(uplink->uplinkPort, &port);
      if (port != NULL) {
         LOG(2, "pktList = %p uplink = %p uplinkPort = 0x%x",
                pktList, uplink, uplink->uplinkPort);
         Portset_Input(port, pktList);
         Portset_ReleasePort(port);
      } else {
         LOG(1, "Port is NULL");
         PktList_ReleaseAll(pktList);
      }
   } else {
      LOG(1, "uplinkPort is not defined for %s", uplink->devName);
      PktList_ReleaseAll(pktList);
   }
   return VMK_OK;
}


/*
 *-----------------------------------------------------------------------------
 *
 * BondOpen --
 *
 *     Bond open routine 
 *     XXX: add some flag to denote the bond's stat?
 *
 * Results:
 *     0 
 *
 * Side effects:
 *     None  
 *
 *-----------------------------------------------------------------------------
 */

static int
BondOpen(void *clientData)
{
   Bond *bond = (Bond *)clientData;
   ASSERT(bond);
   bond->refCount++;
   LOG(0, "%s (%d)", bond->devName, bond->refCount);
   return 0;
}


/*
 *-----------------------------------------------------------------------------
 *
 * BondClose --
 *
 *     Bond close routine 
 *
 * Results:
 *     0 
 *
 * Side effects:
 *     None 
 *
 *-----------------------------------------------------------------------------
 */

static int
BondClose(void *clientData)
{
   Bond *bond = (Bond *)clientData;
   ASSERT(bond);
   bond->refCount--;
   LOG(0, "%s (%d)", bond->devName, bond->refCount);
   return 0;
}

/*
 *-----------------------------------------------------------------------------
 *
 * BondPktHash --
 *
 *     Stage 1: simply return the first slave availble index.
 *
 *     XXX: Stage 2 todo: 
 *     Real teaming algorithm fits here, e.g. based on MAC or IP address hash 
 *
 * Results:
 *      
 *
 * Side effects:
 *      
 *
 *-----------------------------------------------------------------------------
 */

static int
BondPktHash(Bond *bond, PktHandle *pkt)
{
   int i;
#if 0
   if ((pkt->slaveNumHash) && bond->slave[i].connected) {
      return pkt->slaveNumHash;
   }
#else
   // for now, simply find the first usable slave index
   ASSERT(bond);
   ASSERT(bond->totalSlaveCount <= NICTEAMING_MAX_SLAVE_NUM);
   for (i=0; i<bond->totalSlaveCount; i++) {
      if (bond->slave[i].connected) {
         return i;
      }
   }
   return INVALID_SLAVE_NUM;
#endif
}

/*
 *-----------------------------------------------------------------------------
 *
 * BondTxPktList --
 *
 *     Prepare a new xmit pktList: the reason is that not all pkts from the srcList
 *     are necessarily going to the same slave. We first find out the slave index
 *     (currentSlaveHash) for the first pkt on srcList, move this pkt to 
 *     currentSlavePktList. Then we move all other pkts on srcList that are 
 *     currentSlaveHash bound and move them to the currentSlavePktList.
 *
 * Results:
 *     The slave of the first pkt is bound. 
 *
 * Side effects:
 *     None
 *
 *-----------------------------------------------------------------------------
 */

static Slave *
BondTxPktList(Bond *bond, struct PktList *srcList, struct PktList *currentSlavePktList)
{
   PktHandle *pkt = PktList_GetHead(srcList);
   int currentSlaveHash = INVALID_SLAVE_NUM;
   PktHandle *nextPkt = NULL;
   int pktSlaveHash;

   if (!pkt) {
      LOG(0, "no pkt");
      ASSERT(FALSE);
      return NULL;
   }

   PktList_Init(currentSlavePktList);

   while (pkt != NULL) {
      pktSlaveHash = BondPktHash(bond, pkt);

      // No usable slave
      if (pktSlaveHash == INVALID_SLAVE_NUM) {
         return NULL;
      }

      nextPkt = PktList_GetNext(srcList, pkt);

      if (currentSlaveHash == INVALID_SLAVE_NUM) {
         currentSlaveHash = pktSlaveHash;
      }

      if (currentSlaveHash == pktSlaveHash) {
         PktList_Remove(srcList, pkt);
         PktList_AddToTail(currentSlavePktList, pkt);
      }

      pkt = nextPkt;
   }
   LOG(2, "slave = %p", &bond->slave[currentSlaveHash]);
   ASSERT(currentSlaveHash != INVALID_SLAVE_NUM);
   return (&bond->slave[currentSlaveHash]);
}

/*
 *-----------------------------------------------------------------------------
 *
 * BondStartTx --
 *
 *    xmit routine for the upper portset. 
 *    We build one tmpPktList for each slave and call output seperately.
 *
 * Results:
 *    VMK_ReturnStatus     
 *
 * Side effects:
 *     pktList will be freed if the pkts are all sent out successfully.
 *
 *-----------------------------------------------------------------------------
 */

static VMK_ReturnStatus
BondStartTx(void *clientData, struct PktList *pktList)
{
   PktList tmpPktList;
   static VMK_ReturnStatus ret = VMK_OK;
   struct Port *port = NULL;
   Bond *bond = (Bond *)clientData;

   LOG(2, "%s: pktList = %p ps=%p", bond->devName, pktList, bond->portset);
   while (!PktList_IsEmpty(pktList)) {
      Slave *slave = BondTxPktList(bond, pktList, &tmpPktList);
      if (!slave) {
         LOG(0, "no usable slave device for %p", pktList);
         return VMK_FAILURE;
      }

      Portset_GetPort(slave->uplinkPort, &port);
      if (!port) {
         Warning("%s: no uplinkPort yet for slave %p", bond->devName, slave);
         return VMK_FAILURE;
      }

      LOG(2, "%s", bond->devName);
      ret = IOChain_Start(port, &port->outputChain, &tmpPktList);
      Portset_ReleasePort(port);
   }
   return VMK_OK;
}

// functions as a bottom device
Net_Functions bondBottomFunctions = {
   BondStartTx,
   BondOpen,
   BondClose,
};

/*
 *-----------------------------------------------------------------------------
 *
 * BondUplinkNotify --
 *
 *     Called upon slave device status change. 
 *
 *     Stage 2:
 *     XXX: may affect teaming decision 
 *
 * Results:
 *     VMK_ReturnStatus
 *
 * Side effects:
 *     None 
 *
 *-----------------------------------------------------------------------------
 */

static VMK_ReturnStatus
BondUplinkNotify(PortID portID, UplinkData *uplinkData, UplinkStatus status)
{
   LOG(0, "Received device notification for port 0x%x: maxSGLength = %u, "
          "pktHdrSize = %u, status %s", portID, uplinkData->maxSGLength,
          uplinkData->pktHdrSize, (status == UPLINK_DOWN)? "down":"up");

   return VMK_OK;
}


/*
 *-----------------------------------------------------------------------------
 *
 * BondRxPktList --
 *
 *     recv routine for the slave vmnic 
 *
 * Results:
 *     VMK_ReturnStatus 
 *
 * Side effects:
 *     None
 *
 *-----------------------------------------------------------------------------
 */

static VMK_ReturnStatus 
BondRxPktList(Portset *ps, struct PktList *pktList, Port *srcPort)
{
   Bond *bond = (Bond *)ps->devImpl.data;
   ASSERT(bond);
   return(BondRxPkt(bond->uplinkDev, pktList));
}


/*
 *-----------------------------------------------------------------------------
 *
 * BondCreateBottomDevice --
 *
 *     Create a device so that the upper portset can utilize.
 *     We need to do the same thing that a NIC driver needs to do when
 *     registering to the uplink.
 *
 * Results:
 *     VMK_ReturnStatus 
 *
 * Side effects:
 *     None 
 *
 *-----------------------------------------------------------------------------
 */

static VMK_ReturnStatus 
BondCreateBottomDevice(Portset *ps)
{
   VMK_ReturnStatus ret;
   Bond *bond = ps->devImpl.data;
   UplinkConnectArgs args;

   ASSERT(SP_IsLocked(&portsetGlobalLock));
   ASSERT(Portset_LockedExclHint(ps));
   ASSERT(bond);

   // XXX: Make sure vmklinux is loaded. Is it too much to ask for?
   if (vmklinuxPktHdrSize == 0) {
      Warning("vmklinux module is not loaded yet");
      ASSERT(FALSE); // XXX: Remove me!!
      return VMK_FAILURE;
   }

   LOG(2, "bond = %p", bond);

   // Equivalent of calling Uplink_DeviceConnected from the NIC driver
   memset(&args, 0, sizeof(args));

   args.uplinkImpl  = bond;
   args.moduleID    = 0;
   args.functions   = &bondBottomFunctions;
   args.pktHdrSize  = vmklinuxPktHdrSize;
   args.maxSGLength = vmklinuxMaxSGLength;
   args.type = DEVICE_TYPE_DEVICE_BOND;

   strncpy(args.devName, bond->devName, sizeof args.devName);

   ret = Uplink_SetDeviceConnected(&args, &bond->uplinkDev);

   if (ret != VMK_OK) {
      Warning("%s failed connect to the uplink", bond->devName);
      return ret;
   }

   // Equivalent of calling Uplink_PCIDeviceOpen() from the NIC driver
   Uplink_DeviceOpen(bond->uplinkDev);

   return ret;
}



/*
 *-----------------------------------------------------------------------------
 *
 * BondDestroyBottomDevice --
 *
 *     Destroy the bottom device    
 *
 * Results:
 *     VMK_ReturnStatus  
 *
 * Side effects:
 *     None 
 *
 *-----------------------------------------------------------------------------
 */

static void
BondDestroyBottomDevice(Bond *bond)
{
   ASSERT(bond);
   Uplink_DoDeviceDisconnected(bond->uplinkDev);
}

/*
 *-----------------------------------------------------------------------------
 *
 * BondXConfigProcCreate --
 *
 *     Create /proc/vmware/net/devices/bondX/slave
 *
 * Results:
 *     None 
 *
 * Side effects:
 *     None 
 *
 *-----------------------------------------------------------------------------
 */

static void
BondXConfigProcCreate(Bond *bond) {
   ASSERT(bond);
   ASSERT(bond->portset);
   ASSERT(bond->configEntry == NULL);

   bond->configEntry = (Proc_Entry *)Mem_Alloc(sizeof(Proc_Entry));
   ASSERT(bond->configEntry);

   Proc_InitEntry(bond->configEntry);
   bond->configEntry->parent = &bond->portset->procDir;
   bond->configEntry->read = BondXProcRead;
   bond->configEntry->write = BondXProcWrite;
   bond->configEntry->private = bond;
   Proc_Register(bond->configEntry, "slave", FALSE);
}


/*
 *-----------------------------------------------------------------------------
 *
 * BondXConfigProcRemove --
 *
 *     Remove /proc/vmware/net/devices/bondX/slave
 *
 * Results:
 *     None 
 *
 * Side effects:
 *     None 
 *
 *-----------------------------------------------------------------------------
 */

static void
BondXConfigProcRemove(Bond *bond)
{
   ASSERT(bond);
   if (bond->configEntry) {
      LOG(0, "removing %s config proc node", bond->devName);
      Proc_Remove(bond->configEntry);
      Mem_Free(bond->configEntry);
      bond->configEntry = NULL;
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * BondUplinkConnect --
 *
 *     Connect the bond to an a uplink (slave) 
 *
 * Results:
 *     VMK_ReturnStatus 
 *
 * Side effects:
 *     None 
 *
 *-----------------------------------------------------------------------------
 */

static VMK_ReturnStatus
BondUplinkConnect(Portset *ps, char *uplinkName, PortID *portID)
{
   VMK_ReturnStatus ret;
   Port *port = NULL;
   Bond *bond = NULL;
   int slaveNum;
   UplinkData *dummy;

   ASSERT(Portset_LockedExclHint(ps));
   ASSERT(portID);

   bond = ps->devImpl.data;
   ASSERT(bond);
   slaveNum = bond->totalSlaveCount;
   ASSERT(uplinkName);
   ASSERT(uplinkName[0] != '\0');
   ASSERT(bond->slave[slaveNum].uplinkName[0] == '\0');
   LOG(2, "slaveNum = %d", slaveNum);

   if (BondFindSlaveByName(bond, uplinkName)) {
      Warning("%s: already a member of %s", uplinkName, bond->devName);
      return VMK_FAILURE;
   }

   if (bond->slave[slaveNum].connected) {
      Warning("Uplink port %s slave[%d] is already connected",
              bond->slave[slaveNum].uplinkName, slaveNum);
      ASSERT(bond->slave[slaveNum].uplinkPort != ps->numPorts);
      ASSERT(bond->slave[slaveNum].uplinkName[0] != '\0');
      return VMK_FAILURE;
   }

   LOG(0, "Connecting portset %s to uplink %s (%p)", 
          ps->name, uplinkName, &bond->slave[slaveNum]);
   ret = Portset_ConnectPort(ps, &port);
   if (ret != VMK_OK) {
      Warning("cannot open port on %s", ps->name);
      return ret;
   }
 
   ASSERT(Portset_LockedExclHint(ps));
   ret = Uplink_Register(port->portID, uplinkName,
                         DEVICE_TYPE_PORTSET_BOND,
                         BondUplinkNotify, &dummy);
   ASSERT(Portset_LockedExclHint(ps));
   *portID = port->portID;

   if (ret == VMK_OK) {
      bond->slave[slaveNum].index = slaveNum;
      bond->slave[slaveNum].connected = TRUE;
      bond->slave[slaveNum].uplinkPort = port->portID;
      Portset_SetUplinkImplSz(port->ps, dummy->pktHdrSize);
      memcpy(bond->slave[slaveNum].uplinkName,
             uplinkName,
             sizeof(bond->slave[slaveNum].uplinkName));
      Port_Enable(port);
      bond->totalSlaveCount++;
      LOG(0, "Bond %s (%d) connected to uplink slave[%d] %s (0x%x)",
             ps->name, bond->totalSlaveCount, slaveNum, 
             uplinkName, bond->slave[slaveNum].uplinkPort);
      ret = VMK_OK;
   } else if (ret == VMK_NOT_FOUND) {
      bond->slave[slaveNum].index = slaveNum;
      bond->slave[slaveNum].connected = FALSE;
      bond->slave[slaveNum].uplinkPort = port->portID;
      memcpy(bond->slave[slaveNum].uplinkName,
             uplinkName,
             sizeof(bond->slave[slaveNum].uplinkName));
      bond->totalSlaveCount++;
      LOG(0, "Bond %s (%d) uplink slave[%d] %s (0x%x) registered, yet to come up",
             ps->name, bond->totalSlaveCount, slaveNum, 
             uplinkName, bond->slave[slaveNum].uplinkPort);
      ret = VMK_OK;
   } else {
      Portset_DisconnectPort(ps, port->portID);
      Warning("Bond %s (%d) failed to claim uplink device %s",
               ps->name, bond->totalSlaveCount, uplinkName);
      *portID = 0;
      ret = VMK_FAILURE;
   }
   return ret;
}


/*
 *-----------------------------------------------------------------------------
 *
 * BondUplinkDisconnect --
 *
 *     Disconnect an uplink (slave) from the bond
 *
 * Results:
 *     VMK_ReturnStatus 
 *
 * Side effects:
 *     None 
 *
 *-----------------------------------------------------------------------------
 */

static VMK_ReturnStatus
BondUplinkDisconnect(Portset *ps, char *uplinkName)
{
   Bond *bond = NULL;
   Slave *slave = NULL, *lastSlave = NULL;

   ASSERT(Portset_LockedExclHint(ps));

   bond = ps->devImpl.data;
   ASSERT(bond);
   LOG(3, "%s", bond->devName);

   if (bond->totalSlaveCount == 0) {
      Warning("%s: no slave to be removed", bond->devName);
      return VMK_FAILURE;
   }

   slave = BondFindSlaveByName(bond, uplinkName);
   if (!slave) {
      Warning("%s: cannot find slave %s", bond->devName, uplinkName);   
      return VMK_FAILURE;
   }

   lastSlave = &bond->slave[bond->totalSlaveCount - 1];
   bond->totalSlaveCount--;

   LOG(0, "Disconnecting %s slave %p %s uplink (%d left)", 
           ps->name, slave, uplinkName, bond->totalSlaveCount);
   Uplink_Unregister(slave->uplinkPort, slave->uplinkName);
   Portset_DisconnectPort(ps, slave->uplinkPort);

   if (slave != lastSlave) {
      LOG(0, "copy from slave[%d] to slave[%d]",  lastSlave->index, slave->index);
      slave->uplinkPort = lastSlave->uplinkPort;
      strcpy(slave->uplinkName, lastSlave->uplinkName);
      slave->connected = lastSlave->connected;
   }
   lastSlave->uplinkPort = ps->numPorts;
   lastSlave->uplinkName[0] = '\0';
   lastSlave->connected = FALSE;
   lastSlave->index = 0;

   LOG(2, "Disconnected %s from %s (%d left)",
          uplinkName, bond->devName, bond->totalSlaveCount);
   return VMK_OK;
}


/*
 *-----------------------------------------------------------------------------
 *
 * BondPortConnect --
 *
 *     Connect to a bond device. Does nothing.
 *
 * Results:
 *     VMK_ReturnStatus 
 *
 * Side effects:
 *     None 
 *
 *-----------------------------------------------------------------------------
 */

static VMK_ReturnStatus
BondPortConnect(Portset *ps, Port *port)
{
   LOG(1, "%s: portID = 0x%x, flags = 0x%x", ps->name, port->portID, port->flags);
   return VMK_OK;
}


/*
 *-----------------------------------------------------------------------------
 *
 * BondPortDisconnect --
 *
 *     Bond disconnect routine. Does nothing 
 *
 * Results:
 *     VMK_ReturnStatus 
 *
 * Side effects:
 *     None 
 *
 *-----------------------------------------------------------------------------
 */

static VMK_ReturnStatus
BondPortDisconnect(Portset *ps, Port *port)
{
   return VMK_OK;
}

static void
BondDisconnectAllSlaves(Portset *ps)
{
   Bond *bond = ps->devImpl.data;
   ASSERT(bond);
   ASSERT(SP_IsLocked(&portsetGlobalLock));

   while (bond->totalSlaveCount > 0) {
      ASSERT(bond->slave[0].uplinkPort != ps->numPorts);
      ASSERT(bond->slave[0].uplinkName[0] != '\0');
      // slave[0] will be changed if there are multiple slaves
      LOG(0, "%s (%d): unregisering %s 0x%x", 
             bond->devName, bond->totalSlaveCount,
             bond->slave[0].uplinkName, bond->slave[0].uplinkPort);
      BondUplinkDisconnect(ps, bond->slave[0].uplinkName);
   }
   ASSERT(bond->slave[0].uplinkName[0] == '\0');
   ASSERT(bond->totalSlaveCount == 0);
}



/*
 *-----------------------------------------------------------------------------
 *
 * BondCreateBondDevice --
 *
 *     Fully initialize bond and ps->devImpl.data
 *
 * Results:
 *     VMK_ReturnStatus  
 *
 * Side effects:
 *     None 
 *
 *-----------------------------------------------------------------------------
 */

static VMK_ReturnStatus
BondCreateBondDevice(Portset *ps)
{
   int i;
   ASSERT(ps);
   Bond *bond = (Bond *)Mem_Alloc(sizeof(Bond));

   ASSERT(SP_IsLocked(&portsetGlobalLock));
   if (!bond) {
      Warning("Bond %s could not be created", ps->name);
      return VMK_FAILURE;
   }
   memset(bond, 0, sizeof(Bond));
   LOG(1, "%s: bond = %p", ps->name, bond);

   ps->devImpl.data = bond;
   ps->devImpl.dispatch = BondRxPktList;
   ps->devImpl.deactivate = BondDeactivate;
   ps->devImpl.portConnect = BondPortConnect;
   ps->devImpl.portDisconnect = BondPortDisconnect;
   ps->devImpl.uplinkConnect = BondUplinkConnect;
   ps->devImpl.uplinkDisconnect = BondUplinkDisconnect;
   for (i=0; i<MAX_SLAVE_NUM; i++) {
      bond->slave[i].uplinkPort = ps->numPorts;
      bond->slave[i].uplinkName[0] = '\0';
      bond->slave[i].connected = FALSE;
   }
   bond->portset = ps;
   bond->inList = FALSE;
   memcpy(bond->devName, ps->name, sizeof(ps->name));
   return VMK_OK;
}

static void
BondAddToBondList(Bond *bond)
{
   ASSERT(bond);
   ASSERT(bond->inList == FALSE);
   List_InitElement(&bond->listLinks);
   List_Insert(&bond->listLinks, LIST_ATFRONT(&bondList.bondList));
   bond->inList = TRUE;
   LOG(0, "%s (%p): inserted in bond list", bond->devName, bond);
}

static void
BondRemoveFromBondList(Bond *bond)
{
   ASSERT(bond);
   if (bond->inList) {
      List_Remove(&bond->listLinks);
      LOG(0, "%s (%p): removed from bond list", bond->devName, bond);
      bond->inList = FALSE;
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * Bond_Activate --
 *
 *     Initialization routine as a top device (portset device).
 *     ("create bondX")
 *
 * Results:
 *     VMK_ReturnStatus
 *
 * Side effects:
 *     If !VMK_OK is returned, BondDeactivate will be called by the portset library
 *
 *-----------------------------------------------------------------------------
 */

VMK_ReturnStatus
Bond_Activate(Portset *ps)
{
   VMK_ReturnStatus ret;
   Bond *bond = NULL;
   ASSERT(SP_IsLocked(&portsetGlobalLock));
   ASSERT(Portset_LockedExclHint(ps));
   ASSERT(ps);
   ASSERT(ps->name[0] != '\0');

   LOG(0, "%s", ps->name);
   ASSERT(ps->devImpl.data == NULL);
   ret = BondCreateBondDevice(ps);
   if (ret != VMK_OK) {
      Warning("%s: failed to create an upper device", ps->name);
      return ret;
   }
   ASSERT(ps->devImpl.data);

   ret = BondCreateBottomDevice(ps);
   if (ret != VMK_OK) {
      Warning("%s: failed to create a bottom device", ps->name);
      return ret;
   }

   bond = (Bond *)ps->devImpl.data;
   ASSERT(bond);
   BondXConfigProcCreate(bond);
   LOG(0, "Bond %s activated, config entry created", ps->name);

   BondAddToBondList(bond);

   return VMK_OK;
}


/*
 *-----------------------------------------------------------------------------
 *
 * BondDeactivate --
 *
 *     Bond specific part of portset deactivation ("remove bondX")
 *
 * Results:
 *     VMK_ReturnStatus 
 *
 * Side effects:
 *     None 
 *
 *-----------------------------------------------------------------------------
 */

static VMK_ReturnStatus
BondDeactivate(Portset *ps)
{
   Bond *bond = ps->devImpl.data;

   LOG(0, "%s: %d slave(s)", bond->devName, bond->totalSlaveCount);

   ASSERT(SP_IsLocked(&portsetGlobalLock));
   ASSERT(Portset_LockedExclHint(ps)); 

   if (!bond) {
      LOG(0, "%s: no bond created", ps->name);
      return VMK_OK;
   }

   BondRemoveFromBondList(bond);
   BondXConfigProcRemove(bond);
   BondDisconnectAllSlaves(ps);
   BondDestroyBottomDevice(bond);

   ASSERT(bond->inList == FALSE);
   ASSERT(bond->configEntry == NULL);
   ASSERT(bond->totalSlaveCount == 0);

   Mem_Free(bond);
   ps->devImpl.data = NULL;

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------------
 *
 * Bond_ModInit --
 *
 *    Late initialization of the uplink layer. Called from Net_ModInit.
 *
 * Results:
 *    VMK_OK
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */
 
VMK_ReturnStatus
Bond_ModInit(void)
{
   LOG(0, "Loading nicteaming devices (%p)", &bondList.bondList);
   Portset_GlobalLock();
   List_Init(&bondList.bondList);
   Portset_GlobalUnlock();
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------------
 *
 *  Bond_ModCleanup --
 *
 *    Clean up the bond data structures. Called during network module
 *    cleanup.
 *
 *  Results:
 *    VMK_OK always.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

VMK_ReturnStatus
Bond_ModCleanup(void)
{
   LOG(0, "Unloading nicteaming devices");

   Portset_GlobalLock();

   while (!List_IsEmpty(&bondList.bondList)) {
      List_Links *e = List_First(&bondList.bondList);
      Bond *bond = (Bond *)e;
      Portset *ps = bond->portset;
      ASSERT(ps); 
      LOG(0, "deactivating %s", ps->name);
      Portset_LockExcl(ps);
      Portset_Deactivate(ps);
      Portset_UnlockExcl(ps);
   }

   Portset_GlobalUnlock();
   LOG(0, "Unloaded");
   return VMK_OK;
}
