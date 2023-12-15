/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * statusterm.h --
 *
 *	Status terminal specific functions.
 */

#ifndef _STATUSTERM_H
#define _STATUSTERM_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"


extern void StatusTerm_Init(VMnix_ScreenUse screenUse);
extern void StatusTerm_Printf(const char *fmt, ...);
extern void StatusTerm_PrintAlert(const char *message);
extern VMK_ReturnStatus StatusTerm_HostNameCallback(Bool write, Bool changed, int idx);
extern VMK_ReturnStatus StatusTerm_StopShowingProgress(Bool write, Bool changed, int idx);


#endif
