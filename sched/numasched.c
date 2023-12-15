/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * numasched.c --
 *      
 *      NUMA load balancing for cpu scheduler.
 *
 *      Implements an AutoNUMAic(tm) algorithm to maximize memory
 *      locality in multi-node systems. NUMASched uses "soft"
 *      memory and cpu affinity to bind a vsmp to a node. It 
 *      reevaluates these findings periodically (approx. every 5 sec)
 *      in NUMASchedRebalance, which tries to maintain load balance
 *      and swap vsmps to improve locality.
 *
 *      NUMASched can also disable/enable page migration, depending
 *      on a vsmp's current conditions.
 *
 *  TODO:
 *      - some more tuning
 *      - soft migrate-rate?
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "vmkernel.h"
#include "world.h"
#include "numa.h"
#include "memmap.h"
#include "alloc.h"
#include "alloc_inline.h"
#include "parse.h"
#include "timer.h"
#include "util.h"

#include "numasched.h"
#include "cpusched.h"

#define LOGLEVEL_MODULE NUMASched
#include "log.h"

// node placement history tracking
#define NUMASCHED_SHORT_TERM_SAMPLES (10)

// if we don't find a node with 8 megs (arbirtrarily chosen), just 
// place based on node free memory
#define NUMASCHED_MIN_INITIALNODE_PAGES (2000)

typedef struct {
   SP_SpinLock lock;
   CpuMask nodeMasks[NUMA_MAX_NODES];
   uint32 rebalancePeriod;
   Timer_Cycles lastRebalanceTime;
   Proc_Entry procEnt;
   Proc_Entry thresholdsProcEnt;
   Timer_Cycles rebalanceOverheadTime;
   NUMA_Node nextInitialNode;

   uint8 smallestNodePcpus;

   NUMASched_Stats globalStats;

   // latest snapshot
   NUMASched_Snap snap;
   Timer_Cycles prevNodeIdleTime[NUMA_MAX_NODES];

   Timer_Cycles nodeIdleDiff[NUMA_MAX_NODES];
   Timer_Cycles nodeEntitled[NUMA_MAX_NODES];
   int64 nodeOwed[NUMA_MAX_NODES]; 

   // config options
   Bool configRebalance;
   Bool configPageMig;
   uint32 configMigThresh;
} NUMASched;

static NUMASched numaSched;

typedef struct {
   uint32 freePageThresh;
   uint32 pctLocalThresh;
   uint32 nodeHistoryThresh;
   uint32 newMigRate;
} NUMASchedMigRateThreshold;


// table indicating thresholds at which to change page migration rate
#define MAX_NUM_THRESHOLDS (10)

static NUMASchedMigRateThreshold migRateThresholds[MAX_NUM_THRESHOLDS] = {
   { 5,  99, 10, 5 },
   { 6,  95, 10, 10 },
   { 10,  85, 12, 25 },
   { 10,  70, 14, 50 },
   { 20, 55, 16, 75 },
   { 25, 40, 18, 100 },
};

static uint32 numThresholds = 6;

static void NUMASchedRebalance(UNUSED_PARAM(void *ignored),
                               Timer_AbsCycles timestamp);
static void NUMASchedMigRateUpdate(NUMASched_VsmpSnap *vsmpInfo);
static int NUMASchedProcRead(Proc_Entry *entry, char *buffer, int *len);
static int NUMASchedProcWrite(Proc_Entry *entry, char *buffer, int *len);
static int NUMASchedThresholdsProcRead(Proc_Entry *entry, char *buffer, int *len);
static int NUMASchedThresholdsProcWrite(Proc_Entry *entry, char *buffer, int *len);
static NUMA_Node NUMASchedInitialPlacement(World_Handle *world);

/*
 *-----------------------------------------------------------------------------
 *
 * NUMASchedPagesOnNode --
 *
 *     Returns the number of pages that "world" currently has on "node"
 *
 * Results:
 *     Returns num pages
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE uint32
NUMASchedPagesOnNode(World_Handle *world, NUMA_Node node)
{
   return Atomic_Read(&Alloc_AllocInfo(world)->pagesPerNode[node]);
}

/*
 *-----------------------------------------------------------------------------
 *
 * NUMASchedPercentPagesOnNode --
 *
 *     Returns the percentage of world's pages that are on "node"
 *
 * Results:
 *     Returns percentage
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE uint32 
NUMASchedPercentPagesOnNode(World_Handle *world, NUMA_Node node)
{
   NUMA_Node n;
   uint32 totalPages = 0;
   
   NUMA_FORALL_NODES(n) {
      totalPages += NUMASchedPagesOnNode(world, n);
   }

   if (totalPages > 0) {
      // OPT: could use 1024 instead, because we don't really care that it's a percentage
      return (100 * NUMASchedPagesOnNode(world, node)) / totalPages;
   } else {
      return 0;
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * NUMASchedPercentAnonPagesOnNode --
 *
 *     Returns the percentage of a "world"'s overhead memory located on "node"
 *
 * Results:
 *     Returns the percentage of a "world"'s overhead memory located on "node"
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE uint32
NUMASchedPercentAnonPagesOnNode(World_Handle *world, NUMA_Node node)
{
   NUMA_Node n;
   uint32 totalPages = 0;

   NUMA_FORALL_NODES(n) {
      totalPages += Atomic_Read(&Alloc_AllocInfo(world)->anonPagesPerNode[node]);
   }

   if (totalPages > 0) {
      return (100 * Atomic_Read(&Alloc_AllocInfo(world)->anonPagesPerNode[node]))
         / totalPages;
   } else {
      return 0;
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * WorldNUMAInfo --
 *
 *     Simple wrapper to grab VsmpInfo, given a world handle
 *
 * Results:
 *     Returns the world's NUMASched_VsmpInfo
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE NUMASched_VsmpInfo*
WorldNUMAInfo(World_Handle *world)
{
   ASSERT(World_CpuSchedVsmp(world));
   return &(World_CpuSchedVsmp(world)->numa);
}

/*
 *-----------------------------------------------------------------------------
 *
 * NUMASched_SetInitialHomeNode --
 *
 *   Sets this VM's home to the most appropriate node, if possible.
 *
 *   Does not apply to vsmps with hard affinity set or to non-VMM worlds
 *   Caller must NOT hold the cpuSched.lock
 *
 * Results:
 *     None
 *
 * Side effects:
 *     Changes vcpu's home node
 *
 *-----------------------------------------------------------------------------
 */
void
NUMASched_SetInitialHomeNode(World_Handle *world)
{
   NUMA_Node n, bestNode = 0;
   NUMASched_VsmpInfo *info = WorldNUMAInfo(world);
   
   if (!CONFIG_OPTION(NUMA_REBALANCE)) {
      info->homeNode = INVALID_NUMANODE;
      return;
   } else if (!World_IsVMMWorld(world)
              || World_CpuSchedVsmp(world)->vcpus.len > numaSched.smallestNodePcpus
              || CpuSched_WorldHasHardAffinity(world)) {
      // note: very unlikely race when checking vcpus.len, because we don't
      // hold the cpuSched.lock, but it's ok, because we'll check again
      // when rebalance runs
      bestNode = INVALID_NUMANODE;
   } else if (info->homeNode != INVALID_NUMANODE) {
      // already assigned a home node
      bestNode = info->homeNode;
   } else {
      // default case, no home node assigned yet:
      // try to find a node where this VM already has memory allocated
      // this will come into play if the VM has already been running,
      // but was recently added (or re-added) to the NUMA scheduler
      NUMA_FORALL_NODES(n) {
         if (NUMASchedPagesOnNode(world, n) > NUMASchedPagesOnNode(world, bestNode)) {
            bestNode = n;
         }
      }
      
      // no node has very many pages on it (probably just started this world),
      // so go through initial placement
      if (NUMASchedPagesOnNode(world, bestNode) < NUMASCHED_MIN_INITIALNODE_PAGES) {
         bestNode = NUMASchedInitialPlacement(world);
      }
   }

   // note that we still need to call SetHomeNode even on non-primary
   // vcpus, so that the affinity mask on the new vcpu gets set propery
   VMLOG(1, world->worldID, "initial homenode: %u", bestNode);
   CpuSched_SetHomeNode(world, bestNode);
}


