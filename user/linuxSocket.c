/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 */

/*
 * linuxSocket.c -
 *	Linux socket related syscall entrypoints and glue.
 */

#include "sys/types.h"
#include "netinet/in.h"
#include "netinet/tcp.h"
#include "sys/socket.h"

#define _SYS_INTTYPES_H_
#define __FreeBSD__

#include "user_int.h"
#include "linuxFileDesc.h"
#include "userSocket.h"
#include "vmk_net.h"
#include "libc.h"

#define LOGLEVEL_MODULE LinuxSocket
#include "userLog.h"


/*
 * Definitions for the 'whichCall' parameter to the overloaded
 * socketcall() call
 */
#define LINUX_SOCKETCALL_SOCKET      1
#define LINUX_SOCKETCALL_BIND        2
#define LINUX_SOCKETCALL_CONNECT     3
#define LINUX_SOCKETCALL_LISTEN      4
#define LINUX_SOCKETCALL_ACCEPT      5
#define LINUX_SOCKETCALL_GETSOCKNAME 6
#define LINUX_SOCKETCALL_GETPEERNAME 7
#define LINUX_SOCKETCALL_SOCKETPAIR  8
#define LINUX_SOCKETCALL_SEND        9
#define LINUX_SOCKETCALL_RECV        10
#define LINUX_SOCKETCALL_SENDTO      11
#define LINUX_SOCKETCALL_RECVFROM    12
#define LINUX_SOCKETCALL_SHUTDOWN    13
#define LINUX_SOCKETCALL_SETSOCKOPT  14
#define LINUX_SOCKETCALL_GETSOCKOPT  15
#define LINUX_SOCKETCALL_SENDMSG     16
#define LINUX_SOCKETCALL_RECVMSG     17

#define LINUXSOCKET_SOCKOPT_MAXLEN     64 /* probably '8' will do */
#define LINUXSOCKET_SOCKETNAME_MAXLEN 128 /* 110 is unixdomain socket namelen */
#define LINUXSOCKET_CTLMSG_MAXLEN (4*1024)

#define LINUXSOCKET_SHUT_RD          0
#define LINUXSOCKET_SHUT_WR          1
#define LINUXSOCKET_SHUT_RDWR        2

typedef struct {
   LinuxSocketFamily   family;
   LinuxSocketType     type;
   LinuxSocketProtocol protocol;
} LinuxNewSocketArgs;

typedef struct {
   LinuxFd socketfd;
   UserVA /*LinuxSocketName**/ name;
   uint32 namelen;
} LinuxBindArgs;

typedef struct {
   LinuxFd socketfd;
   UserVA /*LinuxSocketName**/ name;
   uint32 namelen;
} LinuxConnectArgs;

typedef struct {
   LinuxFd socketfd;
   int backlog;
} LinuxListenArgs;

typedef struct {
   LinuxFd socketfd;
   UserVA /*LinuxSocketName**/ name;
   UserVA /*uint32**/ namelen;
} LinuxAcceptArgs;

typedef struct {
   LinuxFd socketfd;
   UserVA /*LinuxSocketName**/ name;
   UserVA /*uint32**/ namelen;
} LinuxSocketGetNameArgs;

typedef struct {
   LinuxSocketFamily   family;
   LinuxSocketType     type;
   LinuxSocketProtocol protocol;
   UserVA /*LinuxFd**/ socketfds;
} LinuxSocketSocketpairArgs;

typedef struct {
   LinuxFd      socketfd;
   UserVA /*void**/ buf;
   int          len;
   unsigned int flags;
} LinuxSocketSendArgs;

typedef struct {
   LinuxFd      socketfd;
   UserVA /*void**/ buf;
   int          len;
   unsigned int flags;
} LinuxSocketRecvArgs;

typedef struct {
   LinuxFd socketfd;
   int     level;
   int     optName;
   char*   optVal;
   int     optLen;
} LinuxSocketSetsockoptArgs;

typedef struct {
   LinuxFd socketfd;
   int     level;
   int     optName;
   char*   optVal;
   int*    optLen;
} LinuxSocketGetsockoptArgs;

typedef struct {
   LinuxFd socketfd;
   UserVA /*LinuxMsgHdr**/ msg;
   unsigned int flags;
} LinuxSocketSendmsgArgs;

typedef struct {
   LinuxFd socketfd;
   UserVA /*LinuxMsgHdr**/ msg;
   unsigned int flags;
} LinuxSocketRecvmsgArgs;

typedef struct {
   LinuxFd socketfd;
   UserVA /*void**/ buf;
   int          len;
   unsigned int flags;
   UserVA /*LinuxSocketName**/ name;
   UserVA /*uint32**/ namelen;
} LinuxSocketRecvfromArgs;

