/* **********************************************************
 * Copyright (C) 2004 VMware, Inc. All Rights Reserved
 * -- VMware Confidential.
 * **********************************************************/


/*
 * uplink.c --
 *
 *    Implements the uplink layer. When a particular portset wishes to
 *    assign a particular uplink port to a device (in this context, a bond or
 *    a physical NIC), it registers for notifications pertaining to that
 *    device. When the device comes up or goes down, the portset is notified.
 *
 *    The main data structure in this module is the uplinkTree. Whenever an
 *    unclaimed device comes up or an uplink port is registered, a node is 
 *    added as a child of the root node. Claiming a device has the effect of
 *    making the device node a child of the claimant node. Once a device has
 *    been claimed by a portset, the output function of that device is set as
 *    the last call in the IOChain of the uplink port of that portset. This
 *    allows the transmit path to go directly to the device. Uplink devices
 *    that aren't physical NICS may be children of other Uplink devices. This
 *    situation may occur, for example, in the case of a bond of bonds.
 *    Every uplink device may therefore be thought of as having two ends - the
 *    top end facing the VMKernel and a bottom end facing the physical NIC.
 *    The hierarchical structure helps in easy nic capability manangement. The
 *    uplink layer is also the point where both the vmkernel layer and the
 *    vmklinux layer meet. Consequently, information pertaining to either
 *    side can be easily retrieved at this layer.
 *
 */


#include "vmkernel.h"
#include "vmkernel_dist.h"
#include "vm_libc.h"
#include "memalloc.h"
#include "mod_loader.h"
#include "net_int.h"

#define LOGLEVEL_MODULE Uplink
#include "log.h"

static const int32 INVALID_MODULE_ID = -1;

typedef int (*Cmp)(const char *, const char *, int);

typedef struct UplinkNode {
   struct         UplinkNode *child;
   struct         UplinkNode *sibling;
   char           name[VMNIX_DEVICE_NAME_LENGTH];
   UplinkDevice  *uplinkDev;
   DeviceType     type; // type of the device
   Bool           visited;
} UplinkNode;

typedef struct UplinkTree {
   struct UplinkNode root;
   Cmp cmp;
} UplinkTree;


typedef struct UplinkCapability {
   uint32         chain; /* the level of the chain at which this capability attaches. */
   IOChainFn      fn;    /* software emulation of the capability */
   IOChainInsert  insert;
   IOChainRemove  remove;
   Bool           modifiesList; /* does this capability modify the packet list? */
} UplinkCapability;

#define MAX_CAPABILITIES 32

static UplinkCapability uplinkCap[MAX_CAPABILITIES];

static UplinkTree uplinkTree;

static PortsetName portsetName;

static VMK_ReturnStatus 
DummyCapability(struct Port *port, IOChainData data, struct PktList *list)
{
   LOG(0, "Dummy capability invoked");
   return VMK_OK;
}

static VMK_ReturnStatus UplinkCreateDevNode(const char *, void *, DeviceType,
                                            int32, Net_Functions *, size_t,
                                            size_t, UplinkDevice **, UplinkNode **);

/*
 *----------------------------------------------------------------------------
 *
 * UplinkTree_Init --
 *
 *    Initialize the uplink tree data structures.
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
UplinkTree_Init(UplinkTree *tree, Cmp cmp)
{
   ASSERT(tree);
   tree->root.child = NULL;
   tree->root.sibling = NULL;
   tree->root.uplinkDev = NULL;
   tree->cmp = cmp;
   uplinkCap[31].fn = DummyCapability;
   uplinkCap[31].modifiesList = FALSE;
   uplinkCap[31].chain = IO_CHAIN_RANK_TERMINAL - 1;
}


/*
 *----------------------------------------------------------------------------
 *
 * UplinkTreeDoCleanup --
 *
 *    Cleanup the uplink tree data structures. Does a recursive cleanup.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

void UplinkTreeDoCleanup(UplinkNode *root)
{
   if (root) {
      UplinkNode *cur = root->child;
      while (cur) {
         UplinkNode *sibling = cur->sibling;
         UplinkTreeDoCleanup(cur);
         cur = sibling;
      }
      if (root->uplinkDev) {
         Mem_Free(root->uplinkDev);
      }
      Mem_Free(root);
   }
}


/*
 *----------------------------------------------------------------------------
 *
 * UplinkTree_Cleanup --
 *
 *    External wrapper for cleaning up the uplink tree.
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
UplinkTree_Cleanup(UplinkTree *tree)
{
   ASSERT(tree);
   UplinkTreeDoCleanup(tree->root.child);
}


/*
 *----------------------------------------------------------------------------
 *
 * UplinkTreeDoCheckCycle --
 *
 *    Detects for cycles in the subtree under the given node.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */
 
UplinkNode *
UplinkTreeDoCheckCycle(UplinkNode *root)
{
   UplinkNode *cur;
   UplinkNode *ret = NULL;
   if (root) {
      if (root->visited == FALSE) {
         root->visited = TRUE;
         cur = root->child;
         while (cur) {
            ret = UplinkTreeDoCheckCycle(cur);
            if (ret) {
               Log("Node %s is part of cycle", cur->name);
               break;
            }
            cur = cur->sibling;
         }
      } else {
         Log("Cycle detected at node %s", root->name);
         ret = root;
      }
      root->visited = FALSE;
   }
   return ret;
}


