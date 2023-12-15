/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * proc.h --
 *
 *	Proc module.
 */

#ifndef _PROC_H
#define _PROC_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"
#include "proc_dist.h"
#include "helper_ext.h"

struct VMnix_SharedData;


extern void Proc_Init(struct VMnix_SharedData *sharedData);
extern void Proc_InitEntry(Proc_Entry *entry);
extern VMK_ReturnStatus Proc_HandleRead(int entryNum,
			    Helper_RequestHandle *hostHelperHandle);
extern VMK_ReturnStatus Proc_HandleWrite(int entryNum,
			     Helper_RequestHandle *hostHelperHandle);
extern VMK_ReturnStatus Proc_UpdateRequested(void);
extern void Proc_RegisterHidden(Proc_Entry *entry,
                                char *name,
                                Bool isDirectory);
extern VMK_ReturnStatus Proc_VerboseConfigChange(Bool write, Bool valueChanged, int idx);

#endif
