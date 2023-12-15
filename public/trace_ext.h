/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * trace_ext.h --
 *
 *    Externally-includeable header for vmkernel trace facility.
 *    Contains definitions of data types and constants.
 *    Include vmkernel/main/trace.h if you actually want to annotate your
 *    code with trace events.
 */

#ifndef TRACE_EXT_H
#define TRACE_EXT_H

#include "vm_basic_types.h"
#include "scsi_ext.h" // for MAX_SCSI_ADAPTERS (needed by MAX_WORLDS)
#include "vmkernel_ext.h"
#include "world_ext.h"
#include "return_status.h"
#include "vmkernel_dist.h"


typedef enum {
   TRACE_KEY_PCPU=0,
   TRACE_KEY_WORLD,
   TRACE_KEY_MAXTYPE
} Trace_Key;

// packed to be 32 bytes
typedef struct {
   uint64 timestamp;
   World_ID wid;
   uint32 eclass;
   uint16 id : 16;
   uint16 pcpu : 16;
   int custom;
   int64 data;
} TraceEvent;

// predefine constants for fundamental scheduler events
// because we need to treat them specially in the GUI
#define TRACE_SCHED_PCPU_ID  (0)
#define TRACE_SCHED_WORLD_ID (1)

#define TRACE_BUFFER_LEN (4000)
#define TRACE_META_BUFFER_LEN (2000)

#define TRACE_MAX_NAME_LEN (30)

#define ARR_BUF(TYP, LEN) typedef struct { \
   int count;            \
   TYP entries[LEN];        \
} 
   
typedef struct {
   int numEvents;
   TraceEvent events[TRACE_BUFFER_LEN];
} Trace_DataBuffer;

typedef struct {
   int id;
   char name[TRACE_MAX_NAME_LEN+1];
   int isEnabled;
} TraceClassDef;

typedef struct {
   int eclass;
   int id;
   int defaultKey;
   char name[TRACE_MAX_NAME_LEN+1];
   int pointEvent;
} TraceEventDef;

typedef struct {
   int tagId;
   int eclass;
   char name[TRACE_MAX_NAME_LEN+1];
} TraceCustomTag;

typedef struct {
   int vmid;
   int gid;
   char name[TRACE_MAX_NAME_LEN+1];
} WorldDesc;

typedef struct {
   Bool active;
   int khzEstimate;
   int bufSize;
   int circular;
   int numPcpus;
   int numEvents;
   int numTypes;
   int numWorlds;
   int numClasses;
   int numCustomTags;
} Trace_MetadataBuffer;


ARR_BUF(TraceEvent, TRACE_BUFFER_LEN) Trace_EventBuffer;
ARR_BUF(TraceEventDef, TRACE_META_BUFFER_LEN) Trace_EventDefBuffer;
ARR_BUF(WorldDesc, TRACE_META_BUFFER_LEN) Trace_WorldDescBuffer;
ARR_BUF(TraceClassDef, TRACE_META_BUFFER_LEN) Trace_ClassDefBuffer;
ARR_BUF(TraceCustomTag, TRACE_META_BUFFER_LEN) Trace_CustomTagBuffer;
#endif
