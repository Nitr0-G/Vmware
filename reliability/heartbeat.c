/************************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. 
 * -- VMware Confidential
 ***********************************************************/

/*
 * heartbeat.c --
 *
 *      This module detects CPU lockups in a MP system.
 *      The functionality is similar to that of a watchdog timer
 *      in a system. The difference between this and a watchdog
 *      timer in our system is that, the watchdog timer issues
 *      periodic NMIs irrespective of whether a CPU is locked up 
 *      or not, whereas the heartbeat issues an NMI only if a CPU 
 *      is locked up. We do not want unnecessary NMIs in the
 *      system. The current implementation of the watchdog can be 
 *      dispensed of with the addition of the heartbeat.
 *
 */

/* Includes */

#include "vmkernel.h"
#include "proc_dist.h"
#include "vm_atomic.h"
#include "apic.h"
#include "timer.h"
#include "memalloc.h"
#include "world.h"
#include "parse.h"
#include "sharedArea.h"
#include "heartbeat.h"
#include "vsiDefs.h"
#include "reliability_vsi.h"
#define LOGLEVEL_MODULE Heartbeat
#include "log.h"
                                                                                                   
/* globals */

typedef struct HeartbeatInfo {
   SP_SpinLock    lock;
   uint64         timestampInMS; 
   uint64         lastNMISentAt; 
   uint64         maxDelayBetweenTimestamps;
   uint32         nmiCount;
   Timer_Handle   handle;
} HeartbeatInfo;

static HeartbeatInfo *heartbeatInfo;
static Bool heartbeatTurnedOn   = FALSE;
static uint64 heartbeatInterval = 10000; //in msecs. default 10 seconds
static uint64 heartbeatTimeout  = 60;    //in secs. default 60 seconds.

/* Function Declarations */

VMK_ReturnStatus Heartbeat_StatusGet(VSI_NodeID, VSI_ParamList*, VSI_BOOL *);
VMK_ReturnStatus Heartbeat_StatusSet(VSI_NodeID, VSI_ParamList*, VSI_ParamList*);
VMK_ReturnStatus Heartbeat_PCPUList(VSI_NodeID, VSI_ParamList *, VSI_ParamList *);
VMK_ReturnStatus Heartbeat_InfoGet(VSI_NodeID, VSI_ParamList*, Heartbeat_InfoStruct*);
static void HeartbeatTurnOn(void);
static void HeartbeatTurnOff(void);
static void HeartbeatDetectCPULockups(void *, Timer_AbsCycles);
VMK_ReturnStatus Heartbeat_WorldInit(World_Handle *);
void Heartbeat_WorldCleanup(World_Handle *);
void Heartbeat_Init(void);

/*
 *-----------------------------------------------------------------------------
 *
 * Heartbeat_StatusGet --
 * 
 *     Returns the status of the heartbeat, whether it is running or not.
 *
 * Results:
 *     VMK_OK
 *
 * Side effects:
 *     'data' is populated whether the heartbeat is running or not.
 *
 *-----------------------------------------------------------------------------
 */

