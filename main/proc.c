/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * proc.c - host related functions
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "vm_asm.h"
#include "x86.h"
#include "vmkernel.h"
#include "vmnix_if.h"
#include "libc.h"
#include "host.h"
#include "splock.h"
#include "proc.h"
#include "config.h"
#include "helper.h"
#include "util.h"
#include "list.h"
#include "memalloc.h"
#include "parse.h"
#include "cpusched.h"

#define LOGLEVEL_MODULE Proc
#define LOGLEVEL_MODULE_LEN 4
#include "log.h"

/*
 * procLock protects proc node creation/deletion/refCount.
 * The buffer in procInfo for read/write is protected in vmnix/proc.c
 */
static VMnixProc_Shared procInfo;
static SP_SpinLockIRQ procLock;
static SP_IRQL procIRQL;

/*
 * The maximum time Proc_Remove will spin waiting for refcount to go down.
 * This number is the same value as the maximum spinlock timeout.
 */
#define MAX_PROC_SPIN_SECONDS 2

// magic key to reveal hidden proc nodes
#define PROC_SHOW_HIDDEN_SECRET_STRING "employeesonly"


/*
 * vmkEntries is the vmkernel's view of what nodes exist in the proc
 * file system.  This array is dynamically grown when there are no 
 * more free entries.
 * Entries 0 - PROC_MAX_PREDEF (not inclusive) are reserved for specific linux
 * proc nodes like proc_root, proc_root_driver.
 */
static Proc_Entry **vmkEntries;
static int numVmkEntries;               //current size of above array
static Proc_Entry linuxRoot = { NULL, NULL, NULL, FALSE, NULL, 0 },
                  linuxDrvRoot = { NULL, NULL, &linuxRoot, FALSE, NULL, 0 },
                  linuxNet = { NULL, NULL, &linuxRoot, FALSE, NULL, 0 };

/*
 * It is possible for a read / write request to come from vmnix module
 * to an element of vmkEntries that was already deleted (and possible a new
 * entry was placed in the same slot).  Each proc node has a unique identifier,
 * that is used to protect against this.  
 */
static uint32 procNextGuid = 1;

typedef struct {
   List_Links links;
   VMnixProc_EntryShared info;
} ProcActionItem;


typedef struct {
   List_Links links;
   Proc_Entry *entry;
   char name[VMNIXPROC_MAX_NAME];
   Bool isDirectory;
} ProcHiddenEntry;

static List_Links hiddenEntryList;
static Bool hiddenEntriesShown;

/*
 * If the vmkernel generates more requests to add / delete proc nodes than
 * will fit in the shared queue, use the following linked list to store
 * these requests.
 */
static List_Links reqOverflowQueue;
static int numOverflowEntries = 0; 

//high water mark for numOverflowEntries
static int maxOverflow = 0;
#define MAX_OVERFLOW 4096 

#define PROC_GUARD_ID 0xfedcba98


#define ProcEmptySlots(x)                                                \
   (VMNIXPROC_SHARED_ENTRIES - ((((x).tail + VMNIXPROC_SHARED_ENTRIES) - \
                                 (x).head) % VMNIXPROC_SHARED_ENTRIES))

static VMK_ReturnStatus ProcAddRequestToQueue(VMnixProc_Action action,
      int data, uint32 guid, char *name, int  nParent, Bool allowFailure, Bool cyclic);
static void ProcSyncWithVmnix(void);

#ifdef VMX86_DEBUG
#define PROC_DEBUG 
#endif

static Proc_Entry procStats;
static int ProcStatsReadHandler(Proc_Entry *entry, char *page, int *lenp);
#ifdef PROC_DEBUG 
static int ProcStatsWriteHandler(Proc_Entry *entry, char *page, int *lenp);
#endif

static void ProcRegisterLocked(Proc_Entry *entry, char *name, Bool isDirectory);
static VMK_ReturnStatus ProcRemoveLocked(Proc_Entry *entry);
void ProcLock(void);
void ProcUnlock(void);
void Proc_HideHidden(void);
void Proc_ShowHidden(void);

