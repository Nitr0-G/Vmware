/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * fsSwitch.h --
 *
 *	vmkernel file system switch interface and exported functions.
 */

#ifndef _FSSWITCH_H
#define _FSSWITCH_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "scattergather.h"
#include "async_io.h"
#include "scsi_ext.h"
#include "return_status.h"
#include "fs_ext.h"
#include "vmnix_syscall.h"
#include "semaphore_ext.h"


/*
 * Lock ordering:
 *
 * 9. fileHandleLock (spin lock)
 * 6. cowlock        (sema)
 * 6. fs->descLock   (sema)
 * 5. fd->ioAccess   (rwsema)  // ???
 * 4. file->descLock (sema)
 * 3. dir->descLock  (sema)
 * 2. fsLock         (sema)
 * 0,1. Object cache locks
 */

#define FS_SP_RANK_FILE_HANDLE  (SP_RANK_LEAF)

#define FS_SEMA_RANK_COWLOCK        (SEMA_RANK_STORAGE - 1)
#define FS_SEMA_RANK_FS_DESCLOCK    (SEMA_RANK_STORAGE - 1)
#define FS_SEMA_RANK_FILE_DESCLOCK  (MIN(FS_SEMA_RANK_FS_DESCLOCK,  \
					 FS_SEMA_RANK_COWLOCK) - 1)
#define FS_SEMA_RANK_DIR_DESCLOCK   (FS_SEMA_RANK_FILE_DESCLOCK - 1)
#define FS_SEMA_RANK_FSLOCK         (FS_SEMA_RANK_DIR_DESCLOCK - 1)

#define OC_SEMA_RANK_OCDESC_OBJ    (FS_SEMA_RANK_FSLOCK - 1)
#define OC_SEMA_RANK_OBJDESC_TABLE (OC_SEMA_RANK_OCDESC_OBJ - 1)

#if SEMA_RANK_FS > FS_SEMA_RANK_FSLOCK
#error "SEMA_RANK_FS must be <= lowest ranked FS semaphore"
#endif

extern Semaphore fsLock;
extern SP_SpinLock fileHandleLock;
extern SP_SpinLock renewLock;

extern struct FSDescriptorInt *fsDesc;
extern struct FileDescriptorInt *renewList;
extern Bool doingRenew;

/* Size of buffer to hold VMFS volume attributes. */
#define FSS_MAX_PARTITIONLIST_ENTRIES	32
extern char fsAttributeBuf[VMNIX_PARTITION_ARR_SIZE(FSS_MAX_PARTITIONLIST_ENTRIES)];


extern void FSS_Init(void);

/*
 * The File System Switch (FSS) interface.
 *
 * This is the interface to the various file system implementations
 * (VMFS n, NFS, etc.). FSS automatically forwards calls to the right
 * implementation.
 */

#define FSS_OID_STRING_SIZE 128

/*
 * Object operations
 */
extern VMK_ReturnStatus FSS_Lookup(FSS_ObjectID *parentOID, const char *childName,
				   FSS_ObjectID *childOID);
extern VMK_ReturnStatus FSS_LookupFileHandle(FS_FileHandleID fileHandleID,
					     FSS_ObjectID *oid);
extern VMK_ReturnStatus FSS_OIDtoString(const FSS_ObjectID *oid, char *outString);

/*
 * Non-volume object operations
 */
extern VMK_ReturnStatus FSS_Dump(FSS_ObjectID *oid, Bool verbose);

/*
 * Volume operations
 */
extern VMK_ReturnStatus FSS_Create(const char *fsType, const char *deviceName,
                                   uint32 fileBlockSize, uint32 numFiles);
extern VMK_ReturnStatus FSS_Extend(const char *volumeName, const char *extDeviceName,
                                   uint32 numFiles);
extern VMK_ReturnStatus	FSS_Probe(const char *volumeName, Bool rescanInProgress);
extern VMK_ReturnStatus FSS_GetAttributes(FSS_ObjectID *oid,
					  uint32 maxPartitions,
					  VMnix_PartitionListResult *result);
