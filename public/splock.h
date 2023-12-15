/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

#ifndef _SPLOCK_H
#define _SPLOCK_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMK_MODULE
#define INCLUDE_ALLOW_DISTRIBUTE
#include "includeCheck.h"

#include "vm_asm.h"
#include "vmk_debug.h"
#include "vm_atomic.h"
#include "timer_dist.h"

/*
 * splock.h --
 *
 *   This is the header file for the spin lock routines.
 */

#ifdef VMX86_DEBUG
#define SPLOCK_DEBUG
#define SPLOCK_STATS TRUE
#else // ! VMX86_DEBUG
#ifdef VMX86_ENABLE_SPLOCK_STATS
#define SPLOCK_STATS TRUE
#endif // VMX86_ENABLE_SPLOCK_STATS
#endif // (!) VMX86_DEBUG

extern volatile Bool spInitialized;

#ifdef SPLOCK_STATS
extern Bool spLockStatsEnabled;
#define SPLOCK_STATS_ON spLockStatsEnabled
#else // ! SPLOCK_STATS
#define SPLOCK_STATS_ON FALSE
#endif // (!) SPLOCK_STATS

#ifndef ASM
#define ASM __asm__ __volatile__
#endif

#ifndef INLINE
#define INLINE inline
#endif

// min amd max time we delay in each iteration of a spin loop
#define SP_MIN_SPIN_DELAY       ((1 << 10)-1)
#define SP_MAX_SPIN_DELAY       ((1 << 16)-1)

// SP_RankFlags includes rank info and some flags
typedef int SP_RankFlags;
#define SP_FLAG_SKIPSTATS       (0x10000)
#define SP_RANK_MASK            (0xffff)

/*
 * lock ranks.
 * All locks are associated with a numeric rank.  While holding a lock of
 * rank r, only locks of rank >r can be acquired.
 * Exception: locks with rank SP_RANK_UNRANKED
 * Note: All IRQ locks are ranked higher than non-IRQ locks
 */
typedef int SP_Rank;

/*
 * recursive_flag indicates that an instace of lock L can be held while
 * acquiring another instance of lock L. It is assumed the caller knows
 * what they're doing.
 */
#define SP_RANK_RECURSIVE_FLAG  (0x8000)
#define SP_RANK_UNRANKED        (0xffff)
#define SP_RANK_NUMERIC_MASK    (SP_RANK_RECURSIVE_FLAG - 1) // the real rank

// special locks
#define SP_RANK_LOCK_STATS      (0x4000)
#define SP_RANK_LOG             (SP_RANK_LOCK_STATS - 1)
#define SP_RANK_LOG_EVENT       (SP_RANK_LOG - 1)
#define SP_RANK_BACKTRACE       (SP_RANK_LOG_EVENT - 1)
#define SP_RANK_VMKTAG          (SP_RANK_BACKTRACE - 1)

// to be used for IRQ locks that leafs, except for log/warning
#define SP_RANK_IRQ_LEAF        (SP_RANK_VMKTAG - 1)

// to be used for IRQ locks that depend on mem/timer locks
#define SP_RANK_IRQ_MEMTIMER    (0x3000)

// to be used for IRQ locks that depend on eventqueue/cpusched locks
#define SP_RANK_IRQ_BLOCK       (0x2000)

// to be used for IRQ locks that depend on proc lock
#define SP_RANK_IRQ_PROC        (0x1800)

// lowest possible rank for IRQ locks
#define SP_RANK_IRQ_LOWEST      (0x1000)

// Special rank for tokens used for doing a core dump.  Tokens are not
// accessed in interrupt context and can use non-IRQ locks.  However, in
// the single case of dumping core, SCSI_Dump will directly call a
// driver's interrupt handler in a non-interrupt context.  The interrupt
// handler may get a driver IRQ lock (allocated by vmk_spin_lock_init())
// and call SCSILinuxCmdDone(), which does a token operation.  So, we must
// rank dump tokens higher than SP_RANK_IRQ_LOWEST.
#define SP_RANK_DUMP_TOKEN     (SP_RANK_IRQ_LOWEST+1)

// highest possible rank for non-IRQ locks
// to be used for non-IRQ locks that don't call any other non-IRQ locks
#define SP_RANK_SEMAPHORE       (SP_RANK_IRQ_LOWEST - 1)
/* Leaf locks are ranked lower than spin locks protecting semaphores, so
 * that one can grab a semaphore, grab a leaf lock and then call
 * Semaphore_IsLocked() on the semaphore.
 */
#define SP_RANK_LEAF		(SP_RANK_SEMAPHORE - 1)
#define SP_RANK_BLOCK           (SP_RANK_LEAF)

/* Lowest possible rank for locks used by the SCSI module. Callers into the
 * SCSI module should use locks ranked lower than SP_RANK_SCSI_LOWEST
 */
#define SP_RANK_SCSI_LOWEST	(SP_RANK_LEAF - 0x20)
/* Lowest possible rank for locks used by FS Device Switch. Modules
 * operating above the device switch, and calling into the device switch
 * should use locks ranked lower than this
 */
#define SP_RANK_FDS_LOWEST	(SP_RANK_SCSI_LOWEST)
/* Lowest possible rank for locks used by VMK FS drivers. FSS and everyone
 * above should use locks ranked lower than this.
 */
#define SP_RANK_FSDRIVER_LOWEST	(SP_RANK_FDS_LOWEST - 0x20)

// lowest possible rank for non-IRQ locks
#define SP_RANK_LOWEST		(0x0001)


/*
 * Module specific lock ranks.
 */

// memsched.c
#define SP_RANK_MEMSCHED_STATE   (SP_RANK_IRQ_LEAF)
// buddy.c
#define SP_RANK_BUDDY_ALLOC      (SP_RANK_IRQ_LEAF)

#define SP_RANK_BUDDY_HOTADD     (SP_RANK_BUDDY_ALLOC - 1)

// memmap.c
/*
 * In order to do the following 2 steps atomically
 * o hot add a range of memory
 * o update the free page counters 
 * we hold the memmap lock when we do HotAdd
 */
#define SP_RANK_MEMMAP          (SP_RANK_BUDDY_HOTADD - 1)

// sched.c tree lock
#define	SP_RANK_IRQ_SCHED_TREE	(0x2900)

// cpusched.c lock range
#define	SP_RANK_IRQ_CPUSCHED_HI	(0x28ff)
#define	SP_RANK_IRQ_CPUSCHED_LO	(0x2800)

