/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * userThread.c --
 *
 *	UserWorld thread support
 */

#include "user_int.h"
#include "userThread.h"
#include "timer.h"
#include "memalloc.h"
#include "common.h"
#include "userStat.h"
#include "identity.h"

#define LOGLEVEL_MODULE UserThread
#include "userLog.h"


#define ASSERT_USERTHREADPEERSLOCKED(peers) ASSERT(UserThreadPeersIsLocked(peers))

/*
 * Struct for passing arguments to UserThreadCloneStart from the
 * creator thread in UserThread_Clone.
 */
typedef struct UserThreadCloneArg {
   Reg32 userEIP;
   Reg32 userESP;
} UserThreadCloneArg;

static NORETURN void UserThreadCloneStart(void* arg);


/*
 *----------------------------------------------------------------------
 *
 * UserThreadPeersIsLocked --
 *
 *	Test if the given peers object is locked.
 *
 * Results:
 *	TRUE if locked, FALSE if not.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
inline Bool 
UserThreadPeersIsLocked(UserThread_Peers* peers)
{
   return SP_IsLocked(&peers->lock);
}


/*
 *----------------------------------------------------------------------
 *
 * UserThreadPeersLock --
 *
 *	Lock the given peers object.  Should not already be locked.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Peers lock is acquired.
 *
 *----------------------------------------------------------------------
 */
static INLINE void 
UserThreadPeersLock(UserThread_Peers* peers)
{
   SP_Lock(&peers->lock);
}


/*
 *----------------------------------------------------------------------
 *
 * UserThreadPeersUnlock --
 *
 *	Unlock the given peers object.  Should be locked.
 *
 * Results:
 * 	None
 *
 * Side effects:
 *	Peers object is unlocked
 *
 *----------------------------------------------------------------------
 */
static INLINE void 
UserThreadPeersUnlock(UserThread_Peers* peers)
{
   SP_Unlock(&peers->lock);
}


/*
 *----------------------------------------------------------------------
 *
 * UserThread_PeersInit --
 *
 *	Initialize the cartel-wide thread/wait state.
 *
 * Results:
 *      VMK_OK if initialization was successful, otherwise if not.
 *
 * Side effects:
 *	Peers object is made usable.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserThread_CartelInit(User_CartelInfo* uci)
{
   int i;

   ASSERT(uci != NULL);

   SP_InitLock("User_WaitLock", &uci->waitLock, UW_SP_RANK_WAIT);

   memset(&uci->peers, 0, sizeof(UserThread_Peers));

   for (i = 0; i < ARRAYSIZE(uci->peers.activePeers); i++) {
      uci->peers.activePeers[i] = INVALID_WORLD_ID;
   }

   SP_InitLock("User_ThreadPeers", &uci->peers.lock, UW_SP_RANK_THREADPEER);
   
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserThread_CartelCleanup --
 *
 *	Opposite of UserThread_CartelInit.
 *
 * Results:
 *	VMK_OK
 *
 * Side effects:
 *	Cartel's peers object is unusable.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserThread_CartelCleanup(User_CartelInfo* uci)
{
   UserThread_Peers* const peers = &uci->peers;
   
   ASSERT(uci != NULL);
   ASSERT(peers != NULL);
   
   SP_CleanupLock(&peers->lock);

   if (vmx86_debug) {
      size_t i;
      
      /*
       * Double check that there are no live threads in this cartel
       * Then fill the peer state with garbage.
       */
      for (i = 0; i < ARRAYSIZE(peers->activePeers); i++) {
         ASSERT(peers->activePeers[i] == INVALID_WORLD_ID);
      }

      memset(peers, 0xff, sizeof(UserThread_Peers));
   }

   /* Wait state cleanup */
   SP_CleanupLock(&uci->waitLock);

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * UserThread_SetExitStatus --
 *
 *      Mark the current thread as dead, and record its last integer
 *      for posterity.  This function returns, but the thread won't
 *      make it out of the kernel (it will be collected before
 *      returning to user mode).  All threads should come through here
 *      on their exit path (excepting those that fail very early in
 *      startup or post-clone).
 *
 * Results:
 *      VMK_OK
 *
 * Side effects:
 *	World is tagged for reaping (will happen when the call unwinds
 *	to the syscall entry layer).
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserThread_SetExitStatus(int status)
{
   World_Handle* curr = MY_RUNNING_WORLD;
   User_CartelInfo* uci = curr->userCartelInfo;

   UWLOG(1, "status=%d", status);

   MY_USER_THREAD_INFO->dead = TRUE;

   /*
    * Racy perhaps, but the pthreads library will take care of
    * synchronization when it matters --- the last thread to exit
    * will be the manager thread, and it will exit with the value
    * that should be taken as the cartel exit value.
    */
   uci->shutdown.exitCode = status;

   /*
    * Save exit status of thread for later collection by peers.
    */
   UserThread_SaveStatus(&uci->peers, curr->worldID, status);

   /*
    * Send "death" signal 
    */
   UserSig_SendDeathSignal(curr);

   return VMK_OK;
}   


