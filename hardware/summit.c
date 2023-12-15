/* **********************************************************
 * Copyright 2002 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * summit.c
 *
 * 	Management of IBM Summit chipset.
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "vmkernel.h"
#include "kvmap.h"
#include "proc.h"
#include "parse.h"
#include "memmap.h"
#include "numa.h"
#include "smp.h"
#include "timer.h"
#include "summit.h"

#define LOGLEVEL_MODULE Summit
#define LOGLEVEL_MODULE_LEN 6
#include "log.h"

/*
 * Private data structures
 */
typedef struct {
   NUMA_Node     nodeId;
   Atomic_uint32 initialized;
   IBM_Twister   twister;
   IBM_Cyclone   cyclone;
} Summit_NodeInfo;



/*
 * Globals
 */

// Points to each node's Cyclone MPMC0 counter
CycloneReg *Summit_CycloneCyclesReg[NUMA_MAX_NODES];

/*
 * Locals
 */

static Summit_NodeInfo  summitNode[NUMA_MAX_NODES];
static uint64           myZero = 0;

/*
 * Local functions 
 */
static int SummitProcReadTwister(Proc_Entry  *entry, char *buffer, int *len);
static int SummitProcReadCyclone(Proc_Entry  *entry, char *buffer, int *len);
static int SummitProcWriteTwister(Proc_Entry  *entry, char *buffer, int *len);
static int SummitProcWriteCyclone(Proc_Entry  *entry, char *buffer, int *len);

static Bool SummitInitTwister(Summit_NodeInfo *node, Proc_Entry *parent);
static void SummitInitCyclone(Summit_NodeInfo *node, Proc_Entry *parent, uint32 cbar);

static void SummitMeasureLatency(void *data, Timer_AbsCycles timestamp);


/*
 *----------------------------------------------------------------------
 *
 * Summit_EarlyInit() --
 *
 * 	Early initialization of data structures.
 *
 * Results:
 * 	None.
 *
 * Side effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
void
Summit_EarlyInit(void)
{
   int n;

   /*
    * Initialize MPMC pointers to point at known zero. This prevents
    * somebody from de-referencing a NULL pointer and resetting system.
    */
   for (n=0; n < NUMA_MAX_NODES; n++) {
      Summit_CycloneCyclesReg[n] = (CycloneReg *) &myZero;
   }

   memset(&summitNode, 0, sizeof(summitNode));
}

/*
 * This may be called before Timer_InitCycles(), so be prepared to use
 * alternative sources of time.  XXX Although Timer_GetCycles is based
 * on the Cyclone timer, it is offset to begin at 0 when
 * Timer_InitCycles is called.  Therefore any timestamps this routine
 * returns before Timer_InitCycles are incomparable with timestamps
 * returned afterward.  See PR 52240.
 */
static inline uint64
SummitGetTimestamp(void)
{
   if (LIKELY(Timer_GetCycles != NULL)) {
      return Timer_GetCycles();
   }
   if (LIKELY(summitNode[0].cyclone.present)) {
      return Summit_CycloneCyclesReg[0][0];
   } else {
      return 0;
   }
}

/*
 *----------------------------------------------------------------------
 * SummitConfigTwisterPC --
 *
 *      Configure one of four Twister perf counters to count an event
 *      from a certain event group.  Does not affect counter enable.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
SummitConfigTwisterPC(Summit_NodeInfo *node,
		    int counterNum,     // 0-3
		    Summit_CounterGroup group,
		    uint8 event)
{
   uint64 regVal = node->twister.reg[TWISTER_PMCS_GROUP(group)];

   /* First modify event select registers (PMCS_nn) */
   regVal &= ~TWISTER_PMC_SHIFT(0x00ff, counterNum);
   regVal |= TWISTER_PMC_SHIFT(event, counterNum);
   node->twister.reg[TWISTER_PMCS_GROUP(group)] = regVal;

   /* Then modify PMCS counter group select reg */
   regVal = node->twister.reg[TWISTER_PMCS];
   regVal &= ~TWISTER_PMC_SHIFT(0x00ff, counterNum);
   regVal |= TWISTER_PMC_SHIFT((uint8) group, counterNum);
   node->twister.reg[TWISTER_PMCS] = regVal;
}


