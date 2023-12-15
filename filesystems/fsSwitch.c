/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential

 * **********************************************************/

/*
 * fsSwitch.c --
 *
 *      This is the vmkernel file system switch implementation.
 *      The implementation is documented in the Engineering guide at:
 *      http://vmweb/~mts/WebSite/guide/vmkernel/vmfs.html
 */
 
#include "vm_types.h"
#include "vm_libc.h"
#include "x86.h"
#include "vmkernel.h"
#include "splock.h"
#include "world.h"
#include "fss_int.h"
#include "memalloc.h"
#include "scattergather.h"
#include "vmk_scsi.h"
#include "timer.h"
#include "helper.h"
#include "semaphore_ext.h"
#include "util.h"
#include "config.h"
#include "libc.h"
#include "host_dist.h"
#include "vmkevent.h"
#include "mod_loader_public.h"
#include "fsNameSpace.h"
#include "volumeCache.h"
#include "cow.h"
#include "objectCache.h"

#define LOGLEVEL_MODULE FSS
#define LOGLEVEL_MODULE_LEN 3
#include "log.h"


#define FSS_GET_FH_PTR(fileHandleID)   (&fsFileHandleTable[(fileHandleID) & FS_FILE_HANDLES_MASK])
#define FSS_BAD_FHI(fileHandleID, fhi) (((fhi)->fileDesc == NULL) || ((fhi)->handleID != (fileHandleID)))

#define FSS_FD2FILEOPS(objDesc)	((objDesc)->sp.file.fileOps)
#define FSS_FH_FILEOPS(fhi)	(FSS_FD2FILEOPS(fhi->fileDesc))

 
typedef struct FSFileHandle {
   FS_FileHandleID 	handleID;
   ObjDescriptorInt 	*fileDesc;
   uint32		openFlags;	// flags used to open this handle to file
   Bool			inUse;
} FSFileHandle;


/*
 * Lock for opening/closing file systems.  Protects fssOpenedVolumeList,
 * fsAttributeBuf and startedFlush.
 */
Semaphore fsLock;
/*
 * List of registered file systems. Protected by regFSLock.
 */
static SP_SpinLock regFSLock;
static FSSRegisteredFS *fssRegisteredFSList = NULL;
/*
 * Buffer to hold VMFS volume attributes.
 */
char fsAttributeBuf[VMNIX_PARTITION_ARR_SIZE(FSS_MAX_PARTITIONLIST_ENTRIES)];

/*
 * Protects fsFileHandleTable.
 */
static SP_SpinLock handleLock;
/*
 * Table of (handle, file descriptor) pairs.  Protected by handleLock.
 */
static FSFileHandle fsFileHandleTable[FS_NUM_FILE_HANDLES];


static void FSSRescan(void);
static VMK_ReturnStatus FSSOpenVolume(const char *volumeName, uint32 flags,
                                      ObjDescriptorInt **theVolume);
static void FSSCloseVolume(ObjDescriptorInt *volDesc, uint32 openFlags);

static VMK_ReturnStatus FSSCreateFile(ObjDescriptorInt *dirDesc, const char *name,
				      uint32 opFlags, FS_DescriptorFlags descFlags,
				      void *dataIn,
				      FS_ObjectID *fileOID);
static VMK_ReturnStatus FSSOpenFile(ObjDescriptorInt *fileDesc, uint32 openFlags, void *dataIn,
				    Bool getDescLock,
				    FS_FileHandleID *fileHandleID);
static VMK_ReturnStatus FSSFileIO(FS_FileHandleID fileHandleID,
                                  SG_Array *sgArr, Async_Token *token,
                                  IO_Flags ioFlags, 
                                  uint32 *bytesTransferred);

static void FSSZeroOutBlockTail(ObjDescriptorInt *fileDesc);
static VMK_ReturnStatus FSSSetOpenMode(ObjDescriptorInt *fileDesc, uint32 openMode);

static int FSSGetFreeFileHandle(void);
static VMK_ReturnStatus FSSGetNewFileHandle(FS_FileHandleID *newFhID);
static void FSSReleaseFileHandle(FS_FileHandleID fileHandleID);

static VMK_ReturnStatus FSSGetRegisteredFS(uint16 fsTypeNum,
                                           struct FSSRegisteredFS **regFS);
static void FSSReleaseRegisteredFS(const struct FSSRegisteredFS *regFS);

     
/*
 *-----------------------------------------------------------------------------
 *
 * FSS_Init --
 *    Initialize the switch data structures. Called from init.c
 *
 * Results:
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
void
FSS_Init(void)
{
   int i;
   VMK_ReturnStatus status;

   SP_InitLock("regFS", &regFSLock, SP_RANK_FSDRIVER_LOWEST - 1);

   for (i = 0; i < FS_NUM_FILE_HANDLES; i++) {
      fsFileHandleTable[i].handleID = i;
      fsFileHandleTable[i].inUse = FALSE;
   }

   Semaphore_Init("fsLock", &fsLock, 1, FS_SEMA_RANK_FSLOCK);
   SP_InitLock("fsHandleLock", &handleLock, SP_RANK_LEAF);

   // Initialize the volume cache
   VC_Init();

   // Initialize COW related stuff
   COW_Init();

   // Initialize the object cache
   status = OC_Init();
   ASSERT_NOT_IMPLEMENTED(status == VMK_OK);
}


/*
 *-----------------------------------------------------------------------------
 *
 * FSS_RegisterFS --
 *    Register a file system implementation.
 *
 *    'fsTypeNum' is an implementation-provided number that is used to
 *    partition the FSS-exported OID space. It must be non-zero and
 *    unique among implementations.
 *
 * Results:
 *    The file system "fsType" is registered and a unique ID (>=0) is
 *    returned for the implementation to use. -1 is returned in case of error.
 *
 *-----------------------------------------------------------------------------
 */
int
FSS_RegisterFS(const char *fsType, FSS_FSOps *fsOps, int moduleID, uint16 fsTypeNum)
{
   FSSRegisteredFS *driver, *rfs;

   ASSERT(strlen(fsType) < FSS_MAX_FSTYPE_LENGTH);
   /* Force the file system to implement all the functions that we
    * declare in the VMKernel filesystem interface (i.e. in
    * fsSwitch.h)
    */
   ASSERT(fsOps->FSS_Create && fsOps->FSS_Extend &&
          fsOps->FSS_SetAttribute && fsOps->FSS_GetAttributes &&
          fsOps->FSS_Open && fsOps->FSS_Close &&
          fsOps->FSS_TimerCallback && fsOps->FSS_UpgradeVolume);

   /* Verify fsTypeNum. */
   if (fsTypeNum < 21) {
      Warning("Invalid fsTypenum %u (fsType %s)", fsTypeNum, fsType);
      return -1;
   }

   /* verify fsTypeNum is unique among registered implementations */
   SP_Lock(&regFSLock);
   rfs = fssRegisteredFSList;
   while (rfs) {
      if (rfs->fsTypeNum == fsTypeNum) {
	 SP_Unlock(&regFSLock);
	 Warning("Duplicate fsTypeNum provided (fsType %s): 0x%x", fsType, fsTypeNum);
	 return -1;
      }
      rfs = rfs->next;
   }

   driver = (FSSRegisteredFS *) Mem_Alloc(sizeof(*driver));
   if (driver == NULL) {
      SP_Unlock(&regFSLock);
      return -1;
   }
   memset(driver, 0, sizeof(*driver));
   strncpy(driver->fsType, fsType, sizeof(driver->fsType));
   driver->fsOps = fsOps;
   driver->moduleID = moduleID;
   driver->fsTypeNum = fsTypeNum;
   Log("Registered fs %s, module %d, fsTypeNum 0x%x",
       driver->fsType, driver->moduleID, driver->fsTypeNum);

   if (fssRegisteredFSList == NULL) {
      driver->next = fssRegisteredFSList;
      fssRegisteredFSList = driver;
   } else { 
      rfs = fssRegisteredFSList;
      while (rfs->next) {
         rfs = rfs->next;
      }
      rfs->next = driver;
   }
   SP_Unlock(&regFSLock);

   VC_RescanVolumes(NULL, NULL);
   return 0;
}


/*
 *-----------------------------------------------------------------------------
 *
 * FSS_UnregisterFS --
 *    Unregister a FS implementation from the FS switch, using the moduleID
 *    as the search key. Use fsOps as a primitive way to make sure that a
 *    rogue module can't unregister other modules.
 *
 *-----------------------------------------------------------------------------
 */
void
FSS_UnregisterFS(FSS_FSOps *fsOps, int moduleID)
{
   FSSRegisteredFS *prev, *cur;
   
   SP_Lock(&regFSLock);
   for (prev = NULL, cur = fssRegisteredFSList;
        cur != NULL; prev = cur, cur = cur->next) {
      if (cur->fsOps == fsOps && cur->moduleID == moduleID) {
         Log("Unregistering file system (fsType %s, moduleID %d)", cur->fsType, cur->moduleID);
         if (prev == NULL) {
            fssRegisteredFSList = cur->next;
         } else {
            prev->next = cur->next;
         }
         Mem_Free(cur);
         break;
      }
   }
   SP_Unlock(&regFSLock);
   VC_RescanVolumes(NULL, NULL);
}


/*
 * Initialize the descriptor corresponding to the object named by
 * 'oid'. Call down to initialize implementation-specific fields.
 *
 * 'objDesc' points to a buffer of sizeof(ObjDescriptorInt) bytes. All
 * fields above the "FSS and file system specifics" marker must have
 * been initialized by the caller. All other fields may be assumed to
 * be zeroed out.
 */
