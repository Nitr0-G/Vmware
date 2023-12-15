/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * eventhisto.h --
 *
 *	Header for event time histogram management
 */

#ifndef _EVENTHISTO_H
#define _EVENTHISTO_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "vm_types.h"
#include "vmkernel.h"
#include "vm_libc.h"
#include "histogram.h"

EXTERN Bool eventHistoActive;

EXTERN void EventHisto_Register(uint32 addr);
EXTERN void EventHisto_AddSampleReal(uint32 addr, int64 cycles);
EXTERN void EventHisto_LateInit(void);
EXTERN void EventHisto_Init(void);

static INLINE void
EventHisto_AddSample(uint32 addr, int64 cycles)
{
   if (eventHistoActive) {
      EventHisto_AddSampleReal(addr, cycles);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * EventHisto_StartSample --
 *
 *      Returns the current tsc time iff event histo is active.
 *      Note that this should not be used for events that need to be tracked
 *      across multiple processors, as it'll get confused on NUMA boxes.
 *
 * Results:
 *      Returns tsc or 0
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE int64
EventHisto_StartSample(void)
{
   if (eventHistoActive) {
      return RDTSC();
   } else {
      return (0);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * EventHisto_EndSample --
 *
 *      Records a sample for event "addr" that ends now and began at "startTime"
 *      If startTime is 0, or eventHisto is disabled the sample will be ignored.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May record a sample.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE void
EventHisto_EndSample(uint32 addr, int64 startTime)
{
   if (eventHistoActive && startTime != 0) {
      EventHisto_AddSampleReal(addr, (int64)RDTSC() - startTime);
   }
}

#endif
