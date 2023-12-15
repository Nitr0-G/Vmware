/* **********************************************************
 * Copyright (C) 2004 VMware, Inc. All Rights Reserved
 * - VMware Confidential
 * **********************************************************/


#include "vmkernel.h"
#include "net_int.h"
#include "net_public.h"
#include "kvmap.h"
#include "alloc_inline.h"
#include "host.h"

#include "serial.h"

#define LOGLEVEL_MODULE Net
#define LOGLEVEL_MODULE_LEN 0
#include "log.h"

#include "cos_vmkdev.h"
#include "cos_vmkdev_public.h"

typedef struct COSVMKDev_DevState {
   VA                txRing;
   VA                rxRing;
   VA                mapVP;
   COSVMKDev_State  *mapped;
   SP_SpinLock       lock;
   uint32            intrVector;
   uint32            curTxNICIdx;
   uint32            curRxNICIdx;
} COSVMKDev_DevState;


/*
 *----------------------------------------------------------------------------
 *
 * COSVMKDevPostInterrupt --
 *
 *    Post an interrupt to the COS.
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
COSVMKDevPostInterrupt(Port *port)
{
   COSVMKDev_DevState *netCosDevState = port->impl.data;
   if (netCosDevState) {
      Port_ClientStatInc(&port->clientStats.interrupts, 1);
      Host_InterruptVMnix(netCosDevState->intrVector);
   }
}


/*
 *----------------------------------------------------------------------------
 *
 * COSVMKDevTxComplete --
 *
 *    tx complete handler for ports connected to COS interfaces.
 *
 * Results:
 *    VMK_OK always.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */
 
