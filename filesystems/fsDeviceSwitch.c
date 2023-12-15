/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * fsDeviceSwitch.c --
 *
 *    This is the file system device switch implementation. This abstracts
 *    out the physical storage device from file system specific
 *    implementation.
 *
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "x86.h"
#include "vmkernel.h"
#include "world.h"
#include "fss_int.h"
#include "memalloc.h"
#include "scattergather.h"
#include "libc.h"
#include "fsDeviceSwitch.h"
#include "volumeCache.h"
#include "host.h"
#include "scsi_vmware.h"

#define LOGLEVEL_MODULE FDS
#define LOGLEVEL_MODULE_LEN 3
#include "log.h"

typedef struct FDSRegisteredDriver {
   char driverType[FDS_MAX_DRIVERTYPE_LENGTH];
   FDS_DeviceOps *devOps;
   struct FDSRegisteredDriver *next;
} FDSRegisteredDriver;

// List of registered FS device drivers. **UNPROTECTED**, 'coz we register FS
// dd's at VMK init time and we don't support unregistering FS dd's.
static FDSRegisteredDriver *fdsDriverList = NULL;

void FDS_Init(void)
{
   // Kick the disk device driver so that it initializes and registers itself
   FSDisk_Init();

   // Initialize the file system switch
   FSS_Init();
}

int
FDS_RegisterDriver(const char *driverType, FDS_DeviceOps *devOps)
{
   FDSRegisteredDriver *driver;

   ASSERT(strlen(driverType) < FDS_MAX_DRIVERTYPE_LENGTH);
   /* Force the underlying layer to implement all device functions. It
    * doesn't matter if they are no-ops, but the handlers should be present.
    */
   ASSERT(devOps->FDS_OpenDevice && devOps->FDS_CloseDevice &&
          devOps->FDS_AsyncIO && devOps->FDS_SyncIO &&
          devOps->FDS_Ioctl && devOps->FDS_RescanDevices &&
          devOps->FDS_MakeDev);
          
   driver = (FDSRegisteredDriver *) Mem_Alloc(sizeof(*driver));
   if (driver == NULL) {
      return -1;
   }
   strncpy(driver->driverType, driverType, FDS_MAX_DRIVERTYPE_LENGTH);
   driver->devOps = devOps;

   driver->next = fdsDriverList;
   fdsDriverList = driver;
   Log("%s", driverType);
   Host_VMnixVMKDev(VMNIX_VMKSTOR_DRIVER, NULL, driver->driverType, NULL, 0, TRUE);
   return 0;
}

void
FDS_UnregisterDriver(FDS_DeviceOps *devOps)
{
   FDSRegisteredDriver *prev, *driver = fdsDriverList;

   for (prev = NULL, driver = fdsDriverList;
        driver != NULL;
        prev = driver, driver = driver->next) {
      if (driver->devOps == devOps) {
         Log("%s", driver->driverType);
         if (prev == NULL) {
            fdsDriverList = driver->next;
         } else {
            prev->next = driver->next;
         }
         break;
      }
   }

   if (driver != NULL) {
      VC_RescanVolumes(driver->driverType, NULL);
      Host_VMnixVMKDev(VMNIX_VMKSTOR_DRIVER, NULL, driver->driverType, NULL, 0, FALSE);
      Mem_Free(driver);
   }
}

VMK_ReturnStatus
FDS_OpenDevice(World_ID worldID, const char *deviceName, int flags,
               FDS_HandleID *handleID, FDS_DeviceOps **devOps)
{
   FDSRegisteredDriver *driver = fdsDriverList;

   while (driver != NULL) {
      VMK_ReturnStatus status;

      status = driver->devOps->FDS_OpenDevice(worldID, deviceName,
                                              flags, handleID);
      if (status == VMK_OK) {
         *devOps = driver->devOps;
         return status;
      } else {
         LOG(0, "%s returns %#x for %s", driver->driverType, 
             status, deviceName);
      }
      driver = driver->next;
   }
   return VMK_BAD_PARAM;
}

VMK_ReturnStatus
FDS_RescanDevices(const char *driverType, void *driverData)
{
   FDSRegisteredDriver *driver = fdsDriverList;

   ASSERT(driverType != NULL || driverData == NULL);
   while (driver != NULL) {
      if (driverType == NULL) {
         //A driver should be isolated from errors in other drivers. So
         //ignore errors and continue.
         driver->devOps->FDS_RescanDevices(driverData);
      } else if (strncmp(driverType, driver->driverType, 
                         sizeof(driver->driverType)) == 0) {
         return driver->devOps->FDS_RescanDevices(driverData);
      }
      driver = driver->next;
   }
   return VMK_OK;
}

VMK_ReturnStatus 
FDS_GetDriverType(const FDS_DeviceOps *devOps, char *driverType)
{
   FDSRegisteredDriver *driver = fdsDriverList;

   while (driver != NULL) {
      if (driver->devOps == devOps) {
         strcpy(driverType, driver->driverType);
         return VMK_OK;
      }
      driver = driver->next;
   }
   return VMK_NOT_FOUND;
}

VMK_ReturnStatus
FDS_MakeDev(VMnix_FDS_MakeDevArgs *args)
{
   FDSRegisteredDriver *driver = fdsDriverList;
   VMK_ReturnStatus status;
   
   while (driver != NULL) {
      if(!strcmp(driver->driverType, args->type)) {
         status = driver->devOps->FDS_MakeDev(args->name, args->numDiskBlocks,
                                 args->memBlockSize, args->imagePtr);
         if(status == VMK_OK) {
            VC_RescanVolumes(args->type, NULL);
            Host_VMnixVMKDev(VMNIX_VMKSTOR_DEVICE, args->name, driver->driverType, NULL, 
                             (((uint64)args->numDiskBlocks) << 32) | args->memBlockSize, 
                             TRUE);
         }	 
         return status;
      }
      driver = driver->next;
   }

   return VMK_NOT_FOUND;
}
/*
 * This is to be used only to detect snapshot Ids. If both these are
 * of type unique, then assume that it is the same and return FALSE,
 * else check 
 */
VMK_ReturnStatus
FDS_IsSnapshot(SCSI_DiskId *id1, SCSI_DiskId *id2)
{
   if(((id1->type == VMWARE_SCSI_ID_UNIQUE) && 
       (id2->type == VMWARE_SCSI_ID_UNIQUE)) || 
      ( id1->type == id2->type && id1->type != VMWARE_SCSI_ID_UNIQUE 
	 && id1->len == id2->len && id1->lun == id2->lun 
	 && memcmp(id1->id, id2->id, id1->len) == 0 )) {
      return FALSE;
   } else {
      return TRUE;
   }
}
