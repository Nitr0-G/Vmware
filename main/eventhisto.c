/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 *
 *  EventHisto provides a series of proc nodes under the /proc/vmware/eventhisto/
 *  directory. You can "echo start > /proc/vmware/eventhisto/command" in debug builds
 *  to turn on event histograms. Then, a proc subdirectory will appear for each BH
 *  and interrupt hander with per-pcpu stats about how long the BH/interrupt service
 *  took, including a histogram of handler call durations.
 *
 *  You can also echo "stop" into the proc node to stop measurement or "clear" to reset
 *  the histograms.
 *
 */

#include "eventhisto.h"
#include "proc.h"
#include "mod_loader.h"
#include "memalloc.h"
#include "splock.h"
#include "timer.h"
#include "cpusched.h"
#include "util.h"

#define LOGLEVEL_MODULE EventHisto
#include "log.h"

// use a prime hash table size
#define EVENT_HISTO_TABLE_SIZE (137)
#define EVENT_HISTO_TABLE_ASSOC (2)
#define MAX_SYMNAME_LEN (128)


typedef struct {
   Histogram_Handle *pcpuHistos;
   Proc_Entry *procDir;
   Proc_Entry *globalProcEnt;
   Proc_Entry *pcpuProcEnt;
   char *symname;
   uint32 addr;
} EventHistoEntry;

static Proc_Entry eventHistoProcDir;
static Proc_Entry eventHistoCommandProc;
Bool eventHistoActive;
static SP_SpinLockIRQ eventHistoRegisterLock;

// initialize the default bucket limits,
// which are measured in CPU cycles
static Histogram_Datatype buckets[] = {
   1000,
   10000,
   25000,
   60000,
   250000,
   2     * 1000000ull,
   10    * 1000000ull,
   100   * 1000000ull,
   1000  * 1000000ull,  // about a half second on 2ghz cpu
   10000 * 1000000ull   // about 5 seconds on 2ghz cpu
};

#define NUM_BUCKETS ((sizeof(buckets) / sizeof(Histogram_Datatype)) + 1)

// 2-way associative hashtable with function address as the key
static EventHistoEntry eventEntries[EVENT_HISTO_TABLE_SIZE][EVENT_HISTO_TABLE_ASSOC];

/*
 *-----------------------------------------------------------------------------
 *
 * EventHistoGetEntry --
 *
 *      Returns the EventHistoEntry corresponding to "addr" in the hashtable,
 *      or NULL if no such entry exists;
 *
 * Results:
 *      Returns entry or NULL.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE EventHistoEntry*
EventHistoGetEntry(uint32 addr)
{
   unsigned i, slot = addr % EVENT_HISTO_TABLE_SIZE;

   for (i=0; i < EVENT_HISTO_TABLE_ASSOC; i++) {
      if (eventEntries[slot][i].addr == addr) {
         return &eventEntries[slot][i];
      }
   }
   return (NULL);
}

/*
 *-----------------------------------------------------------------------------
 *
 * EventHistoGlobalProcRead --
 *
 *      Proc read handler for per-event "global" proc node, which aggregates
 *      event counts and stats across all pcpus. May print inconsistent
 *      data due to lack of locking.
 *
 * Results:
 *      Returns VMK_OK on success, errorcode otherwise
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
static int
EventHistoGlobalProcRead(Proc_Entry *procEnt, char *buf, int *len)
{
   Histogram_Handle globalHisto;
   EventHistoEntry *entry = (EventHistoEntry *)procEnt->private;
   
   *len = 0;

   // note that we do no locking here, so the data may be inconsistent
   // and there may be (very rare) atomicity issues with the 64-bit
   // aggregated values (could have read non-atomically while carry
   // from low 32-bit word to high in progress)
   globalHisto = Histogram_Aggregate(mainHeap, entry->pcpuHistos, numPCPUs);
   if (globalHisto == NULL) {
      return (VMK_NO_MEMORY);
   }
   
   LOG(1, "aggregated histograms over all pcpus, count=%Lu",
       Histogram_Count(globalHisto));   
   Histogram_ProcFormat(globalHisto, "", buf, len);
   Histogram_Delete(mainHeap, globalHisto);

   return (VMK_OK);
}

/*
 *-----------------------------------------------------------------------------
 *
 * EventHistoEntryProcInit --
 *
 *      Finds the symbol for this entry and sets up its proc nodes.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Registers proc node.
 *
 *-----------------------------------------------------------------------------
 */