VMK_ReturnStatus
FSS_GetObject(FSS_ObjectID *oid,         // IN
	      ObjDescriptorInt *objDesc) // IN/OUT
{
   VMK_ReturnStatus status;
   FSSRegisteredFS  *regFS;

   LOG(3, FS_OID_FMTSTR, FS_OID_VAARGS(oid));

   status = FSSGetRegisteredFS(oid->fsTypeNum, &regFS);
   if (status != VMK_OK) {
      return status;
   }

   /* Call down to perform implementation-specific initialization. */
   status = regFS->fsOps->FSS_GetObject(&oid->oid, objDesc);
   if (status != VMK_OK) {
      Warning("Failed with status %#x", status);
      FSSReleaseRegisteredFS(regFS);
      return status;
   }

   ASSERT(objDesc->objType != OBJ_INVALID);
   ASSERT(objDesc->objType == OBJ_VOLUME || objDesc->fs != NULL);

   /* Initialize FSS-specific fields. */
   if (objDesc->objType == OBJ_VOLUME) {
      Mod_IncUseCount(regFS->moduleID);  // DecUseCount() in FSS_ObjEvictCB()
      FSDESC(objDesc)->moduleID = regFS->moduleID;
      FSDESC(objDesc)->fsTypeNum = regFS->fsTypeNum;
   }

   objDesc->oid.fsTypeNum = regFS->fsTypeNum;

   FSSReleaseRegisteredFS(regFS);
   FSS_InitObjectDesc(objDesc);

   return VMK_OK;
}


/*
 * Called immediately before the specified descriptor is evicted from
 * the object cache.
 */
void
FSS_ObjEvictCB(ObjDescriptorInt *desc)
{
   VMK_ReturnStatus status;
   FSSRegisteredFS *regFS;

   ASSERT(desc->objType != OBJ_INVALID);

   /* Call implementation-defined eviction callback, if any. */
   if (desc->evictCB) {
      status = FSSGetRegisteredFS(desc->oid.fsTypeNum, &regFS);
      ASSERT(status == VMK_OK);

      if (status == VMK_OK) {
	 desc->evictCB(desc);
	 FSSReleaseRegisteredFS(regFS);
      } else {
	 Warning("FS driver unexpectedly went away.");
      }
   }

   if (desc->objType == OBJ_VOLUME) {
      FSDescriptorInt *fs = FSDESC(desc);
      Mod_DecUseCount(fs->moduleID);  // IncUseCount() in FSS_GetObject()
   }

   FSS_DestroyObjectDesc(desc);
}


void
FSS_ObjLastRefCB(ObjDescriptorInt *desc)
{
   VMK_ReturnStatus status;
   FSSRegisteredFS *regFS;

   ASSERT(desc->refCount == 0);
   ASSERT(desc->objType != OBJ_INVALID);

   /* Call implementation-defined last reference callback, if any. */
   if (desc->lastRefCB) {
      status = FSSGetRegisteredFS(desc->oid.fsTypeNum, &regFS);
      ASSERT(status == VMK_OK);

      if (status == VMK_OK) {
	 desc->lastRefCB(desc);
	 FSSReleaseRegisteredFS(regFS);
      } else {
	 Warning("FS driver unexpectedly went away.");
      }
   }

   switch (desc->objType)
      {
      case OBJ_VOLUME:
	 ASSERT(Semaphore_IsLocked(&fsLock));
	 
	 /*
	  * Since this is the last reference, at most one
	  * FS_OPEN_LOCKED request was outstanding.
	  */
	 ASSERT(FSDESC(desc)->lockedCount <= 1);

	 break;
      case OBJ_DIRECTORY:
      case OBJ_REGFILE:
	 ASSERT(Semaphore_IsLocked(&desc->OCDescLock));
	 break;
      default:
         ASSERT(0);
	 Warning("OBJ_INVALID encountered.");
      }
}


static VMK_ReturnStatus
FSSGetRegisteredFS(uint16 fsTypeNum, struct FSSRegisteredFS **regFS)
{
   VMK_ReturnStatus status = VMK_NOT_FOUND;
   FSSRegisteredFS *rfs;

   SP_Lock(&regFSLock);
   rfs = fssRegisteredFSList;
   while (rfs) {
      if (rfs->fsTypeNum == fsTypeNum) {
	 *regFS = rfs;
         Mod_IncUseCount(rfs->moduleID);
	 status = VMK_OK;
	 break;
      }
      rfs = rfs->next;
   }
   SP_Unlock(&regFSLock);
   return status;
}

static void
FSSReleaseRegisteredFS(const struct FSSRegisteredFS *regFS)
{
   Mod_DecUseCount(regFS->moduleID);
}

/*
 *-----------------------------------------------------------------------------
 *
 * FSS_Lookup --
 *    Given 'parentOID', the OID of a directory and 'childName', the name of an
 *    object within that directory, look up the object's OID and store it
 *    in 'childOID'. 'childOID' should point to a sufficiently sized buffer.
 *
 * Results:
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
FSS_Lookup(FSS_ObjectID *parentOID, const char *childName,
	   FSS_ObjectID *childOID)
{
   VMK_ReturnStatus status;
   ObjDescriptorInt *parentDesc;
   FSDescriptorInt  *parentFS;
   FileDescriptorInt *dd;

   LOG(2, FS_OID_FMTSTR", %s", FS_OID_VAARGS(parentOID), childName);

   if (FSS_IsVMFSRootOID(parentOID)) {
      /* Lookup on FSS root (/vmfs on COS). */
      return VC_Lookup(childName, childOID);
   }

   status = OC_ReserveObject(parentOID, &parentDesc);
   if (status != VMK_OK) {
      return status;
   }

   if (parentDesc->objType != OBJ_DIRECTORY) {
      Warning(FS_OID_FMTSTR" not a directory", FS_OID_VAARGS(parentOID));
      return VMK_NOT_A_DIRECTORY;
   }

   FSS_InitOID(childOID);

   /* Look up 'childName' in parent's name cache */
   dd = FILEDESC(parentDesc);
   SP_Lock(&dd->nameCacheLock);
   if (FSN_ObjNameCacheLookup(parentDesc, childName, childOID) == VMK_OK) {
      SP_Unlock(&dd->nameCacheLock);
      OC_ReleaseObject(parentDesc);
      return VMK_OK;
   }
   SP_Unlock(&dd->nameCacheLock);

   /*
    * Not found -- look up from underlying file system. We currently
    * assume that parent & child are on the same type of file system,
    * thus disallowing hard links across differing FS types.
    */
   parentFS = FSDESC(parentDesc->fs);

   status = parentFS->fsOps->FSS_Lookup(parentDesc, childName, &childOID->oid);
   if (status != VMK_OK) {
      OC_ReleaseObject(parentDesc);
      return status;
   }

   childOID->fsTypeNum = parentFS->fsTypeNum;

   OC_ReleaseObject(parentDesc);

   LOG(2, "returns "FS_OID_FMTSTR, FS_OID_VAARGS(childOID));
   return VMK_OK;
}


/*
 *-----------------------------------------------------------------------------
 *
 * FSS_LookupFileHandle --
 *    Given file handle ID 'fileHandleID', returns the OID of the
 *    opened object. 'oid' should point to a buffer of
 *    sizeof(FSS_ObjectID) bytes.
 *
 * Results:
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
FSS_LookupFileHandle(FS_FileHandleID fileHandleID,
		     FSS_ObjectID *oid)  // OUT
{
   FSFileHandle *fh = FSS_GET_FH_PTR(fileHandleID);

   if (FSS_BAD_FHI(fileHandleID, fh)) {
      return VMK_INVALID_HANDLE;
   }

   FSS_CopyOID(oid, &fh->fileDesc->oid);

   return VMK_OK;
}


/*
 *-----------------------------------------------------------------------------
 *
 * FSS_Create --
 *    Create a file system of type fsType. fsType should match the name of
 *    a registered FS, eg., "vmfs1" or "vmfs2".
 *
 * Results:
 *    VMK_OK on success.
 *
 * Side effects:
 *    Rescans SCSI partitions to 'discover' and report any new VMFS'es created.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
FSS_Create(const char *fsType, const char *deviceName,
           uint32 fileBlockSize, uint32 numFiles)
{
   VMK_ReturnStatus status = VMK_BAD_PARAM;
   FSSRegisteredFS *driver;

   /*
    * Acquire fslock to prevent a create operation from clashing with FS
    * rescan or FS open. Also to serialize access to fssRegisteredFSTable.
    */
   Semaphore_Lock(&fsLock);
   SP_Lock(&regFSLock);
   driver = fssRegisteredFSList;
   while (driver != NULL) {
      if (strncmp(driver->fsType, fsType, sizeof(driver->fsType)) == 0) {
         Mod_IncUseCount(driver->moduleID);
         SP_Unlock(&regFSLock);
         status = driver->fsOps->FSS_Create(deviceName, fileBlockSize,
                                            numFiles);
         Mod_DecUseCount(driver->moduleID);
         break;
      }
      driver = driver->next;
   }
   if (driver == NULL) {
      SP_Unlock(&regFSLock);
   }
   Semaphore_Unlock(&fsLock);

   if (status == VMK_OK) {
      VC_RescanVolumes(NULL, NULL);
   }
   return status;
}

