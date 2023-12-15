/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * cpuMetrics.h --
 *
 *	Load metrics for CPU resources.
 */

#ifndef _CPUMETRICS_H
#define _CPUMETRICS_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "proc.h"

/*
 * Types
 */

struct CpuMetrics_LoadHistory;

/*
 * Operations
 */

// initialization
void CpuMetrics_Init(Proc_Entry *dir);

// load history operations
struct CpuMetrics_LoadHistory *CpuMetrics_LoadHistoryNew(void);
void CpuMetrics_LoadHistoryDelete(struct CpuMetrics_LoadHistory *h);
void CpuMetrics_LoadHistoryReset(struct CpuMetrics_LoadHistory *h);
void CpuMetrics_LoadHistorySampleDelta(struct CpuMetrics_LoadHistory *h,
                                       Timer_Cycles run,
                                       Timer_Cycles ready);
void CpuMetrics_LoadHistorySampleCumulative(struct CpuMetrics_LoadHistory *h,
                                            Timer_Cycles totalRun,
                                            Timer_Cycles totalReady,
                                            Timer_Cycles *deltaRun,
                                            Timer_Cycles *deltaReady);

#endif