/*
 *----------------------------------------------------------------------
 *
 * Proc_Init --
 *
 *      Initialization routine for proc subsystem.
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
Proc_Init(VMnix_SharedData *sharedData)
{
   ASSERT(SP_RANK_HOSTIC_LOCK - 1 > SP_RANK_IRQ_PROC);
   SP_InitLockIRQ("procLck", &procLock, SP_RANK_HOSTIC_LOCK - 1);
   procInfo.reqQueue.head = 0;
   procInfo.reqQueue.tail = 0;
   procInfo.activeGuid = 0;
   procInfo.offset = -1;
   procInfo.guard = PROC_GUARD_ID;
   List_Init(&reqOverflowQueue);
   List_Init(&hiddenEntryList);

   vmkEntries = Mem_Alloc(VMNIXPROC_INITIAL_ENTRIES * sizeof(Proc_Entry *));
   ASSERT(vmkEntries);
   numVmkEntries = VMNIXPROC_INITIAL_ENTRIES;
   memset(vmkEntries, 0, VMNIXPROC_INITIAL_ENTRIES * sizeof(Proc_Entry *));
   vmkEntries[PROC_ROOT] = &linuxRoot;      // represents linux proc_root.
   vmkEntries[PROC_ROOT_DRIVER] = &linuxDrvRoot; // linux proc_root_driver.
   vmkEntries[PROC_ROOT_NET] = &linuxNet;        // linux proc_net.
   SHARED_DATA_ADD(sharedData->proc, VMnixProc_Shared *, &procInfo);

#if defined(VMX86_DEBUG) && defined(VMX86_DEVEL)
   // all "hidden" entries should show up in beta/obj builds
   hiddenEntriesShown = TRUE;
#else
   hiddenEntriesShown = FALSE;
#endif

   procStats.read = ProcStatsReadHandler;
#ifdef PROC_DEBUG
   procStats.write = ProcStatsWriteHandler;
#else
   procStats.write = NULL;
#endif 
   procStats.parent = NULL;
   procStats.private = NULL;
   procStats.canBlock = FALSE;
   Proc_Register(&procStats, "procstats", FALSE);
}

/*
 *----------------------------------------------------------------------
 *
 * Proc_InitEntry --
 *
 *      Initializes proc node "entry".
 *
 * Results: 
 *      Initializes "entry" state.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
Proc_InitEntry(Proc_Entry *entry)
{
   // zero state
   memset(entry, 0, sizeof(Proc_Entry));
}

/*
 *----------------------------------------------------------------------
 *
 * ProcStatsReadHandler --
 *
 *      Prints out the various internal stats.
 *
 * Results: 
 *      VMK_OK
 *
 * Side effects:
 *      *page and *lenp are updated.
 *
 *----------------------------------------------------------------------
 */

static int 
ProcStatsReadHandler(UNUSED_PARAM(Proc_Entry *entry),
                     char        *page,
                     int         *lenp)
{
   int i, numUsed = 0;
   for (i = 0; i < numVmkEntries; i++) {
      if (vmkEntries[i]) {
         numUsed++;
      }
   }

   *lenp = 0;
   Proc_Printf(page, lenp, "numVmkEntries =      %d\n", numVmkEntries);
   Proc_Printf(page, lenp, "entriesUsed =        %d\n", numUsed);
   Proc_Printf(page, lenp, "numOverflowEntries = %d\n", numOverflowEntries);
   Proc_Printf(page, lenp, "maxOverflow =        %d\n", maxOverflow);
   Proc_Printf(page, lenp, "overflowQueued =     %d\n", procInfo.overflowQueued);
   Proc_Printf(page, lenp, "procNextGuid =       %d\n", procNextGuid);
   Proc_Printf(page, lenp, "shared queue head =  %d\n", procInfo.reqQueue.head);
   Proc_Printf(page, lenp, "shared queue tail =  %d\n", procInfo.reqQueue.tail);

   return VMK_OK;
}


#ifdef PROC_DEBUG 
/*
 *----------------------------------------------------------------------
 *
 * ProcDumpArray --
 *
 *      Dumps the contents of the vmkernel's proc array to the
 *      log file.  
 *
 * Results: 
 *      none
 *
 * Side effects:
 *      none
 *
 *----------------------------------------------------------------------
 */

static void
ProcDumpArray(void)
{
   int i;

   ProcLock();
   LOG(0, "dumping vmkernel proc array:");
   for (i = 0; i < numVmkEntries; i++) {
      if (vmkEntries[i] != NULL) {
         int j, parent = -1;

         //find the index of this node's parent
         for (j = 0; j < numVmkEntries; j++) {
            if (vmkEntries[j] && (vmkEntries[j] == vmkEntries[i]->parent)) {
               parent = j;
               break;
            }
         }
         LOG(0, "%10u index = %5d, parent = %5d",
             vmkEntries[i]->guid, i, parent);
      }
   }
   ProcUnlock();
}

/*
 * Change the PROC_TEST_FILES value to test different number of nodes.
 * Formula: n = 255 * (PROC_TEST_FILES + 1)
 */

#define PROC_TEST_FILES                 39 
#define PROC_TEST_LEVELS                8 
#define PROC_TEST_BRANCHES              2   //Must be 2 (i'm lazy)
#define PROC_TEST_ENTRIES       (((PROC_TEST_BRANCHES << (PROC_TEST_LEVELS - 1)) - 1) *  \
                                 (PROC_TEST_FILES + 1))
static Proc_Entry *procTestEntries;

/*
 *----------------------------------------------------------------------
 *
 * ProcUnitTestReadHandler --
 *
 *      prints out the private data for the node.  [which happens
 *      to be the nodes index into a the procTestEntries array].
 *
 *
 * Results: 
 *      none.
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */

static int
ProcUnitTestReadHandler(Proc_Entry  *entry,
                        char        *page,
                        int         *len)
{
   int data = (int)entry->private;

   *len = 0;
   Proc_Printf(page, len, "%d\n", data);
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * ProcUnitTestPopulate --
 *
 *      Create a proc tree of dummy nodes.
 *
 * Results: 
 *      none.
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */

static void
ProcUnitTestPopulate(Proc_Entry **entries, 
                     Proc_Entry *parent, 
                     int level, 
                     char *buf,
                     int buflen) 
{
   int i;

   if (level == PROC_TEST_LEVELS) {
      return;
   }

   *entries = *entries + 1;
   snprintf(buf, buflen, "level%d-directory-%d", level, *entries - procTestEntries);
   (*entries)->parent = parent;
   LOG(1, "Adding directory %s to test slot %d",
       buf, *entries - procTestEntries);
   Proc_Register(*entries, buf, TRUE);
   parent = *entries;

   for (i = 0; i < PROC_TEST_BRANCHES; i++) {
      ProcUnitTestPopulate(entries, parent, level + 1, buf, buflen);
   }

   for (i = 0; i < PROC_TEST_FILES; i++) {
      (*entries)++;
      snprintf(buf, buflen, "level%d-file%d", level, i);
      (*entries)->private = (void *)(*entries - procTestEntries);
      (*entries)->read = ProcUnitTestReadHandler;
      (*entries)->parent = parent;
      LOG(1, "Adding entry %s to test slot %d", buf, 
          (int)(*entries - procTestEntries));
      Proc_Register(*entries, buf, FALSE);
      CpuSched_YieldThrottled();         //let COS run
   }
}


/*
 *----------------------------------------------------------------------
 *
 * ProcUnitTest --
 *
 *      Unit test for the proc module -- populates a large, fake proc
 *      tree in /proc/vmware/ProcTest  [or frees the proc tree if it 
 *      has already been created].
 *
 * Results: 
 *      none.
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */

static void
ProcUnitTestDestroy(void)
{
   if (procTestEntries) {
      int i;
      LOG(0, "Cleaning up test entries\n");
      for (i = 0; i < PROC_TEST_ENTRIES; i++) {
         Proc_Remove(&procTestEntries[i]);
         CpuSched_YieldThrottled();         //let COS run
      }
      Mem_Free(procTestEntries);
      procTestEntries = NULL;
   }
}

static void
ProcUnitTestCreate(void)
{
   char buf[256];
   Proc_Entry *entries;
   int arraySize;

   arraySize = (PROC_TEST_ENTRIES + 1) * sizeof(Proc_Entry);

   ASSERT(PROC_TEST_BRANCHES == 2);

   LOG(0, "files = %d, level = %d, branching factor = %d, num entries = %d",
       PROC_TEST_FILES, PROC_TEST_LEVELS, PROC_TEST_BRANCHES, 
       PROC_TEST_ENTRIES);

   //Allocate enough memory to hold all the test entries, and the
   //parent node.
   entries = Mem_Alloc(arraySize);
   if (!entries) {
      Warning("Failed to allocate memory\n");
      return;
   }

   memset(entries, 0, arraySize);
   procTestEntries = entries;

   Proc_InitEntry(entries);
   Proc_Register(entries, "ProcTest", TRUE);
   ProcUnitTestPopulate(&entries, entries, 0, buf, sizeof buf);
   LOG(0, "Created %d entries", entries - procTestEntries);
}


/*
 *----------------------------------------------------------------------
 *
 * ProcStatsWriteHandler --
 *
 *      Resets maxOverflow to 0.  Optionally dumps the
 *      contents of the vmkernel's or vmnix's internal proc
 *      array.
 *
 * Results: 
 *      VMK_OK
 *
 * Side effects:
 *      none
 *
 *----------------------------------------------------------------------
 */

static int 
ProcStatsWriteHandler(UNUSED_PARAM(Proc_Entry *entry),
                      char        *page,
                      int         *lenp)
{
   char *argv[2];
   int argc = Parse_Args(page, argv, 1);

   maxOverflow = 0;

   if (argc > 0) {
      if (strncmp(argv[0], "vmnix", 5) == 0) {
         //cause a dump of the vmnix side by sending an invalid action
         ProcLock();
         ProcAddRequestToQueue(VMNIXPROC_ACTION_DUMPTREE, 0, 0, NULL, -1, FALSE, FALSE);
         ProcSyncWithVmnix();
         ProcUnlock();
      } else if (strncmp(argv[0], "vmkernel", 8) == 0) {
         ProcDumpArray();
      } else if (strncmp(argv[0], "test", 4) == 0) {
         ProcUnitTestCreate();
      } else if (strncmp(argv[0], "dest", 4) == 0) {
         ProcUnitTestDestroy();
      } 
   }
   return VMK_OK;
}
#endif



/*
 *----------------------------------------------------------------------
 *
 * ProcLock --
 *
 *      Acquire the procLock
 *
 * Results: 
 *      None.
 *
 * Side effects:
 *      procLock locked
 *
 *----------------------------------------------------------------------
 */

void
ProcLock(void)
{
   procIRQL = SP_LockIRQ(&procLock, SP_IRQL_KERNEL);
}
   

/*
 *----------------------------------------------------------------------
 *
 * ProcUnlock --
 *
 *      Release the procLock
 *
 * Results: 
 *      None.
 *
 * Side effects:
 *      procLock unlocked
 *
 *----------------------------------------------------------------------
 */

void
ProcUnlock(void)
{
   SP_UnlockIRQ(&procLock, procIRQL);
}
   

/*
 *----------------------------------------------------------------------
 *
 * Proc_Register --
 *
 *      Register a proc entry. This puts the entry in the /proc
 *      filesystem on the host OS under /proc/vmware
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
Proc_Register(Proc_Entry *entry,
	      char       *name,
	      Bool        isDirectory)
{
   ProcLock();

   if (entry->parent) {
      // child nodes of a hidden node must be hidden also
      ASSERT(!entry->parent->hidden);
   }
   ProcRegisterLocked(entry, name, isDirectory);

   ProcUnlock();
}  


/*
 *-----------------------------------------------------------------------------
 *
 * Proc_RegisterHidden --
 *
 *     Register a proc entry, but only show it when hidden proc nodes have
 *     been revealed. If hidden nodes are currently being shown, this proc
 *     node will appear right away.
 *
 * Results:
 *     None
 *
 * Side effects:
 *     Appends to the hiddenEntryList, may cause proc node to appear
 *
 *-----------------------------------------------------------------------------
 */
void
Proc_RegisterHidden(Proc_Entry *entry,
                    char       *name,
                    Bool        isDirectory)
{
   ProcHiddenEntry *hidden = Mem_Alloc(sizeof(ProcHiddenEntry));
   
   ASSERT(hidden);
   if (!hidden) {
      return;
   }
   
   List_InitElement(&hidden->links);
   entry->hidden = TRUE;
   hidden->entry = entry;

   ASSERT(name != NULL);
   if (name != NULL) {
      strncpy(hidden->name, name, VMNIXPROC_MAX_NAME);
      hidden->name[VMNIXPROC_MAX_NAME - 1] = '\0';
   } else {
      hidden->name[0] = '\0';
   }
   hidden->isDirectory = isDirectory;

   if (entry->parent && entry->parent->hidden) {
      // hidden directories cannot have subdirs
      ASSERT(!isDirectory);
   }

   ProcLock();

   // important to insert at rear, so /proc nodes are added in proper order
   List_Insert(&hidden->links, LIST_ATREAR(&hiddenEntryList));
   if (hiddenEntriesShown) {
      ProcRegisterLocked(entry, name, isDirectory);
   }
   ProcUnlock();

   LOG(1, "added hidden entry: %s", name);
}  


/*
 *-----------------------------------------------------------------------------
 *
 * Proc_ShowHidden --
 *
 *     Makes hidden proc nodes appear.
 *
 * Results:
 *     None
 *
 * Side effects:
 *     Registers many proc nodes
 *
 *-----------------------------------------------------------------------------
 */
void
Proc_ShowHidden(void)
{
   List_Links *elt;

   ProcLock();
   if (hiddenEntriesShown) {
      Warning("hidden entries already shown");
      ProcUnlock();
      return;
   }

   // traverse list forward, adding all hidden entries
   LIST_FORALL(&hiddenEntryList, elt) {
      ProcHiddenEntry *hidden = (ProcHiddenEntry*) elt;
      ProcRegisterLocked(hidden->entry, hidden->name, hidden->isDirectory);
   }
   hiddenEntriesShown = TRUE;

   ProcUnlock();
}

/*
 *-----------------------------------------------------------------------------
 *
 * Proc_HideHidden --
 *
 *     Makes hidden proc nodes disappear
 *
 * Results:
 *     None
 *
 * Side effects:
 *     Removes many proc nodes *
 *-----------------------------------------------------------------------------
 */
void
Proc_HideHidden(void)
{
   List_Links *elt;
   
   ProcLock();
   if (!hiddenEntriesShown) {
      Warning("hidden entries not shown");
      ProcUnlock();
      return;
   }
   
   // traverse list backward, removing to-be-hidden entries
   for (elt = List_Last(&hiddenEntryList);
        !List_IsAtEnd(&hiddenEntryList, elt);
        elt = List_Prev(elt)) {
      ProcHiddenEntry *hidden = (ProcHiddenEntry*) elt;
      ProcRemoveLocked(hidden->entry);
   }

   hiddenEntriesShown = FALSE;

   ProcUnlock();
}
   
/*
 *----------------------------------------------------------------------
 *
 * Proc_RegisterLinux --
 *
 *      Register a proc entry under the host proc root. This puts the entry 
 *      in the /proc filesystem on the host OS under /proc. 
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
Proc_RegisterLinux(Proc_Entry       *entry,
                   char             *name,
                   Proc_LinuxParent linuxParent, 
                   Bool             isDirectory)
{

   switch (linuxParent) {
   case PROC_ROOT:
      ASSERT(entry->parent == NULL);
      entry->parent = &linuxRoot;
      break;
   case PROC_ROOT_DRIVER:
      ASSERT(entry->parent == NULL);
      entry->parent = &linuxDrvRoot;
      break;
   case PROC_ROOT_NET:
      ASSERT(entry->parent == NULL);
      entry->parent = &linuxNet;
      break;
   case PROC_PRIVATE:
      ASSERT(entry->parent != NULL);
      break;
   default:
      LOG(0, "Unknown linux parent %d.", linuxParent);
      ASSERT(FALSE);
      break;
   }

   Proc_Register(entry, name, isDirectory);
}  
   
   
/*
 *----------------------------------------------------------------------
 *
 * ProcAddRequestToQueue --
 *
 *      Adds a proc request to either the shared queue, or if there isn't 
 *      space there, the overflow queue.  To preserve ordering, entries
 *      can only be added to the shared queue if the overflow queue is
 *      empty.
 *
 *      If the allowFail parameter is TRUE, and there are more entries
 *      than MAX_OVERFLOW in the overflow queue, this function will
 *      return VMK_LIMIT_EXCEEDED [lost realloc actions would quickly be
 *      fatal -- so we can't punt on them].
 *
 * Results: 
 *      VMK result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus 
ProcAddRequestToQueue(VMnixProc_Action action,
                      int              data,
                      uint32           guid,
                      char            *name,
                      int              nParent,
                      Bool             allowFailure,
                      Bool             cyclic)
{
   ProcActionItem *request;
   VMnixProc_EntryShared *entry;
   Bool updateSharedQueue = FALSE;

   ASSERT(SP_IsLockedIRQ(&procLock));

   if ((ProcEmptySlots(procInfo.reqQueue) > 1) && 
         List_IsEmpty(&reqOverflowQueue)) {
      entry = &procInfo.reqQueue.entries[procInfo.reqQueue.tail];
      updateSharedQueue = TRUE;
      LOG(1, "head = %d, tail = %d data = %d, op = %d",
          procInfo.reqQueue.head, procInfo.reqQueue.tail,
          data, action);
   } else {
      if (allowFailure && numOverflowEntries >= MAX_OVERFLOW)  {
         return VMK_LIMIT_EXCEEDED;
      }

      request = Mem_Alloc(sizeof(ProcActionItem));
      if (request == NULL) {
         return VMK_LIMIT_EXCEEDED;
      }
      entry = &request->info;
      List_InitElement(&(request->links));
      List_Insert(&request->links, LIST_ATREAR(&reqOverflowQueue));
      numOverflowEntries++;
      maxOverflow = MAX(maxOverflow, numOverflowEntries);
   }

   entry->action = action;
   entry->parent = nParent;
   entry->data = data;
   entry->guid = guid;
   entry->cyclic = cyclic;
   
   LOG(1, "action = %d, data = %d, guid = %d, name = %s, nParent = %d", 
       action, data, guid, (name == NULL) ? "" : name, nParent);
   
   if (name == NULL) {
      /* clear out old name so as not to confuse engineers who no longer remember 
       * how their own code works.
       */
      entry->name[0] = '\0';
   } else {
      strncpy(entry->name, name, VMNIXPROC_MAX_NAME);
      entry->name[VMNIXPROC_MAX_NAME - 1] = '\0';
   } 

   /* 
    * now that we have filled in all of the entry's fields, it is safe
    * to update the tail pointer.
    */

   if (updateSharedQueue) {
      procInfo.reqQueue.tail = (procInfo.reqQueue.tail + 1) % VMNIXPROC_SHARED_ENTRIES;
   }

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * ProcSyncWithVmnix --
 *
 *   Copies entries from the overflow queue into the shared queue,
 *   and sends an interrupt to the vmnix module if there are any
 *   entries in the shared queue.
 *
 * Results: 
 *      None.
 *
 * Side effects:
 *      May cause an interrupt to be sent to the console os.
 *
 *----------------------------------------------------------------------
 */

