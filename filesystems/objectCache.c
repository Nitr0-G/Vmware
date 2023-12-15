/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * objectCache.c --
 *
 *    The file system object cache.
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "hash.h"
#include "objectCache.h"
#include "libc.h"
#include "fs_ext.h"
#include "fsSwitch.h"
#include "semaphore_ext.h"

#define LOGLEVEL_MODULE OC
#define LOGLEVEL_MODULE_LEN 3
#include "log.h"


/*
 * Hash table containing object descriptors.
 */
typedef struct HTBucket {
   uint32           size;   /* number of elements */
   ObjDescriptorInt *head;  /* head of chain */
} HTBucket;

static Semaphore objDescTableLock;
static HTBucket  *objDescTable;
static uint32    objDescTableSize = 0;

/*
 * List containing descriptors of opened volumes. The reasons for
 * keeping this separate will be described here (!!! todo).
 */
extern Semaphore fsLock;
ObjDescriptorInt *openVolList = NULL;

/*
 * Statistics
 */
static uint32 lookupHits = 0, lookupMisses = 0;
static uint32 hashInserts = 0;
static uint32 hashCollisions = 0;


static INLINE uint32 OCGetBucketIdx(FSS_ObjectID *key);
static INLINE Bool OCEqualOIDs(const FSS_ObjectID *oid1, const FSS_ObjectID *oid2);

static VMK_ReturnStatus OCHashInit(uint32 numBuckets);
static VMK_ReturnStatus OCHashLookup(FSS_ObjectID *key, ObjDescriptorInt **ptr);
static VMK_ReturnStatus OCHashInsert(ObjDescriptorInt *desc);
static void OCHashRemove(ObjDescriptorInt *desc);

#if 0
static VMK_ReturnStatus OCHashResize(uint32 newNumBuckets);
static VMK_ReturnStatus OCEvictObject(FSS_ObjectID *oid);
#endif

static VMK_ReturnStatus OCListLookup(FSS_ObjectID *oid, ObjDescriptorInt **desc);
static VMK_ReturnStatus OCListInsert(ObjDescriptorInt *desc);
static void OCListRemove(ObjDescriptorInt **list, ObjDescriptorInt *desc);

static VMK_ReturnStatus OCGetObject(FSS_ObjectID *oid, ObjDescriptorInt *desc);



VMK_ReturnStatus
OC_Init()
{
   VMK_ReturnStatus status;

   status = OCHashInit(OC_DEFAULT_NUM_BUCKETS);

   return status;
}


void
OC_Lock(uint32 type)
{
   switch (type)
      {
      case OC_OBJECTS:
	 Semaphore_Lock(&objDescTableLock);
	 break;
      }
}


void
OC_Unlock(uint32 type)
{
   switch (type)
      {
      case OC_OBJECTS:
	 Semaphore_Unlock(&objDescTableLock);
	 break;
      }
}


VMK_ReturnStatus
OC_LookupObject(FSS_ObjectID *oid,
		Bool reserveIfFound, Bool getLock,
		ObjDescriptorInt **desc)
{
   VMK_ReturnStatus status;
   ObjDescriptorInt *descPtr = NULL;

   if (!FSS_IsValidOID(oid)) {
      return VMK_BAD_PARAM;
   }

   if (getLock) {
      Semaphore_Lock(&objDescTableLock);
   } else {
      ASSERT(Semaphore_IsLocked(&objDescTableLock));
   }

   if (OCHashLookup(oid, &descPtr) == VMK_OK) {
      if (descPtr->refCount == -1) {
	 /*
	  * XXX This means the miss handler triggered by a previous
	  * ReserveObject() failed. As this is a lookup, we don't
	  * retry.
	  */
	 status = VMK_NOT_FOUND;
	 goto done;
      }

#if 0
      Semaphore_Lock(&descPtr->OCDescLock);
#endif
      ASSERT(descPtr->refCount >= 0);
      if (reserveIfFound) {
	 descPtr->refCount++;
      }
#if 0
      Semaphore_Unlock(&descPtr->OCDescLock);
#endif

      *desc = descPtr;
      status = VMK_OK;
   } else {
      status = VMK_NOT_FOUND;
   }

 done:
   if (getLock) {
      Semaphore_Unlock(&objDescTableLock);
   }
   return status;
}


