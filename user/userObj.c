/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

#include "user_int.h"
#include "net.h"
#include "rpc.h"
#include "fsSwitch.h"
#include "userObj.h"
#include "userPipe.h"
#include "vmkpoll.h"
#include "userFile.h"
#include "libc.h"
#include "userStat.h"
#include "userDump.h"
#include "dump_ext.h"

#define LOGLEVEL_MODULE UserObj
#include "userLog.h"

/*
 * Strings for each UserObj type.
 */
#define DEFINE_USEROBJ_TYPE(_name) #_name,
#define DEFINE_USEROBJ_PROXY_TYPE(_name) "PROXY_" #_name,
const char *userObjTypes[] = {
   USEROBJ_TYPES
};
#undef DEFINE_USEROBJ_PROXY_TYPE
#undef DEFINE_USEROBJ_TYPE

static VMK_ReturnStatus
UserObjTraversePath(User_CartelInfo* uci, UserObj* obj, const char* path,
                    uint32 flags, LinuxMode mode,
                    UserObj** objOut, char *arc, int arcLen,
                    unsigned symLinkLimit);

/*
 *----------------------------------------------------------------------
 *
 * UserObjAssertUnused --
 *
 *	ASSERT that obj is not in the current world's fdState.
 *
 * Results:
 *	None
 *
 * Side-effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static inline void
UserObjAssertUnused(UserObj* obj)
{
#ifdef VMX86_DEBUG
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   if (uci) {
      int i;
      UserObj_FDLock(&uci->fdState);
      ASSERT(uci->fdState.cwd != obj);
      for (i = 0; i < USEROBJ_MAX_HANDLES; i++) {
         ASSERT(uci->fdState.descriptors[i] != obj);
      }
      UserObj_FDUnlock(&uci->fdState);
   }
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * UserObj_CartelInit --
 *
 *      Per-cartel initialization of file descriptor state
 *
 * Results:
 *	VMK_OK
 *
 * Side effects:
 *      uci->fdState is initialized
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserObj_CartelInit(User_CartelInfo* uci)
{
   ASSERT(uci != NULL);
   ASSERT(sizeof(UserObj_Data) == sizeof(uint64));

   SP_InitLock("UserObjFD", &uci->fdState.lock, UW_SP_RANK_USEROBJ);

   memset(uci->fdState.descriptors, 0, sizeof uci->fdState.descriptors);
   uci->fdState.cwd =
      UserFile_OpenVMFSRoot(uci, USEROBJ_OPEN_STAT); // safe; doesn't use proxy
   uci->fdState.umask = 0;
   UserFile_CartelInit(uci);

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserObj_CartelCleanup --
 *
 *      Undo UserObj_CartelInit, run UserObj_FDClose() on all objects in the
 *      given thread state.
 *
 * Results:
 *	VMK_OK
 *
 * Side effects:
 *      uci->fdState is cleaned out
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserObj_CartelCleanup(User_CartelInfo* uci)
{
   LinuxFd i;

   ASSERT(uci != NULL);

   UserFile_CartelCleanup(uci);
   for (i = 0; i < USEROBJ_MAX_HANDLES; i++) {
      UserObj_FDClose(uci, i);
   }
   (void) UserObj_Release(uci, uci->fdState.cwd);

   SP_CleanupLock(&uci->fdState.lock);

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserObj_Create --
 *
 *      Create a UserObj.  Initial reference count is 1.  Semaphore is
 *      unlocked.
 *
 * Results:
 *	Pointer to new UserObj.  NULL if no resources.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
UserObj*
UserObj_Create(User_CartelInfo *uci,
               UserObj_Type type,
               UserObj_Data data,
               UserObj_Methods* methods,
               uint32 openFlags)
{
   UserObj *obj = User_HeapAlloc(uci, sizeof(UserObj));

   if (obj != NULL) {
      UserObj_InitObj(obj, type, data, methods, openFlags);
   }
   return obj;
}


/*
 *----------------------------------------------------------------------
 *
 * UserObj_InitObj --
 *
 *      Initialize a UserObj in preallocated memory.  Initial
 *      reference count is 1.  Semaphore is unlocked.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      Fills in object's fields.
 *
 *----------------------------------------------------------------------
 */
void
UserObj_InitObj(UserObj* obj, // IN/OUT
                UserObj_Type type,
                UserObj_Data data,
                UserObj_Methods* methods,
                uint32 openFlags)
{
   /* Shouldn't see flags meant only for TraversePath */
   ASSERT((openFlags &
           (USEROBJ_OPEN_PENULTIMATE
            |USEROBJ_OPEN_IGNTRAILING))
          == 0);

   Semaphore_Init("UserObj", &obj->sema, 1, UW_SEMA_RANK_OBJ);
   Atomic_Write(&obj->refcount, 1);
   obj->offset = 0;
   obj->openFlags = openFlags;
   obj->type = type;
   obj->data = data;
   obj->methods = methods;
   UWSTAT_ARRINC(userObjCreated, type);
}


/*
 *----------------------------------------------------------------------
 *
 * UserObj_Find --
 *
 *      Return the UserObj mapped to the given fd in the given
 *      cartel's state.  Increment the reference count to keep it from
 *      going away.
 *
 * Results:
 *      VMK_INVALID_HANDLE if fd is out of range or not active.
 *	VMK_OK otherwise.
 *      *obj is filled in with a pointer to the object descriptor.
 *
 * Side effects:
 *	*obj's reference count is incremented.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserObj_Find(User_CartelInfo* uci, LinuxFd fd, UserObj **objret)
{
   VMK_ReturnStatus status;
   UserObj* obj;

   ASSERT(uci != NULL);
   ASSERT(objret != NULL);

   if (fd < 0 || fd >= USEROBJ_MAX_HANDLES) {
      UWLOG(1, "Invalid fd %d", fd);
      status = VMK_INVALID_HANDLE;
   } else {
      UserObj_FDLock(&uci->fdState);
      obj = uci->fdState.descriptors[fd];
      if (obj == NULL || obj == USEROBJ_RESERVED_HANDLE) {
         status = VMK_INVALID_HANDLE;
      } else {
         UserObj_Acquire(obj);
         *objret = obj;
         status = VMK_OK;
      }
      UserObj_FDUnlock(&uci->fdState);
   }
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserObj_FdForObj --
 *
 *	Return the file descriptor that this object resides at.
 *
 * Results:
 *	VMK_OK if found, VMK_NOT_FOUND if this object is not in the
 *	descriptor table.
 *
 * Side effects:
 *	*outFd is set to the fd.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserObj_FdForObj(User_CartelInfo *uci, UserObj *obj, LinuxFd *outFd)
{
   VMK_ReturnStatus status = VMK_NOT_FOUND;
   LinuxFd fd;

   ASSERT(uci != NULL);
   ASSERT(outFd != NULL);

   *outFd = USEROBJ_INVALID_HANDLE;

   UserObj_FDLock(&uci->fdState);
   for (fd = 0; fd < USEROBJ_MAX_HANDLES; fd++) {
      if (uci->fdState.descriptors[fd] == obj) {
	 *outFd = fd;
	 status = VMK_OK;
	 break;
      }
   }
   UserObj_FDUnlock(&uci->fdState);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserObj_AcquireCwd --
 *
 *      Return the UserObj for the current working directory of the
 *      given cartel's state.  Increment the reference count to keep
 *      it from going away.
 *
 * Results:
 *      Pointer to the object descriptor.
 *
 * Side effects:
 *	obj's reference count is incremented.
 *
 *----------------------------------------------------------------------
 */

