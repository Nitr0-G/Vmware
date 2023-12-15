/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * helper.c - functions to create a helper world and to make requests for
 * it to execute some code.
 */

#include "vm_types.h"
#include "vmkernel.h"
#include "helper.h"
#include "memalloc.h"
#include "sched.h"
#include "splock.h"
#include "world.h"
#include "vm_asm.h"
#include "bh.h"
#include "host.h"
#include "libc.h"
#include "util.h"
#include "post.h"
#include "libc.h"
#include "host.h"

#define LOGLEVEL_MODULE Helper
#include "log.h"


// adding some extra for non-VMM related helper requests
#define MAX_HELPER_REQUESTS (MAX_WORLDS + 16)

#define HELPER_MAX_NUM_QUEUES_SHIFT (8)
#define HELPER_MAX_NUM_QUEUES (1 << HELPER_MAX_NUM_QUEUES_SHIFT)

typedef union HelperHandleKey{
   uint32 handle;
   struct {
      unsigned qType: HELPER_MAX_NUM_QUEUES_SHIFT;
      unsigned reqIndex: (32 - HELPER_MAX_NUM_QUEUES_SHIFT);
   } __attribute__ ((packed)) data;
} HelperHandleKey;

typedef enum {
   HELPER_CALL_FREE,	// request is free and can be allocated
   HELPER_CALL_PENDING, // request is waiting for helper
   HELPER_CALL_ACTIVE,  // request is being processed
   HELPER_CALL_DONE     // request is done, to be freed by a status request 
} HelperCallStatus;

typedef struct HelpRequest {
   Bool isSync;
   union {
      HelperRequestFn *requestFn;
      HelperRequestSyncFn *requestSyncFn;
   } function;
   HelperRequestCancelFn *cancelFn; // cleanup function to be invoked
                                    // if a request is cancelled
   void *requestData;		// Data to pass to helper function
   void *requestResult;		// Result returned by helper function
   int resultSize;		// Size of result
   void *hostResult;		// Place in host to copy result to
   HelperCallStatus callStatus;	   
   VMK_ReturnStatus returnStatus;  // Status returned by helper function
   VMK_WakeupToken cosWaiter;      // wake up this guy in the COS when done

   struct HelpRequest *next;
   uint64 allocTime;		// from TSC
   Identity reqIdentity;	// Identity of requesting world
} HelpRequest;

typedef struct HelperQueue HelperQueue;
typedef struct HelperWorld {
   World_Handle *world;
   HelperQueue *queue;    
   HelpRequest *request;  // protected by queue->requestLock.
   struct HelperWorld *next;
} HelperWorld;

typedef struct HelperInfo {
   World_GroupID worldGroupID;
   HelperWorld helpers[NUM_HELPER_WORLDS];
   uint32 numHelpers;
   SP_SpinLock lock;
} HelperInfo;

static HelperInfo helperInfo;

struct HelperQueue {
   HelpRequest requests[MAX_HELPER_REQUESTS];
   HelpRequest *requestList;
   HelpRequest *requestListTail;
   SP_SpinLock requestLock;
   uint32      numWorlds;
   HelperWorld *helpers;
};

static HelperQueue helperQueues[HELPER_NUM_QUEUES];

#define HELPER_WORLDS(ignore, num, ignore2) num,
static const uint32 helperInitNumWorlds[HELPER_NUM_QUEUES] = 
                               {HELPER_QUEUE_DEF(HELPER_WORLDS)};

/*
 * Calculate the number of COS accessible / vmkernel private queues
 */
#define PUBLIC_QUEUE 1
#define PRIVATE_QUEUE 0
#define HELPER_QUEUE_ACCESS(ignore, ignore2, num) num+
#define HELPER_NUM_PUBLIC_QUEUES \
   (HELPER_QUEUE_DEF(HELPER_QUEUE_ACCESS)0)
#define HELPER_NUM_PRIVATE_QUEUES \
   (HELPER_NUM_QUEUES-HELPER_NUM_PUBLIC_QUEUES)

/*
 * Infrastructure for interrupting VMnix when a HelpRequest 
 * completes. - We basically use a ring buffer that is large enough
 * even for the worst case.
 */
#define HELPER_INTR_BUFSIZE (HELPER_NUM_PRIVATE_QUEUES*MAX_HELPER_REQUESTS+1)
static VMK_WakeupToken completedCommands[HELPER_INTR_BUFSIZE];
volatile int completedHead;
volatile int completedTail;
static SP_SpinLock completedBufferLock;


static void helpFunc(void *);
static Bool HelperPOST(void *, int, SP_SpinLock *, SP_Barrier *);
static void HelperDump(HelperQueue *queue);
static void HelperNotifyVMnix(VMK_WakeupToken cosWaiter);


/*
 *-------------------------------------------------------------------------
 *
 * HelperGetQueue --
 *
 *      Wrapper to get helper queue for the given "qType"
 *
 * Results:
 *      returns the corresponding helper queue.
 *
 * Side effects:
 *      none.
 *
 *-------------------------------------------------------------------------
 */
