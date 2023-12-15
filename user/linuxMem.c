/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * linuxMem.c --
 *	Linux kernel memory management syscalls
 */

#include "user_int.h"
#include "userMem.h"
#include "linuxAPI.h"
#include "linuxMem.h"
#include "user_layout.h"

#define LOGLEVEL_MODULE LinuxMem
#include "userLog.h"

/*
 *-----------------------------------------------------------------------------
 *
 * LinuxMem_Brk - 
 *	Handler for linux syscall 45 
 * Support: 100%
 * Error case: 100%
 * Results:
 *	Value of break after adjustment (if any).  Illegal adjustments
 *	simply leave the brk unchanged.
 *
 * Side effects:
 *	Changes range of pages that are valid in current cartel's heap.
 *
 *----------------------------------------------------------------------
 */
int
LinuxMem_Brk(UserVA dataEnd)
{
   World_Handle* const curr = MY_RUNNING_WORLD;
   VMK_ReturnStatus status;

   ASSERT(curr);
   ASSERT(curr->userCartelInfo);

   UWLOG_SyscallEnter("(%#x)", dataEnd);

   status = UserMem_SetDataEnd(curr, dataEnd);
   if (status != VMK_OK)
   {
      /*
       * Behavior on Linux seems to be: if invalid request, then return
       * current value...
       */
      UWLOG(2, "(%#x) (ignored %s)",
            dataEnd, VMK_ReturnStatusToString(status));
   }
   UWLOG(1, "(%#x) -> %#x", dataEnd, UserMem_GetDataEnd(curr));
   return UserMem_GetDataEnd(curr);
}

/*
 *-----------------------------------------------------------------------------
 *
 * LinuxMem_Mmap - 
 *	Handler for linux syscall 90 
 * Support: 0% (use mmap2)
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
LinuxMem_Mmap(UserVA linuxMMapArgv)
{
   UWLOG_SyscallUnsupported("UNSUPPORTED (use mmap2() #192)");
   return LINUX_ENOSYS;
}

/*
 *-----------------------------------------------------------------------------
 *
 * LinuxMem_Munmap - 
 *	Handler for linux syscall 91 
 * Support: 100%
 * Error case: 100%
 * Results:
 *	Unmaps the specified region.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
int
LinuxMem_Munmap(UserVA addr, uint32 len)
{
   VMK_ReturnStatus status;

   UWLOG_SyscallEnter("(addr=%#x, len=%u)", addr, len);

   if (addr < VMK_USER_FIRST_TEXT_VADDR ||
       (addr + len) > VMK_USER_LAST_VADDR) {
      return LINUX_EINVAL;
   }

   status = UserMem_Unmap(MY_RUNNING_WORLD, addr, len);
   return User_TranslateStatus(status);
}

/*
 *-----------------------------------------------------------------------------
 *
 * LinuxMem_Mremap - 
 *	Handler for linux syscall 163 
 * Support: 90% (no checks for MMAP_LOCKED)
 * Error case: 100% 
 * Results:
 *      Address of remapped region or error.
 * 
 * Side effects:
 * remaps the specified region to another region of smaller / larger
 * size.
 *
 *----------------------------------------------------------------------
 */
int
LinuxMem_Mremap(UserVA addr, uint32 oldLen, uint32 newLen, uint32 flags)
{
   VMK_ReturnStatus status;
   UserVA newAddr = 0;

   UWLOG_SyscallEnter(
         "(addr=%#x, old_len=%u, new_len=%u, flags=%#x)",
         addr, oldLen, newLen, flags);
   
   //check if address passed is NULL
   if (addr == 0) {
      UWLOG(0, "Invalid Address  (%#x) -> EINVAL",
            addr);
      return LINUX_EINVAL; 
   }
	
   //check if addr is page aligned
   if (PAGE_OFFSET(addr) != 0) {
      UWLOG(0, "Address not page aligned  (%#x) -> EINVAL",
            addr);
      return LINUX_EINVAL; 
   }
   
   // check if flag passed is valid
   if ((flags & ~LINUX_MREMAP_MAYMOVE) != 0) {
      UWLOG(0, "UNSUPPORTED flag(s) (%#x) -> EINVAL", 
            flags);
      return LINUX_EINVAL;
   }

   status = UserMem_Remap(MY_RUNNING_WORLD, addr, oldLen, newLen, flags, &newAddr);
   if (status != VMK_OK) {
      return User_TranslateStatus(status);
   } else {
      return newAddr;
   }
   
}

