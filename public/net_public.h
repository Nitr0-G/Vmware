/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved.
 * -- VMware Confidential
 * **********************************************************/

/*
 * net_public.h  --
 *
 *    Publicly exported interface to vmkernel networking for
 *    clients outside the vmkernel.
 */

#ifndef _NET_PUBLIC_H_
#define _NET_PUBLIC_H_

#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMK_MODULE
#define INCLUDE_ALLOW_VMMEXT
#define INCLUDE_ALLOW_USERLEVEL
#include "includeCheck.h"

#include "vm_basic_types.h"
#include "return_status.h"
#include "vmk_basic_types.h"
#include "net_dist.h"

struct Port;
struct PktList;

#define MAX_PORTSET_NAMELEN      31
typedef char PortsetName[MAX_PORTSET_NAMELEN + 1];

void Net_Sum(void *src, int len, uint32 *sum, int *carry);

void Uplink_GetMACAddr(Net_PortID, char *);
VMK_ReturnStatus Net_RawTXOneLocked(Net_PortID, void *, uint32, uint32);

#define NET_LOG_CONTROL_PORT	6300
#define NET_DEBUGGEE_PORT	6400
#define NET_DUMPER_PORT		6500


/*
 *----------------------------------------------------------------------------
 *
 * Net_SumToChecksum --
 *
 *    Convert the sum into the equivalent checksum.
 *
 * Results:
 *    The checksum is returned.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE uint16
Net_SumToChecksum(uint32 sum)
{
   while (sum >> 16) {
      sum = (sum >> 16) + (sum & 0xffff);
   }
   return ~sum;
}

// logger and debugger stuff
typedef enum {
   NET_LOG_MSG_FETCH,
   NET_LOG_MSG_DATA,
   NET_LOG_MSG_BUSY,
} Net_LogMsgType;

typedef struct Net_LogMsgHdr {
   int32		magic;
   Net_LogMsgType	type;
   int32 		nextLogChar;
   int32		logBufferSize;
   int32		length;
   int32		override;
   uint64		bootTS;
   uint64		pad;	// Windows wants to 8-byte align things.
} Net_LogMsgHdr;

typedef enum {
   NET_DEBUG_MSG_NONE,
   NET_DEBUG_MSG_INIT,
   NET_DEBUG_MSG_SEND,
   NET_DEBUG_MSG_ACK,
   NET_DEBUG_MSG_BREAK
} Net_DebugMsgType;

typedef struct Net_DebugMsgHdr {
   uint32 		magic;
   uint32		sequenceNumber;
   Net_DebugMsgType	type;
   uint64		timestamp;

   /*
    * Used during initialization only.
    */
   uint32 		toDebuggerSequenceNumber;
   uint32		toDebuggeeSequenceNumber;   
} Net_DebugMsgHdr;

typedef enum {
   NET_DUMPER_MSG_NONE,
   NET_DUMPER_MSG_INIT,
   NET_DUMPER_MSG_DATA,
   NET_DUMPER_MSG_DUMP,
   NET_DUMPER_MSG_BREAK,
   NET_DUMPER_MSG_DUMP_AND_BREAK,
   NET_DUMPER_MSG_DONE,
} Net_DumperMsgType;

typedef struct Net_DumperMsgHdr {
   uint32 		magic;
   uint32		sequenceNumber;
   uint64		timestamp;   
   uint32		dumpID;
   Net_DumperMsgType	type;
   uint32		dataOffset;
   uint32		dataLength;
   uint32		status;
   uint32		payload;
} Net_DumperMsgHdr;

#define NET_LOG_MSG_MAGIC	0xbad1fc2
#define NET_DEBUG_MSG_MAGIC	0xefade94a
#define NET_DUMPER_MSG_MAGIC	0xadeca1bf

#define NET_DUMPER_PORT		6500
#define NET_LOG_CONTROL_PORT	6300

// everything else after here is TODO
#ifdef ESX3_NETWORKING_NOT_DONE_YET

// XXX this should move to an ethernet.h file somewhere
typedef uint8 MACAddr[6];
#define ETH_MAX_FRAME_LEN             1518
#define ETH_MIN_FRAME_LEN               60
/*
 * Some switches might strip the 4 byte tag off ETH_MIN_FRAME_LEN byte frames,
 * without taking care to pad it back again to ETH_MIN_FRAME_LEN. All tx'ed
 * packets therefore need to be at least ETH_MIN_FRAME_LEN + 4.
 */
#define MIN_TX_FRAME_LEN (ETH_MIN_FRAME_LEN + 4)

// XXX this belongs to the uplink layer
#define MAX_NET_DEVICES 32

#endif // ESX3_NETWORKING_NOT_DONE_YET

#endif // _NET_PUBLIC_H_
