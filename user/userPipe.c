/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * userPipe.c --
 *
 *     Implementation of pipe
 *
 * TODO:
 *     Collect stats on usage
 *     Take advantage of the fact that pipes are only intra-process.
 *     Allocate pipe buf on its own, complete page.
 *     Track statistics on pipes
 */


#include "user_int.h"
#include "userPipe.h"
#include "memalloc.h"
#include "cpusched.h"
#include "semaphore.h"
#include "timer.h"
#include "vmkpoll.h"
#include "userStat.h"
#include "userSocket.h"

#define LOGLEVEL_MODULE UserPipe
#include "userLog.h"

#define PIPE_WAIT_NOTIMEOUT (-1)
#define PIPE_BUFFER_SIZE (512)

typedef struct UserPipe_Buf {
   Semaphore lock;
   Bool hasReader;
   Bool hasWriter;
   uint32 readStart;
   uint32 readLength;
   VMKPollWaitersList readPollWaiters;
   VMKPollWaitersList writePollWaiters;
   User_CartelInfo *readCartel;
   User_CartelInfo *writeCartel;
   int socketInFlight;		  // Used for fd passing.
   uint8 buf[PIPE_BUFFER_SIZE];
} UserPipe_Buf;

/*
 * Used by the wait/wakeup 
 */
typedef enum {
   USER_PIPE_READER = 0,
   USER_PIPE_WRITER = 1,
} UserPipeWaitEvent;

static VMK_ReturnStatus UserPipeWriteNoBlock(UserVAConst* userBuf,
                                             uint32* bufLen,
                                             uint8* pbuf,
                                             const uint32 pbufSize,
                                             const uint32 readStart,
                                             uint32* readLength);
static VMK_ReturnStatus UserPipeReadNoBlock(UserVA* userBuf,
					     uint32* bufLen,
					     const uint8* pbuf,
					     const uint32 pbufSize,
					     uint32* readStart,
					     uint32* readLength);
static void UserPipeCleanup(User_CartelInfo *uci, UserPipe_Buf* pbuf);

static VMK_ReturnStatus UserPipeClose(UserObj* obj, User_CartelInfo* uci);
static VMK_ReturnStatus UserPipeRead(UserObj* obj,
                                     UserVA userBuf,
				     uint64 offset,
				     uint32 bufLen,
				     uint32 *bytesRead);
static VMK_ReturnStatus UserPipeWrite(UserObj* obj,
                                      UserVAConst userBuf,
				      uint64 offset,
				      uint32 bufLen,
				      uint32 *bytesWritten);
static VMK_ReturnStatus UserPipePoll(UserObj* obj,
				     VMKPollEvent inEvents,
				     VMKPollEvent* outEvents,
				     UserObjPollAction action);
static VMK_ReturnStatus UserPipeFcntl(UserObj* obj,
                                      uint32 cmd,
				      uint32 arg);
static VMK_ReturnStatus UserPipeStat(UserObj* obj,
                                     LinuxStat64* statbuf);
static VMK_ReturnStatus UserPipeSendmsg(UserObj *obj,
					LinuxMsgHdr *msg,
					uint32 len,
					uint32 *bytesSent);
static VMK_ReturnStatus UserPipeRecvmsg(UserObj *obj,
					LinuxMsgHdr *msg,
					uint32 len,
					uint32 *bytesRecv);
static VMK_ReturnStatus UserPipeToString(UserObj *obj,
					 char *string,
					 int length);

/* Methods on a pipe */
static UserObj_Methods pipeMethods = USEROBJ_METHODS(
   (UserObj_OpenMethod) UserObj_NotADirectory,
   UserPipeClose,
   UserPipeRead,
   (UserObj_ReadMPNMethod) UserObj_BadParam,
   UserPipeWrite,
   (UserObj_WriteMPNMethod) UserObj_BadParam,
   UserPipeStat,
   (UserObj_ChmodMethod) UserObj_NotImplemented,   // not needed
   (UserObj_ChownMethod) UserObj_NotImplemented,   // not needed
   (UserObj_TruncateMethod) UserObj_NotImplemented,// not needed
   (UserObj_UtimeMethod) UserObj_NotImplemented,   // not needed
   (UserObj_StatFSMethod) UserObj_NotImplemented,  // not needed
   UserPipePoll,
   (UserObj_UnlinkMethod) UserObj_NotADirectory,
   (UserObj_MkdirMethod) UserObj_NotADirectory,
   (UserObj_RmdirMethod) UserObj_NotADirectory,
   (UserObj_GetNameMethod) UserObj_NotADirectory,
   (UserObj_ReadSymLinkMethod) UserObj_NotADirectory,
   (UserObj_MakeSymLinkMethod) UserObj_NotADirectory,
   (UserObj_MakeHardLinkMethod) UserObj_NotADirectory,
   (UserObj_RenameMethod) UserObj_NotADirectory,
   (UserObj_MknodMethod) UserObj_NotADirectory,
   UserPipeFcntl,
   (UserObj_FsyncMethod) UserObj_BadParam,
   (UserObj_ReadDirMethod) UserObj_NotADirectory,
   (UserObj_IoctlMethod) UserObj_BadParam,
   UserPipeToString,
   (UserObj_BindMethod) UserObj_NotASocket,
   (UserObj_ConnectMethod) UserObj_NotASocket,
   (UserObj_SocketpairMethod) UserObj_NotASocket,
   (UserObj_AcceptMethod) UserObj_NotASocket,
   (UserObj_GetSocketNameMethod) UserObj_NotASocket,
   (UserObj_ListenMethod) UserObj_NotASocket,
   (UserObj_SetsockoptMethod) UserObj_NotASocket,
   (UserObj_GetsockoptMethod) UserObj_NotASocket,
   UserPipeSendmsg,
   UserPipeRecvmsg,
   (UserObj_GetPeerNameMethod) UserObj_NotASocket,
   (UserObj_ShutdownMethod) UserObj_NotASocket
);


/*
 *----------------------------------------------------------------------
 *
 * UserPipeLock --
 *
 *	Lock the given pipe buf
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Lock is taken.
 *
 *----------------------------------------------------------------------
 */
static inline void
UserPipeLock(UserPipe_Buf* pbuf)
{
   Semaphore_Lock(&pbuf->lock);
}


/*
 *----------------------------------------------------------------------
 *
 * UserPipeUnlock --
 *
 *	Unlock the given pipe buf.  Best if you've locked it
 *	beforehand.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Lock is released.
 *
 *----------------------------------------------------------------------
 */
static inline void
UserPipeUnlock(UserPipe_Buf* pbuf)
{
   Semaphore_Unlock(&pbuf->lock);
}


