/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * memsched_int.h --
 *
 *	World memory scheduler.
 */

#ifndef _MEMSCHED_INT_H
#define _MEMSCHED_INT_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

struct Sched_Group;

/*
 * internal operations
 */

void MemSched_SubTreeChanged(struct Sched_Group *group);
void MemSched_GroupGetAllocLocked(struct Sched_Group *group, 
				  Sched_Alloc *alloc);
VMK_ReturnStatus MemSched_GroupSetAllocLocked(struct Sched_Group *group, 
					      const Sched_Alloc *alloc);
VMK_ReturnStatus MemSched_SetupVmGroup(World_Handle *world,
		                       struct Sched_Group *group,
				       const Sched_Alloc *newAlloc);
void MemSched_CleanupVmGroup(World_Handle *world, struct Sched_Group *group);
VMK_ReturnStatus MemSched_AdmitGroup(const struct Sched_Group *group,
                                     const struct Sched_Group *parentGroup);

#endif // _MEMSCHED_INT_H