static void
EventHistoEntryProcInit(EventHistoEntry *entry)
{
   PCPU p;
   Bool res;
   uint32 offset;
   
   // find the corresponding symbol and store it in the histo table entry
   entry->symname = Mem_Alloc(MAX_SYMNAME_LEN);
   ASSERT(entry->symname);
   
   res = Mod_LookupSymbolSafe(entry->addr, MAX_SYMNAME_LEN, entry->symname, &offset);
   if (!res) {
      strncpy(entry->symname, "unknown", MAX_SYMNAME_LEN);
      offset = 0;
      LOG(1, "symbol for 0x%x not found", entry->addr);
   } else {
      LOG(1, "symname for 0x%x is %s", entry->addr, entry->symname);
   }

   // add the address of the symbol to the end of the name
   snprintf(entry->symname + strlen(entry->symname),
            MAX_SYMNAME_LEN - strlen(entry->symname),
            ":0x%08x",
            entry->addr);

   // setup main proc directory for this entry
   entry->procDir = Mem_Alloc(sizeof(Proc_Entry));
   ASSERT(entry->procDir);
   Proc_InitEntry(entry->procDir);
   entry->procDir->parent = &eventHistoProcDir;
   Proc_Register(entry->procDir, entry->symname, TRUE);

   // setup per-pcpu proc nodes for this entry
   entry->pcpuProcEnt = Mem_Alloc(sizeof(Proc_Entry) * numPCPUs);
   ASSERT(entry->pcpuProcEnt);
   for (p=0; p < numPCPUs; p++) {
      char name[32];
      snprintf(name, 32, "pcpu%u", p);
      Proc_InitEntry(&entry->pcpuProcEnt[p]);
      entry->pcpuProcEnt[p].read = Histogram_ProcRead;
      entry->pcpuProcEnt[p].private = (void*)entry->pcpuHistos[p];
      entry->pcpuProcEnt[p].parent = entry->procDir;
      Proc_Register(&entry->pcpuProcEnt[p], name, FALSE);
   }

   // add a global (aggregated over all pcpus) proc node for this pcpu
   entry->globalProcEnt = Mem_Alloc(sizeof(Proc_Entry));
   ASSERT(entry->globalProcEnt);
   Proc_InitEntry(entry->globalProcEnt);
   entry->globalProcEnt->parent = entry->procDir;
   entry->globalProcEnt->private = (void*)entry;
   entry->globalProcEnt->read = EventHistoGlobalProcRead;
   Proc_Register(entry->globalProcEnt, "global", FALSE);   
}

/*
 *-----------------------------------------------------------------------------
 *
 * EventHisto_Register --
 *
 *      Sets up the histograms for the "event" (interrupt handler or bottom half)
 *      with a handler at "addr."
 *
 * Results:
 *      None.
 *
 * Side effects:sched/
 *      Sets up histograms.
 *
 *-----------------------------------------------------------------------------
 */
void
EventHisto_Register(uint32 addr)
{
   PCPU p;
   unsigned slot, i;
   EventHistoEntry *entry;

   LOG(1, "register addr 0x%x, slot=%u", addr, addr % EVENT_HISTO_TABLE_SIZE);
   ASSERT(addr != 0);

   SP_LockIRQ(&eventHistoRegisterLock, SP_IRQL_KERNEL);
   
   // find our slot in the hashtable, if one is available
   slot = addr % EVENT_HISTO_TABLE_SIZE;
   for (i=0; i < EVENT_HISTO_TABLE_ASSOC; i++) {
      if (eventEntries[slot][i].addr == 0) {
         // claim this spot
         break;
      }
      if (eventEntries[slot][i].addr == addr) {
         LOG(0, "already registered event at addr 0x%x", addr);
         SP_UnlockIRQ(&eventHistoRegisterLock, SP_GetPrevIRQ(&eventHistoRegisterLock));
         return;
      }
   }
   if (i == EVENT_HISTO_TABLE_ASSOC) {
      Warning("event at addr 0x%x can not be registed, no slot available", addr);
      SP_UnlockIRQ(&eventHistoRegisterLock, SP_GetPrevIRQ(&eventHistoRegisterLock));      
      return;
   }
   entry = &eventEntries[slot][i];   
   entry->addr = addr;
   entry->symname = NULL;

   // now that we've claimed the slot, we can safely drop the lock
   SP_UnlockIRQ(&eventHistoRegisterLock, SP_GetPrevIRQ(&eventHistoRegisterLock));

   // allocate histograms for each pcpu
   // we may be called before numPCPUs is known, so size based on MAX_PCPUS
   entry->pcpuHistos = Mem_Alloc(sizeof(Histogram_Handle) * MAX_PCPUS);
   ASSERT(entry->pcpuHistos);
   for (p=0; p < MAX_PCPUS; p++) {
      entry->pcpuHistos[p] = Histogram_New(mainHeap, NUM_BUCKETS, buckets);
      ASSERT(entry->pcpuHistos[p]);
   }

}

/*
 *-----------------------------------------------------------------------------
 *
 * EventHisto_AddSampleReal --
 *
 *      Inserts a sample into the hashtable, indicating that this invocation
 *      of the function "addr" took "time" cycles.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Adds sample to hashtable.
 *
 *-----------------------------------------------------------------------------
 */
