/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * sched.h --
 *
 *	World resource scheduling.
 */

#ifndef _SCHED_H
#define _SCHED_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "splock.h"
#include "list.h"

#include "sched_dist.h"
#include "sched_ext.h"
#include "cpusched.h"
#include "memsched.h"

/*
 * Types
 */

typedef struct {
   struct Sched_Node *node;
   Sched_GroupID groupID;
   Sched_GroupPath path;
   Bool cpuValid;			// valid wrt CpuSched?
   Bool memValid;			// valid wrt MemSched?
} Sched_ClientGroup;

typedef struct {
   List_Links		links;		// for lists, must come first
   Sched_ClientGroup	group;		// scheduler tree group state
   CpuSched_Client	cpu;		// cpu scheduling state
   Sched_CpuClientConfig cpuConfig;
} Sched_Client;

struct World_InitArgs;

/*
 * Operations
 */

// initialization
void Sched_Init(uint32 cellSize);

// client configuration
void Sched_ConfigInit(Sched_ClientConfig *config, const char *groupName);
void Sched_ConfigSetCpuAffinity(Sched_ClientConfig *config, CpuMask affinity);
void Sched_ConfigSetCpuMinPct(Sched_ClientConfig *config, int32 minPercent);
const char *Sched_UnitsToString(Sched_Units units);
Sched_Units Sched_StringToUnits(const char *ustr);

// add, remove worlds
void Sched_AddRunning(void);
VMK_ReturnStatus Sched_Add(struct World_Handle *world,
                           CpuSched_StartFunc startFunc,
                           void *startData);
VMK_ReturnStatus Sched_Remove(struct World_Handle *world);
VMK_ReturnStatus Sched_WorldGroupInit(struct World_Handle *world, struct World_InitArgs *args);
VMK_ReturnStatus Sched_WorldGroupCleanup(struct World_Handle *world);
VMK_ReturnStatus Sched_WorldInit(struct World_Handle *world, struct World_InitArgs *args);
void Sched_WorldCleanup(struct World_Handle *world);

// group identity
VMK_ReturnStatus Sched_GroupIDToName(Sched_GroupID id,
                                     char *nameBuf,
                                     uint32 nameBufLen);
Sched_GroupID Sched_GroupNameToID(const char *name);

// group paths
Bool Sched_GroupPathEqual(const Sched_GroupPath *a, const Sched_GroupPath *b);
void Sched_GroupPathCopy(Sched_GroupPath *to, const Sched_GroupPath *from);
void Sched_GroupPathSetRoot(Sched_GroupPath *path);
void Sched_GroupPathInvalidate(Sched_GroupPath *path);
void Sched_LookupGroupPath(struct World_Handle *world, Sched_GroupPath *path);

// add, remove groups
VMK_ReturnStatus Sched_AddGroup(const char *name,
                                Sched_GroupID parentID,
                                Sched_GroupID *groupID);
VMK_ReturnStatus Sched_RemoveGroup(Sched_GroupID id);

// join, leave groups
VMK_ReturnStatus Sched_JoinGroup(struct World_Handle *world, Sched_GroupID id);
void Sched_LeaveGroup(struct World_Handle *world);
VMK_ReturnStatus Sched_ChangeGroup(struct World_Handle *world, Sched_GroupID id);

#endif
