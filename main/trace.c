/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * trace.c --
 *
 *	Fast event trace facility for use with the TraceViz GUI.
 *
 *      The gui is located in bora/support/tools/java.
 *
 *      TODO:  - handle unsync'ed TSCs on NUMA boxes
 *             - add tracing for action posts, host syscalls, many others 
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "vm_asm.h"
#include "vm_atomic.h"
#include "x86.h"
#include "vmkernel.h"
#include "splock.h"
#include "libc.h"
#include "memmap.h"
#include "memalloc.h"
#include "util.h"
#include "trace_ext.h"
#include "trace.h"
#include "parse.h"
#include "timer.h"
#include "xmap.h"

#define	LOGLEVEL_MODULE Trace
#include "log.h"

Bool traceActive = FALSE;

// for proc write handler
#define TRACE_ARGS_MAX (15)

// prevent the user from exhausting all memory with a huge trace
#define TRACE_MAX_SIZE (5000000)

// turns on more verbose proc nodes
#define TRACE_DEBUG (0)

typedef struct {
   Atomic_uint32 traceIndex;          // slot to be used by next event
   uint32 bufferSize;
   Bool circBuffer;
   Proc_Entry traceDirProcEnt;
   Proc_Entry traceControlProcEnt;
   XMap_MPNRange range;  
   TraceEvent *entries;               // pointer for start of memHandle
   uint32 offset;                     // offset for proc read
   SP_SpinLock lock;                  // protects busy
   Bool busy;
} TraceData;



#define TRACE_MAX_CUSTOM_TAGS (1000)
#define TRACE_MAX_RECENT_WORLD_DESCS (50)

// contains info about known worlds, custom tags
typedef struct {
   Atomic_uint32 numCustomTags;
   TraceCustomTag customTags[TRACE_MAX_CUSTOM_TAGS];
   Atomic_uint32 recentWorldDeaths;
   WorldDesc recentlyDeadWorlds[TRACE_MAX_RECENT_WORLD_DESCS];
} TraceMetaDataLists;

static TraceData trace;
static TraceMetaDataLists metaLists;

#undef TRACE_E
#define TRACE_E(CLS, _ARGS...) { CLS, _ARGS},

TraceEventDef traceDefs[] = {
   TRACE_EVENT_LIST
};

#define TRACE_C(ID, NAME) {ID, NAME},
   
TraceClassDef classDefs[] = {
   TRACE_CLASS_LIST
};


// allow enable/disable of individual classes and events
static Bool traceClassActive[TRACE_CLASS_MAX];
Bool traceEventEnabled[TRACE_EVENT_MAX];

static VMK_ReturnStatus TraceStop(void);

#define VMKCALL(num, intState, fnName, group, expl) XSTR(num),
static const char* vmkcallNames[] = {
#include "vmkcallTable_vmcore.h"
#include "vmkcallTable_public.h"
};

/*
 *-----------------------------------------------------------------------------
 *
 * Trace_EventInt --
 *
 *      Records the given trace event in the in-memory buffer. Should only
 *      be called by the external wrapper function, which confirms that
 *      we have tracing enabled.
 *      Claims a slot in the buffer using an atomic fetch-and-inc, making
 *      sure not to go past the end of the array.
 *      If "ts" is -1, we generate a new timestamp for the event.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Writes to in-memory buffer.
 *
 *-----------------------------------------------------------------------------
 */
