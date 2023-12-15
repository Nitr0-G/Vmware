/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * vmkpoll.h --
 *
 *	Functions to manage list of worlds waiting for poll event
 */

#ifndef _VMKPOLL_H
#define _VMKPOLL_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "list.h"
#include "world_ext.h"

typedef enum VMKPollEvent {
   VMKPOLL_NONE   = 0x00, // initialization / clear
   VMKPOLL_READ   = 0x01,
   // save 0x2 for "priority" reads if we implement them
   VMKPOLL_WRITE  = 0x04,
   VMKPOLL_INMASK = (VMKPOLL_READ|VMKPOLL_WRITE),

   // Output/result flags (in addition to READ/WRITE):
   VMKPOLL_RDHUP  = 0x08, // no readers when polling for write
   VMKPOLL_WRHUP  = 0x10, // no writers when polling for read
   VMKPOLL_INVALID= 0x20, // invalid FD
   VMKPOLL_ERRMASK= (VMKPOLL_RDHUP|VMKPOLL_WRHUP|VMKPOLL_INVALID),
} VMKPollEvent;

#define VMKPOLL_WAITLIST_MAGIC 0xee504f4c // 0xee"POL"

typedef struct VMKPollWaitersList {
   List_Links list;
   DEBUG_ONLY(SP_SpinLock *lock;)
   DEBUG_ONLY(uint32 magic;)
} VMKPollWaitersList;

typedef struct VMKPollWaiter {
   List_Links links; // must be the first entry
   World_ID   worldID;
   VMKPollEvent events;
} VMKPollWaiter;


