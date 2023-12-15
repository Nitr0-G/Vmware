/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * logterm.h --
 *
 *	Log terminal specific functions.
 */

#ifndef _LOGTERM_H
#define _LOGTERM_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"


extern void LogTerm_Init(void);
extern void LogTerm_LateInit(void);
extern void LogTerm_CatchUp(void);
extern void LogTerm_Display(void);
extern void LogTerm_DisplayForBluescreen(void);
extern void LogTerm_OffScreen(void);


#endif
