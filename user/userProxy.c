/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * userProxy.c --
 *
 *      Userworld interface to console os files & pipes.
 */

#include "user_int.h"
#include "userObj.h"
#include "userProxy_ext.h"
#include "kvmap.h"
#include "memalloc.h"
#include "libc.h"
#include "linuxAPI.h"
#include "cpusched.h"
#include "userFile.h"
#include "return_status.h"
#include "userStat.h"

#define LOGLEVEL_MODULE UserProxy
#include "userLog.h"

static VMK_ReturnStatus UserProxyObjFind(User_CartelInfo* uci, int fileHandle,
					 UserObj** outObj);
static VMK_ReturnStatus UserProxyOpen(UserObj* obj, const char *path,
                                      uint32 flags, LinuxMode mode,
                                      UserObj **objOut);

static VMK_ReturnStatus UserProxyClose(UserObj* obj,
                                       User_CartelInfo* uci);

static VMK_ReturnStatus UserProxyRead(UserObj* obj, UserVA userData,
                                      uint64 offset, uint32 length,
                                      uint32 *bytesRead);

static VMK_ReturnStatus UserProxyReadMPN(UserObj* obj, MPN mpn,
                                         uint64 offset, uint32 *bytesRead);

static VMK_ReturnStatus UserProxyReadInt(UserObj* obj,
                                         void* userData,
                                         uint64 offset,
                                         uint32 length,
                                         uint32 *bytesRead,
					 Util_BufferType bufType);

static VMK_ReturnStatus UserProxyWrite(UserObj* obj, UserVAConst userData,
                                       uint64 offset, uint32 length,
                                       uint32 *bytesWritten);
static VMK_ReturnStatus UserProxyWriteMPN(UserObj* obj, MPN mpn,
                                          uint64 offset, uint32 *bytesWritten);
static VMK_ReturnStatus UserProxyWriteInt(UserObj* obj,
                                          void* userData,
                                          uint64 offset,
                                          uint32 length,
                                          uint32 *bytesWritten,
                                          Util_BufferType bufType);

static VMK_ReturnStatus UserProxyStat(UserObj* obj, LinuxStat64* statbuf);
static VMK_ReturnStatus UserProxyChmod(UserObj* obj, LinuxMode mode);
static VMK_ReturnStatus UserProxyChown(UserObj* obj, Identity_UserID owner,
                                       Identity_GroupID group);
static VMK_ReturnStatus UserProxyTruncate(UserObj* obj, uint64 size);
static VMK_ReturnStatus UserProxyUtime(UserObj* obj,
                                       uint32 atime, uint32 mtime);
static VMK_ReturnStatus UserProxyStatFS(UserObj* obj, LinuxStatFS64* statbuf);
static VMK_ReturnStatus UserProxyPoll(UserObj* obj, VMKPollEvent inEvent,
				      VMKPollEvent* outEvents,
				      UserObjPollAction action);

static VMK_ReturnStatus UserProxyUnlink(UserObj* parent, const char* arc);
static VMK_ReturnStatus UserProxyRmdir(UserObj* parent, const char* arc);
static VMK_ReturnStatus UserProxyMkdir(UserObj* parent, const char* arc,
                                       LinuxMode mode);
static VMK_ReturnStatus UserProxyGetName(UserObj* obj, char* arc, uint32 length);
static VMK_ReturnStatus UserProxyReadSymLink(UserObj* parent, const char *arc,
                                             char *buf, unsigned buflen);
static VMK_ReturnStatus UserProxyMakeSymLink(UserObj* parent, const char *arc,
                                             const char *link);
static VMK_ReturnStatus UserProxyMakeHardLink(UserObj* parent, const char *arc,
                                              UserObj* target);
static VMK_ReturnStatus UserProxyRename(UserObj* newDir, const char *newArc,
                                        UserObj* oldDir, const char *oldArc);
static VMK_ReturnStatus UserProxyMknod(UserObj* obj, char* arc,
                                       LinuxMode mode);
static VMK_ReturnStatus UserProxyFcntl(UserObj* obj, uint32 cmd, uint32 arg);
static VMK_ReturnStatus UserProxyFsync(UserObj* obj, Bool dataOnly);
static VMK_ReturnStatus UserProxyBind(UserObj* obj, LinuxSocketName* name,
				      uint32 nameLen);
static VMK_ReturnStatus UserProxyConnect(UserObj* obj, LinuxSocketName* name,
					 uint32 nameLen);
static VMK_ReturnStatus UserProxyListen(UserObj* obj, int backlog);
static VMK_ReturnStatus UserProxyAccept(UserObj* obj, UserObj** newObj,
                                        LinuxSocketName* name,
                                        uint32* nameLen);
static VMK_ReturnStatus UserProxyGetSocketName(UserObj* obj,
					       LinuxSocketName* name,
					       uint32* nameLen);
static VMK_ReturnStatus UserProxySendmsg(UserObj* obj, LinuxMsgHdr* msg,
                                         uint32 len, uint32* bytesSent);
static VMK_ReturnStatus UserProxyRecvmsg(UserObj* obj, LinuxMsgHdr* msg,
                                         uint32 len, uint32* bytesRecv);
static VMK_ReturnStatus UserProxySetsockopt(UserObj* obj, int level,
                                            int optName, char* optVal,
                                            int optLen);
static VMK_ReturnStatus UserProxyGetsockopt(UserObj* obj, int level,
                                            int optName, char* optVal,
                                            int* optLen);
static VMK_ReturnStatus UserProxyReadDir(UserObj* obj,
                                         UserVA /* LinuxDirent64* */ userData,
                                         uint32 length, uint32* bytesRead);
static VMK_ReturnStatus UserProxyIoctl(UserObj* obj, uint32 cmd,
                                       LinuxIoctlArgType type,
                                       uint32 size, void *userData,
                                       uint32 *result);
static VMK_ReturnStatus UserProxyToString(UserObj *obj, char *string,
					  int length);
static VMK_ReturnStatus UserProxyRootOpen(UserObj* parent, const char* arc,
                                          uint32 flags, LinuxMode mode,
                                          UserObj** objOut);
static VMK_ReturnStatus UserProxyRootGetName(UserObj* obj,
                                             char* arc,
					     uint32 length);
static VMK_ReturnStatus UserProxyGetPeerName(UserObj* obj,
                                             LinuxSocketName* name,
                                             uint32* nameLen);
static VMK_ReturnStatus UserProxyShutdown(UserObj* obj,
                                          int how);

/* Methods on a proxied file or directory */
static UserObj_Methods proxyFileMethods = USEROBJ_METHODS(
   UserProxyOpen,
   UserProxyClose,
   UserProxyRead,
   UserProxyReadMPN,
   UserProxyWrite,
   UserProxyWriteMPN,
   UserProxyStat,
   UserProxyChmod,
   UserProxyChown,
   UserProxyTruncate,
   UserProxyUtime,
   UserProxyStatFS,
   UserProxyPoll,
   UserProxyUnlink,
   UserProxyMkdir,
   UserProxyRmdir,
   UserProxyGetName,
   UserProxyReadSymLink,
   UserProxyMakeSymLink,
   UserProxyMakeHardLink,
   UserProxyRename,
   UserProxyMknod,
   UserProxyFcntl,
   UserProxyFsync,
   UserProxyReadDir,
   UserProxyIoctl,
   UserProxyToString,
   (UserObj_BindMethod) UserObj_NotASocket,
   (UserObj_ConnectMethod) UserObj_NotASocket,
   (UserObj_SocketpairMethod) UserObj_NotASocket,
   (UserObj_AcceptMethod) UserObj_NotASocket,
   (UserObj_GetSocketNameMethod) UserObj_NotASocket,
   (UserObj_ListenMethod) UserObj_NotASocket,
   (UserObj_SetsockoptMethod) UserObj_NotASocket,
   (UserObj_GetsockoptMethod) UserObj_NotASocket,
   (UserObj_SendmsgMethod) UserObj_NotASocket,
   (UserObj_RecvmsgMethod) UserObj_NotASocket,
   (UserObj_GetPeerNameMethod) UserObj_NotASocket,
   (UserObj_ShutdownMethod) UserObj_NotASocket
);

/* Methods on the root directory ("/") */
static UserObj_Methods proxyRootMethods = USEROBJ_METHODS(
   UserProxyRootOpen,
   UserProxyClose,
   (UserObj_ReadMethod) UserObj_IsADirectory,
   (UserObj_ReadMPNMethod) UserObj_IsADirectory,
   (UserObj_WriteMethod) UserObj_IsADirectory,
   (UserObj_WriteMPNMethod) UserObj_IsADirectory,
   UserProxyStat,
   UserProxyChmod,
   UserProxyChown,
   UserProxyTruncate,
   UserProxyUtime,
   UserProxyStatFS,
   (UserObj_PollMethod) UserObj_IsADirectory,
   UserProxyUnlink,
   UserProxyMkdir,
   UserProxyRmdir,
   UserProxyRootGetName,
   UserProxyReadSymLink,
   UserProxyMakeSymLink,
   UserProxyMakeHardLink,
   UserProxyRename,
   UserProxyMknod,
   UserProxyFcntl,
   UserProxyFsync,
   UserProxyReadDir,
   (UserObj_IoctlMethod) UserObj_BadParam,
   UserProxyToString,
   (UserObj_BindMethod) UserObj_NotASocket,
   (UserObj_ConnectMethod) UserObj_NotASocket,
   (UserObj_SocketpairMethod) UserObj_NotASocket,
   (UserObj_AcceptMethod) UserObj_NotASocket,
   (UserObj_GetSocketNameMethod) UserObj_NotASocket,
   (UserObj_ListenMethod) UserObj_NotASocket,
   (UserObj_SetsockoptMethod) UserObj_NotASocket,
   (UserObj_GetsockoptMethod) UserObj_NotASocket,
   (UserObj_SendmsgMethod) UserObj_NotASocket,
   (UserObj_RecvmsgMethod) UserObj_NotASocket,
   (UserObj_GetPeerNameMethod) UserObj_NotASocket,
   (UserObj_ShutdownMethod) UserObj_NotASocket
);

/* Methods on a proxied fifo */
static UserObj_Methods proxyFifoMethods = USEROBJ_METHODS(
   (UserObj_OpenMethod) UserObj_NotADirectory,
   UserProxyClose,
   UserProxyRead,
   (UserObj_ReadMPNMethod) UserObj_BadParam,
   UserProxyWrite,
   (UserObj_WriteMPNMethod) UserObj_BadParam,
   UserProxyStat,
   UserProxyChmod,
   UserProxyChown,
   (UserObj_TruncateMethod) UserObj_BadParam,
   UserProxyUtime,
   UserProxyStatFS,
   UserProxyPoll,
   (UserObj_UnlinkMethod) UserObj_NotADirectory,
   (UserObj_MkdirMethod) UserObj_NotADirectory,
   (UserObj_RmdirMethod) UserObj_NotADirectory,
   (UserObj_GetNameMethod) UserObj_NotADirectory,
   (UserObj_ReadSymLinkMethod) UserObj_NotADirectory,
   (UserObj_MakeSymLinkMethod) UserObj_NotADirectory,
   (UserObj_MakeHardLinkMethod) UserObj_NotADirectory,
   (UserObj_RenameMethod) UserObj_NotADirectory,
   (UserObj_MknodMethod) UserObj_NotADirectory,
   UserProxyFcntl,
   UserProxyFsync,
   (UserObj_ReadDirMethod) UserObj_NotADirectory,
   (UserObj_IoctlMethod) UserObj_BadParam,
   UserProxyToString,
   (UserObj_BindMethod) UserObj_NotASocket,
   (UserObj_ConnectMethod) UserObj_NotASocket,
   (UserObj_SocketpairMethod) UserObj_NotASocket,
   (UserObj_AcceptMethod) UserObj_NotASocket,
   (UserObj_GetSocketNameMethod) UserObj_NotASocket,
   (UserObj_ListenMethod) UserObj_NotASocket,
   (UserObj_SetsockoptMethod) UserObj_NotASocket,
   (UserObj_GetsockoptMethod) UserObj_NotASocket,
   (UserObj_SendmsgMethod) UserObj_NotASocket,
   (UserObj_RecvmsgMethod) UserObj_NotASocket,
   (UserObj_GetPeerNameMethod) UserObj_NotASocket,
   (UserObj_ShutdownMethod) UserObj_NotASocket
);

/* UserObj callback methods for proxied sockets.  */
static UserObj_Methods proxySocketMethods = USEROBJ_METHODS(
   (UserObj_OpenMethod) UserObj_NotADirectory,
   UserProxyClose,
   UserProxyRead,
   (UserObj_ReadMPNMethod) UserObj_BadParam,
   UserProxyWrite,
   (UserObj_WriteMPNMethod) UserObj_BadParam,
   UserProxyStat,
   (UserObj_ChmodMethod) UserObj_NotImplemented,    // not needed
   (UserObj_ChownMethod) UserObj_NotImplemented,    // not needed
   (UserObj_TruncateMethod) UserObj_NotImplemented, // not needed
   (UserObj_UtimeMethod) UserObj_NotImplemented,    // not needed
   (UserObj_StatFSMethod) UserObj_NotImplemented,   // not needed
   UserProxyPoll,
   (UserObj_UnlinkMethod) UserObj_NotADirectory,
   (UserObj_MkdirMethod) UserObj_NotADirectory,
   (UserObj_RmdirMethod) UserObj_NotADirectory,
   (UserObj_GetNameMethod) UserObj_NotADirectory,
   (UserObj_ReadSymLinkMethod) UserObj_NotADirectory,
   (UserObj_MakeSymLinkMethod) UserObj_NotADirectory,
   (UserObj_MakeHardLinkMethod) UserObj_NotADirectory,
   (UserObj_RenameMethod) UserObj_NotADirectory,
   (UserObj_MknodMethod) UserObj_NotADirectory,
   UserProxyFcntl,
   (UserObj_FsyncMethod) UserObj_BadParam,
   (UserObj_ReadDirMethod) UserObj_NotADirectory,
   UserProxyIoctl,
   UserProxyToString,
   UserProxyBind,
   UserProxyConnect,
   (UserObj_SocketpairMethod) UserObj_NotImplemented, // XXX direct call instead
   UserProxyAccept,
   UserProxyGetSocketName,
   UserProxyListen,
   UserProxySetsockopt,
   UserProxyGetsockopt,
   UserProxySendmsg,
   UserProxyRecvmsg,
   UserProxyGetPeerName,
   UserProxyShutdown
);

/* Methods on a proxied character device */
static UserObj_Methods proxyCharMethods = USEROBJ_METHODS(
   (UserObj_OpenMethod) UserObj_NotADirectory,
   UserProxyClose,
   UserProxyRead,
   UserProxyReadMPN,
   UserProxyWrite,
   UserProxyWriteMPN,
   UserProxyStat,
   UserProxyChmod,
   UserProxyChown,
   (UserObj_TruncateMethod) UserObj_BadParam,
   UserProxyUtime,
   UserProxyStatFS,
   UserProxyPoll,
   (UserObj_UnlinkMethod) UserObj_NotADirectory,
   (UserObj_MkdirMethod) UserObj_NotADirectory,
   (UserObj_RmdirMethod) UserObj_NotADirectory,
   (UserObj_GetNameMethod) UserObj_NotADirectory,
   (UserObj_ReadSymLinkMethod) UserObj_NotADirectory,
   (UserObj_MakeSymLinkMethod) UserObj_NotADirectory,
   (UserObj_MakeHardLinkMethod) UserObj_NotADirectory,
   (UserObj_RenameMethod) UserObj_NotADirectory,
   (UserObj_MknodMethod) UserObj_NotADirectory,
   UserProxyFcntl,
   UserProxyFsync,
   (UserObj_ReadDirMethod) UserObj_NotADirectory,
   UserProxyIoctl,
   UserProxyToString,
   (UserObj_BindMethod) UserObj_NotASocket,
   (UserObj_ConnectMethod) UserObj_NotASocket,
   (UserObj_SocketpairMethod) UserObj_NotASocket,
   (UserObj_AcceptMethod) UserObj_NotASocket,
   (UserObj_GetSocketNameMethod) UserObj_NotASocket,
   (UserObj_ListenMethod) UserObj_NotASocket,
   (UserObj_SetsockoptMethod) UserObj_NotASocket,
   (UserObj_GetsockoptMethod) UserObj_NotASocket,
   (UserObj_SendmsgMethod) UserObj_NotASocket,
   (UserObj_RecvmsgMethod) UserObj_NotASocket,
   (UserObj_GetPeerNameMethod) UserObj_NotASocket,
   (UserObj_ShutdownMethod) UserObj_NotASocket
);

typedef struct UserProxyPollCache {
   SP_SpinLock		lock;
   Bool			enabled;
   uint32		refCount;
   VMKPollEvent		cache;
   uint32		updateGeneration;
   VMKPollWaitersList	waiters;
} UserProxyPollCache;

// XXX comment me
typedef struct UserProxy_ObjInfo {
   UserProxy_CartelInfo *upci;
   int                  fileHandle;
   char                 fullPath[LINUX_PATH_MAX + 1];
   UserProxyPollCache	*pollCache;
} UserProxy_ObjInfo;

#define USERPROXY_INVALID_FD	-1


/*
 *----------------------------------------------------------------------
 *
 * UserProxyUCIForUPCI --
 *
 *	Returns the User_CartelInfo struct for the given proxy
 *	connection.
 *
 * Results:
 *	User_CartelInfo.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static INLINE User_CartelInfo*
UserProxyUCIForUPCI(UserProxy_CartelInfo *upci)
{
   ASSERT(upci != NULL);
   ASSERT(upci->uci != NULL);
   ASSERT(upci->cartelID == upci->uci->cartelID);
   return upci->uci;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyVerifyConnection --
 *
 *      Opens a connection to the proxy, if it doesn't already exist.
 *
 *      [XXX this should eventually be non-lazy]
 *
 * Results:
 *      A VMK_ReturnStatus value.
 *
 * Side effects:
 *      Makes an rpc connection.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus 
UserProxyVerifyConnection(UserProxy_CartelInfo *upci)
{
   VMK_ReturnStatus status = VMK_OK;
   char name[RPC_CNX_NAME_LENGTH];

   if (upci->disconnected) {
      UWLOG(0, "Prior disconnection forced.  Not attempting reconnection.");
      ASSERT(upci->cnxToProxyID == -1);
      ASSERT(upci->cnxToKernelID == -1);
      return VMK_IS_DISCONNECTED;
   }

   if (upci->cnxToProxyID == -1) {
      ASSERT(upci->cnxToKernelID == -1);

      snprintf(name, sizeof(name), "ToProxy.%d", upci->cartelID);
      status = RPC_Connect(name, &upci->cnxToProxyID);
      if (status != VMK_OK) {
         UWLOG(0, "%s connect failed: %s",
               name, UWLOG_ReturnStatusToString(status));
	 return status;
      }

      // need to have at least 10 digits for world ID + null
      ASSERT(RPC_CNX_NAME_LENGTH > sizeof("ToKernel.") + 11);
      snprintf(name, sizeof(name), "ToKernel.%d", upci->cartelID);
      status = RPC_Connect(name, &upci->cnxToKernelID);
      if (status != VMK_OK) {
         RPC_Disconnect(upci->cnxToProxyID);
	 upci->cnxToProxyID = -1;
         UWLOG(0, "%s connect failed: %s",
               name, UWLOG_ReturnStatusToString(status));
	 return status;
      }

      UWLOG(1, "cnxToProxyID = %d", upci->cnxToProxyID);
      UWLOG(1, "cnxToKernelID = %d", upci->cnxToKernelID);
   } 

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyForceDisconnect --
 *
 *	Disconnect the proxy.  Prevent further (lazy) reconnections.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Existing proxy connections are dropped.
 *
 *----------------------------------------------------------------------
 */
