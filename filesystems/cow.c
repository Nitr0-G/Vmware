/* ***********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * cow.c --
 *
 *    copy-on-write mechanism for vmkernel disks
 */

#include <stdarg.h>

#include "vm_types.h"
#include "vm_libc.h"
#include "libc.h"	// for strncpy
#include "vmkernel.h"	// For Log(), ASSERT()
#include "fs_ext.h"
#include "semaphore_ext.h"
#include "kvmap.h"
#include "memmap.h"
#include "prda.h"
#include "vm_device_version.h"
#include "fsSwitch.h"
#include "fss_int.h"
#include "async_io.h"
#include "vmk_scsi.h" /* For the scsi calls */
#include "tlb.h"
#include "cow.h"
#include "cow_ext.h"
#include "fsNameSpace.h"
#include "fsClientLib.h"

#define LOGLEVEL_MODULE Cow
#include "log.h"

/*
 *  The following define the binary format of COW disks.
 *  See also cow_ext.h
 */

/*
 * RedoLog parameters regarding the size 
 */
#define COWDISK_MAX_REDOLOG_SIZE_IN_MB  2048
#define COWDISK_MIN_REDOLOG_SIZE_IN_MB  4
#define COWDISK_MIN_FREE_SPACE_IN_KB    4096

#define COW_NUM_FILE_HANDLES     512
#define COW_FILE_HANDLES_MASK    0x1ff

#define COW_NULL_SECTOR_NO    0xffffffff

/*
 * Leaf Entry in the COWCache
 */
typedef struct COWLeafEntry {
   uint32         sectorOffset[COW_NUM_LEAF_ENTRIES];
} COWLeafEntry;

/* Useful macros to deal with COW disks */
#define PAGES_PER_LEAFENTRY	CEIL(sizeof(COWLeafEntry),PAGE_SIZE)

/*
 * For each COW file, we have a fully associative cache of recently
 * accessed COW leaf entries.  This cache is important, because it
 * helps us avoid many synchronous reads and writes when accessing
 * leaf entries.
 */
#define NUM_LEAF_CACHE_ENTRIES	32

/*
 * Maximum wait time while allocating cache memory.
 */
#define	COW_CACHE_TIMEOUT_MS	(5000)

/*
 * The Meta Data pair which represents the sector in RedoLog Meta Data and 
 * the sector is storing
 */
typedef struct COWMDPair {
   uint32 sector;
   uint32 metaSector;
} COWMDPair;

/*
 * Entry in the leaf cache.
 */
typedef struct COWPair {
   uint32 sectorOffset;		/* Sector of leaf in COW file */
   int numWrites;		/* Whether cached entry is undergoing write */
   unsigned int lastTouch;	/* Last time cache entry was accessed */
   MPN mpns[PAGES_PER_LEAFENTRY];/* MPNs for leaf data */
   SP_SpinLock leafEntrySpin;    /* Lock for protection of leaf entry */
} COWPair;

/*
 * COW MetaData Entry that describes the meta data updates
 */
typedef struct COWMetaData {
   COWPair            *pair;            /* Entry in leaf describing the MetaData */
   COWMDPair          *metaPair;        /* Pair of leafPos and value of the sector */
   uint8              numIOs;           /* Number of outstanding IOs */
   struct COWMetaData *next;            /* Next ptr in the list of MetaData Blocks */
} COWMetaData;

/*
 * foward defn for the COW_MetaDataInfo
 */
typedef struct COW_MetaDataInfo COW_MetaDataInfo;
 
/*
 * the COW MetaData Queue 
 */ 
typedef struct COW_MDQ {
   COW_MetaDataInfo *head;
   COW_MetaDataInfo *tail;
} COW_MDQ; 

/*
 * Cached in-memory data for each of the RedoLogs.
 * This is the core of the COW which describes the RedoLog in detail
 */
typedef struct COWInfo {
   uint32          flags;
   uint32          granularity;	/* # of sectors pointed to by each leaf entry*/
   FS_FileHandleID fd;		/* In-memory file desc. corres. to COW disk */
   COWRootEntry    *rootEntries;	/* List of root entries from COW file */
   uint32          numRootEntries; /* # of root entries to cover whole disk*/
   uint32          rootOffset;	/* sector offset of root entries */
   uint32          freeSector;	/* Next avail sector in COW file */
   Bool		   freeSectorChanged;/* Whether freeSector changed since open */
   char            *tempSectorBuffer; /* Buffer for reading leaf blocks from
                                       parent disk, and also the header */
   int            opCount;	/* Number of reads & writes since open */
   uint32	  savedGeneration; /* In-memory savedGeneration */
   uint32	  numSectors;	/* Total capacity of disk */
   uint32	  allocSectors;	/* Allocated sectors of file */
   /* Fully associative cache of leaf entries from the COW file */
   COWPair	   cache[NUM_LEAF_CACHE_ENTRIES];
   COWLeafEntry	   *leafEntryAddr;/* VA used to access COWLeafEntry data */
   int32	   mapPcpuNum;	/* PCPU on which leafEntryAddr mapping is
                                 * valid */
   unsigned int	   cacheTime;	/* Clock used for setting lastTouch */
   COW_MDQ         active; /* Queue of active MetaData Updates */ 
   COW_MDQ         ready; /* Queue of pending/ready MetaData Updates */ 
   SP_SpinLock     queueLock;  /* Lock to protect the Queues */

   /* Statistics */
   int		   cacheLookups;
   int		   cacheHits;
   int		   initWrites;
   int		   dirtyWrites;
   int		   cacheReads;
#ifdef COW_TIMING
   int		   initTime;
   int		   dirtyTime;
   int		   readTime;
#endif
} COWInfo;

/*
 * Describes the necessary info for each RedoLog
 */
typedef struct COW_FSInfo {
   FS_FileHandleID fsFileHandleID; /* fid for the redolog/Base Disk */
   COWInfo *cowInfo; /* in core structure for the redolog or NULL for base disk */
} COW_FSInfo;

typedef struct COW_HandleInfo {
   COW_HandleID handleID; /* handle for the hierarchy of RedoLogs and base Disk */
   uint32       validRedos; /* num of RedoLogs in the hierarchy */ 
   COW_FSInfo   cowFSInfo[COW_MAX_REDO_LOG + 1]; /* FSInfo for base disk + redologs */
   Bool         inUse;
   RWSemaphore	rwlock;	// Lock to allow fid to be changed.
} COW_HandleInfo;

/*
 * describes the I/O information for the data writes 
 */
typedef struct COW_FSAsyncIOInfo {
   Async_Token *token; /* Child Token per IO*/
   FS_FileHandleID fileHandle; /* File HandleID of the redo undergoing IO */
   COW_HandleID handleID; /* handle for the hierarchy of RedoLogs and base Disk */
   SG_Array *sg_Arr; 
   uint32 length;
} COW_FSAsyncIOInfo;

/*
 * State information of the command in the state machine 
 */
typedef enum {
   COW_IO_INITIALIZED = 0,
   COW_DATAWRITE_PROG,
   COW_DATAWRITE_DONE,
   COW_CACHEUPDATE_DONE,
   COW_WAITING_FOR_MDIO,
   COW_METADATAWRITE_PROG,
   COW_METADATAWRITE_DONE,
} COW_IOState;

/*
 * describes the Meta Data I/O information for the data writes 
 */
struct COW_MetaDataInfo {
   COW_MetaDataInfo  *next;            /* Ptr to maintain the queue of the I/Os */   
   COW_MetaDataInfo  *prev;            /* Ptr to maintain the queue of the I/Os */   
   COW_IOState       ioState;          /* ioState */ 
   int               numLeafEntries;   /* Num of the leafEnteries involved in the IO */ 
   COWInfo           *info;            /* ptr to COWInfo */
   FS_FileHandleID   fileHandle;       /* File Handle for the I/O */
   SG_Array          *sg_Arr;          /* Scatter-Gather for the MetaData I/O */
   Async_Token       *parentToken;     /* Ptr to parent token */
   COWMetaData       *metaDataHead;    /* List of COWMetaData for the MetaData Updates */
   COW_FSAsyncIOInfo *cowIOInfo;       /* Data IO releted info */
   uint32            totalBlocks;      /* Total blocks involved in this MetaData block write */ 
};

/*
 * Used for the AsyncFrame for the MetaData writes
 */
typedef struct COW_MetaDataFrame {
   uint32 magic;
   COW_MetaDataInfo *cowMetaDataInfo;
} COW_MetaDataFrame;

/*
 * Global cow handle list
 */
static COW_HandleInfo cowFileHandleTable[COW_NUM_FILE_HANDLES];

/*
 * Protects the cowFileHandleTable
 */
static SP_SpinLock cowFileHandleLock; 

/*
 * Used for the reads from the different Redo Logs as data might be scattered 
 * across multiple RedoLogs
 */
typedef struct COW_SplitChildInfo {
   FS_FileHandleID fileHandle;
   Async_Token *parentToken;
   COW_FSAsyncIOInfo *cowIOInfo;
   uint32 dataIndex;
   uint32 validRedos;
   uint32 sgLen;
} COW_SplitChildInfo;

#define COW_ASYNC_COUNTER_MAGIC  0x5544

/*
 * Async Frame for the Reads to RedoLogs 
 */
typedef struct COW_AsyncCounter {
   uint32 magic;   
   uint32 needed;   
   uint32 handled;   
} COW_AsyncCounter;

/*
 * A macro for static functions in this module to assure we catch
 * errors (assert) in debug builds, but log an error and try to
 * recover gracefully in release builds.
 */
#define ASSERT_VALID_COWHANDLE(A) \
   do {                                         \
      ASSERT(NULL != (A));			\
      if (NULL == (A)) {			\
         Warning("Unexpected COW Null PTR in cow.c, line %d\n", __LINE__); \
	 return VMK_INVALID_HANDLE;		\
      }                                         \
   } while(0)

static int COWGetFreeHandleIndex(void);
static COW_HandleInfo *COWAllocateHandle(void);
static INLINE COW_HandleInfo *COWGetHandleInfo(COW_HandleID cowHandleID);
static int COWGetIndex(COW_HandleInfo *chi, FS_FileHandleID fileHandle);
static VMK_ReturnStatus COWGetFileHandles(COW_HandleID cowHandle, 
                                          FS_FileHandleID *handleList, int *validHandles);
static VMK_ReturnStatus COWInitCache(COWInfo *info);
static void COWFreeCache(COWInfo *info);
static VMK_ReturnStatus COWIncrementFreeSector(COWInfo *info, uint32 increment);
static VMK_ReturnStatus COWClose(COWInfo *info);
static VMK_ReturnStatus COWCheck(COWInfo *info, uint64 length);
static VMK_ReturnStatus COWInitializeIOInfo(COW_FSAsyncIOInfo *cowIOInfo, uint32 blocks, 
                                            COW_HandleID handleID, FS_FileHandleID fid, 
                                            SG_AddrType addrType, Bool allocToken);
static VMK_ReturnStatus COWInitializeMetaDataInfo(COW_MetaDataInfo *cowMetaDataInfo, 
                                                  uint32 blocks, FS_FileHandleID fid, 
                                                  COWInfo *info, Async_Token *token);
static void COWDestroyMetaDataInfo(COW_MetaDataInfo *cowMetaDataInfo);
static void COWDestroyIOInfo(COW_FSAsyncIOInfo *cowIOInfo, int validRedos);
static VMK_ReturnStatus COWCacheLookup(COWInfo *info, int offset, Bool read, Bool needToLockEntry,
                                       COWPair **pair);
static VMK_ReturnStatus COWOpenFile(FS_FileHandleID fileHandle, COWInfo **cowInfoOut);
static void COWTokenCallback(Async_Token *token);
static VMK_ReturnStatus COWReadGetLBNAndFID(COW_HandleID cowHandleID, uint32 sector, 
                                            FS_FileHandleID *fid, uint64 *actualBlockNumber);
static VMK_ReturnStatus COWWriteGetLBNAndMDB(COW_HandleID cowHandleID, uint32 sector,
                                             COW_MetaDataInfo *cowMetaDataInfo, 
                                             uint64 *actualBlockNumber);
VMK_ReturnStatus COW_AsyncFileIO(COW_HandleID cowHandleID, SG_Array *sgArr, 
                                 Async_Token *token, IO_Flags ioFlags);
static VMK_ReturnStatus COWAsyncFileRead(COW_HandleID cowHandleID, SG_Array *sgArr, 
                                         Async_Token *token);
static VMK_ReturnStatus COWAsyncFileWrite(COW_HandleID cowHandleID, SG_Array *sgArr, 
                                          Async_Token *token);
static void COWAsyncReadDone(Async_Token *token, void *data);
static void COWAsyncWriteDone(Async_Token *token, void *data);
static void COWAsyncMetaDataWriteDone(Async_Token *token, void *data);
static void COWMapLeafEntry(COWLeafEntry *addr, COWPair *pair);
static VMK_ReturnStatus COWReadEntry(FS_FileHandleID fd, uint64 offset, COWPair *pair);
static VMK_ReturnStatus COWWriteEntry(FS_FileHandleID fd, uint64 offset, COWPair *pair);
static VMK_ReturnStatus COWInsertMetaDataList(COW_MetaDataInfo *cowCmd, 
                                              COWPair *pair, uint32 sector, uint32 grainSec);
static void COWUpdateCache(COW_MetaDataInfo *metaDataInfo);
static VMK_ReturnStatus COWWriteMetaDataList(COW_MetaDataInfo *cowMetaDataInfo);
static INLINE void COWInitQueue(COW_MDQ* queue);
static INLINE void COWAppendtoQueue(COW_MDQ* queue, COW_MetaDataInfo* cmd);
static INLINE void COWCmdRemoveFromQueue(COW_MetaDataInfo* cmd);
static INLINE COW_MetaDataInfo* COWRemoveFromQueue(COW_MDQ* queue);
static INLINE COW_MetaDataInfo* COWPeekAtQueue(COW_MDQ* queue);
static INLINE Bool COWHasCmds(const COW_MDQ* queue);
static VMK_ReturnStatus COW_SpliceParent(COW_HandleID cowHandle, int level); 
static uint32 COWGranularity(COW_HandleInfo *chi);
static uint32 COWLength(COW_HandleInfo *chi);
static VMK_ReturnStatus COWWriteMetaDataInfo(COW_MetaDataInfo *cowCmd, 
                                             Async_Token *token);
static VMK_ReturnStatus COWPrepareIOInfo(COW_HandleID cowHandleID, SG_Array *sgArr, 
                        Async_Token *token, COW_FSAsyncIOInfo *cowIOInfo, 
                        COW_MetaDataInfo *cowMetaDataInfo, int *totalIOs);
static void COWCompleteCommand(COW_MetaDataInfo *metaDataInfo);