/*
 *----------------------------------------------------------------------
 *
 * UserThread_Clone --
 *
 *	Start a new thread running in the same cartel as the current
 *	world.  Start the new thread on the given stack running at the
 *	given eip.  Register the given "death signal" and target (See
 *	UserSig_SendDeathSignal) with the new thread.  Death target is
 *	ignored if deathSignal is 0.
 *
 * Results:
 *	VMK_OK if the world was created and started cleanly, sets
 *	*newWorld to point to the new world's handle.  Returns an
 *	error code and ignores newWorld, otherwise.
 *
 * Side effects:
 *	New world is created and started.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserThread_Clone(const char* name,
                 UserVA startAddr,
                 UserVA stackAddr,
                 UserSigId deathSignal,
                 World_ID deathTarget,
                 World_Handle** newWorld)
{
   World_InitArgs args;
   Sched_ClientConfig schedCfg;
   UserThreadCloneArg* arg;
   VMK_ReturnStatus status;
   User_CartelInfo *uci = MY_USER_CARTEL_INFO;

   /* Allocate space for args to pass to clone */
   /* UserThreadCloneStart will free */
   arg = User_HeapAlloc(uci, sizeof(*arg));
   if (arg == NULL) {
      return VMK_NO_MEMORY;
   }

   /*
    * All worlds in the same world group would have the same scheduling
    * group. Because we are creating a new world inside an existing world 
    * group, there is no need to specify scheduling group here, thus 
    * initialize it to invalid group name.
    */
   Sched_ConfigInit(&schedCfg, SCHED_GROUP_NAME_INVALID);
   
   // inherit affinity from calling world.
   schedCfg.cpu.vcpuAffinity[0] = MY_RUNNING_WORLD->sched.cpu.vcpu.affinityMask;

   /* Create the clone */
   World_ConfigArgs(&args, name, WORLD_USER | WORLD_CLONE,
                    World_GetGroupLeaderID(MY_RUNNING_WORLD), &schedCfg);

   status = World_New(&args, newWorld);

   if (status != VMK_OK) {
      User_HeapFree(uci, arg);
      return status;
   }

   /* Fill in args for clone */
   arg->userEIP = startAddr;
   arg->userESP = stackAddr;
   if (deathSignal != 0) {
      /*
       * Death signal is the signal (if any) the child will send to
       * the given target when it dies.  See UserSig_SendDeathSignal.
       */
      UserSig_ThreadInfo* threadSigInfo;
      threadSigInfo = &(*newWorld)->userThreadInfo->signals;
      threadSigInfo->deathSigTarget = deathTarget;
      threadSigInfo->deathSig = deathSignal;
   }
   
   /* Inherit a copy of the creator's identity */
   Identity_Copy(&(*newWorld)->ident, &MY_RUNNING_WORLD->ident);
   status = UserProxy_RegisterThread(uci, (*newWorld)->worldID,
                                        &(*newWorld)->ident);

   status = Sched_Add(*newWorld, UserThreadCloneStart, arg);
   if (status != VMK_OK) {
      User_HeapFree(uci, arg);
      // UserWorld was never "created", so we don't go through
      // normal UserWorld exit path:
      World_Kill(*newWorld);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserThreadCloneStart --
 *
 * 	Helper for UserThread_Clone.  This is the entrypoint for
 *	the new thread, it sets things up so the clone() syscall
 *	will return correctly in the new thread's context.
 *
 * Results:
 *	None, does not return
 *
 * Side effects:
 *	Starts executing user mode code
 *
 *----------------------------------------------------------------------
 */
static void 
UserThreadCloneStart(void* arg)
{
   static const uint16 dataSelector = MAKE_SELECTOR_UNCHECKED(DEFAULT_USER_DATA_DESC, 0, 3);
   static const uint16 codeSelector = MAKE_SELECTOR_UNCHECKED(DEFAULT_USER_CODE_DESC, 0, 3);
   UserThreadCloneArg* parentInfo = (UserThreadCloneArg*)arg;
   VMKFullUserExcFrame initialUserRegs;
   
   ASSERT(MY_USER_THREAD_INFO->exceptionFrame == NULL);
   MY_USER_THREAD_INFO->exceptionFrame = &initialUserRegs;

   UWLOG(1, "userEIP=%#x, userESP=%#x",
         parentInfo->userEIP, parentInfo->userESP);
   
   memset(&initialUserRegs, 0, sizeof initialUserRegs);
   initialUserRegs.frame.errorCode = 0;
   initialUserRegs.frame.eflags = EFLAGS_IF;
   initialUserRegs.frame.cs = codeSelector;
   initialUserRegs.frame.ss = dataSelector;
   initialUserRegs.frame.eip = parentInfo->userEIP;
   initialUserRegs.frame.esp = parentInfo->userESP;
   
   /* Clean up the arg struct passed from creator. */
   User_HeapFree(MY_USER_CARTEL_INFO, parentInfo);
   parentInfo = NULL;

   /*
    * All other regs are zero'd in StartUserWorld; EAX is zero'd too,
    * which will become the return value from the clone function in
    * this new thread's context.  (userEIP must point at a call to
    * clone).
    */
   CpuSched_EnablePreemption();
   StartUserWorld(&initialUserRegs.frame, dataSelector);

   /* StartUserWorld shouldn't return, but just in case ... */
   UWLOG(0, "StartUserWorld returned, exiting");
   UserThread_SetExitStatus(0);
   World_Exit(VMK_OK);
   NOT_REACHED();
}


/*
 *----------------------------------------------------------------------
 *
 * UserThread_Add --
 *
 *	Add the given world to the given peer group.  This should only
 *	be done once per world, while initializing that world's
 *	thread-private state.  Note that it is done before the world
 *	is started.
 *
 * Results:
 *	VMK_OK if successful, VMK_NO_RESOURCES if the active peer list
 *	is full
 *
 * Side effects:
 *	Adds newWorld to active peer table
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserThread_Add(UserThread_Peers* peers,
               World_Handle* newWorld)
{
   VMK_ReturnStatus status = VMK_NO_RESOURCES;
   size_t i;
   
   ASSERT(newWorld != NULL);
   ASSERT(newWorld != MY_RUNNING_WORLD);
   
   UWLOG(3, "world %d to peers at %p", newWorld->worldID, peers);

   UserThreadPeersLock(peers);
   for (i = 0; i < ARRAYSIZE(peers->activePeers); i++) {
      ASSERT(peers->activePeers[i] != newWorld->worldID);
      if (peers->activePeers[i] == INVALID_WORLD_ID) {
         peers->activePeers[i] = newWorld->worldID;
         status = VMK_OK;
         break;
      }
   }
   UserThreadPeersUnlock(peers);

   if (i == ARRAYSIZE(peers->activePeers)) {
      UWLOG(0, "Cannot add world %d to peer struct (full with %d worlds)",
            newWorld->worldID, ARRAYSIZE(peers->activePeers));
      status = VMK_NO_RESOURCES;
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserThread_Remove --
 *
 *	Remove the given thread from the list of active peers.
 *
 * Results:
 *	VMK_OK if the world was added, VMK_NOT_FOUND if the world was
 *	not in the active peer list.
 *
 * Side effects:
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserThread_Remove(UserThread_Peers* peers,
                  World_Handle* deadWorld)
{
   VMK_ReturnStatus status = VMK_NOT_FOUND;
   World_ID deadID;
   size_t i;
   
   ASSERT(deadWorld != NULL);
   ASSERT(deadWorld != MY_RUNNING_WORLD);
   ASSERT(deadWorld->userThreadInfo->dead == TRUE);
   
   UWLOG(3, "world %d from peers at %p", deadWorld->worldID, peers);

   deadID = deadWorld->worldID;
   UserThreadPeersLock(peers);
   for (i = 0; i < ARRAYSIZE(peers->activePeers); i++) {
      if (peers->activePeers[i] == deadID) {
         peers->activePeers[i] = INVALID_WORLD_ID;
         status = VMK_OK;
         break;
      }
   }
   UserThreadPeersUnlock(peers);

   if (i == ARRAYSIZE(peers->activePeers)) {
      UWLOG(0, "Trying to remove %d from %p, but its not there.",
            deadWorld->worldID, peers->activePeers);
      status = VMK_NOT_FOUND;
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserThread_IsOnlyThread --
 *
 *	Test if the given world is the only active world in its
 *	cartel.  No races with world creation as creation is
 *	synchronous and must be accomplished by a world in the cartel
 *	(so if thread is alone, its obviously not executing thread
 *	create code).
 *
 * Results:
 *	TRUE if the world is alone in its cartel, FALSE if a sibiling
 *	world still lives in this cartel.
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
Bool
UserThread_IsOnlyThread(World_Handle* w)
{
   UserThread_Peers* peers = &w->userCartelInfo->peers;
   World_ID wid = w->worldID;
   Bool rc = TRUE;
   size_t i;

   ASSERT(w != NULL);

   UserThreadPeersLock(peers);
   for (i = 0; i < ARRAYSIZE(peers->activePeers); i++) {
      if ((peers->activePeers[i] != INVALID_WORLD_ID) &&
          (peers->activePeers[i] != wid)) {
         rc = FALSE;
         break;
      }
   }
   UserThreadPeersUnlock(peers);

   UWLOG(3, " -> %s", rc ? "alone" : "not alone");

   return rc;
}


/*
 *----------------------------------------------------------------------
 *
 * UserThread_KillPeers --
 *
 *	Kill all the active threads in the given peer table.  If the
 *	current thread is a member of the table, it is spared.  If
 *	'vicious' is true, worlds are slaughtered where they stand,
 *	otherwise they're simply asked to terminate soon.
 *
 *	Only called via User_CartelKill, don't call directly.
 *
 * Results:
 *	VMK_OK if all worlds in the active peer list were killed.
 *	VMK_BUSY if the current world was in the active peer list
 *	(note that that is not a failure -- all the other worlds were
 *	properly killed).
 *
 * Side effects:
 *	Dead worlds.  Beware of vicious, World_Kill is very direct and
 *	brutal.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserThread_KillPeers(UserThread_Peers* peers, Bool vicious)
{
   VMK_ReturnStatus status = VMK_OK;
   World_ID currId = MY_RUNNING_WORLD->worldID;
   size_t i;

   UWLOG(3, "peers @ %p", peers);

   UserThreadPeersLock(peers);
   for (i = 0; i < ARRAYSIZE(peers->activePeers); i++) {
      World_ID peer = peers->activePeers[i];
      if (peer != INVALID_WORLD_ID) {
         World_Handle* peerHandle;
         peerHandle = World_Find(peer);
         if (peerHandle != NULL) {
            UWLOG(2, "Requesting termination of world %d", peer);
            /*
             * Mark peer as dead (polite termination).
             */
            peerHandle->userThreadInfo->dead = TRUE;

            /*
             * Kick (viciously or not) if target isn't me.
             */
            if (peerHandle->worldID != currId) {
               if (vicious) {
                  /* No exit status or "death signal" for this world. */
                  World_Kill(peerHandle);
               } else {
                  UserThread_WakeupWorld(peerHandle, UTW_BACKOUT);
               }
            } else {
               ASSERT(status != VMK_BUSY); // I best not be in the list twice
               status = VMK_BUSY;
            }
            World_Release(peerHandle);
         }
      }
   }
   UserThreadPeersUnlock(peers);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserThread_SaveStatus --
 *
 *	Save the exit status for the given WorldID (assumed to be the
 *	current world because otherwise you have races to deal
 *	with).  Only call once per world.  Saved state can be
 *	collected, see UserThread_Collect.  Any threads blocked
 *	waiting to collect state will be woken.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Zombie world state is updated.
 *
 *----------------------------------------------------------------------
 */
