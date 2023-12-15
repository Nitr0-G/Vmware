/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/* 
 * userObj.h - 
 *	UserWorld file/network/rpc/pipe/stdio objects
 */

#ifndef VMKERNEL_USER_USEROBJ_H
#define VMKERNEL_USER_USEROBJ_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "vm_basic_types.h"
#include "vmkernel.h"
#include "linuxAPI.h"
#include "userProxy_ext.h"
#include "userProxy.h"
#include "vmkpoll.h"

#define USEROBJ_MAX_HANDLES	USERPROXY_MAX_OBJECTS
#define USEROBJ_INVALID_HANDLE	-1
/*
 * Used to reserve a place in the file descriptors table.  This should only be
 * used temporarily (and replaced with a real UserObj).
 */
#define USEROBJ_RESERVED_HANDLE	(UserObj*)-2

struct User_CartelInfo;
struct World_Handle;
struct UserPipe_Buf;
struct UserProxy_ObjInfo;
struct UserFile_ObjInfo;
struct UserSocketInet_ObjInfo;
struct UserSocketUnix_Socket;
struct UserSocketUnix_DataSocket;
struct UserSocketUnix_ServerSocket;
struct UserDump_Header;
struct UserDump_DumpData;

#define USEROBJ_TYPES \
   DEFINE_USEROBJ_TYPE(NONE)                                                   \
   DEFINE_USEROBJ_PROXY_TYPE(FILE)         /* cos proxied files and dirs */    \
   DEFINE_USEROBJ_PROXY_TYPE(FIFO)         /* cos proxied fifos */             \
   DEFINE_USEROBJ_PROXY_TYPE(SOCKET)       /* cos proxied sockets */           \
   DEFINE_USEROBJ_PROXY_TYPE(CHAR)         /* cos proxied character dev */     \
   DEFINE_USEROBJ_TYPE(SOCKET_INET)        /* sockets using vmk tcp/ip stack */\
   DEFINE_USEROBJ_TYPE(SOCKET_UNIX)        /* generic unix socket, will turn */\
                                           /* into one of the following two. */\
   DEFINE_USEROBJ_TYPE(SOCKET_UNIX_DATA)   /* data connection socket */        \
   DEFINE_USEROBJ_TYPE(SOCKET_UNIX_SERVER) /* connection-accepting socket */   \
   DEFINE_USEROBJ_TYPE(FILE)               /* vmfs file */                     \
   DEFINE_USEROBJ_TYPE(PIPEREAD)                                               \
   DEFINE_USEROBJ_TYPE(PIPEWRITE)                                              \
   DEFINE_USEROBJ_TYPE(ROOT)               /* "/" directory */                 \
   DEFINE_USEROBJ_TYPE(RPC)                                                    \
   DEFINE_USEROBJ_TYPE(TERM)               /* native vmkernel terminal */      \
   DEFINE_USEROBJ_TYPE(MAXIMUMTYPE)        /* must be last */

/*
 * Type-flag for the discriminated union that is the UserObj.
 */
#define DEFINE_USEROBJ_TYPE(_name) USEROBJ_TYPE_ ## _name,
#define DEFINE_USEROBJ_PROXY_TYPE(_name) \
   USEROBJ_TYPE_PROXY_ ## _name = USERPROXY_TYPE_ ## _name,
typedef enum {
   USEROBJ_TYPES
} UserObj_Type;
#undef DEFINE_USEROBJ_PROXY_TYPE
#undef DEFINE_USEROBJ_TYPE

/*
 * Raw underyling handle of a UserObj
 */
typedef union UserObj_Data {
   uint64 raw;
   int socket;
   int stdioID;
   struct UserPipe_Buf* pipeBuf;
   struct UserFile_ObjInfo *vmfsObject;
   RPC_Connection rpcCnx;
   struct UserProxy_ObjInfo *proxyInfo;
   struct UserSocketInet_ObjInfo *socketInetInfo;
   struct UserSocketUnix_Socket *socketUnix;
   struct UserSocketUnix_DataSocket *socketUnixData;
   struct UserSocketUnix_ServerSocket *socketUnixServer;
   RPC_Connection cnxID;
} UserObj_Data;

