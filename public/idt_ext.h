/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * idt_ext.h --
 */

#ifndef _IDT_EXT_H
#define _IDT_EXT_H

#define INCLUDE_ALLOW_VMCORE
#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#define NUM_DF_MPNS 3

typedef struct SetupDF {
   MPN root;              //IN to vmk
   MPN mpns[NUM_DF_MPNS]; //IN to vmk
   VA  eip;               //OUT to vmm
   VA  esp;               //OUT to vmm
} SetupDF;

typedef struct VMKIntInfo {
   unsigned monIPIVector;
   unsigned vmkTimerVector;
} VMKIntInfo;

#endif

