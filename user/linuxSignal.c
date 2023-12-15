/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * Contains code generated from code which had LICENSE A and
 * other code which had LICENSE B.  The "adversiting clause"
 * in LICENSE B has been retroactivley deleted. (see:
 *     ftp://ftp.cs.berkeley.edu/pub/4bsd/README.Impt.License.Change
 * ). 
 *
 * LICENSE A:
 * Copyright 1994-2003 FreeBSD, Inc. All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 *    1. Redistributions of source code must retain the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer.
 *    2. Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials
 *       provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE FREEBSD PROJECT ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE FREEBSD PROJECT OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * 
 * The views and conclusions contained in the software and
 * documentation are those of the authors and should not be
 * interpreted as representing official policies, either expressed or
 * implied, of the FreeBSD Project or FreeBSD, Inc.
 *
 *
 * LICENSE B:
 *
 * FreeBSD5.0 linux syscalls.master:
 *      $FreeBSD: src/sys/i386/linux/syscalls.master,v 1.48 2002/09/24 07:03:01 mini Exp $
 *       @(#)syscalls.master     8.1 (Berkeley) 7/19/93
 * 
 * FreeBSD5.0 errno.h:
 *
 * Copyright (c) 1982, 1986, 1989, 1993
 *      The Regents of the University of California.  All rights reserved.
 * (c) UNIX System Laboratories, Inc.
 * All or some portions of this file are derived from material licensed
 * to the University of California by American Telephone and Telegraph
 * Co. or Unix System Laboratories, Inc. and are reproduced herein with
 * the permission of UNIX System Laboratories, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *      This product includes software developed by the University of
 *      California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *      @(#)errno.h     8.5 (Berkeley) 1/21/94
 * $FreeBSD: src/sys/sys/errno.h,v 1.25 2002/10/07 06:25:23 phk Exp $
 * 
 */

/*
 * linuxSignal.c --
 *
 *	Support for linux signal-related syscalls.
 */

#include "user_int.h"
#include "linuxSignal.h"
#include "linuxThread.h"

#define LOGLEVEL_MODULE LinuxSignal
#include "userLog.h"

/*
 * sigaction flags:
 */
#define LINUX_SIGACTFLAG_NOCLDSTOP    0x00000001 /* Unsupported. */
#define LINUX_SIGACTFLAG_SIGINFO      0x00000004 /* (Unsupported) Use 3-arg sighandler. */
#define LINUX_SIGACTFLAG_RESTART      0x10000000 /* Unsupported. */
#define LINUX_SIGACTFLAG_NOMASK       0x40000000 /* Handler is reentrant */
#define LINUX_SIGACTFLAG_ONESHOT      0x80000000 /* (Unsupported) Signal is reset to default after firing. */
#define LINUX_SIGACTFLAG_RESTORER     0x04000000 /* Unsupported.  Use sa_restorer */

/*
 * Linux signal action struct passed in/out of sighandler functions
 */
typedef uint64 LinuxSigSet;
typedef UserSig_Handler LinuxSigHandler;
typedef uint32 LinuxSigRestorer;
typedef struct LinuxSigAction {
   LinuxSigHandler	handler;
   uint32		flags;
   LinuxSigRestorer	restorer;
   LinuxSigSet		mask;
} LinuxSigAction;

/*
 * sigprocmask "how" flags
 */
#define LINUX_SIGHOW_BLOCK       0x0
#define LINUX_SIGHOW_UNBLOCK     0x1
#define LINUX_SIGHOW_SETMASK     0x2


/*
 *----------------------------------------------------------------------
 *
 * LinuxSignal_ToUserSignal --
 *
 *	Return the UserWorld signal id for a given Linux signal
 *	number.  Return LINUX_SIG_ERR if no equivalent signal.  Valid
 *	Linux signals which user worlds do not support are mapped to
 *	LINUX_SIG_ERR, too.
 *
 * Results:
 *	A user signal id.
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
UserSigId
LinuxSignal_ToUserSignal(uint32 linuxSig)
{
   /* Bogus signals are mapped to 0 */
   if (linuxSig >= LINUX_NSIG) {
      return LINUX_SIG_ERR;
   }
   return (UserSigId)linuxSig;
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxSignalSetToUserSignalSet --
 *
 *	Convert the given linux signal set (a mask) to a UserSigSet.
 *	Currently identity mapped.
 *
 * Results:
 *	A user signal set
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static INLINE UserSigSet
LinuxSignalSetToUserSignalSet(LinuxSigSet linuxMask)
{
   return (UserSigSet)linuxMask;
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxSignalSetFromUserSignalSet --
 *
 *	Convert the given user signal set (a mask) to a linux signal
 *	set.  Currently identity mapped.
 *
 * Results:
 *	A linux signal set
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static INLINE LinuxSigSet
LinuxSignalSetFromUserSignalSet(UserSigSet userMask)
{
   return (LinuxSigSet)userMask;
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxSignalSetClearSig --
 *
 *	Return a mask with given signal cleared from given mask.
 *
 * Results:
 *	A new signal set
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static INLINE LinuxSigSet 
LinuxSigSetClearSig(LinuxSigSet linuxMask, uint32 signum)
{
   ASSERT(signum > 0 && signum <= LINUX_NSIG);
   return linuxMask & (~ UserSig_IdToMask(signum));
}


/* 
 *----------------------------------------------------------------------
 *
 * LinuxSignal_Kill - 
 *	Handler for linux syscall 37 
 * Support: 30% (intra-process only, no group kill)
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
LinuxSignal_Kill(LinuxPid pid,
                 uint32 linuxSignum)
{
   VMK_ReturnStatus status;
   UserSigId usig;
   int rc;

   UWLOG_SyscallEnter("pid=%u, signum=%u",
                      pid, linuxSignum);

   if ((pid == 0) || (pid == -1) || (pid < -1)) {
      /*
       * Just FYI:
       *   0 == kill current process group
       *  -1 == kill *all* processes but init
       * <-1 == kill process group -pid
       */
      UWWarn(
         "kill(pid=%u, x) GROUP KILL NOT SUPPORTED -- only single thread targets are supported",
         pid);
      UWLOG_StackTraceCurrent(1);
      return LINUX_ENOSYS;
   }

   switch (linuxSignum) {
   case LINUX_SIGSTOP:
   case LINUX_SIGTSTP:
   case LINUX_SIGTTIN:
   case LINUX_SIGTTOU:
   case LINUX_SIGCONT:
      /*
       * These signals are ignored because they are implicitly
       * cartel-wide, and I only want to support per-thread 
       * signals at the moment...
       */
      UWWarn("signal %d has UNIMPLEMENTED cartel-level semantics",
             linuxSignum);
      UWLOG_StackTraceCurrent(1);
      break;
   }

   if (linuxSignum == 0) {
     /*
      * When linuxSignum == 0, we're just checking that the pid is
      * still valid.  For example, VThreadHostProbeThread() uses
      * kill(pid, 0) for checking if a thread is still alive.
      */
     usig = 0;
   } else {
      usig = LinuxSignal_ToUserSignal(linuxSignum);
      if (usig == 0) {
         UWWarn(" illegal/unuspported signum %d -> EINVAL", linuxSignum);
         UWLOG_StackTraceCurrent(1);
         return LINUX_EINVAL;
      }
   }

   rc = 0;
   status = UserSig_LookupAndSend(LinuxThread_WorldIDForPid(pid), usig, TRUE);
   if (status == VMK_NOT_FOUND) {
      UWLOG(1, "kill(pid=%u, x) no such pid found", pid);
      rc = LINUX_ESRCH;
   } else if (status == VMK_BAD_PARAM) {
      UWWarn(
         "kill(pid=%u, x) INTRA-PROCESS KILL NOT SUPPORTED -- keep to yourself",
         pid);
      UWLOG_StackTraceCurrent(1);
      rc = LINUX_EPERM;
   } else if (status != VMK_OK) {
      UWWarn("kill(pid=%u, sig=%d) failed: %d:%s", 
             pid, usig, status, VMK_ReturnStatusToString(status));
      UWLOG_StackTraceCurrent(1);
      rc = User_TranslateStatus(status);
   }

   return rc;
}


/* 
 *----------------------------------------------------------------------
 *
 * LinuxSignal_Signal - 
 *	Handler for linux syscall 48 
 * Support: 0% (use rtsigaction)
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
LinuxSignal_Signal(uint32 sig,
                   UserVA handler)
{
   UWLOG_SyscallUnsupported("use RTSigaction; (%d, %#x)",
                            sig, handler);
   return LINUX_ENOSYS;
}


/* 
 *----------------------------------------------------------------------
 *
 * LinuxSignal_Sigaction - 
 *	Handler for linux syscall 67 
 * Support: 0% (use rtsigaction)
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
LinuxSignal_Sigaction(uint32 sig,
                      UserVA newsa,
                      UserVA oldsa)
{
   UWLOG_SyscallUnsupported("use RTSigaction; (%d, %#x, %#x)",
                                 sig, newsa, oldsa);
   return LINUX_ENOSYS;
}


/* 
 *----------------------------------------------------------------------
 *
 * LinuxSignal_Sgetmask - 
 *	Handler for linux syscall 68 
 * Support: 0% (use rtsigprocmask)
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
LinuxSignal_Sgetmask(void)
{
   UWLOG_SyscallUnsupported("use RTSigprocmask");
   return LINUX_ENOSYS;
}


/* 
 *----------------------------------------------------------------------
 *
 * LinuxSignal_Ssetmask - 
 *	Handler for linux syscall 69 
 * Support: 0% (use rtsigprocmask)
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
LinuxSignal_Ssetmask(LinuxOldSigSet mask)
{
   UWLOG_SyscallUnsupported("use RTSigprocmask");
   return LINUX_ENOSYS;
}


/* 
 *----------------------------------------------------------------------
 *
 * LinuxSignal_Sigsuspend - 
 *	Handler for linux syscall 72 
 * Support: 0% (use rtsigsuspend)
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
LinuxSignal_Sigsuspend(int32 hist0, int32 hist1, LinuxOldSigSet mask)
{
   UWLOG_SyscallUnsupported("use RTSigsuspend");
   return LINUX_ENOSYS;
}


/* 
 *----------------------------------------------------------------------
 *
 * LinuxSignal_Sigpending - 
 *	Handler for linux syscall 73 
 * Support: 0% (use rtsigpending)
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
LinuxSignal_Sigpending(UserVA maskp)
{
   UWLOG_SyscallUnsupported("use RTSigpending");
   return LINUX_ENOSYS;
}


/* 
 *----------------------------------------------------------------------
 *
 * LinuxSignal_Sigreturn - 
 *	Handler for linux syscall 119 
 * Support: 0% (don't use)
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
LinuxSignal_Sigreturn(UserVA sigframe)
{
   UWLOG(0, "UNEXPECTED! (%#x)", sigframe);
   /*
    * This should never be called by user code and certianly never
    * called by the kernel.  See LinuxSignal_RTSigreturn which is
    * (effectively) called by the kernel.
    */
   return LINUX_ENOSYS;
}


/* 
 *----------------------------------------------------------------------
 *
 * LinuxSignal_Sigprocmask - 
 *	Handler for linux syscall 126 
 * Support: 0% (use rtsigprocmask)
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
LinuxSignal_Sigprocmask(int32 how, UserVA maskp, UserVA omaskp)
{
   UWLOG_SyscallUnsupported("use RTSigprocmask");
   return LINUX_ENOSYS;
}


/* 
 *----------------------------------------------------------------------
 *
 * LinuxSignal_RTSigreturn - 
 *	Handler for linux syscall 173 
 * Support: 100% (don't use)
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
LinuxSignal_RTSigreturn(UserVA magic)
{
   VMK_ReturnStatus status;
   VMKFullUserExcFrame* fullFrame;

   UWLOG_SyscallEnter("(%#x)", magic);

   fullFrame = MY_USER_THREAD_INFO->exceptionFrame;
   ASSERT(fullFrame != NULL);
   status = UserSig_ReturnFromHandler(magic, fullFrame);
   if (status == VMK_OK) {
      /*
       * Switch logging prefix away from rt_sigreturn (it should be
       * whatever the previously interrupted syscall was, but that
       * information isn't easily available).
       */
      UWLOG(1, "Switching from rt_sigreturn back to interrupted context.");
      UWLOG_ClearContext();

      /*
       * Return what was supposed to have been returned before the
       * signal that we're cleaning up after.  fullFrame has been
       * munged so it looks like some other syscall is returning in
       * user-land.
       */
      return fullFrame->regs.eax;
   } else {
      /*
       * Bad restore of full frame.  No legitimate caller of
       * sigreturn is prepared for it to return, so we must
       * aggressively terminate this cartel.
       */
      UWLOG(0, "sigreturn failed.  Killing cartel.");
      User_CartelShutdown(CARTEL_EXIT_SYSERR_BASE+LINUX_SIGSEGV,
                          TRUE, fullFrame);

      ASSERT(MY_USER_THREAD_INFO->dead == TRUE);
      /* Current thread will exit in syscall exit layer */

      return User_TranslateStatus(status);
   }
}


