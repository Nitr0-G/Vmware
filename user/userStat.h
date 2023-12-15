/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * userStat.h --
 *
 *	UserWorld statistics gathering infrastructure.
 *
 *	See userStatDef.h for the defined stats, and simple usage
 *	instructions.
 */

#ifndef VMKERNEL_USER_USERSTAT_H
#define VMKERNEL_USER_USERSTAT_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "vm_basic_types.h"
#include "histogram.h"
#include "timer.h"
#include "splock.h"
#include "user_int.h"
#include "userStatDef.h"
struct User_CartelInfo;

#if defined(USERSTAT_ENABLED) // {

extern UserStat_Record* userStat_OtherRecord;
extern UserStat_Record* userStat_IgnoredRecord;

extern VMK_ReturnStatus UserStat_Init(void);
extern VMK_ReturnStatus UserStat_CartelInit(struct User_CartelInfo* uci);
extern VMK_ReturnStatus UserStat_CartelCleanup(struct User_CartelInfo* uci);
extern VMK_ReturnStatus UserStat_ThreadInit(UserStat_Record* rec,
                                            World_ID threadID,
                                            Heap_ID heap, 
                                            UserStat_Record* cartelStats);
extern VMK_ReturnStatus UserStat_ThreadCleanup(UserStat_Record* rec, Heap_ID heap);


/*
 *----------------------------------------------------------------------
 *
 * UserStat_CartelRecord --
 *
 *	Get the stat record for the current cartel.  If the current
 *	world is not a UserWorld (e.g., a helper world), return the
 *	"other" stats record
 *
 * Results:
 *	A UserStat_Record*
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static INLINE UserStat_Record* 
UserStat_CartelRecord(void) 
{
   World_Handle* const w = MY_RUNNING_WORLD;
   if (World_IsUSERWorld(w)) {
      return &w->userCartelInfo->cartelStats;
   } else {
      return userStat_OtherRecord;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * UserStat_ThreadRecord --
 *
 *	Return the stat record for the current thread.  If the current
 *	world is not a UserWorld, return the "ignored" stats record.
 *
 * Results:
 *	A UserStat_Record*
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static INLINE UserStat_Record* 
UserStat_ThreadRecord(void) 
{
   World_Handle* const w = MY_RUNNING_WORLD;
   if (World_IsUSERWorld(w)) {
      return &w->userThreadInfo->threadStats;
   } else {
      return userStat_IgnoredRecord;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * UserStat_Lock --
 *
 *	Lock the given UserStat_Record
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Lock acquired
 *
 *----------------------------------------------------------------------
 */
static INLINE void
UserStat_Lock(UserStat_Record* rec) {
   SP_Lock(&rec->lock);
}


/*
 *----------------------------------------------------------------------
 *
 * UserStat_Unlock --
 *
 *	Unlock the given UserStat_Record
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Lock released
 *
 *----------------------------------------------------------------------
 */
static INLINE void
UserStat_Unlock(UserStat_Record* rec) {
   SP_Unlock(&rec->lock);
}


/*
 *----------------------------------------------------------------------
 *
 * UWSTAT_ADD --
 *
 *	For given variable, add the given amount to it.  Add it on the
 *	current thread and cartel records.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Stat is recorded
 *
 *----------------------------------------------------------------------
 */
#define UWSTAT_ADD(VAR, VAL) do {                                       \
      UserStat_Record* const cartelRecord = UserStat_CartelRecord();    \
      UserStat_Record* const threadRecord = UserStat_ThreadRecord();    \
      const uint64 tmpVal = (VAL);                                      \
      UserStat_Lock(cartelRecord);                                      \
      cartelRecord->VAR += tmpVal;                                      \
      threadRecord->VAR += tmpVal;                                      \
      UserStat_Unlock(cartelRecord);                                    \
   } while (0)


/*
 *----------------------------------------------------------------------
 *
 * UWSTAT_INC --
 *
 *	UWSTAT_ADD(VAR, 1)
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Stat is incremented
 *
 *----------------------------------------------------------------------
 */
#define UWSTAT_INC(VAR) UWSTAT_ADD(VAR, 1)


/*
 *----------------------------------------------------------------------
 *
 * UWSTAT_ARRADD --
 *
 *	Add the given VAL to the given ARR at the given IDX.  Updates
 *	thread and cartel stat records.  IDX is bounds checked.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Stat is recorded.
 *
 *----------------------------------------------------------------------
 */
#define UWSTAT_ARRADD(ARR, IDX, VAL) do {                               \
      UserStat_Record* const cartelRecord = UserStat_CartelRecord();    \
      UserStat_Record* const threadRecord = UserStat_ThreadRecord();    \
      const uint32 uwStatIndex = (IDX);                                 \
      const uint64 tmpVal = (VAL);                                      \
      ASSERT(uwStatIndex < ARRAYSIZE(cartelRecord->ARR));               \
      if (LIKELY(uwStatIndex < ARRAYSIZE(cartelRecord->ARR))) {         \
         UserStat_Lock(cartelRecord);                                   \
	 cartelRecord->ARR[uwStatIndex] += tmpVal;                      \
	 threadRecord->ARR[uwStatIndex] += tmpVal;                      \
         UserStat_Unlock(cartelRecord);                                 \
      }                                                                 \
   } while (0)


