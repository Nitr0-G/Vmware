/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * world.c --
 *
 *	Manages creation, deletion and switching of worlds.
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "x86.h"
#include "vmkernel.h"
#include "vmnix_if.h"
#include "kvmap.h"
#include "memmap.h"
#include "world.h"
#include "idt.h"
#include "vm_asm.h"
#include "host.h"
#include "util.h"
#include "alloc.h"
#include "vmk_scsi.h"
#include "net.h"
#include "prda.h"
#include "rpc.h"
#include "tlb.h"
#include "kseg.h"
#include "action.h"
#include "addrlayout32.h"
#include "proc.h"
#include "timer.h"
#include "post.h"
#include "user.h"
#include "parse.h"
#include "pagetable.h"
#include "memsched_ext.h"
#include "helper.h"
#include "migrateBridge.h"
#include "vmmstats.h"
#include "libc.h"
#include "vmkevent.h"
#include "xmap.h"
#include "sharedArea.h"
#include "conduit_bridge.h"
#include "apic.h"
#include "nmi.h"
#include "identity.h"
#include "user_layout.h"
#include "reliability.h"
#include "trace.h"
#include "vsiDefs.h"
#include "world_vsi.h"

#define LOGLEVEL_MODULE World
#include "log.h"


/*
 * Notes:  VM creation and destruction explained. (UW VMX only)
 *
 * VM creation:
 *
 *    a. The main VMX world is created. It will create a world group.
 *       and create a UserCartel. The world will become group leader.
 *       It's world ID is used as the group ID of the VM.
 *    b. More VMX worlds will be cloned from the VMX world
 *       and they will share the same world group and UserCartel.
 *    c. When monitor powers on in VMX, the VMM worlds are created
 *       and they join the same world group of the same VMX worlds.
 *
 *       The first VMM world created will become the vmm leader.
 *       At that time, the VMM specific world group data will be 
 *       initialized. The VMM leader will not be cleaned up until
 *       all other VMM worlds exit. This is implemented by increasing
 *       VMM leader's readerCount for every non-leading VMM world when
 *       during group initialization.
 *
 *       When a VMM world is created, its hostCount is incremented
 *       so that the VMM world will not exit before the VMX world powers
 *       off the VMM world or the VMX world stops running.
 *  
 * VM destruction:
 *
 *    There are the ways to quit a VM (or a VM group).
 *
 *    Case 1. Normal power off
 *       a. The main VMX world cleans up all memory mapped from the VMM worlds.
 *       b. The main VMX world decreases host count on the VMM world.
 *       c. VMM worlds start to exit.
 *       d. VMX worlds start to exit.
 *       e. When the last VMX world is exiting, it cleans up UserCartel.
 *       f. Before UserCartel gets cleaned up , it make sure all VMM worlds
 *          no longer runs.
 *       g. When the last VMM world (VMM leader) exits, it cleans up vmm specific
 *          data in the world group.
 *       h. When the last world in a group exists, the world group is freed. 
 *
 *    Case 2: VMX panic
 *       a. A VMX world panics, it tries to stop all peer VMX worlds and
 *          starts to dump core.
 *       b. The VMX world try to kill all sibling VMX worlds in the VM 
 *          world group.
 *       c. From then on, it's the same as case 1.e.
 *  
 *    Case 3: VMM panic
 *       a. The VMM world sends a message to the main VMX world.
 *       b. The VMM worlds start to exit when host count becomes 0.
 *       c. The VMX world dumps cores for all VMM worlds.
 *       d. From then on, it's the same as case 1.a.
 *   
 *    Case 4: echo kill >> /proc/vmware/vm/xxx/debug
 *       a. We find the group leader of world "xxx".
 *       b. From the group leader, we get the UserCartel.
 *       b. We start with killing all VMX worlds in the UserCartel, which
 *          will then kill the VMM worlds.
 *       c. Same as case 1.f
 *
 *    case 5: kill proxy
 *       a. We will forward the signal to the VMX world.
 *       b. Same as case 2.a.
 *
 *    (The following cases do not guarantee a graceful VM shutdown.)
 *
 *    case 6: echo kill -9 >> /proc/vmware/vm/xxx/debug
 *       a. We first clear the host count and ref count on world "xxx".
 *       b. Same as case 4.a
 *       (Notice in this case, if we "kill -9" on the vmm leader before
 *        other vmm worlds quit, PSOD is expected.
 *        Also, if a VMM world exits before the VMX world, the memory
 *        mapped from the VMM world may be corrupted by the VMX world.)
 *
 *    case 7: kill -9 proxy
 *       a. The proxy in COS get killed.
 *       b. At certain point, VMX world get an error because a call to
 *          the proxy failed. (Notice, VMX may survive for a long time.)
 *       c. Same as 2.a.  
 *      
 */


/*
 * Valid World_ID are positive.  In fact we limit it to 30 bits because
 * userworlds add 100,000 to it and still expect positive values.
 * Also, 0 is not allowed.
 */
#define MAX_WORLD_ID    ((1 << 30) - 1)

#define TASK_BASE	(VMM_FIRST_LINEAR_ADDR + VPN_2_VA(TASK_PAGE_START))
#define DEFAULT_TASK_SIZE	(sizeof(Task) + \
				 INTERRUPT_REDIRECTION_BITMAP_SIZE + \
				 IO_PERMISSION_BITMAP_SIZE)
#define NMI_TASK_BASE		(TASK_BASE + DEFAULT_TASK_SIZE)
#define NMI_TASK_SIZE		(sizeof(Task))

/*
 * WORLD_KILL_TIMEOUT_SECS is the number of seconds to wait for a world to
 * die before switching from WORLD_KILL_NICE to WORLD_KILL_DEMAND mode.
 */
#define WORLD_KILL_TIMEOUT_SECS 10

extern void CommonNmiHandler(void);

#define WORLDGROUP_HEAP_INITIAL_SIZE	(1024 * 1024)
#define WORLDGROUP_HEAP_MAX_SIZE 	(2048 * 1024)

/*
 * Wait at most 1 second for low memory in WorldVMKStackInit
 */
#define ALLOC_LOW_MEM_MAX_WAIT 1000 // Time in miliseconds

typedef struct ModuleTable {
   const char * const   name;
   VMK_ReturnStatus     (*init)(World_Handle *, World_InitArgs *);
   void                 (*exit)(World_Handle *);
} ModuleTable;

typedef struct PreCleanupTable {
   const char * const   name;
   void                 (*fn)(World_Handle *);
} PreCleanupTable;


static Descriptor defaultGDT[DEFAULT_NUM_ENTRIES];

/*
 * List of world entries.  A world entry is active if its "inUse"
 * flag is TRUE or its refcount is non-zero.  The first numPCPU
 * worlds are the idle worlds.
 */
#define WORLD_TABLE_LENGTH MAX_WORLDS
static World_Handle worlds[WORLD_TABLE_LENGTH];
static World_Handle *idleWorlds[MAX_PCPUS];

#if WORLD_TABLE_LENGTH > (VMK_NUM_STACK_PDES * VMK_PTES_PER_PDE) / WORLD_VMK_NUM_STACK_VPNS
#error "Not enough stacks to fully populate world table"
#endif

/*
 * This lock protects the 'worlds' table (allocation and deallocation
 * and finding of worlds).
 */
static SP_SpinLockIRQ worldLock;
uint32 cpuidFeatures;  // CPUID features word
static Proc_Entry procWorlds;

/*
 * Lock for world-death waiters to use.  See WorldCleanup and
 * World_ReleaseAndWaitForDeath.
 */
static SP_SpinLock worldDeathLock;

static VMK_PTE *worldStackPTables[VMK_NUM_STACK_PDES];
static Atomic_uint32 worldActiveGroupCount;

static uint32 WorldAllWorlds(World_ID *ids, uint32 *n);
static void WorldClearTaskBusy(Bool fromHost);
static Bool WorldPOST(void *clientData, int id, 
                      SP_SpinLock *lock, 
                      SP_Barrier *barrier);
static World_Handle *WorldNewCOSWorld(MPN stackMPN);
static VMK_ReturnStatus WorldInit(World_Handle *world, 
                                  const ModuleTable* mTable, 
                                  int tableLen, 
                                  World_InitArgs *args);
static VMK_ReturnStatus WorldAddPage2(World_Handle *world, VPN vpn, MPN mpn,
                                      Bool readOnly, MPN *outMPN);
static void WorldCleanup(World_Handle *world);
static void WorldReap(void *data);
void WorldScheduleReap(World_ID worldID, Bool firstTime);
static void WorldTableInitEntry(World_Handle *world, World_InitArgs *args);
#ifdef VMX86_STATS
static int WorldSwitchStatsReadHandler(Proc_Entry *entry, char *page, int *lenp);
static int WorldSwitchStatsWriteHandler(Proc_Entry *entry, char *page, int *lenp);
#endif
static int WorldProcDebugWrite(Proc_Entry *entry, char *buffer, int *len);


// init functions
static VMK_ReturnStatus WorldASInit(World_Handle *world, World_InitArgs *args);
static VMK_ReturnStatus WorldVMMStackInit(World_Handle *world, World_InitArgs *args);
static VMK_ReturnStatus WorldCOSStackInit(World_Handle *world, World_InitArgs *args);
static VMK_ReturnStatus WorldGroupInit(World_Handle *world, World_InitArgs *args);
static VMK_ReturnStatus WorldProcInit(World_Handle *world, World_InitArgs *args);
static VMK_ReturnStatus WorldSharedDataInit(World_Handle *world, World_InitArgs *args);
static VMK_ReturnStatus WorldMiscInit(World_Handle *world, World_InitArgs *args);
static VMK_ReturnStatus WorldSavedStateInit(World_Handle *world, World_InitArgs *args);
static VMK_ReturnStatus WorldVMKStackInit(World_Handle *world, World_InitArgs *args);
static VMK_ReturnStatus WorldVMMLicenseInit(World_Handle *world, World_InitArgs *args);
static VMK_ReturnStatus WorldVMMInit(World_Handle *world, World_InitArgs *args);


// cleanup functions
static void WorldMiscCleanup(World_Handle *world);
static void WorldProcCleanup(World_Handle *world);
static void WorldGroupCleanup(World_Handle *world);
static void WorldSharedDataCleanup(World_Handle *world);
static void WorldASCleanup(World_Handle *world);
static void WorldVMMStackCleanup(World_Handle *world);
static void WorldVMKStackCleanup(World_Handle *world);
static void WorldVMMLicenseCleanup(World_Handle *world);
static void WorldVMMCleanup(World_Handle *world);

/*
 * Initialization and cleanup tables for various types of worlds.
 * We walk the table from 0 to N-1 when initing, and in the reverse
 * order when exiting.
 *
 */

// COMMON_TABLE is used by all worlds

#define COMMON_TABLE1 \
   { "group",           WorldGroupInit,                 WorldGroupCleanup },   

#define COMMON_TABLE2 \
   { "misc",            WorldMiscInit,                  WorldMiscCleanup },     \
   { "proc",            WorldProcInit,                  WorldProcCleanup },     \
   { "vmkperf",         NULL,                           Vmkperf_CleanupWorld }, \
   { "sched",           Sched_WorldInit,                Sched_WorldCleanup },   \
   { "net",             Net_WorldInit,                  Net_WorldCleanup },     \
   { "scsi",            SCSI_WorldInit,                 SCSI_WorldCleanup },    \
   { "identity",        Identity_WorldInit,             Identity_WorldCleanup },\
   { "rpc",             RPC_WorldInit,                  RPC_WorldCleanup },

// NON_HOST_TABLE is used by all worlds except the host world
#define NON_HOST_TABLE \
   { "stack",           WorldVMKStackInit,              WorldVMKStackCleanup }, \
   { "addressSpace",    WorldASInit,                    WorldASCleanup },       \
   { "savedState",      WorldSavedStateInit,            NULL },

// host/COS world
static const ModuleTable consoleTableInit[] = {
   COMMON_TABLE1
   COMMON_TABLE2
   { "cosstack",        WorldCOSStackInit,              WorldVMKStackCleanup },
   { "conduit",         Conduit_WorldInit,              Conduit_WorldCleanup}
};

// VMM worlds
static const ModuleTable vmmTableInit[] = {
   COMMON_TABLE1
   { "WorldVMMInit",    WorldVMMInit,                   WorldVMMCleanup},
   { "sharedArea",      SharedArea_Init,                SharedArea_Cleanup},   
   COMMON_TABLE2
   NON_HOST_TABLE
   { "vmmstack",        WorldVMMStackInit,              WorldVMMStackCleanup },
   { "sharedData",      WorldSharedDataInit,            WorldSharedDataCleanup },
   { "action",          Action_WorldInit,               Action_WorldCleanup },
   { "swap",            Swap_WorldInit,                 Swap_WorldCleanup },
   { "alloc",           Alloc_WorldInit,                Alloc_WorldCleanup },
   { "memsched",        MemSched_WorldInit,             MemSched_WorldCleanup },
   { "migrate",         NULL,                           Migrate_WorldCleanup },
   { "timer",           Timer_WorldInit,                Timer_WorldCleanup },
   { "vmmstats",        VMMStats_WorldInit,             VMMStats_WorldCleanup },
   { "conduit",         Conduit_WorldInit,              Conduit_WorldCleanup },
   { "reliability",     Reliability_WorldInit,          Reliability_WorldCleanup },
   { "license",         WorldVMMLicenseInit,            WorldVMMLicenseCleanup }
};

// userworlds
static const ModuleTable userTableInit[] = {
   COMMON_TABLE1
   COMMON_TABLE2
   NON_HOST_TABLE
   { "swap",            Swap_WorldInit,                 Swap_WorldCleanup },
   { "memsched",        MemSched_WorldInit,             MemSched_WorldCleanup },
   { "user",            User_WorldInit,                 User_WorldCleanup },
};

// all other worlds: idle/helper/driver/migration
static const ModuleTable otherTableInit[] = {
   COMMON_TABLE1
   COMMON_TABLE2
   NON_HOST_TABLE
};

/*
 * List of functions called when reaping a world before all the world
 * cleanup functions are called.  These functions will get called
 * regardless of world ref/reader counts.
 */
static const PreCleanupTable preCleanupTable[] = {
   { "net",             Net_WorldPreCleanup },
   { "conduit",         Conduit_WorldPreCleanup },
};


/*
 * Macros
 */

#define TABLE_SIZE(table) (sizeof(table) / sizeof(table[0]))
#define REAP_RETRY_TIME		1000
#define SCSI_REAP_RETRIES	5
#define PTE_MON_PAGE (PTE_P|PTE_RW)

/*
 *----------------------------------------------------------------------
 *
 * WorldLock --
 *
 *      Acquire exclusive access to world module.
 *
 * Results:
 *      Lock for world module is acquired.
 *      Returns the caller's IRQL level.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE SP_IRQL
WorldLock(void)
{
   return(SP_LockIRQ(&worldLock, SP_IRQL_KERNEL));
}

/*
 *----------------------------------------------------------------------
 *
 * WorldUnlock --
 *
 *      Releases exclusive access to world module, which must have
 *      previously been acquired via WorldLock.  Sets the IRQL
 *	level to "prevIRQL".
 *
 * Results:
 *      Lock for world module is released.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
WorldUnlock(SP_IRQL prevIRQL)
{
   SP_UnlockIRQ(&worldLock, prevIRQL);
}

/*
 *----------------------------------------------------------------------
 *
 * WorldIsLocked --
 *
 *      Returns TRUE iff "worldLock" is locked.
 *
 * Results:
 *      Returns TRUE iff "worldLock" is locked.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
WorldIsLocked(void)
{
   return(SP_IsLockedIRQ(&worldLock));
}


/*
 *----------------------------------------------------------------------
 *
 * WorldWaitEvent --
 *
 *      Return a probably unique event (for CpuSched_Wait/Wakeup) for
 *      waiting on the death of the given world ID.
 *
 * Results:
 *      Returns an event id.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE uint32
WorldWaitEvent(World_ID wid) {
   return (uint32)wid;
}


/*
 *----------------------------------------------------------------------
 *
 * World_Init --
 *
 *      Initialize the world management module.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The COS world is initialized and added to the scheduler.
 *
 *----------------------------------------------------------------------
 */
void
World_Init(VMnix_Init *vmnixInit)
{
   World_Handle *cosWorld;
   uint32 version, features;
   int i;
   STATS_ONLY(static Proc_Entry procSwitchStats);

   ASSERT(WORLD_TABLE_LENGTH == Util_RoundupToPowerOfTwo(WORLD_TABLE_LENGTH));
   
   // initialze the stacks region for all worlds
   for (i = 0; i < VMK_NUM_STACK_PDES; i++) {
      MPN mpn = MemMap_AllocAnyKernelPage();
      ASSERT_NOT_IMPLEMENTED(mpn != INVALID_MPN);
      MemMap_SetIOProtection(mpn, MMIOPROT_IO_DISABLE);

      worldStackPTables[i] = KVMap_MapMPN(mpn, TLB_LOCALONLY);
      Util_ZeroPage(worldStackPTables[i]);

      // add pde for this pagetable page to all pagetables
      PT_AddPageTable(VMK_FIRST_STACK_ADDR + i*PDE_SIZE - VMK_FIRST_ADDR,
                      mpn);
   }

   SP_InitLockIRQ("worldLock", &worldLock, SP_RANK_IRQ_MEMTIMER);
   SP_InitLock("wldDeathLk", &worldDeathLock, SP_RANK_LEAF);

   ASM("cpuid" :
       "=a" (version),
       "=d" (features) :
       "a" (1) :
       "ebx", "ecx");

   cpuidFeatures = features;
   Log("cpuidFeatures = 0x%x", cpuidFeatures);

   /* Make sure user VA and LA spaces are sane: */
   ASSERT(VMK_USER_FIRST_LADDR < VMK_USER_LAST_LADDR);
   ASSERT(VMK_USER_FIRST_TEXT_VADDR < VMK_USER_LAST_VADDR);
   ASSERT(VMK_USER_FIRST_MMAP_TEXT_VADDR < VMK_USER_LAST_MMAP_TEXT_VADDR);
   ASSERT(VMK_USER_LAST_MMAP_TEXT_VADDR < VMK_USER_FIRST_MMAP_DATA_VADDR);
   ASSERT(VMK_USER_FIRST_MMAP_DATA_VADDR < VMK_USER_LAST_MMAP_DATA_VADDR);
   ASSERT(VMK_USER_LAST_MMAP_DATA_VADDR < VMK_USER_LAST_VADDR);
   ASSERT(VMK_USER_MIN_STACK_VADDR < VMK_USER_LAST_VADDR);
   ASSERT((VMK_USER_LAST_LADDR - VMK_USER_FIRST_LADDR)
          == PAGES_2_BYTES(VMK_USER_MAX_PAGES) - 1);
   ASSERT(VMK_USER_LAST_VADDR
          == PAGES_2_BYTES(VMK_USER_MAX_PAGES) - 1);
   ASSERT(VMK_USER_FIRST_LADDR < VMK_USER_LAST_LADDR); /* no wrap-around */
   ASSERT(VMK_USER_FIRST_VPN < VMK_USER_LAST_VPN);

   /* Make sure vmk LA space does not overlap user LA space */
   ASSERT(VMK_FIRST_LINEAR_ADDR + VMK_NUM_PDES * PDE_SIZE
          <= VMK_USER_FIRST_LADDR);
   /* Make sure user LA space does not overlap vmm/task LA space */
   ASSERT(VMK_USER_FIRST_LADDR >= VMK_VA_2_LA(VMK_VA_END));
   ASSERT(VMK_USER_LAST_LADDR < VMM_FIRST_LINEAR_ADDR);
   ASSERT(VMK_USER_LAST_LADDR < TASK_BASE);

   /*
    *  Setup default descriptor table.
    */
   Desc_SetDescriptor(&defaultGDT[DEFAULT_CS_DESC],
                      VMM_FIRST_LINEAR_ADDR, VMM_NUM_PAGES + VMK_NUM_CODE_PAGES - 1,
                      CODE_DESC,       // type
                      1, 0, 1, 1, 1);  // S, DPL, present, DB, gran

   Desc_SetDescriptor(&defaultGDT[DEFAULT_DS_DESC],
                      VMM_FIRST_LINEAR_ADDR, VMM_VMK_PAGES - 1,
                      DATA_DESC,       // type
                      1, 0, 1, 1, 1);  // S, DPL, present, DB, gran 

   Desc_SetDescriptor(&defaultGDT[DEFAULT_USER_CODE_DESC],
                      VMK_USER_FIRST_LADDR, VMK_USER_MAX_CODE_SEG_PAGES,
                      CODE_DESC,       // type
                      1, 3, 1, 1, 1);  // S, DPL, present, DB, gran

   Desc_SetDescriptor(&defaultGDT[DEFAULT_USER_DATA_DESC],
                      VMK_USER_FIRST_LADDR, VMK_USER_MAX_PAGES,
                      DATA_DESC,       // type
                      1, 3, 1, 1, 1);  // S, DPL, present, DB, gran

   Desc_SetDescriptor(&defaultGDT[DEFAULT_TSS_DESC],
                      TASK_BASE, DEFAULT_TASK_SIZE - 1,
                      TASK_DESC,           // type
                      0, 0, 1, 1, 0);      // S, DPL, present, DB, gran 

   Desc_SetDescriptor(&defaultGDT[DEFAULT_NMI_TSS_DESC],
		      NMI_TASK_BASE, NMI_TASK_SIZE - 1,
		      TASK_DESC,
		      0, 0, 1, 1, 0);

   /* worlds proc directory */
   Proc_Register(&procWorlds, "vm", TRUE);

   POST_Register("World", WorldPOST, NULL);

   STATS_ONLY({
      Proc_InitEntry(&procSwitchStats);
      procSwitchStats.read = WorldSwitchStatsReadHandler;
      procSwitchStats.write = WorldSwitchStatsWriteHandler;
      Proc_Register(&procSwitchStats, "switchStats", FALSE);
   });

   cosWorld = WorldNewCOSWorld(vmnixInit->stackMPN);
   ASSERT(cosWorld != NULL);
   // sanity check: links offset
   ASSERT(((void *) cosWorld) == ((void *) &cosWorld->sched.links));

   MY_RUNNING_WORLD = cosWorld;
   CpuSched_DisablePreemption();
   // add running console world to scheduler
   Sched_AddRunning();
}



