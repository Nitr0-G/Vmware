/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * fs_ext.h --
 *
 *	External vmkernel file system structures
 */

#ifndef _FS_EXT_H
#define _FS_EXT_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMMEXT

#include "includeCheck.h"

#include "vm_version.h" //For ToolsVersion
#include "scsi_ext.h"   //For SCSI_HandleID

#define FS_ROOT_NAME		"vmfs"

/* Flags for FSS_Open() */

// Open FS without on-disk lock
#define FS_OPEN_FORCE			0x01
// Open VMFS read-only if VMFS is shared
#define FS_OPEN_READ_ONLY		0x02
// Open FS with on-disk lock
#define FS_OPEN_LOCKED			0x04
// Open FS as part of SCSI adapter/vmfs rescan
#define FS_OPEN_RESCAN			0x08

typedef uint32 IO_Flags;

/* Flags for FSS_FileIO/AsyncFileIO/SGFileIO - IO_Flags */
#define FS_WRITE_OP			0x00
#define FS_READ_OP			0x01   /* Write op otherwise */
#define FS_CANTBLOCK			0x02   /* AsyncIO can block on metadata
                                                * read if not set */

/* Flags for file and directory open (FSS_OpenFile, FSS_CreateFile) */
#define FILEOPEN_DISK_IMAGE_ONLY  0x00000004	// Error if not VM disk file
#define FILEOPEN_READ		  0x00000008	// Open file for read only,
                                                // allow new readers/writers
#define FILEOPEN_READONLY	  0x00000010	// Readers only, allow new
                                                // readers. but no writers
#define FILEOPEN_WRITE		  0x00000020	// Open for write only,
                                                // allow new readers/writers
#define FILEOPEN_EXCLUSIVE	  0x00000040	// Open file exclusively
#define FILEOPEN_PHYSICAL_RESERVE 0x00000080	// Pass through SCSI reserve,
						// reset to physical bus


/* Flags for file and directory open are available to FS switch and
 * FS specific implementations only
 * Also see FSx_FILEOP_FLAG_MASK for recognized opFlags for each FS
 * implementation
 */
// Foll. flags available VMFS-1 onwards
#define FILEOPEN_CANT_EXIST	  0x00000200    // Error if file already exists
// Foll. flags available VMFS-2.00 onwards
#define FILEOPEN_QUERY_RAWDISK	  0x00000400	// Query vmhba name for
						//rawdisk mapping

/*
 * Flags for each file descriptor.
 */
// Flags available in all VMware FSes
#define FS_SWAP_FILE		0x001
#define FS_COW_FILE		0x002
#define FS_NOT_ESX_DISK_IMAGE   0x004
#define FS_NO_LAZYZERO		0x008
// Flags available VMFS-2 onwards
#define FS_RAWDISK_MAPPING	0x010
//Keep 0x020, 0x040, 0x080 free for FS-2 expansion
// Flags available VMFS-3 onwards
#define FS_DIRECTORY		0x100
#define FS_LINK			0x200

/* Flags stored in the file descriptor on disk. Valid flags are defined
 * above: FS_COW_FILE, FS_NO_ESX_DISK_IMAGE, FS_NO_LAZYZERO, and so on.
 */
typedef uint32 FS_DescriptorFlags;

#define FS_FILEFLAGS_2_STR(flags)		      	\
        ((flags) & FS_DIRECTORY ? "dir" :		\
         ((flags) & FS_RAWDISK_MAPPING ? "raw disk" :	\
          ((flags) & FS_COW_FILE ? "redo log" :		\
           (((flags) & FS_NOT_ESX_DISK_IMAGE) ?		\
            ((flags) & FS_SWAP_FILE ? 			\
             "swap" : "") : "disk"))))

/* Flags for FSS_CreateFile() */
#define FS_CREATE_CAN_EXIST		0x01
#define FS_CREATE_SWAP			0x02
#define FS_CREATE_RAWDISK_MAPPING	0x20
#define FS_CREATE_DIR			0x40

