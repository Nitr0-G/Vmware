/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * userSocketUnix.c -
 *
 *   Implementation of unix-domain sockets for UserWorlds.  There are actually
 *   three separate unix-domain socket types.  The first is just a generic
 *   socket.  It's what's created when socket(PF_VMK, SOCK_STREAM, 0) is called.
 *   The only functions you can call on it are bind and connect.  If you call
 *   bind, a new unix-domain socket type is created: a server socket.  With a
 *   server socket, you can call listen and accept.  If you call connect on a
 *   generic socket, it will create a data socket.  This data socket can call
 *   read/write and recvmsg/sendmsg to pass file descriptors.  Data sockets
 *   simply use two one-way pipes for data transfer.
 *
 *   One interesting aspect of this process is that we have to create a new
 *   object of a different type and replace the original object in the file
 *   descriptor table.  We do this rather than just replace the type and method
 *   fields of the original object because we run into problems with threads
 *   that are accessing that data.  So, we just remove the old object from the
 *   file descriptor table, dec'ing its refcount, and letting it get cleaned up
 *   appropriately.
 *
 */

#include "user_int.h"
#include "userSocket.h"
#include "userPipe.h"
#include "userObj.h"
#include "vmkpoll.h"

#define LOGLEVEL_MODULE UserSocketUnix
#include "userLog.h"

static VMK_ReturnStatus UserSocketUnixClose(UserObj* obj,
                                            User_CartelInfo* uci);
static VMK_ReturnStatus UserSocketUnixStat(UserObj *obj,
                                           LinuxStat64 *statBuf);
static VMK_ReturnStatus UserSocketUnixRead(UserObj* obj,
					   UserVA userData,
					   uint64 offset,
					   uint32 userLength,
					   uint32 *bytesRead);
static VMK_ReturnStatus UserSocketUnixWrite(UserObj* obj,
					    UserVAConst userData,
					    uint64 offset,
					    uint32 userLength,
					    uint32* bytesWritten);
static VMK_ReturnStatus UserSocketUnixFcntl(UserObj *obj,
					    uint32 cmd,
					    uint32 arg);
static VMK_ReturnStatus UserSocketUnixBind(UserObj* obj,
					   LinuxSocketName* name,
					   uint32 nameLen);
static VMK_ReturnStatus UserSocketUnixConnect(UserObj* obj,
					      LinuxSocketName* name,
					      uint32 nameLen);
static VMK_ReturnStatus UserSocketUnixListen(UserObj* obj,
					     int backlog);
static VMK_ReturnStatus UserSocketUnixAccept(UserObj* obj,
					     UserObj** newObj,
                                             LinuxSocketName* name,
                                             uint32* nameLen);
static VMK_ReturnStatus UserSocketUnixGetSocketName(UserObj* obj,
                                             LinuxSocketName* name,
                                             uint32* nameLen);
static VMK_ReturnStatus UserSocketUnixSendmsg(UserObj* obj,
					      LinuxMsgHdr* msg,
					      uint32 len,
					      uint32* bytesSent);
static VMK_ReturnStatus UserSocketUnixRecvmsg(UserObj* obj,
					      LinuxMsgHdr* msg,
					      uint32 len,
					      uint32* bytesRecv);
static VMK_ReturnStatus UserSocketUnixPollSocket(UserObj* obj,
                                                 VMKPollEvent inEvents,
                                                 VMKPollEvent* outEvents,
                                                 UserObjPollAction action);
static VMK_ReturnStatus UserSocketUnixPollDataSocket(UserObj *obj,
						     VMKPollEvent inEvents,
						     VMKPollEvent* outEvents,
						     UserObjPollAction action);
static VMK_ReturnStatus UserSocketUnixPollServerSocket(UserObj *obj,
						       VMKPollEvent inEvents,
						       VMKPollEvent *outEvents,
						       UserObjPollAction action);
static VMK_ReturnStatus UserSocketUnixGetPeerName(UserObj* obj,
						  LinuxSocketName* name,
						  uint32* nameLen);
static VMK_ReturnStatus UserSocketUnixSocketToString(UserObj *obj,
						     char *string,
						     int length);
static VMK_ReturnStatus UserSocketUnixDataSocketToString(UserObj *obj,
					 		 char *string,
							 int length);
static VMK_ReturnStatus UserSocketUnixServerSocketToString(UserObj *obj,
							   char *string,
							   int length);

/*
 * Method suite for a new, generic socket.
 *
 * Basically all it can do is bind or connect, then it will turn into one of the
 * other unix socket types.
 */
UserObj_Methods socketUnixMethods = USEROBJ_METHODS(
   (UserObj_OpenMethod) UserObj_NotADirectory,
   UserSocketUnixClose,
   (UserObj_ReadMethod) UserObj_BadParam,
   (UserObj_ReadMPNMethod) UserObj_BadParam,
   (UserObj_WriteMethod) UserObj_BadParam,
   (UserObj_WriteMPNMethod) UserObj_BadParam,
   UserSocketUnixStat,
   (UserObj_ChmodMethod) UserObj_NotImplemented,
   (UserObj_ChownMethod) UserObj_NotImplemented,
   (UserObj_TruncateMethod) UserObj_NotImplemented,
   (UserObj_UtimeMethod) UserObj_NotImplemented,
   (UserObj_StatFSMethod) UserObj_NotImplemented,
   UserSocketUnixPollSocket,
   (UserObj_UnlinkMethod) UserObj_NotADirectory,
   (UserObj_MkdirMethod) UserObj_NotADirectory,
   (UserObj_RmdirMethod) UserObj_NotADirectory,
   (UserObj_GetNameMethod) UserObj_NotADirectory,
   (UserObj_ReadSymLinkMethod) UserObj_NotADirectory,
   (UserObj_MakeSymLinkMethod) UserObj_NotADirectory,
   (UserObj_MakeHardLinkMethod) UserObj_NotADirectory,
   (UserObj_RenameMethod) UserObj_NotADirectory,
   (UserObj_MknodMethod) UserObj_NotADirectory,
   UserSocketUnixFcntl,
   (UserObj_FsyncMethod) UserObj_BadParam,
   (UserObj_ReadDirMethod) UserObj_NotADirectory,
   (UserObj_IoctlMethod) UserObj_NotImplemented,
   UserSocketUnixSocketToString,
   UserSocketUnixBind,
   UserSocketUnixConnect,
   (UserObj_SocketpairMethod) UserObj_NotImplemented, // XXX probably not needed
   (UserObj_AcceptMethod) UserObj_BadParam,
   (UserObj_GetSocketNameMethod) UserObj_NotImplemented,
   (UserObj_ListenMethod) UserObj_BadParam,
   (UserObj_SetsockoptMethod) UserObj_NotImplemented,
   (UserObj_GetsockoptMethod) UserObj_NotImplemented,
   (UserObj_SendmsgMethod) UserObj_BadParam,
   (UserObj_RecvmsgMethod) UserObj_BadParam,
   (UserObj_GetPeerNameMethod) UserObj_BadParam,
   (UserObj_ShutdownMethod) UserObj_BadParam
);

/*
 * Method suite for unix data socket.  Can send and receive data.
 */
