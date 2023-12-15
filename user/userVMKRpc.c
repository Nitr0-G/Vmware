/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * Exposure of VMKernel RPC objects (via FD table) to a userworld.
 */

#include "user_int.h"
#include "userVMKRpc.h"

#define LOGLEVEL_MODULE UserRpc
#include "userLog.h"

static VMK_ReturnStatus UserVMKRpcClose(UserObj* obj, User_CartelInfo* uci);
static VMK_ReturnStatus UserVMKRpcPoll(UserObj* obj,
				       VMKPollEvent inEvents,
				       VMKPollEvent* outEvents,
				       UserObjPollAction action);
static VMK_ReturnStatus UserVMKRpcToString(UserObj *obj,
					   char *string,
					   int length);

/*
 * UserObj callback methods for rpc.
 */
UserObj_Methods rpcMethods = USEROBJ_METHODS(
   (UserObj_OpenMethod) UserObj_NotADirectory,
   UserVMKRpcClose,
   (UserObj_ReadMethod) UserObj_BadParam,
   (UserObj_ReadMPNMethod) UserObj_BadParam,
   (UserObj_WriteMethod) UserObj_BadParam,
   (UserObj_WriteMPNMethod) UserObj_BadParam,
   (UserObj_StatMethod) UserObj_BadParam,
   (UserObj_ChmodMethod) UserObj_BadParam,
   (UserObj_ChownMethod) UserObj_BadParam,
   (UserObj_TruncateMethod) UserObj_BadParam,
   (UserObj_UtimeMethod) UserObj_BadParam,
   (UserObj_StatFSMethod) UserObj_BadParam,
   UserVMKRpcPoll,
   (UserObj_UnlinkMethod) UserObj_BadParam,
   (UserObj_MkdirMethod) UserObj_BadParam,
   (UserObj_RmdirMethod) UserObj_BadParam,
   (UserObj_GetNameMethod) UserObj_BadParam,
   (UserObj_ReadSymLinkMethod) UserObj_BadParam,
   (UserObj_MakeSymLinkMethod) UserObj_BadParam,
   (UserObj_MakeHardLinkMethod) UserObj_BadParam,
   (UserObj_RenameMethod) UserObj_BadParam,
   (UserObj_MknodMethod) UserObj_BadParam,
   (UserObj_FcntlMethod) UserObj_BadParam,
   (UserObj_FsyncMethod) UserObj_BadParam,
   (UserObj_ReadDirMethod) UserObj_BadParam,
   (UserObj_IoctlMethod) UserObj_BadParam,
   UserVMKRpcToString,
   (UserObj_BindMethod) UserObj_BadParam,
   (UserObj_ConnectMethod) UserObj_BadParam,
   (UserObj_SocketpairMethod) UserObj_BadParam,
   (UserObj_AcceptMethod) UserObj_BadParam,
   (UserObj_GetSocketNameMethod) UserObj_BadParam,
   (UserObj_ListenMethod) UserObj_BadParam,
   (UserObj_SetsockoptMethod) UserObj_BadParam,
   (UserObj_GetsockoptMethod) UserObj_BadParam,
   (UserObj_SendmsgMethod) UserObj_BadParam,
   (UserObj_RecvmsgMethod) UserObj_BadParam,
   (UserObj_GetPeerNameMethod) UserObj_BadParam,
   (UserObj_ShutdownMethod) UserObj_BadParam
);