/*
 * Looks up object named by 'oid'. Increments object descriptor
 * refCount, guaranteeing it will remain cached until ReleaseObject is
 * called. Sets '*desc' to point to object descriptor.
 *
 * Returns VMK_OK if the object was successfully reserved, or a VMK
 * error code otherwise.
 */
VMK_ReturnStatus
OC_ReserveObject(FSS_ObjectID *oid, ObjDescriptorInt **desc)
{
   VMK_ReturnStatus status;
   ObjDescriptorInt *newDesc, *descPtr;

   LOG(2, FS_OID_FMTSTR, FS_OID_VAARGS(oid));

   if (!FSS_IsValidOID(oid)) {
      return VMK_BAD_PARAM;
   }

   Semaphore_Lock(&objDescTableLock);
  
   /* Look up in cache. */
   if (OCHashLookup(oid, &descPtr) == VMK_OK) {
      Semaphore_Lock(&descPtr->OCDescLock);
      if (descPtr->refCount == -1) {
 	 newDesc = descPtr;
	 goto miss_handler;
      }

      ASSERT(descPtr->refCount >= 0);
      descPtr->refCount++;
      *desc = descPtr;

      Semaphore_Unlock(&descPtr->OCDescLock);
      Semaphore_Unlock(&objDescTableLock);
      return VMK_OK;
   }

   /* Not found in cache -- create new descriptor. */
   status = OC_CreateObjectDesc(&newDesc);
   if (status != VMK_OK) {
      Semaphore_Unlock(&objDescTableLock);
      return status;
   }

   FSS_CopyOID(&newDesc->oid, oid);

   /*
    * Lock descriptor. If reservation is attempted on object while
    * the miss is being handled, reserver will block.
    */
   Semaphore_Lock(&newDesc->OCDescLock);

   /* 
    * Insert descriptor into cache. This can fail if the cache is full
    * and no object can be evicted (all are in use).
    */
   status = OCHashInsert(newDesc);
   if (status != VMK_OK) {
      Semaphore_Unlock(&newDesc->OCDescLock);
      OC_DestroyObjectDesc(newDesc);
      Semaphore_Unlock(&objDescTableLock);
      return status;
   }

 miss_handler:
   /*
    * Release table lock before exiting module.
    * XXX removed for now, because of LIFO restriction in
    * Semaphore_Unlock().
    */
#if 0
   Semaphore_Unlock(&objDescTableLock);
#endif

   ASSERT(newDesc->refCount == -1);
   
   /* 
    * Call down to the FSS miss handler.
    */
   status = OCGetObject(oid, newDesc);
   if (status != VMK_OK) {
      /*
       * Miss handler failed. The descriptor remains in the cache, but
       * uninitialized. Waiters will recall the miss handler, failing
       * which the descriptor will be cleaned up during cache flush.
       */
      ASSERT(newDesc->refCount == -1);
      Semaphore_Unlock(&newDesc->OCDescLock);
      Semaphore_Unlock(&objDescTableLock);
      return status;
   }

   /*
    * Miss handler succeeded. The descriptor is now
    * initialized. Reserve it and proceed.
    */
   newDesc->refCount = 1;
   *desc = newDesc;

   Semaphore_Unlock(&newDesc->OCDescLock);
   Semaphore_Unlock(&objDescTableLock);
   return VMK_OK;
}


/*
 * Complement to OC_ReserveObject(). Decrements object descriptor
 * 'refCount' and calls last reference callback, if one is registered.
 */
