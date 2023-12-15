/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * diskDriver.c --
 *
 *    This is the implementation of the "disk" file system device driver.
 *
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "x86.h"
#include "vmkernel.h"
#include "world.h"
#include "memalloc.h"
#include "scattergather.h"
#include "libc.h"
#include "fsDeviceSwitch.h"
#include "vmk_scsi.h"
#include "host.h"

#define LOGLEVEL_MODULE FSDisk
#define LOGLEVEL_MODULE_LEN 6
#include "log.h"

static void FSDisk_Register(void);
static VMK_ReturnStatus FSDisk_OpenDevice(World_ID worldID, 
                                          const char *deviceName, int flags,
                                          FDS_HandleID *deviceHandleID);
static VMK_ReturnStatus FSDisk_Ioctl(FDS_HandleID deviceHandleID,
                                     FDS_IoctlCmdType cmd, void *dataInOut);
static Bool FSDiskParseSCSIName(const char *devName, char **scsiName,
                                int *target, int *lun, int *partition,
                                char **fileName);
static VMK_ReturnStatus FSDisk_MakeDev(char* name, uint32 numDiskBlocks, 
                                       uint32 memBlockSize, char* imagePtr);
static INLINE VMK_ReturnStatus FSDisk_CloseDevice(World_ID worldID,
                                                  FDS_HandleID deviceHandleID);
static INLINE VMK_ReturnStatus FSDisk_SyncIO(FDS_HandleID deviceHandleID,
                                             SG_Array *sgArr, Bool isRead);
static INLINE VMK_ReturnStatus FSDisk_AsyncIO(FDS_HandleID deviceHandleID,
                                              SG_Array *sgArr,
                                              Bool isRead, Async_Token *token);

static FDS_DeviceOps fsDiskOps = {
   FSDisk_OpenDevice,
   FSDisk_CloseDevice,
   FSDisk_SyncIO,
   FSDisk_AsyncIO,
   FSDisk_Ioctl,
   SCSI_RescanDevices,
   FSDisk_MakeDev
};

/*
 *-----------------------------------------------------------------------------
 *
 * ParseNumber --
 *    Helper function to FSDiskParseSCSIName(). Similar to atoi().
 *
 * Results:
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
static int
ParseNumber(char *p)
{
   int r = 0;

   while (*p >= '0' && *p <= '9')
      r = r*10 + (*p++ - '0');
   return r;
}

/*
 *-----------------------------------------------------------------------------
 *
 * FSDiskParseSCSIName --
 *    Parse a scsi device name like 'vmhba1:2:0:3'.  May also include an
 *    extra colon or slash and a file name at the end.
 *
 * Results:
 *    TRUE if 'devName' can be successfully resolved into scsiName, target,
 *    lun, partition and fileName.
 *
 * Side effects:
 *    Memory referenced by *scsiName and *fileName needs to be freed on
 *    successful return.
 *
 *-----------------------------------------------------------------------------
 */
