/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * user_int.h --
 *
 *	User World support
 */

#ifndef VMKERNEL_USER_USERINT_H
#define VMKERNEL_USER_USERINT_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "vm_basic_types.h"
#include "vm_libc.h"
#include "vmkernel.h"
#include "world.h"
#include "idt.h"
#include "memalloc.h"
#include "user.h"               /* Include the module interface, and external interfaces. */
#include "userMem.h"
#include "userObj.h"
#include "userSig.h"
#include "userThread.h"
#include "userDebug.h"
#include "userDump.h"
#include "userIdent.h"
#include "userVMKRpc.h"
#include "userStatDef.h"
#include "userTime.h"
#include "userSocket.h"

#define USERWORLD_HEAP_INITIAL_SIZE (200*1024)
#define USERWORLD_HEAP_MAX_SIZE (300*1024)

#define USERWORLD_HEAP_MAXALLOC_SIZE ((8*1024) + 128)
/* In UserFileRead/Write, we allocate up to 8K for a cache buffer. */

/*
 * Lock ranks for all the user-world locks.
 *
 * UW_SP_RANK_SIGTHREAD needs to be lower rank than
 * UW_SP_RANK_SIGCARTEL.  The cartel signal state lock is sometimes
 * acquired while holding a specific thread's thread signal state
 * lock.
 *
 * Some locks are held while calls to wait/wakeup are made and thus
 * those locks must be of a rank lower than UW_SP_RANK_WAIT.
 *
 * The stats lock should be grabbable with any other lock held, so
 * make sure the other locks are lower rank than it.
 */
#define UW_SP_RANK_STATS         (SP_RANK_LEAF)
#define UW_SP_RANK_WAIT          (UW_SP_RANK_STATS - 1)
#define UW_SP_RANK_DUMP          (UW_SP_RANK_STATS - 1)
#define UW_SP_RANK_HEAP          (UW_SP_RANK_STATS - 1)
#define UW_SP_RANK_PROXYSEND     (UW_SP_RANK_STATS - 1)
#define UW_SP_RANK_USERPROCDEBUG (UW_SP_RANK_STATS - 1)       
#define UW_SP_RANK_SLEEP         (UW_SP_RANK_WAIT - 1)
#define UW_SP_RANK_USEROBJ       (UW_SP_RANK_WAIT - 1)
#define UW_SP_RANK_THREADPEER    (UW_SP_RANK_WAIT - 1)
#define UW_SP_RANK_USERMEM       (UW_SP_RANK_WAIT - 1)
#define UW_SP_RANK_POLLWAITERS   (UW_SP_RANK_WAIT - 1)
#define UW_SP_RANK_SIGCARTEL     (UW_SP_RANK_WAIT - 1)
#define UW_SP_RANK_UNIX_SOCKET   (UW_SP_RANK_WAIT - 1)
#define UW_SP_RANK_UNIX_SERVER_SOCKET (UW_SP_RANK_UNIX_SOCKET - 1)
#define UW_SP_RANK_UNIX_NAMESPACE (UW_SP_RANK_UNIX_SERVER_SOCKET - 1)
#define UW_SP_RANK_SIGTHREAD     (UW_SP_RANK_SIGCARTEL - 1)
#define UW_SP_RANK_TIME          (UW_SP_RANK_STATS - 1)
#define UW_SP_RANK_TIMETHREAD    (UW_SP_RANK_TIME - 1)


/*
 * Semaphores are used for calling out to the proxy or locking objects
 * while calling copyin/copyout (e.g., pipes).
 */
#define UW_SEMA_RANK_USERPIPE  (SEMA_RANK_LEAF)
#define UW_SEMA_RANK_PROXY     (SEMA_RANK_LEAF)
#define UW_SEMA_RANK_OBJ       (MIN(SEMA_RANK_FS,UW_SEMA_RANK_PROXY) - 1)
#define UW_SEMA_RANK_IDENT     (UW_SEMA_RANK_PROXY - 1)

/*
 * Exit codes generated for indirect exit calls (i.e., not directly
 * due to exit by user-mode code) start at 128 and go up from there.
 */