/* 
 *----------------------------------------------------------------------
 *
 * LinuxSignal_RTSigaction - 
 *	Handler for linux syscall 174 
 *
 *	See FreeBSD5.0: compat/linux/linux_signal.c:linux_rt_sigaction()
 * Support: 80% (oneshot,nocldstop not supported)
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
LinuxSignal_RTSigaction(uint32 linuxSignum,
                        UserVA /* sigaction* */ userNewAct,
                        UserVA /* sigaction* */ userOldAct,
                        uint32 sigsetsize)
{
   World_Handle* curr = MY_RUNNING_WORLD;
   LinuxSigAction kernOutAct;
   UserSigId usig;

   UWLOG_SyscallEnter("(%u, %#x, %#x, %d)",
                      linuxSignum, userNewAct, userOldAct, sigsetsize);

   /*
    * A weak version check.
    */
   if (sigsetsize != sizeof(LinuxSigSet)) {
      UWWarn("expecting sigsetsize=%d -> EINVAL", sizeof(LinuxSigSet));
      UWLOG_StackTraceCurrent(1);
      return LINUX_EINVAL;
   }

   usig = LinuxSignal_ToUserSignal(linuxSignum);
   if (usig == LINUX_SIG_ERR) {
      UWWarn("illegal/unsupported signal num %d -> EINVAL", linuxSignum);
      UWLOG_StackTraceCurrent(1);
      return LINUX_EINVAL;
   }
   
   /*
    * Fetch existing signal action, if its wanted.  Don't save it into
    * linux userOldAct until after reading userNewAct (in case
    * userNewAct==userOldAct).
    */
   if (userOldAct != 0) {
      LinuxSigSet kernSigMask;
      UserSig_CartelInfo* cartelSigInfo = &curr->userCartelInfo->signals;

      memset(&kernOutAct, 0, sizeof kernOutAct);

      UserSig_CartelLock(cartelSigInfo);

      kernSigMask = LinuxSignalSetFromUserSignalSet(UserSig_GetSigMask(cartelSigInfo, usig));

      //kernOutAct.restorer = 0;
      //kernOutAct.flags = 0;
      if (UserSig_IsOneShot(cartelSigInfo, usig)) {
         kernOutAct.flags |= LINUX_SIGACTFLAG_ONESHOT;
      }
      if (UserSig_IsReentrant(cartelSigInfo, usig)) {
         kernOutAct.flags |= LINUX_SIGACTFLAG_NOMASK;
      }
      kernOutAct.mask = kernSigMask;
      kernOutAct.handler = UserSig_GetSigHandler(cartelSigInfo, usig);
      UserSig_CartelUnlock(cartelSigInfo);

      /* Copied out to user mode below */
   }

   /*
    * Install the new signal handler, if provided.
    */
   if (userNewAct != 0) {
      VMK_ReturnStatus status;
      LinuxSigAction kernInAct;
      UserSigSet uwSigMask;
      UserSig_CartelInfo* cartelSigInfo = &curr->userCartelInfo->signals;
      
      // Do not allow changes to SIGKILL or SIGSTOP
      if ((usig == LINUX_SIGKILL) || (usig == LINUX_SIGSTOP)) {
         UWWarn("cannot install handlers for kill/stop -> EINVAL");
         UWLOG_StackTraceCurrent(1);
         return LINUX_EINVAL;
      }

      status = User_CopyIn(&kernInAct, userNewAct, sizeof kernInAct);
      if (status != VMK_OK) {
         return User_TranslateStatus(status);
      }

      uwSigMask = LinuxSignalSetToUserSignalSet(kernInAct.mask);

      UserSig_CartelLock(cartelSigInfo);
      UserSig_SetSigHandler(cartelSigInfo, usig, kernInAct.handler);
      UserSig_SetReentrant(cartelSigInfo, usig,
                           (kernInAct.flags & LINUX_SIGACTFLAG_NOMASK) != 0);
      UserSig_SetSigMask(cartelSigInfo, usig, uwSigMask);
      UserSig_CartelUnlock(cartelSigInfo);

      /*
       * Mask out supported flags to do a sanity check for unsupported
       * options.
       */
      kernInAct.flags &= ~LINUX_SIGACTFLAG_NOMASK;

      /*
       * kernInAct.restorer and SA_RESTORER.
       *
       * The restorer field is only used if SA_RESTORER is provided
       * (and we don't support that flag).  Its an unsupported
       * mechanism for restoring saved context from the stack after a
       * signal handler returns, we use UserSigDispatch and ktext.
       */
      if ((kernInAct.flags & LINUX_SIGACTFLAG_RESTORER) != 0) {
         /*
          * Because glibc sets the restore flag and field, we quitely
          * ignore it (instead of UWWarn).
          */
         UWLOG(1, "Has SA_RESTORER, sigact.sa_restorer=%#x.  Ignoring.",
               kernInAct.restorer);
         kernInAct.flags &= ~LINUX_SIGACTFLAG_RESTORER;
      }

      /*
       * Warn about other flags.
       *
       * SA_ONESHOT: not implemented, but would be easy if necessary.
       * (some of the userSig infrastructure exists, see
       * UserSig_SetOneShot).
       *
       * SA_NOCLDSTOP controls issue of the SIGCHLD signal when
       * a child process stops.  We don't support child processes,
       * so we can ignore this flag.
       *
       * SA_RESTART: turns on syscall restart for syscalls that are
       * interrupted by this signal.  However, our client, the VMX
       * is already prepared to deal with spurious returns from
       * syscalls due to interruptions (e.g. by other signals), so
       * we won't support this.
       *
       * SA_SIGINFO asks for more information to be pushed on
       * the stack when this signal handler is dispatched.  Not
       * supported.
       *
       * Mask out the simple no-op flags, warn about any unexpected
       * ones.
       */
      kernInAct.flags &= ~(LINUX_SIGACTFLAG_RESTART
                           |LINUX_SIGACTFLAG_NOCLDSTOP);
      if (kernInAct.flags != 0) {
         UWWarn("ignored unexpected signal flags %#x (sig %d)",
                kernInAct.flags, linuxSignum);
         UWLOG_StackTraceCurrent(1);
      }
   }
   
   /*
    * Now copy out the the old sig action if the user wanted it.
    */
   if (userOldAct != 0) {
      VMK_ReturnStatus status;
      status = User_CopyOut(userOldAct, &kernOutAct, sizeof kernOutAct);
      if (status != VMK_OK) {
         return User_TranslateStatus(status);
      }
   }
      
   return 0;
}


