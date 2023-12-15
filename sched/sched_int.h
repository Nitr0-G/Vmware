/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * sched_int.h --
 *
 *	Internal scheduler interfaces.  For use only by files that
 *	implement scheduler operations (CpuSched, MemSched).
 */

#ifndef _SCHED_INT_H
#define _SCHED_INT_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "sched_dist.h"
#include "sched_ext.h"
#include "sched.h"
#include "cpusched.h"
#include "memsched.h"

/*
 * Constants
 */

// limits
#define	SCHED_NODES_MAX_LG	(SCHED_GROUPS_MAX_LG + 1)
#define	SCHED_NODES_MAX		(1 << SCHED_NODES_MAX_LG)
#define	SCHED_NODE_DEPTH_MAX	(SCHED_GROUP_PATH_LEN - 1)

/*
 * Special sched group resource allocation constants
 * used for specifying allocation in terms of total resource.
 *
 * i.e.
 *      "SCHED_ALLOC_TOTAL" means total resource.
 *
 *      "SCHED_ALLOC_TOTAL - 25" means total - 25 units of resource.
 */
#define SCHED_ALLOC_TOTAL   (-1L)

/*
 * Types
 */

typedef enum {
   SCHED_NODE_TYPE_INVALID = 0,
   SCHED_NODE_TYPE_VM,
   SCHED_NODE_TYPE_GROUP
} Sched_NodeType;

typedef struct Sched_Node {
   // tagged union: "vm" or "group" node
   Sched_NodeType nodeType;
   union {
      struct World_Handle *world;
      struct Sched_Group *group;
   } u;

   // parent node (or NULL if root)
   struct Sched_Node *parent;
} Sched_Node;

#define	LIST_ITEM_TYPE Sched_Node*
#define	LIST_NAME Sched_MemberArray
#define	LIST_SIZE SCHED_GROUP_MEMBERS_MAX
#include "staticlist.h"

// scheduler group flags
typedef enum {
   SCHED_GROUP_SELF_DESTRUCT      = 0x0001,
   SCHED_GROUP_IS_PREDEFINED      = 0x0002,
   SCHED_GROUP_IS_LEAF            = 0x0004,
   SCHED_GROUP_IS_VM              = 0x0008,
   SCHED_GROUP_IS_MEMSCHED_CLIENT = 0x0010,
   SCHED_GROUP_IS_SYSTEM          = 0x0020
} SchedGroupFlags;

typedef struct Sched_Group {
   // identity
   Sched_GroupID groupID;
   Sched_GroupID groupNextID;
   SchedGroupFlags flags;
   char groupName[SCHED_GROUP_NAME_LEN];

   // reference count
   // non-zero => can remove, but cannot deallocate
   int32 refCount;
   Bool removed;

   // associated node
   Sched_Node *node;

   // group members
   Sched_MemberArray members;

   // per-resource scheduler state
   CpuSched_GroupState cpu;
   MemSched_GroupState mem;
} Sched_Group;

/*
 * Macros
 */

#define	SCHED_NODE_IS_GROUP(n)	 ((n)->nodeType == SCHED_NODE_TYPE_GROUP)
#define	SCHED_NODE_IS_VM(n)	 ((n)->nodeType == SCHED_NODE_TYPE_VM)
#define	SCHED_NODE_IS_INVALID(n) ((n)->nodeType == SCHED_NODE_TYPE_INVALID)

