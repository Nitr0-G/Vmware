/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. 
 * -- VMware Confidential
 * **********************************************************/

/*
 * nulldev.c  --
 *
 *    Implementation of a nulldev portset.  Each frame 
 *    written to any port on this portset will be counted 
 *    and discarded.
 */

#include "vmkernel.h"
#define LOGLEVEL_MODULE_LEN 0
#define LOGLEVEL_MODULE Net
#include "log.h"

#include "net_int.h"


typedef struct NullPortStats {
   Proc_Entry  procNode;
   uint32      pktCount;
   uint32      byteCount;               
} NullPortStats;

/*
 *----------------------------------------------------------------------
 *
 * NulldevPortStatsProcRead --
 *
 *      Read handler for stats proc node for the port.
 *
 * Results: 
 *	VMK_OK.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
NulldevPortStatsProcRead(Proc_Entry  *entry,
                         char        *page,
                         int         *len)
{
   NullPortStats *stats = (NullPortStats *)entry->private;
   *len = 0;
   
   Proc_Printf(page, len, "%10s %10s\n", "pktCount", "byteCount");
   Proc_Printf(page, len, "%10u %10u\n", stats->pktCount, stats->byteCount);

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * NulldevIncPortStats --
 *
 *      Update stats for the port.
 *
 * Results: 
 *	VMK_OK and stats are updated.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
NulldevIncPortStats(Port *port, IOChainData iocd, 
                    struct PktList *pktList)
{
   PktHandle *pkt = PktList_GetHead(pktList);
   NullPortStats *stats = (NullPortStats *)iocd;

   while (pkt != NULL) {
      stats->pktCount++;
      stats->byteCount += PktGetFrameLen(pkt);
      pkt = PktList_GetNext(pktList, pkt);
   }

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * NulldevPortConnect --
 *
 *      Nulldev-specific port connect routine.
 *
 * Results:
 *      VMK_ReturnStatus.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
NulldevPortConnect(Portset *ps, Port *port)
{
   VMK_ReturnStatus status;
   unsigned int idx = Portset_GetPortIdx(port);
   NullPortStats *stats = (NullPortStats *)ps->devImpl.data;
   stats += idx;

   status = IOChain_InsertCall(&port->inputChain, IO_CHAIN_RANK_PRE_FILTER, 
                               NulldevIncPortStats, NULL, NULL,
                               stats, FALSE, NULL);
   if (status == VMK_OK) {
      Proc_InitEntry(&stats->procNode);
      stats->procNode.parent = &port->procDir;
      stats->procNode.read = NulldevPortStatsProcRead;
      stats->procNode.private = stats;
      ProcNet_Register(&stats->procNode, "nulldev_stats", FALSE);
   }
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * NulldevPortDisconnect --
 *
 *      Nulldev-specific port disconnect routine.
 *
 * Results:
 *      VMK_ReturnStatus.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
NulldevPortDisconnect(Portset *ps, Port *port)
{
   unsigned int idx = Portset_GetPortIdx(port);
   NullPortStats *stats = (NullPortStats *)ps->devImpl.data;
   if (stats->procNode.parent != NULL) {
      ProcNet_Remove(&stats->procNode);
      Proc_InitEntry(&stats->procNode);
   }
   stats[idx].pktCount = 0;
   stats[idx].byteCount = 0;

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * NulldevDispatch --
 *
 *      Nulldev-specific dispatch routine. Does nothing.
 *
 * Results:
 *      VMK_OK.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
NulldevDispatch(Portset *ps, struct PktList *pktList, Port *srcPort)
{
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * NulldevDeactivate --
 *
 *      Nulldev-specific deactivation routine.
 *
 * Results:
 *      VMK_OK.
 *
 * Side effects:
 *      Frees resources.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
NulldevDeactivate(Portset *ps)
{
   if (ps->devImpl.data != NULL) {
      Mem_Free(ps->devImpl.data);
      ps->devImpl.data = NULL;
   }
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Nulldev_Activate --
 *
 *      Nulldev-specific activation routine.
 *
 * Results:
 *      Returns a VMK_ReturnStatus.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
Nulldev_Activate(Portset *ps)
{
   unsigned int size = ps->numPorts * sizeof(NullPortStats);
   ps->devImpl.data = Mem_Alloc(size);
   if ( ps->devImpl.data == NULL) {
      return VMK_NO_RESOURCES;
   }
   memset(ps->devImpl.data, 0, size);
   ps->devImpl.portConnect = NulldevPortConnect;
   ps->devImpl.portDisconnect = NulldevPortDisconnect;
   ps->devImpl.dispatch = NulldevDispatch;
   ps->devImpl.deactivate = NulldevDeactivate;
   
   return VMK_OK;
}


