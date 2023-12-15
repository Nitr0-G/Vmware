
/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. 
 * -- VMware Confidential
 * **********************************************************/

/*
 * vlance_vmkdev.c  --
 *
 *   Interface to vmkernel networking for vlance devices.
 */

#include "vmkernel.h"
#define LOGLEVEL_MODULE_LEN 0
#define LOGLEVEL_MODULE Net
#include "log.h"

#include "net_int.h"
#include "netProto.h"
#include "kseg.h"
#include "action.h"

#include "vlance_vmkdev.h"

typedef struct Vlance_ClientData {
   SP_SpinLock  lock;              // protects this struct
   PktList      rxQueue;           // holds rx pkts until VMM calls for DMA
   uint32       maxRxQueueLen;     // how large can rxQueue grow
   uint32       vmkChannelPending; // channel for interrupts
} Vlance_ClientData;


/*
 *----------------------------------------------------------------------
 *
 * VlanceVMKDevNotfyPending --
 *
 *      Post an action to the VMM connected to a vlance port.
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
VlanceVMKDevNotifyPending(Port *port, Vlance_ClientData *vcd)
{
   World_Handle *world;
   ASSERT(SP_IsLocked(&vcd->lock));

   /*
    * potential TODO:
    *
    * We could coalesce actions here, but since we do a vmm/vmkernel/vmm
    * transition for every packet we receive anyway, it won't help. We
    * could probably get a nice performance bump, however, if we enabled
    * batched receives and coalescing of actions.
    */

   world = Port_ChooseWorldForIntr(port);

   LOG(5, "0x%x %u", port->portID, world->worldID);

   Port_ClientStatInc(&port->clientStats.interrupts, 1);

   Action_Post(world, vcd->vmkChannelPending);

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * VlanceVMKDev_TX --
 *
 *      Transmit packets on a vlance port.
 *
 * Results: 
 *	returns a VMK_ReturnStatus.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
