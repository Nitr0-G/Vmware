/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * proc_dist.h --
 *
 *	Proc module.
 */

#ifndef _PROC_DIST_H
#define _PROC_DIST_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_DISTRIBUTE
#define INCLUDE_ALLOW_VMK_MODULE
#include "includeCheck.h"

typedef struct Proc_Entry Proc_Entry;

typedef enum {
   PROC_PRIVATE = -1,
   PROC_ROOT = 0,
   PROC_ROOT_DRIVER,
   PROC_ROOT_NET,
   PROC_MAX_PREDEF
} Proc_LinuxParent;

typedef int (*Proc_Read)(Proc_Entry *entry, char *buffer, int *len);
typedef int (*Proc_Write)(Proc_Entry *entry, char *buffer, int *len);

struct Proc_Entry {
   Proc_Read   	read;
   Proc_Write  	write;
   Proc_Entry *parent;
   Bool		canBlock;
   void       	*private;
   uint32       guid;
   volatile int refCount;
   Bool        hidden;
   Bool        cyclic;
};

extern void Proc_Register(Proc_Entry *entry, char *name, Bool isDirectory);
extern void Proc_RegisterLinux(Proc_Entry *entry, char *name, 
			       Proc_LinuxParent linuxParent, Bool isDirectory);
extern VMK_ReturnStatus Proc_Remove(Proc_Entry *entry);

extern void Proc_Printf(char *buffer, int *len,
                        const char *format, ...) PRINTF_DECL(3, 4);

#endif
