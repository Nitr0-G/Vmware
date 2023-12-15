/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * world.h --
 *
 *	Interfaces for the basic unit of accounting and scheduling in the
 * 	vmkernel: the World.
 */

#ifndef _WORLD_H
#define _WORLD_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "vm_types.h"
#include "action_ext.h"
#include "world_ext.h"
#include "sched_ext.h"
#include "sched.h"
#include "list.h"
#include "world_ext.h"
#include "scsi_ext.h"
#include "vmnix_syscall.h"
#include "alloc.h"
#include "swap.h"
#include "rpc_types.h"
#include "proc.h"
#include "x86.h"
#include "addrlayout32.h"
#include "watchpoint.h"
#include "world_dist.h"
#include "semaphore_ext.h"
#include "vmkperf.h"
#include "timer_dist.h"
#include "rpc.h"
#include "net_public.h"
#include "identity.h"
#include "net.h"
#include "conduit_ext.h"
#include "sharedAreaDesc.h"
#include "heap_public.h"

struct Descriptor;
struct Action_List;
struct NFFilter;
struct SP_SpinLockIRQ;
struct VMKStats_State;
struct User_ThreadInfo;
struct User_CartelInfo;

#define WORLD_NAME_LENGTH	64

#define WORLD_VMM_NUM_STACKS		2
#define WORLD_VMM_STACK_PGOFF       	CPL0_STACK_PAGES_START
#define WORLD_VMM_2ND_STACK_PGOFF    	CPL1_STACK_PAGES_START
#define WORLD_VMM_NUM_STACK_MPNS     	(CPL0_STACK_PAGES_LEN)

#define WORLD_VMK_NUM_STACK_MPNS	VMK_NUM_STACKPAGES_PER_WORLD
#define WORLD_VMK_NUM_STACK_VPNS	(WORLD_VMK_NUM_STACK_MPNS + 1)

#define DEFAULT_NULL_DESC     	0

#define DEFAULT_USER_CODE_DESC  3
#define DEFAULT_USER_DATA_DESC  4

#define DEFAULT_TSS_DESC        5
/*
 * DEFAULT_DF_TSS_DESC index must be available in the hostGDT as well,
 * otherwise, Host_SetGDTEntry will panic.
 */
#define DEFAULT_DF_TSS_DESC	26
#define DEFAULT_NMI_TSS_DESC    MON_VMK_NMI_TASK

#define DEFAULT_CS_DESC         MONITOR_SEGMENT_CS
#define DEFAULT_CS              MAKE_SELECTOR(DEFAULT_CS_DESC,SELECTOR_GDT,0)
#define DEFAULT_DS_DESC         MONITOR_SEGMENT_DS
#define DEFAULT_SS_DESC         MONITOR_SEGMENT_SS
#define DEFAULT_ES_DESC         MONITOR_SEGMENT_ES
#define DEFAULT_DS              MAKE_SELECTOR(DEFAULT_DS_DESC,SELECTOR_GDT,0)
#define DEFAULT_SS              MAKE_SELECTOR(DEFAULT_SS_DESC,SELECTOR_GDT,0)
#define DEFAULT_FS              MAKE_SELECTOR(DEFAULT_DS_DESC,SELECTOR_GDT,0)
#define DEFAULT_GS              MAKE_SELECTOR(DEFAULT_DS_DESC,SELECTOR_GDT,0)
#define DEFAULT_ES              MAKE_SELECTOR(DEFAULT_ES_DESC,SELECTOR_GDT,0)

#define DEFAULT_NUM_ENTRIES     (MON_VMK_LAST_COMMON_SEL + 1)

#define FXSAVE_AREA_SIZE 512

#define MAX_ACTION_NAME_LEN 32

typedef enum WorldGroupPanicState {
   WORLD_GROUP_PANIC_NONE,
   WORLD_GROUP_PANIC_BEGIN,
   WORLD_GROUP_PANIC_VMXPOST
} WorldGroupPanicState;

#define FOR_ALL_VMM_STACK_MPNS(world, i) \
        for (i = 0; i < WORLD_VMM_NUM_STACK_MPNS; i++)

#define FOR_ALL_VMK_STACK_MPNS(world, i) \
        for (i = 0; i < WORLD_VMK_NUM_STACK_MPNS; i++)

