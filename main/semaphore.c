/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

#include "vm_types.h"
#include "x86.h"
#include "vmkernel.h"
#include "world.h"
#include "sched.h"
#include "semaphore_ext.h"
#include "semaphore.h"

/*
 *----------------------------------------------------------------------
 *
 * Semaphore_Init --
 *
 *      Set the count on a semaphore.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The count on the semaphore is set.
 *
 *----------------------------------------------------------------------
 */
void
Semaphore_Init(char *name, Semaphore *sema, int32 count, SemaRank rank)
{
   sema->count = count;
   sema->waiters = 0;
   // only rank check binary semaphores
   if (count == 1) {
      ASSERT((rank == SEMA_RANK_UNRANKED) || 
             ((rank <= SEMA_RANK_MAX) &&
              (rank >= SEMA_RANK_MIN)));
      sema->rank = rank;
   } else {
      ASSERT(rank == SEMA_RANK_UNRANKED);
      sema->rank = SEMA_RANK_UNRANKED;
   }
   List_InitElement(&sema->nextHeldSema);
   SP_InitLock(name, &sema->lock, SP_RANK_SEMAPHORE);
}

/*
 *----------------------------------------------------------------------
 *
 * Semaphore_Cleanup --
 *
 *      Cleanup a semaphore.
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
Semaphore_Cleanup(Semaphore *sema)
{
   SP_CleanupLock(&sema->lock);
}

/*
 *----------------------------------------------------------------------
 *
 * Semaphore_Lock --
 *
 *      Decrement the semaphore and goto sleep if the count < 0.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The semaphore is decremented and the calling world may go
 *	to sleep.
 *
 *----------------------------------------------------------------------
 */
void
Semaphore_Lock(Semaphore *sema)
{
   /* vmkernel unloading is done in the content of the host world, and
    * unregister_chrdev() uses a semaphore (which will never block).  So,
    * we don't do assert World_IsSafeToBlock() if vmkernelLoaded is FALSE. */
   ASSERT(!vmkernelLoaded || World_IsSafeToBlock());

   // check rank of new semaphore to see if we're allowed to acquire it
   if (vmx86_debug &&
       (sema->rank != SEMA_RANK_UNRANKED) &&
       !List_IsEmpty(&MY_RUNNING_WORLD->heldSemaphores)) {
      Semaphore *lastSema = (Semaphore*)List_First(&MY_RUNNING_WORLD->heldSemaphores);
      if (sema->rank <= lastSema->rank) {
         Panic("rank violation: holding %x:%s while locking %x:%s",
               lastSema->rank, SP_GetLockName(&lastSema->lock),
               sema->rank, SP_GetLockName(&sema->lock));
      }
      ASSERT(sema->rank > lastSema->rank);
   }

   SP_Lock(&sema->lock);
   sema->waiters++;

   while (sema->count <= 0) {
      CpuSched_Wait((uint32)sema, CPUSCHED_WAIT_LOCK, &sema->lock);
      SP_Lock(&sema->lock);
   }
   sema->waiters--;
   sema->count--;

   if (vmx86_debug && sema->rank != SEMA_RANK_UNRANKED) {
      List_Insert(&sema->nextHeldSema, LIST_ATFRONT(&MY_RUNNING_WORLD->heldSemaphores));
   }

   SP_Unlock(&sema->lock);
}

/*
 *----------------------------------------------------------------------
 *
 * Semaphore_Unlock --
 *
 *      Increment the semaphore and if it is >= 0 wakeup anyone waiting
 *	on it.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The semaphore is incremented.
 *
 *----------------------------------------------------------------------
 */
void
Semaphore_Unlock(Semaphore *sema)
{
   SP_Lock(&sema->lock);
   if (vmx86_debug && sema->rank != SEMA_RANK_UNRANKED) {
      /*
       * this assert checks semaphores are release in LIFO order, but
       * technically not needed for correctness.  If you hit this and have
       * a valid reason for non-LIFO release, we can do a "special" unlock
       * function or take out the assert.
       */
      ASSERT(List_First(&MY_RUNNING_WORLD->heldSemaphores) == &sema->nextHeldSema);
      List_Remove(&sema->nextHeldSema);
   }
   sema->count++;
   if (sema->waiters > 0) {
      CpuSched_Wakeup((uint32)sema);
   }
   SP_Unlock(&sema->lock);
}

