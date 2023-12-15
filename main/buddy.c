/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * buddy.c --
 *
 *    Generic buddy allocator. 
 *    Features:
 *    o Manages virtual as well as physical address ranges
 *    o Color aware
 *    o Supports non-contiguous address range
 *    o Supports hot add 
 *
 */
#include "vm_types.h"
#include "vm_libc.h"
#include "vmkernel.h"
#include "buddy.h"
#include "memmap.h"
#include "kseg.h"
#include "memalloc.h"
#include "util.h"
#include "timer.h"

#define LOGLEVEL_MODULE Buddy
#include "log.h"

#define BUDDY_MAGIC_NUMBER         (0xbdbdbdbd)
#define BUDDY_INVALID_MAGIC_NUMBER (0)
#define BUDDY_MAX_LEN              (0xffffffff)

// We limit max buffer size to 16 in order to limit the size of some
// statically allocated arrays 
#define BUDDY_MAX_NUM_BUFFER_SIZES (16)   // maximum number of buffers sizes
                                          // supported
#define BUDDY_INVALID_BUF_NUM      (0xffffffff)
#define BUDDY_HEAD_BUF_NUM         (BUDDY_INVALID_BUF_NUM - 1)
#define BUDDY_TAIL_BUF_NUM         (BUDDY_INVALID_BUF_NUM - 2)
#define BUDDY_MAX_BUF_NUM          (1 << 31) // 2B buffers 

#define BUDDY_INVALID_BUF_SIZE     (0xffffffff)

#define BUDDY_MAX_SIZE_SHIFT       (31)     // max buf size supported is 2Gig
#define BUDDY_MIN_SIZE_SHIFT       (0)      // min buf size supported is 1
#define BUDDY_INVALID_SIZE_SHIFT   (32)
#define BUDDY_3_BUFS_SIZE_SHIFT    (33)
#define BUDDY_COMPLEX_SIZE_SHIFT   (34)

#define BUDDY_BUF_SIZE_3           (3)
#define BUDDY_MAX_NUM_BUFFERS      (0x00ffffff)

#define BUDDY_MAX_REF_COUNT        (64)

#define BUDDY_MAX_STRING           (256)

#define BUDDY_MAX_SCAN_COUNT       (64 * 1024)

// Set this if you want to turn on additional debug checks
#define BUDDY_AID_DEBUGGING (0)

// All the buffers we work on i.e. for splitting or
// coalescing are *always* in the same block by design
#define BUDDY_FOR_BUFS_DO(info, startBuf, numBufs, curBuf, bufStatus)  \
do {                                                                   \
   uint32 _i;                                                          \
   uint32 _block, _ndx;                                                \
   DEBUG_ONLY(uint32 _numBlockBuffers =                                \
                     (info)->blockSize >> (info)->minBufSizeShift);    \
   BuddyBufNum2BlockStatusNdx((info), (startBuf), &_block, &_ndx);     \
   ASSERT(_block < (info)->numBlocks);                                 \
   ASSERT((numBufs) <= _numBlockBuffers);                              \
   for (_i = 0; _i < (numBufs); _i++) {                                \
      ASSERT((_ndx + _i) < _numBlockBuffers);                          \
      (bufStatus) = &(info)->bufBlocks[_block].bufStatus[_ndx + _i];   \
      (curBuf) = (startBuf) + _i;                                            

#define BUDDY_FOR_BUFS_END \
   }\
} while(0) 


// For all buffers in the given len do
#define BUDDY_FOR_BUFS_IN_LEN_DO(info, startBuf, len, buf, shift, minBufs) \
do {                                                                       \
   uint32 _curSize;                                                        \
   uint32 _curLen = (len);                                                 \
   BufNum _nextBuf = (startBuf);                                           \
   while(_curLen)                                                          \
   {                                                                       \
      (buf) = _nextBuf;                                                    \
      _curSize = BuddyFindLargestBufSize((info), (buf), _curLen,           \
                                         &(shift), &(minBufs));            \
      _curLen -= _curSize;                                                 \
      _nextBuf = (buf) + (minBufs);                                   

#define BUDDY_FOR_BUFS_IN_LEN_END           \
   }                                        \
} while (0)

/*
 * The buddy allocator manages address spaces which it calls as memory spaces
 * i.e. BuddyMemSpace, It maintains a linked list of all the BuddyMemSpaces it
 * manages. 
 *    The memspace is comprised of buffers described by BuddyBufInfo. 
 * The buffers are organized by dividing them into Blocks. i.e BuddyBufBlock
 * Each block is a multiple of max buffer size, and for dynamic regions
 * it is also a power of 2. For static regions there is just one block. For
 * each buffer we store the status of the buffer in BuddyBufStatus, this tells
 * us if the buffer is in use, and if yes what size it is. When not in use
 * these buffers are added to a list of free buffers. We maintain a free list
 * per BufferSize and Color combination. The storage for this free list comes
 * out of BuddyBufBlock.listNodes
 */

/*
 * BufNum type dictates the maximum number of buffers that
 * we can support and thus the amount of space we use for
 * the free list.  
 */
typedef uint32 BufNum;

typedef enum {
   BUDDY_STATIC_SPACE,
   BUDDY_DYNAMIC_SPACE,
} BuddyMemSpaceType;


/*  
 * Since we defragment our buffers, they are not always
 * a power of two and hence we cannot store them as 
 * sizeShifts in the BuddyBufStatus structure. What we
 * do is as follows.
 * 
 *    o If the buffer size is a power of 2 we store it in as size shift,
 *      i.e. BUDDY_SIZE_TYPE_POWEOF2
 *    o If the buffer size is 3 buffers, we store a special value
 *      in the size shift to denote this size. i.e. BUDDY_SIZE_TYPE_3
 *    o For sizes greater than 4, (4 is fine because it is a power of 2) 
 *      we use the first BuddyBufStatus to indicate that this is 
 *      a 'complex' size, and use the remaining 3 BuddyBufStatus's to
 *      store the size as the number of minimum sized buffers in
 *      the 24 bit space. i.e. BUDDY_SIZET_TYPE_COMPLEX
 */
typedef enum {
   BUDDY_SIZE_TYPE_POWEROF2 = 0,
   BUDDY_SIZE_TYPE_3,
   BUDDY_SIZE_TYPE_COMPLEX,
   BUDDY_SIZE_TYPE_MAX,
}BuddySizeType;

/*
 * statistics
 */
typedef struct BuddyBufStatistics {
   uint32 numCarvedBuf;       // total num of minimum size buffers that 
                              // have been carved out
   uint32 numFreeCarvedBuf;   // number of min size carved buffers that
                              // are actually free
   uint32 numFreeBuf[BUDDY_MAX_NUM_BUFFER_SIZES];
   uint32 numUsedBuf[BUDDY_MAX_NUM_BUFFER_SIZES];

   uint32 numTypeAllocated[BUDDY_SIZE_TYPE_MAX]; // num of buffers alloc'd or
   uint32 numTypeReleased[BUDDY_SIZE_TYPE_MAX];  // released for each size type

   uint32 numColors;
   uint32 *colorFreeBuf;        // number of min sized free bufs for each color
   uint32 *colorTotBuf;         // total number of min sized bufs for each color
   Proc_Entry procStats;        // procfs node /proc/vmware/buddy/<name>
   Proc_Entry procStatsVerbose; // procfs node /proc/vmware/buddy/<name>

   // variables for keeping track of avg cycles for allocating and freeing a buffer
   TSCCycles allocHistCycles;
   uint64 allocHistSamples;
   TSCCycles allocRunningCycles;
   uint64 allocRunningSamples;

   TSCCycles freeHistCycles;
   uint64 freeHistSamples;
   TSCCycles freeRunningCycles;
   uint64 freeRunningSamples;
} BuddyBufStatistics;

/*
 * NOTE - listNodes storage requirement:
 * ------------------------------------ 
 *   listNodes, which acts as the storage for linking free buffers. 
 * should ideally be defined as 
 *
 *       BuddyListNode listNodes[numBuffers]
 *
 *    where numBuffers - is the number of minimum sized buffers.
 *
 * But we actually define it like
 *
 *       BuddyListNode listNodes[numBuffers/2]
 *
 * The reason we can get away with using only half the storage is
 * because we rely on the buddy to always coalesce adjacent buffers of
 * minimum size. Thus at any given time assuming the worst case of 
 * fragmentation, we will have a maximum of numBuffers/2 min sized buffers
 * free, any more and we would coalesce them. Thus numBuffers/2 is 
 * sufficient storage for our needs.
 */

/*
 * Double linked list pointers to the next and prev free buffers.
 */
typedef struct BuddyListNode {
   BufNum prev;
   BufNum next;
} BuddyListNode;

typedef struct BuddyFreeList {
   uint32 numColors;  // number of colors
   BufNum *head;   // head ptr to free bufs by color i.e. BufNum head[numColors];
   BufNum *tail;   // tail ptr to free bufs by color i.e. BufNum tail[numColors];
} BuddyFreeList;

// Buffer states
#define BUDDY_BUF_RESERVED  (0) 
#define BUDDY_BUF_FREE      (1)
#define BUDDY_BUF_INUSE     (2)

/*
 * Status of a buffer.
 */
typedef struct BuddyBufStatus {
   unsigned char sizeShift:  6;
   unsigned char state:      2;
#if VMX86_DEBUG  
   uint16 debugWorldID;
   uint16 debugRA;         // right shifted by 8, this is sufficient accuracy
#endif
} __attribute__ ((packed)) BuddyBufStatus;

typedef struct BuddyBufBlock {
   BuddyBufStatus *bufStatus; // status of buffers in this block
   BuddyListNode *listNodes;  // storage to store the next/prev pointers for
                              // free buffers, we need storage for only
                              // 1/2 the buffers in this block.
                              // see comment above.
} BuddyBufBlock;

typedef struct {
   uint32 numColorBits;

   uint32 numBufSizes;
   uint32 minBufSize;         // min buffer size
   uint32 minBufSizeShift;    // minBufSize = (1 << minBufSizeShift)
   uint32 maxBufSize;         // max buffer size
   uint32 maxBufSizeShift;    // maxBufSize = (1 << maxBufSizeShift)

   // values can change after hot add, we dont support
   // decreasing startBuf, *only* increasing end buf or doing
   // a hot add in the middle of an existing range
   uint32 startBuf;           // buffer number of the first buffer
   uint32 endBuf;             // buffer number of the last buffer

   uint32 numBlocks;          // number of blocks in this mem space
   uint32 blockSize;          // size of each block
   BuddyBufBlock  *bufBlocks; // Array of blocks of buffers

   // Following fields provide convenient way to translate a
   // Buffer number to a Block number and Index.
   uint32 blockNumSizeShift;  // number of left shifts of bufNum to get block # 
   uint32 blockNdxMask;       // mask of bufNum to get ndx into a block 

   // for each buffer size, pointer to free buffers
   BuddyFreeList freeList[BUDDY_MAX_NUM_BUFFER_SIZES];

   BuddyBufStatistics stats;  // buffer stats
} BuddyBufInfo;

typedef struct BuddyMemSpace {
   // links should be the first element
   List_Links links;       // linked list of memspaces managed by the buddy
   uint32 magicNumber;     // unique magic number for this memspace

   char name[BUDDY_MAX_MEMSPACE_NAME];
   BuddyMemSpaceType type;
   uint32 start;           // start of this address range

   // these varialbles make more sense for dynamic(hot add) spaces
   uint32 maxLen;          // maximum length of this address range 
   uint32 initialLen;      // initial lenght of this address range

   BuddyBufInfo bufInfo;   // information on all the buffers
   SP_SpinLockIRQ lck;
   SP_SpinLockIRQ hotAddLck;

   uint32 refCount;       
   Bool destroyMemSpace;   // is the memspace being destroyed?
} BuddyMemSpace;

typedef struct Buddy {
   List_Links buddyHeader;
   SP_SpinLock lck;
   Bool lateInitDone;
   Proc_Entry procDir;  // procfs node /proc/vmware/buddy
} Buddy;

static Buddy buddy;


// Function prototypes 
static void BuddyAddProcNode(BuddyMemSpace *memSpace);
static uint32 BuddyMemCalculate(Buddy_DynamicRangeInfo *dynRange, 
                                Bool dynamic);
static VMK_ReturnStatus BuddyCreateInt(Buddy_DynamicRangeInfo *dynRange,
                                       uint32 memSize, char *mem, 
                                       uint32 numRanges,
                                       Buddy_AddrRange *addrRange, 
                                       Bool dynamic,
                                       Buddy_Handle *outHandle);
static char *BuddyInitBufInfo(BuddyMemSpace *memSpace, const uint32 memSize,
                              const char *inMem, char *mem, 
                              Buddy_StaticRangeInfo *rangeInfo, 
                              uint32 numRanges, Buddy_AddrRange *addrRange, 
                              uint32 blockSize, uint32 numBlocks, 
                              uint32 numBuffers);
static char *BuddyAssignBlockElements(BuddyMemSpace *memSpace,
                                      char *mem,
                                      uint32 memSize, 
                                      uint32 startBuf);
static VMK_ReturnStatus BuddyCheckBufMemory(BuddyMemSpace *memSpace,
                                            uint32 startBuf,
                                            uint32 numBuffers);
static void BuddyCarveBuffers(BuddyMemSpace *memSpace, uint32 numRanges,
                              Buddy_AddrRange *addrRange);
static void BuddyInitStats(BuddyBufStatistics *stats, uint32 numColors);
static VMK_ReturnStatus BuddyAllocateInt(BuddyMemSpace *memSpace, 
                                         uint32 origSize,
                                         uint32 reqSizeShift,
                                         uint32 reqColor, 
                                         World_ID debugWorldID, 
                                         void *debugRA,
                                         uint32 *loc);
static BufNum BuddyGetFreeBuf(BuddyMemSpace *memSpace, uint32 reqSizeShift,
                              uint32 reqColor, uint32 *freeBufSizeShift);
static BufNum BuddySplitBuf(BuddyBufInfo *info, BufNum buf, 
                            uint32 *bufSizeShift,
                            uint32 reqSizeShift, uint32 reqColor);
static void BuddyBufFreeInt(BuddyMemSpace *memSpace, BufNum buf,
                            uint32 sizeShift);
static Bool BuddyCoalesce(BuddyMemSpace *memSpace, uint32 *buf, uint32 *sizeShift);
static int BuddyProcRead(Proc_Entry *entry, char *buffer, int *len);
static int BuddyProcWrite(Proc_Entry *entry, char *buffer, int *len);
static int BuddyProcReadVerbose(Proc_Entry *entry, char *buffer, int *len);
static void BuddyLogStats(BuddyMemSpace *memSpace);
static Bool BuddyAlignDynamicRange(Buddy_DynamicRangeInfo *dynRange,
                                   uint32 *newStart, uint32 *newLen,
                                   uint32 *blockSize, uint32 *numBlocks,
                                   uint32 *numBuffers) ;
static Bool BuddyAlignStaticRange(Buddy_StaticRangeInfo *rangeInfo,
                                  uint32 *newStart, uint32 *newLen,
                                  uint32 *blockSize, uint32 *numBlocks,
                                  uint32 *numBuffers);
static uint32 BuddyReduceFragmentation(BuddyMemSpace *memSpace, BufNum buf, 
                                       uint32 bufSizeShift, uint32 size);
static BuddySizeType BuddySetSize(BuddyMemSpace *memSpace, BufNum buf, 
                                  uint32 len);
static uint32 BuddyGetSize(BuddyMemSpace *memSpace, BufNum buf,
                           BuddySizeType *sizeType);
static void BuddyAddBuffer(BuddyMemSpace *memSpace, BufNum buf, uint32 sizeShift);


/*
 *---------------------------------------------------------------
 *
 * Buddy_Init --
 * 
 *    Initialize the buddy allocator
 *
 * Results:
 *    none.
 *
 * Side Effects:
 *    none.
 *
 *---------------------------------------------------------------
 */
void
Buddy_Init(void)
{
   SP_InitLock("buddy", &buddy.lck, SP_RANK_LEAF);
   buddy.lateInitDone = FALSE;
   List_Init(&buddy.buddyHeader);
   ASSERT(offsetof(Buddy_DynamicRangeInfo, rangeInfo) == 0);
   ASSERT(offsetof(BuddyMemSpace, links) == 0);
}


/*
 *---------------------------------------------------------------
 *
 * Buddy_LateInit --
 * 
 *    Initialize the proc nodes.
 *
 * Results:
 *    none.
 *
 * Side Effects:
 *    Initializes the proc node.
 *
 *---------------------------------------------------------------
 */
void
Buddy_LateInit(void)
{
   BuddyMemSpace *cur;

   SP_Lock(&buddy.lck);
   Proc_InitEntry(&buddy.procDir);
   Proc_Register(&buddy.procDir, "buddy", TRUE);

   // Add proc nodes for all modules that created their
   // memspace before late init
   LIST_FORALL(&buddy.buddyHeader, (List_Links *)cur) {
      BuddyAddProcNode(cur);
   }
   buddy.lateInitDone = TRUE;
   SP_Unlock(&buddy.lck);
}


/*
 *---------------------------------------------------------------
 *
 * BuddyAddProcNode --
 * 
 *    Add a proc node for the given memSpace.
 *
 * Results:
 *    none.
 *
 * Side Effects:
 *    Initializes the proc node.
 *
 *---------------------------------------------------------------
 */
#define BUDDY_MAX_VERBOSE_NAME (BUDDY_MAX_MEMSPACE_NAME + 8)
static void
BuddyAddProcNode(BuddyMemSpace *memSpace)
{
   BuddyBufStatistics *stats = &memSpace->bufInfo.stats; 
   char verboseName[BUDDY_MAX_VERBOSE_NAME];

   Proc_InitEntry(&stats->procStats);
   stats->procStats.read = BuddyProcRead;
   stats->procStats.write = BuddyProcWrite;
   stats->procStats.parent = &buddy.procDir;
   stats->procStats.canBlock = TRUE;
   stats->procStats.private = (void *)memSpace;
   Proc_Register(&stats->procStats, memSpace->name, FALSE);

   Proc_InitEntry(&stats->procStatsVerbose);
   stats->procStatsVerbose.read = BuddyProcReadVerbose;
   stats->procStatsVerbose.parent = &buddy.procDir;
   stats->procStatsVerbose.canBlock = TRUE;
   stats->procStatsVerbose.private = (void *)memSpace;
   snprintf(verboseName, BUDDY_MAX_VERBOSE_NAME, "%s-verbose", 
            memSpace->name);
   Proc_Register(&stats->procStatsVerbose, verboseName, FALSE);
}

