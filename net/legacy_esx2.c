/* **********************************************************
 * Copyright (C) 2004 VMware, Inc. All Rights Reserved
 * -- VMware Confidential.
 * **********************************************************/


/*
 * legacy_esx2.c --
 *
 *    Provides ESX-2 style interface for code that depends on such
 *    behaviour.
 */


// CONFIG_OPTION(NET_SUPPORT_ESX2)

#include "vmkernel.h"
#include "net_int.h"
#include "legacy_esx2.h"
#include "dump.h"
#include "parse.h"
#include "debug.h"
#include "host.h"


#define LOGLEVEL_MODULE Net
#define LOGLEVEL_MODULE_LEN 0
#include "log.h"


static Proc_Entry netProcStats;

static uint32 netPktQueueLength;
static uint32 netFreePktCount;
static uint32 netOutOfPkts;
static uint32 legacySupport;

static const int VMNIC_CAP_HW_TX_VLAN = 0x0100;
static const int VMNIC_CAP_HW_RX_VLAN = 0x0200;
static const int VMNIC_CAP_SW_VLAN = 0x0400;

#define NET_PHYS_NIC_PREFIX     "vmnic"

typedef struct NetRXClusterStats {
   uint64      timerHits;
   uint64      timerPollHits;
   uint64      timerPkts;
   uint64      pollHits;
   uint64      pollPkts;
   uint64      pollTime;
   uint32      intrPollTransitions;
   uint32      devLockContention;
//#define RXC_RATE_BUCKETS
#ifdef RXC_RATE_BUCKETS
#define NUM_INTR_RATE_BUCKETS  50
#define NUM_PKT_RATE_BUCKETS   50
   uint32      intrRates[NUM_INTR_RATE_BUCKETS];
   uint32      pktRates[NUM_PKT_RATE_BUCKETS];
#endif // RXC_RATE_BUCKETS
} NetRXClusterStats;

typedef struct Net_RXClusteringCtxt {
   SP_SpinLock                  pollLock;
   uint32                       flags;
#define  NET_RXCLUSTERING_ENABLED     0x00000001       
#define  NET_RXCLUSTERING_POLLING     0x00000002    
   uint32                       lastIntrCount;     
   uint32                       lastPktCount;     
   uint32                       wrongState;  
   NetRXClusterStats            stats;
} Net_RXClusteringCtxt;


typedef struct NetDevStats_ESX2 {
   uint16            privatePktCount;
   uint16            privatePktsInUse;
   Net_Stats         stats;
} NetDevStats_ESX2;


typedef struct NetDevConfig_ESX2 {
   char                 name[VMNIX_DEVICE_NAME_LENGTH];
   int                  bus;
   int                  slot;
   int                  fn;
   int                  capabilities;
   int                  minCapabilities;
   int                  maxCapabilities;
   int                  intrHandlerVector;
   Bool                 promiscOK;
   uint32               clusterFlags;
   uint32               linkState;
   uint32               linkSpeed;
   uint32               fullDuplex;
   uint32               xmitStopped;
   NetDevStats_ESX2     devStats;
   Net_RXClusteringCtxt clustering;
} NetDevConfig_ESX2;


typedef struct NetDevProc_ESX2 {
   List_Links         links;
   Proc_Entry         devProcEntry;
   Proc_Entry         configProcEntry;
   Proc_Entry         statsProcEntry;
   Portset           *ps;
   NetDevConfig_ESX2 *config;
} NetDevProc_ESX2;


typedef struct NetDevProcList_ESX2 {
   List_Links        procList;
} NetDevProcList_ESX2;

static NetDevProcList_ESX2 procList;

static int NetProcGlobalStatsRead(Proc_Entry *, char *, int *);
static int NetProcDevStatsRead(Proc_Entry *, char *, int *);
static int NetProcDevConfigRead(Proc_Entry *, char *, int *);
static int NetProcDevConfigWrite(Proc_Entry *, char *, int *);

extern VMK_ReturnStatus Net_ConnectBondUplinkPort(char *, char *, Net_PortID *);
extern VMK_ReturnStatus Net_DisconnectBondUplinkPort(char *, char *);

/*
 *----------------------------------------------------------------------
 *
 *  NetProc_Init--
 *
 *       Sets up the /proc/vmware/net entry
 *
 * Results: 
 *	none
 *
 * Side effects:
 *	none
 *
 *----------------------------------------------------------------------
 */

