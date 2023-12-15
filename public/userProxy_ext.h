/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * userProxy_ext.h --
 *
 *	External definitions for the user proxy code.
 */


#ifndef _USERPROXY_EXT_H
#define _USERPROXY_EXT_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "vmk_basic_types.h"
#include "return_status.h"

#define USERPROXY_PATH_MAX	256
#define USERPROXY_NGROUPS_MAX   32
#define USERPROXY_MAX_IOVEC	10
#define USERPROXY_UTSNAME_LENGTH 65
#define USERPROXY_INVALID_PCHANDLE (-1)

#define USERPROXY_RPCTOKEN_INVALID  RPC_TOKEN_INVALID // -1
#define USERPROXY_RPCTOKEN_FRAGMENT (-2)
#define USERPROXY_RPCTOKEN_ERROR    (-3)

/*
 * MXInitFD() checks that
 *   mxFirstFD = lim.rlim_cur - (MX_MAX_LOCKS + VTHREAD_MAX_THREADS) * 2 - 30
 *   is > 50. With MX_MAX_LOCKS == 50 and VTHREAD_MAX_THREADS == 64,
 *   getrlimit(RLIMIT_NOFILE) needs to return at least 309.
 */
#define USERPROXY_MAX_OBJECTS            320

/*
 * Max number of milliseconds to sleep for before retrying to send on
 * a full RPC queue.
 */
#define USERPROXY_SLEEP_BEFORE_RETRY_MAX 50

/*
 * Marker that the proxy encountered a severe error.
 */
#define USERPROXY_SEVERE_ERROR		 (1 << 31)

/*
 * Proxy object types.  If adding new types, also update userObj.h:UserObj_Type.
 */
typedef enum {
   USERPROXY_TYPE_NONE = 0,
   USERPROXY_TYPE_FILE,
   USERPROXY_TYPE_FIFO,
   USERPROXY_TYPE_SOCKET,
   USERPROXY_TYPE_CHAR
} UserProxyObjType;

typedef enum {
   PROXY_FLAGS_NONE            = 0x0,
   /*
    * The syscall needs to be executed under the identity associated with the
    * world that is performing the syscall.
    */
   PROXY_FLAGS_IMPERSONATE     = 0x1,
   /*
    * The syscall takes an fd argument.  All message structs for these syscalls
    * MUST use UserProxyFdMsgHdr instead of UserProxyMsgHdr.
    */
   PROXY_FLAGS_FDARG           = 0x2,
   /*
    * The syscall has a variable-sized message.  This is only used for error
    * checking on the proxy side.  If this flag is not set for the syscall, then
    * we compare the size of the incoming message to the size given by
    * sizeof(UserProxy<SyscallName>Msg).  All message structs for these syscalls
    * MUST use UserProxyVarSizeFdMsgHdr instead of UserProxyMsgHdr.  Currently,
    * this flag is only valid if PROXY_FLAGS_FDARG is also provided.
    */
   PROXY_FLAGS_VARMSGSIZE      = 0x4,
   /*
    * The syscall returns a variable-sized reply based on the size of the
    * incoming message (ie, read takes a length, which determines how large the
    * reply message will be).  This flag is only valid if PROXY_FLAGS_FDARG is
    * also provided.
    */
   PROXY_FLAGS_VARREPLYSIZE    = 0x8,
   /*
    * The syscall affects the poll state of the fd it's operating on, thus we
    * need to update the poll cache.  All reply structs for these syscalls MUST
    * use UserProxyPollCacheReplyHdr instead of UserProxyReplyHdr.  This flag is
    * only valid if PROXY_FLAGS_FDARG is also proovided.
    */
   PROXY_FLAGS_UPDATEPOLLCACHE = 0x10,
   /*
    * Do not collect per-object statistics for the syscall even though it takes
    * an fd argument.  Currently this is only used by close because it clears
    * out the ProxyObject struct, thus leading to a potential deference of NULL
    * when the stats code is called after the syscall runs.
    */
   PROXY_FLAGS_NOOBJSTATS      = 0x20,
   /*
    * Do not generate a reply for this fd-less syscall.  By default all
    * fd-less syscalls generate an immediate reply.  Only used for Cancel,
    * ATM.  Not allowed with PROXY_FLAGS_FDARG or PROXY_FLAGS_VARREPLYSIZE
    */
   PROXY_FLAGS_NOREPLY         = 0x40,
} ProxySyscallFlags;