/*
 *----------------------------------------------------------------------
 *
 * UWSTAT_ARRINC --
 *
 *	UWSTAT_ARRADD(ARR, IDX, 1)
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Stat is incremented
 *
 *----------------------------------------------------------------------
 */
#define UWSTAT_ARRINC(ARR, IDX) UWSTAT_ARRADD(ARR, IDX, 1)


/*
 *----------------------------------------------------------------------
 *
 * UWSTAT_INSERT --
 *
 *	Put given VAL into the histogram HISTO.  Histograms track the
 *	max, min, mean and a histogram of values put in them.  See
 *	vmkernel/main/histogram.c
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Histogram is updated
 *
 *----------------------------------------------------------------------
 */
#define UWSTAT_INSERT(HISTO, VAL) do {                                  \
      UserStat_Record* const cartelRecord = UserStat_CartelRecord();    \
      UserStat_Record* const threadRecord = UserStat_ThreadRecord();    \
      const uint64 tmpVal = (VAL);                                      \
      UserStat_Lock(cartelRecord);                                      \
      Histogram_Insert(cartelRecord->HISTO, tmpVal);                    \
      Histogram_Insert(threadRecord->HISTO, tmpVal);                    \
      UserStat_Unlock(cartelRecord);                                    \
   } while (0)


/*
 *----------------------------------------------------------------------
 *
 * UWSTAT_TIMERSTART --
 *
 *	Record the current time in the thread-local stat struct for
 *	the given stat.  Not much use until TIMERSTOP is invoked.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Timer_GetCycles() is recorded
 *
 *----------------------------------------------------------------------
 */
#define UWSTAT_TIMERSTART(HISTO) do {                                   \
	UserStat_ThreadRecord()->HISTO.start = Timer_GetCycles();       \
    } while (0)
   

/*
 *----------------------------------------------------------------------
 *
 * UWSTAT_TIMERSTOP --
 *
 *	Record the current time, diff with the saved time from
 *	TIMERSTART, and put the result in a histogram.  If invoked by
 *	a helper world, this stat is dropped (there is no reliable
 *	place to store the start time for a helper world).
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Histogram is updated, timer is reset
 *
 *----------------------------------------------------------------------
 */
#define UWSTAT_TIMERSTOP(HISTO) do {                                    \
      UserStat_Record* const cartelRecord = UserStat_CartelRecord();    \
      UserStat_Record* const threadRecord = UserStat_ThreadRecord();    \
      if (threadRecord != userStat_IgnoredRecord) {                     \
         const Timer_RelCycles tmpDelta = Timer_GetCycles()             \
           	- (threadRecord->HISTO.start);                          \
         if (threadRecord->HISTO.start == -1) {                         \
	     UWWarn("Mis-matched UWSTAT_TIMERSTOP(%s)", #HISTO);        \
         } else {                                                       \
            threadRecord->HISTO.start = -1;                             \
            UserStat_Lock(cartelRecord);                                \
            Histogram_Insert(cartelRecord->HISTO.results, tmpDelta);    \
            Histogram_Insert(threadRecord->HISTO.results, tmpDelta);    \
            UserStat_Unlock(cartelRecord);                              \
         }                                                              \
      }                                                                 \
   } while (0)


#else // } {  /* if defined(USERSTAT_ENABLED) */
/*
 * Stat-collecting macros go away.
 */
#define UWSTAT_ADD(var, val) ((void)0)
#define UWSTAT_INC(var) ((void)0)
#define UWSTAT_ARRADD(var, idx, val) ((void)0)
#define UWSTAT_ARRINC(var, idx) ((void)0)
#define UWSTAT_INSERT(hist, val) ((void)0)
#define UWSTAT_TIMERSTART(timer) ((void)0)
#define UWSTAT_TIMERSTOP(timer) ((void)0)

// No-op wrappers for disabled stat builds, see userStat.c for real versions

static INLINE VMK_ReturnStatus 
UserStat_Init(void) 
{
   return VMK_OK;
}

static INLINE VMK_ReturnStatus 
UserStat_CartelInit(struct User_CartelInfo* uci)
{
   return VMK_OK;
}

static INLINE VMK_ReturnStatus 
UserStat_CartelCleanup(struct User_CartelInfo* uci)
{
   return VMK_OK;
}

static INLINE VMK_ReturnStatus 
UserStat_ThreadInit(UserStat_Record* ign1,
                    World_ID threadID,
                    Heap_ID heap, 
                    UserStat_Record* ign2)
{
   return VMK_OK;
}

static INLINE VMK_ReturnStatus 
UserStat_ThreadCleanup(UserStat_Record* ignored,
                       Heap_ID heap)
{
   return VMK_OK;
}

#endif // } /* USERSTAT_ENABLED is defined */

#endif /* VMKERNEL_USER_USERSTAT_H */
