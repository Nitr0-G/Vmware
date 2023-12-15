/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * userTime.c --
 *
 *	UserWorld time
 */

#include "user_int.h"
#include "userTime.h"
#include "userSig.h"
#include "timer.h"

#define LOGLEVEL_MODULE UserTime
#include "userLog.h"

#define USER_TIME_SAMPLE_MSECS 10

/*
 * Global state of the UserTime module.
 */
typedef struct UserTimeInfo {
   SP_SpinLock	lock;
   unsigned     profiled;  // number of threads using a virtual or prof timer
   Timer_Handle timers[MAX_PCPUS];  // sampling timers for profiling
   Timer_RelCycles sampleTC;
} UserTimeInfo;

UserTimeInfo userTimeInfo;

static void UserTimeRealCB(void *data, Timer_AbsCycles timestamp);
static void UserTimeProfStart(void);
static void UserTimeProfStop(void);
static Bool UserTimeProfCountdown(UserTime_ProfTimer *pt);
static void UserTimeProfCB(void *data, Timer_AbsCycles timestamp);
static void UserTimeTSToUS(LinuxITimerVal* itv,
                           Timer_RelCycles remainingTC,
                           Timer_RelCycles periodTC);


/*
 *----------------------------------------------------------------------
 *
 * UserTime_Init --
 *
 *	Initialize global time state.
 *
 * Results:
 *	VMK_OK
 *
 * Side effects:
 *	Lock allocated
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserTime_Init()
{
   memset(&userTimeInfo, 0, sizeof(userTimeInfo));
   SP_InitLock("UserTimeInfo", &userTimeInfo.lock, UW_SP_RANK_TIME);
   userTimeInfo.sampleTC = Timer_MSToTC(USER_TIME_SAMPLE_MSECS);
   return VMK_OK;
}


#if 0
/*
 *----------------------------------------------------------------------
 *
 * UserTime_Cleanup --
 *
 *	Tear down global time state.
 *      XXX Not yet called.  For use when/if userworlds becomes a module.
 *
 * Results:
 *	VMK_OK
 *
 * Side effects:
 *	Lock cleaned up
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
UserTime_Cleanup()
{
   ASSERT(userTimeInfo.profiled == 0);
   SP_CleanupLock(&userTimeInfo.lock);
   return VMK_OK;
}
#endif


/*
 *----------------------------------------------------------------------
 *
 * UserTime_ThreadInit --
 *
 *	Initialize thread-private time state.
 *
 * Results:
 *	VMK_OK
 *
 * Side effects:
 *	Lock allocated
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
UserTime_ThreadInit(User_ThreadInfo* uti)
{
   memset(&uti->time, 0, sizeof(uti->time));
   SP_InitLock("UserTime_ThreadInfo", &uti->time.lock, UW_SP_RANK_TIMETHREAD);
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserTime_ThreadCleanup --
 *
 *	Undo UserTime_ThreadInit.
 *
 * Results:
 *	VMK_OK
 *
 * Side effects:
 *	Lock cleaned up
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
UserTime_ThreadCleanup(User_ThreadInfo* uti)
{
   if (uti->time.realTimer != TIMER_HANDLE_NONE) {
      Timer_RemoveSync(uti->time.realTimer);
   }
   if (uti->time.virtualTimer.remaining) {
      UserTimeProfStop();
   }
   if (uti->time.profTimer.remaining) {
      UserTimeProfStop();
   }
   SP_CleanupLock(&uti->time.lock);
   memset(&uti->time, 0, sizeof(uti->time));
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserTime_CartelInit --
 *
 *	Initialize cartel-level time state.
 *
 * Results:
 *	VMK_OK
 *
 * Side effects:
 *	Copies User_PTSCGet into ktext.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
UserTime_CartelInit(User_CartelInfo* uci)
{
   VMK_ReturnStatus status;
   // imports from pseudotsc.S:
   extern uint8 User_PTSCGet, User_PTSCGet_End;
   extern uint8 User_PTSCGet_CriticalSection, User_PTSCGet_CriticalSectionEnd;

   ASSERT(uci != NULL);
   status = UserMem_AddToKText(&uci->mem, (void *) &User_PTSCGet,
                               &User_PTSCGet_End - &User_PTSCGet,
                               &uci->time.pseudoTSCGet);
   uci->time.criticalSection = uci->time.pseudoTSCGet +
      &User_PTSCGet_CriticalSection - &User_PTSCGet;
   uci->time.criticalSectionSize = 
      &User_PTSCGet_CriticalSectionEnd - &User_PTSCGet_CriticalSection;
   
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserTime_CartelCleanup --
 *
 *	Undo UserTime_CartelInit
 *
 * Results:
 *	VMK_OK
 *
 * Side effects:
 *	None yet.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
UserTime_CartelCleanup(User_CartelInfo* uci)
{
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserTimeThreadLock --
 *
 *	Lock given thread-private time state.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Lock acquired.
 *
 *----------------------------------------------------------------------
 */