/*
 * Actions to be taken by poll handlers.
 */
typedef enum {
   UserObjPollNoAction, // poll callback should do nothing 
   UserObjPollNotify,   // poll callback should register current world on obj
   UserObjPollCleanup   // poll callback should cleanup registration on obj
} UserObjPollAction;

/*
 * Method suite for a UserObj
 *
 * Note: In general, access checking is required in open, chmod,
 * chown, truncate, utime, unlink, mkdir, rmdir, readSymLink,
 * makeSymLink, makeHardLink, rename, and mknod.  The other methods
 * can assume that any necessary checking was done at open time.
 */
typedef struct UserObj UserObj; // forward

/*
 * Open/create a file or open another object relative to obj.  If arc
 * names a symbolic link, do not follow it; instead return
 * VMK_IS_A_SYMLINK.
 */
typedef VMK_ReturnStatus (*UserObj_OpenMethod)
     (UserObj* obj, const char *arc,
      uint32 flags, LinuxMode mode, UserObj **objOut);

/*
 * Last close of obj; free underlying structure.  (Note: uci argument
 * is needed because this method may be called through WorldReap in a
 * helper world.)
 */
typedef VMK_ReturnStatus (*UserObj_CloseMethod)(UserObj* obj,
                                                struct User_CartelInfo *uci);

/* Read from file obj. */
typedef VMK_ReturnStatus (*UserObj_ReadMethod)
     (UserObj* obj, UserVA userData, uint64 offset, uint32 length,
      uint32 *bytesRead);

/* Page-aligned read from file obj, directly into an MPN. */
typedef VMK_ReturnStatus (*UserObj_ReadMPNMethod)
     (UserObj* obj, MPN mpn, uint64 offset, uint32 *bytesRead);

/* Write to file obj. */
typedef VMK_ReturnStatus (*UserObj_WriteMethod)
     (UserObj* obj, UserVAConst userData, uint64 offset, uint32 length,
      uint32 *bytesWritten);

/* Page-aligned write from an MPN directly into a file obj. */
typedef VMK_ReturnStatus (*UserObj_WriteMPNMethod)
     (UserObj* obj, MPN mpn, uint64 offset, uint32 *bytesWritten);

/* Get information about obj. */
typedef VMK_ReturnStatus (*UserObj_StatMethod)
     (UserObj* obj, LinuxStat64* statbuf);

/* Change access control mode bits of obj. */
typedef VMK_ReturnStatus (*UserObj_ChmodMethod)(UserObj* obj, LinuxMode mode);

/* Change owner and/or group of obj.  -1 => no change. */
typedef VMK_ReturnStatus (*UserObj_ChownMethod)
     (UserObj* obj, Identity_UserID owner, Identity_GroupID group);

/* Change size of obj. */
typedef VMK_ReturnStatus (*UserObj_TruncateMethod)
     (UserObj* obj, uint64 size);

/* Change atime and mtime of obj. */
typedef VMK_ReturnStatus (*UserObj_UtimeMethod)
     (UserObj* obj, uint32 atime, uint32 mtime);

/* Get information about filesystem where obj resides. */
typedef VMK_ReturnStatus (*UserObj_StatFSMethod)
     (UserObj* obj, LinuxStatFS64* statbuf);

/* Check if obj is ready; if not, set up notification. */
typedef VMK_ReturnStatus (*UserObj_PollMethod)(UserObj* obj,
					       VMKPollEvent inEvents,
					       VMKPollEvent* outEvents,
					       UserObjPollAction action);

/* Remove the link named arc from the directory obj. */
typedef VMK_ReturnStatus (*UserObj_UnlinkMethod)(UserObj* obj,
                                                 const char *arc);

/* Create a subdirectory of obj. */
typedef VMK_ReturnStatus (*UserObj_MkdirMethod)(UserObj* obj, const char *arc,
                                                LinuxMode mode);