typedef struct World_State {
   uint32       regs[8];   // general registers, (i.e. REG_EAX, ..)
   Selector     segRegs[NUM_SEGS];  // segment regsiters (i.e. SEG_CS, )
   uint32       DR[8];     // debug registers
   uint32       CR[5];     // control register 
   uint32       eip;       // PC
   uint32       eflags;    // eflags register
   DTR32        IDTR;       
   DTR32        GDTR;       
   uint32       fpuSaveAreaOffset; // Offset into saveArea of FXSAVE mem.
                                   // This is needed since FXSAVE must be
                                   // 16byte aligned.  Buffer is at
                                   // fpuSaveAreaMem + fpuSaveAreaOffset.
   uint8        fpuSaveAreaMem[FXSAVE_AREA_SIZE+16];  // +16 for alignment
} World_State;

// VM-specific timer data -- gets initialized by Timer_Info()
typedef struct {
   struct SP_SpinLockIRQ	lock;
   uint32                       action;
   uint32                       interval;
   Timer_Handle                 handle;
} World_TimerInfo;

typedef struct World_VmmGroupInfo {
   World_Handle                 *vmmLeader;
   World_ID			members[MAX_VCPUS];
   uint32			memberCount;
   Atomic_uint32                panicState;
   World_ID                     panickyWorld;
   char                         *cfgPath;
   char				*uuidString;
   char				*displayName;
   char                         *panicMsg;
   uint32                       vmxPID;
   Proc_Entry                   procVMXInfo;
   Alloc_Info			allocInfo;
   Swap_VmmInfo                 swapInfo;
   Swap_ChkpointFileInfo        swapCptFile;
   struct MigrateInfo		*migrateInfo;
   struct SharedAreaInfo        *sai;
   Atomic_uint32                *scsiCompletionVector;
   Bool                         toeEnabled;
   struct Net_TOEInstance       *toeInstance;
   char                         action[NUM_ACTIONS][MAX_ACTION_NAME_LEN];
   uint32                       nextAction;
   uint32                       vmkActionIndex;
   MemHandle                    mainMemHandle;
   uint64                       delaySCSICmdsUsec; //min time for scsi commands PR19244
   struct Net_VmmGroupInfo      netInfo;
   Bool                         *nmiFromHeartbeat;  
   World_TimerInfo              timerInfo;
} World_VmmGroupInfo;

/*
 * Structure shared among all members of a world group.
 * Protected by the WorldLock().
 */
typedef struct World_GroupInfo {
   World_ID		        groupID;
   uint32                       memberCount;
   Heap_ID			heap;
   struct MemSched_Client       memsched;
   struct Conduit_WorldInfo     conduitInfo;
   World_VmmGroupInfo           vmm;
   Sched_GroupID		schedGroupID;
} World_GroupInfo;

/*
 * World start function type.  See CpuSched_StartWorld().
 */
typedef void (*WorldStartFunction)(World_Handle *previousWorld);


// arguments needed by _WorldInit handlers
typedef struct World_InitArgs {
   const char           *name;
   uint32               flags;          //system, idle, vmm, etc.
   World_ID             groupLeader;
   MPN                  COSStackMPN;
   WorldStartFunction   func;           //CpuSched_StartWorld, generally
   SharedAreaArgs       *sharedAreaArgs;
   Vcpuid               vcpuid;
   Sched_ClientConfig   *sched;
} World_InitArgs;

/*
 * World attributes.
 */
#define WORLD_SYSTEM            0x001
#define WORLD_IDLE              0x002
#define WORLD_USER              0x004
#define WORLD_VMM               0x008
#define WORLD_HELPER            0x010
#define WORLD_HOST              0x020
#define WORLD_CLONE             0x040   // for UserWorld Threads
#define WORLD_TEST              0x080
#define WORLD_POST              0x100

typedef enum WorldKillLevel {
   WORLD_KILL_UNSET,         // not killed
   WORLD_KILL_NICE,          // wait + hope for it to get safepoint to be killed
   WORLD_KILL_DEMAND,        // wakeup with error and hope for safepoint
   WORLD_KILL_UNCONDITIONAL  // take it out regardless of consequences
} WorldKillLevel;

// size of debug-only ring buffer to track reader count holders
#define WORLD_READER_COUNT_HOLDERS 8

typedef struct WorldVMMStackInfo {
   VA stackBase;
   VA stackTop;
   MPN mpns[WORLD_VMM_NUM_STACK_MPNS];
   void *mappedStack;
} WorldVMMStackInfo;

struct SharedAreaInfo;

struct ModuleTable;