/*
 *----------------------------------------------------------------------
 * SummitResetAllTwisterPCs --
 *
 *      Clears all four Twister Perf Counters, records the timestamp,
 *      then enables all the counters.
 *
 * Side effects:
 *      Counters are momentarily disabled. Latency mode is turned off,
 *      since bit 24 always reads back as 0.
 *
 *----------------------------------------------------------------------
 */
static void
SummitResetAllTwisterPCs(Summit_NodeInfo *node)
{
   TwisterReg *mmioT = node->twister.reg;
   uint64 timestamp;

   /* Disable counters */
   mmioT[TWISTER_PMCC] &= ~TWISTER_PMC_ENABLE;

   /* Zero them out */
   mmioT[TWISTER_PMC0] = 0;
   mmioT[TWISTER_PMC1] = 0;
   mmioT[TWISTER_PMC2] = 0;
   mmioT[TWISTER_PMC3] = 0;

   /* Record timestamp and re-enable counters */
   timestamp = SummitGetTimestamp();
   mmioT[TWISTER_PMCC] |= TWISTER_PMC_ENABLE;

   node->twister.tsZeroed[0] = timestamp;
   node->twister.tsZeroed[1] = timestamp;
   node->twister.tsZeroed[2] = timestamp;
   node->twister.tsZeroed[3] = timestamp;
}


static inline void
SummitStartTwisterPC(Summit_NodeInfo *node,
		   int counterNum)
{
   node->twister.reg[TWISTER_PMC0 + counterNum] = 0;
   node->twister.tsZeroed[counterNum] = SummitGetTimestamp();
   node->twister.reg[TWISTER_PMCC] |= (1 << counterNum);
}

static inline void
SummitStopTwisterPC(Summit_NodeInfo *node,
		  int counterNum)
{
   node->twister.reg[TWISTER_PMCC] &= ~(1 << counterNum);
}


/*
 *----------------------------------------------------------------------
 * SummitEnableTwisterLatencyMode
 *
 *      Enables performance counter latency mode.   In latency mode, the
 *      # of events is stored in PMC2 and the # of cycles in PMC0.
 *
 * Side effects:
 *      Counters are all reset.
 *
 *----------------------------------------------------------------------
 */
static void
SummitEnableTwisterLatencyMode(Summit_NodeInfo *node,
			     uint8 event)
{
   // Set PQ_PRICONTROL(24:20) to 0x1f for random cmd selection
   node->twister.reg[TWISTER_PQ_PRICTL] |= (0x1f << 20);
   node->twister.reg[TWISTER_PMCC] &= ~TWISTER_PMC_ENABLE;

   // Counter 3 is unaffected by latency mode, don't touch
   SummitConfigTwisterPC(node, 0, PMCS_PQ, event);
   SummitConfigTwisterPC(node, 2, PMCS_PQ, event);

   // Clear and enable the counters
   SummitResetAllTwisterPCs(node);

   // Enable latency mode.  This must come after the reset....
   node->twister.reg[TWISTER_PMCC] |= TWISTER_PMC_LATENCY;
}


static inline void
SummitDisableTwisterLatencyMode(Summit_NodeInfo *node)
{
   node->twister.reg[TWISTER_PMCC] = 0;
}


/*
 *----------------------------------------------------------------------
 *
 * Summit_LocalInit --
 *
 *      Summit module per-PCPU initialization.
 *
 * Results:
 *      FALSE if hardware initialization fails.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Bool
Summit_LocalInit(PCPU pcpu, Proc_Entry *parent)
{
   NUMA_Node nodeId = NUMA_PCPU2NodeNum(pcpu);
   Summit_NodeInfo *node = &summitNode[nodeId];
   Bool ongoingOrDone;
   Bool ok = TRUE;

   /*
    * We don't need per-cpu initialization, just per-node so don't do
    * the work twice and since all pcpus may be doing this in parallel,
    * use an atomic operation.
    */
   ongoingOrDone = Atomic_ReadWrite(&node->initialized, TRUE);
   if (!ongoingOrDone) {
      node->nodeId = nodeId;
      ASSERT(!node->twister.present);
      ok = SummitInitTwister(node, parent);
   }

   return ok;
}