/*
 * Default callback function.  This is a placeholder that is used to tell
 * VMKProxyPerformCallback to simply re-call the syscall function in place of a
 * special callback function.
 */
#define PROXY_CB_DEFAULT NULL

/*
 * Proxy supported syscalls.
 *
 * The table below defines all the syscalls supported by the proxy.  Each
 * syscall is defined through a call to either DEFINE_PROXY_SYSCALL or
 * DEFINE_PROXY_CB_SYSCALL for syscalls that support polling on an fd and
 * receiving a callback.
 *
 * DEFINE_PROXY_SYSCALL(USERPROXY_FOO, Foo, <flags>) defines USERPROXY_FOO as a
 * part of the UserProxyFunctions enum.  The second field is used to generate
 * the names of the message and reply structs, as well as the syscall function
 * to call in the proxy.  So, the above example would translate to:
 *	Message: VMKProxyFooMsg
 *	Reply  : VMKProxyFooReply
 *	Syscall: VMKProxyHandleFoo
 * So, when adding a new syscall, you would create the VMKProxyFooMsg and
 * VMKProxyFooReply structs in this file, then add VMKProxyHandleFoo to
 * vmkproxySyscall.c.  The flag definitions are described above.
 *
 * DEFINE_PROXY_CB_SYSCALL(USERPROXY_FOO, Foo, <flags>, <cb func>, <poll flags>)
 * has the same first three arguments as DEFINE_PROXY_CB_SYSCALL.  The fourth
 * argument is the callback function to execute when the polled fd becomes
 * ready.  This can be either set to a specific function that comforms to the
 * ProxyCallback interface or PROXY_CB_DEFAULT can be given.  Finally, the poll
 * flags argument defines the flags that should be passed into poll when polling
 * on an fd for this syscall.
 */
