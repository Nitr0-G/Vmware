/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * linuxIdent.h --
 *
 *	Support for linux signal-related syscalls.
 */

#ifndef VMKERNEL_USER_LINUXIDENT_H
#define VMKERNEL_USER_LINUXIDENT_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "linuxAPI.h"
#include "userSig.h"

extern LinuxUID LinuxIdent_Getuid(void);
extern LinuxGID LinuxIdent_Getgid(void);
extern LinuxUID LinuxIdent_Geteuid(void);
extern LinuxGID LinuxIdent_Getegid(void);
extern int LinuxIdent_Setuid(LinuxUID uid);
extern int LinuxIdent_Setgid(LinuxGID gid);
extern int LinuxIdent_Getgroups(int32 size,
                                UserVA /* LinuxGID* */ userGIDs);
extern int LinuxIdent_Setgroups(uint32 size,
                                UserVA /* LinuxGID* */ userGIDs);
extern int LinuxIdent_Setresuid(LinuxUID ruid, LinuxUID euid, LinuxUID suid);
extern int LinuxIdent_Getresuid(UserVA /* LinuxUID* */ ruid,
                                UserVA /* LinuxUID* */ euid,
                                UserVA /* LinuxUID* */ suid);
extern int LinuxIdent_Setresgid(LinuxGID rgid, LinuxGID egid,
                                LinuxGID sgid);
extern int LinuxIdent_Getresgid(UserVA /* LinuxUID* */ ruid,
                                UserVA /* LinuxUID* */ euid,
                                UserVA /* LinuxUID* */ suid);
extern int LinuxIdent_Setuid16(LinuxUID16 uid);
extern int LinuxIdent_Getuid16(void);
extern int LinuxIdent_Setgid16(LinuxGID16 gid);
extern int LinuxIdent_Getgid16(void);
extern int LinuxIdent_Geteuid16(void);
extern int LinuxIdent_Getegid16(void);
extern int LinuxIdent_Setreuid16(LinuxUID16 ruid, LinuxUID16 euid);
extern int LinuxIdent_Setregid16(LinuxGID16 rgid, LinuxGID16 egid);
extern int LinuxIdent_Getgroups16(uint32 gidsetsize,
                                  UserVA /* LinuxGID16* */ gidset);
extern int LinuxIdent_Setgroups16(uint32 gidsetsize,
                                  UserVA /* LinuxGID16* */ gidset);
extern int LinuxIdent_Setresuid16(LinuxUID16 ruid, LinuxUID16 euid,
                                  LinuxUID16 suid);
extern int LinuxIdent_Getresuid16(UserVA /* LinuxUID16* */ ruid,
                                  UserVA /* LinuxUID16* */ euid,
                                  UserVA /* LinuxUID16* */ suid);
extern int LinuxIdent_Setresgid16(LinuxGID16 rgid, LinuxGID16 egid,
                                  LinuxGID16 sgid);
extern int LinuxIdent_Getresgid16(UserVA /* LinuxGID16* */ rgid,
                                  UserVA /* LinuxGID16* */ egid,
                                  UserVA /* LinuxGID16* */ sgid);
extern int LinuxIdent_Setreuid(LinuxUID ruid, LinuxUID euid);
extern int LinuxIdent_Setregid(LinuxGID rgid, LinuxGID egid);
extern int LinuxIdent_Setfsgid16(LinuxGID16 gid);
extern int LinuxIdent_Setfsuid16(LinuxUID16 uid);
extern int LinuxIdent_Setfsuid(LinuxUID uid);
extern int LinuxIdent_Setfsgid(LinuxGID gid);

#endif /* VMKERNEL_USER_LINUXIDENT_H */
