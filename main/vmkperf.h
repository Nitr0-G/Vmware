/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * vmkperf.h --
 *
 *	Functions for Vmkperf performance counters.
 */

#ifndef _VMKPERF_H
#define _VMKPERF_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "smp.h"
#include "x86perfctr.h"
#include "config.h"

/*
 * lock ranks
 */
#define SP_RANK_VMKPERF_USEDCOUNTER (SP_RANK_IRQ_MEMTIMER)

void Vmkperf_Init(void);
void Vmkperf_Cleanup(void);

// By default, activate counters in all builds
#if !defined(VMKPERF_DISABLE)
#define VMKPERF_ENABLE_COUNTERS
#endif

#define INVALID_COUNTER_SENTRY 0xffffffff

extern const uint32 PENTIUM4_COUNTERSET_BPU0[];
extern const uint32 PENTIUM4_COUNTERSET_BPU1[];
extern const uint32 PENTIUM4_COUNTERSET_IQ0[];
extern const uint32 PENTIUM4_COUNTERSET_IQ1[];

struct VmkperfEventInfo;

Bool Vmkperf_LockESCR(uint32 escrAddr);
VMK_ReturnStatus Vmkperf_PerfCtrConfig(const char *eventName, PerfCtr_Config *ctr);
void Vmkperf_FreePerfCtr(PerfCtr_Config *ctr);

void Vmkperf_InitWorld(World_Handle* world);
void Vmkperf_CleanupWorld(World_Handle* world);
void Vmkperf_WorldSave(World_Handle* save);
void Vmkperf_WorldRestore(World_Handle* restore);
void Vmkperf_ResetCounter(World_Handle* world, uint32 counterNum);
uint32 Vmkperf_GetP6Event(const char *eventName);
uint32 Vmkperf_GetDefaultPeriod(const char *eventName);
void Vmkperf_PrintCounterList(char *buffer, int *len);
const char* Vmkperf_GetCanonicalEventName(const char *eventName);
uint64 Vmkperf_GetWorldEventCount(World_Handle *world,
                                  struct VmkperfEventInfo *info);
struct VmkperfEventInfo* Vmkperf_GetEventInfo(const char *eventName);
VMK_ReturnStatus Vmkperf_SetEventActive(struct VmkperfEventInfo *info, Bool active);
void Vmkperf_SetSamplerRate(uint32 sampleMs);
uint64 Vmkperf_ReadLocalCounter(struct VmkperfEventInfo *event, Bool hypertwin);

struct VmkperfWorldCounterInfo;
typedef struct VmkperfWorldCounterInfo* Vmkperf_WorldInfo;

static INLINE Bool Vmkperf_TrackPerWorld(void)
{
   return (CONFIG_OPTION(VMKPERF_PER_WORLD)
      || (SMP_HTEnabled()
          && CONFIG_OPTION(CPU_MACHINE_CLEAR_THRESH) > 0));
}

static INLINE void Vmkperf_WorldSwitch(World_Handle *restore, World_Handle *save)
{
   if (Vmkperf_TrackPerWorld()) {
      Vmkperf_WorldSave(save);
      Vmkperf_WorldRestore(restore);
   }
}



#endif 