/*
 *----------------------------------------------------------------------
 *
 * SummitInitTwister --
 *
 *      Detect and map IBM Twister chips on specific node;
 *      add proc node if initialization succeeds.  Also calls
 *      SummitInitCyclone() at the end.
 *
 *      This routine is called back once per NUMA node.  It will look 
 *      up the IBM Twister at the predefined local node address, then 
 *      get the CBAR value which is this Twister's MMIO (global)
 *      address. Then it maps this address so this Twister can be 
 *      accessed from any node any PCPU, and does some initialization.
 *
 * Results:
 *      FALSE if the Twister cannot be properly identified or set up.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static Bool
SummitInitTwister(Summit_NodeInfo *node, Proc_Entry *parent)
{
   void * vAddr;
   KVMap_MPNRange range;
   TwisterReg *localT;
   TwisterReg *mmioT;
   int         nodeId;
   uint32      cbar;
   uint32      chipId, localId;
   uint8       ver;

   /* Check to make sure nobody else done this node's Twister */
   if (node->twister.present) {
      Warning("Node %d Twister already initialized, this may be a bug",
	      node->nodeId);
      return TRUE;
   }

   /* first map the local Twister registers to our address space */
   range.startMPN = MA_2_MPN(IBM_LOCAL_TWISTER_MA); 
   range.numMPNs = (IBM_TWISTER_REG_SPACE / PAGE_SIZE);

   vAddr = KVMap_MapMPNs(range.numMPNs, &range, 1, TLB_UNCACHED);
   if (!vAddr) {
      Warning("Unable to map node %d Twister regs to virtual address",
	      node->nodeId);
      return FALSE;
   }      
   ASSERT(!((uint32)vAddr & PAGE_MASK));

   localT = (TwisterReg *) vAddr;

   /* Check Twister ID */
   chipId = localT[TWISTER_ECID] & TWISTER_ID_MASK;
   localId = chipId;
   if (chipId == TWISTER_BAD_ID) {
      Warning("Node %d Twister not present, disabling x440 support", node->nodeId);
      KVMap_FreePages((void *) localT);
      return FALSE;
   } else if (chipId == TWISTER3_ID) {
      Log("IBM x445 Twister3 chipset detected");
   } else if (chipId != TWISTER_ID) {
      Warning("Node %d local Twister ID Mismatch "
	      "(found %04X, expected %04X)", node->nodeId,
	      chipId, TWISTER_ID);
   }
   ver = (localT[TWISTER_ECID] & TWISTER_VER_MASK) >> TWISTER_VER_SHIFT;
   Log("Node %d local Twister ver %d found", node->nodeId, ver);
   node->twister.ID = chipId;

   /* Get CBAR - MMIO addr of this node's Cyclone Regs */
   if (chipId == TWISTER3_ID) {
      cbar = (uint32) ( localT[ TWISTER3_CBAR ] );
   } else {
      cbar = (uint32) ( localT[ TWISTER_CBAR ] );
   }
   LOG(1, "CBAR = 0x%08X", cbar);

   /* Map MMIO Twister address to kernel address space */
   range.startMPN = MA_2_MPN((MA) cbar + IBM_TWISTER_OFFSET); 
   range.numMPNs = (IBM_TWISTER_REG_SPACE / PAGE_SIZE);
   
   mmioT = (TwisterReg *) KVMap_MapMPNs(range.numMPNs, &range, 1, TLB_UNCACHED);
   if (!mmioT) {
      Warning("Unable to map node %d MMIO Twister regs to virtual address",
	      node->nodeId);
      KVMap_FreePages((void *) localT);
      return FALSE;
   }      
   ASSERT(!((uint32)mmioT & PAGE_MASK));
   
   node->twister.reg = mmioT;
   
   /* Verify Twister Chip ID and node # */
   /* No matter what the Chip ID we read here must be the same as the Chip ID
    * we read back there, because it's the same chip.
    */
   chipId = mmioT[TWISTER_ECID] & TWISTER_ID_MASK;
   if (chipId != localId) {
      Warning("Node %d Twister ID Mismatch (found %04X, expected %04X)",
	      node->nodeId, chipId, localId);
      Warning("Deprecating to generic NUMA system from x440");
      KVMap_FreePages((void *) mmioT);
      KVMap_FreePages((void *) localT);
      return FALSE;
   }
   Log("Node %d Twister ver %d found at 0x%08X", node->nodeId,
       (chipId & TWISTER_VER_MASK) >> TWISTER_VER_SHIFT,
       cbar + IBM_TWISTER_OFFSET);
   
   if (chipId == TWISTER3_ID) {
      nodeId = mmioT[TWISTER3_NODECONFIG] & TWISTER_NODE_MASK;
   } else {
      nodeId = mmioT[TWISTER_NODECONFIG] & TWISTER_NODE_MASK;
   }
   if (nodeId != node->nodeId) {
      Warning("Node %d Twister reports nodeID as %d", node->nodeId, nodeId);
   }
   node->twister.present = TRUE;

   /* Set up performance monitoring */
   /* PMCS_PQ perf monitoring events:
    * 0x10 - # of BRL commands, Foster source HITM data (proc-to-proc)
    * 0x11 - # of BRL commands, Foster source L3 data   (L3 cache accesses)
    * 0x12 - # of BRL commands, Foster source Memory data (local node mem access)
    * 0x13 - # of BRL commands, Foster source Scalability data (other node mem)
    */
   SummitConfigTwisterPC(node, 1, PMCS_PQ, 0x11);
   SummitConfigTwisterPC(node, 2, PMCS_PQ, 0x12);
   SummitConfigTwisterPC(node, 3, PMCS_PQ, 0x13);
   
   /*
    * PMCS_Quad perf monitoring events:
    * 0x1b - # of BRL/BRIL commands, Foster source 
    * 0x18 - # of BWL/BWIL commands, Foster source
    * 0x0e - # of Interrupt/EOI commands, Foster source
    * 0x1e - # of commands, Foster source
    */
   SummitConfigTwisterPC(node, 0, PMCS_QUAD, 0x1b);

   /* Clear perf counters and start them */
   SummitResetAllTwisterPCs(node);
   
   /* Free virtual pages mapped to local Twister */
   KVMap_FreePages((void *) localT);

   /* Add twister entry for perf counter i/f */
   Proc_InitEntry(&node->twister.procTwister);
   node->twister.procTwister.parent = parent;
   node->twister.procTwister.read  = SummitProcReadTwister;
   node->twister.procTwister.write = SummitProcWriteTwister;
   node->twister.procTwister.private = node;
   Proc_RegisterHidden(&node->twister.procTwister, "twister", FALSE);

   /* Discover and map Cyclone Jr. for timestamp counter */
   ASSERT(!node->cyclone.present);
   SummitInitCyclone(node, parent, cbar);

   return TRUE;
}


