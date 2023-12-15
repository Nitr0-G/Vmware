/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * linuxThread.c -
 *	Linux thread and scheduling related syscalls and glue
 */

#include "user_int.h"
#include "linuxThread.h"
#include "linuxSignal.h"
#include "userSig.h"
#include "nmi.h"

#define LOGLEVEL_MODULE LinuxThread
#include "userLog.h"

#define LINUX_WAIT_NOHANG	0x1
#define LINUX_WAIT_UNTRACED	0x2
#define LINUX_WAIT_CLONE	0x80000000

#define LINUX_WAIT_EXITCODEBITS  0xff00
#define LINUX_WAIT_EXITCODESHIFT      8
#define LINUX_WAIT_EXITSIGBITS   0x007f
#define LINUX_WAIT_EXITSIGSHIFT       0
#define LINUX_WAIT_COREBIT       0x0080

/*
 * Clone flags argument has two parts, bottom 8 bits are a death
 * signal id, top bits are actual flags.
 */
#define LINUX_CLONE_SIGMASK   0x00FF
#define LINUX_CLONE_FLAGSMASK (~LINUX_CLONE_SIGMASK)
/* Clone flags: */
#define LINUX_CLONE_VM        0x0100
#define LINUX_CLONE_FS        0x0200
#define LINUX_CLONE_FILES     0x0400
#define LINUX_CLONE_SIGHAND   0x0800
#define LINUX_CLONE_PID       0x1000
#define LINUX_CLONE_PTRACE    0x2000
#define LINUX_CLONE_VFORK     0x4000
/*#define LINUX_CLONE_PARENT  xxx */
/*#define LINUX_CLONE_THREAD  xxx */

/* scheduler policy types: */
#define LINUX_SCHED_OTHER     0
#define LINUX_SCHED_FIFO      1
#define LINUX_SCHED_RR        2


/*
 *----------------------------------------------------------------------
 *
 * LinuxThread_PidForWorldID
 *
 *	Return the linux pid associated with the given WorldID.
 *
 * Results:
 *	A pid.
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
LinuxPid
LinuxThread_PidForWorldID(World_ID wid) {
   LinuxPid pid;
   ASSERT(sizeof(LinuxPid) == sizeof(World_ID));
   if (wid != INVALID_WORLD_ID) {
      pid = (LinuxPid)wid + LINUX_PID_OFFSET;
   } else {
      pid = INVALID_LINUX_PID;
   }

   if (pid < 0) {
      UWWarn("Returning invalid Linux pid (%d)! Derived from world id %d",
	     pid, wid);
   }

   return pid;
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxThread_WorldIDForPid
 *
 *	Return the WorldID associated with given pid.
 *
 * Results:
 *	A WorldID.
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
World_ID
LinuxThread_WorldIDForPid(LinuxPid pid) {
   ASSERT(sizeof(LinuxPid) == sizeof(World_ID));
   if (pid >= LINUX_PID_OFFSET) {
      return pid - LINUX_PID_OFFSET;
   } else {
      UWWarn("Passed invalid Linux pid (%d)!  Returning INVALID_WORLD_ID.",
	     pid);
      return INVALID_WORLD_ID;
   }
}


/* 
 *----------------------------------------------------------------------
 *
 * LinuxThread_Getpid - 
 *	Handler for linux syscall 20 
 * Support: 100%
 * Error case: 100%
 * Results:
 *	Linux pid associated with the current world
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
int
LinuxThread_Getpid(void)
{
   World_ID currID = MY_RUNNING_WORLD->worldID;
   UWLOG(1, "returning %d", LinuxThread_PidForWorldID(currID));
   return LinuxThread_PidForWorldID(currID);
}


/* 
 *----------------------------------------------------------------------
 *
 * LinuxThread_Clone - 
 *	Handler for linux syscall 120 
 * Support: 30% (must clone fs,files,sighand,vm; must supply stack)
 * Error case: 100%
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
int 
LinuxThread_Clone(int32 linuxFlags, 		// IN: clone flags
                  UserVA stack)			// IN: user-mode stack for new thread
{
   static const int supportedFlags = (LINUX_CLONE_FS|LINUX_CLONE_FILES
                                      |LINUX_CLONE_SIGHAND|LINUX_CLONE_VM);
   UserSigId deathSig;
   int32 flags;
   World_Handle* world;
   VMK_ReturnStatus status;
   char nameBuf[WORLD_NAME_LENGTH];
   Reg32 userEIP;

   ASSERT(MY_USER_THREAD_INFO->exceptionFrame != NULL);
   userEIP = MY_USER_THREAD_INFO->exceptionFrame->frame.eip;

   deathSig = LinuxSignal_ToUserSignal(linuxFlags & LINUX_CLONE_SIGMASK);
   flags = (linuxFlags & LINUX_CLONE_FLAGSMASK);
   
   UWLOG_SyscallEnter("(childSig=%d, flags=%#x, stack=%#x)",
                      deathSig, flags, stack);

   /*
    * Callers that set deathSig to SIGCHLD are probably going outside
    * the scope of our Linux compatibility.  (Linux uses deathSig as
    * SIGCHLD to differentiate "clone" from normal processes in wait4,
    * see waitpid man page).
    */
   if (deathSig == LINUX_SIGCHLD) {
      UWWarn("Setting deathSig to SIGCHLD breaks __WCLONE support in wait4");
      UWLOG_StackTraceCurrent(1);
   }

   /* Make sure we're passed the limited subset of flags we implement. */
   if (flags != supportedFlags) {
      UWWarn("Unsupported flags %#x (from %#x)", flags&~supportedFlags, flags);
      UWLOG_StackTraceCurrent(1);
      return LINUX_EINVAL;
   }

   /* We require a stack argument. */
   if (stack == 0) {
      UWWarn("Unsupported null stack argument");
      UWLOG_StackTraceCurrent(1);
      return LINUX_EINVAL;
   }

   /* Make up a "name" for the new world. */
   ASSERT(MY_RUNNING_WORLD->worldName != NULL);
   snprintf(nameBuf, sizeof nameBuf, "%s", MY_RUNNING_WORLD->worldName);

   /* Create and start the clone. */
   status = UserThread_Clone(nameBuf, userEIP, stack,
                             deathSig, MY_RUNNING_WORLD->worldID,
                             &world);

   if (status == VMK_OK) {
      return LinuxThread_PidForWorldID(world->worldID);
   } else {
      return User_TranslateStatus(status);
   }
}