#define PROXY_SYSCALLS \
   DEFINE_PROXY_SYSCALL(USERPROXY_OPEN, Open, PROXY_FLAGS_IMPERSONATE) \
   DEFINE_PROXY_SYSCALL(USERPROXY_CLOSE, Close, \
			PROXY_FLAGS_FDARG | PROXY_FLAGS_NOOBJSTATS) \
   DEFINE_PROXY_CB_SYSCALL(USERPROXY_READ, Read, \
			   PROXY_FLAGS_FDARG | PROXY_FLAGS_VARREPLYSIZE | \
			   PROXY_FLAGS_UPDATEPOLLCACHE, \
			   PROXY_CB_DEFAULT, POLLIN) \
   DEFINE_PROXY_CB_SYSCALL(USERPROXY_WRITE, Write, \
			   PROXY_FLAGS_FDARG | PROXY_FLAGS_VARMSGSIZE | \
			   PROXY_FLAGS_UPDATEPOLLCACHE, \
			   PROXY_CB_DEFAULT, POLLOUT) \
   DEFINE_PROXY_SYSCALL(USERPROXY_STAT, Stat, PROXY_FLAGS_FDARG) \
   DEFINE_PROXY_SYSCALL(USERPROXY_POLLCACHEENABLE, PollCacheEnable, \
			PROXY_FLAGS_FDARG | PROXY_FLAGS_UPDATEPOLLCACHE) \
   DEFINE_PROXY_SYSCALL(USERPROXY_UNLINK, Unlink, \
			PROXY_FLAGS_IMPERSONATE) \
   DEFINE_PROXY_SYSCALL(USERPROXY_RMDIR, Rmdir, PROXY_FLAGS_IMPERSONATE) \
   DEFINE_PROXY_SYSCALL(USERPROXY_MKDIR, Mkdir, PROXY_FLAGS_IMPERSONATE) \
   DEFINE_PROXY_SYSCALL(USERPROXY_READLINK, Readlink, PROXY_FLAGS_IMPERSONATE) \
   DEFINE_PROXY_SYSCALL(USERPROXY_SYMLINK, Symlink, PROXY_FLAGS_IMPERSONATE) \
   DEFINE_PROXY_SYSCALL(USERPROXY_MKFIFO, Mkfifo, PROXY_FLAGS_IMPERSONATE) \
   DEFINE_PROXY_SYSCALL(USERPROXY_STATFS, StatFS, PROXY_FLAGS_FDARG) \
   DEFINE_PROXY_SYSCALL(USERPROXY_LINK, Link, PROXY_FLAGS_IMPERSONATE) \
   DEFINE_PROXY_SYSCALL(USERPROXY_RENAME, Rename, PROXY_FLAGS_IMPERSONATE) \
   DEFINE_PROXY_SYSCALL(USERPROXY_FCNTL, Fcntl, PROXY_FLAGS_FDARG) \
   DEFINE_PROXY_SYSCALL(USERPROXY_FSYNC, Fsync, PROXY_FLAGS_FDARG) \
   DEFINE_PROXY_SYSCALL(USERPROXY_SYNC, Sync, PROXY_FLAGS_NONE) \
   DEFINE_PROXY_SYSCALL(USERPROXY_REGISTER_THREAD, RegisterThread, \
			PROXY_FLAGS_NONE) \
   DEFINE_PROXY_SYSCALL(USERPROXY_SETRESUID, Setresuid, \
			PROXY_FLAGS_IMPERSONATE) \
   DEFINE_PROXY_SYSCALL(USERPROXY_SETRESGID, Setresgid, \
			PROXY_FLAGS_IMPERSONATE) \
   DEFINE_PROXY_SYSCALL(USERPROXY_SETGROUPS, Setgroups, \
			PROXY_FLAGS_IMPERSONATE) \
   DEFINE_PROXY_SYSCALL(USERPROXY_CHMOD, Chmod, PROXY_FLAGS_FDARG) \
   DEFINE_PROXY_SYSCALL(USERPROXY_CHOWN, Chown, PROXY_FLAGS_FDARG) \
   DEFINE_PROXY_SYSCALL(USERPROXY_TRUNCATE, Truncate, PROXY_FLAGS_FDARG) \
   DEFINE_PROXY_SYSCALL(USERPROXY_UTIME, Utime, \
			PROXY_FLAGS_IMPERSONATE | PROXY_FLAGS_FDARG) \
   DEFINE_PROXY_SYSCALL(USERPROXY_CREATESOCKET, CreateSocket, \
			PROXY_FLAGS_IMPERSONATE) \
   DEFINE_PROXY_SYSCALL(USERPROXY_BIND, Bind, \
			PROXY_FLAGS_IMPERSONATE | PROXY_FLAGS_FDARG) \
   DEFINE_PROXY_CB_SYSCALL(USERPROXY_CONNECT, Connect, \
			   PROXY_FLAGS_FDARG | PROXY_FLAGS_UPDATEPOLLCACHE, \
			   VMKProxyConnectCB, POLLOUT) \
   DEFINE_PROXY_SYSCALL(USERPROXY_SOCKETPAIR, Socketpair, PROXY_FLAGS_NONE) \
   DEFINE_PROXY_SYSCALL(USERPROXY_LISTEN, Listen, \
			PROXY_FLAGS_FDARG | PROXY_FLAGS_UPDATEPOLLCACHE) \
   DEFINE_PROXY_CB_SYSCALL(USERPROXY_ACCEPT, Accept, \
			   PROXY_FLAGS_FDARG | PROXY_FLAGS_UPDATEPOLLCACHE, \
			   PROXY_CB_DEFAULT, POLLIN) \
   DEFINE_PROXY_SYSCALL(USERPROXY_GETNAME, Getname, PROXY_FLAGS_FDARG) \
   DEFINE_PROXY_CB_SYSCALL(USERPROXY_SENDMSG, Sendmsg, \
			   PROXY_FLAGS_FDARG | PROXY_FLAGS_VARMSGSIZE | \
			   PROXY_FLAGS_UPDATEPOLLCACHE, \
			   PROXY_CB_DEFAULT, POLLOUT) \
   DEFINE_PROXY_CB_SYSCALL(USERPROXY_RECVMSG, Recvmsg, \
			   PROXY_FLAGS_FDARG | PROXY_FLAGS_VARREPLYSIZE | \
			   PROXY_FLAGS_UPDATEPOLLCACHE, \
			   PROXY_CB_DEFAULT, POLLIN) \
   DEFINE_PROXY_SYSCALL(USERPROXY_SETSOCKOPT, Setsockopt, \
			PROXY_FLAGS_FDARG | PROXY_FLAGS_VARMSGSIZE) \
   DEFINE_PROXY_SYSCALL(USERPROXY_GETSOCKOPT, Getsockopt, \
			PROXY_FLAGS_FDARG | PROXY_FLAGS_VARREPLYSIZE) \
   DEFINE_PROXY_SYSCALL(USERPROXY_READDIR, ReadDir, \
			PROXY_FLAGS_FDARG | PROXY_FLAGS_VARREPLYSIZE) \
   DEFINE_PROXY_SYSCALL(USERPROXY_IOCTL, Ioctl, \
			PROXY_FLAGS_FDARG | PROXY_FLAGS_VARMSGSIZE | \
			PROXY_FLAGS_VARREPLYSIZE) \
   DEFINE_PROXY_SYSCALL(USERPROXY_UNAME, Uname, PROXY_FLAGS_NONE) \
   DEFINE_PROXY_SYSCALL(USERPROXY_GETPEERNAME, Getpeername, PROXY_FLAGS_FDARG) \
   DEFINE_PROXY_SYSCALL(USERPROXY_SHUTDOWN, Shutdown, PROXY_FLAGS_FDARG) \
   DEFINE_PROXY_SYSCALL(USERPROXY_ISPIDALIVE, IsPidAlive, PROXY_FLAGS_NONE) \
   DEFINE_PROXY_SYSCALL(USERPROXY_CANCEL, Cancel, PROXY_FLAGS_NOREPLY) \
   DEFINE_PROXY_END_SYSCALL(USERPROXY_END) // This should always be last.

