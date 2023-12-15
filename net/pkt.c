/* **********************************************************
 * Copyright (C) 2004 VMware, Inc.All Rights Reserved -- VMware Confidential
 * **********************************************************/

/*
 * pkt.c --
 *    Implements operations on the packet.
 */

#include "vmkernel.h"
#define LOGLEVEL_MODULE_LEN 0
#define LOGLEVEL_MODULE Net
#include "log.h"

//#define NO_PKT_INLINE
#include "net_int.h"

#include "memalloc.h"
#include "vm_libc.h"
#include "memmap_dist.h"
#include "mod_loader.h" // only for backtrace help
#include "vmkstress.h"

#ifdef PKT_DEBUG
List_Links      netPktDbgList[1];
uint32          netPktDbgAllocCount;
List_Links      netPktDbgFreeQueue[1];
uint32          netPktDbgFreeQueueCount;
SP_SpinLockIRQ  netPktDbgLock;
#endif // PKT_DEBUG

#ifndef MIN
#define MIN(x, y)                                             \
   ((x) < (y))? (x):(y)
#endif

// keep a zeroed buffer of len MIN_TX_FRAME_LEN ready for use in the tx path
static char *runtBuffer;
static uint32 runtBufferLen;
MA runtBufferMA;


/*
 *----------------------------------------------------------------------------
 *
 *  Pkt_ModInit --
 *
 *    Initialize the Pkt module at load time.
 *
 *  Results:
 *    None.
 *
 *  Side effects:
 *    Debugging and bookeeping stuff is initialize.
 *
 *----------------------------------------------------------------------------
 */
VMK_ReturnStatus
Pkt_ModInit(void)
{
   VMK_ReturnStatus ret = VMK_OK;
#ifdef PKT_DEBUG
   SP_InitLockIRQ("netPktDbgLock", 
                  &netPktDbgLock, 
                  SP_RANK_IRQ_LEAF);
   List_Init(netPktDbgList);
   List_Init(netPktDbgFreeQueue);
   netPktDbgAllocCount = 0;
   netPktDbgFreeQueueCount = 0;
#endif // PKT_DEBUG
   
   ASSERT(runtBuffer == NULL);
   runtBufferLen = MIN_TX_FRAME_LEN;
   runtBuffer = Mem_Alloc(runtBufferLen);
   if (runtBuffer) {
      MPN runtMPN;
      memset(runtBuffer, 0, runtBufferLen);
      runtMPN = Mem_VA2MPN((VA)runtBuffer);

      runtBufferMA = MPN_2_MA(runtMPN) + ((VA)runtBuffer & PAGE_MASK);
   } else {
      runtBufferLen = 0;
      ret = VMK_FAILURE;
   }
   
   return ret;
}

/*
 *----------------------------------------------------------------------------
 *
 *  Pkt_ModCleanup --
 *
 *    Cleanup the Pkt module at unload time.
 *
 *  Results:
 *    None.
 *
 *  Side effects:
 *    Debugging and bookeeping stuff is cleaned up.
 *
 *----------------------------------------------------------------------------
 */
void
Pkt_ModCleanup(void)
{
#ifdef PKT_DEBUG
   // ASSERT(netPktDbgAllocCount == 0);
   LOG(0, "%u packets unfreed", netPktDbgAllocCount);
#ifdef ESX3_CLEANUP_EVERYTHING
   SP_CleanupLockIRQ(&netPktDbgLock);
#endif
#endif // PKT_DEBUG
   if (runtBuffer) {
      Mem_Free(runtBuffer);
      runtBuffer = NULL;
      runtBufferLen = 0;
      runtBufferMA = 0;
   }
}


/*
 *----------------------------------------------------------------------------
 *
 *  Pkt_DbgLogBT --
 *
 *    Print the bactrace to the log.
 *
 *  Results:
 *    None.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */
