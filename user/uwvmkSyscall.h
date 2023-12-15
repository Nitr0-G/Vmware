/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * uwvmkSyscall.h --
 *
 *	UW -> VMKernel syscall headers
 */

#ifndef UWVMKSYSCALLS_H
#define UWVMKSYSCALLS_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "user_int.h"
#include "uwvmkDispatch.h"

struct VMKFullUserExcFrame;

extern void UWVMKSyscall_Undefined(struct VMKFullUserExcFrame*);

#endif /* UWVMKSYSCALLS_H */
