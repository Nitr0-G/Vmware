/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential

 * **********************************************************/

/*
 * fss_int.h --
 *
 *	vmkernel file system structures used by the fss module
 *	and the file system implementations (FS1, FS2, etc)
 */

#ifndef _FSS_INT_H
#define _FSS_INT_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "fsSwitch.h"
#include "semaphore.h"
#include "fsDeviceSwitch.h"

#define FS_NUM_HANDLES		128
#define FS_HANDLES_MASK		0x7f

#define FS_NUM_FILE_HANDLES	512
#define FS_FILE_HANDLES_MASK	0x1ff
#if FS_NUM_FILE_HANDLES < (4 * SERVER_MAX_VMS)
#error "FS_NUM_FILE_HANDLES might be too small"
/*
 * Need one handle per disk, plus an extra handle for every redo
 * log.  This should let every vm have 2 disks, and 2 redo logs.
 * (probably generous)
 */
#endif

#define FSS_FLUSH_PERIOD	20000   // Flush open FSes every 20 seconds

/* Flags for timer callback operations */
#define FSS_CALLBACK_FLUSH		0x01
#define FSS_CALLBACK_RESCAN		0x02

/* Invalid blocknumber constant for lazy zero */
#define FS_INT_INVALID_ZEROBLOCK	(-1)

#define FS_VMHBA_FMTSTR		"%s:%d:%d:%d"
#define FS_VMHBA_VAARGS(fs)	(fs)->diskName, (fs)->targetID, \
				(fs)->lun, (fs)->partition

#define FS_LOCK_FMTSTR		"%d.%d.%d.%d"
#define FS_LOCK_VAARGS(lockID)	((lockID) & 0xff),        \
				(((lockID) >> 8) & 0xff), \
				(((lockID) >> 16) & 0xff),\
				(((lockID) >> 24) & 0xff)

struct FSS_FSOps;
struct FSS_FileOps;

typedef struct ObjNameCacheEntry {
   Bool         used;
   char         name[FS_MAX_FILE_NAME_LENGTH];
   FSS_ObjectID oid;
} ObjNameCacheEntry;


typedef struct FSSRegisteredFS {
   struct FSSRegisteredFS *next;
   char               fsType[FSS_MAX_FSTYPE_LENGTH];
   struct FSS_FSOps   *fsOps;
   struct FSS_FileOps *fileOps;
   int                moduleID;
   uint16             fsTypeNum;  /* file system-provided type number */
} FSSRegisteredFS;


typedef struct FSDescriptorInt {
   uint16               fsTypeNum;
   int			moduleID;	// module ID of FS implementation

   struct FSS_FSOps	*fsOps;
   void			*fsData;	// FS implementation specific data
   uint32		openCount;      // # of instances of this volume open
   uint32		lockedCount;	// Current locked handles to this FS

   FDS_HandleID		devHandleID;	// Handle to underlying storage device
   const FDS_DeviceOps	*devOps;	// Ops to access underlying storage
                                        // device
   char			volumeName[FS_MAX_VOLUME_NAME_LENGTH];
   Bool			readOnly;
} FSDescriptorInt;


typedef struct FileDescriptorInt {
   struct FSS_FileOps		*fileOps;
   void				*fileData;  //FS implementation specific data

   // Number of handles to this file descriptor. Protected by object 'descLock'.
   // Invariant: openCount = readerCount + sharedReaderCount + writerCount
   uint32			openCount;

   // readerCount, sharedReaderCount, writerCount protected by object 'descLock'
   uint32			readerCount;       //#active readers on this file
   uint32			sharedReaderCount; //#shared readers (readonly)
   uint32			writerCount;       //#active writers on this file

   // Flags used to open file, protected by object 'descLock'
   uint32			openFlags;
   RWSemaphore                  ioAccess; 
   Bool				regenerateGeneration;
   World_ID			reserveID;	// World reserving this file
   SP_SpinLock			zeroLock;	//protects the zero* fields
   int32			zeroBlock; 	//index of block to be zeroed.
  	   					//Can be FS_INT_INVALID_ZEROBLOCK
   uint32			zeroOffset; 	//offset at which current block
                	                    	//  needs to be lazy zeroed
   /*
    * Slightly primitive name cache
    */
#define OBJ_NAME_CACHE_SIZE  10
   SP_SpinLock       nameCacheLock;
   ObjNameCacheEntry nameCache[OBJ_NAME_CACHE_SIZE];

} FileDescriptorInt;