void 
UserThread_SaveStatus(UserThread_Peers* peers, World_ID worldID, int status)
{
   size_t i;

   ASSERT(peers != NULL);
   ASSERT(worldID == MY_RUNNING_WORLD->worldID);

   UserThreadPeersLock(peers);
   for (i = 0; i < ARRAYSIZE(peers->zombiePeers); i++) {
      ASSERT(peers->zombiePeers[i] != worldID); // triggers if shutdown twice
      if (peers->zombiePeers[i] == INVALID_WORLD_ID) {
         UWLOG(3, "my status=%d @ zombie index=%d", status, i);
         peers->zombiePeers[i] = worldID;
         peers->exitState[i] = status;

         UserThread_WakeupGroup(MY_USER_CARTEL_INFO, (uint32)peers);
         break;
      }
   }
   UserThreadPeersUnlock(peers);

   ASSERT(i != ARRAYSIZE(peers->zombiePeers));
}


/*
 *----------------------------------------------------------------------
 *
 * UserThreadPeerMatch --
 *
 *	Test i'th entry in zombie peer table to see if it matches
 *	given worldID.  Given worldID may be INVALID_WORLD_ID, in
 *	which case the first matching zombie world is returned.  In
 *	either case, the found zombie is cleared from the zombie list
 *	and the status is returned in *status.  If no world is found,
 *	FALSE is returned and worldID and status are unchanged.
 *
 * Results:
 *	TRUE if a matching world was found, FALSE if not
 *
 * Side effects:
 *	Zombie's status is cleared on match.
 *
 *---------------------------------------------------------------------- */