static void
UserProxyForceDisconnect(UserProxy_CartelInfo* upci)
{
   /*
    * We can get away without locking because any other racers with the
    * disconnection will eventually hit the problem we ran into, or will
    * refresh their cnxID and notice the -1.  This only works because
    * connections come online early (when there is only 1 thread), and
    * once it goes off-line, it never comes back on.
    */

   upci->disconnected = TRUE;

   if (upci->cnxToProxyID != -1) {
      RPC_Disconnect(upci->cnxToProxyID);
      UWLOG(1, "Disconnected rpc cnx %d", upci->cnxToProxyID);
      upci->cnxToProxyID = -1;
   }

   if (upci->cnxToKernelID != -1) {
      RPC_Disconnect(upci->cnxToKernelID);
      UWLOG(1, "Disconnected rpc cnx %d", upci->cnxToKernelID);
      upci->cnxToKernelID = -1;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyObjPreallocate --
 *
 *	Allocates memory for a new proxy object.
 *
 * Results:
 *	A pointer to the newly allocated proxy object, NULL otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static UserObj*
UserProxyObjPreallocate(User_CartelInfo* uci)
{
   UserObj* obj;
   UserObj_Data data;
   UserProxyPollCache *pollCache;

   pollCache = User_HeapAlloc(uci, sizeof *pollCache);
   if (!pollCache) {
      return NULL;
   }

   data.proxyInfo = User_HeapAlloc(uci, sizeof(*data.proxyInfo));
   if (!data.proxyInfo) {
      User_HeapFree(uci, pollCache);
      return NULL;
   }

   obj = User_HeapAlloc(uci, sizeof(UserObj));
   if (obj == NULL) {
      User_HeapFree(uci, pollCache);
      User_HeapFree(uci, data.proxyInfo);
      return NULL;
   }

   data.proxyInfo->pollCache = pollCache;
   obj->data = data;

   // XXX Mark this as preallocated and unusable somehow.

   return obj;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyObjFreePreallocated --
 *
 *	Frees a preallocated but now unneeded proxy object.
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
UserProxyObjFreePreallocated(User_CartelInfo* uci, UserObj* obj)
{
   // XXX assert that this is preallocated

   User_HeapFree(uci, obj->data.proxyInfo->pollCache);
   User_HeapFree(uci, obj->data.proxyInfo);
   User_HeapFree(uci, obj);
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyPollCacheForObj --
 *
 *	Returns the poll cache for the given object.
 *
 * Results:
 *	PollCache*
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static INLINE UserProxyPollCache*
UserProxyPollCacheForObj(UserObj *obj)
{
   ASSERT(obj != NULL);
   ASSERT(obj->type == USEROBJ_TYPE_PROXY_FILE ||
          obj->type == USEROBJ_TYPE_PROXY_FIFO ||
	  obj->type == USEROBJ_TYPE_PROXY_SOCKET ||
	  obj->type == USEROBJ_TYPE_PROXY_CHAR);
   ASSERT(obj->data.proxyInfo != NULL);
   ASSERT(obj->data.proxyInfo->pollCache != NULL);
   return obj->data.proxyInfo->pollCache;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyPollCacheLock --
 *
 *	Locks the given poll cache's lock.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static INLINE void
UserProxyPollCacheLock(UserProxyPollCache *pollCache)
{
   ASSERT(pollCache != NULL);
   SP_Lock(&pollCache->lock);
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyPollCacheUnlock --
 *
 *	Unlocks the given poll cache's lock.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static INLINE void
UserProxyPollCacheUnlock(UserProxyPollCache *pollCache)
{
   ASSERT(pollCache != NULL);
   SP_Unlock(&pollCache->lock);
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyPollCacheCreate --
 *
 *	Creates an initializes the poll cache.  Note that *pollCache
 *	should be allocated before the call to this function.  However,
 *	if pcHandle is valid, then the poll cache from the object with
 *	that handle is used instead of the one passed in.  In that case,
 *	the passed in pointer is freed and replaced with a pointer to
 *	the other object's poll cache.
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
UserProxyPollCacheCreate(User_CartelInfo *uci, uint32 pcHandle,
			 UserProxyPollCache **pollCache)
{
   Bool initializePC = TRUE;

   if (pcHandle != USERPROXY_INVALID_PCHANDLE) {
      VMK_ReturnStatus status;
      UserObj *pcObj;
      
      status = UserProxyObjFind(uci, pcHandle, &pcObj);
      if (status == VMK_OK) {
         User_HeapFree(uci, *pollCache);
	 *pollCache = pcObj->data.proxyInfo->pollCache;
	 UserObj_Release(uci, pcObj);
	 initializePC = FALSE;
      }
   }

   if (initializePC) {
      memset(*pollCache, 0, sizeof **pollCache);
      SP_InitLock("UserProxy Poll", &(*pollCache)->lock, UW_SP_RANK_POLLWAITERS);
      VMKPoll_InitList(&(*pollCache)->waiters, &(*pollCache)->lock);
   }

   UserProxyPollCacheLock(*pollCache);
   (*pollCache)->refCount++;
   UserProxyPollCacheUnlock(*pollCache);
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyPollCacheDestroy --
 *
 *	Decrements the refcount of this poll cache, and, if the
 *	refcount is zero, will destroy it.
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
UserProxyPollCacheDestroy(User_CartelInfo *uci, UserProxyPollCache *pollCache)
{
   Bool destroy = FALSE;

   UserProxyPollCacheLock(pollCache);
   pollCache->refCount--;
   ASSERT(pollCache->refCount >= 0);
   if (pollCache->refCount == 0) {
      destroy = TRUE;

      if (VMKPoll_HasWaiters(&pollCache->waiters)) {
	 UWWarn("waiters list not empty!");
      }
      VMKPoll_WakeupAndRemoveWaiters(&pollCache->waiters);
   }
   UserProxyPollCacheUnlock(pollCache);
   
   if (destroy) {
      SP_CleanupLock(&pollCache->lock);
      User_HeapFree(uci, pollCache);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyMakeFullName --
 *
 *	Make a '/' separated file path from the parent and arc
 *	components.
 *
 * Results:
 *	VMK_OK if the new path fits in given buffer, VMK_NAME_TOO_LONG
 *	if not.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserProxyMakeFullName(char* buf, int bufsize, const char* parent, const char* arc)
{
   int len;

   if (strcmp(parent, "/") == 0) {
      len = snprintf(buf, bufsize, "/%s", arc);
   } else {
      len = snprintf(buf, bufsize, "%s/%s", parent, arc);
   }
   if (len >= bufsize) {
      UWLOG(1, "Couldn't fit %s and %s into buf (%d bytes)",
            parent, arc, bufsize);
      return VMK_NAME_TOO_LONG;
   }
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyObjInit --
 *
 *	Fills in a new proxy object.  The length is used for
 *	specifying that a prefix of fullPath should be used.
 *
 * Results:
 *	VMK_OK if successful, VMK_NAME_TOO_LONG if name is too long.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserProxyObjInit(User_CartelInfo *uci, UserObj* obj, UserProxy_CartelInfo* upci,
		 UserObj_Type type, int fileHandle, char* fullPath, int length,
		 uint32 openFlags, int pcHandle)
{
   UserObj_Methods* methods;
   UserObj_Data data = obj->data;
   UserProxy_ObjInfo *info = obj->data.proxyInfo;
   int nlen;

   ASSERT(fullPath);
   ASSERT(length <= strlen(fullPath));

   info->upci = upci;
   info->fileHandle = fileHandle;

   if (length == 0) {
      info->fullPath[0] = '\0';
      nlen = 1;
   } else {
      /*
       * length + 1 to account for the null terminator.
       */
      nlen = snprintf(info->fullPath, MIN(sizeof info->fullPath, length + 1),
                      "%s", fullPath);
   }
   if (nlen >= sizeof(info->fullPath)) {
      return VMK_NAME_TOO_LONG;
   }

   UserProxyPollCacheCreate(uci, pcHandle, &info->pollCache);

   switch(type) {
      case USEROBJ_TYPE_PROXY_FILE:
	 methods = &proxyFileMethods;
	 break;
      case USEROBJ_TYPE_PROXY_FIFO:
         methods = &proxyFifoMethods;
	 break;
      case USEROBJ_TYPE_PROXY_CHAR:
         methods = &proxyCharMethods;
	 break;
      case USEROBJ_TYPE_PROXY_SOCKET:
	 methods = &proxySocketMethods;
	 break;
      case USEROBJ_TYPE_ROOT:
	 methods = &proxyRootMethods;
         break;
      default:
	 Panic("Unsupported proxy type: %d", type);
   }

   UserObj_InitObj(obj, type, data, methods, openFlags);

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyObjCreate --
 *
 *	Creates a new proxy object.  The length is used for specifying
 *	that a prefix of fullPath should be used.
 *
 * Results:
 *	VMK_OK if the UserObj was successfully created, obj is a valid
 *	UserObj.  Appropriate error code otherwise (obj will be
 *	invalid).
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserProxyObjCreate(User_CartelInfo* uci, UserProxy_CartelInfo* upci,
		   UserObj_Type type, int fileHandle, char* fullPath,
		   int length, UserObj** obj, uint32 openFlags, int pcHandle)
{
   VMK_ReturnStatus status;

   *obj = UserProxyObjPreallocate(uci);
   if (*obj == NULL) {
      return VMK_NO_MEMORY;
   }

   status = UserProxyObjInit(uci, *obj, upci, type, fileHandle, fullPath,
			     length, openFlags, pcHandle);
   if (status != VMK_OK) {
      UserProxyObjFreePreallocated(uci, *obj);
      *obj = NULL;
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyObjDestroy --
 *
 *	Destroys the given proxy object.
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
UserProxyObjDestroy(User_CartelInfo* uci, UserObj* obj)
{
   UserProxy_ObjInfo *info = obj->data.proxyInfo;
   UserProxyPollCache *pollCache = info->pollCache;
   info->pollCache = NULL;
   UserProxyPollCacheDestroy(uci, pollCache);
   User_HeapFree(uci, info);
   memset(&obj->data, 0, sizeof obj->data);
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyFindObj --
 *
 *	Finds a UserObj given its proxy fileHandle.
 *
 * Results:
 *	VMK_ReturnStatus.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserProxyObjFind(User_CartelInfo* uci, int fileHandle, UserObj** outObj)
{
   VMK_ReturnStatus status = VMK_INVALID_HANDLE;
   int fd;

   UserObj_FDLock(&uci->fdState);
   for (fd = 0; fd < USEROBJ_MAX_HANDLES; fd++) {
      if (uci->fdState.descriptors[fd] != NULL &&
          uci->fdState.descriptors[fd] != USEROBJ_RESERVED_HANDLE &&
          (uci->fdState.descriptors[fd]->type == USEROBJ_TYPE_PROXY_FILE ||
           uci->fdState.descriptors[fd]->type == USEROBJ_TYPE_PROXY_FIFO ||
           uci->fdState.descriptors[fd]->type == USEROBJ_TYPE_PROXY_SOCKET ||
           uci->fdState.descriptors[fd]->type == USEROBJ_TYPE_PROXY_CHAR ||
           uci->fdState.descriptors[fd]->type == USEROBJ_TYPE_ROOT) &&
	  uci->fdState.descriptors[fd]->data.proxyInfo != NULL &&
          uci->fdState.descriptors[fd]->data.proxyInfo->fileHandle == fileHandle) {
         *outObj = uci->fdState.descriptors[fd];
	 UserObj_Acquire(*outObj);
	 status = VMK_OK;
         UWSTAT_INSERT(proxyObjFindHitCt, fd);
	 break;
      }
   }
   UserObj_FDUnlock(&uci->fdState);

   if (status != VMK_OK) {
      UWSTAT_INC(proxyObjFindMissCt);
   }
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyKernelPollCacheUpdate --
 *
 *	Updates the given kernel poll cache, based on events received
 *	either from an asynchronous message or from a reply rpc message.
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
UserProxyKernelPollCacheUpdate(UserProxyPollCache *pollCache,
			       UserProxyPollCacheUpdate *pcUpdate)
{
   VMKPollEvent events = User_LinuxToVMKPollFlags(pcUpdate->events);

   UserProxyPollCacheLock(pollCache);
   if (pollCache->enabled) {
      /*
       * Use modular arithmetic here to solve the wraparound issue.
       */
      if ((int32)(pcUpdate->generation - pollCache->updateGeneration) > 0) {
	 UWLOG(2, "Updating poll events: %#x from linuxEvents: %#x for pc: %p",
	       events, pcUpdate->events, pollCache);
	 pollCache->cache = events;
	 pollCache->updateGeneration = pcUpdate->generation;
	 VMKPoll_WakeupAndRemoveWaitersForEvent(&pollCache->waiters, events);
      } else {
	 UWLOG(2, "Not updating poll events from linuxEvents: %#x for pc: %p  "
	       "id: %#x older than cur id: %#x", pcUpdate->events, pollCache,
	       pcUpdate->generation, pollCache->updateGeneration);
      }
   }
   UserProxyPollCacheUnlock(pollCache);
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyTranslateRpcStatus --
 *
 *	Converts a RPC status code to something more "normal".  The idea
 *	here is that we need to mask our use of a proxy to the UserWorld.
 *	So instead of returning random RPC error codes, we should return
 *	something the UserWorld might expect.
 *
 * Results:
 *	Converted status.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
UserProxyTranslateRpcStatus(VMK_ReturnStatus status)
{
   VMK_ReturnStatus newStatus = status;

   switch(status) {
      // Let these pass directly through.
      case VMK_OK:
      case VMK_NO_RESOURCES:
      case VMK_INVALID_ADDRESS:
      case VMK_WOULD_BLOCK:
      case VMK_WAIT_INTERRUPTED:
         break;
      // We should treat the following errors as if it's a bad file handle.
      case VMK_NOT_INITIALIZED:
      case VMK_NOT_FOUND:
      case VMK_IS_DISCONNECTED:
         newStatus = VMK_INVALID_HANDLE;
	 break;
      case VMK_LIMIT_EXCEEDED:
         // We should always handle this within UserProxySend and
	 // UserProxyReceive.
         Panic("Leaking VMK_LIMIT_EXCEEDED out of RPC code!");
	 break;
      default:
         // Warn if we hit something we didn't expect.
         Warning("Unexpected return status from RPC code: %#x: %s", status,
		 UWLOG_ReturnStatusToString(status));
	 DEBUG_ONLY(ASSERT(FALSE));
         break;
   }

   if (status != VMK_OK) {
      UWLOG(3, "status: %s (%#x) -> %s (%#x)", UWLOG_ReturnStatusToString(status),
	    status, UWLOG_ReturnStatusToString(newStatus), newStatus);
   }

   return newStatus;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyCopyIn --
 *
 *	Copies rpc data in to vmkernel or userworld memory.
 *
 * Results:
 *	VMK_OK on success, appropriate failure code otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserProxyCopyIn(void* dest, const void* src, uint32 length,
                Util_BufferType bufType)
{
   VMK_ReturnStatus status = VMK_OK;

   switch(bufType) {
      case UTIL_VMKERNEL_BUFFER:
         UWSTAT_INSERT(proxyCopyInVMK, length);
         memcpy(dest, src, length);
	 break;
      case UTIL_USERWORLD_BUFFER:
         UWSTAT_INSERT(proxyCopyInUser, length);
         status = User_CopyIn(dest, (UserVA)src, length);
	 break;
      default:
         NOT_IMPLEMENTED();
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyCopyOut --
 *
 *	Copies rpc data out to vmkernel or userworld memory.
 *
 * Results:
 *	VMK_OK on success, appropriate failure code otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserProxyCopyOut(void* dest, const void* src, uint32 length,
                 Util_BufferType bufType)
{
   VMK_ReturnStatus status = VMK_OK;

   switch(bufType) {
      case UTIL_VMKERNEL_BUFFER:
         UWSTAT_INSERT(proxyCopyOutVMK, length);
         memcpy(dest, src, length);
	 break;
      case UTIL_USERWORLD_BUFFER:
         UWSTAT_INSERT(proxyCopyOutUser, length);
         status = User_CopyOut((UserVA)dest, src, length);
	 break;
      default:
         NOT_IMPLEMENTED();
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyCheckedSend --
 *
 *	Calls RPC send with the given data.  If we're hit the limit of
 *	queued messages, it will wait and try again until it succeeds.
 *
 *	See also UserProxyCheckedGetReply
 *
 * Results:
 *	VMK_OK if the message was sent successfully, appropriate failure code
 *	otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserProxyCheckedSend(UserProxy_CartelInfo* upci,
                     UserProxyFunctions fn,
                     int flags,
                     char* msg,
                     uint32 size,
                     Util_BufferType bufType,
                     RPC_Token* token)
{
   int loopCt = 0;
   VMK_ReturnStatus status;
   Timer_AbsCycles startTime = -1;

   if (upci->disconnected) {
      UWLOG(0, "Prior disconnection.");
      return VMK_IS_DISCONNECTED;
   }

   status = RPC_Send(upci->cnxToProxyID, fn, flags, msg, size, bufType, token);
   while (status == VMK_LIMIT_EXCEEDED) {
      int sleepMS;
      Timer_AbsCycles now = Timer_GetCycles();

      /*
       * Note, this notion of timeout is incomplete.  It does not cover
       * timeouts during RPC_Send (i.e., if the RPC queue isn't full).
       * That will either be fixed with conduits or with a patch to rpc.
       */
      if (startTime == -1) {
         startTime = now;
      } else if (Timer_TCToMS(startTime - now) > 1500) {
         // 1.5min is plenty.  Give up.  This is stupendously bad.
         UWWarn("Giving up.  Forcing disconnection from proxy.");
         UserProxyForceDisconnect(upci);
         return VMK_TIMEOUT;
      }

      sleepMS = MIN((loopCt * 2) + 1,
                    USERPROXY_SLEEP_BEFORE_RETRY_MAX);
      loopCt++;

      /*
       * We are trying to queue up too many rpc messages.  So we have to hold
       * off momentarily.
       */
      UWLOG(1, "Too many RPC messages in queue, sleeping %d ms.",
            sleepMS);
      UWSTAT_ADD(proxyRPCSleepMs, sleepMS);
      status = CpuSched_Sleep(sleepMS);
      if (status != VMK_OK) {
         ASSERT(status != VMK_LIMIT_EXCEEDED);
         UWLOG(0, "CpuSched_Sleep(%d): %s.", sleepMS, UWLOG_ReturnStatusToString(status));
      } else {
         if (upci->disconnected) {
            UWLOG(0, "Prior disconnection.");
            status =  VMK_IS_DISCONNECTED;
         } else {
            status = RPC_Send(upci->cnxToProxyID, fn, flags,
                              msg, size, bufType, token);
            if (status == VMK_IS_DISCONNECTED) {
               UserProxyForceDisconnect(upci);
            }
         }
      }
   }

   if (status != VMK_OK) {
      UWLOG(0, "send failed: %s", UWLOG_ReturnStatusToString(status));
   }

   UWSTAT_INSERT(proxyRPCSendLoopCt, loopCt);
   UWSTAT_ARRADD(proxyBytesSent, fn, size);

   return UserProxyTranslateRpcStatus(status);
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxySend --
 *
 *	Sends a message with data to the proxy.
 *
 *	There are two kinds of sending that we're really concerned with here:
 *	   - sending some struct
 *	   - sending a struct along with a user buffer (ie, a write)
 *
 *	In the first case, the struct is already in the kernel's address space,
 *	so we don't need to copy it, instead, we just pass a pointer to RPC_Send
 *	(which, sadly, will always copy it).
 *
 *	In the second case, we have the struct in memory, but not the user
 *	buffer.  RPC_Send is prepared to handle copying in a user buffer for us,
 *	but we have to take care of the initial case, when we want to send the
 *	struct and (if there's room left) some of the user buffer.  Thus we need
 *	a temporary buffer, which is where firstMsg comes in.  We copy the
 *	struct and as much of the user buffer as we can.  For subsequent sends,
 *	all we're doing is copying from user space, so we let RPC handle that
 *	for us.
 *
 * Results:
 *	VMK_OK if the message was sent successfully, appropriate failure code
 *	otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserProxySend(UserProxyFunctions fn,
	      UserProxy_CartelInfo* upci,
	      UserProxyMsgHdr* msgHdr,
	      uint32 msgHdrLen,
	      Bool hasData,
	      void* msgData,
	      Util_BufferType bufType,
	      RPC_Token* token)
{
   User_CartelInfo *uci = UserProxyUCIForUPCI(upci);
   VMK_ReturnStatus status = VMK_OK;
   uint32 chunkSize = RPC_MAX_MSG_LENGTH;
   uint32 curRpcSize;
   uint32 offset;
   uint32 msgLen;
   char *firstMsg = NULL;
   char *curRpcMsg;
   UWSTAT_ONLY(unsigned fragCount = 0);

   if ((status = UserProxyVerifyConnection(upci)) != VMK_OK) {
      return status;
   }

   UWSTAT_ARRINC(proxySyscallCount, fn);

   msgLen = msgHdr->size;

   if (hasData) {
      int toCopy = MIN(RPC_MAX_MSG_LENGTH - msgHdrLen, msgLen - msgHdrLen);

      firstMsg = User_HeapAlloc(uci, RPC_MAX_MSG_LENGTH);
      if (firstMsg == NULL) {
         UWLOG(0, "Failed to allocate memory for RPC message.\n");
	 status = VMK_NO_MEMORY;
	 goto errorExit;
      }

      /*
       * It's OK to ASSERT this since callers should be checking to ensure they
       * don't call us with a zero-length buffer.
       */
      ASSERT(toCopy > 0);

      ASSERT(msgHdrLen < RPC_MAX_MSG_LENGTH);
      memcpy(firstMsg, msgHdr, msgHdrLen);

      status = UserProxyCopyIn(firstMsg + msgHdrLen, msgData, toCopy, bufType);
      if (status != VMK_OK) {
         UWLOG(0, "UserProxyCopyIn failed: %s",
	       UWLOG_ReturnStatusToString(status));
	 goto errorExit;
      }

      msgData += toCopy;
      curRpcMsg = firstMsg;
      curRpcSize = MIN(RPC_MAX_MSG_LENGTH, msgLen);
   } else {
      ASSERT(msgLen == msgHdrLen);
      curRpcMsg = (char*)msgHdr;
      curRpcSize = MIN(chunkSize, msgLen);
   }

   /*
    * Wait until current sender is done.
    *
    * NOTE: We need to take a lock here because otherwise we leave ourselves
    * open to a race on the proxy side.  We need to ensure that all of our
    * rpc's arrive at the proxy contiguously (and in order of course).  However,
    * since we can't put token numbers on rpc's after the first one (and even if
    * we could it'd be a lot of work to create a recv queue on the proxy side),
    * we need to control how messages are sent from the kernel side.  Thus, we
    * turn sendInProgress to TRUE while we're sending so no other world in our
    * cartel can send while we are.
    */
   Semaphore_Lock(&upci->sema);

   UWSTAT_ONLY(fragCount++);
   status = UserProxyCheckedSend(upci, fn, RPC_REPLY_EXPECTED,
				 curRpcMsg, curRpcSize, UTIL_VMKERNEL_BUFFER,
				 token);
   if (status != VMK_OK) {
      /*
       * Nothing got sent, so don't need to involve the proxy in the
       * cleanup.
       */
      goto errorExitLock;
   }

   if (hasData) {
       curRpcMsg = (char*)msgData;
   } else {
      curRpcMsg += curRpcSize;
   }

   for (offset = curRpcSize; offset < msgLen; offset += curRpcSize) {
      RPC_Token fragToken = USERPROXY_RPCTOKEN_FRAGMENT;

      curRpcSize = MIN(chunkSize, msgLen - offset);

      UWSTAT_ONLY(fragCount++);
      status = UserProxyCheckedSend(upci, fn, RPC_FORCE_TOKEN,
                                    curRpcMsg, curRpcSize, bufType,
                                    &fragToken);
      ASSERT(fragToken == USERPROXY_RPCTOKEN_FRAGMENT);
      if (status != VMK_OK) {
         goto errorExitFlush;
      }

      curRpcMsg += curRpcSize;
   }

   UWLOG(2, "OK");

  errorExitFlush:
   if (status != VMK_OK) {
      if (offset != msgLen) {
         VMK_ReturnStatus errStatus;
         RPC_Token errorToken = USERPROXY_RPCTOKEN_ERROR;
	 char tmpMsg[1];

         UWSTAT_ONLY(fragCount++);

	 /*
	  * Not sure if firstMsg has been allocated or not, so just use a temp
	  * message.  The contents of the message are unimportant, only the
	  * token is important.
	  */
         tmpMsg[0] = 0;
         errStatus = UserProxyCheckedSend(upci, fn, RPC_FORCE_TOKEN,
                                          tmpMsg, 1, UTIL_VMKERNEL_BUFFER,
                                          &errorToken);
         ASSERT(errorToken == USERPROXY_RPCTOKEN_ERROR);
         if (errStatus != VMK_OK) {
            UWWarn("Error sending error msg: %s",
                   UWLOG_ReturnStatusToString(errStatus));
         }
      }
   } else {
      /* VMK_OK requires perfection: */
      ASSERT(offset == msgLen);
   }

  errorExitLock:
   UWSTAT_INSERT(proxyRPCsPerMessage, fragCount);
   Semaphore_Unlock(&upci->sema);

  errorExit:
   if (firstMsg != NULL) {
      User_HeapFree(uci, firstMsg);
   }
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxySendCancel --
 *
 *	Directly send a cancel message to the proxy.  Do not wait for a
 *	reply.  The proxy will reply to the message we're canceling
 *	(either with a valid reply or with VMK_WAIT_INTERRUPTED).
 *
 * Results:
 *	Result of UserProxySend.
 *
 * Side effects:
 *	Message sent.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserProxySendCancel(UserProxy_CartelInfo* upci,
                    RPC_Token token)
{
   static const Bool hasData = FALSE;
   UserProxyCancelMsg msg;
   RPC_Token cancelMsgToken;
      
   msg.token = token;
   msg.hdr.size = sizeof msg;

   UWSTAT_INC(proxyCancelMsgCt);

   UWLOG(1, "token=%d", token);

   return UserProxySend(USERPROXY_CANCEL, upci, &msg.hdr, sizeof msg,
                        hasData, NULL, UTIL_VMKERNEL_BUFFER, &cancelMsgToken);

   /*
    * The CANCEL message is unique in that there is no reply sent.  The
    * "reply" will be to the original message (the one matching token).
    */
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyUsefulInterruption --
 *
 *	Test to see if an interrupted wait was "useful".  There are lots
 *	of (useless) spurious wakeups on RPC connections, so this test
 *	determines if the current thread has a pending signal or is dead.
 *
 * Results:
 *	TRUE if interruption is useful, FALSE if not
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static INLINE Bool
UserProxyUsefulInterruption(World_Handle* currWorld)
{
   ASSERT(currWorld != NULL);
   ASSERT(World_IsUSERWorld(currWorld)); // implied by caller's setup
   
   if (World_IsUSERWorld(currWorld)) {
      User_ThreadInfo* uti = currWorld->userThreadInfo;
      const Bool allowed = uti->signals.pendingBit || uti->dead;
      if (allowed) {
         UWLOG(2, "allowing interruption: %s%s",
               uti->dead ? "dead " : "",
               uti->signals.pendingBit ? "sig" : "");
      }
      return allowed;
   }

   return FALSE;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyCheckedGetReply --
 *
 *	Get a single "reply" packet associated with the 'token' message on
 *	the to-kernel RPC connection.  If there is a fatal error in the
 *	RPC connection, the proxy connection is destroyed.
 *
 *	May block.  If 'interruptible' flag is TRUE, will bail out on
 *	spurious interruptions.
 *
 *	See UserProxyCheckedSend
 *
 * Results:
 *	VMK_OK if the message was sent successfully, appropriate failure code
 *	otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserProxyCheckedGetReply(UserProxy_CartelInfo* upci,
                         RPC_Token token,
                         char* curRpcMsg,
                         uint32* curRpcSize,
                         Util_BufferType bufType,
                         Bool interruptible)
{
   VMK_ReturnStatus status;
   int rpcFlags = RPC_CAN_BLOCK;
   World_Handle* const currWorld = MY_RUNNING_WORLD;

   ASSERT(curRpcMsg);
   ASSERT(*curRpcSize <= RPC_MAX_MSG_LENGTH);

   UWLOG(3, "token=%d, interruptible=%d", token, interruptible);

   if (upci->cnxToKernelID == -1) {
      return VMK_IS_DISCONNECTED;
   }

   /*
    * If we allow interruptions on RPC_GetReply, we'll probably get a lot
    * of spurious wakeups (from other threads in the cartel using the same
    * RPC cnx).  So, we check here if there is a good reason for this
    * world to be interrupted (if its got a pending signal or its dead).
    *
    * Doing this check is a bit ugly because some RPCs are done by a
    * helper world (at cleanup time).  These are implicitly
    * uninterruptible.
    */

   if (interruptible && World_IsUSERWorld(currWorld)) {
      rpcFlags |= RPC_ALLOW_INTERRUPTIONS;
   }

   do {
      status = RPC_GetReply(upci->cnxToKernelID, token, rpcFlags,
                            curRpcMsg, curRpcSize, bufType, INVALID_WORLD_ID);
   } while (status == VMK_WAIT_INTERRUPTED
            && ! UserProxyUsefulInterruption(currWorld));

   if (status == VMK_IS_DISCONNECTED) {
      UserProxyForceDisconnect(upci);
   }

   return UserProxyTranslateRpcStatus(status);
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyReceive --
 *
 *	Blocks waiting for a message from the proxy.  A "message" can
 *	be composed of multiple RPC buffers.
 *
 *	As with sending, there are two kinds of replies that we can get:
 *	   - just a struct
 *	   - a struct header with a payload
 *
 *	Again, in the first case, we're reading this into kernel memory.  So we
 *	don't need a temporary buffer.
 *
 *	In the second case, we need a temporary buffer so that we can copy in
 *	both the reply header as well as the first part of the data.  After
 *	that, we can copy directly to a supplied buffer.
 *
 * Results:
 *	VMK_OK is a message was received successfully, appropriate
 *	failure code otherwise.
 *
 * Side effects:
 *	replyData, if provided, will be filled with the message's data.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserProxyReceive(UserProxyFunctions fn,
                 UserProxy_CartelInfo *upci,
		 RPC_Token token,
		 UserProxyReplyHdr *replyHdr,
		 uint32 replyHdrLen,
		 Bool hasData,
		 void* replyData, // XXX may be a UserVA
                 Util_BufferType bufType)
{
   VMK_ReturnStatus status;
   User_CartelInfo *uci = UserProxyUCIForUPCI(upci);
   uint32 offset;
   char *firstMsg = NULL;
   char *curRpcMsg; // XXX may be a UserVA
   uint32 curRpcSize;
   uint32 replyLen;
   UserProxyReplyHdr *tmpHdr;
   Bool interruptible = TRUE;

   ASSERT(replyHdr != NULL);
   ASSERT(replyHdrLen >= sizeof(UserProxyReplyHdr));

   if ((status = UserProxyVerifyConnection(upci)) != VMK_OK) {
      return status;
   }

   UWLOG(3, "Getting reply (fn=%d, token=%d hasData=%s/replyHdrLen=%u)",
         fn, token, hasData?"yes":"no", replyHdrLen);

   /*
    * Always allocate space for firstMsg, even if we never need to use it.
    * In the case that this message has data, we'll definitely need it.  However
    * even if the message doesn't have data, in case there's an error, we'll
    * need to drain the RPC queue and thus firstMsg will be required as a
    * temporary buffer.  We don't want to try and allocate it later when we
    * realize there's an error, because if we fail to allocate memory then we'll
    * be leaving data in the RPC queue.  So it's safest to allocate now.
    */
   firstMsg = User_HeapAlloc(uci, RPC_MAX_MSG_LENGTH);
   if (firstMsg == NULL) {
      return VMK_NO_MEMORY;
   }

   if (hasData) {
      curRpcMsg = firstMsg;
      curRpcSize = RPC_MAX_MSG_LENGTH;
   } else {
      ASSERT((VA)replyHdr < VMK_VA_END); // Must be valid vmkernel addr
      curRpcMsg = (char*)replyHdr;
      curRpcSize = replyHdrLen;
   }

   /*
    * Get the first message in the reply.  If we get interrupted, send an
    * interrupt request to the proxy.  If the request is interruptible
    * (i.e. a reply isn't already pending), the proxy will cleanup the
    * request and send a regular reply.
    */
   status = UserProxyCheckedGetReply(upci, token, curRpcMsg, &curRpcSize,
                                     UTIL_VMKERNEL_BUFFER, interruptible);
   if (status == VMK_WAIT_INTERRUPTED) {
      ASSERT(interruptible == TRUE);
      interruptible = FALSE;

      UWLOG(2, "CheckedGetReply(cnx=%d tok=%d ...) fn=%d interrupted",
            upci->cnxToKernelID, token, fn);

      status = UserProxySendCancel(upci, token);

      /*
       * Ignore non-OK status from UserProxySendCancel.  It won't change
       * my behavior -- the only failure is a total RPC disconnect
       * failure, which will also be hit in the following receive, and
       * cause us to bail out entirely.
       */
      UWLOG((status == VMK_OK) ? 3 : 0, "UserProxySendCancel: %s",
            UWLOG_ReturnStatusToString(status));

      /* Wait for the first reply for cancel or actual result */
      status = UserProxyCheckedGetReply(upci, token, curRpcMsg, &curRpcSize,
                                        UTIL_VMKERNEL_BUFFER, interruptible);
      ASSERT(status != VMK_WAIT_INTERRUPTED);
   }

   if (status != VMK_OK) {
      UWLOG(0, "Failed to get first chunk of reply: %#x", status);
      goto done;
   }

   tmpHdr = (UserProxyReplyHdr*)curRpcMsg;
   replyLen = tmpHdr->size;

   // All replies must be at least as big as the common header
   if (replyLen < sizeof(UserProxyReplyHdr)) {
      UWLOG(0, "reply->size: %d < UserProxyReplyHdr (%d)!",
            replyLen, sizeof(UserProxyReplyHdr));
      status = VMK_BAD_PARAM;
      goto done;
   }

   /*
    * Check if we hit a "severe error".  An error is severe if it prevents the
    * proxy from returning a full reply message for the given syscall.  This
    * could be because the proxy runs out of memory and is not able to allocate
    * enough space for the reply message.
    *
    * Because the error message is only sizeof(UserProxyReplyHdr), no other
    * information that normally accompanies an error message is transferred
    * (such as poll cache update information).  So, technically we should treat
    * this like an error in receiving a message from RPC_GetReply.  The
    * reasoning is that all callers understand that if RPC_GetReply fails, none
    * of the data in the reply can be trusted.  The same situation applies here.
    */
   if (tmpHdr->status & USERPROXY_SEVERE_ERROR) {
      UWWarn("Severe error encountered when receiving for function %d.", fn);
      ASSERT((tmpHdr->status & ~USERPROXY_SEVERE_ERROR) != VMK_OK);
      status = tmpHdr->status & ~USERPROXY_SEVERE_ERROR;
      goto done;
   }

   /* Assume the copy goes okay */
   status = VMK_OK;

   /* Split first message into local and 'replyData' pieces */
   if (hasData) {
      int toCopy = MIN(curRpcSize, replyHdrLen);
      int toCopyOut = curRpcSize - replyHdrLen;

      ASSERT(replyHdrLen < RPC_MAX_MSG_LENGTH);
      memcpy(replyHdr, curRpcMsg, toCopy);
      
      if (toCopyOut > 0) {
	 status = UserProxyCopyOut(replyData, curRpcMsg + replyHdrLen,
				   toCopyOut, bufType);
	 if (status != VMK_OK) {
	    UWLOG(0, "UserProxyCopyOut(%p, %d bytes, %s) failed: %s",
                  replyData, toCopyOut,
                  (bufType == UTIL_VMKERNEL_BUFFER ? "vmk buf" :
                   (bufType == UTIL_USERWORLD_BUFFER ? "user buf" :
                    (bufType == UTIL_HOST_BUFFER ? "host buf" :
                     "UNKNOWN"))),
                  UWLOG_ReturnStatusToString(status));
            /*
             * We have to drain the rest of the RPC queue, so don't
             * return just yet.  Fall through and clean out the RPC
             * backlog.
             */
	 }
      }

      curRpcMsg = (char*)replyData + toCopyOut;
   } else {
      curRpcMsg += curRpcSize;
   }

   /* Either read everything we need, or read a full rpc buffer's worth. */
   ASSERT((curRpcSize == replyLen) || (curRpcSize == RPC_MAX_MSG_LENGTH));

   /*
    * Read in the rest of the reply (if there is any).
    */
   for (offset = curRpcSize; offset < replyLen; offset += curRpcSize) {
      VMK_ReturnStatus fragStatus;

      /*
       * If there was an error copying out, direct any further copies
       * into a throw-away buffer.  
       */
      if (status != VMK_OK) {
         UWLOG(3, "Redirecting next rpc msg to %p", firstMsg);
         curRpcMsg = firstMsg;
         bufType = UTIL_VMKERNEL_BUFFER;
	 curRpcSize = RPC_MAX_MSG_LENGTH;
      } else {
         curRpcSize = MIN(RPC_MAX_MSG_LENGTH, replyLen - offset);
      }

      /*
       * At this point, we will uninterruptibly block until we get our
       * complete message or the RPC connection is destroyed.  Any
       * incoming signals or termination requests will be postponed.
       * However, since the proxy already started this message (we got the
       * first part of it above), it should relatively quickly finish the
       * entire message.
       */
      fragStatus = UserProxyCheckedGetReply(upci, token,
                                            curRpcMsg, &curRpcSize, bufType,
                                            FALSE);
      if (status != VMK_OK) {
         /*
	  * If status is not VMK_OK, that means we've switched into "drain RPC
	  * queue" mode.  We're now just passing in a kernel buffer whose size
	  * should always be RPC_MAX_MSG_LENGTH.  Thus, we should never see
	  * either of these errors.
	  */
         ASSERT(fragStatus != VMK_INVALID_ADDRESS);
	 ASSERT(fragStatus != VMK_NO_RESOURCES);
      }

      if (fragStatus != VMK_OK) {
	 ASSERT(fragStatus != VMK_WOULD_BLOCK);

         // Don't mask earlier failures with new one.
         if (status == VMK_OK) {
            status = fragStatus;
         }

	 /*
	  * If we hit either of these errors, the RPC connection is completely
	  * hosed, so just bail immediately.
	  */
	 if (fragStatus == VMK_NOT_FOUND || fragStatus == VMK_IS_DISCONNECTED) {
	    offset = replyLen;
	 }

	 curRpcSize = 0;
      }

      curRpcMsg += curRpcSize;
   }

   ASSERT(offset == replyLen); // no overshooting
   UWSTAT_ARRADD(proxyBytesRecv, fn, replyLen);

   if (status == VMK_OK) {
      /*
       * The proxy always swallows SIGPIPE, so we have to generate a
       * SIGPIPE here, if one is warranted.
       *
       * XXX this should probably be done somewhere else.
       */
      if (replyHdr->status == VMK_WrapLinuxError(LINUX_EPIPE)) {
         VMK_ReturnStatus sigStatus;
         sigStatus = UserSig_LookupAndSend(MY_RUNNING_WORLD->worldID,
                                           LINUX_SIGPIPE, TRUE);
         /* Only fails if the given worldID is bad, by definition its good: */
         ASSERT(sigStatus == VMK_OK);
      }
   }

done:
   if (firstMsg != NULL) {
      User_HeapFree(uci, firstMsg);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyRemoteCall --
 *
 *      Sends a message to the proxy, and blocks waiting for a reply.
 *	Used for sending only fixed-length requests, that expect
 *	fixed-length replies (i.e., not read or write).
 *
 * Results:
 *	If the RPC failed, then the failure code is returned.  Otherwise, the
 *	status from the reply header is returned.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserProxyRemoteCall(UserProxyFunctions fn, UserProxy_CartelInfo *upci,
		  UserProxyMsgHdr *msg, uint32 msgLen,
		  UserProxyReplyHdr *reply, uint32 replyLen)
{
   VMK_ReturnStatus status;
   RPC_Token token; // token for find reply associated with our request

   msg->size = msgLen;
   UWSTAT_TIMERSTART(proxyCallTime);
   status = UserProxySend(fn, upci, msg, msgLen, FALSE, NULL,
			  UTIL_VMKERNEL_BUFFER, &token);
   if (status != VMK_OK) {
      UWLOG(0, "Failed to send message to proxy.");
      return status;
   }

   status = UserProxyReceive(fn, upci, token, reply, replyLen, FALSE, NULL,
			     UTIL_VMKERNEL_BUFFER);
   UWSTAT_TIMERSTOP(proxyCallTime);
   if (status != VMK_OK) {
      UWLOG(0, "Failed to receive message from proxy.");
      return status;
   }

   if (reply->status != VMK_OK && reply->status != VMK_IS_A_SYMLINK &&
       reply->status != VMK_WOULD_BLOCK) {
      UWLOG(1, "RPC succeeded, request failed: status = %s (%#x)",
            UWLOG_ReturnStatusToString(reply->status), reply->status); 
   } else {
      UWLOG(1, "RPC succeeded, request succeeded: status = %s (%#x)",
            UWLOG_ReturnStatusToString(reply->status), reply->status);
   }

   return reply->status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxy_SendStatusAlert --
 *
 *	Sends the proxy a one-way status alert.  Status alerts must
 *	fit in a single RPC_MAX_MSG_LENGTH buffer.  The first word of
 *	msg must be a User_MessageType.
 *
 * Results:
 *      Status of the RPC call.
 *
 * Side effects:
 *      A message is sent.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserProxy_SendStatusAlert(World_ID cartelID, const void* msg, int length)
{
   VMK_ReturnStatus status;
   RPC_Connection cnxID;
   char cnxName[20];
   LOG_ONLY(int msgType = *(User_MessageType*)msg);

   ASSERT(msg != NULL);
   ASSERT(length < RPC_MAX_MSG_LENGTH);

   snprintf(cnxName, sizeof cnxName, "Status.%d", cartelID);

   LOG(1, "Sending proxy status message of type %d", msgType);

   status = RPC_Connect(cnxName, &cnxID);
   if (status == VMK_OK) {
      RPC_Token token;
      status = RPC_Send(cnxID, 0, 0, msg, length, 
                        UTIL_VMKERNEL_BUFFER, &token);
      if (status != VMK_OK) {
	 LOG(0, "(msgType=%d) RPC_Send returned: %s", msgType,
             UWLOG_ReturnStatusToString(status));
      }
      RPC_Disconnect(cnxID);
   } else {
      LOG(0, "(msgType=%d) RPC_Connect returned :%s", msgType, 
          UWLOG_ReturnStatusToString(status));
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyMakeParentName --
 *
 *      Make the absolute pathname of the parent of "name" by chopping
 *      off the last path element.
 *
 * Results:
 *      VMK_OK or error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus 
UserProxyMakeParentName(char* buf, int bufsize, const char* name)
{
   char *slashPos;
   int len;

   slashPos = strrchr(name, '/');
   if (!slashPos) {
      ASSERT(FALSE);
      return VMK_NOT_FOUND;
   }
   if (slashPos == name) {
      // Don't remove leading "/"
      slashPos++;
   }
   len = snprintf(buf, bufsize, "%.*s", slashPos - name, name);
   if (len >= bufsize) {
      return VMK_NAME_TOO_LONG;
   }
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyOpen --
 *
 *      Open the specified arc relative to the specified directory and
 *      return a new UserObj.
 *
 * Results:
 *      VMK_OK if the object can be found, error condition otherwise
 *
 * Side effects:
 *      A new file may be created.
 *      UserObj for the object is allocated with refcount = 1.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserProxyOpen(UserObj* parent, const char* arc,
              uint32 flags, LinuxMode mode, UserObj** objOut)
{
   UserProxy_ObjInfo *parentInfo = parent->data.proxyInfo;
   User_CartelInfo* uci = UserProxyUCIForUPCI(parentInfo->upci);
   VMK_ReturnStatus status = VMK_OK;
   UserProxyOpenReply reply;
   UserProxyOpenMsg msg;

   ASSERT(parent->type == USEROBJ_TYPE_PROXY_FILE ||
          parent->type == USEROBJ_TYPE_ROOT);
   UWLOG(1, "(path='%s', arc='%s', flags=%#x, mode=%#x)", parentInfo->fullPath, arc, flags, mode);

   *objOut = NULL;

   if (strcmp(arc, "..") == 0) {
      // Strip off last component to get grandparent's name
      status = UserProxyMakeParentName(msg.name, sizeof msg.name,
                                       parentInfo->fullPath);
      
   } else if (strcmp(arc, ".") == 0 || strcmp(arc, "") == 0) {
      // Use parent's name unchanged
      int len = snprintf(msg.name, sizeof msg.name, "%s",
                         parentInfo->fullPath);
      if (len >= sizeof msg.name) {
         status = VMK_NAME_TOO_LONG;
      }

   } else {
      status = UserProxyMakeFullName(msg.name, sizeof msg.name,
                                     parentInfo->fullPath, arc);
   }
   if (status != VMK_OK) {
      return status;
   }
   UWLOG(2, "%s + %s = %s", parentInfo->fullPath, arc, msg.name);

   /*
    * Warn about accesses to the proxied /proc.  Many of these /proc files
    * give bogus information (i.e., either COS-specific or about the
    * proxy, not the app).
    */
   if (vmx86_debug && (strncmp(msg.name, "/proc/self", strlen("/proc/self")) == 0)) {
      UWWarn("Accessing COS /proc/self node: %s", msg.name);
   }

   msg.flags = flags;
   msg.mode = mode;
   status = UserProxyRemoteCall(USERPROXY_OPEN, &uci->proxy,
                             &msg.hdr, sizeof(msg),
                             &reply.hdr, sizeof(reply));

   if (status == VMK_OK) {
      switch(reply.type) {
         case USEROBJ_TYPE_PROXY_FILE:
         case USEROBJ_TYPE_PROXY_FIFO:
         case USEROBJ_TYPE_PROXY_SOCKET:
         case USEROBJ_TYPE_PROXY_CHAR:
	    break;
	 default:
	    status = VMK_BAD_PARAM;
	    break;
      }

      if (status == VMK_OK) {
	 status = UserProxyObjCreate(uci, &uci->proxy, reply.type,
				     reply.fileHandle, msg.name,
				     strlen(msg.name), objOut, flags,
				     reply.pcHandle);
      }
   }

   UWLOG(1, "arc=%s, status = %#x, obj = %p", arc, status, *objOut);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyClose --
 *
 *      Close the underlying file handle in obj.   Must not be null.
 *
 * Results:
 *      VMK_OK or error status from the underlying io object.
 *
 * Side effects:
 *      The file handle is closed.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserProxyClose(UserObj* obj, User_CartelInfo* uci)
{
   UserProxy_ObjInfo *info = obj->data.proxyInfo;
   VMK_ReturnStatus status = VMK_OK;
   UserProxyCloseMsg msg;
   UserProxyCloseReply reply;

   ASSERT(obj != NULL);
   ASSERT(obj->type == USEROBJ_TYPE_PROXY_FILE ||
	  obj->type == USEROBJ_TYPE_PROXY_FIFO ||
	  obj->type == USEROBJ_TYPE_PROXY_SOCKET ||
	  obj->type == USEROBJ_TYPE_PROXY_CHAR ||
          obj->type == USEROBJ_TYPE_ROOT);
   ASSERT(info != NULL);
   ASSERT(uci == UserProxyUCIForUPCI(info->upci));

   UWLOG(2, "cnxToProxy = %d, fh = %d, fp = '%s'", info->upci->cnxToProxyID,
	 info->fileHandle, info->fullPath);

   if (info->fileHandle != USERPROXY_INVALID_FD) {
      msg.fileHandle = info->fileHandle;
      status = UserProxyRemoteCall(USERPROXY_CLOSE, info->upci, &msg.hdr,
				sizeof(msg), &reply, sizeof(reply));
      UWLOG(3, "status = %#x", status);
   }

   UserProxyObjDestroy(uci, obj);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyRead --
 *
 *      Read up to length bytes at the specified offset in the given
 *	file.  Sets bytesRead to number of bytes actually
 * 	read.  BytesRead is undefined if an error is returned.
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

static VMK_ReturnStatus
UserProxyRead(UserObj* obj,
              UserVA userData,
              uint64 offset,
              uint32 length,
              uint32 *bytesRead)
{
   return UserProxyReadInt(obj, (uint8*)userData, offset, length, bytesRead,
                           UTIL_USERWORLD_BUFFER);
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyReadMPN --
 *
 *      Read up to PAGE_SIZE bytes at the specified offset from the
 *	given file into the given MPN.  If less than PAGE_SIZE are
 *	read in, they're left untouched.
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

static VMK_ReturnStatus
UserProxyReadMPN(UserObj* obj, MPN mpn,
                 uint64 offset, uint32 *bytesRead)
{
   VMK_ReturnStatus status;
   uint8* data;
   data = KVMap_MapMPN(mpn, TLB_LOCALONLY);
   if (data == NULL) {
      status =  VMK_NO_ADDRESS_SPACE;
   } else {
      int toRead = PAGE_SIZE;
      uint32 partialBytesRead = 0;

      *bytesRead = 0;

      /*
       * Need to make sure partial reads really mean EOF.  This means
       * partial pages are always read from twice...
       *
       * If the remote object is not a file (somehow its a socket or a
       * pipe), this could take quite a while....
       */
      do {
         status = UserProxyReadInt(obj, data/* + partialBytesRead*/, offset,
				   toRead, &partialBytesRead,
                                   UTIL_VMKERNEL_BUFFER);
         toRead -= partialBytesRead;
         offset += partialBytesRead;
         *bytesRead += partialBytesRead;
      } while ((status == VMK_OK)
               && (toRead > 0)
               && (partialBytesRead > 0));
      
      KVMap_FreePages(data);
   }
   return status;
}



/*
 *----------------------------------------------------------------------
 *
 * UserProxyReadInt --
 *
 *	Internal read function.  Reads up to 'length' bytes at
 *	'offset' from the given proxy object.  Copies resulting data
 *	into 'userData'.
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

static VMK_ReturnStatus
UserProxyReadInt(UserObj* obj,
                 void* userData,
                 uint64 offset,
                 uint32 length,
                 uint32 *bytesRead,
		 Util_BufferType bufType)
{
   UserProxy_ObjInfo *info = obj->data.proxyInfo;
   VMK_ReturnStatus status;
   UserProxyReadReply reply;
   UserProxyReadMsg msg;
   RPC_Token token;

   msg.varHdr.fdHdr.hdr.size = sizeof msg;
   msg.varHdr.fdHdr.fileHandle = info->fileHandle;
   msg.varHdr.dataSize = length;
   msg.offset = offset;

   UWLOG(3, "Reading %d bytes at offset %"FMT64"d from file %s",
       length, offset, info->fullPath);

   UWSTAT_TIMERSTART(proxyCallTime);
   status = UserProxySend(USERPROXY_READ, info->upci, &msg.varHdr.fdHdr.hdr,
			  sizeof msg, FALSE, 0, UTIL_VMKERNEL_BUFFER, &token);
   if (status != VMK_OK) {
      UWLOG(0, "Failed to send message to proxy.");
      return status;
   }

   status = UserProxyReceive(USERPROXY_READ, info->upci, token, &reply.pcHdr.hdr,
                             sizeof reply, TRUE, userData, bufType);
   UWSTAT_TIMERSTOP(proxyCallTime);
   if (status != VMK_OK) {
      UWLOG(0, "Failed to receive message from proxy.");
      return status;
   }

   UserProxyKernelPollCacheUpdate(UserProxyPollCacheForObj(obj),
                                  &reply.pcHdr.pcUpdate);

   if (reply.pcHdr.hdr.status != VMK_OK) {
      UWLOG(1, "Request failed: %s",
	    UWLOG_ReturnStatusToString(reply.pcHdr.hdr.status));
      return reply.pcHdr.hdr.status;
   }

   if (reply.nRead > length) {
      UWLOG(0, "Got back more data than expected: %d vs %d", reply.nRead, length);
      return VMK_BAD_PARAM;
   }

   *bytesRead = reply.nRead;

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyWrite --
 *
 *      Write up to length bytes at the given offset in the given
 *	file.  Sets bytesWritten to number of bytes actually
 * 	written.  bytesWritten is undefined if an error is returned.
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

static VMK_ReturnStatus
UserProxyWrite(UserObj* obj,
               UserVAConst userData,
               uint64 offset,
               uint32 length,
               uint32 *bytesWritten)
{
   return UserProxyWriteInt(obj, (void*)userData, offset, length, bytesWritten,
                            UTIL_USERWORLD_BUFFER);
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyWriteMPN --
 *
 *      Write up to PAGE_SIZE bytes at the given offset in the given
 *	file from the given MPN.  Sets bytesWritten to number of bytes
 *	actually written.  bytesWritten is undefined if an error is
 *	returned.
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

static VMK_ReturnStatus
UserProxyWriteMPN(UserObj* obj,
                  MPN mpn,
                  uint64 offset,
                  uint32 *bytesWritten)
{
   VMK_ReturnStatus status;
   uint8* data;
   int toWrite = PAGE_SIZE;
   uint32 partialBytesWritten;

   data = KVMap_MapMPN(mpn, TLB_LOCALONLY);
   if (data == NULL) {
      return VMK_NO_ADDRESS_SPACE;
   }

   *bytesWritten = 0;
   do {
      status = UserProxyWriteInt(obj, (void*)data, offset, toWrite,
				 &partialBytesWritten, UTIL_VMKERNEL_BUFFER);
      if (status == VMK_OK) {
	 *bytesWritten += partialBytesWritten;
	 toWrite -= partialBytesWritten;
	 offset += partialBytesWritten;
      }
   } while (status == VMK_OK && toWrite > 0 && partialBytesWritten > 0);

   KVMap_FreePages(data);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyWriteInt --
 *
 *	Internal write function.  Writes up to 'length' bytes at
 *	'offset' from the given proxy object.  Copies resulting data
 *	into 'userData'.
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

static VMK_ReturnStatus
UserProxyWriteInt(UserObj* obj,
                  void* userData,
                  uint64 offset,
                  uint32 length,
                  uint32 *bytesWritten,
		  Util_BufferType bufType)
{
   UserProxy_ObjInfo *info = obj->data.proxyInfo;
   VMK_ReturnStatus status;
   UserProxyWriteReply reply;
   UserProxyWriteMsg msg;
   RPC_Token token;

   *bytesWritten = 0;

   msg.fdHdr.hdr.size = sizeof msg + length;
   msg.fdHdr.fileHandle = info->fileHandle;
   msg.offset = offset;
   msg.writeSize = length;

   UWLOG(3, "Writing %d bytes at offset %"FMT64"d to file %s",
       length, offset, info->fullPath);

   UWSTAT_TIMERSTART(proxyCallTime);
   status = UserProxySend(USERPROXY_WRITE, info->upci, &msg.fdHdr.hdr,
			  sizeof msg, TRUE, userData, bufType, &token);
   if (status != VMK_OK) {
      UWLOG(0, "Failed to send message to proxy.");
      return status;
   }

   status = UserProxyReceive(USERPROXY_WRITE, info->upci, token, &reply.pcHdr.hdr,
                             sizeof reply, FALSE, NULL, UTIL_VMKERNEL_BUFFER);
   UWSTAT_TIMERSTOP(proxyCallTime);
   if (status != VMK_OK) {
      UWLOG(0, "Failed to receive message from proxy.");
      return status;
   }

   UserProxyKernelPollCacheUpdate(UserProxyPollCacheForObj(obj),
                                  &reply.pcHdr.pcUpdate);

   if (reply.pcHdr.hdr.status != VMK_OK) {
      UWLOG(1, "(%db) Failed: %s", length,
	    UWLOG_ReturnStatusToString(reply.pcHdr.hdr.status));
      return reply.pcHdr.hdr.status;
   }

   if (reply.nWritten > length) {
      UWLOG(0, "Got back more data than expected: %d vs %d", reply.nWritten,
	    length);
      return VMK_BAD_PARAM;
   }

   *bytesWritten = reply.nWritten;

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyStat --
 *
 *	Get stats for given proxy object.
 *
 * Results:
 *      VMK_OK if stat was retrieved, statbuf is filled in.  If not
 *	VMK_OK, statbuf is undefined.
 *
 * Side effects:
 *      Bytes written to statbuf, RPCs.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserProxyStat(UserObj* obj, LinuxStat64* statbuf)
{
   UserProxy_ObjInfo *info = obj->data.proxyInfo;
   VMK_ReturnStatus status;
   UserProxyStatMsg msg;
   UserProxyStatReply reply;
   ASSERT(obj->type == USEROBJ_TYPE_PROXY_FILE ||
	  obj->type == USEROBJ_TYPE_PROXY_FIFO ||
	  obj->type == USEROBJ_TYPE_PROXY_CHAR ||
          obj->type == USEROBJ_TYPE_PROXY_SOCKET ||
          obj->type == USEROBJ_TYPE_ROOT);

   UWLOG(1, "(obj={%d, '%s'}, buf=%p)", info->fileHandle, info->fullPath, statbuf);

   msg.fileHandle = info->fileHandle;

   status = UserProxyRemoteCall(USERPROXY_STAT, info->upci,
                             &msg.hdr, sizeof(msg),
                             &reply.hdr, sizeof(reply));
   if (status == VMK_OK) {
      int fieldCount = 0;

      if (vmx86_debug) {
	 memset(statbuf, 0xff, sizeof *statbuf);
      } else {
	 memset(statbuf, 0, sizeof *statbuf);
      }

#define USERPROXYSTAT_COPYFIELD(FIELD)               \
      do {                                           \
	 statbuf->FIELD = reply.statBuf.FIELD;       \
	    fieldCount++;                            \
      } while(0)            

      USERPROXYSTAT_COPYFIELD(st_dev);
      USERPROXYSTAT_COPYFIELD(st_mode);
      USERPROXYSTAT_COPYFIELD(st_nlink);
      USERPROXYSTAT_COPYFIELD(st_uid);
      USERPROXYSTAT_COPYFIELD(st_gid);
      USERPROXYSTAT_COPYFIELD(st_rdev);
      USERPROXYSTAT_COPYFIELD(st_size);
      USERPROXYSTAT_COPYFIELD(st_blksize);
      USERPROXYSTAT_COPYFIELD(st_blocks);
      USERPROXYSTAT_COPYFIELD(st_atime);
      USERPROXYSTAT_COPYFIELD(st_mtime);
      USERPROXYSTAT_COPYFIELD(st_ctime);
      USERPROXYSTAT_COPYFIELD(st_ino);

#undef USERPROXYSTAT_COPYFIELD

      ASSERT(reply.statBuf.dbgFieldCount == fieldCount);

      // Fabricate the st_ino32 field:
      statbuf->st_ino32 = (uint32)(statbuf->st_ino);
   }
   
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyChmod --
 *
 *	Change access control mode bits of obj.
 *
 * Results:
 *      VMK_OK or error code.
 *
 * Side effects:
 *      RPCs.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserProxyChmod(UserObj* obj, LinuxMode mode)
{
   UserProxy_ObjInfo *info = obj->data.proxyInfo;
   UserProxyChmodMsg msg;
   UserProxyChmodReply reply;

   ASSERT(obj->type == USEROBJ_TYPE_PROXY_FILE ||
          obj->type == USEROBJ_TYPE_PROXY_FIFO ||
	  obj->type == USEROBJ_TYPE_PROXY_CHAR ||
          obj->type == USEROBJ_TYPE_ROOT);

   UWLOG(1, "(obj={%d, '%s'}, mode=0%o)",
         info->fileHandle, info->fullPath, mode);

   msg.fdHdr.fileHandle = info->fileHandle;
   msg.mode = mode;

   return UserProxyRemoteCall(USERPROXY_CHMOD, info->upci,
                           &msg.fdHdr.hdr, sizeof(msg),
                           &reply, sizeof(reply));
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyChown --
 *
 *	Change owner and/or group of obj.  -1 => no change.
 *
 * Results:
 *      VMK_OK or error code.
 *
 * Side effects:
 *      RPCs.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserProxyChown(UserObj* obj, Identity_UserID owner, Identity_GroupID group)
{
   UserProxy_ObjInfo *info = obj->data.proxyInfo;
   UserProxyChownMsg msg;
   UserProxyReplyHdr reply;

   ASSERT(obj->type == USEROBJ_TYPE_PROXY_FILE ||
          obj->type == USEROBJ_TYPE_PROXY_FIFO ||
	  obj->type == USEROBJ_TYPE_PROXY_CHAR ||
          obj->type == USEROBJ_TYPE_ROOT);

   UWLOG(1, "(obj={%d, '%s'}, owner=%d, group=%d)",
         info->fileHandle, info->fullPath, owner, group);

   msg.fdHdr.fileHandle = info->fileHandle;
   msg.owner = owner;
   msg.group = group;

   return UserProxyRemoteCall(USERPROXY_CHOWN, info->upci,
                           &msg.fdHdr.hdr, sizeof(msg),
                           &reply, sizeof(reply));
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyTruncate --
 *
 *	Change size of obj.
 *
 * Results:
 *      VMK_OK or error code.
 *
 * Side effects:
 *      RPCs.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserProxyTruncate(UserObj* obj, uint64 size)
{
   UserProxy_ObjInfo *info = obj->data.proxyInfo;
   UserProxyTruncateMsg msg;
   UserProxyReplyHdr reply;

   ASSERT(obj->type == USEROBJ_TYPE_PROXY_FILE ||
          obj->type == USEROBJ_TYPE_ROOT);

   UWLOG(1, "(obj={%d, '%s'}, size=%Lu)",
         info->fileHandle, info->fullPath, size);

   msg.fdHdr.fileHandle = info->fileHandle;
   msg.size = size;

   return UserProxyRemoteCall(USERPROXY_TRUNCATE, info->upci,
                           &msg.fdHdr.hdr, sizeof(msg),
                           &reply, sizeof(reply));
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyUtime --
 *
 *	Change atime and mtime of obj.
 *
 * Results:
 *      VMK_OK or error code.
 *
 * Side effects:
 *      RPCs.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserProxyUtime(UserObj* obj, uint32 atime, uint32 mtime)
{
   UserProxy_ObjInfo *info = obj->data.proxyInfo;
   UserProxyUtimeMsg msg;
   UserProxyReplyHdr reply;

   ASSERT(obj->type == USEROBJ_TYPE_PROXY_FILE ||
          obj->type == USEROBJ_TYPE_ROOT);

   UWLOG(1, "(obj={%d, '%s'}, atime=%u, mtime=%u)",
         info->fileHandle, info->fullPath, atime, mtime);

   msg.fdHdr.fileHandle = info->fileHandle;
   msg.atime = atime;
   msg.mtime = mtime;

   return UserProxyRemoteCall(USERPROXY_UTIME, info->upci,
                           &msg.fdHdr.hdr, sizeof(msg),
                           &reply, sizeof(reply));
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyStatFS --
 *
 *	Get statfs info for given proxy object.
 *
 * Results:
 *      VMK_OK if info was retrieved, statbuf is filled in.  If not
 *	VMK_OK, statbuf is undefined.
 *
 * Side effects:
 *      Bytes written to statbuf, RPCs.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserProxyStatFS(UserObj* obj, LinuxStatFS64* statbuf)
{
   UserProxy_ObjInfo *info = obj->data.proxyInfo;
   VMK_ReturnStatus status;
   UserProxyStatFSMsg msg;
   UserProxyStatFSReply reply;
   ASSERT(obj->type == USEROBJ_TYPE_PROXY_FILE ||
          obj->type == USEROBJ_TYPE_PROXY_FIFO ||
	  obj->type == USEROBJ_TYPE_PROXY_CHAR ||
          obj->type == USEROBJ_TYPE_ROOT);

   UWLOG(1, "(obj={%d, '%s'}, buf=%p)",
         info->fileHandle, info->fullPath, statbuf);

   msg.fileHandle = info->fileHandle;

   status = UserProxyRemoteCall(USERPROXY_STATFS, info->upci, &msg.hdr,
				sizeof(msg), &reply.hdr, sizeof(reply));
   if (status == VMK_OK) {
      int fieldCount = 0;

      if (vmx86_debug) {
	 memset(statbuf, 0xff, sizeof *statbuf);
      } else {
	 memset(statbuf, 0, sizeof *statbuf);
      }

#define USERPROXYSTAT_COPYFIELD(FIELD)               \
      do {                                           \
	 statbuf->FIELD = reply.statBuf.FIELD;       \
	    fieldCount++;                            \
      } while(0)            

      USERPROXYSTAT_COPYFIELD(f_type);
      USERPROXYSTAT_COPYFIELD(f_bsize);
      USERPROXYSTAT_COPYFIELD(f_blocks);
      USERPROXYSTAT_COPYFIELD(f_bfree);
      USERPROXYSTAT_COPYFIELD(f_bavail);
      USERPROXYSTAT_COPYFIELD(f_files);
      USERPROXYSTAT_COPYFIELD(f_ffree);
      USERPROXYSTAT_COPYFIELD(f_namelen);
      USERPROXYSTAT_COPYFIELD(f_spare[0]);
      USERPROXYSTAT_COPYFIELD(f_spare[1]);
      USERPROXYSTAT_COPYFIELD(f_spare[2]);
      USERPROXYSTAT_COPYFIELD(f_spare[3]);
      USERPROXYSTAT_COPYFIELD(f_spare[4]);
      USERPROXYSTAT_COPYFIELD(f_spare[5]);

      ASSERT(sizeof(statbuf->f_fsid) == sizeof(reply.statBuf.f_fsid));
      memcpy(&statbuf->f_fsid, &reply.statBuf.f_fsid,
	    sizeof(statbuf->f_fsid));
      fieldCount++;

#undef USERPROXYSTAT_COPYFIELD

      ASSERT(reply.statBuf.dbgFieldCount == fieldCount);
   }
   
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyWakeupPollWaiters --
 *
 *	Wakes up worlds waiting for cos fds.
 *
 * Results:
 *	VMK_OK on success, appropriate error code otherwise.
 *
 * Side effects:
 *	Some worlds will be rescheduled.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserProxyWakeupPollWaiters(World_Handle* world, uint32 fileHandle,
			   UserProxyPollCacheUpdate* pcUpdate)
{
   VMK_ReturnStatus status;
   User_CartelInfo* uci = world->userCartelInfo;
   UserObj* obj;

   UWLOG(2, "(obj={%#x, %#x})", fileHandle, (unsigned short)pcUpdate->events);

   status = UserProxyObjFind(uci, fileHandle, &obj);
   if (status != VMK_OK) {
      return status;
   }

   ASSERT(obj != NULL);
   ASSERT(obj->type == USEROBJ_TYPE_PROXY_FILE ||
          obj->type == USEROBJ_TYPE_PROXY_FIFO ||
	  obj->type == USEROBJ_TYPE_PROXY_CHAR ||
	  obj->type == USEROBJ_TYPE_PROXY_SOCKET);

   UWLOG(2, "(obj={%d, '%s'})", fileHandle, obj->data.proxyInfo->fullPath);

   UserProxyKernelPollCacheUpdate(UserProxyPollCacheForObj(obj), pcUpdate);
   (void) UserObj_Release(uci, obj);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyPoll --
 *
 *	Polls on a cos-side fd.
 *
 * Results:
 *	VMK_OK if the given object is ready, VMK_WOULD_BLOCK if it isn't.
 *
 * Side effects:
 *	The object's fd may be added to the poll loop in vmkload_app.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserProxyPoll(UserObj* obj, VMKPollEvent inEvents, VMKPollEvent *outEvents,
	      UserObjPollAction action)
{
   VMKPollEvent events;
   VMK_ReturnStatus status = VMK_OK;
   UserProxy_ObjInfo *info = obj->data.proxyInfo;
   UserProxyPollCache *pollCache = UserProxyPollCacheForObj(obj);

   ASSERT(obj != NULL);
   ASSERT(obj->type == USEROBJ_TYPE_PROXY_FILE ||
          obj->type == USEROBJ_TYPE_PROXY_FIFO ||
	  obj->type == USEROBJ_TYPE_PROXY_CHAR ||
	  obj->type == USEROBJ_TYPE_PROXY_SOCKET);

   UWLOG(1, "(obj={%d, '%s'}, action=%d)", info->fileHandle, info->fullPath,
	 action);

   if (action == UserObjPollCleanup) {
      UserProxyPollCache *pollCache = UserProxyPollCacheForObj(obj);
      UserProxyPollCacheLock(pollCache);
      VMKPoll_RemoveWaiter(&pollCache->waiters, MY_RUNNING_WORLD->worldID);
      UserProxyPollCacheUnlock(pollCache);
      return VMK_OK;
   }


   UserProxyPollCacheLock(pollCache);
   /*
    * If we haven't started polling on this object yet, tell the proxy to
    * start polling.  The new poll events will be piggybacked on the reply
    * rpc.  If another thread comes through while we're waiting for the new
    * poll events, it will simply return VMK_WOULD_BLOCK and be woken when
    * the new events come in.
    */
   if (!pollCache->enabled) {
      UserProxyPollCacheEnableMsg msg;
      UserProxyPollCacheEnableReply reply;

      pollCache->enabled = TRUE;
      UserProxyPollCacheUnlock(pollCache);

      msg.fileHandle = info->fileHandle;
      status = UserProxyRemoteCall(USERPROXY_POLLCACHEENABLE, info->upci,
                                   &msg.hdr, sizeof msg, &reply.hdr,
                                   sizeof reply);
      if (status != VMK_OK) {
	 UserProxyPollCacheLock(pollCache);
	 pollCache->enabled = FALSE;
	 UserProxyPollCacheUnlock(pollCache);
	 return status;
       }
      UserProxyKernelPollCacheUpdate(pollCache, &reply.pcUpdate);
      UserProxyPollCacheLock(pollCache);
   }

   /*
    * Get the events we're interested in: namely, the events specified in
    * inEvents and any error event.
    */
   events = pollCache->cache & (inEvents | VMKPOLL_ERRMASK);

   /*
    * If events is non-zero, we have something to return.
    */
   if (events) {
      *outEvents = events;
   }
   /*
    * Otherwise they have to wait.
    */
   else {
      if ((inEvents & (VMKPOLL_READ | VMKPOLL_WRITE)) &&
           action == UserObjPollNotify) {
         VMKPoll_AddWaiterForEvent(&pollCache->waiters,
                                   MY_RUNNING_WORLD->worldID, inEvents);
      }
       status = VMK_WOULD_BLOCK;
   }
   UserProxyPollCacheUnlock(pollCache);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyUnlink --
 *
 *      Unlink an arc relative to the specified object.
 *
 * Results:
 *      VMK_OK or error condition.
 *
 * Side effects:
 *      As above.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserProxyUnlink(UserObj* parent, const char* arc)
{
   UserProxy_ObjInfo *parentInfo = parent->data.proxyInfo;
   VMK_ReturnStatus status;
   UserProxyUnlinkMsg msg;
   UserProxyUnlinkReply reply;
   ASSERT(parent->type == USEROBJ_TYPE_PROXY_FILE ||
          parent->type == USEROBJ_TYPE_ROOT);

   UWLOG(1, "(arc=%s)", arc);

   status = UserProxyMakeFullName(msg.name, sizeof msg.name,
                                  parentInfo->fullPath, arc);
   if (status != VMK_OK) {
      return status;
   }

   status = UserProxyRemoteCall(USERPROXY_UNLINK, parentInfo->upci, &msg.hdr,
                             sizeof(msg),
                             &reply,
                             sizeof(reply));

   UWLOG(1, "fullname = '%s', status = %#x", msg.name, status);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyMkdir --
 *
 *      Create a directory relative to the specified object.
 *
 * Results:
 *      VMK_OK or error condition.
 *
 * Side effects:
 *      As above.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserProxyMkdir(UserObj* parent, const char* arc, LinuxMode mode)
{
   UserProxy_ObjInfo *parentInfo = parent->data.proxyInfo;
   VMK_ReturnStatus status;
   UserProxyMkdirMsg msg;
   UserProxyMkdirReply reply;
   ASSERT(parent->type == USEROBJ_TYPE_PROXY_FILE ||
          parent->type == USEROBJ_TYPE_ROOT);

   UWLOG(1, "(arc=%s)", arc);

   status = UserProxyMakeFullName(msg.name, sizeof msg.name,
                                  parentInfo->fullPath, arc);
   if (status != VMK_OK) {
      return status;
   }

   msg.mode = mode;

   status = UserProxyRemoteCall(USERPROXY_MKDIR, parentInfo->upci, &msg.hdr,
                             sizeof(msg),
                             &reply,
                             sizeof(reply));

   UWLOG(1, "fullname = '%s', status = %#x", msg.name, status);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyRmdir --
 *
 *      Remove a directory relative to the specified object.
 *
 * Results:
 *      VMK_OK or error condition.
 *
 * Side effects:
 *      As above.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserProxyRmdir(UserObj* parent, const char* arc)
{
   UserProxy_ObjInfo *parentInfo = parent->data.proxyInfo;
   VMK_ReturnStatus status;
   UserProxyRmdirMsg msg;
   UserProxyRmdirReply reply;
   ASSERT(parent->type == USEROBJ_TYPE_PROXY_FILE ||
          parent->type == USEROBJ_TYPE_ROOT);

   UWLOG(1, "(arc=%s)", arc);

   status = UserProxyMakeFullName(msg.name, sizeof msg.name,
                                  parentInfo->fullPath, arc);
   if (status != VMK_OK) {
      return status;
   }

   status = UserProxyRemoteCall(USERPROXY_RMDIR, parentInfo->upci, &msg.hdr,
                             sizeof(msg),
                             &reply,
                             sizeof(reply));

   UWLOG(1, "fullname = '%s', status = %#x", msg.name, status);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyGetName --
 *
 *      Get the name of obj relative to its parent directory.
 *
 * Results:
 *      VMK_OK or error condition.
 *
 * Side effects:
 *      Returns the name in arc.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserProxyGetName(UserObj* obj, char* arc, uint32 length)
{
   UserProxy_ObjInfo *info = obj->data.proxyInfo;
   ASSERT(obj->type == USEROBJ_TYPE_PROXY_FILE ||
	  obj->type == USEROBJ_TYPE_PROXY_FIFO);
   char *slash;
   int usedLength;

   UWLOG(1, "(fullPath=%s max arc length=%d)", info->fullPath, length);

   slash = strrchr(info->fullPath, '/');
   if (slash == NULL) {
      slash = "";
   } else {
      slash++;
   }
      
   usedLength = snprintf(arc, length, "%s", slash);
   if (usedLength >= length) {
      return VMK_NAME_TOO_LONG;
   }

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyReadSymLink --
 *
 *      Read a symbolic link relative to the specified object.
 *
 * Results:
 *      VMK_OK or error condition.
 *
 * Side effects:
 *      As above.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserProxyReadSymLink(UserObj* parent, const char *arc,
                     char *buf, unsigned buflen)
{
   UserProxy_ObjInfo *parentInfo = parent->data.proxyInfo;
   User_CartelInfo *uci = UserProxyUCIForUPCI(parentInfo->upci);
   VMK_ReturnStatus status;
   UserProxyReadlinkMsg *msg = NULL;  // Too big for stack, allocate on heap
   UserProxyReadlinkReply *reply = NULL;
   ASSERT(parent->type == USEROBJ_TYPE_PROXY_FILE ||
          parent->type == USEROBJ_TYPE_ROOT);

   UWLOG(1, "(arc=%s)", arc);

   msg = User_HeapAlloc(uci, sizeof *msg);
   if (msg == NULL) {
      return VMK_NO_MEMORY;
   }

   reply = User_HeapAlloc(uci, sizeof *reply);
   if (reply == NULL) {
      status = VMK_NO_MEMORY;
      goto done;
   }

   status = UserProxyMakeFullName(msg->name, sizeof msg->name,
                                  parentInfo->fullPath, arc);
   if (status != VMK_OK) {
      goto done;
   }

   status = UserProxyRemoteCall(USERPROXY_READLINK, parentInfo->upci, &msg->hdr,
                             sizeof(*msg),
                             &reply->hdr,
                             sizeof(*reply));

   UWLOG(1, "fullname = '%s', status = %#x, link = '%s'",
         msg->name, status, reply->link);
   if (strlen(reply->link) > buflen) {
      status = VMK_NAME_TOO_LONG;
   } else {
      strncpy(buf, reply->link, buflen);
   }

done:
   if (msg != NULL) {
      User_HeapFree(uci, msg);
   }
   if (reply != NULL) {
      User_HeapFree(uci, reply);
   }
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyMakeSymLink --
 *
 *      Make a symbolic link relative to the specified object.
 *
 * Results:
 *      VMK_OK or error condition.
 *
 * Side effects:
 *      As above.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserProxyMakeSymLink(UserObj* parent, const char *arc, const char *link)
{
   UserProxy_ObjInfo *parentInfo = parent->data.proxyInfo;
   User_CartelInfo *uci = UserProxyUCIForUPCI(parentInfo->upci);
   VMK_ReturnStatus status;
   UserProxySymlinkMsg *msg;  // Too big for stack, allocate on heap instead
   UserProxySymlinkReply reply;
   int nlen;
   ASSERT(parent->type == USEROBJ_TYPE_PROXY_FILE ||
          parent->type == USEROBJ_TYPE_ROOT);

   UWLOG(1, "(arc=%s, link=%s)", arc, link);

   msg = User_HeapAlloc(uci, sizeof *msg);
   if (msg == NULL) {
      return VMK_NO_MEMORY;
   }

   status = UserProxyMakeFullName(msg->name, sizeof msg->name,
                                  parentInfo->fullPath, arc);
   if (status != VMK_OK) {
      goto done;
   }
   
   nlen = snprintf(msg->link, sizeof(msg->link), "%s", link);
   if (nlen >= sizeof(msg->link)) {
      status = VMK_NAME_TOO_LONG;
      goto done;
   }

   status = UserProxyRemoteCall(USERPROXY_SYMLINK, parentInfo->upci, &msg->hdr,
                             sizeof(*msg),
                             &reply,
                             sizeof(reply));

   UWLOG(1, "fullname = '%s', status = %#x", msg->name, status);

done:
   User_HeapFree(uci, msg);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyMakeHardLink --
 *
 *      Make a hard link relative to the specified object.
 *
 * Results:
 *      VMK_OK or error condition.
 *
 * Side effects:
 *      As above.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserProxyMakeHardLink(UserObj* parent, const char *arc, UserObj* target)
{
   UserProxy_ObjInfo *parentInfo = parent->data.proxyInfo;
   UserProxy_ObjInfo *targetInfo = target->data.proxyInfo;
   User_CartelInfo *uci = UserProxyUCIForUPCI(parentInfo->upci);
   VMK_ReturnStatus status;
   UserProxyLinkMsg *msg;  // Too big for stack, allocate on heap instead
   UserProxyLinkReply reply;
   int nlen;
   ASSERT(parent->type == USEROBJ_TYPE_PROXY_FILE ||
          parent->type == USEROBJ_TYPE_ROOT);
   ASSERT(parentInfo->upci == targetInfo->upci);

   UWLOG(1, "(arc=%s)", arc);

   if (target->type != USEROBJ_TYPE_PROXY_FILE &&
       target->type != USEROBJ_TYPE_ROOT) {
      return VMK_CROSS_DEVICE_LINK;
   }

   msg = User_HeapAlloc(uci, sizeof *msg);
   if (msg == NULL) {
      return VMK_NO_MEMORY;
   }

   status = UserProxyMakeFullName(msg->newName, sizeof msg->newName,
                                  parentInfo->fullPath, arc);
   if (status != VMK_OK) {
      goto done;
   }
   
   nlen = snprintf(msg->oldName, sizeof msg->oldName, "%s",
		   targetInfo->fullPath);
   if (nlen >= (sizeof msg->oldName)) {
      status = VMK_NAME_TOO_LONG;
      goto done;
   }

   status = UserProxyRemoteCall(USERPROXY_LINK, parentInfo->upci, &msg->hdr,
                             sizeof(*msg),
                             &reply,
                             sizeof(reply));

   UWLOG(1, "oldName= '%s', newName= '%s', status = %#x",
         msg->oldName, msg->newName, status);

done:
   User_HeapFree(uci, msg);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyRename --
 *
 *      Rename.
 *
 * Results:
 *      VMK_OK or error condition.
 *
 * Side effects:
 *      As above.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserProxyRename(UserObj* newDir, const char *newArc,
                UserObj* oldDir, const char *oldArc)
{
   UserProxy_ObjInfo *newDirInfo = newDir->data.proxyInfo;
   UserProxy_ObjInfo *oldDirInfo = oldDir->data.proxyInfo;
   User_CartelInfo *uci = UserProxyUCIForUPCI(newDirInfo->upci);
   VMK_ReturnStatus status;
   UserProxyRenameMsg *msg;  // Too big for stack, allocate on heap instead
   UserProxyRenameReply reply;
   ASSERT(newDir->type == USEROBJ_TYPE_PROXY_FILE ||
          newDir->type == USEROBJ_TYPE_ROOT);
   ASSERT(newDirInfo->upci == oldDirInfo->upci);

   UWLOG(1, "(oldArc=%s, newArc=%s)", oldArc, newArc);

   if (oldDir->type != USEROBJ_TYPE_PROXY_FILE &&
       oldDir->type != USEROBJ_TYPE_ROOT) {
      return VMK_CROSS_DEVICE_LINK;
   }

   msg = User_HeapAlloc(uci, sizeof *msg);
   if (msg == NULL) {
      return VMK_NO_MEMORY;
   }

   status = UserProxyMakeFullName(msg->newName, sizeof msg->newName,
                                  newDirInfo->fullPath, newArc);
   if (status != VMK_OK) {
      goto done;
   }

   status = UserProxyMakeFullName(msg->oldName, sizeof msg->oldName,
                                  oldDirInfo->fullPath, oldArc);
   if (status != VMK_OK) {
      goto done;
   }

   status = UserProxyRemoteCall(USERPROXY_RENAME, newDirInfo->upci, &msg->hdr,
                             sizeof(*msg),
                             &reply,
                             sizeof(reply));

   UWLOG(1, "oldName= '%s', newName= '%s', status = %#x",
         msg->oldName, msg->newName, status);

done:
   User_HeapFree(uci, msg);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyMknod --
 *
 *	If the object is a fifo, forwards the request to the proxy.
 *
 * Results:
 *	VMK_ReturnStatus
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserProxyMknod(UserObj* parent, char* arc, LinuxMode mode)
{
   UserProxy_ObjInfo *parentInfo = parent->data.proxyInfo;
   VMK_ReturnStatus status;
   UserProxyMkfifoMsg msg;
   UserProxyMkfifoReply reply;
   ASSERT(parent->type == USEROBJ_TYPE_PROXY_FILE ||
          parent->type == USEROBJ_TYPE_ROOT);

   UWLOG(1, "(arc=%s)", arc);

   /*
    * We only support fifos.
    */
   if (!(mode & LINUX_MODE_IFIFO)) {
      return VMK_BAD_PARAM;
   }

   status = UserProxyMakeFullName(msg.name, sizeof msg.name,
                                  parentInfo->fullPath, arc);
   if (status != VMK_OK) {
      return status;
   }
   msg.mode = mode;
   
   status = UserProxyRemoteCall(USERPROXY_MKFIFO, parentInfo->upci, &msg.hdr,
			     sizeof msg, &reply, sizeof reply);

   UWLOG(1, "fullname = '%s', status = %#x", msg.name, status);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyFcntl --
 *
 *	Perform various miscellaneous operations of the given UserObj.
 *
 * Results:
 *	VMK_ReturnStatus
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserProxyFcntl(UserObj* obj, uint32 cmd, uint32 arg)
{
   VMK_ReturnStatus status;
   UserProxyFcntlMsg msg;
   UserProxyFcntlReply reply;
   UserProxy_ObjInfo* info = obj->data.proxyInfo;
   ASSERT(obj->type == USEROBJ_TYPE_PROXY_FILE ||
	  obj->type == USEROBJ_TYPE_PROXY_FIFO ||
	  obj->type == USEROBJ_TYPE_PROXY_CHAR ||
	  obj->type == USEROBJ_TYPE_PROXY_SOCKET ||
          obj->type == USEROBJ_TYPE_ROOT);

   UWLOG(1, "(cmd=%#x arg=%#x)", cmd, arg);

   /*
    * Make sure we only get commands we are expecting (and can handle).
    */
   if (cmd != LINUX_FCNTL_CMD_SETFL) {
      return UserObj_NotImplemented(obj);
   }

   msg.fdHdr.fileHandle = info->fileHandle;
   msg.cmd = cmd;
   msg.arg = arg;
   
   status = UserProxyRemoteCall(USERPROXY_FCNTL, info->upci, &msg.fdHdr.hdr,
				sizeof msg, &reply, sizeof reply);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyFsync --
 *
 *	Force buffered writes on obj to disk.
 *
 * Results:
 *	VMK_ReturnStatus
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserProxyFsync(UserObj* obj, Bool dataOnly)
{
   VMK_ReturnStatus status;
   UserProxyFsyncMsg msg;
   UserProxyFsyncReply reply;
   UserProxy_ObjInfo* info = obj->data.proxyInfo;
   ASSERT(obj->type == USEROBJ_TYPE_PROXY_FILE ||
	  obj->type == USEROBJ_TYPE_PROXY_FIFO ||
	  obj->type == USEROBJ_TYPE_PROXY_CHAR ||
          obj->type == USEROBJ_TYPE_ROOT);

   UWLOG(1, "(dataOnly=%d)", dataOnly);

   msg.fdHdr.fileHandle = info->fileHandle;
   msg.dataOnly = dataOnly;
   
   status = UserProxyRemoteCall(USERPROXY_FSYNC, info->upci, &msg.fdHdr.hdr,
				sizeof msg, &reply, sizeof reply);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyReadDir --
 *
 *      Read directory entries.
 *
 * Results:
 *	VMK_ReturnStatus
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserProxyReadDir(UserObj* obj, UserVA /* LinuxDirent64* */ userData,
                 uint32 length, uint32* bytesRead)
{
   UserProxy_ObjInfo *info = obj->data.proxyInfo;
   VMK_ReturnStatus status;
   UserProxyReadDirMsg msg;
   UserProxyReadDirReply reply;
   RPC_Token token;

   msg.fdHdr.hdr.size = sizeof msg;
   msg.fdHdr.fileHandle = info->fileHandle;
   msg.dataSize = length;

   UWLOG(1, "Readdir %d bytes from dir %s", length, info->fullPath);

   UWSTAT_TIMERSTART(proxyCallTime);
   status = UserProxySend(USERPROXY_READDIR, info->upci, &msg.fdHdr.hdr,
			  sizeof msg, FALSE, 0, UTIL_VMKERNEL_BUFFER, &token);
   if (status != VMK_OK) {
      UWLOG(0, "Failed to send message to proxy.");
      return status;
   }

   status = UserProxyReceive(USERPROXY_READDIR, info->upci, token, &reply.hdr,
                             sizeof reply, TRUE,
                             (void*) userData, UTIL_USERWORLD_BUFFER);
   UWSTAT_TIMERSTOP(proxyCallTime);
   if (status != VMK_OK) {
      UWLOG(0, "Failed to receive message from proxy.");
      return status;
   }

   if (reply.hdr.status != VMK_OK) {
      UWLOG(1, "Request failed: %#x:%s",
            reply.hdr.status, UWLOG_ReturnStatusToString(reply.hdr.status));
      return reply.hdr.status;
   }

   if (reply.nRead > length) {
      UWLOG(0, "Got back more data than expected: %d vs %d",
            reply.nRead, length);
      return VMK_BAD_PARAM;
   }

   *bytesRead = reply.nRead;

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyIoctl --
 *
 *      ioctl for devices and files.
 *
 * Results:
 *	VMK_ReturnStatus
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserProxyIoctl(UserObj* obj, uint32 cmd, LinuxIoctlArgType type,
               uint32 dataSize, void *userData, uint32 *result)
{
   RPC_Token token;
   uint32 allocSize, arg;
   UserProxyIoctlMsg *msg;
   UserProxyIoctlReply *reply;
   VMK_ReturnStatus status;
   LinuxIoctlPackedData *packedData;
   UserProxy_ObjInfo *info = obj->data.proxyInfo;
   User_CartelInfo* uci = UserProxyUCIForUPCI(info->upci);

   UWLOG(1, "(cmd=%#x type=%#x size=%#x userData=%p)",
         cmd, type, dataSize, userData);

   *result = -1;
   ASSERT(info != NULL);

   /*
    * Allocate message buffer
    */
   allocSize = sizeof(*msg) + dataSize;
   msg = (UserProxyIoctlMsg *)User_HeapAlloc(uci, allocSize);
   if (msg == NULL) {
      return VMK_NO_MEMORY;
   }

   msg->varHdr.fdHdr.hdr.size   = allocSize;
   msg->varHdr.fdHdr.fileHandle = info->fileHandle;
   msg->varHdr.dataSize         = dataSize;
   msg->cmd                     = cmd;
   msg->packed                  = (type == LINUX_IOCTL_ARG_PACKED) ? 1 : 0;

   /*
    * Allocate reply buffer
    */
   allocSize = sizeof(UserProxyIoctlReply) + dataSize;
   reply = (UserProxyIoctlReply *)User_HeapAlloc(uci, allocSize);
   if (reply == NULL) {
      User_HeapFree(uci, msg);
      return VMK_NO_MEMORY;
   }

   /*
    * Send proxy request
    */
   UWSTAT_TIMERSTART(proxyCallTime);
   switch (type) {
      case LINUX_IOCTL_ARG_CONST:
         ASSERT(dataSize == sizeof(uint32));
         arg = (uint32)userData;
         status = UserProxySend(USERPROXY_IOCTL, info->upci,
                                &msg->varHdr.fdHdr.hdr, sizeof *msg, TRUE,
                                (void *)&arg, UTIL_VMKERNEL_BUFFER,
                                &token);
         break;
      case LINUX_IOCTL_ARG_PTR:
         status = UserProxySend(USERPROXY_IOCTL, info->upci,
                                &msg->varHdr.fdHdr.hdr, sizeof *msg, TRUE,
                                userData, UTIL_USERWORLD_BUFFER,
                                &token);
         break;
      case LINUX_IOCTL_ARG_PACKED:
         packedData = (LinuxIoctlPackedData *)userData;
         status = UserProxySend(USERPROXY_IOCTL, info->upci,
                                &msg->varHdr.fdHdr.hdr, sizeof *msg, TRUE,
                                (void *)packedData->buf, UTIL_VMKERNEL_BUFFER,
                                &token);
         break;
      default:
         NOT_IMPLEMENTED();
         break;
   }

   if (status != VMK_OK) {
      UWLOG(0, "Failed to send message to proxy: %s",
            UWLOG_ReturnStatusToString(status));
      goto done;
   }

   /*
    * Get the proxy reply
    */
   switch (type) {
      case LINUX_IOCTL_ARG_CONST:
         status = UserProxyReceive(USERPROXY_IOCTL, info->upci, token, &reply->hdr,
                                   sizeof *reply, TRUE, (void *)&arg,
                                   UTIL_VMKERNEL_BUFFER);
         break;
      case LINUX_IOCTL_ARG_PTR:
         status = UserProxyReceive(USERPROXY_IOCTL, info->upci, token, &reply->hdr,
                                   sizeof *reply, TRUE, userData,
                                   UTIL_USERWORLD_BUFFER);
         if ((status == VMK_OK) && (reply->size != dataSize)) {
            UWLOG(0, "Expected data size %d != %d bytes received",
                  dataSize, reply->size);
            status = VMK_BAD_PARAM;
         } 
         break;
      case LINUX_IOCTL_ARG_PACKED:
         packedData = (LinuxIoctlPackedData *)userData;
         status = UserProxyReceive(USERPROXY_IOCTL, info->upci, token, &reply->hdr,
                                   sizeof *reply, TRUE, packedData->buf,
                                   UTIL_VMKERNEL_BUFFER);
         if ((status == VMK_OK) && (reply->size != dataSize)) {
            UWLOG(0, "Expected data size %d != %d bytes received",
                  dataSize, reply->size);
            status = VMK_BAD_PARAM;
         } 
         break;
      default:
         NOT_IMPLEMENTED();
         break;
   }
   UWSTAT_TIMERSTOP(proxyCallTime);

   /*
    * Check status and set result
    */
   if (status != VMK_OK) {
      UWLOG(0, "Failed to receive message from proxy.");
      goto done;
   } else {
      *result = reply->result;
      UWLOG(1, "result=%d", *result);
   }

   /*
    * Finished
    */
done:
   User_HeapFree(uci, msg);
   User_HeapFree(uci, reply);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyToString --
 *
 *	Return a string representation of this object.
 *
 * Results:
 *	VMK_OK.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
UserProxyToString(UserObj *obj, char *string, int length)
{
   int len;
 
   /*
    * Special case root.
    */
   if (obj->type == USEROBJ_TYPE_ROOT) {
      len = snprintf(string, length, "/");
   } else {
      UserProxy_ObjInfo *info = obj->data.proxyInfo;
      ASSERT(info != NULL);

      len = snprintf(string, length, "%s, fh: %d", info->fullPath,
		     info->fileHandle);
   }

   if (len >= length) {
      UWLOG(1, "Description string too long (%d vs %d).  Truncating.", len,
	    length);
   }

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxy_Sync --
 *
 *	Force buffered writes on all COS files to disk.
 *
 * Results:
 *	VMK_ReturnStatus
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
UserProxy_Sync(User_CartelInfo* uci)
{
   VMK_ReturnStatus status;
   UserProxySyncMsg msg;
   UserProxySyncReply reply;

   UWLOG(1, "()");
   status = UserProxyRemoteCall(USERPROXY_SYNC, &uci->proxy, &msg, sizeof msg,
			     &reply, sizeof reply);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxy_RegisterThread --
 *
 *      Relay the initial uids and and gids of a new thread to the
 *      proxy.
 *
 * Results:
 *	VMK_ReturnStatus
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
UserProxy_RegisterThread(struct User_CartelInfo *uci,
                         World_ID worldID, struct Identity *ident)
{
   VMK_ReturnStatus status;
   UserProxyRegisterThreadMsg msg;
   UserProxyRegisterThreadReply reply;

   UWLOG(1, "(%d, ...)", worldID);

   msg.worldID = worldID;
   msg.ruid = ident->ruid;
   msg.euid = ident->euid;
   msg.suid = ident->suid;
   msg.rgid = ident->rgid;
   msg.egid = ident->egid;
   msg.sgid = ident->sgid;
   msg.ngids = ident->ngids;
   memcpy(msg.gids, ident->gids, ident->ngids * sizeof(uint32));

   status = UserProxyRemoteCall(USERPROXY_REGISTER_THREAD, &uci->proxy,
                             &msg.hdr, sizeof msg, &reply, sizeof reply);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxy_Setresuid --
 *
 *      Relay a uid change for this thread to the proxy.
 *
 * Results:
 *	VMK_ReturnStatus
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
UserProxy_Setresuid(struct User_CartelInfo *uci,
                    LinuxUID ruid, LinuxUID euid, LinuxUID suid)
{
   VMK_ReturnStatus status;
   UserProxySetresuidMsg msg;
   UserProxySetresuidReply reply;

   UWLOG(1, "(%u, %u, %u)", ruid, euid, suid);

   msg.ruid = ruid;
   msg.euid = euid;
   msg.suid = suid;

   status = UserProxyRemoteCall(USERPROXY_SETRESUID, &uci->proxy,
                             &msg.hdr, sizeof msg, &reply, sizeof reply);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxy_Setresgid --
 *
 *      Relay a gid change for this thread to the proxy.
 *
 * Results:
 *	VMK_ReturnStatus
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
UserProxy_Setresgid(struct User_CartelInfo *uci,
                    LinuxGID rgid, LinuxGID egid, LinuxGID sgid)
{
   VMK_ReturnStatus status;
   UserProxySetresgidMsg msg;
   UserProxySetresgidReply reply;

   UWLOG(1, "(%u, %u, %u)", rgid, egid, sgid);

   msg.rgid = rgid;
   msg.egid = egid;
   msg.sgid = sgid;

   status = UserProxyRemoteCall(USERPROXY_SETRESGID, &uci->proxy,
                             &msg.hdr, sizeof msg, &reply, sizeof reply);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxy_Setgroups --
 *
 *      Relay a change in supplementary groups for this thread to the
 *      proxy.
 *
 * Results:
 *	VMK_ReturnStatus
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
UserProxy_Setgroups(User_CartelInfo* uci, uint32 ngids, LinuxGID *gids)
{
   VMK_ReturnStatus status;
   UserProxySetgroupsMsg msg;
   UserProxySetgroupsReply reply;

   UWLOG(1, "(%u, [%u, %u, ...])", ngids, gids[0], gids[1]);

   msg.ngids = ngids;
   memcpy(msg.gids, gids, ngids * sizeof(uint32));

   status = UserProxyRemoteCall(USERPROXY_SETGROUPS, &uci->proxy,
                             &msg.hdr, sizeof msg, &reply, sizeof reply);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxySocketObjInit --
 *
 *	A wrapper for UserProxyObjInit for sockets.
 *
 * Results:
 *	None.
 *
 * Side-effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
void
UserProxySocketObjInit(User_CartelInfo* uci, uint32 fileHandle, UserObj* obj,
		       uint32 pcHandle)
{
   VMK_ReturnStatus status;
   char fullpath[50];
   int len;

   len = snprintf(fullpath, sizeof fullpath, "<socket (handle=%d)>", fileHandle);
   ASSERT(len < sizeof fullpath);
   status = UserProxyObjInit(uci, obj, &uci->proxy, USEROBJ_TYPE_PROXY_SOCKET,
			     fileHandle, fullpath, len, USEROBJ_OPEN_RDWR,
			     pcHandle);

   UWLOG(1, "Created %s", fullpath);

   /* The VMK_NAME_TOO_LONG error is not applicable to sockets, so the
    * init had to succeed.  */
   ASSERT(status == VMK_OK);
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxy_CreateSocket --
 *
 *	Create a new socket object with the given family (ie, unix domain
 *	or stream), type (ie, stream or packet), protocol (ie, tcp or udp).
 *
 * Results:
 *	VMK_OK if created and added, otherwise on error.
 *
 * Side-effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserProxy_CreateSocket(User_CartelInfo* uci, LinuxSocketFamily family,
		       LinuxSocketType type, LinuxSocketProtocol protocol,
		       UserObj** outObj)
{
   VMK_ReturnStatus status;
   UserProxyCreateSocketMsg msg;
   UserProxyCreateSocketReply reply;

   msg.family = family;
   msg.type = type;
   msg.protocol = protocol;

   UWLOG(1, "(family=%d, type=%d, protocol=%d, outObj=%p)", family, type,
	 protocol, outObj);

   *outObj = UserProxyObjPreallocate(uci);
   if (*outObj == NULL) {
      return VMK_NO_MEMORY;
   }

   status = UserProxyRemoteCall(USERPROXY_CREATESOCKET, &uci->proxy, &msg.hdr,
			     sizeof msg, &reply.hdr, sizeof reply);
   if (status == VMK_OK) {
      UserProxySocketObjInit(uci, reply.fileHandle, *outObj, reply.pcHandle);
   } else {
      UserProxyObjFreePreallocated(uci, *outObj);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyBind --
 *
 *	Bind the given socket to the given name.
 *
 * Results:
 *	VMK_OK if socket found and bound, otherwise if error.
 *
 * Side-effects:
 * 	Socket is bound to name.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserProxyBind(UserObj* obj, LinuxSocketName* name, uint32 nameLen)
{
   UserProxy_ObjInfo* info = obj->data.proxyInfo;
   UserProxyBindMsg msg;
   UserProxyBindReply reply;

   ASSERT(info != NULL);

   UWLOG(1, "(name=%p, nameLen=%d)", name, nameLen);

   msg.fdHdr.fileHandle = info->fileHandle;
   memcpy(&msg.name, name, MIN(sizeof msg.name, nameLen));
   msg.nameLen = nameLen;

   return UserProxyRemoteCall(USERPROXY_BIND, info->upci, &msg.fdHdr.hdr,
			      sizeof msg, &reply, sizeof reply);
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyConnect --
 *
 *	Connect the given socket to the given name.
 *
 * Results:
 *	VMK_OK if okay, otherwise on error.
 *
 * Side-effects:
 * 	Socket is connected
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
UserProxyConnect(UserObj* obj, LinuxSocketName* name, uint32 nameLen)
{
   VMK_ReturnStatus status;
   UserProxy_ObjInfo* info = obj->data.proxyInfo;
   UserProxyConnectMsg msg;
   UserProxyConnectReply reply;
   RPC_Token token;

   ASSERT(info != NULL);

   UWLOG(1, "(name=%p, nameLen=%d)", name, nameLen);

   msg.fdHdr.hdr.size = sizeof msg;
   msg.fdHdr.fileHandle = info->fileHandle;
   memcpy(&msg.name, name, MIN(sizeof msg.name, nameLen));
   msg.nameLen = nameLen;

   status = UserProxySend(USERPROXY_CONNECT, info->upci, &msg.fdHdr.hdr,
			  sizeof msg, FALSE, NULL, UTIL_VMKERNEL_BUFFER, &token);
   if (status != VMK_OK) {
      return status;
   }
   
   status = UserProxyReceive(USERPROXY_CONNECT, info->upci, token, &reply.hdr,
                             sizeof reply, FALSE, NULL, UTIL_VMKERNEL_BUFFER);
   if (status != VMK_OK) {
      return status;
   }

   UserProxyKernelPollCacheUpdate(UserProxyPollCacheForObj(obj),
                                  &reply.pcUpdate);

   return reply.hdr.status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxy_Socketpair --
 *
 *	Creates a pair of sockets, which are connected together.
 *	(Only supported for UNIX-domain sockets in Linux.)
 *
 * Results:
 *	VMK_OK if okay, otherwise on error.
 *
 * Side-effects:
 * 	Socket is connected
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
UserProxy_Socketpair(LinuxSocketFamily family, LinuxSocketType type,
		     LinuxSocketProtocol protocol, UserObj** obj1, UserObj** obj2)
{
   User_CartelInfo* uci = MY_USER_CARTEL_INFO;
   VMK_ReturnStatus status;
   UserProxySocketpairMsg msg;
   UserProxySocketpairReply reply;

   /*
    * XXX: Always called from a UserWorld syscall context.  Helper worlds will
    * never call this function.
    */
   ASSERT(World_IsUSERWorld(MY_RUNNING_WORLD));

   UWLOG(1, "(family=%d, type=%d, protocol=%d)", family, type, protocol);

   msg.family = family;
   msg.type = type;
   msg.protocol = protocol;

   *obj1 = UserProxyObjPreallocate(uci);
   if (*obj1 == NULL) {
      return VMK_NO_MEMORY;
   }

   *obj2 = UserProxyObjPreallocate(uci);
   if (*obj2 == NULL) {
      UserProxyObjFreePreallocated(uci, *obj1);
      *obj1 = NULL;
      return VMK_NO_MEMORY;
   }

   status = UserProxyRemoteCall(USERPROXY_SOCKETPAIR, &uci->proxy, &msg.hdr,
			     sizeof msg, &reply.hdr, sizeof reply);
   if (status == VMK_OK) {
      UserProxySocketObjInit(uci, reply.fileHandle1, *obj1,
			     USERPROXY_INVALID_PCHANDLE);
      UserProxySocketObjInit(uci, reply.fileHandle2, *obj2,
			     USERPROXY_INVALID_PCHANDLE);
   } else {
      UserProxyObjFreePreallocated(uci, *obj1);
      UserProxyObjFreePreallocated(uci, *obj2);
      *obj1 = *obj2 = NULL;
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyListen --
 *
 *	Listen for incoming connections on the given socket.
 *
 * Results:
 *	VMK_OK if listen succeeds, otherwise on error.
 *
 * Side-effects:
 * 	Incoming connections are acknowledged on this socket.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
UserProxyListen(UserObj* obj, int backlog)
{
   VMK_ReturnStatus status;
   UserProxy_ObjInfo* info = obj->data.proxyInfo;
   UserProxyListenMsg msg;
   UserProxyListenReply reply;
   RPC_Token token;

   ASSERT(info != NULL);

   UWLOG(1, "(backlog=%d)", backlog);

   msg.fdHdr.hdr.size = sizeof msg;
   msg.fdHdr.fileHandle = info->fileHandle;
   msg.backlog = backlog;

   status = UserProxySend(USERPROXY_LISTEN, info->upci, &msg.fdHdr.hdr,
			  sizeof msg, FALSE, NULL, UTIL_VMKERNEL_BUFFER, &token);
   if (status != VMK_OK) {
      return status;
   }
   
   status = UserProxyReceive(USERPROXY_LISTEN, info->upci, token, &reply.hdr,
                             sizeof reply, FALSE, NULL, UTIL_VMKERNEL_BUFFER);
   if (status != VMK_OK) {
      return status;
   }

   UserProxyKernelPollCacheUpdate(UserProxyPollCacheForObj(obj),
                                  &reply.pcUpdate);

   return reply.hdr.status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyAccept --
 *
 *	Accept a remote connection on the given socket.
 *
 * Results:
 *	VMK_OK if accept completed, otherwise on error.
 *
 * Side-effects:
 * 	A new connections on given socket is accepted
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
UserProxyAccept(UserObj* obj, UserObj** newObj,
                LinuxSocketName* name, uint32* nameLen)
{
   UserProxy_ObjInfo* info = obj->data.proxyInfo;
   User_CartelInfo* uci = UserProxyUCIForUPCI(info->upci);
   VMK_ReturnStatus status;
   UserProxyAcceptMsg msg;
   UserProxyAcceptReply reply;
   RPC_Token token;

   ASSERT(info != NULL);

   UWLOG(1, "(fileHandle=%d name@%p, nameLen@%p=%d)",
         info->fileHandle, name, nameLen, *nameLen);

   *newObj = UserProxyObjPreallocate(uci);
   if (*newObj == NULL) {
      return VMK_NO_MEMORY;
   }

   msg.fdHdr.hdr.size = sizeof msg;
   msg.fdHdr.fileHandle = info->fileHandle;
   msg.nameLen = *nameLen;

   status = UserProxySend(USERPROXY_ACCEPT, info->upci, &msg.fdHdr.hdr,
                          sizeof msg, FALSE, NULL, UTIL_VMKERNEL_BUFFER,
                          &token);
   if (status != VMK_OK) {
      goto done;
   }
   
   status = UserProxyReceive(USERPROXY_ACCEPT, info->upci, token,
			     &reply.pcHdr.hdr, sizeof reply, FALSE, NULL,
			     UTIL_VMKERNEL_BUFFER);
   if (status != VMK_OK) {
      goto done;
   }

   UserProxyKernelPollCacheUpdate(UserProxyPollCacheForObj(obj),
                                  &reply.pcHdr.pcUpdate);
   status = reply.pcHdr.hdr.status;

done:
   if (status == VMK_OK) {
      if (name != NULL) {
         if (reply.nameLen > *nameLen) {
            UWWarn("Got a bigger nameLen back than what we sent: %d vs %d, "
                   "truncating result", reply.nameLen, *nameLen);
            reply.nameLen = *nameLen;
         }
         ASSERT(reply.nameLen <= sizeof(LinuxSocketName));
         memcpy(name, &reply.name, reply.nameLen);
         *nameLen = reply.nameLen;
      }
      UserProxySocketObjInit(uci, reply.newFileHandle, *newObj,
			     USERPROXY_INVALID_PCHANDLE);
   } else {
      UserProxyObjFreePreallocated(uci, *newObj);
      *newObj = NULL;
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyGetSocketName --
 *
 *	Get the name of the given socket.
 *
 * Results:
 *	name is set to the name of the given socket.  VMK_OK if okay,
 *	otherwise if error.
 *
 * Side-effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
UserProxyGetSocketName(UserObj* obj, LinuxSocketName* name,
		       uint32* nameLen)
{
   UserProxy_ObjInfo* info = obj->data.proxyInfo;
   VMK_ReturnStatus status;
   UserProxyGetnameMsg msg;
   UserProxyGetnameReply reply;

   ASSERT(info != NULL);

   UWLOG(1, "(name@%p, nameLen@%p=%d)", name, nameLen, *nameLen);

   msg.fdHdr.fileHandle = info->fileHandle;
   msg.nameLen = *nameLen;

   status = UserProxyRemoteCall(USERPROXY_GETNAME, info->upci, &msg.fdHdr.hdr,
			     sizeof msg, &reply.hdr, sizeof reply);
   if (status == VMK_OK) {
      if (reply.nameLen > *nameLen) {
         UWWarn("Got a bigger nameLen back than what we sent: %d vs %d, "
		"truncating result", reply.nameLen, *nameLen);
         reply.nameLen = *nameLen;
      }

      /*
       * It's OK to ASSERT this because we already checked that *nameLen <=
       * sizeof(LinuxSocketName) in linuxSocket.c.
       */
      ASSERT(reply.nameLen <= sizeof(LinuxSocketName));

      memcpy(name, &reply.name, reply.nameLen);
      *nameLen = reply.nameLen;
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyGetPeerName --
 *
 *	Get the name of the connected peer.
 *
 * Results:
 *	name is set to the name of the given peer.  VMK_OK if okay,
 *	otherwise if error.
 *
 * Side-effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
UserProxyGetPeerName(UserObj* obj, LinuxSocketName* name,
		       uint32* nameLen)
{
   UserProxy_ObjInfo* info = obj->data.proxyInfo;
   VMK_ReturnStatus status;
   UserProxyGetnameMsg msg;
   UserProxyGetnameReply reply;

   ASSERT(info != NULL);

   UWLOG(1, "(name@%p, nameLen@%p=%d)", name, nameLen, *nameLen);

   msg.fdHdr.fileHandle = info->fileHandle;
   msg.nameLen = *nameLen;

   status = UserProxyRemoteCall(USERPROXY_GETPEERNAME, info->upci,
				&msg.fdHdr.hdr, sizeof msg, &reply.hdr,
				sizeof reply);
   if (status == VMK_OK) {
      if (reply.nameLen > *nameLen) {
         UWWarn("length returned (%d) is larger than the buffer size (%d)",
		reply.nameLen, *nameLen);
         return VMK_NAME_TOO_LONG;
      }

      ASSERT(reply.nameLen <= sizeof(LinuxSocketName));

      memcpy(name, &reply.name, reply.nameLen);
      *nameLen = reply.nameLen;
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyShutdown --
 *
 *	Shutdown part of a full-duplex connection.
 *
 * Results:
 *	VMK_OK if okay, otherwise if error.
 *
 * Side-effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
UserProxyShutdown(UserObj* obj, int how)
{
   UserProxyShutdownMsg msg;
   UserProxyShutdownReply reply;
   UserProxy_ObjInfo* info = obj->data.proxyInfo;

   ASSERT(info != NULL);

   UWLOG(1, "(how=%d)", how);

   msg.fdHdr.fileHandle = info->fileHandle;
   msg.how = how;

   return UserProxyRemoteCall(USERPROXY_SHUTDOWN, info->upci,
                              &msg.fdHdr.hdr, sizeof msg, &reply,
                              sizeof reply);
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxy_Uname --
 *
 *      Return system information.
 *
 * Results:
 *	VMK_OK if success, error status if failed.
 *
 * Side effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserProxy_Uname(struct User_CartelInfo *uci, LinuxUtsName *utsName)
{
   UserProxyUnameMsg msg;
   VMK_ReturnStatus status;
   UserProxyUnameReply reply;

   UWLOG(1, "(utsName=%p)", utsName);

   ASSERT(sizeof(utsName->sysname)  == sizeof(reply.buf.sysname));
   ASSERT(sizeof(utsName->nodename) == sizeof(reply.buf.nodename));
   ASSERT(sizeof(utsName->release)  == sizeof(reply.buf.release));
   ASSERT(sizeof(utsName->version)  == sizeof(reply.buf.version));
   ASSERT(sizeof(utsName->machine)  == sizeof(reply.buf.machine));
   ASSERT(sizeof(utsName->domainname) == sizeof(reply.buf.domainname));

   status = UserProxyRemoteCall(USERPROXY_UNAME, &uci->proxy,
                                &msg, sizeof msg,
                                &reply.hdr, sizeof reply);

   if (status == VMK_OK) {
      memcpy(utsName->sysname,  reply.buf.sysname,  LINUX_UTSNAME_LENGTH);
      memcpy(utsName->nodename, reply.buf.nodename, LINUX_UTSNAME_LENGTH);
      memcpy(utsName->release,  reply.buf.release,  LINUX_UTSNAME_LENGTH);
      memcpy(utsName->version,  reply.buf.version,  LINUX_UTSNAME_LENGTH);
      memcpy(utsName->machine,  reply.buf.machine,  LINUX_UTSNAME_LENGTH);
      memcpy(utsName->domainname, reply.buf.domainname, LINUX_UTSNAME_LENGTH);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyCanUseFastPath --
 *
 *	Examines a LinuxMsgHdr to see if it's applicable for the fast path.
 *	A LinuxMsgHdr is applicable for the fast path if it only has one
 *	io vector and no control information.  That way, we can handle off
 *	the user space io vector pointer directly to UserProxySend to
 *	minimize copying.
 *
 * Results:
 *	TRUE if it is, FALSE otherwise.
 *
 * Side-effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
UserProxyCanUseFastPath(LinuxMsgHdr* linuxMsg)
{
   return (linuxMsg->control == NULL && linuxMsg->iovLen == 1);
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyFlattenLinuxMsgHdr --
 *
 *	Takes a LinuxMsgHdr and copies it and its pointers into a
 *	UserProxySendmsgMsg.
 *
 * Results:
 *	VMK_OK if success, appropriate failure code otherwise.
 *
 * Side-effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserProxyFlattenLinuxMsgHdr(UserProxySendmsgMsg* msg, LinuxMsgHdr* linuxMsg,
			    Bool fastPath)
{
   uint32 msgOffset = 0;
   int i;
   VMK_ReturnStatus status = VMK_OK;

   /*
    * First try to copy the name.  It's OK if it isn't provided.
    */
   if (linuxMsg->name != NULL) {
      memcpy(&msg->name, linuxMsg->name, linuxMsg->nameLen);
      msg->nameLen = linuxMsg->nameLen;
   } else {
      memset(&msg->name, 0, sizeof msg->name);
      msg->nameLen = 0;
   }

   /*
    * Now copy the iovec, it's mandatory that they have at least one.
    */
   memcpy(msg->iov, linuxMsg->iov, linuxMsg->iovLen * sizeof(LinuxIovec));
   msg->iovLen = linuxMsg->iovLen;

   for (i = 0; i < msg->iovLen; i++) {
      if (!fastPath) {
         status = User_CopyIn(msg->data + msgOffset, linuxMsg->iov[i].base,
		     linuxMsg->iov[i].length);
	 if (status != VMK_OK) {
            return status;
	 }
      }
      msg->iov[i].offset = msgOffset;
      msgOffset += linuxMsg->iov[i].length;
   }

   /*
    * Get the control information.  Not mandatory that it be present.
    */
   if (linuxMsg->control != NULL) {
      memcpy(msg->data + msgOffset, linuxMsg->control, linuxMsg->controlLen);
      msg->controlOffset = msgOffset;
      msg->controlLen = linuxMsg->controlLen;
   } else {
      msg->controlOffset = 0;
      msg->controlLen = 0;
   }

   msg->flags = linuxMsg->flags;
   
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyExpandLinuxMsgHdr --
 *
 *	Takes a UserProxyRecvmsgReply and copies out the fields to
 *	pointers in a LinuxMsgHdr.
 *
 * Results:
 *	VMK_OK on success, appropriate failure code if we hit a snag copying.
 *
 * Side-effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserProxyExpandLinuxMsgHdr(UserProxyRecvmsgReply* reply, LinuxMsgHdr* linuxMsg,
			   Bool fastPath)
{
   /*
    * First try to copy the name.
    */
   if (linuxMsg->name != NULL) {
      // XXX Is this necessarily an error?
      if (reply->nameLen == 0) {
         return VMK_BAD_PARAM;
      }
      
      if (reply->nameLen > linuxMsg->nameLen) {
         return VMK_BAD_PARAM;
      }

      memcpy(linuxMsg->name, &reply->name, reply->nameLen);
      linuxMsg->nameLen = reply->nameLen;
   }

   /*
    * On to the iovec.
    */
   if (reply->iovLen == 0) {
      return VMK_BAD_PARAM;
   }

   if (fastPath) {
      linuxMsg->iov[0].length = reply->iov[0].length;
   } else {
      int i;

      for (i = 0; i < reply->iovLen; i++) {
         VMK_ReturnStatus status;
         ASSERT(reply->iov[i].length <= linuxMsg->iov[i].length);
         status = User_CopyOut(linuxMsg->iov[i].base,
			       reply->iov[i].offset + reply->data,
			       reply->iov[i].length);
	 if (status != VMK_OK) {
	    return status;
	 }
      }
   }

   /*
    * Now copy the control information.
    */
   if (linuxMsg->control != NULL) {
      // XXX Is this necessarily an error?
      if (reply->controlLen == 0) {
         return VMK_BAD_PARAM;
      }

      if (reply->controlLen > linuxMsg->controlLen) {
         return VMK_BAD_PARAM;
      }

      memcpy(linuxMsg->control, reply->data + reply->controlOffset,
	     reply->controlLen);
      linuxMsg->controlLen = reply->controlLen;
   }

   linuxMsg->flags = reply->flags;

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyParseOutgoingControlMessage --
 *
 *	Converts the file descriptor number from the UserWorld's (VMkernel's)
 *	number to the fileHandle for this file in the proxy.  Then the proxy
 *	will convert the fileHandle into a proxy descriptor number.
 *
 *	Note that it's possible for them to try to pass a non-proxied fd. This
 *	is bad.  If we can't convert the fd, we return an error.
 *
 * Results:
 *	VMK_OK on success, otherwise on error.
 *
 * Side-effects:
 *	*fd changed to proxy fileHandle.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserProxyParseOutgoingControlMessage(User_CartelInfo* uci, LinuxFd* fd)
{
   VMK_ReturnStatus status = VMK_OK;
   UserObj* passedObj;

   status = UserObj_Find(uci, *fd, &passedObj);
   if (status != VMK_OK) {
      UWLOG(0, "Trying to pass invalid file descriptor: %d", *fd);
      return status;
   }

   if (passedObj->type != USEROBJ_TYPE_PROXY_SOCKET &&
       passedObj->type != USEROBJ_TYPE_PROXY_FILE &&
       passedObj->type != USEROBJ_TYPE_PROXY_FIFO &&
       passedObj->type != USEROBJ_TYPE_ROOT) {
      UWLOG(0, "Trying to pass non-proxied file descriptor: %d\n", *fd);
      status = VMK_INVALID_HANDLE;
   } else {
      *fd = passedObj->data.proxyInfo->fileHandle;
   }

   (void) UserObj_Release(uci, passedObj);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyParseIncomingControlMessage --
 *
 *	Iterates through the list of file descriptors they're trying to pass,
 *	creates new UserObj's for each, and places them in the file descriptor
 *	table.  We have to make sure to preallocate everything to make cleanup
 *	easier if there's an error with allocating resources.
 *
 * Results:
 *	VMK_OK on success, otherwise on error.
 *
 * Side-effects:
 *	New UserObjs are created.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserProxyParseIncomingControlMessages(User_CartelInfo* uci, LinuxFd* fdptr,
				      int numfds)
{
   VMK_ReturnStatus status = VMK_OK;
   LinuxFd* reservedFds;
   UserObj** preallocObjs;
   int i;

   reservedFds = (LinuxFd*)User_HeapAlloc(uci, numfds * sizeof(LinuxFd));
   preallocObjs = (UserObj**)User_HeapAlloc(uci, numfds * sizeof(UserObj*));

   /*
    * Initialize 'em.
    */
   for (i = 0; i < numfds; i++) {
      reservedFds[i] = USEROBJ_INVALID_HANDLE;
      preallocObjs[i] = NULL;
   }

   /*
    * Go through and make sure we can allocate a fd and space for each new
    * object.
    */
   for (i = 0; i < numfds; i++) {
      reservedFds[i] = UserObj_FDReserve(uci);
      if (reservedFds[i] == USEROBJ_INVALID_HANDLE) {
	 status = VMK_NO_FREE_HANDLES;
	 break;
      }

      preallocObjs[i] = UserProxyObjPreallocate(uci);
      if (preallocObjs[i] == NULL) {
	 status = VMK_NO_MEMORY;
	 break;
      }
   }

   if (status == VMK_OK) {
      /*
       * Now actually create them.
       */
      for (i = 0; i < numfds; i++) {
         /*
	  * It's safe to assume that the incoming fd is a socket because the
	  * proxy checks for this.
	  */
	 UserProxySocketObjInit(uci, fdptr[i], preallocObjs[i],
				USERPROXY_INVALID_PCHANDLE);
	 UserObj_FDAddObj(uci, reservedFds[i], preallocObjs[i]);
	 fdptr[i] = reservedFds[i];
      }
   } else {
      /*
       * There was an error allocating resources, clean up and bail.
       */
      for (i = 0; i < numfds; i++) {
	 if (reservedFds[i] != USEROBJ_INVALID_HANDLE) {
	    UserObj_FDUnreserve(uci, reservedFds[i]);
	 }
	 if (preallocObjs[i] != NULL) {
	    UserProxyObjFreePreallocated(uci, preallocObjs[i]);
	 }
      }
   }

   User_HeapFree(uci, reservedFds);
   User_HeapFree(uci, preallocObjs);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyParseControlMessages --
 *
 *	Parses the control message for file descriptor passing and munges
 *	that data are necessary.
 *
 * Results:
 *	VMK_OK on success, otherwise on error.
 *
 * Side-effects:
 *	The control message will be modified, new objects may be created.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserProxyParseControlMessages(User_CartelInfo* uci, LinuxMsgHdr* linuxMsg,
			      Bool sending)
{
   VMK_ReturnStatus status = VMK_OK;
   LinuxControlMsgHdr* cmsg;

   for (cmsg = LinuxAPI_CmsgFirstHdr(linuxMsg); status == VMK_OK &&
        cmsg != NULL; cmsg = LinuxAPI_CmsgNextHdr(linuxMsg, cmsg)) {
      /*
       * We only care if file descriptors are being passed.
       */
      if (cmsg->level == LINUX_SOCKET_SOL_SOCKET &&
	  cmsg->type == LINUX_SOCKET_SCM_RIGHTS) {
         LinuxFd* fdptr;
         int numfds;

         fdptr = (int*)cmsg->data;
         numfds = (cmsg->length - sizeof(LinuxControlMsgHdr)) / sizeof(LinuxFd);

	 if (sending) {
            int i;

	    for (i = 0; i < numfds; i++) {
	       status = UserProxyParseOutgoingControlMessage(uci, &fdptr[i]);
	       if (status != VMK_OK) {
	          break;
	       }
	    }
	 } else {
	    status = UserProxyParseIncomingControlMessages(uci, fdptr, numfds);
	 }
      } else {
         UWWarn("Unsupported out-of-band control data passing: level: %d type: %d",
		cmsg->level, cmsg->type);
	 status = VMK_NOT_SUPPORTED;
      }
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxySendmsg --
 *
 *	Sends a message on the given socket.
 *
 * Results:
 *	VMK_OK on success, appropriate error code otherwise.
 *
 * Side-effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
UserProxySendmsg(UserObj* obj, LinuxMsgHdr* linuxMsg, uint32 len,
		 uint32* bytesSent)
{
   VMK_ReturnStatus status;
   UserProxy_ObjInfo* info = obj->data.proxyInfo;
   User_CartelInfo* uci = UserProxyUCIForUPCI(info->upci);
   UserProxySendmsgMsg *msg;
   UserProxySendmsgReply reply;
   Util_BufferType bufType;
   Bool useFastPath;
   RPC_Token token;
   uint32 msgHdrSize;
   uint32 msgSize;
   void *buffer;

   ASSERT(info != NULL);

   UWLOG(1, "(msg=%p, len=%u, bytesSent=%p)", linuxMsg, len, bytesSent);

   if (linuxMsg->controlLen > 0) {
      status = UserProxyParseControlMessages(uci, linuxMsg, TRUE);
      if (status != VMK_OK) {
         return status;
      }
   }

   useFastPath = UserProxyCanUseFastPath(linuxMsg);
   if (useFastPath) {
      /*
       * Fast path.  We're not sending any control information and we're only
       * sending from one buffer.  This probably means the user called send.
       * Try to minimize copying.
       */
      bufType = UTIL_USERWORLD_BUFFER;
      buffer = (void*)linuxMsg->iov[0].base;

      msgHdrSize = sizeof *msg;
      msgSize = msgHdrSize + len;
   } else {
      /*
       * Not so fast path.  Here we have an arbitrary number of io buffers
       * (limited to LINUX_MAX_IOVEC) of arbitrary size, as well as an arbitrary
       * sized control information buffer.
       */
      bufType = UTIL_VMKERNEL_BUFFER;
      buffer = NULL;

      msgSize = msgHdrSize = sizeof *msg + len + linuxMsg->controlLen;
   }

   msg = (UserProxySendmsgMsg*)User_HeapAlloc(uci, msgHdrSize);
   if (msg == NULL) {
      return VMK_NO_MEMORY;
   }
   msg->fdHdr.hdr.size = msgSize;
   msg->fdHdr.fileHandle = info->fileHandle;

   status = UserProxyFlattenLinuxMsgHdr(msg, linuxMsg, useFastPath);
   if (status != VMK_OK) {
      goto done;
   }

   UWSTAT_TIMERSTART(proxyCallTime);
   status = UserProxySend(USERPROXY_SENDMSG, info->upci, &msg->fdHdr.hdr,
			  msgHdrSize, useFastPath, buffer, bufType, &token);
   if (status == VMK_OK) {
      status = UserProxyReceive(USERPROXY_SENDMSG, info->upci, token,
      				&reply.pcHdr.hdr, sizeof reply, FALSE, NULL,
				UTIL_VMKERNEL_BUFFER);
   }
   UWSTAT_TIMERSTOP(proxyCallTime);
   if (status != VMK_OK) {
      goto done;
   }

   UserProxyKernelPollCacheUpdate(UserProxyPollCacheForObj(obj),
                                  &reply.pcHdr.pcUpdate);

   status = reply.pcHdr.hdr.status;
   if (status == VMK_OK) {
      *bytesSent = reply.bytesSent;
   }

done:
   User_HeapFree(uci, msg);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyRecvmsg --
 *
 *	Receives a message on the given socket.
 *
 * Results:
 *	VMK_OK on success, appropriate error code otherwise.
 *
 * Side-effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
UserProxyRecvmsg(UserObj* obj, LinuxMsgHdr* linuxMsg, uint32 len,
		 uint32* bytesRecv)
{
   VMK_ReturnStatus status;
   UserProxy_ObjInfo* info = obj->data.proxyInfo;
   User_CartelInfo* uci = UserProxyUCIForUPCI(info->upci);
   UserProxyRecvmsgMsg msg;
   UserProxyRecvmsgReply reply;
   UserProxyRecvmsgReply* replyPtr = &reply;
   uint32 replySize = sizeof(UserProxyRecvmsgReply);
   Bool freeReplyPtr = FALSE;
   Bool canUseFastPath;
   RPC_Token token;
   int i;

   ASSERT(info != NULL);
   ASSERT(linuxMsg != NULL);

   UWLOG(1, "(msg@%p {nameLen=%d, iovLen=%d ctlLen=%d, flags=%#x}, len=%u, bytesRecv@%p)",
         linuxMsg, linuxMsg->nameLen, linuxMsg->iovLen,
         linuxMsg->controlLen, linuxMsg->flags, len, bytesRecv);

   msg.varHdr.fdHdr.hdr.size = sizeof msg;
   msg.varHdr.fdHdr.fileHandle = info->fileHandle;
   msg.nameLen = linuxMsg->nameLen;
   msg.iovLen = linuxMsg->iovLen;
   for (i = 0; i < linuxMsg->iovLen; i++) {
      msg.iovDataLen[i] = linuxMsg->iov[i].length;
   }
   msg.controlLen = linuxMsg->controlLen;
   msg.dataLen = len;
   msg.varHdr.dataSize = msg.dataLen + msg.controlLen;
   msg.flags = linuxMsg->flags;

   canUseFastPath = UserProxyCanUseFastPath(linuxMsg);
   if (!canUseFastPath) {
      replySize = sizeof reply + len + linuxMsg->controlLen;

      UWLOG(2, " using not-so-fast path (replySize=%d)", replySize);

      /*
       * Do the allocation _before_ we send the message to the proxy so that if
       * we fail to allocate space, we won't leave the RPC queue full.
       */
      replyPtr = (UserProxyRecvmsgReply*)User_HeapAlloc(uci, replySize);
      if (replyPtr == NULL) {
         return VMK_NO_MEMORY;
      }
      freeReplyPtr = TRUE;
   }

   UWSTAT_TIMERSTART(proxyCallTime);
   status = UserProxySend(USERPROXY_RECVMSG, info->upci, &msg.varHdr.fdHdr.hdr,
			  sizeof msg, FALSE, 0, UTIL_VMKERNEL_BUFFER, &token);
   if (status != VMK_OK) {
      return status;
   }

   if (canUseFastPath){
      /*
       * Fast path.
       */
      UWLOG(2, " using fast path");
      status = UserProxyReceive(USERPROXY_RECVMSG, info->upci, token,
			        &reply.pcHdr.hdr, sizeof reply, TRUE,
				(void*)linuxMsg->iov[0].base,
                                UTIL_USERWORLD_BUFFER);
   } else {
      /*
       * Not so fast path.
       */
      status = UserProxyReceive(USERPROXY_RECVMSG, info->upci, token,
				&replyPtr->pcHdr.hdr, replySize, FALSE, NULL,
				UTIL_VMKERNEL_BUFFER);
   }
   UWSTAT_TIMERSTOP(proxyCallTime);
   if (status != VMK_OK) {
      goto out_error;
   }
   
   UserProxyKernelPollCacheUpdate(UserProxyPollCacheForObj(obj),
                                  &replyPtr->pcHdr.pcUpdate);

   status = replyPtr->pcHdr.hdr.status;
   if (status != VMK_OK) {
      goto out_error;
   }

   status = UserProxyExpandLinuxMsgHdr(replyPtr, linuxMsg, canUseFastPath);
   if (status != VMK_OK) {
      goto out_error;
   }

   if (linuxMsg->controlLen > 0) {
      status = UserProxyParseControlMessages(uci, linuxMsg, FALSE);
      if (status != VMK_OK) {
         goto out_error;
      }
   }

   *bytesRecv = replyPtr->bytesRecv;

out_error:
   if (freeReplyPtr) {
      User_HeapFree(uci, replyPtr);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxySetsockopt --
 *
 *	Set the given socket option to the given value.
 *
 * Results:
 *	None.
 *
 * Side-effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
UserProxySetsockopt(UserObj* obj, int level, int optName, char* optVal,
		    int optLen)
{
   VMK_ReturnStatus status;
   UserProxy_ObjInfo* info = obj->data.proxyInfo;
   User_CartelInfo* uci = UserProxyUCIForUPCI(info->upci);
   UserProxySetsockoptMsg* msg;
   UserProxySetsockoptReply reply;
   int size = sizeof(UserProxySetsockoptMsg) + optLen;

   ASSERT(info != NULL);

   UWLOG(1, "(level=%d, optName=%d, optVal=%p, optLen=%d)", level, optName,
	 optVal, optLen);

   msg = (UserProxySetsockoptMsg*)User_HeapAlloc(uci, size);
   if (msg == NULL) {
      return VMK_NO_MEMORY;
   }
   msg->fdHdr.fileHandle = info->fileHandle;
   msg->level = level;
   msg->optName = optName;
   msg->optLen = optLen;
   memcpy(msg->optVal, optVal, optLen);

   status = UserProxyRemoteCall(USERPROXY_SETSOCKOPT, info->upci,
				&msg->fdHdr.hdr, size, &reply, sizeof reply);
   User_HeapFree(uci, msg);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyGetsockopt --
 *
 *	Get the given socket option to the given value.
 *
 * Results:
 *	None.
 *
 * Side-effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
UserProxyGetsockopt(UserObj* obj, int level, int optName, char* optVal,
			  int* optLen)
{
   VMK_ReturnStatus status;
   UserProxy_ObjInfo* info = obj->data.proxyInfo;
   User_CartelInfo* uci = UserProxyUCIForUPCI(info->upci);
   UserProxyGetsockoptMsg msg;
   UserProxyGetsockoptReply* reply;
   int size = sizeof(UserProxyGetsockoptReply) + *optLen;

   ASSERT(info != NULL);

   UWLOG(1, "(level=%d, optName=%d, optVal=%p, optLen=%p)", level, optName,
	 optVal, optLen);

   msg.varHdr.fdHdr.fileHandle = info->fileHandle;
   msg.varHdr.dataSize = *optLen;
   msg.level = level;
   msg.optName = optName;
   //msg.optLen = *optLen;

   reply = (UserProxyGetsockoptReply*)User_HeapAlloc(uci, size);
   if (reply == NULL) {
      return VMK_NO_MEMORY;
   }

   status = UserProxyRemoteCall(USERPROXY_GETSOCKOPT, info->upci,
				&msg.varHdr.fdHdr.hdr, sizeof msg, &reply->hdr,
				size);
   if (status == VMK_OK) {
      if (reply->optLen > *optLen) {
         status = VMK_BAD_PARAM;
      } else {
         memcpy(optVal, reply->optVal, reply->optLen);
         *optLen = reply->optLen;
      }
   }

   User_HeapFree(uci, reply);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxy_OpenRoot --
 *
 *      Create a UserObj for "/", opened in USEROBJ_OPEN_STAT mode.
 *
 * Results:
 *      VMK_OK, with a UserObj returned in *objOut; or error status.
 *
 * Side effects:
 *      First time: allocates an object and stashes it in upci->root
 *      for reuse.  The object is freed in UserProxy_CartelCleanup.
 *      Every time: bumps the refcount of upci->root for the caller.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserProxy_OpenRoot(User_CartelInfo *uci, UserObj** objOut)
{
   VMK_ReturnStatus status = VMK_OK;

   if (uci->proxy.root == NULL) {
      UserProxyOpenMsg msg;
      UserProxyOpenReply reply;

      // Note small code overlap with UserProxyOpen
      strcpy(msg.name, "/");
      msg.flags = USEROBJ_OPEN_STAT; 
      msg.mode = 0;
      status = UserProxyRemoteCall(USERPROXY_OPEN, &uci->proxy,
                                &msg.hdr, sizeof(msg),
                                &reply.hdr, sizeof(reply));
      if (status != VMK_OK) {
         return status;
      }
      status = UserProxyObjCreate(uci, &uci->proxy, USEROBJ_TYPE_ROOT,
                                  reply.fileHandle, "/", strlen("/"),
                                  &uci->proxy.root, USEROBJ_OPEN_STAT,
				  USERPROXY_INVALID_PCHANDLE);
      if (status != VMK_OK) {
         return status;
      }
   }

   UserObj_Acquire(uci->proxy.root);
   *objOut = uci->proxy.root;
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyRootOpen --
 *
 *      Open the specified arc relative to "/".
 *
 * Results:
 *      VMK_OK if the object can be found, error condition otherwise
 *
 * Side effects:
 *      UserObj for the object may be allocated.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserProxyRootOpen(UserObj* parent,
                  const char* arc, uint32 flags, LinuxMode mode,
                  UserObj** objOut)
{
   // XXX: Always called from a UserWorld context, I think...
   User_CartelInfo* uci = MY_USER_CARTEL_INFO;
   ASSERT(World_IsUSERWorld(MY_RUNNING_WORLD));

   UWLOG(1, "(arc=%s, flags=%#x, mode=%#x)", arc, flags, mode);
   ASSERT(parent->type == USEROBJ_TYPE_ROOT);

   if (strcmp(arc, "vmfs") == 0) {
      *objOut = UserFile_OpenVMFSRoot(uci, flags);
      return VMK_OK;
   }

   /* Pass through all other cases to the proxy */
   return UserProxyOpen(parent, arc, flags, mode, objOut);
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyRootGetName --
 *
 *      Get the name of / relative to /.
 *
 * Results:
 *      VMK_OK.  Data returned in arc.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserProxyRootGetName(UserObj* obj, char* arc, uint32 length)
{
   strcpy(arc, "");
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyCreateSpecialFd --
 *
 *      Create a UserObj for one of the special (stdin, stdout, stderr) 
 *      file descriptors.  The proxy app knows that fileHandles 0, 1, 2
 *      are special, so no communication is necessary.
 *
 * Results:
 *      VMK_OK or error condition.
 *
 * Side effects:
 *      As above.
 *
 *----------------------------------------------------------------------
 */

static void
UserProxyCreateSpecialFd(User_CartelInfo *uci, int fd, uint32 openFlags,
			 UserProxyObjType proxyType)
{
   UserObj* obj;
   int assignedFd;
   VMK_ReturnStatus status;
   char name[25];
   int len;
   UserObj_Type type = (UserObj_Type)proxyType;

   ASSERT(USERPROXY_TYPE_NONE == USEROBJ_TYPE_NONE);
   ASSERT(USERPROXY_TYPE_FILE == USEROBJ_TYPE_PROXY_FILE);
   ASSERT(USERPROXY_TYPE_FIFO == USEROBJ_TYPE_PROXY_FIFO);
   ASSERT(USERPROXY_TYPE_CHAR == USEROBJ_TYPE_PROXY_CHAR);
   ASSERT(USERPROXY_TYPE_SOCKET == USEROBJ_TYPE_PROXY_SOCKET);

   ASSERT_NOT_IMPLEMENTED(fd >= 0 && fd <= 2);

   len = snprintf(name, sizeof name, "<special fd %d>", fd);
   ASSERT(len < sizeof name);
   
   assignedFd = UserObj_FDReserve(uci);
   ASSERT_NOT_IMPLEMENTED(assignedFd == fd);

   status = UserProxyObjCreate(uci, &uci->proxy, type, fd, name, len, &obj,
			       openFlags, USERPROXY_INVALID_PCHANDLE);
   ASSERT_NOT_IMPLEMENTED(status == VMK_OK);

   UserObj_FDAddObj(uci, fd, obj);
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxy_CreateSpecialFds --
 *
 *	Creates the special fds (stdin, stdout, stderr) using the
 *	specified types.
 *
 * Results:
 *      VMK_OK.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
UserProxy_CreateSpecialFds(World_Handle *world,
			   UserProxyObjType inType,
			   UserProxyObjType outType,
			   UserProxyObjType errType)
{
   User_CartelInfo *uci = world->userCartelInfo;
   LinuxFd reservedInFd = USEROBJ_INVALID_HANDLE;
   LinuxFd reservedOutFd = USEROBJ_INVALID_HANDLE;

   ASSERT(world != MY_RUNNING_WORLD);

   if (! World_IsUSERWorld(world)) {
      return VMK_NOT_FOUND;
   }

   /*
    * Create the special fds as specified by the given types.  If the type is
    * USERPROXY_TYPE_NONE, then the fd isn't opened.  If this is the case, we
    * still reserve the fd for the duration of this function.
    */
   if (inType != USERPROXY_TYPE_NONE) {
      UserProxyCreateSpecialFd(uci, 0, USEROBJ_OPEN_RDONLY, inType);
   } else {
      reservedInFd = UserObj_FDReserve(uci);
      ASSERT_NOT_IMPLEMENTED(reservedInFd != USEROBJ_INVALID_HANDLE);
   }

   if (outType != USERPROXY_TYPE_NONE) {
      UserProxyCreateSpecialFd(uci, 1, USEROBJ_OPEN_WRONLY, outType);
   } else {
      reservedOutFd = UserObj_FDReserve(uci);
      ASSERT_NOT_IMPLEMENTED(reservedOutFd != USEROBJ_INVALID_HANDLE);
   }

   if (errType != USERPROXY_TYPE_NONE) {
      UserProxyCreateSpecialFd(uci, 2, USEROBJ_OPEN_WRONLY, errType);
   }

   if (reservedOutFd != USEROBJ_INVALID_HANDLE) {
      UserObj_FDUnreserve(uci, reservedOutFd);
   }

   if (reservedInFd != USEROBJ_INVALID_HANDLE) {
      UserObj_FDUnreserve(uci, reservedInFd);
   }

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxyDoExitNotify --
 *
 *      Send on the given RPC connection a message containing the
 *	given exit state for a world.
 *
 * Results:
 *      Status of the RPC call.
 *
 * Side effects:
 *      A message is sent.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserProxyDoExitNotify(World_ID cartelID,
                      int exitStatus,
                      int exceptionType,
                      const VMKFullUserExcFrame* fullFrame,
                      const char* coreDumpName)
{
   User_PostExitInfo exitInfo;
   int usedLen = strlen(coreDumpName);
   
   UWLOG(2, "informing COS of world %d's death: exitCode=%d; exception=%s; core=%s",
	 cartelID, exitStatus, (fullFrame != NULL ? "YES" : "no"),
	 coreDumpName);

   exitInfo.type = USER_MSG_POSTEXIT;
   exitInfo.status = exitStatus;
   if (fullFrame != NULL) {
      exitInfo.wasException = TRUE;
      exitInfo.exceptionType = exceptionType;
      exitInfo.cs = fullFrame->frame.cs;
      exitInfo.eip = fullFrame->frame.eip;
      exitInfo.ss = ((VMKUserExcFrame *)(&fullFrame->frame.errorCode))->ss;
      exitInfo.esp = ((VMKUserExcFrame *)(&fullFrame->frame.errorCode))->esp;
      exitInfo.ds = fullFrame->regs.ds;
      exitInfo.es = fullFrame->regs.es;
      exitInfo.fs = fullFrame->regs.fs;
      exitInfo.gs = fullFrame->regs.gs;
      exitInfo.eax = fullFrame->regs.eax;
      exitInfo.ebx = fullFrame->regs.ebx;
      exitInfo.ecx = fullFrame->regs.ecx;
      exitInfo.edx = fullFrame->regs.edx;
      exitInfo.ebp = fullFrame->regs.ebp;
      exitInfo.esi = fullFrame->regs.esi;
      exitInfo.edi = fullFrame->regs.edi;
   } else {
      exitInfo.wasException = FALSE;
   }
   
   if (usedLen > 0) {
      /*
       * Stick as much of the name as will fit.  This is just an
       * informational message (and it has to fit in an RPC buffer).
       * Dump paths can be much longer.
       */
      if (usedLen < USER_MAX_DUMPNAME_LENGTH) {
         snprintf(exitInfo.coreDumpName, USER_MAX_DUMPNAME_LENGTH, "%s", coreDumpName);
      } else {
         static const int prefixlen = USER_MAX_DUMPNAME_LENGTH / 10; // 10% at front
         snprintf(exitInfo.coreDumpName, USER_MAX_DUMPNAME_LENGTH,
                  "%*s...%s", prefixlen, coreDumpName,
                  coreDumpName + (usedLen - (USER_MAX_DUMPNAME_LENGTH - prefixlen - 4)));
      }
      exitInfo.coreDump = TRUE;
   } else {
      exitInfo.coreDump = FALSE;
   }

   return UserProxy_SendStatusAlert(cartelID, &exitInfo, sizeof exitInfo);
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxy_CartelCleanup --
 *
 *	Shuts down the proxy connection:
 *	  - closes the root directory ("/"),
 *	  - sends the exit message to the proxy informing it it can exit,
 *	  - closes the proxy RPC connections for this cartel.
 *
 * Results:
 *      VMK_OK.
 *
 * Side effects:
 *      As above.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus 
UserProxy_CartelCleanup(User_CartelInfo *uci)
{
   UserProxy_CartelInfo *upci = &uci->proxy;

   /*
    * Close the root directory if open.
    */
   if (upci->root) {
      ASSERT(Atomic_Read(&upci->root->refcount) == 1);
      (void) UserObj_Release(uci, upci->root);
      upci->root = NULL;
   }

   /*
    * Send the exit message to the proxy.
    */
   if (UserProxyDoExitNotify(uci->cartelID,
                             uci->shutdown.exitCode,
                             uci->shutdown.exceptionType,
                             (uci->shutdown.hasException ?
                              &uci->shutdown.exceptionFrame : NULL),
                             uci->coreDump.dumpName) != VMK_OK) {
      UWWarn("Failed to send proxy exit message.  Proxy may have to be killed manually.");
   }

   UserProxyForceDisconnect(upci);

   Semaphore_Cleanup(&upci->sema);

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * UserProxy_CartelInit --
 *
 *      Open the proxy RPC connection associated with this cartel.
 *
 * Results:
 *      VMK_OK or error condition.
 *
 * Side effects:
 *      As above.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus 
UserProxy_CartelInit(User_CartelInfo *uci)
{
   UserProxy_CartelInfo *upci = &uci->proxy;

   upci->cnxToProxyID = -1;
   upci->cnxToKernelID = -1;
   upci->cartelID = uci->cartelID;
   upci->disconnected = FALSE;
   upci->cosPid = -1;
   upci->uci = uci;

   Semaphore_Init("UserProxy Send", &upci->sema, 1, UW_SEMA_RANK_PROXY);

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxy_ObjReady --
 *	
 *      Callback from the proxy to signal that the given fileHandle on
 *      the given world/cartel is ready for with the given events.
 *      
 *	Prototype is in vmkernel/private/user.h
 *      
 * Results:
 *	VMK_OK if world is a UserWorld, running and wakeup is
 *	delivered.  Otherwise on error.
 *
 * Side effects:
 *	Any waiting worlds are woken
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserProxy_ObjReady(World_Handle* world,
                   uint32 fileHandle,
                   UserProxyPollCacheUpdate *pcUpdate)
{
   VMK_ReturnStatus status;

   ASSERT(world != MY_RUNNING_WORLD);

   if (! World_IsUSERWorld(world)) {
      return VMK_NOT_FOUND;
   }
      
   status = UserProxyWakeupPollWaiters(world, fileHandle, pcUpdate);
   
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxy_SetCosProxyPid --
 *
 *	Called to save the pid of the cos proxy.
 *
 *	Prototype is in vmkernel/private/user.h
 *
 * Results:
 *	VMK_OK if world is a UserWorld.  Otherwise on error.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
UserProxy_SetCosProxyPid(World_Handle *world,
			 int cosPid)
{
   ASSERT(world != MY_RUNNING_WORLD);

   if (!World_IsUSERWorld(world)) {
      return VMK_NOT_FOUND;
   }

   ASSERT(world->userCartelInfo->proxy.cosPid == -1);
   world->userCartelInfo->proxy.cosPid = cosPid;
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxy_GetCosProxyPid --
 *
 *	Returns the pid of the cos proxy.
 *
 * Results:
 *	cos proxy pid.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
UserProxy_GetCosProxyPid(User_CartelInfo *uci)
{
   ASSERT(uci->proxy.cosPid != -1);
   return uci->proxy.cosPid;
}


/*
 *----------------------------------------------------------------------
 *
 * UserProxy_IsCosPidAlive --
 *
 *	Determines whether the cos process represented by the given pid
 *	is running.
 *
 * Results:
 *	VMK_OK if so, VMK_NOT_FOUND if it's not running.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
UserProxy_IsCosPidAlive(User_CartelInfo *uci, int cosPid)
{
   VMK_ReturnStatus status;
   UserProxyIsPidAliveMsg msg;
   UserProxyIsPidAliveReply reply;

   msg.pid = cosPid;

   status = UserProxyRemoteCall(USERPROXY_ISPIDALIVE, &uci->proxy, &msg.hdr,
				sizeof msg, &reply, sizeof reply);
   return status;
}