void
Trace_EventInt(TraceEventID id, World_ID wid, PCPU p, uint32 custom, int64 data, uint64 ts)
{
   uint32 slot;
   TraceEvent *entry;

   // Important: due to the lack of any locks, events may be out of order in the
   // buffer with respect to their timestamps. External programs need to sort
   // based on timestamp to get a sensible ordering.
   
   // claim our slot
   slot = Atomic_FetchAndInc(&trace.traceIndex);
   
   if (trace.circBuffer) {
      // Note that this approach never requires us to reset
      // the value of "slot" when we wrap the circular buffer,
      // event when we wrap past 32 bits.
      slot %= trace.bufferSize;
   } else if (slot >= trace.bufferSize && !trace.circBuffer) {
      // we've run off the end of the buffer, so stop tracing
      traceActive = FALSE;
      return;
   }

   // Note that there's a nearly-impossible race here, since we checked "traceActive"
   // without any locking before entering this function. If somebody stopped the
   // trace and then wrote to a proc node at exactly the write time, we might
   // might free the "entries" buffer and take a fault here. However, writing
   // to a proc node is really slow and writing to a single cacheline is really
   // fast, so I'm not worried as long as this is on in internal builds only.
   // If we want to ship with tracing enabled for some reason, we should reconsider this.
   
   entry = &trace.entries[slot];
   if (ts == (uint64)-1) {
      // XXX convert to Timer_PseudoTSC
      // right now, Timer_PseudoTSC will die if we're preemptible
      // AND we hold a lock, which happens in the scheduler, but will
      // be fixed in the near future. 
      entry->timestamp = RDTSC();
   } else {
      entry->timestamp = ts;
   }
   entry->wid = wid;
   entry->pcpu = p;
   entry->id = id;
   entry->custom = custom;
   entry->data = data;
   entry->eclass = traceDefs[id].eclass;
}

