/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/************************************************************
 *
 *  infiniband.c
 *
 *   Infiniband support functions.
 *
 ************************************************************/

#include "vm_types.h"
#include "x86.h"
#include "vm_libc.h"
#include "libc.h"
#include "vmkernel.h"
#include "memalloc.h"
#include "proc.h"
#include "parse.h"
#include "helper.h"
#include "infiniband.h"

#define LOGLEVEL_MODULE Infiniband
#define LOGLEVEL_MODULE_LEN 0
#include "log.h"

Inf_Functions inf_Functions;

/*
 *-----------------------------------------------------------------------------
 *
 * Inf_Close --
 *
 *     Close an Infiniband connection.
 *
 * Results:
 *     VMK_NOT_SUPPORTED if the Infiniband stack isn't loaded.
 *     Status from Infiniband stack otherwise.
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
Inf_Close(struct Inf_Connection *cnx) 
{
   if (inf_Functions.close == NULL) {
      Warning("inf_Functions.close is NULL");
      return VMK_NOT_SUPPORTED;
   } else {
      return inf_Functions.close(cnx);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * Inf_Listen --
 *
 *     Listen for Infiniband connections.
 *
 * Results:
 *     VMK_NOT_SUPPORTED if the Infiniband stack isn't loaded.
 *     Status from Infiniband stack otherwise.
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus 
Inf_Listen(const char *serviceName, Inf_ConnectionCallback cb, void *arg,
	   Inf_ListenToken *listenToken)
{
   if (inf_Functions.listen == NULL) {
      return VMK_NOT_SUPPORTED;
   } else {
      return inf_Functions.listen(serviceName, cb, arg, listenToken);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * Inf_StopListen --
 *
 *     Stop listening for Infiniband connections.
 *
 * Results:
 *     VMK_NOT_SUPPORTED if the Infiniband stack isn't loaded.
 *     Status from Infiniband stack otherwise.
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus 
Inf_StopListen(Inf_ListenToken listenToken)
{
   if (inf_Functions.stopListen == NULL) {
      return VMK_NOT_SUPPORTED;
   } else {
      return inf_Functions.stopListen(listenToken);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * Inf_Connect --
 *
 *     Connect to a service given a gid.
 *
 * Results:
 *     VMK_NOT_SUPPORTED if the Infiniband stack isn't loaded.
 *     Status from Infiniband stack otherwise.
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
Inf_Connect(const char *serviceName, const char *gidName,
	    Inf_ConnectionCallback cb, void *arg)
{
   if (inf_Functions.connect == NULL) {
      Warning("inf_Functions.connect not set");
      return VMK_NOT_SUPPORTED;
   } else {
      return inf_Functions.connect(serviceName, gidName, cb, arg);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * Inf_Send --
 *
 *     Send data on an Infiniband connection.
 *
 * Results:
 *     VMK_NOT_SUPPORTED if the Infiniband stack isn't loaded.
 *     Status from Infiniband stack otherwise.
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus 
Inf_Send(struct Inf_Connection *cnx, uint32 immediate, Bool immediateValid,
         Inf_ScatterGatherArray *sgArr)
{
   if (inf_Functions.send == NULL) {
      Warning("inf_Functions.send not set");
      return VMK_NOT_SUPPORTED;
   } else {
      return inf_Functions.send(cnx, immediate, immediateValid, sgArr);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * Inf_RDMA --
 *
 *     Perform RDMA on an Infiniband connection.
 *
 * Results:
 *     VMK_NOT_SUPPORTED if the Infiniband stack isn't loaded.
 *     Status from Infiniband stack otherwise.
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus 
Inf_RDMA(struct Inf_Connection *cnx, 
	 Inf_ScatterGatherArray *localSGArr,
	 Inf_ScatterGatherArray *remoteSGArr,
	 Inf_Op op)
{
   if (inf_Functions.rdma == NULL) {
      Warning("inf_Functions.rdma not set");
      return VMK_NOT_SUPPORTED;
   } else {
      return inf_Functions.rdma(cnx, localSGArr, remoteSGArr, op);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * Inf_CreateMemRegion --
 *
 *     Create a memory region and get the lkey and rkey for it.
 *
 * Results:
 *     VMK_NOT_SUPPORTED if the Infiniband stack isn't loaded.
 *     Status from Infiniband stack otherwise.
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus 
Inf_CreateMemRegion(void *startAddr, uint32 length, 
		    void **regionToken,
		    Inf_LKey *lkey, Inf_RKey *rkey)
{
   if (inf_Functions.createMemRegion == NULL) {
      Warning("inf_Functions.createMemRegion not set");
      return VMK_NOT_SUPPORTED;
   } else {
      return inf_Functions.createMemRegion(startAddr, length, regionToken,
					   lkey, rkey);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * Inf_DestroyMemRegion --
 *
 *     Destroy a memory region.
 *
 * Results:
 *     VMK_NOT_SUPPORTED if the Infiniband stack isn't loaded.
 *     Status from Infiniband stack otherwise.
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus 
Inf_DestroyMemRegion(void *regionToken)
{
   if (inf_Functions.destroyMemRegion == NULL) {
      Warning("inf_Functions.destroyMemRegion not set");
      return VMK_NOT_SUPPORTED;
   } else {
      return inf_Functions.destroyMemRegion(regionToken);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * Inf_RecvQInit --
 *
 *     Initialize the receive queue for an Infiniband connection.
 *
 * Results:
 *     VMK_NOT_SUPPORTED if the Infiniband stack isn't loaded.
 *     Status from Infiniband stack otherwise.
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus 
Inf_RecvQInit(struct Inf_Connection *cnx, 
	      Inf_ScatterGatherArray **recvSG, 
	      uint32 numRecvSG)
{
   if (inf_Functions.recvQInit == NULL) {
      Warning("inf_Functions.recvQInit not set");
      return VMK_NOT_SUPPORTED;
   } else {
      return inf_Functions.recvQInit(cnx, recvSG, numRecvSG);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * Inf_RecvQAppend --
 *
 *     Append a previously received packet to the connection's receive queue.
 *
 * Results:
 *     VMK_NOT_SUPPORTED if the Infiniband stack isn't loaded.
 *     Status from Infiniband stack otherwise.
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus 
Inf_RecvQAppend(Inf_CompletionTag *tag)
{
   if (inf_Functions.recvQAppend == NULL) {
      Warning("inf_Functions.recvQAppend not set");
      return VMK_NOT_SUPPORTED;
   } else {
      return inf_Functions.recvQAppend(tag);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * Inf_RunAsync --
 *
 *     Run a function in a helper thread.
 *
 * Results:
 *     None.
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
void
Inf_RunAsync(void (*func)(void *), void *arg)
{
   Helper_Request(HELPER_MISC_QUEUE, func, arg);
}