/*
 *-----------------------------------------------------------------------------
 *
 * FSS_Extend --
 *    Extend a VMFS volume named by adding another physical extent at
 *    extVolumeName.
 *
 * Results:
 *
 * Side effects:
 *    The extPartition may be formatted.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus FSS_Extend(const char *volumeName,
			    const char *extDeviceName,
                            uint32 numFiles)
{
   VMK_ReturnStatus status;
   ObjDescriptorInt *volDesc;
   FSDescriptorInt  *fs;

   status = FSSOpenVolume(volumeName, FS_OPEN_LOCKED, &volDesc);
   if (status != VMK_OK) {
      return status;
   }

   fs = FSDESC(volDesc);

   status = fs->fsOps->FSS_Extend(volDesc, extDeviceName, numFiles);

   FSSCloseVolume(volDesc, FS_OPEN_LOCKED);

   if (status == VMK_OK) {
      VC_RescanVolumes(NULL, NULL);
   }

   return status;
}


VMK_ReturnStatus
FSS_Probe(const char *volumeName, Bool rescanInProgress)
{
   VMK_ReturnStatus status;
   ObjDescriptorInt *fsObj;
   uint32 openFlags = FS_OPEN_LOCKED;

   if (rescanInProgress) {
      openFlags |= FS_OPEN_RESCAN;
   }

   status = FSSOpenVolume(volumeName, openFlags, &fsObj);
   if (status != VMK_OK) {
      return status;
   }

   FSSCloseVolume(fsObj, openFlags);

   return VMK_OK;
}


/*
 *-----------------------------------------------------------------------------
 *
 * FSS_GetAttributes --
 *    Given the OID of an object within a volume, return the volume's
 *    attributes. Additionally return a list of its partitions, if
 *    provided by the file system implementation. No more than
 *    'maxPartitions' will be included in the list.
 *
 * Results:
 *
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
FSS_GetAttributes(FSS_ObjectID *oid, uint32 maxPartitions,
		  VMnix_PartitionListResult *result)
{
   VMK_ReturnStatus status;
   FSSRegisteredFS *regFS;
   FSS_ObjectID volOID;
   ObjDescriptorInt *fsObj;

   status = FSSGetRegisteredFS(oid->fsTypeNum, &regFS);
   if (status != VMK_OK) {
      return status;
   }

   status = regFS->fsOps->FSS_GetVolumeOID(&oid->oid, &volOID.oid);
   if (status != VMK_OK) {
      goto done;
   }
   volOID.fsTypeNum = oid->fsTypeNum;

   status = OC_ReserveVolume(&volOID, &fsObj);
   if (status != VMK_OK) {
      goto done;
   }
   
   status = regFS->fsOps->FSS_GetAttributes(fsObj, maxPartitions, result);

   OC_ReleaseVolume(fsObj);

 done:
   FSSReleaseRegisteredFS(regFS);
   return status;
}

#ifdef ISSPACE
#error "Need to rename the ISSPACE macro"
#else
#define ISSPACE(c) ((c) == ' ' || (c) == '\f' || (c) == '\n' || \
                    (c) == '\r' || (c) == '\t' || (c) == '\v')
#endif

/*
 *-----------------------------------------------------------------------------
 *
 * FSS_SetAttributes --
 *    Set the attributes of a volume. Currently, the only supported
 *    operations are setting the name and mode.
 *
 * Results:
 *
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
FSS_SetAttributes(const char *volumeName, uint16 opFlags,
                  const char *fsName, int mode)
{
   VMK_ReturnStatus status = VMK_OK;
   ObjDescriptorInt *volDesc;
   FSDescriptorInt  *fs;

   if (opFlags & FSATTR_SET_NAME) {
      VC_VMFSVolume *pt;

      if (fsName[FS_MAX_FS_NAME_LENGTH - 1] != 0) {
         return VMK_NAME_TOO_LONG;
      }
      if (strchr(fsName, ':')) {
         return VMK_BAD_PARAM;
      }
      if (strlen(fsName) > 0 &&
          (ISSPACE(fsName[0]) ||
           ISSPACE(fsName[strlen(fsName) - 1]))) {
         return VMK_BAD_PARAM;
      }
      if (fsName[0] != '\0') {
         pt = VC_FindVMFSVolume(fsName, FALSE);
         VC_ReleaseVMFSVolume(pt);
         if (pt != NULL) {
            /* Don't allow setting name to an existing name. */
            return VMK_EXISTS;
         }
      }

      /* OK to set name. */

      status = FSSOpenVolume(volumeName, FS_OPEN_LOCKED, &volDesc);
      if (status != VMK_OK) {
         return status;
      }

      fs = FSDESC(volDesc);

      status = fs->fsOps->FSS_SetAttribute(volDesc, FSATTR_SET_NAME,
                                           fsName, mode);
      if (status == VMK_OK) {
         /* 
          * Update name in list of named partitions. Use fs->volumeName as
          * the lookup key because this is the canonical FS device name.
	  */
         VC_SetName(fs->volumeName, fsName);
      }

      FSSCloseVolume(volDesc, FS_OPEN_LOCKED);

   } else if (opFlags & FSATTR_SET_MODE) {
      uint32 openFlag = (mode == FS_MODE_RECOVER) ? FS_OPEN_FORCE
	 : FS_OPEN_LOCKED;

      status = FSSOpenVolume(volumeName, openFlag, &volDesc);
      if (status != VMK_OK) {
         return status;
      }

      fs = FSDESC(volDesc);

      Semaphore_Lock(&volDesc->descLock);
      status = fs->fsOps->FSS_SetAttribute(volDesc, FSATTR_SET_MODE, fsName, mode);
      Semaphore_Unlock(&volDesc->descLock);

      FSSCloseVolume(volDesc, openFlag);
   }

   return status;
}


/*
 *-----------------------------------------------------------------------------
 *
 * FSS_UpgradeVolume -- (!!! This should be a fsctl)
 *    Upgrade the given FS volume.
 * 
 * Results:
 *    VMK_OK on success, VMK error code otherwise.
 *
 * Side effects:
 *    Disk metadata may be modified.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
FSS_UpgradeVolume(const char *volumeName)
{
   VMK_ReturnStatus status;
   ObjDescriptorInt *volDesc;
   FSDescriptorInt  *fs;

   status = FSSOpenVolume(volumeName, FS_OPEN_LOCKED, &volDesc);
   if (status != VMK_OK) {
      return status;
   }

   fs = FSDESC(volDesc);

   status = fs->fsOps->FSS_UpgradeVolume(volDesc);

   FSSCloseVolume(volDesc, FS_OPEN_LOCKED);

   if (status == VMK_OK) {
      VC_RescanVolumes(NULL, NULL);
      
      /*
       * Open and close the new FS volume, so that the FS checker can
       * run and resolve inconsistencies (if any). This'll
       * conclude the FS conversion process.
       */
      status = FSSOpenVolume(volumeName, FS_OPEN_LOCKED, &volDesc);
      if (status == VMK_OK) {
         FSSCloseVolume(volDesc, FS_OPEN_LOCKED);
      }
   }

   return status;
}


/*
 * Given an OID, produces a human-readable representation by calling
 * the relevant implementation-defined handler. Stores the produced
 * string in 'outString'. 'outString' should point to a buffer of
 * FSS_OID_STRING_SIZE bytes. The output string will be
 * null-terminated.
 */
VMK_ReturnStatus
FSS_OIDtoString(const FSS_ObjectID *oid,
		char *outString)  // OUT
{
   VMK_ReturnStatus status;
   FSSRegisteredFS *fs;

   status = FSSGetRegisteredFS(oid->fsTypeNum, &fs);
   if (status != VMK_OK) {
      return status;
   }

   fs->fsOps->FSS_OIDtoString(&oid->oid, outString);
   FSSReleaseRegisteredFS(fs);

   outString[FSS_OID_STRING_SIZE - 1] = '\0';
   return VMK_OK;
}



VMK_ReturnStatus
FSS_FileGetPhysLayout(FS_FileHandleID fileHandleID, uint64 offset,
		      VMnix_FileGetPhysLayoutResult *result)
{
   FSFileHandle *fh = FSS_GET_FH_PTR(fileHandleID);
   //   FSS_FileOps *fileOps = FILEDESC(fh->fileDesc)->fileOps;

   VMK_ReturnStatus status;

   if (FSS_BAD_FHI(fileHandleID, fh)) {
      return VMK_INVALID_HANDLE;
   }
   if (NULL != FSS_FH_FILEOPS(fh)->FSS_GetLayoutCommand) {
      status = FSS_FH_FILEOPS(fh)->FSS_GetLayoutCommand(fh->fileDesc, offset, 
							result);
   } else {
      status = VMK_NOT_IMPLEMENTED;
   }
   return status;
}

/*
 *-----------------------------------------------------------------------------
 *
 * FSS_Readdir --
 *    If maxFiles > 0, return a list of files in the specified directory.
 *    Return up to maxFiles and set result->numFiles to indicate the actual
 *    number of files on the volume. 'result' should point to a buffer at
 *    least VMNIX_FSLIST_RESULT_SIZE(maxFiles) bytes long.
 *
 * Results:
 *    On success, 'result' contains the list of files on the FS volume,
 *    along with some FS metadata.
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
FSS_Readdir(FSS_ObjectID *dirOID,
            uint32 maxFiles,
            VMnix_ReaddirResult *result)
{
   VMK_ReturnStatus status;
   ObjDescriptorInt *dirDesc;

   if (maxFiles == 0) {
      return VMK_BAD_PARAM;
   }

   if (FSS_IsVMFSRootOID(dirOID)) {
      return VC_Readdir(maxFiles, result);
   }

   status = OC_ReserveObject(dirOID, &dirDesc);
   if (status != VMK_OK) {
      return status;
   }

   status = FSS_FD2FILEOPS(dirDesc)->FSS_Readdir(dirDesc, maxFiles, result);

   OC_ReleaseObject(dirDesc);
   return status;
}


/*
 *-----------------------------------------------------------------------------
 *
 * FSS_Dump --
 *    Dump object metadata onto serial line. What exactly is dumped is
 *    left to FS implementations. 'oid' must not be the OID of a volume.
 *
 * Results:
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
FSS_Dump(FSS_ObjectID *oid, Bool verbose)
{
   VMK_ReturnStatus status;
   ObjDescriptorInt *desc;

   status = OC_ReserveObject(oid, &desc);
   if (status != VMK_OK) {
      return status;
   }   

   status = FILEDESC(desc)->fileOps->FSS_Dump(desc, verbose);

   OC_ReleaseObject(desc);
   return status;
}


/*
 * Create a file. If 'fileOID' is non-NULL and the file was
 * successfully created, copies its OID there.
 */
VMK_ReturnStatus
FSS_CreateFile(FSS_ObjectID *parentOID, const char *fileName,
               uint32 createFlags,  void *dataIn,
	       FSS_ObjectID *fileOID)
{
   VMK_ReturnStatus status;
   uint32           openFlags;
   ObjDescriptorInt *dirDesc;
   FSDescriptorInt  *parentFS;
   FS_DescriptorFlags descFlags = 0;
   FS_ObjectID      *fsOID = NULL;

   if (fileOID) {
      fsOID = &fileOID->oid;
   }

   /*
    * If a file is created, it should be opened for write by default, 'coz
    * we expect the caller to do a FSS_SetFileAttributes on the file
    * shortly. Exception: When creating a COW file, it should be opened for
    * exclusive access by defaul, because the VM may start using it as soon
    * as it is created.
    */
   openFlags = FILEOPEN_WRITE;
   if (!(createFlags & FS_CREATE_CAN_EXIST)) {
      openFlags |= FILEOPEN_CANT_EXIST;
   }

   if (createFlags & FS_CREATE_DIR) {
      descFlags = FS_DIRECTORY | FS_NO_LAZYZERO | FS_NOT_ESX_DISK_IMAGE;
   } else if (createFlags & FS_CREATE_RAWDISK_MAPPING) {
      descFlags = FS_RAWDISK_MAPPING | FS_NO_LAZYZERO;
   } else if (createFlags & FS_CREATE_SWAP) {
      descFlags |= FS_SWAP_FILE | FS_NO_LAZYZERO | FS_NOT_ESX_DISK_IMAGE;
   } else {
      descFlags |= FS_NOT_ESX_DISK_IMAGE;
   }

   status = OC_ReserveObject(parentOID, &dirDesc);
   if (status != VMK_OK) {
      return status;
   }

   parentFS = FSDESC(dirDesc->fs);
   if (parentFS->readOnly) {
      status = VMK_READ_ONLY;
      goto done;
   }

   /* Create the file. */
   status = FSSCreateFile(dirDesc, fileName,
			  openFlags, descFlags, dataIn,
			  fsOID);
   if (status != VMK_OK) {
      goto done;
   }

   fileOID->fsTypeNum = parentFS->fsTypeNum;

 done:   
   OC_ReleaseObject(dirDesc);
   return status;
}


