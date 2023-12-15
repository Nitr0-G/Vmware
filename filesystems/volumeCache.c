/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/************************************************************
 *
 *  volumeCache.c
 *     VMFS volume cache management functions.
 *
 ************************************************************/

#include "vm_types.h"
#include "x86.h"
#include "vm_libc.h"
#include "vmkernel.h"
#include "list.h"
#include "splock.h"
#include "action.h"
#include "vm_asm.h"
#include "sched.h"
#include "world.h"
#include "memalloc.h"
#include "scsi_defs.h"
#include "mod_loader.h"
#include "config.h"
#include "util.h"
#include "libc.h"
#include "debug.h"
#include "fsSwitch.h"
#include "vmkevent.h"
#include "volumeCache.h"
#include "fsDeviceSwitch.h"

#define LOGLEVEL_MODULE VC
#include "log.h"

/*
 * List of known VMFS volumes.  Protected by vcLock.
 */
static SP_SpinLock vcLock;
static VC_VMFSVolume *VMFSVolumeList = NULL;

// Wait Q to quiesce FS_Open() activity while a device rescan is in progress
static Bool vcRescanInProgress = FALSE;

static INLINE void VCFreeVMFSPartition(VC_VMFSVolume *pt);
static INLINE void VCWaitOnRescan(void);
static VC_VMFSVolume *VCFindVMFSVolume(const char *name, Bool longSearch);

void
VC_Init(void)
{
   SP_InitLock("volCache", &vcLock, SP_RANK_LEAF);
}

/*
 *-----------------------------------------------------------------------------
 *
 * VC_Readdir --
 *    Return a list of the VMFSes that are accessible.  Only return up to
 *    maxEntries results, but set result->numVMFS to the actual number.
 *
 * Results:
 *    A list of VMFS volumes is returned in 'result' and result->numVMFS,
 *    result->numReturned is set on return.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
VC_Readdir(uint32 maxEntries, VMnix_ReaddirResult *result)
{
   uint32 count = 0, descNum = 2;
   VC_VMFSVolume *pt;
   
   /* Insist on an even number of result entries, so we can potentiall
    * return VMFS volume labels along with the canonical volume names which
    * are based on canonical device names.
    */
   if (maxEntries & 1) {
      LOG(0, "Use even number of entries, rather than %d", maxEntries);
      return VMK_BAD_PARAM;
   }

   SP_Lock(&vcLock);
   VCWaitOnRescan();

   for (pt = VMFSVolumeList; pt != NULL; pt = pt->next) {
      if (count < maxEntries) {
         strncpy(result->dirent[count].fileName,
                 pt->fsAttrs->peAddresses[0].peName,
                 FS_MAX_FILE_NAME_LENGTH);
         result->dirent[count].flags = FS_DIRECTORY;
         result->dirent[count].descNum = descNum;
         count++;
         descNum++;
#if 0
         if (pt->fsAttrs->name[0] != '\0') {
            strncpy(result->dirent[count].fileName,
                    pt->fsAttrs->name,
                    FS_MAX_FILE_NAME_LENGTH);
            result->dirent[count].flags = FS_LINK;
            result->dirent[count].descNum = descNum;
            count++;
         }
         descNum++;
#endif
      } else {
#if 0
         count += 2;
#endif
         count++;
      }
   }
   SP_Unlock(&vcLock);

   result->mtime = result->ctime = result->atime = consoleOSTime;
   result->numDirEntriesReturned = (count < maxEntries) ? count : maxEntries;
   result->totalNumDirEntries = count;
   return VMK_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 * VC_Lookup --
 *    Lookup a given VMFS volume name/label and if found, return the
 *    object ID of the volume's root directory in 'rootDirOID'.
 *
 * Results:
 *    VMK_OK on success - 'rootDirOID' contains a valid object ID.
 *    VMK_NOT_FOUND if the volume was not found in the cache.
 *
 * Side effects:
 *    Contents of 'rootDirOID' may be modified.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
VC_Lookup(const char *name, FSS_ObjectID *rootDirOID)
{
   VMK_ReturnStatus status;
   VC_VMFSVolume *pt;

   LOG(2, "%s", name);

   if (!strncmp(name, "..", 3) || !strncmp(name, ".", 2)) {
      FSS_MakeVMFSRootOID(rootDirOID);
      return VMK_OK;
   }

   pt = VC_FindVMFSVolume(name, TRUE);
   if (pt != NULL) {
      FSS_InitOID(rootDirOID);
      FSS_CopyOID(rootDirOID, &pt->fsAttrs->rootDirOID);
      LOG(2, "returns "FS_OID_FMTSTR, FS_OID_VAARGS(rootDirOID));
      status = VMK_OK;
   } else {
      LOG(0, "%s not found", name);
      status = VMK_NOT_FOUND;
   }
   VC_ReleaseVMFSVolume(pt);
   return status;
}