static Bool
UserThreadPeerMatch(UserThread_Peers* peers, // IN: zombie peer list to look through
                    size_t i,                // IN: index to try
                    World_ID* worldID,       // IN/OUT: INVALID_WORLD_ID or worldID / worldID
                    int* status)             // OUT: world status
{
   Bool found = FALSE;

   ASSERT(i < ARRAYSIZE(peers->zombiePeers));
   ASSERT(worldID != NULL);
   ASSERT(status != NULL);
   ASSERT_USERTHREADPEERSLOCKED(peers);

   if (peers->zombiePeers[i] != INVALID_WORLD_ID) {
      if ((*worldID == INVALID_WORLD_ID)
          || (peers->zombiePeers[i] == *worldID)) {
         *worldID = peers->zombiePeers[i];
         *status = peers->exitState[i];

         /* Clear out status */
         peers->zombiePeers[i] = INVALID_WORLD_ID;
         found = TRUE;
      }
   }
   return found;
}


/*
 *----------------------------------------------------------------------
 *
 * UserThread_Collect --
 *
 *	Collect exit information from a dead thread.  If 'block' is
 *	true, wait until thread matching given worldID is found
 *	(worldID may be -1, in which case we wait until any thread's
 *	state is available.)
 *
 * Results:
 *      UserThread_Wait return value
 *      worldID is set to id of collected world, exitStatus
 *      is set to exit status of said world.
 *
 * Side effects:
 *	A thread's zombie state is cleared and exit status is
 *	returned.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserThread_Collect(UserThread_Peers* peers,// IN/OUT
                   World_ID* worldID,      // IN/OUT
                   Bool blocking,	   // IN
                   int* exitStatus)        // OUT
{
   VMK_ReturnStatus status = VMK_OK;
   Bool done = FALSE;

   ASSERT(peers);
   ASSERT(worldID);
   ASSERT(exitStatus);

   UWLOG(3, "(worldID=%d, %s)", *worldID,
         blocking ? "blocking" : "non-blocking");

   UserThreadPeersLock(peers);
   while (!done) {
      size_t i;
      /*
       * Look for a match to worldID
       */
      for (i = 0; i < ARRAYSIZE(peers->zombiePeers); i++) {
         if (UserThreadPeerMatch(peers, i, worldID, exitStatus)) {
            ASSERT(*worldID != INVALID_WORLD_ID);
            status = VMK_OK;
            done = TRUE;
            break;
         }
      }

      /*
       * If we found nothing and were looking for a specific world,
       * make sure caller asked for a legit world ID.  Legit IDs are
       * restricted to those in the current cartel.
       */
      if ((!done) && (worldID != INVALID_WORLD_ID)) {
         for (i = 0; i < ARRAYSIZE(peers->activePeers); i++) {
            if (peers->activePeers[i] == *worldID) {
               /* WorldID is legit */
               break;
            }
         }

         if (i == ARRAYSIZE(peers->activePeers)) {
            UWLOG(1, "waiting for world, %d, not in cartel.", *worldID);
            status = VMK_NO_SUCH_ZOMBIE;
            done = TRUE;
         }
      }

      /*
       * If no world found, and not an error, block or return an error
       * as 'blocking' dictates.
       */
      if (!done) {
         if (blocking) {
            UWLOG(3, "   -> sleeping (waiting for world=%d)", *worldID);
            
            status = UserThread_Wait((uint32)peers, CPUSCHED_WAIT_UW_EXITCOLLECT,
                                     &peers->lock, 0, UTWAIT_WITHOUT_PREPARE);
            if (status != VMK_OK) {
               UWLOG(3, "wait interrupted, returning %s",
                     UWLOG_ReturnStatusToString(status));
               // if wait returned abnormally, just return wait status
               ASSERT(status != VMK_TIMEOUT); /* no timeout given */
               ASSERT(status == VMK_WAIT_INTERRUPTED);
               done = TRUE;
            }
         } else {
            status = VMK_NO_SUCH_ZOMBIE;
            done = TRUE;
         }
      }
   }
   UserThreadPeersUnlock(peers);

   UWLOG(3, "   -> worldID=%d, status=%d", *worldID, *exitStatus);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserThread_Sleep --
 *
 *	Put the current world to sleep for the given number of
 *	timer cycles units specified by "sleepTime".  May be 
 *	interrupted by signals.
 *
 * Results:
 *	Time left to wait (if interrupted) or 0 if sleep is complete.
 *
 * Side effects:
 *	Current thread stops running for a bit
 *
 *----------------------------------------------------------------------
 */
