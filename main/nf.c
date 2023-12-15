/* **********************************************************
 * Copyright 2000 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * nf.c -- 
 *
 *	Network filtering infrastructure.
 */

/*
 * Compilation flags
 */

// debugging
#if	defined(VMX86_DEBUG) && defined(VMX86_DEVEL)
#define	NF_DEBUG_VERBOSE	(0)
#define	NF_DEBUG		(1)
#else
#define	NF_DEBUG_VERBOSE	(0)
#define	NF_DEBUG		(0)
#endif

// compile-time options
#define	NF_SINGLE_WORLD_FILTER	(1)

/*
 * Includes
 */

#include "x86apic.h"
#include "vm_types.h"
#include "vm_libc.h"
#include "vmkernel.h"
#include "net.h"
#include "list.h"
#include "splock.h"
#include "memalloc.h"
#include "scattergather.h"
#include "timer.h"
#include "host.h"
#include "proc.h"
#include "nf.h"
#include "world.h"
#include "parse.h"
#include "libc.h"

#define LOGLEVEL_MODULE NF
#define LOGLEVEL_MODULE_LEN 2
#include "log.h"


/*
 * Constants
 */

#define	NF_CMD_ARGS_MAX		(16)

#define	NF_INSTANCE_NAME_LEN	(32)

#define	NF_PROC_NAME_SIZE	(VMNIXPROC_MAX_NAME)
#define	NF_PROC_BUF_SIZE	(VMNIXPROC_BUF_SIZE)

#define	NF_REAP_DELAY_MS	(10)
#define	NF_PROC_DELAY_MS	(10)

/*
 * Types
 */

struct NFClass;
typedef struct NFClass NFClass;

struct NFPacketQueue {
   List_Links queue;
   int nPackets;
};

static List_Links sendQueue;
static List_Links dropQueue;

SP_SpinLock sendQueueLock;
SP_SpinLock dropQueueLock;

struct NFPacket {
   List_Links	links;			// for NFPacketQueue
   NetSG_Array 	sgArr;			// packet data
   NFType	nfType;			// source type
   World_ID	srcWorldID;		// source world
   Net_HandleID	srcHandleID;		// source handle
   uint32	srcHandleVersion;	// current version of source handle
   uint32	flags;			// source flags
   int32	xmitIndex;		// source transmit ring index
   NFFilter	*filter;		// current filter
};

struct NFFilter {
   List_Links	    links;		// for NFClass instances list
   SP_SpinLock   lock;		        // for mutual exclusion (sync ops)
   int		    id;			// unique instance number
   char		    name[NF_INSTANCE_NAME_LEN]; // non-unique name
   int		    refCount;		// reference count
   NFClass	    *nfClass;		// class reference
   void		    *state;		// instance-specific filter state 
   NFFilter	    *forward;		// next filter (or NULL)
   Proc_Entry	    procEntry;		// procfs entry
   NFTimerFun       periodic;           // periodic code
   int              period;             // in milliseconds
   Timer_Handle     timerHandle;
};

struct NFClass {
   List_Links	    links;		// for global nfClasses list
   int		    id;			// unique class number
   char		    name[NF_CLASS_NAME_LEN]; // unique name
   NFOps	    ops;		// callbacks
   SP_SpinLock      instancesLock;	// protect instances list
   List_Links	    instances;		// active NFFilter instances
   int		    nextInstanceID;	// instance id generator
};


/*
 * Globals
 */

// global lock
static SP_SpinLock nfLock;

// global class list
static List_Links nfClasses;
static int nextClassID;

// procfs entries
static Proc_Entry nfProcDir;		// /proc/vmnix/filters/
static Proc_Entry nfProcXmitDir;	// /proc/vmnix/filters/xmit/
static Proc_Entry nfProcXmitPush;	// /proc/vmnix/filters/xmitpush
static Proc_Entry nfProcXmitPop;	// /proc/vmnix/filters/xmitpop
static Proc_Entry nfProcStatus;		// /proc/vmnix/filters/status

/*
 * Local functions
 */

static void *NFAllocObject(int size, const char *objName);

static INLINE int  SGArrayLength(const NetSG_Array *sgArr);
static INLINE void SGArrayShallowCopy(NetSG_Array *dest, const NetSG_Array *src);
static int SGArrayVirtualCopy(NetSG_Array *dest,
                              const NetSG_Array *src,
                              World_ID srcWorldID);

static void NFPacketFree(NFPacket *pkt, Net_EtherHandle *handle);
static void NFPacketTransmit(NFPacket *pkt, Net_EtherHandle *handle);

static INLINE void    NFLock(void);
static INLINE void    NFUnlock(void);

static NFClass *NFClassNew(int id, char *name, NFOps *ops);
static void     NFClassFree(NFClass *c);
static NFClass *NFClassLookupByID(int id);
static void     NFFilterPeriodic(void *data, Timer_AbsCycles timestamp);
static void     NFClassReapInstances(void *data, Timer_AbsCycles timestamp);

static INLINE void NFClassLockInstances(NFClass *c);
static INLINE void    NFClassUnlockInstances(NFClass *c);

static NFFilter *NFFilterNew(int id, char *name, NFClass *c, void *state);
static void      NFFilterFree(NFFilter *f);
static NFFilter *NFFilterCreate(int classID, char *name, int argc, char **argv);
static int	 NFFilterDestroy(NFFilter *f);
static void      NFFilterRelease(NFFilter *f, const char *debug);
static int       NFFilterProcRead(Proc_Entry *entry, char *buffer, int *len);
static int       NFFilterProcWrite(Proc_Entry *entry, char *buffer, int *len);

static INLINE void NFFilterLock(NFFilter *f);
static INLINE void    NFFilterUnlock(NFFilter *f);

/*
 * Macros
 */

#define	NFALLOC(_type)	((_type *) NFAllocObject(sizeof(_type), #_type))

/*
 * Locking Overview
 *
 * Lock Ordering:
 *   nfLock			- global module lock
 *   NFClassLockInstances()	- per-class lock 
 *   NFFilterLock()		- per-filter lock
 *
 * In addition to the above locks, user-supplied filter modules may
 * internally create and use NFLock objects.
 *
 */

/*
 * Locking wrappers
 */

static INLINE void
NFLock(void)
{
   SP_Lock(&nfLock);
}