/*
 *-----------------------------------------------------------------------------
 *
 * FSS_OpenFile --
 *    Open the specified file and return a file handle on success.
 *
 * Results:
 *    File handle is returned in 'fileHandle' on success.
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus	
FSS_OpenFile(FSS_ObjectID *fileOID,
	     uint32 openFlags,
	     FS_FileHandleID *fileHandleID)
{
   VMK_ReturnStatus status;
   ObjDescriptorInt *fileDesc;

   LOG(2, FS_OID_FMTSTR, FS_OID_VAARGS(fileOID));

   /* Reservation released in FSS_CloseFile. */
   status = OC_ReserveObject(fileOID, &fileDesc);
   if (status != VMK_OK) {
      return status;
   }

   status = FSSOpenFile(fileDesc, openFlags, NULL,
			TRUE, fileHandleID);
   if (status != VMK_OK) {
      LOG(1, "Failed. Status = %#x", status);
      OC_ReleaseObject(fileDesc);
   } else {
      LOG(1, "Succeeded on "FS_OID_FMTSTR". fileHandleID = %"FMT64"d",
          FS_OID_VAARGS(fileOID), *fileHandleID);
   }

   return status;
}


/*
 *-----------------------------------------------------------------------------
 *
 * FSS_CloseFile --
 *    Decrement the refCount on the file. If refCount falls to zero, 
 *    remove it from the list of open files and the renewList. Call the
 *    implementation specific closefile routine to unlock the file on disk,
 *    flush out the bitmaps and close the file. Remove it if the descriptor is
 *    marked removed and refcount falls to zero.
 *
 * Results:
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
FSS_CloseFile(FS_FileHandleID fileHandleID)
{
   FSFileHandle      *fh = FSS_GET_FH_PTR(fileHandleID);
   ObjDescriptorInt  *fileDesc;
   FileDescriptorInt *fdInt;
   uint32 openFlags;

   /*
    * Grab 'handleLock' to prevent race between concurrent
    * FSS_CloseFile()s on the same 'fileHandleID'.
    */
   SP_Lock(&handleLock);
   if (FSS_BAD_FHI(fileHandleID, fh)) {
      SP_Unlock(&handleLock);
      return VMK_INVALID_HANDLE;
   }

   fileDesc = fh->fileDesc;
   fdInt = FILEDESC(fileDesc);

   /* Release file handle. */
   openFlags = fh->openFlags;
   fh->fileDesc = NULL;
   fh->inUse = FALSE;
   SP_Unlock(&handleLock);

   ASSERT(openFlags & (FILEOPEN_READ | FILEOPEN_READONLY |
                       FILEOPEN_WRITE | FILEOPEN_EXCLUSIVE));
   ASSERT(!((openFlags & FILEOPEN_READ) &&
            (openFlags & FILEOPEN_READONLY)));
   ASSERT(!((openFlags & FILEOPEN_EXCLUSIVE) &&
            (openFlags & 
             (FILEOPEN_READONLY | FILEOPEN_READ | FILEOPEN_WRITE))));

   LOG(1, FS_OID_FMTSTR" (%"FMT64"d) flags %s, %s, %s, %s", 
       FS_OID_VAARGS(&fileDesc->oid), fileHandleID,
       (openFlags & FILEOPEN_EXCLUSIVE) ? "EX" : "0",
       (openFlags & FILEOPEN_READ) ? "RD" : "0",
       (openFlags & FILEOPEN_READONLY) ? "RO" : "0",
       (openFlags & FILEOPEN_WRITE) ? "WR" : "0");

   /*
    * Try forcing the underlying FS implementation to zero out any
    * trailing portion of an uninitialized block.
    * XXX: move down to implementation?
    */
   FSSZeroOutBlockTail(fileDesc);

   Semaphore_Lock(&fileDesc->descLock);

   ASSERT(fileDesc->refCount > 0);
   ASSERT(fdInt->openCount > 0);

   /* Restore pre-open open mode. */
   if (openFlags & FILEOPEN_READ) {
      fdInt->readerCount--;
      if (fdInt->readerCount == 0) {
         fdInt->openFlags &= ~FILEOPEN_READ;
      }
   } else if (openFlags & FILEOPEN_READONLY) {
      fdInt->sharedReaderCount--;
      if (fdInt->sharedReaderCount == 0) {
         fdInt->openFlags &= ~FILEOPEN_READONLY;
      }
   }
   if (openFlags & FILEOPEN_WRITE) {
      fdInt->writerCount--;
      if (fdInt->writerCount == 0) {
         fdInt->openFlags &= ~FILEOPEN_WRITE;
      }
   }
   if (openFlags & FILEOPEN_EXCLUSIVE) {
      fdInt->openFlags &= ~FILEOPEN_EXCLUSIVE;
   }

   fdInt->openCount--;

   /*
    * Last handle to file -- call implementation close handler. Object
    * descriptor remains in memory until it's evicted (object
    * 'refCount' may be non-zero).
    */
   if (fdInt->openCount == 0) {
      FSS_FD2FILEOPS(fileDesc)->FSS_CloseFile(fileDesc);
   }

   Semaphore_Unlock(&fileDesc->descLock);

   /* Release the reservation obtained in FSS_OpenFile(). */
   OC_ReleaseObject(fileDesc);
   
   return VMK_OK;
}


VMK_ReturnStatus
FSS_RemoveFile(FSS_ObjectID *parentOID, const char *fileName)
{
   VMK_ReturnStatus status;
   ObjDescriptorInt *parentDesc;
   FSDescriptorInt  *fs;
   FileDescriptorInt *dir;

   LOG(2, "parent "FS_OID_FMTSTR" file \"%s\"",
       FS_OID_VAARGS(parentOID), fileName);

   status = OC_ReserveObject(parentOID, &parentDesc);
   if (status != VMK_OK) {
      return status;
   }

   fs = FSDESC(parentDesc->fs);
   dir = FILEDESC(parentDesc);

   /* Cannot remove if volume is read-only. */
   if (fs->readOnly == TRUE) {
      status = VMK_READ_ONLY;
      goto done;
   }

   /* Call implementation remove handler. */
   status = dir->fileOps->FSS_RemoveFile(parentDesc, fileName);

 done:
   OC_ReleaseObject(parentDesc);
   return status;
}


/*
 *-----------------------------------------------------------------------------
 *
 * FSS_RenameFile --
 *    Rename a file.
 *
 * Results:
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
FSS_RenameFile(FSS_ObjectID *srcDirOID, const char *srcName,
	       FSS_ObjectID *dstDirOID, const char *dstName)
{
   VMK_ReturnStatus status;
   ObjDescriptorInt *srcDirDesc, *dstDirDesc;
   FileDescriptorInt *fd;

   status = OC_ReserveObject(srcDirOID, &srcDirDesc);
   if (status != VMK_OK) {
      return status;
   }

   status = OC_ReserveObject(dstDirOID, &dstDirDesc);
   if (status != VMK_OK) {
      OC_ReleaseObject(srcDirDesc);
      return status;
   }

   fd = FILEDESC(srcDirDesc);

   /*
    * Call implementation rename handler. No synchronization is done
    * by the FSS. It is left to the implementation to enforce as
    * necessary.
    */
   status = fd->fileOps->FSS_RenameFile(srcDirDesc, srcName,
					dstDirDesc, dstName);

   OC_ReleaseObject(dstDirDesc);
   OC_ReleaseObject(srcDirDesc);

   return status;
}