/*
 *-----------------------------------------------------------------------------
 *
 * COW_Init --
 *
 *      Initialize the cow module.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
void 
COW_Init(void)
{
   int i;

   memset(cowFileHandleTable, 0 , COW_NUM_FILE_HANDLES * sizeof(COW_HandleInfo));
   /* Is this Really required */
   for (i = 0; i < COW_NUM_FILE_HANDLES; i++) {
      cowFileHandleTable[i].handleID = i;
      cowFileHandleTable[i].inUse = FALSE;
   }

   SP_InitLock("cowHandle", &cowFileHandleLock, SP_RANK_LEAF);
}

/*
 *-----------------------------------------------------------------------------
 *
 * COWGetFreeHandleIndex --
 *
 *      Get a free cow handle.  The handle is not reserved until
 *      cowFileHandleTable[freeHandleIndex].inUse is set to TRUE.
 *
 * Results:
 *      The index in the cowFileHandleTable or NULL
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
static int 
COWGetFreeHandleIndex(void)
{
   int freeHandleIndex;

   ASSERT(SP_IsLocked(&cowFileHandleLock));
   for (freeHandleIndex = 0; freeHandleIndex < COW_NUM_FILE_HANDLES; freeHandleIndex++) {
      if (cowFileHandleTable[freeHandleIndex].inUse == FALSE) {
         return freeHandleIndex;
      }
   }
   return -1;
}

/*
 *----------------------------------------------------------------------
 *
 * COWAllocateHandle --
 *
 *      Attempts to allocates a new COW handle.
 *
 * Results:
 *      A new COW_HandleInfo struct ptr or NULL
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static COW_HandleInfo *
COWAllocateHandle(void)
{
   int freeHandleIndex;

   SP_Lock(&cowFileHandleLock);

   freeHandleIndex = COWGetFreeHandleIndex();
   if (freeHandleIndex == -1) {
      SP_Unlock(&cowFileHandleLock);
      return NULL;
   }
   COW_HandleInfo *cowHandleInfo = &cowFileHandleTable[freeHandleIndex];
   cowHandleInfo->handleID += COW_NUM_FILE_HANDLES;
   cowHandleInfo->inUse = TRUE;
   cowHandleInfo->validRedos = 0;

   SP_Unlock(&cowFileHandleLock);

   return cowHandleInfo; 
}


/*
 *----------------------------------------------------------------------
 *
 * COWGetHandleInfo  --
 *
 *      Returns a pointer to the COW_HandleInfo structure for a given
 *      cowHandleID. Performs a sanity check on the cowHandleID.
 *      
 * Results:
 *      Pointer to a COW_HandleInfo struct or NULL if the cowHandleID
 *      specified is invalid.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static COW_HandleInfo *
COWGetHandleInfo(COW_HandleID cowHandleID)
{
   COW_HandleInfo *chi;

   chi = &cowFileHandleTable[cowHandleID & COW_FILE_HANDLES_MASK];
   if (((chi)->handleID != (cowHandleID)) || !(chi)->inUse) {
      Log("Cow Handle %"FMT64"d is invalid\n", cowHandleID);
      chi = NULL;
   }
   return chi;
}


/*
 *----------------------------------------------------------------------
 *
 * COWGetIndex  --
 *
 *      Returns the index of fileHandle in chi's cowFSInfo array.
 *      (ie the position of fileHandle in the redo log hierarchy)
 *      
 * Results:
 *      Index of fileHandle or -1
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int 
COWGetIndex(COW_HandleInfo *chi, FS_FileHandleID fileHandle)
{
   int index;
   for(index = 0; index <= chi->validRedos; index++) {
      if(chi->cowFSInfo[index].fsFileHandleID == fileHandle)  
         return index;
   }
   NOT_REACHED();
   return -1;
}

/*
 *----------------------------------------------------------------------
 *
 * COWGetFileHandles  --
 *
 *      Returns the list of file handles associated with this cow
 *      handle.
 *
 *      
 * Results:
 *      An array of fileHandles an the length of this array.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
COWGetFileHandles(COW_HandleID cowHandle, FS_FileHandleID *handleList, int *validHandles)
{
   VMK_ReturnStatus status = VMK_OK; 
   int index;
   COW_HandleInfo *cowHandleInfo = COWGetHandleInfo(cowHandle); 

   ASSERT_VALID_COWHANDLE(cowHandleInfo);
   for (index = 0; index <= cowHandleInfo->validRedos; index++) {
      COW_FSInfo *cowFSInfo = &(cowHandleInfo->cowFSInfo[index]);
      handleList[index] = cowFSInfo->fsFileHandleID; 
      LOG(2, "%d) %"FMT64"d", index, handleList[index]);
   }
   *validHandles = cowHandleInfo->validRedos;
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * COWInitializeIOInfo  --
 *
 * Allocates scatter-gather and token for the data read/write.
 *
 * Results:
 *     A VMK_ReturnStatus value.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
COWInitializeIOInfo(COW_FSAsyncIOInfo *cowIOInfo, uint32 blocks, COW_HandleID handleID, 
                    FS_FileHandleID fid, SG_AddrType addrType, Bool allocToken)
{
   cowIOInfo->handleID = handleID;
   cowIOInfo->fileHandle = fid;
   cowIOInfo->length = 0;
   cowIOInfo->sg_Arr = (SG_Array *)Mem_Alloc(SG_ARRAY_SIZE(blocks));
   if (cowIOInfo->sg_Arr == NULL) {
      return VMK_NO_MEMORY;
   }
   cowIOInfo->sg_Arr->length = 0;
   cowIOInfo->sg_Arr->addrType = addrType;
   if (allocToken) {
      cowIOInfo->token = Async_AllocToken(ASYNC_CALLBACK);
      if (cowIOInfo->token == NULL) {
         Mem_Free(cowIOInfo->sg_Arr);
         return VMK_NO_MEMORY;
      }
   } else {
      cowIOInfo->token = NULL;
   }

   LOG(5, "incoming fid %"FMT64"d", fid);
   LOG(5, "fid %"FMT64"d", cowIOInfo->fileHandle);

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * COWInitializeMetaDataInfo  --
 * 
 * Allocates the scatter gather list and token for the meta data write.
 *
 * Results:
 *     A VMK_ReturnStatus value.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
COWInitializeMetaDataInfo(COW_MetaDataInfo *cowMetaDataInfo, uint32 blocks, 
                     FS_FileHandleID fid, COWInfo *info, Async_Token *token)
{
   cowMetaDataInfo->fileHandle = fid;
   cowMetaDataInfo->parentToken = token; /* Store the parent token */
      
   cowMetaDataInfo->metaDataHead = (COWMetaData *) Mem_Alloc(sizeof(COWMetaData));
   memset(cowMetaDataInfo->metaDataHead, 0 , sizeof(COWMetaData));

   cowMetaDataInfo->metaDataHead->metaPair = (COWMDPair *)Mem_Alloc(sizeof(COWMDPair) * 
                                                                    blocks);
   if (cowMetaDataInfo->metaDataHead->metaPair == NULL) {
      return VMK_NO_MEMORY;
   }
   memset(cowMetaDataInfo->metaDataHead->metaPair, 0 , sizeof(COWMDPair) * blocks);
   cowMetaDataInfo->info = info;
   cowMetaDataInfo->ioState = COW_IO_INITIALIZED;
   cowMetaDataInfo->totalBlocks = blocks;
   cowMetaDataInfo->numLeafEntries++;

   return VMK_OK;
}


/*
 * COWInitQueue --
 *
 *      Initializes a COW_MDQ so that it can be used by the other functions.
 *
 *      The COW queues operate in a tricky way.  COW_MDQ happens
 *      have its 'head' field at the same offset as COW_MetaDataInfo's 'next' field and 
 *      its 'tail' field at the same offset as SCSICmd's 'prev' field.  
 *      This, combined with the convention that an 'empty' queue is one 
 *      where queue->head == queue, underpins our queue operations.
 *
 * Results: 
 *      None.
 *
 * Side effects: 
 *      None.
 */

static INLINE void 
COWInitQueue(COW_MDQ* queue)
{
   queue->head = queue->tail = (COW_MetaDataInfo *) queue; 
}

/*
 *---------------------------------------------------------------------------
 *
 * COWAppendtoQueue --
 *
 * Appends the cmd to the queue.
 * Results:
 *      Void.
 * Side effects:
 *      None.
 *
 *---------------------------------------------------------------------------
 */

static INLINE void
COWAppendtoQueue(COW_MDQ* queue,    // IN
                 COW_MetaDataInfo* cmd)       // IN
{
   cmd->next = (COW_MetaDataInfo *) queue;
   queue->tail->next = cmd;
   cmd->prev = queue->tail;
   queue->tail = cmd;
}

/*
 *---------------------------------------------------------------------------
 * 
 * COWCmdRemoveFromQueue --
 *
 * Results:
 *      None.
 * Side effects:
 *      None.
 *
 *---------------------------------------------------------------------------
 */
 
static INLINE void
COWCmdRemoveFromQueue(COW_MetaDataInfo* cmd)   // IN/OUT
{
   cmd->next->prev = cmd->prev;
   cmd->prev->next = cmd->next;
}

/*
 *---------------------------------------------------------------------------
 *
 * COWRemoveFromQueue --
 *
 *      Takes the head element off a COWMD queue.  
 * Results:
 *      COW_MetaDataInfo * from the front of the queue, or NULL if the queue was
 *      empty.
 * Side effects:
 *      None.
 *
 *---------------------------------------------------------------------------
 */

static INLINE COW_MetaDataInfo*
COWRemoveFromQueue(COW_MDQ* queue)     // IN
{
   COW_MetaDataInfo* cmd = queue->head;
   if (cmd != (COW_MetaDataInfo*) queue) {
      COWCmdRemoveFromQueue(cmd);
      return cmd;
   } else {
      return NULL;
   }
}


/*
 *---------------------------------------------------------------------------
 *
 * COWPeekAtQueue --
 *      Returns a pointer to the first element in the queue without removing
 *      it.
 * Results:
 *      COW_MetaDataInfo *, or NULL if the queue is empty.
 * Side effects:
 *      None.
 *
 *---------------------------------------------------------------------------
 */

static INLINE COW_MetaDataInfo*
COWPeekAtQueue(COW_MDQ* queue) // IN
{
   COW_MetaDataInfo* cmd = queue->head;
   return (cmd != (COW_MetaDataInfo*) queue) ? cmd : NULL;
}


/*
 *---------------------------------------------------------------------------
 *
 * COWHasCmds --
 *
 *      Predicate to check whether a COWMD queue is empty or not.
 * Results:
 *      TRUE if the queue is non-empty, else FALSE.
 * Side effects:
 *      None.
 *
 *---------------------------------------------------------------------------
 */

static INLINE Bool
COWHasCmds(const COW_MDQ* queue)       // IN
{
   return queue->head != (COW_MetaDataInfo*) queue;
}

/*
 *----------------------------------------------------------------------
 *
 * COW_OpenHierarchy --
 *
 *      Create a COW disk hierarchy out of the array of file handles
 *      passed in.
 *
 * Results:
 *      VMK_OK and a COW_HandleID in hidOut or a VMK_ReturnStatus value
 *
 * Side effects:
 *      Parent of this disk is opened.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
COW_OpenHierarchy(FS_FileHandleID *fids, int numFds, COW_HandleID *hidOut)
{
   VMK_ReturnStatus status = VMK_OK;
   COW_HandleInfo *chi;
   int i;

   LOG(1, "Starting: %d", numFds);
   if (numFds < 1) {
      return VMK_BAD_PARAM;
   }

   if (numFds > COW_MAX_REDO_LOG) {
      Warning("Too many redo logs: %d > %d", numFds, COW_MAX_REDO_LOG);
      return VMK_LIMIT_EXCEEDED;
   }

   chi = COWAllocateHandle();
   if (!chi) {
      Warning("COWAllocateHandle failed");
      return VMK_LIMIT_EXCEEDED;
   }

   for (i = 0; i < numFds; i++) {
      COWInfo *info;
      status = COWOpenFile(fids[i], &info);
      LOG(1, "Opened level %d: %#x", i, status);
      if (i == 0 && status == VMK_NOT_SUPPORTED) {
         // base disk
         status = VMK_OK;
         info = NULL;
      } 

      if (status != VMK_OK) {
         goto cleanup;
      }
      chi->cowFSInfo[i].fsFileHandleID = fids[i];
      chi->cowFSInfo[i].cowInfo = info;
   }
   chi->validRedos = numFds - 1;
   Semaphore_RWInit("cowLock", &chi->rwlock);

   *hidOut = chi->handleID;
cleanup:
   LOG(0, "Finished: status = %#x", status);
   if (status != VMK_OK) {
      chi->inUse = FALSE;
   }
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * COWOpenFile --
 *
 *      Open an existing file in the cow hierarchy
 *
 * Results:
 *      Normally returns a newly allocated COWInfo structure. Otherwise
 *      returns a VMK error status
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
COWOpenFile(FS_FileHandleID fileHandle, COWInfo **cowInfoOut)
{
   COWInfo *info = NULL;
   COWDisk_Header *hdr = NULL;
   uint64 offset;
   int size;
   int leafCoverage;
   uint32 bytes;
   FS_FileAttributes attrs;
   VMK_ReturnStatus status;

   LOG(1, "Opening %"FMT64"d", fileHandle);

   /*
    *  Allocate buffer for header
    */
   hdr = Mem_Alloc(sizeof(COWDisk_Header));
   if (!hdr) {
      status = VMK_NO_MEMORY;
      goto error;
   }

   status = FSS_BufferIO(fileHandle, 0, (uint64) (unsigned long)hdr, 
                         sizeof(COWDisk_Header), FS_READ_OP, SG_VIRT_ADDR, &bytes);
   if (status != VMK_OK) {
      goto error;
   }

   if (hdr->magicNumber != COWDISK_MAGIC) {
      status = VMK_NOT_SUPPORTED;
      goto error;
   }

   if (hdr->version != 1) {
      /* Only version 1 supported right now. */
      status = VMK_NOT_SUPPORTED;
      goto error;
   }

   info = Mem_Alloc(sizeof(COWInfo));
   if (!info) {
      status = VMK_NO_MEMORY;
      goto error;
   }
   memset(info, 0, sizeof(COWInfo));
   status = COWInitCache(info);
   if (status != VMK_OK) {
      goto error;
   }

   info->rootEntries = NULL;
   info->flags = hdr->flags;
   info->numSectors = hdr->numSectors;
   info->granularity = hdr->granularity;
   info->rootOffset = hdr->rootOffset;

   info->fd = fileHandle;
   info->opCount = 0;

   leafCoverage = COW_NUM_LEAF_ENTRIES * info->granularity;
   info->numRootEntries = CEIL(hdr->numSectors, leafCoverage);
   info->numRootEntries = (CEIL(info->numRootEntries * sizeof(COWRootEntry),
                                DISK_SECTOR_SIZE) * DISK_SECTOR_SIZE) / sizeof(COWRootEntry);

   if (info->numRootEntries != hdr->numRootEntries) {
      Warning("Number root entries mismatch (%d != %d).",
              info->numRootEntries, hdr->numRootEntries);
      status = VMK_METADATA_READ_ERROR;
      goto error;
   }

   info->rootEntries = Mem_Alloc(info->numRootEntries * sizeof(COWRootEntry));
   if (!info->rootEntries) {
      status = VMK_NO_MEMORY;
      goto error;
   }

   offset = SECTORS_TO_BYTES(info->rootOffset);
   size = info->numRootEntries * sizeof(COWRootEntry);

   /*
    *  Read root entries.
    */
   status = FSS_BufferIO(info->fd, offset, 
                         (uint64) (unsigned long)info->rootEntries, size, 
                         FS_READ_OP, SG_VIRT_ADDR, &bytes);
   if (status != VMK_OK) {
      goto error;
   }

   /*
    * Allocate tempSectorBuffer before potentially doing a COWCheck.
    */
   info->tempSectorBuffer = Mem_Alloc(MAX(info->granularity * DISK_SECTOR_SIZE,
                                          sizeof(COWDisk_Header)));
   if (!info->tempSectorBuffer) {
      status = VMK_NO_MEMORY;
      goto error;
   }

   COWInitQueue(&info->ready);
   COWInitQueue(&info->active);
   SP_InitLock("cowQueueLock", &info->queueLock, SP_RANK_LEAF);

   status = FSClient_GetFileAttributes(fileHandle, &attrs);
   if (status != VMK_OK) {
      goto error;
   }
   if (hdr->savedGeneration == attrs.generation &&
       SECTORS_TO_BYTES(hdr->freeSector) <= attrs.length) {
      /* COW file was closed cleanly */
      info->freeSector = hdr->freeSector;
   }
   else {
      //XXX remove the hdr->savedGeneration != 0 check.  It is bogus
      // and only necessary because userland creates cow disks funny.
      if (hdr->savedGeneration == 0) {
         Warning("savedGeneration = 0, fs gen = %#x, assuming newly created disk (bug 49269)",
                 attrs.generation);
         info->freeSector = hdr->freeSector;
      } else {
         Warning("COW file was not closed cleanly, doing checks");
         status = COWCheck(info, attrs.length);
         if (status != VMK_OK) {
            goto error;
         }
      }
      hdr->savedGeneration = attrs.generation;
   }
   info->savedGeneration = hdr->savedGeneration;
   info->allocSectors = attrs.length / DISK_SECTOR_SIZE;
   info->freeSectorChanged = FALSE;

   Mem_Free(hdr);

   *cowInfoOut = info;
   return VMK_OK;