static INLINE void
NFUnlock(void)
{
   SP_Unlock(&nfLock);
}

static INLINE void
NFClassLockInstances(NFClass *c)
{
   SP_Lock(&c->instancesLock);
}

static INLINE void
NFClassUnlockInstances(NFClass *c)
{
   SP_Unlock(&c->instancesLock);
}

static INLINE void
NFFilterLock(NFFilter *f)
{
   SP_Lock(&f->lock);
}

static INLINE void
NFFilterUnlock(NFFilter *f)
{
   SP_Unlock(&f->lock);
}

/*
 * Utility operations
 */

int
NF_ParseArgs(char *buf,		// IN/OUT: source string
             char *argv[],	// IN/OUT: arg vector
             int argc)		// IN:     vector size
{
   return(Parse_Args(buf, argv, argc));
}

int
NF_ParseInt(const char *s)	// IN: string to parse
{
   // parse s as integer value
   int value = simple_strtoul(s, NULL, 0);
   return(value);
}

static void *
NFAllocObject(int size, const char *objName)
{
   // allocate storage
   void *obj = Mem_Alloc(size);

   if (obj == NULL) {
      // log warning and fail
      Warning("unable to allocate %s", objName);
      return(NULL);
   } else {
      // return zero-filled object
      memset(obj, 0, size);
      return(obj);
   }
}


void
NFInsertSendPacket(NFPacket *pkt)
{
   SP_Lock(&sendQueueLock);
   List_InitElement(&pkt->links);
   List_Insert(&pkt->links, LIST_ATREAR(&sendQueue));
   SP_Unlock(&sendQueueLock);
}

void
NFInsertDropPacket(NFPacket *pkt)
{
   SP_Lock(&dropQueueLock);
   List_InitElement(&pkt->links);
   List_Insert(&pkt->links, LIST_ATREAR(&dropQueue));
   SP_Unlock(&dropQueueLock);
}


void
NFFilter_DrainSendQueue(Net_EtherHandle *handle)
{
   List_Links queue;
   List_Links *itemPtr;
   List_Links *nextPtr;
   List_Init(&queue);

   /*
    * if handle != NULL then it means we are executing inline through
    * the net code. This means we have the EtherDev txLock held and the
    * EtherHandle lock held on handle.
    * As a consequence we can only send
    * out packets belonging to that handle because acquiring any other
    * handle's lock will cause a lock-rank violation with the EtherDev txLock.
    * Packets belonging to other handles will get sent out through the 
    * periodic timer where no net locks are held as we enter this routine.
    * Note: The above also applies to NFFilter_DrainDropQueue which calls
    * into the net code through Net_FreePacket.
    */
   SP_Lock(&sendQueueLock);
   nextPtr = List_First(&sendQueue);
   itemPtr = nextPtr;
   for (;!List_IsAtEnd((&sendQueue), itemPtr);
        itemPtr = nextPtr) {

      NFPacket *pkt = (NFPacket *)itemPtr;
      nextPtr = List_Next(itemPtr);
      if (!handle || (handle->hd.handleID == pkt->srcHandleID)) {
         List_Remove(itemPtr);
         List_Insert(itemPtr, &queue);
      }

   }

   SP_Unlock(&sendQueueLock);
   
   while(!List_IsEmpty(&queue)) {
      NFPacket *pkt;
      List_Links *elt;

      elt = List_First(&queue);
      List_Remove(elt);
      pkt = (NFPacket *)elt;
      NFPacketTransmit(pkt, handle);      
   }
}

void
NFFilter_DrainDropQueue(Net_EtherHandle *handle)
{
   List_Links queue;
   List_Links *itemPtr;
   List_Links *nextPtr;
   List_Init(&queue);
   
   SP_Lock(&dropQueueLock);
   nextPtr = List_First(&dropQueue);
   itemPtr = nextPtr;
   for (;!List_IsAtEnd((&dropQueue), itemPtr);
        itemPtr = nextPtr) {
      
      NFPacket *pkt = (NFPacket *)itemPtr;
      nextPtr = List_Next(itemPtr);
      if (!handle || (handle->hd.handleID == pkt->srcHandleID)) {
         List_Remove(itemPtr);
         List_Insert(itemPtr, &queue);
      }

   }
   
   SP_Unlock(&dropQueueLock);
   
   while(!List_IsEmpty(&queue)) {
      NFPacket *pkt;
      List_Links *elt;

      elt = List_First(&queue);
      List_Remove(elt);
      pkt = (NFPacket *)elt;
      NFPacketFree(pkt, handle);      
   }
}



/*
 * NFPacketQueue operations
 */

NFPacketQueue *
NFPacketQueue_New(void)
{
   NFPacketQueue *new;

   // allocate storage
   if ((new = NFALLOC(NFPacketQueue)) == NULL) {
      return(NULL);
   }

   // initialize
   List_Init(&new->queue);
   new->nPackets = 0;

   return(new);
}

void
NFPacketQueue_Free(NFPacketQueue *q)
{
   // sanity check
   if (q->nPackets != 0) {
      Warning("%d packets on queue", q->nPackets);
   }

   // reclaim storage
   Mem_Free(q);
}

int
NFPacketQueue_Length(NFPacketQueue *q)
{
   return(q->nPackets);
}
     
void
NFPacketQueue_Insert(NFPacketQueue *q, NFPacket *pkt)
{
   // add pkt to tail of queue
   List_Insert(&pkt->links, LIST_ATREAR(&q->queue));
   q->nPackets++;
}

void
NFPacketQueue_Remove(NFPacketQueue *q, NFPacket *pkt)
{
   // remove pkt from queue
   List_Remove(&pkt->links);
   q->nPackets--;
}

NFPacket *
NFPacketQueue_First(NFPacketQueue *q)
{
   NFPacket *pkt;

   // NULL if list empty
   if (List_IsEmpty(&q->queue)) {
      return(NULL);
   }

   // otherwise pkt at head of queue
   pkt = (NFPacket *) List_First(&q->queue);
   return(pkt);
}

/*
 * NetSG_Array utility operations NFPacket operations
 */

static INLINE int
SGArrayLength(const NetSG_Array *sgArr)
{
   int i, length;
   
   length = 0;
   for (i = 0; i < sgArr->length; i++) {
      length += sgArr->sg[i].length;
   }

   return(length);
}

