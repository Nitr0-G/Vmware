/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. 
 * -- VMware Confidential
 * **********************************************************/

/*
 * socket_dist.h --
 *
 *      BSD style sockets interface.
 */

#ifndef _SOCKET_DIST_H_
#define _SOCKET_DIST_H_

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMK_MODULE
#define INCLUDE_ALLOW_DISTRIBUTE
#include "includeCheck.h"

#include "net_dist.h"
#include "world_dist.h"
#include "scattergather.h"

 
#ifdef NET_SOCK_DEBUG
#define NetSockDbg(_fmt, _args...) \
   _Log("NETDBG: 0x%x:" _fmt, (uint32)__builtin_return_address(0), _args)
#else
#define NetSockDbg(_args...)
#endif

#include "toe.h"

#define DEFAULT_STACK NULL
struct iovec;
struct sockaddr;
struct sockaddr_in_bsd;
struct stat;
#define NET_SOCKET_POLLIN	0x01
#define NET_SOCKET_POLLOUT	0x04
/*
 *  Socket callback function: Arg. 1 (int) socket descriptor
 *                            Arg. 2 (void *) callback context
 *                            Arg. 3 (int)  unused
 */
typedef void (*Net_SocketCallbackFn)(int, void *, int);

typedef struct Net_StackFunctions {
   VMK_ReturnStatus (*add_route)(uint32 dst_addr, uint32 netmask, uint32 gw);
   void             (*terminate_connections)(void);
   void             (*register_callback)(int socket, 
                                         Net_SocketCallbackFn fn, 
                                         void *arg);
   VMK_ReturnStatus (*socket)(int type, int protocol, int *newSocket);
   VMK_ReturnStatus (*bind)(int socket, struct sockaddr_in_bsd *nam, int namelen);
   VMK_ReturnStatus (*sendto)(int so, int flags, struct sockaddr *addr, void *data, 
			      int len, int *bytesSent);
   VMK_ReturnStatus (*sendto_sg)(int so, int flags, struct sockaddr *addr, void *hdr, 
		                 int hdrLen, SG_Array *sgArr, int *bytesSent);
   VMK_ReturnStatus (*sendto_linux)(int so, int flags, 
                                    struct iovec *iov, int iovlen, 
                                    int len, int *copied);
   
   VMK_ReturnStatus (*recvfrom)(int so, int flags, void *data, int len, 
		                struct sockaddr *from, int *fromlen, 
				int *bytesRecieved);

   VMK_ReturnStatus (*recvfrom_linux)(int so, int flags, struct iovec *iov, int iovlen,
                                      int len, int *bytesRecieved);

   VMK_ReturnStatus (*setsockopt)(int so, int level, int optname, 
				  const void *optval, int optlen);

   VMK_ReturnStatus (*setsockopt_linux)(int so, int level, int optname, 
                                        const void *optval, int optlen);

   VMK_ReturnStatus (*getsockopt)(int so, int level, int optname, 
				  void *optval, int *optlen);
   VMK_ReturnStatus (*getsockopt_linux)(int so, int level, int optname, 
                                        void *optval, int *optlen);
   VMK_ReturnStatus (*listen)(int so, int backlog);
   VMK_ReturnStatus (*accept)(int s, Bool canBlock, struct sockaddr *name, int *namelen, 
			      int *newSocket);
   VMK_ReturnStatus (*getsockname)(int s, struct sockaddr *name, int *namelen);
   VMK_ReturnStatus (*connect)(int s, struct sockaddr *name, int namelen);
   VMK_ReturnStatus (*close)(int s);
   VMK_ReturnStatus (*shutdown)(int s, int how);
   int (*poll)(int s, int events);
   //
   // The worldID should be of type World_ID but it can't be because the tcpip
   // stack can't bring in the necessary include files.  The definition of World_ID 
   // needs to be isolated into an include file that can be included by 
   // vmkernel/networking/lib/support.c
   //
   int (*poll_for_world)(int s, int events, unsigned long worldID);
   int (*check_socket)(int s);
   void (*dumpState)(void);
   VMK_ReturnStatus (*getpeername)(int s, struct sockaddr *name, int *namelen);
   VMK_ReturnStatus (*ioctl)(int socket, int cmd, char *arg);
   VMK_ReturnStatus (*stat)(int socket, struct stat *buf);
} Net_StackFunctions;


struct Net_TcpipStack;

typedef int (*Net_TcpipInitFunc)(struct Net_TcpipStack *stack, 
                                 PPN *toeCmdPPNs, uint32 numPPNs);
typedef int (*Net_TcpipExitFunc)(void);

typedef struct Net_TcpipStack {
   VA readOnlyBase;
   VA readWriteBase;
   VA textBase;
   VA dataBase;
   VA bssBase;
   Net_TcpipInitFunc initFunc;
   Net_TcpipExitFunc exitFunc;
   struct World_Handle *worldHandle; // World that owns this TOE instance
   Net_StackFunctions sf;
} Net_TcpipStack;

