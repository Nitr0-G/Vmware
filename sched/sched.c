/* **********************************************************
 * Copyright 1999 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * sched.c --
 *
 *	World resource scheduling.
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "vmkernel.h"
#include "world.h"
#include "prda.h"
#include "parse.h"
#include "util.h"
#include "net.h"
#include "event.h"
#include "memsched.h"
#include "memsched_int.h"
#include "numasched.h"
#include "cpusched.h"
#include "cpusched_int.h"
#include "sched.h"
#include "sched_int.h"
#include "vmmem.h"
#include "memalloc.h"
#include "memmap.h"

#define LOGLEVEL_MODULE Sched
#include "log.h"

/*
 * Compile-time options
 */

// general debugging
#define	SCHED_DEBUG			(vmx86_debug && vmx86_devel)
#define	SCHED_DEBUG_VERBOSE		(0)

// targeted debugging
#define SCHED_DISABLE_INLINING		(SCHED_DEBUG)

/*
 * Constants
 */

// IRQ level
#define	SCHED_IRQL			(SP_IRQL_KERNEL)

// common group name prefixes
#define	SCHED_GROUP_ANON_PREFIX		"anon."
#define SCHED_GROUP_VM_PREFIX		"vm."


/*
 * Types
 */

#define LIST_ITEM_TYPE Sched_Group*
#define LIST_NAME SchedGroupArray
#define LIST_SIZE SCHED_GROUPS_MAX
#include "staticlist.h"

#define	LIST_ITEM_TYPE Sched_Node*
#define	LIST_NAME SchedNodeArray
#define	LIST_SIZE SCHED_NODES_MAX
#include "staticlist.h"

typedef struct {
   // for mutual exclusion
   SP_SpinLockIRQ lock;

   // groups
   Sched_Group groupTable[SCHED_GROUPS_MAX];
   SchedGroupArray groups;
   Sched_Group *groupRoot;

   // nodes
   Sched_Node nodeTable[SCHED_NODES_MAX];
   SchedNodeArray nodes;
   Sched_Node *nodeRoot;

   // procfs nodes
   Proc_Entry procSchedDir;		// /proc/vmware/sched/
   Proc_Entry procGroups;		// /proc/vmware/sched/groups
} SchedTree;

typedef struct {
   const char *name;
   Sched_GroupID groupID;
   Sched_GroupID parentID;
   Sched_Alloc cpu;
   Sched_Alloc mem;
} SchedPredefinedGroup;

/*
 * Globals
 */

static SchedTree  schedTree;

// predefine sched groups
#define SCHED_PREDEFINED_GROUPS(_id, _pid,                               \
                                _cpuMin, _cpuMax, _cpuShares,            \
                                _memMin, _memMax, _memShares, _memMinLimit, _memHardMax) \
   {                                                                     \
      SCHED_GROUP_NAME_##_id,                                            \
      SCHED_GROUP_ID_##_id,                                              \
      SCHED_GROUP_ID_##_pid,                                             \
      {                                                                  \
         _cpuMin, _cpuMax, _cpuShares, 0, 0, SCHED_UNITS_PERCENT         \
      },                                                                 \
      {                                                                  \
         _memMin, _memMax, _memShares, _memMinLimit, _memHardMax,        \
	 SCHED_UNITS_PAGES                                               \
      },                                                                 \
   },

static SchedPredefinedGroup schedPredefinedGroups[] = {
   SCHED_PREDEFINED_GROUP_LIST   
};

#undef SCHED_PREDEFINED_GROUPS

/*
 * Function inlining
 */

#if (SCHED_DISABLE_INLINING)
#undef	INLINE
#define INLINE
#endif

/*
 * Macros
 */

// iterator macros
#define	FORALL_GROUPS(_g) \
	do { \
	  uint32 _gi; \
	  ASSERT(Sched_TreeIsLocked()); \
	  for (_gi = 0; _gi < schedTree.groups.len; _gi++) { \
	    Sched_Group* _g = schedTree.groups.list[_gi];
#define	GROUPS_DONE }} while(0) 

/*
 * Local forward declarations
 */

static void SchedTreeInit(void);
static void SchedGroupInit(void);
static VMK_ReturnStatus SchedSetupVMGroup(struct World_Handle *world,
                                          const Sched_ClientConfig *config);



/*
 * Operations
 */

/*
 *----------------------------------------------------------------------
 *
 * Sched_ConfigSetCpuAffinity --
 *
 *	Sets "config" cpu affinity to "affinity".
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
Sched_ConfigSetCpuAffinity(Sched_ClientConfig *config, CpuMask affinity)
{
   uint32 i;
   for (i = 0; i < CPUSCHED_VSMP_VCPUS_MAX; i++) {
      config->cpu.vcpuAffinity[i] = affinity;
   }
}   


/*
 *----------------------------------------------------------------------
 *
 * Sched_ConfigSetCpuMinPct --
 *
 *	Sets "config" cpu minimum guaranteed rate to "minPercent".
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
Sched_ConfigSetCpuMinPct(Sched_ClientConfig *config, int32 minPercent)
{
   config->cpu.alloc.min = minPercent;
   config->cpu.alloc.units = SCHED_UNITS_PERCENT;
}   


/*
 *----------------------------------------------------------------------
 *
 * Sched_ConfigInit --
 *
 *	Initializes "config" to default values.
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
Sched_ConfigInit(Sched_ClientConfig *config, const char *groupName)
{
   // zero state 
   memset(config, 0, sizeof(Sched_ClientConfig));

   strncpy(config->group.groupName, groupName, SCHED_GROUP_NAME_LEN);
   config->group.groupName[SCHED_GROUP_NAME_LEN - 1] = '\0';
   config->group.createContainer = FALSE;

   // default cpu config (normal uni)
   config->cpu.numVcpus = 1;
   config->cpu.alloc.shares = CPUSCHED_SHARES_NORMAL(1);
   config->cpu.htSharing = CPUSCHED_HT_SHARE_ANY;
   Sched_ConfigSetCpuAffinity(config, CPUSCHED_AFFINITY_NONE);

   // default mem config (none)
   config->group.mem.shares = SCHED_CONFIG_NONE;
}


/*
 *----------------------------------------------------------------------
 *
 * SchedAddNetFilter --
 *
 *      Attach network filter specified by "nfClass" and "nfArgs" to
 *	the specified "world".  Returns VMK_OK if successful, otherwise
 *	error code.
 *
 * Results:
 *      Returns VMK_FAILURE
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
SchedAddNetFilter(World_Handle *world, char *nfClass, char *nfArgs)
{
#ifdef ESX3_NETWORKING_NOT_DONE_YET
   return VMK_FAILURE;
#else 
#error "implement netfilter"
#endif
}

/*
 *----------------------------------------------------------------------
 *
 * SchedClientGroupInvalidate --
 *
 *      Sets "clientGroup" to invalid state.
 *
 * Results:
 *      Sets "clientGroup" to invalid state.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
SchedClientGroupInvalidate(Sched_ClientGroup *clientGroup)
{
   clientGroup->node = NULL;
   clientGroup->groupID = SCHED_GROUP_ID_INVALID;
   Sched_GroupPathInvalidate(&clientGroup->path);
   clientGroup->cpuValid = FALSE;
   clientGroup->memValid = FALSE;
}

/*
 *----------------------------------------------------------------------
 *
 * Sched_AddRunning --
 *
 *      The currently-running world is added to the scheduler.
 *
 *      XXX Field "cpuConfig" is copied in Sched_WorldInit() 
 *          when the world was created. This is used for initializing
 *          cpusched.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The currently-running world is added to the scheduler.
 *
 *----------------------------------------------------------------------
 */
void
Sched_AddRunning(void)
{
   World_Handle *world = MY_RUNNING_WORLD;
   VMK_ReturnStatus status;

   // sanity checks
   ASSERT(world->inUse);
   ASSERT(World_IsSYSTEMWorld(world));
   ASSERT(world->sched.cpuConfig.numVcpus > 0);

   // debugging
   VmLog(world->worldID, "name='%s'", world->worldName);

   // add running world to cpu scheduler
   status = CpuSched_Add(world, &world->sched.cpuConfig, TRUE);
   ASSERT_NOT_IMPLEMENTED(status == VMK_OK);
}

/*
 *-----------------------------------------------------------------------------
 *
 * SchedGetShares --
 *
 *      Returns the number of shares associated with the given share config
 *      for a vm with "numVcpus" vcpus. This converts any special share
 *      values (e.g. low, normal, high) to their appropriate values.
 *
 * Results:
 *      Returns the number of shares associated with the given share config
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
static int32
SchedGetShares(uint8 numVcpus, int32 shareConfig)
{

   // replace special cpu shares config with corresponding value
   if (SCHED_CONFIG_SHARES_SPECIAL(shareConfig)) {
      switch (shareConfig) {
      case SCHED_CONFIG_SHARES_LOW:
         return (CPUSCHED_SHARES_LOW(numVcpus));
      case SCHED_CONFIG_SHARES_HIGH:
         return (CPUSCHED_SHARES_HIGH(numVcpus));
      case SCHED_CONFIG_SHARES_NORMAL:
      default:
         return (CPUSCHED_SHARES_NORMAL(numVcpus));
      }
   } else {
      return shareConfig;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * Sched_Add --
 *
 *      The specified "world" is added to the cpu scheduler
 *	and the specified start function "startFunc" and start
 *	parameter "startData".
 *
 *      XXX Field "cpuConfig" is copied in Sched_WorldInit() 
 *          when the world was created. This specifies cpu
 *          scheduler configuration.
 *
 * Results:
 *      Returns VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 *      The specified "world" is added to the scheduler.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Sched_Add(World_Handle *world,
          CpuSched_StartFunc startFunc,
	  void *startData)
{
   VMK_ReturnStatus status;
   
   // initialize
   List_InitElement(&world->sched.links);
   world->sched.cpu.startFunc = startFunc;
   world->sched.cpu.startData = startData;

   // add world to cpu scheduler, fail if unable
   status = CpuSched_Add(world, &world->sched.cpuConfig, FALSE);
   if (status != VMK_OK) {
      // issue warning, fail
      VmWarn(world->worldID,
             "unable to add to scheduler: %s",
             VMK_ReturnStatusToString(status));
      return (status);
   }

   // find a home NUMA node, if necessary
   // note that this may change affinity settings, but that's ok
   if (NUMA_GetNumNodes() > 1) {
      NUMASched_SetInitialHomeNode(world);
   }
   
   // attempt immediate reallocation
   CpuSched_Reallocate();

   // debugging
   VMLOG(1, world->worldID, "adding '%s': done", world->worldName);

   // everything OK
   return (VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * Sched_Remove --
 *
 *      Attempts to remove "world" from scheduler.  If "world" is
 *	currently running, then an error is returned.
 *      This function is idempotent, i.e. multiple invocations of
 *      function will have the same effect as being called once.
 *
 * Results:
 *      Returns VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 *      Updates scheduler state associated with "world".
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Sched_Remove(World_Handle *world)
{
   VMK_ReturnStatus status;

   // debugging
   VmLog(world->worldID, "name='%s'", world->worldName);

   // remove world from cpu scheduler (must be idempotent)
   status = CpuSched_Remove(world);

   return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * SchedCpuAllocNormalize --
 *
 *	Normalize cpu config values to be usable by sched nodes.
 *      Convert special share configs (low, normal, high) and
 *      replace other missing params with default values.
 *
 * Results:
 *      "alloc" changed.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
SchedCpuAllocNormalize(Sched_Alloc *alloc, int numVcpus)
{
   alloc->shares = SchedGetShares(numVcpus, alloc->shares);
   if (alloc->min < 0) {
      alloc->min = 0;
   }
   if (alloc->max < 0) {
      alloc->max = CPUSCHED_ALLOC_MAX_NONE;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * SchedConfigNormalize --
 *
 *	Normalize config values to be usable by sched nodes.
 *
 * Results:
 *      "config" changed.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
SchedConfigNormalize(Sched_ClientConfig *config)
{
   int i;

   if (config->group.createContainer) {
      SchedCpuAllocNormalize(&config->group.cpu, config->cpu.numVcpus);
   }

   SchedCpuAllocNormalize(&config->cpu.alloc, config->cpu.numVcpus);

   for (i = 0; i < CPUSCHED_VSMP_VCPUS_MAX; i++) {
      // 0 is used to indicate unconstrained affinnity 
      if (config->cpu.vcpuAffinity[i] == 0) {
         config->cpu.vcpuAffinity[i] = CPUSCHED_AFFINITY_NONE;
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * Sched_WorldInit --
 *
 *      Initialize specific scheduling state associated with "world".
 *
 * Results:
 *      Returns VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Sched_WorldInit(World_Handle *world, World_InitArgs *args)
{
   VMK_ReturnStatus status;
   Sched_ClientConfig *config = args->sched;

   // sanity checks
   ASSERT(world->inUse);
   ASSERT(config->cpu.numVcpus > 0);

   SchedConfigNormalize(config);

   // save sched config to be used by cpusched add.
   world->sched.cpuConfig = config->cpu;

   if (World_IsGroupLeader(world)) {
      // Perform sched group initialization for world group leader
      status = Sched_WorldGroupInit(world, args);
      if (status != VMK_OK) {
         return (status);
      }
   }

   // change group config if a new VM is created
   if (World_IsVmmLeader(world)) {
      VMLOG(1, world->worldID, "setup vm group");
      status = SchedSetupVMGroup(world, config);
      if (status != VMK_OK) {
         return (status);
      }
   }

   // initialize group state
   SchedClientGroupInvalidate(&world->sched.group);

   // join group, if vsmp leader
   if (CpuSched_IsVsmpLeader(world)) {
      Sched_GroupID groupID = world->group->schedGroupID;
      ASSERT(groupID != SCHED_GROUP_ID_INVALID);
      status = Sched_JoinGroup(world, groupID);
      ASSERT(status == VMK_OK);
      if (status != VMK_OK) {
         VmWarn(world->worldID, "unable to join group");
         return (status);
      }
   }

   // add network filter for world, if any
   SchedAddNetFilter(world, config->netFilterClass, config->netFilterArgs);

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Sched_WorldCleanup --
 *
 *      Cleanup various scheduling state associated with "world".
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
Sched_WorldCleanup(World_Handle *world)
{
   if (World_IsVmmLeader(world)) {
      Sched_Group *group;

      /*
       * Since the VM is terminating restore container group 
       * allocations to the state before the VM was powered-on.
       */
      Sched_TreeLock();
      group = Sched_TreeLookupGroup(world->group->schedGroupID);
      MemSched_CleanupVmGroup(world, group);
      Sched_TreeUnlock();
   }

   // leave scheduler group, if vsmp leader
   if (CpuSched_IsVsmpLeader(world)) {
      Sched_GroupID groupID = world->sched.group.groupID;
      ASSERT(groupID != SCHED_GROUP_ID_INVALID);
      if (groupID != SCHED_GROUP_ID_INVALID) {
         Sched_LeaveGroup(world);
      }
   }

   // cleanup cpu scheduler state
   CpuSched_WorldCleanup(world);
}


