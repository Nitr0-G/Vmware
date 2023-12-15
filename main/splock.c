/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

#include "vm_types.h"
#include "vm_libc.h"
#include "x86.h"
#include "vmkernel.h"
#include "vm_asm.h"
#include "util.h"
#include "splock.h"
#include "sched.h"
#include "world.h"
#include "libc.h"
#include "serial.h"
#include "semaphore_ext.h"
#include "post.h"
#include "libc.h"
#include "debug.h"
#include "dump.h"
#include "log_int.h"

#define LOGLEVEL_MODULE SP
#define LOGLEVEL_MODULE_LEN 2
#include "log.h"

/*
 * If we spin too many times then we should panic because no one should hold a 
 * lock for very long.
 *
 * The variable maxSpinCycles is used to hold the max number of CPU cycles
 * to spin before printing a warning message. The value is calculated by:
 *       maxSpinCycles = SPIN_SECONDS * cpuHzEstimate
 * This gives about 4 billion cycles for a 1Ghz processor in a release build
 * and about 2 billion cycles in a developement build.
 *
 * Print a warning after the SPIN_SECONDS times the CPU cycles and panic
 * after SPIN_OUTS_BEFORE_PANIC times SPIN_SECONDS times the CPU cycles.
 */
#ifdef VMX86_DEVEL
#define SPIN_SECONDS           2
#else
#define SPIN_SECONDS           4
#endif

#define SPIN_OUTS_BEFORE_PANIC        5
#define MAX_SPIN_CYCLES_DEFAULT       (SPIN_SECONDS * 4000000000ull)
/*
 * Must set a large default value since we may use a lock before the 
 * SP_EarlyInit() func is called.
 */
static uint64 maxSpinCycles = MAX_SPIN_CYCLES_DEFAULT;

volatile Bool spInitialized = FALSE;
static Bool spDebugInitialized = FALSE;
static Bool SPPost(void *clientData, int id, SP_SpinLock *lock, SP_Barrier *barrier);
static Semaphore testSem;
static SP_SpinLockIRQ testLockIRQ;
static SP_RWLock testRWLock;
static SP_RWLockIRQ testRWLockIRQ;

#ifdef SPLOCK_STATS
static SP_SpinLockIRQ lockStatsLock;
static SP_SpinCommon *lockStatsList;
static uint32 lockStatsListRA;
static Proc_Entry lockStatsProcEntry;
Bool spLockStatsEnabled = SPLOCK_STATS;

/*
 * Enabling spLockCheckStatsList can hurt performance, but useful if some
 * bug is causing stats list to be messed up.
*/
Bool spLockCheckStatsList = FALSE;
#endif // SPLOCK_STATS

#ifdef SPLOCK_DEBUG
/*
 *----------------------------------------------------------------------
 *
 * SPStackGetStack --
 *
 *      Helper function to get stack of given type
 *
 * Results:
 *      The SP_Stack
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE volatile SP_Stack *
SPStackGetStack(Bool IRQ)
{
   volatile SP_Stack *stack = NULL;
   if (!spDebugInitialized) {
      return NULL;
   }
   if (IRQ) {
      stack = &myPRDA.spStack[SP_STACK_IRQ_STACK];
   } else {
      stack = &myPRDA.spStack[SP_STACK_NON_IRQ_STACK];
   }

   return stack;
}

/*
 *----------------------------------------------------------------------
 *
 * SPStackGetTopLock --
 *
 *      Get the last acquired lock on the given lock stack
 *
 * Results:
 *      The lock
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE SP_SpinCommon *
SPStackGetTopLock(Bool IRQ)
{
   volatile SP_Stack *stack = SPStackGetStack(IRQ);
   if (stack == NULL) {
      return NULL;
   }

   if (stack->nLocks != 0) {
      return stack->locks[stack->nLocks-1];
   } else {
      return NULL;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * SPStackAddLock --
 *
 *      Push the given lock in the given locks stack
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
SPStackAddLock(SP_SpinCommon *lck, Bool IRQ)
{
   volatile SP_Stack *stack = SPStackGetStack(IRQ);
   if (stack == NULL) {
      return;
   }

   ASSERT(stack->nLocks < SP_STACK_MAX_LOCKS);
   /*
    * Increment should happen after adding lock pointer because we could
    * be interrupted and someone could then query current rank.
    */
   stack->locks[stack->nLocks] = lck;
   stack->nLocks++;
}

/*
 *----------------------------------------------------------------------
 *
 * SPStackRemoveLock --
 *
 *      Push the given lock in the acquired locks stack
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
SPStackRemoveLock(SP_SpinCommon *lck, Bool IRQ)
{
   int i;
   volatile SP_Stack *stack = SPStackGetStack(IRQ);
   if (stack == NULL) {
      return;
   }
   ASSERT(stack->nLocks > 0);

   for (i = stack->nLocks - 1; i >= 0; i--) {
      if (stack->locks[i] == lck) {
         break;
      }
   }
   ASSERT(i >= 0);
   for (; i < stack->nLocks - 1; i++) {
      stack->locks[i] = stack->locks[i+1];
   }
   /*
    * Decrement should happen before remove lock pointer because we could
    * be interrupted and someone could then query current rank.
    */
   stack->nLocks--;
   stack->locks[i] = NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * SPStackGetCurrentRank --
 *
 *      Find the current lock rank of this lock stack.  Also return the
 *      lock responsible for the rank.
 *
 * Results:
 *      The current rank
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE SP_Rank
SPStackGetCurrentRank(Bool IRQ, SP_SpinCommon **retLock)
{
   int i;
   volatile SP_Stack *stack = SPStackGetStack(IRQ);

   *retLock = NULL;

   if (stack == NULL) {
      return SP_RANK_UNRANKED;
   }

   for (i = stack->nLocks - 1; i >= 0; i--) {
      SP_SpinCommon *lock = stack->locks[i];
      if (lock->debug.rank != SP_RANK_UNRANKED) {
         *retLock = lock;
         return lock->debug.rank;
      }
   }

   /*
    * We don't have any ranked locks, so return SP_RANK_UNRANKED, which is
    * the lowest possible rank.
    */
   return SP_RANK_UNRANKED;
}


/*
 *----------------------------------------------------------------------
 *
 * SPStackPrintLockStack
 *
 *      Print all the locks on this lock stack
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
SPStackPrintLockStack(Bool IRQ)
{
   int i;
   volatile SP_Stack *stack = SPStackGetStack(IRQ);
   if (stack == NULL) {
      return;
   }

   for (i = stack->nLocks - 1; i >= 0; i--) {
      SP_SpinCommon *lock = stack->locks[i];
      Warning("lock %s rank %x ra %x\n",
              lock->name, lock->debug.rank, lock->debug.ra);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * SPGetTopLock --
 *
 *      Get the last acquired lock on either lock stack
 *
 * Results:
 *      The lock
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static SP_SpinCommon *
SPGetTopLock(void)
{
   SP_SpinCommon *lock;
   if (!spDebugInitialized) {
      return NULL;
   }

   lock = SPStackGetTopLock(TRUE);
   if (lock == NULL) {
      lock = SPStackGetTopLock(FALSE);
   }
   return lock;
}


/*
 *----------------------------------------------------------------------
 *
 * SPDebugLocked --
 *
 *      Store debug info when a spin lock is acquired.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      Sets debug fields in the spin lock structure and
 *      sets the current lock.
 *
 *----------------------------------------------------------------------
 */
