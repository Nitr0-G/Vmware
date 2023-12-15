/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/************************************************************
 *
 *  procDebug.c - UserWorld debugging from the COS through
 *                proc nodes.
 *
 ************************************************************/

#include "vmkernel.h"
#include "world.h"
#include "proc.h"
#include "debug.h"
#include "userProcDebug.h"
#include "user_int.h"
#include "memalloc.h"

#define LOGLEVEL_MODULE UserProcDebug 
#include "userLog.h"

#define USERPROCDEBUG_BUFFER_LENGTH 4096 

/*
 * This is the debug state held inside each cartel for debugging through
 * the proc node. 
 * Here 'in' and 'out' are from the perspective of the vmkernel debugger.
 * That is, the functions in the Debug_CnxFunctions struct expect characters
 * to come in from gdb in the inBuffer and send characters meant for gdb 
 * to the outBuffer.
 *
 * The *gdb* does its reads and writes with UserProcDebug_CartelProcRead / Write
 * and its the *vmkernel debugger* that does the putting/getting with the
 * UserProcDebugPutChar / GetChar
 *
 * The inBuffer.lock is the spinlock on which the getchar function waits
 * for characters to show up in the inBuffer from the proc write handler.
 * The outBuffer.lock is the spinlock which the putchar signals to the 
 * proc read handler that a character(s) is available in the outBuffer.
 *
 * isAlive is used to prevent the read / write handlers from looping
 * forever in case debugging was stopped when they're waiting on the 
 * spinlocks.
 *
 */
typedef struct UserProcDebugBuf {
   unsigned char*       buffer;
   int                  head;
   int                  tail;
   SP_SpinLock          lock;
} UserProcDebugBuf;

typedef struct UserProcDebugState {
   Heap_ID              heapID;
   UserProcDebugBuf     inBuffer;
   UserProcDebugBuf     outBuffer;
   int                  charsRead;
   Proc_Entry           procDebugEntry;
   Bool                 isAlive;
} UserProcDebugState;

static VMK_ReturnStatus UserProcDebugCnxStart(Debug_Context* dbgCtx);
static VMK_ReturnStatus UserProcDebugListeningOn(Debug_Context* dbgCtx, char* desc,
					    int length);
static VMK_ReturnStatus UserProcDebugGetChar(Debug_Context* dbgCtx, unsigned char* ch);
static VMK_ReturnStatus UserProcDebugPutChar(Debug_Context* dbgCtx, unsigned char ch);
static VMK_ReturnStatus UserProcDebugFlush(Debug_Context* dbgCtx);
static VMK_ReturnStatus UserProcDebugCnxStop(Debug_Context* dbgCtx);
static VMK_ReturnStatus UserProcDebugPollChar(Debug_Context* dbgCtx,
					      unsigned char *ch);
static VMK_ReturnStatus UserProcDebugCnxCleanup(Debug_Context *dbgCtx);

static Debug_CnxFunctions UserProcDebugCnxFunctions = {
   UserProcDebugCnxStart,
   UserProcDebugListeningOn,
   UserProcDebugGetChar,
   UserProcDebugPutChar,
   UserProcDebugFlush,
   UserProcDebugCnxStop,
   UserProcDebugPollChar,
   UserProcDebugCnxCleanup
};

/*
 *----------------------------------------------------------------------
 *
 * UserProcDebugWait --
 *
 *      Wait on the spinlock for the specified 'event'.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Puts the world to sleep and wakes up when a corresponding
 *      CpuSched_Wakeup is called.
 *
 *----------------------------------------------------------------------
 */
static void 
UserProcDebugWait(SP_SpinLock *lock)
{
   CpuSched_Wait((uint32)lock, CPUSCHED_WAIT_UW_PROCDEBUG, lock);
   SP_Lock(lock);
}

/*
 *----------------------------------------------------------------------
 *
 * UserProcDebugWakeup --
 *
 *      Wakeup a world on the specified event.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Wakes up any worlds that are waiting on the event.
 *      Releases the spinlock.
 *
 *----------------------------------------------------------------------
 */