#define CARTEL_EXIT_SYSERR_BASE	128

struct UserDump_Header;

typedef struct User_EnvInfo {
   char **environ;
   int numVars;
   int maxVars;
} User_EnvInfo;

typedef struct User_LoaderInfo {
   uint32 phdr;
   uint32 phent;
   uint32 phnum;
   uint32 base;
   uint32 entry;
} User_LoaderInfo;

typedef struct User_FileInfo {
   struct User_FileInfo* next;
   uint32 id;
   struct UserObj* obj;
   char name[USER_MAX_FNAME_LENGTH];
} User_FileInfo;

typedef struct User_MapInfo {
   struct User_MapInfo* next;
   uint32 addr;
   uint32 length;
   uint32 prot;
   uint32 flags;
   uint32 id;
   uint32 offset;
   uint32 zeroAddr;
} User_MapInfo;

typedef struct User_Arg {
   struct User_Arg *next;
   char* arg;
   int length;
} User_Arg;

typedef struct User_InitArgs {
   User_LoaderInfo ldInfo;
   char         *workingDirName;
   size_t	num;
   User_Arg	*head;
   User_Arg	*tail;
   User_MapInfo *mapHead;
   User_MapInfo *mapTail;
   User_FileInfo *fileHead;
   User_EnvInfo *envInfo;
} User_InitArgs;

typedef struct User_ShutdownArgs {
   int		exitCode;
   int		exceptionType;
   Bool		hasException;
   VMKFullUserExcFrame exceptionFrame;
} User_ShutdownArgs;

typedef struct User_CoreDumpState {
   SP_SpinLock  dumpLock;
   World_ID     dumperWorld;
   Bool         enabled;
   Bool         inProgress;
   char         dumpName[LINUX_PATH_MAX];
   struct UserDump_Header *header;
} User_CoreDumpState;

/*
 * Per-thread logging context.  Used to track the currently active
 * system call, and thus allow per-syscall logging controls.
 */
typedef struct User_LogContext
{
#ifdef VMX86_LOG
   Bool   linuxCall;    // or UWVMK call 
   int    syscallNum;
   char   oprefix[48];
   char   prefix[48];
#endif
} User_LogContext;


/*
 * The User_CartelInfo state is shared among all the threads in a
 * cartel. It is effectively the "process-level" state.
 */
typedef struct User_CartelInfo {
   Atomic_uint32	refCount;

   World_ID		cartelID;

   /*
    * Wait lock is cartel-wide to guarantee wakeups of both "groups"
    * (via a CpuSched_Wakeup event id) and of specific worlds (via
    * CpuSched_ForceWakeup) are synchronized with waits.
    */
   SP_SpinLock		waitLock;

   UserObj_State	fdState;
   UserSig_CartelInfo	signals;
   UserThread_Peers	peers;
   UserMem		mem;
   UserDebug_State	debugger;
   UserTime_CartelInfo  time;

   User_InitArgs 	args;
   User_ShutdownArgs	shutdown;
   User_CoreDumpState   coreDump;

   Heap_ID              heap;
   UserProxy_CartelInfo proxy;

   UserStat_Record      cartelStats;
   UserSocketInetCnx    socketInetCnx;
} User_CartelInfo;

/*
 * Per-thread user world state.
 */
typedef struct User_ThreadInfo {
   UserSig_ThreadInfo	signals;

   Timer_Handle         selectTimer;
   Bool                 selectTimeout;

   volatile Bool	dead;

   UserThread_WaitInfo  waitInfo;

   /*
    * A pointer to the current "exception frame" pushed onto the
    * kernel stack by the trap that got the current thread into the
    * kernel.
    *
    * This is useful if a system call wants to modify the user-mode
    * register state, or if some code wants to print/walk a user-mode
    * stack trace.
    *
    * WARNING: This pointer is only valid when handling a system call
    * or other trap, and is only valid for the course of that system
    * call.  Do not store a copy of this pointer!
    */
   VMKFullUserExcFrame* exceptionFrame;

   User_LogContext	logContext;
   UserStat_Record      threadStats;
   UserTime_ThreadInfo  time;
   UserMem_ThreadInfo   mem;
} User_ThreadInfo;