/*
 *----------------------------------------------------------------------------
 *
 * UplinkTreeCheckCycle --
 *
 *    Checks if cycles exist in the given tree.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

UplinkNode *
UplinkTreeCheckCycle(UplinkTree *tree)
{
   ASSERT(tree);
   // TODO: implement breadth wise cycle check
   return UplinkTreeDoCheckCycle(&tree->root);
   
}


/*
 *----------------------------------------------------------------------------
 *
 * UplinkTree_AddChild --
 *
 *    Add the specified child to the given parent.
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
UplinkTree_AddChild(UplinkTree *tree, UplinkNode *parent, UplinkNode *child)
{
   UplinkNode *cycleNode, *siblingNode;
   ASSERT(parent);
   ASSERT(child);
   siblingNode = child->sibling;
   child->sibling = parent->child;
   parent->child = child;
   // check if a cycle was introduced
   if ((cycleNode = UplinkTreeCheckCycle(tree)) != NULL) {
      LOG(0, "Cycle detected at node %p", cycleNode);
      // roll back
      parent->child = child->sibling;
      child->sibling = siblingNode;
   }
}


/*
 *----------------------------------------------------------------------------
 *
 * UplinkTreeDoRemoveChild --
 *
 *    Helper function that recursively traverses the (sub)tree under root and
 *    removes the specified node.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

Bool
UplinkTreeDoRemoveChild(UplinkNode *root, UplinkNode *node,
                        UplinkNode **prev)
{
   if (root) {
      UplinkNode *cur = root->child;
      if (node == root) {
         *prev = root->sibling;
         return TRUE;
      }
      prev = &root->child;
      while (cur) {
         if (UplinkTreeDoRemoveChild(cur, node, prev) == TRUE) {
            return TRUE;
         }
         prev = &cur->sibling;
         cur = cur->sibling;
      }
   }
   return FALSE;
}


 
/*
 *----------------------------------------------------------------------------
 *
 * UplinkTree_RemoveChild --
 *
 *    Removes the specified child from the tree.
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
UplinkTree_RemoveChild(UplinkTree *tree, UplinkNode *node)
{
   ASSERT(tree);
   ASSERT(node);
   ASSERT(node != &tree->root); // root cannot be removed
   UplinkTreeDoRemoveChild(&tree->root, node, NULL);
}



/*
 *----------------------------------------------------------------------------
 *
 * UplinkTreeDoDFS --
 *
 *    Does a recursive DFS of the tree.
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
UplinkTreeDoDFS(UplinkNode *root, const char *name, DeviceType type,
                UplinkNode **dev, Cmp cmp)
{
   ASSERT(name);
   ASSERT(dev);
   if (root) {
      UplinkNode *cur = root->child;
      if (root->type & type && !cmp(root->name, name, sizeof root->name)) {
         *dev = root;
         return;
      }
      while (cur) {
         UplinkTreeDoDFS(cur, name, type, dev, cmp);
         if (*dev) {
            return;
         }
         cur = cur->sibling;
      }
   }
   *dev = NULL;
}

void
UplinkTree_FindPortset(UplinkTree *tree, const char *name, UplinkNode **dev)
{
   ASSERT(tree);
   ASSERT(dev);
   // if this is triggered, something's wrong with UplinkTree_AddChild
   ASSERT(!UplinkTreeCheckCycle(tree));
   UplinkTreeDoDFS(&tree->root, name,
                   DEVICE_TYPE_PORTSET_TOPLEVEL|DEVICE_TYPE_PORTSET_BOND,
                   dev,
                   tree->cmp);
}


void
UplinkTree_FindDevice(UplinkTree *tree, const char *name, UplinkNode **dev)
{
   ASSERT(tree);
   ASSERT(dev);
   // if this is triggered, something's wrong with UplinkTree_AddChild
   ASSERT(!UplinkTreeCheckCycle(tree));
   UplinkTreeDoDFS(&tree->root, name, 
                   DEVICE_TYPE_DEVICE_LEAF|DEVICE_TYPE_DEVICE_BOND,
                   dev,
                   tree->cmp);
}


void
UplinkTree_FindToplevelPortset(UplinkTree *tree, const char *name, UplinkNode **dev)
{
   ASSERT(tree);
   ASSERT(dev);
   // if this is triggered, something's wrong with UplinkTree_AddChild
   ASSERT(!UplinkTreeCheckCycle(tree));
   UplinkTreeDoDFS(&tree->root, name,
                   DEVICE_TYPE_PORTSET_TOPLEVEL,
                   dev,
                   tree->cmp);
}


void
UplinkTree_FindLeafDevice(UplinkTree *tree, const char *name, UplinkNode **dev)
{
   ASSERT(tree);
   ASSERT(dev);
   // if this is triggered, something's wrong with UplinkTree_AddChild
   ASSERT(!UplinkTreeCheckCycle(tree));
   UplinkTreeDoDFS(&tree->root, name, 
                   DEVICE_TYPE_DEVICE_LEAF,
                   dev,
                   tree->cmp);
}



void
UplinkTree_FindBondPortset(UplinkTree *tree, const char *name, UplinkNode **dev)
{
   ASSERT(tree);
   ASSERT(dev);
   // if this is triggered, something's wrong with UplinkTree_AddChild
   ASSERT(!UplinkTreeCheckCycle(tree));
   UplinkTreeDoDFS(&tree->root, name,
                   DEVICE_TYPE_PORTSET_BOND,
                   dev,
                   tree->cmp);
}


void
UplinkTree_FindBondDevice(UplinkTree *tree, const char *name, UplinkNode **dev)
{
   ASSERT(tree);
   ASSERT(dev);
   // if this is triggered, something's wrong with UplinkTree_AddChild
   ASSERT(!UplinkTreeCheckCycle(tree));
   UplinkTreeDoDFS(&tree->root, name,
                   DEVICE_TYPE_DEVICE_BOND,
                   dev,
                   tree->cmp);
}


/*
 *----------------------------------------------------------------------------
 *
 * UplinkTree_Find --
 *
 *    Finds the node in the tree corresponding to the specified name.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */
#if 0
void
UplinkTree_Find(UplinkTree *tree, const char *name, UplinkNode **dev)
{
   ASSERT(tree);
   ASSERT(dev);
   // if this is triggered, something's wrong with UplinkTree_AddChild
   ASSERT(!UplinkTreeCheckCycle(tree));
   UplinkTreeDoDFS(&tree->root, name, dev, tree->cmp);
}
#endif



static UplinkTree uplinkTree;
static Proc_Entry uplinkProcEntry;