error:
   if (info) {
      if (info->rootEntries) {
         Mem_Free(info->rootEntries);
      }
      if (info->tempSectorBuffer) {
         Mem_Free(info->tempSectorBuffer);
      }
      if (hdr != NULL) {
         Mem_Free(hdr);
      }
      COWFreeCache(info);
      Mem_Free(info);
   }
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * COWClose --
 *
 *      Cleanup the cow data structures & write out the new generation
 *      number for the disk.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Frees all memory associated with node.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus 
COWClose(COWInfo *info)  // IN/OUT: cow disk node
{
   COWDisk_Header *hdr;
   uint32 bytes;
   VMK_ReturnStatus status;
   FS_FileAttributes attrs;


   LOG(1, "Closing fd = %"FMT64"d", info->fd);
   
   if(COWHasCmds(&info->active) || COWHasCmds(&info->ready)) {
      Warning("Trying to close before commands are drained");
      return VMK_BUSY;
   }
   FSClient_GetFileAttributes(info->fd, &attrs);

   /* If the generation of info->fd has changed, we have done a write
    * and we need to update savedGeneration in the COW hdr to mark the
    * COW file as cleanly closed.  We also need to save freeSector at
    * the same time, since it is not written to disk when it is
    * changed.  We also need to check the extra freeSectorChanged
    * flag, since the generation is not changed during a partial
    * commit, but the freeSector needs to be written out. 
    */
   if (info->savedGeneration != attrs.generation ||
       info->freeSectorChanged) {
      hdr = (COWDisk_Header *)info->tempSectorBuffer;
      status = FSS_BufferIO(info->fd, 0, (uint64)(unsigned int)hdr, sizeof(COWDisk_Header), 
                            FS_READ_OP, SG_VIRT_ADDR, &bytes);
      if (status != VMK_OK) {
         goto failure;
      }

      hdr->savedGeneration = attrs.generation;
      hdr->freeSector = info->freeSector;

      status = FSS_BufferIO(info->fd, 0, (uint64)(unsigned int)hdr, sizeof(COWDisk_Header), 
                            FS_WRITE_OP, SG_VIRT_ADDR, &bytes);
      if (status != VMK_OK) {
         goto failure;
      }
   }

   if (info->rootEntries) {
      Mem_Free(info->rootEntries);
   }

   if (info->tempSectorBuffer) {
      Mem_Free(info->tempSectorBuffer);
   }

   COWFreeCache(info);

   SP_CleanupLock(&info->queueLock);

   memset(info,0,sizeof(*info));
   Mem_Free(info);
   return VMK_OK;

failure:

   Warning("Failed to write out COW disk header: %#x", status);
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * COW_CloseHierarchy --
 *
 *      Close a cow disk hierarchy.
 *
 * Results:
 *      A VMK_ReturnStatus value.
 *
 * Side effects:
 *      Frees all memory associated with node.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
COW_CloseHierarchy(COW_HandleID cowHandleID)
{
   COW_HandleInfo *chi = COWGetHandleInfo(cowHandleID);
   VMK_ReturnStatus status, retval = VMK_OK;
   int i;

   if (NULL == chi) {
      return VMK_INVALID_HANDLE;
   }

   LOG(1, "Starting");
   for (i = 0; i <= chi->validRedos; i++) {
      COWInfo *info = chi->cowFSInfo[i].cowInfo;
      FS_FileHandleID fileHandle = chi->cowFSInfo[i].fsFileHandleID;

      if (!info) {
         //base disk
         LOG(1, "%d) Skipping base disk: %"FMT64"d", i, fileHandle);
         continue;
      }

      /*
       * The underlying FS implementation is going to remove any in-memory
       * data structures for this file. So close any cow links this
       * file might have.
       */
      status = COWClose(info);
      if (status != VMK_OK) {
         retval = status;
         Warning("%d) couldn't close COW file %"FMT64"d:%#x", i, fileHandle, status);
      }
   }
   Semaphore_RWCleanup(&chi->rwlock);
   chi->inUse = FALSE;
   return retval;
}

/*
 *----------------------------------------------------------------------
 *
 * COWLength --
 *
 *      Return the length of Base Disk in the COW hierarchy.
 *
 * Results:
 *      Length of COW disk.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static uint32 
COWLength(COW_HandleInfo *chi)         // IN: the COW disk node
{
   ASSERT(NULL != chi);
   return chi->cowFSInfo[1].cowInfo->numSectors;
}

/*
 *----------------------------------------------------------------------
 *
 * COWGranularity --
 *
 *      Return the leaf granularity of the COW disk in sectors.
 *
 * Results:
 *      Granularity of COW disk.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static uint32 
COWGranularity(COW_HandleInfo *chi)         // IN: the COW disk node
{
   ASSERT(NULL != chi);
   return chi->cowFSInfo[chi->validRedos].cowInfo->granularity;
}

/*
 *-----------------------------------------------------------------------------
 *
 * COWTokenCallback --
 *    Indicate that the entire COW_AsyncFileIO operation is done with the
 *    SCSI results stored in the specified token.
 *
 * Results:
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
static void
COWTokenCallback(Async_Token *token)
{
   if (token->flags & ASYNC_CALLBACK) {
      ASSERT(token->callback != NULL);
      token->callback(token);
   }
   Async_Wakeup(token);
}

/*
 *-----------------------------------------------------------------------------
 *
 * COW_AsyncFileIO --
 *    Do scatter-gather read or write to a COW file.  If token is non-null,
 *    do asynchronous IO.
 *
 * Results:
 *    Amount of data transferred is returned in bytesTransferred.
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus 
COW_AsyncFileIO(COW_HandleID cowHandleID, SG_Array *sgArr, 
                Async_Token *token, IO_Flags ioFlags)
{
   VMK_ReturnStatus status;
   COW_HandleInfo *chi = COWGetHandleInfo(cowHandleID); 
   
   if (NULL == chi) {
      return VMK_INVALID_HANDLE;
   }

   Semaphore_BeginRead(&chi->rwlock); 

   if (ioFlags & FS_READ_OP) {
      status = COWAsyncFileRead(cowHandleID, sgArr, token);
   } else {
      status = COWAsyncFileWrite(cowHandleID, sgArr, token);
   }

   if(status != VMK_OK) {
      Semaphore_EndRead(&chi->rwlock);
   }
   
   return status;
}

/*
 *-----------------------------------------------------------------------------
 *
 * COWAsyncFileRead --
 *    Do scatter-gather read to a COW file.  If token is non-null,
 *    do asynchronous IO.
 *
 * Results:
 *    Amount of data transferred is returned in bytesTransferred.
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus 
COWAsyncFileRead(COW_HandleID cowHandleID, SG_Array *sgArr, Async_Token *token)
{
   SG_Array *sg = NULL;   
   VMK_ReturnStatus status = VMK_OK;   
   COW_HandleInfo *chi;
   COW_FSAsyncIOInfo *cowIOInfo = NULL;
   int index = 0;
   Async_Token *childToken;
   COW_SplitChildInfo *childInfo = NULL;
   COW_AsyncCounter *cac = NULL;
   uint32 cacOffset;
   int totalSGs = 0;
  
   chi = COWGetHandleInfo(cowHandleID); 
   ASSERT_VALID_COWHANDLE(chi);
   LOG(2, "Starting: handle = %"FMT64"d (%d) ", cowHandleID, chi->validRedos);

   cowIOInfo = Mem_Alloc((chi->validRedos + 1) * sizeof(COW_FSAsyncIOInfo));  
   if (!cowIOInfo) {
      return VMK_NO_MEMORY;
   }
   memset(cowIOInfo, 0,  sizeof(COW_FSAsyncIOInfo) * (chi->validRedos + 1));

   status = COWPrepareIOInfo(cowHandleID, sgArr, token, cowIOInfo, NULL, &totalSGs);

   if (status != VMK_OK) {
      LOG(4, "Failed! status = %#x", status);
   } else {
      if (token != NULL && (sgArr->length == 0 && totalSGs == 0)) {
         ((SCSI_Result *)token->result)->status = SCSI_MAKE_STATUS(SCSI_HOST_OK, SDSTAT_GOOD);
         COWTokenCallback(token);
         COWDestroyIOInfo(cowIOInfo, chi->validRedos);
         Mem_Free(cowIOInfo); 
      } else if (token != NULL) {
         /*
          * Store the necessary data in the parent token
          */
         cacOffset = token->callerPrivateUsed;
         ASSERT_NOT_IMPLEMENTED(cacOffset + sizeof(COW_AsyncCounter) <= ASYNC_MAX_PRIVATE);
         cac = (COW_AsyncCounter *)&token->callerPrivate[cacOffset];
         token->callerPrivateUsed += sizeof(COW_AsyncCounter);

         cac->magic = COW_ASYNC_COUNTER_MAGIC;
         cac->handled = 0;
         cac->needed = totalSGs + 1;

         for (index = 0; index <= chi->validRedos; index++) {
            sg = cowIOInfo[index].sg_Arr;
            if (!sg) {
               continue;
            }

            LOG(5, "I/O %d) fid = %"FMT64"d len = %d" , 
                index, cowIOInfo[index].fileHandle, sg->length);
            
            // Use Push and Pop interfaces
            //
            childToken = cowIOInfo[index].token;
            childInfo =(COW_SplitChildInfo *)Async_PushCallbackFrame(childToken, 
                                                                     COWAsyncReadDone, 
                                                                     sizeof(COW_SplitChildInfo));
            childInfo->fileHandle = cowIOInfo[index].fileHandle;
            childInfo->parentToken = token;
            childInfo->sgLen = sg->length;
            childInfo->cowIOInfo = cowIOInfo;
            childInfo->dataIndex = index;
            childInfo->validRedos = chi->validRedos;

            ASSERT(childInfo != NULL);
            ASSERT(childInfo->dataIndex <= COW_MAX_REDO_LOG + 1);

            childToken->clientData = (void *)cacOffset; 
            childToken->resID = token->resID; 
            childToken->cmd = token->cmd; 
            childToken->originSN = token->originSN; 
            childToken->originHandleID = token->originHandleID; 

            Async_RefToken(token);
            // Revisit
	    status = FSS_AsyncFileIO(cowIOInfo[index].fileHandle, sg, childToken, FS_READ_OP);
            if (status != VMK_OK) {
               Warning("Error 0x%x", status);
               ASSERT(cac->needed != cac->handled);
               ((SCSI_Result *)token->result)->status =
                  SCSI_MAKE_STATUS(SCSI_HOST_ERROR, SDSTAT_GOOD);

               // If the first IO fails just break out.
               SP_Lock(&childInfo->parentToken->lock);
               if (index == 0) {
                  SP_Unlock(&childInfo->parentToken->lock);
                  COWTokenCallback(token); 
                  Async_ReleaseToken(token);
                  break;    
               // If the earlier IOs have completed just call the callback. 
               } else if (cac->handled == (index + 1)) {
                  cac->needed = cac->handled + 1;
                  SP_Unlock(&childInfo->parentToken->lock);
                  Async_PopCallbackFrame(childToken);
               } else {
                  cac->needed = index + 1; 
                  SP_Unlock(&childInfo->parentToken->lock);
               }
               Async_ReleaseToken(token);
               status = VMK_OK;
               break;
            }
         }
         SP_Lock(&token->lock);
         cac->needed--; 
         if (cac->needed == cac->handled) {
             SP_Unlock(&token->lock);
             COWTokenCallback(token); 
             Semaphore_EndRead(&chi->rwlock);

             //Destroy the cowIOInfo and free the memory associated with that
             COWDestroyIOInfo(cowIOInfo, chi->validRedos);
             Mem_Free(cowIOInfo);
         } else {
             SP_Unlock(&token->lock);
         }
      } else {
         ASSERT(sgArr->addrType == SG_VIRT_ADDR);
         for (index = chi->validRedos; index >= 0; index--) {
            sg = cowIOInfo[index].sg_Arr;
            if (sg == NULL)
               continue;
	    status = FSS_SGFileIO(cowIOInfo[index].fileHandle, sg, 
                                  FS_READ_OP, &cowIOInfo[index].length);
            if (status != VMK_OK)
               break;
         }
         if (childInfo->cowIOInfo) {
            COWDestroyIOInfo(childInfo->cowIOInfo, childInfo->validRedos);
            Mem_Free(cowIOInfo); 
         }
      }
   }

   if (status != VMK_OK) {
      Warning("File read error 0x%x", status);
      if (cowIOInfo) {
         COWDestroyIOInfo(cowIOInfo, chi->validRedos);
         Mem_Free(cowIOInfo); 
      }
      status = VMK_READ_ERROR;
   }
   return status;      
}

