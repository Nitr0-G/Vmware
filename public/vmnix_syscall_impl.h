/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

#ifndef VMNIX_SYSCALL_IMPL_H
#define VMNIX_SYSCALL_IMPL_H

/*
 * vmnix_syscall_impl.h --
 *
 *	Include this file to define the '_vmnix' function.
 */

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_DISTRIBUTE
#include "includeCheck.h"

/*
 *-----------------------------------------------------------------------------
 *
 * _vmnix --
 *
 *      The Vmnix system call.
 *
 * Results:
 *      -1 on failure, with errno set.
 *
 * Side effects:
 *      A Vmnix system call is preformed.
 *
 *-----------------------------------------------------------------------------
 */

#define __NR__vmnix  271
static _syscall5(int, _vmnix, unsigned, cmd, void*, inBuffer, 
                 unsigned, inBufferLength, void*, outBuffer, 
                 unsigned, outBufferLength);

#endif
