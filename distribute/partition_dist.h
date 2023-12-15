/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * partition_dist.h --
 *
 *      Partition struct definitions. 
 */

#ifndef _PARTITION_DIST_H_
#define _PARTITION_DIST_H_

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_DISTRIBUTE
#define INCLUDE_ALLOW_VMK_MODULE
#include "includeCheck.h"

typedef struct Partition_Entry {
   uint32 startSector;
   uint32 numSectors;
   uint32 type;
   uint32 number;
   uint32 ptableLBN;  //Absolute LBN of ptable containing this entry
   uint8  ptableIndex;//Index of this entry in the ptable on 'ptableLBN'
} Partition_Entry;

#endif