/*
 *-----------------------------------------------------------------------------
 *
 * COWAsyncFileWrite --
 *    Do scatter-gather write to a COW file.  If token is non-null,
 *    do asynchronous IO.
 *
 * Results:
 *    Amount of data transferred is returned in bytesTransferred.
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus 
COWAsyncFileWrite(COW_HandleID cowHandleID, SG_Array *sgArr, Async_Token *token)
{
   uint32 totalSGs = 0;   
   SG_Array *sg = NULL;   
   VMK_ReturnStatus status = VMK_OK;   
   COW_FSAsyncIOInfo *cowIOInfo = NULL;
   COW_MetaDataInfo   *cowMetaDataInfo = NULL; 
   COW_MetaDataFrame  *tokenMetaDataFrame;
   Async_Token *childToken;
  
   cowIOInfo = Mem_Alloc(sizeof(COW_FSAsyncIOInfo));  
   if (cowIOInfo == NULL)
      return VMK_NO_MEMORY;
   memset(cowIOInfo, 0, sizeof(COW_FSAsyncIOInfo));

   cowMetaDataInfo = Mem_Alloc(sizeof(COW_MetaDataInfo));  
   if (cowMetaDataInfo == NULL) {
      Mem_Free(cowIOInfo);
      return VMK_NO_MEMORY;
   } else {
      memset(cowMetaDataInfo, 0, sizeof(COW_MetaDataInfo));
   }

   status = COWPrepareIOInfo(cowHandleID, sgArr, token, cowIOInfo, cowMetaDataInfo, 
                             &totalSGs);
   
   if(status != VMK_OK) {
      LOG(4, "Failed! status = %#x", status);
   }
   else {
      // Revisit wrt sg->length

      sg = cowIOInfo->sg_Arr;
      if (token != NULL && (sg->length == 0 && totalSGs == 0)) {
	 ((SCSI_Result *)token->result)->status =
	    SCSI_MAKE_STATUS(SCSI_HOST_OK, SDSTAT_GOOD);
	 COWTokenCallback(token);
         
         /*
          * Destroy allocated ioInfo and meta data info 
          */
         if (cowIOInfo) {
            COWDestroyIOInfo(cowIOInfo, 0);
            Mem_Free(cowIOInfo); 
         }
         if (cowMetaDataInfo) {
            COWDestroyMetaDataInfo(cowMetaDataInfo);
            Mem_Free(cowMetaDataInfo); 
         }
      } else if (token != NULL) {
         // Use Push and Pop interfaces
         childToken = cowIOInfo->token;
         /*
          * Store the necessary information regarding MetaData in the parent token
          */
         cowMetaDataInfo->cowIOInfo = cowIOInfo;
         tokenMetaDataFrame = (COW_MetaDataFrame *)Async_PushCallbackFrame(childToken, 
                                                                           COWAsyncWriteDone, 
                                                                           sizeof(COW_MetaDataFrame));
         tokenMetaDataFrame->magic = COW_ASYNC_COUNTER_MAGIC;
         tokenMetaDataFrame->cowMetaDataInfo = cowMetaDataInfo; 
         
         ASSERT(tokenMetaDataFrame != NULL);
         ASSERT(tokenMetaDataFrame->cowMetaDataInfo != NULL);

         childToken->resID = token->resID; 
         childToken->cmd = token->cmd; 
         childToken->originSN = token->originSN; 
         childToken->originHandleID = token->originHandleID; 

         // Reference count on the original
         Async_RefToken(cowMetaDataInfo->parentToken);
         cowMetaDataInfo->ioState = COW_DATAWRITE_PROG;
         status = FSS_AsyncFileIO(cowIOInfo->fileHandle, sg, cowIOInfo->token, FS_WRITE_OP);
         if (status != VMK_OK) {
            ((SCSI_Result *)token->result)->status =
               SCSI_MAKE_STATUS(SCSI_HOST_ERROR, SDSTAT_GOOD);
            Async_FreeCallbackFrame(childToken);
            Async_ReleaseToken(cowMetaDataInfo->parentToken);
            COWTokenCallback(cowMetaDataInfo->parentToken); 
         }
      } else {
         /* In this case, All the MetaData updates have to be Sync */
         ASSERT(sgArr->addrType == SG_VIRT_ADDR);

         status = FSS_SGFileIO(cowIOInfo->fileHandle, sgArr, FS_WRITE_OP, &cowIOInfo->length);
         if (status != VMK_OK) {
            goto Error; 
         }

         /* Update the cowCache */
         COWUpdateCache(cowMetaDataInfo);

         /* Write out the MetaData */
         status = COWWriteMetaDataList(cowMetaDataInfo);
         if (status != VMK_OK)
            goto Error; 

         if (cowIOInfo) {
            COWDestroyIOInfo(cowIOInfo, 0);
            Mem_Free(cowIOInfo); 
         }
         if (cowMetaDataInfo) {
            COWDestroyMetaDataInfo(cowMetaDataInfo);
            Mem_Free(cowMetaDataInfo); 
         }
      }
   }

Error :
   if (status != VMK_OK) {
      Warning("SCSI write error 0x%x", status);
      if (cowIOInfo) {
         COWDestroyIOInfo(cowIOInfo, 0);
         Mem_Free(cowIOInfo); 
      }
      if (cowMetaDataInfo) {
         COWDestroyMetaDataInfo(cowMetaDataInfo);
         Mem_Free(cowMetaDataInfo); 
      }
      status = VMK_WRITE_ERROR;
   }
   return status;
}

static VMK_ReturnStatus
COWPrepareIOInfo(COW_HandleID cowHandleID, SG_Array *sgArr, Async_Token *token, 
                 COW_FSAsyncIOInfo *cowIOInfo, COW_MetaDataInfo *cowMetaDataInfo,
                 int *totalIOs) 
{
   int i, index = 0;
   uint32 totalSGs = 0, totalBlocks, totalLength;
   SG_Array *sg = NULL;   
   VMK_ReturnStatus status = VMK_OK;   
   uint64 fileLength;
   uint32 grainSize;
   uint32 granularity;
   FS_FileHandleID fid;
   COW_HandleInfo *chi = NULL;
   COWInfo *info = NULL;
  
   /*
    * Count the total number of COW leaf blocks that will be accessed,
    * since that will determine the upper bound on the number of
    * actual scatter/gather ops.
    */
   totalBlocks = 0;
   totalLength = 0;

   chi = COWGetHandleInfo(cowHandleID); 
   ASSERT_VALID_COWHANDLE(chi);
   fileLength = SECTORS_TO_BYTES(COWLength(chi));
   granularity = COWGranularity(chi);
   grainSize = granularity * DISK_SECTOR_SIZE;

   /*
    * Calculate the grainSize
    */

   for (i = 0; i < sgArr->length; i++) {
      uint64 offset = sgArr->sg[i].offset;
      uint32 length = sgArr->sg[i].length;

      /* request exceeds file length */
      if (offset + length > fileLength) {
	 return VMK_LIMIT_EXCEEDED;
      }

      totalBlocks += (offset + length - 1) / grainSize -
	 (offset / grainSize) + 1;
      totalLength += length;
   }

   for (i = 0; i < sgArr->length; i++) {
      uint32 blockNumber;
      uint32 blockOffset;
      uint32 bytesLeft;
      uint64 offset = sgArr->sg[i].offset;
      uint32 length = sgArr->sg[i].length;
      uint64 data = sgArr->sg[i].addr;

      blockNumber = offset / grainSize;
      blockOffset = offset & (grainSize - 1);
      bytesLeft = length;

      LOG(4, "%d) bytesLeft = %d, offset = %"FMT64"d, bn = %d, bo = %d, gn = %d", 
          i, bytesLeft, offset, blockNumber, blockOffset, granularity);

      while (bytesLeft > 0) {
         uint64 actualBlockNumber;
         uint32 toXfer;

         toXfer = grainSize - blockOffset;
         if (toXfer > bytesLeft) {
            toXfer = bytesLeft;
         }

	 /*
	  * Get the actual sector on the SCSI disk where the file block
	  * is located, if the sector is already allocated in the COW disk.
          * This can sleep.
	  */
         if (cowMetaDataInfo == NULL) {
	    status = COWReadGetLBNAndFID(cowHandleID, blockNumber * granularity,
                                         &fid, &actualBlockNumber);
         } else {
            fid = chi->cowFSInfo[chi->validRedos].fsFileHandleID; 
            info = chi->cowFSInfo[chi->validRedos].cowInfo; 
            if (cowMetaDataInfo->info == NULL) {
               status = COWInitializeMetaDataInfo(cowMetaDataInfo, totalBlocks, 
                                                  fid, info, token);
               if (status != VMK_OK) { 
                  return status;
               }
            }
	    status = COWWriteGetLBNAndMDB(cowHandleID, blockNumber * granularity,
                                          cowMetaDataInfo, &actualBlockNumber);
            LOG(4, "fid=%"FMT64"d, sector = %"FMT64"d", fid, actualBlockNumber);
         }

	 if (status != VMK_OK) {
	    return status;
	 }

	 if (actualBlockNumber == COW_NULL_SECTOR_NO) {
	    if (!Util_Memset(sgArr->addrType, data + (uint64)(length - bytesLeft),
			   0, toXfer)) {
               status = VMK_FAILURE;
	       return status;
            }
	 } else {
            if (cowMetaDataInfo == NULL) { 
               index = COWGetIndex(chi, fid);
            } else {
               index = 0;
            }

            if (cowIOInfo[index].sg_Arr == NULL) {
               status = COWInitializeIOInfo(&cowIOInfo[index], totalBlocks, cowHandleID, fid, 
                                            sgArr->addrType, (token ? TRUE : FALSE));
               if (status != VMK_OK) {
                  return status;
               }
               totalSGs++;
            }
            sg = cowIOInfo[index].sg_Arr;

            sg->sg[sg->length].offset = SECTORS_TO_BYTES(actualBlockNumber) + (uint64)blockOffset;
            sg->sg[sg->length].addr = data + (uint64)(length - bytesLeft);
            sg->sg[sg->length].length = toXfer;
            cowIOInfo[index].length += toXfer;

            if (sg->length > 0) {
               if (sg->sg[sg->length].offset != 
                   sg->sg[sg->length - 1].offset + sg->sg[sg->length - 1].length) {
                  /* offset should be disk-block aligned, if there is
                   * a discontinuity */
                   if ((sg->sg[sg->length].offset & (DISK_SECTOR_SIZE - 1)) != 0) {
                      status = VMK_BAD_PARAM;
                      return status;
                   }
               }
               else if ((sg->sg[sg->length].addr == sg->sg[sg->length-1].addr +
                         sg->sg[sg->length-1].length)) {
                  /* Merge this scatter-gather entry with the
                   * preceding one if COW grains are next to each
                   * other in COW file.  Important so that the # of
                   * scatter-gather entries don't get too large. */
                  sg->length--;
                  sg->sg[sg->length].length += sg->sg[sg->length+1].length;
               }
            }
            sg->length++;
         }
         bytesLeft -= toXfer;
         blockNumber++;
         blockOffset = 0;
      }
   }
   *totalIOs = totalSGs;
   return status;
}

/*
 *-----------------------------------------------------------------------------
 *
 * COWWriteMetaDataList --
 *
 * Synchronously update write out the meta Data entries to the disk.

 * Results:
 *
 * Side effects:
 *             None
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
COWWriteMetaDataList(COW_MetaDataInfo *cowMetaDataInfo)
{
   COWMetaData *ptr = cowMetaDataInfo->metaDataHead;
   VMK_ReturnStatus status = VMK_OK;

   while (ptr != NULL) {
      status = COWWriteEntry(cowMetaDataInfo->info->fd, 
                             SECTORS_TO_BYTES(ptr->pair->sectorOffset), ptr->pair);
      if (status != VMK_OK)
         return status;
      ptr = ptr->next;
   }
   return status; 
}

/*
 *-----------------------------------------------------------------------------
 *
 * COWDestroyIOInfo --
 *
 * Destroy the cowIOInfo allocated for the Data I/O
 *
 * Results:
 *
 * Side effects:
 *             None
 *
 *-----------------------------------------------------------------------------
 */