#ifdef PKT_DEBUG
void
Pkt_DbgLogBT(char *str, PktHandle *pkt, PktBTArr *btArr) 
{
   int i;

   for (i = 0; i < PKT_BT_LEN; i++) {
      uint32 offset;
      char *name;

      if (btArr->RA[i] == 0) {
         break;
      }

      if (Mod_LookupPC(btArr->RA[i], &name, &offset)) {
         _Log("%s (pkt %p): [0x%x]%s+0x%x\n", str, pkt, btArr->RA[i], name, offset);
      } else {
	 _Log("%s (pkt %p): [0x%x]\n", str, pkt, btArr->RA[i]);         
      }      

   }
}
#endif // PKT_DEBUG


/*
 *----------------------------------------------------------------------------
 *
 *  Pkt_DbgBT --
 *
 *    Stash the current call stack in the provided array.
 *
 *  Results:
 *    None.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */
#ifdef PKT_DEBUG
void
Pkt_DbgBT(PktBTArr *btArr) 
{
   uint32 *x = (uint32 *) __builtin_frame_address(0);  
   int i;

   for (i = 0; i < PKT_BT_LEN; i++) {
      if (((VA)(&x[1]) >= World_GetVMKStackTop(MY_RUNNING_WORLD)) ||
          ((VA)(&x[1]) < World_GetVMKStackBase(MY_RUNNING_WORLD))) {
         break;
      }
      btArr->RA[i] = x[1];
      x = (uint32 *)x[0];
   }
   if (i < PKT_BT_LEN) {
      btArr->RA[i] = 0;
   }
}
#endif // PKT_DEBUG

