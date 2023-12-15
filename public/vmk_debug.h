/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * debug.h --
 *
 *	Debugger support module.
 */

#ifndef _VMK_DEBUG_H
#define _VMK_DEBUG_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_DISTRIBUTE
#define INCLUDE_ALLOW_VMK_MODULE
#include "includeCheck.h"

static INLINE Bool Debug_InDebugger(void)
{
   extern volatile Bool debugInDebugger;
   return debugInDebugger;
}

extern Bool Debug_EverInDebugger(void);
extern Bool Debug_SerialDebugging(void);
extern Bool Debug_InCall(void);

#endif // _VMK_DEBUG_H