// memsched.c
#define SP_RANK_MEMSCHED        (SP_RANK_LEAF)
// memmap.c
#define SP_RANK_HOTMEMADD       (SP_RANK_LEAF)
// async_io.c
#define SP_RANK_ASYNC_TOKEN     (SP_RANK_BLOCK)
// swap.c 
#define SP_RANK_FREESLOTS       (SP_RANK_BLOCK)
#define SP_RANK_SWAPASYNCIO     (SP_RANK_BLOCK)
#define SP_RANK_SWAPINFO        (SP_RANK_BLOCK)

// numasched.c
#define SP_RANK_NUMASCHED       (SP_RANK_MEMSCHED - 1)
// swap.c
#define SP_RANK_FILEMAP         (SP_RANK_ASYNC_TOKEN - 1)

// alloc.c
#define SP_RANK_ALLOC           (SP_RANK_FILEMAP - 1)

// swap.c
#define SP_RANK_SWAP            (SP_RANK_ALLOC - 1)

/*
 *  IRQL defines the level of interrupt masking. All interupts
 *  are ordered by priority. When you grab a lock you may want to
 *  continue to service some high priority interrupts. You must be
 *  careful, if an interrupt service routine grabs a spin lock then
 *  all acquitions of that lock must be done with at least an IRQL
 *  that would disable that interrupt. Unless you have some good
 *  reason you should use SP_IRQL_KERNEL.
 *
 *  For now the only level of interrupt masking is to enable/disable
 *  all interrupts. Please do not rely on this is as I'm sure we are
 *  going to change IRQL from a boolean to a interrupt mask. I say again,
 *  do not rely on the bits of IRQL, use the defined constants. - devine
 */

#define SP_IRQL_NONE       0
#define SP_IRQL_KERNEL     1

typedef int SP_IRQL;

#define SPINLOCK_NAME_SIZE 19
#define SPLOCK_INIT_MAGIC  0xa8d46f9c

/*
 * Common lock statistics fields
 */
#ifdef SPLOCK_STATS
typedef struct SP_Stats {
   TSCCycles                waitCycles;        // cycles spent waiting for the lock
   TSCCycles                lockedWhen;        // TSC when locked
   TSCCycles                lockedCycles;      // cycles lock was held
   TSCCycles                irqDisabledWhen;   // TSC when IRQ disabled
   TSCCycles                irqDisabledCycles; // cycles this lock kept irqs disabled
   uint32                   uncontendedAcq;    // nonblocking acquisitions
   uint32                   contendedAcq;      // blocking acquisitions
   uint32                   failedAcq;         // failing acquisitions (from trylocks)
   void                     *statsNext;        // link to next
   uint32                   statsNextRA;       // ra statsNext lock (to debug PR22342)
   Bool                     skipStats;         // don't do stats for this lock
} SP_Stats;
#endif // SPLOCK_STATS


#ifdef SPLOCK_DEBUG

/*
 * Spinlock debug fields
 */
typedef struct SP_SpinDebug {
   uint32                initMagic;   // set to SPLOCK_INIT_MAGIC by SP_InitLock{IRQ} 
   uint32                ra;          // return address of lock/unlock routine
   uint16                lastCPU;     // CPU that last acquired or released this lock
   uint16                holderCPU;   // CPU that currently holds the lock or -1
                                      //   lastCPU cannot be used because
                                      //   there is a race condition since lock bit 
                                      //   and lastCPU are not updated atomically
                                      //   We should look at storing
                                      //   current PCPU or worldID in lock field
   void                  *world;      // world that last acquired or released this lock
   SP_Rank               rank;        // lock's rank
} SP_SpinDebug;

/*
 * Reader/writer debug fields
 */
typedef struct SP_RWDebug {
   uint32                    initMagic;        // set to SPLOCK_INIT_MAGIC by SP_InitRWLock{IRQ} 
#define SP_RDLOCK_DBG_HISTORY 6
   TSCCycles                 tsLock[SP_RDLOCK_DBG_HISTORY];
   uint32                    raLock[SP_RDLOCK_DBG_HISTORY]; 
   uint32                    cpuLock[SP_RDLOCK_DBG_HISTORY]; 
   void                      *worldLock[SP_RDLOCK_DBG_HISTORY]; 
   TSCCycles                 tsUnlock[SP_RDLOCK_DBG_HISTORY];
   uint32                    raUnlock[SP_RDLOCK_DBG_HISTORY]; 
   uint32                    cpuUnlock[SP_RDLOCK_DBG_HISTORY]; 
   void                      *worldUnlock[SP_RDLOCK_DBG_HISTORY]; 
} SP_RWDebug;
#endif // SPLOCK_DEBUG

/*
 * Spinlock common fields
 */
typedef struct SP_SpinCommon {
   volatile uint32           lock;             // the lock (0 or PCPU+1)
   uint32                    delay;            // current backoff delay
   char name[SPINLOCK_NAME_SIZE + 1];          // lock name
   struct SP_RWCommon        *readerWriter;    // points to R/W parent
#ifdef SPLOCK_STATS
   SP_Stats                  stats;            // lock statistics
#endif // SPLOCK_STATS
#ifdef SPLOCK_DEBUG
   SP_SpinDebug              debug;            // debugging info
#endif // SPLOCK_DEBUG
} SP_SpinCommon;

/*
 * Reader/writer common fields
 */
typedef struct SP_RWCommon {
   Atomic_uint32           read;               // the lock
   uint32                  delay;              // current backoff delay
#ifdef SPLOCK_STATS
   SP_Stats                stats;              // lock statistics
#endif // SPLOCK_STATS
#ifdef SPLOCK_DEBUG
   SP_RWDebug              debug;              // debugging info
#endif // SPLOCK_DEBUG
} SP_RWCommon;

/*
 *  Basic spin lock definitions.
 *
 *     SP_SpinLockIRQ     - Disables all interrupts after acquisition
 *                          Note: SP_RWLockGeneric assumes that the first
 *                          field is SP_SpinCommon.
 *
 *     SP_SpinLock        - Interrupts remain unchanged after acquisition
 *
 */
typedef struct SP_SpinLockIRQ {
   SP_SpinCommon           common;             // common data
   SP_IRQL                 prevIRQL;           // previous IRQL
} SP_SpinLockIRQ;

typedef struct SP_SpinLock {
   SP_SpinCommon           common;             // common data
} SP_SpinLock;

