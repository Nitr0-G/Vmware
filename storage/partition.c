/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

#include "x86apic.h"
#include "vm_types.h"
#include "vmkernel.h"
#include "splock.h"
#include "prda.h"
#include "world.h"
#include "vmk_scsi.h"
#include "partition.h"
#include "vm_libc.h"

#define LOGLEVEL_MODULE Partition
#define LOGLEVEL_MODULE_LEN 9
#include "log.h"

#define MSDOS_LABEL_MAGIC		0xAA55
#define MSDOS_LABEL_MAGIC_OFFSET	510

/* 
 * Partition table parsing rules.
 *
 *    This file implements a Linux complaint partition table parser.There
 *    are NO written rules and NO industry standards on how fdisk should
 *    work, but there are a few givens:
 *
 *    1. In the MBR there can be 0-4 primary partitions, or,
 *       0-3 primary partitions and 0-1 extended partition entry.
 *    2. In an extended partition there can be 0-1 secondary partition
 *       entries and 0-1 nested-extended partition entries.
 *    3. Only 1 primary partition in the MBR can be marked active at any
 *       given time. The COS's life depends on the active flag, but
 *       we are exempt from parsing the active flag. Reason: If the thread
 *       of execution comes this far, the COS is already up and running.
 *    4. In most versions of fdisk, the first sector of a partition will be
 *       aligned such that it is at head 0, sector 1 of a cylinder. This
 *       means there may be unused sectors on the track(s) prior to the
 *       first sector of a partition and that there may be unused sectors
 *       following a partition table sector.
 *       For example, most new versions of FDISK start the first partition
 *       at cylinder 0, head 1, sector 1. This leaves the sectors at
 *       cylinder 0, head 0, sectors 2...n as unused sectors. The same
 *       layout may be seen on the first track of an extended partition.
 */

static VMK_ReturnStatus
PartitionExtended(SCSI_Handle *handle, Partition *p, uint8 *buffer, 
                  uint32 diskBlockSize, Partition_Table *table)
{
   uint32 firstSector = p->firstSector;
   uint32 extSector = firstSector;

   while (1) {
      int i;   
      Partition_Entry *entry;
      uint16 *label;
      VMK_ReturnStatus status;

      status = SCSI_Read(handle->handleID, (uint64)firstSector * diskBlockSize,
                         (void *)buffer, diskBlockSize);
      if (status != VMK_OK) {
	 return status;
      }
      label = (unsigned short *)(buffer + MSDOS_LABEL_MAGIC_OFFSET);
      if (*label != MSDOS_LABEL_MAGIC) {
	 Warning("Bad label 0x%x", (uint32)*label);
	 return VMK_NOT_FOUND;
      }

      entry = &table->entries[table->numPartitions];
      p = SCSI_FIRSTPTABLEENTRY(buffer);

      for (i = 0; i < 4; i++, p++) {
	 if (table->numPartitions == VMNIX_MAX_PARTITIONS) {
            /* One could have partitioned the disk using windows or
             * something. In this case, we'll exceed the Linux limit of 16
             * partitions and we should handle it gracefully.
             */
	    return VMK_OK;
	 }
	 if (p->numSectors == 0 || SCSI_ISEXTENDEDPARTITION(p)) {
	    continue;
	 }
	 memset(entry, 0, sizeof(*entry));
	 entry->startSector = firstSector + p->firstSector;
	 entry->numSectors = p->numSectors;	    
	 entry->type = p->type;
	 entry->number = table->numPartitions;
         entry->ptableLBN = firstSector;
         entry->ptableIndex = i;

	 LOG(2, "Logical: %d for %d type %d", 
             entry->startSector, entry->numSectors, entry->type);

         entry++;
         table->numPartitions++;
      }

      p = SCSI_FIRSTPTABLEENTRY(buffer);

      /*
       * Linux only processes the first extended partition so I do the same
       * thing.
       */
      for (i = 0; i < 4; i++, p++) {
	 if (p->numSectors != 0 && SCSI_ISEXTENDEDPARTITION(p))  {
	    LOG(2, "Nested extended: %d for %d type %#x", 
	        extSector + p->firstSector, p->numSectors, p->type);
	    firstSector = extSector + p->firstSector;
	    break;
	 }
      }

      if (i == 4) {
	 return VMK_OK;
      }
   }
}

/*
 * Layout of table->entries[]:
 *    [0] = whole disk; only 'numSectors' is valid.
 *    [1..4] = Sequentially stored non-zero size primary/extended partitions.
 *             Physical partition number is "[i].partition" (not "i").
 *    [5..VMNIX_MAX_PARTITIONS-1] = sequentially stored non-zero size extended
 *                                  partition table entries.
 * XXX Convoluted. The entries can be stored at the index == partition number. 
 */

VMK_ReturnStatus
Partition_ReadTable(SCSI_Handle *handle, Partition_Table *table)
{
   Partition *p;
   uint16 *label;
   Partition_Entry *entry;
   int i;
   VMK_ReturnStatus status;
   SCSI_Target *target;
   uint32 diskBlockSize;
   uint8 *buffer;

   memset(table, 0, sizeof(*table));
   target = handle->target;
   diskBlockSize = target->blockSize;
   buffer = Mem_Alloc(diskBlockSize);

   table->numPartitions = 1;
   entry = table->entries;

   entry->numSectors = target->numBlocks;
   entry++;

   if (buffer == NULL) {
      return VMK_NO_RESOURCES;
   }

   status = SCSI_Read(handle->handleID, 0, buffer, diskBlockSize);
   if (status != VMK_OK) {
      goto exit;
   }
   SCSI_ReadGeometry(handle, buffer, diskBlockSize);

   label = (unsigned short *)(buffer + MSDOS_LABEL_MAGIC_OFFSET);
   if (*label != MSDOS_LABEL_MAGIC) {
      goto exit;
   }

   p = SCSI_FIRSTPTABLEENTRY(buffer);

   for (i = 1; i <= 4; i++, p++) {
      if (p->numSectors != 0) {
	 LOG(2, "%s partition: %d for %d type %#x", 
	     SCSI_ISEXTENDEDPARTITION(p) ? "Extended" : "Primary",
             p->firstSector, p->numSectors, p->type);
	 memset(entry, 0, sizeof(*entry));		 
	 entry->startSector = p->firstSector;
	 entry->numSectors = p->numSectors;
	 entry->type = p->type;
	 entry->number = i;
         entry->ptableLBN = 0;
         entry->ptableIndex = i - 1;
	 entry++;
      }
   }

   table->numPartitions = 5;

   p = SCSI_FIRSTPTABLEENTRY(buffer);

   for (i = 0; i < 4; i++, p++) {
      if (p->numSectors != 0 && SCSI_ISEXTENDEDPARTITION(p))  {
	 status = PartitionExtended(handle, p, buffer, diskBlockSize, table);
	 if (status != VMK_OK) {
	    if (status == VMK_NOT_FOUND) {
	       status = VMK_OK;
	       continue;
	    } else {
	       goto exit;
	    }
	 }
      }
   }

exit:

   Mem_Free(buffer);

   return status;
}
