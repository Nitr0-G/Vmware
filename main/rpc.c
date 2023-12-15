/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * rpc.c --
 *
 *	Remote procedure call module.  This provides RPCs from the vmkernel or
 *	any guest world to the host.
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "vm_asm.h"
#include "x86.h"
#include "vmkernel.h"
#include "vmnix_if.h"
#include "kvmap.h"
#include "memmap.h"
#include "world.h"
#include "rpc.h"
#include "list.h"
#include "idt.h"
#include "util.h"
#include "host.h"
#include "memalloc.h"
#include "timer.h"
#include "vmkpoll.h"
#include "user.h"
#include "trace.h"

#include "proc.h"

#define LOGLEVEL_MODULE RPC

#include "log.h"

// lock ranks
#define SP_RANK_RPCLOCK SP_RANK_BLOCK
#define SP_RANK_CNXLOCK (SP_RANK_RPCLOCK - 1)

#define MX_MUTEX_BLOCK_ON_VCPU   1

/*
 * The current implementation uses statically defined message queues,
 * connections, and buffers.  This needs to be made more dynamic and
 * more efficient with memory.
 */
typedef struct RPCMessage {
   List_Links links;
   RPC_Token  token;   
   int32      function;
   void       *buffer;
   uint32     bufferLength;
   World_ID   worldID;
} RPCMessage;

typedef RPCMessage RPCReply;

typedef struct RPCConnection {
   Bool               allocated;  // protected by rpcLock

   SP_SpinLock        cnxLock;    // protects everything else in this struct
   uint32             generation; 

   /*
    * The data in the fields above this comment is saved across different
    * users of this structure, but the remaining part is obliterated before
    * reuse.  The 'id' fields marks the start of the fields that will be
    * reset.
    */

   // don't put fields that should be reset before "id" field.
   RPC_Connection     id;
   Bool               pendingDestroy;

   uint32             useCount;
   char               name[RPC_CNX_NAME_LENGTH];
   Heap_ID            heap;           // used when freeing the buffers

   World_ID           associatedWorld;
   List_Links         associatedLinks;

   uint32             maxBufSize;
   List_Links         messageList;
   List_Links         replyList;
   List_Links         freeList;
   uint32             nQueuedMessages;   //includes both messages and 
                                         //replies on this connection

   uint32             nextToken;
   Bool               notifyCOS;
   Bool               isSemaphore;
   VMKPollWaitersList pollWaiters;
} RPCConnection;


#define RPC_CNX_TABLE_SIZE	RPC_MAX_CONNECTIONS

RPCConnection connections[RPC_CNX_TABLE_SIZE];

static SP_SpinLock rpcLock;

static RPC_CnxList pendingCnx;
static Proc_Entry procRPCStats;

static void RPCRemoveAndUnlockCnx(RPCConnection *cnx);
static VMK_ReturnStatus RPCNewConnection(const char *name, uint32 *retIndex);
static VMK_ReturnStatus RPCInitBuffers(RPCConnection *cnx, int numBuffers,
                                       uint32 bufferLength, Heap_ID heap);
static RPCMessage *RPCAllocMessage(RPCConnection *cnx, uint32 bufferLength);
static void RPCFreeMessage(RPCConnection *cnx, RPCMessage *msg);
static VMK_ReturnStatus RPCUnregister(RPC_Connection cnxID, World_ID worldID);

static Bool initialized = FALSE;

/*
 * RPC statistics
 */
#define RPC_STATS_DISABLE               0
#define RPC_STATS_ENABLE                1

typedef struct RPCStatsData {
   Timer_AbsCycles startTime;    /* time when stats enabled  */
   Timer_AbsCycles endTime;      /* time when stats disabled */
   Timer_RelCycles activeTime;   /* elapsed time for stats   */
   Bool      state;       /* stats collection state  */
} RPCStatsData;

static RPCStatsData rpcStatsData;

