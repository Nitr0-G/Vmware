/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * linuxMem.h --
 *
 *	Linux memory (VA management) syscalls.
 */

#ifndef VMKERNEL_USER_LINUXMEM_H
#define VMKERNEL_USER_LINUXMEM_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "vm_basic_types.h"

extern int LinuxMem_Brk(UserVA dsend);
extern int LinuxMem_Mmap(UserVA linuxMMapArgv);
extern int LinuxMem_Munmap(UserVA addr, uint32 len);
extern int LinuxMem_Mremap(UserVA addr, uint32 old_len, uint32 new_len, uint32 flags);
extern int LinuxMem_Mmap2(UserVA addr, uint32 len, uint32 prot, uint32 flags, LinuxFd fd, uint32 pgoff);
extern int LinuxMem_Mlock(UserVA addr, uint32 len);
extern int LinuxMem_Munlock(UserVA addr, uint32 len);
extern int LinuxMem_Mlockall(int how);
extern int LinuxMem_Munlockall(void);
extern int LinuxMem_Mprotect(UserVA addr, uint32 len, int prot);
extern int LinuxMem_Mincore(UserVA start, uint32 len, UserVA /* uint8* */ vec);
extern int LinuxMem_Madvise(UserVA start, uint32 length, int advice);


#endif /* VMKERNEL_USER_LINUXMEM_H */