/*
 *-----------------------------------------------------------------------------
 *
 * VC_GetFileAttributes --
 *    Get attributes for the VMFS root directory (/vmfs).
 * 
 * Results:
 *    VMK_OK, attributes for '/vmfs' are returned in 'attrs'.
 *
 * Side effects:
 *    Contents of 'attrs' are modified.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
VC_GetFileAttributes(FS_FileAttributes *attrs)
{
   memset(attrs, 0, sizeof(*attrs));
   attrs->length = 512;
   attrs->diskBlockSize = 512;
   attrs->fsBlockSize = 1024576;
   attrs->flags = FS_DIRECTORY;
   attrs->generation = 0xbadbeef;
   attrs->descNum = 0xffffffff;
   attrs->mtime = consoleOSTime;
   attrs->ctime = consoleOSTime;
   attrs->atime = consoleOSTime;
   attrs->uid = 0;
   attrs->gid = 0;
   attrs->mode = 01777; //S_IRWXUGO | S_ISVTX
   attrs->toolsVersion = 0;
   attrs->virtualHWVersion = 0;
   attrs->rdmRawHandleID = -1;
   strncpy(attrs->fileName, FS_ROOT_NAME, sizeof(attrs->fileName));
   return VMK_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 * VC_UpdateVMFSVolume --
 *    Given information about a VMFS volume in 'result', first search for
 *    a match in the cached list of VMFS partitions. In case of a cache hit
 *    (thats the case most of the time on a steady state ESX system), check
 *    that the volume label (fsName) stored in the cache is up-to-date. This
 *    check is necessary in the light of the fact that multiple ESX servers
 *    can change the volume label of a public VMFS volume.
 *
 *    In case of a cache miss, we never saw this partition earlier and we
 *    need to add it to the cache.
 *
 * Results:
 *
 * Side effects:
 *    An entry in the linked list starting with VMFSVolumeList may be
 *    added or modified. 
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
VC_UpdateVMFSVolume(const VMnix_PartitionListResult *result,  // IN: latest partition info
                    const char *driverType,
		    Bool calledFromRescan)
{
   VMK_ReturnStatus status = VMK_OK;
   VC_VMFSVolume *pt;

   ASSERT(result->numPhyExtents > 0 &&
          result->numPhyExtentsReturned > 0);
   ASSERT(strlen(driverType) < sizeof(pt->driverType));

   SP_Lock(&vcLock);
   if (vcRescanInProgress && !calledFromRescan) {
      SP_Unlock(&vcLock);
      return VMK_OK;
   }
   pt = VCFindVMFSVolume(result->peAddresses[0].peName, TRUE);

   if (pt == NULL) {
      /* Add a new entry to the list of VMFS volumes */
      pt = (VC_VMFSVolume *)Mem_Alloc(sizeof(*pt));
      if (pt == NULL) {
         status = VMK_NO_MEMORY;
         goto exit;
      }
      pt->fsAttrs = (VMnix_PartitionListResult *)
         Mem_Alloc(VMNIX_PARTITION_ARR_SIZE(result->numPhyExtentsReturned));
      if (pt->fsAttrs == NULL) {
         Mem_Free(pt);
         status = VMK_NO_MEMORY;
         goto exit;
      }
      memcpy(pt->fsAttrs, result,
             VMNIX_PARTITION_ARR_SIZE(result->numPhyExtentsReturned));
      strncpy(pt->driverType, driverType, sizeof(pt->driverType));
      pt->next = VMFSVolumeList;
      VMFSVolumeList = pt;
      
      LOG(0, "Attributes for %s", pt->fsAttrs->peAddresses[0].peName);
   } else {
      Bool triggerVMFSEvent = FALSE;
      /* Refresh the cached information, in case some remote server has
       * modified the volume label.
       */
      if (pt->fsAttrs->versionNumber != result->versionNumber) {
         pt->fsAttrs->versionNumber = result->versionNumber;
         triggerVMFSEvent = TRUE;
      }
      if (pt->fsAttrs->minorVersion != result->minorVersion) {
         pt->fsAttrs->minorVersion = result->minorVersion;
         triggerVMFSEvent = TRUE;
      }
      if (!FSS_OIDIsEqual(&pt->fsAttrs->rootDirOID, &result->rootDirOID)) {
	 FSS_CopyOID(&pt->fsAttrs->rootDirOID, &result->rootDirOID);
	 triggerVMFSEvent = TRUE;
      }
      if (memcmp(&pt->fsAttrs->uuid, &result->uuid, sizeof(UUID))) {
         memcpy(&pt->fsAttrs->uuid, &result->uuid, sizeof(UUID));
         triggerVMFSEvent = TRUE;
      }
      ASSERT(strlen(result->name) < FS_MAX_FS_NAME_LENGTH);
      if (strcmp(pt->fsAttrs->name, result->name)) {
         strcpy(pt->fsAttrs->name, result->name);
         triggerVMFSEvent = TRUE;
      }
      if (strcmp(pt->driverType, driverType)) {
         LOG(0, "Device %s is now managed by %s driver",
             result->peAddresses[0].peName, driverType);
         strcpy(pt->driverType, driverType);
         triggerVMFSEvent = TRUE;
      }

      if (triggerVMFSEvent) {
         VmkEvent_VMFSArgs args;
         
         memset(&args, 0, sizeof(args));
         args.validData = TRUE;
         strncpy(args.volumeName, result->peAddresses[0].peName,
                 FS_MAX_VOLUME_NAME_LENGTH);
         strncpy(args.volumeLabel, result->name,
                 FS_MAX_FS_NAME_LENGTH);
         
         VmkEvent_PostHostAgentMsg(VMKEVENT_VMFS, &args, sizeof(args));
      }
   }