typedef enum {
   OBJ_INVALID = 0,  // Uninitialized
   OBJ_VOLUME,    // Note: A volume root directory has type OBJ_DIRECTORY.
   OBJ_DIRECTORY,
   OBJ_REGFILE,
} FS_ObjectType;

struct ObjDescriptorInt;
typedef void (*ObjCBFn) (struct ObjDescriptorInt *objDesc);

typedef struct ObjDescriptorInt {
   /*
    * CAUTION: 'OCDescLock', 'refCount' and 'next' are exclusively for
    * use by the object cache and the FSS.
    */
   Semaphore OCDescLock;
   /*
    * -1: not initialized, no references, may be evicted
    *  0: initialized, no references, may be evicted
    * >0: initialized, >=1 reference, may not be evicted
    */
   int refCount;
   struct ObjDescriptorInt *next;

   FSS_ObjectID oid;

   /*
    * Fields below are specific to the FSS and FS implementations.
    */
   Semaphore descLock;           // Rank varies according to 'objType'
   ObjCBFn   evictCB;            // Called before evicting descriptor
   ObjCBFn   lastRefCB;          // Called when refCount goes to 0
   FS_ObjectType objType;        // Type of object
   struct ObjDescriptorInt *fs;  // Volume containing this object

   union {
      FSDescriptorInt   fs;
      FileDescriptorInt file;
   } sp;
} ObjDescriptorInt;

#define FSDESC(objDescPtr)   (&((objDescPtr)->sp.fs))
#define FILEDESC(objDescPtr) (&((objDescPtr)->sp.file))

typedef VMK_ReturnStatus (*FSS_CreateOp)(const char *deviceName, uint32 fileBlockSize, uint32 numFiles);
typedef VMK_ReturnStatus (*FSS_ExtendOp)(ObjDescriptorInt *fsObj, const char *extDeviceName, uint32 numFiles);
typedef VMK_ReturnStatus (*FSS_OpenOp)(const char *deviceName, uint32 flags, ObjDescriptorInt *fsObj);
typedef VMK_ReturnStatus (*FSS_CloseOp)(ObjDescriptorInt *fsObj, uint32 flags);
typedef VMK_ReturnStatus (*FSS_SetAttributeOp)(ObjDescriptorInt *fsObj, uint16 opFlag,	const char *fsName, int mode);
typedef VMK_ReturnStatus (*FSS_GetAttributesOp)(ObjDescriptorInt *fsObj, uint32 maxPartitions, VMnix_PartitionListResult *result);
typedef void (*FSS_TimerCallbackOp)(void *data, uint16 flags);
typedef VMK_ReturnStatus (*FSS_UpgradeVolumeOp)(ObjDescriptorInt *fsDesc);

typedef VMK_ReturnStatus (*FSS_LookupOp)(ObjDescriptorInt *parent, const char *child, FS_ObjectID *oid);
typedef VMK_ReturnStatus (*FSS_GetObjectOp)(FS_ObjectID *oid, ObjDescriptorInt *desc);
typedef void (*FSS_OIDtoStringOp)(const FS_ObjectID *oid, char *outString);

typedef VMK_ReturnStatus (*FSS_GetVolumeOIDOp)(const FS_ObjectID *src, FS_ObjectID *dst);

#if 0  // !!! todo
typedef VMK_ReturnStatus (*FSS_FsctlOp)(FsctlCommand cmd, void *arg, void *result); 
#endif

typedef VMK_ReturnStatus (*FSS_OpenFileOp)(ObjDescriptorInt *fileData, uint32 openFlags, void *dataIn);
typedef VMK_ReturnStatus (*FSS_CloseFileOp)(ObjDescriptorInt *fileDesc);
typedef VMK_ReturnStatus (*FSS_FileIOOp)(ObjDescriptorInt *filesDesc, SG_Array *sgArr, Async_Token *token, IO_Flags ioFlags, uint32 *bytesTransferred);
typedef VMK_ReturnStatus (*FSS_GetFileAttributesOp)(ObjDescriptorInt *file, FS_FileAttributes *attrs);
typedef VMK_ReturnStatus (*FSS_SetFileAttributesOp)(ObjDescriptorInt *file, uint16 opFlags, const FS_FileAttributes *attrs);
typedef VMK_ReturnStatus (*FSS_FlushFileOp)(ObjDescriptorInt *fileDesc);
typedef VMK_ReturnStatus (*FSS_ReserveFileOp)(ObjDescriptorInt *file, World_ID worldID, Bool testOnly);
typedef VMK_ReturnStatus (*FSS_ReleaseFileOp)(ObjDescriptorInt *fileDesc, World_ID worldID, Bool reset);

