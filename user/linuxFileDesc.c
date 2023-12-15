/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * linuxFileDesc.c -
 *	Linux file descriptor related syscall entrypoints and glue.
 */

#include "user_int.h"
#include "userPipe.h"
#include "memalloc.h"
#include "linuxFileDesc.h"
#include "linuxIoctl.h"
#include "linuxCDROM.h"
#include "linuxFloppy.h"
#include "linuxSerial.h"
#include "linuxParallel.h"
#include "timer.h"
#include "userStat.h"

#define LOGLEVEL_MODULE LinuxFileDesc
#include "userLog.h"

typedef enum {
   LINUX_FILE_IOCTL_IN  = 0,
   LINUX_FILE_IOCTL_OUT = 1
} LinuxFileDescIoctlDir;

/*
 *-----------------------------------------------------------------------------
 *
 * LinuxFileDescAllocAndCopyPath --
 *
 *      Allocate a heap buffer for the given path and copy it in.  Buffer
 *      is free'd if any errors occur on copyin.
 *
 *	See LinuxFileDescFreePath.
 *
 * Results:
 *      Returns 0 and vmkPath is allocated and initialized with
 *      contents of userPath.  OR, returns a linux error code.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
static int
LinuxFileDescAllocAndCopyPath(User_CartelInfo* const uci, 
                              UserVAConst userPath,
                              char** vmkPath)
{
   static const int pathLen = LINUX_PATH_MAX + 1;
   VMK_ReturnStatus status;

   *vmkPath = User_HeapAlloc(uci, pathLen); // XXX with Heap overhead its just over 1 page ...
   if (*vmkPath == NULL) {
      UWLOG(0, "Failed to allocate path buffer");
      return LINUX_ENOMEM;
   }

   status = User_CopyInString(*vmkPath, userPath, pathLen);
   if (status == VMK_LIMIT_EXCEEDED) {
      UWLOG(1, "User path at %#x too long (max is %d)", userPath, pathLen);
      User_HeapFree(uci, *vmkPath);
      return LINUX_ENAMETOOLONG; // User_TranslateStatus would generate EFBIG
   } else if (status != VMK_OK) {
      UWLOG(1, "User path at %#x invalid: %s",
            userPath, VMK_ReturnStatusToString(status));
      User_HeapFree(uci, *vmkPath);
      return User_TranslateStatus(status);
   }

   return 0;
}


/*
 *-----------------------------------------------------------------------------
 *
 * LinuxFileDescFreePath --
 *
 *	Free path allocated in LinuxFileDescAllocAndCopyPath
 *
 *	See LinuxFileDescAllocAndCopyPath
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE void
LinuxFileDescFreePath(User_CartelInfo* const uci, char* vmkPath)
{
   User_HeapFree(uci, vmkPath);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Read - 
 *	Handler for linux syscall 3 
 * Support: 100% (for supported fd types)
 * Error case: 100%
 * Results:
 *	number of bytes read
 *
 * Side effects:
 *	Up to nbyte bytes are read from fd and copied to given userBuf
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Read(LinuxFd fd, UserVA userBuf, uint32 nbyte)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   VMK_ReturnStatus status;
   uint32 bytesRead = 0;
   UserObj* obj;

   UWLOG_SyscallEnter("(fd=%u, buf=0x%x, nbyte=%d)", fd, userBuf, nbyte);
   ASSERT(uci != NULL);

   if (nbyte > LINUX_SSIZE_MAX) {
      UWLOG(0, "nbyte (%u) > LINUX_SSIZE_MAX (%d)!", nbyte, LINUX_SSIZE_MAX);
      return LINUX_EINVAL;
   }

   status = UserObj_Find(uci, fd, &obj);
   if (status != VMK_OK) {
      UWLOG(2, "No object for fd %d: %s",
            fd, UWLOG_ReturnStatusToString(status));
      return User_TranslateStatus(status);
   }
   
   if (!UserObj_IsOpenForRead(obj)) {
      (void) UserObj_Release(uci, obj);
      status = VMK_INVALID_HANDLE;
      UWLOG(1, "Fd %d not open for read", fd);
      return User_TranslateStatus(status);
   }

   if (nbyte == 0) {
      (void) UserObj_Release(uci, obj);
      return 0;
   }

   Semaphore_Lock(&obj->sema);
   status = obj->methods->read(obj, userBuf, obj->offset, nbyte, &bytesRead);
   obj->offset += bytesRead;
   Semaphore_Unlock(&obj->sema);
   
   (void) UserObj_Release(uci, obj);

   if (status == VMK_OK) {
      return bytesRead;
   }

   return User_TranslateStatus(status);
}

/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Write - 
 *	Handler for linux syscall 4 
 * Support: 100% (for supported fd types)
 * Error case: 100%
 * Results:
 *	Number of bytes written
 *
 * Side effects:
 *	Up to nbyte bytes are written from userBuf to file
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Write(LinuxFd fd, UserVAConst userBuf, uint32 nbyte)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   VMK_ReturnStatus status;
   uint32 bytesWritten = 0;
   UserObj* obj;

   UWLOG_SyscallEnter("(fd=%u, buf=0x%x, nbyte=%d)", fd, userBuf, nbyte);

   if (nbyte > LINUX_SSIZE_MAX) {
      UWLOG(0, "nbyte (%u) > LINUX_SSIZE_MAX (%d)!", nbyte, LINUX_SSIZE_MAX);
      return LINUX_EINVAL;
   }

   status = UserObj_Find(uci, fd, &obj);
   if (status != VMK_OK) {
      UWLOG(2, "No object for fd %d: %s",
            fd, UWLOG_ReturnStatusToString(status));
      return User_TranslateStatus(status);
   }
   
   if (!UserObj_IsOpenForWrite(obj)) {
      (void) UserObj_Release(uci, obj);
      status = VMK_INVALID_HANDLE;
      UWLOG(1, "Fd %d not open for write", fd);
      return User_TranslateStatus(status);
   }

   if (nbyte == 0) {
      (void) UserObj_Release(uci, obj);
      return 0;
   }

   /*
    * Lame inclusion of the message being written into the UWLOG.  This
    * is very handy for debugging simple programs, but will eventually
    * get annoying...
    */
   if ((LOGLEVEL() > 2) && (nbyte > 0) && (userBuf != 0)) {
      uint32 i;
      char buf[64];
      int copyLen = MIN(nbyte, sizeof(buf));

      status = User_CopyIn(buf, userBuf, copyLen);
      buf[copyLen - 1] = '\0';
         
      if ((status == VMK_OK)
          && (LOGLEVEL() > 3)
          && (nbyte == 148)
          && (obj->type == USEROBJ_TYPE_PIPEWRITE)) {
         LOG_ONLY(int* bufAsInt = (int*)buf);

         /*
          * Probably a pthread_request... format it as such in the log
          */
         UWLOG(3, "buf=0x%x=pthread_request?{requester=%#x; kind=%d; f1=%#x f2=%#x}",
               userBuf, bufAsInt[0], bufAsInt[1], bufAsInt[2], bufAsInt[3]);
      } else if (status == VMK_OK) {
         /*
          * Put in log assuming its a string.  Avoid printing
          * unprintables.
          */
         for (i = 0; i < sizeof buf; i++) {
            if (i == nbyte) {
               buf[i] = '\0';
            }
            if (buf[i] == '\0') {
               break;
            }
            if ((buf[i] < 0x20)
                || (buf[i] >= 0x7F)) {
               buf[i] = '~';
            }
         }
         UWLOG(2, "buf=0x%x{'%s'}", userBuf, buf);
      }
   }

   Semaphore_Lock(&obj->sema);
   status = obj->methods->write(obj, userBuf, obj->offset, nbyte,
                                &bytesWritten);
   obj->offset += bytesWritten;
   Semaphore_Unlock(&obj->sema);
   
   (void) UserObj_Release(uci, obj);

   if (status == VMK_OK) {
      return bytesWritten;
   }

   return User_TranslateStatus(status);
}

/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Open - 
 *	Handler for linux syscall 5 
 * Support: 90% (except for stuff in /dev, /proc, /etc).  Only some flags supported.
 * Error case: 100%
 * Results:
 *	File descriptor
 *
 * Side effects:
 *	New file is opened
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Open(UserVAConst /* const char* */ userPath,
                   uint32 flags, LinuxMode mode)
{
   User_CartelInfo* const uci = MY_USER_CARTEL_INFO;
   char* vmkPath;
   VMK_ReturnStatus status;
   int res, fd;
   int rc;
   UserObj* obj;

   res = LinuxFileDescAllocAndCopyPath(uci, userPath, &vmkPath);
   if (res != 0) {
      return res;
   }

   // We allow large files to be opened regardless of whether LARGEFILE is set.
   flags &= ~USEROBJ_OPEN_LARGEFILE;

   // We allow USEROBJ_OPEN_STAT through the open() system call
   // interface so that fstest's test for it can succeed in a
   // userworld as well as on the COS.  It's really only the proxy
   // running on the COS that needs this nonstandard file mode to work
   // through open().
   if ((flags & ~USEROBJ_OPEN_SUPPORTED) ||
       ((flags & USEROBJ_OPEN_FOR) > USEROBJ_OPEN_RDWR &&
        (flags & USEROBJ_OPEN_FOR) != USEROBJ_OPEN_STAT)) {
      UWWarn("(path=%s, flags=%#x, mode=%#x): UNSUPPORTED flags %#x",
             vmkPath, flags, mode, flags & ~USEROBJ_OPEN_SUPPORTED);
      UWLOG_StackTraceCurrent(1);
      rc = LINUX_EINVAL;
   } else {
      UWLOG_SyscallEnter("(path=%s, flags=%#x, mode=%#x)", vmkPath, flags, mode);

      UserObj_FDLock(&uci->fdState);
      mode &= ~uci->fdState.umask;
      UserObj_FDUnlock(&uci->fdState);
      
      fd = UserObj_FDReserve(uci);
      if (fd == USEROBJ_INVALID_HANDLE) {
         rc = User_TranslateStatus(VMK_NO_FREE_HANDLES);
      } else {
         status = UserObj_Open(uci, vmkPath, flags, mode, &obj);
         if (status == VMK_OK) {
            UserObj_FDAddObj(uci, fd, obj);
            rc = fd;
         } else {
            UserObj_FDUnreserve(uci, fd);
            rc = User_TranslateStatus(status);
         }
      }
   }

   LinuxFileDescFreePath(uci, vmkPath);
   return rc;
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Close - 
 *	Handler for linux syscall 6 
 * Support: 100%
 * Error case: 100%
 * Results:
 *	0
 *
 * Side effects:
 *	Given fd is closed
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Close(LinuxFd fd)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;

   UWLOG_SyscallEnter("fd=%d", fd);

   ASSERT(uci != NULL);

   return User_TranslateStatus(UserObj_FDClose(uci, fd));
}