/*
 *----------------------------------------------------------------------
 *
 * RPCStatUpdate --
 *
 *      Utility function to increment the user rpc call stats.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      Increments user RPC stat count and updates call times.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
RPCStatUpdate(World_Handle *world, uint32 userCall, Timer_RelCycles curTime)
{
   int index = userCall - 300;  /* aka USERCALL_NONE */

   /* 
    * Catch people who run vmms that are too old / new (and
    * thus have more usercalls than we can track)
    */
   if (vmx86_debug && (index < 0 || index >= RPC_NUM_USER_RPC_CALLS)) {
      World_Panic(world, "Invalid usercall %d (or RPC_NUM_USER_RPC_CALLS needs to be bumped)\n", 
                  index);
      return;
   }

   ASSERT(World_IsVMMWorld(world));
   if ((index >= 0) && (index < RPC_NUM_USER_RPC_CALLS)) {
      World_VmmInfo *vmmInfo = World_VMM(world);
      vmmInfo->userRPCStats->callCnt[index]++;
      if (curTime > vmmInfo->userRPCStats->maxTime[index]) {
         vmmInfo->userRPCStats->maxTime[index] = curTime;
      }
      vmmInfo->userRPCStats->totTime[index] += curTime;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * RPCStatWorldReset --
 *
 *      Reset the user rpc stats for a world.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
void
RPCStatWorldReset(World_Handle *world)
{
   int i;

   for (i = 0; i < RPC_NUM_USER_RPC_CALLS; i++) {
      World_VMM(world)->userRPCStats->callCnt[i] = 0;
      World_VMM(world)->userRPCStats->maxTime[i] = 0;
      World_VMM(world)->userRPCStats->totTime[i] = 0;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * RPCStatCallPerSec --
 *              
 *      Convert number of calls over a time interval into calls/sec.
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
RPCStatCallPerSec(uint32 count,
                  uint32 secElap, uint32 usecElap,
                  uint64 *sec, uint32 *msec)
{
   uint64 scale = 1000000;
   uint64 scaledCount = (uint64)count * scale;
   uint64 scaledDelta = (uint64)secElap * scale + usecElap;
   uint64 remainder, seconds, uSeconds;

   scaledDelta = (uint64)secElap * scale + usecElap;
   seconds     = scaledCount / scaledDelta;
   remainder   = (scaledCount - seconds * scaledDelta) * scale;
   uSeconds    = remainder / scaledDelta + 500;
   if (uSeconds > 1000000) {
      seconds++;
      uSeconds = 0;
   }

   *sec  = (uint32)seconds;
   *msec = (uint32)uSeconds / 1000;
}


/*
 *----------------------------------------------------------------------
 *
 * RPCStatPrintTitle --
 *              
 *      Print stats header.
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
RPCStatPrintTitle(
            char *buf,
            int  *len,
            uint32 memberCount,
            World_Handle *memberWorlds[])
{ 
   uint32 i;

   // if this is an SMP world, output totals
   // and span each world output with it's name.
   if (memberCount > 1) {
      Proc_Printf(buf, len, "%*s Total %*s", 43, " ", 16, " ");
      for (i = 0; i < memberCount; i++) {
         int n = strlen(memberWorlds[i]->worldName);
         int r = 20 - n / 2;
         int l = 38 - n - r;
         Proc_Printf(buf, len, "%*s %s %*s", r, " ",
                     memberWorlds[i]->worldName, l, " ");
      }
      Proc_Printf(buf, len, "\n");
   }
   Proc_Printf(buf, len, "%5s %8s %10s %10s %8s",
               "User RPC #", "Count",
               "Max (sec)", "Tot (sec)",
               "Call/sec");
   if (memberCount > 1) {
      for (i = 0; i < memberCount; i++) {
         Proc_Printf(buf, len, " %8s %10s %10s %8s",
                     "Count", "Max (sec)", "Tot (sec)",
                     "Call/sec");
      }
   }
   Proc_Printf(buf, len, "\n");
}


/*
 *----------------------------------------------------------------------
 *
 * RPCStatPrintData --
 *              
 *      Print RPC call stats data.
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
RPCStatPrintData(
            char *buf,
            int  *len,
            uint32 memberCount,
            World_Handle *memberWorlds[])
{ 
   uint32 i, j;
   Timer_RelCycles activeInterval;
   uint64 secElp;
   uint32 uSecElp;
   RPC_UserRPCStats *userRPCStats;

   // compute sample interval
   activeInterval = rpcStatsData.activeTime;
   if (rpcStatsData.state == RPC_STATS_ENABLE) {
      rpcStatsData.endTime = Timer_GetCycles();
      activeInterval += rpcStatsData.endTime - rpcStatsData.startTime;
   }
   Timer_TCToSec(activeInterval, &secElp, &uSecElp);

   // for each user call, dump stats
   // SMP worlds will also generate totals
   for (i = 0; i < RPC_NUM_USER_RPC_CALLS; i++) {
      uint64 secMax, secTot, secCall;
      uint32 uSecMax, uSecTot, mSecCall;
      uint32 sumCallCnt = 0;
      Timer_RelCycles sumTotTime = 0;
      Timer_RelCycles maxTime = 0;
      for (j = 0; j < memberCount; j++) {
         userRPCStats = World_VMM(memberWorlds[j])->userRPCStats;
         sumTotTime += userRPCStats->totTime[i];
         if (userRPCStats->maxTime[i] > maxTime) {
            maxTime = userRPCStats->maxTime[i];
         }
         sumCallCnt += userRPCStats->callCnt[i];
      }
      if (sumCallCnt == 0) {
         continue;
      }
      Timer_TCToSec(maxTime, &secMax, &uSecMax);
      Timer_TCToSec(sumTotTime, &secTot, &uSecTot);
      RPCStatCallPerSec(sumCallCnt, secElp, uSecElp,
                        &secCall, &mSecCall);
      Proc_Printf(buf, len, "%5d %8u %3Lu.%06u %3Lu.%06u %4Lu.%03u",
                  i,
                  sumCallCnt,
                  secMax, uSecMax, secTot, uSecTot,
                  secCall, mSecCall);
      for (j = 0; memberCount > 1 && j < memberCount; j++) {
         userRPCStats = World_VMM(memberWorlds[j])->userRPCStats;
         Timer_TCToSec(userRPCStats->maxTime[i], &secMax, &uSecMax);
         Timer_TCToSec(userRPCStats->totTime[i], &secTot, &uSecTot);
         RPCStatCallPerSec(userRPCStats->callCnt[i], secElp, uSecElp,
                           &secCall, &mSecCall);
         Proc_Printf(buf, len, " %8u %3Lu.%06u %3Lu.%06u %4Lu.%03u",
                     userRPCStats->callCnt[i],
                     secMax, uSecMax, secTot, uSecTot,
                     secCall, mSecCall);
      }
      Proc_Printf(buf, len, "\n");
   }
}


/*
 *----------------------------------------------------------------------
 *
 * RPCStatProcRead --
 *              
 *    Callback for read operation on "userRPC" procfs node.
 *
 * Results:
 *      Returns VMK_OK.
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
static int
RPCStatProcRead(Proc_Entry *entry,
            char *buf,
            int  *len)
{
   uint32 i;
   World_Handle *world;
   World_ID worldID = (World_ID)entry->private;
   World_Handle *memberWorlds[MAX_VCPUS];

   *len = 0;
   memset(memberWorlds, 0, sizeof(memberWorlds));

   world = World_Find(worldID);
   ASSERT(world);
   if (!world) {
      return(VMK_OK);
   }
   ASSERT(World_VMMGroup(world)->memberCount <= MAX_VCPUS);
   for (i = 0; i < World_VMMGroup(world)->memberCount; i++) {
      memberWorlds[i] = World_Find(World_VMMGroup(world)->members[i]);
      if (!memberWorlds[i]) {
         /* 
          * If a world is in the process of being cleaned up, we'll hit 
          * this case.
          */
         goto done;
      }
   }

   // dump call stats
   if ((rpcStatsData.activeTime == 0) &&
       (rpcStatsData.state == RPC_STATS_DISABLE)) {
      Proc_Printf(buf, len, "no rpc stats available\n");
   } else {
      RPCStatPrintTitle(buf, len, World_VMMGroup(world)->memberCount, 
                        memberWorlds);
      RPCStatPrintData(buf, len, World_VMMGroup(world)->memberCount, 
                       memberWorlds);
   }

done:
   for (i = 0; i < World_VMMGroup(world)->memberCount; i++) {
      if (memberWorlds[i]) {
         World_Release(memberWorlds[i]);
      }
   }

   World_Release(world);
   return(VMK_OK);
}


/*
 *----------------------------------------------------------------------
 *
 * RPCStatProcWrite --
 *              
 *      Callback for write operation on "userRPC" procfs node.
 *
 * Results:
 *      Returns VMK_OK.
 *
 * Side effects:
 *      Can enable/disable RPC stats collection.
 *
 *----------------------------------------------------------------------
 */