/*
 *----------------------------------------------------------------------
 *
 * VMKEXCFRAME_TO_FULLUSERFRAME --
 *
 * 	Cast a VMKExcFrame* into a VMKFullUserExcFrame*.  We can do this
 * 	because the kernel's trap code pushes additional state to make all
 * 	the on-stack trap frame information more completely (and uniformly
 * 	sized) see IDTGenerateHandler for details.
 *
 * Results:
 *	Pointer to the enclosing VMKFullUserExcFrame
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static inline VMKFullUserExcFrame*
VMKEXCFRAME_TO_FULLUSERFRAME(VMKExcFrame* eFrame) {
   return (void*)eFrame - sizeof(VMKExcRegs);
}

/*
 *----------------------------------------------------------------------
 *
 * VMKFULLUSERFRAME_TO_EXCFRAME --
 *
 * 	Cast a VMKFullUserExcFrame* into a VMKExcFrame*.  We can do
 * 	this because the FullUser state is a superset of the ExcFrame.
 * 	See VMKEXCFRAME_TO_FULLUSERFRAME.
 *
 * Results:
 *	Pointer to the contained VMKExcFrame;
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static inline VMKExcFrame*
VMKFULLUSERFRAME_TO_EXCFRAME(VMKFullUserExcFrame* fullFrame) {
   return (void*)fullFrame + sizeof(VMKExcRegs);
}

/*
 *----------------------------------------------------------------------
 * 
 * User_FindVmmLeader --
 *
 *      Lock the vmm leader of the current world group.
 * 
 * Results:
 *      Return vmm leader world if it exists. Otherwise, return NULL. 
 *
 * Side effects:
 *      None.
 *----------------------------------------------------------------------
 */
static INLINE World_Handle *
User_FindVmmLeader(const World_Handle *world)
{
   World_Handle *vmmLeader = World_VMMGroup(world)->vmmLeader;
   if (vmmLeader != NULL) {
      return World_Find(vmmLeader->worldID);
   } else {
      return NULL;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * User_HeapAlloc --
 *
 *	Allocate some memory from cartel's heap.  INLINE'd so the heap
 *	records the correct return address for debugging.
 *
 * Results:
 *	pointer to the allocated memory
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void *
User_HeapAlloc(User_CartelInfo* uci, uint32 size)
{
   ASSERT(size < USERWORLD_HEAP_MAXALLOC_SIZE);
   return Heap_Alloc(uci->heap, size);
}

/*
 *----------------------------------------------------------------------
 *
 * User_HeapAlign --
 *
 *	Allocate some aligned memory from cartel's heap.
 *
 * Results:
 *	pointer to the allocated memory
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void *
User_HeapAlign(User_CartelInfo* uci, uint32 size, uint32 alignment)
{
   ASSERT(size < USERWORLD_HEAP_MAXALLOC_SIZE);
   return Heap_Align(uci->heap, size, alignment);
}

/*
 *----------------------------------------------------------------------
 *
 * User_HeapFree --
 *
 *	Free the given memory (previously allocated by User_HeapAlloc) back
 *	to cartel's heap
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
User_HeapFree(User_CartelInfo* uci, void *mem)
{
   Heap_Free(uci->heap, mem);
}

extern uint8 zeroPage[PAGE_SIZE];

extern void User_LogStackTrace(VMKFullUserExcFrame *excFrame);

extern void User_SelWakeup(World_ID worldID);

extern void User_CartelShutdown(int exitCode,
                                Bool coreDump,
                                VMKFullUserExcFrame* fullFrame);

extern int User_TranslateStatus(VMK_ReturnStatus status);

extern VMK_ReturnStatus User_CleanFrameCopy(VMKFullUserExcFrame* const outFrame,
                                            const VMKFullUserExcFrame* inFrame);

extern VMK_ReturnStatus User_CopyInString(char* dest, UserVA src, int maxLen);

#include "userLinux.h"

#define MY_USER_CARTEL_INFO (MY_RUNNING_WORLD->userCartelInfo)
#define MY_USER_THREAD_INFO (MY_RUNNING_WORLD->userThreadInfo)

#endif