/*
 * Define the syscall enums.
 */
#define DEFINE_PROXY_SYSCALL(_syscallNum, _name, _flags) \
	_syscallNum,
#define DEFINE_PROXY_CB_SYSCALL(_syscallNum, _name, _flags, _callback, \
				_pollFlags) \
	_syscallNum,
#define DEFINE_PROXY_END_SYSCALL(_syscallNum) _syscallNum
typedef enum {
   PROXY_SYSCALLS
} UserProxyFunctions;
#undef DEFINE_PROXY_SYSCALL
#undef DEFINE_PROXY_CB_SYSCALL
#undef DEFINE_PROXY_END_SYSCALL

#define USERPROXY_OPEN_RDONLY      0x00000000
#define USERPROXY_OPEN_WRONLY      0x00000001
#define USERPROXY_OPEN_RDWR        0x00000002
#define USERPROXY_OPEN_FOR         0x80000003 // mask for type of access
#define USERPROXY_OPEN_CREATE      0x00000040
#define USERPROXY_OPEN_EXCLUSIVE   0x00000080
#define USERPROXY_OPEN_NOCTTY      0x00000100
#define USERPROXY_OPEN_TRUNCATE    0x00000200
#define USERPROXY_OPEN_APPEND      0x00000400
#define USERPROXY_OPEN_NONBLOCK    0x00000800
#define USERPROXY_OPEN_SYNC        0x00001000
#define USERPROXY_OPEN_LARGEFILE   0x00008000
#define USERPROXY_OPEN_DIRECTORY   0x00010000 // fail if not a directory
#define USERPROXY_OPEN_NOFOLLOW    0x00020000
#define USERPROXY_OPEN_ASYNC       0x00002000 // not supported, but we need to
#define USERPROXY_OPEN_DIRECT      0x00004000 //    check for these in fcntl
#define USERPROXY_OPEN_VMFSFILE    0x1ff00000 // special flags for /vmfs files
#define USERPROXY_OPEN_SUPPORTED   0x9ff39fc3 // allowed in syscalls
#define USERPROXY_OPEN_PENULTIMATE 0x20000000 // internal use: skip last arc
#define USERPROXY_OPEN_IGNTRAILING 0x40000000 // internal use: ign trailing /
#define USERPROXY_OPEN_SEARCH      0x80000000 // internal use: chk srch access
#define USERPROXY_OPEN_OWNER       0x80000001 // internal use: chk ownership
#define USERPROXY_OPEN_GROUP       0x80000002 // internal use: chk in group
#define USERPROXY_OPEN_STAT        0x80000003 // internal use: no access chk