EXTERN Net_StackFunctions stackFunctions;

Bool Net_TcpipStackLoaded(void);

static INLINE VMK_ReturnStatus 
Net_RegisterCallback(int socket, 
                     Net_SocketCallbackFn fn,
                     void *arg,
                     struct Net_StackFunctions *sf)
{
   if (sf == DEFAULT_STACK) {
      sf = &stackFunctions;
   }

   if (sf->register_callback != NULL) {
      NetSockDbg("Register socket callback: %d\n", socket);
      sf->register_callback(socket, fn, arg);
      return VMK_OK;
   } else {
      return VMK_NOT_SUPPORTED;
   }
}

static INLINE VMK_ReturnStatus 
Net_CreateSocket(int type, 
                 int protocol, 
                 int *newSocket,
                 struct Net_StackFunctions *sf)
{
   if (sf == DEFAULT_STACK) {
      sf = &stackFunctions;
   }
   if (sf->socket != NULL) {
      VMK_ReturnStatus retval = sf->socket(type, protocol, newSocket);
      NetSockDbg("Created Socket: %d\n", *newSocket);
      return retval;
   } else {
      return VMK_NOT_SUPPORTED;
   }
}

static INLINE VMK_ReturnStatus 
Net_Bind(int socket, 
         struct sockaddr_in_bsd *nam, 
         int namelen,
         struct Net_StackFunctions *sf)
{
   if (sf == DEFAULT_STACK) {
      sf = &stackFunctions;
   }

   if (sf->bind != NULL) {
      NetSockDbg("Bound Socket: %d\n", socket);
      return sf->bind(socket, nam, namelen);
   } else {
      return VMK_NOT_SUPPORTED;
   }
}

static INLINE VMK_ReturnStatus 
Net_SendTo(int so, int flags, struct sockaddr *addr,
           void *data, int len, int *bytesSent,
           struct Net_StackFunctions *sf)
{
   if (sf == DEFAULT_STACK) {
      sf = &stackFunctions;
   }

   if (sf->sendto != NULL) {
      return sf->sendto(so, flags, addr, data, len, bytesSent);
   } else {
      return VMK_NOT_SUPPORTED;
   }
}


static INLINE VMK_ReturnStatus 
Net_SendToSG(int so, int flags, struct sockaddr *addr, 
             void *hdr, int hdrLen,
             SG_Array *dataSG, int *bytesSent,
             struct Net_StackFunctions *sf)
{
   if (sf == DEFAULT_STACK) {
      sf = &stackFunctions;
   }

   if (sf->sendto != NULL) {
      return sf->sendto_sg(so, flags, addr, hdr, hdrLen, dataSG, bytesSent);
   } else {
      return VMK_NOT_SUPPORTED;
   }
}

static INLINE VMK_ReturnStatus 
Net_SendAll(int so, void *bufOrHdr, int bufOrHdrLength, 
            SG_Array *sgArr)
{
   return VMK_FAILURE;
}

static INLINE VMK_ReturnStatus 
Net_RecvFrom(int so, int flags, void *data, int len, 
             struct sockaddr *from, int *fromlen, 
             int *bytesReceived, struct Net_StackFunctions *sf)
{
   if (sf == DEFAULT_STACK) {
      sf = &stackFunctions;
   }


   if (sf->recvfrom != NULL) {
      return sf->recvfrom(so, flags, data, len, from, fromlen, bytesReceived);
   } else {
      return VMK_NOT_SUPPORTED;
   }
}

static INLINE VMK_ReturnStatus 
Net_SetSockOpt(int so, int level, int optname, 
               const void *optval, int optlen,
               struct Net_StackFunctions *sf)
{
   if (sf == DEFAULT_STACK) {
      sf = &stackFunctions;
   }
   
   if (sf->setsockopt != NULL) {
      return sf->setsockopt(so, level, optname, optval, optlen);
   } else {
      return VMK_NOT_SUPPORTED;
   }
}

static INLINE VMK_ReturnStatus 
Net_GetSockOpt(int so, int level, int optname, 
               void *optval, int *optlen,
               struct Net_StackFunctions *sf)
{
   if (sf == DEFAULT_STACK) {
      sf = &stackFunctions;
   }

   if (sf->getsockopt != NULL) {
      return sf->getsockopt(so, level, optname, optval, optlen);
   } else {
      return VMK_NOT_SUPPORTED;
   }
}

static INLINE VMK_ReturnStatus 
Net_Listen(int so, int backlog, struct Net_StackFunctions *sf)
{
   if (sf == DEFAULT_STACK) {
      sf = &stackFunctions;
   }

   if (sf->listen != NULL) {
      NetSockDbg("Listening on socket: %d\n", so);
      return sf->listen(so, backlog);
   } else {
      return VMK_NOT_SUPPORTED;
   }
}