/* 
 *----------------------------------------------------------------------
 *
 * LinuxThread_Waitpid - 
 *	Handler for linux syscall 7 
 * Support: 0% (use wait4)
 * Error case: 
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
int
LinuxThread_Waitpid(LinuxPid pid,
                    UserVA userOutStatus,
                    int32 options)
{
   UWLOG_SyscallUnsupported("(pid=%d, status@%#x, options=%#x) -- use wait4",
                            pid, userOutStatus, options);
   return LINUX_ENOSYS;
}

/* 
 *----------------------------------------------------------------------
 *
 * LinuxThread_Wait4 - 
 *	Handler for linux syscall 114 
 * Support: 40% (must supply untraced,nohang,clone flags;
 *	no group or inter-process wait; termSig not set; interruption
 *	semantics -- EINTER on delivery of unblocked signal -- not
 *	supported.)
 * Error case: 100%
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
int
LinuxThread_Wait4(LinuxPid linuxPid,
                  UserVA userOutStatus,
                  int32 options,
                  UserVA userRusage)
{
   int kstatus = 0;
   int exitCode = 0;
   World_ID worldID;
   VMK_ReturnStatus status;
   User_CartelInfo* const uci = MY_USER_CARTEL_INFO;
   Bool blocking;

   UWLOG_SyscallEnter("(pid=%d, status@%#x, options=%#x, rusage@%#x)",
                      linuxPid, userOutStatus, options, userRusage);

   /* Unrecognized options flags are simply ignored */
   if ((options & ~(LINUX_WAIT_NOHANG|LINUX_WAIT_CLONE)) != 0) {
      /*
       * Known unsupported flags include:
       *   WUNTRACED: for detecting stopped (but not dead) children
       *   __WALL: ignore "clone" vs. non-clone distinction
       *   __WNOTHREAD: exclude peer-thread's children (?)
       * Unknown unsupported flags may exist.
       */
      UWWarn("Unsupported wait flags %#x.  Ignoring.",
             options & ~(LINUX_WAIT_NOHANG|LINUX_WAIT_CLONE));
      UWLOG_StackTraceCurrent(1);
   }

   /*
    * Four cases for linuxPid:
    *    A specific world id (positive number): wait for that pid
    *    -1: wait for any child (we wait for any thread in cartel)
    *    less than -1: wait for specific group (NOT SUPPORTED)
    *    0: wait for current group (we map to -1 behavior)
    */
   if (linuxPid < -1) {
      UWWarn("Waiting on specific group (%d) not supported.", linuxPid);
      UWLOG_StackTraceCurrent(1);
      return LINUX_ENOSYS;
   }

   if (userRusage != 0) {
      UWWarn("Non-null rusage being IGNORED.");
      UWLOG_StackTraceCurrent(1);
   }

   if (linuxPid == 0 || linuxPid == -1) {
      /*
       * We make no distinction between waiting on "child" threads and
       * waiting on other threads in this cartel.
       */
      linuxPid = -1;
      worldID = INVALID_WORLD_ID;
   } else {
      worldID = LinuxThread_WorldIDForPid(linuxPid);
   }

   /*
    * LINUX_WAIT_CLONE: wait only for "threads" (IGNORED)
    *
    * A bit of a hack for handling __WCLONE.  Does not handle case
    * where worldID == INVALID_WORLD_ID (i.e., waiting on any thread),
    * but does handle the case we'll run into from LinuxThreads.
    */
   if ((worldID == uci->cartelID)
       && (options & LINUX_WAIT_CLONE)) {
      UWLOG(2, "Wait for cartel-leader (%d) with __WCLONE is a no-op.",
            worldID);
      return LINUX_ECHILD;
   }

   // LINUX_WAIT_NOHANG: non-blocking check for dead children
   blocking = ((options & LINUX_WAIT_NOHANG) == 0);

   status = UserThread_Collect(&uci->peers, &worldID, blocking, &exitCode);
   if (status == VMK_OK) {
      ASSERT(worldID != INVALID_WORLD_ID);

      if (worldID == uci->cartelID) {
         UWWarn("Reaped the initial thread in the cartel.");
      }

      /* Generate the linux status festival of bits. */
      if (userOutStatus != 0) {
         static const int core = 0; // only intra-cartel waits, so must be 0
         static const int termSig = 0; // No storage of termination signals
         
         kstatus |= ((exitCode << LINUX_WAIT_EXITCODESHIFT) & LINUX_WAIT_EXITCODEBITS);
         kstatus |= ((termSig << LINUX_WAIT_EXITSIGSHIFT) & LINUX_WAIT_EXITSIGBITS);
         if (core) {
            kstatus |= LINUX_WAIT_COREBIT;
         }
         status = User_CopyOut(userOutStatus, &kstatus, sizeof(kstatus));
      }

      if (status == VMK_OK) {
         return LinuxThread_PidForWorldID(worldID);
      }
   } 
   
   ASSERT(status != VMK_OK);
   return User_TranslateStatus(status);
}