static void
ProcSyncWithVmnix(void) {

   ASSERT(SP_IsLockedIRQ(&procLock));

   while (!List_IsEmpty(&reqOverflowQueue) && 
         ProcEmptySlots(procInfo.reqQueue) > 1) {  //XXX wastes one entry
      int i = procInfo.reqQueue.tail;
      List_Links *elt = List_First(&reqOverflowQueue);
      ProcActionItem *request = (ProcActionItem*)elt;

      memcpy(&procInfo.reqQueue.entries[i],
             &request->info, sizeof(VMnixProc_EntryShared));

      procInfo.reqQueue.tail = (procInfo.reqQueue.tail + 1) % VMNIXPROC_SHARED_ENTRIES;
      List_Remove(elt);
      numOverflowEntries--;
      Mem_Free(request);
   }

   if (ProcEmptySlots(procInfo.reqQueue) != VMNIXPROC_SHARED_ENTRIES) {
      Host_InterruptVMnix(VMNIX_PROC_STATUS_CHANGE);
   }

   // tell the vmnix module that there are still requests left in the
   // overflow queue.
   procInfo.overflowQueued = !List_IsEmpty(&reqOverflowQueue);
}


/*
 *----------------------------------------------------------------------
 *
 * ProcRealloc --
 *
 *     allocate a new vmkEntries array, and copy the existing array
 *     into the first portion of the new array.
 *
 * Results: 
 *      TRUE on success, false otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Bool
ProcRealloc(void) 
{
   int oldLen = numVmkEntries;
   int newLen = oldLen + VMNIXPROC_INITIAL_ENTRIES;
   Proc_Entry **newArray;

   ASSERT(SP_IsLockedIRQ(&procLock));

   newArray = Mem_Alloc(newLen * sizeof(Proc_Entry*));

   if (!newArray) {
      return FALSE;
   }

   memcpy(newArray, vmkEntries, oldLen * sizeof(Proc_Entry *));
   memset(newArray + oldLen, 0, (newLen - oldLen) * sizeof(Proc_Entry *));

   Mem_Free(vmkEntries);

   vmkEntries = newArray;
   numVmkEntries = newLen;

   return TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * ProcGetFreeEntry
 *
 *   Finds a free slot in the vmkEntries array.  If there are
 *   no free slots left, ProcRealloc() is called to resize the
 *   array.
 *
 * Results: 
 *      An index into the vmkEntries array, or -1 on  failure.
 *
 * Side effects:
 *      If the array is resized, an interrupt will be sent to vmnix module.
 *
 *----------------------------------------------------------------------
 */