/*
 * Reader/writer locks. 
 *
 *     SP_RWLockIRQ     - Disables all interrupts after acquisition
 *
 *     SP_RWLock        - Interrupts remain unchanged after acquisition
 */
typedef struct SP_RWLockIRQ {
   SP_RWCommon             common;             // common data
   SP_SpinLockIRQ          write;              // write lock
} SP_RWLockIRQ;

typedef struct SP_RWLock {
   SP_RWCommon             common;             // common data
   SP_SpinLock             write;              // write lock
} SP_RWLock;

/*
 * Spin barriers
 */
typedef struct SP_Barrier {
   SP_SpinLockIRQ          lock;               // the lock
   uint32                  members;            // number of members
   volatile Bool           smashed;            // no longer block anyone
   volatile Bool           sense;              // barrier sense
   volatile uint16         count;              // current member count
} SP_Barrier;


/*
 * SP_STACK_MAX_LOCKS is the maximum number of spin locks a single CPU can
 * acquire.  This is used for tracking lock ranks.
 * 32 should be more than enough since vmkernel stack is sized (12K) to
 * accomodate around 60 frames (assuming 200 bytes per frame), and
 * expecting about half of those frames to acquire locks in extreme case.
 */
#define SP_STACK_MAX_LOCKS 32

typedef struct SP_Stack {
   uint32 nLocks;
   struct SP_SpinCommon *locks[SP_STACK_MAX_LOCKS];
} SP_Stack;

typedef enum SP_StackType {
   SP_STACK_NON_IRQ_STACK,
   SP_STACK_IRQ_STACK,
   SP_STACK_NUM_STACKS
} SP_StackType;


/*
 * Prototypes
 */
EXTERN void SP_EarlyInit(void);
EXTERN void SP_Init(void);
EXTERN void SP_LateInit(void);

EXTERN void SP_InitLockCommon(char *name, SP_SpinCommon *lck, SP_RankFlags rankFlags);
EXTERN void SP_InitRWLockCommon(SP_RWCommon *rwl);
EXTERN void SP_CleanupLockCommon(SP_SpinCommon *lck);

EXTERN SP_IRQL SP_TryLockIRQ(SP_SpinLockIRQ *lck, SP_IRQL irql, Bool *acq);
EXTERN Bool SP_TryLock(SP_SpinLock *lck);

EXTERN void SP_WaitLockIRQ(SP_SpinLockIRQ *lck, uint32 ifEnabled);
EXTERN void SP_WaitLock(SP_SpinLock *lck);
EXTERN void SP_WaitReadLock(SP_RWLock *lck);

EXTERN void SP_InitBarrier(char *name, uint32 members, SP_Barrier *barrier);
EXTERN void SP_CleanupBarrier(SP_Barrier *barrier);
EXTERN void SP_SpinBarrier(SP_Barrier *barrier);
EXTERN void SP_SpinBarrierNoYield(SP_Barrier *barrier);
EXTERN void SP_SmashBarrier(SP_Barrier *barrier);

#ifdef SPLOCK_DEBUG
EXTERN void SPDebugLocked(SP_SpinCommon *lck, Bool IRQ);
EXTERN void SPDebugUnlocked(SP_SpinCommon *lck, Bool IRQ, Bool special);
EXTERN void SPDebugAcqReadLock(SP_RWCommon *rwl, Bool IRQ, SP_SpinCommon *lck);
EXTERN void SPDebugRelReadLock(SP_RWCommon *rwl, Bool IRQ, 
                               SP_SpinCommon *lck, Bool special);
EXTERN void SPCheckRank(SP_SpinCommon *lck);
EXTERN void SP_AssertNoLocksHeld(void);
EXTERN void SP_AssertNoIRQLocksHeld(void);
EXTERN void SP_AssertOneLockHeld(const SP_SpinLock *lock);
EXTERN void SP_AssertOneLockHeldIRQ(const SP_SpinLockIRQ *lockIRQ);
#else
static INLINE void SPDebugLocked(SP_SpinCommon *lck, Bool IRQ) { }
static INLINE void SPDebugUnlocked(SP_SpinCommon *lck, Bool IRQ, Bool special) { }
static INLINE void SPDebugAcqReadLock(SP_RWCommon *rwl, Bool IRQ, SP_SpinCommon *lck) { }
static INLINE void SPDebugRelReadLock(SP_RWCommon *rwl, Bool IRQ, 
                                      SP_SpinCommon *lck, Bool special) { }
static INLINE void SPCheckRank(SP_SpinCommon *lck) { }
static INLINE void SP_AssertNoLocksHeld(void) { }
static INLINE void SP_AssertNoIRQLocksHeld(void) { }
static INLINE void SP_AssertOneLockHeld(const SP_SpinLock* l) { }
static INLINE void SP_AssertOneLockHeldIRQ(const SP_SpinLockIRQ* l) { }
#endif // SPLOCK_DEBUG

EXTERN void *SP_GetLockAddrIRQ(SP_SpinLockIRQ *lck);
EXTERN void *SP_GetLockAddr(SP_SpinLock *lck);

uint32 PRDA_GetPCPUNumSafe(void);
/*
 *----------------------------------------------------------------------
 *
 * SP_InitLockIRQ --
 *
 *      Initialize a spin lock which disables interrupts.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Calls SP_InitLockCommon().
 *
 *----------------------------------------------------------------------
 */
static INLINE void
SP_InitLockIRQ(char *name, SP_SpinLockIRQ *lck, SP_RankFlags rankFlags)
{
   ASSERT(((rankFlags & SP_RANK_NUMERIC_MASK) >= SP_RANK_IRQ_LOWEST) ||
          ((rankFlags & SP_RANK_MASK) == SP_RANK_UNRANKED));
   SP_InitLockCommon(name, &lck->common, rankFlags);
   lck->prevIRQL = SP_IRQL_NONE;
}


/*
 *----------------------------------------------------------------------
 *
 * SP_InitLock --
 *
 *      Initialize a spin lock.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Calls SP_InitLockCommon().
 *
 *----------------------------------------------------------------------
 */
static INLINE void
SP_InitLock(char *name, SP_SpinLock *lck, SP_RankFlags rankFlags)
{
   ASSERT(((rankFlags & SP_RANK_NUMERIC_MASK) <= SP_RANK_DUMP_TOKEN) ||
          ((rankFlags & SP_RANK_MASK) == SP_RANK_UNRANKED));

   SP_InitLockCommon(name, &lck->common, rankFlags);
}