/* 
 *----------------------------------------------------------------------
 *
 * LinuxThread_SchedSetparam - 
 *	Handler for linux syscall 154 
 * Support: 0%
 * Error case:
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
int
LinuxThread_SchedSetparam(LinuxPid pid, UserVA l_sched_param)
{
   UWLOG_SyscallUnimplemented("(pid=%u, param@%#x)", pid, l_sched_param);
   return LINUX_ENOSYS;
}

/* 
 *----------------------------------------------------------------------
 *
 * LinuxThread_SchedGetparam - 
 *	Handler for linux syscall 155 
 * Support: 0%
 * Error case:
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
int
LinuxThread_SchedGetparam(LinuxPid pid, UserVA l_sched_param)
{
   UWLOG_SyscallUnimplemented("(pid=%u, param@%#x)", pid, l_sched_param);
   return LINUX_ENOSYS;
}

/* 
 *----------------------------------------------------------------------
 *
 * LinuxThread_SchedSetscheduler - 
 *	Handler for linux syscall 156 
 * Support: 20% (Only support SCHED_OTHER on current thread)
 * Error case: 100%
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
int
LinuxThread_SchedSetscheduler(LinuxPid linuxPid, int32 policy, UserVA linuxSchedParam)
{
   World_ID worldID;
   int rc;

   UWLOG_SyscallEnter("(pid=%u, policy=%d, param@%#x)",
                      linuxPid, policy, linuxSchedParam);

   worldID = LinuxThread_WorldIDForPid(linuxPid);

   if (worldID != MY_RUNNING_WORLD->worldID) {
      UWLOG(0,
            "Cannot set scheduling policy for other than current pid (trying to hit %d)",
            linuxPid);
      rc = LINUX_EINVAL;
   } else {
      switch(policy)
      {
      case LINUX_SCHED_OTHER:
         /* Basically this is the only option that works. */
         rc = 0;
         break;
      case LINUX_SCHED_FIFO:
      case LINUX_SCHED_RR:
         /* XXX unimplemented. */
         UWWarn("(pid=%d) Ignoring valid policy %d\n", linuxPid, policy);
         UWLOG_StackTraceCurrent(1);
         rc = LINUX_EINVAL;
         break;
      default:
         rc = LINUX_EINVAL;
         break;
      }
   }
   return rc;
}