UserObj* 
UserObj_AcquireCwd(struct User_CartelInfo* uci)
{
   UserObj* obj;

   UserObj_FDLock(&uci->fdState);
   obj = uci->fdState.cwd;
   UserObj_Acquire(obj);
   UserObj_FDUnlock(&uci->fdState);
   return obj;
}

/*
 *----------------------------------------------------------------------
 *
 * UserObj_Release --
 *
 *      Done with object returned by UserObj_Find or UserObj_Create.
 *      Decrement the reference count.
 *
 * Results:
 *      VMK_OK normally.  Error condition if the refcount went to 0 and
 *      there was an error closing the underlying io object.
 *
 *      XXX It's unfortunate that we can return an error here.  Most
 *      callers aren't prepared to deal with it; only UserObj_FDClose
 *      really wants it.  On the other hand, other callers shouldn't
 *      be able to get this error unless they are racing with
 *      UserObj_FDClose, so maybe it's OK.  Still, can we prevent
 *      this?  Needs a bit more thought.
 *
 * Side effects:
 *      Decrements refcount and possibly closes underlying io object.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserObj_Release(User_CartelInfo *uci, UserObj* obj)
{
   VMK_ReturnStatus status = VMK_OK;

   if ((int32) Atomic_FetchAndDec(&obj->refcount) <= 1) {
      // Ref count went from 1 to 0.  Object is permanently dead.
      ASSERT(Atomic_Read(&obj->refcount) == 0);
      UserObjAssertUnused(obj);
      status = obj->methods->close(obj, uci);
      Semaphore_Cleanup(&obj->sema);
      UWSTAT_ARRINC(userObjDestroyed, obj->type);
      User_HeapFree(uci, obj);
   }

   if (status != VMK_OK) {
      UWLOG(0, "returning %s", VMK_ReturnStatusToString(status));
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserObj_FDReserve --
 *
 *	Pre-allocates a fd from the cartel's descriptor table.
 *
 * Results:
 *      The descriptor number or USEROBJ_INVALID_HANDLE if none can be
 *      allocated.
 *
 * Side effects:
 *      If successful, this world's descriptor table is updated with
 *	USEROBJ_RESERVED_HANDLE.
 *
 *----------------------------------------------------------------------
 */
LinuxFd
UserObj_FDReserve(User_CartelInfo* uci)
{
   UserObj_State* fdState = &uci->fdState;
   LinuxFd i;

   UserObj_FDLock(fdState);
   for (i = 0; i < USEROBJ_MAX_HANDLES; i++) {
      if (fdState->descriptors[i] == NULL) {
	 fdState->descriptors[i] = USEROBJ_RESERVED_HANDLE;
         break;
      }
   }
   UserObj_FDUnlock(fdState);

   if (i >= USEROBJ_MAX_HANDLES) {
      UWLOG(0, "No free fds (%d allocated)", USEROBJ_MAX_HANDLES);
      i = USEROBJ_INVALID_HANDLE;
   }

   return i;
}


/*
 *----------------------------------------------------------------------
 *
 * UserObj_FDUnreserve --
 *
 *	Releases a previously pre-allocated fd that is no longer needed
 *	because of an error.  This should only be called if the UserObj
 *	for this fd was not successfully created.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      This world's descriptor table is cleared at position fd.
 *
 *----------------------------------------------------------------------
 */
void
UserObj_FDUnreserve(User_CartelInfo* uci, LinuxFd fd)
{
   UserObj_State* fdState = &uci->fdState;

   UserObj_FDLock(fdState);
   ASSERT(fdState->descriptors[fd] == USEROBJ_RESERVED_HANDLE);
   fdState->descriptors[fd] = NULL;
   UserObj_FDUnlock(fdState);
}


/*
 *----------------------------------------------------------------------
 *
 * UserObj_FDReplaceObj --
 *
 *	Replace an existing UserObj that's already in the file
 *	descriptor table with a new UserObj, maintaining the same index.
 *
 * Results:
 *	VMK_OK on success, VMK_NOT_FOUND if the original UserObj isn't
 *	found.
 *
 * Side effects:
 *      This world's descriptor table is updated.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserObj_FDReplaceObj(User_CartelInfo *uci, UserObj *oldObj, UserObj *newObj)
{
   VMK_ReturnStatus status = VMK_NOT_FOUND;
   int i;

   UserObj_State* fdState = &uci->fdState;

   UserObj_FDLock(fdState);

   for (i = 0; i < USEROBJ_MAX_HANDLES; i++) {
      if (fdState->descriptors[i] == oldObj) {
         fdState->descriptors[i] = newObj;
	 status = VMK_OK;
      }
   }
   UserObj_FDUnlock(fdState);
   
   if (status == VMK_OK) {
      /*
       * Call Release on it to dec the refcount and possibly destroy
       * this object.
       */
      (void) UserObj_Release(uci, oldObj);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserObj_FDAddObj --
 *
 *      Add an existing UserObj to the given thread's descriptor
 *      table at the given fd.  The refcount is assumed to have been
 *	incremented as necessary already.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      This world's descriptor table is updated, with an actual UserObj
 *	replacing USEROBJ_RESERVED_HANDLE.
 *
 *----------------------------------------------------------------------
 */
void
UserObj_FDAddObj(User_CartelInfo* uci, LinuxFd fd, UserObj* obj)
{
   UserObj_State* fdState = &uci->fdState;

   ASSERT(obj != NULL);

   UserObj_FDLock(fdState);
   ASSERT(fdState->descriptors[fd] == USEROBJ_RESERVED_HANDLE);
   fdState->descriptors[fd] = obj;
   UserObj_FDUnlock(fdState);
}


