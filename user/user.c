/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * user.c --
 *
 *	This module manages the user level world operations.
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "vm_asm.h"
#include "vmkernel.h"
#include "vmk_net.h"
#include "world.h"
#include "util.h"
#include "memalloc.h"
#include "dump.h"
#include "user_int.h"
#include "userSig.h"
#include "userMem.h"
#include "userThread.h"
#include "userDebug.h"
#include "userStat.h"
#include "userInit.h"
#include "common.h"
#include "compatErrno.h"
#include "bh.h"
#include "userFile.h"
#include "userProxy.h"
#include "userCopy.h"
#include "uwvmkSyscall.h"
#include "xmap.h"
#include "memmap.h"
#include "kvmap.h"
#include "user_layout.h"
#include "userSocket.h"
#include "heapMgr.h"
#include "trace.h"

#define LOGLEVEL_MODULE User
#include "userLog.h"

/*
 * Zero page for use by UserWorlds.
 */
uint8 zeroPage[PAGE_SIZE];

/*
 * aux info vector element to be passed on stack to new
 * user processes.
 *
 * See System V i386 ABI Spec.
 */
#define AUXVEC_AT_NULL		0
#define AUXVEC_AT_IGNORE	1
#define AUXVEC_AT_EXECFD	2  /* file descriptor of program */
#define AUXVEC_AT_PHDR		3  /* location of program headers for program */
#define AUXVEC_AT_PHENT		4  /* size of program header entry */
#define AUXVEC_AT_PHNUM		5  /* number of program headers */
#define AUXVEC_AT_PAGESIZE	6  /* page size */
#define AUXVEC_AT_BASE		7  /* base address of interpreter */
#define AUXVEC_AT_FLAGS		8
#define AUXVEC_AT_ENTRY		9  /* entry point of program */
#define AUXVEC_AT_NOTELF	10
#define AUXVEC_AT_UID		11
#define AUXVEC_AT_EUID		12
#define AUXVEC_AT_GID		13
#define AUXVEC_AT_EGID		14
#define AUXVEC_AT_PLATFORM	15 /* cpu type */
#define AUXVEC_AT_HWCAP		16 /* cpu capabilities */
#define AUXVEC_AT_CLKTCK	17 /* clock tick frequency */

#define SET_AUX_VEC(auxVec, i, type, val)	\
   do {						\
      (auxVec)[(i)].a_type = (type);		\
      (auxVec)[(i)].a_un.a_val = (val);		\
   } while(0)

typedef struct UserAuxVec {
   int a_type;
   union {
      long a_val;
      void *a_ptr;
      void (*a_fcn)(void);
   } a_un;
} UserAuxVec;

/*
 * Cartel initialization/cleanup table.  Note the union of function
 * pointers: these two function signatures are call-time compatible, so we
 * can push the arguments to the uciWorldFn, and still (semi-)legitimately
 * call uciFn.
 */