/*
 *---------------------------------------------------------------
 *
 * BuddyStatsAddCycles --
 * 
 *    Utility function to add the given cycles to the running stats
 *    and if required update the history stats
 *
 * Results:
 *    none.
 *
 * Side Effects:
 *    none.
 *
 *---------------------------------------------------------------
 */
static INLINE void
BuddyStatsAddCycles(TSCCycles startTSC,
                    TSCCycles endTSC,
                    TSCCycles *runningCycles,
                    uint64 *runningSamples,
                    TSCCycles *histCycles,
                    uint64 *histSamples) 
{
   TSCCycles cycles;
   cycles = *runningCycles;
   cycles += endTSC - startTSC;
   // check for overflow
   if (UNLIKELY(cycles <= *runningCycles)) {
      *histCycles = *runningCycles;
      *histSamples = *runningSamples;
      *runningSamples = 0;
      cycles = endTSC - startTSC;
   }
   *runningCycles = cycles;
   (*runningSamples)++;
}

/*
 *---------------------------------------------------------------
 *
 * BuddyValidateMemSpace --
 * 
 *    Check that the magic number is as expected.
 *
 * Results:
 *    TRUE if the mem space is valid 
 *    FALSE otherwise.
 *
 * Side Effects:
 *    none.
 *
 *---------------------------------------------------------------
 */
static INLINE Bool
BuddyValidateMemSpace(BuddyMemSpace *memSpace) 
{
   ASSERT(memSpace->magicNumber == ((uint32)memSpace & BUDDY_MAGIC_NUMBER));
   if (UNLIKELY(memSpace->magicNumber != 
                ((uint32)memSpace & BUDDY_MAGIC_NUMBER))) {
      Warning("Hanlde 0x%x is invalid", (uint32)memSpace);
      return FALSE;
   }
   return TRUE;
}


/*
 *---------------------------------------------------------------
 *
 * BuddyIncMemSpaceRefCount --
 * 
 *    Increment the ref count on this memspace
 *    If 'inIRQL' is not NULL this function will *not* release
 *    'memSpace->lck' and will let the caller release it.
 *
 * Results:
 *    TRUE if successful, FALSE otherwise.
 *
 * Side Effects:
 *    none.
 *
 *---------------------------------------------------------------
 */
static INLINE Bool
BuddyIncMemSpaceRefCount(BuddyMemSpace *memSpace,
                         SP_IRQL *inIRQL) 
{
   SP_IRQL localIRQL;
   SP_IRQL *prevIRQL = ((inIRQL == NULL) ? &localIRQL : inIRQL);

   *prevIRQL = SP_LockIRQ(&memSpace->lck, SP_IRQL_KERNEL);
   if (memSpace->destroyMemSpace) {
      LOG(2, "(%s): failed because memspace is being destroyed",
          memSpace->name); 
      SP_UnlockIRQ(&memSpace->lck, *prevIRQL);
      return FALSE;
   }

   ASSERT(memSpace->refCount < BUDDY_MAX_REF_COUNT);
   // increment ref count
   memSpace->refCount++;

   // Dont release lock since caller requested to grab the lock
   if (!inIRQL) {
      SP_UnlockIRQ(&memSpace->lck, *prevIRQL);
   }
   return TRUE;
}


/*
 *---------------------------------------------------------------
 *
 * BuddyDecMemSpaceRefCount --
 * 
 *    Decrement the ref count on this memspace
 *
 * Results:
 *    none.
 *
 * Side Effects:
 *    none.
 *
 *---------------------------------------------------------------
 */
static INLINE void
BuddyDecMemSpaceRefCount(BuddyMemSpace *memSpace,
                         SP_IRQL *inIRQL)
{
   SP_IRQL localIRQL;
   SP_IRQL *prevIRQL = ((inIRQL == NULL) ? &localIRQL : inIRQL);

   // decrement ref count
   if (!inIRQL) {
      *prevIRQL = SP_LockIRQ(&memSpace->lck, SP_IRQL_KERNEL);
   }
   ASSERT(memSpace->refCount > 0);
   memSpace->refCount--;
   SP_UnlockIRQ(&memSpace->lck, *prevIRQL);
}


/*
 *---------------------------------------------------------------
 *
 * BuddyGetNumMinBufs --
 * 
 *    Utility function to get the number of minimum sized buffers
 *    in a given size.
 *
 * Results:
 *    The number of minimum sized buffers in the given size
 *
 * Side Effects:
 *    none.
 *  
 *---------------------------------------------------------------
 */
static INLINE uint32
BuddyGetNumMinBufs(BuddyBufInfo *info,
                   uint32 sizeShift)
{
   uint32 numBufs;
   ASSERT(sizeShift >= info->minBufSizeShift);
   ASSERT(sizeShift <= info->maxBufSizeShift);
   /*
    * calculate the number of min size buffers 
    * in the given size
    */
   numBufs = 1 << (sizeShift - info->minBufSizeShift);
   return numBufs;
}


/*
 *---------------------------------------------------------------
 *
 * BuddySize2Shift --
 * 
 *    Utility function that converts the given size to the
 *    corresponding size shifts
 *
 *    NOTE: this function expects the size to be a power of 2
 *
 * Results:
 *    returns the number of shifts corresponding to this size.
 *
 * Side Effects:
 *    none.
 *
 *---------------------------------------------------------------
 */
static INLINE uint32
BuddySize2Shift(uint32 size)
{
   uint32 i = 0;
   ASSERT(Util_IsPowerOf2(size));
   for (i = 0; i < 32; i++) {
      if ((1 << i) == size) {
         return i;
      }
   }
   ASSERT_NOT_IMPLEMENTED(0);
   return BUDDY_INVALID_SIZE_SHIFT;
}


/*
 *---------------------------------------------------------------
 *
 * BuddyGetNumColors --
 * 
 *    Utility function to find out the number of colors.
 *
 * Results:
 *    returns the number of colors
 *
 * Side Effects:
 *    none.
 *
 *---------------------------------------------------------------
 */
static INLINE uint32
BuddyGetNumColors(uint32 numColorBits,
                  uint32 sizeShift)
{
   uint32 numColors;
   if (numColorBits == BUDDY_NO_COLORS) {
      return 1;
   }
   if (numColorBits > sizeShift) {
      numColors = 1 << (numColorBits - sizeShift);
   } else {
      numColors = 1;
   }
   return numColors;
}


/*
 *---------------------------------------------------------------
 *
 * BuddySize2ListIndex --
 * 
 *    Utility function to get the index of the freeList 
 * for the given size.
 *
 * Results:
 *    returns the listIndex.
 *
 * Side Effects:
 *    none.
 *
 *---------------------------------------------------------------
 */
static INLINE uint32 
BuddySize2ListIndex(BuddyBufInfo *info,
                    uint32 sizeShift)
{
   uint32 listNdx = (sizeShift - info->minBufSizeShift);
   ASSERT(sizeShift <= info->maxBufSizeShift);
   ASSERT(sizeShift >= info->minBufSizeShift);
   return listNdx;
}


/*
 *---------------------------------------------------------------
 *
 * BuddyBufNum2BlockStatusNdx --
 * 
 *    Utility function that converts a buffer number into the
 *    'block' and the 'ndx' which contains its BuddyBufStatus
 *
 * Results:
 *    none.
 *
 * Side Effects:
 *    none.
 *
 *---------------------------------------------------------------
 */
static INLINE void
BuddyBufNum2BlockStatusNdx(BuddyBufInfo *info, 
                           BufNum buf, 
                           uint32 *block, 
                           uint32 *ndx)
{
   DEBUG_ONLY(uint32 numBlockBuffers = 
                     info->blockSize >> info->minBufSizeShift);
   ASSERT(buf >= info->startBuf);

   buf -= info->startBuf;
   *block = buf >> info->blockNumSizeShift;
   if (ndx) {
      *ndx = buf & info->blockNdxMask;
      ASSERT(*ndx < numBlockBuffers);
   }
   ASSERT(*block < info->numBlocks);
   ASSERT(info->bufBlocks);
}


/*
 *---------------------------------------------------------------
 *
 * BuddyBufNum2Status --
 * 
 *    Utility function to map the status for the given
 *    buffer.
 *
 * Results:
 *    On success, returns the pointer to BufStatus of this buffer.
 *    On failure, returns NULL.
 *
 * Side Effects:
 *    none.
 *  
 *---------------------------------------------------------------
 */
static INLINE BuddyBufStatus *
BuddyBufNum2Status(BuddyBufInfo *info,
                   BufNum buf)
{
   uint32 block, ndx;
   ASSERT(buf >= info->startBuf);
   ASSERT(buf < info->endBuf);
   
   BuddyBufNum2BlockStatusNdx(info, buf, &block, &ndx);
   ASSERT(info->bufBlocks);
   if (info->bufBlocks[block].bufStatus == NULL) {
      return NULL;
   } else {
      return &info->bufBlocks[block].bufStatus[ndx];
   }
}


/*
 *---------------------------------------------------------------
 *
 * BuddyBufNum2ListNode --
 * 
 *    Utility function that converts a buffer number into the
 *    'block' and the 'ndx' which contains its BuddyListNode
 *
 * Results:
 *    none.
 *
 * Side Effects:
 *    none.
 *
 *---------------------------------------------------------------
 */
static INLINE BuddyListNode *
BuddyBufNum2ListNode(BuddyBufInfo *info, 
                     BufNum buf)
{
   uint32 block, ndx;
   BuddyBufNum2BlockStatusNdx(info, buf, &block, &ndx);
   ndx /= 2;
   return  &info->bufBlocks[block].listNodes[ndx];
}


/*
 *---------------------------------------------------------------
 *
 * BuddyAlignStartAndEnd --
 * 
 *    Utility function that aligns the start and end to the
 *    alignSize.
 *
 *    NOTE: this function aligns the start by doing a ROUNDDOWN
 *    and the end by doing a ROUNDUP
 *
 * Results:
 *    returns the listIndex.
 *
 * Side Effects:
 *    none.
 *
 *---------------------------------------------------------------
 */
static INLINE void
BuddyAlignStartAndEnd(uint32 start,
                      uint32 len,
                      uint32 alignSize,
                      uint32 *newStart,
                      uint32 *newLen)
{
   uint32 newEnd;
   *newStart = ROUNDDOWN(start, alignSize);
   newEnd = ROUNDUP((start + len), alignSize);
   ASSERT(newEnd >= *newStart);
   *newLen = newEnd - *newStart;
}


/*
 *---------------------------------------------------------------
 *
 * BuddyBufNum2Loc --
 * 
 *    Utility function that converts the buffer number to 
 *    its corresponding location in the memory space.
 *
 * Results:
 *    location corresponding to this buffer number.
 *
 * Side Effects:
 *    none.
 *  
 *---------------------------------------------------------------
 */
static INLINE uint32
BuddyBufNum2Loc(BuddyMemSpace *memSpace,
                BufNum buf)
{
   BuddyBufInfo *info = &memSpace->bufInfo;
   ASSERT(buf >= info->startBuf);
   ASSERT(buf < info->endBuf);
   return buf << info->minBufSizeShift;
}


/*
 *---------------------------------------------------------------
 *
 * BuddyIsColored --
 *
 *    Utility function for finding if this memspace
 *    uses colored buffers
 *
 * Results:
 *    returns TRUE if this memspace has colored buffers,
 *    FALSE otherwise. 
 *
 * Side Effects:
 *    none.
 *  
 *---------------------------------------------------------------
 */
static INLINE Bool
BuddyIsColored(BuddyBufInfo *info)
{
   return (info->minBufSizeShift < info->numColorBits);
}
                     

/*
 *---------------------------------------------------------------
 *
 * BuddyBufNum2Color --
 *
 *    Utility function for finding the color of a buffer
 *    given its size.
 *
 * Results:
 *    returns color for 'newSizeShift'
 *
 * Side Effects:
 *    none.
 *  
 *---------------------------------------------------------------
 */
static INLINE uint32
BuddyBufNum2Color(BuddyBufInfo *info,
                  BufNum buf,
                  uint32 sizeShift)
{
   uint32 diffShift = sizeShift - info->minBufSizeShift;
   uint32 color = buf;
   if (info->numColorBits <= sizeShift) {
      return 0;
   }
   ASSERT(sizeShift >= info->minBufSizeShift);
   ASSERT((buf & ((1 << diffShift) - 1)) == 0);
   color >>= diffShift;
   color &= ((1 << (info->numColorBits - sizeShift)) - 1);
   return color;
}


/*
 *---------------------------------------------------------------
 *
 * BuddyAdjustPerColorStats --
 *
 *    Utility function to increment/decrement per color 
 *    buffer stats.
 *
 * Results:
 *    none.
 *
 * Side Effects:
 *    none.
 *  
 *---------------------------------------------------------------
 */
static INLINE void
BuddyAdjustPerColorStats(BuddyMemSpace *memSpace,
                         BufNum buf,
                         uint32 numBufs,
                         Bool increment) 
{
   BuddyBufInfo *info = &memSpace->bufInfo;
   BufNum curBuf;
   BuddyBufStatus *bufStatus;
   uint32 color;

   ASSERT(SP_IsLockedIRQ(&memSpace->lck));
   if (BuddyIsColored(info)) {
      // Adjust stats for each buffer
      BUDDY_FOR_BUFS_DO(info, buf, numBufs, curBuf, bufStatus) {
         color = BuddyBufNum2Color(info, curBuf, info->minBufSizeShift);
         ASSERT(color < info->stats.numColors);
         if (increment) {
            info->stats.colorFreeBuf[color]++;
         } else {
            info->stats.colorFreeBuf[color]--;
         }
      } BUDDY_FOR_BUFS_END;
   } else {
      color = BuddyBufNum2Color(info, buf, info->minBufSizeShift);
      ASSERT(color < info->stats.numColors);
      if (increment) {
         info->stats.colorFreeBuf[color] += numBufs;
      } else {
         info->stats.colorFreeBuf[color] -= numBufs;
      }
   }
}

/*
 *---------------------------------------------------------------
 *
 * BuddyFindLargestBufSize --
 * 
 *    Find the maximum sized buffer that is aligned with the 'startBuf' 
 *    location that will fit in the given 'len'
 *    As a convenience, we also set 
 *       o 'sizeShift' to the number of shifts for the max sized buffer 
 *       o 'numMinBuffers' to the number of minimum sized buffers in
 *         the returned size.
 *    
 * Results:
 *    Size of the largest buffer that will fit in the given length
 *
 * Side Effects:
 *    none.
 *  
 *---------------------------------------------------------------
 */
static INLINE uint32
BuddyFindLargestBufSize(BuddyBufInfo *info,
                        BufNum startBuf,
                        uint32 len,
                        uint32 *sizeShift,
                        uint32 *numMinBuffers)
{
   const uint32 startLoc = startBuf << info->minBufSizeShift;
   uint32 size;
   ASSERT(len > 0);
   if (UNLIKELY(len == 0)) {
      return 0;
   }

   // given length should be a multiple of minBufSize
   ASSERT(ROUNDDOWN(len, info->minBufSize) == len);

   // Find the largest power of 2 buffer to fit in this length
   size = Util_RounddownToPowerOfTwo(len);
   // Find out the buffer size which aligns with the startBuf 
   *sizeShift = ffs(size | startLoc| info->maxBufSize) - 1;
   size = 1 << *sizeShift;
   ASSERT(size <= info->maxBufSize);
   ASSERT(size >= info->minBufSize);
   *numMinBuffers = BuddyGetNumMinBufs(info, *sizeShift);
   return size;
}


/*
 *---------------------------------------------------------------
 *
 * Buddy_AlignRange
 *    
 *    Utility function to call the appropriate alignment function
 *
 * Results:
 *    TRUE if alignment was successful, FALSE otherwise
 *    
 * Side effects:
 *    none.
 *
 *---------------------------------------------------------------
 */
static INLINE Bool
BuddyAlignRange(Buddy_DynamicRangeInfo *dynRange,
                Bool dynamic, 
                uint32 *newStart,
                uint32 *newLen,
                uint32 *blockSize,
                uint32 *numBlocks,
                uint32 *numBuffers) 
{
   if (dynamic) {
      // We align the memory spaces so that they
      // are most convenient for us to manage. This means aligning
      // the start to blocksize and making length a multiple
      // of block size.
      return BuddyAlignDynamicRange(dynRange, newStart, newLen, 
                                    blockSize, numBlocks, numBuffers);
   } else {
      // We align the memory spaces so that they
      // are most convenient for us to manage. This means aligning
      // the start to max buffer size and making the length a multiple
      // of max buffer size.
      return BuddyAlignStaticRange(&dynRange->rangeInfo, newStart, newLen,
                                   blockSize, numBlocks, numBuffers);
   }
}


/*
 *---------------------------------------------------------------
 *
 * BuddyStaticSanityCheck --
 * 
 *    Utility function to check that the
 *    buffer size and length is within supported range for this
 *    static range
 *
 * Results:
 *    returns TRUE on success,
 *            FALSE on failure
 *
 * Side Effects:
 *    none.
 *
 *---------------------------------------------------------------
 */
static INLINE Bool
BuddyStaticSanityCheck(uint32 start,
                       uint32 len,
                       uint32 minBufSizeShift,
                       uint32 maxBufSizeShift)
{
   uint32 maxSize = 1 << maxBufSizeShift;
   uint32 startBuf, endBuf;
   ASSERT(maxBufSizeShift <= BUDDY_MAX_SIZE_SHIFT);
   ASSERT(maxBufSizeShift >= minBufSizeShift);
   if (UNLIKELY((maxBufSizeShift > BUDDY_MAX_SIZE_SHIFT) ||
                (maxBufSizeShift < minBufSizeShift))) {
      Warning("invalid max size shift (0x%x)", maxBufSizeShift);
      return FALSE;
   }

   ASSERT(len > 0);
   // make sure start + len dont overflow
   ASSERT((start + len) > start);
   // Make sure that we do not overflow our len field
   ASSERT((start + len) <= ROUNDDOWN(BUDDY_MAX_LEN, maxSize));
   if (UNLIKELY((start + len) > ROUNDDOWN(BUDDY_MAX_LEN, maxSize))) {
      Warning("Specified len (0x%x) exceeds max supported len", (start + len));
      return FALSE;
   }

   startBuf = start >> minBufSizeShift;
   endBuf = (start + len) >> minBufSizeShift;
   ASSERT(startBuf <= BUDDY_MAX_BUF_NUM);      
   ASSERT(endBuf <= BUDDY_MAX_BUF_NUM);      
   return TRUE;
}