/* Remove a subdirectory of obj. */
typedef VMK_ReturnStatus (*UserObj_RmdirMethod)(UserObj* obj,
                                                const char *arc);

/*
 * Get the name of obj relative to its parent directory.  Used only by
 * the getcwd() system call, so does not need to be implemented on
 * anything but directories.
*/
typedef VMK_ReturnStatus (*UserObj_GetNameMethod)(UserObj* obj,
                                                  char* arc,
						  uint32 length);

/* Read a symbolic link in the directory obj. */
typedef VMK_ReturnStatus (*UserObj_ReadSymLinkMethod)
   (UserObj* obj, const char *arc, char *buf, unsigned buflen);

/* Insert a symbolic link into the directory obj. */
typedef VMK_ReturnStatus (*UserObj_MakeSymLinkMethod)
   (UserObj* obj, const char *arc, const char *link);

/* Make a hard link in the directory obj to target. */
typedef VMK_ReturnStatus (*UserObj_MakeHardLinkMethod)
   (UserObj* obj, const char *arc, UserObj* target);

/* Rename (oldDir, oldArc) to (newDir, newArc).  
   Called as a method of newObj.*/
typedef VMK_ReturnStatus (*UserObj_RenameMethod)
   (UserObj* newDir, const char *newArc, UserObj* oldDir, const char *oldArc);

/* Creates special files. */
typedef VMK_ReturnStatus (*UserObj_MknodMethod)
   (UserObj* obj, char* arc, LinuxMode mode);

/* Performs various miscellaneous operations on fd. */
typedef VMK_ReturnStatus (*UserObj_FcntlMethod)
   (UserObj* obj, uint32 cmd, uint32 arg);

/* Force buffered writes on obj to disk. */
typedef VMK_ReturnStatus (*UserObj_FsyncMethod)(UserObj* obj, Bool dataOnly);

/* Read a portion of a directory */
typedef VMK_ReturnStatus (*UserObj_ReadDirMethod)
     (UserObj* obj, UserVA /* LinuxDirent64* */ userData,
      uint32 length, uint32* bytesRead);

/* Universal escape for type-specific operations -- ugh */
typedef VMK_ReturnStatus (*UserObj_IoctlMethod)
     (UserObj* obj, uint32 cmd, LinuxIoctlArgType type,
      uint32 size, void *userData, uint32 *result);

/* Generate a string reperesentation of this object. */
typedef VMK_ReturnStatus (*UserObj_ToStringMethod)
     (UserObj *obj, char *string, int length);

/*
 * Socket-specific methods.
 */

/* Bind a socket to an address. */
typedef VMK_ReturnStatus (*UserObj_BindMethod)(UserObj* obj,
                                               LinuxSocketName* name,
                                               uint32 nameLen);

/* Connects a socket to the given address. */
typedef VMK_ReturnStatus (*UserObj_ConnectMethod)(UserObj* Obj,
                                                  LinuxSocketName* name,
                                                  uint32 nameLen);

/* Connects two sockets. */
typedef VMK_ReturnStatus (*UserObj_SocketpairMethod)(UserObj* obj1,
                                                     UserObj* obj2);

/* Block waiting for a socket to connect to the socket, returns remote socket.*/
typedef VMK_ReturnStatus (*UserObj_AcceptMethod)(UserObj* obj,
                                                 UserObj** newObj,
                                                 LinuxSocketName* name,
                                                 uint32* nameLen);

/* Returns the name of this socket. */
typedef VMK_ReturnStatus (*UserObj_GetSocketNameMethod)(UserObj* obj,
							LinuxSocketName* name,
							uint32* nameLen);

/* Enables a socket to accept incoming connections. */
typedef VMK_ReturnStatus (*UserObj_ListenMethod)(UserObj* obj,
						 int backlog);

/* Sets socket-specific options. */
typedef VMK_ReturnStatus (*UserObj_SetsockoptMethod)(UserObj* obj,
						     int level,
					             int optName,
					             char* optVal,
					             int optLen);

