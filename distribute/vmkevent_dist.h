/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * vmkevent_dist.h --
 *
 *	vmkevent specific functions for use in modules.
 */

#ifndef _VMKEVENT_DIST_H
#define _VMKEVENT_DIST_H

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_DISTRIBUTE
#define INCLUDE_ALLOW_VMK_MODULE
#include "includeCheck.h"

#include "vmkevent_ext.h"

void VmkEvent_AlertHelper(const char *file, uint32 line, VmkAlertMessage msg, 
                          const char *fmt, ...) PRINTF_DECL(4,5);

#define VmkEvent_PostAlert(_msgID, _fmt...) \
   VmkEvent_AlertHelper(__FUNCTION__, __LINE__, _msgID, _fmt)
#endif

