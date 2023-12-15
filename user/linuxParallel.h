/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * linuxParallel.h:
 *
 *    Linux-compatible parallel port support.
 */

#ifndef _VMKERNEL_USER_LINUX_PARALLEL_H
#define _VMKERNEL_USER_LINUX_PARALLEL_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

/*
 * Parallel port ioctls (byte 0x70)
 */
#define LINUX_PPCLAIM                 0x708b   /* Claim the port */
#define LINUX_PPRELEASE               0x708c   /* Release the port */
#define LINUX_PPYIELD                 0x708d   /* Yield the port */
#define LINUX_PPEXCL                  0x708f   /* Register device exclusively */

#endif // VMKERNEL_USER_LINUX_PARALLEL_H
