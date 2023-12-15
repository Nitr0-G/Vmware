/* **********************************************************
 * Copyright 2000 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * alloc_dist.h --
 *	This is the header file for machine memory manager.
 */


#ifndef _ALLOC_DIST_H
#define _ALLOC_DIST_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMK_MODULE
#define INCLUDE_ALLOW_DISTRIBUTE
#include "includeCheck.h"

typedef struct Alloc_Result {
   uint32               length;
   MA                   maddr;
   void                 *vaddr;
} Alloc_Result;

#endif
