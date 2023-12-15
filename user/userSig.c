/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * userSig.c --
 *
 *	UserWorld signals
 */

#include "user_int.h"
#include "userSig.h"
#include "userStat.h"
#include "timer_dist.h"

#define LOGLEVEL_MODULE UserSig
#include "userLog.h"


#define ASSERT_SIGTHREADLOCKED(s) ASSERT(UserSigThreadIsLocked(s))
#define ASSERT_CARTELSIGLOCKED(s) ASSERT(UserSigCartelIsLocked(s))


typedef struct UserSigRestoreContext {
/*
 * This part of the struct is compatible with Linux 'struct sigcontext'.
 * Its put on the stack and used by old-style (not SA_SIGINFO) handlers
 * to get at interrupted register state.  Used by the SIGPROF profiler
 * hook and by the VMX to dump register state.
 */
    uint16 gs, gsPad;
    uint16 fs, fsPad;
    uint16 es, esPad;
    uint16 ds, dsPad;
    uint32 edi;
    uint32 esi;
    uint32 ebp;
    uint32 esp;
    uint32 ebx;
    uint32 edx;
    uint32 ecx;
    uint32 eax;
    uint32 trapno;
    uint32 err;
    uint32 eip;
    uint16 cs, csPad;
    uint32 eflags;
    uint32 espAtSignal;
    uint16 ss, ssPad;
    struct _fpstate *fpstate;
    uint32 oldmask;
    uint32 cr2;
/* This part of the struct is specific to the vmkernel. */
    UserSigSet restoreMask;
} UserSigRestoreContext;


/*
 * Flags for different possible "default" behaviors for a signal handler.
 */
typedef enum UserSigDefault {
   USERSIG_DFL_UNK  = 0x0, /* unknown default handler */
   USERSIG_DFL_CORE = 0x1, /* default handler core dumps */
   USERSIG_DFL_TERM = 0x2, /* default handler terminates */
   USERSIG_DFL_IGN  = 0x3, /* default handler ignores */
   USERSIG_DFL_STOP = 0x4, /* default handler stops cartel */
} UserSigDefault;


/*
 * Table mapping signals to their default behaviors.  Entries not listed
 * should probably be _CORE (see UserSigDefaultAction)
 */
static const UserSigDefault userSigDefaultFlags[] = {
   [LINUX_SIGHUP] = USERSIG_DFL_TERM,
   [LINUX_SIGINT] = USERSIG_DFL_TERM,
   [LINUX_SIGQUIT] = USERSIG_DFL_CORE,
   [LINUX_SIGILL] = USERSIG_DFL_CORE,
   [LINUX_SIGABRT] = USERSIG_DFL_CORE,
   [LINUX_SIGFPE] = USERSIG_DFL_CORE,
   [LINUX_SIGKILL] = USERSIG_DFL_TERM,
   [LINUX_SIGSEGV] = USERSIG_DFL_CORE,
   [LINUX_SIGPIPE] = USERSIG_DFL_TERM,
   [LINUX_SIGALRM] = USERSIG_DFL_TERM,
   [LINUX_SIGTERM] = USERSIG_DFL_TERM,
   [LINUX_SIGUSR1] = USERSIG_DFL_TERM,
   [LINUX_SIGUSR2] = USERSIG_DFL_TERM,
   [LINUX_SIGCHLD] = USERSIG_DFL_IGN,
   [LINUX_SIGCONT] = USERSIG_DFL_IGN, /* XXX not implemented correctly */
   [LINUX_SIGSTOP] = USERSIG_DFL_STOP,
   [LINUX_SIGTSTP] = USERSIG_DFL_STOP,
   [LINUX_SIGTTIN] = USERSIG_DFL_STOP,
   [LINUX_SIGTTOU] = USERSIG_DFL_STOP,
   [LINUX_SIGBUS] = USERSIG_DFL_CORE,
   [LINUX_SIGPOLL] = USERSIG_DFL_TERM,
   [LINUX_SIGPROF] = USERSIG_DFL_TERM,
   [LINUX_SIGSYS] = USERSIG_DFL_CORE,
   [LINUX_SIGTRAP] = USERSIG_DFL_CORE,
   [LINUX_SIGURG] = USERSIG_DFL_IGN,
   [LINUX_SIGVTALRM] = USERSIG_DFL_TERM,
   [LINUX_SIGXCPU] = USERSIG_DFL_CORE,
   [LINUX_SIGXFSZ] = USERSIG_DFL_CORE,
};

static VMK_ReturnStatus UserSigDispatchInFrame(VMKFullUserExcFrame* fullFrame, 
                                               UserSigId signum,
                                               UserSig_Handler handler,
                                               UserSigSet *blockedRestore);
static Bool UserSigIsBlocked(const UserSig_ThreadInfo* threadSigInfo, UserSigId sig);
static void UserSigBlock(UserSig_ThreadInfo* threadSigInfo, UserSigId sig);
static void UserSigAddPending(struct World_Handle* target,
                              UserSig_ThreadInfo* threadSigInfo,
                              UserSigId sig);
static void UserSigDropPending(UserSig_ThreadInfo* threadSigInfo, UserSigId sig);
static UserSigId UserSigFirstPendingUnblocked(UserSig_ThreadInfo* threadSigInfo);
static VMK_ReturnStatus UserSigWaitOnSignal(UserSig_ThreadInfo* threadSigInfo,
                                            CpuSched_WaitState waitState,
                                            Timer_RelCycles timeout);
static VMK_ReturnStatus UserSigInitKText(User_CartelInfo *uci);

/* Following two symbols are defined in userSigDispatch.S: */
extern void UserSigDispatch(void);
extern void UserSigDispatchEnd(void);

/*
 *----------------------------------------------------------------------
 *
 * UserSigIsQueuableSig --
 *
 *	Test is the given signal id is a "queueable" signal.  (I.e.,
 *	one of the RT signal numbers).
 *
 * Results:
 *	TRUE if queueable, FALSE otherwise
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
UserSigIsQueuableSig(UserSigId sig)
{
   ASSERT(sig < USERWORLD_NSIGNAL);
   return (sig >= LINUX_SIGRTMIN) && (sig < LINUX_SIGRTMAX);
}


/*
 *----------------------------------------------------------------------
 *
 * UserSigQueuedSigIndex --
 *
 *	Find the queue index for the given queueable signal.
 *
 * Results:
 *	An index for the rtSigPending array.
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static INLINE uint32
UserSigQueuedSigIndex(UserSigId sig)
{
   ASSERT(UserSigIsQueuableSig(sig));
   return sig - LINUX_SIGRTMIN;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSigThreadIsLocked --
 *
 *	Test if the given world's thread-private signal state is locked.
 *
 * Results:
 *	TRUE if thread-private signal lock is held.
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
UserSigThreadIsLocked(const UserSig_ThreadInfo* threadSigInfo)
{
   return SP_IsLocked(&threadSigInfo->lock);
}


/*
 *----------------------------------------------------------------------
 *
 * UserSigDefaultAction --
 *
 *	Get the UserSigDefault describing the default action (core, term,
 *	ignore, etc) for the given signal.
 *
 * Results:
 *	A valid UserSigDefault describing the behavior to take
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static INLINE UserSigDefault
UserSigDefaultAction(UserSigId sig)
{
   UserSigDefault f;
   ASSERT(sig > 0);
   ASSERT(sig < USERWORLD_NSIGNAL);

   /*
    * Signals not in the table default to _CORE.
    */

   if (sig >= ARRAYSIZE(userSigDefaultFlags)) {
      UWLOG(0, "sig %u not in table, defaults to core", sig);
      return USERSIG_DFL_CORE;
   }

   f = userSigDefaultFlags[sig];

   if (f == USERSIG_DFL_UNK) {
      UWLOG(0, "sig %u has unknown flags, defaults to core", sig);
      return USERSIG_DFL_CORE;
   }

   return f;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSigCartelIsLocked --
 *
 *	Test if the thread-shared signal state is locked.
 *
 * Results:
 *	TRUE if thread-shared signal lock is held.
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
UserSigCartelIsLocked(const UserSig_CartelInfo* cartelSigInfo)
{
   return SP_IsLocked(&cartelSigInfo->lock);
}