static INLINE void
SGArrayShallowCopy(NetSG_Array *dest, const NetSG_Array *src)
{
   int i;

   // copy structure only, not data
   dest->addrType = src->addrType;
   dest->length   = src->length;
   for (i = 0; i < src->length; i++) {
      dest->sg[i] = src->sg[i];
   } 
}

static int
SGArrayVirtualCopy(NetSG_Array *dest, const NetSG_Array *src, World_ID srcWorldID)
{
   int dataLength, copyLength;
   World_Handle *world;
   void *data;
   VMK_ReturnStatus status;
   

   // lookup world by id, fail if not found
   world = World_FindNoRefCount(srcWorldID);
   if (world == NULL) {
      return(NF_FAILURE);
   }

   // determine packet size
   dataLength = SGArrayLength(src);

   // allocate data storage, fail if unable
   data = (void *) Mem_Alloc(dataLength);
   if (data == NULL) {
      // release world, fail
      World_ReleaseNoRefCount(world);
      return(NF_FAILURE);
   }

   // prepare destination for coalesced copy
   dest->addrType = NET_SG_VIRT_ADDR;
   dest->length = 1;
   dest->sg[0].addrLow = (VA)data;
   dest->sg[0].length = dataLength;
   ASSERT(dataLength <= ETH_MAX_FRAME_LEN); // XXX: TSO possible?
   status = Net_CopyPacket(MY_RUNNING_WORLD, dest, 
                           world, (NetSG_Array *) src, FALSE, &copyLength);
   // sanity check
   if (copyLength != dataLength) {
      if (status != VMK_NO_RESOURCES) {
         VmWarn(srcWorldID, "packet copy length mismatch");
      }
      // release world, fail
      World_ReleaseNoRefCount(world);
      return(NF_FAILURE);
   }

   // release world, succeed
   World_ReleaseNoRefCount(world);
   return(NF_SUCCESS);
}

/*
 * NFPacket operations
 */

NFPacket *
NFPacket_CreateTransmit(World_ID worldID,
                        Net_EtherHandle *handle,
                        NetSG_Array *sgArr,
                        uint32 flags,
			int xmitIndex)
{
   NFPacket *pkt;
   int status;

   // allocate container, fail if unable
   if ((pkt = (NFPacket *) Mem_Alloc(sizeof(NFPacket))) == NULL) {
      return(NULL);
   }

   // initialize
   List_InitElement(&pkt->links);
   pkt->nfType = NF_TRANSMIT;
   pkt->srcWorldID = worldID;
   pkt->srcHandleID = handle->hd.handleID;
   pkt->srcHandleVersion = handle->hd.handleVersion;
   pkt->flags = flags;
   pkt->xmitIndex = xmitIndex;
   pkt->filter = NULL;

   // physical packet: copy sg structure only
   if ((sgArr->addrType == NET_SG_PHYS_ADDR) && (flags & VMXNET_XMIT_CAN_KEEP)){
      SGArrayShallowCopy(&pkt->sgArr, sgArr);
      return(pkt);
   }

   // virt or phys packet, but not allowed to keep
   ASSERT(sgArr->addrType == NET_SG_PHYS_ADDR || sgArr->addrType == NET_SG_VIRT_ADDR);


   status = SGArrayVirtualCopy(&pkt->sgArr, sgArr, worldID);
   if (status != NF_SUCCESS) {
      VmWarn(worldID, "packet create: virtual copy failed");
      Mem_Free(pkt);
      return(NULL);
   }

   return(pkt);
}

static void
NFPacketFree(NFPacket *pkt, Net_EtherHandle *handle)
{
   if (pkt->sgArr.addrType == NET_SG_PHYS_ADDR) {
      // reclaim packet data (physical memory)
      ASSERT(pkt->nfType == NF_TRANSMIT);
      Net_ReturnXmitNFPkt(pkt->srcHandleID, 
                          handle,
                          pkt->srcHandleVersion, 
			  pkt->xmitIndex);

      // reclaim container
      Mem_Free(pkt);
      return;
   }
   
   if (pkt->sgArr.addrType == NET_SG_VIRT_ADDR) {
      // reclaim packet data (virtual memory)
      ASSERT(pkt->sgArr.length == 1);
      Mem_Free((void*)(VA)pkt->sgArr.sg[0].addrLow);

      // reclaim container
      Mem_Free(pkt);
      return;
   }

   // unexpected packet type
   VmWarn(pkt->srcWorldID, "packet free: bad packet type");
}

void
NFPacket_Drop(NFPacket *pkt)
{
   // sanity check
   ASSERT(pkt->filter != NULL);

   // release filter
   NFFilterRelease(pkt->filter, "NFPacket_Drop");
   NFInsertDropPacket(pkt);
}

static void
NFPacketTransmit(NFPacket *pkt, Net_EtherHandle *handle)
{
   VMK_ReturnStatus status;

   // sanity check
   ASSERT(pkt->nfType == NF_TRANSMIT);

   // perform actual hardware transmit
   /*
    * vmxnet: if status != VMK_OK then Net_NFTransmit 
    * will queue packet into the device queue. The packet will
    * be resent at a later time when the device queue gets drained.
    * vlance and old_vmxnet: The packet is discarded. This is the
    * same behavior we have without the netfilter in place.
    */
 
   status = Net_NFTransmit(pkt->srcHandleID, handle,
                           &pkt->sgArr, pkt->flags, pkt->xmitIndex);

   // NET_SG_VIRT_ADDR: primitive transmit internally copies virtual packet data
   if (pkt->sgArr.addrType == NET_SG_VIRT_ADDR) {
      NFPacketFree(pkt, handle);
      return;
   }

   // primitive transmit internally reclaims physical packet data
   if (pkt->sgArr.addrType == NET_SG_PHYS_ADDR) {
      // deallocate NFPacket container only
      Mem_Free(pkt);
      return;
   }

   // unexpected packet type
   VmWarn(pkt->srcWorldID, "packet xmit: bad packet type");
}

void
NFPacket_Forward(NFPacket *pkt)
{
   NFFilter *f = pkt->filter;

   // sanity check
   ASSERT(f != NULL);

   NFFilterRelease(f, "NFPacket_Forward");
   // forward to next network filter stage, if any
   if (f->forward != NULL) {
      // release reference to current filter
      NFFilter_Filter(f->forward, pkt);
      return;
   }

   // forward to actual hardware
   if (pkt->nfType == NF_TRANSMIT) {
      pkt->filter = NULL;
      NFInsertSendPacket(pkt);
      return;
   }
   
   // unexpected packet type
   VmWarn(pkt->srcWorldID, "packet forward: bad packet type");
}