/*
 * UserProxyMsgHdr should be the first element of all message types, except
 * those with certain proxy flags, as described above.  Similarly,
 * UserProxyReplyHdr should be the first element of most all reply types.
 */
typedef struct {
   uint32	size;
} UserProxyMsgHdr;

/*
 * The message header for all syscalls with the PROXY_FLAGS_FDARG flag.
 */
typedef struct {
   UserProxyMsgHdr hdr;
   uint32          fileHandle;
} UserProxyFdMsgHdr;

/*
 * The message header for all syscalls with the PROXY_FLAGS_VARMSGSIZE flag.
 */
typedef struct {
   UserProxyFdMsgHdr fdHdr;
   uint32            dataSize;
} UserProxyVarSizeFdMsgHdr;

/*
 * Poll cache update.  Contains the new ready events and a unique id.
 */
typedef struct {
   short	events;
   uint32	generation;
} UserProxyPollCacheUpdate;

typedef struct {
   VMK_ReturnStatus     status;
   uint32		size;
} UserProxyReplyHdr;

/*
 * The reply header for all syscalls with the PROXY_FLAGS_POLLCACHEUPDATE flag.
 */
typedef struct {
   UserProxyReplyHdr   hdr;
   UserProxyPollCacheUpdate pcUpdate;
} UserProxyPollCacheReplyHdr;

typedef struct {
   UserProxyMsgHdr hdr;
   char         name[USERPROXY_PATH_MAX + 1];
   uint32       flags;
   uint32       mode;
} UserProxyOpenMsg;

typedef struct {
   UserProxyReplyHdr    hdr;
   uint32               fileHandle;
   uint32		pcHandle;
   UserProxyObjType	type;
} UserProxyOpenReply;

typedef UserProxyFdMsgHdr UserProxyCloseMsg;
typedef UserProxyReplyHdr UserProxyCloseReply;

typedef struct {
   UserProxyFdMsgHdr fdHdr;
   uint64            offset;
   uint32	     writeSize;
   uint8             data[0];
} UserProxyWriteMsg;

typedef struct {
   UserProxyPollCacheReplyHdr pcHdr;
   uint32                     nWritten;
} UserProxyWriteReply;

typedef struct {
   UserProxyVarSizeFdMsgHdr varHdr;
   uint64                   offset;
} UserProxyReadMsg;

typedef struct {
   UserProxyPollCacheReplyHdr pcHdr;
   uint32                     nRead;
   uint8                      data[0];
} UserProxyReadReply;

typedef UserProxyFdMsgHdr UserProxyStatMsg;

// Copied field-by-field, so we don't need the same order or
// padding as LinuxStat64 struct (or struct stat)
typedef struct UserProxyStatBuf {
   uint64 st_dev;
   uint64 st_blocks;
   int64  st_size;
   uint64 st_ino;
   uint64 st_rdev;

   uint32 st_mode;
   uint32 st_nlink;
   uint32 st_uid;
   uint32 st_gid;
   uint32 st_blksize;
   int32  st_atime;
   int32  st_mtime;
   int32  st_ctime;

   int dbgFieldCount;
} UserProxyStatBuf;