/*
 *----------------------------------------------------------------------
 *
 * Sched_Init --
 *  
 *	Initializes the scheduler groups tree. Initializes CpuSched, 
 *	MemSched, and NUMASched scheduler modules. Sets initial 
 *	running world to "console".  Uses "cellSize" as the preferred 
 *	CpuSched cell size.
 *
 * Results:
 *      None. 
 *
 * Side effects:
 *      Modifies scheduler state.
 *
 *----------------------------------------------------------------------
 */
void
Sched_Init(uint32 cellSize)
{
   // initialize scheduler tree
   SchedTreeInit();

   // initialize cpu and memory schedulers
   CpuSched_Init(&schedTree.procSchedDir, cellSize);
   MemSched_Init(&schedTree.procSchedDir);

   // initizlied predefined sched groups
   SchedGroupInit();

   // compute initial cpu allocations
   CpuSched_RequestReallocate();
   CpuSched_Reallocate();

   // finish event initialization
   Event_LateInit(&schedTree.procSchedDir);

   // initialize NUMA scheduler, if necessary
   if (NUMA_GetNumNodes() > 1) {
      NUMASched_Init(&schedTree.procSchedDir);
   }
}



/*
 * Grouping Operations
 */

/*
 *----------------------------------------------------------------------
 *
 * Sched_TreeLock --
 *
 *      Acquire exclusive access to scheduler tree state.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      Lock for scheduler tree state is acquired.
 *
 *----------------------------------------------------------------------
 */
INLINE void
Sched_TreeLock(void)
{
   (void) SP_LockIRQ(&schedTree.lock, SCHED_IRQL);
}

/*
 *----------------------------------------------------------------------
 *
 * Sched_TreeUnlock --
 *
 *      Releases exclusive access to scheduler tree state.  Sets
 *      the IRQL level to the previous value associated with the lock.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Lock for scheduler tree state is released.
 *
 *----------------------------------------------------------------------
 */
INLINE void
Sched_TreeUnlock(void)
{
   ASSERT_NO_INTERRUPTS();
   SP_UnlockIRQ(&schedTree.lock, SP_GetPrevIRQ(&schedTree.lock));
}