/*
 *---------------------------------------------------------------
 *
 * BuddyDynamicSanityCheck --
 * 
 *    Utility function to check that the
 *    buffer size and length is within supported range for this
 *    dynamic range
 *
 * Results:
 *    returns TRUE on success,
 *            FALSE on failure
 *
 * Side Effects:
 *    none.
 *
 *---------------------------------------------------------------
 */
static INLINE Bool
BuddyDynamicSanityCheck(uint32 start,
                        uint32 initialLen,
                        uint32 minBufSizeShift,
                        uint32 maxBufSizeShift,
                        uint32 finalLen)
{
   uint32 maxSize = 1 << maxBufSizeShift;
   ASSERT(finalLen >= maxSize);
   ASSERT(finalLen >= initialLen);
   if (UNLIKELY((finalLen < maxSize) ||
                (finalLen < initialLen))) {
      Warning("invalid final length (0x%x) specified", finalLen);
      return FALSE;
   }

   return BuddyStaticSanityCheck(start, finalLen, minBufSizeShift,
                                 maxBufSizeShift);
}

/*
 *---------------------------------------------------------------
 *
 * BuddyNumBlocksInMem --
 * 
 *    Utility function to find out the number of blocks that can 
 *    be supported in the given 'memSize'.
 *
 * Results:
 *    returns number of blocks that can be supported in 'memSize'
 *
 * Side Effects:
 *    none.
 *
 *---------------------------------------------------------------
 */
static INLINE uint32
BuddyNumBlocksInMem(BuddyBufInfo *info,
                    const uint32 memSize)
{
   const uint32 minShift = info->minBufSizeShift;
   const uint32 numBlockBuffers = info->blockSize >> minShift;
   uint32 memPerBlock = 0;
   memPerBlock += numBlockBuffers * sizeof(BuddyBufStatus);
   memPerBlock += (numBlockBuffers / 2) * sizeof(BuddyListNode);

   return (memSize/memPerBlock);
}


/*
 *---------------------------------------------------------------
 *
 * BuddyAlignDynamicRange --
 * 
 *    Utility function to align the given hot add range to the
 *    block size in doing so it adjusts the follwing values.
 *    Sets 
 *       'newStart' to the new aligned start
 *       'newLen' to the new length of the range
 *       'blockSize' to the block size for this range
 *       'numBlocks' to the total number of blocks in this range
 *       'numBuffers' to the number of minimum sized buffers
 *                    in this range
 *
 *    It also does sanity checks on the calculated values to 
 *    make sure they dont overflow or anything and sets the
 *    return value accordingly
 *
 *    NOTE:
 *       For dynamic ranges we divide the *entire* address range
 *       into *blocks*. Each block is a multiple of max buffer size.
 *       The size of the blocks is derived by rounding up the
 *       dynRange->minHotAddLenHint to the max buffer size
 *
 * Results:
 *    TRUE on success i.e. none of the values overflow,
 *    FALSE on failure i.e. values overflow
 *
 * Side Effects:
 *    none.
 *
 *---------------------------------------------------------------
 */
static Bool
BuddyAlignDynamicRange(Buddy_DynamicRangeInfo *dynRange,
                       uint32 *newStart,
                       uint32 *newLen,
                       uint32 *blockSize,
                       uint32 *numBlocks,
                       uint32 *numBuffers) 
{
   Buddy_StaticRangeInfo *rangeInfo = &dynRange->rangeInfo;
   DEBUG_ONLY(const uint32 maxSize = rangeInfo->maxSize);
   uint32 finalStart, finalLen;
   const uint32 minShift = BuddySize2Shift(rangeInfo->minSize);
   const uint32 maxShift = BuddySize2Shift(rangeInfo->maxSize);

   ASSERT(rangeInfo->len <= dynRange->maxLen);
   ASSERT(dynRange->minHotAddLenHint >= maxSize);
   /*
    * For hot add we divide the *entire* address range into *blocks* 
    * Each block is multiple of max buffer size.  The size of each 
    * block is derived by rounding up the dynRange->minHotAddLenHint 
    * to the max buffer size. 
    */
   *blockSize = Util_RoundupToPowerOfTwo(dynRange->minHotAddLenHint);
   ASSERT(ROUNDDOWN(*blockSize, maxSize) == *blockSize);

   BuddyAlignStartAndEnd(rangeInfo->start, dynRange->maxLen, *blockSize,
                         &finalStart, &finalLen);
   *numBlocks = finalLen / *blockSize;

   // Align the beginning and the end to block size
   BuddyAlignStartAndEnd(rangeInfo->start, rangeInfo->len, *blockSize, 
                         newStart, newLen);

   // Calculate the number of minimum sized buffers.
   *numBuffers = *newLen / rangeInfo->minSize;

   return BuddyDynamicSanityCheck(*newStart, *newLen, minShift, 
                                  maxShift, finalLen);
}


/*
 *---------------------------------------------------------------
 *
 * BuddyAlignStaticRange --
 * 
 *    Utility function to align the given range to the
 *    max buffer size, in doing so it adjusts the following
 *    values. 
 *    Sets 
 *       'newStart' to the new aligned start
 *       'newLen' to the new length of the range
 *       'blockSize' to the block size for this range
 *       'numBlocks' to the total number of blocks in this range
 *       'numBuffers' to the number of minimum sized buffers
 *                    in this range
 *
 *    It also does sanity checks on the calculated values to 
 *    make sure they dont overflow or anything and sets the
 *    return value accordingly
 *
 * Results:
 *    TRUE on success i.e. none of the values overflow,
 *    FALSE on failure i.e. values overflow
 *
 * Side Effects:
 *    none.
 *
 *---------------------------------------------------------------
 */
static Bool 
BuddyAlignStaticRange(Buddy_StaticRangeInfo *rangeInfo,
                      uint32 *newStart,
                      uint32 *newLen,
                      uint32 *blockSize,
                      uint32 *numBlocks,
                      uint32 *numBuffers)
{
   const uint32 maxSize = rangeInfo->maxSize;
   const uint32 minShift = BuddySize2Shift(rangeInfo->minSize);
   const uint32 maxShift = BuddySize2Shift(rangeInfo->maxSize);

   // Align the beginning and the end to maxbuffer size
   BuddyAlignStartAndEnd(rangeInfo->start, rangeInfo->len, maxSize, 
                         newStart, newLen);
   // Calculate the number of minimum sized buffers.
   *numBuffers = *newLen >> minShift;

   // For static regions, we just use one block whose size
   // is the length of this range
   *numBlocks = 1;
   *blockSize = *newLen;
   return BuddyStaticSanityCheck(*newStart, *newLen, minShift, maxShift);
}


/*
 *---------------------------------------------------------------
 *
 * Buddy_StaticRangeMemReq, Buddy_DynamicRangeMemReq --
 *    
 *    Find out the amount of memory to manage this address range
 *
 * Results:
 *    Size of memory required to manage this range
 *    
 * Side effects:
 *    none.
 *
 *---------------------------------------------------------------
 */
uint32
Buddy_StaticRangeMemReq(Buddy_StaticRangeInfo *rangeInfo)
{
   return BuddyMemCalculate((Buddy_DynamicRangeInfo *)rangeInfo, FALSE);
}

uint32
Buddy_DynamicRangeMemReq(Buddy_DynamicRangeInfo *dynRange) 
{
   return BuddyMemCalculate(dynRange, TRUE);
}


/*
 *---------------------------------------------------------------
 *
 * BuddyMemCalculate --
 * 
 *    Calculate the amount of memory required by the 
 *    buddy allocator to manage the given memspace
 *
 * Results:
 *    On success, the amount of memory required to manage 
 *    this memspace.
 *    0 on failure.
 *
 *
 * Side Effects:
 *    none.
 *
 *---------------------------------------------------------------
 */
static uint32
BuddyMemCalculate(Buddy_DynamicRangeInfo *dynRange,
                  Bool dynamic)
{
   Buddy_StaticRangeInfo *rangeInfo = &dynRange->rangeInfo;
   uint32 memRequired = 0;
   uint32 numBlocks = 0;
   uint32 numBuffers = 0;
   uint32 sizeShift;
   uint32 newStart, newLen, blockSize;
   const uint32 minShift = BuddySize2Shift(rangeInfo->minSize);
   const uint32 maxShift = BuddySize2Shift(rangeInfo->maxSize);
   uint32 numColors;

   // space for BuddyMemSpace
   memRequired += sizeof(BuddyMemSpace);

   // Align the address range so that it
   // is convenient for us to manage.
   if (UNLIKELY(!BuddyAlignRange(dynRange, dynamic, &newStart, &newLen, 
                                 &blockSize, &numBlocks, &numBuffers))) {
      return 0;
   }

   // space for storing all the blocks 
   memRequired += numBlocks * sizeof(BuddyBufBlock);

   // space for storing the buffer status 
   memRequired += numBuffers * sizeof(BuddyBufStatus);

   /*
    * space for storing BuddyListNode for each buffer
    * NOTE: we only need space for numBuffers/2, see comment
    * at the start of this file
    */
   memRequired += (numBuffers/2) * sizeof(BuddyListNode);

   // ok, now deal with colors
   for (sizeShift = minShift; sizeShift <= maxShift; sizeShift++) {
      numColors = BuddyGetNumColors(rangeInfo->numColorBits, sizeShift);
      // space for head and tail
      memRequired += (2 * numColors * sizeof(BufNum));
   }

   // Find number of colors for min sized buffers
   numColors = BuddyGetNumColors(rangeInfo->numColorBits, minShift);
   // space for storing free buffer counts by color and
   /// total free buffers by color
   memRequired += (2 * numColors * sizeof(uint32));

   LOG(2, "(%s): memory required = %d", rangeInfo->name, memRequired);
   return memRequired;
}


/*
 *---------------------------------------------------------------
 *
 * Buddy_CreateStatic, Buddy_CreateDynamic, BuddyCreateInt --
 *
 *       Instructs the buddy allocator to start managing the 
 *    given virtual/physical address space/range. Buddy allocator
 *    refers to the virtual/physical address space/range as
 *    memory space.
 *       Sets 'handle' to the newly created handle for this
 *    memory space
 *
 * Results:
 *    VMK_OK on success.
 *    
 * Side effects:
 *    none.
 *
 *---------------------------------------------------------------
 */
VMK_ReturnStatus
Buddy_CreateStatic(Buddy_StaticRangeInfo *rangeInfo,
                   uint32 memSize,
                   char *mem,
                   uint32 numRanges,
                   Buddy_AddrRange *addrRange,
                   Buddy_Handle *handle)
{
   Buddy_DynamicRangeInfo *dynRange = (Buddy_DynamicRangeInfo *)rangeInfo;
   ASSERT(memSize >= BuddyMemCalculate(dynRange, FALSE));
   return BuddyCreateInt(dynRange, memSize, mem, numRanges, addrRange, 
                         FALSE, handle);
}

VMK_ReturnStatus
Buddy_CreateDynamic(Buddy_DynamicRangeInfo *dynRange,
                    uint32 memSize,
                    char *mem,
                    uint32 numRanges,
                    Buddy_AddrRange *addrRange,
                    Buddy_Handle *handle)
{
   ASSERT(memSize >= BuddyMemCalculate(dynRange, TRUE));
   return BuddyCreateInt(dynRange, memSize, mem, numRanges, addrRange, 
                         TRUE, handle);
}

static VMK_ReturnStatus
BuddyCreateInt(Buddy_DynamicRangeInfo *dynRange,
               uint32 memSize,
               char *mem,
               uint32 numRanges,
               Buddy_AddrRange *addrRange,
               Bool dynamic,
               Buddy_Handle *outHandle)
{
   Buddy_StaticRangeInfo *rangeInfo = &dynRange->rangeInfo;
   BuddyMemSpace *memSpace;
   char *inMem = mem;
   uint32 numBuffers;
   uint32 blockSize, numBlocks;
   
   ASSERT(memSize > 0);
   if (UNLIKELY(memSize == 0)) {
      Warning("(%s): illegal memsize 0", rangeInfo->name);
      return VMK_FAILURE;
   }

   // initialize mem
   memset(mem, 0, memSize);

   *outHandle = memSpace = (BuddyMemSpace *)mem;
   mem += sizeof(BuddyMemSpace);
   strncpy(memSpace->name, rangeInfo->name, BUDDY_MAX_MEMSPACE_NAME);

   // initialize the memspace lock
   SP_InitLockIRQ(rangeInfo->name, &memSpace->lck, SP_RANK_BUDDY_ALLOC);
   SP_InitLockIRQ(rangeInfo->name, &memSpace->hotAddLck, SP_RANK_BUDDY_HOTADD);

   memSpace->refCount = 0;       
   memSpace->destroyMemSpace = FALSE;
   memSpace->type = (dynamic ? BUDDY_DYNAMIC_SPACE : BUDDY_STATIC_SPACE);

   // Align the address range so that it
   // is convenient for us to manage.
   if (UNLIKELY(!BuddyAlignRange(dynRange, dynamic, &memSpace->start,
                                 &memSpace->initialLen, &blockSize, 
                                 &numBlocks, &numBuffers))) {
      return VMK_FAILURE;
   }
   memSpace->maxLen = numBlocks * blockSize;

   ASSERT(numBuffers < BUDDY_MAX_BUF_NUM);

   if (vmx86_debug) {
      DEBUG_ONLY(uint32 maxSize = rangeInfo->maxSize);
      // make sure that start and end is max buffer size aligned
      ASSERT((memSpace->start & (maxSize - 1)) == 0);
      ASSERT(((memSpace->start + memSpace->initialLen) & (maxSize - 1)) == 0);
      ASSERT(((memSpace->start + memSpace->maxLen) & (maxSize - 1)) == 0);
   }

   // initialize buffer info
   mem = BuddyInitBufInfo(memSpace, memSize, inMem, mem, rangeInfo, numRanges, 
                          addrRange, blockSize, numBlocks, numBuffers);
   ASSERT_NOT_IMPLEMENTED(mem <= (inMem + memSize));

   SP_Lock(&buddy.lck);

   // Add this memspace to the list of memspaces being managed
   List_InitElement(&memSpace->links);
   List_Insert(&memSpace->links, LIST_ATFRONT(&buddy.buddyHeader));

   // if late init is done then we add the proc node, else
   // buddy will add it when it does the late init
   if (buddy.lateInitDone) {
      BuddyAddProcNode(memSpace);
   }

   // Assign the magic number last, it indiactes that memspace is initialized
   memSpace->magicNumber = BUDDY_MAGIC_NUMBER & (uint32)memSpace;
   SP_Unlock(&buddy.lck);

   return VMK_OK;
}


/*
 *---------------------------------------------------------------
 *
 * BuddyInitBufInfo --
 * 
 *    Initialize the BuddyBufInfo for this mem space. 
 *
 * Results:
 *    Returns the new 'mem' pointer after it has used the memory it 
 *    needs
 *
 * Side Effects:
 *    Buffers get initialize, backing store is allocated for the
 *    buffer blocks, buffer status, list nodes and colors
 *
 *---------------------------------------------------------------
 */
