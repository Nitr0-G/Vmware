/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * userLinux.h --
 *
 *	User World Linux-compatibility system calls
 */

#ifndef _USERLINUX_H
#define _USERLINUX_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "user_int.h"

struct VMKFullUserExcFrame;

typedef int (*User_SyscallHandler)(uint32, uint32, uint32, uint32,
                                   uint32, uint32);

extern User_SyscallHandler UserLinux_SyscallTable[];
extern int UserLinux_SyscallTableLen;
extern int UserLinux_UndefinedSyscall(uint32, uint32, uint32, uint32, uint32, uint32);

#endif /*_USERLINUX_H*/
