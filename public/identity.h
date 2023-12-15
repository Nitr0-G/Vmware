/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/* 
 * identity.h - 
 *	World identity for access control purposes.
 */

#ifndef VMKERNEL_IDENTITY_H
#define VMKERNEL_IDENTITY_H

#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "vm_basic_types.h"
#include "return_status.h"

typedef uint32 Identity_UserID;
typedef uint32 Identity_GroupID;
#define IDENTITY_NGROUPS_MAX 32

typedef struct Identity {
   Identity_UserID  ruid, euid, suid; // real, effective, and saved uid
   Identity_GroupID rgid, egid, sgid; // real, effective, and saved gid
   unsigned         ngids;
   Identity_GroupID gids[IDENTITY_NGROUPS_MAX]; // supplementary gids
} Identity;

extern void Identity_Copy(Identity *dst, const Identity *src);

struct World_Handle;
struct World_InitArgs;
extern VMK_ReturnStatus Identity_WorldInit(struct World_Handle *world,
                                           struct World_InitArgs *args);

extern void Identity_WorldCleanup(struct World_Handle *world);

#endif // VMKERNEL_IDENTITY_H