struct {
   const char* name;
   union {
      VMK_ReturnStatus (*uciFn)(User_CartelInfo* uci);
      VMK_ReturnStatus (*uciWorldFn)(User_CartelInfo* uci, World_Handle* world);
   } init;
   union {
      VMK_ReturnStatus (*uciFn)(User_CartelInfo* uci);
      VMK_ReturnStatus (*uciWorldFn)(User_CartelInfo* uci, World_Handle* world);
   } cleanup;
} userCartelInitTable[] = {
#define WITH_UCI(_f) { .name = #_f ,                            \
         .init = { .uciFn = _f##_CartelInit },                  \
         .cleanup = { .uciFn = _f##_CartelCleanup } }
#define WITH_UCI_AND_WORLD(_f) { .name = #_f ,                  \
         .init = { .uciWorldFn = _f##_CartelInit },             \
         .cleanup = { .uciWorldFn = _f##_CartelCleanup } }
   /*
    * Order is important.  Top-to-bottom for initialization.
    * Bottom-to-top for cleanup.
    *
    * On the init side, must init ktext (UserMem) before UserTime or
    * UserSig or UserDebug (which add ktext handlers).
    *
    * On the cleanup side, we must clean up subsystems that can hold
    * open handles to proxy objects (obj and mem) before taking down
    * the proxy.
    */
   WITH_UCI(UserDump),
   WITH_UCI(UserStat),
   WITH_UCI(UserThread),
   WITH_UCI(UserProxy),
   WITH_UCI_AND_WORLD(UserMem),
   WITH_UCI(UserDebug),
   WITH_UCI(UserTime),
   WITH_UCI(UserSig),
   WITH_UCI(UserObj),
   WITH_UCI(UserInit),
   WITH_UCI(UserSocket),
};

static void UserSetShutdownState(User_CartelInfo* uci,
                                 int exitStatus,
                                 int exceptionType,
                                 const VMKFullUserExcFrame *fullFrame);

static Heap_ID UserNewHeap(World_ID cartelID);
static void UserDestroyHeap(Heap_ID heap);
static NORETURN void UserGenericSyscallExit(VMKFullUserExcFrame* fullFrame,
                                            Bool succeeded);

#define DEFINE_VMK_ERR(_err, _str, _uerr) LINUX_##_uerr,
#define DEFINE_VMK_ERR_AT(_err, _str, _val, _uerr)  LINUX_##_uerr,
int vmkToLinuxCodeMap[] = {
   VMK_ERROR_CODES
};
#undef DEFINE_VMK_ERR
#undef DEFINE_VMK_ERR_AT

/*
 *----------------------------------------------------------------------
 *
 * User_TranslateStatus --
 *	
 *      Generic translation of VMK status codes to Linux error codes.
 *      Some Linux system calls may need to special-case certain status
 *      codes, but this function should at least work as a default case.
 *      
 * Results:
 *	Linux error code.
 *
 * Side effects:
 *	Optional logging.
 *
 *----------------------------------------------------------------------
 */
int
User_TranslateStatus(VMK_ReturnStatus status)
{
   int rc;

   if (status >= VMK_GENERIC_LINUX_ERROR) {
      // unwrap opaque linux error code and negate
      rc = -(status - VMK_GENERIC_LINUX_ERROR);

   } else {
      if (status >= VMK_FAILURE) {
         status -= VMK_FAILURE - 1;
      }
      if ((uint32)status >= ARRAYSIZE(vmkToLinuxCodeMap)) {
         rc = LINUX_ENOSYS;
      } else {
         rc = vmkToLinuxCodeMap[status];
      }
   }

   if (rc != 0) {
      UWLOG(3, "%#x (%s) -> %d", status,
            UWLOG_ReturnStatusToString(status), rc);
   }
   return rc;
}


/*
 *----------------------------------------------------------------------
 *
 * User_Init --
 *	
 *	Initialize UserWorlds module.
 *
 * Results:
 *	VMK_OK
 *
 * Side effects:
 *	Many locks are initialized.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
User_Init(void)
{
   VMK_ReturnStatus status;
   
   UserMem_Init();
   status = UserStat_Init();
   if (status != VMK_OK) {
      return status;
   }
   
   UserDebug_Init();
   status = UserTime_Init();
   if (status != VMK_OK) {
      return status;
   }
   status = UserSocketUnix_Init();

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserKernelEntry --
 *
 *	Called when first entering user*.c code from usermode.
 *	Currently the only two entry points are during an interrupt or a
 *	syscall, thus User_InterruptCheck and UserGenericSyscallEntry
 *	are the only two callers of this function currently.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Will terminate the world if it has died, sets up uti->exceptionFrame.
 *
 *----------------------------------------------------------------------
 */
static void
UserKernelEntry(World_Handle *world, VMKFullUserExcFrame *fullFrame)
{
   User_ThreadInfo *uti = world->userThreadInfo;

   /*
    * ASSERTs for sanity.
    */
   ASSERT(User_SegInUsermode(fullFrame->frame.cs));
   ASSERT(World_IsUSERWorld(world));
   ASSERT(!CpuSched_IsPreemptible());
   ASSERT(World_IsSafeToBlock()); /* May block in signal dispatch or debugger */

   uti->exceptionFrame = fullFrame;

   if (uti->dead || world->deathPending) {
      UWLOG(1, "Termination requested, was in user code.  Dying.");
      World_Exit(VMK_OK);
      NOT_REACHED();
   }
}


/*
 *----------------------------------------------------------------------
 *
 * UserKernelExit --
 *
 *	Called upon exit from user*.c code back to the vmkernel.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Will terminate the world if it has died, clears out
 *	uti->exceptionFrame and the logging context.
 *
 *----------------------------------------------------------------------
 */
static void
UserKernelExit(World_Handle *world)
{
   User_ThreadInfo *uti = world->userThreadInfo;

   ASSERT(World_IsUSERWorld(world));

   if (uti->dead || world->deathPending) {
      UWLOG(1, "Termination requested, was in user code.  Dying.");
      World_Exit(VMK_OK);
      NOT_REACHED();
   }

   uti->exceptionFrame = NULL;
   UWLOG_ClearContext();
}


/*
 *----------------------------------------------------------------------
 *
 * User_InterruptCheck --
 *
 *	Invoked by IDT interrupt handler if a timer interrupt hit a
 *	userworld running CPL 3 code.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	May munge register state and user-mode stack to dispatch a
 *	signal.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
User_InterruptCheck(World_Handle* interruptedWorld, VMKExcFrame* regs)
{
   User_ThreadInfo* uti = interruptedWorld->userThreadInfo;
   User_CartelInfo* uci = interruptedWorld->userCartelInfo;

   ASSERT(interruptedWorld == MY_RUNNING_WORLD);

   UWLOG_SetContextException(UWLOG_INTERRUPT);
   UserKernelEntry(interruptedWorld, VMKEXCFRAME_TO_FULLUSERFRAME(regs));

   /*
    * If userworld is inside critical section of PTSC_Get, back up its EIP to
    * the beginning of the critical section.  Do this *before* calling
    * InDebuggerCheck or InterruptCheck, as they can munge eip.
    */
   if ((regs->eip - (uci->time.criticalSection + 1))
       < (uci->time.criticalSectionSize - 1)) {
      regs->eip = uci->time.criticalSection;
   }

   /*
    * See if we're currently in the debugger or dumping.  If so,
    * InDebuggerCheckFromInterrupt will munge things so that upon return to
    * userland we'll immediately make a syscall so that we can block this world.
    */
   if ((Debug_UWDebuggerIsEnabled() || UserDump_DumpInProgress()) &&
       UserDebug_InDebuggerCheckFromInterrupt(regs)) {
      goto done;
   }

   /*
    * See if I have pending signals to handle.
    */
   if (uti->signals.pendingBit != 0) {
      UWSTAT_INC(pendingSigsInt);
      /*
       * We have pending signals (or at least should do a more thorough
       * check on the pending signal mask).  May schedule the cartel for
       * termination.
       */
      UserSig_HandlePending(&interruptedWorld->userThreadInfo->signals,
                            uti->exceptionFrame);
   }

done:
   UserKernelExit(interruptedWorld);
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserGenericSyscallEntry --
 *
 *      Entry code common to both Linux and UWVMK syscalls.  Simple
 *      sanity checks on the state of the UserWorld, and then sets up
 *      machine state.
 *
 * Results:
 *	VMK_NOT_READY if UserWorld isn't completely initialized.
 *	VMK_OK otherwise.  Enables interrupts in either case.
 *
 * Side effects:
 *	Interrupts re-enabled, preemption disabled.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserGenericSyscallEntry(VMKFullUserExcFrame* fullFrame)
{
   World_Handle* const currentWorld = MY_RUNNING_WORLD;
   User_ThreadInfo* uti = currentWorld->userThreadInfo;

   ASSERT_NO_INTERRUPTS();

   ASSERT(CpuSched_IsPreemptible());
   CpuSched_DisablePreemption();
   ENABLE_INTERRUPTS();

   Watchpoint_Enable(TRUE);

   if (!World_IsUSERWorld(currentWorld)) {
      UWWarn("non-UserWorld trying to invoke syscall (ignoring)");
      return VMK_NOT_READY;
   }
   
   if ((currentWorld->userCartelInfo == NULL)
       || (uti == NULL)) {
      UWWarn("Partially initialized UserWorld trying to"
             " invoke syscall (ignoring)");
      return VMK_NOT_READY;
   }

   UserKernelEntry(currentWorld, fullFrame);
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserGenericSyscallExit --
 *
 *	Syscall exit code common to both Linux and UWVMK syscalls.
 *	Cleans up machine state and checks for (and dispatches to) any
 *	pending signals.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	fullFrame may get munged to effect a signal handler dispatch.
 *	May terminate cartel if dispatch goes sour.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
UserGenericSyscallExit(VMKFullUserExcFrame* fullFrame, Bool succeeded)
{
   User_ThreadInfo* uti = MY_RUNNING_WORLD->userThreadInfo;

   ASSERT(uti != NULL);

   /* Print a stack trace if syscall failed. */
   if (!succeeded) {
      UWLOG(1, "not obvious success: eax=%#x", fullFrame->regs.eax);
      UWLOG_StackTrace(1, fullFrame);
   }

   if (UserDump_DumpInProgress()) {
      UserDump_WaitForDumper();
   }

   if (Debug_UWDebuggerIsEnabled()) {
      UserDebug_InDebuggerCheck();
   }

   /*
    * Don't bother with signal dispatch if we're dead.  However, we
    * may die trying to dispatch signals, so just postpone the actual
    * exit.
    */
   if (!uti->dead) {
      /*
       * Dispatch to any pending, unblocked signals.  Current fullFrame
       * will be saved away, and then heavily modified to perform
       * user-mode dispatch.  May schedule the cartel for termination if
       * dispatch fails.
       */
      UserSig_HandlePending(&uti->signals, fullFrame);
   }

   BH_Check(TRUE);

   Watchpoint_Disable(TRUE);

   UserKernelExit(MY_RUNNING_WORLD);

   CLEAR_INTERRUPTS();
   ASSERT(!CpuSched_IsPreemptible());
   CpuSched_EnablePreemption();

   asm("clts");
   CommonRet(VMKFULLUSERFRAME_TO_EXCFRAME(fullFrame));
   NOT_REACHED();
}


/*
 *----------------------------------------------------------------------
 *
 * User_LinuxSyscallHandler --
 *
 *      Handle a Linux-compatibility system call from user level.
 *
 * Results:
 *      Does not return (returns via given excFrame)
 *
 * Side effects:
 *      Whatever the side-effects of the syscall are.
 *
 *----------------------------------------------------------------------
 */
void
User_LinuxSyscallHandler(VMKExcFrame *excFrame)
{      
   VMKFullUserExcFrame* fullFrame = VMKEXCFRAME_TO_FULLUSERFRAME(excFrame);
   const uint32 syscallNum = fullFrame->regs.eax;
   Bool success = FALSE;
   User_SyscallHandler handler = UserLinux_UndefinedSyscall;

   UWLOG_SetContextSyscall(TRUE, syscallNum);
   if (UserGenericSyscallEntry(fullFrame) == VMK_OK) {
      /* Find appropriate syscall handler */
      if ((syscallNum < UserLinux_SyscallTableLen)
          && (UserLinux_SyscallTable[syscallNum] != NULL)) {
         UWSTAT_ARRINC(linuxSyscallCount, syscallNum);
         handler = UserLinux_SyscallTable[syscallNum];
      }

      UWLOG(3, "eip=%#x ebx=%#x ecx=%#x edx=%#x esi=%#x edi=%#x ebp=%#x",
            fullFrame->frame.eip, 
            fullFrame->regs.ebx, fullFrame->regs.ecx,
            fullFrame->regs.edx, fullFrame->regs.esi,
            fullFrame->regs.edi, fullFrame->regs.ebp);

      Trace_EventLocal(TRACE_USERWORLD_SYSCALL, syscallNum, syscallNum);
      /*
       * Put all possible syscall arguments on the stack.  Most won't be
       * looked at.
       *
       * The fullFrame parameter is passed last so syscalls can get at
       * the usermode state (e.g., sigsuspend, or so they can print
       * a stack trace of user mode).
       */
      fullFrame->regs.eax = handler(fullFrame->regs.ebx,
                                    fullFrame->regs.ecx,
                                    fullFrame->regs.edx,
                                    fullFrame->regs.esi,
                                    fullFrame->regs.edi,
                                    fullFrame->regs.ebp);
      success = (fullFrame->regs.eax < ((uint32)LINUX_ERRNO_MAX));
      // null trace event to indicate end of a call
      Trace_EventLocal(TRACE_USERWORLD_SYSCALL, 0, 0); 

      UWLOG(3, "<complete>: eax=%#x", fullFrame->regs.eax);
   }

   UserGenericSyscallExit(fullFrame, success);
   NOT_REACHED();
}


/*
 *----------------------------------------------------------------------
 *
 * User_UWVMKSyscallHandler --
 *
 *      Handle a UWVMK system call from user level.
 *
 * Results:
 *      Does not return (returns via given excFrame)
 *
 * Side effects:
 *      Whatever the side-effects of the syscall are.
 *
 *----------------------------------------------------------------------
 */
void
User_UWVMKSyscallHandler(VMKExcFrame *excFrame)
{      
   VMKFullUserExcFrame* fullFrame = VMKEXCFRAME_TO_FULLUSERFRAME(excFrame);
   const uint32 syscallNum = fullFrame->regs.eax;
   Bool success = FALSE;
   UWVMKSyscall_Handler handler = UWVMKSyscall_Undefined;

   UWLOG_SetContextSyscall(FALSE, syscallNum);
   if (UserGenericSyscallEntry(fullFrame) == VMK_OK) {
      handler = UWVMKSyscall_GetHandler(syscallNum, UWVMKSyscall_Undefined);
      ASSERT(handler != NULL);
      if (handler != UWVMKSyscall_Undefined) {
         UWSTAT_ARRINC(uwvmkSyscallCount, syscallNum);
      }

      UWLOG(3, "ebx=%#x ecx=%#x edx=%#x esi=%#x edi=%#x ebp=%#x",
            fullFrame->regs.ebx, fullFrame->regs.ecx,
            fullFrame->regs.edx, fullFrame->regs.esi,
            fullFrame->regs.edi, fullFrame->regs.ebp);

      /* Invoke handler */
      Trace_EventLocal(TRACE_USERWORLD_VMKCALL, syscallNum, syscallNum);
      handler(fullFrame);
      // null trace event to indicate end of a call
      Trace_EventLocal(TRACE_USERWORLD_VMKCALL, 0, 0);
      success = (fullFrame->regs.eax == 0);
      UWLOG(3, "<complete>: eax=%#x", fullFrame->regs.eax);
   }

   UserGenericSyscallExit(fullFrame, success);
   NOT_REACHED();
}


/*
 *----------------------------------------------------------------------
 *
 * User_CopyIn --
 *
 *      Copy data in from a user world.
 *
 * Results:
 *      VMK_OK if OK, VMK_INVALID_ADDRESS if not.
 *
 * Side effects:
 *      The destination is modified.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
User_CopyIn(void *dest, UserVA /*const void* */ src, int length)
{
   ASSERT(dest != NULL);
   ASSERT(length > 0);

   /*
    * Faults on the UserVA can block on swap access or on RPCs out to
    * the proxy.  So User_CopyIn can only be called in blockable
    * contexts.
    */
   ASSERT(World_IsSafeToBlock());

   /* Cannot support recursive calls to copy in/out */
   ASSERT(MY_RUNNING_WORLD->userLongJumpPC == NULL);

   if ((src < VMK_USER_FIRST_TEXT_VADDR) ||
       (src > VMK_USER_LAST_VADDR)) {
      UWLOG(1, "Bad user addr %#x -- obviously outside user VA range "
            "(%#x to %#x)",
            src, VMK_USER_FIRST_TEXT_VADDR, VMK_USER_LAST_VADDR);
      /* Don't optimize for this case, just want the UWLOG statement ... */
   }

   UWSTAT_INSERT(copyInSizes, length);

   MY_RUNNING_WORLD->userLongJumpPC = UserDoCopyInDone;
   MY_RUNNING_WORLD->userCopyStatus = VMK_OK;

   UserDoCopyIn(MAKE_SELECTOR(DEFAULT_USER_DATA_DESC, 0, 3), dest, src, length);

   MY_RUNNING_WORLD->userLongJumpPC = NULL;

   return MY_RUNNING_WORLD->userCopyStatus;
}


/*
 *----------------------------------------------------------------------
 *
 * User_CopyInString --
 *
 *      Copy a null-terminated string in from the current user world.
 *      Copy up to maxLen bytes (including the null terminator) into dest.
 *	maxLen must always be at least 1 (always room for the null
 *	terminator).
 *
 * Results:
 *      VMK_OK if OK, error code (generally VMK_INVALID_ADDRESS or
 *      VMK_NO_ACCESS) if not.  VMK_LIMIT_EXCEEDED if src string does
 *	not terminate within maxLen bytes.
 *
 * Side effects:
 *      The destination is modified.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
User_CopyInString(char* dest, UserVA /*const void* */ src, int maxLen)
{
   int copied;

   ASSERT(dest != NULL);

   /*
    * Faults on the UserVA can block on swap access or on RPCs out to
    * the proxy.  So User_CopyInString can only be called in blockable
    * contexts.
    */
   ASSERT(World_IsSafeToBlock());

   /* Cannot support recursive calls to copy in/out */
   ASSERT(MY_RUNNING_WORLD->userLongJumpPC == NULL);

   /* UserDoCopyInString assumes room for at least null terminator. */
   if (maxLen < 1) {
      return VMK_LIMIT_EXCEEDED;
   }

   if ((src < VMK_USER_FIRST_TEXT_VADDR) ||
       (src > VMK_USER_LAST_VADDR)) {
      UWLOG(1, "Bad user addr %#x -- obviously outside user VA range "
            "(%#x to %#x)",
            src, VMK_USER_FIRST_TEXT_VADDR, VMK_USER_LAST_VADDR);
      /* Don't optimize for this case, just want the UWLOG statement ... */
   }

   MY_RUNNING_WORLD->userLongJumpPC = UserDoCopyInStringDone;
   MY_RUNNING_WORLD->userCopyStatus = VMK_OK;

   copied = maxLen; // copied is in/out maxlen/actuallen
   UserDoCopyInString(MAKE_SELECTOR(DEFAULT_USER_DATA_DESC, 0, 3),
                      dest, src, &copied);

   MY_RUNNING_WORLD->userLongJumpPC = NULL;

   if (MY_RUNNING_WORLD->userCopyStatus == VMK_OK) {
      // 'copied' is only updated if VMK_OK
      ASSERT(copied >= 0 && copied <= maxLen);
      UWSTAT_INSERT(copyInSizes, copied);
      if (copied == maxLen) {
         UWLOG(1, "String at %#x too long (max %d)", src, maxLen);
         return VMK_LIMIT_EXCEEDED;
      }
      ASSERT(copied == strlen(dest) + 1);
   }

   return MY_RUNNING_WORLD->userCopyStatus;
}


/*
 *----------------------------------------------------------------------
 *
 * User_CopyOut --
 *
 *      Copy data out to the user world.
 *
 * Results:
 *      VMK_OK if OK, VMK_INVALID_ADDRESS if not.
 *
 * Side effects:
 *      The destination is modified.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
User_CopyOut(VA dest, const void *src, int length)
{
   ASSERT(src != NULL);
   ASSERT(length > 0);

   /*
    * Faults on the UserVA can block on swap access or on RPCs out to
    * the proxy.  So User_CopyOut can only be called in blockable
    * contexts.
    */
   ASSERT(World_IsSafeToBlock());

   /* Cannot support recursive calls to copy in/out */
   ASSERT(MY_RUNNING_WORLD->userLongJumpPC == NULL);

   if ((dest < VMK_USER_FIRST_TEXT_VADDR) ||
       (dest > VMK_USER_LAST_VADDR)) {
      UWLOG(1, "Bad user addr %#x -- obviously outside user VA range "
            "(%#x to %#x)",
            dest, VMK_USER_FIRST_TEXT_VADDR, VMK_USER_LAST_VADDR);
      /* Don't optimize for this case, just want the UWLOG statement ... */
   }

   UWSTAT_INSERT(copyOutSizes, length);

   MY_RUNNING_WORLD->userLongJumpPC = UserDoCopyOutDone;
   MY_RUNNING_WORLD->userCopyStatus = VMK_OK;

   UserDoCopyOut(MAKE_SELECTOR(DEFAULT_USER_DATA_DESC, 0, 3), dest, src, length);

   MY_RUNNING_WORLD->userLongJumpPC = NULL;

   return MY_RUNNING_WORLD->userCopyStatus;
}


/*
 *----------------------------------------------------------------------
 *
 * UserCheckEFlags --
 *
 *	Check the given 'newFlags' against the 'oldFlags' to make sure
 *	that privileged or reserved bits are not changed.  
 *
 * Results:
 *	VMK_OK if eflags is safe, VMK_BAD_EXCFRAME if not.
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserCheckEFlags(Reg32 oldFlags, Reg32 *newFlags)
{
   const x86_FLAGS clear   = ~(EFLAGS_SET | EFLAGS_USER | EFLAGS_PRIV);
   const x86_FLAGS passable= (EFLAGS_AC | EFLAGS_TF | EFLAGS_USER);
   VMK_ReturnStatus status = VMK_OK;

   // Make sure we've got all bits in %eflags covered
   ASSERT((clear ^ EFLAGS_SET ^ EFLAGS_USER ^ EFLAGS_PRIV) == 0xffffffff);

   if ((*newFlags & EFLAGS_SET) != EFLAGS_SET) {
      UWLOG(0, "Always-1 eflags (%#x) clear.", *newFlags & EFLAGS_SET);
      status = VMK_BAD_EXCFRAME;
   }

   if ((*newFlags & clear) != 0) {
      UWLOG(0, "Always-0 eflags (%#x) set.", *newFlags & clear);
      status = VMK_BAD_EXCFRAME;
   }

   if (status == VMK_OK) {
      /*
       * Only restore the following flags from user mode:
       * _AC, _TF, _OF, _DF, _SF, _ZF, _AF, _PF, _CF
       * 
       * Other flags should remain unchanged from whatever their
       * current value is (which may be different from legitimately
       * saved values).
       */
      *newFlags = (oldFlags & (~passable)) | (*newFlags & passable);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * User_CleanFrameCopy --
 *
 *	Given an 'inFrame' from userspace, clean the frame to
 *	make sure the user isn't being subtle and tricky.  It is
 *	assumed that outFrame points to the current frame that got
 *	usermode into the vmkernel (for getting a valid eflags out
 *	of).
 *
 * Results:
 *      VMK_OK if OK, appropriate error code otherwise.
 *
 * Side effects:
 *	*outFrame is modified
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
User_CleanFrameCopy(VMKFullUserExcFrame* const outFrame,
                    const VMKFullUserExcFrame* inFrame)
{
   VMK_ReturnStatus status;
   Reg32 newEflags = inFrame->frame.eflags;

   /*
    * We could restore the entire inFrame on top of the existing
    * outFrame, but most of that stuff will only be changed by
    * illegitimate users (i.e., RawSyscallRiot).  The majority cannot
    * correctly be changed by a user.  So, we'll just copy:
    *    - the basic registers (eax, ebx, ecx, edx, ebp, esi, edi)
    *    - eflags (after cleaning it)
    *    - eip (after range checking it)
    *    - esp
    */

   /* Check that eflags is clean (compare against current) */
   status = UserCheckEFlags(outFrame->frame.eflags, &newEflags);
   if (status != VMK_OK) {
      UWLOG(0, "Failed eflags sanity check (%#x is bad).  Faulting.",
            inFrame->frame.eflags);
      return status;
   }
   
   /* Check that restored eip falls within CS. */
   if (inFrame->frame.eip > VMK_USER_LAST_TEXT_VADDR) {
      UWLOG(0, "Bad eip is out of range (%#x greater than %#x).  Faulting.",
            inFrame->frame.eip, VMK_USER_LAST_TEXT_VADDR);
      return VMK_BAD_EXCFRAME;
   }

   outFrame->frame.eflags = newEflags;
   outFrame->frame.eip = inFrame->frame.eip;
   outFrame->frame.esp = inFrame->frame.esp;
   /* Ignore changes to: %cs, %ss, .errorCode */
   /* Ignore changes to: .pushValue, .gateNum */
   outFrame->regs.eax = inFrame->regs.eax;
   outFrame->regs.ebx = inFrame->regs.ebx;
   outFrame->regs.ecx = inFrame->regs.ecx;
   outFrame->regs.edx = inFrame->regs.edx;
   outFrame->regs.ebp = inFrame->regs.ebp;
   outFrame->regs.esi = inFrame->regs.esi;
   outFrame->regs.edi = inFrame->regs.edi;
   /* Ignore changes to: %es, %ds, %fs, %gs */

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * UserExceptionHandleMappingFault --
 *
 *	Try to handle a fault at the given la/va by playing with
 *	the memory mappings.  If the address is outside the heap
 *	stack, or mmap areas, just return false.
 *
 * Results:
 *      VMK_OK if fault is handled, !VMK_OK if some other remedy must be
 *      found.
 *
 * Side effects:
 *	A page might get mapped.
 *
 *      If the system is low in memory, it may block on trying to
 *      get a page.
 *
 * Would move this into userMem.c, but I want to keep it as an
 * INLINE_SINGLE_CALLER near its caller...
 *
 *----------------------------------------------------------------------
 */
static INLINE_SINGLE_CALLER VMK_ReturnStatus
UserExceptionHandleMappingFault(LA la, uint32 excErrorCode)
{
   VMK_ReturnStatus status;
   const VA va = VMK_USER_LA_2_VA(la);
   Bool isWrite = ((excErrorCode & PF_RW) != 0);
   
   UWLOG(4, "Page fault la=%#x va=%#x", la, va);
   UWLOG(4, "due to %s in %s mode%s%s",         
       (isWrite ? "write":"read"),
       ((excErrorCode & PF_US) ? "user":"supervisor"),
       ((excErrorCode & PF_P) ? " (prot fault)":""),
       ((excErrorCode & PF_RSVD) ? " (RSVD fault)":""));

   if (excErrorCode & PF_RSVD) {
      UWLOG(0, "%s %s va=%#x: reserved bit violation ",
            ((excErrorCode & PF_US) ? "user":"supervisor"),
            (isWrite ? "write":"read"), va);
      status = VMK_NO_ACCESS;  // XXX ??
   } else if (/* (va >= 0) && */
	      (va <= VMK_USER_LAST_VADDR)) {
      do {
         // fault in the page
         status = UserMem_HandleMapFault(MY_RUNNING_WORLD, la, va, isWrite);
         /*
          * Wait for memory reschedule if no memory and/or 
          * the user world exceeds allocated memory target.
          */
         if (status == VMK_NO_MEMORY || 
             MemSched_UserWorldShouldBlock(MY_RUNNING_WORLD)) {
            VMK_ReturnStatus waitStatus;
            UWLOG(2, "Block on memsched: %s", UWLOG_ReturnStatusToString(status));
            waitStatus = MemSched_UserWorldBlock();
            if (waitStatus != VMK_OK) {
               UWLOG(0, "Receive %s while blocking on memsched", 
                     UWLOG_ReturnStatusToString(waitStatus));
               ASSERT(waitStatus == VMK_DEATH_PENDING);
               return waitStatus;
            }
         }
      } while (status == VMK_NO_RESOURCES);
      if (status != VMK_OK) {
         UWLOG((excErrorCode & PF_US) ? 0 : 1,
               "%s %s va=%#x: HandleMapFault: %s",
               ((excErrorCode & PF_US) ? "user":"supervisor"),
               (isWrite ? "write":"read"), va,
               UWLOG_ReturnStatusToString(status));
      }
   } else {
      // VA is outside valid regions.
      UWLOG((excErrorCode & PF_US) ? 0 : 1,
            "%s %s va=%#x: outside user segment ",
            ((excErrorCode & PF_US) ? "user":"supervisor"),
            (isWrite ? "write":"read"), va);
      status = VMK_INVALID_ADDRESS;
   }

   /*
    * If it's a user fault and we can't service it, print out the stack
    * backtrace so we know who the offender is.
    */
   if (excErrorCode & PF_US &&
       (status == VMK_INVALID_ADDRESS || status == VMK_NO_ACCESS)) {
      UWLOG_StackTraceCurrent(0);
   }

   return status;
}

      
/*
 *----------------------------------------------------------------------
 *
 * UserExceptionHandleCopyFault --
 *	Handle a fault with an active handler registered
 *
 * Results:
 *	None.  Always succeeds.
 *
 * Side effects:
 *	excFrame is munged to jump user to handler, and have handler
 *	return failure.
 *
 *----------------------------------------------------------------------
 */
static void
UserExceptionHandleCopyFault(uint32 vector,
                             World_Handle* const curr,
                             VMKFullUserExcFrame* fullFrame,
			     VMK_ReturnStatus copyStatus)
{
   ASSERT(curr == MY_RUNNING_WORLD);
   ASSERT((curr->userLongJumpPC == UserDoCopyInDone)
          || (curr->userLongJumpPC == UserDoCopyOutDone)
          || (curr->userLongJumpPC == UserDoCopyInStringDone));
   /* XXX ASSERT( faulted in kernel mode ) */

   if ((VA)fullFrame->frame.eip > (VA)curr->userLongJumpPC) {
      Warning("Current handler @%#x is before eip(%#X).  Probably not good.",
              (VA)curr->userLongJumpPC, fullFrame->frame.eip);
   }
   
   if (vmx86_devel) {
      if (vector == EXC_GP) {
         UWLOG(1, "GP Fault during User_Copy ec=%#x eip=%#x:%#x (handler@%p)",
               fullFrame->frame.errorCode, fullFrame->frame.cs,
               fullFrame->frame.eip, curr->userLongJumpPC);
      } else {
         ASSERT(vector == EXC_PF);
         VA va, la;
         GET_CR2(la);
         va = VMK_USER_LA_2_VA(la);
         UWLOG(1, "Unhandled page fault during User_Copy ec=%#x eip=%#x:%#x"
               " (handler@%p la=%#x va=%#x)",
               fullFrame->frame.errorCode, fullFrame->frame.cs, fullFrame->frame.eip,
               curr->userLongJumpPC, la, va);
      }
   }
   
   /* Set the copy function return status to failure */
   curr->userCopyStatus = copyStatus;
   /* restart at the error handler registered by the copy routine */
   fullFrame->frame.eip = (uint32)curr->userLongJumpPC;
   /* Disable the jump handler */
   curr->userLongJumpPC = NULL;

   UWLOG(1, "done");
}


/*
 *----------------------------------------------------------------------
 *
 * UserExceptionHandleFault --
 *	Handle the given vector (fault) if possible.  
 *
 * Results:
 *      TRUE if fault is handled, FALSE if some other remedy must be
 *      found.
 *
 * Side effects:
 *	Pages could get mapped, or bits in the excFrame could get
 * 	twiddled.
 *
 *----------------------------------------------------------------------
 */
static INLINE_SINGLE_CALLER Bool
UserExceptionHandleFault(uint32 vector, VMKFullUserExcFrame* fullFrame)
{
   World_Handle* const curr = MY_RUNNING_WORLD;
   VMK_ReturnStatus status = VMK_INVALID_ADDRESS;

   /*
    * First, try to handle page faults that can be fixed by playing
    * with the memory mappings.  Fall through if that doesn't fix it.
    */
   if (vector == EXC_PF) {
      LA la;
      GET_CR2(la);
      UWSTAT_TIMERSTART(pageFaultHandleTime);
      status = UserExceptionHandleMappingFault(la, fullFrame->frame.errorCode);
      if (status == VMK_OK) {
         UWSTAT_TIMERSTOP(pageFaultHandleTime);
         return TRUE;
      } else if (status == VMK_DEATH_PENDING) {
         return FALSE;
      }
   }

   /*
    * Second, if a copy routine handler is installed, munge excFrame
    * to invoke that on GP or PF exceptions.
    */
   if (((vector == EXC_GP) || (vector == EXC_PF))
       && (curr->userLongJumpPC != NULL)) {
      UWSTAT_INC(userCopyFaults);
      UserExceptionHandleCopyFault(vector, curr, fullFrame, status);
      return TRUE;
   }      

   /*
    * There are a few small windows where a userworld might be resumed with
    * TS set. An NM must be from here to be ok.
    */
   if (UNLIKELY(vector == EXC_NM)) {
      uint32 cr0_val;
      GET_CR0(cr0_val);
      ASSERT(myPRDA.configNMI && ((cr0_val & CR0_TS) == CR0_TS));
      return TRUE;
   }

   return FALSE;
}


/*
 *----------------------------------------------------------------------
 *
 * UserExceptionHandleFatal --
 *	A fatal exception has occurred.  Jump to a signal handler, if
 * 	possible.  Dump core and/or drop into the debugger, if that
 *	doesn't work.  Otherwise terminate the current world.
 *
 * 	Do not inline, it would clutter User_Exception.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Core might be dumped or given excFrame might be munged to drop
 *	into debugger or to jump to a signal handler.
 *
 *----------------------------------------------------------------------
 */
static void
UserExceptionHandleFatal(uint32 vector, VMKFullUserExcFrame* fullFrame)
{
   VMK_ReturnStatus status;
   Bool triedToDebug = FALSE;
   
   UWLOG(1, "Exception %d from user level world @ eip=0x%x",
         vector, fullFrame->frame.eip);
   UWLOG(2, "  errorCode=0x%x esp=0x%x es=0x%x ds=0x%x eax=0x%x ebx=0x%x",
         fullFrame->frame.errorCode, fullFrame->frame.esp, 
         fullFrame->regs.es,
         fullFrame->regs.ds,
         fullFrame->regs.eax,
         fullFrame->regs.ebx);
   UWLOG(2, "  ecx=0x%x edx=0x%x esi=0x%x edi=0x%x ebp=0x%x",
         fullFrame->regs.ecx,
         fullFrame->regs.edx,
         fullFrame->regs.esi,
         fullFrame->regs.edi,
         fullFrame->regs.ebp);

   UWLOG_StackTrace(1, fullFrame);

   /*
    * If we've already been in the debugger before, then just go directly back
    * in.
    */
   if (Debug_UWDebuggerIsEnabled() && UserDebug_EverInDebugger()) {
      triedToDebug = TRUE;
      if (UserDebug_Entry(vector)) {
         return;
      }
   }

   /*
    * Perhaps a signal handler will take it.
    */
   status = UserSig_HandleVector(MY_RUNNING_WORLD, vector, fullFrame);
   if (status == VMK_OK) {
      return;
   }

   /*
    * Only go into the debugger if we didn't try above and userworld debugging
    * is enabled.
    */
   if (!triedToDebug && Debug_UWDebuggerIsEnabled()) {
      if (UserDebug_Entry(vector)) {
         return;
      }
   }

   /*
    * Since the debugger didn't run, we should try to dump core and prepare to
    * exit.
    */
   User_CartelShutdown(CARTEL_EXIT_SYSERR_BASE+LINUX_SIGSEGV, TRUE, fullFrame);

   /*
    * This is a clean termination point (from the point of view of the
    * kernel), so we can exit here.
    */
   ASSERT(MY_USER_THREAD_INFO->dead == TRUE);
   World_Exit(VMK_OK);
   NOT_REACHED();
}


/*
 *----------------------------------------------------------------------
 *
 * User_Exception --
 *
 *      Handle an exception from a userworld.
 *
 * Results:
 *      Does not return (returns via given excFrame)
 *
 * Side effects:
 *      A user page may be faulted in or a user world may be killed.
 *
 *----------------------------------------------------------------------
 */
void
User_Exception(World_Handle* currentWorld, uint32 vector, VMKExcFrame *excFrame)
{
   VMKFullUserExcFrame* fullFrame = VMKEXCFRAME_TO_FULLUSERFRAME(excFrame);
   const Bool wasPreemptible = CpuSched_DisablePreemption();
   const Bool fromUserMode = User_SegInUsermode(excFrame->cs);
   User_ThreadInfo* const uti = currentWorld->userThreadInfo;

   ASSERT(currentWorld == MY_RUNNING_WORLD);
   ASSERT(excFrame != NULL);
   ENABLE_INTERRUPTS();
   UWSTAT_INC(exceptions);

   /*
    * Save user exception frame (unless this is an in-kernel fault).
    *
    * Note, we may have been in VMKernel already -- if kernel
    * page-faults on invalid/unmapped user address.
    */
   if (fromUserMode) {
      ASSERT(wasPreemptible);
      uti->exceptionFrame = fullFrame;
   } else {
      /* If we were in the kernel, then we have to have a fall-back. */
      ASSERT(currentWorld->userLongJumpPC != NULL);
   }

   ASSERT(uti->exceptionFrame != NULL);

   UWLOG_SetContextException(vector);

   if (UNLIKELY(! UserExceptionHandleFault(vector, fullFrame))) {
      /*
       * In-kernel faults should have a handler, and should not get
       * this far.  If they do we risk blocking another thread
       * indefinitely if we're holding an in-kernel semaphore.
       */
      ASSERT(fromUserMode);
      ASSERT(currentWorld->userLongJumpPC == NULL);

      /*
       * If we can't handle the fault via a simple mapping or registered
       * fault handler, then something is wrong, and the world must die
       * (or fall into the debugger, at least).
       */
      UserExceptionHandleFatal(vector, fullFrame);
   }

   if (fromUserMode) {
      // May have been terminated on exception path.  Only make this fatal
      // from user mode; fatal faults in kernel mode may require
      // kernel-side cleanup.
      if (uti->dead || currentWorld->deathPending) {
         UWLOG(1, "Termination requested.  Dying.");
         World_Exit(VMK_OK);
         NOT_REACHED();
      }

      uti->exceptionFrame = NULL;
   }
   UWLOG_ClearContext();

   CLEAR_INTERRUPTS();

   ASSERT(!CpuSched_IsPreemptible());
   CpuSched_RestorePreemption(wasPreemptible);

   asm("clts");
   CommonRet(excFrame);
   NOT_REACHED();
}


/*
 *----------------------------------------------------------------------
 *
 * User_CartelShutdown --
 *
 *      Shutdown the current cartel.  All threads are tagged for
 *      termination.
 * 
 * Results:
 *      None.
 *
 * Side effects:
 *	Changes saved shutdown state for given cartel.
 *
 *----------------------------------------------------------------------
 */
void
User_CartelShutdown(int exitCode,
                    Bool wantCoreDump,
                    VMKFullUserExcFrame* fullFrame)
{
   User_CartelInfo* uci = MY_USER_CARTEL_INFO;
   VMK_ReturnStatus status;

   ASSERT(World_IsUSERWorld(MY_RUNNING_WORLD)); // don't call from a helper

   if (wantCoreDump && uci->coreDump.enabled) {
      ASSERT(fullFrame != NULL);

      status = UserDump_CoreDump();
      // May return VMK_BUSY, which is fine.
      if (status != VMK_OK) {
         UWLOG(0, "Dump returned: %s", UWLOG_ReturnStatusToString(status));
      }
   }

   UserSetShutdownState(uci, exitCode, 0, NULL);
   User_CartelKill(MY_RUNNING_WORLD, FALSE);
   UserThread_SetExitStatus(exitCode);
   ASSERT(MY_USER_THREAD_INFO->dead == TRUE);
}   


/*
 *----------------------------------------------------------------------
 *
 * UserSetShutdownState --
 *
 *      Update the shutdown state for a cartel with given info.
 * 
 *	May be called from a BH.
 *
 *	Note: this should only be called from User_CartelShutdown.
 *	Do not call it directly.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	Changes saved shutdown state for given cartel.
 *
 *----------------------------------------------------------------------
 */
void
UserSetShutdownState(
   User_CartelInfo* uci,             // IN: current cartel's shared state
   int exitCode,                     // IN: exit status to pass in RPC
   int exceptionType,                // IN: exceptionType (only if fullFrame != NULL)
   const VMKFullUserExcFrame* fullFrame) // IN: full exception frame (or NULL)
{
   /*
    * Record the coredumping thread's interpretation of just what
    * went wrong.  This info will be pushed out to the COS proxy
    * in UserWorldCleanupCartelInfo.
    */
   if (uci->coreDump.dumperWorld == INVALID_WORLD_ID ||
       uci->coreDump.dumperWorld == MY_RUNNING_WORLD->worldID) {
      UWLOG(0, "exitCode=%d/exceptionType=%d, coreDump=%s",
            exitCode, exceptionType, uci->coreDump.dumpName);
      uci->shutdown.exitCode = exitCode;
      uci->shutdown.exceptionType = exceptionType;
      uci->shutdown.hasException = (fullFrame != NULL);
      if (fullFrame != NULL) {
         uci->shutdown.exceptionFrame = *fullFrame;
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * User_CartelKill --
 *
 *	Kill the given world and all of its peers (i.e., terminate the
 *	entire cartel).  The running world will not World_Kill itself
 *	via this function.  See World_Kill for per-world semantics.
 *
 * Results:
 *	VMK_OK if all killed, VMK_BUSY if all others were killed but
 *	current world is a peer (and is still kicking around).
 *
 * Side effects:
 *	Worlds are slaughtered left and right
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
User_CartelKill(World_Handle* w, Bool vicious)
{
   static const User_PreExitMsg preExit = { .type = USER_MSG_PREEXIT };
   VMK_ReturnStatus status;

   ASSERT(w != NULL);
   ASSERT(w->userCartelInfo != NULL);
   ASSERT(World_IsUSERWorld(w));

   if (!vicious) {
      /*
       * TODO: Start a timer for a vicious kill of the cartel....
       * See Bug 39985.
       */
   }

   /*
    * Tell the proxy that we're going to die, so it can kick any
    * mid-flight RPCs back out.
    */
   UWLOG(2, "Sending PreExitMsg for cartel %d to proxy",
         w->userCartelInfo->cartelID);
   status = UserProxy_SendStatusAlert(w->userCartelInfo->cartelID,
                                      &preExit, sizeof preExit);
   if (status != VMK_OK) {
      UWWarn("Error informing proxy of cartel termination: %s",
             UWLOG_ReturnStatusToString(status));
   }

   return UserThread_KillPeers(&w->userCartelInfo->peers, vicious);
}


/*
 *----------------------------------------------------------------------
 *
 * UserWorldInitThreadInfo --
 *	Initialize thread-private world state for a user world.
 *
 * Results:
 *      VMK_OK if okay, otherwise if init should be aborted.
 *
 * Side effects:
 *      Allocates and initializes per-thread state
 *
 *----------------------------------------------------------------------
 */
static INLINE_SINGLE_CALLER VMK_ReturnStatus
UserWorldInitThreadInfo(World_Handle* world)
{
   VMK_ReturnStatus status = VMK_OK;
   User_CartelInfo* const uci = world->userCartelInfo;
   User_ThreadInfo* uti;

   status = UserThread_Add(&uci->peers, world);
   if (status != VMK_OK) {
      return status;
   }

   uti = User_HeapAlloc(uci, sizeof(User_ThreadInfo));
   if (uti == NULL) {
      return VMK_NO_MEMORY;
   }

   uti->dead = FALSE;
   uti->selectTimer = TIMER_HANDLE_NONE;
   uti->exceptionFrame = NULL;

   /* XXX handle initialization errors */

   status = UserStat_ThreadInit(&uti->threadStats,
                                world->worldID,
                                uci->heap,
                                &uci->cartelStats);
   ASSERT_NOT_IMPLEMENTED(status == VMK_OK);

   status = UserLog_ThreadInit(uti);
   ASSERT_NOT_IMPLEMENTED(status == VMK_OK);

   status = UserSig_ThreadInit(uti);
   if (status == VMK_OK) {
      memset(&uti->waitInfo, 0, sizeof uti->waitInfo);
      
      world->userThreadInfo = uti;
   } else {
      User_HeapFree(uci, uti);
      return status;
   }

   status = UserTime_ThreadInit(uti);
   ASSERT_NOT_IMPLEMENTED(status == VMK_OK);

   status = UserMem_ThreadInit(uti, world);
   ASSERT_NOT_IMPLEMENTED(status == VMK_OK); 

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserWorldCreateCartelInfo --
 *
 *	Allocate and initialize a new cartelInfo for the given world.
 *
 * Results:
 *      VMK_OK if all is right, otherwise if the initialization should
 *      abort.
 *
 * Side effects:
 *	Allocates memory, initializes all modules hanging off cartel init.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserWorldCreateCartelInfo(World_Handle* world)      // IN
{
   VMK_ReturnStatus status;
   Heap_ID heap;
   User_CartelInfo* uci;
   int i;

   ASSERT(world != NULL);
   ASSERT(world != MY_RUNNING_WORLD);
   ASSERT(World_IsUSERWorld(world));

   /*
    * Create the new cartel's private heap first, so we can allocate
    * the uci on it.
    */
   heap = UserNewHeap(world->worldID);
   if (heap == INVALID_HEAP_ID) {
      return VMK_NO_MEMORY;
   }
   
   uci = Heap_Alloc(heap, sizeof(User_CartelInfo));
   if (uci == NULL) {
      UWWarn("brand new heap, but allocation failed.");
      UserDestroyHeap(heap);
      return VMK_NO_MEMORY;
   }
   
   UWLOGFor(1, world, "new uci @ %p", uci);
   
   memset(uci, 0, sizeof(*uci));
   uci->heap = heap;
   
   /*
    * Assume this cartel will shutdown cleanly.  
    */
   uci->shutdown.exitCode = 0;
   uci->shutdown.exceptionType = 0;
   uci->shutdown.hasException = 0;

   /*
    * The set of threads that run in the same address space
    * all share a "cartelID".  This ID is the ID
    * of the initial world in the cartel.  Thus, this ID
    * is the ID used by the COS program that created
    * the first world.
    */
   uci->cartelID = world->worldID;
   world->userCartelInfo = uci;
   Atomic_Write(&uci->refCount, 1);
   
   for (i = 0; i < ARRAYSIZE(userCartelInitTable); i++) {
      UWLOGFor(1, world, "Initializing %s", userCartelInitTable[i].name);
      status = userCartelInitTable[i].init.uciWorldFn(uci, world);
      if (status != VMK_OK) {
         break;
      }
   }

   if (status != VMK_OK) {
      UWWarn("Error during %s in cartel setup.", userCartelInitTable[i].name);
      i--; // skip the cleanup for the init function that failed
      for (; i >= 0; i--) {
         VMK_ReturnStatus tmpStatus;
         tmpStatus = userCartelInitTable[i].cleanup.uciWorldFn(uci, world);
         if (tmpStatus != VMK_OK) {
            UWWarn("Error during startup %s, error during cleanup: %s",
                   UWLOG_ReturnStatusToString(status),
                   UWLOG_ReturnStatusToString(tmpStatus));
         }
      }
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserWorldInitCartelInfo --
 *	Initialize state shared among a cartel of threads
 *
 * Results:
 *      VMK_OK if all is right, otherwise if the initialization should
 *      abort.
 *
 * Side effects:
 *      Allocates and initializes group state if this is the first
 * 	thread in the group, picks up leader's state otherwise.
 *
 *----------------------------------------------------------------------
 */
static INLINE_SINGLE_CALLER VMK_ReturnStatus
UserWorldInitCartelInfo(World_Handle* world)
{
   VMK_ReturnStatus status = VMK_OK;
   
   /*
    * If we're initializing a new thread in a cartel, then
    * the new thread share's the creator's User_CartelInfo.
    * Otherwise, create a new User_CartelInfo
    */
   if (World_IsCLONEWorld(world)) {
      ASSERT(MY_RUNNING_WORLD != world);
      world->userCartelInfo = MY_RUNNING_WORLD->userCartelInfo;
      UWLOGFor(1, world, "sharing creator's uci @ %p; cartelID=%#x",
               world->userCartelInfo, world->userCartelInfo->cartelID);
      
      ASSERT(Atomic_Read(&world->userCartelInfo->refCount) >= 1);
      Atomic_Inc(&world->userCartelInfo->refCount);
   } else {
      status = UserWorldCreateCartelInfo(world);
   }
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserWorldCleanupThreadInfo --
 *	Cleanup per thread-private user world state.
 *
 * Results:
 *      Cleaner world.
 *
 * Side effects:
 *      Free's world's userThreadInfo
 *
 *----------------------------------------------------------------------
 */
static INLINE_SINGLE_CALLER void
UserWorldCleanupThreadInfo(World_Handle* world) 
{
   VMK_ReturnStatus status;
   User_ThreadInfo* const uti = world->userThreadInfo;
   User_CartelInfo* const uci = world->userCartelInfo;

   if (uti != NULL) {
      /* XXX fix ASSERT_NOT_IMPLEMENTED */
      status = UserThread_Remove(&uci->peers, world);
      ASSERT_NOT_IMPLEMENTED(status == VMK_OK);
      status = UserMem_ThreadCleanup(uti, world);
      ASSERT_NOT_IMPLEMENTED(status == VMK_OK);
      status = UserTime_ThreadCleanup(uti);
      ASSERT_NOT_IMPLEMENTED(status == VMK_OK);
      status = UserSig_ThreadCleanup(uti);
      ASSERT_NOT_IMPLEMENTED(status == VMK_OK);
      status = UserStat_ThreadCleanup(&uti->threadStats, uci->heap);
      ASSERT_NOT_IMPLEMENTED(status == VMK_OK);
      User_HeapFree(uci, uti);
   } else {
      UWLOGFor(0, world, "Odd.  world->userThreadInfo is already NULL");
   }
   world->userThreadInfo = NULL;
}


/*
 *----------------------------------------------------------------------
 *
 * UserWorldCleanupCartelInfo --
 *	Cleanup shared user-world state.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Cleans and free's world user state if this is the last thread
 *      to leave the cartel
 *
 *----------------------------------------------------------------------
 */
static INLINE_SINGLE_CALLER void
UserWorldCleanupCartelInfo(World_Handle* world)
{
   User_CartelInfo* uci = world->userCartelInfo;

   if (uci != NULL) {
      uint32 oldref = Atomic_FetchAndDec(&uci->refCount);

      if (oldref == 1) {
         VMK_ReturnStatus status = VMK_OK;
         Heap_ID heap;
         int i;

         UWLOGFor(2, world, "Cleaning up (no-longer shared) state %p", uci);

         /*
          * Kill the vmm world if it's still running.  We need to make
          * sure the VMM is destroyed before cleaning up the
          * potentially shared bits of the cartel info (UserMem is
          * really the important case).
          */
         World_DestroyVmms(world, TRUE, TRUE);

         for (i = ARRAYSIZE(userCartelInitTable) - 1; i >= 0; i--) {
            VMK_ReturnStatus cs;
            UWLOGFor(1, world, "Cleanup %s", userCartelInitTable[i].name);
            cs = userCartelInitTable[i].cleanup.uciWorldFn(uci, world);
            if (cs != VMK_OK) {
               UWWarn("Cleanup entry %s failed: %s.  Ignoring ... ",
                      userCartelInitTable[i].name,
                      UWLOG_ReturnStatusToString(status));
               status = cs;
            }
         }

         heap = uci->heap;
         Heap_Free(heap, uci);

         if (status == VMK_OK) {
            UserDestroyHeap(heap);
         } else {
            UWWarn("Leaving heap unreclaimed: %s", UWLOG_ReturnStatusToString(status));
         }
      }
   } else {
      UWLOGFor(0, world, "Odd. world->userCartelInfo was already NULL.");
   }
   world->userCartelInfo = NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * User_WorldInit --
 *
 *      Initialize a user world.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The world's thread-private and threadgroup-private state is
 *	initialized.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
User_WorldInit(World_Handle *world, World_InitArgs *args)
{
   VMK_ReturnStatus status = VMK_OK;

   if (!World_IsUSERWorld(world)) {
      UWWarn("Initialization of world %d should not "
             "use 'user' table entry (world type=%#x)",
             world->worldID, world->typeFlags);
      status = VMK_OK;
   } else {
      UWLOGFor(3, world, "initializing per-group user-world state");
      status = UserWorldInitCartelInfo(world);
      if (status == VMK_OK) {
         UWLOGFor(3, world, "initializing per-thread user-world state");
         status = UserWorldInitThreadInfo(world);
         if (status != VMK_OK) {
            UserWorldCleanupCartelInfo(world);
         }
      }
   }      

   UWLOGFor(3, world, "complete.");
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * User_WorldCleanup --
 *
 *      Cleanup user world state.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      All state for this world is closed or deleted.
 *
 *----------------------------------------------------------------------
 */
void
User_WorldCleanup(World_Handle *world)
{
   ASSERT(world);
   ASSERT(World_IsUSERWorld(world));

   UserWorldCleanupThreadInfo(world);
   UserWorldCleanupCartelInfo(world);
}


/*
 *----------------------------------------------------------------------
 *
 * UserSetupWorkingDirectory --
 *
 *      Set the working directory for the given world to the given
 *      name.
 *
 * Results:
 *      VMK_OK if working directory was set, otherwise if an error
 *      occurred.
 *
 * Side effects:
 *      Working directory for given world is updated.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserSetupWorkingDirectory(User_CartelInfo* uci,
                          const char* dirName)
{
   VMK_ReturnStatus status;
   UserObj* obj;

   status = UserObj_Open(uci, dirName, USEROBJ_OPEN_STAT, 0, &obj);
   if (status == VMK_OK) {
      status = UserObj_Chdir(uci, obj);
   }

   UWLOG(2, "UserObj_Open(%s) returned %s",
         dirName, UWLOG_ReturnStatusToString(status));

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserSetErrorMsg --
 *
 *	Fills in the given User_ErrorMsg struct so that it can be passed out to
 *	the proxy.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
UserSetErrorMsg(User_ErrorMsg *errMsg, VMK_ReturnStatus status, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(errMsg->str, sizeof errMsg->str, fmt, ap);
   va_end(ap);

   if (status >= VMK_GENERIC_LINUX_ERROR) {
      errMsg->err = - User_TranslateStatus(status);
   } else {
      errMsg->err = 0;
      if (status != 0) {
	 snprintf(errMsg->str, sizeof errMsg->str, "%s: %s", errMsg->str,
		  UWLOG_ReturnStatusToString(status));
      }
   }

   errMsg->type = USER_MSG_ERROR;
}


/*
 *----------------------------------------------------------------------
 *
 * UserLoaderInit --
 *
 *	Set up things so that the loader will run properly.
 *
 * Results:
 *	VMK_OK on success.
 *
 * Side effects:
 *	Regions are mmaped, data is pushed on the stack.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserLoaderInit(UserAuxVec* kauxVec, User_ErrorMsg* errMsg)
{
   World_Handle *world = MY_RUNNING_WORLD;
   User_CartelInfo* uci = world->userCartelInfo;
   VMK_ReturnStatus status;
   User_FileInfo* fileInfo;
   User_MapInfo* mapInfo;

   SET_AUX_VEC(kauxVec, 0, AUXVEC_AT_PAGESIZE, PAGE_SIZE);
   SET_AUX_VEC(kauxVec, 1, AUXVEC_AT_PHDR, uci->args.ldInfo.phdr);
   SET_AUX_VEC(kauxVec, 2, AUXVEC_AT_PHENT, uci->args.ldInfo.phent);
   SET_AUX_VEC(kauxVec, 3, AUXVEC_AT_PHNUM, uci->args.ldInfo.phnum);
   SET_AUX_VEC(kauxVec, 4, AUXVEC_AT_BASE, uci->args.ldInfo.base);
   SET_AUX_VEC(kauxVec, 5, AUXVEC_AT_ENTRY, uci->args.ldInfo.entry);
   SET_AUX_VEC(kauxVec, 6, AUXVEC_AT_NULL, 0);

   /*
    * First, go through and open all the files the user specified.
    */
   for (fileInfo = uci->args.fileHead; fileInfo != NULL;
	fileInfo = fileInfo->next) {
      status = UserObj_Open(uci, fileInfo->name, USEROBJ_OPEN_RDONLY, 0,
			    &fileInfo->obj);
      if (status != VMK_OK) {
         UserSetErrorMsg(errMsg, status, "Failed to open %s", fileInfo->name);
         return status;
      }
   }

   /*
    * Now we can go through the map sections list, mapping in each region.
    */
   for (mapInfo = uci->args.mapHead; mapInfo != NULL; mapInfo = mapInfo->next) {
      UserObj* obj = NULL;
      UserVA addr = mapInfo->addr;

      /*
       * Find which file we're supposed to be using.
       */
      if (! (mapInfo->flags & LINUX_MMAP_ANONYMOUS)) {
         ASSERT(mapInfo->id >= 0);

	 for (fileInfo = uci->args.fileHead; fileInfo != NULL;
	      fileInfo = fileInfo->next) {
	    if (fileInfo->id == mapInfo->id) {
	       obj = fileInfo->obj;
	       break;
	    }
	 }

	 if (obj == NULL) {
	    UserSetErrorMsg(errMsg, 0, "Invalid file id given for a map section.");
	    return VMK_BAD_PARAM;
	 }
      }

      status = UserMem_MapObj(world, &addr, mapInfo->length, mapInfo->prot,
			      mapInfo->flags, obj, VA_2_VPN(mapInfo->offset),
			      TRUE);
      if (status != VMK_OK) {
         UserSetErrorMsg(errMsg, status, "Failed to map section");
         return status;
      }

      /*
       * vmkload_app will always call with MAP_FIXED, thus we should always get
       * the address we requested.
       */
      ASSERT(addr == mapInfo->addr);

      /*
       * Check if we need to explicitly zero part of this mmap'ed region.  See
       * apps/vmkload_app/vmkelf.c for details.
       */
      if (mapInfo->zeroAddr) {
         uint32 zeroLength = ALIGN_UP(mapInfo->zeroAddr, PAGE_SIZE)  -
			     mapInfo->zeroAddr;
	 ASSERT(zeroLength < PAGE_SIZE);
	 if (zeroLength) {
	    uint8* data = User_HeapAlloc(uci, zeroLength);
	    memset(data, 0, zeroLength);
	    status = User_CopyOut(mapInfo->zeroAddr, data, zeroLength);
	    User_HeapFree(uci, data);
	    if (status != VMK_OK) {
	       UserSetErrorMsg(errMsg, status, "Failed to zero region (addr=%#x len=%d)",
                               mapInfo->zeroAddr, zeroLength);
	       return status;
	    }
	 }
      }

   }

   /*
    * Finally, go back through and release the refcount we hold on the UserObjs
    * and free data we allocated.
    * Note: We told UserMem_MapObj to inc the refcount for us, so we don't need
    * to worry about references held by it.
    */
   for (fileInfo = uci->args.fileHead; fileInfo != NULL; ) {
      User_FileInfo* nextFileInfo;

      status = UserObj_Release(uci, fileInfo->obj);
      if (status != VMK_OK) {
         UserSetErrorMsg(errMsg, status, "Failed to release object");
         return status;
      }

      nextFileInfo = fileInfo->next;
      User_HeapFree(uci, fileInfo);
      fileInfo = nextFileInfo;
   }
   uci->args.fileHead = NULL;

   for (mapInfo = uci->args.mapHead; mapInfo != NULL; ) {
      User_MapInfo* nextMapInfo;

      nextMapInfo = mapInfo->next;
      User_HeapFree(uci, mapInfo);
      mapInfo = nextMapInfo;
   }
   uci->args.mapHead = NULL;
   uci->args.mapTail = NULL;

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * User_WorldStart --
 *
 *      Start a user world running.
 *
 * Results:
 *      This function does not return.
 *
 * Side effects:
 *      The user world's stack is initialized and the user world 
 *	starts running.
 *
 *----------------------------------------------------------------------
 */
void
User_WorldStart(World_Handle* world, void* userStartFunc)
{
   User_Arg *karg;
   int totalLength;
   UserVA userArgValues;
   UserVA userArgVector;
   UserVA userEnvValues;
   UserVA userEnvVector;
   UserAuxVec kauxVec[7];
   UserVA userAuxVectorAddr;
   UserVA userArgcAddr;
   UserVA userEsp;
   UserVA userStackEnd;
   size_t i;
   User_CartelInfo* uci = world->userCartelInfo;
   VMKFullUserExcFrame fullFrame;
   VMKUserExcFrame* excFrame;   
   VMK_ReturnStatus status = VMK_OK;
   static const uint32 zero = 0;
   static const uint16 dataSelector = MAKE_SELECTOR_UNCHECKED(DEFAULT_USER_DATA_DESC, 0, 3);
   const char *envp;
   User_ErrorMsg errMsg;

   /* all worlds start with preemption disabled */
   ASSERT(!CpuSched_IsPreemptible());

   /*
    * Setup a fullFrame in case we trip into the debugger or core dump
    * before entering usermode.
    */
   memset(&fullFrame, 0, sizeof fullFrame);
   world->userThreadInfo->exceptionFrame = &fullFrame;

   UWLOG(2, "setting up user world stack");

   excFrame = &fullFrame.frame;
   excFrame->eip = (uint32)userStartFunc;
   excFrame->cs = MAKE_SELECTOR(DEFAULT_USER_CODE_DESC, 0, 3);
   excFrame->eflags = EFLAGS_IF;
   excFrame->ss = dataSelector;
   excFrame->errorCode = 0xbeefd00d;
   /* Note: %es and %ds are initialized in StartUserWorld */

   /* Setup working directory */
   status = VMK_FAILURE;
   if (uci->args.workingDirName != NULL) {
      status = UserSetupWorkingDirectory(uci, uci->args.workingDirName);
   }

   if (status != VMK_OK) {
      UWLOG(0, "No valid working directory provided, defaulting to '/'");
      status = UserSetupWorkingDirectory(uci, "/");
   }

   if (status != VMK_OK) {
      UserSetErrorMsg(&errMsg, status, "Failed to set working directory");
      goto bail;
   }

   /*
    * Get the loader ready to go.
    */
   status = UserLoaderInit(kauxVec, &errMsg);
   if (status != VMK_OK) {
      goto bail;
   }

   /*
    * Map in the heap and stack.
    */
   status = UserMem_InitAddrSpace(world, &userStackEnd);
   if (status != VMK_OK) {
      UserSetErrorMsg(&errMsg, status, "Failed to set up address space");
      goto bail;
   }

   /*
    * Setup the stack for the new process.  While we don't follow the
    * System V ABI spec for Intel386, we want to be easily compatible
    * with glibc, so we set the stack up in a similar way.  We include
    * a dummy "environment", an "auxilary vector" and argv and argc.
    * See Figure 3-31 in the System V Application Binary Interface
    * Intel 386 Architecture Processor Supplement, Fourth Edition.
    *
    * TODO: Pull necessary environment variables from the proxy.
    */

   /*
    * All environment and argument contents are at the top
    * of the stack, followed by the auxvec, envp and argv
    * arrays. Compute how much space is needed.
    */
   totalLength = 0;

   /* Compute environment size */
   for (i = 0; i < uci->args.envInfo->numVars; i++) {
      envp = uci->args.envInfo->environ[i];
      ASSERT(envp != NULL);
      UWLOG(3, "environment entry %d is \"%s\"", i, envp);
      do {
         totalLength++;
      } while (*envp++);
   }
   userEnvValues = userStackEnd - totalLength;

   /* Compute argument size */
   for (karg = uci->args.head; karg != NULL; karg = karg->next) {
      totalLength += karg->length;
   }
   userArgValues = userStackEnd - totalLength;
   totalLength = ALIGN_UP(totalLength, sizeof(int)); /* align */

   /* Space for aux vec */
   totalLength += sizeof kauxVec;
   userAuxVectorAddr = userStackEnd - totalLength;

   /* Space for environment array (+ list trailing NULL) */
   totalLength += (sizeof(char *) * (uci->args.envInfo->numVars + 1));
   userEnvVector = userStackEnd - totalLength;

   /* Space for argv array (+ list trailing NULL) */
   totalLength += (sizeof(char *) * (uci->args.num + 1)); /* argv */
   userArgVector = userStackEnd - totalLength;

   /* Space for argc */
   totalLength += sizeof(int); /* argc */
   userArgcAddr = userStackEnd - totalLength;

   userEsp = userStackEnd - totalLength;
   /* stack addresses must be word aligned */
   ASSERT((VA)userEsp == ALIGN_UP((VA)userEsp, sizeof(int)));

   UWLOG(2, "startFunc=%p; userEsp=%#x", userStartFunc, userEsp);

   /* Copy environment onto stack and fill the env vector */
   UWLOG(2, "Copying environment onto user mode stack...");
   for (i = 0; i < uci->args.envInfo->numVars; i++) {
      envp = uci->args.envInfo->environ[i];
      UWLOG(3, "Copying envp[%d] (%#x) onto user mode stack @ %#x.",
            i, userEnvValues, userEnvVector + (i * sizeof(char *)));
      status = User_CopyOut(userEnvVector + (i * sizeof(char *)),
                            &userEnvValues, sizeof(char *));
      if (status != VMK_OK) {
         UserSetErrorMsg(&errMsg, status, "Failed to copy environment data");
         goto bail;
      }
      UWLOG(3, "environment item number %d onto user mode stack @ %#x",
            i, userEnvValues);
      do {
        status = User_CopyOut(userEnvValues++, envp, sizeof(char));
        if (status != VMK_OK) {
           UserSetErrorMsg(&errMsg, status, "Failed to copy environment data");
           goto bail;
        }
      } while (*envp++);
   }

   UWLOG(3, "NULL-terminating userEnvVector @ %#x",
         userEnvVector + (i * sizeof(char *)));
   status = User_CopyOut(userEnvVector + (i * sizeof(char *)),
                         &zero, sizeof zero);
   if (status != VMK_OK) {
      UserSetErrorMsg(&errMsg, status, "Failed to copy environment to stack");
      goto bail;
   }

   /*
    * Copy argument contents into place and initialize argv.  Free
    * the kernel arg elements as we go.  Continue to free them, at least,
    * if any errors occur (we just stop copying values out on the first
    * error).
    */
   UWLOG(2, "Copying argv values onto user mode stack...");
   for (i = 0, karg = uci->args.head; i < uci->args.num;
	i++, karg = karg->next) {
      if (karg == NULL) {
         UserSetErrorMsg(&errMsg, VMK_BAD_PARAM,
                         "Unexpected NULL argument in userworlds karg[%d].", i);
         goto bail;
      }

      UWLOG(3, "Copying argv value %d onto user mode stack @ %#x.",
	    i, userArgValues);
      ASSERT(userArgValues > userEsp);
      ASSERT(userArgValues + karg->length <= userStackEnd);
      // Copy the value out to stack
      status = User_CopyOut(userArgValues, karg->arg, karg->length);
      if (status == VMK_OK) {
	 UWLOG(3, "Copying argv[%d] (%#x) onto user mode stack @ %#x.",
	       i, userArgValues, userArgVector+(i*sizeof(char*)));
	 // Copy pointer to the value into argv
	 status = User_CopyOut(userArgVector+(i*sizeof(char*)), &userArgValues,
			       sizeof(char*));
      }
      if (status != VMK_OK) {
	 UserSetErrorMsg(&errMsg, status, "Failed to copy arguments to stack");
	 goto bail;
      }

      userArgValues += karg->length;
   }

   UWLOG(3, "NULL-terminating userArgVector@%#x",
         userArgVector+(i*sizeof(char*)));
   status = User_CopyOut(userArgVector+(i*sizeof(char*)), &zero, sizeof zero);
   if (status != VMK_OK) {
      UserSetErrorMsg(&errMsg, status, "Failed to copy arguments to stack");
      goto bail;
   }

   /* Copy aux vec out to user stack */
   UWLOG(3, "Copying aux vec %p (%d bytes) to %#x",
         kauxVec, sizeof kauxVec, userAuxVectorAddr);
   status = User_CopyOut(userAuxVectorAddr, &kauxVec, sizeof kauxVec);
   if (status != VMK_OK) {
      UserSetErrorMsg(&errMsg, status, "Failed to copy aux vec to stack");
      goto bail;
   }

   /* Copy argc count out to user stack */
   UWLOG(3, "Copying argc (%d) to %#x", uci->args.num, userArgcAddr);
   status = User_CopyOut(userArgcAddr, &uci->args.num, sizeof(uci->args.num));
   if (status != VMK_OK) {
      UserSetErrorMsg(&errMsg, status, "Failed to copy argc to stack");
      goto bail;
   }
   
   /* Setup initial register state */
   excFrame->esp = (uint32)userEsp;

   /* Cleanup all the initialization arguments. */
   UserInit_CartelCleanup(uci);
	    
   UWLOG(3, "Initialization complete.  Switching to user mode (using %p as stack)...",
         excFrame);
   ASSERT(!CpuSched_IsPreemptible());
   CpuSched_EnablePreemption();
   StartUserWorld(excFrame, dataSelector);
   UWLOG(2, " StartUserWorld returned! Exiting ... ");
   UserThread_SetExitStatus(0);
   World_Exit(VMK_OK);
   NOT_REACHED();

  bail:
   UWWarn("Bailing due to error in initial cartel setup: %s",
   	  errMsg.str);
   UserProxy_SendStatusAlert(uci->cartelID, &errMsg, sizeof errMsg);
   User_CartelShutdown(CARTEL_EXIT_SYSERR_BASE, FALSE, NULL);
   /*
    * This is a clean termination point. 
    */
   ASSERT(MY_USER_THREAD_INFO->dead == TRUE);
   World_Exit(status);
   NOT_REACHED();
}

/*
 *----------------------------------------------------------------------
 *
 * User_PSharePage --
 *
 *      PShare the page at the given virtual page number.
 *
 * Results:
 *      VMK_OK.
 *
 * Side effects:
 *      The page is marked as readonly and the MPN could be changed as
 *      a result of page sharing.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
User_PSharePage(World_Handle *world, // IN: World to add page to
                VPN vpn)             // IN: VPN of the page
{
   ASSERT(world);
   ASSERT(world->userCartelInfo);
   UserMem_PSharePage(world, vpn);
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * User_MarkSwapPage --
 *
 *      Callback function for a page that has been swapped out.
 *      Mark a page "wpn" as swapped in the user world's page table.
 *
 * Results:
 *      TRUE if the page has been swapped.
 *
 * Side effects:
 *      MPN could be freed.
 *
 *----------------------------------------------------------------------
 */
Bool
User_MarkSwapPage(World_Handle *world, // IN: the user world
                  uint32 reqNum,       // IN: req number in the swap req list
                  Bool writeFailed,    // IN: TRUE if swap out failed
                  uint32 swapFileSlot, // IN: swap file slot number
                  PPN ppn,             // IN: PPN of the page
                  MPN mpn)             // IN: MPN of the page
{
   ASSERT(world);
   ASSERT(world->userCartelInfo);
   return UserMem_MarkSwapPage(world, reqNum, writeFailed, swapFileSlot, (LPN)ppn, mpn);
}


/*
 *----------------------------------------------------------------------
 *
 * User_SwapOutPage --
 *
 *      Try to swap out "numPages" of pages from the userworld.
 *
 * Results:
 *      Number of pages that will be swapped out.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
uint32
User_SwapOutPages(World_Handle *world, uint32 numPages)
{
   ASSERT(World_IsUSERWorld(world));
   return UserMem_SwapOutPages(world, numPages);
}


/*
 *----------------------------------------------------------------------
 *
 * User_GetPageMPN --
 *
 *      Get the mpn for the given vpn in the userworld cartel.
 *
 * Results:
 *      VMK_OK if a valid mpn is written to *mpnOut.
 *
 * Side effects:
 *      Page is allocated if not already exist. 
 *      Page may be pinned.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
User_GetPageMPN(World_Handle *world, VPN vpn, UserPageType pageType, MPN *mpnOut)
{
   VMK_ReturnStatus status;
   VA va = VPN_2_VA(vpn);
   LA la = VMK_USER_VA_2_LA(va);
   
   *mpnOut = INVALID_MPN;
   status = UserMem_HandleMapFault(world, la, va, TRUE);

   if (status == VMK_OK) {
      return UserMem_LookupMPN(world, vpn, pageType, mpnOut);
   } else {
      UWWarn("vpn 0x%x status %d", vpn, status);
      return status;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * User_Wakeup --
 *
 *	Wakeup the blocked userworld.  Only invoked for a user world
 *	that is waiting on a select semaphore in the main part of the
 *	vmkernel (e.g., in the TCP/IP stack).
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
void 
User_Wakeup(World_ID worldID)
{
   UserThread_Wakeup(worldID, UTW_WAIT_COMPLETE);
}

/*
 *----------------------------------------------------------------------
 *
 * UserHeapRequest --
 *
 *	Wrapper for HeapMgr's request function. Only allows growing a heap
 *	in core dump paths, or if MY_USER_CARTEL_INFO is NULL, indicating that
 *	this is the request for the inital heap memory.  
 *
 * Results:
 *	VMK_OK if memory request is satisfied.
 *	VMK_NO_MEMORY (most likely) if memory request is denied.
 *
 * Side effects:
 *	May result in added memory to a dynamic heap.	
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserHeapRequest(uint32 request, void **addr, uint32* length)
{
   if (!MY_USER_CARTEL_INFO || UserDump_DumpInProgress()) {
      return HeapMgr_RequestAnyMem(request, addr, length);
   }

   UWLOG(1, "Rejecting request.");
   return VMK_NO_MEMORY;
}

/*
 *----------------------------------------------------------------------
 *
 * UserNewHeap --
 *
 *	Allocate a new dynamic heap (to be used by a cartel).  
 *
 * Results:
 *	Heap identifier or INVALID_HEAP_ID
 *
 * Side effects:
 *	None.	
 *
 *----------------------------------------------------------------------
 */
Heap_ID
UserNewHeap(World_ID cartelID)
{
   char name[MAX_HEAP_NAME];

   snprintf(name, sizeof(name), "cartel%d", cartelID);

   return Heap_CreateCustom(name, 
	                    USERWORLD_HEAP_INITIAL_SIZE,
		            USERWORLD_HEAP_MAX_SIZE,
			    UserHeapRequest,
			    HeapMgr_FreeAnyMem);
}

/*
 *----------------------------------------------------------------------
 *
 * UserDestroyHeap --
 *
 *	Destroy the given userworld heap (allocated by UserNewHeap)
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	May free the real and virtual memory if bunch of heaps are now unused.
 *
 *----------------------------------------------------------------------
 */
void
UserDestroyHeap(Heap_ID heap)
{
   Heap_Destroy(heap);
}


/*
 *----------------------------------------------------------------------
 *
 * User_UpdatePseudoTSCConv --
 *
 *	Copy updated pseudo-TSC conversion parameters into a
 *	userworld's thread data page.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      As above.
 *
 *----------------------------------------------------------------------
 */
void
User_UpdatePseudoTSCConv(World_Handle *world,
                         const RateConv_Params *conv)
{
   User_ThreadData *tdata;

   tdata = KVMap_MapMPN(world->userThreadInfo->mem.mpn, TLB_LOCALONLY);
   if (tdata == NULL) {
      LOG(0, "Failed to map in tdata page");
      return;
   }
   tdata->pseudoTSCConv = *conv;

   KVMap_FreePages(tdata);
}