VMK_ReturnStatus
OC_ReleaseObject(ObjDescriptorInt *desc)
{
   if (!FSS_IsValidOID(&desc->oid)) {
      return VMK_BAD_PARAM;
   }

   LOG(2, FS_OID_FMTSTR, FS_OID_VAARGS(&desc->oid));

   Semaphore_Lock(&objDescTableLock);
   Semaphore_Lock(&desc->OCDescLock);

   ASSERT(desc->refCount > 0);
   desc->refCount--;

   if (desc->refCount == 0) {
      FSS_ObjLastRefCB(desc);

      /* For now, we evict an object when its 'refCount' goes to 0. */
      FSS_ObjEvictCB(desc);
      OCHashRemove(desc);

      Semaphore_Unlock(&desc->OCDescLock);
      OC_DestroyObjectDesc(desc);
   } else {
      Semaphore_Unlock(&desc->OCDescLock);
   }

   Semaphore_Unlock(&objDescTableLock);
   return VMK_OK;
}


#if 0
static VMK_ReturnStatus
OCEvictObject(FSS_ObjectID *oid)
{
   return VMK_NOT_IMPLEMENTED;
}
#endif

		   
VMK_ReturnStatus
OC_LookupVolume(FSS_ObjectID *oid, Bool reserveIfFound,
		ObjDescriptorInt **desc)
{
   VMK_ReturnStatus status;
   ObjDescriptorInt *vol;

   if (!FSS_IsValidOID(oid)) {
      return VMK_BAD_PARAM;
   }

   Semaphore_Lock(&fsLock);

   if (OCListLookup(oid, &vol) == VMK_OK) {
      if (reserveIfFound) {
	 ASSERT(vol->refCount >= 0);
	 
	 vol->refCount++;
	 *desc = vol;
      }

      *desc = vol;
      status = VMK_OK;
   } else {
      status = VMK_NOT_FOUND;
   }

   Semaphore_Unlock(&fsLock);
   return status;
}


VMK_ReturnStatus
OC_ReserveVolume(FSS_ObjectID *oid, ObjDescriptorInt **desc)
{
   VMK_ReturnStatus status;
   ObjDescriptorInt *newDesc, *descPtr;

   if (!FSS_IsValidOID(oid)) {
      return VMK_BAD_PARAM;
   }

   Semaphore_Lock(&fsLock);
  
   /* Look up in cache. */
   if (OCListLookup(oid, &descPtr) == VMK_OK) {
      ASSERT(descPtr->refCount >= 0);
      descPtr->refCount++;
      *desc = descPtr;

      status = VMK_OK;
      LOG(2, "Found "FS_OID_FMTSTR, FS_OID_VAARGS(oid));
      goto done;
   }

   /* Not found in cache -- create new descriptor. */
   status = OC_CreateObjectDesc(&newDesc);
   if (status != VMK_OK) {
      goto done;
   }

   FSS_CopyOID(&newDesc->oid, oid);

   ASSERT(newDesc->refCount == -1);
   
   /* 
    * Call down to the FSS miss handler.
    */
   status = OCGetObject(oid, newDesc);
   if (status != VMK_OK) {
      ASSERT(newDesc->refCount == -1);
      OC_DestroyObjectDesc(newDesc);
      goto done;
   }

   /*
    * Miss handler succeeded. The descriptor is now initialized.
    * Reserve it and proceed.
    */
   newDesc->refCount = 1;

   /* 
    * Insert descriptor into cache. This can fail if the cache is full
    * and no object can be evicted (all are in use).
    */
   status = OCListInsert(newDesc);
   if (status != VMK_OK) {
      OC_DestroyObjectDesc(newDesc);
      goto done;
   }
   LOG(2, "Inserted "FS_OID_FMTSTR, FS_OID_VAARGS(oid));
   *desc = newDesc;

 done:
   Semaphore_Unlock(&fsLock);
   return status;
}