/*
 *-----------------------------------------------------------------------------
 *
 * NUMASchedInitialPlacement --
 *
 *     Helper function for above. Used iff a VM has no previous
 *     home node set, it meets our requirements for numasched management,
 *     and it doesn't already have a lot of memory allocated on a node.
 *
 *     May use round-robin placement or placement on node with most free
 *     memory, depending on NUMARoundRobin config option.
 *
 * Results:
 *     Returns the node on which this world should be placed
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static NUMA_Node
NUMASchedInitialPlacement(World_Handle *world)
{
   NUMA_Node bestNode = 0;
   NUMA_Node n;

   if (CONFIG_OPTION(NUMA_ROUND_ROBIN)) {
      // round-robin (race here is rare and unimportant)
      bestNode = numaSched.nextInitialNode;
      numaSched.nextInitialNode = (numaSched.nextInitialNode + 1) % NUMA_GetNumNodes();
      VMLOG(2, world->worldID, "round-robin selects node %u", bestNode);
   } else {
      NUMA_FORALL_NODES(n) {
         if (MemMap_NodeFreePages(n) > MemMap_NodeFreePages(bestNode)) {
            bestNode = n;
         }
      }
   }

   return (bestNode);
}

/*
 *-----------------------------------------------------------------------------
 *
 * NUMASched_GetNodeMask --
 *
 *     Obtains the PCPU mask corresponding to node "n"
 *
 * Results:
 *     Returns mask containing all pcpus on node "n"
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
CpuMask
NUMASched_GetNodeMask(NUMA_Node n) {
   return numaSched.nodeMasks[n];
}


/*
 *-----------------------------------------------------------------------------
 *
 * NUMASchedCanMigrate --
 *
 *     Determines whether this vsmp is a candidate for migration, etc.
 *
 * Results:
 *     Returns TRUE iff we're capable of NUMASched-migrating this vsmp
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE Bool
NUMASchedCanMigrate(const NUMASched_VsmpSnap *vsmpInfo)
{
   return (vsmpInfo->isVMMWorld 
           && vsmpInfo->numVcpus <= numaSched.smallestNodePcpus
           && vsmpInfo->node != INVALID_NUMANODE
           && !vsmpInfo->justMigrated
           && !vsmpInfo->hardCpuAffinity
           && !vsmpInfo->hardMemAffinity);
}


/*
 *-----------------------------------------------------------------------------
 *
 * NUMASchedLocalitySwap --
 *
 *     Try to swap vsmps between nodes in order to improve locality.
 *     This is our second-tier concern (after rebalancing for fairness),
 *     but still important. Basically, we want to consider the net change in
 *     locality that would be produced by swapping any pair of vsmps
 * 
 *     The algorithm is simply:
 *           localityDiff_N = %pages_local_after_swap_N - %pages_local_currently_N
 *           netChange = localityDiff_1 + localityDiff_2
 *           if (netChange > NUMASchedMigImbalanceThreshold)
 *               then swap_home_nodes(vsmp_1,vsmp_2)
 *
 *     Note that at most one swap will be conducted per call. If multiple pairs
 *     of vsmps would be eligible for swapping, the pair that produces the greatest
 *     net benefit in memory locality will be chosen.
 *
 *     The LocalitySwap algorithm does not take into account the amount of free
 *     memory on a node. In my opinion, this problem will correct itself 
 *     automagically -- if a node is out of memory, requests for allocations
 *     there will spill over to other nodes, so vsmps will have a smaller percentage
 *     of local memory on the maxed-out node and will begin to migrate away from
 *     it (or at least stop migrating towards it).
 *
 * Results:
 *     Returns TRUE iff a swap actually took place
 *
 * Side effects:
 *     May set new home nodes on a pair of vsmps
 *
 *-----------------------------------------------------------------------------
 */
static Bool
NUMASchedLocalitySwap(void)
{
   uint32 i, j;
   NUMASched_VsmpSnap *maxA = NULL, *maxB = NULL;
   int32 migrateDiffMax = 0;

   LOG(1, "considering locality swap");

   // this may be slow, so we should have interrupts enabled
   ASSERT_HAS_INTERRUPTS();

   // Note: algorithm runs in O(N^2), but don't worry:
   // with 25 vms on an 8-way box, vmkstats shows it to have less than 0.001% overhead
  
   // Loop over all possible pairs of vsmps and find the best candidate for swapping
   for (i=0; i < numaSched.snap.numVsmps; i++) {
      NUMASched_VsmpSnap *infoA = &numaSched.snap.vsmps[i];
      World_Handle *leaderA;

      if (!NUMASchedCanMigrate(infoA)) {
         VMLOG(2, infoA->leaderID, "Can't migrate (hardAffin=%d, numVcpus=%d)", 
               infoA->hardCpuAffinity,
               infoA->numVcpus);
         continue;
      }

      // get a lock on this world, skipping it if we fail
      leaderA = World_Find(infoA->leaderID);
      if (leaderA == NULL) {
         continue;
      }

      for (j=i+1; j < numaSched.snap.numVsmps; j++) {
         int32 localTotal, remoteTotal, migrateDiff;
         World_Handle *leaderB;
         NUMASched_VsmpSnap *infoB = &numaSched.snap.vsmps[j];

         // obviously, don't swap with yourself or with your own node
         if (j==i || !NUMASchedCanMigrate(infoB) || infoA->node == infoB->node) {
            continue;
         }

         // get a lock on this world, skipping it if we fail
         leaderB = World_Find(infoB->leaderID);
         if (leaderB == NULL) {
            continue;
         }

         // local pages in current situation
         localTotal = (int32) (NUMASchedPercentPagesOnNode(leaderA, infoA->node)
            + NUMASchedPercentPagesOnNode(leaderB, infoB->node));

         // local pages after hypothetical swap of nodes
         remoteTotal = (int32) (NUMASchedPercentPagesOnNode(leaderA, infoB->node)
            + NUMASchedPercentPagesOnNode(leaderB, infoA->node));
         
         migrateDiff = remoteTotal - localTotal;
         LOG(1, "swap? %d<->%d, pre=%d, post=%d, diff=%d", 
             leaderA->worldID, leaderB->worldID, 
             localTotal, remoteTotal, 
             migrateDiff);
            
         // always track the best candidate for a swap
         if (migrateDiff > migrateDiffMax) {
            migrateDiffMax = migrateDiff;
            maxA = infoA;
            maxB = infoB;
         }
         
         World_Release(leaderB);
      }

      World_Release(leaderA);
   }

   // determine whether we should do a swap at all
   if (migrateDiffMax > (int32)CONFIG_OPTION(NUMA_SWP_LOCALITY_THRESHOLD)
       && maxA != NULL && maxB != NULL) {
      World_Handle *leaderA, *leaderB;
      NUMA_Node temp;

      leaderA = World_Find(maxA->leaderID);
      if (leaderA == NULL) {
         LOG(0, "Could not find world %d for swap", maxA->leaderID);
         return FALSE;
      }
      leaderB = World_Find(maxB->leaderID);
      if (leaderB == NULL) {
         LOG(0, "Could not find world %d for swap", maxB->leaderID);
         World_Release(leaderA);
         return FALSE;
      }

      LOG(1, "locality swap: vsmp %d and vsmp %d", 
          maxA->leaderID, maxB->leaderID);
      
      // swap A and B, don't need a temp var because maxA/maxB is a snapshot
      CpuSched_SetHomeNode(leaderA, maxB->node);
      CpuSched_SetHomeNode(leaderB, maxA->node);
     
      // update stats
      WorldNUMAInfo(leaderA)->stats.nLocalitySwap++;
      WorldNUMAInfo(leaderB)->stats.nLocalitySwap++;
      numaSched.globalStats.nLocalitySwap++;

      // update vsmpInfo too (MigrateRateControl cares)
      temp = maxA->node;
      maxA->node = maxB->node;
      maxB->node = temp;
      
      World_Release(leaderA);
      World_Release(leaderB);

      // consider updating migrate rates
      NUMASchedMigRateUpdate(maxA);
      NUMASchedMigRateUpdate(maxB);

      return TRUE; 
   } else {
      LOG(1, "No locality swap");
      return FALSE;
   }   
}

/*
 *-----------------------------------------------------------------------------
 *
 * NUMASchedLoadBalance --
 *
 *     Consider moving a vsmp from "maxNode" to "minNode" in order to improve load
 *     balancing and fairness. We select the vsmp from maxNode that gets the greatest
 *     net locality benefit from the maxNode->minNode migration (or that suffers
 *     the lowest reduction in memory locality). 
 * 
 *     Try to prevent thrashing by never moving a single vsmp in two back-to-back
 *     rebalancing sessions. Also, if moving a vsmp would create such an imbalance
 *     that it would probably require another rebalance in the opposite direction
 *     at the next balancing interval, don't do it.
 *
 * Results:
 *     Returns TRUE iff a vsmp had its home node moved from maxNode to minNode
 *
 * Side effects:
 *     May set the home node of a vsmp
 *
 *-----------------------------------------------------------------------------
 */