/*
 *----------------------------------------------------------------------
 *
 * UserPipeIsLocked --
 *
 *	Check lock status of pbuf.
 *
 * Results:
 *	TRUE if locked, FALSE if not.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static inline Bool
UserPipeIsLocked(UserPipe_Buf* pbuf)
{
   return Semaphore_IsLocked(&pbuf->lock);
}


/*
 *----------------------------------------------------------------------
 *
 * UserPipeWaitAs --
 *
 *	Wait on the given pipe (must be locked) as either a reader or
 * 	writer (waiting for the other).  Returns after timeout, or
 *	when someone broadcasts to the appropriate group on this pipe.
 *	(See UserPipeBroadcastTo()).  Cannot detect spurious wakeups,
 *	caller must deal with that (and tracking any partial usage of
 *	timeout if caller cares).
 *
 * Results:
 *	UserThread_Wait return value
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserPipeWaitAs(UserPipe_Buf* pbuf,
               UserPipeWaitEvent event,
               int timeoutMillis)
{
   const uint32 evId = (uint32)(pbuf) + event;
   const uint32 evReason = (event == USER_PIPE_WRITER)
      ? CPUSCHED_WAIT_UW_PIPEWRITER : CPUSCHED_WAIT_UW_PIPEREADER;
   Timer_RelCycles timeout = 0;

   ASSERT(UserPipeIsLocked(pbuf));
   if (timeoutMillis != PIPE_WAIT_NOTIMEOUT) {
      timeout == Timer_MSToTC(timeoutMillis);
   }

   return UserThread_WaitSema(evId, evReason, &pbuf->lock,
                              timeout, UTWAIT_WITHOUT_PREPARE);
}


/*
 *----------------------------------------------------------------------
 *
 * UserPipeBroadcastTo --
 *
 *	Wakeup anyone waiting for the given event to occur on the
 *	given pipe buf.  See UserPipeWaitAs.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Wakeup any worlds waiting for the given event.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
UserPipeBroadcastTo(User_CartelInfo* uci,
                    UserPipe_Buf* pbuf,
                    UserPipeWaitEvent event)
{
   const uint32 evId = (uint32)(pbuf) + event;
   ASSERT(UserPipeIsLocked(pbuf));
   UserThread_WakeupGroup(uci, evId);
}


/*
 *----------------------------------------------------------------------
 *
 * UserPipe_CreatePipe --
 *
 *	Create and initialized a new pipe object and buffer.
 *
 * Results:
 *	Returns VMK_OK if the buffer was created.  Other if something
 *	went wrong.
 *
 * Side effects:
 *	Pipe is allocated.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserPipe_CreatePipe(User_CartelInfo *readCartel, User_CartelInfo *writeCartel,
		    UserPipe_Buf **pbuf)
{
   UserPipe_Buf* pipeBuf;

   /*
    * If the read and write cartels are the same, then allocate the pipe buffer
    * on the cartel heap, otherwise, use the main heap.
    */
   if (readCartel == writeCartel) {
      pipeBuf = User_HeapAlloc(readCartel, sizeof(UserPipe_Buf));
   } else {
      pipeBuf = Mem_Alloc(sizeof(UserPipe_Buf));
   }

   if (pipeBuf == NULL) {
      *pbuf = NULL;
      return VMK_NO_MEMORY;
   } else {
      memset(pipeBuf, 0, sizeof *pipeBuf);
      Semaphore_Init("User_PipeBuf", &pipeBuf->lock, 1, UW_SEMA_RANK_USERPIPE);
      pipeBuf->hasReader = TRUE;
      pipeBuf->hasWriter = TRUE;
      pipeBuf->readCartel = readCartel;
      pipeBuf->writeCartel = writeCartel;
      VMKPoll_InitList(&pipeBuf->readPollWaiters, NULL);
      VMKPoll_InitList(&pipeBuf->writePollWaiters, NULL);
      *pbuf = pipeBuf;
      return VMK_OK;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * UserPipe_Open --
 *
 *	Create a new pipe object and buffer and two file descriptors
 *	to represent the read and write ends.
 *
 * Results:
 *	Returns VMK_OK if the buffer was created and the descriptors
 *	attached.  Other if something went wrong.  Sets readEnd and
 *	writeEnd to filedescriptors if VMK_OK.  They're undefined for
 *	any other return value.
 *
 * Side effects:
 *	Pipe is allocated, two filedescriptors are allocated.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
UserPipe_Open(User_CartelInfo* uci, int* readEnd, int* writeEnd)
{
   VMK_ReturnStatus status;
   UserPipe_Buf* pipeBuf;

   ASSERT(uci != NULL);
   ASSERT(readEnd != NULL);
   ASSERT(writeEnd != NULL);

   status = UserPipe_CreatePipe(uci, uci, &pipeBuf);
   if (status == VMK_OK) {
      *readEnd = UserObj_FDAdd(uci, USEROBJ_TYPE_PIPEREAD,
                               (UserObj_Data)pipeBuf, &pipeMethods,
                               USEROBJ_OPEN_RDONLY);
      *writeEnd = UserObj_FDAdd(uci, USEROBJ_TYPE_PIPEWRITE,
                                (UserObj_Data)pipeBuf, &pipeMethods,
                                USEROBJ_OPEN_WRONLY);

      if ((*readEnd < 0) || (*writeEnd < 0)) {
         if (*readEnd >= 0) {
            UserObj_FDClose(uci, *readEnd);
         }
         if (*writeEnd >= 0) {
            UserObj_FDClose(uci, *writeEnd);
         }

         /*
          * Either the read or write end (if not both) failed to be
          * fully opened, so at least one end wasn't closed cleanly.
          * Free the pipeBuf forcibly.
          */
         UserPipeCleanup(uci, pipeBuf);

         status = VMK_NO_RESOURCES;
      } else {
         UWLOG(2, "pipe(%d, %d) created at %p",
               *readEnd, *writeEnd, pipeBuf);
      }
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserPipe_Poll --
 *
 *	Check if any data available for reading or space for writing.
 *
 * Results:
 *	VMK_OK if there are bytes available or if there is an error on
 *	the pipe, VMK_WOULD_BLOCK otherwise
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
UserPipe_Poll(UserPipe_Buf* pbuf, UserObj_Type type, VMKPollEvent inEvents,
	      VMKPollEvent* outEvents, UserObjPollAction action)
{
   VMK_ReturnStatus status;

   ASSERT(type == USEROBJ_TYPE_PIPEREAD || type == USEROBJ_TYPE_PIPEWRITE);
   ASSERT(MY_USER_CARTEL_INFO == pbuf->readCartel ||
	  MY_USER_CARTEL_INFO == pbuf->writeCartel);

   status = VMK_OK;

   UserPipeLock(pbuf);
   switch (type) {
   case USEROBJ_TYPE_PIPEREAD:
      if (action == UserObjPollCleanup) {
	 /*
	  * Try to remove ourselves from the waiter list. We may not be on it,
	  * but that's ok.
	  */
         VMKPoll_RemoveWaiter(&pbuf->readPollWaiters,
			      MY_RUNNING_WORLD->worldID);
	 UWLOG(3, "cleaned up waiter on read side");
      } else {
         ASSERT(action == UserObjPollNotify || action == UserObjPollNoAction);

	 /*
	  * Note: we don't cleanly handle all the ways bad parameters can
	  * be passed in.  That's okay, as the VMX is generally good.
	  */

	 if ((!pbuf->hasWriter) && (pbuf->readLength == 0)) {
	    /* Always flag a required WRHUP, regardless of inEvents: */
	    *outEvents |= VMKPOLL_WRHUP;
	 } else if (inEvents & VMKPOLL_WRITE) {
	    /* Any write on this descriptor will return immed ... */
	    *outEvents |= VMKPOLL_WRITE;
	 } else if (inEvents & VMKPOLL_READ) {
	    /*
	     * Return immediate VMKPOLL_READ if bytes are available.  If
	     * no bytes are available and there are still active writers,
	     * then block on this descriptor.
	     */
	    if (pbuf->readLength != 0) {
	       *outEvents |= VMKPOLL_READ;
	    } else if (pbuf->hasWriter) {
	       if (action == UserObjPollNotify) {
		  VMKPoll_AddWaiter(&pbuf->readPollWaiters, 
				    MY_RUNNING_WORLD->worldID);
		  UWLOG(3 , "added waiter for read side");
	       } 
	       status = VMK_WOULD_BLOCK;
	    }
	 } else {
	    status = VMK_WOULD_BLOCK;
	 }
      }
      break;
   case USEROBJ_TYPE_PIPEWRITE:
      if (action == UserObjPollCleanup) {
	 /*
	  * Try to remove ourselves from the waiter list. We may not be on it,
	  * but that's ok.
	  */
         VMKPoll_RemoveWaiter(&pbuf->writePollWaiters,
			      MY_RUNNING_WORLD->worldID);
	 UWLOG(3, "cleaned up waiter on write side");
      } else {
         ASSERT(action == UserObjPollNotify || action == UserObjPollNoAction);

	 if (!pbuf->hasReader) {
	    /* Always flag a required RDHUP, regardless of inEvents: */
	    *outEvents |= VMKPOLL_RDHUP;
	 } else {
	    if (inEvents & VMKPOLL_READ) {
	       /* Any read on this descriptor will return immed ...  */
	       *outEvents |= VMKPOLL_READ;
	    }  else if (inEvents & VMKPOLL_WRITE) {
	       if (pbuf->readLength < PIPE_BUFFER_SIZE) {
		  *outEvents |= VMKPOLL_WRITE;
	       } else {
		  if (action == UserObjPollNotify) {
		     VMKPoll_AddWaiter(&pbuf->writePollWaiters, 
				       MY_RUNNING_WORLD->worldID);
		     UWLOG(3 , "added waiter for write side");
		  }
		  status = VMK_WOULD_BLOCK;
	       }
	    } else {
	       status = VMK_WOULD_BLOCK;
	    }
	 } 
      }
      break;
   default:
      *outEvents = VMKPOLL_INVALID;
      UWWarn("UserPipePoll call on non-pipe object (%d)", type);
   }
   UserPipeUnlock(pbuf);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserPipePoll --
 *
 *	Poll function for object method suite.  Simply wraps call to
 *	UserPipe_Poll.
 *
 * Results:
 *	Same as UserPipe_Poll.
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
UserPipePoll(UserObj* obj, VMKPollEvent inEvents, VMKPollEvent* outEvents,
	     UserObjPollAction action)
{
   return UserPipe_Poll(obj->data.pipeBuf, obj->type, inEvents, outEvents,
			action);
}


/*
 *----------------------------------------------------------------------
 *
 * UserPipeFcntl --
 *
 *	No-op.  All supported, fcntl'able state is handled in the
 *	linux-compat fcntl handler.  (See LinuxfileDesc_Fcntl64.)
 *
 * Results:
 *	VMK_OK
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserPipeFcntl(UserObj* obj,
              uint32 cmd,
              uint32 arg)
{
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserPipeRead --
 *
 *	Object method suite wrapper for UserPipe_Read.
 *
 * Results:
 *	Those of UserPipe_Read.
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserPipeRead(UserObj *obj,
	     UserVA userBuf,
	     UNUSED_PARAM(uint64 offset),
	     const uint32 bufLen,
	     uint32 *bytesRead)
{
   ASSERT(obj->type == USEROBJ_TYPE_PIPEREAD);
   return UserPipe_Read(obj->data.pipeBuf, UserObj_IsOpenForBlocking(obj),
			 userBuf, bufLen, bytesRead);
}


/*
 *----------------------------------------------------------------------
 *
 * UserPipe_Read --
 *
 *	Read upto bufLen bytes from the pbuf.  Will return early if
 *	more than 1 byte has been read, but reading more would block.  
 *
 * Results:
 *	VMK_OK if read succeeded (even a partial read), otherwise if
 *	a user-copy error occurred, or if the pipe was closed.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
UserPipe_Read(UserPipe_Buf *pbuf,	   // IN/OUT
	      Bool canBlock,		   // IN
              UserVA userBuf,              // OUT
              uint32 bufLen,               // IN
              uint32 *bytesRead)           // OUT
{
   VMK_ReturnStatus status = VMK_OK;
   uint32 bytesRemaining = bufLen;
   Bool done = FALSE;
   
   *bytesRead = 0;

   ASSERT(pbuf->readCartel == MY_USER_CARTEL_INFO);

   if (bufLen == 0) {
      return VMK_OK;
   }

   /*
    * Loop until the buffer is full or an error occurs.  Hold the pbuf
    * lock the whole time (unless we block -- wait as will drop the
    * lock).
    */
   UserPipeLock(pbuf);
   while (!done && (status == VMK_OK)) {

      UWLOG(3, "PRE: pbuf=%p(%d+%d), user_buf=%#x(%d/%d)",
            pbuf, pbuf->readStart, pbuf->readLength,
            userBuf, bufLen-bytesRemaining, bufLen);

      status = UserPipeReadNoBlock(&userBuf,
                                   &bytesRemaining,
                                   pbuf->buf, PIPE_BUFFER_SIZE,
                                   &pbuf->readStart,
                                   &pbuf->readLength);
      UWLOG(3, "POST: pbuf=%p(%d+%d), user_buf=%#x(%d/%d)",
            pbuf, pbuf->readStart, pbuf->readLength,
            userBuf, bufLen-bytesRemaining, bufLen);

      if (status == VMK_OK) {
         /* Optimization.  Keep the front of the buffer warm. */
         if (pbuf->readLength == 0) {
            pbuf->readStart = 0;
         }

         /* Probably read at least a byte, wake any waiting writers. */
         /* Somewhat lame that we have two ways of waiting ... */
         UserPipeBroadcastTo(MY_USER_CARTEL_INFO, pbuf, USER_PIPE_WRITER);
         VMKPoll_WakeupAndRemoveWaiters(&pbuf->writePollWaiters);

         /*
          * If we didn't get any bytes, and there are still writers
          * around, wait for at least one byte before returning.
          */
         if ((bufLen == bytesRemaining) && pbuf->hasWriter) {
            if (canBlock) {
               ASSERT(pbuf->readLength == 0);
               /* Releases and re-acquires the pbuf lock: */
               status = UserPipeWaitAs(pbuf, USER_PIPE_READER, PIPE_WAIT_NOTIMEOUT);
               ASSERT(status != VMK_TIMEOUT);
            } else {
               status = VMK_WOULD_BLOCK;
               done = TRUE;
            }
         } else {
            done = TRUE;
         }
      }
   }
   UserPipeUnlock(pbuf);

   *bytesRead = bufLen - bytesRemaining;
   UWSTAT_INSERT(pipeReadSizes, *bytesRead);

   if ((*bytesRead > 0) && (status != VMK_OK)) {
      UWLOG(1, "Read some bytes, so dropping status %s (using VMK_OK)",
            VMK_ReturnStatusToString(status));
      status = VMK_OK;
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserPipeWrite --
 *
 *	Object method suite wrapper for UserPipe_Write.
 *
 * Results:
 *	Those of UserPipe_Write.
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserPipeWrite(UserObj *obj,
	      UserVAConst userBuf,
	      UNUSED_PARAM(uint64 offset),
	      const uint32 bufLen,
	      uint32 *bytesWritten)
{
   ASSERT(obj->type == USEROBJ_TYPE_PIPEWRITE);
   return UserPipe_Write(obj->data.pipeBuf, UserObj_IsOpenForBlocking(obj),
			 userBuf, bufLen, bytesWritten);
}


/*
 *----------------------------------------------------------------------
 *
 * UserPipe_Write --
 *
 *	Write given buf into pbuf.  Will block until all bytes
 * 	have been written, or pipe is closed (or an error occurs).
 *	bytesWritten may not be updated correctly if an error occurs.
 *
 * Results:
 *	VMK_OK if write succeeded, otherwise if an error occurred due
 * 	to the userBuf memory or if all readers close the read end
 *  	of the pipe
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
UserPipe_Write(UserPipe_Buf *pbuf,
	       Bool canBlock,
               UserVAConst userBuf,
               const uint32 bufLen,
               uint32 *bytesWritten)
{
   VMK_ReturnStatus status = VMK_OK;
   uint32 bytesRemaining = bufLen; // we mutate bytesRemaining
   Bool done = FALSE;

   *bytesWritten = 0;

   ASSERT(pbuf->writeCartel == MY_USER_CARTEL_INFO);

   UWSTAT_INSERT(pipeWriteSizes, bufLen);

   if (bufLen == 0) {
      return VMK_OK;
   }

   /*
    * Loop until all bytes are written out, or until
    * an error occurs.
    */
   UserPipeLock(pbuf);
   while (!done && (status == VMK_OK)) {
      ASSERT(bytesRemaining > 0);
      ASSERT(bytesRemaining <= bufLen);

      UWLOG(3, "PRE: pbuf=%p(%d+%d), user_buf=%#x(%d/%d)",
            pbuf, pbuf->readStart, pbuf->readLength,
            userBuf, bufLen-bytesRemaining, bufLen);

      /* If there are no readers then bail. */
      if (!pbuf->hasReader) {
         /* See below for SIGPIPE signal generation */
         status = VMK_BROKEN_PIPE;
      } else {
         /* Copy without blocking: */
         status = UserPipeWriteNoBlock(&userBuf, &bytesRemaining,
                                       pbuf->buf, PIPE_BUFFER_SIZE,
                                       pbuf->readStart, &pbuf->readLength);
         UWLOG(3, "POST: pbuf=%p(%d+%d), user_buf=%#x(%d/%d)",
               pbuf, pbuf->readStart, pbuf->readLength,
               userBuf, bufLen-bytesRemaining, bufLen);
         
         if (status == VMK_OK) {
            /* Probably wrote something, wake any waiting readers */
            /* Somewhat lame that we have two ways of waiting ... */
            UserPipeBroadcastTo(MY_USER_CARTEL_INFO, pbuf, USER_PIPE_READER);
            VMKPoll_WakeupAndRemoveWaiters(&pbuf->readPollWaiters);

            /*
             * If I haven't written everything, have to wait until
             * someone makes some room (or be in non-blocking mode)
             */
            if (bytesRemaining > 0) {
               if (canBlock) {
                  /*
                   * Assert pipe is actually full, or we have an
                   * atomic-sized write that won't fit.
                   */
                  ASSERT((pbuf->readLength == PIPE_BUFFER_SIZE)
                         || ((bytesRemaining <= PIPE_BUFFER_SIZE)
                             && (bytesRemaining >
                                 (PIPE_BUFFER_SIZE - pbuf->readLength))));
                  /* Releases and re-acquires the pbuf lock: */
                  status = UserPipeWaitAs(pbuf, USER_PIPE_WRITER,
                                          PIPE_WAIT_NOTIMEOUT);
                  ASSERT(status != VMK_TIMEOUT);
               } else {
                  status = VMK_WOULD_BLOCK;
                  done = TRUE;
               }
            } else {
               done = TRUE;
            }
         }
      }
   }
   UserPipeUnlock(pbuf);
   
   *bytesWritten = bufLen - bytesRemaining;

   /*
    * If a writer tries to write to a pipe with no readers, we return
    * VMK_BROKEN_PIPE (which will become LINUX_EPIPE).  POSIX mandates
    * that we also send SIGPIPE in this case.
    */
   if (status == VMK_BROKEN_PIPE) {
      VMK_ReturnStatus sigStatus;
      sigStatus = UserSig_LookupAndSend(MY_RUNNING_WORLD->worldID,
                                        LINUX_SIGPIPE, TRUE);
      /* Only fails if the given worldID is bad, by definition its good: */
      ASSERT(sigStatus == VMK_OK);
      *bytesWritten = 0; // anything written will never be read
   }

   if ((*bytesWritten > 0) && (status != VMK_OK)) {
      UWLOG(1, "Wrote some bytes, so dropping status %s (using VMK_OK)",
            VMK_ReturnStatusToString(status));
      status = VMK_OK;
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserPipeCopyChunkIn --
 *
 *	Copy the given chunk of user data in to the given destination
 *	(part of a pipe buffer).  If the copy succeeds, the userBuf,
 *	userBufLen, and readLength are all updated.
 *
 * Results:
 *	VMK_OK if copy succeed, otherwise if there was an error during
 *	the copy.
 *
 * Side effects:
 *	Several IN/OUT parameters are modified, no other side-effects.
 *
 *----------------------------------------------------------------------
 */
static INLINE VMK_ReturnStatus
UserPipeCopyChunkIn(UserVAConst* userBuf,   // IN/OUT: offset into user buf
                    uint32* userBufLen,     // IN/OUT: len of user buf
                    uint8* chunkDest,       // IN: location to write at
                    const uint32 chunkSize, // IN: len to copy in
                    uint32* readLength)     // IN/OUT: readable bytes in buf
{
   VMK_ReturnStatus status;

   ASSERT(chunkSize <= PIPE_BUFFER_SIZE);
   ASSERT(*userBufLen >= chunkSize);
   
   status = User_CopyIn(chunkDest, *userBuf, chunkSize);
   if (status == VMK_OK) {
      *userBuf += chunkSize;
      *userBufLen -= chunkSize;
      *readLength += chunkSize;
   }

   ASSERT(*readLength <= PIPE_BUFFER_SIZE);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserPipeWriteNoBlock --
 *
 * 	If userBufLen is greater than PIPE_BUFFER_SIZE then write as
 *      much of userBuf as will fit into pbuf, otherwise write userBuf
 *      atomically into pbuf (such that it won't get split by other
 *      reads or writes).  Afterwards, either pbuf will be full or the
 *      userBuf will be empty. Does not honor output arguments if an
 *      error is returned.
 *
 * Results:
 *	0 or more bytes of userBuf are copied into pbuf
 *
 * Side effects:
 *	userBuf, bufLen, and readLength are updated to reflect the
 *	data copied into pbuf.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserPipeWriteNoBlock(UserVAConst* userBuf,    // IN/OUT: offset into user buf
                     uint32* userBufLen,      // IN/OUT: len of user buf
                     uint8* pbuf,             // IN: pipe buffer
                     const uint32 pbufSize,   // IN: len of pbuf
                     const uint32 readStart,  // IN: read offset in pbuf
                     uint32* readLength)      // IN/OUT: bytes readable
{
   VMK_ReturnStatus status;
   uint32 chunk1Size, chunk2Size;
   uint32 writeStart = readStart + *readLength;

   ASSERT(*userBufLen > 0); /* == 0 case handled before this point */
   ASSERT(pbuf != NULL);
   ASSERT(pbufSize > 0);
   ASSERT(writeStart < (2*pbufSize));
   ASSERT(readStart < pbufSize);
   ASSERT(*readLength <= pbufSize);

   /*
    * Return immed. if there is insufficient room to write "small"
    * chunks atomically.
    *
    * Note if a writer writes a chunk greater than PIPE_BUFFER_SIZE,
    * they'll obviously block, on the subsequent write of remaining
    * data, they'll jump through this "atomic write" hoop, which isn't
    * specifically necessary.  But, it should be harmless.
    */
   if ((*userBufLen <= PIPE_BUFFER_SIZE)
       && (*userBufLen > (PIPE_BUFFER_SIZE - *readLength))) {
      UWLOG(2, "Atomic write (%d bytes) postponed, insufficient space (%d bytes)",
            *userBufLen, PIPE_BUFFER_SIZE - *readLength);
      return VMK_OK;
   }

   /*
    * At most two writes are required to get all of the given bytes
    * (that will fit) into the buffer without blocking.  The first
    * write is from the end of readable data up to the end of the
    * buffer, the second is from the beginning of the buffer to just
    * before readStart:
    */

   /* |--R++---| : common case: read not far behind */
   if (writeStart < pbufSize) {
      chunk1Size = MIN(*userBufLen,
                       pbufSize - writeStart);
      chunk2Size = MIN(*userBufLen - chunk1Size,
                       readStart);
   }
   /* |++---R++| : less common: read way behind or near end of buffer */
   else {
      writeStart -= pbufSize;
      ASSERT(writeStart < pbufSize);
      ASSERT(readStart >= writeStart);
      chunk1Size = MIN(*userBufLen,
                       readStart - writeStart);
      chunk2Size = 0; /* no 2nd chunk */
   }

   ASSERT(chunk1Size + chunk2Size <= *userBufLen);
   ASSERT(*readLength + chunk1Size + chunk2Size <= pbufSize);
   UWLOG(2, "c1=%d c2=%d", chunk1Size, chunk2Size);

   if (chunk1Size > 0) {
      status = UserPipeCopyChunkIn(userBuf, userBufLen,
                                   pbuf + writeStart, chunk1Size,
                                   readLength);
      ASSERT(*readLength <= pbufSize);

      if ((status == VMK_OK)
          && (chunk2Size > 0)) {
         status = UserPipeCopyChunkIn(userBuf, userBufLen,
                                      pbuf + 0, chunk2Size,
                                      readLength);
         ASSERT(*readLength <= pbufSize);
      }
   } else {
      status = VMK_OK;
   }

   return status;
}

 
/*
 *----------------------------------------------------------------------
 *
 * UserPipeCopyChunkOut --
 *
 *	Copy the given chunk of pipe data out to the given user
 *	destination.  If the copy succeeds, the userBuf, userBufLen,
 *	and readLength are all updated.
 *
 * Results:
 *	VMK_OK if copy succeed, otherwise if there was an error during
 *	the copy.
 *
 * Side effects:
 *	Several IN/OUT parameters are modified, no other side-effects.
 *
 *----------------------------------------------------------------------
 */
static INLINE VMK_ReturnStatus
UserPipeCopyChunkOut(UserVA* userBuf,        // IN/OUT: offset into user buf
                     uint32* userBufLen,     // IN/OUT: len of user buf
                     const uint8* chunkSrc,  // IN: location to write at
                     const uint32 chunkSize, // IN: len to copy in
                     uint32* readStart,      // IN/OUT: read offset in buf
                     uint32* readLength)     // IN/OUT: readable bytes in buf
{
   VMK_ReturnStatus status;

   ASSERT(chunkSize <= PIPE_BUFFER_SIZE);
   ASSERT(*userBufLen >= chunkSize);
   ASSERT(*readLength >= chunkSize);
   
   status = User_CopyOut(*userBuf, chunkSrc, chunkSize);
   if (status == VMK_OK) {
      *userBuf += chunkSize;
      *userBufLen -= chunkSize;
      *readStart += chunkSize;
      *readLength -= chunkSize;
   }

   ASSERT(*readStart <= PIPE_BUFFER_SIZE);
   ASSERT(*readLength <= PIPE_BUFFER_SIZE);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserPipeReadNoBlock --
 *
 *	Read all available, readable bytes from the buffer that will
 *	fit in the given userBuf.  Does not block.
 *
 * Results:
 *	VMK_OK if all worked, otherwise if there was a copyout
 *	problem.
 *
 * Side effects:
 *	Various output parameters will be changed: the
 *	userBuf, userBufLen, readStart, readLength, and bytesRead.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserPipeReadNoBlock(UserVA* userBuf,        // IN/OUT: offset into user buf
                    uint32* userBufLen,     // IN/OUT: len of user buf
                    const uint8* pbuf,      // IN: pipe buffer
                    const uint32 pbufSize,  // IN: len of pbuf
                    uint32* readStart,      // IN/OUT: read offset in pbuf
                    uint32* readLength)     // IN/OUT: readable byte count
{
   VMK_ReturnStatus status;
   uint32 chunk1Size, chunk2Size;

   ASSERT(*userBufLen > 0); /* Handled before this point */
   ASSERT(pbuf != NULL);
   ASSERT(pbufSize > 0);
   ASSERT(*readStart < pbufSize);
   ASSERT(*readLength <= pbufSize);

   /*
    * At most two copies are required to read all the available
    * bytes out of the pbuf: from readStart to end of the pbuf,
    * and from the beginning of the pbuf up to readLength - x
    */
   chunk1Size = MIN(MIN(*userBufLen,
                        *readLength),
                    pbufSize - *readStart);
   chunk2Size = MIN(*userBufLen - chunk1Size,
                    *readLength - chunk1Size);
   
   ASSERT(chunk1Size + chunk2Size <= *userBufLen);
   ASSERT(chunk1Size + chunk2Size <= *readLength);
   
   if (chunk1Size > 0) {
      status = UserPipeCopyChunkOut(userBuf, userBufLen,
                                    pbuf + *readStart, chunk1Size,
                                    readStart, readLength);
      if (*readStart == pbufSize) {
         *readStart = 0;
      }

      if ((status == VMK_OK)
          && (chunk2Size > 0)) {
         ASSERT(*readStart == 0);
         status = UserPipeCopyChunkOut(userBuf, userBufLen,
                                       pbuf + 0, chunk2Size,
                                       readStart, readLength);
      }
   } else {
      status = VMK_OK;
   }
   
   ASSERT(*readStart < pbufSize);
   
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserPipe_Close --
 *
 *	Close a reader or writer side of the given pbuf.  Destroy
 *	pbuf if this is the last reference holder.
 *
 * Results:
 *	pbuf will have one less reader/writer
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
UserPipe_Close(UserPipe_Buf *pbuf, UserObj_Type type)
{
   User_CartelInfo *uci;
   Bool cleanup;

   UserPipeLock(pbuf);
   switch (type) {
   case USEROBJ_TYPE_PIPEREAD:
      ASSERT(pbuf->hasReader);
      pbuf->hasReader = FALSE;
      if (pbuf->hasWriter) {
         UserPipeBroadcastTo(pbuf->writeCartel, pbuf, USER_PIPE_WRITER);
      }
      uci = pbuf->readCartel;
      break;
   case USEROBJ_TYPE_PIPEWRITE:
      ASSERT(pbuf->hasWriter);
      pbuf->hasWriter = FALSE;
      if (pbuf->hasReader) {
         UserPipeBroadcastTo(pbuf->readCartel, pbuf, USER_PIPE_READER);
      }
      uci = pbuf->writeCartel;
      break;
   default:
      ASSERT(FALSE);
      return VMK_BAD_PARAM;
   }
   cleanup = !pbuf->hasReader && !pbuf->hasWriter;
   UserPipeUnlock(pbuf);
   if (cleanup) {
      UserPipeCleanup(uci, pbuf);
   }      
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserPipeClose --
 *
 *	Close function for object method suite.  Simply wraps call to
 *	UserPipe_Close.
 *
 * Results:
 *	pbuf will have one less reader/writer
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
UserPipeClose(UserObj* obj, UNUSED_PARAM(User_CartelInfo* uci))
{
   return UserPipe_Close(obj->data.pipeBuf, obj->type);
}


/*
 *----------------------------------------------------------------------
 *
 * UserPipeStat --
 *
 *      Stat a pipe.
 *
 * Results:
 *      VMK_OK, Data returned in statbuf.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserPipeStat(UserObj* obj, LinuxStat64* statbuf)
{
   Identity* ident = &MY_RUNNING_WORLD->ident;
   UserPipe_Buf* pbuf = obj->data.pipeBuf;

   ASSERT((obj->type == USEROBJ_TYPE_PIPEREAD)
          || (obj->type == USEROBJ_TYPE_PIPEWRITE));
   ASSERT(MY_USER_CARTEL_INFO == pbuf->readCartel ||
	  MY_USER_CARTEL_INFO == pbuf->writeCartel);
   
   memset(statbuf, 0, sizeof(*statbuf));

   statbuf->st_mode = LINUX_MODE_IFIFO;
   if (obj->type == USEROBJ_TYPE_PIPEWRITE) {
      statbuf->st_mode |= LINUX_MODE_IRUSR;
   } else {
      statbuf->st_mode |= LINUX_MODE_IWUSR;
   }

   statbuf->st_blksize = 1024;
   statbuf->st_blocks = PIPE_BUFFER_SIZE / 512;

   // Meaningless.  We just fill in the caller's ids.
   statbuf->st_uid = ident->ruid;
   statbuf->st_gid = ident->rgid;

   UserPipeLock(pbuf);
   statbuf->st_size = pbuf->readLength;
   UserPipeUnlock(pbuf);
   
   // These are wrong.  But we don't expect anyone to look at them.
   {
      uint32 now = (uint32)(Timer_GetTimeOfDay() / (1000*1000LL));
      statbuf->st_atime = now;
      statbuf->st_mtime = now;
      statbuf->st_ctime = now;
   }
   
   // Ignored: st_dev, st_ino32, st_nlink, st_rdev, st_ino.

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserPipe_ToString --
 *
 *	Returns a string representation of this pipe.
 *
 * Results:
 *	VMK_OK.
 *
 * Side-effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserPipe_ToString(UserPipe_Buf *pbuf, char *string, int length)
{
   int len;

   UserPipeLock(pbuf);
   /*
    * Pipes with the same read and write cartels are just normal pipes.  Pipes
    * with different read and write cartels are used as the data transport for
    * unix sockets.
    */
   if (pbuf->readCartel == pbuf->writeCartel) {
      len = snprintf(string, length,
		     "Anon: %p: %s, %s, rdStrt: %d rdLen: %d", pbuf,
		     pbuf->hasReader ? "HsRdr" : "NoRdr",
		     pbuf->hasWriter ? "HsWrtr" : "NoWrtr", pbuf->readStart,
		     pbuf->readLength);
   } else {
      len = snprintf(string, length,
		     "Unix: %p: %s, %s, rdStrt: %d rdLen: %d scktInFlt: %d",
		     pbuf, pbuf->hasReader ? "HsRdr" : "NoRdr",
		     pbuf->hasWriter ? "HsWrtr" : "NoWrtr", pbuf->readStart,
		     pbuf->readLength, pbuf->socketInFlight);
   }
   UserPipeUnlock(pbuf);

   if (len >= length) {
      UWLOG(1, "Description string too long (%d vs %d).  Truncating.", len,
	    length);
   }
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserPipeToString --
 *
 *	Method suite interface for UserPipe_ToString.
 *
 * Results:
 *	See UserPipe_ToString.
 *
 * Side-effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserPipeToString(UserObj *obj, char *string, int length)
{
   return UserPipe_ToString(obj->data.pipeBuf, string, length);
}


/*
 *----------------------------------------------------------------------
 *
 * UserPipe_Sendmsg --
 *
 *	Sends a message over the pipe.
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
UserPipe_Sendmsg(UserPipe_Buf *pbuf, Bool canBlock, LinuxMsgHdr *msg,
		 uint32 len, uint32 *bytesWritten)
{
   VMK_ReturnStatus status;
   LinuxControlMsgHdr *cmsg;
   UserObj *objToPass = NULL;

   ASSERT(msg != NULL);
   ASSERT(MY_USER_CARTEL_INFO == pbuf->writeCartel);

   /*
    * Sending to a specific name is not supported.
    */
   if (msg->name != NULL && msg->nameLen > 0) {
      UWWarn("Sending to a specific socket name not supported.");
      return VMK_BAD_PARAM;
   }

   /*
    * No flags are supported.
    */
   if (msg->flags) {
      UWWarn("No flags are supported. (flags given: %#x)", msg->flags);
      return VMK_BAD_PARAM;
   }

   /*
    * Only one buffer supported.
    */
   if (msg->iovLen != 1) {
      UWWarn("Only one buffer supported. (iovLen given: %d)", msg->iovLen);
      return VMK_BAD_PARAM;
   }

   /*
    * Take care of descriptor passing.
    */
   cmsg = LinuxAPI_CmsgFirstHdr(msg);
   if (cmsg) {
      LinuxFd *fdToPass;
      LinuxFd socket;

      /*
       * We only support passing file descriptors.  If they're trying to do
       * anything else, return an error.
       */
      if (cmsg->length != sizeof(LinuxControlMsgHdr) + sizeof(LinuxFd) ||
          cmsg->level != LINUX_SOCKET_SOL_SOCKET ||
	  cmsg->type != LINUX_SOCKET_SCM_RIGHTS) {
	 UWWarn("Invalid control message. len: %d level: %d type: %d",
	        cmsg->length, cmsg->level, cmsg->type);
	 return VMK_BAD_PARAM;
      }

      /*
       * Make sure we're only trying to pass one file descriptor.
       */
      if (LinuxAPI_CmsgNextHdr(msg, cmsg) != NULL) {
         UWWarn("Only one control message supported per message.");
	 return VMK_BAD_PARAM;
      }

      /*
       * Now retrieve the fd and find its UserObj.
       */
      fdToPass = (LinuxFd*)cmsg->data;
      status = UserObj_Find(pbuf->writeCartel, *fdToPass, &objToPass);
      if (status != VMK_OK) {
         UWLOG(0, "Couldn't find obj for fd %d", *fdToPass);
	 return status;
      }

      /*
       * We only support passing of inet sockets.
       */
      if (objToPass->type != USEROBJ_TYPE_SOCKET_INET) {
         UWWarn("Trying to pass unsupported object type: %d.  Only inet "
	        "sockets are supported.", objToPass->type);
	 (void) UserObj_Release(pbuf->writeCartel, objToPass);
	 return VMK_BAD_PARAM;
      }

      /*
       * Ok, now we know we have a valid inet socket.
       */
      status = UserSocketInet_GetSocket(objToPass, &socket);
      ASSERT(status == VMK_OK);

      UserPipeLock(pbuf);

      /*
       * We only allow one socket to be sent across at one time.
       */
      if (pbuf->socketInFlight != 0) {
         UWLOG(0, "Already a socket in flight.");
	 UserPipeUnlock(pbuf);
	 (void) UserObj_Release(pbuf->writeCartel, objToPass);
	 return VMK_LIMIT_EXCEEDED;
      }

      /*
       * Save the socket.
       */
      pbuf->socketInFlight = socket;

      UserPipeUnlock(pbuf);
   }

   status = UserPipe_Write(pbuf, canBlock, msg->iov[0].base, len, bytesWritten);
   if (status == VMK_OK) {
      if (objToPass) {
         /*
	  * Now that the fd was successfully passed, this side no longer "owns"
	  * the fd and thus is not responsible for closing it.  So mark it as
	  * such.
	  */
	 UserSocketInet_RelinquishOwnership(objToPass);
      }
   } else if (cmsg != NULL) {
      UserPipeLock(pbuf);
      pbuf->socketInFlight = 0;
      UserPipeUnlock(pbuf);
   }

   if (objToPass) {
      (void) UserObj_Release(pbuf->writeCartel, objToPass);
   }

   UWLOG(2, "status: %s  bytesWritten: %d", UWLOG_ReturnStatusToString(status),
	 *bytesWritten);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserPipeSendmsg --
 *
 *	Sendmsg function for object method suite.  Simply wraps call to
 *	UserPipe_Sendmsg.
 *
 * Results:
 *	VMK_OK if okay, otherwise on error.
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
UserPipeSendmsg(UserObj *obj,
		LinuxMsgHdr *msg,
		uint32 len,
		uint32 *bytesSent)
{
   ASSERT(obj->type == USEROBJ_TYPE_PIPEWRITE);
   return UserPipe_Sendmsg(obj->data.pipeBuf, UserObj_IsOpenForBlocking(obj),
			   msg, len, bytesSent);
}


/*
 *----------------------------------------------------------------------
 *
 * UserPipe_Recvmsg --
 *
 *	Receives a message over the pipe.
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
UserPipe_Recvmsg(UserPipe_Buf *pbuf, Bool canBlock, LinuxMsgHdr *msg,
		 uint32 len, uint32 *bytesRead)
{
   VMK_ReturnStatus status;

   ASSERT(msg != NULL);
   ASSERT(MY_USER_CARTEL_INFO == pbuf->readCartel);

   /*
    * Receiving from a specific name is not supported.
    */
   if (msg->name != NULL && msg->nameLen > 0) {
      UWWarn("Receiving from a specific socket name not supported.");
      return VMK_BAD_PARAM;
   }

   /*
    * No flags are supported.
    */
   if (msg->flags) {
      UWWarn("No flags are supported. (flags given: %#x)", msg->flags);
      return VMK_BAD_PARAM;
   }

   /*
    * Only one buffer supported.
    */
   if (msg->iovLen != 1) {
      UWWarn("Only one buffer supported. (iovLen given: %d)", msg->iovLen);
      return VMK_BAD_PARAM;
   }
  
   status = UserPipe_Read(pbuf, canBlock, msg->iov[0].base, len, bytesRead);
   if (status != VMK_OK) {
      return status;
   }

   if (msg->controlLen > 0) {
      LinuxControlMsgHdr *cmsg = NULL;
      LinuxFd newFd = USEROBJ_INVALID_HANDLE;
      UserSocketInet_ObjInfo *socketInfo = NULL;
      UserObj *newObj = NULL;

      UserPipeLock(pbuf);

      /*
       * First we make sure there's actually a socket to be received.
       */
      if (pbuf->socketInFlight == 0) {
         UWLOG(0, "No socket in flight.");
	 goto control_error;
      }

      /*
       * Get the control message header.
       */
      cmsg = LinuxAPI_CmsgFirstHdr(msg);
      if (cmsg == NULL) {
         UWLOG(0, "Couldn't find control message header.");
	 goto control_error;
      }

      /*
       * Make sure they've allocated enough space to store the fd.
       */
      if (msg->controlLen < LinuxAPI_CmsgLen(sizeof(LinuxFd))) {
         UWLOG(0, "control message length too small.");
	 goto control_error;
      }

      /*
       * Reserve an fd in this cartel.
       */
      newFd = UserObj_FDReserve(pbuf->readCartel);
      if (newFd == USEROBJ_INVALID_HANDLE) {
         UWLOG(0, "Unable to reserve fd.");
	 goto control_error;
      }

      /*
       * Allocate memory for new object.
       */
      newObj = User_HeapAlloc(pbuf->readCartel, sizeof *newObj);
      if (newObj == NULL) {
         UWLOG(0, "Can't allocate memory for new object.");
	 goto control_error;
      }

      /*
       * Allocate inet socket info.
       */
      socketInfo = User_HeapAlloc(pbuf->readCartel, sizeof *socketInfo);
      if (socketInfo == NULL) {
         UWLOG(0, "Can't allocate memory for socket info.");
	 goto control_error;
      }

      /*
       * Now initialize the inet object and add it to the fd list..
       */
      UserSocketInet_ObjInit(newObj, socketInfo, pbuf->socketInFlight);
      UserObj_FDAddObj(pbuf->readCartel, newFd, newObj);

      /*
       * Finally, replace the fd for this cartel into the data section of the
       * control message.
       */
      cmsg->length = LinuxAPI_CmsgLen(sizeof(LinuxFd));
      cmsg->level = LINUX_SOCKET_SOL_SOCKET;
      cmsg->type = LINUX_SOCKET_SCM_RIGHTS;
      *(int*)cmsg->data = newFd;

      /*
       * Since we successfully received the socket, it's no longer in flight.
       */
      pbuf->socketInFlight = 0;

      goto control_done;

control_error:
      if (cmsg != NULL) {
         cmsg->length = 0;
	 cmsg->level = 0;
	 cmsg->type = 0;
      }

      if (newFd != USEROBJ_INVALID_HANDLE) {
         UserObj_FDUnreserve(pbuf->readCartel, newFd);
      }
      if (newObj != NULL) {
         User_HeapFree(pbuf->readCartel, newObj);
      }
      if (socketInfo != NULL) {
         User_HeapFree(pbuf->readCartel, socketInfo);
      }
      msg->controlLen = 0;

control_done:
      UserPipeUnlock(pbuf);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserPipeRecvmsg --
 *
 *	Recvmsg function for object method suite.  Simply wraps call to
 *	UserPipe_Recvmsg.
 *
 * Results:
 *	VMK_OK if okay, otherwise on error.
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
UserPipeRecvmsg(UserObj *obj,
		LinuxMsgHdr *msg,
		uint32 len,
		uint32 *bytesSent)
{
   ASSERT(obj->type == USEROBJ_TYPE_PIPEWRITE);
   return UserPipe_Recvmsg(obj->data.pipeBuf, UserObj_IsOpenForBlocking(obj),
			   msg, len, bytesSent);
}


/*
 *----------------------------------------------------------------------
 *
 * UserPipeCleanup --
 *
 *	Cleanup state associated with given pipe, and then free the
 * 	structure.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Releases pbuf
 *
 *----------------------------------------------------------------------
 */
static void
UserPipeCleanup(User_CartelInfo *uci, UserPipe_Buf* pbuf)
{
   UWLOG(1, "freeing pbuf at %p", pbuf);
   if (pbuf->socketInFlight) {
      UserSocketInet_CloseSocket(uci, pbuf->socketInFlight);
   }
   if (vmx86_debug) {
      pbuf->readStart = -1;
      pbuf->readLength = -1;
   }
   UserPipeLock(pbuf);
   if (VMKPoll_HasWaiters(&pbuf->readPollWaiters)) {
      UWWarn("readPollWaiters is not empty!");
   }
   VMKPoll_WakeupAndRemoveWaiters(&pbuf->readPollWaiters);
   
   if (VMKPoll_HasWaiters(&pbuf->writePollWaiters)) {
      UWWarn("writePollWaiters is not empty!");
   }
   VMKPoll_WakeupAndRemoveWaiters(&pbuf->writePollWaiters);
   UserPipeUnlock(pbuf);
   Semaphore_Cleanup(&pbuf->lock);

   if (pbuf->readCartel == pbuf->writeCartel) {
      ASSERT(pbuf->readCartel == uci);
      User_HeapFree(uci, pbuf);
   } else {
      Mem_Free(pbuf);
   }
}