static int
ProcGetFreeEntry(void) 
{
   int i;

   ASSERT(SP_IsLockedIRQ(&procLock));

   for (i = 0; i < numVmkEntries; i++) {
      if (vmkEntries[i] == NULL) {
         break;
      }
   }

   if (i == numVmkEntries) {
      if (!ProcRealloc()) {
         return -1;
      }

      // tell the vmnix layer how many proc entries we now have
      if (ProcAddRequestToQueue(VMNIXPROC_ACTION_REALLOC, 
                                numVmkEntries, 0 , NULL, 0, FALSE, FALSE) != VMK_OK) {
         /* 
          * The only way this can fail currently (even with allowFail false) is
          * if Mem_Alloc() fails. We should probably release the realloc-ed 
          * memory and return -1. 
          */
         NOT_IMPLEMENTED();
      }
   }
   return i;
}

/*
 *----------------------------------------------------------------------
 *
 * ProcRegisterLocked --
 *
 *      Register a proc entry. This puts the entry in the /proc
 *      filesystem on the console OS.  Caller must hold procLock
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
ProcRegisterLocked(Proc_Entry *entry,
                   char       *name,
                   Bool        isDirectory)
{
   int n, i, nParent = -1;
   VMnixProc_Action action;
   
   ASSERT(SP_IsLockedIRQ(&procLock));

   if ((n = ProcGetFreeEntry()) == -1) {
      goto fail;
   }

   for (i = 0; i < numVmkEntries; i++) {
      if (vmkEntries[i] == entry) {
         Panic("Proc entry %s (0x%p) is already registered at slot %d\n", name, entry, i);
      }
   }

   ASSERT(!vmkEntries[n]);
   vmkEntries[n] = entry;
   entry->guid = procNextGuid++;

   if (entry->parent) {
      for (i = 0; i < numVmkEntries; i++) {
         if (vmkEntries[i] == entry->parent) {
            nParent = i;
            break;
         }
      }
   }

   LOG(5, "%#x is registering proc %s '%s', entry = %d guid = %d, parent = %d",
       (uint32)__builtin_return_address(1), isDirectory? "dir" : "node", 
       name, n, entry->guid, nParent);

   action = (isDirectory) ? VMNIXPROC_ACTION_NEW_DIR: VMNIXPROC_ACTION_NEW_FILE;

   if (ProcAddRequestToQueue(action, n, entry->guid, 
                             name, nParent, TRUE, entry->cyclic) != VMK_OK) {
      goto fail;
   }

   ProcSyncWithVmnix();
   return;

  fail:
   Warning("failed to register %s.  %d entries in array, %d in overflow",
	   name, numVmkEntries, numOverflowEntries);
}


/*
 *----------------------------------------------------------------------
 *
 * Proc_Remove --
 *
 *      Remove a previously registered entry from the host /proc
 *      file system.  Have to wait until refcount drop to zero. Can't
 *      block here because the caller may be holding spin locks, so
 *      spin with a timeout.
 *
 * Results: 
 *      VMK result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
Proc_Remove(Proc_Entry *entry)
{
   Timer_AbsCycles startTime;
   int rc;

   ProcLock();
   ASSERT(entry->refCount >= 0);
   startTime = Timer_GetCycles();
   while (entry->refCount > 0) {
      ProcUnlock(); // free lock so the refcount can be decremented
      while (entry->refCount > 0) {
         if (Timer_TCToMS(Timer_GetCycles() - startTime) > 
             MAX_PROC_SPIN_SECONDS * 1000) {
            Panic("timeout");
         }
         PAUSE();
         // don't spam bus with Timer_GetCycles on x44x boxes
         Util_Udelay(1);
      }
      ProcLock();
   }

   rc = ProcRemoveLocked(entry);

   if (entry->hidden) {
      Bool removedEntry = FALSE;
      List_Links *elt = List_First(&hiddenEntryList);

      // remove from the list of hidden entries, also remove all child nodes
      // basically a custom version of LIST_FORALL, because we need
      // to remove and free list entries while iterating over them
      while (!List_IsAtEnd(&hiddenEntryList, elt)) {
         List_Links *next;
         ProcHiddenEntry *hidden = (ProcHiddenEntry*) elt;
         next = List_Next(elt);

         if (hidden->entry == entry) {
            LOG(1, "removing hidden entry: %s", hidden->name);
            List_Remove(elt);
            Mem_Free(hidden);
            removedEntry = TRUE;
         }  else if (hidden->entry->parent == entry) {
            LOG(1, "removing hidden child: %s", hidden->name);
            List_Remove(elt);
            Mem_Free(hidden);
         }
         elt = next;
      }

      // uh-oh, we couldn't find this entry
      if (!removedEntry) {
         Warning("could not find proc entry to remove");
         rc = VMK_NOT_FOUND;
      }
   }
   ProcUnlock();

   return rc;
}

/*
 *----------------------------------------------------------------------
 *
 * ProcRemoveLocked --
 *
 *      Remove a previously registered entry from the host /proc
 *      file system.  Assumes caller has procLock held
 *
 * Results: 
 *      VMK result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
ProcRemoveLocked(Proc_Entry *entry)
{
   int i;
   
   ASSERT(SP_IsLockedIRQ(&procLock));

   for (i = PROC_MAX_PREDEF; i < numVmkEntries; i++) {
      if (vmkEntries[i] == entry) {
         uint32 guid = vmkEntries[i]->guid;
         vmkEntries[i] = NULL;
         
         if (ProcAddRequestToQueue(VMNIXPROC_ACTION_DELETE, i, 
                                   guid, NULL, -1, TRUE, FALSE) != VMK_OK) {
            Warning("Failed to remove entry %d", i);
         }
         ProcSyncWithVmnix();
         return VMK_OK;
      }
   }
   
   return VMK_NOT_FOUND;
}

/*
 *----------------------------------------------------------------------
 *
 * Proc_Printf --
 *
 *      Print a string to buffer at offset len, taking care not to exceed
 *      the buffer limit, set at VMNIXPROC_BUF_SIZE
 *
 * Results: 
 *      none
 *
 * Side effects:
 *      *len is incremented by number of characters printed
 *
 *----------------------------------------------------------------------
 */