exit:
   SP_Unlock(&vcLock);
   return status;
}

/*
 *-----------------------------------------------------------------------------
 *
 * VC_SetName --
 *    Update the volume label for a given VMFS volume.
 *
 * Results:
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
void
VC_SetName(const char *volumeName, const char *fsName)
{
   VC_VMFSVolume *pt;

   SP_Lock(&vcLock);
   VCWaitOnRescan();

   pt = VCFindVMFSVolume(volumeName, TRUE);
   if (pt != NULL) {
      strncpy(pt->fsAttrs->name, fsName, FS_MAX_FS_NAME_LENGTH);
   }
   SP_Unlock(&vcLock);
}

/*
 *-----------------------------------------------------------------------------
 *
 * VCFindVMFSVolume --
 *    Return info on a VMFS volume with the specified volume name/label.
 *    First search for the volume assuming 'name' refers to a VMFS volume
 *    label. If this search fails and longSearch is TRUE, do another search
 *    based on the default VMFS volume naming convention: vmhbax:y:z:a.
 *
 *    On successful search, a ptr to the volume information
 *    structure is returned.
 *
 *    Requires that vcLock is held.
 *
 * Results:
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
static VC_VMFSVolume *
VCFindVMFSVolume(const char *name, Bool longSearch)
{
   VC_VMFSVolume *pt;

   ASSERT(SP_IsLocked(&vcLock));
   for (pt = VMFSVolumeList; pt != NULL; pt = pt->next) {
      if ((strcmp(pt->fsAttrs->name, name) == 0) ||
          (longSearch &&
           (strcmp(pt->fsAttrs->peAddresses[0].peName, name) == 0))) {         
	 return pt;
      }
   }
   return NULL;
}

/*
 *-----------------------------------------------------------------------------
 *
 * VC_FindVMFSVolume --
 *    Return info on a VMFS volume with the specified name. The returned
 *    VC_VMFSVolume reference points to live info, and the caller
 *    should release the reference after use by calling
 *    VC_ReleaseVMFSVolume(). Failure to do so will result in 'spin
 *    count exceeded'. It follows that the caller is not allowed to do
 *    blocking operations between find and release.
 *
 * Results:
 *    Pointer to partition info on success.
 *
 * Side effects:
 *    vcLock is acquired and held.
 *
 *-----------------------------------------------------------------------------
 */
VC_VMFSVolume *
VC_FindVMFSVolume(const char *volumeName, Bool longSearch)
{
   VC_VMFSVolume *pt;

   SP_Lock(&vcLock);
   VCWaitOnRescan();
   pt = VCFindVMFSVolume(volumeName, longSearch);
   return pt;
}

/*
 *-----------------------------------------------------------------------------
 *
 * VCFindVMFSVolumeByUUID --
 *    Return info on a partition with the specified UUID.
 *    Requires that vcLock is held.
 *
 * Results:
 *    Pointer to partition information on success.
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
static VC_VMFSVolume *
VCFindVMFSVolumeByUUID(const UUID *uuid)
{
   VC_VMFSVolume *pt;

   ASSERT(SP_IsLocked(&vcLock));
   for (pt = VMFSVolumeList; pt != NULL; pt = pt->next) {
      if (memcmp(uuid, &pt->fsAttrs->uuid, sizeof(UUID)) == 0) {
	 return pt;
      }
   }
   return NULL;
}

/*
 *-----------------------------------------------------------------------------
 *
 * VC_FindVMFSVolumeByUUID --
 *    Return info on a partition with the specified UUID.
 *
 * Results:
 *    Pointer to partition information on success.
 *
 * Side effects:
 *    vcLock is acquired and held.
 *
 *-----------------------------------------------------------------------------
 */