typedef struct World_VmmInfo {
   Vcpuid                       vcpuid;
   Proc_Entry                   procVMMStats;
   WorldVMMStackInfo            vmmStackInfo[WORLD_VMM_NUM_STACKS];
   World_ID                     vmxThreadID;
   Reg32                        vmmCoreDumpEBP;
   Reg32                        vmmCoreDumpESP;
   Reg32                        vmmCoreDumpEIP;
   RPC_UserRPCStats             *userRPCStats;
   uint32                       semaActionMask;
   Bool				inVMMPanic;
} World_VmmInfo;

struct World_Handle {
   /*
    * Sched_Client must come first because of embedded list links.
    */
   Sched_Client	                sched;
   World_State                  savedState;

   uint32                       generation;
   World_ID                     worldID;
   char				worldName[WORLD_NAME_LENGTH];

   // Ref count caused by vmware processes via World_Bind().  A host count
   // of 0 doesn't mean the world is dead, just that there are no userlevel
   // processes (vmx, mks) bound [as will always be the case for non-vmm
   // worlds]. 
   int32			hostCount;

   // Ref count to make sure that World_Find() and World_Release() match up
   // (but doesn't prevent world from being destroyed).
   int32			refCount;

   // Optional World_Find() ref count that prevents world from being
   // destroyed while temporarily in use.  Also incremented by group 
   // members to make sure group leader stays around until all other
   // members are destroyed.
   int32                        readerCount;

   // Debugging for readerCount reference leaks.  See WorldFindOptReaderLock
   DEBUG_ONLY(void              *countHolders[WORLD_READER_COUNT_HOLDERS];)
   DEBUG_ONLY(unsigned          countHolderIndex;)

   uint32                       modulesInited;
   const struct ModuleTable     *moduleTable;
   uint32                       moduleTableLen;

   uint32                       typeFlags;

   Bool				inUse;

   Bool				deathPending;
   WorldKillLevel               killLevel;

   Bool                         reapScheduled;  // to prevent multiple reaps
   uint32                       reapCalls;      // number of reap attempts 
                                                // that have been made.
   
   Bool                         reapStarted;    //TRUE if actual cleanup of
                                                //this world's data has 
   //                                           //started.

   VMK_ReturnStatus             exitStatus;

   Bool				okToReadRegs;
   Bool				preemptionDisabled;

   Watchpoint_State		watchpointState;

   World_GroupInfo              *group;

   MA				pageRootMA;
   MPN				pageTableMPNs[MON_PAGE_TABLE_LEN];
   MPN				vmkStackMPNs[WORLD_VMK_NUM_STACK_MPNS];
   VA				vmkStackStart;
   uint32			vmkStackLength;
   MPN				taskMPN;

   MPN				nmiStackMPN;
   VA				nmiStackStart;

   // Per-world kernel GDT.  It gets mapped in WorldASInit.
   MPN				gdtMPN[GDT_AREA_LEN];
   struct Descriptor		*kernelGDT;

   List_Links                   cnxList;

#ifdef VMKPERF_ENABLE_COUNTERS
   Vmkperf_WorldInfo             vmkperfInfo;
#endif

   Timer_Handle                 pseudoTSCTimer;  // VMM worlds and userworlds

   struct User_CartelInfo*	userCartelInfo;
   struct User_ThreadInfo*	userThreadInfo;
   void				*userLongJumpPC;
   VMK_ReturnStatus		userCopyStatus;

   Identity                     ident;

   struct WorldScsiState        *scsiState;

   Proc_Entry                   procWorldNetDir;
   Proc_Entry                   procWorldDir;
   Proc_Entry                   procWorldDebug;

   Semaphore			selectSema;

   int                          minStackLeft;

   List_Links                   heldSemaphores;

   Bool                         netInitialized; // used by Net_WorldPreCleanup()

   Atomic_uint32                bhPending;

   // nmisInMonitor indicates if we should leave nmis running when entering
   // the world.
   Bool				nmisInMonitor;
   World_VmmInfo                *vmm;            // per vmm-world info
   struct VMK_SharedData        *vmkSharedData;  // should go away entirely...

};

/*
 * inline operations
 */

static INLINE CpuSched_Vsmp *
World_CpuSchedVsmp(const World_Handle *world)
{
   return(world->sched.cpu.vcpu.vsmp);
}

static INLINE CpuSched_Vcpu *
World_CpuSchedVcpu(World_Handle *world)
{
   return(&world->sched.cpu.vcpu);
}

static INLINE World_Handle *
World_VcpuToWorld(const CpuSched_Vcpu *vcpu)
{
   return((World_Handle *) (((char *) vcpu) - offsetof(World_Handle,sched.cpu.vcpu)));
}

static INLINE CpuSched_RunState
World_CpuSchedRunState(const World_Handle *world)
{
   return(world->sched.cpu.vcpu.runState);
}

