/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * $Id$
 * **********************************************************/

/*
 * event.h --
 *
 *	Event queue header file.
 */

#ifndef _EVENT_H
#define _EVENT_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "splock.h"
#include "list.h"

/*
 * constants
 */

// irq level
#define	EVENTQUEUE_IRQL			(SP_IRQL_KERNEL)

/*
 * types
 */

// EventQueue contains a set of worlds blocked on an event.
typedef struct EventQueue {
   SP_SpinLockIRQ lock;		// for mutual exclusion
   List_Links     queue;	// worlds blocked on event
   uint32	  id;		// queue identifier
} EventQueue;

/*
 * inline operations
 */

/*
 *----------------------------------------------------------------------
 *
 * EventQueue_Lock --
 *
 *      Acquire exclusive access to "q".
 *
 * Results:
 *      Lock for "q" is acquired.
 *      Returns the caller's IRQL level.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE SP_IRQL
EventQueue_Lock(EventQueue *q)
{
   return(SP_LockIRQ(&q->lock, EVENTQUEUE_IRQL));
}

/*
 *----------------------------------------------------------------------
 *
 * EventQueue_Unlock --
 *
 *      Releases exclusive access to "q".
 *      Sets the IRQL level to "prevIRQL".
 *
 * Results:
 *      Lock for "q" is released.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
EventQueue_Unlock(EventQueue *q, SP_IRQL prevIRQL)
{
   SP_UnlockIRQSpecial(&q->lock, prevIRQL);
}

/*
 *----------------------------------------------------------------------
 *
 * EventQueue_IsLocked --
 *
 *      Returns TRUE iff "q" is locked.
 *
 * Results:
 *      Returns TRUE iff "q" is locked.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
EventQueue_IsLocked(EventQueue *q)
{
   return(SP_IsLockedIRQ(&q->lock));
}

/*
 * operations
 */

void Event_Init(void);
void Event_LateInit(Proc_Entry *procSchedDir);

void EventQueue_Init(EventQueue *q, uint32 id);
void EventQueue_Insert(EventQueue *q, World_Handle *world);
Bool EventQueue_Remove(EventQueue *q, World_Handle *world);
Bool EventQueue_Contains(EventQueue *q, World_Handle *world);
EventQueue *EventQueue_Find(uint32 event);

#endif

