/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential

 * **********************************************************/

/*
 * fsDeviceSwitch.h --
 *
 *	vmkernel file system device switch interface and exported functions.
 */

#ifndef _FSDEVICESWITCH_H
#define _FSDEVICESWITCH_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "scattergather.h"
#include "async_io.h"
#include "return_status.h"
#include "world_ext.h"
#include "fsSwitch.h"

#define FDS_MAX_DRIVERTYPE_LENGTH	8

#define FDS_INVALID_DEVICE_HANDLE	(-1)
typedef int64 FDS_HandleID;

typedef VMK_ReturnStatus (*FDS_OpenDeviceOp)(World_ID worldID, const char *deviceName, int flags, FDS_HandleID *handleID);
typedef VMK_ReturnStatus (*FDS_CloseDeviceOp)(World_ID worldID, FDS_HandleID fdsHandleID);
typedef VMK_ReturnStatus (*FDS_SyncIOOp)(FDS_HandleID fdsHandleID, SG_Array *sgArr, Bool isRead);
typedef VMK_ReturnStatus (*FDS_AsyncIOOp)(FDS_HandleID fdsHandleID, SG_Array *sgArr, Bool isRead, Async_Token *token);
typedef VMK_ReturnStatus (*FDS_IoctlOp)(FDS_HandleID fdsHandleID, FDS_IoctlCmdType cmd, void *dataInOut);
typedef VMK_ReturnStatus (*FDS_RescanDevicesOp)(void *driverData);
typedef VMK_ReturnStatus (*FDS_MakeDevOp)(char* name, uint32 numDiskBlocks, uint32 memBlockSize, char* imagePtr);

typedef struct {
   FDS_OpenDeviceOp FDS_OpenDevice;
   FDS_CloseDeviceOp FDS_CloseDevice;
   FDS_SyncIOOp FDS_SyncIO;
   FDS_AsyncIOOp FDS_AsyncIO;
   FDS_IoctlOp FDS_Ioctl;
   FDS_RescanDevicesOp FDS_RescanDevices;
   FDS_MakeDevOp FDS_MakeDev;
} FDS_DeviceOps;

typedef struct FDS_Handle {
   FDS_HandleID hid;
   FDS_DeviceOps *devOps;
} FDS_Handle;

extern void FDS_Init(void);
extern int FDS_RegisterDriver(const char *deviceType, FDS_DeviceOps *devOps);
extern void FDS_UnregisterDriver(FDS_DeviceOps *devOps);
extern VMK_ReturnStatus FDS_OpenDevice(World_ID worldID,
                                       const char *deviceName, int flags,
                                       FDS_HandleID *handleID,
                                       FDS_DeviceOps **devOps);
extern VMK_ReturnStatus FDS_RescanDevices(const char *driverType,
                                          void *driverData);
extern VMK_ReturnStatus FDS_GetDriverType(const FDS_DeviceOps *devOps,
                                          char *driverType);
extern VMK_ReturnStatus FDS_MakeDev(VMnix_FDS_MakeDevArgs *args);
extern VMK_ReturnStatus FDS_IsSnapshot(SCSI_DiskId *id1, SCSI_DiskId *id2);

// Storage device initialization function. Here because disk driver is not a
// module (and we don't plan to make it one), unlike memdriver and filedriver.
extern void FSDisk_Init(void);

#endif //_FSDEVICESWITCH_H