static INLINE void
World_CpuSchedRunStateInit(World_Handle *world)
{
   world->sched.cpu.vcpu.runState = CPUSCHED_NEW;
}

static INLINE VA
World_GetVMKStackBase(const World_Handle *world)
{
   return world->vmkStackStart;
}

static INLINE VA
World_GetVMKStackTop(const World_Handle *world)
{
   return world->vmkStackStart + world->vmkStackLength;
}

static INLINE size_t
World_GetVMKStackLength(const World_Handle *world)
{
   return world->vmkStackLength;
}

static INLINE void *
World_Alloc(const World_Handle *world, uint32 size)
{
   return Heap_Alloc(world->group->heap, size);
}

static INLINE void
World_Free(const World_Handle *world, void *mem)
{
   Heap_Free(world->group->heap, mem);
}

static INLINE void *
World_Align(const World_Handle *world, uint32 size, uint32 align)
{
   return Heap_Align(world->group->heap, size, align);
} 

/*
 * operations
 */

void World_Init(VMnix_Init *vmnixInit);
void World_ConfigArgs(World_InitArgs *args,
                      const char *name,
                      uint32 flags,
                      World_ID worldGroupNumber,
                      Sched_ClientConfig *sched);
void World_ConfigVMMArgs(World_InitArgs *args,
                         VMnix_CreateWorldArgs *vmnixArgs);
void World_ConfigUSERArgs(World_InitArgs *args,
                          VMnix_CreateWorldArgs *vmnixArgs);
VMK_ReturnStatus World_New(World_InitArgs *args, World_Handle **handle);
VMK_ReturnStatus World_NewDefaultWorld(const char *name, World_Handle **handle);
VMK_ReturnStatus World_NewIdleWorld(uint32 pcpuNum, World_Handle **world);
void World_DestroySlavePCPU(uint32 pcpuNum);
int World_Bind(World_ID worldID);

World_Handle *World_Find(World_ID worldID);
void World_Release(World_Handle *world);
void World_ReleaseAndWaitForDeath(World_Handle *world);
World_Handle *World_FindNoRefCount(World_ID worldID);
void World_ReleaseNoRefCount(World_Handle *world);

World_Handle *World_GetIdleWorld(int pcpuNum);
void World_SetIdleWorld(int pcpuNum, World_Handle *world);
VMK_ReturnStatus World_AddPage(World_ID worldID, VPN vpn, MPN mpn, Bool readOnly);

World_Handle *World_Switch(World_Handle *restore, World_Handle *save);
VMK_ReturnStatus World_ReadRegs(World_ID worldID, VMnix_ReadRegsResult *regs);
VMK_ReturnStatus World_VPN2MPN(World_Handle* world, VPN vpn, MPN *outMPN);
VMK_ReturnStatus World_GetVMKStackPage(World_Handle *world, int pageNum, VA *va);

VMK_ReturnStatus World_Cleanup(Bool force);
void World_LateCleanup(void);
Bool World_Exists(World_ID worldID);
VMK_ReturnStatus World_MakeRunnable(World_ID worldID, 
                                    void (*start)(void *startArg));
                       

VMKERNEL_ENTRY World_Connect(DECLARE_2_ARGS(VMK_RPC_CONNECT, char *, name, 
                                            RPC_Connection *, cnxID));
VMK_ReturnStatus World_Destroy(World_ID worldID, Bool clearHostCount);
VMK_ReturnStatus World_DestroyVmms(World_Handle *world, Bool waitForDeath,
                                   Bool clearHostCount);
void World_Kill(World_Handle* world);
void World_GroupKill(World_Handle *world);

uint32 World_AllWorlds(World_ID *ids, uint32 *n);
int World_ActiveGroupCount(void);

void World_DumpPTE(VA vaddr);
void World_ResetDefaultDT(void);
void World_WatchpointsChanged(void);
void World_ChangeCodeProtection(Bool writable);
void World_SelectWakeup(World_ID worldID);
void World_SelectBlock(void);
World_ID World_VcpuidToWorldID(struct World_Handle *world, Vcpuid vcpuid);
void World_SetDefaultGDTEntry(int index, LA base, VA limit, uint32 type, 
                              uint32 S, uint32 DPL, uint32 present, uint32 DB, 
                              uint32 gran);
MPN World_GetStackMPN(VA va);
struct World_Handle *World_GetWorldFromStack(VA va);
VMK_ReturnStatus World_SetVMXInfo(void *hostArgs);
VMK_ReturnStatus World_SetVMXInfoWork(World_ID worldID, uint32 vmxPID,
                                      const char *cfgPath,
                                      const char *uuidString,
                                      const char *displayName);
