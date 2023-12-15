/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/* 
 * linuxSocket.h - 
 *	Linux compatibility socket-related syscalls.
 */

#ifndef VMKERNEL_USER_LINUXSOCKET_H
#define VMKERNEL_USER_LINUXSOCKET_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "vm_basic_types.h"

extern int LinuxSocket_Socketcall(uint32 what, void* userArgs);

#endif