int
NFPacket_Size(NFPacket *pkt)
{
   return(SGArrayLength(&pkt->sgArr));
}


void *
NFPacket_Data(NFPacket *pkt)
{
   // virtual packet: simply return data
   if (pkt->sgArr.addrType == NET_SG_VIRT_ADDR) {
      ASSERT(pkt->sgArr.length == 1);
      return((void*)(VA)pkt->sgArr.sg[0].addrLow);
   }
   
   // sanity check
   ASSERT(pkt->nfType == NF_TRANSMIT);

   // physical packet: convert to virtual
   if (pkt->sgArr.addrType == NET_SG_PHYS_ADDR) {
      NetSG_Array sgVirtual;
      int status;

      // attempt virtual copy
      status = SGArrayVirtualCopy(&sgVirtual, &pkt->sgArr, pkt->srcWorldID);
      if (status != NF_SUCCESS) {
         Warning("virtual copy failed");
         return(NULL);
      }

      // clear "can keep packet" flag associated w/ physical packets
      pkt->flags &= ~VMXNET_XMIT_CAN_KEEP;

      // reclaim original physical memory
      Net_ReturnXmitNFPkt(pkt->srcHandleID, 
                          NULL,
                          pkt->srcHandleVersion, 
                          pkt->xmitIndex);

      // replace with new virtual copy, return data
      SGArrayShallowCopy(&pkt->sgArr, &sgVirtual);
      ASSERT(pkt->sgArr.length == 1);
      return((void*)(VA)pkt->sgArr.sg[0].addrLow);
   }

   // unexpected packet type
   VmWarn(pkt->srcWorldID, "packet data: bad packet type");
   return(NULL);
}

/*
 * NFClass operations
 */

static NFClass *
NFClassNew(int id, char *name, NFOps *ops)
{
   NFClass *c;
   
   // sanity check
   if (strlen(name) >= NF_CLASS_NAME_LEN) {
      Warning("name \"%s\" exceeds max length", name);
      return(NULL);
   }

   // allocate storage, fail if unable
   if ((c = NFALLOC(NFClass)) == NULL) {
      return(NULL);
   }

   // intialize
   c->id = id;
   strcpy(c->name, name);
   c->ops = *ops;

   // initialize instances data
   SP_InitLock("NFClassInstancesLock", 
                  &c->instancesLock, SP_RANK_NF_INSTANCES);
   List_Init(&c->instances);
   c->nextInstanceID = 0;

   return(c);
}

static void
NFClassFree(NFClass *c)
{
   // reclaim container
   SP_CleanupLock(&c->instancesLock);
   Mem_Free(c);
}

int
NFClass_Register(char *name, NFOps *ops)
{
   int classID;
   NFClass *c;
   
   // sanity checks
   if ((ops == NULL) ||
       (ops->create  == NULL) ||
       (ops->destroy == NULL) ||
       (ops->filter  == NULL)) {
      return(NF_CLASS_ID_NONE);
   }

   // prevent duplicate class names
   if (NFClass_LookupByName(name) != NF_CLASS_ID_NONE) {
      return(NF_CLASS_ID_NONE);
   }

   // acquire global lock
   NFLock();

   // generate unique class id
   classID = nextClassID++;

   c = NFClassNew(classID, name, ops);
   if (c == NULL) {
      // release lock and fail
      NFUnlock();      
      return(NF_CLASS_ID_NONE);
   }

   // add to classes list
   List_Insert(&c->links, LIST_ATREAR(&nfClasses));

   // release lock
   NFUnlock();

   // debugging
   if (NF_DEBUG) {
      LOG(0, "id=%d, name=%s", classID, name);
   }

   return(classID);
}

// requires: nfLock held
static NFClass *
NFClassLookupByID(int id)
{
   List_Links *elt;

   // search list for match
   LIST_FORALL(&nfClasses, elt) {
      NFClass *c = (NFClass *) elt;
      if (c->id == id) {
         return(c);
      }
   }

   // not found
   return(NULL);
}

void
NFClass_Unregister(int classID)
{
   Bool reclaim;
   NFClass *c;

   // acquire lock
   NFLock();

   // lookup class by id, fail if not found
   c = NFClassLookupByID(classID);
   if (c == NULL) {
      // debugging
      if (NF_DEBUG) {
         LOG(0, "id=%d not found", classID);
      }
      // release lock, fail
      NFUnlock();
      return;
   }

   // remove from list of active classes
   List_Remove(&c->links);
   if (NF_DEBUG) {
      LOG(0, "id=%d unregistered", classID);
   }

   // reclaim only if no instances exist
   NFClassLockInstances(c);
   reclaim = List_IsEmpty(&c->instances);
   NFClassUnlockInstances(c);

   if (reclaim) {
      NFClassFree(c);
      if (NF_DEBUG) {
         LOG(0, "id=%d reclaimed", classID);
      }
   }

   // release lock
   NFUnlock();
}

int
NFClass_LookupByName(char *name)
{
   List_Links *elt;
   int classID;

   // acquire lock
   NFLock();

   // search list for match
   classID = NF_CLASS_ID_NONE;
   LIST_FORALL(&nfClasses, elt) {
      NFClass *c = (NFClass *) elt;
      if (strcmp(c->name, name) == 0) {
         classID = c->id;
         break;
      }
   }
   
   // release lock
   NFUnlock();

   // debugging
   if (NF_DEBUG_VERBOSE) {
      LOG(0, "\"%s\" => %d", name, classID);
   }

   return(classID);
}

static void
NFFilterPeriodic(void *data, UNUSED_PARAM(Timer_AbsCycles timestamp))
{
   NFFilter *f = (NFFilter *) data;

   // execute timer function for passed in filter instance
   NFFilterLock(f);

   (*f->periodic)(f->state);
   
   /* refCount == 1 implies the timer routine is the only
    * one holding a reference to the filter object
    */
   if (f->refCount == 1) {
      if (NF_DEBUG) {
         LOG(0, "Removing timer for %s\n", f->name);
      }
      /* There should be no race here because we are removing a 
       * timer from within a callback, there can be no cocurrent callbacks
       * and timers are fixed to a particular PCPU
       */
      Timer_Remove(f->timerHandle);
      NFFilterRelease(f, "NFFilterPeriodic");
   }  
   
   NFFilterUnlock(f);

   /* 
    * Drain send and drop queues 
    */
   NFFilter_DrainSendQueue(NULL);
   NFFilter_DrainDropQueue(NULL);
}

