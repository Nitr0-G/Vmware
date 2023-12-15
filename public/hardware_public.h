/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * hardware_public.h --
 *
 * 	Public definitions for the hardware module.
 */
#ifndef  _HARDWARE_PUBLIC_H
#define _HARDWARE_PUBLIC_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMMEXT
#include "includeCheck.h"

#define DMI_UUID_SIZE   16

/*
 * NOTE:
 * VMFS 3 locking is BIOS id based so the VMFS 3 lock called
 * FS3_DiskLock depends on Hardware_DMIUUID size and property.
 * Before changing the Hardware_DMIUUID make sure to read the
 * fs3_int.h header file. Also take a look at FS3_DiskBlock union
 * which can't be larger that 512 bytes.
 */
typedef struct {
   char UUID[DMI_UUID_SIZE];
} Hardware_DMIUUID;

#endif