static void
UserProcDebugWakeup(SP_SpinLock *lock)
{
   CpuSched_Wakeup((uint32)lock);
}

/*
 *----------------------------------------------------------------------
 *
 * UserProcDebugCnxStart --
 *
 *      Initialize the state for debugging through the proc node
 *      Allocates a chunk of space from the cartel's heap to store the
 *      debug state in and also creates the proc node for debugging the
 *      cartel.
 *
 * Results:
 *      VMK_OK.
 *
 * Side effects:
 *      Debugger state is initialized.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserProcDebugCnxStart(Debug_Context* dbgCtx)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   char procEntryName[20];
   UserProcDebugState* userProcDebugCnxData;

   /*
    * Assume that if cnxData is already set to something, we've already been
    * through this function.
    */
   if (dbgCtx->cnxData != NULL) {
      ASSERT(dbgCtx->functions == &UserProcDebugCnxFunctions);
      return VMK_OK;
   }

   /*
    * Initialize cnxData so that it points to the struct with
    * the in and out buffers.
    */
   userProcDebugCnxData = (UserProcDebugState *)User_HeapAlloc(uci, 
                          sizeof(UserProcDebugState));
   if (userProcDebugCnxData == NULL) {
      return VMK_NO_MEMORY;
   }

   userProcDebugCnxData->inBuffer.buffer = (unsigned char *)User_HeapAlloc(uci,
                                           USERPROCDEBUG_BUFFER_LENGTH);
   if (userProcDebugCnxData->inBuffer.buffer == NULL) {
      User_HeapFree(uci, userProcDebugCnxData);
      return VMK_NO_MEMORY;
   }
   userProcDebugCnxData->outBuffer.buffer = (unsigned char *)User_HeapAlloc(uci,
                                            USERPROCDEBUG_BUFFER_LENGTH);
   if (userProcDebugCnxData->outBuffer.buffer == NULL) {
      User_HeapFree(uci, userProcDebugCnxData->inBuffer.buffer);
      User_HeapFree(uci, userProcDebugCnxData);
      return VMK_NO_MEMORY;
   }
    
   dbgCtx->cnxData = (void*) userProcDebugCnxData;

   /*
    * Save the heap id so that we can free the memory we allocated here in
    * CnxCleanup.
    */
   userProcDebugCnxData->heapID = MY_USER_CARTEL_INFO->heap;

   userProcDebugCnxData->inBuffer.tail = userProcDebugCnxData->inBuffer.head = 0;
   userProcDebugCnxData->outBuffer.tail = userProcDebugCnxData->outBuffer.head = 0; 
   userProcDebugCnxData->charsRead = 0;
   userProcDebugCnxData->isAlive = TRUE;
   
   // Create the proc entry for the cartel under /proc/vmware/uwdebug 
   Proc_InitEntry(&userProcDebugCnxData->procDebugEntry);
   userProcDebugCnxData->procDebugEntry.private = uci;
   userProcDebugCnxData->procDebugEntry.parent = &procDebugDir;
   userProcDebugCnxData->procDebugEntry.read = UserProcDebug_CartelProcRead;
   userProcDebugCnxData->procDebugEntry.write = UserProcDebug_CartelProcWrite;
   userProcDebugCnxData->procDebugEntry.canBlock = TRUE;
   userProcDebugCnxData->procDebugEntry.cyclic = TRUE;
   
   snprintf(procEntryName, sizeof procEntryName, "%d", uci->cartelID);
   Proc_RegisterHidden(&userProcDebugCnxData->procDebugEntry, procEntryName , FALSE);

   SP_InitLock("inbuffer lock", &userProcDebugCnxData->inBuffer.lock,
               UW_SP_RANK_USERPROCDEBUG);
   SP_InitLock("outbuffer lock", &userProcDebugCnxData->outBuffer.lock,
               UW_SP_RANK_USERPROCDEBUG);

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * UserProcDebugListeningOn --
 *
 *      Return a string saying we're listening on the proc node for that
 *      particular cartel.
 *
 * Results:
 *      VMK_OK.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserProcDebugListeningOn(Debug_Context* dbgCtx, char* desc, int length)
{
   User_CartelInfo* uci = MY_USER_CARTEL_INFO;

   snprintf(desc, length, "(hidden) proc node: /proc/vmware/uwdebug/%d", uci->cartelID);
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * UserProcDebugGetChar --
 *
 *      Gets a character stored at the head of the debug state's inbuffer.
 *      If there's no character available yet (that is, the debugger hasn't
 *      yet sent any character to it), it waits until a character becomes
 *      available in the inBuffer.
 *
 * Results:
 *      VMK_OK.
 *
 * Side effects:
 *      ch points to the character at the head of inBuffer.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserProcDebugGetChar(Debug_Context* dbgCtx, unsigned char* ch)
{
   UserProcDebugState* cnxData = (UserProcDebugState*) dbgCtx->cnxData;

   SP_Lock(&cnxData->inBuffer.lock);
   
   // Wait till something gets filled in the inBuffer
   while ((cnxData->inBuffer.head == cnxData->inBuffer.tail) && 
         (cnxData->isAlive == TRUE)) {
      UserProcDebugWait(&cnxData->inBuffer.lock);
   }

   if (cnxData->isAlive == TRUE) {
      *ch = cnxData->inBuffer.buffer[cnxData->inBuffer.tail++];

      if (cnxData->inBuffer.tail == cnxData->inBuffer.head) {
         cnxData->inBuffer.tail = cnxData->inBuffer.head = 0;
      }
   }
   
   /*
    * wake up anyone who's waiting for space in inBuffer.
    * (i.e., the proc write handler)
    */
   UserProcDebugWakeup(&cnxData->inBuffer.lock);

   SP_Unlock(&cnxData->inBuffer.lock);
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * UserProcDebugPutChar --
 *
 *      Puts the character in ch into the tail end of outBuffer 
 *      for the debugger. Returns error if buffer is full.
 *      Also wakes up the any worlds that are waiting for a character
 *      to appear in the outBuffer.
 *
 * Results:
 *      VMK_OK upon success, VMK_LIMIT_EXCEEDED if buffer full.
 *
 * Side effects:
 *      Possibly wakes up a world that's waiting for the character 
 *      (The proc read handler).
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserProcDebugPutChar(Debug_Context* dbgCtx, unsigned char ch)
{
   UserProcDebugState* cnxData = (UserProcDebugState*) dbgCtx->cnxData;

   SP_Lock(&cnxData->outBuffer.lock);

   while ((cnxData->outBuffer.head == USERPROCDEBUG_BUFFER_LENGTH) &&
         (cnxData->isAlive == TRUE)) {
      UserProcDebugWait(&cnxData->outBuffer.lock);
   }

   if (cnxData->isAlive == TRUE) {
      cnxData->outBuffer.buffer[cnxData->outBuffer.head++] = ch;
   }

   /*
    * Wake up anyone who's waiting for characters in the outBuffer
    * (i.e., the proc read handler)
    */
  
   UserProcDebugWakeup(&cnxData->outBuffer.lock);

   SP_Unlock(&cnxData->outBuffer.lock);
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * UserProcDebugFlush --
 *
 *      No-op for proc nodes.
 *
 * Results:
 *      VMK_OK.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserProcDebugFlush(Debug_Context* dbgCtx)
{
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * UserProcDebugPollChar --
 *
 *	No-op.  But it shouldn't be called.
 *
 * Results:
 *      VMK_OK.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserProcDebugPollChar(Debug_Context* dbgCtx, unsigned char *ch)
{
   ASSERT(FALSE);
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * UserProcDebugCnxStop --
 *
 *	No-op.
 *
 * Results:
 *      VMK_OK.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserProcDebugCnxStop(Debug_Context* dbgCtx)
{
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * UserProcDebugCnxCleanup --
 *
 *      Cleans up the data in dbgCtx->cnxData. Cleans up the read and
 *      write locks held by the proc node read and write handlers once
 *      the proc node entry for this cartel has been removed and there
 *      are no helper worlds waiting on the locks.
 *
 * Results:
 *      VMK_OK on success, VMK_BAD_PARAM if debugger state wasnt 
 *      initialized before calling this function.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserProcDebugCnxCleanup(Debug_Context *dbgCtx)
{
   UserProcDebugState *cnxData = (UserProcDebugState*) dbgCtx->cnxData;
   Heap_ID heap;

   if (cnxData == NULL) {
      return VMK_BAD_PARAM;
   }
 
   cnxData->isAlive = FALSE;

   /*
    * Wake up waiters that may be waiting for data.
    */
   SP_Lock(&cnxData->outBuffer.lock);
   UserProcDebugWakeup(&cnxData->outBuffer.lock);
   SP_Unlock(&cnxData->outBuffer.lock);

   SP_Lock(&cnxData->inBuffer.lock);
   UserProcDebugWakeup(&cnxData->inBuffer.lock);
   SP_Unlock(&cnxData->inBuffer.lock);

   /*
    * Remove the proc entry for this cartel.
    *
    * We can't successfully remove the proc node until all the waiters have
    * exited (which they should thanks to the wakeup calls above), but once we
    * have removed the proc node, we're assured that no new waiter can open the
    * proc node and wait.  Thus, after this call, we're free to cleanup our
    * locks and data structures.
    */
   Proc_Remove(&cnxData->procDebugEntry);
 
   SP_CleanupLock(&cnxData->inBuffer.lock);
   SP_CleanupLock(&cnxData->outBuffer.lock);

   heap = cnxData->heapID;
   Heap_Free(heap, cnxData->inBuffer.buffer);
   Heap_Free(heap, cnxData->outBuffer.buffer);
   Heap_Free(heap, cnxData);
   dbgCtx->cnxData = NULL;

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * UserProcDebug_DebugCnxInit --
 *
 *     Initializes the functions required by userworld debugger
 *     to debug through the proc node.
 *
 * Results:
 *      VMK_OK.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserProcDebug_DebugCnxInit(Debug_Context* dbgCtx)
{
   dbgCtx->cnxData = NULL;
   dbgCtx->functions = &UserProcDebugCnxFunctions;
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * UserProcDebug_CartelProcRead --
 *
 *      The cartel proc read handler. This is the handler for the cartel
 *      proc node under /proc/vmware/uwdebug. It copies the contents of
 *      the outBuffer in cnxData to the proc Buffer. If outBuffer is
 *      empty, it waits until there's a character available.      
 *
 * Results:
 *      returns 0 upon success.
 *
 * Side effects:
 *      Resets the outBuffer's index and length values after it has
 *      read the outBuffer contents.
 *
 *----------------------------------------------------------------------
 */
int
UserProcDebug_CartelProcRead(Proc_Entry *entry, char *buffer, int *len)
{
   // Get the private pointer (It points to the uci)
   User_CartelInfo* uci = (User_CartelInfo*) entry->private;
   UserDebug_State dbgState = uci->debugger;
   UserProcDebugState* cnxData = (UserProcDebugState*) dbgState.dbgCtx.cnxData;
   int copyLen;

   SP_Lock(&cnxData->outBuffer.lock);

   // If outBuffer is empty, wait until some character becomes available
   while ((cnxData->outBuffer.head == 0) && (cnxData->isAlive == TRUE)) {
      UserProcDebugWait(&cnxData->outBuffer.lock);
   }

   if (cnxData->isAlive == TRUE) {
   
      /*
       * Keep track of the characters read so far.
       * 
       * XXX: This whole thing is a hack to prevent VMnixProcVMKRead 
       * from returning an EOF to the debugger even when there are 
       * characters copied into the buffer.
       *
       * VMnixProcVMKRead expects characters in the buffer to arrive at
       * offsets that keep getting incrementally bigger depending on the
       * length of data returned in the buffer. So I'm keeping track of the 
       * characters read out so far so that the read handler can find 
       * the characters at the offset it expects. 
       *
       * Once enough characters have been read that the count goes beyond
       * VMNIXPROC_BUF_SIZE, the offset is adjusted in VMnixProcRead
       * so that the characters are read out from the correct offset again.
       */ 
   
      *len = cnxData->charsRead;

      // If we've reached the buffer size limit, reset the length.
      if (*len == VMNIXPROC_BUF_SIZE) {
         *len = cnxData->charsRead = 0;
      }
   
      copyLen = cnxData->outBuffer.head - cnxData->outBuffer.tail;
      if ((*len+copyLen-1) > VMNIXPROC_BUF_SIZE) { 
         copyLen = VMNIXPROC_BUF_SIZE - *len;
     }
   
      memcpy(buffer+*len, cnxData->outBuffer.buffer+cnxData->outBuffer.tail, copyLen);

      *len += copyLen; 
      cnxData->charsRead += copyLen;

      if (copyLen == cnxData->outBuffer.head-cnxData->outBuffer.tail) {
        /*
         * If we copied the entire contents of outBuffer, reset the 
         * outBufferLen and outBufferIndex parameters.
         */
         cnxData->outBuffer.head = cnxData->outBuffer.tail = 0;
      } else {
         cnxData->outBuffer.tail += copyLen;
      }

      /*
       * wake up anyone who's waiting for space in outBuffer.
       * (i.e., putchar)
       */
      UserProcDebugWakeup(&cnxData->outBuffer.lock);
   }
  
   SP_Unlock(&cnxData->outBuffer.lock);
   return 0;

}

/*
 *----------------------------------------------------------------------
 *
 * UserProcDebug_CartelProcWrite --
 *
 *      The cartel proc write handler. This is the handler for the cartel
 *      proc node under /proc/vmware/uwdebug. It copies the contents of 
 *      the proc buffer into the inBuffer of the cartel's debug context.
 *      Also wakes up any worlds waiting on the inBuffer's read spinlock. 
 *
 * Results:
 *      0 upon success. 1 if inBuffer is full.
 *
 * Side effects:
 *      Wakes up any worlds waiting for data in the inBuffer.
 *
 *----------------------------------------------------------------------
 */
int
UserProcDebug_CartelProcWrite(Proc_Entry *entry, char *buffer, int *len)
{
   // Get the private pointer. It points to the uci info
   User_CartelInfo* uci = (User_CartelInfo*) entry->private;
   UserDebug_State dbgState = uci->debugger;
   UserProcDebugState* cnxData = (UserProcDebugState*) dbgState.dbgCtx.cnxData;

   SP_Lock(&cnxData->inBuffer.lock);

   if (strlen(buffer) > USERPROCDEBUG_BUFFER_LENGTH) {
      UWLOG(0, "String too long to fit into proc buffer\n");
      SP_Unlock(&cnxData->inBuffer.lock);
      return 1;
   }
   
   while (((strlen(buffer)+cnxData->inBuffer.head) > USERPROCDEBUG_BUFFER_LENGTH) &&
         cnxData->isAlive == TRUE) {
         UWLOG(1, "inBuffer full\n");
         UserProcDebugWait(&cnxData->inBuffer.lock);
   }
  
   if (cnxData->isAlive == TRUE) {
      memcpy(cnxData->inBuffer.buffer+cnxData->inBuffer.head, buffer, strlen(buffer));
      cnxData->inBuffer.head += strlen(buffer);

      /*
       * wake up anyone who's waiting for data in the inBuffer
       * (i.e., getchar)
       */
      UserProcDebugWakeup(&cnxData->inBuffer.lock);
   }

   SP_Unlock(&cnxData->inBuffer.lock);
   return 0;
} 