VMK_ReturnStatus
OC_ReleaseVolume(ObjDescriptorInt *desc)
{
   if (!FSS_IsValidOID(&desc->oid)) {
      return VMK_BAD_PARAM;
   }

   LOG(2, FS_OID_FMTSTR, FS_OID_VAARGS(&desc->oid));

   Semaphore_Lock(&fsLock);

   ASSERT(desc->refCount > 0);
   desc->refCount--;

   if (desc->refCount == 0) {
      FSS_ObjLastRefCB(desc);

      /* For now, we evict a volume when its 'refCount' goes to 0. */
      FSS_ObjEvictCB(desc);
      OCListRemove(&openVolList, desc);
      OC_DestroyObjectDesc(desc);
   }

   Semaphore_Unlock(&fsLock);
   return VMK_OK;
}


VMK_ReturnStatus
OC_InsertVolume(ObjDescriptorInt *desc)
{
   VMK_ReturnStatus status;

   ASSERT(FSS_IsValidOID(&desc->oid));

   Semaphore_Lock(&fsLock);
   status = OCListInsert(desc);
   Semaphore_Unlock(&fsLock);

   return status;
}


VMK_ReturnStatus
OC_RemoveVolume(ObjDescriptorInt *desc, Bool getLock)
{
   ASSERT(FSS_IsValidOID(&desc->oid));

   if (getLock) {
      Semaphore_Lock(&fsLock);
   } else {
      ASSERT(Semaphore_IsLocked(&fsLock));
   }

   OCListRemove(&openVolList, desc);

   if (getLock) {
      Semaphore_Unlock(&fsLock);
   }

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * OCHashOID --
 *    Returns a 32-bit hash of the specified OID.
 *
 * Results:
 *
 * Side effects:
 *
 *----------------------------------------------------------------------
 */
static INLINE uint32
OCHashOID(FSS_ObjectID *oid)
{
   /*
    * Hash only the OID data as fsTypenum & length won't provide much
    * information.
    */
   return Hash_Bytes(oid->oid.data, oid->oid.length);
}

static INLINE uint32
OCGetBucketIdx(FSS_ObjectID *key)
{
   return (OCHashOID(key) % objDescTableSize);
}

#define FS_OID(fss_oid_ptr) ((fss_oid_ptr)->oid)

static INLINE Bool
OCEqualOIDs(const FSS_ObjectID *oid1, const FSS_ObjectID *oid2)
{
   return (oid1->fsTypeNum == oid2->fsTypeNum &&
	   FS_OID(oid1).length == FS_OID(oid2).length &&
	   memcmp(FS_OID(oid1).data, FS_OID(oid2).data,
		  FS_OID(oid1).length) == 0);
}

static VMK_ReturnStatus
OCHashInit(uint32 numBuckets)
{
   objDescTable = (HTBucket *) Mem_Alloc(sizeof(HTBucket) * numBuckets);
   if (objDescTable == NULL) {
      return VMK_NO_MEMORY;
   }

   memset(objDescTable, 0, sizeof(HTBucket) * numBuckets);
   objDescTableSize = numBuckets;

   Semaphore_Init("objDescTable", &objDescTableLock, 1,
		  OC_SEMA_RANK_OBJDESC_TABLE);

   return VMK_OK;
}

/*
 * Requires objDescTableLock to be held.
 */
static VMK_ReturnStatus
OCHashInsert(ObjDescriptorInt *desc)
{
   uint32 bucketIdx;

   ASSERT(FSS_IsValidOID(&desc->oid));
   ASSERT(Semaphore_IsLocked(&objDescTableLock));

   bucketIdx = OCGetBucketIdx(&desc->oid);

   desc->next = objDescTable[bucketIdx].head;
   objDescTable[bucketIdx].head = desc;
   objDescTable[bucketIdx].size++;

   hashInserts++;
   if (objDescTable[bucketIdx].size > 1) {
      hashCollisions++;
   }

   return VMK_OK;
}

static void
OCHashRemove(ObjDescriptorInt *desc)
{
   uint32 bucketIdx;

   ASSERT(FSS_IsValidOID(&desc->oid));
   ASSERT(Semaphore_IsLocked(&objDescTableLock));

   bucketIdx = OCGetBucketIdx(&desc->oid);
   OCListRemove(&objDescTable[bucketIdx].head, desc);

   objDescTable[bucketIdx].size--;
}

/*
 * Places a pointer to the relevant object descriptor in '*ptr'.
 * Requires objDescTableLock to be held.
 */
static VMK_ReturnStatus
OCHashLookup(FSS_ObjectID *key, ObjDescriptorInt **ptr)
{
   uint32 bucketIdx;
   ObjDescriptorInt *dp;

   ASSERT(Semaphore_IsLocked(&objDescTableLock) == TRUE);

   bucketIdx = OCGetBucketIdx(key);

   dp = objDescTable[bucketIdx].head;
   while (dp) {
      if (FSS_OIDIsEqual(key, &dp->oid)) {
	 *ptr = dp;
	 lookupHits++;
	 return VMK_OK;
      }

      dp = dp->next;
   }

   lookupMisses++;
   return VMK_NOT_FOUND;
}

#if 0
/*
 * Resize the hash table while retaining its original contents. 
 */
static VMK_ReturnStatus
OCHashResize(uint32 newNumBuckets)
{
   return VMK_NOT_IMPLEMENTED;
}
#endif


static VMK_ReturnStatus
OCListLookup(FSS_ObjectID *oid, ObjDescriptorInt **desc)
{
   ObjDescriptorInt *vol;

   ASSERT(Semaphore_IsLocked(&fsLock));

   for (vol = openVolList; vol != NULL; vol = vol->next) {
      if (FSS_OIDIsEqual(&vol->oid, oid)) {
	 *desc = vol;
	 lookupHits++;
	 return VMK_OK;
      }
   }

   lookupMisses++;
   return VMK_NOT_FOUND;
}


static VMK_ReturnStatus
OCListInsert(ObjDescriptorInt *desc)
{
   ASSERT(Semaphore_IsLocked(&fsLock));

   desc->next = openVolList;
   openVolList = desc;

   return VMK_OK;
}


static void
OCListRemove(ObjDescriptorInt **list, ObjDescriptorInt *desc)
{
   ObjDescriptorInt *prev, *vol;

   for (prev = NULL, vol = *list; vol != NULL;
	prev = vol, vol = vol->next) {
      if (vol == desc) {
	 if (prev == NULL) {
	    *list = vol->next;
	 } else {
	    prev->next = vol->next;
	 }
         return;
      }
   }
}


/*
 * Allocates space for an object descriptor and initializes general
 * fields. Sets '*desc' to point to newly created descriptor.
 */
VMK_ReturnStatus
OC_CreateObjectDesc(ObjDescriptorInt **desc)
{
   ObjDescriptorInt *newDesc;

   newDesc = (ObjDescriptorInt *) Mem_Alloc(sizeof(ObjDescriptorInt));
   if (newDesc == NULL) {
      return VMK_NO_MEMORY;
   }

   memset(newDesc, 0, sizeof(ObjDescriptorInt));
   newDesc->refCount = -1;

   Semaphore_Init("OCDescLockObj", &newDesc->OCDescLock, 1,
		  OC_SEMA_RANK_OCDESC_OBJ);

   *desc = newDesc;
   return VMK_OK;
}

/*
 * Call down to the FSS to initialize the FSS and file system specific
 * fields in the descriptor.
 */
static VMK_ReturnStatus
OCGetObject(FSS_ObjectID *oid, ObjDescriptorInt *desc)  // OUT
{
   return FSS_GetObject(oid, desc);
}

VMK_ReturnStatus
OC_DestroyObjectDesc(ObjDescriptorInt *desc)
{
//XXX FIXME: device may be left open
   Semaphore_Cleanup(&desc->OCDescLock);
   Mem_Free(desc);

   return VMK_OK;
}