static char *
BuddyInitBufInfo(BuddyMemSpace *memSpace, 
                 const uint32 memSize,
                 const char *inMem,
                 char *mem, 
                 Buddy_StaticRangeInfo *rangeInfo, 
                 uint32 numRanges,
                 Buddy_AddrRange *addrRange, 
                 uint32 blockSize, 
                 uint32 numBlocks, 
                 uint32 numBuffers)
{
   const uint32 minSizeShift = BuddySize2Shift(rangeInfo->minSize);
   const uint32 maxSizeShift = BuddySize2Shift(rangeInfo->maxSize);
   const uint32 numColorBits = rangeInfo->numColorBits;
   uint32 roundedBlockSize, numBlockBuffers;
   uint32 i;
   uint32 numColors;
   BuddyBufInfo *info = &memSpace->bufInfo;
   SP_IRQL prevIRQL;
   uint32 memRemaining = 0;
   VMK_ReturnStatus status;

   LOG(2, "start = %d, initialLen = %d, minSizeShift = %d, "
       "maxSizeShift = %d, numColorBits = %d, numBuffers = %d"
       "blockSize = %d, numBlocks = %d",
       memSpace->start, memSpace->initialLen, minSizeShift, 
       maxSizeShift, numColorBits, numBuffers,
       blockSize, numBlocks);

   if (numColorBits == BUDDY_NO_COLORS) {
      info->numColorBits = minSizeShift;
   } else {
      info->numColorBits = numColorBits;
   }

   info->numBufSizes = (maxSizeShift - minSizeShift + 1);
   ASSERT(info->numBufSizes <= BUDDY_MAX_NUM_BUFFER_SIZES);

   info->minBufSize = (1 << minSizeShift);
   info->minBufSizeShift = minSizeShift;

   info->maxBufSize = (1 << maxSizeShift);
   info->maxBufSizeShift = maxSizeShift;

   // initialize pointers
   info->bufBlocks = NULL;
   for (i = 0; i < info->numBufSizes; i++) {
      info->freeList[i].head = NULL;
      info->freeList[i].tail = NULL;
   }

   info->startBuf = memSpace->start >> minSizeShift;
   ASSERT(info->startBuf < BUDDY_MAX_BUF_NUM);
   info->endBuf = info->startBuf + numBuffers;
   ASSERT(info->endBuf >= info->startBuf);
   ASSERT(info->endBuf < BUDDY_MAX_BUF_NUM);

   info->numBlocks = numBlocks;
   info->blockSize = blockSize;
   // block size should be multiple of max buffer size
   ASSERT((blockSize & (info->maxBufSize - 1)) == 0);

   // Ok for dynamic ranges the block size is already a power of 2,
   // but for static ranges it is not, so first convert the blockSize
   // to a power of 2 which will just be nop for dynamic ranges.
   roundedBlockSize = Util_RoundupToPowerOfTwo(info->blockSize);
   ASSERT(roundedBlockSize >= info->blockSize);
   ASSERT(Util_IsPowerOf2(roundedBlockSize));
   // num min sized buffers in this roundedBlockSize
   numBlockBuffers = roundedBlockSize >> info->minBufSizeShift;
   // numBlockBuffers should also be a power of 2 by design
   ASSERT(Util_IsPowerOf2(numBlockBuffers));
   info->blockNumSizeShift = BuddySize2Shift(numBlockBuffers);
   info->blockNdxMask = numBlockBuffers - 1;
   if (vmx86_debug) {
      if (memSpace->type == BUDDY_DYNAMIC_SPACE) {
         ASSERT(Util_IsPowerOf2(info->blockSize));
         ASSERT(roundedBlockSize == info->blockSize);
      }
   }

   // Assign memory for blocks 
   info->bufBlocks = (BuddyBufBlock *)mem;
   mem += numBlocks * sizeof(BuddyBufBlock);
   for (i = 0; i < numBlocks; i++) {
      info->bufBlocks[i].bufStatus = NULL;
      info->bufBlocks[i].listNodes = NULL;
   }


   // Assign memory for the colors
   for (i = 0; i < info->numBufSizes; i++) {
      uint32 j;
      numColors = BuddyGetNumColors(numColorBits, minSizeShift + i);
      const uint32 colorLen = numColors * sizeof(BufNum);
      BuddyFreeList *freeList = &info->freeList[i];

      ASSERT(numColors > 0);
      freeList->numColors = numColors;

      freeList->head = (BufNum *)mem;
      mem += colorLen;
      freeList->tail = (BufNum *)mem;
      mem += colorLen;

      for (j = 0; j < freeList->numColors; j++) {
         freeList->head[j] = BUDDY_TAIL_BUF_NUM;
         freeList->tail[j] = BUDDY_HEAD_BUF_NUM;
      }
   }

   // Assign memory for free buf stats by color and total buf stats by color
   numColors = BuddyGetNumColors(numColorBits, minSizeShift);
   info->stats.colorFreeBuf = (uint32 *)mem;
   mem += numColors * sizeof(uint32);
   info->stats.colorTotBuf = (uint32 *)mem;
   mem += numColors * sizeof(uint32);

   // Acquire lock just to keep BuddyAssignBlockElements happy, 
   // Otherwise at init we dont expect any races
   prevIRQL = SP_LockIRQ(&memSpace->hotAddLck, SP_IRQL_KERNEL);

   // Assign memory for block elements i.e. buffer status and list nodes
   // NOTE: This function uses up all the remaining memory
   memRemaining = memSize - ((VA)mem - (VA)inMem);
   mem = BuddyAssignBlockElements(memSpace, mem, memRemaining, info->startBuf);
   ASSERT_NOT_IMPLEMENTED(mem <= (inMem + memSize));

   // Confirm that memory is allocated for the buffers.
   status = BuddyCheckBufMemory(memSpace, info->startBuf, numBuffers);
   ASSERT_NOT_IMPLEMENTED(status == VMK_OK);

   SP_UnlockIRQ(&memSpace->hotAddLck, prevIRQL);

   // initialize the statistics
   BuddyInitStats(&info->stats, numColors);

   // Acquire lock just to keep BuddyCarveBuffers happy, 
   // Otherwise at init we dont expect any races
   prevIRQL = SP_LockIRQ(&memSpace->hotAddLck, SP_IRQL_KERNEL);
   // Carve out the free buffers from the given range
   BuddyCarveBuffers(memSpace, numRanges, addrRange);
   SP_UnlockIRQ(&memSpace->hotAddLck, prevIRQL);

   return mem;
}


/*
 *---------------------------------------------------------------
 *
 * BuddyInitStats --
 * 
 *    Initialize the buffer stats.
 *
 * Results:
 *    none.
 *
 * Side Effects:
 *    none.
 *
 *---------------------------------------------------------------
 */
static void
BuddyInitStats(BuddyBufStatistics *stats,
               uint32 numColors)
{
   uint32 i;

   stats->numCarvedBuf = 0;
   stats->numFreeCarvedBuf = 0;
   for (i = 0; i < BUDDY_MAX_NUM_BUFFER_SIZES; i++) {
      stats->numFreeBuf[i] = 0;
      stats->numUsedBuf[i] = 0;
   }
   stats->numColors = numColors;
   for (i = 0; i < numColors; i++) {
      stats->colorFreeBuf[i] = 0;
      stats->colorTotBuf[i] = 0;
   }
   LOG(2, "stats initialized");
}


/*
 *---------------------------------------------------------------
 *
 * BuddyAssignBlockElements --
 * 
 *    Assign memory to the buffer status and list nodes of the
 *    buffers belonging to this block
 *       Caller must acquire memSpace->hotAddlck.
 *    NOTE: This function will try to use up all the 
 *          given memory for block elements
 *
 * Results:
 *    returns the value of 'mem' pointer after the amount of 
 *    memory required for these buffer statuses and list nodes
 *    is used up
 *
 * Side Effects:
 *    Initializes the buffer status and list nodes
 *
 *---------------------------------------------------------------
 */
static char *
BuddyAssignBlockElements(BuddyMemSpace *memSpace,
                         char *mem,
                         uint32 memSize, 
                         uint32 startBuf)
{
   BuddyBufInfo *info = &memSpace->bufInfo;
   const uint32 minShift = info->minBufSizeShift;
   const uint32 numBlockBuffers = info->blockSize >> minShift;
   uint32 startBlock, block;
   uint32 numBlocksInMem = BuddyNumBlocksInMem(info, memSize);

   ASSERT(SP_IsLockedIRQ(&memSpace->hotAddLck));

   BuddyBufNum2BlockStatusNdx(info, startBuf, &startBlock, NULL);
   for (block = startBlock; (block < info->numBlocks) && (numBlocksInMem > 0); 
        block++) {
      // We support hot add this combined with the fact that we align the
      // starts and ends of a range to the blockSize means that there
      // may be some blocks which may overlap and hence may have already
      // been assigned and initialized. So we only assign memory
      // for un-assigned blocks
      if (info->bufBlocks[block].bufStatus == NULL) {
         uint32 j;
         ASSERT(mem);
         ASSERT(info->bufBlocks[block].listNodes == NULL);
         info->bufBlocks[block].bufStatus = (BuddyBufStatus *)mem;
         mem += numBlockBuffers * sizeof(BuddyBufStatus);
         info->bufBlocks[block].listNodes = (BuddyListNode *)mem;
         mem += (numBlockBuffers / 2) * sizeof(BuddyListNode);
         // initialize the bufStatus and listNodes
         for (j = 0; j < numBlockBuffers; j++) {
            BuddyBufStatus *bufStatus = &info->bufBlocks[block].bufStatus[j];
            bufStatus->state = BUDDY_BUF_RESERVED;
            bufStatus->sizeShift = BUDDY_INVALID_SIZE_SHIFT;
            if (j < numBlockBuffers/2) {
               BuddyListNode *listNode = &info->bufBlocks[block].listNodes[j];
               listNode->prev = BUDDY_INVALID_BUF_NUM;
               listNode->next = BUDDY_INVALID_BUF_NUM;
            }
         }
         numBlocksInMem--;
      }
   }
   return mem;
}


/*
 *---------------------------------------------------------------
 *
 * BuddyCheckBufMemory --
 * 
 *    Go through the number of buffers starting at 'startBuf'
 *    and find out if they have backing store allocated to store
 *    their buffer status and list nodes.
 *       Caller must acquire memSpace->hotAddlck.
 *
 * Results:
 *    VMK_OK if all buffers have backing store,
 *    error code on failure
 *
 * Side Effects:
 *    None.
 *
 *---------------------------------------------------------------
 */
static VMK_ReturnStatus
BuddyCheckBufMemory(BuddyMemSpace *memSpace,
                    uint32 startBuf,
                    uint32 numBuffers)
{
   BuddyBufInfo *info = &memSpace->bufInfo;
   uint32 i;

   LOG(0, "Checking buffers to find backing store");
   ASSERT(SP_IsLockedIRQ(&memSpace->hotAddLck));

   for (i = 0; i < numBuffers; i++) {
      uint32 block;
      BuddyBufNum2BlockStatusNdx(info, startBuf + i, &block, NULL);
      ASSERT(block < info->numBlocks);
      if (block >= info->numBlocks) {
         return VMK_FAILURE;
      }
      if (info->bufBlocks[block].bufStatus == NULL) {
         return VMK_NO_MEMORY;
      }
      ASSERT(info->bufBlocks[block].listNodes);
   }
   return VMK_OK;
}


/*
 *---------------------------------------------------------------
 *
 * BuddyCarveBuffers --
 * 
 *    Carve out free buffers from the given address range. Only
 *    carve out buffers that are currently marked as reserved. This
 *    handles cases where 'hot add' may try to add overlapping
 *    regions.
 *       Caller must acquire memSpace->hotAddlck.
 *
 * Results:
 *    none.
 *
 * Side Effects:
 *    free buffers are added to the appropriate free lists
 *
 *---------------------------------------------------------------
 */
static void
BuddyCarveBuffers(BuddyMemSpace *memSpace, 
                  uint32 numRanges,
                  Buddy_AddrRange *addrRange) 
{
   BuddyBufInfo *info = &memSpace->bufInfo;
   uint32 i;

   ASSERT(SP_IsLockedIRQ(&memSpace->hotAddLck));
   for (i = 0; i < numRanges; i++) {
      uint32 start, end, len, shift, minBufs;
      BufNum startBuf, buf, endBuf;
      // align the range to be on buffer boundaries
      start = ROUNDUP(addrRange[i].start, info->minBufSize);
      end = ROUNDDOWN((addrRange[i].start + addrRange[i].len), info->minBufSize);
      if (start >= end) {
         continue;
      }
      len = end - start;
      startBuf = start >> info->minBufSizeShift;
      endBuf = end >> info->minBufSizeShift;
      ASSERT(startBuf >= info->startBuf);
      if (UNLIKELY(startBuf < info->startBuf)) {
         Warning("(%s): start buffer = 0x%x, "
                 "trying to add buffer 0x%x, skipping",
                 memSpace->name, info->startBuf, startBuf);
         continue;
      }
      ASSERT(endBuf <= info->endBuf);
      if (UNLIKELY(endBuf > info->endBuf)) {
         Warning("(%s): end buffer = 0x%x, "
                 "trying to add buffer 0x%x, skipping",
                 memSpace->name, info->endBuf, endBuf);
         continue;
      }

      // Add the buffers in the current addr range to the free list
      BUDDY_FOR_BUFS_IN_LEN_DO(info, startBuf, len, buf, shift, minBufs) {
         Bool addOne = FALSE;
         BufNum curBuf;
         BuddyBufStatus *bufStatus;

         BUDDY_FOR_BUFS_DO(info, buf, minBufs, curBuf, bufStatus) {
            // If even one buffer in this range is already in use
            // or is free then revert to adding one min sized
            // buffer at a time
            if (bufStatus->state != BUDDY_BUF_RESERVED) {
               addOne = TRUE;
               break;
            }
         }BUDDY_FOR_BUFS_END;
         
         if (!addOne) {
            BuddyAddBuffer(memSpace, buf, shift);
         } else {
            BUDDY_FOR_BUFS_DO(info, buf, minBufs, curBuf, bufStatus) {
               if (bufStatus->state != BUDDY_BUF_RESERVED) {
                  continue;
               }
               BuddyAddBuffer(memSpace, curBuf, info->minBufSizeShift);
            }BUDDY_FOR_BUFS_END;
         }
      }BUDDY_FOR_BUFS_IN_LEN_END;
   }
}

/*
 *---------------------------------------------------------------
 *
 * BuddyAddBuffer --
 * 
 *    Add the given buffer to the list of free buffers.
 *
 * Results:
 *    none.
 *
 * Side Effects:
 *    none.
 *
 *---------------------------------------------------------------
 */
static void
BuddyAddBuffer(BuddyMemSpace *memSpace,
               BufNum buf,
               uint32 sizeShift)
{
   BuddyBufInfo *info = &memSpace->bufInfo;
   BuddyBufStatus *bufStatus;
   uint32 numBufs = BuddyGetNumMinBufs(info, sizeShift);
   SP_IRQL prevIRQL;
   uint32 color;
   BufNum curBuf;

   prevIRQL = SP_LockIRQ(&memSpace->lck, SP_IRQL_KERNEL);
   BUDDY_FOR_BUFS_DO(info, buf, numBufs, curBuf, bufStatus) {
      ASSERT(bufStatus->state == BUDDY_BUF_RESERVED);
      ASSERT(bufStatus->sizeShift = BUDDY_INVALID_SIZE_SHIFT);
      if (BUDDY_AID_DEBUGGING) {
         bufStatus->state = BUDDY_BUF_INUSE;
      } else {
         bufStatus->state = BUDDY_BUF_FREE;
      }
      // increment per color stats
      color = BuddyBufNum2Color(info, curBuf, info->minBufSizeShift);
      ASSERT(color < info->stats.numColors);
      info->stats.colorTotBuf[color]++;
      info->stats.colorFreeBuf[color]++;
   }BUDDY_FOR_BUFS_END;

   bufStatus = BuddyBufNum2Status(info, buf);
   bufStatus->state = BUDDY_BUF_INUSE;
   bufStatus->sizeShift = sizeShift;

   // update stats
   info->stats.numCarvedBuf += numBufs;
   info->stats.numFreeCarvedBuf += numBufs;

   // Add this buffer to the free list
   BuddyBufFreeInt(memSpace, buf, sizeShift);
   SP_UnlockIRQ(&memSpace->lck, prevIRQL);
}

/*
 *---------------------------------------------------------------
 *
 * Buddy_Destroy --
 * 
 *    Destroys the given memspace, essentially marks the handle
 *    as invalid and removes the memspace from the list of memspaces.
 *
 * Results:
 *    none.
 *
 * Side Effects:
 *    This function could *block*, waiting for outstanding requests
 *    on this memspace to finish.
 *
 *---------------------------------------------------------------
 */
void
Buddy_Destroy(Buddy_Handle handle) 
{
   BuddyMemSpace *memSpace = handle;
   BuddyBufStatistics *stats = &memSpace->bufInfo.stats;
   volatile uint32 *refCount = (volatile uint32 *)&memSpace->refCount;
   SP_IRQL prevIRQL;

   if (!BuddyValidateMemSpace(memSpace)) {
      return;
   }

   prevIRQL = SP_LockIRQ(&memSpace->lck, SP_IRQL_KERNEL);
   memSpace->destroyMemSpace = TRUE;
   if (*refCount != 0) {
      while (*refCount != 0) {
         SP_UnlockIRQ(&memSpace->lck, prevIRQL);
         CpuSched_YieldThrottled();
         prevIRQL = SP_LockIRQ(&memSpace->lck, SP_IRQL_KERNEL);
      }
   }
   memSpace->magicNumber = BUDDY_INVALID_MAGIC_NUMBER;
   if (vmx86_debug) {
      BuddyLogStats(memSpace);
   }
   SP_UnlockIRQ(&memSpace->lck, prevIRQL);

   SP_Lock(&buddy.lck);
   Proc_Remove(&stats->procStats);
   Proc_Remove(&stats->procStatsVerbose);
   List_Remove(&memSpace->links);
   SP_CleanupLockIRQ(&memSpace->lck);
   SP_CleanupLockIRQ(&memSpace->hotAddLck);
   SP_Unlock(&buddy.lck);
}


/*
 *---------------------------------------------------------------
 *
 * BuddySize2StatsIndex --
 * 
 *    Utility function to find the stats index corresponding
 *    to the given buffer size
 *
 * Results:
 *    The stats index for the given buffer size.
 *
 * Side Effects:
 *    none.
 *  
 *---------------------------------------------------------------
 */
static INLINE uint32
BuddySize2StatsIndex(BuddyBufInfo *info,
                     uint32 sizeShift)
{
   return BuddySize2ListIndex(info, sizeShift);
}


/*
 *---------------------------------------------------------------
 *
 * BuddyAreBuffersFree --
 * 
 *    Determine if the specified buffers are *all* free.  
 *
 * Results:
 *    TRUE, if all buffers are free.
 *    FALSE otherwise
 *
 * Side Effects:
 *    none.
 *  
 *---------------------------------------------------------------
 */
static INLINE Bool
BuddyAreBuffersFree(BuddyBufInfo *info, 
                    BufNum buf, 
                    uint32 sizeShift)
{
   BufNum curBuf;
   uint32 numBufs = BuddyGetNumMinBufs(info, sizeShift);
   BuddyBufStatus *bufStatus = BuddyBufNum2Status(info, buf);
   ASSERT((buf + numBufs) <= info->endBuf);

   if (bufStatus->state == BUDDY_BUF_FREE &&
       bufStatus->sizeShift == sizeShift) {
      if (BUDDY_AID_DEBUGGING) { 
         // make sure all buffers are free
         BUDDY_FOR_BUFS_DO(info, buf+1, numBufs-1, curBuf, bufStatus) {
            ASSERT(bufStatus->state == BUDDY_BUF_FREE);
            ASSERT(bufStatus->sizeShift == BUDDY_INVALID_SIZE_SHIFT);
         } BUDDY_FOR_BUFS_END;
      }
      return TRUE;
   } else {
      return FALSE;
   }
}


/*
 *---------------------------------------------------------------
 *
 * BuddyLoc2BufNum --
 * 
 *    Utility function that converts the given location to a 
 *    buffer number
 *
 * Results:
 *    buffer number corresponding to this address, 
 *    BUDDY_INVALID_BUF_NUM if the location is invalid.
 *
 * Side Effects:
 *    none.
 *  
 *---------------------------------------------------------------
 */
