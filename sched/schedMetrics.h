/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * schedMetrics.h --
 *
 *	Scheduler load metrics.
 */

#ifndef _SCHED_METRICS_H
#define _SCHED_METRICS_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

/*
 * Constants
 */

// fixed-point constants (12-bit fractional part)
#define	FIXEDNUM_1_LG		(12)
#define	FIXEDNUM_1		(1 << FIXEDNUM_1_LG)

/*
 * Types
 */

// fixed-point binary number
typedef uint64 FixedNum;

// fixed-point decimal number
typedef struct {
   uint32 whole;
   uint32 milli;
} DecimalNum;

typedef struct {
   FixedNum value;
   FixedNum avg1;
   FixedNum avg5;
   FixedNum avg15;
} FixedAverages;

typedef struct {
   uint32 exp1;
   uint32 exp5;
   uint32 exp15;
} FixedAverageDecays;

typedef struct {
   DecimalNum value;
   DecimalNum avg1;
   DecimalNum avg5;
   DecimalNum avg15;
} DecimalAverages;

/*
 * Operations
 */

/*
 *-----------------------------------------------------------------------------
 *
 * IntToFixedNum --
 *
 *      Returns fixed-point number representation of "value".
 *
 * Results:
 *      Returns fixed-point number representation of "value".
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE FixedNum 
IntToFixedNum(uint32 value)
{
   return(((FixedNum) value) << FIXEDNUM_1_LG);
}

/*
 *-----------------------------------------------------------------------------
 *
 * FixedNumToDecimal --
 *
 *      Sets "value" to decimal representation of "fixed",
 *	with 3 digits of decimal precision.
 *
 * Results:
 *      Sets "value" to decimal representation of "fixed",
 *	with 3 digits of decimal precision.
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE void
FixedNumToDecimal(FixedNum fixed, DecimalNum *value)
{
   uint32 tmp;
   value->whole = (uint32) (fixed >> FIXEDNUM_1_LG);
   tmp = (uint32) (fixed - IntToFixedNum(value->whole));
   value->milli = (tmp * 1000) >> FIXEDNUM_1_LG;
}

/*
 *-----------------------------------------------------------------------------
 *
 * FixedAveragesToDecimal --
 *
 *      Sets "d" to the decimal representation of "f".  Each
 *	component of "d" is converted from fixed-point binary
 *	to decimal representation with 3 digits decimal precision.
 *
 * Results:
 *      Sets "d" to the decimal representation of "f".
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE void
FixedAveragesToDecimal(const FixedAverages *f, DecimalAverages *d)
{
   FixedNumToDecimal(f->value, &d->value);
   FixedNumToDecimal(f->avg1,  &d->avg1);
   FixedNumToDecimal(f->avg5,  &d->avg5);
   FixedNumToDecimal(f->avg15, &d->avg15);
}

/*
 *-----------------------------------------------------------------------------
 *
 * FixedAverageUpdate --
 *
 *      Returns updated exponentially-weighted moving average (EMWA)
 *	computed from previous average "oldAvg" and new value "sample",
 *	using specified "weight" for previous average.
 *
 * Results:
 *      Returns updated EMWA computed from specified parameters.
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE FixedNum
FixedAverageUpdate(FixedNum oldAvg, uint32 weight, FixedNum sample)
{
   FixedNum newAvg;
   newAvg = (oldAvg * weight) + (sample * (FIXEDNUM_1 - weight));
   return(newAvg >> FIXEDNUM_1_LG);
}

/*
 *-----------------------------------------------------------------------------
 *
 * FixedAveragesUpdate --
 *
 *      Updates fixed-point moving averages associated with "f"
 *	to incorporate new sample with specified "value", using
 *	exponential decay weights specified by "d".
 *
 * Results:
 *	Modifies moving averages associated with "f".
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE void
FixedAveragesUpdate(FixedAverages *f,
                    const FixedAverageDecays *d,
                    uint32 value)
{
   f->value = IntToFixedNum(value);
   f->avg1  = FixedAverageUpdate(f->avg1,  d->exp1,  f->value);
   f->avg5  = FixedAverageUpdate(f->avg5,  d->exp5,  f->value);
   f->avg15 = FixedAverageUpdate(f->avg15, d->exp15, f->value);
}

#endif