/*
 *----------------------------------------------------------------------
 *
 * Sched_TreeIsLocked --
 *
 *      Returns TRUE iff scheduler tree state is locked.
 *
 * Results:
 *      Returns TRUE iff scheduler tree state is locked.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
INLINE Bool
Sched_TreeIsLocked(void)
{
   return(SP_IsLockedIRQ(&schedTree.lock));
}


/*
 *----------------------------------------------------------------------
 *
 * SchedDeallocateGroup --
 *
 *	Marks group "g" invalid, allowing its slot to be reused.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
SchedDeallocateGroup(Sched_Group *g)
{
   g->groupID = SCHED_GROUP_ID_INVALID;
}

/*
 *----------------------------------------------------------------------
 *
 * SchedGroupIsLeaf --
 *
 *	Return if the group is a leaf group.
 *
 * Results:
 *	TRUE if the group is a leaf group.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
SchedGroupIsLeaf(const Sched_Group * g)
{
   return (g->flags & SCHED_GROUP_IS_LEAF) != 0;
}

/*
 *----------------------------------------------------------------------
 *
 * SchedGroupIsPredefined --
 *
 *	Return if the group is a predefined group.
 *
 * Results:
 *	TRUE if the group is a predefined group.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
SchedGroupIsPredefined(const Sched_Group * g)
{
   return (g->flags & SCHED_GROUP_IS_PREDEFINED) != 0;
}

/*
 *----------------------------------------------------------------------
 *
 * SchedFindUnusedGroupSlot --
 *
 *	Attempts to allocate storage for new group data structure.
 *	Returns new group, or NULL if unable to allocate.  Caller
 *	must hold scheduler tree lock.
 *
 * Results:
 *	Attempts to allocate storage for new group data structure.
 *	Returns new group, or NULL if unable to allocate.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static Sched_Group *
SchedFindUnusedGroupSlot(void)
{
   Sched_Group *unused = NULL;
   uint32 i;

   // search group table for unused slot
   ASSERT(Sched_TreeIsLocked());
   for (i = 0; i < SCHED_GROUPS_MAX; i++) {
      Sched_Group *g = &schedTree.groupTable[i];
      if (g->groupID == SCHED_GROUP_ID_INVALID) {
         // prefer unused slot with smallest id, to keep ids compact
         if ((unused == NULL) || (g->groupNextID < unused->groupNextID)) {
            unused = g;
         }
      }
   }

   return(unused);
}

/*
 *----------------------------------------------------------------------
 *
 * SchedDeallocateNode --
 *
 *	Marks node "n" invalid, allowing its slot to be reused.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
SchedDeallocateNode(Sched_Node *n)
{
   n->nodeType = SCHED_NODE_TYPE_INVALID;
}

/*
 *----------------------------------------------------------------------
 *
 * SchedFindUnusedNodeSlot --
 *
 *	Attempts to allocate storage for new node data structure.
 *	Returns new node, or NULL if unable to allocate.  Caller
 *	must hold scheduler tree lock.
 *
 * Results:
 *	Attempts to allocate storage for new node data structure.
 *	Returns new node, or NULL if unable to allocate.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static Sched_Node *
SchedFindUnusedNodeSlot(void)
{
   uint32 i;

   // search nodes table for unused slot
   ASSERT(Sched_TreeIsLocked());
   for (i = 0; i < SCHED_NODES_MAX; i++) {
      Sched_Node *n = &schedTree.nodeTable[i];
      if (SCHED_NODE_IS_INVALID(n)) {
         return(n);
      }
   }

   // no unused slots
   return(NULL);
}

/*
 *----------------------------------------------------------------------
 *
 * SchedLookupGroupByID --
 *
 *	Returns the group associated with "id", or NULL
 *	if no such group exists.  Caller must hold
 *	scheduler tree lock.
 *
 * Results:
 *	Returns the group associated with "id", or NULL
 *	if no such group exists.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Sched_Group *
SchedLookupGroupByID(Sched_GroupID id)
{
   ASSERT(Sched_TreeIsLocked());
   if (id == SCHED_GROUP_ID_INVALID) {
      return(NULL);
   } else {
      Sched_Group *group = &schedTree.groupTable[id & SCHED_GROUPS_MASK];
      if ((group->groupID == id) && (!group->removed)) {
         return(group);
      } else {
         return(NULL);
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * SchedLookupGroupByName --
 *
 *	Returns the group associated with "name", or NULL
 *	if no such group exists.  Caller must hold scheduler
 *	tree lock.
 *
 * Results:
 *	Returns the group associated with "name", or NULL
 *	if no such group exists.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static Sched_Group *
SchedLookupGroupByName(const char *name)
{
   ASSERT(Sched_TreeIsLocked());

   // no match for invalid name
   if (name == NULL) {
      return(NULL);
   }

   // search for match
   FORALL_GROUPS(g) {
      if (strcmp(g->groupName, name) == 0) {
         return(g);
      }
   } GROUPS_DONE;

   // not found
   return(NULL);
}

/*
 *----------------------------------------------------------------------
 *
 * SchedNodeDepth --
 *
 *	Returns the depth of "node" in the scheduler tree,
 *	where the root has depth zero.  Caller must hold
 *	scheduler tree lock.
 *
 * Results:
 *	Returns the depth of "node" in the scheduler tree.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static uint32
SchedNodeDepth(const Sched_Node *node)
{
   Sched_Node *n;
   uint32 depth;

   // traverse leaf-to-root path
   depth = 0;
   for (n = node->parent; n != NULL; n = n->parent) {
      depth++;
   }
   return(depth);
}

/*
 *----------------------------------------------------------------------
 *
 * SchedGroupParent --
 *
 *	Returns the parent group associated with "group", or NULL if
 *	no parent exists.  Caller must hold scheduler tree lock.
 *
 * Results:
 *	Returns the parent group associated with "group", or NULL if
 *	no parent exists.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static Sched_Group *
SchedGroupParent(const Sched_Group *group)
{
   Sched_Node *parentNode;

   // sanity check
   ASSERT(Sched_TreeIsLocked());

   // fail if group not valid
   if ((group->node == NULL) || (!SCHED_NODE_IS_GROUP(group->node))) {
      return(NULL);
   }

   // fail if parent group not valid
   parentNode = group->node->parent;
   if ((parentNode == NULL) || (!SCHED_NODE_IS_GROUP(parentNode))) {
      return(NULL);
   }

   // valid parent group
   return(parentNode->u.group);
}

/*
 *----------------------------------------------------------------------
 *
 * SchedAddGroupInt --
 *
 *	Attempts to create new scheduler group with specified
 *	"name" and "parent" group.  If "name" is NULL, a unique
 *	name will be generated automatically.  Caller must hold
 *	scheduler tree lock.
 *
 * Results:
 *	Returns VMK_OK if successful, otherwise error code.
 *	If successful, sets "group" to newly-added scheduler group.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
SchedAddGroupInt(const char *name,
                 Sched_Group *parent,
                 Sched_Group **group)
{
   Sched_GroupID groupNextID;
   Sched_Group *g;
   Sched_Node *n;

   // sanity checks
   ASSERT(Sched_TreeIsLocked());
   if (parent != NULL) {
      ASSERT(parent->groupID != SCHED_GROUP_ID_INVALID);
      ASSERT(SCHED_NODE_IS_GROUP(parent->node));
   }

   // default
   *group = NULL;

   // debugging
   if (SCHED_DEBUG) {
      LOG(1, "trying to create group '%s' with parent='%s'",
          (name == NULL) ? "[anon]" : name,
          (parent == NULL) ? "[NULL]" : parent->groupName);
   }

   if (parent != NULL) {
      // fail if adding group would exceed max depth
      if (SchedNodeDepth(parent->node) >= SCHED_NODE_DEPTH_MAX) {
      VmWarn(MY_RUNNING_WORLD->worldID, 
	     "adding group '%s' under group '%s' will exceed max tree depth", 
	     name, parent->groupName);
         return(VMK_LIMIT_EXCEEDED);
      }

      // fail if attempting to add group under a leaf group
      if (SchedGroupIsLeaf(parent)) {
         return(VMK_BAD_PARAM);
      }
      ASSERT(!(parent->flags & SCHED_GROUP_IS_VM));
   }


   // allocate slots
   if ((g = SchedFindUnusedGroupSlot()) == NULL) {
      return(VMK_LIMIT_EXCEEDED);
   }
   if ((n = SchedFindUnusedNodeSlot()) == NULL) {
      return(VMK_LIMIT_EXCEEDED);
   }

   // initialize group slot, preserving next group ID
   groupNextID = g->groupNextID;
   memset(g, 0, sizeof(Sched_Group));
   g->groupID = SCHED_GROUP_ID_INVALID;
   g->groupNextID = groupNextID;

   // initialize node slot
   memset(n, 0, sizeof(Sched_Node));
   n->nodeType = SCHED_NODE_TYPE_INVALID;

   // initialize group name
   if (name == NULL) {
      // generate unique anonymous name automatically
      snprintf(g->groupName,
               SCHED_GROUP_NAME_LEN,
               SCHED_GROUP_ANON_PREFIX "%u",
               g->groupNextID);
   } else {
      // prevent conflicts with anonymous names
      if (strcmp(name, SCHED_GROUP_ANON_PREFIX) == 0) {
         return(VMK_BAD_PARAM);
      }
      // use specified name
      strncpy(g->groupName, name, SCHED_GROUP_NAME_LEN);
   }
   g->groupName[SCHED_GROUP_NAME_LEN - 1] = '\0';

   // ensure name unique
   if (SchedLookupGroupByName(g->groupName) != NULL) {
      ASSERT(name != NULL);
      return(VMK_EXISTS);
   }

   // set group id
   g->groupID = g->groupNextID;
   g->groupNextID += SCHED_GROUPS_MAX;

   // attach enclosing node
   g->node = n;
   
   // add to group list
   ASSERT(schedTree.groups.len < SCHED_GROUPS_MAX);
   SchedGroupArrayAdd(&schedTree.groups, g);

   // validate node
   n->nodeType = SCHED_NODE_TYPE_GROUP;
   n->u.group = g;

   // add to node list
   ASSERT(schedTree.nodes.len < SCHED_NODES_MAX);
   SchedNodeArrayAdd(&schedTree.nodes, n);

   // attach group node into hierarchy
   if (parent == NULL) {
      n->parent = NULL;
   } else {
      n->parent = parent->node;
      Sched_MemberArrayAdd(&parent->members, n);
   }

   // initialize resource scheduler state
   CpuSched_GroupStateInit(&g->cpu);
   MemSched_GroupStateInit(&g->mem);

   // debugging
   if (SCHED_DEBUG) {
      LOG(1, "created group: id=%u, name='%s'",
          g->groupID, g->groupName);
   }

   // succeessful
   *group = g;
   return(VMK_OK);
}


/*
 *----------------------------------------------------------------------
 *
 * Sched_AddGroup --
 *
 *	Attempts to create new scheduler group with specified
 *	"name" and group identified by "parentID".  
 *	NULL, a unique name will be generated automatically.
 *
 * Results:
 *	Returns VMK_OK if successful, otherwise error code.
 *	If successful, sets "groupID" to newly-added scheduler group id.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Sched_AddGroup(const char *name,
               Sched_GroupID parentID,
               Sched_GroupID *groupID)
{
   Sched_Group *parentGroup, *group;
   VMK_ReturnStatus status;

   *groupID = SCHED_GROUP_ID_INVALID;

   Sched_TreeLock();

   // lookup parent, fail if unable
   parentGroup = SchedLookupGroupByID(parentID);
   if (parentGroup == NULL) {
      Sched_TreeUnlock();
      return(VMK_NOT_FOUND);
   }

   // create group
   status = SchedAddGroupInt(name, parentGroup, &group);
   if (status == VMK_OK) {
      *groupID = group->groupID;
   }

   // debugging
   if (SCHED_DEBUG && (status != VMK_OK)) {
      LOG(0, "creation of group '%s' : status %s", 
          name, VMK_ReturnStatusToString(status));
   }

   Sched_TreeUnlock();

   return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * SchedReapGroup --
 *
 *	Deallocates storage associated with "group".  Requires
 *	that "group" has already been removed, and that its
 *	reference count is zero.  Caller must hold scheduler
 *	tree lock.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
SchedReapGroup(Sched_Group *group)
{
   // debugging
   if (SCHED_DEBUG) {
      LOG(0, "reaping group '%s'", group->groupName);
   }

   // sanity checks
   ASSERT(Sched_TreeIsLocked());
   ASSERT(group->refCount == 0);
   ASSERT(group->removed);

   // cleanup resource scheduler state
   CpuSched_GroupStateCleanup(&group->cpu);
   MemSched_GroupStateCleanup(&group->mem);

   // reclaim group structure
   SchedDeallocateGroup(group);
}

/*
 *----------------------------------------------------------------------
 *
 * SchedRemoveGroupInt --
 *
 *	Attempts to remove existing "group" from scheduler tree.
 *	Caller must hold scheduler tree lock.
 *
 * Results:
 *	Returns VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
SchedRemoveGroupInt(Sched_Group *group)
{
   Sched_Group *parentGroup;
   Sched_Node *node;

   // debugging
   if (SCHED_DEBUG) {
      LOG(0, "trying to remove group '%s'", group->groupName);
   }

   // fail if attempting to remove a predefined group
   if (SchedGroupIsPredefined(group)) {
      VmWarn(MY_RUNNING_WORLD->worldID, 
	     "predefined group '%s' cannot be removed", group->groupName);
      return(VMK_FAILURE);
   }

   // initialize
   node = group->node;

   // sanity checks
   ASSERT(Sched_TreeIsLocked());
   ASSERT(node != NULL);
   ASSERT(SCHED_NODE_IS_GROUP(node));
   ASSERT(!group->removed);
   if ((node == NULL) || (!SCHED_NODE_IS_GROUP(node))) {
      return(VMK_FAILURE);
   }

   // fail if group contains any members
   if (group->members.len > 0) {
      return(VMK_BUSY);
   }

   // find parent group
   parentGroup = SchedGroupParent(group);
   ASSERT(parentGroup != NULL);
   if (parentGroup == NULL) {
      return(VMK_FAILURE);
   }

   // detach group node from hierarchy
   Sched_MemberArrayRemoveByData(&parentGroup->members, node);
   node->parent = NULL;

   // remove from node list
   SchedNodeArrayRemoveByData(&schedTree.nodes, node);

   // remove from group list
   SchedGroupArrayRemoveByData(&schedTree.groups, group);

   // Update internal memory resource related state in the scheduler tree
   MemSched_SubTreeChanged(parentGroup);

   // reclaim node structure
   SchedDeallocateNode(node);
   group->node = NULL;

   // mark group removed
   group->removed = TRUE;

   // reap group if no outstanding references
   if (group->refCount == 0) {
      SchedReapGroup(group);
   }

   // group successfully removed
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * Sched_RemoveGroup --
 *
 *	Attempts to remove existing group identified by "id" from
 *	scheduler tree.
 *
 * Results:
 *	Returns VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Sched_RemoveGroup(Sched_GroupID id)
{
   VMK_ReturnStatus status;
   Sched_Group *group;

   Sched_TreeLock();
   group = SchedLookupGroupByID(id);
   if (group == NULL) {
      Sched_TreeUnlock();
      return(VMK_NOT_FOUND);
   }
   status = SchedRemoveGroupInt(group);
   Sched_TreeUnlock();

   // debugging
   if (SCHED_DEBUG) {
      LOG(0, "removed group id=%u: status %s",
          id, VMK_ReturnStatusToString(status));
   }

   return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * SchedLookupGroupPath --
 *
 *	Sets "path" to the root-to-leaf ancestor path associated
 *	with "vmNode".  Requires that "vmNode" is a VM node.
 *	Caller must hold scheduler tree lock.
 *
 * Results:
 *	Sets "path" to the root-to-leaf ancestor path associated
 *	with "vmNode".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
SchedLookupGroupPath(const Sched_Node *vmNode, Sched_GroupPath *path)
{
   Sched_GroupID reversePath[SCHED_GROUP_PATH_LEN];
   Sched_Node *n;
   uint32 len, i;

   // sanity checks
   ASSERT(Sched_TreeIsLocked());   
   ASSERT(SCHED_NODE_IS_VM(vmNode));

   // traverse leaf-to-root
   len = 0;
   for (n = vmNode->parent; n != NULL; n = n->parent) {
      Sched_Group *group = n->u.group;
      ASSERT(SCHED_NODE_IS_GROUP(n));
      reversePath[len++] = group->groupID;
   }

   // set root-to-leaf path
   for (i = 0; i < len; i++) {
      uint32 index = (len - i) - 1;
      path->level[i] = reversePath[index];
   }

   // add path terminator
   path->level[len] = SCHED_GROUP_ID_INVALID;

   // sanity check
   ASSERT(path->level[0] == SCHED_GROUP_ID_ROOT);
}
                      
/*
 *----------------------------------------------------------------------
 *
 * SchedRedoPath --
 * 
 *	Recursively decends down the tree, starting at "group." For
 *	each VM node that is encountered the root-to-leaf path is
 *	recomputed.
 *
 * Results:
 * 	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
SchedRedoPath(Sched_Group *group, World_ID *worldArray, int worldArrayIndex)
{
   int i;

   ASSERT(Sched_TreeIsLocked());

   for (i = 0; i < group->members.len; i++) {
      Sched_Node *node = group->members.list[i];

      if (SCHED_NODE_IS_GROUP(node)) {
         SchedRedoPath(node->u.group, worldArray, worldArrayIndex);
      } else if (SCHED_NODE_IS_VM(node)) {
         World_Handle *world = node->u.world;
         Sched_ClientGroup *clientGroup = &world->sched.group;

         SchedLookupGroupPath(node, &clientGroup->path);

	 ASSERT(worldArrayIndex < MAX_WORLDS);
	 ASSERT(worldArray[worldArrayIndex] == INVALID_WORLD_ID);
	 ASSERT(world->worldID != INVALID_WORLD_ID);

	 worldArray[worldArrayIndex++] = world->worldID;
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * SchedMoveGroupInt --
 * 
 *	Internal routine that implements the logic for relocating the
 *	sub-tree specified by "group" to a new position in the 
 *	scheduler tree hierarchy. Caller must hold scheduler tree lock.
 *
 *	"worldArray" will be used provide the caller with the list
 *	of worlds that were affected by the relocation of the sub-tree.
 *
 * Results:
 *	Returns VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
SchedMoveGroupInt(Sched_Group *group, 
		  Sched_Group *parentGroup,
		  Sched_Group *newParentGroup,
		  World_ID *worldArray)
{
   Sched_Node *node;
   VMK_ReturnStatus status;

   ASSERT(Sched_TreeIsLocked());
   ASSERT(SCHED_NODE_IS_GROUP(group->node));
   ASSERT(SCHED_NODE_IS_GROUP(parentGroup->node));
   ASSERT(SCHED_NODE_IS_GROUP(newParentGroup->node));
   ASSERT(parentGroup != newParentGroup);

   // Fail if moving target group would exceed max tree depth
   if (SchedNodeDepth(newParentGroup->node) >= SCHED_NODE_DEPTH_MAX) {
      VmWarn(MY_RUNNING_WORLD->worldID, 
	     "moving group '%s' under group '%s' will exceed max tree depth", 
	     group->groupName, newParentGroup->groupName);
      return (VMK_LIMIT_EXCEEDED);
   }

   // Fail if attempting to move a predefined group
   if (SchedGroupIsPredefined(group)) {
      VmWarn(MY_RUNNING_WORLD->worldID, 
	     "predefined group '%s' cannot be moved", group->groupName);
      return (VMK_BAD_PARAM);
   }

   // Fail if attempting to move under a leaf group
   if (SchedGroupIsLeaf(newParentGroup)) {
      VmWarn(MY_RUNNING_WORLD->worldID, 
	     "cannot move group '%s' under memsched client group '%s'", 
	     group->groupName, newParentGroup->groupName);
      return (VMK_BAD_PARAM);
   }
   ASSERT(!(newParentGroup->flags & SCHED_GROUP_IS_VM));

   // Fail if attempting to move under the "UW Nursery" group
   if (newParentGroup == SchedLookupGroupByName(SCHED_GROUP_NAME_UW_NURSERY)) {
      VmWarn(MY_RUNNING_WORLD->worldID, 
	     "cannot move group '%s' under system group '%s'", 
	     group->groupName, newParentGroup->groupName);
      return (VMK_BAD_PARAM);
   }

   // Fail if attempting to move target group under a direct descendant
   node = newParentGroup->node;
   for (node = node->parent; node != NULL; node = node->parent) {
      ASSERT(SCHED_NODE_IS_GROUP(node));
      Sched_Group *tmpGroup = node->u.group;
      if (tmpGroup == group) {
         VmWarn(MY_RUNNING_WORLD->worldID, 
	        "cannot move group '%s' under direct descendant '%s'", 
	        group->groupName, newParentGroup->groupName);
         return (VMK_BAD_PARAM);
      }
   }

   node = group->node;
   ASSERT(node != NULL);

   // Remove target group from the scheduler tree hierarchy. 
   Sched_MemberArrayRemoveByData(&parentGroup->members, node);
   node->parent = NULL;

   // Update internal memory resource related state in the scheduler tree
   MemSched_SubTreeChanged(parentGroup);

   /*
    * Perform resource specific admission control checks.
    * Note: Because we are holding the Sched_Tree lock the cpu
    *       and memory admission control checks are atomic. 
    */
   status = CpuSched_AdmitGroup(group, newParentGroup);
   if (status == VMK_OK) {
      status = MemSched_AdmitGroup(group, newParentGroup);
   }

   if (status == VMK_OK) {
      // admission checks have passed; assign new parent to group
      parentGroup = newParentGroup;
   }

   // attach target group back into the scheduler tree hierarchy
   node->parent = parentGroup->node;
   Sched_MemberArrayAdd(&parentGroup->members, node);

   // Update internal memory resource related state in the scheduler tree
   MemSched_SubTreeChanged(parentGroup);

   if (status == VMK_OK) {
      // Recompute paths for all the descendant VM nodes
      SchedRedoPath(group, worldArray, 0);
   }

   return (status);
}

