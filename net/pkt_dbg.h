/* **********************************************************
 * Copyright (C) 2004 VMware, Inc. All Rights Reserved
 * -- VMware Confidential.
 * **********************************************************/


/*
 * pkt_dbg.h --
 *
 *    debugging structures and macros for the Pkt API
 */

#ifndef _NET_PKTDBG_PUBLIC_H_
#define _NET_PKTDBG_PUBLIC_H_

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMK_MODULE
#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_DISTRIBUTE
#include "includeCheck.h"

#include "net_pkt.h" // needs to be here to pick up PKT_DEBUG switch

#ifdef PKT_DEBUG

#define PKT_DEBUG_FREE_QUEUE_LEN 500

#define PKT_BT_LEN 12
typedef struct PktBTArr {
   uint32        RA[PKT_BT_LEN];
} PktBTArr;

struct PktDbgInfo {
   List_Links     links;       // we keep a list of all allocated packets,
   PktHandle     *pkt;         // a back pointer to parent for convienience,
   PktBTArr       allocBT;     // and a backtrace of their allocation,
   PktBTArr       inputBT;     // their port input,
   PktBTArr       outputBT;    // their last port output,
   PktBTArr       enqueueBT;   // their last enqueueing,
   PktBTArr       dequeueBT;   // their last dequeueing,
   PktBTArr       notifyBT;    // the release which should trigger notification,
   PktBTArr       completeBT;  // their io completion notification,
   PktBTArr       freeBT;      // and their first (and hopefully only) free.
};

// export these which are needed for the inlines
extern SP_SpinLockIRQ  netPktDbgLock;
extern List_Links      netPktDbgList[1];
extern uint32          netPktDbgAllocCount;
extern List_Links      netPktDbgFreeQueue[1]; 
extern uint32          netPktDbgFreeQueueCount;

void Pkt_DbgBT(PktBTArr *btArr);
void Pkt_DbgLogBT(char *str, PktHandle *pkt, PktBTArr *btArr);

#endif // PKT_DEBUG


/*
 *----------------------------------------------------------------------------
 *
 *  Pkt_DbgOnAlloc --
 *
 *    Grab a backtrace and put the packet on a list so we can find it.
 *
 *  Results:
 *    None.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */
static INLINE void
Pkt_DbgOnAlloc(PktHandle *pkt) 
{
#ifdef PKT_DEBUG
   SP_IRQL irql;

   pkt->dbg = Mem_Alloc(sizeof(*pkt->dbg));
   ASSERT(pkt->dbg != NULL);
   memset(pkt->dbg, 0, sizeof(*pkt->dbg));
   pkt->dbg->pkt = pkt;
   Pkt_DbgBT(&pkt->dbg->allocBT);
   irql = SP_LockIRQ(&netPktDbgLock, SP_IRQL_KERNEL);
   List_Insert(&pkt->dbg->links, LIST_ATFRONT(&netPktDbgList));
   netPktDbgAllocCount++;
   ASSERT(netPktDbgAllocCount < 10000);
   SP_UnlockIRQ(&netPktDbgLock, irql);
#endif // PKT_DEBUG
}


/*
 *----------------------------------------------------------------------------
 *
 *  Pkt_DbgOnInput --
 *
 *    Grab a backtrace for the last input.
 *
 *  Results:
 *    None.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */
static INLINE void
Pkt_DbgOnInput(PktList *pktList) 
{
#ifdef PKT_DEBUG
   PktHandle *pkt = PktList_GetHead(pktList);

   while (pkt != NULL) {
      Pkt_DbgBT(&pkt->dbg->inputBT);
      pkt = PktList_GetNext(pktList, pkt);
   }
#endif // PKT_DEBUG
}


/*
 *----------------------------------------------------------------------------
 *
 *  Pkt_DbgOnOutput --
 *
 *    Grab a backtrace for the last output.
 *
 *  Results:
 *    None.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */
static INLINE void
Pkt_DbgOnOutput(PktList *pktList) 
{
#ifdef PKT_DEBUG
   PktHandle *pkt = PktList_GetHead(pktList);

   while (pkt != NULL) {
      Pkt_DbgBT(&pkt->dbg->outputBT);
      //Pkt_DbgLogBT("Pkt_DbgOnOutput", pkt, &pkt->dbg->outputBT);
      pkt = PktList_GetNext(pktList, pkt);
   }
#endif // PKT_DEBUG
}


