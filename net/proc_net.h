/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. 
 * -- VMware Confidential
 * **********************************************************/

/*
 * proc_net.h  --
 *
 *   procfs implementation for vmkernel networking.
 */

#ifndef _PROC_NET_H_
#define _PROC_NET_H_

VMK_ReturnStatus   ProcNet_ModInit(void);
void               ProcNet_ModCleanup(void);
Proc_Entry        *ProcNet_GetRootNode(void);
void               ProcNet_Register(Proc_Entry *entry, char *name, Bool isDirectory);
void               ProcNet_Remove(Proc_Entry *entry);

#endif // _PROC_NET_H_