static INLINE BufNum
BuddyLoc2BufNum(BuddyMemSpace *memSpace,
                uint32 loc)
{
   BuddyBufInfo *info = &memSpace->bufInfo;
   BufNum buf = loc >> info->minBufSizeShift;
   uint32 minSize = 1 << info->minBufSizeShift;

   ASSERT((loc & (minSize - 1)) == 0);
   if (UNLIKELY((loc & (minSize - 1)) != 0)) {
      Warning("Invalid loc 0x%x", loc);
      return BUDDY_INVALID_BUF_NUM;
   }

   ASSERT(buf >= info->startBuf);
   if (UNLIKELY(buf < info->startBuf)) {
      Warning("Invalid loc 0x%x, buf 0x%x , start 0x%x",
              loc, buf, info->startBuf);
      return BUDDY_INVALID_BUF_NUM;
   }

   ASSERT(buf < info->endBuf);
   if (UNLIKELY(buf >= info->endBuf)) {
      Warning("Invalid loc 0x%x, buf 0x%x , endBuf 0x%x",
              loc, buf, info->endBuf);
      return BUDDY_INVALID_BUF_NUM;
   }
   return buf;
}


/*
 *---------------------------------------------------------------
 *
 * BuddyMarkBufferFree --
 * 
 *    Utility function to set the buffer status as free.
 *
 * Results:
 *    none.
 *
 * Side Effects:
 *    none.
 *  
 *---------------------------------------------------------------
 */
static INLINE void
BuddyMarkBufferFree(BuddyBufInfo *info,
                    BufNum buf,
                    uint32 sizeShift)
{
   BuddyBufStatus *bufStatus;
   const uint32 numBufs = BuddyGetNumMinBufs(info, sizeShift);

   ASSERT((buf + numBufs) <= info->endBuf);

   if (BUDDY_AID_DEBUGGING) {
      BufNum curBuf;
      BUDDY_FOR_BUFS_DO(info, buf+1, numBufs-1, curBuf, bufStatus) {
         ASSERT(bufStatus->state == BUDDY_BUF_INUSE);
         ASSERT(bufStatus->sizeShift == BUDDY_INVALID_SIZE_SHIFT);
      } BUDDY_FOR_BUFS_END;
   }

   bufStatus = BuddyBufNum2Status(info, buf);
   ASSERT(bufStatus->state == BUDDY_BUF_INUSE);
   ASSERT(bufStatus->sizeShift == sizeShift);
   bufStatus->state = BUDDY_BUF_FREE;

   if (BUDDY_AID_DEBUGGING) {
      BufNum curBuf;
      BUDDY_FOR_BUFS_DO(info, buf+1, numBufs-1, curBuf, bufStatus) {
         bufStatus->state = BUDDY_BUF_FREE;
      } BUDDY_FOR_BUFS_END;
   }
}


/*
 *---------------------------------------------------------------
 *
 * BuddyMarkBufferInUse --
 * 
 *    Utility function to set the buffer status as in use.
 *
 * Results:
 *    none.
 *
 * Side Effects:
 *    updates sizeShift
 *  
 *---------------------------------------------------------------
 */
static INLINE void
BuddyMarkBufferInUse(BuddyBufInfo *info,
                     BufNum buf,
                     uint32 sizeShift)
{
   BuddyBufStatus *bufStatus;
   const uint32 numBufs = BuddyGetNumMinBufs(info, sizeShift);

   ASSERT((buf + numBufs) <= info->endBuf);

   if (BUDDY_AID_DEBUGGING) {
      BufNum curBuf;
      // check that all buffers are free
      BUDDY_FOR_BUFS_DO(info, buf+1, numBufs-1, curBuf, bufStatus) {
         ASSERT(bufStatus->state == BUDDY_BUF_FREE);
         ASSERT(bufStatus->sizeShift == BUDDY_INVALID_SIZE_SHIFT);
      } BUDDY_FOR_BUFS_END;
   }

   bufStatus = BuddyBufNum2Status(info, buf);
   ASSERT(bufStatus->state == BUDDY_BUF_FREE);
   bufStatus->state = BUDDY_BUF_INUSE;
   bufStatus->sizeShift = sizeShift;

   if (BUDDY_AID_DEBUGGING) {
      BufNum curBuf;
      // mark all buffers as in use
      BUDDY_FOR_BUFS_DO(info, buf+1, numBufs-1, curBuf, bufStatus) {
         bufStatus->state = BUDDY_BUF_INUSE;
      } BUDDY_FOR_BUFS_END;
   }
}


/*
 *---------------------------------------------------------------
 *
 * BuddyMarkBufferSize --
 * 
 *    Utility function to set the buffer size.
 *
 * Results:
 *    none.
 *
 * Side Effects:
 *    none. 
 *  
 *---------------------------------------------------------------
 */
static INLINE void
BuddyMarkBufferSize(BuddyBufInfo *info,
                    BufNum buf,
                    uint32 sizeShift)
{
   BuddyBufStatus *bufStatus = BuddyBufNum2Status(info, buf);
   ASSERT(bufStatus->state == BUDDY_BUF_FREE);
   bufStatus->sizeShift = sizeShift;
}


/*
 *---------------------------------------------------------------
 *
 * BuddyTranslateColor --
 *
 *    Utility function for translating colors between buffer sizes.
 *    'oldColor' is the color corresponding to 'oldSizeShift'. For
 *    a buffer of 'newSizeShift' which is bigger than 'oldSizeShift'
 *    the color will be translated to account for the increase
 *    in the buffer size.
 *
 * Results:
 *    returns color for 'newSizeShift'
 *
 * Side Effects:
 *    none.
 *  
 *---------------------------------------------------------------
 */
static INLINE uint32
BuddyTranslateColor(uint32 oldSizeShift,
                    uint32 oldColor,
                    uint32 newSizeShift)
{
   uint32 diffShift = (newSizeShift - oldSizeShift);
   if (oldColor == BUDDY_NO_COLORS) {
      return oldColor;
   } 
   // Only translate from smaller size to bigger size
   ASSERT(oldSizeShift <= newSizeShift);
   return (oldColor >> diffShift);
}
                     
/*
 *---------------------------------------------------------------
 *
 * BuddyFreeListAdd --
 *
 *    Add the buffer 'buf' to the specified free list.
 *
 * Results:
 *    none
 *
 * Side Effects:
 *    The given buf is added to the list.
 *  
 *---------------------------------------------------------------
 */
static void
BuddyFreeListAdd(BuddyBufInfo *info,
                 BufNum buf,
                 uint32 listNdx,
                 uint32 color)
{
   BuddyListNode *node = BuddyBufNum2ListNode(info, buf);
   BuddyListNode *nextNode;
   BufNum next;
   BuddyFreeList *freeList;
   ASSERT(node->prev == BUDDY_INVALID_BUF_NUM);
   ASSERT(node->next == BUDDY_INVALID_BUF_NUM);

   ASSERT(listNdx < info->numBufSizes);
   freeList = &info->freeList[listNdx];
   
   ASSERT(color < freeList->numColors);
   next = node->next = freeList->head[color];
   node->prev = BUDDY_HEAD_BUF_NUM;
   freeList->head[color] = buf;

   if (next == BUDDY_TAIL_BUF_NUM) {
      ASSERT(freeList->tail[color] == BUDDY_HEAD_BUF_NUM);
      freeList->tail[color] = buf;
   } else {
      nextNode = BuddyBufNum2ListNode(info, next);
      ASSERT(nextNode->prev == BUDDY_HEAD_BUF_NUM);
      nextNode->prev = buf;
   }
}


/*
 *---------------------------------------------------------------
 *
 * BuddyFreeListRemove --
 *
 *    Remove the buffer 'buf' from the free list
 *
 * Results:
 *    none
 *
 * Side Effects:
 *    The prev and next pointers of this buffer are modified
 *  
 *---------------------------------------------------------------
 */
static void
BuddyFreeListRemove(BuddyBufInfo *info, 
                    BufNum buf, 
                    uint32 listNdx,
                    uint32 color)
{
   BuddyFreeList *freeList = &info->freeList[listNdx];
   BuddyListNode *node = BuddyBufNum2ListNode(info, buf); 
   BufNum prev, next;

   ASSERT(listNdx < info->numBufSizes);
   prev = node->prev;
   next = node->next;
   node->prev = BUDDY_INVALID_BUF_NUM;
   node->next = BUDDY_INVALID_BUF_NUM;

   ASSERT(prev != BUDDY_INVALID_BUF_NUM);
   ASSERT(next != BUDDY_INVALID_BUF_NUM);

   // Adjust the next pointer of the previous buffer
   if (prev == BUDDY_HEAD_BUF_NUM) {
      ASSERT(freeList->head[color] == buf);
      freeList->head[color] = next;
   } else {
      BuddyListNode *prevNode = BuddyBufNum2ListNode(info, prev);
      ASSERT(prevNode->next == buf);
      prevNode->next = next;
   }

   // Adjust the previous pointer of the next buffer
   if (next == BUDDY_TAIL_BUF_NUM) {
      ASSERT(freeList->tail[color] == buf);
      freeList->tail[color] = prev;
   } else {
      BuddyListNode *nextNode = BuddyBufNum2ListNode(info, next);
      ASSERT(nextNode->prev == buf);
      nextNode->prev = prev;
   }
}


/*
 *---------------------------------------------------------------
 *
 * BuddyInsertFreeBuf --
 *
 *    Utility function that adds the given buffer to the free list.
 *    Adds the buffer to the appropriate colored list.
 *
 * Results:
 *    none.
 *
 * Side Effects:
 *    Updates statistics
 *  
 *---------------------------------------------------------------
 */
static void INLINE
BuddyInsertFreeBuf(BuddyBufInfo *info,
                   BufNum freeBuf,
                   uint32 sizeShift)
{
   uint32 listNdx = BuddySize2ListIndex(info, sizeShift);
   uint32 color = BuddyBufNum2Color(info, freeBuf, sizeShift);
   uint32 statsNdx = BuddySize2StatsIndex(info, sizeShift);

   ASSERT(sizeShift >= info->minBufSizeShift);
   ASSERT(sizeShift <= info->maxBufSizeShift);
   ASSERT(freeBuf != BUDDY_INVALID_BUF_NUM);

   // sanity checks
   ASSERT(BuddyAreBuffersFree(info, freeBuf, sizeShift));

   // add this buffer to the free list
   BuddyFreeListAdd(info, freeBuf, listNdx, color);

   // Update stats
   info->stats.numFreeBuf[statsNdx]++;
}


/*
 *---------------------------------------------------------------
 *
 * BuddyRemoveFreeBuf --
 *
 *    Utility function that removes the given buffer from the free 
 *    list. Removes the buffer from the appropriate colored list.
 *
 * Results:
 *    none.
 *
 * Side Effects:
 *    Updates statistics
 *  
 *---------------------------------------------------------------
 */
static void INLINE
BuddyRemoveFreeBuf(BuddyBufInfo *info,
                   BufNum freeBuf,
                   uint32 sizeShift)
{
   uint32 listNdx = BuddySize2ListIndex(info, sizeShift);
   uint32 color = BuddyBufNum2Color(info, freeBuf, sizeShift);
   uint32 statsNdx = BuddySize2StatsIndex(info, sizeShift);

   ASSERT(sizeShift >= info->minBufSizeShift);
   ASSERT(sizeShift <= info->maxBufSizeShift);
   ASSERT(freeBuf != BUDDY_INVALID_BUF_NUM);

   // sanity checks
   ASSERT(BuddyAreBuffersFree(info, freeBuf, sizeShift));

   // remove this buffer from the free list
   BuddyFreeListRemove(info, freeBuf, listNdx, color);

   // Update stats
   info->stats.numFreeBuf[statsNdx]--;
}


/*
 *---------------------------------------------------------------
 *
 * BuddyRemoveHead --
 *
 *    Utility function to remove the buffer at the head of 
 *    a given list.
 *
 * Results:
 *    If list is not empty returns the buffer at the head.
 *    If list is empty returns BUDDY_INVALID_BUF_NUM
 *
 * Side Effects:
 *    none.
 *  
 *---------------------------------------------------------------
 */
static INLINE BufNum
BuddyRemoveHead(BuddyBufInfo *info,
                uint32 listNdx,
                uint32 color,
                uint32 sizeShift)
{
   BuddyFreeList *freeList = &info->freeList[listNdx];
   BufNum buf = freeList->head[color];
   ASSERT(color < freeList->numColors);
   if (buf == BUDDY_TAIL_BUF_NUM) {
      return BUDDY_INVALID_BUF_NUM;
   }
   ASSERT(buf != BUDDY_INVALID_BUF_NUM);
   BuddyRemoveFreeBuf(info, buf, sizeShift);
   return buf;
}


/*
 *---------------------------------------------------------------
 *
 * BuddyValidateColor --
 * 
 *    Find if the color is valid for the specified size.
 *
 * Results:
 *    returns TRUE if the color is valid,
 *    FALSE otherwise.
 *
 * Side Effects:
 *    none.
 *  
 *---------------------------------------------------------------
 */
static INLINE Bool
BuddyValidateColor(BuddyBufInfo *info,
                   uint32 sizeShift,
                   uint32 color)
{
   if (color == BUDDY_NO_COLORS) {
      return TRUE;
   } else {
      uint32 listNdx = BuddySize2ListIndex(info, sizeShift);
      BuddyFreeList *freeList = &info->freeList[listNdx];
      return (color < freeList->numColors);
   }
}


/*
 *---------------------------------------------------------------
 *
 * BuddyGetBufSizeShift --
 * 
 *    Utility function to ROUNDUP the given size to
 *    the next buffer size.
 *
 * Results:
 *    On success the size_shift for the buffer with the best fit.
 *    BUDDY_INVALID_BUF_SIZE on failure.
 *
 * Side Effects:
 *    none.
 *  
 *---------------------------------------------------------------
 */
static INLINE uint32
BuddyGetBufSizeShift(BuddyBufInfo *info, 
                     const uint32 size)
{
   uint32 i;
   for (i = 0; i < info->numBufSizes; i++) {
      if (size <= ( 1 << (info->minBufSizeShift + i))) {
         return (info->minBufSizeShift + i);
      }
   }
   LOG(2, "Requested size 0x%x is greater than maxsize 0x%x",
       size, info->maxBufSize);
   return BUDDY_INVALID_BUF_SIZE;
}


/*
 *---------------------------------------------------------------
 *
 * Buddy_AllocateColor, Buddy_Allocate --
 * 
 *    Allocate the requested size of phys/virt memory from the
 *    memory space specified by 'handle'.
 *       Sets 'loc' to the allocated location on success.
 *
 * Results:
 *    VMK_OK on success,
 *    error code on failure
 *
 * Side Effects:
 *    none.
 *  
 *---------------------------------------------------------------
 */
VMK_ReturnStatus 
Buddy_AllocateColor(Buddy_Handle handle, 
                    uint32 size,
                    uint32 color,
                    World_ID debugWorldID,
                    void *debugRA,
                    uint32 *loc)
{
   BuddyMemSpace *memSpace = handle;
   uint32 sizeShift;
   VMK_ReturnStatus status;
   SP_IRQL prevIRQL;
   TSCCycles startTSC, endTSC;
   BuddyBufInfo *info;

   if (!BuddyValidateMemSpace(memSpace)) {
      return VMK_INVALID_HANDLE;
   }

   // increment ref count
   if (!BuddyIncMemSpaceRefCount(memSpace, &prevIRQL)) {
      return VMK_FAILURE;
   }
   // non-preemptible here
   startTSC = RDTSC();

   // Convert the given size into one of the standard buf sizes
   sizeShift = BuddyGetBufSizeShift(&memSpace->bufInfo, size);
   ASSERT(sizeShift >= memSpace->bufInfo.minBufSizeShift);
   ASSERT(sizeShift <= memSpace->bufInfo.maxBufSizeShift);
   ASSERT(sizeShift != BUDDY_INVALID_BUF_SIZE);
   if (UNLIKELY((sizeShift == BUDDY_INVALID_BUF_SIZE) ||
                (size == 0))) {
      Warning("%s: size(%d) is not supported", memSpace->name, size);
      status = VMK_FAILURE;
      goto out;
   }

   // Validate the requested color
   if (UNLIKELY(!BuddyValidateColor(&memSpace->bufInfo, sizeShift, color))) {
      LOG(2, "%s: color(%d) for size(%d) is not valid", 
          memSpace->name, color, size);
      status = VMK_FAILURE;
      goto out;
   }

   // Size and color both have been validated, now allocate the space
   status = BuddyAllocateInt(memSpace, size, sizeShift, color, 
                             debugWorldID, debugRA, loc);

   endTSC = RDTSC();
   info = &memSpace->bufInfo;
   BuddyStatsAddCycles(startTSC, endTSC, &info->stats.allocRunningCycles, 
                       &info->stats.allocRunningSamples, 
                       &info->stats.allocHistCycles, 
                       &info->stats.allocHistSamples);
out:
   // decrement ref count
   BuddyDecMemSpaceRefCount(memSpace, &prevIRQL);
   return status;
}

VMK_ReturnStatus
Buddy_Allocate(Buddy_Handle handle,
               uint32 size,
               World_ID debugWorldID,
               void *debugRA,
               uint32 *loc)   // out
{
   return Buddy_AllocateColor(handle, size, BUDDY_NO_COLORS, 
                              debugWorldID, debugRA, loc);
}


/*
 *---------------------------------------------------------------
 *
 * BuddyAllocateInt --
 * 
 *    Allocate the requested size of phys/virt memory from the
 *    given memory space.
 *
 * Results:
 *    returns the allocated location on success 
 *    otherwise returns NULL
 *
 * Side Effects:
 *    none.
 *  
 *---------------------------------------------------------------
 */