void
Proc_Printf(char *buffer, int *len, const char *format, ...)
{
   va_list ap;

   if (*len >= VMNIXPROC_BUF_SIZE) {
      return;
   }

   va_start(ap, format);
   *len += vsnprintf(buffer+*len, VMNIXPROC_BUF_SIZE-*len, format, ap);
   *len = MIN(*len, VMNIXPROC_BUF_SIZE);
   va_end(ap);

   ASSERT(procInfo.guard == PROC_GUARD_ID);
   return;
}   

/*
 *----------------------------------------------------------------------
 *
 * ProcHandleReadFn --
 *
 *      Handle a read operation on the proc entry from the host OS.
 *      Basically this comes as a system call and is passed to the
 *      read handler for this entry.  Executes in the helper world
 *
 * Results: 
 *      VMK result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
ProcHandleReadFn(void *data, UNUSED_PARAM(void **result))
{

   int entryNum = (int) data;
   Proc_Entry *entry;
   VMK_ReturnStatus retval;

   ProcLock();

   entry = vmkEntries[entryNum];
   ASSERT(entry != NULL);
   ASSERT(entry->guid == procInfo.activeGuid);
   retval = VMK_READ_ERROR;

   ProcUnlock();

   if (entry->read) {
      retval = entry->read(entry, procInfo.buffer, &procInfo.len);
      ASSERT(procInfo.len <= VMNIXPROC_BUF_SIZE);
   }

   ProcLock();
   ASSERT(entry->refCount > 0);
   entry->refCount--; // undo increment done in Proc_HandleRead
   ProcUnlock();

   return retval;
}


/*
 *----------------------------------------------------------------------
 *
 * Proc_HandleRead --
 *
 *      Handle a read operation on the proc entry from the host OS.
 *      Basically this comes as a system call and is passed to the
 *      read handler for this entry.  Checks the locally stored 
 *      guid for the entry with the one supplied from the vmnix
 *      module.  If they don't match, then it is assumed that the
 *      entry was deleted, and we return and empty string
 *
 * Results: 
 *      VMK result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
Proc_HandleRead(int entryNum, Helper_RequestHandle *hostHelperHandle)
{
   Helper_RequestHandle helperHandle = -1;
   VMK_ReturnStatus status = VMK_OK;
   Proc_Entry *entry;

   ASSERT(entryNum >= PROC_MAX_PREDEF);  

   ProcLock();
   entry = vmkEntries[entryNum];
   /*
    * If entry->canBlock is TRUE, calling proc file handlers is done
    * using the helper world.  Otherwise the handler may block indefinitely
    * in the vmkernel due to host IRQ sharing
    */
   if (!entry || entry->guid != procInfo.activeGuid) {
      procInfo.len = 0;
      ProcUnlock();
      goto exit;
   }

   entry->refCount++; // decremented in ProcHandleReadFn after handler returns
   ProcUnlock();

   if (entry->canBlock) {
      helperHandle =
         Helper_RequestSync(HELPER_MISC_QUEUE, ProcHandleReadFn, 
                            (void *)entryNum, NULL, 0, NULL);
      status = VMK_STATUS_PENDING;
   } else {
      status = ProcHandleReadFn((void *)entryNum, NULL);
   }