/*
 *-----------------------------------------------------------------------------
 *
 * FSS_ChangeMode --
 *    Change the open mode of an open file to exclusive (one write) or
 *    to readonly (multiple readers).  If going from exclusive to shared,
 *    flush the file first, since writes will not be allowed any longer.
 *    Assumes that there are write operations on the file have already
 *    been stopped.  Doesn't allow change from shared to exclusive if
 *    there is more than one handle open to the file.
 *
 *    NOTE: This function is idempotent w.r.t. 'exclusive'.
 *
 * Results:
 *
 * Side effects:
 *    In-memory changes to file metadata may be flushed out to disk.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
FSS_ChangeMode(FS_FileHandleID fileHandleID, Bool exclusive)
{
   VMK_ReturnStatus  status = VMK_OK;
   FSFileHandle      *fh = FSS_GET_FH_PTR(fileHandleID);
   ObjDescriptorInt  *fileDesc;
   FileDescriptorInt *fdInt;

   if (FSS_BAD_FHI(fileHandleID, fh)) {
      return VMK_INVALID_HANDLE;
   }

   fileDesc = fh->fileDesc;
   fdInt = FILEDESC(fileDesc);

   if (!exclusive) {
      status = FSS_FH_FILEOPS(fh)->FSS_FlushFile(fileDesc);
      if (status != VMK_OK) {
         return status;
      }
   }

   Semaphore_Lock(&fileDesc->descLock);

   if (exclusive) {
      if (!(fh->openFlags & FILEOPEN_EXCLUSIVE)) {
         if (fdInt->openCount == 1) {
            ASSERT(fdInt->readerCount == 0);
            ASSERT(fdInt->sharedReaderCount == 1);
            ASSERT(fdInt->writerCount == 0);
            ASSERT(fdInt->openFlags & FILEOPEN_READONLY);
            ASSERT(fh->openFlags & FILEOPEN_READONLY);
            
            //XXXXXXXXXX
            //When switching from RO to RX, VMFS-2.11+ needs to switch from
            //on-disk RO to on-disk EX. Currently, it is ok for add_redo.pl
            //on persistent disks.
            LOG(1, "Switching "FS_OID_FMTSTR" from RO to EX",
                FS_OID_VAARGS(&fileDesc->oid));
            
            fh->openFlags &= ~FILEOPEN_READONLY;
            fh->openFlags |= FILEOPEN_EXCLUSIVE;        
            
            fdInt->openFlags &= ~FILEOPEN_READONLY;
            fdInt->openFlags |= FILEOPEN_EXCLUSIVE;
            fdInt->sharedReaderCount--;
         } else {
            status = VMK_BUSY;
         }
      }
   } else if (!(fh->openFlags & FILEOPEN_READONLY)) {
      ASSERT(fdInt->openCount == 1);
      ASSERT(fdInt->readerCount == 0);
      ASSERT(fdInt->sharedReaderCount == 0);
      ASSERT(fdInt->writerCount == 0);
      ASSERT(fdInt->openFlags & FILEOPEN_EXCLUSIVE);
      ASSERT(fh->openFlags & FILEOPEN_EXCLUSIVE);

      LOG(1, "Switching "FS_OID_FMTSTR" from EX to RO", 
          FS_OID_VAARGS(&fileDesc->oid));

      fh->openFlags &= ~FILEOPEN_EXCLUSIVE;
      fh->openFlags |= FILEOPEN_READONLY;

      fdInt->openFlags &= ~FILEOPEN_EXCLUSIVE;
      fdInt->openFlags |= FILEOPEN_READONLY;
      fdInt->sharedReaderCount++;
   }

   Semaphore_Unlock(&fileDesc->descLock);

   return status;
}

/*
 *-----------------------------------------------------------------------------
 *
 * FSS_BufferCacheIO --
 *    Read/write to/from COS /vmfs buffer cache from/to a file.
 *    Data must refer to a buffer valid in 'addrType' address space.
 *    Please see FSS_FileIO documentation for details on 'ioFlags'.
 *    
 *    The read/write is done synchronously and the actual amount of data
 *    transferred is returned in 'bytesTransferred'.
 *
 * Results:
 *      .
 *
 *
 * Side effects:
 *
 *
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus 
FSS_BufferCacheIO(FSS_ObjectID *fileOID, uint64 offset, uint64 data,
                  uint32 length, IO_Flags ioFlags, SG_AddrType addrType,
                  uint32 *bytesTransferred)
{
   VMK_ReturnStatus status;
   ObjDescriptorInt *fileDesc;
   SG_Array sgArr;
   
   status = OC_ReserveObject(fileOID, &fileDesc);
   if (status != VMK_OK) {
      return status;
   }

   FSSSingletonSGArray(&sgArr, offset, data, length, addrType);
   status = FSS_FD2FILEOPS(fileDesc)->FSS_FileIO(fileDesc, &sgArr, 
                                                 NULL, ioFlags,
                                                 bytesTransferred);

   LOG(2, "%llu %u, returns %#x, %u",
       offset, length, status, *bytesTransferred);

   OC_ReleaseObject(fileDesc);
   return status;
}

/*
 *-----------------------------------------------------------------------------
 *
 * FSS_BufferIO --
 *    Read/write from/to a file.  Data must refer to a buffer valid in
 *    'addrType' address space. Please see FSS_FileIO documentation for
 *    details on 'ioFlags'.
 *    
 *    The read/write is done synchronously and the actual amount of data
 *    transferred is returned in 'bytesTransferred'.
 *
 * Results:
 *    VMK_OK on success.
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus 
FSS_BufferIO(FS_FileHandleID fileHandleID, uint64 offset,
             uint64 data, uint32 length, IO_Flags ioFlags,
             SG_AddrType addrType, uint32 *bytesTransferred)
{
   SG_Array sgArr;

   FSSSingletonSGArray(&sgArr, offset, data, length, addrType);
   return FSSFileIO(fileHandleID, &sgArr, NULL, ioFlags, bytesTransferred);
}


/*
 *-----------------------------------------------------------------------------
 *
 * FSS_SGFileIO --
 *    Do synchronous scatter-gather IO on a file. The amount of data
 *    transferred is returned in bytesTransferred.
 *
 * Results:
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
FSS_SGFileIO(FS_FileHandleID fileHandleID, SG_Array *sgArr,
             IO_Flags ioFlags, uint32 *bytesTransferred)
{
   return FSSFileIO(fileHandleID, sgArr, NULL, ioFlags, bytesTransferred);
}


/*
 *-----------------------------------------------------------------------------
 *
 * FSS_AsyncFileIO --
 *    Do asynchronous scatter-gather IO on a file.
 *
 * Results:
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
FSS_AsyncFileIO(FS_FileHandleID fileHandleID, SG_Array *sgArr,
                Async_Token *token, IO_Flags ioFlags)
{
   uint32 bytesTransferred;
   
   return FSSFileIO(fileHandleID, sgArr, token, ioFlags, &bytesTransferred);
}


/*
 *-----------------------------------------------------------------------------
 *
 * FSS_GetFileAttributes --
 *    Read file attributes and return them in 'attrs'.
 *
 * Results:
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus	
FSS_GetFileAttributes(FSS_ObjectID *fileOID, FS_FileAttributes *attrs)
{
   VMK_ReturnStatus  status;
   ObjDescriptorInt  *file;
   FileDescriptorInt *fd;

   LOG(2, FS_OID_FMTSTR, FS_OID_VAARGS(fileOID));

   if (FSS_IsVMFSRootOID(fileOID)) {
      return VC_GetFileAttributes(attrs);
   }

   status = OC_ReserveObject(fileOID, &file);
   if (status != VMK_OK) {
      return status;
   }

   fd = FILEDESC(file);

   status = fd->fileOps->FSS_GetFileAttributes(file, attrs);

   OC_ReleaseObject(file);
   return status;
}


/*
 *-----------------------------------------------------------------------------
 *
 * FSS_SetFileAttributes --
 *    Set the attributes of a file.
 *
 * Results:
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
FSS_SetFileAttributes(FSS_ObjectID *fileOID,
                      uint16 opFlags,
                      const FS_FileAttributes *attrs)
{
   VMK_ReturnStatus  status;
   ObjDescriptorInt  *file;
   FileDescriptorInt *fd;

   LOG(2, FS_OID_FMTSTR, FS_OID_VAARGS(fileOID));

   /*
    * Only COW code is allowed to call FSS_RawSetFileAttributes() with
    * this flag set (and hence holding shared reader ioAccess).
    */
   ASSERT((opFlags & FILEATTR_UPGRADEABLE_LOCK) == 0);

   status = OC_ReserveObject(fileOID, &file);
   if (status != VMK_OK) {
      return status;
   }

   fd = FILEDESC(file);

#if 0  // file need not be opened
   if (!(fd->openFlags & (FILEOPEN_WRITE | FILEOPEN_EXCLUSIVE))) {
      OC_ReleaseObject(file);
      return VMK_WRITE_ERROR;
   }
#endif

   status = fd->fileOps->FSS_SetFileAttributes(file, opFlags, attrs);

   OC_ReleaseObject(file);
   return status;
}


/*
 *-----------------------------------------------------------------------------
 *
 * FSS_ReserveFile --
 *    ESX clustering. Reserve a file for a given physical ESX server and a
 *    given VM (worldID) running on the server.
 *
 *    ??? Does reserve/release work on ALL files?
 *
 * Results:
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
FSS_ReserveFile(FS_FileHandleID fileHandleID,
                World_ID worldID, Bool testOnly)
{
   FSFileHandle *fh = FSS_GET_FH_PTR(fileHandleID);
   
   if (FSS_BAD_FHI(fileHandleID, fh)) {
      return VMK_INVALID_HANDLE;
   }

   return FSS_FH_FILEOPS(fh)->FSS_ReserveFile(fh->fileDesc, worldID, testOnly);
}

/*
 *-----------------------------------------------------------------------------
 *
 * FSS_ReleaseFile --
 *    ESX clustering. Complementary to FSS_ReserveFile().
 *
 * Results:
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
FSS_ReleaseFile(FS_FileHandleID fileHandleID,
                World_ID worldID, Bool reset)
{
   FSFileHandle *fh = FSS_GET_FH_PTR(fileHandleID);
   
   if (FSS_BAD_FHI(fileHandleID, fh)) {
      return VMK_INVALID_HANDLE;
   }

   return FSS_FH_FILEOPS(fh)->FSS_ReleaseFile(fh->fileDesc, worldID, reset);
}


/*
 *-----------------------------------------------------------------------------
 *
 * FSS_IsMultiWriter --
 *    Return TRUE is a VMFS file can be written to by multiple
 *    processes/threads, FALSE otherwise.
 *
 * Results:
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
Bool
FSS_IsMultiWriter(FS_FileHandleID fileHandleID)
{
   FSFileHandle      *fh = FSS_GET_FH_PTR(fileHandleID);
   FileDescriptorInt *fdInt;

   if (FSS_BAD_FHI(fileHandleID, fh)) {
      Log("Invalid handle %"FMT64"d, expected %"FMT64"d",
	  fileHandleID, fh->handleID);
      return FALSE;
   }
   
   fdInt = FILEDESC(fh->fileDesc);
   
   return (fdInt->openFlags & FILEOPEN_WRITE) ? TRUE : FALSE;
}


/*
 * Get the information on the raw disk specified by a VMFS raw disk mapping.
 */