static int
RPCStatProcWrite(Proc_Entry *entry,
            char *buf,
            int  *len)
{
   uint32 i;
   World_Handle *world;
   World_ID worldID = (World_ID)entry->private;

   world = World_Find(worldID);
   ASSERT(world);
   if (!world) {
      return(VMK_OK);
   }

   if (strncmp(buf, "disable", 7) == 0) {
      RPC_StatsDisable();
   } else if (strncmp(buf, "enable", 6) == 0) {
      RPC_StatsEnable();
   } else if (strncmp(buf, "reset", 5) == 0) {
      ASSERT(World_VMMGroup(world)->memberCount <= MAX_VCPUS);
      for (i = 0; i < World_VMMGroup(world)->memberCount; i++) {
         World_Handle *memberWorld;
         memberWorld = World_Find(World_VMMGroup(world)->members[i]);
         if (memberWorld) {
            RPCStatWorldReset(memberWorld);
            World_Release(memberWorld);
         }
      }
      rpcStatsData.startTime = Timer_GetCycles();
      rpcStatsData.endTime = 0;
      rpcStatsData.activeTime = 0;
   }

   World_Release(world);
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * RPCStatsInit --
 *
 *      Initialize the user rpc stats.
 *
 * Results:
 *      none.
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
RPCStatsInit(World_Handle *world)
{
   RPC_UserRPCStats *stats;

   ASSERT(World_IsVMMWorld(world));
   stats = (RPC_UserRPCStats*) World_Alloc(world, sizeof *stats);
   if (stats == NULL) {
      return VMK_NO_MEMORY;
   }
   World_VMM(world)->userRPCStats = stats;
   RPCStatWorldReset(world);

   // create the userRPC proc node
   Proc_InitEntry(&stats->procUserRPC);
   stats->procUserRPC.parent = &world->procWorldDir;
   stats->procUserRPC.read = RPCStatProcRead;
   stats->procUserRPC.write = RPCStatProcWrite;
   stats->procUserRPC.private = (void *)world->worldID;
   Proc_Register(&stats->procUserRPC, "userRPC", FALSE);

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * RPCStatsCleanup --
 *
 *      Cleanup the userRPC stats 
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
RPCStatsCleanup(World_Handle *world)
{
   RPC_UserRPCStats *stats;

   ASSERT(World_IsVMMWorld(world));
   stats = World_VMM(world)->userRPCStats;

   Proc_Remove(&stats->procUserRPC);
   World_Free(world, stats);
   World_VMM(world)->userRPCStats = NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * RPCProcReadHandler --
 *
 *      Prints out some minimal data about the memory consumption
 *      of this module.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int 
RPCProcReadHandler(Proc_Entry *entry,
                   char        *page,
                   int         *lenp)
{
   int i;

   *lenp = 0;

   SP_Lock(&rpcLock);
   for (i = 0; i < RPC_CNX_TABLE_SIZE; i++) {
      if (connections[i].allocated) {
         Proc_Printf(page, lenp, "%d) cnx 0x%x name '%s', useCount %d, "
                                 "pendingDestroy %d, nMsgs %d\n",
                                 i,
                                 connections[i].id,
                                 connections[i].name,
                                 connections[i].useCount,
                                 connections[i].pendingDestroy,
                                 connections[i].nQueuedMessages);
      }
   }
   SP_Unlock(&rpcLock);

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * RPC_Init --
 *
 *      Initialize the RPC module.
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
RPC_Init(VMnix_SharedData *sharedData)
{
   int index;

   ASSERT(RPC_CNX_TABLE_SIZE == Util_RoundupToPowerOfTwo(RPC_CNX_TABLE_SIZE));
   SP_InitLock("rpcLck", &rpcLock, SP_RANK_RPCLOCK);

   pendingCnx.maxIndex = 0;

   SHARED_DATA_ADD(sharedData->cnxList, RPC_CnxList *, &pendingCnx);

   Proc_InitEntry(&procRPCStats);
   procRPCStats.read = RPCProcReadHandler;
   Proc_Register(&procRPCStats, "rpcStats", FALSE);

   rpcStatsData.state = RPC_STATS_ENABLE;
   rpcStatsData.startTime = Timer_GetCycles();
   rpcStatsData.activeTime = 0;

   for (index = 0; index < RPC_CNX_TABLE_SIZE; index++) {
      RPCConnection *cnx = &connections[index];
      SP_InitLock("cnxLock", &cnx->cnxLock, SP_RANK_CNXLOCK);
      cnx->allocated = FALSE;
   }

   initialized = TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * RPC_WorldInit --
 *
 *      Initialize cnxList for all worlds.  For VMM worlds, initialize user
 *      RPC counts and add the "userRPC" proc node .
 *
 * Results:
 *      VMK_ReturnStatus
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
RPC_WorldInit(World_Handle *world, UNUSED_PARAM(World_InitArgs *args))
{
   List_Init(&world->cnxList);

   if (World_IsVMMWorld(world)) {
      return RPCStatsInit(world);
   } else {
      return VMK_OK;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * RPC_WorldCleanup --
 *
 *      Close all of this world's connections.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	The world's connections are closed.
 *
 *----------------------------------------------------------------------
 */
void
RPC_WorldCleanup(World_Handle *world)
{
   LOG(0, "unregistering connections");
   while (!List_IsEmpty(&world->cnxList)) {
      List_Links *element = List_First(&world->cnxList);
      RPCConnection *cnx = (RPCConnection *) (((VA)element) - 
                                              offsetof(RPCConnection, associatedLinks));

      // cnx->id is read without holding cnxLock, so it may not remain valid
      // till unregister, but that's OK because unregister handles it.
      RPCUnregister(cnx->id, world->worldID);
   }

   if (World_IsVMMWorld(world)) {
      RPCStatsCleanup(world);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * RPCFindAndLockCnx
 *
 *      Find the connection that matches this id.  If the connection is
 *      still alive and not pending destruction, lock it and increment
 *      useCount.
 *
 * Results:
 *      A pointer to the connection.
 *      VMK_ReturnStatus
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
RPCFindAndLockCnx(RPC_Connection cnxID, RPCConnection **retCnx)
{
   uint32 index = cnxID % RPC_CNX_TABLE_SIZE;
   RPCConnection *cnx;

   *retCnx = NULL;

   // it's OK to read this unlocked because we'll reverify afterwards
   cnx = &connections[index];
   if (cnx->id != cnxID) {
      LOG(0, "Invalid connection ID: 0x%x", cnxID);
      return VMK_NOT_FOUND;
   }
   SP_Lock(&cnx->cnxLock);
   // previous cnx->id check was racy, so check again
   if (cnx->id != cnxID) {
      LOG(0, "Invalid connection ID: 0x%x", cnxID);
      SP_Unlock(&cnx->cnxLock);
      return VMK_NOT_FOUND;
   }
   if (cnx->pendingDestroy) {
      LOG(1, "Not connected (cnxID 0x%x)", cnxID);
      SP_Unlock(&cnx->cnxLock);
      return VMK_IS_DISCONNECTED;
   }

   ASSERT(cnx->useCount > 0);
   cnx->useCount++;
   LOG(3, "cnx 0x%x count=%d", cnxID, cnx->useCount);

   *retCnx = cnx;
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * RPCReleaseAndUnlockCnx
 *
 *      Release a connection that was previously obtained from
 *      FindAndLockCnx.  The useCount is decremented and lock released.
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
RPCReleaseAndUnlockCnx(RPCConnection *cnx)
{
   ASSERT(SP_IsLocked(&cnx->cnxLock));
   ASSERT(cnx->useCount > 0);
   cnx->useCount--;
   LOG(3, "cnx 0x%x count=%d", cnx->id, cnx->useCount);

   if (cnx->useCount == 0) {
      ASSERT(cnx->pendingDestroy);
      RPCRemoveAndUnlockCnx(cnx);
   } else {
      SP_Unlock(&cnx->cnxLock);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * RPCRelockAndVerify
 *
 *      Lock a connection was explicity unlocked after a FindAndLockCnx.
 *      It's assumed that the useCount increment in FindAndLockCnx is still
 *      prevent cnx from being destroyed.  Also, check if the connection is
 *      being unregistered, and if so return FALSE.
 *
 * Results:
 *      TRUE if connection good, FALSE otherwise
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static Bool
RPCRelockAndVerify(RPCConnection *cnx)
{
   SP_Lock(&cnx->cnxLock);
   ASSERT(cnx->allocated);
   if (cnx->pendingDestroy) {
      LOG(1, "Not connected (cnxID 0x%x)", cnx->id);
      return FALSE;
   }

   ASSERT(cnx->useCount > 0);
   return TRUE;
}

/*
 *----------------------------------------------------------------------
 *
 * RPCFindCnxByName --
 *
 *      Find a connection that matches the given name.  This should be
 *      changed to use a hash table.
 *
 * Results:
 *	A pointer to the named connection object or NULL if nothing is 
 *	found.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static RPCConnection *
RPCFindCnxByName(const char *name)
{
   int index;

   ASSERT(SP_IsLocked(&rpcLock));
   for (index = 0; index <= pendingCnx.maxIndex; index++) {
      RPCConnection *cnx = &connections[index];
      if (cnx->allocated && !cnx->pendingDestroy &&
          (strcmp(cnx->name, name) == 0)) {
	 return cnx;
      }
   }

   return NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * RPCResetCnx --
 *
 *      memset the cnx structure to catch use-after-free errors
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
RPCResetCnx(RPCConnection *cnx)
{
   // using 0xff so pointers are invalid and also to match RPC_CNX_INVALID
   ASSERT(RPC_CNX_INVALID == 0xffffffff);
   memset(((void*)cnx) + offsetof(RPCConnection, id),
          0xff,
          sizeof(*cnx) - offsetof(RPCConnection, id));

   // INVALID_WORLD_ID is not 0xffffffff, so resetting manually
   cnx->associatedWorld = INVALID_WORLD_ID;
}

/*
 *----------------------------------------------------------------------
 *
 * RPCNewConnection --
 *
 *      Verify there are no other connections with the given name, and
 *      allocate an index in the connections table for the new connection.
 *
 * Results:
 *      The index in the connections table
 *      VMK_ReturnStatus
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
RPCNewConnection(const char *name, uint32 *retIndex)
{
   VMK_ReturnStatus status = VMK_OK;
   RPCConnection *cnx;
   int index;

   SP_Lock(&rpcLock);
   if (RPCFindCnxByName(name) != NULL) {
      status = VMK_EXISTS;
      goto done;
   }
   for (index = 0; index < RPC_CNX_TABLE_SIZE; index++) {
      if (!connections[index].allocated) {
	 break;
      }
   }
   if (index == RPC_CNX_TABLE_SIZE) {
      status = VMK_NO_RESOURCES;
      goto done;
   }

   *retIndex = index;
   cnx = &connections[index];

   RPCResetCnx(cnx);

   cnx->allocated = TRUE;
   cnx->pendingDestroy = FALSE;

   strncpy(cnx->name, name, sizeof (cnx->name));

done:
   SP_Unlock(&rpcLock);
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * RPCAssociateWorld --
 *
 *      Associate this world with the given RPC connection so that the
 *      connection is automatically unregistered when this world dies.
 *
 * Results:
 *	VMK_ReturnStatus
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
RPCAssociateWorld(World_ID associatedWorld, RPCConnection *cnx)
{
   World_Handle *world = World_Find(associatedWorld);
   if (world == NULL) {
      return VMK_NOT_FOUND;
   }

   ASSERT(SP_IsLocked(&cnx->cnxLock));

   ASSERT(cnx->associatedWorld == INVALID_WORLD_ID);
   cnx->associatedWorld = associatedWorld;
   List_InitElement(&cnx->associatedLinks);

   // to prevent simultaneous associations to same world
   SP_Lock(&rpcLock);
   List_Insert(&cnx->associatedLinks, LIST_ATREAR(&world->cnxList));
   SP_Unlock(&rpcLock);

   World_Release(world);

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * RPCInitBuffers
 *
 *      Allocate the given number of buffers for this connection from the
 *      given heap.
 *
 * Results:
 *      VMK_NO_MEMORY on failure or VMK_OK
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
RPCInitBuffers(RPCConnection *cnx, int numBuffers, uint32 bufferLength, Heap_ID heap)
{
   int i;

   List_Init(&cnx->messageList);
   List_Init(&cnx->replyList);
   List_Init(&cnx->freeList);
   cnx->nQueuedMessages = 0;
   cnx->heap = heap;

   cnx->maxBufSize = bufferLength;
   for (i = 0; i < numBuffers; i++) {
      RPCMessage *msg = (RPCMessage*)Heap_Alloc(heap, bufferLength + sizeof(*msg));
      if (msg == NULL) {
         return VMK_NO_MEMORY;
      }
      msg->buffer = ((void*)msg) + sizeof (*msg);
      List_InitElement(&msg->links);
      List_Insert(&msg->links, LIST_ATREAR(&cnx->freeList));
   }

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * RPC_Register --
 *
 *      Register a connection under the given name.  Once this connection
 *	is registered a host thread can wait for messages and vmkernel
 *	worlds can send messages.
 *
 * Results:
 *	VMK_OK if connection is created, error code otherwise.
 *
 * Side effects:
 *      Connection queues are modified.  pendingCnx may be modified.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
RPC_Register(const char *name, Bool isSemaphore, Bool notifyCOS, 
             World_ID associatedWorld,
             int numBuffers, uint32 bufferLength,
             Heap_ID heap, RPC_Connection *resultCnxID)
{
   RPCConnection *cnx = NULL;
   VMK_ReturnStatus status = VMK_OK;
   uint32 index;

   LOG(1, "name=%s isSem=%d notifyCOS=%d nBuf=%d len=%d", name,
       isSemaphore, notifyCOS, numBuffers, bufferLength);

   if (strlen(name) >= RPC_CNX_NAME_LENGTH) {
      status = VMK_NAME_TOO_LONG;
      goto done;
   }

   status = RPCNewConnection(name, &index);
   if (status != VMK_OK) {
      goto done;
   }

   cnx = &connections[index];

   SP_Lock(&cnx->cnxLock);
   cnx->id = cnx->generation * RPC_CNX_TABLE_SIZE + index;
   cnx->generation++;
   if (cnx->id == RPC_CNX_INVALID) {
      cnx->id = cnx->generation * RPC_CNX_TABLE_SIZE + index;
      cnx->generation++;
   }

   cnx->useCount = 1;
   cnx->notifyCOS = notifyCOS;
   cnx->isSemaphore = isSemaphore;
   VMKPoll_InitList(&cnx->pollWaiters, &cnx->cnxLock);

   status = RPCInitBuffers(cnx, numBuffers, bufferLength, heap);
   if (status != VMK_OK) {
      goto done;
   }

   status = RPCAssociateWorld(associatedWorld, cnx);
   if (status != VMK_OK) {
      goto done;
   }

   if (index > pendingCnx.maxIndex) {
      pendingCnx.maxIndex = index;
   }

   *resultCnxID = cnx->id;

done:
   LOG(1, "name=%s, id=0x%x status = 0x%x", name, 
       (cnx != NULL) ? cnx->id : RPC_CNX_INVALID, status);

   if (cnx != NULL) {
      if (status != VMK_OK) {
         cnx->pendingDestroy = TRUE;
         RPCRemoveAndUnlockCnx(cnx);
      } else {
         SP_Unlock(&cnx->cnxLock);
      }
   }

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * RPCUnregister --
 *
 *      Destroy the given connection.  If world ID is specified, unregister
 *      only if associatedWorld matches given world ID.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
RPCUnregister(RPC_Connection cnxID, World_ID worldID)
{
   RPCConnection *cnx;
   VMK_ReturnStatus status;

   ASSERT(World_IsSafeToBlock());

   status = RPCFindAndLockCnx(cnxID, &cnx);
   if (status != VMK_OK) {
      return status;
   }

   if (worldID != INVALID_WORLD_ID) {
      if (cnx->associatedWorld != worldID) {
         RPCReleaseAndUnlockCnx(cnx);
         return VMK_NOT_FOUND;
      }
   }

   LOG(1, "name=%s, id=0x%x", cnx->name, cnx->id);

   ASSERT(cnx->useCount > 1);
   cnx->useCount --;

   cnx->pendingDestroy = TRUE;

   // wait until noone is using this cnx except for myself
   while (cnx->useCount > 1) {
      CpuSched_Wakeup((uint32)cnx);
      SP_Unlock(&cnx->cnxLock);
      CpuSched_Sleep(10);
      SP_Lock(&cnx->cnxLock);
   }
   ASSERT(cnx->useCount == 1);

   // to prevent simultaneous dis-associations to same world
   SP_Lock(&rpcLock);
   List_Remove(&cnx->associatedLinks);
   SP_Unlock(&rpcLock);

   RPCReleaseAndUnlockCnx(cnx);
   LOG(1, "done id=0x%x", cnx->id);

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * RPC_Unregister --
 *
 *      External interface to destroy an RPC connection
 *
 * Results:
 *	VMK_ReturnStatus
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
RPC_Unregister(RPC_Connection cnxID)
{
   return RPCUnregister(cnxID, INVALID_WORLD_ID);
}

/*
 *----------------------------------------------------------------------
 *
 * RPC_Connect --
 *
 *      Connect to a named connection.  
 *
 * Results:
 *	VMK_OK if connection is found and attached, VMK_NOT_FOUND or
 *	VMK_DISCONNECTED in error cases.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
RPC_Connect(const char *name, RPC_Connection *cnxID)
{
   RPCConnection *cnx;
   VMK_ReturnStatus status = VMK_NOT_FOUND;

   *cnxID = RPC_CNX_INVALID;

   LOG(1, "name=%s", name);

   SP_Lock(&rpcLock);

   cnx = RPCFindCnxByName(name);
   if (cnx != NULL) {
      if (!cnx->pendingDestroy) {
	 *cnxID = cnx->id;
	 status = VMK_OK;
      } else {
	 status = VMK_IS_DISCONNECTED;
      }
   }
   SP_Unlock(&rpcLock);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * RPC_Disconnect --
 *
 *      Disconnect from the given connection.  Since RPC_Connect doesn't
 *      do much, nothing to undo here.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
RPC_Disconnect(RPC_Connection cnxID)
{
   //nothing to do here
}

/*
 *----------------------------------------------------------------------
 *
 * RPCWaitCnx --
 *
 *      Wait for activity on the given connection.
 *      After coming back from the wait, relock and verify the connection.
 *
 * Results:
 *	VMK_IS_DISCONNECTED if connection is getting unregistered.
 *      Otherwise, return status from CpuSched_Wait
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
RPCWaitCnx(RPCConnection *cnx, uint32 mask, World_ID switchToWorldID)
{
   VMK_ReturnStatus status;

   ASSERT(SP_IsLocked(&cnx->cnxLock));
   ASSERT(cnx->allocated);
   ASSERT(!cnx->pendingDestroy);
   
   status = CpuSched_WaitDirectedYield((uint32)cnx,
                                       cnx->isSemaphore ? CPUSCHED_WAIT_SEMAPHORE : CPUSCHED_WAIT_RPC,
                                       mask,
                                       &cnx->cnxLock,
                                       switchToWorldID);
   if (!RPCRelockAndVerify(cnx)) {
      if (status == VMK_OK) {
         status = VMK_IS_DISCONNECTED;
      }
   }
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * RPCStressWakeup --
 *
 *      Wakeup all registered connections if the stress counter triggers
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
RPCStressWakeup(void)
{
   int index;
   if (VMK_STRESS_DEBUG_COUNTER(RPC_WAKEUP)) {
      SP_Lock(&rpcLock);
      for (index = 0; index <= pendingCnx.maxIndex; index++) {
         RPCConnection *cnx = &connections[index];
         if (cnx->allocated) {
            CpuSched_Wakeup((uint32)cnx);
         }
      }
      SP_Unlock(&rpcLock);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * RPCNextToken --
 *
 *      Get the next magic token number.  These are for associating
 *      RPC replies with a particular RPC send.  We reserve -10 .. 0
 *      for special tokens (e.g., the UserWorld proxy uses -2 for
 *      errors discovered during a reply).  -1 is globally reserved
 *      for the RPC_TOKEN_INVALID, and it implies that no response is
 *      expected.
 *
 * Results:
 *	Token number.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE_SINGLE_CALLER RPC_Token
RPCNextToken(RPCConnection *cnx)
{
   ASSERT(SP_IsLocked(&cnx->cnxLock));

   // Reserve -10 .. 0 for special flag tokens
   if (cnx->nextToken == -10) {
      cnx->nextToken = 0;
   }

   return ++(cnx->nextToken);
}


/*
 *----------------------------------------------------------------------
 *
 * RPC_Send --
 *
 *      Send a message on the given connection.  If (flags & RPC_REPLY_EXPECTED)
 *	then *token contains a token that can be used to get the reply.
 *
 * Results:
 *	VMK_OK if no problems, otherwise one of the following error 
 *	codes is returned:
 *
 *	VMK_IS_DISCONNECTED:	The host has disconnected.
 *	VMK_NO_RESOURCES:	There are no resources available to store 
 *	                        this message.
 *	VMK_LIMIT_EXCEEDED:     Max messages queued limit hit for this 
 *	                        connection.
 *
 * Side effects:
 *      The message queue for the given connection has a new message
 *	added to it.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
RPC_Send(RPC_Connection cnxID, 
	 int32 function,	// IN: Which RPC this is. 
	 uint32 flags,		// IN: RPC_REPLY_EXPECTED
         const char *argBuffer,	// IN: Pointer to RPC arguments
	 uint32 argLength, 	// IN: Length of RPC arguments
	 Util_BufferType bufType, // IN: type of buffer
	 RPC_Token *token)	// OUT: Token used to get reply
{
   VMK_ReturnStatus status = VMK_OK;
   RPCMessage *msg = NULL;
   RPCConnection *cnx = NULL;
   VMKPollWaitersList pollWaiters;
   Bool waitersPresent = FALSE;
   Bool notifyCOS = FALSE;

   if (!initialized) {
      return VMK_NOT_INITIALIZED;
   }

   /*
    * Allocate the rpc message.
    */
   if (argLength > RPC_MAX_MSG_LENGTH) {
      Warning("argLength=%d > %d", argLength, RPC_MAX_MSG_LENGTH);
      argLength = RPC_MAX_MSG_LENGTH;
   }

   status = RPCFindAndLockCnx(cnxID, &cnx);
   if (status != VMK_OK) {
      return status;
   }

   msg = RPCAllocMessage(cnx, argLength);
   if (msg == NULL) {
      status = VMK_LIMIT_EXCEEDED;
      goto error;
   }

   /*
    * Copy contents of this message in from caller.  Must copy data in
    * without rpc lock (in case it faults) until PR 53504 is fixed.
    */
   SP_Unlock(&cnx->cnxLock);
   status = Util_CopyIn(msg->buffer, argBuffer, argLength, bufType);
   if (!RPCRelockAndVerify(cnx)) {
      status = VMK_IS_DISCONNECTED;
      goto error;
   }

   if (status != VMK_OK) {
      LOG(0, "Faulted on msg=%p/%s len=%u cnxID=0x%x status=%s",
          argBuffer,
          (bufType == UTIL_VMKERNEL_BUFFER ? "vmk buf" :
           (bufType == UTIL_USERWORLD_BUFFER ? "user buf" :
            (bufType == UTIL_HOST_BUFFER ? "host buf" :
             "UNKNOWN"))), argLength, cnxID,
          VMK_ReturnStatusToString(status));
      goto error;
   }

   /*
    * Initialize message header, pick a token for this message.
    */
   if (flags & RPC_REPLY_EXPECTED) {
      msg->token = RPCNextToken(cnx);
      ASSERT(msg->token != RPC_TOKEN_INVALID);
   } else if (flags & RPC_FORCE_TOKEN) {
      msg->token = *token;
   } else {
      msg->token = RPC_TOKEN_INVALID;
   }
   msg->function = function;
   msg->worldID = MY_RUNNING_WORLD->worldID;
   *token = msg->token;

   LOG(2, "sending to %s:0x%x token %d", cnx->name, cnxID, *token);

   List_Insert(&msg->links, LIST_ATREAR(&cnx->messageList));
   cnx->nQueuedMessages++;

   RPCSetMask(&pendingCnx, cnx->id);

   VMKPoll_InitList(&pollWaiters, NULL);
   waitersPresent = VMKPoll_MoveWaiters(&cnx->pollWaiters, &pollWaiters);

   /*
    * Wakeup any vmkernel worlds that are waiting.
    */
   CpuSched_Wakeup((uint32)cnx);

   notifyCOS = cnx->notifyCOS;

   RPCReleaseAndUnlockCnx(cnx);
   cnx = NULL;

   RPCStressWakeup();

   /*
    * There are three ways to wakeup worlds.  This is too messy and should
    * be unified somehow ...
    */

   /*
    * Wake up the host in case it is waiting.
    */
   if (notifyCOS) {
      Host_InterruptVMnix(VMNIX_RPC_EVENT);
   }

   /*
    * Wakeup user processes in case they are polling.
    */
   if (waitersPresent) {
      VMKPoll_WakeupAndRemoveWaiters(&pollWaiters);
   }

   return VMK_OK;

error:

   if (msg != NULL) {
      RPCFreeMessage(cnx, msg);
   }

   if (cnx != NULL) {
      RPCReleaseAndUnlockCnx(cnx);
   }


   return status;
}

static void
RPCTimeoutCallback(void *data, UNUSED_PARAM(Timer_AbsCycles timestamp))
{
   RPCConnection *cnx = (RPCConnection*) data;
   LOG(3, "timeout cnx 0x%x", cnx->id);
   CpuSched_Wakeup((uint32)cnx);
}


/*
 *----------------------------------------------------------------------
 *
 * RPCGetMsg --
 *
 *      Return the next available message on this connection. *msgInfo is
 *      filled in with information about the message.  The "timeout"
 *      parameter, if non-zero, specifies how long in milliseconds, this
 *      function will wait for a message to arrive. The interruptible
 *      parameter, if set, specifies that the world can be interrupted
 *      while "waiting".
 *      switchToWorldID is a hint to the scheduler to tell it to switch to
 *      the given world in case this call blocks.
 *
 * Results:
 *	VMK_OK if no problems, otherwise one of the following error 
 *	codes is returned:
 *
 *	VMK_IS_DISCONNECTED:	The host has disconnected.
 *	VMK_WOULD_BLOCK:	There are no messages available and 
 *				(flags & RPC_CAN_BLOCK) is not set.
 *      VMK_TIMEOUT:            timeout ms passed without a message
 *                              arriving.
 *      VMK_WAIT_INTERRUPTED    The wait was interrupted.
 *      CpuSchedWait return status
 *
 * Side effects:
 *      The message queue for the given connection may have a message
 *	removed from it.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
RPCGetMsg(RPC_Connection cnxID, 
          uint32 flags, 
          RPC_MsgInfo *msgInfo, 
          uint32 timeout,
          Bool interruptible,
          Util_BufferType bufType,
          World_ID switchToWorldID)
{
   VMK_ReturnStatus status = VMK_OK;
   RPCConnection *cnx = NULL;
   Bool interrupted = FALSE;
   Timer_Handle th = TIMER_HANDLE_NONE;
   RPCMessage *msg = NULL;
   Timer_AbsCycles startTime = Timer_GetCycles();
   Timer_AbsCycles endTime = 0;
   RPC_MsgInfo tmpMsg; 
   void *inMsgBuf;
   uint32 inMsgLen;

   if (timeout) {
      endTime = startTime + Timer_MSToTC(timeout);
   }

   status = Util_CopyIn(&tmpMsg, msgInfo, sizeof(*msgInfo), bufType);
   if (status != VMK_OK) {
      return status;
   }
   inMsgLen = tmpMsg.dataLength;
   inMsgBuf = tmpMsg.data;

   if (inMsgBuf == NULL) {
      return VMK_BAD_PARAM;
   }

   status = RPCFindAndLockCnx(cnxID, &cnx);
   if (status != VMK_OK) {
      return status;
   }

   while (1) {

      if (List_IsEmpty(&cnx->messageList)) {
         uint32 actionWakeupMask;

	 if (!(flags & RPC_CAN_BLOCK)) {
	    status = VMK_WOULD_BLOCK;
	    goto done;
	 }

         if (interruptible && interrupted) {
            status = VMK_WAIT_INTERRUPTED;
            goto done;
         }

         if (timeout) {
            if (Timer_GetCycles() > endTime) {
               status = VMK_TIMEOUT;
               goto done;
            }
            th = Timer_Add(myPRDA.pcpuNum, RPCTimeoutCallback, timeout,
                           TIMER_ONE_SHOT, cnx);
         }

         // wait action-based wakeups?
         if (cnx->isSemaphore && interruptible) {
            actionWakeupMask = World_VMM(MY_RUNNING_WORLD)->semaActionMask;
         } else {
            actionWakeupMask = 0;
         }

         Trace_EventLocal(TRACE_RPC_GET, (uint32)cnx, switchToWorldID);

         LOG(2, "Waiting for message on cnx %s:0x%x", cnx->name, cnxID);
         status = RPCWaitCnx(cnx, actionWakeupMask, switchToWorldID);

         Trace_EventLocal(TRACE_RPC_DONE, (uint32)cnx, switchToWorldID);
         interrupted = TRUE;

         if (timeout) {
            Timer_Remove(th);
         }

         if (status != VMK_OK) {
            goto done;
         }

	 continue;
      }

      break;
   }

   msg = (RPCMessage *)List_First(&cnx->messageList);

   if (msg->bufferLength > inMsgLen) {
      LOG(1, "msg->bufferLength (%u) > inMsgLen (%u)", msg->bufferLength, inMsgLen);
      // set msg to NULL so it's not freed by error handling code.
      msg = NULL;
      status = VMK_NO_RESOURCES;
      goto done;
   }

   List_Remove(&msg->links);
   ASSERT(cnx->nQueuedMessages > 0);
   cnx->nQueuedMessages--;
   if (List_IsEmpty(&cnx->messageList)) {
      RPCClearMask(&pendingCnx, cnx->id);
   }

   tmpMsg.token = msg->token;   
   tmpMsg.function = msg->function;
   tmpMsg.data = msg->buffer;
   tmpMsg.dataLength = msg->bufferLength;
   tmpMsg.worldID = msg->worldID;

   // must copy data without rpc lock (in case it faults) until PR 53504 is fixed
   SP_Unlock(&cnx->cnxLock);

   status = Util_CopyOut(inMsgBuf, tmpMsg.data, tmpMsg.dataLength, bufType);
   if (!RPCRelockAndVerify(cnx)) {
      status = VMK_IS_DISCONNECTED;
   }
   if (status != VMK_OK) {
      goto done;
   }
   tmpMsg.data = inMsgBuf;

   // must copy data without rpc lock (in case it faults)
   SP_Unlock(&cnx->cnxLock);

   status = Util_CopyOut(msgInfo, &tmpMsg, sizeof(RPC_MsgInfo), bufType);
   if (!RPCRelockAndVerify(cnx)) {
      status = VMK_IS_DISCONNECTED;
   }

done:

   // free the message
   if (msg != NULL) {
      RPCFreeMessage(cnx, msg);
   }

   RPCReleaseAndUnlockCnx(cnx);

   RPCStressWakeup();

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * RPC_GetMsg --
 *
 *      Wrapper to do a "non-interruptible" RPCGetMsg 
 *
 * Results:
 *      same results as RPCGetMsg. 
 *     
 * Side effects:
 *      none.
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
RPC_GetMsg(RPC_Connection cnxID,
           uint32 flags,
           RPC_MsgInfo *msgInfo,
           uint32 timeout,
           Util_BufferType bufType,
           World_ID switchToWorldID)
{
   return RPCGetMsg(cnxID, flags, msgInfo, timeout, FALSE, bufType,
                    switchToWorldID);
}

/*
 *----------------------------------------------------------------------
 *
 * RPC_GetMsgInterruptible --
 *
 *      Wrapper to do an "interruptible" RPCGetMsg.
 *
 * Results:
 *      same results as RPCGetMsg. 
 *     
 * Side effects:
 *      none.
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
RPC_GetMsgInterruptible(RPC_Connection cnxID, 
                        uint32 flags, 
                        RPC_MsgInfo *msgInfo, 
                        uint32 timeout,
                        Util_BufferType bufType,
                        World_ID switchToWorldID)
{
   return RPCGetMsg(cnxID, flags, msgInfo, timeout, TRUE, bufType,
                    switchToWorldID);
}


/*
 *----------------------------------------------------------------------
 *
 * RPC_PostReply --
 *
 *      Enqueue a reply for the message identified by token on the given
 *	connections reply queue.
 *
 * Results:
 *	VMK_OK if no problems, otherwise one of the following error 
 *	codes is returned:
 *
 *	VMK_IS_DISCONNECTED:	The host has disconnected.
 *	VMK_NO_RESOURCES:	There are no resources available to store this reply.
 *	VMK_LIMIT_EXCEEDED:     Max messages queued limit hit for this 
 *	                        connection.
 *
 * Side effects:
 *      The reply queue for the given connection has a new message
 *	added to it.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
RPC_PostReply(RPC_Connection cnxID, 	// IN: The connection that gets the reply.
              RPC_Token token, 		// IN: Token identifying the message with
					//     which the reply is associated.
	      const char *buffer,	// IN: Data for the reply.
	      uint32 bufferLength,	// IN: Length of reply data.
              Util_BufferType bufType)  // IN: buffer type
{
   VMK_ReturnStatus status = VMK_OK;
   RPCConnection *cnx = NULL;
   RPCReply *reply = NULL;

   ASSERT(bufferLength <= RPC_MAX_MSG_LENGTH);   
   if (bufferLength > RPC_MAX_MSG_LENGTH) {
      Warning("bufferLength=%d > %d", bufferLength, RPC_MAX_MSG_LENGTH);
      bufferLength = RPC_MAX_MSG_LENGTH;
   }

   status = RPCFindAndLockCnx(cnxID, &cnx);
   if (status != VMK_OK) {
      return status;
   }

   reply = RPCAllocMessage(cnx, bufferLength);
   if (reply == NULL) {
      status = VMK_LIMIT_EXCEEDED;
      goto error;
   }

   // Must copyin without the cnxLock in case copyin faults until PR 53504 is fixed
   SP_Unlock(&cnx->cnxLock);

   status = Util_CopyIn(reply->buffer, buffer, bufferLength, bufType);
   if (!RPCRelockAndVerify(cnx)) {
      status = VMK_IS_DISCONNECTED;
      goto error;
   }
   if (status != VMK_OK) {
      goto error;
   }

   reply->token = token;

   LOG(2, "Posting reply on cnx %s:0x%x token %d", cnx->name, cnxID, token);
   List_Insert(&reply->links, LIST_ATREAR(&cnx->replyList));
   cnx->nQueuedMessages++;

   CpuSched_Wakeup((uint32)cnx);
   RPCReleaseAndUnlockCnx(cnx);
   cnx = NULL;

   RPCStressWakeup();

   return VMK_OK;

error:

   if (reply != NULL) {
      RPCFreeMessage(cnx, reply);
   }

   if (cnx != NULL) {
      RPCReleaseAndUnlockCnx(cnx);
   }

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * RPC_GetReply --
 *
 *      Return then next available reply that matches token from the 
 *	connection.
 *
 * Results:
 *	VMK_OK if no problems, otherwise one of the following error 
 *	codes is returned:
 *
 *	VMK_IS_DISCONNECTED:	The host has disconnected.
 *	VMK_NO_RESOURCES:	The supplied argument buffer is too small to
 *				store the reply.
 *	VMK_WOULD_BLOCK:	There is no reply and RPC_CAN_BLOCK is not 
 *				set in flags.
 *	VMK_WAIT_INTERRUPTED:   Spurious(?) wakeup and RPC_ALLOW_INTERRUPTIONS
 *                              flag was set
 *      CpuSchedWait return status
 *
 * Side effects:
 *      The reply queue for the given connection has a new message
 *	added to it.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
RPC_GetReply(RPC_Connection cnxID, 	// IN: The connection to get the reply from.
             RPC_Token token, 		// IN: Token identifying which reply is wanted.
	     uint32 flags,		// IN: RPC_CAN_BLOCK if can wait if there are
					//     no available replies.  RPC_ALLOW_INTERRUPTIONS
                                        //     if *any* interruption should cause an early out   
	     char *outArgBuffer, 	// IN: Place to store reply data.
	     uint32 *outArgLength,	// IN/OUT: Size of outArgBuffer on input,
					//	   and size of reply on return.
             Util_BufferType bufType,	// IN: type of buffer
             World_ID switchToWorldID)  // IN: world to switch to while waiting
{
   VMK_ReturnStatus status = VMK_OK;
   RPCConnection *cnx = NULL;
   Bool triedOnce = FALSE;

   status = RPCFindAndLockCnx(cnxID, &cnx);
   if (status != VMK_OK) {
      return status;
   }
   while (1) {
      List_Links *l;   

      LIST_FORALL(&cnx->replyList, l) {
	 RPCReply *reply = (RPCReply *)l;
	 if (reply->token == token) {
	    if (*outArgLength < reply->bufferLength) {
	       status = VMK_NO_RESOURCES;
	       goto done;
	    }

            LOG(2, "Found reply on Cnx %s:0x%x", cnx->name, cnxID);
            // Remove reply from list (assume all is well).
	    List_Remove(&reply->links);
            ASSERT(cnx->nQueuedMessages > 0);
            cnx->nQueuedMessages--;

            // Unlock to copy message out (in case copy faults) until PR 53504 is fixed
            SP_Unlock(&cnx->cnxLock);
            status = Util_CopyOut(outArgBuffer, reply->buffer, reply->bufferLength, bufType);

            // Re-acquire RPC lock to free or re-insert message
            if (!RPCRelockAndVerify(cnx)) {
               status = VMK_IS_DISCONNECTED;
               goto done;
            }

	    if (status == VMK_OK) {
               *outArgLength = reply->bufferLength;
               RPCFreeMessage(cnx, reply);
	    } else {
               // May re-order messages, but only if multiple
               // receivers, and you're hosed anyway in that case.
               List_Insert(&reply->links, LIST_ATFRONT(&cnx->replyList));
               cnx->nQueuedMessages++;
            }
	    goto done;
	 }
      }

      if (!(flags & RPC_CAN_BLOCK)) {
         LOG(3, "cnx=%s:%d would block, not blocking.  Returning.", cnx->name, cnxID);
	 status = VMK_WOULD_BLOCK;
	 goto done;
      }

      if ((flags & RPC_ALLOW_INTERRUPTIONS) && triedOnce) {
         LOG(3, "cnx=%s:%d interrupted.  Returning.", cnx->name, cnxID);
         status = VMK_WAIT_INTERRUPTED;
         goto done;
      }

      LOG(2, "Waiting for reply on cnx %s:0x%x", cnx->name, cnxID);
      status = RPCWaitCnx(cnx, 0, switchToWorldID);

      if (status != VMK_OK) {
         break;
      }

      triedOnce = TRUE;
   }

done:
   RPCReleaseAndUnlockCnx(cnx);
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * RPC_Call --
 *
 *      Perform a synchronous RPC call on the given connection.
 *
 * Results:
 *	VMK_OK if no problems, otherwise one of the return codes from
 *	RPC_Send or RPC_GetReply can be returned.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
RPC_Call(RPC_Connection cnxID, 	// IN: The connection to perform the call on.
	 int32 function, 	// IN: Which RPC function this is.
         World_ID switchToWorldID, // IN: world to switch to while waiting
         char *inArgBuffer, 	// IN: Contains input arguments.
	 uint32 inArgLength,	// IN: Length of input arguments.
	 char *outArgBuffer, 	// IN: Place to put result.
	 uint32 *outArgLength)	// IN/OUT: Size of outArgBuffer on input and
				//	   actual size of result on return.
{
   RPC_Token token;
   VMK_ReturnStatus status;
   Timer_AbsCycles before = 0, after = 0;

   // send request
   status = RPC_Send(cnxID, function, RPC_REPLY_EXPECTED, 
		     inArgBuffer, inArgLength, UTIL_VMKERNEL_BUFFER, &token);
   if (status != VMK_OK) {
      return status;
   }

   before = Timer_GetCycles();

   // note that this RPC_Call path is not used for intra-VSMP RPCs (semaphores)
   status = RPC_GetReply(cnxID, token, RPC_CAN_BLOCK, 
                         outArgBuffer, outArgLength, UTIL_VMKERNEL_BUFFER,
                         switchToWorldID);

   after = Timer_GetCycles();
   if (rpcStatsData.state == RPC_STATS_ENABLE) {
      World_Handle *current = MY_RUNNING_WORLD;
      RPCStatUpdate(current, function, after - before);
   }

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * RPC_CheckPendingMsgs --
 *
 *      Return the list of connections with message pending on them.
 *
 * Results:
 *	1.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
int
RPC_CheckPendingMsgs(RPC_CnxList *cnxList)
{
   ASSERT_NOT_IMPLEMENTED(!CpuSched_IsHostWorld());
   memcpy(cnxList, &pendingCnx, sizeof(pendingCnx));

   return 1;
}

/*
 *----------------------------------------------------------------------
 *
 * RPCRemoveAndUnlockCnx --
 *
 *      Destroy the given connection and free its buffers.  All pending
 *      messages and replies are thrown away.  We zero out most of the
 *      connection structure to catch use-after-free.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
RPCRemoveAndUnlockCnx(RPCConnection *cnx)
{
   ASSERT(SP_IsLocked(&cnx->cnxLock));
   ASSERT(cnx->useCount == 0);
   ASSERT(cnx->pendingDestroy);

   RPCClearMask(&pendingCnx, cnx->id);

   while (!List_IsEmpty(&cnx->messageList)) {
      RPCMessage *msg = (RPCMessage *)List_First(&cnx->messageList);
      List_Remove(&msg->links);
      ASSERT(cnx->nQueuedMessages > 0);
      cnx->nQueuedMessages--;
      Heap_Free(cnx->heap, msg);
   }
   while (!List_IsEmpty(&cnx->replyList)) {
      RPCReply *reply = (RPCReply *)List_First(&cnx->replyList);
      List_Remove(&reply->links);
      ASSERT(cnx->nQueuedMessages > 0);
      cnx->nQueuedMessages--;
      Heap_Free(cnx->heap, reply);
   }
   ASSERT(cnx->nQueuedMessages == 0);

   while (!List_IsEmpty(&cnx->freeList)) {
      RPCMessage *msg = (RPCMessage *)List_First(&cnx->freeList);
      List_Remove(&msg->links);
      Heap_Free(cnx->heap, msg);
   }

   VMKPoll_WakeupAndRemoveWaiters(&cnx->pollWaiters);

   SP_Lock(&rpcLock);
   cnx->allocated = FALSE;
   SP_Unlock(&rpcLock);

   SP_Unlock(&cnx->cnxLock);

   RPCResetCnx(cnx);
}


/*
 *----------------------------------------------------------------------
 *
 * RPCAllocMessage --
 *
 *      Allocate a message from the connection's freelist
 *
 * Results:
 *      The allocated message, or NULL if too many messages allocated
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static RPCMessage *
RPCAllocMessage(RPCConnection *cnx, uint32 length)
{
   ASSERT(SP_IsLocked(&cnx->cnxLock));
   if (length > cnx->maxBufSize) {
      LOG(0, "requesting length = %d, maxSize = %d", length, cnx->maxBufSize);
      return NULL;
   }

   if (List_IsEmpty(&cnx->freeList)) {
      LOG(1, "cnx 0x%x: queued=%d", cnx->id, cnx->nQueuedMessages);
      return NULL;
   } else {
      RPCMessage *msg = (RPCMessage *)List_First(&cnx->freeList);
      List_Remove(&msg->links);
      msg->bufferLength = length;
      return msg;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * RPCFreeMessage --
 *
 *      Release the message to the connection's freelist
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
RPCFreeMessage(RPCConnection *cnx, RPCMessage *msg)
{
   ASSERT(SP_IsLocked(&cnx->cnxLock));

   // Insert at front to match popping off front (re-use warm memory)
   List_Insert(&msg->links, LIST_ATFRONT(&cnx->freeList));
}


/*
 *----------------------------------------------------------------------
 *
 * RPC_Poll --
 *
 *	Polls on the RPC connection for any incoming data.  If none is
 *	immediately available, a callback is set so that this world
 *	will be awakened when data becomes ready.
 *
 * Results:
 *      a VMK_ReturnStatus value.
 *
 * Side effects:
 *	Current world might get awakened later.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
RPC_Poll(RPC_Connection cnxID, uint32 inEvents, uint32 *outEvents, Bool notify)
{
   RPCConnection *cnx;
   VMK_ReturnStatus status;
   uint32 revents = 0;   

   if (!initialized) {
      return VMK_NOT_INITIALIZED;
   }

   status = RPCFindAndLockCnx(cnxID, &cnx);
   if (status != VMK_OK) {
      return status;
   }

   LOG(2, "Cnx %s:0x%x inEvents=0x%x", cnx->name, cnxID, inEvents);
   if (inEvents & RPC_POLL_GET_MSG) {
      if (!List_IsEmpty(&cnx->messageList)) {
         revents = RPC_POLL_GET_MSG;
      } else {
         status = VMK_WOULD_BLOCK;

         if (notify) {
            VMKPoll_AddWaiter(&cnx->pollWaiters, MY_RUNNING_WORLD->worldID);
         }
      }
   }

   if (inEvents & ~RPC_POLL_GET_MSG) {
      Warning("Only support RPC_POLL_GET_MSG tried 0x%x", inEvents);
      status = VMK_BAD_PARAM;
   }

   RPCReleaseAndUnlockCnx(cnx);

   if (status == VMK_OK || status == VMK_WOULD_BLOCK) {
      Util_CopyOut(outEvents, &revents, sizeof(revents), UTIL_VMKERNEL_BUFFER);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * RPC_PollCleanup --
 *
 *	Remove current world from poll waiters (if it's there).
 *
 * Results:
 *      a VMK_ReturnStatus value.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
RPC_PollCleanup(RPC_Connection cnxID)
{
   RPCConnection *cnx;
   VMK_ReturnStatus status;

   if (!initialized) {
      return VMK_NOT_INITIALIZED;
   }

   status = RPCFindAndLockCnx(cnxID, &cnx);
   if (status != VMK_OK) {
      return status;
   }

   VMKPoll_RemoveWaiter(&cnx->pollWaiters, MY_RUNNING_WORLD->worldID);

   RPCReleaseAndUnlockCnx(cnx);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * RPC_StatsDisable --
 *
 *      Stop gathering RPC stats
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
void
RPC_StatsDisable(void)
{
   if (rpcStatsData.state != RPC_STATS_DISABLE) {
      rpcStatsData.state = RPC_STATS_DISABLE;
      rpcStatsData.endTime = Timer_GetCycles();
      rpcStatsData.activeTime +=
         rpcStatsData.endTime - rpcStatsData.startTime;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * RPC_StatsEnable --
 *
 *      Start gathering RPC stats
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
void
RPC_StatsEnable(void)
{
   if (rpcStatsData.state != RPC_STATS_ENABLE) {
      rpcStatsData.state = RPC_STATS_ENABLE;
      rpcStatsData.startTime = Timer_GetCycles();
   }
}