static void
NFClassReapInstances(void *data, UNUSED_PARAM(Timer_AbsCycles timestamp))
{
   NFClass *c = (NFClass *) data;
   List_Links reapQueue;
   List_Links *elt;

   // initialize
   List_Init(&reapQueue);

   // acquire instances lock
   NFClassLockInstances(c);

   // remove any unreferenced instances
   elt = List_First(&c->instances);
   while (!List_IsAtEnd(elt, &c->instances)) {
      List_Links *next = List_Next(elt);
      NFFilter *f = (NFFilter *) elt;

      NFFilterLock(f);
      if (f->refCount == 0) {
         // move filter to reapQueue
         List_Remove(elt);
         List_Insert(elt, LIST_ATREAR(&reapQueue));
      }
      NFFilterUnlock(f);

      elt = next;
   }

   // release instances lock
   NFClassUnlockInstances(c);

   // destroy all filters on reapQueue
   while (!List_IsEmpty(&reapQueue)) {
      NFFilter *f = (NFFilter *) List_First(&reapQueue);
      List_Remove(&f->links);
      NFFilterDestroy(f);
   }
}

/*
 * NFFilter operations
 */

static NFFilter *
NFFilterNew(int id, char *name, NFClass *c, void *state)
{
   NFFilter *f;
   
   // allocate storage, fail if unable
   if ((f = NFALLOC(NFFilter)) == NULL) {
      return(NULL);
   }

   // initialize
   List_InitElement(&f->links);
   SP_InitLock("NFFilterLock", &f->lock,
                  SP_RANK_RECURSIVE_FLAG | SP_RANK_NF_FILTER);
   f->id = id;
   f->name[0] = '\0';
   f->refCount = 1;
   f->nfClass = c;
   f->state = state;
   f->forward = NULL;

   // set name, if valid
   if (name != NULL) {
      if (strlen(name) < NF_INSTANCE_NAME_LEN) {
         strcpy(f->name, name);
      }
   }

   return(f);
}

static void
NFFilterFree(NFFilter *f)
{
   SP_CleanupLock(&f->lock);
   Mem_Free(f);
}

// requires: caller holds f lock
static void
NFFilterRelease(NFFilter *f, const char *debug)
{
   // remove reference
   f->refCount--;

   // prepare to reap if last reference
   if (f->refCount == 0) {
      // schedule callback to reap filter
      Timer_Add(HOST_PCPU,
                NFClassReapInstances,
                NF_REAP_DELAY_MS,
                TIMER_ONE_SHOT,
                f->nfClass);

      // debugging
      if (NF_DEBUG) {
         LOG(0, "%s: %s.%d.%s: refCount==0",
             debug,
             f->nfClass->name,
             f->id,
             f->name);
      }
   }
}

static int
NFFilterProcRead(Proc_Entry *entry, char *buffer, int *len)
{
   NFFilter *f = (NFFilter *) entry->private;
   NFOps *ops = &f->nfClass->ops;

   // initialize
   *len = 0;

   // acquire lock
   NFFilterLock(f);

   // common status info
   Proc_Printf(buffer, len,
	       "netfilter %s.%d.%s: class %d, instance %d\n",
	       f->nfClass->name,
	       f->id,
	       f->name,
	       f->nfClass->id,
	       f->id);
   if (f->forward != NULL) {
      Proc_Printf(buffer, len,
		  "forwards: %s.%d.%s\n",
		  f->forward->nfClass->name,
		  f->forward->id,
		  f->forward->name);
   }

   // debugging
   if (NF_DEBUG) {
      Proc_Printf(buffer, len,
		  "debug: refCount %d\n",
		  f->refCount);
   }

   // invoke registered status operation, if any
   if (ops->status != NULL) {
      *len += (*ops->status)(f->state, buffer + *len, NF_PROC_BUF_SIZE - *len);
   }

   // release lock, succeed
   NFFilterUnlock(f);
   return(0);
}

static int
NFFilterProcWrite(Proc_Entry *entry, char *buffer, int *len)
{
   NFFilter *f = (NFFilter *) entry->private;
   NFOps *ops = &f->nfClass->ops;
   char *argv[NF_CMD_ARGS_MAX];
   int argc;

   // fail if no registered command op
   if (ops->command == NULL) {
      return(NF_FAILURE);
   }

   // parse args (assumes OK to overwrite buffer)
   argc = NF_ParseArgs(buffer, argv, NF_CMD_ARGS_MAX);

   // invoke register command op
   NFFilterLock(f);
   (*ops->command)(f->state, argc, argv);
   NFFilterUnlock(f);

   // succeed
   return(0);
}

static void
NFFilterCreateProc(void *data, UNUSED_PARAM(Timer_AbsCycles timestamp))
{
   NFFilter *f = (NFFilter *) data;
   char nameBuf[NF_PROC_NAME_SIZE];

   // acquire lock
   NFFilterLock(f);

   // initialize procfs entry
   f->procEntry.parent   = &nfProcXmitDir;
   f->procEntry.private  = f;
   f->procEntry.read     = NFFilterProcRead;
   f->procEntry.write    = NFFilterProcWrite;
   f->procEntry.canBlock = FALSE;

   // add procfs entry, drop reference
   snprintf(nameBuf, sizeof nameBuf, "%s.%d.%s", f->nfClass->name, f->id, f->name);
   Proc_Register(&f->procEntry, nameBuf, FALSE);
   NFFilterRelease(f, "NFFilterCreateProc");

   // release lock
   NFFilterUnlock(f);
}

/* Schedule a netfilter timer on the most suitable PCPU
 * Currently we pick PCPUs on a round-robin basis based
 * on the f->id count. At some point if there is a good
 * reason to we can make timer placement dynamic based on
 * PCPU load. But for now we statically assign PCPUs to
 * timers.
 */