typedef int64 FS_FileHandleID;
typedef int64 COW_HandleID;

#define FS_INVALID_FS_HANDLE   (-1)
#define FS_INVALID_FILE_HANDLE (-1)
#define COW_INVALID_HANDLE     (-1)

#define COW_MAX_REDO_LOG 	32

/* Bits in config field */
#define FS_CONFIG_SHARED	1	// VMFS is shared among multiple hosts
#define FS_CONFIG_PUBLIC	2	// VMFS is accessible to multiple hosts

#define FSS_MAX_FSTYPE_LENGTH 	8

/* disk tail defines and typedef */

#define FS_DISK_TAIL_SIZE	512
#define FS_DISK_IMAGE_MAGIC 0x83563204
#define FS_DISK_IMAGE_LONG_MAGIC "This is a VMware ESX Server disk image."
#define FS_DISK_IMAGE_LONG_MAGIC_SIZE 40
typedef struct FS_DiskImageTail {
   uint32 magic;
   char longMagic[FS_DISK_IMAGE_LONG_MAGIC_SIZE];
   uint64 fileSize;
   uint32 generation;
   uint32 cowFile;
   ToolsVersion toolsVersion;
   uint32 virtualHWVersion;
   char pad[FS_DISK_TAIL_SIZE - FS_DISK_IMAGE_LONG_MAGIC_SIZE
            - 3 * sizeof(uint32) - sizeof(uint64)
            - sizeof(ToolsVersion) - sizeof(uint32)];
} FS_DiskImageTail;

typedef struct FS_FileAttributes {
   uint64		length;			// Length of file
   uint32		diskBlockSize;		// Block size of disk
   uint32		fsBlockSize;		// Block size of file system
   uint32		numBlocks;		// Number of blocks used by file
   FS_DescriptorFlags	flags;
   uint32		generation;		// Generation number
   int32        	descNum;                // Descriptor number 
   uint32       	mtime;
   uint32       	ctime;
   uint32       	atime;
   uint16		uid;
   uint16		gid;
   uint16		mode;
   ToolsVersion		toolsVersion;		// Version of tools on disk
   uint32		virtualHWVersion;	// Virt HW version of VM using disk
   SCSI_HandleID	rdmRawHandleID;		// Handle to raw disk, if
                                                //   file is RDM
   char			fileName[128];		// XXX: should be deleted
} FS_FileAttributes;

#define FS_TYPENUM_INVALID	0
#define FS_TYPENUM_ROOT		1

#define FS_SLASH_VMFS_MAGIC_STR		"/vmfs"

#define FS_OID_MAX_LENGTH 64

#define FS_OID_FMTSTR \
   "%x %d %"FMT64"x %"FMT64"x %"FMT64"x %"FMT64"x %"FMT64"x %"FMT64"x %"FMT64"x %"FMT64"x"

#define FS_OID_VAARGS(oidPtr)                       \
      (oidPtr)->fsTypeNum, (oidPtr)->oid.length,    \
      *((uint64 *)(&(oidPtr)->oid.data[0])),        \
      *((uint64 *)(&(oidPtr)->oid.data[8])),        \
      *((uint64 *)(&(oidPtr)->oid.data[16])),       \
      *((uint64 *)(&(oidPtr)->oid.data[24])),       \
      *((uint64 *)(&(oidPtr)->oid.data[32])),       \
      *((uint64 *)(&(oidPtr)->oid.data[40])),       \
      *((uint64 *)(&(oidPtr)->oid.data[48])),       \
      *((uint64 *)(&(oidPtr)->oid.data[56]))

typedef struct FS_ObjectID {
   uint16 length;
   char   data[FS_OID_MAX_LENGTH];
} FS_ObjectID;

typedef struct FSS_ObjectID {
   uint16      fsTypeNum;
   FS_ObjectID oid;
} FSS_ObjectID;

