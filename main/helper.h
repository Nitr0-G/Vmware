/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

#ifndef _VMK_HELPER_H_
#define _VMK_HELPER_H_

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"
#include "helper_ext.h"

/*
 * helper.h --
 *
 *	Initializing and making requests to the helper world
 */

typedef void (HelperRequestFn)(void *clientData);
typedef VMK_ReturnStatus (HelperRequestSyncFn)(void *clientData, void **result);
typedef void (HelperRequestCancelFn)(void *clientData);

extern void Helper_Init(VMnix_SharedData *sharedData);
extern VMK_ReturnStatus Helper_Request(Helper_QueueType qType,
                                       HelperRequestFn *, void *data);
extern Helper_RequestHandle Helper_RequestSync(Helper_QueueType qType,
                                        HelperRequestSyncFn *,
                                        void *data, HelperRequestCancelFn *,
                                        int resultSize, void *hostResult);
extern VMK_ReturnStatus Helper_RequestStatus(Helper_RequestHandle handle);
extern VMK_ReturnStatus Helper_RequestCancel(Helper_RequestHandle handle,
                                             Bool force);
extern VMK_ReturnStatus Helper_SetCOSContext(VMnix_SetCOSContextArgs *args);

extern Helper_RequestHandle Helper_GetActiveRequestHandle(void);

#endif 
