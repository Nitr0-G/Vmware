/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/



/*
 * hardware.h --
 *
 * 	This is the header for the hardware module.
 */

#ifndef _HARDWARE_H
#define _HARDWARE_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "hardware_public.h"
#include "vmnix_syscall.h"
#include "vmksysinfoInt.h"


EXTERN void Hardware_Init(VMnix_Init *vmnixInit);
EXTERN VMK_ReturnStatus Hardware_GetUUID(Hardware_DMIUUID *uuid);
EXTERN VMK_ReturnStatus Hardware_GetInfo(VMnix_HardwareInfoArgs *args, VMnix_HardwareInfoResult *result, unsigned long resultLen);

#endif