/*
 *----------------------------------------------------------------------
 *
 * Sched_MoveGroup --
 * 
 *	Implements relocation of the sub-tree identified by "groupID" 
 *	to a new location in the scheduler tree hierarchy.
 *
 * Results:
 *	Returns VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Sched_MoveGroup(Sched_GroupID groupID, Sched_GroupID newParentGroupID)
{
   Sched_Group *group, *parentGroup, *newParentGroup;
   VMK_ReturnStatus status;
   World_ID *worldArray;
   int i;

   // Setup array to store list of affected worlds
   worldArray = (World_ID *)Mem_Alloc(sizeof(World_ID) * MAX_WORLDS);
   if (worldArray == NULL) {
      return (VMK_NO_MEMORY);
   }
   for (i = 0; i < MAX_WORLDS; i++) {
      worldArray[i] = INVALID_WORLD_ID;
   }

   Sched_TreeLock();

   // Fail if either target group or new parent group doesn't exist.
   group = SchedLookupGroupByID(groupID);
   newParentGroup = SchedLookupGroupByID(newParentGroupID);
   if ((group == NULL) || (newParentGroup == NULL)) {
      Sched_TreeUnlock();
      Mem_Free(worldArray);
      return (VMK_NOT_FOUND);
   }

   // If current parent is same as new parent then we are done.
   parentGroup = SchedGroupParent(group);
   ASSERT(parentGroup != NULL);
   if (parentGroup == newParentGroup) {
      Sched_TreeUnlock();
      Mem_Free(worldArray);
      return (VMK_OK);
   }

   // relocate the target group to its new location
   status = SchedMoveGroupInt(group, parentGroup, newParentGroup, worldArray);

   Sched_TreeUnlock();

   /*
    * Scan list of affected worlds and notify the resource schedulers 
    * of worlds that changed groups
    */
   for (i = 0; i < MAX_WORLDS; i++) {
      World_Handle *world;
      World_ID worldID;
     
      worldID = worldArray[i];
      if (worldID == INVALID_WORLD_ID) {
         break;
      }

      world = World_Find(worldID);
      if (world != NULL) {
         CpuSched_GroupChanged(world);
         MemSched_GroupChanged(world);
         World_Release(world);
      }
   }

   Mem_Free(worldArray);

   return (status);
}

