/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * user.h --
 *
 *	Interfaces from vmkernel/user/ exported to other vmkernel
 *	modules.  See vmkernel/public/user_ext.h for external
 *	interfaces.
 */

#ifndef _VMKERNEL_PRIVATE_USER_H
#define _VMKERNEL_PRIVATE_USER_H

#include "user_ext.h"           /* Include the external interfaces. */
#include "userProxy_ext.h"
#include "moduleCommon.h"
#include "debug.h"

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

typedef VA UserVA;       // Userspace pointer to data that we may read or write
typedef VA UserVAConst;  // Userspace pointer to data that we may only read 

typedef enum {
   USER_PAGE_NOT_PINNED,
   USER_PAGE_PINNED,
} UserPageType;

/*
 * Test if the given segment (generally CS) is from usermode.
 */
static INLINE Bool User_SegInUsermode(uint16 seg)
{
   return SELECTOR_RPL(seg) == 3;
}

extern VMK_ReturnStatus User_Init(void);
extern VMK_ReturnStatus User_WorldInit(World_Handle *world, World_InitArgs *args);
extern void User_WorldCleanup(World_Handle *world);
extern VMK_ReturnStatus User_CartelKill(World_Handle *world, Bool vicious);

extern NORETURN void User_UWVMKSyscallHandler(VMKExcFrame *regs);
extern NORETURN void User_LinuxSyscallHandler(VMKExcFrame *regs);
extern VMK_ReturnStatus User_InterruptCheck(World_Handle* currentWorld, VMKExcFrame *regs);
extern NORETURN void User_Exception(World_Handle* world, uint32 vector, VMKExcFrame *regs);

extern NORETURN void User_WorldStart(World_Handle* world, void *userStartFunc);

extern VMK_ReturnStatus User_PSharePage(World_Handle *world, VPN vpn);
extern Bool User_MarkSwapPage(World_Handle *world, uint32 reqNum, 
                              Bool writeFailed, uint32 swapFileSlot, WPN wpn, MPN mpn);
extern uint32 User_SwapOutPages(World_Handle *world, uint32 numPages);
extern VMK_ReturnStatus User_GetPageMPN(World_Handle *world, VPN vpn, 
                                        UserPageType pageType, MPN *mpnOut);

extern VMK_ReturnStatus User_CopyIn(void *dest, UserVA src, int length);
extern VMK_ReturnStatus User_CopyOut(UserVA dest, const void *src, int length);

extern VMK_ReturnStatus UserDebug_WantBreakpoint(VMnix_WantBreakpointArgs* args);
extern VMK_ReturnStatus UserProcDebug_DebugCnxInit(Debug_Context* dbgCtx);

extern void User_Wakeup(World_ID worldID);

extern void User_UpdatePseudoTSCConv(World_Handle *world,
                                     const RateConv_Params *conv);

/* System Calls from COS */

extern VMK_ReturnStatus LinuxSignal_Forward(World_Handle* world, int signum);
extern VMK_ReturnStatus UserProxy_ObjReady(World_Handle* world,
					   uint32 fileHandle,
					   UserProxyPollCacheUpdate *pcUpdate);
extern VMK_ReturnStatus UserProxy_CreateSpecialFds(World_Handle* world,
						   UserProxyObjType inType,
						   UserProxyObjType outType,
						   UserProxyObjType errType);
extern VMK_ReturnStatus UserTerm_CreateSpecialFds(World_Handle* world);

/* userInit: */
extern VMK_ReturnStatus UserInit_AddArg(World_Handle* world, const char* arg);
extern VMK_ReturnStatus UserInit_SetWorldWD(World_Handle* world, const char* dirname);
extern VMK_ReturnStatus UserInit_SetBreak(World_Handle* world, UserVA brk);
extern VMK_ReturnStatus UserInit_SetLoaderInfo(World_Handle* world, uint32 phdr,
                                               uint32 phent, uint32 phnum,
                                               uint32 base, uint32 entry);
extern VMK_ReturnStatus UserInit_AddMapFile(World_Handle* world, int fid,
                                            const char* fname);
extern VMK_ReturnStatus UserInit_AddMapSection(World_Handle* world,
                                               VA addr, uint32 length,
                                               uint32 prot, int flags,
                                               int id, uint64 offset,
                                               uint32 zeroAddr);
extern VMK_ReturnStatus UserInit_SetIdentity(World_Handle* world, uint32 umask,
                                             uint32 ruid, uint32 euid, uint32 suid,
                                             uint32 rgid, uint32 egid, uint32 sgid,
                                             uint32 ngids, uint32* gids);
extern VMK_ReturnStatus UserInit_SetDumpFlag(World_Handle* world, Bool enabled);
extern VMK_ReturnStatus UserInit_SetMaxEnvVars(World_Handle *world,
					       int maxEnvVars);
extern VMK_ReturnStatus UserInit_AddEnvVar(World_Handle* world,
					   char *hostEnvVar, uint32 length);
extern VMK_ReturnStatus UserDump_SetExecName(World_Handle *world,
					     char *execName);
extern VMK_ReturnStatus UserProxy_SetCosProxyPid(World_Handle *world,
						 int cosPid);

#endif
