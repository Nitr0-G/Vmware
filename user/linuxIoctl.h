/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * linuxIoctl.h:
 *
 *    Linux-compatible ioctl support.
 */

#ifndef _VMKERNEL_USER_LINUX_IOCTL_H
#define _VMKERNEL_USER_LINUX_IOCTL_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

/*
 * fs ioctls (byte 0x12)
 */
#define LINUX_BLKGETSIZE              0x1260   /* Block device size */
#define LINUX_BLKSSZGET               0x1268   /* Block device sector size */

/*
 * file i/o ioctls (byte 0x54)
 */
#define LINUX_FIONREAD                0x541b   /* get # bytes in receive buffer */

#endif // VMKERNEL_USER_LINUX_IOCTL_H