/*
 *----------------------------------------------------------------------------
 *
 *  Pkt_Alloc --
 *
 *    Allocate a packet from the free pool. If there are no free packets
 *    available, allocate a packet. The packet returned has the following
 *    structure --
 *
 *        -----------------------------------------------------------
 *       |           |               |          |                    |
 *       | PktHandle | PktDescriptor | headroom |  frame data(size)  |
 *       |           |               |          |                    |
 *        -----------------------------------------------------------
 *
 *  Results:
 *    A packet handle to the allocated packet.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

PktHandle *
Pkt_Alloc(size_t headroom, size_t size)
{
   VMK_ReturnStatus status;
   uint32 pktSize;
   PktHandle *handle = NULL;
   ASSERT(size <= (uint32)-1 - sizeof(PktHandle) - sizeof(PktDescriptor));
   pktSize = sizeof(PktHandle) + sizeof(PktDescriptor) + headroom + size;
  
#ifdef ESX3_NETWORKING_NOT_DONE_YET
   pktSize += 15; // XXX do something better about alignment
#else 
#error fix this alignment hack
#endif

   handle = (PktHandle *)Mem_Alloc(pktSize);

   if (UNLIKELY(VMK_STRESS_DEBUG_COUNTER(NET_PKT_ALLOC_FAIL))) {
      if (handle) {
         Mem_Free(handle);
         handle = NULL;
      }
   }

   if (handle) {
      memset(handle, 0, sizeof(PktHandle) + sizeof(PktDescriptor));
      handle->flags = PKT_FLAG_ALLOCATED;
      handle->pktDesc = (PktDescriptor *)(handle + 1);
      PktSetMaster(handle, handle);
      PktSetRefCount(handle, 1);
      PktOverwriteDescFlags(handle, PKTDESC_FLAG_ALLOCATED);
      handle->bufDesc = PktGetDescBufDesc(handle);
      handle->frameMappedLen = size;
      PktSetHeadroomLen(handle, headroom); 
      handle->bufDesc->sgSize = NET_PKT_SG_DEFAULT_SIZE;
      handle->bufDesc->sgMA.length = 0;
      if (size > 0) {
         handle->frameVA = 
            (FrameHdrVA)((VA)((PktDescriptor *)(handle + 1) + 1) + headroom);
#ifdef ESX3_NETWORKING_NOT_DONE_YET
         // XXX do something better about alignment
         handle->frameVA = (FrameHdrVA)(((VA)handle->frameVA + 15) & ~15);
#else 
#error fix this alignment hack
#endif

         // we assume contiguous MAs from Mem_Alloc()
         ASSERT((VMK_VA2MA((VA)handle->frameVA + size - 1) - 
                 VMK_VA2MA((VA)handle->frameVA)) == (size - 1));

         status = Pkt_AppendFrag(VMK_VA2MA((VA)handle->frameVA), size, handle);
         ASSERT(status == VMK_OK);
      } else {
         handle->frameVA = NULL;
      }         
      if (headroom > 0) {
         handle->headroom = (void *)((PktDescriptor *)(handle + 1) + 1);
      } else {
         handle->headroom = NULL;
      }
         
   }

   Pkt_DbgOnAlloc(handle); // nop in release builds

   return handle;
}


/*
 *----------------------------------------------------------------------------
 *
 *  Pkt_GetSGIndexFromOffset --
 *
 *    Find the element and index in the given SG array that describes the byte
 *    just after offset.
 *
 *  Results:
 *    An element and offset into the given SG array.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

void
Pkt_GetSGIndexFromOffset(const SG_MA *baseSG, size_t offset,
                        uint16 *sgElement, size_t *index)
{
   size_t numBytesLeft = offset;
   uint16 curSGElement = 0;

   LOG(10, "numBytesLeft: %u, baseSG->sg[%u].length: %u",
       numBytesLeft, curSGElement, baseSG->sg[curSGElement].length);  

   ASSERT(baseSG);
   while (curSGElement < baseSG->length &&
          numBytesLeft >= baseSG->sg[curSGElement].length) {
      numBytesLeft -= baseSG->sg[curSGElement].length;
      curSGElement++;
      LOG(10, "numBytesLeft: %u, baseSG->sg[%u].length: %u",
          numBytesLeft, curSGElement, baseSG->sg[curSGElement].length);  
   }

   *sgElement = curSGElement;
   *index = numBytesLeft;
}


/*
 *----------------------------------------------------------------------------
 *
 *  Pkt_CopyBytesToSGMA --
 *
 *    Copies len bytes of data into an SG_MA, starting at offset, 
 *    temporarily mapping each necessary sg element.
 *
 *  Results:
 *    VMK_ReturnStatus
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

VMK_ReturnStatus
Pkt_CopyBytesToSGMA(const SG_MA *baseSG, 
                    size_t numBytes, size_t offset, char *buf)
{
   size_t numBytesRemaining = numBytes;
   char *curPtr = buf;
   size_t elemOffset;
   uint32 curLength;
   uint16 curSGElement = 0;
   MA curMA;
   VMK_ReturnStatus status = VMK_FAILURE;

   ASSERT(buf);
   ASSERT(baseSG);
   ASSERT(baseSG->addrType == NET_SG_MACH_ADDR);

   // get the starting MA since it may not be on an sg boundary
   Pkt_GetSGIndexFromOffset(baseSG, offset, &curSGElement, &elemOffset);
   curMA = baseSG->sg[curSGElement].addr + elemOffset;
   curLength = baseSG->sg[curSGElement].length - elemOffset;

   if (UNLIKELY(VMK_STRESS_DEBUG_COUNTER(NET_COPY_TO_SGMA_FAIL))) {
      return VMK_FAILURE;
   }

   // loop through the sg array until we've copied all we want
   while (numBytesRemaining > 0 && curSGElement < baseSG->length) {
      KSEG_Pair *framePair;
      void *tmpElement = Kseg_GetPtrFromMA(curMA, curLength, &framePair);

      if (UNLIKELY(VMK_STRESS_DEBUG_COUNTER(NET_KSEG_FAIL))) {
         if (tmpElement) {
            Kseg_ReleasePtr(framePair);
         }
         tmpElement = NULL;
      }

      if (tmpElement) {
         uint32 numBytesConsumed = MIN(numBytesRemaining, curLength);
         memcpy(tmpElement, curPtr, numBytesConsumed);
         curPtr += numBytesConsumed;
         numBytesRemaining -= numBytesConsumed;
         curSGElement++;
         curLength = baseSG->sg[curSGElement].length;
         curMA = baseSG->sg[curSGElement].addr;
      } else {
         // Failed to map sg element into the kernel address space.
         status = VMK_INVALID_ADDRESS;
         break;
      }
      Kseg_ReleasePtr(framePair);
   }

   if (numBytesRemaining == 0) {
      status = VMK_OK;
   }

   return status;
}


/*
 *----------------------------------------------------------------------------
 *
 *  Pkt_CopyFrameFromSGMA --
 *
 *    Copies len bytes of data out of an SG_MA, starting at offset, 
 *    temporarily mapping each necessary sg element.
 *
 *    XXX this is an *exact* copy of Pkt_CopyBytesToSGMA, with the
 *        memcpy params swapped.  ripe for some type of code factoring
 *        but I don't want to pass in a fn pointer or use cpp magic. yet.
 *
 *  Results:
 *    VMK_ReturnStatus
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

VMK_ReturnStatus
Pkt_CopyBytesFromSGMA(const SG_MA *baseSG, 
                      size_t numBytes, size_t offset, char *buf)
{
   size_t numBytesRemaining = numBytes;
   char *curPtr = buf;
   size_t elemOffset;
   uint32 curLength;
   uint16 curSGElement = 0;
   MA curMA;
   VMK_ReturnStatus status = VMK_FAILURE;

   ASSERT(buf);
   ASSERT(baseSG);
   ASSERT(baseSG->addrType == NET_SG_MACH_ADDR);

   // get the starting MA since it may not be on an sg boundary
   Pkt_GetSGIndexFromOffset(baseSG, offset, &curSGElement, &elemOffset);
   curMA = baseSG->sg[curSGElement].addr;
   curLength = baseSG->sg[curSGElement].length - elemOffset;

   LOG(15, "copy bytes from sgMA: (%p, %u, %u, %p) baseSG->length %u", 
       baseSG, numBytes, offset, buf, baseSG->length);

   if (UNLIKELY(VMK_STRESS_DEBUG_COUNTER(NET_COPY_FROM_SGMA_FAIL))) {
      return VMK_FAILURE;
   }

   // loop through the sg array until we've copied all we want
   while (numBytesRemaining > 0 && curSGElement < baseSG->length) {
      KSEG_Pair *framePair;
      void *tmpElement = Kseg_GetPtrFromMA(curMA, curLength, &framePair);

      if (UNLIKELY(VMK_STRESS_DEBUG_COUNTER(NET_KSEG_FAIL))) {
         if (tmpElement) {
            Kseg_ReleasePtr(framePair);
         }
         tmpElement = NULL;
      }

      if (tmpElement) {
         uint32 numBytesConsumed = MIN(numBytesRemaining, curLength);
         memcpy(curPtr, tmpElement, numBytesConsumed);
         curPtr += numBytesConsumed;
         numBytesRemaining -= numBytesConsumed;
         curSGElement++;
         curLength = baseSG->sg[curSGElement].length;
         curMA = baseSG->sg[curSGElement].addr;
      } else {
         // Failed to map sg element into the kernel address space.
         LOG(1, "invalid address at element %u: 0x%Lx", 
             curSGElement, baseSG->sg[curSGElement].addr);
         status = VMK_INVALID_ADDRESS;
         break;
      }
      Kseg_ReleasePtr(framePair);
   }

   if (numBytesRemaining == 0) {
      status = VMK_OK;
   } else {
      LOG(1, "numBytesRemaining %u", numBytesRemaining);
   }

   return status;
}


/*
 *----------------------------------------------------------------------------
 *
 *  Pkt_CreatePrivateFrameHdr --
 *
 *    Copies the first numBytes of the frame described by the given pkt handle
 *    into its own private buffer.
 *
 *  Results:
 *    VMK_ReturnStatus indicating the outcome.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

VMK_ReturnStatus
Pkt_CreatePrivateFrameHdr(PktHandle *handle, size_t headroom, size_t numBytes)
{
   VMK_ReturnStatus ret = VMK_OK;
   uint32 flags = PKT_FLAG_FRAME_HEADER_MAPPED | PKT_FLAG_PRIVATE_BUF_DESC;
   PktBufDescriptor *bufDesc = NULL;
   FrameHdrVA frameVA;
   unsigned int frameVAOffset, headroomOffset;
   PktBufDescriptor *newBufDesc;
   unsigned int extraSGElems = 0;

   ASSERT(handle);
   ASSERT(!PktIsMaster(handle));
   ASSERT(IS_CLR(handle->flags, flags));

   bufDesc = PktGetDescBufDesc(handle);

   ASSERT(bufDesc->sgMA.addrType == NET_SG_MACH_ADDR);
   ASSERT(bufDesc->sgMA.length > 0);

   LOG(10, "attempting to create a %u byte private buffer", numBytes);
  
   /*
    * we need 2 more SG_Elems than the source packet because
    * 1) we use up one for our private buffer, but usually still 
    *    index into the first  elem of the sourxe with our second elem.
    * 2) we might cross a page boundary in our private buffer where
    *    the source packet didn't.
    */
   if (bufDesc->sgSize > (NET_PKT_SG_DEFAULT_SIZE - 2)) {
      extraSGElems = (bufDesc->sgSize - NET_PKT_SG_DEFAULT_SIZE) + 2;
   }
   headroomOffset = sizeof(PktBufDescriptor) + sizeof(SG_Elem) * extraSGElems;
   frameVAOffset = headroomOffset + headroom;
   newBufDesc = Mem_Alloc(frameVAOffset + numBytes);

   if (UNLIKELY(VMK_STRESS_DEBUG_COUNTER(NET_PRIV_HDR_MEM_FAIL))) {
      if (newBufDesc) {
         Mem_Free(newBufDesc);
         newBufDesc = NULL;
      }
   }

   if (newBufDesc) {
      uint16 curSGElement;
      uint16 curBaseSGElement;
      size_t index;
      unsigned int i;

      frameVA = (FrameHdrVA)((VA)newBufDesc + frameVAOffset);

      memset(newBufDesc, 0, sizeof(*newBufDesc));
      newBufDesc->sgSize = NET_PKT_SG_DEFAULT_SIZE + extraSGElems;

      if (numBytes <= handle->frameMappedLen) {
         // common case, no mapping needed
         memcpy(frameVA, handle->frameVA, numBytes);
      } else {
         // have to iterate SG elems and do mappings
         ret = Pkt_CopyBytesFromSGMA(&bufDesc->sgMA, numBytes, 0, frameVA);
         if (ret != VMK_OK) {
            return ret;
         }
      }

      handle->bufDesc = newBufDesc;
      handle->frameVA = frameVA;
      handle->frameMappedLen = numBytes;
      handle->flags |= flags;

      handle->headroom = (void *)((VA)newBufDesc + headroomOffset);
      PktSetHeadroomLen(handle, headroom); 

      Pkt_AppendFrag(VMK_VA2MA((VA)handle->frameVA), 
                     handle->frameMappedLen, 
                     handle);

      curSGElement = newBufDesc->sgMA.length;
      /* 
       * locate the first byte following the frameVA buffer by skipping over
       * frameLen bytes in the baseSG
       */
      Pkt_GetSGIndexFromOffset(&bufDesc->sgMA, 
                               handle->frameMappedLen, 
                               &curBaseSGElement, 
                               &index);

      LOG(10, "curBaseSGElement = %u, index = %u", curBaseSGElement, index);
  
      /* copy the rest of the sg array to the destination as is */
      for (i = curBaseSGElement;
           i < bufDesc->sgMA.length && curSGElement < newBufDesc->sgSize;
           i++, curSGElement++) {
         MA tmp = bufDesc->sgMA.sg[i].addr;
         size_t tmpLen = bufDesc->sgMA.sg[i].length - index;
         ASSERT(tmp < ((MA)-1) - index);
         tmp += index;
         LOG(10, "MA = 0x%Lx, length = %u", tmp, tmpLen);  
         newBufDesc->sgMA.sg[curSGElement].addr = tmp;
         newBufDesc->sgMA.sg[curSGElement].length = tmpLen;
         newBufDesc->sgMA.sg[curSGElement].offset = 0;
         index = 0; /* do something better later */
      }
      newBufDesc->sgMA.length = curSGElement;
      newBufDesc->bufLen = bufDesc->bufLen;
      newBufDesc->frameLen = bufDesc->frameLen;
   } else {
      ret = VMK_NO_MEMORY;
   }

   return ret;
}


