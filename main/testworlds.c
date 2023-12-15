/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * testworlds.c
 *
 *	Provides synthetic worlds for testing purposes.
 *   
 *   To add a new test world, you need to add a new TestWorldType
 *   structure to the testWorldBuiltins array. It should contain one
 *   boolean value (wantNewWorld) and three function pointers:
 * 
 *   - startFunc: Launches the testworld(s) based on parameters in 
 *                argc and argv. If wantNewWorld is TRUE, this startFunc
 *                will be called from its own UP world/thread. If wantNewWorld
 *                is false, this function must set up its own new world(s)
 *                (see TestWorldsBasicVsmpStart for an example).
 *   - stopFunc:  Should kill the running test worlds of the given type.
 *   - readFunc:  Proc read handler, prints usage or status information.
 */


/*
 * Includes
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "vmkernel.h"
#include "world.h"
#include "timer.h"
#include "parse.h"
#include "util.h"
#include "event.h"
#include "cpusched.h"
#include "memalloc.h"

#define LOGLEVEL_MODULE TestWorlds

#include "log.h"
#include "testworlds_ext.h"
#include "testworlds.h"

#define TESTWORLDS_MAX_ARGS 64
#define TESTWORLDS_NUM_TYPES (sizeof(testWorldBuiltins)/sizeof(TestWorldType))
// time, in milliseconds, between timer interrupts
#define TESTWORLDS_TIMER_WAIT (10)
#define TESTWORLDS_MAX_REASONABLE_RAND (1000)

#define TESTWORLDS_MAGIC_GUARD (0x112779)

#define TESTWORLDS_MIN_RAND_PRECISION (100)

// root directory for all test world proc nodes
static Proc_Entry testParentDir;

// set to TRUE to shut down running test worlds
static volatile Bool testStop = FALSE;

/* ************ Basic test VSMP ************ */
/* The "basic" test vsmp runs in a loop, spinning for a random time
 * (uniform distribution, average = msecRunAvg) then sleeping
 * for a random time (uniform distribution average = msecWaitAvg). */

typedef struct {
   // allocation
   uint32 nvcpus;
   int32 shares;
   CpuMask affinity;

   // workload
   uint32 msecRunAvg;
   uint32 msecWaitAvg;
   uint32 rndSeed;
} TestWorldBasicVsmpConfig;


static Proc_Entry timerVsmpProcEnt;
static Proc_Entry basicVsmpProcEnt;


/*
 *-----------------------------------------------------------------------------
 *
 * TestWorlds_NewVsmp --
 *
 *	Creates and starts a new (possibly SMP) VM with "numVcpus" vcpus.
 *	The "data" parameter will be passed as an argument to the start
 *	function "sf", for each vcpu.  The name of the i-th vcpu will be
 *	the i-th elements of "vcpuNames".  The "groupName", "shares", and
 *	"numVcpus" parameters specify initial resource management controls.
 *
 *	Caveats: some failure paths may cause the VM to be partially
 *	created (e.g. subset of vcpus), or partially added to the scheduler.
 *
 * Results:
 *	World group identifier for created VM, 0 on failure.
 *
 * Side effects:
 *	Creates new worlds and adds them to the scheduler.
 *
 *-----------------------------------------------------------------------------
 */
World_GroupID
TestWorlds_NewVsmp(CpuSched_StartFunc sf,
                   void *data,
                   char *vcpuNames[],
                   char *groupName,
                   uint32 shares,
                   uint8 numVcpus)
{
   World_Handle *world;
   World_ID idResult = 0;
   unsigned i;
   World_ID worldGroup = WORLD_GROUP_DEFAULT;
   World_Handle *leaderWorld = NULL;
   Sched_ClientConfig cfg;
   
   // configure world, make runnable
   ASSERT(groupName != NULL);
   Sched_ConfigInit(&cfg, groupName);
   cfg.group.createContainer = TRUE;
   cfg.group.cpu.shares = shares;

   cfg.cpu.numVcpus = numVcpus;

   // create world per vcpu
   for (i = 0; i < numVcpus; i++) {
      World_InitArgs args;

      World_ConfigArgs(&args, vcpuNames[i], WORLD_SYSTEM | WORLD_TEST,
                       worldGroup, &cfg);
      if (World_New(&args, &world) != VMK_OK) {
         Warning("unable to create world, name=%s", vcpuNames[i]);
         return (0);
      } 

      if (i == 0) {
         worldGroup = World_GetGroupLeaderID(world);
         leaderWorld = world;
      }
      
      if (Sched_Add(world, sf, data) != VMK_OK) {
         // issue warning, cleanup world
         Warning("Unable to start world %s", vcpuNames[i]);

         World_Destroy(world->worldID, FALSE);
         return (0);
      } else {
         idResult = world->worldID;
      }

   }

   return (worldGroup);
}


