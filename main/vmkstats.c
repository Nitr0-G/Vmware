/* **********************************************************
 * Copyright 2000 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * vmkstats.c -- 
 *
 *	VMKernel statistics collection.
 * 
 *  	This is a profiler for the kernel.  It sets up and is called by an
 *  	NMI interrupt handler that is called whenever the performance
 *  	counters overflow.  We record the eip and walk up the stack each
 *  	time the NMI interrupt handler is called.  This data is then
 *  	stored in compact hash tables in the kernel heap, and can be
 *  	pulled out by user processes in the console.
 *
 *  	The interrupt handler records samples in a per-cpu sample buffer
 *  	sampleBuffers[pcpuNum].  This buffer has a get and a put pointer
 *  	and stores variable sized entries of type StatsQuickSample.  When
 *  	the buffer is half full, we schedule a bottom-half to drain the
 *	pcpu buffer into global stats data structures.  Currently, the
 *	draining is performed in the context of a separate "vmkstats" world.
 *
 *  	There are two important global data structures.  A hash-set of
 *  	CallStacks (stored in data.callStacksMap and data.callStacks), and
 *  	a hash-table of StatsSample -> count.  Both of these hashes use
 *  	open addressing.
 */

/*
 * Includes
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "x86.h"
#include "vm_asm.h"
#include "vmkernel.h"
#include "vmnix_if.h"
#include "semaphore.h"
#include "prda.h"
#include "host.h"
#include "sched.h"
#include "memalloc.h"
#include "proc.h"
#include "vmk_layout.h"
#include "x86perfctr.h"
#include "nmi.h"
#include "world.h"
#include "vmkstats.h"
#include "parse.h"
#include "libc.h"
#include "hash.h"
#include "bh.h"
#include "memmap.h"
#include "xmap.h"
#include "heapsort.h"
#include "timer.h"
#include "cpusched.h"

#define LOGLEVEL_MODULE VmkStats
#include "log.h"


/*
 * Compilation flags
 */

// debugging
#if	defined(VMX86_DEBUG) && defined(VMX86_DEVEL)
#define	VMKSTATS_DEBUG_VERBOSE	(0)
#define	VMKSTATS_DEBUG		(1)
#else
#define	VMKSTATS_DEBUG_VERBOSE	(0)
#define	VMKSTATS_DEBUG		(0)
#endif

// targeted debugging
#define	VMKSTATS_DEBUG_MEM	(0)

/*
 * Constants
 */

#define	VMK_TEXT_START		(VMK_CODE_START)
#define	VMK_TEXT_END		((uint32) &_etext)
#define	VMK_TEXT_SIZE		((VMK_TEXT_END - VMK_TEXT_START) + 1)

#define	PROC_CMD_ARGS_MAX	(16)

#define	STATS_MAX_IMAGES	(16)

#define STATS_MAX_CALL_DEPTH                    (50)
#define STATS_MAX_HASH_FILL_PERCENT             (75)
#define STATS_INITIAL_SAMPLE_MAP_COUNT          (1000)
#define STATS_INITIAL_CALL_STACKS_SIZE          (4000)   //in bytes
#define STATS_INITIAL_CALL_STACKS_MAP_COUNT     (500)    
#define STATS_SAMPLE_MAP_GROW_PERCENT           (200)   // must be > 100
#define STATS_CALL_STACKS_GROW_PERCENT          (200)   // must be > 100
#define STATS_CALL_STACKS_MAP_GROW_PERCENT      (200)   // must be > 100
#define STATS_SAMPLE_BUFFER_COUNT               (50000) // count * 4 = size in bytes
#define STATS_MAX_ROOTS                         (15)
#define STATS_INVALID_INDEX			(-1)

/*
 * Types
 */

//variable sized... the zero length array just mean a variable sized array 
//follows this struct in memory
typedef struct {
   int length;
   uint32 stack[0];
} CallStack;

typedef struct {
   uint32 eip;
   int callStackIndex;
   uint32 otherData;
   uint32 count;
} StatsSample;

typedef struct {
   uint32 eip;
   uint32 otherData;
   CallStack callStack;
} StatsQuickSample;

typedef struct {
   XMap_MPNRange bufferRange;
   uint32* buffer; //actually contains variable sized StatsQuickSample entries
   uint32 entries;
   uint32 get;
   uint32 put;
   uint32 maxSafePut;
   Bool stalledOnWrite;
   Bool drainRequested;
} StatsSampleBuffer;

// this information just has to do with what modules we have loaded
// this has nothing specifically to do with vmkstats..
typedef struct {
   char modName[VMNIX_MODULE_NAME_LENGTH];
   uint64 imageId;
   uint32 addr;
   uint32 size;
   uint32 initFunc;
   uint32 cleanupFunc;
   Proc_Entry procDir;
   Proc_Entry procId;
   Proc_Entry loadmap;
} StatsImage;

typedef struct {
   Semaphore sem;
   Proc_Entry procState;
   Proc_Entry procCallStacks;
   Proc_Entry procSamples;

   XMap_MPNRange sampleMapRange;
   StatsSample* sampleMap;
   uint32 sampleMapMaxCapacity;
   uint32 sampleMapNumSamples;

   XMap_MPNRange callStacksMapRange;
   int* callStacksMap;
   uint32 callStacksMapMaxCapacity;
   uint32 callStacksMapNumStacks;

   XMap_MPNRange callStacksRange; 
   uint32* callStacks;
   int callStacksSize;
   int callStacksNextIndex;
} StatsData;

typedef struct {
   uint32 interrupts;
   uint32 samples;
   uint32 ignored;
   uint32 noimage;
   Timer_AbsCycles startTime;
} StatsMeta;

typedef enum { 
   STATS_OTHER_DATA_NONE,
   STATS_OTHER_DATA_WORLDID,
   STATS_OTHER_DATA_PCPU,
   STATS_OTHER_DATA_INT_ENABLED,
   STATS_OTHER_DATA_PREEMPTIBLE
} StatsOtherDataType;

typedef struct {
   uint32 startPC;
   uint32 endPC;
} StatsRoot;

/*
 * Globals
 */


// stats images (keep track of loaded modules)
static StatsImage *statsImage[STATS_MAX_IMAGES];
static int statsImageNext;

// per physical CPU stats buffers
static int statsInitialized = FALSE;
static StatsSampleBuffer statsSampleBuffers[MAX_PCPUS];

// stats data
static StatsData data;

// sorted array of all configured stats roots, aligned to fit in 
// a single cache line (along with numStatsRoots)
static StatsRoot statsRoots[STATS_MAX_ROOTS] __attribute__((aligned (16)));
static int numStatsRoots = 0;

// flags
static Bool statsIgnoreFlag = TRUE;
static StatsOtherDataType recordOtherData = STATS_OTHER_DATA_NONE;

// meta statistics
static StatsMeta statsTotal;
static StatsMeta statsEpoch;

// procfs entries
static Proc_Entry statsProcDir;
static Proc_Entry statsProcImagesDir;
static Proc_Entry statsProcCommand;
static Proc_Entry statsProcStatus;

// proc read ptrs
static int currentCallStacksProcReadIndex = STATS_INVALID_INDEX;
static char* currentSamplesProcReadPtr = NULL;

// bottom half handler number
static uint32 statsBHnum;

// event on which drainer world blocks
static uint32 statsWorldEvent;

/*
 * Local functions
 */

// generic ops
static void VMKStatsReset(void);
static VMK_ReturnStatus VMKStatsSamplerConfig(char *eventName, char *periodString);

// generic procfs
static int VMKStatsCommandProcWrite(Proc_Entry *entry, char *buf, int *len);
static int VMKStatsCommandProcRead(Proc_Entry *entry, char *buf, int *len);
static int VMKStatsStatusProcRead(Proc_Entry *entry, char *buf, int *len);

// per module info
static int VMKStatsImageLoadmapProcRead(Proc_Entry *entry, char *buf, int *len);
static int VMKStatsImageIdProcRead(Proc_Entry *entry, char *buf, int *len);
static void VMKStatsImageLoaded(char *modName,
                                uint64 imageId,
                                uint32 addr,
                                uint32 size,
                                uint32 initFunc,
                                uint32 cleanupFunc);
static StatsImage *VMKStatsImageCreate(char *modName,
                                       uint64 imageId,
                                       uint32 addr,
                                       uint32 size,
                                       uint32 initFunc,
                                       uint32 cleanupFunc);


// stats data
static int VMKStatsDataCallStacksProcRead(Proc_Entry *entry, char *buf, int *len);
static int VMKStatsDataCallStacksProcWrite(Proc_Entry *entry, char *buf, int *len);
static int VMKStatsDataSamplesProcRead(Proc_Entry *entry, char *buf, int *len);
static int VMKStatsDataSamplesProcWrite(Proc_Entry *entry, char *buf, int *len);

static VMK_ReturnStatus VMKStatsDrainBuffer(StatsSampleBuffer *sampleBuffer);
static Bool VMKStatsCheckRep(void);

// end of vmkernel text
extern uint32 _etext;