/*
 *-----------------------------------------------------------------------------
 *
 * TraceStart --
 *
 *      Activates tracing after allocating a buffer large enough to
 *      hold "traceSize" entries. Will fail if tracing is already active
 *      or if we can't allocate enough memory to create the buffer.
 *      If "circBuffer" is TRUE, the trace data will go into a circular buffer of
 *      fixed size, so that tracing will not automatically stop when exhausting the
 *      buffer, but rather just overwrite old data.
 *
 *      If an old buffer had been allocated for a previous trace,
 *      this will free it automatically. All global values (min, max, offset, index)
 *      are also reset.
 *
 * Results:
 *      Returns VMK_OK on success, or VMK_BAD_PARAM or VMK_NO_MEMORY on error
 *
 * Side effects:
 *      Allocates tons of memory. Frees old buffers.
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
TraceStart(uint32 traceSize, Bool circBuffer)
{
   VMK_ReturnStatus status = VMK_OK;

   // grab the lock so we don't free the buffer while somebody is reading
   SP_Lock(&trace.lock);
   if (trace.busy) {
      SP_Unlock(&trace.lock);
      Log("trace read or realloc in progress, cannot restart");
      return (VMK_BUSY);
   }
   trace.busy = TRUE;
   SP_Unlock(&trace.lock);
   
   if (traceActive) {
      Warning("trace still active, must stop it first");
      status = VMK_BAD_PARAM;
      goto done;
   }
   if (traceSize > TRACE_MAX_SIZE) {
      Warning("desired trace size of %u is too large, max is %u",
              traceSize,
              TRACE_MAX_SIZE);
      status = VMK_BAD_PARAM;
      goto done;
   }

   // free old trace memory
   if (trace.bufferSize > 0) {
      XMap_Unmap(trace.range.numMPNs, trace.entries);
      MemMap_FreeKernelPages(trace.range.startMPN);
      trace.entries = NULL;
      trace.offset = 0;
   }

   // allocate the new buffer
   trace.range.numMPNs = CEIL(sizeof(TraceEvent) * traceSize, PAGE_SIZE);
   trace.range.startMPN = MemMap_NiceAllocKernelPages(trace.range.numMPNs,
   						      MM_NODE_ANY,
						      MM_COLOR_ANY,
						      MM_TYPE_ANY);

   if (trace.range.startMPN == INVALID_MPN) {
      Warning("insufficient memory to allocate %u events", traceSize);
      status = VMK_NO_MEMORY;
      goto done;
   }

   trace.entries = XMap_Map(trace.range.numMPNs, &trace.range, 1);
  
   if (trace.entries == NULL) {
      Warning("insufficient memory to map %u events", traceSize);
      status = VMK_NO_ADDRESS_SPACE;
      MemMap_FreeKernelPages(trace.range.startMPN); 
      trace.range.startMPN = INVALID_MPN;
      goto done;
   }

   memset(trace.entries, 0, sizeof(TraceEvent) * traceSize);
   trace.bufferSize = traceSize;
   Atomic_Write(&trace.traceIndex, 0);
   
   trace.circBuffer = circBuffer;
   traceActive = TRUE;
 
   LOG(0, "started trace with size %u", traceSize);

  done:
   trace.busy = FALSE;
   return (status);
}

/*
 *-----------------------------------------------------------------------------
 *
 * TraceStop --
 *
 *      Stops tracing, but does not free buffers or reset any data.
 *
 * Results:
 *      Returns VMK_OK on success, VMK_BAD_PARAM if no trace was running
 *
 * Side effects:
 *      Stops tracing.
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
TraceStop(void)
{
   if (trace.bufferSize == 0) {
      Warning("no trace was started");
      return (VMK_BAD_PARAM);
   }

   traceActive = FALSE;

   return (VMK_OK);
}


/*
 *-----------------------------------------------------------------------------
 *
 * TraceSetClassesActive --
 *
 *      Activates or deactivates (depending on "active") all the trace classes
 *      whose names are in the string list "classNameList."
 *
 * Results:
 *      Returns VMK_OK on success. VMK_NOT_FOUND if none of the events
 *      could be found in the class list.
 *
 * Side effects:
 *      Enables/disables event classes.
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
TraceSetClassesActive(char **classNameList, int listLen, Bool active)
{
   unsigned listEntry, numDone=0;
   
   for (listEntry=0; listEntry < listLen; listEntry++) {
      unsigned i, classID;
      const char *className = classNameList[listEntry];
      
      for (i=0; i < TRACE_CLASS_MAX; i++) {
         if (!strcmp(className, classDefs[i].name)) {
            break;
         }
      }
      if (i == TRACE_CLASS_MAX) {
         Warning("%s is not a valid trace class name", className);
         continue;
      }
      classID = i;

      if (classID == TRACE_VMKPERF) {
         if (active) {
            // sample every millisecond
            Vmkperf_SetSamplerRate(1);
         } else {
            // revert to default
            Vmkperf_SetSamplerRate(-1);
         }
      } 
      // loop over all events in this class, enabling or disabling them
      // note that we do this unlocked, so some events may get disabled/enabled
      // slightly after others
      for (i=0; i < TRACE_EVENT_MAX; i++) {
         if (traceDefs[i].eclass == classID) {
            traceEventEnabled[i] = active;
         }
      }
      traceClassActive[classID] = active;
      LOG(0, "enabled events for trace class %s", className);
      numDone++;
   }
   if (numDone > 0) {
      return (VMK_OK);
   } else {
      return (VMK_NOT_FOUND);
   }
}
/*
 *-----------------------------------------------------------------------------
 *
 * TraceControlProcWrite --
 *
 *      Handles "stop", "offset", and "start" commands written to trace control
 *      proc node.
 *
 * Results:
 *      Returns VMK_OK on success, VMK_BAD_PARAM if command was not understood
 *
 * Side effects:
 *      May start tracing, stop tracing, or set proc read offset.
 *
 *-----------------------------------------------------------------------------
 */
