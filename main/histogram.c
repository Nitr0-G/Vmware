/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/* 
 * Histogram.c --
 *
 *	Source code for histogram data structure.
 *
 *      A Histogram (always accessed externally via Histogram_Handle) will
 *      track the distribution of data (int64's) inserted into it. Data are
 *      sorted into a user-defined number of buckets with user-defined upper
 *      bounds. Because a histogram dynamically allocates memory, you must be
 *      sure to use Histogram_New to initize one and Histogram_Delete to free
 *      it.  In addition to tracking bucket distribution, the histogram will
 *      also record min, max, and mean values.
 *
 *      Note that Histograms are not serialized, so the caller should provide
 *      its own locking.
 *
 */

#include "vm_types.h"
#include "vmkernel.h"
#include "vm_libc.h"
#include "histogram.h"
#include "proc.h"
#include "parse.h"

#define LOGLEVEL_MODULE Histogram
#include "log.h"


// Callers should always refer to this structure via a Histogram_Handle
struct Histogram {
   Histogram_Datatype min;
   Histogram_Datatype max;
   Histogram_Datatype total;
   
   uint64 count;
   uint32 numBuckets;
   Histogram_Datatype *bucketLimits;
   uint64 *bucketCounts;
};


/*
 *-----------------------------------------------------------------------------
 *
 * Histogram_Reset --
 *
 *     Resets all counts associated with this histogram to 0.
 *
 * Results:
 *     None
 *
 * Side effects:
 *     Modifies "histo" data
 *
 *-----------------------------------------------------------------------------
 */
void 
Histogram_Reset(Histogram_Handle histo)
{
   histo->count = 0;
   histo->min = 0;
   histo->max = 0;
   histo->total = 0;

   memset(histo->bucketCounts, 0, sizeof(uint64) * (histo->numBuckets));
}

/*
 *-----------------------------------------------------------------------------
 *
 * HistogramConfig --
 *
 *     Internal function to setup a histogram with the specified number of 
 *     buckets and bucketLimits. All data formerly associated with "histo"
 *     will be cleared. Does NOT free internal dynamic memory if it was
 *     previously allocated.
 *
 * Results:
 *     Returns VMK_OK on success, or VMK_NO_MEMORY
 *
 * Side effects:
 *     Allocates heap memory. 
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus 
HistogramConfig(Heap_ID heap, Histogram_Handle histo, uint32 numBuckets, Histogram_Datatype bucketLimits[])
{
   uint64 i;
   
   memset(histo, 0, sizeof(struct Histogram));

   histo->numBuckets = numBuckets;
   histo->bucketLimits = Heap_Alloc(heap, numBuckets * sizeof(Histogram_Datatype));
   if (histo->bucketLimits == NULL) {
      return VMK_NO_MEMORY;
   }

   histo->bucketCounts = Heap_Alloc(heap, numBuckets * sizeof(uint64));
   if (histo->bucketCounts == NULL) {
      Heap_Free(heap, histo->bucketLimits);
      return VMK_NO_MEMORY;
   }

   memset(histo->bucketLimits, 0, sizeof(Histogram_Datatype) * numBuckets);
   memset(histo->bucketCounts, 0, sizeof(uint64) * numBuckets);
   
   histo->numBuckets = numBuckets;

   for (i=0; i < numBuckets - 1; i++) {
      histo->bucketLimits[i] = bucketLimits[i];
   }

   histo->bucketLimits[numBuckets - 1] = 0xffffffffffffffffLL;
   
   return VMK_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 * Histogram_Reconfigure --
 *
 *     Resets the histogram and assigns new bucket limites and numBuckets.
 *
 * Results:
 *     Returns 
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
Histogram_Reconfigure(Heap_ID heap, Histogram_Handle histo, uint32 numBuckets, Histogram_Datatype bucketLimits[])
{
   Heap_Free(heap, histo->bucketCounts);
   Heap_Free(heap, histo->bucketLimits);
   return HistogramConfig(heap, histo, numBuckets, bucketLimits);
}

/*
 *-----------------------------------------------------------------------------
 *
 * Histogram_New --
 *
 *     Allocates a new histogram with the specified number of buckets
 *     and limits provided in the array "bucketLimits"
 *
 *     All memory used by this histogram, including memory allocated for
 *     later resizing, will come from "heap"
 *
 * Results:
 *     Returns the new histogram handle
 *
 * Side effects:
 *     Allocates heap memory (use Histogram_Delete to free it)
 *
 *-----------------------------------------------------------------------------
 */
