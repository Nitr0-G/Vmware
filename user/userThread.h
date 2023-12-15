/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * userThread.h --
 *
 *	UserWorld thread support
 */

#ifndef VMKERNEL_USER_USERTHREAD_H
#define VMKERNEL_USER_USERTHREAD_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "vm_basic_types.h"
#include "vmkernel.h"
#include "splock.h"
#include "userSig.h"

struct World_Handle;
struct User_CartelInfo;

/*
 * Maximum number of active threads in a cartel.  An active thread is
 * one that has been created (World_New) but not cleaned up
 * (World_Destroy).
 */
#define USER_MAX_ACTIVE_PEERS 16

/*
 * Maximum number of zombie threads in a cartel.  A zombie thread is
 * one that has been cleaned up (World_Destroy) but has not had its
 * exit status "collected" (UserThread_Collect).  The overhead of a
 * zombie world is just this state, the corresponding world structure
 * has been reclaimed.
 */
#define USER_MAX_ZOMBIE_PEERS 32

typedef struct UserThread_Peers {
   SP_SpinLock	lock;

   World_ID	activePeers[USER_MAX_ACTIVE_PEERS];

   /* Parallel arrays for zombie peers and their exit state */
   World_ID	zombiePeers[USER_MAX_ZOMBIE_PEERS];
   uint32	exitState[USER_MAX_ZOMBIE_PEERS];
} UserThread_Peers;

extern VMK_ReturnStatus UserThread_CartelInit(struct User_CartelInfo* uci);
extern VMK_ReturnStatus UserThread_CartelCleanup(struct User_CartelInfo* uci);

extern Bool UserThread_IsOnlyThread(struct World_Handle* w);

extern VMK_ReturnStatus UserThread_Clone(const char* name,
                                         UserVA startAddr,
                                         UserVA stackAddr,
                                         UserSigId deathSignal,
                                         World_ID deathTarget,
                                         World_Handle** newWorld);
extern VMK_ReturnStatus UserThread_Add(UserThread_Peers* peers,
                                       struct World_Handle* newworld);
extern VMK_ReturnStatus UserThread_Remove(UserThread_Peers* peers,
                                          struct World_Handle* deadWorld);

extern VMK_ReturnStatus UserThread_KillPeers(UserThread_Peers* peers,
                                             Bool vicious);
extern VMK_ReturnStatus UserThread_SetExitStatus(int status);

extern void UserThread_SaveStatus(UserThread_Peers* peers,
                                  World_ID worldID, int status);
extern VMK_ReturnStatus UserThread_Collect(UserThread_Peers* peers,
                                           World_ID* worldID,
                                           Bool block,
                                           int* status);
extern Timer_RelCycles UserThread_Sleep(Timer_RelCycles sleepTime);

typedef enum UserThread_WaitState {
   UTW_AWAKE = 0,               // so new threads start "awake"
   UTW_PRE_BLOCK = 1100,        // prepared to block, but not blocked
   UTW_BLOCKED,
   UTW_TIMEOUT,
   UTW_BACKOUT,
   UTW_WAIT_COMPLETE,
} UserThread_WaitState;

typedef struct UserThread_WaitInfo {
   UserThread_WaitState  state;
} UserThread_WaitInfo;

extern void UserThread_PrepareToWait(void);
extern void UserThread_CancelPreparedWait(void);

/*
 * Macros to map a thread's pointer to a wait "event id".  We
 * generally use object and lock addresses as the associated event
 * IDs. However, sleep and poll don't really have an associated
 * object, so we use a function of the thread address that shouldn't
 * overlap with other event ids inside the thread struct.
 */
#define UTWAIT_SLEEP_EVENT(uti) (uint32)((void*)(&(uti->waitInfo.state)) + 0)
#define UTWAIT_POLL_EVENT(uti)  (uint32)((void*)(&(uti->waitInfo.state)) + 1)

#define UTWAIT_WITHOUT_PREPARE FALSE
#define UTWAIT_WITH_PREPARE    TRUE
extern VMK_ReturnStatus UserThread_WaitInt(uint32 event, uint32 reason,
                                           SP_SpinLock *lock, Semaphore* sema,
                                           Timer_RelCycles timeout,
                                           Bool withPrepare);
extern void UserThread_Wakeup(World_ID worldID, UserThread_WaitState newState);
extern void UserThread_WakeupWorld(World_Handle* world, UserThread_WaitState newState);

extern void UserThread_WakeupGroup(struct User_CartelInfo* uci, uint32 event);

static INLINE VMK_ReturnStatus
UserThread_Wait(uint32 event, uint32 reason,
                SP_SpinLock *lock,
                Timer_RelCycles timeout,
                Bool withPrepare)
{
   // NULL sema
   return UserThread_WaitInt(event, reason, lock, NULL, timeout, withPrepare);
}

static INLINE VMK_ReturnStatus
UserThread_WaitSema(uint32 event, uint32 reason,
                    Semaphore *sema,
                    Timer_RelCycles timeout,
                    Bool withPrepare)
{
   // NULL splock
   return UserThread_WaitInt(event, reason, NULL, sema, timeout, withPrepare);
}

/*
 * Debugging routines.
 */
extern int UserThread_NumPeersDebug(void);
extern int UserThread_GetPeersDebug(World_ID* peerList);
extern Bool UserThread_IsPeerDebug(World_ID worldID);

#endif /* VMKERNEL_USER_USERTHREAD_H */