/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Creat - 
 *	Handler for linux syscall 8 
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
LinuxFileDesc_Creat(UserVAConst /*const char* */ userPath, int32 mode)
{
   UWLOG_SyscallUnsupported("use #5: open(%#x, %#x)",
                            userPath, mode);
   return LINUX_ENOSYS;
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Link - 
 *	Handler for linux syscall 9 
 * Support: 95%
 *      Making a hard link to a symlink follows the symlink first,
 *      unlike Linux.  The GNU ln utility prints "warning: making a
 *      hard link to a symbolic link is not portable" if you try to do
 *      this, so we I doubt we need to support code that depends on it.
 * Error case: 100%
 *
 * Results:
 *	0 on success, or appropriate Linux error code
 *
 * Side effects:
 *      newPath is created as a hard link to the existing object named
 *      oldPath.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Link(UserVAConst /*const char* */ oldPath,
                   UserVAConst /*const char* */ newPath)
{
   User_CartelInfo* const uci = MY_USER_CARTEL_INFO;
   char* vmkOldPath;
   char* vmkNewPath;
   int res;
   VMK_ReturnStatus status;
   UserObj *oldObj;
   UserObj *newParent;
   char arc[LINUX_ARC_MAX+1];

   res = LinuxFileDescAllocAndCopyPath(uci, oldPath, &vmkOldPath);
   if (res != 0) {
      return res;
   }

   res = LinuxFileDescAllocAndCopyPath(uci, newPath, &vmkNewPath);
   if (res != 0) {
      LinuxFileDescFreePath(uci, vmkOldPath);
      return res;
   }

   UWLOG_SyscallEnter("(oldPath=%s, newPath=%s)", vmkOldPath, vmkNewPath);

   /* Look up existing object */
   status = UserObj_Open(uci, vmkOldPath, USEROBJ_OPEN_STAT, 0, &oldObj);
   if (status != VMK_OK) {
      res = User_TranslateStatus(status);
      goto errExit;
   }

   /* Look up new parent directory */
   status = UserObj_TraversePath(uci, vmkNewPath, USEROBJ_OPEN_PENULTIMATE,
                                 0, &newParent, arc, sizeof arc);
   if (status != VMK_OK) {
      (void) UserObj_Release(uci, oldObj);
      res = User_TranslateStatus(status);
      goto errExit;
   }

   status = newParent->methods->makeHardLink(newParent, arc, oldObj);
   (void) UserObj_Release(uci, newParent);
   (void) UserObj_Release(uci, oldObj);
   res = User_TranslateStatus(status);

  errExit:
   LinuxFileDescFreePath(uci, vmkOldPath);
   LinuxFileDescFreePath(uci, vmkNewPath);
   return res;
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Unlink - 
 *	Handler for linux syscall 10 
 * Support: 100%
 * Error case: 100%
 * Results:
 *	0
 *
 * Side effects:
 *	Named file is deleted
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Unlink(UserVAConst /* const char* */ userPath)
{
   User_CartelInfo* const uci = MY_USER_CARTEL_INFO;
   char* vmkPath;
   int res;
   VMK_ReturnStatus status;

   ASSERT(uci != NULL);

   res = LinuxFileDescAllocAndCopyPath(uci, userPath, &vmkPath);
   if (res != 0) {
      return res;
   }

   UWLOG_SyscallEnter("(path=%s)", vmkPath);

   status = UserObj_Unlink(uci, vmkPath);

   LinuxFileDescFreePath(uci, vmkPath);
   return User_TranslateStatus(status);
}


/*
 *----------------------------------------------------------------------
 * 
 * LinuxFileDesc_Mknod - 
 *      Handler for linux syscall 14 
 * Support: 25% (only support S_IFIFO flag)
 * Error case: 100%
 * Results:
 *	0 on success, or appropriate Linux error code
 *
 * Side effects:
 *	A new special file is created.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Mknod(UserVAConst /* const char* */ userPath,
		    LinuxMode mode,
		    UNUSED_PARAM(uint64 unusedDevId))
{
   User_CartelInfo* const uci = MY_USER_CARTEL_INFO;
   char* vmkpath;
   int res;
   VMK_ReturnStatus status;
   UserObj* parent;
   char arc[LINUX_ARC_MAX + 1];

   res = LinuxFileDescAllocAndCopyPath(uci, userPath, &vmkpath);
   if (res != 0) {
      return res;
   }

   UWLOG_SyscallEnter("(path=%s, mode=%#x, dev=%#llx)", vmkpath,
		      (uint32)mode, unusedDevId);

   /*
    * Currently we only support creating fifos.
    */                                                          
   if (!(mode & LINUX_MODE_IFIFO) ||
       (mode & (LINUX_MODE_IFREG | LINUX_MODE_IFCHR | LINUX_MODE_IFBLK))) {
      UWLOG(0, "Unsupported mode %#x", mode);
      res = LINUX_EINVAL;
   } else {
      UserObj_FDLock(&uci->fdState);
      mode &= ~uci->fdState.umask;
      UserObj_FDUnlock(&uci->fdState);

      status = UserObj_TraversePath(uci, vmkpath, USEROBJ_OPEN_PENULTIMATE, 0,
                                    &parent, arc, sizeof arc);
      if (status == VMK_OK) {
         status = parent->methods->mknod(parent, arc, mode);
         (void) UserObj_Release(uci, parent);
      }
      res = User_TranslateStatus(status);
   }

   LinuxFileDescFreePath(uci, vmkpath);
   
   return res;
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Access - 
 *	Handler for linux syscall 33 
 * Support: 100%
 * Error case: 100%
 *
 * Results:
 *      Checks whether the real (not effective!) uid of the cartel has
 *      access permission on the specified path.  0 on success, or
 *      appropriate Linux error code.
 *
 * Side effects:
 *      None.
 *
 * Strategy:
 *      Temporarily set the effective UID/GID of this thread to the
 *      real UID/GID, do the check, then set them back.  A somewhat
 *      low-performance implementation, but it avoids adding
 *      complexity to the access checking code to support checking
 *      against real instead of effective UID/GID.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Access(UserVAConst /* const char* */ userPath, int32 mode)
{
   char* vmkPath;
   int res;
   User_CartelInfo* const uci = MY_USER_CARTEL_INFO;
   Identity* ident = &MY_RUNNING_WORLD->ident;
   VMK_ReturnStatus status;
   Bool diff = (ident->euid != ident->ruid || ident->egid != ident->rgid);
   LinuxUID euid = ident->euid;
   LinuxUID egid = ident->egid;
   UserObj* obj;
   LinuxStat64 vmkStatbuf;

   res = LinuxFileDescAllocAndCopyPath(uci, userPath, &vmkPath);
   if (res != 0) {
      return res;
   }

   UWLOG_SyscallEnter("(pathname=%s, mode=%#x)", vmkPath, mode);

   // Special case: access("", flags) returns ENOENT on Linux (PR 48558).
   if (vmkPath[0] == '\0') {
      LinuxFileDescFreePath(uci, vmkPath);
      return LINUX_ENOENT;
   }

   if (diff) {
      status = UserProxy_Setresuid(uci, -1, ident->ruid, -1);
      if (status != VMK_OK) {
         goto out;
      }
      ident->euid = ident->ruid;
      status = UserProxy_Setresgid(uci, -1, ident->rgid, -1);
      if (status != VMK_OK) {
         goto out;
      }
      ident->egid = ident->rgid;
   }

   status = UserObj_Open(uci, vmkPath, USEROBJ_OPEN_STAT, 0, &obj);
   if (status != VMK_OK) {
      goto out;
   }

   status = obj->methods->stat(obj, &vmkStatbuf);
   (void) UserObj_Release(uci, obj);
   if (status != VMK_OK) {
      goto out;
   }

   status = UserIdent_CheckAccessMode(ident, mode, vmkStatbuf.st_uid,
                                      vmkStatbuf.st_gid, vmkStatbuf.st_mode);

 out:
   if (diff) {
      (void) UserProxy_Setresuid(uci, -1, euid, -1);
      ident->euid = euid;
      (void) UserProxy_Setresgid(uci, -1, egid, -1);
      ident->egid = egid;
   }

   LinuxFileDescFreePath(uci, vmkPath);

   return User_TranslateStatus(status);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Rename - 
 *	Handler for linux syscall 38 
 * Support: 100%
 * Error case: 100%
 *
 * Results:
 *      0 or Linux error code
 *
 * Side effects:
 *      Object oldPath is renamed to newPath.  If newPath previously
 *      existed, it is unlinked first.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Rename(UserVAConst /* const char* */ oldPath,
                     UserVAConst /* const char* */ newPath)
{
   User_CartelInfo* const uci = MY_USER_CARTEL_INFO;
   char* vmkOldPath;
   char* vmkNewPath;
   int res;
   VMK_ReturnStatus status;
   UserObj *oldParent;
   char oldArc[LINUX_ARC_MAX+1];
   UserObj *newParent;
   char newArc[LINUX_ARC_MAX+1];

   res = LinuxFileDescAllocAndCopyPath(uci, oldPath, &vmkOldPath);
   if (res != 0) {
      return res;
   }
   res = LinuxFileDescAllocAndCopyPath(uci, newPath, &vmkNewPath);
   if (res != 0) {
      LinuxFileDescFreePath(uci, vmkOldPath);
      return res;
   }

   UWLOG_SyscallEnter("(oldPath=%s, newPath=%s)", vmkOldPath, vmkNewPath);

   /* Look up old parent directory */
   status = UserObj_TraversePath(uci, vmkOldPath, USEROBJ_OPEN_PENULTIMATE,
                                 0, &oldParent, oldArc, sizeof oldArc);
   if (status == VMK_OK) {
      /* Look up new parent directory */
      status = UserObj_TraversePath(uci, vmkNewPath, USEROBJ_OPEN_PENULTIMATE,
                                    0, &newParent, newArc, sizeof newArc);
      if (status == VMK_OK) {
         /* Do the rename */
         status = newParent->methods->rename(newParent, newArc, oldParent, oldArc);
         (void) UserObj_Release(uci, newParent);
      }
      (void) UserObj_Release(uci, oldParent);
   }

   LinuxFileDescFreePath(uci, vmkOldPath);
   LinuxFileDescFreePath(uci, vmkNewPath);

   return User_TranslateStatus(status);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Mkdir - 
 *	Handler for linux syscall 39 
 * Support: 100%
 * Error case: 100%
 * Results:
 *      0 or Linux error code
 *
 * Side effects:
 *      Makes a directory.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Mkdir(UserVAConst /* const char* */ userPath, LinuxMode mode)
{
   User_CartelInfo* const uci = MY_USER_CARTEL_INFO;
   char* vmkPath;
   int res;
   VMK_ReturnStatus status;
   UserObj *parent;
   char arc[LINUX_ARC_MAX+1];

   ASSERT(uci != NULL);

   res = LinuxFileDescAllocAndCopyPath(uci, userPath, &vmkPath);
   if (res != 0) {
      return res;
   }

   UWLOG_SyscallEnter("(path=%s, mode=0%o)", vmkPath, mode);

   UserObj_FDLock(&uci->fdState);
   mode &= ~uci->fdState.umask;
   UserObj_FDUnlock(&uci->fdState);

   status = UserObj_TraversePath(uci, vmkPath,
                                 USEROBJ_OPEN_PENULTIMATE|USEROBJ_OPEN_IGNTRAILING,
                                 0, &parent, arc, sizeof arc);
   if (status == VMK_OK) {
      status = parent->methods->mkdir(parent, arc, mode);
      (void) UserObj_Release(uci, parent);
   }

   LinuxFileDescFreePath(uci, vmkPath);

   return User_TranslateStatus(status);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Rmdir - 
 *	Handler for linux syscall 40 
 * Support: 100%
 * Error case:
 * Results:
 *      0 or Linux error code
 *
 * Side effects:
 *      Removes a directory.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Rmdir(UserVAConst /* const char* */ userPath)
{
   User_CartelInfo* const uci = MY_USER_CARTEL_INFO;
   char* vmkPath;
   int res;
   VMK_ReturnStatus status;
   UserObj *parent;
   char arc[LINUX_ARC_MAX+1];

   ASSERT(uci != NULL);

   res = LinuxFileDescAllocAndCopyPath(uci, userPath, &vmkPath);
   if (res != 0) {
      return res;
   }

   UWLOG_SyscallEnter("(path=%s)", vmkPath);

   status =
      UserObj_TraversePath(uci, vmkPath,
                           USEROBJ_OPEN_PENULTIMATE|USEROBJ_OPEN_IGNTRAILING,
                           0, &parent, arc, sizeof arc);
   if (status == VMK_OK) {
      status = parent->methods->rmdir(parent, arc);
      (void) UserObj_Release(uci, parent);
   }

   LinuxFileDescFreePath(uci, vmkPath);

   return User_TranslateStatus(status);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Dup - 
 *	Handler for linux syscall 41 
 * Support: 100%
 * Error case: 100%
 * Results:
 *      File descriptor number or Linux error code.
 *
 * Side effects:
 *	Duplicates a file descriptor.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Dup(LinuxFd fd)
{
   User_CartelInfo* const uci = MY_USER_CARTEL_INFO;
   VMK_ReturnStatus status;
   LinuxFd newfd;

   UWLOG_SyscallEnter("(%d)", fd);
   ASSERT(uci != NULL);

   status = UserObj_FDDup(uci, fd, 0, &newfd);
   if (status == VMK_OK) {
      return newfd;
   } else {
      return User_TranslateStatus(status);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Pipe - 
 *	Handler for linux syscall 42 
 * Support: 100%
 * Error case: 100%
 * Results:
 *      0 or Linux error code
 *
 * Side effects:
 *      Creates a pipe.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Pipe(UserVA pipefds)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   VMK_ReturnStatus status;
   LinuxFd kfds[2];
   
   ASSERT(uci != NULL);

   status = UserPipe_Open(uci, &(kfds[0]), &(kfds[1]));
   if (status == VMK_OK) {
      status = User_CopyOut(pipefds, kfds, sizeof kfds);
      if (status != VMK_OK) {
         UserObj_FDClose(uci, kfds[0]);
         UserObj_FDClose(uci, kfds[1]);
      }
   }
   return User_TranslateStatus(status);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDescPackIoctl - 
 *
 *      Pack an ioctl with embedded pointers.
 *
 * Results:
 *      VMK_OK if the packing was successful or error if failed.
 *
 * Side effects:
 *
 *      The packed ioctl argument buffer format:
 *
 *          -   +============================+
 *          ^   |   sizeof original ioctl    |  \
 *          |   +----------------------------+   | packed parm hdr
 *          |   | n (number of packed args)  |  /
 *          |   +============================+                  _
 *          |   |                            |  \               ^
 *          |   |   ioctl struct field #0    |   |  m           |
 *          |   |                            |   |  arbitrary   |
 *          |   +----------------------------+   |  sized       |   D
 *          |   |                            |   |  fields      |   a
 *          |   |   ioctl struct field #1    |   |  with        | v t
 *          |   |                            |   |  n           | m a
 *          |   +----------------------------+   |  fields      | k S
 *          |   |         ...                |   |  of          |   i
 *          |   +----------------------------+   |  embedded    |   z
 *          |   |                            |   |  pointers    |   e
 *          |   |   ioctl struct field #m    |   |              |
 *          |   |                            |  /               v
 *          |   +============================+                  -
 *          |   |     packedArg.offset 0     |  \
 *        b |   +----------------------------+   |
 *        u |   |     packedArg.length 0     |   | n packed arg
 *        f |   +============================+   | structs with
 *        S |   |     packedArg.offset 1     |   | offset and 
 *        i |   +----------------------------+   | length data
 *        z |   |     packedArg.length 1     |   |
 *        e |   +============================+   | offset value is
 *          |   |         ...                |   | the field
 *          |   +============================+   | offset in the
 *          |   |     packedArg.offset n     |   | original
 *          |   +----------------------------+   | ioctl struct
 *          |   |     packedArg.length n     |  /
 *          |   +============================+
 *          |   |                            |   ^
 *          |   |   ioctl embedded data #0   |   | packedArg.length 0
 *          |   |                            |   v
 *          |   +----------------------------+   -
 *          |   |                            |   ^
 *          |   |   ioctl embedded data #1   |   | packedArg.length 1
 *          |   |                            |   v
 *          |   +----------------------------+   -
 *          |   |         ...                |
 *          |   +----------------------------+   -
 *          |   |                            |   ^
 *          |   |   ioctl embedded data #n   |   | packedArg.length n
 *          v   |                            |   v
 *          -   +============================+   -
 *
 *      We could augment the packed data to include a "display"
 *      structure for nested ioctl data. However, nested structs
 *      are currently not supported.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
LinuxFileDescPackIoctl(LinuxFd fd,
                       uint32 cmd,
                       LinuxIoctlPackedData *packedData,
                       uint32 vmkDataSize,
                       void *vmkData,
                       int nPacked,
                       ...)
{
   int i;
   va_list ap;
   uint32 offset;
   VMK_ReturnStatus status = VMK_OK;
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;

   /*
    * Allocate args struct
    */
   ASSERT(nPacked > 0);
   packedData->nPacked = nPacked;
   packedData->bufSize = 2 * sizeof(uint32) + vmkDataSize;
   packedData->packedArg = User_HeapAlloc(uci, sizeof(LinuxIoctlPackedDataArg) * nPacked);
   if (packedData->packedArg == NULL) {
      return LINUX_ENOMEM;
   }

   /*
    * Process args
    */
   va_start(ap, nPacked);
   for (i = 0; i < nPacked; i++) {
      packedData->packedArg[i].offset = va_arg(ap, uint32);
      ASSERT(packedData->packedArg[i].offset <= vmkDataSize - sizeof(void *));
      packedData->packedArg[i].length = va_arg(ap, uint32);
      packedData->bufSize += sizeof(LinuxIoctlPackedDataArg) +
                                    packedData->packedArg[i].length;
   } 
   va_end(ap);

   /*
    * Allocate buffer
    */
   packedData->buf = User_HeapAlloc(uci, packedData->bufSize);
   if (packedData->buf == NULL) {
      return LINUX_ENOMEM;
   }

   /*
    * Pack ioctl buffer
    */
   offset = 0;
   memcpy(packedData->buf, &vmkDataSize, sizeof(uint32)); 
   offset += sizeof(uint32);
   memcpy(packedData->buf + offset, &nPacked, sizeof(uint32)); 
   offset += sizeof(uint32);
   memcpy(packedData->buf + offset, vmkData, vmkDataSize);
   offset += vmkDataSize;
   memcpy(packedData->buf + offset, packedData->packedArg,
          sizeof(LinuxIoctlPackedDataArg) * nPacked);
   offset += sizeof(LinuxIoctlPackedDataArg) * nPacked;
   for (i = 0; i < nPacked; i++) {
      status = User_CopyIn(packedData->buf + offset,
                           *(UserVA *)(vmkData + packedData->packedArg[i].offset),
                           packedData->packedArg[i].length);
      if (status != VMK_OK) {
         return status;
      }
      offset += packedData->packedArg[i].length;
      UWLOG(2, "Packed ioctl fd=%d, cmd=%#x, offset=%u, length=%u",
         fd, cmd, packedData->packedArg[i].offset,
         packedData->packedArg[i].length);
   }
   ASSERT(offset == packedData->bufSize);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDescUnpackIoctl - 
 *
 *      Unpack an ioctl with embedded pointers.
 *
 * Results:
 *      VMK_OK if the unpacking was successful or error if failed.
 *
 * Notes:
 *      See the comments in LinuxFileDescPackIoctl().
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
LinuxFileDescUnpackIoctl(LinuxFd fd,
                         uint32 cmd,
                         LinuxIoctlPackedData *packedData,
                         uint32 vmkDataSize,
                         void *vmkData,
                         UserVA userData)
{
   int i;
   VMK_ReturnStatus status = VMK_OK;
   LinuxIoctlPackedDataArg packedArg;
   uint32 bufOffset, dataSize, nPacked;

   /*
    * Extract ioctl size
    */
   bufOffset = 0;
   memcpy(&dataSize, packedData->buf, sizeof(uint32));
   ASSERT(dataSize == vmkDataSize);
   bufOffset += sizeof(uint32);

   /*
    * Extract nPacked
    */
   memcpy(&nPacked, packedData->buf + bufOffset, sizeof(uint32));
   ASSERT(nPacked == packedData->nPacked);
   bufOffset += sizeof(uint32);

   /*
    * Copy out ioctl struct
    */
   status = User_CopyOut(userData, packedData->buf + bufOffset, vmkDataSize);
   if (status != VMK_OK) {
      return status;
   }
   bufOffset += vmkDataSize;

   /*
    * Check packed args
    */
   for (i = 0; i < packedData->nPacked; i++) {
      memcpy(&packedArg, packedData->buf + bufOffset, sizeof(LinuxIoctlPackedDataArg));
      UWLOG(2, "Unpacking ioctl fd=%d, cmd=%#x, offset=%u, length=%u",
            fd, cmd, packedArg.offset, packedArg.length);
      ASSERT(packedArg.offset == packedData->packedArg[i].offset);
      ASSERT(packedArg.length == packedData->packedArg[i].length);
      bufOffset += sizeof(LinuxIoctlPackedDataArg);
   } 

   /*
    * Process embedded args
    */
   for (i = 0; i < packedData->nPacked; i++) {
      status = User_CopyOut(*(UserVA *)(vmkData + packedData->packedArg[i].offset),
                            packedData->buf + bufOffset,
                            packedData->packedArg[i].length);
      if (status != VMK_OK) {
         return status;
      }
      bufOffset += packedData->packedArg[i].length;
   } 

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDescCheckIoctl - 
 *
 *      Check an ioctl for embedded pointers.
 *
 * Results:
 *      VMK_OK if the embedded pointer is NULLed or error if failed.
 *
 * Side effects:
 *      Depending upon the ioctl, embedded pointers are either NULLed
 *      or packed and unpacked.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
LinuxFileDescCheckIoctl(LinuxFd fd, uint32 cmd,
                        LinuxFileDescIoctlDir dir,
                        UserVA userData,
                        LinuxIoctlPackedData *packedData)
{
   VMK_ReturnStatus status = VMK_OK;
   struct LinuxFloppy_struct vmkFloppyStruct;
   struct LinuxFloppy_raw_cmd vmkFloppyRawCmd;
   struct LinuxFloppy_drive_struct vmkFloppyDriveStruct;

   switch (LINUX_IOCTL_CMD(cmd)) {
   case LINUX_FLOPPY_FDGETPRM:
      status = User_CopyIn(&vmkFloppyStruct, userData,
                           sizeof(struct LinuxFloppy_struct));
      if (status != VMK_OK) {
         return status;
      }
      if (vmkFloppyStruct.name) {
         UWWarn("UNIMPLEMENTED (fd=%d, cmd=%#x, name=%#x)",
                fd, cmd, (UserVA)vmkFloppyStruct.name);
         UWLOG_StackTraceCurrent(1);
         vmkFloppyStruct.name = 0;
         status = User_CopyOut(userData, &vmkFloppyStruct,
                               sizeof(struct LinuxFloppy_struct));
      }
      break;
   case LINUX_FLOPPY_FDGETDRVSTAT:
   case LINUX_FLOPPY_FDPOLLDRVSTAT:
      status = User_CopyIn(&vmkFloppyDriveStruct, userData,
                           sizeof(struct LinuxFloppy_drive_struct));
      if (status != VMK_OK) {
         return status;
      }
      if (vmkFloppyDriveStruct.dmabuf) {
         UWWarn("UNIMPLEMENTED (fd=%d, cmd=%#x, dmabuf=%#x)",
                fd, cmd, (UserVA)vmkFloppyDriveStruct.dmabuf);
         UWLOG_StackTraceCurrent(1);
         vmkFloppyDriveStruct.dmabuf = 0;
         status = User_CopyOut(userData, &vmkFloppyDriveStruct,
                               sizeof(struct LinuxFloppy_drive_struct));
      }
      break;
   case LINUX_FLOPPY_FDRAWCMD:
      status = User_CopyIn(&vmkFloppyRawCmd, userData,
                           sizeof(struct LinuxFloppy_raw_cmd));
      if (status != VMK_OK) {
         return status;
      }

      if (vmkFloppyRawCmd.kernel_data ||
                 vmkFloppyRawCmd.next ||
                 (vmkFloppyRawCmd.data && vmkFloppyRawCmd.length == 0)) {
         /*
          * Windows guests sometimes set the length field to 0,
          *    but leave garbage in the data field pointer.
          *
          * Don't spew messages for Windows guests.
          */
         if (vmkFloppyRawCmd.data && vmkFloppyRawCmd.length == 0) {
            UWLOG(1, "UNIMPLEMENTED (fd=%d, cm=%#x, data=%#x length=%lu)",
                   fd, cmd, (UserVA)vmkFloppyRawCmd.data,
                   vmkFloppyRawCmd.length);
            UWLOG_StackTraceCurrent(1);
         } else {
            UWWarn("UNIMPLEMENTED (fd=%d, cm=%#x, kernel_data=%#x, next=%#x data=%#x length=%lu)",
                   fd, cmd, (UserVA)vmkFloppyRawCmd.kernel_data,
                   (UserVA)vmkFloppyRawCmd.next,
                   (UserVA)vmkFloppyRawCmd.data,
                   vmkFloppyRawCmd.length);
            UWLOG_StackTraceCurrent(1);
         }
         vmkFloppyRawCmd.kernel_data = 0;
         vmkFloppyRawCmd.next = 0;
         vmkFloppyRawCmd.data = 0;
         status = User_CopyOut(userData, &vmkFloppyRawCmd,
                               sizeof(struct LinuxFloppy_drive_struct));
      }

      /*
       * FD_RAW_READ and FD_RAW_WRITE makes use of the
       * embedded data pointer. Pack and unpack it.
       */
      if ((status == VMK_OK) &&
            (vmkFloppyRawCmd.data && (vmkFloppyRawCmd.length > 0))) {
         if (dir == LINUX_FILE_IOCTL_IN) {
            UWLOG(2, "Packing ioctl fd=%d, cmd=%#x, data=%#x length=%lu",
                  fd, cmd, (UserVA)vmkFloppyRawCmd.data, vmkFloppyRawCmd.length);
            status = LinuxFileDescPackIoctl(fd, cmd, packedData, sizeof(vmkFloppyRawCmd),
                        &vmkFloppyRawCmd, 1, offsetof(struct LinuxFloppy_raw_cmd, data),
                        vmkFloppyRawCmd.length);
         } else {
            UWLOG(2, "Unpacking ioctl fd=%d, cmd=%#x, data=%#x length=%lu",
                  fd, cmd, (UserVA)vmkFloppyRawCmd.data, vmkFloppyRawCmd.length);
            status = LinuxFileDescUnpackIoctl(fd, cmd, packedData, sizeof(vmkFloppyRawCmd),
                        &vmkFloppyRawCmd, userData);
         }
      }
      break;
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Ioctl - 
 *	Handler for linux syscall 54 
 * Support: 1% (only as needed)
 * Error case: 50% (when inapplicable, returns EINVAL instead of ENOTTY)
 * Results:
 *      Linux error code or cmd-specific return value.
 *
 * Side effects:
 *      May read or write data at address userData, depending on cmd.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Ioctl(LinuxFd fd, uint32 cmd, UserVA userData)
{
   UserObj* obj;
   VMK_ReturnStatus status;
   uint32 result, argSize = 0;
   LinuxIoctlPackedData packedData;
   User_CartelInfo* uci = MY_USER_CARTEL_INFO;
   LinuxIoctlArgType argType = LINUX_IOCTL_ARG_PTR;

   status = UserObj_Find(uci, fd, &obj);
   if (status != VMK_OK) {
      return User_TranslateStatus(status);
   }

   UWLOG_SyscallEnter("(fd=%d, cmd=%#x, userData=%#x)", fd, cmd, userData);

   memset(&packedData, 0, sizeof(packedData));

   /*
    * Dispatch based on the ioctl command.
    *   Some ioctl's take pointers to user data, while others take
    *   constants. Based on the command, set up the right one for
    *   our ioctl method. We copy pointer data in/out even if it's
    *   only unidirectional. The corresponding call by the proxy in
    *   the COS will do the right thing.
    */
   switch (LINUX_IOCTL_CMD(cmd)) {
   /*
    * Floppy (byte 0x02)
    *    Structs are checked for embedded pointers and packed if necessary.
    */
   case LINUX_FLOPPY_FDGETPRM:
      status = LinuxFileDescCheckIoctl(fd, cmd, LINUX_FILE_IOCTL_IN,
                                       userData, &packedData);
      argSize = sizeof(struct LinuxFloppy_struct);
      break;
   case LINUX_FLOPPY_FDGETDRVTYP:
      argSize = sizeof(LinuxFloppy_drive_name);
      break;
   case LINUX_FLOPPY_FDGETDRVSTAT:
   case LINUX_FLOPPY_FDPOLLDRVSTAT:
      status = LinuxFileDescCheckIoctl(fd, cmd, LINUX_FILE_IOCTL_IN,
                                       userData, &packedData);
      argSize = sizeof(struct LinuxFloppy_drive_struct);
      break;
   case LINUX_FLOPPY_FDFLUSH:
   case LINUX_FLOPPY_FDRESET:
      argType = LINUX_IOCTL_ARG_CONST;
      argSize = sizeof(uint32);
      break;
   case LINUX_FLOPPY_FDRAWCMD:
      status = LinuxFileDescCheckIoctl(fd, cmd, LINUX_FILE_IOCTL_IN,
                                       userData, &packedData);
      if (status == VMK_OK) {
         if (packedData.nPacked > 0) {
            argType = LINUX_IOCTL_ARG_PACKED;
            argSize = packedData.bufSize;
         } else {
            argSize = sizeof(struct LinuxFloppy_raw_cmd);
         }
      }
      break;

   /*
    * Filesystem (byte 0x12)
    */
   case LINUX_BLKGETSIZE:           /* Get block device size */
   case LINUX_BLKSSZGET:            /* Get block device sector size */
      argSize = sizeof(uint32);
      break;

   /*
    * CDROM (byte 0x53)
    */
   case LINUX_CDROMPLAYMSF:         /* Play audio MSF */
      argSize = sizeof(struct Linux_cdrom_msf);
      break;
   case LINUX_CDROMPLAYTRKIND:      /* Play audio track/index */
      argSize = sizeof(struct Linux_cdrom_ti);
      break;
   case LINUX_CDROMREADTOCHDR:      /* Read TOC header */
      argSize = sizeof(struct Linux_cdrom_tochdr);
      break;
   case LINUX_CDROMREADTOCENTRY:    /* Read TOC entry */
      argSize = sizeof(struct Linux_cdrom_tocentry);
      break;
   case LINUX_CDROMVOLCTRL:         /* Control volume */
   case LINUX_CDROMVOLREAD:         /* Get the drive's volume setting */
      argSize = sizeof(struct Linux_cdrom_volctrl);
      break;
   case LINUX_CDROMSUBCHNL:         /* Read subchannel data */
      argSize = sizeof(struct Linux_cdrom_subchnl);
      break;
   case LINUX_CDROMMULTISESSION:    /* Obtain start-of-last-session */
      argSize = sizeof(struct Linux_cdrom_multisession);
      break;
   case LINUX_CDROM_GET_MCN:        /* Obtain UPC */
      argSize = sizeof(struct Linux_cdrom_mcn);
      break;
   case LINUX_CDROMPAUSE:           /* Pause audio */
   case LINUX_CDROMRESUME:          /* Resume audio */
   case LINUX_CDROMSTOP:            /* Stop drive */
   case LINUX_CDROMSTART:           /* Start drive */
   case LINUX_CDROMEJECT:           /* Eject media */
   case LINUX_CDROMEJECT_SW:        /* enable=1/disable=0 auto-ejecting */
   case LINUX_CDROMRESET:           /* Hard reset */
   case LINUX_CDROMCLOSETRAY:       /* Close tray */
   case LINUX_CDROM_SET_OPTIONS:    /* Set behavior options */
   case LINUX_CDROM_CLEAR_OPTIONS:  /* Clear behavior options */
   case LINUX_CDROM_SELECT_SPEED:   /* Set speed */
   case LINUX_CDROM_SELECT_DISC:    /* Select disc */
   case LINUX_CDROM_MEDIA_CHANGED:  /* Check media changed */
   case LINUX_CDROM_DRIVE_STATUS:   /* Get tray position */
   case LINUX_CDROM_DISC_STATUS:    /* Get disc type, etc. */
   case LINUX_CDROM_CHANGER_NSLOTS: /* Get number of slots */ 
   case LINUX_CDROM_LOCKDOOR:       /* Lock or unlock door */
   case LINUX_CDROM_DEBUG:          /* Turn debug messages on/off */
   case LINUX_CDROM_GET_CAPABILITY: /* get capabilities */
      argType = LINUX_IOCTL_ARG_CONST;
      argSize = sizeof(uint32);
      break;

   /*
    * Terminal (byte 0x54)
    */
   case LINUX_TCGETS:               /* Get termios struct */
   case LINUX_TCSETS:               /* Set termios struct */
      argSize = sizeof(struct Linux_termios);
      break;
   case LINUX_FIONREAD:             /* Get # bytes in receive buffer */
   case LINUX_TIOCMGET:             /* Get status of control lines */
   case LINUX_TIOCMBIS:             /* Enable RTS, DTR or loopback regs */
   case LINUX_TIOCMBIC:             /* Disable RTS, DTR or loopback regs */
      argSize = sizeof(uint32);
      break;
   case LINUX_FIONBIO:              /* Set non-blocking i/o */
   case LINUX_TIOCSBRK:             /* Set break (BSD compatibility) */
   case LINUX_TIOCCBRK:             /* Clear break (BSD compatibility) */
      argType = LINUX_IOCTL_ARG_CONST;
      argSize = sizeof(uint32);
      break;

   /*
    * Parallel port (byte 0x70)
    */
   case  LINUX_PPCLAIM:             /* Claim the port */
   case  LINUX_PPRELEASE:           /* Release the port */
   case  LINUX_PPYIELD:             /* Yield the port */
   case  LINUX_PPEXCL:              /* Register device exclusively */
      argType = LINUX_IOCTL_ARG_CONST;
      argSize = sizeof(uint32);
      break;

   /*
    * Funky vmfs ioctls for disklib
    */
   case IOCTLCMD_VMFS_GET_FILE_HANDLE:
   case IOCTLCMD_VMFS_GET_FREE_SPACE:
      argSize = sizeof(uint64);
      break;

   default:
      UWLOG_SyscallUnsupported("(fd=%u, cmd=%#x, userData=%#x)",
                               fd, cmd, userData);
      status = VMK_NOT_SUPPORTED;
      break;
   }

   if (status != VMK_OK) {
      goto end;
   }

   /*
    * Invoke ioctl method
    */
   if (argType == LINUX_IOCTL_ARG_PACKED) {
      status = obj->methods->ioctl(obj, cmd, argType, argSize,
                                   &packedData, &result);
   } else {
      status = obj->methods->ioctl(obj, cmd, argType, argSize,
                                   (void *)userData, &result);
   }

   switch (LINUX_IOCTL_CMD(cmd)) {
   case LINUX_FLOPPY_FDGETPRM:
      status = LinuxFileDescCheckIoctl(fd, cmd, LINUX_FILE_IOCTL_OUT,
                                       userData, &packedData);
      break;
   case LINUX_FLOPPY_FDGETDRVSTAT:
   case LINUX_FLOPPY_FDPOLLDRVSTAT:
      status = LinuxFileDescCheckIoctl(fd, cmd, LINUX_FILE_IOCTL_OUT,
                                       userData, &packedData);
      break;
   case LINUX_FLOPPY_FDRAWCMD:
      status = LinuxFileDescCheckIoctl(fd, cmd, LINUX_FILE_IOCTL_OUT,
                                       userData, &packedData);
      break;
   }

  end:
   (void)UserObj_Release(uci, obj);

   if (packedData.buf) {
      User_HeapFree(uci, packedData.buf);
   }

   if (packedData.packedArg) {
      User_HeapFree(uci, packedData.packedArg);
   }

   if (status == VMK_OK) {
      return result;
   }

   return User_TranslateStatus(status);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Fcntl - 
 *	Handler for linux syscall 55 
 * Support: 0% (use fcntl64)
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
LinuxFileDesc_Fcntl(LinuxFd fd, uint32 cmd, uint32 arg)
{
   /* Require the fcntl64 API. */
   UWLOG_SyscallUnsupported("(%u, %u, %#x) -- use Fcntl64",
                            fd, cmd, arg);
   return LINUX_ENOSYS;
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Umask - 
 *	Handler for linux syscall 60 
 * Support: 100%
 * Error case: 100%
 *
 * Results:
 *      Old value of umask
 *
 * Side effects:
 *      Sets the cartel's umask to newmask & 0777.
 *
 *----------------------------------------------------------------------
 */
uint32
LinuxFileDesc_Umask(uint32 newmask)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   uint32 oldmask;

   UserObj_FDLock(&uci->fdState);
   oldmask = uci->fdState.umask;
   uci->fdState.umask = newmask & 0777;
   UserObj_FDUnlock(&uci->fdState);

   return oldmask;
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Dup2 - 
 *	Handler for linux syscall 63 
 * Support: 100%
 * Error case:
 * Results:
 *      File descriptor number or Linux error code
 *
 * Side effects:
 *      Duplicates a file descriptor.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Dup2(LinuxFd from, LinuxFd to)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   VMK_ReturnStatus status;

   UWLOG_SyscallEnter("(%d, %d)", from, to);

   ASSERT(uci != NULL);

   status = UserObj_FDDup2(uci, from, to);
   if (status == VMK_OK) {
      return to;
   } else {
      return User_TranslateStatus(status);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Symlink - 
 *	Handler for linux syscall 83 
 * Support: 100%
 * Error case: 100%
 *
 * Results:
 *      0 or Linux error code
 *
 * Side effects:
 *      Makes a symlink.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Symlink(UserVAConst /* const char* */ userTo,
                      UserVAConst /* const char* */ userPath)
{
   User_CartelInfo* const uci = MY_USER_CARTEL_INFO;
   char* vmkPath;
   char* vmkTo;
   int res;
   VMK_ReturnStatus status;
   UserObj *parent;
   char arc[LINUX_ARC_MAX+1];

   ASSERT(uci != NULL);

   res = LinuxFileDescAllocAndCopyPath(uci, userPath, &vmkPath);
   if (res != 0) {
      return res;
   }
   res = LinuxFileDescAllocAndCopyPath(uci, userTo, &vmkTo);
   if (res != 0) {
      LinuxFileDescFreePath(uci, vmkPath);
      return res;
   }

   UWLOG_SyscallEnter("(path=%s, to=%s)", vmkPath, vmkTo);

   status = UserObj_TraversePath(uci, vmkPath, USEROBJ_OPEN_PENULTIMATE,
                                 0, &parent, arc, sizeof arc);
   if (status == VMK_OK) {
      status = parent->methods->makeSymLink(parent, arc, vmkTo);
      (void) UserObj_Release(uci, parent);
   }

   LinuxFileDescFreePath(uci, vmkPath);
   LinuxFileDescFreePath(uci, vmkTo);

   return User_TranslateStatus(status);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Readlink - 
 *	Handler for linux syscall 85 
 * Support: 100%
 * Error case: 90%
 *      (If the name doesn't fit into the buffer, we may return
 *      ENAMETOOLONG, but the Linux man page claims we should silently
 *      truncate it instead.  Ugh.)
 *
 * Results:
 *      bytes used in userBuf or Linux error code
 *
 * Side effects:
 *      Reads symlink value into userBuf.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Readlink(UserVAConst /* const char* */ userPath,
                       UserVA /* uint8* */ userBuf,
                       int32 count)
{
   User_CartelInfo* const uci = MY_USER_CARTEL_INFO;
   char* vmkPath;
   int res;
   VMK_ReturnStatus status;
   UserObj* obj;
   char arc[LINUX_ARC_MAX+1];

   ASSERT(uci != NULL);
   if (count < 0) {
      return LINUX_EINVAL;
   } else if (count > LINUX_PATH_MAX) {
      count = LINUX_PATH_MAX;
   }

   res = LinuxFileDescAllocAndCopyPath(uci, userPath, &vmkPath);
   if (res != 0) {
      return res;
   }

   UWLOG_SyscallEnter("(path=%s buf=%#x)", vmkPath, userBuf);

   status = UserObj_TraversePath(uci, vmkPath, USEROBJ_OPEN_PENULTIMATE, 0,
                                 &obj, arc, sizeof arc);
   if (status == VMK_OK) {
      // reuse vmkPath buffer to receive the link value
      status = obj->methods->readSymLink(obj, arc, vmkPath, count);
      (void) UserObj_Release(uci, obj);
      if (status == VMK_OK) {
         if (count != 0) {
            /*
             * We must return the length and *not* null-terminate the buffer.
             * Strange but true.
             */
            count = MIN(count, strlen(vmkPath));
            status = User_CopyOut(userBuf, vmkPath, count);
         }
      }
   }

   LinuxFileDescFreePath(uci, vmkPath);

   if (status != VMK_OK) {
      return User_TranslateStatus(status);
   } else {
      return count;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Fsync - 
 *	Handler for linux syscall 118 
 * Support: 100%
 * Error case: 90%
 * Results:
 *      0 or Linux error code.
 *
 * Side effects:
 *      Force buffered writes on fd to disk.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Fsync(LinuxFd fd)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   VMK_ReturnStatus status;
   UserObj* obj;

   UWLOG_SyscallEnter("(fd=%d)", fd);

   status = UserObj_Find(uci, fd, &obj);
   if (status != VMK_OK) {
      return User_TranslateStatus(status);
   }
   Semaphore_Lock(&obj->sema);
   status = obj->methods->fsync(obj, FALSE);
   Semaphore_Unlock(&obj->sema);
   (void) UserObj_Release(uci, obj);
   return User_TranslateStatus(status);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Select - 
 *      Handler for linux syscall 142 
 * Support: 10% (only support read/write fds, just calls poll)
 * Error case: ?
 * Results:
 *      Most likely an error, but maybe 0 if we succeed.
 *
 * Side effects:
 *	Same as those of UserObj_Poll.
 *
 * Note:
 *      Upon return, timeout is not modified. Ordinarily, on Linux,
 *      timeout is modified to reflect the remaining time not slept.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Select(int32 n,
                     UserVA /* LinuxFdSet* */ readfds,
                     UserVA /* LinuxFdSet* */ writefds,
                     UserVA /* LinuxFdSet* */ exceptfds,
                     UserVA /* LinuxTimeval* */ timeout)
{
   int rc;
   int i, j;
   int numReady;
   int nPollFDs = 0;
   int nReadFDs = 0;
   int nWriteFDs = 0;
   int timeoutMillis;
   LinuxTimeval ktimeout;
   VMK_ReturnStatus status;
   LinuxPollfd *kpollfd = NULL;
   LinuxFdSet *kreadfds = NULL;
   LinuxFdSet *kwritefds = NULL;
   User_CartelInfo* uci = MY_USER_CARTEL_INFO;
   
   UWLOG_SyscallEnter("(%d, %#x, %#x, %#x, %#x)",
                      n, readfds, writefds, exceptfds, timeout);

   /*
    * exceptfds not supported
    */
   if (exceptfds != 0) {
      UWLOG(0, "select on except fds not supported");
      return LINUX_EINVAL;
   }

   if ((n < 0) || (n > USEROBJ_MAX_HANDLES)) {
      UWLOG(0, "Bogus fd count (%d) (max is %d).",
            n, USEROBJ_MAX_HANDLES);
      return LINUX_EINVAL;
   }

   if (n == 0) {
      UWLOG(0, "0-fd case not supported. Use a high-resolution timer.");
      return LINUX_EINVAL;
   }

   /*
    * Process readfds 
    */
   if (readfds != 0) {
      kreadfds = User_HeapAlloc(uci, sizeof *kreadfds);
      if (kreadfds == NULL) {
         rc = LINUX_ENOMEM;
	 goto end;
      }

      status = User_CopyIn(kreadfds, readfds, sizeof *kreadfds);
      if (status != VMK_OK) {
         rc = User_TranslateStatus(status);
         goto end;
      }

      /*
       * count number of readfds and nPollFDs
       */
      for (i = 0; i < n; i++) {
         if (LINUX_FD_ISSET(i, kreadfds)) {
            UWLOG(3, "read fd %d", i);
            nReadFDs++;
	    nPollFDs++;
         }
      }

      if (nReadFDs == 0) {
         UWLOG(0, "No fds set in readfds");
         rc = LINUX_EINVAL;
         goto end;
      }
   }

   /*
    * Process writefds
    */
   if (writefds != 0) {
      kwritefds = User_HeapAlloc(uci, sizeof *kwritefds);
      if (kwritefds == NULL) {
         rc = LINUX_ENOMEM;
	 goto end;
      }

      status = User_CopyIn(kwritefds, writefds, sizeof *kwritefds);
      if (status != VMK_OK) {
         rc = User_TranslateStatus(status);
         goto end;
      }

      /*
       * count number of writefds and nPollFDs
       */
      for (i = 0; i < n; i++) {
         if (LINUX_FD_ISSET(i, kwritefds)) {
            UWLOG(3, "write fd %d", i);
            nWriteFDs++;
	    if (kreadfds && !LINUX_FD_ISSET(i, kreadfds)) {
	       nPollFDs++;
            }
         }
      }

      if (nWriteFDs == 0) {
         UWLOG(0, "No fds set in writefds");
         rc = LINUX_EINVAL;
         goto end;
      }
   }

   /*
    * Process timeout.
    */
   if (timeout != 0) {
      status = User_CopyIn(&ktimeout, timeout, sizeof ktimeout);
      if (status != VMK_OK) {
         rc = User_TranslateStatus(status);
         goto end;
      }
      timeoutMillis = ktimeout.tv_sec * 1000 + ktimeout.tv_usec;
   } else {
      timeoutMillis = -1;
   }

   UWLOG(2, "nReadFDs=%d nWriteFDs=%d nPollFDs=%d",
         nReadFDs, nWriteFDs, nPollFDs);

   /*
    * Allocate and fill kpollfd
    */
   kpollfd = User_HeapAlloc(uci, nPollFDs * sizeof(LinuxPollfd));
   if (kpollfd == NULL) {
      rc = LINUX_ENOMEM;
      goto end;
   }
   memset(kpollfd, 0, nPollFDs * sizeof(LinuxPollfd));

   for (i = j = 0; i < n; i++) {
      if (kreadfds && LINUX_FD_ISSET(i, kreadfds)) {
         kpollfd[j].inEvents |= LINUX_POLLFLAG_IN;
      }
      if (kwritefds && LINUX_FD_ISSET(i, kwritefds)) {
         kpollfd[j].inEvents |= LINUX_POLLFLAG_OUT;
      }
      if ((kreadfds && LINUX_FD_ISSET(i, kreadfds)) ||
          (kwritefds && LINUX_FD_ISSET(i, kwritefds))) {
         kpollfd[j].fd = i;
	 j++;
      }
   }
   ASSERT(j == nPollFDs);

   /*
    * Poll the fds
    */
   status = UserObj_Poll(kpollfd, nPollFDs, timeoutMillis, &numReady);

   /*
    * Handle status and set return code
    */
   if (status == VMK_WAIT_INTERRUPTED) {
      rc = LINUX_EINTR;
   } else if (status == VMK_OK) {
      if (kreadfds) {
         LINUX_FD_ZERO(kreadfds);
         for (i = 0; i < nPollFDs; i++) {
            if (kpollfd[i].outEvents & LINUX_POLLFLAG_IN) {
               LINUX_FD_SET(kpollfd[i].fd, kreadfds);
            }
         }
         status = User_CopyOut(readfds, kreadfds, sizeof *kreadfds);
         if (status != VMK_OK) {
            rc = User_TranslateStatus(status);
            goto end;
         }
      }
      if (kwritefds) {
         LINUX_FD_ZERO(kwritefds);
         for (i = 0; i < nPollFDs; i++) {
            if (kpollfd[i].outEvents & LINUX_POLLFLAG_OUT) {
               LINUX_FD_SET(kpollfd[i].fd, kwritefds);
            }
         }
         status = User_CopyOut(writefds, kwritefds, sizeof *kwritefds);
         if (status != VMK_OK) {
            rc = User_TranslateStatus(status);
            goto end;
         }
      }
      rc = numReady;
   } else if (status == VMK_TIMEOUT) {
      ASSERT(numReady == 0);
      rc = 0;
   } else {
      rc = User_TranslateStatus(status);
   }

end:
   if (kreadfds) {
       User_HeapFree(uci, kreadfds);
   }

   if (kwritefds) {
       User_HeapFree(uci, kwritefds);
   }

   if (kpollfd) {
       User_HeapFree(uci, kpollfd);
   }

   return rc;
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Flock - 
 *	Handler for linux syscall 143 
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
LinuxFileDesc_Flock(LinuxFd fd, uint32 how)
{
   UWLOG_SyscallUnimplemented("(%d, %#x)", fd, how);
   return LINUX_ENOSYS;
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Readv - 
 *	Handler for linux syscall 145 
 * Support: 90% (only small vectors)
 * Error case: 100%
 * Results:
 *      Number of bytes read or Linux error code
 *
 * Side effects:
 *      Reads file data into the user's buffer.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Readv(LinuxFd fd, UserVA /* struct LinuxIovec* */ userIovp,
                    uint32 iovcnt)
{
   User_CartelInfo* const uci = MY_USER_CARTEL_INFO;
   uint32 i;
   uint32 totalBytesRead = 0;
   struct LinuxIovec kernIovp[LINUX_MAX_IOVEC];
   VMK_ReturnStatus status;
   uint32 iovSize;
   UserObj* obj;

   UWLOG_SyscallEnter("(%u, %#x, %d)", fd, userIovp, iovcnt);

   if (iovcnt <= 0 || iovcnt > LINUX_MAX_IOVEC) {
      return LINUX_EINVAL;
   }
   iovSize = sizeof(struct LinuxIovec) * iovcnt;
   status = User_CopyIn(kernIovp, userIovp, iovSize);
   if (status != VMK_OK) {
      return User_TranslateStatus(status);
   }

   status = UserObj_Find(uci, fd, &obj);
   if (status != VMK_OK) {
      return User_TranslateStatus(status);
   }
   
   if (!UserObj_IsOpenForRead(obj)) {
      (void) UserObj_Release(uci, obj);
      return User_TranslateStatus(VMK_INVALID_HANDLE);
   }

   Semaphore_Lock(&obj->sema);

   for (i = 0; i < iovcnt; i++) {
      uint32 bytesRead;

      /*
       * Just skip this iovec if its length is 0.
       */
      if (kernIovp[i].length == 0) {
         continue;
      }

      status = obj->methods->read(obj, kernIovp[i].base, obj->offset,
                                  kernIovp[i].length, &bytesRead);
      if (status != VMK_OK) {
         // Stop on error
         break;
      }
      obj->offset += bytesRead;
      totalBytesRead += bytesRead;
      if (bytesRead < kernIovp[i].length) {
         // Stop on end of file
         break;
      }
   }

   Semaphore_Unlock(&obj->sema);
   
   (void) UserObj_Release(uci, obj);

   if (status == VMK_OK) {
      return totalBytesRead;
   }
   return User_TranslateStatus(status);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Writev - 
 *	Handler for linux syscall 146 
 * Support: 90% (Only small vectors)
 * Error case: 100%
 * Results:
 *      Number of bytes written or Linux error code
 *
 * Side effects:
 *      Writes file data from the user's buffer.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Writev(LinuxFd fd, UserVA /* struct LinuxIovec* */ userIovp,
                     uint32 iovcnt)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   uint32 i;
   uint32 totalBytesWritten = 0;
   struct LinuxIovec kernIovp[LINUX_MAX_IOVEC]; /* XXX */
   VMK_ReturnStatus status;
   uint32 iovSize;
   UserObj* obj;

   UWLOG_SyscallEnter("(%u, %#x, %d)", fd, userIovp, iovcnt);
   UWSTAT_INSERT(writevSizes, iovcnt);

   if (iovcnt <= 0 || iovcnt > LINUX_MAX_IOVEC) {
      return LINUX_EINVAL;
   }
   iovSize = sizeof(struct LinuxIovec) * iovcnt;
   status = User_CopyIn(kernIovp, userIovp, iovSize);
   if (status != VMK_OK) {
      return User_TranslateStatus(status);
   }

   status = UserObj_Find(uci, fd, &obj);
   if (status != VMK_OK) {
      return User_TranslateStatus(status);
   }
   
   if (!UserObj_IsOpenForWrite(obj)) {
      (void) UserObj_Release(uci, obj);
      return User_TranslateStatus(VMK_INVALID_HANDLE);
   }

   Semaphore_Lock(&obj->sema);

   for (i = 0; i < iovcnt; i++) {
      uint32 bytesWritten;

      /*
       * Just skip this iovec if its length is 0.
       */
      if (kernIovp[i].length == 0) {
         continue;
      }

      status = obj->methods->write(obj, kernIovp[i].base, obj->offset,
                                   kernIovp[i].length, &bytesWritten);
      if (status != VMK_OK) {
         // Stop on error
         break;
      }
      obj->offset += bytesWritten;
      totalBytesWritten += bytesWritten;
      if (bytesWritten < kernIovp[i].length) {
         // Stop on end of file
         break;
      }
   }

   Semaphore_Unlock(&obj->sema);
   
   (void) UserObj_Release(uci, obj);

   if (status == VMK_OK) {
      return totalBytesWritten;
   }
   return User_TranslateStatus(status);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Fdatasync - 
 *	Handler for linux syscall 148 
 * Support: 100%
 * Error case: 90%
 * Results:
 *      0 or Linux error code.
 *
 * Side effects:
 *      Force buffered writes on fd's data to disk, but not its metadata.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Fdatasync(LinuxFd fd)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   VMK_ReturnStatus status;
   UserObj* obj;

   UWLOG_SyscallEnter("(fd=%d)", fd);

   status = UserObj_Find(uci, fd, &obj);
   if (status != VMK_OK) {
      return User_TranslateStatus(status);
   }
   Semaphore_Lock(&obj->sema);
   status = obj->methods->fsync(obj, TRUE);
   Semaphore_Unlock(&obj->sema);
   (void) UserObj_Release(uci, obj);
   return User_TranslateStatus(status);
}

/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Poll - 
 *	Handler for linux syscall 168 
 * Support: 60% (only some fd types, nfds==0 handling is different)
 * Error case: 100%
 * Results:
 *      0 or Linux error code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Poll(UserVA userPollFds, uint32 nfds, int32 timeoutMillis)
{
   User_CartelInfo* uci = MY_USER_CARTEL_INFO;
   LinuxPollfd *kpfd;
   int rc = 0;
   int numReady = 0;
   VMK_ReturnStatus status;

   UWLOG_SyscallEnter("(fds@%#x, nfds=%u, timeout=%d)",
                      userPollFds, nfds, timeoutMillis);

   if (nfds > USEROBJ_MAX_HANDLES) {
      return LINUX_ENOMEM;
   }
   kpfd = (LinuxPollfd *)User_HeapAlloc(uci, nfds*sizeof kpfd[0]);
   if (kpfd == NULL) {
      return LINUX_ENOMEM;
   }
   if (nfds > 0) {
      if (User_CopyIn(kpfd, userPollFds, nfds * sizeof kpfd[0]) !=
          VMK_OK) {
         User_HeapFree(uci, kpfd);
         return LINUX_EFAULT;
      }
   }
   status = UserObj_Poll(kpfd, nfds, timeoutMillis, &numReady);
   if (status == VMK_WAIT_INTERRUPTED) {
      rc = LINUX_EINTR;
   } else if (status == VMK_OK) {
      rc = numReady;
   } else if (status == VMK_TIMEOUT) {
      ASSERT(numReady == 0);
      rc = 0;
   } else {
      rc = User_TranslateStatus(status);
   }

   if (nfds > 0) {
      status = User_CopyOut(userPollFds, kpfd, nfds * sizeof kpfd[0]);
      if (status != VMK_OK) {
         rc = User_TranslateStatus(status);
      }
   }

   User_HeapFree(uci, kpfd);

   return rc;
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Pread - 
 *	Handler for linux syscall 180 
 * Support: 100%
 * Error case: 100%
 * Results:
 *      Number of bytes read or Linux error code
 *
 *	Note, glibc passes us a 64-bit offset in two 32-bit chunks based
 *	on the processor's endianness.  Since we're little endian, we get
 *	the lower order	bits first.
 *
 * Side effects:
 *      Reads file data into the user's buffer.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Pread(LinuxFd fd,
                    UserVA /* uint8* */ userBuf,
                    uint32 nbyte,
                    int32 olow, int32 ohigh)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   VMK_ReturnStatus status;
   int64 offset = ((int64) ohigh << 32) | olow;
   uint32 bytesRead = 0;
   UserObj* obj;

   UWLOG_SyscallEnter("(fd=%d, userBuf@%#x, nbyte=%d, offset=%Ld)",
                      fd, userBuf, nbyte, offset);
   ASSERT(uci != NULL);

   if (nbyte > LINUX_SSIZE_MAX) {
      UWLOG(0, "nbyte (%u) > LINUX_SSIZE_MAX (%d)!", nbyte, LINUX_SSIZE_MAX);
      return LINUX_EINVAL;
   }

   if (offset < 0) {
      return LINUX_EINVAL;
   }

   status = UserObj_Find(uci, fd, &obj);
   if (status != VMK_OK) {
      return User_TranslateStatus(status);
   }
   
   if (!UserObj_IsOpenForRead(obj)) {
      (void) UserObj_Release(uci, obj);
      status = VMK_INVALID_HANDLE;
      return User_TranslateStatus(status);
   }

   if (nbyte == 0) {
      (void) UserObj_Release(uci, obj);
      return 0;
   }

   Semaphore_Lock(&obj->sema);
   status = obj->methods->read(obj, userBuf, offset, nbyte, &bytesRead);
   Semaphore_Unlock(&obj->sema);
   
   (void) UserObj_Release(uci, obj);

   if (status == VMK_OK) {
      return bytesRead;
   }

   return User_TranslateStatus(status);
}

/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Pwrite - 
 *	Handler for linux syscall 181 
 * Support: 100%
 * Error case: 100%
 * Results:
 *      Number of bytes written or Linux error code
 *
 * Side effects:
 *      Writes file data from the user's buffer.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Pwrite(LinuxFd fd,
                     UserVAConst /* const uint8* */ userBuf,
                     uint32 nbyte,
                     int32 olow, int32 ohigh)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   VMK_ReturnStatus status;
   int64 offset = ((int64) ohigh << 32) | olow;
   uint32 bytesWritten = 0;
   UserObj* obj;

   UWLOG_SyscallEnter("(fd=%d, userBuf@%#x, nbyte=%d, offset=%Ld)",
                      fd, userBuf, nbyte, offset);
   ASSERT(uci != NULL);

   if (nbyte > LINUX_SSIZE_MAX) {
      UWLOG(0, "nbyte (%u) > LINUX_SSIZE_MAX (%d)!", nbyte, LINUX_SSIZE_MAX);
      return LINUX_EINVAL;
   }

   if (offset < 0) {
      return LINUX_EINVAL;
   }

   status = UserObj_Find(uci, fd, &obj);
   if (status != VMK_OK) {
      return User_TranslateStatus(status);
   }
   
   if (!UserObj_IsOpenForWrite(obj)) {
      (void) UserObj_Release(uci, obj);
      status = VMK_INVALID_HANDLE;
      return User_TranslateStatus(status);
   }

   if (nbyte == 0) {
      (void) UserObj_Release(uci, obj);
      return 0;
   }

   Semaphore_Lock(&obj->sema);
   status = obj->methods->write(obj, userBuf, offset, nbyte, &bytesWritten);
   Semaphore_Unlock(&obj->sema);
   
   (void) UserObj_Release(uci, obj);

   if (status == VMK_OK) {
      return bytesWritten;
   }

   return User_TranslateStatus(status);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Stat64 - 
 *	Handler for linux syscall 195 
 * Support: 95%
 *      For certain file types, some fields are placeholders, though
 *      we don't expect this to cause any problems for VMX or any other
 *      program we've thought about running in a userworld.  The fields
 *      that are most often placeholders are st_ino, st_ino32, st_dev,
 *      and st_rdev.
 *      - For COS files and "/", all fields are correct.
 *      - "/vmfs" mostly matches the COS /vmfs; see UserFileStatVMFSRoot.
 *      - Each "/vmfs/xxx" mostly matches the COS; see UserFileStatVMFS.
 *      - VMFS files match the COS almost exactly; see UserFileStat.
 * Error case: 100%
 * Results:
 *      0 or Linux error code
 *
 * Side effects:
 *      Copies stat information to user's statbuf.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Stat64(UserVAConst /* const char* */ userPath,
                     UserVA /* LinuxStat64* */ statbuf)
{
   User_CartelInfo* const uci = MY_USER_CARTEL_INFO;
   char* vmkPath;
   int res;
   VMK_ReturnStatus status;
   UserObj* obj;
   LinuxStat64 vmkStatbuf;

   ASSERT(uci != NULL);

   res = LinuxFileDescAllocAndCopyPath(uci, userPath, &vmkPath);
   if (res != 0) {
      return res;
   }

   UWLOG_SyscallEnter("(path=%s statbuf=%#x)", vmkPath, statbuf);

   status = UserObj_Open(uci, vmkPath, USEROBJ_OPEN_STAT, 0, &obj);
   if (status == VMK_OK) {
      status = obj->methods->stat(obj, &vmkStatbuf);
      (void) UserObj_Release(uci, obj);
      if (status == VMK_OK) {
         status = User_CopyOut(statbuf, &vmkStatbuf, sizeof(LinuxStat64));
      }
   }

   LinuxFileDescFreePath(uci, vmkPath);

   return User_TranslateStatus(status);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Lstat64 - 
 *	Handler for linux syscall 196 
 * Support: 90% (for links, only st_mode is filled in; for others,
        see LinuxFileDesc_Stat64)
 * Error case: 100%
 * Results:
 *      0 or Linux error code
 *
 * Side effects:
 *      Copies stat information to user's statbuf.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Lstat64(UserVAConst /* const char* */ userPath,
                      UserVA /* LinuxStat64* */ statbuf)
{
   User_CartelInfo* const uci = MY_USER_CARTEL_INFO;
   char* vmkPath;
   int res;
   VMK_ReturnStatus status;
   UserObj* obj;
   LinuxStat64 vmkStatbuf;
   char arc[LINUX_ARC_MAX+1];

   ASSERT(uci != NULL);

   res = LinuxFileDescAllocAndCopyPath(uci, userPath, &vmkPath);
   if (res != 0) {
      return res;
   }

   UWLOG_SyscallEnter("(path=%s statbuf=%#x)", vmkPath, statbuf);

   status = UserObj_TraversePath(uci, vmkPath,
                                 USEROBJ_OPEN_STAT|USEROBJ_OPEN_NOFOLLOW, 0,
                                 &obj, arc, sizeof arc);

   LinuxFileDescFreePath(uci, vmkPath);

   if (status == VMK_IS_A_SYMLINK) {
      // Fake up stat info for a symlink here.
      memset(&vmkStatbuf, 0, sizeof(vmkStatbuf));
      vmkStatbuf.st_mode = LINUX_MODE_IFLNK;
      status = VMK_OK;
   } else if (status == VMK_OK) {
      if (*arc) {
         // Named object doesn't exist, but its parent does.  We
         // aren't interested in this case, so turn it back into
         // ENOENT.
         status = VMK_NOT_FOUND;
      } else {
         status = obj->methods->stat(obj, &vmkStatbuf);
      }
      (void) UserObj_Release(uci, obj);
   }

   if (status == VMK_OK) {
      status = User_CopyOut(statbuf, &vmkStatbuf, sizeof(LinuxStat64));
   }

   return User_TranslateStatus(status);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Fstat64 - 
 *	Handler for linux syscall 197 
 * Support: 95% (see LinuxFileDesc_Stat64)
 * Error case: 100%
 * Results:
 *      0 or Linux error code
 *
 * Side effects:
 *      Copies stat information to user's statbuf.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Fstat64(LinuxFd fd, UserVA /* LinuxStat64* */ statbuf)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   VMK_ReturnStatus status;
   UserObj* obj;
   LinuxStat64 vmkStatbuf;

   UWLOG_SyscallEnter("(fd=%d, statbuf=%#x)", fd, statbuf);

   ASSERT(uci != NULL);

   status = UserObj_Find(uci, fd, &obj);
   if (status != VMK_OK) {
      return User_TranslateStatus(status);
   }

   status = obj->methods->stat(obj, &vmkStatbuf);
   (void) UserObj_Release(uci, obj);
   if (status != VMK_OK) {
      return User_TranslateStatus(status);
   }

   status = User_CopyOut(statbuf, &vmkStatbuf, sizeof(LinuxStat64));

   return User_TranslateStatus(status);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Fcntl64 - 
 *	Handler for linux syscall 221 
 * Support: 10% (minimal getfd, setfd; getfl; partial setfl; full dupfd)
 * Error case: 100%
 * Results:
 *      0 or Linux error code
 *
 * Side effects:
 *      Depends on cmd.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Fcntl64(LinuxFd fd, uint32 cmd, uint32 arg)
{
   VMK_ReturnStatus status;
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   UserObj* obj;
   int newfd;
   uint32 flags;

   UWLOG_SyscallEnter("(%u, %u, %#x)", fd, cmd, arg);
   ASSERT(uci != NULL);

   switch (cmd) {
   case LINUX_FCNTL_CMD_GETFD:
      /* get close-on-exec flag */
      return 0x7fffffff;  //XXX ??

   case LINUX_FCNTL_CMD_SETFD:
      /* set close-on-exec flag */
      UWLOG(1, "F_SETFD ignored");
      return 0;

   case LINUX_FCNTL_CMD_DUPFD:
      status = UserObj_FDDup(uci, fd, arg, &newfd);
      return User_TranslateStatus(status);

   case LINUX_FCNTL_CMD_GETFL:
      status = UserObj_Find(uci, fd, &obj);
      if (status != VMK_OK) {
         return User_TranslateStatus(status);
      }

      flags = obj->openFlags;
      (void) UserObj_Release(uci, obj);

      return flags;

   case LINUX_FCNTL_CMD_SETFL:
      status = UserObj_Find(uci, fd, &obj);
      if (status != VMK_OK) {
         return User_TranslateStatus(status);
      }

      /*
       * Silently zero bits not supported by Linux.
       */
      arg &= USEROBJ_FCNTL_SETFL_LINUX_SUPPORTED;

      /*
       * Now we should zero bits not supported by us.  However, give an error if
       * any of the bits we don't support but Linux does are on.
       */
      if ((arg & ~USEROBJ_FCNTL_SETFL_VMK_SUPPORTED) != 0) {
         UWWarn("Trying to change unsupported flags!");
         UWLOG_StackTraceCurrent(1);
      }
      arg &= USEROBJ_FCNTL_SETFL_VMK_SUPPORTED;

      status = obj->methods->fcntl(obj, cmd, arg);
      if (status == VMK_OK) {
        /*
	 * First clear out the old values of these flags.
	 */
        obj->openFlags &= ~USEROBJ_FCNTL_SETFL_LINUX_SUPPORTED;
	/*
	 * Then set them to the new ones.
	 */
	obj->openFlags |= arg;
      }

      (void)UserObj_Release(uci, obj);
      return User_TranslateStatus(status);

   default:
      break;
   }

   UWWarn("UNIMPLEMENTED for cmd (fd=%u, cmd=%u, arg=%#x)", fd, cmd, arg);
   UWLOG_StackTraceCurrent(1);
   return LINUX_ENOSYS;
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Lseek - 
 *	Handler for linux syscall 19 
 * Support: 100%
 * Error case: 95%
 *      Seeking on a proxied fifo or tty is a no-op but should be ESPIPE.
 * Results:
 *      File position after the seek, or Linux error code.
 *
 * Side effects:
 *      Modifies file seek pointer.
 *
 *----------------------------------------------------------------------
 */
int32
LinuxFileDesc_Lseek(LinuxFd fd, int32 offset, int32 whence)
{
   uint64 res;
   VMK_ReturnStatus status;
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;

   UWLOG_SyscallEnter("(%d %d %d)", fd, offset, whence);
   ASSERT(uci != NULL);

   status = UserObj_FDSeek(uci, fd, offset, whence, &res);

   if (status != VMK_OK) {
      return User_TranslateStatus(status);      
   }

   return (int32) res;
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Llseek - 
 *	Handler for linux syscall 140 
 * Support: 100%
 * Error case: 95%
 *      Seeking on a proxied fifo or tty is a no-op but should be ESPIPE.
 * Results:
 *      File position after the seek, or Linux error code.
 *
 * Side effects:
 *      Modifies file seek pointer.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Llseek(LinuxFd fd, uint32 ohigh, uint32 olow,
                     UserVA /* uint64* */ userRes, uint32 whence)
{
   int64 offset = ((int64) ohigh << 32) | olow;
   uint64 res;
   VMK_ReturnStatus status;
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;

   UWLOG_SyscallEnter("(fd=%d offset=%Ld whence=%s result@%#x )",
                      fd, offset, ((whence==0)?"set":
                                   ((whence==1)?"cur":
                                    ((whence==2)?"end":"ERR"))),
                      userRes);
   ASSERT(uci != NULL);

   status = UserObj_FDSeek(uci, fd, offset, whence, &res);

   if (status == VMK_OK && userRes != 0) {
      status = User_CopyOut(userRes, &res, sizeof res);
   }

   return User_TranslateStatus(status);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Chdir - 
 *	Handler for linux syscall 12 
 * Support: 100%
 * Error case: 100%
 * Results:
 *      0 or Linux error code
 *
 * Side effects:
 *      Changes cartel's working directory.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Chdir(UserVAConst /* const char * */ userPath)
{
   User_CartelInfo* const uci = MY_USER_CARTEL_INFO;
   char* vmkPath;
   int res;
   VMK_ReturnStatus status;
   UserObj* obj;

   ASSERT(uci != NULL);

   res = LinuxFileDescAllocAndCopyPath(uci, userPath, &vmkPath);
   if (res != 0) {
      return res;
   }

   UWLOG_SyscallEnter("(path=%s)", vmkPath);

   status = UserObj_Open(uci, vmkPath, USEROBJ_OPEN_STAT, 0, &obj);
   if (status == VMK_OK) {
      status = UserObj_Chdir(uci, obj);
   }

   LinuxFileDescFreePath(uci, vmkPath);

   return User_TranslateStatus(status);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Fchdir - 
 *	Handler for linux syscall 133 
 * Support: 100%
 * Error case: 100%
 * Results:
 *      0 or Linux error code
 *
 * Side effects:
 *      Changes cartel's working directory.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Fchdir(LinuxFd fd)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   VMK_ReturnStatus status;
   UserObj* obj;

   UWLOG_SyscallEnter("(fd=%d)", fd);
   ASSERT(uci != NULL);

   status = UserObj_Find(uci, fd, &obj);
   if (status == VMK_OK) {
      status = UserObj_Chdir(uci, obj);
   }
   return User_TranslateStatus(status);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Getcwd - 
 *	Handler for linux syscall 183 
 * Support: 100%
 * Error case: 100%
 * Results:
 *      0 or Linux error code
 *
 * Side effects:
 *      Returns pathname of the cartel's working directory.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Getcwd(UserVA /* char* */ buf,
                     uint32 bufsize)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   char* vmkBuf;
   char* vmkResult;
   VMK_ReturnStatus status;
   UserObj* cwd;
   uint32 size = 0;

   UWLOG_SyscallEnter("(buf=%#x, bufsize=%u)", buf, bufsize);

   ASSERT(uci != NULL);

   if (bufsize == 0) {
      return LINUX_ERANGE;
   } else if (bufsize > LINUX_PATH_MAX) {
      // Trim ginormous buffer size to the most we can handle
      bufsize = LINUX_PATH_MAX;
   }

   vmkBuf = User_HeapAlloc(uci, bufsize);
   if (vmkBuf == NULL) {
      return LINUX_ENOMEM;
   }

   cwd = UserObj_AcquireCwd(uci);
   status = UserObj_GetDirName(uci, cwd, vmkBuf, bufsize, &vmkResult);
   (void) UserObj_Release(uci, cwd);

   if (status == VMK_OK) {
      size = bufsize - (vmkResult - vmkBuf);
      UWLOG(2, "result=\"%s\", size=%d", vmkResult, size);
      status = User_CopyOut(buf, vmkResult, size);
   }
   User_HeapFree(uci, vmkBuf);

   if (status == VMK_OK) {
      return size;
   } else {
      return User_TranslateStatus(status);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Chmod - 
 *	Handler for linux syscall 15 
 * Support: 100%
 * Error case: 100%
 * Results:
 *      0 or Linux error code
 *
 * Side effects:
 *      Changes file mode bits.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Chmod(UserVAConst /* const char* */ userPath,
                    LinuxMode mode)
{
   User_CartelInfo* const uci = MY_USER_CARTEL_INFO;
   char* vmkPath;
   int res;
   VMK_ReturnStatus status;
   UserObj* obj;

   ASSERT(uci != NULL);

   res = LinuxFileDescAllocAndCopyPath(uci, userPath, &vmkPath);
   if (res != 0) {
      return res;
   }

   UWLOG_SyscallEnter("(path=%s, mode=%#x)", vmkPath, mode);

   status = UserObj_Open(uci, vmkPath, USEROBJ_OPEN_OWNER, 0, &obj);

   LinuxFileDescFreePath(uci, vmkPath);

   if (status == VMK_OK) {
      status = obj->methods->chmod(obj, mode);
      (void) UserObj_Release(uci, obj);
   }

   return User_TranslateStatus(status);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Fchmod - 
 *	Handler for linux syscall 94 
 * Support: 90% 
 * Error case: 100%
 * Results:
 *      0 or Linux error code
 *
 * Side effects:
 *      Changes file mode bits.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Fchmod(LinuxFd fd, LinuxMode mode)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   VMK_ReturnStatus status;
   UserObj* obj;

   UWLOG_SyscallEnter("(fd=%d, mode=%#x)", fd, mode);
   ASSERT(uci != NULL);

   status = UserObj_Find(uci, fd, &obj);
   if (status == VMK_OK) {
      status = obj->methods->chmod(obj, mode);
      (void) UserObj_Release(uci, obj);
   }
   return User_TranslateStatus(status);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Fchown - 
 *	Handler for linux syscall 95 
 * Support: 100%
 * Error case: 100%
 * Results:
 *      0 or Linux error code
 *
 * Side effects:
 *      Changes file owner and group.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Fchown(LinuxFd fd, LinuxUID uid, LinuxGID gid)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   VMK_ReturnStatus status;
   UserObj* obj;

   UWLOG_SyscallEnter("(fd=%d, uid=%u, gid=%u)", fd, uid, gid);
   ASSERT(uci != NULL);

   status = UserObj_Find(uci, fd, &obj);
   if (status == VMK_OK) {
      status = obj->methods->chown(obj, uid, gid);
      (void) UserObj_Release(uci, obj);   
   }
   return User_TranslateStatus(status);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Lchown - 
 *	Handler for linux syscall 198 
 * Support: 0% (linux doesn't support it either)
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
LinuxFileDesc_Lchown(UserVAConst /* const char* */ path,
                     LinuxUID uid,
                     LinuxGID gid)
{
   // Judging from its man page, lchown appears to be a historical
   // Linux mistake, so we probably don't need to implement it.
   UWLOG_SyscallUnimplemented("(path@%#x, uid=%u, gid=%u)", path, uid, gid);
   return LINUX_ENOSYS;
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Chown - 
 *	Handler for linux syscall 212 
 * Support: 100%
 * Error case: 100%
 * Results:
 *      0 or Linux error code
 *
 * Side effects:
 *      Changes file owner and group.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Chown(UserVAConst /* const char* */ userPath,
                    LinuxUID uid,
                    LinuxGID gid)
{
   User_CartelInfo* const uci = MY_USER_CARTEL_INFO;
   char* vmkPath;
   int res;
   VMK_ReturnStatus status;
   UserObj* obj;

   ASSERT(uci != NULL);

   res = LinuxFileDescAllocAndCopyPath(uci, userPath, &vmkPath);
   if (res != 0) {
      return res;
   }

   UWLOG_SyscallEnter("(path=%s, uid=%u, gid=%u)", vmkPath, uid, gid);

   status = UserObj_Open(uci, vmkPath, USEROBJ_OPEN_OWNER, 0, &obj);

   LinuxFileDescFreePath(uci, vmkPath);

   if (status == VMK_OK) {
      status = obj->methods->chown(obj, uid, gid);
      (void) UserObj_Release(uci, obj);
   }
   return User_TranslateStatus(status);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Truncate - 
 *	Handler for linux syscall 92 
 * Support: 100%
 * Error case: 100%
 * Results:
 *      0 or Linux error code
 *
 * Side effects:
 *      Change the length of a file.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Truncate(UserVAConst /* const char* */ userPath,
                       int32 length)
{
   UWLOG_SyscallEnter("(path=..., length=%d) -> truncate64()", length);

   if (length < 0) {
      return LINUX_EINVAL;
   }

   return LinuxFileDesc_Truncate64(userPath, length, 0);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Ftruncate - 
 *	Handler for linux syscall 93 
 * Support: 100%
 * Error case: 100%
 * Results:
 *      0 or Linux error code
 *
 * Side effects:
 *      Change the length of a file.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Ftruncate(LinuxFd fd, int32 length)
{
   UWLOG_SyscallEnter("(fd=%d, length=%d) -> ftruncate64()", fd, length);

   if (length < 0) {
      return LINUX_EINVAL;
   }

   return LinuxFileDesc_Ftruncate64(fd, length, 0);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDescStatfsCommon - 
 *
 *      Common tail for statfs and fstatfs
 *
 * Results:
 *      0 or Linux error code
 *
 * Side effects:
 *      Copies statfs information to user's buffer.  Releases obj.
 *
 *----------------------------------------------------------------------
 */
static int
LinuxFileDescStatfsCommon(UserObj *obj, UserVA /* LinuxStatFS* */ userBuf)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   VMK_ReturnStatus status;
   LinuxStatFS64 vmkBuf64;
   LinuxStatFS vmkBuf32;
   int i;

   status = obj->methods->statFS(obj, &vmkBuf64);
   (void) UserObj_Release(uci, obj);
   if (status != VMK_OK) {
      return User_TranslateStatus(status);
   }

   memset(&vmkBuf32, 0, sizeof(vmkBuf32));
   vmkBuf32.f_type = vmkBuf64.f_type;
   vmkBuf32.f_bsize = vmkBuf64.f_bsize;
   vmkBuf32.f_blocks = vmkBuf64.f_blocks;
   vmkBuf32.f_bfree = vmkBuf64.f_bfree;
   vmkBuf32.f_bavail = vmkBuf64.f_bavail;
   vmkBuf32.f_files = vmkBuf64.f_files;
   vmkBuf32.f_ffree = vmkBuf64.f_ffree;
   vmkBuf32.f_fsid = vmkBuf64.f_fsid;
   vmkBuf32.f_namelen = vmkBuf64.f_namelen;
   for (i = 0; i < ARRAYSIZE(vmkBuf32.f_spare); i++) {
      vmkBuf32.f_spare[i] = vmkBuf64.f_spare[i];
   }

   status = User_CopyOut(userBuf, &vmkBuf32, sizeof(vmkBuf32));

   return User_TranslateStatus(status);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Statfs - 
 *	Handler for linux syscall 99 
 * Support: 100%
 * Error case: 100%
 *
 * Results:
 *      0 or Linux error code
 *
 * Side effects:
 *      Copies statfs information to user's buffer.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Statfs(UserVAConst /* const char*  */ userPath,
                     UserVA /* LinuxStatFS* */ userBuf)
{
   User_CartelInfo* const uci = MY_USER_CARTEL_INFO;
   char* vmkPath;
   int res;
   VMK_ReturnStatus status;
   UserObj* obj;

   res = LinuxFileDescAllocAndCopyPath(uci, userPath, &vmkPath);
   if (res != 0) {
      return res;
   }

   UWLOG_SyscallEnter("(path=%s, userBuf=%#x)", vmkPath, userBuf);

   status = UserObj_Open(uci, vmkPath, USEROBJ_OPEN_STAT, 0, &obj);

   LinuxFileDescFreePath(uci, vmkPath);

   if (status != VMK_OK) {
      return User_TranslateStatus(status);
   }

   return LinuxFileDescStatfsCommon(obj, userBuf);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Fstatfs - 
 *	Handler for linux syscall 100 
 * Support: 100%
 * Error case: 99% (pipes, sockets, etc. return ENOSYS)
 *
 * Results:
 *      0 or Linux error code
 *
 * Side effects:
 *      Copies statfs information to user's buffer.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Fstatfs(LinuxFd fd,
                      UserVA /* LinuxStatFS* */ userBuf)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   VMK_ReturnStatus status;
   UserObj* obj;

   UWLOG_SyscallEnter("(fd=%d, userBuf=%#x)", fd, userBuf);

   status = UserObj_Find(uci, fd, &obj);
   if (status != VMK_OK) {
      return User_TranslateStatus(status);
   }

   return LinuxFileDescStatfsCommon(obj, userBuf);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Truncate64 - 
 *	Handler for linux syscall 193 
 * Support: 100%
 * Error case: 100%
 * Results:
 *      0 or Linux error code
 *
 * Side effects:
 *      Change the length of a file.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Truncate64(UserVAConst /* const char* */ userPath,
                         uint32 llow,
                         int32 lhigh)
{
   int64 length = ((int64)lhigh << 32) | llow;
   User_CartelInfo* const uci = MY_USER_CARTEL_INFO;
   char* vmkPath;
   int res;
   VMK_ReturnStatus status;
   UserObj* obj;

   ASSERT(uci != NULL);

   if (length < 0) {
      return LINUX_EINVAL;
   }

   res = LinuxFileDescAllocAndCopyPath(uci, userPath, &vmkPath);
   if (res != 0) {
      return res;
   }

   UWLOG_SyscallEnter("(path=%s, length=%Ld)", vmkPath, length);

   status = UserObj_Open(uci, vmkPath, USEROBJ_OPEN_WRONLY, 0, &obj);

   LinuxFileDescFreePath(uci, vmkPath);

   if (status == VMK_OK) {
      Semaphore_Lock(&obj->sema);
      status = obj->methods->truncate(obj, length);
      Semaphore_Unlock(&obj->sema);
      (void) UserObj_Release(uci, obj);
   }

   return User_TranslateStatus(status);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Ftruncate64 - 
 *	Handler for linux syscall 194 
 * Support: 100%
 * Error case: 100%
 * Results:
 *      0 or Linux error code
 *
 * Side effects:
 *      Change the length of a file.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Ftruncate64(LinuxFd fd,
                          uint32 llow,
                          int32 lhigh)
{
   int64 length = ((int64)lhigh << 32) | llow;
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   VMK_ReturnStatus status;
   UserObj* obj;

   UWLOG_SyscallEnter("(fd=%d, length=%Ld)", fd, length);
   ASSERT(uci != NULL);

   if (length < 0) {
      return LINUX_EINVAL;
   }

   status = UserObj_Find(uci, fd, &obj);
   if (status != VMK_OK) {
      return User_TranslateStatus(status);
   }
   if (!UserObj_IsOpenForWrite(obj)) {
      (void) UserObj_Release(uci, obj);
      return LINUX_EBADF;
   }
   Semaphore_Lock(&obj->sema);
   status = obj->methods->truncate(obj, length);
   Semaphore_Unlock(&obj->sema);
   (void) UserObj_Release(uci, obj);

   return User_TranslateStatus(status);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Utime - 
 *	Handler for linux syscall 30 
 * Support: 80%
 * Error case: 100%
 * Results:
 *      0 or Linux error code
 *
 * Side effects:
 *      Change a file's mtime and atime.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Utime(UserVAConst /* const char* */ userPath,
                    UserVA userTimeBuf)
{
   User_CartelInfo* const uci = MY_USER_CARTEL_INFO;
   char* vmkPath;
   struct { int32 atime; int32 mtime; } vmkTimeBuf;
   int res;
   VMK_ReturnStatus status;
   UserObj* obj;

   ASSERT(uci != NULL);

   if (userTimeBuf != 0) {
      status = User_CopyIn(&vmkTimeBuf, userTimeBuf, sizeof(vmkTimeBuf));
      if (status != VMK_OK) {
         return User_TranslateStatus(status);
      }
   } else {
      vmkTimeBuf.atime = vmkTimeBuf.mtime = Timer_GetTimeOfDay() / 1000000;
   }

   res = LinuxFileDescAllocAndCopyPath(uci, userPath, &vmkPath);
   if (res != 0) {
      return res;
   }

   UWLOG_SyscallEnter("(path=%s, atime=%d, mtime=%d)",
                      vmkPath, vmkTimeBuf.atime, vmkTimeBuf.mtime);

   status = UserObj_Open(uci, vmkPath, USEROBJ_OPEN_WRONLY, 0, &obj);
   if (status == VMK_OK) {
      status = obj->methods->utime(obj, vmkTimeBuf.atime, vmkTimeBuf.mtime);
      (void) UserObj_Release(uci, obj);
   }

   LinuxFileDescFreePath(uci, vmkPath);

   return User_TranslateStatus(status);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Getdents64 - 
 *	Handler for linux syscall 220 
 * Support: 100%
 * Error case: 100%
 *
 * Results:
 *      Number of bytes read or Linux error code.
 *
 * Side effects:
 *      Up to nbyte bytes worth of directory entries are read and
 *      copied to the user buffer.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Getdents64(LinuxFd fd,
                         UserVA /* LinuxDirent64* */ userBuf,
                         uint32 nbyte)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   VMK_ReturnStatus status;
   uint32 bytesRead = 0;
   UserObj* obj;

   UWLOG_SyscallEnter("(fd=%u, buf=0x%x, nbyte=%d)", fd, userBuf, nbyte);

   if (nbyte == 0) {
      return LINUX_EINVAL;
   }

   status = UserObj_Find(uci, fd, &obj);
   if (status != VMK_OK) {
      return User_TranslateStatus(status);
   }
   
   if (!UserObj_IsOpenForRead(obj)) {
      (void) UserObj_Release(uci, obj);
      status = VMK_INVALID_HANDLE;
      return User_TranslateStatus(status);
   }

   Semaphore_Lock(&obj->sema);
   status = obj->methods->readDir(obj, userBuf, nbyte, &bytesRead);
   Semaphore_Unlock(&obj->sema);
   
   (void) UserObj_Release(uci, obj);

   if (status == VMK_OK) {
      return bytesRead;
   }

   return User_TranslateStatus(status);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Statfs64 - 
 *	Handler for linux syscall 268
 * Support: 100%
 * Error case: 100%
 *
 * Results:
 *      0 or Linux error code
 *
 * Side effects:
 *      Copies statfs information to user's buffer.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Statfs64(UserVAConst /* const char*  */ userPath,
                       UserVA /* LinuxStatFS64* */ userBuf)
{
   User_CartelInfo* const uci = MY_USER_CARTEL_INFO;
   char* vmkPath;
   int res;
   VMK_ReturnStatus status;
   UserObj* obj;
   LinuxStatFS64 vmkBuf;

   res = LinuxFileDescAllocAndCopyPath(uci, userPath, &vmkPath);
   if (res != 0) {
      return res;
   }

   UWLOG_SyscallEnter("(path=%s, userBuf=%#x)", vmkPath, userBuf);

   status = UserObj_Open(uci, vmkPath, USEROBJ_OPEN_STAT, 0, &obj);

   LinuxFileDescFreePath(uci, vmkPath);

   if (status != VMK_OK) {
      return User_TranslateStatus(status);
   }

   status = obj->methods->statFS(obj, &vmkBuf);
   (void) UserObj_Release(uci, obj);
   if (status != VMK_OK) {
      return User_TranslateStatus(status);
   }

   status = User_CopyOut(userBuf, &vmkBuf, sizeof(vmkBuf));

   return User_TranslateStatus(status);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxFileDesc_Fstatfs64 - 
 *	Handler for linux syscall 269
 * Support: 100%
 * Error case: 99% (pipes, sockets, etc. return ENOSYS)
 *
 * Results:
 *      0 or Linux error code
 *
 * Side effects:
 *      Copies statfs information to user's buffer.
 *
 *----------------------------------------------------------------------
 */
int
LinuxFileDesc_Fstatfs64(LinuxFd fd,
                        UserVA /* LinuxStatFS64* */ userBuf)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   VMK_ReturnStatus status;
   UserObj* obj;
   LinuxStatFS64 vmkBuf;

   UWLOG_SyscallEnter("(fd=%d, userBuf=%#x)", fd, userBuf);

   status = UserObj_Find(uci, fd, &obj);
   if (status != VMK_OK) {
      return User_TranslateStatus(status);
   }

   status = obj->methods->statFS(obj, &vmkBuf);
   (void) UserObj_Release(uci, obj);
   if (status != VMK_OK) {
      return User_TranslateStatus(status);
   }

   status = User_CopyOut(userBuf, &vmkBuf, sizeof(vmkBuf));

   return User_TranslateStatus(status);
}
