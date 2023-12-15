/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. 
 * -- VMware Confidential
 * **********************************************************/

/*
 * eth.c  --
 *
 *    Virtual ethernet.
 */

#include "vmkernel.h"

#include "net_int.h"
#include "eth.h"


#define LOGLEVEL_MODULE_LEN 0
#define LOGLEVEL_MODULE Net
#include "log.h"

Eth_Address netEthBroadcastAddr = ETH_BROADCAST_ADDRESS;

/*
 *----------------------------------------------------------------------
 *
 * EthMulticastHashFilter --
 *
 *      Pass or fail the given multicast address based on the given 
 *      filter using the old lance style LADRF hashing mechanism. 
 *
 *      XXX we don't have any choice for vlance or vmxnet < ESX3,
 *          but it's dumb to not use a list of explicit multicast 
 *          addrs for future vmxnet.
 *
 * Results: 
 *	TRUE if pass, FALSE if fail.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
Bool
EthMulticastHashFilter(Eth_Filter *filter, Eth_Address *ethAddr)
{
   int32 crc, poly = 0x04c11db7;      // Ethernet CRC, big endian 
   int i, bit, byte;
   uint16 hash;
   char *addr = (char *)ethAddr;

   if (!(filter->flags & ETH_FILTER_USE_LADRF)) {
      return FALSE;
   }

   LOG(20, "compare multicast " ETH_ADDR_FMT_STR " with LADRF 0x%8x:0x%8x",
       ETH_ADDR_FMT_ARGS(ethAddr), filter->ladrf[0], filter->ladrf[1]);

   crc = 0xffffffff;                  // init CRC for each address
   for (byte=0; byte<6; byte++) {     // for each address byte 
      // process each address bit
      for (bit = *addr++,i=0;i<8;i++, bit>>=1) {
	 crc = (crc << 1) ^ ((((crc<0?1:0) ^ bit) & 0x01) ? poly : 0);
      }
   }
   hash = (crc & 1);                   // hash is 6 LSb of CRC ... 
   for (i=0; i<5; i++) {               // ... in reverse order. 
      hash = (hash << 1) | ((crc>>=1) & 1);
   }                                      

   if (((uint8 *)filter->ladrf)[hash >> 3] & (1 << (hash & 0x07))) {
      return TRUE;
   }  

   return FALSE;
}


/*
 *----------------------------------------------------------------------
 *
 * Eth_RunFilter --
 *
 *      Pass or fail the given address based on the given filter.
 *
 * Results: 
 *	TRUE if pass, FALSE if fail.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
Bool
Eth_RunFilter(Eth_Filter *filter, Eth_Address *addr)
{
   Bool passFrame = FALSE;

   if (Eth_IsUnicastAddr(addr)) {
      LOG(20, "compare unicast "ETH_ADDR_FMT_STR " with " ETH_ADDR_FMT_STR,
          ETH_ADDR_FMT_ARGS(addr), ETH_ADDR_FMT_ARGS(&filter->unicastAddr));
      if ((filter->flags & ETH_FILTER_PROMISC) ||
          Eth_IsAddrMatch(addr, &filter->unicastAddr)) {
         if (filter->flags & (ETH_FILTER_UNICAST | ETH_FILTER_PROMISC)) {
            Eth_StatInc(&filter->passed.unicastFrames, 1);
            passFrame = TRUE;
            goto done;
         }
      }
      Eth_StatInc(&filter->blocked.unicastFrames, 1);
   } else if (Eth_IsBroadcastAddr(addr)) {
      LOG(20, "broadcast " ETH_ADDR_FMT_STR, ETH_ADDR_FMT_ARGS(addr));
      if (filter->flags & (ETH_FILTER_PROMISC | ETH_FILTER_BROADCAST)) {
         Eth_StatInc(&filter->passed.broadcastFrames, 1);
         passFrame = TRUE;
         goto done;
      }
      Eth_StatInc(&filter->blocked.broadcastFrames, 1);
   } else {
      if (filter->flags & ETH_FILTER_PROMISC) {
         passFrame = TRUE;
      } else if (filter->flags & ETH_FILTER_MULTICAST) {
         unsigned int i;
         
         for (i = 0; i < filter->numMulticastAddrs; i++) {
            LOG(20, "compare multicast "ETH_ADDR_FMT_STR " with " ETH_ADDR_FMT_STR,
                ETH_ADDR_FMT_ARGS(addr), ETH_ADDR_FMT_ARGS(&filter->multicastAddrs[i]));
            if (Eth_IsAddrMatch(addr, &filter->multicastAddrs[i])) {
               passFrame = TRUE;
               break;
            }
         }
      } else if (filter->flags & ETH_FILTER_ALLMULTI) {
         passFrame = TRUE;
      }

      if (passFrame) {
         Eth_StatInc(&filter->passed.multicastFrames, 1);
      } else {
         // fall back on the LADRF hash if any
         if (!EthMulticastHashFilter(filter, addr)) {
            Eth_StatInc(&filter->blocked.multicastFrames, 1);
         } else {
            passFrame = TRUE;
            Eth_StatInc(&filter->passed.multicastFrames, 1);
         }
      }
   }

  done:

   LOG(20, "%s frame", passFrame ? "passing" : "failing");

   return passFrame;
}