/* Gets socket-specific options. */
typedef VMK_ReturnStatus (*UserObj_GetsockoptMethod)(UserObj* obj,
					             int level,
					             int optName,
					             char* optVal,
					             int* optLen);

/* Sends a message over the socket's connection. */
typedef VMK_ReturnStatus (*UserObj_SendmsgMethod)(UserObj* obj,
						  LinuxMsgHdr* msg,
						  uint32 msgLen,
						  uint32* bytesSent);

/* Receives a message from the socket's connection. */
typedef VMK_ReturnStatus (*UserObj_RecvmsgMethod)(UserObj* obj,
						  LinuxMsgHdr* msg,
						  uint32 msgLen,
						  uint32* bytesRecv);

/* Returns peer name of this socket. */
typedef VMK_ReturnStatus (*UserObj_GetPeerNameMethod)(UserObj* obj,
                                                      LinuxSocketName* name,
                                                      uint32* nameLen);

/* Shut down part of a connection. */
typedef VMK_ReturnStatus (*UserObj_ShutdownMethod)(UserObj* obj,
                                                   int how);

/*
 * Use this macro to declare a method suite so that the compiler
 * checks that all methods are supplied.  Otherwise if you leave out
 * some at the end, NULL is silently filled in.
 */
#define USEROBJ_METHODS(m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, \
                        m13, m14, m15, m16, m17, m18, m19, m20, m21, m22,  \
                        m23, m24, m25, m26, m27, m28, m29, m30, m31, m32,  \
                        m33, m34, m35, m36, m37, m38, m39) \
                      { m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, \
                        m13, m14, m15, m16, m17, m18, m19, m20, m21, m22,  \
                        m23, m24, m25, m26, m27, m28, m29, m30, m31, m32,  \
                        m33, m34, m35, m36, m37, m38, m39}

typedef struct UserObj_Methods {
   UserObj_OpenMethod open;
   UserObj_CloseMethod close;
   UserObj_ReadMethod read;
   UserObj_ReadMPNMethod readMPN;
   UserObj_WriteMethod write;
   UserObj_WriteMPNMethod writeMPN;
   UserObj_StatMethod stat;
   UserObj_ChmodMethod chmod;
   UserObj_ChownMethod chown;
   UserObj_TruncateMethod truncate;
   UserObj_UtimeMethod utime;
   UserObj_StatFSMethod statFS;
   UserObj_PollMethod poll;
   UserObj_UnlinkMethod unlink;
   UserObj_MkdirMethod mkdir;
   UserObj_RmdirMethod rmdir;
   UserObj_GetNameMethod getName;
   UserObj_ReadSymLinkMethod readSymLink;
   UserObj_MakeSymLinkMethod makeSymLink;
   UserObj_MakeHardLinkMethod makeHardLink;
   UserObj_RenameMethod rename;
   UserObj_MknodMethod mknod;
   UserObj_FcntlMethod fcntl;
   UserObj_FsyncMethod fsync;
   UserObj_ReadDirMethod readDir;
   UserObj_IoctlMethod ioctl;
   UserObj_ToStringMethod toString;
   UserObj_BindMethod bind;
   UserObj_ConnectMethod connect;
   UserObj_SocketpairMethod socketpair;
   UserObj_AcceptMethod accept;
   UserObj_GetSocketNameMethod getSocketName;
   UserObj_ListenMethod listen;
   UserObj_SetsockoptMethod setsockopt;
   UserObj_GetsockoptMethod getsockopt;
   UserObj_SendmsgMethod sendmsg;
   UserObj_RecvmsgMethod recvmsg;
   UserObj_GetPeerNameMethod getPeerName;
   UserObj_ShutdownMethod shutdown;
} UserObj_Methods;

struct UserObj {
   Semaphore sema;  // serializes reads and writes, protecting offset and cache
   uint64 offset;
   uint32 openFlags;
   Atomic_uint32 refcount;
   UserObj_Type	type;
   UserObj_Data	data;
   UserObj_Methods *methods;
};