VMK_ReturnStatus
FSS_QueryRawDisk(const VMnix_QueryRawDiskArgs *args,
		 VMnix_QueryRawDiskResult *result)
{
   return VMK_NOT_IMPLEMENTED;
#if 0
   VMK_ReturnStatus status;
   FS_HandleID fsHandleID;
   FS_FileHandleID fileHandleID;
   FSHandleInfo *hi;

   status = FSS_Open(args->diskName, args->targetID, args->lun,
		    args->partition, FS_OPEN_LOCKED, &fsHandleID);
   if (status != VMK_OK) {
      return status;
   }
   hi = FSS_GET_HI_PTR(fsHandleID);
   status = FSSDoFileOp(hi->fs->rootDir, args->fileName,
                         FILEOPEN_QUERY_RAWDISK | FILEOPEN_READ, 0,
                         (void *)result, &fileHandleID);
   if (status == VMK_OK) {
      FSS_CloseFile(fileHandleID);
      status = FSS_Close(fsHandleID);
   } else {
      FSS_Close(fsHandleID);
   }
   return status;
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * FSS_AbortCommand -- (!!! Will become a fsctl)
 *    Abort the given command.
 * 
 * Results:
 *    Result returned by relevant filesystem handler.
 *
 * Side effects:
 *    None.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
FSS_AbortCommand(FS_FileHandleID fileHandleID,
		 SCSI_Command *cmd)
{
   FSFileHandle *fh = FSS_GET_FH_PTR(fileHandleID);
   VMK_ReturnStatus result;

   if (FSS_BAD_FHI(fileHandleID, fh)) {
      return VMK_INVALID_HANDLE;
   }

   result = FSS_FH_FILEOPS(fh)->FSS_AbortCommand(fh->fileDesc, cmd);

   return result;
}

/*
 *-----------------------------------------------------------------------------
 *
 * FSS_ResetCommand -- (!!! Will become a fsctl)
 *    Reset the device(s) that the file corresponds to.
 * 
 * Results:
 *    Result returned by relevant filesystem handler.
 *
 * Side effects:
 *    None.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
FSS_ResetCommand(FS_FileHandleID fileHandleID,
		 SCSI_Command *cmd)
{
   FSFileHandle *fh = FSS_GET_FH_PTR(fileHandleID);
   VMK_ReturnStatus result;

   if (FSS_BAD_FHI(fileHandleID, fh)) {
      return VMK_INVALID_HANDLE;
   }

   result = FSS_FH_FILEOPS(fh)->FSS_ResetCommand(fh->fileDesc, cmd);

   return result;
}


VMK_ReturnStatus
FSS_ListPEs(const char *volumeName, uint32 maxPartitions,
            VMnix_PartitionListResult *result)
{
   char token[FS_MAX_FILE_NAME_LENGTH];
   VMK_ReturnStatus status;
   FSN_TokenType tokenType;
   const char *nextToken = NULL;
   VC_VMFSVolume *pt;

   ASSERT(result != NULL);
   nextToken = FSN_AbsPathNTokenizer(volumeName, nextToken,
                                     FS_MAX_VOLUME_NAME_LENGTH,
                                     token, &tokenType);
   if (tokenType != FSN_TOKEN_VOLUME_ROOT) {
      return VMK_INVALID_NAME;
   }
   pt = VC_FindVMFSVolume(token, TRUE);
   if (pt != NULL) {
      uint32 numPhyExtents;

      numPhyExtents = MIN(maxPartitions, pt->fsAttrs->numPhyExtents);
      memcpy(result, pt->fsAttrs, VMNIX_PARTITION_ARR_SIZE(numPhyExtents));
      result->numPhyExtentsReturned = numPhyExtents;
      status = VMK_OK;
   } else {
      status = VMK_NOT_FOUND;
   }
   VC_ReleaseVMFSVolume(pt);

   return status;
}


/*
 *-----------------------------------------------------------------------------
 *
 * FSS_BeginRescan --
 *    Called by VC_RescanVolumes (volume cache) to signal the start of
 *    a VMFS partition rescan, so that the FS switch and the FS
 *    implementations can prepare for it. Responsible for establishing
 *    mutual exclusion for FS rescan requests.
 *
 *    This function grabs fsLock so that non-rescan FSS_Open requests block
 *    for the duration of the rescan.
 *
 * Results:
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
void
FSS_BeginRescan(void)
{
   FSSRescan();
   Semaphore_Lock(&fsLock);
}

/*
 *-----------------------------------------------------------------------------
 *
 * FSS_EndRescan --
 *    Called by vmk_scsi to signal that it is done rescanning the
 *    VMFS partitions.
 *
 * Results:
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
void
FSS_EndRescan(void)
{
   Semaphore_Unlock(&fsLock);
   FSSRescan();
}


/*
 *-----------------------------------------------------------------------------
 *
 * FSSOpenVolume --
 *
 *    Open a volume by its name. The following flags are supported:
 *
 *      - FS_OPEN_LOCKED: lock the file system 
 *      - FS_OPEN_RESCAN: don't acquire 'fsLock' as the call was made by
 *                        the rescanner, who already holds it
 *
 *    Set 'theVolume' to point to the volume's object descriptor.
 *
 *    !!! There is horrible duplication of code between
 *    FSS{Open,Close}Volume and the OC volume functions. Also, the FSS
 *    should not be poking around in the OC. No time to deal with this
 *    now.
 *
 * Results:
 *    '*theVolume' points to the object descriptor for the specified volume.
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
FSSOpenVolume(const char *volumeName,       // IN: the volume's name
	      uint32 flags,                 // IN: open modifiers
	      ObjDescriptorInt **theVolume) // OUT: filled out descriptor
{
   VMK_ReturnStatus status;
   Bool calledFromRescan = FALSE;
   Bool needDestroy = FALSE, needClose = FALSE;
   Bool shouldUpdateVMFSVolume = FALSE, triggerVMFSEvent = FALSE;
   FSSRegisteredFS *regFS = NULL;
   char volName[FS_MAX_VOLUME_NAME_LENGTH];
   ObjDescriptorInt *openVol;
   FSDescriptorInt *openFS;
   FSDescriptorInt *fs;
   extern ObjDescriptorInt *openVolList;

   if (volumeName[0] == '\0') {
      return VMK_BAD_PARAM;
   }

   if (flags & FS_OPEN_RESCAN) {
      calledFromRescan = TRUE;
      ASSERT(Semaphore_IsLocked(&fsLock));
   } else {
      Semaphore_Lock(&fsLock);
   }

   strncpy(volName, volumeName, FS_MAX_VOLUME_NAME_LENGTH);

   if (calledFromRescan == FALSE) {
      VC_VMFSVolume *np;

      np = VC_FindVMFSVolume(volumeName, TRUE);
      if (np) {
         strncpy(volName, np->fsAttrs->peAddresses[0].peName,
                 FS_MAX_VOLUME_NAME_LENGTH);
      } else {
         /*
	  * User is trying to open a uncached VMFS volume. If this
	  * open is successful, remember to notify userlevel about the
	  * new volume. (??? Can notification be done by VC_UpdateVMFSVolume?)
          */
         triggerVMFSEvent = TRUE;
      }
      VC_ReleaseVMFSVolume(np);
   }

   /*
    * Check if the volume is already opened. If it is, reserve its descriptor
    * in the object cache as it will be passed down to an implementation.
    */
   for (openVol = openVolList; openVol != NULL;
	openVol = openVol->next) {
      openFS = FSDESC(openVol);

      if (strncmp(openFS->volumeName, volName, FS_MAX_VOLUME_NAME_LENGTH) == 0) {
         //XXX refCount > 0 in future
	 ASSERT(openVol->refCount >= 0);
	 openVol->refCount++;

	 /*
	  * An opened volume's implementation can be determined from its OID.
	  */
	 status = FSSGetRegisteredFS(openVol->oid.fsTypeNum, &regFS);
	 ASSERT(status == VMK_OK);
         if (status == VMK_OK) {
            FSSReleaseRegisteredFS(regFS);
         } else {
            Warning("RefCount on %s is %d, but failed to get driver",
                    FSDESC(openVol)->volumeName, openVol->refCount);
            goto onError;
         }
	 break;
      }
   }

   if (openVol == NULL) {
      /* Volume is not opened. Initialize a new object descriptor. */
      status = OC_CreateObjectDesc(&openVol);
      if (status != VMK_OK) {
	 goto onError;
      }
      openVol->objType = OBJ_VOLUME;
#if 0  // initialization moved to FS2_Open (probably not the best place..)
      FSS_InitObjectDesc(openVol);
#endif

      fs = FSDESC(openVol);

      /* Try each implementation's volume open handler. */
      status = VMK_INVALID_FS;

      SP_Lock(&regFSLock);
      regFS = fssRegisteredFSList;
      while (regFS != NULL) {
	 Mod_IncUseCount(regFS->moduleID);
         SP_Unlock(&regFSLock);
	 status = regFS->fsOps->FSS_Open(volName, flags, openVol);
	 if (status == VMK_OK) {
	    break;
	 } else {
            /* OK to reacquire lock here because we incremented the module
             * refCount. So 'regFS' is still a valid reference.
             */
            SP_Lock(&regFSLock);
            Mod_DecUseCount(regFS->moduleID);
            if (status == VMK_MISSING_FS_PES) {
               SP_Unlock(&regFSLock);
               break;
            }
	 }
	 regFS = regFS->next;
      }
      if (regFS == NULL) {
         SP_Unlock(&regFSLock);
      }

      if (status != VMK_OK) {
	 needDestroy = TRUE;
	 goto onError;
      }

      /* Previously unopened volume -- update volume cache later. */
      shouldUpdateVMFSVolume = TRUE;

      /* Initialize FSS fields. */
      openVol->oid.fsTypeNum = regFS->fsTypeNum;
      openVol->objType = OBJ_VOLUME;
      FSDESC(openVol)->moduleID = regFS->moduleID;
      FSDESC(openVol)->fsTypeNum = regFS->fsTypeNum;

      /* Insert into opened volume list. */
      openVol->refCount = 1;
      openVol->next = openVolList;
      openVolList = openVol;

   } else {
      //XXX: FIXME: FS driver may be unloaded while we are trying to do the
      //foll.
      /* Volume opened. */
      fs = FSDESC(openVol);
      ASSERT(fs->openCount > 0);

      Mod_IncUseCount(regFS->moduleID);
      status = regFS->fsOps->FSS_Open(NULL, flags, openVol);
      if (status != VMK_OK) {
	 Mod_DecUseCount(regFS->moduleID);
	 goto onError;
      }

      if (calledFromRescan == TRUE) {
	 /* Volume cache is destroyed during a rescan. */
	 shouldUpdateVMFSVolume = TRUE;
      }
   }
      
   ASSERT(!triggerVMFSEvent || shouldUpdateVMFSVolume);

   if (shouldUpdateVMFSVolume == TRUE) {
      char driverType[FDS_MAX_DRIVERTYPE_LENGTH];
      VMnix_PartitionListResult *result = \
	 (VMnix_PartitionListResult *) fsAttributeBuf;

      /*
       * Update volume cache if the volume was newly opened, or if the
       * volume cache was invalidated by a rescan.
       */
      memset(fsAttributeBuf, 0,
	     VMNIX_PARTITION_ARR_SIZE(FSS_MAX_PARTITIONLIST_ENTRIES));

      status = regFS->fsOps->FSS_GetAttributes(openVol,
					       FSS_MAX_PARTITIONLIST_ENTRIES,
					       result);
      if (status != VMK_OK) {
	 needClose = TRUE;
	 goto onError;
      }
      result->rootDirOID.fsTypeNum = regFS->fsTypeNum;

      if (FSDESC(openVol)->devOps == NULL) {
	 /* Some FS implementations, like NFS and Stor, don't
          * have an underlying device. */
	 strcpy(driverType, VC_DRIVERTYPE_NONE_STR);
      } else {
	 status = FDS_GetDriverType(FSDESC(openVol)->devOps, driverType);
	 if (status != VMK_OK) {
	    needClose = TRUE;
	    goto onError;
	 }
      }

      status = VC_UpdateVMFSVolume(result, driverType, calledFromRescan);

      if (status != VMK_OK) {
	 needClose = TRUE;
	 goto onError;
      }

      if (triggerVMFSEvent) {
	 /*
	  * Ask serverd to refresh its VMFS volume information. In case
	  * of a FS_OPEN_RESCAN, optimize by postponing the event till
	  * the end of VC_RescanVolumes().
	  */
	 VmkEvent_VMFSArgs args;

	 memset(&args, 0, sizeof(args));
	 args.validData = TRUE;
	 strncpy(args.volumeName, result->peAddresses[0].peName,
		 FS_MAX_VOLUME_NAME_LENGTH - 1);
	 strncpy(args.volumeLabel, result->name,
		 FS_MAX_FS_NAME_LENGTH - 1);
            
	 VmkEvent_PostHostAgentMsg(VMKEVENT_VMFS, &args, sizeof(args));
      }
   }

   *theVolume = openVol;

   if (calledFromRescan == FALSE) {
      Semaphore_Unlock(&fsLock);
   }
   return VMK_OK;