VMK_ReturnStatus
Heartbeat_StatusGet(VSI_NodeID             nodeID,
                    VSI_ParamList         *instanceArgs, 
                    VSI_BOOL              *data)
{
   LOG(0,"In heartbeat Status get");  
   *data = heartbeatTurnedOn;
   return VMK_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 * Heartbeat_StatusSet --
 * 
 *     Sets the status of the heartbeat to be running or not-running. The 
 *
 * Results:
 *     VMK_OK
 *
 * Side effects:
 *     heartbeat is stopped or started.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
Heartbeat_StatusSet(VSI_NodeID     nodeID,
                    VSI_ParamList *instanceArgs, 
                    VSI_ParamList *inputArgs)
{
   VSI_Param        *param;
   int               choice;
   
   param       = VSI_ParamListGetParam(inputArgs, 0);

   if ( param->type != VSI_PARAM_INT64) {
      return VMK_BAD_PARAM;
   }

   choice      = VSI_ParamGetInt(param); 
   
   LOG(0,"In heartbeat Status set, choice = %d", choice);  
   switch (choice) {
      case 0:  
         HeartbeatTurnOff();
         break;
      case 1:
         HeartbeatTurnOn();
         break;
      default:
         Warning("invalid argument.");
         return VMK_BAD_PARAM;
   } 
   return VMK_OK;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Heartbeat_PCPUList --
 *
 *     Gets the list of PCPUs on the machine for the sysinfo caller.
 *
 * Results:
 *     VMK_OK if all is well, VMK_BUSY on any error condition. 
 *
 * Side effects:
 *     instanceListOut is populated with the list of PCPUs.
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus 
Heartbeat_PCPUList(VSI_NodeID    nodeID, 
                  VSI_ParamList *instanceArgs, 
                  VSI_ParamList *instanceListOut)
{
   int i;
   VMK_ReturnStatus  status = VMK_OK;
   
   for (i = 0 ; i < numPCPUs; i++ ) {
      status = VSI_ParamListAddInt(instanceListOut, i);
      if (status != VMK_OK) {
         break;
      }
   }
   return status;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Heartbeat_InfoGet --
 * 
 *     Returns heartbeat information for a particular PCPU.
 *
 * Results:
 *     VMK_OK
 *
 * Side effects:
 *     'data' is populated with heartbeat information.
 *
 *-----------------------------------------------------------------------------
 */

VMK_ReturnStatus
Heartbeat_InfoGet(VSI_NodeID              nodeID,
                  VSI_ParamList          *instanceArgs, 
                  Heartbeat_InfoStruct   *data)
{
   VSI_Param            *param;
   int                   myPCPUNum;

   param       = VSI_ParamListGetParam(instanceArgs, 0);
   if ( param->type != VSI_PARAM_INT64) {
      return VMK_BAD_PARAM;
   }
   myPCPUNum   = VSI_ParamGetInt(param); 
   
   LOG(0,"In heartbeat Info Get, PCPU Num = %d", myPCPUNum);  
   if ( (myPCPUNum < 0)  || (myPCPUNum >= numPCPUs) ) {
      return VMK_BAD_PARAM;
   } 

   SP_Lock(&heartbeatInfo[myPCPUNum].lock);

   data->timestampInMS             = heartbeatInfo[myPCPUNum].timestampInMS;
   data->lastNMISentAt             = heartbeatInfo[myPCPUNum].lastNMISentAt;
   data->maxDelayBetweenTimestamps = heartbeatInfo[myPCPUNum].maxDelayBetweenTimestamps;
   data->nmiCount                  = heartbeatInfo[myPCPUNum].nmiCount;

   SP_Unlock(&heartbeatInfo[myPCPUNum].lock);

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * HeartbeatTurnOn --
 *
 *      Turns on the heartbeat
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
HeartbeatTurnOn(void)
{
   int i;
   Timer_AbsCycles deadlineTC;

   /* 
    * Register timer callback function on each CPU which sets off every 
    * HEARTBEAT_INTERVAL micro seconds
    */
   if (heartbeatTurnedOn) {
      Warning("Heartbeat Already Running");
      return;
   }
   heartbeatInterval = CONFIG_OPTION(HEARTBEAT_INTERVAL) * 1000;
   deadlineTC = Timer_GetCycles();
   for (i = 0; i < numPCPUs; i++) {

      /* stagger the timers across the CPUs */
      deadlineTC += Timer_USToTC(heartbeatInterval + 
                    (i * heartbeatInterval) / numPCPUs); 
      heartbeatInfo[i].timestampInMS = Timer_TCToMS(deadlineTC);
      heartbeatInfo[i].lastNMISentAt = 0;
      heartbeatInfo[i].handle = Timer_AddTC(i, 
                                            DEFAULT_GROUP_ID, 
                                            HeartbeatDetectCPULockups, 
                                            deadlineTC,
                                            Timer_USToTC(heartbeatInterval),
                                            NULL); 
      SP_InitLock("HeartbeatLock", &heartbeatInfo[i].lock, SP_RANK_LEAF);
   }
   heartbeatTurnedOn = TRUE;
   LOG(0,"Turned on Heartbeat");
}


/*
 *----------------------------------------------------------------------
 *
 * HeartbeatTurnOff --
 *
 *      Turns off the heartbeat
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
HeartbeatTurnOff(void)
{
   int i;
   Bool status;

   if (!heartbeatTurnedOn) {
      Warning("Heartbeat Already Turned off");
      return;
   }

   /* Remove the timer callback functions */
   for (i = 0; i < numPCPUs; i++) {
      status = Timer_Remove(heartbeatInfo[i].handle); 
   }
   heartbeatTurnedOn = FALSE;
   LOG(0,"Turned off Heartbeat");
}


/*
 *----------------------------------------------------------------------
 *
 * HeartbeatDetectCPULockups --
 *
 *      This method detects if any of the CPUs in the system are locked. 
 *      if so, sends an IPI NMI to those CPUs.
 *      Note: If all the CPUs in the system are locked the heartbeat 
 *            can't do anything. If you suspect this might happen, enable
 *            the watchdog timer,before this happens.
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
HeartbeatDetectCPULockups(void* data, UNUSED_PARAM(Timer_AbsCycles timestamp))
{
   int i;
   uint64 timestampInMS;
   int myPCPUNum = MY_PCPU;

   ASSERT(!CpuSched_IsPreemptible());
   timestampInMS = Timer_TCToMS(timestamp);

   /* Disable hearbeat checking if we entered debuger earlier */
   if (Debug_EverInDebugger()) {
      return;
   }

   for (i = 0; i < numPCPUs; i++) {
      if (i == myPCPUNum) {
         uint64 delay;
         
         delay = timestampInMS - heartbeatInfo[i].timestampInMS;
         SP_Lock(&heartbeatInfo[i].lock);
         /* update the current CPU's timestamp delays and current timestamp */
         if (delay > heartbeatInfo[i].maxDelayBetweenTimestamps) {
             heartbeatInfo[i].maxDelayBetweenTimestamps = delay;
         } 
         heartbeatInfo[i].timestampInMS = timestampInMS;
         SP_Unlock(&heartbeatInfo[i].lock);
      } else {
         uint64 remoteCPUTimestamp;
         uint64 remoteCPUNMISentAt;
         Bool sendNMI = FALSE;

         if (!SP_TryLock(&heartbeatInfo[i].lock)) {  
            continue; //Some other cpu is already checking for lockup.
         }
         heartbeatTimeout    = CONFIG_OPTION(HEARTBEAT_TIMEOUT) * 1000;
         remoteCPUTimestamp  = heartbeatInfo[i].timestampInMS;
         remoteCPUNMISentAt  = heartbeatInfo[i].lastNMISentAt;
          
         if (remoteCPUTimestamp > timestampInMS) {
            SP_Unlock(&heartbeatInfo[i].lock);
            continue;
         }

         /* Check if we have timedout,to send NMI to the remote CPU */

         if (remoteCPUTimestamp > remoteCPUNMISentAt) {

            /*
             * Either 
             * 1) we are sending an NMI to that PCPU for the first time
             *           OR 
             * 2) the PCPU in question recovered from an earlier lockup 
             *    (in this case the remoteCPUNMISentAt would have a stale value)
             *    and we should treat this same as case 1.
             */
           if ((timestampInMS - remoteCPUTimestamp) > heartbeatTimeout) {
             sendNMI = TRUE;
           }
         } else {

            /*
             * we have sent an NMI recently, we need to only send
             * the next NMI at twice the timeout value we had earlier
             */
            if (((timestampInMS - remoteCPUTimestamp) / heartbeatTimeout) >
                2 * ((remoteCPUNMISentAt - remoteCPUTimestamp) / heartbeatTimeout)) {

               sendNMI = TRUE;
            }
         } 

         if (sendNMI) { 
            heartbeatInfo[i].lastNMISentAt = timestampInMS;
            heartbeatInfo[i].nmiCount++;
         }

         SP_Unlock(&heartbeatInfo[i].lock);

         if (sendNMI) {
            World_Handle *world;

            world = World_Find(prdas[i]->runningWorld->worldID);
            if (world != NULL) {

               if (World_IsVMMWorld(world)) {

                  /*
                   * Set vmkernel-monitor shared area flag to 
                   * indicate that the NMI is due to the heartbeat.
                   * Incase monitor is running on the CPU to which
                   * we are going to send the NMI, it can check this 
                   * flag to know if the cause of the NMI is heartbeat.
                   * Note: One issue is this flag will be acknowledged/cleared 
                   *       in the Monitor NMI handler. If for some reason 
                   *       the locked up pcpu recovers and some other
                   *       world is scheduled by the time the NMI reaches
                   *       that cpu, this flag will not be cleared. 
                   *       Though this possibility is remote, the monitor 
                   *       will mistakenly percieve it's next NMI to be
                   *       from heartbeat. I assume there are no dire 
                   *       consequences for this rare case.
                   */
                   *(world->group->vmm.nmiFromHeartbeat) = TRUE;
               }
               World_Release(world);
            }
            SysAlert("PCPU %d didn't have a heartbeat for %Ld seconds. *may* "\
                     "be locked up", i, 
                     (timestampInMS - remoteCPUTimestamp)/1000);
            APIC_SendNMI(i);
         }
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * Heartbeat_WorldInit --
 *
 *      Initialize NMI shared area between vmkernel and the vmm NMI handler.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
Heartbeat_WorldInit(World_Handle *world)
{
   Bool         *nmiFromHeartbeat;
   ASSERT(World_IsVMMWorld(world));
   /* Initialize NMI shared area between vmkernel and vmm for the heartbeat. */
   nmiFromHeartbeat = SharedArea_Alloc(world, "nmiFromHeartbeat", sizeof(Bool));

   World_VMMGroup(world)->nmiFromHeartbeat = nmiFromHeartbeat;

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Heartbeat_WorldCleanup --
 *
 *      Cleanup heartbeat shared area data.
 *      Effectively no items to free. Kept this method for code consistency.
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
Heartbeat_WorldCleanup(World_Handle *world)
{
}


/*
 *----------------------------------------------------------------------
 *
 * Heartbeat_Init --
 *
 *      Registers for a timer callback function on each CPU to periodically 
 *      check if other CPUs are locked up.
 * 
 * Results:
 *      none
 *
 * Side effects:
 *      none
 *
 *----------------------------------------------------------------------
 */

void
Heartbeat_Init(void)
{

   /* Do not run this program on a UP machine */
   if ( numPCPUs == 1 ) {
      return;
   }

   /* Declare Datastructures for each CPU */
   heartbeatInfo = (HeartbeatInfo*) Mem_Alloc(numPCPUs * sizeof(HeartbeatInfo));
   memset(heartbeatInfo, 0, numPCPUs * sizeof(HeartbeatInfo));

   /* Turn on the heartbeat by default */
   HeartbeatTurnOn();
}
