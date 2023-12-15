/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * linuxTime.c --
 *
 *	Linux time-related syscalls.
 */

#include "user_int.h"
#include "linuxTime.h"
#include "timer.h"

#define LOGLEVEL_MODULE LinuxTime
#include "userLog.h"

/*
 *----------------------------------------------------------------------
 *
 * LinuxTime_Time - 
 *	Handler for linux syscall 13 
 * Support: 100%
 * Error case: 100%
 *
 * Results:
 *      Unix time of day in seconds.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
LinuxTimeT
LinuxTime_Time(UserVA /* LinuxTimeT* */ tm)
{
   VMK_ReturnStatus status;

   LinuxTimeT sec = Timer_GetTimeOfDay() / 1000000;
   if (tm) {
      status = User_CopyOut(tm, &sec, sizeof(tm));
      if (status != VMK_OK) {
         return User_TranslateStatus(status);
      }
   }
   return sec;
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxTime_Gettimeofday - 
 *	Handler for linux syscall 78 
 * Support: 60% (no timezone support)
 * Error case: 100%
 *
 * Results:
 *      Unix time of day in seconds and microseconds.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
int
LinuxTime_Gettimeofday(UserVA /* LinuxTimeval* */ tvp,
                       UserVA /* LinuxTimezone* */ tzp)
{
   int rc = 0;
   VMK_ReturnStatus status = VMK_OK;

   UWLOG_SyscallEnter("tv@%#x tz@%#x", tvp, tzp);

   if (tvp) {
      LinuxTimeval tv;
      int64 usec;

      usec = Timer_GetTimeOfDay();
      tv.tv_sec = usec / 1000000;
      tv.tv_usec = usec % 1000000;

      status = User_CopyOut(tvp, &tv, sizeof(tv));
   }

   if (status == VMK_OK && tzp) {
      LinuxTimezone tz;

      /*
       * For now we always return 0, as the VMX doesn't need to know
       * the host timezone.
       */
      tz.tz_minuteswest = 0;
      tz.tz_dsttime = 0;

      status = User_CopyOut(tzp, &tz, sizeof(tz));
   }

   rc = User_TranslateStatus(status);
   return rc;
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxTime_Settimeofday - 
 *	Handler for linux syscall 79 
 * Support: 60% (no timezone support)
 * Error case: 100%
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Changes time of day in seconds and microseconds.
 *
 *----------------------------------------------------------------------
 */
int
LinuxTime_Settimeofday(UserVA /*const LinuxTimeval* */ tvp,
                       UserVA /*const LinuxTimezone* */ tzp)
{
   int rc = 0;
   VMK_ReturnStatus status = VMK_OK;

   UWLOG_SyscallEnter("(tvp=%#x, tzp=%#x)", tvp, tzp);

   if (tvp) {
      LinuxTimeval tv;

      status = User_CopyIn(&tv, tvp, sizeof(tv));
      if (status == VMK_OK) {
         Timer_SetTimeOfDay(tv.tv_sec * 1000000LL + tv.tv_usec);
      }
   }

   if (status == VMK_OK && tzp) {
      /*
       * For now we ignore attempts to set the timezone.  See
       * LinuxTime_Gettimeofday.
       */
      // LinuxTimezone tz;

      // status = User_CopyIn(&tz, (void*)tzp, sizeof(tz));

      UWLOG(0, "timezone ignored");
   }

   if (status != VMK_OK) {
      rc = LINUX_EFAULT;
   }

   return rc;
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxTime_Setitimer - 
 *	Handler for linux syscall 104 
 * Support: 83% (ITIMER_VIRTUAL runs during both user and system time,
 *               like ITIMER_PROF)
 * Error case: 100%
 *
 * Results:
 *      0 or negative Linux error code.
 *
 * Side effects:
 *      Sets an interval timer.
 *
 *----------------------------------------------------------------------
 */
int
LinuxTime_Setitimer(LinuxITimerWhich which,
                    UserVAConst /* LinuxITimerVal* */ userItv,
                    UserVA /* LinuxITimerVal* */ userOitv)
{
   LinuxITimerVal itv, oitv;
   VMK_ReturnStatus status;

   status = User_CopyIn(&itv, userItv, sizeof(itv));
   if (status != VMK_OK) {
      return User_TranslateStatus(status);
   }

   status = UserTime_SetITimer(which, &itv, userOitv ? &oitv : NULL);
   if (status != VMK_OK) {
      return User_TranslateStatus(status);
   }

   if (userOitv) {
      status = User_CopyOut(userOitv, &oitv, sizeof(oitv));
      return User_TranslateStatus(status);
   }
   return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxTime_Getitimer - 
 *	Handler for linux syscall 105 
 * Support: 100%
 * Error case: 100%
 *
 * Results:
 *      Returns 0 or negative Linux error code.
 *      Stores timer info in *itv.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
int
LinuxTime_Getitimer(LinuxITimerWhich which,
                    UserVA /* LinuxITimerVal* */ userItv)
{
   LinuxITimerVal itv;
   VMK_ReturnStatus status;

   status = UserTime_GetITimer(which, &itv);
   if (status != VMK_OK) {
      return User_TranslateStatus(status);
   }

   status = User_CopyOut(userItv, &itv, sizeof(itv));
   return User_TranslateStatus(status);
}
