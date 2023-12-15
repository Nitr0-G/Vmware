/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * linuxTime.h --
 *
 *	Linux time-related syscalls.
 */

#ifndef VMKERNEL_USER_LINUXTIME_H
#define VMKERNEL_USER_LINUXTIME_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "linuxAPI.h"
#include "userTime.h"

extern LinuxTimeT LinuxTime_Time(UserVA /* LinuxTimeT* */ tm);
extern int LinuxTime_Gettimeofday(UserVA /* LinuxTimeval* */ tvp,
                                  UserVA /* LinuxTimezone* */ tzp);
extern int LinuxTime_Settimeofday(UserVA /*const LinuxTimeval* */ tvp,
                                  UserVA /*const LinuxTimezone* */ tzp);
extern int LinuxTime_Setitimer(LinuxITimerWhich which,
                               UserVAConst /* LinuxITimerVal* */ userItv,
                               UserVA /* LinuxITimerVal* */ userOitv);
extern int LinuxTime_Getitimer(LinuxITimerWhich which,
                               UserVA /* LinuxITimerVal* */ userItv);

#endif /* VMKERNEL_USER_LINUXTIME_H */