/*
 *----------------------------------------------------------------------
 *
 * WorldInit --
 *
 *      Steps through the mTable, and calls the init function for all
 *      entries.  
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Lots.  a new world is born.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
WorldInit(World_Handle *world, 
          const ModuleTable *mTable, 
          int tableLen, 
          World_InitArgs *args)
{
   const ModuleTable *m;

   //Make sure world->modulesInited has enough bits
   ASSERT(tableLen < 32);

   VMLOG(0, world->worldID,
         "starting world init via module table: '%s', 0x%x, %d", 
         args->name, (uint32)mTable, tableLen);

   for (m = mTable; m < mTable + tableLen; m++) {
      if (m->init) {
         VMK_ReturnStatus err;

         VMLOG(1, world->worldID, "Starting %s", m->name);
         if ((err = m->init(world, args)) != VMK_OK) {
            VmWarn(world->worldID, "init fn %s failed with: %s!",
                   m->name, VMK_ReturnStatusToString(err));
            return err;
         }
      } else {
         VMLOG(1, world->worldID, "No init fn for %s.", m->name);
      }
      world->modulesInited |= (1 << (m - mTable));
   }

   VMLOG(1, world->worldID, "init done");
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * WorldCleanup --
 *
 *      Steps through the mTable, and calls the exit function for all
 *      entries that were initialized.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Lots.  World is cleaned up.
 *
 *----------------------------------------------------------------------
 */
static void
WorldCleanup(World_Handle *world)
{
   const ModuleTable *mTable = world->moduleTable;
   const ModuleTable *m;
   int tableLen = world->moduleTableLen;
   uint32 generation;
   World_ID wid = world->worldID;

   ASSERT(mTable);
   ASSERT(!WorldIsLocked());
   ASSERT(List_IsEmpty(&world->heldSemaphores));

   VmLog(world->worldID, "Starting cleanup via module table");
   if (world->refCount > 0) {
      VmLog(world->worldID, "refCount=%d", world->refCount);
   }

   for (m = mTable + tableLen - 1; m >= mTable; m--) {
      if (m->exit != NULL && (world->modulesInited & (1 << (m - mTable)))) {
         VMLOG(1, world->worldID, "Stopping %s", m->name);
	 m->exit(world);
      } else {
         VMLOG(1, world->worldID, 
               "Not stopping %s: initfn = 0x%x, exitfn = 0x%x, inited = %d",
               m->name, (uint32)m->init, (uint32)m->exit,
               world->modulesInited & (1 << (m - mTable)));
      }
      world->modulesInited &= ~(1 << (m - mTable));
   }

   VmLog(world->worldID, "cleanup done for '%s', rc=%d",
         world->worldName, world->refCount);

   SP_Lock(&worldDeathLock);
   generation = world->generation;
   /* 
    * The memset should help catch code that attempts to use 
    * worlds after they have been reaped.
    */
   memset(world, 0xff, sizeof *world);
   world->generation = generation;


   /*
    * Don't need world lock here because there are no valid handles to
    * this world outstanding, so we're only user.  Except for the
    * world-death waiters.
    */
   world->refCount = 0;
   world->readerCount = 0;
   world->inUse = FALSE;

   /* Wakeup waiters in World_ReleaseAndWaitForDeath: */
   CpuSched_Wakeup(WorldWaitEvent(wid));
   SP_Unlock(&worldDeathLock);
}


/*
 *----------------------------------------------------------------------
 *
 * WorldFind --
 *
 *      Return a world pointer based on it world id.
 *
 * Results:
 *      A pointer to the world that matches the world id or NULL
 *	if no world is found.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static World_Handle *
WorldFind(World_ID worldID)
{
   World_Handle *world;

   if (worldID == INVALID_WORLD_ID) {
      return NULL;
   }

   world = &worlds[worldID % WORLD_TABLE_LENGTH];

   // ensure id matches, world in use
   if ((world->worldID != worldID) || !world->inUse) {
      world = NULL;
   }

   return(world);
}

/*
 *----------------------------------------------------------------------
 *
 * World_FindDebug --
 *
 *      Return a world pointer based on it world id.  Should only be
 *	used by functions in debug.c.
 *
 * Results:
 *      A pointer to the world that matches the world id or NULL
 *	if no world is found.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
World_Handle *
World_FindDebug(World_ID worldID)
{
   return WorldFind(worldID);
}

/*
 *----------------------------------------------------------------------
 *
 * WorldFindOptReaderLock --
 *
 *      Return a world pointer based on it world id.   Atomic version
 *      of WorldFind.  If readLock is TRUE the world won't be reaped
 *      until a corresponding World_Release call is made.
 *
 * Results:
 *      A pointer to the world that matches the world id or NULL
 *	if no world is found.
 *
 * Side effects:
 *	Increases refCount and possibly readerCount.
 *
 *----------------------------------------------------------------------
 */
static INLINE World_Handle *
WorldFindOptReaderLock(World_ID worldID, Bool readLock)
{
   World_Handle *world;
   SP_IRQL prevIRQL;

   prevIRQL = WorldLock();

   world = WorldFind(worldID);

   if (world != NULL) {
      if (world->reapStarted) {
         /* The cleanup of the world handle struct has been started.  
          * Things are possibly in an inconsistent state [and the readLock
          * parameter definitely won't work as expected] so pretend we
          * didn't find the world.
          */
         world = NULL;
      } else {
         if (readLock) {
            world->readerCount++;
#ifdef VMX86_DEBUG
            /*
             * Tracking all the callers is too expensive to always do,
             * so just enable it if you're debugging a reference
             * counting problem.
             */
            if (0) {
               int i;
               void* caller = __builtin_return_address(0);
               unsigned idx;

               for (i = 0; i < WORLD_READER_COUNT_HOLDERS; i++) {
                  if (world->countHolders[i] == caller) {
                     idx = i; // write over exisiting
                     break;
                  }
                  if (world->countHolders[i] == 0) {
                     idx = i; // write over empty
                     break;
                  }
               }
               if (i == WORLD_READER_COUNT_HOLDERS) {
                  idx = world->countHolderIndex % WORLD_READER_COUNT_HOLDERS; // "random" entry
               }
               ASSERT(idx < WORLD_READER_COUNT_HOLDERS);
               world->countHolders[idx] = caller;
               world->countHolderIndex++; // also, total # of calls
            }
#endif
         } else {
            ASSERT(world->refCount >= 0);
            world->refCount++;   
         }
      }
   }

   WorldUnlock(prevIRQL);

   return world;
}

/*
 *----------------------------------------------------------------------
 *
 * World_SetIdleWorld --
 *
 *      Sets the pointer to the idle world for this PCPU.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
void
World_SetIdleWorld(int pcpuNum, World_Handle * world)
{
   idleWorlds[pcpuNum] = world;
}

/*
 *----------------------------------------------------------------------
 *
 * World_GetIdleWorld --
 *
 *      Return a pointer to the idle world for this PCPU.
 *
 * Results:
 *      A pointer to the idle world
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
World_Handle *
World_GetIdleWorld(int pcpuNum)
{
   return idleWorlds[pcpuNum];
}

/*
 *----------------------------------------------------------------------
 *
 * World_Exists --
 *
 *      Return TRUE if the world identified by this world ID exists.  (A
 *	world "exists" until its been completely destroyed.  World_Find
 *	could still return NULL, as the world may be in the reap state.)
 *
 * Results:
 *      TRUE if the world identified by this world ID exists.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
Bool
World_Exists(World_ID worldID)
{
   Bool exists;
   SP_IRQL prevIRQL;

   prevIRQL = WorldLock();
   exists = ((WorldFind(worldID) != NULL) ? TRUE : FALSE);
   WorldUnlock(prevIRQL);

   return exists;
}

/*
 *----------------------------------------------------------------------
 *
 * WorldAllWorlds --
 *
 *      Sets elements of "ids" to all existing world identifiers,
 *	up to a maximum of "n" identifiers.  Sets "n" to the number
 *	of identifiers returned in "ids".  Returns the total number
 *	of existing worlds, which may be larger than "n".
 *
 *	Caller is responsible for grabbing the worldLock, or otherwise
 *	ensuring consistency.
 *
 * Results:
 *      Modifies "ids" to contain up to "n" existing world identifiers.
 *	Sets "n" to number of identifiers returned in "ids".
 *	Returns the total number of existing worlds.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static uint32
WorldAllWorlds(World_ID *ids,    // OUT: existing world ids
	       uint32 *n)       // IN/OUT: size of ids
{
   uint32 i, count, next;

   // initialize
   count = 0;
   next = 0;

   // iterate over all world slots
   for (i = 0; i < WORLD_TABLE_LENGTH; i++) {
      // capture valid world ids 
      World_Handle *world = &worlds[i];
      if (world->inUse) {
         count++;
         if (next < *n) {
            ids[next++] = world->worldID;
         }
      }
   }

   // success
   *n = next;
   return(count);
}

/*
 *----------------------------------------------------------------------
 *
 * World_AllWorldsDebug --
 *
 *      Sets elements of "ids" to all existing world identifiers,
 *	up to a maximum of "n" identifiers.  Sets "n" to the number
 *	of identifiers returned in "ids".  Returns the total number
 *	of existing worlds, which may be larger than "n".  Should only
 *	be used by functions in debug.c.
 *
 * Results:
 *      Modifies "ids" to contain up to "n" existing world identifiers.
 *	Sets "n" to number of identifiers returned in "ids".
 *	Returns the total number of existing worlds.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

uint32
World_AllWorldsDebug(World_ID *ids, // OUT:     existing world ids
		     uint32 *n)	    // IN/OUT:  size of ids
{
   return WorldAllWorlds(ids, n);
}

/*
 *----------------------------------------------------------------------
 *
 * World_AllWorlds --
 *
 *      Sets elements of "ids" to all existing world identifiers,
 *	up to a maximum of "n" identifiers.  Sets "n" to the number
 *	of identifiers returned in "ids".  Returns the total number
 *	of existing worlds, which may be larger than "n".  Atomic
 *	version of WorldAllWorlds.
 *
 * Results:
 *      Modifies "ids" to contain up to "n" existing world identifiers.
 *	Sets "n" to number of identifiers returned in "ids".
 *	Returns the total number of existing worlds.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

uint32
World_AllWorlds(World_ID *ids,	// OUT:     existing world ids
                uint32 *n)	// IN/OUT:  size of ids
{
   int count;
   SP_IRQL prevIRQL;

   // acquire global lock
   prevIRQL = WorldLock();

   count = WorldAllWorlds(ids, n);

   // release global lock
   WorldUnlock(prevIRQL);

   return(count);
}


/*
 *----------------------------------------------------------------------
 *
 * World_ActiveGroupCount --
 *
 *      Fetch the number of active world groups.
 *
 * Results:
 *	An integer between 0 and MAX_WORLDS
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
int 
World_ActiveGroupCount(void)
{
   return Atomic_Read(&worldActiveGroupCount);
}


/*
 *----------------------------------------------------------------------
 *
 * WorldFindUnusedSlot --
 *
 *      Finds an unused slot in the world array.
 *
 * Results:
 *
 *      A World_Handle on success, NULL if there are no free worlds
 *      left.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static World_Handle*
WorldFindUnusedSlot(void)
{
   int i;
   static int lastUsedIndex = 0;

   ASSERT(WorldIsLocked());

   for (i = 0; i < WORLD_TABLE_LENGTH; i++) {
      int nextWorld = lastUsedIndex++ % WORLD_TABLE_LENGTH;
      if (!worlds[nextWorld].inUse) {
         ASSERT(worlds[nextWorld].refCount == 0);
         ASSERT(worlds[nextWorld].readerCount == 0);
         LOG(1, "FindSlot: %d %d %d", nextWorld, i,  WORLD_TABLE_LENGTH);
         return &worlds[nextWorld];
      }
   }
   return NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * World_WorldFromStack --
 *
 *      Given a stack address return a pointer to the corresponding 
 *	world entry.
 *
 * Results:
 *      A pointer to the world entry that corresponds to this stack 
 *	virtual address.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
World_Handle *
World_GetWorldFromStack(VA va)
{
   int worldIndex = VA_2_VPN(va - VMK_FIRST_STACK_ADDR) / WORLD_VMK_NUM_STACK_VPNS;
   if (worldIndex >= 0 && worldIndex < WORLD_TABLE_LENGTH) {
      return &worlds[worldIndex];
   } else {
      return NULL;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * World_GetStackMPN --
 *
 *      Return the stack MPN that corresponds to this stack virtual
 *	address.
 *
 * Results:
 *      The stack MPN for this virtual address.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
MPN
World_GetStackMPN(VA va)
{
   VMK_PTE *ptable;
   VMK_PTE pte;
   VPN stackPage = VA_2_VPN(va - VMK_FIRST_STACK_ADDR);
   int ptableNum = stackPage / VMK_PTES_PER_PDE;
   ASSERT(ptableNum < VMK_NUM_STACK_PDES);

   ptable = worldStackPTables[ptableNum];
   pte = ptable[stackPage - ptableNum * VMK_PTES_PER_PDE];
   if (PTE_PRESENT(pte)) {
      return VMK_PTE_2_MPN(pte);
   } else {
      return INVALID_MPN;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * World_ConfigArgs --
 *
 *      Config the arguements for creating a new world.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
void
World_ConfigArgs(World_InitArgs *args, 
                 const char *name,
                 uint32 flags,
                 World_ID worldGroupNumber,
                 Sched_ClientConfig *sched)
{
   memset(args, 0, sizeof(*args));
   args->func = CpuSched_StartWorld;
   args->flags = flags;
   args->name = name;
   args->groupLeader = worldGroupNumber;
   args->sharedAreaArgs = NULL;
   args->sched = sched;
   args->vcpuid = 0;
}


/*
 *----------------------------------------------------------------------
 *
 * World_ConfigUSERArgs --
 *
 *      Config the arguements for creating a new USER world.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
void
World_ConfigUSERArgs(World_InitArgs *args, 
                     VMnix_CreateWorldArgs *vmnixArgs)
{
   ASSERT((vmnixArgs->flags & VMNIX_USER_WORLD) != 0);
   World_ConfigArgs(args, vmnixArgs->name, WORLD_USER, 
                    vmnixArgs->groupLeader, &vmnixArgs->sched);
}


/*
 *----------------------------------------------------------------------
 *
 * World_ConfigVMMArgs --
 *
 *      Config the arguements for creating a new VMM world.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
void
World_ConfigVMMArgs(World_InitArgs *args, 
                    VMnix_CreateWorldArgs *vmnixArgs)
{
   ASSERT((vmnixArgs->flags & VMNIX_USER_WORLD) == 0);
   World_ConfigArgs(args, vmnixArgs->name, WORLD_VMM, 
                    vmnixArgs->groupLeader, &vmnixArgs->sched);
   args->sharedAreaArgs = &vmnixArgs->sharedAreaArgs;
   args->vcpuid = vmnixArgs->vcpuid;
}


/*
 *----------------------------------------------------------------------
 *
 * WorldNewInt --
 *
 *      Create a new world ready to run an initial function.
 *
 * Results:
 *      If successful returns VMK_OK, and handle will point to
 *      the new world.
 *
 *      otherwise returns a VMK_ReturnStatus error value
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
WorldNewInt(World_InitArgs *args,
            const ModuleTable *m,
            int tableLen,
            World_Handle **handle)
{
   World_Handle *newWorld = NULL;
   SP_IRQL prevIRQL;
   VMK_ReturnStatus retval;

   prevIRQL = WorldLock();

   newWorld = WorldFindUnusedSlot();

   if (newWorld == NULL) {
      WorldUnlock(prevIRQL);
      Warning("Max worlds exceeded.");
      return VMK_LIMIT_EXCEEDED;
   }

   ASSERT(newWorld->refCount == 0 && !newWorld->inUse);
   WorldTableInitEntry(newWorld, args);
   WorldUnlock(prevIRQL);

   retval = WorldInit(newWorld, m, tableLen, args);

   newWorld->moduleTable = m;
   newWorld->moduleTableLen = tableLen;

   if (retval != VMK_OK) {
      VmWarn(newWorld->worldID, "WorldInit failed: trying to cleanup.");
      WorldCleanup(newWorld);
      return retval;
   }

   *handle = newWorld;
   VmLog(newWorld->worldID, "Successfully created new world: '%s'", newWorld->worldName);
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * WorldNewCOSWorld --
 *
 *      Creates a new console os (aka COS, aka Service Console) world.
 *
 * Results:
 *      The new world, or NULL on failure.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static World_Handle *
WorldNewCOSWorld(MPN stackMPN)
{
   World_InitArgs args;
   Sched_ClientConfig sched;
   World_Handle *world;

   Log("Creating COS world");
   // configure console world
   Sched_ConfigInit(&sched, SCHED_GROUP_NAME_SYSTEM);
   Sched_ConfigSetCpuAffinity(&sched, CPUSCHED_AFFINITY(0));
   Sched_ConfigSetCpuMinPct(&sched, CONFIG_OPTION(CPU_COS_MIN_CPU));

   World_ConfigArgs(&args, "console", WORLD_SYSTEM | WORLD_HOST,
                    WORLD_GROUP_DEFAULT, &sched);
   args.COSStackMPN = stackMPN;

   WorldNewInt(&args, consoleTableInit, TABLE_SIZE(consoleTableInit), &world);

   return world;
}


/*
 *----------------------------------------------------------------------
 *
 * World_NewIdleWorld
 *
 *      Each PCPU gets a world to handle random things that are not in
 *      any other specific world (i.e. the idle loop). 
 *
 * Results:
 *      VMK_ReturnStatus from World_New
 *      And, if successful returns the new world handle,
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
World_NewIdleWorld(PCPU pcpuNum, World_Handle **world)
{
   char nameBuf[20];
   VMK_ReturnStatus status;
   Sched_ClientConfig sched;
   World_InitArgs args;

   Sched_ConfigInit(&sched, SCHED_GROUP_NAME_IDLE);
   Sched_ConfigSetCpuAffinity(&sched, CPUSCHED_AFFINITY(pcpuNum));

   Log("Creating idle world for pcpu %d.", pcpuNum);
   snprintf(nameBuf, sizeof nameBuf, "idle%d", pcpuNum);
   World_ConfigArgs(&args, nameBuf, WORLD_SYSTEM | WORLD_IDLE,
                    WORLD_GROUP_DEFAULT, &sched);
   status = World_New(&args, world);
   if (status != VMK_OK) {
      SysAlert("Couldn't create %s (status=%x)", nameBuf, status);
      if (vmx86_debug) {
         Panic("World_New for %s failed\n", nameBuf);
      }
      *world = NULL;
   } else {
      World_SetIdleWorld(pcpuNum, *world);
   }

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * World_NewDefaultWorld
 *
 *      Create default worlds (no flags, default sched config).
 *
 * Results:
 *      VMK_ReturnStatus from World_New
 *      And, if successful returns the new world handle,
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
World_NewDefaultWorld(const char *name, World_Handle **world)
{
   VMK_ReturnStatus status;
   Sched_ClientConfig sched;
   World_InitArgs args;

   Sched_ConfigInit(&sched, SCHED_GROUP_NAME_DRIVERS);
   World_ConfigArgs(&args, name, 0, WORLD_GROUP_DEFAULT, &sched);
   status = World_New(&args, world);
   if (status != VMK_OK) {
      SysAlert("Couldn't create %s (status=%x)", name, status);
      *world = NULL;
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * World_DestroySlavePCPU --
 *
 *      Destroy a slave PCPU because the PCPU didn't start up.
 *
 *	NOTE: We don't free the address space because if we do we
 *	      can reset the machine.  It appears that even if a CPU doesn't
 *	      come up completely it still may needs it address space. 
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	Proc entry deleted and scheduler state set to SCHED_NEW.
 *
 *----------------------------------------------------------------------
 */
