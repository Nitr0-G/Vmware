/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * vmksysinfo.c -
 *
 *	Handlers for (GET|SET)_VMK_INFO vmnix calls.
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "vmkernel.h"
#include "memalloc.h"
#include "util.h"
#include "host.h"
#include "vmnix_syscall.h"
#include "vmksysinfo.h"
#include "vmksysinfoInt.h"
#include "vsiDefs.h"

#define LOGLEVEL_MODULE VSI
#include "log.h"

#define min(x, y) ((x) < (y))? (x):(y)

// Generate dispatch tables.
#define DECL_SET_HANDLER(funcId, dispatchFunc, type1, args1)                  \
	_##dispatchFunc,

#define DECL_GET_HANDLER(funcId, dispatchFunc, type1, args1, type2, args2)

static SetHandler setDispatchTable[] = {VMKSYSINFO_DISPATCH_TABLE};

#undef DECL_GET_HANDLER
#undef DECL_SET_HANDLER
#define DECL_SET_HANDLER(funcId, dispatchFunc, type1, args1)
#define DECL_GET_HANDLER(funcId, dispatchFunc, type1, args1, type2, args2)    \
	_##dispatchFunc,

static GetHandler getDispatchTable[] = {VMKSYSINFO_DISPATCH_TABLE};

/*
 *----------------------------------------------------------------------------
 *
 *  VSI_NULL --
 *    NULL handler
 *
 *  Returns:
 *    VMK_NOT_FOUND
 *
 *  Side-effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

VMK_ReturnStatus
VSI_NULL(VSI_ParamList *paramList, ...)
{
   return VMK_NOT_FOUND;
}

/*
 *----------------------------------------------------------------------------
 *
 *  VSIWriteToCosBuffers --
 *    Write a buffer of length outBufLen to pages in the Cos referred to by
 *    the outPageDir structure.
 *
 *  Returns:
 *    VMK_ReturnStatus indicating the outcome.
 *
 *  Side-effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static VMK_ReturnStatus
VSIWriteToCosBuffers(PageDirectory *outPageDir,
                     char *buffer,
                     unsigned long outBufLen)
{
   PageDirectory *pageDir = NULL,    /* Temporary to hold the current directory page */
                 *curDirPage = NULL; /* VA in the Cos address space corresponding
                                        to pageDir */

   VMK_ReturnStatus ret = VMK_BAD_PARAM;

   if (!outPageDir || !buffer || !outBufLen) {
      LOG(1, "Bad parameters specified.\n");
      return ret;
   }

   curDirPage = outPageDir;

   /* Copy the pageDirectory from the host and lookup the VAs of pages in
    * the Cos that have to be written into. If we have used up all the pages
    * pointed to by the current directory page, we get the next directory
    * page and repeat the process till we finish writing or we run out of
    * the pages that the Cos had allocated for this call.
    */
   pageDir= Mem_Alloc(sizeof(PageDirectory));
   if (pageDir) {
      while (curDirPage && outBufLen > 0) {
         int i;
         LOG(1, "Using directory page %p\n", curDirPage);
         CopyFromHost(pageDir, curDirPage, sizeof(*pageDir));
         for (i = 0; i < pageDir->numPageEntries && outBufLen > 0; i++) {
            void *page = pageDir->pages[i];
            if (page) {
               unsigned long numBytesWritten = min(PAGE_SIZE, outBufLen);

               CopyToHost(page, buffer, numBytesWritten);
               buffer += numBytesWritten;
               outBufLen -= numBytesWritten;
            } else {
               LOG(0, "Looks Suspicious: NULL page entry at offset %d in dir page"
                   " %p\n", i, pageDir);
               ASSERT(FALSE);
            }
         }
         LOG(2, "Writing numEntries used: %d", i);
         CopyToHost(&curDirPage->numEntriesUsed, &i,
                    sizeof(curDirPage->numEntriesUsed));
         curDirPage = pageDir->nextDirPage;
      }

      if (outBufLen > 0) {
         /* We have run out of buffers in the Cos */
         LOG(0, "Run out of Cos buffers.\n");

         /* The return code should be better than this. It might sometimes be
          * preferable to indicate to the Cos that we had overflowed the
          * buffer. The most appropriate would be an internal error code
          * between the Cos and the vmkernel. Revisit later.
          */
         ret = VMK_NO_MEMORY;
      } else {
         LOG(2, "Write to COS buffers done successfully\n");
         ret = VMK_OK;
      }
   }

   Mem_Free(pageDir);
   return ret;
}



