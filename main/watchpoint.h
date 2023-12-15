/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * watch.h -- 
 *
 *	Watchpoint definitions.
 */

#ifndef	_WATCH_H
#define	_WATCH_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#define WATCHPOINT_LIMIT_NONE	-1

typedef enum {
   WATCHPOINT_TYPE_NONE,
   WATCHPOINT_TYPE_EXEC,
   WATCHPOINT_TYPE_WRITE,
   WATCHPOINT_TYPE_READ_WRITE,
} Watchpoint_Type;

typedef enum {
   WATCHPOINT_ACTION_NONE,
   WATCHPOINT_ACTION_CONTINUE,
   WATCHPOINT_ACTION_BREAK,
} Watchpoint_Action;

typedef struct Watchpoint_State {
   uint32 enabledCount;
   Bool changed;
   uint32 dr0, dr1, dr2, dr3, dr6, dr7;
} Watchpoint_State;

#ifdef VMX86_ENABLE_WATCHPOINTS

extern void Watchpoint_Enable(Bool save);
extern void Watchpoint_ForceEnable(void);
extern Bool Watchpoint_ForceDisable(void);
extern void Watchpoint_Disable(Bool restore);
extern void Watchpoint_Update(void);
extern Watchpoint_Action Watchpoint_Check(VMKExcFrame *regs);

#else

static inline void 
Watchpoint_Enable(UNUSED_PARAM(Bool save))
{
}

static inline void 
Watchpoint_ForceEnable(void)
{
}

static inline Bool 
Watchpoint_ForceDisable(void)
{
   return FALSE;
}

static inline void 
Watchpoint_Disable(UNUSED_PARAM(Bool restore))
{
}

static inline void 
Watchpoint_Update(void)
{
}

static inline Watchpoint_Action 
Watchpoint_Check(UNUSED_PARAM(VMKExcFrame *regs))
{
   return WATCHPOINT_ACTION_NONE;
}

#endif

struct World_Handle;
extern void Watchpoint_Init(void);
extern void Watchpoint_WorldInit(struct World_Handle *world);
extern Bool Watchpoint_Add(VA vaddr, uint32 length, Watchpoint_Type type,
			   Watchpoint_Action action, int32 limit);
extern Bool Watchpoint_Remove(VA vaddr, uint32 length, Watchpoint_Type type);
extern Bool Watchpoint_Enabled(void);

#endif
