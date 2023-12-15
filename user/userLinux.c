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
 * userLinux.c --
 *
 *	This module implements linux syscall compatibility for
 * 	User Worlds.
 */

#include "vm_basic_types.h"
#include "vmkernel.h"
#include "memmap.h"
#include "world.h"
#include "alloc.h"
#include "util.h"
#include "timer.h"
#include "userLinux.h"
#include "userThread.h"
#include "userFile.h"
#include "userProxy_ext.h"
#include "userProxy.h"
#include "linuxRLimit.h"
#include "linuxSignal.h"
#include "linuxFileDesc.h"
#include "linuxThread.h"
#include "linuxSocket.h"
#include "linuxMem.h"
#include "linuxIdent.h"
#include "linuxTime.h"
#include "sched.h"
#include "user_layout.h"

#define LOGLEVEL_MODULE UserLinux
#include "userLog.h"

/*
 * For UserLinuxSysctl:
 */
#define LINUX_SYSCTL_KERN 1
#define LINUX_SYSCTL_KERN_VERSION 4
struct l_sysctl_args {
   UserVA /* int* */name;
   int nlen;
   UserVA /* void * */ oldval;
   UserVA /* size_t* */ oldlenp;
   UserVA /* void* */ newval;
   size_t newlen;
};

/*
 * The only critical part of this value is that it contain
 * the string "SMP": pthreads will then try spinning on mutexes
 * before just going to sleep...
 */
static const char* sysctlKernVersion = "#1 SMP Thu Jun 26 13:05:42 PDT 2003";


/* 
 *----------------------------------------------------------------------
 *
 * UserLinux_UndefinedSyscall --
 *
 *	Handler for undefined Linux system calls.
 *
 * Results:
 *	LINUX_ENOSYS
 *
 * Side effects:
 *	A log message
 *
 *----------------------------------------------------------------------
 */
int
UserLinux_UndefinedSyscall(uint32 arg1,
                           uint32 arg2,
                           uint32 arg3,
                           uint32 arg4,
                           uint32 arg5,
                           uint32 arg6)
{
   UWLOG_SyscallEnter("arg1=%#08x arg2=%#08x arg3=%#08x arg4=%#08x "
                      "arg5=%#08x arg6=%#08x", 
                      arg1, arg2, arg3, arg4, arg5, arg6);
   return LINUX_ENOSYS;
}

/* 
 *----------------------------------------------------------------------
 *
 * UserLinuxExit - 
 *	Handler for linux syscall 1
 * Support: 100%
 * Error case: 100%
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Current thread is marked for death
 *
 *----------------------------------------------------------------------
 */
static void
UserLinuxExit(int rc)
{
   UWLOG_SyscallEnter("(rc=%d)", rc);
   UserThread_SetExitStatus(rc);
   ASSERT(MY_USER_THREAD_INFO->dead == TRUE);
   // will actually exit in syscall exit layer.
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxFork - 
 *	Handler for linux syscall 2 
 * Support: 0%
 * Error case: 0%
 *
 * Results:
 *	LINUX_ENOSYS
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxFork(void)
{
   UWLOG_SyscallUnsupported("(void)");
   return LINUX_ENOSYS;
}


/*
 *----------------------------------------------------------------------
 *
 * UserLinuxExecve - 
 *	Handler for linux syscall 11 
 * Support: 0%
 * Error case: 0%
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxExecve(UserVAConst /* const char *  */ userPath,
                UserVAConst /* const char ** */ userArgp,
                UserVAConst /* const char ** */ userEnvp)
{
   UWLOG_SyscallUnsupported("use clone");
   return LINUX_ENOSYS;
}


