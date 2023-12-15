/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * linuxSignal.h --
 *
 *	Support for linux signal-related syscalls.
 */

#ifndef VMKERNEL_USER_LINUXSIGNAL_H
#define VMKERNEL_USER_LINUXSIGNAL_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "linuxAPI.h"
#include "userSig.h"

typedef uint32 LinuxOldSigSet; /* new sigset is 64-bit */

extern UserSigId LinuxSignal_ToUserSignal(uint32 linuxSig);

extern int LinuxSignal_Kill(LinuxPid pid, 
                            uint32 signum);
extern int LinuxSignal_Signal(uint32 sig, 
                              UserVA handler);
extern int LinuxSignal_Sigaction(uint32 sig, 
                                 UserVA newsa,
                                 UserVA oldsa);
extern int LinuxSignal_Sgetmask(void);
extern int LinuxSignal_Ssetmask(LinuxOldSigSet mask);
extern int LinuxSignal_Sigsuspend(int32 hist0,
                                  int32 hist1,
                                  LinuxOldSigSet mask);
extern int LinuxSignal_Sigpending(UserVA maskp);
extern int LinuxSignal_Sigreturn(UserVA linuxSigframe);
extern int LinuxSignal_Sigprocmask(int32 how, 
                                   UserVA maskp, 
                                   UserVA omaskp);
extern int LinuxSignal_RTSigreturn(UserVA magic);
extern int LinuxSignal_RTSigaction(uint32 sig,
                                   UserVA act,
                                   UserVA oact,
                                   uint32 sigsetsize);
extern int LinuxSignal_RTSigprocmask(int32 how, 
                                     UserVA mask,
                                     UserVA omask,
                                     uint32 sigsetsize);
extern int LinuxSignal_RTSigpending(UserVA linuxPendingmask,
                                    uint32 linuxSigsetsize);
extern int LinuxSignal_RTSigtimedwait(UserVA linuxPendingmask,
                                      UserVA linuxSiginfo,
                                      UserVA userTimeout,
                                      uint32 linuxSigsetsize);
extern int LinuxSignal_RTSigqueueinfo(LinuxPid pid,
                                      uint32 signum,
                                      UserVA siginfo);
extern int LinuxSignal_RTSigsuspend(UserVA userBlockedSigSet,
                                    uint32 sigsetsize);
extern int LinuxSignal_Sigaltstack(UserVA altstack,
                                   UserVA oldAltstack);

#endif /* VMKERNEL_USER_LINUXSIGNAL_H */
