
/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * vmnix_if_dist.h --
 *      
 *      Constants for the vmnix-vmkernel interface.
 */

#ifndef _VMNIX_IF_DIST_H_
#define _VMNIX_IF_DIST_H_

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_VMK_MODULE
#define INCLUDE_ALLOW_DISTRIBUTE
#define INCLUDE_ALLOW_VMCORE
#include "includeCheck.h"


// Max length of a device name such as 'scsi0'
#define VMNIX_DEVICE_NAME_LENGTH	32
// Max length of a module name such as 'e1000'
#define VMNIX_MODULE_NAME_LENGTH	32
// Max length that can be read from a proc read
#define VMNIX_PROC_READ_LENGTH          32768
// Max length for hostname
#define VMNIX_HOSTNAME_LENGTH		64


// Possible use of screen
typedef enum VMnix_ScreenUse {
   VMNIX_SCREEN_STATUS = 0,
   VMNIX_SCREEN_LOG_THEN_STATUS,
   VMNIX_SCREEN_LOG,
} VMnix_ScreenUse;


#if VMNIX_DEVICE_NAME_LENGTH != 32
#error "VMNIX_MAX_DEVICE_NAME_LENGTH cannot be changed from 32"
#endif

 //Maximum length of a VMFS volume label, including the null byte.
#define FS_MAX_FS_NAME_LENGTH		32
#define FS_MAX_FILE_NAME_LENGTH		128
#define FS_MAX_PATH_NAME_LENGTH		2048
#define FS_MAX_VOLUME_NAME_LENGTH	(7+VMNIX_DEVICE_NAME_LENGTH+12)

#if FS_MAX_FS_NAME_LENGTH != VMNIX_DEVICE_NAME_LENGTH
#error "FS_MAX_FS_NAME_LENGTH and VMNIX_DEVICE_NAME_LENGTH should match."
#endif

#if FS_MAX_FS_NAME_LENGTH > FS_MAX_VOLUME_NAME_LENGTH
#error "FS_MAX_VOLUME_NAME_LENGTH should be bigger than FS_MAX_FS_NAME_LENGTH."
#endif

#if FS_MAX_VOLUME_NAME_LENGTH > FS_MAX_FILE_NAME_LENGTH
#error "FS_MAX_FILE_NAME_LENGTH should be bigger than FS_MAX_VOLUME_NAME_LENGTH."
#endif

#endif // ifndef _VMNIX_IF_DIST_H_
