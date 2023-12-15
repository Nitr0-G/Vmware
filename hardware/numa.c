/* **********************************************************
 * Copyright 2002 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * numa.c
 *    Utility functions to gather information, statistics, and
 *    support a NUMA-based MP system.   See numa.h for definition
 *    of the NUMA informational structures.
 *
 * VMkernel flags: -i  (ignoreNUMA), -z fakeNUMA
 *   -z <#Nodes>   
 *               The FakeNUMA option has no effect on machines with
 *               an ACPI SRAT table, i.e. NUMA machines.
 *
 * Proc nodes:
 *   /proc/vmware/NUMA/hardware
 *   /proc/vmware/NUMA/nodeN/acpi
 *       read  - NUMA data from ACPI SRAT table
 *   /proc/vmware/NUMA/nodeN/twister
 *       read  - contents of Twister performance counters & chip ID
 *       write - controls various performance counters
 *   /proc/vmware/NUMA/nodeN/cyclone
 *       read  - contents of Cyclone timer (MPMC0) counter
 *       write - <pcpu#> measure Cyclone & Twister read latency
 *
 * Optional/To-do's:
 *    API for getting NUMA node information.
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "vmkernel.h"
#include "kvmap.h"
#include "proc.h"
#include "parse.h"
#include "memmap.h"
#include "numa_int.h"
#include "smp_int.h"
#include "timer.h"
#include "summit.h"

#define LOGLEVEL_MODULE NUMA
#define LOGLEVEL_MODULE_LEN 4
#include "log.h"


/*
 * Constants
 */
#define NODE_MEMSIZE_MULTIPLE      (16 * 1024 * 1024L)
#define NODE_MEMSIZE_MASK          (NODE_MEMSIZE_MULTIPLE - 1)
#define NUMA_MAX_CPUS_PER_NODE     (8)

/*
 * Private data structures
 */
typedef struct {
   NUMA_Node     nodeId;

   int           numCpus;
   int           numMemRanges;
   NUMA_MemRange memRange[NUMA_MAX_MEM_RANGES];

   uint32        apicIDs[NUMA_MAX_CPUS_PER_NODE];

   Proc_Entry    procNodeDir;
   Proc_Entry    procAcpi;
} NUMA_NodeInfo;

typedef struct {
   NUMA_Systype  systemType;
   Bool          ignoreNUMA;
   int           numNodes;
   NUMA_NodeInfo node[NUMA_MAX_NODES];
} NUMA_Info;


/*
 * Globals 
 */

NUMA_Node          pcpuToNUMANodeMap[MAX_PCPUS];


/*
 * Locals
 */

static Proc_Entry       procNumaDir;
static Proc_Entry       procNumaHwDir;
static NUMA_Info        vmkNumaInfo;
static uint32           totalSRATPages = 0;

/*
 * Local functions 
 */
static NUMA_NodeInfo * NUMAMapId2NodeEntry(NUMA_Node nodeId, Bool createNew);

static int NUMAProcReadHardware(Proc_Entry  *entry,
				char        *buffer,
				int         *len);

static int NUMAProcReadAcpi(Proc_Entry  *entry,
			    char        *buffer,
			    int         *len);

static Bool NUMAParseRealSRAT(VMnix_Init *vmnixInit);

static uint32 NUMAGetTotalNodePages(NUMA_Node node);


