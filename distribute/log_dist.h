/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * log_dist.h --
 *
 *	vmkernel logging macros
 */

#ifndef _LOG_DIST_H
#define _LOG_DIST_H

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_DISTRIBUTE
#define INCLUDE_ALLOW_VMK_MODULE
#include "includeCheck.h"
#include "vm_assert.h"

#define _LogPrefix ""
#define _WarningPrefix ""
#define _SysAlertPrefix ""
#include "log_defines.h"

typedef enum {
#define LOGLEVEL_DEF(name, len) LOGLEVEL_##name,
#include "logtable_dist.h"
} LogExternalVals;

extern int logLevelPtr[];

extern Bool Panic_IsSystemInPanic(void);


#endif
