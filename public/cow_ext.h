/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * cow_ext.h --
 *
 *	External cow file structures
 */

#ifndef _COW_EXT_H
#define _COW_EXT_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMKERNEL

#include "includeCheck.h"

#define COWDISK_MAX_NAME_LEN    60
#define COWDISK_MAX_DESC_LEN    512
#define COWDISK_MAX_PARENT_FILELEN  1024

#define COW_NUM_LEAF_ENTRIES 4096

/*
 * Granularity is 1 sector, so we don't ever have to copy data from
 * the parent (which will require a synchronous read and write) before
 * doing an asynchronous write.  (Writes are always on sector
 * boundaries and have a length which is a multiple of the sector
 * size.)  We want to do this for VMkernel COW disks, because we don't
 * have the buffer cache of the host file system underneath to make
 * synchronous reads and writes be fast.  The cost of doing this is
 * that we have a greater overhead of meta-data in the COW file.
 */
#define COWDISK_DEFAULT_GRAN    1
#define COWDISK_DEFAULT_ROOTOFF 4

#define COWDISK_MAGIC           0x44574f43  // 'COWD'

#define COWDISK_ROOT            0x01
#define COWDISK_CHECKCAPABLE    0x02
#define COWDISK_INCONSISTENT    0x04

/*
 * # of disk sectors added when size of COW file is increased.
 * Currently 16 Mbytes.
 */
#define COWDISK_SIZE_INCREMENT	32768


typedef struct COWDisk_Header {
   uint32         magicNumber;
   uint32         version;
   uint32         flags;
   uint32         numSectors;	/* Total sectors in disk */
   uint32         granularity;  /* Size of data pointed to by leaf entries */
   uint32         rootOffset;   /* Start of root entries in COW file,
                                   in sectors */
   uint32         numRootEntries;/* # of root entries to cover numSectors*/
   uint32         freeSector;   /* Next free sector in COW file, but
                                   file length is real truth. */
   union {
      struct {
         uint32   cylinders;            //XXX unused
         uint32   heads;                //XXX unused
         uint32   sectors;              //XXX unused
      } root;
      struct {
         char     parentFileName[COWDISK_MAX_PARENT_FILELEN];           //XXX unused
         uint32   parentGeneration;                                     //XXX unused
      } child;
   } u;
   uint32         generation;	/* Generation - not used */
   char           name[COWDISK_MAX_NAME_LEN];           //XXX unused
   char           description[COWDISK_MAX_DESC_LEN];    //XXX unused
   uint32         savedGeneration;  /* Generation when clean - added for 1.1*/
   char           reserved[8];  // used to be drivetype (XXX we might not need this).
   /* Padding so header is integral number of sectors */
   char		  padding[400];
} COWDisk_Header; 

typedef struct COWRootEntry {
   uint32         sectorOffset;
} COWRootEntry;

#endif
