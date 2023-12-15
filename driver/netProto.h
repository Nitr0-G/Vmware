/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. 
 * **********************************************************/


/************************************************************
 *
 *  net_proto.h
 *
 *    macros and inlines useful for handling network protocol 
 *    headers
 *
 ************************************************************/

#ifndef _NET_PROTO_H
#define _NET_PROTO_H

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_DISTRIBUTE
#define INCLUDE_ALLOW_VMK_MODULE
#include "includeCheck.h"

#define ETHER_MIN_FRAME_LEN 60
#define ETHER_ADDR_LENGTH    6
#define ETHER_HDR_LENGTH    14
#define ETH_P_IP 	       0x0800
#define ETH_P_IP_NBO           0x0008
#define VLAN_PROTO_NETORDER    0x0081
#define ETH_P_ARP	       0x0806
#define ETH_P_RARP	       0x8035

typedef struct EtherHdr {
   unsigned char	dest[ETHER_ADDR_LENGTH];
   unsigned char	source[ETHER_ADDR_LENGTH];
   unsigned short	proto;
} EtherHdr;



static inline unsigned short ip_fast_csum(unsigned char * iph,
					  unsigned int ihl) {
	unsigned int sum;

	__asm__ __volatile__("\t"
	    "movl (%1), %0"   "\n\t"
	    "subl $4, %2"     "\n\t"
	    "jbe 2f"          "\n\t"
	    "addl 4(%1), %0"  "\n\t"
	    "adcl 8(%1), %0"  "\n\t"
	    "adcl 12(%1), %0" "\n"
"1:	    adcl 16(%1), %0"  "\n\t"
	    "lea 4(%1), %1"   "\n\t"
	    "decl %2"         "\n\t"
	    "jne	1b"   "\n\t"
	    "adcl $0, %0"     "\n\t"
	    "movl %0, %2"     "\n\t"
	    "shrl $16, %0"    "\n\t"
	    "addw %w2, %w0"   "\n\t"
	    "adcl $0, %0"     "\n\t"
	    "notl %0"         "\n"
"2:\n"
	/* Since the input registers which are loaded with iph and ipl
	   are modified, we must also specify them as outputs, or gcc
	   will assume they contain their original values. */
	: "=r" (sum), "=r" (iph), "=r" (ihl)
	: "1" (iph), "2" (ihl));
	return(sum);
}

#define IP_HDR_MIN_LENGTH      20
#define IP_HDR_LENGTH(ip)      ((*(uint8 *)(ip) & 0x0f) * 4)
#define IP_PROTO_OFFSET        9
#define IP_PROTO(ip)           (*((uint8 *)(ip) + IP_PROTO_OFFSET)) 
#define TCP_HDR_MIN_LENGTH     20
#define TCP_CSUM_OFFSET        16
#define UDP_CSUM_OFFSET        6

#define IPPROTO_ICMP	1
#define IPPROTO_UDP     17 
#define IPPROTO_TCP     6

#define	ARPOP_REQUEST	1	/* ARP request.  */
#define	ARPOP_REPLY	2	/* ARP reply.    */
#define	RARPOP_REQUEST	3	/* RARP request. */
#define	RARPOP_REPLY	4	/* RARP reply.   */

#define ICMP_ECHO		8	/* Echo Request	*/
#define ICMP_ECHOREPLY		0	/* Echo Reply */
#define ICMP_DEST_UNREACH	3	/* Destination Unreachable */

typedef struct IPHdr {
   uint8	ihl:4,
                version:4;
   uint8	tos;
   uint16	tot_len;
   uint16	id;
   uint16	frag_off;
   uint8	ttl;
   uint8	protocol;
   uint16	check;
   uint32	saddr;
   uint32	daddr;
} IPHdr;

typedef struct ICMPHdr {
   uint8	type;
   uint8	code;
   uint16	checksum;
} ICMPHdr;

typedef struct ICMPEcho {
   uint16       id;
   uint16       seq;
} ICMPEcho;

typedef struct UDPHdr {
   uint16	source;
   uint16	dest;
   uint16	len;
   uint16	check;
} UDPHdr;

typedef struct TCPHdr {
   uint16	source;
   uint16	dest;
   uint32	seq;
   uint32	ack_seq;
   uint16	res1:4,
                doff:4,
                fin:1,
                syn:1,
                rst:1,
                psh:1,
                ack:1,
                urg:1,
                ece:1,
                cwr:1;
   uint16	window;
   uint16	check;
   uint16	urg_ptr;
} TCPHdr;

typedef struct PseudoHdr {
   uint32	sourceIPAddr;
   uint32	destIPAddr;
   uint8	zero;
   uint8	protocol;
   uint16	length;
} PseudoHdr;

struct ArpHdr {
   unsigned short int ar_hrd;		/* Format of hardware address.  */
   unsigned short int ar_pro;		/* Format of protocol address.  */
   unsigned char ar_hln;		/* Length of hardware address.  */
   unsigned char ar_pln;		/* Length of protocol address.  */
   unsigned short int ar_op;		/* ARP opcode (command).  */
};

typedef struct	EtherArp {
   struct ArpHdr ea_hdr;		/* fixed-size header */
   uint8 arp_sha[ETHER_ADDR_LENGTH];	/* sender hardware address */
   uint8 arp_spa[4];			/* sender protocol address */
   uint8 arp_tha[ETHER_ADDR_LENGTH];	/* target hardware address */
   uint8 arp_tpa[4];			/* target protocol address */
} EtherArp;

#endif // _NET_PROTO_H

