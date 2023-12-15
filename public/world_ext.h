/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * world_ext.h --
 *
 *	External definitions for worlds.
 */

#ifndef _WORLD_EXT_H
#define _WORLD_EXT_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_VMMEXT
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMCORE
#include "includeCheck.h"

#include "vmk_basic_types.h"

#define SERVER_MAX_VCPUS 80

// with worst case all 1VCPU VMs
#define SERVER_MAX_VMS SERVER_MAX_VCPUS

#if defined(MAX_VMS) && (MAX_VMS != SERVER_MAX_VMS)
#error "MAX_VMS must equal SERVER_MAX_VCPUS."
#endif

/*
 * Number of vmkernel worlds per VM =
 * (3 + 2*n + numFloppy + numCdroms) where n is number of VCPUs
 * 3 = (VMX main thread +  MKS thread + pthread manager)
 * 2 = VMX thread + VMM world
 * Worst case for worlds per VCPU is a single VCPU VM, in which case
 * you get 3 + 2*1 + numFloppy + numCdrom.
 * Assuming 1 floppy and cdrom, you get 7, but going with 10 to leave some
 * space.
 */
#define MAX_WORLDS_PER_VCPU 10

/*
 * MISC + SUSPEND_RESUME + FAILOVER worlds
 */
#define NUM_HELPER_WORLDS 6

/*
 * Some scsi drivers create kernel threads (aka "driver" worlds).
 * 5 seems like a nice, round, and probably incorrect value.
 */
#define NUM_DRIVER_WORLDS 5

/*
 * Hot migration needs some worlds, 3 should be enough to migrate
 * a vm off of a completely maxed out server.
 */
#define NUM_MIGRATE_WORLDS  3

#define NUM_HOST_WORLDS 1

#define NUM_IDLE_WORLDS MAX_PCPUS

/*
 * Number of UserWorld applications.
 */
#define MAX_USERWORLD_APPS  0

/*
 * Try to calculate how many worlds are needed.
 *
 * TOTAL = 847
 */
#define MAX_REQUIRED_WORLDS  (SERVER_MAX_VCPUS*MAX_WORLDS_PER_VCPU +   \
                              MAX_SCSI_ADAPTERS + NUM_HELPER_WORLDS +  \
                              NUM_DRIVER_WORLDS + NUM_MIGRATE_WORLDS + \
                              NUM_HOST_WORLDS + NUM_IDLE_WORLDS + \
                              MAX_USERWORLD_APPS)

// MAX_WORLDS should be a power of 2
#define MAX_WORLDS 1024
#if MAX_REQUIRED_WORLDS > MAX_WORLDS
#error "too few worlds"
#endif

/* 
 * A memsched ID (MemID) uniquely identifies a VM or a userworld application.
 */
typedef World_ID MemSched_ID;

#define WORLD_GROUP_DEFAULT	INVALID_WORLD_ID

/*
 * VMX debug data saved by VMMon_SetVMXInfo() in World_GroupInfo
 */
#define WORLD_MAX_CONFIGFILE_SIZE	256
#define WORLD_MAX_UUIDTEXT_SIZE		128
#define WORLD_MAX_DISPLAYNAME_SIZE	128

#endif