/*
 *----------------------------------------------------------------------
 *
 * SP_InitRWLockIRQ --
 *
 *      Initialize a reader/writer lock which disables interrupts.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Calls SP_InitRWLockCommon().
 *
 *----------------------------------------------------------------------
 */
static INLINE void
SP_InitRWLockIRQ(char *name, SP_RWLockIRQ *rwl, SP_RankFlags rankFlags)
{
   SP_InitRWLockCommon(&rwl->common);
   SP_InitLockIRQ(name, &rwl->write, rankFlags);
   rwl->write.common.readerWriter = &rwl->common;
}

/*
 *----------------------------------------------------------------------
 *
 * SP_InitRWLock --
 *
 *      Initialize a reader/writer lock.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Calls SP_InitRWLockCommon().
 *
 *----------------------------------------------------------------------
 */
static INLINE void
SP_InitRWLock(char *name, SP_RWLock *rwl, SP_RankFlags rankFlags)
{
   SP_InitRWLockCommon(&rwl->common);
   SP_InitLock(name, &rwl->write, rankFlags);
   rwl->write.common.readerWriter = &rwl->common;
}


/*
 *----------------------------------------------------------------------
 *
 * SP_CleanupLockIRQ --
 *
 *      Cleanup a spin lock which disables interrupts.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Calls SP_CleanupLockCommon().
 *
 *----------------------------------------------------------------------
 */
static INLINE void
SP_CleanupLockIRQ(SP_SpinLockIRQ *lck)
{
   SP_CleanupLockCommon(&lck->common);
}


/*
 *----------------------------------------------------------------------
 *
 * SP_CleanupLock --
 *
 *      Cleanup a spin lock.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Calls SP_CleanupLockCommon().
 *
 *----------------------------------------------------------------------
 */
static INLINE void
SP_CleanupLock(SP_SpinLock *lck)
{
   SP_CleanupLockCommon(&lck->common);
}


/*
 *----------------------------------------------------------------------
 *
 * SP_IsLocked{IRQ} --
 *
 *      Return TRUE if spin lock is locked.
 *
 * Results:
 *      TRUE if the lock is locked, FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
SP_IsLockedCommon(const SP_SpinCommon *commonLock)
{
   if (commonLock->lock == 0) {
      return FALSE;
   } else {
#ifdef SPLOCK_DEBUG
      if (commonLock->debug.holderCPU == PRDA_GetPCPUNumSafe()) {
         return TRUE;
      } else {
         return FALSE;
      }
#else
      return TRUE;
#endif
   }
}

static INLINE Bool
SP_IsLockedIRQ(const SP_SpinLockIRQ *lck)
{
   return SP_IsLockedCommon(&lck->common);
}


static INLINE Bool
SP_IsLocked(const SP_SpinLock *lck)
{
   return SP_IsLockedCommon(&lck->common);
}

/*
 *----------------------------------------------------------------------
 *
 * SP_GetPrevIRQ --
 *
 *      Get a previous interrupt level for a spin lock.
 *
 * Results:
 *      Previous IRQL.
 *
 * Side effects:
 *      none
 *
 *----------------------------------------------------------------------
 */
static INLINE SP_IRQL
SP_GetPrevIRQ(SP_SpinLockIRQ *lck)
{
   ASSERT(SP_IsLockedIRQ(lck));

   return lck->prevIRQL;
}


/*
 *----------------------------------------------------------------------
 *
 * SP_GetPrevWriteIRQ --
 *
 *      Get a previous interrupt level for a writer lock.
 *
 * Results:
 *      Previous IRQL.
 *
 * Side effects:
 *      none
 *
 *----------------------------------------------------------------------
 */
static INLINE SP_IRQL
SP_GetPrevWriteIRQ(SP_RWLockIRQ *lck)
{
   return SP_GetPrevIRQ(&lck->write);
}


/*
 *----------------------------------------------------------------------
 * SPTestAndSet --
 *    Perform atomic test-and-set on location, returning old contents.
 *    This is a module-private work function. Do not invoke directly.
 *----------------------------------------------------------------------
 */
static INLINE uint32
SPTestAndSet(volatile uint32 *location)
{
   uint32 res = 0;
   ASM(
       "lock ; btsl $0, (%1)"  "\n\t"
       "adcl $0, %0"           "\n"
       : "=rm" (res)
       : "r" (location),
         "0" (res)
       : "memory", "cc"
   );
   return res;
}


/*
 *----------------------------------------------------------------------
 *
 * SP_LockIRQ --
 *
 *      Disable interrupts and lock a spin lock
 *
 * Results:
 *      The previous IRQL is returned.
 *
 * Side effects:
 *      Interrupts may be disabled, if not already.
 *      If active, lock stats are updated.
 *
 *----------------------------------------------------------------------
 */