/*
 *----------------------------------------------------------------------
 *
 * UserObj_FDAdd --
 *
 *	Pre-allocates a fd, creates a new UserObj, then calls
 *	UserObj_FDAddObj to add it to the descriptor table at the
 *	allocated fd.
 *
 * Results:
 *      The descriptor number or USEROBJ_INVALID_HANDLE if none can be
 *      allocated.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
LinuxFd
UserObj_FDAdd(User_CartelInfo* uci, UserObj_Type type, UserObj_Data data,
              UserObj_Methods* methods, uint32 openFlags)
{
   LinuxFd fd;
   UserObj* obj;

   fd = UserObj_FDReserve(uci);
   if (fd == USEROBJ_INVALID_HANDLE) {
      return USEROBJ_INVALID_HANDLE;
   }

   obj = UserObj_Create(uci, type, data, methods, openFlags);
   if (obj == NULL) {
      UserObj_FDUnreserve(uci, fd);
      return USEROBJ_INVALID_HANDLE;
   }
   
   UserObj_FDAddObj(uci, fd, obj);

   return fd;
}


/*
 *----------------------------------------------------------------------
 *
 * UserObj_FDDup --
 *
 *      Add another reference to fromfd with descriptor number >= minfd.
 *      minfd is 0 for dup, nonzero for fcntl(fromfd, F_DUPFD, minfd).
 *
 * Results:
 *      VMK_OK or error condition. *newfd is the new fd.
 *
 * Side effects:
 *      If successful, this world's descriptor table is updated and
 *      obj's reference count is incremented.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserObj_FDDup(User_CartelInfo* uci, LinuxFd fromfd, LinuxFd minfd,
              LinuxFd* newfd)
{
   UserObj_State* fdState = &uci->fdState;
   VMK_ReturnStatus status = VMK_OK;
   UserObj *obj = NULL;
   LinuxFd i;

   if (fromfd >= USEROBJ_MAX_HANDLES) {
      return VMK_INVALID_HANDLE;
   }

   if (minfd < 0) {
      return VMK_INVALID_HANDLE;
   }

   status = UserObj_Find(uci, fromfd, &obj);
   if (status != VMK_OK) {
      return status;
   }

   UserObj_FDLock(fdState);
   for (i = minfd; i < USEROBJ_MAX_HANDLES; i++) {
      if (fdState->descriptors[i] == NULL) {
         break;
      }
   }
   if (i >= USEROBJ_MAX_HANDLES) {
      status = VMK_NO_FREE_HANDLES;
   } else {
      UserObj_Acquire(obj);
      fdState->descriptors[i] = obj;
      *newfd = i;
   }
   UserObj_FDUnlock(fdState);

   (void) UserObj_Release(uci, obj);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserObj_FDDup2 --
 *
 *      Add another reference to fromfd with descriptor number tofd,
 *      closing tofd first if it is in use.
 *
 * Results:
 *      VMK_OK or error condition.  If a close is needed on the old fd
 *      and it returns an error, fd is pointed to the new object
 *      anyway, and the error code from the close is ignored.
 *
 * Side effects:
 *      If successful, this world's descriptor table is updated and
 *      obj's reference count is incremented.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserObj_FDDup2(User_CartelInfo* uci, LinuxFd fromfd, LinuxFd tofd)
{
   UserObj_State* fdState = &uci->fdState;
   VMK_ReturnStatus status = VMK_OK;
   UserObj *obj;
   UserObj *oldObj;

   if (tofd >= USEROBJ_MAX_HANDLES) {
      return VMK_NO_FREE_HANDLES;
   } else if (tofd < 0) {
      return VMK_INVALID_HANDLE;
   }

   status = UserObj_Find(uci, fromfd, &obj);
   if (status != VMK_OK) {
      return status;
   }

   UserObj_FDLock(fdState);
   oldObj = fdState->descriptors[tofd];
   if (oldObj == USEROBJ_RESERVED_HANDLE) {
      /*
       * Race with open.  When we're opening something, we pre-allocate a fd.
       * It seems that the user's trying to dup something to that reserved fd.
       * In this (unlikely) case, Linux just returns EBUSY, so we will too.
       */
      status = VMK_BUSY;
   } else {
      UserObj_Acquire(obj);
      fdState->descriptors[tofd] = obj;
   }
   UserObj_FDUnlock(fdState);

   if (oldObj && oldObj != USEROBJ_RESERVED_HANDLE) {
      (void) UserObj_Release(uci, oldObj);
   }
   (void) UserObj_Release(uci, obj);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserObj_TraversePath --
 *
 *      Traverse the given pathname.
 * 
 *      With no flags, continue until either we have traversed the
 *      entire pathname or we've hit an arc that's not present.
 *      Follow all symlinks encountered, up to a maximum recursion
 *      depth of USEROBJ_SYMLINK_LIMIT.  If the entire pathname was
 *      traversed, return VMK_OK and set arc to "".  If the last
 *      object found was a directory and exactly one arc is left,
 *      return VMK_OK and return the remaining arc in arc.  Otherwise
 *      return an error.
 *
 *	With USEROBJ_OPEN_CREATE, if the last object found is a
 *	directory and exactly one arc is left, attempt to create the
 *	object as a file.  If successful, return the new file;
 *	otherwise return an error.
 *
 *      With USEROBJ_OPEN_CREATE and USEROBJ_OPEN_EXCLUSIVE both set,
 *      return VMK_EXISTS if the object already exists and do not open
 *      it.
 *
 *      With USEROBJ_OPEN_TRUNCATE, if the object is a file, attempt
 *      to truncate it to zero length after opening it.
 *
 *      With USEROBJ_OPEN_NOFOLLOW, if at some point the current
 *      object is a symlink and there are no more arcs in the current
 *      path, return VMK_IS_A_SYMLINK.
 *
 *      With USEROBJ_OPEN_PENULTIMATE, if at some point the current
 *      object is not a symlink and there is only one arc (or none)
 *      left in the current path, stop at the current object without
 *      trying to look up the remaining arc.
 *
 *      With USEROBJ_OPEN_IGNTRAILING, trailing slashes at the end of
 *      the pathname are ignored.  Otherwise a pathname with a
 *      trailing slash is considered to have "" as its final arc.
 *
 *      Note: although this routine crushes out consecutive slashes
 *      rather than considering them empty arcs, it is still possible
 *      for an object's open method to be called with an empty arc in
 *      some cases.  Essentially, this is a way of checking whether
 *      the object is a directory; if it is, the open should return
 *      the object itself (with incremented refcount); if not, it
 *      should return VMK_NOT_A_DIRECTORY.
 *
 * Results:
 *      As noted above.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserObj_TraversePath(User_CartelInfo* uci, const char* path,
                     uint32 flags, LinuxMode mode,
                     UserObj** objOut, char *arc, int arcLen)
{
   VMK_ReturnStatus status;
   UserObj* obj;

   // Find out whether to start at root or cwd
   if (path[0] == '/') {
      do {
         path++;
      } while (path[0] == '/');
      status = UserProxy_OpenRoot(uci, &obj);
      if (status != VMK_OK) {
         return status;
      }
   } else {
      obj = UserObj_AcquireCwd(uci);
   }

   return UserObjTraversePath(uci, obj, path, flags, mode, objOut, arc, arcLen,
                              USEROBJ_SYMLINK_LIMIT);
}


