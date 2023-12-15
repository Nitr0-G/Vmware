/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * numa_ext.h --
 *
 *	This is the header file for VMkernel NUMA module.
 */


#ifndef _NUMA_EXT_H
#define _NUMA_EXT_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMMEXT
#define INCLUDE_ALLOW_VMCORE
#include "includeCheck.h"

/*
 * Constants
 */
#define NUMA_LG_MAX_NODES                      (2)
#define NUMA_MAX_NODES                         (1 << NUMA_LG_MAX_NODES)
#define NUMA_MAX_MEM_RANGES                    (8)
#define INVALID_NUMANODE                       ((NUMA_Node) -1)


/*
 * Typedefs
 */
typedef uint8    NUMA_Node;


/*
 * Structures
 */
typedef struct {
   MPN   startMPN;
   MPN   endMPN;
} NUMA_MemRange;

typedef struct {
   int           numMemRanges;
   NUMA_MemRange memRange[NUMA_MAX_MEM_RANGES];
} NUMA_MemRangesList;


#endif
