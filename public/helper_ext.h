/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * helper_ext.h
 *
 */
#ifndef _HELPER_EXT_H
#define _HELPER_EXT_H

// Defines the helper queue type and the initial number of helper worlds for
// that queue, i.e. <queue_type, num_worlds, PUBLIC_QUEUE|PRIVATE_QUEUE>
// PUBLIC_QUEUE is a COS-accessible queue, while PRIVATE_QUEUE is a
// vmkernel-only queue.
#define HELPER_QUEUE_DEF(def)  \
   def(HELPER_MISC_QUEUE, 2, PUBLIC_QUEUE) \
   def(HELPER_SUSPEND_RESUME_QUEUE, 2, PUBLIC_QUEUE) \
   def(HELPER_FAILOVER_QUEUE, 1, PRIVATE_QUEUE) \
   def(HELPER_PATHEVAL_QUEUE, 1, PRIVATE_QUEUE) 
              
#define HELPER_QTYPE(type, ignore1, ignore2) type,
typedef enum {
   HELPER_INVALID_QUEUE  = -1,
   HELPER_QUEUE_DEF(HELPER_QTYPE)
   HELPER_NUM_QUEUES,
} Helper_QueueType;

/*
 * Helper world request completion handle
 */
typedef int32 Helper_RequestHandle;

/*
 * Opaque token for vmkernel to signal VMnix that a particular request
 * has completed.
 */
typedef struct VMK_COSContext VMK_COSContext;
typedef VMK_COSContext* VMK_WakeupToken;

typedef struct VMnix_SetCOSContextArgs {
   Helper_RequestHandle helperHandle;
   VMK_WakeupToken cosWaiter;
} VMnix_SetCOSContextArgs;


#define HELPER_INVALID_HANDLE (-1)

#endif