void
SPDebugLocked(SP_SpinCommon *lck, Bool IRQ)
{
   ASSERT(!spInitialized || lck->debug.initMagic == SPLOCK_INIT_MAGIC);

   lck->debug.ra = (uint32)__builtin_return_address(0);
   lck->debug.lastCPU = PRDA_GetPCPUNumSafe();
   lck->debug.holderCPU = PRDA_GetPCPUNumSafe();
   lck->debug.world = CpuSched_GetCurrentWorld();

   SPStackAddLock(lck, IRQ);
   if (spDebugInitialized) {
      if (!IRQ) {
         if (!Panic_IsSystemInPanic()) { // skip these checks if panic'ing
            ASSERT(!myPRDA.inInterruptHandler);
            if (MY_RUNNING_WORLD != NULL) {
               // preemptible worlds can't grab non-IRQ spin locks
               // don't worry if we're in the debugger, presumably it knows what its doing
               ASSERT(!CpuSched_IsPreemptible() || Debug_InDebugger());
            }
         }
      }
   }

}


/*
 *----------------------------------------------------------------------
 *
 * SPDebugUnlockedCommon --
 *
 *      Update debugging information when a spin lock is released.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      Sets debug fields in lock structure.  Verifies that the locks
 *      are released in LIFO order, unless this is a "special" known
 *      out-or-order lock release.
 *
 *----------------------------------------------------------------------
 */
void
SPDebugUnlocked(SP_SpinCommon *lck, Bool IRQ, Bool special)
{
   /*
    * We use stack specific getTopLock function here instead of generic
    * getlastlock because we could be called from a coredump scenario where
    * we're trying to release the special dump token lock, which is non IRQ,
    * while holding other IRQ locks.
    */
   SP_SpinCommon *last = SPStackGetTopLock(IRQ);

   ASSERT(!spInitialized || lck->debug.initMagic == SPLOCK_INIT_MAGIC);

   lck->debug.ra = (uint32)__builtin_return_address(0);
   lck->debug.lastCPU = PRDA_GetPCPUNumSafe();
   lck->debug.holderCPU = -1;
   lck->debug.world = CpuSched_GetCurrentWorld();

   if ((last == NULL) || (lck == last) || special) {
      SPStackRemoveLock(lck, IRQ);
   } else {
      Warning("releasing %p:%s last %p:%s ra=%p\n",
              lck, lck->name, 
              last, last->name,
              __builtin_return_address(0));
      ASSERT(0);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * SPDebugAcqReadLock --
 *
 *      Store debug info when a read lock is acquired.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      Sets debug fields in the read lock structure.
 *
 *----------------------------------------------------------------------
 */
void
SPDebugAcqReadLock(SP_RWCommon *rwl, Bool IRQ, SP_SpinCommon *lck)
{
   uint32 readerNum = Atomic_Read(&rwl->read) - 1;
   uint8 cpu = PRDA_GetPCPUNumSafe();

   ASSERT(!spInitialized || rwl->debug.initMagic == SPLOCK_INIT_MAGIC);

   SPStackAddLock(lck, IRQ);

   if (readerNum >= SP_RDLOCK_DBG_HISTORY) {
      return;
   }

   rwl->debug.tsLock[readerNum] = RDTSC();
   rwl->debug.raLock[readerNum] = (uint32)__builtin_return_address(0);
   rwl->debug.cpuLock[readerNum] = cpu;
   rwl->debug.worldLock[readerNum] = CpuSched_GetCurrentWorld();
}


/*
 *----------------------------------------------------------------------
 *
 * SPDebugRelReadLock --
 *
 *      Update debugging information when a r/w lock is released.
 *      Verifies that the locks are released in LIFO order, unless 
 *      this is a "special" known out-of-order lock release.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      Sets debug fields in lock structure.
 *
 *----------------------------------------------------------------------
 */
void
SPDebugRelReadLock(SP_RWCommon *rwl, Bool IRQ, 
                   SP_SpinCommon *lck, Bool special)
{
   uint32 readerNum = Atomic_Read(&rwl->read) - 1;
   uint8 cpu = PRDA_GetPCPUNumSafe();

   ASSERT(!spInitialized || rwl->debug.initMagic == SPLOCK_INIT_MAGIC);

   ASSERT(special || (SPGetTopLock() == lck));
   SPStackRemoveLock(lck, IRQ);

   if (readerNum >= SP_RDLOCK_DBG_HISTORY) {
      return;
   }
   rwl->debug.tsUnlock[readerNum] = RDTSC();
   rwl->debug.raUnlock[readerNum] = (uint32)__builtin_return_address(0);
   rwl->debug.cpuUnlock[readerNum] = cpu;
   rwl->debug.worldUnlock[readerNum] = CpuSched_GetCurrentWorld();
}


#endif // SPLOCK_DEBUG


/*
 *----------------------------------------------------------------------
 *
 * SP_CheckSpinCount --
 *
 *      Check spin count for possible deadlock.  If we've reached max
 *      spin count, then panic unless we're already in a panic, in which
 *      case, forcibly grant the lock to the caller.
 *
 * Results:
 *      TRUE if forcibly granding lock to caller, FALSE otherwise
 *
 * Side effects:
 *      May Panic() if spin count is exceeded and not already in panic
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
SP_CheckSpinCount(SP_SpinCommon *lck, uint64 *start, uint64 end,
                     uint32 *numSpinOuts, uint32 *switchCount)
{
   if ((end - *start) > maxSpinCycles) {
      uint32 newSwitchCount = CpuSched_WorldSwitchCount(MY_RUNNING_WORLD);
      if (newSwitchCount > *switchCount) {
         // we've been descheduled, so reset our spin counter, but
         // don't count this as a spin-out
         Serial_Printf("spinlock: %s: deschedule during spin wait\n",
                       lck->name);
         *switchCount = newSwitchCount;
         *start = end;
      } else {
         (*numSpinOuts)++;
         if (*numSpinOuts < SPIN_OUTS_BEFORE_PANIC) {
            /*
             * Don't call Warning here since we could be stuck on a lock in
             * Warning.
             */
            Serial_Printf("WARNING: %s: Spin count exceeded - possible deadlock\n",
                          lck->name);
            *start = end;
         } else if (Panic_IsCPUInPanic()) {
            // no need to cause another panic, let's just grant the lock
            Serial_Printf("WARNING: forcibly granting lock %s\n", lck->name);
            return TRUE;
         } else {
            Panic("Spin count exceeded (%s) - possible deadlock\n", lck->name);
         }
      }
   }
   return FALSE;
}