inline void
NFFilterTimerSchedule(NFFilter *f)
{
   int pcpu;

   f->refCount++;
   pcpu = f->id % numPCPUs;
   f->timerHandle = Timer_Add(pcpu, NFFilterPeriodic,
                              f->period, TIMER_PERIODIC, f);
}


/*
 * Should be called with NFLock held 
 *
 */
static NFFilter *
NFFilterCreate(int classID, char *name, int argc, char **argv)
{
   int status, id;
   void *nfPrivate;
   NFFilter *f;
   NFClass *c;

   // lookup class, fail if not found
   c = NFClassLookupByID(classID);
   if (c == NULL) {
      return(NULL);
   }
   
   // create new filter instance
   status = (*c->ops.create)(argc, argv, &nfPrivate);
   if (status != NF_SUCCESS) {
      return(NULL);
   }

   // construct filter container
   id = c->nextInstanceID++;
   f = NFFilterNew(id, name, c, nfPrivate);
   if (f == NULL) {
      // destroy nfPrivate, release lock, and fail
      (void) (*c->ops.destroy)(nfPrivate);
      return(NULL);
   }

   // update active instances, add timer-based callback
   NFClassLockInstances(c);
   if ((c->ops.periodic != NULL) && (c->ops.period > 0)) {
      f->periodic = c->ops.periodic;
      f->period = c->ops.period;
      /* Add filter timer */
      NFFilterTimerSchedule(f);
   }

   List_Insert(&f->links, LIST_ATREAR(&c->instances));
   NFClassUnlockInstances(c);

   // schedule callback to register procfs entry
   f->refCount++;
   Timer_Add(HOST_PCPU,
             NFFilterCreateProc,
             NF_PROC_DELAY_MS,
             TIMER_ONE_SHOT,
             f);

   // successful instantiation
   return(f);
}

static int
NFFilterDestroy(NFFilter *f)
{
   NFClass *c;
   int status;

   // acquire lock
   NFLock();

   // sanity check
   ASSERT(f->refCount == 0);

   // obtain class from instance
   c = f->nfClass;

   // debugging
   if (NF_DEBUG) {
      LOG(0, "destroy filter %s.%d.%s", c->name, f->id, f->name);
   }

   // remove procfs entry
   Proc_Remove(&f->procEntry);

   // destroy filter instance
   status = (*c->ops.destroy)(f->state);
   
   // reclaim storage
   NFFilterFree(f);

   // release lock
   NFUnlock();      

   return(status);
}

int
NFFilter_Filter(NFFilter *filter, NFPacket *pkt)
{
   int status;
   NFFilter *f;
   World_Handle *world, *leader;

   leader = NULL;
   /*
    * filter == NULL implies we need to grab the NetFilter structure
    * from the packet. In order to do so safely we first need to grab
    * the transmitFilterLock. This locks prevents the list of NetFilter
    * structures from being modified. 
    * In the other case, when filter != NULL, we can safely access it
    * because the caller ensured the filter ref count was bumped up
    * (This is how it always worked. I am just pointing it out because
    * it was non-obvious to me)
    */
   if (filter == NULL) {


      world = World_FindNoRefCount(pkt->srcWorldID);
      if (world == NULL) {
         NFInsertDropPacket(pkt);
         return NF_FAILURE;
      }
      leader = World_GetVmmLeader(world);
      SP_Lock(leader->nfInfo.transmitFilterLock);

      f = leader->nfInfo.transmitFilter;
      if (f  == NULL) {
         SP_Unlock(leader->nfInfo.transmitFilterLock);
         NFInsertDropPacket(pkt);
         return NF_FAILURE;
      }
   } else {
      f = filter; 
   }

   NFFilterLock(f);

   // add reference, associate filter and packet
   f->refCount++;
   pkt->filter = f;

   // execute registered filter operation
   status = (*f->nfClass->ops.filter)(f->state, pkt);

   NFFilterUnlock(f);

   if (filter == NULL) {
      ASSERT(leader != NULL);
      SP_Unlock(leader->nfInfo.transmitFilterLock);
   }

   return NF_SUCCESS;
}


/*
 *----------------------------------------------------------------------
 *
 * NF_WorldInit --
 *
 *      Initialize network filtering state for "world".
 *
 * Results:
 *      Returns VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 *	Allocates network filter locks for "world".
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
NF_WorldInit(World_Handle *world, UNUSED_PARAM(World_InitArgs *args))
{
   World_NFInfo *nfInfo = &world->nfInfo;

   // sanity check
   ASSERT(!nfInfo->initialized);
   
   // initialize transmit filter state, fail if unable
   nfInfo->transmitFilterLock = (SP_SpinLock *) World_Alloc(world, sizeof(SP_SpinLock));
   if (nfInfo->transmitFilterLock == NULL) {
      return VMK_NO_MEMORY;
   }
   SP_InitLock("WorldTransmitFilterLock",
               nfInfo->transmitFilterLock,
               SP_RANK_NF_TRANSMIT);
   nfInfo->transmitFilter = NULL;

   // initialize receive filter state, fail if unable
   nfInfo->receiveFilterLock = (SP_SpinLock *) World_Alloc(world, sizeof(SP_SpinLock));
   if (nfInfo->receiveFilterLock == NULL) {
      SP_CleanupLock(nfInfo->transmitFilterLock);
      World_Free(world, nfInfo->transmitFilterLock);
      nfInfo->transmitFilterLock = NULL;
      return VMK_NO_MEMORY;
   }
   SP_InitLock("WorldReceiveFilterLock",
               nfInfo->receiveFilterLock,
               SP_RANK_NF_RECEIVE);
   nfInfo->receiveFilter = NULL;

   // initialization successful
   nfInfo->initialized = TRUE;
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * NF_WorldCleanup --
 *
 *      Free all resources related to network filtering for "world".
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	Reclaims dynamically-allocated network filter locks for "world".
 *
 *----------------------------------------------------------------------
 */