static INLINE HelperQueue *
HelperGetQueue(Helper_QueueType qType)
{
   ASSERT(qType != HELPER_INVALID_QUEUE);
   ASSERT(qType < HELPER_NUM_QUEUES);
   return(&helperQueues[qType]);
}

/*
 *-------------------------------------------------------------------------
 *
 * HelperGetQType --
 *
 *      Wrapper to get helper queue type for the given "handle"
 *
 * Results:
 *      returns the corresponding helper queue.
 *
 * Side effects:
 *      none.
 *
 *-------------------------------------------------------------------------
 */
static INLINE Helper_QueueType 
HelperGetQType(Helper_RequestHandle handle)
{
   HelperHandleKey key;
   ASSERT(handle != HELPER_INVALID_HANDLE);
   key.handle = handle;
   return(key.data.qType);
}

/*
 *-------------------------------------------------------------------------
 *
 * HelperGetReqIndex --
 *
 *      Wrapper to get request index for the given "handle"
 *
 * Results:
 *      returns the corresponding helper queue.
 *
 * Side effects:
 *      none.
 *
 *-------------------------------------------------------------------------
 */
static INLINE Helper_QueueType 
HelperGetReqIndex(Helper_RequestHandle handle)
{
   HelperHandleKey key;
   ASSERT(handle != HELPER_INVALID_HANDLE);
   key.handle = handle;
   return(key.data.reqIndex);
}

/*
 *-------------------------------------------------------------------------
 *
 * HelperInitQueues --  
 *      
 *      Initialize the various helper queues
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      The helper queues are initialzed.
 *
 *-------------------------------------------------------------------------
 */
void 
HelperInitQueues(void)
{
   int j;

   for (j = 0; j < HELPER_NUM_QUEUES; j++) {
      HelperQueue *queue = HelperGetQueue(j);
      int i;
      char lckName[SPINLOCK_NAME_SIZE];
      snprintf(lckName, sizeof lckName, "helpReq%d", j);
      SP_InitLock(lckName, &queue->requestLock, SP_RANK_BLOCK);

      queue->requestList = NULL;
      queue->requestListTail = NULL;
      queue->helpers = NULL;
      for (i = 0; i < MAX_HELPER_REQUESTS; i++) {
         HelpRequest *request = &queue->requests[i];
         request->callStatus = HELPER_CALL_FREE;
         request->returnStatus = VMK_OK;
         request->next = NULL;
      }
   }
}

/*
 *-------------------------------------------------------------------
 *
 * Helper_AddWorld --
 *
 *      Create a world to service the given "queue"
 *
 * Results:
 *      TRUE on success, FALSE otherwise
 *
 * Side effects:
 *      A new helper world is created.
 *
 *-------------------------------------------------------------------
 */
Bool
Helper_AddWorld(Helper_QueueType qType)
{
   char nameBuf[WORLD_NAME_LENGTH];
   HelperQueue *queue = HelperGetQueue(qType);
   Sched_ClientConfig sched;
   World_InitArgs args;
   HelperWorld *helperWorld;
   World_GroupID worldGroupID;

   SP_Lock(&helperInfo.lock);
   worldGroupID = helperInfo.worldGroupID;
   helperWorld = &helperInfo.helpers[helperInfo.numHelpers];
   ASSERT(helperInfo.numHelpers < NUM_HELPER_WORLDS);
   if (helperInfo.numHelpers >= NUM_HELPER_WORLDS) {
      Warning("cannot created more than %d helper worlds\n",
               NUM_HELPER_WORLDS);
      SP_Unlock(&helperInfo.lock);
      return FALSE;
   }
   helperInfo.numHelpers++;
   SP_Unlock(&helperInfo.lock);

   snprintf(nameBuf, sizeof nameBuf, "helper%d-%d", qType, helperInfo.numHelpers);

   Sched_ConfigInit(&sched, SCHED_GROUP_NAME_HELPER);
   World_ConfigArgs(&args, nameBuf, WORLD_SYSTEM | WORLD_HELPER, 
                    helperInfo.worldGroupID, &sched);
   if (World_New(&args, &helperWorld->world) != VMK_OK) {
      Warning("World_New failed for helper world");
      goto fail;
   } 
  
   helperWorld->queue = queue;

   if (Sched_Add(helperWorld->world, helpFunc, (void *)qType) != VMK_OK) {
      Warning("sched add failed to add helper%d-%d", qType, helperInfo.numHelpers);
      goto fail;
   }

   SP_Lock(&helperInfo.lock);
   // Make sure that world group ID hasn't changed since the last critical section.
   if (worldGroupID != helperInfo.worldGroupID) {
      Warning("Inconsistent helper world group leader ID %d, new leader ID %d", 
              worldGroupID, helperInfo.worldGroupID);
      SP_Unlock(&helperInfo.lock);
      goto fail;
   }
   // Assign world group ID if none exists.
   if (worldGroupID == WORLD_GROUP_DEFAULT) {
      helperInfo.worldGroupID = World_GetGroupLeaderID(helperWorld->world);
   }
   SP_Unlock(&helperInfo.lock);

   SP_Lock(&queue->requestLock);
   queue->numWorlds++;
   ASSERT(helperWorld->next == NULL);
   ASSERT(helperWorld->world);
   helperWorld->next = queue->helpers;
   queue->helpers = helperWorld;
   SP_Unlock(&queue->requestLock);
   return TRUE;

fail:
   SP_Lock(&helperInfo.lock);
   helperInfo.numHelpers--;
   helperWorld->world = NULL;
   helperWorld->queue = NULL;
   helperWorld->next = NULL;
   SP_Unlock(&helperInfo.lock);
   return FALSE;
}