static VMK_ReturnStatus 
BuddyAllocateInt(BuddyMemSpace *memSpace,
                 uint32 origSize,
                 uint32 reqSizeShift,
                 uint32 reqColor,
                 World_ID debugWorldID,
                 void *debugRA,
                 uint32 *loc)
{
   BufNum buf;
   uint32 bufSizeShift;
   BuddyBufStatus *bufStatus;
   BuddyBufInfo *info = &memSpace->bufInfo;
   uint32 actualSize;
   BuddySizeType sizeType;
   uint32 numBufs;

   ASSERT(SP_IsLockedIRQ(&memSpace->lck));

   *loc = BUDDY_INVALID_BUF_NUM;

   // Get a free buffer
   buf = BuddyGetFreeBuf(memSpace, reqSizeShift, reqColor, &bufSizeShift);
   if (UNLIKELY(buf == BUDDY_INVALID_BUF_NUM)) {
      LOG(1, "(%s): Failed to allocate %d bytes with color %d, "
             "debugWorldID 0x%x, debugRA %p", 
             memSpace->name, (1 << reqSizeShift), reqColor, 
             debugWorldID, debugRA);
      if (vmx86_debug) {
         BuddyLogStats(memSpace);
      }
      return VMK_FAILURE;
   }

   // Check if buffers are really free
   ASSERT(BuddyAreBuffersFree(info, buf, bufSizeShift));
   
   // mark the buffer as used, and of the required size
   BuddyMarkBufferInUse(info, buf, bufSizeShift);

   if (vmx86_debug) {
      bufStatus = BuddyBufNum2Status(info, buf);
      DEBUG_ONLY(bufStatus->debugWorldID = debugWorldID);
      DEBUG_ONLY(bufStatus->debugRA = ((VA)debugRA) >> 8);
   }


   // split the buffer if required
   while (reqSizeShift < bufSizeShift) {
      buf = BuddySplitBuf(info, buf, &bufSizeShift, reqSizeShift, reqColor); 
   }

   // Modify the vanilla buddy allocator by adding this extra-optimization
   // layer which tries to free as many fragmented buffers as possible
   actualSize = BuddyReduceFragmentation(memSpace, buf, bufSizeShift, 
                                         origSize);

   // Save the buffer size
   sizeType = BuddySetSize(memSpace, buf, actualSize);

   // Adjust per color free buffer stats, decrement free buf count
   ASSERT(ROUNDDOWN(actualSize, info->minBufSize) == actualSize);
   numBufs = actualSize/info->minBufSize;
   BuddyAdjustPerColorStats(memSpace, buf, numBufs, FALSE);
 
   // update stats
   info->stats.numTypeAllocated[sizeType]++;

   if (vmx86_debug) {
      DEBUG_ONLY(BuddySizeType debugType);
      ASSERT(actualSize == BuddyGetSize(memSpace, buf, &debugType));
      ASSERT(debugType == sizeType);
   }


   ASSERT(reqColor == BUDDY_NO_COLORS || 
          reqColor == BuddyBufNum2Color(info, buf, bufSizeShift));

   *loc = BuddyBufNum2Loc(memSpace, buf);
   return VMK_OK;
}


/*
 *---------------------------------------------------------------
 *
 * Buddy_AllocRange --
 * 
 *    Allocate the free block containing *loc.
 *
 * Results:
 *    On success, *loc contains the starting point of
 *    the allocated block and *size contains the block size.
 *    
 *    On failure, if *loc is outside of the current memspace,
 *    *loc is unchanged, otherwise, *loc contains the next free
 *    block within the current memspace. If there is no
 *    free block in the current memspace, *loc is unchanged.
 *
 * Side Effects:
 *    pages allocated
 *  
 *---------------------------------------------------------------
 */
VMK_ReturnStatus
Buddy_AllocRange(Buddy_Handle handle,           // IN
                 uint32 *loc,                   // IN/OUT
                 uint32 *size)                  // OUT
{
   BuddyMemSpace *memSpace = (BuddyMemSpace *)handle;
   BuddyBufInfo *info = &memSpace->bufInfo;
   BuddyBufStatus *bufStatus;
   BufNum locBuf;
   VMK_ReturnStatus status = VMK_FAILURE;
   TSCCycles startTSC, endTSC;
   SP_IRQL prevIRQL;
   uint32 sizeShift;

   if (!BuddyValidateMemSpace(memSpace)) {
      return VMK_INVALID_HANDLE;
   }

   // increment ref count
   if (!BuddyIncMemSpaceRefCount(memSpace, &prevIRQL)) {
      return VMK_FAILURE;
   }
   // non-preemptible here
   startTSC = RDTSC();

   *size = 0;
   locBuf = *loc >> info->minBufSizeShift;

   if (locBuf < info->startBuf || locBuf >= info->endBuf) {
      goto out;
   } 

   /*
    * Find the biggest free memory range enclosing loc.
    */
   for (sizeShift = info->minBufSizeShift; 
        sizeShift <= info->maxBufSizeShift; sizeShift++) {
      uint32 start = ALIGN_DOWN(*loc, (1 << sizeShift));
      BufNum startBuf = BuddyLoc2BufNum(memSpace, start);

      bufStatus = BuddyBufNum2Status(info, startBuf);

      if (bufStatus == NULL) { // hit a hole, bail out
         break;
      }

      if (bufStatus->state == BUDDY_BUF_FREE) {
         if (bufStatus->sizeShift == sizeShift) {
            // we found a free buffer
            uint32 numBufs = BuddyGetNumMinBufs(info, sizeShift);
            uint32 statsNdx = BuddySize2StatsIndex(info, sizeShift);

            // Remove from free list and mark as used.
            BuddyRemoveFreeBuf(info, startBuf, sizeShift);
            BuddyMarkBufferInUse(info, startBuf, sizeShift);
            // assign return values
            *loc = start;
            *size = 1 << bufStatus->sizeShift;
            status = VMK_OK;

            // stats
            info->stats.numUsedBuf[statsNdx]++;
            info->stats.numFreeCarvedBuf -= numBufs;
            info->stats.numTypeAllocated[BUDDY_SIZE_TYPE_POWEROF2]++;

            // Adjust per color free buffer stats, decrement free buf count
            BuddyAdjustPerColorStats(memSpace, startBuf, numBufs, FALSE);
 
            break;
         }
      } else {
         break;
      }
   }

   endTSC = RDTSC();
   BuddyStatsAddCycles(startTSC, endTSC, &info->stats.allocRunningCycles, 
                       &info->stats.allocRunningSamples, 
                       &info->stats.allocHistCycles, 
                       &info->stats.allocHistSamples);

   /*
    * Set up the next location to allocate.
    */
   if (status != VMK_OK) {
      int32 scanCount = BUDDY_MAX_SCAN_COUNT;
      while (locBuf < info->endBuf && scanCount > 0) {
         bufStatus = BuddyBufNum2Status(info, locBuf);
         if (bufStatus == NULL) {
            locBuf += (info->blockSize >> info->minBufSizeShift);
         } else if (bufStatus->state == BUDDY_BUF_FREE) {
            break;
         } else if (bufStatus->state == BUDDY_BUF_INUSE) {
            BuddySizeType type;
            locBuf += (BuddyGetSize(memSpace, locBuf, &type) >> info->minBufSizeShift);
         } else {
            locBuf++;
         }
         scanCount--;
      }
      *loc = locBuf << info->minBufSizeShift;
   }

out:
   // decrement ref count
   BuddyDecMemSpaceRefCount(memSpace, &prevIRQL);
   return status;
}


/*
 *---------------------------------------------------------------
 *
 * BuddyGetFreeBuf --
 *    
 *    Get a free buffer of the required size from the buffers.
 *    If free buffers of the requried size are unavailable, search
 *    the free list of bigger size.
 *       Sets "freebufSizeShift" to the size shift of the free buffer,
 *
 * Results:
 *    Returns the buffer number of the selected buffer on success.
 *    Returns BUDDY_INVALID_BUF_NUM on failure.
 *
 * Side Effects:
 *    none.
 *  
 *---------------------------------------------------------------
 */
static BufNum
BuddyGetFreeBuf(BuddyMemSpace *memSpace,
                uint32 reqSizeShift,
                uint32 reqColor,
                uint32 *freeBufSizeShift) // OUT
{
   BuddyBufInfo *info = &memSpace->bufInfo;
   BufNum freeBuf;
   uint32 sizeShift;
   ASSERT(SP_IsLockedIRQ(&memSpace->lck));
   uint32 color = reqColor;

   // Search the freelists 
   for (sizeShift = reqSizeShift; sizeShift <= info->maxBufSizeShift; 
        sizeShift++) {
      const uint32 listNdx = BuddySize2ListIndex(info, sizeShift);
      BuddyFreeList *freeList = &info->freeList[listNdx];

      if (color == BUDDY_NO_COLORS) {
         uint32 i;
         for (i = 0; i < freeList->numColors; i++) {
            freeBuf = BuddyRemoveHead(info, listNdx, i, sizeShift);
            if (freeBuf != BUDDY_INVALID_BUF_NUM) {
               *freeBufSizeShift = sizeShift;
               return freeBuf;
            }
         }
      } else {
         ASSERT(color < freeList->numColors);
         freeBuf = BuddyRemoveHead(info, listNdx, color, sizeShift);
         if (freeBuf != BUDDY_INVALID_BUF_NUM) {
            *freeBufSizeShift = sizeShift;
            return freeBuf;
         }
         // adjust color for next buffer size
         color >>= 1;
      }
   }

   LOG(1, "(%s): Buddy allocator out of buffers", memSpace->name);
   *freeBufSizeShift = BUDDY_INVALID_SIZE_SHIFT;
   return BUDDY_INVALID_BUF_NUM;
}


/*
 *---------------------------------------------------------------
 *
 * BuddySplitBuf --
 * 
 *    Split the given buffer into 2. 
 *
 * Results:
 *    From the 2 split buffers, returns the buffer with the 
 * requested color
 *
 * Side Effects:
 *    Buffers are split and added to the free list
 *  
 *---------------------------------------------------------------
 */
static BufNum
BuddySplitBuf(BuddyBufInfo *info,
              BufNum buf,
              uint32 *bufSizeShift,
              uint32 reqSizeShift,
              uint32 reqColor)
{
   uint32 reqBufColor;
   uint32 bufColor;
   const uint32 splitSizeShift = *bufSizeShift - 1;
   // number of min sized buffers after the split
   const uint32 numBufs = BuddyGetNumMinBufs(info, splitSizeShift);
   BufNum splitBuf;

   ASSERT(*bufSizeShift > reqSizeShift);
   ASSERT(*bufSizeShift > info->minBufSizeShift);

   // Mark the buffer as free, before splitting it
   BuddyMarkBufferFree(info, buf, *bufSizeShift);

   /*
    * translate the required color to the color 
    * for the split buffer size
    */
   reqBufColor = BuddyTranslateColor(reqSizeShift, reqColor, splitSizeShift);
   bufColor = BuddyBufNum2Color(info, buf, splitSizeShift);

   if ((reqBufColor == BUDDY_NO_COLORS) || (reqBufColor == bufColor )) {
      // split the buffer, add latter half to free list
      splitBuf = buf + numBufs;
   } else {
      // split the buffer, add earlier half to free list
      splitBuf = buf;
      buf = buf + numBufs;

      ASSERT(reqBufColor != BUDDY_NO_COLORS);
      ASSERT(reqBufColor == BuddyBufNum2Color(info, buf, splitSizeShift));
   }

   // Mark buf as in use
   BuddyMarkBufferInUse(info, buf, splitSizeShift);

   // Change the size of the splitted buf
   BuddyMarkBufferSize(info, splitBuf, splitSizeShift);

   // Add the split part to the appropriate free list
   BuddyInsertFreeBuf(info, splitBuf, splitSizeShift);

   *bufSizeShift = splitSizeShift;
   return buf;
}


/*
 *---------------------------------------------------------------
 *
 * Buddy_GetLocSize --
 *
 *    Gives the size of the specified location
 *
 * Results:
 *    Size of the location.
 *
 * Side Effects:
 *    none.
 *  
 *---------------------------------------------------------------
 */
uint32
Buddy_GetLocSize(Buddy_Handle handle,
                 uint32 loc)
{
   BuddyMemSpace *memSpace = handle;
   BufNum buf;
   BuddySizeType sizeType;
   uint32 retSize;
   SP_IRQL prevIRQL;

   if (!BuddyValidateMemSpace(memSpace)) {
      return 0;
   }

   // increment ref count
   if (!BuddyIncMemSpaceRefCount(memSpace, &prevIRQL)) {
      return 0;
   }

   ASSERT(SP_IsLockedIRQ(&memSpace->lck));
   buf = BuddyLoc2BufNum(memSpace, loc);
   ASSERT(buf != BUDDY_INVALID_BUF_NUM);
   if (buf == BUDDY_INVALID_BUF_NUM) {
      Warning("invalid loc 0x%x", loc);
      BuddyDecMemSpaceRefCount(memSpace, &prevIRQL);
      return 0;
   }

   retSize = BuddyGetSize(memSpace, buf, &sizeType);

   // decrement ref count
   BuddyDecMemSpaceRefCount(memSpace, &prevIRQL);

   return retSize;
}


/*
 *---------------------------------------------------------------
 *
 * BuddyReduceFragmentation --
 * 
 *    The vanilla buddy allocator allocated buffers which are 
 *    power of 2 sizes, this function optimizes this allocation 
 *    by freeing up some of the extra buffers at the end. e.g.
 *    if we need 5 buffers, the vanilla allocator would allocate
 *    8, this function frees up the remaining 3 buffers.
 *
 * Results:
 *    Size of the buffer after some of the buffers are released.
 *
 * Side Effects:
 *    none.
 *  
 *---------------------------------------------------------------
 */
static uint32
BuddyReduceFragmentation(BuddyMemSpace *memSpace, 
                         BufNum buf, 
                         uint32 bufSizeShift, 
                         uint32 size)
{
   BuddyBufInfo *info = &memSpace->bufInfo;
   const uint32 origBufSize = 1 << bufSizeShift;
   const uint32 minBufSizeAlignedLen = ROUNDUP(size, info->minBufSize);
   uint32 len, numMinBufs;
   uint32 curSizeShift;
   BufNum curBuf, freeBuf;

   ASSERT(SP_IsLockedIRQ(&memSpace->lck));
   ASSERT(minBufSizeAlignedLen <= origBufSize);
   
   // done, if no fragmentation OR
   //       if fragmentation less than min buffer size
   if ((size == origBufSize) ||
       (minBufSizeAlignedLen == origBufSize)) {
      uint32 statsNdx;
      // Update stats
      statsNdx = BuddySize2StatsIndex(info, bufSizeShift);
      info->stats.numUsedBuf[statsNdx]++;
      info->stats.numFreeCarvedBuf -= BuddyGetNumMinBufs(info, bufSizeShift);
      return origBufSize;
   }

   ASSERT((origBufSize - minBufSizeAlignedLen) >= info->minBufSize);
   // Mark the buffer as free before defragmenting
   BuddyMarkBufferFree(info, buf, bufSizeShift);

   // walk through this buffer and mark as 'used' the potential 
   // *buddys* of the smaller sized buffers we are planning to free 
   // i.e. when we free the fragmented buffers they dont coalesce with
   // the parts of this buffer which are actually in use.
   len = minBufSizeAlignedLen;

   BUDDY_FOR_BUFS_IN_LEN_DO(info, buf, len, curBuf, curSizeShift, numMinBufs) {
      uint32 statsNdx;
      // Update stats
      statsNdx = BuddySize2StatsIndex(info, curSizeShift);
      info->stats.numUsedBuf[statsNdx]++;
      info->stats.numFreeCarvedBuf -= numMinBufs;

      // Mark buffer in use
      BuddyMarkBufferInUse(info, curBuf, curSizeShift);

   }BUDDY_FOR_BUFS_IN_LEN_END;

   // now we release the unused buffers
   len = origBufSize - minBufSizeAlignedLen;
   ASSERT(ROUNDDOWN(len, info->minBufSize) == len);
   ASSERT(len >= info->minBufSize);
   ASSERT(len <= info->maxBufSize);
   freeBuf = buf + (minBufSizeAlignedLen >> info->minBufSizeShift);

   BUDDY_FOR_BUFS_IN_LEN_DO(info, freeBuf, len, curBuf, curSizeShift, numMinBufs) {
      // first mark buffer as in use and then free, 
      // otherwise free gets confused
      BuddyMarkBufferInUse(info, curBuf, curSizeShift);
      BuddyBufFreeInt(memSpace, curBuf, curSizeShift);
   }BUDDY_FOR_BUFS_IN_LEN_END;

   return minBufSizeAlignedLen;
}


/*
 *---------------------------------------------------------------
 *
 * BuddySetSize --
 * 
 *    Store the buffer size, for later use during Buddy_Free.
 *    Since we defragment our buffers, they are not always
 *    a power of two and hence we cannot store them as 
 *    sizeShifts in the BuddyBufStatus structure. What we
 *    do is as follows.
 *    
 *    o If the buffer size is a power of 2 we store it in as size shift
 *    o If the buffer size is 3 buffers, we store a special value
 *      in the size shift to denote this size.
 *    o For sizes greater than 4, (4 is fine because it is a power of 2) 
 *      we use the first BuddyBufStatus to indicate that this is 
 *      a 'complex' size, and use the remaining 3 BuddyBufStatus's to
 *      store the size as the number of minimum sized buffers in
 *      the 24 bit space.
 *
 * Results:
 *    Type of the size.
 *
 * Side Effects:
 *    none.
 *  
 *---------------------------------------------------------------
 */
static BuddySizeType 
BuddySetSize(BuddyMemSpace *memSpace,
             BufNum buf,
             uint32 size)
{
   BuddyBufInfo *info = &memSpace->bufInfo;
   BuddyBufStatus *bufStatus;
   const uint32 numBuffers = size >> info->minBufSizeShift;
   ASSERT((numBuffers << info->minBufSizeShift) == size);

   ASSERT(SP_IsLockedIRQ(&memSpace->lck));

   // if size is a power of 2 then store it as size shifts
   if (Util_IsPowerOf2(size)) {
      const uint32 sizeShift = BuddySize2Shift(size);
      bufStatus = BuddyBufNum2Status(info, buf);
      ASSERT(bufStatus->state == BUDDY_BUF_INUSE);
      ASSERT(sizeShift <= BUDDY_MAX_SIZE_SHIFT);
      bufStatus->sizeShift = sizeShift;
      return BUDDY_SIZE_TYPE_POWEROF2;
   } else if (numBuffers == BUDDY_BUF_SIZE_3) {
      // Special case handling for buffers of size 3
      bufStatus = BuddyBufNum2Status(info, buf);
      ASSERT(bufStatus->state == BUDDY_BUF_INUSE);
      bufStatus->sizeShift = BUDDY_3_BUFS_SIZE_SHIFT;
      return BUDDY_SIZE_TYPE_3;
   } else {
      uint32 i;
      // For buffers which are *not* of size 3 and *not* power of 2
      ASSERT(numBuffers > BUDDY_BUF_SIZE_3);
      ASSERT(numBuffers <= BUDDY_MAX_NUM_BUFFERS);          

      bufStatus = BuddyBufNum2Status(info, buf);
      ASSERT(bufStatus->state == BUDDY_BUF_INUSE);
      bufStatus->sizeShift = BUDDY_COMPLEX_SIZE_SHIFT;

      for (i = 0; i < 3; i++) {
         const uint8 mask = 0x0ff;
         bufStatus = BuddyBufNum2Status(info, buf + 1 + i);
         *(unsigned char *)bufStatus = (numBuffers >> (8 * i)) & mask;
      }
      return BUDDY_SIZE_TYPE_COMPLEX;
   }
}