/*
 *-----------------------------------------------------------------------------
 *
 * TestWorldsHelpProcRead --
 *
 *  Prints a "usage" guide for the test worlds proc node
 *
 * Results:
 *     Returns VMK_OK
 *
 * Side effects:
 *     Prints to the buffer "buf"
 *
 *-----------------------------------------------------------------------------
 */
static int
TestWorldsHelpProcRead(UNUSED_PARAM(Proc_Entry* e), char* buf, int* len)
{
   // initialize
   *len = 0;

   // report usage
   Proc_Printf(buf, len,
               "commands:\n"
               "  start <nvcpus> <shares> <msecRun> <msecWait>\n"
               "  stop\n");

   return (VMK_OK);
}

/*
 *-----------------------------------------------------------------------------
 *
 * TestWorldsBasicVsmpLoop --
 *
 *  Main loop for test world: spin then wait for random times,
 *  until somebody sets testStop to TRUE;
 *
 * Results:
 *     Void. Only returns after test world dies
 *
 * Side effects:
 *     None
 *
 *-----------------------------------------------------------------------------
 */
static void
TestWorldsBasicVsmpLoop(void *data)
{
   TestWorldBasicVsmpConfig test = *((TestWorldBasicVsmpConfig *) data);
   uint64 count;

   CpuSched_EnablePreemption();
   ENABLE_INTERRUPTS();

   // initialize
   count = 0;
   test.rndSeed = Util_RandSeed();

   Log("Started basic test vsmp");
   
   // infinite loop
   while (vmkernelLoaded && !testStop) {
      uint32 msecRun, msecWait;

      // run for random period
      if (test.msecRunAvg > 0) {
         test.rndSeed = Util_FastRand(test.rndSeed);
         msecRun = test.rndSeed % test.msecRunAvg;
         Util_Udelay(msecRun * 1000);
      }

      if (test.msecWaitAvg > 0) {
         test.rndSeed = Util_FastRand(test.rndSeed);
         msecWait = test.rndSeed % test.msecWaitAvg;
         if (msecWait > 0) {
            // disable preemption while waiting for "busy wait" optimization
            Bool preemptible = CpuSched_DisablePreemption();
            CpuSched_Sleep(msecWait);
            CpuSched_RestorePreemption(preemptible);
         }
      }

      count++;
   }

   // reclaim storage (but not if vcpus > 1), simply leak for now
   if (World_IsGroupLeader(MY_RUNNING_WORLD)) {
      Mem_Free(data);
   }

   Log("terminating basic vsmp: count=%Lu", count);

   World_Exit(VMK_OK);
}


/*
 * ------------------------------------------------------------------------
 *
 * TestWorldsBasicVsmpStart --
 *
 *	Creates a test Vsmp according to the given arguments:
 *	argv = "start" nvcpus nshares msecRun msecWait groupName.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Creates and runs one or more vcpus as specified.
 *
 *-------------------------------------------------------------------------
 */