Timer_RelCycles
UserThread_Sleep(Timer_RelCycles sleepTime)
{
   World_Handle* const currWorld = MY_RUNNING_WORLD;
   User_ThreadInfo* uti = currWorld->userThreadInfo;
   Timer_AbsCycles now = Timer_GetCycles(); 
   Timer_AbsCycles endTime = now + sleepTime; 
   Timer_RelCycles remainingTime = sleepTime; 
   UWLOG(3, "(sleepTime=%Ld cycles)", sleepTime);

   if (remainingTime <= 0) {
      UWLOG(3, "sleep already done.");
      remainingTime = 0;
   } else {
      VMK_ReturnStatus status;
      status = UserThread_Wait(UTWAIT_SLEEP_EVENT(uti),
                               CPUSCHED_WAIT_UW_SLEEP,
                               NULL, remainingTime,
                               UTWAIT_WITHOUT_PREPARE);
      
      remainingTime = endTime - Timer_GetCycles();
      if (remainingTime < 0) {
         remainingTime = 0;
      }
      
      UWLOG(3, "awoken from sleep status=%s (%#x), remaining=%Ld cycles",
            UWLOG_ReturnStatusToString(status), status,
            remainingTime);
   }
   
   ASSERT(remainingTime >= 0);
   return remainingTime;
}