/* 
 *----------------------------------------------------------------------
 *
 * LinuxThread_SchedGetscheduler - 
 *	Handler for linux syscall 157 
 * Support: 100% (though we always just return SCHED_OTHER)
 * Error case: 100%
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
int
LinuxThread_SchedGetscheduler(LinuxPid pid)
{
   UWLOG_SyscallEnter("(pid=%u) -> always returns SCHED_OTHER.",
                      pid);

   /*
    * LINUX_SCHED_OTHER is the only policy that we support, so its
    * the policy in use by the given pid.
    */

   return LINUX_SCHED_OTHER;
}

/* 
 *----------------------------------------------------------------------
 *
 * LinuxThread_SchedYield - 
 *	Handler for linux syscall 158 
 * Support: 100%
 * Error case: 100%
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
int
LinuxThread_SchedYield(void)
{
   CpuSched_YieldThrottled();
   return 0;
}

/* 
 *----------------------------------------------------------------------
 *
 * LinuxThread_SchedGetMaxPriority - 
 *	Handler for linux syscall 159 
 * Support: 0%
 * Error case:
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
int
LinuxThread_SchedGetMaxPriority(int32 policy)
{
   UWLOG_SyscallUnimplemented("(policy=%d)", policy);
   return LINUX_ENOSYS;
}

/* 
 *----------------------------------------------------------------------
 *
 * LinuxThread_SchedGetMinPriority - 
 *	Handler for linux syscall 160 
 * Support: 0%
 * Error case:
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
int
LinuxThread_SchedGetMinPriority(int32 policy)
{
   UWLOG_SyscallUnimplemented("(policy=%d)", policy);
   return LINUX_ENOSYS;
}

/* 
 *----------------------------------------------------------------------
 *
 * LinuxThread_SchedGetRRInterval - 
 *	Handler for linux syscall 161 
 * Support: 0%
 * Error case:
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
int
LinuxThread_SchedGetRRInterval(LinuxPid pid, UserVA intervalTimespec)
{
   UWLOG_SyscallUnimplemented("(pid=%u, interval@%#x)", pid, intervalTimespec);
   return LINUX_ENOSYS;
}

/* 
 *----------------------------------------------------------------------
 *
 * LinuxThread_Nanosleep - 
 *	Handler for linux syscall 162 
 * Support: 100%
 * Error case: 100%
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
int
LinuxThread_Nanosleep(UserVA requestTimespec, // IN
                      UserVA remainTimespec) // OUT
{
   VMK_ReturnStatus status;
   LinuxTimespec kreqspec;
   int rc;
   
   UWLOG_SyscallEnter("(req@%#x, remain@%#x)",
                      requestTimespec, remainTimespec);

   status = User_CopyIn(&kreqspec, requestTimespec, sizeof kreqspec);
   if (status != VMK_OK) {
      rc = User_TranslateStatus(status);
   } else if ((kreqspec.nanoseconds >= (1000*1000*1000))
             || (kreqspec.seconds < 0)) {
      UWLOG(1, "Invalid request (secs=%d, nanos=%d)",
	    kreqspec.seconds, kreqspec.nanoseconds);
      rc = LINUX_EINVAL;
   } else {
      Timer_RelCycles remainInTC;
      Timer_RelCycles sleepInTC;

      sleepInTC = Timer_NSToTC((int64)kreqspec.seconds * 1000*1000*1000LL +
                               kreqspec.nanoseconds);

      UWLOG(3, "%ds + %uns = %Ld cycles",
            kreqspec.seconds, kreqspec.nanoseconds, sleepInTC);

      remainInTC = UserThread_Sleep(sleepInTC);
      
      UWLOG(3, "woke from sleep with %Ld cycles left",
            remainInTC);

      /* Were we interrupted? */
      if (remainInTC > 0) {
         rc = LINUX_EINTR;
      } else {
         /* overwaiting is ignored. */
         remainInTC = 0;
         rc = 0;
      }
      
      /*
       * Note that we only copy the remainder spec out if we were
       * interrupted.  If the sleep completed, then we don't bother
       * over-writing remainspec, since no one should look at it...
       *
       * This is the linux behavior.
       */
      if ((rc != 0)
          && (remainTimespec != 0)) {
         DEBUG_ONLY(Timer_RelCycles fullRemainTC = remainInTC);
         LinuxTimespec kremainspec;
         int64 nanosecs;

         /*
          * Since input is 31bit number of seconds, I don't have to worry
          * about overflow for computing remainder time.
          */
         nanosecs = Timer_TCToNS(remainInTC);
         kremainspec.seconds = nanosecs / (1000*1000*1000LL);
         kremainspec.nanoseconds = nanosecs % (1000*1000*1000LL);

         UWLOG(3, " %Ld cycles = %Ld nanos = %ds + %uns",
               fullRemainTC,
               nanosecs, kremainspec.seconds, kremainspec.nanoseconds);

         status = User_CopyOut(remainTimespec, &kremainspec, sizeof kremainspec);
         if (status != VMK_OK) {
            rc = User_TranslateStatus(status);
         }
      }
   }
   return rc;
}