/*
 *----------------------------------------------------------------------
 *
 * Semaphore_IsLocked --
 *
 *      Return TRUE if semaphore is currently locked
 *
 * Results:
 *      Return TRUE if semaphore is currently locked, FALSE otherwise
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
Bool
Semaphore_IsLocked(Semaphore *sema)
{
   Bool isLocked;

   SP_Lock(&sema->lock);
   if (sema->count <= 0) {
      isLocked = TRUE;
   } else {
      isLocked = FALSE;
   }
   SP_Unlock(&sema->lock);

   return isLocked;
}


void
Semaphore_RWInit(char *name, RWSemaphore *sema)
{
   sema->upgradeWaiter = FALSE;
   sema->exclusiveWaiters = sema->sharedWaiters = 0;
   sema->exclusiveAccess = sema->sharedAccess = 0;
   SP_InitLock(name, &sema->lock, SP_RANK_SEMAPHORE);
}

void
Semaphore_RWCleanup(RWSemaphore *sema)
{
   SP_CleanupLock(&sema->lock);
}

/*
 * Begin a read on a suspending reader-writer lock.
 */
void
Semaphore_BeginRead(RWSemaphore *sema)
{
   ASSERT(World_IsSafeToBlock());

   SP_Lock(&sema->lock);

   sema->sharedWaiters++;
   while (sema->exclusiveWaiters || sema->exclusiveAccess ||
          sema->upgradeWaiter) {
      CpuSched_Wait((uint32)&sema->sharedWaiters,
                    CPUSCHED_WAIT_RWLOCK,
                    &sema->lock);
      SP_Lock(&sema->lock);
   }
   sema->sharedWaiters--;
   ASSERT(sema->exclusiveAccess == 0);
   sema->sharedAccess++;

   SP_Unlock(&sema->lock);
}

/*
 * End a read on a suspending reader-writer lock.
 * If this is the last shared IO, wake up anyone requesting exclusive access.
 */
void
Semaphore_EndRead(RWSemaphore *sema)
{
   SP_Lock(&sema->lock);

   ASSERT(sema->exclusiveAccess == 0);
   sema->sharedAccess--;
   if ((sema->upgradeWaiter) && (sema ->sharedAccess == 1)) {
      CpuSched_Wakeup((uint32)&sema->upgradeWaiter);
   } else if ((sema->sharedAccess == 0) && (sema->exclusiveWaiters)) {
      CpuSched_Wakeup((uint32)&sema->exclusiveWaiters);
   }

   SP_Unlock(&sema->lock);
}

/*
 * Begin a write on a suspending reader-writer lock.
 */
void
Semaphore_BeginWrite(RWSemaphore *sema)
{
   ASSERT(World_IsSafeToBlock());

   SP_Lock(&sema->lock);
   sema->exclusiveWaiters++;
   while (sema->sharedAccess || sema->exclusiveAccess ||
          sema->upgradeWaiter) {
      CpuSched_Wait((uint32)&sema->exclusiveWaiters,
                    CPUSCHED_WAIT_RWLOCK,
                    &sema->lock);
      SP_Lock(&sema->lock);
   }
   sema->exclusiveWaiters--;
   ASSERT((sema->exclusiveAccess == 0) && (sema->sharedAccess == 0));
   sema->exclusiveAccess++;
   SP_Unlock(&sema->lock);
}

/*
 * End a write on a suspending reader-writer lock.
 * Wake up any exclusive or shared waiters (preference given to exclusive)
 */
void
Semaphore_EndWrite(RWSemaphore *sema)
{
   SP_Lock(&sema->lock);

   sema->exclusiveAccess--;
   /* If there was upgradeWaiter at Semaphore_BeginWrite(), it should
    * have been serviced first. Also, an upgradeWaiter couldn't have come
    * in while this exclusive writer was working, because an upgradeWaiter
    * would first need to spin as sharedWaiter
    */
   ASSERT(sema->upgradeWaiter == FALSE);
   ASSERT((sema->exclusiveAccess == 0) && (sema->sharedAccess == 0));
   if (sema->exclusiveWaiters) {
      CpuSched_Wakeup((uint32)&sema->exclusiveWaiters);
   } else if (sema->sharedWaiters) {
      CpuSched_Wakeup((uint32)&sema->sharedWaiters);
   }

   SP_Unlock(&sema->lock);
}

