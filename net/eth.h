/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. 
 * -- VMware Confidential
 * **********************************************************/

/*
 * eth.h  --
 *
 *    Virtual ethernet.
 */

#ifndef _ETH_H_
#define _ETH_H_

#include "vm_libc.h"
#include "libc.h"

typedef uint8 Eth_Address[6];

// printf helpers
#define ETH_ADDR_FMT_STR     "%02x:%02x:%02x:%02x:%02x:%02x"
#define ETH_ADDR_FMT_ARGS(a) ((uint8 *)a)[0], ((uint8 *)a)[1], ((uint8 *)a)[2], \
                             ((uint8 *)a)[3], ((uint8 *)a)[4], ((uint8 *)a)[5]

// DIX type fields we care about
enum {
   ETH_TYPE_IP      = 0x0800,  
   ETH_TYPE_ARP     = 0x0806,  
   ETH_TYPE_RARP    = 0x8035,
   ETH_TYPE_802_1PQ = 0x8100,  // not really a DIX type, but used as such
};

typedef struct Eth_DIX {
   uint16       type;  // indicates the higher level protocol
} Eth_DIX;

// XXX incomplete, but probably useless for us anyway...
typedef struct Eth_LLC {
   uint8   dsap;
   uint8   ssap;
   uint8   control;
} Eth_LLC;

typedef struct Eth_802_3 {  
   uint16       len;         // length of the frame
   Eth_LLC      llc;         // LLC header
} Eth_802_3;

// 802.1p priority tags
enum {
   ETH_802_1_P_ROUTINE      = 0,
   ETH_802_1_P_PRIORITY     = 1,
   ETH_802_1_P_IMMEDIATE    = 2,
   ETH_802_1_P_FLASH        = 3,
   ETH_802_1_P_FLASH_OVR    = 4,
   ETH_802_1_P_CRITICAL     = 5,
   ETH_802_1_P_INTERNETCTL  = 6,
   ETH_802_1_P_NETCTL       = 7
};

typedef struct Eth_802_1pq {
   uint16   type;             // always ETH_TYPE_802_1PQ
   uint16   priority:3,       // 802.1p priority tag
            canonical:1,      // bit order (should always be 0)
            vlanID:12;        // 802.1q vlan tag
   union {
      Eth_DIX      dix;       // DIX header follows
      Eth_802_3    e802_3;    // or 802.3 header follows 
   };
} Eth_802_1pq;

typedef struct Eth_Header {
   Eth_Address     dst;       // all types of ethernet frame have dst first
   Eth_Address     src;       // and the src next (at least all the ones we'll see)
   union {
      Eth_DIX      dix;       // followed by a DIX header
      Eth_802_3    e802_3;    // or an 802.3 header
      Eth_802_1pq  e802_1pq;  // or an 802.1[pq] tag and a header
   };
} Eth_Header;

typedef struct Eth_Stats {
   uint64   unicastFrames;    // frames directed at a single station
   uint64   broadcastFrames;  // frames directed to all stations
   uint64   multicastFrames;  // frames directed to a subset of all stations
} Eth_Stats;

#ifdef ESX3_NETWORKING_NOT_DONE_YET
#define ETH_LADRF_LEN 2 
#else 
#error "need common LADRF length definition"
#endif // ESX3_NETWORKING_NOT_DONE_YET

// ethernet frame filtering encapsulation
typedef struct Eth_Filter {
   enum {
      ETH_FILTER_UNICAST   = 0x0001,   // pass unicast (directed) frames
      ETH_FILTER_MULTICAST = 0x0002,   // pass some multicast frames
      ETH_FILTER_ALLMULTI  = 0x0004,   // pass *all* multicast frames
      ETH_FILTER_BROADCAST = 0x0008,   // pass broadcast frames
      ETH_FILTER_PROMISC   = 0x0010,   // pass all frames (ie no filter)
      ETH_FILTER_USE_LADRF = 0x0020    // use the LADRF for multicast filtering
   }             flags;            
   Eth_Address   unicastAddr;          // unicast addr to filter on
   uint16        numMulticastAddrs;    // number of exact multcast addrs
   Eth_Address  *multicastAddrs;       // should only use LADRF as a last resort
   uint32        ladrf[ETH_LADRF_LEN]; // lance style logical address filter 
   Eth_Stats     passed;               // frames of various types we passed
   Eth_Stats     blocked;              // frames of various types we blocked
} Eth_Filter;

