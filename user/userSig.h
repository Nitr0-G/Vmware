/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * userSig.h --
 *
 *	UserWorld "signals"
 */

#ifndef VMKERNEL_USER_USERSIG_H
#define VMKERNEL_USER_USERSIG_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "cpusched.h"

struct World_Handle;
struct User_CartelInfo;
struct User_ThreadInfo;
struct VMKFullUserExcFrame;

/*
 * Linux signal names and numbers
 */
#define LINUX_SIG_ERR   0
#define LINUX_SIGHUP    1
#define LINUX_SIGINT    2
#define LINUX_SIGQUIT   3
#define LINUX_SIGILL    4
#define LINUX_SIGABRT   6
#define LINUX_SIGFPE    8
#define LINUX_SIGKILL   9
#define LINUX_SIGSEGV   11
#define LINUX_SIGPIPE   13
#define LINUX_SIGALRM   14
#define LINUX_SIGTERM   15
#define LINUX_SIGUSR1   10
#define LINUX_SIGUSR2   12
#define LINUX_SIGCHLD   17
#define LINUX_SIGCONT   18
#define LINUX_SIGSTOP   19
#define LINUX_SIGTSTP   20
#define LINUX_SIGTTIN   21
#define LINUX_SIGTTOU   22
#define LINUX_SIGBUS    7
#define LINUX_SIGPOLL   29
#define LINUX_SIGPROF   27
#define LINUX_SIGSYS    31
#define LINUX_SIGTRAP   5
#define LINUX_SIGURG    23
#define LINUX_SIGVTALRM 26
#define LINUX_SIGXCPU   24
#define LINUX_SIGXFSZ   25
#define LINUX_SIGRTMIN  32
/*
 * NOTE: 32 == SIGRTMIN   == pthread_restart
 *       33 == SIGRTMIN+1 == pthread_cancel
 *       34 == SIGRTMIN+2 == pthread_debug
 */
#define LINUX_SIGRTMAX  63
#define LINUX_NSIG      LINUX_SIGRTMAX
#define LINUX_NRTSIG    (LINUX_SIGRTMAX - LINUX_SIGRTMIN)
#define USERWORLD_NSIGNAL LINUX_NSIG

typedef uint8 UserSigId;

typedef uint64 UserSigSet;
#define USERSIGSET_EMPTY (0LL)

/*
 * UserSig_Handler is the address of a signal handler.  Technically,
 * it should have this signature:
 * 	typedef void (*UserSig_Handler)(int, void*, void*);
 * but the kernel best not be invoking it, so we just use a UserVA
 * to identify it.
 *
 * See also LINUX_SIG_DFL and LINUX_SIG_IGN in linuxAPI.h
 */
typedef UserVA UserSig_Handler;


/*
 * SigAction flags.
 */
typedef enum {
   USERSIGACT_FLAG_ONESHOT   = 0x0001, /* reset after fire */
   USERSIGACT_FLAG_REENTRANT = 0x0002, /* unblock before fire */
} UserSigActFlags;


/*
 * Structure tracking what to do for a specific signal.  Includes
 * the handler, the mask to use while the handler is active, and
 * flags for oneshot, reentrant, etc.
 */
typedef struct UserSigAction {
   UserSig_Handler	handler;
   UserSigSet		mask;
   UserSigActFlags	flags;
} UserSigAction;

/*
 * Per-cartel signal state.  The handlers and flags for handling a
 * signal are shared among the threads in a cartel.
 *
 * Always lock thread-private state before locking shared state (if
 * you have to lock both).
 */
typedef struct UserSig_CartelInfo {
   SP_SpinLock   lock;
   UserSigAction sigActions[USERWORLD_NSIGNAL];	/* currently installed handlers */
   UserVA        dispatchEntry;
} UserSig_CartelInfo;

/*
 * Per-thread signal state.  Each thread has its own bits for pending
 * and blocked signals.  The "real-time" signals are "queued" (they
 * cannot be lost), so the rtSigPending counts the number of pending
 * invocations for each "real-time" signal.
 *
 * Always lock thread-private state before locking shared state (if
 * you have to lock both).
 *
 * A thread should only manipulate its own thread-private signal
 * state, except for setting the pending bit (and perhaps incrementing
 * rtSigPending) when sending a signal to a target.  The other fields
 * should only be read or written by their owner, thus they don't need
 * the lock.
 */
