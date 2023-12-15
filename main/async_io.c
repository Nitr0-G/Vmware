/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

#include "vm_types.h"
#include "vm_libc.h"
#include "x86.h"
#include "vmkernel.h"
#include "vm_asm.h"
#include "splock.h"
#include "sched.h"
#include "world.h"
#include "async_io.h"

#define LOGLEVEL_MODULE AsyncIO
#define LOGLEVEL_MODULE_LEN 7
#include "log.h"

#define ASYNC_CALLBACK_FRAME_MAGIC	0x4346 // 'CF'

typedef struct AsyncCallbackFrame {
   uint16               magic;
   uint8                payloadSize;
   uint8                savedCallbackFrameOffset;
   Async_Callback       savedCallback;
   Async_FrameCallback  callback;
} AsyncCallbackFrame;

Async_Token *
Async_AllocToken(uint32 flags)
{
   Async_Token *token = (Async_Token *)Mem_Alloc(sizeof(Async_Token));
   if (token == NULL) {
      return NULL;
   }
#ifdef VMX86_DEBUG
   token->dbgCurr = 0;
#endif
   SP_InitLock("tokenLck", &token->lock, SP_RANK_ASYNC_TOKEN);
   token->refCount = 1;
   token->flags = flags;
   token->originSN = 0;
   token->originHandleID = 0;
   token->cmd = NULL;
   token->callback = NULL;
   token->callbackFrameOffset = 0;
   token->callerPrivateUsed = 0;
   token->startTSC = RDTSC();
   token->issueTSC = 0;
   token->sgList = NULL;
   /* 
    * Set resID to INVALID_WORLD_ID so we'll know if it hasn't been 
    * initialized by the user of the token.
    */
   token->resID = INVALID_WORLD_ID;
   memset(token->result, 0, sizeof(token->result));
   if (World_IsVMMWorld(MY_RUNNING_WORLD)) {
      /*
       * We check the time on VCPU0, which may not be the current VCPU
       * because we send the interrupt only to VCPU0.  Also, since
       * delaySCSICmds is used during bootup, we're likely to be on VCPU0
       * anyway.
       */
      token->startVmTime = CpuSched_VcpuUsageUsec(MY_VMM_GROUP_LEADER);
   } else {
      /*
       * If the world is not VMM, startVmTime is not used, but we assign it
       * to be the current Vcpu time.
       */
      token->startVmTime = CpuSched_VcpuUsageUsec(MY_RUNNING_WORLD);
   }
   return token;
}

void
Async_RefToken(Async_Token *token)
{
   SP_Lock(&token->lock);

   ASSERT(token->refCount > 0);
   token->refCount++;

   SP_Unlock(&token->lock);
}

void
Async_ReleaseToken(Async_Token *token)
{
   Bool freeIt;

   SP_Lock(&token->lock);

   ASSERT(token->refCount > 0);
#ifdef VMX86_DEBUG
   token->dbgList[token->dbgCurr % ASYNC_DBG_SLOTS].refCount = 
      token->refCount;
   token->dbgList[token->dbgCurr % ASYNC_DBG_SLOTS].pcpu = 
      MY_PCPU;
   token->dbgList[token->dbgCurr % ASYNC_DBG_SLOTS].freePC = 
      *(((uint32 *)&token) - 1);
   token->dbgCurr++;
#endif
   token->refCount--;
   freeIt = token->refCount == 0;

   if (freeIt) {
      ASSERT(!(token->flags & ASYNC_WAITER));
   }
   SP_Unlock(&token->lock);

   if (freeIt) {
      /*
       * Supposed to be freed here. 
       */
      if (token->cmd) {
         Mem_Free(token->cmd);
         token->cmd = NULL;
      }
      token->refCount = -99999;
      token->freePC = *(((uint32 *)&token) - 1);
      SP_CleanupLock(&token->lock);
      Mem_Free(token);
   }
}

void
Async_PrepareToWait(Async_Token *token)
{
   SP_Lock(&token->lock);
   token->flags |= ASYNC_WAITER;
   SP_Unlock(&token->lock);
}

void
Async_Wait(Async_Token *token)
{
   ASSERT(token->refCount > 0);
   SP_Lock(&token->lock);

   while (token->flags & ASYNC_WAITER) {
      CpuSched_Wait((uint32)token, CPUSCHED_WAIT_AIO, &token->lock);
      ASSERT(token->refCount > 0);
      SP_Lock(&token->lock);
   }

   SP_Unlock(&token->lock);
}

void
Async_Wakeup(Async_Token *token)
{
   ASSERT(token->refCount > 0);
   SP_Lock(&token->lock);

   if (token->flags & ASYNC_WAITER) {
      token->flags &= ~ASYNC_WAITER;
      CpuSched_Wakeup((uint32)token);
   }

   SP_Unlock(&token->lock);
}

void
Async_WaitForIO(Async_Token *token)
{
   ASSERT(token->refCount > 0);
   SP_Lock(&token->lock);

   while (!(token->flags &
	    (ASYNC_IO_DONE | ASYNC_IO_TIMEDOUT))) {
      token->flags |= ASYNC_WAITER;
      CpuSched_Wait((uint32)token, CPUSCHED_WAIT_AIO, &token->lock);
      ASSERT(token->refCount > 0);
      SP_Lock(&token->lock);
      token->flags &= ~ASYNC_WAITER;
   }

   SP_Unlock(&token->lock);
}

/*
 * Async_IODone()
 *
 *
 *	Set the ASYNC_IO_DONE bit in the token. This
 *	indicates that the command has completed successfully.
 *
 */