Histogram_Handle
Histogram_New(Heap_ID heap, uint32 numBuckets, Histogram_Datatype bucketLimits[])
{
   Histogram_Handle histo = NULL;

   histo = (Histogram_Handle) Heap_Alloc(heap, sizeof(struct Histogram));
   if (histo == NULL) {
      return NULL;
   }

   if (HistogramConfig(heap, histo, numBuckets, bucketLimits) != VMK_OK) {
      LOG(0, "Failed to create histogram");
      Heap_Free(heap, histo);
      return NULL;
   }
   
   return histo;
}

/*
 *-----------------------------------------------------------------------------
 *
 * Histogram_Delete --
 *
 *     Frees all memory associated with this histogram, which should have
 *     come from "heap"
 *
 * Results:
 *     None.
 *
i * Side effects:
 *     Frees memory.
 *
 *-----------------------------------------------------------------------------
 */
void 
Histogram_Delete(Heap_ID heap, Histogram_Handle histo)
{
   if (histo != NULL) {
      Heap_Free(heap, histo->bucketLimits);
      Heap_Free(heap, histo->bucketCounts);
      Heap_Free(heap, histo);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * Histogram_Insert --
 *
 *     Stores a new datum in the histogram, incrementing the count in the 
 *     appropriate bucket and updating global statistics as appropriate.
 *
 * Results:
 *     None.
 *
 * Side effects:
 *     Modifes internal "histo" data
 *
 *-----------------------------------------------------------------------------
 */
void 
Histogram_Insert(Histogram_Handle histo, Histogram_Datatype datum)
{
   uint32 i;

   if (datum < histo->min || histo->count == 0) {
      histo->min = datum;
   }

   if (datum > histo->max || histo->count == 0) {
      histo->max = datum;
   }

   // OPT: could binary-search if we plan large numbers of buckets
   for (i=0; i < histo->numBuckets - 1 && datum > histo->bucketLimits[i]; i++)  {
      /* no loop body */ 
   } 
   
   histo->bucketCounts[i]++;
   
   histo->total += datum;
   histo->count++;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Histogram_MergeIn --
 *
 *	Merge the 'sourceHisto' into 'destHisto'.  Both histograms
 *	must have the exact same bucket limits.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      destHisto is munged left and right
 *
 *-----------------------------------------------------------------------------
 */
void
Histogram_MergeIn(Histogram_Handle destHisto, // IN/OUT
                  Histogram_Handle sourceHisto)  // IN
{
   int i;

   // must have same number of buckets in both histos
   ASSERT(destHisto->numBuckets == sourceHisto->numBuckets);
   
   if (sourceHisto->min < destHisto->min) {
      destHisto->min = sourceHisto->min;
   }
   if (sourceHisto->max > destHisto->max) {
      destHisto->max = sourceHisto->max;
   }
   destHisto->total += sourceHisto->total;
   destHisto->count += sourceHisto->count;
   
   for (i=0; i < destHisto->numBuckets; i++) {
      destHisto->bucketCounts[i] += sourceHisto->bucketCounts[i];
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * Histogram_Aggregate --
 *
 *      Returns a new histogram whose contents are the aggregation of all
 *      "numHandles" histograms in the list "handles." All histograms in
 *      "handles" must have the exact same bucket limits. The caller must
 *      free the returned histogram with Histogram_Delete.
 *
 *      The new histogram will use memory allocated from "heap"
 *
 * Results:
 *      Returns a new histogram or NULL if one could not be created
 *
 * Side effects:
 *      Allocates histogram.
 *
 *-----------------------------------------------------------------------------
 */
Histogram_Handle
Histogram_Aggregate(Heap_ID heap, Histogram_Handle *handles, unsigned numHandles)
{
   Histogram_Handle newHandle;
   unsigned numBuckets, i;
   
   if (numHandles < 0) {
      return (NULL);
   }

   // we assume that all histograms in the list have the same buckets
   numBuckets = handles[0]->numBuckets;
   newHandle = Histogram_New(heap, numBuckets, handles[0]->bucketLimits);

   if (newHandle == NULL) {
      return NULL;
   }
   
   newHandle->min = handles[0]->min;
   newHandle->max = handles[0]->max;
   
   for (i=0; i < numHandles; i++) {
      Histogram_MergeIn(newHandle, handles[i]);
   }

   return newHandle;
}

/*
 *-----------------------------------------------------------------------------
 *
 * Histogram_ProcFormat --
 *
 *      Writes statistics for "histo" (min/max/mean/count/buckets) to the
 *      provided buffer.
 *
 * Results:
 *      Writes to "buffer"
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
void
Histogram_ProcFormat(Histogram_Handle histo, const char* prefix, char *buffer, int *len)
{
   unsigned i;
   
   Proc_Printf(buffer, len, "%smin:   %Ld\n", prefix, Histogram_Min(histo));
   Proc_Printf(buffer, len, "%smax:   %Ld\n", prefix, Histogram_Max(histo));
   Proc_Printf(buffer, len, "%scount: %Lu\n", prefix, Histogram_Count(histo));
   Proc_Printf(buffer, len, "%smean:  %Ld\n\n", prefix, Histogram_Mean(histo));
   
   for (i=0; i < histo->numBuckets - 1; i++) {
      Proc_Printf(buffer, len, "%s%-18Lu (<= %18Ld)\n",
                  prefix, 
                  histo->bucketCounts[i], 
                  histo->bucketLimits[i]);
   }

   Proc_Printf(buffer, len, "%s%-18Lu (>  %18Ld)\n",
               prefix, 
               histo->bucketCounts[histo->numBuckets - 1],
               histo->bucketLimits[histo->numBuckets - 2]);
   
}

/*
 *-----------------------------------------------------------------------------
 *
 * Histogram_ProcRead --
 *
 *     Simple proc read handler. The proc entry's "private" field should
 *     hold a Histogram_Handle. This will simply display all the buckets,
 *     the count for each, and the basic summary statistics for the histogram.
 *
 * Results:
 *     Returns VMK_OK.
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
int 
Histogram_ProcRead(Proc_Entry *entry, char *buffer, int *len)
{
   Histogram_Handle histo = (Histogram_Handle) entry->private;

   ASSERT(histo != NULL);
   
   *len = 0;

   Histogram_ProcFormat(histo, "", buffer, len);
   
   return VMK_OK;
}


// Simple query operations

/*
 *-----------------------------------------------------------------------------
 *
 * Histogram_NumBuckets --
 *
 *     Returns the number of buckets in the histogram
 *
 * Results:
 *     Returns number of buckets
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
uint32
Histogram_NumBuckets(Histogram_Handle histo)
{
   return histo->numBuckets;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Histogram_Max --
 *
 *     Returns the highest value inserted into this histogram
 *
 * Results:
 *     Returns max value
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
Histogram_Datatype 
Histogram_Max(Histogram_Handle histo)
{
   return histo->max;
}

/*
 *-----------------------------------------------------------------------------
 *
 * Histogram_Min --
 *
 *     Returns the lowest value inserted into this histogram
 *
 * Results:
 *     Returns min value
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
Histogram_Datatype 
Histogram_Min(Histogram_Handle histo)
{
   return histo->min;
}

/*
 *-----------------------------------------------------------------------------
 *
 * Histogram_Mean --
 *
 *     Returns the arithmetic mean of all values inserted into this histogram
 *
 * Results:
 *     Returns mean value
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
Histogram_Datatype 
Histogram_Mean(Histogram_Handle histo)
{
   if (histo->count > 0) {
      return histo->total / (Histogram_Datatype)histo->count;
   } else {
      return 0;
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * Histogram_Count --
 *
 *     Returns the number of items inserted into this histogram
 *
 * Results:
 *     Returns item count
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
uint64 
Histogram_Count(Histogram_Handle histo) 
{
   return histo->count;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Histogram_BucketCount --
 *
 *     Returns the number of items in the specified bucket (bucket)
 *
 * Results:
 *     Returns count of items in "bucket"
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
uint64 
Histogram_BucketCount(Histogram_Handle histo, uint32 bucket)
{
   ASSERT(bucket < histo->numBuckets);
   return histo->bucketCounts[bucket];
}


/*
 *-----------------------------------------------------------------------------
 *
 * Histogram_BucketLimit --
 *
 *     Returns the upper limit of "bucket"
 *
 * Results:
 *     Returns bucket limit
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
Histogram_Datatype 
Histogram_BucketLimit(Histogram_Handle histo, uint32 bucket)
{
   ASSERT(bucket < histo->numBuckets);
   return histo->bucketLimits[bucket];
}
