/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * memmap.h --
 *
 *	This is the header file for machine memory manager.
 */


#ifndef _MEMMAP_H
#define _MEMMAP_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"
#include "memmap_dist.h"
#include "world.h"
#include "numa.h"

/*
 * Constants
 */

/*
 * types
 */

/*
 * A bit mask to specify which NUMA nodes the policy function
 * will choose from.  bit 0 = NUMA node #0, etc.  This parameter
 * will be superceded by VM memory/node affinity.
 */
typedef uint32 MM_NodeMask;

/* Any node is OK - allow internal policy fn to pick */
#define MM_NODE_ANY                               (0xffffffff)

typedef uint32 MM_Color;

/* Any color is OK - allow internal policy fn to pick */
#define MM_COLOR_ANY                              ((MM_Color) -1)

/*
 * MM_AllocType is used to choose between high or low pages.
 * If MM_TYPE_ANY is specified, the policy function will pick
 * one of the types of pages.
 */
typedef enum MMAllocType {
   MM_TYPE_ANY,
   MM_TYPE_HIGH,
   MM_TYPE_LOW,
   MM_TYPE_LOWRESERVED
} MM_AllocType;


typedef void (*MemMap_Callback)(uint32 nFreePages);

#define MAX_AVAIL_MEM_RANGES 32


/*
 * operations
 */

extern VMK_ReturnStatus MemMap_HotAdd(MA startAddress,
				    uint64 size,
	 		            Bool memCheckEveryWord,
				    unsigned char attrib, 
				    VMnix_Init *vmnixInit);
extern void MemMap_LateInit(void);

extern MPN MemMap_AllocVMPage(World_Handle *world,
			      PPN ppn,
			      MM_NodeMask nodeMask,
			      MM_Color color,
			      MM_AllocType type);
extern MPN MemMap_AllocVMPageWait(World_Handle *world,
				  PPN ppn,
				  MM_NodeMask nodeMask,
				  MM_Color color,
				  MM_AllocType type,
				  uint32 msTimeout);
extern MPN MemMap_AllocUserWorldPage(World_Handle* world,
                                     MM_NodeMask nodeMask,
                                     MM_Color color,
                                     MM_AllocType type);
extern MPN MemMap_AllocUserWorldPageWait(World_Handle* world,
                                         MM_NodeMask nodeMask,
                                         MM_Color color,
                                         MM_AllocType type,
                                         uint32 msTimeout);
extern MPN MemMap_AllocKernelPage(MM_NodeMask nodeMask,
				  MM_Color color,
				  MM_AllocType type);
extern MPN MemMap_AllocKernelPageWait(MM_NodeMask nodeMask,
				      MM_Color color,
				      MM_AllocType type,
				      uint32 msTimeout);
extern MPN MemMap_AllocKernelLargePage(MM_NodeMask nodeMask, 
                                       MM_Color color, 
                                       MM_AllocType type);
extern MPN MemMap_AllocKernelPages(uint32 numMPNs,
                                   MM_NodeMask nodeMask,
				   MM_Color color,
				   MM_AllocType type);
extern MPN MemMap_NiceAllocKernelPages(uint32 numPages, 
                                       MM_NodeMask nodeMask, 
                                       MM_Color color, 
                                       MM_AllocType type);
extern MPN MemMap_NiceAllocKernelLargePage(MM_NodeMask nodeMask, 
                                           MM_Color color, 
                                           MM_AllocType type);
extern MPN MemMap_NiceAllocKernelPage(MM_NodeMask nodeMask, MM_Color color, 
                                      MM_AllocType type);

extern void MemMap_FreeVMPage(World_Handle *world, MPN mpn);
extern void MemMap_FreeUserWorldPage(MPN mpn);
extern void MemMap_FreeKernelPages(MPN mpn);

extern MM_Color MemMap_DefaultColor(World_Handle *world, PPN ppn);
extern MM_Color MemMap_GetNumColors(void);

extern uint32 MemMap_ManagedPages(void);
extern uint32 MemMap_KernelPages(void);
extern uint32 MemMap_UnusedPages(void);
extern MPN MemMap_GetLastValidMPN(void);
extern uint32 MemMap_GetNumNodes(void);
extern void MemMap_LogState(int level);

extern void MemMap_SetTrigger(uint32 lowPages, uint32 highPages);

extern VMK_ReturnStatus MemMap_Init(void);
extern VMK_ReturnStatus MemMap_EarlyInit(VMnix_Init *vmnixInit, 
                                         Bool memCheckEveryWord);
extern MPN MemMap_EarlyAllocPage(MM_AllocType type);

EXTERN uint32 MemMap_NodeFreePages(NUMA_Node n);
EXTERN uint32 MemMap_NodeTotalPages(NUMA_Node n);
EXTERN uint32 MemMap_NodePctMemFree(NUMA_Node n);

EXTERN VMK_ReturnStatus MemMap_AllocPageRange(World_Handle *world, 
                                              MPN *startMPN, uint32 *numPages);
EXTERN uint32 MemMap_FreePageRange(MPN startMPN, uint32 numPages);

// simple wrappers
extern INLINE MPN
MemMap_AllocAnyKernelPage(void)
{
   return MemMap_AllocKernelPage(MM_NODE_ANY, MM_COLOR_ANY, MM_TYPE_ANY);
}
extern INLINE void MemMap_FreeKernelPage(MPN mpn)
{
   MemMap_FreeKernelPages(mpn);
}

/*
 *-----------------------------------------------------------------------------
 *
 * MemMap_PCPU2NodeMask --
 *
 *     Returns the a nodemask that includes only the given pcpu
 *
 * Results:
 *     A nodemask that includes only the given pcpu
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE MM_NodeMask
MemMap_PCPU2NodeMask(PCPU p)
{
   return 1 << NUMA_PCPU2NodeNum(p);
}

extern MM_Color MemMap_MPN2Color(const MPN mpn);
struct VMnix_MemMapInfoArgs;
struct VMnix_MemMapInfoResults;
extern VMK_ReturnStatus MemMap_GetInfo(struct VMnix_MemMapInfoArgs *args, 
                                       struct VMnix_MemMapInfoResult *result,
                                       unsigned long resultLen);
extern VMK_ReturnStatus MemMap_GetCacheSize(uint32 *assoc, uint32 *size);

#endif