static VMK_ReturnStatus
COSVMKDevTxComplete(Port *port, IOChainData data, PktList *pktList)
{
   PktHandle *pkt;
   COSVMKDev_DevState *netCosDevState = port->impl.data;
   COSVMKDev_TxEntry *txRing = (COSVMKDev_TxEntry *)netCosDevState->txRing;
   
   pkt = PktList_GetHead(pktList);
   while (pkt != NULL) {
      uint32 txEntryIdx;
      COSVMKDev_TxEntry *txEntry;
      PktList_Remove(pktList, pkt);
      txEntryIdx = (uint32)PktGetIOCompleteData(pkt);
      txEntry = txRing + txEntryIdx;
      ASSERT(txEntry->txState == COSVMKDEV_TX_IN_PROGRESS);
      COMPILER_MEM_BARRIER();
      txEntry->txState = COSVMKDEV_TX_DONE;
      /* DON'T TOUCH THE CURRENT txEntry AFTER THIS */

      PktClearIOCompleteData(pkt);
      Pkt_Release(pkt);
      pkt = PktList_GetHead(pktList);
   }

   LOG(3, "Posting tx complete interrupt");
   COSVMKDevPostInterrupt(port);

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------------
 *
 * COSVMKDevRx --
 *
 *    rx handler for ports to which connect to COS interfaces. Grabs a free
 *    entry in the shared rx ring, copies the rx'ed packet into the
 *    pre-allocated buffer pointed to by that entry and changes the rxState to
 *    indicate a successful packet receive.
 *
 * Results:
 *    VMK_ReturnStatus indicating the outcome.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */
 
static VMK_ReturnStatus
COSVMKDevRx(Port *port, IOChainData data, PktList *pktList)
{
   VMK_ReturnStatus status = VMK_OK;
   COSVMKDev_DevState *netCosDevState = port->impl.data;
   PktHandle *pkt;

   ASSERT(pktList);

   if (netCosDevState) {
      /* handle the case where numRxBuffers == 0 */
      SP_Lock(&netCosDevState->lock);

      LOG(2, "rxRing maddr = 0x%llx", VMK_VA2MA(netCosDevState->rxRing));
      pkt = PktList_GetHead(pktList);

      if (netCosDevState->curRxNICIdx == netCosDevState->mapped->numRxBuffers) {
         SP_Unlock(&netCosDevState->lock);
         Port_ClientStatInc(&port->clientStats.droppedRx, PktList_Count(pktList));
         return VMK_NO_MEMORY;
      }

      while (pkt) {
         COSVMKDev_RxEntry *rxEntry;
         
         rxEntry = (COSVMKDev_RxEntry *)netCosDevState->rxRing;
         rxEntry += netCosDevState->curRxNICIdx;
         if (rxEntry->rxState == COSVMKDEV_RX_AVAIL) {
            KSEG_Pair *pair;
            char *buf;
            ASSERT(pkt->bufDesc->frameLen <= rxEntry->bufLen);

            rxEntry->dataLen = 0;
            rxEntry->status = COSVMKDEV_RX_FAILED;

            buf = Kseg_GetPtrFromMA(rxEntry->maddr, pkt->bufDesc->frameLen,
                                    &pair);
            if (buf) {
               status = Pkt_CopyBytesOut(buf, pkt->bufDesc->frameLen, 0, pkt); 
               if (status == VMK_OK) {
                  rxEntry->dataLen = pkt->bufDesc->frameLen;
                  rxEntry->status = COSVMKDEV_RX_OK;

                  Port_ClientStatInc(&port->clientStats.pktsRxOK, 1);
                  Port_ClientStatInc(&port->clientStats.bytesRxOK, rxEntry->dataLen);
                  
                  LOG(2, "rxEntry: maddr = 0x%llx, idx = 0x%x, len = 0x%x",
                      rxEntry->maddr, netCosDevState->curRxNICIdx,
                      rxEntry->dataLen);

                  COMPILER_MEM_BARRIER();
                  rxEntry->rxState = COSVMKDEV_RX_USED;
                  /* *** DON't TOUCH rxEntry AFTER THIS *** */

                  if (++netCosDevState->curRxNICIdx ==
                      netCosDevState->mapped->numRxBuffers) {
                     netCosDevState->curRxNICIdx = 0;
                  }
               } else {
                  LOG(1, "Pkt_CopyBytesOut failed. buf = %p, len = 0x%x, "
                      "maddr = 0x%llx, pkt = %p", buf, pkt->bufDesc->frameLen,
                      rxEntry->maddr, pkt);
                  Port_ClientStatInc(&port->clientStats.droppedRx, 1);
               }
               Kseg_ReleasePtr(pair);
            } else {
               LOG(1, "Couldn't kseg maddr: 0x%llx, frameLen: 0x%x",
                   rxEntry->maddr, pkt->bufDesc->frameLen);
               Port_ClientStatInc(&port->clientStats.droppedRx, 1);
               status = VMK_NO_MEMORY;
            }
         } else {
            LOG(1, "Run out of available rx buffers\n");
            Port_ClientStatInc(&port->clientStats.droppedRx, 1);
            status = VMK_NO_RESOURCES;
            break;
         }
         pkt = PktList_GetNext(pktList, pkt);
      }
      SP_Unlock(&netCosDevState->lock);
   } else {
      Port_ClientStatInc(&port->clientStats.droppedRx, PktList_Count(pktList));
   }

   LOG(5, "Posting rx interrupt");
   COSVMKDevPostInterrupt(port);
   return status;
}


/*
 *----------------------------------------------------------------------------
 *
 * COSVMKDevPrepareTx --
 *
 *    Prepare a pkt for Tx'ing the given entry. A master packet is created and
 *    filled in with the sg array pointed to by the given entry. A partial copy
 *    of this packet, with headers mapped in, is then created for the actual
 *    Tx.
 *
 * Results:
 *    VMK_ReturnStatus indicating the outcome.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static VMK_ReturnStatus
COSVMKDevPrepareTx(COSVMKDev_TxEntry *txEntry, PortID portID,
                   PktList *pktList, IOData data,
                   uint32 headroom, uint32 *frameLength)
{
   VMK_ReturnStatus status = VMK_OK;
   PktHandle *pkt = NULL;
   PktHandle *txPkt = NULL;
   uint32 frameLen = 0;
   uint32 padLen = 0;
   uint32 copyLen = CONFIG_OPTION(NET_VMM_TX_COPYBREAK);
   int i;

   txEntry->status = COSVMKDEV_TX_FAILED;
   ASSERT(frameLength);
   *frameLength = 0;

   pkt = Pkt_Alloc(0, 0);
   if (!pkt) {
      status = VMK_NO_MEMORY;
      goto done;
   }

   PktSetBufType(pkt, NET_SG_MACH_ADDR);
   PktSetSrcPort(pkt, portID);
   PktSetIOCompleteData(pkt, data);
   LOG(2, "txEntry addr = 0x%x, length = 0x%x", txEntry->sg.sg[0].addrLow,
       txEntry->sg.length);

   for (i = 0; i < txEntry->sg.length; i++) { 
      status = Pkt_AppendFrag(QWORD(txEntry->sg.sg[i].addrHi, txEntry->sg.sg[i].addrLow),
                              txEntry->sg.sg[i].length, pkt);
      if (status != VMK_OK) {
         goto done;
      }
      frameLen += txEntry->sg.sg[i].length;
   }

   ASSERT(status == VMK_OK);
   // pad the packet to minimum tx length if necessary
   padLen = MAX(frameLen, MIN_TX_FRAME_LEN) - frameLen;

   if (UNLIKELY(padLen > 0)) {
      // get the remaining bytes from the zeroed runtBuffer
      status = Pkt_PadWithZeroes(pkt, padLen);
      if (status != VMK_OK) {
         goto done;
      }
   }

   PktSetFrameLen(pkt, frameLen + padLen);

   // The lower layers require a part of the frame to be mapped in
   copyLen = MIN(PktGetFrameLen(pkt), copyLen);
   txPkt = Pkt_PartialCopy(pkt, headroom, copyLen);
   if (txPkt == NULL) {
      status = VMK_NO_RESOURCES;
      goto done;
   }
   
   LOG(2, "pkt = %p, txPkt = %p, frame len = 0x%x", pkt, txPkt, frameLen);
   pkt = Pkt_ReleaseOrComplete(pkt);
   ASSERT(pkt == NULL);
   PktList_AddToTail(pktList, txPkt);
   txEntry->status = COSVMKDEV_TX_OK;
   *frameLength = frameLen;
   status = VMK_OK;

 done:
   if (pkt) {
      PktClearIOCompleteData(pkt);
      Pkt_Release(pkt);
      pkt = NULL;
   }
   return status;
}


/*
 *----------------------------------------------------------------------------
 *
 * COSVMKDev_Tx --
 *
 *    Handler for the tx vmnix-vmk call. Goes through the shared tx ring and
 *    transmits all that are ready to be sent.
 *
 * Results:
 *    VMK_ReturnStatus indicating the outcome.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

VMK_ReturnStatus
COSVMKDev_Tx(Port *port)
{
   VMK_ReturnStatus status = VMK_OK;
   PktList pktList;
   uint32 frameLen;
   COSVMKDev_TxEntry *txEntry = NULL;
   COSVMKDev_DevState *netCosDevState = NULL;

   ASSERT(port);

   PktList_Init(&pktList);

   netCosDevState = (COSVMKDev_DevState *)port->impl.data;
   if (!netCosDevState) {
      status = VMK_BAD_PARAM;
      goto done;
   }

   SP_Lock(&netCosDevState->lock);
   LOG(2, "netCosDevState->curTxNICIdx = 0x%x, "
       "netCosDevState->mapped->numTxBuffers = 0x%x, portID = 0x%x",
       netCosDevState->curTxNICIdx, netCosDevState->mapped->numTxBuffers,
       port->portID);

   if (netCosDevState->curTxNICIdx == netCosDevState->mapped->numTxBuffers) {
      goto done;
   }

   txEntry = (COSVMKDev_TxEntry *)netCosDevState->txRing;
   txEntry += netCosDevState->curTxNICIdx;
   
   while (txEntry->txState == COSVMKDEV_TX_START) {
      status = COSVMKDevPrepareTx(txEntry, port->portID, &pktList,
                                  (IOData)netCosDevState->curTxNICIdx,
                                  Portset_GetMaxUplinkImplSz(port->ps),
                                  &frameLen);

      if (status == VMK_OK) {
         Port_ClientStatInc(&port->clientStats.pktsTxOK, 1);
         Port_ClientStatInc(&port->clientStats.bytesTxOK, frameLen);

         COMPILER_MEM_BARRIER();
         txEntry->txState = COSVMKDEV_TX_IN_PROGRESS;
         /* DON'T TOUCH THE CURRENT txEntry AFTER THIS */

         if (++netCosDevState->curTxNICIdx == netCosDevState->mapped->numTxBuffers) {
            netCosDevState->curTxNICIdx = 0;
            txEntry = (COSVMKDev_TxEntry *)netCosDevState->txRing;
         } else {
            txEntry++;
         }
      } else {
         Port_ClientStatInc(&port->clientStats.droppedTx, 1);
         break;
      }
   }