static int
TraceControlProcWrite(Proc_Entry *entry, char *buffer, int *len)
{ 
   int argc;
   char *argv[TRACE_ARGS_MAX];
   VMK_ReturnStatus res;
   
   argc = Parse_Args(buffer, argv, TRACE_ARGS_MAX);

   if (argc == 1 && !strcmp(argv[0], "stop")) {
      Log("stopping trace");
      res = TraceStop();
   } else if (argc == 1 && !strcmp(argv[0], "restart")) {
      if (trace.bufferSize == 0) {
         Warning("no trace was previously started, use 'start' instead");
         res = VMK_BAD_PARAM;
      } else {
         traceActive = TRUE;
         res = VMK_OK;
      }
   } else if (argc == 2 && !strcmp(argv[0], "offset")) {
      uint32 newOffset;
      if (Parse_Int(argv[1], strlen(argv[1]), &newOffset) != VMK_OK
         || newOffset % sizeof(TraceEvent) != 0
         || newOffset / sizeof(TraceEvent) > trace.bufferSize) {
         Warning("invalid offset: %s", argv[1]);
         res = VMK_BAD_PARAM;
      } else {
         trace.offset = newOffset;
         res = VMK_OK;
      }
   } else if (argc >= 2 && !strcmp(argv[0], "start")) {
      uint32 size;
      Bool circBuffer = FALSE;
      if (argc == 3 && !strcmp(argv[2], "circular")) {
         circBuffer = TRUE;
      }
      if (Parse_Int(argv[1], strlen(argv[1]), &size) != VMK_OK) {
         Warning("invalid start size: %s", argv[1]);
         res = VMK_BAD_PARAM;
      } else {
         if (size == 0 || size > TRACE_MAX_SIZE) {
            Warning("invalid start size: %u", size);
            return (VMK_BAD_PARAM);
         }
         res = TraceStart(size, circBuffer);
      }
   } else if (argc >= 2 && !strcmp(argv[0], "enable")) {
      res = TraceSetClassesActive(&argv[1], argc-1, TRUE);
   } else if (argc >= 2 && !strcmp(argv[0], "disable")) {
      res = TraceSetClassesActive(&argv[1], argc-1, FALSE);
   } else {
      Warning("invalid args");
      res = VMK_BAD_PARAM;
   }
   
   return(res);
}

/*
 *-----------------------------------------------------------------------------
 *
 * TraceControlProcRead --
 *
 *      Displays basic data about the currently-active trace, if any.
 *      If TRACE_DEBUG is TRUE, this node also displays the first 100 events
 *      in a text format.
 *
 * Results:
 *      Returns VMK_OK;
 *
 * Side effects:
 *      Writes to proc buffer.
 *
 *-----------------------------------------------------------------------------
 */
static int
TraceControlProcRead(Proc_Entry *entry, char *buffer, int *len)
{
   unsigned i;
   uint32 tracePos;
   
   *len = 0;

   tracePos = Atomic_Read(&trace.traceIndex);
   if (trace.circBuffer) {
      tracePos %= trace.bufferSize;
   }
   
   Proc_Printf(buffer, len,
               "index:     %8u\n"
               "size:      %8u\n"
               "circular:  %8s\n"
               "offset:    %8u\n"
               "active?    %8s\n\n",
               tracePos,
               trace.bufferSize,
               trace.circBuffer ? "yes" : "no",
               trace.circBuffer ? trace.offset % trace.bufferSize : trace.offset,
               traceActive ? "yes" : "no");

   for (i=0; i < TRACE_CLASS_MAX; i++) {
      Proc_Printf(buffer, len, "%14s  %s\n",
                  classDefs[i].name,
                  traceClassActive[i] ? "active" : "off");
   }
   Proc_Printf(buffer, len, "\n");

   // print out the first 100 events in text format
   if (TRACE_DEBUG && Atomic_Read(&trace.traceIndex) > 100) {
      int i;
      for (i=0; i < 100; i++) {
         TraceEvent *ev = &trace.entries[i];
         Proc_Printf(buffer, len,
                     "(world: %u) (pcpu: %u) (class: %u) (id: %u) (idName: %s) (data: %Ld) (timestamp(M): %Lu)\n",
                     ev->wid,
                     ev->pcpu,
                     ev->eclass,
                     ev->id,
                     traceDefs[ev->id].name,
                     ev->data,
                     ev->timestamp / 1000000ll);
      }
   }
   return(VMK_OK);
}