typedef struct {
   LinuxFd      socketfd;
   UserVA /*void**/ buf;
   int          len;
   unsigned int flags;
   UserVA /*LinuxSocketName**/ name;
   UserVA /*uint32**/ namelen;
} LinuxSocketSendToArgs;

typedef struct {
   LinuxFd      socketfd;
   int          how;
} LinuxSocketShutdownArgs;

/*
 *----------------------------------------------------------------------
 *
 * LinuxSocketCopyOutName --
 *
 *	Copy the given socket name out into the given user address.
 * 
 * Results:
 *	VMK_OK if success, otherwise on error
 *
 * Side effects:
 *	Data is copied out to userspace.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
LinuxSocketCopyOutName(UserVA /*LinuxSocketName**/ nameaddr,
                       UserVA /*uint32**/ namelenaddr,
		       const LinuxSocketName* const name,
		       uint32 namelen)
{
   VMK_ReturnStatus status;

   ASSERT(namelen < sizeof *name);
   
   status = User_CopyOut(nameaddr, name, namelen);
   if (status == VMK_OK) {
      status = User_CopyOut(namelenaddr, &namelen, sizeof namelen);
   }

   return status;
}


// Convenience macro for copying a user-mode address into a kernel-mode
// struct, and returning a linux EFAULT if copy fails.
#define COPYIN_OR_RETURN(kargs, userargs)                                \
   do {                                                                  \
      if (VMK_OK != User_CopyIn(&(kargs), (userargs), sizeof (kargs))) {  \
         return LINUX_EFAULT;                                            \
      }                                                                  \
   } while (0)


