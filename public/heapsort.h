/*
 * heapsort.h --
 *
 * heapsort functions, based on BSD libc
 *
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 *
 * Note that the heapsort module itself (which does not include this header)
 * is Copyright 1991, 1993, by the Regents of the University of California.
 */

#ifndef _HEAPSORT
#define _HEAPSORT

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_USERLEVEL

#include "includeCheck.h"

typedef int		 cmp_t(const void *, const void *);

extern int heapsort(void* a, uint32 n, uint32 es, cmp_t *cmp, void* temporary);

#endif // _HEAPSORT