onError:
   LOG(0, "Failed to open %s, status = %#x", volName, status);
   if (calledFromRescan == FALSE) {
      Semaphore_Unlock(&fsLock);
   }
   ASSERT(!(needClose && needDestroy));
   if (needClose == TRUE) {
      FSSCloseVolume(openVol, flags);
   }
   if (needDestroy == TRUE) {
#if 0  // destruction moved to FS2_Close (probably not the best place..)
      FSS_DestroyObjectDesc(openVol);
#endif
      OC_DestroyObjectDesc(openVol);
   }
   return status;

#if 0
      if (!startedFlush) {
         /* Start timer calls to flush the file systems. */
         Timer_Add(0, (Timer_Callback) FSS_FlushCallback,
                   FSS_FLUSH_PERIOD, TIMER_ONE_SHOT, 0);
         startedFlush = TRUE;
      }
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * FSSCloseVolume --
 *    Close the volume corrresponding to cached descriptor 'volDesc'.
 *
 *    !!! There is horrible duplication of code between
 *    FSS{Open,Close}Volume and the OC volume functions. Also, the FSS
 *    should not be poking around in the OC. No time to deal with this
 *    now.
 *
 * Results:
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
static void
FSSCloseVolume(ObjDescriptorInt *volDesc, uint32 openFlags)
{
   FSDescriptorInt *fs;

   if (!(openFlags & FS_OPEN_RESCAN)) {
      Semaphore_Lock(&fsLock);
   } else {
      ASSERT(Semaphore_IsLocked(&fsLock));
   }

   ASSERT(volDesc->refCount > 0);

   fs = FSDESC(volDesc);
   fs->fsOps->FSS_Close(volDesc, openFlags);

   /* Release reservation obtained in FSSOpenVolume(). */
   volDesc->refCount--;
   if (volDesc->refCount == 0) {
      ASSERT(fs->lockedCount <= 1);

      Mod_DecUseCount(fs->moduleID);
      OC_RemoveVolume(volDesc, FALSE);
      OC_DestroyObjectDesc(volDesc);
   }

   if (!(openFlags & FS_OPEN_RESCAN)) {
      Semaphore_Unlock(&fsLock);
   }
}


static VMK_ReturnStatus
FSSSetOpenMode(ObjDescriptorInt *fileDesc, uint32 openMode)
{
   VMK_ReturnStatus  status = VMK_OK;
   FS_FileAttributes attrs;
   FileDescriptorInt *fdInt = FILEDESC(fileDesc);

   ASSERT(fileDesc && fdInt->fileOps);
   ASSERT(Semaphore_IsLocked(&fileDesc->descLock));

   if ((openMode & (FILEOPEN_WRITE | FILEOPEN_DISK_IMAGE_ONLY)) != 0) {
      status = fdInt->fileOps->FSS_GetFileAttributes(fileDesc, &attrs);
      ASSERT(status == VMK_OK);
      if (status != VMK_OK) {
         Warning("status %x getting attrs", status);
         return status;
      }
   }
   
   if (fdInt->openCount > 0) {
      /*
       * File has already been opened, & so we have its descriptor.
       * We need to validate the new open flags against fdInt->openFlags
       * according to the foll. matrix:
       *
       *                  fd->openFlags
       *
       *               \  X S R W              X = FILEOPEN_EXCLUSIVE
       *                ----------             S = FILEOPEN_READONLY
       *        n r    X| n n n n              R = FILEOPEN_READ
       *        e e    S| n y y n              W = FILEOPEN_WRITE
       *        w q.   R| n y y y
       *               W| n n y y 
       *
       * We do not honor FILEOPEN_CREATE_FILE requests when a file of
       * the same name is open in X or S mode. Also, we do not honor
       * COW file creation requests if a file of the same name is open
       * under any mode.
       */

      LOG(2, "File "FS_OID_FMTSTR" already open.",
          FS_OID_VAARGS(&fileDesc->oid));

      if (openMode & FILEOPEN_CANT_EXIST) {
         return VMK_EXISTS;
      }
      
      if ((fdInt->openFlags & FILEOPEN_EXCLUSIVE) ||
          (openMode & FILEOPEN_EXCLUSIVE) ||
          ((openMode & FILEOPEN_WRITE) && 
           (fdInt->openFlags & FILEOPEN_READONLY)) ||
          ((fdInt->openFlags & FILEOPEN_WRITE) && 
           (openMode & FILEOPEN_READONLY))) {
         return VMK_BUSY;
      }
      
      if ((openMode & FILEOPEN_DISK_IMAGE_ONLY) && 
          (attrs.flags & FS_NOT_ESX_DISK_IMAGE)) {
         Warning("Accessing non-disk-image VMFS file "
                 FS_OID_FMTSTR" as a virtual disk",
                 FS_OID_VAARGS(&fileDesc->oid));
         return VMK_BAD_PARAM;
      }
   }
   
   /* Set regenerateGeneration if disk image file is opened by a writer */
   fdInt->regenerateGeneration = ((openMode & FILEOPEN_WRITE) &&
				  !(attrs.flags & FS_NOT_ESX_DISK_IMAGE));
   
   if (openMode & FILEOPEN_READ) {
      fdInt->readerCount++;
   } else if (openMode & FILEOPEN_READONLY) {
      fdInt->sharedReaderCount++;
   }
   if (openMode & FILEOPEN_WRITE) {
      fdInt->writerCount++;
   }
   
   /* Remember if opened with either of these flags */
   fdInt->openFlags |= (openMode & (FILEOPEN_READ |
				    FILEOPEN_READONLY |
				    FILEOPEN_WRITE |
				    FILEOPEN_EXCLUSIVE |
				    FILEOPEN_PHYSICAL_RESERVE));
   return status;
}


/*
 * Part of the former FSSDoFileOp.
 */
static VMK_ReturnStatus
FSSOpenFile(ObjDescriptorInt *fileDesc, uint32 openFlags, void *dataIn,
	    Bool getDescLock,               // IN: if TRUE, obtain descriptor lock
	    FS_FileHandleID *fileHandleID)  // OUT
{
   VMK_ReturnStatus  status;
   FS_FileHandleID   newFhID;
   FSFileHandle      *newFh;
   FileDescriptorInt *fd =  FILEDESC(fileDesc);

   ASSERT(openFlags & (FILEOPEN_READ | FILEOPEN_READONLY |
		       FILEOPEN_WRITE | FILEOPEN_EXCLUSIVE));
   ASSERT(!((openFlags & FILEOPEN_READ) &&
            (openFlags & FILEOPEN_READONLY)));
   ASSERT(!((openFlags & FILEOPEN_EXCLUSIVE) &&
            (openFlags & 
             (FILEOPEN_READ | FILEOPEN_READONLY | FILEOPEN_WRITE))));
   ASSERT(!((openFlags & (FILEOPEN_READ | FILEOPEN_WRITE)) &&
            (openFlags & FILEOPEN_READONLY)));
   LOG(1, FS_OID_FMTSTR" flags %s, %s, %s, %s", 
       FS_OID_VAARGS(&fileDesc->oid),
       (openFlags & FILEOPEN_EXCLUSIVE) ? "EX" : "0",
       (openFlags & FILEOPEN_READ) ? "RD" : "0",
       (openFlags & FILEOPEN_READONLY) ? "RO" : "0",
       (openFlags & FILEOPEN_WRITE) ? "WR" : "0");

   /* Obtain descriptor lock if necessary. */
   if (getDescLock == TRUE) {
      Semaphore_Lock(&fileDesc->descLock);
   } else {
      ASSERT(Semaphore_IsLocked(&fileDesc->descLock));
   }

   /* Get new, initialized file handle. */
   status = FSSGetNewFileHandle(&newFhID);
   if (status != VMK_OK) {
      goto done;
   }
   newFh = FSS_GET_FH_PTR(newFhID);
   ASSERT(newFh->handleID == newFhID);

   /*
    * Call implementation open file handler only if the file is not
    * already open.
    */
   if (fd->openCount == 0) {
      status = fd->fileOps->FSS_OpenFile(fileDesc, openFlags, dataIn);
      if (status != VMK_OK) {
	 FSSReleaseFileHandle(newFhID);
	 goto done;
      }
   }

   status = FSSSetOpenMode(fileDesc, openFlags);
   if (status != VMK_OK) {
      FSSReleaseFileHandle(newFhID);
      goto done;
   }

   /* File successfully opened. */
   fd->openCount++;
   
   SP_Lock(&handleLock);
   newFh->fileDesc = fileDesc;
   newFh->openFlags = (openFlags & (FILEOPEN_READ | FILEOPEN_READONLY |
				    FILEOPEN_WRITE | FILEOPEN_EXCLUSIVE |
				    FILEOPEN_PHYSICAL_RESERVE));
   *fileHandleID = newFhID;
   SP_Unlock(&handleLock);
   
   status = VMK_OK;

done:
   if (getDescLock == TRUE) {
      Semaphore_Unlock(&fileDesc->descLock);
   }
   return status;
}


