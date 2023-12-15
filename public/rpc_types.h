
#ifndef _RPC_TYPES_H
#define _RPC_TYPES_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMMEXT
#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMCORE
#include "includeCheck.h"

#include "vmk_basic_types.h"
#include "rpc_calls.h"
#include "iocontrols.h"
#include "vm_atomic.h"


#define RPC_TOKEN_INVALID  (-1)
#define RPC_CNX_INVALID    (-1)

/*
 * Flags for RPC_Send and RPC_GetReply.
 */
#define RPC_CAN_BLOCK		0x01
#define RPC_REPLY_EXPECTED	0x02
#define RPC_FORCE_TOKEN         0x08
#define RPC_ALLOW_INTERRUPTIONS 0x10

#define RPC_CNX_NAME_LENGTH	32
#define RPC_MAX_MSG_LENGTH	512
#define RPC_MAX_REPLY_LENGTH	RPC_MAX_MSG_LENGTH


/* 
 * We need approximately 60 connections per VM to cover all the locks,
 * UserRPCs, and to implement barriers.
 */
#define RPC_REQUIRED_CONNECTIONS        (60 * MAX_VMS)

// RPC_MAX_CONNECTIONS must be power of 2 and >= RPC_REQUIRED_CONNECTIONS
#define RPC_MAX_CONNECTIONS             8192
#if RPC_REQUIRED_CONNECTIONS > RPC_MAX_CONNECTIONS
#error "not enough connections"
#endif

#define RPC_BITS_PER_CNX_MASK		32
#define RPC_NUM_CNX_MASKS		(RPC_MAX_CONNECTIONS / RPC_BITS_PER_CNX_MASK)

#define RPC_MIN_IRQ	0x2a
#define RPC_MAX_IRQ	0x2f

typedef struct RPC_MsgInfo {
   RPC_Token      token;
   int            function;
   void           *data;
   unsigned long  dataLength;
   World_ID       worldID;
} RPC_MsgInfo;

typedef struct RPC_CnxList {
   Atomic_uint32 masks[RPC_NUM_CNX_MASKS];
   int		 maxIndex;
} RPC_CnxList;

#define RPC_INDEX_2_MASK_BYTE(i) ((i)/RPC_BITS_PER_CNX_MASK)
#define RPC_INDEX_2_MASK_BIT(i)  (1 << ((i)%RPC_BITS_PER_CNX_MASK))

/*
 *----------------------------------------------------------------------
 *
 * RPC(Set|Clear)Mask
 *
 *      Set or clear the mask bit for the given cnxID
 *
 * Results:
 *	None
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
RPCSetMask(RPC_CnxList *list, uint32 cnxID)
{
   uint32 index = cnxID % RPC_MAX_CONNECTIONS;
   Atomic_Or(&list->masks[RPC_INDEX_2_MASK_BYTE(index)],
             RPC_INDEX_2_MASK_BIT(index));
}

static INLINE void
RPCClearMask(RPC_CnxList *list, uint32 cnxID)
{
   uint32 index = cnxID % RPC_MAX_CONNECTIONS;
   Atomic_And(&list->masks[RPC_INDEX_2_MASK_BYTE(index)],
              ~RPC_INDEX_2_MASK_BIT(index));
}

/*
 *----------------------------------------------------------------------
 *
 * RPCIsMaskSet
 *
 *      Check if the mask bit for the given cnxID is set
 *
 * Results:
 *	TRUE if set, FALSE otherwise
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
RPCIsMaskSet(RPC_CnxList *list, uint32 cnxID)
{
   uint32 index = cnxID % RPC_MAX_CONNECTIONS;
   return (Atomic_Read(&list->masks[RPC_INDEX_2_MASK_BYTE(index)]) & 
           RPC_INDEX_2_MASK_BIT(index)) != 0;
}


#endif