/*
 *----------------------------------------------------------------------
 *
 * UserThread_WakeupWorld --
 *
 *	Wakeup the given world and have it wake with the given
 *	WaitState.  
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Target is awoken if asleep, nothing otherwise.
 *
 *----------------------------------------------------------------------
 */
void 
UserThread_WakeupWorld(World_Handle* world, UserThread_WaitState newState)
{
   UserThread_WaitInfo* const waitInfo = &world->userThreadInfo->waitInfo;
   User_CartelInfo* const uci = world->userCartelInfo;

   ASSERT(world);
   ASSERT(World_IsUSERWorld(world));
   ASSERT(newState != UTW_PRE_BLOCK);
   ASSERT(newState != UTW_BLOCKED);
   ASSERT(newState != UTW_AWAKE);

   UWLOGFor(2, world, "wakeup newState=%d from %p:%p",
            newState, __builtin_return_address(0), __builtin_return_address(1));

   SP_Lock(&uci->waitLock);
   if (waitInfo->state != UTW_AWAKE) {
      CpuSched_ForceWakeup(world);
      /*
       * There are several states the target world could be in.
       * (Note that it isn't in UTW_AWAKE.)
       *
       * It could be blocked in a UserThread_Wait, having never been
       * woken (UTW_BLOCKED).  It could running with UTW_PRE_BLOCK.
       * In either of these cases, just overwrite the target state.
       *
       * It could be blocked in an "uninterruptible" wait (a direct
       * call to CpuSched_Wait without using UserThread_Wait, e.g., in
       * RPCs or a semaphore).  Normally this would happen with
       * UTW_AWAKE, and so we'd skip the wakeup entirely.  But the
       * target could be in UTW_PRE_BLOCK code, in which case we'll
       * record the state change.
       *
       * Lastly, if the target was already woken once or more (from a
       * real block or a pre-block), but hasn't gotten a chance to
       * run, then its state could be UTW_TIMEOUT, UTW_WAIT_COMPLETE,
       * or UTW_BACKOUT.  In those cases we prioritize BACKOUT over
       * COMPLETE over TIMEOUT.
       */
      switch (waitInfo->state) {
      case UTW_BLOCKED:
      case UTW_PRE_BLOCK:
         waitInfo->state = newState;
         break;
      case UTW_TIMEOUT:
         switch (newState) {
         case UTW_TIMEOUT:
            break;
         case UTW_WAIT_COMPLETE:
         case UTW_BACKOUT:
            waitInfo->state = newState;
            break;
         default:
            ASSERT(FALSE);
            break;
         }
         break;
      case UTW_WAIT_COMPLETE:
         switch (newState) {
         case UTW_TIMEOUT:
         case UTW_WAIT_COMPLETE:
            break;
         case UTW_BACKOUT:
            waitInfo->state = newState;
            break;
         default:
            ASSERT(FALSE);
            break;
         }
         break;
      case UTW_BACKOUT:
         /* Nothing overrides this */
         break;
      default:
         ASSERT(FALSE);
         break;
      }         
   }
   SP_Unlock(&uci->waitLock);
}   

/*
 *----------------------------------------------------------------------
 *
 * UserThread_Wakeup --
 *
 *	Wakeup the blocked world.  Skips the wakeup if the target is
 *	no longer alive or isn't a UserWorld.  See
 *	UserThread_WakeupWorld
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Target is awoken if asleep, nothing otherwise.
 *
 *----------------------------------------------------------------------
 */
void 
UserThread_Wakeup(World_ID worldID, UserThread_WaitState newState)
{
   World_Handle* world;
   world = World_Find(worldID);
   if (world != NULL) {
      if (World_IsUSERWorld(world)) {
         UserThread_WakeupWorld(world, newState);
      } else {
         UWLOG(1, "Skipping.  Target (%d) is not a user world (type=%#x)",
               worldID, world->typeFlags);
      }
      World_Release(world);
   } else {
      UWLOG(1, "wid=%d -> not found", worldID);
   }
}



