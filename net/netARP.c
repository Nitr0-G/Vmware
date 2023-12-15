/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved.
 * **********************************************************/

/************************************************************
 *
 *  netARP.c
 *
 *  Intended destination for ARP code for the vmkernel. 
 *
 ************************************************************/

#include "vm_types.h"
#include "x86.h"
#include "vm_libc.h"
#include "vmkernel.h"
#include "net.h"
#include "net_driver.h"
#include "net_public.h"
#include "netProto.h"
#include "netARP.h"

#include "libc.h"
#include "netProto.h"
#include "cpusched.h"
#include "world.h"
#include "memalloc.h"

#define LOGLEVEL_MODULE Net
#define LOGLEVEL_MODULE_LEN 3
#include "log.h"

#define IS_ARP_REQUEST(ea) (ea->ea_hdr.ar_op == htons(ARPOP_REQUEST))
#define IS_ARP_REPLY(ea) (ea->ea_hdr.ar_op == htons(ARPOP_REPLY))
#define IS_DEST_IP(ea, ipAddr) (*(uint32 *)(&ea->arp_tpa[0]) == ntohl(ipAddr))

/*
 *----------------------------------------------------------------------
 *
 * NetARP_SendRARP --
 *
 *      Sends an reverse arp request.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
NetARP_SendRARP(Net_PortID portID, uint8 *srcMACAddr)
{
   uint8 *arpRequest = Mem_Alloc(ETH_MIN_FRAME_LEN);
   VMK_ReturnStatus status = VMK_OK;
   if (arpRequest) {
      EtherHdr *reqEH;
      EtherArp *reqAH;

      memset(arpRequest, 0, sizeof(arpRequest));

      reqEH = (EtherHdr *)&arpRequest[0];
      reqEH->proto = ntohs(ETH_P_RARP);
      memset(reqEH->dest, 0xff, ETHER_ADDR_LENGTH);
      memcpy(reqEH->source, srcMACAddr, ETHER_ADDR_LENGTH);

      reqAH = (EtherArp *)(reqEH + 1);
      reqAH->ea_hdr.ar_op = htons(RARPOP_REQUEST);
      reqAH->ea_hdr.ar_hrd = htons(1);
      reqAH->ea_hdr.ar_pro = htons(ETH_P_IP);
      reqAH->ea_hdr.ar_hln = ETHER_ADDR_LENGTH;
      reqAH->ea_hdr.ar_pln = 4;
      memcpy(reqAH->arp_sha, srcMACAddr, ETHER_ADDR_LENGTH);
      memcpy(reqAH->arp_tha, srcMACAddr, ETHER_ADDR_LENGTH);

      status = Net_RawTXOneLocked(portID, arpRequest, ETH_MIN_FRAME_LEN, 0);

      Mem_Free(arpRequest);
   } else {
      status = VMK_NO_RESOURCES;
   }
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * NetARP_SendARP --
 *
 *      Sends an arp request.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
NetARP_SendARP(Net_PortID portID, uint32 srcIPAddr, uint8 *srcMACAddr,
               uint32 dstIPAddr)
{
   uint8 *arpRequest = Mem_Alloc(ETH_MIN_FRAME_LEN);
   VMK_ReturnStatus status = VMK_OK;

   if (arpRequest) {
      EtherHdr *reqEH;
      EtherArp *reqAH;
      reqEH = (EtherHdr *)&arpRequest[0];
      reqEH->proto = ntohs(ETH_P_ARP);
      memset(reqEH->dest, 0xff, ETHER_ADDR_LENGTH);
      memcpy(reqEH->source, srcMACAddr, ETHER_ADDR_LENGTH);

      reqAH = (EtherArp *)(reqEH + 1);
      reqAH->ea_hdr.ar_op = htons(ARPOP_REQUEST);
      reqAH->ea_hdr.ar_hrd = htons(1);
      reqAH->ea_hdr.ar_pro = htons(ETH_P_IP);
      reqAH->ea_hdr.ar_hln = ETHER_ADDR_LENGTH;
      reqAH->ea_hdr.ar_pln = 4;
      memcpy(reqAH->arp_sha, srcMACAddr, ETHER_ADDR_LENGTH);
      *(uint32 *)(&reqAH->arp_spa[0]) = htonl(srcIPAddr);
      memset(reqAH->arp_tha, 0, ETHER_ADDR_LENGTH);
      *(uint32 *)(&reqAH->arp_tpa[0]) = htonl(dstIPAddr);

      LOG(2, "srcIPr = 0x%x, srcMAC = %02x:%02x:%02x:%02x:%02x:%02x, dstIP = 0x%x",
          srcIPAddr, srcMACAddr[0], srcMACAddr[1], srcMACAddr[2], srcMACAddr[3], 
          srcMACAddr[4], srcMACAddr[5], dstIPAddr);

      status = Net_RawTXOneLocked(portID, arpRequest, ETH_MIN_FRAME_LEN, 0);
      Mem_Free(arpRequest);
   } else {
      status = VMK_NO_RESOURCES;
   }
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * NetARPSendReplyARP --
 *
 *      Sends a reply arp.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
NetARPSendReplyARP(Net_PortID portID, uint32 srcIPAddr, uint8 *srcMACAddr,
		   uint32 dstIPAddr, uint8 *dstMACAddr)
{
   uint8 *arpReply = Mem_Alloc(ETH_MIN_FRAME_LEN);
   VMK_ReturnStatus status = VMK_OK;

   if (arpReply) {
      EtherHdr *replyEH;
      EtherArp *replyAH;

      replyEH = (EtherHdr *)&arpReply[0];
      replyEH->proto = ntohs(ETH_P_ARP);
      memcpy(replyEH->dest, dstMACAddr, ETHER_ADDR_LENGTH);
      memcpy(replyEH->source, srcMACAddr, ETHER_ADDR_LENGTH);

      replyAH = (EtherArp *)(replyEH + 1);
      replyAH->ea_hdr.ar_op = htons(ARPOP_REPLY);
      replyAH->ea_hdr.ar_hrd = htons(1);
      replyAH->ea_hdr.ar_pro = htons(ETH_P_IP);
      replyAH->ea_hdr.ar_hln = ETHER_ADDR_LENGTH;
      replyAH->ea_hdr.ar_pln = 4;
      memcpy(replyAH->arp_sha, srcMACAddr, ETHER_ADDR_LENGTH);
      *(uint32 *)(&replyAH->arp_spa[0]) = htonl(srcIPAddr);
      memcpy(replyAH->arp_tha, dstMACAddr, ETHER_ADDR_LENGTH);
      *(uint32 *)(&replyAH->arp_tpa[0]) = htonl(dstIPAddr);

      VMLOG(2, MY_RUNNING_WORLD->worldID,
            "srcIPr = 0x%x, srcMAC = %02x:%02x:%02x:%02x:%02x:%02x, "
            "dstIP = 0x%x, dstMAC = %02x:%02x:%02x:%02x:%02x:%02x",
            srcIPAddr, srcMACAddr[0], srcMACAddr[1], srcMACAddr[2], srcMACAddr[3], 
            srcMACAddr[4], srcMACAddr[5], dstIPAddr, dstMACAddr[0], dstMACAddr[1],
            dstMACAddr[2], dstMACAddr[3], dstMACAddr[4], dstMACAddr[5]);

      status = Net_RawTXOneLocked(portID, arpReply, ETH_MIN_FRAME_LEN, 0);
      Mem_Free(arpReply);
   } else {
      status = VMK_NO_RESOURCES;
   }
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * NetARP_ProcessARP --
 *
 *	Parses the incoming ARP and responds accordingly or updates its
 *	cache.
 *
 * Results: 
 *	VMK_ReturnStatus.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
NetARP_ProcessARP(Net_PortID portID, NetARP_State* arpState, uint32 ipAddr,
		  uint8* macAddr, EtherHdr* eh)
{
   EtherArp *ea = (EtherArp *)(eh + 1);

   if (IS_ARP_REQUEST(ea) && IS_DEST_IP(ea, ipAddr)) {

      VMLOG(2, MY_RUNNING_WORLD->worldID,
	       "Arp request for our ip:\n"
	       "proto=%d dst=%02x:%02x:%02x:%02x:%02x:%02x\n"
               "         src=%02x:%02x:%02x:%02x:%02x:%02x",
               ntohs(eh->proto),
               eh->dest[0], eh->dest[1], eh->dest[2],
               eh->dest[3], eh->dest[4], eh->dest[5],
               eh->source[0], eh->source[1], eh->source[2],
               eh->source[3], eh->source[4], eh->source[5]);

      return NetARPSendReplyARP(portID, ipAddr, macAddr, (uint32)ea->arp_spa,
			        eh->source);
   } else if (IS_ARP_REPLY(ea)) {
      int i;
      int emptyIndex = -1;
      uint32 ipAddr = htonl(*(uint32 *)(&ea->arp_spa[0]));

      for (i = 0; i < ARP_CACHE_LENGTH; i++) {
	 if (arpState->cache[i].ipAddr == ipAddr) {
	    memcpy(arpState->cache[i].macAddr, ea->arp_sha, ETHER_ADDR_LENGTH);
	    break;
	 } else if (arpState->cache[i].ipAddr == 0) {
	    emptyIndex = i;
	 }
      }

      if (i == ARP_CACHE_LENGTH) {
	 if (emptyIndex == -1) {
	    uint64 tsc = RDTSC();
	    emptyIndex = tsc & (ARP_CACHE_LENGTH - 1);
	    ASSERT(emptyIndex < ARP_CACHE_LENGTH);
	 }
	 arpState->cache[emptyIndex].ipAddr = ipAddr;
	 memcpy(arpState->cache[emptyIndex].macAddr, ea->arp_sha,
		ETHER_ADDR_LENGTH);
      }
   }

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * NetARP_GetMACFromIP --
 *
 *	Attempts to find the IP in the arp cache, failing that it sends
 *	an arp request for it.
 *
 * Results: 
 *	VMK_OK if the IP is in the cache, or VMK_BUSY if it has to send
 *	out an arp request (and thus should be called again shortly).
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
NetARP_GetMACFromIP(Net_PortID portID, NetARP_State* arpState, uint32 ipAddr,
		    uint8* macAddr, uint32 srcIPAddr, uint8* srcMACAddr)
{
   int i;

   /*
    * First try to find it in our cache.
    */
   for (i = 0; i < ARP_CACHE_LENGTH; i++) {
      if (arpState->cache[i].ipAddr == ipAddr) {
         memcpy(macAddr, arpState->cache[i].macAddr, ETHER_ADDR_LENGTH);

         return VMK_OK;
      }
   }

   /*
    * There is no ARP entry, so send out an ARP request.
    */
   NetARP_SendARP(portID, srcIPAddr, srcMACAddr, ipAddr);

   return VMK_BUSY;
}

