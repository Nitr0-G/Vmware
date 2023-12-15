/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * userStat.c --
 *
 *	UserWorld statistics gathering infrastructure.
 *
 *	See userStatDef.h for the defined stats, and simple usage
 *	instructions.
 */

#include "user_int.h"
#include "userStatDef.h"
#include "userStat.h"
#include "histogram.h"
#include "userLinux.h" // for linux syscall table length
#include "uwvmkDispatch.h" // for uwvmk syscall table length

#define LOGLEVEL_MODULE UserStat
#include "userLog.h"

/*
 * This file is empty if stats are not enabled.
 */
#if defined(USERSTAT_ENABLED) // {

/*
 * Global stats object.  Never destroyed.  Holds accumulated stats for
 * all dead cartels (as they die, they merge stats into here).
 */
static UserStat_Record userStatGlobalRecord;

/*
 * Global object for recording cartel stats when invoked by
 * non-userworlds.  (E.g., helper worlds).  This is the "cartel" level
 * stats for non-userworlds, and is useful.
 */
static UserStat_Record userStatOtherRecord;

/*
 * Global object for recording thread stats when invoked by
 * non-userworlds.  (E.g., helper worlds).  This is the "thread" level
 * stats for non-userworlds and is basically useless (use
 * userStatOtherRecord).
 */
static UserStat_Record userStatIgnoredRecord;

/*
 * Exported pointer to the global stats.  Doubles as initialization
 * flag.
 */
UserStat_Record* userStat_GlobalRecord = NULL;

UserStat_Record* userStat_OtherRecord = NULL;

UserStat_Record* userStat_IgnoredRecord = NULL;

static int UserStatProcRead(Proc_Entry *entry, char* buffer, int* len);
static int UserStatProcWrite(Proc_Entry *entry, char* buffer, int* len);

static void UserStatPrintCounter(uint64 val,
                                 const char* name,
                                 char* buffer,
                                 int* bufLen);
static void UserStatPrintArray(const uint64* array,
                               const int arrayLen,
                               const char* name,
                               char* buffer,
                               int* bufLen);

/* XXX these three should be auto-generated someday: */
static VMK_ReturnStatus UserStatInitRecord(UserStat_Record* rec,
                                           Heap_ID heap,
                                           const char* type);
static VMK_ReturnStatus UserStatCleanupRecord(UserStat_Record* rec,
                                              Heap_ID heap);
static VMK_ReturnStatus UserStatResetRecord(UserStat_Record* rec);
static void UserStatPrint(UserStat_Record* rec, char* buffer, int* len);