void
World_DestroySlavePCPU(uint32 pcpuNum)
{
   World_Handle *world = World_GetIdleWorld(pcpuNum);

   WorldCleanup(world); 
}


/*
 *----------------------------------------------------------------------
 *
 * World_New --
 *
 *      Create a new world ready to run CpuSched_StartWorld().
 *
 * Results:
 *      If successful returns VMK_OK, and handle will point
 *      to the world.
 *
 *      otherwise returns an error, and handle isn't modified.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
World_New(World_InitArgs *args, World_Handle **handle)
{
   const ModuleTable* initTable;
   int initTableSize;

   if (args->groupLeader != WORLD_GROUP_DEFAULT &&
       !World_Exists(args->groupLeader)) {
      return VMK_BAD_PARAM;
   }

   if (args->flags & WORLD_USER) {
      initTable = userTableInit;
      initTableSize = TABLE_SIZE(userTableInit);
   } else if (args->flags & WORLD_VMM) {
      initTable = vmmTableInit;
      initTableSize = TABLE_SIZE(vmmTableInit);
   } else {
      initTable = otherTableInit;
      initTableSize = TABLE_SIZE(otherTableInit);
   }      

   return WorldNewInt(args, initTable, initTableSize, handle);
}


/*
 *----------------------------------------------------------------------
 *
 * World_Cleanup --
 *
 *      Cleanup all non-system worlds so that SCSI & Net drivers can be
 *      unloaded.
 *
 * Results:
 *      VMK_OK if all non-system worlds were cleaned up, VMK_BUSY if not.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
World_Cleanup(Bool force)
{
   int i;

   // cleanup non-system worlds
   for (i = 0; i < WORLD_TABLE_LENGTH; i++) {
      World_Handle *world = World_Find(worlds[i].worldID);

      if (world != NULL) {
         if (!World_IsSYSTEMWorld(world)) {
            if (Sched_Remove(world) != VMK_OK && !force) {
               World_Release(world);
               return VMK_BUSY;
            }
         }
         World_Release(world);
      }
   }

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * World_LateCleanup --
 *
 *      For now, cleans up idle worlds before vmkernel is unloaded.
 *
 * Results:
 *      none
 *
 * Side effects:
 *	Idle worlds killed - other worlds better be runnable from here!
 *
 *----------------------------------------------------------------------
 */
