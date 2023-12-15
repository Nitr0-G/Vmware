/* **********************************************************
 * Copyright 2000 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * alloc_ext.h --
 *
 *	This is the header file for machine memory manager.
 */


#ifndef _ALLOC_EXT_H
#define _ALLOC_EXT_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMCORE
#include "includeCheck.h"

#include "numa_ext.h"


// COS VMX only alloc info
typedef struct {
   // user virtual address in the VMX where the 
   // overhead memory is mapped
   VPN vmxOvhdMemVPN;

   // VMX overhead memory
   uint32 numOverheadPages;

   // two-level index from VPN -> MPN
   // pages is array of MPNs containing the second level of index
   uint32       numOvhdPDirEntries;
   MPN          *ovhdPages;
} Alloc_CosVmxInfo;

/*
 * Mapping from world virtual addr space to machine pages
 */
typedef struct {
   // Is this info initialized?
   Bool valid;

   // guest physical memory
   uint32 numPhysPages;

   // reserved anon memory
   uint32 numAnonPages;

   // two-level index from PPN -> MPN
   // pages is array of MPNs containing the second level of index
   uint32       numPDirEntries;
   MPN	 	*pages;

   Alloc_CosVmxInfo cosVmxInfo;
} Alloc_PageInfo;

/*
 * Page remapping definitions.
 */

// remapping specification
typedef struct {
   unsigned valid      : 1;
   unsigned remapLow   : 1;
   unsigned remapHigh  : 1;
   unsigned remapNode  : 1;
   unsigned remapColor : 1;
   unsigned unused     : 3;
   uint8 nodeMask;
   uint16 color;
} Alloc_RemapControl;

// remapping state for single page
typedef struct {
   Alloc_RemapControl op;
   PPN ppn;
   MPN mpnOld;
   MPN mpnNew;
} Alloc_RemapState;

// remap request batch
#define	ALLOC_REMAP_BATCH_SIZE	(PAGE_SIZE/sizeof(Alloc_RemapState))
typedef struct {
   Alloc_RemapState remap[ALLOC_REMAP_BATCH_SIZE];
} Alloc_RemapBatch;

// vmk high-to-low remap request queue
#define	ALLOC_REMAP_LOW_REQUESTS_MAX	(16)

/*
 * An Alloc_P2M is used to cache a physical-to-machine mapping
 * which may span two pages.
 */
#define ALLOC_P_2_M_CACHE_SIZE		(256)
typedef struct Alloc_P2M {
   PPN		firstPPN;
   PPN		lastPPN;
   MA 		maddr;
   uint32	copyHints;
   Bool		readOnly;
} Alloc_P2M;

// checkpoint buffer (in pages)
// best bandwidth for SCSI devices seems to be for at least 256K writes
#define	ALLOC_CHECKPOINT_BUF_SIZE	(64)

/*
 * Definitions for host map tracking.
 */
#ifdef VMX86_DEBUG
#define	ALLOC_HOST_MAPS_MAX		(16)
#else
#define	ALLOC_HOST_MAPS_MAX		(32)
#endif
#define	ALLOC_HOST_MAP_REMOVE		(0)
#define	ALLOC_HOST_MAP_ADD		(1)

#endif
