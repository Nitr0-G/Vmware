/* **********************************************************
 * Copyright 2002 VMware, Inc.  All rights reserved. -- VMware Confidential
 * 
 * **********************************************************/
/*
 * vmkevent.c - implements user calls from the vmkernel
 */

#include "vm_types.h"
#include "vmkernel.h"
#include "rpc.h"
#include "world.h"
#include "util.h"
#include "vmkevent.h"
#include "libc.h"
#include "memalloc.h"

#define LOGLEVEL_MODULE VmkEvent
#include "log.h"

typedef struct {
   char         rpcCnxName[RPC_CNX_NAME_LENGTH];
   VmkEventType function;
   uint8        data[RPC_MAX_MSG_LENGTH];
   uint32       dataLen;
   DEBUG_ONLY(Bool assertOnSendFailure;)
} VmkEventMsg;


/*
 *----------------------------------------------------------------------
 *
 * VmkEventPostMsgTimerCB --
 *
 *      Connects to the specified rpc channel, and sends the message.
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
VmkEventPostMsgTimerCB(void *data, UNUSED_PARAM(Timer_AbsCycles timestamp))
{
   VmkEventMsg *msg = (VmkEventMsg *)data;
   VMK_ReturnStatus status;
   RPC_Connection cnx;
   RPC_Token token;

   LOG(1, "EventRPC sending function %d len %d to %s.", msg->function, 
       msg->dataLen, msg->rpcCnxName);

   if ((status = RPC_Connect(msg->rpcCnxName, &cnx)) != VMK_OK) {
      LOG(1, "RPC_Connect failed to find cnx (%s): %#x", msg->rpcCnxName, status);
   } else {
      status = RPC_Send(cnx, msg->function, 0, msg->data, msg->dataLen,
                        UTIL_VMKERNEL_BUFFER, &token);
      RPC_Disconnect(cnx);
      if (status != VMK_OK ) {
         /*
          * This should really never happen.  If it does, there are probably
          * other, more serious problems.  (ie running out of heap, running
          * out of rpc connections).
          */
         LOG(0, "RPC_Send->%s event failed with status %d", msg->rpcCnxName, status);
         ASSERT(!msg->assertOnSendFailure);
      }
   }
   Mem_Free(msg);
}

/*
 *----------------------------------------------------------------------
 *
 * VmkEventPostMsg --
 *
 *      Helper function for posting messages.  Uses a Timer callback +
 *      helper request so that this function can be called from any 
 *      context (ie both Mem_Alloc and Timer_Add only acquire leaf
 *      locks).
 *
 * Results: 
 *      A VMK_ReturnStatus value
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
VmkEventPostMsg(VmkEventType function, void * data, int dataLen,
                Bool assertOnSendFailure, char *fmt, ...)
{
   va_list args;
   VmkEventMsg *msg;

   ASSERT(data && dataLen <= RPC_MAX_MSG_LENGTH);
   dataLen = MIN(dataLen, RPC_MAX_MSG_LENGTH);

   if (!(msg = Mem_Alloc(sizeof *msg))) {
      LOG(0, "Failed to allocate memory for msg %d", function);
      return VMK_NO_MEMORY;
   }

   va_start(args, fmt);
   vsnprintf(msg->rpcCnxName, sizeof msg->rpcCnxName, fmt, args);
   va_end(args);

   msg->function = function;
   msg->dataLen = dataLen;
   memcpy(msg->data, data, msg->dataLen);
   DEBUG_ONLY(msg->assertOnSendFailure = assertOnSendFailure;)
      Timer_Add(MY_PCPU, (Timer_Callback) VmkEventPostMsgTimerCB, 
                0, TIMER_ONE_SHOT, msg);
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * VmkEvent_PostHostAgentMsg --
 *
 *      Send an event to serverd.  Doesn't wait for a reply.
 *
 * Results: 
 *      A VMK_ReturnStatus value
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
VmkEvent_PostHostAgentMsg(VmkEventType function,      // IN: event type
                       void *data,                 // IN: data
                       int dataLen)                // IN: length of data
{
   return VmkEventPostMsg(function, data, dataLen, FALSE, "serverd");
}

/*
 *----------------------------------------------------------------------
 *
 * VmkEvent_PostVmxMsg --
 *
 *      Send an event to vmx.  Doesn't wait for a reply.  The vmmWorldID
 *      parameter must be the id of the vmm world (not the vmx userworld).
 *
 * Results: 
 *      A VMK_ReturnStatus value
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
VmkEvent_PostVmxMsg(World_ID vmmWorldID, 
                    VmkEventType function,      // IN: event type
                    void *data,                 // IN: data
                    int dataLen)                // IN: length of data
{
   World_ID wid;
   World_Handle *w;

   /* 
    * Need to the vmm group leader world id so we can connect to the vmx.
    */

   w = World_Find(vmmWorldID);
   if (!w) {
      WarnVmNotFound(vmmWorldID);
      return VMK_NOT_FOUND;
   }

   if (!World_IsVMMWorld(w)) {
      VmLog(vmmWorldID, "non vmm world id supplied");
      World_Release(w);
      return VMK_BAD_PARAM;
   }
   wid = World_GetVmmLeaderID(w);
   World_Release(w);

   return VmkEventPostMsg(function, data, dataLen, TRUE, "vmkevent.%d", wid);
}


/*
 *----------------------------------------------------------------------
 *
 * VmkEvent_AlertHelper --
 *
 *      Send an alert message to userlevel (serverd)
 *
 *      XXX loglevel module would be more useful than function 
 *      name -- but currently the sole client of this code 
 *      doesn't support loglevels.
 *
 * Results: 
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void 
VmkEvent_AlertHelper(const char *fn, uint32 line, VmkAlertMessage msg, 
                  const char *fmt, ...) 
{
   VmkEvent_Alert arg;
   va_list args;

   arg.lineNumber = line;
   arg.msg = msg;
   strncpy(arg.fnName, fn, sizeof(arg.fnName));
   arg.fnName[sizeof(arg.fnName) - 1] = '\0';
   va_start(args, fmt);
   vsnprintf(arg.messageTxt, sizeof(arg.messageTxt), fmt, args);
   va_end(args);
   VmkEvent_PostHostAgentMsg(VMKEVENT_ALERT, (char *)&arg, sizeof(arg));
   LOG(0, "Received message %d@%s:%d: %s", msg, fn, line, arg.messageTxt);
}
