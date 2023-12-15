/* **********************************************************
 * Copyright 2002 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * mce.c --
 *
 *	This module manages machine check exceptions.
 */

/*
 * Includes
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "x86.h"
#include "vmkernel.h"
#include "sched.h"
#include "mce.h"

#define LOGLEVEL_MODULE MCE
#define LOGLEVEL_MODULE_LEN 3
#include "log.h"

Bool MCEEnabled = FALSE;


/*
 * MCE operations
 */

/*
 *----------------------------------------------------------------------
 *
 * MCE_Init --
 *
 *      Initialize MCEs for the current processor.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
void
MCE_Init(void)
{
   uint32 version, features;
   uint32 temp, mc_cap, mc_status;
   int numbanks, bank;
   uint32 cr4_reg;


   /*
    * Check that the current processor supports the machine check architecture.
    */
   ASM("cpuid" :
       "=a" (version),
       "=d" (features) :
       "a" (1) :
       "ebx", "ecx");

   if ((features & (CPUID_FEATURE_COMMON_ID1EDX_MCK | 
                    CPUID_FEATURE_COMMON_ID1EDX_MCA)) !=
                   (CPUID_FEATURE_COMMON_ID1EDX_MCK | 
                    CPUID_FEATURE_COMMON_ID1EDX_MCA)) {
      Warning("Can't do MCE on processors without MCA support.");
      /*
       * Since all processors must be identical, MCE should not have been
       * enabled already.
       */
      ASSERT(!MCEEnabled);
      return;
   } else {
      /* Since all processors must be identical, MCE should have been enabled
       * already or this is the BSP.
       */
      ASSERT(MCEEnabled || (MY_PCPU == 0));
   }

   LOG(0, "** Setting up MCEs on pcpu %d **", MY_PCPU);

   RDMSR(MSR_MCG_CAP, mc_cap, temp);
   RDMSR(MSR_MCG_STATUS, mc_status, temp);

   /*
    * If MSR_MCG_CTL exists, enable all machine check features.
    */
   if (mc_cap & MCG_CTL_P) {
      WRMSR(MSR_MCG_CTL, -1, -1);
   }

   /*
    * Initialize all the error-reporting banks.
    * [Note that MSR_MC0_CTL can be modified by software only on P IV]
    */
   numbanks = mc_cap & MCG_CNT;
   if (numbanks) {
       if (cpuType == CPUTYPE_INTEL_PENTIUM4) {
           WRMSR(MSR_MC0_CTL, -1, -1);
       }
       WRMSR(MSR_MC0_STATUS, 0, 0);
       for (bank = 1; bank < numbanks; bank++) {
           WRMSR(MSR_MC0_CTL + 4*bank, -1, -1);
           WRMSR(MSR_MC0_STATUS + 4*bank, 0, 0);
       }
   }

   /*
    * Enable MCE on this processor.
    * Since all the processors are assumed to be identical, the
    * setting of MCEEnabled is valid for all of them.
    */
   GET_CR4(cr4_reg);
   cr4_reg |= CR4_MCE;
   SET_CR4(cr4_reg);
   MCEEnabled = TRUE;

   return;
}


/*
 *----------------------------------------------------------------------
 *
 * MCE_Handle_Exception --
 *
 *      Handle machine check exception on the current processor.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
MCE_Handle_Exception(void)
{
   uint32 mcg_status, mcg_statush, mcg_cap;
   int recoverable;
   int numbanks, bank;
   uint32 status;
   uint32 l, h;


   asm("cld");

   // want to do this before any asserts
   SysAlert("Machine Check Exception");

   /*
    * Unless MCE has been enabled, we should never reach here.
    */
   ASSERT(MCEEnabled);

   /*
    * Check the status of the machine check error.
    */
   RDMSR(MSR_MCG_STATUS, mcg_status, mcg_statush);
   recoverable = mcg_status & MCG_RIPV;
   SysAlert("Machine Check Exception: General Status %08x%08x",
               mcg_statush, mcg_status);

   /*
    * Examine all the error-reporting banks to determine whether the
    * error is truly recoverable.
    */
   RDMSR(MSR_MCG_CAP, mcg_cap, h);
   numbanks = mcg_cap & MCG_CNT;
   for (bank = 0; bank < numbanks; bank++) {
      RDMSR(MSR_MC0_STATUS + 4*bank, l, status);
      SysAlert("Machine Check Exception: Bank %d, Status %08x%08x",
                  bank, status, l);
      if (status & MC0_VAL) {
         // This bank contains valid information
         if (status & (MC0_UC|MC0_PCC)) {
            // Error was left uncorrected or processor context is corrupted
            recoverable = 0;
         }
         if (status & MC0_MISCV) {
            // MISC register contains valid information
            RDMSR(MSR_MC0_MISC + 4*bank, l, h);
            SysAlert("Machine Check Exception: Bank %d, Misc %08x%08x",
                        bank, h, l);
         }
         if (status & MC0_ADDRV) {
            // ADDR register contains valid information
            RDMSR(MSR_MC0_ADDR + 4*bank, l, h);
            SysAlert("Machine Check Exception: Bank %d, Addr %08x%08x",
                        bank, h, l);
         }
         // Reset this bank
         WRMSR(MSR_MC0_STATUS + 4*bank, 0, 0);
      }
   }

   /*
    * If the error was recoverable, reset by clearing the
    * 'machine check in progress' flag and continue.
    */
   if (recoverable) {
      SysAlert("Machine Check Exception: Attempting to continue...");
      mcg_status &= ~MCG_MCIP;
      WRMSR(MSR_MCG_STATUS, mcg_status, mcg_statush);
   } else {
      Panic("Machine Check Exception: Unable to continue\n");
   }

   return;
}


void
MCE_Exception(uint32 cs, uint32 eip, uint32 esp, uint32 ebp)
{
#ifdef VMX86_DEBUG
   uint32 eflagsBefore;
   uint32 eflagsAfter;
   SAVE_FLAGS(eflagsBefore);
#endif
    MCE_Handle_Exception();
#ifdef VMX86_DEBUG
   SAVE_FLAGS(eflagsAfter);
   ASSERT((eflagsBefore & EFLAGS_IF) == (eflagsAfter & EFLAGS_IF));
#endif
}
