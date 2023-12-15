/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * vmkevent.h --
 *
 *	vmkernel->vmx usercalls
 */

#ifndef _VMKEVENT_VMK_H
#define _VMKEVENT_VMK_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "vmkevent_dist.h"

VMK_ReturnStatus VmkEvent_PostVmxMsg(World_ID vmmWorldID, VmkEventType function,
                                     void *data, int dataLen);
VMK_ReturnStatus VmkEvent_PostHostAgentMsg(VmkEventType function, void *data,
                                           int dataLen);
#endif