typedef VMK_ReturnStatus (*FSS_AbortCommandOp)(ObjDescriptorInt *fileDesc, SCSI_Command *cmd);
typedef VMK_ReturnStatus (*FSS_ResetCommandOp)(ObjDescriptorInt *fileDesc, SCSI_Command *cmd);
typedef VMK_ReturnStatus (*FSS_GetLayoutCommandOp)(ObjDescriptorInt *fileDesc,  uint64 offset, VMnix_FileGetPhysLayoutResult *result);

typedef VMK_ReturnStatus (*FSS_ReaddirOp)(ObjDescriptorInt *dirDesc, uint32 maxFiles, VMnix_ReaddirResult *result);
typedef VMK_ReturnStatus (*FSS_DumpOp)(ObjDescriptorInt *dirDesc, Bool verbose);

typedef VMK_ReturnStatus (*FSS_CreateFileOp)(ObjDescriptorInt *parent, const char *child, uint32 opFlags, uint32 descFlags, void *dataIn, FS_ObjectID *fileOID);

typedef VMK_ReturnStatus (*FSS_RemoveFileOp)(ObjDescriptorInt *parent, const char *childName);
typedef VMK_ReturnStatus (*FSS_RenameFileOp)(ObjDescriptorInt *srcDirDesc, const char *srcName, ObjDescriptorInt *dstDirDesc, const char *dstName);

#define FSS_OPERATION(fssop)	FSS_##fssop##Op FSS_##fssop

typedef struct FSS_FSOps {
   FSS_OPERATION(Create);
   FSS_OPERATION(Extend);
   FSS_OPERATION(Open);
   FSS_OPERATION(Close);
   FSS_OPERATION(SetAttribute);
   FSS_OPERATION(GetAttributes);
   FSS_OPERATION(TimerCallback);
   FSS_OPERATION(UpgradeVolume);
   FSS_OPERATION(Lookup);
   FSS_OPERATION(GetObject);
   FSS_OPERATION(OIDtoString);
   FSS_OPERATION(GetVolumeOID);
} FSS_FSOps;

typedef struct FSS_FileOps {
   FSS_OPERATION(OpenFile);
   FSS_OPERATION(CloseFile);
   FSS_OPERATION(FileIO);
   FSS_OPERATION(GetFileAttributes);
   FSS_OPERATION(SetFileAttributes);
   FSS_OPERATION(FlushFile);
   FSS_OPERATION(ReserveFile);
   FSS_OPERATION(ReleaseFile);
   FSS_OPERATION(AbortCommand);
   FSS_OPERATION(ResetCommand);
   FSS_OPERATION(GetLayoutCommand);	
   FSS_OPERATION(Readdir);
   FSS_OPERATION(Dump);
   FSS_OPERATION(CreateFile);
   FSS_OPERATION(RemoveFile);
   FSS_OPERATION(RenameFile);
} FSS_FileOps;


/*
 * Functions exported by FSS to internal VMK consumers
 */
int              FSS_RegisterFS(const char *fsType, FSS_FSOps *fsOps,
				int moduleID, uint16 fsTypeNum);
void             FSS_UnregisterFS(FSS_FSOps *fsOps, int moduleID);
VMK_ReturnStatus FSS_GetObject(FSS_ObjectID *oid, ObjDescriptorInt *objDesc);
VMK_ReturnStatus FSS_GetVolumeOID(const char *volumeName,
				  FSS_ObjectID *oid);
void             FSS_InitObjectDesc(ObjDescriptorInt *desc);
void             FSS_DestroyObjectDesc(ObjDescriptorInt *desc);
void             FSS_ObjEvictCB(ObjDescriptorInt *desc);
void             FSS_ObjLastRefCB(ObjDescriptorInt *desc);