typedef struct UserObj_State {
   SP_SpinLock	lock;
   UserObj*	descriptors[USEROBJ_MAX_HANDLES];
   UserObj*     cwd; // current working directory
   uint32       umask;
   Timer_Handle fileTimer;
} UserObj_State;

/* Flags for open method and UserObj_Open */
#define USEROBJ_OPEN_RDONLY      USERPROXY_OPEN_RDONLY
#define USEROBJ_OPEN_WRONLY      USERPROXY_OPEN_WRONLY
#define USEROBJ_OPEN_RDWR        USERPROXY_OPEN_RDWR
#define USEROBJ_OPEN_STAT        USERPROXY_OPEN_STAT
#define USEROBJ_OPEN_FOR         USERPROXY_OPEN_FOR
#define USEROBJ_OPEN_CREATE      USERPROXY_OPEN_CREATE
#define USEROBJ_OPEN_EXCLUSIVE   USERPROXY_OPEN_EXCLUSIVE
#define USEROBJ_OPEN_NOCTTY      USERPROXY_OPEN_NOCTTY
#define USEROBJ_OPEN_TRUNCATE    USERPROXY_OPEN_TRUNCATE
#define USEROBJ_OPEN_APPEND      USERPROXY_OPEN_APPEND
#define USEROBJ_OPEN_NONBLOCK    USERPROXY_OPEN_NONBLOCK
#define USEROBJ_OPEN_SYNC        USERPROXY_OPEN_SYNC
#define USEROBJ_OPEN_LARGEFILE   USERPROXY_OPEN_LARGEFILE
#define USEROBJ_OPEN_DIRECTORY   USERPROXY_OPEN_DIRECTORY
#define USEROBJ_OPEN_NOFOLLOW    USERPROXY_OPEN_NOFOLLOW
#define USEROBJ_OPEN_ASYNC       USERPROXY_OPEN_ASYNC
#define USEROBJ_OPEN_DIRECT      USERPROXY_OPEN_DIRECT
#define USEROBJ_OPEN_VMFSFILE    USERPROXY_OPEN_VMFSFILE
#define USEROBJ_OPEN_SUPPORTED   USERPROXY_OPEN_SUPPORTED
#define USEROBJ_OPEN_PENULTIMATE USERPROXY_OPEN_PENULTIMATE
#define USEROBJ_OPEN_IGNTRAILING USERPROXY_OPEN_IGNTRAILING
#define USEROBJ_OPEN_SEARCH      USERPROXY_OPEN_SEARCH
#define USEROBJ_OPEN_OWNER       USERPROXY_OPEN_OWNER
#define USEROBJ_OPEN_GROUP       USERPROXY_OPEN_GROUP

/*
 * In Linux, fcntl(F_SETFL) can only affect these flags.
 */
#define USEROBJ_FCNTL_SETFL_LINUX_SUPPORTED (USEROBJ_OPEN_APPEND | \
					     USEROBJ_OPEN_NONBLOCK | \
					     USEROBJ_OPEN_ASYNC | \
					     USEROBJ_OPEN_DIRECT)

/*
 * Currently, we only support a subset of these.
 */
#define USEROBJ_FCNTL_SETFL_VMK_SUPPORTED (USEROBJ_OPEN_APPEND | \
					   USEROBJ_OPEN_NONBLOCK)

/* Whence values for UserObj_FDSeek */
#define USEROBJ_SEEK_SET 0
#define USEROBJ_SEEK_CUR 1
#define USEROBJ_SEEK_END 2

/* Maximum number of symlink indirections when traversing a path */
#define USEROBJ_SYMLINK_LIMIT    10

extern VMK_ReturnStatus UserObj_CartelInit(struct User_CartelInfo* uci);
extern VMK_ReturnStatus UserObj_CartelCleanup(struct User_CartelInfo* uci);

extern UserObj* UserObj_Create(struct User_CartelInfo* uci, UserObj_Type type,
                               UserObj_Data data, UserObj_Methods* methods,
                               uint32 openFlags);