UserObj_Methods socketUnixDataMethods = USEROBJ_METHODS(
   (UserObj_OpenMethod) UserObj_NotADirectory,
   UserSocketUnixClose,
   UserSocketUnixRead,
   (UserObj_ReadMPNMethod) UserObj_BadParam,
   UserSocketUnixWrite,
   (UserObj_WriteMPNMethod) UserObj_BadParam,
   UserSocketUnixStat,
   (UserObj_ChmodMethod) UserObj_NotImplemented,
   (UserObj_ChownMethod) UserObj_NotImplemented,
   (UserObj_TruncateMethod) UserObj_NotImplemented,
   (UserObj_UtimeMethod) UserObj_NotImplemented,
   (UserObj_StatFSMethod) UserObj_NotImplemented,
   UserSocketUnixPollDataSocket,
   (UserObj_UnlinkMethod) UserObj_NotADirectory,
   (UserObj_MkdirMethod) UserObj_NotADirectory,
   (UserObj_RmdirMethod) UserObj_NotADirectory,
   (UserObj_GetNameMethod) UserObj_NotADirectory,
   (UserObj_ReadSymLinkMethod) UserObj_NotADirectory,
   (UserObj_MakeSymLinkMethod) UserObj_NotADirectory,
   (UserObj_MakeHardLinkMethod) UserObj_NotADirectory,
   (UserObj_RenameMethod) UserObj_NotADirectory,
   (UserObj_MknodMethod) UserObj_NotADirectory,
   UserSocketUnixFcntl,
   (UserObj_FsyncMethod) UserObj_BadParam,
   (UserObj_ReadDirMethod) UserObj_NotADirectory,
   (UserObj_IoctlMethod) UserObj_NotImplemented,
   UserSocketUnixDataSocketToString,
   (UserObj_BindMethod) UserObj_BadParam,
   (UserObj_ConnectMethod) UserObj_BadParam,
   (UserObj_SocketpairMethod) UserObj_NotImplemented,
   (UserObj_AcceptMethod) UserObj_BadParam,
   UserSocketUnixGetSocketName,
   (UserObj_ListenMethod) UserObj_BadParam,
   (UserObj_SetsockoptMethod) UserObj_NotImplemented,
   (UserObj_GetsockoptMethod) UserObj_NotImplemented,
   UserSocketUnixSendmsg,
   UserSocketUnixRecvmsg,
   UserSocketUnixGetPeerName,
   (UserObj_ShutdownMethod) UserObj_NotImplemented 
);

/*
 * Methods for a unix server socket.  Can listen and accept connections.
 */
UserObj_Methods socketUnixServerMethods = USEROBJ_METHODS(
   (UserObj_OpenMethod) UserObj_NotADirectory,
   UserSocketUnixClose,
   (UserObj_ReadMethod) UserObj_BadParam,
   (UserObj_ReadMPNMethod) UserObj_BadParam,
   (UserObj_WriteMethod) UserObj_BadParam,
   (UserObj_WriteMPNMethod) UserObj_BadParam,
   UserSocketUnixStat,
   (UserObj_ChmodMethod) UserObj_NotImplemented,
   (UserObj_ChownMethod) UserObj_NotImplemented,
   (UserObj_TruncateMethod) UserObj_NotImplemented,
   (UserObj_UtimeMethod) UserObj_NotImplemented,
   (UserObj_StatFSMethod) UserObj_NotImplemented,
   UserSocketUnixPollServerSocket,
   (UserObj_UnlinkMethod) UserObj_NotADirectory,
   (UserObj_MkdirMethod) UserObj_NotADirectory,
   (UserObj_RmdirMethod) UserObj_NotADirectory,
   (UserObj_GetNameMethod) UserObj_NotADirectory,
   (UserObj_ReadSymLinkMethod) UserObj_NotADirectory,
   (UserObj_MakeSymLinkMethod) UserObj_NotADirectory,
   (UserObj_MakeHardLinkMethod) UserObj_NotADirectory,
   (UserObj_RenameMethod) UserObj_NotADirectory,
   (UserObj_MknodMethod) UserObj_NotADirectory,
   UserSocketUnixFcntl,
   (UserObj_FsyncMethod) UserObj_BadParam,
   (UserObj_ReadDirMethod) UserObj_NotADirectory,
   (UserObj_IoctlMethod) UserObj_NotImplemented,
   UserSocketUnixServerSocketToString,
   (UserObj_BindMethod) UserObj_BadParam,
   (UserObj_ConnectMethod) UserObj_BadParam,
   (UserObj_SocketpairMethod) UserObj_NotImplemented,
   UserSocketUnixAccept,
   UserSocketUnixGetSocketName,
   UserSocketUnixListen,
   (UserObj_SetsockoptMethod) UserObj_NotImplemented,
   (UserObj_GetsockoptMethod) UserObj_NotImplemented,
   (UserObj_SendmsgMethod) UserObj_BadParam,
   (UserObj_RecvmsgMethod) UserObj_BadParam,
   (UserObj_GetPeerNameMethod) UserObj_NotImplemented,
   (UserObj_ShutdownMethod) UserObj_NotImplemented 
);

struct UserPipe_Buf;
struct UserSocketUnix_Socket;
struct UserSocketUnixNameEntry;

/*
 * Connect waiter.  Callers of connect fill out this struct to add themselves to
 * a server socket's connect waiter list.
 */
typedef struct UserSocketUnixWaiter {
   User_CartelInfo *cartel;
   struct UserSocketUnix_Socket *socket;
   World_ID worldID;
} UserSocketUnixWaiter;

/*
 * Server socket.  Contains a list of waiters who called connect on this socket.
 */
typedef struct UserSocketUnix_ServerSocket {
   Bool listening;
   struct UserSocketUnixNameEntry *nameEntry;

   SP_SpinLock waiterLock;

   UserSocketUnixWaiter *connectWaiters;
   int maxConnectWaiters;
   int curConnectWaiters;

   World_ID acceptWaiterWorldID;
   Bool hasAcceptWaiter;
} UserSocketUnix_ServerSocket;

/*
 * Data socket.  Contains two pipes: one for reading, one for writing.
 */
typedef struct UserSocketUnix_DataSocket {
   Bool			connected;
   struct UserPipe_Buf *readPipe;
   struct UserPipe_Buf *writePipe;
   char		       *name;
} UserSocketUnix_DataSocket;

/*
 * Describes the state of a UserSocketUnix_Socket.
 */
typedef enum {
   USERSOCKETUNIX_NOTCONNECTED,
   USERSOCKETUNIX_CONNECTING,
   USERSOCKETUNIX_CONNECTED
} UserSocketUnixSocketState;

/*
 * Generic socket.  Contains pending connection state.
 */
typedef struct UserSocketUnix_Socket {
   SP_SpinLock lock;

   /*
    * Set to true when in call to connect or bind.
    */
   Bool inCall;
   UserSocketUnixSocketState state;
   UserSocketUnix_DataSocket *dataSocket;
   UserObj *obj;

   Bool connectFailed;
} UserSocketUnix_Socket;

/*
 * An entry in the global namespace.
 */
typedef struct UserSocketUnixNameEntry {
   struct UserSocketUnixNameEntry *next;

   char *name;
   UserSocketUnix_ServerSocket *socket;
} UserSocketUnixNameEntry;

static SP_SpinLock namespaceLock;
static UserSocketUnixNameEntry *namespaceRoot;


