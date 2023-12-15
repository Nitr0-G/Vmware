
#ifndef DEVCNX_H
#define DEVCNX_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "rpc_types.h"
#include "world_ext.h"

/*
 *  To be included at user level.
 */

/*
 *  IOCTLs
 */

#define CNXIOCS_BIND	     SIOCDEVPRIVATE + 1
#define CNXIOCS_GET_MSG	     SIOCDEVPRIVATE + 2
#define CNXIOCS_SEND_MSG     SIOCDEVPRIVATE + 3
#define CNXIOCS_SEND_REPLY   SIOCDEVPRIVATE + 4
#define CNXIOCS_GET_CNX_ID   SIOCDEVPRIVATE + 5
#define CNXIOCS_STATS_START  SIOCDEVPRIVATE + 6
#define CNXIOCS_STATS_STOP   SIOCDEVPRIVATE + 7
#define CNXIOCS_STATS_REPORT SIOCDEVPRIVATE + 8

typedef struct DevCnx_BindParamBlock {
   char			name[RPC_CNX_NAME_LENGTH];  // IN: connection name
   unsigned long 	flags;                      // IN: connection flags
} DevCnx_BindParamBlock;

#define DEVCNX_LOGGER		0x01
#define DEVCNX_OVERRIDE_CNX	0x02

typedef struct DevCnx_GetMsgParamBlock {
   int			requestID;   // IN: request ID
   Bool			blocking;    // IN: whether to block if no msg
   uint32               timeout;     // IN: timeout in msec (0 = infinite)
   unsigned long	dataLength;  // IN/OUT: length of payload
   RPC_Token		token;       // OUT: reply token
   int			function;    // OUT: message operation
   World_ID		worldID;     // OUT: ID of world that sent this message
   char			data[RPC_MAX_MSG_LENGTH];     // OUT: payload
} DevCnx_GetMsgParamBlock;

typedef struct DevCnx_SendMsgParamBlock {
   int			function;    			// IN: message operation
   unsigned long	dataLength;  			// IN: length of payload
   char			data[RPC_MAX_MSG_LENGTH];       // IN: payload   
} DevCnx_SendMsgParamBlock;

typedef struct DevCnx_SendReplyParamBlock {
   RPC_Token		token;       // IN: reply token
   unsigned long 	dataLength;  // IN: length of payload
   char			data[RPC_MAX_REPLY_LENGTH]; // IN: payload
} DevCnx_SendReplyParamBlock;

#define DEVCNX_REPLY_HDR_LEN(r) ((char *)&((r)->data[0]) - (char *)r)

/* structures used by CNX_STATS_xxx ioctls to measure RPC activity */
typedef struct CNXRPCStat {
   uint64 count;
   uint64 time;
   uint32 startSec;
   uint32 startUsec;
} CNXRPCStat;

#define CNX_STATS_NUM_RPCS 128
typedef struct CNXStat {
   RPC_Token lastToken;
   int lastIndex;
   int missed;
   int missedTime;
   long long startTime, startJiffies;
   long long time, jiffies;
   CNXRPCStat rpcStats[CNX_STATS_NUM_RPCS];
} CNXStat;


#endif
