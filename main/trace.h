/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * trace.h --
 *
 *    Internal header for vmkernel trace module. Should be included
 *    by vmkernel code that wants to record trace data.
 *
 */

#include "trace_ext.h"
#define TRACE_MODULE_ACTIVE (vmx86_debug)


#define TRACE_CLASS_LIST \
  TRACE_C(TRACE_SCHED_PCPU, "SCHED_PCPU") \
  TRACE_C(TRACE_SCHED_WORLD, "SCHED_WORLD") \
  TRACE_C(TRACE_SCHED_DATA, "SCHED_DATA") \
  TRACE_C(TRACE_VMKPERF, "VMKPERF") \
  TRACE_C(TRACE_RXCLUSTER, "RXCLUSTER") \
  TRACE_C(TRACE_INTERRUPT, "INTERRUPT") \
  TRACE_C(TRACE_UWSYSCALL, "UWSYSCALL") \
  TRACE_C(TRACE_RPC, "RPC") \
  TRACE_C(TRACE_VMMVMKCALL, "VMKCALL") \
  TRACE_C(TRACE_SCHED_QUANTUM, "QUANTUM")\
  TRACE_C(TRACE_HOST_INTERRUPT, "HOST_INTR")\

#define TRACE_EVENT_LIST \
  TRACE_E( TRACE_SCHED_PCPU, TRACE_SCHED_PCPU_RUN, TRACE_KEY_PCPU, "RUN") \
  TRACE_E( TRACE_SCHED_PCPU, TRACE_SCHED_PCPU_BWAIT, TRACE_KEY_PCPU, "BWAIT") \
  TRACE_E( TRACE_SCHED_PCPU, TRACE_SCHED_PCPU_IDLE, TRACE_KEY_PCPU, "IDLE") \
  TRACE_E( TRACE_SCHED_WORLD, TRACE_SCHED_STATE_NEW, TRACE_KEY_WORLD, "NEW") \
  TRACE_E( TRACE_SCHED_WORLD, TRACE_SCHED_STATE_ZOMBIE, TRACE_KEY_WORLD, "ZOMBIE") \
  TRACE_E( TRACE_SCHED_WORLD, TRACE_SCHED_STATE_RUN, TRACE_KEY_WORLD, "RUN") \
  TRACE_E( TRACE_SCHED_WORLD, TRACE_SCHED_STATE_READY, TRACE_KEY_WORLD, "READY") \
  TRACE_E( TRACE_SCHED_WORLD, TRACE_SCHED_STATE_READY_CORUN, TRACE_KEY_WORLD, "CORUN") \
  TRACE_E( TRACE_SCHED_WORLD, TRACE_SCHED_STATE_READY_COSTOP, TRACE_KEY_WORLD,  "COSTOP") \
  TRACE_E( TRACE_SCHED_WORLD, TRACE_SCHED_STATE_WAIT, TRACE_KEY_WORLD, "WAIT") \
  TRACE_E( TRACE_SCHED_WORLD, TRACE_SCHED_STATE_BUSY_WAIT, TRACE_KEY_WORLD, "BWAIT") \
  TRACE_E( TRACE_SCHED_DATA, TRACE_SCHED_INTRASKEW, TRACE_KEY_WORLD, "IntraSkew") \
  TRACE_E( TRACE_SCHED_DATA, TRACE_SCHED_INTRASKEW_OUT, TRACE_KEY_WORLD, "IntraSkewOut") \
  TRACE_E( TRACE_VMKPERF, TRACE_VMKPERF_SAMPLE, TRACE_KEY_PCPU, "VmkperfEvents") \
  TRACE_E( TRACE_RXCLUSTER, TRACE_RXCLUSTER_PENDING, TRACE_KEY_WORLD, "PktsPending") \
  TRACE_E( TRACE_RXCLUSTER, TRACE_RXCLUSTER_RECVD, TRACE_KEY_WORLD, "PktsRecvd") \
  TRACE_E( TRACE_RXCLUSTER, TRACE_RXCLUSTER_STATECHANGE, TRACE_KEY_WORLD, "RxClusterState") \
  TRACE_E( TRACE_RXCLUSTER, TRACE_RXCLUSTER_RETURNED, TRACE_KEY_WORLD, "PktsReturned")  \
  TRACE_E( TRACE_INTERRUPT, TRACE_INTERRUPT_DEVICE, TRACE_KEY_PCPU, "Interrupt", TRUE)  \
  TRACE_E( TRACE_UWSYSCALL, TRACE_USERWORLD_SYSCALL, TRACE_KEY_WORLD, "UWSyscall")  \
  TRACE_E( TRACE_UWSYSCALL, TRACE_USERWORLD_VMKCALL, TRACE_KEY_WORLD, "UWVmkCall")  \
  TRACE_E( TRACE_VMMVMKCALL, TRACE_VMM_VMKCALL, TRACE_KEY_WORLD, "VmmVmkCall")  \
  TRACE_E( TRACE_RPC, TRACE_RPC_GET, TRACE_KEY_WORLD, "RPCGET")  \
  TRACE_E( TRACE_RPC, TRACE_RPC_DONE, TRACE_KEY_WORLD, "RPCDONE")  \
  TRACE_E( TRACE_SCHED_QUANTUM, TRACE_SCHED_QUANTUM_REMAIN, TRACE_KEY_WORLD, "SchedQuantum") \
  TRACE_E( TRACE_HOST_INTERRUPT, TRACE_HOST_INTR, TRACE_KEY_WORLD, "HostIntr", TRUE) \