#define SUMMIT_TIME_MPMC_REPS      (1000)
/*
 *----------------------------------------------------------------------
 *
 * SummitInitCyclone --
 *
 *      Detect and map IBM Cyclone Jr. chipset on specific node;
 *      add proc node if initialization succeeds.
 *      Note: only the one page containing the MPMC registers are mapped
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
SummitInitCyclone(Summit_NodeInfo *node, Proc_Entry *parent, uint32 cbar)
{
   MPN startMPN;
   CycloneReg *mmioC;
   uint64 lastTime;

   /* Check to make sure nobody else done this node's Cyclone */
   if (node->cyclone.present) {
      Warning("Node %d Cyclone already initialized, this may be a bug",
	      node->nodeId);
      return;
   }

   /* Map MMIO Cyclone address to kernel address space */
   startMPN = MA_2_MPN((MA) cbar + IBM_CYCLONE_OFFSET + IBM_CYCLONE_PMC_OFFSET); 
   
   mmioC = (CycloneReg *) KVMap_MapMPN(startMPN, TLB_UNCACHED);
   if (!mmioC) {
      Warning("Unable to map node %d MMIO Cyclone regs to virtual address",
	      node->nodeId);
      return;
   }      
   ASSERT(!((uint32)mmioC & PAGE_MASK));

   node->cyclone.PMCreg = mmioC;
   
   /* Set up counter to count cycles */
   /* PMCS perf monitoring events:
    * 0x01 - cycles
    */
   mmioC[CYCLONE_PMCS] = 0x00000001;

   /* PMCC perf counter control register
    * For counter 0:
    * bit 24: 1 = 200 MHz
    * bit 16: 0 = continue after rollover
    * bit  8: 0 = count cycles
    * bit  0: 1 = enable
    */
   mmioC[CYCLONE_PMCC] = 0x01000001;

   Summit_CycloneCyclesReg[(int) node->nodeId] = &mmioC[CYCLONE_MPMC0];

   /* Check that the cycles reg is counting */
   lastTime = mmioC[CYCLONE_MPMC0];
   if (mmioC[CYCLONE_MPMC0] == lastTime) {
      Warning("Node %d Cyclone is not counting, disabling", node->nodeId);
      KVMap_FreePages((void *) mmioC);
      return;
   }
   node->cyclone.present = TRUE;

   /* Clear counter */
   mmioC[CYCLONE_MPMC0] = 0;
   node->cyclone.tsZeroed = SummitGetTimestamp();

   /* Add cyclone entry for perf counter i/f */
   Proc_InitEntry(&node->cyclone.procCyclone);
   node->cyclone.procCyclone.parent = parent;
   node->cyclone.procCyclone.read  = SummitProcReadCyclone;
   node->cyclone.procCyclone.write = SummitProcWriteCyclone;
   node->cyclone.procCyclone.private = node;
   Proc_RegisterHidden(&node->cyclone.procCyclone, "cyclone", FALSE);
}


