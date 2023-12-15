/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * nmi.h -- 
 *
 *	Non-maskable interrupt definitions.
 */

#ifndef	_NMI_H
#define	_NMI_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "nmi_ext.h"
#include "world.h"

typedef enum NMISource {
   NMI_FROM_VMKERNEL = 1,
   NMI_FROM_COS,
   NMI_FROM_COS_USER,
   NMI_FROM_USERMODE,
} NMISource;

typedef struct NMIContext {
   uint32 eip;
   uint16 cs;
   uint32 esp;
   uint16 ss;
   uint32 ebp;
   uint32 eflags;
   NMISource source;
} NMIContext;

/*
 * NMI operations
 */
extern void NMI_Init(void);
extern void NMI_Interrupt(NMIContext *nmiContext);

extern void NMI_SamplerChange(Bool turnOn);
extern VMK_ReturnStatus NMI_SamplerSetConfig(const char *event, uint32 period);
extern uint32 NMI_SamplerGetPeriod(void);
extern const char* NMI_SamplerGetEventName(void);

extern void NMI_WatchdogTurnOn(void);

extern uint32 NMI_GetAverageSamplerCycles(void);
extern void NMI_ResetAverageSamplerCycles(void);
extern void NMI_GetPerfCtrConfig(PerfCtr_Config *ctr);

extern Bool NMI_IsEnabled(void);

extern Bool NMI_Mask(void);
extern void NMI_Unmask(void);

extern void NMI_TaskToNMIContext(const Task *task, NMIContext *nmiContext);

extern void NMI_PatchTask(Task *task);

static INLINE Bool NMI_IsNMIStack(uint32 addr, World_Handle *world) {
   return (addr > world->nmiStackStart &&
           addr < (world->nmiStackStart + PAGE_SIZE - 1));
}

static INLINE void NMI_Enable(void)
{
   extern void NMIEnableInt(void);
   if (PRDA_IsInitialized() && myPRDA.configNMI) {
      NMIEnableInt();
   }
}
static INLINE void NMI_Disable(void)
{
   extern void NMIDisableInt(void);
   if (PRDA_IsInitialized() && myPRDA.configNMI) {
      NMIDisableInt();
   }
}

static INLINE Bool NMI_IsCPUInNMI(void) 
{
   return PRDA_IsInitialized() && myPRDA.inNMI;
}

extern Bool NMIInited;
extern Bool NMI_Pending;
extern void NMI_Disallow(void);

#define NMI_SAMPLER_DEFAULT_PERIOD ((uint32)-1)

#endif