VC_VMFSVolume *
VC_FindVMFSVolumeByUUID(const UUID *uuid)
{
   VC_VMFSVolume *pt;

   SP_Lock(&vcLock);
   VCWaitOnRescan();
   pt = VCFindVMFSVolumeByUUID(uuid);
   return pt;
}


/*
 *-----------------------------------------------------------------------------
 *
 * VC_ReleaseVMFSVolume --
 *    Release a reference to a VMFSVolumeList entry. For now, the
 *    argument 'pt' is just so that calls look nicer.
 *
 * Results:
 *
 * Side effects:
 *    vcLock is released.
 *
 *-----------------------------------------------------------------------------
 */
void
VC_ReleaseVMFSVolume(const VC_VMFSVolume *pt)
{
   ASSERT(SP_IsLocked(&vcLock));
   SP_Unlock(&vcLock);
}

/*
 *-----------------------------------------------------------------------------
 *
 * VCFreeVMFSPartition --
 *    Free up memory allocated to a named partition structure.
 *    Requires that vcLock is held
 *
 * Results:
 *
 * Side effects:
 *    'pt' is a stale reference on return.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE void
VCFreeVMFSPartition(VC_VMFSVolume *pt)
{
   ASSERT(SP_IsLocked(&vcLock));
   ASSERT(pt != NULL);
   ASSERT(pt->fsAttrs != NULL);

   Mem_Free(pt->fsAttrs);
   Mem_Free(pt);
}

/*
 *-----------------------------------------------------------------------------
 *
 * VCWaitOnRescan --
 *    If VMFS/adapter rescan is in progress, release the vcLock and put the
 *    caller to sleep on 'vcRescanInProgress'. Requires vcLock to be
 *    held on entry.
 *
 * Results:
 *
 * Side effects:
 *    This world might go into the WAIT state. Also, vcLock will be
 *    released over the duration of the wait.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE void
VCWaitOnRescan(void)
{
   ASSERT(SP_IsLocked(&vcLock));
   while (vcRescanInProgress) {
      CpuSched_Wait((uint32)&vcRescanInProgress, CPUSCHED_WAIT_FS, &vcLock);
      SP_Lock(&vcLock);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * VC_RescanVolumes --
 *    1. Quiesce FS open/close activity.
 *    2. Partly (driverType != NULL)/fully (driverType == NULL)
 *       invalidate VMFS volume cache.
 *    3. Call into the device layer to request hardware rescan.
 *    4. Partly (driverType != NULL)/fully (driverType == NULL)
 *       rebuild VMFS volume cache.
 *    5. Resume FS open/close activity.
 *    6. Send rescan completion notification to userlevel.
 *
 * Results:
 *    VMK_OK on success, error code otherwise.
 *
 * Side effects:
 *    VMFS volumes or underlying FS devices may appear/dissappear
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
VC_RescanVolumes(const char *driverType, void *driverData)
{
   VMK_ReturnStatus status;
   VmkEvent_VMFSArgs args;
   VC_VMFSVolume *prev, *cur;

   FSS_BeginRescan();

   SP_Lock(&vcLock);
   if (vcRescanInProgress) {
      SP_Unlock(&vcLock);
      FSS_EndRescan();
      return VMK_BUSY;
   }
   vcRescanInProgress = TRUE;

   prev = NULL;
   cur = VMFSVolumeList;
   while (cur != NULL) {
      Bool deleteCacheEntry = FALSE;

      if (driverType == NULL) {
         deleteCacheEntry = (strncmp(cur->driverType, VC_DRIVERTYPE_NONE_STR,
                                     sizeof(cur->driverType)) != 0);
      } else if (strncmp(driverType, cur->driverType, 
                         sizeof(cur->driverType)) == 0) {
         deleteCacheEntry = TRUE;
      }

      if (deleteCacheEntry) {
         VC_VMFSVolume *removeMe;

         if (prev == NULL) {
            VMFSVolumeList = cur->next;
         } else {
            prev->next = cur->next;
         }
         removeMe = cur;
         cur = cur->next;
         VCFreeVMFSPartition(removeMe);
      } else {
         prev = cur;
         cur = cur->next;
      }
   }
   SP_Unlock(&vcLock);

   status = FDS_RescanDevices(driverType, driverData);

   SP_Lock(&vcLock);
   vcRescanInProgress = FALSE;
   CpuSched_Wakeup((uint32)&vcRescanInProgress);
   SP_Unlock(&vcLock);

   FSS_EndRescan();
   
   /* Notify serverd of the rescan */
   memset(&args, 0, sizeof(args));
   args.validData = FALSE;
   VmkEvent_PostHostAgentMsg(VMKEVENT_VMFS, &args, sizeof(args));

   return status;
}
