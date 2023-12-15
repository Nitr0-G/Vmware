/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

#include "vm_types.h"
#include "vm_asm.h"
#include "libc.h"
#include "vmkernel.h"
#include "splock.h"
#include "world.h"
#include "post.h"
#include "statusterm.h"


#define LOGLEVEL_MODULE Post
#define LOGLEVEL_MODULE_LEN 4
#include "log.h"

#define MAX_POSTCALLBACKS 16
#define MAX_NAMELEN 16

typedef struct POSTCallbackEntry {
   char name[MAX_NAMELEN];
   POSTCallback *callback;
   void *clientData;
} POSTCallbackEntry;

static POSTCallbackEntry CallbackEntries[MAX_POSTCALLBACKS];
static int numCallbackEntries = 0;

static SP_Barrier POSTBarrier;
static SP_SpinLock POSTLock;

static void POSTStartPOSTWorlds(void);
static void postFn(void *data);
static void POSTRun(void);

static Bool POSTDone = TRUE;

/*
 *----------------------------------------------------------------------
 *
 * POST_Start --
 *
 *      Initialize the POST module, make it ready for calling 
 *      the POST callbacks and run POST by firing off test worlds
 *
 * Results:
 *      None
 *
 * Side effects:
 *      POSTLock, and POSTBarrier
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
POST_Start(void)
{
   StatusTerm_Printf("Starting vmkernel power-on self-tests:\n");

   SP_InitLock("POST Lock", &POSTLock, SP_RANK_LEAF);
   SP_InitBarrier("POST Barrier", numPCPUs, &POSTBarrier);

#define START_POST_WORLDS
#ifdef START_POST_WORLDS
   POSTDone = FALSE;
   POSTStartPOSTWorlds();
#else
   POSTDone = TRUE;
#endif

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * POST_IsDone --
 *
 * 	Check whether POST is still ongoing
 *
 * Results:
 *      VMK_OK or VMK_STATUS_PENDING
 *
 * Side effects:
 *      none
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
POST_IsDone(void)
{
   return POSTDone ? VMK_OK : VMK_STATUS_PENDING;
}


/*
 *----------------------------------------------------------------------
 *
 * POST_Register --
 *
 *      Register a callback for power on self test
 *
 * Results:
 *      TRUE if sucessful, FALSE otherwise
 *
 * Side effects:
 *      numCallbackEntries, CallbackEntries
 *
 *----------------------------------------------------------------------
 */
Bool
POST_Register(char *name,
	      POSTCallback *callback,
	      void *clientData)
{
   POSTCallbackEntry *entry = &(CallbackEntries[numCallbackEntries++]);
   if (numCallbackEntries > MAX_POSTCALLBACKS) {
      Warning("%s: too many entries", name);
      return FALSE;
   }

   strncpy(entry->name, name, MAX_NAMELEN);
   entry->callback = callback;
   entry->clientData = clientData;

   return TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * POSTStartPOSTWorlds --
 *
 *      Start one world per pcpu to run power on self tests
 *
 * Results:
 *      None
 *
 * Side effects:
 *      yes
 *
 *----------------------------------------------------------------------
 */
static void
POSTStartPOSTWorlds(void)
{
   PCPU i;

   for (i = 0; i < numPCPUs; i++) {
      World_InitArgs args;
      Sched_ClientConfig sched;
      World_Handle *postWorld;
      char name[16];

      // generate unique name
      snprintf(name, sizeof name, "test%u", i);

      Sched_ConfigInit(&sched, SCHED_GROUP_NAME_SYSTEM);
      Sched_ConfigSetCpuAffinity(&sched, CPUSCHED_AFFINITY(i));

      // create POST system world
      World_ConfigArgs(&args, name, WORLD_POST, WORLD_GROUP_DEFAULT, &sched);
      if (World_New(&args, &postWorld) != VMK_OK) {
         Warning("World_New failed");
         return;
      }

      Sched_Add(postWorld, postFn, NULL);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * postFn --
 *
 *      Wrapper for Power on self test function. 
 *
 * Results:
 *      Does not return
 *
 * Side effects:
 *      none
 *
 *----------------------------------------------------------------------
 */
static void
postFn(UNUSED_PARAM(void *data))
{
   CpuSched_DisablePreemption();
   ENABLE_INTERRUPTS();

   POSTRun();
   
   World_Exit(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * POSTRun --
 *
 *      Power on self test function.  Put the tests here
 *
 * Results:
 *      None
 *
 * Side effects:
 *      yes
 *
 *----------------------------------------------------------------------
 */
static Atomic_uint32 numFailures = {0};
static uint8 pcpuPresent[MAX_PCPUS];

#define POSTENTRY(s)   if (myID == HOST_PCPU) { \
                           Log("Testing %s ...", s); \
                           StatusTerm_Printf("Testing %s ...\n", s); \
                       }
#define POSTFAILURE(s) Log("%s failed on %u", s, MY_PCPU);
#define POSTSUCCESS(s) Log("%s test passed", s);

static void
POSTRun(void)
{
   PCPU myID;
   int i;

   // get an ID
   SP_Lock(&POSTLock);
   myID = MY_PCPU;
   pcpuPresent[MY_PCPU]++;
   SP_Unlock(&POSTLock);

   SP_SpinBarrier(&POSTBarrier);
   if (myID == HOST_PCPU) {
      PCPU p;

      Log("********** POST: Running tests **********");
      for (p = 0; p < numPCPUs; p++) {
	 if (pcpuPresent[p] != 1) {
	    Warning("PCPU %u present = %d", p, pcpuPresent[p]);
	 }
      }
   }

   // target test list: spinlocks (done) Semaphore (done), IM (done),
   //			timer (done), rtc (done), helper (done)
   //			world (done) alloc and sched
   //                   TLB, memalloc, KSEG(done), KVMAP(done), memmap
   //                   RPC (needs code in devcnx.c to support this)
   for (i = 0; i < numCallbackEntries; i++) {
      POSTCallbackEntry *entry = &(CallbackEntries[i]);
      Bool passed = TRUE;
      if (! entry->callback) {
	 continue;
      }
      SP_SpinBarrier(&POSTBarrier);
      POSTENTRY(entry->name);
      if (! entry->callback(entry->clientData, myID, &POSTLock, &POSTBarrier)) {
	 Atomic_Inc(&numFailures);
	 POSTFAILURE(entry->name);
	 passed = FALSE;
      }
      SP_SpinBarrier(&POSTBarrier);
      if (passed && myID == HOST_PCPU) {
         POSTSUCCESS(entry->name);
      }
   }

   // report number of failures
   SP_SpinBarrier(&POSTBarrier);
   if (numFailures.value) {
      if (myID == HOST_PCPU) {
	 Log("POST encountered %d failures", numFailures.value);
      }
   }

   if (myID == HOST_PCPU) {
      Log("********** POST: Done  ******************");
      POSTDone = TRUE;
      StatusTerm_Printf("Vmkernel power-on self-tests done.\n\n");
   }
}