void
Async_IODone(Async_Token *token)
{
   ASSERT(token->refCount > 0);
   SP_Lock(&token->lock);

   token->flags |= ASYNC_IO_DONE;
   if (token->flags & ASYNC_WAITER) {
      token->flags &= ~ASYNC_WAITER;
      CpuSched_Wakeup((uint32)token);
   }

   SP_Unlock(&token->lock);
}

/*
 * Async_IOTimedOut()
 *
 *
 *	Set the ASYNC_IO_TIMEDOUT bit in the token. This
 *	indicates that the command has timed out. It may
 *	still be active in the device driver.
 */
void
Async_IOTimedOut(Async_Token *token)
{
   ASSERT(token->refCount > 0);
   SP_Lock(&token->lock);

   token->flags |= ASYNC_IO_TIMEDOUT;
   if (token->flags & ASYNC_WAITER) {
      token->flags &= ~ASYNC_WAITER;
      CpuSched_Wakeup((uint32)token);
   }

   SP_Unlock(&token->lock);
}

/*
 *----------------------------------------------------------------------
 *
 * Async_PushCallbackFrame
 *
 *      Push a new callback frame on the token's callback stack
 *
 * Results: 
 *	Return a pointer to the allocated payload area
 *
 * Side effects:
 *      None
 *	
 *----------------------------------------------------------------------
 */
void *
Async_PushCallbackFrame(Async_Token *token, Async_FrameCallback callback,
			uint8 payloadSize)
{
   uint32 callbackFrameOffset;
   AsyncCallbackFrame *frame;

   ASSERT(token->refCount > 0);
   ASSERT(ASYNC_MAX_PRIVATE <= (1 << 8 * sizeof(frame->payloadSize)));
   ASSERT(ASYNC_MAX_PRIVATE <= (1 << 8 * sizeof(frame->savedCallbackFrameOffset)));

   SP_Lock(&token->lock);

   ASSERT_NOT_IMPLEMENTED(token->callerPrivateUsed + sizeof(AsyncCallbackFrame)
			  + payloadSize <= ASYNC_MAX_PRIVATE);

   callbackFrameOffset = token->callerPrivateUsed;
   frame = (AsyncCallbackFrame *) &token->callerPrivate[callbackFrameOffset];

   frame->magic = ASYNC_CALLBACK_FRAME_MAGIC;
   ASSERT(callback != NULL);
   frame->callback = callback;
   frame->savedCallback = token->callback;
   frame->savedCallbackFrameOffset = token->callbackFrameOffset;
   frame->payloadSize = payloadSize;

   token->callerPrivateUsed += sizeof(AsyncCallbackFrame) + payloadSize;
   token->callback = Async_PopCallbackFrame;
   token->callbackFrameOffset = callbackFrameOffset;
   token->flags |= ASYNC_CALLBACK;

   SP_Unlock(&token->lock);

   return payloadSize ? frame + 1 : NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * Async_PopCallbackFrame
 *
 *      Pop the frame at the top of the token's callback stack and invoke
 *      it. The callback is not allowed to push anything on the callback
 *      stack while executing.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	All the side effects of the callback.
 *
 * Note:
 *      Once all token users have migrated to the push/pop framework, we
 *      can move the frame's callback field to token->callback and replace
 *      all invokations of token->callback() by Async_PopCallbackFrame.
 *----------------------------------------------------------------------
 */
void
Async_PopCallbackFrame(Async_Token *token)
{
   AsyncCallbackFrame *frame;
   AsyncCallbackFrame *data = NULL;

   SP_Lock(&token->lock);

   frame = (AsyncCallbackFrame *)
      &token->callerPrivate[token->callbackFrameOffset];

   ASSERT(token->refCount > 0);
   ASSERT(frame->magic == ASYNC_CALLBACK_FRAME_MAGIC);
   ASSERT(token->flags & ASYNC_CALLBACK);
   /* Some users of callerPrivate[] never cleanup after themselves, so the
    * frame may not be the last piece of data at the end of the private area.
    * ASSERT(token->callbackFrameOffset + sizeof(AsyncCallbackFrame)
    *        + frame->payloadSize == token->callerPrivateUsed);
    */

   /* Krishna : Added to achieve multiple frames  */
   if (frame->payloadSize) {
      data = (AsyncCallbackFrame *)Mem_Alloc(frame->payloadSize);
      memcpy(data, frame + 1, frame->payloadSize);
   }

   token->callerPrivateUsed = token->callbackFrameOffset;
   token->callback = frame->savedCallback;
   token->callbackFrameOffset = frame->savedCallbackFrameOffset;

   SP_Unlock(&token->lock);

   frame->magic = -1;
   //frame->callback(token, frame->payloadSize ? frame + 1 : NULL);
   if (frame->payloadSize) {
      frame->callback(token, data);
      Mem_Free(data);
   }
   else
      frame->callback(token, NULL);
}

/*
 *----------------------------------------------------------------------
 *
 * Async_FreeCallbackFrame
 *
 * Async_PopCallbackFrame calls the frame->callback. In case of multi layered
 * async IO subsytems (like COW), in case of error, Async_PopCallbackFrame still
 * tries to execute the callback(which is not good).
 * Currently, Async_FreeCallbackFrame is a no-op but in future, it has to clear 
 * all the memory Push allocates. 
 *
 * Results: 
 *	Free the memory, if incase allocated dynamically.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
void
Async_FreeCallbackFrame(Async_Token *token) 
{ 
}