/*
 *----------------------------------------------------------------------
 *
 * SP_EarlyInit --
 *
 *      Initialize the synchronization primatives module.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Initializes test and stats locks, registers SPPost callback.
 *
 *----------------------------------------------------------------------
 */
void
SP_EarlyInit(void)
{
   extern uint64 cpuHzEstimate;
   /*
    * Setup maxSpinCycles based on the actual CPU speed
    * A 1 Ghz processor should spin for about 4,000,000,000 cycles max
    */
   maxSpinCycles = SPIN_SECONDS * cpuHzEstimate;
   Serial_Printf("SP_EarlyInit: maxSpinCycles[%u]\n", maxSpinCycles);
   
   SP_InitLockIRQ("testLockIRQ", &testLockIRQ, SP_RANK_IRQ_LEAF);
   SP_InitRWLock("testRWLock", &testRWLock, SP_RANK_LEAF);
   SP_InitRWLockIRQ("testRWLockIRQ", &testRWLockIRQ, SP_RANK_IRQ_LEAF);
   Semaphore_Init("testSem", &testSem, 1, SEMA_RANK_LEAF);
   POST_Register("Sync", SPPost, NULL);
#ifdef SPLOCK_STATS
   strncpy(lockStatsLock.common.name, "lockStatsLock",
           sizeof(lockStatsLock.common.name));
#ifdef SPLOCK_DEBUG
   lockStatsLock.common.debug.initMagic = SPLOCK_INIT_MAGIC;
   lockStatsLock.common.debug.rank = SP_RANK_LOCK_STATS;
#endif // SPLOCK_DEBUG
#endif // SPLOCK_STATS
}


/*
 *----------------------------------------------------------------------
 *
 * SPProcWrite --
 *
 *      Spin lock stats procfs write routine.
 *
 * Results:
 *      Returns 0 if successful.  
 *
 * Side effects:
 *      May reset the stats for all the locks.
 *
 *----------------------------------------------------------------------
 */
#ifdef SPLOCK_STATS
static int
SPProcWrite(UNUSED_PARAM(Proc_Entry *entry), char *buffer, UNUSED_PARAM(int *len))
{
   if(strncmp(buffer, "reset", 5) == 0) {
      SP_SpinCommon *lck;

      SP_LockIRQ(&lockStatsLock, SP_IRQL_KERNEL);
      lck = lockStatsList;
      while (lck) {
         lck->stats.uncontendedAcq = 0;
         lck->stats.contendedAcq = 0;
         lck->stats.waitCycles = 0LL;
         lck->stats.irqDisabledWhen = 0LL;
         lck->stats.irqDisabledCycles = 0LL;
         lck->stats.lockedWhen = 0LL;
         lck->stats.lockedCycles = 0LL;
         if (lck->readerWriter) {
            SP_RWCommon *rwl = lck->readerWriter;
            rwl->stats.uncontendedAcq = 0;
            rwl->stats.contendedAcq = 0;
            rwl->stats.waitCycles = 0LL;
            rwl->stats.irqDisabledWhen = 0LL;
            rwl->stats.irqDisabledCycles = 0LL;
            rwl->stats.lockedWhen = 0LL;
            rwl->stats.lockedCycles = 0LL;
         }
         lck = lck->stats.statsNext;
      }
      SP_UnlockIRQ(&lockStatsLock, SP_GetPrevIRQ(&lockStatsLock));
   } else if (strncmp(buffer, "enable", 6) == 0) {
      spLockStatsEnabled = 1;
   } else if (strncmp(buffer, "disable", 7) == 0) {
      spLockStatsEnabled = 0;
   }

   return 0;
}
#endif // SPLOCK_STATS


/*
 *----------------------------------------------------------------------
 *
 * SPProcRead --
 *
 *      Spin lock stats procfs status routine.
 *
 * Results:
 *      Writes human-readable lock stats into "buffer".  
 *      Sets "length" to number of bytes written.
 *      Always returns 0.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
#ifdef SPLOCK_STATS
static int
SPProcRead(UNUSED_PARAM(Proc_Entry *entry), char *buffer, int *len)
{
   SP_SpinCommon *lck;

   *len = 0;

   Proc_Printf(buffer, len, "commands:  enable | disable | reset\n\n");

   Proc_Printf(buffer, len, "%-23s %15s %15s %15s %20s %20s %20s\n",
               "",
               "contended",
               "uncontended",
               "failed",
               "wait",
               "locked",
               "irq disabled");
   Proc_Printf(buffer, len, "%-23s %15s %15s %15s %20s %20s %20s\n\n",
               "",
               "acquisitions",
               "acquisitions",
               "acquisitions",
               "cycles",
               "cycles",
               "cycles");

   if (!spLockStatsEnabled) {
      Proc_Printf(buffer, len, "-- disabled --\n\n");
   }

   SP_LockIRQ(&lockStatsLock, SP_IRQL_KERNEL);
   lck = lockStatsList;
   while (lck) {
#ifdef SPLOCK_DEBUG
      if (spInitialized && lck->debug.initMagic != SPLOCK_INIT_MAGIC) {
         SP_UnlockIRQ(&lockStatsLock, SP_GetPrevIRQ(&lockStatsLock));
         ASSERT(0);
      }
#endif // SPLOCK_DEBUG
      if (lck->stats.contendedAcq ||
            (lck->readerWriter &&
               lck->readerWriter->stats.contendedAcq)) {
         char *nameSuf = "   ";
         if (lck->readerWriter) {
            SP_RWCommon *rwl = lck->readerWriter;
            Proc_Printf(buffer, len, "%-20s%s %15u %15u %15u %20Lu %20Lu %20Lu\n", 
                        lck->name,
                        ".rd",
                        rwl->stats.contendedAcq,
                        rwl->stats.uncontendedAcq,
                        rwl->stats.failedAcq,
                        rwl->stats.waitCycles,
                        rwl->stats.lockedCycles,
                        rwl->stats.irqDisabledCycles);
            nameSuf = ".wr";
         }
         Proc_Printf(buffer, len, "%-20s%s %15u %15u %15u %20Lu %20Lu %20Lu\n", 
                     lck->name,
                     nameSuf,
                     lck->stats.contendedAcq,
                     lck->stats.uncontendedAcq,
                     lck->stats.failedAcq,
                     lck->stats.waitCycles,
                     lck->stats.lockedCycles,
                     lck->stats.irqDisabledCycles);
      }
      lck = lck->stats.statsNext;
   }
   SP_UnlockIRQ(&lockStatsLock, SP_GetPrevIRQ(&lockStatsLock));
   
   return 0;
}
#endif // SPLOCK_STATS


/*
 *----------------------------------------------------------------------
 *
 * SP_Init --
 *
 *      Synchronization primitives initialization.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Sets "spDebugInitialized" to true.
 *
 *----------------------------------------------------------------------
 */
