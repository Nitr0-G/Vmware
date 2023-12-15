
/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. 
 * -- VMware Confidential
 * **********************************************************/

/*
 * vmxnet2_vmkdev.c  --
 *
 *   Interface to vmkernel networking for vmxnet2 style devices.
 */

#include "vmkernel.h"
#define LOGLEVEL_MODULE_LEN 0
#define LOGLEVEL_MODULE Net
#include "log.h"

#include "net_int.h"
#include "vmxnet2_vmkdev.h"
#include "vmxnet2_def.h"
#include "alloc.h"
#include "alloc_inline.h"
#include "kvmap.h"
#include "kseg.h"
#include "action.h"
#include "config_dist.h"

typedef struct Vmxnet2_ClientData {
   SP_SpinLock          lock;
   Vmxnet2_ImplData     id;
   PktList              txDeferred[1];
   VA                   ddMapped;
   uint32               ddLen;
   uint32               ddOffset;
   uint32               intrActionIdx;
} Vmxnet2_ClientData;


/*
 *----------------------------------------------------------------------
 *
 * Vmxnet2VMKDevPktAlloc --
 *
 *      Allocate packets for use with vmxnet2 devices.
 *
 * TODO:
 *      - make this do something besides normal Pkt_Alloc(),
 *        like have a cache to avoid the extra lock(s) and
 *        data reinitialization.
 *
 * Results: 
 *	Returns an allocated packet buffer, or NULL on failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static INLINE PktHandle * 
Vmxnet2VMKDevPktAlloc(Port *port, Vmxnet2_ClientData *vcd, unsigned int len)
{
   return Pkt_Alloc(Portset_GetMaxUplinkImplSz(port->ps), len);
}
#ifndef ESX3_NETWORKING_NOT_DONE_YET
#error see TODO above
#endif


/*
 *----------------------------------------------------------------------
 *
 * Vmxnet2VMKDevTxCompleteOne --
 *
 *      Tx complete a packet on a vmxnet2 port.
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
Vmxnet2VMKDevTxCompleteOne(Port *port, Vmxnet2_ClientData *vcd, 
                           PktHandle *pkt)
{
   Vmxnet2_TxRingEntry *txre;
   unsigned int idx;

   idx = (unsigned int)pkt->pktDesc->ioCompleteData;
   ASSERT(idx < vcd->id.txRingLength);
   
   LOG(10, "%p: %p %u", port, pkt, idx);
   
   txre = Vmxnet2_GetTxEntry(&vcd->id, idx);
   ASSERT(txre->ownership == VMXNET2_OWNERSHIP_NIC_PENDING);
   txre->ownership = VMXNET2_OWNERSHIP_DRIVER;
   
   PktClearIOCompleteData(pkt);
}


/*
 *----------------------------------------------------------------------
 *
 * Vmxnet2VMKDevPostIntr --
 *
 *      Post an interrupt to the guest connected to a vmxnet2 port.
 *
 * Results: 
 *	VMK_ReturnStatus.
 *
 * Side effects:
 *	posts a monitor action.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus 
Vmxnet2VMKDevPostIntr(Port *port, Vmxnet2_ClientData *vcd)
{
   World_Handle *world;
   ASSERT(SP_IsLocked(&vcd->lock));

#ifndef ESX3_NETWORKING_NOT_DONE_YET
#error need intr coalescing logic here
#endif

   world = Port_ChooseWorldForIntr(port);

   LOG(5, "%p %u", port, world->worldID);

   Port_ClientStatInc(&port->clientStats.interrupts, 1);

   Action_Post(world, vcd->intrActionIdx);

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Vmxnet2VMKDevGetMAs --
 *
 *      Get and pin MAs for guest PAs.
 *
 * TODO:
 *
 *     - handle underlying devs with no high DMA support
 *     - exploit guest pinned buffers
 *     - actually pin the pages instead of just grabbing MAs
 *
 * Results: 
 *	returns a VMK_ReturnStatus, if VMK_OK, then sgMA is filled with
 *      the MAs corresponding to txre.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static INLINE VMK_ReturnStatus 
Vmxnet2VMKDevGetMAs(World_Handle *world, Vmxnet2_ClientData *vcd, 
                    Vmxnet2_TxRingEntry *txre, PktHandle *pkt)
{
   VMK_ReturnStatus status = VMK_BAD_PARAM;
   int i;
   uint16 totalLen = 0;
   
   if (UNLIKELY(txre->sg.length > VMXNET2_SG_DEFAULT_LENGTH)) {
      // XXX should throttle this
      VmWarn(world->worldID, "bad txre sg length: %u", txre->sg.length);
      ASSERT(FALSE);
      return VMK_BAD_PARAM;
   }
   
   PktSetBufType(pkt, NET_SG_MACH_ADDR);
   
   for (i = 0; i < txre->sg.length; i++) {
      Alloc_Result result;
      PA guestPA;
      unsigned int length;
      
      guestPA = NET_SG_MAKE_PA(txre->sg.sg[i]);
      length = txre->sg.sg[i].length;
      
     next_frag:
      status = Alloc_PhysToMachine(world, 
                                   guestPA, 
                                   length, 
                                   0, 
                                   FALSE, 
                                   &result);
      if (status != VMK_OK) {
         LOG(1, "failed to get MA for 0x%Lx: %s", 
             guestPA, VMK_ReturnStatusToString(status));
         break;
      }
      if (result.length >= length) {
         status = Pkt_AppendFrag(result.maddr, length, pkt);
         if (UNLIKELY(status != VMK_OK)) {
            return status;
         }
         totalLen += length;
      } else {
         status = Pkt_AppendFrag(result.maddr, result.length, pkt);
         if (UNLIKELY(status != VMK_OK)) {
            return status;
         }
         totalLen += result.length;
         length -= result.length;
         guestPA += result.length;
         goto next_frag;
      }
   }
   
   PktSetFrameLen(pkt, totalLen);
   
   return status;
}
#ifndef ESX3_NETWORKING_NOT_DONE_YET
#error see TODO above
#endif


/*
 *----------------------------------------------------------------------
 *
 * Vmxnet2VMKDev_PinTxBuffers --
 *
 *      Pin down the tx buffers from the guest into the vmkernel
 *      so that we don't have to translate/pin them over and over.
 *
 * Results: 
 *	returns a VMK_ReturnStatus
 *
 * Side effects:
 *      Pages are pinned, MAs and VAs are stored by the client impl.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
Vmxnet2VMKDev_PinTxBuffers(Net_PortID portID)
{
#ifndef ESX3_NETWORKING_NOT_DONE_YET
#error need Mike Chen to implement vmkernel based page pinning.
#endif // ESX3_NETWORKING_NOT_DONE_YET
   return VMK_FAILURE;
}


/*
 *----------------------------------------------------------------------
 *
 * Vmxnet2VMKDevCancelAllPendingTx --
 *
 *      Complete all pending transmits without even attempting to send 
 *      them.
 *
 * Results: 
 *      None
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

static void
Vmxnet2VMKDevCancelAllPendingTx(Port *port, Vmxnet2_ClientData *vcd)
{
   Vmxnet2_TxRingEntry *txre;

   ASSERT(SP_IsLocked(&vcd->lock));

   while ((txre = Vmxnet2_GetNextTx(&vcd->id)) != NULL) {
      Port_ClientStatInc(&port->clientStats.droppedTx, 1);
      Vmxnet2_IncNextTx(&vcd->id);
      txre->ownership = VMXNET2_OWNERSHIP_DRIVER;  
   }
}


/*
 *----------------------------------------------------------------------
 *
 * Vmxnet2VMKDevPollTxRing --
 *
 *      Create a PktList of all the ready tx entries in the device's
 *      tx ring.  All packets after the first whose buffers cannot be 
 *      pinned are queued until those buffers are paged in and the 
 *      guest device driver is notified that the device is stopped.
 *      We pull them from the guest and attempt to pin them all here
 *      since the first failed attempt to pin the page will actually
 *      initiate the required page-in.
 *
 * Results: 
 *	returns a VMK_ReturnStatus
 *      pktList is filled with any frames which are ready for tx
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

static INLINE VMK_ReturnStatus
Vmxnet2VMKDevPollTxRing(Port *port, Vmxnet2_ClientData *vcd, PktList *pktList)
{
   VMK_ReturnStatus status = VMK_OK;
   PktHandle *pkt, *pktMapped;
   Vmxnet2_TxRingEntry *txre;
   unsigned int idx;
   World_Handle *world = Port_GetWorldGroupLeader(port);
   unsigned int copyBreak = CONFIG_OPTION(NET_VMM_TX_COPYBREAK);

   ASSERT(SP_IsLocked(&vcd->lock));

   /*
    * clear out the guest's ring.
    */ 
   while ((txre = Vmxnet2_GetNextTx(&vcd->id)) != NULL) {

      LOG(10, "pulling ring index %u", vcd->id.txNICNext);

      pkt = Vmxnet2VMKDevPktAlloc(port, vcd, 0);
      if (UNLIKELY(pkt == NULL)) {
         LOG(1, "cannot allocate packet");
         status = VMK_NO_RESOURCES;
         break;
      }

      status = Vmxnet2VMKDevGetMAs(world, vcd, txre, pkt);
      if (UNLIKELY(status != VMK_OK)) {
         LOG(1, "cannot get MAs for guest packet: %s", 
             VMK_ReturnStatusToString(status));
         break;
      }

      idx = Vmxnet2_IncNextTx(&vcd->id);
      PktSetIOCompleteData(pkt, (IOData)idx);
      PktSetSrcPort(pkt, port->portID);

      if (PktGetFrameLen(pkt) > copyBreak) {
         /*
          * make a partial copy of the packet so that we have the headers
          * mapped into vmkernel and then toss the original packet handle
          */
         LOG(10, "%u byte partial copy of %u byte packet", 
             copyBreak, PktGetFrameLen(pkt));
         pktMapped = Pkt_PartialCopy(pkt, 
                                     Portset_GetMaxUplinkImplSz(port->ps),
                                     copyBreak);
         pkt = Pkt_ReleaseOrComplete(pkt);
         ASSERT(pkt == NULL); // will complete when pktMapped is released
      } else {
         /*
          * don't bother with the partial copy mess if the packet
          * is small.
          */
         LOG(10, "full copy of %u byte packet", PktGetFrameLen(pkt));
         pktMapped = Vmxnet2VMKDevPktAlloc(port, vcd, copyBreak);
         if (pktMapped != NULL) {
            status = Pkt_CopyBytesOut(pktMapped->frameVA, 
                                      PktGetFrameLen(pkt),
                                      0,
                                      pkt);
            if (status == VMK_OK) {
               uint32 padLen = copyBreak - PktGetFrameLen(pkt);
               memset(pktMapped->frameVA + PktGetFrameLen(pkt), 0, padLen);
               PktSetFrameLen(pktMapped,
                              MAX(PktGetFrameLen(pkt), MIN_TX_FRAME_LEN));
            } else {
               Pkt_Release(pktMapped);
            }
         }

         // done with the original now
         Vmxnet2VMKDevTxCompleteOne(port, vcd, pkt);
         Pkt_Release(pkt);
         Vmxnet2VMKDevPostIntr(port, vcd);
      }

      if (pktMapped != NULL) {
         LOG(10, "sending %u byte pkt", PktGetFrameLen(pktMapped));
         Port_ClientStatInc(&port->clientStats.pktsTxOK, 1);
         Port_ClientStatInc(&port->clientStats.bytesTxOK, PktGetFrameLen(pktMapped));
         PktList_AddToTail(pktList, pktMapped);
      } else {
         status = VMK_NO_RESOURCES;
         LOG(0, "can't map headers or can't copy packet");
         Port_ClientStatInc(&port->clientStats.droppedTx, 1);
      }
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Vmxnet2VMKDev_TX --
 *
 *      Transmit packets on a vmxnet2 port.
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
Vmxnet2VMKDev_Tx(Net_PortID portID)
{
   Port *port;
   VMK_ReturnStatus status;

   status = Portset_GetPort(portID, &port);
   if (status == VMK_OK) {
      Vmxnet2_ClientData *vcd = (Vmxnet2_ClientData *)port->impl.data;
      PktList txList[1];
      
      PktList_Init(txList);

      SP_Lock(&vcd->lock);
      status = Vmxnet2VMKDevPollTxRing(port, vcd, txList);
      SP_Unlock(&vcd->lock);

      LOG(5, "pulled %u pkts from ring: %s", 
          PktList_Count(txList), VMK_ReturnStatusToString(status));

      // transmit anything we pulled from the tx ring
      if (PktList_Count(txList) != 0) {
         Port_Input(port, txList);
      }

      Portset_ReleasePort(port);
   } else {
      LOG(0, "failed to get port %u", portID);
   }
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Vmxnet2VMKDevRx --
 *
 *      Receive packets on a vmxnet2 port.
 *
 * Results: 
 *	returns a VMK_ReturnStatus.
 *
 * Side effects:
 *	Also polls the device's tx ring.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus 
Vmxnet2VMKDevRx(Port *port, IOChainData iocd, struct PktList *rxList)
{
   Vmxnet2_ClientData *vcd = (Vmxnet2_ClientData *)port->impl.data;
   PktHandle *pkt = PktList_GetHead(rxList);
   unsigned int pktsRx = 0;
   unsigned int bytesRx = 0;
   VMK_ReturnStatus status = VMK_OK;
   PktList txList[1];

   LOG(5, "%p: %p", port, rxList);

   SP_Lock(&vcd->lock);
   
   while (pkt != NULL) {
      Vmxnet2_RxRingEntry *rxre;
      unsigned int rxLen = 0;
      KSEG_Pair *pair;
      char *dst;

      LOG(10, "%p: %p %p %u,%u", 
          port, rxList, pkt, vcd->id.rxNICNext, vcd->id.rxNICNext2);

      rxre = Vmxnet2_GetNextRx(&vcd->id);
      if (UNLIKELY(rxre == NULL)) {
         break;
      }

      // map the guest buffer
      dst = Kseg_GetPtrFromPA(Port_GetLeaderWorld(port), 
                              rxre->paddr, pkt->bufDesc->frameLen, 
                              FALSE, &pair, &status);

      if (UNLIKELY(VMK_STRESS_DEBUG_COUNTER(NET_KSEG_FAIL))) {
         if (dst) {
            Kseg_ReleasePtr(pair);
         }
         dst = NULL;
         status = VMK_WOULD_BLOCK;
      }

      if (status == VMK_OK) {
         status = Pkt_CopyBytesOut(dst, pkt->bufDesc->frameLen, 0, pkt);
         if (status == VMK_OK) {
            rxLen = pkt->bufDesc->frameLen;
            pktsRx++;
            bytesRx += rxLen;
         }
         
         Kseg_ReleasePtr(pair);
         pkt = PktList_GetNext(rxList, pkt);
      } else {
         // we shouldn't ever see anything other than this error
         ASSERT(status == VMK_WOULD_BLOCK);
      }

      /*
       * we can't put it back since we don't know which ring
       * it came from, so if we failed to copy the data above
       * we just give it to the guest with a zero len so they 
       * can ignore it. (rxLen initialized to 0, and only set 
       * to anything else on success)
       */
      Vmxnet2_PutRx(rxre, rxLen);
   }

   Port_ClientStatInc(&port->clientStats.pktsRxOK, pktsRx);
   Port_ClientStatInc(&port->clientStats.bytesRxOK, bytesRx);

   if (PktList_Count(rxList) > pktsRx) {
      unsigned int dropped = PktList_Count(rxList) - pktsRx;
      LOG(5, "0x%x: dropped %u packets", port->portID, dropped);
      Port_ClientStatInc(&port->clientStats.droppedRx, dropped);
   }

   // take this opportunity to pull any transmits down
   PktList_Init(txList);
   Vmxnet2VMKDevPollTxRing(port, vcd, txList);

   // interrupt the guest
   Vmxnet2VMKDevPostIntr(port, vcd);

   SP_Unlock(&vcd->lock);
   
   // transmit anything we pulled from the tx ring
   if (PktList_Count(txList) != 0) {
      Port_Input(port, txList);
   }

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * Vmxnet2VMKDevTxComplete --
 *
 *      Handle tx complete notifications on a vmxnet2 port.
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
Vmxnet2VMKDevTxComplete(Port *port, IOChainData iocd, struct PktList *pktList)
{
   Vmxnet2_ClientData *vcd = (Vmxnet2_ClientData *)port->impl.data;
   PktHandle *pkt = PktList_GetHead(pktList);

   LOG(5, "%p: %p", port, pktList);

   while (pkt != NULL) {
      Vmxnet2VMKDevTxCompleteOne(port, vcd, pkt);
      PktList_Remove(pktList, pkt);
      Pkt_Release(pkt);
      pkt = PktList_GetHead(pktList);
   }

   // interrupt the guest
   SP_Lock(&vcd->lock);
   Vmxnet2VMKDevPostIntr(port, vcd);
   SP_Unlock(&vcd->lock);

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Vmxnet2VMKDevDisable --
 *
 *      Disable a vmxnet2 port.
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
Vmxnet2VMKDevDisable(Port *port, Bool force)
{
   VMK_ReturnStatus status = VMK_OK;
   Vmxnet2_ClientData *vcd;
   
   LOG(1, "portID 0x%x", port->portID);

   // we need to be protected from further rx, see comment below
   ASSERT(!Port_IsOutputActive(port));

   // but we need to be able to flush tx queues
   ASSERT(Port_IsInputActive(port));

   if (port->impl.data != NULL) {
      vcd = (Vmxnet2_ClientData *)port->impl.data;

      if (vcd->ddMapped != (VA)NULL) {
         PktList txList[1];
         Vmxnet2_DriverData *dd = 
            (Vmxnet2_DriverData *)(vcd->ddMapped + vcd->ddOffset);

         PktList_Init(txList);
         SP_Lock(&vcd->lock);
         if (!force) {
            // be nice and grab anything pending for transmit
            status = Vmxnet2VMKDevPollTxRing(port, vcd, txList);
         } else {
            // not so nice, but always effective
            Vmxnet2VMKDevCancelAllPendingTx(port, vcd);
            Vmxnet2VMKDevPostIntr(port, vcd);
         }            
         SP_Unlock(&vcd->lock);
         if (PktList_Count(txList) != 0) {
            Port_Input(port, txList);
         }

         /*
          * LOOKOUT: don't clean anything up above here because we want
          * to be called again since we still have packets pending
          */
         if (status != VMK_OK) {
            goto done;
         }

         dd->savedRxNICNext = vcd->id.rxNICNext;
         dd->savedRxNICNext2 = vcd->id.rxNICNext2;
         dd->savedTxNICNext = vcd->id.txNICNext;
         LOG(0, "saved ring indices: rxRings: %u,%u  txRing: %u",
             vcd->id.rxNICNext, vcd->id.rxNICNext2, vcd->id.txNICNext);

         KVMap_FreePages((char *)vcd->ddMapped);
      }

      SP_CleanupLock(&vcd->lock);
      Mem_Free(port->impl.data);
      Port_InitImpl(port);
   }

   IOChain_RemoveCall(&port->notifyChain, Vmxnet2VMKDevTxComplete);
   IOChain_RemoveCall(&port->outputChain, Vmxnet2VMKDevRx);

  done:

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Vmxnet2VMKDev_UpdateEthFRP --
 *
 *      Update the frame routing policy on a vmxnet2 port.
 *
 * Results: 
 *	returns a VMK_ReturnStatus.
 *
 * Side effects:
 *	Modifies the underlying portsets' frame routing policies.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
Vmxnet2VMKDev_UpdateEthFRP(Port *port, Eth_Address *unicastAddr)
{
   Eth_FRP frp;
   Vmxnet2_ClientData *vcd = (Vmxnet2_ClientData *)port->impl.data;
   Vmxnet2_DriverData *dd;

   if (vcd == NULL) {
      // nothing to do yet
      return VMK_OK;
   }

   dd = (Vmxnet2_DriverData *)(vcd->ddMapped + vcd->ddOffset);

   memcpy(&frp, &port->ethFRP, sizeof (frp));

   memcpy(&frp.outputFilter.unicastAddr, unicastAddr, sizeof(*unicastAddr));

   memcpy(&frp.outputFilter.ladrf, &dd->LADRF, sizeof(dd->LADRF));
   frp.outputFilter.flags |= ETH_FILTER_USE_LADRF;

   frp.outputFilter.flags &= ~(ETH_FILTER_UNICAST    |
                               ETH_FILTER_MULTICAST  |
                               ETH_FILTER_BROADCAST  |
                               ETH_FILTER_PROMISC);

   if (dd->ifflags & VMXNET_IFF_PROMISC) {
      frp.outputFilter.flags |= ETH_FILTER_PROMISC;
   }
   if (dd->ifflags & VMXNET_IFF_BROADCAST) {
      frp.outputFilter.flags |= ETH_FILTER_BROADCAST;
   }
   if (dd->ifflags & VMXNET_IFF_MULTICAST) {
         frp.outputFilter.flags |= ETH_FILTER_MULTICAST;
   }

   // guest driver doesn't set VMXNET_IFF_DIRECTED explicitly (but it should)
   frp.outputFilter.flags |= ETH_FILTER_UNICAST;
   
   return Port_UpdateEthFRP(port, &frp);
}


/*
 *----------------------------------------------------------------------
 *
 * Vmxnet2VMKDev_Enable --
 *
 *      Enable a vmxnet2 port.
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
Vmxnet2VMKDev_Enable(Port *port, 
                     VA ddMapped, uint32 ddLen, uint32 ddOffset, 
                     uint32 intrActionIdx)
{
   VMK_ReturnStatus status = VMK_OK;
   Vmxnet2_ClientData *vcd;
   uint32 rxRingOffset;
   uint32 rxRingOffset2;
   uint32 txRingOffset;
          
   LOG(0, "ddMapped: 0x%x, ddLen: %u, ddOffset: %u, intrActionIdx: %u",
       ddMapped, ddLen, ddOffset, intrActionIdx);

   vcd = (Vmxnet2_ClientData *)Mem_Alloc(sizeof(Vmxnet2_ClientData));
   if (vcd == NULL) {
      status = VMK_NO_MEMORY;
      goto done;
   }
   SP_InitLock("vmxnet2_client", &vcd->lock, SP_RANK_VMXNET2_CLIENT);
   port->impl.data = (void *)vcd;  
   port->impl.disable = Vmxnet2VMKDevDisable;

   /*
    * restrict the scope of the dd struct here so that we don't  
    * accidently add any code which tests a field for sanity and
    * then assumes it won't change.  Otherwise the guest could 
    * play games on another VCPU by trying to toggle the contents
    * between safe and unsafe values, possibly bypassing our
    * sanity checks.
    */
   {
      Vmxnet2_DriverData *dd = (Vmxnet2_DriverData *)(ddMapped + ddOffset);
      
      rxRingOffset = dd->rxRingOffset + ddOffset;
      rxRingOffset2 = dd->rxRingOffset2 + ddOffset;
      txRingOffset = dd->txRingOffset + ddOffset;
      vcd->id.rxRingLength = dd->rxRingLength;
      vcd->id.rxRingLength2 = dd->rxRingLength2;
      vcd->id.txRingLength = dd->txRingLength;
      vcd->id.rxNICNext = dd->savedRxNICNext;
      vcd->id.rxNICNext2 = dd->savedRxNICNext2;
      vcd->id.txNICNext = dd->savedTxNICNext;
   }

   // sanity check all the guest data we'll use
   if ((rxRingOffset > (ddLen + ddOffset)) ||
       (rxRingOffset2 > (ddLen + ddOffset)) ||
       (txRingOffset > (ddLen + ddOffset))) {
      status = VMK_BAD_PARAM;
      Warning("bad guest ring offset: %u, %u, %u", 
              rxRingOffset, rxRingOffset2, txRingOffset);
      ASSERT(FALSE);
      goto done;
   }
   if (vcd->id.rxRingLength > 
       (((ddLen + ddOffset) - rxRingOffset) / sizeof(Vmxnet2_RxRingEntry)) ||
       vcd->id.rxRingLength2 > 
       (((ddLen + ddOffset) - rxRingOffset2) / sizeof(Vmxnet2_RxRingEntry)) ||
       vcd->id.txRingLength > 
       (((ddLen + ddOffset) - txRingOffset) / sizeof(Vmxnet2_TxRingEntry))) {
      status = VMK_BAD_PARAM;
      Warning("bad guest ring length: %u, %u, %u", 
              vcd->id.rxRingLength, vcd->id.rxRingLength2, vcd->id.txRingLength);
      ASSERT(FALSE);
      goto done;
   }
   if ((vcd->id.rxNICNext >= vcd->id.rxRingLength) ||
       (vcd->id.rxNICNext2 >= vcd->id.rxRingLength2) ||
       (vcd->id.txNICNext >= vcd->id.txRingLength)) {
      status = VMK_BAD_PARAM;
      Warning("bad saved index: %u, %u, %u", 
              vcd->id.rxNICNext, vcd->id.rxNICNext2, vcd->id.txNICNext);
      ASSERT(FALSE);
      goto done;
   }

   LOG(0, "numRxBuffers %u,%u numTxBuffers %u",
       vcd->id.rxRingLength, vcd->id.rxRingLength2, vcd->id.txRingLength);
   LOG(0, "restored indices: rxRings: %u,%u  txRing: %u",
       vcd->id.rxNICNext, vcd->id.rxNICNext2, vcd->id.txNICNext);

   vcd->id.rxRingPtr = (Vmxnet2_RxRingEntry *)(ddMapped + rxRingOffset);
   vcd->id.rxRingPtr2 = (Vmxnet2_RxRingEntry *)(ddMapped + rxRingOffset2);
   vcd->id.txRingPtr = (Vmxnet2_TxRingEntry *)(ddMapped + txRingOffset);

   status = IOChain_InsertCall(&port->outputChain, 
                               IO_CHAIN_RANK_TERMINAL, 
                               Vmxnet2VMKDevRx, NULL, NULL,
                               NULL, 
                               FALSE,
                               NULL);
   if (status != VMK_OK) {
      ASSERT(FALSE);
      Warning("failed to terminate output chain: port 0x%x on %s: %s",
              port->portID, port->ps->name, VMK_ReturnStatusToString(status));
      goto done;
   }

   status = IOChain_InsertCall(&port->notifyChain, 
                               IO_CHAIN_RANK_TERMINAL, 
                               Vmxnet2VMKDevTxComplete, 
                               NULL, NULL,
                               NULL, 
                               FALSE,
                               NULL);
   if (status != VMK_OK) {
      ASSERT(FALSE);
      Warning("failed to terminate notify chain: port 0x%x on %s: %s",
              port->portID, port->ps->name, VMK_ReturnStatusToString(status));
      goto done;
   }

#ifndef ESX3_NETWORKING_NOT_DONE_YET
   status = Vmxnet2UpdateLADRF(port);
   if (status != VMK_OK) {
      goto done;
   }

   status = Vmxnet2UpdateIFF(port);
   if (status != VMK_OK) {
      goto done;
   }
#endif // ESX3_NETWORKING_NOT_DONE_YET

   vcd->intrActionIdx = intrActionIdx;

   /*
    * We may be reconnecting a device, and the guest may have tried to
    * transmit while it was disconnected, in which case the guest
    * driver has likely stopped its queue and won't ask us again to
    * transmit unless we wake it up.  We can't actually transmit
    * anything yet because the rest of the port's and portset's
    * infrastructure isn't fully initialized, and given that anything
    * in there is likely to be quite stale, we'll just toss anything
    * that accumulated while we were disconnected.
    */
   SP_Lock(&vcd->lock);
   Vmxnet2VMKDevCancelAllPendingTx(port, vcd);
   Vmxnet2VMKDevPostIntr(port, vcd);
   SP_Unlock(&vcd->lock);

   /*
    * Do this last so that we only have it if we've succeeded,
    * since our caller will unmap these for us and we don't want 
    * to double free in our disable function
    */
   vcd->ddMapped = ddMapped;
   vcd->ddLen = ddLen;
   vcd->ddOffset = ddOffset;

  done:

   if (status != VMK_OK) {
      Warning("failed to enable port 0x%x on %s: %s",  
              port->portID, port->ps->name, VMK_ReturnStatusToString(status));
   }

   return status;
}



