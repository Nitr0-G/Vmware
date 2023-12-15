/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * vscsi_ext.h --
 *
 *      VSCSI support in the vmkernel.
 */

#ifndef _VSCSI_EXT_H_
#define _VSCSI_EXT_H_

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMMEXT
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_MODULE
#include "includeCheck.h"
#include "return_status.h"
#include "vmk_basic_types.h"
#include "fs_ext.h"

typedef int32 VSCSI_HandleID;

#define VSCSI_MAX_DEVTYPE 4
#define VSCSI_INVALID_HANDLEID -1

typedef enum {
   VSCSI_FS = 0,
   VSCSI_COW,
   VSCSI_RAWDISK,
   VSCSI_RDMP,
} VSCSI_DevType;

typedef struct VSCSI_DevDescriptor {
   VSCSI_DevType        type;
   union {
      FS_FileHandleID   fid;
      COW_HandleID      cid;
      SCSI_HandleID     rawID;
   } u;
   uint32               vmkChannel;
} VSCSI_DevDescriptor;

#endif