/* 
 *----------------------------------------------------------------------
 *
 * LinuxSignal_RTSigprocmask - 
 *	Handler for linux syscall 175 
 * Support: 98% (can block "unblockable" signals)
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
LinuxSignal_RTSigprocmask(int32 how,
                          UserVA userNewMask,
                          UserVA userOldMask,
                          uint32 userSigsetsize)
{
   VMK_ReturnStatus status;
   World_Handle* const curr = MY_RUNNING_WORLD;
   UserSig_ThreadInfo* const threadSigInfo = &curr->userThreadInfo->signals;
   LinuxSigSet kernNewMask = 0;
   UserSigSet uwOldMask;
   int rc = 0;

   /*
    * A weak version check
    */
   if (userSigsetsize != sizeof(LinuxSigSet)) {
      UWWarn("expecting sigsetsize=%d (got %d) -> EINVAL",
             sizeof(LinuxSigSet), userSigsetsize);
      UWLOG_StackTraceCurrent(1);
      return LINUX_EINVAL;
   }

   /*
    * Copy the new mask in from the user.  (Do before copyout of old mask,
    * in case same pointer is given for both.)
    */
   if (userNewMask != 0) {
      status = User_CopyIn(&kernNewMask, userNewMask, sizeof kernNewMask);
      if (status != VMK_OK) {
         UWLOG(0, "Failed to copy new signal mask: %s",
               UWLOG_ReturnStatusToString(status));
         return User_TranslateStatus(status);
      }
   }

   UWLOG(2, "(how=%s, nmask@%#x=%#"FMT64"x, omask@%#x, sz=%u)",
         ((how == LINUX_SIGHOW_BLOCK) ? "block"
            : ((how == LINUX_SIGHOW_UNBLOCK) ? "unblock"
               : ((how == LINUX_SIGHOW_SETMASK) ? "set"
                  : "<illegal how>"))),
         userNewMask, kernNewMask, userOldMask, userSigsetsize);

   UserSig_GetBlocked(threadSigInfo, &uwOldMask);

   /*
    * Copy out the old mask, if the user wanted it.  (Do after the copyin,
    * in case the same pointer is given for both masks.)
    */
   if (userOldMask != 0) {
      LinuxSigSet kernOldMask;
      kernOldMask = LinuxSignalSetFromUserSignalSet(uwOldMask);
      UWLOG(2, "save omask@%#x=%"FMT64"x (uw=%#"FMT64"x)",
            userOldMask, kernOldMask, uwOldMask);
      status = User_CopyOut(userOldMask, &kernOldMask, sizeof(LinuxSigSet));
      if (status != VMK_OK) {
         return User_TranslateStatus(status);
      }

   }

   /*
    * Set the new signal mask.  Note that we don't need a lock protecting
    * the signal mask because it is truly thread-private.
    */
   if (userNewMask != 0) {
      UserSigSet deltaSigMask;
      /*
       * Always remove kill and stop.  They cannot be blocked (or
       * unblocked) by the user.
       */
      kernNewMask = LinuxSigSetClearSig(kernNewMask, LINUX_SIGKILL);
      kernNewMask = LinuxSigSetClearSig(kernNewMask, LINUX_SIGSTOP);

      deltaSigMask = LinuxSignalSetToUserSignalSet(kernNewMask);

      switch (how) {
      case LINUX_SIGHOW_BLOCK:
         uwOldMask |= deltaSigMask;
         break;
      case LINUX_SIGHOW_UNBLOCK:
         uwOldMask &= (~deltaSigMask);
         break;
      case LINUX_SIGHOW_SETMASK:
         uwOldMask = deltaSigMask;
         break;
      default:
         rc = LINUX_EINVAL;
      }
      
      if (rc == 0) {
         UWLOG(2, "setting signal mask to %#"FMT64"x (uw=%#"FMT64"x)",
               kernNewMask, uwOldMask);
         UserSig_SetBlocked(threadSigInfo, uwOldMask);
      }
   }

   return rc;
}