typedef struct {
   UserProxyReplyHdr    hdr;
   UserProxyStatBuf	statBuf;
} UserProxyStatReply;

typedef UserProxyFdMsgHdr UserProxyStatFSMsg;

// Copied field-by-field, so we don't need the same order or
// padding as LinuxStatFS struct (or struct statfs)
typedef struct UserProxyStatFSBuf {
   int32 f_type;
   int32 f_bsize;
   int64 f_blocks;
   int64 f_bfree;
   int64 f_bavail;
   int64 f_files;
   int64 f_ffree;
   struct { int32 val[2]; } f_fsid;
   int32 f_namelen;
   int32 f_spare[6];

   int dbgFieldCount;
} UserProxyStatFSBuf;

typedef struct {
   UserProxyReplyHdr    hdr;
   UserProxyStatFSBuf	statBuf;
} UserProxyStatFSReply;

typedef UserProxyFdMsgHdr UserProxyPollCacheEnableMsg;
typedef UserProxyPollCacheReplyHdr UserProxyPollCacheEnableReply;

typedef struct {
   UserProxyMsgHdr hdr;
   char         name[USERPROXY_PATH_MAX + 1];
} UserProxyUnlinkMsg;

typedef UserProxyReplyHdr UserProxyUnlinkReply;

typedef struct {
   UserProxyMsgHdr hdr;
   char         name[USERPROXY_PATH_MAX + 1];
} UserProxyRmdirMsg;

typedef UserProxyReplyHdr UserProxyRmdirReply;

typedef struct {
   UserProxyMsgHdr hdr;
   char         name[USERPROXY_PATH_MAX + 1];
   uint32       mode;
} UserProxyMkdirMsg;

typedef UserProxyReplyHdr UserProxyMkdirReply;

typedef struct {
   UserProxyMsgHdr hdr;
   char         name[USERPROXY_PATH_MAX + 1];
} UserProxyReadlinkMsg;

typedef struct {
   UserProxyReplyHdr    hdr;
   char         link[USERPROXY_PATH_MAX + 1];
} UserProxyReadlinkReply;

typedef struct {
   UserProxyMsgHdr hdr;
   char         name[USERPROXY_PATH_MAX + 1];
   char         link[USERPROXY_PATH_MAX + 1];
} UserProxySymlinkMsg;

typedef UserProxyReplyHdr UserProxySymlinkReply;

typedef struct {
   UserProxyMsgHdr hdr;
   char		name[USERPROXY_PATH_MAX + 1];
   uint32	mode;
} UserProxyMkfifoMsg;

typedef UserProxyReplyHdr UserProxyMkfifoReply;

typedef struct {
   UserProxyMsgHdr hdr;
   char         newName[USERPROXY_PATH_MAX + 1];
   char         oldName[USERPROXY_PATH_MAX + 1];
} UserProxyLinkMsg;

typedef UserProxyReplyHdr UserProxyLinkReply;

typedef struct {
   UserProxyMsgHdr hdr;
   char         newName[USERPROXY_PATH_MAX + 1];
   char         oldName[USERPROXY_PATH_MAX + 1];
} UserProxyRenameMsg;

typedef UserProxyReplyHdr UserProxyRenameReply;

typedef struct {
   UserProxyFdMsgHdr fdHdr;
   uint32	     cmd;
   uint32	     arg;
} UserProxyFcntlMsg;

typedef UserProxyReplyHdr UserProxyFcntlReply;

typedef struct {
   UserProxyFdMsgHdr fdHdr;
   Bool              dataOnly;
} UserProxyFsyncMsg;

typedef UserProxyReplyHdr UserProxyFsyncReply;

typedef UserProxyMsgHdr UserProxySyncMsg;
typedef UserProxyReplyHdr UserProxySyncReply;

