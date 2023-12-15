/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * vmkevent_ext.h
 *
 * Interaction between the module device driver and serverd
 */

#ifndef _VMKEVENT_H_
#define _VMKEVENT_H_

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_VMCORE
#include "includeCheck.h"

#include "vmnix_if_dist.h"

/*
 * vmkernel events.
 */
 
typedef enum VmkEventType { 
   VMKEVENT_NONE = 2047,

#define UEVENT(call) VMKEVENT_##call,
#include "vmkeventtable_ext.h"

   VMKEVENT_LAST
} VmkEventType;

#define NUM_VMKEVENT_TYPE (VMKEVENT_LAST - VMKEVENT_NONE)

typedef enum {
   VMK_ALERT_MSG_DUP_IP,
   VMK_ALERT_SYSALERT
} VmkAlertMessage;

typedef struct VmkEvent_Alert {
   VmkAlertMessage      msg;
   char                 messageTxt[128];
   char                 fnName[20];
   uint32               lineNumber;
} VmkEvent_Alert;

typedef struct VmkEvent_VmkLoadArgs {
   int load;
} VmkEvent_VmkLoadArgs;

typedef struct VmkEvent_VmkLoadModArgs {
   int load;
   char name[FS_MAX_FILE_NAME_LENGTH];
} VmkEvent_VmkLoadModArgs;

typedef struct VmkEvent_VmkNicStateModifiedArgs {
   char devName[VMNIX_DEVICE_NAME_LENGTH];
   char modName[VMNIX_MODULE_NAME_LENGTH];
   int linkSpeed;
   int linkState;
   int duplexity;
   Bool autoneg;
#define NIC_10_HALF_CAP        0x00000001
#define NIC_10_FULL_CAP        0x00000002
#define NIC_100_HALF_CAP       0x00000004
#define NIC_100_FULL_CAP       0x00000008
#define NIC_AUTONEG_CAP        0x00000010

#define NIC_LOOPBACK_STATE     0x00000100
#define NIC_XCEIVER_RESET      0x00000200
#define NIC_XCEIVER_DISCONNECT 0x00000400
#define NIC_AUTONEG_RESTART    0x00000800
#define NIC_COLL_TEST_ENABLED  0x00001000
#define NIC_AUTONEG_COMPLETE   0x00002000
   uint32 capAndState;
}VmkEvent_VmkNicStateModifiedArgs;

typedef struct VmkEvent_VMFSArgs {
   Bool validData;
   char volumeName[FS_MAX_VOLUME_NAME_LENGTH];
   char volumeLabel[FS_MAX_FS_NAME_LENGTH];
} VmkEvent_VMFSArgs;

typedef char DeviceName [VMNIX_DEVICE_NAME_LENGTH];

typedef struct VmkEvent_VmkUpdateDisks {
   Bool newDisks;
} VmkEvent_VmkUpdateDisksArgs;

#endif // ifndef _VMKEVENT_H_