/*
 *----------------------------------------------------------------------------
 *
 *  PktMemFree --
 *
 *    Worker function to release/recache/whatever pkt handles and any
 *    buffers and/or descriptors associated with them. 
 *
 *  Results:
 *    Memory is made available for reallocation.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

void
PktMemFree(PktHandle *pkt)
{
   ASSERT(pkt->flags & PKT_FLAG_ALLOCATED);
   ASSERT(!(pkt->flags & PKT_FLAG_FREE));
   ASSERT(!(pkt->flags & ~PKT_VALID_FLAGS));

   pkt->flags &= ~PKT_FLAG_ALLOCATED;
   pkt->flags |= PKT_FLAG_FREE;

   pkt = Pkt_DbgOnFree(pkt); // nop on release builds

   if (pkt != NULL) {
      if (UNLIKELY(IS_SET(pkt->flags, PKT_FLAG_PRIVATE_BUF_DESC))) {
         Mem_Free(pkt->bufDesc);
      }
      
      Mem_Free(pkt);
   }
}


/*
 *----------------------------------------------------------------------------
 *
 *  Pkt_ReleaseOrComplete --
 *
 *    Release a reference to a pkt, and if this is the last reference
 *    and the packet is flagged for io completion notification, we
 *    return the master handle with a refcount of 1 so that it may be 
 *    passed to an io completion handler.
 *
 *  Results:
 *    The master is returned if completion is needed.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

PktHandle *
Pkt_ReleaseOrComplete(PktHandle *pkt)
{
   uint32 prevRefCount;
   PktHandle *master;
   uint32 descFlags;

   ASSERT(pkt);

   if (IS_SET(pkt->flags, PKT_FLAG_PRIVATE_BUF_DESC)) {
      ASSERT(!PktIsMaster(pkt));
      ASSERT((FrameHdrVA)pkt->bufDesc
             + sizeof(PktBufDescriptor) 
             + (sizeof(SG_Elem) * (pkt->bufDesc->sgSize - NET_PKT_SG_DEFAULT_SIZE))
             == pkt->headroom);
      ASSERT((pkt->headroom + pkt->bufDesc->headroomLen) == pkt->frameVA);
   }

   master = PktGetMaster(pkt);
   descFlags = pkt->pktDesc->flags;

   prevRefCount = PktDecRefCount(pkt);
   /*
    * DO NOT DEREFERENCE pkt->pktDesc BELOW HERE, unless prevRefCount
    * is 1, otherwise another thread may come along and decrement 
    * refcount further and possibly free the descriptor.  
    */

   // we let the master hang around until completion
   if (UNLIKELY(pkt != master)) {
      PktMemFree(pkt);
   }
   pkt = NULL;

   if (prevRefCount == 1) {
      /*
       * it's safe in here to play around with the descriptor bc
       * we know we hold the only outstanding reference to it.
       */
      if (IS_SET(descFlags, PKTDESC_FLAG_NOTIFY_COMPLETE)) {
         PktSetRefCount(master, 1);
         Pkt_DbgOnComplete(master); // nop on release builds
         return master;
      } else {
         PktMemFree(master); 
      }
   }

   return NULL;
}