static INLINE void 
UserTimeThreadLock(UserTime_ThreadInfo* tti)
{
   ASSERT(tti != NULL);
   SP_Lock(&tti->lock);
}


/*
 *----------------------------------------------------------------------
 *
 * UserTimeThreadUnlock --
 *
 *	Unlock given thread-private time state
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Lock released.
 *
 *----------------------------------------------------------------------
 */
static INLINE void 
UserTimeThreadUnlock(UserTime_ThreadInfo* tti)
{
   SP_Unlock(&tti->lock);
}


/*
 *----------------------------------------------------------------------
 *
 * UserTimeRealCB --
 *
 *      Timer callback for LINUX_ITIMER_REAL
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May send a signal.  
 *
 *----------------------------------------------------------------------
 */
static void
UserTimeRealCB(void *data, UNUSED_PARAM(Timer_AbsCycles timestamp))
{
   World_Handle *world;

   world = World_Find((World_ID) data);
   if (!world) {
      return;
   }

   if (World_IsUSERWorld(world)) {
      PCPU pcpu;

      // Move timer if world moved.  This is not needed for
      // correctness, but we do it in an attempt to improve locality
      // and load balancing.
      pcpu = world->sched.cpu.vcpu.pcpu;
      if (MY_PCPU != pcpu) {
         Timer_AbsCycles deadlineTS;
         Timer_RelCycles periodTS;
         Bool found;
         UserTime_ThreadInfo *tti = &world->userThreadInfo->time;

         UserTimeThreadLock(tti);
         Timer_GetTimeoutTC(tti->realTimer, &deadlineTS, &periodTS);
         found = Timer_Remove(tti->realTimer);
         if (found) {
            tti->realTimer = Timer_AddTC(pcpu, DEFAULT_GROUP_ID,
                                         UserTimeRealCB,
                                         deadlineTS, periodTS,
                                         (void *) world->worldID);
         }
         UserTimeThreadUnlock(tti);
      }

      // Send the signal
      UserSig_Send(world, LINUX_SIGALRM);
   }
   World_Release(world);
}


/*
 *----------------------------------------------------------------------
 *
 * UserTimeProfStart --
 *
 *      A profiling timer (LINUX_ITIMER_VIRTUAL or LINUX_ITIMER_PROF)
 *      is being started.  Increment the count of running timers.  If
 *      the count was previously zero, start the real timers that do
 *      the underlying sampling.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      As noted above.
 *
 *----------------------------------------------------------------------
 */
static void
UserTimeProfStart()
{
   SP_Lock(&userTimeInfo.lock);
   if (userTimeInfo.profiled++ == 0) {
      int i;
      for (i = 0; i < numPCPUs; i++) {
         userTimeInfo.timers[i] = Timer_Add(i, UserTimeProfCB,
                                            USER_TIME_SAMPLE_MSECS,
                                            TIMER_PERIODIC, NULL);
      }
   }
   SP_Unlock(&userTimeInfo.lock);
}


/*
 *----------------------------------------------------------------------
 *
 * UserTimeProfStop --
 *
 *      A profiling timer (LINUX_ITIMER_VIRTUAL or LINUX_ITIMER_PROF)
 *      is being stopped.  Decrement the count of running timers.  If
 *      the count goes to zero, stop the real timers that do the
 *      underlying sampling.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      As noted above.
 *
 *----------------------------------------------------------------------
 */