static INLINE VMK_ReturnStatus 
Net_Accept(int s, Bool canBlock,
           struct sockaddr *name, int *namelen,
           int *newSocket, struct Net_StackFunctions *sf)
{
   if (sf == DEFAULT_STACK) {
      sf = &stackFunctions;
   }

   if (sf->accept != NULL) {
      VMK_ReturnStatus status = sf->accept(s, canBlock, name, namelen, newSocket);
      NetSockDbg("Accepted new socket from %d: %d\n", s, *newSocket);
      return status;
   } else {
      return VMK_NOT_SUPPORTED;
   }
}

static INLINE VMK_ReturnStatus 
Net_GetSockName(int s, struct sockaddr *name, 
                int *namelen, struct Net_StackFunctions *sf)
{
   if (sf == DEFAULT_STACK) {
      sf = &stackFunctions;
   }
   
   if (sf->getsockname != NULL) {
      return sf->getsockname(s, name, namelen);
   } else {
      return VMK_NOT_SUPPORTED;
   }
}

static INLINE VMK_ReturnStatus 
Net_GetPeerName(int s, struct sockaddr *name,
                int *namelen, struct Net_StackFunctions *sf)
{
   if (sf == DEFAULT_STACK) {
      sf = &stackFunctions;
   }
   
   if (sf->getpeername != NULL) {
      return sf->getpeername(s, name, namelen);
   } else {
      return VMK_NOT_SUPPORTED;
   }
}


static INLINE VMK_ReturnStatus 
Net_SocketIoctl(int s, int cmd, char *data,
                struct Net_StackFunctions *sf)
{
   if (sf == DEFAULT_STACK) {
      sf = &stackFunctions;
   }
   
   if (sf->ioctl != NULL) {
      return sf->ioctl(s, cmd, data);
   } else {
      return VMK_NOT_SUPPORTED;
   }
}

static INLINE VMK_ReturnStatus 
Net_SocketStat(int s, struct stat *buf,
               struct Net_StackFunctions *sf)
{
   if (sf == DEFAULT_STACK) {
      sf = &stackFunctions;
   }
   
   if (sf->stat != NULL) {
      return sf->stat(s, buf);
   } else {
      return VMK_NOT_SUPPORTED;
   }
}

static INLINE VMK_ReturnStatus 
Net_ConnectSocket(int s, 
                  struct sockaddr *name, int namelen,
                  struct Net_StackFunctions *sf)
{
   if (sf == DEFAULT_STACK) {
      sf = &stackFunctions;
   }

   if (sf->connect != NULL) {
      NetSockDbg("Connected on socket: %d\n", s);
      return sf->connect(s, name, namelen);
   } else {
      return VMK_NOT_SUPPORTED;
   }
}

static INLINE VMK_ReturnStatus 
Net_CloseSocket(int s, struct Net_StackFunctions *sf)
{
   if (sf == DEFAULT_STACK) {
      sf = &stackFunctions;
   }

   if (sf->close != NULL) {
      NetSockDbg("Closing socket: %d\n", s);
      return sf->close(s);
   } else {
      return VMK_NOT_SUPPORTED;
   }
}

static INLINE VMK_ReturnStatus 
Net_ShutdownSocket(int s, int how, struct Net_StackFunctions *sf)
{
   if (sf == DEFAULT_STACK) {
      sf = &stackFunctions;
   }

   if (sf->shutdown != NULL) {
      NetSockDbg("Shuttingdown socket: %d how: %d\n", s, how);
      return sf->shutdown(s, how);
   } else {
      return VMK_NOT_SUPPORTED;
   }
}

static INLINE int 
Net_PollSocket(int s, int events, struct Net_StackFunctions *sf)
{
   if (sf == DEFAULT_STACK) {
      sf = &stackFunctions;
   }

   if (sf->poll != NULL) {
      return sf->poll(s, events);
   } else {
      return -1;
   }
}

static INLINE int 
Net_PollSocketForWorld(int s, int events, World_ID worldID,
                       struct Net_StackFunctions *sf)
{
   if (sf == DEFAULT_STACK) {
      sf = &stackFunctions;
   }

   if (sf->poll_for_world != NULL) {
      return sf->poll_for_world(s, events, worldID);
   } else {
      return -1;
   }
}

static INLINE int 
Net_CheckSocket(int s, struct Net_StackFunctions *sf)
{
   if (sf == DEFAULT_STACK) {
      sf = &stackFunctions;
   }

   if (sf->check_socket != NULL) {
      return sf->check_socket(s);
   } else {
      return -1;
   }
}

