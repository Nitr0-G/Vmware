/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * cpusched_int.h --
 *
 *      World CPU scheduler.
 */

#ifndef _CPUSCHED_INT_H
#define _CPUSCHED_INT_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

struct Sched_Group;

/*
 * inline functions
 */

static INLINE struct World_Handle *
CpuSched_GetVsmpLeader(struct World_Handle *world)
{
   if (World_IsVMMWorld(world) || World_IsTESTWorld(world)) {
      return World_GetVmmLeader(world);
   } else {
      return world;
   }
}

static INLINE Bool
CpuSched_IsVsmpLeader(struct World_Handle *world)
{
   return world == CpuSched_GetVsmpLeader(world);
}

/*
 * internal operations
 */

void CpuSched_GroupStateInit(CpuSched_GroupState *s);
void CpuSched_GroupStateCleanup(CpuSched_GroupState *s);
void CpuSched_ProcGroupsRead(char *buf, int *len);
VMK_ReturnStatus CpuSched_GroupSetAllocLocked(struct Sched_Group *group, 
					      const Sched_Alloc *alloc);
VMK_ReturnStatus CpuSched_AdmitGroup(const struct Sched_Group *group,
		                     const struct Sched_Group *newParentGroup);
uint32 CpuSched_PercentTotal(void);

#endif