static void
TestWorldsBasicVsmpStart(int argc, char** argv)
{
   TestWorldBasicVsmpConfig *test;
   char *names[MAX_VCPUS];
   char *groupName;
   uint32 i, nvcpus;

   ASSERT(!CpuSched_IsPreemptible());
   
   if ((argc < 5) || (argc > 6)) {
      Log("invalid start command");
      return;
   }

   test = Mem_Alloc(sizeof(TestWorldBasicVsmpConfig));
   if (test == NULL) {
      Warning("Could not allocate memory for test world config");
      return;
   }

   // parse "<nvcpus>" arg
   if (Parse_Int(argv[1], strlen(argv[1]), &nvcpus) != VMK_OK 
       || nvcpus > numPCPUs) {
      Log("invalid start nvcpus: %s", argv[1]);
      Mem_Free(test);
      return;
   }
   test->nvcpus = nvcpus;
   
   // parse "<shares>" arg
   if (Parse_Int(argv[2], strlen(argv[2]), &test->shares) != VMK_OK) {
      Log("invalid start shares: %s", argv[2]);
      Mem_Free(test);
      return;
   }

   // parse "<msecRun>" arg
   if (Parse_Int(argv[3], strlen(argv[3]), &test->msecRunAvg) != VMK_OK) {
      Log("invalid start msecRun: %s", argv[3]);
      Mem_Free(test);
      return;
   }

   // parse "<msecWait>" arg
   if (Parse_Int(argv[4], strlen(argv[4]), &test->msecWaitAvg) != VMK_OK) {
      Log("invalid start msecWait: %s", argv[4]);
      Mem_Free(test);
      return;
   }

   // parse optional "<groupName>" arg
   if ((argc > 5) && (argv[5] != NULL)) {
      groupName = argv[5];
   } else {
      groupName = SCHED_GROUP_NAME_LOCAL;
   }

   testStop = FALSE;

   // debugging
   VmLog(MY_RUNNING_WORLD->worldID,
         "group=%s, nvcpus=%u, shares=%d, affinity=%x, "
         "msecRun=%u, msecWait=%u",
         groupName, test->nvcpus, test->shares, test->affinity,
         test->msecRunAvg, test->msecWaitAvg);

   // setup per-vcpu names
   for (i=0; i < nvcpus; i++) {
      // generate name
      names[i] = Mem_Alloc(TESTWORLDS_MAX_NAME_LEN);
      ASSERT_NOT_IMPLEMENTED(names[i] != 0);
      snprintf(names[i], TESTWORLDS_MAX_NAME_LEN, "tw-%d-%d.%u",
               test->msecRunAvg, test->msecWaitAvg, i);
   }

   TestWorlds_NewVsmp(TestWorldsBasicVsmpLoop,
                      test,
                      names,
                      groupName,
                      test->shares,
                      test->nvcpus);
   
   // cleanup after ourselves
   for (i=0; i < nvcpus; i++) {
      Mem_Free(names[i]);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * TestWorldsBasicVsmpStop --
 *
 *  Causes ALL test worlds to stop. May not happen immediately,
 *  because worlds only check their "testStop" flag once per iteration
 *  through their work loops.
 *
 * Results:
 *     Returns none
 *
 * Side effects:
 *     Sets flag
 *
 *-----------------------------------------------------------------------------
 */
static void
TestWorldsBasicVsmpStop(UNUSED_PARAM(int argc), UNUSED_PARAM(char** argv))
{
   Log("stopping basic vsmp");
   testStop = TRUE;
}
               
/* ************ Timer-based test world ************ */

/* The timer-based test world is slightly more complicated, but
 * it more closely approximates the behavior of a real VM. 
 * 
 * - Whenever the world is doing nothing, it waits in the WAIT_IDLE state.
 * - Every 10 ms a fake timer interrupt occurs, waking the world
 * - "Events" arrive according to a Poisson process, with an average inter-event
 *   arrival time of "usWait". When such an event arrives, the eventPending flag is
 *   set, and the world is woken from its slumber. 
 * - When the world wakes, if the eventPending flag is true, it does a random amount
 *   of work (spinning in a loop), with the work time drawn from an exponential 
 *   distribution with an average of "usRun" */

/*
 *-----------------------------------------------------------------------------
 *
 * TestWorlds_ExponentialRand --
 *
 *  Returns a random integer from the exponential distribution, with an average
 *  of "randAvg." randAvg should not be 0.
 * 
 *  Precision varies based on the size of "randAvg," because the algorithm
 *  used to generate these numbers can take a long time to generate accurate
 *  numbers with high averages. For instance, random values over 2000 are only
 *  random in their thousands places, while random values between 200 and 2000 are
 *  random in their hundreds places.
 *
 * Results:
 *     Returns random integer with desired average.
 *
 * Side effects:
 *     Modifies seed
 *
 *-----------------------------------------------------------------------------
 */
static uint32
TestWorlds_ExponentialRand(uint32* seed, uint32 randAvg)
{
   uint32 curRand, precision, myAvg;
   uint32 numReps = 0;
   ASSERT(randAvg != 0);

   // don't like to do more than 20 reps on average, so
   // reduce precision for very large numbers (worst case precision=1000)
   // currently, we take about 80 cycles per rep on a p4
   if (randAvg <= 20) {
      precision = 1;
   } else if (randAvg <= 200) {
      precision = 10;
   } else if (randAvg <= 2000) {
      precision = 100;
   } else {
      precision = 1000;
   }
   myAvg = randAvg / precision;
   
   // Keep drawing random numbers, stopping with a probability
   // of (1 / randAvg). This generates an exponential distribution,
   // albeit slowly. If we could use floating point math in the kernel,
   // we could do this much faster...
   do {
      numReps++;
      curRand = Util_FastRand(*seed);
      *seed = curRand;
   } while ((curRand % myAvg) != 0);

   return numReps * precision;
}

/* One of these structs exists per vcpu of the each timer world. */
typedef struct {
   // input params
   uint32 event;
   uint32 usWait;
   uint32 usRun;

   // data storage
   uint32 preGuard;
   uint32 seed;
   uint32 postGuard;
   Bool eventPending;
   uint32 eventsHandled;
   Timer_Handle eventTimer;
   Timer_Handle timerTimer;
   Atomic_uint32 useCount;
   Bool worldDying;
} TimerWorldData;


/*
 *-----------------------------------------------------------------------------
 *
 * TimerWorldTimerCallback --
 *
 *  Fake timer interrupt handler. Wake up the world.
 *
 * Results:
 *     Returns none
 *
 * Side effects:
 *     Wakes test world
 *
 *-----------------------------------------------------------------------------
 */
static void
TimerWorldTimerCallback(void* data, UNUSED_PARAM(Timer_AbsCycles timestamp))
{
   TimerWorldData *eventData = (TimerWorldData*) data;
   uint32 event = eventData->event;

   if (!vmkernelLoaded) {
      return;
   }
   
   if (eventData->worldDying) {
      // we're dying, so remove the timer
      Bool res = Timer_Remove(eventData->timerTimer);
      if (res) {
         Atomic_Dec(&eventData->useCount);
      } 
   }

   CpuSched_Wakeup(event);
}

/*
 *-----------------------------------------------------------------------------
 *
 * TimerWorldEventCallback --
 *
 *  Tell test world to handle an "event", which would probably be
 *  an interrupt in a real VM. Sets eventPending flag, so test world should
 *  do some work on wakeup.
 *
 * Results:
 *     Returns none
 *
 * Side effects:
 *     Wakes test world
 *
 *-----------------------------------------------------------------------------
 */
static void
TimerWorldEventCallback(void* data, UNUSED_PARAM(Timer_AbsCycles timestamp))
{
   uint64 waitTime;
   TimerWorldData *eventData = (TimerWorldData*) data;
   uint32 event = eventData->event;

   if (!vmkernelLoaded) {
      return;
   }

   if (!eventData->worldDying) {
      // post the event to the world
      eventData->eventPending = TRUE;
      
      // reinstall the timer
      if (eventData->usWait > 0) {
         ASSERT(eventData->preGuard == TESTWORLDS_MAGIC_GUARD 
                && eventData->postGuard == TESTWORLDS_MAGIC_GUARD);
         waitTime = (uint64)TestWorlds_ExponentialRand(&eventData->seed,
                                               eventData->usWait);
      } else {
         waitTime = 0;
      }
      
      eventData->eventTimer = Timer_AddHiRes(MY_PCPU,
                                             TimerWorldEventCallback,
                                             waitTime,
                                             TIMER_ONE_SHOT,
                                             eventData);

   } else {
      Atomic_Dec(&eventData->useCount);
   }


   
   CpuSched_Wakeup(event);
}

/*
 *-----------------------------------------------------------------------------
 *
 * TestWorldsTimerWorldLoop --
 *
 *  Main loop for timer world. Sleep, maybe handle an event, then sleep again.
 *  Stops only when testStop is set to TRUE;
 *
 * Results:
 *     Returns none
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static void
TestWorldsTimerWorldLoop(void *data)
{
   uint32 timerWait = TESTWORLDS_TIMER_WAIT;
   TimerWorldData eventData = *((TimerWorldData*) data);

   CpuSched_EnablePreemption();
   ENABLE_INTERRUPTS();

   eventData.event = (uint32) MY_RUNNING_WORLD;
   eventData.eventsHandled = 0;
   eventData.worldDying = FALSE;

   // used by this loop, plus two timer callbacks
   Atomic_Write(&eventData.useCount, 3);

   // seed with the timestamp value
   eventData.seed = Util_RandSeed();
   eventData.preGuard = TESTWORLDS_MAGIC_GUARD;
   eventData.postGuard = TESTWORLDS_MAGIC_GUARD;

   // setup the fake timer callback
   eventData.timerTimer = Timer_Add(MY_PCPU, 
                                     TimerWorldTimerCallback, 
                                     timerWait, 
                                     TIMER_PERIODIC, 
                                     (void*) &eventData);

   // fire the first event with a timeout of 0
   eventData.eventTimer = Timer_Add(MY_PCPU, 
                                     TimerWorldEventCallback, 
                                     0, 
                                     TIMER_ONE_SHOT, 
                                     (void*) &eventData);

   while (vmkernelLoaded && !testStop) {
      // just keep waiting forever, or until we're supposed to stop
      if (eventData.usWait != 0) {
         // disable preemption while waiting for "busy wait" optimization
         Bool preemptible = CpuSched_DisablePreemption();
         CpuSched_WaitIRQ(eventData.event, CPUSCHED_WAIT_IDLE, NULL, 0);
         CpuSched_RestorePreemption(preemptible);
      }

      // note that run time of 0 is not permitted (checked in TestWorldsTimerVsmpStart)
      ASSERT(eventData.usRun != 0);

      if (eventData.eventPending) {
         uint32 delayTime;

         ASSERT(eventData.preGuard == TESTWORLDS_MAGIC_GUARD 
                && eventData.postGuard == TESTWORLDS_MAGIC_GUARD);
         // the little race here (with TimerWorldEventCallback) is fine         
         delayTime = TestWorlds_ExponentialRand(&eventData.seed, eventData.usRun);
         eventData.eventPending = FALSE;
         Log_Event("timerworld-event", delayTime, EVENTLOG_TESTWORLDS);
         Util_Udelay(delayTime);
         eventData.eventsHandled++;
      }
      Log_Event("timerworld-up", MY_RUNNING_WORLD->worldID, EVENTLOG_TESTWORLDS);
   }
   
   Atomic_Dec(&eventData.useCount);

   eventData.worldDying = TRUE;

   // spin, waiting for timers to complete
   // note that interrupts are on and preemption is enabled, so this is OK
   while (Atomic_Read(&eventData.useCount) > 0) {
      PAUSE();
   }

   Log("killing timertest world\n");

   if (World_IsGroupLeader(MY_RUNNING_WORLD)) {
      Mem_Free(data);
   }

   World_Exit(VMK_OK);
}


/*
 *-----------------------------------------------------------------------------
 *
 * TestWorldsParseTime --
 *
 *      Parses the integer in "buf" like Parse_Int, but allows the suffix 'u'
 *      to be included to indicate microseconds (leave as-is) or 'm' to
 *      specify the result is in milliseconds (multiply by 1000).
 *
 * Results:
 *      Returns VMK_OK on success, error code otherwise.
 *      Stores value from string "buf" into "value", multiplying by
 *      1000 if the value is in milliseconds.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
TestWorldsParseTime(char *buf, int len, uint32 *value)
{
   VMK_ReturnStatus res;
   int realLen = len, factor=1000;

   // default to milliseconds, but allow 'u' suffix
   // to indicate that units are in microseconds
   if (len > 1 && buf[len-1] == 'u') {
      realLen--;
      factor=1;
   } else if (len > 1 && buf[len-1] == 'm') {
      realLen--;
   }

   res = Parse_Int(buf, realLen, value);
   *value *= factor;

   return (res);
}

/*
 *-----------------------------------------------------------------------------
 *
 * TestWorldsTimerVsmpStart --
 *
 *  Parses params and launches a timer-based test world.
 *
 * Results:
 *     Returns none
 *
 * Side effects:
 *     vsmp created, launched
 *
 *-----------------------------------------------------------------------------
 */
static void
TestWorldsTimerVsmpStart(int argc, char** argv)
{
   uint32 i, nvcpus, nshares, avgWait, avgRun;
   char *names[MAX_VCPUS];
   TimerWorldData *testSetup;
   char *groupName;

   ASSERT(!CpuSched_IsPreemptible());

   if ((argc < 5) || (argc > 6)) {
      Log("Invalid argument for TimerVsmpStart");
      return;
   }

   if (Parse_Int(argv[1], strlen(argv[1]), &nvcpus) != VMK_OK 
      || nvcpus > numPCPUs) {
      Log("Invalid value for nvcpus: %s", argv[2]);
      return;
   }
   if (Parse_Int(argv[2], strlen(argv[2]), &nshares) != VMK_OK) {
      Log("Invalid value for nshares: %s", argv[2]);
      return;
   }
   if (TestWorldsParseTime(argv[3], strlen(argv[3]), &avgRun) != VMK_OK) {
      Log("Invalid value for avgRun: %s", argv[3]);
      return;
   }
   if (TestWorldsParseTime(argv[4], strlen(argv[4]), &avgWait) != VMK_OK) {
      Log("Invalid value for avgWait: %s", argv[4]);
      return;
   }

   // parse optional "<groupName>" arg
   if ((argc > 5) && (argv[5] != NULL)) {
      groupName = argv[5];
   } else {
      groupName = SCHED_GROUP_NAME_LOCAL;
   }

   if (avgRun > TESTWORLDS_MAX_REASONABLE_RAND) {
      Warning("it is not advisable to run timer worlds "
              "with avgRun or avgWait > %d",
              TESTWORLDS_MAX_REASONABLE_RAND);
   }
   
   if (avgRun == 0) {
      Warning("timerworlds with run time of 0 are not allowed");
      return;
   }

   testStop = FALSE;

   testSetup = (TimerWorldData*) Mem_Alloc(sizeof(TimerWorldData));
   ASSERT_NOT_IMPLEMENTED(testSetup != NULL);

   testSetup->usWait = avgWait;
   testSetup->usRun = avgRun;

   // fill in per-vcpu world name
   for (i=0; i < nvcpus; i++) {
      names[i] = Mem_Alloc(TESTWORLDS_MAX_NAME_LEN);
      ASSERT_NOT_IMPLEMENTED(names[i] != NULL);
      
      // generate name
      snprintf(names[i], TESTWORLDS_MAX_NAME_LEN, "tmw-%d-%d.%d", 
               testSetup->usRun,
               testSetup->usWait,
               i);
   }
   
   TestWorlds_NewVsmp(TestWorldsTimerWorldLoop,
                      testSetup,
                      names,
                      groupName,
                      nshares,
                      nvcpus);

   // clean up names
   for (i=0; i < nvcpus; i++) {
      Mem_Free(names[i]);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * TestWorldsTimerVsmpStop --
 *
 *  Stops ALL test worlds.
 *
 * Results:
 *     Returns none
 *
 * Side effects:
 *     Stops worlds
 *
 *-----------------------------------------------------------------------------
 */
static void 
TestWorldsTimerVsmpStop(UNUSED_PARAM(int argc), UNUSED_PARAM(char** argv))
{
   Log("Stop timer-based test worlds");
   testStop = TRUE;
}


/* ************ List of known test worlds ************ */

TestWorldType testWorldBuiltins[] = {
   { "basic", 
     1,
     &basicVsmpProcEnt, 
     TestWorldsBasicVsmpStart,
     TestWorldsBasicVsmpStop,
     TestWorldsHelpProcRead,
     FALSE
   },
   { "timer-based",
     1,
     &timerVsmpProcEnt,
     TestWorldsTimerVsmpStart,
     TestWorldsTimerVsmpStop,
     TestWorldsHelpProcRead, 
     FALSE
   },
};


/* ************  Proc node handling ************ */

static int TestWorldsProcWrite(Proc_Entry *e, char *buf, int *len);

// stores arguments for a callback
typedef struct {
   TestWorldCallback callback;
   Bool  wantNewWorld;
   int numVCPUs;
   Atomic_uint32 refCount;
   int  argc;
   char* argv[TESTWORLDS_MAX_ARGS];
   char  names[MAX_VCPUS][TESTWORLDS_MAX_NAME_LEN];
   char argBuf[0];
} TestWorldsCallbackData;


/*
 *-----------------------------------------------------------------------------
 *
 * TestWorldsStartFuncWrapper --
 *
 *  Small utility function to bridge CpuSched_StartFunc and TestWorldsCallback
 *  interfaces
 *
 * Results:
 *     Calls "data->callback"
 *
 * Side effects:
 *     None
 *
 *-----------------------------------------------------------------------------
 */
void 
TestWorldsStartFuncWrapper(void* data)
{
   TestWorldsCallbackData *cbData = (TestWorldsCallbackData*) data;

   ENABLE_INTERRUPTS();

   // run the main loop of the test world ("callback")
   cbData->callback(cbData->argc, cbData->argv);

   if (Atomic_FetchAndDec(&cbData->refCount) == 1) {
      // clean up dynamically-allocated memory
      Mem_Free(cbData);
   }

   World_Exit(VMK_OK);
}


/*
 *-----------------------------------------------------------------------------
 *
 * TestWorldsDoCallback --
 *
 *  Called from a timer-handler context (without proclock held),
 *  TestWorldsDoCallback simply calls data->callback with appropriate args
 *
 * Results:
 *     Returns none
 *
 * Side effects:
 *     Frees data to clean up
 *
 *-----------------------------------------------------------------------------
 */
static void
TestWorldsDoCallback(void* data, UNUSED_PARAM(Timer_AbsCycles timestamp))
{
   TestWorldsCallbackData *cbData = (TestWorldsCallbackData*) data;
   char *names[MAX_VCPUS];
   int i;

   ASSERT(data != NULL);

   for (i = 0; i < cbData->numVCPUs; i++) {
      names[i] = cbData->names[i];
   }

   if (cbData->wantNewWorld && !strcmp(cbData->argv[0], "start")) {
      Atomic_Write(&cbData->refCount, cbData->numVCPUs);
      TestWorlds_NewVsmp(TestWorldsStartFuncWrapper,
                         data,
                         names,
                         SCHED_GROUP_NAME_LOCAL,
                         1000,
                         cbData->numVCPUs);
   } else {
      cbData->callback(cbData->argc, cbData->argv);
      Mem_Free(cbData);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * TestWorldsProcWrite --
 *
 *  Handles a write to a test world proc node. Parses the incoming command,
 *  makes a deep copy of the arguments, and passes them to the appropriate callback.
 *
 * Results:
 *     Returns VMK_OK or VMK_BAD_PARAM
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static int
TestWorldsProcWrite(Proc_Entry *e, char *buf, int *len)
{
   int bufLen;
   char *cmd;
   TestWorldType* worldType;
   TestWorldsCallbackData *data;
   int numVCPUs, i;

   bufLen = strlen(buf) + 1;
   data =  Mem_Alloc(sizeof(TestWorldsCallbackData) + bufLen);
   if (data == NULL) {
      Warning("Could not allocate memory for test world data");
      return (VMK_NO_MEMORY);
   }

   memcpy(&data->argBuf, buf, bufLen);
   data->argc = Parse_Args(buf, data->argv, TESTWORLDS_MAX_ARGS);
   if (data->argc < 1) {
      Mem_Free(data);
      return (VMK_BAD_PARAM);
   }
   
   worldType = (TestWorldType*) e->private;

   data->wantNewWorld = worldType->wantNewWorld;
   for (i = 0; i < worldType->numVCPUs; i++) {
      snprintf(data->names[i], TESTWORLDS_MAX_NAME_LEN,  "%s.%u", worldType->name, i);
   }
   data->numVCPUs = worldType->numVCPUs;

   cmd = data->argv[0];

   if (strcmp(cmd, "stop") == 0) {
      if (worldType->stopFunc != NULL) {
         data->callback = worldType->stopFunc;
         Timer_Add(MY_PCPU, TestWorldsDoCallback, 0, TIMER_ONE_SHOT, data);
      }
   } else if (strcmp(cmd, "start") == 0) {
      if (worldType->startFunc != NULL) {
         data->callback = worldType->startFunc;
         Timer_Add(MY_PCPU, TestWorldsDoCallback, 0, TIMER_ONE_SHOT, data);
      }
   } else if (strcmp(cmd, "vcpus") == 0) {
      Parse_Int(data->argv[1], 2, &numVCPUs);
      if ((numVCPUs < 1) || (numVCPUs > MAX_VCPUS)) {
         Mem_Free(data);
         return (VMK_BAD_PARAM);
      }    
      worldType->numVCPUs = numVCPUs;
      Log("testworld type %s set to use %u VCPUs", 
          worldType->name, worldType->numVCPUs);
      Mem_Free(data);
   } else {
      Mem_Free(data);
      return (VMK_BAD_PARAM);
   }
      
   return (VMK_OK);
}

/*
 *-----------------------------------------------------------------------------
 *
 * TestWorlds_RegisterType --
 *
 *     Installs a new test world type of "testType" and sets up its proc node.
 *
 * Results:
 *     None
 *
 * Side effects:
 *     A proc node for this testType should appear.
 *
 *-----------------------------------------------------------------------------
 */
void
TestWorlds_RegisterType(TestWorldType* testType)
{
   Proc_Entry* entry = testType->procEnt;
   entry->parent = &testParentDir;
   entry->write = TestWorldsProcWrite;
   entry->read = testType->readFunc;
   entry->private = testType;
   
   Proc_RegisterHidden(testType->procEnt,
                 testType->name,
                 FALSE);
}


/*
 *-----------------------------------------------------------------------------
 *
 * TestWorlds_UnregisterType --
 *
 *     Removes the "testType" world type. This does not stop currently-running
 *     worlds of type "testType," so you should do that beforehand.
 *     
 *
 * Results:
 *     None
 *
 * Side effects:
 *     Makes relevant proc node disappear
 *
 *-----------------------------------------------------------------------------
 */
void
TestWorlds_UnregisterType(TestWorldType* testType)
{
   Proc_Entry* entry = testType->procEnt;
   
   Proc_Remove(entry);
}


/*
 *-----------------------------------------------------------------------------
 *
 * TestWorlds_Init --
 *
 *  Install proc handlers for all known test world types
 *
 * Results:
 *     Returns 
 *
 * Side effects:
 *     Modifies /proc 
 *
 *-----------------------------------------------------------------------------
 */
void
TestWorlds_Init(void)
{
   uint32 i;

   Proc_RegisterHidden(&testParentDir, "testworlds", TRUE);

   // register all known test world types
   for (i = 0; i < TESTWORLDS_NUM_TYPES; i++) {
      TestWorlds_RegisterType(&testWorldBuiltins[i]);
   }
}