/*
 *----------------------------------------------------------------------
 *
 * VMKStatsFreeMem --
 *
 *	Free MemMap and XMap memory.
 *
 * Results:
 *	None. 
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void 
VMKStatsFreeMem(void **addr, XMap_MPNRange *range)
{
   ASSERT(range);
   ASSERT(addr);
   ASSERT(*addr);

   XMap_Unmap(range->numMPNs, *addr);
   MemMap_FreeKernelPages(range->startMPN);

   *addr = NULL;
   range->startMPN = INVALID_MPN;
   range->numMPNs = 0;
}

/*
 *----------------------------------------------------------------------
 *
 * VMKStatsAllocateMem --
 *
 *	MemMap and XMap memory.
 *
 * Results:
 *	Sets *addr to be XMap addr of memory, fills out range info. Returns
 *	VMK_OK if all went well, an approriate error otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
VMKStatsAllocateMem(uint32 size, void **addr, XMap_MPNRange *range)
{
   ASSERT(range);
   ASSERT(addr);

   range->numMPNs = CEIL(size, PAGE_SIZE);

   range->startMPN = MemMap_NiceAllocKernelPages(range->numMPNs,
						 MM_NODE_ANY,
						 MM_COLOR_ANY,
						 MM_TYPE_ANY);
   
   if (range->startMPN == INVALID_MPN) {
      Warning("insufficient physical pages for statistics");
      return VMK_NO_MEMORY;
   }

   *addr = XMap_Map(range->numMPNs, range, 1);
   if (*addr == NULL) {
      Warning("could not map memory for stats sample buffers");
      MemMap_FreeKernelPages(range->startMPN);
      range->startMPN = INVALID_MPN;
      return VMK_NO_ADDRESS_SPACE;
   }

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * VMKStatsDrainRequest --
 *
 *      Request that per-pcpu buffer associated with caller
 *	be drained into global stats data structures.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	May cause reschedule to occur.
 *
 *----------------------------------------------------------------------
 */
static void
VMKStatsDrainRequest(UNUSED_PARAM(void *unused))
{
   StatsSampleBuffer *s = &statsSampleBuffers[MY_PCPU];
   s->drainRequested = TRUE;
   CpuSched_Wakeup(statsWorldEvent);
}


/*
 *----------------------------------------------------------------------
 *
 * VMKStatsDrainWorldLoop --
 *
 *      Main loop executed by "vmkstats" drainer world.
 *	Processes pending requests to drain per-pcpu buffer into
 *	global stats data structures.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Blocks when no requests are pending.
 *
 *----------------------------------------------------------------------
 */