/*
 * Create a file.
 *
 * Part of the former FSSDoFileOp.
 */
static VMK_ReturnStatus
FSSCreateFile(ObjDescriptorInt *dirDesc, const char *name,
	      uint32 opFlags, FS_DescriptorFlags descFlags, void *dataIn,
	      FS_ObjectID *fileOID)  // OUT: on success, OID of created file
{
   VMK_ReturnStatus status = VMK_OK;
   FileDescriptorInt  *dir =  FILEDESC(dirDesc);

   if (strlen(name) >= FS_MAX_FILE_NAME_LENGTH) {
      return VMK_NAME_TOO_LONG;
   }

   ASSERT(dirDesc->refCount > 0);
   
   /* Call down to actually create the file. */
   status = dir->fileOps->FSS_CreateFile(dirDesc, name,
                                         opFlags, descFlags, dataIn,
                                         fileOID);
   return status;
}


/*
 *-----------------------------------------------------------------------------
 *
 * FSSFileIO --
 *    Call into the FS implementation to do synchronous or asynchronous
 *    file IO. The actual amount of data transferred is returned in
 *    bytesTransferred.
 *
 *    This function should do asynchronous IO if token != NULL, synchronous
 *    otherwise.
 *
 *    The ioFlags can specify if its a read (FS_READ_OP) or write
 *    (FS_WRITE_OP), if ioAccess lock should be acquired (default)
 *    or not (FS_NOLOCK); and in case of async IO, if the function
 *    can block on doing small file metadata reads (FS_CANBLOCK) if needed.
 *
 *    For AsyncFileIO, FS_CANTBLOCK implies that the FS
 *    implementation should not make any blocking calls and shouldn't
 *    acquire semaphores. NOTE: In this case, there's no locking of the
 *    file to guard against simultaneous read/write vs. change file size.
 *    The caller is responsible for file consistency.
 *
 *    Usually asyncIO shouldn't use the FS_CANTBLOCK flag. FS_CANTBLOCK
 *    is normally used by the swapper, when calling in from a BH.
 *
 * Results:
 *    The amount of data read/written is returned in bytesTransferred.
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
FSSFileIO(FS_FileHandleID fileHandleID, SG_Array *sgArr, Async_Token *token,
          IO_Flags ioFlags, uint32 *bytesTransferred)
{
   FSFileHandle *fh = FSS_GET_FH_PTR(fileHandleID);

   if (FSS_BAD_FHI(fileHandleID, fh)) {
      return VMK_INVALID_HANDLE;
   }

   if (ioFlags & FS_READ_OP) {
      if (!(fh->openFlags &
            (FILEOPEN_READ | FILEOPEN_READONLY |
             FILEOPEN_EXCLUSIVE))) {
         Log("Can't read from "FS_OID_FMTSTR" flags %x", 
             FS_OID_VAARGS(&fh->fileDesc->oid), fh->openFlags);
         return VMK_READ_ERROR;
      }
   }

   if (!((ioFlags & FS_READ_OP) ||
         (fh->openFlags & (FILEOPEN_WRITE | FILEOPEN_EXCLUSIVE)))) {
      Log("Can't write to "FS_OID_FMTSTR" flags %x", 
          FS_OID_VAARGS(&fh->fileDesc->oid), fh->openFlags);
      return VMK_WRITE_ERROR;
   }
   
   return FSS_FH_FILEOPS(fh)->FSS_FileIO(fh->fileDesc, sgArr, token, ioFlags,
                                         bytesTransferred);
}


/*
 *-----------------------------------------------------------------------------
 *
 * FSSZeroOutBlockTail --
 *    Try to zero out the trailing portion of an uninitialized block
 *
 * Results:
 *    On success, fh->zeroBlock is set to FS_INT_INVALID_ZEROBLOCK. No
 *    more lazy zeroing needed.
 *
 * Side effects:
 *    fh->zeroBlock will be reset to FS_INT_INVALID_ZEROBLOCK on success.
 *
 *
 *-----------------------------------------------------------------------------
 */
static void
FSSZeroOutBlockTail(ObjDescriptorInt *fileDesc)
{
   FileDescriptorInt *fd =  FILEDESC(fileDesc);
   Bool forceBlockTailZero = FALSE;

   SP_Lock(&fd->zeroLock);
   if (fd->zeroBlock != FS_INT_INVALID_ZEROBLOCK) {
      forceBlockTailZero = TRUE;
   }
   SP_Unlock(&fd->zeroLock);

   if (forceBlockTailZero) {
      uint32 bytesWritten = 0;
      SG_Array sgArr;

      FSSSingletonSGArray(&sgArr, 0, 0, 0, SG_VIRT_ADDR);
      FSS_FD2FILEOPS(fileDesc)->FSS_FileIO(fileDesc, &sgArr, NULL, FS_WRITE_OP, &bytesWritten);
      ASSERT(bytesWritten == 0);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * FSSRescan --
 *    Signal the FS implementations that a rescan to discover or drop FS
 *    volumes is going to take place or has just ended.
 *
 * Results:
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
static void
FSSRescan()
{
   FSSRegisteredFS *driver;

   SP_Lock(&regFSLock);
   driver = fssRegisteredFSList;
   while (driver != NULL) {
      driver->fsOps->FSS_TimerCallback(NULL, FSS_CALLBACK_RESCAN);
      driver = driver->next;
   }
   SP_Unlock(&regFSLock);
}

/*
 *-----------------------------------------------------------------------------
 *
 * FSSGetFreeFileHandle --
 *    Get a free file handle. The handle is not reserved until
 *    fsFileHandleTable[freeHandleIndex].inUse is set to TRUE.
 *
 * Results:
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
static int
FSSGetFreeFileHandle(void)
{
   int freeHandleIndex;

   ASSERT(SP_IsLocked(&handleLock));
   for (freeHandleIndex = 0; freeHandleIndex < FS_NUM_FILE_HANDLES; freeHandleIndex++) {
      if (fsFileHandleTable[freeHandleIndex].inUse == FALSE) {
	 return freeHandleIndex;
      }
   }
   return -1;
}

static VMK_ReturnStatus
FSSGetNewFileHandle(FS_FileHandleID *newFhID)
{
   int freeHandleIndex;

   SP_Lock(&handleLock);
   freeHandleIndex = FSSGetFreeFileHandle();
   if (freeHandleIndex < 0) {
      SP_Unlock(&handleLock);
      return VMK_NO_FREE_HANDLES;
   }
   fsFileHandleTable[freeHandleIndex].handleID += FS_NUM_FILE_HANDLES;
   fsFileHandleTable[freeHandleIndex].fileDesc = NULL;
   fsFileHandleTable[freeHandleIndex].inUse = TRUE;
   SP_Unlock(&handleLock);

   *newFhID = fsFileHandleTable[freeHandleIndex].handleID;

   return VMK_OK;
}

static void
FSSReleaseFileHandle(FS_FileHandleID fileHandleID)
{
   FSFileHandle *fh = FSS_GET_FH_PTR(fileHandleID);

   SP_Lock(&handleLock);
   fh->fileDesc = NULL;
   fh->inUse = FALSE;     
   SP_Unlock(&handleLock);
}

void
FSS_InitObjectDesc(ObjDescriptorInt *desc)
{
   switch (desc->objType)
      {
      case OBJ_VOLUME:
	 {
	    // XXX Moved to FS2_Open.
#if 0
	    Semaphore_Init("volDescLock", &desc->descLock, 1,
			   FS_SEMA_RANK_FS_DESCLOCK);
#endif
	    break;
	 }
      case OBJ_DIRECTORY:
	 {
	    FileDescriptorInt *dd = FILEDESC(desc);

	    Semaphore_Init("dirDescLock", &desc->descLock, 1,
			   FS_SEMA_RANK_DIR_DESCLOCK);

	    SP_InitLock("nameCacheLock", &dd->nameCacheLock, SP_RANK_LEAF);
	    break;
	 }
      case OBJ_REGFILE:
	 {
	    FileDescriptorInt *fd = FILEDESC(desc);

	    Semaphore_Init("fileDescLock", &desc->descLock, 1,
			   FS_SEMA_RANK_FILE_DESCLOCK);

	    fd->zeroBlock = FS_INT_INVALID_ZEROBLOCK;
	    Semaphore_RWInit("ioAccess", &fd->ioAccess);
	    SP_InitLock("zeroLock", &fd->zeroLock, SP_RANK_LEAF);
	    break;
	 }
      default:
	 Panic("Unrecognized object type.");
      }
}

void
FSS_DestroyObjectDesc(ObjDescriptorInt *desc)
{
   switch (desc->objType)
      {
      case OBJ_VOLUME:
	 {
	    // XXX Moved to FS2_Close.
#if 0
	    Semaphore_Cleanup(&desc->descLock);
#endif
	    break;
	 }
      case OBJ_DIRECTORY:
	 {
	    FileDescriptorInt *dd = FILEDESC(desc);

	    Semaphore_Cleanup(&desc->descLock);
	    SP_CleanupLock(&dd->nameCacheLock);
	    break;
	 }
      case OBJ_REGFILE:
	 {
	    FileDescriptorInt *fd = FILEDESC(desc);

	    Semaphore_Cleanup(&desc->descLock);
	    Semaphore_RWCleanup(&fd->ioAccess);
	    SP_CleanupLock(&fd->zeroLock);
	    break;
	 }
      default:
	 Panic("Unrecognized object type.");
      }
}

VMK_ReturnStatus
FSS_GetVolumeOID(const char *volumeName,
		 FSS_ObjectID *oid)
{
   return VMK_NOT_IMPLEMENTED;
}