void
NF_WorldCleanup(World_Handle *world)
{
   World_NFInfo *nfInfo = &world->nfInfo;

   // done if uninitialized
   if (!nfInfo->initialized) {
      return;
   }

   // detach network filters from world, if any
   NF_WorldDetachFilters(world);

   // reclaim transmit lock
   ASSERT(nfInfo->transmitFilterLock != NULL);
   if (nfInfo->transmitFilterLock != NULL) {
      SP_CleanupLock(nfInfo->transmitFilterLock);
      World_Free(world, nfInfo->transmitFilterLock);
      nfInfo->transmitFilterLock = NULL;
   }

   // reclaim receive lock
   ASSERT(nfInfo->receiveFilterLock != NULL);
   if (nfInfo->receiveFilterLock != NULL) {
      SP_CleanupLock(nfInfo->receiveFilterLock);
      World_Free(world, nfInfo->receiveFilterLock);
      nfInfo->receiveFilterLock = NULL;
   }

   // clear flag
   nfInfo->initialized = FALSE;
}

// add "push" to end of filter chain starting with "current"
// XXX locking entire chain may be overkill
static void
NFPushFilter(NFFilter *current, NFFilter *push)
{
   NFFilterLock(current);
   if (current->forward == NULL) {
      // base case
      current->forward = push;
   } else {
      // recursive case
      NFPushFilter(current->forward, push);
   }
   NFFilterUnlock(current);
}

int
NF_WorldTransmitPush(World_Handle *world,
                     int classID,
                     int argc,
                     char **argv)
{
   char nameBuf[NF_INSTANCE_NAME_LEN];
   World_NFInfo *nfInfo = &world->nfInfo;
   NFFilter *newFilter;

   // fail if uninitialized
   if (!nfInfo->initialized) {
      return(NF_FAILURE);
   }

   // acquire module lock, world filter lock
   NFLock();
   SP_Lock(nfInfo->transmitFilterLock);

   // enforce single filter per world, if specified
   if (NF_SINGLE_WORLD_FILTER) {
      if (nfInfo->transmitFilter != NULL) {
         // release lock, fail 
         SP_Unlock(nfInfo->transmitFilterLock);
         NFUnlock();
         return(NF_FAILURE);         
      }
   }

   // construct filter name
   //   for now, simply use world id
   snprintf(nameBuf, sizeof nameBuf, "%d", world->worldID);

   // create filter instance, fail if unable
   newFilter = NFFilterCreate(classID, nameBuf, argc, argv);
   if (newFilter == NULL) {
      // release lock, fail 
      SP_Unlock(nfInfo->transmitFilterLock);
      NFUnlock();
      return(NF_FAILURE);
   }

   // attach filter to world
   if (nfInfo->transmitFilter == NULL) {
      // special case: push first filter
      nfInfo->transmitFilter = newFilter;
   } else {
      // push additional filter
      NFPushFilter(nfInfo->transmitFilter, newFilter);
   }

   // release locks, succeed
   SP_Unlock(nfInfo->transmitFilterLock);
   NFUnlock();
   return(NF_SUCCESS);
}

static int
NFWorldTransmitPushProc(UNUSED_PARAM(Proc_Entry *entry),
                        char *buffer,
                        int *len)
{
   int argc, classID, status;
   World_ID worldID;
   char *argv[NF_CLASS_ARGS_MAX];
   World_Handle *world, *leader;
   char *className;

   // parse args: <worldID> <className> (assumes OK to overwrite buffer)
   argc = NF_ParseArgs(buffer, argv, NF_CLASS_ARGS_MAX);
   if (argc < 2) {
      Warning("xmitpush failed: too few arguments");
      return(NF_FAILURE);
   }
   worldID = NF_ParseInt(argv[0]);
   className = argv[1];
   
   // lookup class by name, fail if not found
   classID = NFClass_LookupByName(className);
   if (classID == NF_CLASS_ID_NONE) {
      Warning("xmitpush failed: nfclass %s not found", className);
      return(NF_FAILURE);
   }
   
   // lookup world by id, fail if not found
   world = World_FindNoRefCount(worldID);
   if (world == NULL) {
      Warning("xmitpush failed: vm %d not found", worldID);
      return(NF_FAILURE);
   }

   // filters are associated with vmm leader
   leader = World_GetVmmLeader(world);
   if (leader == NULL) {
      VmWarn(worldID, "xmitpush failed: group leader not found");
      return(NF_FAILURE);
   }  

   // create and push filter
   status = NF_WorldTransmitPush(leader, classID, argc - 2, &argv[2]);
   if (status != NF_SUCCESS) {
      VmWarn(worldID, "xmitpush failed: class %d", classID);
   }

   // release world, done
   World_ReleaseNoRefCount(world);   
   return(status);
}

// requires: "current" is not last filter in list
// pop from end of filter chain starting with "current"
// XXX locking entire chain is overkill
static void
NFPopFilter(NFFilter *current)
{
   NFFilter *next;

   NFFilterLock(current);
   next = current->forward;
   ASSERT(next != NULL);
   if (next->forward == NULL) {
      // pop next
      current->forward = NULL;
      NFFilterRelease(next, "PopFilter");
   } else {
      // recursive case
      NFPopFilter(next);
   }
   NFFilterUnlock(current);
}

int
NF_WorldTransmitPop(World_Handle *world)
{
   World_NFInfo *nfInfo = &world->nfInfo;

   // fail if uninitialized
   if (!nfInfo->initialized) {
      return(NF_FAILURE);
   }

   // acquire world filter lock
   SP_Lock(nfInfo->transmitFilterLock);

   // sanity check
   if (nfInfo->transmitFilter == NULL) {
      // release lock, fail
      SP_Unlock(nfInfo->transmitFilterLock);
      return(NF_FAILURE);
   }

   // detach filter from world
   if (nfInfo->transmitFilter->forward == NULL) {
      // convenient abbrev
      NFFilter *f = nfInfo->transmitFilter;

      // detach only filter
      nfInfo->transmitFilter = NULL;

      NFFilterLock(f);
      NFFilterRelease(f, "NF_WorldTransmitPop");
      NFFilterUnlock(f);
   } else {
      // detach last filter
      NFPopFilter(nfInfo->transmitFilter);
   }

   // release lock, succeed
   SP_Unlock(nfInfo->transmitFilterLock);
   return(NF_SUCCESS);
}