/*
 *----------------------------------------------------------------------
 *
 * Sched_RenameGroup --
 *
 * 	Renames an existing scheduler group. 
 *
 * Results:
 *	Returns VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Sched_RenameGroup(Sched_GroupID groupID, const char *newGroupName)
{
   Sched_Group *group;

   Sched_TreeLock();

   // Fail if target group doesn't exist.
   group = SchedLookupGroupByID(groupID);
   if (group == NULL) {
      Sched_TreeUnlock();
      return (VMK_NOT_FOUND);
   }

   // Fail if attempting to rename a predefined group.
   if (SchedGroupIsPredefined(group)) {
      Sched_TreeUnlock();
      VmWarn(MY_RUNNING_WORLD->worldID, 
	     "predefined group '%s' cannot be renamed", group->groupName);
      return (VMK_BAD_PARAM);
   }

   // Fail if the new name for the group is already in use.
   if (SchedLookupGroupByName(newGroupName) != NULL) {
      Sched_TreeUnlock();
      return (VMK_EXISTS);
   }

   // Perform the name change.
   strncpy(group->groupName, newGroupName, SCHED_GROUP_NAME_LEN);
   group->groupName[SCHED_GROUP_NAME_LEN - 1] = '\0';

   Sched_TreeUnlock();
   return (VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * SchedJoinGroupInt --
 *
 *	Attempts to adds VM associated with "leader" to "parent" group.
 *	Caller must hold scheduler tree lock.
 *
 * Results:
 *	Returns VMK_OK if successful, otherwise error code.
 *	If successful, associates "leader" with "parent" group.
 *
 * Side effects:
 *      Modifies scheduler tree.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
SchedJoinGroupInt(World_Handle *leader, Sched_Group *parent)
{
   Sched_ClientGroup *clientGroup = &leader->sched.group;
   Sched_Node *n;

   // sanity checks
   ASSERT(Sched_TreeIsLocked());
   ASSERT(parent != NULL);
   ASSERT(CpuSched_IsVsmpLeader(leader));
   ASSERT(clientGroup->node == NULL);
   
   // fail if adding vm would exceed max depth
   if (SchedNodeDepth(parent->node) >= SCHED_NODE_DEPTH_MAX) {
      return(VMK_LIMIT_EXCEEDED);
   }

   // allocate slot
   if ((n = SchedFindUnusedNodeSlot()) == NULL) {
      return(VMK_LIMIT_EXCEEDED);
   }

   // initialize slot
   memset(n, 0, sizeof(Sched_Node));

   // validate node
   n->nodeType = SCHED_NODE_TYPE_VM;
   n->u.world = leader;

   // add to node list
   ASSERT(schedTree.nodes.len < SCHED_NODES_MAX);
   SchedNodeArrayAdd(&schedTree.nodes, n);

   // attach vm node into hierarchy
   clientGroup->node = n;
   n->parent = parent->node;
   Sched_MemberArrayAdd(&parent->members, n);

   // set client group id, path
   clientGroup->groupID = parent->groupID;
   SchedLookupGroupPath(n, &clientGroup->path);
   if (SCHED_DEBUG) {
      uint32 i;
      for (i = 0; i < SCHED_GROUP_PATH_LEN; i++) {
         Sched_GroupID id = clientGroup->path.level[i];
         if (id == SCHED_GROUP_ID_INVALID) {
            break;
         } else {
            LOG_ONLY(Sched_Group *group = SchedLookupGroupByID(id));
            VMLOG(1, leader->worldID, "groupPath[%u] = %u (%s)",
                  i, id, (group == NULL) ? "N/A" : group->groupName);
         }
      }
   }

   // debugging
   if (SCHED_DEBUG) {
      VMLOG(1, leader->worldID,
            "created vm node: parent group='%s'", 
            parent->groupName);
   }

   return(VMK_OK);
}

                      
/*
 *----------------------------------------------------------------------
 *
 * Sched_JoinGroup --
 *
 *	Attempts to adds VM associated with "world" to group
 *	associated with "id".
 *
 * Results:
 *	Returns VMK_OK if successful, otherwise error code.
 *	If successful, associates "world" with specified group.
 *
 * Side effects:
 *      Modifies scheduler tree.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Sched_JoinGroup(World_Handle *world, Sched_GroupID id)
{
   VMK_ReturnStatus status;
   Sched_Group *group;

   Sched_TreeLock();
   group = SchedLookupGroupByID(id);
   if (group != NULL) {
      status = SchedJoinGroupInt(CpuSched_GetVsmpLeader(world), group);
   } else {
      status = VMK_NOT_FOUND;
   }
   Sched_TreeUnlock();

   return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * SchedLeaveGroupInt --
 *
 *	Removes VM associated with "leader" from its current group.
 *	Caller must hold scheduler tree lock.
 *
 * Results:
 *	Removes VM associated with "leader" from its current group.
 *
 * Side effects:
 *      Modifies scheduler tree.
 *
 *----------------------------------------------------------------------
 */
