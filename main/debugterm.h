/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * debugterm.h --
 *
 *	Debug terminal specific functions.
 */

#ifndef _DEBUGTERM_H
#define _DEBUGTERM_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"


extern void DebugTerm_Init(void);
extern void DebugTerm_DisplayForBluescreen(void);


#endif