/*
 *----------------------------------------------------------------------
 *
 * UserThreadWaitTimeout --
 *
 *	Timeout handler used by UserThread_Wait.  Wakeup the thread
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static void 
UserThreadWaitTimeout(void* wid, UNUSED_PARAM(Timer_AbsCycles timestamp))
{
   UserThread_Wakeup((World_ID)wid, UTW_TIMEOUT);
}   


/*
 *----------------------------------------------------------------------
 *
 * UserThread_PrepareToWait
 *
 *      This thread is about to wait on an event, and informing various
 *      objects to wake it up, but the thread is not going to sleep right
 *      now.  Set up things to record wakeups if they happen before the
 *      thread actually sleeps.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
UserThread_PrepareToWait(void)
{
   UserThread_WaitInfo* const waitInfo = &MY_USER_THREAD_INFO->waitInfo;
   User_CartelInfo* const uci = MY_USER_CARTEL_INFO;

   SP_Lock(&uci->waitLock);
   ASSERT(waitInfo->state == UTW_AWAKE);
   waitInfo->state = UTW_PRE_BLOCK;
   SP_Unlock(&uci->waitLock);
}


/*
 *----------------------------------------------------------------------
 *
 * UserThread_CancelPreparedWait
 *
 *	Clean the current thread's waitInfo struct of state from a
 *	prior call to UserThread_PrepareToWait (assuming the intended
 *	UserThread_Wait won't be called).  Any delivered wakeups or
 *	backouts are ignored, as we assume the caller is backing out
 *	already.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Wait state is clean.
 *
 *----------------------------------------------------------------------
 */
void
UserThread_CancelPreparedWait(void)
{
   UserThread_WaitInfo * const waitInfo = &MY_USER_THREAD_INFO->waitInfo;
   User_CartelInfo* const uci = MY_USER_CARTEL_INFO;

   SP_Lock(&uci->waitLock);
   ASSERT(waitInfo->state != UTW_AWAKE);
   ASSERT(waitInfo->state != UTW_BLOCKED);
   waitInfo->state = UTW_AWAKE;
   SP_Unlock(&uci->waitLock);
}