void
World_LateCleanup(void)
{
   PCPU p;

   // cleanup idle worlds
   for (p = 0; p < numPCPUs; p++) {
      Sched_Remove(idleWorlds[p]);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * World_Bind --
 *
 *      Bind to the given world (increase its hostCount).
 *
 * Results:
 *      0 if bound to the world, non-zero if something goes wrong.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
int
World_Bind(World_ID worldID)
{
   World_Handle *world;
   SP_IRQL prevIRQL;
   int status;

   prevIRQL = WorldLock();

   world = WorldFind(worldID);
   if ((world == NULL) || world->reapStarted) {
      WarnVmNotFound(worldID);
      status = 1;
   } else {
      /*
       * World_Bind only applies to VMM worlds.
       */
      if (World_IsVMMWorld(world)) {
         world->hostCount++;
         VMLOG(1, world->worldID, "hostCount now %d", world->hostCount);
      } else {
         VMLOG(3, world->worldID, "World_Bind ignored for non-VMM world.");
      }
      status = 0;
   }

   WorldUnlock(prevIRQL);

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * World_Destroy --
 *
 *	Undo a World_Bind.  Reduce/reset the hostCount of the world.  
 *      If the hostCount is not more than 0, then call World_Kill 
 *      on the world.
 *
 * Results:
 *      A VMK_ReturnStatus value.
 *
 * Side effects:
 *	World is killed if/when host count goes to 0.
 *
 *---------------------------------------------------------------------- */
VMK_ReturnStatus
World_Destroy(World_ID worldID, Bool clearHostCount)
{
   World_Handle *world;

   world = World_Find(worldID);
   if (world == NULL) {
      WarnVmNotFound(worldID);
      return VMK_NOT_FOUND;
   } 

   if (World_IsVMMWorld(world)) {
      int hostCount;
      SP_IRQL prevIRQL;

      /*
       * Drop host count.  Only terminate if its zero.  (The hostCount
       * is used to track active VMXen that are associated with this
       * VMM world.)
       */
      prevIRQL = WorldLock();
      if (clearHostCount) {
         world->hostCount = 0;
      } else {
         ASSERT(world->hostCount > 0);
         world->hostCount--;
      }
      hostCount = world->hostCount;
      WorldUnlock(prevIRQL);

      if (hostCount > 0) {
         VMLOG(0, world->worldID, "host count present: %d", world->hostCount);
      } else {
         World_Kill(world);
      }
   } else {
      /*
       * Non-VMM worlds are simply killed.
       */
      World_Kill(world);
   }

   World_Release(world);

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * World_DestroyVmms --
 *
 *      Reduce or clear host bindings and kill all VMM worlds of a
 *      world group if the host count for the VMM world is 0.
 *
 * Results: 
 *      None
 *
 * Side effects:
 *      All vmm worlds in the group may be destroyed.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
World_DestroyVmms(World_Handle *world, Bool waitForDeath, Bool clearHostCount)
{
   int i;
   World_VmmGroupInfo *vmmGroup;
   World_Handle *vmmLeader = World_Find(World_GetVmmLeaderID(world));

   if (vmmLeader == NULL) {
      return VMK_NOT_FOUND;
   }

   vmmGroup = World_VMMGroup(world);
   ASSERT(vmmGroup->memberCount > 0);
     
   if (clearHostCount) { 
      VmWarn(world->worldID, "VMMWorld group leader = %d, members = %d",
             vmmLeader->worldID, vmmGroup->memberCount);
   }
   // We need to Kill all vcpus.   
   for (i = 0; i < vmmGroup->memberCount; i++) {
      VMK_ReturnStatus status = World_Destroy(vmmGroup->members[i], clearHostCount);
      if (status != VMK_OK) {
         VmWarn(world->worldID, "Couldn't destroy world %d", vmmGroup->members[i]);
      }
   }

   // wait for vmm leader
   if (waitForDeath) {
      World_ReleaseAndWaitForDeath(vmmLeader);
   } else {
      World_Release(vmmLeader);
   }
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * World_GroupKill --
 *
 *      Kill a virtual machine, user cartel, or other world without
 *      its cooperation.
 *
 * Results: 
 *      None
 *
 * Side effects:
 *      Given world and all of its associated worlds (i.e., other VMM
 *      Worlds in its group) are given last rites.
 *
 *----------------------------------------------------------------------
 */
void
World_GroupKill(World_Handle *world)
{
   World_Handle *leader = World_Find(World_GetGroupLeaderID(world));

   if (leader != NULL) {
      if (World_IsVMMWorld(leader)) {
         // destroy all vmm worlds, don't wait for death
         World_DestroyVmms(world, FALSE, TRUE);
      } else if (World_IsUSERWorld(leader)) {
         // destroy all userworlds in the cartel
         User_CartelKill(leader, FALSE);
      } else {
         // destroy the single world in the group
         ASSERT(leader == world);
         World_Kill(world);
      }
      World_Release(leader);
   } else {
      World_Kill(world);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * WorldKillUnconditional --
 *
 *      Kill the given world regardless of its state and don't worry about
 *      the consequences of such actions.  The world is either forced awake
 *      (if not removable) or scheduled for reaping.  The destruction is
 *      completed in WorldReap() when readerCount goes to zero and there
 *      are no active SCSI handles.
 *
 * Results: 
 *      None
 *
 * Side effects:
 *      World is started into its death spiral, when it next runs it
 *      will clean itself up and terminate
 *
 *----------------------------------------------------------------------
 */
static void
WorldKillUnconditional(World_Handle *world)
{
   world->deathPending = TRUE;
   world->killLevel = WORLD_KILL_UNCONDITIONAL;

   if (Sched_Remove(world) != VMK_OK) {
      VmLog(world->worldID, "deathPending set; world is running, waking up");
      CpuSched_ForceWakeup(world);
   } else {
      VmLog(world->worldID, "deathPending set; world not running, scheduling reap");
      WorldScheduleReap(world->worldID, TRUE);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * WorldKillDemand --
 *
 *      Kill the given world somewhat forcefully.  Set the killLevel to
 *      DEMAND and wake up the world.  The world will notice that the
 *      CpuSched_Wait* funtion returns a VMK_DEATH_PENDING, and it is
 *      supposed to get to safepoint faster.
 *
 * Results: 
 *      None
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
static void
WorldKillDemand(void *arg)
{
   World_ID worldID = (World_ID)arg;
   World_Handle *world = World_Find(worldID);

   if (world == NULL) {
      // World gone, nothing more to do.
      return;
   }

   // We need to take stronger measures to kill this guy.
   world->killLevel = WORLD_KILL_DEMAND;
   CpuSched_ForceWakeup(world);

   World_Release(world);

}

/*
 *----------------------------------------------------------------------
 *
 * World_Kill--
 *
 *      Kill the given world nicely.  This is a "nice" kill where we wait
 *      for the world to get to a safepoint (vmkernel entry/exit) and then
 *      kill it.  deathPending is set, and killLevel is NICE.  If the world
 *      takes too long to get to safepoint, we will call WorldKillDemand to
 *      take stronger measures.
 *
 * Results: 
 *      None
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
void
World_Kill(World_Handle* world)
{
   world->deathPending = TRUE;
   world->killLevel = WORLD_KILL_NICE;

   // if the world has never been scheduled, we can be harsh right now
   if (World_CpuSchedRunState(world) == CPUSCHED_NEW) {
      WorldKillUnconditional(world);
   } else {
#if defined(VMX86_DEVEL) && defined(VMX86_DEBUG)
      // for obj builds, call demand directly every so often to exercise
      // this path more
      if ((RDTSC() % 2) == 0) {
         WorldKillDemand((void*)world->worldID);
         return;
      }
#endif
      Timer_Add(MY_PCPU, (Timer_Callback) WorldKillDemand,
                WORLD_KILL_TIMEOUT_SECS * 1000, 0, (void*)world->worldID);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * WorldSetupGDT  --
 *
 *      Creates a GDT to be used by the specified world, and copies
 *      defaultGDT into it.
 *
 * Results:
 *      A VMK_ReturnStatus value.
 *
 * Side effects:
 *	does a XMap to map the world's GDT, modifies the world structure
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
WorldSetupGDT(World_Handle *world) 
{
   uint32 i;
   int numRanges = 0;
   XMap_MPNRange range[GDT_AREA_LEN];
   // Make sure that we have allocated all the 
   // stack pages before we map it
   for (i = 0; i < GDT_AREA_LEN; i++) {
      world->gdtMPN[i] = MemMap_AllocAnyKernelPage();
      range[i].startMPN = world->gdtMPN[i];
      range[i].numMPNs = 1;
      numRanges++;
   }

   world->kernelGDT = XMap_Map(GDT_AREA_LEN, range, numRanges);
   if (world->kernelGDT == NULL) {
      VmWarn(world->worldID, "Couldn't map GDT");
      return VMK_FAILURE;
   }
   memcpy(world->kernelGDT, &defaultGDT, sizeof(defaultGDT));
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * WorldASInit  --
 *
 *      Create the basic page tables for the given world.
 *
 * Results:
 *      A VMK_ReturnStatus value.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
WorldASInit(World_Handle *world, UNUSED_PARAM(World_InitArgs *args))
{
   VMK_PDPTE *pageRoot = NULL;
   VMK_PTE *pageTables[MON_PAGE_TABLE_LEN];
   Task *task = NULL;
   Task *nmiTask = NULL;
   int i;
   static const int numMonPTs = VMM_NUM_PAGES/VMK_PTES_PER_PDE;
   XMap_MPNRange range;

   ASSERT(numMonPTs <= MON_PAGE_TABLE_LEN);

   world->taskMPN = INVALID_MPN; 
   world->nmiStackMPN = INVALID_MPN;

   for (i = 0; i < numMonPTs; i++) {
      world->pageTableMPNs[i] = INVALID_MPN;
      pageTables[i] = NULL;
   }

   /*
    * Allocate page roots.
    */
   pageRoot = PT_AllocPageRoot(&world->pageRootMA, TLB_GetVMKernelPDir());
   if (pageRoot == NULL) {
      goto exit;
   }

   world->nmiStackMPN = MemMap_AllocAnyKernelPage();
   if (world->nmiStackMPN == INVALID_MPN) {
      goto exit;
   }
   range.startMPN = world->nmiStackMPN;
   range.numMPNs = 1;
   world->nmiStackStart = (VA) XMap_Map(1, &range, 1);
   if (world->nmiStackStart == (VA) NULL) {
      goto exit;
   }

   /*
    * Allocate and initialize world's x86 task structure
    */
   world->taskMPN = MemMap_AllocAnyKernelPage();
   if (world->taskMPN == INVALID_MPN) {
      goto exit;
   }
   ASSERT(VMK_IsValidMPN(world->taskMPN));
   MemMap_SetIOProtection(world->taskMPN, MMIOPROT_IO_DISABLE);

   task = (Task *)KVMap_MapMPN(world->taskMPN, TLB_LOCALONLY);
   if (task == NULL) {
      goto exit;
   }

   ASSERT(DEFAULT_TASK_SIZE + NMI_TASK_SIZE <= PAGE_SIZE);

   IDT_DefaultTaskInit(task, 
		       0,            // task is running, no need to set eip
		       World_GetVMKStackTop(world),
		       world->pageRootMA);

   /* 
    * We give each task page an interrupt redirection bit map and 
    * an io bit map, both with all bits set.  This disallows all port
    * access and all interrupt redirection.
    */
   task->IOMapBase = sizeof(Task) + INTERRUPT_REDIRECTION_BITMAP_SIZE;
   memset((char *)(task + 1), 0xff, 
          INTERRUPT_REDIRECTION_BITMAP_SIZE + IO_PERMISSION_BITMAP_SIZE);

   nmiTask = (Task*) (((uint32) task) + DEFAULT_TASK_SIZE);

   IDT_DefaultTaskInit(nmiTask, 
		       (uint32)CommonNmiHandler, 
		       world->nmiStackStart + PAGE_SIZE - 4,
		       world->pageRootMA);
   nmiTask->eflags = 0;

   KVMap_FreePages(task);
   task = NULL;
   nmiTask = NULL;

   /*
    * setup monitor page tables (see vmx/public/addrlayout32.h)
    */
   for (i = 0; i < numMonPTs; i++) {
      pageTables[i] = PT_AllocPageTable(world->pageRootMA, 
                                        VMM_FIRST_LINEAR_ADDR + i * PDE_SIZE,
                                        PTE_PAGE_TABLE,
                                        NULL, &world->pageTableMPNs[i]);
      if (pageTables[i] == NULL) {
         goto exit;
      }
   }

   pageTables[0][MMU_ROOT_START] = VMK_MAKE_PTE(MA_2_MPN(world->pageRootMA), 
                                                0, PTE_KERNEL);

   for (i = 0; i < numMonPTs; i++) {
      pageTables[0][MON_PAGE_TABLE_START + i] = 
         VMK_MAKE_PTE(world->pageTableMPNs[i], 0, PTE_KERNEL);
   }
   pageTables[0][TASK_PAGE_START] = VMK_MAKE_PTE(world->taskMPN, 0, PTE_KERNEL);

   ASSERT(VMK_NUM_PDPTES == MMU_PAE_PAGE_DIR_LEN);
   for (i = 0; i < VMK_NUM_PDPTES; i++) {
      pageTables[0][MMU_PAE_PAGE_DIR_START + i] = 
         VMK_MAKE_PTE(VMK_PTE_2_MPN(pageRoot[i]), 0, PTE_KERNEL);
   }

   for (i = 0; i < numMonPTs; i++) {
      PT_ReleasePageTable(pageTables[i], NULL);
   }

   PT_ReleasePageRoot(pageRoot);

   if (WorldSetupGDT(world) != VMK_OK) {
      goto exit;
   }

   return VMK_OK;

exit:
   for (i = 0; i < MON_PAGE_TABLE_LEN; i++) {
      if (pageTables[i] != NULL) {
         PT_ReleasePageTable(pageTables[i], NULL);
         MemMap_FreeKernelPage(world->pageTableMPNs[i]);
      }
   }
   if (task != NULL) {
      KVMap_FreePages(task);
   }
   if (world->taskMPN != INVALID_MPN) {
      ASSERT(world->taskMPN != 0);
      MemMap_FreeKernelPage(world->taskMPN);
   }
   if (world->nmiStackMPN != INVALID_MPN) {
      ASSERT(world->nmiStackMPN != 0);
      MemMap_FreeKernelPage(world->nmiStackMPN);
   }
   if (pageRoot != NULL) {
      PT_ReleasePageRoot(pageRoot);
   }
   if (world->pageRootMA != 0) {
      PT_FreePageRoot(world->pageRootMA);
   }
   if (world->kernelGDT != NULL) {
      XMap_Unmap(GDT_AREA_LEN, world->kernelGDT);
   }
   if (world->nmiStackStart != (VA) NULL) {
      XMap_Unmap(1, (void*)world->nmiStackStart);
   }
   for (i=0; i<GDT_AREA_LEN; i++) {
      if ((world->gdtMPN[i] != INVALID_MPN) &&
	  (world->gdtMPN[i] != 0)) {
	 MemMap_FreeKernelPage(world->gdtMPN[i]);
      }
   }
   return VMK_FAILURE;
}

/*
 *----------------------------------------------------------------------
 *
 * WorldVmmASInit  --
 *
 *      Wire monitor page table for vmm worlds.
 *
 * Results:
 *      A VMK_ReturnStatus value.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
WorldVmmASInit(VMK_PDPTE *vmmPageRoot, MPN rootMPN, MPN *pdirMPNs)
{
   unsigned i;
   MA rootMA;
 
   VMK_PTE *mmuRootPT = NULL;
   World_Handle *world = MY_RUNNING_WORLD;
   static const int numMonPTs = VMM_NUM_PAGES/VMK_PTES_PER_PDE;

   ASSERT(World_IsVMMWorld(world));
   
   rootMA = MPN_2_MA(rootMPN);


   // Map PDIRs.
   for (i = 0; i < VMK_NUM_PDPTES; i++) {
      PT_SET(&vmmPageRoot[i], MAKE_PDPTE(pdirMPNs[i], 0, PDPTE_FLAGS));
   }
   
   // Setup monitor page tables (see vmx/public/addrlayout32.h)
   for (i = 0; i < numMonPTs; i++) {
      VMK_PDE *vmmPageDir = NULL;

      // Link up the monitor pagedirs to the monitor pagetables. 
      vmmPageDir = PT_GetPageDir(rootMA, 
                                 VMM_FIRST_LINEAR_ADDR + i * PDE_SIZE, NULL);
      ASSERT(vmmPageDir);
      if (vmmPageDir == NULL) {
         return VMK_FAILURE;
      }
      PT_SET(&vmmPageDir[ADDR_PDE_BITS(VMM_FIRST_LINEAR_ADDR + i * PDE_SIZE)],
             VMK_MAKE_PDE(world->pageTableMPNs[i], 0, PTE_KERNEL));
      PT_ReleasePageDir(vmmPageDir, NULL);
   }

   mmuRootPT = PT_GetPageTable(rootMA, VMM_FIRST_LINEAR_ADDR, NULL);
   if (mmuRootPT == NULL) {
      return VMK_FAILURE;
   }

   mmuRootPT[MMU_ROOT_START] = VMK_MAKE_PTE(rootMPN, 0, PTE_KERNEL);

   ASSERT(MMU_PAE_PAGE_DIR_START + VMK_NUM_PDPTES <= VMK_PTES_PER_PDE);
   for (i = 0; i < VMK_NUM_PDPTES; i++) {
      mmuRootPT[MMU_PAE_PAGE_DIR_START + i] = 
         VMK_MAKE_PTE(pdirMPNs[i], 0, PTE_KERNEL);
   }
   
   PT_ReleasePageTable(mmuRootPT, NULL);

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * WorldVmmASAllocMPNs  --
 *
 *      Allocate MPNs for montior pagetable.
 *
 * Results:
 *      A VMK_ReturnStatus value.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
WorldVmmASAllocMPNs(MPN *vmmRootMPN, MPN *pdirMPNs)
{
   int i;
   VMK_ReturnStatus retval;
   
   // Allocate PDPT.
   retval = Alloc_KernelAnonPage(MY_VMM_GROUP_LEADER, TRUE, vmmRootMPN);
   if (retval != VMK_OK) {
      return retval;
   }
   retval = Util_ZeroMPN(*vmmRootMPN);
   if (retval != VMK_OK) {
      return retval;
   }
   // Allocate and map PDIRs.
   for (i = 0; i < VMK_NUM_PDPTES; i++) {
      retval = Alloc_KernelAnonPage(MY_VMM_GROUP_LEADER, FALSE, pdirMPNs + i);
      if (retval != VMK_OK) {
         return retval;
      }
      retval = Util_ZeroMPN(pdirMPNs[i]);
      if (retval != VMK_OK) {
         return retval;
      }
   }
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * World_VmmASInit  --
 *
 *      Allocate and wire monitor page table for vmm worlds.
 *
 * Results:
 *      A VMK_ReturnStatus value.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMKERNEL_ENTRY
World_VmmASInit(DECLARE_1_ARG(VMK_VMM_AS_INIT, MA *, vmmCR3))
{
   VMK_ReturnStatus retval;
   KSEG_Pair *pair;
   MPN rootMPN;
   MPN pdirMPNs[VMK_NUM_PDPTES];
   VMK_PDPTE *vmmPageRoot = NULL;
   PROCESS_1_ARG(VMK_VMM_AS_INIT, MA *, vmmCR3);

   retval = WorldVmmASAllocMPNs(&rootMPN, pdirMPNs);
   if (retval == VMK_OK) {
      vmmPageRoot = Kseg_MapMPN(rootMPN, &pair);
      if (vmmPageRoot == NULL) {
         retval = VMK_FAILURE;
      } else {
         retval = WorldVmmASInit(vmmPageRoot, rootMPN, pdirMPNs);
         Kseg_ReleasePtr(pair);
      }
   }

   if (retval != VMK_OK) {
      World_Panic(MY_RUNNING_WORLD,
                  "Unable to allocate memory for monitor page tables\n");
   }

   *vmmCR3 = MPN_2_MA(rootMPN);
   return VMK_OK;
}

VMKERNEL_ENTRY 
World_InitMainMem(DECLARE_1_ARG(VMK_VMM_MAINMEM_INIT, MemHandle, 
                                mainMemHandle))
{
   World_Handle *world;
   PROCESS_1_ARG(VMK_VMM_MAINMEM_INIT, MemHandle, mainMemHandle);
   world = MY_RUNNING_WORLD;
   World_VMMGroup(world)->mainMemHandle = mainMemHandle;
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * WorldASCleanup --
 *
 *      Release all resources for this world's address space.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	The world's address space is freed.
 *
 *----------------------------------------------------------------------
 */
static void
WorldASCleanup(World_Handle *world)
{
   int i;

   if (world->pageRootMA != 0) {
      PT_FreePageRoot(world->pageRootMA);
      world->pageRootMA = 0;
   }

   for (i = 0; i < MON_PAGE_TABLE_LEN; i++) {
      if (world->pageTableMPNs[i] != 0) {
         MemMap_FreeKernelPage(world->pageTableMPNs[i]);
         world->pageTableMPNs[i] = 0;
      }
   }

   if (world->taskMPN != INVALID_MPN) {
      ASSERT(world->taskMPN != 0);
      MemMap_FreeKernelPage(world->taskMPN);
      world->taskMPN = INVALID_MPN;
   }
   if (world->nmiStackMPN != INVALID_MPN) {
      ASSERT(world->nmiStackMPN != 0);
      MemMap_FreeKernelPage(world->nmiStackMPN);
      world->nmiStackMPN = INVALID_MPN;
   }
   if (world->kernelGDT != NULL) {
      XMap_Unmap(GDT_AREA_LEN, world->kernelGDT);
   }
   if (world->nmiStackStart != (VA) NULL) {
      XMap_Unmap(1, (void*)world->nmiStackStart);
   }
   for (i=0; i < GDT_AREA_LEN; i++) {
      if (world->gdtMPN[i] != INVALID_MPN &&
          world->gdtMPN[i] != 0) {
	 MemMap_FreeKernelPage(world->gdtMPN[i]);
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * WorldClearTaskBusy --
 *
 *      Clear the busy bit in the current task so we will be able
 * 	to reload the task later.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static void
WorldClearTaskBusy(Bool fromHost)
{
   DTR32	gdtreg;
   uint16	trReg;      

   GET_GDT(gdtreg); 
   GET_TR(trReg);   
   if (trReg != 0) {
      if (fromHost) {
	 Descriptor desc;

         /*
          * can't use CopyFromHost (without the Int) because this function
          * is called when switching worlds and MY_RUNNING_WORLD has been
          * updated to the new world even though we're still running on the
          * host world.
          */
	 CopyFromHostInt(&desc, (void *)(gdtreg.offset + trReg), sizeof(desc));
	 if (Desc_Type(&desc) == TASK_DESC_BUSY) {
	    Desc_SetType(&desc, TASK_DESC);
	    CopyToHostInt((void *)(gdtreg.offset + trReg), &desc, sizeof(desc));
	 }
      } else {
	 Descriptor *descp = 
	    (Descriptor *)(VMK_LA_2_VA(gdtreg.offset) + trReg);
	 if (Desc_Type(descp) == TASK_DESC_BUSY) {
	    Desc_SetType(descp, TASK_DESC);
	 }
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * WorldSaveDebugRegisters --
 *
 *      Save the debug registers.  Since accessing the
 *      hardware registers is expensive, avoid it when
 *      possible. The monitor guarantees that <hardware DR i>
 *      == <shadow DR i>, but if we're switching from a non-VM
 *      world, we must read the value from the register.
 *
 *----------------------------------------------------------------------
 */

static INLINE void
WorldSaveDebugRegisters(World_Handle *restore,
                        World_Handle *save)
{
   if (World_IsVMMWorld(save)) {
      save->savedState.DR[0] = save->vmkSharedData->shadowDR[0];
      save->savedState.DR[1] = save->vmkSharedData->shadowDR[1];
      save->savedState.DR[2] = save->vmkSharedData->shadowDR[2];
      save->savedState.DR[3] = save->vmkSharedData->shadowDR[3];
      save->savedState.DR[6] = save->vmkSharedData->shadowDR[6];
      save->savedState.DR[7] = save->vmkSharedData->shadowDR[7];
   } else {
      GET_DR0(save->savedState.DR[0]);
      GET_DR1(save->savedState.DR[1]);
      GET_DR2(save->savedState.DR[2]);
      GET_DR3(save->savedState.DR[3]);
      GET_DR6(save->savedState.DR[6]);
      GET_DR7(save->savedState.DR[7]);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * WorldRestoreDebugRegisters --
 *
 *      Restore the debug registers.  See comment for
 *      WorldSaveDebugRegisters.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
WorldRestoreDebugRegisters(World_Handle *restore,
                           World_Handle *save)
{
   /*
    * Restore the debug registers.
    */
   
#define RESTOREDR(i) do {                                                    \
      if (restore->savedState.DR[i] != save->savedState.DR[i]) {             \
         SET_DR ## i(restore->savedState.DR[i]);                             \
      }                                                                      \
   } while (0)

   RESTOREDR(7);
   RESTOREDR(6);        // always restore because of ICEBP

   RESTOREDR(0);
   RESTOREDR(1);
   RESTOREDR(2);
   RESTOREDR(3);
}


#ifdef VMX86_STATS

/*
 *----------------------------------------------------------------------
 *
 * WorldDoSwitchStats --
 *    Accumulate switch statistics.
 *
 *----------------------------------------------------------------------
 */
static INLINE_SINGLE_CALLER void
WorldDoSwitchStats(World_Handle *restore,
                   World_Handle *save, 
                   TSCCycles diff)
{
   if (World_IsVMMWorld(restore)) {
      if (World_IsVMMWorld(save)) {
         myPRDA.switchStats.vmmToVMM += diff;
         myPRDA.switchStats.vmmToVMMCnt++;
      } else {
         myPRDA.switchStats.nvmmToVMM += diff;
         myPRDA.switchStats.nvmmToVMMCnt++;
      }
   } else {
      if (World_IsVMMWorld(save)) {
         myPRDA.switchStats.vmmToNVMM += diff;
         myPRDA.switchStats.vmmToNVMMCnt++;
      } else {
         myPRDA.switchStats.nvmmToNVMM += diff;
         myPRDA.switchStats.nvmmToNVMMCnt++;
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * WorldSwitchStatsWriteHandler --
 *    Proc write handler for /proc/switchStats. This is racy, but that's 
 *    fine.
 *
 * Results:
 *    VMK_OK.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------
 */
static int 
WorldSwitchStatsWriteHandler(Proc_Entry *entry,
                             char        *page,
                             int         *lenp)
{
   int i;
   for (i = 0; i < numPCPUs; i++) {
      SwitchStats *switchStats = &prdas[i]->switchStats;
      memset(switchStats, 0, sizeof (SwitchStats));
   }
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * WorldSwitchStatsReadHandler --
 *    Proc handler for /proc/switchStats. This is racy, but that's fine.
 *
 * Results:
 *      VMK_OK.
 *
 * Side effects:
 *      None.
 *
 * The data output represents the cummulative number of cycles and
 * cummulative number of instances, across all pcpus, of each of the
 * types of world switches which can occur:
 *
 *    vmm     <--> vmm
 *    vmm     <--> non-vmm
 *    non vmm <--> vmm
 *    non vmm <--> non-vmm
 *
 *----------------------------------------------------------------------
 */
static int 
WorldSwitchStatsReadHandler(Proc_Entry *entry,
                            char        *page,
                            int         *lenp)
{
   int i;
   uint64 vmmToVMMCnt, vmmToNVMMCnt, nvmmToVMMCnt, nvmmToNVMMCnt;
   uint64 vmmToVMM, vmmToNVMM, nvmmToVMM, nvmmToNVMM;

   *lenp = 0;
   for (i = 0; i < numPCPUs; i++) {
      SwitchStats *switchStats = &prdas[i]->switchStats;
      vmmToVMMCnt   = switchStats->vmmToVMMCnt;
      vmmToNVMMCnt  = switchStats->vmmToNVMMCnt;
      nvmmToVMMCnt  = switchStats->nvmmToVMMCnt;
      nvmmToNVMMCnt = switchStats->nvmmToNVMMCnt;
      vmmToVMM      = switchStats->vmmToVMM;
      vmmToNVMM     = switchStats->vmmToNVMM;
      nvmmToVMM     = switchStats->nvmmToVMM;
      nvmmToNVMM    = switchStats->nvmmToNVMM;
      if (vmmToVMMCnt != 0) {
         Proc_Printf(page, lenp, "PCPU%d:  VMM<-> VMM cycles %16Lu count %10Lu avg %Lu\n",
                     i, vmmToVMM, vmmToVMMCnt, vmmToVMM / vmmToVMMCnt);
      }
      if (vmmToNVMMCnt != 0) {
         Proc_Printf(page, lenp, "PCPU%d:  VMM<->NVMM cycles %16Lu count %10Lu avg %Lu\n",
                     i, vmmToNVMM, vmmToNVMMCnt, vmmToNVMM / vmmToNVMMCnt);
      }
      if (nvmmToVMMCnt != 0) {
         Proc_Printf(page, lenp, "PCPU%d: NVMM<-> VMM cycles %16Lu count %10Lu avg %Lu\n",
                     i, nvmmToVMM, nvmmToVMMCnt, nvmmToVMM / nvmmToVMMCnt);
      }
      if (nvmmToNVMMCnt != 0) {
         Proc_Printf(page, lenp, "PCPU%d: NVMM<->NVMM cycles %16Lu count %10Lu avg %Lu\n",
                     i, nvmmToNVMM, nvmmToNVMMCnt, nvmmToNVMM / nvmmToNVMMCnt);
      }
   }
   return VMK_OK;
}
#endif

/*
 * WorldSwitchKind: Indicates, to WorldDoSwitch, the type of world
 *                  switch to be performed.
 *
 *  The values chosen are very specific for the assembly code which
 *  has been written.  Do not alter these values without examining
 *  WorldDoSwitch() very carefully.
 */
typedef enum WorldSwitchKind {
   WSK_VmmToNvmm  = 0,          /* zero                                     */
   WSK_NvmmToVmm  = 1,          /* no sign & no parity (parity: low 8 bits) */
   WSK_VmmToVmm   = 3,          /* no sign &    parity (parity: low 8 bits) */
   WSK_NvmmToNvmm = 0x80000003, /*    sign &    parity (parity: low 8 bits) */
} WorldSwitchKind;

extern World_Handle *
WorldDoSwitch(World_Handle *restore, World_Handle *save, WorldSwitchKind kind)
     __attribute__((regparm(3)));

/*
 *----------------------------------------------------------------------
 *
 * World_Switch --
 *
 *      Switch to a new world saving the current worlds state.
 *
 * Results:
 *      The previous world.
 *
 * Side effects:
 *	The other world runs.
 *
 *----------------------------------------------------------------------
 */

World_Handle *
World_Switch(World_Handle *restore,  // IN: world state to switch to
             World_Handle *save)     // OUT: world state to switch from
{
   static const WorldSwitchKind switchKind[2][2] = {
      { WSK_NvmmToNvmm, WSK_NvmmToVmm }, /* nvmm -> { nvmm, vmm } */
      { WSK_VmmToNvmm,  WSK_VmmToVmm  }, /* vmm -> { nvmm, vmm }  */
   };
      
   /*
    * Exempt any null switches from these checks (we somtimes switch
    * to the current world for the debugger's purposes.)
    */
   if ((restore != MY_RUNNING_WORLD) || (save != MY_RUNNING_WORLD)) {
      ASSERT(World_IsSafeToDeschedule());
   }

   WorldClearTaskBusy(CpuSched_HostWorldCmp(save));

   Vmkperf_WorldSwitch(restore, save);

   STATS_ONLY(myPRDA.switchStats.switchBegin = RDTSC());
   WorldSaveDebugRegisters(restore, save);

   /*
    * Do the switch.
    */
   save = WorldDoSwitch(restore, save,
                        switchKind[(int)World_IsVMMWorld(save)]
                                  [(int)World_IsVMMWorld(restore)]);

   /*
    * We're in the context of the new world now, and the
    * values of all local variables have changed.  To
    * restore the meanings of SAVE and RESTORE from before
    * the switch (SAVE is the old world, RESTORE the new
    * (current) one) we make WorldDoSwitch return its third
    * argument (SAVE), and we get RESTORE from the global
    * context.  Otherwise SAVE would be the current world,
    * and RESTORE some undefined world.
    */

   restore = MY_RUNNING_WORLD;

   WorldRestoreDebugRegisters(restore, save);

   STATS_ONLY(WorldDoSwitchStats(save, restore, 
                                 RDTSC() - myPRDA.switchStats.switchBegin));

   /*
    * Don't enable the performance counters if the host is
    * running because we can't afford to take an NMI when
    * the host is trying to switch stacks when it calls us.
    * A better solution is to enable the performance counter
    * when we return to the host.
    */

   return save;
}

/*
 *----------------------------------------------------------------------
 *
 * World_VPN2MPN --
 *
 *	Return the MPN mapped at vpn, or INVALID_MPN if the pte is
 *	unmapped or is an apic-mapped region.
 *
 * Results:
 *      VMK_OK if success, otherwise if failure.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
World_VPN2MPN(World_Handle *world, VPN vpn, MPN* outMPN)
{
   VMK_PTE *pageTable;	
   VMK_PTE pte;
   int vpnPT, vpnPage;
   VMK_ReturnStatus status = VMK_OK;

   *outMPN = INVALID_MPN;

   ASSERT(world->inUse);
   vpnPT = vpn / VMK_PTES_PER_PDE;
   vpnPage = vpn % VMK_PTES_PER_PDE;

   ASSERT(vpnPT < MON_PAGE_TABLE_LEN);
   ASSERT(world->pageTableMPNs[vpnPT] != 0);

   pageTable = (VMK_PTE *)KVMap_MapMPN(world->pageTableMPNs[vpnPT], TLB_LOCALONLY);

   ASSERT(pageTable != NULL);

   pte = pageTable[vpnPage];

   KVMap_FreePages(pageTable);

   if (PTE_PRESENT(pte)) {
      uint32 mpnAPIC = MA_2_MPN(APIC_GetBaseMA());
      MPN mpn = VMK_PTE_2_MPN(pte);

      /* avoid copying from APIC-mapped region (would trigger error intrs) */
      if (mpn == mpnAPIC) {
         VMLOG(0, world->worldID, "copying zero page for APIC mpn=%x", mpn);
      } else {
         *outMPN = mpn;
      }
   }

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * World_AddPage --
 *
 *      Map the mpn into the given worlds page table at vpn.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	The world's page table is modified.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
World_AddPage(World_ID worldID, VPN vpn, MPN mpn, Bool readOnly)
{
   VMK_ReturnStatus status;
   World_Handle *world = World_Find(worldID);

   if (world == NULL) {
      return VMK_NOT_FOUND;
   }

   status = WorldAddPage2(world, vpn, mpn, readOnly, NULL);

   World_Release(world);

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * WorldMapVMMStack --
 *      
 *      Maps a VMM stack if all the stack pages have been allocated.
 *     
 * Results:
 *      None.
 *
 * Side effects:
 *      A stack may be mapped.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
WorldMapVMMStack(World_Handle *world, int stackNum)
{
   uint32 i;
   int numRanges = 0;
   KVMap_MPNRange range[WORLD_VMM_NUM_STACK_MPNS];
   // Make sure that we have allocated all the 
   // stack pages before we map it
   for (i = 0; i < WORLD_VMM_NUM_STACK_MPNS; i++) {
      if (World_VMM(world)->vmmStackInfo[stackNum].mpns[i] == INVALID_MPN) {
         return;
      }
      range[i].startMPN = World_VMM(world)->vmmStackInfo[stackNum].mpns[i];
      range[i].numMPNs = 1;
      numRanges++;
   }
   if (World_VMM(world)->vmmStackInfo[stackNum].mappedStack != NULL) {
      KVMap_FreePages(World_VMM(world)->vmmStackInfo[stackNum].mappedStack);
   }
   World_VMM(world)->vmmStackInfo[stackNum].mappedStack = 
      KVMap_MapMPNs(numRanges, range, numRanges, TLB_LOCALONLY);
   if (World_VMM(world)->vmmStackInfo[stackNum].mappedStack == NULL) {
      VmWarn(world->worldID, "Couldn't map stack");
   }
}

/*
 *----------------------------------------------------------------------
 *
 * World_GetVMKStackPage --
 *
 *      Return a pointer to the data in the Nth vmkernel stack page in the
 *	given world.  Meaningless if world is running.
 *
 * Results:
 *      VMK_OK if success, otherwise if failure.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
World_GetVMKStackPage(World_Handle *world, int pageNum, VA *va)
{
   if (pageNum < 0) {
      return VMK_BAD_PARAM;
   }

   if (pageNum >= World_GetVMKStackLength(world) / PAGE_SIZE) {
      return VMK_LIMIT_EXCEEDED;
   }

   *va = World_GetVMKStackBase(world) + (pageNum * PAGE_SIZE);
   VMLOG(0, world->worldID, "pageNum: %d: va %x", pageNum, *va);

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * WorldAddPage2 --
 *
 *      Map the mpn into the given world's page table at vpn.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	The world's page table is modified.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
WorldAddPage2(World_Handle *world, VPN vpn, MPN mpnToAdd, Bool readOnly, MPN *outMPN)
{
   int i;
   VMK_PTE *pageTable;	
   int vpnPT, vpnPage;
   uint32 flags;
   World_VmmInfo *vmm = World_VMM(world);

   ASSERT(World_IsVMMWorld(world));

   ASSERT(mpnToAdd != INVALID_MPN);

   vpnPT = vpn / VMK_PTES_PER_PDE;
   vpnPage = vpn % VMK_PTES_PER_PDE;

   ASSERT(vpnPT < MON_PAGE_TABLE_LEN);
   ASSERT(world->pageTableMPNs[vpnPT] != 0);

   pageTable = (VMK_PTE *)KVMap_MapMPN(world->pageTableMPNs[vpnPT], TLB_LOCALONLY);
   ASSERT(pageTable != NULL);

   if ((pageTable[vpnPage] != 0) &&
       (pageTable[vpnPage] != VMK_MAKE_PTE(mpnToAdd, 0, PTE_MON_PAGE))) {
      Warning("vpn %x added twice with different mpn %x", vpn, mpnToAdd);
      KVMap_FreePages(pageTable);
      if (outMPN != NULL) {
	 *outMPN = INVALID_MPN;
      }
      return VMK_BAD_PARAM;
   }
   flags = PTE_MON_PAGE;
   if (readOnly) {
      flags &= ~PTE_RW;
   }
   PT_SET(&pageTable[vpnPage], VMK_MAKE_PTE(mpnToAdd, 0, flags));

   if (outMPN != NULL) {
      *outMPN = VMK_PTE_2_MPN(pageTable[vpnPage]);
   }

   KVMap_FreePages(pageTable);

   for (i = 0; i < WORLD_VMM_NUM_STACKS; i++) {
      if (vpn >= VA_2_VPN(vmm->vmmStackInfo[i].stackBase) &&
	  vpn < VA_2_VPN(vmm->vmmStackInfo[i].stackTop)) {
	 int stackPage = vpn - VA_2_VPN(vmm->vmmStackInfo[i].stackBase);
	 vmm->vmmStackInfo[i].mpns[stackPage] = mpnToAdd;
	 if (KVMap_NumEntriesFree() >= CONFIG_OPTION(KVMAP_ENTRIES_LOW)) {
	    WorldMapVMMStack(world, i);
	 }
	 break;
      }
   }

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * World_DumpPTE --
 *
 *      Print out the PTE at the given address.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
void 
World_DumpPTE(VA vaddr)
{
   World_Handle *world;
   VMK_PTE *pageTable;

   world = MY_RUNNING_WORLD;

   pageTable = PT_GetPageTable(world->pageRootMA, vaddr, NULL);
   if (pageTable == NULL) {
      Warning("couldn't find pagetable for %x", vaddr);
      return;
   }

   Warning("PTE @ 0x%x = 0x%"FMTPT"x", vaddr, pageTable[ADDR_PTE_BITS(vaddr)]);

   PT_ReleasePageTable(pageTable, NULL);
}

/*
 *----------------------------------------------------------------------
 *
 * World_ReadRegs --
 *
 *      Return the contents of the registers of the given world if
 *	it has been switched out.  If the world is running then this 
 *	call is meaningless. Called during coredump.
 *
 * Results:
 *      VMK_OK is success, non-zero if failure.
 *
 * Side effects:
 *	*regs is filled in.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
World_ReadRegs(World_ID worldID, VMnix_ReadRegsResult *regs) 
{
   World_Handle *world = World_Find(worldID);
   if (world == NULL) {
      return VMK_FAILURE;
   }

   ASSERT(World_IsVMMWorld(world) && 
          (!World_VMM(world)->inVMMPanic || world->okToReadRegs));

   regs->ebx = world->savedState.regs[REG_EBX];
   regs->ecx = world->savedState.regs[REG_ECX];
   regs->edx = world->savedState.regs[REG_EDX];
   regs->esi = world->savedState.regs[REG_ESI];
   regs->edi = world->savedState.regs[REG_EDI];
   if (World_VMM(world)->vmmCoreDumpEBP) {
      regs->ebp = World_VMM(world)->vmmCoreDumpEBP;
      regs->esp = World_VMM(world)->vmmCoreDumpESP;
      regs->eip = World_VMM(world)->vmmCoreDumpEIP;
   } else {
      regs->ebp = world->savedState.regs[REG_EBP];
      regs->esp = world->savedState.regs[REG_ESP];
      regs->eip = world->savedState.eip;
   }
   regs->eax = world->savedState.regs[REG_EAX];
   regs->cs = world->savedState.segRegs[SEG_CS];
   regs->ds = world->savedState.segRegs[SEG_DS];
   regs->es = world->savedState.segRegs[SEG_ES];
   regs->ss = world->savedState.segRegs[SEG_SS];
   regs->eflags = world->savedState.eflags;

   World_Release(world);

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * World_Exit --
 *
 *      Handle a request for the current world to exit.  
 *      This function will deschedule the world right away, 
 *      no matter what.  A reap callback is scheduled to cleanup
 *      the world. 
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	The world's exit status may be set.
 *
 *----------------------------------------------------------------------
 */
void
World_Exit(VMK_ReturnStatus status)
{
   uint32 flags;
   World_Handle *world = MY_RUNNING_WORLD;   

   ASSERT(World_IsSafeToBlock());
   ASSERT(List_IsEmpty(&world->heldSemaphores));

   SAVE_FLAGS(flags);

   world->exitStatus = status;

   VMLOG(0, world->worldID, "Killing self with interrupts %s.  Status=%#x:%s", 
         (flags & EFLAGS_IF) ? "enabled" : "disabled",
         status, VMK_ReturnStatusToString(status));

   /*
    * If interrupts are disabled, we may have taken an interrupt but may not
    * have handled it yet.
    */
   if (!(flags & EFLAGS_IF)) {
      IDT_CheckInterrupt();
   }

   /* 
    * It is possible that the first reap callback happens before 
    * CpuSched_Die() gets a chance to complete [esp. when the cpusched
    * lock is contended].  WorldReap() handles this case by just
    * checking again in 1 second.
    */
   WorldScheduleReap(world->worldID, TRUE);  
   CpuSched_Die();
   NOT_REACHED();
}

/*
 *----------------------------------------------------------------------
 *
 * WorldMiscInit --
 *
 *      Stuff that doesn't really fit anywhere else.
 *
 * Results:
 *      A VMK_ReturnStatus value.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
WorldMiscInit(World_Handle *world, UNUSED_PARAM(World_InitArgs *args))
{
   world->nmisInMonitor = FALSE;

   /*
    * It is only unsafe to read the registers once the world
    * has started running.  (note this is mainly a work around
    * for a calling World_Panic() on a group member before the
    * other words have been made runnable.)
    */
   world->okToReadRegs = TRUE;

   List_Init(&world->heldSemaphores);

   Semaphore_Init("Select sema", &world->selectSema, 0, SEMA_RANK_UNRANKED);
   
   World_CpuSchedRunStateInit(world); 

   // start worlds with preemption disabled
   world->preemptionDisabled = TRUE;

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * WorldMiscCleanup --
 *
 *      Free all of the world's state.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
WorldMiscCleanup(World_Handle *world)
{   
   Trace_RecentWorldDeath(world);
   Semaphore_Cleanup(&world->selectSema);
}


/*
 *----------------------------------------------------------------------
 *
 * WorldVMMInit --
 *
 *      Initialize per-vmm world datastructures.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
WorldVMMInit(World_Handle *world, World_InitArgs *args)
{   
   ASSERT(World_IsVMMWorld(world));
   world->vmm = World_Alloc(world, sizeof(World_VmmInfo));
   memset(world->vmm, 0, sizeof(World_VmmInfo));
   world->vmm->vcpuid = args->vcpuid;
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * WorldVMMCleanup --
 *
 *      Cleanup per-vmm datastructures.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
WorldVMMCleanup(World_Handle *world)
{   
   ASSERT(World_IsVMMWorld(world));
   World_Free(world, world->vmm);
}

/*
 *----------------------------------------------------------------------
 *
 * WorldReleaseOptReaderLock --
 *
 *      Decrement the reference count on this world.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	The world's ref count and possibly readerCount are decremented.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
WorldReleaseOptReaderLock(World_Handle *world, Bool readLocked)
{   
   SP_IRQL prevIRQL;

   prevIRQL = WorldLock();

   if (readLocked) {
      ASSERT(world->readerCount > 0);
      world->readerCount--;
   } else {
      ASSERT(world->refCount > 0);
      world->refCount--;
   }

   WorldUnlock(prevIRQL);
}

/*
 *----------------------------------------------------------------------
 *
 * World_Connect --
 *
 *      Open up an RPC connection for this world.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	The world's connection list may be modified.
 *
 *----------------------------------------------------------------------
 */
VMKERNEL_ENTRY
World_Connect(DECLARE_2_ARGS(VMK_RPC_CONNECT, char *, name, RPC_Connection *, cnxID))
{
   VMK_ReturnStatus status;
   PROCESS_2_ARGS(VMK_RPC_CONNECT, char *, name, RPC_Connection *, cnxID);

   status = RPC_Connect(name, cnxID);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * WorldUserWorldStart --
 *
 *	Start the given UserWorld.
 *
 * Results:
 *      Does not return
 *
 * Side effects:
 *	startFunc will be run
 *
 *----------------------------------------------------------------------
 */
static NORETURN void
WorldUserWorldStart(void* startFunc)
{
   ASSERT(World_IsUSERWorld(MY_RUNNING_WORLD));
   User_WorldStart(MY_RUNNING_WORLD, startFunc);
   /* User_WorldStart does not return */
}


/*
 *----------------------------------------------------------------------
 *
 * World_MakeRunnable --
 *
 *      Make this world runnable.  It will be added to the scheduler
 *      and will start with the given startFunc(startArg).
 *
 * Results:
 *      VMK_OK if the world was succesfully added.  Error if this
 *	world is already known to the scheduler or was not found.
 *
 * Side effects:
 *	A new world can run free.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
World_MakeRunnable(World_ID worldID,
                   void (*startFunc)(void *startArg))
{
   VMK_ReturnStatus status;
   World_Handle *world;
   void* startArg = NULL;

   world = World_Find(worldID);

   if (world) {
#ifndef ESX3_NETWORKING_NOT_DONE_YET
#error "nuke toe or fix it"
      Net_AllowTOE(world, config->toeEnabled);
#endif // ESX3_NETWORKING_NOT_DONE_YET

      VMLOG(1, MY_RUNNING_WORLD->worldID,
            "worldID=%d, startFunc=%p startArg=%p",
            worldID, startFunc, startArg);

      if (World_CpuSchedRunState(world) == CPUSCHED_NEW) {
         /*
          * UserWorlds start in WorldUserWorldStart, and then jump into the
          * user-provided function.
          */
         if (World_IsUSERWorld(world)) {
            startArg = startFunc;
            startFunc = WorldUserWorldStart;
         }      
         status = Sched_Add(world, startFunc, startArg);
      } else {
	 VmWarn(worldID, "non-NEW state=%d", World_CpuSchedRunState(world));
	 status = VMK_BUSY;
      }
      World_Release(world);
   } else {
      status = VMK_NOT_FOUND;
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * WorldReapCallback --
 *
 *      Schedules a WorldReap helper request if necessary.
 *      [Sometimes both the monitor & userlevel decide to destroy
 *      the world at the same time.  In this case two reap call
 *      backs would be scheduled -- and havoc ensue.  The reapScheduled,
 *      flag guarantees that WorldReap only gets called once.
 *      
 * Results:
 *      None.
 *
 * Side effects:
 *	A world will get reaped.
 *
 *----------------------------------------------------------------------
 */
static void
WorldReapCallback(void *data, UNUSED_PARAM(Timer_AbsCycles timestamp))
{
   World_ID worldID = (World_ID) data;
   SP_IRQL prevIRQL;
   VMK_ReturnStatus status;
   World_Handle *world;

   prevIRQL = WorldLock();
   world = WorldFind(worldID);
   if ((world == NULL) || world->reapScheduled) {
      WorldUnlock(prevIRQL);
      VMLOG(0, worldID, "world already reaped or scheduled for reaping.");
      return;
   }
   world->reapScheduled = TRUE;
   WorldUnlock(prevIRQL);

   VMLOG(1, worldID, "scheduling reap callback.");
   status = Helper_Request(HELPER_MISC_QUEUE, WorldReap, world);

   // If we are out of helper requests, try later
   if (status != VMK_OK) {
      prevIRQL = WorldLock();
      ASSERT(WorldFind(worldID) == NULL);
      world->reapScheduled = FALSE;
      WorldUnlock(prevIRQL);

      VmWarn(worldID, "Out of helper handles, scheduling reap");
      WorldScheduleReap(worldID, FALSE);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * WorldScheduleReap --
 *
 *      Schedule the a reap call back for world.  Uses a timer
 *      callback so that this function can be called from any context
 *      (can't make a helper request when interrupts are disabled).
 *      If this is the first reap schedule, use an immediate timer
 *      callback, but we're calling here again because someone still has a
 *      reference to this world, wait a bit first...
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
void
WorldScheduleReap(World_ID worldID, Bool firstTime)
{
   Timer_Add(MY_PCPU, WorldReapCallback, 
             firstTime ? 0 : REAP_RETRY_TIME,
             TIMER_ONE_SHOT, (void *) worldID);
}


/*
 *----------------------------------------------------------------------
 *
 * WorldPreCleanup --
 *
 *      This world is about to go away, so call the pre cleanup handlers
 *      for this world.
 *      
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
WorldPreCleanup(World_Handle *world)
{
   const PreCleanupTable *m;

   for (m = preCleanupTable; m < preCleanupTable + TABLE_SIZE(preCleanupTable); m++) {
      ASSERT(m->fn);

      VMLOG(1, world->worldID, "Starting %s", m->name);
      m->fn(world);
   }
}



/*
 *----------------------------------------------------------------------
 *
 * WorldReap --
 *
 *      Function executed in a helper world to finish destruction of a 
 *      descheduled world.  This function checks to see if the world
 *      can be cleaned up (not running, no readers, no host count,
 *      no active scsi handles), if so so, the data associated with
 *      the world  handle in cleaned up.  If it isn't safe to cleanup
 *      the world, the reap is attempted again in 1 second.
 *      
 * Results:
 *      Possbily cleans up the data associated with a world handle.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
WorldReap(void *data)
{
   World_Handle *world = (World_Handle *) data;
   SP_IRQL prevIRQL;
   Bool scsiActive;
   World_ID worldID = world->worldID;

   prevIRQL = WorldLock();
   ASSERT(WorldFind(worldID) == world);

   world->reapCalls++;

   // if first attempt at reaping, call all the PreCleanup functions.
   if (world->reapCalls == 1) {
      /*
       * We have to release the world lock before calling PreCleanup
       * functions due to lock ordering.  It's OK to release/reacquire the
       * lock here because reapScheduled variable guarantees only one
       * thread can be executing this piece of code.
       */
      WorldUnlock(prevIRQL);
      WorldPreCleanup(world);
      prevIRQL = WorldLock();
      ASSERT(WorldFind(worldID) == world);
   }

   scsiActive = (world->reapCalls < SCSI_REAP_RETRIES) && 
                SCSI_ActiveHandles(worldID);

   if ((world->readerCount > 0) || (world->hostCount > 0) || 
       scsiActive || (World_CpuSchedRunState(world) != CPUSCHED_ZOMBIE)) {
      // log the first two times we try to reap, plus every 256th (~4min) thereafter
      LOG_ONLY(int logLevel = (world->reapCalls < 4) ? 0
               : (((world->reapCalls % 256) ==0) ? 0 : 1));
      VMLOG(logLevel,
            worldID, "reapCount = %d, readers = %d, hostCount = %d, scsiActive =  %d",
            world->reapCalls, world->readerCount, world->hostCount, scsiActive);
      world->reapScheduled = FALSE;
      WorldUnlock(prevIRQL);

      /*
       * Delay reaping world if readerCount or hostCount is non-zero
       * or if there are still outstanding scsi requests.  
       * Try again in 1 second.
       */
      WorldScheduleReap(worldID, FALSE);
   } else {
      world->reapStarted = TRUE;
      if (World_IsVMMWorld(world) && World_VMM(world)->inVMMPanic && 
          Atomic_Read(&World_VMMGroup(world)->panicState) != 
          WORLD_GROUP_PANIC_VMXPOST) {
         VmWarn(worldID, "world panicked, but isn't in state VMXPOST, "
                "vmm core may be absent or corrupted.");
      }
      WorldUnlock(prevIRQL);
      WorldCleanup(world); 
   }
}


/*
 * POST stuff 
 */

/*
 * Can increase this value up to 1000, but takes a long time...
 * Also, you may have to disable cpusched proc nodes to avoid proc limit
 */
#define NUM_TEST_WORLDS 50

static SP_Barrier worldPostBarrier;
static Atomic_uint32 successCount;

/*
 *----------------------------------------------------------------------
 *
 * worldPOSTfn
 *
 * 	Dummy  function for the test worlds.  Spins and waits to 
 * 	be destroyed.
 *
 * Results:
 * 	Increments successCount if no errors are detected.
 *
 * Side effects:
 * 	none
 *
 *----------------------------------------------------------------------
 */

static void 
worldPOSTfn(UNUSED_PARAM(void *data))
{
   Bool success=TRUE;

   CpuSched_DisablePreemption();
   ENABLE_INTERRUPTS();

   if (success) {
      Atomic_Inc(&successCount);
   }

   SP_SpinBarrier(&worldPostBarrier);

   //Spin slowly
   while(!MY_RUNNING_WORLD->deathPending){
      CpuSched_Sleep(1000);
   }

   World_Exit(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * WorldCreateTest
 *
 * 	Helper function for WorldPost.  Create numWorlds worlds that belong
 * 	to the same scheduler group.
 *
 * Results:
 * 	FALSE if unable to create all the worlds, TRUE otherwise
 *      The world IDs of all the newly created worlds
 *      The scheduler group ID for the newly created scheduler group
 *
 * Side effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
static Bool 
WorldCreateTest(int numWorlds, World_Handle **testWorlds, Sched_GroupID *schedGroupID)
{
   int i;
   char name[30];
   char schedGroupName[SCHED_GROUP_NAME_LEN];
   Sched_GroupID parentID;
   Sched_Alloc groupAlloc;
   VMK_ReturnStatus status;

   // create the scheduler group
   snprintf(schedGroupName, sizeof(schedGroupName), "test_%d", MY_PCPU);
   parentID = Sched_GroupNameToID(SCHED_GROUP_NAME_SYSTEM);
   status = Sched_AddGroup(schedGroupName, parentID, schedGroupID);
   if (status != VMK_OK) {
      Warning("failed to create group (status=%x)", status);
      return FALSE;
   }

   // give the scheduler group enough shares
   groupAlloc.min = 0;
   groupAlloc.max = CPUSCHED_ALLOC_MAX_NONE;
   groupAlloc.units = SCHED_UNITS_PERCENT;
   groupAlloc.shares = MIN(CPUSCHED_SHARES_NORMAL(numWorlds), CPUSCHED_SHARES_MAX);
   status = CpuSched_GroupSetAlloc(*schedGroupID, &groupAlloc);
   if (status != VMK_OK) {
      Warning("failed to set group alloc (status=%x)", status);
      return FALSE;
   }

   // now create the worlds
   for(i = 0; i < numWorlds; i++) {
      Sched_ClientConfig sched;
      World_InitArgs args;

      snprintf(name, sizeof name, "test_%d_%d", MY_PCPU, i);
      Sched_ConfigInit(&sched, schedGroupName);
      World_ConfigArgs(&args, name, WORLD_SYSTEM | WORLD_POST, WORLD_GROUP_DEFAULT, &sched);

      if (World_New(&args, &testWorlds[i]) != VMK_OK) {
	 Warning("Could only create %d of %d worlds", i, numWorlds);
	 return FALSE;
      }

      // configure cpu info
      if (Sched_Add(testWorlds[i], worldPOSTfn, NULL) != VMK_OK) {
	 Warning("Could only create %d of %d worlds", i, numWorlds);
         return FALSE;
      }

      /*
       * Need to slow down a bit because proc node creation can't handle
       * too many proc nodes at once.
       */
      if ((i % 10) == 0) {
         CpuSched_Sleep(1);
      }
   }
   return TRUE;
}

/*
 *----------------------------------------------------------------------
 *
 * WorldPOST
 *
 *      Perform a power on test of World creation and deletion.
 *
 * Results:
 *      FALSE if error detected, TRUE otherwise
 *
 * Side effects:
 *
 *----------------------------------------------------------------------
 */

Bool
WorldPOST(UNUSED_PARAM(void *clientData),
          int id,
          UNUSED_PARAM(SP_SpinLock *lock),
          SP_Barrier *barrier)
{
   int i;
   Bool success = TRUE;
   World_Handle **testWorlds;
   int numWorldsPerCPU = NUM_TEST_WORLDS/numPCPUs;
   static volatile Bool worldCreateSucceeded = TRUE;
   Sched_GroupID schedGroupID = SCHED_GROUP_ID_INVALID;

   // sanity check (non-preemptible set by top-level postFn)
   ASSERT(!CpuSched_IsPreemptible());

   if (id == 0) {
      SP_InitBarrier("world POST Barrier", 
                     (numWorldsPerCPU + 1)*numPCPUs,
                     &worldPostBarrier); 
      Atomic_Write(&successCount, 0);
   }

   SP_SpinBarrier(barrier);

   testWorlds = (World_Handle **)Mem_Alloc(numWorldsPerCPU * sizeof testWorlds[0]);
   ASSERT_NOT_IMPLEMENTED(testWorlds != NULL);
   memset(testWorlds, 0, numWorldsPerCPU * sizeof testWorlds[0]);

   if (!WorldCreateTest(numWorldsPerCPU, testWorlds, &schedGroupID)) {
      worldCreateSucceeded = FALSE;
   }

   SP_SpinBarrier(barrier);

   if (worldCreateSucceeded) {

      //Wait for test worlds to complete
      SP_SpinBarrier(&worldPostBarrier);

      success = (Atomic_Read(&successCount) == numWorldsPerCPU*numPCPUs);
   } else {
      Warning("smashing world post barrier");
      SP_SmashBarrier(&worldPostBarrier);
      success = FALSE;
   }

   //Check WorldFind
   for (i = 0; i < numWorldsPerCPU; i++){
      if (testWorlds[i] == NULL) {
         break;
      }
      if (WorldFind(testWorlds[i]->worldID) != testWorlds[i]) {
         success = FALSE;
         Warning("Failed to find world %d",testWorlds[i]->worldID);
      }
   }

   //Check World_Destroy
   for (i = 0; i < numWorldsPerCPU; i++) {
      VMK_ReturnStatus status;
      if (testWorlds[i] == NULL) {
         break;
      }
      status = World_Destroy(testWorlds[i]->worldID, FALSE);
      if (status != VMK_OK) {
	 success = FALSE;
	 Warning("Could not destroy test world #%d: %s",
                 i, VMK_ReturnStatusToString(status));
      }
      /*
       * Need to slow down a bit because we world destruction needs timers
       * and helper queues, and they have limits.
       */
      if ((i % 10) == 0) {
         CpuSched_Sleep(1);
         CpuSched_YieldThrottled();
      }
   } 

   /* 
    * World destruction is asynchronous -- sleep for a while and hope
    * for the best
    */

   CpuSched_Sleep(3000 + 300 * numWorldsPerCPU);

   for (i = 0; i < numWorldsPerCPU; i++) {
      if (testWorlds[i] == NULL) {
         break;
      }
      if (WorldFind(testWorlds[i]->worldID) != NULL) {
	 success = FALSE;
	 Warning("Found destroyed world %d", testWorlds[i]->worldID);
      }
   } 

   if (schedGroupID != SCHED_GROUP_ID_INVALID) {
      Sched_RemoveGroup(schedGroupID);
   }

   return success;
}


/*
 *----------------------------------------------------------------------
 *
 * World_VcpuidToWorldID --
 *      
 *      Returns the worldID corresponding to the given vcpuid. The 
 *      "world" parmeter sets the context for the vcpuid.
 *      i.e. the group of worlds to which this vcpuid refers to.
 *
 * Results:
 *      0, on failure
 *      WorldID of the given vcpuid on success.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
World_ID
World_VcpuidToWorldID(World_Handle *world, Vcpuid vcpuid)
{
   ASSERT(world->group != NULL);

   ASSERT(vcpuid >= 0 && vcpuid < World_VMMGroup(world)->memberCount);

   return World_VMMGroup(world)->members[vcpuid];
}


/*
 *----------------------------------------------------------------------
 *
 * World_ResetDefaultDT --
 *
 *      Reset the descriptor tables and descriptors for this world to 
 *	be the default ones instead of whatever it is using.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	The descriptor table and descriptors are reset to the defaults.
 *
 *----------------------------------------------------------------------
 */
void
World_ResetDefaultDT(void)
{
   DTR32 IDTR;
   uint32 TR;
   DTR32 GDTR;
   uint32 ds;
   uint32 cs;
   Reg32 eflags;

   SAVE_FLAGS(eflags);
   CLEAR_INTERRUPTS();

   /*
    * Switch to the default GDT.
    */
   ds = DEFAULT_DS;
   cs = DEFAULT_CS;
   GDTR.limit = sizeof(defaultGDT) - 1;
   if (!World_IsHOSTWorld(MY_RUNNING_WORLD)) {
      GDTR.offset = VMK_VA_2_LA((VA)MY_RUNNING_WORLD->kernelGDT);
      Desc_SetDescriptor(&MY_RUNNING_WORLD->kernelGDT[DEFAULT_TSS_DESC],
			 TASK_BASE, DEFAULT_TASK_SIZE - 1, TASK_DESC,
			 0, 0, 1, 1, 0);

   } else {
      GDTR.offset = VMK_VA_2_LA((VA)defaultGDT);
      Desc_SetDescriptor(&defaultGDT[DEFAULT_TSS_DESC],
			 TASK_BASE, DEFAULT_TASK_SIZE - 1, TASK_DESC,
			 0, 0, 1, 1, 0);

   }
   SET_GDT(GDTR);
   __asm__ volatile("" : : "a" (ds), "b" (cs));
   __asm__ volatile("movl %eax, %ss\n\t"
		    "movl %eax, %ds\n\t"
		    "movl %eax, %es\n\t"
		    "movl %eax, %fs\n\t"		    
		    "movl %eax, %gs\n\t"		    
		    "pushl %ebx\n\t"
		    "pushl $GDTInitL\n\t"
		    "lret\n\t"
		    "GDTInitL:\n\t");

   /* Switch to the default task segment. */
   TR = MAKE_SELECTOR(DEFAULT_TSS_DESC, 0, 0);    
   SET_TR(TR);

   /* Switch to the default IDT. */
   IDT_GetDefaultIDT(&IDTR);   
   SET_IDT(IDTR);
   RESTORE_FLAGS(eflags);
}

/*
 *----------------------------------------------------------------------
 *
 * World_WatchpointsChanged --
 *
 *      Mark all worlds to indicate that watchpoints have changed.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	The watchpoint state for each world is changed.
 *
 *----------------------------------------------------------------------
 */
void
World_WatchpointsChanged(void)
{
   int i;

   for (i = 0; i < WORLD_TABLE_LENGTH; i++) {
      worlds[i].watchpointState.changed = TRUE;
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * World_CreateKernelThread
 *
 *      Creates a thread of execution for a device driver.
 *
 * Results:
 *      TRUE on success, false otherwise.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
Bool 
World_CreateKernelThread(CpuSched_StartFunc fn, void *clientData) 
{
   World_Handle *world;
   Sched_ClientConfig sched;
   World_InitArgs args;

   Sched_ConfigInit(&sched, SCHED_GROUP_NAME_DRIVERS);
   World_ConfigArgs(&args, "driver", WORLD_SYSTEM, WORLD_GROUP_DEFAULT, &sched);

   if (World_New(&args, &world) != VMK_OK) {
      Warning("Couldn't create world");
      return FALSE;
   }

   Sched_Add(world, fn, clientData);

   return TRUE;
}

/*
 *-----------------------------------------------------------------------------
 *
 * World_GetID
 *
 *      Intended for use in driver code where the World_Handle in abstract
 *      type. 
 *
 * Results:
 *      Returns the worldID field of the given world.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
World_ID 
World_GetID(World_Handle *world)
{
   return world->worldID;
}

/*
 *-----------------------------------------------------------------------------
 *
 * World_SelectWakeup
 *
 *      Wakeup the given world that may be waiting on its select
 *	semaphore.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	A world may be awakened.
 *
 *-----------------------------------------------------------------------------
 */
void
World_SelectWakeup(World_ID worldID)
{
   World_Handle *world = World_Find(worldID);

   if (world != NULL) {
      VMLOG(10, MY_RUNNING_WORLD->worldID, 
            "Waking up sleeper %d", worldID);
      if (World_IsUSERWorld(world)) {
         User_Wakeup(world->worldID);
      } else {
         Semaphore_Unlock(&world->selectSema);
      }
      World_Release(world);
   } else {
      LOG(0, "couldn't find world %d", worldID);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * World_SelectBlock
 *
 *      Block the current world on select semaphore.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */
void
World_SelectBlock(void)
{
   /*
    * user worlds use UserObj_Poll insetad of select.  World_SelectWakeup
    * relies on the fact that userworlds don't use selectblock.
    */
   ASSERT(!World_IsUSERWorld(MY_RUNNING_WORLD));
   Semaphore_Lock(&MY_RUNNING_WORLD->selectSema);
}

#define STACK_MAGIC_COOKIE 0x49471296

/*
 *-----------------------------------------------------------------------------
 *
 * WorldSetupStackMagic --
 *
 *      Writes our magic cookie to every word in this world's stack (below our
 *      current stack address). See World_CheckStack for full details.
 *
 * Results:
 *      Returns void.
 *
 * Side effects:
 *	 
 *      World's stack is updated.
 *
 *-----------------------------------------------------------------------------
 */
static void
WorldSetupStackMagic(int* stackPos, int* stackStart)
{
   int* curPos;
   for (curPos = stackStart; curPos < stackPos - 40; curPos++) {
      *curPos = STACK_MAGIC_COOKIE;
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * World_CheckStack --
 *
 *      Checks to see whether we've come close to overflowing this world's stack.
 *      This uses the monitor approach of writing a magic cookie to every word in 
 *      our stack space. When CheckStack is called, it searches for the highest
 *      stack address that still contains the cookie, which is the "high water 
 *      mark" for the stack (because normal stack growth will overwrite the cookie).
 *      This function prints a warning if we hit a new stack depth maximum.
 *      The function also ASSERTs that we have at least "minStack" bytes of stack space left.
 *      By default, this function is not used anywhere. If you need to debug a stack
 *      problem, just insert a call to this function somewhere in a commonly-used path
 *      in your code.
 *
 * Results:
 *      Returns void.
 *
 * Side effects:
 *	
 *      May print a warning if stack space low. May hit ASSERT if space really low.
 *      If a non-vmm world has not had its stack initialized with the cookie,
 *      this will call the initialization routine. For vmm worlds, we let the
 *      monitor set up the magic stack info.
 *
 *-----------------------------------------------------------------------------
 */
void
World_CheckStack(World_Handle* world, int minStackRemaining)
{
   int* ptr;
   int* stackPos = (int *)&ptr;
   int* stackStart;

   stackStart = 0;
   if (!vmkernelLoaded) { 
      return;
   }

   if (World_IsHOSTWorld(world)) {
      stackStart = (int*)(VMK_HOST_STACK_BASE);
   } else if ((VA)stackPos >= World_GetVMKStackBase(world) &&
	      (VA)stackPos < World_GetVMKStackTop(world)) {
      stackStart = (int *)World_GetVMKStackBase(world);
   } else {
      // uh-oh, we have NO IDEA where this stack is... this shouldn't happen
      ASSERT(0);
   }

   ptr = (int*)stackStart;

   if (world->minStackLeft == 0 && *ptr != STACK_MAGIC_COOKIE) {
      // didn't find any magic cookie, because this stack not yet checked
      WorldSetupStackMagic(stackPos, stackStart);
   }
   
   while (*ptr == STACK_MAGIC_COOKIE) {
      ptr++;
   }

   if (world->minStackLeft == 0 ||
       (ptr - stackStart) * sizeof(int) < world->minStackLeft) {
      world->minStackLeft = (ptr - stackStart) * sizeof(int);
      LOG(2, "New stack minimum: %d bytes remaining, world:%d",
              world->minStackLeft, world->worldID);

   }

   ASSERT(world->minStackLeft > minStackRemaining);
}

/*
 *-----------------------------------------------------------------------------
 *
 * World_SetDefaultGDTEntry --
 *
 *      Set an entry in the default GDT.  This should only be called during
 *      Init phase because it won't change the GDT of any currently running 
 *      worlds
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	The defaultGDT is modified.
 *
 *-----------------------------------------------------------------------------
 */
void
World_SetDefaultGDTEntry(int index, LA base, VA limit, uint32 type, 
                         uint32 S, uint32 DPL, uint32 present, uint32 DB, 
                         uint32 gran)
{
   // check to make sure that no worlds have been started yet.
   ASSERT(!worlds[0].inUse);
   Desc_SetDescriptor(&defaultGDT[index], base, limit, type, S, DPL, present, DB, gran);
   Host_SetGDTEntry(index, base, limit, type, S, DPL, present, DB, gran);
}


/*
 *----------------------------------------------------------------------
 *
 * WorldTableInitEntry --
 *
 *      Initialize an entry in the world table.   Calculates the
 *      worldID, and initializes misc fields.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
WorldTableInitEntry(World_Handle *world, World_InitArgs *args)
{
   int entryNum = world - worlds;
   uint32 generation = world->generation + 1;
   World_ID genID;

   /*
    * World_ID should be positive and not 0, and also world_id + 100K needs
    * to be positive, so limit to 30 bits, and check for 0.  Technically we
    * don't have to check for 0 since the first world is always COS world,
    * which can never die...
    */
   genID = (generation * WORLD_TABLE_LENGTH) % (MAX_WORLD_ID + 1);
   if (genID == 0) {
      generation ++;
      genID = (generation * WORLD_TABLE_LENGTH) % (MAX_WORLD_ID + 1);
   }

   memset(world, 0, sizeof(*world));
   world->generation = generation;

   world->worldID = entryNum + genID;
   ASSERT(world->worldID <= MAX_WORLD_ID);

   VMLOG(1, world->worldID, "using table entry %d (%p)", entryNum, world);

   world->inUse = TRUE;
   world->typeFlags = args->flags;

   ASSERT(strlen(args->name) < WORLD_NAME_LENGTH);
   strncpy(world->worldName, args->name, WORLD_NAME_LENGTH);

   // fpuSaveArea need be 16bytes aligned for the FXSAVE instruction. 
   world->savedState.fpuSaveAreaOffset = 
      (0xf - (((uint32)(&(world->savedState.fpuSaveAreaMem[0])) + 0xf) & 0xf));
}

/*
 *----------------------------------------------------------------------
 *
 * WorldSharedDataInit --
 *
 *      Allocate and initialize the vmm<->vmk shared data structure.
 *
 * Results:
 *      A VMK_ReturnStatus value.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
WorldSharedDataInit(World_Handle *world, World_InitArgs *args)
{
   VMK_SharedData *shared;

   shared = SharedArea_Alloc(world, "vmkSharedData",
                             sizeof(VMK_SharedData) * MAX_VCPUS);
   if (shared == NULL) {
      Warning("vmkSharedData not present in shared area");
      return VMK_NOT_SUPPORTED;  // Essentially a VMKCheckVersion failure
   }
   shared += args->vcpuid;  // point to the part that's for this VCPU
   memset(shared, 0, sizeof(struct VMK_SharedData));
   shared->sizeofSharedData = sizeof(VMK_SharedData);

   world->vmkSharedData = shared;
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * WorldSharedDataCleanup --
 *
 *      Clean up vmm<->vmk shared data.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void 
WorldSharedDataCleanup(World_Handle *world)
{
   world->vmkSharedData = NULL;
}


/*
 *----------------------------------------------------------------------
 *
 * WorldProcInit --
 *
 *      Create this worlds proc entry. Other modules can add world specific
 *      proc entries in this directory
 *
 * Results:
 *      A VMK_ReturnStatus value.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
WorldProcInit(World_Handle *world, UNUSED_PARAM(World_InitArgs *args))
{
   char buf[16];

   Proc_InitEntry(&world->procWorldDir);
   world->procWorldDir.parent = &procWorlds;
   snprintf(buf, sizeof buf, "%d", world->worldID);
   Proc_Register(&world->procWorldDir, buf, TRUE);

   Proc_InitEntry(&world->procWorldDebug);
   world->procWorldDebug.parent = &world->procWorldDir;
   world->procWorldDebug.private = (void *)world->worldID;   
   world->procWorldDebug.write = WorldProcDebugWrite;
   snprintf(buf, sizeof buf, "debug");
   Proc_RegisterHidden(&world->procWorldDebug, buf, FALSE);


   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * WorldProcCleanup --
 *
 *      removes the worlds proc directory.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
WorldProcCleanup(World_Handle *world)
{
   Proc_Remove(&world->procWorldDebug);
   Proc_Remove(&world->procWorldDir);
}


/*
 *----------------------------------------------------------------------
 *
 * WorldGroupInit --
 *
 *      Initializes the world->group structure, which is shared
 *      among the group members.
 *
 * Results:
 *      A VMK_ReturnStatus value.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
WorldGroupInit(World_Handle *world, World_InitArgs *args)
{
   SP_IRQL prevIRQL;
   Heap_ID heap;
   char name[MAX_HEAP_NAME];

   if (args->groupLeader == WORLD_GROUP_DEFAULT) {
      
      snprintf(name, sizeof(name), "worldGroup%d", world->worldID); 
      
      heap = Heap_CreateDynamic(name,
				WORLDGROUP_HEAP_INITIAL_SIZE,
				WORLDGROUP_HEAP_MAX_SIZE);
      
      if (heap == INVALID_HEAP_ID) {
	 return VMK_NO_MEMORY;
      }

      world->group = Heap_Alloc(heap, sizeof(World_GroupInfo));
      if (!world->group) {
	 Heap_Destroy(heap);
         return VMK_NO_MEMORY;
      }
      memset(world->group, 0, sizeof(World_GroupInfo));
      world->group->groupID = world->worldID;
      world->group->heap = heap;
   } else {
     World_Handle *groupLeader = World_Find(args->groupLeader);

     if (groupLeader == NULL) {
        return VMK_NOT_FOUND;
     }
     world->group = groupLeader->group;
     World_Release(groupLeader);
   }

   prevIRQL = WorldLock();
   world->group->memberCount++;
   WorldUnlock(prevIRQL);

   if (World_IsVMMWorld(world) || World_IsTESTWorld(world)) {
      World_Handle *vmmLeader;
      World_VmmGroupInfo *group = World_VMMGroup(world);

      prevIRQL = WorldLock();
      vmmLeader = group->vmmLeader;
      if (vmmLeader == NULL) {
         ASSERT(group->memberCount == 0);
         group->vmmLeader = world;
         group->memberCount = 0;
      }
      ASSERT(group->memberCount < MAX_VCPUS);
      group->members[group->memberCount++] = world->worldID;
      WorldUnlock(prevIRQL);

      if (!World_IsVmmLeader(world)) {
         World_Find(vmmLeader->worldID);
         /* 
          * World_Release is called in WorldGroupCleanup -- this ensures 
          * that the vmm leader doesn't go away until all the other members 
          * have been destroyed.
          */
      }
      // If the VMM world is the group leader, it also must be the vmm leader.
      ASSERT(!World_IsGroupLeader(world) || World_IsVmmLeader(world));
   }
   
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * WorldGroupCleanup --
 *
 *      Decrement's the reference count on the vmm leader and 
 *      the group struct. Free group struct if no other references.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	The vmm leader will be allowed to be reaped.
 *
 *----------------------------------------------------------------------
 */
static void
WorldGroupCleanup(World_Handle *world)
{
   SP_IRQL prevIRQL;
   World_GroupInfo *g = world->group;
   uint32 count;
   Heap_ID heap;

   if (World_IsVMMWorld(world)) {
      if (World_IsVmmLeader(world)) {
         if (g->vmm.cfgPath || g->vmm.uuidString || g->vmm.displayName) {
            Proc_Remove(&g->vmm.procVMXInfo);
         }
         if (g->vmm.cfgPath) {
            World_Free(world, g->vmm.cfgPath);
         }
         if (g->vmm.uuidString) {
            World_Free(world, g->vmm.uuidString);
         }
         if (g->vmm.displayName) {
            World_Free(world, g->vmm.displayName);
         }
         // clear all vmm specific fields
         memset(&g->vmm, 0, sizeof(g->vmm));
      } else {
         // release the vmm leader
         World_Release(World_GetVmmLeader(world));
      }
   } else if (World_IsTESTWorld(world) && !World_IsGroupLeader(world)) {
      // release the leader
      World_Release(World_GetVmmLeader(world));
   }
   prevIRQL = WorldLock();
   world->group->memberCount--;
   count = world->group->memberCount;
   WorldUnlock(prevIRQL);

   if (count == 0) {
      if (Sched_WorldGroupCleanup(world) != VMK_OK) {
         Warning("Sched group %d was not destroyed cleanly.", g->schedGroupID);
      }

      // last step: free group memory
      heap = g->heap;
      Heap_Free(heap, g);

      if (Heap_Destroy(heap) != VMK_OK) {
         Warning("World group heap at %p was not destroyed cleanly.", heap);
      }
   }

   world->group = NULL;
}


/*
 *----------------------------------------------------------------------
 *
 * WorldCOSStackInit --
 *
 *      Initialize the stack of the Console OS world.
 *
 * Results:
 *      A VMK_ReturnStatus value.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
WorldCOSStackInit(World_Handle *world, World_InitArgs *args)
{
   ASSERT(VMK_HOST_STACK_PAGES <= WORLD_VMK_NUM_STACK_MPNS);

   world->vmkStackMPNs[0] = args->COSStackMPN;
   world->vmkStackMPNs[1] = args->COSStackMPN + 1;
   world->vmkStackStart = VMK_HOST_STACK_BASE;
   world->vmkStackLength = VMK_HOST_STACK_PAGES * PAGE_SIZE;

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * WorldVMMStackInit --
 *
 *      Initialize the stack of a vmm world.
 *
 * Results:
 *      A VMK_ReturnStatus value.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
WorldVMMStackInit(World_Handle *world, World_InitArgs *args)
{
   int i, j;
   World_VmmInfo *vmm = World_VMM(world);

   /*
    * We assume that both VMM stacks are the same length.
    */
   ASSERT(CPL0_STACK_PAGES_LEN == CPL1_STACK_PAGES_LEN);
   vmm->vmmStackInfo[0].stackBase = WORLD_VMM_STACK_PGOFF * 
      PAGE_SIZE;
   vmm->vmmStackInfo[0].stackTop = vmm->vmmStackInfo[0].stackBase + 
      WORLD_VMM_NUM_STACK_MPNS * PAGE_SIZE;
   vmm->vmmStackInfo[1].stackBase = WORLD_VMM_2ND_STACK_PGOFF * PAGE_SIZE;
   vmm->vmmStackInfo[1].stackTop = vmm->vmmStackInfo[1].stackBase + 
      WORLD_VMM_NUM_STACK_MPNS * PAGE_SIZE;

   for (i = 0; i < WORLD_VMM_NUM_STACKS; i++) {
      for (j = 0; j < WORLD_VMM_NUM_STACK_MPNS; j++) {
	 vmm->vmmStackInfo[i].mpns[j] = INVALID_MPN;
      }
   }

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * WorldVMMStackCleanup --
 *
 *      Unmaps the pages of the vmm stack that have been mapped in.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	Pages are freed.
 *
 *----------------------------------------------------------------------
 */
static void
WorldVMMStackCleanup(World_Handle *world)
{
   int i;
   for (i = 0; i < WORLD_VMM_NUM_STACKS; i++) {
      if (World_VMM(world)->vmmStackInfo[i].mappedStack != NULL) {
         KVMap_FreePages(World_VMM(world)->vmmStackInfo[i].mappedStack);
         World_VMM(world)->vmmStackInfo[i].mappedStack = NULL;
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * WorldVMKStackInit --
 *
 *      Allocate and map the vmkernel stack for this world.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	Pages are allocated 
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
WorldVMKStackInit(World_Handle *world, UNUSED_PARAM(World_InitArgs *args))
{
   int i;
   int firstStackPage = (world - &worlds[0]) * WORLD_VMK_NUM_STACK_VPNS;

   VMLOG(1, world->worldID, "VMM-VMK stack: firstStackPage = %d, VPN=%d", 
         firstStackPage, VMK_FIRST_STACK_VPN + firstStackPage);

   for (i = 0; i < WORLD_VMK_NUM_STACK_VPNS; i++) {
      int ptableNum;
      VMK_PTE *ptable;      
      VMK_PTE *pte;

      ptableNum = (firstStackPage + i) / VMK_PTES_PER_PDE;
      ASSERT(ptableNum < VMK_NUM_STACK_PDES);
      ptable = worldStackPTables[ptableNum];
      pte = &ptable[firstStackPage + i - ptableNum * VMK_PTES_PER_PDE];
      if (i == 0) {
	 PT_SET(pte, 0);	 
      } else {
	 MPN mpn = MemMap_AllocKernelPageWait(MM_NODE_ANY,
					      MM_COLOR_ANY,
					      MM_TYPE_LOWRESERVED,
					      ALLOC_LOW_MEM_MAX_WAIT);
	 if (mpn == INVALID_MPN) {
	    return VMK_NO_MEMORY;
	 }
	 PT_SET(pte, VMK_MAKE_PTE(mpn, 0, PTE_KERNEL));
	 world->vmkStackMPNs[i - 1] = mpn;
      }
   }

   TLB_FLUSH();       

   world->vmkStackStart = VPN_2_VA(VMK_FIRST_STACK_VPN + firstStackPage + 1);
   world->vmkStackLength = WORLD_VMK_NUM_STACK_MPNS * PAGE_SIZE;    
   memset((void *)(world->vmkStackStart), 0, WORLD_VMK_NUM_STACK_MPNS * PAGE_SIZE);

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * WorldVMKStackCleanup --
 *
 *      Free the vmkernel stack for this world.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	Pages are freed.
 *
 *----------------------------------------------------------------------
 */
static void
WorldVMKStackCleanup(World_Handle *world)
{
   int i;
   int firstStackPage = (world - &worlds[0]) * WORLD_VMK_NUM_STACK_VPNS;
   for (i = 0; i < WORLD_VMK_NUM_STACK_VPNS; i++) {
      int ptableNum;
      VMK_PTE *ptable;
      VMK_PTE *pte;

      ptableNum = (firstStackPage + i) / VMK_PTES_PER_PDE;
      ptable = worldStackPTables[ptableNum];
      ASSERT(ptable != NULL);

      pte = &ptable[firstStackPage + i - ptableNum * VMK_PTES_PER_PDE];
      if (PTE_PRESENT(*pte)) {
	 MPN mpn = VMK_PTE_2_MPN(*pte);
	 ASSERT(mpn == world->vmkStackMPNs[i - 1]);
	 MemMap_FreeKernelPage(mpn);
	 world->vmkStackMPNs[i - 1] = INVALID_MPN;
	 PT_SET(pte, 0);
      }
   }
   TLB_FLUSH();
}



/*
 *----------------------------------------------------------------------
 *
 * WorldSavedStateInit --
 *
 *      Initialize the savedState fields -- these will be used as
 *      the initial state the first time this world is scheduled.
 *
 * Results:
 *      A VMK_ReturnStatus value.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
WorldSavedStateInit(World_Handle *world, World_InitArgs *args)
{
   world->savedState.eip = (uint32) args->func;

   world->savedState.DR[0] = 0;
   world->savedState.DR[1] = 0;
   world->savedState.DR[2] = 0;
   world->savedState.DR[3] = 0;
   world->savedState.DR[4] = 0;
   world->savedState.DR[5] = 0;
   world->savedState.DR[6] = DR6_ONES;
   world->savedState.DR[7] = DR7_ONES;


   world->savedState.segRegs[SEG_ES] =  DEFAULT_ES;
   world->savedState.segRegs[SEG_CS] =  DEFAULT_CS;
   world->savedState.segRegs[SEG_SS] =  DEFAULT_SS;
   world->savedState.segRegs[SEG_DS] =  DEFAULT_DS;
   world->savedState.segRegs[SEG_FS] =  DEFAULT_FS;
   world->savedState.segRegs[SEG_GS] =  DEFAULT_GS;

   world->savedState.segRegs[SEG_LDTR] = 0;

   /*
    * We need to give each world its own GDT so we can run more than one 
    * world at a time.
    */
   world->savedState.segRegs[SEG_TR] =  MAKE_SELECTOR(DEFAULT_TSS_DESC, SELECTOR_GDT, 0);

   /* gdtr */
   world->savedState.GDTR.limit = sizeof(defaultGDT) - 1;
   world->savedState.GDTR.offset = VMK_VA_2_LA((VA) world->kernelGDT);
   VMLOG(1, world->worldID, "GDT at offset = 0x%x", world->savedState.GDTR.offset);

   /* idtr */
   if (World_IsUSERWorld(world)) {
      IDT_GetDefaultUserIDT(&(world->savedState.IDTR));
   } else {
      IDT_GetDefaultIDT(&(world->savedState.IDTR));
   }

   world->savedState.CR[0] = 0; //  cr2
   world->savedState.CR[2] = 0; // mutable cr0
   /* User Worlds can RDTSC */
   if (World_IsUSERWorld(world)) {
      world->savedState.CR[4] = (CR4_DE|CR4_PCE);
   } else {
      world->savedState.CR[4] = (CR4_TSD|CR4_DE|CR4_PCE);
   }


   /* saved state */

   world->savedState.CR[3] = world->pageRootMA;

   world->savedState.regs[REG_EAX] = 0;
   world->savedState.regs[REG_EBX] = 0;
   world->savedState.regs[REG_ECX] = 0; 
   world->savedState.regs[REG_EDX] = 0; 
   world->savedState.regs[REG_ESI] = 0; 
   world->savedState.regs[REG_EDI] = 0; 
   world->savedState.regs[REG_EBP] = 0;

   world->savedState.regs[REG_ESP] = World_GetVMKStackTop(world);

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * WorldVMXInfoReadHandler  --
 *
 *      Proc read handler.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
WorldVMXInfoReadHandler(Proc_Entry  *entry,
                        char        *page,
                        int         *len)
{
   World_Handle *world = entry->private;
   *len = 0;
   Proc_Printf(page, len,
               "vmid=%-6d "
               "pid=%-6d "
               "cfgFile=\"%s\"  "
               "uuid=\"%s\"  "
               "displayName=\"%s\"\n", 
               world->worldID,
               World_VMMGroup(world)->vmxPID,
               World_VMMGroup(world)->cfgPath,
               World_VMMGroup(world)->uuidString,
               World_VMMGroup(world)->displayName);
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * World_SetVMXInfoWork  --
 *
 *      Called by the vmx to cache vmx specific information in the
 *      vmkernel.  This is mostly useful for debuging.
 *
 * Results:
 *      If world is not found returns VMK_NOT_FOUND, if World_Alloc()
 *      of string space fails returns VMK_NO_MEMORY.
 *
 * Side effects:
 *	VMX debugging info is cached.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
World_SetVMXInfoWork(World_ID vmmLeaderID,
                     uint32 vmxPID,
                     const char *cfgPath, 
                     const char *uuidString,
                     const char *displayName)
{
   int len;
   World_GroupInfo *g;
   World_Handle *world;

   world = World_Find(vmmLeaderID);
   if (!world) {
      WarnVmNotFound(vmmLeaderID);
      return VMK_NOT_FOUND;
   }

   if (!World_IsVMMWorld(world)) {
      World_Release(world);
      return VMK_NOT_SUPPORTED;
   }

   g = world->group;

   if (g->vmm.cfgPath || g->vmm.uuidString || g->vmm.displayName) {
      World_Release(world);
      World_Panic(world, "Should only set VMX info once!\n");
      return VMK_OK;
   }

   g->vmm.vmxPID = vmxPID;

   len = strnlen(cfgPath, WORLD_MAX_CONFIGFILE_SIZE);
   g->vmm.cfgPath = World_Alloc(world, len + 1);
   if (!g->vmm.cfgPath) {
      World_Release(world);
      return VMK_NO_MEMORY;
   }
   strncpy(g->vmm.cfgPath, cfgPath, len);
   g->vmm.cfgPath[len] = '\0';

   len = strnlen(uuidString, WORLD_MAX_UUIDTEXT_SIZE);
   g->vmm.uuidString = World_Alloc(world, len + 1);
   if (!g->vmm.uuidString) {
      World_Free(world, g->vmm.cfgPath);
      g->vmm.cfgPath = NULL;
      World_Release(world);
      return VMK_NO_MEMORY;
   }
   strncpy(g->vmm.uuidString, uuidString, len);
   g->vmm.uuidString[len] = '\0';

   len = strnlen(displayName, WORLD_MAX_DISPLAYNAME_SIZE);
   g->vmm.displayName = World_Alloc(world, len + 1);
   if (!g->vmm.displayName) {
      World_Free(world, g->vmm.cfgPath);
      g->vmm.cfgPath = NULL;
      World_Free(world, g->vmm.uuidString);
      g->vmm.uuidString = NULL;
      World_Release(world);
      return VMK_NO_MEMORY;
   }
   strncpy(g->vmm.displayName, displayName, len);
   g->vmm.displayName[len] = '\0';

   Proc_InitEntry(&g->vmm.procVMXInfo);
   g->vmm.procVMXInfo.parent = &world->procWorldDir;
   g->vmm.procVMXInfo.read  = WorldVMXInfoReadHandler;
   g->vmm.procVMXInfo.private = world;
   Proc_Register(&g->vmm.procVMXInfo, "names", FALSE);

   World_Release(world);
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * World_SetVMXInfo --
 *
 *      Cache vmx specific information in the vmkernel for debugging.
 *      Marshalls arguments in a VMnix_VMXInfoArgs struct. Called by
 *      a non-userworld vmx.
 *
 * Results:
 *      Returs result fromm World_SetVMXInfoWork, normally VMK_OK.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
World_SetVMXInfo(void *hostArgs) 
{
   VMnix_VMXInfoArgs args;

   ASSERT(sizeof(args.cfgPath) == WORLD_MAX_CONFIGFILE_SIZE);
   ASSERT(sizeof(args.uuidString) == WORLD_MAX_UUIDTEXT_SIZE);
   ASSERT(sizeof(args.displayName) == WORLD_MAX_DISPLAYNAME_SIZE);

   CopyFromHost(&args, hostArgs, sizeof(args));

   return World_SetVMXInfoWork(args.worldID, args.vmxPID, args.cfgPath,
                               args.uuidString, args.displayName);
}


/*
 *----------------------------------------------------------------------
 *
 * World_LogBacktrace  --
 *
 *      Dumps a backtrace for the given world to the log.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

void
World_LogBacktrace(World_ID wid)
{
   uint32 *p = (uint32 *)&wid;
   World_Handle *world;

   world = World_Find(wid);
   if (!world) {
      WarnVmNotFound(wid);
      return;
   }

   VmLog(wid, "Generating backtrace for '%s'", world->worldName);
   if (MY_RUNNING_WORLD == world) {
      Util_Backtrace(*(p - 1), *(p - 2), _Log, TRUE);
   } else {
      Util_Backtrace(world->savedState.eip, world->savedState.regs[REG_EBP], _Log, TRUE);
   }
   World_Release(world);
}


/*
 *----------------------------------------------------------------------
 *
 * World_AfterPanic --
 *      
 *      Called when a vmm world panics and has been switched out
 *      permanently.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	May post panic to VMX.
 *	
 *----------------------------------------------------------------------
 */
void
World_AfterPanic(World_Handle *world)
{
   int i;
   Bool postVMXPanic = TRUE;
   World_VmmGroupInfo *vmmGroup = World_VMMGroup(world);
   world->okToReadRegs = TRUE;
   ASSERT(World_IsVMMWorld(world) && World_VMM(world)->inVMMPanic);

   for (i = 0; i < vmmGroup->memberCount; i++) {
      World_Handle *w = World_Find(vmmGroup->members[i]);
      if (w) {
         postVMXPanic = postVMXPanic && w->okToReadRegs;
         if (w->worldID != vmmGroup->panickyWorld) {
            /* Did it already for the world which panicked. */
            World_LogBacktrace(w->worldID);
         }
         World_Release(w);
      } else {
         VMLOG(0, vmmGroup->members[i], "Panicking world went away!");
      }
   }
   /* Make sure we post to the VMX exactly once for a group. */
   if (postVMXPanic) {
     if (Atomic_ReadIfEqualWrite(&vmmGroup->panicState, WORLD_GROUP_PANIC_BEGIN, 
                       WORLD_GROUP_PANIC_VMXPOST) == WORLD_GROUP_PANIC_BEGIN) {
        VmkEvent_PostVmxMsg(vmmGroup->panickyWorld, VMKEVENT_PANIC, 
                            vmmGroup->panicMsg ? vmmGroup->panicMsg : "Unknown", 
                            RPC_MAX_MSG_LENGTH);
        if (vmmGroup->panicMsg) {
           World_Free(world, vmmGroup->panicMsg);
        }
        VMLOG(0, world->worldID, "Posting panic to vmx (%d)", 
              vmmGroup->panickyWorld);
     } else {
        VMLOG(0, world->worldID, "Not posting panic to vmx (%d)", 
              vmmGroup->panickyWorld);
     }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * WorldPanicWork --
 *
 *      Panic a world group.   The world, and the other members of 
 *      the world group will get descheduled on the next interrupt,
 *      scheduling decision, or before returning to the monitor.
 *
 * Results: 
 *      None.
 *
 * Side effects:
 *      The target world, and its group, are panic'd, unless
 *      there is no group or the world is not associated with a VMM.
 *
 *----------------------------------------------------------------------
 */
static void 
WorldPanicWork(World_Handle *world, const char *fmt, va_list args)
{
   World_VmmGroupInfo *vmmGroup = World_VMMGroup(world);
   char panicMsg[RPC_MAX_MSG_LENGTH];
   uint32 *p = (uint32 *)&world;
   int i;

   vsnprintf(panicMsg, sizeof panicMsg, fmt, args);

   ASSERT_NOT_IMPLEMENTED(world->group && World_IsVMMWorld(world));

   if (vmmGroup->vmmLeader->deathPending) {
      VmWarn(world->worldID, "Secondary World_Panic: %s", panicMsg);
      world->deathPending = TRUE;
      World_VMM(world)->inVMMPanic = TRUE;
      return;
   }
   VmWarn(world->worldID, "%s:%s", world->worldName, panicMsg);

   if (Atomic_ReadIfEqualWrite(&vmmGroup->panicState, WORLD_GROUP_PANIC_NONE, 
                               WORLD_GROUP_PANIC_BEGIN) == 
       WORLD_GROUP_PANIC_NONE) {
      vmmGroup->panicMsg = World_Alloc(world, RPC_MAX_MSG_LENGTH);
      if (vmmGroup->panicMsg) {
         memcpy(vmmGroup->panicMsg, panicMsg, RPC_MAX_MSG_LENGTH);
      } 
      vmmGroup->panickyWorld = world->worldID;
   }

   Log("vmm group leader = %d, members = %d",
       World_GetVmmLeaderID(world), vmmGroup->memberCount);

   Util_Backtrace(*(p - 1), *(p - 2), _Log, TRUE);

   // Post death to all members.
   for (i = 0; i < vmmGroup->memberCount; i++) {
      World_Handle *w = World_Find(vmmGroup->members[i]);
      if (w == NULL) {
         VmWarn(world->worldID, "Couldn't find group member %d", 
                vmmGroup->members[i]);
      } else {
         PCPU pcpu = World_CpuSchedVcpu(world)->pcpu;
         VMLOG(0, MY_RUNNING_WORLD->worldID, "Sending death to vm %d", 
               w->worldID);
         w->deathPending = TRUE;
         World_VMM(w)->inVMMPanic = TRUE;
         ASSERT_NOT_IMPLEMENTED(pcpu != INVALID_PCPU);
         /* This is a racy request to reschedule, which is ok. */
         CpuSched_MarkReschedule(pcpu);
         World_Release(w);
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * World_Panic --
 *
 *      See WorldPanicWork.  Always returns.  Callers need to
 *      do the appropriate cleanup.
 *
 * Results: 
 *      None.
 *
 * Side effects:
 *      World eventually dies horribly.
 *
 *----------------------------------------------------------------------
 */
void
World_Panic(World_Handle *world, const char *fmt, ...)
{
   va_list args;

   va_start(args, fmt);
   if (world == MY_RUNNING_WORLD) {
      World_ResetDefaultDT();
   }
   WorldPanicWork(world, fmt, args);
   va_end(args);
}



/*
 *----------------------------------------------------------------------
 *
 * World_VMMPanic --
 *
 *      Called by the vmm to panic the current World.  Does not return.
 *
 * Results: 
 *      Never returns.
 *
 * Side effects:
 *      Current world is terminated, other worlds in WorldGroup are
 *      scheduled for termination.
 *
 *----------------------------------------------------------------------
 */
VMKERNEL_ENTRY
World_VMMPanic(DECLARE_ARGS(VMK_PANIC))
{
   PROCESS_5_ARGS(VMK_PANIC, Reg32, ebp, Reg32, eip, Reg32, esp, 
                  char *, fmt, va_list, panicArgs);
   
   NMI_Disable();

   /*
    * Switch to default descriptor tables so that any further
    * exception will not be handled by the VMM (as it is
    * already panic'ing).
    */
   World_ResetDefaultDT();

   /* VMM world's coredump routine (see ReadRegs) uses vmmCoreDumpEBP. */ 
   World_VMM(MY_RUNNING_WORLD)->vmmCoreDumpEBP = ebp;
   World_VMM(MY_RUNNING_WORLD)->vmmCoreDumpESP = esp;
   World_VMM(MY_RUNNING_WORLD)->vmmCoreDumpEIP = eip;
   WorldPanicWork(MY_RUNNING_WORLD, fmt, panicArgs);
   World_Exit(VMK_OK);
}


/*
 *-----------------------------------------------------------------------------
 *
 * World_GetVmmMembers --
 *
 *     Obtains the list of all vmm members in "world"'s vsmp and stores
 *     it in the "outHandles" array. outHandles should have room for up
 *     to MAX_VCPUS entries.
 *     Increments the reader count of each returned world if readerLock
 *     is TRUE.
 *
 * Results:
 *     Returns the number of worlds in the group
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
uint32
World_GetVmmMembers(World_Handle *world, // in
                    World_Handle **outHandles) // out
{
   uint32 i, members;
   World_VmmGroupInfo *vmmGroup;
   SP_IRQL prevIRQL;

   prevIRQL = WorldLock();
   vmmGroup = World_VMMGroup(world);
   members = vmmGroup->memberCount;
   for (i=0; i < members; i++) {
      outHandles[i] = WorldFind(vmmGroup->members[i]);
      ASSERT(outHandles[i] != NULL);
      outHandles[i]->readerCount++;
   }
   WorldUnlock(prevIRQL);

   return members;
}

/*
 *-----------------------------------------------------------------------------
 *
 * World_ReleaseVmmMembers --
 *
 *     Decrements the reader count on each vmm world member
 *
 * Results:
 *     None.
 *
 * Side effects:
 *     Decrements the reader count on each vmm world member.
 *
 *-----------------------------------------------------------------------------
 */
void
World_ReleaseVmmMembers(World_Handle *world)
{
   uint32 i;
   World_VmmGroupInfo *vmmGroup;
   SP_IRQL prevIRQL;

   prevIRQL = WorldLock();
   vmmGroup = World_VMMGroup(world);
   for (i=0; i < vmmGroup->memberCount; i++) {
      World_Handle *thisWorld = WorldFind(vmmGroup->members[i]);
      ASSERT(thisWorld != NULL);
      if (thisWorld != NULL) {
         thisWorld->readerCount--;
      }
   }
   WorldUnlock(prevIRQL);
}

/*
 *----------------------------------------------------------------------
 *
 * World_FormatTypeFlags --
 *
 *      Writes terse string summary of world type "flags"
 *	into "buf", without exceeding "maxLen" characters.
 *
 * Results:
 *      Returns the number of characters written to "buf".
 *
 * Side effects:
 *      Modifies "buf".
 *
 *----------------------------------------------------------------------
 */
int
World_FormatTypeFlags(uint32 flags, char *buf, int maxLen)
{
   char name[10];
   int len = 0;

   if (flags & WORLD_SYSTEM) {
      name[len++] = 'S';
   }
   if (flags & WORLD_IDLE) {
      name[len++] = 'I';
   }
   if (flags & WORLD_USER) {
      name[len++] = 'U';
   }   
   if (flags & WORLD_VMM) {
      name[len++] = 'V';
   }
   if (flags & WORLD_HELPER) {
      name[len++] = 'H';
   }
   if (flags & WORLD_HOST) {
      name[len++] = 'C';
   }
   if (flags & WORLD_TEST) {
      name[len++] = 'T';
   }         
   if (flags & WORLD_POST) {
      name[len++] = 'P';
   }         
   name[len++] = '\0';
   
   return(MIN(maxLen, snprintf(buf, maxLen, "%s", name)));
}

/*
 *----------------------------------------------------------------------
 *
 * WorldIsSafeToDescheduleWithLock --
 *
 *      Is it safe to deschedule the current world?  If given lock is not
 *      NULL, it's OK to hold that lock, but only that lock.
 *
 * Results:
 *      TRUE if safe, FALSE otherwise
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
Bool
World_IsSafeToDescheduleWithLock(const SP_SpinLock *lock, const SP_SpinLockIRQ *lockIRQ)
{
   /*
    * not with spinlocks held -- callers shouldn't even call this function
    * in this case since we can't check this on release builds.
    */
   if (lock) {
      ASSERT(lockIRQ == NULL);
      SP_AssertOneLockHeld(lock);
   } else if (lockIRQ) {
      ASSERT(lock == NULL);
      SP_AssertOneLockHeldIRQ(lockIRQ);
   } else {
      SP_AssertNoLocksHeld();
   }
   
   if ((myPRDA.ksegActiveMaps != 0) || //  active kseg mappings
       (myPRDA.bhInProgress) ||        //  in a bottom half
       (myPRDA.inNMI) ||               //  in an NMI handler
       (myPRDA.inInterruptHandler)) {  //  in an interrupt handler
      return FALSE;
   } else {
      return TRUE;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * World_IsSafeToBlockWithLock --
 *
 *      Is it safe to block the current world? If given lock is not
 *      NULL, it's OK to hold that lock, but only that lock.
 *
 * Results:
 *      TRUE if safe, FALSE otherwise
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
Bool
World_IsSafeToBlockWithLock(const SP_SpinLock *lock, const SP_SpinLockIRQ *lockIRQ)
{
   // if can't deschedule, then obviously can't block
   if (!World_IsSafeToDescheduleWithLock(lock, lockIRQ)) {
      return FALSE;
   }
   // COS world can't be blocked because it handles shared interrupts
   if (World_IsHOSTWorld(MY_RUNNING_WORLD)) {
      return FALSE;
   }
   // Idle world can't be blocked either
   if (World_IsIDLEWorld(MY_RUNNING_WORLD)) {
      return FALSE;
   }
   return TRUE;
}

/*
 *----------------------------------------------------------------------
 *
 * WorldProcDebugWrite  --
 *
 *      Callback for write operation on /proc/vmware/vm/<id>/debug
 *	procfs node.
 *
 * Results:
 *      Returns VMK_OK.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
WorldProcDebugWrite(Proc_Entry *entry,
                    char       *buffer,
                    int        *len)
{
   World_ID wid = (World_ID) entry->private;
   World_Handle *world;
  
   if ((world = World_Find(wid)) == NULL) {
      WarnVmNotFound(wid);
      return VMK_OK;
   }

   buffer[*len] = '\0';
   VmLog(wid, "Got command '%s'", buffer);
   if (strncmp(buffer, "panic", 5) == 0) {
      World_Panic(world, "panic for debug purposes");
   } else if (strncmp(buffer, "coredump", 8) == 0) {
      Bool consistent = FALSE;
      VmkEvent_PostVmxMsg(world->worldID, VMKEVENT_REQUEST_VMMCOREDUMP, &consistent, 
                          sizeof consistent);
   } else if (strncmp(buffer, "consistent coredump", 19) == 0) {
      Bool consistent = TRUE;
      VmkEvent_PostVmxMsg(world->worldID, VMKEVENT_REQUEST_VMMCOREDUMP, &consistent, 
                          sizeof consistent);
   } else if (strncmp(buffer, "vmxcore", 7) == 0) {
      int dummy = 0;
      VmkEvent_PostVmxMsg(world->worldID, VMKEVENT_REQUEST_VMXCOREDUMP, &dummy, sizeof dummy);
   } else if (strncmp(buffer, "tcl", 3) == 0) {
      /* ie "tcl set LOGLEVEL(vmm.intr) 10" */
      VmkEvent_PostVmxMsg(world->worldID, VMKEVENT_REQUEST_TCLCMD, 
                          buffer + 4, *len - 4);
   } else if (strncmp(buffer, "bt", 2) == 0) {
      World_LogBacktrace(world->worldID);
#ifdef VMX86_DEBUG
   } else if (strncmp(buffer, "kill -9", 7) == 0) {
      /* may have to do 'kill -9' multiple times */
      if (world->hostCount != 0) {
         world->hostCount = 0;
      } else if (world->readerCount != 1) {
         /*
          * Assume the World_Find() in this function is the reader, so that we can reap
          * the world. Should never do "kill -9" before trying "kill".
          */
         world->readerCount = 1;
      }
      WorldKillUnconditional(world);
#endif
   } else if (strncmp(buffer, "kill", 4) == 0) {
      World_GroupKill(world);
   } 

   World_Release(world);
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * WorldVMMLicenseInit --
 *
 *      Keep track of the number of VMs for licensing.
 *
 * Results:
 *      VMK_OK
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
WorldVMMLicenseInit(World_Handle *world, World_InitArgs *args)
{
   ASSERT(World_IsVMMWorld(world));

   if (World_IsVmmLeader(world)) {
      Atomic_Inc(&worldActiveGroupCount);
      LOG(1, "Incremented active world count to %d",
          Atomic_Read(&worldActiveGroupCount));
   }

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * WorldVMMLicenseCleanup --
 *
 *      Keep track of the number of VMs for licensing.
 *
 * Results:
 *      None
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
void
WorldVMMLicenseCleanup(World_Handle *world)
{
   ASSERT(World_IsVMMWorld(world));

   if (World_IsVmmLeader(world)) {
      ASSERT(Atomic_Read(&worldActiveGroupCount) > 0);
      Atomic_Dec(&worldActiveGroupCount);
      LOG(1, "Dropped active world count to %d",
          Atomic_Read(&worldActiveGroupCount));
   }

}


/*
 *----------------------------------------------------------------------
 *
 * World_Find --
 *
 *      Return a world pointer based on it world id.  Also increment the
 *      reader count
 *
 * Results:
 *      world_handle pointer
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
World_Handle *
World_Find(World_ID worldID)
{
   return WorldFindOptReaderLock(worldID, TRUE);
}


/*
 *----------------------------------------------------------------------
 *
 * World_Release --
 *
 *      Release the reference to world_handle that was previously returned
 *      from World_Find
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
void
World_Release(World_Handle *world)
{
   ASSERT(world);
   WorldReleaseOptReaderLock(world, TRUE);
}



/*
 *----------------------------------------------------------------------
 *
 * World_ReleaseAndWaitForDeath --
 *
 *	Release handle on world, wait for it to die.  You really should've
 *	invoked World_Destroy/World_Kill before calling this.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	Sleep a bit.
 *
 *----------------------------------------------------------------------
 */
void 
World_ReleaseAndWaitForDeath(World_Handle *world)
{
   World_ID wid = world->worldID;
   SP_IRQL prevIRQL;
   Bool done = FALSE;

   ASSERT(world);
   ASSERT(world->deathPending);  /* Perhaps too aggressive? */
   ASSERT(World_IsSafeToBlock());

   /* Drop our reference (hopefully last one) on world. */
   World_Release(world);

   SP_Lock(&worldDeathLock);
   prevIRQL = WorldLock();
   do {
      World_Handle* handle;

      // Test to see if world is still around
      handle = WorldFind(wid);

      // If so, wait for WorldCleanup to finish cleaning up world.
      if (handle != NULL) {
         ASSERT(handle == world);
         ASSERT(handle->deathPending);
         if (handle->inUse) {
            WorldUnlock(prevIRQL);
            CpuSched_Wait(WorldWaitEvent(wid), CPUSCHED_WAIT_WORLDDEATH,
                          &worldDeathLock);
            SP_Lock(&worldDeathLock);
            prevIRQL = WorldLock();
         }
      } else {
         done = TRUE;
      }
   } while (! done);
   
   WorldUnlock(prevIRQL);
   SP_Unlock(&worldDeathLock);
}


/*
 *----------------------------------------------------------------------
 *
 * World_FindNoRefCount --
 *
 *      Return a world pointer based on it world id without acquiring the
 *      reader lock.  This is dangerous since the world can go away at any
 *      time, so you better have a good reason to use this function.
 *
 * Results:
 *      world_handle pointer
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
World_Handle *
World_FindNoRefCount(World_ID worldID)
{
   return WorldFindOptReaderLock(worldID, FALSE);
}


/*
 *----------------------------------------------------------------------
 *
 * World_ReleaseNoRefCount --
 *
 *      Release the reference to world_handle that was previously returned
 *      from World_FindNoRefCount.  Doesn't decrement reader lock.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
void
World_ReleaseNoRefCount(World_Handle *world)
{
   WorldReleaseOptReaderLock(world, FALSE);
}


/*
 *----------------------------------------------------------------------
 *
 * World_VsiGetIDsList --
 *
 *      Returns the list of all world ids in the system.
 *
 * Results:
 *      A VMK_ReturnStatus value.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
World_VsiGetIDsList(VSI_NodeID nodeID, VSI_ParamList *instanceArgs, 
                    VSI_ParamList *instanceListOut)
{
   VMK_ReturnStatus status = VMK_OK;
   SP_IRQL prevIRQL;
   int i;

   LOG(0, "Here!");
   if (VSI_ParamListUsedCount(instanceArgs) != 0) {
      Warning("Incorrect # of instance args passed: %d",
              VSI_ParamListUsedCount(instanceArgs));
      return VMK_BAD_PARAM;
   }

   prevIRQL = WorldLock();
   for (i = 0; i < WORLD_TABLE_LENGTH; i++) {
      if (worlds[i].inUse) {
         status = VSI_ParamListAddInt(instanceListOut, worlds[i].worldID);
         if (status != VMK_OK) {
            Warning("Input list not long enough: %#x", status);
            break;
         }
      }
   }
   WorldUnlock(prevIRQL);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * World_VsiGetInfo --
 *
 *      Returns a WorldVsiInfo struct for the specified world.
 *
 * Results:
 *      A VMK_ReturnStatus value.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
World_VsiGetInfo(VSI_NodeID nodeID, VSI_ParamList *instanceArgs, WorldVsiInfo *out)
{
   VSI_Param *iParam;
   World_Handle *world;

   LOG(0, "Here!");
   if (VSI_ParamListUsedCount(instanceArgs) != 1) {
      Warning("Incorrect # of instance args passed: %d",
              VSI_ParamListUsedCount(instanceArgs));
      return VMK_BAD_PARAM;
   }

   iParam = VSI_ParamListGetParam(instanceArgs, 0);
   
   if (VSI_ParamGetType(iParam) != VSI_PARAM_INT64) {
      Warning("Non-int instance param");
      return VMK_BAD_PARAM;
   }

   world = World_Find((World_ID)VSI_ParamGetInt(iParam));
   if (!world) {
      LOG(0, "World not found %d", (World_ID)VSI_ParamGetInt(iParam));
      return VMK_NOT_FOUND;
   }

   memset(out, 0, sizeof *out);
   snprintf(out->name, sizeof out->displayName, "%s", World_VMMGroup(world)->displayName);
   snprintf(out->name, sizeof out->name, "%s", world->worldName);
   snprintf(out->cfgPath, sizeof out->cfgPath, "%s", World_VMMGroup(world)->cfgPath);
   snprintf(out->uuid, sizeof out->uuid, "%s", World_VMMGroup(world)->uuidString);
   out->worldID = world->worldID;
   out->pid = World_VMMGroup(world)->vmxPID,
               
   World_Release(world);

   return VMK_OK;
}

/*
 * Bogus test function
 */

VMK_ReturnStatus
World_VsiGetGroupList(VSI_NodeID nodeID, VSI_ParamList *instanceArgs, 
                      VSI_ParamList *instanceListOut)
{
   VMK_ReturnStatus status = VMK_OK;
   VSI_Param *iParam;
   World_Handle *world;
   int i;

   LOG(0, "Here!");
   if (VSI_ParamListUsedCount(instanceArgs) != 1) {
      Warning("Incorrect # of instance args passed: %d",
              VSI_ParamListUsedCount(instanceArgs));
      return VMK_BAD_PARAM;
   }

   iParam = VSI_ParamListGetParam(instanceArgs, 0);
   
   if (VSI_ParamGetType(iParam) != VSI_PARAM_INT64) {
      Warning("Non-int instance param");
      return VMK_BAD_PARAM;
   }

   world = World_Find((World_ID)VSI_ParamGetInt(iParam));
   if (!world) {
      LOG(0, "World not found %d", (World_ID)VSI_ParamGetInt(iParam));
      return VMK_NOT_FOUND;
   }

   if (World_IsVMMWorld(world)) {
      for (i = 0; i < World_VMMGroup(world)->memberCount; i++) {
         status = VSI_ParamListAddInt(instanceListOut, World_VMMGroup(world)->members[i]);
         if (status != VMK_OK) {
            Warning("Input list not long enough: %#x", status);
            break;
         }
      }
   }

   World_Release(world);

   return status;
}

/*
 * Bogus test function
 */
VMK_ReturnStatus
World_VsiGetGroupMember(VSI_NodeID nodeID, VSI_ParamList *instanceArgs, 
                        WorldVsiGroupMember *out)
{
   VSI_Param *wParam, *gParam;
   World_Handle *world;

   LOG(0, "Here!");
   if (VSI_ParamListUsedCount(instanceArgs) != 2) {
      Warning("Incorrect # of instance args passed: %d",
              VSI_ParamListUsedCount(instanceArgs));
      return VMK_BAD_PARAM;
   }

   wParam = VSI_ParamListGetParam(instanceArgs, 0);
   
   if (VSI_ParamGetType(wParam) != VSI_PARAM_INT64) {
      Warning("Non-int instance param");
      return VMK_BAD_PARAM;
   }

   gParam = VSI_ParamListGetParam(instanceArgs, 1);
   
   if (VSI_ParamGetType(gParam) != VSI_PARAM_INT64) {
      Warning("Non-int instance param");
      return VMK_BAD_PARAM;
   }

   world = World_Find((World_ID)VSI_ParamGetInt(gParam));
   if (!world) {
      LOG(0, "World not found %d", (World_ID)VSI_ParamGetInt(wParam));
      return VMK_NOT_FOUND;
   }

   memset(out, 0, sizeof *out);
   snprintf(out->name, sizeof out->name, "%s", world->worldName);
   out->leaderID = World_VMMGroup(world)->vmmLeader->worldID;
               
   World_Release(world);

   return VMK_OK;
}
