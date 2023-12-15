/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * memmap_dist.h --
 *
 *	This is the header file for machine memory manager.
 */


#ifndef _MEMMAP_DIST_H
#define _MEMMAP_DIST_H

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_DISTRIBUTE
#define INCLUDE_ALLOW_VMK_MODULE
#include "includeCheck.h"

extern MPN MemMap_AllocDriverPage(Bool lowPage);
extern void MemMap_FreeDriverPage(MPN mpn);

#define FOUR_GB_MA             (1LL << 32)
#define FOUR_GB_MPN            MA_2_MPN(FOUR_GB_MA)

static INLINE Bool
IsLowMPN(MPN mpn)
{
   return (mpn < FOUR_GB_MPN);
}

#define IsHighMPN(x) (!IsLowMPN(x))

static INLINE Bool
IsLowMA(MA ma)
{
   return (ma < FOUR_GB_MA);
}

#define IsHighMA(x) (!IsLowMA(x))

#define MMIOPROT_IO_ENABLE  TRUE
#define MMIOPROT_IO_DISABLE FALSE
#ifdef VMX86_DEBUG
extern void MemMap_SetIOProtection(MPN mpn, Bool ioAble);
extern void MemMap_SetIOProtectionRange(MA maddr, uint64 len, Bool ioAble);
extern Bool MemMap_IsIOAble(MPN mpn);
extern Bool MemMap_IsIOAbleRange(MA maddr, uint64 len);
#else
static INLINE void MemMap_SetIOProtection(MPN mpn, Bool ioAble)
{
}
static INLINE void MemMap_SetIOProtectionRange(MA maddr, uint64 len, Bool ioAble)
{
}
static INLINE Bool MemMap_IsIOAble(MPN mpn)
{
   return TRUE;
}
static INLINE Bool MemMap_IsIOAbleRange(MA maddr, uint64 len)
{
   return TRUE;
}
#endif

#endif