void World_AfterPanic(World_Handle *world);
void World_Panic(struct World_Handle *world, const char *fmt, ...);
VMKERNEL_ENTRY World_VMMPanic(DECLARE_ARGS(VMK_PANIC));

uint32 World_GetVmmMembers(World_Handle *world, World_Handle **outHandles);
void World_ReleaseVmmMembers(World_Handle *world);

int World_FormatTypeFlags(uint32 flags, char *buf, int maxLen);

VMKERNEL_ENTRY World_VmmASInit(DECLARE_1_ARG(VMK_VMM_AS_INIT, MA *, vmmCR3));
VMKERNEL_ENTRY World_InitMainMem(DECLARE_1_ARG(VMK_VMM_MAINMEM_INIT, MemHandle, 
                                               mainMemHandle));

/*
 * The debugger needs to be able to call World_* functions, however, it cannot
 * risk attempting to acquire WorldLocks, as there is a chance of deadlock if
 * code in a World_* function faulted.  Thus, special, non-lock-acquiring
 * functions have been created for the debugger's sole use -- no one else should
 * use these functions.
 */
World_Handle *World_FindDebug(World_ID worldID);
uint32 World_AllWorldsDebug(World_ID *ids, uint32 *n);

void World_LogBacktrace(World_ID wid);


#ifdef VMKPERF_ENABLE_COUNTERS
void World_ResetVmkperfCounters(uint32 counterNum);
#endif // end VMKPERF_ENABLE_COUNTERS

void World_CheckStack(World_Handle* world, int minStackLeft);
/*
 * Wrapper function
 */
static INLINE World_ID
World_GetGroupLeaderID(const World_Handle *world)
{
   ASSERT(world->group);
   return world->group->groupID;
}

static INLINE Bool
World_IsGroupLeader(const World_Handle *world) 
{
   return world->group && (world->worldID == World_GetGroupLeaderID(world));
}

//Generate World_IsIDLEWorld() & other helpful macros:

#define GEN_FLAGS_FN(name)                              \
   static INLINE Bool                                   \
   World_Is##name##World(const World_Handle *world)     \
   {                                                    \
      return((world->typeFlags & WORLD_##name) != 0);   \
   }

GEN_FLAGS_FN(HOST)
GEN_FLAGS_FN(VMM)
GEN_FLAGS_FN(HELPER)
GEN_FLAGS_FN(SYSTEM)
GEN_FLAGS_FN(IDLE)
GEN_FLAGS_FN(USER)
GEN_FLAGS_FN(TEST)
GEN_FLAGS_FN(POST)
GEN_FLAGS_FN(CLONE)

#undef GEN_FLAGS_FN

static INLINE World_VmmGroupInfo *
World_VMMGroup(const World_Handle *world)
{
   return &world->group->vmm;
}

static INLINE World_VmmInfo *
World_VMM(const World_Handle *world)
{
   ASSERT(World_IsVMMWorld(world));
   return world->vmm;
}

/*
 * Returns the VMM leader world. 
 * This function can only be called on a VMM world.
 */
static INLINE World_Handle *
World_GetVmmLeader(const World_Handle *world)
{
   ASSERT(world->group);
   ASSERT(World_IsVMMWorld(world) || World_IsTESTWorld(world));
   ASSERT(World_VMMGroup(world)->vmmLeader != NULL);
   return World_VMMGroup(world)->vmmLeader;
}

static INLINE World_GroupID
World_GetVmmLeaderID(const World_Handle *world)
{
   World_Handle *vmmLeader = World_VMMGroup(world)->vmmLeader;
   return (vmmLeader != NULL) ? vmmLeader->worldID : INVALID_WORLD_ID;
}

static INLINE Bool
World_IsVmmLeader(const World_Handle *world) 
{
   return world->group && (world->worldID == World_GetVmmLeaderID(world));
}

Bool World_IsSafeToBlockWithLock(const SP_SpinLock *lock,
                                        const SP_SpinLockIRQ *lockIRQ);
Bool World_IsSafeToDescheduleWithLock(const SP_SpinLock *lock,
                                             const SP_SpinLockIRQ *lockIRQ);

static INLINE Bool
World_IsSafeToDeschedule(void)
{
   return World_IsSafeToDescheduleWithLock(NULL, NULL);
}

static INLINE Bool
World_IsSafeToBlock(void)
{
   return World_IsSafeToBlockWithLock(NULL, NULL);
}

#endif