/* 
 *----------------------------------------------------------------------
 *
 * LinuxSignal_RTSigpending - 
 *	Handler for linux syscall 176 
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
LinuxSignal_RTSigpending(UserVA linuxPendingmask,
                         uint32 userSigsetsize)
{
   UWLOG_SyscallUnsupported("(outmask@%#x, sz=%u)",
			    linuxPendingmask, userSigsetsize);
   return LINUX_ENOSYS;
}


/* 
 *----------------------------------------------------------------------
 *
 * LinuxSignal_RTSigtimedwait - 
 *	Handler for linux syscall 177 
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
LinuxSignal_RTSigtimedwait(UserVA userPendingmask,
                           UserVA userSiginfo,
                           UserVA userTimeout,
                           uint32 userSigsetsize)
{
   UWLOG_SyscallUnsupported(
      "(outmask@%#x, siginfo@%#x, timeout@%x, sz=%u",
      userPendingmask, userSiginfo, userTimeout, userSigsetsize);
   return LINUX_ENOSYS;
}


/* 
 *----------------------------------------------------------------------
 *
 * LinuxSignal_RTSigqueueinfo - 
 *	Handler for linux syscall 178 
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
LinuxSignal_RTSigqueueinfo(LinuxPid pid, 
                           uint32 linuxSignum, 
                           UserVA siginfo)
{
   UWLOG_SyscallUnsupported("(pid=%u, signum=%u, siginfo@%#x)",
			    pid, linuxSignum, siginfo);
   return LINUX_ENOSYS;
}


/* 
 *----------------------------------------------------------------------
 *
 * LinuxSignal_RTSigsuspend - 
 *	Handler for linux syscall 179 
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
LinuxSignal_RTSigsuspend(UserVA userBlockedSigSet,
                         uint32 userSigsetsize)
{
   World_Handle* curr = MY_RUNNING_WORLD;
   VMK_ReturnStatus status;
   LinuxSigSet kernSet;
   UserSigSet uBlockedSigSet;

   /*
    * A weak version check
    */
   if (userSigsetsize != sizeof(LinuxSigSet)) {
      UWWarn("expecting sigsetsize=%d (got %d) -> EINVAL",
             sizeof(LinuxSigSet), userSigsetsize);
      UWLOG_StackTraceCurrent(1);
      return LINUX_EINVAL;
   }

   status = User_CopyIn(&kernSet, userBlockedSigSet, sizeof kernSet);
   if (status != VMK_OK) {
      return User_TranslateStatus(status);
   }

   uBlockedSigSet = LinuxSignalSetToUserSignalSet(kernSet);
   UWLOG(1, "blockset=%"FMT64"x (uw=%#"FMT64"x)",
         kernSet, uBlockedSigSet);
   
   ASSERT(curr->userThreadInfo->exceptionFrame != NULL);
   return UserSig_Suspend(&curr->userThreadInfo->signals,
                          uBlockedSigSet,
                          curr->userThreadInfo->exceptionFrame,
                          LINUX_EINTR);
}


