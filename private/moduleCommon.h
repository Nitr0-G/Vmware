/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * moduleCommon.h
 *
 *	Common utility functions for modules.
 */

#ifndef VMKERNEL_MODULECOMMON_H
#define VMKERNEL_MODULECOMMON_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#define BITS_PER_HALFWORD (8 * (sizeof(int) / 2))

#define MAKE_VERSION(major,minor) (((major) << BITS_PER_HALFWORD) | (minor))
#define VERSION_MAJOR(version) ((version) >> BITS_PER_HALFWORD)
#define VERSION_MINOR(version) ((version) & 0x0000ffff)

#endif

