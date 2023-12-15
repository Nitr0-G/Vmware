/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. 
 * -- VMware Confidential
 * **********************************************************/

/*
 * loopback.c  --
 *
 *    Implementation of a loopback portset.  Each frame 
 *    written to the any port on this portset will
 *    be reflected back to that port.
 */

#include "vmkernel.h"
#define LOGLEVEL_MODULE_LEN 0
#define LOGLEVEL_MODULE Net
#include "log.h"

#include "net_int.h"

typedef struct LoopbackChainStats {
   uint32      pktCount;
   uint32      byteCount;               
} LoopbackChainStats;

typedef struct LoopbackPortStats {
   Proc_Entry           procNode;
   LoopbackChainStats   input;
   LoopbackChainStats   output;
   LoopbackChainStats   complete;
} LoopbackPortStats;

/*
 *----------------------------------------------------------------------
 *
 * LoopbackPortStatsProcRead --
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
LoopbackPortStatsProcRead(Proc_Entry  *entry,
                          char        *page,
                          int         *len)
{
   LoopbackPortStats *stats = (LoopbackPortStats *)entry->private;
   *len = 0;
   
   Proc_Printf(page, len, "%10s %10s %10s %10s %10s %10s\n", 
               "pktsIn", "bytesIn", "pktsOut", "bytesOut", "pktsComp", "bytesComp");
   Proc_Printf(page, len, "%10u %10u %10u %10u %10u %10u\n", 
               stats->input.pktCount, stats->input.byteCount,
               stats->output.pktCount, stats->output.byteCount,
               stats->complete.pktCount, stats->complete.byteCount);

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * LoopbackIncChainStats --
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
LoopbackIncChainStats(Port *port, IOChainData iocd, 
                     struct PktList *pktList)
{
   PktHandle *pkt = PktList_GetHead(pktList);
   LoopbackChainStats *stats = (LoopbackChainStats *)iocd;

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
 * LoopbackPortConnect --
 *
 *      Loopback-specific port connect routine.
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
LoopbackPortConnect(Portset *ps, Port *port)
{
   VMK_ReturnStatus status;
   unsigned int idx = Portset_GetPortIdx(port);
   LoopbackPortStats *stats = (LoopbackPortStats *)ps->devImpl.data;
   stats += idx;

   status = IOChain_InsertCall(&port->inputChain, IO_CHAIN_RANK_PRE_FILTER, 
                               LoopbackIncChainStats, NULL, NULL, &stats->input,
                               FALSE, NULL);
   if (status == VMK_OK) {
      status = IOChain_InsertCall(&port->outputChain, IO_CHAIN_RANK_POST_QUEUE, 
                                  LoopbackIncChainStats, NULL, NULL,
                                  &stats->output, FALSE, NULL);
      if (status == VMK_OK) {
         status = IOChain_InsertCall(&port->notifyChain, IO_CHAIN_RANK_POST_QUEUE, 
                                     LoopbackIncChainStats, NULL, NULL,
                                     &stats->complete, FALSE, NULL);
         if (status == VMK_OK) {
            Proc_InitEntry(&stats->procNode);
            stats->procNode.parent = &port->procDir;
            stats->procNode.read = LoopbackPortStatsProcRead;
            stats->procNode.private = stats;
            ProcNet_Register(&stats->procNode, "loopback_stats", FALSE);
         }
      }
   }
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * LoopbackPortDisconnect --
 *
 *      Loopback-specific port disconnect routine.
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
LoopbackPortDisconnect(Portset *ps, Port *port)
{
   unsigned int idx = Portset_GetPortIdx(port);
   LoopbackPortStats *stats = (LoopbackPortStats *)ps->devImpl.data;
   if (stats->procNode.parent != NULL) {
      ProcNet_Remove(&stats->procNode);
      Proc_InitEntry(&stats->procNode);
   }
   stats[idx].input.pktCount = 0;
   stats[idx].input.byteCount = 0;
   stats[idx].output.pktCount = 0;
   stats[idx].output.byteCount = 0;
   stats[idx].complete.pktCount = 0;
   stats[idx].complete.byteCount = 0;

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * LoopbackDispatch --
 *
 *      Loopback-specific dispatch routine, simply reflects any frames
 *      input to a port back to that port's output chain unmodified.
 *
 * Results:
 *      Returns a VMK_ReturnStatus.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
LoopbackDispatch(Portset *ps, struct PktList *pktList, Port *srcPort)
{
   VMK_ReturnStatus status;
   
   status = Port_Output(srcPort, pktList);

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * LoopbackDeactivate --
 *
 *      Loopback-specific deactivation routine.
 *
 * Results:
 *      Returns a VMK_ReturnStatus.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
LoopbackDeactivate(Portset *ps)
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
 * Loopback_Activate --
 *
 *      Loopback-specific activation routine.
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
Loopback_Activate(Portset *ps)
{
   unsigned int size = ps->numPorts * sizeof(LoopbackPortStats);
   ps->devImpl.data = Mem_Alloc(size);
   if ( ps->devImpl.data == NULL) {
      return VMK_NO_RESOURCES;
   }
   memset(ps->devImpl.data, 0, size);
   ps->devImpl.portConnect = LoopbackPortConnect;
   ps->devImpl.portDisconnect = LoopbackPortDisconnect;
   ps->devImpl.dispatch = LoopbackDispatch;
   ps->devImpl.deactivate = LoopbackDeactivate;

   return VMK_OK;
}