/*
 *----------------------------------------------------------------------
 *
 * UserStat_Init --
 *
 *	Set up global stat struct, register top-level proc node for
 *	uwstats.  Undo with UserStat_Cleanup.
 *
 * Results:
 *	VMK_OK
 *
 * Side effects:
 *	Proc nodes and a lock created
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserStat_Init(void)
{
   ASSERT(userStat_GlobalRecord == NULL);

   userStat_GlobalRecord = &userStatGlobalRecord;
   UserStatInitRecord(userStat_GlobalRecord, mainHeap, "global");

   /*
    * Make sure the various array sizes are sufficient to cover the
    * things they track.  Also, the ARRINC and ARRADD macros will
    * bounds-check their arguments.
    */
   ASSERT(ARRAYSIZE(userStatGlobalRecord.linuxSyscallCount) >= UserLinux_SyscallTableLen);
   ASSERT(ARRAYSIZE(userStatGlobalRecord.uwvmkSyscallCount) >= UWVMKSYSCALL_SYSCALLNUM_MAX);
   ASSERT(ARRAYSIZE(userStatGlobalRecord.signalsSent) >= USERWORLD_NSIGNAL);
   ASSERT(ARRAYSIZE(userStatGlobalRecord.userObjCreated) >= USEROBJ_TYPE_MAXIMUMTYPE);
   ASSERT(ARRAYSIZE(userStatGlobalRecord.userObjDestroyed) >= USEROBJ_TYPE_MAXIMUMTYPE);
   ASSERT(ARRAYSIZE(userStatGlobalRecord.proxySyscallCount) >= USERPROXY_END);

   userStat_IgnoredRecord = &userStatIgnoredRecord;
   UserStatInitRecord(userStat_IgnoredRecord, mainHeap, "ignored");

   userStat_OtherRecord = &userStatOtherRecord;
   UserStatInitRecord(userStat_OtherRecord, mainHeap, "other");

   /* Add "uwstats" /proc directory */
   Proc_InitEntry(&userStatGlobalRecord.procDir);
   Proc_Register(&userStatGlobalRecord.procDir, "uwstats", TRUE);
   /*
    * Note, hidden directories cannot have subdirectories, so the
    * above uwstats proc node cannot be hidden (it has cartel-<id>
    * subdirectories).
    */
   
   /* Add 'global' stats entry to directory */
   Proc_InitEntry(&userStatGlobalRecord.procEntry);
   userStatGlobalRecord.procEntry.private = &userStatGlobalRecord;
   userStatGlobalRecord.procEntry.parent = &userStatGlobalRecord.procDir;
   userStatGlobalRecord.procEntry.read = UserStatProcRead;
   userStatGlobalRecord.procEntry.write = UserStatProcWrite;
   Proc_RegisterHidden(&userStatGlobalRecord.procEntry, "global", FALSE);

   /* Add 'other' stats entry to directory */
   Proc_InitEntry(&userStatOtherRecord.procEntry);
   userStatOtherRecord.procEntry.private = &userStatOtherRecord;
   userStatOtherRecord.procEntry.parent = &userStatGlobalRecord.procDir;
   userStatOtherRecord.procEntry.read = UserStatProcRead;
   userStatOtherRecord.procEntry.write = UserStatProcWrite;
   Proc_RegisterHidden(&userStatOtherRecord.procEntry, "other", FALSE);

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserStat_CartelInit --
 *
 *	Initialize cartel-level stats infrastructure.  Initialize
 *	cartel stat record and creates a subdirectory for the cartel
 *	stats in the proc tree, puts an "allthreads" proc node in that
 *	directory.
 *
 * Results:
 *	VMK_OK
 *
 * Side effects:
 *	Proc nodes created, stat record initialized
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserStat_CartelInit(User_CartelInfo* uci)
{
   UserStat_Record* const rec = &uci->cartelStats;
   VMK_ReturnStatus status;
   char pname[128];

   ASSERT(rec);

   status = UserStatInitRecord(rec, uci->heap, "cartel");
   if (status != VMK_OK) {
      return status;
   }

   /* Add cartel sub-directory of uwstats */
   snprintf(pname, sizeof pname, "cartel-%d", uci->cartelID);
   Proc_InitEntry(&rec->procDir);
   rec->procDir.parent = &userStatGlobalRecord.procDir;
   Proc_RegisterHidden(&rec->procDir, pname, TRUE);

   /* Add cartel-wide stats entry in new directory */
   snprintf(pname, sizeof pname, "allthreads");
   Proc_InitEntry(&rec->procEntry);
   rec->procEntry.private = rec;
   rec->procEntry.parent = &rec->procDir;
   rec->procEntry.read = UserStatProcRead;
   rec->procEntry.write = UserStatProcWrite;
   Proc_RegisterHidden(&rec->procEntry, pname, FALSE);

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserStat_ThreadInit --
 *
 *	Initialize per-thread stat infrastructure.  Hooks into
 *	cartel's proc node for displaying stats.
 *
 * Results:
 *	VMK_OK
 *
 * Side effects:
 *	Thread stats initialized
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserStat_ThreadInit(UserStat_Record* rec,
                    World_ID threadID,
                    Heap_ID heap,
                    UserStat_Record* cartelStats)
{
   VMK_ReturnStatus status;
   char pname[128];

   ASSERT(rec);

   status = UserStatInitRecord(rec, heap, "thread");
   if (status != VMK_OK) {
      return status;
   }

   /* Add thread stats proc entry in cartel's directory */
   snprintf(pname, sizeof pname, "thread-%d", threadID);
   Proc_InitEntry(&rec->procEntry);
   rec->procEntry.private = rec;
   rec->procEntry.parent = &cartelStats->procDir;
   rec->procEntry.read = UserStatProcRead;
   rec->procEntry.write = UserStatProcWrite;
   Proc_RegisterHidden(&rec->procEntry, pname, FALSE);

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserStat_CartelCleanup --
 *
 *	Undo UserStat_CartelInit
 *
 * Results:
 *	VMK_OK
 *
 * Side effects:
 *	Removes proc nodes and directories.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserStat_CartelCleanup(User_CartelInfo* uci)
{
   UserStat_Record* const rec = &uci->cartelStats;
   int i;

   ASSERT(rec);

   /*
    * Copy all of the dead cartel's stats into the global stat record.
    * No need to lock it since the cartel is dead.
    */

   /*
    * Define Hutchins macros to copy each type of stat.
    */
#define UWSTAT_DECLARE_COUNTER(NAME)            \
   userStatGlobalRecord.NAME += rec->NAME;

#define UWSTAT_DECLARE_ARRAY(NAME, size)                        \
   for (i = 0; i < ARRAYSIZE(userStatGlobalRecord.NAME); i++)   \
   {                                                            \
      userStatGlobalRecord.NAME[i] += rec->NAME[i];             \
   }

#define UWSTAT_DECLARE_HISTOGRAM(NAME, init... )        \
   Histogram_MergeIn(userStatGlobalRecord.NAME,         \
                     rec->NAME);

#define UWSTAT_DECLARE_TIMER(units, NAME, init... )     \
   Histogram_MergeIn(userStatGlobalRecord.NAME.results, \
                     rec->NAME.results);

   /* Expand stats list to merge rec into global stats */
   USERSTAT_STATSLIST

#undef UWSTAT_DECLARE_ARRAY
#undef UWSTAT_DECLARE_COUNTER
#undef UWSTAT_DECLARE_HISTOGRAM
#undef UWSTAT_DECLARE_TIMER

   Proc_Remove(&rec->procEntry);
   Proc_Remove(&rec->procDir);
   UserStatCleanupRecord(rec, uci->heap);
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserStat_ThreadCleanup --
 *
 *	Undo UserStat_ThreadInit.
 *
 * Results:
 *	VMK_OK
 *
 * Side effects:
 *	Removes proc entries
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserStat_ThreadCleanup(UserStat_Record* rec, Heap_ID heap)
{
   /*
    * No need to merge into cartel stats, those are updated
    * simultaneously with the thread stats.
    */

   ASSERT(rec);
   Proc_Remove(&rec->procEntry);
   UserStatCleanupRecord(rec, heap);
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserStatProcRead --
 *
 *	Proc node read callback handler.  Assumes 'private' field of
 *	proc entry contains a UserStat_Record.
 *
 * Results:
 *	0
 *
 * Side effects:
 *	Many Proc_Printfs
 *
 *----------------------------------------------------------------------
 */
static int
UserStatProcRead(Proc_Entry *entry, char* buffer, int* len)
{
   UserStat_Record* rec = entry->private;

   ASSERT(rec != NULL);

   UserStatPrint(rec, buffer, len);

   return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * UserStatProcWrite --
 *
 *	Proc node write callback handler.  Assumes 'private' field of
 *	proc entry contains a UserStat_Record.
 *
 * Results:
 *	0
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static int
UserStatProcWrite(Proc_Entry *entry, char* buffer, int* len)
{
   static const char* CMD_RESET = "reset";
   UserStat_Record* rec = entry->private;
   ASSERT(rec != NULL);

   if (strncmp(buffer, CMD_RESET, strlen(CMD_RESET)) == 0) {
      UserStatResetRecord(rec);
   }

   return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * UserStatInitRecord --
 *
 *	Initialize record (wipe to zero, allocate histograms)
 *
 * Results:
 *	VMK_OK
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
UserStatInitRecord(UserStat_Record* rec,
                   Heap_ID heap,
                   const char* type)
{
   Bool allocFailed = FALSE;
   char lockName[20];
   ASSERT(rec);

   UWLOG(1, "rec=%p type=%s", rec, type);

   /* UserStat_Init must have been called */
   ASSERT(userStat_GlobalRecord != NULL);

   snprintf(lockName, sizeof lockName, "uwstat-%s", type);
   SP_InitLock(lockName, &rec->lock, UW_SP_RANK_STATS);

   /*
    * Define Hutchins macros to initialize each type of stat.
    */

#define UWSTAT_DECLARE_ARRAY(NAME, size) \
   memset(rec->NAME, 0, sizeof rec->NAME);

#define UWSTAT_DECLARE_COUNTER(NAME) \
   rec->NAME = 0ULL;

#define UWSTAT_DECLARE_HISTOGRAM(NAME, INIT... )        \
   {                                                    \
      int64 initializer[] = INIT;                       \
      rec->NAME = Histogram_New(heap,                   \
                                ARRAYSIZE(initializer), \
                                initializer);           \
      if (rec->NAME == NULL) {                          \
         allocFailed = TRUE;                            \
      }                                                 \
   }

#define UWSTAT_DECLARE_TIMER(units, NAME, INIT... )             \
   {                                                            \
      int64 initializer[] = INIT;                               \
      rec->NAME.start = 0;                                      \
      rec->NAME.results = Histogram_New(heap,                   \
                                        ARRAYSIZE(initializer), \
                                        initializer);           \
      if (rec->NAME.results == NULL) {                          \
         allocFailed = TRUE;                                    \
      }                                                         \
   }

   /* Expand stats list to initialize each stat */
   USERSTAT_STATSLIST

#undef UWSTAT_DECLARE_ARRAY
#undef UWSTAT_DECLARE_COUNTER
#undef UWSTAT_DECLARE_HISTOGRAM
#undef UWSTAT_DECLARE_TIMER

   if (allocFailed) {
      UWLOG(0, "Alloc of stat histogram failed");
      (void) UserStatCleanupRecord(rec, heap);
      return VMK_NO_MEMORY;
   }

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserStatResetRecord --
 *
 *	Reset given record (wipe to zero, reset histograms)
 *
 * Results:
 *	VMK_OK
 *
 * Side effects:
 *	Stats are lost
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
UserStatResetRecord(UserStat_Record* rec)
{
   ASSERT(rec);

   UWLOG(1, "rec=%p", rec);

   /* UserStat_Init must have been called */
   ASSERT(userStat_GlobalRecord != NULL);

   /*
    * Note: if rec is a 'thread' or 'ignored' stats rec, then this lock
    * doesn't protect the structure.  That's okay.  Don't do that.
    */
   UserStat_Lock(rec);

   /*
    * Define Hutchins macros to reset each type of stat.
    */

#define UWSTAT_DECLARE_ARRAY(NAME, size)          \
   memset(rec->NAME, 0, sizeof rec->NAME);

#define UWSTAT_DECLARE_COUNTER(NAME)      \
   rec->NAME = 0ULL;

#define UWSTAT_DECLARE_HISTOGRAM(NAME, init... )        \
   Histogram_Reset(rec->NAME);

/* Note leave rec->NAME.start field alone: in-progress timers not impacted. */
#define UWSTAT_DECLARE_TIMER(units, NAME, init... )             \
   Histogram_Reset(rec->NAME.results);

   /* Expand stats list to reset each stat */
   USERSTAT_STATSLIST

#undef UWSTAT_DECLARE_ARRAY
#undef UWSTAT_DECLARE_COUNTER
#undef UWSTAT_DECLARE_HISTOGRAM
#undef UWSTAT_DECLARE_TIMER

   UserStat_Unlock(rec);

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserStatCleanupRecord --
 *
 *	Undo UserStatInitRecord.
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus 
UserStatCleanupRecord(UserStat_Record* rec, Heap_ID heap)
{
   ASSERT(rec);

   SP_CleanupLock(&rec->lock);

   /*
    * Define Hutchins Macros to destroy each type of stat
    *
    * NOTE: invoked directly if there is a partial failure in
    * UserStatInitRecord, so must be prepared for NULL
    * allocations (Histogram_Delete is okay with that).
    */

#define UWSTAT_DECLARE_ARRAY(name, size) /* no-op */

#define UWSTAT_DECLARE_COUNTER(name)     /* no-op */

#define UWSTAT_DECLARE_HISTOGRAM(NAME, init... )        \
   Histogram_Delete(heap, rec->NAME);

#define UWSTAT_DECLARE_TIMER(units, NAME, init... )     \
   Histogram_Delete(heap, rec->NAME.results);

   /* Expand stats list to destroy each stat */
   USERSTAT_STATSLIST

#undef UWSTAT_DECLARE_ARRAY
#undef UWSTAT_DECLARE_COUNTER
#undef UWSTAT_DECLARE_HISTOGRAM
#undef UWSTAT_DECLARE_TIMER

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserStatPrintCounter --
 *
 *	Callback for the stat printing routine.  Print a uint64 counter.
 *
 * Results:
 *	Updates *bufLen
 *
 * Side effects:
 *	Proc_Printf
 *
 *----------------------------------------------------------------------
 */
static void
UserStatPrintCounter(uint64 val,
                     const char* name,
                     char* buffer,
                     int* bufLen)
{
   Proc_Printf(buffer, bufLen,
               "%s = %"FMT64"u (uint64 counter)\n"
               "\n",
               name, val);
}


/*
 *----------------------------------------------------------------------
 *
 * UserStatPrintArray --
 *
 *	Callback for the stat printing routine.  Print a uint64 array.
 *
 * Results:
 *	Updates *bufLen
 *
 * Side effects:
 *	Proc_Printf
 *
 *----------------------------------------------------------------------
 */
static void
UserStatPrintArray(const uint64* array,
                   const int arrayLen,
                   const char* name,
                   char* buffer,
                   int* bufLen)
{
   static const int PRINT_PER_LINE = 2;
   static const char* INDENT = "    ";
   int printed = 0;
   uint64 total = 0;
   int i;

   Proc_Printf(buffer, bufLen, "%s: (uint64 array, %d entries)\n", name, arrayLen);
   for (i = 0; i < arrayLen; i++) {
      uint64 val = array[i];

      // Only print interesting (non-zero) values
      if (val != 0) {
         const char* prefix = ", ";
         if (printed == 0) {
            prefix = INDENT;
         }
         Proc_Printf(buffer, bufLen, "%s[%3d] = %16"FMT64"u", prefix, i, val);
         printed++;
         total += val;
      }

      if (printed > PRINT_PER_LINE) {
         Proc_Printf(buffer, bufLen, ",\n");
         printed = 0;
      }
   }

   if (printed != 0) {
      Proc_Printf(buffer, bufLen, "\n");
   }

   // Print total hits over the whole array
   Proc_Printf(buffer, bufLen,
               "%stotal = %"FMT64"u\n"
               "\n",
               INDENT, total);
}


/*
 *----------------------------------------------------------------------
 *
 * UserStatPrintHistogramInt --
 *
 *	Used by Histogram and Timer print routines.  Handles empty
 *	histograms nicely.
 *
 * Results:
 *	Updates *bufLen
 *
 * Side effects:
 *	Proc_Printf
 *
 *----------------------------------------------------------------------
 */
static void
UserStatPrintHistogramInt(Histogram_Handle histo,
                          char* buffer,
                          int* bufLen)
{
   uint64 hits = Histogram_Count(histo);
   if (hits > 0) {
      Histogram_ProcFormat(histo, "    ", buffer, bufLen);
   } else {
      Proc_Printf(buffer, bufLen, "    [no hits]\n");
   }
   Proc_Printf(buffer, bufLen, "\n");
}

/*
 *----------------------------------------------------------------------
 *
 * UserStatPrintHistogram --
 *
 *	Callback for the stat printing routine.  Print a histogram.
 *
 * Results:
 *	Updates *bufLen
 *
 * Side effects:
 *	Proc_Printf
 *
 *----------------------------------------------------------------------
 */
static void
UserStatPrintHistogram(Histogram_Handle histo,
                        const char* name,
                        char* buffer,
                        int* bufLen)
{
   Proc_Printf(buffer, bufLen, "%s: (histogram)\n", name);
   UserStatPrintHistogramInt(histo, buffer, bufLen);
}


/*
 *----------------------------------------------------------------------
 *
 * UserStatPrintTimer --
 *
 *	Callback for the stat printing routine.  Print a timer (just
 *	print its histogram).  
 *
 * Results:
 *	Updates *bufLen
 *
 * Side effects:
 *	Proc_Printf
 *
 *----------------------------------------------------------------------
 */
static void
UserStatPrintTimer(UserStat_Timer* timerHisto,
                    const char* name,
                    const char* units,
                    char* buffer,
                    int* bufLen)
{
   Proc_Printf(buffer, bufLen, "%s: (raw cycles)\n", name);
   // XXX Should honor/use the units passed in
   UserStatPrintHistogramInt(timerHisto->results, buffer, bufLen);
}


/*
 *----------------------------------------------------------------------
 *
 * UserStatPrint --
 *
 *	Print the given UserStat_Record to the given buffer/len with
 *	Proc_Printf.  Prints each element using the appropriate
 *	callback.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Lots of Proc_Printfs
 *
 *----------------------------------------------------------------------
 */
static void
UserStatPrint(UserStat_Record* rec,
              char* buffer,
              int* len)
{
   *len = 0;

   UWLOG(1, "rec=%p buffer=%p", rec, buffer);

   Proc_Printf(buffer, len, "UserStat_Record:\n");
   Proc_Printf(buffer, len, "    sizeof UserStat_Record = %d bytes\n",
               sizeof(UserStat_Record));
   
   /*
    * Note this doesn't protect printing of thread-specific stats, as
    * they're guarded with the cartel lock when updated.
    */
   UserStat_Lock(rec);

   /*
    * Declare Hutchins Macros for printing each type of stat.
    */

#define UWSTAT_DECLARE_ARRAY(NAME, SIZE) \
   UserStatPrintArray(rec->NAME, SIZE, #NAME, buffer, len);

#define UWSTAT_DECLARE_COUNTER(NAME) \
   UserStatPrintCounter(rec->NAME, #NAME, buffer, len);

#define UWSTAT_DECLARE_HISTOGRAM(NAME, init... ) \
   UserStatPrintHistogram(rec->NAME, #NAME, buffer, len);

#define UWSTAT_DECLARE_TIMER(UNITS, NAME, init... ) \
   UserStatPrintTimer(&rec->NAME, #NAME, #UNITS, buffer, len);

   /*
    * Expand statslist to generate print calls for each stat
    */
   USERSTAT_STATSLIST

#undef UWSTAT_DECLARE_ARRAY
#undef UWSTAT_DECLARE_COUNTER
#undef UWSTAT_DECLARE_HISTOGRAM
#undef UWSTAT_DECLARE_TIMER

   UserStat_Unlock(rec);

   return;
}

#endif /* USERSTAT_ENABLED */ // }