static void
UserTimeProfStop()
{
   SP_Lock(&userTimeInfo.lock);
   ASSERT(userTimeInfo.profiled != 0);
   if (--userTimeInfo.profiled == 0) {
      int i;
      for (i = 0; i < numPCPUs; i++) {
         Timer_Remove(userTimeInfo.timers[i]);
      }
   }
   SP_Unlock(&userTimeInfo.lock);
}


/*
 *----------------------------------------------------------------------
 *
 * UserTimeProfCountdown --
 *
 *      Count down a profiling timer.
 *
 * Results:
 *      TRUE if the timer should fire.
 *
 * Side effects:
 *      As above.
 *
 *----------------------------------------------------------------------
 */
static Bool
UserTimeProfCountdown(UserTime_ProfTimer *pt)
{
   Bool fire;

   if (!pt->remaining) {
      return FALSE;
   }
   pt->remaining -= userTimeInfo.sampleTC;
   fire = pt->remaining <= 0;
   if (fire) {
      if (pt->period) {
         pt->remaining += pt->period;
      } else {
         pt->remaining = 0;
         UserTimeProfStop();
      }
   }
   return fire;
}


/*
 *----------------------------------------------------------------------
 *
 * UserTimeProfCB --
 *
 *      Timer callback for LINUX_ITIMER_VIRTUAL and LINUX_ITIMER_PROF
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May count down current world's virtual and/or prof timers.
 *      May send it a signal.  
 *
 * Bugs:
 *      We currently don't check whether we interrupted the world from
 *      user or kernel mode, so the virtual timer is incorrect.  It
 *      behaves the same as the prof timer, counting both user and
 *      system time.
 *
 *----------------------------------------------------------------------
 */