static void
SchedLeaveGroupInt(World_Handle *leader)
{
   Sched_ClientGroup *clientGroup = &leader->sched.group;
   Sched_Node *n, *parentNode;
   Sched_Group *parentGroup;

   // sanity checks
   ASSERT(Sched_TreeIsLocked());
   ASSERT(CpuSched_IsVsmpLeader(leader));

   // find node associated with vsmp, done if none
   n = clientGroup->node;
   ASSERT(n != NULL);
   if (n == NULL) {
      if (SCHED_DEBUG) {
         VmLog(leader->worldID, "no node associated with vm");
      }
      return;
   }

   // detach vsmp node from hierarchy
   ASSERT(SCHED_NODE_IS_VM(n));
   parentNode = n->parent;
   ASSERT(parentNode != NULL);
   ASSERT(SCHED_NODE_IS_GROUP(parentNode));
   parentGroup = parentNode->u.group;
   Sched_MemberArrayRemoveByData(&parentGroup->members, n);

   // reset client group
   SchedClientGroupInvalidate(clientGroup);

   // remove from node list
   SchedNodeArrayRemoveByData(&schedTree.nodes, n);

   // reclaim node
   SchedDeallocateNode(n);

   // debugging
   if (SCHED_DEBUG) {
      VMLOG(1, leader->worldID, "detached vm node: parent group='%s'",
            parentGroup->groupName);
   }

   if ((parentGroup->flags & SCHED_GROUP_SELF_DESTRUCT)
       && parentGroup->members.len == 0) {
      VMK_ReturnStatus status;
      status = SchedRemoveGroupInt(parentGroup);
      ASSERT(status == VMK_OK);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * SchedLeaveGroup --
 *
 *	Removes VM associated with "world" from its current group.
 *
 * Results:
 *	Removes VM associated with "world" from its current group.
 *
 * Side effects:
 *      Modifies scheduler tree.
 *
 *----------------------------------------------------------------------
 */
void
Sched_LeaveGroup(World_Handle *world)
{
   Sched_TreeLock();
   SchedLeaveGroupInt(CpuSched_GetVsmpLeader(world));
   Sched_TreeUnlock();
}

/*
 *----------------------------------------------------------------------
 *
 * SchedChangeGroupInt --
 *
 *	Attempts to reparent the VM associated with "world" to the
 *	group associated with "id".  Caller must hold scheduler
 *	tree lock.
 *
 * Results:
 *	Returns VMK_OK if successful, otherwise error code.
 *	If successful, associates "world" with specified group.
 *
 * Side effects:
 *      Modifies scheduler tree.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
SchedChangeGroupInt(World_Handle *world, Sched_GroupID id)
{
   World_Handle *leader = CpuSched_GetVsmpLeader(world);
   Sched_Group *oldGroup, *newGroup;
   Sched_ClientGroup *clientGroup;
   VMK_ReturnStatus status;

   // sanity check
   ASSERT(Sched_TreeIsLocked());

   // lookup old group, fail if unable
   clientGroup = &leader->sched.group;
   oldGroup = SchedLookupGroupByID(clientGroup->groupID);
   if (oldGroup == NULL) {
      return(VMK_NOT_FOUND);
   }

   // lookup new group, fail if unable
   newGroup = SchedLookupGroupByID(id);
   if (newGroup == NULL) {
      return(VMK_NOT_FOUND);
   }

   // reparent leader, rejoin old group if unable
   SchedLeaveGroupInt(leader);
   status = SchedJoinGroupInt(leader, newGroup);
   VMLOG(0, leader->worldID,
         "joining new group '%s': status %s",
         newGroup->groupName,
         VMK_ReturnStatusToString(status));
   if (status != VMK_OK) {
      VMK_ReturnStatus rejoinStatus;
      rejoinStatus = SchedJoinGroupInt(leader, oldGroup);
      VMLOG(0, leader->worldID,
            "rejoining old group '%s': status %s",
            oldGroup->groupName,
            VMK_ReturnStatusToString(rejoinStatus));
      ASSERT_NOT_IMPLEMENTED(rejoinStatus == VMK_OK);
   }

   return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * Sched_ChangeGroup --
 *
 *	Attempts to reparent the VM associated with "world" to the
 *	group associated with "id", updating per-resource scheduler
 *	modules appropriately.
 *
 * Results:
 *	Returns VMK_OK if successful, otherwise error code.
 *	If successful, associates "world" with specified group.
 *
 * Side effects:
 *      Modifies scheduler tree.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Sched_ChangeGroup(World_Handle *world, Sched_GroupID parentID)
{
   VMK_ReturnStatus status, changeStatus;
   Sched_GroupID tmpID;

   // create temporary group for transient reservation
   status = Sched_AddGroup(NULL, parentID, &tmpID);
   if (status != VMK_OK) {
      return(status);
   }

   // move allocation to temporary group
   status = CpuSched_MoveVmAllocToGroup(world, tmpID);
   if (status != VMK_OK) {
      VmWarn(world->worldID, "unable to move allocation to group %u", tmpID);
      Sched_RemoveGroup(tmpID);
      return(status);
   }
   
   // reparent "world" in scheduler tree
   Sched_TreeLock();
   changeStatus = SchedChangeGroupInt(world, parentID);
   Sched_TreeUnlock();
   if (changeStatus != VMK_OK) {
      VmWarn(world->worldID, "unable to reparent: status %s",
             VMK_ReturnStatusToString(changeStatus));
   }

   // move allocation back from temporary group
   status = CpuSched_MoveGroupAllocToVm(tmpID, world);
   if (status != VMK_OK) {
      VmWarn(world->worldID,
             "unable to restore original allocation: status %s",
             VMK_ReturnStatusToString(status));
   }

   // destroy temporary group
   Sched_RemoveGroup(tmpID);

   // notify resource schedulers that group changed
   CpuSched_GroupChanged(world);

   return(changeStatus);
}

/*
 *----------------------------------------------------------------------
 *
 * Sched_GroupIDToName --
 *
 *      Sets "nameBuf" to scheduler group name associated with "id",
 *	writing no more than "nameBufLen" characters.
 *
 * Results:
 *      Returns VMK_OK if successful, otherwise error code.
 *	If successful, sets "nameBuf" to scheduler group name
 *	associated with "id".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Sched_GroupIDToName(Sched_GroupID id,
                    char *nameBuf,
                    uint32 nameBufLen)
{
   Sched_Group *group;

   // sanity check
   ASSERT(nameBufLen > 0);
   if (nameBufLen == 0) {
      return(VMK_BAD_PARAM);
   }

   // avoid lookup for predefined groups
   if (id < SCHED_NUM_PREDEFINED_GROUPS) {
      strncpy(nameBuf, schedPredefinedGroups[id].name, nameBufLen);
      nameBuf[nameBufLen - 1] = '\0';
      return(VMK_OK);
   }

   // lookup name
   Sched_TreeLock();
   group = SchedLookupGroupByID(id);
   if (group == NULL) {
      Sched_TreeUnlock();
      nameBuf[0] = '\0';
      return(VMK_NOT_FOUND);
   }
   strncpy(nameBuf, group->groupName, nameBufLen);
   nameBuf[nameBufLen - 1] = '\0';
   Sched_TreeUnlock();

   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * Sched_GroupNameToID --
 *
 *      Returns scheduler group id associated with "name", or
 *	SCHED_GROUP_ID_INVALID if not found.
 *
 * Results:
 *      Returns scheduler group id associated with "name", or
 *	SCHED_GROUP_ID_INVALID if not found.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Sched_GroupID
Sched_GroupNameToID(const char *name)
{
   Sched_Group *group;
   Sched_GroupID id;

   Sched_TreeLock();
   group = SchedLookupGroupByName(name);
   if (group == NULL) {
      id = SCHED_GROUP_ID_INVALID;
   } else {
      id = group->groupID;
   }
   Sched_TreeUnlock();

   return(id);
}

/*
 *----------------------------------------------------------------------
 *
 * SchedProcGroupsRead --
 *
 *	Callback for read operation on /proc/vmware/sched/groups
 *	procfs node.
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
SchedProcGroupsRead(UNUSED_PARAM(Proc_Entry *entry),
		    char *buf,
		    int *len)
{
   *len = 0;

   // Invoke resource specific rotines to do the appropriate
   CpuSched_ProcGroupsRead(buf, len);
   MemSched_ProcGroupsRead(buf, len);

   return (VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * SchedProcGroupsWrite --
 *
 *	Callback for write operation on /proc/vmware/sched/groups
 *	procfs node. Implements the following scheduler group operations.
 *	 - Creating a new group.
 *	 - Removing an existing group.
 *	 - Renaming an existing group.
 *	 - Moving the sub-tree specified by a group to another
 *	   location within the scheduler tree hierarchy.
 *       - Changing an existing group's cpu/memory resource allocations.
 *
 * Results:
 *      Return VMK_OK on success; error code on failure.
 *
 * Side effects:
 *	Possible change in scheduler state.
 *
 *----------------------------------------------------------------------
 */
static int
SchedProcGroupsWrite(UNUSED_PARAM(Proc_Entry *entry),
		     char *buf,
		     int *len)
{
   char *argv[9], *cmd, *groupName;
   Sched_GroupID groupID;
   VMK_ReturnStatus status;
   int argc;

   // parse command, group, args
   argc = Parse_Args(buf, argv, 8);
   if (argc < 2) {
      Warning("invalid command: too few parameters");
      return (VMK_BAD_PARAM);
   }
   cmd = argv[0];
   groupName = argv[1];
   groupID = Sched_GroupNameToID(groupName);

   if (SCHED_DEBUG) {
      LOG(0, "argc=%d: cmd=%s, name=%s", argc, cmd, groupName);
   }

   if ((strcmp(cmd, "mk") == 0) || (strcmp(cmd, "create") == 0)) {
      /*
       * Create a new group
       * Format: mk/create <groupName> <parentGroupName>
       */
      if (argc == 3) {
         const char *parentName = argv[2];
         Sched_GroupID parentID;

         if (groupID != SCHED_GROUP_ID_INVALID) {
            Warning("invalid group name: %s already exists", groupName);
            return (VMK_EXISTS);
         }

	 if (strcmp(parentName, SCHED_GROUP_NAME_UW_NURSERY) == 0) {
            Warning("invalid group name: %s", parentName);
            return (VMK_BAD_PARAM);
         }

         parentID = Sched_GroupNameToID(parentName);
         if (parentID == SCHED_GROUP_ID_INVALID) {
            Warning("invalid group name: %s not found", parentName);
            return (VMK_NOT_FOUND);
         }

         status = Sched_AddGroup(groupName, parentID, &groupID);
	 return (status);
      }
   } else if ((strcmp(cmd, "rm") == 0) || (strcmp(cmd, "remove") == 0)) {
      /*
       * Remove existing group
       * Format: rm/remove <groupName>
       */
      if (groupID == SCHED_GROUP_ID_INVALID) {
         Warning("invalid group name: %s not found", groupName);
         return (VMK_NOT_FOUND);
      }

      if (argc == 2) {
         status = Sched_RemoveGroup(groupID);
	 return (status);
      }
   } else if (strcmp(cmd, "rename") == 0) {
      /*
       * Rename group
       * Format: rename <groupName> <newGroupName>
       */
      if (argc == 3) {
         const char *newGroupName = argv[2];
         Sched_GroupID newGroupID;

         if (groupID == SCHED_GROUP_ID_INVALID) {
            Warning("invalid group name: %s not found", groupName);
            return (VMK_NOT_FOUND);
         }

         newGroupID = Sched_GroupNameToID(newGroupName);
         if (newGroupID != SCHED_GROUP_ID_INVALID) {
            Warning("invalid group name: %s already exists", newGroupName);
            return (VMK_EXISTS);
         }

         status = Sched_RenameGroup(groupID, newGroupName);
	 return (status);
      }
   } else if ((strcmp(cmd, "mv") == 0) || (strcmp(cmd, "move") == 0)) {
      /*
       * Relocate sub-tree under groupName within sched tree hierarchy
       * Format: move <groupName> <newParentGroupName>
       */
      if (argc == 3) {
         const char *newParentName = argv[2];
         Sched_GroupID newParentID;

         if (groupID == SCHED_GROUP_ID_INVALID) {
            Warning("invalid group name: %s not found", groupName);
            return (VMK_NOT_FOUND);
         }

         newParentID = Sched_GroupNameToID(newParentName);
         if (newParentID == SCHED_GROUP_ID_INVALID) {
            Warning("invalid group name: %s not found", newParentName);
            return (VMK_NOT_FOUND);
         }

         status = Sched_MoveGroup(groupID, newParentID);
	 return (status);
      }
   } else if (strcmp(cmd, "alloc") == 0) {
      /*
       * Change group allocation
       * Format: alloc <groupName> <resource> <min> <max> <shares> 
       *               <minLimit> <hardMax> <units?>"
       */
      if (argc >= 8) {
	 const char *resource = argv[2];
         const char *minStr = argv[3];
         const char *maxStr = argv[4];
         const char *sharesStr = argv[5];
         const char *minLimitStr = argv[6];
         const char *hardMaxStr = argv[7];
         Sched_Alloc alloc;
         
         if (groupID == SCHED_GROUP_ID_INVALID) {
            Warning("invalid group name: %s not found", groupName);
            return (VMK_NOT_FOUND);
         }

         if ((Parse_Int(minStr, strlen(minStr), &alloc.min) == VMK_OK) &&
             (Parse_Int(maxStr, strlen(maxStr), &alloc.max) == VMK_OK) &&
             (Parse_Int(sharesStr, strlen(sharesStr), 
			&alloc.shares) == VMK_OK) &&
             (Parse_Int(minLimitStr, strlen(minLimitStr), 
			&alloc.minLimit) == VMK_OK) &&
             (Parse_Int(hardMaxStr, strlen(hardMaxStr), 
			&alloc.hardMax) == VMK_OK)) {
            alloc.units = SCHED_UNITS_INVALID;
            if (argc == 9) {
               alloc.units = Sched_StringToUnits(argv[7]);
               if (alloc.units == SCHED_UNITS_INVALID) {
                  Warning("invalid unit specification");
                  return (VMK_BAD_PARAM);
               }
            }
            if (strcmp(resource, "cpu") == 0) {
               if (alloc.units == SCHED_UNITS_INVALID) {
                  alloc.units = SCHED_UNITS_PERCENT;
               }   
	       status = CpuSched_GroupSetAlloc(groupID, &alloc);
	    } else if (strcmp(resource, "mem") == 0) {
               if (alloc.units == SCHED_UNITS_INVALID) {
                  alloc.units = SCHED_UNITS_MB;
               }

	       if (((alloc.min < 0) && (alloc.min != SCHED_CONFIG_NONE)) ||
	           ((alloc.max < 0) && (alloc.max != SCHED_CONFIG_NONE)) ||
	           ((alloc.minLimit < 0) && 
		    (alloc.minLimit != SCHED_CONFIG_NONE)) ||
	           ((alloc.hardMax < 0) && 
		    (alloc.hardMax != SCHED_CONFIG_NONE))) {
                  Warning("invalid memory allocation parameters");
                  return (VMK_BAD_PARAM);
               }
	       status = MemSched_GroupSetAlloc(groupID, &alloc);
            } else {
               Warning("invalid resource specification");
               status = VMK_BAD_PARAM;
            }

	    return (status);
         }
      }
   }

   Warning("invalid command: \"%s\"", cmd);
   return (VMK_BAD_PARAM);
}

/*
 *----------------------------------------------------------------------
 *
 * SchedProcInit --
 *
 *	Create the scheduler tree's proc entries.
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
SchedProcInit(void)
{
   Proc_Entry *schedDir;
   Proc_Entry *groups;

   // register [top-level] "/proc/vmware/sched/" directory entry
   schedDir = &schedTree.procSchedDir; 
   Proc_InitEntry(schedDir);
   schedDir->parent = NULL;
   Proc_Register(schedDir, "sched", TRUE);

   // register "sched/groups" node entry
   groups = &schedTree.procGroups;
   Proc_InitEntry(groups);
   groups->parent = schedDir;
   groups->read = SchedProcGroupsRead;
   groups->write = SchedProcGroupsWrite;
   Proc_Register(groups, "groups", FALSE);
}

/*
 *----------------------------------------------------------------------
 *
 * SchedTreeInit --
 *
 *      Initializes the scheduler tree data structures.
 *
 * Results:
 *      None. 
 *
 * Side effects:
 *      Modifies scheduler state.
 *
 *----------------------------------------------------------------------
 */
static void
SchedTreeInit(void)
{
   uint32 i;

   // initialize locks
   SP_InitLockIRQ("SchedTree", &schedTree.lock, SP_RANK_IRQ_SCHED_TREE);
 
   // initialize tables
   for (i = 0; i < SCHED_GROUPS_MAX; i++) {
      schedTree.groupTable[i].groupID = SCHED_GROUP_ID_INVALID;
      schedTree.groupTable[i].groupNextID = i;
   }
   for (i = 0; i < SCHED_NODES_MAX; i++) {
      schedTree.nodeTable[i].nodeType = SCHED_NODE_TYPE_INVALID;
   }

   // scheduler related procfs initialization
   SchedProcInit(); 
}

/*
 *----------------------------------------------------------------------
 *
 * Sched_GroupPathInvalidate --
 *
 *	Sets "path" to invalid top-level group.
 *
 * Results:
 *	Sets "path" to invalid top-level group.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
void
Sched_GroupPathInvalidate(Sched_GroupPath *path)
{
   path->level[0] = SCHED_GROUP_ID_INVALID;
}

/*
 *----------------------------------------------------------------------
 *
 * Sched_GroupPathSetRoot --
 *
 *	Sets "path" to specify top-level root group.
 *
 * Results:
 *      Sets "path" to specify top-level root group.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
void
Sched_GroupPathSetRoot(Sched_GroupPath *path)
{
   path->level[0] = SCHED_GROUP_ID_ROOT;
   path->level[1] = SCHED_GROUP_ID_INVALID;
}

/*
 *----------------------------------------------------------------------
 *
 * Sched_GroupPathEqual --
 *
 *	Returns TRUE iff paths "a" and "b" are equal.
 *
 * Results:
 *	Returns TRUE iff paths "a" and "b" are equal.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
Bool
Sched_GroupPathEqual(const Sched_GroupPath *a,
                     const Sched_GroupPath *b)
{
   uint32 i;
   for (i = 0; i < SCHED_GROUP_PATH_LEN; i++) {
      if (a->level[i] != b->level[i]) {
         return(FALSE);
      }
      if (a->level[i] == SCHED_GROUP_ID_INVALID) {
         break;
      }
   }
   return(TRUE);
}

/*
 *----------------------------------------------------------------------
 *
 * Sched_GroupPathCopy --
 *
 *	Sets path "to" to identical copy of path "from".
 *
 * Results:
 *	Sets path "to" to identical copy of path "from".
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
void
Sched_GroupPathCopy(Sched_GroupPath *to,
                    const Sched_GroupPath *from)
{
   uint32 i;
   for (i = 0; i < SCHED_GROUP_PATH_LEN; i++) {
      to->level[i] = from->level[i];
      if (from->level[i] == SCHED_GROUP_ID_INVALID) {
         break;
      }
   }
}

/*
 * Internal Operations
 * Exported only to scheduler modules via "sched_int.h".
 */

/*
 *----------------------------------------------------------------------
 *
 * Sched_ForAllGroupsDo --
 *
 *	Performs "f(g, data)" for all groups "g".
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	May modify scheduler group state.
 *
 *----------------------------------------------------------------------
 */
void
Sched_ForAllGroupsDo(Sched_ForAllGroupsFn f, void *data)
{
   Sched_TreeLock();

   FORALL_GROUPS(g) {
      f(g, data);
   } GROUPS_DONE;

   Sched_TreeUnlock();
}

/*
 *----------------------------------------------------------------------
 *
 * Sched_TreeRootNode --
 *
 *	Returns the root node of the scheduler tree.
 *	Caller must hold scheduler tree lock.
 *
 * Results:
 *	Returns the root node of the scheduler tree.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
Sched_Node *
Sched_TreeRootNode(void)
{
   ASSERT(Sched_TreeIsLocked());
   return(schedTree.nodeRoot);
}

/*
 *----------------------------------------------------------------------
 *
 * Sched_TreeNodeCount --
 *
 *	Returns the number of nodes in the scheduler tree.
 *	Caller must hold scheduler tree lock.
 *
 * Results:
 *	Returns the number of nodes in the scheduler tree.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
uint32
Sched_TreeNodeCount(void)
{
   ASSERT(Sched_TreeIsLocked());
   return(schedTree.nodes.len);
}

/*
 *----------------------------------------------------------------------
 *
 * Sched_TreeRootGroup --
 *
 *	Returns the root group of the scheduler tree.
 *	Caller must hold scheduler tree lock.
 *
 * Results:
 *	Returns the root group of the scheduler tree.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
Sched_Group *
Sched_TreeRootGroup(void)
{
   ASSERT(Sched_TreeIsLocked());
   return(schedTree.groupRoot);
}

/*
 *----------------------------------------------------------------------
 *
 * Sched_TreeGroupParent --
 *
 *	Returns the parent group associated with "group",
 *	or NULL if no parent exists (e.g. root group).
 *	Caller must hold scheduler tree lock.
 *
 * Results:
 *	Returns the parent group associated with "group",
 *	or NULL if no parent exists.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
Sched_Group *
Sched_TreeGroupParent(const Sched_Group *group)
{
   return(SchedGroupParent(group));
}

/*
 *----------------------------------------------------------------------
 *
 * Sched_TreeGroupCount --
 *
 *	Returns the number of groups in the scheduler tree.
 *	Caller must hold scheduler tree lock.
 *
 * Results:
 *	Returns the number of groups in the scheduler tree.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
uint32
Sched_TreeGroupCount(void)
{
   ASSERT(Sched_TreeIsLocked());
   return(schedTree.groups.len);
}

/*
 *----------------------------------------------------------------------
 *
 * Sched_TreeLookupGroup --
 *
 *	Returns the group associated with "id", or NULL
 *	if no such group exists.  Caller must hold
 *	scheduler tree lock.
 *
 * Results:
 *	Returns the group associated with "id", or NULL
 *	if no such group exists.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Sched_Group *
Sched_TreeLookupGroup(Sched_GroupID id)
{
   return(SchedLookupGroupByID(id));
}

/*
 *----------------------------------------------------------------------
 *
 * Sched_TreeLookupGroupSlot --
 *
 *	Returns the group associated with the slot for "id".
 *
 * Results:
 *	Returns the group associated with the slot for "id".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Sched_Group *
Sched_TreeLookupGroupSlot(Sched_GroupID id)
{
   return(&schedTree.groupTable[id & SCHED_GROUPS_MASK]);
}

/*
 *----------------------------------------------------------------------
 *
 * Sched_TreeGroupAddReference --
 *
 *	Increments reference count for "group".
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      Increments reference count for "group".
 *
 *----------------------------------------------------------------------
 */
void
Sched_TreeGroupAddReference(Sched_Group *group)
{
   ASSERT(Sched_TreeIsLocked());
   ASSERT(group->refCount >= 0);
   group->refCount++;
}

/*
 *----------------------------------------------------------------------
 *
 * Sched_TreeGroupRemoveReference --
 *
 *	Decrements reference count for "group".
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      Decrements reference count for "group".
 *
 *----------------------------------------------------------------------
 */
void
Sched_TreeGroupRemoveReference(Sched_Group *group)
{
   ASSERT(Sched_TreeIsLocked());
   ASSERT(group->refCount > 0);
   group->refCount--;
   if (group->removed && (group->refCount == 0)) {
      SchedReapGroup(group);
   }
}


#undef SCHEDUNITS
#define SCHEDUNITS(EN, S) S,

static const char* schedUnitStrings[] = {
   SCHED_UNITS_LIST
};

/*
 *-----------------------------------------------------------------------------
 *
 * Sched_UnitsToString --
 *
 *      Returns a constant string corresponding to the specified unit type.
 *
 * Results:
 *      Returns a constant string corresponding to the specified unit type.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
const char *
Sched_UnitsToString(Sched_Units units)
{
   ASSERT(units <= SCHED_UNITS_INVALID);
   return (schedUnitStrings[units]);
}

Sched_Units
Sched_StringToUnits(const char *ustr)
{
   unsigned i;
   
   for (i=0; i < SCHED_UNITS_INVALID; i++) {
      if (strcmp(ustr, schedUnitStrings[i]) == 0) {
         break;
      }
   }

   return (Sched_Units)i;
}


/*
 *-----------------------------------------------------------------------------
 *
 * SchedGroupAddFlags --
 *
 *      Add "flags" to the flags of the group "groupID".
 *	Supported flags include: 
 *         SCHED_GROUP_SELF_DESTRUCT -- 
 *            When set, the group will be automatically destroyed when 
 *            its member count count drops from 1 to 0.
 *         SCHED_GROUP_IS_PREDEFINED --
 *            When set, the group is a predefined system group. It cannot
 *            be changed through proc nodes.
 *         SCHED_GROUP_IS_LEAF --
 *            Indicates that the group cannot have a group as child.
 *         SCHED_GROUP_IS_VM --
 *            Indicates that the scheduler group represents a VM consisting 
 *            of both vmm and vmx worlds.
 *         SCHED_GROUP_IS_MEMSCHED_CLIENT --
 *            Indicates that the scheduler group represents a memsched 
 *            client.
 *         SCHED_GROUP_IS_SYSTEM --
 *            Indicates that the scheduler gorup represents a group 
 *            that only contains system worlds.
 *
 * Results:
 *      Returns VMK_OK on success, VMK_NOT_FOUND if groupID doesn't exist
 *
 * Side effects:
 *      Changes flags field of groupID.
 *
 *-----------------------------------------------------------------------------
 */
static void
SchedGroupAddFlags(Sched_Group* group, SchedGroupFlags flags)
{
   ASSERT(Sched_TreeIsLocked());
   ASSERT(group != NULL);
   group->flags |= flags;
}

/*
 *-----------------------------------------------------------------------------
 *
 * SchedGroupInit --
 *
 *      Initialize predefined groups and add them to the schedTree.
 *
 * Results:
 *      Fill schedTree with predefined groups.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
static void
SchedGroupInit(void)
{
   Sched_GroupID id;

   Sched_TreeLock();

   // initialize non-root predefined groups
   for (id = 0; id < SCHED_NUM_PREDEFINED_GROUPS; id++) {
      VMK_ReturnStatus status;
      SchedPredefinedGroup *grp = &schedPredefinedGroups[id];
      Sched_Group *group, *parent;

      parent = SchedLookupGroupByID(grp->parentID);
      // only root doesn't have parent
      ASSERT(parent != NULL || id == SCHED_GROUP_ID_ROOT);

      status = SchedAddGroupInt(grp->name, parent, &group);
      // sanity check
      ASSERT_NOT_IMPLEMENTED(status == VMK_OK);
      ASSERT_NOT_IMPLEMENTED((id == grp->groupID) && 
		             (grp->groupID == group->groupID));

      SchedGroupAddFlags(group, SCHED_GROUP_IS_PREDEFINED);

      // assign root group 
      if (parent == NULL) {
         schedTree.groupRoot = group;
         schedTree.nodeRoot = group->node;
      }

      /*
       * If min/max (= -x) is negative, it represents (total - x + 1).
       * Add (total + 1) to min/max.
       */ 
      ASSERT(SCHED_ALLOC_TOTAL == -1);
      if (grp->cpu.min < 0) {
         grp->cpu.min += CpuSched_PercentTotal() - SCHED_ALLOC_TOTAL;
         ASSERT(grp->cpu.min >= 0);
      }
      if (grp->cpu.max < 0) {
         grp->cpu.max += CpuSched_PercentTotal() - SCHED_ALLOC_TOTAL;
         ASSERT(grp->cpu.max >= 0);
      }
      if (grp->mem.min < 0) {
         grp->mem.min += MemMap_ManagedPages() - SCHED_ALLOC_TOTAL;
         ASSERT(grp->mem.min >= 0);
      }
      if (grp->mem.max < 0) {
         grp->mem.max += MemMap_ManagedPages() - SCHED_ALLOC_TOTAL;
         ASSERT(grp->mem.max >= 0);
      }

      status = CpuSched_GroupSetAllocLocked(group, &grp->cpu);
      ASSERT_NOT_IMPLEMENTED(status == VMK_OK);

      status = MemSched_GroupSetAllocLocked(group, &grp->mem);
      ASSERT_NOT_IMPLEMENTED(status == VMK_OK);
   }

   Sched_TreeUnlock();
}