/*
 *----------------------------------------------------------------------
 *
 * UserObjTraversePath --
 *
 *      Recursive inner portion of UserObj_TraversePath, q.v.
 *
 * Results:
 *      VMK_ReturnStatus
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserObjTraversePath(User_CartelInfo* uci, UserObj* obj, const char* path,
                    uint32 flags, LinuxMode mode,
                    UserObj** objOut, char *arc, int maxArcLen,
                    unsigned symLinkLimit)
{
   VMK_ReturnStatus status;

   // Loop though the path arc by arc
   for (;;) {
      const char* tail;
      int arclen;
      UserObj* next;
      Bool finalArc;

      UWLOG(2, "obj %p, path %s, flags %#x", obj, path, flags);

      // Split path into <arc, tail>
      tail = strchr(path, '/');
      if (tail == NULL) {
         arclen = strlen(path);
      } else {
         arclen = tail - path;
      }
      while (tail && *tail == '/') {
         tail++;
      }
      finalArc = !tail || ((flags & USEROBJ_OPEN_IGNTRAILING) && !*tail);

      // Check if arc is too long
      if (arclen >= maxArcLen) {
         (void) UserObj_Release(uci, obj);
         *objOut = NULL;
         return VMK_NAME_TOO_LONG;
      }

      // Copy current arc to buffer.
      memcpy(arc, path, arclen);
      arc[arclen] = '\0';

      UWLOG(4, "arc %s, tail %s", arc, tail);

      if (finalArc && (flags & USEROBJ_OPEN_PENULTIMATE)) {
         // Path is down to one arc (or zero!), and that's as far as
         // we were asked to go.
         ASSERT(Atomic_Read(&obj->refcount) > 0);
         *objOut = obj;
         return VMK_OK;
      }

      if (finalArc) {
         // Open the final arc on the path.

         // These two flags are only intended for this function; don't
         // pass them on.
         uint32 oflags = flags & ~(USEROBJ_OPEN_PENULTIMATE
                                   |USEROBJ_OPEN_IGNTRAILING);
         status = obj->methods->open(obj, arc, oflags, mode, &next);

         if (status == VMK_OK) {
            (void) UserObj_Release(uci, obj);
            ASSERT(Atomic_Read(&next->refcount) > 0);
            *objOut = next;
            *arc = '\0';
            return VMK_OK;
         }
         if (status == VMK_NOT_FOUND) {
            // Special case: only the final arc was not found.  As noted
            // in the header comment, we tell the caller about this by
            // returning the last directory found and the nonempty final arc.
            ASSERT(Atomic_Read(&obj->refcount) > 0);
            *objOut = obj;
            return VMK_OK;
         }

      } else {
         // Open the next directory on the path.
         status = obj->methods->open(obj, arc, USEROBJ_OPEN_STAT, 0, &next);
      }

      // Handle indirection through symlinks
      if (status == VMK_IS_A_SYMLINK && symLinkLimit > 0) {
         char* path2;

         if (finalArc && (flags & USEROBJ_OPEN_NOFOLLOW)) {
            // Final arc is a symlink and we were asked not to follow it
            (void) UserObj_Release(uci, obj);
            return VMK_IS_A_SYMLINK;
         }

         path2 = User_HeapAlloc(uci, LINUX_PATH_MAX+1);
	 if (path2 == NULL) {
	    return VMK_NO_MEMORY;
	 }
         status = obj->methods->readSymLink(obj, arc, path2, LINUX_PATH_MAX);
         if (status == VMK_OK) {
            UWLOG(2, "symlink to %s", path2);
            if (strlen(path2) + (tail ? 1 + strlen(tail) : 0)
                > LINUX_PATH_MAX) {
               status = VMK_NAME_TOO_LONG;
            } else {
               if (tail && *tail) {
                  strcat(path2, "/");
                  strcat(path2, tail);
               }
               path = path2;

               // Find out whether to recurse in root
               if (path[0] == '/') {
                  do {
                     path++;
                  } while (path[0] == '/');
                  (void) UserObj_Release(uci, obj);
                  status = UserProxy_OpenRoot(uci, &obj);
               }
               if (status == VMK_OK) {
                  status = UserObjTraversePath(uci, obj, path, flags, mode,
                                               objOut, arc, maxArcLen,
					       symLinkLimit - 1);
               }
               User_HeapFree(uci, path2);
               return status;
            }
         }
         User_HeapFree(uci, path2);
      }

      (void) UserObj_Release(uci, obj);
      if (status != VMK_OK) {
         *objOut = NULL;
         return status;
      }

      obj = next;
      if (tail) {
         path = tail;
      } else {
         path = "";
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * UserObj_Open --
 *
 *      Open the specified pathname and return a UserObj.
 *      Usable for chdir, stat, and file open.
 *
 * Results:
 *      VMK_OK if the object can be found, error condition otherwise
 *
 * Side effects:
 *      A new file may be created.
 *      UserObj for the object is allocated.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserObj_Open(User_CartelInfo* uci,
             const char* path, uint32 flags, LinuxMode mode, UserObj** objOut)
{
   VMK_ReturnStatus status;
   UserObj* obj;
   char arc[LINUX_ARC_MAX+1] = "";

   *objOut = NULL;

   status = UserObj_TraversePath(uci, path, flags, mode, &obj, arc, sizeof arc);
   if (status != VMK_OK) {
      UWLOG(2, "UserObj_TraversePath returned %s",
            VMK_ReturnStatusToString(status));
      return status;
   }

   if (*arc) {
      // Object did not exist, and didn't get created as a file.
      // We don't want to do anything more in this case.
      (void) UserObj_Release(uci, obj);
      return VMK_NOT_FOUND;
   }      

   ASSERT(obj->openFlags == flags);
   *objOut = obj;

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserObj_Unlink --
 *
 *      Unlink the file in the given path.
 *
 * Results:
 *      VMK_OK or error condition.
 *
 * Side effects:
 *      The file's reference count in the filesystem is decremented.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserObj_Unlink(struct User_CartelInfo* uci,
               const char* path)
{
   VMK_ReturnStatus status;
   UserObj *parent;
   char arc[LINUX_ARC_MAX+1];

   status = UserObj_TraversePath(uci, path, USEROBJ_OPEN_PENULTIMATE, 0,
                                 &parent, arc, sizeof arc);
   if (status == VMK_OK) {
      status = parent->methods->unlink(parent, arc);
      (void) UserObj_Release(uci, parent);   
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserObj_FDClose --
 *
 *      Close the given descriptor.
 *
 * Results:
 *      VMK_OK or error condition.
 *
 * Side effects:
 *      The underlying io object is closed if the refcount goes to 0.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserObj_FDClose(User_CartelInfo* uci, LinuxFd fd)
{
   UserObj* obj;
   VMK_ReturnStatus status, status2;
   uint32 refcount;

   ASSERT(uci != NULL);

   status = UserObj_Find(uci, fd, &obj);
   if (status != VMK_OK) {
      return status;
   }

   UserObj_FDLock(&uci->fdState);
   // Check descriptor is still here; we could have raced with another close
   if (uci->fdState.descriptors[fd] == obj) {
      refcount = Atomic_FetchAndDec(&obj->refcount);
      ASSERT(refcount >= 2);  // 1 for our find, 1 for the reference
      uci->fdState.descriptors[fd] = NULL;
   } else {
      status = VMK_INVALID_HANDLE;
   }
   UserObj_FDUnlock(&uci->fdState);

   /*
    * Force a sync-on-close of VMFS files.  This is to minimize races
    * between the per-fd buffer cache and readMPN (which skips the
    * cache).  This lets open-write-mmap-close-<fault on mmap> 
    * idiom work for a VMFS fd.
    *
    * XXX REMOVE THIS when PR 44754 is fixed.  (When readmpn is
    * coherent with read and write.)
    */
   if (status == VMK_OK && obj->type == USEROBJ_TYPE_FILE) {
      Semaphore_Lock(&obj->sema);
      status = obj->methods->fsync(obj, FALSE);
      Semaphore_Unlock(&obj->sema);
   }

   /*
    * The underlying object close, if needed, is done in
    * UserObj_Release when it sees the refcount go to zero.
    */
   status2 = UserObj_Release(uci, obj);
   return status != VMK_OK ? status : status2;
}