/*
 *----------------------------------------------------------------------------
 *
 *  Pkt_DbgOnEnqueue --
 *
 *    Grab a backtrace for the last enqueue.
 *
 *  Results:
 *    None.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */
static INLINE void
Pkt_DbgOnEnqueue(PktList *pktList) 
{
#ifdef PKT_DEBUG
   PktHandle *pkt = PktList_GetHead(pktList);

   while (pkt != NULL) {
      Pkt_DbgBT(&pkt->dbg->enqueueBT);
      pkt = PktList_GetNext(pktList, pkt);
   }
#endif // PKT_DEBUG
}


/*
 *----------------------------------------------------------------------------
 *
 *  Pkt_DbgOnDequeue --
 *
 *    Grab a backtrace for the last dequeue.
 *
 *  Results:
 *    None.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */
static INLINE void
Pkt_DbgOnDequeue(PktList *pktList) 
{
#ifdef PKT_DEBUG
   PktHandle *pkt = PktList_GetHead(pktList);

   while (pkt != NULL) {
      Pkt_DbgBT(&pkt->dbg->dequeueBT);
      pkt = PktList_GetNext(pktList, pkt);
   }
#endif // PKT_DEBUG
}


/*
 *----------------------------------------------------------------------------
 *
 *  Pkt_DbgOnComplete --
 *
 *    Grab a backtrace for the caller who *should* do the complete 
 *    notification.
 *
 *  Results:
 *    None.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */
static INLINE void
Pkt_DbgOnComplete(PktHandle *pkt) 
{
#ifdef PKT_DEBUG
   ASSERT(pkt->dbg->completeBT.RA[0] == 0); // we better be the first and only
   Pkt_DbgBT(&pkt->dbg->completeBT);
   //Pkt_DbgLogBT("Pkt_DbgOnComplete", pkt, &pkt->dbg->completeBT);
#endif // PKT_DEBUG
}


/*
 *----------------------------------------------------------------------------
 *
 *  Pkt_DbgOnNotify --
 *
 *    Grab a backtrace for the io complete notification.
 *
 *  Results:
 *    None.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */
static INLINE void
Pkt_DbgOnNotify(PktList *pktList) 
{
#ifdef PKT_DEBUG
   PktHandle *pkt = PktList_GetHead(pktList);

   while (pkt != NULL) {
      ASSERT(pkt->dbg->notifyBT.RA[0] == 0); // we better be the first and only
      Pkt_DbgBT(&pkt->dbg->notifyBT);
      pkt = PktList_GetNext(pktList, pkt);
   }
#endif // PKT_DEBUG
}


/*
 *----------------------------------------------------------------------------
 *
 *  Pkt_DbgOnFree --
 *
 *    Remove the packet from the global list and dec the total count
 *
 *  Results:
 *    None.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */
static INLINE PktHandle *
Pkt_DbgOnFree(PktHandle *pkt) 
{
#ifdef PKT_DEBUG
   SP_IRQL irql;

   Pkt_DbgBT(&pkt->dbg->freeBT);
   
   irql = SP_LockIRQ(&netPktDbgLock, SP_IRQL_KERNEL);
   List_Remove(&pkt->dbg->links);
   netPktDbgAllocCount--;
   List_Insert(&pkt->dbg->links, LIST_ATREAR(netPktDbgFreeQueue));
   if (netPktDbgFreeQueueCount < PKT_DEBUG_FREE_QUEUE_LEN) {
      netPktDbgFreeQueueCount++;
      pkt = NULL;
      SP_UnlockIRQ(&netPktDbgLock, irql);
   } else {
      struct PktDbgInfo *dbg = (struct PktDbgInfo *)List_First(netPktDbgFreeQueue);
      pkt = dbg->pkt;
      List_Remove(&dbg->links);
      SP_UnlockIRQ(&netPktDbgLock, irql);
      Mem_Free(dbg);
   }
#endif // PKT_DEBUG

   return pkt;
}

#endif // _NET_PKTDBG_PUBLIC_H_