extern VMK_ReturnStatus FSS_SetAttributes(const char *volumeName, uint16 opFlags,
					  const char *fsName, int mode);
extern VMK_ReturnStatus FSS_UpgradeVolume(const char *volumeName);

/*
 * Directory operations
 */
extern VMK_ReturnStatus FSS_Readdir(FSS_ObjectID *dirOID, uint32 maxDirEntries,
                                    VMnix_ReaddirResult *result);

/*
 * File operations
 */
extern VMK_ReturnStatus	FSS_CreateFile(FSS_ObjectID *parentOID, const char *fileName,
                                       uint32 createFlags, void *data,
                                       FSS_ObjectID *fileOID);
extern VMK_ReturnStatus	FSS_OpenFile(FSS_ObjectID *fileOID, uint32 flags,
				     FS_FileHandleID *fileHandle);
extern VMK_ReturnStatus	FSS_CloseFile(FS_FileHandleID handle);
extern VMK_ReturnStatus FSS_ChangeMode(FS_FileHandleID fileHandleID, Bool exclusive);
extern VMK_ReturnStatus	FSS_RemoveFile(FSS_ObjectID *parentOID, const char *fileName);
extern VMK_ReturnStatus	FSS_RenameFile(FSS_ObjectID *srcDirOID, const char *srcName,
				       FSS_ObjectID *dstDirOID, const char *dstName);
extern VMK_ReturnStatus	FSS_GetFileAttributes(FSS_ObjectID *fileOID,
                                              FS_FileAttributes *attrs);
extern VMK_ReturnStatus FSS_SetFileAttributes(FSS_ObjectID *fileOID,
                                              uint16 opFlags,
                                              const FS_FileAttributes *attrs);

extern VMK_ReturnStatus FSS_BufferIO(FS_FileHandleID fileHandleID,
                                      uint64 offset, uint64 data, uint32 length,
                                      IO_Flags ioFlags, SG_AddrType addrType,
				     uint32 *bytesTransferred);
extern VMK_ReturnStatus FSS_SGFileIO(FS_FileHandleID fileHandleID,
                                     SG_Array *sgArr, IO_Flags ioFlags,
                                     uint32 *bytesTransferred);
extern VMK_ReturnStatus FSS_AsyncFileIO(FS_FileHandleID fileHandleID, 
                                        SG_Array *sgArr, Async_Token *token,
                                        IO_Flags ioFlags);

extern VMK_ReturnStatus FSS_ReserveFile(FS_FileHandleID fileHandleID,
                                        World_ID worldID, Bool testOnly);
extern VMK_ReturnStatus FSS_ReleaseFile(FS_FileHandleID fileHandleID,
                                        World_ID worldID, Bool reset);
extern Bool FSS_IsMultiWriter(FS_FileHandleID fileHandleID);


/*
 * Miscellanous
 * - Some of these should probably go into an ioctl-style mechanism.
 */
extern VMK_ReturnStatus FSS_QueryRawDisk(const VMnix_QueryRawDiskArgs *args,
                                         VMnix_QueryRawDiskResult *result);
extern VMK_ReturnStatus FSS_FileGetPhysLayout(FS_FileHandleID fileHandleID, 
					      uint64 offset,
					      VMnix_FileGetPhysLayoutResult 
					      *result);
extern VMK_ReturnStatus FSS_AbortCommand(FS_FileHandleID fileHandleID,
					 SCSI_Command *cmd);
extern VMK_ReturnStatus FSS_ResetCommand(FS_FileHandleID fileHandleID,
					 SCSI_Command *cmd);
extern VMK_ReturnStatus FSS_ListPEs(const char *volumeName,
                                    uint32 maxPartitions,
                                    VMnix_PartitionListResult *result);
extern void FSS_BeginRescan(void);
extern void FSS_EndRescan(void);


/*
 * To deprecate
 */
extern VMK_ReturnStatus FSS_BufferCacheIO(FSS_ObjectID *fileOID,
                                          uint64 offset, uint64 data,
                                          uint32 length, IO_Flags ioFlags,
                                          SG_AddrType addrType,
                                          uint32 *bytesTransferred);

#endif //_FSSWITCH_H