/*
 *----------------------------------------------------------------------------
 *
 * Pkt_Complete --
 *
 *    Complete and free a single packet.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

void
Pkt_Complete(PktHandle *pkt)
{
   pkt = Pkt_ReleaseOrComplete(pkt);
   if (pkt != NULL) {
      VMK_ReturnStatus status;
      Port *port;

      status = Portset_GetPort(pkt->pktDesc->srcPortID, &port);
      if (status == VMK_OK) {
         PktList tmpList;

         PktList_Init(&tmpList);
         PktList_AddToTail(&tmpList, pkt);
         Pkt_DbgOnNotify(&tmpList); // nop on release builds
         IOChain_Start(port, &port->notifyChain, &tmpList);
         ASSERT(PktList_IsEmpty(&tmpList));
         Portset_ReleasePort(port);
      }
   }
}


/*
 *----------------------------------------------------------------------------
 *
 *  Pkt_PartialCopy --
 *
 *    Duplicate the given handle, copy _at_least_ (but maybe more than)
 *    the first numBytes of the sg array into the handle's private frame
 *    header and create a new sg array to describe the resultant
 *    packet.
 *
 *  Results:
 *    A pointer to a handle that is a clone of the given PktHandle.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

PktHandle *
Pkt_PartialCopy(PktHandle *srcHandle, size_t headroom, size_t numBytes)
{
   PktHandle *destHandle;

   ASSERT(srcHandle);
   ASSERT(PktGetBufType(srcHandle) == NET_SG_MACH_ADDR);

   LOG(15, "%u bytes%s", numBytes, numBytes ? "" : " (clone)");

   destHandle = Mem_Alloc(sizeof(*destHandle));

   if (UNLIKELY(VMK_STRESS_DEBUG_COUNTER(NET_PART_COPY_FAIL))) {
      if (destHandle) {
         Mem_Free(destHandle);
         destHandle = NULL;
      }
   }

   if (destHandle) {
      memcpy(destHandle, srcHandle, sizeof(*destHandle));
      memset(&destHandle->pktLinks, 0, sizeof(List_Links));
      PktIncRefCount(srcHandle);
      // TODO: better way to copy certain flags selectively.
      destHandle->flags = srcHandle->flags & 
         ~(PKT_FLAG_FRAME_HEADER_MAPPED | PKT_FLAG_PRIVATE_BUF_DESC);
      if (UNLIKELY(IS_SET(srcHandle->flags, PKT_FLAG_PRIVATE_BUF_DESC))) {
         /*
          * If the source handle already had a private copy of
          * the headers we need to make sure that our copy is at
          * least as long as that one, so that we don't share a
          * portion of the frame that may be modified at any time
          * (because it's considered private by the source handle)
          */
         numBytes = MAX(srcHandle->frameMappedLen, numBytes);  
      }

      /*
       * and don't sweat it if the caller asked for more bytes 
       * than the src pkt has
       */
      numBytes = MIN(srcHandle->bufDesc->frameLen, numBytes);

      /*
       * and finally make sure we have at least as much headroom
       * as the original
       */
      headroom = MAX(srcHandle->bufDesc->headroomLen, headroom);

      if (numBytes > 0) {
         if (Pkt_CreatePrivateFrameHdr(destHandle, headroom, numBytes) != VMK_OK) {
            Mem_Free(destHandle);
            destHandle = NULL;
         }
      }
      Pkt_DbgOnAlloc(destHandle);  // nop on release builds
   }

   return destHandle;
}
