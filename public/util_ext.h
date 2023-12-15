/* **********************************************************
 * Copyright 2000 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * util_ext.h: This is the header file for vmkernel utility functions
 */

#ifndef _UTIL_EXT_H_
#define _UTIL_EXT_H_
 
#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMX
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_MODULE
#include "includeCheck.h"


/* This is an on-disk data structure. Do not modify it */
typedef struct UUID {
   uint32          timeLo;
   uint32          timeHi;
   uint16	   rand;
   uint8           macAddr[6];
} __attribute__ ((packed)) UUID;

/* provide implicit endpoint termination for string copy */
#define UTIL_STRNCPY_SAFE(dest, src, max) \
   do {                                   \
      strncpy(dest, src, max);            \
      dest[max-1] = 0;                    \
   } while (0)

#endif // _UTIL_EXT_H_
