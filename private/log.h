/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * log.h --
 *
 *	vmkernel logging macros
 */

#ifndef _LOG_H
#define _LOG_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"
#include "log_dist.h"
#include "vmkernel_ext.h"
#include "vmkernel_dist.h"

typedef enum {
#define LOGLEVEL_DEF(name, len) PLACEHOLDER_LOGLEVEL_##name,
#include "logtable_dist.h"
#define LOGLEVEL_DEF(name, len) LOGLEVEL_##name,
#include "logtable.h"
} LogVals;

struct VMnix_ConfigOptions;
struct World_Handle;
struct VMnix_SharedData;

VMKERNEL_ENTRY Log_VMMLog(DECLARE_ARGS(VMK_VMM_SERIAL_LOGGING));

/*
 * Event Logging
 */

typedef enum {
   EVENTLOG_CPUSCHED,
   EVENTLOG_CPUSCHED_COSCHED,
   EVENTLOG_CPUSCHED_HALTING,
   EVENTLOG_TIMER,
   EVENTLOG_TESTWORLDS,
   EVENTLOG_VMKSTATS,
   EVENTLOG_OTHER,
   EVENTLOG_MAX_TYPE
} EventLogType;

// Turn EVENTLOG on by default in all build types
#define VMX86_ENABLE_EVENTLOG (1)

void Log_EventLogSetTypeActive(EventLogType evtType, Bool activate);
#ifdef	VMX86_ENABLE_EVENTLOG
extern Bool eventLogActiveTypes[];

extern void Log_EventInt(const char *eventName, int64 eventData, EventLogType eventType);

static INLINE void 
Log_Event(const char *eventName, int64 eventData, EventLogType eventType)
{
   // our current settings may say not to deal with this event type
   ASSERT(eventType < EVENTLOG_MAX_TYPE);
   if (UNLIKELY(eventLogActiveTypes[eventType])) {
      Log_EventInt(eventName, eventData, eventType);
   }
}
#else
static INLINE void
Log_Event(const char *eventName, int64 eventData, EventLogType eventType)
{
}
#endif


#endif