void 
NetProc_Init(void)
{
   legacySupport = CONFIG_OPTION(NET_ESX2_COMPAT);

   if (legacySupport) {
      netProcStats.read = NetProcGlobalStatsRead;
      netProcStats.write = NULL;
      netProcStats.parent = ProcNet_GetRootNode();
      netProcStats.private = NULL;
      Proc_Register(&netProcStats, "stats", FALSE);

      List_Init(&procList.procList);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * NetProc_Cleanup --
 *
 *       Remove the /proc/vmware/net entry
 *
 * Results: 
 *	none
 *
 * Side effects:
 *	none
 *
 *----------------------------------------------------------------------
 */

void 
NetProc_Cleanup(void)
{
   if (legacySupport) {
      Proc_Remove(&netProcStats);
   }
}



/*
 *----------------------------------------------------------------------
 *
 * NetProcGlobalStatsRead --
 *
 *      Print out global stats about the net module.
 *
 * Results: 
 *	VKM_OK.
 *
 * Side effects:
 *	*page and *len are updated.
 *
 *----------------------------------------------------------------------
 */

static int
NetProcGlobalStatsRead(Proc_Entry  *entry,
	               char        *page,
                       int         *len)
{
   *len = 0;
   Proc_Printf(page, len, "Alloc packet queue length     %d\n", netPktQueueLength);
   Proc_Printf(page, len, "Free packet queue length      %d\n", netFreePktCount);
   Proc_Printf(page, len, "Free queue empty              %d\n", netOutOfPkts);

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NetPrintRateBuckets --
 *
 *       Print interrupt rate buckets to *page.
 *
 * Results: 
 *	None
 *
 * Side effects:
 *	*page is updated.
 *
 *----------------------------------------------------------------------
 */
void
NetPrintRateBuckets(char *page, int *len, NetRXClusterStats *stats)
{
#ifdef RXC_RATE_BUCKETS
   int bucket = 0;
   Proc_Printf(page, len, "intrRates: %u", stats->intrRates[bucket++]);
   while(bucket < NUM_INTR_RATE_BUCKETS) {
      Proc_Printf(page, len, ",%u", stats->intrRates[bucket++]);
   }
   Proc_Printf(page, len, "\n");

   bucket = 0;
   Proc_Printf(page, len, "pktRates: %u", stats->pktRates[bucket++]);
   while(bucket < NUM_INTR_RATE_BUCKETS) {
      Proc_Printf(page, len, ",%u", stats->pktRates[bucket++]);
   }
   Proc_Printf(page, len, "\n");
#endif // RXC_RATE_BUCKETS
}



/*
 *----------------------------------------------------------------------
 *
 * NetPrintRXClusterStats --
 *
 *       Print these stats to *page.
 *
 * Results: 
 *	None
 *
 * Side effects:
 *	*page is updated.
 *
 *----------------------------------------------------------------------
 */

void
NetPrintRXClusterStats(char *page, int *len, NetRXClusterStats *stats)
{
   Proc_Printf(page, len, "Interrupt Clustering Statistics:\n\n");

   Proc_Printf(page, len, "    Total polled packets: %20Lu\n", 
	       stats->pollPkts);
   Proc_Printf(page, len, "    Timer polled packets: %20Lu\n", 
	       stats->timerPkts);
   Proc_Printf(page, len, "    Total poll calls:     %20Lu\n", 
	       stats->pollHits);
   Proc_Printf(page, len, "    Timer poll calls:     %20Lu\n", 
	       stats->timerPollHits);
   Proc_Printf(page, len, "    Timer calls:          %20Lu\n", 
	       stats->timerHits);
   Proc_Printf(page, len, "    Total time polling:   %20Lu usec\n",
	       stats->pollTime);
   Proc_Printf(page, len, "    Intr <-> Poll transitions:      %10u\n", 
	       stats->intrPollTransitions);
   Proc_Printf(page, len, "    Device lock contention:         %10u\n\n", 
	       stats->devLockContention);

   if(stats->timerPollHits && stats->pollHits) {
      Proc_Printf(page, len, "    TxPoll:TimerPoll call ratio:    %7d.%02u\n", 
		  (int32)((stats->pollHits - stats->timerPollHits) / 
			   stats->timerPollHits), 
		  (uint32)((100 * ((stats->pollHits - stats->timerPollHits) 
				   % stats->timerPollHits)) / 
			   stats->timerPollHits));
   }

   if(stats->timerPkts && stats->pollPkts) {
      Proc_Printf(page, len, "    TxPoll:TimerPoll packet ratio:  %7d.%02u\n", 
		  (int32)((stats->pollPkts - stats->timerPkts) / 
			  stats->timerPkts), 
		  (uint32)((100 * ((stats->pollPkts - stats->timerPkts)  
				   % stats->timerPkts)) /  
			   stats->timerPkts));
   }

   if (stats->intrPollTransitions / 2) {
      Proc_Printf(page, len, "    Average polling period: %18Lu usec\n",
		  stats->pollTime / (stats->intrPollTransitions / 2));
   } 

   Proc_Printf(page, len, "\n");
}


/*
 *----------------------------------------------------------------------
 *
 * NetPrintStats --
 *
 *       Print these stats to *page.
 *
 * Results: 
 *	None
 *
 * Side effects:
 *	*page is updated.
 *
 *----------------------------------------------------------------------
 */
static void
NetPrintStats(char *page, int *len, Net_Stats *stats)
{

   Proc_Printf(page, len, "Interrupts:   %10u\n\n", 
	       stats->remote.interrupts);

   /* Header; 
    * the fields are right-aligned in 11 spaces. 
    * there is a leader of 10 spaces also. 
    */
   Proc_Printf(page, len, "          ");
   Proc_Printf(page, len, "    pktsTx       KBTx physPktsTx   physKBTx");
   Proc_Printf(page, len, "     pktsRx       KBRx physPktsRx   physKBRx");
   Proc_Printf(page, len, "     TxQOvD     TxQOvQ      RxQOv   RxQEmpty    TxLowCp");
   Proc_Printf(page, len, "    RxDelay  RxDelIdle RxWrgSlvDs RxWrgSlvKp");
   Proc_Printf(page, len, "     RxCsum     TxCsum");
   Proc_Printf(page, len, "      HwTSO     SwTSO TxNoGdSlv  BcnStChg  LnkStChg\n");

   Proc_Printf(page, len, "Total:    ");
   Proc_Printf(page, len, "%10u ", 
	       stats->local.virtPacketsSent + stats->remote.virtPacketsSent +
	       stats->local.physPacketsSent + stats->remote.physPacketsSent);

   Proc_Printf(page, len, "%10u ", 
	       (uint32)((stats->local.virtBytesSent + 
			 stats->remote.virtBytesSent +
			 stats->local.physBytesSent +
			 stats->remote.physBytesSent) >> 10));

   Proc_Printf(page, len, "%10u ", 
	       stats->local.physPacketsSent + stats->remote.physPacketsSent);

   Proc_Printf(page, len, "%10u ", 
	       (uint32)((stats->local.physBytesSent + 
			 stats->remote.physBytesSent) >> 10));			    

   Proc_Printf(page, len, "%10u ", 
	       stats->local.virtPacketsReceived + stats->remote.virtPacketsReceived +
	       stats->local.physPacketsReceived + stats->remote.physPacketsReceived);
   Proc_Printf(page, len, "%10u ", 
	       (uint32)((stats->local.virtBytesReceived + 
			 stats->remote.virtBytesReceived +
			 stats->local.physBytesReceived +
			 stats->remote.physBytesReceived) >> 10));

   Proc_Printf(page, len, "%10u ", 
	       stats->local.physPacketsReceived + stats->remote.physPacketsReceived);
   Proc_Printf(page, len, "%10u ", 
	       (uint32)((stats->local.physBytesReceived + 
			 stats->remote.physBytesReceived) >> 10));

   Proc_Printf(page, len, "%10u ", 
	       stats->local.sendOverflowDrop + stats->remote.sendOverflowDrop);
   Proc_Printf(page, len, "%10u ", 
	       stats->local.sendOverflowQueue + stats->remote.sendOverflowQueue);
   Proc_Printf(page, len, "%10u ", 
	       stats->local.receiveOverflow + stats->remote.receiveOverflow);
   Proc_Printf(page, len, "%10u ", 
	       stats->local.receiveQueueEmpty + stats->remote.receiveQueueEmpty);
   Proc_Printf(page, len, "%10u ", 
	       stats->local.pktCopiedLow + stats->remote.pktCopiedLow);
   Proc_Printf(page, len, "%10u ",
	       stats->local.recvPacketsClustered + stats->remote.recvPacketsClustered);
   Proc_Printf(page, len, "%10u ",
	       stats->local.recvPacketsClusteredUntilHalt + 
	       stats->remote.recvPacketsClusteredUntilHalt);
   Proc_Printf(page, len, "%10u ",
	       stats->local.recvInboundLBMismatchDiscard +
	       stats->remote.recvInboundLBMismatchDiscard);
   Proc_Printf(page, len, "%10u ",
	       stats->local.recvInboundLBMismatchKeep +
	       stats->remote.recvInboundLBMismatchKeep);
   Proc_Printf(page, len, "%10u ",
	       stats->local.rxsumOffload + 
	       stats->remote.rxsumOffload);
   Proc_Printf(page, len, "%10u ",
	       stats->local.txsumOffload + 
	       stats->remote.txsumOffload);
   Proc_Printf(page, len, "%10u",
	       stats->local.tcpSegOffloadHw +
               stats->remote.tcpSegOffloadHw);
   Proc_Printf(page, len, "%10u",
	       stats->local.tcpSegOffloadSw +
               stats->remote.tcpSegOffloadSw);
   Proc_Printf(page, len, "%10u",
	       stats->local.xmitNoGoodSlave +
               stats->remote.xmitNoGoodSlave);
   Proc_Printf(page, len, "%10u",
	       stats->local.beaconStateChange +
               stats->remote.beaconStateChange);
   Proc_Printf(page, len, "%10u\n",
	       stats->local.linkStateChange +
               stats->remote.linkStateChange);

   if (stats->remote.virtPacketsSent > 0 || stats->remote.virtPacketsReceived > 0 ||
       stats->remote.physPacketsSent > 0 || stats->remote.physPacketsReceived > 0) {

      Proc_Printf(page, len, "\n");

      Proc_Printf(page, len, "Remote:   ");
      Proc_Printf(page, len, "%10u ", 
		  stats->remote.physPacketsSent + stats->remote.virtPacketsSent);
      Proc_Printf(page, len, "%10u ", 
		  (uint32)((stats->remote.virtBytesSent + 
			    stats->remote.physBytesSent) >> 10));
      Proc_Printf(page, len, "%10u ", stats->remote.physPacketsSent);
      Proc_Printf(page, len, "%10u ", 
		  (uint32)(stats->remote.physBytesSent >> 10));			
      Proc_Printf(page, len, "%10u ", stats->remote.virtPacketsReceived + 
		  stats->remote.physPacketsReceived);
      Proc_Printf(page, len, "%10u ", 
		  (uint32)((stats->remote.virtBytesReceived + 
			    stats->remote.physBytesReceived) >> 10));
      Proc_Printf(page, len, "%10u ", stats->remote.physPacketsReceived);
      Proc_Printf(page, len, "%10u ", 
		  (uint32)(stats->remote.physBytesReceived >> 10));
      Proc_Printf(page, len, "%10u ", stats->remote.sendOverflowDrop);
      Proc_Printf(page, len, "%10u ", stats->remote.sendOverflowQueue);
      Proc_Printf(page, len, "%10u ", stats->remote.receiveOverflow);
      Proc_Printf(page, len, "%10u ", stats->remote.receiveQueueEmpty);
      Proc_Printf(page, len, "%10u ", stats->remote.pktCopiedLow);
      Proc_Printf(page, len, "%10u %10u ",
		  stats->remote.recvPacketsClustered, 
		  stats->remote.recvPacketsClusteredUntilHalt);
      Proc_Printf(page, len, "%10u ", stats->remote.recvInboundLBMismatchDiscard);
      Proc_Printf(page, len, "%10u ", stats->remote.recvInboundLBMismatchKeep);
      Proc_Printf(page, len, "%10u ", stats->remote.rxsumOffload);
      Proc_Printf(page, len, "%10u ", stats->remote.txsumOffload);
      Proc_Printf(page, len, "%10u", stats->remote.tcpSegOffloadHw);
      Proc_Printf(page, len, "%10u", stats->remote.tcpSegOffloadSw);
      Proc_Printf(page, len, "%10u", stats->remote.xmitNoGoodSlave);
      Proc_Printf(page, len, "%10u", stats->remote.beaconStateChange);
      Proc_Printf(page, len, "%10u\n", stats->remote.linkStateChange);
   }

   if (stats->local.virtPacketsSent > 0 || stats->local.virtPacketsReceived > 0 ||
       stats->local.physPacketsSent > 0 || stats->local.physPacketsReceived > 0) {
      Proc_Printf(page, len, "\n");
      Proc_Printf(page, len, "Local:    ");
      Proc_Printf(page, len, "%10u ",
		  stats->local.virtPacketsSent + stats->local.physPacketsSent);
      Proc_Printf(page, len, "%10u ", 
		  (uint32)((stats->local.virtBytesSent + 
			    stats->local.physBytesSent) >> 10));
      Proc_Printf(page, len, "%10u ", stats->local.physPacketsSent);
      Proc_Printf(page, len, "%10u ", (uint32)(stats->local.physBytesSent >> 10));			       
      Proc_Printf(page, len, "%10u ", 
		                        stats->local.virtPacketsReceived +
		                        stats->local.physPacketsReceived);
      Proc_Printf(page, len, "%10u ", 
		  (uint32)((stats->local.virtBytesReceived +
			    stats->local.physBytesReceived) >> 10));
      Proc_Printf(page, len, "%10u ", 
		  stats->local.physPacketsReceived);
      Proc_Printf(page, len, "%10u ", 
		  (uint32)(stats->local.physBytesReceived >> 10));

      Proc_Printf(page, len, "%10u ", stats->local.sendOverflowDrop);
      Proc_Printf(page, len, "%10u ", stats->local.sendOverflowQueue);
      Proc_Printf(page, len, "%10u ", stats->local.receiveOverflow);
      Proc_Printf(page, len, "%10u ", stats->local.receiveQueueEmpty);
      Proc_Printf(page, len, "%10u ", stats->local.pktCopiedLow);
      Proc_Printf(page, len, "%10u %10u ",
		  stats->local.recvPacketsClustered, 
		  stats->local.recvPacketsClusteredUntilHalt);
      Proc_Printf(page, len, "%10u ", stats->local.recvInboundLBMismatchDiscard);
      Proc_Printf(page, len, "%10u ", stats->local.recvInboundLBMismatchKeep);
      Proc_Printf(page, len, "%10u ", stats->local.rxsumOffload);
      Proc_Printf(page, len, "%10u ", stats->local.txsumOffload);
      Proc_Printf(page, len, "%10u", stats->local.tcpSegOffloadHw);
      Proc_Printf(page, len, "%10u", stats->local.tcpSegOffloadSw);
      Proc_Printf(page, len, "%10u", stats->local.xmitNoGoodSlave);
      Proc_Printf(page, len, "%10u", stats->local.beaconStateChange);
      Proc_Printf(page, len, "%10u\n", stats->local.linkStateChange);
   }
   
   if (stats->remote.beacon.rxSuccess > 0 
    || stats->remote.beacon.rxTaggedBeacon > 0 
    || stats->remote.beacon.rxUnmatchedLen > 0 
    || stats->remote.beacon.rxUnmatchedMagic > 0 
    || stats->remote.beacon.rxUnmatchedServer > 0
    || stats->remote.beacon.rxLoopDetected > 0
    || stats->remote.beacon.txSuccess > 0
    || stats->remote.beacon.txTaggedBeacon > 0
    || stats->remote.beacon.txFailure > 0
    || stats->remote.beacon.txLinkDown > 0) {
      Proc_Printf(page, len, "\n          ");  
      Proc_Printf(page, len, " rxBecn   rxTagged    rxUmTag    rxUmLen  rxUmMagic");
      Proc_Printf(page, len, " rxUmServer   rxSwLoop    ");
      Proc_Printf(page, len, " txBecn   txTagged  txFailure   txLnkDwn\n");
      Proc_Printf(page, len, "Beacon:");
      Proc_Printf(page, len, "%10u ", stats->remote.beacon.rxSuccess);
      Proc_Printf(page, len, "%10u ", stats->remote.beacon.rxTaggedBeacon);
      Proc_Printf(page, len, "%10u ", stats->remote.beacon.rxUmTag);
      Proc_Printf(page, len, "%10u ", stats->remote.beacon.rxUnmatchedLen);
      Proc_Printf(page, len, "%10u ", stats->remote.beacon.rxUnmatchedMagic);
      Proc_Printf(page, len, "%10u ", stats->remote.beacon.rxUnmatchedServer);
      Proc_Printf(page, len, "%10u ", stats->remote.beacon.rxLoopDetected);
      Proc_Printf(page, len, "%10u ", stats->remote.beacon.txSuccess);
      Proc_Printf(page, len, "%10u ", stats->remote.beacon.txTaggedBeacon);
      Proc_Printf(page, len, "%10u ", stats->remote.beacon.txFailure);
      Proc_Printf(page, len, "%10u ", stats->remote.beacon.txLinkDown);
      Proc_Printf(page, len, "\n");  
   }

   if (stats->remote.vlan.xmitSwTagged> 0 
    || stats->remote.vlan.xmitHwAccel> 0 
    || stats->remote.vlan.recvSwUntagged > 0 
    || stats->remote.vlan.recvHwAccel > 0
    || stats->remote.vlan.xmitErrNoCapability > 0
    || stats->remote.vlan.recvErrHandleNoCapability > 0
    || stats->remote.vlan.recvErrHandleNoVlan > 0
    || stats->remote.vlan.recvErrNoTag > 0
    || stats->remote.vlan.recvErrTagMismatch > 0
    || stats->remote.vlan.recvErrOnPlainNic > 0
    || stats->remote.vlan.recvNativeVlan > 0) {
      Proc_Printf(page, len, "\n          ");  
      Proc_Printf(page, len, "txSwTag  txHwAccel  rxSwUntag  rxHwAccel  txErNoCap  rxErNoCap");
      Proc_Printf(page, len, " rxNoVlnHdl    rxNoTag   rxTagMis rxTagOnNoV");
      Proc_Printf(page, len, " txNativVln rxNativVln\n");
      Proc_Printf(page, len, "VLan  :");
      Proc_Printf(page, len, "%10u ", stats->remote.vlan.xmitSwTagged);
      Proc_Printf(page, len, "%10u ", stats->remote.vlan.xmitHwAccel);
      Proc_Printf(page, len, "%10u ", stats->remote.vlan.recvSwUntagged);
      Proc_Printf(page, len, "%10u ", stats->remote.vlan.recvHwAccel);
      Proc_Printf(page, len, "%10u ", stats->remote.vlan.xmitErrNoCapability);
      Proc_Printf(page, len, "%10u ", stats->remote.vlan.recvErrHandleNoCapability);
      Proc_Printf(page, len, "%10u ", stats->remote.vlan.recvErrHandleNoVlan);
      Proc_Printf(page, len, "%10u ", stats->remote.vlan.recvErrNoTag);
      Proc_Printf(page, len, "%10u ", stats->remote.vlan.recvErrTagMismatch);
      Proc_Printf(page, len, "%10u ", stats->remote.vlan.recvErrOnPlainNic);
      Proc_Printf(page, len, "%10u ", stats->remote.vlan.xmitNativeVlan);
      Proc_Printf(page, len, "%10u ", stats->remote.vlan.recvNativeVlan);
      Proc_Printf(page, len, "\n");  
   }

   Proc_Printf(page, len, "\n                ");  
   Proc_Printf(page, len, "   Delayed    NoDelay   Overflow   !Running       Idle    Halting       ToOn      ToOff   Off&Pend\n");   
   Proc_Printf(page, len, "RX Clustering:  ");
   Proc_Printf(page, len, "%10u ", stats->remote.recvPacketsClustered);
   Proc_Printf(page, len, "%10u ", stats->remote.recvPacketsNoDelay);
   Proc_Printf(page, len, "%10u ", stats->remote.recvPacketsClusteredOverflow);
   Proc_Printf(page, len, "%10u ", stats->remote.recvPacketsClusteredNotRunning);
   Proc_Printf(page, len, "%10u ", stats->remote.recvPacketsClusteredIdle);
   Proc_Printf(page, len, "%10u ", stats->remote.recvPacketsClusteredUntilHalt);
   Proc_Printf(page, len, "%10u ", stats->remote.recvClusterOn);
   Proc_Printf(page, len, "%10u ", stats->remote.recvClusterOff);
   Proc_Printf(page, len, "%10u\n", stats->remote.recvClusterOffPktPending);

   Proc_Printf(page, len, "\n               ");  
   Proc_Printf(page, len, "    Packets      Calls    StopInt    CompInt      TOInt    IdleInt   QueueLow    Halting    Receive       ToOn      ToOff   Off&Pend\n");   
   Proc_Printf(page, len, "Xmit Clustering:");
   Proc_Printf(page, len, "%10u ", 
	       stats->remote.virtPacketsSent + stats->remote.physPacketsSent);
   Proc_Printf(page, len, "%10u ", stats->remote.xmitCalls);
   Proc_Printf(page, len, "%10u ", stats->remote.xmitStoppedIntr);
   Proc_Printf(page, len, "%10u ", stats->remote.xmitCompleteIntr);
   Proc_Printf(page, len, "%10u ", stats->remote.xmitTimeoutIntr);
   Proc_Printf(page, len, "%10u ", stats->remote.xmitIdleIntr);   
   Proc_Printf(page, len, "%10u ", stats->remote.xmitQueueLow);
   Proc_Printf(page, len, "%10u ", stats->remote.xmitClusteredUntilHalt);
   Proc_Printf(page, len, "%10u ", stats->remote.xmitClusteredUntilRecv);
   Proc_Printf(page, len, "%10u ", stats->remote.xmitClusterOn);
   Proc_Printf(page, len, "%10u ", stats->remote.xmitClusterOff);
   Proc_Printf(page, len, "%10u\n", stats->remote.xmitClusterOffPktPending);
}


/*
 *----------------------------------------------------------------------
 *
 * NetProcDevStatsRead --
 *
 *       Return stats for this adapter.
 *
 * Results: 
 *	None
 *
 * Side effects:
 *	*page is updated.
 *
 *----------------------------------------------------------------------
 */
static int
NetProcDevStatsRead(Proc_Entry  *entry,
                    char        *page,
                    int         *len)
{
   NetDevConfig_ESX2 *config = entry->private;
   NetDevStats_ESX2 *stats = &config->devStats;

   *len = 0;

   Proc_Printf(page, len, "DevQueueSize: %10u\n", stats->privatePktCount);
   Proc_Printf(page, len, "DevQueueLen:  %10u\n", stats->privatePktsInUse);

   NetPrintStats(page, len, &stats->stats);

   if(config->clusterFlags & NET_RXCLUSTERING_ENABLED) {
      NetPrintRXClusterStats(page, len, &config->clustering.stats);
      NetPrintRateBuckets(page, len, &config->clustering.stats);
   }

   return VMK_OK;
}


static void
NetNicTeamingProcPrint(NetDevConfig_ESX2 *config, char *page, int *len)
{
}


/*
 *----------------------------------------------------------------------
 *
 * NetProcDevConfigRead --
 *
 *      Return state about this adapter.
 *
 * Results: 
 *	VKM_OK.
 *
 * Side effects:
 *	*page and *len are updated.
 *
 *----------------------------------------------------------------------
 */
static int
NetProcDevConfigRead(Proc_Entry *entry, char *page, int *len)
{
   NetDevConfig_ESX2 *config = entry->private;

   *len = 0;
   if (config->capabilities & VMNIC_CAP_HW_TX_VLAN) {
      Proc_Printf(page, len, "VLanHwTxAccel             Yes\n");
   } else {
      Proc_Printf(page, len, "VLanHWTxAccel             No\n");
   }

   if (config->capabilities & VMNIC_CAP_HW_RX_VLAN) {
      Proc_Printf(page, len, "VLanHwRxAccel             Yes\n");
   } else {
      Proc_Printf(page, len, "VLanHwRxAccel             No\n");
   }

   if (config->capabilities & VMNIC_CAP_SW_VLAN) {
      Proc_Printf(page, len, "VLanSwTagging             Yes\n");
   } else {
      Proc_Printf(page, len, "VLanSwTagging             No\n");
   }

   if (config->promiscOK) {
      Proc_Printf(page, len, "PromiscuousAllowed        Yes\n");
   } else {
      Proc_Printf(page, len, "PromiscuousAllowed        No\n");
   }

   if (config->clusterFlags & NET_RXCLUSTERING_ENABLED) {
      Proc_Printf(page, len, "InterruptClustering       Yes\n");
   } else {
      Proc_Printf(page, len, "InterruptClustering       No\n");
   }
   
   if (config->linkState != NETDEV_LINK_UNK) {
      Proc_Printf(page, len, "Link state:               %s\n",
             config->linkState ? "Up" : "Down");
      if (config->linkState == NETDEV_LINK_UP) {
         Proc_Printf(page, len, "Speed:                    %d Mbps, %s duplex\n",
                  config->linkSpeed, config->fullDuplex ? "full" : "half");
         Proc_Printf(page, len, "Queue:                    %s\n",
                     config->xmitStopped? "Stopped" : "Running");

      }
   }

   Proc_Printf(page, len, "PCI (bus:slot.func):      %d:%d.%d\n",
               config->bus, config->slot, config->fn);

   Proc_Printf(page, len, "Minimum Capabilities      0x%x\n", config->minCapabilities);
   Proc_Printf(page, len, "Device Capabilities       0x%x\n", config->capabilities);
   Proc_Printf(page, len, "Maximum Capabilities      0x%x\n", config->maxCapabilities);

   NetNicTeamingProcPrint(config, page, len);

   Proc_Printf(page, len, "\nInterrupt vector          0x%x\n", config->intrHandlerVector);

   NetDebug_ProcPrint(page, len);

   if (Dump_GetIPAddr() != 0) {
      uint32 ipAddr = Dump_GetIPAddr();
      Proc_Printf(page, len, "Dumper:                   netdumper @ %d.%d.%d.%d\n",
		  (ipAddr >> 24) & 0xff,
		  (ipAddr >> 16) & 0xff,
		  (ipAddr >> 8) & 0xff,
		  (ipAddr) & 0xff);
   }

   return(VMK_OK);
}


VMK_ReturnStatus
Net_DiscoverOpen(char *name)
{
   return VMK_FAILURE;
}


void
NetVlanSwitchHwTxAccel(NetDevConfig_ESX2 *config, Bool on)
{
}



/*
 *----------------------------------------------------------------------
 *
 * NetUtilMemDup --
 *
 *      A utility routine that does the combination of malloc and memcpy.
 *      (Sort of like Unix strdup(3)).
 *
 * Results: 
 *      ptr to the newly allocated string.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static char *NetUtilMemDup(char *src)
{
   char *dst = Mem_Alloc(strlen(src) + 1);
   ASSERT(dst != NULL);
   memcpy(dst, src, strlen(src) + 1);
   return dst;
}


/*
 *----------------------------------------------------------------------
 *
 * NetDevPromiscOff --
 *
 *      Timer callback function to turn promiscuous mode off for
 *	an adapter.
 *
 * Results: 
 *	VMK_OK.
 *
 * Side effects:
 *	*page is updated.
 *
 *----------------------------------------------------------------------
 */
static void
NetDevPromiscOff(void *data, UNUSED_PARAM(Timer_AbsCycles timestamp))
{
}


/*
 *----------------------------------------------------------------------
 *
 * NetProcDevConfigWrite --
 *
 *      Update configuration info for the adapter.
 *
 * Results: 
 *	VMK_OK.
 *
 * Side effects:
 *	*page is updated.
 *
 *----------------------------------------------------------------------
 */
static int
NetProcDevConfigWrite(Proc_Entry  *entry,
	              char        *page,
                      int         *lenp)
{
   char *argv[3];
   NetDevConfig_ESX2 *config = entry->private;   
   int argc = Parse_Args(page, argv, 3);
   char *psName = config->name;

   if (argc < 2) {
      if (strcmp(argv[0], "ClearStats") == 0) {
         LOG(0, "%s stats cleared", config->name);
         memset(&config->devStats, 0, sizeof(config->devStats));
         return VMK_OK;
      } else if (strcmp(argv[0], "Discover") == 0)  {
         if (Net_DiscoverOpen(config->name) != VMK_OK) {
            return VMK_BAD_PARAM;
         }
         return VMK_OK;
      } else {
         LOG(0, "Not enough arguments for \"%s\"", argv[0]);
         return VMK_BAD_PARAM;
      }
   }
#ifdef VMX86_DEBUG
   if (strcmp(argv[0], "VLanSwTagging") == 0) {
      if (strcmp(argv[1], "Yes") == 0 || strcmp(argv[1], "yes") == 0) {
         config->capabilities |= VMNIC_CAP_SW_VLAN;
      } else if (strcmp(argv[1], "No") == 0 || strcmp(argv[1], "no") == 0) {
         config->capabilities &= ~VMNIC_CAP_SW_VLAN;
      }
   } else if (strcmp(argv[0], "VLanHwTxAccel") == 0) {
      if (strcmp(argv[1], "Yes") == 0 || strcmp(argv[1], "yes") == 0) {
         NetVlanSwitchHwTxAccel(config, TRUE); 
      } else if (strcmp(argv[1], "No") == 0 || strcmp(argv[1], "no") == 0) {
         NetVlanSwitchHwTxAccel(config, FALSE); 
      }
   } else if (strcmp(argv[0], "VLanHwRxAccel") == 0) {
      Warning("No support to turn on/off VLanHwRxAccel");
   } else
#endif
   if (strcmp(argv[0], "PromiscuousAllowed") == 0) {
      if (strcmp(argv[1], "Yes") == 0 || strcmp(argv[1], "yes") == 0) {
	 config->promiscOK = TRUE;
      } else if (strcmp(argv[1], "No") == 0 || strcmp(argv[1], "no") == 0) {
	 /* 
	  * Schedule the turning off in a timer because we can't grab any net 
	  * locks right now or we can have a deadlock.  We can't schedule a 
	  * helper function because interrupts are disabled since the proc
	  * lock is an IRQ lock.  Helper_Request panics if interrupts are 
	  * disabled.
	  */
	 char *name = NetUtilMemDup(config->name);
	 Timer_Add(0, NetDevPromiscOff, 10, TIMER_ONE_SHOT, name);
      }
   } else if (strcmp(argv[0], "InterruptClustering") == 0) {
      if (strcmp(argv[1], "Yes") == 0 || strcmp(argv[1], "yes") == 0) {
#ifdef INTR_CLUSTERING_IS_NOT_BROKEN
	 config->clustering.flags |= NET_RXCLUSTERING_ENABLED;
	 NetRXClusterOn();
#endif // INTR_CLUSTERING_IS_NOT_BROKEN
      } else if (strcmp(argv[1], "No") == 0 || strcmp(argv[1], "no") == 0) {
	 config->clustering.flags &= ~NET_RXCLUSTERING_ENABLED;
      }	 
   } else if (strcmp(argv[0], "MinCapabilities") == 0) {
      Parse_Hex(argv[1], strlen(argv[1]), &config->minCapabilities);
      Log("Minimum Capabilities are 0x%x", config->minCapabilities);      
   } else if (strcmp(argv[0], "MaxCapabilities") == 0) {
      Parse_Hex(argv[1], strlen(argv[1]), &config->maxCapabilities);
      Log("Maximum Capabilities are 0x%x", config->maxCapabilities);
   } else if (strcmp(argv[0], "Capabilities") == 0) {
      Parse_Hex(argv[1], strlen(argv[1]), &config->capabilities);
      config->capabilities &= config->maxCapabilities;
      config->capabilities |= config->minCapabilities;
      Log("Device Capabilities are 0x%x", config->capabilities);
   } else if (!strcmp(argv[0], "nicteaming") || !strcmp(argv[0], "nt")) {
      Net_PortID dummy;
      /* 
       * Syntax examples:
       * #echo "nicteaming add vmnic0" >> /proc/vmware/bond0/config
       * #echo "nicteaming delete vmnic0" >> /proc/vmware/bond0/config
       */
      if (argc != 3) {
	 Warning("nicteaming called with %d args", argc);	 
	 return VMK_BAD_PARAM;
      } 

      if (!strcmp(argv[1], "add")) {
         return Net_ConnectBondUplinkPort(psName, argv[2], &dummy);
      } else if (!strcmp(argv[1], "delete")) {
         return Net_DisconnectBondUplinkPort(psName, argv[2]);
      } else {
         return VMK_BAD_PARAM;
      }
   } else if (strcmp(argv[0], "DebugSocket") == 0)  {
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


void
NetProc_HostChange(Portset *ps, Bool reg)
{
   // Only physical NICs.
   if (strncmp(ps->name, NET_PHYS_NIC_PREFIX, strlen(NET_PHYS_NIC_PREFIX)) == 0) {
      Host_VMnixVMKDev(VMNIX_VMKDEV_NET, ps->name, NULL, NULL, 0, reg);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * NetProc_AddPortset --
 *
 *      Sets up the /proc/vmware/net/eth<n> and
 *	/proc/vmware/net/eth<n>/stats
 *
 * Results: 
 *	none
 *
 * Side effects:
 *	none
 *
 *----------------------------------------------------------------------
 */

void 
NetProc_AddPortset(Portset *ps)
{
   if (legacySupport && strncmp(ps->name, "legacyBond", 10)) {
      NetDevProc_ESX2 *proc = Mem_Alloc(sizeof(*proc));
      NetDevConfig_ESX2 *config = Mem_Alloc(sizeof(*config));
      if (proc && config) {
         memset(proc, 0, sizeof(*proc));
         memset(config, 0, sizeof(*config));

         proc->devProcEntry.read = NULL;
         proc->devProcEntry.write = NULL;
         proc->devProcEntry.parent = ProcNet_GetRootNode();
         proc->devProcEntry.private = ps;
         Proc_Register(&proc->devProcEntry, ps->name, TRUE);

         memcpy(config->name, ps->name, sizeof(config->name));
         proc->configProcEntry.read = NetProcDevConfigRead;
         proc->configProcEntry.write = NetProcDevConfigWrite;
         proc->configProcEntry.parent = &proc->devProcEntry;
         proc->configProcEntry.canBlock = FALSE;
         proc->configProcEntry.private = config;
         Proc_Register(&proc->configProcEntry, "config", FALSE);

         proc->statsProcEntry.read = NetProcDevStatsRead;
         proc->statsProcEntry.write = NULL;
         proc->statsProcEntry.parent = &proc->devProcEntry;
         proc->statsProcEntry.canBlock = FALSE;
         proc->statsProcEntry.private = config;
         Proc_Register(&proc->statsProcEntry, "stats", FALSE);

         proc->ps = ps;
         proc->config = config;


         List_InitElement(&proc->links);
         List_Insert(&proc->links, LIST_ATREAR(&procList.procList));
      } else {
         if (proc) {
            Mem_Free(proc);
         }

         if (config) {
            Mem_Free(config);
         }
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * NetProc_RemovePortset --
 *
 *       Sets up the /proc/vmware/scsi/scsi<n> entry
 *
 * Results: 
 *	none
 *
 * Side effects:
 *	none
 *
 *----------------------------------------------------------------------
 */

void 
NetProc_RemovePortset(Portset *ps)
{
   if (legacySupport) {
      NetDevProc_ESX2 *proc = (NetDevProc_ESX2 *)List_First(&procList.procList);
      while (proc) {
         if (!strncmp(proc->config->name, ps->name, sizeof(proc->config->name))) {
            Log("found portset %s", ps->name);
            List_Remove(&proc->links);
            break;
         }

         proc = (NetDevProc_ESX2 *) List_Next(&proc->links);
         if ((List_Links *)proc == &procList.procList) {
            proc = NULL;
         }
      }

      if (proc) {
         Proc_Remove(&proc->devProcEntry);
         Proc_Remove(&proc->statsProcEntry);
         Proc_Remove(&proc->configProcEntry);

         Mem_Free(proc->config);
         Mem_Free(proc);
      }
   }
}


/*
 *----------------------------------------------------------------------------
 *
 *  --
 *
 *
 *----------------------------------------------------------------------------
 */
 
VMK_ReturnStatus
Net_CreatePortsetESX2(const char *name)
{
   VMK_ReturnStatus ret = VMK_FAILURE;
   if (legacySupport) {
      Portset *ps = NULL;
      ret = Portset_Activate(32, (char *)name, &ps);      
      if (ret == VMK_OK) {
         LOG(0, "Portset %s activated", name);
         ASSERT(ps);

         /* XXX: old code assumes that the device is a switch */
         ret = Hub_Activate(ps);

         if (ret == VMK_OK) {
            ps->type = NET_TYPE_HUBBED;
            if (Portset_IsActive(ps) && ps->devImpl.uplinkConnect) {
               PortID portID;
               ret = ps->devImpl.uplinkConnect(ps, (char *)name, &portID);
               if (ret != VMK_OK) {
                  Log("Uplink connect failed for %s: %s", name,
                      VMK_ReturnStatusToString(ret));
                  Portset_Deactivate(ps);
               }
            }
         } else {
            Log("Failed to activate hub %s: %s", name, VMK_ReturnStatusToString(ret));
            Portset_Deactivate(ps);
         }
         
      }

      if (ps) {
         Portset_UnlockExcl(ps);
      }
   }

   return ret;
}


void
Bond_LegacyInit(void)
{
   // create bonds 0 - 9
   int i;
   static char bondHub[6];
   static char bondPortset[12];
   for (i = 0; i < 10; i++) {
      Net_PortID dummy;
      snprintf(bondHub, sizeof bondHub, "bond%d", i);
      Net_Create(bondHub, NET_TYPE_HUBBED, ESX2_MAX_NUM_PORTS_PER_SET);
      snprintf(bondPortset, sizeof bondPortset, "legacyBond%d", i);
      Net_Create(bondPortset, NET_TYPE_BOND, ESX2_MAX_NUM_PORTS_PER_SET);
      Net_ConnectUplinkPort(bondHub, bondPortset, &dummy);
   }
}

