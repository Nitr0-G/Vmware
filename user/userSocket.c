/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

#include "user_int.h"
#include "userSocket.h"
#include "net.h"
#include "socket_dist.h"

#define LOGLEVEL_MODULE UserSocket
#include "userLog.h"


/*
 *----------------------------------------------------------------------
 *
 * UserSocket_UsingVMKTcpIpStack --
 *
 *	Determines whether the UserWorld specified by the given Linux
 *	pid is using the VMkernel TCP/IP stack.
 *
 * Results:
 *	VMK_OK if so, otherwise if not.
 *
 * Side-effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserSocket_UsingVMKTcpIpStack(User_CartelInfo *uci)
{
   if (uci->socketInetCnx == USERSOCKETINET_NATIVE) {
      return VMK_OK;
   } else if (uci->socketInetCnx == USERSOCKETINET_PROXIED) {
      /*
       * XXX: No clean way to return "Not using vmk tcp/ip stack".  Seems like
       * overkill to create a new return status though...  All the callers of
       * this function are looking for is either VMK_OK or "not VMK_OK", so this
       * will work, albeit in a hacky manner.
       */
      return VMK_MODULE_NOT_LOADED;
   } else {
      NOT_IMPLEMENTED();
   }
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocket_CartelInit --
 *
 *	Initialize the generic socket part of the uci.  Currently this
 *	is only determining whether IP traffic will be proxied or go
 *	through the VMkernel TCP/IP stack (ie, native).
 *
 * Results:
 *	VMK_OK.
 *
 * Side-effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserSocket_CartelInit(User_CartelInfo *uci)
{
   Bool shouldProxy = TRUE;

   if (CONFIG_OPTION(USER_SOCKET_INET_TCPIP)) {
      if (Net_TcpipStackLoaded()) {
         /*
	  * Only if both the TCP/IP stack is loaded and the UserSocketInetTCP
	  * config option is set will we use the TCP/IP stack.
	  */
         UWLOG(1, "Using the vmkernel TCP/IP stack");
         shouldProxy = FALSE;
      } else {
         UWWarn("Non-proxied socket requested, but the vmkernel TCP/IP stack "
		"is not loaded.  Using proxied connection.");
      }
   } else {
      UWLOG(1, "Using proxied connection.");
   }

   if (shouldProxy) {
      uci->socketInetCnx = USERSOCKETINET_PROXIED;
   } else {
      uci->socketInetCnx = USERSOCKETINET_NATIVE;
   }

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocket_CartelCleanup --
 *
 *	No-op.
 *
 * Results:
 *	VMK_OK.
 *
 * Side-effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserSocket_CartelCleanup(User_CartelInfo *uci)
{
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketFind --
 *
 *	Find the given sockfd in the given cartel and set obj to the
 *	associated object.  Must be a USEROBJ_TYPE_SOCKET_XXXX.
 *
 * Results:
 *	VMK_OK if obj is set, VMK_INVALID_HANDLE if sockfd points to a
 *	non-socket object, other UserObj_Find errors.
 *
 * Side-effects:
 * 	Bumps ref count on obj up.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserSocketFind(User_CartelInfo* uci,
               LinuxFd sockfd,
               UserObj** obj)
{
   VMK_ReturnStatus status;

   status = UserObj_Find(uci, sockfd, obj);
   if (status != VMK_OK) {
      UWLOG(1, "No socket found for fd %d (%#x:%s)",
            sockfd, status, VMK_ReturnStatusToString(status));
   } else if ((*obj)->type != USEROBJ_TYPE_SOCKET_INET &&
	      (*obj)->type != USEROBJ_TYPE_SOCKET_UNIX &&
	      (*obj)->type != USEROBJ_TYPE_SOCKET_UNIX_DATA &&
	      (*obj)->type != USEROBJ_TYPE_SOCKET_UNIX_SERVER &&
	      (*obj)->type != USEROBJ_TYPE_PROXY_SOCKET) {
      (void)UserObj_Release(uci, *obj);
      status = VMK_NOT_A_SOCKET;

      if (vmx86_debug) {
         *obj = NULL;
      }
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketRelease --
 *
 *	Release object found via UserSocketFind.
 *
 * Results:
 *	None
 *
 * Side-effects:
 * 	Drops ref count on obj
 *
 *----------------------------------------------------------------------
 */
static void
UserSocketRelease(User_CartelInfo *uci, UserObj* obj)
{
   ASSERT(obj != NULL);
   (void)UserObj_Release(uci, obj);
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocket_NewSocket --
 *
 *	Create a new socket object with the given type (i.e., stream)
 *	and protocol (i.e., tcp or udp) by dispatching to the appropriate
 *	family create function.  Adds the new socket object to the fd list
 *	in the given cartel.
 *
 * Results:
 *	VMK_OK if created and added, otherwise on error; socketfd set
 *	if VMK_OK.
 *
 * Side-effects:
 * 	Socket created, process fd table updated
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserSocket_NewSocket(LinuxSocketFamily family,	   // IN
                     LinuxSocketType type,         // IN
                     LinuxSocketProtocol protocol, // IN
                     LinuxFd* socketfd)            // OUT
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   VMK_ReturnStatus status;
   UserObj* obj;

   ASSERT(socketfd);

   *socketfd = UserObj_FDReserve(uci);
   if (*socketfd == USEROBJ_INVALID_HANDLE) {
      UWLOG(0, "No free uw descriptors available");
      return VMK_NO_FREE_HANDLES;
   }

   switch(family) {
      case LINUX_SOCKETFAMILY_UNIX:
         /*
          * Unix domain sockets are visible in the file system
          * namespace, and since the VMFS doesn't support them,
          * they're always proxied.
          */
         status = UserProxy_CreateSocket(uci, family, type, protocol, &obj);
	 break;
      case LINUX_SOCKETFAMILY_INET:
         if (uci->socketInetCnx == USERSOCKETINET_PROXIED) {
            status = UserProxy_CreateSocket(uci, family, type, protocol, &obj);
         } else if (uci->socketInetCnx == USERSOCKETINET_NATIVE) {
            status = UserSocketInet_Create(uci, type, protocol, &obj);
         } else {
	    NOT_IMPLEMENTED();
	 }
	 break;
      case LINUX_SOCKETFAMILY_VMK:
         status = UserSocketUnix_Create(uci, type, protocol, &obj);
	 break;
      default:
         status = VMK_ADDRFAM_UNSUPP;
         break;
   }

   if (status == VMK_OK) {
      UserObj_FDAddObj(uci, *socketfd, obj);
   } else {
      UWLOG(0, "UserSocket%s_Create(%d) failed: %#x:%s",
	    family == LINUX_SOCKETFAMILY_UNIX ? "Unix" :
            (family == LINUX_SOCKETFAMILY_INET ? "Inet" :
	    (family == LINUX_SOCKETFAMILY_VMK ? "VMK" : "unknown")),
            family, status, VMK_ReturnStatusToString(status));
      UserObj_FDUnreserve(uci, *socketfd);
   }
   
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocket_Bind --
 *
 *	Bind the given socket to the given address.
 *
 * Results:
 *	VMK_OK if socket found and bound, otherwise if error.
 *
 * Side-effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserSocket_Bind(LinuxFd sockfd,
                LinuxSocketName* name,
		uint32 nameLen)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   UserObj* obj;
   VMK_ReturnStatus status;

   status = UserSocketFind(uci, sockfd, &obj);
   if (status == VMK_OK) {
      status = obj->methods->bind(obj, name, nameLen);
      UserSocketRelease(uci, obj);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocket_Connect --
 *
 *	Connect the given socket to the given name.
 *
 * Results:
 *	VMK_OK if okay, otherwise on error.
 *
 * Side-effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
UserSocket_Connect(LinuxFd sockfd,
                   LinuxSocketName* name,
		   uint32 nameLen)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   UserObj* obj;
   VMK_ReturnStatus status;

   status = UserSocketFind(uci, sockfd, &obj);
   if (status == VMK_OK) {
      status = obj->methods->connect(obj, name, nameLen);
      UserSocketRelease(uci, obj);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocket_Listen --
 *
 *	Listen for incoming connections on the given socket.
 *
 * Results:
 *	VMK_OK if listen succeeds, otherwise on error.
 *
 * Side-effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
UserSocket_Listen(LinuxFd sockfd,
                  uint32 backlog)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   UserObj* obj;
   VMK_ReturnStatus status;

   status = UserSocketFind(uci, sockfd, &obj);
   if (status == VMK_OK) {
      status = obj->methods->listen(obj, backlog);
      UserSocketRelease(uci, obj);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocket_Accept --
 *
 *	Accept a remote connection on the given socket.
 *
 * Results:
 *	VMK_OK if accept completed, otherwise on error.
 *
 * Side-effects:
 * 	A new connections on given socket is accepted.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
UserSocket_Accept(LinuxFd sockfd,
                  LinuxFd* acceptedfd,
		  LinuxSocketName* name,
		  uint32* nameLen)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   UserObj* obj;
   UserObj* newObj;
   VMK_ReturnStatus status;

   *acceptedfd = UserObj_FDReserve(uci);
   if (*acceptedfd == USEROBJ_INVALID_HANDLE) {
      return VMK_NO_FREE_HANDLES;
   }

   status = UserSocketFind(uci, sockfd, &obj);
   if (status == VMK_OK) {
      status = obj->methods->accept(obj, &newObj, name, nameLen);
      UserSocketRelease(uci, obj);
   }

   if (status == VMK_OK) {
      UserObj_FDAddObj(uci, *acceptedfd, newObj);
   } else {
      UserObj_FDUnreserve(uci, *acceptedfd);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocket_GetName --
 *
 *	Get the name of the given socket.
 *
 * Results:
 *	VMK_OK if okay, otherwise if error.
 *
 * Side-effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
UserSocket_GetName(LinuxFd sockfd,
                   LinuxSocketName* name,
		   uint32* nameLen)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   UserObj* obj;
   VMK_ReturnStatus status;

   status = UserSocketFind(uci, sockfd, &obj);
   if (status == VMK_OK) {
      status = obj->methods->getSocketName(obj, name, nameLen);
      UserSocketRelease(uci, obj);
   }
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocket_Socketpair --
 *
 *	Create two sockets of the given specifications, then connect them.
 *
 * Results:
 *	VMK_OK if okay, otherwise if error.
 *
 * Side-effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
UserSocket_Socketpair(LinuxSocketFamily family,
		      LinuxSocketType type,
		      LinuxSocketProtocol protocol,
		      LinuxFd sockfds[2])
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   VMK_ReturnStatus status;
   UserObj* obj1;
   UserObj* obj2;

   /*
    * Only supported for unix-domain sockets, which are only supported
    * in the COS, so we can go straight to the proxy for this case.
    */
   if (family != LINUX_SOCKETFAMILY_UNIX) {
      UWLOG(0, "Unsupported family %d for socketpair (only unix domain socketpairs are supported)",
            family);
      return LINUX_EINVAL;
   }

   sockfds[0] = UserObj_FDReserve(uci);
   if (sockfds[0] == USEROBJ_INVALID_HANDLE) {
      UWLOG(0, "No free uw descriptors available");
      return VMK_NO_FREE_HANDLES;
   }

   sockfds[1] = UserObj_FDReserve(uci);
   if (sockfds[1] == USEROBJ_INVALID_HANDLE) {
      UWLOG(0, "No free uw descriptors available");
      UserObj_FDUnreserve(uci, sockfds[0]);
      return VMK_NO_FREE_HANDLES;
   }

   status = UserProxy_Socketpair(family, type, protocol, &obj1, &obj2);
   if (status == VMK_OK) {
      UserObj_FDAddObj(uci, sockfds[0], obj1);
      UserObj_FDAddObj(uci, sockfds[1], obj2);
   } else {
      UserObj_FDUnreserve(uci, sockfds[0]);
      UserObj_FDUnreserve(uci, sockfds[1]);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocket_Sendto --
 *
 *	Send a message to a specific address, if provided, or to the other
 *	side of the connection if connected.
 *
 * Results:
 *	VMK_OK if okay, otherwise on error.
 *
 * Side-effects:
 *	*bytesSent is filled with the number of bytes sent.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
UserSocket_Sendto(LinuxFd sockfd,
		  UserVA /*void**/ userBuf,
		  uint32 userLen,
		  uint32 flags,
		  LinuxSocketName* name,
		  uint32 nameLen,
		  uint32* bytesSent)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   VMK_ReturnStatus status;
   UserObj* obj;
   LinuxMsgHdr msg;
   LinuxIovec iov;

   ASSERT(bytesSent);

   status = UserSocketFind(uci, sockfd, &obj);
   if (status != VMK_OK) {
      return status;
   }

   if (userLen == 0) {
      UserSocketRelease(uci, obj);
      *bytesSent = 0;
      return VMK_OK;
   }

   if (! UserObj_IsOpenForBlocking(obj)) {
      flags |= LINUX_SOCKET_MSG_DONTWAIT;
   }

   iov.base = userBuf;
   iov.length = userLen;

   msg.name = name;
   msg.nameLen = nameLen;
   msg.iov = &iov;
   msg.iovLen = 1;
   msg.control = NULL;
   msg.controlLen = 0;
   msg.flags = flags;

   status = obj->methods->sendmsg(obj, &msg, userLen, bytesSent);
   UserSocketRelease(uci, obj);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocket_RecvFrom --
 *
 *	Receive a message from a specific address, if provided, or from the
 *	other side of the connection if connected.
 *
 * Results:
 *	VMK_OK if okay, otherwise on error.
 *
 * Side-effects:
 *	*bytesRecv is filled with the number of bytes received.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
UserSocket_RecvFrom(LinuxFd sockfd,
		    UserVA /*void**/ userBuf,
		    uint32 userLen,
		    uint32 flags,
		    LinuxSocketName* name,
		    uint32* nameLen,
		    uint32* bytesRecv)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   VMK_ReturnStatus status;
   UserObj* obj;
   LinuxMsgHdr msg;
   LinuxIovec iov;

   ASSERT(bytesRecv);

   status = UserSocketFind(uci, sockfd, &obj);
   if (status != VMK_OK) {
      return status;
   }

   if (userLen == 0) {
      UserSocketRelease(uci, obj);
      *bytesRecv = 0;
      return VMK_OK;
   }

   if (! UserObj_IsOpenForBlocking(obj)) {
      flags |= LINUX_SOCKET_MSG_DONTWAIT;
   }

   iov.base = userBuf;
   iov.length = userLen;

   msg.name = name;
   msg.nameLen = nameLen == NULL ? 0 : *nameLen;
   msg.iov = &iov;
   msg.iovLen = 1;
   msg.control = NULL;
   msg.controlLen = 0;
   msg.flags = flags;

   status = obj->methods->recvmsg(obj, &msg, userLen, bytesRecv);
   UserSocketRelease(uci, obj);

   if (status == VMK_OK && nameLen != NULL) {
      *nameLen = msg.nameLen;
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocket_Setsockopt --
 *
 *	Set the given socket option to the given value.
 *
 * Results:
 *	VMK_OK if okay, otherwise on error.
 *
 * Side-effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
UserSocket_Setsockopt(LinuxFd sockfd,
		      int level,
		      int optName,
		      char* optVal,
		      int optLen)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   VMK_ReturnStatus status;
   UserObj* obj;

   ASSERT(optVal);

   status = UserSocketFind(uci, sockfd, &obj);
   if (status == VMK_OK) {
      status = obj->methods->setsockopt(obj, level, optName, optVal, optLen);
      UserSocketRelease(uci, obj);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocket_Getsockopt --
 *
 *	Get the given socket option.
 *
 * Results:
 *	VMK_OK if okay, otherwise on error.
 *
 * Side-effects:
 *	*optVal and *optLen are set to the option value and length,
 *	respectively.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
UserSocket_Getsockopt(LinuxFd sockfd,
		      int level,
		      int optName,
		      char* optVal,
		      int* optLen)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   VMK_ReturnStatus status;
   UserObj* obj;

   ASSERT(optVal);
   ASSERT(optLen);

   status = UserSocketFind(uci, sockfd, &obj);
   if (status == VMK_OK) {
      status = obj->methods->getsockopt(obj, level, optName, optVal, optLen);
      UserSocketRelease(uci, obj);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocket_Sendmsg --
 *
 *	Send a generic message on the given socket.
 *
 * Results:
 *	VMK_OK if okay, otherwise on error.
 *
 * Side-effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
UserSocket_Sendmsg(LinuxFd sockfd,
		   LinuxMsgHdr* msg,
		   uint32 userLen,
		   uint32* bytesSent)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   VMK_ReturnStatus status;
   UserObj* obj;

   ASSERT(bytesSent);

   status = UserSocketFind(uci, sockfd, &obj);
   if (status != VMK_OK) {
      return status;
   }

   if (userLen == 0) {
      UserSocketRelease(uci, obj);
      *bytesSent = 0;
      return VMK_OK;
   }

   status = obj->methods->sendmsg(obj, msg, userLen, bytesSent);
   UserSocketRelease(uci, obj);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocket_Recvmsg --
 *
 *	Receive a generic message on the given socket.
 *
 * Results:
 *	VMK_OK if okay, otherwise on error.
 *
 * Side-effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
UserSocket_Recvmsg(LinuxFd sockfd,
		   LinuxMsgHdr* msg,
		   uint32 userLen,
		   uint32* bytesRecv)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   VMK_ReturnStatus status;
   UserObj* obj;

   ASSERT(bytesRecv);

   status = UserSocketFind(uci, sockfd, &obj);
   if (status != VMK_OK) {
      return status;
   }

   if (userLen == 0) {
      UserSocketRelease(uci, obj);
      *bytesRecv = 0;
      return VMK_OK;
   }

   status = obj->methods->recvmsg(obj, msg, userLen, bytesRecv);

   UserSocketRelease(uci, obj);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocket_GetPeerName --
 *
 *	Get the name of connected peer.
 *
 * Results:
 *	VMK_OK if okay, otherwise if error.
 *
 * Side-effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
UserSocket_GetPeerName(LinuxFd sockfd,
                   LinuxSocketName* name,
		   uint32* nameLen)
{
   UserObj* obj;
   VMK_ReturnStatus status;
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;

   status = UserSocketFind(uci, sockfd, &obj);

   if (status == VMK_OK) {
      status = obj->methods->getPeerName(obj, name, nameLen);
      UserSocketRelease(uci, obj);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocket_Shutdown --
 *
 *      Shutdown part of a full-duplex connection.
 *
 * Results:
 *      VMK_OK if okay, otherwise if error.
 *
 * Side-effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
UserSocket_Shutdown(LinuxFd sockfd,
                   int how)
{
   UserObj* obj;
   VMK_ReturnStatus status;
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;

   status = UserSocketFind(uci, sockfd, &obj);

   if (status == VMK_OK) {
      status = obj->methods->shutdown(obj, how);
      UserSocketRelease(uci, obj);
   }

   return status;
}


