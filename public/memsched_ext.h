/* **********************************************************
 * Copyright 2000 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * memsched_ext.h --
 *
 *	VMKernel <-> VMM memory resource management info.
 */

#ifndef _MEMSCHED_EXT_H
#define _MEMSCHED_EXT_H

#define INCLUDE_ALLOW_VMX
#define INCLUDE_ALLOW_VMMEXT
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_VMCORE
#include "includeCheck.h"

#include "pshare_ext.h"
#include "memsched_defs.h"
#include "memsched_shared.h"

// Restrict overhead memory to 384M, partly because the sum
// of overhead memory and main memory cannot be more than 4GB
// This is due to bug #20955. Once it is fixed we can raise the
// overhead memory.

/*
 * types
 */

typedef struct {
   uint32 target;	// VMK -> VMM
   uint32 size;		// VMK <- VMM
   uint32 nOps;		// VMK <- VMM
   uint32 nReset;	// VMK <- VMM
   uint32 guestType;	// VMK <- VMM
} MemSched_BalloonInfo;

typedef struct {
   // VMK -> VMM
   Bool   enable;
   Bool   debug;
   uint32 scanRate;
   uint32 checkRate;

   // VMK <- VMM
   PShare_MonitorStats stats;
} MemSched_PShareInfo;

typedef struct {
   // remapping ops
   uint32 vmkAttempt;
   uint32 vmkRemap;
   uint32 migrateAttempt;
   uint32 migrateRemap;
   uint32 recolorAttempt;
   uint32 recolorRemap;

   // higher-level ops
   uint32 period;
   uint32 pickup;
   uint32 scan;
   uint32 stop;
} MemSched_RemapStats;

typedef struct {
   // VMK -> VMM
   uint32 migrateNodeMask;
   uint32 migrateScanRate;

   // VMK <- VMM
   MemSched_RemapStats stats;
} MemSched_RemapInfo;

typedef struct {
   MemSched_BalloonInfo  balloon;
   MemSched_SampleInfo   sample;
   MemSched_PShareInfo   pshare;
   MemSched_RemapInfo    remap;
} MemSched_Info;

#endif