#define MAX_NUMBER_LENGTH	10
Bool
FSDiskParseSCSIName(const char *devName, char **scsiName, int *target,
                    int *lun, int *partition, char **fileName)
{
   const char *s;
   char *d;
   char diskName[VMNIX_DEVICE_NAME_LENGTH];
   char numberString[MAX_NUMBER_LENGTH + 1];   
   char *scsi = NULL;
   char *file = NULL;

   ASSERT(*scsiName == NULL);
   ASSERT(*fileName == NULL);

   s = devName;
   d = diskName;
   while (*s != ':' && *s != 0 && d - diskName < VMNIX_DEVICE_NAME_LENGTH-1) {
      *d++ = *s++;
   } 
   if (*s != ':' && *s != '\0') {
      return FALSE;
   }
   *d++ = 0;
   scsi = Mem_Alloc(d - diskName);
   if (scsi == NULL) {
      goto errorOut;
   }
   memcpy(scsi, diskName, d - diskName);

   if (*s == '\0' || strchr(s+1, ':') == NULL) {
      /* Allow user to specify just the VMFS volume label,
       * instead of devicename:target:partition. */
      *target = *partition = *lun = 0;
   } else {
      s++;
      d = numberString;
      while (*s >= '0' && *s <= '9' && d - numberString < MAX_NUMBER_LENGTH) {
	 *d++ = *s++;
      } 
      *d = 0;   
      *target = ParseNumber(numberString);

      if (*s != ':') {
         goto errorOut;
      }

      s++;

      d = numberString;
      while (*s >= '0' && *s <= '9' && d - numberString < MAX_NUMBER_LENGTH) {
	 *d++ = *s++;
      } 
      *d = 0;
      // this is the lun
      *lun = ParseNumber(numberString);

         // Now look for the partition number
      if (*s != ':') {
         goto errorOut;
      }
      s++;        
      d = numberString;
      while (*s >= '0' && *s <= '9' && d - numberString < MAX_NUMBER_LENGTH) {
         *d++ = *s++;
      } 
      *d = 0;

      *partition = ParseNumber(numberString);
   }

   if (*s == ':' || *s == '/') {
      int len = strlen(s+1) + 1;
      if (len > FS_MAX_FILE_NAME_LENGTH) {
	 goto errorOut;
      }
      file = Mem_Alloc(len);
      if (file == NULL) {
	 goto errorOut;
      }
      memcpy(file, s+1, len);
   } else if (*s != '\0') {
      goto errorOut;
   }
   *scsiName = scsi;
   *fileName = file;
   return TRUE;
errorOut:
   if (scsi) {
      Mem_Free(scsi);
   }
   if (file) {
      Mem_Free(file);
   }
   return FALSE;
}

void
FSDisk_Init(void)
{
   FSDisk_Register();
}

static void
FSDisk_Register(void)
{
   int retval;

   retval = FDS_RegisterDriver(SCSI_DISK_DRIVER_STRING, &fsDiskOps);
   if (retval != 0) {
      Warning("failed");
   }
}

static VMK_ReturnStatus
FSDisk_OpenDevice(World_ID worldID, const char *deviceName,
                  int flags, FDS_HandleID *deviceHandleID)
{
   char *adapName = NULL, *fileName = NULL;
   uint32 targetID, lun, partition;
   VMK_ReturnStatus status;
   SCSI_HandleID diskHandle;
   Bool ok;

   ok = FSDiskParseSCSIName(deviceName, &adapName, &targetID, &lun, &partition,
                          &fileName);
   if (!ok) {
      ASSERT(adapName == NULL);
      ASSERT(fileName == NULL);
      return VMK_INVALID_NAME;
   }
   ASSERT(adapName != NULL);
   if (fileName != NULL) {
      Mem_Free(fileName);
      Mem_Free(adapName);
      return VMK_INVALID_NAME;
   }
   status = SCSI_OpenDevice(worldID, adapName, targetID, lun, partition,
                            flags, &diskHandle);
   Mem_Free(adapName);
   if (status != VMK_OK) {
      return status;
   } else if (SCSI_GetTargetClass(diskHandle) != SCSI_CLASS_DISK) {
      SCSI_CloseDevice(worldID, diskHandle);
      return VMK_INVALID_TYPE;
   }
   *deviceHandleID = (FDS_HandleID)diskHandle;
   return VMK_OK;
}

static INLINE VMK_ReturnStatus
FSDisk_CloseDevice(World_ID worldID, FDS_HandleID deviceHandleID)
{
   return SCSI_CloseDevice(worldID, (SCSI_HandleID)deviceHandleID);
}

static INLINE VMK_ReturnStatus
FSDisk_SyncIO(FDS_HandleID deviceHandleID, SG_Array *sgArr, Bool isRead)
{
   return SCSI_SGIO((SCSI_HandleID)deviceHandleID, sgArr, isRead);
}