/* 
 *----------------------------------------------------------------------
 *
 * LinuxSignal_Sigaltstack - 
 *	Handler for linux syscall 186 
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
LinuxSignal_Sigaltstack(UserVA altstack,
                        UserVA oAltstackp)
{
   UWLOG_SyscallUnimplemented("(altstack@%#x, oAltstackp@%#x)",
                              altstack, oAltstackp);
   return LINUX_ENOSYS;
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxSignal_Forward --
 *	
 *      Forward the given linux signal from the proxy to the cartel.
 *	Handles the cartel kill if the target world isn't far along
 *	enough to receive a signal.
 *      
 *	Prototype is in vmkernel/private/user.h
 *
 * Results:
 *	VMK_BAD_PARAM if signal is invalid, VMK_OK if signal is
 *	delivered.  Other errors if world is invalid.
 *
 * Side effects:
 *	Signal is delivered.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
LinuxSignal_Forward(World_Handle* world,
                    int linuxSigNum)
{
   VMK_ReturnStatus status;
   UserSigId sig;

   ASSERT(world != MY_RUNNING_WORLD);
   ASSERT(World_IsUSERWorld(world));

   sig = LinuxSignal_ToUserSignal(linuxSigNum); /* maps any bad sig to LINUX_SIG_ERR */
      
   if (sig == LINUX_SIG_ERR) {
      UWLOG(0, "Invalid signal number (%d), not sending to world %d",
            sig, world->worldID);
      status = VMK_BAD_PARAM;
   } else {
      SysAlert("Sending signal %d to world %d.\n", sig, world->worldID);

      status = UserSig_LookupAndSend(world->worldID, sig, FALSE);
      ASSERT(status == VMK_OK);

      /*
       * If the world hasn't started running yet, sending it a signal
       * won't do much.  So just kill it off explicitly.
       */
      if (World_CpuSchedRunState(world) == CPUSCHED_NEW) {
         VMK_ReturnStatus killStatus;

         /*
          * Can't call 'User_CartelShutdown', as that expects to be
          * invoked by the dying world itself.  So, we just kill the
          * cartel directly.  Shutdown state will be left alone.  (The
          * proxy obviously knows what is going on --- it initiated
          * this termination.)
          */
         killStatus = User_CartelKill(world, FALSE);
         ASSERT(killStatus == VMK_OK);
         
         /*
          * Clean termination point for target (it hasn't started
          * running.)
          */
	 World_Kill(world);
      }
   }

   return status;
}