static void 
COWDestroyIOInfo(COW_FSAsyncIOInfo *cowIOInfo, int validRedos)
{
   int index;
   for (index = validRedos; index >= 0; index--) {
      if (cowIOInfo[index].sg_Arr) {
         Mem_Free(cowIOInfo[index].sg_Arr);
         Async_ReleaseToken(cowIOInfo[index].token);
      }
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * COWDestroyMetaDataInfo --
 *
 * Destroy the cowMetaDataInfo allocated for the Data I/O
 *
 * Results:
 *
 * Side effects:
 *             None
 *
 *-----------------------------------------------------------------------------
 */
static void 
COWDestroyMetaDataInfo(COW_MetaDataInfo *cowMetaDataInfo)
{
   COWMetaData *tmpptr, *ptr = cowMetaDataInfo->metaDataHead;

   while (ptr != NULL) {
      tmpptr = ptr;
      /* Takes care of Cache Hits */
      if(ptr->pair) {
         SP_Lock(&ptr->pair->leafEntrySpin);
         ptr->pair->numWrites--;
         if(ptr->pair->numWrites == 0) {
            CpuSched_Wakeup((uint32)ptr->pair);
         }
         SP_Unlock(&ptr->pair->leafEntrySpin);
      }
      Mem_Free(ptr->metaPair);
      ptr = ptr->next;
      Mem_Free(tmpptr);
   }
   
   if (cowMetaDataInfo->sg_Arr) {
      Mem_Free(cowMetaDataInfo->sg_Arr);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * COWAsyncReadDone --
 *
 * Async Read completion routine Data Read.
 *
 * Results:
 *
 * Side effects:
 *             None
 *
 *-----------------------------------------------------------------------------
 */
static void
COWAsyncReadDone(Async_Token *token, void *data)
{  
   COW_SplitChildInfo *childInfo = (COW_SplitChildInfo *)data;
   SCSI_Result *result = (SCSI_Result *)token->result;
   SCSI_Result *parentResult = (SCSI_Result *)childInfo->parentToken->result;
   COW_HandleInfo *chi;

   ASSERT(childInfo->dataIndex <= COW_MAX_REDO_LOG + 1);
   ASSERT(childInfo->validRedos <= COW_MAX_REDO_LOG + 1);

   chi = COWGetHandleInfo(childInfo->cowIOInfo[childInfo->dataIndex].handleID); 

   ASSERT(NULL != chi);
   SP_Lock(&childInfo->parentToken->lock);

   int32 tokenOffset = (uint32) token->clientData;
   if (tokenOffset > 0) {
      COW_AsyncCounter *cac = NULL;

      ASSERT(tokenOffset + sizeof(COW_AsyncCounter) <= ASYNC_MAX_PRIVATE);
      cac = (COW_AsyncCounter *)&childInfo->parentToken->callerPrivate[tokenOffset]; 
      ASSERT(cac->magic == COW_ASYNC_COUNTER_MAGIC);

      /*
       * Save the SCSI_Result data (status & sense buffer) if this is the
       * first command back, and also if this is the first command with a
       * SCSI error.
       */
      if (cac->handled == 0 || (result->status != 0 && parentResult->status == 0)) {
	 memcpy(parentResult, result, sizeof(SCSI_Result));
      }

      cac->handled++;
      if (cac->handled != cac->needed) {
         SP_Unlock(&childInfo->parentToken->lock);
         Async_ReleaseToken(childInfo->parentToken);
         return;
      }
      else {
         childInfo->parentToken->callerPrivateUsed -= sizeof(COW_AsyncCounter);
         SP_Unlock(&childInfo->parentToken->lock);
         COWTokenCallback(childInfo->parentToken); 
         Async_ReleaseToken(childInfo->parentToken);

         /*
          * Destroy the cowIOInfo and free the memory associated with that
          */
         Semaphore_EndRead(&chi->rwlock);
         COWDestroyIOInfo(childInfo->cowIOInfo, childInfo->validRedos);
         Mem_Free(childInfo->cowIOInfo);
      }
   }
   else {
      memcpy(parentResult, result, sizeof(SCSI_Result));
      SP_Unlock(&childInfo->parentToken->lock);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * COWAsyncWriteDone --
 *
 * Async Write completion routine for the Data Writes
 *
 * Results:
 *
 * Side effects:
 *             Will issue the MetaData write from the bottom half
 *
 *-----------------------------------------------------------------------------
 */
static void
COWAsyncWriteDone(Async_Token *token, void *data)
{
   COW_MetaDataFrame *frame = (COW_MetaDataFrame *)data;
   COW_MetaDataInfo *cowMetaDataInfo = frame->cowMetaDataInfo;
   COW_MetaDataInfo *cowCmd;
   COWInfo *info;
   VMK_ReturnStatus status;
   COW_MDQ *active;
   COW_HandleInfo *chi = COWGetHandleInfo(cowMetaDataInfo->cowIOInfo->handleID); 
   SCSI_Result *result = (SCSI_Result *)token->result;
   SCSI_Result *parentResult = (SCSI_Result *)cowMetaDataInfo->parentToken->result;

   ASSERT(NULL != chi);
   ASSERT(cowMetaDataInfo->ioState == COW_DATAWRITE_PROG);
   cowMetaDataInfo->ioState = COW_DATAWRITE_DONE;

   /*
    * This is the case of complete cache hit. No MetaData update required.
    */
   if (cowMetaDataInfo->metaDataHead->pair == NULL) {
      goto Done;
   }

   /*
    * Save the SCSI_Result data (status & sense buffer) if this is the
    * first command back, and if this is the command with a SCSI error.
    * Complete the command. Do not do MetaData Write.
    */
   if (parentResult->status == 0) {
      memcpy(parentResult, result, sizeof(SCSI_Result));
   }
   if (!((SCSI_HOST_STATUS(result->status) == SCSI_HOST_OK) && 
       (SCSI_DEVICE_STATUS(result->status) == SDSTAT_GOOD))) {
       goto Done;
   }

   /*
    * Enqueue the MetaData update in the non-working ready queue 
    */ 
   info = cowMetaDataInfo->info;
   SP_Lock(&info->queueLock);
   COWAppendtoQueue(&info->ready, cowMetaDataInfo);
   cowMetaDataInfo->ioState = COW_WAITING_FOR_MDIO;

   if(!COWHasCmds(&info->active)) {
      while ((cowCmd = COWRemoveFromQueue(&info->ready)) != NULL) {
         COWAppendtoQueue(&info->active, cowCmd);
      }
   } else {
      // frame will be freed after the callback returns
      SP_Unlock(&info->queueLock);
      return;
   }

   /*
    * The current algorithm walks through the list of commands in the active list
    * and updates the cache and issues a MetaData Write for each command.
    * This might involve the same pages in the cowCache being written out.
    * We can optimise by combining these writes to a single write , but we will
    * have to maintain the list of tokens to be notified to the VM after that single
    * write completes
    */ 
   active = &info->active;
   cowCmd = active->head;
   SP_Unlock(&info->queueLock);

   while ((cowCmd != (COW_MetaDataInfo *)active) && (cowCmd->ioState == COW_WAITING_FOR_MDIO)) {
      ASSERT(token == cowCmd->cowIOInfo->token);
      /*
       * Update the cowCache now ..
       */
      COWUpdateCache(cowCmd);

      status = COWWriteMetaDataInfo(cowCmd, cowCmd->cowIOInfo->token); 
      if (status != VMK_OK) {
         // Signal the VM that command is done and complete the command.
         COWCompleteCommand(cowCmd);
         return;
      }
      cowCmd = cowCmd->next;
   }
   return;
Done :
   // Call the parent token callback
   COWTokenCallback(cowMetaDataInfo->parentToken); 
   Async_ReleaseToken(cowMetaDataInfo->parentToken);
   
   cowMetaDataInfo->ioState = COW_METADATAWRITE_DONE;
   if (NULL == chi) {
      Warning("AsyncWriteDone: Chould not find a valid COWHandleInfo");
   } else {
      Semaphore_EndRead(&chi->rwlock);
   }

   if (cowMetaDataInfo->cowIOInfo) {
      COWDestroyIOInfo(cowMetaDataInfo->cowIOInfo, 0);
      Mem_Free(cowMetaDataInfo->cowIOInfo);
   }
   //DestroyInfo is releasing the parent token ..
   COWDestroyMetaDataInfo(cowMetaDataInfo);
   Mem_Free(cowMetaDataInfo);
   return;
}

/*
 *-----------------------------------------------------------------------------
 *
 * COWAsyncMetaDataWriteDone --
 *
 * Async MetaData Write completion routine for the MetaData Writes
 *
 * Results:
 *
 * Side effects:
 *
 *
 *-----------------------------------------------------------------------------
 */
static void
COWAsyncMetaDataWriteDone(Async_Token *token, void *data)
{
   COW_MetaDataFrame *cowMetaDataFrame = (COW_MetaDataFrame *)data;
   COW_MetaDataInfo *cowMDInfo = cowMetaDataFrame->cowMetaDataInfo;
   COWInfo *info = cowMDInfo->info;
   COW_MDQ *active = &info->active;
   COW_MetaDataInfo *cowCmd;
   VMK_ReturnStatus status;
   SCSI_Result *result = (SCSI_Result *)token->result;
   SCSI_Result *parentResult = (SCSI_Result *)cowMDInfo->parentToken->result;

   ASSERT(cowMDInfo->ioState == COW_METADATAWRITE_PROG);

   memcpy(parentResult, result, sizeof(SCSI_Result));

   //DestroyInfo is releasing the token ..
   Async_ReleaseToken(token);

   COWCompleteCommand(cowMDInfo);
   
   SP_Lock(&info->queueLock);
   if(!COWHasCmds(&info->active)) {
      while ((cowCmd = COWRemoveFromQueue(&info->ready)) != NULL) {
         COWAppendtoQueue(&info->active, cowCmd);
      }
   }
   cowCmd = active->head;
   SP_Unlock(&info->queueLock);

   if ((cowCmd != (COW_MetaDataInfo *)active) && (cowCmd->ioState == COW_WAITING_FOR_MDIO)) {
      Async_Token *childToken = cowCmd->cowIOInfo->token;
      /*
       * Update the cowCache now ..
       */
      COWUpdateCache(cowCmd);
 
      status = COWWriteMetaDataInfo(cowCmd, childToken);
      if (status != VMK_OK) {
         // Remove the cmd from the active Queue, signal VM and Complete Command
         COWCompleteCommand(cowCmd);
         return;
      } 
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * COWWriteMetaDataInfo --
 *
 * Write the MetaData information to the redo log (for the crash consistency).
 *
 * Results:
 *
 * Side effects:
 *
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
COWWriteMetaDataInfo(COW_MetaDataInfo *cowCmd, Async_Token *token)
{
   COWMetaData       *ptr = NULL;
   SG_Array          *mdsg = NULL;   
   COW_MetaDataFrame *cowMetaDataFrame;
   VMK_ReturnStatus  status;
   int               i;

   ASSERT(cowCmd->ioState == COW_WAITING_FOR_MDIO);
   cowCmd->ioState = COW_CACHEUPDATE_DONE;
      
   cowCmd->sg_Arr = (SG_Array *)Mem_Alloc(SG_ARRAY_SIZE(cowCmd->numLeafEntries * PAGES_PER_LEAFENTRY));
   ASSERT(cowCmd->sg_Arr != NULL);

   memset(cowCmd->sg_Arr, 0, SG_ARRAY_SIZE(cowCmd->numLeafEntries * PAGES_PER_LEAFENTRY));
   cowCmd->sg_Arr->length = 0;
   cowCmd->sg_Arr->addrType = SG_MACH_ADDR;

   mdsg = cowCmd->sg_Arr;

   /*
    *  Update the MetaData sg entries.
    */
   ptr = cowCmd->metaDataHead;
   if (ptr == NULL) {
      return VMK_OK;
   } else {
      while(ptr != NULL) {
         COWPair *pair = ptr->pair;
         for (i = 0; i < PAGES_PER_LEAFENTRY; i++) {
            mdsg->sg[mdsg->length].offset = SECTORS_TO_BYTES(pair->sectorOffset) + i * PAGE_SIZE;
            mdsg->sg[mdsg->length].length = PAGE_SIZE;
	    mdsg->sg[mdsg->length].addr = MPN_2_MA(pair->mpns[i]);
            mdsg->length++;
         }

	 if (mdsg->length > 0) {
            if (mdsg->sg[mdsg->length].offset != 
	       mdsg->sg[mdsg->length - 1].offset + mdsg->sg[mdsg->length - 1].length) {
               /* offset should be disk-block aligned, if there is
	        * a discontinuity Let the File Systems handle this one. 
                if ((mdsg->sg[mdsg->length].offset & (DISK_SECTOR_SIZE - 1)) != 0) {
                   status = VMK_BAD_PARAM;
                   return status;
                }
                */
            } else if (mdsg->sg[mdsg->length].addr == 
                      (mdsg->sg[mdsg->length-1].addr + mdsg->sg[mdsg->length-1].length)) {
	        /* Merge this scatter-gather entry with the
		 * preceding one if COW grains are next to each
		 * other in COW file.  Important so that the # of
		 * scatter-gather entries don't get too large. */
	        mdsg->length--;
		mdsg->sg[mdsg->length].length += mdsg->sg[mdsg->length+1].length;
	    }
	 }
         ptr = ptr->next;
      }
   }
   cowMetaDataFrame = (COW_MetaDataFrame *)Async_PushCallbackFrame(token, 
                                           COWAsyncMetaDataWriteDone, 
                                           sizeof(COW_MetaDataFrame));

   cowMetaDataFrame->magic = COW_ASYNC_COUNTER_MAGIC;
   cowMetaDataFrame->cowMetaDataInfo = cowCmd;
   token->flags |= ASYNC_CANT_BLOCK;

   // Reference count on the original
   Async_RefToken(token);
   cowCmd->ioState = COW_METADATAWRITE_PROG;

   status = FSS_AsyncFileIO(cowCmd->fileHandle, mdsg, token, FS_WRITE_OP | FS_CANTBLOCK);
   if (status != VMK_OK) {
       ((SCSI_Result *)cowCmd->parentToken->result)->status =
           SCSI_MAKE_STATUS(SCSI_HOST_ERROR, SDSTAT_GOOD);
       Async_ReleaseToken(token);
   }
   return status;
}

/*
 *-----------------------------------------------------------------------------
 *
 * COWCompleteCommand --
 *
 * Command Completion for the writes.
 *
 * Results:
 *    Remove the command from the active Queue
 *    Signal the VM that the command is completed
 *
 * Side effects:
 *    The command will be destroyed too.
 *
 *
 *-----------------------------------------------------------------------------
 */
static void
COWCompleteCommand(COW_MetaDataInfo *cowMetaDataInfo)
{
   if (cowMetaDataInfo) {
      COW_HandleInfo *chi = COWGetHandleInfo(cowMetaDataInfo->cowIOInfo->handleID); 
      // Call the parent token callback
      ASSERT(NULL != chi);
      COWTokenCallback(cowMetaDataInfo->parentToken); 
      Async_ReleaseToken(cowMetaDataInfo->parentToken);
      Semaphore_EndRead(&chi->rwlock);
   
      cowMetaDataInfo->ioState = COW_METADATAWRITE_DONE;
      //Remove the command from the Active Queue
      SP_Lock(&cowMetaDataInfo->info->queueLock);
      COWCmdRemoveFromQueue(cowMetaDataInfo);
      SP_Unlock(&cowMetaDataInfo->info->queueLock);

      if (cowMetaDataInfo->cowIOInfo) {
         COWDestroyIOInfo(cowMetaDataInfo->cowIOInfo, 0);
         Mem_Free(cowMetaDataInfo->cowIOInfo);
      }

      //DestroyInfo is releasing the parent token ..
      COWDestroyMetaDataInfo(cowMetaDataInfo);
      Mem_Free(cowMetaDataInfo);
   }
}

/*
 * Updates te cache for the given meta data write
 */
static void
COWUpdateCache(COW_MetaDataInfo *metaDataInfo)
{
   COWInfo *info = metaDataInfo->info;
   COWMetaData *cowMetaDataList = metaDataInfo->metaDataHead; 
   int leafPos, grain, i;
   COWMetaData   *ptr = NULL;
   
   ptr = cowMetaDataList;
   while (ptr != NULL) {
      COWMDPair *mdPair = ptr->metaPair; 
      COWPair *pair = ptr->pair; 
      SP_Lock(&pair->leafEntrySpin);
      COWMapLeafEntry(info->leafEntryAddr, pair);
      for ( i = 0; i < ptr->numIOs; i++) {
          grain = mdPair[i].sector / info->granularity; 
          leafPos = grain % COW_NUM_LEAF_ENTRIES;
          info->leafEntryAddr->sectorOffset[leafPos] = mdPair[i].metaSector;
          ASSERT(info->leafEntryAddr->sectorOffset[leafPos] < info->freeSector);
      }
      SP_Unlock(&pair->leafEntrySpin);
      ptr = ptr->next;
   }
}

/*
 * Wrapper over the COWReadGetLBNAndFID..used by the syscall to get the 
 * actual block in the Redo Log or Base disk
 */
VMK_ReturnStatus 
COW_GetBlockOffsetAndFileHandle(COW_HandleID cowHandleID,  uint32 sector,
                                FS_FileHandleID *fid, 
				uint64 *actualBlockNumber, 
				uint32 *length)
{
   VMK_ReturnStatus  status;
   uint64            currentBlock;
   FS_FileHandleID   currentFid;
   uint32            count, numSectors;
   COW_HandleInfo    *chi = COWGetHandleInfo(cowHandleID);
   FS_FileAttributes attrs;
   Bool              contiguous;

   if (NULL == chi) {
      return VMK_INVALID_HANDLE;
   }

   /*
    * Get the size of the base disk, to make sure we don't try to map
    * blocks past the end of the file...
    */
   status = FSClient_GetFileAttributes(chi->cowFSInfo[0].fsFileHandleID, &attrs);
   if (VMK_OK != status) {
      return status;
   }
   numSectors = (uint32)(attrs.length / (uint64)attrs.diskBlockSize);
   if (sector >= numSectors) {
      // attempt to map past the end of the virtual disk
      return VMK_LIMIT_EXCEEDED;
   }

   status = COWReadGetLBNAndFID(cowHandleID, sector, &currentFid, 
				&currentBlock);
   if (VMK_OK == status) {
      *actualBlockNumber = currentBlock;
      *fid = currentFid;
   }
   count = 1;   
   contiguous = TRUE;
   while ((VMK_OK == status) && // prior call to GetLBNAndFID succeeded
	  (sector + count < numSectors) && // next index not past end	  
	  contiguous // blocks so far are contiguous
	 ) {

      status = COWReadGetLBNAndFID(cowHandleID, sector+count, &currentFid, 
				   &currentBlock);
      contiguous = (*fid == currentFid) && 
	 (*actualBlockNumber + count == currentBlock);
      if (contiguous) {
	 count += 1;
      }
   }

   if (VMK_OK == status) {      
      *length = count;
   }

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * COWReadGetLBNAndFID --
 *
 *      Return the actual File handle and the absolute offset inside
 *      the VMFS File for the indicated COW sector. 
 *
 * Results:
 *      File handle and absolute Logical Block offset on the vmfs File
 *
 * SideEffects :
 *      This can sleep.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
COWReadGetLBNAndFID(COW_HandleID cowHandleID, // IN: the COW disk node
		     uint32 sector,	// IN: the sector offset
		     FS_FileHandleID *fid,	// OUT: actual File Handle for indicated COW Sector 
		     uint64 *actualBlockNumber) // OUT: abs. sector offset on raw disk
{
   COW_HandleInfo *cowHandleInfo = COWGetHandleInfo(cowHandleID); 
   COW_FSInfo *cowFSInfo;
   COWInfo *cowInfo;
   int rootIdx, index, leafPos;
   uint32 grain;
   uint64 realSector;
   VMK_ReturnStatus status;
   COWPair *pair;

   ASSERT_VALID_COWHANDLE(cowHandleInfo);
   cowFSInfo = &cowHandleInfo->cowFSInfo[cowHandleInfo->validRedos];
   for (index = cowHandleInfo->validRedos; index > 0 ; index--) {
      cowFSInfo = &cowHandleInfo->cowFSInfo[index];
      cowInfo = cowFSInfo->cowInfo;

      ASSERT(cowInfo->rootEntries);
      ASSERT(cowInfo->fd == cowFSInfo->fsFileHandleID);

      grain = sector / cowInfo->granularity;
      rootIdx = grain / COW_NUM_LEAF_ENTRIES;
      leafPos = grain % COW_NUM_LEAF_ENTRIES;

      /*
       * Validate the sector offset.
       */
      if (rootIdx < 0 || rootIdx >= cowInfo->numRootEntries) {
	 LOG(4, "Failed: rootIdx = %d, cowInfo->numRootEntries = %d", 
	     rootIdx, cowInfo->numRootEntries);
	 return VMK_METADATA_READ_ERROR;
      }

      if (!cowInfo->rootEntries[rootIdx].sectorOffset) {
         //Not present at this level
         continue;
      } else {
         ASSERT(cowInfo->rootEntries[rootIdx].sectorOffset);
         status = COWCacheLookup(cowInfo, cowInfo->rootEntries[rootIdx].sectorOffset,
                                 TRUE, TRUE, &pair);
         if (status != VMK_OK) {
            return status;
         }
         COWMapLeafEntry(cowInfo->leafEntryAddr, pair);
         cowInfo->mapPcpuNum = myPRDA.pcpuNum;

         if (!cowInfo->leafEntryAddr->sectorOffset[leafPos]) {
            //Not present at this level
            continue;
         } else {
            ASSERT(cowInfo->leafEntryAddr->sectorOffset[leafPos]);
            ASSERT(cowInfo->leafEntryAddr->sectorOffset[leafPos] < cowInfo->freeSector);
            realSector = cowInfo->leafEntryAddr->sectorOffset[leafPos] +
               (sector % cowInfo->granularity);
            *actualBlockNumber = realSector;
            *fid = cowInfo->fd;
            return VMK_OK;
         }
      }
   } /* End of for wrt index*/

   //Data is in base disk
   if (cowHandleInfo->cowFSInfo[0].cowInfo) {
      //sparse base disk
      *actualBlockNumber = COW_NULL_SECTOR_NO;
      *fid = FS_INVALID_FILE_HANDLE;
   } else {
      *actualBlockNumber = sector;
      *fid = cowHandleInfo->cowFSInfo[0].fsFileHandleID;
   }

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * COWWriteLBNAndFID --
 *
 *      Return the actual disk handle and the absolute offset inside
 *      the VMFS partition for the indicated COW sector. 
 *      Allocates the sector if necessary.
 *
 * Results:
 *      Disk handle and absolute sector offset on the partition
 *
 * Side effects:
 *      May allocate space in the COW disk or read a parent disk.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
COWWriteGetLBNAndMDB(COW_HandleID cowHandleID, // IN: the COW disk node
		     uint32 sector,	     // IN: the sector offset
                     COW_MetaDataInfo *cowMetaDataInfo,// OUT: MetaData Block Number
		     uint64 *actualBlockNumber)// OUT:abs. sector offset on raw disk
{
   COW_HandleInfo *cowHandleInfo = COWGetHandleInfo(cowHandleID); 
   COW_FSInfo *cowFSInfo;
   COWInfo *cowInfo;
   int rootIdx;
   int leafPos;
   uint32 len;
   uint64 offset;
   uint32 bytes;
   VMK_ReturnStatus status;
   uint64 realSector;
   uint32 grain;
   COWPair *pair;

   ASSERT_VALID_COWHANDLE(cowHandleInfo);
   cowFSInfo = &cowHandleInfo->cowFSInfo[cowHandleInfo->validRedos];
   cowInfo = cowFSInfo->cowInfo;

   ASSERT(cowInfo->rootEntries);
   ASSERT(cowInfo->fd == cowFSInfo->fsFileHandleID);

   grain = sector / cowInfo->granularity;
   rootIdx = grain / COW_NUM_LEAF_ENTRIES;
   leafPos = grain % COW_NUM_LEAF_ENTRIES;

   /*
    * Validate the sector offset.
    */
   if (rootIdx < 0 || rootIdx >= cowInfo->numRootEntries) {
      LOG(1, "Failed: rootIdx = %d, cowInfo->numRootEntries = %d", 
          rootIdx, cowInfo->numRootEntries);
      return VMK_METADATA_READ_ERROR;
   }

   LOG(4, "rootIdx = %d, cowInfo->numRootEntries = %d, sectorOffset = %d", 
       rootIdx, cowInfo->numRootEntries, cowInfo->rootEntries[rootIdx].sectorOffset);

   if (!cowInfo->rootEntries[rootIdx].sectorOffset) {
      int leafSector;
      leafSector = cowInfo->freeSector;
      ASSERT((sizeof(COWLeafEntry) & (DISK_SECTOR_SIZE-1))==0);

      status = COWIncrementFreeSector(cowInfo, sizeof(COWLeafEntry)/DISK_SECTOR_SIZE);
      if (status != VMK_OK) {
         return status;
      }

      cowInfo->rootEntries[rootIdx].sectorOffset = leafSector;
      /*
       * Find a free leaf entry in the cache.
       */
      status = COWCacheLookup(cowInfo, leafSector, FALSE, FALSE, &pair);
      if (status != VMK_OK) {
         return status;
      }
      COWMapLeafEntry(cowInfo->leafEntryAddr, pair);
      cowInfo->mapPcpuNum = myPRDA.pcpuNum;
      memset(cowInfo->leafEntryAddr, 0, sizeof(COWLeafEntry));
#ifdef COW_TIMING
      start = GET_TSC();
#endif
      /* Save the leaf entry to disk. This zeroes out the metaData Blocks of 16k*/
      status = COWWriteEntry(cowInfo->fd, SECTORS_TO_BYTES(leafSector), pair);
      if (status != VMK_OK) {
         return status;
      }

      /* Save the table of rootEntries to the host disk */
      offset = SECTORS_TO_BYTES(cowInfo->rootOffset);
      len = cowInfo->numRootEntries * sizeof(COWRootEntry);
      status = FSS_BufferIO(cowInfo->fd, offset, (uint64)(unsigned int)cowInfo->rootEntries, 
                            len, FS_WRITE_OP, SG_VIRT_ADDR, &bytes);
      cowInfo->initWrites++;
#ifdef COW_TIMING
      cowInfo->initTime += (int) (GET_TSC() - start);
#endif
      if (status != VMK_OK) {
         return status;
      }
      if (myPRDA.pcpuNum != cowInfo->mapPcpuNum) {
         /* Revalidate if there was a migration during the writes. */
         COWMapLeafEntry(cowInfo->leafEntryAddr, pair);
	 cowInfo->mapPcpuNum = myPRDA.pcpuNum;
      }
   }
   else {
      ASSERT(cowInfo->rootEntries[rootIdx].sectorOffset);
      status = COWCacheLookup(cowInfo, cowInfo->rootEntries[rootIdx].sectorOffset,
	    		      TRUE, FALSE, &pair);
      if (status != VMK_OK) {
          return status;
      }
      COWMapLeafEntry(cowInfo->leafEntryAddr, pair);
      cowInfo->mapPcpuNum = myPRDA.pcpuNum;
   }

   if (!cowInfo->leafEntryAddr->sectorOffset[leafPos]) {
      int curSec, grainSec;

      curSec = sector & ~(cowInfo->granularity - 1);
      grainSec = cowInfo->freeSector;

      status = COWIncrementFreeSector(cowInfo, cowInfo->granularity);
      if (status != VMK_OK) {
         return status;
      }

      if (myPRDA.pcpuNum != cowInfo->mapPcpuNum) {
         /* Revalidate if there was a migration during
          * COWIncrementFreeSector(). */
          COWMapLeafEntry(cowInfo->leafEntryAddr, pair);
	  cowInfo->mapPcpuNum = myPRDA.pcpuNum;
      }
      //Update COWCache after the Data write is done
      //cowInfo->leafEntryAddr->sectorOffset[leafPos] = grainSec;
      realSector = grainSec + (sector % cowInfo->granularity);

      status = COWInsertMetaDataList(cowMetaDataInfo, pair, sector, grainSec);
      if (status != VMK_OK)
          return status;
#ifdef XXX
      /* Read the grain from the parent disk */
      if (cowInfo->link != -1) {
         status = FSS_BufferIO(cowInfo->link, SECTORS_TO_BYTES(curSec),
                               (uint64)(unsigned long)cowInfo->tempSectorBuffer,
                               cowInfo->granularity * DISK_SECTOR_SIZE,
                               FS_READ_OP , SG_VIRT_ADDR, &bytes);
	 if (status != VMK_OK) {
	     return status;
	 }
      } else {
         ASSERT(cowInfo->flags & COWDISK_ROOT);
         memset(cowInfo->tempSectorBuffer, 0, cowInfo->granularity * DISK_SECTOR_SIZE);
      }
      /* Write the grain to the child disk */
       offset = SECTORS_TO_BYTES(cowInfo->leafEntryAddr->sectorOffset[leafPos]);
       len = cowInfo->granularity * DISK_SECTOR_SIZE;
       status = FSS_BufferIO(cowInfo->fd, offset, (uint64)(unsigned int)cowInfo->tempSectorBuffer, len,
                             FS_WRITE_OP, SG_VIRT_ADDR, &bytes);
       if (status != VMK_OK) {
          return status;
       }

       /*
        * Update of COW dirty Leaf Entries will not be done.
        */
       status = COWWriteDirtyLeafEntry(cowInfo, pair, TRUE, FALSE);
       if (status != VMK_OK) {
          return status;
       }
       if (myPRDA.pcpuNum != cowInfo->mapPcpuNum) {
          /* Revalidate if there was a migration during COWWriteDirtyLeafEntry(). */
	  COWMapLeafEntry(cowInfo->leafEntryAddr, pair);
	  cowInfo->mapPcpuNum = myPRDA.pcpuNum;
       }
#endif
   } else {
      ASSERT(cowInfo->leafEntryAddr->sectorOffset[leafPos]);
      ASSERT(cowInfo->leafEntryAddr->sectorOffset[leafPos] < cowInfo->freeSector);
      realSector = cowInfo->leafEntryAddr->sectorOffset[leafPos] +
          (sector % cowInfo->granularity);
   }

   *actualBlockNumber = realSector;
   return VMK_OK;
}

/*
 * Insert the COWMDPair for the each write block
 */
static VMK_ReturnStatus
COWInsertMetaDataList(COW_MetaDataInfo *cowMetaDataInfo, COWPair *pair, uint32 sector, 
                      uint32 grainSec)
{
   VMK_ReturnStatus status = VMK_OK;
   COWMetaData *ptr = cowMetaDataInfo->metaDataHead;
   uint32 allocedBlocks = 0;

   while (ptr != NULL) {
      if ((ptr->pair == NULL) || (ptr->pair == pair)) {
         if (!ptr->pair) {
            ptr->pair = pair;
            SP_Lock(&pair->leafEntrySpin);
            pair->numWrites++;
            SP_Unlock(&pair->leafEntrySpin);
         }
         ptr->metaPair[ptr->numIOs].sector = sector;
         ptr->metaPair[ptr->numIOs].metaSector = grainSec;
         ptr->numIOs++;
         break;
      } else {
         allocedBlocks += ptr->numIOs;
         ptr = ptr->next;
      }
   }

   if (ptr == NULL) {
      ptr = Mem_Alloc(sizeof(COWMetaData));
      if (ptr == NULL)
         return VMK_NO_MEMORY;
      memset(ptr, 0, sizeof(COWMetaData));
      // Revisit wrt how many COWMDPair to allocate
      ptr->metaPair = Mem_Alloc(sizeof(COWMDPair) * (cowMetaDataInfo->totalBlocks - allocedBlocks));
      if ( ptr->metaPair == NULL ) {
         Mem_Free(ptr);
         return VMK_NO_MEMORY;
      }
      memset(ptr->metaPair, 0, sizeof(COWMDPair) * (cowMetaDataInfo->totalBlocks - allocedBlocks));
      ptr->pair = pair; 
      SP_Lock(&pair->leafEntrySpin);
      pair->numWrites++;
      SP_Unlock(&pair->leafEntrySpin);
      ptr->metaPair[ptr->numIOs].sector = sector;
      ptr->metaPair[ptr->numIOs].metaSector = grainSec;
      ptr->numIOs++;
      cowMetaDataInfo->numLeafEntries++;
      ptr->next = cowMetaDataInfo->metaDataHead;
      cowMetaDataInfo->metaDataHead = ptr;
   }
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * COWIncrementFreeSector --
 *
 *      Allocate the next 'increment' sectors in COW file.  Increase the size
 *	of the COW file itself if necessary. upgradeableLock should be TRUE
 *	if caller (actually the FS on the callers behalf) is holding shared
 *	ioAccess.
 *
 * Results:
 *      A VMK_ReturnStatus value
 *
 * Side effects:
 *      The COW file may be increased in length.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
COWIncrementFreeSector(COWInfo *info,       // IN/OUT: the COW disk
                       uint32 increment)    // IN: number of sectors to add
{
   VMK_ReturnStatus status = VMK_OK;
   int allocSectors;
   LOG(4, "increment %d freeSector %d", increment, info->freeSector);

   info->freeSectorChanged = TRUE;
   if (info->freeSector + increment > info->allocSectors) {
      LOG(4, "increment %d freeSector %d", increment, info->freeSector);
      FS_FileAttributes attrs;

      /* Grow the COW file in increments of COWDISK_SIZE_INCREMENT sectors. */
      allocSectors = info->allocSectors + COWDISK_SIZE_INCREMENT;
      attrs.length = SECTORS_TO_BYTES(allocSectors);
      status = FSClient_SetFileAttributes(info->fd, FILEATTR_SET_LENGTH, &attrs);
      if (status != VMK_OK) {
         return status;
      }
      info->allocSectors = allocSectors;
      info->freeSector += increment;
      ASSERT(info->freeSector <= info->allocSectors);
      return VMK_OK;
   }
   else {
      info->freeSector += increment;
      return VMK_OK;
   }
}

#define COW_COMMIT_SECTORS 512

/*
 *----------------------------------------------------------------------
 *
 * COWCommit --
 *
 *      Read sectors from a COW disk and write them to the parent disk,
 *	according to the specified fraction ranges.  If doing up to
 *	FS_MAX_COMMIT_FRACTION, also change the generation number of the
 *	parent to match the child.
 *
 * Results:
 *      A VMK_ReturnStatus value.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
COWCommit(COW_HandleID cowHandleID, int level, int startFraction, int endFraction)
{
   int i;
   int j;
   VMK_ReturnStatus status;
   uint32 bytes;
   char *buf;
   int k = 0, l;
   COWLeafEntry *leafEntry;
   int start, end;
   FS_FileAttributes attrs;
   COW_HandleInfo *chi = COWGetHandleInfo(cowHandleID);
   COWInfo *info = NULL;
   FS_FileHandleID parentHandleID;

   if (NULL == chi) {
      return VMK_INVALID_HANDLE;
   }

   parentHandleID  = chi->cowFSInfo[level-1].fsFileHandleID;
   LOG(0, "parentHandle %Ld", parentHandleID);
   if (level > 1)
      info = chi->cowFSInfo[level-1].cowInfo; 
   else 
      info = chi->cowFSInfo[level].cowInfo; 

   ASSERT(startFraction <= FS_MAX_COMMIT_FRACTION);
   ASSERT(endFraction <= FS_MAX_COMMIT_FRACTION);

   if (parentHandleID == -1) {
      return VMK_OK;
   }

   /* TODO: Check if there's enough space if parent disk a COW too. */

   /*
    * Since we are writing in its original generation number, this
    * has no effect except turning off regenerateGeneration, so the
    * generation number doesn't get changed by the first write.  This
    * is important, so that the commit is idempotent and can be rerun
    * if interrupted.
    */
   FSClient_GetFileAttributes(parentHandleID, &attrs);
   status = FSClient_SetFileAttributes(parentHandleID, FILEATTR_SET_GENERATION, &attrs);
   if (status != VMK_OK) {
      return status;
   }

   buf = Mem_Alloc(info->granularity * COW_COMMIT_SECTORS * DISK_SECTOR_SIZE);
   if (buf == NULL) {
      return VMK_NO_MEMORY;
   }
   leafEntry = Mem_Alloc(sizeof(COWLeafEntry));
   if (leafEntry == NULL) {
      Mem_Free(buf);
      return VMK_NO_MEMORY;
   }

   /*
    * Read all grains in COW disk and write them at appropriate
    * location in the destination disk.
    */
   start = ((int64)info->numRootEntries * (int64)startFraction) / FS_MAX_COMMIT_FRACTION;
   end = ((int64)info->numRootEntries * (int64)endFraction) / FS_MAX_COMMIT_FRACTION;
   LOG(0, "%d %d", start, end);
   for (i = start; i < end; i++) {
      if (info->rootEntries[i].sectorOffset) {
         uint32 entry;

         entry = info->rootEntries[i].sectorOffset;
         status = FSS_BufferIO(info->fd, SECTORS_TO_BYTES(entry),
                               (uint64)(unsigned int)leafEntry, sizeof(COWLeafEntry),
                               FS_READ_OP, SG_VIRT_ADDR, &bytes);
         if (status != VMK_OK) {
            goto error;
         }

         for (j = 0; j < COW_NUM_LEAF_ENTRIES; j = k) {
            uint32 sector;

            if (leafEntry->sectorOffset[j] == 0) {
               k = j+1;
               continue;
            }

            /*
             * Read in sequences of grains until we hit a zero sector,
             * so we can write them all out at once.
             */
            k = j;
            while (k < COW_NUM_LEAF_ENTRIES && k -j < COW_COMMIT_SECTORS) {
               if (leafEntry->sectorOffset[k] == 0) {
                  break;
               }
               /*
                * Read in grains that are consecutive in the COW file
                * all at once, to speed up commit.
                */
               for (l = k+1; l < COW_NUM_LEAF_ENTRIES && l - j < COW_COMMIT_SECTORS; l++) {
                  if (leafEntry->sectorOffset[l] != leafEntry->sectorOffset[l-1]+1){
                     break;
                  }
               }
               status = FSS_BufferIO(info->fd,
                                     SECTORS_TO_BYTES(leafEntry->sectorOffset[k]),
                                     (uint64)(unsigned int)(buf + (k-j) * info->granularity * DISK_SECTOR_SIZE),
                                     (l-k) * info->granularity * DISK_SECTOR_SIZE,
                                     FS_READ_OP, SG_VIRT_ADDR, &bytes);
               if (status != VMK_OK) {
                  goto error;
               }
               k = l;
            }
            sector = (i * COW_NUM_LEAF_ENTRIES + j) * info->granularity;
            status = FSS_BufferIO(parentHandleID, SECTORS_TO_BYTES(sector), 
                                  (uint64)(unsigned long)buf,
                                  (k-j) * info->granularity * DISK_SECTOR_SIZE,
                                  FS_WRITE_OP,  SG_VIRT_ADDR,
                                  &bytes);
            if (status != VMK_OK) {
               goto error;
            }
         }
      }
   }

   Mem_Free(leafEntry);
   Mem_Free(buf);

   /*
    * Set the generation number of the now-committed parent to the
    * generation number of the child, so incremental importing of REDO
    * logs will work.
    */
   if (endFraction == FS_MAX_COMMIT_FRACTION) {
      FS_FileAttributes attrs;

      /* Get the parent's file attributes, in case it is a COW file, to
       * make sure that it is opened even if there weren't any writes in
       * this part of the commit.  Then it will be closed cleanly with
       * the new generation number. */
      status = FSClient_GetFileAttributes(parentHandleID, &attrs);
      if (status != VMK_OK) {
         return status;
      }
      /* Now that the commit is done, set the generation and tools/hw
       * versions of the parent to generation and tools/hw version of the
       * child. */
      status = FSClient_GetFileAttributes(info->fd, &attrs);
      if (status != VMK_OK) {
         return status;
      }
      status = FSClient_SetFileAttributes(parentHandleID,
					  FILEATTR_SET_GENERATION |
					  FILEATTR_SET_TOOLSVERSION |
					  FILEATTR_SET_VIRTUALHWVERSION,
					  &attrs);
      if (status != VMK_OK) {
         return status;
      }
   }
   return VMK_OK;

error:
   Mem_Free(leafEntry);
   Mem_Free(buf);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * COWCheck  --
 *
 *      Check that no COW pointers point past the end of the allocated file
 *      size, and determine the first free sector in the file (which may
 *      not be correct in the header if the file was not closed properly).
 *
 * Results:
 *      A VMK_ReturnStatus value.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
COWCheck(COWInfo *info, uint64 length)
{
   int i;
   int j;
   COWLeafEntry *leafEntry;
   uint32 entry;
   VMK_ReturnStatus status;
   int maxsector = 0;
   uint32 bytes;
   int numBadRoots = 0;
   int numBadLeafs = 0;

   /*
    * A COWLeafEntry is too big to allocate on the stack.
    */
   leafEntry = Mem_Alloc(sizeof(COWLeafEntry));
   if (leafEntry == NULL) {
      return VMK_NO_MEMORY;
   }
   for (i = 0; i < info->numRootEntries; i++) {
      if (info->rootEntries[i].sectorOffset != 0) {
         entry = info->rootEntries[i].sectorOffset;
         if (SECTORS_TO_BYTES(entry) >= length) {
            if (numBadRoots < 4 || LOGLEVEL() > 4) {
               Warning("Bad root entry: info->rootEntries[%d].sectorOffset "
                       "(bytes=%"FMT64"d) >= length (%"FMT64"d)",
                       i, SECTORS_TO_BYTES(entry), length);
            }
            numBadRoots++;
         }
         status = FSS_BufferIO(info->fd, SECTORS_TO_BYTES(entry),
                               (uint64)(unsigned int)leafEntry, sizeof(COWLeafEntry),
                               FS_READ_OP, SG_VIRT_ADDR, &bytes);
         if (status != VMK_OK) {
            Mem_Free(leafEntry);
            return status;
         }
         entry += sizeof(COWLeafEntry) / DISK_SECTOR_SIZE;
         if (entry > maxsector) {
            maxsector = entry;
         }

         for (j = 0; j < COW_NUM_LEAF_ENTRIES; j++) {
            uint32 sector = leafEntry->sectorOffset[j] + info->granularity;
            if (SECTORS_TO_BYTES(sector) >= length) {
               if (numBadLeafs < 4 || LOGLEVEL() > 5) {
                  Warning("Bad leaf entry: leafEntry->sectorOffset[%d] + "
                          "info->granularity (bytes = %"FMT64"d) >= length (%"FMT64"d)",
                          j, SECTORS_TO_BYTES(sector), length);
               }
               numBadLeafs++;
            }
            if (sector > maxsector) {
               maxsector = sector;
            }
         }
      }
   }
   Mem_Free(leafEntry);
   Log("Setting freeSector to %d.  bad root entries = %d, bad leaf entry = %d", 
       maxsector, numBadRoots, numBadLeafs);
   info->freeSector = maxsector;
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * COWMapLeafEntry  --
 *
 *      Map data of a cache entry to the specified address.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
COWMapLeafEntry(COWLeafEntry *addr, COWPair *pair)
{
   int i;

   for (i = 0; i < PAGES_PER_LEAFENTRY; i++) {
      TLB_Validate(VA_2_VPN((VA)addr) + i, pair->mpns[i], TLB_LOCALONLY);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * COWInitCache  --
 *
 *      Initialize the cache of leaf entries.  Each entry is allocated from
 *      machine memory, and can only be accesses after a COWMapLeafEntry()
 *      call.
 *
 * Results:
 *      A VMK_ReturnStatus value.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
COWInitCache(COWInfo *info)
{
   int i;
   int j;

   ASSERT(sizeof(COWLeafEntry) == PAGES_PER_LEAFENTRY * PAGE_SIZE);
   ASSERT(PAGES_PER_LEAFENTRY < SG_DEFAULT_LENGTH);

   for (i = 0; i < NUM_LEAF_CACHE_ENTRIES; i++) {
      for (j = 0; j < PAGES_PER_LEAFENTRY; j++) {
         info->cache[i].mpns[j] = INVALID_MPN;
      }
   }

   /* Allocate a virtual address range to be used in accessing the
    * cache entries. */
   info->leafEntryAddr = KVMap_AllocVA(PAGES_PER_LEAFENTRY);
   if (info->leafEntryAddr == NULL) {
      return VMK_NO_MEMORY;
   }

   for (i = 0; i < NUM_LEAF_CACHE_ENTRIES; i++) {
      info->cache[i].sectorOffset = COW_NULL_SECTOR_NO;
      info->cache[i].lastTouch = 0;
      info->cache[i].numWrites = 0;
      /* Allocate machine pages for the COWLeafEntry in the cache entry */
      for (j = 0; j < PAGES_PER_LEAFENTRY; j++) {
         MPN mpn = INVALID_MPN;
         if (MemSched_MemoryIsLowWait(COW_CACHE_TIMEOUT_MS) == VMK_OK) {
            mpn = MemMap_AllocAnyKernelPage();
         }
         if (mpn == INVALID_MPN) {
            /* COWFreeCache() will be called to free up all the MPNs */
            return VMK_NO_MEMORY;
         }
         info->cache[i].mpns[j] = mpn;
      }
      COWMapLeafEntry(info->leafEntryAddr, &(info->cache[i]));
      memset(info->leafEntryAddr, 0, sizeof(COWLeafEntry));
      SP_InitLock("cowLeafEntryLock", &info->cache[i].leafEntrySpin, SP_RANK_LEAF);
   }
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * COWFreeCache  --
 *
 *      Free up the machine memory used by the cache, and the virtual
 *      address range used to access the entries.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
COWFreeCache(COWInfo *info)
{
   int i;
   COWPair *pair;
   int j;

   if (info->leafEntryAddr != NULL) {
      KVMap_FreePages(info->leafEntryAddr);
   }
   for (i = 0; i < NUM_LEAF_CACHE_ENTRIES; i++) {
      pair = &(info->cache[i]);   
      for (j = 0; j < PAGES_PER_LEAFENTRY; j++) {
         if (pair->mpns[j] != INVALID_MPN) {
            MemMap_FreeKernelPage(pair->mpns[j]);
         }
      }
      SP_CleanupLock(&pair->leafEntrySpin);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * COWCacheLookup  --
 *
 *      Look in the cache for leaf entry at specified sector offset.  If
 *      not found, free up an appropriate entry in the cache, writing the
 *      entry out first if it is dirty.  Then, if read is TRUE, actually
 *      read the leaf entry into the cache.  Return a pointer to the cache
 *      entry.
 *
 * Results:
 *      A VMK_ReturnStatus value
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
COWCacheLookup(COWInfo *info, int offset, Bool read, Bool needToLockEntry, COWPair **pairp)
{
   int i;
   COWPair *pair;
   VMK_ReturnStatus status;
   int min;
   int lru;

   if (LOGLEVEL() > 1) {
      if ((info->cacheLookups % 5000) == 0) {
#ifdef COW_TIMING
         Log("COW Cache %d/%d inits %d (%d) dirty %d (%d) reads %d (%d)",
             info->cacheHits, info->cacheLookups,
             info->initWrites, info->initTime / 550000,
             info->dirtyWrites, info->dirtyTime / 550000,
             info->cacheReads, info->readTime / 550000);
         info->initTime = info->dirtyTime = info->readTime = 0;
#else
         LOG(1, "COW Cache %d/%d inits %d dirty %d reads %d",
             info->cacheHits, info->cacheLookups,
             info->initWrites, info->dirtyWrites, info->cacheReads);
#endif
         info->cacheHits = info->cacheLookups = info->initWrites =
            info->dirtyWrites = info->cacheReads = 0;
      }
   }
   info->cacheLookups++;
   info->cacheTime++;
   lru = -1;
   min = -1;
   for (i = 0; i < NUM_LEAF_CACHE_ENTRIES; i++) {
      pair = &(info->cache[i]);
      SP_Lock(&pair->leafEntrySpin);
      if (pair->sectorOffset == offset) {
         while(pair->numWrites && needToLockEntry) {
            CpuSched_Wait((uint32)pair, CPUSCHED_WAIT_FS, &pair->leafEntrySpin);
            SP_Lock(&pair->leafEntrySpin);
         }
         pair->lastTouch = info->cacheTime;
         info->cacheHits++;
         *pairp = pair;
         SP_Unlock(&pair->leafEntrySpin);
         return VMK_OK;
      }
      SP_Unlock(&pair->leafEntrySpin);
      if (min == -1 || pair->lastTouch < min) {
         if (pair->numWrites > 0) {
            continue;
         }
         min = pair->lastTouch;
         lru = i;
      }
   }
   ASSERT(lru != -1);

   pair = &(info->cache[lru]);
   pair->sectorOffset = COW_NULL_SECTOR_NO;
   if (read) {
#ifdef COW_TIMING
      uint64 start = GET_TSC();
#endif
      status = COWReadEntry(info->fd, SECTORS_TO_BYTES(offset), pair);
      if (status != VMK_OK) {
         return status;
      }
      info->cacheReads++;
#ifdef COW_TIMING
      info->readTime += (int)(GET_TSC() - start);
#endif
   }
   SP_Lock(&pair->leafEntrySpin);
   pair->lastTouch = info->cacheTime;
   pair->sectorOffset = offset;
   pair->numWrites = 0;
   SP_Unlock(&pair->leafEntrySpin);

   *pairp = pair;
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * COWReadEntry  --
 *
 *      Read a leaf into a cache entry.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
COWReadEntry(FS_FileHandleID fd, uint64 offset, COWPair *pair)
{
   uint32 bytes;
   SG_Array sgArr;
   int i;

   sgArr.addrType = SG_MACH_ADDR;
   sgArr.length = PAGES_PER_LEAFENTRY;

   for (i = 0; i < PAGES_PER_LEAFENTRY; i++) {
      sgArr.sg[i].offset = offset + i * PAGE_SIZE;
      sgArr.sg[i].length = PAGE_SIZE;
      sgArr.sg[i].addr = MPN_2_MA(pair->mpns[i]);
   }
   return FSS_SGFileIO(fd, &sgArr, FS_READ_OP, &bytes);
}

/*
 *----------------------------------------------------------------------
 *
 * COWWriteEntry  --
 *
 *      Write a leaf from a cache entry.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
COWWriteEntry(FS_FileHandleID fd, uint64 offset, COWPair *pair)
{
   uint32 bytes;
   SG_Array sgArr;
   int i;

   sgArr.addrType = SG_MACH_ADDR;
   sgArr.length = PAGES_PER_LEAFENTRY;

   for (i = 0; i < PAGES_PER_LEAFENTRY; i++) {
      sgArr.sg[i].offset = offset + i * PAGE_SIZE;
      sgArr.sg[i].length = PAGE_SIZE;
      sgArr.sg[i].addr = MPN_2_MA(pair->mpns[i]);
   }
   return FSS_SGFileIO(fd, &sgArr, FS_WRITE_OP, &bytes);
}

/*
 *----------------------------------------------------------------------
 *
 * COW_SpliceParent  --
 *
 *      Change the COW file represented by 'info' to point to the parent of
 *      COW file 'info1'.  This includes changing the COW header of the
 *      'info' file.
 *
 * Results:
 *      A VMK_ReturnStatus value.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
COW_SpliceParent(COW_HandleID cowHandleID, int level) 
{
   COWDisk_Header *hdr, *hdr1;
   uint32 bytes;
   VMK_ReturnStatus status;
   COW_HandleInfo *chi = COWGetHandleInfo(cowHandleID);
   COWInfo *info;
   COWInfo *info1;

   ASSERT(level>=1);
   if (level < 1) {
      // prevent bad things from happening in release builds
      Warning("Illegal level argument");
      return VMK_BAD_PARAM;
   }
   ASSERT_VALID_COWHANDLE(chi);
   info = chi->cowFSInfo[level].cowInfo;
   info1 = chi->cowFSInfo[level-1].cowInfo;

   hdr1 = (COWDisk_Header *)info1->tempSectorBuffer;
   status = FSS_BufferIO(info1->fd, 0, (uint64)(unsigned int)hdr1, sizeof(COWDisk_Header),
                         FS_READ_OP, SG_VIRT_ADDR, &bytes);
   if (status != VMK_OK) {
      return status;
   }

   hdr = (COWDisk_Header *)info->tempSectorBuffer;
   status = FSS_BufferIO(info->fd, 0, (uint64)(unsigned int)hdr, sizeof(COWDisk_Header),
                         FS_READ_OP, SG_VIRT_ADDR, &bytes);
   if (status != VMK_OK) {
      return status;
   }

   strcpy(hdr->u.child.parentFileName, hdr1->u.child.parentFileName);

   status = FSS_BufferIO(info->fd, 0, (uint64)(unsigned int)hdr, sizeof(COWDisk_Header),
                         FS_WRITE_OP, SG_VIRT_ADDR, &bytes);
   if (status != VMK_OK) {
      return status;
   }

   /* Changing the indices in the array of Redos */
   memcpy(&chi->cowFSInfo[level], &chi->cowFSInfo[level + 1], 
          sizeof(COW_FSInfo) * chi->validRedos - level);
   /* This REDO log is now a root disk, since it no longer has a
    * parent. */
   info1->flags |= COWDISK_ROOT;
   return VMK_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 * COW_CommitFile --
 *
 *    Commit the changes of the specified COW file to its parent file.
 *    This is a normal commit if level is zero and virtSCSI is -1.
 *
 *    If level is zero and virtSCSI is not -1, then this is an online
 *    commit of the top REDO log.  Therefore, the virtual SCSI adapter
 *    has to be locked so that the VM can't write while we're committing.
 *    When the commit is done, we change the virtual SCSI pointer to
 *    point to the parent of the REDO log.
 *
 *    If the level is one, then this is an online commit of the
 *    next-to-top REDO log.  When the commit is done, we change the top
 *    REDO log to point to the parent of the next-to-top REDO log.  We
 *    only have to lock the virtual SCSI adapter during the brief time
 *    when we are splicing out the next-to-top REDO log.
 *    
 *    With COW layer coming above the FSS; the level is redundant as we can 
 *    internally determine the level from the fileHandle
 *
 * Results:
 *      A VMK_ReturnStatus value.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
COW_CommitFile(COW_HandleID cowHandleID, int level, int startFraction, int endFraction)
{
   VMK_ReturnStatus status;
   FS_FileHandleID parentHandleID = FS_INVALID_FILE_HANDLE;
   int commitlevel = level - 1;
   COW_HandleInfo *chi = COWGetHandleInfo(cowHandleID);
   COWInfo *cowInfo = NULL, *parentCowInfo = NULL;

   if (NULL == chi) {
      return VMK_INVALID_HANDLE;
   }

   if (level < 1) {
      return VMK_BAD_PARAM;
   }

   parentHandleID = chi->cowFSInfo[commitlevel].fsFileHandleID;

   parentCowInfo = chi->cowFSInfo[commitlevel].cowInfo;
   cowInfo = chi->cowFSInfo[level].cowInfo;

   Semaphore_BeginWrite(&chi->rwlock);

   if (level > 1) {
      /* We are committing the 2nd-level REDO log, which is the parent of
         the specified handle. */
      ASSERT(commitlevel >= 1);
      if(parentCowInfo == NULL)
         COWOpenFile(parentHandleID, &parentCowInfo);
   } else {
      ASSERT(parentCowInfo == NULL);
   }

   LOG(0, "%d %d", startFraction, endFraction);

   if (startFraction == 0 && level > 1) {
      if (parentCowInfo == NULL) {
         status = COWOpenFile(parentHandleID, &parentCowInfo);
         if (status != VMK_OK) {
            Semaphore_EndWrite(&chi->rwlock);
            return status;
         }
      }
   }

   status = COWCommit(cowHandleID, level, startFraction, endFraction);

   if (status != VMK_OK) {
      /* Change the parent to be readable again if the commit failed.
       * Ignore error code, so we can return the commit error code. */
      Semaphore_EndWrite(&chi->rwlock);
      return status;
   }

   if (endFraction == FS_MAX_COMMIT_FRACTION) {
      if (level > 1) {
         /* Wait for SCSI ops to top-level disk to finish and hold up any
          *  further accesses. This ensures that there are no more
          *  accesses to the 2nd-level redo log, before we splice it
          *  out.
          */

         /* Splice out the second-level REDO log. */
         status = COW_SpliceParent(cowHandleID, level);
         Semaphore_EndWrite(&chi->rwlock);
         if (status != VMK_OK) {
            return status;
         }
      }
   } else {
      Semaphore_EndWrite(&chi->rwlock);
   }
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * COW_GetCapacity  --
 *
 *      Returns the size in bytes of of the cowdisk for the handle
 *      passed in.
 *
 * Results:
 *      VMK_OK
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
COW_GetCapacity(COW_HandleID cowHandle, uint64 *lengthInBytes, uint32 *diskBlockSize)
{
   COW_HandleInfo *cowHandleInfo = COWGetHandleInfo(cowHandle);
   VMK_ReturnStatus status = VMK_OK;

   if (NULL == cowHandleInfo) {
      return VMK_INVALID_HANDLE;
   }

   if (cowHandleInfo->cowFSInfo[0].cowInfo) {
      *lengthInBytes = cowHandleInfo->cowFSInfo[0].cowInfo->numSectors;
      *lengthInBytes *= DISK_SECTOR_SIZE;
      *diskBlockSize = DISK_SECTOR_SIZE;
   } else {
      FS_FileAttributes attrs;

      status = FSClient_GetFileAttributes(cowHandleInfo->cowFSInfo[0].fsFileHandleID, &attrs);
      if (status == VMK_OK) {
         *lengthInBytes = attrs.length;
         *diskBlockSize = attrs.diskBlockSize;
      }
   }
   return status;
}


/*
 * XXX temporary wrapper till userland knows how to deal with start & end fractions
 */
VMK_ReturnStatus
COW_Combine(COW_HandleID *cid, int linkOffsetFromBottom)
{
   return COW_CommitFile(*cid, linkOffsetFromBottom, 0, 100);
}


VMK_ReturnStatus 
COW_ResetTarget(COW_HandleID handleID, World_ID worldID, SCSI_Command *cmd)
{
   FS_FileHandleID *handleList;
   int validRedos, index;
   VMK_ReturnStatus status;

   if (NULL == COWGetHandleInfo(handleID)) {
      return VMK_INVALID_HANDLE;
   }
   handleList = Mem_Alloc(sizeof(FS_FileHandleID) * (COW_MAX_REDO_LOG + 1)); 
   if (!handleList) {
      LOG(0, "MemAlloc failed");
      return VMK_NO_MEMORY;
   }

   status = COWGetFileHandles(handleID, handleList, &validRedos);
   if (status != VMK_OK) {
      Mem_Free(handleList);
      Warning("COWGetHandles failed with status %d", status);
      return status;
   }
   
   for (index = validRedos; index >= 0; index--) {
      LOG(3, "Resetting target (command sn %u)\n", cmd->serialNumber);
      LOG(2, "handleList[%d] = %"FMT64"d", index, handleList[index]);

      status = FSS_ResetCommand(handleList[index], cmd);
      if (status != VMK_OK) {
         Warning("Failed to reset handleList[%d] = %"FMT64"d", index, handleList[index]);
      }
   
      // Finally release the reservation on this file (this may actually
      // trigger a physical reset if you are doing clustering)
      status = FSS_ReleaseFile(handleList[index], worldID, TRUE);
      if (status != VMK_OK) {
         Warning("Failed to release handleList[%d] = %"FMT64"d after reset", 
                  index, handleList[index]);
      }
   }
   Mem_Free(handleList);
   return status;
}

VMK_ReturnStatus 
COW_AbortCommand(COW_HandleID handleID, SCSI_Command *cmd)
{
   FS_FileHandleID *handleList;
   int validRedos, index;
   VMK_ReturnStatus status;

   if (NULL == COWGetHandleInfo(handleID)) {
      return VMK_INVALID_HANDLE;
   }

   handleList = Mem_Alloc(sizeof(FS_FileHandleID) * (COW_MAX_REDO_LOG + 1)); 
   if (!handleList) {
      LOG(0, "MemAlloc failed");
      return VMK_NO_MEMORY;
   }

   status = COWGetFileHandles(handleID, handleList, &validRedos);
   if (status != VMK_OK) {
      Mem_Free(handleList);
      Warning("COWGetHandles failed with status %d", status);
      return status;
   }
   
   for (index = validRedos; index >= 0; index--) {
      LOG(3, "Resetting target (command sn %u)\n", cmd->serialNumber);
      LOG(2, "handleList[%d] = %"FMT64"d", index, handleList[index]);
      status = FSS_AbortCommand(handleList[index], cmd);
      if (status != VMK_OK) {
         Warning("Failed to abort commands on handleList[%d] = %"FMT64"d", 
                  index, handleList[index]);
      }
   }
   Mem_Free(handleList);
   return status;
}