Bool
NUMASchedLoadBalance(NUMA_Node maxNode, 
                     int64 maxNodeOwed, 
                     NUMA_Node minNode, 
                     int64 minNodeOwed)
{
   uint32 i;
   int64 owedDiff;
   int32 maxMemDiff = -101;
   NUMASched_VsmpSnap *bestVsmp = NULL;
   
   owedDiff = maxNodeOwed - minNodeOwed;
   
   if (maxNode == INVALID_NUMANODE 
       || minNode == INVALID_NUMANODE 
       || maxNode == minNode 
       || owedDiff < numaSched.configMigThresh) {
      return FALSE;
   }

   // find a vsmp on the maxNode that we can migrate
   for (i=0; i < numaSched.snap.numVsmps; i++) {
      int32 memDiff;
      World_Handle *vsmpLeader;
      NUMASched_VsmpSnap *vsmpInfo = &numaSched.snap.vsmps[i];
      int64 vsmpOwed = vsmpInfo->owed;

      if (vsmpInfo->node != maxNode
          || vsmpOwed <= 0
          || !vsmpInfo->valid
          || !NUMASchedCanMigrate(vsmpInfo)) {
         continue;
      }
      
      VMLOG(2, vsmpInfo->leaderID, "consider mig, owed=%Ld, minNodeOwed=%Ld, maxNodeOwed=%Ld", 
            Timer_TCToMS(vsmpOwed), 
            Timer_TCToMS(minNodeOwed), 
            Timer_TCToMS(maxNodeOwed));
      
      // don't overcompensate! we don't want to end up thrashing between nodes
      if (minNodeOwed + vsmpOwed > maxNodeOwed - vsmpOwed + numaSched.configMigThresh
          || minNodeOwed + (2*vsmpOwed) - maxNodeOwed >= owedDiff) {
         VMLOG(1, vsmpInfo->leaderID,
               "prevent thrash: "
               "(minNodeOwed + vsmpOwed = %Ld, "
               "maxNodeOwed - vsmpOwed = %Ld, "
               "thresh = %Ld)", 
               Timer_TCToMS(minNodeOwed + vsmpOwed), 
               Timer_TCToMS(maxNodeOwed - vsmpOwed), 
               Timer_TCToMS(numaSched.configMigThresh));
         continue;
      }

      vsmpLeader = World_Find(vsmpInfo->leaderID);
      if (vsmpLeader == NULL) {
         continue;
      }

      // track the "bestVsmp," i.e. the one with the best change in memory
      // after moving from maxNode to minNode
      memDiff = NUMASchedPercentPagesOnNode(vsmpLeader, minNode)
         - NUMASchedPercentPagesOnNode(vsmpLeader, maxNode);
      if (memDiff > maxMemDiff) {
         bestVsmp = vsmpInfo;
         maxMemDiff = memDiff;
      }
      World_Release(vsmpLeader);
   }

   if (bestVsmp != NULL) {
      // we found a good candidate to migrate 
      World_Handle *leader = World_Find(bestVsmp->leaderID);
      if (leader == NULL) {
         LOG(0, "could not find world %d to migrate", bestVsmp->leaderID);
         return FALSE;
      }
      VMLOG(1, bestVsmp->leaderID, "NUMAMIG: old=%u, new=%u, worldDiff %d", 
            bestVsmp->node,  minNode, maxMemDiff);
       
      // we found a vsmp to migrate
      CpuSched_SetHomeNode(leader, minNode);
      bestVsmp->node = minNode;

      // update stats
      WorldNUMAInfo(leader)->stats.nBalanceMig++;
      numaSched.globalStats.nBalanceMig++;

      World_Release(leader);

      // consider updating migrate rates
      NUMASchedMigRateUpdate(bestVsmp);

      return TRUE;
   }
   
   // no migration
   return FALSE;
}



/*
 *-----------------------------------------------------------------------------
 *
 * NUMASchedVsmpRebalanceCompute --
 *
 *     Fill the balance-related fields of vsmpInfo with data about this vsmp's
 *     owed and wasted cycles, for later use in rebalancing calculations
 *
 *     TODO: explore alternative balance metrics, such as a mix of idle time
 *           and "extrasec," etc.
 *
 * Results:
 *     None
 *
 * Side effects:
 *     Modifies vsmpInfo
 *
 *-----------------------------------------------------------------------------
 */