/*
 *----------------------------------------------------------------------
 *
 * Helper_Init --
 *
 *      Initialize the helper   
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
void
Helper_Init(VMnix_SharedData *sharedData)
{
   int i;

   // sanity checks only on debug builds
   if (vmx86_debug) {
      DEBUG_ONLY(HelperHandleKey tmpKey;)
      // make sure that we have sufficient space in the Helper_RequestHandle
      // for all the queues we are planning to support.
      // NOTE: HELPER_NUM_QUEUES can be a maximum of 
      // (HELPER_MAX_NUM_QUEUS - 1) since we reserve -1 for
      // HELPER_INVALID_HANDLE
      ASSERT(HELPER_MAX_NUM_QUEUES > HELPER_NUM_QUEUES);

      uint32 numWorlds = 0;
      for (i = 0; i < HELPER_NUM_QUEUES; i++) {
         numWorlds += helperInitNumWorlds[i];
      }
      ASSERT(numWorlds <= NUM_HELPER_WORLDS);

      // Make sure that the size of the handle in the key matches
      // the size of the Helper_RequestHandle
      ASSERT(sizeof(tmpKey) == sizeof(Helper_RequestHandle));
      ASSERT(sizeof(tmpKey.handle) == sizeof(Helper_RequestHandle));

      // Make sure that number of helper requests is within the limit
      // supported by the size of HelperHandleKey.data.reqIndex field
      ASSERT((1 << (32 - HELPER_MAX_NUM_QUEUES_SHIFT)) > MAX_HELPER_REQUESTS );
   }

   SP_InitLock("helper", &helperInfo.lock, SP_RANK_LEAF);
   helperInfo.numHelpers = 0;
   helperInfo.worldGroupID = WORLD_GROUP_DEFAULT;
   for (i = 0; i < NUM_HELPER_WORLDS; i++) {
      helperInfo.helpers[i].world = NULL;
      helperInfo.helpers[i].queue = NULL;
      helperInfo.helpers[i].request = NULL;
      helperInfo.helpers[i].next = NULL;
   }

   // Initialize the helper queues.
   HelperInitQueues();

   // create helper worlds for these queues
   for (i = 0; i < HELPER_NUM_QUEUES; i++) {
      Helper_QueueType curQType = i;
      int j;
      Bool rtn;
      for (j = 0; j < helperInitNumWorlds[curQType]; j++) {
         rtn = Helper_AddWorld(curQType);
         ASSERT(rtn);
         if (!rtn) {
            Warning("Could not create helper world for qType = %d",
                    curQType);
            return;
         }
      }

      POST_Register("Helper", HelperPOST, (void *)curQType);
   }

   // Initialize the interrupt ring buffer infrastructure
   completedHead = 0;
   completedTail = 0;
   memset(&completedCommands, 0, sizeof(completedCommands));
   SP_InitLock("helperNotifyLock", &completedBufferLock, SP_RANK_LEAF);      
   sharedData->helperBufferLength = HELPER_INTR_BUFSIZE;
   SHARED_DATA_ADD(sharedData->helperBuffer, VMK_WakeupToken*, 
		   completedCommands);
   SHARED_DATA_ADD(sharedData->helperBufferHead, int*, &completedHead);
   SHARED_DATA_ADD(sharedData->helperBufferTail, int*, &completedTail);
}



/*
 *-----------------------------------------------------------------------------
 *
 *  HelperFindWorld --
 *
 *      Find the helper-world handle for the given world. 
 *
 * Results:
 *      HelperWorld pointer if world found.
 *      NULL if world not found or not a helper. 
 *
 * Side effects:
 *      None. 
 *
 *-----------------------------------------------------------------------------
 */

static HelperWorld*
HelperFindWorld(World_Handle *world)
{
   uint16 i;
   SP_Lock(&helperInfo.lock);
   for (i = 0; i < helperInfo.numHelpers; i++) {
      HelperWorld *hw = &helperInfo.helpers[i];
      if (hw->world == world) {
         SP_Unlock(&helperInfo.lock);
         return hw; 
      }
   }
   SP_Unlock(&helperInfo.lock);
   return NULL;
}