/*
 *----------------------------------------------------------------------
 *
 * UserThread_WaitInt --
 *
 *      Wait on given event up to the given timeout period.  Can be
 *      awoken by UserThread_WakeupWorld or UserThread_WakeupGroup or
 *      someone directly calling cpusched_wakeup on the event (though
 *      by definition this would be a spurious wakeup).  Only one of
 *      the lock and sema parameters should be valid (or both can be
 *      NULL).
 *
 *	All UserThread_Wait sleeps are interruptible for termination,
 *	signals, etc.  Callers should be prepared to return to the
 *	syscall entry layer to handle the interruption.
 *
 *	Don't call directly.  Use the UserThread_Wait or
 *	UserThread_WaitSema wrappers.
 *
 * Results:
 *      VMK_TIMEOUT if woken up due to timeout
 *      VMK_WAIT_INTERRUPTED if woken up due to signal, death, etc.
 *      VMK_OK if normal wakeup
 *
 * Side effects:
 *      Lock (or sema) is released before sleeping and reacquired
 *      before return.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserThread_WaitInt(uint32 event,
                   uint32 reason, 
                   SP_SpinLock *lock,
                   Semaphore* sema,
                   Timer_RelCycles timeout,
                   Bool withPrepare)
{
   User_CartelInfo* const uci = MY_USER_CARTEL_INFO;
   User_ThreadInfo* const uti = MY_USER_THREAD_INFO;
   UserThread_WaitInfo* const waitInfo = &uti->waitInfo;
   VMK_ReturnStatus status = VMK_OK;
   Bool earlyReturn = FALSE;
   UserThread_WaitState oldState;

   UWLOG(2, "waiting for event=%x reason=%x lock=%p from %p:%p",
         event, reason, lock,
         __builtin_return_address(0), __builtin_return_address(1));

   ASSERT((lock == NULL) || (sema == NULL));  // just not both
   ASSERT((lock == NULL) || SP_IsLocked(lock));
   ASSERT((sema == NULL) || Semaphore_IsLocked(sema));
   
   SP_Lock(&uci->waitLock);

   if (withPrepare) {
      /*
       * If we called UserThread_PrepareToWait, then we may have been
       * "awoken" before we got to the actual wait.  Simply return as
       * if a wakeup was delivered immediately.
       */
      if (waitInfo->state != UTW_PRE_BLOCK) {
         earlyReturn = TRUE;
      }
   } else {
      ASSERT(waitInfo->state == UTW_AWAKE);
   }

   /*
    * Check for signal/death pending.  This will override a complete
    * or timeout wakeup.
    *
    * Note the lack of locking.  We're holding the waitLock, so we
    * can't grab the signal lock.  We rely on the fact that whenever
    * these bits are set the twiddler of the bit will then grab the
    * waitLock and kick us.
    */
   if ((uti->dead) || (uti->signals.pendingBit != 0)) {
      waitInfo->state = UTW_BACKOUT;
      earlyReturn = TRUE;
   }

   if (!earlyReturn) {
      Timer_Handle th = TIMER_HANDLE_NONE;

      /*
       * Going to block.  Register a timeout handler if necessary, and
       * then change state to blocked, drop the caller's lock and
       * sleep on the event.
       */
      if (timeout) {
         ASSERT(timeout > 0);
         th = Timer_AddTC(myPRDA.pcpuNum, DEFAULT_GROUP_ID,
                          UserThreadWaitTimeout,
                          Timer_GetCycles() + timeout, 0,
                          (void *)MY_RUNNING_WORLD->worldID);
      }
      
      waitInfo->state = UTW_BLOCKED;

      if (sema != NULL) {
         Semaphore_Unlock(sema);
      }

      if (lock != NULL) {
         /*
          * This is a "special" unlock. Special means we're dropping
          * it out of LIFO order.  (We just grabbed the waitLock, but
          * are releasing the caller's lock first).
          */
         SP_UnlockSpecial(lock);
      }

      UWSTAT_TIMERSTART(waitTimes);
      CpuSched_Wait(event, reason, &uci->waitLock);
      UWSTAT_TIMERSTOP(waitTimes);

      if (lock != NULL) {
         SP_Lock(lock);
      }

      if (sema != NULL) {
         Semaphore_Lock(sema);
      }

      SP_Lock(&uci->waitLock);

      if (timeout) {
         Timer_Remove(th);
      }
   }

   /* 
    * Save state for computing return value, set my state as woken,
    * and release lock so others can see it.
    */
   oldState = waitInfo->state;
   waitInfo->state = UTW_AWAKE;
   SP_Unlock(&uci->waitLock);

   /*
    * Convert waitInfo->state to a return status.  State was either
    * changed by the thread that woke me, or if I fell through from an
    * "earlyReturn".
    */
   switch (oldState) {
   case UTW_TIMEOUT:
      status = VMK_TIMEOUT;
      break;
   case UTW_BACKOUT:
      // Will turn into EINTR
      status = VMK_WAIT_INTERRUPTED;
      break;
   case UTW_BLOCKED:
      // Spurious wakeups, direct calls to CpuSched_Wakeup, and
      // UserThread_WakeupGroup could result in UTW_BLOCKED, so treat
      // this as a normal WAIT_COMPLETE event.  Caller has to detect
      // spurious wakeups.
   case UTW_WAIT_COMPLETE:
      status = VMK_OK;
      break;
   case UTW_AWAKE:
   case UTW_PRE_BLOCK:
   default:
      Panic("Unexpected wait state %d\n", oldState);
      break;
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserThread_WakeupGroup --
 *
 *	Wakeup a "group" (i.e., the readers on a pipe).  Woken worlds
 *	will return success (VMK_OK), excepting races with other wakeups.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Any waiting worlds are woken.
 *
 *----------------------------------------------------------------------
 */
void
UserThread_WakeupGroup(User_CartelInfo* uci, uint32 event)
{
   /*
    * Grab waitLock to synchronize with other wakers and with the
    * waiter.  Prevents wakeups from getting lost since the Wait code
    * explicitly drops the caller's lock before going to sleep (but
    * holds the waitLock while doing that) and we cannot acquire the
    * caller's lock for all the targets we're going to wake.
    */
   SP_Lock(&uci->waitLock);
   CpuSched_Wakeup(event);
   SP_Unlock(&uci->waitLock);
}


/*
 *----------------------------------------------------------------------
 *
 * UserThread_NumPeersDebug --
 *
 *	Returns the number of peers in the cartel (including current
 *	world).
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
int
UserThread_NumPeersDebug(void)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   int i;
   int count;

   ASSERT(uci->debugger.inDebugger);

   count = 0;
   for (i = 0; i < ARRAYSIZE(uci->peers.activePeers); i++) {
      if (uci->peers.activePeers[i] != INVALID_WORLD_ID) {
         count++;
      }
   }

   return count;
}


/*
 *----------------------------------------------------------------------
 *
 * UserThread_GetPeersDebug --
 *
 *	Returns a compact list of peer World_IDs (including current
 *	world).
 *
 *	Assumes given array is large enough to hold all peer ids.
 *
 * Results:
 *	Number of peers copied into supplied array.	
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
int
UserThread_GetPeersDebug(World_ID* peerList)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   int i, n;

   ASSERT(uci->debugger.inDebugger || UserDump_DumpInProgress());

   n = 0;
   for (i = 0; i < ARRAYSIZE(uci->peers.activePeers); i++) {
      if (uci->peers.activePeers[i] != INVALID_WORLD_ID) {
         peerList[n] = uci->peers.activePeers[i];
	 n++;
      }
   }

   return n;
}


/*
 *----------------------------------------------------------------------
 *
 * UserThread_IsPeerDebug --
 *
 *	Returns true if given world is a peer of the current world.
 *
 * Results:
 *	None.	
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
Bool
UserThread_IsPeerDebug(World_ID worldID)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   int i;

   ASSERT(worldID != INVALID_WORLD_ID);
   ASSERT(uci->debugger.inDebugger);

   for (i = 0; i < ARRAYSIZE(uci->peers.activePeers); i++) {
      if (uci->peers.activePeers[i] == worldID) {
         return TRUE;
      }
   }

   return FALSE;
}