/*
 *---------------------------------------------------------------
 *
 * BuddyGetSize --
 * 
 *    Get the size of this buffer, refer to the comment in
 *    BuddySetSize for a description of how we store buffer sizes.
 *       Sets 'sizeType' to the type of size.
 *
 * Results:
 *    Size of the buffer.
 *
 * Side Effects:
 *    none.
 *  
 *---------------------------------------------------------------
 */
static uint32
BuddyGetSize(BuddyMemSpace *memSpace,
             BufNum buf,
             BuddySizeType *sizeType)
{
   BuddyBufStatus *bufStatus;
   BuddyBufInfo *info = &memSpace->bufInfo;
   bufStatus = BuddyBufNum2Status(info, buf);
   uint32 retSize;

   ASSERT(SP_IsLockedIRQ(&memSpace->lck));
   ASSERT(bufStatus->state == BUDDY_BUF_INUSE);
   if (bufStatus->sizeShift == BUDDY_COMPLEX_SIZE_SHIFT) {
      uint32 numBuffers = 0;
      uint32 i;
      for (i = 3; i > 0; i--) {
         const uint8 mask = 0x0ff;
         BuddyBufStatus *bufStatus;
         uint32 value;
         bufStatus = BuddyBufNum2Status(info, buf + i);
         value = *(unsigned char *)bufStatus & mask; 
         numBuffers = (numBuffers << 8) | value;
      }
      ASSERT(numBuffers > BUDDY_BUF_SIZE_3);
      ASSERT(numBuffers <= BUDDY_MAX_NUM_BUFFERS);          
      retSize = numBuffers << info->minBufSizeShift;
      *sizeType = BUDDY_SIZE_TYPE_COMPLEX;
   } else if (bufStatus->sizeShift == BUDDY_3_BUFS_SIZE_SHIFT) {
      // Check if the size is 3, special case handling
      ASSERT(BUDDY_BUF_SIZE_3 <= BuddyGetNumMinBufs(info, info->maxBufSizeShift));
      retSize = (BUDDY_BUF_SIZE_3 << info->minBufSizeShift);
      *sizeType = BUDDY_SIZE_TYPE_3;
   } else {
      // Sizes are power of 2
      bufStatus = BuddyBufNum2Status(info, buf);
      retSize = 1 << bufStatus->sizeShift;
      *sizeType = BUDDY_SIZE_TYPE_POWEROF2;
   }
   ASSERT(retSize > 0);
   ASSERT(retSize >= info->minBufSize);
   ASSERT(retSize <= info->maxBufSize);
   ASSERT(ROUNDDOWN(retSize, info->minBufSize) == retSize);
   return retSize;
}


/*
 *---------------------------------------------------------------
 *
 * BuddyGetAndClearSize --
 * 
 *    Get the size of this buffer and setup the buffer status sizeShift
 *    field to be in a state that the vanilla buddy allocator expects
 *    it to be.
 *
 * Results:
 *    Size of the buffer.
 *
 * Side Effects:
 *    none.
 *  
 *---------------------------------------------------------------
 */
static uint32
BuddyGetAndClearSize(BuddyMemSpace *memSpace, 
                     BufNum buf, 
                     BuddySizeType *sizeType)
{
   BuddyBufInfo *info = &memSpace->bufInfo;
   uint32 size = BuddyGetSize(memSpace, buf, sizeType);
   ASSERT(SP_IsLockedIRQ(&memSpace->lck));
   switch (*sizeType) {
      case BUDDY_SIZE_TYPE_COMPLEX: {
         uint32 bufSize, sizeShift, numMinBufs;
         uint32 i;
         BuddyBufStatus *bufStatus;
         bufSize = BuddyFindLargestBufSize(info, buf, size, &sizeShift, &numMinBufs); 
         bufStatus = BuddyBufNum2Status(info, buf);
         bufStatus->sizeShift = sizeShift;
         ASSERT(bufStatus->state == BUDDY_BUF_INUSE);
         for (i = 1; i < 4; i++) {
            bufStatus = BuddyBufNum2Status(info, buf + i);
            bufStatus->sizeShift = BUDDY_INVALID_SIZE_SHIFT;
            bufStatus->state = BUDDY_BUF_FREE;
            if (BUDDY_AID_DEBUGGING) {
               bufStatus->state = BUDDY_BUF_INUSE;
            }
         }
         break;
      }
      case BUDDY_SIZE_TYPE_3: {
         uint32 bufSize, sizeShift, numMinBufs;
         BuddyBufStatus *bufStatus;
         bufSize = BuddyFindLargestBufSize(info, buf, size, &sizeShift, &numMinBufs); 
         ASSERT(numMinBufs == 2);
         bufStatus = BuddyBufNum2Status(info, buf);
         bufStatus->sizeShift = sizeShift;
         break;
      }
      default:
         // nothing to do for power of 2 buffers
         break;
   }
   return size;
}


/*
 *---------------------------------------------------------------
 *
 * Buddy_Free --
 * 
 *    Free the given location.
 *
 * Results:
 *    Size of the location just released
 *
 * Side Effects:
 *    none.
 *  
 *---------------------------------------------------------------
 */
uint32
Buddy_Free(Buddy_Handle handle,
           uint32 loc)
{
   BuddyMemSpace *memSpace = handle;
   BuddyBufInfo *info = &memSpace->bufInfo;
   BufNum buf, curBuf;
   uint32 size, minBufs;
   uint32 sizeShift;
   uint32 retSize;
   SP_IRQL prevIRQL;
   uint32 statsNdx;
   BuddySizeType sizeType;
   TSCCycles startTSC, endTSC;

   if (!BuddyValidateMemSpace(memSpace)) {
      return 0;
   }

   // increment ref count
   if (!BuddyIncMemSpaceRefCount(memSpace, &prevIRQL)) {
      return 0;
   }
   // non-preemptible here
   startTSC = RDTSC();

   ASSERT(SP_IsLockedIRQ(&memSpace->lck));
   buf = BuddyLoc2BufNum(memSpace, loc);
   ASSERT(buf != BUDDY_INVALID_BUF_NUM);
   if (buf == BUDDY_INVALID_BUF_NUM) {
      Warning("invalid location 0x%x", loc);
      BuddyDecMemSpaceRefCount(memSpace, &prevIRQL);
      return 0;
   }
   retSize = size = BuddyGetAndClearSize(memSpace, buf, &sizeType);

   // update stats
   info->stats.numTypeReleased[sizeType]++;

   BUDDY_FOR_BUFS_IN_LEN_DO(info, buf, size, curBuf, sizeShift, minBufs) {
      // update stats
      statsNdx = BuddySize2StatsIndex(info, sizeShift);
      info->stats.numUsedBuf[statsNdx]--;
      info->stats.numFreeCarvedBuf += minBufs;

      // Adjust per color free buffer stats, increment free buf count
      BuddyAdjustPerColorStats(memSpace, curBuf, minBufs, TRUE); 

      // Free this buffer
      BuddyBufFreeInt(memSpace, curBuf, sizeShift);
   } BUDDY_FOR_BUFS_IN_LEN_END;

   endTSC = RDTSC();
   BuddyStatsAddCycles(startTSC, endTSC, &info->stats.freeRunningCycles, 
                       &info->stats.freeRunningSamples, 
                       &info->stats.freeHistCycles, 
                       &info->stats.freeHistSamples);

   // decrement ref count
   BuddyDecMemSpaceRefCount(memSpace, &prevIRQL);

   return retSize;
}


/*
 *---------------------------------------------------------------
 *
 * BuddyBufFreeInt --
 * 
 *    Mark the given buffer as free, coalesce if possible and 
 *    add it to the free list.
 *
 * Results:
 *    none.
 *
 * Side Effects:
 *    May cause coalescing of adjacent buffers.
 *  
 *---------------------------------------------------------------
 */
static void
BuddyBufFreeInt(BuddyMemSpace *memSpace,
                BufNum buf,
                uint32 sizeShift)
{
   BuddyBufInfo *info = &memSpace->bufInfo;

   ASSERT(SP_IsLockedIRQ(&memSpace->lck));
   while (1) {
      if (!BuddyCoalesce(memSpace, &buf, &sizeShift)) {
         break;
      }
   }
   
   ASSERT(SP_IsLockedIRQ(&memSpace->lck));
   if (vmx86_debug) {
      BuddyBufStatus *bufStatus;
      bufStatus = BuddyBufNum2Status(info, buf);
      ASSERT(bufStatus);
      ASSERT(bufStatus->state == BUDDY_BUF_INUSE);
   }
   // Mark buffer as free
   BuddyMarkBufferFree(info, buf, sizeShift);

   // Find the appropriate free list and add to it
   BuddyInsertFreeBuf(info, buf, sizeShift);
}


/*
 *---------------------------------------------------------------
 *
 * BuddyCoalesce --
 * 
 *    Coalesce adjacent buffers if possible.
 *    Sets "buf" to the new buffer number if
 *    coalescing was successful
 *    Sets "sizeShift" to the new buffer size if 
 *    coalescing was successful
 *       Callers must acquire the memSpace->lck.
 *
 * Results:
 *    On success, returns TRUE.
 *    On failure, returns FALSE.
 *
 * Side Effects:
 *    none.
 *  
 *---------------------------------------------------------------
 */
static Bool 
BuddyCoalesce(BuddyMemSpace *memSpace,
              uint32 *buf,        // IN/OUT
              uint32 *sizeShift)  // IN/OUT
{
   BuddyBufInfo *info = &memSpace->bufInfo;
   uint32 newBuf;
   uint32 buddyBuf;

   const uint32 numMinBuffers = BuddyGetNumMinBufs(info, *sizeShift);
   const uint32 shiftDiff = *sizeShift - info->minBufSizeShift;
   const uint32 nextSizeMask = (1 << (shiftDiff + 1)) - 1;
   DEBUG_ONLY(const uint32 curSizeMask = (1 << shiftDiff) - 1);

   ASSERT(SP_IsLockedIRQ(&memSpace->lck));
   // Don't expect to coalesce buffer of max size
   if (*sizeShift >= info->maxBufSizeShift) {
      return FALSE;
   }

   ASSERT((*buf & curSizeMask) == 0); 

   if ((*buf & nextSizeMask) == 0) {
      // Buddy = buf + (num min size buffers in sizeShift)
      buddyBuf = *buf + numMinBuffers;
      newBuf = *buf;
   } else {
      // Buddy = buf - (num min size buffers in sizeShift)
      ASSERT(*buf >= numMinBuffers);
      buddyBuf = newBuf = *buf - numMinBuffers;
      ASSERT((newBuf & nextSizeMask) == 0);
   }

   // Check if our buddy is free
   if (!BuddyAreBuffersFree(info, buddyBuf, *sizeShift)) {
      return FALSE;
   }

   // Mark the original buffer as free
   BuddyMarkBufferFree(info, *buf, *sizeShift);

   // Remove our buddy from its free list
   BuddyRemoveFreeBuf(info, buddyBuf, *sizeShift);

   // Mark both bufs to invalid size (no longer head buf of free block)
   BuddyMarkBufferSize(info, *buf, BUDDY_INVALID_SIZE_SHIFT);
   BuddyMarkBufferSize(info, buddyBuf, BUDDY_INVALID_SIZE_SHIFT);

   // Mark the newBuf as in use
   BuddyMarkBufferInUse(info, newBuf, *sizeShift + 1);

   *buf = newBuf;
   *sizeShift = *sizeShift + 1;
   return TRUE;
}


/*
 *---------------------------------------------------------------
 *
 * Buddy_HotAddMemRequired --
 *
 *    Find out the amount of memory required to 'hot add' the
 *    given 'len' to the memory space identified by 'handle'
 *
 *    Note: memSize of '0' is a valid value for Hot add ranges
 *    because we reserve space for each block, and clients 
 *    could potentially make calls to add ranges within one block
 *    in which case the first call would have allocated all the
 *    space and the second call does not need to allocate any 
 *    additional space and hence the memSize of '0'.
 *    
 * Results:
 *    VMK_OK on success, 
 *    error code otherwise.
 *    
 * Side effects:
 *    none.
 *
 *---------------------------------------------------------------
 */
VMK_ReturnStatus
Buddy_HotAddMemRequired(Buddy_Handle handle,
                        uint32 start,
                        uint32 len,
                        uint32 *memRequired)
{
   uint32 numBuffers;
   BufNum startBuf;
   BuddyMemSpace *memSpace = handle;
   BuddyBufInfo *info= &memSpace->bufInfo;
   const uint32 numBlockBuffers = info->blockSize >> info->minBufSizeShift;
   uint32 newStart, newLen;
   uint32 startBlock;
   uint32 numBlocks;
   uint32 i;

   *memRequired = 0;
   ASSERT(len > 0);
   if (!BuddyValidateMemSpace(memSpace)) {
      return VMK_INVALID_HANDLE;
   }

   ASSERT(memSpace->type == BUDDY_DYNAMIC_SPACE);
   if (UNLIKELY(memSpace->type != BUDDY_DYNAMIC_SPACE)) {
      Warning("(%s): hot add called on a static mem space", memSpace->name);
      return VMK_INVALID_TYPE;
   }
   ASSERT(info->blockSize > 0);

   // check overflow
   ASSERT((start + len) > start);  
   ASSERT((start + len) <= (memSpace->start + memSpace->maxLen));

   // Align the start and end to block size
   BuddyAlignStartAndEnd(start, len, info->blockSize, &newStart, &newLen);
   // check overflow
   ASSERT((newStart + newLen) > newStart);  
   ASSERT((newStart + newLen) <= (memSpace->start + memSpace->maxLen));

   numBuffers = newLen >> info->minBufSizeShift;
   startBuf = newStart >> info->minBufSizeShift;

   ASSERT(startBuf >= info->startBuf);
   if (UNLIKELY(startBuf < info->startBuf)) {
      Warning("(%s): new start buf(0x%x) cannont be lower "
              "than start buf(0x%x)", memSpace->name, startBuf, info->startBuf);
      return VMK_BAD_PARAM;
   }

   numBlocks = CEILING(newLen, info->blockSize);
   BuddyBufNum2BlockStatusNdx(info, startBuf, &startBlock, NULL);
   for (i = 0; i < numBlocks; i++) {
      const uint32 block = startBlock + i;
      ASSERT(block < info->numBlocks);
      if (info->bufBlocks[block].bufStatus == NULL) {
         *memRequired += numBlockBuffers * sizeof(BuddyBufStatus);
      }
      if (info->bufBlocks[block].listNodes == NULL) {
         *memRequired += (numBlockBuffers / 2) * sizeof(BuddyListNode);
      }
   }
   return VMK_OK;
}


/*
 *---------------------------------------------------------------
 *
 * Buddy_HotAddRange --
 *
 *    Hot add the given range to the memory space identified by
 *    'handle'
 *
 *    Note: memSize of '0' is a valid value for Hot add ranges
 *    because we reserve space for each block, and clients 
 *    could potentially make calls to add ranges within one block
 *    in which case the first call would have allocated all the
 *    space and the second call does not need to allocate any 
 *    additional space and hence the memSize of '0'.
 *    
 * Results:
 *    On success, returns VMK_OK
 *    On failure, returns error code
 * 
 *    
 * Side effects:
 *    none.
 *
 *---------------------------------------------------------------
 */
VMK_ReturnStatus
Buddy_HotAddRange(Buddy_Handle handle,
                  uint32 memSize,
                  char *mem,
                  uint32 start,
                  uint32 len,
                  uint32 numRanges,
                  Buddy_AddrRange *addrRange)
{
   BuddyMemSpace *memSpace = handle;
   BuddyBufInfo *info = &memSpace->bufInfo;
   uint32 newStart, newLen;
   uint32 numBuffers;
   char *inMem = mem;
   BufNum startBuf;
   SP_IRQL prevIRQL;
   uint32 endBuf;
   VMK_ReturnStatus status = VMK_OK;

   if (!BuddyValidateMemSpace(memSpace)) {
      return VMK_INVALID_HANDLE;
   }

   // increment ref count
   if (!BuddyIncMemSpaceRefCount(memSpace, NULL)) {
      return VMK_FAILURE;
   }

   // len can be zero if we are trying to add a region of memory which
   // is completely consumed by existing regions and hence mem
   // required for such regions is zero
   if (UNLIKELY(len == 0)) {
      Log("(%s) len is zero, for start 0x%x, len = %d",
          memSpace->name, start, len);
   }

   ASSERT(memSpace->type == BUDDY_DYNAMIC_SPACE);
   if (UNLIKELY(memSpace->type != BUDDY_DYNAMIC_SPACE)) {
      Warning("(%s): hot add called on a static mem space", memSpace->name);
      status = VMK_FAILURE;
      goto out;
   }

   if (vmx86_debug) {
      DEBUG_ONLY(uint32 memRequired);
      DEBUG_ONLY(VMK_ReturnStatus status);
      DEBUG_ONLY(status = Buddy_HotAddMemRequired(handle, start, len,
                                                  &memRequired));
      ASSERT(status == VMK_OK);
      ASSERT(memSize >= memRequired);
   }

   // guard against other simultaneous hot adds
   prevIRQL = SP_LockIRQ(&memSpace->hotAddLck, SP_IRQL_KERNEL);

   // check overflow
   ASSERT((start + len) > start);  
   ASSERT((start + len) <= (memSpace->start + memSpace->maxLen));

   // Align the start and end to block size
   BuddyAlignStartAndEnd(start, len, info->blockSize, &newStart, &newLen);
   // check overflow
   ASSERT((newStart + newLen) > newStart);  
   ASSERT((newStart + newLen) <= (memSpace->start + memSpace->maxLen));

   numBuffers = newLen >> info->minBufSizeShift;
   startBuf = newStart >> info->minBufSizeShift;
   ASSERT(startBuf >= info->startBuf);
   if (UNLIKELY(startBuf < info->startBuf)) {
      Warning("(%s): new start buf(0x%x) cannont be lower than start buf(0x%x)",
              memSpace->name, startBuf, info->startBuf);
      SP_UnlockIRQ(&memSpace->hotAddLck, prevIRQL);
      status = VMK_FAILURE;
      goto out;
   }

   endBuf = (newStart + newLen) >> info->minBufSizeShift;
   ASSERT(endBuf >= startBuf);
   ASSERT(endBuf < BUDDY_MAX_BUF_NUM);
   if (LIKELY(endBuf > info->endBuf)) {
      info->endBuf = endBuf;
   }

   if (memSize) {
      // Assign memory to block elements, i.e. buffer status and list nodes
      // NOTE: This function uses up all the given memory
      mem = BuddyAssignBlockElements(memSpace, mem, memSize, startBuf);
      ASSERT_NOT_IMPLEMENTED(mem <= (inMem + memSize));
   }
   // Confirm that memory is allocated for the buffers
   status = BuddyCheckBufMemory(memSpace, startBuf, numBuffers);
   ASSERT_NOT_IMPLEMENTED(status == VMK_OK);

   // Add this new range to the free list 
   BuddyCarveBuffers(memSpace, numRanges, addrRange);

   SP_UnlockIRQ(&memSpace->hotAddLck, prevIRQL);


out:
   // decrement ref count
   BuddyDecMemSpaceRefCount(memSpace, NULL);
   return status;
}