void
SP_Init(void)
{
   spDebugInitialized = TRUE;
   ASSERT((SP_RANK_RECURSIVE_FLAG | SP_RANK_NUMERIC_MASK) == SP_RANK_MASK);
}


/*
 *----------------------------------------------------------------------
 *
 * SP_LateInit --
 *
 *      Late initialization.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Sets "spInitialized" to true and registers the splock proc
 *      node if stats are enabled.
 *
 *----------------------------------------------------------------------
 */
void
SP_LateInit(void)
{
#ifdef SPLOCK_STATS
   lockStatsProcEntry.read = SPProcRead;
   lockStatsProcEntry.write = SPProcWrite;
   Proc_Register(&lockStatsProcEntry, "lockstats", FALSE);
#endif // SPLOCK_STATS
   spInitialized = TRUE;
}

#ifdef SPLOCK_STATS
/*
 *----------------------------------------------------------------------
 *
 * SP_LockStatsListCheck --
 *
 *      Check the sanity of the lockStatsList of locks.  Also, return if
 *      the given lock is in the list.
 *
 * Results:
 *      TRUE if checkLock is in the list, FALSE otherwise
 *
 * Side effects:
 *      None, unless it PSODs or Panics
 *
 *----------------------------------------------------------------------
 */
Bool
SP_LockStatsListCheck(SP_SpinCommon *checkLock)
{
   SP_SpinCommon *lck, *loopStartLock = NULL;
   int nLocks = 0;
   Bool found = FALSE;
   
   ASSERT(spLockCheckStatsList);

   SP_LockIRQ(&lockStatsLock, SP_IRQL_KERNEL);
   lck = lockStatsList;
   while (lck) {
      if (lck == checkLock) {
         found = TRUE;
      }

      if (lck == lck->stats.statsNext) {
         SP_UnlockIRQ(&lockStatsLock, SP_GetPrevIRQ(&lockStatsLock));
         Panic("Lock points to itself (lck@ %p)\n", lck);
      }
#ifdef SPLOCK_DEBUG
      if (lck->debug.initMagic != SPLOCK_INIT_MAGIC) {
         SP_UnlockIRQ(&lockStatsLock, SP_GetPrevIRQ(&lockStatsLock));
         Panic("Lock initMagic wrong (got %#x, expected %#x)\n",
               lck->debug.initMagic, SPLOCK_INIT_MAGIC);
      }
#endif

      /*
       * check for loops in the chain.  If we get more than 100K locks,
       * there's probably a loop somewhere, so mark the 100Kth lock as loop
       * start and start dumping all locks in the list until we return back
       * to the loopStartLock, and Panic...
       */
      nLocks++;
      if (nLocks == 100100) {
         loopStartLock = lck;
         Warning("Found %d locks, probably a chain. start=%p", nLocks, lck);
      }
      if (loopStartLock != NULL) {
         Log("%p: %s next=%p nra=0x%x",
             lck, lck->name, lck->stats.statsNext, lck->stats.statsNextRA);
      }
      lck = lck->stats.statsNext;
      if ((lck != NULL) && (lck == loopStartLock)) {
         Panic("Found a loop\n");
      }
   }
   SP_UnlockIRQ(&lockStatsLock, SP_GetPrevIRQ(&lockStatsLock));

   return found;
}
#endif

/*
 *----------------------------------------------------------------------
 *
 * SP_InitLockCommon --
 *
 *      Common initialization for spin locks.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Checks and sets magic in debug header and sets the name field.
 *
 *----------------------------------------------------------------------
 */