exit:
   CopyToHost(hostHelperHandle, &helperHandle, sizeof(Helper_RequestHandle));
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * ProcHandleWriteFn --
 *
 *      Handle a write operation on the proc entry from the host OS.
 *      Basically this comes as a system call and is passed to the
 *      write handler for this entry.  Executes in the helper world.
 *      Checks the locally stored  guid for the entry with the one 
 *      supplied from the vmnix  module.  If they don't match, then 
 *      it is assumed that the entry was deleted, and we return 
 *      without doing anything.
 *
 * Results: 
 *      VMK result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
ProcHandleWriteFn(void *data, UNUSED_PARAM(void **result))
{
   int entryNum = (int) data;
   Proc_Entry *entry;
   VMK_ReturnStatus retval;

   ProcLock();

   entry = vmkEntries[entryNum];
   retval = VMK_WRITE_ERROR;
   
   if (!entry || entry->guid != procInfo.activeGuid) {
      procInfo.len = 0;
      ProcUnlock();
      return VMK_OK;
   }
   
   ASSERT(procInfo.len <= VMNIXPROC_BUF_SIZE);
   entry->refCount++;
   ProcUnlock();

   if (entry->write) {
      retval = entry->write(entry, procInfo.buffer, &procInfo.len);
   }
   
   ProcLock();
   ASSERT(entry->refCount > 0);
   entry->refCount--;
   ProcUnlock();
   return retval;
}