static void
VMKStatsDrainWorldLoop(UNUSED_PARAM(void *unused))
{
   ASSERT_HAS_INTERRUPTS();
   ASSERT(!CpuSched_IsPreemptible());

   // process drain requests 
   while (TRUE) {
      PCPU pcpu;

      for (pcpu = 0; pcpu < numPCPUs; pcpu++) {
         StatsSampleBuffer *s = &statsSampleBuffers[pcpu];

         Semaphore_Lock(&data.sem);
         if (s->drainRequested) {
            // drain buffer, ignore future samples if unable
            if (VMKStatsDrainBuffer(s) != VMK_OK) {
               statsIgnoreFlag = TRUE;
               Warning("unable to drain buffer for pcpu %u, "
                       "data collection suspended", pcpu);
            }
            s->drainRequested = FALSE;
         }
         Semaphore_Unlock(&data.sem);
      }

      // wait until kicked by drain request
      //   n.b. OK if racy, since NMI handler will keep kicking us
      CpuSched_Wait(statsWorldEvent, CPUSCHED_WAIT_REQUEST, NULL);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * VMKStatsDrainWorldCreate --
 *
 *      Create "vmkstats" system world as convenient context for
 *	performing VMKStats operations that may block.
 *
 * Results:
 *	Returns VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 *	Creates new "vmkstats" system world, and schedules it
 *	for execution.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
VMKStatsDrainWorldCreate(void)
{
   World_InitArgs args;
   Sched_ClientConfig sched;
   World_Handle *drainWorld;
   VMK_ReturnStatus status;

   // configure scheduling parameters
   Sched_ConfigInit(&sched, SCHED_GROUP_NAME_SYSTEM);
   World_ConfigArgs(&args, "vmkstats", WORLD_SYSTEM, WORLD_GROUP_DEFAULT, &sched);

   // create world
   status = World_New(&args, &drainWorld);
   if (status != VMK_OK) {
      return(status);
   }

   // start world running
   status = Sched_Add(drainWorld, VMKStatsDrainWorldLoop, NULL);
   if (status != VMK_OK) {
      return(status);
   }

   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * VMKStatsImageDestroy --
 *
 *      Destroy an image.
 *
 * Results: 
 *      None.
 *      
 * Side effects:
 *	Removes an image struct from the global array and frees memory.
 *      Removes procfs directory name and entries for code regions.
 *      Copies down any entries above the removed one in the global array.
 *
 *----------------------------------------------------------------------
 */
void
VMKStatsImageDestroy(char *modName)
{
   int i, j;

   if (VMKSTATS_DEBUG_VERBOSE) {
      LOG(0, "modName=%s, statsImageNext=%d", modName, statsImageNext);
   }

   i = j = 0;
   while (i < statsImageNext) {
      if (strncmp(statsImage[i]->modName, modName, VMNIX_MODULE_NAME_LENGTH) == 0) {
         Proc_Remove(&statsImage[i]->loadmap);
         Proc_Remove(&statsImage[i]->procId);
         Proc_Remove(&statsImage[i]->procDir);
         Mem_Free(statsImage[i]);
         statsImageNext--;
         j++;
      }
      statsImage[i++] = statsImage[j++];         
   }
}

/*
 *----------------------------------------------------------------------
 *
 * VMKStatsImageCreate --
 *
 *      Create a new statistics image object for sampled instructions
 *	in the address range [addr, addr + size].
 *
 * Results: 
 *	Returns a new, initialized StatsImage object.
 *      
 * Side effects:
 *      Registers procfs directory name and entries for code regions.
 *
 *----------------------------------------------------------------------
 */
static StatsImage *
VMKStatsImageCreate(char *modName,
                    uint64 imageId,
                    uint32 addr,
                    uint32 size,
                    uint32 initFunc,
                    uint32 cleanupFunc)
{
   StatsImage *image;

   // debugging
   if (VMKSTATS_DEBUG_VERBOSE) {
      LOG(0, "modName=%s, addr=%x, size=%x", modName, addr, size);
   }

   // allocate container, fail if unable
   image = (StatsImage *) Mem_Alloc(sizeof(StatsImage));
   if (image == NULL) {
      Warning("could not allocate StatsImage");
      return(NULL);
   }
   memset(image, 0, sizeof(StatsImage));
   
   // copy name
   strncpy(image->modName, modName, VMNIX_MODULE_NAME_LENGTH);

   // initialize
   image->imageId     = imageId;
   image->addr        = addr;
   image->size        = size;
   image->initFunc    = initFunc;
   image->cleanupFunc = cleanupFunc;
   
   // register procfs directory
   Proc_InitEntry(&image->procDir);
   image->procDir.parent = &statsProcImagesDir;
   Proc_Register(&image->procDir, modName, TRUE);

   // register procfs name
   Proc_InitEntry(&image->procId);
   image->procId.parent = &image->procDir;
   image->procId.read = VMKStatsImageIdProcRead;
   image->procId.private = image;
   Proc_Register(&image->procId, "id", FALSE);

   // register procfs loadmap
   Proc_InitEntry(&image->loadmap);
   image->loadmap.parent  = &image->procDir;
   image->loadmap.read = VMKStatsImageLoadmapProcRead;
   image->loadmap.private = image;
   Proc_Register(&image->loadmap, "loadmap", FALSE);

   // success
   return(image);
}

static void
VMKStatsImageLoaded(char *modName,
                    uint64 imageId,
                    uint32 addr,
                    uint32 size,
                    uint32 initFunc,
                    uint32 cleanupFunc)
{
   // update list of loaded images
   if (statsImageNext < STATS_MAX_IMAGES) {
      // create image, add to list if successful
      StatsImage *image = VMKStatsImageCreate(modName,
                                              imageId,
                                              addr,
                                              size,
                                              initFunc,
                                              cleanupFunc);
      if (image != NULL) {
         statsImage[statsImageNext++] = image;
         
         // debugging
         if (VMKSTATS_DEBUG_VERBOSE) {
            LOG(0, "modName=%s, imageId=%Lx, "
                "addr=%x, size=%u, init=%x, cleanup=%x\n",
                modName, imageId,
                addr, size, initFunc, cleanupFunc);
         }
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * VMKStats_Init --
 *
 *      Initialize the stats module.
 *
 * Results: 
 *      None.
 *      
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
VMKStats_Init(void)
{
   int i;
   
   // initialize flags
   statsIgnoreFlag = FALSE;

   // initialize event
   statsWorldEvent = (uint32) &statsWorldEvent;

   // initialize statistics
   memset(&statsTotal, 0, sizeof(statsTotal));
   memset(&statsEpoch, 0, sizeof(statsEpoch));

   // initialize images
   statsImageNext = 0;
   for (i = 0; i < STATS_MAX_IMAGES; i++) {
      statsImage[i] = NULL;
   }

   // initialize all fields of data to zero
   //   when the first sample is being recorded, we'll allocate space
   memset(&data, 0, sizeof(data));

   // initialize semaphore
   Semaphore_Init("vmkstats", &data.sem, 1, SEMA_RANK_LEAF);

   // register top-level "vmkstats" procfs directory
   Proc_InitEntry(&statsProcDir);
   Proc_RegisterHidden(&statsProcDir, "vmkstats", TRUE);

   // register top-level  "images" procfs directory
   Proc_InitEntry(&statsProcImagesDir);
   Proc_Register(&statsProcImagesDir, "images", TRUE);

   // register "command" procfs entry
   Proc_InitEntry(&statsProcCommand);
   statsProcCommand.parent = &statsProcDir;
   statsProcCommand.read  = VMKStatsCommandProcRead;
   statsProcCommand.write = VMKStatsCommandProcWrite;
   statsProcCommand.canBlock = TRUE;
   Proc_RegisterHidden(&statsProcCommand, "command", FALSE);

   // register "status" procfs entry
   Proc_InitEntry(&statsProcStatus);
   statsProcStatus.parent = &statsProcDir;
   statsProcStatus.read = VMKStatsStatusProcRead;
   Proc_RegisterHidden(&statsProcStatus, "status", FALSE);

   // register "callStacks" procfs entry
   Proc_InitEntry(&data.procCallStacks);
   data.procCallStacks.parent   = &statsProcDir;
   data.procCallStacks.read     = VMKStatsDataCallStacksProcRead;
   data.procCallStacks.write    = VMKStatsDataCallStacksProcWrite;
   Proc_RegisterHidden(&data.procCallStacks, "callStacks", FALSE);

   // register "samples" procfs entry
   Proc_InitEntry(&data.procSamples);
   data.procSamples.parent   = &statsProcDir;
   data.procSamples.read     = VMKStatsDataSamplesProcRead;
   data.procSamples.write    = VMKStatsDataSamplesProcWrite;
   Proc_RegisterHidden(&data.procSamples, "samples", FALSE);

   // initialize timestamps
   statsTotal.startTime = statsEpoch.startTime = Timer_GetCycles();

   // initialize vmkernel image
   VMKStatsImageLoaded("vmkernel", 0, VMK_TEXT_START, VMK_TEXT_SIZE, 0, 0);

   return;
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMKStats_LateInit --
 *
 *      Sets the default sampler to "unhalted_cycles" on a hyperthreaded box
 *      or "cycles" on non-hyperthreaded one.
 *
 * Results:
 *      Sets the default sampler.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
void
VMKStats_LateInit(void)
{
   VMK_ReturnStatus res;
   
   // setup the default sampler configuration
   if (SMP_HTEnabled()) {
      res = VMKStatsSamplerConfig("unhalted_cycles", NULL);
   } else {
      // works on both P4 and P3
      res = VMKStatsSamplerConfig("cycles", NULL);
   }

   ASSERT(res == VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * VMKStats_ModuleLoaded --
 *
 *      Update PC-sampling statistics collection to reflect newly loaded
 *      module "modName" mapped at ["baseAddr", "baseAddr" + "size"].
 *
 * Results: 
 *      None.
 *      
 * Side effects:
 *      May register procfs directory and entries for code regions.
 *
 *----------------------------------------------------------------------
 */
void
VMKStats_ModuleLoaded(char *modName,
                      uint64 imageId,
                      uint32 baseAddr,
                      uint32 size,
                      uint32 initFunc,
                      uint32 cleanupFunc)
{
   VMKStatsImageLoaded(modName, imageId,
                       baseAddr, size, initFunc, cleanupFunc);
}

/*
 *----------------------------------------------------------------------
 *
 * VMKStats_ModuleUnloaded --
 *
 *      Update PC-sampling statistics collection to reflect an unloaded
 *      module "modName".
 *
 * Results: 
 *      None.
 *      
 * Side effects:
 *      Unregisters procfs directory and entries for code regions, frees
 *      memory.
 *
 *----------------------------------------------------------------------
 */
void
VMKStats_ModuleUnloaded(char *modName)
{
   if (statsInitialized) {
      Warning("unloading module %s with vmkstats initialized", modName);
   }
   VMKStatsImageDestroy(modName);
}

/*
 *----------------------------------------------------------------------
 *
 * VMKStatsDataCallStacksProcWrite --
 *
 *      Resets the read ptr for the data call stacks node
 *
 * Results: 
 *      Returns 0
 *      
 * Side effects:
 *	updates currentCallStacksProcReadIndex
 *
 *----------------------------------------------------------------------
 */
static int
VMKStatsDataCallStacksProcWrite(Proc_Entry *entry, char *buffer, int *len) 
{
   int byteToSeekTo;
   if (Parse_Int(buffer, *len, &byteToSeekTo) == VMK_OK) {
      if (byteToSeekTo % 4 != 0) {
	 Warning("invalid address, not word aligned");
	 return(VMK_BAD_PARAM);
      }
      currentCallStacksProcReadIndex = byteToSeekTo / 4;
      return(VMK_OK);
   } else {
      return(VMK_BAD_PARAM);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * VMKStatsDataCallStacksProceRead --
 *
 *      Read out the call stacks array from the
 *      currentCallStacksProcReadIndex.  If the read ptr is at the end
 *      already, return no data
 *
 * Results: 
 *	Returns 0 iff successful.
 *      
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
VMKStatsDataCallStacksProcRead(Proc_Entry *entry, char *buffer, int *len) 
{
   *len = 0;
   
   if (!statsIgnoreFlag) {
      Proc_Printf(buffer, len, "Error: vmkstats is running, stop it before reading\n");
      return(VMK_FAILURE);
   }

   if (currentCallStacksProcReadIndex > data.callStacksNextIndex) {
      Proc_Printf(buffer, len, "Error: trying to read past end of Call Stacks\n");
      return(VMK_BAD_PARAM);
   } else if (currentCallStacksProcReadIndex > data.callStacksNextIndex) {
      return(VMK_OK);
   } else if (currentCallStacksProcReadIndex < 0) {
      Proc_Printf(buffer, len, 
                  "Error: Invalid read location.. write the offset "
                  "to this node before reading.\n");
      return(VMK_BAD_PARAM);
   }

   *len = MIN(VMNIX_PROC_READ_LENGTH, 
	      sizeof(uint32) * (data.callStacksNextIndex - currentCallStacksProcReadIndex));

   memcpy(buffer, data.callStacks + currentCallStacksProcReadIndex, *len);
   
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * VMKStatsDataSamplesProcWrite --
 *
 *      Resets the data samples read ptr
 *
 * Results: 
 *	Returns VMK_OK iff successful.
 *      
 * Side effects:
 *	Updates currentSamplesProcReadPtr
 *
 *----------------------------------------------------------------------
 */
static int
VMKStatsDataSamplesProcWrite(Proc_Entry *entry, char *buffer, int *len) 
{   
   int readVal;
   if (Parse_Int(buffer, *len, &readVal) == VMK_OK) {
      currentSamplesProcReadPtr = (char*)data.sampleMap + readVal;
      return(VMK_OK);
   } else {
      Warning("invalid offset");
      return(VMK_BAD_PARAM);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * VMKStatsDataSamplesProcRead --
 *
 *      Read out the samples map from the currentSamplesProcReadPtr.
 *      If the read ptr is at the end already, return no data
 *
 * Results: 
 *	Returns VMK_OK iff successful.
 *      
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
VMKStatsDataSamplesProcRead(Proc_Entry *entry, char *buffer, int *len) 
{
   *len = 0;

   if (!statsIgnoreFlag) {
      Proc_Printf(buffer, len, "Error: vmkstats is running, stop it before reading");
      return(VMK_FAILURE);
   }

   if (currentSamplesProcReadPtr > (char*) (data.sampleMap + data.sampleMapMaxCapacity)) {
      Proc_Printf(buffer, len, "Error: Trying to read past the end of samplemap");
      return(VMK_FAILURE);
   } else if (currentSamplesProcReadPtr == (char*) (data.sampleMap + data.sampleMapMaxCapacity)) {
      return(VMK_OK);
   } else if (currentSamplesProcReadPtr < (char*) data.sampleMap) {
      Proc_Printf(buffer, len, 
                  "Error: Invalid read location.. write the offset "
                  "to this node before reading.\n");
      return(VMK_FAILURE);
   }

   *len = MIN(VMNIX_PROC_READ_LENGTH, 
	      (char*) (data.sampleMap + data.sampleMapMaxCapacity) - currentSamplesProcReadPtr);

   memcpy(buffer, currentSamplesProcReadPtr, *len);

   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * VMKStatsImageIdProcRead --
 *
 *      Report image identity info associated with statistics image.
 *
 * Results: 
 *      Writes ASCII identity information for image.
 *      Sets "length" to the number of bytes written.
 *	Returns VMK_OK iff successful.
 *      
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
VMKStatsImageIdProcRead(Proc_Entry *entry, char *buffer, int *len)
{
   StatsImage *image = (StatsImage *) entry->private;
   char *buildType = "unknown";

   // determine build type
#if	defined(VMX86_RELEASE)
   buildType = "release";
#elif	defined(VMX86_ALPHA)
   buildType = "alpha";
#elif	defined(VMX86_BETA)
   buildType = "beta";
#elif	defined(VMX86_DEVEL) && !defined(VMX86_DEBUG)
   buildType = "opt";
#elif	defined(VMX86_DEVEL) && defined(VMX86_DEBUG)
   buildType = "obj";
#endif

   //initialize
   *len = 0;

   // format identity info
   Proc_Printf(buffer, len,
               "build %s\n"
               "file  %s\n"
               "id    %Lx\n",
               buildType,
               image->modName,
               image->imageId);
   
   // success
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * VMKStatsImageLoadmapProcRead --
 *
 *      Report loadmap associated with statistics image.
 *
 * Results: 
 *      Writes ASCII representation of loadmap for image.
 *      Sets "length" to the number of bytes written.
 *	Returns VMK_OK iff successful.
 *      
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
VMKStatsImageLoadmapProcRead(Proc_Entry *entry, char *buffer, int *len)
{
   StatsImage *image = (StatsImage *) entry->private;
   *len = 0;

   // format loadmap info
   Proc_Printf(buffer, len,
	       "%08x base\n"
	       "%08x size\n"
	       "%08x init\n"
	       "%08x cleanup\n",
	       image->addr,
	       image->size,
	       image->initFunc,
	       image->cleanupFunc);

   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * VMKStatsCallStacksEqual --
 *
 * Results: 
 *      Returns TRUE if callstacks are equal
 *      
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static Bool
VMKStatsCallStacksEqual(CallStack* a, CallStack* b) 
{
   if (a->length != b->length) {
      return(FALSE);
   }

   if (memcmp(a->stack, b->stack, sizeof(uint32) * a->length) == 0) {
      return(TRUE);
   } else {
      return(FALSE);
   }
}

static INLINE uint64
VMKStatsHashSample(uint32 eip, int callStackIndex, uint32 otherData) 
{
   return (callStackIndex ^ otherData) | ((uint64) eip << 32);
}

static INLINE uint64
VMKStatsRehash(uint64 oldHashVal, int numConflicts) 
{
   // really... something better than this should be done... but here is 
   // my version of a rehash function.
   if (numConflicts < 64) {
      return (oldHashVal << 1) | (oldHashVal >> 63);
   } else {
      return (oldHashVal + 1);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * VMKStatsAllocMoreSampleMap --
 *      
 *      Allocates a larger sample map and copies all of the sample data
 *      frome the old map into the new one.
 *
 * Results: 
 *      Returns VMK_OK iff successful
 *      
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
VMKStatsAllocMoreSampleMap(void) 
{
   VMK_ReturnStatus status; 
   XMap_MPNRange newSampleMapRange;
   StatsSample* newSampleMap;
   int newSampleMapMaxCapacity;
   int i;

   if (data.sampleMapMaxCapacity == 0) {
      newSampleMapMaxCapacity = STATS_INITIAL_SAMPLE_MAP_COUNT;
   } else {
      newSampleMapMaxCapacity = (data.sampleMapMaxCapacity * STATS_SAMPLE_MAP_GROW_PERCENT) / 100;
   }
   
   ASSERT(newSampleMapMaxCapacity > data.sampleMapMaxCapacity);

   status = VMKStatsAllocateMem(newSampleMapMaxCapacity * sizeof(StatsSample),
   				(void**)&newSampleMap,
				&newSampleMapRange);

   if (status != VMK_OK) {
      Warning("could not allocate memory for larger sample map");
      return status;
   }
   memset(newSampleMap, 0, newSampleMapMaxCapacity * sizeof(StatsSample));
   
   for (i = 0; i < data.sampleMapMaxCapacity; i++) {
      if (data.sampleMap[i].count != 0) {
	 uint64 hashVal = VMKStatsHashSample(data.sampleMap[i].eip,
                                             data.sampleMap[i].callStackIndex, 
                                             data.sampleMap[i].otherData);
	 int numConflicts = 0;
	 while (1) {
	    StatsSample* newSample = &newSampleMap[hashVal % newSampleMapMaxCapacity];
	    if (newSample->count == 0) {
	       *newSample = data.sampleMap[i];
	       break;
	    } else {
	       numConflicts++;
	       hashVal = VMKStatsRehash(hashVal, numConflicts);
	    }
	 }
      }
   }

   if (data.sampleMapMaxCapacity != 0) {
      VMKStatsFreeMem((void**)&data.sampleMap, &data.sampleMapRange);
   }
   data.sampleMapRange = newSampleMapRange;
   data.sampleMap = newSampleMap;
   data.sampleMapMaxCapacity = newSampleMapMaxCapacity;

   ASSERT(VMKStatsCheckRep());

   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * VMKStatsIncSample --
 *      
 *      Increments the sample given by eip and callStackIndex, or add the
 *      sample to the sample map with count 1.
 *
 * Results: 
 *      returns TRUE iff successful
 *      
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static Bool
VMKStatsIncSample(uint32 eip, int callStackIndex, uint32 otherData) 
{
   uint64 hashVal = VMKStatsHashSample(eip, callStackIndex, otherData);
   int numConflicts = 0;
   // check if we need to allocate a larger hash map
   if (data.sampleMapNumSamples >= ((data.sampleMapMaxCapacity * STATS_MAX_HASH_FILL_PERCENT) / 100)) {
      if (VMKStatsAllocMoreSampleMap() != VMK_OK) {
	 return FALSE;
      }
   }

   while (1) {
      StatsSample* sample = &data.sampleMap[hashVal % data.sampleMapMaxCapacity];
      if ((eip == sample->eip) && 
          (callStackIndex == sample->callStackIndex) &&
          (otherData == sample->otherData)) {
	 sample->count++;
	 break;
      } else if (sample->count == 0) {
	 data.sampleMapNumSamples++;
	 sample->eip = eip;
	 sample->callStackIndex = callStackIndex;
         sample->otherData = otherData;
	 sample->count = 1;
	 break;
      } else {
	 numConflicts++;
	 hashVal = VMKStatsRehash(hashVal, numConflicts);
      }
   }
   return TRUE;
}

/*
 *----------------------------------------------------------------------
 *
 * VMKStatsAllocMoreCallStacks --
 *      
 *      Allocates a larger call stacks array and copies all of the call
 *      stacks from the old array into the new one
 *
 * Results: 
 *      Returns VMK_OK iff successful
 *      
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
VMKStatsAllocMoreCallStacks(void) 
{
   VMK_ReturnStatus status; 
   XMap_MPNRange newCallStacksRange;
   uint32* newCallStacks;
   int newCallStacksSize;

   ASSERT(data.callStacksNextIndex <= data.callStacksSize / sizeof(uint32));

   // check if we need to allocate more call stacks array
   if (data.callStacksSize == 0) {
      newCallStacksSize = STATS_INITIAL_CALL_STACKS_SIZE;
   } else {
      newCallStacksSize = (data.callStacksSize * STATS_CALL_STACKS_GROW_PERCENT) / 100;
   }
 
   status = VMKStatsAllocateMem(newCallStacksSize,
   				(void**)&newCallStacks,
				&newCallStacksRange);

   if (status != VMK_OK) {
      Warning("could not allocate memory for larger call stacks array");
      return status;
   }
   memcpy(newCallStacks, data.callStacks, data.callStacksNextIndex * sizeof(uint32));

   if (data.callStacksSize != 0) {
      VMKStatsFreeMem((void**)&data.callStacks, &data.callStacksRange); 
   }
   data.callStacksRange = newCallStacksRange;
   data.callStacks = newCallStacks;
   data.callStacksSize = newCallStacksSize;

   ASSERT(VMKStatsCheckRep());

   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * VMKStatsInsertCallStack --
 *      
 *      Inserts a call stack into the call stacks array.
 *
 * Results: 
 *      Returns the index of the inserted call stack, or 
 *	STATS_INVALID_INDEX if unsuccessful
 *      
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
VMKStatsInsertCallStack(CallStack* callStack) 
{
   int newCallStackIndex;
   CallStack* newCallStackLocation;

   ASSERT(callStack->length <= STATS_MAX_CALL_DEPTH);

   // check for overflow
   if (data.callStacksNextIndex + 1 + STATS_MAX_CALL_DEPTH >= data.callStacksSize / sizeof(uint32)) {
      LOG(1,"ran out of call stack room.. allocating a bigger array "
          "currentIndex=%d, callStacks=0x%x, callStacksSize=%u",
          (uint32) data.callStacksNextIndex,
          (uint32) data.callStacks,
          data.callStacksSize);
      if (VMKStatsAllocMoreCallStacks() != VMK_OK) {
	 return(STATS_INVALID_INDEX);
      }
   }

   newCallStackIndex = data.callStacksNextIndex;
   newCallStackLocation = (CallStack*) (data.callStacks + newCallStackIndex);
   ASSERT(((char*)newCallStackLocation >= (char*)data.callStacks) &&
          ((char*)newCallStackLocation < ((char*)data.callStacks) + data.callStacksSize));

   newCallStackLocation->length = callStack->length;
   memcpy(newCallStackLocation->stack, callStack->stack, callStack->length * sizeof(uint32));
   
   ASSERT(VMKStatsCallStacksEqual(callStack, newCallStackLocation));

   data.callStacksNextIndex += 1 + callStack->length;

   return(newCallStackIndex);
}

static INLINE uint64
VMKStatsHashCallStack(CallStack* cs) {
   uint64 hashVal = Hash_Bytes((char*) cs->stack, (cs->length) * sizeof(uint32));
   return(hashVal);
}

/*
 *----------------------------------------------------------------------
 *
 * VMKStatsAllocMoreCallStackMap --
 *      
 *      Allocates a larger call stack map and copies all of the call
 *      stack entries from the old map into the new one
 *
 * Results: 
 *      Returns VMK_OK iff successful
 *      
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
VMKStatsAllocMoreCallStackMap(void)
{
   VMK_ReturnStatus status; 
   XMap_MPNRange newCallStacksMapRange;
   int* newCallStacksMap;
   int newCallStacksMapMaxCapacity;
   int i;
   
   if (data.callStacksMapMaxCapacity == 0) {
      newCallStacksMapMaxCapacity = STATS_INITIAL_CALL_STACKS_MAP_COUNT;
   } else {
      newCallStacksMapMaxCapacity = (data.callStacksMapMaxCapacity * STATS_CALL_STACKS_MAP_GROW_PERCENT) / 100;
   }

   ASSERT(newCallStacksMapMaxCapacity > data.callStacksMapMaxCapacity);
   
   status = VMKStatsAllocateMem(newCallStacksMapMaxCapacity * sizeof(int),
   				(void**)&newCallStacksMap,
				&newCallStacksMapRange);

   if (status != VMK_OK) {
      Warning("could not allocate memory for larger call stacks hash");
      return status;
   }
   memset(newCallStacksMap, -1, newCallStacksMapMaxCapacity * sizeof(int));

   for (i = 0; i < data.callStacksMapMaxCapacity; i++) {
      if (data.callStacksMap[i] >= 0) {
         CallStack* callStack = (CallStack*) (data.callStacks + data.callStacksMap[i]);
	 uint64 hashVal = VMKStatsHashCallStack(callStack);
         int numConflicts = 0;

         ASSERT(data.callStacks + data.callStacksMap[i] == &data.callStacks[data.callStacksMap[i]]);

	 while (1) {
	    int* newCallStackIndex = &newCallStacksMap[hashVal % newCallStacksMapMaxCapacity];
	    if (*newCallStackIndex < 0) {
	       *newCallStackIndex = data.callStacksMap[i];
	       break;
	    } else {
               ASSERT(*newCallStackIndex != data.callStacksMap[i]); //each entry should only be in the map once

               ASSERT(VMKStatsCallStacksEqual(callStack, (CallStack*) (data.callStacks + *newCallStackIndex)) == FALSE);

               numConflicts++;
	       hashVal = VMKStatsRehash(hashVal, numConflicts);
               ASSERT(numConflicts < data.callStacksMapMaxCapacity);
	    }
	 }
      }
   }

   if (data.callStacksMapMaxCapacity != 0) {
      VMKStatsFreeMem((void**)&data.callStacksMap, &data.callStacksMapRange);
   }
   data.callStacksMapRange = newCallStacksMapRange;
   data.callStacksMap = newCallStacksMap;
   data.callStacksMapMaxCapacity = newCallStacksMapMaxCapacity;

   if (!VMKStatsCheckRep()) {
      Warning("checkrep failed!");
      return(VMK_FAILURE);
   }

   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * VMKStatsCheckRep --
 *      
 *      Checks the internel representation of the data.  Makes sure that
 *      our counts of entries are correct, and that maps are pointing at
 *      valid things.
 *
 * Results: 
 *      Returns TRUE if no errors were found
 *      
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static Bool VMKStatsCheckRep() {
   Bool retVal = TRUE;

   int j = 0;
   int currentCount = 0;
   int numMaxedCS = 0;

   while (j < data.callStacksNextIndex) {
      CallStack* cs = (CallStack*) (data.callStacks + j);
      uint64 hashVal;
      int numConflicts = 0;
      if ((cs->length < 0) ||
          (cs->length > STATS_MAX_CALL_DEPTH)) {
         LOG(0,"call stack at offset %d has invalid length (%d)",
             j, cs->length);
         retVal = FALSE;
         break;
      } 

      if (cs->length == STATS_MAX_CALL_DEPTH) {
         numMaxedCS++;
      }

      hashVal = VMKStatsHashCallStack(cs);
      while (1) {
         int index = data.callStacksMap[hashVal % data.callStacksMapMaxCapacity];
         if (index == j) {
            break;
         } else if (index < 0) {
            LOG(0,"call stack not mapped correctly, csOffset = %d, %d",
                j, index);
            retVal = FALSE;
            goto doneCallStacks;
         } 
	 numConflicts++;
	 hashVal = VMKStatsRehash(hashVal, numConflicts);
      }
      j += 1 + cs->length;
   }
 doneCallStacks:
   currentCount = 0;
   for (j=0; j < data.callStacksMapMaxCapacity; j++) {
      int csIndex = data.callStacksMap[j];
      if (csIndex >= 0) {
         currentCount++;
         if (csIndex >= data.callStacksNextIndex) {
            LOG(0,"Invalid callstacks map index, too high. index = %d, callStacksNextIndex = %d",
                csIndex, data.callStacksNextIndex);
            retVal = FALSE;
            break;
         }
      }
   }
   if (currentCount != data.callStacksMapNumStacks) {
      LOG(0,"count in call stack map is wrong, callStacksMapNumStacks = %d, actual count = %d",
          data.callStacksMapNumStacks,
          currentCount);
      retVal = FALSE;
   }

   currentCount = 0;
   for (j=0; j < data.sampleMapMaxCapacity; j++) {
      StatsSample* sample = &data.sampleMap[j];
      if (sample->count > 0) {
         currentCount++;
         if ((sample->callStackIndex >= data.callStacksNextIndex) ||
             (sample->callStackIndex < 0)) {
            LOG(0,"Invalid sample call stack index %d (eip = 0x%x)",
                sample->callStackIndex, sample->eip);
            retVal = FALSE;
            break;
         }
      }
   }
   if (currentCount != data.sampleMapNumSamples) {
      LOG(0,"count in sample map is wrong, sampleMapNumSamples = %d, actual count = %d",
          data.sampleMapNumSamples,
          currentCount);
      retVal = FALSE;
   }

   return(retVal);
}

/*
 *----------------------------------------------------------------------
 *
 * VMKStatsFindInsertCallStack --
 *      
 *      Checks to see if callStack has already been seen (if it is in 
 *      callstackmap and the call stacks array), if so, return its index
 *      otherwise, add it and return the index of the added entry.
 *
 * Results: 
 *      Returns index of callStack, or STATS_INVALID_INDEX if it fails.
 *      
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
VMKStatsFindInsertCallStack(CallStack* callStack) 
{
   uint64 hashVal = VMKStatsHashCallStack(callStack);
   int numConflicts = 0;

   if (data.callStacksMapNumStacks >= ((data.callStacksMapMaxCapacity * STATS_MAX_HASH_FILL_PERCENT) / 100)) {
      if (VMKStatsAllocMoreCallStackMap() != VMK_OK) {
         return(STATS_INVALID_INDEX);
      }
   }
   
   while (1) {
      int* possibleMatchAddr = &data.callStacksMap[hashVal % data.callStacksMapMaxCapacity];
      if (*possibleMatchAddr < 0) {
	 *possibleMatchAddr = VMKStatsInsertCallStack(callStack);
         if (*possibleMatchAddr < 0) {
            LOG(0, "error adding new call Stack");
            return(STATS_INVALID_INDEX);
         }
	 data.callStacksMapNumStacks++;
	 return(*possibleMatchAddr);
      } else if (VMKStatsCallStacksEqual((CallStack*) (&data.callStacks[*possibleMatchAddr]), callStack) == TRUE) {
	 return(*possibleMatchAddr);
      } else {
	 //rehash?  could do better than inc.
	 numConflicts++;
	 hashVal = VMKStatsRehash(hashVal, numConflicts);
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * VMKStatsDrainBuffer --
 *      
 *      Drain "sampleBuffer", adding its samples to the stats data 
 *      structures.  If an error is encountered, stops the recording
 *	and draining of stats.  Caller must hold global "data.sem".
 *
 * Results: 
 *      None.
 *      
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
VMKStatsDrainBuffer(StatsSampleBuffer *sampleBuffer)
{
   int entriesDrained = 0;

   // sanity check
   ASSERT(Semaphore_IsLocked(&data.sem));

   if (sampleBuffer->stalledOnWrite) {
      LOG(1, "sample buffer was stalled waiting to be drained");
      // could result in error being reported multiple times... 
      sampleBuffer->stalledOnWrite = FALSE;
   }

   while (sampleBuffer->get != sampleBuffer->put) {
      StatsQuickSample* quickSample;

      quickSample = (StatsQuickSample*) &sampleBuffer->buffer[sampleBuffer->get];

      if (entriesDrained > STATS_SAMPLE_BUFFER_COUNT) {
         Warning("excessive drain count: get=%d put=%d size=%d", 
		 sampleBuffer->get, sampleBuffer->put, sampleBuffer->entries);
         return(VMK_LIMIT_EXCEEDED);
      }

      if (quickSample->eip == 0) {
         LOG(0, "error: recorded eip of 0");
      }

      ASSERT(quickSample->callStack.length <= STATS_MAX_CALL_DEPTH);

      if (VMK_IsVMKEip(quickSample->eip)) {
	 //the eip is not from the console os... record it
	 int insertedCallStackIndex;
	 insertedCallStackIndex = VMKStatsFindInsertCallStack(&quickSample->callStack);
	 if (insertedCallStackIndex < 0) {
	    Warning("error inserting call stack");
            return(VMK_FAILURE);
	 }
	 
	 if (VMKStatsIncSample(quickSample->eip, insertedCallStackIndex, quickSample->otherData) == FALSE) {
	    Warning("error inserting sample");
            return(VMK_FAILURE);
	 }
      }

      // now that we are done with the data, we can inc the get pointer
      sampleBuffer->get += sizeof(StatsQuickSample) / sizeof(uint32) + quickSample->callStack.length;
      if (sampleBuffer->get > sampleBuffer->maxSafePut) {
         sampleBuffer->get = 0;
      }
      entriesDrained++;
   }

   // successful drain
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * VMKStatsVerifyStackAddr --
 *
 *      Verify that an address is a valid stack address for the current 
 *      world.  This needs to be called because the stack could have
 *      pointers to unmapped memory, or we might walk up the stack, and 
 *      then keep going past the top of the stack.  We check addr and 
 *      addr+4 because addr contains the new ebp and addr+4 has the
 *      callers eip.
 *
 * Results:
 *      TRUE if addr (and addr + 4) are valid stack addresses.
 *
 * Side Effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static Bool 
VMKStatsVerifyStackAddr(VA addr)
{
   if (CpuSched_IsHostWorld()) {
      return ((addr >= VMK_HOST_STACK_BASE) && 
              (addr < VMK_HOST_STACK_TOP - 8));
   } else {
      return ((addr >= MY_RUNNING_WORLD->vmkStackStart) &&
              (addr < (MY_RUNNING_WORLD->vmkStackStart + MY_RUNNING_WORLD->vmkStackLength-8)));
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * VMKStatsRootSorter --
 *
 *     Utility function, used by heapsort to compare two StatsRoot*s
 *     based on their startPC fields
 *
 * Results:
 *     Returns 1 if a is greater, -1 if b is greater, 0 if they're equal
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static int
VMKStatsRootSorter(const void *a, const void *b)
{
   StatsRoot *rootA, *rootB;
   rootA = (StatsRoot*) a;
   rootB = (StatsRoot*) b;

   if (rootA->startPC < rootB->startPC) {
      return (-1);
   } else if (rootA->startPC > rootB->startPC) {
      return (1);
   } else {
      return (0);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMKStatsAddRoot --
 *
 *     Inserts a new root into the roots list and resorts the list.
 *     The new root represents a function that starts at the address "startPC"
 *     and ends just before endPC.
 *
 * Results:
 *     Returns VMK_OK on success, VMK_FAILURE otherwise
 *
 * Side effects:
 *     Updates statsRoots list.
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
VMKStatsAddRoot(uint32 startPC, uint32 endPC)
{
   StatsRoot temp;

   if (numStatsRoots >= STATS_MAX_ROOTS) {
      Warning("limit on number of stats roots already reached");
      return (VMK_FAILURE);
   }
   
   // note that we don't lock here, as we know the sampler isn't running
   statsRoots[numStatsRoots].startPC = startPC;
   statsRoots[numStatsRoots].endPC = endPC;
   numStatsRoots++;

   // keep this list sorted by "startPC"
   heapsort(statsRoots, numStatsRoots, sizeof(StatsRoot), VMKStatsRootSorter, &temp);

   LOG(0, "added root: 0x%08x:0x%08x, %d roots configured", startPC, endPC, numStatsRoots);
   return (VMK_OK);
}

/*
 *-----------------------------------------------------------------------------
 *
 * VMKStatsRemoveRoot --
 *
 *     Removes the root for the function starting at startPC and ending just
 *     before endPC.
 *
 * Results:
 *     Returns VMK_OK on success, VMK_NOT_FOUND if the specified root could
 *     not be found.
 *
 * Side effects:
 *     Updates statsRoots lists.
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
VMKStatsRemoveRoot(uint32 startPC, uint32 endPC)
{
   StatsRoot tmp;
   int i;

   ASSERT(myPRDA.configNMI != NMI_USING_SAMPLER);

   for (i=0; i < numStatsRoots; i++) {
      if (statsRoots[i].startPC == startPC &&
          statsRoots[i].endPC == endPC) {
         // found a match, remove it and swap it with the 
         // last root in the list
         tmp = statsRoots[numStatsRoots];
         statsRoots[i] = tmp;
         numStatsRoots--;

         // keep this list sorted by "startPC"
         heapsort(statsRoots, numStatsRoots, sizeof(StatsRoot), VMKStatsRootSorter, &tmp);

         return (VMK_OK);
      }
   }

   return (VMK_NOT_FOUND);
}

/*
 *-----------------------------------------------------------------------------
 *
 * VMKStatsRemoveAllRoots --
 *
 *     Unregisters all configured stats roots
 *
 * Results:
 *     None.
 *
 * Side effects:
 *     Unregisters all configured stats roots
 *
 *-----------------------------------------------------------------------------
 */
static void
VMKStatsRemoveAllRoots(void)
{
   ASSERT(myPRDA.configNMI != NMI_USING_SAMPLER);
   numStatsRoots = 0;
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMKStatsIsRootPC --
 *
 *     Returns TRUE iff "pc" is within a root function.
 *     We should not continue walking the stack beyond a root function
 *     as it represents a logical entry point to the vmkernel, for example
 *     an interrupt handler or a vmkcall.
 *
 * Results:
 *     Returns TRUE iff "pc" is within a root function.
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE Bool
VMKStatsIsRootPC(uint32 pc)
{
   int i;

   // search through the sorted list of stats roots
   // could do binary search if these lists get large
   for (i=0; i < numStatsRoots; i++) {
      if (pc < statsRoots[i].startPC) {
         // we've gone too far already
         break;
      } else if (pc < statsRoots[i].endPC) {
         // greater than start but less than end => match!
         return (TRUE);
      }
   }

   // ran off end of list without finding anything
   return (FALSE);
}


/*
 *----------------------------------------------------------------------
 *
 * VMKStats_*Mode --
 *
 *      Placeholder symbols for samples taken in contexts where we
 *      don't have symbols.  Never actually called.
 *
 * Results: 
 *      None.
 *      
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void VMKStats_UserMode(void) { return; }
void VMKStats_COSUserMode(void) { return; }
void VMKStats_COSKernelMode(void) { return; }


/*
 *----------------------------------------------------------------------
 *
 * VMKStatsGetCallStack --
 *
 *      Walk the stack backwards, recording the eips that called us.
 *
 * Results: 
 *      The depth of the call stack (how many entries we recorded)
 *      
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
VMKStatsGetCallStack(const NMIContext *nmiContext, uint32* callStack)
{
   int length = 0;
   Bool ebpTweaked = FALSE;
   uint32 *ebp = (uint32*)nmiContext->ebp;

   /*
    * Only works on VMKernel stacks.  COS stacks and UserWorld stacks
    * are not safe to walk from an NMI handler.
    */
   if (nmiContext->source != NMI_FROM_VMKERNEL) {
      return 0;
   }

   /* We might have just called a function, but not pushed ebp and
      moved esp to ebp. If we don't take action, we'll be missing a
      stack frame! */
   if (*(uint8*)nmiContext->eip == 0x55) {
      /* We were about to execute a push %ebp instruction.  We're
         most likely at the beginning of a function. While this may
         occur in context switch code, it's harmless to grab the
         base pointer, since it will be range checked before
         use. */
      ebpTweaked = TRUE;
      ebp = (uint32*)(nmiContext->esp - 4);
   }
   if (*(uint16*)nmiContext->eip == 0xe589) {
      /* We were about to execute a mov %esp, %ebp instruction.
         We're most likely at the beginning of a function. In fact,
         I can't think of any other situation when you'd do this,
         but even if there were one it still can't hurt because
         we'll range check everything before use. */
      ebp = (uint32*)(nmiContext->esp);
   }
   if (*(uint8*)nmiContext->eip == 0xc3 &&
       *(uint8*)(nmiContext->eip - 1) == 0x5d) {
      /* Unless we jumped to a return or something, we just
         executed a pop ebp, and we're about to execute a
         return instruction. We need to restore our copy of
         ebp to point just below the return address. */
      ebpTweaked = TRUE;
      ebp = (uint32*)(nmiContext->esp - 4);
   }
   
   while (length < STATS_MAX_CALL_DEPTH) {
      uint32 pc;
      
      if (!VMKStatsVerifyStackAddr((uint32) ebp)) {
         break;
      }
      
      pc = *((uint32*) (((VA) ebp) + 4));  //get the PC of who called us

      /* If the EIP is not reasonable then stop the stack trace. */
      if ((nmiContext->source == NMI_FROM_VMKERNEL) && !VMK_IsVMKEip(pc)) {
         break;
      }
      
      *callStack++ = pc;
      if (!ebpTweaked) {
         ebp = (uint32*) *ebp;
      } else {
         ebp = (uint32*) nmiContext->ebp;
         ebpTweaked = FALSE;
      }
      length++;

      if (VMKStatsIsRootPC(pc)) {
         break;
      }
   }

   return length; 
}


/*
 *----------------------------------------------------------------------
 *
 * VMKStats_Sample --
 *
 *      Called from the nmi.  Records a sample in the per-cpu sample 
 *      buffers.
 *
 * Results: 
 *      None.
 *      
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
VMKStats_Sample(NMIContext *nmiContext)
{
   PCPU pcpu = MY_PCPU;
   uint32 eip = nmiContext->eip;

   if (statsInitialized == FALSE) {
      return;
   }
   
   // update statistics
   statsTotal.interrupts++;
   statsEpoch.interrupts++;

   // ignore sample, if specified
   if (statsIgnoreFlag) {
      statsTotal.ignored++;
      statsEpoch.ignored++;
      return;
   }

   // update statistics
   statsTotal.samples++;
   statsEpoch.samples++;

   StatsSampleBuffer* sampleBuffer = &statsSampleBuffers[pcpu];
   int roomLeft;
   if (sampleBuffer->put >= sampleBuffer->get) {
      roomLeft = sampleBuffer->maxSafePut + sampleBuffer->get - sampleBuffer->put;
   } else {
      roomLeft = sampleBuffer->get - sampleBuffer->put;
   }

   //schedule us for draining if we are more than half filled
   if (roomLeft < STATS_SAMPLE_BUFFER_COUNT / 2) {
      BH_SetLocalPCPU(statsBHnum);
   }

   if (roomLeft < sizeof(StatsQuickSample) / sizeof(uint32) + STATS_MAX_CALL_DEPTH) {
      sampleBuffer->stalledOnWrite = TRUE;
   } else {
      StatsQuickSample* quickSample;
      quickSample = (StatsQuickSample*) &sampleBuffer->buffer[sampleBuffer->put];
      switch (nmiContext->source) {
      case NMI_FROM_USERMODE:
         quickSample->eip = (uint32)VMKStats_UserMode;
         break;
      case NMI_FROM_COS:
         quickSample->eip = (uint32)VMKStats_COSKernelMode;
         break;
      case NMI_FROM_COS_USER:
         quickSample->eip = (uint32)VMKStats_COSUserMode;
         break;
      case NMI_FROM_VMKERNEL:
         quickSample->eip = eip;
         break;
      default:
         Panic("Undefined NMI source (%d)", nmiContext->source);
         break;
      }

      switch (recordOtherData) {
      case STATS_OTHER_DATA_NONE:
         quickSample->otherData = 0;
         break;
      case STATS_OTHER_DATA_WORLDID:
         quickSample->otherData = MY_RUNNING_WORLD->worldID;
         break;
      case STATS_OTHER_DATA_PCPU:
         quickSample->otherData = pcpu;
         break;
      case STATS_OTHER_DATA_INT_ENABLED:
         quickSample->otherData = 
            (nmiContext->eflags & EFLAGS_IF) ? TRUE : FALSE;
         break;
      case STATS_OTHER_DATA_PREEMPTIBLE:
         quickSample->otherData = CpuSched_IsPreemptible();
         break;
      }

      if (VMKStatsIsRootPC(eip)) {
         quickSample->callStack.length = 0;
      } else {
         quickSample->callStack.length = 
            VMKStatsGetCallStack(nmiContext, quickSample->callStack.stack);
      }
      sampleBuffer->put += sizeof(StatsQuickSample) / sizeof(uint32) + quickSample->callStack.length;
      if (sampleBuffer->put > sampleBuffer->maxSafePut) {
         sampleBuffer->put = 0;
      }            
   }
}

/*
 *----------------------------------------------------------------------
 *
 * VMKStatsReset --
 *
 *      Reset the recorded samples, setting all counts to 0
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
VMKStatsReset(void)
{
   int i;
   Bool ignoreFlag = statsIgnoreFlag;
   statsIgnoreFlag = TRUE;

   Semaphore_Lock(&data.sem);

   //reset stats
   memset(data.sampleMap, 0, data.sampleMapMaxCapacity * sizeof(StatsSample));
   data.sampleMapNumSamples = 0;

   for (i=0; i<numPCPUs; i++) {
      statsSampleBuffers[i].get = 0;
      statsSampleBuffers[i].put = 0;
      statsSampleBuffers[i].stalledOnWrite = FALSE;
      statsSampleBuffers[i].drainRequested = FALSE;
   }
   Semaphore_Unlock(&data.sem);

   for (i=0; i < numPCPUs; i++) {
      prdas[i]->vmkstatsClearStats = TRUE;
   }
   statsIgnoreFlag = ignoreFlag;

   // reset epoch stats
   memset(&statsEpoch, 0, sizeof(statsEpoch));
   statsEpoch.startTime = Timer_GetCycles();
}

/*
 *----------------------------------------------------------------------
 *
 * VMKStatsStatusProcRead --
 *
 *      VMKStats procfs status reporting routine.
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
VMKStatsStatusProcRead(Proc_Entry *entry, char *buffer, int *len)
{
   uint32 usecElapsedTotal, usecElapsedEpoch, usecSampledTotal, usecSampledEpoch;
   uint64 secElapsedTotal, secElapsedEpoch, secSampledTotal, secSampledEpoch;
   Timer_Cycles now, elapsedTotal, elapsedEpoch, sampledTotal, sampledEpoch;
   uint32 period, averageHandlerCycles, totalMemUsed;
   uint64 lostEvents, lostSamples, overheadMilliPct;
   Bool vmkstatsActive;
   const char *eventName, *tagDataName;
   int i;

   // initialize
   *len = 0;

   vmkstatsActive = FALSE;
   if (myPRDA.configNMI != NMI_USING_SAMPLER) {
      Proc_Printf(buffer, len,
                  "VMKStats has not been turned on yet.  To start it run:\n"
                  "echo start > /proc/vmware/vmkstats/command\n\n");
   } else if (!statsIgnoreFlag) {
      vmkstatsActive = TRUE;
   }

   // get sampler configuration,lookup event name
   eventName = NMI_SamplerGetEventName();
   if (eventName == NULL) {
      eventName = "unknown";
   }

   period = NMI_SamplerGetPeriod();

   totalMemUsed = data.sampleMapMaxCapacity * sizeof(StatsSample) +
      data.callStacksMapMaxCapacity * sizeof(int) +
      data.callStacksSize;

   // compute elapsed time, uptime fraction
   now = Timer_GetCycles();
   elapsedTotal = now - statsTotal.startTime;
   elapsedEpoch = now - statsEpoch.startTime;
   if (!strcmp(eventName, "cycles")) {
      sampledTotal = statsTotal.samples * (uint64) period;
      sampledEpoch = statsEpoch.samples * (uint64) period;
   } else {
      sampledTotal = 0;
      sampledEpoch = 0;
   }

   // convert cycles to seconds
   Timer_TCToSec(elapsedTotal, &secElapsedTotal, &usecElapsedTotal);
   Timer_TCToSec(sampledTotal, &secSampledTotal, &usecSampledTotal);
   Timer_TCToSec(elapsedEpoch, &secElapsedEpoch, &usecElapsedEpoch);
   Timer_TCToSec(sampledEpoch, &secSampledEpoch, &usecSampledEpoch);

   // compute the number of samples that would have elapsed
   // during the time when we had sampler NMIs disabled
   // (e.g. when running in the monitor or doing a world switch)
   lostEvents = 0;
   for (i=0; i < numPCPUs; i++) {
      uint64 pcpuEvents =  prdas[i]->vmkstatsMissedEvents;
      LOG(1, "pcpu events for pcpu %d = %Lu", i, pcpuEvents);
      lostEvents += pcpuEvents;
   }

   // compute sample processing cost
   averageHandlerCycles = NMI_GetAverageSamplerCycles();

   // compute period-adjusted values, avoiding divison by zero
   if (period != 0) {
      lostSamples = lostEvents / period;
      overheadMilliPct = (averageHandlerCycles * 100000ULL) / period;
   } else {
      lostSamples = 0;
      overheadMilliPct = 0;
   }

   switch (recordOtherData) {
     case STATS_OTHER_DATA_NONE:
      tagDataName = "none";
      break;
     case STATS_OTHER_DATA_WORLDID:
      tagDataName = "worldID";
      break;
     case STATS_OTHER_DATA_PCPU:
      tagDataName = "pcpu";
      break;
     case STATS_OTHER_DATA_INT_ENABLED:
      tagDataName = "intEnabled";
      break;
     case STATS_OTHER_DATA_PREEMPTIBLE:
      tagDataName = "preemptible";
      break;
   default:
      tagDataName = "unknown";
   }
   
   // report header
   Proc_Printf(buffer, len,
	       "profiling:\n"
	       "%12s sampling\n"
	       "%12s event\n"
	       "%12u period\n"
               "%12s tagging\n"
	       "totals:\n"
	       "%12u interrupts\n"
	       "%12u samples\n"
	       "%12u noimage\n"
	       "%12u ignored\n"
	       "%8Lu.%03u elapsed seconds\n"
	       "%8Lu.%03u sampled seconds\n"
	       "epoch:\n"
	       "%12u interrupts\n"
	       "%12u samples\n"
	       "%12u noimage\n"
	       "%12u ignored\n"
               "%12Lu lostSamples\n"
	       "%8Lu.%03u elapsed seconds\n"
	       "%8Lu.%03u sampled seconds\n"
	       "%12u total unique samples\n"
	       "%12u total unique call stacks\n"
               "%12u KB total memory used for per cpu buffers\n"
	       "%12u KB total memory used for recorded stats\n"
               "%12u average nmi number of cycles\n"
               "%8Lu.%03u percentage overhead from nmis\n"
	       "%12u sample map max capacity\n"
	       "%12u sample map entries\n"
	       "%12u call stacks set max capacity\n"
	       "%12u call stacks set entries\n"
	       "%12u call stacks capacity\n"
	       "%12u call stacks used\n",
	       vmkstatsActive ? "STARTED" : "STOPPED",
	       eventName,
	       period,
               tagDataName,
	       statsTotal.interrupts,
	       statsTotal.samples,
	       statsTotal.noimage,
	       statsTotal.ignored,
               secElapsedTotal, usecElapsedTotal / 1000,
               secSampledTotal, usecSampledTotal / 1000,
	       statsEpoch.interrupts,
	       statsEpoch.samples,
	       statsEpoch.noimage,
	       statsEpoch.ignored,
               lostSamples,
               secElapsedEpoch, usecElapsedEpoch / 1000,
               secSampledEpoch, usecSampledEpoch / 1000,
	       data.sampleMapNumSamples,
	       data.callStacksMapNumStacks,
               STATS_SAMPLE_BUFFER_COUNT * sizeof(uint32) * numPCPUs / 1024,
               totalMemUsed / 1024,
               averageHandlerCycles,
               overheadMilliPct / 1000,
               (uint32) (overheadMilliPct % 1000),
	       data.sampleMapMaxCapacity,
	       data.sampleMapNumSamples,
	       data.callStacksMapMaxCapacity,
	       data.callStacksMapNumStacks,
	       data.callStacksSize / sizeof(uint32),
	       data.callStacksNextIndex);

   for (i=0; i < numPCPUs; i++) {
      uint32 pcpuLostSamples;
      if (period == 0) {
         pcpuLostSamples = 0;
      } else {
         pcpuLostSamples = prdas[i]->vmkstatsMissedEvents / period;
      }

      Proc_Printf(buffer, len, "%12u pcpu%uLostSamples\n",
                  pcpuLostSamples,
                  i);
   }
   
   // reset overhead stats
   NMI_ResetAverageSamplerCycles();

   // display stats roots
   Proc_Printf(buffer, len, "\nroot pcs:\n");
   for (i=0; i < numStatsRoots; i++) {
      Proc_Printf(buffer, len, "0x%08x:0x%08x\n", 
                  statsRoots[i].startPC, 
                  statsRoots[i].endPC);
   }

   // success
   return(VMK_OK);
}

static VMK_ReturnStatus
VMKStatsSamplerConfig(char *eventName, char *periodString)
{
   uint32 period;
   Bool ignoreFlag;
   VMK_ReturnStatus res;

   // parse period, use default if none
   if (periodString != NULL) {
      //safe to put in large length because this came from parse args
      Parse_Int(periodString, 100, &period);
   } else {
      period = NMI_SAMPLER_DEFAULT_PERIOD;
   }

   // ignore samples during config
   ignoreFlag = statsIgnoreFlag;
   statsIgnoreFlag = TRUE;

   // reset data, configure NMI sampler
   res = NMI_SamplerSetConfig(eventName, period);
   if (res == VMK_OK) {
      VMKStatsReset();
   }

   // restore flag
   statsIgnoreFlag = ignoreFlag;

   // succeed
   return(res);
}

/*
 *----------------------------------------------------------------------
 *
 * VMKStatsCommandProcWrite --
 *
 *      VMKStats procfs command interface.
 *
 * Results:
 *      Reads ASCII command information from "buffer".
 *	Returns 0 iff successful.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
VMKStatsStart(void) {
   // allocate stats data structures, if necessary
   if (myPRDA.configNMI == NMI_USING_SAMPLER) {
      Warning("sampler already active, not changing");
      return(VMK_FAILURE);
   }

   // setup data structures on the first start only
   if (!statsInitialized) {
      VMK_ReturnStatus status;
      int i;

      // initialize per-cpu sample buffers
      for (i = 0; i < numPCPUs; i++) {
         StatsSampleBuffer *s = &statsSampleBuffers[i];
         status = VMKStatsAllocateMem(STATS_SAMPLE_BUFFER_COUNT * sizeof(uint32),
	                    	      (void**)&s->buffer,
				      &s->bufferRange);

         if (status != VMK_OK) {
	    Warning("Problem allocating memory for statistics.");
	    return status;
	 }

	 s->entries = STATS_SAMPLE_BUFFER_COUNT;
	 s->get = 0;
	 s->put = 0;
	 s->maxSafePut = STATS_SAMPLE_BUFFER_COUNT - 1 - 
	    sizeof(StatsQuickSample) / sizeof(uint32) - STATS_MAX_CALL_DEPTH;
	 s->stalledOnWrite = FALSE;
         s->drainRequested = FALSE;
      }
      
      // create drainer world, fail if unable
      status = VMKStatsDrainWorldCreate();
      if (status != VMK_OK) {
         Warning("unable to create vmkstats world: %s",
                 VMK_ReturnStatusToString(status));
         return(status);
      }

      // set up BH handlers for draining
      statsBHnum = BH_Register(VMKStatsDrainRequest, NULL);
      
      statsInitialized = TRUE;
   }

   // actually tell pcpus to turn on their samplers
   NMI_SamplerChange(TRUE);

   statsIgnoreFlag = FALSE;
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * VMKStatsCommandProcWrite --
 *
 *      VMKStats procfs command interface.
 *
 * Results:
 *      Reads ASCII command information from "buffer".
 *	Returns 0 iff successful.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
VMKStatsCommandProcWrite(Proc_Entry *entry, char *buffer, int *length)
{
   char *argv[PROC_CMD_ARGS_MAX];
   char *cmd;
   int argc;

   // parse buffer into args (assumes OK to overwrite buffer)
   argc = Parse_Args(buffer, argv, PROC_CMD_ARGS_MAX);
   if (argc < 1) {
      Warning("invalid empty command");
      return(VMK_BAD_PARAM);
   }

   // convenient abbrev
   cmd = argv[0];

   // "reset" command
   if (strcmp(cmd, "reset") == 0) {
      VMKStatsReset();
      LOG(0, "reset");
      return(VMK_OK);
   }
   
   // "start" command
   if (strcmp(cmd, "start") == 0) {
      LOG(0, "start");
      return(VMKStatsStart());
   }

   // "stop" command
   if (strcmp(cmd, "stop") == 0) {
      statsIgnoreFlag = TRUE;
      NMI_SamplerChange(FALSE);
      LOG(0, "stop");
      return(VMK_OK);
   }

   // forcibly drain the buffers
   if (strcmp(cmd, "drain") == 0) {
      PCPU p;
      for (p=0; p < numPCPUs; p++) {
         Timer_Add(p, (Timer_Callback) VMKStatsDrainRequest,
                   1, TIMER_ONE_SHOT, NULL);
      }
      return (VMK_OK);
   }

   // "perPCPU" command
   if (strcmp(cmd, "tagdata") == 0) {
      if (argc != 2) {
         Warning("invalid tagdata command: tagdata <type>");
         return(VMK_BAD_PARAM);
      }
      uint32 oldRecordOtherData = recordOtherData;
      if (strcmp(argv[1], "none") == 0) {
         recordOtherData = STATS_OTHER_DATA_NONE;
      } else if (strcmp(argv[1], "world") == 0) {
         recordOtherData = STATS_OTHER_DATA_WORLDID;
      } else if (strcmp(argv[1], "pcpu") == 0) {
         recordOtherData = STATS_OTHER_DATA_PCPU;
      } else if (strcmp(argv[1], "intEnabled") == 0) {
         recordOtherData = STATS_OTHER_DATA_INT_ENABLED;
      } else if (strcmp(argv[1], "preemptible") == 0) {
         recordOtherData = STATS_OTHER_DATA_PREEMPTIBLE;
      } else {
         Warning("invalid tagdata type");
         return(VMK_BAD_PARAM);
      }

      if (oldRecordOtherData != recordOtherData) {
         VMKStatsReset();
      }

      LOG(0,"tagdata");
      return(VMK_OK);
   }

   // "config" command
   if (strcmp(cmd, "config") == 0) {
      // check usage
      if ((argc != 2) && (argc != 3)) {
         Warning("invalid config command: config <event> <period>");
         return(VMK_BAD_PARAM);
      }

      if (myPRDA.configNMI == NMI_USING_SAMPLER) {
         Warning("must stop stats collection in order to reconfigure");
         return(VMK_FAILURE);
      }

      // actually setup the new config
      if (VMKStatsSamplerConfig(argv[1], argc == 2 ? NULL : argv[2]) == 0) {
         LOG(0, "config");
         if (statsInitialized) {
            VMKStatsReset();
            LOG(0, "reset");
         }
      } else {
         Warning("invalid config command");
      }

      return(VMK_OK);
   }

   if (strcmp(cmd, "root") == 0
      || strcmp(cmd, "unroot") == 0) {
      uint32 startPC, endPC;

      if (argc != 3) {
         Warning("invalid number of parameters for root command");
         return (VMK_BAD_PARAM);
      }
      if (myPRDA.configNMI == NMI_USING_SAMPLER) {
         Warning("must stop stats collection in order to change roots");
         return (VMK_FAILURE);
      }
      
      if (Parse_Hex(argv[1], strlen(argv[1]), &startPC) != VMK_OK
          || Parse_Hex(argv[2], strlen(argv[2]), &endPC) != VMK_OK) {
         Warning("invalid PC parameters for root command");
         return (VMK_BAD_PARAM);
      }
      if (strcmp(cmd, "root") == 0) {
         return (VMKStatsAddRoot(startPC, endPC));
      } else {
         return (VMKStatsRemoveRoot(startPC, endPC));
      }
   }
   if (strcmp(cmd, "unrootall") == 0) {
      if (myPRDA.configNMI == NMI_USING_SAMPLER) {
         Warning("must stop stats collection in order to reconfigure");
         return(VMK_FAILURE);
      }

      Log("removing all configured roots");
      VMKStatsRemoveAllRoots();
      return (VMK_OK);
   }

   // fail if unrecognized command
   Warning("invalid command=\"%s\"", buffer);
   return(VMK_BAD_PARAM);
}

/*
 *----------------------------------------------------------------------
 *
 * VMKStatsCommandProcRead --
 *
 *      Report procfs command interface usage.
 *
 * Results:
 *      Writes ASCII command usage information into "buffer".
 *      Sets "length" to the number of bytes written.
 *	Returns 0 iff successful.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
VMKStatsCommandProcRead(Proc_Entry *entry, char *buffer, int *len)
{
   *len = 0;
   // print commands
   Proc_Printf(buffer, len,
	       "start\n"
	       "  Resume data collection.\n\n"
	       "stop\n"
	       "  Suspend data collection.\n\n"
	       "reset\n"
	       "  Zero all sample counts.\n\n"
               "root <startPC> <endPC>\n"
               "  Makes the function starting at startPC and ending before endPC"
               "  into a new 'root'\n"
               "unroot <startPC> <endPC>\n"
               "  Removes the root corresponding to the given program counters\n"
               "unrootall\n"
               "  Removes all configured roots\n"
               "tagdata <type>\n"
               "  Tag stored data based on type (also resets the counters).\n"
               "  Valid types are: none, world, pcpu, intEnabled, preemptible\n\n"
	       "config <event>\n"
	       "config <event> <period>\n"
	       "  Configure sampling event and period.\n"
	       "  Performs reset and stops data collection during config.\n"
	       "  Supported <event> types and default <period>:\n\n");

   // print supported <events>
   Vmkperf_PrintCounterList(buffer, len);

   // success
   return(VMK_OK);
}
