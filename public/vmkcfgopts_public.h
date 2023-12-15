/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

#ifndef _VMKCFGOPTS_PUBLIC_H
#define _VMKCFGOPTS_PUBLIC_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

typedef struct VMnix_ConfigOptions {
#define VMKCFGOPT(_opt, _type, _func, _default, _desc) _type _opt;
#include "vmk_cotable.h"
#undef VMKCFGOPT
} VMnix_ConfigOptions;

#endif
