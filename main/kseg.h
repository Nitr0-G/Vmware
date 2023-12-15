/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * kseg.h --
 *
 *	This is the header file for the kseg vmkernel virtual
 *      address space manager.
 */

#ifndef _KSEG_H
#define _KSEG_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "vmkernel.h"
#include "world.h"
#include "kseg_dist.h"

struct KSEG_Pair {
   uint32 	pageNum;  // Machine or physical page currently mapped by vaddr
			  // 0xffffffff means not currently mapped.
   uint16    	count;    // Number of outstanding references by Kseg_GetPtr
   uint16	lastWay;
   World_ID	worldID;  // world whose physical page is mapped (or INVALID_WORLD_ID)
   MA	  	maxAddr;  // Max machine/physical address that is mapped
   VA     	vaddr;    // Virtual address for this kseg entry

   // for debugging
   void         *ra;

   // Make this structure 32 byte aligned so that 
   //   it is cache sized aligned.
   char 	pad[32 - 2*sizeof(uint32) - 2*sizeof(uint16) -
                sizeof(MA) - sizeof(VA) - sizeof(void*)];
};

struct VMnix_Init;

extern void Kseg_Init(void);
extern void Kseg_Flush(void);
extern void *Kseg_GetPtrIRQFromMA(MA maddr, uint32 length, KSEG_Pair **pair);
extern void Kseg_ReleasePtr(KSEG_Pair *pair);
extern void Kseg_FlushRemote(World_ID worldID, PPN ppn);
extern Bool Kseg_CheckRemote(World_ID worldID, PPN ppn);
extern VMK_ReturnStatus Kseg_Dump(void);

extern void *Kseg_GetPtrFromPA(World_Handle *world, PA paddr, 
                                uint32 length, Bool canBlock,
                                KSEG_Pair **pair, VMK_ReturnStatus *retStatus);

extern void Kseg_InvalidatePtr(World_Handle *world, PPN ppn);
extern void *Kseg_DebugMap(MPN mpn);
extern void Kseg_DebugMapRestore(void);
extern VMK_ReturnStatus Kseg_MapRegion(PCPU pcpu, MA pageRoot);

// convenient wrapper for mapping single machine page
static inline void *Kseg_MapMPN(MPN mpn, KSEG_Pair **pair)
{
   return(Kseg_GetPtrFromMA(MPN_2_MA(mpn), PAGE_SIZE, pair));
}

#endif
