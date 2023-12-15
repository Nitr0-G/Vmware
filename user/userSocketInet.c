/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * ESX additions are in the BSD stack's "sys/socket.h",
 * Make sure we get them.
 */
#define FIXED_FOR_ESX

/*
 * BSD headers
 */
#include "sys/types.h"
#include "netinet/in.h"
#include "netinet/tcp.h"
#include "sys/socket.h"
#include "sys/poll.h"
#include "sys/filio.h"

/*
 * _BSD_VISIBLE needs to be set in "sys/cdefs.h" so
 * the "sys/stat.h" included below is consistent with
 * the as-built BSD TCP stack.  Defining _KERNEL makes
 * this happen.
 */
#define _KERNEL
#include "sys/stat.h"

#define _SYS_INTTYPES_H_
#define __FreeBSD__

/*
 * These #define's from "sys/stat.h" collide with the field
 * definitions in the Linux API. Since stat() in the BSD TCP
 * stack doesn't fill them, they aren't necessary.
 */
#undef st_atime
#undef st_mtime
#undef st_ctime

#include "user_int.h"
#include "userSocket.h"
#include "linuxIoctl.h"
#include "net.h"
#include "memalloc.h" /* XXX get rid of double net copies on read/write. */
#include "vmk_net.h"
#include "userStat.h"

#define LOGLEVEL_MODULE UserSocketInet
#include "userLog.h"

static VMK_ReturnStatus UserSocketInetClose(UserObj* obj,
                                            User_CartelInfo* uci);
static VMK_ReturnStatus UserSocketInetRead(UserObj* obj,
					   UserVA userData,
					   uint64 offset,
					   uint32 userLength,
					   uint32 *bytesRead);
static VMK_ReturnStatus UserSocketInetWrite(UserObj* obj,
					    UserVAConst userData,
					    uint64 offset,
					    uint32 userLength,
					    uint32* bytesWritten);
static VMK_ReturnStatus UserSocketInetFcntl(UserObj *obj,
					    uint32 cmd,
					    uint32 arg);
static VMK_ReturnStatus UserSocketInetBind(UserObj* obj,
					   LinuxSocketName* name,
					   uint32 nameLen);
static VMK_ReturnStatus UserSocketInetConnect(UserObj* obj,
					      LinuxSocketName* name,
					      uint32 nameLen);
static VMK_ReturnStatus UserSocketInetListen(UserObj* obj,
					     int backlog);
static VMK_ReturnStatus UserSocketInetAccept(UserObj* obj,
					     UserObj** newObj,
                                             LinuxSocketName* name,
                                             uint32 *linuxNamelen);
static VMK_ReturnStatus UserSocketInetGetSocketName(UserObj* obj,
						    LinuxSocketName* name,
						    uint32* nameLen);
static VMK_ReturnStatus UserSocketInetSendmsg(UserObj* obj,
					      LinuxMsgHdr* msg,
					      uint32 len,
					      uint32* bytesSent);
static VMK_ReturnStatus UserSocketInetRecvmsg(UserObj* obj,
					      LinuxMsgHdr* msg,
					      uint32 len,
					      uint32* bytesRecv);
static VMK_ReturnStatus UserSocketInetSetsockopt(UserObj* obj,
						 int level,
						 int optName,
						 char* optVal,
						 int optLen);
static VMK_ReturnStatus UserSocketInetGetsockopt(UserObj* obj,
						 int level,
						 int optName,
						 char* optVal,
						 int* optLen);
static VMK_ReturnStatus UserSocketInetPoll(UserObj* obj,
                                           VMKPollEvent inEvents,
                                           VMKPollEvent* outEvents,
                                           UserObjPollAction action);
static VMK_ReturnStatus UserSocketInetGetPeerName(UserObj* obj,
						  LinuxSocketName* name,
						  uint32* nameLen);
static VMK_ReturnStatus UserSocketInetIoctl(UserObj* obj,
                                            uint32 linuxCmd,
                                            LinuxIoctlArgType type,
                                            uint32 size,
                                            void *userData,
                                            uint32 *result);
static VMK_ReturnStatus UserSocketInetStat(UserObj* obj,
                                           LinuxStat64 *statBuf);
static VMK_ReturnStatus UserSocketInetShutdown(UserObj* obj,
                                               int how);
static VMK_ReturnStatus UserSocketInetToString(UserObj *obj, char *string,
					       int length);

/*
 * UserObj callback methods for sockets.  Only encompasses the common
 * socket ops like read/write (not listen, bind, etc).
 */
UserObj_Methods socketInetMethods = USEROBJ_METHODS(
   (UserObj_OpenMethod) UserObj_NotADirectory,
   UserSocketInetClose,
   UserSocketInetRead,
   (UserObj_ReadMPNMethod) UserObj_BadParam,
   UserSocketInetWrite,
   (UserObj_WriteMPNMethod) UserObj_BadParam,
   UserSocketInetStat,
   (UserObj_ChmodMethod) UserObj_NotImplemented,   // not needed
   (UserObj_ChownMethod) UserObj_NotImplemented,   // not needed
   (UserObj_TruncateMethod) UserObj_NotImplemented,// not needed
   (UserObj_UtimeMethod) UserObj_NotImplemented,   // not needed
   (UserObj_StatFSMethod) UserObj_NotImplemented,  // not needed
   UserSocketInetPoll,
   (UserObj_UnlinkMethod) UserObj_NotADirectory,
   (UserObj_MkdirMethod) UserObj_NotADirectory,
   (UserObj_RmdirMethod) UserObj_NotADirectory,
   (UserObj_GetNameMethod) UserObj_NotADirectory,
   (UserObj_ReadSymLinkMethod) UserObj_NotADirectory,
   (UserObj_MakeSymLinkMethod) UserObj_NotADirectory,
   (UserObj_MakeHardLinkMethod) UserObj_NotADirectory,
   (UserObj_RenameMethod) UserObj_NotADirectory,
   (UserObj_MknodMethod) UserObj_NotADirectory,
   UserSocketInetFcntl,
   (UserObj_FsyncMethod) UserObj_BadParam,
   (UserObj_ReadDirMethod) UserObj_NotADirectory,
   UserSocketInetIoctl,
   UserSocketInetToString,
   UserSocketInetBind,
   UserSocketInetConnect,
   (UserObj_SocketpairMethod) UserObj_BadParam,
   UserSocketInetAccept,
   UserSocketInetGetSocketName,
   UserSocketInetListen,
   UserSocketInetSetsockopt,
   UserSocketInetGetsockopt,
   UserSocketInetSendmsg,
   UserSocketInetRecvmsg,
   UserSocketInetGetPeerName,
   UserSocketInetShutdown
);