done:
   if (netCosDevState) {
      SP_Unlock(&netCosDevState->lock);
   }

   if (PktList_Count(&pktList)) {
      Port_Input(port, &pktList);
   }

   return status;
}


/*
 *----------------------------------------------------------------------------
 *
 * COSVMKDevDisable --
 *
 *    VMKernel side disable handler for COS networking devices.
 *
 * Results:
 *    VMK_ReturnStatus.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static VMK_ReturnStatus
COSVMKDevDisable(Port *port, Bool force)
{
   VMK_ReturnStatus status = VMK_OK;
   if (port->impl.data) {
      COSVMKDev_DevState *netCosDevState = (COSVMKDev_DevState *)port->impl.data;
      SP_CleanupLock(&netCosDevState->lock);

      KVMap_FreePages((void *)netCosDevState->mapVP);

      Mem_Free(port->impl.data);
      port->impl.data = NULL;
      IOChain_RemoveCall(&port->notifyChain, COSVMKDevTxComplete);
      IOChain_RemoveCall(&port->notifyChain, COSVMKDevRx);
   }
   port->impl.disable = NULL;
   return status;
}



/*
 *----------------------------------------------------------------------------
 *
 * COSVMKDevEnable --
 *
 *    Setup the VMKernel side of a COS vswif interface. Initializes pointers
 *    to the shared tx and rx ring. IOChain calls are inserted to handle rx
 *    and tx complete notifications.
 *
 * Results:
 *    VMK_ReturnStatus indicating the outcome.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

VMK_ReturnStatus
COSVMKDev_Enable(Port *port, VA cosStateVA, uint32 cosStateLen, VA cosStateVP)
{
   VMK_ReturnStatus status;
   
   COSVMKDev_State *netCosState = (COSVMKDev_State *)cosStateVA;
   if (netCosState) {
      COSVMKDev_DevState *netCosDevState = Mem_Alloc(sizeof(COSVMKDev_DevState));
      if (netCosDevState) {
         SP_InitLock("console_net", &netCosDevState->lock, SP_RANK_UNRANKED);
         netCosDevState->mapVP =  cosStateVP;

         LOG(0, "port = 0x%x, cosStateVA = 0x%x, cosStateVP = 0x%x, cosStateLen=0x%x", 
             port->portID, cosStateVA, cosStateVP, cosStateLen);

         /* initialize fields owned by this module */
         netCosDevState->curTxNICIdx = 0;
         netCosDevState->curRxNICIdx = 0;
         netCosDevState->intrVector  = VMNIX_NET_INTERRUPT;
         netCosDevState->rxRing = cosStateVA + netCosState->rxRingOffset;
         netCosDevState->txRing = cosStateVA + netCosState->txRingOffset;
         netCosDevState->mapped = netCosState;

         LOG(0, "txRing = 0x%x, rxRing = 0x%x, numRxBufs = 0x%x, numTxBufs = 0x%x", 
             netCosDevState->txRing,
             netCosDevState->rxRing,
             netCosState->numRxBuffers,
             netCosState->numTxBuffers);

         port->impl.data  = netCosDevState;
         port->impl.disable = COSVMKDevDisable;
         status = IOChain_InsertCall(&port->notifyChain,
                                     IO_CHAIN_RANK_TERMINAL,
                                     COSVMKDevTxComplete, NULL, NULL, NULL,
                                     FALSE, NULL);
         if (status == VMK_OK) {
            status = IOChain_InsertCall(&port->outputChain,
                                        IO_CHAIN_RANK_TERMINAL,
                                        COSVMKDevRx, NULL, NULL, NULL,
                                        FALSE, NULL);
         }
      } else {
         LOG(0, "Couldn't allocate memory for port 0x%x", port->portID);
         status = VMK_NO_MEMORY;
      }
   } else {
      LOG(0, "Device not mapped");
      status = VMK_FAILURE;
   }
   return status;
}