extern void UserObj_InitObj(UserObj* obj, UserObj_Type type,
                            UserObj_Data data, UserObj_Methods* methods,
                            uint32 openFlags);
extern VMK_ReturnStatus UserObj_Find(struct User_CartelInfo* uci,
                                     LinuxFd fd, struct UserObj **objret);
extern VMK_ReturnStatus UserObj_FdForObj(struct User_CartelInfo *uci,
					 struct UserObj *obj, LinuxFd *outFd);
extern UserObj* UserObj_AcquireCwd(struct User_CartelInfo* uci);

extern VMK_ReturnStatus UserObj_Release(struct User_CartelInfo* uci, UserObj* obj);

extern LinuxFd UserObj_FDReserve(struct User_CartelInfo* uci);
extern void UserObj_FDUnreserve(struct User_CartelInfo* uci, LinuxFd fd);

extern VMK_ReturnStatus UserObj_FDReplaceObj(struct User_CartelInfo *uci,
					     UserObj *oldObj, UserObj *newObj);
extern void UserObj_FDAddObj(struct User_CartelInfo* uci, LinuxFd fd, UserObj* obj);
extern LinuxFd UserObj_FDAdd(struct User_CartelInfo* uci,
                             UserObj_Type type, UserObj_Data data,
                             UserObj_Methods* methods, uint32 openFlags);
extern VMK_ReturnStatus UserObj_FDDup(struct User_CartelInfo* uci,
                                      LinuxFd fromfd, LinuxFd minfd,
                                      LinuxFd* newfd);
extern VMK_ReturnStatus UserObj_FDDup2(struct User_CartelInfo* uci,
                                       LinuxFd fromfd, LinuxFd tofd);
extern VMK_ReturnStatus UserObj_TraversePath(struct User_CartelInfo* uci,
                                             const char* path,
                                             uint32 flags, LinuxMode mode,
                                             UserObj** objOut,
                                             char *arc, int arcLen);
extern VMK_ReturnStatus UserObj_Open(struct User_CartelInfo* uci,
                                     const char* path, uint32 flags,
                                     LinuxMode mode, UserObj** objOut);
extern VMK_ReturnStatus UserObj_Unlink(struct User_CartelInfo* uci,
                                       const char* path);
extern VMK_ReturnStatus UserObj_FDClose(struct User_CartelInfo* uci,
                                        LinuxFd fd);
extern VMK_ReturnStatus UserObj_FDSeek(struct User_CartelInfo* uci, LinuxFd fd,
                                       int64 offset, int whence, uint64 *res);
extern VMK_ReturnStatus UserObj_ReadMPN(UserObj* obj, MPN mpn,
                                        uint64 offset, uint32 *bytesRead);
extern VMK_ReturnStatus UserObj_WriteMPN(UserObj* obj, MPN mpn,
                                         uint64 offset, uint32 *bytesWritten);
extern VMK_ReturnStatus UserObj_Poll(LinuxPollfd *pfds, uint32 nfds,
                                     int32 timeoutMillis, uint32 *outNumReady);
extern VMK_ReturnStatus UserObj_Chdir(struct User_CartelInfo* uci,
                                      UserObj* obj);
extern VMK_ReturnStatus UserObj_GetDirName(struct User_CartelInfo* uci,
                                           struct UserObj* obj,
                                           char* buf, uint32 bufsize,
                                           char** bufOut);
extern VMK_ReturnStatus UserObj_ToString(struct UserObj *obj, char *string,
					 int length);
extern VMK_ReturnStatus UserObj_DumpObjTypes(struct UserDump_Header *dumpHeader,
					     struct UserDump_DumpData *dumpData);
extern VMK_ReturnStatus UserObj_DumpFdTable(struct UserDump_Header *dumpHeader,
					    struct UserDump_DumpData *dumpData);

/*
 *----------------------------------------------------------------------
 *
 * UserObj_FDLock --
 *
 *	Lock the given fdState.  Only used to protect the consistency
 *	of the descriptors array, not the fd objects themselves.
 *
 * Results:
 *	None
 *
 * Side-effects:
 *	Lock is taken.
 *
 *----------------------------------------------------------------------
 */