void
EventHisto_AddSampleReal(uint32 addr, int64 time)
{
   EventHistoEntry *entry = EventHistoGetEntry(addr);

   ASSERT(time >= 0);
   
   if (entry != NULL) {
      Histogram_Insert(entry->pcpuHistos[MY_PCPU], time);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * EventHistoClear --
 *
 *      Zeros out all event histograms.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Zeros out all event histograms.
 *
 *-----------------------------------------------------------------------------
 */
static void
EventHistoClear(void)
{
   unsigned i, j;
   
   for (i=0; i < EVENT_HISTO_TABLE_SIZE; i++) {
      for (j=0; j < EVENT_HISTO_TABLE_ASSOC; j++) {
         EventHistoEntry *entry = &eventEntries[i][j];
         if (entry->addr != 0) {
            PCPU p;
            for (p=0; p < numPCPUs; p++) {
               Histogram_Reset(entry->pcpuHistos[p]);
            }
         }
      }
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * EventHistoCommandProcWrite --
 *
 *      Write handler for "command" proc node. Understands the "start," "stop,"
 *      and "clear" commands.
 *
 * Results:
 *      Returns VMK_OK on success, VMK_BAD_PARAM otherwise.
 *
 * Side effects:
 *      May cause event histogram proc nodes to appear.
 *
 *-----------------------------------------------------------------------------
 */
static int
EventHistoCommandProcWrite(Proc_Entry *entry,
                             char       *buffer,
                             int        *len)
{
   unsigned i, j;
   
   if (strncmp(buffer, "start", sizeof("start") - 1) == 0) {
      if (eventHistoActive) {
         Warning("event histograms already active");
         return (VMK_BUSY);
      }

      for (i=0; i < EVENT_HISTO_TABLE_SIZE; i++) {
         for (j=0; j < EVENT_HISTO_TABLE_ASSOC; j++) {
            EventHistoEntry *entry = &eventEntries[i][j];
            if (entry->addr != 0) {
               EventHistoEntryProcInit((void*)entry);
            }
         }
      }

      eventHistoActive = TRUE;
   } else if (strncmp(buffer, "clear", sizeof("clear") - 1) == 0) {
      Bool oldHistoActive = eventHistoActive;
      
      eventHistoActive = FALSE;
      // greatly reduces chance of a race with Histogram_Insert,
      // but doesn't completely eliminate it
      Util_Udelay(50);
      EventHistoClear();

      eventHistoActive = oldHistoActive;
   } else if (strncmp(buffer, "stop", sizeof("stop") - 1) == 0) {
      Log("disabled event histograms");
      eventHistoActive = FALSE;
   } else {
      Warning("command not understood");
      return (VMK_BAD_PARAM);
   }

   return (VMK_OK);
}


/*
 *-----------------------------------------------------------------------------
 *
 * EventHistoCommandProcRead --
 *
 *      Proc read handler to disaply instructions for eventhisto use.
 *
 * Results:
 *      VMK_OK
 *
 * Side effects:
 *      Fills in proc buffer.
 *
 *-----------------------------------------------------------------------------
 */
static int
EventHistoCommandProcRead(Proc_Entry *entry,
                             char       *buffer,
                             int        *len)
{
   *len = 0;
   
   Proc_Printf(buffer, len,
               "Commands:\n"
               "    'clear'  -- resets event histograms\n"
               "    'stop'   -- stops event histogram accounting\n"
               "    'start'  -- begins event histogram accounting\n"
               "\n\n"
               "eventhisto produces histograms of the time consumed, in cycles\n"
               "by each interrupt handler and bottom-half handler on both global\n"
               "and per-pcpu bases.\n\n"
               "To read the event histo stats for a given event, read \n"
               "/proc/vmware/eventhisto/<EventName:addr>/global for global stats\n"
               "or /proc/vmware/eventhisto/<EventName:addr>/pcpuXX for pcpu-specific\n"
               "stats. These proc nodes only appear after you have started eventhisto.\n");
   Proc_Printf(buffer, len, "\nstatus:  %s\n", eventHistoActive ? "active" : "disabled");
               
   return (VMK_OK);
}

/*
 *-----------------------------------------------------------------------------
 *
 * EventHisto_LateInit --
 *
 *      Initializes lock for eventhisto
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Initializes lock for eventhisto
 *
 *-----------------------------------------------------------------------------
 */
void
EventHisto_Init(void)
{
   if (vmx86_debug) {
      SP_InitLockIRQ("eventhisto-reg", &eventHistoRegisterLock, SP_RANK_IRQ_LEAF);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * EventHisto_LateInit --
 *
 *      Registers the main proc nodes for event histograms
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Registers the main proc nodes for event histograms
 *
 *-----------------------------------------------------------------------------
 */
void
EventHisto_LateInit(void)
{
   if (vmx86_debug) {
      Proc_InitEntry(&eventHistoProcDir);
      eventHistoProcDir.parent = NULL;
      Proc_Register(&eventHistoProcDir, "eventhisto", TRUE);
      
      Proc_InitEntry(&eventHistoCommandProc);
      eventHistoCommandProc.parent = &eventHistoProcDir;
      eventHistoCommandProc.canBlock = TRUE;
      eventHistoCommandProc.write = EventHistoCommandProcWrite;
      eventHistoCommandProc.read = EventHistoCommandProcRead;
      
      Proc_Register(&eventHistoCommandProc, "command", FALSE);
   }
}