static INLINE void FSS_BeginIOShared(FileDescriptorInt *fd);
static INLINE void FSS_EndIOShared(FileDescriptorInt *fd);
static INLINE void FSS_BeginIOExclusive(FileDescriptorInt *fd);
static INLINE void FSS_EndIOExclusive(FileDescriptorInt *fd);
static INLINE VMK_ReturnStatus FSS_UpgradeIOFromShared(FileDescriptorInt *fd);
static INLINE void FSS_DowngradeIOToShared(FileDescriptorInt *fd);
static INLINE void FSSSingletonSGArray(SG_Array *sgArr, uint64 offset,
                                       uint64 addr, uint32 length, SG_AddrType);

/*
 *-----------------------------------------------------------------------------
 *
 * FSS_BeginIOShared --
 *    Start a sequence of Shared IO to a file. Multiple shared IOs can occur
 *    at the same time.
 *
 * Results:
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
static INLINE void
FSS_BeginIOShared(FileDescriptorInt *fd)
{
   Semaphore_BeginRead(&fd->ioAccess);
}

/*
 *-----------------------------------------------------------------------------
 *
 * FSS_EndIOShared --
 *    Indicate that Shared IO is completely done.
 *
 * Results:
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
static INLINE void
FSS_EndIOShared(FileDescriptorInt *fd)
{
   Semaphore_EndRead(&fd->ioAccess);
}

/*
 *-----------------------------------------------------------------------------
 *
 * FSS_BeginIOExclusive --
 *    Start an exclusive IO operation (such as a file truncate or extend).
 *
 * Results:
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
static INLINE void
FSS_BeginIOExclusive(FileDescriptorInt *fd)
{
   Semaphore_BeginWrite(&fd->ioAccess);
}

/*
 *-----------------------------------------------------------------------------
 *
 * FSS_EndIOExclusive --
 *    Indicate that Exclusive IO is completely done.
 *
 * Results:
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
static INLINE void
FSS_EndIOExclusive(FileDescriptorInt *fd)
{
   Semaphore_EndWrite(&fd->ioAccess);
}

/*
 *-----------------------------------------------------------------------------
 *
 * FSS_UpgradeIOFromShared --
 *    Upgrade shared ioAccess lock to exclusive.
 *
 * Results:
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
static INLINE VMK_ReturnStatus
FSS_UpgradeIOFromShared(FileDescriptorInt *fd)
{
   return Semaphore_UpgradeFromShared(&fd->ioAccess);
}

/*
 *-----------------------------------------------------------------------------
 *
 * FSS_DowngradeIOToShared --
 *    Downgrade exclusive ioAccess lock to shared.
 *
 * Results:
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
static INLINE void
FSS_DowngradeIOToShared(FileDescriptorInt *fd)
{
   Semaphore_DowngradeToShared(&fd->ioAccess);
}

/*
 *-----------------------------------------------------------------------------
 *
 * FSSSingletonSGArray --
 *    Make a SG array containing a single virtual address element.
 *
 * Results:
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
static INLINE void
FSSSingletonSGArray(SG_Array *sgArr, uint64 offset,
                    uint64 addr, uint32 length, SG_AddrType addrType)
{
   sgArr->length = 1;
   sgArr->addrType = addrType;
   sgArr->sg[0].offset = offset;
   sgArr->sg[0].addr = addr;
   sgArr->sg[0].length = length;
}

static INLINE VMK_ReturnStatus
FSS_DeviceRead(const FDS_DeviceOps *devOps, FDS_HandleID fdsHandleID,
               uint64 offset, void *data, uint32 length)
{
   SG_Array sgArr;

   FSSSingletonSGArray(&sgArr, offset, (VA)data, length, SG_VIRT_ADDR);
   return devOps->FDS_SyncIO(fdsHandleID, &sgArr, TRUE);
}

static INLINE VMK_ReturnStatus
FSS_DeviceWrite(const FDS_DeviceOps *devOps, FDS_HandleID fdsHandleID,
                uint64 offset, void *data, uint32 length)
{
   SG_Array sgArr;

   FSSSingletonSGArray(&sgArr, offset, (VA)data, length, SG_VIRT_ADDR);
   return devOps->FDS_SyncIO(fdsHandleID, &sgArr, FALSE);
}

//Exported by fsNameSpace to FSS
VMK_ReturnStatus FSN_ObjNameCacheLookup(ObjDescriptorInt *desc,
                                        const char *name,
					FSS_ObjectID *oid);

#endif