/*
 *----------------------------------------------------------------------
 *
 * SchedInitContainerGroup -- 
 * 	A container group is created for "world" and added to
 *      to the parent gorup with group id of "parentID".
 *
 * 	"World" must be a world group leader.
 *
 * Results:
 *      Returns VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
SchedInitContainerGroup(World_Handle *world, 
                        Sched_ClientConfig *config, 
                        Sched_GroupID parentID,
                        Sched_GroupID *groupID)
{
   VMK_ReturnStatus status;
   char groupName[SCHED_GROUP_NAME_LEN];
   Sched_Group *group;

   /*
    * Create a new container group for this world group.
    */
   snprintf(groupName, 
            SCHED_GROUP_NAME_LEN, 
	    SCHED_GROUP_ANON_PREFIX "%u", 
            world->worldID);
   groupName[SCHED_GROUP_NAME_LEN - 1] = '\0';
   status = Sched_AddGroup(groupName, parentID, groupID);
   if (status != VMK_OK) {
      return (status);
   }

   // debug
   LOG(1, "created container group '%s' for world %d", groupName, world->worldID);

   Sched_TreeLock();
   group = SchedLookupGroupByID(*groupID);
   ASSERT(group != NULL);
   SchedGroupAddFlags(group, SCHED_GROUP_IS_LEAF | SCHED_GROUP_SELF_DESTRUCT);
   if (World_IsVMMWorld(world) || World_IsUSERWorld(world)) {
      SchedGroupAddFlags(group, SCHED_GROUP_IS_MEMSCHED_CLIENT);
   } else if (World_IsSYSTEMWorld(world)) {
      SchedGroupAddFlags(group, SCHED_GROUP_IS_SYSTEM);
   }

   // setup cpu resource specific allocations for the group
   status = CpuSched_GroupSetAllocLocked(group, &config->group.cpu);
   if (status != VMK_OK) {
      VmWarn(world->worldID, "invalid cpu allocation for new group: %s",
             VMK_ReturnStatusToString(status));
   } else {
      // Setup memory resource specific allocations for the group
      status = MemSched_GroupSetAllocLocked(group, &config->group.mem);
      if (status != VMK_OK) {
         VmWarn(world->worldID, "invalid memory allocation for new group: %s",
                VMK_ReturnStatusToString(status));
      }
   }

   Sched_TreeUnlock();
   return (status);
}


