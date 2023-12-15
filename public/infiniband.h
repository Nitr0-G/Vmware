/**********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * infiniband.h --
 *
 *      Infiniband headers.
 */

#ifndef _INFINIBAND_H_
#define _INFINIBAND_H_

#include "return_status.h"

typedef uint64	Inf_ServiceID;
typedef uint8 Inf_GID[16];

struct Inf_Connection;

typedef uint32 Inf_LKey;
typedef uint32 Inf_RKey;

typedef struct Inf_ScatterGatherElem {
   uint64 		address;
   int32 		length;
} Inf_ScatterGatherElem;

typedef struct Inf_ScatterGatherArray {
   int32			length;
   Inf_LKey			key;
   void				*tag;   
   Inf_ScatterGatherElem	sg[0];
} Inf_ScatterGatherArray;

struct Inf_Config;

typedef enum {
   INF_CNX_STATUS_CONNECTED,
   INF_CNX_STATUS_FAILURE,
   INF_CNX_STATUS_GID_NOT_FOUND,
   INF_CNX_STATUS_SERVICE_ID_NOT_FOUND,
   INF_CNX_STATUS_DISCONNECTED,
} Inf_ConnectionStatus;

typedef enum {
   INF_COMP_OK,
   INF_COMP_ERROR,
   INF_COMP_REQUEST_FLUSHED,
   INF_COMP_LOCAL_PROTECTION_ERROR,
   INF_COMP_LOCAL_ACCESS_ERROR,
   INF_COMP_REMOTE_ACCESS_ERROR,
   INF_COMP_REMOTE_OPERATION_ERROR,
   INF_COMP_RETRY_COUNTER_EXCEEDED,
   INF_COMP_REMOTE_ABORTED,
} Inf_CompletionStatus;

typedef enum {
   INF_OP_SEND,
   INF_OP_RECEIVE,
   INF_OP_RDMA_READ,
   INF_OP_RDMA_WRITE
} Inf_Op;

typedef struct Inf_CompletionTag {
   struct Inf_Connection	*cnx;
   Inf_Op			op;
   void				*tag;
} Inf_CompletionTag;

typedef void (*Inf_ConnectionDestructor)(struct Inf_Connection *cnx, void *arg);
typedef VMK_ReturnStatus (*Inf_IOCallback)(Inf_CompletionStatus status, 
					   Inf_CompletionTag *completionTag,
					   uint32 bytesTransferred, 
					   uint32 immediateData,
					   Bool immediateDataValid);
typedef VMK_ReturnStatus (*Inf_ConnectionCallback)(
   Inf_ConnectionStatus status, 
   void **arg, 
   struct Inf_Connection *cnx,
   Inf_ConnectionDestructor *cnxDestructor,
   void **cnxDestructorArg,
   Inf_IOCallback *ioCallback
);

typedef void *Inf_ListenToken;

//
// Function to allow Infiniband implementation to run a request in 
// a helper thread.
//
void Inf_RunAsync(void (*func)(void *), void *arg);

typedef struct Inf_Functions {
   VMK_ReturnStatus (*listDevices)(void);
   VMK_ReturnStatus (*getPhysMemKeys)(Inf_LKey *lkey, Inf_RKey *rkey);
   VMK_ReturnStatus (*listen)(const char *serviceName,
                              Inf_ConnectionCallback cb,
		              void *cbArg, Inf_ListenToken *listenToken);
   VMK_ReturnStatus (*stopListen)(Inf_ListenToken token);
   VMK_ReturnStatus (*connect)(const char *serviceName, 
			       const char *gidName,
	                       Inf_ConnectionCallback cb, void *cbArg);
   VMK_ReturnStatus (*send)(struct Inf_Connection *cnx, 
			    uint32 immediateData, Bool immediateDataValid,
			    Inf_ScatterGatherArray *sgArr);
   VMK_ReturnStatus (*rdma)(struct Inf_Connection *cnx, 
			    Inf_ScatterGatherArray *localSGArr,
			    Inf_ScatterGatherArray *remoteSGArr,
			    Inf_Op op);
   VMK_ReturnStatus (*createMemRegion)(void *startAddr, uint32 length, 
				       void **regionToken,
				       Inf_LKey *lkey, Inf_RKey *rkey);
   VMK_ReturnStatus (*destroyMemRegion)(void *regionToken);
   VMK_ReturnStatus (*recvQInit)(struct Inf_Connection *cnx, 
				 Inf_ScatterGatherArray **recvSG, uint32 numRecvSG);
   VMK_ReturnStatus (*recvQAppend)(Inf_CompletionTag *tag);
   VMK_ReturnStatus (*close)(struct Inf_Connection *cnx);
} Inf_Functions;

VMK_ReturnStatus Inf_Listen(const char *serviceName, Inf_ConnectionCallback cb, void *arg,
		      Inf_ListenToken *listenToken);
VMK_ReturnStatus Inf_StopListen(Inf_ListenToken token);
VMK_ReturnStatus Inf_Connect(const char *serviceName, const char *destName,
		             Inf_ConnectionCallback cb, void *arg);
VMK_ReturnStatus Inf_Send(struct Inf_Connection *cnx, 
			  uint32 immediateData, Bool immediateDataValid,
			  Inf_ScatterGatherArray *sgArr);
VMK_ReturnStatus Inf_GetPhysMemKeys(Inf_LKey *lkey, Inf_RKey *rkey);
VMK_ReturnStatus Inf_Close(struct Inf_Connection *cnx);
VMK_ReturnStatus Inf_RDMA(struct Inf_Connection *cnx, 
			  Inf_ScatterGatherArray *localSGArr,
			  Inf_ScatterGatherArray *remoteSGArr,
			  Inf_Op op);
VMK_ReturnStatus Inf_CreateMemRegion(void *startAddr, uint32 length, 
				     void **regionToken,
				     Inf_LKey *lkey, Inf_RKey *rkey);
VMK_ReturnStatus Inf_DestroyMemRegion(void *regionToken);
VMK_ReturnStatus Inf_RecvQInit(struct Inf_Connection *cnx, 
			       Inf_ScatterGatherArray **recvSG, 
			       uint32 numRecvSG);
VMK_ReturnStatus Inf_RecvQAppend(Inf_CompletionTag *tag);

extern Inf_Functions inf_Functions;

extern void Infiniband_Init(void);

#endif
