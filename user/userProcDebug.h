/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * procDebug.h --
 *
 *      Userworld debugging through Proc nodes
 */

#ifndef _PROCDEBUG_H_
#define _PROCDEBUG_H_

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"
#include "proc.h"

// Proc read and write handlers for each cartel
extern int UserProcDebug_CartelProcRead(Proc_Entry *entry, char *buffer, int *len);
extern int UserProcDebug_CartelProcWrite(Proc_Entry *entry, char *buffer, int *len);

Proc_Entry procDebugDir;
#endif