/* 
 *----------------------------------------------------------------------
 *
 * LinuxThread_Getppid - 
 *	Handler for linux syscall 64 
 * Support: 30% (Just enough for pthread_manager semantics)
 * Error case: 0%
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
int
LinuxThread_Getppid(void)
{
   const User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   LinuxPid ppid;

   /*
    * Only used by GLIBC pthreads.c:__pthread_manager().
    *
    * This is used by the pthread manager to determine if the main
    * thread of a process has gone away.  In Linux, if the main thread
    * of a process dies Linux re-parents the manager thread to the
    * init process.  So, we return the initial thread id (thankfully
    * handy as the cartel id).  However, if that thread is dead, we
    * return 1.
    *
    */
   if (World_Exists(uci->cartelID)) {
      ppid = LinuxThread_PidForWorldID(uci->cartelID);
   } else {
      ppid = (LinuxPid)1; /* reparented to init process. */
   }

   UWLOG_SyscallEnter(
         "WARNING: getppid is a hack specifically for pthreads (returning %d)",
         ppid);
   return ppid;
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxThread_Setpgid - 
 *	Handler for linux syscall 57 
 * Support: 0%
 *      XXX Joe's call graph shows this can be used by the VMX,
 *      however, I think implementing it as a no-op will be ok.  --mann
 * Error case:
 *
 * Results:
 *      0 or Linux error code.
 *
 * Side effects:
 *      Set the process group identifier of pid to pgid.  Note that a
 *      process group identifier (pgid) has nothing to do with a user
 *      group identifier (gid).
 *
 *----------------------------------------------------------------------
 */
int
LinuxThread_Setpgid(LinuxPid pid,
                    LinuxPid pgid)
{
   UWLOG_SyscallUnsupported("(pid=%d, pgid=%d)", pid, pgid);
   return LINUX_ENOSYS;
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxThread_Getpgrp - 
 *	Handler for linux syscall 65 
 * Support: 0%
 * Error case:
 *
 * Results:
 *      Returns the process group identifer (pgid) of the currrent
 *      process; equivalent to getpgid(0).
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
LinuxPid
LinuxThread_Getpgrp(void)
{
   UWLOG_SyscallUnsupported("(void)");
   return LINUX_ENOSYS;
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxThread_Getpgid - 
 *	Handler for linux syscall 132 
 * Support: 0%
 * Error case:
 *
 * Results:
 *      Returns the process group identifer (pgid) of pid.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
LinuxPid
LinuxThread_Getpgid(LinuxPid pid)
{
   UWLOG_SyscallUnsupported("(pid=%d)", pid);
   return LINUX_ENOSYS;
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxThread_Setsid - 
 *	Handler for linux syscall 66 
 * Support: 0%
 * Error case:
 *
 * Results:
 *      New process group id.
 *
 * Side effects:
 *      Creates a session and sets the process group ID.
 *
 *----------------------------------------------------------------------
 */
LinuxPid
LinuxThread_Setsid(void)
{
   UWLOG_SyscallUnsupported("(void)");
   return LINUX_ENOSYS;
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxThread_Getsid - 
 *	Handler for linux syscall 147 
 * Support: 0%
 * Error case:
 *
 * Results:
 *      Returns the session ID of process pid.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
LinuxPid
LinuxThread_Getsid(LinuxPid pid)
{
   UWLOG_SyscallUnsupported("pid=%d", pid);
   return LINUX_ENOSYS;
}