static void
NUMASchedVsmpRebalanceCompute(NUMASched_VsmpSnap *vsmpInfo, 
                              Timer_Cycles timeDiff, 
                              uint32 totalShares)
{
   Timer_Cycles nodeIdle;
   int64 competed, wasted;
   NUMA_Node node;
   
   if (!vsmpInfo->valid || totalShares == 0 || vsmpInfo->node == INVALID_NUMANODE) {
      return;
   }

   node = vsmpInfo->node;
   nodeIdle = numaSched.nodeIdleDiff[node];

   // compute entitled and owed cycles for this vsmp
   competed = vsmpInfo->runDiff + vsmpInfo->readyDiff;
   vsmpInfo->entitled = MIN(vsmpInfo->shares * ((numPCPUs * timeDiff) / totalShares), competed);
   vsmpInfo->owed = vsmpInfo->entitled - vsmpInfo->runDiff;

   // adjust for wasted cycles (intersection of my wait time and node's idle time)
   wasted = MIN(vsmpInfo->waitDiff, nodeIdle);   
   if (vsmpInfo->owed > 0) {
      // Well, it's our fault that we wasted these cycles, 
      // so we weren't as badly cheated as we thought
      vsmpInfo->owed -= wasted;
      vsmpInfo->owed = MAX(vsmpInfo->owed, 0);
   } else {
      // Essentially, when we waited, the node went idle, so we weren't
      // cheating anybody out of their time by exceeding our entitlement then
      vsmpInfo->owed += wasted;
      vsmpInfo->owed = MIN(vsmpInfo->owed, 0);
   }

   // ignore worlds that slept forever and were thus entitled to nothing
   if (vsmpInfo->entitled > 0) {
      VMLOG(2, vsmpInfo->leaderID,
            "owed (%Ld), entitled (%Ld), shares (%d), node (%d)",
            Timer_TCToMS(vsmpInfo->owed),
            Timer_TCToMS(vsmpInfo->entitled),
            vsmpInfo->shares,
            vsmpInfo->node);
      
      // add owed, entitled to the node's total counts
      numaSched.nodeEntitled[node] += vsmpInfo->entitled;
      numaSched.nodeOwed[node] += vsmpInfo->owed;
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * NUMASchedMigRateUpdate --
 *
 *     Analyzes vsmpInfo's history and current node to see if it should 
 *     migrate pages towards its current memory affinity node.
 *
 * Results:
 *     None
 *
 * Side effects:
 *     May change page migration rate
 *
 *-----------------------------------------------------------------------------
 */
static void
NUMASchedMigRateUpdate(NUMASched_VsmpSnap *vsmpInfo)
{
   NUMA_Node home = vsmpInfo->node;
   World_Handle *leader;
   uint32 newMigRate, pctLocal, nodeHistory, nodeFreeMemPct;
   uint32 i, oldMigRate;

   // only worry about VMMs with home nodes
   if (!vsmpInfo->isVMMWorld || vsmpInfo->node == INVALID_NUMANODE) {
      return;
   }

   // don't want world to disappear
   leader = World_Find(vsmpInfo->leaderID);
   if (leader == NULL) {
      return;
   }

   // three basic criteria:
   nodeHistory = vsmpInfo->longTermHistory[home];
   nodeFreeMemPct = MemMap_NodePctMemFree(home);
   pctLocal = NUMASchedPercentPagesOnNode(leader, home);

   // find the threshold that we fall under, and use it to set our migRate
   newMigRate = 0;
   for (i=0; i < numThresholds; i++) {
      if (nodeFreeMemPct < migRateThresholds[i].freePageThresh ||
          pctLocal > migRateThresholds[i].pctLocalThresh ||
          nodeHistory < migRateThresholds[i].nodeHistoryThresh) {
         break;
      } else {
         newMigRate = migRateThresholds[i].newMigRate;
      }
   }

   // maintain stats
   oldMigRate = MemSched_GetMigRate(leader);
   if (newMigRate > 0 && oldMigRate == 0) {
      WorldNUMAInfo(leader)->stats.nPageMigOn++;
      numaSched.globalStats.nPageMigOn++;
   } else if (newMigRate == 0 && oldMigRate > 0) {
      WorldNUMAInfo(leader)->stats.nPageMigOff++;
      numaSched.globalStats.nPageMigOff++;
   } else if (newMigRate > oldMigRate) {
      WorldNUMAInfo(leader)->stats.nPageMigIncr++;
      numaSched.globalStats.nPageMigIncr++;
   } else if (newMigRate < oldMigRate) {
      WorldNUMAInfo(leader)->stats.nPageMigDecr++;
      numaSched.globalStats.nPageMigDecr++;
   }

   // actually implement the new rate
   if (oldMigRate != newMigRate) {
      MemSched_SetMigRate(leader, newMigRate);
   }
   LOG(1, "newMigRate -- vsmp: %d, node: %d, rate: %d", 
       leader->worldID, home, newMigRate);

   World_Release(leader);
}


/*
 *-----------------------------------------------------------------------------
 *
 * NUMASchedMonitorMigConsider --
 *
 *     Determines whether to migrate the monitor for the vsmp "snap"
 *     Actually initiates the migration if appropriate
 *
 * Results:
 *     Returns TRUE iff monitor was migrated
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static Bool
NUMASchedMonitorMigConsider(NUMASched_VsmpSnap *snap)
{
   NUMA_Node node;
   Bool didMig = FALSE;

   if (!snap->isVMMWorld 
       || snap->justMigrated 
       || snap->node == INVALID_NUMANODE) {
      return (FALSE);
   }

   node = snap->node;

   if (snap->longTermHistory[node] > CONFIG_OPTION(NUMA_MONMIG_HISTORY)) {
      World_Handle *leader  = World_Find(snap->leaderID);

      if (leader) {
         CpuSched_Vsmp *vsmp;
         uint32 pctLocal = NUMASchedPercentPagesOnNode(leader, node);

         vsmp = World_CpuSchedVsmp(leader);

         // if most of our memory is remote, and we didn't migrate TO 
         // this node last time, we can initiate a monitor migration
         if (pctLocal < CONFIG_OPTION(NUMA_MONMIG_LOCALITY)
             && (MEMSCHED_NODE_AFFINITY(snap->node) & vsmp->numa.lastMonMigMask) == 0) {
            if (MemSched_NumaMigrateVMM(leader) == VMK_OK) {
               didMig = TRUE;
               numaSched.globalStats.nMonMigs++;
               WorldNUMAInfo(leader)->stats.nMonMigs++;
            }
         }
         World_Release(leader);
      }
   }

   return (didMig);
}

/*
 *-----------------------------------------------------------------------------
 *
 * NUMASchedReinstallTimer --
 *
 *     Utility function to re-add our rebalance timer, rotating between pcpus
 *
 * Results:
 *     None
 *
 * Side effects:
 *     Adds timer
 *
 *-----------------------------------------------------------------------------
 */
static void
NUMASchedReinstallTimer(void)
{
   // round-robin pcpu for timer
   Timer_Add((MY_PCPU + 1) % numPCPUs, 
             NUMASchedRebalance,
             CONFIG_OPTION(NUMA_REBALANCE_PERIOD),
             TIMER_ONE_SHOT,
             NULL);
}

/*
 *-----------------------------------------------------------------------------
 *
 * NUMASchedCpuAllNodeAffinity --
 *
 *      Returns TRUE iff the vsmp corresponding to "snap" could safely
 *      run on all NUMA nodes, i.e. if it has at least "numVcpus"
 *      affinity-permitted pcpus on each node.
 *
 * Results:
 *      Returns TRUE iff vsmp can run on all nodes, FALSE otherwise
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE Bool
NUMASchedCpuAllNodeAffinity(const NUMASched_VsmpSnap *snap)
{
   NUMA_Node n;
   CpuMask affinity;
   
   if (!snap->hardCpuAffinity) {
      // no constraints, can go anywhere
      return (TRUE);
   }
   if (!snap->jointAffinity) {
      // we don't know how to deal with joint affinity here
      return (FALSE);
   }

   // see if we have at least "numVcpus" cpus in our
   // affinity mask on every node
   affinity = snap->totalCpuAffinity;
   NUMA_FORALL_NODES(n) {
      uint8 numPackages;
      CpuMask nodeMask;

      nodeMask = numaSched.nodeMasks[n] & affinity;
      numPackages = CpuSched_NumAffinityPackages(nodeMask);

      // need at least one package per vcpu in this node
      if (numPackages < snap->numVcpus) {
         VMLOG(2, snap->leaderID,
               "only have affinity to %u packages on node %u, need %u",
               numPackages,
               n,
               snap->numVcpus);
         return (FALSE);
      }
   }

   VMLOG(2, snap->leaderID, "has affinity for all nodes");
   return (TRUE);
}

/*
 *-----------------------------------------------------------------------------
 *
 * NUMASchedCpuAffinityNode --
 *
 *     If affinity is a subset of the cpus on a node N, return N, otherwise
 *     return INVALID_NUMANODE
 *
 * Results:
 *     Returns the numa node number corresponding to affinity, or INVALID_NUMANODE
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE NUMA_Node
NUMASchedCpuAffinityNode(CpuMask affinity)
{
   NUMA_Node n;

   NUMA_FORALL_NODES(n) {
      if ((affinity & numaSched.nodeMasks[n]) == affinity) {
         return n;
      }
   }

   return (INVALID_NUMANODE);
}

/*
 *-----------------------------------------------------------------------------
 *
 * NUMASchedUpdateManagedStatus --
 *
 *     We may have lost or gained the ability to NUMASched-manage this
 *     vsmp since the last rebalance point. Update its home node 
 *     and "valid" bit accordingly, setting a new "initial placement" on a
 *     vsmp that we could not previously control.
 *
 *     We can only manage vsmps that are VMMs with fewer vcpus than the
 *     smallest node has pcpus and that don't have hard memory affinity set.
 * 
 *     If a vsmp has hard cpu affinity set and that affinity maps it to a single
 *     node, or to a subset of the pcpus on a single node, we'll detect that
 *     situation and automatically set the vsmp's home node accordingly. We
 *     will not try to migrate such a vsmp, but we will set its memory
 *     affinity and page migration rates as necessary.
 *  
 *     May grab cpuSched.lock (via CpuSched_SetHomeNode)
 *
 * Results:
 *     None.
 *
 * Side effects:
 *     May modify home node of vsmp corresponding to "snap"
 *     May change "valid" bit of "snap"
 *
 *-----------------------------------------------------------------------------
 */
void
NUMASchedUpdateManagedStatus(NUMASched_VsmpSnap *snap)
{
   Bool canManage;
   NUMA_Node mandatoryNode = INVALID_NUMANODE;
   World_Handle *leader = NULL;

   if (!snap->isVMMWorld 
       || snap->numVcpus > numaSched.smallestNodePcpus
       || snap->hardMemAffinity) {
      canManage = FALSE;
   } else if (snap->hardCpuAffinity
              && !NUMASchedCpuAllNodeAffinity(snap)) {
      // attempt to determine an implicit homde node from an explicit
      // affinity setting
      NUMA_Node cpuNode = NUMASchedCpuAffinityNode(snap->totalCpuAffinity);

      if (cpuNode == INVALID_NUMANODE || CONFIG_OPTION(NUMA_AUTO_MEMAFFINITY) == 0) {
         canManage = FALSE;
      } else {
         canManage = TRUE;
         mandatoryNode = cpuNode;
      }
   } else {
      // vanilla VM case: we can manage
      canManage = TRUE;
   }


   // handle transitions between management states due to changes
   // in a vsmp (e.g. setting or unsetting of affinity)
   if (snap->node == INVALID_NUMANODE && canManage) {
      if (mandatoryNode == INVALID_NUMANODE) {
         // start managing
         leader = World_Find(snap->leaderID);
         if (leader) {
            VMLOG(0, snap->leaderID, "start managing");
            NUMASched_SetInitialHomeNode(leader);
            World_Release(leader);
         }
      } else {
         // send to mandatory node
         leader = World_Find(snap->leaderID);
         if (leader) {
            VMLOG(0, snap->leaderID, "start managing -- mandatory node=%d", mandatoryNode);
            CpuSched_SetHomeNode(leader, mandatoryNode);
            World_Release(leader);
         }
      }
   } else if (snap->node != INVALID_NUMANODE) {
      if (!canManage) {
         // stop managing
         leader = World_Find(snap->leaderID);
         if (leader) {
            VMLOG(0, snap->leaderID, "stop managing");
            CpuSched_SetHomeNode(leader, INVALID_NUMANODE);
            World_Release(leader);
         }
      } else if (mandatoryNode != INVALID_NUMANODE && snap->node != mandatoryNode) {
         // move to a different mandatory node
         leader = World_Find(snap->leaderID);
         if (leader) {
            VMLOG(0, snap->leaderID, "start managing -- mandatory node=%d", mandatoryNode);
            CpuSched_SetHomeNode(leader, mandatoryNode);
            World_Release(leader);
         }
      }

      // note: if a vsmp previously had a mandatory node, but was just changed
      // to unconstrained affinity, then everything "just works," because
      // it will now pass the NUMASchedCanMigrate test, so we can float it
      // around like any other unconstrained VM
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * NUMASchedRebalance --
 *
 *     Timer callback to rebalance between NUMA nodes.
 *     We snapshot the current state of the scheduler, including info about
 *     all running vsmps. Then, we try two strategies to improve cpu balance
 *     and memory locality:
 *     
 *       (1) Move a vsmp from an overloaded node to a lightly-loaded node
 *           (see: NUMASchedLoadBalance)
 *
 *       (2) If load balance is reasonable, consider swapping the home 
 *           nodes of two vsmps to improve memory locality
 *           (see: NUMASchedLocalitySwap)
 *
 *
 *     NUMASchedRebalance also manages page migration policies, based
 *     on long-term run history of the vsmp's home node. Eventually,
 *     this will extend to include management of overhead memory
 *     migration.
 *
 *     Note that this is a very heavyweight process, but it holds no
 *     locks and only runs infrequently, with a period configurable
 *     by NUMASchedRebalancePeriod
 *
 *
 * Results:
 *     None
 *
 * Side effects:
 *     May change the home nodes of one or more vsmps
 *
 *-----------------------------------------------------------------------------
 */
static void
NUMASchedRebalance(UNUSED_PARAM(void *ignored), Timer_AbsCycles timestamp)
{
   NUMA_Node n, maxNode, minNode;
   uint32 i;
   int64 maxNodeOwed, minNodeOwed;
   NUMASched_Snap *snap;
   NUMASched_Stats *gstats;
   Timer_Cycles timeNow, timeDiff;
   Bool migrated;

   SP_Lock(&numaSched.lock);

   // load our config parameters
   numaSched.configRebalance = CONFIG_OPTION(NUMA_REBALANCE);
   numaSched.configPageMig = CONFIG_OPTION(NUMA_PAGE_MIG);
      
   if (!numaSched.configRebalance) {
      goto exit;
   }

   snap = &numaSched.snap;
   memset(snap, 0, sizeof(NUMASched_Snap));

   timeNow = timestamp;
   timeDiff = timeNow - numaSched.lastRebalanceTime;
   numaSched.lastRebalanceTime = timeNow;

   numaSched.configMigThresh = 
      CONFIG_OPTION(NUMA_MIG_THRESHOLD) * Timer_TCToMS(timeDiff) / 1000;

   // take the snapshot
   CpuSched_NUMASnap(snap);

   gstats = &numaSched.globalStats;
   // store previous-hour and previous-minute history
   if ((int64)gstats->minuteAgoCycles < 
       (int64)numaSched.lastRebalanceTime - ((int64)Timer_CyclesPerSecond() * 60)) {
      gstats->minuteAgoLocal = gstats->localPages;
      gstats->minuteAgoRemote = gstats->remotePages;
      gstats->minuteAgoCycles = numaSched.lastRebalanceTime;
   }
   if ((int64)gstats->hourAgoCycles < 
       (int64)numaSched.lastRebalanceTime - ((int64)Timer_CyclesPerSecond() * 60 * 60)) {
      gstats->hourAgoLocal = gstats->localPages;
      gstats->hourAgoRemote = gstats->remotePages;
      gstats->hourAgoCycles = numaSched.lastRebalanceTime;
   }

   // no relevant worlds, don't bother to balance
   if (snap->totalShares == 0) {
      goto exit;
   }

   // compute per-node idle time
   NUMA_FORALL_NODES(n) {
      if (LIKELY(snap->nodeIdleTotal[n] > numaSched.prevNodeIdleTime[n])) {
         numaSched.nodeIdleDiff[n] = snap->nodeIdleTotal[n] - numaSched.prevNodeIdleTime[n];
      } else {
         numaSched.nodeIdleDiff[n] = 0;
      }
      numaSched.prevNodeIdleTime[n] = snap->nodeIdleTotal[n];      
      numaSched.nodeEntitled[n] = 0;
      numaSched.nodeOwed[n] = -((int64)numaSched.nodeIdleDiff[n]);
   }

   // compute owed and entitled for each vsmp and node
   for (i=0; i < snap->numVsmps; i++) {
      NUMASchedUpdateManagedStatus(&snap->vsmps[i]);
      NUMASchedVsmpRebalanceCompute(&snap->vsmps[i], timeDiff, snap->totalShares);
   }

   // find the min and max nodes
   minNode = maxNode = INVALID_NUMANODE;
   minNodeOwed = maxNodeOwed = 0;
   NUMA_FORALL_NODES(n) {
      LOG(1, "Node[%d] entitled = %8Ld, owed = %8Ld, idle = %8Ld",
          n, 
          Timer_TCToMS(numaSched.nodeEntitled[n]),
          Timer_TCToMS(numaSched.nodeOwed[n]),
          Timer_TCToMS(numaSched.nodeIdleDiff[n]));

      // we want the average "owed" amount per cpu in the node
      numaSched.nodeOwed[n] = numaSched.nodeOwed[n] / NUMA_GetNumNodeCpus(n);

      if (numaSched.nodeOwed[n] > maxNodeOwed || maxNode == INVALID_NUMANODE) {
         maxNodeOwed = numaSched.nodeOwed[n];
         maxNode = n;
      }

      if (numaSched.nodeOwed[n] < minNodeOwed || minNode == INVALID_NUMANODE) {
         minNodeOwed = numaSched.nodeOwed[n];
         minNode = n;
      }
   }

   LOG(1, "minNode=%u [%Ld], maxNode=%u [%Ld]",
       minNode, Timer_TCToMS(minNodeOwed),
       maxNode, Timer_TCToMS(maxNodeOwed));

   // balance the nodes for CPU load
   migrated = NUMASchedLoadBalance(maxNode, maxNodeOwed, minNode, minNodeOwed);

   // if CPU load is balanced, try to swap VMs to improve memory locality
   if (!migrated) {
      NUMASchedLocalitySwap();
   }

   for (i=0; i < snap->numVsmps; i++) {
      if (numaSched.configPageMig && snap->vsmps[i].historyUpdate) {
         NUMASchedMigRateUpdate(&snap->vsmps[i]);
         NUMASchedMonitorMigConsider(&snap->vsmps[i]);
      }
   }

   numaSched.rebalanceOverheadTime = Timer_GetCycles() - timeNow;

  exit:
   NUMASchedReinstallTimer();
   SP_Unlock(&numaSched.lock);
}


/*
 *-----------------------------------------------------------------------------
 *
 * NUMASched_Init --
 *
 *     Install the timer and setup the node masks.
 *     Must be called after NUMA_LateInit() and Timer_Init().
 *
 * Results:
 *     None
 *
 * Side effects:
 *     Sets up global structures and timer
 *
 *-----------------------------------------------------------------------------
 */
void 
NUMASched_Init(Proc_Entry *procSchedDir)
{
   PCPU p;
   NUMA_Node n;
   uint8 minNodePcpus = numPCPUs;

   if (NUMA_GetNumNodes() <= 1) {
      return;
   }

   // rank check
   ASSERT(SP_RANK_NUMASCHED < SP_RANK_MEMSCHED);

   SP_InitLock("numaSched", &numaSched.lock, SP_RANK_NUMASCHED);
   
   // init masks
   for (p=0; p < numPCPUs; p++) {
      numaSched.nodeMasks[NUMA_PCPU2NodeNum(p)] |= CPUSCHED_AFFINITY(p);
   }

   // compute the number of pcpus in the smallest node
   // NUMASched can't manage any vsmp bigger than this
   NUMA_FORALL_NODES(n) {
      uint8 numNodePcpus = 0;
      
      NUMA_FORALL_NODE_PCPUS(n, p) {
         numNodePcpus++;
      }
      
      if (numNodePcpus < minNodePcpus) {
         minNodePcpus = numNodePcpus;
      }
   }
   numaSched.smallestNodePcpus = minNodePcpus;
   
   Log("initialized NUMASched");

   // register "sched/numasched" proc node
   Proc_InitEntry(&numaSched.procEnt);
   numaSched.procEnt.parent = procSchedDir;
   numaSched.procEnt.read = NUMASchedProcRead;
   numaSched.procEnt.write = NUMASchedProcWrite;
   Proc_Register(&numaSched.procEnt, "numasched", FALSE);   


   numaSched.thresholdsProcEnt.parent = procSchedDir;
   numaSched.thresholdsProcEnt.read = NUMASchedThresholdsProcRead;
   numaSched.thresholdsProcEnt.write = NUMASchedThresholdsProcWrite;
   Proc_RegisterHidden(&numaSched.thresholdsProcEnt, "NUMASchedThresholds", FALSE);

   // install the timer
   Timer_Add(MY_PCPU, 
             NUMASchedRebalance,
             CONFIG_OPTION(NUMA_REBALANCE_PERIOD),
             TIMER_ONE_SHOT,
             NULL);
}


/*
 *-----------------------------------------------------------------------------
 *
 * NUMASchedUpdateLocalityStats --
 *
 *     Records info on memory locality (percent local and remote pages)
 *     of vsmp. 
 *
 * Results:
 *     None.
 *
 * Side effects:
 *     Modifies vsmp's NUMASched stats
 *
 *-----------------------------------------------------------------------------
 */
void
NUMASchedUpdateLocalityStats(CpuSched_Vsmp *vsmp)
{
   NUMASched_VsmpInfo *info = &vsmp->numa;
   World_Handle *world = vsmp->leader;
   NUMA_Node n;
   uint32 localPages = 0, remotePages = 0;
   NUMASched_Stats *stats = &info->stats;

   
   NUMA_FORALL_NODES(n) {
      if (n == info->homeNode) {
         localPages += NUMASchedPagesOnNode(world, n);
      } else {
         remotePages += NUMASchedPagesOnNode(world, n);
      }
   }
   
   // to compute a moving average of the local/remote memory
   // ratio, we keep a running count of the number of total local
   // and global pages that we've seen over all rebalancing periods
   // and report their ratio on demand.
   stats->localPages += localPages;
   stats->remotePages += remotePages;
   numaSched.globalStats.localPages += localPages;
   numaSched.globalStats.remotePages += remotePages;

   if ((int64)stats->minuteAgoCycles < 
       (int64)numaSched.lastRebalanceTime - ((int64)Timer_CyclesPerSecond() * 60)) {
      stats->minuteAgoLocal = stats->localPages;
      stats->minuteAgoRemote = stats->remotePages;
      stats->minuteAgoCycles = numaSched.lastRebalanceTime;
   }

   if ((int64)stats->hourAgoCycles < 
       (int64)numaSched.lastRebalanceTime - ((int64)Timer_CyclesPerSecond() * 60 * 60)) {
      stats->hourAgoLocal = stats->localPages;
      stats->hourAgoRemote = stats->remotePages;
      stats->hourAgoCycles = numaSched.lastRebalanceTime;
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * NUMASchedUpdateVsmpHistory --
 *
 *     Ages and increments this vsmp's node run history.
 *     Caller must hold cpuSched.lock
 *
 * Results:
 *     None
 *
 * Side effects:
 *     Modifies vsmp->numa structure
 *
 *-----------------------------------------------------------------------------
 */
static void
NUMASchedUpdateVsmpHistory(CpuSched_Vsmp *vsmp, NUMASched_VsmpSnap *snap) {
   NUMASched_VsmpInfo *info = &vsmp->numa;
   int i;

   info->shortTermHistory[info->homeNode]++;
   info->shortTermSamples++;
   
   // fold short term info into long term history
   if (info->shortTermSamples >= NUMASCHED_SHORT_TERM_SAMPLES) {
      info->shortTermSamples = 0;
      for (i=0; i < NUMA_GetNumNodes(); i++) {
         info->longTermHistory[i] >>= 1;
         info->longTermHistory[i] += info->shortTermHistory[i];
         LOG(2, "vsmp %d: history[%d] = %u",
             vsmp->leader->worldID,
             i,
             info->longTermHistory[i]);
         info->shortTermHistory[i] = 0;
      }
      snap->historyUpdate = TRUE;
   } else {
      snap->historyUpdate = FALSE;
   }

   // no need to worry about overflow here
   if (info->homeNode != INVALID_NUMANODE) {
      info->stats.nodeRunCounts[info->homeNode]++;
   }

   // update locality stats
   NUMASchedUpdateLocalityStats(vsmp);
}

/*
 *-----------------------------------------------------------------------------
 *
 * NUMASchedVcpuNUMASnap --
 *
 *     Adds this vcpus run/wait/etc. times to vsmp's totals
 *     Caller must protect vcpu with World_Find locking
 *
 * Results:
 *     Increases in/out parameter values appropriately
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE void
NUMASchedVcpuNUMASnap(const CpuSched_Vcpu *vcpu, 
                      CpuMask      *totalAffinity,  // in/out
                      Timer_Cycles *totalRun,       // in/out
                      Timer_Cycles *totalReady,     // in/out
                      Timer_Cycles *totalWait)      // in/out
{
   *totalRun += vcpu->runStateMeter[CPUSCHED_RUN].elapsed;
   *totalWait += vcpu->runStateMeter[CPUSCHED_WAIT].elapsed
      + vcpu->runStateMeter[CPUSCHED_BUSY_WAIT].elapsed;
   *totalReady += vcpu->runStateMeter[CPUSCHED_READY].elapsed;

   *totalAffinity |= vcpu->affinityMask;
}

/*
 *-----------------------------------------------------------------------------
 *
 * NUMASched_VsmpNUMASnap --
 *
 *     Saves data about "vsmp" into "info"
 *     Caller must hold cpuSched.lock
 *
 * Results:
 *     Saves data about "vsmp" into "info"
 *
 * Side effects:
 *     Modifies vsmp's VsmpInfo
 *
 *-----------------------------------------------------------------------------
 */
void
NUMASched_VsmpNUMASnap(CpuSched_Vsmp *vsmp, NUMASched_VsmpSnap *snap)
{
   Timer_Cycles totalRun = 0, totalReady = 0, totalWait = 0;   
   CpuMask totalAffinity = 0;
   int i;
   
   // ASSERT(CpuSchedIsLocked())
  
   // store basic, non-diff info about the vsmp
   snap->leaderID = vsmp->leader->worldID;
   snap->node = vsmp->numa.homeNode;
   snap->hardCpuAffinity = CpuSched_WorldHasHardAffinity(vsmp->leader);
   snap->hardMemAffinity = MemSched_HasNodeHardAffinity(vsmp->leader);
   snap->jointAffinity = vsmp->jointAffinity;

   snap->shares = vsmp->base.shares;
   if (vsmp->numa.nextMigrateAllowed > numaSched.lastRebalanceTime) {
      snap->justMigrated = TRUE;
   } else {
      snap->justMigrated = FALSE;
   }
   snap->isVMMWorld = World_IsVMMWorld(vsmp->leader);
   snap->numVcpus = vsmp->vcpus.len;
   
   if (vsmp->vcpus.list[0]->idle) {
      snap->valid = FALSE;
      return;
   }

   // sum run, ready, wait times over all vcpus
   for (i=0; i < vsmp->vcpus.len; i++) {
      CpuSched_Vcpu *vcpu = vsmp->vcpus.list[i];

      NUMASchedVcpuNUMASnap(vcpu, &totalAffinity, &totalRun, &totalReady, &totalWait);
   }

   // ignore soft affinity
   if (snap->hardCpuAffinity) {
      snap->totalCpuAffinity = totalAffinity;
   } else {
      snap->totalCpuAffinity = CPUSCHED_AFFINITY_NONE;
   }

   // if these values have decreased, we probably reset stats,
   // so we should consider this interval invalid
   if (totalRun < vsmp->numa.prevRun 
       || totalWait < vsmp->numa.prevWait
       || totalReady < vsmp->numa.prevReady
       || totalRun + totalReady <= 0) {
      snap->valid = FALSE;
   } else {
      snap->valid = TRUE;
   }

   if (snap->valid && World_IsVMMWorld(vsmp->leader)) {
      NUMASchedUpdateVsmpHistory(vsmp, snap);
   }

   // snapshot history info
   memcpy(snap->longTermHistory, 
          vsmp->numa.longTermHistory,
          sizeof(uint32) * NUMA_GetNumNodes());

   // snapshot the diffs
   snap->runDiff = totalRun - vsmp->numa.prevRun;
   snap->readyDiff = totalReady - vsmp->numa.prevReady;
   snap->waitDiff = totalWait - vsmp->numa.prevWait;

   // store the current run, ready, wait times for future reference
   vsmp->numa.prevRun = totalRun;
   vsmp->numa.prevReady = totalReady;
   vsmp->numa.prevWait = totalWait;
}

/*
 *-----------------------------------------------------------------------------
 *
 * NUMASchedUnsetAllHomeNodes --
 *
 *     Sets every vsmp's home node to "INVALID_NUMANODE" and undoes all
 *     soft vsmp cpu/mem affinity. Also sets all page migrate-rates down 
 *     to 0.
 *
 * Results:
 *     None.
 *
 * Side effects:
 *     Grabs cpuSched lock. Modifies NUMA state/affinity for ALL vsmps
 *
 *-----------------------------------------------------------------------------
 */
// helper callback function for UnsetAllHomeNodes
static void
NUMASchedUnsetHomeNodeCB(World_Handle *leader, UNUSED_PARAM(void *data))
{
   if (World_IsVMMWorld(leader)) {
      CpuSched_SetHomeNode(leader, INVALID_NUMANODE);
      MemSched_SetMigRate(leader, 0);
   }
}

static void
NUMASchedUnsetAllHomeNodes(void)
{
   if (CpuSched_ForallGroupLeadersDo(NUMASchedUnsetHomeNodeCB, NULL) == VMK_OK) {
      Log("unset all NUMA home nodes");
   } else {
      Log("failed to unset all NUMA home nodes");
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * NUMASchedProcWrite --
 *
 *     Handles "reset" and "unbind" commands to global numasched proc node
 *
 * Results:
 *     Returns VMK_OK
 *
 * Side effects:
 *     May reset global numasched stats
 *
 *-----------------------------------------------------------------------------
 */
static int
NUMASchedProcWrite(UNUSED_PARAM(Proc_Entry *entry), UNUSED_PARAM(char *buffer), UNUSED_PARAM(int *len))
{ 
   SP_Lock(&numaSched.lock);

   if (strncmp(buffer, "reset", 5) == 0) {
      memset(&numaSched.globalStats, 0, sizeof(NUMASched_Stats));
      CpuSched_ResetNUMAStats();
   } else if (strncmp(buffer, "unbind", 6) == 0) {
      NUMASchedUnsetAllHomeNodes();
   }

   SP_Unlock(&numaSched.lock);

   return(VMK_OK);
}

static uint32 
NUMASchedLocalityPct(uint64 local, uint64 remote)
{
   uint32 pctLocal;

   if (local + remote > 0) {
      pctLocal = (uint32) ((local * 100ull) / (local + remote));
   } else {
      pctLocal = 0;
   }

   return (pctLocal);
}

/*
 *-----------------------------------------------------------------------------
 *
 * NUMASchedPrintStats --
 *
 *     Internal utility function to print a NUMASched_Stats structure
 *     to an output buffer (e.g. a proc read handler buffer)
 *
 * Results:
 *     None
 *
 * Side effects:
 *     Writes to buffer and len
 *
 *-----------------------------------------------------------------------------
 */
static void
NUMASchedPrintStats(char *buffer, int *len, NUMASched_Stats *stats)
{
   uint32 pctLocalNow, pctLocalMinute, pctLocalHour, hourAgoUsec, minuteAgoUsec;
   uint64 hourAgoSec, minuteAgoSec;
   Timer_AbsCycles now = Timer_GetCycles();

   pctLocalNow = NUMASchedLocalityPct(stats->localPages, stats->remotePages);
   pctLocalMinute = NUMASchedLocalityPct(stats->localPages - stats->minuteAgoLocal, 
                                         stats->remotePages - stats->minuteAgoRemote);
   pctLocalHour = NUMASchedLocalityPct(stats->localPages - stats->hourAgoLocal, 
                                       stats->remotePages - stats->hourAgoRemote);
   Timer_TCToSec(now - stats->minuteAgoCycles, &minuteAgoSec, &minuteAgoUsec);
   Timer_TCToSec(now - stats->hourAgoCycles, &hourAgoSec, &hourAgoUsec);

   Proc_Printf(buffer, len, "balanceMig:    %u\n", stats->nBalanceMig);
   Proc_Printf(buffer, len, "localitySwap:  %u\n", stats->nLocalitySwap);
   Proc_Printf(buffer, len, "pageMigOn:     %u\n", stats->nPageMigOn);
   Proc_Printf(buffer, len, "pageMigOff:    %u\n", stats->nPageMigOff);
   Proc_Printf(buffer, len, "pageMigIncr:   %u\n", stats->nPageMigIncr);
   Proc_Printf(buffer, len, "pageMigDecr:   %u\n", stats->nPageMigDecr);
   Proc_Printf(buffer, len, "monMigs:       %u\n", stats->nMonMigs);
   
   Proc_Printf(buffer, len, "pctLocalTot:   %u%%\n", pctLocalNow);
   Proc_Printf(buffer, len, "pctLocalMin:   %u%% (  %2Lus ago)\n", 
               pctLocalMinute, minuteAgoSec);
   Proc_Printf(buffer, len, "pctLocalHr:    %u%% (%4Lus ago) \n", 
               pctLocalHour, hourAgoSec);
   Proc_Printf(buffer, len, "localPgSamp:   %Lu\n", stats->localPages);
   Proc_Printf(buffer, len, "remotePgSamp:  %Lu\n", stats->remotePages);
}

/*
 *-----------------------------------------------------------------------------
 *
 * NUMASchedProcRead --
 *
 *     Proc handler to print basic global NUMASched stats
 *
 * Results:
 *     Returns VMK_OK
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static int
NUMASchedProcRead(UNUSED_PARAM(Proc_Entry *entry), char *buffer, int *len)
{
   NUMA_Node n;
   *len = 0;

   SP_Lock(&numaSched.lock);

   NUMASchedPrintStats(buffer, len, &numaSched.globalStats);
   
   // print stats about last rebalance
   Proc_Printf(buffer, len, "\nLast rebalance %Lu msec ago\n",
               Timer_TCToMS(Timer_GetCycles() - numaSched.lastRebalanceTime));
   Proc_Printf(buffer, len, "Last rebalance took %Lu msec\n\n",
               Timer_TCToMS(numaSched.rebalanceOverheadTime));

   Proc_Printf(buffer, len,    "node       idle      entitled      owed\n");
   NUMA_FORALL_NODES(n) {
      Proc_Printf(buffer, len, "  %2u   %8Lu      %8Lu  %8Ld\n",
                  n,
                  Timer_TCToMS(numaSched.nodeIdleDiff[n]),
                  Timer_TCToMS(numaSched.nodeEntitled[n]),
                  Timer_TCToMS(numaSched.nodeOwed[n]));
   }

   SP_Unlock(&numaSched.lock);

   return(VMK_OK);
}


/*
 *-----------------------------------------------------------------------------
 *
 * NUMASchedWorldProcRead --
 *
 *     Read handler for /proc/vmware/vm/<vmid>/cpu/numasched.
 *     Prints out per-vsmp NUMASched stats.
 *     This proc node should only exist for the group leader.
 *
 * Results:
 *     Returns VMK_OK on success, VMK_BAD_PARAM on failure
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static int
NUMASchedWorldProcRead(Proc_Entry *entry, char *buffer, int *len)
{
   World_Handle *world;
   NUMASched_Stats *stats;
   NUMASched_VsmpInfo *info;
   uint32 totalSamples = 0;
   NUMA_Node n;
   int i;
   CpuSched_Vsmp *vsmp;
   Timer_Cycles totalRun = 0, totalReady = 0, totalWait = 0;
   Timer_Cycles totalDiff, readyDiff, runDiff, waitDiff, totalTime;
   CpuMask totalAffinity = 0;
   uint32 numVcpus;

   world = World_Find((World_ID)entry->private);
   if (world == NULL) {
      return (VMK_BAD_PARAM);
   }
   info = WorldNUMAInfo(world);

   *len = 0;
  
   if (info->homeNode == INVALID_NUMANODE) {
      Proc_Printf(buffer, len, 
                  "curHomeNode:  n/a\n");
   } else {
      Proc_Printf(buffer, len, 
                  "curHomeNode:  %2u\n",
                  info->homeNode);
   }

   stats = &info->stats;
   NUMASchedPrintStats(buffer, len, stats);
   
   // print per-node run times
   Proc_Printf(buffer, len, "\n");
   NUMA_FORALL_NODES(n) {
      totalSamples += stats->nodeRunCounts[n];
   }
   NUMA_FORALL_NODES(n) {
      uint32 pctHere = (totalSamples > 0) ? 
         (stats->nodeRunCounts[n] * 100) / totalSamples : 0;
      Proc_Printf(buffer, len, "noderun[%2u]  %3u%%   %8u sec\n",
                  n, pctHere, 
                  stats->nodeRunCounts[n] * 
                  CONFIG_OPTION(NUMA_REBALANCE_PERIOD) / 1000);
   }

   // print pages on each node
   // this information can be obtained from mem/NUMA, but let's see it all
   // in one place
   Proc_Printf(buffer, len, "\n");
   if (World_IsVMMWorld(world)) {
      NUMA_FORALL_NODES(n) {
         Proc_Printf(buffer, len, "node[%2u]  pages:  %7u    pctMem:  %3u%%\n",
                     n,
                     NUMASchedPagesOnNode(world, n),
                     NUMASchedPercentPagesOnNode(world, n)
            );
      }
   }

   // print current per-node history
   Proc_Printf(buffer, len, "\n");
   NUMA_FORALL_NODES(n) {
      Proc_Printf(buffer, len, 
                  "nodehistory[%2u]   %2u\n",
                  n, info->longTermHistory[n]);
   }


   // print run, ready, wait times in most recent interval
   // XXX: is this safe without the cpusched lock help?? probably not
   vsmp = World_CpuSchedVsmp(world);
   for (i=0; i < vsmp->vcpus.len; i++) {
      CpuSched_Vcpu *vcpu = vsmp->vcpus.list[i];
      if (vcpu) {
         NUMASchedVcpuNUMASnap(vcpu, &totalAffinity, &totalRun, &totalReady, &totalWait);
      }
   }

   runDiff = totalRun - info->prevRun;
   readyDiff = totalReady - info->prevReady;
   waitDiff = totalWait - info->prevWait;
   totalDiff = Timer_GetCycles() - numaSched.lastRebalanceTime;
   numVcpus = vsmp->vcpus.len;
   totalTime = numVcpus * totalDiff;

   Proc_Printf(buffer, len, 
               "\nprevRun:   %6Ld ms [%3d%%]\n",
               Timer_TCToMS(runDiff),
               totalTime > 0 ?
               (int32)((100 * runDiff) / totalTime) : 0);
   Proc_Printf(buffer, len, 
               "prevReady: %6Ld ms [%3d%%]\n",
               Timer_TCToMS(readyDiff),
               totalTime > 0 ? 
               (int32)((100 * readyDiff) / totalTime) : 0);
   Proc_Printf(buffer, len, 
               "prevWait:  %6Ld ms [%3d%%]\n",
               Timer_TCToMS(waitDiff),
               totalTime > 0 ?
               (int32) ((100 * waitDiff) / totalTime) : 0);
   Proc_Printf(buffer, len, 
               "\nlastRebalance: %Lu msec ago\n",
               Timer_TCToMS(totalDiff));
   if (info->lastMigrateTime > 0) {
      Proc_Printf(buffer, len, 
                  "lastMigrate:   %Lu msec ago\n",
                  Timer_TCToMS(Timer_GetCycles() - info->lastMigrateTime));
   } else {
      Proc_Printf(buffer, len,
                  "lastMigrate:   n/a\n");
   }
   
   World_Release(world);

   return(VMK_OK);
}



/*
 *-----------------------------------------------------------------------------
 *
 * NUMASched_AddWorldProcEntries --
 *
 *     Installs /proc/vmware/vm/<vmid>/cpu/numasched proc entry
 *
 * Results:
 *     None.
 *
 * Side effects:
 *     Adds proc node
 *
 *-----------------------------------------------------------------------------
 */
void 
NUMASched_AddWorldProcEntries(World_Handle *world, 
                              Proc_Entry *procDir)
{
   Proc_Entry *entry = &WorldNUMAInfo(world)->procWorldNUMA;

   if (NUMA_GetNumNodes() > 1 && World_IsVmmLeader(world)) {
      Proc_InitEntry(entry);
      entry->parent = procDir;
      entry->read = NUMASchedWorldProcRead;
      entry->private = (void*) world->worldID;
      Proc_Register(entry, "numasched", FALSE);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * NUMASched_RemoveWorldProcEntries --
 *
 *     Unregisters the per-world NUMASched proc entry
 *
 * Results:
 *     None
 *
 * Side effects:
 *     Removes proc node
 *
 *-----------------------------------------------------------------------------
 */
void
NUMASched_RemoveWorldProcEntries(World_Handle *world)
{
   CpuSched_Vsmp *vsmp = World_CpuSchedVsmp(world);

   // it's possible that the world hasn't been fully initialized,
   // so it doesn't have its vsmp set up yet
   if (vsmp != NULL && World_IsVmmLeader(world)) {
      Proc_Remove(&vsmp->numa.procWorldNUMA);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * NUMASchedParseThresholds --
 *
 *     Converts a string with a threshold specification into new page
 *     migration thresholds and then implements them.
 *     The string should be a series of lines, with four integers in
 *     each line, in the order: 
 *        "<freePage%>  <pctLocalMem>  <nodeHist>  <newMigRate>
 *
 *     There must be at least one full line, or the conversion will fail.
 * 
 * Results:
 *     Returns VMK_OK if the entire string was successfully converted into
 *     thresholds and implemented, VMK_BAD_PARAM otherwise.
 *
 * Side effects:
 *     Changes global page migration thresholds
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
NUMASchedParseThresholds(char *threshDesc)
{
   char *argv[4 * MAX_NUM_THRESHOLDS];
   int argc, i, numNewThresh = 0;

   argc = Parse_Args(threshDesc, argv, 4 * MAX_NUM_THRESHOLDS);
   numNewThresh = argc / 4;

   if (numNewThresh < 1) {
      // need at least one full line
      Warning("need at least one threshold specification");
      return VMK_BAD_PARAM;
   }

   SP_Lock(&numaSched.lock);

   numThresholds = 0;
   for (i=0; i < numNewThresh; i++) {
      uint32 freePages, pctLocal, nodeHist, newRate;
      uint32 idx = i * 4;

      if (Parse_Int(argv[idx], strlen(argv[idx]), &freePages) != VMK_OK) {
         SP_Unlock(&numaSched.lock);
         return VMK_BAD_PARAM;
      }
      if (Parse_Int(argv[idx+1], strlen(argv[idx+1]), &pctLocal) != VMK_OK) {
         SP_Unlock(&numaSched.lock);
         return VMK_BAD_PARAM;
      }
      if (Parse_Int(argv[idx+2], strlen(argv[idx+2]), &nodeHist) != VMK_OK) {
         SP_Unlock(&numaSched.lock);
         return VMK_BAD_PARAM;
      }
      if (Parse_Int(argv[idx+3], strlen(argv[idx+3]), &newRate) != VMK_OK) {
         SP_Unlock(&numaSched.lock);
         return VMK_BAD_PARAM;
      }
      
      migRateThresholds[i].freePageThresh = freePages;
      migRateThresholds[i].pctLocalThresh = pctLocal;
      migRateThresholds[i].nodeHistoryThresh = nodeHist;
      migRateThresholds[i].newMigRate = newRate;
      numThresholds++;
   }

   
   SP_Unlock(&numaSched.lock);   

   return VMK_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 * NUMASchedThresholdsProcWrite --
 *
 *     Proc write handler to modify page migration thresholds
 *
 * Results:
 *     Returns VMK_OK on success, VMK_BAD_PARAM oterhwise
 *
 * Side effects:
 *     Updates page migration thresholds
 *
 *-----------------------------------------------------------------------------
 */
static int
NUMASchedThresholdsProcWrite(UNUSED_PARAM(Proc_Entry *entry), char *buffer, UNUSED_PARAM(int *len))
{ 
   VMK_ReturnStatus res = NUMASchedParseThresholds(buffer);

   if (res != VMK_OK) {
      Log("failed to configure thresholds");
      return (res);
   }

   Log("set new page migration rate thresholds");
   
   return(VMK_OK);
}

/*
 *-----------------------------------------------------------------------------
 *
 * NUMASchedThresholdsProcRead --
 *
 *     Proc read handler to display current page migration thresholds
 *
 * Results:
 *     Returns VMK_OK 
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static int
NUMASchedThresholdsProcRead(UNUSED_PARAM(Proc_Entry *entry), char *buffer, int *len)
{
   uint32 i;
   *len = 0;
  
   SP_Lock(&numaSched.lock);
   
   Proc_Printf(buffer, len,   "%%free  %%local hist rate\n");
   for (i=0; i < numThresholds; i++) {
     Proc_Printf(buffer, len, "  %3u     %3u  %3u  %3u\n",
                 migRateThresholds[i].freePageThresh,
                 migRateThresholds[i].pctLocalThresh,
                 migRateThresholds[i].nodeHistoryThresh,
                 migRateThresholds[i].newMigRate);
   }
  
  SP_Unlock(&numaSched.lock);

  return(VMK_OK);

}

