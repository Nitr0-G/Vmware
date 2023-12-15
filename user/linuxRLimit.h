/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/* 
 * linuxRLimit.h - 
 *	Linux compatibility for resource limit-related syscalls
 */

#ifndef VMKERNEL_USER_LINUXRLIMIT_H
#define VMKERNEL_USER_LINUXRLIMIT_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

/* -- rlimit resources -- */

typedef int32 rlim_t; /* a guess */

/* From getrlimit(2) */
struct l_rlimit
{
   rlim_t rlim_cur;
   rlim_t rlim_max;
};

/* Resource types */
#define RLIMIT_CPU      0
#define RLIMIT_FSIZE    1
#define RLIMIT_DATA     2
#define RLIMIT_STACK    3
#define RLIMIT_CORE     4
#define RLIMIT_RSS      5
#define RLIMIT_NPROC    6
#define RLIMIT_NOFILE   7
#define RLIMIT_MEMLOCK  8
#define RLIMIT_AS       9

/* predefined resource values */
#define RLIM_INFINITY   -1

#endif /* VMKERNEL_USER_LINUXRLIMIT_H */