static void
UserTimeProfCB(void *data, UNUSED_PARAM(Timer_AbsCycles timestamp))
{
   World_Handle *world = MY_RUNNING_WORLD;

   ASSERT(!CpuSched_IsPreemptible());

   if (World_IsUSERWorld(world)) {
      UserTime_ThreadInfo *tti = &world->userThreadInfo->time;
      Bool fireVirtual, fireProf;

      UserTimeThreadLock(tti);
      fireVirtual = UserTimeProfCountdown(&tti->virtualTimer);
      fireProf = UserTimeProfCountdown(&tti->profTimer);
      UserTimeThreadUnlock(tti);
      if (fireVirtual) {
         UserSig_Send(world, LINUX_SIGVTALRM);
      }
      if (fireProf) {
         UserSig_Send(world, LINUX_SIGPROF);
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * UserTimeTSToUS --
 *
 *      Convert timestamp units to microseconds and store in a
 *      LinuxITimerVal.
 *
 *----------------------------------------------------------------------
 */
static void
UserTimeTSToUS(LinuxITimerVal* itv,
               Timer_RelCycles remainingTC, Timer_RelCycles periodTC)
{
   int64 remainingUS, periodUS;
   Timer_RelCycles roundupTC;

   // convert units, rounding up
   roundupTC = Timer_NSToTC(500);
   remainingUS = Timer_TCToUS(remainingTC + roundupTC);
   periodUS = Timer_TCToUS(periodTC + roundupTC);

   itv->value.tv_sec = remainingUS / 1000000;
   itv->value.tv_usec = remainingUS % 1000000;
   itv->interval.tv_sec = periodUS / 1000000;
   itv->interval.tv_usec = periodUS % 1000000;
}


/*
 *----------------------------------------------------------------------
 *
 * UserTime_SetITimer --
 *
 *      Set a userworld interval timer
 *
 * Results:
 *      VMK_OK or error.  Previous setting stored in *oitv.
 *
 * Side effects:
 *      May add/remove a vmkernel timer.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserTime_SetITimer(LinuxITimerWhich which,
                   const LinuxITimerVal* itv,
                   LinuxITimerVal* oitv)
{
   UserTime_ThreadInfo *tti = &MY_USER_THREAD_INFO->time;
   Timer_RelCycles oldRemaining, oldPeriod, newRemaining, newPeriod;
   Timer_AbsCycles now = Timer_GetCycles();
   VMK_ReturnStatus status = VMK_OK;

   oldRemaining = 0;
   oldPeriod = 0;
   newRemaining = Timer_USToTC(itv->value.tv_sec * 1000000LL +
                               itv->value.tv_usec);
   newPeriod = Timer_USToTC(itv->interval.tv_sec * 1000000LL +
                            itv->interval.tv_usec);
   if (newRemaining < 0 || newPeriod < 0 ||
       (0 < newPeriod && newPeriod < TIMER_MIN_PERIOD)) {
      // Disallow negative values or overly short periods.
      return VMK_BAD_PARAM;
   }

   UserTimeThreadLock(tti);
   switch (which) {
   case LINUX_ITIMER_REAL:
      /*
       * Set a vmkernel timer to go off exactly when requested.
       */
      if (tti->realTimer != TIMER_HANDLE_NONE) {
         Timer_AbsCycles oldDeadline;
         if (Timer_GetTimeoutTC(tti->realTimer, &oldDeadline, &oldPeriod)) {
            oldRemaining = oldDeadline > now ? oldDeadline - now : 0;
            (void) Timer_Remove(tti->realTimer);
         }
      }
      tti->realTimer = Timer_AddTC(MY_PCPU, DEFAULT_GROUP_ID,
                                   UserTimeRealCB,
                                   now + newRemaining, newPeriod,
                                   (void *) MY_RUNNING_WORLD->worldID);
      break;

   case LINUX_ITIMER_VIRTUAL:
      oldRemaining = tti->virtualTimer.remaining;
      oldPeriod = tti->virtualTimer.period;
      tti->virtualTimer.remaining = newRemaining;
      tti->virtualTimer.period = newPeriod;
      if (oldRemaining) {
         if (!newRemaining) {
            UserTimeProfStop();
         }
      } else {
         if (newRemaining) {
            UserTimeProfStart();
         }
      }
      break;

   case LINUX_ITIMER_PROF:
      oldRemaining = tti->profTimer.remaining;
      oldPeriod = tti->profTimer.period;
      tti->profTimer.remaining = newRemaining;
      tti->profTimer.period = newPeriod;
      if (oldRemaining) {
         if (!newRemaining) {
            UserTimeProfStop();
         }
      } else {
         if (newRemaining) {
            UserTimeProfStart();
         }
      }
      break;

   default:
      status = VMK_BAD_PARAM;
      break;
   }
   UserTimeThreadUnlock(tti);

   if (oitv) {
      UserTimeTSToUS(oitv, oldRemaining, oldPeriod);
   }

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * UserTime_GetITimer --
 *
 *      Get time remaining on a userworld interval timer.
 *
 * Results:
 *      VMK_OK or error.  Time remaining stored in *itv.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserTime_GetITimer(LinuxITimerWhich which,
                   LinuxITimerVal* itv)
{
   UserTime_ThreadInfo *tti = &MY_USER_THREAD_INFO->time;
   Timer_RelCycles remaining = 0, period = 0;
   Timer_AbsCycles now = Timer_GetCycles();
   VMK_ReturnStatus status = VMK_OK;

   UserTimeThreadLock(tti);
   switch (which) {
   case LINUX_ITIMER_REAL:
      if (tti->realTimer != TIMER_HANDLE_NONE) {
         Timer_AbsCycles deadline;
         if (Timer_GetTimeoutTC(tti->realTimer, &deadline, &period)) {
            remaining = deadline > now ? deadline - now : 0;
         }
      }
      break;

   case LINUX_ITIMER_VIRTUAL:
      remaining = tti->virtualTimer.remaining;
      period = tti->virtualTimer.period;
      break;

   case LINUX_ITIMER_PROF:
      remaining = tti->profTimer.remaining;
      period = tti->profTimer.period;
      break;

   default:
      status = VMK_BAD_PARAM;
      break;
   }
   UserTimeThreadUnlock(tti);

   UserTimeTSToUS(itv, remaining, period);
   return status;
}