/*
 *---------------------------------------------------------------
 *
 * Buddy_GetNumFreeBufs --
 *
 *    Get the number of minimun sized buffers that are currently
 *    free.
 *
 * Results:
 *    number of free buffers.
 *
 * Side Effects:
 *    none.
 *  
 *---------------------------------------------------------------
 */
uint32 
Buddy_GetNumFreeBufs(Buddy_Handle handle)
{
   BuddyMemSpace *memSpace = handle;
   BuddyBufStatistics *stats = &memSpace->bufInfo.stats;
   if (!BuddyValidateMemSpace(memSpace)) {
      return 0;
   }
   return stats->numFreeCarvedBuf;
}


/*
 *---------------------------------------------------------------
 *
 * Buddy_GetNumUsedBufs --
 *
 *    Get the number of minimun sized buffers that are currently
 *    in use.
 *
 * Results:
 *    number of used buffers.
 *
 * Side Effects:
 *    none.
 *  
 *---------------------------------------------------------------
 */
uint32 
Buddy_GetNumUsedBufs(Buddy_Handle handle)
{
   BuddyMemSpace *memSpace = handle;
   BuddyBufStatistics *stats = &memSpace->bufInfo.stats;
   if (!BuddyValidateMemSpace(memSpace)) {
      return 0;
   }
   return (stats->numCarvedBuf - stats->numFreeCarvedBuf);
}


/*
 *---------------------------------------------------------------
 *
 * Buddy_GetNumFreeBufsForColor --
 *
 *    Get the number of minimun sized buffers that are currently
 *    free for the given color.
 *
 * Results:
 *    number of free buffers.
 *
 * Side Effects:
 *    none.
 *  
 *---------------------------------------------------------------
 */
uint32 
Buddy_GetNumFreeBufsForColor(Buddy_Handle handle,
                             uint32 color)
{
   BuddyMemSpace *memSpace = handle;
   BuddyBufStatistics *stats = &memSpace->bufInfo.stats;
   if (!BuddyValidateMemSpace(memSpace)) {
      return 0;
   }
   ASSERT(stats->numColors > color);
   return stats->colorFreeBuf[color];
}


/*
 *---------------------------------------------------------------
 *
 * Buddy_GetNumUsedBufsForColor --
 *
 *    Get the number of minimun sized buffers that are currently
 *    used for the given color.
 *
 * Results:
 *    number of used buffers.
 *
 * Side Effects:
 *    none.
 *  
 *---------------------------------------------------------------
 */
uint32 
Buddy_GetNumUsedBufsForColor(Buddy_Handle handle,
                             uint32 color)
{
   uint32 numUsed;
   BuddyMemSpace *memSpace = handle;
   SP_IRQL prevIRQL;
   BuddyBufStatistics *stats = &memSpace->bufInfo.stats;
   if (!BuddyValidateMemSpace(memSpace)) {
      return 0;
   }
   ASSERT(stats->numColors > color);
   prevIRQL = SP_LockIRQ(&memSpace->lck, SP_IRQL_KERNEL);
   ASSERT(stats->colorTotBuf[color] >= stats->colorFreeBuf[color]);
   numUsed = (stats->colorTotBuf[color] - stats->colorFreeBuf[color]);
   SP_UnlockIRQ(&memSpace->lck, prevIRQL);
   return numUsed;
}


/*
 *---------------------------------------------------------------
 *
 * Buddy_DumpEntries --
 *
 *    Dump the buffer status of buffers in use
 *
 * Results:
 *    number of used buffers.
 *
 * Side Effects:
 *    none.
 *  
 *---------------------------------------------------------------
 */
void 
Buddy_DumpEntries(Buddy_Handle handle)
{
   BuddyMemSpace *memSpace = handle;
   BuddyBufInfo *info = &memSpace->bufInfo;
   const uint32 numBlockBuffers = info->blockSize >> info->minBufSizeShift;
   uint32 block;
   uint32 numDumped = 0;
   if (!BuddyValidateMemSpace(memSpace)) {
      return;
   }
   // increment ref count
   if (!BuddyIncMemSpaceRefCount(memSpace, NULL)) {
      return;
   }

   Log("Dumping %s", memSpace->name);
   // Go through each block, and log buffers that are in use
   for (block = 0; block < info->numBlocks; block++) {
      BuddyBufStatus *bufStatus;
      BufNum startBuf = block * numBlockBuffers + info->startBuf;
      BufNum nextBuf = startBuf;
      BufNum curBuf;

      // Because of hot add support, not all blocks have buffers allocated
      if (!info->bufBlocks[block].bufStatus) {
         continue;
      }

      BUDDY_FOR_BUFS_DO(info, startBuf, numBlockBuffers, curBuf, bufStatus) {
         if (nextBuf == curBuf) {
            uint32 numUsed = 1;
            if (bufStatus->state == BUDDY_BUF_INUSE) {
               World_ID debug1 = INVALID_WORLD_ID;
               uint16 debug2 = 0;
               BuddySizeType sizeType;
               SP_IRQL prevIRQL;
               uint32 size;

               prevIRQL = SP_LockIRQ(&memSpace->lck, SP_IRQL_KERNEL);
               size = BuddyGetSize(memSpace, curBuf, &sizeType);
               SP_UnlockIRQ(&memSpace->lck, prevIRQL);
               
               DEBUG_ONLY(debug1 = bufStatus->debugWorldID);
               DEBUG_ONLY(debug2 = bufStatus->debugRA);
               Log("Location 0x%x, size %d, debugWorldID 0x%x, debugRA %x",
                   BuddyBufNum2Loc(memSpace, curBuf), size, debug1, debug2);

               ASSERT(ROUNDDOWN(size, info->minBufSize) == size);
               numUsed = size >> info->minBufSizeShift;
               numDumped += numUsed;
            }
            nextBuf = curBuf + numUsed;
         }
      } BUDDY_FOR_BUFS_END;
   }
   Log("Done dumping %s, dumped %d min sized blocks", 
       memSpace->name, numDumped);

   // decrement ref count
   BuddyDecMemSpaceRefCount(memSpace, NULL);
}


/*
 *---------------------------------------------------------------
 *
 * BuddyOutputString --
 * 
 *    Utility function that writes the given string either to the
 *    proc node or to the log file.
 *
 * Results:
 *    none.
 *
 * Side Effects:
 *    none.
 *  
 *---------------------------------------------------------------
 */
static void
BuddyOutputString(Proc_Entry *entry,
                  char *buffer,
                  int *len,
                  uint32 loglevel,
                  char *str)
{
   if (entry) {
      Proc_Printf(buffer, len, "%s\n", str);
   } else {
      LOG(loglevel, "%s", str);
   }
}

/*
 *---------------------------------------------------------------
 *
 * BuddySizeType2Str --
 * 
 *    Utility function to get the name for a given size type.
 * 
 * Results:
 *    none.
 *
 * Side Effects:
 *    none.
 *  
 *---------------------------------------------------------------
 */
#define BUDDY_TYPE_NAME_MAX  (16)
static INLINE void
BuddySizeType2Str(BuddySizeType type,
                  char *str,
                  uint32 len)
{
   switch (type) {
      case BUDDY_SIZE_TYPE_POWEROF2:
         snprintf(str, len, "%s", "powerof2");
         break;
      case BUDDY_SIZE_TYPE_COMPLEX:
         snprintf(str, len, "%s", "complex");
         break;
      case BUDDY_SIZE_TYPE_3:
         snprintf(str, len, "%s", "size3");
         break;
      default:
         snprintf(str, len, "%s", "unknown");
         break;
   }
}

/*
 *---------------------------------------------------------------
 *
 * BuddyOutputAvgCycles --
 * 
 *    Dump the average cycles.
 * 
 * Results:
 *    none.
 *
 * Side Effects:
 *    Stats are written
 *  
 *---------------------------------------------------------------
 */
static void
BuddyOutputAvgCycles(Bool history,
                     uint64 samples,
                     TSCCycles totCycles,
                     Proc_Entry *entry,
                     char *buffer,
                     int *len,
                     uint32 loglevel,
                     char *scratchBuf,
                     uint32 scratchBufLen)
{
   snprintf(scratchBuf, scratchBufLen, "%10s %10s, %10s %20s", 
            (history ?  "History:" : "Current"), "Samples",
            "Avg Cycles","Avg sec:usec");
   BuddyOutputString(entry, buffer, len, loglevel, scratchBuf);

   if (samples != 0) {
      TSCCycles avgHistCycles = totCycles / samples;
      uint32 sec, usec;
      Timer_TSCToSec(avgHistCycles, &sec, &usec);
      snprintf(scratchBuf, scratchBufLen, "%10s %10"FMT64"d %10"FMT64"d, %10d:%10d", 
               " ", samples, avgHistCycles, sec, usec);
      BuddyOutputString(entry, buffer, len, loglevel, scratchBuf);
   }
}


/*
 *---------------------------------------------------------------
 *
 * BuddyOutputAvgOvhd --
 * 
 *    Dump the average overhead for either 'allocations' or 'free'
 *    depending on the input.
 * 
 * Results:
 *    none.
 *
 * Side Effects:
 *    Stats are written
 *  
 *---------------------------------------------------------------
 */
static void
BuddyOutputAvgOvhd(BuddyBufInfo *info,
                   Bool allocation,
                   Proc_Entry *entry,
                   char *buffer,
                   int *len,
                   uint32 loglevel,
                   char *scratchBuf,
                   uint32 scratchBufLen)
{
   uint64 samples;
   TSCCycles cycles;

   snprintf(scratchBuf, scratchBufLen, "\nAvg %s stats", 
            (allocation ? "allocation" : "free"));
   BuddyOutputString(entry, buffer, len, loglevel, scratchBuf);

   // print historical average
   samples = (allocation ? info->stats.allocHistSamples :
                           info->stats.freeHistSamples);
   cycles = (allocation ? info->stats.allocHistCycles :
                          info->stats.freeHistCycles);
   BuddyOutputAvgCycles(TRUE, samples, cycles, entry, buffer, len, loglevel,
                        scratchBuf, scratchBufLen);


   // print running average
   samples = (allocation ? info->stats.allocRunningSamples :
                           info->stats.freeRunningSamples);
   cycles = (allocation ? info->stats.allocRunningCycles :
                          info->stats.freeRunningCycles);
   BuddyOutputAvgCycles(FALSE, samples, cycles, entry, buffer, len, loglevel,
                        scratchBuf, scratchBufLen);
}
/*
 *---------------------------------------------------------------
 *
 * BuddyOutputStats --
 * 
 *    Dump the statistics for the given memspace to either the
 *    proc node or to the log file.
 * 
 * Results:
 *    none.
 *
 * Side Effects:
 *    none.
 *  
 *---------------------------------------------------------------
 */
static void
BuddyOutputStats(BuddyMemSpace *memSpace,
                 Proc_Entry *entry,
                 char *buffer,
                 int *len,
                 uint32 loglevel,
                 Bool verbose)
{
   BuddyBufInfo *info = &memSpace->bufInfo;
   uint32 i;
   BuddyBufStatistics *stats = &info->stats;
   char str[BUDDY_MAX_STRING];

   snprintf(str, sizeof(str), "%s", memSpace->name);
   BuddyOutputString(entry, buffer, len, loglevel, str);

   snprintf(str, sizeof(str), "Number of %u sized buffers     : %u", 
            info->minBufSize, stats->numCarvedBuf);
   BuddyOutputString(entry, buffer, len, loglevel, str);

   snprintf(str, sizeof(str), "Number of %u sized buffers free: %u", 
            info->minBufSize, stats->numFreeCarvedBuf);
   BuddyOutputString(entry, buffer, len, loglevel, str);

   snprintf(str, sizeof(str), "Number of %u sized buffers used: %u", 
            info->minBufSize, (stats->numCarvedBuf - stats->numFreeCarvedBuf));
   BuddyOutputString(entry, buffer, len, loglevel, str);

   snprintf(str, sizeof(str), "%12s %10s %10s", 
            "Buffer Size", "Free", "Used");
   BuddyOutputString(entry, buffer, len, loglevel, str);

   for (i = 0; i < info->numBufSizes; i++) {
      snprintf(str, sizeof(str), "%12u %10u %10u", 
               (1 << (info->minBufSizeShift + i)), stats->numFreeBuf[i], 
               stats->numUsedBuf[i]);
      BuddyOutputString(entry, buffer, len, loglevel, str);
   }

   if (verbose) {
      uint32 colorFreeTotal = 0;
      uint32 colorUsedTotal = 0;
      uint32 colorTotal = 0;
      snprintf(str, sizeof(str), "\n%12s %10s %10s %10s", 
               "Color", "Free", "Used", "Total");
      BuddyOutputString(entry, buffer, len, loglevel, str);
      for (i = 0; i < info->stats.numColors; i++) {
         snprintf(str, sizeof(str), "%12u %10u %10u %10u", 
                  i, stats->colorFreeBuf[i],
                  stats->colorTotBuf[i] - stats->colorFreeBuf[i],
                  stats->colorTotBuf[i]);
         BuddyOutputString(entry, buffer, len, loglevel, str);
         colorFreeTotal += stats->colorFreeBuf[i];
         colorUsedTotal += (stats->colorTotBuf[i] - stats->colorFreeBuf[i]);
         colorTotal += stats->colorTotBuf[i];
      }
      snprintf(str, sizeof(str), "%12s %10u %10u %10u", 
               "Total", colorFreeTotal, colorUsedTotal, colorTotal);
      BuddyOutputString(entry, buffer, len, loglevel, str);

      snprintf(str, sizeof(str), "\n%12s %10s %10s", 
               "Size Type", "Allocated", "Freed");
      BuddyOutputString(entry, buffer, len, loglevel, str);

      for (i = 0; i < BUDDY_SIZE_TYPE_MAX; i++) {
         char type[BUDDY_TYPE_NAME_MAX];
         BuddySizeType2Str(i, type, BUDDY_TYPE_NAME_MAX);
         snprintf(str, sizeof(str), "%12s %10u %10u", 
                  type, stats->numTypeAllocated[i], stats->numTypeReleased[i]);
         BuddyOutputString(entry, buffer, len, loglevel, str);
      }

      // print average overhead for allocation
      BuddyOutputAvgOvhd(info, TRUE, entry, buffer, len, loglevel, 
                         str, sizeof(str));
      // print average overhead for free
      BuddyOutputAvgOvhd(info, FALSE, entry, buffer, len, loglevel, 
                         str, sizeof(str));
   }
}

/*
 *---------------------------------------------------------------
 *
 * BuddyLogStats --
 * 
 *    Log statistics
 *
 * Results:
 *    none.
 *
 * Side Effects:
 *    statistics are logged
 *  
 *---------------------------------------------------------------
 */
static void
BuddyLogStats(BuddyMemSpace *memSpace) 
{
   BuddyOutputStats(memSpace, NULL, NULL, NULL, 2, TRUE);
}


/*
 *---------------------------------------------------------------
 *
 * BuddyProcRead --
 * 
 *    Callback function for read on procfs 
 *    /proc/vmware/buddy/<name>
 *
 * Results:
 *    VMK_OK
 *
 * Side Effects:
 *    none.
 *  
 *---------------------------------------------------------------
 */
static int
BuddyProcRead(Proc_Entry *entry,
              char *buffer,
              int *len)
{
   BuddyMemSpace *memSpace = (BuddyMemSpace *)entry->private; 
   *len = 0;
   if (!BuddyValidateMemSpace(memSpace)) {
      return VMK_OK;
   }

   BuddyOutputStats(memSpace, entry, buffer, len, 0, FALSE);
   return VMK_OK;
}

/*
 *---------------------------------------------------------------
 *
 * BuddyProcReadVerbose --
 * 
 *    Callback function for read on procfs 
 *    /proc/vmware/buddy/<name>-verbose
 *
 * Results:
 *    VMK_OK
 *
 * Side Effects:
 *    none.
 *  
 *---------------------------------------------------------------
 */
static int
BuddyProcReadVerbose(Proc_Entry *entry,
                     char *buffer,
                     int *len)
{
   BuddyMemSpace *memSpace = (BuddyMemSpace *)entry->private; 
   *len = 0;
   if (!BuddyValidateMemSpace(memSpace)) {
      return VMK_OK;
   }

   BuddyOutputStats(memSpace, entry, buffer, len, 0, TRUE);
   return VMK_OK;
}


/*
 *---------------------------------------------------------------
 *
 * BuddyProcWrite --
 * 
 *    Callback function for write to procfs 
 *    /proc/vmware/buddy/<name>
 *
 * Results:
 *    VMK_OK
 *
 * Side Effects:
 *    none.
 *  
 *---------------------------------------------------------------
 */
static int
BuddyProcWrite(Proc_Entry *entry,
               char *buffer,
               int *len)
{
   BuddyMemSpace *memSpace = (BuddyMemSpace *)entry->private; 
   if (!BuddyValidateMemSpace(memSpace)) {
      return VMK_OK;
   }
   Buddy_DumpEntries(memSpace);
   return VMK_OK;
}

