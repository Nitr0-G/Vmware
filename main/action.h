/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * action.h --
 *
 *	VMKernel to VMM action queue.
 */

#ifndef _ACTION_H
#define _ACTION_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "action_ext.h"
#include "world.h"
#include "sched.h"
#include "prda.h"

void Action_Init(void);
VMK_ReturnStatus Action_WorldInit(struct World_Handle *world, World_InitArgs *args);
uint32 Action_Alloc(struct World_Handle *world, 
                    const char *name);

VMKERNEL_ENTRY  Action_CreateChannel(DECLARE_2_ARGS(VMK_CREATE_CHANNEL,
                                                    const char *, name,
                                                    uint32 *, actionIndex));
void Action_WorldCleanup(World_Handle *world);

VMKERNEL_ENTRY Action_InitVMKActions(DECLARE_2_ARGS(VMK_INIT_ACTIONS,
						    VA, actionStatusOff,
						    unit32, vmkActionIndex));
VMKERNEL_ENTRY Action_DisableVMKActions(DECLARE_ARGS(VMK_DISABLE_ACTIONS));
VMKERNEL_ENTRY Action_EnableVMKActions(DECLARE_ARGS(VMK_ENABLE_ACTIONS));

/*
 *----------------------------------------------------------------------
 *
 * Action_Set --
 *
 *      Set an the appropriate bits to mark an action as present.
 *      Order _is_ important:  The monitor loops clears
 *      the actionStatus field first, and then the vector.  This function
 *      sets in the opposite order to avoid lost actions.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
Action_Set(Action_Info *ai, uint32 index, uint32 vmkActionIndex) 
{
   ASSERT(index < NUM_ACTIONS);
   Atomic_Or(&ai->vector, 1 << index);
   Atomic_Or(ai->actionStatus, 1 << vmkActionIndex);
}

static INLINE Bool
Action_Present(Action_Info *ai, uint32 index) 
{
   ASSERT(index < NUM_ACTIONS);
   return (ai->vector.value & (1 << index));
}

static INLINE void
Action_ClearBit(Atomic_uint32 *vector, uint32 index) 
{
   ASSERT(index < NUM_ACTIONS);
   Atomic_And(vector, ~(1 << index));
}

static INLINE void
Action_Post(World_Handle *world, uint32 index)
{
   Action_Info *ai = &world->vmkSharedData->actions;

   ASSERT(index < NUM_ACTIONS);
   if ((index < NUM_ACTIONS) && !Action_Present(ai, index)) {
      World_VmmGroupInfo *vmmGroup = World_VMMGroup(world);

      Action_Set(ai, index, vmmGroup->vmkActionIndex);

      if (world != MY_RUNNING_WORLD) {
	 CpuSched_AsyncCheckActions(world);
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * Action_MonitorNotifyHint --
 *
 *      Sets action notification hint associated with "vcpuid" in
 *	"world".  If "notify" is TRUE, requests that the monitor
 *	performs a VMK_ACTION_NOTIFY_VCPU vmkcall whenever it posts
 *	an action to the vcpu identified by "vcpuid".
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
Action_MonitorNotifyHint(World_Handle *world, Vcpuid vcpuid, Bool notify)
{
   Action_Info *ai = &world->vmkSharedData->actions;

   ASSERT(vcpuid < MAX_VCPUS);
   if (vcpuid < MAX_VCPUS) {
      ai->notify.vcpuHint[vcpuid] = notify ? 1 : 0;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * Action_Pending --
 *
 *      Returns TRUE iff any action is currently pending for "world".
 *
 * Results:
 *      Returns TRUE iff any action is currently pending for "world".
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
Action_Pending(const World_Handle *world)
{
   return(Atomic_Read(world->vmkSharedData->actions.actionStatus) != 0);
}

/*
 *----------------------------------------------------------------------
 *
 * Action_PendingInMask --
 *
 *      Returns TRUE iff any action specified by "actionMask"
 *	is currently pending for "world".
 *
 * Results:
 *      Returns TRUE iff any action specified by "actionMask"
 *	is currently pending for "world".
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
Action_PendingInMask(const World_Handle *world, uint32 actionMask)
{
   return((Atomic_Read(world->vmkSharedData->actions.actionStatus) & actionMask) != 0);
}


#endif

