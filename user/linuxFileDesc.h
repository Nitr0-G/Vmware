/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/* 
 * linuxFileDesc.h - 
 *	Linux compatibility misc file descriptor-related syscalls.
 */

#ifndef VMKERNEL_USER_LINUXFILEDESC_H
#define VMKERNEL_USER_LINUXFILEDESC_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "linuxAPI.h"

extern int LinuxFileDesc_Pipe(UserVA pipefds);
extern int LinuxFileDesc_Read(LinuxFd fd, UserVA buf, uint32 nbyte);
extern int LinuxFileDesc_Write(LinuxFd fd, UserVAConst buf, uint32 nbyte);
extern int LinuxFileDesc_Open(UserVAConst /* const char* */ path,
                              uint32 flags, uint32 mode);
extern int LinuxFileDesc_Close(LinuxFd fd);
extern int LinuxFileDesc_Creat(UserVAConst /*const char* */ path, int32 mode);
extern int LinuxFileDesc_Dup(LinuxFd fd);
extern int LinuxFileDesc_Ioctl(LinuxFd fd, uint32 cmd, uint32 arg);
extern int LinuxFileDesc_Fcntl(LinuxFd fd, uint32 cmd, uint32 arg);
extern int LinuxFileDesc_Dup2(LinuxFd from, LinuxFd to);
extern int LinuxFileDesc_Symlink(UserVAConst /* const char* */ path,
                                 UserVAConst /* const char* */ to);
extern int LinuxFileDesc_Readlink(UserVAConst /* const char* */ userName,
                                  UserVA /* uint8* */ userBuf, int32 count);
extern int LinuxFileDesc_Fsync(LinuxFd fd);
extern int LinuxFileDesc_Select(int32 n, UserVA /* LinuxFdSet* */ readfds,
                                UserVA /* LinuxFdSet* */ writefds,
                                UserVA /* LinuxFdSet* */ exceptfds,
                                UserVA /* LinuxTimeval* */ timeout);
extern int LinuxFileDesc_Flock(LinuxFd fd, uint32 how);
extern int LinuxFileDesc_Readv(LinuxFd fd, UserVA /*struct l_iovec* */ iovp, uint32 iovcnt);
extern int LinuxFileDesc_Writev(LinuxFd fd, UserVA /*struct l_iovec* */ iovp, uint32 iovcnt);
extern int LinuxFileDesc_Fdatasync(LinuxFd fd);
extern int LinuxFileDesc_Poll(UserVA fds, uint32 nfds, int32 timeoutMillis);
extern int LinuxFileDesc_Pread(LinuxFd fd, UserVA /* uint8* */ buf,
			       uint32 nbyte, int32 olow, int32 ohigh);
extern int LinuxFileDesc_Pwrite(LinuxFd fd, UserVAConst /* const uint8* */ buf, 
			        uint32 nbyte, int32 olow, int32 ohigh);
extern int LinuxFileDesc_Stat64(UserVAConst /* const char* */ path,
                                UserVA /* LinuxStat64* */ statbuf);
extern int LinuxFileDesc_Lstat64(UserVAConst /* const char* */ path,
                                UserVA /* LinuxStat64* */ statbuf);
extern int LinuxFileDesc_Fstat64(LinuxFd fd, UserVA/* LinuxStat64* */ statbuf);
extern int LinuxFileDesc_Fcntl64(LinuxFd fd, uint32 cmd, uint32 arg);
extern int32 LinuxFileDesc_Lseek(LinuxFd fd, int32 offset, int32 whence);
extern int LinuxFileDesc_Llseek(LinuxFd fd, uint32 ohigh, uint32 olow,
                                UserVA /* uint64* */ res, uint32 whence);
extern int LinuxFileDesc_Chdir(UserVAConst /* const char * */ userPath);
extern int LinuxFileDesc_Fchdir(LinuxFd fd);
extern int LinuxFileDesc_Unlink(UserVAConst /* const char * */ userPath);
extern int LinuxFileDesc_Mknod(UserVAConst /* const char * */ userPath,
			       LinuxMode mode, uint64 unusedDevId);
extern int LinuxFileDesc_Mkdir(UserVAConst /* const char * */ userPath,
                               uint32 mode);
extern int LinuxFileDesc_Rmdir(UserVAConst /* const char * */ userPath);
extern int LinuxFileDesc_Getcwd(UserVA /* char* */ buf, uint32 bufsize);
extern int LinuxFileDesc_Chmod(UserVAConst /* const char* */ userPath,
                               uint32 mode);
extern int LinuxFileDesc_Fchmod(LinuxFd fd, uint32 mode);
extern int LinuxFileDesc_Fchown(LinuxFd fd, LinuxUID uid, LinuxGID gid);
extern int LinuxFileDesc_Lchown(UserVAConst /* const char* */ path,
                                LinuxUID uid, LinuxGID gid);
extern int LinuxFileDesc_Chown(UserVAConst /* const char* */ path,
                               LinuxUID uid, LinuxGID gid);
extern int LinuxFileDesc_Truncate(UserVAConst /* const char* */ path,
                                  int32 length);
extern int LinuxFileDesc_Ftruncate(LinuxFd fd, int32 length);
extern int LinuxFileDesc_Truncate64(UserVAConst /* const char* */ path,
                                    uint32 llow,
                                    int32 lhigh);
extern int LinuxFileDesc_Ftruncate64(LinuxFd fd,
                                     uint32 llow,
                                     int32 lhigh);
extern int LinuxFileDesc_Utime(UserVAConst /* const char* */ userPath,
                               UserVA userTimeBuf);
extern int LinuxFileDesc_Statfs(UserVAConst /* const char*  */ userPath,
                                UserVA /* LinuxStatFS* */ buf);
extern int LinuxFileDesc_Fstatfs(LinuxFd fd, UserVA /* LinuxStatFS* */ buf);
extern int LinuxFileDesc_Statfs64(UserVAConst /* const char*  */ userPath,
                                  UserVA /* LinuxStatFS64* */ buf);
extern int LinuxFileDesc_Fstatfs64(LinuxFd fd,
                                   UserVA /* LinuxStatFS64* */ buf);
extern int LinuxFileDesc_Link(UserVAConst /*const char* */ userPath,
                              UserVAConst /*const char* */ userTo);
extern int LinuxFileDesc_Rename(UserVAConst /* const char* */ from,
                                UserVAConst /* const char* */ to);
extern uint32 LinuxFileDesc_Umask(uint32 newmask);
extern int LinuxFileDesc_Getdents64(LinuxFd fd,
                                    UserVA /* LinuxDirent64* */ userBuf,
                                    uint32 nbyte);
extern int LinuxFileDesc_Access(UserVAConst /* const char* */ userPath,
                                int32 flags);

#endif /* VMKERNEL_USER_LINUXFILEDESC_H */