/*
 *----------------------------------------------------------------------
 *
 * VMKPoll_InitList --
 *
 *      Initialize a list of PollWaiters
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static INLINE void
VMKPoll_InitList(VMKPollWaitersList *waiters, SP_SpinLock *lock)
{
   List_Init(&waiters->list);
   DEBUG_ONLY(waiters->lock = lock);
   DEBUG_ONLY(waiters->magic = VMKPOLL_WAITLIST_MAGIC);
}

/*
 *----------------------------------------------------------------------
 *
 * VMKPollCheckValidAndLocked --
 *
 *      Check if list is valid and locked
 *
 * Results:
 *      None
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static INLINE void
VMKPollCheckValidAndLocked(VMKPollWaitersList *waiters)
{
   ASSERT(waiters->magic == VMKPOLL_WAITLIST_MAGIC);
   ASSERT((waiters->lock == NULL) || SP_IsLocked(waiters->lock));
}


/*
 *----------------------------------------------------------------------
 *
 * VMKPollFindWaiter --
 *
 *      Internal function to find the given worldID is in the waiters list.
 *      If found, return the associated VMKPollWaiter struct
 *
 * Results:
 *      VMKPollWaiter struct for world or NULL
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static INLINE VMKPollWaiter *
VMKPollFindWaiter(VMKPollWaitersList *waiters, World_ID worldID)
{
   VMKPollWaiter *waiter;
   List_Links *item;
   LIST_FORALL(&waiters->list, item) {
      waiter = (VMKPollWaiter *)item;
      if (waiter->worldID == worldID) {
         return waiter;
      }
   }
   return NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * VMKPoll_HasWaiters --
 *
 *	Determines if the given waiter list has any waiters on it.
 *
 * Results:
 *	TRUE if there are waiters present, FALSE otherwise.
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
VMKPoll_HasWaiters(VMKPollWaitersList *waiters)
{
   VMKPollCheckValidAndLocked(waiters);
   return !List_IsEmpty(&waiters->list);
}

/*
 *----------------------------------------------------------------------
 *
 * VMKPoll_AddWaiterForEvent --
 *
 *      Add this given worldID to the list of worlds waiting for the
 *	specified poll event(s)
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static INLINE void
VMKPoll_AddWaiterForEvent(VMKPollWaitersList *waiters, World_ID worldID,
			  VMKPollEvent events)
{
   VMKPollCheckValidAndLocked(waiters);
   if (!VMKPollFindWaiter(waiters, worldID)) {
      VMKPollWaiter *newWaiter = (VMKPollWaiter *)Mem_Alloc(sizeof *newWaiter);
      ASSERT_NOT_IMPLEMENTED(newWaiter);

      List_InitElement(&newWaiter->links);
      newWaiter->worldID = worldID;
      newWaiter->events = events;
      List_Insert(&newWaiter->links, LIST_ATREAR(&waiters->list));
   }
}

/*
 *----------------------------------------------------------------------
 *
 * VMKPoll_AddWaiter --
 *
 *      Add this given worldID to the list of worlds waiting for any poll event
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static INLINE void
VMKPoll_AddWaiter(VMKPollWaitersList *waiters, World_ID worldID)
{
   VMKPoll_AddWaiterForEvent(waiters, worldID, VMKPOLL_INMASK);
}

/*
 *----------------------------------------------------------------------
 *
 * VMKPoll_RemoveWaiter --
 *
 *      Remove this given worldID from the list of worlds waiting for poll event
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static INLINE void
VMKPoll_RemoveWaiter(VMKPollWaitersList *waiters, World_ID worldID)
{
   VMKPollWaiter *waiter;
   VMKPollCheckValidAndLocked(waiters);
   waiter = VMKPollFindWaiter(waiters, worldID);
   if (waiter != NULL) {
      List_Remove(&waiter->links);
      Mem_Free(waiter);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * VMKPoll_MoveWaiters --
 *
 *      Move all entries in list of waiters to new list and clear the
 *      original list.  Also return whether there are any waiters in the
 *      list.
 *
 * Results:
 *      TRUE if there are waiters on the list, FALSE otherwise
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
VMKPoll_MoveWaiters(VMKPollWaitersList *waiters, VMKPollWaitersList *newWaiters)
{
   VMKPollCheckValidAndLocked(waiters);
   VMKPollCheckValidAndLocked(newWaiters);
   if (List_IsEmpty(&waiters->list)) {
      return FALSE;
   } else {
      ASSERT(List_IsEmpty(&newWaiters->list));

      newWaiters->list = waiters->list;
      List_Prev(List_First(&newWaiters->list)) = &newWaiters->list;
      List_Next(List_Last(&newWaiters->list)) = &newWaiters->list;
      
      List_Init(&waiters->list);
      return TRUE;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * VMKPoll_WakeupAndRemoveWaitersForEvent --
 *
 *      Wakeup and remove waiters in the list waiting on the type of
 *      events specified.  If any VMKPOLL_ERR* event is given, all
 *      waiters are woken.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	Worlds are woken and removed from waiter list.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
VMKPoll_WakeupAndRemoveWaitersForEvent(VMKPollWaitersList *waiters,
				       VMKPollEvent events)
{
   List_Links *item, *next;
   VMKPollWaiter *waiter;

   VMKPollCheckValidAndLocked(waiters);

   for (item = List_First(&waiters->list);
	!List_IsEmpty(&waiters->list) && !List_IsAtEnd(&waiters->list, item);
        item = next) {
      next = List_Next(item);

      waiter = (VMKPollWaiter*)item;
      /*
       * Doesn't have to be an exact match, as long as any flags present in
       * events match or we hit an error, we'll wake the world up.
       */
      if ((waiter->events & events)
          || (events & VMKPOLL_ERRMASK)) {
	 World_SelectWakeup(waiter->worldID);
	 List_Remove(item);
	 Mem_Free(waiter);
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * VMKPoll_WakeupAndRemoveWaiters --
 *
 *      Wakeup and remove all waiters in the list.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	All worlds on waiters list are woken and list is cleared
 *
 *----------------------------------------------------------------------
 */
static INLINE void
VMKPoll_WakeupAndRemoveWaiters(VMKPollWaitersList *waiters)
{
   VMKPoll_WakeupAndRemoveWaitersForEvent(waiters, VMKPOLL_INMASK);
   ASSERT(! VMKPoll_HasWaiters(waiters));
}


#endif // _VMKPOLL_H