typedef struct UserSig_ThreadInfo {
   /* These fields are thread-private (need no lock): */
   UserSigSet	blocked;	/* currently blocked signals */
   UserSigId	deathSig;       /* If non-zero, sig to send to deathSigTarget */ 
   World_ID	deathSigTarget;

   /* These fields may be accessed by other threads: */
   SP_SpinLock	lock;
   volatile int pendingBit;     /* Bit checked at wait/interrupt */
   UserSigSet	pending;	/* currently pending signals */
   uint32	rtSigPending[LINUX_NRTSIG];
} UserSig_ThreadInfo;

extern inline UserSigSet 
UserSig_IdToMask(UserSigId id) 
{
   /* glibc wants to save a measly bit, by using bit 0 for signal 1... */
   return (((UserSigSet)1)<<(id-1));
}

extern VMK_ReturnStatus UserSig_HandleVector(struct World_Handle* w,
                                             int vector,
                                             struct VMKFullUserExcFrame* fullFrame);
extern int UserSig_Suspend(UserSig_ThreadInfo* threadSigInfo,
                            UserSigSet blocked,
                            struct VMKFullUserExcFrame* fullFrame,
                            int returnCode);
extern VMK_ReturnStatus UserSig_LookupAndSend(World_ID id, UserSigId sig, Bool sameCartel);
extern void UserSig_Send(struct World_Handle* target, UserSigId sig);
extern void UserSig_HandlePending(UserSig_ThreadInfo* threadSigInfo,
                                  struct VMKFullUserExcFrame* fullFrame);
extern void UserSig_SendDeathSignal(struct World_Handle* dyingWorld);

extern VMK_ReturnStatus UserSig_ThreadInit(struct User_ThreadInfo* uti);
extern VMK_ReturnStatus UserSig_ThreadCleanup(struct User_ThreadInfo* uti);
extern VMK_ReturnStatus UserSig_CartelInit(struct User_CartelInfo* uci);
extern VMK_ReturnStatus UserSig_CartelCleanup(struct User_CartelInfo* uci);

extern VMK_ReturnStatus UserSig_ReturnFromHandler(UserVA savedContext,
                                                  struct VMKFullUserExcFrame* f);

extern UserSigId UserSig_FromIntelException(uint32 vector);

extern void UserSig_CartelLock(UserSig_CartelInfo* cartelSigInfo);
extern void UserSig_CartelUnlock(UserSig_CartelInfo* cartelSigInfo);

extern void UserSig_SetBlocked(UserSig_ThreadInfo* threadSigInfo, UserSigSet blocked);
extern void UserSig_GetBlocked(const UserSig_ThreadInfo* threadSigInfo, UserSigSet* oset);

extern UserSig_Handler UserSig_GetSigHandler(const UserSig_CartelInfo* cartelSigInfo,
                                             UserSigId sig);
extern void UserSig_SetSigHandler(UserSig_CartelInfo* cartelSigInfo,
                                  UserSigId sig,
                                  UserSig_Handler handler);
extern UserSigSet UserSig_GetSigMask(const UserSig_CartelInfo* cartelSigInfo,
                                     UserSigId sig);
extern void UserSig_SetSigMask(UserSig_CartelInfo* cartelSigInfo,
                               UserSigId sig,
                               UserSigSet mask);
extern Bool UserSig_IsOneShot(const UserSig_CartelInfo* cartelSigInfo,
                              UserSigId sig);
extern void UserSig_SetOneShot(UserSig_CartelInfo* cartelSigInfo,
                               UserSigId sig,
                               Bool enable);
extern Bool UserSig_IsReentrant(const UserSig_CartelInfo* cartelSigInfo,
                                UserSigId sig);
extern void UserSig_SetReentrant(UserSig_CartelInfo* cartelSigInfo,
                                 UserSigId sig,
                                 Bool enable);
extern void UserSig_InterruptCheck(World_Handle* interruptedWorld,
                                   VMKFullUserExcFrame *interruptContext);

extern VMK_ReturnStatus UserSig_CopyChunk(UserVA* esp,
					  size_t chunkSize,
					  const void* chunkData,
					  const char* logName);


#endif /* VMKERNEL_USER_USERSIG_H */