/*
 *----------------------------------------------------------------------
 *
 * HelperAllocRequest -
 *
 *      Find an unused request and return a handle to it.
 *	Caller must be holding requestLock.
 *
 * Results: 
 *      Handle to a free helper request, or HELPER_INVALID_HANDLE
 *
 * Side effects:
 *      callStatus field of request block
 *
 *----------------------------------------------------------------------
 */
static Helper_RequestHandle 
HelperAllocRequest(Helper_QueueType qType)
{
   HelpRequest *rl;
   int i;
   HelperQueue *queue = HelperGetQueue(qType);
   HelpRequest *requests = queue->requests;
   HelperHandleKey key;
   
   for (i = 0; i < MAX_HELPER_REQUESTS; i++) {
      rl = &requests[i];
      if (rl->callStatus == HELPER_CALL_FREE) {
	 rl->callStatus = HELPER_CALL_PENDING;
         rl->allocTime = RDTSC();
	 break;
      }
   }
   if (i == MAX_HELPER_REQUESTS) {
      Warning("out of helper requests\n");
      HelperDump(queue);
      ASSERT_BUG_DEBUGONLY(16182, i < MAX_HELPER_REQUESTS);
      return HELPER_INVALID_HANDLE;
   }

   key.data.qType = qType;
   key.data.reqIndex = i;
   return key.handle;
}

/*
 *----------------------------------------------------------------------
 *
 * HelperFreeRequest -
 *
 *      Free the helper request 
 *
 * Results: 
 *      none
 *
 * Side effects:
 *      helper request fields
 *
 *----------------------------------------------------------------------
 */
static void
HelperFreeRequest(HelpRequest *rl)
{
   ASSERT(rl->callStatus != HELPER_CALL_FREE);
   rl->callStatus = HELPER_CALL_FREE;
}

/*
 *----------------------------------------------------------------------
 *
 * HelperFindRequest -
 *
 *      Find and return the helper request identified by handle
 *
 * Results: 
 *      Pointer to helper request. 
 *
 * Side effects:
 *      none
 *
 *----------------------------------------------------------------------
 */
static HelpRequest *
HelperFindRequest(Helper_RequestHandle handle)
{
   HelpRequest *rl;
   HelpRequest *requests;
   HelperQueue *queue;
   uint32 reqIndex;

   queue = HelperGetQueue(HelperGetQType(handle));
   reqIndex = HelperGetReqIndex(handle);

   requests = queue->requests;

   if (reqIndex >= 0 && reqIndex < MAX_HELPER_REQUESTS) {
      rl = &requests[reqIndex];
   } else {
      Warning("invalid handle id %d", handle);
      rl = NULL;
   }
   
   return rl;
}

/*
 *----------------------------------------------------------------------
 *
 * HelperFindRequestHandle -
 *
 *      Find and return the helper request handle corresponding to the request
 *      "hr" in "queue."
 *
 * Results: 
 *      Helper_RequestHandle or INVALID_HELPER_HANDLE. 
 *
 * Side effects:
 *      none
 *
 *----------------------------------------------------------------------
 */