/*
 *----------------------------------------------------------------------
 *
 * NUMA_MPN2NodeNum() --   
 *
 *      Returns the NUMA node # corresponding to the given machine page.
 *      Returns 0 if UMA machine or ignoreNUMA is on.
 *
 * Results:
 *      The node #, or -1 if the MPN is not within any node's memory ranges
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
NUMA_Node
NUMA_MPN2NodeNum(MPN mpn)
{
   int n, i;
   NUMA_Info *info = &vmkNumaInfo;

   if (info->ignoreNUMA || info->numNodes==0) {
      return 0;
   } else {
      for (n=0; n < info->numNodes; n++) {
	 for (i=0; i < info->node[n].numMemRanges; i++) {
	    NUMA_MemRange *range = &info->node[n].memRange[i];
	    if (mpn >= range->startMPN && mpn <= range->endMPN) {
	       return(n);
	    }
	 }
      }
      return(INVALID_NUMANODE);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * NUMA_GetNumNodes() 
 *
 *      Returns the # of NUMA nodes, or 1 if this isn't a NUMA system.
 *
 * Results:
 *      The # of nodes.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
int
NUMA_GetNumNodes(void)
{
   NUMA_Info *info = &vmkNumaInfo;

   if (info->ignoreNUMA || info->numNodes==0) {
      return 1;
   } else {
      return info->numNodes;
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * NUMA_GetNumNodeCpus --
 *
 *     Returns the number of cpus in "node"
 *
 * Results:
 *     Returns the number of cpus in "node"
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
uint8
NUMA_GetNumNodeCpus(NUMA_Node node)
{
   NUMA_NodeInfo *info = &vmkNumaInfo.node[node];

   return info->numCpus;
}

/*
 *----------------------------------------------------------------------
 *
 * NUMA_GetSystemType() 
 *
 *      Returns one of the NUMA_Systype values as determined during 
 *      NUMA_Init(), such as NUMA_SYSTEM_IBMX440, etc.
 *      Returns NUMA_SYSTEM_GENERIC_UMA for non-NUMA systems.
 *      Not affected by the ignoreNUMA flag / -i option.
 *
 * Results:
 *      System type identifier.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
NUMA_Systype
NUMA_GetSystemType(void)
{
   return vmkNumaInfo.systemType;
}


/*
 *----------------------------------------------------------------------
 *
 * NUMA_MemRangeIntersection()
 *
 *      Finds the intersection between a node's memory ranges and the
 *      given memory range inRange, and returns the result in outRange.
 *      To return the first intersection, call with 
 *        outRange.startMPN == INVALID_MPN
 *      To return the next intersections if there are multiple ones,
 *      call this function with the previous returned outRange, and the
 *      intersection search will start at the spot of the previous match.
 *
 * Results:
 *      TRUE if the intersection is found.  Returned in outRange.
 *      FALSE if there are no (more) intersections.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Bool
NUMA_MemRangeIntersection(NUMA_Node node,
			  const NUMA_MemRange *inRange,
			  NUMA_MemRange *outRange)
{
   int i;
   NUMA_NodeInfo *nInfo = &vmkNumaInfo.node[node];
   NUMA_MemRange range;
   Bool foundLastMatch = FALSE;

   ASSERT(node < vmkNumaInfo.numNodes);
   ASSERT(nInfo != NULL);

   // Looking for first intersection?
   if (outRange->startMPN == INVALID_MPN) {
      foundLastMatch = TRUE;
   }

   for (i=0; i < nInfo->numMemRanges; i++) {
      if ((inRange->startMPN <= nInfo->memRange[i].endMPN) &&
	  (inRange->endMPN   >= nInfo->memRange[i].startMPN)) {
	 // found an overlapping range
	 range.startMPN = MAX(inRange->startMPN, nInfo->memRange[i].startMPN);
	 range.endMPN   = MIN(inRange->endMPN,   nInfo->memRange[i].endMPN);

	 if (foundLastMatch) {
	    outRange->startMPN = range.startMPN;
	    outRange->endMPN   = range.endMPN;
	    return TRUE;
	 } else if (outRange->startMPN == range.startMPN) {
	    foundLastMatch = TRUE;
	 }
      }
   }
   outRange->startMPN = INVALID_MPN;
   return FALSE;
}


/*
 *----------------------------------------------------------------------
 *
 * NUMAGetMemRanges -- 
 *
 *     Return the memory ranges of a specified node.
 *
 * Results:
 *     VMK_BAD_PARAM if node number is invalid.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
NUMAGetMemRanges(NUMA_Node node,                // IN
		  NUMA_MemRangesList *ranges)    // OUT
{
   int i;
   NUMA_NodeInfo *nInfo = &vmkNumaInfo.node[node];
   ASSERT(node < vmkNumaInfo.numNodes);

   if (nInfo == NULL) {
      return(VMK_BAD_PARAM);
   } else {
      ranges->numMemRanges = nInfo->numMemRanges;
      for (i=0; i < nInfo->numMemRanges; i++) {
	 ranges->memRange[i].startMPN = nInfo->memRange[i].startMPN;
	 ranges->memRange[i].endMPN   = nInfo->memRange[i].endMPN;
      }
   }
   return(VMK_OK);
}


/*
 *-----------------------------------------------------------------------------
 *
 * NUMAInitFakeNodes --
 *
 *     fakeNUMAnodes  - for NUMA debugging on UMA systems
 *     vmkloader -z flag
 *     Simulates fakeNUMAnodes nodes by dividing machine memory evenly
 *     amongst fake nodes.   Will have no effect on NUMA machines, with
 *     intact SRAT tables.
 * 
 *     Should be defined for special testing purposes only, such as to fake
 *     a NUMA system, or for use with VMNIX's without ACPI parsing code.
 *
 *
 * Results:
 *     None
 *
 * Side effects:
 *     Modifies vmkNumaInfo
 *
 *-----------------------------------------------------------------------------
 */
