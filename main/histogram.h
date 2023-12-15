/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * histogram.h --
 *
 *	Header for histogram data structure library.
 */

#ifndef _HISTOGRAM_H
#define _HISTOGRAM_H

#include "vm_types.h"
#include "proc_dist.h"
#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"
#include "heap_public.h"

#define HISTOGRAM_BUCKETS_MAX (40)

struct Histogram;
typedef struct Histogram* Histogram_Handle;

typedef int64 Histogram_Datatype;

EXTERN Histogram_Handle Histogram_New(Heap_ID heap, uint32 numBuckets, Histogram_Datatype bucketLimits[]);
EXTERN void Histogram_Delete(Heap_ID heap, Histogram_Handle histo);

EXTERN void Histogram_Insert(Histogram_Handle histo, Histogram_Datatype datum);
EXTERN Histogram_Datatype Histogram_Max(Histogram_Handle histo);
EXTERN Histogram_Datatype Histogram_Min(Histogram_Handle histo);
EXTERN Histogram_Datatype Histogram_Mean(Histogram_Handle histo);
EXTERN uint64 Histogram_Count(Histogram_Handle histo);
EXTERN uint64 Histogram_BucketCount(Histogram_Handle histo, uint32 bucket);
EXTERN Histogram_Datatype Histogram_BucketLimit(Histogram_Handle histo, uint32 bucket);
EXTERN uint32 Histogram_NumBuckets(Histogram_Handle histo);

EXTERN int Histogram_ProcRead(Proc_Entry *entry, char *buffer, int *len);
EXTERN int Histogram_BucketsProcWrite(Proc_Entry *entry, char *buffer, int *len);
EXTERN void Histogram_Reset(Histogram_Handle histo);
EXTERN VMK_ReturnStatus Histogram_Reconfigure(Heap_ID heap, Histogram_Handle histo, uint32 numBuckets, Histogram_Datatype bucketLimits[]);
EXTERN Histogram_Handle Histogram_Aggregate(Heap_ID heap, Histogram_Handle *handles, unsigned numHandles);
EXTERN void Histogram_MergeIn(Histogram_Handle histo, Histogram_Handle otherHisto);
EXTERN void Histogram_ProcFormat(Histogram_Handle handle, const char* prefix, char *buffer, int *len);
#endif