/*
 *----------------------------------------------------------------------------
 *
 *  UplinkOutput --
 *
 *    An IOChainFn signatured wrapper for the device startTx function.
 *
 *  Results:
 *    VMK_OK always.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static VMK_ReturnStatus
UplinkOutput(struct Port *port, IOChainData data, struct PktList *pktList)
{
   UplinkDevice *dev = (UplinkDevice *)data;
   ASSERT(dev);
   ASSERT(dev->functions);
   ASSERT(dev->netDevice);
   ASSERT(dev->functions->startTx);
   return dev->functions->startTx(dev->netDevice, pktList);
}


/*
 *----------------------------------------------------------------------------
 *
 *  Uplink_ModEarlyInit --
 *
 *    Initialize the uplink data structures. Called during initialization of
 *    the network module.
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
Uplink_ModEarlyInit(void)
{
   UplinkTree_Init(&uplinkTree, strncmp);
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------------
 *
 * UplinkProcRead --
 *
 *    Uplink proc read handler. Dumps the uplink table to the proc node.
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
UplinkProcRead(Proc_Entry *entry, char *page, int *len)
{
#ifdef ESX3_NETWORKING_NOT_DONE_YET
   *len = 0;
#else // ESX3_NETWORKING_NOT_DONE_YET
   List_Links *e;
   UplinkDevice *dev;

   *len = 0;
   Proc_Printf(page, len,"%-16s %-16s %10s\n",
               "Uplink Port", "Device Name", "Flags");
   Portset_GlobalLock();

   LIST_FORALL(&uplinkTable.uplinks, e) {
      dev = (UplinkDevice *)e;
      Proc_Printf(page, len, "0x%-16x %-16s %s%s%s%s\n", dev->uplinkPort, dev->devName,
                  (dev->flags & DEVICE_AVAILABLE)? "A":"NA",
                  (dev->flags & DEVICE_PRESENT)? ", P":", NP",
                  (dev->flags & DEVICE_OPENED)? ", O":", NO",
                  (dev->flags & DEVICE_EVENT_NOTIFIED)?", NS":", NNS");
   }

   Portset_GlobalUnlock();
   Proc_Printf(page, len, 
              "Legend:\n"
              "\tA:  Available NA:  Not available\n"
              "\tP:  Present   NP:  Absent\n"
              "\tO:  Opened    NO:  Closed\n"
              "\tNS: Notified  NNS: Not notified\n");
#endif // ESX3_NETWORKING_NOT_DONE_YET
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------------
 *
 * UplinkProcCreate --
 *
 *    Create a proc node for the uplink table.
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
UplinkProcCreate(void)
{
   Proc_InitEntry(&uplinkProcEntry);
   uplinkProcEntry.parent = ProcNet_GetRootNode();
   uplinkProcEntry.read = UplinkProcRead;
   uplinkProcEntry.write = NULL;
   uplinkProcEntry.private = NULL;
   ProcNet_Register(&uplinkProcEntry, "uplink", FALSE);
}


/*
 *----------------------------------------------------------------------------
 *
 * Uplink_ModInit --
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
Uplink_ModInit(void)
{
   UplinkProcCreate();
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------------
 *
 *  Uplink_ModCleanup --
 *
 *    Clean up the uplink data structures. Called during network module
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
Uplink_ModCleanup(void)
{
#ifdef ESX3_CLEANUP_EVERYTHING
   UplinkTree_Cleanup(&uplinkTree);
#endif
   ProcNet_Remove(&uplinkProcEntry);

   return VMK_OK;
}



/*
 *----------------------------------------------------------------------------
 *
 *  UplinkConnectPortToDevice --
 *
 *    Set the 'impl data' in the port. Also registers the UplinkOutput function
 *    with the port's output iochain.
 *
 *  Results:
 *    VMK_ReturnStatus indicating the outcome.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static VMK_ReturnStatus
UplinkConnectPortToDevice(Port *port, UplinkDevice *dev)
{
   VMK_ReturnStatus ret = VMK_OK;
   ASSERT(dev);
   ASSERT(!(dev->flags & DEVICE_AVAILABLE));
   ASSERT(port);
   if (dev->flags & DEVICE_PRESENT &&
       dev->flags & DEVICE_OPENED) {
      LOG(0, "Inserting UplinkOutput into port 0x%x 's IOChain", port->portID);
      ret = IOChain_InsertCall(&port->outputChain, IO_CHAIN_RANK_TERMINAL,
                               UplinkOutput, NULL, NULL, (IOChainData)dev, TRUE, NULL);
      if (ret == VMK_OK) {
         int i;
         dev->flags |= DEVICE_EVENT_NOTIFIED;
         Port_InitImpl(port);
         port->impl.data = dev->netDevice;
         port->ps->uplinkDev = dev;
         // remove sw capabilities where hw capabilities may exist
         for (i = 0; i < MAX_CAPABILITIES; i++) {
            int j = 1 << i;
            if ((dev->hwCap & j) && (dev->swCap & j) && uplinkCap[i].fn) {
               Log("Removing call %p, index = 0x%x from port 0x%x", 
                   uplinkCap[i].fn, i, port->portID);
               IOChain_RemoveCall(&port->outputChain,  uplinkCap[i].fn);
            }
         }
         dev->swCap ^= dev->hwCap;
      } else {
         LOG(0, "IOChain insert failed for port 0x%x", port->portID);
      }
   } else {
      LOG(0, "Device %s is either not present or opened", dev->devName);
      ret = VMK_FAILURE;
   }
   return ret;
}


/*
 *----------------------------------------------------------------------------
 *
 * UplinkConnectAndNotify --
 *
 *    Connect the specified port to the specified device and, if required,
 *    notify the portset.
 *
 * Results:
 *    VMK_OK on success. VMK_FAILURE otherwise.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE VMK_ReturnStatus
UplinkConnectAndNotify(PortID uplinkPort, UplinkDevice *dev)
{
   Port *port;
   VMK_ReturnStatus ret = VMK_OK;
   ASSERT(dev);
   ASSERT(dev->uplinkPort == uplinkPort);
   port = Portset_GetPortExcl(uplinkPort);
   if (port != NULL) {
      ret = UplinkConnectPortToDevice(port, dev);
      if (ret == VMK_OK) {
         if (dev->notifyFn) {
            dev->notifyFn(dev->uplinkPort, &dev->uplinkData, UPLINK_UP);
         }
      } else {
         LOG(0, "IOChain insert failed for port 0x%x", uplinkPort);
      }
      Portset_ReleasePortExcl(port);
   } else {
      LOG(0, "Failed to get port associated with uplink port 0x%x", uplinkPort);
      ret = VMK_FAILURE;
   }
   return ret;
}


/*
 *----------------------------------------------------------------------------
 *
 *  UplinkDisconnectPortFromDevice --
 *
 *    Disconnect the specified port from the device. Removes implementation data
 *    and UplinkOutput from the port's data structures.
 *
 *  Results:
 *    None.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static void
UplinkDisconnectPortFromDevice(Port *port, UplinkDevice *dev)
{
   ASSERT(dev);
   ASSERT(port);
   if (dev->flags & DEVICE_EVENT_NOTIFIED) {
      dev->flags &= ~DEVICE_EVENT_NOTIFIED;
      LOG(0, "Removing IOChain links from port 0x%x, device %s", port->portID,
          dev->devName);
      IOChain_RemoveCall(&port->outputChain, UplinkOutput);
      Port_InitImpl(port);
      port->ps->uplinkDev = NULL;
   }
   return;
}



/*
 *----------------------------------------------------------------------------
 *
 * UplinkDisconnectAndNotify --
 *
 *    Disconnect the specified port from the specified device and generate a
 *    notification if necessary.
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
UplinkDisconnectAndNotify(PortID uplinkPort, UplinkDevice *dev)
{
   Port *port;
   ASSERT(dev);
   ASSERT(dev->uplinkPort == uplinkPort);
   port = Portset_GetPortExcl(uplinkPort);
   if (port != NULL) {
      Bool sendNotification = FALSE;
      if (dev->flags & DEVICE_EVENT_NOTIFIED) {
         sendNotification = TRUE;
      }
      UplinkDisconnectPortFromDevice(port, dev);
      if (sendNotification && dev->notifyFn) {
         LOG(0, "%s going down. Notifying 0x%x", dev->devName, uplinkPort);
         dev->notifyFn(dev->uplinkPort, &dev->uplinkData, UPLINK_DOWN);;
      }
      Portset_ReleasePortExcl(port);
   } else {
      LOG(0, "Failed to get port associated with uplink port 0x%x", uplinkPort);
   }
}


/*
 *----------------------------------------------------------------------------
 *
 *  Uplink_Register --
 *
 *    Associate an uplink port with a particular device. When the device comes
 *    up or goes down, the portset will be notified. In addition, a portset
 *    notification function is also registered if the portset needs to
 *    be notified on device events. The etherSwitch, for example, may need to
 *    update some private fields.
 *
 *    Caller shouldn't hold the lock for the portset that contains the uplink
 *    port to avoid circular lock dependencies. The caller should hold an
 *    exclusive lock for its own data structures.
 *
 *  Results:
 *    VMK_OK if the device is present and the register call was successful.
 *    VMK_NOT_FOUND if the register was successful but the device isn't
 *    present. VMK_FAILURE on error.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */


VMK_ReturnStatus
Uplink_Register(PortID uplinkPort, char *devName, DeviceType portType,
                NotifyFn *notifyFn, UplinkData **uplinkData)
{
   UplinkNode *devNode, *portsetNode;
   UplinkDevice *dev = NULL;
   VMK_ReturnStatus ret = VMK_OK;
   Portset_GetNameFromPortID(uplinkPort, portsetName, sizeof portsetName);

   ASSERT(uplinkData);
   *uplinkData = NULL;

   UplinkTree_FindDevice(&uplinkTree, devName, &devNode);
   if (devNode) {
      LOG(0, "Device %s found", devName);
      dev = devNode->uplinkDev;
      if (dev->flags & DEVICE_AVAILABLE) {
         UplinkNode *root = uplinkTree.root.child;
         Bool found = FALSE;
         while (root) {
            if (!strncmp(root->name, devName, sizeof (root->name))) {
               found = TRUE;
               break;
            }
            root = root->sibling;
         }
         ASSERT(found == TRUE);
      } else if (dev->uplinkPort == uplinkPort) {
         LOG(0, "Device %s already claimed by this port(0x%x)", dev->devName,
             uplinkPort);
         //XXX: ASSERT that device is a child of portset
         *uplinkData = &dev->uplinkData;
         return VMK_OK;
      } else {
         Portset_GetNameFromPortID(dev->uplinkPort, portsetName,
                                   sizeof portsetName);
         // device already claimed by somebody else
         LOG(0, "Device %s already claimed by %s", dev->devName, portsetName);
         return VMK_NO_RESOURCES;
      }
   } else { // create a device node and add it to root.
      ret = UplinkCreateDevNode(devName, NULL, DEVICE_TYPE_DEVICE_UNKNOWN,
                                INVALID_MODULE_ID, NULL,  0, 0, &dev, &devNode);
      if (ret == VMK_OK) {
         ASSERT(dev);
         dev->flags = DEVICE_AVAILABLE; // obviously, the device isn't yet there
      } else {
         return ret;
      }
   }

   if (portsetName[0] != '\0') {
      if (portType == DEVICE_TYPE_PORTSET_BOND) {
         UplinkTree_FindBondPortset(&uplinkTree, portsetName, &portsetNode);
      } else {
         ASSERT(portType == DEVICE_TYPE_PORTSET_TOPLEVEL);
         UplinkTree_FindToplevelPortset(&uplinkTree, portsetName, &portsetNode);
      }

      if (portsetNode) {
         UplinkNode *cur = portsetNode->child;
         while (cur) {
            if (cur->uplinkDev->uplinkPort == uplinkPort) {
               LOG(0, "Uplink port (0x%x) has already claimed device %s",
                   uplinkPort, cur->uplinkDev->devName);
               return VMK_FAILURE;
            }
            cur = cur->sibling;
         }
      } else {
         portsetNode = Mem_Alloc(sizeof *portsetNode);
         if (portsetNode) {
            memset(portsetNode, 0, sizeof *portsetNode);
            strncpy(portsetNode->name, portsetName, sizeof portsetNode->name);
            portsetNode->type = portType;
            UplinkTree_AddChild(&uplinkTree, &uplinkTree.root, portsetNode);
         } else {
            return VMK_FAILURE;
         }
      }
      UplinkTree_RemoveChild(&uplinkTree, devNode);
      UplinkTree_AddChild(&uplinkTree, portsetNode, devNode);
      dev->uplinkPort = uplinkPort;
      dev->notifyFn = notifyFn;
      dev->flags &= ~DEVICE_AVAILABLE;
      if (dev->flags & DEVICE_PRESENT) {
         Port *port;
         Portset_GetLockedPort(uplinkPort, &port);
         Log("Connecting Port 0x%x to device %s", port->portID, dev->devName);
         if (port) {
            ret = UplinkConnectPortToDevice(port, dev);
            if (ret == VMK_OK) {
               *uplinkData = &dev->uplinkData;
            }
         } else {
            LOG(0, "Couldn't find port associated with uplinkPort 0x%x",
                uplinkPort);
            ret = VMK_FAILURE;
         }
      } else {
         Log("Device %s not found", devName);
         ret = VMK_NOT_FOUND;
      }
   } else {
      Log("Couldn't find portset name for uplink port 0x%x", uplinkPort);
      ret = VMK_FAILURE;
   }
   return ret;
}


