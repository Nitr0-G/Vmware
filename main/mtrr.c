/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * mtrr.c --
 *
 *	This file manages the processor's MTRRs (Memory Type Range Registers)
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "x86.h"
#include "vmkernel.h"
#include "host_dist.h"

#include "mtrr.h"

#define LOGLEVEL_MODULE MTRR
#define LOGLEVEL_MODULE_LEN 4
#include "log.h"

#define MAX_VARIABLE_MTRRS 8
#define MAX_FIXED_MA       0x100000

typedef unsigned char MTRR_Type;

typedef struct {
   Bool valid;
   MTRR_Type type;
   MA startAddr;
   MA endAddr;
} MTRRVariable;

typedef struct {
   MTRR_Type defaultType;
   MTRRVariable variable[MAX_VARIABLE_MTRRS];
   Bool fixedEnabled;
   MTRR_Type fixed[MAX_FIXED_MA/PAGE_SIZE];
} MTRR;

// CPU0's MTRR
static MTRR mtrrs[MAX_PCPUS];

static VMK_ReturnStatus MTRRRead(PCPU pcpu, MTRR *mtrr);

/*
 *----------------------------------------------------------------------
 *
 * MTRR_Init --
 *
 *      Initialize MTRR module.
 *
 * Results:
 *      VMK_UNSUPPORTED_CPU if MTRRs are not as expected
 *      VMK_OK otherwise
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
MTRR_Init(PCPU pcpu)
{
   // Read the hardware MTRRs and write into perCPU MTRR struct
   return MTRRRead(pcpu, &mtrrs[pcpu]);
}

/*
 *----------------------------------------------------------------------
 *
 * MTRRGetTypeMPN
 *
 *      Get the memory type for the given MPN
 *
 * Results:
 *      MTRR_Type of MPN
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static MTRR_Type
MTRRGetTypeMPN(MPN mpn)
{
   MA ma = MPN_2_MA(mpn);
   int i;
   Bool foundWB = FALSE;
   MTRR *mtrr = &mtrrs[0];

   // First check fixed MTRRs
   if (mtrr->fixedEnabled && (ma < MAX_FIXED_MA)) {
      return mtrr->fixed[mpn];
   }

   // Now check variable MTRRs (uncached wins if overlapped regions)
   for (i = 0; i < MAX_VARIABLE_MTRRS; i++) {
      if (mtrr->variable[i].valid) {
         if ((ma >= mtrr->variable[i].startAddr) &&
             (ma < mtrr->variable[i].endAddr)) {
            // non-WB types can't be overlapped
            if (mtrr->variable[i].type != MTRR_TYPE_WB) {
               return mtrr->variable[i].type;
            }
            foundWB = TRUE;
         }
      }
   }
   if (foundWB) {
      return MTRR_TYPE_WB;
   }

   // No variable region either, so use default type
   return mtrr->defaultType;
}

/*
 *----------------------------------------------------------------------
 *
 * MTRR_IsWBCachedMPN
 *
 *      Check wheter the given MPN is writeback cached according to MTRRs
 *
 * Results:
 *      TRUE if writeback cached, FALSE otherwise
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Bool
MTRR_IsWBCachedMPN(MPN mpn)
{
   return (MTRRGetTypeMPN(mpn) == MTRR_TYPE_WB);
}

/*
 *----------------------------------------------------------------------
 *
 * MTRR_IsUncachedMPN
 *
 *      Check wheter the given MPN is uncached according to MTRRs
 *
 * Results:
 *      TRUE if uncached, FALSE otherwise
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Bool
MTRR_IsUncachedMPN(MPN mpn)
{
   return (MTRRGetTypeMPN(mpn) == MTRR_TYPE_UC);
}

/*
 *----------------------------------------------------------------------
 *
 * Mask2Size --
 *
 *      Convert a variable MTRR mask to the size of the region it maps
 *
 * Results:
 *      Size of MTRR region
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE uint64
Mask2Size(uint64 mask)
{
   uint64 size = 0xfffffff000000000LL;

   size |= mask;
   size = ~size;
   size++;

   return size;
}

/*
 *----------------------------------------------------------------------
 *
 * MTRRAddVariable
 *
 *      Add the given variable MTRR region to the MTRR struct
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
MTRRAddVariable(PCPU pcpu, MTRR *mtrr, uint64 base, uint64 mask)
{
   int i;
   Bool different = FALSE;

   if (!(mask & MTRR_MASK_VALID)) {
      return;
   }

   for (i = 0; i < MAX_VARIABLE_MTRRS; i++) {
      if (!mtrr->variable[i].valid) {
         mtrr->variable[i].valid = TRUE;
         mtrr->variable[i].type = base & MTRR_BASE_TYPE_MASK;
         mtrr->variable[i].startAddr = base & MTRR_BASE_ADDR_MASK;
         mtrr->variable[i].endAddr = mtrr->variable[i].startAddr + 
            Mask2Size(mask & MTRR_MASK_ADDR_MASK);
         if ((pcpu != HOST_PCPU) &&
             ((mtrr->variable[i].startAddr != mtrrs[HOST_PCPU].variable[i].startAddr) ||
              (mtrr->variable[i].endAddr != mtrrs[HOST_PCPU].variable[i].endAddr) ||
              (mtrr->variable[i].type != mtrrs[HOST_PCPU].variable[i].type))) {
            SysAlert("MTRRs different between CPU 0 and %d", pcpu);
            different = TRUE;
         }
         if ((pcpu == HOST_PCPU) || different) {
            Log("MTRR %d: start=0x%Lx end=0x%Lx type=%x", 
                i, mtrr->variable[i].startAddr, mtrr->variable[i].endAddr,
                mtrr->variable[i].type);
         }
         break;
      }
   }
   ASSERT_NOT_IMPLEMENTED(i != MAX_VARIABLE_MTRRS);
}

/*
 *----------------------------------------------------------------------
 *
 * MTRRAddFixed
 *
 *      Add the given fixed MTRR region to the MTRR struct
 *      Also, on non-HOST_PCPU processors make sure the MTRRs are the
 *      same a HOST_PCPU's value.  This is safe to do withouth flushing
 *      TLB and caches because vmkernel doesn't touch 0-1MB machine
 *      addresses except maybe for bluescreen code.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
MTRRAddFixed(PCPU pcpu, MTRR *mtrr, int msrNum, 
             uint32 sizeKB, MA startMA)
{
   int i;
   int numPages = sizeKB*1024/PAGE_SIZE;
   uint64 mtrrTypes = __GET_MSR(msrNum);

   if (pcpu == HOST_PCPU) {
      Log("start=0x%Lx type=0x%Lx", startMA, mtrrTypes);
   }
   for (i = 0; i < 8; i++) {
      int page;
      for (page = 0; page < numPages; page++) {
         MPN mpn = MA_2_MPN(startMA) + i*numPages + page;
         MTRR_Type type = (mtrrTypes >> i*8) & MTRR_BASE_TYPE_MASK;

         if ((pcpu != HOST_PCPU) &&
             (type != mtrrs[HOST_PCPU].fixed[mpn])) {
            Log("MTRR (mpn 0x%x) different between CPU 0 (%d) and %d (%d)",
                mpn, mtrrs[HOST_PCPU].fixed[mpn], pcpu, type);

            // overwrite the fixed MTRR to match HOST_PCPU
            type = mtrrs[HOST_PCPU].fixed[mpn];
            mtrrTypes &= ~(((uint64)0xff) << i*8);
            mtrrTypes |= ((uint64)type) << i*8;
            __SET_MSR(msrNum, mtrrTypes);
         }

         mtrr->fixed[mpn] = type;
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * MTRRRead
 *
 *      Read this CPU's hardware MTRRs and store it in the given struct
 *
 * Results:
 *      VMK_ReturnStatus
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
MTRRRead(PCPU pcpu, MTRR *mtrr)
{
   uint64 base, mask;

   base = __GET_MSR(MSR_MTRR_CAP);
   Log("MTRR: cap=0x%Lx", base);
   if ((base & MTRR_CAP_VCNT_MASK) != MAX_VARIABLE_MTRRS) {
      SysAlert("Unsupported number of MTRRS %Ld", base & MTRR_CAP_VCNT_MASK);
#if defined(VMX86_DEBUG) && defined(VMX86_DEVEL)
      // vcpus do not have MTRRs, so assume WB to allow vmkernel to run in a VM
      mtrr->defaultType = MTRR_TYPE_WB;
      return VMK_OK;
#else
      return VMK_UNSUPPORTED_CPU;
#endif
   }

   mask = __GET_MSR(MSR_MTRR_DEF_TYPE);
   if (pcpu == HOST_PCPU) {
      Log("MTRR: deftype=0x%Lx", mask);
   }
   if (!(mask & MTRR_DEF_ENABLE)) {
      mtrr->defaultType = MTRR_TYPE_UC;
      return VMK_OK;
   }
   mtrr->fixedEnabled = (mask & MTRR_DEF_FIXED_ENABLE) ? TRUE : FALSE;
   if (mtrr->fixedEnabled != mtrrs[HOST_PCPU].fixedEnabled) {
      SysAlert("MTRR (fixed enable) different CPU 0 (%d) and %d (%d)",
               mtrrs[HOST_PCPU].fixedEnabled, pcpu, mtrr->fixedEnabled);
   }

   mtrr->defaultType = mask & MTRR_DEF_TYPE_MASK;
   if (mtrr->defaultType != mtrrs[HOST_PCPU].defaultType) {
      SysAlert("MTRR (default) different CPU 0 (%d) and %d (%d)",
               mtrrs[HOST_PCPU].defaultType, pcpu, mtrr->defaultType);
   }

   base = __GET_MSR(MSR_MTRR_BASE0);
   mask = __GET_MSR(MSR_MTRR_MASK0);
   MTRRAddVariable(pcpu, mtrr, base, mask);

   base = __GET_MSR(MSR_MTRR_BASE1);
   mask = __GET_MSR(MSR_MTRR_MASK1);
   MTRRAddVariable(pcpu, mtrr, base, mask);

   base = __GET_MSR(MSR_MTRR_BASE2);
   mask = __GET_MSR(MSR_MTRR_MASK2);
   MTRRAddVariable(pcpu, mtrr, base, mask);

   base = __GET_MSR(MSR_MTRR_BASE3);
   mask = __GET_MSR(MSR_MTRR_MASK3);
   MTRRAddVariable(pcpu, mtrr, base, mask);

   base = __GET_MSR(MSR_MTRR_BASE4);
   mask = __GET_MSR(MSR_MTRR_MASK4);
   MTRRAddVariable(pcpu, mtrr, base, mask);

   base = __GET_MSR(MSR_MTRR_BASE5);
   mask = __GET_MSR(MSR_MTRR_MASK5);
   MTRRAddVariable(pcpu, mtrr, base, mask);

   base = __GET_MSR(MSR_MTRR_BASE6);
   mask = __GET_MSR(MSR_MTRR_MASK6);
   MTRRAddVariable(pcpu, mtrr, base, mask);

   base = __GET_MSR(MSR_MTRR_BASE7);
   mask = __GET_MSR(MSR_MTRR_MASK7);
   MTRRAddVariable(pcpu, mtrr, base, mask);

   if (!mtrr->fixedEnabled) {
      return VMK_OK;
   }

   MTRRAddFixed(pcpu, mtrr, MSR_MTRR_FIX64K_00000, 64, 0);
   MTRRAddFixed(pcpu, mtrr, MSR_MTRR_FIX16K_80000, 16, 0x80000);
   MTRRAddFixed(pcpu, mtrr, MSR_MTRR_FIX16K_A0000, 16, 0xA0000);
   MTRRAddFixed(pcpu, mtrr, MSR_MTRR_FIX4K_C0000, 4, 0xC0000);
   MTRRAddFixed(pcpu, mtrr, MSR_MTRR_FIX4K_C8000, 4, 0xC8000);
   MTRRAddFixed(pcpu, mtrr, MSR_MTRR_FIX4K_D0000, 4, 0xD0000);
   MTRRAddFixed(pcpu, mtrr, MSR_MTRR_FIX4K_D8000, 4, 0xD8000);
   MTRRAddFixed(pcpu, mtrr, MSR_MTRR_FIX4K_E0000, 4, 0xE0000);
   MTRRAddFixed(pcpu, mtrr, MSR_MTRR_FIX4K_E8000, 4, 0xE8000);
   MTRRAddFixed(pcpu, mtrr, MSR_MTRR_FIX4K_F0000, 4, 0xF0000);
   MTRRAddFixed(pcpu, mtrr, MSR_MTRR_FIX4K_F8000, 4, 0xF8000);

   return VMK_OK;
}