static Helper_RequestHandle 
HelperFindRequestHandle(HelperQueue *queue, 
                        HelpRequest *hr)
{
   HelpRequest *requests = queue->requests;
   HelperHandleKey key;
   uint32 i;
   Helper_QueueType qt;
  
   ASSERT(SP_IsLocked(&queue->requestLock));
   qt = helperQueues - queue;
   if (qt >= 0 && qt < HELPER_NUM_QUEUES) {
      key.data.qType = qt;
   } else {
      return HELPER_INVALID_HANDLE;
   }
   i = requests - hr;
   if (i >= 0 && i < MAX_HELPER_REQUESTS) {
      key.data.reqIndex = i;
      return key.handle;
   } else {
      return HELPER_INVALID_HANDLE;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * HelperDump --
 *
 *      Log the pending and active helper requests
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
HelperDump(HelperQueue *queue)
{
   int i;
   HelpRequest *requests = queue->requests;
   ASSERT(SP_IsLocked(&queue->requestLock));

   Log("Dumping requests at %"FMT64"d", RDTSC());
   for (i = 0; i < MAX_HELPER_REQUESTS; i++) {
      if (requests[i].callStatus != HELPER_CALL_FREE) {
         Log("%d: status=%d func=%p since=%"FMT64"d",
             i, requests[i].callStatus, requests[i].function.requestFn,
             requests[i].allocTime);
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * helpFunc
 *
 *      The main loop of the helper world.  Loops waiting for
 *      helper requests to perform.
 *
 * Results: 
 *      none, never returns
 *
 * Side effects:
 *      lots
 *
 *----------------------------------------------------------------------
 */
static void
helpFunc(void *data)
{
   VMK_ReturnStatus status;
   Helper_QueueType qType = (Helper_QueueType)data;
   HelperQueue *q = HelperGetQueue(qType);
   HelperWorld *hw;

   ASSERT_HAS_INTERRUPTS();
   CpuSched_DisablePreemption();

   hw = HelperFindWorld(MY_RUNNING_WORLD);

   while (TRUE) {
      HelpRequest *rl;
      VMK_WakeupToken cosWaiter;
   
      SP_Lock(&q->requestLock);
      while (q->requestList == NULL) {
	 CpuSched_Wait((uint32)&q->requestLock,
                       CPUSCHED_WAIT_REQUEST,
                       &q->requestLock);
         ASSERT_HAS_INTERRUPTS();
	 SP_Lock(&q->requestLock);
      }
      // dequeue the request
      rl = q->requestList;
      if (rl == q->requestListTail) {
         // last element
         q->requestListTail = NULL;
      }
      q->requestList = rl->next;
      rl->next = NULL;
      ASSERT(rl->callStatus == HELPER_CALL_PENDING);
      rl->callStatus = HELPER_CALL_ACTIVE;
      ASSERT(hw);
      hw->request = rl;
      SP_Unlock(&q->requestLock);

      // Set the identity of the helper world to the identity of the
      // requesting world for this request.
      Identity_Copy(&(MY_RUNNING_WORLD->ident), &rl->reqIdentity);

      /* if a sync request takes too long, the caller might timeout and
       * convert it to an async request */
      if (rl->isSync) {
	 rl->requestResult = NULL;
	 status = rl->function.requestSyncFn(rl->requestData,
					     &rl->requestResult);
      } else {
	 rl->function.requestFn(rl->requestData);
	 status = VMK_OK;
      }

      ASSERT_HAS_INTERRUPTS();
      ASSERT(World_IsSafeToBlock());
      SP_Lock(&q->requestLock);
      hw->request = NULL;
      ASSERT(rl->callStatus == HELPER_CALL_ACTIVE);

      // set the return code
      rl->returnStatus = status;
      cosWaiter = rl->cosWaiter; // Will be NULL for all async requests
      if (rl->isSync) {
	 // For a sync. call hold on to the HelpRequest until the caller
	 // has called Helper_RequestStatus
	 rl->callStatus = HELPER_CALL_DONE;
      } else {
	 ASSERT(NULL == cosWaiter);
	 HelperFreeRequest(rl);
      }
      SP_Unlock(&q->requestLock);

      /*
       * If the caller expects us to get notified when the request is done,
       * do so.
       */
      if (NULL != cosWaiter) {
	 LOG(5,"NotifyVMnix: helpFunc Notifies COS");
	 HelperNotifyVMnix(cosWaiter);
      }

      // run pending bottom-halves
      BH_Check(TRUE);
      ASSERT_HAS_INTERRUPTS();
   }
}

/*
 *----------------------------------------------------------------------
 *
 * Helper_Request -
 *
 *      Make a request for the helper world associated with the given
 *      "qType" to execute the indicated function with the specified data.
 *
 * Results: 
 *      VMK_OK or VMK_NO_FREE_HANDLES if out of helper requests
 *
 * Side effects:
 *      Schedule the request to be executed by the helper world.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
Helper_Request(Helper_QueueType qType, 
               HelperRequestFn *requestFunc, 
               void *requestData)
{
   Helper_RequestHandle handle;
   HelpRequest *rl;
   HelperQueue *q = HelperGetQueue(qType);

   ASSERT_HAS_INTERRUPTS();

   SP_Lock(&q->requestLock);

   // allocate an unused request
   handle = HelperAllocRequest(qType);
   if (handle == HELPER_INVALID_HANDLE) {
      SP_Unlock(&q->requestLock);
      return VMK_NO_FREE_HANDLES;
   }
   rl = HelperFindRequest(handle);
   ASSERT(rl != NULL);

   // set up the request 
   rl->isSync = FALSE;
   rl->function.requestFn = requestFunc;
   rl->requestData = requestData;
   rl->returnStatus = VMK_OK;
   rl->cosWaiter = NULL;
   rl->cancelFn = NULL;
   Identity_Copy(&rl->reqIdentity, &(MY_RUNNING_WORLD->ident));

   rl->next = NULL;
   // enqueue the request
   if (q->requestList == NULL) {
      // empty list, add
      q->requestList = q->requestListTail = rl;
   } else {
      (q->requestListTail)->next = rl;
      q->requestListTail = rl;
   }

   CpuSched_Wakeup((uint32)&q->requestLock);
   SP_Unlock(&q->requestLock);

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Helper_RequestSync -
 *
 *      Make a request for the helper world associated with 
 *      the given "qType" to execute the indicated
 *	function with the specified data.  If the helper function
 *	returns a result, copy that result (with indicated size) to
 *	specified host location.
 *
 * Results: 
 *      a request handle that MUST be used to synchronize
 *      command completion using Helper_RequestStatus at a later time
 *      returns HELPER_INVALID_HANDLE if out of helper requests
 *
 * Side effects:
 *      Schedule the request to be executed by the helper world.
 *
 *----------------------------------------------------------------------
 */
Helper_RequestHandle
Helper_RequestSync(Helper_QueueType qType,
                   HelperRequestSyncFn *requestFunc,
                   void *requestData,
                   HelperRequestCancelFn *cancelFn,
                   int resultSize,
                   void *hostResult)
{
   Helper_RequestHandle handle;
   HelpRequest *rl;
   HelperQueue *q = HelperGetQueue(qType);

   SP_Lock(&q->requestLock);

   // allocate an unused request
   handle = HelperAllocRequest(qType);
   if (handle == HELPER_INVALID_HANDLE) {
      SP_Unlock(&q->requestLock);
      return handle;
   }
   rl = HelperFindRequest(handle);
   ASSERT(rl != NULL);

   // set up the request 
   rl->isSync = TRUE;
   rl->function.requestSyncFn = requestFunc;
   rl->requestData = requestData;
   rl->cancelFn = cancelFn;
   rl->returnStatus = VMK_STATUS_PENDING;
   rl->requestResult = NULL;
   rl->resultSize = resultSize;
   rl->hostResult = hostResult;
   rl->cosWaiter = NULL;
   memcpy(&rl->reqIdentity, &(MY_RUNNING_WORLD->ident), sizeof(Identity));

   rl->next = NULL;
   // enqueue the request
   if (q->requestList == NULL) {
      // empty list, add
      q->requestList = q->requestListTail = rl;
   } else {
      (q->requestListTail)->next = rl;
      q->requestListTail = rl;
   }

   CpuSched_Wakeup((uint32)&q->requestLock);
   SP_Unlock(&q->requestLock);

   return handle;
}


/*
 *----------------------------------------------------------------------
 *
 * Helper_RequestStatus -
 *
 *      Determine the status of the helper request.
 *
 * Results: 
 *      VMK_STATUS_PENDING or VMK_OK
 *
 * Side effects:
 *      request is freed if status is VMK_OK
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Helper_RequestStatus(Helper_RequestHandle handle)
{
   HelpRequest *rl;
   VMK_ReturnStatus status;
   HelperQueue *q;
   Helper_QueueType qType;

   qType = HelperGetQType(handle);
   q = HelperGetQueue(qType);

   rl = HelperFindRequest(handle);
   if (rl == NULL) {
      // caller used an invalid handle
      return VMK_OK;
   }

   // this lookup avoids locking requestLock until
   // after the call has been handled
   if ((rl->callStatus == HELPER_CALL_PENDING) ||
       (rl->callStatus == HELPER_CALL_ACTIVE)) {
      return VMK_STATUS_PENDING;
   }

   SP_Lock(&q->requestLock);
   if ((rl->callStatus == HELPER_CALL_PENDING) ||
       (rl->callStatus == HELPER_CALL_ACTIVE)) {
      ASSERT(rl->returnStatus == VMK_STATUS_PENDING);
      status = VMK_STATUS_PENDING;
   } else if (rl->callStatus == HELPER_CALL_DONE) {
      status = rl->returnStatus;
      ASSERT(status != VMK_STATUS_PENDING);
      if (rl->requestResult != NULL) {
	 CopyToHost(rl->hostResult, rl->requestResult, rl->resultSize);
	 Mem_Free(rl->requestResult);
      }

      rl->cosWaiter = NULL;
      HelperFreeRequest(rl);
   } else {
      ASSERT(rl->callStatus == HELPER_CALL_FREE);
      ASSERT(rl->returnStatus == VMK_OK);
      Warning("called on freed handle");
      status = VMK_OK;
   }
   SP_Unlock(&q->requestLock);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * HelperPOSTFn
 *
 *      Test helper function for POST
 *
 * Results:
 *      VMK_OK
 *
 * Side effects:
 *      *data
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
HelperPOSTFn(void *data, UNUSED_PARAM(void **result))
{
   uint32 *count = (uint32 *) data;

   (*count)++;

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * HelperPOST
 *
 *      Perform a power on test of helper requests
 *
 * Results:
 *      FALSE if error detected, TRUE otherwise
 *
 * Side effects:
 *      
 *
 *----------------------------------------------------------------------
 */
static uint32 helperCount[HELPER_NUM_QUEUES];

Bool
HelperPOST(void *clientData,
           int id,
           UNUSED_PARAM(SP_SpinLock *lock),
           SP_Barrier *barrier)
{
   Helper_RequestHandle *h;
   uint32 i, numRequests;
   int waitCount = 0;   
   Bool preemptible;
   Helper_QueueType qType = (Helper_QueueType)clientData;
   HelperQueue *q = HelperGetQueue(qType);
   HelpRequest *requests = q->requests;

   if (id == 0) {
      ASSERT(qType < HELPER_NUM_QUEUES && qType >= 0);
      helperCount[qType] = 0;
   }

   /* 
    * We have to wait for the helper queue to empty because we fill it up
    * with our own stuff.
    */
   while (1) {
      int i;

      SP_Lock(&q->requestLock);

      for (i = 0; i < MAX_HELPER_REQUESTS; i++) {
	 if (requests[i].callStatus != HELPER_CALL_FREE) {
	    break;
	 }
      }

      SP_Unlock(&q->requestLock);

      if (i != MAX_HELPER_REQUESTS) {
	 waitCount++;
	 if (waitCount > 1) {
	    Warning("Waiting for helper queue to empty on cpu %d", myPRDA.pcpuNum);
	 }
	 CpuSched_Sleep(100);
      } else {
	 break;
      }
   }

   SP_SpinBarrier(barrier);

   // need to disable preemption before calling Helper_Requests
   // otherwise may deadlock on requestLock if get preempted by
   // helpFunc since helper worlds are not preemptible.
   preemptible = CpuSched_DisablePreemption();

   numRequests = MAX_HELPER_REQUESTS/numPCPUs;
   h = (Helper_RequestHandle*)Mem_Alloc(numRequests * sizeof h[0]);
   ASSERT_NOT_IMPLEMENTED(h != NULL);

   // launch the helper requests
   for (i = 0; i < numRequests; i++) {
      h[i] = Helper_RequestSync(qType, HelperPOSTFn, 
                                (void *)&helperCount[qType], NULL, 0, NULL);
   }
   // wait for them to complete
   for (i = 0; i < numRequests; i++) {
      while (TRUE) {
	 if (Helper_RequestStatus(h[i]) == VMK_OK) {
	    break;
	 }
	 CpuSched_YieldThrottled();
      }
   }

   Mem_Free(h);
   CpuSched_RestorePreemption(preemptible);

   SP_SpinBarrier(barrier);

   return (helperCount[qType] == numRequests*numPCPUs);
}


/*
 *----------------------------------------------------------------------
 *
 * Helper_RequestCancel -
 *
 *      Remove the given request from helper queue if the request is
 *      not yet active.
 *      If force is set and the given request is active, turn it into
 *      an asynchronous request.
 *
 * Results: 
 *      VMK_STATUS_PENDING if request is (active && !forcing) || done.
 *      VMK_OK if the request is removed or make async
 *
 * Side effects:
 *      request is freed if it's still pending
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Helper_RequestCancel(Helper_RequestHandle handle, 
                     Bool force)
{
   HelpRequest *rl, *prev;
   VMK_ReturnStatus status = VMK_STATUS_PENDING;
   Helper_QueueType qType;
   HelperQueue *q;
   HelperRequestCancelFn *cancelFn = NULL;
   void *cancelData = NULL;

   qType = HelperGetQType(handle);
   q = HelperGetQueue(qType);

   rl = HelperFindRequest(handle);
   if (rl == NULL) {
      // caller used an invalid handle
      return VMK_INVALID_HANDLE;
   }
   Warning("cancel request handle=%d fn=%p", handle, rl->function.requestFn);
   SP_Lock(&q->requestLock);
   HelperDump(q);
   if (rl->callStatus == HELPER_CALL_PENDING) {
      ASSERT(rl->isSync);
      /* helper world hasn't started processing this request yet,
       * so we can remove it from the list and free it.  */
      ASSERT (q->requestList != NULL);
      if (q->requestList == rl) {
         q->requestList = rl->next;
         prev = NULL;
      } else {
         for (prev = q->requestList; 
              (prev->next != NULL) && (prev->next != rl);
              prev = prev->next) {
         }
         ASSERT (prev->next == rl);
         prev->next = rl->next;
      }
      if (q->requestListTail == rl) {
         q->requestListTail = prev;
      }
      cancelFn = rl->cancelFn;
      cancelData = rl->requestData;
      rl->cosWaiter = NULL;
      HelperFreeRequest(rl);
      status = VMK_OK;
   } else if (force) {
      ASSERT(rl->isSync);
      ASSERT(rl->callStatus != HELPER_CALL_PENDING);
      ASSERT(rl->callStatus != HELPER_CALL_FREE);
      if (rl->callStatus == HELPER_CALL_ACTIVE) {
         // make asynchronous so it will be freed when completed
         SysAlert("making request(%p) async.", rl->function.requestSyncFn);
         rl->isSync = FALSE;
	 rl->cosWaiter = NULL;
         status = VMK_OK;
      }
   }
   SP_Unlock(&q->requestLock);

   // invoke the appropriate cleanup function
   if (cancelFn) {
      ASSERT(cancelData);
      cancelFn(cancelData);
   }
   return status;
}

/*
 *-----------------------------------------------------------------------------
 *
 *  Helper_GetActiveRequestHandle --
 *
 *      Return handle of current helper world's active request.
 *      Must be called only from a helper world's context.
 *
 * Results:
 *      Helper_RequestHandle, INVALID_HANDLE 
 *
 * Side effects:
 *      None. 
 *
 *-----------------------------------------------------------------------------
 */

Helper_RequestHandle
Helper_GetActiveRequestHandle(void)
{
   Helper_RequestHandle rh = HELPER_INVALID_HANDLE;
   HelperWorld *hw;
   HelperQueue *q;

   if (!World_IsHELPERWorld(MY_RUNNING_WORLD)) {
      ASSERT(FALSE);
      return rh;
   }
   hw = HelperFindWorld(MY_RUNNING_WORLD);
   ASSERT(hw);
   if (hw == NULL) {
      return rh;
   }
   q = hw->queue;
   SP_Lock(&q->requestLock);
   if (hw->request) {
      rh = HelperFindRequestHandle(q, hw->request);
   }
   SP_Unlock(&q->requestLock);
   return rh;
}

/*
 *-----------------------------------------------------------------------------
 *
 *  HelperNotifyVMnix --
 *
 *      Internal helper function to put a context in VMnix that needds to be
 *      woken up (specified by "cosWaiter") into the interrupt ring buffer. 
 *      Must be called only from a helper world's context.
 *
 *      Does not cause a VMnix interrupt itself.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      Adds entry to interrupt ring buffer.
 *
 *-----------------------------------------------------------------------------
 */
void
HelperNotifyVMnix(VMK_WakeupToken cosWaiter)
{
   int index, next;

   /*
    * Get a free slot in the notification ring buffer. - We know that we
    * cannot overflow here b/c the buffer is large enough...
    *
    * There could be multiple helper worlds trying to get a slot at the
    * same time, so use locking 
    */
   SP_Lock(&completedBufferLock);

   index = completedHead;
   next = (index+1) % HELPER_INTR_BUFSIZE;

   // make sure we don't overwrite anything
   ASSERT(NULL == completedCommands[index]);

   /*
    * The sequence of these two steps is important 
    * (race w. consumer [VMnix interrupt handler]
    * who doesn't lock).
    */
   completedCommands[index] = cosWaiter;
   completedHead = next;

   SP_Unlock(&completedBufferLock);

   Host_InterruptVMnix(VMNIX_HELPERCOMMAND_COMPLETE);
}


/*
 *-----------------------------------------------------------------------------
 *
 *  Helper_SetCOSContext --
 *
 *      Associates a HelpRequest with a given COS context to create an
 *      interrupt to the VMnix when processing for the specified request
 *      finishes. 
 *
 *      Note that this function has to generate the interrupt itself if the
 *      HelpRequest completed before this function was called.
 *
 * Results:
 *      VMK_INVALID_HANDLE - context specified an invalid request handle
 *      VMK_STATUS_PENDING - operation is still ongoing, interrupt will
 *                           be posted when it is done.
 *      VMK_OK             - operation completed already.
 *
 * Side effects:
 *      Modifies HelpRequest entry
 *      Might cause an interrupt to the VMnix
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
Helper_SetCOSContext(VMnix_SetCOSContextArgs *args)
{
   HelpRequest *rl;
   HelperQueue *q;
   VMK_ReturnStatus status;

   ASSERT(NULL != args);
   q = HelperGetQueue(HelperGetQType(args->helperHandle));
   rl = HelperFindRequest(args->helperHandle);

   if (NULL == rl) {
      /*
       * Invalid handle. - Print a log message
       * Should never happen...
       */
      Log("VMnix specified an unknown request handle: %d", 
	  args->helperHandle);
      ASSERT(0);
      status = VMK_INVALID_HANDLE;
   } else {
      SP_Lock(&q->requestLock);
      /*
       * Set the callback information in the HelpRequest.
       * Do that with the request lock held so that we don't lose any
       * notifications in case we are racing with helpFunc processing
       * the request.
       */
      LOG(4, "Helper_SetCOSContext Associating request (%d) (%p)" 
	  "with wait queue %p", args->helperHandle, rl, 
	  args->cosWaiter);
	  
      rl->cosWaiter = args->cosWaiter;
      switch (rl->callStatus) {
      case HELPER_CALL_PENDING:
      case HELPER_CALL_ACTIVE:
	 /*
	  * Function not done yet, notification will be sent when it 
	  * is done.
	  */
	 status = VMK_STATUS_PENDING;
	 break;
      case HELPER_CALL_DONE:
	 /*
	  * Processing finished before we tried to wait
	  */
	 status = VMK_OK;
	 break;
      case HELPER_CALL_FREE:
      default:
	 /*
	  * This should never happen 
	  */
	 Warning("WaitForVMKernel was called for handle #%d, state: %d\n", 
		 args->helperHandle, rl->callStatus);	     		 
	 ASSERT(0);
	 status = VMK_INVALID_HANDLE;
      }
      SP_Unlock(&q->requestLock);
   }

   return status;
}