/*
 *----------------------------------------------------------------------
 *
 * Proc_HandleWrite --
 *
 *      Handle a write operation on the proc entry from the host OS.
 *      Basically this comes as a system call and is passed to the
 *      write handler for this entry.
 *
 * Results: 
 *      VMK result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
Proc_HandleWrite(int entryNum, Helper_RequestHandle *hostHelperHandle)
{
   Helper_RequestHandle helperHandle;

   ASSERT(entryNum >= PROC_MAX_PREDEF);  
   helperHandle = Helper_RequestSync(HELPER_MISC_QUEUE, ProcHandleWriteFn, 
                                     (void *)entryNum, NULL, 0, NULL);

   CopyToHost(hostHelperHandle, &helperHandle, sizeof(Helper_RequestHandle));
   return VMK_STATUS_PENDING;
}

/*
 *----------------------------------------------------------------------
 *
 * Proc_UpdateRequested --
 *
 *      Handle a request to refill the shared request queue.
 * 
 * Results: 
 *      VMK result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
Proc_UpdateRequested(void)
{
   ProcLock();
   ProcSyncWithVmnix();
   ProcUnlock();
   return VMK_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 * Proc_VerboseConfigChange --
 *
 *     Callback for changes to "ProcVerbose" config option.
 *     Echo the secret string into this node to unhide all nodes.
 *     Echo anything else here to re-hide them.
 *
 * Results:
 *     None
 *
 * Side effects:
 *     May show or hide "hidden" proc nodes
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus 
Proc_VerboseConfigChange(Bool write, Bool valueChanged, int idx)
{
   char *optVal = (char*)Config_GetStringOption(CONFIG_PROC_VERBOSE);

   if (write && valueChanged) {
      if (!strncmp(optVal, PROC_SHOW_HIDDEN_SECRET_STRING, sizeof(PROC_SHOW_HIDDEN_SECRET_STRING) - 1)) {
         LOG(1, "should show hidden");
         Proc_ShowHidden();
      } else {
         LOG(1, "should re-hide hidden");
         Proc_HideHidden();
      }
   }
   return VMK_OK;
}