/*
 *----------------------------------------------------------------------------
 *
 * UplinkDoUnregister --
 *
 *    Helper function for disconnecting the device from the specified port.
 *
 * Results:
 *    VMK_OK on success. VMK_FAILURE on failure.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE VMK_ReturnStatus
UplinkDoUnregister(UplinkNode *portsetNode, PortID uplinkPort, const char *devName)
{
   UplinkNode *cur = portsetNode->child;
   Bool found = FALSE;
   while (cur) {
      if (!strncmp(cur->name, devName, sizeof cur->name)) {
         UplinkDevice *dev = cur->uplinkDev;
         found = TRUE;
         if (!(dev->flags & DEVICE_AVAILABLE)) {
            if (dev->uplinkPort == uplinkPort) {
               if (dev->flags & DEVICE_PRESENT) {
                  Port *port;
                  LOG(0, "Disconnecting port 0x%x from device %s",
                      uplinkPort, dev->devName);
                  Portset_GetLockedPort(uplinkPort, &port);
                  if (port != NULL) {
                     ASSERT(port->portID == uplinkPort);
                     UplinkDisconnectPortFromDevice(port, dev);
                  }
               }
               dev->flags |= DEVICE_AVAILABLE;
               dev->uplinkPort = 0;
               dev->notifyFn = NULL;
            } else {
               LOG(0, "Device is associated with port 0x%x. Specified port = "
                   "0x%x", dev->uplinkPort, uplinkPort);
               return VMK_FAILURE;
            }
         } else {
            LOG(1, "Device %s has already been relinquished", dev->devName);
            return VMK_FAILURE;
         }

         UplinkTreeDoRemoveChild(portsetNode, cur, &portsetNode->child);
         UplinkTree_AddChild(&uplinkTree, &uplinkTree.root, cur);
         break;
      }
      cur = cur->sibling;
   }
   if (found == FALSE) {
      LOG(0, "Device %s isn't associated with uplink port 0x%x", devName, 
          uplinkPort);
      return VMK_FAILURE;
   }
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------------
 *
 *  Uplink_Unregister --
 *
 *    Unregister an uplink port. Breaks the association between the nic and the
 *    uplink port. The portset with which the uplink port is associated
 *    will no longer receive nic state change notifications. This function
 *    rolls back whatever Uplink_Register might have done and is also used for
 *    backing out in the case of an error.
 *
 *  Results:
 *    VMK_OK on success. VMK_FAILURE otherwise.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

VMK_ReturnStatus
Uplink_Unregister(PortID uplinkPort, char *devName)
{
   UplinkNode *portsetNode;
   Portset_GetNameFromPortID(uplinkPort, portsetName, sizeof portsetName);
   ASSERT(devName);

   if (portsetName[0] != '\0') {
      UplinkTree_FindPortset(&uplinkTree, portsetName, &portsetNode);
      if (portsetNode) {
         return UplinkDoUnregister(portsetNode, uplinkPort, devName);
      } else {
         LOG(0, "Portset %s doesn't exist in the uplink tree", portsetName);
         return VMK_FAILURE;
      }
   } else {
      LOG(0, "No portset associated with uplink port 0x%x", uplinkPort);
      return VMK_FAILURE;
   }
}



/*
 *----------------------------------------------------------------------------
 *
 *  UplinkSetDeviceParams --
 *
 *    Set the parameters for the specified device.
 *
 *  Results:
 *    VMK_OK always.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static VMK_ReturnStatus
UplinkSetDeviceParams(UplinkDevice *dev, void *netDevice, int32 moduleID,
                      Net_Functions *functions, size_t pktHdrSize,
                      size_t maxSGLength)
{
   ASSERT(dev);
   VMK_ReturnStatus ret = VMK_OK;
   dev->netDevice = netDevice;
   dev->moduleID = moduleID;
   dev->functions = functions;
   dev->uplinkData.pktHdrSize = pktHdrSize;
   dev->uplinkData.maxSGLength = maxSGLength;
   return ret;
}



/*
 *----------------------------------------------------------------------------
 *
 *  UplinkCreateDevice --
 *
 *    Creates an entry in the uplink table for the specified device. The device
 *    is marked as available and present.
 *
 *  Results:
 *    VMK_OK on success. Other VMK_ReturnStatus on failure.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static VMK_ReturnStatus
UplinkCreateDevice(const char *devName, void *device, int32 moduleID,
                   Net_Functions *functions, size_t pktHdrSize,
                   size_t maxSGLength, UplinkDevice **uplinkDev)
{
   VMK_ReturnStatus ret = VMK_OK;
   UplinkDevice *dev = Mem_Alloc(sizeof(UplinkDevice));
   ASSERT(uplinkDev);
   *uplinkDev = NULL;
   if (dev) {
      LOG(0, "Creating device %s", devName);
      memset(dev, 0, sizeof *dev);
      memcpy(dev->devName, devName, VMNIX_DEVICE_NAME_LENGTH);
      dev->flags = DEVICE_AVAILABLE | DEVICE_PRESENT;
      dev->uplinkPort = 0;
      dev->notifyFn = NULL;
      dev->hwCap = 0;
      dev->swCap = 0;
      UplinkSetDeviceParams(dev, device, moduleID, functions, pktHdrSize,
                            maxSGLength);
      *uplinkDev = dev;
   } else {
      LOG(0, "Couldn't allocate memory for uplink entry");
      ret = VMK_NO_RESOURCES;
   }
   return ret;
}


static VMK_ReturnStatus
UplinkCreateDevNode(const char *devName, void *device, DeviceType type,
                    int32 moduleID, Net_Functions *functions, size_t pktHdrSize,
                    size_t maxSGLength, UplinkDevice **uplinkDev,
                    UplinkNode **uplinkNode)
{
   VMK_ReturnStatus ret = VMK_OK;
   UplinkNode *devNode = Mem_Alloc(sizeof *devNode);
   *uplinkDev = NULL;
   *uplinkNode = NULL;

   if (devNode) {
      memset(devNode, 0, sizeof *devNode);
      memcpy(devNode->name, devName, VMNIX_DEVICE_NAME_LENGTH);
      ret = UplinkCreateDevice(devName, device, moduleID, functions, pktHdrSize,
                               maxSGLength, &devNode->uplinkDev);
      if (ret == VMK_OK) {
         devNode->type = type;
         UplinkTree_AddChild(&uplinkTree, &uplinkTree.root, devNode);
         *uplinkDev = devNode->uplinkDev;
         *uplinkNode = devNode;
      } else {
         Mem_Free(devNode);
      }
   } else {
      ret = VMK_NO_RESOURCES;
   }
   return ret;
}

/*
 *----------------------------------------------------------------------------
 *
 *  UplinkDoDeviceDisconnected --
 *
 *    Breaks the association between the port and the device if any such
 *    exists and also resets all the device specific fields. However, the
 *    port is still deemed to have a claim on the device and should the device
 *    come up again, the fields would be reinitialized and the notify to which
 *    the uplink port belongs would be notified of the event.
 *
 *  Results:
 *    None.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static void
UplinkDoDeviceDisconnected(UplinkDevice *dev)
{
   ASSERT(dev);
   if (!(dev->flags & DEVICE_AVAILABLE)) {
      LOG(0, "Breaking association between port 0x%x and device %s",
          dev->uplinkPort, dev->devName);
      UplinkDisconnectAndNotify(dev->uplinkPort, dev);
   }

   // XXX: Need to free softirq tx and rx queues when they are incorporated.
   if (dev->flags & DEVICE_OPENED) {
      LOG(0, "Closing device %s (%p)", dev->devName, dev->functions->close);
      dev->functions->close(dev->netDevice);
      dev->flags &= ~DEVICE_OPENED;
   }
   dev->flags &= ~DEVICE_PRESENT;
   dev->netDevice = NULL;
   dev->uplinkData.intrHandler = NULL;
   dev->uplinkData.intrHandlerVector = INVALID_VECTOR;
   dev->uplinkData.pktHdrSize = 0;
   dev->uplinkData.maxSGLength = 0;
}

void
Uplink_DoDeviceDisconnected(UplinkDevice *dev)
{
   LOG(0, "cannot get port excl on %s", dev->devName);
   UplinkDoDeviceDisconnected(dev);
}


/*
 *----------------------------------------------------------------------------
 *
 * Uplink_DeviceDisconnected --
 *
 *    Disconnect the device with the given name. This function is used by
 *    logical devices to indicate that they may no longer be used.
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
Uplink_DeviceDisconnected(char *devName)
{
   UplinkNode *devNode = NULL;
   if (devName) {
      Portset_GlobalLock();
      UplinkTree_FindDevice(&uplinkTree, devName, &devNode);
      if (devNode) {
         UplinkDoDeviceDisconnected(devNode->uplinkDev);
      } else {
         LOG(0, "Device %s couldn't be found", devName);
      }
      Portset_GlobalUnlock();
   } else {
      LOG(0, "Cannot remove nameless device");
   }
}

/*
 *----------------------------------------------------------------------------
 *
 *  Uplink_PCIDeviceClose --
 *
 *    PCI device notification handler. Takes care of cleaning up device
 *    specific data.
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
Uplink_PCIDeviceClose(int moduleID, PCI_Device *pcidev)
{
   UplinkNode *devNode = NULL;
   Portset_GlobalLock();
   if (pcidev != NULL) {
      UplinkTree_FindDevice(&uplinkTree, pcidev->name, &devNode);
      if (devNode) {
         LOG(0, "Device close notification for %s", pcidev->name);
         UplinkDoDeviceDisconnected(devNode->uplinkDev);
      }
   }
   Portset_GlobalUnlock();
}


/*
 *----------------------------------------------------------------------------
 *
 * Uplink_SetDeviceConnected --
 *
 *    Set the uplink data structures indicating device connection.
 *
 *  Results:
 *    VMK_OK on success. Error code on failure.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */
 