/*
 *-----------------------------------------------------------------------------
 *
 * LinuxMem_Mmap2 - 
 *	Handler for linux syscall 192 
 * Support: 70% (no 'shared', ignore protection bits, other limits?)
 * Error case: 90%
 *      Mmapping a proxied fifo, tty, or directory fails to return an error.
 *      (Actually, we aren't sure what mmapping a directory should do.)
 *      See PR 35663.
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
int
LinuxMem_Mmap2(UserVA addr, uint32 len, uint32 prot, uint32 flags,
               LinuxFd fd, uint32 pgoff)
{
   VMK_ReturnStatus status;
   int rc;

   UWLOG_SyscallEnter(
         "(addr=%#x, len=%u, prot=%#x, flags=%#x, fd=%d, pgoff=%u)",
         addr, len, prot, flags, fd, pgoff);

   // Check for unsupported 'shared' flag
   if (flags & LINUX_MMAP_SHARED) {
      UWLOG(0, "UNSUPPORTED flags (%#x) -> EINVAL",
            flags);
      UWLOG_StackTraceCurrent(1);
      return LINUX_EINVAL;
   }

   // Since we don't support MMAP_SHARED, caller *must* provide MMAP_PRIVATE
   if ((flags & LINUX_MMAP_PRIVATE) == 0) {
      UWLOG(0, "Required MMAP_PRIVATE flag (%#x) missing -> EINVAL",
            LINUX_MMAP_PRIVATE);
      UWLOG_StackTraceCurrent(1);
      return LINUX_EINVAL;
   }

   // length and alignment checks are done in UserMem_Map

   status = UserMem_Map(MY_RUNNING_WORLD, &addr, len, prot, flags, fd, pgoff);
   if (status == VMK_OK) {
      /*
       * if addr > 2GB, we'll return a negative number (negative
       * returns normally indicate an error condition).  But, glibc
       * is smart enough to only count -4096 to -1 as error codes,
       * so we can return anything up to the last 3.99GB.
       */
      rc = addr;
   } else {
      rc = User_TranslateStatus(status);
   }
   return rc;
}

/*
 *-----------------------------------------------------------------------------
 *
 * LinuxMem_Mlock - 
 *	Handler for linux syscall 150 
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
LinuxMem_Mlock(UserVA addr, uint32 len)
{
   UWLOG_SyscallUnimplemented("(addr=%#x len=%d)", addr, len);
   return LINUX_ENOSYS;
}

/*
 *-----------------------------------------------------------------------------
 *
 * LinuxMem_Munlock - 
 *	Handler for linux syscall 151 
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
LinuxMem_Munlock(UserVA addr, uint32 len)
{
   UWLOG_SyscallUnimplemented("(addr=%#x len=%d)", addr, len);
   return LINUX_ENOSYS;
}

/*
 *-----------------------------------------------------------------------------
 *
 * LinuxMem_Mlockall - 
 *	Handler for linux syscall 152 
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
LinuxMem_Mlockall(int how)
{
   UWLOG_SyscallUnimplemented("(how=%d)", how);
   return LINUX_ENOSYS;
}

/*
 *-----------------------------------------------------------------------------
 *
 * LinuxMem_Munlockall - 
 *	Handler for linux syscall 153 
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
LinuxMem_Munlockall(void)
{
   UWLOG_SyscallUnimplemented("(void)");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * LinuxMem_Mprotect - 
 *	Handler for linux syscall 125 
 * Support: 95% (doesn't handle setting no permissions to mapped ptes)
 * Error case: 100%
 * Results:
 *	Changes the protection bits for the given region.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
int
LinuxMem_Mprotect(UserVA addr, uint32 len, int prot)
{
   VMK_ReturnStatus status;
   UWLOG_SyscallEnter("(addr=%#x, len=%u, prot=%#x)", addr, len, prot);
   status = UserMem_Protect(MY_RUNNING_WORLD, addr, len, prot);
   return User_TranslateStatus(status);
}

/*
 *----------------------------------------------------------------------
 *
 * LinuxMem_Mincore - 
 *	Handler for linux syscall 218 
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
LinuxMem_Mincore(UserVA start, uint32 len, UserVA /* uint8* */ vec)
{
   UWLOG_SyscallUnsupported("(start=%#x, len=%u, vec@%#x)", start, len, vec);
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * LinuxMem_Madvise - 
 *	Handler for linux syscall 219 
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
LinuxMem_Madvise(UserVA start, uint32 length, int advice)
{
   UWLOG_SyscallUnimplemented("(start=%#x, length=%u, advice=%#x",
			      start, length, advice);
   return LINUX_ENOSYS;
}
