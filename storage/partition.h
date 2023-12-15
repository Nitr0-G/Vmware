/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * partition.h --
 *
 *      Disk partition table support.
 */

#ifndef _PARTITION_H_
#define _PARTITION_H_

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "return_status.h"
#include "partition_dist.h"
#include "vmnix_syscall.h"

typedef struct Partition_Table {
   uint32 numPartitions;
   Partition_Entry entries[VMNIX_MAX_PARTITIONS];
} Partition_Table;

struct SCSI_Handle;
extern VMK_ReturnStatus Partition_ReadTable(struct SCSI_Handle *handle, 
                                            Partition_Table *table);

#endif