/*
 *-----------------------------------------------------------------------------
 *
 * Semaphore_UpgradeFromShared --
 *    Ask for exclusive writer access while already holding shared reader
 *    privilege through the given reader-writer semaphore 'sema'. If the
 *    upgrade is not immediately available, only the first caller can
 *    wait for the upgrade. Others fail.
 *
 * Results:
 *    VMK_OK if exclusive write access is granted to the caller on return.
 *    VMK_BUSY if not possible to upgrade caller, caller retains shared
 *    reader status.
 *
 * Side effects:
 *    Caller may be suspended while waiting for upgrade.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
Semaphore_UpgradeFromShared(RWSemaphore *sema)
{
   ASSERT(World_IsSafeToBlock());

   SP_Lock(&sema->lock);
   ASSERT(sema->sharedAccess > 0);
   ASSERT(sema->exclusiveAccess == 0);
   if (sema->upgradeWaiter) {
      SP_Unlock(&sema->lock);
      return VMK_BUSY;
   }
   sema->upgradeWaiter = TRUE;
   while (sema->sharedAccess > 1) {
      CpuSched_Wait((uint32)&sema->upgradeWaiter,
                    CPUSCHED_WAIT_RWLOCK,
                    &sema->lock);
      SP_Lock(&sema->lock);
   }
   ASSERT(sema->exclusiveAccess == 0);
   sema->upgradeWaiter = FALSE;
   sema->sharedAccess--;
   sema->exclusiveAccess++;
   SP_Unlock(&sema->lock);
   return VMK_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 * Semaphore_DowngradeToShared --
 *    Ask to be dowgraded from exclusive writer access to shared reader
 *    access, through the given reader-writer semaphore 'sema'.
 *
 * Results:
 *    Caller has shared reader access on return from this function.
 *
 * Side effects:
 *    Wake up any shared waiters.
 *
 *-----------------------------------------------------------------------------
 */
void
Semaphore_DowngradeToShared(RWSemaphore *sema)
{
   SP_Lock(&sema->lock);
   ASSERT((sema->exclusiveAccess == 1) && (sema->sharedAccess == 0));
   /* For an upgradeWaiter to arrive, it needs to be a shared reader first,
    * and we couldn't have received a shared reader during the time the
    * upgrade was in effect.
    */
   ASSERT(sema->upgradeWaiter == FALSE);
   sema->exclusiveAccess--;
   sema->sharedAccess++;
   if ((sema->exclusiveWaiters == 0) && (sema->sharedWaiters)) {
      CpuSched_Wakeup((uint32)&sema->sharedWaiters);      
   }
   SP_Unlock(&sema->lock);
}

/*
 *----------------------------------------------------------------------
 *
 * Semaphore_IsShared
 *
 *      Return TRUE if RWsemaphore currently has shared user(s)
 *
 * Results:
 *      Return TRUE if RWsemaphore currently has shared user(s)
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
Bool
Semaphore_IsShared(RWSemaphore *sema)
{
   Bool isShared;

   SP_Lock(&sema->lock);
   if (sema->sharedAccess > 0) {
      isShared = TRUE;
   } else {
      isShared = FALSE;
   }
   SP_Unlock(&sema->lock);

   return isShared;
}

/*
 *----------------------------------------------------------------------
 *
 * Semaphore_IsExclusive
 *
 *      Return TRUE if RWsemaphore currently has exclusive user
 *
 * Results:
 *      Return TRUE if RWsemaphore currently has exclusive user
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
Bool
Semaphore_IsExclusive(RWSemaphore *sema)
{
   Bool isExclusive;

   SP_Lock(&sema->lock);
   if (sema->exclusiveAccess > 0) {
      isExclusive = TRUE;
   } else {
      isExclusive = FALSE;
   }
   SP_Unlock(&sema->lock);

   return isExclusive;
}
