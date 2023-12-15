/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/* 
 * userSocket.h - 
 *	UserWorld access to sockets
 */

#ifndef VMKERNEL_USER_USERSOCKET_H
#define VMKERNEL_USER_USERSOCKET_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "linuxAPI.h"
#include "vmkpoll.h"

struct UserSocketInet_ObjInfo;
struct User_CartelInfo;

extern VMK_ReturnStatus UserSocketUnix_Init(void);

/*
 * Type of connection that UserSocketInet_* will use.
 */
typedef enum {
   /*
    * This should never be used.
    */
   USERSOCKETINET_UNDEFINED,
   /*
    * UserWorld is using the VMkernel TCP/IP stack.
    */
   USERSOCKETINET_NATIVE,
   /*
    * UserWorld is using the proxy to go through the COS TCP/IP stack.
    */
   USERSOCKETINET_PROXIED
} UserSocketInetCnx;

/*
 * User inet socket object info.
 */
typedef struct UserSocketInet_ObjInfo {
   int                socket;
   /*
    * TRUE if this struct "owns" the underlying bsd socket.  Ownership is
    * defined as the need to close the socket when the object is closed.
    * By default the socket is owned by the object.  If this object is passed to
    * another cartel, then this object loses ownership of the underlying socket
    * and its corresponding object in the other cartel gets ownership.
    */
   Bool		      owner;
   SP_SpinLock        pollLock;
   VMKPollWaitersList waiters;
   VMKPollEvent       pollEvents;
} UserSocketInet_ObjInfo;

extern VMK_ReturnStatus UserSocket_CartelInit(struct User_CartelInfo *uci);
extern VMK_ReturnStatus UserSocket_CartelCleanup(struct User_CartelInfo *uci);
extern VMK_ReturnStatus UserSocket_UsingVMKTcpIpStack(struct User_CartelInfo *uci);

/*
 * Functions called by UserPipe_*.
 */
extern void UserSocketInet_ObjInit(UserObj *obj,
				   struct UserSocketInet_ObjInfo *socketInfo,
				   int socket);
extern VMK_ReturnStatus UserSocketInet_GetSocket(UserObj *obj, int *socket);
extern VMK_ReturnStatus UserSocketInet_RelinquishOwnership(UserObj *obj);
extern VMK_ReturnStatus UserSocketInet_CloseSocket(struct User_CartelInfo *uci,
						   int socket);


/*
 * Functions called by UserSocket_NewSocket.
 */
extern VMK_ReturnStatus UserSocketInet_Create(struct User_CartelInfo* uci,
					      LinuxSocketType type,
					      LinuxSocketProtocol protocol,
					      UserObj** outObj);
extern VMK_ReturnStatus UserSocketUnix_Create(struct User_CartelInfo* uci,
					      LinuxSocketType type,
					      LinuxSocketProtocol protocol,
					      UserObj** outObj);

/*
 * Functions called by linuxSocket.c.
 */
extern VMK_ReturnStatus UserSocket_NewSocket(LinuxSocketFamily family,
					     LinuxSocketType type,
					     LinuxSocketProtocol protocol,
					     LinuxFd* socketfd);
extern VMK_ReturnStatus UserSocket_Bind(LinuxFd socketfd,
					LinuxSocketName* name,
					uint32 nameLen);
extern VMK_ReturnStatus UserSocket_Connect(LinuxFd socketfd,
					   LinuxSocketName* name,
					   uint32 nameLen);
extern VMK_ReturnStatus UserSocket_Listen(LinuxFd socketfd,
                                          uint32 backlog);
extern VMK_ReturnStatus UserSocket_Accept(LinuxFd socketfd,
                                          LinuxFd *acceptedfd,
					  LinuxSocketName* name,
					  uint32* nameLen);
extern VMK_ReturnStatus UserSocket_GetName(LinuxFd socketfd,
					   LinuxSocketName* name,
					   uint32* nameLen);
extern VMK_ReturnStatus UserSocket_Socketpair(LinuxSocketFamily family,
					      LinuxSocketType type,
					      LinuxSocketProtocol protocol,
					      LinuxFd sockfds[2]);
extern VMK_ReturnStatus UserSocket_Sendto(LinuxFd socketfd,
					  UserVA /*void**/ userBuf,
					  uint32 userLen,
					  uint32 flags,
					  LinuxSocketName* name,
					  uint32 nameLen,
					  uint32* bytesSent);
extern VMK_ReturnStatus UserSocket_RecvFrom(LinuxFd socketfd,
					    UserVA /*void**/ userBuf,
					    uint32 userLen,
					    uint32 flags,
					    LinuxSocketName* name,
					    uint32* nameLen,
					    uint32* bytesRecv);
extern VMK_ReturnStatus UserSocket_Setsockopt(LinuxFd socketfd,
					      int level,
					      int optName,
					      char* optVal,
					      int optLen);
extern VMK_ReturnStatus UserSocket_Getsockopt(LinuxFd socketfd,
					      int level,
					      int optName,
					      char* optVal,
					      int* optLen);
extern VMK_ReturnStatus UserSocket_Sendmsg(LinuxFd socketfd,
					   LinuxMsgHdr* msg,
					   uint32 userLen,
					   uint32* bytesSent);
extern VMK_ReturnStatus UserSocket_Recvmsg(LinuxFd socketfd,
					   LinuxMsgHdr* msg,
					   uint32 userLen,
					   uint32* bytesRecv);
extern VMK_ReturnStatus UserSocket_GetPeerName(LinuxFd socketfd,
                                               LinuxSocketName* name,
                                               uint32* nameLen);
extern VMK_ReturnStatus UserSocket_Shutdown(LinuxFd socketfd,
                                            int how);

#endif /* VMKERNEL_USER_USERSOCKET_H */
