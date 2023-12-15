/* **********************************************************
 * Copyright 2002 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * watchpoint.c --
 *
 *	This module manages watchpoints.
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "x86.h"
#include "vmkernel.h"
#include "vm_asm.h"
#include "sched.h"
#include "prda.h"
#include "watchpoint.h"
#include "world.h"
#include "proc.h"
#include "debug.h"

#define LOGLEVEL_MODULE Watchpoint
#define LOGLEVEL_MODULE_LEN 10
#include "log.h"

#define NUM_WATCHPOINTS		4

typedef struct WatchpointInfo {
   Watchpoint_Type	type;
   VA 			vaddr;
   uint32		length;
   Watchpoint_Action	action;
   int32 		count;
   int32		limit;
   VA			lastEIP;
} WatchpointInfo;

static SP_SpinLockIRQ	watchpointLock;
static int numWatchpoints;
static WatchpointInfo	watchpoints[NUM_WATCHPOINTS];

static int WatchpointProcRead(Proc_Entry *entry, char *buffer, int *len);

#ifdef VMX86_ENABLE_WATCHPOINTS
#define WATCHPOINTS_ENABLED	1
#else
#define WATCHPOINTS_ENABLED	0
#endif

static Proc_Entry watchpointProcEntry;

/*
 *----------------------------------------------------------------------
 *
 * Watchpoint_Init --
 *
 *      Initialize the watchpoint module.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	The lock is initialized.
 *
 *----------------------------------------------------------------------
 */
void
Watchpoint_Init(void)
{
   SP_InitLockIRQ("watchpointLock", &watchpointLock, SP_RANK_IRQ_LEAF);

   memset(&watchpointProcEntry, 0, sizeof(watchpointProcEntry));
   watchpointProcEntry.read = WatchpointProcRead;
   Proc_Register(&watchpointProcEntry, "watchpoints", FALSE);
}

/*
 *----------------------------------------------------------------------
 *
 * Watchpoint_WorldInit --
 *
 *      Initialize the per-world watchpoint state.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	The given world's watchpoint state is initialized.
 *
 *----------------------------------------------------------------------
 */
void
Watchpoint_WorldInit(World_Handle *world)
{
   world->watchpointState.enabledCount = 0;
   world->watchpointState.changed = FALSE;
}

/*
 *----------------------------------------------------------------------
 *
 * WatchpointSet --
 *
 *      Setup the debug registers for the current world to take
 *	all watch points.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	The debug registers are set up.
 *
 *----------------------------------------------------------------------
 */