VlanceVMKDev_Tx(Port *port, NetSG_Array *sg)
{
   VMK_ReturnStatus status;
   PktHandle *pkt;
   PktList txList[1];
   unsigned int len = 0;
   unsigned int i;
   uint32 allocLen;

   LOG(5, "0x%x: %p", port->portID, sg);

   for (i = 0; i < sg->length; i++) {
      len += sg->sg[i].length;
   }

   allocLen = MAX(len, MIN_TX_FRAME_LEN);

   pkt = Pkt_Alloc(Portset_GetMaxUplinkImplSz(port->ps), allocLen);
   if (UNLIKELY(pkt == NULL)) {
      status = VMK_NO_RESOURCES;
      goto done;
   }

   for (i = 0; i < sg->length; i++) {
      void *vaddr;
      KSEG_Pair *pair;

      vaddr = Kseg_GetPtrFromPA(Port_GetWorldGroupLeader(port),
                                NET_SG_MAKE_PA(sg->sg[i]), 
                                sg->sg[i].length, 
                                TRUE, 
                                &pair,
                                &status);
      if (vaddr != NULL) {
         status = Pkt_AppendBytes(vaddr, sg->sg[i].length, pkt);
         Kseg_ReleasePtr(pair);
      }
      
      if (UNLIKELY(status != VMK_OK)) {
         Port_ClientStatInc(&port->clientStats.droppedTx, 1);
         Pkt_Release(pkt);
         goto done;
      }
   }

   memset(pkt->frameVA + len, 0, allocLen - len);

   PktSetFrameLen(pkt, allocLen);

   Port_ClientStatInc(&port->clientStats.pktsTxOK, 1);
   // we use the padded length so that we'll be consistent with the receiver
   Port_ClientStatInc(&port->clientStats.bytesTxOK, allocLen);

   PktList_Init(txList);
   PktList_AddToHead(txList, pkt);
   Port_Input(port, txList);

  done:
   
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * VlanceVMKDevRx --
 *
 *      Receive packets on a vlance port, put them on a queue and
 *      post an action to the VMM requesting it call down to receive
 *      the data.
 *
 * Results: 
 *	returns a VMK_ReturnStatus.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus 
VlanceVMKDevRx(Port *port, IOChainData iocd, struct PktList *rxList)
{
   unsigned int queueAvailable;
   VMK_ReturnStatus status = VMK_OK;
   PktList clonedList;
   Vlance_ClientData *vcd = (Vlance_ClientData *)port->impl.data;

   LOG(5, "0x%x: %u packets", port->portID, PktList_Count(rxList));

   SP_Lock(&vcd->lock);
      
   queueAvailable = vcd->maxRxQueueLen - PktList_Count(&vcd->rxQueue);
   status = PktList_CloneN(rxList, &clonedList, queueAvailable);
   if (status == VMK_OK) {
      PktList_Join(&vcd->rxQueue, &clonedList);
      VlanceVMKDevNotifyPending(port, vcd);
   } else {
      status = VMK_NO_RESOURCES;
   }

   SP_Unlock(&vcd->lock);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * VlanceVMKDev_RxDMA --
 *
 *      The VMM calls this upon our request via an action post so that
 *      we may copy the packet data up to the guest buffers.
 *
 * Results: 
 *	returns a VMK_ReturnStatus, and the number of bytes DMA'd is
 *      placed in byteCount.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus 
VlanceVMKDev_RxDMA(Port *port, NetSG_Array *sg, uint32 *byteCount)
{
   Vlance_ClientData *vcd;
   PktHandle *pkt;
   int i, totalBytesLeft, padLen;
   VMK_ReturnStatus status = VMK_BAD_PARAM;

   LOG(5, "0x%x: %p", port->portID, sg);

   *byteCount = 0;

   vcd = (Vlance_ClientData *)port->impl.data;

   SP_Lock(&vcd->lock);

   pkt = PktList_GetHead(&vcd->rxQueue);
   if (UNLIKELY(pkt == NULL)) {
      /*
       * OK, it's only slightly more LIKELY that it isn't NULL
       * since the VMM rx loop is terminated by a failing call
       * to this function when there are no more packets queued
       */
      status = VMK_OK;
      goto done;
   }

   if (UNLIKELY(sg == NULL)) {
      /* 
       * The caller can pass a NULL pointer if he just wants to drop
       * the packet.
       */
      status = VMK_OK;
      Port_ClientStatInc(&port->clientStats.droppedRx, 1);
      goto done;
   }

   padLen = 4; // four extra bytes for the FCS
   totalBytesLeft = PktGetFrameLen(pkt) + padLen; 

   for (i = 0; i < sg->length; i++) {
      char *vaddr;
      KSEG_Pair *pair;
      int bytesToMap = MIN(totalBytesLeft, sg->sg[i].length);
      int bytesToCopy = 0;
      int bytesToZero = 0;
      
      vaddr = Kseg_GetPtrFromPA(Port_GetWorldGroupLeader(port),
                                NET_SG_MAKE_PA(sg->sg[i]), 
                                bytesToMap, 
                                FALSE, 
                                &pair,
                                &status);
      ASSERT(status != VMK_WOULD_BLOCK);  // The VMM should have pinned it
         
      if (vaddr != NULL) {
         /*
          * This is a little ugly because we need to pad the guest
          * buffer with fcs but those bytes may or may not span sg
          * array entries, just as the rest of the buffer might or
          * might not, so we have the following conditionals to
          * evaluate as we iterate.  The common case is that all three
          * of the following evaluate TRUE for the first (and only)
          * scatter gather entry.
          *
          * XXX is it really the common case?  not that I'm going to
          *     rewrite it if it isn't but we should verify that for
          *     various guests we support. -wal
          */
         if (*byteCount < PktGetFrameLen(pkt)) {
            ASSERT(totalBytesLeft > padLen);
            bytesToCopy = MIN(totalBytesLeft - padLen, bytesToMap);
            status = Pkt_CopyBytesOut(vaddr, bytesToCopy, *byteCount, pkt);
            *byteCount += bytesToCopy;
            totalBytesLeft -= bytesToCopy;
         }
         if (*byteCount >= PktGetFrameLen(pkt)) {
            bytesToZero = bytesToMap - bytesToCopy; 
            memset(vaddr + bytesToCopy, 0, bytesToZero);
            *byteCount += bytesToZero;
            totalBytesLeft -= bytesToZero;
         }
         Kseg_ReleasePtr(pair);
         if (totalBytesLeft == 0) {
            status = VMK_OK;
            break;
         }
      }
      
      if (UNLIKELY(status != VMK_OK)) {
         break;
      }
   }  

   if (status == VMK_OK) {
      /*
       * can't do this in the similar conditional below bc that's where 
       * we go for intentional drops (sg == NULL)
       */
      Port_ClientStatInc(&port->clientStats.pktsRxOK, 1);
      Port_ClientStatInc(&port->clientStats.bytesRxOK, PktGetFrameLen(pkt));
   }

  done:

   if (status == VMK_OK) {
      if (pkt != NULL) {
         PktList_Remove(&vcd->rxQueue, pkt);
      }
   } else {
      pkt = NULL;
   }
   
   SP_Unlock(&vcd->lock);

   if (pkt != NULL) {
      Net_IOComplete(pkt);
   }

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * VlanceVMKDevDisable --
 *
 *      Disable a vlance port.
 *
 * Results: 
 *	returns a VMK_ReturnStatus.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
VlanceVMKDevDisable(Port *port, Bool force)
{
   VMK_ReturnStatus status = VMK_OK;
   Vlance_ClientData *vcd = (Vlance_ClientData *)port->impl.data;

   LOG(0, "0x%x", port->portID);

   if (vcd != NULL) {
      SP_CleanupLock(&vcd->lock);
      Mem_Free(vcd);
      Port_InitImpl(port);
   }
   IOChain_RemoveCall(&port->outputChain, VlanceVMKDevRx);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * VlanceVMKDev_Enable --
 *
 *      Enable a vlance port.
 *
 * Results: 
 *	returns a VMK_ReturnStatus.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
VlanceVMKDev_Enable(Port *port, uint32 vmkChannelPending)
{
   VMK_ReturnStatus status = VMK_OK;
   Vlance_ClientData *vcd;
   
   LOG(0, "0x%x: %u", port->portID, vmkChannelPending);

   vcd = (Vlance_ClientData *)Mem_Alloc(sizeof(Vlance_ClientData));
   if (vcd == NULL) {
      status = VMK_NO_MEMORY;
      goto done;
   }
   SP_InitLock("vlance_client", &vcd->lock, SP_RANK_VLANCE_CLIENT);

   port->impl.data = (void *)vcd;  
   port->impl.disable = VlanceVMKDevDisable;

   vcd->vmkChannelPending = vmkChannelPending;

   PktList_Init(&vcd->rxQueue);
   vcd->maxRxQueueLen = CONFIG_OPTION(NET_MAX_PORT_RX_QUEUE);

   status = IOChain_InsertCall(&port->outputChain, 
                               IO_CHAIN_RANK_TERMINAL, 
                               VlanceVMKDevRx, 
                               NULL, 
                               NULL,
                               NULL,
                               FALSE,
                               NULL);
   if (status != VMK_OK) {
      ASSERT(FALSE);
      Warning("failed to terminate output chain: port 0x%x on %s: %s",
              port->portID, port->ps->name, VMK_ReturnStatusToString(status));
      goto done;
   }

  done:

   if (status != VMK_OK) {
      Warning("failed to enable port 0x%x on %s: %s",  
              port->portID, port->ps->name, VMK_ReturnStatusToString(status));
   }

   return status;
}