/*
 *-----------------------------------------------------------------------------
 *
 * VSI_GetInfoOld --
 *
 *	Handler for the SYSINFO_GET_OLD vmnix call. Dispatches to the
 *	appropriate handler and copies the result back to the host.
 *      This is here temporarily for backward compatibility until
 *	all the old sysinfo users have been moved over to the new sysinfo.
 *
 * Results:
 *    VMK_BAD_PARAM if an invalid buffer was passed/Return value from the
 *    the dispatcher.
 *
 * Side-effects:
 *    May ASSERT.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
VSI_GetInfoOld(void *inBuf, void *outBuf, int outBufLen)
{
   VMK_ReturnStatus ret = 0;
   int funcId;
   Vmnix_SysInfoOldInfo info;

   ASSERT(outBuf && outBufLen > 0);

   if(!outBuf || outBufLen <= 0) {
      return VMK_BAD_PARAM;
   }

   ASSERT(inBuf);
   if (!inBuf) {
      return VMK_BAD_PARAM;
   }

   CopyFromHost((char *)&info, inBuf, sizeof info);
   funcId = info.funcId;

   ASSERT(funcId > VMKSYSINFO_GET_NONE && funcId < MAX_SYSINFO_GET_DESC);
   if (funcId <= VMKSYSINFO_GET_NONE || funcId >= MAX_SYSINFO_GET_DESC) {
      return VMK_BAD_PARAM;
   }
   funcId -= VMKSYSINFO_GET_NONE + 1;

   if (getDispatchTable[funcId]) {
      char *tmpInBuf = NULL;
      int inBufLen = info.bufLen;

      char *tmpOutBuf = (char *)Mem_Alloc(outBufLen);
      if (!tmpOutBuf) {
	 return VMK_NO_MEMORY;
      }
      if (inBufLen > 0) {
	 tmpInBuf = (char *)Mem_Alloc(inBufLen);
	 if (!tmpInBuf) {
	    Mem_Free(tmpOutBuf);
	    return VMK_NO_MEMORY;
	 }
	 CopyFromHost(tmpInBuf, inBuf + sizeof(info), inBufLen);
      }

      LOG(1, "Dispatching controlDesc 0x%x to %p\n",
	  funcId, getDispatchTable[funcId]);
      ret = getDispatchTable[funcId](tmpInBuf, inBufLen, tmpOutBuf, outBufLen);
      VSIWriteToCosBuffers((PageDirectory *)outBuf, tmpOutBuf,
                           outBufLen);
      Mem_Free(tmpOutBuf);
      if (tmpInBuf) { // XXX might get rid of this if Mem_Free handles NULL.
	 Mem_Free(tmpInBuf);
      }

      return ret;
   }

   // shouldn't happen as we have static tables.
   Log("No entry in getDispatchTable for funcId %d\n", funcId);
   NOT_REACHED();
   return VMK_BAD_PARAM;
}

/*
 *-----------------------------------------------------------------------------
 *
 * VSI_SetInfo --
 *	Handler for the SYSINFO_SET_OLD vmnix call. Dispatches to the
 *	appropriate handler after copying in the buffer from the host.
 *      This is here temporarily for backward compatibility until
 *	all the old sysinfo users have been moved over to the new sysinfo.
 *
 * Results:
 *    VMK_BAD_PARAM if an invalid buffer was passed/The return status from
 *    the dispatch function.
 *
 * Side-effects:
 *    May ASSERT.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
VSI_SetInfoOld(char *inBuf)
{
   VMK_ReturnStatus ret = 0;

   Vmnix_SysInfoOldInfo info;
   int funcId;
   int inBufLen;
   ASSERT(inBuf);
   if (!inBuf) {
      return VMK_BAD_PARAM;
   }
   CopyFromHost(&info, inBuf, sizeof(info));
   funcId = info.funcId;
   inBufLen = info.bufLen;

   ASSERT(funcId > VMKSYSINFO_SET_NONE && funcId < MAX_SYSINFO_SET_DESC);
   if (funcId <= VMKSYSINFO_SET_NONE || funcId >= MAX_SYSINFO_SET_DESC) {
      return VMK_BAD_PARAM;
   }
   funcId -= VMKSYSINFO_SET_NONE + 1;

   if (setDispatchTable[funcId]) {
      char *tmpBuf = NULL;
      if (inBufLen > 0) {
	 tmpBuf = (char *)Mem_Alloc(inBufLen);
	 if (!tmpBuf) {
	    return VMK_NO_MEMORY;
	 }
	 CopyFromHost(tmpBuf, inBuf + sizeof(info), inBufLen);
      }

      LOG(1, "Dispatching controlDesc 0x%x to %p, inBufLen = %d\n",
	  funcId, setDispatchTable[funcId], inBufLen);
      ret = setDispatchTable[funcId](tmpBuf, inBufLen);
      if (tmpBuf) {
	 Mem_Free(tmpBuf);
      }

      return ret;
   }

   // shouldn't land here as we have static tables.
   Log("No entry in setDispatchTable for funcId %d\n", funcId);
   NOT_REACHED();
   return VMK_BAD_PARAM;
}

/*
 *----------------------------------------------------------------------
 *
 * VSIProcessInfo --
 *
 *	Copy the VSI_CallInfo struct from COS pointer, verify the node is a
 *	leaf node, and copy any instance arguemnts from COS.
 *
 * Results:
 *	VMK_ReturnStatus and cloned instance arguments list
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
VSIProcessInfo(void *infoHost, VSI_CallInfo *info, VSI_ParamList **instanceArgs)
{
   VMK_ReturnStatus status;
   VSI_NodeID nodeID;

   ASSERT(infoHost != NULL);
   if (infoHost == NULL) {
      return VMK_BAD_PARAM;
   }

   CopyFromHost(info, infoHost, sizeof *info);
   nodeID = info->nodeID;

   if (info->magic != VSI_CALLINFO_MAGIC) {
      LOG(0, "Magic mismatch for node %d", nodeID);
      return VMK_BAD_PARAM;
   }

   if (!VSI_IsValidNode(nodeID)) {
      LOG(0, "Invalid node %d", nodeID);
      return VMK_NOT_FOUND;
   }

   if (info->nInstanceArgs > 0) {
      if (info->nInstanceArgs > VSI_MAX_INSTANCE_ARGS) {
         LOG(0, "Too many args: %d > %d ", info->nInstanceArgs, VSI_MAX_INSTANCE_ARGS);
         return VMK_BAD_PARAM;
      }

      *instanceArgs = VSI_ParamListCreateFixed(VSI_PARAM_LIST_INSTANCE, 
                                               info->nInstanceArgs);
      if (*instanceArgs == NULL) {
         return VMK_NO_MEMORY;
      }
      status = VSI_ParamListCopyParams(TRUE, *instanceArgs, infoHost + sizeof(*info),
                                       info->nInstanceArgs);
      if (status != VMK_OK) {
         VSI_ParamListDestroy(*instanceArgs);
         *instanceArgs = NULL;
         return status;
      }
   } 

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * VSI_GetInfo --
 *
 *	Entry point into vmkernel for a sysinfo GET call.  Copies stuff
 *	from COS, calls the appropriate handler, copies results back to
 *	COS, and returns the error code from the handler
 *
 * Results:
 *	VMK_ReturnStatus from the handler, or other error code
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
VSI_GetInfo(void *infoHost, void *outBuf, int outBufLen)
{
   VMK_ReturnStatus status;
   VSI_NodeID nodeID;
   VSI_CallInfo info;
   void *tmpOutBuf = NULL;
   VSI_ParamList *instanceArgs = NULL;
   VSI_GetHandler handler;

   if((outBuf == NULL) || (outBufLen <= 0)) {
      return VMK_BAD_PARAM;
   }

   status = VSIProcessInfo(infoHost, &info, &instanceArgs);
   if (status != VMK_OK) {
      LOG(0, "VSIProcessInfo returned %#x", status);
      return status;
   }
   nodeID = info.nodeID;

   if (!VSI_IsLeafNode(nodeID)) {
      LOG(0, "Node %d isn't leaf", nodeID);
      status = VMK_IS_A_DIRECTORY;
      goto done;
   }

   handler = VSI_NodeGetHandler(nodeID);
   if (handler == NULL) {
      LOG(0, "Missing get handler for node %d", nodeID);
      status = VMK_READ_ERROR;
      goto done;
   }

   if (VSI_GetOutputSize(nodeID) != outBufLen) {
      status = VMK_CHECKSUM_MISMATCH;
      goto done;
   }

   tmpOutBuf = Mem_Alloc(outBufLen);
   if (tmpOutBuf == NULL) {
      status = VMK_NO_MEMORY;
      goto done;
   }

   LOG(1, "Dispatching GET handler %p for node %d",
       handler, nodeID);
   status = handler(nodeID, instanceArgs, tmpOutBuf);
   CopyToHost(outBuf, tmpOutBuf, outBufLen);
   Mem_Free(tmpOutBuf);

done:
   if (instanceArgs != NULL) {
      VSI_ParamListDestroy(instanceArgs);
   }

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * VSI_SetInfo --
 *
 *	Entry point into vmkernel for a sysinfo SET call.  Copies stuff
 *	from COS, calls the appropriate handler, and returns the error code
 *	from the handler
 *
 * Results:
 *	VMK_ReturnStatus from the handler, or other error code
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
VSI_SetInfo(void *infoHost, void *inputArgsHost)
{
   VMK_ReturnStatus status;
   VSI_NodeID nodeID;
   VSI_CallInfo info;
   VSI_ParamList *instanceArgs = NULL;
   VSI_ParamList *inputArgs = NULL;
   VSI_SetHandler handler;

   status = VSIProcessInfo(infoHost, &info, &instanceArgs);
   if (status != VMK_OK) {
      LOG(0, "VSIProcessInfo returned %#x", status);
      return status;
   }
   nodeID = info.nodeID;

   if (!VSI_IsLeafNode(nodeID)) {
      LOG(0, "Node %d isn't leaf", nodeID);
      status = VMK_IS_A_DIRECTORY;
      goto done;
   }

   handler = VSI_NodeSetHandler(nodeID);
   if (handler == NULL) {
      LOG(0, "Missing set handler for node %d", nodeID);
      status = VMK_READ_ERROR;
      goto done;
   }

   if (info.nInputArgs > 0) {
      if (info.nInputArgs > VSI_MAX_INPUT_ARGS) {
         status =  VMK_BAD_PARAM;
         goto done;
      }

      inputArgs = VSI_ParamListCreateFixed(VSI_PARAM_LIST_INPUT,
                                           info.nInputArgs);
      if (inputArgs == NULL) {
         status = VMK_NO_MEMORY;
         goto done;
      }
      status = VSI_ParamListCopyParams(TRUE, inputArgs, inputArgsHost,
                                       info.nInputArgs);
      if (status != VMK_OK) {
         goto done;
      }
   }

   LOG(1, "Dispatching SET handler %p for node %d",
       handler, nodeID);
   status = handler(nodeID, instanceArgs, inputArgs);

done:
   if (instanceArgs != NULL) {
      VSI_ParamListDestroy(instanceArgs);
   }
   if (inputArgs != NULL) {
      VSI_ParamListDestroy(inputArgs);
   }

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * VSI_GetList --
 *
 *	Entry point into vmkernel for a sysinfo GETLIST call.  Copies stuff
 *	from COS, calls the appropriate handler, copies results back to
 *	COS, and returns the error code from the handler
 *
 * Results:
 *	VMK_ReturnStatus from the handler, or other error code
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
VSI_GetList(void *infoHost, void *outBuf, int outBufLen)
{
   VMK_ReturnStatus status;
   VSI_NodeID nodeID;
   VSI_CallInfo info;
   VSI_ParamList *instanceArgs = NULL;
   VSI_ParamList *instanceOutList = NULL;
   VSI_ListHandler handler;

   if((outBuf == NULL) || (outBufLen <= 0)) {
      LOG(0, "Bad output buffer: %p:%d", outBuf, outBufLen);
      return VMK_BAD_PARAM;
   }

   status = VSIProcessInfo(infoHost, &info, &instanceArgs);
   if (status != VMK_OK) {
      LOG(0, "VSIProcessInfo returned %#x", status);
      return status;
   }
   nodeID = info.nodeID;

   handler = VSI_NodeGetListHandler(nodeID);
   if (handler == NULL) {
      LOG(0, "Missing list handler for node %d", nodeID);
      status = VMK_READ_ERROR;
      goto done;
   }

   instanceOutList = VSI_ParamListCreateFixed(VSI_PARAM_LIST_INSTANCE,
                                              info.nInstanceOutParams);
   if (instanceOutList == NULL) {
      status = VMK_NO_MEMORY;
      goto done;
   }
   ASSERT(VSI_ParamListAllocSize(instanceOutList) == outBufLen);

   LOG(1, "Dispatching GETLIST handler %p for node %d",
       handler, nodeID);
   status = handler(nodeID, instanceArgs, instanceOutList);
   if (status == VMK_OK) {
      status = VSI_ParamListCopyParams(FALSE, instanceOutList,
                                       outBuf,
                                       VSI_ParamListUsedCount(instanceOutList));
      if (status != VMK_OK) {
         LOG(1, "Failed to copy out %d args for %d: %#x", 
             VSI_ParamListUsedCount(instanceOutList), nodeID, status);
      }
   }

done:
   if (instanceArgs != NULL) {
      VSI_ParamListDestroy(instanceArgs);
   }
   if (instanceOutList != NULL) {
      VSI_ParamListDestroy(instanceOutList);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * VSI_Alloc/Free --
 *
 *      Wrapper functions for memory allocation done by calls from
 *      lib/vmksysinfo
 *
 * Results:
 *	Values of called functions.
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
void * 
VSI_Alloc(uint32 size)
{
   return Mem_Alloc(size);
}

void
VSI_Free(void *ptr)
{
   Mem_Free(ptr);
}
