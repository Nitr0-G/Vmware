/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * userDebug.h --
 *	UserWorld debugging glue.
 */

#ifndef VMKERNEL_USER_USERDEBUG_H
#define VMKERNEL_USER_USERDEBUG_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "vm_basic_types.h"
#include "vmkernel.h"
#include "splock.h"
#include "debug.h"
struct User_CartelInfo;

typedef int32 Thread_ID;

typedef struct UserDebug_State {
   SP_SpinLock		lock;

   Bool                 inDebugger;
   Bool			everInDebugger;
   Bool			wantBreakpoint;

   int			numWorlds;
   World_ID		threadToWorldMap[USER_MAX_ACTIVE_PEERS+1];
   World_ID		initialWorld;
   Thread_ID		initialThread;

   /* Target gdb thread for continue/step operations. */
   Thread_ID		targetContStep;
   /* Target gdb thread for all other operations. */
   Thread_ID		targetOther;

   VMKFullUserExcFrame*	currentUserState;

   Debug_Context	dbgCtx;

   UserVA               debugMagicStubEntry;

   char                 *inBuffer;
   char                 *outBuffer;
} UserDebug_State;

extern VMK_ReturnStatus UserDebug_CartelInit(struct User_CartelInfo* uci);
extern VMK_ReturnStatus UserDebug_CartelCleanup(struct User_CartelInfo* uci);
extern Bool UserDebug_Entry(uint32 vector);
extern void UserDebug_InDebuggerCheck(void);
extern Bool UserDebug_InDebuggerCheckFromInterrupt(VMKExcFrame* excFrame);
extern Bool UserDebug_EverInDebugger(void);
extern void UserDebug_Init(void);

#endif