static void
WatchpointSet(void)
{
   int i;
   uint32 dr7 = DR7_ONES | DR7_LE | DR7_GE;

   ASSERT_NO_INTERRUPTS(); // to keep shadow/real DRs consistent
   MY_RUNNING_WORLD->watchpointState.changed = FALSE;

   for (i = 0; i < NUM_WATCHPOINTS; i++) {
      if (watchpoints[i].type != WATCHPOINT_TYPE_NONE) {
	 LA la;
	 uint32 rwl = 0;

	 VMLOG(1, MY_RUNNING_WORLD->worldID,
               "Adding watchpoint @ 0x%x for %d bytes of type %d",
               watchpoints[i].vaddr, watchpoints[i].length, 
               watchpoints[i].type);

	 dr7 |= 0x3 << (i * 2);
	 switch (watchpoints[i].type) {
	 case WATCHPOINT_TYPE_EXEC:
	    rwl = DR7_RW_INST;
	    break;
	 case WATCHPOINT_TYPE_WRITE:
	    rwl = DR7_RW_WRITES;
	    break;
	 case WATCHPOINT_TYPE_READ_WRITE:
	    rwl = DR7_RW_ACCESS;
	    break;
	 default:
	    NOT_REACHED();
	 }
	 rwl |= (watchpoints[i].length - 1) << 2;
	 dr7 |= rwl << (16 + i * 4);

         la = watchpoints[i].vaddr - VMK_FIRST_ADDR;

	 switch (i) {
	 case 0:
	    SET_DR0(la);
	    break;
	 case 1:
	    SET_DR1(la);
	    break;
	 case 2:
	    SET_DR2(la);
	    break;
	 case 3:
	    SET_DR3(la);
	    break;
	 default:
	    NOT_REACHED();
	 }
         // for vmm worlds shadowDR must be in sync with real DR
         if (World_IsVMMWorld(MY_RUNNING_WORLD)) {
            MY_RUNNING_WORLD->vmkSharedData->shadowDR[i] = la;
         }
      }
   }

   SET_DR7(dr7);
   if (World_IsVMMWorld(MY_RUNNING_WORLD)) {
      MY_RUNNING_WORLD->vmkSharedData->shadowDR[7] = dr7;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * Watchpoint_Add --
 *
 *      Add a watchpoint.
 *
 * Results:
 *      Return TRUE if the watchpoint could be added.
 *
 * Side effects:
 *	A watchpoint is added to the list of watchpoints.
 *
 *----------------------------------------------------------------------
 */
Bool
Watchpoint_Add(VA vaddr, uint32 length, Watchpoint_Type type,
	       Watchpoint_Action action, int32 limit)
{
   if (WATCHPOINTS_ENABLED) {
      int i;
      Bool retval = FALSE;
      SP_IRQL prevIRQL;

      if (length != 4 && length != 2 && length != 1) {
	 Warning("length of %d not supported", length);
	 return FALSE;
      }

      prevIRQL = SP_LockIRQ(&watchpointLock, SP_IRQL_KERNEL);

      for (i = 0; i < NUM_WATCHPOINTS; i++) {
	 if (watchpoints[i].type == WATCHPOINT_TYPE_NONE) {
	    watchpoints[i].type = type;
	    watchpoints[i].vaddr = vaddr;
	    watchpoints[i].length = length;
	    watchpoints[i].action = action;
	    watchpoints[i].limit = limit;
	    watchpoints[i].count = 0;
	    numWatchpoints++;

	    World_WatchpointsChanged();

	    WatchpointSet();

	    retval = TRUE;
	    break;
	 }
      }

      SP_UnlockIRQ(&watchpointLock, prevIRQL);

      if (!retval) {
	 Warning("Too many watchpoints");
      }

      return retval;
   } else {
      Warning("Watchpoints are not enabled");
      return FALSE;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * Watchpoint_Remove --
 *
 *      Remove a watchpoint.
 *
 * Results:
 *      Return TRUE if the watchpoint could be removed.
 *
 * Side effects:
 *	A watchpoint is removed from the list of watchpoints.
 *
 *----------------------------------------------------------------------
 */
Bool
Watchpoint_Remove(VA vaddr, uint32 length, Watchpoint_Type type)
{
   if (WATCHPOINTS_ENABLED) {
      int i;
      Bool retval = FALSE;
      SP_IRQL prevIRQL = SP_LockIRQ(&watchpointLock, SP_IRQL_KERNEL);

      for (i = 0; i < NUM_WATCHPOINTS; i++) {
	 if (watchpoints[i].type == type &&
	     watchpoints[i].vaddr == vaddr &&
	     watchpoints[i].length == length) {
	    watchpoints[i].type = WATCHPOINT_TYPE_NONE;
	    numWatchpoints--;
	    World_WatchpointsChanged();
	    retval = TRUE;
	 }
      }

      if (!retval) {
	 Warning("Couldn't find watchpoint");
      }

      SP_UnlockIRQ(&watchpointLock, prevIRQL);

      return retval;
   } else {
      Warning("Watchpoints are not enabled");
      return FALSE;
   }
}

#ifdef VMX86_ENABLE_WATCHPOINTS

/*
 *----------------------------------------------------------------------
 *
 * Watchpoint_Enable --
 *
 *      Enable watchpoints for the current world and save the current
 *	debug registers if "save" is TRUE.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	The debug registers may be saved into the world structure.
 *	Watchpoints are enabled for the current world.
 *
 *----------------------------------------------------------------------
 */
void
Watchpoint_Enable(Bool save)
{
   SP_IRQL prevIRQL = SP_LockIRQ(&watchpointLock, SP_IRQL_KERNEL);

   if (MY_RUNNING_WORLD->watchpointState.enabledCount == 0) {
      /*
       * Watchpoint_Disable restores DR regardless of numWatchpoints,
       * so we better save it regardless of numWatchpoints.
       */
      if (save) {
         GET_DR0(MY_RUNNING_WORLD->watchpointState.dr0);
         GET_DR1(MY_RUNNING_WORLD->watchpointState.dr1);
         GET_DR2(MY_RUNNING_WORLD->watchpointState.dr2);
         GET_DR3(MY_RUNNING_WORLD->watchpointState.dr3);
         GET_DR6(MY_RUNNING_WORLD->watchpointState.dr6);
         GET_DR7(MY_RUNNING_WORLD->watchpointState.dr7);
      }

      if (numWatchpoints > 0) {
	 WatchpointSet();
      }
   }

   MY_RUNNING_WORLD->watchpointState.enabledCount++;

   SP_UnlockIRQ(&watchpointLock, prevIRQL);   
}

/*
 *----------------------------------------------------------------------
 *
 * Watchpoint_ForceEnable --
 *
 *      Enable watchpoints for the current world.  They should
 *	already be enabled but the host world may turn them off.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	Watchpoints are enabled for the current world.
 *
 *----------------------------------------------------------------------
 */
void
Watchpoint_ForceEnable(void)
{
   SP_IRQL prevIRQL = SP_LockIRQ(&watchpointLock, SP_IRQL_KERNEL);

   if (numWatchpoints > 0) {
      WatchpointSet();
   }

   SP_UnlockIRQ(&watchpointLock, prevIRQL);   
}

/*
 *----------------------------------------------------------------------
 *
 * Watchpoint_ForceDisable --
 *
 *      Disable watchpoints for the current world.  Do it brute force
 *	without locks and such.  This function should only be called
 *	when the vmkernel is being dumped or maybe in the debugger.
 *
 * Results:
 *      TRUE is watchpoints were enabled.
 *
 * Side effects:
 *	Watchpoints are disabled.
 *
 *----------------------------------------------------------------------
 */
Bool
Watchpoint_ForceDisable(void)
{
   SET_DR0(0x00000000);
   SET_DR1(0x00000000);
   SET_DR2(0x00000000);
   SET_DR3(0x00000000);
   SET_DR6(0x00000000);
   SET_DR7(DR7_DEFAULT);

   return MY_RUNNING_WORLD->watchpointState.enabledCount > 0;
}

/*
 *----------------------------------------------------------------------
 *
 * Watchpoint_Disable --
 *
 *      Disable watchpoints for the current world.  The old debug
 *	registers are restored if "restore" is TRUE.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	The debug registers may be restored.
 *	Watchpoints are disabled for the current world.
 *
 *----------------------------------------------------------------------
 */
void 
Watchpoint_Disable(Bool restore)
{
   SP_IRQL prevIRQL = SP_LockIRQ(&watchpointLock, SP_IRQL_KERNEL);

   MY_RUNNING_WORLD->watchpointState.enabledCount--;
   if (MY_RUNNING_WORLD->watchpointState.enabledCount == 0) {
      if (restore) {
         /* 
          * Restore the current state of the debug registers.
          */
         SET_DR0(MY_RUNNING_WORLD->watchpointState.dr0);
         SET_DR1(MY_RUNNING_WORLD->watchpointState.dr1);
         SET_DR2(MY_RUNNING_WORLD->watchpointState.dr2);
         SET_DR3(MY_RUNNING_WORLD->watchpointState.dr3);
         SET_DR6(MY_RUNNING_WORLD->watchpointState.dr6);
         SET_DR7(MY_RUNNING_WORLD->watchpointState.dr7);
      } else {
         /*
          * Disable the debug registers.
          */
         SET_DR0(0x00000000);
         SET_DR1(0x00000000);
         SET_DR2(0x00000000);
         SET_DR3(0x00000000);
         SET_DR6(0x00000000);
         SET_DR7(DR7_DEFAULT);      
      }
      // For vmm worlds we need to keep shadowDRs in sync with real DRs
      if (World_IsVMMWorld(MY_RUNNING_WORLD)) {
         ASSERT_NO_INTERRUPTS(); // to keep shadow/real DRs consistent
         GET_DR0(MY_RUNNING_WORLD->vmkSharedData->shadowDR[0]);
         GET_DR1(MY_RUNNING_WORLD->vmkSharedData->shadowDR[1]);
         GET_DR2(MY_RUNNING_WORLD->vmkSharedData->shadowDR[2]);
         GET_DR3(MY_RUNNING_WORLD->vmkSharedData->shadowDR[3]);
         GET_DR6(MY_RUNNING_WORLD->vmkSharedData->shadowDR[6]);
         GET_DR7(MY_RUNNING_WORLD->vmkSharedData->shadowDR[7]);
      }
   } 

   SP_UnlockIRQ(&watchpointLock, prevIRQL);
}

/*
 *----------------------------------------------------------------------
 *
 * Watchpoint_Update --
 *
 *      Update the debug watchpoint state for this world if it has
 * 	changed.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
void
Watchpoint_Update(void)
{
   SP_IRQL prevIRQL = SP_LockIRQ(&watchpointLock, SP_IRQL_KERNEL);

   ASSERT(MY_RUNNING_WORLD->watchpointState.enabledCount > 0);

   if (MY_RUNNING_WORLD->watchpointState.changed) {
      WatchpointSet();
   }

   SP_UnlockIRQ(&watchpointLock, prevIRQL);
}

/*
 *----------------------------------------------------------------------
 *
 * Watchpoint_Enabled --
 *
 *      Return TRUE if watchpoints are enabled.
 *
 * Results:
 *      TRUE if watchpoints are enabled.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
Bool
Watchpoint_Enabled(void)
{
   Bool enabled;

   SP_IRQL prevIRQL = SP_LockIRQ(&watchpointLock, SP_IRQL_KERNEL);

   enabled = (Bool)(MY_RUNNING_WORLD->watchpointState.enabledCount > 0);

   SP_UnlockIRQ(&watchpointLock, prevIRQL);

   return enabled;
}

/*
 *----------------------------------------------------------------------
 *
 * Watchpoint_Check --
 *
 *      Check whether the current debug exception happened because
 *	we hit a watch point.
 *
 * Results:
 *      The action to take because of this watchpoint.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
Watchpoint_Action
Watchpoint_Check(VMKExcFrame *regs)
{
   int i;
   uint32 dr6;

   GET_DR6(dr6);
   SET_DR6(0);

   if ((dr6 & 0xf) == 0) {
      /*
       * Not a watchpoint.
       */
      return WATCHPOINT_ACTION_NONE;
   } else if (myPRDA.inWatchpoint) {
      /*
       * We got a recursive watchpoint.
       */
      return WATCHPOINT_ACTION_CONTINUE;
   } else if (Debug_InDebugger()) {
      /*
       * We hit a watchpoint while in the debugger.
       */
      return WATCHPOINT_ACTION_CONTINUE;
   }

   myPRDA.inWatchpoint = TRUE;

   for (i = 0; i < 4; i++) {
      if (dr6 & (1 << i)) {
	 Watchpoint_Action action;
	 SP_IRQL prevIRQL = SP_LockIRQ(&watchpointLock, SP_IRQL_KERNEL);

	 if (watchpoints[i].type == WATCHPOINT_TYPE_NONE) {
	    Warning("No watchpoint???");
	    action = WATCHPOINT_ACTION_CONTINUE;
	 } else {
	    Log("Watchpoint[%d]<0x%x, %d> @ eip 0x%x for world '%s':%d", i,
		watchpoints[i].vaddr, watchpoints[i].length,
		regs->eip, MY_RUNNING_WORLD->worldName,
		MY_RUNNING_WORLD->worldID);
	    watchpoints[i].lastEIP = regs->eip;
	    watchpoints[i].count++;
	    action = watchpoints[i].action;
	    if (watchpoints[i].limit > 0 && 
		watchpoints[i].count >= watchpoints[i].limit) {
	       Log("Watchpoint[%d] reached limit of %d", i, watchpoints[i].limit);
	       watchpoints[i].type = WATCHPOINT_TYPE_NONE;
	       numWatchpoints--;
	       World_WatchpointsChanged();
	    }
	 }

	 SP_UnlockIRQ(&watchpointLock, prevIRQL);

	 myPRDA.inWatchpoint = FALSE;

	 return action;
      }
   }

   NOT_REACHED();
   return WATCHPOINT_ACTION_CONTINUE;
}

#endif


/*
 *----------------------------------------------------------------------
 *
 * WatchpointProcRead --
 *
 *      Watchdog procfs status routine.
 *
 * Results:
 *      Writes human-readable status information about all watchpoints
 *	into "buffer".  Sets "length" to number of bytes written.
 *	Returns 0 iff successful.  
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
WatchpointProcRead(UNUSED_PARAM(Proc_Entry *entry), char *buffer, int *len)
{
   int i;

   *len = 0;

   if (numWatchpoints == 0) {
      return 0;
   }

   Proc_Printf(buffer, len, "%-18s%-10s%-10s%-12s%-15s%-7s%-6s\n",
	       "Virtual Address", "Length", "Limit", "Count", "Last EIP", 
	       "Type", "Action");

   for (i = 0; i < NUM_WATCHPOINTS; i++) {
      if (watchpoints[i].type != WATCHPOINT_TYPE_NONE) {
	 char *type = "";
	 switch (watchpoints[i].type) {
	 case WATCHPOINT_TYPE_EXEC:
	    type = "Exec";
	    break;
	 case WATCHPOINT_TYPE_WRITE:
	    type = "Write";
	    break;
	 case WATCHPOINT_TYPE_READ_WRITE:
	    type = "Rd/Wr";
	    break;
	 default:
	    NOT_REACHED();
	 }

	 if (watchpoints[i].limit <= 0) {
	    Proc_Printf(buffer, len, "0x%-16x%-10d%-10s%-12d0x%-13x%-7s%-6s\n",
			watchpoints[i].vaddr,
			watchpoints[i].length,
			"None",
			watchpoints[i].count,
			watchpoints[i].lastEIP,
			type,
			watchpoints[i].action == WATCHPOINT_ACTION_BREAK ? 
			   "Break" : "Log");
	 } else {
	    Proc_Printf(buffer, len, "0x%-16x%-10d%-10d%-12d0x%-13x%-7s%-6s\n",
			watchpoints[i].vaddr,
			watchpoints[i].length,
			watchpoints[i].limit,
			watchpoints[i].count,
			watchpoints[i].lastEIP,
			type,
			watchpoints[i].action == WATCHPOINT_ACTION_BREAK ? 
			   "Break" : "Log");
	 }
      }
   }

   return 0;
}