/*
 *----------------------------------------------------------------------
 *
 * SummitMeasureLatency
 *
 *      Measures the average # of CPU cycles needed to read from the 
 *      performance counters of the Twister and Cyclone.  Result is
 *      dumped into the vmkernel log file.  Called from a timer.
 *
 *----------------------------------------------------------------------
 */
static void
SummitMeasureLatency(void *data, UNUSED_PARAM(Timer_AbsCycles timestamp))
{
   Summit_NodeInfo *node = (Summit_NodeInfo *)data;
   uint64 cycTime, twTime, lastTime;
   uint64 readTime;
   int i;

   if (!node->twister.present) {
      Log("No IBM Twister chipset found, skipping test");
      return;
   }
   if (!node->cyclone.present) {
      Log("No IBM Cyclone chipset found, skipping test");
      return;
   }

   NO_INTERRUPTS_BEGIN();

   /* Time how long it takes to read Cyclone, Twister counters */
   twTime = RDTSC();
   for (i=0; i < SUMMIT_TIME_MPMC_REPS; i++) {
      lastTime = node->twister.reg[TWISTER_PMC0];
   }
   twTime = RDTSC() - twTime;

   cycTime = RDTSC();
   for (i=0; i < SUMMIT_TIME_MPMC_REPS; i++) {
      lastTime = node->cyclone.PMCreg[CYCLONE_MPMC0];
   }
   cycTime = RDTSC() - cycTime;

   readTime = RDTSC();
   for (i=0; i < SUMMIT_TIME_MPMC_REPS; i++) {
      lastTime = Timer_GetCycles();
   }
   readTime = RDTSC() - readTime;

   NO_INTERRUPTS_END();

   Log("Reading node %d Twister PMC0 = %Ld cycles  Cyclone MPMC0 = %Ld cycles",
       node->nodeId,
       twTime / SUMMIT_TIME_MPMC_REPS,
       cycTime / SUMMIT_TIME_MPMC_REPS);
   Log("Timer_GetCycles() latency = %Ld cycles",readTime/SUMMIT_TIME_MPMC_REPS);
}


// Stores last address value from write to /proc..../twister.
static uint32 addr = 0;