typedef struct {
   UserProxyMsgHdr hdr;
   World_ID worldID;
   uint32       ruid, euid, suid;
   uint32       rgid, egid, sgid;
   uint32       ngids;
   uint32       gids[USERPROXY_NGROUPS_MAX];
} UserProxyRegisterThreadMsg;

typedef UserProxyReplyHdr UserProxyRegisterThreadReply;

typedef struct {
   UserProxyMsgHdr hdr;
   uint32       ruid, euid, suid;
} UserProxySetresuidMsg;

typedef UserProxyReplyHdr UserProxySetresuidReply;

typedef struct {
   UserProxyMsgHdr hdr;
   uint32       rgid, egid, sgid;
} UserProxySetresgidMsg;

typedef UserProxyReplyHdr UserProxySetresgidReply;

typedef struct {
   UserProxyMsgHdr hdr;
   uint32       ngids;
   uint32       gids[USERPROXY_NGROUPS_MAX];
} UserProxySetgroupsMsg;

typedef UserProxyReplyHdr UserProxySetgroupsReply;

typedef struct {
   UserProxyFdMsgHdr fdHdr;
   uint32            mode;
} UserProxyChmodMsg;

typedef UserProxyReplyHdr UserProxyChmodReply;

typedef struct {
   UserProxyFdMsgHdr fdHdr;
   uint32            owner;
   uint32            group;
} UserProxyChownMsg;

typedef UserProxyReplyHdr UserProxyChownReply;

typedef struct {
   UserProxyFdMsgHdr fdHdr;
   int64             size;
} UserProxyTruncateMsg;

typedef UserProxyReplyHdr UserProxyTruncateReply;

typedef struct {
   UserProxyFdMsgHdr fdHdr;
   uint32 atime;
   uint32 mtime;
} UserProxyUtimeMsg;

typedef UserProxyReplyHdr UserProxyUtimeReply;

typedef struct {
   UserProxyMsgHdr hdr;
   int		family;
   int		type;
   int		protocol;
} UserProxyCreateSocketMsg;

typedef struct {
   UserProxyReplyHdr hdr;
   uint32	fileHandle;
   uint32	pcHandle;
} UserProxyCreateSocketReply;

typedef struct {
   short	family;
   char		data[108];
} UserProxySocketName;

typedef struct {
   UserProxyFdMsgHdr   fdHdr;
   UserProxySocketName name;
   uint32	       nameLen;
} UserProxyBindMsg;

typedef UserProxyReplyHdr UserProxyBindReply;

typedef struct {
   UserProxyFdMsgHdr   fdHdr;
   UserProxySocketName name;
   uint32	       nameLen;
} UserProxyConnectMsg;

typedef UserProxyPollCacheReplyHdr UserProxyConnectReply;

typedef struct {
   UserProxyMsgHdr hdr;
   int		family;
   int		type;
   int		protocol;
} UserProxySocketpairMsg;

typedef struct {
   UserProxyReplyHdr hdr;
   uint32	fileHandle1;
   uint32	fileHandle2;
} UserProxySocketpairReply;

typedef struct {
   UserProxyFdMsgHdr fdHdr;
   uint32	     backlog;
} UserProxyListenMsg;

typedef UserProxyPollCacheReplyHdr UserProxyListenReply;

typedef struct {
   UserProxyFdMsgHdr fdHdr;
   uint32            nameLen;
} UserProxyAcceptMsg;

typedef struct {
   UserProxyPollCacheReplyHdr pcHdr;
   uint32                     newFileHandle;
   UserProxySocketName        name;
   uint32                     nameLen;
} UserProxyAcceptReply;

typedef struct {
   UserProxyFdMsgHdr fdHdr;
   uint32	     nameLen;
} UserProxyGetnameMsg;

typedef struct {
   UserProxyReplyHdr hdr;
   UserProxySocketName name;
   uint32	nameLen;
} UserProxyGetnameReply;

typedef UserProxyGetnameMsg UserProxyGetpeernameMsg;
typedef UserProxyGetnameReply UserProxyGetpeernameReply;

typedef struct {
   uint32	offset;
   uint32	length;
} UserProxyIovec;