/*
 *----------------------------------------------------------------------
 *
 * UserObj_FDSeek --
 *
 *      Seek on the given descriptor.
 *
 * Results:
 *      VMK_OK or error condition.  *res is always written to (new
 *      position on success, current on error).
 *
 * Side effects:
 *      Modifies the seek pointer.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserObj_FDSeek(User_CartelInfo* uci, LinuxFd fd, int64 offset, int whence,
               uint64 *res)
{
   UserObj* obj;
   VMK_ReturnStatus status;
   LinuxStat64 statbuf;

   ASSERT(uci != NULL);

   status = UserObj_Find(uci, fd, &obj);
   if (status != VMK_OK) {
      return status;
   }

   // Check that obj is seekable
   switch (obj->type) {
   case USEROBJ_TYPE_NONE:
      status = VMK_NOT_IMPLEMENTED;
      ASSERT(FALSE);
      break;
   case USEROBJ_TYPE_SOCKET_INET:
   case USEROBJ_TYPE_SOCKET_UNIX:
      status = VMK_ILLEGAL_SEEK;
      break;
   case USEROBJ_TYPE_PIPEREAD:
   case USEROBJ_TYPE_PIPEWRITE:
      status = VMK_ILLEGAL_SEEK;
      break;
   case USEROBJ_TYPE_ROOT:
   case USEROBJ_TYPE_FILE:
   case USEROBJ_TYPE_PROXY_FILE:
      status = VMK_OK;
      break;
   case USEROBJ_TYPE_PROXY_SOCKET:
   case USEROBJ_TYPE_PROXY_FIFO:
   case USEROBJ_TYPE_PROXY_CHAR:
      status = VMK_ILLEGAL_SEEK;
      break;
   default:
      status = VMK_NOT_IMPLEMENTED;
      ASSERT(FALSE);
      break;
   }
   if (status != VMK_OK) {
      (void) UserObj_Release(uci, obj);
      return status;
   }

   Semaphore_Lock(&obj->sema);
   switch (whence) {
   case USEROBJ_SEEK_SET:
      obj->offset = offset;
      break;
   case USEROBJ_SEEK_CUR:
      obj->offset += offset;
      break;
   case USEROBJ_SEEK_END:
      status = obj->methods->stat(obj, &statbuf);
      if (status == VMK_OK) {
         obj->offset = statbuf.st_size + offset;
      }
      break;
   default:
      status = VMK_BAD_PARAM;
      break;
   }
   *res = obj->offset;
   Semaphore_Unlock(&obj->sema);

   (void) UserObj_Release(uci, obj);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserObj_ReadMPN --
 *
 *      Read upto PAGE_SIZE bytes at the given offset in the given userObj
 *      Sets bytesRead to number of bytes actually read.  BytesRead is
 *      undefined if an error is returned.
 *
 * Results:
 *      VMK_OK if some bytes were successfully read.  Error code
 *      otherwise.  *bytesRead is set to number of bytes read.
 *
 * Side effects:
 *      Bytes are read from the object.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserObj_ReadMPN(UserObj *obj,
                MPN mpn,
                uint64 offset,
                uint32 *bytesRead)
{
   VMK_ReturnStatus status;

   ASSERT(bytesRead);
   ASSERT(offset % PAGE_SIZE == 0);
   *bytesRead = 0;

   if (!UserObj_IsOpenForRead(obj)) {
      return VMK_INVALID_HANDLE;
   }

   status = obj->methods->readMPN(obj, mpn, offset, bytesRead);

   ASSERT(*bytesRead <= PAGE_SIZE);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserObj_WriteMPN --
 *
 *      Write PAGE_SIZE bytes at the current offset in the given userObj.
 *      Sets bytesWritten to number of bytes actually written.  BytesWritten is
 *      undefined if an error is returned.
 *
 * Results:
 *      VMK_OK if some bytes were successfully written.  Error code
 *      otherwise.  *bytesWritten is set to number of bytes written.
 *
 * Side effects:
 *      Bytes are written to the object.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserObj_WriteMPN(UserObj *obj,
                MPN mpn,
		uint64 offset,
                uint32 *bytesWritten)
{
   VMK_ReturnStatus status;

   ASSERT(bytesWritten);
   ASSERT(offset % PAGE_SIZE == 0);
   *bytesWritten = 0;

   if (!UserObj_IsOpenForWrite(obj)) {
      return VMK_INVALID_HANDLE;
   }

   status = obj->methods->writeMPN(obj, mpn, offset, bytesWritten);

   ASSERT(*bytesWritten <= PAGE_SIZE);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserObjFDPoll --
 *
 *      Check if the given file descriptor is ready for vmkpollEvent
 *	If action is UserObjPollNotify, the object will record the
 *	current world to wakeup when the fd is ready.
 *
 * Results:
 *	VMK_OK if the fd is ready for the given inEvents (or is invalid)
 *	VMK_WOULD_BLOCK if the fd is not ready
 *	*outEvents contains per-fd status
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
UserObjFDPoll(struct User_CartelInfo* uci,
              LinuxFd fd,
              VMKPollEvent inEvents,
	      VMKPollEvent* outEvents,
              UserObjPollAction action)
{
   UserObj* obj;
   VMK_ReturnStatus status;

   ASSERT(uci != NULL);
   status = UserObj_Find(uci, fd, &obj);
   if (status == VMK_INVALID_HANDLE) {
      UWLOG(1, "Poll on invalid fd=%d.", fd);
      *outEvents = VMKPOLL_INVALID;
      return VMK_OK;
   } else if (status != VMK_OK) {
      UWLOG(0, "UserObj_Find(%d) returned %s.",
            fd, VMK_ReturnStatusToString(status));
      return status;
   }

   status = obj->methods->poll(obj, inEvents, outEvents, action);
   if ((status != VMK_OK) && (status != VMK_WOULD_BLOCK)) {
      UWLOG(0, "Poll on fd=%d (type %d) returned %s.",
            fd, obj->type, VMK_ReturnStatusToString(status));
   }

   (void) UserObj_Release(uci, obj);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserObjPollCleanupWaiters --
 *
 *      Clean up any waiters that were created by the call to
 *	UserObjPollNonBlock.  We disregard any errors we get along the
 *	way, since the only errors we'll hit is if the object is
 *	closed, and at that point cleanup doesn't really matter.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
UserObjPollCleanupWaiters(LinuxPollfd *pfds, uint32 nfds)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   int i;

   for (i = 0; i < nfds; i++) {
      VMK_ReturnStatus status;
      VMKPollEvent inEvents = 0;
      VMKPollEvent outEvents = 0;

      /*
       * Call the poll handler for this fd, telling it to free any resources
       * used for polling.
       */
      status = UserObjFDPoll(uci, pfds[i].fd, inEvents, &outEvents,
                             UserObjPollCleanup);
      /*
       * XXX: Arg... While it'd be nice to ASSERT just status == VMK_OK and
       * outEvents == 0, it's possible that the fd lookup will fail, which will
       * set outEvents to VMKPOLL_INVALID.  It's possible for the user to close
       * the fd from another thread after calling poll from this thread and
       * after we iterate through the fd list the first time but before we get
       * here.
       */
      ASSERT(status == VMK_OK &&
	     (outEvents == 0 || outEvents == VMKPOLL_INVALID));
   }
}