/*
 *----------------------------------------------------------------------
 *
 * SummitProcReadTwister() --
 *
 *      Called when /proc/vmware/NUMA/nodeN/twister is dumped out.
 *      Writes out IBM x440 Twister chipset performance counters.
 *
 * Results:
 *      Returns VMK_OK.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
SummitProcReadTwister(Proc_Entry  *entry,
		    char        *buffer,
		    int         *len)
{
   Summit_NodeInfo *node = (Summit_NodeInfo *) entry->private;
   uint64 timenow;
   uint64 pcval[4];
   int i;

   ASSERT(node != NULL);
   *len = 0;

   /* Dump out some Twister register contents */
   Proc_Printf(buffer, len, "Twister ID %4x", node->twister.ID);
   switch (node->twister.ID) {
   case TWISTER3_ID:
      Proc_Printf(buffer, len, " (x445)\n");
      break;
   case TWISTER_ID:
      Proc_Printf(buffer, len, " (x440)\n");
      break;
   default:
      Proc_Printf(buffer, len, "\n");
      break;
   }
   Proc_Printf(buffer, len, "PMCC   = 0x%08Lx\n", node->twister.reg[TWISTER_PMCC]);
   Proc_Printf(buffer, len, "PMCS   = 0x%08Lx\n", node->twister.reg[TWISTER_PMCS]);
   Proc_Printf(buffer, len, "PMCS_PQ = 0x%08Lx\n", node->twister.reg[TWISTER_PMCS_PQ]);
   Proc_Printf(buffer, len, "PMCS_Q  = 0x%08Lx\n", node->twister.reg[TWISTER_PMCS_QUAD]);
   Proc_Printf(buffer, len, "0x%04X = 0x%08Lx\n", addr, node->twister.reg[addr/8]);

   /* Read counters quickly to minimize skew between them */
   pcval[0] = node->twister.reg[TWISTER_PMC0] & TWISTER_PMC_MASK;
   pcval[1] = node->twister.reg[TWISTER_PMC1] & TWISTER_PMC_MASK;
   pcval[2] = node->twister.reg[TWISTER_PMC2] & TWISTER_PMC_MASK;
   pcval[3] = node->twister.reg[TWISTER_PMC3] & TWISTER_PMC_MASK;
   timenow = SummitGetTimestamp();

   /* Dump out Twister performance counters */
   for (i=0; i<4; i++) {
      if (UNLIKELY(timenow == node->twister.tsZeroed[i])) {
	 timenow++;
      }
      Proc_Printf(buffer, len, "Counter %d\t\t%15" FMT64 "d\t"
		  "%10Ld per million bus cycles\n", i, pcval[i],
		  pcval[i] / ((timenow - node->twister.tsZeroed[i])/1000000L) );
   }

   return (VMK_OK);
}


static int
SummitProcReadCyclone(Proc_Entry  *entry,
		    char        *buffer,
		    int         *len)
{
   Summit_NodeInfo *node = (Summit_NodeInfo *) entry->private;
   uint64 timenow = SummitGetTimestamp();
   uint64 pcval = Summit_CycloneCyclesReg[(int) node->nodeId][0];

   ASSERT(node != NULL);
   *len = 0;

   if (UNLIKELY(timenow == node->cyclone.tsZeroed)) {
      timenow++;
   }
   Proc_Printf(buffer, len, "Cycles Counter\t\t%15" FMT64 "d\t"
	       "%10Ld per million bus cycles\n", pcval,
	       pcval / ((timenow - node->cyclone.tsZeroed)/1000000L) );

   return (VMK_OK);
}