typedef struct {
   UserProxyFdMsgHdr   fdHdr;
   // The fields on the LinuxMsgHdr struct.
   UserProxySocketName name;
   uint32	       nameLen;
   UserProxyIovec      iov[USERPROXY_MAX_IOVEC];
   uint32	       iovLen;
   uint32	       controlOffset;
   uint32	       controlLen;
   uint32	       flags;
   char		       data[0];
} UserProxySendmsgMsg;

typedef struct {
   UserProxyPollCacheReplyHdr pcHdr;
   uint32	              bytesSent;
} UserProxySendmsgReply;

typedef struct {
   UserProxyVarSizeFdMsgHdr varHdr;
   uint32	            nameLen;
   uint32	            iovLen;
   uint32	            iovDataLen[USERPROXY_MAX_IOVEC];
   uint32	            controlLen;
   uint32	            dataLen;
   uint32	            flags;
} UserProxyRecvmsgMsg;

typedef struct {
   UserProxyPollCacheReplyHdr pcHdr;
   uint32	              bytesRecv;
   // The fields on the LinuxMsgHdr struct.
   UserProxySocketName        name;
   uint32	              nameLen;
   UserProxyIovec             iov[USERPROXY_MAX_IOVEC];
   uint32	              iovLen;
   uint32	              controlOffset;
   uint32	              controlLen;
   uint32	              flags;
   char		              data[0];
} UserProxyRecvmsgReply;

typedef struct {
   UserProxyFdMsgHdr fdHdr;
   uint32	     level;
   uint32	     optName;
   uint32	     optLen;
   char		     optVal[0];
} UserProxySetsockoptMsg;

typedef UserProxyReplyHdr UserProxySetsockoptReply;

typedef struct {
   UserProxyVarSizeFdMsgHdr varHdr;
   uint32	            level;
   uint32	            optName;
} UserProxyGetsockoptMsg;

typedef struct {
   UserProxyReplyHdr hdr;
   uint32	optLen;
   char		optVal[0];
} UserProxyGetsockoptReply;

typedef UserProxyVarSizeFdMsgHdr UserProxyReadDirMsg;

typedef struct {
   UserProxyReplyHdr    hdr;
   uint32               nRead;
   uint8                data[0];
} UserProxyReadDirReply;

typedef struct {
   UserProxyVarSizeFdMsgHdr varHdr;
   uint32                   cmd;
   uint32                   packed;
   uint8                    data[0];
} UserProxyIoctlMsg;

typedef struct {
   UserProxyReplyHdr     hdr;
   uint32                size;
   uint32                result;
   uint8                 data[0];
} UserProxyIoctlReply;

typedef UserProxyMsgHdr UserProxyUnameMsg;

typedef struct UserProxyUtsName {
   char sysname[USERPROXY_UTSNAME_LENGTH];
   char nodename[USERPROXY_UTSNAME_LENGTH];
   char release[USERPROXY_UTSNAME_LENGTH];
   char version[USERPROXY_UTSNAME_LENGTH];
   char machine[USERPROXY_UTSNAME_LENGTH];
   char domainname[USERPROXY_UTSNAME_LENGTH];
} UserProxyUtsName;

typedef struct {
   UserProxyReplyHdr hdr;
   UserProxyUtsName buf;
} UserProxyUnameReply;

typedef struct {
   UserProxyFdMsgHdr fdHdr;
   int               how;
} UserProxyShutdownMsg;

typedef UserProxyReplyHdr UserProxyShutdownReply;

typedef struct {
   UserProxyMsgHdr hdr;
   int		   pid;
} UserProxyIsPidAliveMsg;

typedef UserProxyReplyHdr UserProxyIsPidAliveReply;

typedef struct {
   UserProxyMsgHdr hdr;         /* must be first */
   RPC_Token       token;
} UserProxyCancelMsg;

/*
 * No reply to a cancel msg is generated. A reply to the canceled msg is
 * generated.
 */
typedef UserProxyReplyHdr UserProxyCancelReply;

#endif
