/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * action_ext.h --
 *
 *	VMKernel to VMM action queue.
 */

#ifndef _ACTION_EXT_H
#define _ACTION_EXT_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMMEXT
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMCORE
#include "includeCheck.h"

#include "vm_atomic.h"
#include "world_ext.h"
#include "vcpuid.h"

#define NUM_ACTIONS    32
#define ACTION_INVALID (NUM_ACTIONS + 1)

// Action_NotifyInfo contains hints indicating when the
// vmkernel should be notified of a monitor action post.
// When vcpuHint[v] is non-zero, it indicates that the
// vcpu identified by "v" wants the vmkernel to be notified
// when a monitor action is pending for it. Since there are
// separate shared areas between each vcpu's vmm and the vmk,
// this data is replicated into each shared area. A uint32
// is used when a single bit would suffice in order to avoid
// the need for atomic operations.
typedef struct {
   uint32 vcpuHint[MAX_VCPUS];
} Action_NotifyInfo;

/*
 * Pending actions are now stored in a two level tree.  
 * The actionStatus field is shared with the monitor.  If 
 * the vmkernel bit in the actionStatus vector is set,
 * then one or more bits in vector is also set.
 */ 
typedef struct Action_Info {
   Atomic_uint32       *actionStatus;
   Atomic_uint32        vector;
   Action_NotifyInfo    notify;
   Atomic_uint32       *actionStatusMapped;
} Action_Info;


#endif