/*
 *----------------------------------------------------------------------
 *
 * UserObjPollNonBlock --
 *
 *      Poll the given list of descriptors, but don't block.
 *      If notify is TRUE, the object will record the current world to
 *      wakeup when the object is ready.
 *
 * Results:
 *	VMK_OK if at least one descriptor read, else VMK_WOULD_BLOCK
 *      or error status
 *      Also set outNumReady to number of descriptors that are ready 
 *
 * Side effects:
 *	nfds is set to the number of descriptors examined (shouldn't
 *	change from what's passed in unless we hit an error).
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserObjPollNonBlock(LinuxPollfd *pfds,
                    uint32* nfds,
                    Bool notify,
                    uint32 *outNumReady)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   UserObjPollAction action = notify ? UserObjPollNotify : UserObjPollNoAction;
   int i;
   int numReady = 0;

   for (i = 0; i < *nfds; i++) {
      VMK_ReturnStatus status;
      VMKPollEvent inEvents;
      VMKPollEvent outEvents;

      inEvents = User_LinuxToVMKPollFlags(pfds[i].inEvents);
      outEvents = 0;
      pfds[i].outEvents = 0;

      UWLOG(1, "  pfds[%d]={fd=%u, in=%#x, out=%#x}",
            i, pfds[i].fd, pfds[i].inEvents, pfds[i].outEvents);

      /*
       * Poll the actual object and ask it to notify us unless we already
       * have one active event, in which case we're not going to block.
       */
      status = UserObjFDPoll(uci, pfds[i].fd, inEvents, &outEvents,
                             (numReady > 0) ? UserObjPollNoAction : action);
      if (status == VMK_OK) {
         ASSERT(outEvents != 0); /* Must have a reason for not blocking. */
         numReady++;
         pfds[i].outEvents = User_VMKToLinuxPollFlags(outEvents);
      } else if (status != VMK_WOULD_BLOCK) {
         UWWarn("error %d", status);
	 *nfds = i;
         return status;
      }

   }

   if (outNumReady) {
      *outNumReady = numReady;
   }

   return numReady ? VMK_OK : VMK_WOULD_BLOCK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserObj_Poll --
 *
 *      Poll the given list of descriptors, and potentially block upto
 *      timeoutMillis ms.  A zero timeout means don't block, and a negative
 *      timeout means indefinite block until someone explicitly wakes this
 *      guy up.
 *
 * Results:
 *	UserThread_Wait return codes or errors
 *      Also set outNumReady to number of descriptors that are ready 
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserObj_Poll(LinuxPollfd *pfds,		// IN/OUT(.outEvents)
             uint32 inNfds,		// IN
             int32 timeoutMillis,	// IN
             uint32 *outNumReady)	// OUT
{
   User_ThreadInfo* uti = MY_RUNNING_WORLD->userThreadInfo;
   VMK_ReturnStatus status;
   const Bool blocking = (timeoutMillis != 0);
   uint32 nfds = inNfds;

   ASSERT(outNumReady != NULL);
   *outNumReady = 0;

   UWSTAT_INSERT(pollFdCount, inNfds);

   // UserThread_PrepareToWait handles notifications that arrive before wait
   if (blocking) {
      UserThread_PrepareToWait();
   }

   // poll and request objects to notify us when ready
   status = UserObjPollNonBlock(pfds, &nfds, blocking, outNumReady);
   UWLOG(2, "Registered with %d objects, %d are ready now",
         nfds, *outNumReady);
   if (status != VMK_WOULD_BLOCK) {
      /*
       * Error occurred, or one of the fds is ready to go, so let's clean up
       * any lingering waiters and return.
       */
      UserObjPollCleanupWaiters(pfds, nfds);
      if (blocking) {
         UserThread_CancelPreparedWait();
      }
      return status;
   }

   status = VMK_OK;
   ASSERT(*outNumReady == 0);
   ASSERT(nfds == inNfds);
   if (blocking) {
      Timer_RelCycles timeout = (timeoutMillis > 0) ? Timer_MSToTC(timeoutMillis) : 0;

      status = UserThread_Wait(UTWAIT_POLL_EVENT(uti),
                               CPUSCHED_WAIT_UW_POLL,
                               NULL, timeout,
                               UTWAIT_WITH_PREPARE);
      if (status == VMK_OK) {
         /* 
          * if an object sent us a wakeup, query again to find out
          * who, but ask to be notified because we're not going
          * to sleep again.
          */
         status = UserObjPollNonBlock(pfds, &nfds, FALSE, outNumReady);
         UWLOG(2, "Polled %d objects (after sleeping), %d are now ready",
               nfds, *outNumReady);
         if (status == VMK_WOULD_BLOCK) {
            status = VMK_OK;
         }
      }

      /*
       * Finally, clean up any waiters still around.
       *
       * Note: We use inNfds here because we know that the first call to
       * UserObjPollNonBlock iterated through inNfds and potentially added all
       * the fds as waiters.  The call above may have run into some trouble and
       * returned early.  However, we want to be sure to clean up the waiters
       * for all the fds, so we call this function with inNfds.
       */
      UserObjPollCleanupWaiters(pfds, inNfds);
   }

   ASSERT(uti->waitInfo.state == UTW_AWAKE);

   return status;
}