/*
 *----------------------------------------------------------------------
 *
 * SummitProcWriteTwister() --
 *
 *      Called when /proc/vmware/NUMA/nodeN/twister is written to.
 *      Interface to Twister registers and performance counters:
 *      clear             
 *            Clear and restart all perf counters
 *
 *      pc 0 start 1 1b
 *            Start counter 0 with group 1 event 0x1b
 *            Note this also cancels latency mode
 *
 *      pc 3 stop
 *            Stop counter 3
 *
 *      latency start 51
 *            Turn on latency mode and count event 51.  List of events:
 *                 0x51 - L3 cache hit read latency
 *                 0x53 - local memory read latency
 *                 0x54 - remote node read latency
 *                 0x55 - HITM (same node proc-to-proc) latency
 *
 *      addr aaaa       (where aaaa is hex register offset in bytes)
 *      data 01234567   (32-bit hex data)
 *            Write data 01234567 into Twister register aaaa
 *
 * This is intended for changing performance monitoring collection.
 *
 * Results:
 *      Returns VMK_OK, or VMK_BAD_PARAM upon getting invalid input args
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
SummitProcWriteTwister(Proc_Entry  *entry,
		     char        *buffer,
		     int         *len)
{
   Summit_NodeInfo *node = (Summit_NodeInfo *) entry->private;
   char *argv[5];
   int argc;
   uint32 val;

   argc = Parse_Args(buffer, argv, 5);
   if (argc && strncmp(argv[0], "clear", 5) == 0) {
      SummitResetAllTwisterPCs(node);
   } else if (argc == 2) {
      if (Parse_Hex(argv[1], 8, &val) != VMK_OK) {
	 Log("Invalid Twister addr/data arg %s", argv[1]);
	 return(VMK_BAD_PARAM);
      }
      if (strcmp(argv[0], "addr") == 0) {
	 addr = (val & 0x7ffff);
      } else if (strcmp(argv[0], "data") == 0) {
	 node->twister.reg[addr / 8] = val;
      }
   } else if (argc >= 3 && 
	      strncmp(argv[0], "pc", 2)==0 ) {
      /* perf counter controls */
      int counterNum;
      int group;
      if (Parse_Int(argv[1], 1, &counterNum) != VMK_OK ||
	  counterNum > 3) {
	 Log("Invalid counter # '%s'", argv[1]);
	 return(VMK_BAD_PARAM);
      }
      if (argc == 5 && strcmp(argv[2], "start")==0 ) {
	 /* start new perf counter */
	 if (Parse_Hex(argv[3], 1, &group) != VMK_OK) {
	    Log("Invalid group arg %s", argv[3]);
	    return(VMK_BAD_PARAM);
	 }
	 if (Parse_Hex(argv[4], 2, &val) != VMK_OK) {
	    Log("Invalid event arg %s", argv[4]);
	    return(VMK_BAD_PARAM);
	 }
	 SummitDisableTwisterLatencyMode(node);
	 SummitConfigTwisterPC(node, counterNum,
			     (Summit_CounterGroup) group,
			     (uint8) val);
	 SummitStartTwisterPC(node, counterNum);
      } else if (strcmp(argv[2], "stop")==0) {
	 SummitStopTwisterPC(node, counterNum);
      }
   } else if (argc >= 3 &&
	      strncmp(argv[0], "lat", 3)==0 ) {
      /* set latency mode */
      if (Parse_Hex(argv[2], 2, &val) != VMK_OK) {
	 Log("Invalid event arg %s", argv[2]);
	 return(VMK_BAD_PARAM);
      }
      SummitEnableTwisterLatencyMode(node, (uint8) val);
   }

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * SummitProcWriteCyclone() --
 *
 *      Called when /proc/vmware/NUMA/nodeN/cyclone is written to.
 *      Right now it simply calls SummitMeasureLatency() to measure
 *      latencies from a given CPU # to this node's Twister/Cyclones.
 *      Output on the log file.
 *      You just echo the PCPU # to measure from.
 *
 * Results:
 *      Returns VMK_OK, or VMK_BAD_PARAM upon getting invalid input args
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
SummitProcWriteCyclone(Proc_Entry  *entry,
		     char        *buffer,
		     int         *len)
{
   Summit_NodeInfo *node = (Summit_NodeInfo *) entry->private;
   int pcpu;

   if (Parse_Int(buffer, 1, &pcpu) != VMK_OK) {
      Log("Invalid PCPU #");
      return(VMK_BAD_PARAM);
   }

   Timer_Add(pcpu, SummitMeasureLatency, 1000,
	     TIMER_ONE_SHOT, node);
   return VMK_OK;
}
