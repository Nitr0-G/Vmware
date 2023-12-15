/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * volumeCache.h --
 *
 *      VMFS volume cache handling
 */

#ifndef _VOLUMECACHE_H_
#define _VOLUMECACHE_H_

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "fsDeviceSwitch.h" // for FDS_MAX_DRIVERTYPE_LENGTH
#include "fs_ext.h"         // for FSS_ObjectID

// Driver string for FS drivers that don't operate on Direct Connect Storage
#define VC_DRIVERTYPE_NONE_STR	"notDCS"

typedef struct VC_VMFSVolume {
   struct VC_VMFSVolume	     *next;
   VMnix_PartitionListResult *fsAttrs;
   char                      driverType[FDS_MAX_DRIVERTYPE_LENGTH];
} VC_VMFSVolume;

extern void VC_Init(void);

extern void VC_SetName(const char *volumeName, const char *fsName);

extern VMK_ReturnStatus VC_UpdateVMFSVolume(const VMnix_PartitionListResult *result,
                                            const char *driverType,
                                            Bool calledFromRescan);

extern VC_VMFSVolume *VC_FindVMFSVolume(const char *volumeName,
                                        Bool longSearch);
extern VC_VMFSVolume *VC_FindVMFSVolumeByUUID(const UUID *uuid);
extern void VC_ReleaseVMFSVolume(const VC_VMFSVolume *pt);

extern VMK_ReturnStatus VC_RescanVolumes(const char *driverType, void *driverData);

extern VMK_ReturnStatus VC_Readdir(uint32 maxEntries,
                                   VMnix_ReaddirResult *result);
extern VMK_ReturnStatus VC_Lookup(const char *name, FSS_ObjectID *volumeRootDirOID);
extern VMK_ReturnStatus VC_GetFileAttributes(FS_FileAttributes *attrs);
#endif //_VOLUMECACHE_H_