/*
 *-----------------------------------------------------------------------------
 *
 * UserObj_Chdir --
 *
 *      If obj is a directory, set the cartel's current working
 *      directory to obj, closing its old working directory.  If obj
 *      is not a directory (or can't be stat'ed), close obj and return
 *      an error.
 *
 *      If a close is needed on the old cwd and that returns an
 *      error, cwd is pointed to the new object anyway, and the error
 *      code from the close is ignored.
 *
 * Results:
 *      VMK_OK, VMK_NOT_A_DIRECTORY, or error on stat'ing obj.
 *
 * Side effects:
 *    The cartel's cwd is updated.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
UserObj_Chdir(User_CartelInfo* uci, UserObj* obj)
{
   UserObj* oldCwd;
   LinuxStat64 statbuf;
   VMK_ReturnStatus status;

   status = obj->methods->stat(obj, &statbuf);
   if (status != VMK_OK) {
      (void) UserObj_Release(uci, obj);
      return status;
   }
   if ((statbuf.st_mode & LINUX_MODE_IFMT) != LINUX_MODE_IFDIR) {
      (void) UserObj_Release(uci, obj);
      return VMK_NOT_A_DIRECTORY;
   }

   UserObj_FDLock(&uci->fdState);
   oldCwd = uci->fdState.cwd;
   uci->fdState.cwd = obj;
   UserObj_FDUnlock(&uci->fdState);
   if (vmx86_log)
   {
      VMK_ReturnStatus status;
      char oldCwdStr[LINUX_PATH_MAX+1];
      char newCwdStr[LINUX_PATH_MAX+1];
      status = oldCwd->methods->getName(oldCwd, oldCwdStr, sizeof oldCwdStr);
      if (status != VMK_OK) {
         snprintf(oldCwdStr, sizeof oldCwdStr, "<n/a>");
      }
      status = obj->methods->getName(obj, newCwdStr, sizeof newCwdStr);
      if (status != VMK_OK) {
         snprintf(newCwdStr, sizeof newCwdStr, "<n/a>");
      }
      UWLOG(3, "chdir from '%s' to '%s'",oldCwdStr, newCwdStr);
   }
   (void) UserObj_Release(uci, oldCwd);

   return VMK_OK;
}


/*
 *-----------------------------------------------------------------------------
 *
 * UserObj_GetDirName --
 *
 *      Get the canonical pathname of the specified directory object.
 *
 * Results:
 *      VMK_ReturnStatus is returned; name is placed within buf,
 *      starting at *bufOut.
 *      
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
UserObj_GetDirName(struct User_CartelInfo* uci, UserObj *obj,
                   char* buf, uint32 bufsize, char **bufOut)
{
   char arc[LINUX_ARC_MAX+1];
   int i = bufsize; // i = number of unused bytes in buffer
   VMK_ReturnStatus status = VMK_OK;

   if (bufsize < 2) {
      // must be room at least for "/"!
      return VMK_RESULT_TOO_LARGE;
   }

   UserObj_Acquire(obj);

   buf[--i] = '\0';
   
   for (;;) {
      UserObj* parent;
      int arclen;

      status = obj->methods->getName(obj, arc, sizeof arc);
      if (status != VMK_OK) {
         break;
      }
      UWLOG(4, "arc=\"%s\"", arc);

      arclen = strlen(arc);
      if (arclen == 0) {
         break;
      }
      if (arclen + 1 > i) {
         status = VMK_RESULT_TOO_LARGE;
         break;
      }

      i -= arclen;
      memcpy(&buf[i], arc, arclen);
      buf[--i] = '/';

      status = obj->methods->open(obj, "..", USEROBJ_OPEN_STAT, 0, &parent);
      if (status != VMK_OK) {
         break;
      }
      (void) UserObj_Release(uci, obj);
      obj = parent;
   }

   (void) UserObj_Release(uci, obj);
   if (status == VMK_OK) {
      if (buf[i] == '\0') {
         // Root is "/", not ""
         buf[--i] = '/';
      }
      *bufOut = &buf[i];
   }
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserObj_ToString --
 *
 *	Invokes the toString method suite function.
 *
 * Results:
 *	Refer to the toString functions.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserObj_ToString(UserObj *obj, char *string, int length)
{
   return obj->methods->toString(obj, string, length);
}


/*
 *----------------------------------------------------------------------
 *
 * UserObj_Nop --
 *
 *      Do nothing, but report success.  Not an effective way to climb
 *      the R&D technical ladder.
 *
 * Results:
 *      VMK_OK
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserObj_Nop(void)
{
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserObj_NotADirectory --
 *
 *      Handle attempts to open things relative to non-directories
 *
 * Results:
 *      VMK_NOT_A_DIRECTORY
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserObj_NotADirectory(void)
{
   return VMK_NOT_A_DIRECTORY;
}


/*
 *----------------------------------------------------------------------
 *
 * UserObj_IsADirectory --
 *
 *      Handle attempts to read or write a directory.
 *
 * Results:
 *      VMK_IS_A_DIRECTORY
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserObj_IsADirectory(void)
{
   return VMK_IS_A_DIRECTORY;
}


/*
 *----------------------------------------------------------------------
 *
 * UserObj_BadParam --
 *
 *      Handle attempts to read/write something that inherently can't
 *      be read/written.  Not needed for the case of reading something
 *      that's not open for reading or writing something that's not
 *      open for writing; that's handled in UserObj_FDRead/FDWrite.
 *
 * Results:
 *      VMK_BAD_PARAM
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserObj_BadParam(void)
{
   UWLOG_StackTraceCurrent(1);
   return VMK_BAD_PARAM;
}


/*
 *----------------------------------------------------------------------
 *
 * UserObj_NoRmAccess --
 *
 *      Handle operations that should return VMK_NO_ACCESS if the
 *      object exists or otherwise VMK_NOT_FOUND.
 *
 * Results:
 *      VMK_NOT_FOUND or VMK_NO_ACCESS
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserObj_NoRmAccess(UserObj *obj, const char* arc)
{
   UserObj* child;
   VMK_ReturnStatus status;

   ASSERT(World_IsUSERWorld(MY_RUNNING_WORLD));
   status = obj->methods->open(obj, arc, 0, 0, &child);
   if (status == VMK_OK) {
      (void) UserObj_Release(MY_USER_CARTEL_INFO, child);
      return VMK_NO_ACCESS;
   }
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserObj_NoMkAccess --
 *
 *      Handle operations that should return VMK_EXISTS if the object
 *      exists or otherwise VMK_NO_ACCESS.
 *
 * Results:
 *      VMK_EXISTS or VMK_NO_ACCESS
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserObj_NoMkAccess(UserObj *obj, const char* arc)
{
   UserObj* child;
   VMK_ReturnStatus status;

   ASSERT(World_IsUSERWorld(MY_RUNNING_WORLD));
   status = obj->methods->open(obj, arc, 0, 0, &child);
   if (status == VMK_OK) {
      (void) UserObj_Release(MY_USER_CARTEL_INFO, child);
      return VMK_EXISTS;
   }
   if (status == VMK_NOT_FOUND) {
      return VMK_NO_ACCESS;
   }
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserObj_NotASocket --
 *
 *      Handle operations that should return VMK_NOT_A_SOCKET if the
 *	object is not a socket.
 *
 * Results:
 *	VMK_NOT_A_SOCKET.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserObj_NotASocket(UserObj* obj)
{
   UWWarn("Trying to perform a socket operation on type %d", obj->type);
   UWLOG_StackTraceCurrent(0);
   return VMK_NOT_A_SOCKET;
}


/*
 *----------------------------------------------------------------------
 *
 * UserObj_NotImplemented --
 *
 *      Placeholder for methods that are not implemented.  Uses of
 *      this method should have a comment saying whether we expect to
 *      need an implementation in the future.
 *
 * Results:
 *      VMK_NOT_IMPLEMENTED
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserObj_NotImplemented(UserObj* obj)
{
   UWWarn("Unimplemented operation on type %d", obj->type);
   UWLOG_StackTraceCurrent(0);
   return VMK_NOT_IMPLEMENTED;
}


/*
 *----------------------------------------------------------------------
 *
 * UserObj_DumpObjTypes --
 *
 *	Dumps the object type strings.
 *
 * Results:
 *	VMK_OK on success, otherwise on failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserObj_DumpObjTypes(UserDump_Header *header, UserDump_DumpData *dumpData)
{
   VMK_ReturnStatus status;
   int i, len;

   for (i = 0; i <= USEROBJ_TYPE_MAXIMUMTYPE; i++) {
      len = strlen(userObjTypes[i]) + 1;

      status = UserDump_Write(dumpData, (void*)userObjTypes[i], len);
      if (status != VMK_OK) {
	 UWLOG(0, "Failed to write out userObjTypes[%d]: %s -> %s", i,
	       userObjTypes[i], UWLOG_ReturnStatusToString(status));
	 return status;
      }

      header->objTypesSize += len;
   }

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserObj_DumpFdTable --
 *
 *	Dumps the fd table of this cartel to the core file.
 *
 *	Note: We have to jump through some hoops here.  Calls to the
 *	method suite function 'toString' may need to block, so we can't
 *	hold the UserObj lock while calling it.  Thus, we need to save
 *	off the fds and UserObjs while holding the UserObj lock, then
 *	perform the toString calls later.
 *
 * Results:
 *	VMK_OK on success, otherwise on failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserObj_DumpFdTable(UserDump_Header *dumpHeader, UserDump_DumpData *dumpData)
{
   User_CartelInfo *uci = MY_USER_CARTEL_INFO;
   VMK_ReturnStatus status = VMK_OK;
   UserDump_ObjEntry *objEntry = NULL;
   LinuxFd fd;
   int numObjs = 0;
   struct {
      int fd;
      UserObj *obj;
   } *fdList = NULL;

   objEntry = User_HeapAlloc(uci, sizeof *objEntry);
   if (objEntry == NULL) {
      return VMK_NO_MEMORY;
   }

   fdList = User_HeapAlloc(uci, USEROBJ_MAX_HANDLES * sizeof *fdList);
   if (fdList == NULL) {
      status = VMK_NO_MEMORY;
      goto done;
   }

   UserObj_FDLock(&uci->fdState);
   for (fd = 0; fd < USEROBJ_MAX_HANDLES; fd++) {
      if (uci->fdState.descriptors[fd] != NULL) {
	 fdList[numObjs].fd = fd;
         fdList[numObjs].obj = uci->fdState.descriptors[fd];
	 /*
	  * Up the refcount on this UserObj so it won't go away.
	  */
	 UserObj_Acquire(fdList[numObjs].obj);
	 numObjs++;
      }
   }
   UserObj_FDUnlock(&uci->fdState);

   for (fd = 0; fd < numObjs; fd++) {
      memset(objEntry, 0, sizeof *objEntry);

      objEntry->obj = (uint32)fdList[fd].obj;
      objEntry->fd = fdList[fd].fd;
      objEntry->type = fdList[fd].obj->type;
      status = fdList[fd].obj->methods->toString(fdList[fd].obj,
						 objEntry->description,
						 sizeof objEntry->description);
      if (status != VMK_OK) {
	 UWLOG(0, "toString failed for obj %p (type %d): %s", fdList[fd].obj,
	       fdList[fd].obj->type, UWLOG_ReturnStatusToString(status));
	 snprintf(objEntry->description, sizeof objEntry->description,
		  "Unable to retrieve description for this object.");
      }

      status = UserDump_Write(dumpData, (uint8*)objEntry, sizeof *objEntry);
      if (status != VMK_OK) {
	 UWLOG(0, "Failed to dump file descriptor table (at fd %d): %s",
	       fdList[fd].fd, UWLOG_ReturnStatusToString(status));
	 goto done;
      }
   }

   dumpHeader->objEntries = numObjs;

done:
   for (fd = 0; fd < numObjs; fd++) {
      UserObj_Release(uci, fdList[fd].obj);
   }
   if (objEntry != NULL) {
      User_HeapFree(uci, objEntry);
   }
   if (fdList != NULL) {
      User_HeapFree(uci, fdList);
   }
   return status;
}
