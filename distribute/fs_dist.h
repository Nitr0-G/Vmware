/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * fs_dist.h --
 *
 *	File system extensions.  Usable when opening VMFS files from
 *	an ESX service console process (future) or a userworld.
 */


#ifndef _FS_DIST_H
#define _FS_DIST_H

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_DISTRIBUTE
#include "includeCheck.h"

/*
 * Merge extra VMNIX_FILE_OPEN flags into flags for open() system call.
 *
 * XXX Move flag definitions here from vmnix_syscall.h?
 */
#define FS_OPEN_FLAGS_SHIFT 20
#define FS_OPEN_FLAGS_BITS  16
#define FS_OPEN_FLAGS_MASK  ((1 << FS_OPEN_FLAGS_BITS) - 1)
#define FS_OPEN_FLAGS_COMBINE(linuxFlags, fsFlags) \
  ((linuxFlags) | ((fsFlags) << FS_OPEN_FLAGS_SHIFT))
#define FS_OPEN_FLAGS_EXTRACT(combinedFlags) \
  (((combinedFlags) >> FS_OPEN_FLAGS_SHIFT) & FS_OPEN_FLAGS_MASK)

#endif // _VMFS_DIST_H