/*
 *----------------------------------------------------------------------
 *
 * UserSocketUnix_Init --
 *
 *	Initializes unix socket data.
 *
 * Results:
 *	VMK_OK.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
UserSocketUnix_Init(void)
{
   SP_InitLock("Unix Namespace", &namespaceLock, UW_SP_RANK_UNIX_NAMESPACE);
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketUnixStrdup --
 *
 *	strdup that allocates on the given cartel's heap.
 *
 * Results:
 *	The copied string or NULL if out of memory.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static char*
UserSocketUnixStrdup(User_CartelInfo *uci, char *str)
{
   int len = strlen(str) + 1;
   char *newStr;

   newStr = User_HeapAlloc(uci, len);
   if (newStr != NULL) {
      snprintf(newStr, len, "%s", str);
   }

   return newStr;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketUnixGetName --
 *
 *	Returns a null-terminated version of the Linux name passed in.
 *
 * Results:
 *	The new name or NULL if out of memory.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static char*
UserSocketUnixGetName(User_CartelInfo *uci, LinuxSocketName *name,
		      uint32 namelen)
{
   char *str;
   int sizeToCopy = namelen - sizeof name->family + 1;

   /*
    * Already checked at lower level.
    */
   ASSERT(namelen <= sizeof *name);

   str = User_HeapAlloc(uci, sizeToCopy);
   if (str != NULL) {
      snprintf(str, sizeToCopy, "%s", name->data);
   }

   return str;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketUnixWaiterAdd --
 *
 *	Adds a socket to a server socket's connect waiter list.
 *
 * Results:
 *	VMK_OK on success, VMK_LIMIT_EXCEEDED if pending queue is full.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserSocketUnixWaiterAdd(UserSocketUnix_ServerSocket *serverSocket,
			UserSocketUnix_Socket *socket,
			UserSocketUnixWaiter **outWaiter)
{
   UserSocketUnixWaiter *waiter;

   ASSERT(SP_IsLocked(&serverSocket->waiterLock));
   ASSERT(serverSocket->curConnectWaiters <= serverSocket->maxConnectWaiters);
   if (serverSocket->curConnectWaiters == serverSocket->maxConnectWaiters) {
      return VMK_LIMIT_EXCEEDED;
   }

   waiter = &serverSocket->connectWaiters[serverSocket->curConnectWaiters];
   waiter->cartel = MY_USER_CARTEL_INFO;
   waiter->socket = socket;
   waiter->worldID = MY_RUNNING_WORLD->worldID;
   serverSocket->curConnectWaiters++;

   *outWaiter = waiter;

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketUnixWaiterRemove --
 *
 *	Removes the given socket from the server socket's connect waiter
 *	list.
 *
 * Results:
 *	VMK_OK on success, VMK_NOT_FOUND if the socket is not found.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserSocketUnixWaiterRemove(UserSocketUnix_ServerSocket *serverSocket,
			   UserSocketUnix_Socket *socket)
{
   Bool found = FALSE;
   int i;

   ASSERT(SP_IsLocked(&serverSocket->waiterLock));

   for (i = 0; i < serverSocket->curConnectWaiters; i++) {
      if (serverSocket->connectWaiters[i].socket == socket) {
         memmove(&serverSocket->connectWaiters[i],
	         &serverSocket->connectWaiters[serverSocket->curConnectWaiters],
		 (serverSocket->curConnectWaiters - i - 1) *
		    sizeof(UserSocketUnixWaiter));
         found = TRUE;
	 break;
      }
   }

   if (found) {
      serverSocket->curConnectWaiters--;
      return VMK_OK;
   } else {
      return VMK_NOT_FOUND;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketUnixNameFindLocked --
 *
 *	Searches for a given name in the namespace.  Assumes the
 *	namespace lock is locked.
 *
 * Results:
 *	VMK_OK on success, VMK_NOT_FOUND if the name doesn't exist.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserSocketUnixNameFindLocked(char *name,
			     UserSocketUnix_ServerSocket **outSocket)
{
   VMK_ReturnStatus status = VMK_NOT_FOUND;
   UserSocketUnixNameEntry *cur;

   ASSERT(SP_IsLocked(&namespaceLock));

   if (outSocket != NULL) {
      *outSocket = NULL;
   }

   for (cur = namespaceRoot; cur != NULL; cur = cur->next) {
      if (strcmp(cur->name, name) == 0) {
         if (outSocket != NULL) {
	    *outSocket = cur->socket;
	 }
	 status = VMK_OK;
	 break;
      }
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketUnixNameFind --
 *
 *	Searches for a given name in the namespace.
 *
 * Results:
 *	VMK_OK on success, VMK_NOT_FOUND if the name doesn't exist.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */


static VMK_ReturnStatus
UserSocketUnixNameFind(char *name, UserSocketUnix_ServerSocket **outSocket)
{
   VMK_ReturnStatus status;

   SP_Lock(&namespaceLock);
   status = UserSocketUnixNameFindLocked(name, outSocket);
   SP_Unlock(&namespaceLock);

   /*
    * It may be the case that we found the name but a server socket has not yet
    * been associated with that name.  If this is the case, act as if the name
    * itself was not found.
    */
   if (status == VMK_OK && *outSocket == NULL) {
      status = VMK_NOT_FOUND;
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketUnixNameReserve --
 *
 *	Reserve a name in the global namespace.
 *
 * Results:
 *	VMK_OK on success, VMK_EXISTS if a socket with the given name
 *	already exists.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserSocketUnixNameReserve(char *name, UserSocketUnixNameEntry **outEntry)
{
   VMK_ReturnStatus status;

   SP_Lock(&namespaceLock);
   status = UserSocketUnixNameFindLocked(name, NULL);
   if (status == VMK_NOT_FOUND) {
      UserSocketUnixNameEntry *entry;

      entry = User_HeapAlloc(MY_USER_CARTEL_INFO, sizeof *entry);
      if (entry == NULL) {
         status = VMK_NO_MEMORY;
      } else {
         entry->name = UserSocketUnixStrdup(MY_USER_CARTEL_INFO, name);
	 entry->socket = NULL;

	 entry->next = namespaceRoot;
	 namespaceRoot = entry;

	 *outEntry = entry;
	 status = VMK_OK;
      }
   } else if (status == VMK_OK) {
      status = VMK_EXISTS;
   } else {
      ASSERT(FALSE);
   }
   SP_Unlock(&namespaceLock);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketUnixNameRemove --
 *
 *	Removes the given socket from the global namespace.
 *
 * Results:
 *	VMK_OK on success, VMK_NOT_FOUND if the name isn't found.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserSocketUnixNameRemove(User_CartelInfo *uci, UserSocketUnixNameEntry *entry)
{
   VMK_ReturnStatus status = VMK_NOT_FOUND;
   UserSocketUnixNameEntry *prev, *cur;

   SP_Lock(&namespaceLock);
   for (prev = NULL, cur = namespaceRoot; cur != NULL;
	prev = cur, cur = cur->next) {
      if (cur == entry) {
         if (prev == NULL) {
	    namespaceRoot = cur->next;
	 } else {
	    prev->next = cur->next;
	 }

	 User_HeapFree(uci, cur->name);
	 User_HeapFree(uci, cur);

	 status = VMK_OK;
	 break;
      }
   }
   SP_Unlock(&namespaceLock);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketUnixLogNamespace --
 *
 *	Dumps out all the names in the unix socket namespace.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
UserSocketUnixLogNamespace(int logLevel)
{
   if (vmx86_log) {
      UserSocketUnixNameEntry *cur;

      UWLOG(logLevel, "Dumping VMK unix-domain socket namespace...");

      SP_Lock(&namespaceLock);
      if (namespaceRoot == NULL) {
	 UWLOG(logLevel, "namespace empty!");
      }
      for (cur = namespaceRoot; cur != NULL; cur = cur->next) {
	 UWLOG(logLevel, "name: '%s', serverSocket: %p", cur->name,
	       cur->socket);
      }
      SP_Unlock(&namespaceLock);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketUnixServerSocketCreate --
 *
 *	Creates a new UserSocketUnix_ServerSocket and adds it for the
 *	unix socket namespace.
 *
 * Results:
 *	VMK_OK on success, VMK_EXISTS if the name already exists.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserSocketUnixServerSocketCreate(User_CartelInfo *uci,
				 UserSocketUnixNameEntry *nameEntry,
				 UserSocketUnix_ServerSocket **outSocket)
{
   UserSocketUnix_ServerSocket *newSocket;

   ASSERT(nameEntry->socket == NULL);

   newSocket = User_HeapAlloc(uci, sizeof *newSocket);
   if (newSocket == NULL) {
      return VMK_NO_MEMORY;
   }

   memset(newSocket, 0, sizeof *newSocket);
   newSocket->nameEntry = nameEntry;
   nameEntry->socket = newSocket;
   SP_InitLock("Unix Domain Server", &newSocket->waiterLock,
               UW_SP_RANK_UNIX_SERVER_SOCKET);

   *outSocket = newSocket;

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketUnixServerSocketDestroy --
 *
 *	Cleans up the socket struct.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
UserSocketUnixServerSocketDestroy(User_CartelInfo *uci,
				  UserSocketUnix_ServerSocket *serverSocket)
{
   int i;

   /*
    * Remove this server socket's name from the global namespace.
    */
   UserSocketUnixNameRemove(uci, serverSocket->nameEntry);

   /*
    * Wake up all the connect waiters.  Set their state to NOTCONNECTED so they
    * know the connect attempt failed and will clean themselves up.
    */
   SP_Lock(&serverSocket->waiterLock);
   serverSocket->listening = FALSE;
   for (i = 0; i < serverSocket->curConnectWaiters; i++) {
      UserSocketUnix_Socket *socket = serverSocket->connectWaiters[i].socket;
      SP_Lock(&socket->lock);
      socket->state = USERSOCKETUNIX_NOTCONNECTED;
      socket->connectFailed = TRUE;
      UserThread_Wakeup(serverSocket->connectWaiters[i].worldID,
			UTW_WAIT_COMPLETE);
      SP_Unlock(&socket->lock);

   }
   if (serverSocket->connectWaiters != NULL) {
      User_HeapFree(uci, serverSocket->connectWaiters);
   }
   SP_Unlock(&serverSocket->waiterLock);
   SP_CleanupLock(&serverSocket->waiterLock);
   User_HeapFree(uci, serverSocket);
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketUnixDataSocketCreate --
 *
 *      Creates a new, unconnected, anonymous unix socket object.
 *	Data sockets' connection (two pipes) are created in Accept.
 *
 * Results:
 *      Returns a UserObj or NULL if VMK_NO_MEMORY.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserSocketUnixDataSocketCreate(User_CartelInfo *uci, char *name,
			       UserSocketUnix_DataSocket **outSocket)
{
   UserSocketUnix_DataSocket *newSocket;

   newSocket = User_HeapAlloc(uci, sizeof *newSocket);
   if (newSocket == NULL) {
      return VMK_NO_MEMORY;
   }

   memset(newSocket, 0, sizeof *newSocket);

   newSocket->name = UserSocketUnixStrdup(uci, name);
   if (newSocket->name == NULL) {
      User_HeapFree(uci, newSocket);
      return VMK_NO_MEMORY;
   }

   *outSocket = newSocket;

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketUnixDataSocketDestroy --
 *
 *	Cleans up and frees a unix socket info struct.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
UserSocketUnixDataSocketDestroy(User_CartelInfo *uci,
				UserSocketUnix_DataSocket *socket)
{
   ASSERT(socket != NULL);

   if (socket->readPipe) {
      UserPipe_Close(socket->readPipe, USEROBJ_TYPE_PIPEREAD);
   }
   if (socket->writePipe) {
      UserPipe_Close(socket->writePipe, USEROBJ_TYPE_PIPEWRITE);
   }

   if (socket->name) {
      User_HeapFree(uci, socket->name);
   }

   User_HeapFree(uci, socket);
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketUnixSocketCreate --
 *
 *	Creates a new UserSocketUnix_Socket.
 *
 * Results:
 *	VMK_OK on success, VMK_NO_MEMORY on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserSocketUnixSocketCreate(User_CartelInfo *uci,
			   UserSocketUnix_Socket **outSocket)
{
   UserSocketUnix_Socket *newSocket;

   newSocket = User_HeapAlloc(uci, sizeof *newSocket);
   if (newSocket == NULL) {
      return VMK_NO_MEMORY;
   }

   memset(newSocket, 0, sizeof *newSocket);

   SP_InitLock("Unix Domain Generic", &newSocket->lock,
	       UW_SP_RANK_UNIX_SOCKET);

   *outSocket = newSocket;

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketUnixSocketDestroy --
 *
 *	Cleans up the socket struct.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
UserSocketUnixSocketDestroy(User_CartelInfo *uci,
			    UserSocketUnix_Socket *socket)
{
   if (socket->state == USERSOCKETUNIX_CONNECTING) {
      ASSERT(socket->dataSocket != NULL);
      ASSERT(socket->obj != NULL);

      UserSocketUnixDataSocketDestroy(uci, socket->dataSocket);
      User_HeapFree(uci, socket->obj);
   } else {
      ASSERT(socket->state == USERSOCKETUNIX_NOTCONNECTED ||
	     socket->state == USERSOCKETUNIX_CONNECTED);
   }

   SP_CleanupLock(&socket->lock);
   User_HeapFree(uci, socket);
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketUnixObjDestroy --
 *
 *      Destroys the given unix socket object.
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
UserSocketUnixObjDestroy(User_CartelInfo *uci, UserObj *obj)
{
   ASSERT(obj != NULL);

   if (obj->type == USEROBJ_TYPE_SOCKET_UNIX) {
      ASSERT(obj->data.socketUnix != NULL);

      UserSocketUnixSocketDestroy(uci, obj->data.socketUnix);
      obj->data.socketUnix = NULL;
   } else if (obj->type == USEROBJ_TYPE_SOCKET_UNIX_DATA) {
      ASSERT(obj->data.socketUnixData != NULL);

      UserSocketUnixDataSocketDestroy(uci, obj->data.socketUnixData);
      obj->data.socketUnixData = NULL;
   } else if (obj->type == USEROBJ_TYPE_SOCKET_UNIX_SERVER) {
      ASSERT(obj->data.socketUnixServer != NULL);

      UserSocketUnixServerSocketDestroy(uci, obj->data.socketUnixServer);
      obj->data.socketUnixServer = NULL;
   } else {
      ASSERT(FALSE);
   }

   memset(&obj->data, 0, sizeof obj->data);
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketUnix_Create --
 *
 *	Create a new, generic socket object.
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
UserSocketUnix_Create(User_CartelInfo* uci, LinuxSocketType type,
		      LinuxSocketProtocol protocol, UserObj** outObj)
{
   VMK_ReturnStatus status;
   UserSocketUnix_Socket *socket;

   if (type != LINUX_SOCKETTYPE_STREAM) {
      UWWarn("Unsupported linux socket type %#x", type);
      return VMK_NOT_SUPPORTED;
   }

   if (protocol != LINUX_SOCKETPROTO_DEFAULT) {
      UWWarn("Unsupported linux socket protocol %#x", protocol);
      return VMK_NOT_SUPPORTED;
   }

   status = UserSocketUnixSocketCreate(uci, &socket);
   if (status == VMK_OK) {
      *outObj = UserObj_Create(uci, USEROBJ_TYPE_SOCKET_UNIX,
			       (UserObj_Data)socket, &socketUnixMethods,
			       USEROBJ_OPEN_RDWR);
      if (*outObj == NULL) {
         UserSocketUnixSocketDestroy(uci, socket);
	 status = VMK_NO_RESOURCES;
      }
   }
   
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketUnixClose --
 *
 *	Close the given socket object.
 *
 * Results:
 *	VMK_OK.
 *
 * Side-effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserSocketUnixClose(UserObj* obj, User_CartelInfo* uci)
{
   UserSocketUnixObjDestroy(uci, obj);
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketUnixStat --
 *
 *      Get stats for the socket.
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
UserSocketUnixStat(UserObj* obj, LinuxStat64 *statBuf)
{
   memset(statBuf, 0x0, sizeof *statBuf);

   statBuf->st_mode = LINUX_MODE_IFSOCK | LINUX_MODE_IRUSR | LINUX_MODE_IWUSR;

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketUnixRead --
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
UserSocketUnixRead(UserObj* obj,
                   UserVA userData,
                   UNUSED_PARAM(uint64 offset),
                   uint32 userLength,
                   uint32 *bytesRead)
{
   ASSERT(obj->type == USEROBJ_TYPE_SOCKET_UNIX_DATA);

   if (!obj->data.socketUnixData->readPipe) {
      UWLOG(0, "read() called on socket before connected.");
      return VMK_BAD_PARAM;
   }

   return UserPipe_Read(obj->data.socketUnixData->readPipe,
			UserObj_IsOpenForBlocking(obj), userData, userLength,
			bytesRead);
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketUnixWrite --
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
UserSocketUnixWrite(UserObj* obj,
                    UserVAConst userData,
                    UNUSED_PARAM(uint64 offset),
                    uint32 userLength,
                    uint32* bytesWritten)
{
   ASSERT(obj->type == USEROBJ_TYPE_SOCKET_UNIX_DATA);

   if (!obj->data.socketUnixData->writePipe) {
      UWLOG(0, "write() called on socket before connected.");
      return VMK_BAD_PARAM;
   }

   return UserPipe_Write(obj->data.socketUnixData->writePipe,
			 UserObj_IsOpenForBlocking(obj), userData, userLength,
			 bytesWritten);
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketUnixFcntl --
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
UserSocketUnixFcntl(UNUSED_PARAM(UserObj *obj),
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
 * UserSocketUnix*SocketToString --
 *
 *	Returns a string representation of the given object.
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
UserSocketUnixSocketToString(UserObj *obj, char *string, int length)
{
   UserSocketUnix_Socket *socket = obj->data.socketUnix;
   int len;

   SP_Lock(&socket->lock);
   if (socket->inCall) {
      len = snprintf(string, length, "InCall, %s, dSock: %p obj: %p",
		     socket->state == USERSOCKETUNIX_NOTCONNECTED ?
			"NotConnected" :
			(socket->state == USERSOCKETUNIX_CONNECTING ?
			   "Connecting" : "Connected"),
		     socket->dataSocket, socket->obj);
   } else {
      len = snprintf(string, length, "NotInCall%s.",
		     socket->connectFailed ? ", LastConnectFailed" : "");
   }
   SP_Unlock(&socket->lock);

   if (len >= length) {
      UWLOG(1, "Description string too long (%d vs %d).  Truncating.", len,
	    length);
   }
   return VMK_OK;
}

static VMK_ReturnStatus
UserSocketUnixDataSocketToString(UserObj *obj, char *string, int length)
{
   VMK_ReturnStatus status;
   UserSocketUnix_DataSocket *dataSocket = obj->data.socketUnixData;
   int len;

   len = snprintf(string, length, "%s, %s, rdPipe: ", dataSocket->name,
		  dataSocket->connected ? "Connected" : "NotConnected");
   if (len >= length) {
      goto done;
   }

   status = UserPipe_ToString(dataSocket->readPipe, string + len, length - len);
   ASSERT(status == VMK_OK);
   if (len >= length) {
      goto done;
   }

   len = strlen(string);
   len += snprintf(string + len, length - len, ", wrPipe: ");
   if (len >= length) {
      goto done;
   }

   status = UserPipe_ToString(dataSocket->writePipe, string + len, length - len);
   ASSERT(status == VMK_OK);

done:
   if (len >= length) {
      UWLOG(1, "Description string too long (%d vs %d).  Truncating.", len,
	    length);
   }
   return VMK_OK;
}

static VMK_ReturnStatus
UserSocketUnixServerSocketToString(UserObj *obj, char *string, int length)
{
   UserSocketUnix_ServerSocket *serverSocket = obj->data.socketUnixServer;
   int len;

   if (serverSocket->listening) {
      SP_Lock(&serverSocket->waiterLock);
      len = snprintf(string, length,
		     "%s: Listening, %d/%d cnct wtrs, %s, acptWldId: %d",
		     serverSocket->nameEntry->name,
		     serverSocket->curConnectWaiters,
		     serverSocket->maxConnectWaiters,
		     serverSocket->hasAcceptWaiter ? "HasAcptWtr" : "NoAcptWtr",
		     serverSocket->acceptWaiterWorldID);
      SP_Unlock(&serverSocket->waiterLock);
   } else {
      len = snprintf(string, length, "%s: NotListening",
		     serverSocket->nameEntry->name);
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
 * UserSocketUnixBind --
 *
 *	Bind the given socket to the given name.  The generic socket
 *	passed in will turn into a server socket.
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
UserSocketUnixBind(UserObj* obj, LinuxSocketName* name, uint32 linuxNamelen)
{
   VMK_ReturnStatus status;
   User_CartelInfo *uci = MY_USER_CARTEL_INFO;
   UserSocketUnix_Socket *socket = obj->data.socketUnix;
   UserSocketUnix_ServerSocket *serverSocket = NULL;
   UserSocketUnixNameEntry *reservation = NULL;
   UserObj *newObj;
   char *tmpName = NULL;

   ASSERT(name != NULL);
   ASSERT(obj->type == USEROBJ_TYPE_SOCKET_UNIX);

   /*
    * First check if we're already in a bind or connect call.
    */
   SP_Lock(&socket->lock);
   if (socket->inCall) {
      SP_Unlock(&socket->lock);
      return VMK_BAD_PARAM;
   }
   socket->inCall = TRUE;
   SP_Unlock(&socket->lock);

   /*
    * Do some error checking.
    */
   if (name->family != LINUX_SOCKETFAMILY_VMK) {
      UWLOG(0, "Unsupported family: %d", name->family);
      status = VMK_NOT_SUPPORTED;
      goto done;
   }

   /*
    * Convert the given name into a null-terminated string.
    */
   tmpName = UserSocketUnixGetName(uci, name, linuxNamelen);
   if (tmpName == NULL) {
      status = VMK_NO_RESOURCES;
      goto done;
   }

   UWLOG(0, "Trying to bind to name: '%s'", tmpName);

   /*
    * Reserve an entry in the namespace.  Will fail if the name we're trying to
    * bind to already exists.
    */
   status = UserSocketUnixNameReserve(tmpName, &reservation);
   if (status != VMK_OK) {
      UWLOG(0, "Couldn't reserve name '%s': %s", tmpName,
	    UWLOG_ReturnStatusToString(status));
      goto done;
   }

   /*
    * We got the name, so create the server socket.
    */
   status = UserSocketUnixServerSocketCreate(uci, reservation, &serverSocket);
   if (status != VMK_OK) {
      UWLOG(0, "Couldn't create server socket: %s",
	    UWLOG_ReturnStatusToString(status));
      goto done;
   }

   /*
    * Create the new object.
    */
   newObj = UserObj_Create(uci, USEROBJ_TYPE_SOCKET_UNIX_SERVER,
			   (UserObj_Data)serverSocket, &socketUnixServerMethods,
			   obj->openFlags);
   if (newObj == NULL) {
      status = VMK_NO_RESOURCES;
      goto done;
   }

   /*
    * Replace the old object with the new object in the fd list.
    */
   status = UserObj_FDReplaceObj(uci, obj, newObj);
   /*
    * Note: There is a very unlikely case in which this call could fail:
    * if the program were to call close() on this socket from another thread
    * right after the first thread called bind().  In that case, the current
    * obj would not be found in the file descriptor list.
    */

done:
   if (status != VMK_OK) {
      /*
       * If serverSocket was created, then it will take care of destroying the
       * namespace entry for us.  If not, then we need to remove the reservation
       * ourselves.
       */
      if (serverSocket != NULL) {
         UserSocketUnixServerSocketDestroy(uci, serverSocket);
      } else {
	 if (reservation != NULL) {
	    UserSocketUnixNameRemove(uci, reservation);
	 }
      }

      SP_Lock(&socket->lock);
      socket->inCall = FALSE;
      SP_Unlock(&socket->lock);
   }
   if (tmpName != NULL) {
      User_HeapFree(uci, tmpName);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketUnixInitDataObject --
 *
 *	Initializes a new data object and replaces the original socket
 *	object with it in the file descriptor table.
 *
 * Results:
 *	VMK_OK if okay, otherwise on error.
 *
 * Side-effects:
 *	New socket replaces old in fd table.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserSocketUnixInitDataObject(UserObj *socketObj, UserObj *socketDataObj,
			     UserSocketUnix_DataSocket *dataSocket)
{
   VMK_ReturnStatus status;

   UserObj_InitObj(socketDataObj, USEROBJ_TYPE_SOCKET_UNIX_DATA,
		   (UserObj_Data)dataSocket, &socketUnixDataMethods,
		   socketObj->openFlags);
   status = UserObj_FDReplaceObj(MY_USER_CARTEL_INFO, socketObj, socketDataObj);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketUnixConnect --
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
UserSocketUnixConnect(UserObj* obj, LinuxSocketName* name, uint32 linuxNamelen)
{
   VMK_ReturnStatus status;
   User_CartelInfo *uci = MY_USER_CARTEL_INFO;
   UserSocketUnix_Socket *socket = obj->data.socketUnix;
   UserSocketUnix_ServerSocket *serverSocket = NULL;
   UserSocketUnix_DataSocket *dataSocket = NULL;
   UserSocketUnixWaiter *waiter;
   Bool serverSocketLocked = FALSE;
   Bool waiterAdded = FALSE;
   UserObj *newObj = NULL;
   char *tmpName = NULL;

   ASSERT(name != NULL);
   ASSERT(obj->type == USEROBJ_TYPE_SOCKET_UNIX);

   SP_Lock(&socket->lock);
   /*
    * Return EINPROGRESS if we're connecting.
    */
   if (socket->state == USERSOCKETUNIX_CONNECTING) {
      SP_Unlock(&socket->lock);
      return VMK_STATUS_PENDING;
   }

   /*
    * If we're connected, check if we need to create the object.
    */
   if (socket->state == USERSOCKETUNIX_CONNECTED) {
      newObj = socket->obj;
      dataSocket = socket->dataSocket;
      SP_Unlock(&socket->lock);

      if (dataSocket == NULL || newObj == NULL) {
         /*
	  * If either of them are NULL, they should both be NULL.
	  */
	 ASSERT(dataSocket == NULL && newObj == NULL);

	 /*
	  * Looks like the new object has already been created.
	  */
         status = VMK_OK;
      } else {
         /*
	  * Initialize the new object.
	  */
         status = UserSocketUnixInitDataObject(obj, newObj, dataSocket);
      }

      return status;
   }

   /*
    * Check if we're already in a bind or connect call.
    */
   if (socket->inCall) {
      SP_Unlock(&socket->lock);
      return VMK_BAD_PARAM;
   }
   socket->inCall = TRUE;
   SP_Unlock(&socket->lock);

   /*
    * Make sure socket family is correct.
    */
   if (name->family != LINUX_SOCKETFAMILY_VMK) {
      UWLOG(0, "Unsupported family: %d", name->family);
      status = VMK_NOT_SUPPORTED;
      goto done;
   }

   /*
    * Translate the incoming name to a null-terminated string.
    */
   tmpName = UserSocketUnixGetName(uci, name, linuxNamelen);
   if (tmpName == NULL) {
      status = VMK_NO_RESOURCES;
      goto done;
   }

   /*
    * Find the server socket to connect to.
    */
   status = UserSocketUnixNameFind(tmpName, &serverSocket);
   if (status != VMK_OK) {
      UWLOG(0, "Couldn't find '%s'", tmpName);
      UserSocketUnixLogNamespace(1);
      goto done;
   }

   SP_Lock(&serverSocket->waiterLock);
   serverSocketLocked = TRUE;

   /*
    * If the server socket hasn't called listen yet, there's not much we can
    * do.
    */
   if (!serverSocket->listening) {
      UWLOG(0, "Not listening.");
      status = VMK_ECONNREFUSED;
      goto done;
   }

   /*
    * First allocate space for the new UserObj so that we can't fail from a
    * lack of memory later on.
    */
   newObj = User_HeapAlloc(uci, sizeof *newObj);
   if (newObj == NULL) {
      status = VMK_NO_MEMORY;
      goto done;
   }

   /*
    * Create the new data socket.
    */
   status = UserSocketUnixDataSocketCreate(uci, tmpName, &dataSocket);
   if (status != VMK_OK) {
      UWLOG(0, "Couldn't create data socket");
      goto done;
   }

   /*
    * Fill in socket data before waking up accept thread.
    */
   SP_Lock(&socket->lock);
   socket->state = USERSOCKETUNIX_CONNECTING;
   socket->dataSocket = dataSocket;
   socket->obj = newObj;
   socket->connectFailed = FALSE;
   SP_Unlock(&socket->lock);

   /*
    * Add this socket to the connect waiter queue.
    */
   status = UserSocketUnixWaiterAdd(serverSocket, socket, &waiter);
   if (status != VMK_OK) {
      SP_Unlock(&serverSocket->waiterLock);
      goto done;
   }
   waiterAdded = TRUE;

   /*
    * Notify the accept waiter if present.
    */
   if (serverSocket->hasAcceptWaiter) {
      UserThread_Wakeup(serverSocket->acceptWaiterWorldID, UTW_WAIT_COMPLETE);
   }
   SP_Unlock(&serverSocket->waiterLock);
   serverSocketLocked = FALSE;

   /*
    * If this object shouldn't block, just return immediately.
    */
   if (!UserObj_IsOpenForBlocking(obj)) {
      status = VMK_WOULD_BLOCK;
      goto done;
   }

   /*
    * Otherwise, proceed to waiting for an accept.
    */
   SP_Lock(&socket->lock);
   while (socket->state == USERSOCKETUNIX_CONNECTING) {
      status = UserThread_Wait((uint32)waiter, CPUSCHED_WAIT_UW_UNIX_CONNECT,
			       &socket->lock, 0, UTWAIT_WITHOUT_PREPARE);
      if (status != VMK_OK) {
         SP_Unlock(&socket->lock);
	 goto done;
      }
   }
   if (socket->state != USERSOCKETUNIX_CONNECTED) {
      if (status == VMK_OK) {
         status = VMK_ECONNREFUSED;
      }
      SP_Unlock(&socket->lock);
      goto done;
   }
   SP_Unlock(&socket->lock);

   /*
    * Now initialize and add the new data socket to the fd list.
    */
   status = UserSocketUnixInitDataObject(obj, newObj, dataSocket);

done:
   /*
    * If something went wrong, do some cleanup.  Otherwise (if the connection
    * was successful or if non-blocking mode was set), just leave everything
    * alone.  Eventually they'll call poll, at which point we can finish the
    * transition to a data socket.
    */
   if (status != VMK_OK && status != VMK_WOULD_BLOCK) {
      if (serverSocketLocked) {
         ASSERT(serverSocket != NULL);
	 SP_Unlock(&serverSocket->waiterLock);
      }
      if (waiterAdded) {
         VMK_ReturnStatus tmpStatus;

	 /*
	  * Since the serverSocket may have died, we need to re-lookup the name.
	  */
	 tmpStatus = UserSocketUnixNameFind(tmpName, &serverSocket);
	 if (tmpStatus == VMK_OK) {
	    SP_Lock(&serverSocket->waiterLock);
	    UserSocketUnixWaiterRemove(serverSocket, socket);
	    SP_Unlock(&serverSocket->waiterLock);
	 }
      }

      SP_Lock(&socket->lock);
      socket->state = USERSOCKETUNIX_NOTCONNECTED;
      socket->inCall = FALSE;
      socket->dataSocket = NULL;
      socket->obj = NULL;
      socket->connectFailed = TRUE;
      SP_Unlock(&socket->lock);

      if (dataSocket != NULL) {
         UserSocketUnixDataSocketDestroy(uci, dataSocket);
      }

      if (newObj != NULL) {
         User_HeapFree(uci, newObj);
      }
   }
   if (tmpName != NULL) {
      User_HeapFree(uci, tmpName);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketUnixListen --
 *
 *	Listen for incoming connections on the given socket.  Takes a
 *	fresh server socket.
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
UserSocketUnixListen(UserObj* obj, int backlog)
{
   UserSocketUnix_ServerSocket *serverSocket = obj->data.socketUnixServer;

   ASSERT(obj->type == USEROBJ_TYPE_SOCKET_UNIX_SERVER);
   ASSERT(serverSocket != NULL);

   if (serverSocket->listening) {
      UWLOG(0, "Already listening on socket.");
      return VMK_EADDRINUSE;
   }

   if (backlog <= 0) {
      UWLOG(0, "Invalid backlog param: %d", backlog);
      return VMK_BAD_PARAM;
   }

   /*
    * Allocate space for the connect waiters queue.
    */
   SP_Lock(&serverSocket->waiterLock);
   serverSocket->connectWaiters = User_HeapAlloc(MY_USER_CARTEL_INFO,
   						 backlog *
						 sizeof(UserSocketUnixWaiter));
   if (serverSocket->connectWaiters == NULL) {
      SP_Unlock(&serverSocket->waiterLock);
      return VMK_NO_MEMORY;
   }
   serverSocket->maxConnectWaiters = backlog;
   SP_Unlock(&serverSocket->waiterLock);

   serverSocket->listening = TRUE;

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * UserSocketUnixAccept --
 *
 *	Accept a remote connection on the given socket.  Takes a
 *	server socket that has already called listen.
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
UserSocketUnixAccept(UserObj* obj, UserObj** acceptedSockObj,
                     LinuxSocketName *linuxName, uint32 *linuxNamelen)
{
   VMK_ReturnStatus status;
   User_CartelInfo *uci = MY_USER_CARTEL_INFO;
   UserSocketUnix_ServerSocket *serverSocket = obj->data.socketUnixServer;
   UserSocketUnix_DataSocket *dataSocket = NULL;
   UserSocketUnixWaiter waiter;
   volatile int numWaiters;
   UserObj *newObj = NULL;

   ASSERT(obj->type == USEROBJ_TYPE_SOCKET_UNIX_SERVER);

   /*
    * Must call listen before accept.
    */
   if (!serverSocket->listening) {
      return VMK_BAD_PARAM;
   }

   *acceptedSockObj = NULL;

   SP_Lock(&serverSocket->waiterLock);

   /*
    * Return an error if someone else already called accept.
    */
   if (serverSocket->hasAcceptWaiter) {
      status = VMK_BAD_PARAM;
      goto done;
   }

   /*
    * See how many waiters we have.  If we don't have any and are in
    * non-blocking mode, then return EAGAIN.
    */
   numWaiters = serverSocket->curConnectWaiters;
   if (numWaiters == 0 && !UserObj_IsOpenForBlocking(obj)) {
      status = VMK_WOULD_BLOCK;
      goto done;
   }

   /*
    * Wait for someone to try and connect.
    */
   while (numWaiters == 0) {
      serverSocket->acceptWaiterWorldID = MY_RUNNING_WORLD->worldID;
      serverSocket->hasAcceptWaiter = TRUE;

      status = UserThread_Wait((uint32)&serverSocket->acceptWaiterWorldID,
			       CPUSCHED_WAIT_UW_POLL, &serverSocket->waiterLock,
			       0, UTWAIT_WITHOUT_PREPARE);
      serverSocket->hasAcceptWaiter = FALSE;
      serverSocket->acceptWaiterWorldID = INVALID_WORLD_ID;
      if (status != VMK_OK) {
         goto done;
      }
      numWaiters = serverSocket->curConnectWaiters;
   }

   ASSERT(serverSocket->curConnectWaiters >= 1);

   /*
    * Save the first waiter.
    */
   memcpy(&waiter, &serverSocket->connectWaiters[0], sizeof waiter);

   /*
    * Create connection.
    */
   newObj = User_HeapAlloc(uci, sizeof *newObj);
   if (newObj == NULL) {
      status = VMK_NO_MEMORY;
      goto done;
   }

   status = UserSocketUnixDataSocketCreate(uci, serverSocket->nameEntry->name,
					   &dataSocket);
   if (status != VMK_OK) {
      goto done;
   }

   /*
    * Create the pipes connecting the two data sockets.
    */
   status = UserPipe_CreatePipe(uci, waiter.cartel, &dataSocket->readPipe);
   if (status != VMK_OK) {
      goto done;
   }

   status = UserPipe_CreatePipe(waiter.cartel, uci, &dataSocket->writePipe);
   if (status != VMK_OK) {
      (void) UserPipe_Close(dataSocket->readPipe, USEROBJ_TYPE_PIPEREAD);
      (void) UserPipe_Close(dataSocket->readPipe, USEROBJ_TYPE_PIPEWRITE);
      goto done;
   }

   waiter.socket->dataSocket->readPipe = dataSocket->writePipe;
   waiter.socket->dataSocket->writePipe = dataSocket->readPipe;

   /*
    * Remove the waiting connection from the connection queue and wake up the
    * waiter.
    */
   UserSocketUnixWaiterRemove(serverSocket, waiter.socket);
   SP_Lock(&waiter.socket->lock);
   waiter.socket->state = USERSOCKETUNIX_CONNECTED;
   UserThread_Wakeup(waiter.worldID, UTW_WAIT_COMPLETE);
   SP_Unlock(&waiter.socket->lock);

   /*
    * Finally, initialize new object.
    */
   UserObj_InitObj(newObj, USEROBJ_TYPE_SOCKET_UNIX_DATA,
		   (UserObj_Data)dataSocket, &socketUnixDataMethods,
		   USEROBJ_OPEN_RDWR);
   *acceptedSockObj = newObj;

done:
   SP_Unlock(&serverSocket->waiterLock); 

   if ((status == VMK_OK) && (linuxName != NULL)) {
      linuxName->family = LINUX_SOCKETFAMILY_VMK;
      snprintf(linuxName->data, sizeof linuxName->data, "%s",
               newObj->data.socketUnixData->name);
      *linuxNamelen = strlen(newObj->data.socketUnixData->name) +
                             sizeof(linuxName->family);
   }

   if (status != VMK_OK) {
      if (status != VMK_WOULD_BLOCK) {
         UWLOG(0, "accept() failed: %s", UWLOG_ReturnStatusToString(status));
      }

      if (newObj != NULL) {
         User_HeapFree(uci, newObj);
      }
      if (dataSocket != NULL) {
         UserSocketUnixDataSocketDestroy(uci, dataSocket);
      }
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketUnixGetSocketName --
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
UserSocketUnixGetSocketName(UserObj* obj, LinuxSocketName* outName,
			    uint32* linuxNamelen)
{
   char *name = NULL;

   if (obj->type == USEROBJ_TYPE_SOCKET_UNIX_DATA) {
      name = obj->data.socketUnixData->name;
   } else if (obj->type == USEROBJ_TYPE_SOCKET_UNIX_SERVER) {
      name = obj->data.socketUnixServer->nameEntry->name;
   } else {
      NOT_REACHED();
   }

   outName->family = LINUX_SOCKETFAMILY_VMK;
   snprintf(outName->data, sizeof outName->data, "%s", name);
   *linuxNamelen = strlen(name) + sizeof outName->family;

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketUnixSendmsg --
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
UserSocketUnixSendmsg(UserObj* obj, LinuxMsgHdr* msg, uint32 len,
		      uint32* bytesSent)
{
   ASSERT(obj->type == USEROBJ_TYPE_SOCKET_UNIX_DATA);
   return UserPipe_Sendmsg(obj->data.socketUnixData->writePipe,
			   UserObj_IsOpenForBlocking(obj), msg, len, bytesSent);
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketUnixRecvmsg --
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
UserSocketUnixRecvmsg(UserObj* obj, LinuxMsgHdr* msg, uint32 len,
		      uint32* bytesRecv)
{
   ASSERT(obj->type == USEROBJ_TYPE_SOCKET_UNIX_DATA);
   return UserPipe_Recvmsg(obj->data.socketUnixData->readPipe,
			   UserObj_IsOpenForBlocking(obj), msg, len, bytesRecv);
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketUnixPollDataSocket --
 *
 *	Polls on a data socket.
 *
 * Results:
 *      VMK_OK if the socket is ready for the given ops, or
 *      VMK_WOULD_BLOCK if not.
 *
 * Side-effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserSocketUnixPollDataSocket(UserObj *obj, VMKPollEvent inEvents,
			     VMKPollEvent* outEvents, UserObjPollAction action)
{
   VMK_ReturnStatus status = VMK_OK;

   ASSERT(obj->type == USEROBJ_TYPE_SOCKET_UNIX_DATA);

   if (action == UserObjPollNoAction || action == UserObjPollNotify ||
       action == UserObjPollCleanup) {
      if (inEvents & VMKPOLL_READ) {
	 status = UserPipe_Poll(obj->data.socketUnixData->readPipe,
				USEROBJ_TYPE_PIPEREAD, VMKPOLL_READ, outEvents,
				action);
      }

      /*
       * We only want to perform the poll for write if the poll for read
       * succeeded, unless we're in cleanup mode.  In that case, we still want
       * to try and cleanup the write pipe.
       */
      if (inEvents & VMKPOLL_WRITE &&
	  (status == VMK_OK || status == VMK_WOULD_BLOCK ||
	   action == UserObjPollCleanup)) {
         VMK_ReturnStatus tmpStatus;
	 VMKPollEvent tmpEvents = 0;

	 tmpStatus = UserPipe_Poll(obj->data.socketUnixData->writePipe,
				   USEROBJ_TYPE_PIPEWRITE, VMKPOLL_WRITE,
				   &tmpEvents, action);
	 if (tmpStatus == VMK_OK) {
	    *outEvents |= tmpEvents;
	 } else if (status == VMK_OK) {
	    status = tmpStatus;
	 }
      }

      if (action != UserObjPollCleanup) {
         /* either we have an event or we would block */
         ASSERT((*outEvents != 0) || (status != VMK_OK));
      }
   } else {
      ASSERT(FALSE);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketUnixPollServerSocket --
 *
 *	Polls on a server socket.
 *
 * Results:
 *      VMK_OK if the socket is ready for the given ops, or
 *      VMK_WOULD_BLOCK if not.
 *
 * Side-effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserSocketUnixPollServerSocket(UserObj *obj, VMKPollEvent inEvents,
			       VMKPollEvent *outEvents,
			       UserObjPollAction action)
{
   VMK_ReturnStatus status = VMK_OK;

   ASSERT(obj->type == USEROBJ_TYPE_SOCKET_UNIX_SERVER);

   if (action == UserObjPollCleanup) {
      SP_Lock(&obj->data.socketUnixServer->waiterLock);
      obj->data.socketUnixServer->hasAcceptWaiter = FALSE;
      obj->data.socketUnixServer->acceptWaiterWorldID = INVALID_WORLD_ID;
      SP_Unlock(&obj->data.socketUnixServer->waiterLock);
   } else if ((action == UserObjPollNoAction) ||
              (action == UserObjPollNotify)) {
      if (inEvents & (VMKPOLL_READ | VMKPOLL_WRITE)) {
         SP_Lock(&obj->data.socketUnixServer->waiterLock);

         /*
	  * If there are connect waiters, they can call accept now.
	  */
         if (obj->data.socketUnixServer->curConnectWaiters > 0) {
	    *outEvents = (inEvents & (VMKPOLL_READ | VMKPOLL_WRITE));
	 } else {
	    if (action == UserObjPollNotify) {
	       obj->data.socketUnixServer->hasAcceptWaiter = TRUE;
	       obj->data.socketUnixServer->acceptWaiterWorldID =
						      MY_RUNNING_WORLD->worldID;
	    }
	    status = VMK_WOULD_BLOCK;
	 }

         SP_Unlock(&obj->data.socketUnixServer->waiterLock);

	 /* either we have an event or we would block */
	 ASSERT((*outEvents != 0) || (status != VMK_OK));
      }
   } else {
      ASSERT(FALSE);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketUnixPollSocket --
 *
 *	Polls on a generic socket.
 *
 * Results:
 *      VMK_OK if the socket is ready for the given ops, or
 *      VMK_WOULD_BLOCK if not.
 *
 * Side-effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserSocketUnixPollSocket(UserObj* obj, VMKPollEvent inEvents,
			 VMKPollEvent* outEvents, UserObjPollAction action)
{
   VMK_ReturnStatus status = VMK_OK;
   UserSocketUnix_Socket *socket = obj->data.socketUnix;

   ASSERT(obj->type == USEROBJ_TYPE_SOCKET_UNIX);

   SP_Lock(&socket->lock);

   if (action == UserObjPollCleanup) {
      ASSERT(socket->state != USERSOCKETUNIX_CONNECTED);
      if (socket->state == USERSOCKETUNIX_CONNECTING) {
         User_CartelInfo *uci = MY_USER_CARTEL_INFO;

         ASSERT(socket->dataSocket != NULL);
         ASSERT(socket->obj != NULL);
         UserSocketUnixDataSocketDestroy(uci, socket->dataSocket);
	 User_HeapFree(uci, socket->obj);
	 socket->dataSocket = NULL;
	 socket->obj = NULL;
	 socket->state = USERSOCKETUNIX_NOTCONNECTED;
      }
   } else if (action == UserObjPollNotify || action == UserObjPollNoAction) {
      if (inEvents & (VMKPOLL_READ | VMKPOLL_WRITE)) {
	 if (socket->state == USERSOCKETUNIX_CONNECTING) {
	    ASSERT(socket->inCall);
	    status = VMK_WOULD_BLOCK;
	 } else if (socket->state == USERSOCKETUNIX_NOTCONNECTED) {
	    if (socket->connectFailed) {
	       /*
	        * Polling on a failed connection should return POLLIN | POLLOUT.
		*/
	       *outEvents |= inEvents & (VMKPOLL_READ | VMKPOLL_WRITE);
	    } else {
	       /*
	        * Polling on an unconnected socket should return POLLOUT |
		* POLLHUP.
		*/
	       *outEvents |= (inEvents & VMKPOLL_WRITE) | VMKPOLL_WRHUP;
	    }
	 } else if (socket->state == USERSOCKETUNIX_CONNECTED) {
	    /*
	     * Connection succeeded, create UserObj and add it to fd list.
	     */
	    UserSocketUnix_DataSocket *dataSocket;
	    UserSocketUnixSocketState state;
	    UserObj *newObj;

	    dataSocket = socket->dataSocket;
	    newObj = socket->obj;
	    state = socket->state;

	    /*
	     * Unlock here because UserSocketUnixInitDataObject will hit a lock
	     * rank failure otherwise.
	     */
	    SP_Unlock(&socket->lock);

	    status = UserSocketUnixInitDataObject(obj, newObj, dataSocket);
	    if (status == VMK_OK) {
	       /*
	        * Poll on data socket.
		*/
	       status = UserSocketUnixPollDataSocket(newObj, inEvents,
						     outEvents, action);
	    }

	    return status;
	 }
      }
   } else {
      ASSERT(FALSE);
   }

   SP_Unlock(&socket->lock);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSocketUnixGetPeerName --
 *
 *	Get the name of the remote end connected to this socket.
 *
 * Results:
 *	name is set to the name of the remote socket.  VMK_OK if okay,
 *	otherwise if error.
 *
 * Side-effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
UserSocketUnixGetPeerName(UserObj* obj, LinuxSocketName* outName,
			  uint32* linuxNamelen)
{
   char *name = NULL;
   int len;

   if (obj->type == USEROBJ_TYPE_SOCKET_UNIX_DATA) {
      name = obj->data.socketUnixData->name;
   } else {
      NOT_REACHED();
   }

   outName->family = LINUX_SOCKETFAMILY_VMK;
   len = snprintf(outName->data, sizeof outName->data, "%s", name);
   if (len > sizeof outName->data) {
      return VMK_NAME_TOO_LONG;
   }
   *linuxNamelen = len - 1 + sizeof outName->family;

   return VMK_OK;
}