#define	FORALL_GROUP_MEMBER_NODES(_g, _n) \
	do { \
	  uint32 _mi; \
	  ASSERT(Sched_TreeIsLocked()); \
	  for (_mi = 0; _mi < _g->members.len; _mi++) { \
	    Sched_Node* _n = _g->members.list[_mi];
#define	GROUP_MEMBER_NODES_DONE }} while(0) 

// predefined scheduler groups

#define SCHED_PREDEFINED_GROUPS(_id, _pid, \
                                _cpuMin, _cpuMax, _cpuShares, \
                                _memMin, _memMax, _memShares, _memMinLimit, _memHardMax)
#define SCHED_PREDEFINED_GROUP_LIST \
   SCHED_PREDEFINED_GROUPS(ROOT,           INVALID,                   \
        SCHED_ALLOC_TOTAL, CPUSCHED_ALLOC_MAX_NONE, 10000,            \
        SCHED_ALLOC_TOTAL, SCHED_ALLOC_TOTAL, MEMSCHED_SHARES_MAX, SCHED_ALLOC_TOTAL, SCHED_ALLOC_TOTAL) \
                                                                      \
   SCHED_PREDEFINED_GROUPS(IDLE,           ROOT,                      \
        0, CPUSCHED_ALLOC_MAX_NONE, CPUSCHED_SHARES_IDLE,             \
        0, 0, 10000, 0, 0)                                            \
   SCHED_PREDEFINED_GROUPS(SYSTEM,         ROOT,                      \
        25, CPUSCHED_ALLOC_MAX_NONE, 1000,                            \
        0, SCHED_CONFIG_NONE, 10000, SCHED_CONFIG_NONE, 0)            \
   SCHED_PREDEFINED_GROUPS(LOCAL,          ROOT,                      \
        SCHED_ALLOC_TOTAL - 25, CPUSCHED_ALLOC_MAX_NONE, 10000,       \
        0, SCHED_CONFIG_NONE, 10000, SCHED_CONFIG_NONE, 0)            \
   SCHED_PREDEFINED_GROUPS(CLUSTER,        ROOT,                      \
        0, CPUSCHED_ALLOC_MAX_NONE,  10000,                           \
        0, SCHED_CONFIG_NONE, 10000, SCHED_CONFIG_NONE, 0)            \
                                                                      \
   SCHED_PREDEFINED_GROUPS(UW_NURSERY,     SYSTEM,                    \
        5, CPUSCHED_ALLOC_MAX_NONE,  1000,                            \
        32 * PAGES_PER_MB, SCHED_CONFIG_NONE, 10000, 32 * PAGES_PER_MB, 0) \
   SCHED_PREDEFINED_GROUPS(HELPER,         SYSTEM,                    \
        3, CPUSCHED_ALLOC_MAX_NONE,  5000,                            \
        0, 0, 10000, 0, 0)                                            \
   SCHED_PREDEFINED_GROUPS(DRIVERS,        SYSTEM,                    \
        3, CPUSCHED_ALLOC_MAX_NONE,  5000,                            \
        0, 0, 10000, 0, 0)                                            \

#undef SCHED_PREDEFINED_GROUPS


#define SCHED_PREDEFINED_GROUPS(_id, _pid, \
                                _cpuMin, _cpuMax, _cpuShares, \
                                _memMin, _memMax, _memShares, _memMinLimit, _memHardMax) \
    SCHED_GROUP_ID_##_id,

typedef enum {
   SCHED_PREDEFINED_GROUP_LIST
   SCHED_NUM_PREDEFINED_GROUPS
} Sched_PredefinedGroupID;

#undef SCHED_PREDEFINED_GROUPS

/*
 * Operations
 */

// iterators
typedef void (*Sched_ForAllGroupsFn)(Sched_Group *, void *);
void Sched_ForAllGroupsDo(Sched_ForAllGroupsFn f, void *data);

// tree synchronization
void Sched_TreeLock(void);
void Sched_TreeUnlock(void);
Bool Sched_TreeIsLocked(void);

// unsynchronized operations
//  (caller responsible for sched tree locking)
Sched_Node *Sched_TreeRootNode(void);
uint32 Sched_TreeNodeCount(void);
Sched_Group *Sched_TreeRootGroup(void);
Sched_Group *Sched_TreeGroupParent(const Sched_Group *group);
uint32 Sched_TreeGroupCount(void);
Sched_Group *Sched_TreeLookupGroup(Sched_GroupID id);
Sched_Group *Sched_TreeLookupGroupSlot(Sched_GroupID id);
void Sched_TreeGroupAddReference(Sched_Group *group);
void Sched_TreeGroupRemoveReference(Sched_Group *group);
#endif