/*
 *----------------------------------------------------------------------
 *
 * LinuxSocketNewSocket --
 *
 *	Create a new socket with given family, type, and protocol.
 * 
 * Results:
 *	An fd for the new socket on success, Linux error code on failure.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
LinuxSocketNewSocket(UserVA userArgs)
{
   LinuxFd socketfd;
   VMK_ReturnStatus status;
   LinuxNewSocketArgs kargs;

   COPYIN_OR_RETURN(kargs, userArgs);

   UWLOG_SyscallEnter("(family=%d, type=%d, protocol=%d)", kargs.family, kargs.type,
                      kargs.protocol);

   status = UserSocket_NewSocket(kargs.family, kargs.type, kargs.protocol,
				 &socketfd);
   if (status != VMK_OK) {
      return User_TranslateStatus(status);
   }

   return socketfd;
}


/*
 *----------------------------------------------------------------------
 * 
 * LinuxSocketBind --
 *
 *	Bind the given socket to the given name.
 * 
 * Results:
 *	0 on success, a Linux error code otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
LinuxSocketBind(UserVA userArgs)
{
   LinuxBindArgs kargs;
   VMK_ReturnStatus status;
   LinuxSocketName kname;

   UWLOG_SyscallEnter("(userArgs=%#x)", userArgs);

   COPYIN_OR_RETURN(kargs, userArgs);
   if (kargs.namelen > sizeof kname) {
      return LINUX_ENAMETOOLONG;
   }
   if (kargs.namelen > 0) {
      status = User_CopyIn(&kname, kargs.name, kargs.namelen);
      if (status != VMK_OK) {
	 return status;
      }
   } else {
      memset(&kname, 0, sizeof kname);
   }
   
   UWLOG(1, "(fd=%u, name=%p, namelen=%d)",
         kargs.socketfd, &kname, kargs.namelen);

   status = UserSocket_Bind(kargs.socketfd, &kname, kargs.namelen);
   if (status != VMK_OK) {
      UWLOG(0, "Bind failed for socket %d (%#x:%s)",
            kargs.socketfd, status, UWLOG_ReturnStatusToString(status));
   }

   return User_TranslateStatus(status);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxSocketConnect
 *
 *	Connect the given fd to the given remote name.
 * 
 * Results:
 *	0 on success, a Linux error code otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
LinuxSocketConnect(UserVA userArgs)
{
   LinuxConnectArgs kargs;
   VMK_ReturnStatus status;
   LinuxSocketName kname;

   UWLOG_SyscallEnter("(userArgs=%#x)", userArgs);

   COPYIN_OR_RETURN(kargs, userArgs);
   if (kargs.namelen > sizeof kname) {
      return LINUX_ENAMETOOLONG;
   }
   if (kargs.namelen > 0) {
      status = User_CopyIn(&kname, kargs.name, kargs.namelen);
      if (status != VMK_OK) {
	 return status;
      }
   } else {
      memset(&kname, 0, sizeof kname);
   }

   UWLOG(1, "(fd=%u, name=%p, namelen=%d)",
         kargs.socketfd, &kname, kargs.namelen);

   status = UserSocket_Connect(kargs.socketfd, &kname, kargs.namelen);
   if (status != VMK_OK) {
      UWLOG(0, "connect failed for socket %d (%#x:%s)",
            kargs.socketfd, status, UWLOG_ReturnStatusToString(status));
   }

   return User_TranslateStatus(status);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxSocketListen --
 *
 *	Listen for incoming connections on the given socket.
 * 
 * Results:
 *	0 on success, a Linux error code otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
LinuxSocketListen(UserVA userArgs)
{
   LinuxListenArgs kargs;
   VMK_ReturnStatus status;

   UWLOG_SyscallEnter("(userArgs=%#x)", userArgs);

   COPYIN_OR_RETURN(kargs, userArgs);

   UWLOG(1, "(fd=%u, backlog=%u)",
         kargs.socketfd, kargs.backlog);

   status = UserSocket_Listen(kargs.socketfd, kargs.backlog);
   if (status != VMK_OK) {
      UWLOG(0, "listen failed for socket %d (%#x:%s)",
            kargs.socketfd, status, UWLOG_ReturnStatusToString(status));
   }

   return User_TranslateStatus(status);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxSocketAccept --
 *
 *	Accept a new connection on the given socket.
 * 
 * Results:
 *	0 on success, a Linux error code otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
LinuxSocketAccept(UserVA userArgs)
{
   LinuxAcceptArgs kargs;
   uint32 knamelen;
   LinuxFd acceptfd = -1;
   VMK_ReturnStatus status;
   LinuxSocketName kname, *knamePtr;

   UWLOG_SyscallEnter("(userArgs=%#x)", userArgs);

   COPYIN_OR_RETURN(kargs, userArgs);

   if (kargs.namelen) {
      status = User_CopyIn(&knamelen,  kargs.namelen, sizeof(kargs.namelen));
      if (status != VMK_OK) {
         return status;
      }
   } else {
      knamelen = 0;
   }

   UWLOG(1, "(fd=%u, name@%#x, knamelen=%d)",
         kargs.socketfd, kargs.name, knamelen);

   if (knamelen > sizeof kname) {
      /*
       * This goes against the accept(2) man page, but oh well (apparently
       * they're ok with a user passing in any old length).
       */
      return LINUX_ENAMETOOLONG;
   }

   if (kargs.name == 0) {
      knamePtr = NULL;
   } else {
      knamePtr = &kname;
   }

   status = UserSocket_Accept(kargs.socketfd, &acceptfd, knamePtr, &knamelen);
   if (status == VMK_OK && knamePtr != NULL) {
      ASSERT(knamelen > 0);
      status = LinuxSocketCopyOutName(kargs.name, kargs.namelen, &kname,
				      knamelen);
   } else if (status != VMK_OK) {
      UWLOG(0, "accept failed for socket %d (%#x:%s)",
            kargs.socketfd, status, UWLOG_ReturnStatusToString(status));
   }

   if (status != VMK_OK) {
      return User_TranslateStatus(status);
   } else {
      ASSERT(acceptfd >= 0);
      return acceptfd;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxSocketGetpeername --
 *
 *	Get the name of the connected peer.
 * 
 * Results:
 *	0 on success, a Linux error code otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
LinuxSocketGetpeername(UserVA userArgs)
{
   LinuxSocketGetNameArgs kargs;
   uint32 knamelen;
   LinuxSocketName kname;
   VMK_ReturnStatus status;

   UWLOG_SyscallEnter("(userArgs=%#x)", userArgs);

   COPYIN_OR_RETURN(kargs, userArgs);
   COPYIN_OR_RETURN(knamelen, kargs.namelen);

   UWLOG(1, "(fd=%u, name@%#x, namelen=%d)",
         kargs.socketfd, kargs.name, knamelen);

   if (knamelen > sizeof kname) {
      return LINUX_ENAMETOOLONG;
   }

   status = UserSocket_GetPeerName(kargs.socketfd, &kname, &knamelen);

   if (status == VMK_OK) {
      if (knamelen > 0) {
	 status = LinuxSocketCopyOutName(kargs.name, kargs.namelen, &kname,
					 knamelen);
      }
   } else {
      UWLOG(0, "getpeername failed for socket %d: %s",
            kargs.socketfd, UWLOG_ReturnStatusToString(status));
   }

   return User_TranslateStatus(status);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxSocketGetName --
 *
 *	Get the local name for the given socket.
 * 
 * Results:
 *	0 on success, a Linux error code otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
LinuxSocketGetName(UserVA userArgs)
{
   LinuxSocketGetNameArgs kargs;
   uint32 knamelen;
   LinuxSocketName kname;
   VMK_ReturnStatus status;

   UWLOG_SyscallEnter("(userArgs=%#x)", userArgs);

   COPYIN_OR_RETURN(kargs, userArgs);
   COPYIN_OR_RETURN(knamelen, kargs.namelen);

   UWLOG(1, "(fd=%u, name@%#x, namelen=%d)",
         kargs.socketfd, kargs.name, knamelen);

   if (knamelen > sizeof kname) {
      /*
       * This goes against the getsockname(2) man page, but oh well (apparently
       * they're ok with a user passing in any old length).
       */
      return LINUX_ENAMETOOLONG;
   }

   status = UserSocket_GetName(kargs.socketfd, &kname, &knamelen);
   if (status == VMK_OK) {
      if (knamelen > 0) {
	 status = LinuxSocketCopyOutName(kargs.name, kargs.namelen, &kname,
					 knamelen);
      }
   } else {
      UWLOG(0, "getsocketname failed for socket %d: %s",
            kargs.socketfd, UWLOG_ReturnStatusToString(status));
   }

   return User_TranslateStatus(status);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxSocketSocketpair --
 *
 *	Connects the two given sockets.
 * 
 * Results:
 *	0 on success, a Linux error code otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
LinuxSocketSocketpair(UserVA userArgs)
{
   VMK_ReturnStatus status;
   LinuxSocketSocketpairArgs kargs;
   LinuxFd kfds[2];

   UWLOG_SyscallEnter("(userArgs=%#x)", userArgs);

   COPYIN_OR_RETURN(kargs, userArgs);
   if (User_CopyIn(kfds, kargs.socketfds, sizeof kfds) != VMK_OK) {
      return LINUX_EFAULT;
   }

   UWLOG(1, "(family=%d, type=%d, protocol=%d)", kargs.family, kargs.type,
         kargs.protocol);

   status = UserSocket_Socketpair(kargs.family, kargs.type, kargs.protocol,
				  kfds);
   if (status == VMK_OK) {
      status = User_CopyOut(kargs.socketfds, kfds, sizeof kfds);
   }

   return User_TranslateStatus(status);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxSocketSend --
 *
 *	Send data on the given socket.
 * 
 * Results:
 *	Number of bytes sent on success, a Linux error code otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
LinuxSocketSend(UserVA userArgs)
{
   VMK_ReturnStatus status;
   LinuxSocketSendArgs kargs;
   int bytesSent = 0;

   UWLOG_SyscallEnter("(userArgs=%#x)", userArgs);

   COPYIN_OR_RETURN(kargs, userArgs);

   UWLOG(1, "(fd=%u, buf=%#x, len=%d, flags=%#x)", kargs.socketfd, kargs.buf,
	 kargs.len, kargs.flags);

   status = UserSocket_Sendto(kargs.socketfd, kargs.buf, kargs.len, kargs.flags,
			      NULL, 0, &bytesSent);
   if (status == VMK_OK) {
      return bytesSent;
   } else {
      return User_TranslateStatus(status);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxSocketRecv --
 *
 *	Receive data on the given socket.
 * 
 * Results:
 *	Number of bytes received on success, a Linux error code otherwise.
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static int
LinuxSocketRecv(UserVA userArgs)
{
   VMK_ReturnStatus status;
   LinuxSocketRecvArgs kargs;
   int bytesRecv = 0;

   UWLOG_SyscallEnter("(userArgs=%#x)", userArgs);

   COPYIN_OR_RETURN(kargs, userArgs);

   UWLOG(1, "(fd=%u, buf=%#x, len=%d, flags=%#x)", kargs.socketfd, kargs.buf,
	 kargs.len, kargs.flags);

   status = UserSocket_RecvFrom(kargs.socketfd, kargs.buf, kargs.len,
				kargs.flags, NULL, 0, &bytesRecv);
   if (status == VMK_OK) {
      return bytesRecv;
   } else {
      return User_TranslateStatus(status);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxSocketSetsockopt --
 *
 *	Set the given socket option to the given value.
 * 
 * Results:
 *	0 on success, a Linux error code otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
LinuxSocketSetsockopt(UserVA userArgs)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   VMK_ReturnStatus status;
   LinuxSocketSetsockoptArgs kargs;
   char* koptVal;

   UWLOG_SyscallEnter("(userArgs=%#x)", userArgs);

   COPYIN_OR_RETURN(kargs, userArgs);

   if ((kargs.optLen <= 0)
       || (kargs.optLen > LINUXSOCKET_SOCKOPT_MAXLEN)) {
      /*
       * Disallow bogus option lengths (since we do an alloc based on
       * the value).
       */
      UWWarn("Invalid option length (%d).", kargs.optLen);
      UWLOG_StackTraceCurrent(1);
      return LINUX_EINVAL;
   }

   koptVal = (char*)User_HeapAlloc(uci, kargs.optLen);
   if (koptVal == NULL) {
      return LINUX_ENOMEM;
   }
   if (User_CopyIn(koptVal, (UserVA)kargs.optVal, kargs.optLen) != VMK_OK) {
      User_HeapFree(uci, koptVal);
      return LINUX_EFAULT;
   }
   
   UWLOG(1, "(fd=%u, level=%d, optName=%d, optVal=%p, optLen=%d)",
	 kargs.socketfd, kargs.level, kargs.optName, koptVal, kargs.optLen);

   status = UserSocket_Setsockopt(kargs.socketfd, kargs.level, kargs.optName,
				  koptVal, kargs.optLen);
   User_HeapFree(uci, koptVal);
   return User_TranslateStatus(status);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxSocketGetsockopt --
 *
 *	Get the value of the given socket option.
 * 
 * Results:
 *	0 on success, a Linux error code otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
LinuxSocketGetsockopt(UserVA userArgs)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   LinuxSocketGetsockoptArgs kargs;
   VMK_ReturnStatus status;
   char* koptVal;
   int koptLen;
   int origkoptLen;

   UWLOG_SyscallEnter("(userArgs=%#x)", userArgs);

   COPYIN_OR_RETURN(kargs, userArgs);
   COPYIN_OR_RETURN(koptLen, (UserVA)kargs.optLen); // kargs.optLen is an IN/OUT parameter

   if ((koptLen <= 0)
       || (koptLen > LINUXSOCKET_SOCKOPT_MAXLEN)) {
      /*
       * Disallow bogus option lengths (since we do an alloc based on
       * the value).
       */
      UWWarn("Invalid option length (%d).", koptLen);
      UWLOG_StackTraceCurrent(1);
      return LINUX_EINVAL;
   }

   koptVal = (char*)User_HeapAlloc(uci, koptLen);
   if (koptVal == NULL) {
      return LINUX_ENOMEM;
   }
   
   UWLOG(1, "(fd=%u, level=%d, optName=%d, optVal=%p, optLen=%d)",
	 kargs.socketfd, kargs.level, kargs.optName, koptVal, koptLen);

   origkoptLen = koptLen;
   status = UserSocket_Getsockopt(kargs.socketfd, kargs.level, kargs.optName,
				  koptVal, &koptLen);
   if (status == VMK_OK) {
      /*
       * Make sure we're not trying to copy more data than is in our buffer.
       */
      ASSERT(koptLen <= origkoptLen);
      status = User_CopyOut((UserVA)kargs.optVal, koptVal, koptLen);
      if (status == VMK_OK) {
         status = User_CopyOut((UserVA)kargs.optLen, &koptLen, sizeof koptLen);
      }
   }
         
   User_HeapFree(uci, koptVal);

   return User_TranslateStatus(status);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxSocketCopyInMsgHdr --
 *
 *	Copies in the fields of a linux message header struct and
 *	overwrites the user pointers with kernel pointers.
 * 
 * Results:
 *	VMK_OK if okay, otherwise on error.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
LinuxSocketCopyInMsgHdr(LinuxMsgHdr* msg, int* totalLen)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   VMK_ReturnStatus status;
   LinuxSocketName* name = NULL;
   LinuxIovec* iov = NULL;
   int iovLen;
   void* control = NULL;
   int i;

   /*
    * name is optional.  So copy in only if they gave us a name.
    */
   if ((msg->nameLen > 0)
       && (msg->nameLen < LINUXSOCKET_SOCKETNAME_MAXLEN)) {
      name = (LinuxSocketName*)User_HeapAlloc(uci, msg->nameLen);
      if (name == NULL) {
         status = VMK_NO_MEMORY;
	 goto out_error;
      }

      status = User_CopyIn(name, (UserVA)msg->name, msg->nameLen);
      if (status != VMK_OK) {
         goto out_error;
      }
   } else if (msg->nameLen != 0) {
      UWLOG(0, "Invalid name length (%d)", msg->nameLen);
      status = VMK_BAD_PARAM;
      goto out_error;
   }

   /*
    * iov is NOT optional.  We need at least one iovec.
    *
    * Note the copied in iovec structs still contain pointers to userspace
    * buffers.  We want to keep it this way so we can minimize copying.
    */
   if ((int)msg->iovLen < 1 || (int)msg->iovLen > LINUX_MAX_IOVEC) {
      UWLOG(0, "Invalid io vector length (%d)", msg->iovLen);
      status = VMK_BAD_PARAM;
      goto out_error;
   }

   iovLen = msg->iovLen * sizeof(LinuxIovec);

   iov = (LinuxIovec*)User_HeapAlloc(uci, iovLen);
   if (iov == NULL) {
      status = VMK_NO_MEMORY;
      goto out_error;
   }

   status = User_CopyIn(iov, (UserVA)msg->iov, iovLen);
   if (status != VMK_OK) {
      goto out_error;
   }

   /*
    * Compute the total amount of data we're sending.
    */
   *totalLen = 0;
   for (i = 0; i < msg->iovLen; i++) {
      *totalLen += iov[i].length;
   }

   if ((msg->controlLen > 0)
       && (msg->controlLen < LINUXSOCKET_CTLMSG_MAXLEN)) {
      control = User_HeapAlloc(uci, msg->controlLen);
      if (control == NULL) {
         status = VMK_NO_MEMORY;
	 goto out_error;
      }

      status = User_CopyIn(control, (UserVA)msg->control, msg->controlLen);
      if (status != VMK_OK) {
         goto out_error;
      }
   } else if (msg->controlLen != 0) {
      UWLOG(0, "Invalid control message length (%d)", msg->controlLen);
      status = VMK_BAD_PARAM;
      goto out_error;
   }

   /*
    * Now overwrite the user space addresses with kernel pointers.
    */
   msg->name = name;
   msg->iov = iov;
   msg->control = control;

   return VMK_OK;

out_error:
   if (name != NULL) {
      User_HeapFree(uci, name);
   }
   if (iov != NULL) {
      User_HeapFree(uci, iov);
   }
   if (control != NULL) {
      User_HeapFree(uci, control);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxSocketFreeCopiedMsgHdr --
 *
 *	Free the kernel copies of message header data.
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
LinuxSocketFreeCopiedMsgHdr(LinuxMsgHdr* msg)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;

   if (msg->name != NULL) {
      User_HeapFree(uci, msg->name);
   }
   if (msg->iov != NULL) {
      User_HeapFree(uci, msg->iov);
   }
   if (msg->control != NULL) {
      User_HeapFree(uci, msg->control);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxSocketSendmsg --
 *
 *	Send data on the given socket.
 * 
 * Results:
 *	Number of bytes sent on success, a Linux error code otherwise.
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static int
LinuxSocketSendmsg(UserVA userArgs)
{
   VMK_ReturnStatus status;
   LinuxSocketSendmsgArgs kargs;
   LinuxMsgHdr kmsg;
   uint32 totalLen;
   uint32 bytesSent = 0;

   UWLOG_SyscallEnter("(userArgs=%#x)", userArgs);

   COPYIN_OR_RETURN(kargs, userArgs);
   COPYIN_OR_RETURN(kmsg, kargs.msg);
   status = LinuxSocketCopyInMsgHdr(&kmsg, &totalLen);
   if (status != VMK_OK) {
      return User_TranslateStatus(status);
   }
   kmsg.flags = kargs.flags;

   UWLOG(1, "(fd=%u, msg=%#x, flags=%#x)", kargs.socketfd, kargs.msg,
	 kargs.flags);

   status = UserSocket_Sendmsg(kargs.socketfd, &kmsg, totalLen, &bytesSent);

   LinuxSocketFreeCopiedMsgHdr(&kmsg);

   if (status == VMK_OK) {
      return bytesSent;
   } else {
      return User_TranslateStatus(status);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxSocketCopyOutMsgHdr --
 *
 *	Copy the data in the given LinuxMsgHdr out to user space.  'msg'
 *	is a completely in-kernel data structure (ie, with pointers to
 *	kernel memory).  'umsg' is itself in kernel space, but it has
 *	pointers into user space.  Finally, 'umsgPtr' is a pointer to the
 *	message in user space.
 * 
 * Results:
 *	VMK_OK if okay, otherwise on error.
 *
 * Side effects:
 *	Data is copied into user space.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
LinuxSocketCopyOutMsgHdr(LinuxMsgHdr* msg, LinuxMsgHdr* umsg, UserVA umsgPtr)
{
   VMK_ReturnStatus status = VMK_OK;

   /*
    * name is optional.
    */
   if (umsg->name != NULL && (int)umsg->nameLen > 0) {
      ASSERT(msg->nameLen <= umsg->nameLen);
      if (msg->name != NULL && (int)msg->nameLen > 0) {
	 umsg->nameLen = msg->nameLen;
	 status = User_CopyOut((UserVA)umsg->name, msg->name, umsg->nameLen);
      }
   }

   /*
    * iov is mandatory.
    */
   if (status == VMK_OK) {
      ASSERT(msg->iovLen > 0 && msg->iovLen <= umsg->iovLen);
      umsg->iovLen = msg->iovLen;
      status = User_CopyOut((UserVA)umsg->iov, msg->iov, umsg->iovLen);
   }

   /*
    * control is optional.
    */
   if (status == VMK_OK && umsg->control != NULL && (int)umsg->controlLen > 0) {
      ASSERT(msg->controlLen <= umsg->controlLen);
      if (msg->control != NULL && (int)msg->controlLen > 0) {
	 umsg->controlLen = msg->controlLen;
	 status = User_CopyOut((UserVA)umsg->control, msg->control,
			       umsg->controlLen);
      }
   }

   /*
    * Now copy umsg out to userspace.
    */
   if (status == VMK_OK) {
      status = User_CopyOut(umsgPtr, umsg, sizeof *umsg);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxSocketRecvmsg --
 *
 *	Recieve data on the given socket.
 * 
 * Results:
 *	Number of bytes received on success, a Linux error code otherwise.
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static int
LinuxSocketRecvmsg(UserVA userArgs)
{
   VMK_ReturnStatus status;
   LinuxSocketRecvmsgArgs kargs;
   LinuxMsgHdr kmsg;
   LinuxMsgHdr umsg;
   uint32 totalLen;
   uint32 bytesRecv = 0;

   UWLOG_SyscallEnter("(userArgs=%#x)", userArgs);

   COPYIN_OR_RETURN(kargs, userArgs);
   COPYIN_OR_RETURN(umsg, kargs.msg);
   COPYIN_OR_RETURN(kmsg, kargs.msg);
   status = LinuxSocketCopyInMsgHdr(&kmsg, &totalLen);
   if (status != VMK_OK) {
      return User_TranslateStatus(status);
   }
   kmsg.flags = kargs.flags;

   UWLOG(1, "(fd=%u, msg=%#x, flags=%#x)", kargs.socketfd, kargs.msg,
	 kargs.flags);

   status = UserSocket_Recvmsg(kargs.socketfd, &kmsg, totalLen, &bytesRecv);
   if (status == VMK_OK) {
      status = LinuxSocketCopyOutMsgHdr(&kmsg, &umsg, kargs.msg);
   }

   LinuxSocketFreeCopiedMsgHdr(&kmsg);

   if (status == VMK_OK) {
      return bytesRecv;
   } else {
      return User_TranslateStatus(status);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxSocketSendTo --
 *
 *	Send data on the given socket, connected or not.
 * 
 * Results:
 *	Number of bytes sent on success, a Linux error code otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
LinuxSocketSendTo(UserVA userArgs)
{
   int bytesSent = 0;
   LinuxSocketName kname;
   VMK_ReturnStatus status;
   LinuxSocketSendToArgs kargs;

   UWLOG_SyscallEnter("(userArgs=%#x)", userArgs);

   COPYIN_OR_RETURN(kargs, userArgs);

   UWLOG(1, "(fd=%u, buf=%#x, len=%d, flags=%#x, name@%#x, namelen=%d)",
      kargs.socketfd, kargs.buf, kargs.len, kargs.flags,
      kargs.name, kargs.namelen);

   if (kargs.namelen > sizeof kname) {
      return LINUX_ENAMETOOLONG;
   }

   if (kargs.namelen > 0) {
      status = User_CopyIn(&kname, kargs.name, kargs.namelen);
      if (status != VMK_OK) {
	 return status;
      }
   } else {
      memset(&kname, 0, sizeof kname);
   }

   status = UserSocket_Sendto(kargs.socketfd, kargs.buf, kargs.len,
                              kargs.flags, &kname, kargs.namelen,
                              &bytesSent);

   if (status == VMK_OK) {
      return bytesSent;
   } else {
      UWLOG(0, "sendto failed for socket %d (%#x:%s)",
            kargs.socketfd, status, UWLOG_ReturnStatusToString(status));
      return User_TranslateStatus(status);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxSocketRecvfrom --
 *
 *	Receive data on the given socket.
 * 
 * Results:
 *	Number of bytes received on success, a Linux error code otherwise.
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static int
LinuxSocketRecvfrom(UserVA userArgs)
{
   uint32 knamelen;
   int bytesRecv = 0;
   LinuxSocketName kname;
   VMK_ReturnStatus status;
   LinuxSocketName* knamePtr;
   LinuxSocketRecvfromArgs kargs;

   UWLOG_SyscallEnter("(userArgs=%#x)", userArgs);

   COPYIN_OR_RETURN(kargs, userArgs);

   /*
    * If address to namelen is supplied, copy it in.
    */
   if (kargs.namelen) {
      status = User_CopyIn(&knamelen,  kargs.namelen, sizeof(kargs.namelen));
      if (status != VMK_OK) {
          return status;
      }
   } else {
      knamelen = 0;
   }

   if (knamelen > sizeof kname) {
      return LINUX_ENAMETOOLONG;
   }

   if (kargs.name == 0) {
      knamePtr = NULL;
   } else {
      knamePtr = &kname;
   }

   UWLOG(1, "(fd=%u, buf=%#x, len=%d, flags=%#x, name@%#x, namelen=%d)",
             kargs.socketfd, kargs.buf, kargs.len, kargs.flags,
             kargs.name, knamelen);

   status = UserSocket_RecvFrom(kargs.socketfd, kargs.buf, kargs.len,
                                kargs.flags, knamePtr, &knamelen,
                                &bytesRecv);

   if (status == VMK_OK && knamePtr != NULL) {
      ASSERT(knamelen > 0);
      status = LinuxSocketCopyOutName(kargs.name, kargs.namelen, &kname,
				      knamelen);
   }

   if (status == VMK_OK) {
      return bytesRecv;
   } else {
      return User_TranslateStatus(status);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxSocketShutdown --
 *
 *	Shutdown part of a full-duplex connection
 * 
 * Results:
 *	0 on success, a Linux error code otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
LinuxSocketShutdown(UserVA userArgs)
{
   LinuxSocketShutdownArgs kargs;
   VMK_ReturnStatus status = VMK_NOT_SUPPORTED;

   UWLOG_SyscallEnter("(userArgs=%#x)", userArgs);

   COPYIN_OR_RETURN(kargs, userArgs);

   UWLOG(1, "(fd=%u, how=%d)", kargs.socketfd, kargs.how);

   status = UserSocket_Shutdown(kargs.socketfd,kargs.how);

   return User_TranslateStatus(status);
}


/* 
 *----------------------------------------------------------------------
 *
 * LinuxSocket_Socketcall - 
 *	Handler for linux syscall 102 
 *
 * Support: 100% (although some socket semantics haven't been tested)
 *
 * Error case: 100%
 *
 * Results:
 *	Depends on the call.  See above.
 *
 * Side effects:
 *	Depends on the call.  See above.
 *
 *----------------------------------------------------------------------
 */
int
LinuxSocket_Socketcall(uint32 whichCall, UserVA userArgs)
{
   switch (whichCall)
   {
   case LINUX_SOCKETCALL_SOCKET:		/* 0x01 */
      return LinuxSocketNewSocket(userArgs);
   case LINUX_SOCKETCALL_BIND:			/* 0x02 */
      return LinuxSocketBind(userArgs);
   case LINUX_SOCKETCALL_CONNECT:		/* 0x03 */
      return LinuxSocketConnect(userArgs);
   case LINUX_SOCKETCALL_LISTEN:		/* 0x04 */
      return LinuxSocketListen(userArgs);
   case LINUX_SOCKETCALL_ACCEPT:		/* 0x05 */
      return LinuxSocketAccept(userArgs);
   case LINUX_SOCKETCALL_GETSOCKNAME:		/* 0x06 */
      return LinuxSocketGetName(userArgs);
   case LINUX_SOCKETCALL_GETPEERNAME:		/* 0x07 */
      return LinuxSocketGetpeername(userArgs);
   case LINUX_SOCKETCALL_SOCKETPAIR:		/* 0x08 */
      return LinuxSocketSocketpair(userArgs);
   case LINUX_SOCKETCALL_SEND:			/* 0x09 */
      return LinuxSocketSend(userArgs);
   case LINUX_SOCKETCALL_RECV:			/* 0x0a */
      return LinuxSocketRecv(userArgs);
   case LINUX_SOCKETCALL_SENDTO:		/* 0x0b */
      return LinuxSocketSendTo(userArgs);
   case LINUX_SOCKETCALL_RECVFROM:		/* 0x0c */
      return LinuxSocketRecvfrom(userArgs);
   case LINUX_SOCKETCALL_SHUTDOWN:		/* 0x0d */
      return LinuxSocketShutdown(userArgs);
   case LINUX_SOCKETCALL_SETSOCKOPT:		/* 0x0e */
      return LinuxSocketSetsockopt(userArgs);
   case LINUX_SOCKETCALL_GETSOCKOPT:		/* 0x0f */
      return LinuxSocketGetsockopt(userArgs);
   case LINUX_SOCKETCALL_SENDMSG:		/* 0x10 */
      return LinuxSocketSendmsg(userArgs);
   case LINUX_SOCKETCALL_RECVMSG:		/* 0x11 */
      return LinuxSocketRecvmsg(userArgs);
   default:
      UWWarn("UNKNOWN/UNSUPPORTED socketcall op (whichCall=%#x, args@%#x)",
             whichCall, userArgs);
      UWLOG_StackTraceCurrent(1);
      return LINUX_ENOSYS;
   }
}