/*
 *-----------------------------------------------------------------------------
 *
 * Trace_GetEventDefs --
 *
 *      Sysinfo handler to fill in the given buffer with all known
 *      event definitions.
 *
 * Results:
 *      Returns VMK_OK
 *
 * Side effects:
 *      Fills in buffer.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
Trace_GetEventDefs(int *index, Trace_EventDefBuffer *buf, unsigned long outBufLen)
{
   unsigned i;
   if (outBufLen < sizeof(*buf)) {
      Warning("input buffer too small");
      return (VMK_BAD_PARAM);
   }
   memset(buf, 0, sizeof(*buf));
   for (i=0; i < TRACE_EVENT_MAX; i++) {
      TraceEventDef *def = &buf->entries[i];
      strncpy(def->name, traceDefs[i].name, TRACE_MAX_NAME_LEN);
      def->name[TRACE_MAX_NAME_LEN] = '\0';
      def->id = traceDefs[i].id;
      def->eclass = traceDefs[i].eclass;
      def->defaultKey = traceDefs[i].defaultKey;
      def->pointEvent = traceDefs[i].pointEvent;
   }
   buf->count = (int)TRACE_EVENT_MAX;
   return (VMK_OK);
}

/*
 *-----------------------------------------------------------------------------
 *
 * Trace_GetEventClasses --
 *
 *      Sysinfo handler to fill in the given buffer with all known event
 *      class definitions.
 *
 * Results:
 *      Returns VMK_OK
 *
 * Side effects:
 *      Fills in buffer.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
Trace_GetEventClasses(int *unused, Trace_ClassDefBuffer *buf, unsigned long outBufLen)
{
   unsigned i;
   if (outBufLen < sizeof(*buf)) {
      Warning("input buffer too small");
      return (VMK_BAD_PARAM);
   }
   memset(buf, 0, sizeof(*buf));
   buf->count = (int)TRACE_CLASS_MAX;
   for (i=0; i < TRACE_CLASS_MAX; i++) {
      buf->entries[i] = classDefs[i];
      buf->entries[i].isEnabled = traceClassActive[i];
   }
   return (VMK_OK);
}

/*
 *-----------------------------------------------------------------------------
 *
 * Trace_GetCustomTags --
 *
 *      Sysinfo handler to fill in the given buffer with all known custom
 *      tags.
 *
 * Results:
 *      Returns VMK_OK
 *
 * Side effects:
 *      Fills in buffer.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
Trace_GetCustomTags(int *unused, Trace_CustomTagBuffer *buf, unsigned long outBufLen)
{
   unsigned i;
   if (outBufLen < sizeof(*buf)) {
      Warning("input buffer too small");
      return (VMK_BAD_PARAM);
   }
   // because there's no real locking here, we could get a corrupt tag name
   // if another caller is registering a custom tag while we read
   buf->count = Atomic_Read(&metaLists.numCustomTags);
   for (i=0; i < buf->count; i++) {
      buf->entries[i] = metaLists.customTags[i];
   }
   return (VMK_OK);
}

/*
 *-----------------------------------------------------------------------------
 *
 * TraceSetupWorldDesc --
 *
 *      Fills in "thisVm" with info representing world "w"
 *      If the "isDead" flag is passed in, the name stored name will
 *      be modifed to indicate that the world is dead.
 *
 * Results:
 *      Fills in "thisVm"
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
static void
TraceSetupWorldDesc(WorldDesc *thisVM, const World_Handle *w, Bool isDead)
{
   const char *fmt;
   if (isDead) {
      fmt = "<%s>";
   } else {
      fmt = "%s";
   }
   snprintf(thisVM->name, TRACE_MAX_NAME_LEN, fmt, w->worldName);
   thisVM->name[TRACE_MAX_NAME_LEN] = '\0';
   thisVM->vmid = w->worldID;
   thisVM->gid = World_GetGroupLeaderID(w);
}

/*
 *-----------------------------------------------------------------------------
 *
 * Trace_GetWorldDescs --
 *
 *      Sysinfo handler to fill in buffer with info about known worlds.
 *
 * Results:
 *      Returns
 *
 * Side effects:
 *      Fills in buffer.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
Trace_GetWorldDescs(int *unused, Trace_WorldDescBuffer *buf, unsigned long outBufLen)
{
   World_ID *allWorlds;
   unsigned i;
   uint32 nworlds = TRACE_META_BUFFER_LEN - TRACE_MAX_RECENT_WORLD_DESCS;

   memset(buf, 0, sizeof(*buf));
   
   // fill in the list of VMs and their names
   allWorlds = Mem_Alloc(sizeof(World_ID) * nworlds);
   if (!allWorlds) {
      Warning("insufficient memory");
      return (VMK_NO_MEMORY);
   }
   
   World_AllWorlds(allWorlds, &nworlds);

   // fill in the list of all live worlds
   buf->count = 0;
   for (i=0; i<nworlds; i++) {
      World_Handle *w = World_Find(allWorlds[i]);
      if (w) {
         int index = buf->count;
         WorldDesc *thisVM = &buf->entries[index];
         TraceSetupWorldDesc(thisVM, w, FALSE);
         buf->count++;
         
         World_Release(w);
      }
   }
   Mem_Free(allWorlds);

   // fill in the list of recently dead worlds
   for (i=0;
        i < MIN(Atomic_Read(&metaLists.recentWorldDeaths), TRACE_MAX_RECENT_WORLD_DESCS);
        i++) {
      buf->entries[buf->count] = metaLists.recentlyDeadWorlds[i];
      buf->count++;
   }
   LOG(1, "filled in list of %u vms", buf->count);
   return (VMK_OK);
}

/*
 *-----------------------------------------------------------------------------
 *
 * Trace_GetMetadata --
 *
 *      Vmksysinfo handler to fill in a buffer of trace metadata.
 *      The metadata include info on active VMs, known classes, and known
 *      eventtypes.
 *
 * Results:
 *      Returns VMK_OK, fills in "buf" with appropriate metadata
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */ 
VMK_ReturnStatus
Trace_GetMetadata(UNUSED_PARAM(int *unused), Trace_MetadataBuffer *buf /* out */, unsigned long outBufLen)
{
   uint32 nworlds = MAX_WORLDS;
   World_ID *allWorlds;
   ASSERT(outBufLen >= sizeof(Trace_MetadataBuffer));
   memset(buf, 0, sizeof(Trace_MetadataBuffer));

   // basic metadata
   buf->active = traceActive;
   buf->khzEstimate = cpuKhzEstimate;
   buf->bufSize = trace.bufferSize;
   buf->circular = trace.circBuffer;
   buf->numEvents = MIN(trace.bufferSize, Atomic_Read(&trace.traceIndex) - 1);
   buf->numPcpus = numPCPUs;
   buf->numCustomTags = Atomic_Read(&metaLists.numCustomTags);

   // find number of worlds, which might change before GetWorldDescs is called
   // caller must read the "count" in the GetWorldDescs result to overcome this
   allWorlds = Mem_Alloc(sizeof(World_ID) * MAX_WORLDS);
   if (!allWorlds) {
      Warning("insufficient memory");
      return (VMK_NO_MEMORY);
   }
   World_AllWorlds(allWorlds, &nworlds);
   buf->numWorlds = nworlds + MIN(Atomic_Read(&metaLists.recentWorldDeaths),
                                  TRACE_MAX_RECENT_WORLD_DESCS);
   Mem_Free(allWorlds);
   
   // list of event types
   buf->numTypes = (int)TRACE_EVENT_MAX;

   // list of event classes
   buf->numClasses = (int)TRACE_CLASS_MAX;

   LOG(1, "filled in %u classes, active=%d", buf->numClasses, buf->active);

   return (VMK_OK);
}