/*
 *----------------------------------------------------------------------
 *
 * UserVMKRpc_Create --
 *
 *	Create a new rpc object.  Lamely, RPC objects have two
 *	different identifiers the "fd" id (like other opened files),
 *	and the 'cnxID' which is the vmkernel-internal id.  Both(?)
 *	are currently used by the VMX...
 *
 * Results:
 *	VMK_OK if created and added, otherwise on error.  *cnxFD and
 *	*cnxID are set to valid or invalid values, as error indicates.
 *
 * Side-effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
UserVMKRpc_Create(User_CartelInfo* uci,
                  const char* cnxName,
                  LinuxFd* cnxFD,
                  RPC_Connection *cnxID)
{
   VMK_ReturnStatus status;
   uint32 numBuffers, bufferLength;
   Bool isSemaphore = FALSE;

   UWLOG(1, "(uci=%p, cnxFD=%p, cnxID=%p)", uci, cnxFD, cnxID);

   // XXX hack until we modify the userlevel+interface to pass this info
   if (strncmp(cnxName, "sema.", 5) == 0) {
      // mutex
      numBuffers = 1;
      bufferLength = sizeof(uint32);
      isSemaphore = TRUE;
   } else if (strncmp(cnxName, "userVCPU.", 8) == 0) {
      // userRPC to vcpu thread
      numBuffers = 1;
      bufferLength = sizeof(uint32);
   } else if (strncmp(cnxName, "vmxApp.", 6) == 0) {
      // cross userRPC to vmx thread
      numBuffers = MAX_VCPUS;
      bufferLength = sizeof(uint32);
   } else if (strncmp(cnxName, "vmkevent.", 9) == 0) {
      // vmkevent_vmx
      numBuffers = 2;
      bufferLength = 512;
   } else {
      UWWarn("Unknown rpc name (%s)", cnxName);
      return VMK_NOT_SUPPORTED;
   }
   // Create/lookup an RPC endpoint
   status = RPC_Register(cnxName, isSemaphore, FALSE, uci->cartelID,
                         numBuffers, bufferLength, uci->heap, cnxID);
   if (status != VMK_OK) {
      UWLOG(0, "RPC_Register(%s) failed: %s", cnxName,
            UWLOG_ReturnStatusToString(status));
      *cnxFD = USEROBJ_INVALID_HANDLE;
      *cnxID = -1;
      return status;
   }

   // Stick it in the userObj table
   *cnxFD = UserObj_FDAdd(uci, USEROBJ_TYPE_RPC, (UserObj_Data)(*cnxID),
                          &rpcMethods, USEROBJ_OPEN_STAT);

   if (*cnxFD == USEROBJ_INVALID_HANDLE) {
      UWLOG(0, "UserObj_FDAdd failed.");
      RPC_Unregister(*cnxID);
      *cnxID = -1;
      status = VMK_NO_FREE_HANDLES;
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserVMKRpcClose --
 *
 *	Unregister the RPC object and clean out the userObj.
 *
 * Results:
 *	VMK_OK if object is valid, VMK_BAD_PARAM if RPC object was
 *	already closed.
 *
 * Side-effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserVMKRpcClose(UserObj* obj, User_CartelInfo* uci)
{
   VMK_ReturnStatus status;

   ASSERT(obj);
   ASSERT(uci);

   ASSERT(obj->type == USEROBJ_TYPE_RPC);

   if (obj->data.rpcCnx != -1) {
      RPC_Unregister(obj->data.rpcCnx);
      obj->data.rpcCnx = -1;
      status = VMK_OK;
   } else {
      UWLOG(0, "rpcCnx already destroyed.");
      status = VMK_BAD_PARAM;
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserVMKRpcPoll --
 *
 *	Polls on this obj's RPC cnxID.
 *
 * Results:
 *	VMK_OK if data is ready, VMK_WOULD_BLOCK if no data ready,
 *	otherwise on error.
 *
 * Side-effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserVMKRpcPoll(UserObj* obj, VMKPollEvent inEvents, VMKPollEvent* outEvents,
               UserObjPollAction action)
{
   UWLOG(1, "(inEvents=%#x action=%d)", inEvents, action);

   if (action == UserObjPollCleanup) {
      return RPC_PollCleanup(obj->data.cnxID);
   } else {
      return RPC_Poll(obj->data.cnxID, inEvents, outEvents,
		      action == UserObjPollNotify);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * UserVMKRpcToString --
 *
 *	Returns a string representation of this object.
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
UserVMKRpcToString(UserObj *obj, char *string, int length)
{
   snprintf(string, length, "cnxId: %d", obj->data.rpcCnx);
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserVMKRpc_GetIDForFD --
 *
 *	Converts from a UserWorld file descriptor to a RPC cnxID.
 *
 * Results:
 *	VMK_OK on success, otherwise on error.
 *
 * Side-effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
UserVMKRpc_GetIDForFD(User_CartelInfo* uci, LinuxFd fd, RPC_Connection* id)
{
   VMK_ReturnStatus status;
   UserObj* obj;

   status = UserObj_Find(uci, fd, &obj);
   if (status == VMK_OK) {
      if (obj->type == USEROBJ_TYPE_RPC) {
         *id = obj->data.cnxID;
      } else {
         status = VMK_BAD_PARAM;
      }
   }
   (void) UserObj_Release(uci, obj);

   return status;
}

