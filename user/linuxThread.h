/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/* 
 * linuxThread.h - 
 *	Linux threading compatibility syscalls.
 */

#ifndef VMKERNEL_USER_LINUXTHREAD_H
#define VMKERNEL_USER_LINUXTHREAD_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "linuxAPI.h"
#include "userMem.h"

extern LinuxPid LinuxThread_PidForWorldID(World_ID wid);
extern World_ID LinuxThread_WorldIDForPid(LinuxPid pid);
extern int LinuxThread_Getpid(void);
extern int LinuxThread_Clone(int32 user_flags, UserVA stack);
extern int LinuxThread_Waitpid(LinuxPid pid, UserVA status, int32 options);
extern int LinuxThread_Wait4(LinuxPid pid, UserVA status, int32 options, UserVA l_rusage);
extern int LinuxThread_SchedSetparam(LinuxPid pid, UserVA l_sched_param);
extern int LinuxThread_SchedGetparam(LinuxPid pid, UserVA l_sched_param);
extern int LinuxThread_SchedSetscheduler(LinuxPid pid, int32 policy, UserVA l_sched_param);
extern int LinuxThread_SchedGetscheduler(LinuxPid pid);
extern int LinuxThread_SchedYield(void);
extern int LinuxThread_SchedGetMaxPriority(int32 policy);
extern int LinuxThread_SchedGetMinPriority(int32 policy);
extern int LinuxThread_SchedGetRRInterval(LinuxPid pid, UserVA l_timespec_interval);
extern int LinuxThread_Nanosleep(UserVA l_timespec_rqtp, UserVA l_timespec_rmtp);
extern int LinuxThread_Getppid(void);
extern int LinuxThread_Setpgid(LinuxPid pid, int pgid);
extern int LinuxThread_Getpgid(LinuxPid pid);
extern int LinuxThread_Getpgrp(void);
extern int LinuxThread_Getsid(LinuxPid pid);
extern int LinuxThread_Setsid(void);

#endif /* VMKERNEL_USER_LINUXTHREAD_H */