/*
 *-----------------------------------------------------------------------------
 *
 * Trace_GetBatchData --
 *
 *      Vmksysinfo handler to copy the bulk trace event data into "buf."
 *      Tracing must be stopped before obtaining batch data.
 *      This function may return VMK_BUSY if another read or buffer reallocation
 *      is in progress at the same time.
 *
 * Results:
 *      Returns
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
Trace_GetBatchData(int *index, Trace_DataBuffer *buf, unsigned long outBufLen)
{
   int len, curIndex;
   VMK_ReturnStatus status = VMK_OK;
   
   // grab the lock to ensure that this buffer doesn't get freed
   // while we're reading from it
   SP_Lock(&trace.lock);
   if (trace.busy) {
      SP_Unlock(&trace.lock);
      Log("trace read or realloc in progress, cannot read");
      return (VMK_BUSY);
   }
   trace.busy = TRUE;
   SP_Unlock(&trace.lock);
   
   if (traceActive) {
      Warning("trace still active, must stop before reading");
      status = VMK_BUSY;
      goto done;
   }
   if (trace.bufferSize == 0) {
      Warning("no trace data");
      status = VMK_BAD_PARAM;
      goto done;
   }

   curIndex = MIN(Atomic_Read(&trace.traceIndex) - 1, trace.bufferSize);
   if (*index >= curIndex) {
      LOG(0, "read past end of trace data");
      buf->numEvents = 0;
      status = VMK_OK;
      goto done;
   }

   len = MIN(curIndex - *index, TRACE_BUFFER_LEN);
   ASSERT(sizeof(TraceEvent) * len <=outBufLen);
   memcpy(buf->events, trace.entries + *index, sizeof(TraceEvent) * len);
   buf->numEvents = len;

   LOG(1, "copied %d events into databuffer", buf->numEvents);

  done:
   trace.busy = FALSE;
   return (status);
}


/*
 *-----------------------------------------------------------------------------
 *
 * TraceProcSetup --
 *
 *      Installs all necessary proc nodes for the trace module.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Adds proc nodes.
 *
 *-----------------------------------------------------------------------------
 */