static void 
NUMAInitFakeNodes(uint8 fakeNUMAnodes, VMnix_Init *vmnixInit) 
{
   MA top = 0;
   MPN last;
   NUMA_Info *info = &vmkNumaInfo;
   int n, nodelen;
   
   if (fakeNUMAnodes > NUMA_MAX_NODES) {
      fakeNUMAnodes = NUMA_MAX_NODES;
   }
   for (n = 0; n < MAX_VMNIX_MEM_RANGES; n++) {
      if (vmnixInit->vmkMem[n].startMPN == 0) {
         break;
      } else {
         LOG(1, "vmkmem start: %d", vmnixInit->vmkMem[n].startMPN);
      }
      top = MPN_2_MA(vmnixInit->vmkMem[n].endMPN);
   }
   top = (top + (MA)NODE_MEMSIZE_MULTIPLE) & ~((MA) NODE_MEMSIZE_MASK);
   nodelen = (MA_2_MPN(top) - vmnixInit->vmkMem[0].startMPN) / fakeNUMAnodes;
   last = 0;

   LOG(0, "Faking %d NUMA nodes", fakeNUMAnodes);
   for (n = 0; n < fakeNUMAnodes; n++) {
      info->node[n].nodeId = n;
      info->node[n].numMemRanges = 1;
      info->node[n].memRange[0].startMPN = last;

      if (n == fakeNUMAnodes - 1) {
         // make sure we allocate all memory in the system to valid
         // nodes (we might miss some due to rounding after dividing
         // total memory by "fakeNUMAnodes")
         info->node[n].memRange[0].endMPN = MA_2_MPN(top) - 1;
      } else if (n == 0) {
         // leave bonus space for COS
         info->node[n].memRange[0].endMPN   = vmnixInit->vmkMem[0].startMPN + (nodelen - 1);
      } else {
         info->node[n].memRange[0].endMPN   = last + nodelen - 1;
      }
      totalSRATPages += (info->node[n].memRange[0].endMPN -
			 info->node[n].memRange[0].startMPN) + 1;

      info->node[n].numCpus = 0;

      Log("Node %d  0x%x - 0x%x", n,
          info->node[n].memRange[0].startMPN,
          info->node[n].memRange[0].endMPN);
      last = info->node[n].memRange[0].endMPN + 1;
   }
   info->numNodes = fakeNUMAnodes;
}


/*
 *-----------------------------------------------------------------------------
 *
 * NUMAFindAPICId --
 *
 *     Finds the node in which the processor corresponding to APIC ID lies
 *
 * Results:
 *     Returns the node containing "id" or INVALID_NUMANODE fi not found
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
NUMA_Node
NUMAFindAPICId(uint32 id)
{
   NUMA_Node n;
   int i;

   for (n=0; n < vmkNumaInfo.numNodes; n++) {
      NUMA_NodeInfo *node = &vmkNumaInfo.node[n];
      for (i=0; i < node->numCpus; i++) {
         if (id == node->apicIDs[i]) {
            return n;
         }
      }
   }

   return INVALID_NUMANODE;
}

/*
 *-----------------------------------------------------------------------------
 *
 * NUMAParseRealSRAT --
 *
 *     Configures vmkNumaInfo by rading the SRAT in a NUMA system.
 *     Currently a "bad SRAT table" is one with duplicate entries, or
 *      incomplete sections (no processor or mem entries), or extra
 *      entries (ie processor).  IOW any errors.
 *
 * Results:
 *     TRUE if good SRAT found, FALSE if bad SRAT found
 *
 * Side effects:
 *     Modifies vmkNumaInfo.  numNodes set to # of nodes in SRAT table.
 *     If bad SRAT found, then numNodes will be set to 0.   
 *
 *-----------------------------------------------------------------------------
 */