VMK_ReturnStatus
Uplink_SetDeviceConnected(UplinkConnectArgs *args, void **uplinkDev)
{

   UplinkNode *devNode = NULL;
   UplinkDevice *dev = NULL;
   VMK_ReturnStatus ret = VMK_OK;
   int32 moduleID = INVALID_MODULE_ID;

   /* This function must be called with the global lock held */
   ASSERT(Portset_GlobalLockedHint());
   ASSERT(args);
   ASSERT(args->uplinkImpl);
   ASSERT(args->functions);

   ASSERT(uplinkDev);
   *uplinkDev = NULL;

   if (args->type == DEVICE_TYPE_DEVICE_LEAF) {
      if (args->moduleID > 0) {
         moduleID = args->moduleID;
      } else {
         Warning("Leaf device %s doesn't have moduleID set", args->devName);
      }
   } else {
      ASSERT(args->type == DEVICE_TYPE_DEVICE_BOND);
   }

   UplinkTree_FindDevice(&uplinkTree, args->devName, &devNode);

   if (!devNode) {
      LOG(0, "Creating an entry for %s in the uplink table", args->devName);

      ret = UplinkCreateDevNode(args->devName, args->uplinkImpl, args->type,
                                moduleID, args->functions,  args->pktHdrSize,
                               args->maxSGLength, &dev, &devNode);
   } else {
      dev = devNode->uplinkDev;

      if (!dev) {
         LOG(0, "Creating device %s", args->devName);
         UplinkCreateDevice(args->devName, args->uplinkImpl, moduleID,
                            args->functions, args->pktHdrSize,
                            args->maxSGLength, &devNode->uplinkDev);
         dev = devNode->uplinkDev;
         if (dev) {
            dev->flags = DEVICE_AVAILABLE;
            devNode->type = args->type;
         } else {
            LOG(0, "Couldn't allocate memory for uplink device %s", args->devName);
            return VMK_FAILURE;
         }
      }
      if(devNode->type == DEVICE_TYPE_DEVICE_UNKNOWN) {
         devNode->type = args->type;
      }

      if (dev->flags & DEVICE_PRESENT) {
         LOG(0, "Uplink Device %s is already present", dev->devName);
         return VMK_FAILURE;
      } else {
         ASSERT(devNode->type == args->type);
         dev->flags |= DEVICE_PRESENT;
         LOG(0, "Entry found for device in the uplink table. Setting "
             "parameters");
         ret = UplinkSetDeviceParams(dev, args->uplinkImpl, moduleID,
                                     args->functions,args->pktHdrSize,
                                     args->maxSGLength);
      }
   }

   *uplinkDev = dev;
   if (ret != VMK_OK && dev) {
      LOG(0, "Device connect failed.");
      UplinkDoDeviceDisconnected(dev);
   }

   return ret;
}