static inline void
UserObj_FDLock(UserObj_State* fdState)
{
   SP_Lock(&fdState->lock);
}


/*
 *----------------------------------------------------------------------
 *
 * UserObj_FDUnlock --
 *
 *	Unlock the given fdState.  Only used to protect the
 *	consistency of the descriptors array, not the fd objects
 *	themselves.
 *
 * Results:
 *	None
 *
 * Side-effects:
 *	Lock is released.
 *
 *----------------------------------------------------------------------
 */
static inline void
UserObj_FDUnlock(UserObj_State* fdState)
{
   SP_Unlock(&fdState->lock);
}


/*
 *----------------------------------------------------------------------
 *
 * UserObj_Acquire --
 *
 *      Increment the reference count on obj.
 *
 * Precondition:
 *      refcount must be nonzero.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	obj's reference count is incremented.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
UserObj_Acquire(UserObj *obj)
{
   ASSERT(obj != USEROBJ_RESERVED_HANDLE);
   ASSERT(Atomic_Read(&obj->refcount) > 0);
   Atomic_Inc(&obj->refcount);
}

/*
 *----------------------------------------------------------------------
 *
 * UserObj_IsOpenForRead --
 *
 *      Check if obj is open for reading.
 *
 * Results:
 *      TRUE if obj is open for reading; FALSE if not.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
UserObj_IsOpenForRead(const UserObj* obj)
{
   uint32 openFlags = obj->openFlags & USEROBJ_OPEN_FOR;

   return (openFlags == USEROBJ_OPEN_RDONLY ||
           openFlags == USEROBJ_OPEN_RDWR);
}


/*
 *----------------------------------------------------------------------
 *
 * UserObj_IsOpenForBlocking --
 *
 *      Check if obj is open for blocking accesses.
 *
 * Results:
 *      TRUE if ops on obj can block; FALSE if not.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
UserObj_IsOpenForBlocking(const UserObj* obj)
{
   return (obj->openFlags & USEROBJ_OPEN_NONBLOCK) == 0;
}


/*
 *----------------------------------------------------------------------
 *
 * UserObj_IsOpenForWrite --
 *
 *      Check if obj is open for writing.
 *
 * Results:
 *      TRUE if obj is open for writing; FALSE if not.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
UserObj_IsOpenForWrite(const UserObj* obj)
{
   uint32 openFlags = obj->openFlags & USEROBJ_OPEN_FOR;

   return (openFlags == USEROBJ_OPEN_WRONLY ||
           openFlags == USEROBJ_OPEN_RDWR);
}


/*
 *----------------------------------------------------------------------
 *
 * UserObj_PollActionToString --
 *
 *      Human readable version of a poll action.
 *
 * Results:
 *      A string constant.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE const char*
UserObj_PollActionToString(UserObjPollAction action) {
   switch(action) {
   case UserObjPollNoAction:
      return "NoAction";
   case UserObjPollNotify:
      return "Notify";
   case UserObjPollCleanup:
      return "Cleanup";
   default:
      return "<unknown>";
   }
}

/*
 * Some methods to use for unsupported operations.  Cast them to the
 * required type.
 */

extern VMK_ReturnStatus UserObj_Nop(void);
extern VMK_ReturnStatus UserObj_NotADirectory(void);
extern VMK_ReturnStatus UserObj_IsADirectory(void);
extern VMK_ReturnStatus UserObj_BadParam(void);
extern VMK_ReturnStatus UserObj_NoRmAccess(UserObj* obj, const char* arc);
extern VMK_ReturnStatus UserObj_NoMkAccess(UserObj* obj, const char* arc);
extern VMK_ReturnStatus UserObj_NotASocket(UserObj* obj);
extern VMK_ReturnStatus UserObj_NotImplemented(UserObj* obj);

#endif /* VMKERNEL_USER_USEROBJ_H */