static INLINE SP_IRQL
SP_LockIRQ(SP_SpinLockIRQ *lck, UNUSED_PARAM(SP_IRQL irql))
{
   uint32 eflags;
   SP_IRQL prevIRQL;
#ifdef SPLOCK_STATS
   Bool contended = FALSE;
#endif // SPLOCK_STATS
   // ASSERT(irql == SP_IRQL_KERNEL);

   SPCheckRank(&lck->common);

   SAVE_FLAGS(eflags);
   if (eflags & EFLAGS_IF) {
      prevIRQL = SP_IRQL_NONE;
      CLEAR_INTERRUPTS();
   } else {
      prevIRQL = SP_IRQL_KERNEL;
   }

   if (SPTestAndSet(&lck->common.lock) != 0) {
#ifdef SPLOCK_STATS
      contended = TRUE;
#endif // SPLOCK_STATS
      SP_WaitLockIRQ(lck, (eflags & EFLAGS_IF));
   }

#ifdef SPLOCK_STATS
   if (SPLOCK_STATS_ON) {
      if (contended) {
         lck->common.stats.contendedAcq++;
      } else {
         lck->common.stats.uncontendedAcq++;
      }
      lck->common.stats.lockedWhen = RDTSC();
      if (eflags & EFLAGS_IF) {
         lck->common.stats.irqDisabledWhen = lck->common.stats.lockedWhen;
      } else {
          lck->common.stats.irqDisabledWhen = 0LL;
      }
   }
#endif // SPLOCK_STATS

   lck->prevIRQL = prevIRQL;

   SPDebugLocked(&lck->common, TRUE);

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
 * SP_RestoreIRQ --
 *
 *      restore the irql to prevIRQL
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Interrupts may be enabled.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
SP_RestoreIRQ(SP_IRQL prevIRQL)
{
   if (prevIRQL == SP_IRQL_NONE) {
      ENABLE_INTERRUPTS();
   } else {
      ASSERT(prevIRQL == SP_IRQL_KERNEL);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * SPDoUnlockIRQ --
 *
 *      Unlock a spin lock, and maybe enable interrupts.  If
 *      skipOrderCheck is TRUE, allow out-of-order unlocks in debug
 *      builds.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Interrupts may be enabled.
 *      If active, lock stats are updated.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
SPDoUnlockIRQ(SP_SpinLockIRQ* lck, SP_IRQL prevIRQL, Bool skipOrderCheck)
{
   /*
    * DON'T put anything before this barrier.  It's here to prevent
    * the compiler from reording the unlock code before the code that's
    * inside the locked region (PR 28372).
    */
   COMPILER_MEM_BARRIER();

   ASSERT(lck->common.lock);
   SPDebugUnlocked(&lck->common, TRUE, skipOrderCheck);

#ifdef SPLOCK_STATS
   if (SPLOCK_STATS_ON) {
      TSCCycles now = RDTSC();
      lck->common.stats.lockedCycles += now - lck->common.stats.lockedWhen;
      if (lck->common.stats.irqDisabledWhen != 0LL) {
          lck->common.stats.irqDisabledCycles +=
                                   now - lck->common.stats.irqDisabledWhen;
      }
   }
#endif // SPLOCK_STATS
   lck->common.lock = 0;
   SP_RestoreIRQ(prevIRQL);
}   


/*
 *----------------------------------------------------------------------
 *
 * SP_UnlockIRQ --
 *
 *      Unlock a spin lock, and maybe enable interrupts
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Interrupts may be enabled.
 *      If active, lock stats are updated.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
SP_UnlockIRQ(SP_SpinLockIRQ *lck, SP_IRQL prevIRQL)
{
   SPDoUnlockIRQ(lck, prevIRQL, FALSE);
}


/*
 *----------------------------------------------------------------------
 *
 * SP_UnlockIRQSpecial --
 *
 *      Unlock a spin lock, and possibly enable interrupts.
 *      Similar to SP_UnlockIRQ except used by callers that release
 *      locks out of LIFO order, but are known to be safe.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Interrupts may be enabled.
 *      If active, lock stats are updated.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
SP_UnlockIRQSpecial(SP_SpinLockIRQ *lck, SP_IRQL prevIRQL)
{
   SPDoUnlockIRQ(lck, prevIRQL, TRUE);
}


/*
 *----------------------------------------------------------------------
 *
 * SP_Lock --
 *
 *      Lock a spin lock.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      If active, lock stats are updated.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
SP_Lock(SP_SpinLock *lck)
{
#ifdef SPLOCK_STATS
   Bool contended = FALSE;
#endif // SPLOCK_STATS

   SPCheckRank(&lck->common);

   if (SPTestAndSet(&lck->common.lock) != 0) {
#ifdef SPLOCK_STATS
      contended = TRUE;
#endif // SPLOCK_STATS
      SP_WaitLock(lck);
   }

#ifdef SPLOCK_STATS
   if (SPLOCK_STATS_ON) {
      if (contended) {
         lck->common.stats.contendedAcq++;
      } else {
         lck->common.stats.uncontendedAcq++;
      }
      lck->common.stats.lockedWhen = RDTSC();
   }
#endif // SPLOCK_STATS

   SPDebugLocked(&lck->common, FALSE);

   /*
    * DON'T put anything after this barrier.  It's here to prevent the
    * compiler from reording the code in the lock routine after the code
    * inside the locked region (PR 28372).
    */
   COMPILER_MEM_BARRIER();
}


/*
 *----------------------------------------------------------------------
 *
 * SPDoUnlock --
 *
 *      Unlock a spin lock, skip out-of-order check if skipOrderCheck is
 *      TRUE.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      If active, lock stats are updated.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
SPDoUnlock(SP_SpinLock* lck, Bool skipOrderCheck)
{
   /*
    * DON'T put anything before this barrier.  It's here to prevent
    * the compiler from reording the unlock code before the code that's
    * inside the locked region (PR 28372).
    */
   COMPILER_MEM_BARRIER();

   ASSERT(lck->common.lock);
   SPDebugUnlocked(&lck->common, FALSE, skipOrderCheck);

#ifdef SPLOCK_STATS
   if (SPLOCK_STATS_ON) {
      lck->common.stats.lockedCycles += RDTSC() - lck->common.stats.lockedWhen;
   }
#endif // SPLOCK_STATS
   lck->common.lock = 0;
}   


/*
 *----------------------------------------------------------------------
 *
 * SP_Unlock --
 *
 *      Unlock a spin lock.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      If active, lock stats are updated.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
SP_Unlock(SP_SpinLock *lck)
{
   SPDoUnlock(lck, FALSE);
}


/*
 *----------------------------------------------------------------------
 *
 * SP_UnlockSpecial --
 *
 *      Unlock a spin lock. Similar to SP_Unlock except used by
 *      callers that release locks out of LIFO order, but are
 *      known to be safe.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      If active, lock stats are updated.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
SP_UnlockSpecial(SP_SpinLock *lck)
{
   SPDoUnlock(lck, TRUE);
}


/*
 *----------------------------------------------------------------------
 *
 * SP_CleanupRWLock{IRQ} --
 *
 *      Clean up a reader/writer spin lock.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The write lock is cleaned up.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
SP_CleanupRWLockIRQ(SP_RWLockIRQ *rwl)
{
   SP_CleanupLockIRQ(&rwl->write);
}

static INLINE void
SP_CleanupRWLock(SP_RWLock *rwl)
{
   SP_CleanupLock(&rwl->write);
}


/*
 *----------------------------------------------------------------------
 *
 * SP_HintWriteLocked{IRQ} --
 *
 *      Return true if this lock's writer count is > 0.
 *      This is roughly equivalent to the lock being write locked,
 *      and good enough for ASSERT()'ing that you hold the lock.
 *      It is not authoritative.
 *
 * Results:
 *      TRUE if the lock is *probably* write locked, FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
SP_HintWriteLockedIRQ(SP_RWLockIRQ *lck)
{
   return (lck->write.common.lock != 0);
}

static INLINE Bool
SP_HintWriteLocked(SP_RWLock *lck)
{
   return (lck->write.common.lock != 0);
}


/*
 *----------------------------------------------------------------------
 *
 * SP_HintReadLocked{IRQ} --
 *
 *      Return true if this lock's reader count is > 0.
 *      This is roughly equivalent to the lock being read locked,
 *      and good enough for ASSERT()'ing that you hold the lock.
 *      It is not authoritative.
 *
 * Results:
 *      TRUE if the lock is *probably* read locked, FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
SP_HintReadLockedIRQ(SP_RWLockIRQ *rwl)
{
   return (Atomic_Read(&rwl->common.read) != 0);
}

static INLINE Bool
SP_HintReadLocked(SP_RWLock *rwl)
{
   return (Atomic_Read(&rwl->common.read) != 0);
}


/*
 *----------------------------------------------------------------------
 *
 * SP_AcqReadLock --
 *
 *      Grab a non-exclusive lock.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      If active, lock stats are updated.
 *
 *----------------------------------------------------------------------
 */
static INLINE void SP_AcqReadLock(SP_RWLock *rwl)
{
#ifdef SPLOCK_STATS
   Bool contended = FALSE;
   uint32 priorReaders = Atomic_FetchAndInc(&rwl->common.read);
#else // ! SPLOCK_STATS
   Atomic_FetchAndInc(&rwl->common.read);
#endif // (!) SPLOCK_STATS

   SPCheckRank(&rwl->write.common);

   if (rwl->write.common.lock) {
      // must wait for a writer
#ifdef SPLOCK_STATS
      contended = TRUE;
#endif // SPLOCK_STATS
      Atomic_Dec(&rwl->common.read);
      SP_WaitLock(&rwl->write);
      SPDebugLocked(&rwl->write.common, FALSE);

      Atomic_Inc(&rwl->common.read);
      SP_Unlock(&rwl->write);
   }
#ifdef SPLOCK_STATS
   if (SPLOCK_STATS_ON) {
      // only adjust time related things if there were no prior readers
      if (priorReaders == 0) {
         rwl->common.stats.lockedWhen = RDTSC();
      }
      if (contended) {
         rwl->common.stats.contendedAcq++;
      } else {
         rwl->common.stats.uncontendedAcq++;
      }
   }
#endif // SPLOCK_STATS

   SPDebugAcqReadLock(&rwl->common, FALSE, &rwl->write.common);

   /*
    * DON'T put anything after this barrier.  It's here to prevent the
    * compiler from reording the code in the lock routine after the code
    * inside the locked region (PR 28372).
    */
   COMPILER_MEM_BARRIER();
}

/*
 *----------------------------------------------------------------------
 *
 * SP_TryReadLock --
 *
 *      Try to grab a non-exclusive lock.
 *
 * Results:
 *      Returns True if the lock was obtained, FALSE otherwise.
 *
 * Side effects:
 *      If active, lock stats are updated.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool SP_TryReadLock(SP_RWLock *rwl)
{
   Bool success = TRUE;
#ifdef SPLOCK_STATS
   uint32 priorReaders = Atomic_FetchAndInc(&rwl->common.read);
#else // ! SPLOCK_STATS
   Atomic_FetchAndInc(&rwl->common.read);
#endif // (!) SPLOCK_STATS

   if (rwl->write.common.lock) {
      Atomic_Dec(&rwl->common.read);
      success = FALSE;
   }
   if (success) {
      SPDebugAcqReadLock(&rwl->common, FALSE, &rwl->write.common);
   }

#ifdef SPLOCK_STATS
   if (SPLOCK_STATS_ON) {
      if (success) {
         rwl->common.stats.uncontendedAcq++;
         // only adjust time related things if there were no prior readers
         if (priorReaders == 0) {
            rwl->common.stats.lockedWhen = RDTSC();
         }
      } else {
         rwl->common.stats.failedAcq++; 
      }
   }
#endif // SPLOCK_STATS

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
 * SPDoRelReadLock --
 *
 *      Release a non-exclusive lock.  Will skip LIFO ordering checks if
 *      skipOrderCheck is TRUE.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      If active, lock stats are updated.
 *
 *----------------------------------------------------------------------
 */
static INLINE void 
SPDoRelReadLock(SP_RWLock *rwl, Bool skipOrderCheck)
{
   uint32 readers;
#ifdef SPLOCK_STATS
   uint64 lockedWhen;
#endif // SPLOCK_STATS

   /*
    * DON'T put anything before this barrier.  It's here to prevent
    * the compiler from reording the unlock code before the code that's
    * inside the locked region (PR 28372).
    */
   COMPILER_MEM_BARRIER();

#ifdef SPLOCK_STATS
   lockedWhen = rwl->common.stats.lockedWhen;
#endif // SPLOCK_STATS

   SPDebugRelReadLock(&rwl->common, FALSE, &rwl->write.common, skipOrderCheck);

   ASSERT(Atomic_Read(&rwl->common.read) > 0);
   readers = Atomic_FetchAndDec(&rwl->common.read);

#ifdef SPLOCK_STATS
   // only adjust things if we were the last reader
   if (SPLOCK_STATS_ON && (readers == 1)) {
      rwl->common.stats.lockedCycles += RDTSC() - lockedWhen;
   }
#endif // SPLOCK_STATS
}


/*
 *----------------------------------------------------------------------
 *
 * SP_RelReadLock --
 *
 *      Release a non-exclusive lock.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      If active, lock stats are updated.
 *
 *----------------------------------------------------------------------
 */
static INLINE void 
SP_RelReadLock(SP_RWLock *rwl)
{
   SPDoRelReadLock(rwl, FALSE);
}


/*
 *----------------------------------------------------------------------
 *
 * SP_RelReadLockSpecial --
 *
 *      Release a non-exclusive lock.  The "Special" suffix means that
 *      out-of-order unlocks are allowed (normally locks must be unlocked
 *      in the order acquired).
 *
 * Results:
 *      None
 *
 * Side effects:
 *      If active, lock stats are updated.
 *
 *----------------------------------------------------------------------
 */
static INLINE void 
SP_RelReadLockSpecial(SP_RWLock *rwl)
{
   SPDoRelReadLock(rwl, TRUE);
}


/*
 *----------------------------------------------------------------------
 *
 * SP_AcqWriteLock --
 *
 *      Grab an exclusive lock.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      If active, lock stats are updated.
 *
 *----------------------------------------------------------------------
 */
static INLINE void SP_AcqWriteLock(SP_RWLock *rwl)
{
   while (1) {
      // lock out new readers and writers
      SP_Lock(&rwl->write); 

      // wait for existing readers
      if (Atomic_Read(&rwl->common.read)) { 
         SP_Unlock(&rwl->write);
         SP_WaitReadLock(rwl);
      } else {
         break;
      }
   }

   /*
    * DON'T put anything after this barrier.  It's here to prevent the
    * compiler from reording the code in the lock routine after the code
    * inside the locked region (PR 28372).
    */
   COMPILER_MEM_BARRIER();
}

/*
 *----------------------------------------------------------------------
 *
 * SP_TryWriteLock --
 *
 *      Try to grab an exclusive lock.
 *
 * Results:
 *      Returns TRUE if the lock was obtained, FALSE otherwise.
 *
 * Side effects:
 *      If active, lock stats are updated.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool SP_TryWriteLock(SP_RWLock *rwl)
{
   // try to lock out new readers and writers
   if (!SP_TryLock(&rwl->write)) {
      return FALSE;
   }

   // check for existing readers
   if (Atomic_Read(&rwl->common.read)) { 
      SP_Unlock(&rwl->write); 
#ifdef SPLOCK_STATS
      if (SPLOCK_STATS_ON) {
         rwl->write.common.stats.uncontendedAcq--;
         rwl->write.common.stats.failedAcq++;
      }
#endif // SPLOCK_STATS
      return FALSE;
   } 

   /*
    * DON'T put anything after this barrier.  It's here to prevent the
    * compiler from reording the code in the lock routine after the code
    * inside the locked region (PR 28372).
    */
   COMPILER_MEM_BARRIER();

   return TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * SPDoRelWriteLock --
 *
 *      Release an exclusive lock.  Skip LIFO checks if skipOrderCheck is
 *      TRUE.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      The associated write lock is released.
 *
 *----------------------------------------------------------------------
 */
static INLINE void 
SPDoRelWriteLock(SP_RWLock *rwl, Bool skipOrderCheck)
{
   /*
    * DON'T put anything before this barrier.  It's here to prevent
    * the compiler from reording the unlock code before the code that's
    * inside the locked region (PR 28372).
    */
   COMPILER_MEM_BARRIER();

   SPDoUnlock(&rwl->write, skipOrderCheck);
}


/*
 *----------------------------------------------------------------------
 *
 * SP_RelWriteLock --
 *
 *      Release an exclusive lock.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      The associated write lock is released.
 *
 *----------------------------------------------------------------------
 */
static INLINE void 
SP_RelWriteLock(SP_RWLock *rwl)
{
   SPDoRelWriteLock(rwl, FALSE);
}


/*
 *----------------------------------------------------------------------
 *
 * SP_RelWriteLockSpecial --
 *
 *      Release an exclusive lock.  The "Special" suffix means that
 *      out-of-order unlocks are allowed (normally locks must be unlocked
 *      in the order acquired).
 *
 * Results:
 *      None
 *
 * Side effects:
 *      The associated write lock is released.
 *
 *----------------------------------------------------------------------
 */
static INLINE void 
SP_RelWriteLockSpecial(SP_RWLock *rwl)
{
   SPDoRelWriteLock(rwl, TRUE);
}


/*
 *----------------------------------------------------------------------
 *
 * SP_AcqReadLockIRQ --
 *
 *      Disable interrupts and grab a non-exclusive lock.
 *
 * Results:
 *      The previous IRQL is returned
 *
 * Side effects:
 *      Interrupts may be disabled, if not already.
 *
 *----------------------------------------------------------------------
 */
static INLINE SP_IRQL
SP_AcqReadLockIRQ(SP_RWLockIRQ *rwl, UNUSED_PARAM(SP_IRQL irql))
{
   uint32 eflags;
   SP_IRQL prevIRQL;
   uint32 priorReaders;
#ifdef SPLOCK_STATS
   Bool contended = FALSE;
#endif // SPLOCK_STATS

   SPCheckRank(&rwl->write.common);

   SAVE_FLAGS(eflags);
   if (eflags & EFLAGS_IF) {
      prevIRQL = SP_IRQL_NONE;
      CLEAR_INTERRUPTS();
   } else {
      prevIRQL = SP_IRQL_KERNEL;
   }

   priorReaders = Atomic_FetchAndInc(&rwl->common.read);

   if (rwl->write.common.lock) {
      // must wait for a writer
#ifdef SPLOCK_STATS
      contended = TRUE;
#endif // SPLOCK_STATS
      Atomic_Dec(&rwl->common.read);
      SP_WaitLockIRQ(&rwl->write, (eflags & EFLAGS_IF));
      SPDebugLocked(&rwl->write.common, TRUE);

      priorReaders = Atomic_FetchAndInc(&rwl->common.read);
      SP_UnlockIRQ(&rwl->write, SP_IRQL_KERNEL);
   }

#ifdef SPLOCK_STATS
   if (SPLOCK_STATS_ON) {
      // only adjust time related things if there were no prior readers
      if (priorReaders == 0) {
         rwl->common.stats.lockedWhen = RDTSC();
         if (eflags & EFLAGS_IF) {
            rwl->common.stats.irqDisabledWhen = rwl->common.stats.lockedWhen;
         } else {
            rwl->common.stats.irqDisabledWhen = 0LL;
         }
      }
      if (contended) {
         rwl->common.stats.contendedAcq++;
      } else {
         rwl->common.stats.uncontendedAcq++;
      }
   }
#endif // SPLOCK_STATS

   SPDebugAcqReadLock(&rwl->common, TRUE, &rwl->write.common);

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
 * SP_TryReadLockIRQ --
 *
 *      Disable interrupts and try to grab a non-exclusive lock.
 *
 * Results:
 *      The previous IRQL is returned and acquired is set to indicate
 *      success or failure.
 *
 * Side effects:
 *      Interrupts may be disabled, if not already.
 *
 *----------------------------------------------------------------------
 */
static INLINE SP_IRQL
SP_TryReadLockIRQ(SP_RWLockIRQ *rwl,
                  UNUSED_PARAM(SP_IRQL irql),
                  Bool *acquired)
{
   uint32 priorReaders;
   uint32 eflags;
   SP_IRQL prevIRQL;

   SAVE_FLAGS(eflags);
   if (eflags & EFLAGS_IF) {
      prevIRQL = SP_IRQL_NONE;
      CLEAR_INTERRUPTS();
   } else {
      prevIRQL = SP_IRQL_KERNEL;
   }

   priorReaders = Atomic_FetchAndInc(&rwl->common.read);
   if (rwl->write.common.lock) {
      Atomic_Dec(&rwl->common.read);
      SP_RestoreIRQ(prevIRQL);
      *acquired = FALSE;
   } else {
      *acquired = TRUE;
   }

#ifdef SPLOCK_STATS
   if (SPLOCK_STATS_ON) {
      if (*acquired) {
         // only adjust time related things if there were no prior readers
         if (priorReaders == 0) {
            rwl->common.stats.lockedWhen = RDTSC();
            if (eflags & EFLAGS_IF) {
               rwl->common.stats.irqDisabledWhen = rwl->common.stats.lockedWhen;
            } else {
               rwl->common.stats.irqDisabledWhen = 0LL;
            }
         }
         rwl->common.stats.uncontendedAcq++;
      } else {
        rwl->common.stats.failedAcq++;
      }
   }
#endif // SPLOCK_STATS

   SPDebugAcqReadLock(&rwl->common, TRUE, &rwl->write.common);

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
 * SP_RelReadLockIRQ --
 *
 *      Release a non-exclusive lock, and maybe enable interrupts.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      Interrupts may be enabled.
 *
 *----------------------------------------------------------------------
 */
static INLINE void SP_RelReadLockIRQ(SP_RWLockIRQ *rwl, SP_IRQL prevIRQL)
{
   uint32 readers;
#ifdef SPLOCK_STATS
   uint64 lockedWhen;
#endif // SPLOCK_STATS

   /*
    * DON'T put anything before this barrier.  It's here to prevent
    * the compiler from reording the unlock code before the code that's
    * inside the locked region (PR 28372).
    */
   COMPILER_MEM_BARRIER();

   SPDebugRelReadLock(&rwl->common, TRUE, &rwl->write.common, FALSE);
#ifdef SPLOCK_STATS
   lockedWhen = rwl->common.stats.lockedWhen;
#endif // SPLOCK_STATS

   ASSERT(Atomic_Read(&rwl->common.read) > 0);
   readers = Atomic_FetchAndDec(&rwl->common.read);
   SP_RestoreIRQ(prevIRQL);

#ifdef SPLOCK_STATS
   // only adjust things if we were the last reader
   if (SPLOCK_STATS_ON && (readers == 1)) {
      rwl->common.stats.lockedCycles += RDTSC() - lockedWhen;
   }
#endif // SPLOCK_STATS

}

/*
 *----------------------------------------------------------------------
 *
 * SP_AcqWriteLockIRQ --
 *
 *      Disable interrupts and grab an exclusive lock.
 *
 * Results:
 *      The previous IRQL is returned
 *
 * Side effects:
 *      Interrupts may be enabled.
 *
 *----------------------------------------------------------------------
 */
static INLINE SP_IRQL SP_AcqWriteLockIRQ(SP_RWLockIRQ *rwl, SP_IRQL irql)
{
   // lock out new readers and writers
   SP_IRQL prevIRQL = SP_LockIRQ(&rwl->write, irql); 

   // wait for existing readers
   // Note: cast is okay because we're only
   // touching the reader part of the lock
   if (Atomic_Read(&rwl->common.read)) {
      SP_WaitReadLock((SP_RWLock *)rwl);
   } 

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
 * SP_TryWriteLockIRQ --
 *
 *      Disable interrupts and try to grab an exclusive lock.
 *
 * Results:
 *      The previous IRQL is returned and acquired is set to
 *      indicate whether the lock was aquired or not.
 *
 * Side effects:
 *      Interrupts may be disabled.
 *
 *----------------------------------------------------------------------
 */
static INLINE SP_IRQL SP_TryWriteLockIRQ(SP_RWLockIRQ *rwl, SP_IRQL irql, 
                                         Bool *acquired)
{
   // try to lock out new readers and writers
   SP_IRQL prevIRQL = SP_TryLockIRQ(&rwl->write, irql, acquired); 
   if (*acquired) {
      // check for existing readers
      if (Atomic_Read(&rwl->common.read)) {
         SP_UnlockIRQ(&rwl->write, prevIRQL);
#ifdef SPLOCK_STATS
         if (SPLOCK_STATS_ON) {
            rwl->write.common.stats.uncontendedAcq--;
            rwl->write.common.stats.failedAcq++;
         }
#endif // SPLOCK_STATS
         *acquired = FALSE;
      } 
   }

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
 * SP_RelWriteLockIRQ --
 *
 *      Release an exclusive lock and maybe enable interrupts.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      Interrupts may be enabled.
 *
 *----------------------------------------------------------------------
 */
static INLINE void SP_RelWriteLockIRQ(SP_RWLockIRQ *rwl, SP_IRQL prevIRQL)
{
   /*
    * DON'T put anything before this barrier.  It's here to prevent
    * the compiler from reording the unlock code before the code that's
    * inside the locked region (PR 28372).
    */
   COMPILER_MEM_BARRIER();

   SP_UnlockIRQ(&rwl->write, prevIRQL);
}

/*
 *----------------------------------------------------------------------
 *
 * SP_GetLockRA --
 *
 *      Return the saved return address from a lock. This only makes
 *      sense when the lock is held and is only used for debugging to
 *      assert fail if a lock is dropped and regrabbed in a protected
 *      region.
 *
 * Results:
 *      The return address from the call where the lock was grabbed
 *      for debug builds and 0 for release builds.
 *
 * Side effects:
 *      none
 *
 *----------------------------------------------------------------------
 */
static INLINE uint32 SP_GetLockRA(SP_SpinLock *lck)
{
#ifdef SPLOCK_DEBUG
   ASSERT(spInitialized);
   ASSERT(lck->common.debug.initMagic == SPLOCK_INIT_MAGIC);
   return (lck->common.debug.ra);
#else
   return 0;
#endif
}

/*
 *----------------------------------------------------------------------
 *
 * SP_GetLockName --
 *
 *      Return the name of the lock -- used for debugging
 *
 * Results:
 *      pointer to lock name
 *
 * Side effects:
 *      none
 *
 *----------------------------------------------------------------------
 */
static INLINE char *SP_GetLockName(SP_SpinLock *lck)
{
   return lck->common.name;
}

#endif // _SPLOCK_H