/*
 *----------------------------------------------------------------------------
 *
 *  Uplink_DeviceConnected --
 *
 *    Called from the depths to indicate that the specified device has been
 *    connected. Takes care of initializing device specific data.
 *
 *  Results:
 *    VMK_ReturnStatus indicating the outcome.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

VMK_ReturnStatus
Uplink_DeviceConnected(const char *devName, void *device, int32 moduleID,
                       Net_Functions *functions, size_t pktHdrSize,
                       size_t maxSGLength, void **uplinkDev)
{
   VMK_ReturnStatus ret = VMK_OK;

   ASSERT(devName);
   ASSERT(device);
   ASSERT(functions);

   if (devName) {
      UplinkConnectArgs args;

      Portset_GlobalLock();

      memset(&args, 0, sizeof(args));

      args.uplinkImpl  = device;
      args.moduleID    = moduleID;
      args.functions   = functions;
      args.pktHdrSize  = pktHdrSize;
      args.maxSGLength = maxSGLength;
      args.type        = DEVICE_TYPE_DEVICE_LEAF;
      strncpy(args.devName, devName, sizeof args.devName);

      ret = Uplink_SetDeviceConnected(&args, uplinkDev);

      Portset_GlobalUnlock();
   } else {
      LOG(0, "Device has no name");
      ret = VMK_FAILURE;
   }
   return ret;
}


/*
 *----------------------------------------------------------------------------
 *
 *  Uplink_DeviceOpen --
 *
 *    Helper for PCI device open requests.
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
Uplink_DeviceOpen(UplinkDevice *dev)
{
   VMK_ReturnStatus ret = VMK_OK;

   ASSERT(Portset_GlobalLockedHint());
   ASSERT(dev);
   ASSERT(dev->functions);

   if ((dev->flags & DEVICE_OPENED) == FALSE &&
       dev->functions->open != NULL) {
      if (dev->functions->open(dev->netDevice) == 0) {
         LOG(0, "Device open called successfully for %s", dev->devName);
         dev->flags |= DEVICE_OPENED;
      } else {
         LOG(0, "Open handler failed for %s", dev->devName);
         ret = VMK_FAILURE;
      }
   } else {
      LOG(0, "Device %s hasn't registered an open function", dev->devName);
   }

   if (ret == VMK_OK && !(dev->flags & DEVICE_AVAILABLE)) {
      LOG(0, "Connecting device %s to port 0x%x", dev->devName, dev->uplinkPort);
      ret = UplinkConnectAndNotify(dev->uplinkPort, dev);
   }

   if (ret != VMK_OK) {
      LOG(0, "Device %s failed to open", dev->devName);
      UplinkDoDeviceDisconnected(dev);
   }
}



/*
 *----------------------------------------------------------------------------
 *
 *  Uplink_PCIDeviceOpen --
 *
 *    PCI handler for opening the specified device.
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
Uplink_PCIDeviceOpen(int moduleID, PCI_Device *pcidev)
{
   UplinkNode *devNode = NULL;

   LOG(0, "Opening PCI NIC device %s", pcidev->name);
   Portset_GlobalLock();
   if (pcidev != NULL) {
      UplinkTree_FindDevice(&uplinkTree, pcidev->name, &devNode);
      if (devNode) {
         Uplink_DeviceOpen(devNode->uplinkDev);
      }
   }
   Portset_GlobalUnlock();
}



/*
 *----------------------------------------------------------------------------
 *
 *  Uplink_SetupIRQ --
 *
 *    Setup the IRQ for the specified device.
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
Uplink_SetupIRQ(void *d, uint32 vector, IDT_Handler h, void *handlerData)
{
   char *devName = d;
   UplinkDevice *dev = NULL;
   UplinkNode *devNode = NULL;

   if (devName) {
      UplinkTree_FindDevice(&uplinkTree, devName, &devNode);
      if (devNode) {
         dev = devNode->uplinkDev;
         ASSERT(dev);
         dev->uplinkData.intrHandler = h;
         dev->uplinkData.intrHandlerData = handlerData;
         dev->uplinkData.intrHandlerVector = vector;
      } else {
         ASSERT(devNode == NULL);
         LOG(0, "Device not found: %s", devName);
      }
   } else {
      LOG(0, "Nameless device");
   }
}


/*
 *----------------------------------------------------------------------------
 *
 * UplinkTreeDoDeviceOpen --
 *
 *    Goes through the uplink tree and opens every device associated with the
 *    specified module.
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
UplinkTreeDoDeviceOpen(UplinkNode *node, int32 moduleID)
{
   if (node) {
      UplinkNode *cur = node->child;
      while (cur) {
         UplinkTreeDoDeviceOpen(cur, moduleID);
         cur = cur->sibling;
      }
      if (node->uplinkDev && node->uplinkDev->moduleID == moduleID) {
         Uplink_DeviceOpen(node->uplinkDev);
      }
   }
}


/*
 *----------------------------------------------------------------------------
 *
 * UplinkTreeDoDeviceDisconnected --
 *
 *    Goes through the uplink tree and disconnects every device associated with
 *    the specified module.
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
UplinkTreeDoDeviceDisconnected(UplinkNode *node, int32 moduleID)
{
   if (node) {
      UplinkNode *cur = node->child;
      while (cur) {
         UplinkTreeDoDeviceDisconnected(cur, moduleID);
         cur = cur->sibling;
      }
      if (node->uplinkDev && node->uplinkDev->moduleID == moduleID) {
         UplinkDoDeviceDisconnected(node->uplinkDev);
      }
   }
}


/*
 *----------------------------------------------------------------------------
 *
 *  Uplink_PostModuleInit --
 *
 *    PostModuleInit handler that initializes all the devices that were claimed
 *    by the module but haven't still been initialized.
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
Uplink_PostModuleInit(void *data)
{
   int32 moduleID = (int32)data;

   LOG(0, "Initializing devices claimed by module 0x%x", moduleID);
   Portset_GlobalLock();
   UplinkTreeDoDeviceOpen(&uplinkTree.root, moduleID);
   Portset_GlobalUnlock();
}

/*
 *----------------------------------------------------------------------------
 *
 *  Uplink_PreModuleUnload --
 *
 *    PreModuleUnload handler that closes all the devices owned by the
 *    specified vmkernel module.
 *
 *  Results:
 *    None.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static void
Uplink_PreModuleUnload(void *data)
{
   int32 moduleID = (int32)data;
   LOG(0, "Removing all devices with moduleID 0x%x", moduleID);
   Portset_GlobalLock();
   UplinkTreeDoDeviceDisconnected(&uplinkTree.root, moduleID);
   Portset_GlobalUnlock();
}


/*
 *----------------------------------------------------------------------------
 *
 *  Uplink_PostModuleInitFail --
 *
 *    ModuleInitFail handler. Does a cleanup for the device that failed to
 *    initialize.
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
Uplink_PostModuleInitFail(void *dev)
{
   Portset_GlobalLock();
   UplinkDoDeviceDisconnected(dev);
   Portset_GlobalUnlock();
}


/*
 *----------------------------------------------------------------------------
 *
 *  Uplink_RegisterCallbacks --
 *
 *    Register PCI callbacks for the specified device to allow for hot plug
 *    notifications.
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
Uplink_RegisterCallbacks(UplinkDevice *dev)
{
   ASSERT(dev);
   if (dev->moduleID != 0) {
      LOG(0, "Registering callbacks for device %s", dev->devName);
      Mod_RegisterPostInitFunc(dev->moduleID, Uplink_PostModuleInit, (void *)dev->moduleID,
                               Uplink_PostModuleInitFail, (void *)dev);
      Mod_RegisterPreUnloadFunc(dev->moduleID, Uplink_PreModuleUnload, (void *)dev->moduleID);
      Mod_RegisterDevCBFuncs(dev->moduleID, Uplink_PCIDeviceOpen, Uplink_PCIDeviceClose);
   }
}



/*
 *----------------------------------------------------------------------------
 *
 *  Uplink_GetImpl --
 *
 *    Return implementation data associated with the uplink device.
 *
 *  Results:
 *
 *    The implementation data that was associated with the device.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

void *
Uplink_GetImpl(const char *devName)
{
   UplinkNode *devNode;
   void *ret = NULL;
   ASSERT(devName);
   Portset_GlobalLock();
   UplinkTree_FindDevice(&uplinkTree, devName, &devNode);
   if (devNode && (devNode->uplinkDev->flags & DEVICE_PRESENT)) {
      ret = devNode->uplinkDev->netDevice;
   }
   Portset_GlobalUnlock();

   return ret;
}


/*
 *----------------------------------------------------------------------------
 *
 * UplinkSetCapability --
 *
 *    Set the specified capability for all nics under the specified root. If
 *    a nic doesn't have the required capability, a IOChain call is setup in
 *    its uplink port's output chain.
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
UplinkSetCapability(UplinkNode *root, PortID uplinkPort, uint32 idx)
{
   UplinkNode *cur = root->child;
   ASSERT(idx < MAX_CAPABILITIES);
   if (!cur) {
      UplinkDevice *dev = root->uplinkDev;
      uint32 cap = 1 << idx;
      /* root is a leaf node. See if it is a device */
      if (dev && !(dev->hwCap & cap) && !(dev->swCap & cap)) {
         Port *port;
         if (dev->uplinkPort == uplinkPort) {
            Portset_GetLockedPort(dev->uplinkPort, &port);
         } else {
            port = Portset_GetPortExcl(dev->uplinkPort);
         }
         if (port) {
            if (IOChain_InsertCall(&port->outputChain,
                                   uplinkCap[idx].chain,
                                   uplinkCap[idx].fn,
                                   uplinkCap[idx].insert,
                                   uplinkCap[idx].remove,
                                   (IOChainData)dev->netDevice,
                                   uplinkCap[idx].modifiesList,
                                   NULL) == VMK_OK) {
               LOG(0, "Capability 0x%x(fn ptr %p) set for port 0x%x", idx, uplinkCap[idx].fn,
                   uplinkPort);
               dev->swCap |= cap;
            } 
         }
         if (port && dev->uplinkPort != uplinkPort) {
            Portset_ReleasePortExcl(port);
         }
      }
   }

   while (cur) {
      UplinkSetCapability(cur, uplinkPort, idx);
      cur = cur->sibling;
   }
}