void
SP_InitLockCommon(char *name, SP_SpinCommon *lck, SP_RankFlags rankFlags)
{

#ifdef SPLOCK_DEBUG
   // make sure we haven't already initialized this lock
   if (lck->debug.initMagic == SPLOCK_INIT_MAGIC) {
      Warning("Lock %s already initialized", lck->name);
   }
#endif // SPLOCK_DEBUG

   memset(lck, 0, sizeof(*lck));
   strncpy((char *)lck->name, name, SPINLOCK_NAME_SIZE);

#ifdef SPLOCK_DEBUG
   lck->debug.initMagic = SPLOCK_INIT_MAGIC;
   lck->debug.rank = rankFlags & SP_RANK_MASK;
   lck->debug.holderCPU = -1;
#endif // SPLOCK_DEBUG

#ifdef SPLOCK_STATS
   if (spLockCheckStatsList) {
      ASSERT(SP_LockStatsListCheck(lck) == FALSE);
   }
   if (rankFlags & SP_FLAG_SKIPSTATS) {
      lck->stats.skipStats = TRUE;
   } else {
      lck->stats.skipStats = FALSE;
      SP_LockIRQ(&lockStatsLock, SP_IRQL_KERNEL);
      lck->stats.statsNext = lockStatsList;
      lck->stats.statsNextRA = lockStatsListRA;
      lockStatsList = lck;
      lockStatsListRA = (uint32)__builtin_return_address(0);
      if (lck == lck->stats.statsNext) {
         SP_UnlockIRQ(&lockStatsLock, SP_GetPrevIRQ(&lockStatsLock));
         ASSERT(0);
      }
      SP_UnlockIRQ(&lockStatsLock, SP_GetPrevIRQ(&lockStatsLock));
   }
   if (spLockCheckStatsList) {
      ASSERT(SP_LockStatsListCheck(lck) == !lck->stats.skipStats);
   }
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * SP_InitRWLockCommon --
 *
 *      Common initialization for a reader/writer lock.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      Sets magic in debug header and initializes the write spin lock.
 *
 *----------------------------------------------------------------------
 */
void SP_InitRWLockCommon(SP_RWCommon *rwl)
{
   memset(rwl, 0, sizeof(*rwl));
#ifdef SPLOCK_DEBUG
   rwl->debug.initMagic = SPLOCK_INIT_MAGIC;
#endif // SPLOCK_DEBUG
}


/*
 *----------------------------------------------------------------------
 *
 * SP_CleanupLockCommon --
 *
 *      Common clean up for a spin lock.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Lock is cleaned up.
 *
 *----------------------------------------------------------------------
 */
void
SP_CleanupLockCommon(SP_SpinCommon *lck)
{
#ifdef SPLOCK_DEBUG
   ASSERT(lck->debug.initMagic == SPLOCK_INIT_MAGIC);
#endif // SPLOCK_DEBUG
#ifdef SPLOCK_STATS
   if (spLockCheckStatsList) {
      ASSERT(SP_LockStatsListCheck(lck) == !lck->stats.skipStats);
   }
   if (!lck->stats.skipStats) {
      SP_SpinCommon *cur;
      SP_SpinCommon *prev;
      
      SP_LockIRQ(&lockStatsLock, SP_IRQL_KERNEL);
      cur = lockStatsList;
      prev = cur;
      while (cur && (cur != lck)) {
      if (cur == cur->stats.statsNext) {
         // unlock so we can dump core
         SP_UnlockIRQ(&lockStatsLock, SP_GetPrevIRQ(&lockStatsLock));
         ASSERT(0);
      }
      prev = cur;
      cur = cur->stats.statsNext;
      }
      if (cur == NULL) {
         // unlock so we can dump core
         SP_UnlockIRQ(&lockStatsLock, SP_GetPrevIRQ(&lockStatsLock));
         ASSERT(0);
      } else if (cur == lockStatsList) {
         lockStatsList = cur->stats.statsNext;
         lockStatsListRA = cur->stats.statsNextRA;
      } else {
         prev->stats.statsNext = cur->stats.statsNext;
	 prev->stats.statsNextRA = cur->stats.statsNextRA;
      }
      
      SP_UnlockIRQ(&lockStatsLock, SP_GetPrevIRQ(&lockStatsLock));
   }
   if (spLockCheckStatsList) {
      ASSERT(SP_LockStatsListCheck(lck) == FALSE);
   }
#endif // SPLOCK_STATS

   memset(lck, 0, sizeof(*lck));

#ifdef SPLOCK_DEBUG
   lck->debug.initMagic = 0xdeaddead;
#endif // SPLOCK_DEBUG
}


/*
 *----------------------------------------------------------------------
 *
 * SP_TryLockIRQ
 *
 *      Try once to get an IRQ lock.  
 *
 * Results:
 *      if lock was successfully acquired,
 *         *acq = TRUE
 *      else
 *         *acq = FALSE
 *      Return the previous interrupt level.
 *
 * Side effects:
 *      Interrupts may be disabled, if not already.
 *      Updates lock stats, if they are active.
 *
 *----------------------------------------------------------------------
 */
SP_IRQL
SP_TryLockIRQ(SP_SpinLockIRQ *lck, UNUSED_PARAM(SP_IRQL irql), Bool *acq)
{
   uint32 eflags;
   SP_IRQL prevIRQL;
   // ASSERT(irql == SP_IRQL_KERNEL);

   SAVE_FLAGS(eflags);
   if (eflags & EFLAGS_IF) {
      CLEAR_INTERRUPTS();
      prevIRQL = SP_IRQL_NONE;
   } else {
      prevIRQL = SP_IRQL_KERNEL;
   }
   *acq = (SPTestAndSet(&lck->common.lock) == 0);
   if (!*acq) {
      SP_RestoreIRQ(prevIRQL);
   } else {
      SPDebugLocked(&lck->common, TRUE);
   }

#ifdef SPLOCK_STATS
   if ((SPLOCK_STATS_ON) && (!lck->common.stats.skipStats)) {
      if (*acq) {
         lck->common.stats.lockedWhen = RDTSC();
         lck->common.stats.uncontendedAcq++;
      } else {
         lck->common.stats.failedAcq++; 
      }
   }
#endif // SPLOCK_STATS

   /*
    * DON'T put anything after this barrier.  It's here to prevent the
    * compiler from reording the code in the lock routine after the code
    * inside the locked region (PR 28372).
    */
   COMPILER_MEM_BARRIER();

   return prevIRQL;
}


/*
 *----------------------------------------------------------------------
 *
 * SP_TryLock
 *
 *      Try once to get a lock.  
 *
 * Results:
 *      if lock was successfully acquired,
 *         TRUE
 *      else
 *         FALSE
 *
 * Side effects:
 *      Updates lock stats, if they are active.
 *
 *----------------------------------------------------------------------
 */
Bool
SP_TryLock(SP_SpinLock *lck)
{
   Bool success;
   success = (SPTestAndSet(&lck->common.lock) == 0);
#ifdef SPLOCK_STATS
   if ((SPLOCK_STATS_ON) && (!lck->common.stats.skipStats)) {
      if (success) {
         lck->common.stats.lockedWhen = RDTSC();
         lck->common.stats.uncontendedAcq++;
      } else {
         lck->common.stats.failedAcq++; 
      }
   }
#endif // SPLOCK_STATS

   if (success) {
      SPDebugLocked(&lck->common, FALSE);
   }

   /*
    * DON'T put anything after this barrier.  It's here to prevent the
    * compiler from reording the code in the lock routine after the code
    * inside the locked region (PR 28372).
    */
   COMPILER_MEM_BARRIER();

   return success;
}  


/*
 *----------------------------------------------------------------------
 *
 * SP_WaitLockIRQ
 *
 *      Disable interrupts and lock a spin lock after a wait.
 *
 * Results:
 *      none
 *
 * Side effects:
 *      Interrupts may be disabled, if not already.
 *      Updates lock stats, if they are active.
 *
 *----------------------------------------------------------------------
 */
void
SP_WaitLockIRQ(SP_SpinLockIRQ *lck, uint32 ifEnabled)
{
   int numSpinOuts = 0;
   TSCCycles start = RDTSC();
   uint32 delay = lck->common.delay;
#ifdef SPLOCK_STATS
   TSCCycles startWait = start;
   uint64 intrEnabledWhen = 0LL;
   uint64 intrEnabledTime = 0LL;
#endif // SPLOCK_STATS
   uint32 switchCount = CpuSched_WorldSwitchCount(MY_RUNNING_WORLD);

   // do test&test&set with exponential backoff
   do {
      TSCCycles end = RDTSC();
      if (ifEnabled) {
         ENABLE_INTERRUPTS();
#ifdef SPLOCK_STATS
         if (SPLOCK_STATS_ON) {
            intrEnabledWhen = RDTSC();
         }
#endif // SPLOCK_STATS
      }
      delay      = MIN((delay << 1) + 1, SP_MAX_SPIN_DELAY); // increase backoff
      end       += (myPRDA.randSeed = Util_FastRand(myPRDA.randSeed)) & delay;
      while (RDTSC() < end) {
         PAUSE(); // On P4, improves spinlock power+perf; REPZ-NOP on non-P4
      }
      if (ifEnabled) {
         CLEAR_INTERRUPTS();
#ifdef SPLOCK_STATS
         if (SPLOCK_STATS_ON) {
            intrEnabledTime += RDTSC() - intrEnabledWhen;
         }
#endif // SPLOCK_STATS
      }
      if (SP_CheckSpinCount(&lck->common, &start, end, &numSpinOuts, &switchCount)) {
         break;
      }
   } while ((lck->common.lock != 0) || (SPTestAndSet(&lck->common.lock) != 0));
   lck->common.delay = MAX((delay >> 2), SP_MIN_SPIN_DELAY); // decrease backoff

#ifdef SPLOCK_STATS
   if ((SPLOCK_STATS_ON) && (!lck->common.stats.skipStats)) {
      lck->common.stats.waitCycles += RDTSC() - startWait;
      if (lck->common.stats.irqDisabledWhen != 0) {
         // compensate for the time irqs were enabled
         lck->common.stats.irqDisabledWhen += intrEnabledTime; 
      }
   }
#endif // SPLOCK_STATS
}


/*
 *----------------------------------------------------------------------
 *
 * SP_WaitLock --
 *
 *      Lock a spin lock after a wait.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      Updates lock stats, if they are active.
 *
 *----------------------------------------------------------------------
 */
void
SP_WaitLock(SP_SpinLock *lck)
{
   int numSpinOuts = 0;
   TSCCycles start = RDTSC();
   uint32 delay = lck->common.delay;
#ifdef SPLOCK_STATS
   TSCCycles startWait = start;
#endif // SPLOCK_STATS
   uint32 switchCount = CpuSched_WorldSwitchCount(MY_RUNNING_WORLD);

   // do test&test&set with exponential backoff
   do {
      TSCCycles end = RDTSC();
      delay      = MIN((delay << 1) + 1, SP_MAX_SPIN_DELAY); // increase backoff 
      end       += (myPRDA.randSeed = Util_FastRand(myPRDA.randSeed)) & delay;
      while (RDTSC() < end) {
         PAUSE(); // On P4, improves spinlock power+perf; REPZ-NOP on non-P4
      }
      if (SP_CheckSpinCount(&lck->common, &start, end, &numSpinOuts, &switchCount)) {
         break;
      }
   } while ((lck->common.lock != 0) || (SPTestAndSet(&lck->common.lock) != 0));
   lck->common.delay = MAX((delay >> 2), SP_MIN_SPIN_DELAY); // decrease backoff

#ifdef SPLOCK_STATS
   if ((SPLOCK_STATS_ON) && (!lck->common.stats.skipStats)) {
      lck->common.stats.waitCycles += RDTSC() - startWait;
   }
#endif // SPLOCK_STATS
}


/*
 *----------------------------------------------------------------------
 *
 * SP_WaitReadLock
 *
 *      Wait for all readers to release a lock.
 *
 * Results:
 *      none
 *
 * Side effects:
 *      Updates lock stats, if they are active.
 *
 *----------------------------------------------------------------------
 */
void
SP_WaitReadLock(SP_RWLock *rwl)
{
   int numSpinOuts = 0;
   TSCCycles start = RDTSC();
   uint32 delay = rwl->common.delay;
#ifdef SPLOCK_STATS
   TSCCycles startWait = start;
#endif // SPLOCK_STATS
   uint32 switchCount = CpuSched_WorldSwitchCount(MY_RUNNING_WORLD);

   // do test&test&set with exponential backoff
   do {
      TSCCycles end = RDTSC();
      delay      = MIN((delay << 1) + 1, SP_MAX_SPIN_DELAY); // increase backoff
      end       += (myPRDA.randSeed = Util_FastRand(myPRDA.randSeed)) & delay;
      while (RDTSC() < end) {
         PAUSE(); // On P4, improves spinlock power+perf; REPZ-NOP on non-P4
      }
      if (SP_CheckSpinCount(&rwl->write.common, &start, end, &numSpinOuts, &switchCount)) {
         break;
      }
   } while (Atomic_Read(&rwl->common.read));
   rwl->common.delay = MAX((delay >> 2), SP_MIN_SPIN_DELAY); // decrease backoff

#ifdef SPLOCK_STATS
   if ((SPLOCK_STATS_ON) && (!rwl->common.stats.skipStats)) {
      rwl->common.stats.waitCycles += RDTSC() - startWait;
   }
#endif // SPLOCK_STATS
}


/*
 *----------------------------------------------------------------------
 *
 * SP_InitBarrier --
 *
 *      Initialize a spin barrier.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      Barrier and its associated spin lock are initialized.
 *
 *----------------------------------------------------------------------
 */
void
SP_InitBarrier(char *name, uint32 members, SP_Barrier *barrier)
{
   SP_InitLockIRQ(name, &barrier->lock, SP_RANK_IRQ_LEAF);
   barrier->smashed = FALSE;
   barrier->sense = TRUE;
   barrier->members = members;
   barrier->count  = members;
}


/*
 *----------------------------------------------------------------------
 *
 * SP_CleanupBarrier --
 *
 *      Cleanup a barrier.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      Lock associated with the spin barrier is cleaned up.
 *
 *----------------------------------------------------------------------
 */
void
SP_CleanupBarrier(SP_Barrier *barrier)
{
   SP_CleanupLockIRQ(&barrier->lock);
}


/*
 *----------------------------------------------------------------------
 *
 * SP_SmashBarrier --
 *
 *      Release everyone from the barrier, and make this barrier a nop for
 *      all future attempts to use it.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
SP_SmashBarrier(SP_Barrier *barrier)
{
   Warning("Smashing barrier %s.", barrier->lock.common.name);

   SP_LockIRQ(&barrier->lock, SP_IRQL_KERNEL);
   barrier->smashed = TRUE;
   SP_UnlockIRQ(&barrier->lock, SP_GetPrevIRQ(&barrier->lock));
}


/*
 *----------------------------------------------------------------------
 *
 * SP_SpinBarrier --
 *
 *      Wait until all members of barrier have arrived.  
 *
 * WARNING:
 *      Unlike SP_Lock, this routine yields the CPU while waiting for
 *      the other members to arrive.  See SP_SpinBarrierNoYield for a
 *      non-yielding version.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
void
SP_SpinBarrier(SP_Barrier *barrier)
{
   Bool sense;
   uint16 count;

   // determine which sense to use
   sense = ! barrier->sense;

   // fetch and decrement the counter
   // n.b., could use an atomic fetch and decrement instruction here
   SP_LockIRQ(&barrier->lock, SP_IRQL_KERNEL);
   count = barrier->count;
   barrier->count = count-1;
   SP_UnlockIRQ(&barrier->lock, SP_GetPrevIRQ(&barrier->lock));

   if (count == 1) {
      // I am the last one, reset the counter and flip the sense
      // n.b., must be done in this order
      barrier->count = barrier->members;
      barrier->sense = sense;
   } else {
      while ((barrier->sense != sense) && !barrier->smashed) {
         Util_Udelay(1);
         if (barrier->sense != sense) {
            CpuSched_YieldThrottled();
         }
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * SP_SpinBarrierNoYield --
 *
 *      Wait until all members of barrier have arrived.  This version
 *      is a true spinlock; it does not yield the CPU while waiting.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
void
SP_SpinBarrierNoYield(SP_Barrier *barrier)
{
   Bool sense;
   uint16 count;

   // determine which sense to use
   sense = ! barrier->sense;

   // fetch and decrement the counter
   // n.b., could use an atomic fetch and decrement instruction here
   SP_LockIRQ(&barrier->lock, SP_IRQL_KERNEL);
   count = barrier->count;
   barrier->count = count-1;
   SP_UnlockIRQ(&barrier->lock, SP_GetPrevIRQ(&barrier->lock));

   if (count == 1) {
      // I am the last one, reset the counter and flip the sense
      // n.b., must be done in this order
      barrier->count = barrier->members;
      barrier->sense = sense;
   } else {
      while ((barrier->sense != sense) && !barrier->smashed) { // spin
         PAUSE();
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * SPPost
 *
 *      Perform a test on the atomicity provided by this spin lock,
 *      barrier and semphore code. 
 *
 * Results:
 *      FALSE if error detected, TRUE otherwise
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
#define NUMITERS 5000
#define BARRIER_COUNT 100
#define BUSYTIME 5000
static volatile uint32 testCount;
static volatile uint32 barCount;
static uint32 procMask = 0;
static Atomic_uint32 numFailed = {0};

Bool
SPPost(UNUSED_PARAM(void *clientData),
       int id,
       SP_SpinLock *lock,
       SP_Barrier *barrier)
{
   uint32 count;
   Bool fail;
   int i, j;

   // test spin lock
   for (i = 0; i < NUMITERS; i++) {
      SP_Lock(lock);
      procMask |= (1 << id);
      for (j = 0; j < BUSYTIME; j++) {
         // widens the window for lock contention
      }
      if (procMask & ~(1 << id)) {
         Warning("spin lock POST failure");
         Atomic_Inc(&numFailed);
      }
      procMask = 0;
      SP_Unlock(lock);
   }

   // test barrier
   if (id == 0) {
      barCount = 0;
   }
   SP_SpinBarrier(barrier);
   for (i = 0; i < BARRIER_COUNT; i++) {
      if (id == 0) {
         barCount++;
      }
      SP_SpinBarrier(barrier);
   }
   if (barCount != BARRIER_COUNT) {
      Warning("%d: barrier POST failure, count=%u, expected %u",
              id, barCount, BARRIER_COUNT);
      Atomic_Inc(&numFailed);
   }

   // test IRQ spin lock
   if (id == 0) {
      testCount = 0;
   }
   SP_SpinBarrier(barrier);
   // try to increment testCount atomically using IRQ lock
   for (i = 0; i < NUMITERS; i++) {
      SP_LockIRQ(&testLockIRQ, SP_IRQL_KERNEL);
      count = testCount;
      for (j = 0; j < BUSYTIME; j++) {
         // this increases the window for contention
      }
      testCount = count+1;
      SP_UnlockIRQ(&testLockIRQ, SP_GetPrevIRQ(&testLockIRQ));
   }
   SP_SpinBarrier(barrier);
   if (testCount != NUMITERS*numPCPUs) {
      Warning("%d: SPLock IRQ POST failure, count=%u, expected %u",
              id, testCount, NUMITERS*numPCPUs);
      Atomic_Inc(&numFailed);
   }
   SP_SpinBarrier(barrier);

   // test semaphore
   if (id == 0) {
      testCount = 0;
   }
   SP_SpinBarrier(barrier);
   // try to increment testCount atomically using semaphore
   for (i = 0; i < NUMITERS; i++) {
      Semaphore_Lock(&testSem);
      count = testCount;
      for (j = 0; j < BUSYTIME; j++) {
         // this increases the window for contention
      }
      testCount = count+1;
      Semaphore_Unlock(&testSem);
   }
   SP_SpinBarrier(barrier);
   if (testCount != NUMITERS*numPCPUs) {
      Warning("%d: semaphore POST failure, count=%u, expected %u",
              id, testCount, NUMITERS*numPCPUs);
      Atomic_Inc(&numFailed);
   }
   SP_SpinBarrier(barrier);

   // test reader vs. writer locks
   if (id == 0) {
      testCount = 0;
   }
   SP_SpinBarrier(barrier);
   for (i = 0; i < NUMITERS; i++) {
      if (id == 0) {
         SP_AcqReadLock(&testRWLock);
         count = testCount;
         for (j = 0; j < BUSYTIME; j++) {
            PAUSE();
         }
         testCount = count+1;
         SP_RelReadLock(&testRWLock);
      } else {
         SP_AcqWriteLock(&testRWLock);
         count = testCount;
         for (j = 0; j < BUSYTIME; j++) {
            PAUSE();
         }
         testCount = count+1;
         SP_RelWriteLock(&testRWLock);
      }
   }
   SP_SpinBarrier(barrier);
   if (testCount != NUMITERS*numPCPUs) {
      Warning("%d: reader/writer lock POST failure, count = %d, expected %d",
              id, testCount, NUMITERS*numPCPUs);
      Atomic_Inc(&numFailed);
   }
   SP_SpinBarrier(barrier);

   // test reader vs. reader locks
   fail = FALSE;
   SP_SpinBarrier(barrier);
   for (i = 0; i < NUMITERS; i++) {
      if (!SP_TryReadLock(&testRWLock)) {
         fail = TRUE;
      } else {
         for (j = 0; j < BUSYTIME; j++) {
         }
         SP_RelReadLock(&testRWLock);
      }
   }
   SP_SpinBarrier(barrier);
   if (fail) {
      Warning("%d: reader/reader lock POST failure", id);
      Atomic_Inc(&numFailed);
   }
   SP_SpinBarrier(barrier);

   // test reader vs. writer irq locks
   if (id == 0) {
      testCount = 0;
   }
   SP_SpinBarrier(barrier);
   for (i = 0; i < NUMITERS; i++) {
      if (id == 0) {
         SP_IRQL prevIRQL;
	 prevIRQL = SP_AcqReadLockIRQ(&testRWLockIRQ, SP_IRQL_KERNEL);
         count = testCount;
         for (j = 0; j < BUSYTIME; j++) {
         }
         testCount = count+1;
         SP_RelReadLockIRQ(&testRWLockIRQ, prevIRQL);
      } else {
         SP_AcqWriteLockIRQ(&testRWLockIRQ, SP_IRQL_KERNEL);
         count = testCount;
         for (j = 0; j < BUSYTIME; j++) {
         }
         testCount = count+1;
         SP_RelWriteLockIRQ(&testRWLockIRQ, SP_GetPrevWriteIRQ(&testRWLockIRQ));
      }
   }
   SP_SpinBarrier(barrier);
   if (testCount != NUMITERS*numPCPUs) {
      Warning("%d: reader/writer irq lock POST failure, count=%u, expected %u",
              id, testCount, NUMITERS*numPCPUs);
      Atomic_Inc(&numFailed);
   }
   SP_SpinBarrier(barrier);

   // test reader vs. reader irq locks
   count = 0;
   SP_SpinBarrier(barrier);
   for (i = 0; i < NUMITERS; i++) {
      SP_IRQL prevIRQL;
      Bool acquired = FALSE;
      prevIRQL = SP_TryReadLockIRQ(&testRWLockIRQ, SP_IRQL_KERNEL, &acquired);
      for (j = 0; j < BUSYTIME; j++) {
      }
      if (acquired) {
         SP_RelReadLockIRQ(&testRWLockIRQ, prevIRQL);
      } else {
         count++;
      }
   }
   SP_SpinBarrier(barrier);
   if (count != 0) {
      Warning("%d: reader/reader lock POST failure", id);
      Atomic_Inc(&numFailed);
   }

   return (numFailed.value == 0);
}




#ifdef SPLOCK_DEBUG
/*
 *----------------------------------------------------------------------
 *
 * SP_AssertNoLocksHeld --
 *
 *      Assert that no spin locks are currently held by this cpu.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May assert fail.
 *
 *----------------------------------------------------------------------
 */
void
SP_AssertNoLocksHeld(void)
{
   // assert no spin locks locks held except when debugging or dumping
   if (spDebugInitialized && !Debug_InDebugger() && !Panic_IsSystemInPanic() &&
       (SPGetTopLock() != NULL)) {
      SP_SpinCommon *lock = SPGetTopLock();
      Panic("Asserting no locks held, but holding lock %p:%s ra=%p\n",
            lock, lock->name, __builtin_return_address(0));
   }
}

/*
 *----------------------------------------------------------------------
 *
 * SP_AssertNoIRQLocksHeld --
 *
 *      Assert that no IRQ spin locks are currently held by this cpu.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May assert fail.
 *
 *----------------------------------------------------------------------
 */
void
SP_AssertNoIRQLocksHeld(void)
{
   // assert no spin locks locks held except when debugging or dumping
   if (spDebugInitialized && !Debug_InDebugger() && !Panic_IsSystemInPanic() &&
       (SPStackGetTopLock(TRUE) != NULL)) {
      SP_SpinCommon *lock = SPStackGetTopLock(TRUE);
      Panic("Asserting no IRQ locks held, but holding lock %p:%s ra=%p\n",
            lock, lock->name, __builtin_return_address(0));
   }
}

/*
 *----------------------------------------------------------------------
 *
 * SPAssertOneLockHeldCommon --
 *
 *      Assert that only the given lock is held
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May assert fail.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
SPAssertOneLockHeldCommon(const SP_SpinCommon *heldLock)
{
   // assert no spin locks locks held except when debugging or dumping
   if (spDebugInitialized && !Debug_InDebugger() && !Panic_IsSystemInPanic()) {
      int numLocksHeld = myPRDA.spStack[SP_STACK_IRQ_STACK].nLocks +
         myPRDA.spStack[SP_STACK_NON_IRQ_STACK].nLocks;
      SP_SpinCommon *lastLock = SPGetTopLock();

      if ((numLocksHeld != 1) || (lastLock != heldLock)) {
         Panic("Asserting one locks held, but holding %d locks %p:%s ra=%p\n",
               numLocksHeld, lastLock, lastLock->name,
               __builtin_return_address(0));
      }
   }
}

void
SP_AssertOneLockHeld(const SP_SpinLock *lock)
{
   SPAssertOneLockHeldCommon(&lock->common);
}
void
SP_AssertOneLockHeldIRQ(const SP_SpinLockIRQ *lockIRQ)
{
   SPAssertOneLockHeldCommon(&lockIRQ->common);
}
#endif


/*
 *----------------------------------------------------------------------
 *
 * SP_GetLockAddr{IRQ} --
 *
 *      Get the spin lock address.
 *
 * Results:
 *      Address of the lock.
 *
 * Side effects:
 *      none
 *
 *----------------------------------------------------------------------
 */
void *
SP_GetLockAddrIRQ(SP_SpinLockIRQ *lck)
{
   return (void *)&lck->common.lock;
}

void *
SP_GetLockAddr(SP_SpinLock *lck)
{
   return (void *)&lck->common.lock;
}


#ifdef SPLOCK_DEBUG
/*
 *----------------------------------------------------------------------
 *
 * SPGetCurrentRank --
 *
 *      Find the current lock rank of this CPU
 *
 * Results:
 *      The current rank
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE SP_Rank
SPGetCurrentRank(SP_SpinCommon **lock)
{
   SP_Rank rank = SPStackGetCurrentRank(TRUE, lock);
   if (rank == SP_RANK_UNRANKED) {
      rank = SPStackGetCurrentRank(FALSE, lock);
   }
   return rank;
}


/*
 *----------------------------------------------------------------------
 *
 * SPPrintLockStack
 *
 *      Print all the locks currently held
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
SPPrintLockStack(void)
{
   SPStackPrintLockStack(TRUE);
   SPStackPrintLockStack(FALSE);
}

/*
 *----------------------------------------------------------------------
 *
 * SPCheckRank -- 
 *
 *      Check to see if we're allowed to grab a lock with the given rank
 *      based on our current rank.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      Might panic
 *
 *----------------------------------------------------------------------
 */
void
SPCheckRank(SP_SpinCommon *lck)
{
   SP_Rank currentRank, currentNumericRank;
   SP_Rank lockNumericRank;
   SP_SpinCommon *currentLock;

   // too early to check prda
   if (!spDebugInitialized) {
      return;
   }

   // no checking if we've already panic'ed.
   if (Panic_IsSystemInPanic()) {
      return;
   }

   // we are only allowed log locks in the NMI handler
   if (lck->debug.rank != SP_RANK_LOG) {
      ASSERT(!myPRDA.inNMI);
   }

   // unranked lock is always fine
   if (lck->debug.rank == SP_RANK_UNRANKED) {
      return;
   }

   currentRank = SPGetCurrentRank(&currentLock);

   // not holding any locks right now
   if (currentRank == SP_RANK_UNRANKED) {
      return;
   }

   lockNumericRank = lck->debug.rank & SP_RANK_NUMERIC_MASK;
   currentNumericRank = currentRank & SP_RANK_NUMERIC_MASK;

   // rank check OK
   if (lockNumericRank > currentNumericRank) {
      return;
   }

   // recursive rank check OK
   if ((lck->debug.rank & SP_RANK_RECURSIVE_FLAG) &&
       (currentRank & SP_RANK_RECURSIVE_FLAG)) {
      if (lockNumericRank == currentNumericRank) {
         return;
      }
   }

   SPPrintLockStack();
   Panic("Lock rank violation: current %x (%s:%x) asking for %x (%s:%p)\n",
         currentRank, currentLock->name, currentLock->debug.ra,
         lck->debug.rank, lck->name, __builtin_return_address(0));
}
#endif // SPLOCK_DEBUG
