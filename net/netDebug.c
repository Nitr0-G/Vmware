/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/************************************************************
 *
 *  netDebug.c
 *
 ************************************************************/

#include "vmkernel.h"
#include "netProto.h"
#include "netARP.h"
#include "netDebug.h"
#include "serial.h"
#include "debug.h"
#include "dump.h"
#include "libc.h"
#include "idt.h"
#include "bh.h"
#include "log_int.h"
#include "parse.h"

#define LOGLEVEL_MODULE Net
#include "log.h"
#include "net_int.h"
#include "uplink.h"

// #define DEBUG_NET

#ifdef DEBUG_NET
   #define NETDEBUG_LOG Serial_Printf
#else
   #define NETDEBUG_LOG(...)
#endif

#define MAX_WAIT_USEC           10000000
#define MAX_LOCK_WAIT_USEC	1000000

#define MAX_NUM_RECV_BUFFERS    32
#define MAX_NUM_XMIT_BUFFERS	8

#define DEBUG_INPUT_BUFFER_LENGTH  4096
#define DEBUG_OUTPUT_BUFFER_LENGTH 1450

#define MAX_USER_DEBUGGERS	10
/*
 * Defines the index into the context array for the kernel debugger (ie, it's at
 * the end).  See below.
 */
#define KERNEL_DEBUGGER		MAX_USER_DEBUGGERS

#define MAX_DEBUG_PORTS		8

struct NetDebugContext;
void (*NetFlushBuffers)(void);

/*
 * Holds state for a debugging session.
 */
typedef struct NetDebugState {
   unsigned char        outBuffer[DEBUG_OUTPUT_BUFFER_LENGTH];
   int                  outBufferLen;

   unsigned char        inBuffer[DEBUG_INPUT_BUFFER_LENGTH];
   int                  inBufferLen;
   int                  inBufferIndex;

   int                  recvSeqNum;
   int                  sendSeqNum;
   uint64               timestamp;

   uint32		highestAck;

   SP_SpinLock		ackWaiter;
   SP_SpinLock		sendWaiter;
} NetDebugState;

/*
 * Holds state for the network logger.
 */
typedef struct NetLogState {
   uint64		lastTSC;   
   uint64		bootTS;
   SP_SpinLock		debugLock;
   SP_SpinLockIRQ	queueLock;
   int			queuePtr;
   int			queueLen;
} NetLogState;

/*
 * Contains general connection information.  The cnxState holds more specific
 * session data (ie, either NetDebugState or NetLogState).
 */
typedef struct NetDebug_Cnx {
   struct NetDebugContext* netDbgCtx;

   uint32		srcPort;
   
   unsigned char        dstMACAddr[ETHER_ADDR_LENGTH];
   uint32               dstIPAddr;
   uint32               dstPort;

   int			protocol;

   Bool			connected;

   void*		cnxState;
} NetDebug_Cnx;

typedef void (*NetPacketFunc)(NetDebug_Cnx* cnxInfo, uint8 *srcMACAddr,
			      uint32 srcIPAddr, uint32 srcUDPPort,
			      void *data, uint32 length);

/*
 * Holds information for a specific udp port.  There are currently three ports:
 * debugger, logger, dumper.  Each one has a different PacketFunc and cnxInfo.
 */
typedef struct NetDebugPortInfo {
   uint32 		port;
   NetPacketFunc	pktFunc;
   NetDebug_Cnx		cnxInfo;
} NetDebugPortInfo;

/*
 * Contains state specific to a device.  Multiple connections can be multiplexed
 * on top off this struct through the ports array.
 */
typedef struct NetDebugContext {
   Net_PortID           portID;
   
   int                  numRecvBuffers;
   int                  numXmitBuffers;
   void                 *recvBuffers[MAX_NUM_RECV_BUFFERS];
   
   PktHandle            *packet;
   
   int32                ipIDCount;

   unsigned char        srcMACAddr[ETHER_ADDR_LENGTH];
   uint32               srcIPAddr;
   
   NetARP_State         arpState;
   
   uint32               debugFlags;
   Bool			kernelDebugger;
   Bool                 netDebugStarted;

   NetDebugPortInfo*	ports[MAX_DEBUG_PORTS];
} NetDebugContext;

typedef VMK_ReturnStatus (*NetPortInitFunc)(NetDebug_Cnx* cnx);
typedef VMK_ReturnStatus (*NetPortCleanupFunc)(NetDebug_Cnx* cnx);

/*
 * Predefine port info for udp ports that we care about.
 */
typedef struct NetDebugPortTypes {
   uint32		flags;
   int			port;
   NetPacketFunc	pktFunc;
   NetPortInitFunc	initFunc;
   NetPortCleanupFunc	cleanupFunc;
} NetDebugPortTypes;

typedef struct NetDebugOpenArgs {
   uint32 		ipAddr;
   uint32		flags;
   VMnix_NetConnectArgs netConnectArgs;
} NetDebugOpenArgs;


/*
 * Indices 0 - 9 are reserved for UserWorlds;
 * Index 10 for kernel debugger/logger.
 */
static NetDebugContext netDebugContext[MAX_USER_DEBUGGERS + 1];

static Bool loggerInitialized;
static Bool loggerConnected;
static NetLogState* loggerState;
static NetDebug_Cnx* loggerCnx;
static int loggerBHNum;

/*
 * Low-level network functions.
 */
static void NetDebugOpenCB(void *a);
static VMK_ReturnStatus NetDebugHandleMsg(NetDebugContext*, char *, uint32);
static VMK_ReturnStatus NetDebugCB(Port *port, void *data, PktList *pktList);
static Bool NetDebugStart(Debug_Context* dbgCtx);
static void NetDebugStop(NetDebugContext* netDbgCtx);
static void NetDebugPoll(NetDebugContext* netDbgCtx);
static Bool NetDebugTransmit(NetDebug_Cnx* cnx, void *hdr, uint32 hdrLength,
			     void *data, uint32 dataLength);

static Bool NetDebugLockedTransmit(void*, uint32, void*, uint32, uint32, uint8*,
                                   uint32, uint32, int);
static void NetDebugWaitTimeout(void* event);

/*
 * debug.c function pointer interface.
 */
static VMK_ReturnStatus NetDebugCnxStart(Debug_Context* dbgCtx);
static VMK_ReturnStatus NetDebugListeningOn(Debug_Context* dbgCtx, char* desc,
					    int length);
static VMK_ReturnStatus NetDebugGetChar(Debug_Context* dbgCtx, unsigned char* ch);
static VMK_ReturnStatus NetDebugPutChar(Debug_Context* dbgCtx, unsigned char ch);
static VMK_ReturnStatus NetDebugFlush(Debug_Context* dbgCtx);
static VMK_ReturnStatus NetDebugCnxStop(Debug_Context* dbgCtx);
static VMK_ReturnStatus NetDebugPollChar(Debug_Context* dbgCtx, unsigned char* ch);

static Debug_CnxFunctions NetDebugCnxFunctions = {
   NetDebugCnxStart,
   NetDebugListeningOn,
   NetDebugGetChar,
   NetDebugPutChar,
   NetDebugFlush,
   NetDebugCnxStop,
   NetDebugPollChar
};

/*
 * Net debugger functions.
 */
static VMK_ReturnStatus NetDebugStateInit(NetDebug_Cnx* cnx);
static VMK_ReturnStatus NetDebugStateCleanup(NetDebug_Cnx* cnx);
static void NetDebugPktFunc(NetDebug_Cnx* cnx, uint8 *srcMACAddr,
			    uint32 srcIPAddr, uint32 srcUDPPort, void *data,
			    uint32 length);
static void NetDebugSendPacket(NetDebug_Cnx* cnx, Net_DebugMsgHdr *hdr,
			       void *data, uint32 dataLength);

/*
 * Net logger functions.
 */
static VMK_ReturnStatus NetLogStateInit(NetDebug_Cnx* cnx);
static VMK_ReturnStatus NetLogStateCleanup(NetDebug_Cnx* cnx);
static void NetLogBH(void *v);
static void NetLogPortFunc(NetDebug_Cnx* cnx, uint8 *srcMACAddr,
			   uint32 srcIPAddr, uint32 srcUDPPort,
			   void *data, uint32 length);

static NetDebugContext* NetDebugGetKernCtx(void);


static NetDebugPortTypes portTypes[] = {
   { NETDEBUG_ENABLE_LOG, NET_LOG_CONTROL_PORT, NetLogPortFunc, NetLogStateInit,
     NetLogStateCleanup },
   { NETDEBUG_ENABLE_DEBUG|NETDEBUG_ENABLE_USERWORLD, NET_DEBUGGEE_PORT,
     NetDebugPktFunc, NetDebugStateInit, NetDebugStateCleanup },
   { NETDEBUG_ENABLE_DUMP, NET_DUMPER_PORT, Dumper_PktFunc, NULL, NULL }
};

#define NETDEBUG_LOG_INDEX	0
#define NETDEBUG_DEBUG_INDEX	1
#define NETDEBUG_DUMP_INDEX	2

static INLINE NetDebugState*
DEBUGCONTEXT_TO_NETDEBUGSTATE(Debug_Context* dbgCtx)
{
   NetDebugContext* netDbgCtx = (NetDebugContext*)dbgCtx->cnxData;
   NetDebugPortInfo* port = netDbgCtx->ports[NETDEBUG_DEBUG_INDEX];
   return (NetDebugState*)port->cnxInfo.cnxState;
}