// UserWorld equivalent of sockaddr_in
typedef struct {
   uint16 family;
   uint16 port;
   uint32 ipAddr;
} UserSocketInetName;

/*
 * Bounce buffer in UserSocketInetSendTo()
 */
#define BOUNCE_BUFFER_SIZE (8 * 1024)


/*
 *----------------------------------------------------------------------
 *
 * UserSocketInetNetStack --
 *
 *	Get the network stack to use for the given cartel.
 *
 * Results:
 *	A network stack
 *
 * Side-effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Net_StackFunctions*
UserSocketInetStack(UNUSED_PARAM(User_CartelInfo* uci))
{
   return DEFAULT_STACK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketInet_GetSocket --
 *
 *	Returns the bsd socket fd.
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
UserSocketInet_GetSocket(UserObj *obj, int *socket)
{
   if (obj->type != USEROBJ_TYPE_SOCKET_INET) {
      return VMK_BAD_PARAM;
   }

   if (obj->data.socketInetInfo == NULL) {
      return VMK_BAD_PARAM;
   }

   *socket = obj->data.socketInetInfo->socket;
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketInet_RelinquishOwnership --
 *
 *	Removes ownership of underlying bsd socket.
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
UserSocketInet_RelinquishOwnership(UserObj *obj)
{
   if (obj->type != USEROBJ_TYPE_SOCKET_INET) {
      return VMK_BAD_PARAM;
   }

   if (obj->data.socketInetInfo == NULL) {
      return VMK_BAD_PARAM;
   }

   obj->data.socketInetInfo->owner = FALSE; 
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketInet_ObjInit --
 *
 *      Initializes a new inet socket object.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
UserSocketInet_ObjInit(UserObj *obj, UserSocketInet_ObjInfo *socketInfo,
		       int socket)
{
   socketInfo->socket = socket;
   socketInfo->owner = TRUE;
   socketInfo->pollEvents = VMKPOLL_NONE;

   SP_InitLock("UserSocketInetPoll", &socketInfo->pollLock,
               UW_SP_RANK_POLLWAITERS);

   VMKPoll_InitList(&socketInfo->waiters, &socketInfo->pollLock);

   UserObj_InitObj(obj, USEROBJ_TYPE_SOCKET_INET, (UserObj_Data)socketInfo,
                   &socketInetMethods, USEROBJ_OPEN_RDWR);
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketInet_CloseSocket --
 *
 *	Simply calls Net_CloseSocket.
 *
 * Results:
 *	Whatever Net_CloseSocket returns.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
UserSocketInet_CloseSocket(User_CartelInfo *uci, int socket)
{
   return Net_CloseSocket(socket, UserSocketInetStack(uci));
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketInetObjDestroy --
 *
 *      Destroys the given inet socket object.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
UserSocketInetObjDestroy(User_CartelInfo *uci, UserObj *obj)
{
   ASSERT(obj != NULL);
   ASSERT(obj->data.socketInetInfo != NULL);

   SP_Lock(&obj->data.socketInetInfo->pollLock);
   if (VMKPoll_HasWaiters(&obj->data.socketInetInfo->waiters)) {
      UWWarn("waiters list not empty!");
   }
   VMKPoll_WakeupAndRemoveWaiters(&obj->data.socketInetInfo->waiters);
   SP_Unlock(&obj->data.socketInetInfo->pollLock);

   SP_CleanupLock(&obj->data.socketInetInfo->pollLock);
   memset(obj->data.socketInetInfo, 0, sizeof obj->data.socketInetInfo);
   User_HeapFree(uci, obj->data.socketInetInfo);
   memset(&obj->data, 0, sizeof obj->data);

   return;
}


/*
 *----------------------------------------------------------------------
 *                           
 * UserSocketInetLinuxToBSDName --
 *                           
 *      Convert a LinuxSocketInetName to a sockaddr_in_bsd.
 *                           
 * Results:
 *	VMK_OK if linuxName is the right size.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserSocketInetLinuxToBSDName(
   struct sockaddr_in_bsd* const bsdName,      // OUT
   const UserSocketInetName* const linuxName,  // IN
   int linuxNamelen)                           // IN 
{
   ASSERT(bsdName);
   ASSERT(linuxName);

   memset(bsdName, 0, sizeof(struct sockaddr_in_bsd));

   if (linuxNamelen < sizeof(UserSocketInetName)) { /* or use != ? */
      UWWarn("Mis-sized linuxname %db (expected %db)",
             linuxNamelen, sizeof(UserSocketInetName));
      return VMK_BAD_PARAM;
   }

   if (linuxName->family != LINUX_SOCKETFAMILY_INET) {
      UWWarn("Unexpected socket family %d", linuxName->family);
   }

   bsdName->sin_family      = linuxName->family;
   bsdName->sin_port        = linuxName->port;
   bsdName->sin_addr.s_addr = linuxName->ipAddr;
   bsdName->sin_len         = sizeof(struct sockaddr_in_bsd);

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketInetBSDToLinuxName --
 *
 *      Convert a sockaddr_in_bsd into a LinuxSocketInetName.
 * 
 * Results:
 *	VMK_OK if linux name is appropriate size.  *linuxNameSize is
 *	set to the actual size of the name returned.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserSocketInetBSDToLinuxName(
   const struct sockaddr_in_bsd* const bsdName, // IN
   UserSocketInetName* const linuxName,         // OUT
   int* linuxNameSize)                          // IN/OUT 
{
   ASSERT(linuxName != NULL);
   ASSERT(bsdName != NULL);

   if (*linuxNameSize < sizeof(UserSocketInetName)) {
      UWWarn("Passed in a buffer that is too small: %d vs %d", *linuxNameSize,
	     sizeof(UserSocketInetName));
      return VMK_BAD_PARAM;
   }

   if (bsdName->sin_family != AF_INET) {
      UWWarn("Unexpected BSD socket family %d", bsdName->sin_family);
   }

   memset(linuxName, 0, sizeof(UserSocketInetName));
   linuxName->family = bsdName->sin_family;
   linuxName->port   = bsdName->sin_port;
   linuxName->ipAddr = bsdName->sin_addr.s_addr;

   *linuxNameSize = sizeof(UserSocketInetName);
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *                           
 * UserSocketInetLinuxToBSDIoctl --
 *                           
 *      Map a Linux ioctl cmd to a BSD equivalent ioctl cmd.
 *                           
 * Results:
 *      VMK_OK if there is a mapping.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserSocketInetLinuxToBSDIoctl(
   uint32 linuxCmd,
   uint32 *bsdCmd)
{
   ASSERT(bsdCmd);

   switch (linuxCmd) {
   case LINUX_FIONREAD:
      *bsdCmd = FIONREAD;
      break;
   default:
      return VMK_BAD_PARAM;
   }

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketInet_Create --
 *
 *	Create a new socket object with the given type (i.e., stream or
 *	packet) and protocol (i.e., tcp or udp).
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
UserSocketInet_Create(User_CartelInfo* uci, LinuxSocketType type,
		      LinuxSocketProtocol protocol, UserObj** outObj)
{
   int bsdSocketFD = -1;
   VMK_ReturnStatus status;
   UserObj *obj;
   UserSocketInet_ObjInfo *socketInfo;

   /* Map Linux socket type into BSD socket type */
   switch(type) {
      case LINUX_SOCKETTYPE_STREAM:
	 type = SOCK_STREAM;
	 break;
      case LINUX_SOCKETTYPE_DATAGRAM:
	 type = SOCK_DGRAM;
	 break;
      case LINUX_SOCKETTYPE_RAW:
	 type = SOCK_RAW;
	 break;
      default:
	 UWWarn("Unknown linux socket type %#x", type);
	 return VMK_BAD_PARAM;
   }

   /* Map Linux socket protocol into BSD socket protocol */
   switch(protocol) {
      case 0:
	 /* 0 means let the stack pick the best. */
	 protocol = 0;
	 break;
      case LINUX_SOCKETPROTO_UDP:
	 protocol = IPPROTO_UDP;
	 break;
      case LINUX_SOCKETPROTO_TCP:
	 protocol = IPPROTO_TCP;
	 break;
      default:
	 UWWarn("Unknown linux socket protocol %#x", protocol);
	 return VMK_BAD_PARAM;
   }

   /*
    * Pre-allocate UserObj and socket info.
    */
   obj = User_HeapAlloc(uci, sizeof *obj);
   if (obj == NULL) {
      return VMK_NO_MEMORY;
   }
   socketInfo = User_HeapAlloc(uci, sizeof *socketInfo);
   if (socketInfo == NULL) {
      User_HeapFree(uci, obj);
      return VMK_NO_MEMORY;
   }

   status = Net_CreateSocket(type, protocol, &bsdSocketFD,
			     UserSocketInetStack(uci));
   if (status == VMK_OK) {
      UserSocketInet_ObjInit(obj, socketInfo, bsdSocketFD);
      *outObj = obj;
   } else {
      UWLOG(0, "Net_CreateSocket(type=%d, proto=%d, stack=%p) failed: %#x:%s",
            type, protocol, UserSocketInetStack(uci),
            status, VMK_ReturnStatusToString(status));
      User_HeapFree(uci, obj);
      User_HeapFree(uci, socketInfo);
   }
   
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketInetSendTo --
 *
 *	Send a message to a specific address, if provided, or to the other
 *	side of a connection.
 *
 * Results:
 *	VMK_OK on success, appropriate error code otherwise.
 *
 * Side-effects:
 *	User buffer has to be chopped and copied in before we call
 *      Net_SentTo.  Bummer.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserSocketInetSendTo(int bsdSocketFd,
                     struct sockaddr_in_bsd* bsdName, // may be NULL
                     UserVA /*void**/ userBuf,
                     uint32 userLen,
                     uint32 bsdFlags,
                     uint32* bytesSent)
{
   uint32 chunkSize, offset, sent, toCopy;
   User_CartelInfo* uci = MY_USER_CARTEL_INFO;
   VMK_ReturnStatus status = VMK_OK;
   uint8* localData;

   chunkSize = MIN(userLen, BOUNCE_BUFFER_SIZE);

   /*
    * XXX double copy of network data is bogus.
    */
   localData = User_HeapAlloc(uci, chunkSize);
   if (localData == NULL) {
      return VMK_NO_MEMORY;
   }

   for (offset = *bytesSent = 0; offset < userLen; offset += toCopy) {
      toCopy = MIN(chunkSize, userLen - offset);

      status = User_CopyIn(localData, userBuf + offset, toCopy);
      if (status == VMK_OK) {
         /* XXX blocking and signal behavior? */
         status = Net_SendTo(bsdSocketFd, bsdFlags, 
                             (struct sockaddr*)bsdName,
                             localData, toCopy, &sent,
                             UserSocketInetStack(uci));
      }
      if (status == VMK_OK) {
         *bytesSent += sent;
      } else {
         break;
      }
   }

   User_HeapFree(uci, localData);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketInetRecvFrom --
 *
 *	Receive a message from a specific address, if provided, or from the
 *	other side of a connection.
 *
 * Results:
 *	VMK_OK on success, appropriate error code otherwise.
 *
 * Side-effects:
 *	User buffer has to be copied out after we call Net_RecvFrom.  Bummer.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserSocketInetRecvFrom(int bsdSocketFd,                 // IN: bsd fd
                       struct sockaddr_in_bsd* bsdName, // IN: remote addr may be NULL
                       int nameLen,                     // IN: length of name buffer
		       UserVA /*void**/ userBuf,        // IN/OUT: buffer to write to
                       uint32 userLen,                  // IN: buf size
		       uint32 bsdFlags,                 // IN: bsd recvfrom flags
                       uint32* bytesRecv)               // OUT:
{
   User_CartelInfo* uci = MY_USER_CARTEL_INFO;
   VMK_ReturnStatus status;
   uint8* localData;
   
   ASSERT(bytesRecv != NULL);

   ASSERT(userLen < USERWORLD_HEAP_MAXALLOC_SIZE); /* XXX support large recvs */

   /*
    * XXX double copy of network data is bogus.
    * XXX handle userLength > PAGE_SIZE better
    */
   localData = User_HeapAlloc(uci, userLen);
   if (localData == NULL) {
      return VMK_NO_MEMORY;
   }

   /*
    * XXX Blocks in support.c:tsleep, will change to be non-blocking
    * in tcp stack.
    */
   status = Net_RecvFrom(bsdSocketFd, bsdFlags, localData, userLen,
                         (struct sockaddr*)bsdName,
                         &nameLen,
                         bytesRecv,
                         UserSocketInetStack(uci));
   if (status == VMK_OK && (*bytesRecv > 0)) {
      ASSERT((bsdName == NULL) || (nameLen >= bsdName->sin_len));
      status = User_CopyOut(userBuf, localData, *bytesRecv);
   }

   User_HeapFree(uci, localData);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketInetClose --
 *
 *	Close the given socket object.
 *
 * Results:
 *	See Net_CloseSocket.
 *
 * Side-effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserSocketInetClose(UserObj* obj, User_CartelInfo* uci)
{
   VMK_ReturnStatus status = VMK_OK;

   ASSERT(obj != NULL);
   if (obj->data.socketInetInfo->owner) {
      status = UserSocketInet_CloseSocket(uci,
					  obj->data.socketInetInfo->socket);
   }
   UserSocketInetObjDestroy(uci, obj);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketInetRead --
 *
 *	Read up to userLength bytes from the given obj.  offset is
 *	ignored.
 *
 * Results:
 *	VMK_OK if bytes are read successfully, *bytesRead set to
 *	number of bytes actually read off the socket.
 *
 * Side-effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
UserSocketInetRead(UserObj* obj,
                   UserVA userData,
                   UNUSED_PARAM(uint64 offset),
                   uint32 userLength,
                   uint32 *bytesRead)
{
   static const int bsdFlags = 0; /* XXX */
   static struct sockaddr_in_bsd* recvAddr = NULL;
   static const int recvAddrLen = 0;

   return UserSocketInetRecvFrom(obj->data.socketInetInfo->socket,
				 recvAddr, recvAddrLen,
				 userData,
				 userLength,
				 bsdFlags,
				 bytesRead);
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketInetWrite --
 *
 *	Write the given userLength bytes of userData to the given
 *	socket.  offset is ignored.
 *
 * Results:
 *	VMK_OK if write succeeded, otherwise if an error occurred.
 *
 * Side-effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserSocketInetWrite(UserObj* obj,
                    UserVAConst userData,
                    UNUSED_PARAM(uint64 offset),
                    uint32 userLength,
                    uint32* bytesWritten)
{
   return UserSocketInetSendTo(obj->data.socketInetInfo->socket,
			       NULL,    // sockaddr_in_bsd
			       userData,
			       userLength,
			       0,       // flags
			       bytesWritten);
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketInetFcntl --
 *
 *	Does nothing.
 *
 * Results:
 *	VMK_OK.
 *
 * Side-effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserSocketInetFcntl(UNUSED_PARAM(UserObj *obj),
		    uint32 cmd,
		    uint32 arg)
{
   if (cmd != LINUX_FCNTL_CMD_SETFL) {
      UWWarn("cmd %d not supported", cmd);
      return VMK_NOT_SUPPORTED;
   }

   /*
    * Since we support all flags within USEROBJ_FCNTL_SETFL_VMK_SUPPORTED
    * without having to do anything, just return VMK_OK.
    */
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketInetBind --
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
UserSocketInetBind(UserObj* obj, LinuxSocketName* name, uint32 linuxNamelen)
{
   User_CartelInfo* uci = MY_USER_CARTEL_INFO;
   struct sockaddr_in_bsd bsdName;
   VMK_ReturnStatus status;

   ASSERT(name != NULL);

   memset(&bsdName, 0, sizeof bsdName);
   bsdName.sin_len = sizeof bsdName;

   status = UserSocketInetLinuxToBSDName(&bsdName,
                                         (UserSocketInetName*)name,
                                         linuxNamelen);

   if (status == VMK_OK) {
      UWLOG(1, "(name={fam=%#x, port=%#x, addr=%#x}",
            bsdName.sin_family, ntohs(bsdName.sin_port),
            ntohl(bsdName.sin_addr.s_addr));

      status = Net_Bind(obj->data.socketInetInfo->socket, &bsdName,
                        bsdName.sin_len, UserSocketInetStack(uci));
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketInetConnect --
 *
 *	Connect the given socket to the given name.
 *
 * Results:
 *	VMK_OK if okay, otherwise on error.
 *
 * Side-effects:
 * 	Socket is connected.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
UserSocketInetConnect(UserObj* obj, LinuxSocketName* name, uint32 linuxNamelen)
{
   User_CartelInfo* uci = MY_USER_CARTEL_INFO;
   struct sockaddr_in_bsd bsdName;
   VMK_ReturnStatus status;

   ASSERT(name != NULL);

   memset(&bsdName, 0, sizeof bsdName);
   bsdName.sin_len = sizeof bsdName;

   status = UserSocketInetLinuxToBSDName(&bsdName,
                                         (UserSocketInetName*)name,
                                         linuxNamelen);
   if (status == VMK_OK) {
      UWLOG(1, "(name={fam=%#x, port=%#x, addr=%#x}, namelen=%u)",
            bsdName.sin_family, ntohs(bsdName.sin_port),
            ntohl(bsdName.sin_addr.s_addr), linuxNamelen);

      status = Net_ConnectSocket(obj->data.socketInetInfo->socket,
                                 (struct sockaddr*)&bsdName,
                                 bsdName.sin_len, UserSocketInetStack(uci));
   }
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketInetListen --
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
UserSocketInetListen(UserObj* obj, int backlog)
{
   User_CartelInfo* uci = MY_USER_CARTEL_INFO;
   VMK_ReturnStatus status;

   // XXX blocking / signals?
   status = Net_Listen(obj->data.socketInetInfo->socket,
                       backlog, UserSocketInetStack(uci));

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketInetAccept --
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
static VMK_ReturnStatus 
UserSocketInetAccept(UserObj* obj, UserObj** acceptedSockObj,
                     LinuxSocketName* linuxName, uint32* linuxNamelen)
{
   int netFd;
   uint32 bsdNameLen;
   sockaddr_in_bsd bsdName;
   VMK_ReturnStatus status;
   Bool canBlock = UserObj_IsOpenForBlocking(obj);
   User_CartelInfo* uci = MY_USER_CARTEL_INFO;
   UserObj *newObj;
   UserSocketInet_ObjInfo *newSocket;

   *acceptedSockObj = NULL;

   newObj = User_HeapAlloc(uci, sizeof *newObj);
   if (newObj == NULL) {
      UWLOG(0, "Failed to allocate new UserObj");
      return VMK_NO_MEMORY;
   }

   newSocket = User_HeapAlloc(uci, sizeof *newSocket);
   if (newSocket == NULL) {
      UWLOG(0, "Failed to allocate new UserSocketInet_ObjInfo");
      User_HeapFree(uci, newObj);
      return VMK_NO_MEMORY;
   }

   memset(&bsdName, 0, sizeof bsdName);
   bsdName.sin_len = sizeof bsdName;

   UWLOG(2, "obj=%p, so=%d, %s", obj, obj->data.socketInetInfo->socket,
         canBlock ? "blocking" : "non-blocking");

   status = Net_Accept(obj->data.socketInetInfo->socket,
                       canBlock,
                       (struct sockaddr*)&bsdName,
                       &bsdNameLen,
                       &netFd,
                       UserSocketInetStack(uci));

   if (status == VMK_OK) {
      if (linuxName != NULL) {
         ASSERT(bsdNameLen == bsdName.sin_len);
         status = UserSocketInetBSDToLinuxName(&bsdName,
                                               (UserSocketInetName *)linuxName,
                                               linuxNamelen);
      }

      if (status == VMK_OK) {
         UserSocketInet_ObjInit(newObj, newSocket, netFd);
         *acceptedSockObj = newObj;
      }
   }

   if (status != VMK_OK) {
      UWLOG(0, "accept failed: %s", UWLOG_ReturnStatusToString(status));
      User_HeapFree(uci, newObj);
      User_HeapFree(uci, newSocket);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketInetGetSocketName --
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
UserSocketInetGetSocketName(UserObj* obj, LinuxSocketName* name,
			    uint32* linuxNamelen)
{
   User_CartelInfo* uci = MY_USER_CARTEL_INFO;
   VMK_ReturnStatus status;
   sockaddr_in_bsd bsdName;
   uint32 bsdNamelen;

   memset(&bsdName, 0, sizeof bsdName);
   bsdName.sin_len = sizeof bsdName;
   bsdNamelen = sizeof bsdName;

   status = Net_GetSockName(obj->data.socketInetInfo->socket,
                            (struct sockaddr*)&bsdName,
                            &bsdNamelen, UserSocketInetStack(uci));

   ASSERT(bsdNamelen == bsdName.sin_len);
   if (status == VMK_OK) {
      status = UserSocketInetBSDToLinuxName(&bsdName,
                                            (UserSocketInetName*)name,
                                            linuxNamelen);
   }
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketInetSendmsg --
 *
 *	Sends a message on the given socket.
 *
 * Results:
 *	VMK_OK if okay, otherwise on error.
 *
 * Side-effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
UserSocketInetSendmsg(UserObj* obj, LinuxMsgHdr* msg, uint32 len,
		      uint32* bytesSent)
{
   sockaddr_in_bsd bsdName;
   sockaddr_in_bsd *namep = NULL;
   
   uint32 flags = 0;

   ASSERT(msg != NULL);

   /*
    * Convert local socket name for outbound message, if its given.
    */
   if ((msg->name != NULL)
       && (msg->nameLen > 0)) {
      VMK_ReturnStatus status;

      status = UserSocketInetLinuxToBSDName(&bsdName,
                                            (UserSocketInetName*)msg->name,
                                            msg->nameLen);
      if (status != VMK_OK) {
         return status;
      }

      namep = &bsdName;
   }

   // XXX need to convert flags

   ASSERT(msg->iovLen == 1); /* XXX arbitrary limit */

   return UserSocketInetSendTo(obj->data.socketInetInfo->socket, namep,
                               msg->iov->base, len, flags, bytesSent);
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketInetRecvmsg --
 *
 *	Receives a message on the given socket.
 *
 * Results:
 *	VMK_OK if okay, otherwise on error.
 *
 * Side-effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
UserSocketInetRecvmsg(UserObj* obj, LinuxMsgHdr* msg, uint32 len,
		      uint32* bytesRecv)
{
   sockaddr_in_bsd bsdName;
   VMK_ReturnStatus status;
   const Bool wantName = (msg->nameLen > 0);
   sockaddr_in_bsd* namep;
   int namelen;
   uint32 flags;
   

   ASSERT(obj);
   ASSERT(obj->type == USEROBJ_TYPE_SOCKET_INET);

   // XXX need to convert flags
   flags = msg->flags;

   /*
    * Setup namep and namelen to fetch the remote name if caller wants
    * it.
    */
   if (wantName) {
      if (msg->nameLen < sizeof (UserSocketInetName)) {
         UWLOG(0, "Mis-sized linuxname %db (expected at least %db)",
               msg->nameLen, sizeof(UserSocketInetName));
         return VMK_BAD_PARAM;
      }

      namep = &bsdName;
      namelen = sizeof bsdName;

      // Clean bsdName before retreving the remote name.
      memset(namep, 0, sizeof bsdName);
      bsdName.sin_len = namelen;
   } else {
      namep = NULL;
      namelen = 0;
   }

   status = UserSocketInetRecvFrom(obj->data.socketInetInfo->socket,
                                   namep, namelen,
                                   msg->iov->base,
				   len, flags, bytesRecv);

   // XXX need to return updated flags

   if (status == VMK_OK) {
      msg->iovLen = 1;
      msg->iov->length = *bytesRecv;
      if (wantName) {
         ASSERT(msg->name != NULL);
         /* UserSocketInetBSDToLinuxName cannot fail, already checked msg. */
         (void) UserSocketInetBSDToLinuxName(namep,
                                      (UserSocketInetName*)msg->name,
                                      &msg->nameLen);
      }
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketInetSetsockopt --
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
static VMK_ReturnStatus 
UserSocketInetSetsockopt(UserObj* obj,
                         int level,
                         int optName,
                         char* optVal,
			 int optLen)
{
   User_CartelInfo* uci = MY_USER_CARTEL_INFO;
   int bsdLevel;
   int bsdOptName;

   ASSERT(obj);
   ASSERT(obj->type == USEROBJ_TYPE_SOCKET_INET);

   /*
    * We only support setsockopt at the socket level (not tcp or udp);
    */
   if (level != LINUX_SOCKET_SOL_SOCKET) {
      UWWarn("Unsupported socket option level %d", level);
      return VMK_NOT_SUPPORTED;
   }
   bsdLevel = SOL_SOCKET;
   
   /*
    * We only support a few options.
    */
   switch(optName) {
   case LINUX_SOCKET_SO_REUSEADDR:
      bsdOptName = SO_REUSEADDR;
      break;
   case LINUX_SOCKET_SO_ERROR:
      bsdOptName = SO_ERROR;
      break;
   case LINUX_SOCKET_SO_SNDBUF:
      bsdOptName = SO_SNDBUF;
      break;
   case LINUX_SOCKET_SO_RCVBUF:
      bsdOptName = SO_RCVBUF;
      break;
   case LINUX_SOCKET_SO_KEEPALIVE:
      bsdOptName = SO_KEEPALIVE;
      break;
   case LINUX_SOCKET_SO_LINGER:
      bsdOptName = SO_LINGER;
      break;
   default:
      UWWarn("Unsupported SOL_SOCKET sockopt optName=%d", optName);
      return VMK_NOT_SUPPORTED;
      break;
   }

   return Net_SetSockOpt(obj->data.socketInetInfo->socket, bsdLevel,
                         bsdOptName, optVal, optLen,
			 UserSocketInetStack(uci));
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketInetGetsockopt --
 *
 *	Get the given socket option to the given value.
 *
 * Results:
 *	VMK_OK if okay, otherwise on error.
 *
 * Side-effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
UserSocketInetGetsockopt(UserObj* obj, int level, int optName, char* optVal,
			 int* optLen)
{
   int bsdLevel;
   int bsdOptName;
   VMK_ReturnStatus status;
   User_CartelInfo* uci = MY_USER_CARTEL_INFO;

   ASSERT(obj);
   ASSERT(obj->type == USEROBJ_TYPE_SOCKET_INET);

   /*
    * We only support getsockopt at the socket level (not tcp or udp);
    */
   if (level != LINUX_SOCKET_SOL_SOCKET) {
      UWWarn("Unsupported socket option level %d", level);
      return VMK_NOT_SUPPORTED;
   }
   bsdLevel = SOL_SOCKET;
   
   switch(optName) {
   case LINUX_SOCKET_SO_REUSEADDR:
      bsdOptName = SO_REUSEADDR;
      break;
   case LINUX_SOCKET_SO_ERROR:
      bsdOptName = SO_ERROR;
      break;
   case LINUX_SOCKET_SO_SNDBUF:
      bsdOptName = SO_SNDBUF;
      break;
   case LINUX_SOCKET_SO_RCVBUF:
      bsdOptName = SO_RCVBUF;
      break;
   case LINUX_SOCKET_SO_KEEPALIVE:
      bsdOptName = SO_KEEPALIVE;
      break;
   case LINUX_SOCKET_SO_LINGER:
      bsdOptName = SO_LINGER;
      break;
   default:
      UWWarn("Unsupported SOL_SOCKET sockopt optName=%d", optName);
      return VMK_NOT_SUPPORTED;
      break;
   }

   status = Net_GetSockOpt(obj->data.socketInetInfo->socket, bsdLevel,
                           bsdOptName, optVal, optLen,
                           UserSocketInetStack(uci));

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketInetPollLock --
 *
 *      Locks the given obj's inet socket poll lock.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static INLINE void
UserSocketInetPollLock(UserObj *obj)
{
   ASSERT(obj != NULL);
   ASSERT(obj->type == USEROBJ_TYPE_SOCKET_INET);
   ASSERT(obj->data.socketInetInfo != NULL);
   SP_Lock(&obj->data.socketInetInfo->pollLock);
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketInetPollUnlock --
 *
 *      Unlocks the given obj's inet socket poll lock.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static INLINE void
UserSocketInetPollUnlock(UserObj *obj)
{
   ASSERT(obj != NULL);
   ASSERT(obj->type == USEROBJ_TYPE_SOCKET_INET);
   ASSERT(obj->data.socketInetInfo != NULL);
   SP_Unlock(&obj->data.socketInetInfo->pollLock);
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketInetPollCheck --
 *
 *      Check Net_PollSocket() for events.
 *
 * Results:
 *      Return code from Net_PollSocket().
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserSocketInetPollCheck(int sock, VMKPollEvent inEvents,
                        VMKPollEvent *outEvents)
{
   int rc;
   VMK_ReturnStatus status = VMK_OK;
   User_CartelInfo* uci = MY_USER_CARTEL_INFO;

   rc = Net_PollSocket(sock, inEvents, UserSocketInetStack(uci));

   if (rc == -1) {
      *outEvents = 0;
      UWWarn("Poll returned -1, not sure what exactly went wrong ... ");
      status = VMK_BAD_PARAM;
   } else {
      UWLOG(1, "Net_PollSocket() socket=%d rc=%d", sock, rc);
      ASSERT((rc & ~(POLLIN|POLLOUT|POLLPRI|POLLERR|POLLHUP|POLLNVAL)) == 0);
      *outEvents = rc;
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketInetFindObj --
 *
 *      Finds a UserObj corresponding to a given socket.
 *
 * Results:
 *      VMK_ReturnStatus.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserSocketInetObjFind(User_CartelInfo* uci, int sock, UserObj** outObj)
{
   VMK_ReturnStatus status = VMK_INVALID_HANDLE;
   int fd;

   UserObj_FDLock(&uci->fdState);
   for (fd = 0; fd < USEROBJ_MAX_HANDLES; fd++) {
      if (uci->fdState.descriptors[fd] != NULL &&
          uci->fdState.descriptors[fd] != USEROBJ_RESERVED_HANDLE &&
          uci->fdState.descriptors[fd]->type == USEROBJ_TYPE_SOCKET_INET &&
          uci->fdState.descriptors[fd]->data.socketInetInfo != NULL &&
          uci->fdState.descriptors[fd]->data.socketInetInfo->socket == sock) {
         *outObj = uci->fdState.descriptors[fd];
         UserObj_Acquire(*outObj);
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
 * UserSocketInetPollCallback --
 *
 *      Poll callback function.
 *
 * Results:
 *      nothing
 *
 * Side-effects:
 *      Records socket associated with the callback.
 *
 * Notes:
 *      Once registered in UserSocketInetPoll(), this callback is
 *      invoked when there is any activity on the socket. The code
 *      uses the socket argument to find the corresponding UserObj,
 *      if it still exists, and wakes up worlds on the waiter list.
 *
 *----------------------------------------------------------------------
 */
static void
UserSocketInetPollCallback(int sock, void *worldArg, UNUSED_PARAM(int unused))
{
   UserObj *obj;
   World_Handle *world;
   User_CartelInfo* uci;
   World_ID world_ID = *(World_ID *)worldArg;

   UWSTAT_INC(userSocketInetPollCallback);

   world = World_Find(world_ID);
   if (world == NULL) {
      return;
   }

   if (!World_IsUSERWorld(world)) {
      World_Release(world);
      return;
   }

   uci = world->userCartelInfo;
   ASSERT(uci != NULL);

   if (UserSocketInetObjFind(uci, sock, &obj) != VMK_OK) {
      World_Release(world);
      return;
   }

   ASSERT(obj != NULL);
   ASSERT(obj->type == USEROBJ_TYPE_SOCKET_INET);

   UserSocketInetPollLock(obj);
   VMKPoll_WakeupAndRemoveWaitersForEvent(&obj->data.socketInetInfo->waiters,
                                          obj->data.socketInetInfo->pollEvents);
   UserSocketInetPollUnlock(obj);

   (void)UserObj_Release(uci, obj);
   World_Release(world);

   return;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketInetPoll --
 *
 *      Poll-related 'action' on the given socket.
 *
 * Results:
 *      VMK_OK if the socket is ready for the given ops, or
 *      VMK_WOULD_BLOCK if not.
 *
 * Side-effects:
 *      The object may be added to internal VMKPoll lists.
 *
 * Notes:
 *      The vmkernel TCP stack acquires the unranked iplLock
 *      (see PR #22937) during callback registration.  When
 *      the callback is invoked, the iplLock is already held
 *      by the TCP stack.  To protect the VMKPoll list, the
 *      callback acquires the socket's pollLock when waking
 *      up the poll waiters.
 *
 *      Consequently, the pollLock can't be taken and held for
 *      the duration of a UserObjPollNoAction or UserObjPollNotify
 *      action or deadlock will occur.  Instead, the socket's
 *      pollLock is acquired to determine if a callback must
 *      be registered and then the lock is dropped. Once the
 *      callback is registered, the socket is rechecked for any
 *      events that may have occured between the time since it
 *      was last checked and the registration completed.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserSocketInetPoll(UserObj* obj, VMKPollEvent inEvents,
                   VMKPollEvent* outEvents, UserObjPollAction action)
{
   int hasWaiters;
   VMK_ReturnStatus status = VMK_OK;
   User_CartelInfo* uci = MY_USER_CARTEL_INFO;

   ASSERT(obj);
   ASSERT(obj->type == USEROBJ_TYPE_SOCKET_INET);

   /* Thanks to the Linux and BSD folks for cribbing from the same source. */
   ASSERT(LINUX_POLLFLAG_IN  == POLLIN);
   ASSERT(LINUX_POLLFLAG_PRI == POLLPRI);
   ASSERT(LINUX_POLLFLAG_OUT == POLLOUT);
   ASSERT(LINUX_POLLFLAG_ERR == POLLERR);
   ASSERT(LINUX_POLLFLAG_HUP == POLLHUP);
   ASSERT(LINUX_POLLFLAG_NVAL== POLLNVAL);
   
   ASSERT((inEvents & ~(POLLIN|POLLOUT|POLLPRI|POLLERR|POLLHUP|POLLNVAL)) == 0);

   UWLOG(3, "inEvents=%#x, action=%s socket=%d", inEvents,
         UserObj_PollActionToString(action),
         obj->data.socketInetInfo->socket);

   if (action == UserObjPollCleanup) {
      /* remove this world from the waiter list */
      UserSocketInetPollLock(obj);
      VMKPoll_RemoveWaiter(&obj->data.socketInetInfo->waiters,
                           MY_RUNNING_WORLD->worldID);
      hasWaiters = VMKPoll_HasWaiters(&obj->data.socketInetInfo->waiters);
      obj->data.socketInetInfo->pollEvents = VMKPOLL_NONE;
      UserSocketInetPollUnlock(obj);

      if (!hasWaiters) {
         /* no waiters remaining, unregister the callback */
         status = Net_RegisterCallback(obj->data.socketInetInfo->socket,
                                       NULL, 0, UserSocketInetStack(uci));
      }

   } else if ((action == UserObjPollNoAction) ||
              (action == UserObjPollNotify)) {

      /* check for existing waiters */
      UserSocketInetPollLock(obj);
      hasWaiters = VMKPoll_HasWaiters(&obj->data.socketInetInfo->waiters);
      UserSocketInetPollUnlock(obj);
      if (!hasWaiters) {
         obj->data.socketInetInfo->pollEvents = inEvents;
      } else {
         /* all inEvents for any waiter on the socket are the same */
         ASSERT(obj->data.socketInetInfo->pollEvents != VMKPOLL_NONE);
         ASSERT(obj->data.socketInetInfo->pollEvents == inEvents);
      }

      if (!hasWaiters) {
         /* currently no waiters, register the callback */
         status = Net_RegisterCallback(obj->data.socketInetInfo->socket,
                                       UserSocketInetPollCallback,
                                       &MY_RUNNING_WORLD->worldID,
                                       UserSocketInetStack(uci));
         if (status != VMK_OK) {
            UWWarn("Net_RegisterCallback failed");
            return status;
         }
      }

      /* check for events on the socket */
      status = UserSocketInetPollCheck(obj->data.socketInetInfo->socket,
                                       inEvents, outEvents);

      /* check returned successfully with no outEvents */
      if ((status == VMK_OK) && (*outEvents == 0)) {

         /* add waiter to list */
         UserSocketInetPollLock(obj);
         VMKPoll_AddWaiterForEvent(&obj->data.socketInetInfo->waiters,
                                   MY_RUNNING_WORLD->worldID, inEvents);
         UserSocketInetPollUnlock(obj);

         /* re-check for events occuring before VMKPoll_AddWaiterForEvent() */
         status = UserSocketInetPollCheck(obj->data.socketInetInfo->socket,
                                          inEvents, outEvents);

         if ((status == VMK_OK) && (*outEvents == 0)) {
            status = VMK_WOULD_BLOCK;
         }
      }

      /* either we have an event or we would block */
      ASSERT((*outEvents != 0) || (status != VMK_OK));

   } else {
      ASSERT(FALSE);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketInetGetPeerName --
 *
 *      Get the name of the connected peer.
 *
 * Results:
 *      name is set to the name of the given peer.  VMK_OK if okay,
 *      otherwise if error.
 *
 * Side-effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
UserSocketInetGetPeerName(UserObj* obj, LinuxSocketName* name,
                          uint32* linuxNamelen)
{
   User_CartelInfo* uci = MY_USER_CARTEL_INFO;
   VMK_ReturnStatus status;
   sockaddr_in_bsd bsdName;
   uint32 bsdNamelen;

   memset(&bsdName, 0, sizeof bsdName);
   bsdName.sin_len = sizeof bsdName;
   bsdNamelen = sizeof bsdName;

   status = Net_GetPeerName(obj->data.socketInetInfo->socket,
                            (struct sockaddr*)&bsdName,
                            &bsdNamelen, UserSocketInetStack(uci));

   ASSERT(bsdNamelen == bsdName.sin_len);
   if (status == VMK_OK) {
      status = UserSocketInetBSDToLinuxName(&bsdName,
                                            (UserSocketInetName*)name,
                                            linuxNamelen);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketInetIoctl --
 *
 *      ioctl for the inet socket.
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
UserSocketInetIoctl(UserObj* obj, uint32 linuxCmd,
                    LinuxIoctlArgType type, uint32 dataSize,
                    void *userData, uint32 *result)
{
   uint32 bsdCmd;
   uint8 *localData;
   VMK_ReturnStatus status;
   User_CartelInfo* uci = MY_USER_CARTEL_INFO;

   UWLOG(1, "(cmd=%#x type=%#x size=%#x userData=%p)",
         linuxCmd, type, dataSize, userData);

   status = UserSocketInetLinuxToBSDIoctl(linuxCmd, &bsdCmd);
   if (status != VMK_OK) {
      return status;
   }

   ASSERT(dataSize < USERWORLD_HEAP_MAXALLOC_SIZE);

   localData = User_HeapAlloc(uci, dataSize);
   if (localData == NULL) {
      return VMK_NO_MEMORY;
   }

   switch (type) {
      case LINUX_IOCTL_ARG_CONST:
         memcpy(localData, &userData, dataSize);
         break;
      case LINUX_IOCTL_ARG_PTR:
         status = User_CopyIn(localData, (UserVA)userData, dataSize);
         break;
      default:
         NOT_IMPLEMENTED();
   }

   if (status == VMK_OK) {
      status = Net_SocketIoctl(obj->data.socketInetInfo->socket, bsdCmd,
                               localData, UserSocketInetStack(uci));
   }

   if (status == VMK_OK) {
       switch (type) {
          case LINUX_IOCTL_ARG_CONST:
             memcpy(&userData, localData, dataSize);
             break;
          case LINUX_IOCTL_ARG_PTR:
             status = User_CopyOut((UserVA)userData, localData, dataSize);
             break;
         default:
            NOT_IMPLEMENTED();
       }
   }

   if (status == VMK_OK) {
      *result = 0;
   } else {
      *result = -1;
   }

   User_HeapFree(uci, localData);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketInetStat --
 *
 *      Get stats for the inet socket.
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
UserSocketInetStat(UserObj* obj, LinuxStat64 *statBuf)
{
   struct stat bsdStatBuf;
   VMK_ReturnStatus status = VMK_OK;
   User_CartelInfo* uci = MY_USER_CARTEL_INFO;

   UWLOG(1, "socket=%d (buf=%p)", obj->data.socketInetInfo->socket, statBuf);

   status = Net_SocketStat(obj->data.socketInetInfo->socket,
                           &bsdStatBuf, UserSocketInetStack(uci));

   if (status == VMK_OK) {
      memset(statBuf, 0x0, sizeof *statBuf);

#define USERSOCKETINETSTAT_COPYFIELD(FIELD)               \
      do {                                                \
              statBuf->FIELD = bsdStatBuf.FIELD;          \
      } while(0)            

      /*
       * The vmkernel TCP stack doesn't fill in
       * st_atime, st_mtime, st_ctime or st_ino.
       */
      USERSOCKETINETSTAT_COPYFIELD(st_dev);
      USERSOCKETINETSTAT_COPYFIELD(st_mode);
      USERSOCKETINETSTAT_COPYFIELD(st_nlink);
      USERSOCKETINETSTAT_COPYFIELD(st_uid);
      USERSOCKETINETSTAT_COPYFIELD(st_gid);
      USERSOCKETINETSTAT_COPYFIELD(st_rdev);
      USERSOCKETINETSTAT_COPYFIELD(st_size);
      USERSOCKETINETSTAT_COPYFIELD(st_blksize);
      USERSOCKETINETSTAT_COPYFIELD(st_blocks);

#undef USERSOCKETINETSTAT_COPYFIELD
   }
   
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketInetShutdown --
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
UserSocketInetShutdown(UserObj* obj, int how)
{
   User_CartelInfo* uci = MY_USER_CARTEL_INFO;

   UWLOG(1, "socket=%d (how=%d)", obj->data.socketInetInfo->socket, how);

   return Net_ShutdownSocket(obj->data.socketInetInfo->socket,
                             how, UserSocketInetStack(uci));
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketInetToString --
 *
 *	Return a string representation of this object.
 *
 * Results:
 *	VMK_OK.
 *
 * Side-effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserSocketInetToString(UserObj *obj, char *string, int length)
{
   snprintf(string, length, "sckt: %d, %s", obj->data.socketInetInfo->socket,
	    obj->data.socketInetInfo->owner ? "Owner" : "NotOwner");
   return VMK_OK;
}