static INLINE VMK_ReturnStatus
FSDisk_AsyncIO(FDS_HandleID deviceHandleID, SG_Array *sgArr,
               Bool isRead, Async_Token *token)
{
   return SCSI_AsyncIO((SCSI_HandleID)deviceHandleID, sgArr,
                       isRead, token);
}

static VMK_ReturnStatus
FSDisk_Ioctl(FDS_HandleID deviceHandleID,
             FDS_IoctlCmdType cmd, void *dataInOut)
{
   switch (cmd) {
   case FDS_IOCTL_RESERVE_DEVICE:
      return SCSI_ReservePhysTarget(deviceHandleID, TRUE);
   case FDS_IOCTL_RELEASE_DEVICE:
      return SCSI_ReservePhysTarget(deviceHandleID, FALSE);
   case FDS_IOCTL_GET_CAPACITY:
      return SCSI_GetCapacity(deviceHandleID, 
                              (VMnix_GetCapacityResult *)dataInOut);
   case FDS_IOCTL_TIMEDWAIT:
   {
      SCSI_RetryStatus rstatus;

      SCSI_TimedWait(deviceHandleID, (Async_Token *)dataInOut, &rstatus);
      return VMK_OK;
   }
   case FDS_IOCTL_RESET_DEVICE:
      return SCSI_ResetPhysBus(deviceHandleID,
                               CONFIG_OPTION(DISK_USE_LUN_RESET));
   case FDS_IOCTL_ABORT_COMMAND:
      return SCSI_AbortCommand(deviceHandleID, (SCSI_Command *)dataInOut);
   case FDS_IOCTL_RESET_COMMAND:
      return SCSI_ResetCommand(deviceHandleID, (SCSI_Command *)dataInOut);
   case FDS_IOCTL_GET_TARGETINFO: 
         {  
            char *name;
            uint32 targetID, lun, partition, partitionType;
            VMK_ReturnStatus status;

           SCSI_QueryHandle((SCSI_HandleID)deviceHandleID, &name, &targetID, &lun, &partition, &partitionType);
   
   
       status = SCSI_GetTargetInfo(name, targetID, lun, (VMnix_TargetInfo *)dataInOut);
  
       return status;
         }
   case FDS_IOCTL_GET_PARTITION:
         {  
            char *name;
            uint32 targetID, lun, partition, partitionType;
            VMK_ReturnStatus status;

	    status = SCSI_QueryHandle((SCSI_HandleID)deviceHandleID, &name, 
				      &targetID, &lun, &partition, 
				      &partitionType);
	    if (VMK_OK == status) {
	       *((uint32*)dataInOut) = partition;
	    }
	    return status;
         }

   default:
      Log("%d not supported", cmd);
   }
   return VMK_BAD_PARAM;
}

static VMK_ReturnStatus
FSDisk_MakeDev(char* name, uint32 numDiskBlocks, 
              uint32 memBlockSize, char* imagePtr)
{
   return VMK_NOT_IMPLEMENTED;
}

void
FSDisk_RegisterDevice(const char* adapterName, uint16 targetID, uint16 lun,
                      uint32 numBlocks, uint32 blockSize)
{
   char name[VMNIX_DEVICE_NAME_LENGTH];
   snprintf(name, sizeof(name), "%s:%d:%d:0", adapterName, targetID, lun);
   Host_VMnixVMKDev(VMNIX_VMKSTOR_DEVICE, name, SCSI_DISK_DRIVER_STRING, NULL, 
                    (((uint64)numBlocks) << 32) | blockSize, TRUE);
}

void
FSDisk_UnregisterDevice(const char* adapterName, uint16 targetID, uint16 lun)
{
   char name[VMNIX_DEVICE_NAME_LENGTH];
   snprintf(name, sizeof(name), "%s:%d:%d:0", adapterName, targetID, lun);
   Host_VMnixVMKDev(VMNIX_VMKSTOR_DEVICE, name, SCSI_DISK_DRIVER_STRING, NULL, 
                    0, FALSE);
}