/*
 *----------------------------------------------------------------------
 *
 * FSS_CopyOID --
 *    Copy the OID pointed to by 'srcPtr' to the OID pointed to by
 *    'dstPtr'. Not declared as a function because of memcpy #include
 *    issues.
 *
 * Results:
 *
 * Side effects:
 *
 *----------------------------------------------------------------------
 */
#define FSS_CopyOID(dstPtr, srcPtr)                                     \
   do {                                                                 \
      ASSERT(FSSCheckOID((srcPtr)));                                    \
      ASSERT(sizeof(*(srcPtr)) == sizeof(FSS_ObjectID));                \
      ASSERT(sizeof(*(dstPtr)) == sizeof(FSS_ObjectID));                \
                                                                        \
      memcpy((dstPtr), (srcPtr), sizeof(*(srcPtr)));                    \
   } while (FALSE)
      
/*
 *----------------------------------------------------------------------
 *
 * FSS_OIDIsEqual --
 *    Returns TRUE if the two OIDs specified have the same file system
 *    type, length and data.  Otherwise, returns FALSE.
 *    Not declared as a function because of C memcpy #include issues.
 *
 * Results:
 *
 * Side effects:
 *
 *----------------------------------------------------------------------
 */
#define FSS_OIDIsEqual(XPtr, YPtr)                              \
   ((XPtr)->fsTypeNum == (YPtr)->fsTypeNum &&                   \
    (XPtr)->oid.length == (YPtr)->oid.length &&                 \
    memcmp((XPtr)->oid.data, (YPtr)->oid.data, (XPtr)->oid.length) == 0)

#define FSS_InitOID(oidPtr)                       \
   do {                                           \
      ASSERT(sizeof(*(oidPtr)) == sizeof(FSS_ObjectID));        \
      memset((oidPtr), 0, sizeof(*(oidPtr)));                   \
   } while (FALSE)

#define FSS_MakeVMFSRootOID(oidPtr)                                     \
   do {                                                                 \
      FSS_InitOID((oidPtr));                                            \
      (oidPtr)->fsTypeNum = FS_TYPENUM_ROOT;                            \
      ASSERT(strlen(FS_SLASH_VMFS_MAGIC_STR) < FS_OID_MAX_LENGTH);      \
      strcpy((oidPtr)->oid.data, FS_SLASH_VMFS_MAGIC_STR);              \
      (oidPtr)->oid.length = strlen(FS_SLASH_VMFS_MAGIC_STR) + 1;       \
   } while (FALSE)

#define FSS_IsVMFSRootOID(oidPtr)                                  \
   ((oidPtr)->fsTypeNum == FS_TYPENUM_ROOT &&                      \
    (oidPtr)->oid.length == strlen(FS_SLASH_VMFS_MAGIC_STR) + 1 && \
    strcmp((oidPtr)->oid.data, FS_SLASH_VMFS_MAGIC_STR) == 0)

#define FSS_MakeInvalidOID(oidPtr)                      \
   do {                                                 \
      FSS_InitOID((oidPtr));                            \
      (oidPtr)->fsTypeNum = FS_TYPENUM_INVALID;         \
   } while (FALSE)

// XXX: CheckOID should actually become IsValidOID once VC becomes a FS
// driver. Dont call this.
#define FSSCheckOID(oidPtr)                       \
   ((oidPtr)->fsTypeNum != FS_TYPENUM_INVALID &&  \
    (oidPtr)->oid.length != 0 &&                  \
    (oidPtr)->oid.length <= FS_OID_MAX_LENGTH)

#define FSS_IsValidOID(oidPtr)                    \
   ((oidPtr)->fsTypeNum != FS_TYPENUM_INVALID &&  \
    (oidPtr)->fsTypeNum != FS_TYPENUM_ROOT &&     \
    (oidPtr)->oid.length != 0 &&                  \
    (oidPtr)->oid.length <= FS_OID_MAX_LENGTH)


#endif