/*
 *----------------------------------------------------------------------------
 *
 * UplinkRemoveCapability --
 *
 *    Remove the specified capability from all nics under the specified root.
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
UplinkRemoveCapability(UplinkNode *root, PortID uplinkPort, uint32 idx)
{
   UplinkNode *cur = root->child;
   ASSERT(idx < MAX_CAPABILITIES);
   if (!cur) {
      UplinkDevice *dev = root->uplinkDev;
      uint32 cap = 1 << idx;
      if (dev && (dev->swCap & cap)) {
         Port *port;
         if (dev->uplinkPort == uplinkPort) {
            Portset_GetLockedPort(dev->uplinkPort, &port);
         } else {
            port = Portset_GetPortExcl(dev->uplinkPort);
         }
         if (port) {
            IOChain_RemoveCall(&port->outputChain,  uplinkCap[idx].fn);
            dev->swCap &= ~cap;
            if (dev->uplinkPort != uplinkPort) {
               Portset_ReleasePortExcl(port);
            }
         }
      }
   }

   while (cur) {
      UplinkRemoveCapability(cur, uplinkPort, idx);
      cur = cur->sibling;
   }
}


/*
 *----------------------------------------------------------------------------
 *
 * Uplink_RequestCapability --
 *
 *    External wrapper for adding a capability to the tree under the specified
 *    portset.
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
Uplink_RequestCapability(PortID uplinkPort, uint32 feature)
{
   Portset_GetNameFromPortID(uplinkPort, portsetName, sizeof portsetName);
   if (portsetName[0] != '\0') {
      UplinkNode *portsetNode;
      UplinkTree_FindPortset(&uplinkTree, portsetName, &portsetNode);
      if (portsetNode && uplinkCap[feature].fn) {
         UplinkSetCapability(portsetNode, uplinkPort, feature);
         return VMK_OK;
      } else {
         LOG(0, "Couldn't find portset node for port 0x%x", uplinkPort);
         return VMK_FAILURE;
      }
   } else {
      LOG(0, "Uplink port 0x%x has no portset associated with it.",
          uplinkPort);
      return VMK_FAILURE;
   }
}


/*
 *----------------------------------------------------------------------------
 *
 * Uplink_RemoveCapability --
 *
 *   External wrapper to remove the specified  capability from the tree under
 *   the specified portset.
 *
 * Results:
 *    None
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

void
Uplink_RemoveCapability(PortID uplinkPort, uint32 feature)
{
   Portset_GetNameFromPortID(uplinkPort, portsetName, sizeof portsetName);
   if (portsetName[0] != '\0') {
      UplinkNode *portsetNode;
      UplinkTree_FindPortset(&uplinkTree, portsetName, &portsetNode);
      if (portsetNode) {
         UplinkRemoveCapability(portsetNode, uplinkPort, feature);
      }
   }
}