/*
 *----------------------------------------------------------------------
 *
 * Sched_WorldGroupInit -- 
 * 	Initialize the scheduler group for the world group.
 * 	"World" must be a world group leader.
 *
 * Results:
 *      Returns VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Sched_WorldGroupInit(World_Handle *world, World_InitArgs *args)
{
   VMK_ReturnStatus status;
   Sched_GroupID parentID;
   Sched_ClientConfig *config = args->sched;

   ASSERT(World_IsGroupLeader(world));

   world->group->schedGroupID = SCHED_GROUP_ID_INVALID;

   parentID= Sched_GroupNameToID(config->group.groupName);
   if (parentID == SCHED_GROUP_ID_INVALID) {
      return (VMK_BAD_PARAM);
   }

   if (config->group.createContainer) {
      status = SchedInitContainerGroup(world, config, parentID,
                                       &world->group->schedGroupID);
      if (status != VMK_OK) {
         VmWarn(world->worldID, "cannot create container group status %s",
                VMK_ReturnStatusToString(status));
         return status;
      }
   } else {
      world->group->schedGroupID = parentID;
   }

   MemSched_WorldGroupInit(world, args);

   return (VMK_OK);
}


/*
 *----------------------------------------------------------------------
 *
 * Sched_WorldGroupCleanup --
 * 	Per world group scheduler clean up.
 *
 * Results:
 *      Returns VMK_OK if successful, otherwise error code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Sched_WorldGroupCleanup(World_Handle *world)
{
   MemSched_WorldGroupCleanup(world);
   world->group->schedGroupID = SCHED_GROUP_ID_INVALID;
   return (VMK_OK);
}


/*
 *-----------------------------------------------------------------------------
 *
 * SchedSetupVMGroup --
 *
 *	This routine sets up the scheduler group (initially created under
 *	uwnursery) for the VM.  First the group is renamed appropriately 
 *	and placed under the desired parent group as specified in the
 *	config file. Then, based on the specifications in the config file 
 *	the scheduler parameters for the group are initialized.
 *
 * Results:
 *      Returns VMK_OK on success, otherwise error code.
 *
 * Side effects:
 * 	Changes to group state and scheduler tree.
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
SchedSetupVMGroup(struct World_Handle *world,
                  const Sched_ClientConfig *config)
{
   Sched_Group *group;
   Sched_GroupID groupID, parentID;
   char groupName[SCHED_GROUP_NAME_LEN];
   VMK_ReturnStatus status;

   // debugging
   VmLog(world->worldID,
         "adding '%s': "
         "group '%s': "
         "cpu: shares=%d min=%d max=%d",
         world->worldName,
         config->group.groupName,
         config->group.cpu.shares,
         config->group.cpu.min,
         config->group.cpu.max);

   groupID = world->group->schedGroupID;
   ASSERT(groupID != SCHED_GROUP_ID_INVALID);

   // rename the group
   snprintf(groupName, SCHED_GROUP_NAME_LEN, SCHED_GROUP_VM_PREFIX "%u", world->worldID);
   groupName[SCHED_GROUP_NAME_LEN - 1] = '\0';
   status = Sched_RenameGroup(groupID, groupName);
   if (status != VMK_OK) {
      return (status);
   }
   VmLog(world->worldID, "renamed group %d to %s", groupID, groupName); 

   // place the group under desired parent group
   parentID = Sched_GroupNameToID(config->group.groupName);
   if (parentID == SCHED_GROUP_ID_INVALID) {
      VmWarn(world->worldID, "group name %s not found, defaulting to %s",
              config->group.groupName,
              SCHED_GROUP_NAME_LOCAL);
      parentID = SCHED_GROUP_ID_LOCAL;
   }

   status = Sched_MoveGroup(groupID, parentID);
   if (status != VMK_OK) {
      return (status);
   }
   VmLog(world->worldID, "moved group %d to be under group %d", 
	 groupID, parentID); 

   Sched_TreeLock();
   group = SchedLookupGroupByID(groupID);
   ASSERT(group != NULL);
   // indicate that the group represents a VM
   SchedGroupAddFlags(group, SCHED_GROUP_IS_VM);

   // setup cpu resource specific alloctions for the group
   status = CpuSched_GroupSetAllocLocked(group, &config->group.cpu);
   if (status != VMK_OK) {
      VmWarn(world->worldID, "invalid cpu allocation for VM group: %s",
             VMK_ReturnStatusToString(status));
   } else {
      // Setup memory resource specific alloctions for the group
      status = MemSched_SetupVmGroup(world, group, &config->group.mem);
      if (status != VMK_OK) {
         VmWarn(world->worldID, "invalid memory allocation for VM group: %s",
                VMK_ReturnStatusToString(status));
      }
   }

   Sched_TreeUnlock();
   return (status);
}