static INLINE NetDebug_Cnx*
DEBUGCONTEXT_TO_NETDEBUGCNX(Debug_Context* dbgCtx)
{
   NetDebugContext* netDbgCtx = (NetDebugContext*)dbgCtx->cnxData;
   NetDebugPortInfo* port = netDbgCtx->ports[NETDEBUG_DEBUG_INDEX];
   return (NetDebug_Cnx*)&port->cnxInfo;
}


/*
 *----------------------------------------------------------------------
 *
 * NetDebug_Init --
 *
 *      Init the network debug module.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	Some logger state is initialized.
 *
 *----------------------------------------------------------------------
 */
void
NetDebug_Init(void)
{
   loggerInitialized = FALSE;
   loggerConnected = FALSE;
   loggerState = NULL;
   loggerCnx = NULL;
   loggerBHNum = BH_Register(NetLogBH, NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * NetDebugOpenCB --
 *
 *      Timer call-back to open the debug socket.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	The debug socket is opened.
 *
 *----------------------------------------------------------------------
 */
static void
NetDebugOpenCB(void *a)
{
   VMK_ReturnStatus status;
   Port *portPtr = NULL;
   UplinkDevice *dev = NULL;
   NetDebugOpenArgs *args = (NetDebugOpenArgs *)a;
   NetDebugContext* netDbgCtx = NULL;
   int err;
   int i;
   NetRawCBData cbData;

   if (args->ipAddr == 0xffffffff) {
      goto fail;
   }

   /*
    * If we have a userworld, try to find an open context.
    */
   if (args->flags & NETDEBUG_ENABLE_USERWORLD) {
      for (i = 0; i < MAX_USER_DEBUGGERS; i++) {
         if (netDebugContext[i].portID == NET_INVALID_PORT_ID) {
	    netDbgCtx = &netDebugContext[i];
	    break;
	 }
      }

      if (netDbgCtx == NULL) {
      	 goto fail;
      }
   } else {
      netDbgCtx = &netDebugContext[KERNEL_DEBUGGER];

      /*
       * Obliterates the old instance.  Not sure if this is a good thing or not.
       */
      if (netDbgCtx->portID != 0) {
         // First clean up port handlers.
         for (i = 0; i < MAX_DEBUG_PORTS; i++) {
	    if (netDbgCtx->ports[i] != NULL) {
	       if (portTypes[i].cleanupFunc != NULL) {
	          portTypes[i].cleanupFunc(&netDbgCtx->ports[i]->cnxInfo);
	       }
	       Mem_Free(netDbgCtx->ports[i]);
	       netDbgCtx->ports[i] = NULL;
	    }
	 }

	 Net_PortDisable(netDbgCtx->portID, TRUE);
	 Net_RawDisconnect(netDbgCtx->portID);
	 netDbgCtx->portID = NET_INVALID_PORT_ID;
      }
   }   

   memset(netDbgCtx, 0, sizeof(*netDbgCtx));   
   netDbgCtx->srcIPAddr = args->ipAddr;
   netDbgCtx->debugFlags = args->flags;
   if (args->flags & NETDEBUG_ENABLE_DEBUG) {
      ASSERT(! (args->flags & NETDEBUG_ENABLE_USERWORLD));
      netDbgCtx->kernelDebugger = TRUE;
   }

   /*
    * Currently there are only three ports used: logger, debugger, dumper.
    * The logger and dumper are only used by the kernel context.
    */
   for (i = 0; i < ARRAYSIZE(portTypes); i++) {
      if (args->flags & portTypes[i].flags) {
	 NetDebugPortInfo* port =
	 		(NetDebugPortInfo*)Mem_Alloc(sizeof(NetDebugPortInfo));
	 if (port == NULL) {
	    Warning("Couldn't allocate memory for debug socket state data");
	    goto fail;
	 }
	 memset(port, 0, sizeof(port));

	 netDbgCtx->ports[i] = port;

	 port->cnxInfo.netDbgCtx = netDbgCtx;
	 port->port = portTypes[i].port;
	 port->pktFunc = portTypes[i].pktFunc;
	 if (portTypes[i].initFunc != NULL &&
	     portTypes[i].initFunc(&port->cnxInfo) != VMK_OK) {
	    Warning("Unable to initialize port state");
	    Mem_Free(port);
	    netDbgCtx->ports[i] = NULL;
	    goto fail;
	 }
      }
   }
#ifndef ESX3_NETWORKING_NOT_DONE_YET
   if (Net_FindDevice(args->netConnectArgs.name, &nicType, 
                      FALSE,  /* no netLock held yet */
                      TRUE    /* opened device only */)) {
      /* if this is a nicteaming master device */
      if (nicType == VMK_NICTEAMING_VALID_MASTER) {
         Warning("%s: netlog is not supported on nicteaming", args->openNetDevArgs.name);
         return;
      } else if (nicType == VMK_NICTEAMING_SLAVE) {
         Warning("nicteaming device %s is unavailable", args->openNetDevArgs.name);
         return;
      }
   }
   ASSERT(nicType == VMK_NICTEAMING_REGULAR_VMNIC);
#endif

   status = Net_RawConnect(args->netConnectArgs.name, &netDbgCtx->portID);
   
   if (status != VMK_OK) {
      Warning("Net_OpenDevice failed");   
      goto fail;
   }

   Portset_GetPort(netDbgCtx->portID, &portPtr);
   ASSERT(portPtr != NULL);
   
   dev = portPtr->ps->uplinkDev;

   if (dev == NULL) {
      Warning("Uplink port of %s not present", portPtr->ps->name);
      goto fail;
   }
   ASSERT(dev->flags & DEVICE_PRESENT);
   netDbgCtx->packet = Pkt_Alloc(Portset_GetMaxUplinkImplSz(portPtr->ps), 
                                 NET_MAX_PKT_SIZE);
   
   if (netDbgCtx->packet == NULL) {
      Warning("Couldn't allocate transmit packet buffer");
      goto fail;
   }

   // XXX: Do we need to do this for userworlds as well??
   err = dev->functions->getPhysicalMACAddr(dev->netDevice, netDbgCtx->srcMACAddr);

   if (err != 0) {
      Warning("Couldn't get MAC address for NIC named %s", dev->devName);
      goto fail;
   }
   Portset_ReleasePort(portPtr);
   portPtr = NULL;

   cbData.routine = NetDebugCB;
   cbData.data  = netDbgCtx;
   Net_SetRawCB(netDbgCtx->portID, &cbData);


#ifndef ESX3_NETWORKING_NOT_DONE_YET
   // XXX: Portsets don't have MAC addresses associated with them.
   // Fix this later.

   if (args->flags & NETDEBUG_ENABLE_USERWORLD) {
      /*
       * Generate a random MAC address for this UserWorld debugger.
       */
      Net_RawGetMACAddr(netDbgCtx->portID, netDbgCtx->srcMACAddr);
   } else {
      /*
       * We set the MAC address to that of the hardware.  Generating a MAC
       * address is dangerous since a conflict will prevent us from debugging
       * the vmkernel.  No one else is going to use this address so we might
       * as well.
       */
      Net_RawSetMACAddr(netDbgCtx->portID, netDbgCtx->srcMACAddr);
   }
#endif

   Log("Net_OpenDevice succeeded");      
   Mem_Free(args);   
   if (netDbgCtx->kernelDebugger == TRUE) {
      /*
       * If we just set up a kernel debugger, presumably the user wants to
       * use the network debugger, so set serial debugging to false.
       */
      Debug_SetSerialDebugging(FALSE);
   } else {
      /*
       * Likewise for userworld debuggers, if the user set one up, it
       * probably means they want debugging enabled.
       */
      Debug_UWDebuggerEnable(TRUE);
   }
   Net_PortEnable(netDbgCtx->portID);
   return;

  fail:

   Log("Net_OpenDevice failed");      

   Mem_Free(args);   

   /*
    * Something went wrong so clean things up.
    */
   if (portPtr != NULL) {
      Portset_ReleasePort(portPtr);
   }
   if (netDbgCtx != NULL) {
      if (netDbgCtx->portID != 0) {
	 Net_RawDisconnect(netDbgCtx->portID);
	 netDbgCtx->portID = NET_INVALID_PORT_ID;
      }
      for (i = 0; i < MAX_DEBUG_PORTS; i++) {
	 if (netDbgCtx->ports[i] != NULL) {
	    if (portTypes[i].cleanupFunc != NULL) {
	       portTypes[i].cleanupFunc(&netDbgCtx->ports[i]->cnxInfo);
	    }
	    Mem_Free(netDbgCtx->ports[i]);
	 }
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * NetDebug_Open --
 *
 *      Schedule the open of a network debug socket.  A timer is used
 *	because we are running as the result of a proc write 
 *	right now and we can't call any routines that don't use 
 *	IRQ locks.
 *
 * Results: 
 *	VMK_NO_RESOURCES if there is no memory available.
 *	VMK_OK otherwise.
 *
 * Side effects:
 *	A timer is set up to schedule a helper action.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
NetDebug_Open(char *name, uint32 srcAddr, uint32 flags)
{
   NetDebugOpenArgs *args;

   Warning("%s srcIP=0x%x", name, srcAddr);

   args = (NetDebugOpenArgs *)Mem_Alloc(sizeof(NetDebugOpenArgs));
   if (args == NULL) {
      Warning("Couldn't allocate memory");
      return VMK_NO_RESOURCES;
   }
   memset(args, 0, sizeof(NetDebugOpenArgs));

   args->ipAddr = srcAddr;
   args->flags = flags;
   strcpy(args->netConnectArgs.name, name);

   Timer_Add(0, (Timer_Callback) NetDebugOpenCB, 10, TIMER_ONE_SHOT, args);
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * NetDebug_Shutdown --
 *
 *      Forcibly close down a debug socket.  This function is called when
 *	we need to stop using the network debugger/logger immediately and
 *	don't want to risk/wait for a standard shutdown.  Setting the
 *	handleID to zero will result in attempts by the debugger/logger
 *	to fail and, at least in the case of the debugger, will force
 *	it to revert to a serial connection.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	The debug/logging handle ID is zeroed out.
 *
 *----------------------------------------------------------------------
 */
void
NetDebug_Shutdown(Debug_Context* dbgCtx)
{
   if (dbgCtx != NULL) {
      NetDebugContext* netDbgCtx = (NetDebugContext*)dbgCtx->cnxData;
      netDbgCtx->portID = NET_INVALID_PORT_ID;
   } else {
      int i;

      for (i = 0; i < MAX_USER_DEBUGGERS + 1; i++) {
         netDebugContext[i].portID = NET_INVALID_PORT_ID;
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * NetDebugHandleMsg --
 *
 *      Process a single pending message.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	A message may be removed from the full queue and appended
 *	to the empty queue.  Also ARP and ICMP requests are handled.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
NetDebugHandleMsg(NetDebugContext* netDbgCtx, char *frameHdr, uint32 frameLen)
{
   EtherHdr *eh;

   if ((netDbgCtx->debugFlags & NETDEBUG_ENABLE_LOG) ||
   	netDbgCtx->netDebugStarted) {
      eh = (EtherHdr *)frameHdr;
#if 0
      Warning("len=%d proto=%d dst=%02x:%02x:%02x:%02x:%02x:%02x\n"
	      "                src=%02x:%02x:%02x:%02x:%02x:%02x", 
	      frameLen, 
	      ntohs(eh->proto),
	      eh->dest[0], eh->dest[1], eh->dest[2], 
	      eh->dest[3], eh->dest[4], eh->dest[5],
	      eh->source[0], eh->source[1], eh->source[2], 
	      eh->source[3], eh->source[4], eh->source[5]);
#endif
      switch (ntohs(eh->proto)) {
      case ETH_P_ARP: {
         NetARP_ProcessARP(netDbgCtx->portID, &netDbgCtx->arpState,
			   netDbgCtx->srcIPAddr, netDbgCtx->srcMACAddr, eh);
	 break;
      }
      case ETH_P_IP: {
	 IPHdr *ip = (IPHdr *)(eh + 1);
	 switch (ip->protocol) {
	 case IPPROTO_UDP: {
	    /*
	     * Call any port handler for this UDP packet.
	     */
	    int i;
	    UDPHdr *udp = (UDPHdr *)(ip + 1);
	    for (i = 0; i < MAX_DEBUG_PORTS; i++) {
	       if (netDbgCtx->ports[i] != NULL &&
		   netDbgCtx->ports[i]->port == ntohs(udp->dest)) {
	          NetDebugPortInfo* port = netDbgCtx->ports[i];

		  port->pktFunc(&port->cnxInfo, eh->source, ntohl(ip->saddr),
		  		ntohs(udp->source), udp + 1,
				ntohs(udp->len) - sizeof(UDPHdr));

		  break;
	       }
	    }
	    break;
	 }
	 case IPPROTO_ICMP: {
	    /*
	     * We handle ICMP_ECHO and ICMP_DEST_UNREACH requests only.
	     */
	    ICMPHdr *icmp = (ICMPHdr *)(ip + 1);
	    if (icmp->type == ICMP_ECHO) {
               int carry = 0;
               uint32 sum = 0;
	       icmp->type = ICMP_ECHOREPLY;
	       icmp->code = 0;
	       icmp->checksum = 0;
	       Net_Sum((uint16 *)icmp,  ntohs(ip->tot_len) - sizeof(IPHdr),
                       &sum, &carry);
               icmp->checksum = Net_SumToChecksum(sum);


#ifdef VMX86_DEBUG
               {
                  ICMPEcho *echo = (ICMPEcho *)(icmp + 1);
                  LOG(20, "sending echo reply, id=%u, seq=%u", 
                      ntohs(echo->id), ntohs(echo->seq));
               }
#endif // VMX86_DEBUG

	       NetDebugLockedTransmit(NULL, 0, icmp,
	       		              ntohs(ip->tot_len) - sizeof(IPHdr), 0,
				      eh->source, ntohl(ip->saddr), 0, IPPROTO_ICMP);
	    } else if (icmp->type == ICMP_DEST_UNREACH) {
	       if (loggerInitialized && loggerConnected) {
		  IPHdr *tip = (IPHdr *)(((char *)(icmp + 1)) + 4);

		  if (ntohl(tip->daddr) == loggerCnx->dstIPAddr &&
			tip->protocol == IPPROTO_UDP) {
		     UDPHdr *udp = (UDPHdr *)(tip + 1);
		     if (ntohs(udp->dest) == loggerCnx->dstPort) {
			Warning("Net logger @ %d.%d.%d.%d:%d is unreachable",
			      (tip->daddr) & 0xff,
			      (tip->daddr >> 8) & 0xff,
			      (tip->daddr >> 16) & 0xff,
			      (tip->daddr >> 24) & 0xff,
			      ntohs(udp->dest));
			loggerConnected = FALSE;
#ifdef notdef
		     } else {
			Warning("Destination @ %d.%d.%d.%d:%d is unreachable",
			      (tip->daddr) & 0xff,
			      (tip->daddr >> 8) & 0xff,
			      (tip->daddr >> 16) & 0xff,
			      (tip->daddr >> 24) & 0xff,
			      ntohs(udp->dest));
#endif
		     }
		  }
	       }
	    }
	    break;
	 }
	 default:
	    break;
	 }
      }
      default:
	 break;
      }
   }

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * NetDebugCB --
 *
 *      Callback to handle incoming messages on the debug socket.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
NetDebugCB(Port *port, void *data, PktList *pktList)
{
   PktHandle *pkt = PktList_GetHead(pktList);
   while (pkt) {
      // assume for now that the pkt is completely mapped.
#ifdef ESX3_NETWORKING_NOT_DONE_YET
      NetDebugHandleMsg(data, pkt->frameVA, pkt->bufDesc->frameLen);
#else
#error need to map the entire pkt into frameVA
#endif
      pkt = PktList_GetNext(pktList, pkt);
   }
   return VMK_OK;
}



/*
 *----------------------------------------------------------------------------
 *
 * NetDebugLockedTransmit --
 *
 *    Transmit through a port that has been locked. This function is useful for
 *    any transmits that are done inline in the IOChain handler.
 *
 * Results:
 *    TRUE if the transmit was successful. FALSE otherwise.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static Bool
NetDebugLockedTransmit(void* hdr, uint32 hdrLength, void* data, uint32 dataLength,
		       uint32 srcPort, uint8* dstMACAddr, uint32 dstIPAddr,
		       uint32 dstPort, int protocol)
{
   NetDebug_Cnx cnx;

   cnx.netDbgCtx = NetDebugGetKernCtx();
   cnx.srcPort = srcPort;
   memcpy(cnx.dstMACAddr, dstMACAddr, ETHER_ADDR_LENGTH);
   cnx.dstIPAddr = dstIPAddr;
   cnx.dstPort = dstPort;
   cnx.protocol = protocol;
   cnx.cnxState = NULL;

   return NetDebugTransmit(&cnx, hdr, hdrLength, data, dataLength);
   
}

/*
 *----------------------------------------------------------------------
 *
 * NetDebug_Transmit --
 *
 *      Transmit a packet to the given destination.
 *
 * Results: 
 *	TRUE if the packet could be transmitted.
 *
 * Side effects:
 *	A packet is transmitted.
 *
 *----------------------------------------------------------------------
 */
Bool
NetDebug_Transmit(void* hdr, uint32 hdrLength, void* data, uint32 dataLength,
		  uint32 srcPort, uint8* dstMACAddr, uint32 dstIPAddr,
		  uint32 dstPort, int protocol)
{
   NetDebug_Cnx cnx;
   Port *portPtr;
   Bool ret;

   cnx.netDbgCtx = NetDebugGetKernCtx();
   cnx.srcPort = srcPort;
   memcpy(cnx.dstMACAddr, dstMACAddr, ETHER_ADDR_LENGTH);
   cnx.dstIPAddr = dstIPAddr;
   cnx.dstPort = dstPort;
   cnx.protocol = protocol;
   cnx.cnxState = NULL;

   Portset_GetPort(cnx.netDbgCtx->portID, &portPtr);
   ret = NetDebugTransmit(&cnx, hdr, hdrLength, data, dataLength);
   Portset_ReleasePort(portPtr);
   return ret;
}

/*
 *----------------------------------------------------------------------
 *
 * NetDebugTransmit --
 *
 *      Transmit a packet to the given destination.
 *
 * Results: 
 *	TRUE if the packet could be transmitted.
 *
 * Side effects:
 *	A packet is transmitted.
 *
 *----------------------------------------------------------------------
 */
static Bool
NetDebugTransmit(NetDebug_Cnx* cnx,		// High-level connection info.
		 void *hdr, 			// Packet header. 
		 uint32 hdrLength, 		// Length of packet header.
		 void *data,			// Packet data.
		 uint32 dataLength) 		// Packet data length.
{
   int protoHdrSize;
   int totalLen;
   EtherHdr *eh;
   IPHdr *iph;  
   PktHandle *pkt;
   Port *portPtr = NULL;
   NetDebugContext* netDbgCtx = cnx->netDbgCtx;
   UplinkDevice *dev;
   char netHdrBuf[64];
   int netHdrLength;

   LOG(30, "%u bytes of hdr at %p %u bytes of data at %p", 
       hdrLength, hdr, dataLength, data);

   if (netDbgCtx->portID == NET_INVALID_PORT_ID) {
      LOG(1, "netDbgCtx %p doesn't have a valid port", netDbgCtx);
      return FALSE;
   }

   if (cnx->protocol == 0) {
      cnx->protocol = IPPROTO_UDP;
   }

   switch (cnx->protocol) {
   case IPPROTO_UDP:
      protoHdrSize = sizeof(UDPHdr);
      break;
   case IPPROTO_ICMP:
      protoHdrSize = 0;
      break;
   default:
      NOT_IMPLEMENTED();
   }

   netHdrLength = sizeof(EtherHdr) + sizeof(IPHdr) + protoHdrSize;
   if (netHdrLength > sizeof(netHdrBuf)) {
       Warning("size of %d > %d", netHdrLength, sizeof(netHdrBuf));
      return FALSE;
  }

   totalLen = netHdrLength + hdrLength + dataLength;
   if (totalLen > ETH_MAX_FRAME_LEN) {
      Warning("size of %d > %d", totalLen, ETH_MAX_FRAME_LEN);
      return FALSE;
   }

   Portset_GetLockedPort(netDbgCtx->portID, &portPtr);
   if (portPtr == NULL) {
      Serial_Printf("NetDebugTransmit: can't get handle for %d .. dropping\n", 
		    netDbgCtx->portID);
      return FALSE;
   }

   pkt = Pkt_Alloc(Portset_GetMaxUplinkImplSz(portPtr->ps), NET_MAX_PKT_SIZE);

   if (pkt == NULL) {
      // fall back on our preallocated single packet, but never let this packet 
      // be free since we want to reuse it
      PktIncRefCount(netDbgCtx->packet);
      pkt = netDbgCtx->packet;
   }

   eh = (EtherHdr *)netHdrBuf;
   eh->proto = htons(ETH_P_IP);
   memcpy(eh->dest, cnx->dstMACAddr, ETHER_ADDR_LENGTH);
#ifdef ESX3_NETWORKING_NOT_DONE_YET
   memcpy(eh->source, netDbgCtx->srcMACAddr, ETHER_ADDR_LENGTH);
#else
   // XXX: MAC addresses are no longer associated with portsets
   memcpy(eh->source, handlePtr->macAddr, ETHER_ADDR_LENGTH);
#endif

   iph = (IPHdr *)(eh + 1);  
   iph->version = 4;
   iph->ihl = 5;
   iph->tos = 0;   
   iph->tot_len = htons(sizeof(IPHdr) + protoHdrSize + hdrLength + dataLength);
   iph->id = htons(netDbgCtx->ipIDCount++);   
   iph->frag_off = 0;   
   iph->ttl = 10;   
   iph->protocol = cnx->protocol;
   iph->saddr = htonl(netDbgCtx->srcIPAddr);
   iph->daddr = htonl(cnx->dstIPAddr);
   iph->check = 0;
   iph->check = ip_fast_csum((unsigned char *)iph, iph->ihl);

   switch (cnx->protocol) {
   case IPPROTO_UDP: {
      PseudoHdr ph;
      uint32 sum = 0;
      int carry = 0;
      UDPHdr *udph = (UDPHdr *)(iph + 1);
      udph->len = htons(hdrLength + dataLength + sizeof(UDPHdr));
      udph->dest = htons(cnx->dstPort);
      udph->source = htons((cnx->srcPort == 0 ? 1024 : cnx->srcPort));
      udph->check = 0;
      ph.sourceIPAddr = iph->saddr;
      ph.destIPAddr = iph->daddr;
      ph.zero = 0;
      ph.protocol = IPPROTO_UDP;
      ph.length = htons(hdrLength + dataLength + sizeof(UDPHdr));

      Net_Sum((uint16 *)&ph, sizeof(ph), &sum, &carry);
      Net_Sum((uint16 *)udph, sizeof(UDPHdr), &sum, &carry);
      if (hdr != NULL) {
         Net_Sum((uint16 *)hdr, hdrLength, &sum, &carry);
      }
      Net_Sum((uint16 *)data, dataLength, &sum, &carry);
      udph->check = Net_SumToChecksum(sum);

      break;
   }
   case IPPROTO_ICMP: {
      break;
   }
   default:
      NOT_IMPLEMENTED();
   }

   Pkt_AppendBytes(netHdrBuf, netHdrLength, pkt);
   if (hdr != NULL) {
      Pkt_AppendBytes(hdr, hdrLength, pkt);
   }
   Pkt_AppendBytes(data, dataLength, pkt);

#if 0
   {
      char pbuf[128];
      eh = (EtherHdr *)pkt->frameVA;
      snprintf(pbuf, sizeof pbuf,
              "NetDebugTransmit: "
              "Dest=%02x:%02x:%02x:%02x:%02x:%02x, "
	      "Src=%02x:%02x:%02x:%02x:%02x:%02x, len=%d\n",
	      eh->dest[0], eh->dest[1], eh->dest[2],
	      eh->dest[3], eh->dest[4], eh->dest[5],
	      eh->source[0], eh->source[1], eh->source[2],
	      eh->source[3], eh->source[4], eh->source[5],
	      totalLen);
      Serial_PutString(pbuf);
   }
#endif
   dev = portPtr->ps->uplinkDev;

   // XXX: No reason for uw debugger to do something different
   if (dev && (dev->flags & DEVICE_PRESENT)) {
      VMK_ReturnStatus status;
      PktList tmpList;
      PktList_Init(&tmpList);
      PktSetSrcPort(pkt, netDbgCtx->portID);
      PktList_AddToTail(&tmpList, pkt);
      status = dev->functions->startTx(dev->netDevice, &tmpList);
      PktList_ReleaseAll(&tmpList);
#ifdef VMX86_DEBUG
      if (LOGLEVEL() >= 1) {
	 if (status == VMK_NO_RESOURCES) {
	    Serial_PutString("NetDebugTransmit: no resources for packet\n");
	 }
      }
#endif
   } else {
      Pkt_Release(pkt);
   }

   return TRUE;
}


/*
 *----------------------------------------------------------------------
 *  
 * NetDebugWaitTimeout --
 *
 *	Wakes up a waiting thread.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
NetDebugWaitTimeout(void* event)
{
   CpuSched_Wakeup((uint32)event);
}


/*
 *----------------------------------------------------------------------
 *
 * debug.c function pointer interface.
 *
 *---------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 *  
 * NetDebug_DebugCnxInit --
 *
 *	Initialize data for this debugger context.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
NetDebug_DebugCnxInit(Debug_Context* dbgCtx)
{
   dbgCtx->functions = &NetDebugCnxFunctions;

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *  
 * NetDebugCnxStart --
 *
 *	Attaches this debugging instance to a network debugger context.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
NetDebugCnxStart(Debug_Context* dbgCtx)
{
   if (NetDebugStart(dbgCtx)) {
      return VMK_OK;
   } else {
      return VMK_FAILURE;
   }
}


/*
 *----------------------------------------------------------------------
 *  
 * NetDebugListeningOn --
 *
 *	Returns a string with the ip address that this debugger is
 *	listening on.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
NetDebugListeningOn(Debug_Context* dbgCtx, char* desc, int length)
{
   NetDebugContext* netDbgCtx = (NetDebugContext*)dbgCtx->cnxData;
   uint32 ipAddr;
   
   if (netDbgCtx == NULL) {
      return VMK_BAD_PARAM;
   }

   if (netDbgCtx->portID == NET_INVALID_PORT_ID) {
      return VMK_INVALID_HANDLE;
   }

   ipAddr = netDbgCtx->srcIPAddr;
   
   snprintf(desc, length, "network port @ %d.%d.%d.%d",
   	    (ipAddr >> 24) & 0xff, (ipAddr >> 16) & 0xff, (ipAddr >> 8) & 0xff,
	    ipAddr & 0xff);

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *  
 * NetDebugPutChar --
 *
 *	Puts a character on the network buffer.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
NetDebugPutChar(Debug_Context* dbgCtx, unsigned char ch)
{
   NetDebugState* dbgState = DEBUGCONTEXT_TO_NETDEBUGSTATE(dbgCtx);

   if (dbgState->outBufferLen == DEBUG_OUTPUT_BUFFER_LENGTH) {
      Serial_PutString("NetDebugPutChar: Buffer full\n");
      return VMK_LIMIT_EXCEEDED;
   }

   dbgState->outBuffer[dbgState->outBufferLen++] = ch;

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *  
 * NetDebugFlush --
 *
 *	Flushs the network buffer to the network.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
NetDebugFlush(Debug_Context* dbgCtx)
{
   Net_DebugMsgHdr hdr;
   NetDebugContext* netDbgCtx = (NetDebugContext*)dbgCtx->cnxData;
   NetDebug_Cnx* cnx = DEBUGCONTEXT_TO_NETDEBUGCNX(dbgCtx);
   NetDebugState* dbgState = DEBUGCONTEXT_TO_NETDEBUGSTATE(dbgCtx);

   dbgState->outBuffer[dbgState->outBufferLen] = 0;

   NETDEBUG_LOG("NetDebugFlush:%s:\n", dbgState->outBuffer);

   if (cnx->connected == FALSE) {
      Serial_PutString("NetDebugFlush: No debugger so dropping\n");
      dbgState->outBufferLen = 0;
      return VMK_FAILURE;
   }

   hdr.magic = NET_DEBUG_MSG_MAGIC;
   hdr.sequenceNumber = dbgState->sendSeqNum++;
   hdr.timestamp = dbgState->timestamp;
   hdr.type = NET_DEBUG_MSG_SEND;

   if (netDbgCtx->kernelDebugger) {
      uint64 resendTSC = 0;
      uint64 start = RDTSC();

      /*
       * Since we can't block, we have to spin constantly checking if any
       * interesting packets arrived.  If nothing arrives after 1 second, resend
       * the packet.  If nothing is received after 10 secs (MAX_WAIT_USEC), then
       * give up.
       */
      while (RDTSC() - start < MAX_WAIT_USEC * cpuMhzEstimate) {
	 uint64 curTSC = RDTSC();
	 if (curTSC > resendTSC) {
	    if (resendTSC != 0) {
	       NETDEBUG_LOG("Retry ... sn: %d\n", hdr.sequenceNumber);      
	    }

	    NetDebugSendPacket(cnx, &hdr, dbgState->outBuffer,
			       dbgState->outBufferLen);
	    resendTSC = curTSC + 1000000 * cpuMhzEstimate;
	 }

	 NetDebugPoll(netDbgCtx);

	 if (dbgState->highestAck == hdr.sequenceNumber) {
	    NETDEBUG_LOG("NetDebugFlush: Packet was acked, sn: %d\n",
			 hdr.sequenceNumber);
	    break;
	 }
      }
   } else {
      int retries;

      for (retries = 0; retries < 5; retries++) {
	 Timer_Handle th;

	 NetDebugSendPacket(cnx, &hdr, dbgState->outBuffer,
			    dbgState->outBufferLen);

	 th = Timer_Add(myPRDA.pcpuNum, (Timer_Callback) NetDebugWaitTimeout,
                        1000, TIMER_ONE_SHOT, (void*)&dbgState->ackWaiter);
	 CpuSched_Wait((uint32)&dbgState->ackWaiter, CPUSCHED_WAIT_NET, NULL);
	 Timer_Remove(th);

	 SP_Lock(&dbgState->ackWaiter);
	 if (dbgState->highestAck == hdr.sequenceNumber) {
            SP_Unlock(&dbgState->ackWaiter);
	    break;
	 }
         SP_Unlock(&dbgState->ackWaiter);
      }
   }

   dbgState->outBufferLen = 0;

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *  
 * NetDebugGetChar --
 *
 *	Gets a character from the network buffer.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
NetDebugGetChar(Debug_Context* dbgCtx, unsigned char* ch)
{
   NetDebugContext* netDbgCtx = (NetDebugContext*)dbgCtx->cnxData;
   NetDebugState* dbgState = DEBUGCONTEXT_TO_NETDEBUGSTATE(dbgCtx);

   if (dbgCtx->kernelDebugger) {
      while (dbgState->inBufferLen == 0) {
         /*
	  * Serial debugging could have been set by a call to
	  * 'vmkdebug wantserial'.  This means that we need to abort network
	  * debugging and revert to serial.
	  */
         if (Debug_CheckSerial()) {
	    return VMK_WAIT_INTERRUPTED;
	 }
         NetDebugPoll(netDbgCtx);
      }
   } else {
      SP_Lock(&dbgState->sendWaiter);

      if (dbgState->inBufferLen == 0) {
	 CpuSched_Wait((uint32)&dbgState->sendWaiter, CPUSCHED_WAIT_NET,
		       &dbgState->sendWaiter);
         SP_Lock(&dbgState->sendWaiter);

	 ASSERT(dbgState->inBufferLen > 0);
      }
   }

   *ch = dbgState->inBuffer[dbgState->inBufferIndex++];

   if (dbgState->inBufferIndex == dbgState->inBufferLen) {
      dbgState->inBufferIndex = dbgState->inBufferLen = 0;
   }

   if (!dbgCtx->kernelDebugger) {
      SP_Unlock(&dbgState->sendWaiter);
   }

   NETDEBUG_LOG("NetDebugGetChar returning :%c:\n", *ch);

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *  
 * NetDebugCnxStop --
 *
 *	Disconnects this debugger from it's network context.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
NetDebugCnxStop(Debug_Context* dbgCtx)
{
   if (dbgCtx->cnxData == NULL) {
      return VMK_BAD_PARAM;
   }

   NetDebugStop((NetDebugContext*)dbgCtx->cnxData);
   dbgCtx->cnxData = NULL;

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NetDebugPollChar --
 *
 * 	Check whether a character is available and return it if so
 * 	(character 0 is returned if nothing is available)
 *
 * Results:
 * 	VMK_FAILURE if not called for kernel debugger or if serial
 * 	is becoming active
 * 	VMK_OK otherwise
 *
 * Side Effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
NetDebugPollChar(Debug_Context* dbgCtx, unsigned char *ch)
{
   NetDebugContext* netDbgCtx = (NetDebugContext*)dbgCtx->cnxData;
   NetDebugState* dbgState = DEBUGCONTEXT_TO_NETDEBUGSTATE(dbgCtx);

   if (dbgCtx->kernelDebugger) { // Only valid for kernel debugger
      if (dbgState->inBufferLen == 0) {
         /*
          * Serial debugging could have been set by a call to
          * 'vmkdebug wantserial'.  This means that we need to abort network
          * debugging and revert to serial.
          */
         if (Debug_CheckSerial()) {
	    return VMK_FAILURE; // Caller has to check serial input now
         }
         NetDebugPoll(netDbgCtx);
      }

      if (dbgState->inBufferLen != 0) {
         *ch = dbgState->inBuffer[dbgState->inBufferIndex++];
         if (dbgState->inBufferIndex == dbgState->inBufferLen) {
            dbgState->inBufferIndex = dbgState->inBufferLen = 0;
         }
         NETDEBUG_LOG("NetDebugPollChar returning :%c:\n", *ch);
	 return VMK_OK;
      } else {
	 *ch = 0;
	 NETDEBUG_LOG("NetDebugPollChar not returning any char\n");
	 return VMK_OK;
      }
   }
   return VMK_FAILURE;
}


/*
 *----------------------------------------------------------------------
 *
 * Net Debugger Functions
 *
 *----------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 *
 * NetDebugStateInit --
 *
 *	Initialize the debugger state for a new connection.
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
NetDebugStateInit(NetDebug_Cnx* cnx)
{
   NetDebugState* dbgState = (NetDebugState*)Mem_Alloc(sizeof(NetDebugState));
   if (dbgState == NULL) {
      return VMK_NO_RESOURCES;
   }
   memset(dbgState, 0, sizeof(NetDebugState));
   SP_InitLock("ack waiter", &dbgState->ackWaiter, SP_RANK_NET_DEBUG);
   SP_InitLock("send waiter", &dbgState->sendWaiter, SP_RANK_NET_DEBUG);

   cnx->dstIPAddr = 0;
   cnx->connected = FALSE;
   cnx->srcPort = NET_DEBUGGEE_PORT;
   cnx->protocol = IPPROTO_UDP;
   cnx->cnxState = (void*)dbgState;

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NetDebugStateCleanup --
 *
 *	Frees resources used by the debugger.
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
NetDebugStateCleanup(NetDebug_Cnx* cnx)
{
   NetDebugState* dbgState = (NetDebugState*)cnx->cnxState;
   ASSERT(dbgState != NULL);

   SP_CleanupLock(&dbgState->ackWaiter);
   SP_CleanupLock(&dbgState->sendWaiter);

   Mem_Free(dbgState);
   cnx->cnxState = NULL;

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *  
 * NetDebugSerialize --
 *
 *	Converts a string into a gdb-readable form. len is the length of
 *	the output buffer (which will be twice the length of the input
 *	buffer).
 *
 * Results:
 *	The number of characters in the output buffer.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
NetDebugSerialize(char* out, char* in, int len)
{
   const char hexchars[]="0123456789abcdef";
   int i;
   unsigned char ch;

   for (i = 0; *in != 0 && i < len; i += 2) {
      ch = (unsigned char)*in++;

      *out++ = hexchars[ch >> 4];
      *out++ = hexchars[ch % 16];
   }

   *out = 0;

   return i;
}


/*
 *----------------------------------------------------------------------
 *  
 * NetDebugPktFunc --
 *
 *	Parses and handles incoming packets.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
NetDebugPktFunc(NetDebug_Cnx* cnx, uint8 *srcMACAddr, uint32 srcIPAddr,
		uint32 srcUDPPort, void *data, uint32 length)
{
   NetDebugContext* netDbgCtx = cnx->netDbgCtx;
   NetDebugState* dbgState = (NetDebugState*)cnx->cnxState;
   Net_DebugMsgHdr *hdr = (Net_DebugMsgHdr *)data;

   NETDEBUG_LOG("Got packet from port %d length %d type %d sn %d\n", 
	        srcUDPPort, length, hdr->type, hdr->sequenceNumber);

   if (length < sizeof(Net_DebugMsgHdr)) {
      Serial_PutString("Too short\n");
      return;
   }

   if (hdr->magic != NET_DEBUG_MSG_MAGIC) {
      Serial_PutString("Bad magic\n");
      return;
   }

   switch (hdr->type) {
   case NET_DEBUG_MSG_INIT:
      NETDEBUG_LOG("NET_DEBUG_MSG_INIT\n");   

      /*
       * Check if this is a fresh connection or a reconnection from the same
       * machine.  Refer to Bug 26542.  'connected' is reset to false upon a
       * clean exit from the debugger (ie, calling NetDebugStop).
       */
      if (cnx->connected == FALSE || cnx->dstIPAddr == srcIPAddr) {
	 cnx->connected = TRUE;
	 NETDEBUG_LOG("NET_DEBUG_MSG_INIT: !Dup\n"); 

	 dbgState->timestamp = hdr->timestamp;
	 dbgState->recvSeqNum = hdr->toDebuggeeSequenceNumber + 1;
	 dbgState->sendSeqNum = hdr->toDebuggerSequenceNumber;
	 dbgState->inBufferLen = 0;
	 dbgState->outBufferLen = 0;
	 dbgState->highestAck = 0;
	 memcpy(cnx->dstMACAddr, srcMACAddr, ETHER_ADDR_LENGTH);
	 cnx->dstIPAddr = srcIPAddr;
	 cnx->dstPort = srcUDPPort;

	 hdr->type = NET_DEBUG_MSG_ACK;
	 NetDebugSendPacket(cnx, hdr, NULL, 0);
      } else {
         char buffer[250];
	 char out[500];
	 int len;
	 uint32 ip = cnx->dstIPAddr;

	 /*
	  * XXX: Broken!  GDB won't accept this message until we ack its 'init'
	  * message.  Need to figure out a better way to respond when a second
	  * user is trying to connect to a debugging session. -kit
	  */

	 snprintf(buffer, sizeof(buffer),
	    "ERROR: Another debugger already connected at ip=%3d.%3d.%3d.%3d\n",
	          ip >> 24, (ip >> 16) & 0xff00, (ip >> 8) & 0xffff00,
		  ip & 0xffffff00);

	 len = NetDebugSerialize(out + 1, buffer, 500);
	 out[0] = 'O';

	 hdr->sequenceNumber = 200000;
	 hdr->type = NET_DEBUG_MSG_SEND;
	 NetDebugSendPacket(cnx, hdr, out, len + 1);
      }
      break;
   case NET_DEBUG_MSG_ACK:
      NETDEBUG_LOG("Ack for %d\n", hdr->sequenceNumber);
      
      if (netDbgCtx->kernelDebugger) {
         if (hdr->sequenceNumber > dbgState->highestAck) {
	    dbgState->highestAck = hdr->sequenceNumber;
	 }
      } else {
         SP_Lock(&dbgState->ackWaiter);
         if (hdr->sequenceNumber > dbgState->highestAck) {
	    dbgState->highestAck = hdr->sequenceNumber;
	    CpuSched_Wakeup((uint32)&dbgState->ackWaiter);
	 }
	 SP_Unlock(&dbgState->ackWaiter);
      } 

      break;
   case NET_DEBUG_MSG_SEND:
      NETDEBUG_LOG("NET_DEBUG_MSG_SEND\n");

      if (!netDbgCtx->kernelDebugger) {
         SP_Lock(&dbgState->sendWaiter);
      }

      if (hdr->sequenceNumber >= dbgState->recvSeqNum) {
	 int len = length - sizeof(Net_DebugMsgHdr);

	 if (dbgState->inBufferLen + len > DEBUG_INPUT_BUFFER_LENGTH) {
	    Serial_PutString("Input buffer full - dropping message\n");
	 } else {
	    memcpy(dbgState->inBuffer + dbgState->inBufferLen, hdr + 1, len);
	    dbgState->inBufferLen += len;
	    dbgState->recvSeqNum = hdr->sequenceNumber + 1;

	    if (!netDbgCtx->kernelDebugger) {
	       CpuSched_Wakeup((uint32)&dbgState->sendWaiter);
	    }
	 }
      }

      if (!netDbgCtx->kernelDebugger) {
         SP_Unlock(&dbgState->sendWaiter);
      }

      NETDEBUG_LOG("NET_DEBUG_MSG_SEND: Ack for %d\n", hdr->sequenceNumber);

      hdr->type = NET_DEBUG_MSG_ACK;
      NetDebugSendPacket(cnx, hdr, NULL, 0);
      break;
   case NET_DEBUG_MSG_BREAK:
      NETDEBUG_LOG("NET_DEBUG_MSG_BREAK\n");

      if (hdr->sequenceNumber >= dbgState->recvSeqNum) {
	 if (netDbgCtx->debugFlags & NETDEBUG_ENABLE_DEBUG) {
	    /*
	     * If we get a want breakpoint over the network, we infer this to
	     * mean that the user wants to debug over the network as well.  So
	     * set serial debugging to false.
	     */
	    Debug_SetSerialDebugging(FALSE);
	    IDT_WantBreakpoint();
	 }
	 /*
	  * Nothing to do if this is a userworld net debug context.
	  */
      }
      hdr->type = NET_DEBUG_MSG_ACK;
      NetDebugSendPacket(cnx, hdr, NULL, 0);
      break;
   case NET_DEBUG_MSG_NONE:
      break;
   }
}


/*
 *----------------------------------------------------------------------
 *  
 * NetDebugSendPacket --
 *
 *	Writes a packet to the network.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
NetDebugSendPacket(NetDebug_Cnx* cnx, Net_DebugMsgHdr *hdr,
		   void *data, uint32 dataLength)
{
   Port *portPtr;
   if (dataLength > sizeof(Net_DebugMsgHdr)) {
      NETDEBUG_LOG("Sending packet :%s:\n", data + sizeof(Net_DebugMsgHdr));
   }

   NETDEBUG_LOG("NetDebugTransmit: type=%d len=%d sn=%d\n",
	         hdr->type, dataLength, hdr->sequenceNumber);

   Portset_GetPort(cnx->netDbgCtx->portID, &portPtr);
   if (!NetDebugTransmit(cnx, hdr, sizeof(*hdr), data, dataLength)) {
      Serial_PutString("NetDebugSendPacket: NetDebugTransmit failed\n");
   }
   Portset_ReleasePort(portPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * Net Logger Functions
 *
 *----------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 *
 * NetLogStateInit --
 *
 *	Initialize the logger state for a new connection.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
NetLogStateInit(NetDebug_Cnx* cnx)
{
   NetLogState* logState;
   
   if (loggerInitialized == TRUE) {
      Warning("Only one kernel logger supported at this time!");
      return VMK_LIMIT_EXCEEDED;
   }
   
   logState = (NetLogState*)Mem_Alloc(sizeof(NetLogState));
   if (logState == NULL) {
      return VMK_NO_RESOURCES;
   }
   memset(logState, 0, sizeof(NetLogState));

   logState->bootTS = RDTSC();
   SP_InitLock("debugLock", &logState->debugLock, SP_RANK_NET_DEBUG);
   SP_InitLockIRQ("queueLock", &logState->queueLock, SP_RANK_NET_LOG_QUEUE);
   logState->queuePtr = -1;

   cnx->srcPort = NET_LOG_CONTROL_PORT;
   cnx->protocol = IPPROTO_UDP;
   cnx->cnxState = (void*)logState;

   loggerInitialized = TRUE;
   loggerState = logState;
   loggerCnx = cnx;

   return VMK_OK;
}
   

/*
 *----------------------------------------------------------------------
 *
 * NetLogStateCleanup --
 *
 *	Free resources used by logger.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
NetLogStateCleanup(NetDebug_Cnx* cnx)
{
   NetLogState* logState = (NetLogState*)cnx->cnxState;
   ASSERT(loggerInitialized == TRUE);
   ASSERT(logState != NULL);

   SP_CleanupLock(&logState->debugLock);
   SP_CleanupLockIRQ(&logState->queueLock);

   Mem_Free(logState);
   cnx->cnxState = NULL;

   loggerInitialized = FALSE;
   loggerConnected = FALSE;
   loggerState = NULL;
   loggerCnx = NULL;

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NetLogBH --
 *
 *      Bottom half handler for the net logger queue.  We use a bottom
 *      half for this to avoid lock ranking issues, since log messages 
 *      can be generated with pretty much any lock in the system held.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	All queued log messages are sent
 *
 *----------------------------------------------------------------------
 */
static void 
NetLogBH(void *v)
{
   int nextLogChar;
   int length;
   int sent = 0;
   int sendSize = ETH_MAX_FRAME_LEN - 100;
   NetLogState* logState = (NetLogState*)loggerCnx->cnxState;

   SP_LockIRQ(&logState->queueLock, SP_IRQL_KERNEL);
   nextLogChar = logState->queuePtr;
   logState->queuePtr = -1;
   length = logState->queueLen;
   SP_UnlockIRQ(&logState->queueLock, SP_GetPrevIRQ(&logState->queueLock));   

   while (sent < length) {
      if (sendSize > (length - sent)) {
         sendSize = (length - sent);
      }
      Log_SendMore(nextLogChar + sent, sendSize);
      sent += sendSize;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * NetLog_Queue --
 *
 *      Queue a log message to be sent in a bh to the logger.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	A log message is queued.
 *
 *----------------------------------------------------------------------
 */
void
NetLog_Queue(int nextLogChar, uint32 length)
{
   if (loggerInitialized && loggerConnected) {
      SP_LockIRQ(&loggerState->queueLock, SP_IRQL_KERNEL);
      if(loggerState->queuePtr == -1) {
         loggerState->queuePtr = nextLogChar;
         loggerState->queueLen = length;
      } else {
         loggerState->queueLen += length;
      }
      BH_SetLocalPCPU(loggerBHNum);
      SP_UnlockIRQ(&loggerState->queueLock,
		   SP_GetPrevIRQ(&loggerState->queueLock));
   }
}

/*
 *----------------------------------------------------------------------
 *
 * NetLog_Send --
 *
 *      Send a log message to the logger.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	A log message is sent.
 *
 *----------------------------------------------------------------------
 */
void
NetLog_Send(int nextLogChar, void *data, uint32 length)
{
   if (loggerInitialized && loggerConnected) {
      Net_LogMsgHdr msg;
      Port *port;

      msg.type = NET_LOG_MSG_DATA;
      msg.nextLogChar = nextLogChar;
      msg.length = length;
      msg.logBufferSize =  VMK_LOG_BUFFER_SIZE;
      msg.bootTS = loggerState->bootTS;
      if (Portset_GetPort(loggerCnx->netDbgCtx->portID, &port) == VMK_OK) {
         NetDebugTransmit(loggerCnx, &msg, sizeof(msg), data, length);
         Portset_ReleasePort(port);
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * NetLogPortFunc --
 *
 *      Handle messages to the log port.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	logger state may be changed.
 *
 *----------------------------------------------------------------------
 */
static void
NetLogPortFunc(NetDebug_Cnx* cnx, uint8 *srcMACAddr, uint32 srcIPAddr,
	       uint32 srcUDPPort, void *data, uint32 length)
{
   Net_LogMsgHdr *hdr;
   NetLogState* logState = (NetLogState*)cnx->cnxState;

   if (length < sizeof(Net_LogMsgHdr)) {
      return;
   }

   hdr = (Net_LogMsgHdr *)data;
   if (hdr->magic != NET_LOG_MSG_MAGIC) {
      Warning("Bad magic number");
      return;
   }

   if (hdr->type == NET_LOG_MSG_FETCH) {
      SP_Lock(&logState->debugLock);
      if (memcmp(cnx->dstMACAddr, srcMACAddr, ETHER_ADDR_LENGTH) == 0 &&
	  cnx->dstIPAddr == srcIPAddr && cnx->dstPort == srcUDPPort) {
	 /*
	  * This is the same guy that talked to us last time.  Send him anything
	  * else that we have after what he already has.
	  */
	 logState->lastTSC = RDTSC();
	 SP_Unlock(&logState->debugLock);
	 Log_SendMore(hdr->nextLogChar, ETH_MAX_FRAME_LEN - 100);
      } else if (!hdr->override && cnx->dstIPAddr != 0 &&
	         RDTSC() - logState->lastTSC < MAX_WAIT_USEC * cpuMhzEstimate) {
	 /*
	  * This is a different guy than the one that talked to use last time.
	  * We will return a busy error reply to the caller until enough
	  * time has passed since we last heard from the other logger.
	  */
	 Net_LogMsgHdr msg;
	 uint32 destIPAddr = cnx->dstIPAddr;

	 SP_Unlock(&logState->debugLock);

	 Warning("Busy with %02x:%02x:%02x:%02x:%02x:%02x", 
                 cnx->dstMACAddr[0], cnx->dstMACAddr[1], cnx->dstMACAddr[2], 
                 cnx->dstMACAddr[3], cnx->dstMACAddr[4], cnx->dstMACAddr[5]);

	 msg.type = NET_LOG_MSG_BUSY;

	 NetDebugLockedTransmit(&msg, sizeof(msg), &destIPAddr, sizeof(destIPAddr),
	 	  	        0, srcMACAddr, srcIPAddr, srcUDPPort, IPPROTO_UDP);
      } else {
	 /*
	  * This is a log message request from a new logger.  Record his
	  * address and send him everything that we got.
	  */
	 Warning("DestMAC=%02x:%02x:%02x:%02x:%02x:%02x"
		 "DestIP=%d.%d.%d.%d destPort=%d",
		 srcMACAddr[0], srcMACAddr[1], srcMACAddr[2],
		 srcMACAddr[3], srcMACAddr[4], srcMACAddr[5],
		 (srcIPAddr) & 0xff,
		 (srcIPAddr >> 8) & 0xff,
		 (srcIPAddr >> 16) & 0xff,
		 (srcIPAddr >> 24) & 0xff,
		 srcUDPPort);

	 memcpy(cnx->dstMACAddr, srcMACAddr, ETHER_ADDR_LENGTH);
	 cnx->dstIPAddr = srcIPAddr;
	 cnx->dstPort = srcUDPPort;
	 logState->lastTSC = RDTSC();
	 loggerConnected = TRUE;

	 SP_Unlock(&logState->debugLock);

	 Log_SendMore(-1, ETH_MAX_FRAME_LEN - 100);
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * NetIsDebugHandle --
 *
 *      Return TRUE is this handle is the handle used for 
 *	debugging/logging.
 *
 * Results: 
 *	TRUE if this handle is the handle used for debugging/logging.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
Bool
NetIsDebugHandle(Port *port)
{
   /*
    * Assume the caller wants to use the kernel debugger context.
    */
   NetDebugContext* netDbgCtx = NetDebugGetKernCtx();
   return port->portID == netDbgCtx->portID;
}

/*
 *----------------------------------------------------------------------
 *
 * NetDebug_ProcPrint --
 *
 *      Print out proc information about the debug socket.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	*page and *len are updated.
 *
 *----------------------------------------------------------------------
 */
void
NetDebug_ProcPrint(char *page, int *len)
{
   int i;
   Bool userworlds = FALSE;
   NetDebugContext* kernCtx;

   for (i = 0; i < MAX_USER_DEBUGGERS; i++) {
      if (netDebugContext[i].portID != 0) {
         userworlds = TRUE;
	 Proc_Printf(page, len, "DebugSocket               UserWorld @ %d.%d.%d.%d\n",
	       (netDebugContext[i].srcIPAddr >> 24) & 0xff,
	       (netDebugContext[i].srcIPAddr >> 16) & 0xff,
	       (netDebugContext[i].srcIPAddr >> 8) & 0xff,
	       (netDebugContext[i].srcIPAddr) & 0xff);
      }
   }

   if (!userworlds) {
      Proc_Printf(page, len, "DebugSocket               UserWorld Closed\n");
   } else if (!Debug_UWDebuggerIsEnabled()) {
      Proc_Printf(page, len, "       --->               UserWorld debugging DISABLED\n");
   }

   kernCtx = NetDebugGetKernCtx();
   if (kernCtx->portID == NET_INVALID_PORT_ID) {
      Proc_Printf(page, len, "DebugSocket               vmkernel Closed\n");
   } else {
      Proc_Printf(page, len, "DebugSocket               vmkernel @ %d.%d.%d.%d\n",
		  (kernCtx->srcIPAddr >> 24) & 0xff,
		  (kernCtx->srcIPAddr >> 16) & 0xff,
		  (kernCtx->srcIPAddr >> 8) & 0xff,
		  (kernCtx->srcIPAddr) & 0xff);
      if (loggerInitialized && loggerConnected) {
	 Proc_Printf(page, len, 
		             "                          logger   @ %d.%d.%d.%d:%d\n", 
		     (loggerCnx->dstIPAddr >> 24) & 0xff,
		     (loggerCnx->dstIPAddr >> 16) & 0xff,
		     (loggerCnx->dstIPAddr >> 8) & 0xff,
		     (loggerCnx->dstIPAddr) & 0xff,
	             loggerCnx->dstPort);
      }
      Proc_Printf(page, len,  "                          flags:%s%s%s\n",
		  kernCtx->debugFlags & NETDEBUG_ENABLE_DEBUG ?
		  	"DEBUG " : " ",
		  kernCtx->debugFlags & NETDEBUG_ENABLE_DUMP ?
		  	"DUMP " : " ",
		  kernCtx->debugFlags & NETDEBUG_ENABLE_LOG ?
		  	"LOG" : " ");
   }
}


/*
 *----------------------------------------------------------------------
 *
 * NetDebug_Start --
 *
 *	See NetDebugStart.
 *
 * Results: 
 *	None
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
Bool
NetDebug_Start(void)
{
   Debug_Context dbgCtx;
   dbgCtx.kernelDebugger = TRUE;

   return NetDebugStart(&dbgCtx);
}


/*
 *----------------------------------------------------------------------
 *
 * NetDebugStart --
 *
 *      Try to start debugging using the network debug socket.
 *
 * Results: 
 *	TRUE if we can use the socket, FALSE otherwise.
 *
 * Side effects:
 *	May disable interrupts for the network card its using.
 *
 *----------------------------------------------------------------------
 */
Bool
NetDebugStart(Debug_Context* dbgCtx)
{
   Port *portPtr = NULL;
   NetDebugContext* netDbgCtx = NULL;

   if (dbgCtx->kernelDebugger) {
      UplinkDevice *dev;
      NetDebugContext* kernCtx = NetDebugGetKernCtx();

      if (!(kernCtx->debugFlags & NETDEBUG_ENABLE_DEBUG) ||
	  kernCtx->portID == NET_INVALID_PORT_ID) {
         goto fail;
      }

      if (kernCtx->netDebugStarted) {
         Warning("Kernel debugger already opened.\n");
	 return TRUE;
      }
      netDbgCtx = kernCtx;

#ifndef ESX3_NETWORKING_NOT_DONE_YET
      /*
       * Try to lock the network device.  If we can't lock it, then we probably
       * took a fault in the networking code.
       */
      startTSC = RDTSC();
      while (1) {
	 NetTryHandle(netDbgCtx->handleID, &handlePtr, NET_DEV_WRITE_LOCK);
	 if (handlePtr == NULL) {
	    if (RDTSC() - startTSC > MAX_LOCK_WAIT_USEC * cpuMhzEstimate) {
	       Serial_Printf("NetDebugStart: device write locked\n");
	       goto fail;
	    }
	 } else {
	    break;
	 }
      }
#else
      // For now, just acquire the lock...
      Portset_GetPort(netDbgCtx->portID, &portPtr);
      dev = portPtr->ps->uplinkDev;
      if (dev && (dev->flags & DEVICE_PRESENT)) {
         /*
          * Prevent interrupts from disturbing the driver.
          */
         IDT_VectorDisable(dev->uplinkData.intrHandlerVector, IDT_VMK);
         IDT_VectorSync(dev->uplinkData.intrHandlerVector);
      } else {
         goto fail;
      }
      Portset_ReleasePort(portPtr);
      portPtr = NULL;
#endif
      netDbgCtx->netDebugStarted = TRUE;
   } else {
      int i;

      for (i = 0; i < MAX_USER_DEBUGGERS; i++) {
	 if (netDebugContext[i].portID != 0 &&
	     netDebugContext[i].netDebugStarted == FALSE) {
	    netDbgCtx = &netDebugContext[i];
	    netDbgCtx->netDebugStarted = TRUE;
	    break;
	 }
      }

      if (netDbgCtx == NULL) {
         Warning("No open UserWorld debugger IP addresses found!\n"
	         " use \"echo \'DebugSocket 172.16.23.xxx UserWorld\' >> /proc/vmware/net/vmnic0/config\"");
         goto fail;
      }
   }

   dbgCtx->cnxData = (void*)netDbgCtx;

   return TRUE;

  fail:
   if (portPtr) {
      Portset_ReleasePort(portPtr);
   }
   return FALSE;
}


/*
 *----------------------------------------------------------------------
 *
 * NetDebug_Stop --
 *
 *	See NetDebugStop.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
void
NetDebug_Stop(void)
{
   NetDebugStop(NetDebugGetKernCtx());
}


/*
 *----------------------------------------------------------------------
 *
 * NetDebugStop --
 *
 *      Stop debugging using the network debug socket.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	May reenable driver interrupts for the net card it was using.
 *
 *----------------------------------------------------------------------
 */
void
NetDebugStop(NetDebugContext* netDbgCtx)
{
   if (!netDbgCtx->netDebugStarted) {
      return;
   }

   if (netDbgCtx->kernelDebugger) {
      UplinkDevice *dev = NULL;
      Port *portPtr = NULL;
      Portset_GetPort(netDbgCtx->portID, &portPtr);

      ASSERT(portPtr != NULL);

      dev = portPtr->ps->uplinkDev;
      if (dev && (dev->flags & DEVICE_PRESENT)) {
         /*
          * Re-enable interrupts for the driver.
          */
         IDT_VectorEnable(dev->uplinkData.intrHandlerVector, IDT_VMK);
      }
      Portset_ReleasePort(portPtr);
   }

   netDbgCtx->netDebugStarted = FALSE;
}

/*
 *----------------------------------------------------------------------
 *
 * NetDebug_Poll --
 *
 *	See NetDebugPoll.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
void
NetDebug_Poll(void)
{
   NetDebugPoll(NetDebugGetKernCtx());
}

/*
 *----------------------------------------------------------------------
 *
 * NetDebugPoll --
 *
 *      Poll the network device by simulating an interrupt.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	An interrupt is given to the network device.
 *
 *----------------------------------------------------------------------
 */
void
NetDebugPoll(NetDebugContext* netDbgCtx)
{
   Port *portPtr = NULL;
   UplinkDevice *dev = NULL;
   static int ctr = 0; 

   Portset_GetPort(netDbgCtx->portID, &portPtr);
   ASSERT(portPtr != NULL);

   dev = portPtr->ps->uplinkDev;
   if (ctr < 10) {
      Log("In NetDebugPoll: %p", dev->uplinkData.intrHandler);
      ctr++;
   }
          
   dev->uplinkData.intrHandler(dev->uplinkData.intrHandlerData,
                               dev->uplinkData.intrHandlerVector);
   Portset_ReleasePort(portPtr);
   NetFlushBuffers();
}

/*
 *----------------------------------------------------------------------
 *
 * NetDebugGetKernCtx --
 *
 *	Returns a pointer to the kernel's NetDebugContext.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static NetDebugContext*
NetDebugGetKernCtx(void)
{
   return &netDebugContext[KERNEL_DEBUGGER];
}

/*
 *----------------------------------------------------------------------
 *
 * NetDebug_ARP --
 *
 *      Sends an arp request, if ip Addr isn't already in the cache.
 *
 * Results:
 *	TRUE if a mac address was obtained.
 *
 * Side effects:
 *	May modify arp cache.
 *
 *----------------------------------------------------------------------
 */

Bool
NetDebug_ARP(uint32 ipAddr, uint8 *macAddr)
{
   NetDebugContext* netDbgCtx = NetDebugGetKernCtx();
   Port *port;
   VMK_ReturnStatus status = VMK_FAILURE;
   Portset_GetPort(netDbgCtx->portID, &port);
   if (port) {
      status = NetARP_GetMACFromIP(netDbgCtx->portID, &netDbgCtx->arpState,
                        	   ipAddr, macAddr, netDbgCtx->srcIPAddr,
                                   netDbgCtx->srcMACAddr);
      Portset_ReleasePort(port);
   }

   if (status == VMK_OK) {
       return TRUE;
   }

   return FALSE;
}

void
NetDummyFlushBuffers(void)
{
}


int
NetDebug_ProcWrite(Proc_Entry *entry, char *page, int *lenp)
{
   char *argv[3];
   int argc = Parse_Args(page, argv, 3);
   char *psName = (char *)entry->private;
   ASSERT(psName);

   if (strcmp(argv[0], "DebugSocket") == 0)  {
      /*
       * DebugSocket now applies to UserWorld debugging.
       *
       * The old format still holds for kernel debugging:
       *  echo "DebugSocket 172.16.23.xxx Now" >> /proc/vmware/net/vmnic0/config
       *
       * New format for UserWorlds:
       *  echo "DebugSocket 172.16.23.xxx UserWorld" >> ...
       * You can define up to 10 UserWorld ip's.  When a UserWorld breaks into
       * the debugger, it will use the next available ip.  If none are left, it
       * will simply coredump and exit.
       *
       * Now there is a global for enabling/disabling UserWorld debuggers:
       *  echo "DebugSocket Disable UserWorld" >> ..   or
       *  echo "DebugSocket Enable UserWorld" >> ..
       * UserWorld debuggers are implicitly enabled whenever you add a new
       * UserWorld debugger ip.
       */
      uint32 flags = 0;
      if (argc > 3) {
	 Warning("DebugSocket called with %d args", argc);	 
	 return VMK_BAD_PARAM;
      } else {
	 uint32 ipAddr;
	 if (argc == 3) {
	    if (strcmp(argv[2], "Now") == 0 || strcmp(argv[2], "now") == 0) {
	       flags = NETDEBUG_ENABLE_LOG | NETDEBUG_ENABLE_DEBUG |
		       NETDEBUG_ENABLE_DUMP;
	    } else if (strcmp(argv[2], "DebugOnly") == 0 ||
		       strcmp(argv[2], "debugonly") == 0) {
	       flags = NETDEBUG_ENABLE_DEBUG | NETDEBUG_ENABLE_DUMP;
	    } else if (strcmp(argv[2], "LogOnly") == 0 ||
		       strcmp(argv[2], "logonly") == 0) {
	       flags = NETDEBUG_ENABLE_LOG | NETDEBUG_ENABLE_DUMP;
	    } else if (strcmp(argv[2], "UserWorld") == 0 ||
		       strcmp(argv[2], "userworld") == 0) {
	       flags = NETDEBUG_ENABLE_USERWORLD;
	    } else {
	       Warning("Unknown option %s to DebugSocket.  Expected \"Now\", "
		       "\"DebugOnly\", \"LogOnly\", or \"UserWorld\"", argv[2]);
	       return VMK_BAD_PARAM;
	    }
	 }
	 if (flags & NETDEBUG_ENABLE_USERWORLD) {
	    if (strcmp(argv[1], "Disable") == 0 ||
	    	strcmp(argv[1], "disable") == 0) {
	       Debug_UWDebuggerEnable(FALSE);
	       return VMK_OK;
	    } else if (strcmp(argv[1], "Enable") == 0 ||
	    	       strcmp(argv[1], "enable") == 0) {
	       Debug_UWDebuggerEnable(TRUE);
	       return VMK_OK;
	    }
	 }
	 ipAddr = Net_GetIPAddr(argv[1]);
	 if (ipAddr == 0) {
	    Warning("Invalid IP address");
	    return VMK_BAD_PARAM;
	 } else if (NetDebug_Open(psName, ipAddr, flags) != VMK_OK) {
	    Warning("NetDebug_Open failed");
	    return VMK_BAD_PARAM;
	 }
      }
   } else if (strcmp(argv[0], "DumpIPAddr") == 0)  {
      if (argc > 2) {
	 Warning("DumpIPAddr called with %d args", argc);	 
	 return VMK_BAD_PARAM;
      } else {
	 uint32 ipAddr = Net_GetIPAddr(argv[1]);
	 if (ipAddr == 0) {
	    Warning("NetDebugOpen: Invalid IP address");
	    return VMK_BAD_PARAM;
	 }
	 Dump_SetIPAddr(ipAddr);
      }
   } else {
      LOG(0, "Invalid option \"%s\"", argv[0]);
      return VMK_BAD_PARAM;
   }

   return VMK_OK;
}


int
NetDebug_ProcRead(Proc_Entry *entry, char *page, int *len)
{
   *len = 0;
   NetDebug_ProcPrint(page, len);      
   if (Dump_GetIPAddr() != 0) {
      uint32 ipAddr = Dump_GetIPAddr();
      Proc_Printf(page, len, "Dumper:                   netdumper @ %d.%d.%d.%d\n",
		  (ipAddr >> 24) & 0xff, (ipAddr >> 16) & 0xff,
                  (ipAddr >> 8) & 0xff, (ipAddr) & 0xff);
   }

   return(VMK_OK);
}

