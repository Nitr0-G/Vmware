/* **********************************************************
 * Copyright 2000 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * numa.h --
 *	This is the header file for the NUMA module.
 */


#ifndef _NUMA_H
#define _NUMA_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "vm_basic_types.h"
#include "return_status.h"
#include "numa_ext.h"
#include "host_dist.h"


typedef enum {
   NUMA_SYSTEM_GENERIC_UMA     = 0,
   NUMA_SYSTEM_FAKE_NUMA,
   NUMA_SYSTEM_GENERIC_NUMA,
   NUMA_SYSTEM_IBMX440,               // IBM VIGIL

   NUMA_SYSTEM_MAX
} NUMA_Systype;


/* 
 * Public Functions
 */
extern VMK_ReturnStatus NUMA_Init(VMnix_Init *vmnixInit,
				  Bool ignoreNUMA,
				  uint8 fakeNUMAnodes);

extern VMK_ReturnStatus NUMA_LateInit(void);
extern void NUMA_LocalInit(PCPU pcpuNum);

extern int NUMA_GetNumNodes(void);

extern NUMA_Systype NUMA_GetSystemType(void);

extern NUMA_Node NUMA_MPN2NodeNum(MPN mpn);

extern Bool NUMA_MemRangeIntersection(NUMA_Node node,
				      const NUMA_MemRange *inRange,
				      NUMA_MemRange *outRange);


// iterates over all pcpus in the specified nodes
#define NUMA_FORALL_NODE_PCPUS(_node, _pcpu) \
        for (_pcpu = 0; _pcpu < numPCPUs; _pcpu++) \
          if (CPUSCHED_AFFINITY(_pcpu) & numaSched.nodeMasks[_node])

// iterates over all nodes (1 node in a UMA system)
#define NUMA_FORALL_NODES(_node) \
        for (_node = 0; _node < NUMA_GetNumNodes(); _node++)



/*
 * VMK Entry Points
 */
 
extern VMKERNEL_ENTRY
NUMA_GetSystemInfo(DECLARE_1_ARG(VMK_NUMA_GET_SYS_INFO,
                                 uint32 *, numNodes));

extern VMKERNEL_ENTRY
NUMA_GetNodeInfo(DECLARE_2_ARGS(VMK_NUMA_GET_NODE_INFO,
                                NUMA_Node, node,
                                NUMA_MemRangesList *, memRangesList));


extern uint8 NUMA_GetNumNodeCpus(NUMA_Node node);
/*
 *-----------------------------------------------------------------------------
 *
 * NUMA_PCPU2NodeNum --
 *
 *     Returns the NUMA node number associated with PCPU "p"
 *
 * Results:
 *     Returns NUMA node number, or 0 on non-NUMA system
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE NUMA_Node
NUMA_PCPU2NodeNum(PCPU p)
{
   extern NUMA_Node pcpuToNUMANodeMap[];

   if (UNLIKELY(numPCPUs == 0)) {
      // special hack for Kseg_EarlyInit, which is called before numPCPUs
      // is initialized. This assumption validated in NUMA_LateInit
      ASSERT(p == HOST_PCPU);
      return 0;
   } else {
      ASSERT(p < numPCPUs);
      return pcpuToNUMANodeMap[p];
   }
}

#endif // _NUMA_H
