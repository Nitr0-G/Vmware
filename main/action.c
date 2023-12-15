/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * action.c --
 *
 *	Handle list of vmkernel to vmm actions.
 */
#include "vm_types.h"
#include "vm_libc.h"
#include "vm_asm.h"
#include "vmkernel.h"
#include "action.h"
#include "kvmap.h"
#include "world.h"
#include "prda.h"
#include "memalloc.h"
#include "sharedArea.h"

#define LOGLEVEL_MODULE Action
#define LOGLEVEL_MODULE_LEN 6
#include "log.h"

static SP_SpinLockIRQ actionLock;

static Atomic_uint32 dummyVector;

void
Action_Init(void)
{
   SP_InitLockIRQ("actionLck", &actionLock, SP_RANK_IRQ_LEAF);
}

/*
 *----------------------------------------------------------------------
 *
 * Action_WorldInit --
 *
 *      Initializes the per world action data structures. 
 *      Make actionStatus point to a dummy variable for now -- this will 
 *      be changed once the monitor tells us where its actionStatus 
 *      variable is located (see Action_InitVMKActions below)
 *
 * Results:
 *      none.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
Action_WorldInit(World_Handle *world, UNUSED_PARAM(World_InitArgs *args))
{
   Action_Info *ai = &world->vmkSharedData->actions;

   ai->actionStatus = &dummyVector;
   ai->vector.value = 0;

   return VMK_OK;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Action_WorldCleanup
 *
 *    Resets actionStatus state.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
void
Action_WorldCleanup(World_Handle *world)
{
   Action_Info *ai = &world->vmkSharedData->actions;

   ai->actionStatus = &dummyVector;
   ai->actionStatusMapped = NULL;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Action_Alloc --
 *
 *     Allocates a new action with the same action index on all worlds
 *     within a group.
 *
 * Results:
 *     the index of the action created, or ACTION_INVALID on
 *     failure.
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
uint32
Action_Alloc(World_Handle *world, const char *name)
{
   uint32 index;
   World_VmmGroupInfo *vmmGroup;
   SP_IRQL prevIRQL = SP_LockIRQ(&actionLock, SP_IRQL_KERNEL);

   vmmGroup = World_VMMGroup(world);

   // check for existing action
   for (index = 0; index < vmmGroup->nextAction; index++) {
      if (strncmp(vmmGroup->action[index], name, MAX_ACTION_NAME_LEN) == 0) {
         SP_UnlockIRQ(&actionLock, prevIRQL);
         return (index);
      }
   }

   if (vmmGroup->nextAction == NUM_ACTIONS) {
      VmWarn(world->worldID, "Out of free action entries");
      index = ACTION_INVALID;
   } else {
      index = vmmGroup->nextAction++;
      strncpy(vmmGroup->action[index], name, MAX_ACTION_NAME_LEN);
   }

   SP_UnlockIRQ(&actionLock, prevIRQL);

   VMLOG(1, world->worldID, "Action #%d allocated", index);
   return (index);
}


/*
 *-----------------------------------------------------------------------------
 *
 * Action_InitVMKActions
 *
 *    Points actions.actionStatus to a designated location (within the 
 *    shared area) as specified by the monitor and records the bit vector
 *    to be used for posting VMK actions.
 *   
 *    Also checks for actions posted by the vmkernel prior to initializing 
 *    actionStatus and sets the appropriate bits in the mapped action status 
 *    field.
 *
 * Results:
 *      a VMK_ReturnStatus value is return in status.
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
VMKERNEL_ENTRY
Action_InitVMKActions(DECLARE_2_ARGS(VMK_INIT_ACTIONS,
				     VA, actionStatusOff,
				     uint32, vmkActionIndex))
{
   World_Handle *world;
   Action_Info *ai;
   World_VmmGroupInfo *vmmGroup;
   VA baseAddr;
   PROCESS_2_ARGS(VMK_INIT_ACTIONS, 
		  VA, actionStatusOff, uint32, vmkActionIndex);

   world = MY_RUNNING_WORLD;
   ai = &world->vmkSharedData->actions;

   /*
    * Use the offset into the shared area to compute the final designated 
    * address for actionStatus. This address, saved in actionStatusMapped, 
    * is used to initialize actionStatus whenever VMK actions are enabled.
    */
   baseAddr = (VA)SharedArea_GetBase(world);
   ASSERT(baseAddr != (VA)NULL);

   ASSERT(ai->actionStatusMapped == NULL);
   ai->actionStatusMapped = (Atomic_uint32 *)(baseAddr + actionStatusOff);

   /*
    * Enable VMK actions (by default).
    */
   ai->actionStatus = ai->actionStatusMapped;

   /*
    * Save the actionStatus bit vector for later use. We expect that 
    * the same bit vector will be used for all worlds belonging to a 
    * group.
    */
   vmmGroup = World_VMMGroup(world);
   ASSERT((vmmGroup->vmkActionIndex == 0) ||
          (vmmGroup->vmkActionIndex == vmkActionIndex));
   vmmGroup->vmkActionIndex = vmkActionIndex;

   /*
    * Actions could have been posted before actionStatus was pointing to
    * the correct location. Check if there are pending actions, and
    * set the appropriate bit in actionStatus.
    */
   if (Atomic_Read(&ai->vector)) {
      Atomic_Or(ai->actionStatus, 1 << vmmGroup->vmkActionIndex);
   }

   return VMK_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 * Action_DisableVMKActions
 *
 *    Disable posting of any new actions to the current world.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
VMKERNEL_ENTRY 
Action_DisableVMKActions(DECLARE_ARGS(VMK_DISABLE_ACTIONS))
{
   Action_Info *ai = &MY_RUNNING_WORLD->vmkSharedData->actions;
   PROCESS_0_ARGS(VMK_DISABLE_ACTIONS);

   ai->actionStatus = &dummyVector;
   return VMK_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 * Action_EnableVMKActions
 *
 *    Enable posting of new actions to the current world.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
VMKERNEL_ENTRY 
Action_EnableVMKActions(DECLARE_ARGS(VMK_ENABLE_ACTIONS))
{
   Action_Info *ai = &MY_RUNNING_WORLD->vmkSharedData->actions;
   PROCESS_0_ARGS(VMK_ENABLE_ACTIONS);

   ASSERT(ai->actionStatusMapped);
   ai->actionStatus = ai->actionStatusMapped;
   return VMK_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 * Action_CreateChannel
 *
 *    Create a channel to send message to the monitor
 *    We allocate an action with the same index on every member of 
 *    this world group. Thus, this should be used after the world
 *    group has reached its final size.
 *
 * Results:
 *    none.
 *
 * Side effects:
 *    Allocate an action
 *
 *-----------------------------------------------------------------------------
 */
VMKERNEL_ENTRY 
Action_CreateChannel(DECLARE_2_ARGS(VMK_CREATE_CHANNEL,
                                    const char *, name,
                                    uint32 *, actionIndex))
{
   PROCESS_2_ARGS(VMK_CREATE_CHANNEL,
                  const char *, name,
                  uint32 *, actionIndex);
   *actionIndex = Action_Alloc(MY_RUNNING_WORLD, name);
   return VMK_OK;
}