static void
TraceProcSetup(void)
{
   Proc_InitEntry(&trace.traceControlProcEnt);
   Proc_InitEntry(&trace.traceDirProcEnt);

   Proc_Register(&trace.traceDirProcEnt, "trace", TRUE);
   
   trace.traceControlProcEnt.parent = &trace.traceDirProcEnt;
   trace.traceControlProcEnt.read = TraceControlProcRead;
   trace.traceControlProcEnt.write = TraceControlProcWrite;
   Proc_Register(&trace.traceControlProcEnt, "trace-control", FALSE);
}

/*
 *-----------------------------------------------------------------------------
 *
 * Trace_Init --
 *
 *      Initializes the trace module, adds proc nodes.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Adds proc nodes.
 *
 *-----------------------------------------------------------------------------
 */
void
Trace_Init(void)
{
   extern void UWLOG_SetupSyscallTraceNames(void);
   if (TRACE_MODULE_ACTIVE) {
      int i;
      LOG(0, "initializing trace module");
      TraceProcSetup();
      SP_InitLock("trace", &trace.lock, SP_RANK_LEAF);

      UWLOG_SetupSyscallTraceNames();

      // setup vmkcall names
      for (i=0; i < VMK_MAX_FUNCTION_ID; i++) {
         Trace_RegisterCustomTag(TRACE_VMMVMKCALL, i, vmkcallNames[i]);
      }
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * Trace_RegisterCustomTag --
 *
 *      Saves the mapping from "customVal" to "customTag" as a tag associated
 *      with the given eclass.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Saves the mapping
 *
 *-----------------------------------------------------------------------------
 */
void
Trace_RegisterCustomTag(int eclass, uint32 customVal, const char *customTag)
{
   if (TRACE_MODULE_ACTIVE) {
      uint32 thisIndex = Atomic_FetchAndInc(&metaLists.recentWorldDeaths);
      TraceCustomTag *thisTag = &metaLists.customTags[thisIndex];
      thisTag->tagId = customVal;
      thisTag->eclass = eclass;
      strncpy(thisTag->name,
              customTag,
              TRACE_MAX_NAME_LEN);
      thisTag->name[TRACE_MAX_NAME_LEN] = 0;
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * Trace_RecentWorldDeath --
 *
 *      Saves info about world "w" in a cache of recently-dead worlds
 *      into a circular buffer. No locking, uses atomic ops instead.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Stores info about "w"
 *
 *-----------------------------------------------------------------------------
 */
void
Trace_RecentWorldDeath(const World_Handle *w)
{
   if (TRACE_MODULE_ACTIVE
       && !World_IsPOSTWorld(w)) {
      uint32 thisIndex = Atomic_FetchAndInc(&metaLists.recentWorldDeaths)
         % TRACE_MAX_RECENT_WORLD_DESCS;
      TraceSetupWorldDesc(&metaLists.recentlyDeadWorlds[thisIndex], w, TRUE);
   }
}
