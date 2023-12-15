/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * $Id$
 * **********************************************************/

/*
 * event.c --
 *
 *	Event queues.
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "vmkernel.h"
#include "splock.h"
#include "list.h"
#include "proc.h"
#include "world.h"
#include "cpusched.h"

#include "event.h"

/*
 * Compile-time options
 */

// general debugging
#if	defined(VMX86_DEBUG) && defined(VMX86_DEVEL)
#define	EVENT_DEBUG_VERBOSE	(0)
#define	EVENT_DEBUG		(1)
#else
#define	EVENT_DEBUG_VERBOSE	(0)
#define	EVENT_DEBUG		(0)
#endif

/*
 * Constants
 */

// event table size (prime)
#define	EVENT_TABLE_SIZE	(101)

// lock rank
#define	SP_RANK_EVENT		(SP_RANK_IRQ_CPUSCHED_LO - 1)

/*
 * Types
 */

/*
 * EventTable contains a set of EventQueue objects.
 * Event numbers are hashed to select a particular EventQueue.
 */
typedef EventQueue EventTable[EVENT_TABLE_SIZE];

/*
 * Globals
 */

static EventTable eventTable;
static Proc_Entry procEvent;

/*
 * EventQueue Operations
 */

/*
 *----------------------------------------------------------------------
 *
 * EventQueue_Init --
 *
 *      Initializes event queue "q".  The "id" parameter
 *	is used only to generate a descriptive lock name.
 *
 * Results:
 *      Initializes "q".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
EventQueue_Init(EventQueue *q, uint32 id)
{
   char nameBuf[32];
   snprintf(nameBuf, sizeof nameBuf, "EventQueue.%u", id);
   ASSERT(SP_RANK_EVENT > SP_RANK_IRQ_BLOCK);
   SP_InitLockIRQ(nameBuf, &q->lock, SP_RANK_EVENT);
   List_Init(&q->queue);
   q->id = id;
}

/*
 *----------------------------------------------------------------------
 *
 * EventQueue_Contains --
 *
 *      Returns TRUE iff "q" contains "world".
 *	Caller must hold "q" lock.
 *
 * Results:
 *      Returns TRUE iff "q" contains "world".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Bool
EventQueue_Contains(EventQueue *q, World_Handle *world)
{
   List_Links *elt;

   // search for world in q
   LIST_FORALL(&q->queue, elt) {
      World_Handle *w = (World_Handle *) elt;
      if (w->worldID == world->worldID) {
         return(TRUE);
      }
   }

   // not found
   return(FALSE);
}

/*
 *----------------------------------------------------------------------
 *
 * EventQueue_Insert --
 *
 *      Adds "world" to event queue "q".
 *	Caller must hold "q" lock.
 *
 * Results:
 *      Modifies "q", "world".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
EventQueue_Insert(EventQueue *q, World_Handle *world)
{
   List_Insert(&world->sched.links, LIST_ATREAR(&q->queue));   
}

/*
 *----------------------------------------------------------------------
 *
 * EventQueue_Remove --
 *
 *      If event queue "q" contains "world", remove "world" from "q".
 *	Returns TRUE iff "world" was successfully removed from "q".
 *	Caller must hold "q" lock.
 *
 * Results:
 *      Returns TRUE iff "world" was successfully removed from "q".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Bool
EventQueue_Remove(EventQueue *q, World_Handle *world)
{
   Bool removed = FALSE;

   if (EventQueue_Contains(q, world)) {
      List_Remove(&world->sched.links);
      removed = TRUE;
   }

   return(removed);
}

/*
 *----------------------------------------------------------------------
 *
 * EventQueue_Find --
 *
 *      Returns event queue associated with "event".
 *
 * Results:
 *      Returns event queue associated with "event".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
EventQueue *
EventQueue_Find(uint32 event)
{
   // OPT: consider using "hash(event) & mask" instead of mod
   uint32 bucket = event % EVENT_TABLE_SIZE;
   ASSERT(bucket < EVENT_TABLE_SIZE);
   return(&eventTable[bucket]);
}

/*
 *----------------------------------------------------------------------
 *
 * EventQueueFormat --
 *
 *      Formats contents of event queue "q" into "buf".
 *
 * Results:
 *      Updates "buf" and "len" to reflect formatted data.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
EventQueueFormat(EventQueue *q, char *buf, int *len)
{
   SP_IRQL prevIRQL;

   // format event queue contents, if any
   prevIRQL = EventQueue_Lock(q);
   if (!List_IsEmpty(&q->queue)) {
      List_Links *elt;

      Proc_Printf(buf, len, "%3u ", q->id);
      LIST_FORALL(&q->queue, elt) {
         World_Handle *world = (World_Handle *) elt;
         Proc_Printf(buf, len, "%u/%x ",
                     world->worldID,
                     world->sched.cpu.vcpu.waitEvent);
      }
      Proc_Printf(buf, len, "\n");
   }
   EventQueue_Unlock(q, prevIRQL);
}

static void
EventQueueFormatHeader(char *buf, int *len)
{
   // format header
   Proc_Printf(buf, len, "evq queue[vcpu/event...]\n");
}

/*
 * Event Operations
 */

/*
 *----------------------------------------------------------------------
 *
 * Event_Init --
 *
 *      Initializes the Event module.
 *
 * Results:
 *      None. 
 *
 * Side effects:
 *      Modifies global event state.
 *
 *----------------------------------------------------------------------
 */
void
Event_Init(void)
{
   int i;

   // initialize event wait queues
   for (i = 0; i < EVENT_TABLE_SIZE; i++) {
      EventQueue_Init(&eventTable[i], i);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * EventProcRead --
 *
 *      Callback for read operation on /proc/vmware/sched/events
 *	procfs node.
 *
 * Results:
 *      Returns VMK_OK.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
EventProcRead(UNUSED_PARAM(Proc_Entry *entry), char *buf, int *len)
{
   int i;

   // initialize
   *len = 0;
   
   // format event queue table
   EventQueueFormatHeader(buf, len);
   for (i = 0; i < EVENT_TABLE_SIZE; i++) {
      EventQueueFormat(&eventTable[i], buf, len);
   }
   Proc_Printf(buf, len, "\n");

   // everything OK
   return(VMK_OK);
}


/*
 *----------------------------------------------------------------------
 *
 * Event_LateInit --
 *
 *      Final initialization of Event module.
 *	Registers procfs node under "procSchedDir".
 *
 * Results:
 *      None. 
 *
 * Side effects:
 *      Modifies global event state.
 *
 *----------------------------------------------------------------------
 */
void
Event_LateInit(Proc_Entry *procSchedDir)
{
   // register "sched/events" proc node
   Proc_InitEntry(&procEvent);
   procEvent.parent = procSchedDir;
   procEvent.read = EventProcRead;
   Proc_Register(&procEvent, "events", FALSE);
}