#define TRACE_C(ID, NAME) ID,

typedef enum {
   TRACE_CLASS_LIST
   TRACE_CLASS_MAX
} TraceEventClass;

#define TRACE_E(CLS, ID, _ARGS...) ID,

typedef enum {
   TRACE_EVENT_LIST
   TRACE_EVENT_MAX
} TraceEventID;

extern Bool traceActive;
extern Bool traceEventEnabled[TRACE_EVENT_MAX];

EXTERN void Trace_RegisterCustomTag(int eclass, uint32 customVal, const char *customTag);
EXTERN void Trace_UnregisterCustomTag(int eclass, uint32 customVal);
EXTERN void Trace_RecentWorldDeath(const World_Handle *w);

EXTERN void Trace_EventInt(TraceEventID id, World_ID wid, PCPU p, uint32 custom, int64 data, uint64 ts);

static INLINE void
Trace_Event(TraceEventID id, World_ID wid, PCPU p, uint32 custom, int64 data)
{
   if (UNLIKELY(TRACE_MODULE_ACTIVE && traceActive && traceEventEnabled[id])) {
      Trace_EventInt(id, wid, p, custom, data, -1);
   }
}

static INLINE void
Trace_EventLocal(TraceEventID id, uint32 custom, uint32 data)
{
   Trace_Event(id, MY_RUNNING_WORLD->worldID, MY_PCPU, custom, data);
}

static INLINE void
Trace_EventWithTimestamp(TraceEventID id, World_ID wid, PCPU p, uint32 custom, int64 data, uint64 ts)
{
   if (UNLIKELY(TRACE_MODULE_ACTIVE && traceActive && traceEventEnabled[id])) {
      Trace_EventInt(id, wid, p, custom, data, ts);
   }
}

EXTERN void Trace_Init(void);
EXTERN VMK_ReturnStatus Trace_GetBatchData(int *index, Trace_DataBuffer *buf, unsigned long outBufLen);
EXTERN VMK_ReturnStatus Trace_GetMetadata(int *unused, Trace_MetadataBuffer *buf, unsigned long outBufLen);

EXTERN VMK_ReturnStatus Trace_GetEventDefs(int *index, Trace_EventDefBuffer *buf, unsigned long outBufLen);
EXTERN VMK_ReturnStatus Trace_GetEventClasses(int *unused, Trace_ClassDefBuffer *buf, unsigned long outBufLen);
EXTERN VMK_ReturnStatus Trace_GetCustomTags(int *unused, Trace_CustomTagBuffer *buf, unsigned long outBufLen);
EXTERN VMK_ReturnStatus Trace_GetWorldDescs(int *unused, Trace_WorldDescBuffer *buf, unsigned long outBufLen);