/*
 *----------------------------------------------------------------------------
 *
 * COSVMKDev_UpdateEthFRP --
 *
 *    Pull down the MAC address, ifflags, and multicast adress(es) from
 *    the shared driver data, and build and install an ethernet frame
 *    routing policy on the port based on the new info.
 *
 * Results:
 *    VMK_OK on success. Error code on failure.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

VMK_ReturnStatus
COSVMKDev_UpdateEthFRP(Port *port)
{
   Eth_FRP frp;
   COSVMKDev_DevState *netCosDevState = (COSVMKDev_DevState *)port->impl.data;
   COSVMKDev_State *netCosState = (COSVMKDev_State *)netCosDevState->mapped;

   if ((netCosDevState == NULL) || (netCosState == NULL)) {
      // nothing to do yet
      return VMK_OK;
   }

   memcpy(&frp, &port->ethFRP, sizeof (frp));

   memcpy(&frp.outputFilter.unicastAddr, netCosState->macAddr, 
          sizeof(frp.outputFilter.unicastAddr));

   if (netCosState->numMulticast > 0) {
      frp.outputFilter.multicastAddrs = (Eth_Address *)&netCosState->multicastAddrs;
      if (netCosState->numMulticast <= NUM_COSVMKDEV_EXPL_MULTICAST) {
         frp.outputFilter.numMulticastAddrs = netCosState->numMulticast;
      } else {
         frp.outputFilter.numMulticastAddrs = NUM_COSVMKDEV_EXPL_MULTICAST;
         memcpy(&frp.outputFilter.ladrf, &netCosState->ladrf, 
                sizeof(frp.outputFilter.ladrf));
         frp.outputFilter.flags |= ETH_FILTER_USE_LADRF;
      }
   }

   frp.outputFilter.flags &= ~(ETH_FILTER_UNICAST    |
                               ETH_FILTER_MULTICAST  |
                               ETH_FILTER_BROADCAST  |
                               ETH_FILTER_PROMISC);

   LOG(1, "ifflags: " IFF_FMT_STR, IFF_FMT_ARGS(netCosState->ifflags));

   if (netCosState->ifflags & IFF_UP) {
      frp.outputFilter.flags |= ETH_FILTER_UNICAST;
   }
   if (netCosState->ifflags & IFF_PROMISC) {
      frp.outputFilter.flags |= ETH_FILTER_PROMISC;
   }
   if (netCosState->ifflags & IFF_BROADCAST) {
      frp.outputFilter.flags |= ETH_FILTER_BROADCAST;
   }
   if (netCosState->ifflags & IFF_MULTICAST) {
         frp.outputFilter.flags |= ETH_FILTER_MULTICAST;
   }
   if (netCosState->ifflags & IFF_ALLMULTI) {
         frp.outputFilter.flags |= ETH_FILTER_ALLMULTI;
   }
   
   return Port_UpdateEthFRP(port, &frp);
}