static Bool
NUMAParseRealSRAT(VMnix_Init *vmnixInit) 
{
   NUMA_Info *info = &vmkNumaInfo;
   int n;
   uint8 *ptr, *end;
   acpi_dt_entry_header *entry_header;
   acpi_srat_proc *proc_entry;
   acpi_srat_mem *mem_entry;
   NUMA_MemRange myRange;
   NUMA_NodeInfo *node;
   int errors = 0;

   /* Find SRAT entries */
   ptr = ((acpi_srat *)vmnixInit->savedACPI.srat)->entries;
   end = &vmnixInit->savedACPI.srat
      [((acpi_srat *)vmnixInit->savedACPI.srat)->header.length];

   /* Go through each SRAT entry */
   for (; ptr < end; ptr += entry_header->length) {
      entry_header = (acpi_dt_entry_header *)ptr;
      switch (entry_header->type) {
      case SRAT_PROC:
         proc_entry = (acpi_srat_proc *)entry_header;
         Log("SRAT proc entry  nodeID=0x%02x apicID=0x%02x",
             proc_entry->nodeID, proc_entry->apicID);

         node = NUMAMapId2NodeEntry(proc_entry->nodeID, TRUE);
         if (node == (NUMA_NodeInfo *) NULL) {
            Warning("Node Table Full, no room for new node %d",
                    proc_entry->nodeID);
            break;
         }

         if (node->numCpus >= NUMA_MAX_CPUS_PER_NODE) {
            Warning("# of cpus in this node exceeds limit of %d", NUMA_MAX_CPUS_PER_NODE);
            break;
         }

	 /* Fail if this is a duplicate APIC ID */
         if (NUMAFindAPICId(proc_entry->apicID) != INVALID_NUMANODE) {
            Warning("Duplicate APIC ID found in SRAT, skipping entry...");
            errors++;
         } else {
            node->apicIDs[node->numCpus] = proc_entry->apicID;
            node->numCpus++;
         }
         break;

      case SRAT_MEM:
         mem_entry = (acpi_srat_mem *)entry_header;
         Log("SRAT mem entry   nodeID=0x%02x start=0x%09Lx size=0x%09Lx",
             mem_entry->nodeID, mem_entry->start, mem_entry->size);
         
         node = NUMAMapId2NodeEntry(mem_entry->nodeID, TRUE);
         if (node == (NUMA_NodeInfo *) NULL) {
            Warning("Node Table Full, no room for new node %d",
                    mem_entry->nodeID);
            break;
         }
         ASSERT_NOT_IMPLEMENTED(node->numMemRanges < NUMA_MAX_MEM_RANGES);
         node->memRange[node->numMemRanges].startMPN = MA_2_MPN(mem_entry->start);
         node->memRange[node->numMemRanges].endMPN = 
            MA_2_MPN(mem_entry->start + mem_entry->size) - 1;
	 totalSRATPages += MA_2_MPN(mem_entry->size);

         // Check that new mem range doesn't intersect with existing ones
         myRange.startMPN = INVALID_MPN;
	 for (n=0; n < info->numNodes; n++) {
	   if (NUMA_MemRangeIntersection(n,
					 &node->memRange[node->numMemRanges],
					 &myRange)) {
	     Warning("SRAT memory range conflicts with previous one [%x000-%x000]",
		     myRange.startMPN, myRange.endMPN);
	     errors++;
	     break;
	   }
         }
	 if (n == info->numNodes) {
	    node->numMemRanges++;
	 }
         break;

      default:
         Warning("Unknown SRAT entry (type %d)", entry_header->type);
	 errors++;
	 break;
      }    // switch on SRAT entry type

   }    // for each SRAT entry

   if (errors) {
     Warning("%d errors found in SRAT table", errors);
     return FALSE;
   }

   // make sure SRAT table nonempty
   if (info->numNodes) {
      Log("%d nodes found in SRAT table", info->numNodes);
      for (n=0; n < info->numNodes; n++) {
	if (info->node[n].numCpus == 0) {
	  Warning("No processors detected in SRAT node %d", n);
	  return FALSE;
	} else if (info->node[n].numMemRanges == 0) {
	  Warning("No memory detected in SRAT node %d", n);
          // NB:
          // we do not return FALSE here because we want the system
          // type to remain NUMA.  That way, MemMap_Init() will detect
          // that there's no memory in a Node, return an error, and
          // cause the vmkernel to stop loading.
	}
      }
      return TRUE;
   } else {
      Warning("Empty SRAT table found, header len = %d",
	      ((acpi_srat *)vmnixInit->savedACPI.srat)->header.length);
      return FALSE;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * NUMA_Init() --   NUMA module initialization
 *
 *      Initialize the NUMANode[] structures with info from the SRAT 
 *      table (on CPUs and memories).  The FakeNUMA (-z) and IgnoreNUMA
 *      (-i) vmkloader options are processed here.  Notes:
 *         -z FakeNuma  has no effect on machines with SRAT tables
 *         -i IgnoreNUMA:  Twister/Cyclone still accessible on x440's
 *  
 *      NOTE: the cpu info is not valid until after NUMA_LateInit() 
 *            has been called
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
NUMA_Init(VMnix_Init *vmnixInit,         // IN: the init block from vmnix
	  Bool ignoreNUMA,               // IN: ignore NUMA flag (-i)
	  uint8 fakeNUMAnodes)           // IN: # fake NUMA nodes
{
   int n;
   NUMA_Info *info;
   VMnix_SavedMPS *mps = &vmnixInit->savedMPS;
   MPConfigTable *mpc;

   mpc = (MPConfigTable*)&mps->mpc;
   ASSERT_NOT_IMPLEMENTED((void *) mpc != NULL);

   info = &vmkNumaInfo;
   info->numNodes = 0;
   for (n=0; n < NUMA_MAX_NODES; n++) {
      info->node[n].nodeId = INVALID_NUMANODE;
   }
   info->ignoreNUMA = ignoreNUMA;

   /* scan the ACPI SRAT entries for NUMA information */
   if (!ignoreNUMA && vmnixInit->savedACPI.srat[0]) {
      /* If SRAT table missing, don't declare the system an x440, otherwise 
       * the APIC ID's will be nonsense and the code for initializing IBM 
       * chipsets and Cyclone TSC might not start properly.
       */
      if (NUMAParseRealSRAT(vmnixInit)) {
	 // good SRAT table found, check NUMA system type
	 if (!strncmp(mpc->oem, "IBM ENSW", 8) &&
	     !strncmp(mpc->productid, "VIGIL SMP", 9)) {
	    info->systemType = NUMA_SYSTEM_IBMX440;
	    Log("IBM NUMA Summit-based system found.");
	    Summit_EarlyInit();
	 } else if (info->numNodes > 1) {
	    info->systemType = NUMA_SYSTEM_GENERIC_NUMA;
	    Log("Unknown OEM [%s], Product ID [%s]", mpc->oem, mpc->productid);
	    Log("Generic NUMA system found, no NUMA performance info available.");
	 } else {
	    info->systemType = NUMA_SYSTEM_GENERIC_UMA;
            Log("Only 1 node found in SRAT table, treating as UMA system");
         }
      } else {
         // Treat any SRAT with >= 2 nodes as NUMA system
         if (info->numNodes > 1) {
            // broken SRAT table, probably due to BIOS bug
            Warning("Errors parsing SRAT table, treating as UMA system");
         } else {
            // one node, ignore SRAT and treat as UMA system.
            // 1-Node IBM x440's with bad SRAT's also come here
            Log("Ignoring 1 node found in SRAT table, treating as UMA system");
         }
	 info->numNodes = 0;
	 info->systemType = NUMA_SYSTEM_GENERIC_UMA;
      }

   } else if (fakeNUMAnodes > 1) {
      NUMAInitFakeNodes(fakeNUMAnodes, vmnixInit);
      info->systemType = NUMA_SYSTEM_FAKE_NUMA;
      Log("Fake NUMA system found, obviously no NUMA performance info");

   } else {
      // No SRAT table & no -z option. Generic UMA system
      info->systemType = NUMA_SYSTEM_GENERIC_UMA;
      Log("Generic UMA system found");
   }

   /* Check node structure consistency */
   if (info->numNodes) {
      for (n=0; n < info->numNodes; n++) {
	 ASSERT(info->node[n].numCpus > 0 || info->systemType == NUMA_SYSTEM_FAKE_NUMA);
      }
   }

   return(VMK_OK);
}


/*
 *----------------------------------------------------------------------
 *
 * NUMA_LateInit() --   proc node initialization
 *
 *      Create /proc/vmware/NUMA/... nodes.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
NUMA_LateInit(void)
{
   NUMA_Info *info = &vmkNumaInfo;
   NUMA_Node minNode = INVALID_NUMANODE, maxNode = INVALID_NUMANODE;
   char procName[8];
   int n, i;

   /* Set up proc nodes if there are NUMA nodes */
   if (info->numNodes) {

      /* Register the /proc/vmware/NUMA dir */
      Proc_InitEntry(&procNumaDir);
      procNumaDir.parent = NULL;
      Proc_Register(&procNumaDir, "NUMA", TRUE);

      /* Register /proc/vmware/NUMA/hardware */
      Proc_InitEntry(&procNumaHwDir);
      procNumaHwDir.parent = &procNumaDir;
      procNumaHwDir.read = NUMAProcReadHardware;
      Proc_Register(&procNumaHwDir, "hardware", FALSE);

      /* For each node, register nodeN as a dir */
      for (n=0; n < info->numNodes; n++) {
	 NUMA_NodeInfo *node = &info->node[n];

         Proc_InitEntry(&node->procNodeDir);
	 (void) snprintf(procName, 7, "node%d", node->nodeId);
	 node->procNodeDir.parent = &procNumaDir;
	 Proc_RegisterHidden(&node->procNodeDir, procName, TRUE);

	 /* Add acpi entry */
	 Proc_InitEntry(&node->procAcpi);
	 node->procAcpi.parent = &node->procNodeDir;
	 node->procAcpi.read = NUMAProcReadAcpi;
	 node->procAcpi.private = node;
	 Proc_RegisterHidden(&node->procAcpi, "acpi", FALSE);

      }   // for each node
   }

   // setup a mapping from PCPU numbers to node numbers
   memset(&pcpuToNUMANodeMap, 0, sizeof(pcpuToNUMANodeMap));

   if (info->systemType == NUMA_SYSTEM_FAKE_NUMA) {
      int pcpusMapped = 0;
      // parcel out the cpus one-at-a-time to the numa nodes
      for (i = 0; i < numPCPUs; i++) {
         NUMA_Node nodeNum = i % info->numNodes;
         info->node[nodeNum].numCpus++;
      }
      // we want to make sure that the pcpu->node mappings seem logical,
      // so that, for instance, pcpus 0 and 1 are in node 0 on a 4 way/2 node
      // system, so we assign apicIDs and pcpu->node mappings sequentially
      for (n = 0; n < info->numNodes; n++) {
         for (i=0; i < info->node[n].numCpus; i++) {
            PCPU thisPcpu = pcpusMapped;
            pcpuToNUMANodeMap[thisPcpu] = n;
            info->node[n].apicIDs[i] = SMP_GetApicID(thisPcpu);
            pcpusMapped++;
         }
      }
   } else {
      for (n=0; !info->ignoreNUMA && n < info->numNodes; n++) {
         NUMA_NodeInfo *node = &info->node[n];
         
         for (i=0; i < node->numCpus; i++) {
            uint32 apicID = node->apicIDs[i];
            PCPU p = SMP_GetPCPUNum(apicID);

            pcpuToNUMANodeMap[p] = n;
            ASSERT(p != 0 || n == 0);
         }
      }
   }

   // search for imbalance between NUMA node configurations
   NUMA_FORALL_NODES(n) {
      uint32 thisNodePages = NUMAGetTotalNodePages(n);
      if (maxNode == INVALID_NUMANODE || thisNodePages > NUMAGetTotalNodePages(maxNode)) {
         maxNode = n;
      } 
      if (minNode == INVALID_NUMANODE || thisNodePages < NUMAGetTotalNodePages(minNode)) {
         minNode = n;
      }
   }
   // warn the user if there is an imbalance of more than 30% between two nodes
   if (NUMAGetTotalNodePages(maxNode) > ((13 * NUMAGetTotalNodePages(minNode)) / 10)) {
      Warning("Memory is incorrectly balanced between the NUMA nodes of this system, which will lead to poor performance. "
              "See /proc/vmware/NUMA/hardware for details on your current memory configuration");
   }

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NUMA_LocalInit --
 *
 *      NUMA module per-PCPU initialization.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
NUMA_LocalInit(PCPU pcpu)
{
   NUMA_Info *info = &vmkNumaInfo;
   NUMA_NodeInfo *node = &info->node[NUMA_PCPU2NodeNum(pcpu)];
   Bool ok;

   if (info->systemType == NUMA_SYSTEM_IBMX440) {
      ok = Summit_LocalInit(pcpu, &node->procNodeDir);
      if (!ok) {
	 // Specific hardware initialization failed, fall back
	 info->systemType = NUMA_SYSTEM_GENERIC_NUMA;
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * NUMAMapId2NodeEntry -- 
 *      Given a NUMA Node ID, returns a pointer to the corresponding 
 *      NUMA_NodeInfo struct. 
 *      If no existing NUMA_NodeInfo entry exists for that Node Id,
 *      and createNew is TRUE, then a new entry is created for it.
 *      If there is no room for a new entry, then NULL is returned.
 *
 * Results:
 *      Pointer to NUMA_NodeInfo struct for given Node ID.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static NUMA_NodeInfo *
NUMAMapId2NodeEntry(NUMA_Node nodeId, Bool createNew)
{
   NUMA_Info *info = &vmkNumaInfo;
   int n;

   for (n=0; n < info->numNodes; n++) {
      if (info->node[n].nodeId == nodeId) {
	 return(&info->node[n]);
      }
   }

   // entry not found, return if not creating new entry
   if (!createNew) {
      return((NUMA_NodeInfo *) NULL);
   }

   // entry with nodeId not found, attempt to create one
   if (info->numNodes >= NUMA_MAX_NODES) {
      return((NUMA_NodeInfo *) NULL);
   } else {
      n = info->numNodes;
      info->node[n].nodeId = nodeId;
      info->numNodes++;
      return(&info->node[n]);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * NUMAGetTotalNodePages() --
 *
 * 	Returns the total number of MPNs in the specified node, as defined 
 * 	by the ACPI SRAT table.  This may include areas outside of vmkernel
 * 	management such as Console OS memory.  If IgnoreNUMA mode is on,
 * 	returns the total size of installed RAM as reported by SRAT table.
 *
 * Results:
 *      Returns the total number of installed pages on one node.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static uint32
NUMAGetTotalNodePages(NUMA_Node node)
{
   uint32 size = 0;
   int i;
   NUMA_NodeInfo *info = &vmkNumaInfo.node[node];

   if (vmkNumaInfo.ignoreNUMA) {
      return totalSRATPages;
   } else {
      for (i=0; i < info->numMemRanges; i++) {
	 size += (info->memRange[i].endMPN - info->memRange[i].startMPN + 1);
      }
   }

   return size;
}

/*
 *----------------------------------------------------------------------
 *
 * NUMAProcReadHardware() --
 *
 *      Called upon a file read from /proc/vmware/NUMA/hardware
 *      Writes out summary on NUMA hardware, PCPU #'s, etc.
 *      If the fakeNUMA load option is used (-z), the fake NUMA info
 *       will be displayed.
 *      If the IgnoreNUMA option is used (-i), this displays one node
 *       even if the system in fact has more than one node.
 *      To check the real state of the machine, please read the
 *       (hidden) /proc/vmware/NUMA/nodeN/acpi files.
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
NUMAProcReadHardware(Proc_Entry  *entry,
		     char        *buffer,
		     int         *len)
{
   NUMA_Info *info = &vmkNumaInfo;
   *len = 0;
   NUMA_Node i;
   int j;

   // Dump out system type
   Proc_Printf(buffer, len, "System type    : ");
   switch(info->systemType) {
   case NUMA_SYSTEM_IBMX440:
      Proc_Printf(buffer, len, "IBM Summit NUMA System\n");
      break;
   case NUMA_SYSTEM_GENERIC_NUMA:
      Proc_Printf(buffer, len, "Generic NUMA System\n");
      break;
   case NUMA_SYSTEM_FAKE_NUMA:
      Proc_Printf(buffer, len, "Fake NUMA System\n");
      break;
   case NUMA_SYSTEM_GENERIC_UMA:
      Proc_Printf(buffer, len, "Not a NUMA System!\n");
      break;
   default:
      NOT_REACHED();
   }

   // Dump out # of managed nodes, total system RAM
   Proc_Printf(buffer, len, "# NUMA Nodes   : %d\n", NUMA_GetNumNodes());
   Proc_Printf(buffer, len, "Total memory   : %u MB\n", PagesToMB(totalSRATPages));

   // Dump out PCPU information for each node
   Proc_Printf(buffer, len, "Node ID  MachineMem  ManagedMem   CPUs\n");
   for (i = 0;  i < NUMA_GetNumNodes(); i++) {
      Proc_Printf(buffer, len, " %3d %02x  %7u MB  %7u MB   ", i,
		  info->node[i].nodeId,
		  PagesToMB(NUMAGetTotalNodePages(i)),
		  PagesToMB(MemMap_NodeTotalPages(i)) );
      // now dump out each PCPU
      for (j = 0; j < numPCPUs; j++) {
	 if (pcpuToNUMANodeMap[j] == i) {
	    Proc_Printf(buffer, len, "%d ", j);
	 }
      }
      Proc_Printf(buffer, len, "\n");
   }

   return(VMK_OK);
}


/*
 *----------------------------------------------------------------------
 *
 * NUMAProcReadAcpi() --
 *
 *      Called when user dumps out /proc/vmware/NUMA/nodeN/acpi
 *      Writes out info on NUMA node from ACPI-SRAT tables
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
NUMAProcReadAcpi(Proc_Entry  *entry,
		 char        *buffer,
		 int         *len)
{
   NUMA_NodeInfo *node = (NUMA_NodeInfo *) entry->private;
   int i;
   ASSERT(node != NULL);

   *len = 0;

   /* Dump out node CPU and memory info */
   Proc_Printf(buffer, len, "nodeId      %d\n", node->nodeId);
   Proc_Printf(buffer, len, "numCpus     %d\n", node->numCpus);
   
   Proc_Printf(buffer, len, "APIC IDs: ");
   for (i=0; i < node->numCpus; i++) {
      Proc_Printf(buffer, len, "%02x(%d) ",
                  node->apicIDs[i],
                  SMP_GetPCPUNum(node->apicIDs[i]));
   }
   Proc_Printf(buffer, len, "\n");

   Proc_Printf(buffer, len, "numMemRanges %d\n", node->numMemRanges);
   for (i=0; i < node->numMemRanges; i++) {
      Proc_Printf(buffer, len, "memRange[%d] startMPN=0x%08X  endMPN=0x%08X\n",
		  i, node->memRange[i].startMPN, node->memRange[i].endMPN);
   }

   return (VMK_OK);
}
   

/*
 *----------------------------------------------------------------------
 *
 * NUMA_GetSystemInfo --
 *
 *      Obtain system-level NUMA info.
 *
 * Results:
 *      Sets "numNodes" to number of NUMA nodes (1 if UMA system).
 *	Returns VMK_OK.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMKERNEL_ENTRY
NUMA_GetSystemInfo(DECLARE_1_ARG(VMK_NUMA_GET_SYS_INFO,
                                 uint32 *, numNodes))
{
   PROCESS_1_ARG(VMK_NUMA_GET_SYS_INFO,
                 uint32 *, numNodes);

   // obtain NUMA node count
   *numNodes = NUMA_GetNumNodes();
   return(VMK_OK);
}

                   
/*
 *----------------------------------------------------------------------
 *
 * NUMA_GetNodeInfo --
 *
 *      Obtain node-level NUMA info for specified "node".
 *
 * Results:
 *      Modifies "memRangesList" to contain MPN ranges associated
 *	with specified NUMA "node".  Returns VMK_OK if successful,
 *	otherwise error code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMKERNEL_ENTRY
NUMA_GetNodeInfo(DECLARE_2_ARGS(VMK_NUMA_GET_NODE_INFO,
                                NUMA_Node, node,
                                NUMA_MemRangesList *, memRangesList))
{
   VMK_ReturnStatus status;
   PROCESS_2_ARGS(VMK_NUMA_GET_NODE_INFO,
                  uint32, node,
                  NUMA_MemRangesList *, memRangesList);

   // obtain NUMA memory ranges
   status = NUMAGetMemRanges(node, memRangesList);
   return(status);
}