// filter flags printf helpers
#define ETH_FILTER_FLAG_FMT_STR     "%s%s%s%s%s%s"
#define ETH_FILTER_FLAG_FMT_ARGS(f) (f) & ETH_FILTER_UNICAST   ? "  UNICAST"   : "", \
                                    (f) & ETH_FILTER_MULTICAST ? "  MULTICAST" : "", \
                                    (f) & ETH_FILTER_ALLMULTI  ? "  ALLMULTI"  : "", \
                                    (f) & ETH_FILTER_BROADCAST ? "  BROADCAST" : "", \
                                    (f) & ETH_FILTER_PROMISC   ? "  PROMISC"   : "", \
                                    (f) & ETH_FILTER_USE_LADRF ? "  USE_LADRF" : ""  

// ethernet Frame Routing Policy element for a given port
typedef struct Eth_FRP {
   Eth_Filter    outputFilter;   // like the rx filter on a real NIC
   Eth_Filter    inputFilter;    // to enforce additional security policies
   uint16        vlanID;         // which VLAN the port should tag/filter
} Eth_FRP;


#define ETH_BROADCAST_ADDRESS { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }

extern Eth_Address netEthBroadcastAddr;

Bool Eth_RunFilter(Eth_Filter *filter, Eth_Address *addr);


/*
 *----------------------------------------------------------------------------
 *
 * Eth_StatInc --
 *
 *    Increment the stat by inc.
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
Eth_StatInc(uint64 *stat, unsigned int inc)
{
   *stat += inc;
}


/*
 *----------------------------------------------------------------------
 *
 * Eth_IsAddrMatch --
 *
 *      Do the two ethernet addresses match?
 *
 * Results: 
 *	TRUE or FALSE.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
Eth_IsAddrMatch(Eth_Address *addr1, Eth_Address *addr2) 
{
   return !(memcmp(addr1, addr2, sizeof (Eth_Address)));
}


/*
 *----------------------------------------------------------------------
 *
 * Eth_IsUnicastAddr --
 *
 *      Is the address the broadcast address?
 *
 * Results: 
 *	TRUE or FALSE.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
Eth_IsBroadcastAddr(Eth_Address *addr) 
{
   return Eth_IsAddrMatch(addr, &netEthBroadcastAddr);
}

/*
 *----------------------------------------------------------------------
 *
 * Eth_IsUnicastAddr --
 *
 *      Is the address a unicast address?
 *
 * Results: 
 *	TRUE or FALSE.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
Eth_IsUnicastAddr(Eth_Address *addr) 
{
   // broadcast and multicast frames always have the low bit set in byte 0 
   return !(((char *)addr)[0] & 0x1);
}


/*
 *----------------------------------------------------------------------
 *
 * Eth_DestinationFilter --
 *
 *      Filter ethernet frames based on the destination address.
 *
 * Results: 
 *	Any filtered packets are returned in pktListOut.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
Eth_DestinationFilter(Eth_Filter *filter, 
                      PktList *pktListIn, PktList *pktListOut)
{
   PktHandle *nextPkt;
   PktHandle *pkt = PktList_GetHead(pktListIn);

   while (pkt != NULL) {
      Eth_Header *eh = (Eth_Header *)pkt->frameVA;

      nextPkt = PktList_GetNext(pktListIn, pkt);
      if (UNLIKELY(!Eth_RunFilter(filter, &eh->dst))) {
         PktList_Remove(pktListIn, pkt);
         PktList_AddToHead(pktListOut, pkt);
      }
      pkt = nextPkt;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * Eth_SourceFilter --
 *
 *      Filter ethernet frames based on the source address.
 *
 * Results: 
 *	Any filtered packets are returned in pktListOut.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
Eth_SourceFilter(Eth_Filter *filter, 
                      PktList *pktListIn, PktList *pktListOut)
{
   PktHandle *nextPkt;
   PktHandle *pkt = PktList_GetHead(pktListIn);

   while (pkt != NULL) {
      Eth_Header *eh = (Eth_Header *)pkt->frameVA;

      nextPkt = PktList_GetNext(pktListIn, pkt);
      if (UNLIKELY(!Eth_RunFilter(filter, &eh->src))) {
         PktList_Remove(pktListIn, pkt);
         PktList_AddToHead(pktListOut, pkt);
      }
      pkt = nextPkt;
   }
}


#endif // _ETH_H_