static int
NFWorldTransmitPopProc(UNUSED_PARAM(Proc_Entry *entry),
                       char *buffer,
                       int *len)
{
   char *argv[NF_CLASS_ARGS_MAX];
   int argc, status;
   World_ID worldID;
   World_Handle *world, *leader;

   // parse args: <worldID> (assumes OK to overwrite buffer)
   argc = NF_ParseArgs(buffer, argv, NF_CLASS_ARGS_MAX);
   if (argc != 1) {
      Warning("xmitpop failed: wrong number of arguments");
      return(NF_FAILURE);
   }
   worldID = NF_ParseInt(argv[0]);

   // lookup world by id, fail if not found
   world = World_FindNoRefCount(worldID);
   if (world == NULL) {
      Warning("xmitpop failed: vm %d not found", worldID);
      return(NF_FAILURE);
   }
   leader = World_GetVmmLeader(world);
   if (leader == NULL) {
      VmWarn(worldID, "xmitpop failed: group leader not found");
      return(NF_FAILURE);
   }

   // pop filter
   status = NF_WorldTransmitPop(leader);
   if (status != NF_SUCCESS) {
      VmWarn(worldID, "xmitpop failed");
   }
   
   // release world, done
   World_ReleaseNoRefCount(world);   
   return(status);
}

void
NF_WorldDetachFilters(World_Handle *world)
{
   // debugging
   if (NF_DEBUG) {
      VMLOG(0, world->worldID, "detaching filters");
   }

   // pop all transmit filters
   while (NF_WorldTransmitPop(world) == NF_SUCCESS) {
      // repeat until no remaining filters
   }

   // XXX pop all receive filters
}

/*
 *----------------------------------------------------------------------
 *
 * NFStatusProcRead --
 *
 *      Network filtering general status reporting routine.
 *
 * Results:
 *      Writes ASCII status information into "buffer".
 *	Sets "length" to number of bytes written.
 *	Returns 0 iff successful.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
NFStatusProcRead(UNUSED_PARAM(Proc_Entry *entry), char *buffer, int *len)
{
   List_Links *classElt;
   int nWorlds, i;
   World_ID *worldID;

   // allocate memory for array of all world ids
   nWorlds = MAX_WORLDS;
   worldID = Mem_Alloc(sizeof(World_ID) * nWorlds);
   if (worldID == NULL) {
      return VMK_NO_MEMORY;
   }

   // obtain snapshot of world ids
   (void) World_AllWorlds(worldID, &nWorlds);

   // acquire global lock
   NFLock();

   *len = 0;

   Proc_Printf(buffer, len, "classes:\n");

   // report classes
   LIST_FORALL(&nfClasses, classElt) {
      NFClass *c = (NFClass *) classElt;
      int instanceCount = 0;
      List_Links *instanceElt;

      NFClassLockInstances(c);
      LIST_FORALL(&c->instances, instanceElt) {
         // NFFilter *f = (NFFilter *) instanceElt;
         instanceCount++;
      }
      NFClassUnlockInstances(c);

      Proc_Printf(buffer, len,
		  "  %-8s (%d instances)\n",
		  c->name,
		  instanceCount);
   }

   // report filters by world
   for (i = 0; i < nWorlds; i++){
      World_Handle *world = World_FindNoRefCount(worldID[i]);
      if (world != NULL) {
         World_NFInfo *nfInfo = &world->nfInfo;
         if (nfInfo->transmitFilter != NULL) {
            NFFilter *f;

            SP_Lock(nfInfo->transmitFilterLock);

	    Proc_Printf(buffer, len,
			"world %d:\n",
			world->worldID);

            // n.b. filter locking unnecessary, since holding nfLock,
            //      which is required by NFFilterDestroy()
            for (f = nfInfo->transmitFilter;
                 f != NULL;
                 f = f->forward) {
	       Proc_Printf(buffer, len,
			   "  %s.%d.%s\n",
			   f->nfClass->name,
			   f->id,
			   f->name);
            }

            SP_Unlock(nfInfo->transmitFilterLock);
         }

         World_ReleaseNoRefCount(world);
      }
   }
      
   // release lock
   NFUnlock();

   // reclaim memory
   Mem_Free(worldID);

   // success
   return(0);
}

/*
 * Module initialization
 */

/*
 *----------------------------------------------------------------------
 *
 * NFInitProc --
 *
 *      Initializes network filter procfs tree.
 *
 * Results:
 *      None. 
 *
 * Side effects:
 *      Registers procfs nodes to control network filtering.
 *
 *----------------------------------------------------------------------
 */
static void
NFInitProc(void)
{
   // top-level "filters" procfs directory
   Proc_InitEntry(&nfProcDir);
   Proc_Register(&nfProcDir, "filters", TRUE);

   // "xmit" subdirectory for filter instances
   Proc_InitEntry(&nfProcXmitDir);
   nfProcXmitDir.parent = &nfProcDir;
   Proc_Register(&nfProcXmitDir, "xmit", TRUE);

   // "xmitpush" command entry
   Proc_InitEntry(&nfProcXmitPush);
   nfProcXmitPush.parent = &nfProcDir;
   nfProcXmitPush.write = NFWorldTransmitPushProc;
   Proc_Register(&nfProcXmitPush, "xmitpush", FALSE);

   // "xmitpop" command entry
   Proc_InitEntry(&nfProcXmitPop);
   nfProcXmitPop.parent = &nfProcDir;
   nfProcXmitPop.write = NFWorldTransmitPopProc;
   Proc_Register(&nfProcXmitPop, "xmitpop", FALSE);

   // "status" entry
   Proc_InitEntry(&nfProcStatus);
   nfProcStatus.parent = &nfProcDir;
   nfProcStatus.read = NFStatusProcRead;
   Proc_Register(&nfProcStatus, "status", FALSE);   
}

/*
 *----------------------------------------------------------------------
 *
 * NF_Init --
 *
 *      Initializes network filter module.
 *
 * Results:
 *      None. 
 *
 * Side effects:
 *      Modifies global network filter state.
 *
 *----------------------------------------------------------------------
 */
void
NF_Init(void)
{
   // log initialization message
   LOG(0, "network filtering initialized");

   // initialize global state
   SP_InitLock("NetFilterLock", &nfLock, SP_RANK_NF_NETFILTER);
   SP_InitLock("sendQueueLock", &sendQueueLock, SP_RANK_NF_SENDQUEUE);
   SP_InitLock("dropQueueLock", &dropQueueLock, SP_RANK_NF_DROPQUEUE);
   
   List_Init(&nfClasses);
   List_Init(&sendQueue);
   List_Init(&dropQueue);
   nextClassID = 0;

   // initialize procfs entries
   NFInitProc();
}
