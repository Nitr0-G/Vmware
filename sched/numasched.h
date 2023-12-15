/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * numasched.h --
 *
 *	NUMA scheduler module
 */

#ifndef _NUMASCHED_H
#define _NUMASCHED_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "sched_ext.h"
#include "world_ext.h"
#include "vmkernel_dist.h"
#include "proc.h"
#include "numa.h"

typedef struct {
   uint32 nBalanceMig;
   uint32 nLocalitySwap;

   uint32 nPageMigOn;
   uint32 nPageMigIncr;
   uint32 nPageMigDecr;

   uint32 nMonMigs;
   
   uint32 nPageMigOff;

   // track a history of memory locality
   uint64 remotePages;
   uint64 localPages;
   
   Timer_Cycles minuteAgoCycles;
   uint64 minuteAgoLocal;
   uint64 minuteAgoRemote;

   Timer_Cycles hourAgoCycles;
   uint64 hourAgoLocal;
   uint64 hourAgoRemote;

   // not used by global stats
   uint32 nodeRunCounts[NUMA_MAX_NODES];
} NUMASched_Stats;

typedef struct {
   Timer_Cycles prevRun;
   Timer_Cycles prevReady;
   Timer_Cycles prevWait;
   uint8 shortTermSamples;
   uint8 shortTermHistory[NUMA_MAX_NODES];
   uint32 longTermHistory[NUMA_MAX_NODES];

   NUMASched_Stats stats;
   Proc_Entry procWorldNUMA;

   Timer_AbsCycles nextMigrateAllowed;
   Timer_AbsCycles lastMigrateTime;
   NUMA_Node lastMonMigMask;

   NUMA_Node homeNode;
} NUMASched_VsmpInfo;

typedef struct {
   // basic info
   World_ID leaderID;
   uint8 numVcpus;
   uint32 shares;
   NUMA_Node node;
   
   // accounting for last time interval
   Timer_Cycles runDiff;
   Timer_Cycles readyDiff;
   Timer_Cycles waitDiff;

   // affinity
   Bool hardCpuAffinity;
   Bool hardMemAffinity;
   Bool jointAffinity;
   CpuMask totalCpuAffinity;

   // misc flags
   Bool valid;
   Bool justMigrated;
   Bool historyUpdate;
   Bool isVMMWorld; 

   // maintain an aged, running history of 
   // where this vsmp has executed
   uint32 longTermHistory[NUMA_MAX_NODES];

   // used internally by NUMASched
   int64 owed;
   int64 entitled;
} NUMASched_VsmpSnap;

typedef struct NUMASched_Snap {
   Timer_Cycles nodeIdleTotal[NUMA_MAX_NODES];
   NUMASched_VsmpSnap vsmps[MAX_WORLDS];

   uint32 totalShares;
   uint32 numVsmps;
} NUMASched_Snap;

struct CpuSched_Vcpu;
struct CpuSched_Vsmp;

/*
 * operations
 */

PCPU NUMASched_InitialPlacement(const struct CpuSched_Vcpu *vcpu);
void NUMASched_SetInitialHomeNode(struct World_Handle *world);
void NUMASched_VsmpNUMASnap(struct CpuSched_Vsmp *vsmp, NUMASched_VsmpSnap *info);
CpuMask NUMASched_GetNodeMask(NUMA_Node n);
void NUMASched_Init(Proc_Entry *entry);
void NUMASched_AddWorldProcEntries(struct World_Handle *world, Proc_Entry *procDir);
void NUMASched_RemoveWorldProcEntries(struct World_Handle *world);
#endif