static INLINE VMK_ReturnStatus 
NetLinuxRecvFrom(int so, int flags, 
                 struct iovec *iov, int iovlen,
                 int len, int *bytesReceived, 
                 struct Net_StackFunctions *sf)
{
   if (sf == DEFAULT_STACK) {
      sf = &stackFunctions;
   }


   if (sf->recvfrom_linux != NULL) {
      return sf->recvfrom_linux(so, flags, iov, iovlen, len, bytesReceived);
   } else {
      return VMK_NOT_SUPPORTED;
   }
}

static INLINE VMK_ReturnStatus 
NetLinuxSetSockOpt(int so, int level, int optname, 
                   const void *optval, int optlen,
                   struct Net_StackFunctions *sf)
{
   if (sf == DEFAULT_STACK) {
      sf = &stackFunctions;
   }
   
   if (sf->setsockopt != NULL) {
      return sf->setsockopt_linux(so, level, optname, optval, optlen);
   } else {
      return VMK_NOT_SUPPORTED;
   }
}

static INLINE VMK_ReturnStatus 
NetLinuxSendTo(int so, int flags, 
               struct iovec *iov,
               int iovlen, 
               int len,
               int *bytesSent,
               struct Net_StackFunctions *sf)
{
   if (sf == DEFAULT_STACK) {
      sf = &stackFunctions;
   }

   if (sf->sendto != NULL) {
      return sf->sendto_linux(so, flags, iov, iovlen, len, bytesSent);
   } else {
      return VMK_NOT_SUPPORTED;
   }
}

static INLINE VMK_ReturnStatus 
NetLinuxGetSockOpt(int so, int level, int optname, 
                   void *optval, int *optlen,
                   struct Net_StackFunctions *sf)
{
   if (sf == DEFAULT_STACK) {
      sf = &stackFunctions;
   }

   if (sf->getsockopt != NULL) {
      return sf->getsockopt_linux(so, level, optname, optval, optlen);
   } else {
      return VMK_NOT_SUPPORTED;
   }
}

static INLINE VMK_ReturnStatus 
Net_LinuxConnect(int s, struct sockaddr *name, int namelen)
{
   return Net_ConnectSocket(s, name, namelen, DEFAULT_STACK);
}


static INLINE VMK_ReturnStatus 
Net_LinuxCloseSocket(int s)
{
   return Net_CloseSocket(s, DEFAULT_STACK);
}



static INLINE VMK_ReturnStatus 
Net_LinuxCreateSocket(int type, 
                      int protocol, 
                      int *newSocket)
{
   return Net_CreateSocket(type, protocol, newSocket, DEFAULT_STACK);
}

static INLINE VMK_ReturnStatus 
Net_LinuxSendTo(int so, int flags,
                struct iovec  *iovec, int iovlen, 
                int len,
                int *copied)
{
   return NetLinuxSendTo(so, flags, iovec, iovlen, len, copied, DEFAULT_STACK);
}


static INLINE VMK_ReturnStatus 
Net_LinuxRecvFrom(int so, int flags, struct iovec *iovec, int iovlen,
                  int len, int *copied)
{
   
   return NetLinuxRecvFrom(so, flags, iovec, iovlen, len, copied, DEFAULT_STACK);
}


static INLINE VMK_ReturnStatus 
Net_LinuxSetSockOpt(int so, 
                    int level, int optname,
                    const void *optval, int optlen)
{
   
   return NetLinuxSetSockOpt(so, level, optname, optval, optlen, DEFAULT_STACK);
}


static INLINE VMK_ReturnStatus 
Net_LinuxGetSockOpt(int so, 
                    int level, int optname,
                    void *optval, int *optlen)
{
   
   return NetLinuxGetSockOpt(so, level, optname, optval, optlen, DEFAULT_STACK);
}

static INLINE void 
Net_SetStackFunctions(Net_StackFunctions *f)
{
   stackFunctions = *f;
}

typedef VMK_ReturnStatus (*Net_TcpipLoaderHook)(Net_TcpipStack *stack);
extern Net_TcpipLoaderHook TOELoaderCB;

static INLINE void Net_TcpipRegisterLoaderHook(Net_TcpipLoaderHook f)
{
   TOELoaderCB = f;
}

static INLINE void Net_TcpipUnRegisterLoaderHook(void)
{
   TOELoaderCB = NULL;

}

/*
 * Socket/TCP options exported by the vmkernel TCP/IP stack
 */
#define    VMK_IPPROTO_TCP               6

/*
 * vmkernel socket level code
 */
#define    VMK_SOL_SOCKET                0xffff

/*
 * vmkernel socketname codes
 */
#define    VMK_SO_SNDBUF                 0x1001
#define    VMK_SO_RCVBUF                 0x1002
#define    VMK_SO_LINGER                 0x0080

#endif // _SOCKET_DIST_H_