/*
 *----------------------------------------------------------------------
 *
 * UserLinuxLchown16 - 
 *	Handler for linux syscall 16 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
int
UserLinuxLchown16(UserVAConst /* const char* */ userPath,
                  LinuxUID16 uid,
                  LinuxGID16 gid)
{
   UWLOG_SyscallUnsupported("use 32-bit version");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxStat - 
 *	Handler for linux syscall 18 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxStat(UserVAConst /* const char* */ userPath,
              UserVA ostatAddr)
{
   UWLOG_SyscallUnimplemented("..."); /* XXX unsupported? */
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxMount - 
 *	Handler for linux syscall 21 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxMount(UserVAConst /* const char* */ userSpecialfile,
               UserVAConst /* const char* */ userDir,
               UserVAConst /* const char* */ filesystemtype,
               uint32 rwflag,
               UserVA data)
{
   UWLOG_SyscallUnsupported("Try in the Service Console");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxOldumount - 
 *	Handler for linux syscall 22 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxOldumount(UserVAConst /*char* */ path)
{
   UWLOG_SyscallUnsupported("try in the service console");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxStime - 
 *	Handler for linux syscall 25 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxStime(void)
{
   UWLOG_SyscallUnsupported("use settimeofday");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxPtrace - 
 *	Handler for linux syscall 26 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxPtrace(int32 req, LinuxPid pid, UserVA addr, UserVA data)
{
   UWLOG_SyscallUnsupported("(req=%#x pid=%d addr=%#x data=%#x)",
			    req, pid, addr, data);
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxAlarm - 
 *	Handler for linux syscall 27 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxAlarm(uint32 secs)
{
   UWLOG_SyscallUnsupported("(alarm=%u sec) -- use itimer", secs);
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxOldFstat - 
 *	Handler for linux syscall 28 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxOldFstat(LinuxFd fd, void* statbuf)
{
   UWLOG_SyscallUnsupported("(fd=%d statbuf=%p) -- use fstat64",
			    fd, statbuf);
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxPause - 
 *	Handler for linux syscall 29 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxPause(void)
{
   UWLOG_SyscallUnsupported("use sigsuspendrt");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxNice - 
 *	Handler for linux syscall 34 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxNice(int32 inc)
{
   UWLOG_SyscallUnsupported("(inc=%d)", inc);
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxSync - 
 *	Handler for linux syscall 36 
 * Support: 100%
 * Error case: 100%
 *
 * Results:
 *      0 always.
 *
 * Side effects:
 *      Force buffered writes on all files to disk.
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxSync(void)
{
   User_CartelInfo* uci = MY_USER_CARTEL_INFO;

   UserFile_Sync(uci);
   UserProxy_Sync(uci);
   return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxTimes - 
 *	Handler for linux syscall 43 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxTimes(UserVA timesArgvBuf)
{
   UWLOG_SyscallUnsupported("(%#x) use getrusage", timesArgvBuf);
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxAcct - 
 *	Handler for linux syscall 51 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxAcct(UserVAConst /* const char* */ path)
{
   UWLOG_SyscallUnsupported("(%#x)", path);
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxUmount - 
 *	Handler for linux syscall 52 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxUmount(UserVAConst /* const char* */ userPath,
                int32 flags)
{
   UWLOG_SyscallUnsupported("...");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxVeryOlduname - 
 *	Handler for linux syscall 59 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxVeryOlduname(void)
{
   UWLOG_SyscallUnsupported("use uname - #122");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxChroot - 
 *	Handler for linux syscall 61 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxChroot(UserVAConst /* const char* */ path)
{
   UWLOG_SyscallUnsupported("(%#x)", path);
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxUstat - 
 *	Handler for linux syscall 62 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxUstat(uint32 dev,
               UserVA userUstatBuf)
{
   UWLOG_SyscallUnsupported("(dev=%#x buf=%#x)", dev, userUstatBuf);
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxOsethostname - 
 *	Handler for linux syscall 74 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxOsethostname(UserVAConst /* const char* */ hostname,
                      uint32 len)
{
   UWLOG_SyscallUnsupported("(%#x, %d)", hostname, len);
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxSetrlimit - 
 *	Handler for linux syscall 75 
 * Support: ?
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxSetrlimit(uint32 resource,
                   UserVA userRLimit)
{
   UWLOG_SyscallUnimplemented("(res=%#x,rlim@%#x)",
                              resource, userRLimit);
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxOld_getrlimit - 
 *	Handler for linux syscall 76 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxOld_getrlimit(uint32 resource,
                       UserVA userRLimit)
{
   UWLOG_SyscallUnsupported("(res=%d, ptr=%#x)", resource, userRLimit);
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxGetrusage - 
 *	Handler for linux syscall 77 
 * Support: 60% -- only utime is valid, though it covers both user and system time
 * Error case: 100%
 *
 * Results:
 *	Linux error code
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxGetrusage(int who,
                   UserVA userRUsage)
{
   enum LinuxRUsage {
      LINUX_RUSAGE_WHO_CHILDREN = -1,
      LINUX_RUSAGE_WHO_SELF = 0,
   };

   struct {
      LinuxTimeval userTime;
      LinuxTimeval systemTime;
      /* Plus a bunch of fields we don't keep track of. */
      uint32 ignoredFields[14];
   } kernRUsage;

   VMK_ReturnStatus status;

   UWLOG(1, "(who=%d, userRUsage=%#x)", who, userRUsage);

   memset(&kernRUsage, 0, sizeof kernRUsage);

   if (who == LINUX_RUSAGE_WHO_SELF) {
      uint64 usage = CpuSched_VcpuUsageUsec(MY_RUNNING_WORLD);
      kernRUsage.userTime.tv_sec  = usage / 1000000;
      kernRUsage.userTime.tv_usec = usage % 1000000;
      /* Note systemTime is 0: the vmkernel is *fast*.... by fiat. */

      UWLOG(1, "Ignoring all rusage stats except user and system time.");
      
      status = User_CopyOut(userRUsage, &kernRUsage, sizeof kernRUsage);
   } else if (who == LINUX_RUSAGE_WHO_CHILDREN) {
      /* No children, means child usage is always empty. */
      status = User_CopyOut(userRUsage, &kernRUsage, sizeof kernRUsage);
   } else {
      status = VMK_BAD_PARAM;
   }

   return User_TranslateStatus(status);
}


/*
 *----------------------------------------------------------------------
 *
 * UserLinuxOldSelect - 
 *	Handler for linux syscall 82 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxOldSelect(UserVA /* struct l_old_select_argv* */ ptr)
{
   UWLOG_SyscallUnsupported("use new select");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxOstat - 
 *	Handler for linux syscall 84 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxOstat(UserVAConst /* const char* */ path,
               UserVA userOstatp)
{
   UWLOG_SyscallUnsupported("use stat64");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxUselib - 
 *	Handler for linux syscall 86 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxUselib(UserVAConst /* const char* */ library)
{
   UWLOG_SyscallUnsupported("(lib=%#x) use elf ld.so", library);
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxSwapon - 
 *	Handler for linux syscall 87 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxSwapon(UserVAConst /* const char* */ name, int flags)
{
   UWLOG_SyscallUnsupported("no swap");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxReboot - 
 *	Handler for linux syscall 88 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxReboot(int32 magic1, int32 magic2, uint32 cmd,
                UserVA /* void* */ arg)
{
   UWLOG_SyscallUnsupported("only service console may reboot");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxReaddir - 
 *	Handler for linux syscall 89 
 * Support: ?
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxReaddir(LinuxFd fd,
                 UserVA /* struct l_dirent* */ dent,
                 uint32 count)
{
   UWLOG_SyscallUnimplemented("...");
   return LINUX_ENOSYS;
}


/*
 *----------------------------------------------------------------------
 *
 * UserLinuxOldFchown - 
 *	Handler for linux syscall 95 
 * Support: 0% (use #207)
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxOldFchown(LinuxFd fd, int uid, int gid)
{
   UWLOG_SyscallUnimplemented("use #207");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxGetpriority - 
 *	Handler for linux syscall 96 
 * Support: 10% (Not really implemented, -10 returned for process
 * 	prio, error for group or user prios.)
 * Error case: 100%
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxGetpriority(int which, int who)
{
   int rc;

   UWLOG_SyscallEnter("which=%d who=%d", which, who);
   if ((which == LINUX_PRIO_USER)
       || (which == LINUX_PRIO_PGRP)) {
      /* Unsupported */
      UWWarn("Unsupported getpriority: which=user/pgrp");
      UWLOG_StackTraceCurrent(1);
      rc = LINUX_ENOSYS;
   } else if (which == LINUX_PRIO_PROCESS) {
      /*
       * vmkload_app defaults to a priority of -10.  Just return that
       * as the "current" priority.  We don't really implement
       * setpriority (see below), so this is okay.
       */
      static const int priority = -10;
      
      /*
       * XXX 'who' should be limited to 0 or a pid in the current
       * cartel.
       */
      rc = LINUX_GETPRIORITY_OFFSET - priority; // see man page

      UWLOG(1, "priority=%d, rc=%d", priority, rc);
   } else {
      /* Illegal */
      UWWarn("Illegal getpriority 'which=%d'", which);
      UWLOG_StackTraceCurrent(1);
      rc = LINUX_EINVAL;
   }
   return rc;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxSetpriority - 
 *	Handler for linux syscall 97 
 * Support: 10% (Not really implemented, -1 or -10 are silently
 *	ignoredfor process prio, errors for group or user prio changes.)
 * Error case: 100%
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxSetpriority(int which, int who, int prio)
{
   int rc;
   UWLOG_SyscallEnter("which=%d, who=%d, prio=%d", which, who, prio);
   if ((which == LINUX_PRIO_USER)
       || (which == LINUX_PRIO_PGRP)) {
      /* Unsupported */
      UWWarn("Unsupported user/pgrp setpriority.");
      UWLOG_StackTraceCurrent(1);
      rc = LINUX_ENOSYS;
   } else if (which == LINUX_PRIO_PROCESS) {
      /*
       * Bounds check prio.
       */
      if ((prio < -20) || (prio > 19)) {
         rc = LINUX_EINVAL;
      } else {
         /*
          * XXX 'who' should be limited to 0, or a pid in the current
          * cartel.
          */
         UWLOG(2, "Setting priority of who=%d to prio=%d", who, prio);
         if ((prio != -10) && (prio != -1)) {
            /*
             * VMX only uses -1 and -10, others uses will require us
             * to implement this better.
             */
            UWWarn("Unexpected priority %d", prio);
            UWLOG_StackTraceCurrent(1);
         }
         rc = 0;
      }
   } else {
      /* Illegal */
      UWWarn("Illegal setpriority 'which=%d'", which);
      UWLOG_StackTraceCurrent(1);
      rc = LINUX_EINVAL;
   }
   return rc;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxIoperm - 
 *	Handler for linux syscall 101 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxIoperm(uint32 start, uint32 length, int32 enable)
{
   UWLOG_SyscallUnsupported("...");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxSyslog - 
 *	Handler for linux syscall 103 
 * Support: 0% (cat /proc/vmware/log)
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxSyslog(int32 type, UserVA /* char* */ buf, int32 len)
{
   UWLOG_SyscallUnsupported("...");
   return LINUX_ENOSYS;
}


/*
 *----------------------------------------------------------------------
 *
 * UserLinuxNewstat - 
 *	Handler for linux syscall 106 
 * Support: 0% (use #195)
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxNewstat(UserVA pathStr, UserVA userNewstatbuf)
{
   UWLOG_SyscallUnsupported("use stat64 - #195");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxNewlstat - 
 *	Handler for linux syscall 107 
 * Support: 0% (use #196)
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxNewlstat(UserVA pathStr, UserVA userNewlstatbuf)
{
   UWLOG_SyscallUnsupported("use lstat64 - #196");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxFstat - 
 *	Handler for linux syscall 108 
 * Support: 0% (use #197)
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxFstat(LinuxFd fd, void *userStatbuf)
{
   UWLOG_SyscallUnsupported("use fstat64 - #197");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxOldUname - 
 *	Handler for linux syscall 109 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxOldUname(void)
{
   /* See UserLinuxUname ... */
   UWLOG_SyscallUnsupported("use uname - #122");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxIopl - 
 *	Handler for linux syscall 110 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxIopl(uint32 level)
{
   UWLOG_SyscallUnsupported("level=%d", level);
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxVhangup - 
 *	Handler for linux syscall 111 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxVhangup(void)
{
   UWLOG_SyscallUnsupported("(void)");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxVm86old - 
 *	Handler for linux syscall 113 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxVm86old(void)
{
   UWLOG_SyscallUnsupported("(void)");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxSwapoff - 
 *	Handler for linux syscall 115 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxSwapoff(void)
{
   UWLOG_SyscallUnsupported("(void)");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxSysinfo - 
 *	Handler for linux syscall 116 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxSysinfo(UserVA /* struct l_sysinfo* */ info)
{
   UWLOG_SyscallUnsupported("(%#x)", info);
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxIpc - 
 *	Handler for linux syscall 117 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxIpc(uint32 what, int32 arg1, int32 arg2, int32 arg3, UserVA ptr, int32 arg5)
{
   UWLOG_SyscallUnimplemented("...");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxSetdomainname - 
 *	Handler for linux syscall 121 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxSetdomainname(UserVAConst /* const char* */ name,
                       int len)
{
   UWLOG_SyscallUnsupported("(name=%#x, len=%d)", name, len);
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxUname - 
 *	Handler for linux syscall 122 
 * Support: 100%
 * Error case: 100%
 *
 * Results:
 *      Calls out to COS via the proxy and returns system info.
 *
 * Side effects:
 *      When built with --enable-kernel, glibc's dynamic linker code
 *      in sysdeps/unix/sysv/linux/dl-osinfo.h computes a version
 *      number based on uname()'s release string. glibc's code assumes
 *      a version string of the form "x.y.z". 
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxUname(UserVA userUtsName)
{
   VMK_ReturnStatus status;
   LinuxUtsName kernelUtsName;

   UWLOG_SyscallEnter("(userUtsName @ %#x)",
                      userUtsName);

   status = UserProxy_Uname(MY_RUNNING_WORLD->userCartelInfo,
                            &kernelUtsName);
 
   if (status == VMK_OK) {
      status = User_CopyOut(userUtsName,
                            &kernelUtsName, sizeof(kernelUtsName));
   }

   return User_TranslateStatus(status);
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxModify_ldt - 
 *	Handler for linux syscall 123 
 * Support: 100%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxModify_ldt(int32 func, UserVA ptr, uint32 bytecount)
{
   UWLOG_SyscallUnimplemented("...");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxAdjtimex - 
 *	Handler for linux syscall 124 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxAdjtimex(UserVA userStructTimex)
{
   UWLOG_SyscallUnimplemented("...");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxCreate_module - 
 *	Handler for linux syscall 127 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxCreate_module(void)
{
   UWLOG_SyscallUnsupported("...");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxInit_module - 
 *	Handler for linux syscall 128 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxInit_module(void)
{
   UWLOG_SyscallUnsupported("...");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxDelete_module - 
 *	Handler for linux syscall 129 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxDelete_module(void)
{
   UWLOG_SyscallUnsupported("...");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxGet_kernel_syms - 
 *	Handler for linux syscall30 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxGet_kernel_syms(void)
{
   UWLOG_SyscallUnsupported("...");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxQuotactl - 
 *	Handler for linux syscall 131 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxQuotactl(void)
{
   UWLOG_SyscallUnsupported("...");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxBdflush - 
 *	Handler for linux syscall 134 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxBdflush(void)
{
   UWLOG_SyscallUnsupported("...");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxSysfs - 
 *	Handler for linux syscall 135 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxSysfs(int32 option, uint32 arg1, uint32 arg2)
{
   UWLOG_SyscallUnsupported("...");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxPersonality - 
 *	Handler for linux syscall 136 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxPersonality(uint32 per)
{
   UWLOG_SyscallUnsupported("...");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxGetdents - 
 *	Handler for linux syscall 141 
 * Support: 0% -- use getdents64
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxGetdents(LinuxFd fd, UserVA dent, uint32 count)
{
   UWLOG_SyscallUnsupported("...");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxMsync - 
 *	Handler for linux syscall 144 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxMsync(uint32 addr, uint32 len, int32 fl)
{
   UWLOG_SyscallUnimplemented("write-back of mmap regions unsupported");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxSysctl - 
 *	Handler for linux syscall 149 
 * Support: 1% -- just the kern.version sysctl
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxSysctl(UserVA userArgs)
{
#define VMK_HACK_SYSCTL_MAX_ARGS 6
   struct l_sysctl_args kargs;
   int tmpArray[VMK_HACK_SYSCTL_MAX_ARGS];
   VMK_ReturnStatus status;

   status = User_CopyIn(&kargs, userArgs, sizeof(kargs));
   if (status != VMK_OK) {
      return User_TranslateStatus(status);
   }
   
   UWLOG_SyscallEnter(
      "(%#x[n=%#x; l=%d; oldval=%#x; oldlenp=%#x; nval=%#x; nlen=%d])",
      userArgs, kargs.name, kargs.nlen, kargs.oldval,
      kargs.oldlenp, kargs.newval, kargs.newlen);
   
   if (kargs.nlen == 0) {
      return 0; /* XXX is success on 0-length queries correct return code? */
   }

   /* We don't support any lengthy queries... */
   if ((kargs.nlen > 0)
       && (kargs.nlen <= VMK_HACK_SYSCTL_MAX_ARGS)) {
      int i;
      status = User_CopyIn(tmpArray, kargs.name, sizeof(int) * kargs.nlen);
      if (status != VMK_OK) {
         return User_TranslateStatus(status);
      }
      for (i = 0; i < kargs.nlen; i++) {
         UWLOG(2, "    name[%d]=%d", i, tmpArray[i]);
      }

      /* XXX We only support one sysctl call */
      if ((tmpArray[0] == LINUX_SYSCTL_KERN)
          && (tmpArray[1] == LINUX_SYSCTL_KERN_VERSION)) {
         size_t koldlen;
         size_t copyOutLen;

         if (kargs.newval != 0) {
            return LINUX_EPERM; /* Cannot change kernel version. */
         }

         status = User_CopyIn(&koldlen, kargs.oldlenp, sizeof koldlen);
         if (status != VMK_OK) {
            return User_TranslateStatus(status);
         }

         if (koldlen == 0) {
            return LINUX_EFAULT;
         }

         if (koldlen > strlen(sysctlKernVersion)+1) {
            copyOutLen = strlen(sysctlKernVersion)+1;
         } else {
            copyOutLen = koldlen;
         }
         
         /*
          * Copy sysctlKernVersion out, and copy the new length out.
          */
         status = User_CopyOut(kargs.oldval, sysctlKernVersion, copyOutLen);
         if (status != VMK_OK) {
            return User_TranslateStatus(status);
         }

         status = User_CopyOut(kargs.oldlenp, &copyOutLen, sizeof copyOutLen);
         if (status != VMK_OK) {
            return User_TranslateStatus(status);
         }
         
         UWLOG(2, "    -> %s (len=%d)", sysctlKernVersion, copyOutLen);

         return 0;
      }
   }

   UWLOG(2, "    -> err=notfound");

   /* "name" was not found: */
   return LINUX_ENOTDIR;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxVm86 - 
 *	Handler for linux syscall 166 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxVm86(void)
{
   UWLOG_SyscallUnsupported("(void)");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxQuery_module - 
 *	Handler for linux syscall 167 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxQuery_module(void)
{
   UWLOG_SyscallUnsupported("(void)");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxNfsservctl - 
 *	Handler for linux syscall 169 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxNfsservctl(void)
{
   UWLOG_SyscallUnsupported("(void)");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxPrctl - 
 *	Handler for linux syscall 172 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxPrctl(void)
{
   UWLOG_SyscallUnsupported("...");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxChown16 - 
 *	Handler for linux syscall 182 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxChown16(UserVAConst /* const char* */ path,
                 LinuxUID16 uid,
                 LinuxGID16 gid)
{
   UWLOG_SyscallUnsupported("use 32-bit version");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxCapget - 
 *	Handler for linux syscall 184 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxCapget(void)
{
   UWLOG_SyscallUnsupported("...");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxCapset - 
 *	Handler for linux syscall 185 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxCapset(void)
{
   UWLOG_SyscallUnsupported("...");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxSendfile - 
 *	Handler for linux syscall 187 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxSendfile(int outfd, int infd, UserVA /* uint64* */ offset, uint32 count)
{
   UWLOG_SyscallUnsupported("(in=%d, out=%d, offset@%#x, count=%u",
			    outfd, infd, offset, count);
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxVfork - 
 *	Handler for linux syscall 190 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxVfork(void)
{
   UWLOG_SyscallUnsupported("(void)");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * UserLinuxGetrlimit - 
 *	Handler for linux syscall 191 
 * Support: 33% (cur stack broken, stack limit okay, other limits are
 *       "infinity")
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxGetrlimit(uint32 resource, UserVA userRlim)
{
   struct l_rlimit kernRLimit;
   int rc = 0;

   switch(resource)
   {
   case RLIMIT_STACK:
      kernRLimit.rlim_cur = VMK_USER_MAX_STACK_PAGES * PAGE_SIZE; /* XXX */
      kernRLimit.rlim_max = VMK_USER_MAX_STACK_PAGES * PAGE_SIZE;
      break;
   case RLIMIT_NOFILE:
      kernRLimit.rlim_cur = USEROBJ_MAX_HANDLES;
      kernRLimit.rlim_max = USEROBJ_MAX_HANDLES;
      break;
   case RLIMIT_CPU:
   case RLIMIT_FSIZE:
   case RLIMIT_DATA:
   case RLIMIT_CORE:
   case RLIMIT_RSS:
   case RLIMIT_NPROC:
   case RLIMIT_MEMLOCK:
   case RLIMIT_AS:
      kernRLimit.rlim_cur = RLIM_INFINITY;
      kernRLimit.rlim_max = RLIM_INFINITY;
      break;
   default:
      rc = LINUX_EINVAL;
      break;
   }

   if (rc == 0) {
      VMK_ReturnStatus status;

      UWLOG_SyscallEnter("(%#x, %#x) -> (cur=%d; max=%d)",
                         resource, userRlim,
                         kernRLimit.rlim_cur, kernRLimit.rlim_max);

      status = User_CopyOut(userRlim, &kernRLimit, sizeof(kernRLimit));
      if (status != VMK_OK) {
         rc = User_TranslateStatus(status);
      }
   } else {
      UWLOG_SyscallEnter("(%#x, %#x) -> EINVAL",
                         resource, userRlim);
   }

   return rc;
}


/*
 *----------------------------------------------------------------------
 *
 * UserLinuxPivotRoot - 
 *	Handler for linux syscall 217 
 * Support: 0%
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static int
UserLinuxPivotRoot(UserVAConst /* const char* */ newRoot,
                   UserVAConst /* const char* */ oldRoot)
{
   UWLOG_SyscallUnsupported("(newroot@%#x, oldroot@%#x", newRoot, oldRoot);
   return LINUX_ENOSYS;
}


/*
 * This list was generated via some hacky perl scripts 
 * from FreeBSD's sys/i386/linux/syscalls.master...
 */
User_SyscallHandler UserLinux_SyscallTable[] =
{
/* 0: */ (User_SyscallHandler)UserLinux_UndefinedSyscall, /* 0 UNIMPL setup */
/* 1: */ (User_SyscallHandler)UserLinuxExit,
/* 2: */ (User_SyscallHandler)UserLinuxFork,
/* 3: */ (User_SyscallHandler)LinuxFileDesc_Read,
/* 4: */ (User_SyscallHandler)LinuxFileDesc_Write,
/* 5: */ (User_SyscallHandler)LinuxFileDesc_Open,
/* 6: */ (User_SyscallHandler)LinuxFileDesc_Close,
/* 7: */ (User_SyscallHandler)LinuxThread_Waitpid,
/* 8: */ (User_SyscallHandler)LinuxFileDesc_Creat,
/* 9: */ (User_SyscallHandler)LinuxFileDesc_Link,
/* 10: */ (User_SyscallHandler)LinuxFileDesc_Unlink,
/* 11: */ (User_SyscallHandler)UserLinuxExecve,
/* 12: */ (User_SyscallHandler)LinuxFileDesc_Chdir,
/* 13: */ (User_SyscallHandler)LinuxTime_Time,
/* 14: */ (User_SyscallHandler)LinuxFileDesc_Mknod,
/* 15: */ (User_SyscallHandler)LinuxFileDesc_Chmod,
/* 16: */ (User_SyscallHandler)UserLinuxLchown16,
/* 17: */ (User_SyscallHandler)UserLinux_UndefinedSyscall, /* 17 UNIMPL break */
/* 18: */ (User_SyscallHandler)UserLinuxStat,
/* 19: */ (User_SyscallHandler)LinuxFileDesc_Lseek,
/* 20: */ (User_SyscallHandler)LinuxThread_Getpid,
/* 21: */ (User_SyscallHandler)UserLinuxMount,
/* 22: */ (User_SyscallHandler)UserLinuxOldumount,
/* 23: */ (User_SyscallHandler)LinuxIdent_Setuid16,
/* 24: */ (User_SyscallHandler)LinuxIdent_Getuid16,
/* 25: */ (User_SyscallHandler)UserLinuxStime,
/* 26: */ (User_SyscallHandler)UserLinuxPtrace,
/* 27: */ (User_SyscallHandler)UserLinuxAlarm,
/* 28: */ (User_SyscallHandler)UserLinuxOldFstat,
/* 29: */ (User_SyscallHandler)UserLinuxPause,
/* 30: */ (User_SyscallHandler)LinuxFileDesc_Utime,
/* 31: */ (User_SyscallHandler)UserLinux_UndefinedSyscall, /* 31 UNIMPL stty */
/* 32: */ (User_SyscallHandler)UserLinux_UndefinedSyscall, /* 32 UNIMPL gtty */
/* 33: */ (User_SyscallHandler)LinuxFileDesc_Access,
/* 34: */ (User_SyscallHandler)UserLinuxNice,
/* 35: */ (User_SyscallHandler)UserLinux_UndefinedSyscall, /* 35 UNIMPL ftime */
/* 36: */ (User_SyscallHandler)UserLinuxSync,
/* 37: */ (User_SyscallHandler)LinuxSignal_Kill,
/* 38: */ (User_SyscallHandler)LinuxFileDesc_Rename,
/* 39: */ (User_SyscallHandler)LinuxFileDesc_Mkdir,
/* 40: */ (User_SyscallHandler)LinuxFileDesc_Rmdir,
/* 41: */ (User_SyscallHandler)LinuxFileDesc_Dup,
/* 42: */ (User_SyscallHandler)LinuxFileDesc_Pipe,
/* 43: */ (User_SyscallHandler)UserLinuxTimes,
/* 44: */ (User_SyscallHandler)UserLinux_UndefinedSyscall, /* 44 UNIMPL prof */
/* 45: */ (User_SyscallHandler)LinuxMem_Brk,
/* 46: */ (User_SyscallHandler)LinuxIdent_Setgid16,
/* 47: */ (User_SyscallHandler)LinuxIdent_Getgid16,
/* 48: */ (User_SyscallHandler)LinuxSignal_Signal,
/* 49: */ (User_SyscallHandler)LinuxIdent_Geteuid16,
/* 50: */ (User_SyscallHandler)LinuxIdent_Getegid16,
/* 51: */ (User_SyscallHandler)UserLinuxAcct,
/* 52: */ (User_SyscallHandler)UserLinuxUmount,
/* 53: */ (User_SyscallHandler)UserLinux_UndefinedSyscall, /* 53 UNIMPL lock */
/* 54: */ (User_SyscallHandler)LinuxFileDesc_Ioctl,
/* 55: */ (User_SyscallHandler)LinuxFileDesc_Fcntl,
/* 56: */ (User_SyscallHandler)UserLinux_UndefinedSyscall, /* 56 UNIMPL mpx */
/* 57: */ (User_SyscallHandler)LinuxThread_Setpgid,
/* 58: */ (User_SyscallHandler)UserLinux_UndefinedSyscall, /* 58 UNIMPL ulimit */
/* 59: */ (User_SyscallHandler)UserLinuxVeryOlduname,
/* 60: */ (User_SyscallHandler)LinuxFileDesc_Umask,
/* 61: */ (User_SyscallHandler)UserLinuxChroot,
/* 62: */ (User_SyscallHandler)UserLinuxUstat,
/* 63: */ (User_SyscallHandler)LinuxFileDesc_Dup2,
/* 64: */ (User_SyscallHandler)LinuxThread_Getppid,
/* 65: */ (User_SyscallHandler)LinuxThread_Getpgrp,
/* 66: */ (User_SyscallHandler)LinuxThread_Setsid,
/* 67: */ (User_SyscallHandler)LinuxSignal_Sigaction,
/* 68: */ (User_SyscallHandler)LinuxSignal_Sgetmask,
/* 69: */ (User_SyscallHandler)LinuxSignal_Ssetmask,
/* 70: */ (User_SyscallHandler)LinuxIdent_Setreuid16,
/* 71: */ (User_SyscallHandler)LinuxIdent_Setregid16,
/* 72: */ (User_SyscallHandler)LinuxSignal_Sigsuspend,
/* 73: */ (User_SyscallHandler)LinuxSignal_Sigpending,
/* 74: */ (User_SyscallHandler)UserLinuxOsethostname,
/* 75: */ (User_SyscallHandler)UserLinuxSetrlimit,
/* 76: */ (User_SyscallHandler)UserLinuxOld_getrlimit,
/* 77: */ (User_SyscallHandler)UserLinuxGetrusage,
/* 78: */ (User_SyscallHandler)LinuxTime_Gettimeofday,
/* 79: */ (User_SyscallHandler)LinuxTime_Settimeofday,
/* 80: */ (User_SyscallHandler)LinuxIdent_Getgroups16,
/* 81: */ (User_SyscallHandler)LinuxIdent_Setgroups16,
/* 82: */ (User_SyscallHandler)UserLinuxOldSelect,
/* 83: */ (User_SyscallHandler)LinuxFileDesc_Symlink,
/* 84: */ (User_SyscallHandler)UserLinuxOstat,
/* 85: */ (User_SyscallHandler)LinuxFileDesc_Readlink,
/* 86: */ (User_SyscallHandler)UserLinuxUselib,
/* 87: */ (User_SyscallHandler)UserLinuxSwapon,
/* 88: */ (User_SyscallHandler)UserLinuxReboot,
/* 89: */ (User_SyscallHandler)UserLinuxReaddir,
/* 90: */ (User_SyscallHandler)LinuxMem_Mmap,
/* 91: */ (User_SyscallHandler)LinuxMem_Munmap,
/* 92: */ (User_SyscallHandler)LinuxFileDesc_Truncate,
/* 93: */ (User_SyscallHandler)LinuxFileDesc_Ftruncate,
/* 94: */ (User_SyscallHandler)LinuxFileDesc_Fchmod,
/* 95: */ (User_SyscallHandler)UserLinuxOldFchown,
/* 96: */ (User_SyscallHandler)UserLinuxGetpriority,
/* 97: */ (User_SyscallHandler)UserLinuxSetpriority,
/* 98: */ (User_SyscallHandler)UserLinux_UndefinedSyscall, /* 98 UNIMPL profil */
/* 99: */ (User_SyscallHandler)LinuxFileDesc_Statfs,
/* 100: */ (User_SyscallHandler)LinuxFileDesc_Fstatfs,
/* 101: */ (User_SyscallHandler)UserLinuxIoperm,
/* 102: */ (User_SyscallHandler)LinuxSocket_Socketcall,
/* 103: */ (User_SyscallHandler)UserLinuxSyslog,
/* 104: */ (User_SyscallHandler)LinuxTime_Setitimer,
/* 105: */ (User_SyscallHandler)LinuxTime_Getitimer,
/* 106: */ (User_SyscallHandler)UserLinuxNewstat,
/* 107: */ (User_SyscallHandler)UserLinuxNewlstat,
/* 108: */ (User_SyscallHandler)UserLinuxFstat,
/* 109: */ (User_SyscallHandler)UserLinuxOldUname,
/* 110: */ (User_SyscallHandler)UserLinuxIopl,
/* 111: */ (User_SyscallHandler)UserLinuxVhangup,
/* 112: */ (User_SyscallHandler)UserLinux_UndefinedSyscall, /* 112 UNIMPL idle */
/* 113: */ (User_SyscallHandler)UserLinuxVm86old,
/* 114: */ (User_SyscallHandler)LinuxThread_Wait4,
/* 115: */ (User_SyscallHandler)UserLinuxSwapoff,
/* 116: */ (User_SyscallHandler)UserLinuxSysinfo,
/* 117: */ (User_SyscallHandler)UserLinuxIpc,
/* 118: */ (User_SyscallHandler)LinuxFileDesc_Fsync,
/* 119: */ (User_SyscallHandler)LinuxSignal_Sigreturn,
/* 120: */ (User_SyscallHandler)LinuxThread_Clone,
/* 121: */ (User_SyscallHandler)UserLinuxSetdomainname,
/* 122: */ (User_SyscallHandler)UserLinuxUname,
/* 123: */ (User_SyscallHandler)UserLinuxModify_ldt,
/* 124: */ (User_SyscallHandler)UserLinuxAdjtimex,
/* 125: */ (User_SyscallHandler)LinuxMem_Mprotect,
/* 126: */ (User_SyscallHandler)LinuxSignal_Sigprocmask,
/* 127: */ (User_SyscallHandler)UserLinuxCreate_module,
/* 128: */ (User_SyscallHandler)UserLinuxInit_module,
/* 129: */ (User_SyscallHandler)UserLinuxDelete_module,
/* 130: */ (User_SyscallHandler)UserLinuxGet_kernel_syms,
/* 131: */ (User_SyscallHandler)UserLinuxQuotactl,
/* 132: */ (User_SyscallHandler)LinuxThread_Getpgid,
/* 133: */ (User_SyscallHandler)LinuxFileDesc_Fchdir,
/* 134: */ (User_SyscallHandler)UserLinuxBdflush,
/* 135: */ (User_SyscallHandler)UserLinuxSysfs,
/* 136: */ (User_SyscallHandler)UserLinuxPersonality,
/* 137: */ (User_SyscallHandler)UserLinux_UndefinedSyscall, /* 137 UNIMPL afs_syscall */
/* 138: */ (User_SyscallHandler)LinuxIdent_Setfsuid16,
/* 139: */ (User_SyscallHandler)LinuxIdent_Setfsgid16,
/* 140: */ (User_SyscallHandler)LinuxFileDesc_Llseek,
/* 141: */ (User_SyscallHandler)UserLinuxGetdents,
/* 142: */ (User_SyscallHandler)LinuxFileDesc_Select,
/* 143: */ (User_SyscallHandler)LinuxFileDesc_Flock,
/* 144: */ (User_SyscallHandler)UserLinuxMsync,
/* 145: */ (User_SyscallHandler)LinuxFileDesc_Readv,
/* 146: */ (User_SyscallHandler)LinuxFileDesc_Writev,
/* 147: */ (User_SyscallHandler)LinuxThread_Getsid,
/* 148: */ (User_SyscallHandler)LinuxFileDesc_Fdatasync,
/* 149: */ (User_SyscallHandler)UserLinuxSysctl,
/* 150: */ (User_SyscallHandler)LinuxMem_Mlock,
/* 151: */ (User_SyscallHandler)LinuxMem_Munlock,
/* 152: */ (User_SyscallHandler)LinuxMem_Mlockall,
/* 153: */ (User_SyscallHandler)LinuxMem_Munlockall,
/* 154: */ (User_SyscallHandler)LinuxThread_SchedSetparam,
/* 155: */ (User_SyscallHandler)LinuxThread_SchedGetparam,
/* 156: */ (User_SyscallHandler)LinuxThread_SchedSetscheduler,
/* 157: */ (User_SyscallHandler)LinuxThread_SchedGetscheduler,
/* 158: */ (User_SyscallHandler)LinuxThread_SchedYield,
/* 159: */ (User_SyscallHandler)LinuxThread_SchedGetMaxPriority,
/* 160: */ (User_SyscallHandler)LinuxThread_SchedGetMinPriority,
/* 161: */ (User_SyscallHandler)LinuxThread_SchedGetRRInterval,
/* 162: */ (User_SyscallHandler)LinuxThread_Nanosleep,
/* 163: */ (User_SyscallHandler)LinuxMem_Mremap,
/* 164: */ (User_SyscallHandler)LinuxIdent_Setresuid16,
/* 165: */ (User_SyscallHandler)LinuxIdent_Getresuid16,
/* 166: */ (User_SyscallHandler)UserLinuxVm86,
/* 167: */ (User_SyscallHandler)UserLinuxQuery_module,
/* 168: */ (User_SyscallHandler)LinuxFileDesc_Poll,
/* 169: */ (User_SyscallHandler)UserLinuxNfsservctl,
/* 170: */ (User_SyscallHandler)LinuxIdent_Setresgid16,
/* 171: */ (User_SyscallHandler)LinuxIdent_Getresgid16,
/* 172: */ (User_SyscallHandler)UserLinuxPrctl,
/* 173: */ (User_SyscallHandler)LinuxSignal_RTSigreturn,
/* 174: */ (User_SyscallHandler)LinuxSignal_RTSigaction,
/* 175: */ (User_SyscallHandler)LinuxSignal_RTSigprocmask,
/* 176: */ (User_SyscallHandler)LinuxSignal_RTSigpending,
/* 177: */ (User_SyscallHandler)LinuxSignal_RTSigtimedwait,
/* 178: */ (User_SyscallHandler)LinuxSignal_RTSigqueueinfo,
/* 179: */ (User_SyscallHandler)LinuxSignal_RTSigsuspend,
/* 180: */ (User_SyscallHandler)LinuxFileDesc_Pread,
/* 181: */ (User_SyscallHandler)LinuxFileDesc_Pwrite,
/* 182: */ (User_SyscallHandler)UserLinuxChown16,
/* 183: */ (User_SyscallHandler)LinuxFileDesc_Getcwd,
/* 184: */ (User_SyscallHandler)UserLinuxCapget,
/* 185: */ (User_SyscallHandler)UserLinuxCapset,
/* 186: */ (User_SyscallHandler)LinuxSignal_Sigaltstack,
/* 187: */ (User_SyscallHandler)UserLinuxSendfile,
/* 188: */ (User_SyscallHandler)UserLinux_UndefinedSyscall, /* 188 UNIMPL getpmsg */
/* 189: */ (User_SyscallHandler)UserLinux_UndefinedSyscall, /* 189 UNIMPL putpmsg */
/* 190: */ (User_SyscallHandler)UserLinuxVfork,
/* 191: */ (User_SyscallHandler)UserLinuxGetrlimit,
/* 192: */ (User_SyscallHandler)LinuxMem_Mmap2,
/* 193: */ (User_SyscallHandler)LinuxFileDesc_Truncate64,
/* 194: */ (User_SyscallHandler)LinuxFileDesc_Ftruncate64,
/* 195: */ (User_SyscallHandler)LinuxFileDesc_Stat64,
/* 196: */ (User_SyscallHandler)LinuxFileDesc_Lstat64,
/* 197: */ (User_SyscallHandler)LinuxFileDesc_Fstat64,
/* 198: */ (User_SyscallHandler)LinuxFileDesc_Lchown,
/* 199: */ (User_SyscallHandler)LinuxIdent_Getuid,
/* 200: */ (User_SyscallHandler)LinuxIdent_Getgid,
/* 201: */ (User_SyscallHandler)LinuxIdent_Geteuid,
/* 202: */ (User_SyscallHandler)LinuxIdent_Getegid,
/* 203: */ (User_SyscallHandler)LinuxIdent_Setreuid,
/* 204: */ (User_SyscallHandler)LinuxIdent_Setregid,
/* 205: */ (User_SyscallHandler)LinuxIdent_Getgroups,
/* 206: */ (User_SyscallHandler)LinuxIdent_Setgroups,
/* 207: */ (User_SyscallHandler)LinuxFileDesc_Fchown,
/* 208: */ (User_SyscallHandler)LinuxIdent_Setresuid,
/* 209: */ (User_SyscallHandler)LinuxIdent_Getresuid,
/* 210: */ (User_SyscallHandler)LinuxIdent_Setresgid,
/* 211: */ (User_SyscallHandler)LinuxIdent_Getresgid,
/* 212: */ (User_SyscallHandler)LinuxFileDesc_Chown,
/* 213: */ (User_SyscallHandler)LinuxIdent_Setuid,
/* 214: */ (User_SyscallHandler)LinuxIdent_Setgid,
/* 215: */ (User_SyscallHandler)LinuxIdent_Setfsuid,
/* 216: */ (User_SyscallHandler)LinuxIdent_Setfsgid,
/* 217: */ (User_SyscallHandler)UserLinuxPivotRoot,
/* 218: */ (User_SyscallHandler)LinuxMem_Mincore,
/* 219: */ (User_SyscallHandler)LinuxMem_Madvise,
/* 220: */ (User_SyscallHandler)LinuxFileDesc_Getdents64,
/* 221: */ (User_SyscallHandler)LinuxFileDesc_Fcntl64,
/* 222: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 223: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 224: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 225: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 226: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 227: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 228: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 229: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 230: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 231: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 232: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 233: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 234: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 235: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 236: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 237: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 238: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 239: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 240: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 241: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 242: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 243: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 244: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 245: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 246: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 247: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 248: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 249: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 250: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 251: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 252: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 253: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 254: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 255: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 256: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 257: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 258: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 259: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 260: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 261: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 262: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 263: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 264: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 265: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 266: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 267: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 268: */ (User_SyscallHandler)LinuxFileDesc_Statfs64,
/* 269: */ (User_SyscallHandler)LinuxFileDesc_Fstatfs64,
/* 270: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 271: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 272: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 273: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 274: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 275: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 276: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 277: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 278: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
/* 279: */ (User_SyscallHandler)UserLinux_UndefinedSyscall,
};

int UserLinux_SyscallTableLen = sizeof(UserLinux_SyscallTable) / sizeof(UserLinux_SyscallTable[0]);