/*
 *----------------------------------------------------------------------
 *
 * UserSig_ThreadInit --
 *
 *	Initialize thread-private signal state.
 *
 * Results:
 *	VMK_OK
 *
 * Side effects:
 *	Lock allocated
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
UserSig_ThreadInit(User_ThreadInfo* uti)
{
   UWLOG(4, "started");

   SP_InitLock("UserSig_ThreadInfo", &uti->signals.lock, UW_SP_RANK_SIGTHREAD);
   uti->signals.pending = 0;
   uti->signals.blocked = 0;
   memset(&uti->signals.rtSigPending, 0, sizeof(uti->signals.rtSigPending));
   uti->signals.deathSig = 0;
   uti->signals.deathSigTarget = INVALID_WORLD_ID;
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSig_ThreadCleanup --
 *
 *	Undo UserSig_ThreadInit.
 *
 * Results:
 *	VMK_OK
 *
 * Side effects:
 *	Lock cleaned up
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
UserSig_ThreadCleanup(User_ThreadInfo* uti)
{
   uti->signals.pending = 0;
   uti->signals.blocked = 0;
   SP_CleanupLock(&uti->signals.lock);
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSig_CartelInit --
 *
 *	Initialize cartel-level shared signal state.
 *
 * Results:
 *	VMK_OK
 *
 * Side effects:
 *	Lock initialized
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
UserSig_CartelInit(User_CartelInfo* uci)
{
   VMK_ReturnStatus status;
   
   ASSERT(uci != NULL);
   memset(&uci->signals.sigActions, 0, sizeof(uci->signals.sigActions));
   /*
    * Note on lock ranks: The thread-private lock is higher rank than
    * this, the cartel-level lock.  If you acquire both locks, you must
    * acquire the thread-private one first.
    */
   SP_InitLock("UserSig_CartelInfo", &uci->signals.lock, UW_SP_RANK_SIGCARTEL);
   status = UserSigInitKText(uci);

   if (status != VMK_OK) {
      (void) UserSig_CartelCleanup(uci);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSig_CartelCleanup --
 *
 *	Undo UserSig_CartelInit
 *
 * Results:
 *	VMK_OK
 *
 * Side effects:
 *	Cleanup lock
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
UserSig_CartelCleanup(User_CartelInfo* uci)
{
   SP_CleanupLock(&uci->signals.lock);
   if (vmx86_debug) {
      memset(&uci->signals, 0xff, sizeof(UserSig_CartelInfo));
   }
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSig_FromIntelException --
 *
 *	Convert an intel exception number into a signal id.  Returns
 *	LINUX_SIG_ERR if no appropriate signal.
 *
 * Results:
 *	A signal number.
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
UserSigId
UserSig_FromIntelException(uint32 vector)
{
   UserSigId rc = LINUX_SIG_ERR;
   switch(vector) {
   case EXC_MF:
   case EXC_DE: 
   case EXC_XF:
   case EXC_NM:
      /* floating point, simd, or divide exception is a FPE */
      rc = LINUX_SIGFPE;
      break;
   case EXC_BP:
   case EXC_DB:
      /*
       * Debugger exceptions should have been caught and handled by
       * our semi-inkernel debugger already.  
       */
      rc = LINUX_SIGTRAP;
      break;
   case EXC_NMI:
   case EXC_TS:
   case EXC_NP:
   case EXC_MC:
   case EXC_DF:
      /* None of these have a mapping in sysv i386 ABI */
      rc = LINUX_SIG_ERR;
      break;
   case EXC_UD:
      /* Invalid opcode is illegal instruction */
      rc = LINUX_SIGILL;
      break;
   case EXC_OF:
   case EXC_BR:
   case EXC_SS:
   case EXC_GP:
   case EXC_PF:
   case EXC_AC:
      /* Overflow, range checks, segment errors, stack/general/page faults */
      rc = LINUX_SIGSEGV;
      break;
   }
   return rc;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSigThreadLock --
 *
 *	Lock given thread-private signal state.  Thread-private lock
 *	has lower rank than cartel-level lock, so acquire
 *	thread-private first.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Lock acquired.
 *
 *----------------------------------------------------------------------
 */
static INLINE void 
UserSigThreadLock(UserSig_ThreadInfo* threadSigInfo)
{
   ASSERT(threadSigInfo != NULL);
   SP_Lock(&threadSigInfo->lock);
}


/*
 *----------------------------------------------------------------------
 *
 * UserSigThreadUnlock --
 *
 *	Unlock given thread-private signal state
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Lock released.
 *
 *----------------------------------------------------------------------
 */
static INLINE void 
UserSigThreadUnlock(UserSig_ThreadInfo* threadSigInfo)
{
   ASSERT_SIGTHREADLOCKED(threadSigInfo);
   SP_Unlock(&threadSigInfo->lock);
}


/*
 *----------------------------------------------------------------------
 *
 * UserSig_CartelLock --
 *
 *	Lock given cartel-level signal state.  Thread-private lock has
 *	lower rank than shared lock, so get thread-private lock first.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Lock acquired.
 *
 *----------------------------------------------------------------------
 */
void 
UserSig_CartelLock(UserSig_CartelInfo* cartelSigInfo)
{
   ASSERT(cartelSigInfo != NULL);
   SP_Lock(&cartelSigInfo->lock);
}


/*
 *----------------------------------------------------------------------
 *
 * UserSig_CartelUnlock --
 *
 *	Unlock given cartel-level signal state.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Lock released.
 *
 *----------------------------------------------------------------------
 */
void 
UserSig_CartelUnlock(UserSig_CartelInfo* cartelSigInfo)
{
   ASSERT_CARTELSIGLOCKED(cartelSigInfo);
   SP_Unlock(&cartelSigInfo->lock);
}


/*
 *----------------------------------------------------------------------
 *
 * UserSigIsBlocked --
 *
 *	Test given thread-private signal state to see if given signal
 *	is blocked.
 *
 *	Caller doesn't need thread-private signal lock because only
 * 	the owner is allowed to manipulate the blocked signal mask.
 *
 * Results:
 *	TRUE if given signal is blocked.
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static Bool 
UserSigIsBlocked(const UserSig_ThreadInfo* threadSigInfo,
                 UserSigId sig)
{
   ASSERT(sig > 0);
   ASSERT(sig < USERWORLD_NSIGNAL);
   ASSERT(threadSigInfo == &MY_USER_THREAD_INFO->signals);
   return (threadSigInfo->blocked & UserSig_IdToMask(sig)) != USERSIGSET_EMPTY;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSigBlock --
 *
 *	Add the given signal to the set of blocked signals in the
 *	given signal state.
 *
 *	Caller doesn't need thread-private signal lock because only
 * 	the owner is allowed to manipulate the blocked signal mask.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Given signal is blocked.
 *
 *----------------------------------------------------------------------
 */
static INLINE void 
UserSigBlock(UserSig_ThreadInfo* threadSigInfo, UserSigId sig)
{
   ASSERT(sig > 0);
   ASSERT(sig < USERWORLD_NSIGNAL);
   ASSERT(threadSigInfo == &MY_USER_THREAD_INFO->signals);
   threadSigInfo->blocked |= UserSig_IdToMask(sig);
}


/*
 *----------------------------------------------------------------------
 *
 * UserSig_SetBlocked --
 *
 *	Set the blocked signal mask for the given signal
 *	state.
 *
 *	Caller doesn't need thread-private signal lock because only
 * 	the owner is allowed to manipulate the blocked signal mask.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Blocked signal mask for associated world is changed
 *
 *----------------------------------------------------------------------
 */
void
UserSig_SetBlocked(UserSig_ThreadInfo* threadSigInfo, UserSigSet blocked)
{
   ASSERT(threadSigInfo);
   ASSERT(threadSigInfo == &MY_USER_THREAD_INFO->signals);
   threadSigInfo->blocked = blocked;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSig_GetBlocked --
 *
 *	Get the blocked signal mask (into oset)
 *
 *	Caller doesn't need thread-private signal lock because only
 * 	the owner is allowed to manipulate the blocked signal mask.
 *
 * Results:
 *	oset is set to the blocked signal mask
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
void
UserSig_GetBlocked(const UserSig_ThreadInfo* threadSigInfo, UserSigSet* oset)
{
   ASSERT(threadSigInfo);
   ASSERT(oset);
   ASSERT(threadSigInfo == &MY_USER_THREAD_INFO->signals);
   *oset = threadSigInfo->blocked;
}

/*
 *----------------------------------------------------------------------
 *
 * UserSigAnyPendingUnblocked --
 *
 *	Test to see if there are any pending, unblocked signals in the
 *	given signal state.
 *
 *	Caller needs the thread-private signal lock to get a
 *	consistent pending signal mask.
 *	
 * Results:
 *	TRUE if there are pending, unblocked signals awaiting delivery.
 *
 * Side effects:
 * 	None
 *
 *----------------------------------------------------------------------
 */
static Bool
UserSigAnyPendingUnblocked(const UserSig_ThreadInfo* threadSigInfo)
{
   ASSERT(threadSigInfo == &MY_USER_THREAD_INFO->signals);
   ASSERT_SIGTHREADLOCKED(threadSigInfo);
   return (threadSigInfo->pending & (~threadSigInfo->blocked)) != USERSIGSET_EMPTY;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSigAddPending --
 *
 *	Add the given signal to the pending signal set for the given
 *	signal state.  Properly queues queueable signals.  Caller must
 *	hold the thread-level signal lock.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Signal is added to pending signal list.
 *
 *----------------------------------------------------------------------
 */
static INLINE_SINGLE_CALLER void
UserSigAddPending(World_Handle* target, UserSig_ThreadInfo* threadSigInfo, UserSigId sig)
{
   ASSERT(threadSigInfo != NULL);
   ASSERT(sig > 0);
   ASSERT(sig < USERWORLD_NSIGNAL);
   ASSERT_SIGTHREADLOCKED(threadSigInfo);

   if (UserSigIsQueuableSig(sig)) {
      const uint32 idx = UserSigQueuedSigIndex(sig);
      threadSigInfo->rtSigPending[idx]++;
      UWLOG(((sig == LINUX_SIGPROF) ? 5 : 1),
            "Add to pending queue for sig %d on world %d (now %d pending)",
            sig, target->worldID,
            threadSigInfo->rtSigPending[idx]);
   }
   threadSigInfo->pending |= UserSig_IdToMask(sig);
}


/*
 *----------------------------------------------------------------------
 *
 * UserSigDropPending --
 *
 *	Drop the given signal from the pending signal mask in the
 *	given signal state.  If the signal is queuable, the count is
 *	decremented by one.  Caller must hold the thread-level signal
 *	lock.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	One less pending signal to deal with.
 *
 *----------------------------------------------------------------------
 */
static INLINE_SINGLE_CALLER void
UserSigDropPending(UserSig_ThreadInfo* threadSigInfo, UserSigId sig)
{
   ASSERT(sig > 0);
   ASSERT(sig < USERWORLD_NSIGNAL);
   ASSERT(threadSigInfo == &MY_USER_THREAD_INFO->signals);
   ASSERT_SIGTHREADLOCKED(threadSigInfo);
   
   if (UserSigIsQueuableSig(sig)) {
      const uint32 idx = UserSigQueuedSigIndex(sig);
      int nct;
      ASSERT(threadSigInfo->rtSigPending[idx] > 0);
      nct = --threadSigInfo->rtSigPending[idx];
      UWLOG(((sig == LINUX_SIGPROF) ? 5 : 1),
            "Dropped pending queue for %d (now %d pending)",
            sig, threadSigInfo->rtSigPending[idx]);
      if (nct > 0) {
         return;
      }
      /* else nct == 0, fall through and clear mask bit */
   }
   threadSigInfo->pending &= ~(UserSig_IdToMask(sig));
}


/*
 *----------------------------------------------------------------------
 *
 * UserSigFirstPendingUnblocked --
 *
 *	Return the first pending, unblocked signal in the given signal
 *	state.  Returns 0 if no signal is pending.
 *
 *	Caller needs thread-private signal lock to get a consistent
 *	pending signal mask.
 *
 * Results:
 *	Pending signal id.
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static INLINE_SINGLE_CALLER UserSigId
UserSigFirstPendingUnblocked(UserSig_ThreadInfo* threadSigInfo)
{
   UserSigSet pendingUnblocked;
   UserSigId i;
   
   ASSERT(threadSigInfo == &MY_USER_THREAD_INFO->signals);
   ASSERT_SIGTHREADLOCKED(threadSigInfo);

   pendingUnblocked = threadSigInfo->pending & (~threadSigInfo->blocked);
   /*
    * This could be a slicker search using ffsll()
    */
   for (i = 0; i < USERWORLD_NSIGNAL; i++) {
      UserSigSet s = UserSig_IdToMask(i);
      if ((pendingUnblocked & s) != 0) {
         return i;
      }
   }
   return LINUX_SIG_ERR;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSig_GetSigHandler --
 *
 *	Get the signal handler for the given signal id in the given
 *	shared state.  Caller must hold the shared signal lock.
 *
 * Results:
 *	A handler.
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
UserSig_Handler 
UserSig_GetSigHandler(const UserSig_CartelInfo* cartelSigInfo, UserSigId sig)
{
   ASSERT_CARTELSIGLOCKED(cartelSigInfo);
   ASSERT(sig < USERWORLD_NSIGNAL);
   ASSERT(sig != LINUX_SIG_ERR);
   return cartelSigInfo->sigActions[sig].handler;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSig_SetSigHandler --
 *
 *	Set the signal handler for the given signal in the given
 *	shared signal state.  Overwrites existing handler.  Caller
 *	must hold the cartel-level signal lock.
 *
 * Results:
 *
 * Side effects:
 *
 *----------------------------------------------------------------------
 */
void
UserSig_SetSigHandler(UserSig_CartelInfo* cartelSigInfo,
                      UserSigId sig,
                      UserSig_Handler handler)
{
   ASSERT_CARTELSIGLOCKED(cartelSigInfo);
   ASSERT(sig < USERWORLD_NSIGNAL);
   cartelSigInfo->sigActions[sig].handler = handler;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSig_GetSigMask --
 *
 *	Get the blocked signal mask that will be used during the run
 *	of the given signal.  This is cartel-level state, so caller
 *	must hold the cartel-level signal lock.
 *
 * Results:
 *	The mask in effect during handlers for the given signal
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
UserSigSet
UserSig_GetSigMask(const UserSig_CartelInfo* cartelSigInfo, UserSigId sig)
{
   ASSERT_CARTELSIGLOCKED(cartelSigInfo);
   ASSERT(sig < USERWORLD_NSIGNAL);
   return cartelSigInfo->sigActions[sig].mask;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSig_SetSigMask --
 *
 *	Set the blocked signal mask to be used when the given signal's
 *	handler is run. This is cartel-level state, so caller must
 *	hold the cartel-level signal lock.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Sets the mask for the given signal
 *
 *----------------------------------------------------------------------
 */
void 
UserSig_SetSigMask(UserSig_CartelInfo* cartelSigInfo, UserSigId sig, UserSigSet mask)
{
   ASSERT_CARTELSIGLOCKED(cartelSigInfo);
   ASSERT(sig < USERWORLD_NSIGNAL);
   cartelSigInfo->sigActions[sig].mask = mask;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSig_IsOneShot --
 *
 *	Test the given signal to see if its a one-shot handler (i.e.,
 *	it won't be restored after firing). This is cartel-level
 *	state, so caller must hold the cartel-level signal lock.
 *
 * Results:
 *	TRUE if the given signal has a one-shot handler.
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
Bool 
UserSig_IsOneShot(const UserSig_CartelInfo* cartelSigInfo, UserSigId sig)
{
   ASSERT_CARTELSIGLOCKED(cartelSigInfo);
   ASSERT(sig < USERWORLD_NSIGNAL);
   return (cartelSigInfo->sigActions[sig].flags & USERSIGACT_FLAG_ONESHOT);
}


/*
 *----------------------------------------------------------------------
 *
 * UserSig_SetOneShot --
 *
 *	Set the "one-shot" flag on the given signal to the value of
 *	enable. This is cartel-level state, so caller must hold the
 *	cartel-level signal lock.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	May twiddle one-shot flag in given signal's action struct.
 *
 *----------------------------------------------------------------------
 */
void 
UserSig_SetOneShot(UserSig_CartelInfo* cartelSigInfo, UserSigId sig, Bool enable)
{
   UserSigAction* sigAct;

   ASSERT_CARTELSIGLOCKED(cartelSigInfo);
   ASSERT(sig < USERWORLD_NSIGNAL);
   sigAct = &(cartelSigInfo->sigActions[sig]);
   if (enable) {
      sigAct->flags |= USERSIGACT_FLAG_ONESHOT;
   } else {
      sigAct->flags &= (~USERSIGACT_FLAG_ONESHOT);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * UserSig_IsReentrant --
 *
 *	Test if the handler for the given signal will be "reentrant"
 *	(that is if the given signal can be delivered while the
 *	handler for the signal is running). This is cartel-level
 *	state, so caller must hold the cartel-level signal lock.
 *
 * Results:
 *	TRUE if the handler is reentrant
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
Bool 
UserSig_IsReentrant(const UserSig_CartelInfo* cartelSigInfo, UserSigId sig)
{
   ASSERT_CARTELSIGLOCKED(cartelSigInfo);
   ASSERT(sig < USERWORLD_NSIGNAL);
   return (cartelSigInfo->sigActions[sig].flags & USERSIGACT_FLAG_REENTRANT);
}


/*
 *----------------------------------------------------------------------
 *
 * UserSig_SetReentrant --
 *
 *	Set the reentrant flag for the given signal.  See
 *	UserSig_IsReentrant.  This is cartel-level state, so caller
 *	must hold the cartel-level signal lock.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	reentrancy flag may be twiddled for given signal
 *
 *----------------------------------------------------------------------
 */
void 
UserSig_SetReentrant(UserSig_CartelInfo* cartelSigInfo, UserSigId sig, Bool enable)
{
   UserSigAction* sigAct;

   ASSERT_CARTELSIGLOCKED(cartelSigInfo);
   ASSERT(sig < USERWORLD_NSIGNAL);
   sigAct = &(cartelSigInfo->sigActions[sig]);
   if (enable) {
      sigAct->flags |= USERSIGACT_FLAG_REENTRANT;
   } else {
      sigAct->flags &= (~USERSIGACT_FLAG_REENTRANT);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * UserSig_SendDeathSignal --
 *
 *	Send the given world's death signal to it's death signal
 *	target (if its defined).  See LinuxThread_CloneStart.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	If death signal target is defined, a signal is sent to it.
 *
 *----------------------------------------------------------------------
 */
void
UserSig_SendDeathSignal(World_Handle* dyingWorld)
{
   UserSig_ThreadInfo* const threadSigInfo = &dyingWorld->userThreadInfo->signals;
   World_ID target = 0;
   UserSigId sig = 0;

   ASSERT(dyingWorld == MY_RUNNING_WORLD);

   target = threadSigInfo->deathSigTarget;
   sig = threadSigInfo->deathSig;
   
   if (target != 0) {
      World_Handle *targetWorld; // generally the main thread in cartel
      
      targetWorld = World_Find(target);
      if (targetWorld != NULL) {
         if (World_IsUSERWorld(targetWorld)
             && (targetWorld != dyingWorld)) {
            UWLOG(2, "Sending death signal %d to world %d (on behalf of %d)",
                  sig, target, dyingWorld->worldID);
            UserSig_Send(targetWorld, sig);
         } else {
            UWLOG(0, "Death signal (%d) not sent: target world (%d) not a UserWorld",
                  sig, target);
         }
         World_Release(targetWorld);
      } else {
         UWLOG(0, "Death signal (%d) not sent: target world (%d) not a valid target",
               sig, target);
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * UserSigWaitOnSignal --
 *
 *	Have the current world wait, with the given waitState, on the
 *	given signal struct (which must be associated with the current
 *	world --- worlds can only wait on their own signal struct).
 *	Will be awoken when a signal is sent to this thread
 *
 * Results:
 *	UserThread_Wait return value
 *
 * Side effects:
 *	Thread goes to sleep, drops thread-private signal lock, and
 *	reacquires it after waking up.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserSigWaitOnSignal(UserSig_ThreadInfo* threadSigInfo,
                    CpuSched_WaitState waitState,
                    Timer_RelCycles timeout)
{
   ASSERT(threadSigInfo == &MY_USER_THREAD_INFO->signals);
   ASSERT_SIGTHREADLOCKED(threadSigInfo);

   return UserThread_Wait((uint32)threadSigInfo, waitState,
                   &threadSigInfo->lock, timeout, UTWAIT_WITHOUT_PREPARE);
}


/*
 *----------------------------------------------------------------------
 *
 * UserSigHandleOneSignal --
 *
 *	Handle given pending signal.
 *
 *	threadSigInfo must be locked.  It *will* be unlocked.
 *
 * Results:
 *	VMK_OK if signal was dispatched.
 *	VMK_NO_SIGNAL_HANDLER if not ignorable, and would be ignored
 *
 * Side effects:
 *	May munge fullFrame (user mode register state) to reflect a signal
 *	dispatch.  Or, may tag the current cartel as terminated if the
 *	signal is unhandled and fatal.  Unlocks threadSigInfo.
 *
 *----------------------------------------------------------------------
 */
static void
UserSigHandleOneSignal(UserSigId sig,
                       UserSig_ThreadInfo* threadSigInfo,
                       UserSig_CartelInfo* cartelSigInfo,
                       VMKFullUserExcFrame* fullFrame,
                       UserSigSet *restoreMask,
                       Bool canIgnore)
{
   UserSig_Handler handler;
   Bool ignored;
   Bool fatal;
   Bool wantCoreDump;
   Bool reentrant;

   ASSERT(sig != LINUX_SIG_ERR);
   ASSERT(threadSigInfo == &MY_USER_THREAD_INFO->signals);
   ASSERT(cartelSigInfo == &MY_USER_CARTEL_INFO->signals);
   ASSERT(restoreMask != NULL);

   ASSERT(UserSigThreadIsLocked(threadSigInfo));
   ASSERT(! UserSigIsBlocked(threadSigInfo, sig));
   ASSERT((UserSig_IdToMask(sig) & threadSigInfo->pending) != USERSIGSET_EMPTY);

   UWLOG((sig == LINUX_SIGPROF) ? 5 : 2, "sig=%d %s", sig,
         canIgnore ? "ignorable":"unignorable");

   /*
    * Clear the signal.  It is "handled" as far as I'm concerned.
    *
    * Yes, even if the dispatch fails.  (Since that implies that the
    * default handler "handled" the signal.)
    */
   UserSigDropPending(threadSigInfo, sig);

   UserSig_CartelLock(cartelSigInfo);
   handler = UserSig_GetSigHandler(cartelSigInfo, sig);
   reentrant = UserSig_IsReentrant(cartelSigInfo, sig);
   UserSig_CartelUnlock(cartelSigInfo);

   /*
    * Now that we've dropped the pending signal and fetched the handler
    * we're going to use, we don't need the threadSigInfo lock.  The
    * remaining uses of threadSigInfo manipulate the blocked signal state,
    * which is thread-private and so doesn't need a lock.
    */
   UserSigThreadUnlock(threadSigInfo);

   /*
    * Users are not allowed to set handlers for SIGKILL|SIGSTOP, see
    * LinuxSignal_RTSigaction.
    */
   if ((sig == LINUX_SIGKILL) || (sig == LINUX_SIGSTOP)) {
      ASSERT(handler == LINUX_SIG_DFL);
      handler = LINUX_SIG_DFL;
   }

   /*
    * Check for default/ignored handlers.  For fatal default handlers,
    * check the core dump requirement, too.
    */
   fatal = FALSE;
   ignored = FALSE;
   wantCoreDump = FALSE;
   if (handler == LINUX_SIG_IGN) {
      UWLOG((sig == LINUX_SIGPROF) ? 6 : 3, "%d handler is SIG_IGN", sig);
      ignored = TRUE;
   } else if (handler == LINUX_SIG_DFL) {
      // Handle default action
      switch(UserSigDefaultAction(sig)) {
      case USERSIG_DFL_TERM:
         fatal = TRUE;
         wantCoreDump = FALSE;
         break;
      case USERSIG_DFL_CORE:
         fatal = TRUE;
         wantCoreDump = TRUE;
         break;
      case USERSIG_DFL_STOP:
         UWWarn("Default signal behavior 'Stop' not implemented.  Fatal signal.");
         fatal = TRUE;
         wantCoreDump = TRUE;
         break;
      case USERSIG_DFL_IGN:
         ignored = TRUE;
         break;
      default:
         UWWarn("Unknown default action %#x.  Fatal.", UserSigDefaultAction(sig));
         ASSERT(FALSE);
         fatal = TRUE;
         wantCoreDump = TRUE;
         break;
      }
      UWLOG((sig == LINUX_SIGPROF) ? 6 : 3,
            "%d handler is default (%d: %s %s %s)", sig, UserSigDefaultAction(sig),
            ignored ? "ignored" : "handled", fatal ? "fatal" : "non-fatal",
            wantCoreDump ? "with-core" : "core-free");
   } else {
      UWLOG((sig == LINUX_SIGPROF) ? 6 : 3, "handler is %#x", handler);
   }

   /*
    * "Handle" ignored signals by ignoring or making it fatal.
    */
   if (ignored) {
      if (canIgnore) {
         /*
          * If it really was handled, sigreturn would've restored the
          * restoreMask.  Do that and we can pretend a dispatch happened.
          */
         UWLOG((sig == LINUX_SIGPROF) ? 6 : 3,
               "Ignoring %d.  Restoring mask %#Lx", sig, *restoreMask);
         UserSig_SetBlocked(threadSigInfo, *restoreMask);
         
         return;
      } else {
         /*
          * If not allowed to ignore this signal (E.g., its from a processor
          * exception), then the signal is fatal.
          */
         UWLOG((sig == LINUX_SIGPROF) ? 6 : 3,
               "Can't ignore %d.  Fatal (with core)", sig);
         fatal = TRUE;
         wantCoreDump = TRUE;
      }
   }

   if (! fatal) {
      VMK_ReturnStatus status;

      /*
       * Block signal if not flagged as reentrant (SA_NOMASK). Caller must
       * have already saved off the current signal mask for post-signal
       * restoration.
       */
      if (! reentrant) {
         UWLOG((sig == LINUX_SIGPROF) ? 6 : 3,
               "Blocking signal %d during handler (not reentrant)", sig);
         UserSigBlock(threadSigInfo, sig);
      }      

      status = UserSigDispatchInFrame(fullFrame, sig, handler, restoreMask);
      if (status != VMK_OK) {
         wantCoreDump = TRUE;
         fatal = TRUE;
      }
   }
   
   /*
    * Kill this cartel from error during dispatch or fatal default
    * handler.
    */
   if (fatal) {
      /* Tag this cartel for shutdown */
      User_CartelShutdown(CARTEL_EXIT_SYSERR_BASE+sig,
                          wantCoreDump, fullFrame);

      /* If it really was handled, sigreturn would've restored the restoreMask */
      UserSig_SetBlocked(threadSigInfo, *restoreMask);
   }

   /*
    * At this point, either the usermode register state (fullFrame) was
    * munged to reflect a signal dispatch, or the cartel has been tagged
    * for termination.
    */
}


/*
 *----------------------------------------------------------------------
 *
 * UserSig_Suspend --
 *
 *	Suspend the current world until a signal occurs.  The world
 *	will suspend with the given blocked signal set and shall not
 *	return until an unblocked signal is delivered.
 *
 * Results:
 *	None.
 * 
 * Side effects:
 *	Current world probably sleeps for a while.
 *
 *----------------------------------------------------------------------
 */
int
UserSig_Suspend(UserSig_ThreadInfo* threadSigInfo,
                UserSigSet blocked,
                VMKFullUserExcFrame* fullFrame,
                int returnCode)
{
   UserSig_CartelInfo* const cartelSigInfo = &MY_USER_CARTEL_INFO->signals;
   UserSigId handledSigNum = LINUX_SIG_ERR;
   UserSigSet oset;
   
   ASSERT(threadSigInfo == &MY_USER_THREAD_INFO->signals);

   /*
    * Switch to the given blocked signal set.  Save the old blocked
    * signal set off to be restored later.
    */
   UserSig_GetBlocked(threadSigInfo, &oset);
   UserSig_SetBlocked(threadSigInfo, blocked);
   
   /* Grab lock to protect pending signals mask */
   UserSigThreadLock(threadSigInfo);

   /*
    * If no pending signals under the new mask go to sleep until
    * something arrives.
    */
   if (! UserSigAnyPendingUnblocked(threadSigInfo)) {
      VMK_ReturnStatus suspendStatus;
      UWLOG(2, "Waiting for unblkd sigs (p=%#Lx, b=%#Lx, ob=%#Lx)",
            threadSigInfo->pending, blocked, oset);
      suspendStatus = UserSigWaitOnSignal(threadSigInfo, CPUSCHED_WAIT_UW_SIGWAIT, 0);
      ASSERT(suspendStatus == VMK_WAIT_INTERRUPTED);
   }

   /*
    * Normally we return up to the syscall layer to dispatch signals
    * (sigsuspend is special because of the blocked signal mask).
    * Since we prep usermode for a signal dispatch here, we need to
    * store the sigsuspend return code, so the dispatch can save it
    * off for post-handler restoration.
    */
   fullFrame->regs.eax = returnCode;

   /*
    * We may have been woken from the sleep because a signal arrived,
    * or we're just being kicked out to the entry layer (died,
    * debugger, etc.)  So, re-check for unblocked signals.
    */
   if (UserSigAnyPendingUnblocked(threadSigInfo)) {
      
      UWLOG(2, "Dispatching pending signal(s)");

      handledSigNum = UserSigFirstPendingUnblocked(threadSigInfo);
      ASSERT(handledSigNum != LINUX_SIG_ERR);
      
      // May terminate cartel or may munge fullFrame
      UserSigHandleOneSignal(handledSigNum, threadSigInfo, cartelSigInfo,
                             fullFrame, &oset, TRUE);
   } else {
      UserSigThreadUnlock(threadSigInfo);

      /*
       * Spurious wakeup or termination wakeup.  Restore the blocked
       * signal mask.
       */
      UserSig_SetBlocked(threadSigInfo, oset);
   }

   /*
    * Probably munged by UserSigDispatchInFrame.
    */
   return fullFrame->regs.eax;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSig_LookupAndSend --
 *
 *	Send the given signal to the given world.  If the world is
 *	found and is a UserWorld and (optionally) in the same cartel
 *	as the current world (the sender) invokes UserSig_Send on the
 *	world.
 *
 * Results:
 *	VMK_OK if the signal was sent; VMK_BAD_PARAM if the given
 *	world is not in the same cartel as the sender (if sameCartel
 *	is true); VMK_NOT_FOUND if the given world is no longer active
 *	or is not a UserWorld.
 *
 * Side effects:
 *	Signal is added to pending list for target, and target is
 *	woken if sleeping.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserSig_LookupAndSend(World_ID id,
                      UserSigId sig,
                      Bool sameCartel)
{
   World_Handle* target;
   VMK_ReturnStatus status;

   target = World_Find(id);
   if (target != NULL) {
      if (sig == 0) {
        /*
         * If sig == 0, we're just checking that the id
         * is still valid. VThreadHostProbeThread() uses
         * kill(pid, 0) to check for thread liveness.
         */
        status = VMK_OK;
      } else if (sameCartel
          && (target->userCartelInfo != MY_USER_CARTEL_INFO)) {
         status = VMK_BAD_PARAM;
      } else if (!World_IsUSERWorld(target)) {
         status = VMK_NOT_FOUND;
      } else {
         UserSig_Send(target, sig);
         status = VMK_OK;
      }
      World_Release(target);
   } else {
      status = VMK_NOT_FOUND;
   }
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSig_Send --
 *
 *	Send the given signal to the given world.  The signal will be
 *	added to the pending signal mask for the target, and the
 *	target will be kicked in case its waiting (i.e., in sigSuspend
 *	or in sleep).  If the target is not waiting, we rely on the
 *	fact that it will eventually check its pending signal mask and
 *	handle the signal.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Signal is added to pending list for target, and target is
 *	woken if sleeping.
 *
 *----------------------------------------------------------------------
 */
void 
UserSig_Send(World_Handle* target, UserSigId sig)
{
   UserSig_ThreadInfo* const targetThreadSigInfo = &target->userThreadInfo->signals;
   ASSERT(target);

   UWLOG((sig == LINUX_SIGPROF) ? 5 : 1,
         "(target=%d, sig=%d)", target->worldID, sig);
   UWSTAT_ARRINC(signalsSent, sig);

   UserSigThreadLock(targetThreadSigInfo);
   UserSigAddPending(target, targetThreadSigInfo, sig);

   /*
    * Make the target at least check its pending signal status
    * (only if its interrupted).
    *
    * It is required that we do this before doing the Wakeup (actually
    * before grabbing the waitInfo lock).  See UserThread_Wait
    */
   targetThreadSigInfo->pendingBit = 1;
   UserThread_WakeupWorld(target, UTW_BACKOUT);
   UserSigThreadUnlock(targetThreadSigInfo);
   /*
    * If target world is off spinning in user mode (e.g., in an
    * infinite loop) then we'll dispatch the signal during the next
    * timer interrupt that takes the CPU from the world (see
    * User_InterruptCheck and UserSig_InterruptCheck).
    */
}   


/*
 *----------------------------------------------------------------------
 *
 * UserSig_HandlePending --
 *
 *	Handle any one pending, unblocked signal in the given (current
 *	world's) thread-private signal state.  Normally, this function
 *	will return without changing anything (no signal to dispatch)
 *	or after having munged the given fullFrame to jump into the
 *	dispatched signal's handler.
 *
 *	May schedule the cartel for termination if signal dispatch fails,
 *	or no handler is registered.
 *
 *	Must be safe to run in a bottom half.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Dispatch one pending, unblocked signal.  Munges fullFrame.
 *
 *----------------------------------------------------------------------
 */
void
UserSig_HandlePending(UserSig_ThreadInfo* threadSigInfo,
                      VMKFullUserExcFrame* fullFrame)
{
   
   ASSERT(threadSigInfo == &MY_USER_THREAD_INFO->signals);

   UserSigThreadLock(threadSigInfo);

   if (UNLIKELY(UserSigAnyPendingUnblocked(threadSigInfo))) {
      UserSigId handledSigNum;
      UserSigSet currentMask;
      
      UWLOG(2, "Have pending signals (pending=%#Lx blocked=%#Lx)",
            threadSigInfo->pending, threadSigInfo->blocked);

      handledSigNum = UserSigFirstPendingUnblocked(threadSigInfo);
      ASSERT(handledSigNum != LINUX_SIG_ERR);
      
      // for restoration after dispatch or error
      UserSig_GetBlocked(threadSigInfo, &currentMask);

      // May terminate cartel or may munge fullFrame
      UserSigHandleOneSignal(handledSigNum, threadSigInfo,
                             &MY_USER_CARTEL_INFO->signals,
                             fullFrame, &currentMask, TRUE);
   } else {
      UWLOG(6, "Have no pending, unblocked signals (pending=%#Lx blocked=%#Lx)",
            threadSigInfo->pending, threadSigInfo->blocked);
      threadSigInfo->pendingBit = 0;
      UserSigThreadUnlock(threadSigInfo);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * UserSig_HandleVector --
 *	Try to find a signal handler to dispatch to for the given x86
 *	exception vector.  Munge the given (user) fullFrame to jump to
 *	the handler if possible.
 *
 * Results:
 *	VMK_OK if fullFrame was munged to handle the vector, otherwise
 *	if no handler or dispatch failed.
 *
 * Side effects:
 *	fullFrame might be munged to jump into the signal handler.
 * 	User mode stack will get whacked on, too.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserSig_HandleVector(World_Handle* currWorld,
                     int vector,
                     VMKFullUserExcFrame* fullFrame)
{
   UserSigId sig;
   UserSig_CartelInfo* const cartelSigInfo = &currWorld->userCartelInfo->signals;
   UserSig_ThreadInfo* const threadSigInfo = &currWorld->userThreadInfo->signals;
   UserSigSet currentMask;

   ASSERT(currWorld == MY_RUNNING_WORLD);
   sig = UserSig_FromIntelException(vector);

   /* Fail if no mapping from this exception to a user signal */
   if (sig == LINUX_SIG_ERR) {
      UWLOG(1, "vector=%d has no mapping to a signal", vector);
      return VMK_NO_SIGNAL_HANDLER;
   }

   if (UserSigIsBlocked(threadSigInfo, sig)) {
      UWLOG(1, "signal %d (for vector=%d) is blocked", sig, vector);
      /*
       * Caller will terminate the cartel because of this (or drop into
       * the debugger), so there's no need to flag a pending signal or
       * anything.
       */
      return VMK_FATAL_SIGNAL_BLOCKED;
   }

   UWLOG(2, "signal %d being dispatched (vector=%d)", sig, vector);
   
   UserSigThreadLock(threadSigInfo);
   UserSigAddPending(currWorld, threadSigInfo, sig); // for consistency

   // for restoration after dispatch
   UserSig_GetBlocked(threadSigInfo, &currentMask);

   // May terminate cartel or may munge fullFrame
   UserSigHandleOneSignal(sig, threadSigInfo, cartelSigInfo,
                          fullFrame, &currentMask, FALSE);
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSig_CopyChunk --
 *
 *	Copy a chunk of data onto the usermode stack at the given
 *	*esp.  Bump *esp to reflect size of data copied on.
 * 
 * Results:
 *	Result of User_CopyOut
 *
 * Side effects:
 *	Chunk is copied to user-mode at *esp
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserSig_CopyChunk(UserVA* esp,           // IN/OUT: user stack addr
                  size_t chunkSize,      // IN
                  const void* chunkData, // IN
                  const char* logname)   // IN
{
   VMK_ReturnStatus status;

   *esp -= chunkSize;
   *esp = ALIGN_DOWN(*esp, sizeof(int32));
   status = User_CopyOut(*esp, chunkData, chunkSize);
   if (status == VMK_OK) {
      UWLOG(3, "Copied %s (%u bytes) from (kernel) %p to (user) %#x",
            logname, chunkSize, chunkData, *esp);
   } else {
      UWLOG(0, "Error copying %s (%d bytes) from (kernel) %p to (user) %#x: %s",
            logname, chunkSize, chunkData, *esp,
            VMK_ReturnStatusToString(status));
   }
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSigDispatchInFrame --
 *
 *	Dispatch the given signal with the given handler in the given
 *	fullFrame.  To do the dispatch we setup the user mode stack
 *	for a call to a stub function that will call the handler then
 *	call sigreturn.  (The stub function is part of the ktext page.)
 *	We also put the restore context to be passed to sigreturn on
 *	the stack.  The fullFrame is modified to represent the new
 *	stack state and to jump into the stub.
 *
 *	See UserSig_ReturnFromHandler for the return path.
 * 
 * Results:
 *	VMK_OK if stack and fullFrame were munged to effect the dispatch
 *	to the handler.  Or, a User_CopyOut error if copying state to the
 *	user-mode stack caused an error.
 *
 * Side effects:
 * 	Lots.  A couple hundred bytes are copied out to the user mode
 * 	stack, the given fullFrame is munged to jump into a user mode
 * 	signal handler.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserSigDispatchInFrame(VMKFullUserExcFrame* fullFrame,
                       UserSigId sig,
                       UserSig_Handler handler,
                       UserSigSet* restoreMask)
{
   UserSigRestoreContext restoreContext;
   VMK_ReturnStatus status = VMK_OK;
   UserVA esp;
   Reg32 baseAddr;
   Reg32 contextAddr;

   ASSERT(fullFrame);

   /*
    * Fetch existing stack pointer.  Make sure its aligned before
    * pushing anything.
    */
   esp = ALIGN_DOWN((UserVA)fullFrame->frame.esp, sizeof(int32));

   ASSERT(handler != LINUX_SIG_IGN); // handled before this point
   ASSERT(handler != LINUX_SIG_DFL); // handled before this point
   
   UWLOG(1, "Dispatching to handler @%#x for signal %d", handler, sig);

   UWLOG_FullExcFrame(3, "Fault Frame", NULL, fullFrame, NULL);
   
   // Standard bits of restoreContext:
   restoreContext.gs = (uint16)fullFrame->regs.gs;
   restoreContext.fs = (uint16)fullFrame->regs.fs;
   restoreContext.es = (uint16)fullFrame->regs.es;
   restoreContext.ds = (uint16)fullFrame->regs.ds;
   restoreContext.edi = fullFrame->regs.edi;
   restoreContext.esi = fullFrame->regs.esi;
   restoreContext.ebp = fullFrame->regs.ebp;
   restoreContext.esp = fullFrame->frame.esp;
   restoreContext.ebx = fullFrame->regs.ebx;
   restoreContext.edx = fullFrame->regs.edx;
   restoreContext.ecx = fullFrame->regs.ecx;
   restoreContext.eax = fullFrame->regs.eax;
   restoreContext.trapno = fullFrame->gateNum;
   restoreContext.err = fullFrame->frame.errorCode;
   restoreContext.eip = fullFrame->frame.eip;
   restoreContext.cs  = fullFrame->frame.cs;
   restoreContext.eflags = fullFrame->frame.eflags;
   restoreContext.espAtSignal = fullFrame->frame.esp;
   restoreContext.ss = fullFrame->frame.ss;
   restoreContext.fpstate = NULL; // XXX 
   restoreContext.oldmask = -1; // XXX
   restoreContext.cr2 = -1; // XXX
   // Non-standard bits of restoreContext:
   ASSERT(restoreMask);
   restoreContext.restoreMask = *restoreMask;
      
   if (vmx86_debug) {
      int i;
      for (i = 0; i < sizeof(restoreContext)/4; i++) {
         UWLOG(5, "restoreContext[%d] = %#x", i, ((int*)&restoreContext)[i]);
      }
   }

   /*
    * Muck up reg state to jump into signal handler.  Copy the
    * restoreContext onto the stack so we can use it if the signal
    * handler returns.  Then, finish setting up the stack for the
    * UserSigDispatch (via ktext, it calls directly into handler).
    */
   UWLOG(3, "Starting to muck with user stack. esp=%#x", esp);

   // To simulate a call instr (for pretty backtraces), copy the eip and ebp
   // onto the stack.
   if (status == VMK_OK) {
      status = UserSig_CopyChunk(&esp, sizeof(Reg32),
                                 &fullFrame->frame.eip, "interrupted eip");
   }
   if (status == VMK_OK) {
      status = UserSig_CopyChunk(&esp, sizeof(Reg32),
                                 &fullFrame->regs.ebp, "interrupted ebp");
   }
   baseAddr = esp;

   // Copy UserSigRestoreContext
   if (status == VMK_OK) {
      status = UserSig_CopyChunk(&esp, sizeof restoreContext,
                                 &restoreContext, "UserSigRestoreContext");
   }
   contextAddr = esp;
   
   // Copy the signal number to the stack
   if (status == VMK_OK) {
      int32 sig4byte = (int32)sig;
      status = UserSig_CopyChunk(&esp, sizeof(int32),
                                 &sig4byte, "signum");
   }
   
   ASSERT(esp == ALIGN_DOWN(esp, sizeof(int32)));   /* stack addresses must be word aligned */

   if (status == VMK_OK) {
      UserVA dispatchEntry = MY_USER_CARTEL_INFO->signals.dispatchEntry;

      /*
       * Finally, tweak esp, ebp and eip to fake the call to the
       * UserSigDispatch code (in the ktext page).  Put the handler
       * address in user mode eax, and put the context address in user
       * mode esi.
       */
      UWLOG(3, "Tweaking eip (@%p) to %#x (from %#x);",
            &fullFrame->frame.eip, dispatchEntry, fullFrame->frame.eip);
      UWLOG(3, "Tweaking eax (@%p) to %#x (from %#x)",
            &fullFrame->regs.eax, handler, fullFrame->regs.eax);
      UWLOG(3, "Tweaking ebp (@%p) to %#x (from %#x)",
            &fullFrame->regs.ebp, baseAddr, fullFrame->regs.ebp);
      UWLOG(3, "Tweaking esp (@%p) to %#x (from %#x)",
            &fullFrame->frame.esp, esp, fullFrame->frame.esp);
      UWLOG(3, "Tweaking esi (@%p) to %#x (from %#x)",
            &fullFrame->regs.esi, contextAddr, fullFrame->regs.esi);
      
      fullFrame->frame.eip = (Reg32)dispatchEntry;
      fullFrame->regs.eax = (uint32)handler;
      fullFrame->frame.esp = (Reg32)esp;
      fullFrame->regs.ebp = baseAddr;
      fullFrame->regs.esi = contextAddr;

      UWLOG_FullExcFrame(3, "Handler Frame", NULL, fullFrame, NULL);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSig_ReturnFromHandler --
 *
 *	Restore user mode register state to the pre-handler dispatch
 *	state.  Restore blocked signal mask if necessary.
 *
 * Results:
 *	VMK_OK if restore completed cleanly.  Otherwise if fault or
 *	error copying user-mode state in.
 *
 * Side effects:
 *	Overwrites user-mode register state.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserSig_ReturnFromHandler(UserVA userSavedContext,
                          struct VMKFullUserExcFrame* currentExcFrame)
{
   World_Handle* const currWorld = MY_RUNNING_WORLD;
   UserSig_ThreadInfo* const threadSigInfo = &currWorld->userThreadInfo->signals;
   VMK_ReturnStatus status;
   UserSigRestoreContext kSavedContext;

   ASSERT(currentExcFrame != NULL);

   status = User_CopyIn(&kSavedContext, userSavedContext, sizeof kSavedContext);
   if (status != VMK_OK) {
      // return, the caller should blow up ...  not very graceful, though
      UWWarn("Error copying savedContext into kernel (%s).  Bailing on user.",
             VMK_ReturnStatusToString(status));
      return status;
   }

   UWLOG(3, "userSavedContext @%#x, currentContext @%p",
         userSavedContext, currentExcFrame);
   
   status = User_CopyIn(&kSavedContext, userSavedContext, sizeof kSavedContext);

   /*
    * Convert back from linux-compatible UserSigRestoreContext to a
    * VMKFullUserExcFrame.  Note that we don't have to copy all of the
    * fields (see User_CleanFrameCopy).
    */
   if (status == VMK_OK) {
      VMKFullUserExcFrame kExcFrame;

      if (vmx86_debug) {
         int i;
         for (i = 0; i < sizeof(kSavedContext)/4; i++) {
            UWLOG(5, "kSavedContext[%d] = %#x", i, ((int*)&kSavedContext)[i]);
         }
      }

      /*
       * Restore the blocked signals to the state saved before the
       * handler dispatch (generally this re-enables a non-reentrant
       * signal, or restores the blocking context after a sigsuspend).
       *
       * We set this up to always have a valid mask so we can blindly
       * restore it here.
       */
      UserSig_SetBlocked(threadSigInfo, kSavedContext.restoreMask);

      memset(&kExcFrame, 0, sizeof kExcFrame);

      /*
       * Ignore all segment registers, errorCode, pushValue and gateNum.
       * (See User_CleanFrameCopy for details.)
       */
      kExcFrame.frame.eflags = kSavedContext.eflags;
      kExcFrame.frame.eip = kSavedContext.eip;
      kExcFrame.frame.esp = kSavedContext.esp;
      kExcFrame.regs.eax = kSavedContext.eax;
      kExcFrame.regs.ebx = kSavedContext.ebx;
      kExcFrame.regs.ecx = kSavedContext.ecx;
      kExcFrame.regs.edx = kSavedContext.edx;
      kExcFrame.regs.ebp = kSavedContext.ebp;
      kExcFrame.regs.esi = kSavedContext.esi;
      kExcFrame.regs.edi = kSavedContext.edi;

      UWLOG_FullExcFrame(3, "Old Frame", "New Frame",
                         currentExcFrame, &kExcFrame);

      /*
       * This is the actual "sigreturn".  Simply restore the user mode
       * register state to the pre-handler state.  This implicitly
       * rolls the stack back by restoring %esp.
       */
      status = User_CleanFrameCopy(currentExcFrame, &kExcFrame);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSigInitKText
 *
 *	Initialize kernel text for signal dispatcher.
 *
 * Results:
 *	VMK_OK if the install succeeds
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserSigInitKText(User_CartelInfo *uci)
{
   const int size = (UserSigDispatchEnd - UserSigDispatch);
   VMK_ReturnStatus status;
   UserVA uva = 0;

   ASSERT(UserSigDispatch < UserSigDispatchEnd);

   status = UserMem_AddToKText(&uci->mem, UserSigDispatch, size, &uva);
   UWLOG(1, "UserSigDispatch=%p/sz=%d, %s, uva=%#x",
         UserSigDispatch, size, VMK_ReturnStatusToString(status), uva);
   uci->signals.dispatchEntry = uva;

   return status;
}
