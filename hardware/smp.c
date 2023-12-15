/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * smp.c: multiprocessor host specific functions
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "vmkernel.h"
#include "util.h"
#include "kvmap.h"
#include "world.h"
#include "host.h"
#include "apic_int.h"
#include "prda.h"
#include "tlb.h"
#include "memalloc.h"
#include "kseg.h"
#include "nmi.h"
#include "mce.h"
#include "smp.h"
#include "timer.h"
#include "mtrr.h"
#include "cpuid_info.h"
#include "x86cpuid.h"
#include "vmkemit.h"
#include "idt.h"
#include "proc.h"
#include "timer.h"

#define LOGLEVEL_MODULE SMP
#include "log.h"

// start of AP statup code
#define VMK_STARTUP_EIP         (VMNIX_AP_STARTUP_PAGE * PAGE_SIZE)

// the code using these rely on byte reads/writes to be atomic
static volatile Bool apRunning[MAX_PCPUS];
static int apicIDs[MAX_PCPUS];
static int initialAPICIDs[MAX_PCPUS];
extern uint32 apicID_range;
extern CPUIDSummary cpuids[];

// setup for cpu timestamp counter (TSC)
static SP_Barrier tscBarrier;
static Bool tscReset;

// hyperthreading support
#define HT_APICID_THREADNUM_MASK      (1)
#define HT_INITIAL_APICID_BITS        (0xFF000000)
#define HT_INITIAL_APICID_SHIFT       (24)
#define HT_CPUID_BIT                  (0x10000000)
#define HT_NUM_LOGICAL_BITS           (0x00FF0000)
#define HT_APICID_PANIC_STRING "Invalid APIC ID from ACPI table -- " \
                               "You may be able to load with hyperthreading disabled.\n"

static Proc_Entry cpuInfoProc;
struct SMP_HTInfo hyperthreading;

static MA bootPageRoot = 0;
static MA SMPSetupSlaveBootPT(World_Handle *world);
extern void StartSlaveWorld(void);
static int SMPCpuInfoProcRead(Proc_Entry *entry, char *buf, int *len);

/*
 *----------------------------------------------------------------------
 *
 * SMPResetTSC --
 *
 *      Called from all PCPUs at vmkernel initialization.
 *      Synchronizes the TSCs by bringing all PCPUs to a barrier,
 *      then zeroing the TSCs.
 *
 * Results:
 *      Returns the old TSC value just before it was zeroed.
 *
 * Side effects:
 *      Zeroes the Time Stamp Counter register (TSC).
 *
 *----------------------------------------------------------------------
 */
TSCRelCycles
SMPResetTSC(PCPU pcpuNum)
{
   TSCCycles tsc = 0, oldtsc;
   LOG(1, "tscBarrier spin (tsc reset), pcpu=%u", pcpuNum);
   SP_SpinBarrierNoYield(&tscBarrier);
   oldtsc = RDTSC();
   __SET_MSR(MSR_TSC, tsc);
   tsc = RDTSC();
   if (pcpuNum == 0) {
      // inform Timer module that we reset the TSC.
      Timer_CorrectForTSCShift(oldtsc);
   }
   // Wait a while to reduce contention on the log lock
   Util_Udelay(10000 * pcpuNum);
   Log("cpu %d: TSC reset %Lu -> %Lu", pcpuNum, oldtsc, tsc);
   return oldtsc;
}


/*
 *----------------------------------------------------------------------
 *
 * SMPSaveCPUID --
 *
 *      Called from all PCPUs at vmkernel initialization.
 *      Saves the CPUID information for subsequent queries
 *      by vmx on behalf of the monitor.
 *
 * Side effects:
 *      Saves CPUID 0 and 1 information.
 *
 *----------------------------------------------------------------------
 */

static void
SMPSaveCPUID(void)
{
   uint32 regs[4];
   CPUIDSummary *cpuid = &cpuids[myPRDA.pcpuNum];

   __GET_CPUID (0, regs);

   /*
    * Get vendor and CPUID capabilities information.
    */

   cpuid->id0.numEntries              = regs[0];
   *(uint32 *) (cpuid->id0.name + 0)  = regs[1];
   *(uint32 *) (cpuid->id0.name + 4)  = regs[3];
   *(uint32 *) (cpuid->id0.name + 8)  = regs[2];
   *(uint32 *) (cpuid->id0.name + 12) = 0;

   /*
    * Get version and feature information.
    */

   if (cpuid->id0.numEntries >= 1) {
      __GET_CPUID (1, (uint32 *) &cpuid->id1);
   }

   /*
    * If supported, get extended leaf information.
    */

   __GET_CPUID(0x80000000, (uint32 *) &cpuid->id80);

   if (cpuid->id80.numEntries >= 0x80000001) {
      __GET_CPUID(0x80000001, (uint32 *) &cpuid->id81);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * SMPGetInitialAPICID --
 *
 *      Reads and returns the initial APIC ID for the current processor.
 *
 * Results:
 *      Returns the initial APIC ID for the current processor.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
static int
SMPGetInitialAPICID(void)
{
   int initialID;
   uint32 version, features, ebx;

   ASM("cpuid" :
       "=a" (version),
       "=d" (features),
       "=b" (ebx) :
       "a" (1) :
       "ecx");      
   initialID = (ebx & HT_INITIAL_APICID_BITS) >> HT_INITIAL_APICID_SHIFT;
   
   return initialID;
}


void SMP_SlaveHaltCheck(PCPU pcpuNum)
{
   if (myPRDA.stopAP) {
      NMI_Disable();
      apRunning[pcpuNum] = FALSE;
      // Warning("slave %d halting", pcpuNum);
      while (1) {
	 __asm__("cli");
	 __asm__("hlt");
      }
   }
}


/*
 *--------------------------------------------------------------------------
 *
 * SMPLogMicrocodeLevel --
 *
 *      Log the microcode level of the current PCPU
 *
 * Results:
 *      None
 *
 * Side Effects:
 *      None
 *
 *--------------------------------------------------------------------------
 */
static void
SMPLogMicrocodeLevel(void)
{
   uint32 regs[8];
   uint64 signature, platform;

   if (cpuType != CPUTYPE_INTEL_PENTIUM4) {
      Log("No information available");
      return;
   }

   // CPUID 1 will put it in the appropriate MSR.
   __SET_MSR(MSR_BIOS_SIGN_ID, 0); // as recommended by Intel
   __GET_CPUID(1, regs);
   signature = __GET_MSR(MSR_BIOS_SIGN_ID);
   platform = __GET_MSR(MSR_PLATFORM_ID);
   Log("Update signature %"FMT64"x, Platform ID %"FMT64"x",
       signature, platform);
}


void
SMPSlaveIdle(World_Handle *previous)
{
   uint32 cr0reg;
   PCPU pcpuNum;
   int i;

   /* No interrupts allowed until after APIC is enabled */
   ASSERT_NO_INTERRUPTS();

   SET_CR0(hostCR0);
   GET_CR0(cr0reg);
   ASSERT(cr0reg & CR0_WP);
   SET_CR4(hostCR4);

   APIC_SlaveInit();

   pcpuNum = APIC_GetPCPU();
   LOG(0, "slave on pcpu %u", pcpuNum);
   myPRDA.pcpuNum   = pcpuNum;
   myPRDA.pcpuState = PCPU_AP;
   myPRDA.runningWorld = World_GetIdleWorld(pcpuNum);
   myPRDA.currentTicks = 1;
   myPRDA.perfCounterInts = 0;
   myPRDA.previousTicks = 0;
   myPRDA.hungCount = 0;
   myPRDA.stopAP = FALSE;

   apRunning[pcpuNum] = TRUE;
   debugRegs[0]++;


   // obtain and store our initial APIC ID for sanity checking later
   initialAPICIDs[pcpuNum] = SMPGetInitialAPICID();
   LOG(0, "pcpu %u initial APICID=0x%x", pcpuNum, initialAPICIDs[pcpuNum]);

   SMPLogMicrocodeLevel();

   if (SMP_HTEnabled()) {
      // confirm that the cpu and ACPI table agree about our thread num
      if ((initialAPICIDs[pcpuNum] & HT_APICID_THREADNUM_MASK)
          != SMP_GetHTThreadNum(pcpuNum)) {
         Panic(HT_APICID_PANIC_STRING);
      }
   }

   /*
    * PR#24271: preemption must be disabled or spin lock requests will panic
    * due to the fact that we are in a slave world.
    * See sp_lock.c:SPSetCurrentLock(), ASSERT(SCHED_CURRENT_WORLD->inVMKernel)
    */
   CpuSched_DisablePreemption();

   // reset TSC counter, if requested
   if (tscReset) {
      SMPResetTSC(pcpuNum);
   }

   /*
    * Wait a while so that all PCPUs are not pounding on the bus at the
    * same time.  This seems to be needed to fix PR 34866.
    */
   for (i = 0; i < pcpuNum - 1; i++) {
      Timer_GetCycles(); // must be called at least every 5.368 seconds
      Util_Udelay(4000000);
   }
   APIC_HzEstimate(&myPRDA.cpuHzEstimate, &myPRDA.busHzEstimate);
   Log("cpu %d: measured cpu speed is %Ld Hz", pcpuNum, myPRDA.cpuHzEstimate);
   Log("cpu %d: measured bus speed is %Ld Hz", pcpuNum, myPRDA.busHzEstimate);

   SMPSaveCPUID();

   // wait here until SMP_StartAPs is called
   SP_SpinBarrierNoYield(&tscBarrier); 

   if (SMP_HTEnabled()) {
      int partnerID;
      PCPU partner = SMP_GetPartnerPCPU(pcpuNum);

      // make sure that our initial APICID, as reported by cpuid
      // is really the same as our partner's, disregarding the last bit
      // this is a sanity check to make sure we didn't setup
      // the HT mappings based on a horribly-weird ACPI table
      partnerID = initialAPICIDs[partner];
      if ((partnerID & ~HT_APICID_THREADNUM_MASK)
          != (initialAPICIDs[pcpuNum] & ~HT_APICID_THREADNUM_MASK)) {
         Panic(HT_APICID_PANIC_STRING);
      }
   }

   NUMA_LocalInit(pcpuNum);

   MTRR_Init(pcpuNum);

   Sched_AddRunning();

   ENABLE_INTERRUPTS();
   Watchpoint_Enable(FALSE);
   MCE_Init();

   TLB_LocalInit();

   CpuSched_EnablePreemption();
   CpuSched_IdleLoop();
   NOT_REACHED();
   /* never returns */
}


/*
 *----------------------------------------------------------------------
 *
 * SMP_StartAPs --
 *
 *      Release APs from the barrier in SMPSlaveIdle and let them
 *      start doing real work.
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
SMP_StartAPs(void)
{
   SP_SpinBarrierNoYield(&tscBarrier);
}

/*
 *----------------------------------------------------------------------
 *
 * MPFChecksum --
 *
 *      Checksum the Intel MPF Floating Pointer Structure
 *
 * Results: 
 *      zero if checksum ok, non-zero otherwise
 *      
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
MPFChecksum(IntelMPFloating *mpf)
{
   int i;
   uint8 *ptr = (uint8 *)mpf;
   int sum = 0;

   ASSERT(mpf->length == 1);
   ASSERT(sizeof(IntelMPFloating) == 16);
   for (i = 0; i < 16; i++) {
      sum += *ptr++;
   }
   return sum & 0xFF;
}


/*
 *-----------------------------------------------------------------------------
 *
 * SMPIsHTSupported --
 *
 *    Does the processor support hyperthreading?
 *
 * Results:
 *    TRUE on success, FALSE otherwise
 *
 * Side effects:
 *    none.
 *
 *-----------------------------------------------------------------------------
 */
static Bool
SMPIsHTSupported(void)
{
   uint32 features = 0, version, ebx;
   // read the CPU features
   ASM("cpuid" :
       "=a" (version),
       "=d" (features),
       "=b" (ebx) :
       "a" (1) :
       "ecx");

   return (features & HT_CPUID_BIT) != 0;

}


/*
 *-----------------------------------------------------------------------------
 *
 * SMPInitHyperthreading --
 *
 *     Initializes data structures associated with hyperthreading, will
 *     not initialize more than "pcpuLimit" logical processors.
 *
 * Results:
 *     None.
 *
 * Side effects:
 *     Modifies hyperthreading data.
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
SMPInitHyperthreading(uint32 pcpuLimt)
{
   PCPU c, i;
   uint32 curNum, features = 0, version, ebx, maxNumPcpus;
   int oldApicIds[MAX_PCPUS];
   Bool htPresent;

   // read the CPU features
   ASM("cpuid" :
       "=a" (version),
       "=d" (features),
       "=b" (ebx) :
       "a" (1) :
       "ecx");

   htPresent = (features & HT_CPUID_BIT) != 0;
   
   if (!htPresent) {
      Warning("processor does not support hyperthreading");
      return (VMK_NOT_SUPPORTED);
   }

   maxNumPcpus = MIN(pcpuLimt, numPCPUs);
   
   // bits [16:23] of ebx indicate the number of logical cpus per package
   hyperthreading.logicalPerPackage = (ebx & HT_NUM_LOGICAL_BITS) >> 16;
   LOG(0, "logicalPerPackage = %u", hyperthreading.logicalPerPackage);
   
   // Intel has implied that they won't increase the number of
   // logical cpus per pkg for quite a while, so we don't worry about it
   if (hyperthreading.logicalPerPackage != 2 || numPCPUs % 2 != 0) {
      SysAlert("Unable to start hyperthreading, perhaps it is disabled in the BIOS?");
      return (VMK_NOT_SUPPORTED);
   }
   ASSERT(numPCPUs % 2 == 0);

   // sanitize order of apicids, so that
   // partner-lcpus are adjacent in the pcpu numbering
   // e.g. pcpu 0 is on the same package as pcpu 1
   memcpy(oldApicIds, apicIDs, sizeof(apicIDs));
   memset(apicIDs, 0, sizeof(apicIDs));
   curNum = 0;
   for (c=0; c < numPCPUs; c++) {
      // only look for "primary logical processors" (even apicid)
      if ((oldApicIds[c] & HT_APICID_THREADNUM_MASK) != 0) {
         continue;
      }
      apicIDs[curNum++] = oldApicIds[c];
      for (i=0; i < numPCPUs; i++) {
         if (i != c &&
             (oldApicIds[i] & ~HT_APICID_THREADNUM_MASK) == (oldApicIds[c] & ~HT_APICID_THREADNUM_MASK)) {
            // we learned that "i" is the partner of "c", so put them together
            apicIDs[curNum++] = oldApicIds[i];
            break;
         }
      }

      // make sure we found a partner for this cpu
      if (i == numPCPUs) {
         Log("no hyperthread partner found for pcpu %u, apicID %x", c, oldApicIds[c]);
         SysAlert("BIOS reporting invalid hyperthreading configuration, "
                  "hyperthreading will not be enabled");
         return (VMK_NOT_SUPPORTED);
      }
      if (curNum == maxNumPcpus) {
         break;
      }
   }

   // hyperthreading initialization will succeed
   hyperthreading.htEnabled = TRUE;
   Log("hyperthreading enabled");
   
   // better not move the host PCPU:
   ASSERT(oldApicIds[HOST_PCPU] == apicIDs[HOST_PCPU]);

   // setup HT data structures
   for (c=0; c < maxNumPcpus; c++) {
      SMP_PackageInfo *curPkg;
      int pkg;
      int apicID = apicIDs[c];

      for (pkg=0; pkg < hyperthreading.numPackages; pkg++) {
         // see if we match a known apicID, except for the least sig. bit
         if ((apicID & ~HT_APICID_THREADNUM_MASK) == hyperthreading.packages[pkg].baseApicID) {
            break;
         }
      }

      if (pkg == hyperthreading.numPackages) {
         hyperthreading.numPackages++;
      }

      // setup list/map of packages
      hyperthreading.cpuToPkgMap[c] = pkg;
      curPkg = &hyperthreading.packages[pkg];
      curPkg->baseApicID = (apicID & ~HT_APICID_THREADNUM_MASK);
      curPkg->logicalCpus[curPkg->numLogical] = c;
      curPkg->numLogical++;
         
      LOG(0, "pcpu %d lies in package %d", c, pkg);
   }

   Log("num HT packages = %d", hyperthreading.numPackages);
   return (VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * SMPParseCPUTables --
 *
 *     Reads the MPS table and possibly the ACPI table to determine the number
 *     of PCPUs and their apicIDs. The ACPI table will only be parsed if
 *     "hypertwinsEnabled" is TRUE, because it contains information about the
 *     secondary logical processor on a hyperthreaded system, while the MPS
 *     table does not.
 *
 * Results:
 *    VMK_OK on success, error code on failure
 *
 * Side effects:
 *     Initializes numPcpus and global apicIDs
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
SMPParseCPUTables(VMnix_Init *vmnixInit,
                  VMnix_ConfigOptions *vmnixOptions,
                  VMnix_AcpiInfo *acpiInfo, 
                  Bool hypertwinsEnabled)
{
   VMnix_SavedMPS *mps = &vmnixInit->savedMPS;
   MPConfigTable *mpc;
   MPProcessorEntry *mpp;
   uint8 *ptr;
   int i;
   int mps_numPCPUs;

   tscReset = vmnixOptions->resetTSC;

   for (i = 0; i < MAX_PCPUS; i++) {
      apicIDs[i] = -1;
   }

   if (vmnixInit->biosDataSource == VMNIX_TRY_ACPI ||
       vmnixInit->biosDataSource == VMNIX_STRICT_ACPI) {
      // use ACPI
      Bool htSupported = SMPIsHTSupported();
      int bspApicID;
      Bool bspApicIDFound = FALSE;
      VMK_ReturnStatus status;

      // Acpi tables dont have a special flag to denote the apicID 
      // for the BSP, so we find its apicID directly from the APIC.
      status = APIC_GetCurPCPUApicID(&bspApicID);
      if (status != VMK_OK) {
         Warning("Failed to get the apic id for bsp");
         return status;
      }

      ASSERT(acpiInfo);
      ASSERT(acpiInfo->apicInfoValid); 
      numPCPUs = 1; // we have atleast the bsp

      for (i = 0; i < acpiInfo->numAPICs; i++) {
         // If hyperthreading is *not* supported by the processor 
         // then use all the enumerated 
         // apic IDs. If it *is* supported then use all the enumerated apic IDs
         // only if hypertwins are enabled
         if (!htSupported || hypertwinsEnabled) {
            if (!acpiInfo->apics[i].enabled) {
               Log("this pcpu 0x%x is disabled", acpiInfo->apics[i].id);
               continue;
            }
         } else {
            // only look for "primary logical processors" (even apicid)
            if ((acpiInfo->apics[i].id & HT_APICID_THREADNUM_MASK) != 0) {
               continue;
            }
            if (!acpiInfo->apics[i].enabled) {
               Log("this pcpu 0x%x is disabled", acpiInfo->apics[i].id);
               continue;
            }
         }
         if (numPCPUs == MAX_PCPUS) {
            Log("max. # of supported pcpus reached already");
            break;
         }

         // Special handling of the bsp apic ID. The vmkernel kindof relies on
         // the bsp getting the first slot in apicIDs i.e. bsp pcpuNum = 0.
         if (acpiInfo->apics[i].id == bspApicID) {
            Log("APICid 0x%02x ->pcpu %d, bsp", 
                acpiInfo->apics[i].id, HOST_PCPU);
            ASSERT(HOST_PCPU == 0);
            apicIDs[HOST_PCPU] = bspApicID;
            bspApicIDFound = TRUE;
         } else {
            Log("APICid 0x%02x ->pcpu %d", 
                acpiInfo->apics[i].id, numPCPUs);
            apicIDs[numPCPUs] = acpiInfo->apics[i].id;
            numPCPUs++;
         }
      }
      ASSERT_NOT_IMPLEMENTED(bspApicIDFound);
   } else {
      // use MPS

      if (!mps->present) {
         Log("NO MPS table found");
         /*
          *  no MPS table, must be UP
          */
         numPCPUs = 1;

      } else if (vmnixOptions->checksumMPS && MPFChecksum(&mps->mpf)) {
         /*
          *  bad checksum, assume UP
          */
         Log("Bad MPF checksum");
         mps->present = FALSE;
         numPCPUs = 1;
         
      } else if (mps->mpf.feature1) {

         /*
          *  use default configuration
          */

         switch (mps->mpf.feature1) {
         case 1:
         case 2:
         case 3:
         case 4:
         case 5:
         case 6:
         case 7:
            LOG(0, "default config %d.", mps->mpf.feature1);
            numPCPUs = 2;
            break;
         default:
            NOT_IMPLEMENTED();
            break;
         }
         
      } else {

         /*
          *  scan the MPC table for configuration
          */

         mpc = (MPConfigTable*)&mps->mpc;
         numPCPUs = 0;
         
         for (ptr=(uint8*)(mpc+1), i=0; i<mpc->count; i++) {
            switch (*ptr) {
            case PROC_ENTRY:
               mpp = (MPProcessorEntry*)ptr;
               Log("proc lapicid=0x%x lapicver=0x%x flags=0x%x "
                   "sig=0x%x feature=0x%x",
                   mpp->lapicid, mpp->lapicver, mpp->flags,
                   mpp->sig, mpp->feature);
               if (mpp->flags & MPS_PROC_ENABLED) {
                  if (numPCPUs == MAX_PCPUS) {
                     Log("max. # of supported pcpus reached already");
                  } else {
                     Log("APICid 0x%02x -> pcpu %d", mpp->lapicid, numPCPUs);
                     apicIDs[numPCPUs] = mpp->lapicid;
                     if (mpp->flags & MPS_PROC_BSP) {
                        ASSERT(numPCPUs == HOST_PCPU);
                     } else {
                        ASSERT(numPCPUs != HOST_PCPU);
                     }
                     numPCPUs++;
                  }
               } else {
                  Log("this pcpu is disabled");
               }
               ptr += 20;
               break;
            case BUS_ENTRY:
               // LOG(0, "MPS: bus");
               ptr += 8;
               break;
            case IOAPIC_ENTRY:
               // LOG(0, "MPS: ioapic");
               ptr += 8;
               break;
            case IOINT_ENTRY:
               // LOG(0, "MPS: ioint");
               ptr += 8;
               break;
            case LOCALINT_ENTRY:
               // LOG(0, "MPS: local");
               ptr += 8;
               break;
            default:
               Log("bad entry 0x%x", *ptr);
               NOT_IMPLEMENTED();
               break;
            }
         }

         /*
          * if hypertwins are enabled, scan the ACPI information
          */

         if (hypertwinsEnabled && acpiInfo && acpiInfo->apicInfoValid) {
            uint32 j;
            mps_numPCPUs = numPCPUs;
            VMnix_AcpiAPIC *acpiAPICInfo = acpiInfo->apics;
            for (j = 0; j < acpiInfo->numAPICs; j++) {
               if (!acpiAPICInfo[j].enabled) {
                  Log("this pcpu is disabled");
                  continue;
               }
               for (i = 0; i < mps_numPCPUs; i++) {
                  if (acpiAPICInfo[j].id == apicIDs[i]) {
                     Log("already added by MPS as pcpu %d", i);
                     break;
                  }
               }
               if (i == mps_numPCPUs) {
                  if (numPCPUs == MAX_PCPUS) {
                     Log("max. # of supported pcpus reached already");
                  } else {
                     Log("APICid 0x%02x -> pcpu %d",
                         acpiAPICInfo[j].id, numPCPUs);
                     apicIDs[numPCPUs] = acpiAPICInfo[j].id;
                     numPCPUs++;
                  }
               }
            }
         }
      }
   }

   if (numPCPUs == 0) {
      /*
       *  no CPU entries, assume UP
       */
      Log("No CPUs in MPS");
      mps->present = FALSE;
      numPCPUs = 1;
   } else {
      Log("numPCPUs = %d", numPCPUs);
   }
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * SMP_Init --
 *
 *      Setup core SMP info: apicIDs, numPCPUs, and hyperthreading data
 *
 * Results: 
 *    VMK_OK on success, error code of failure.
 *      
 * Side effects:
 *      Initializes SMP global data
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
SMP_Init(VMnix_Init *vmnixInit, 
         VMnix_ConfigOptions *vmnixOptions, 
         VMnix_AcpiInfo *acpiInfo)
{
   Bool htEnabled;
   int i;
   VMK_ReturnStatus status;
   
   tscReset = vmnixOptions->resetTSC;
   htEnabled = vmnixOptions->hyperthreading;

   // obtain and store the initial APIC ID for the BSP
   initialAPICIDs[HOST_PCPU] = SMPGetInitialAPICID();
   LOG(0, "BSP initial APICID=0x%x", initialAPICIDs[HOST_PCPU]);

   SMPLogMicrocodeLevel();

   status = SMPParseCPUTables(vmnixInit, vmnixOptions, acpiInfo, htEnabled);
   if (status != VMK_OK) {
      return status;
   }
   
   if (htEnabled) {
      uint32 cpuLimit = numPCPUs;
      VMK_ReturnStatus status;
      
      if (vmnixOptions->maxPCPUs) {
         // vmnixOptions maxPCPUs is specified in physical packages,
         // not logical processors, as are all licensing-related values
         cpuLimit = MIN(numPCPUs, vmnixOptions->maxPCPUs * 2);
      }
      
      status = SMPInitHyperthreading(cpuLimit);
      if (status == VMK_OK) {
         // cap numPCPUs at our cpuLimit
         if (numPCPUs > cpuLimit) {
            Log("%u physical processors found, but only using %u due to specified limit",
                numPCPUs / 2, cpuLimit / 2);
            numPCPUs = cpuLimit;
         }
      } else {
         Warning("hyperthreading will not be enabled");
         htEnabled = FALSE;

         // re-parse the global tables WITHOUT hyperthreading this time
         status = SMPParseCPUTables(vmnixInit, vmnixOptions, acpiInfo, htEnabled);
         if (status != VMK_OK) {
            return status;
         }
      }
   }

   if (!htEnabled) {
      // non-hyperthreaded maxCPUs capping
      if (vmnixOptions->maxPCPUs && numPCPUs > vmnixOptions->maxPCPUs) {
         Log("%u processors found, but only using %u due to specified limit",
             numPCPUs, vmnixOptions->maxPCPUs);
         numPCPUs = vmnixOptions->maxPCPUs;
      }
      
      // setup simple mapping with one pcpu per package
      memset(&hyperthreading, 0, sizeof(hyperthreading));
      hyperthreading.logicalPerPackage = 1;
      hyperthreading.numPackages = numPCPUs;
      for (i=0; i < numPCPUs; i++) {
         hyperthreading.cpuToPkgMap[i] = i;
         hyperthreading.packages[i].numLogical = 1;
         hyperthreading.packages[i].baseApicID = apicIDs[i];
         hyperthreading.packages[i].apicId[0] = apicIDs[i];
         hyperthreading.packages[i].logicalCpus[0] = i;
      }
   }

   // initialize TSC barrier
   SP_InitBarrier("TSC barrier", numPCPUs, &tscBarrier);

   // register the cpuinfo proc node
   Proc_InitEntry(&cpuInfoProc);
   cpuInfoProc.read = SMPCpuInfoProcRead;
   Proc_Register(&cpuInfoProc, "cpuinfo", FALSE);
   return VMK_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 * SMP_GetPackageInfo --
 *
 *     Returns a pointer to the "package" info structure, describing the package
 *     on which PCPU p lies. Returns NULL on non-HT-enabled systems.
 *
 * Results:
 *     Returns p's package structure
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
SMP_PackageInfo* 
SMP_GetPackageInfo(PCPU p)
{
   ASSERT(p >= 0 && p < numPCPUs);
   return &hyperthreading.packages[hyperthreading.cpuToPkgMap[p]];
}

/*
 *-----------------------------------------------------------------------------
 *
 * SMP_GetHTThreadNum --
 *
 *     Returns the hyperthread number corresponding to PCPU p.
 *
 * Results:
 *     Returns 0 or 1, depending on thread number
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
uint8 
SMP_GetHTThreadNum(PCPU p)
{
   SMP_PackageInfo *pkg;

   if (!hyperthreading.htEnabled) {
      return (0);
   }

   ASSERT(SMP_MAX_CPUS_PER_PACKAGE == 2);

   pkg = SMP_GetPackageInfo(p);
   return (pkg->logicalCpus[0] == p) ? 0 : 1;
}

/*
 *-----------------------------------------------------------------------------
 *
 * SMP_GetPartnerPCPU --
 *
 *     Returns the PCPU number of p's "partner," i.e. the PCPU that shares the
 *     same physical package. Do not call this function if hyperthreading
 *     is not enabled
 *
 * Results:
 *     Returns partner's PCPU number
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
PCPU
SMP_GetPartnerPCPU(PCPU p)
{
   int i;
   PCPU partner = INVALID_PCPU;
   SMP_PackageInfo *pkg = SMP_GetPackageInfo(p);

   if (!hyperthreading.htEnabled) {
      return INVALID_PCPU;
   }

   ASSERT(p < numPCPUs);

   for (i=0; i < pkg->numLogical; i++) {
      if (pkg->logicalCpus[i] != p) {
         partner = pkg->logicalCpus[i];
         break;
      }
   }

   ASSERT(partner != INVALID_PCPU);
   ASSERT(partner < numPCPUs);
   
   return partner;
}


/*
 *-----------------------------------------------------------------------------
 *
 * SMP_LogicalCPUPerPackage
 *
 *     Returns the number of logical processors per physical package in this system
 *
 * Results:
 *     Returns the number of logical processors per physical package in this system
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
uint8
SMP_LogicalCPUPerPackage(void)
{
   if (hyperthreading.htEnabled) {
      return (hyperthreading.logicalPerPackage);
   } else {
      return (1);
   }
}

/*
 *----------------------------------------------------------------------
 * SMPSetupSlaveBootPT --
 *
 *      Slave PCPUs (APs in intelease) start in real mode at
 *      VMK_STARTUP_EIP.  In order to switch to paging mode we need a
 *      pagetable where VA VMK_STARTUP_EIP maps to MA VMK_STARTUP_EIP, but
 *      the rest of the pages map to the standard vmkernel stuff.
 *      Since VA VMK_STARTUP_EIP is usualy mapped by large page, we need to
 *      create a new page table that uses small pages to map the rest of
 *      the stuff in the original large page, and have the special mapping
 *      for VMK_STARTUP_EIP.
 *
 * Results:
 *      MA of the page table root to be used for bootup
 *
 * Side effects:
 *	None.
 *----------------------------------------------------------------------
 */
MA
SMPSetupSlaveBootPT(World_Handle *world)
{
   VMK_PDPTE *pageRoot;
   VMK_PDE *pageDir;
   VMK_PTE *pageTable;
   int i;
   MA pageRootMA;
   MPN largeMPN;

   // start with an identical page table as the given world but don't share
   // the first page directory.
   pageRoot = PT_CopyPageRoot(world->pageRootMA, &pageRootMA, INVALID_MPN);
   ASSERT_NOT_IMPLEMENTED(pageRoot);
   PT_ReleasePageRoot(pageRoot);

   // now let's modify it to our needs
   ASSERT(PAGE_OFFSET(VMK_STARTUP_EIP) == 0);
   pageDir = PT_GetPageDir(pageRootMA, VMK_STARTUP_EIP, NULL);
   ASSERT_NOT_IMPLEMENTED(pageDir);

   ASSERT(pageDir[ADDR_PDE_BITS(VMK_STARTUP_EIP)] & PTE_PS);
   largeMPN = VMK_PTE_2_MPN(pageDir[ADDR_PDE_BITS(VMK_STARTUP_EIP)]);
   PT_ReleasePageDir(pageDir, NULL);

   pageTable = PT_AllocPageTable(pageRootMA, VMK_STARTUP_EIP,
                                 PTE_KERNEL, NULL, NULL);
   ASSERT_NOT_IMPLEMENTED(pageTable);

   for (i = 0; i < VMK_PTES_PER_PDE; i++) {
      if (i == ADDR_PTE_BITS(VMK_STARTUP_EIP)) {
         PT_SET(&pageTable[i], VMK_MAKE_PTE(MA_2_MPN(VMK_STARTUP_EIP), 0, PTE_P));
      } else {
         PT_SET(&pageTable[i], VMK_MAKE_PTE(largeMPN + i, 0, PTE_P));
      }
   }
   PT_ReleasePageTable(pageTable, NULL);

   return pageRootMA;
}

static INLINE void
Push(uint32 **stackPtr, uint32 val)
{
   (*stackPtr)--;
   **stackPtr = val;
}

/*
 *----------------------------------------------------------------------
 * SMPSlaveInit
 *
 *      The master PCPU (the BSP in intelease) comes to us in the hosts 
 *      world so we can create its PCPU through the normal mechanism and 
 *      just switch to it saving the state in the hosts world. However 
 *      the slave PCPUs (APs in intelease) start in real mode and have no
 *      current world, so we must have special code that initializes
 *      the world from scratch.
 *
 * Results:
 *      A VMK_ReturnStatus value.
 *
 * Side effects:
 *	None.
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
SMPSlaveInit(uint32 pcpuNum, World_Handle *world)
{
   uint8 *memptr;
   uint8 *real;
   uint8 *ptrLoc;
   DTR32 *gdt;
   Selector *tss;
   uint32 *esp;
   VMK_ReturnStatus status;

   // setup boot page table if it hasn't been initialized yet
   if (bootPageRoot == 0) {
      bootPageRoot = SMPSetupSlaveBootPT(world);
   }

   // add prda/kseg to slave world's main page table
   status = PRDA_MapRegion(pcpuNum, world->pageRootMA);
   if (status != VMK_OK) {
      return status;
   }
   status = Kseg_MapRegion(pcpuNum, world->pageRootMA);
   if (status != VMK_OK) {
      return status;
   }

   /*
    *  Setup init code that runs when AP is kicked. This must setup 
    *  all the protected mode hardware structures, switch to protected
    *  mode with paging and jump out to c code.
    *
    *  NOTE: The startup code must be at a page boundary. When it
    *  starts executing it, the AP is in real mode (16 bit data/address)
    *  with IP at 0 and CS is in a special mode where it contains the
    *  offset to the startup code. Therefore although the startup code
    *  physical address may be anywhere, its logical address always starts
    *  at 0 when executed by the AP.
    */
   ASSERT(PAGE_OFFSET(VMK_STARTUP_EIP) == 0);
   real = (char*)KVMap_MapMPN(MA_2_MPN(VMK_STARTUP_EIP), TLB_LOCALONLY);
   if (!real) {
      return VMK_FAILURE;
   }

   esp = (uint32 *)World_GetVMKStackTop(world);

   // prepare the stack for StartSlaveWorld
   Push(&esp, (uint32)SMPSlaveIdle);
   Push(&esp, world->pageRootMA);
   // push the CS:EIP for the FARRET below
   Push(&esp, DEFAULT_CS);
   Push(&esp, (uint32)StartSlaveWorld);

   memptr = real;

   EMIT_CLI;

   /*
    *
   // Invalidate caches
   EMIT_BYTE(0x0f); EMIT_BYTE(0x08); // INVD
    *
    * The caches are invalidated under the assumption that they may
    * contain garbage when the processor starts executing.
    * However invalidating the caches will crash the machine if
    * hyperthreading is enabled because it also invalidates the
    * data of the hypertwin which may contain this very emitted code.
    *
    * Using WBINVD (that is flushing) is fine but assumes that
    * the content of the caches is correct and therefore doesn't
    * really need to be flushed.
    *
    * Neither Linux nor Intel uses INVD or WBINVD at the start of what
    * a woken up processor executes.
    */

   EMIT_SAVE_SEGMENT_REG(SEG_CS, /* --> */ REG_EAX);
   EMIT_LOAD_SEGMENT_REG(SEG_DS, /* <-- */ REG_EAX);

   EMIT_OPSIZE_OVERRIDE;
   EMIT32_OR_REG_IMM(REG_EAX, hostCR4);
   EMIT_MOVE_TO_CR(REG_EAX, 4);

   EMIT_OPSIZE_OVERRIDE;
   EMIT32_LOAD_REG_IMM(REG_EAX, bootPageRoot);
   EMIT_MOVE_TO_CR(REG_EAX, 3);

   EMIT_MOVE_FROM_CR(REG_EAX, 0);
   EMIT_OPSIZE_OVERRIDE;
   EMIT32_OR_REG_IMM(REG_EAX, 0x80000001);
   EMIT_MOVE_TO_CR(REG_EAX, 0);

   ptrLoc = memptr + 2;
   EMIT_OPSIZE_OVERRIDE;
   EMIT32_LOAD_REG_IMM(REG_EBX, 0x0);

   EMIT_ADDRESS_OVERRIDE;
   EMIT_OPSIZE_OVERRIDE;
   EMIT_LIDT(0,REG_EBX);

   EMIT_ADDRESS_OVERRIDE;
   EMIT_OPSIZE_OVERRIDE;
   EMIT_LGDT(8,REG_EBX);

   EMIT_ADDRESS_OVERRIDE;
   EMIT_OPSIZE_OVERRIDE;
   EMIT_LTR(16,REG_EBX);

   EMIT_OPSIZE_OVERRIDE;
   EMIT32_LOAD_REG_IMM(REG_EAX, 0x0);
   EMIT_LLDT_REG(REG_EAX);

   EMIT_OPSIZE_OVERRIDE;
   EMIT32_LOAD_REG_IMM(REG_EAX, DEFAULT_DS);
   EMIT_LOAD_SEGMENT_REG(SEG_DS, /* <-- */ REG_EAX);
   EMIT_LOAD_SEGMENT_REG(SEG_ES, /* <-- */ REG_EAX);
   EMIT_LOAD_SEGMENT_REG(SEG_FS, /* <-- */ REG_EAX);
   EMIT_LOAD_SEGMENT_REG(SEG_GS, /* <-- */ REG_EAX);
   EMIT_LOAD_SEGMENT_REG(SEG_SS, /* <-- */ REG_EAX);

   EMIT_OPSIZE_OVERRIDE;
   EMIT32_LOAD_REG_IMM(REG_ESP, esp);

   EMIT_OPSIZE_OVERRIDE;
   EMIT_FARRET;

   /* makes above emitted code load EBX with the base pointer */
   *(uint32*)ptrLoc = (uint32)(memptr - real); // from logical 0. See NOTE above.

   /* idtr */
   IDT_GetDefaultIDT((DTR32*)memptr);
   memptr += 8;

   /* gdtr */
   gdt = (DTR32*)memptr;
   gdt->limit = DEFAULT_NUM_ENTRIES * sizeof(Descriptor) - 1;
   gdt->offset = VMK_VA_2_LA((VA) world->kernelGDT);
   LOG(1, "gdt->offset = 0x%x", gdt->offset);
   memptr += 8;

   /* tss */
   tss = (Selector *)memptr;
   *tss = MAKE_SELECTOR(DEFAULT_TSS_DESC, SELECTOR_GDT, 0);
   memptr += 4;

   KVMap_FreePages(real);

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * SMP_BootAPs --
 *
 *      Initialize the PRDAs for all the pcpus, and start the APs
 *      running just long enough to synchronize the TSCs.  They then
 *      block on a barrier in SMPSlaveIdle until SMP_StartAPs is called.
 *
 * Results: 
 *      Returns the number of cycles that the TSC was turned back
 *      when the TSCs were synced, or 0 if TSC sync is disabled.
 *      
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

// DO_WARM_BOOT seems to be necessary only for 486 and dual Pentium systems
// PPro and newer systems use the vector in the STARTUP IPI - bhlim
// #define DO_WARM_BOOT

#ifdef DO_WARM_BOOT
#define WARM_BOOT_VECTOR_EIP 0x467
#define WARM_BOOT_VECTOR_CS  0x469
#define WARM_BOOT_HLT_INSTR 0x0
#endif

TSCRelCycles
SMP_BootAPs(VMnix_Init *vmnixInit)
{
   int i, count, numAvail;
   void *page0Ptr;
   void *savedPage0;
   World_Handle *slaveWorld;
   TSCRelCycles tscOffset = 0;
   uint32 *tmp;
#ifdef DO_WARM_BOOT
   uint8 savedPost;

   OUTB(0x70, 0xf);
   savedPost = INB(0x71);
#endif

   SMPSaveCPUID();

   if (numPCPUs == 1) {
      return 0;
   }

   Log("Booting APs...");

   page0Ptr = (char*)KVMap_MapMPN(0, TLB_LOCALONLY);
   savedPage0 = Mem_Alloc(PAGE_SIZE);
   ASSERT_NOT_IMPLEMENTED(savedPage0 != NULL);
   memcpy(savedPage0, page0Ptr, PAGE_SIZE);   
   memset(page0Ptr, 0, PAGE_SIZE);
#ifdef DO_WARM_BOOT
#if 1
   // warm boot to a halt instruction
   *(uint8*)(page0Ptr+WARM_BOOT_HLT_INSTR) = 0xF4;
   *(uint16*)(page0Ptr+WARM_BOOT_VECTOR_EIP) = 0;
   *(uint16*)(page0Ptr+WARM_BOOT_VECTOR_CS)  = 0;
#else
   *(uint16*)(page0Ptr+WARM_BOOT_VECTOR_EIP) = vmnixInit->realCodeMA & 0x0f;
   *(uint16*)(page0Ptr+WARM_BOOT_VECTOR_CS)  = vmnixInit->realCodeMA >> 4;
#endif
#endif
   KVMap_FreePages(page0Ptr);

   numAvail = numPCPUs;
   for (i=0; i<numAvail; i++) {
      if (i == HOST_PCPU) {
	 // host pcpu idle world is launched by Idle_Init()
         continue;
      }
      if (apicIDs[i] >= apicID_range) {
	 // some MP config blocks are broken and report 255 (-1)
	 // e.g, on serengeti
	 Log("skipping AP %d, APICId %d", i, apicIDs[i]);
	 numPCPUs--;
	 continue;
      }

      World_NewIdleWorld(i, &slaveWorld);
      ASSERT_NOT_IMPLEMENTED(slaveWorld != NULL);
      SMPSlaveInit(i, slaveWorld);

      LOG(2, "back from World_NewIdleWorld");

#ifdef DO_WARM_BOOT
      // request warm reset 
      OUTB(0x70, 0xf);
      OUTB(0x71, 0xa);
#endif

      /*
       * Send the necessary IPIs
       */
      Log("kicking AP %d, apicID %d", i, apicIDs[i]);
      APIC_KickAP(apicIDs[i], VMK_STARTUP_EIP);

      /*
       * Wait up to 1 second for a response
       */
      for (count = 0; count < 10000; count++) {
	 if (apRunning[i]) {
	    break;
	 }
	 Util_Udelay(100);
      }

      if (! apRunning[i]) {
	 SysAlert("could not start pcpu %d", i);
	 World_DestroySlavePCPU(i);
	 numPCPUs--;
      }
   }

   // reset TSC counter, if requested
   if (tscReset) {
      tscOffset = SMPResetTSC(0);
   }


#ifdef DO_WARM_BOOT
   OUTB(0x70, 0xf);
   OUTB(0x71, savedPost);
#endif

   page0Ptr = (char*)KVMap_MapMPN(0, TLB_LOCALONLY);
   ASSERT(page0Ptr != NULL);
   tmp = (uint32 *)page0Ptr;
   for (i = 0; i < 1024; i++) {
      ASSERT(tmp[i] == 0);
   }
   memcpy(page0Ptr, savedPage0, PAGE_SIZE);
   KVMap_FreePages(page0Ptr);
   Mem_Free(savedPage0);   

   Log("...finished booting APs, numPCPUs=%d", numPCPUs);

   return tscOffset;
}


/*
 *----------------------------------------------------------------------
 *
 * SMP_StopAPs --
 *
 *      Stop the APs by making them execute a CLI;HLT
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
SMP_StopAPs(void)
{
   Bool apIsRunning;
   int i, count;

   if (numPCPUs == 1) {
      return;
   }

   ASSERT(myPRDA.pcpuState == PCPU_BSP);

   for (i = 0; i < numPCPUs; i++) {
      if (i == HOST_PCPU) {
	 continue;
      }
      // XXX should use an IPI here
      prdas[i]->stopAP = TRUE;
   }

   Log("Stopping APs...");

   count = 0;
   do {   
      apIsRunning = FALSE;   
      for (i = 0; i < numPCPUs; i++) {
	 if (i != HOST_PCPU && apRunning[i]) {
	    apIsRunning = TRUE;
	    break;
	 }
      }
      count++;
   } while (apIsRunning && count < 1000000);

   if (apIsRunning) {
      Warning("could not stop all APs");
      for (i = 0; i < numPCPUs; i++) {
	 Warning("apRunning[%d] = %d", i, apRunning[i]);
      }
      return;
   }

   Log("...APs stopped");
}


/*
 *----------------------------------------------------------------------
 *
 * SMP_GetPCPUNum --
 *
 *      Return the pcpu number associated with the APIC id
 *
 * Results: 
 *      pcpu number if found, -1 otherwise
 *      
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

PCPU
SMP_GetPCPUNum(int apicID)
{
   int i;

   /* Search for APIC Ids defined by the MPS block */
   for (i = 0; i < MAX_PCPUS; i++) {
      if (apicIDs[i] == apicID) {
	 return i;
      }
   }

   /* APIC Id wasn't defined by the MPS - set it up here */
   Warning("apicID 0x%x not found, adding it to apicIDs", apicID); 
   for (i = 0; i < MAX_PCPUS; i++) {
     if (apicIDs[i] == -1) {
       apicIDs[i] = apicID;
       return i;
     }
   }
   
   return -1;
}

/*
 *-----------------------------------------------------------------------------
 *
 * SMP_GetApicID --
 *
 *     Gets the APIC ID for pcpuNum
 *
 * Results:
 *     Returns appropriate apicID
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
int
SMP_GetApicID(PCPU pcpuNum)
{
   ASSERT(pcpuNum < numPCPUs);
   return apicIDs[pcpuNum];
}

// helper macro for SMPCpuInfoProcRead, prints a line containing "_name"
// and prints "_val" once on that line for each pcpu, using the format "_fmt"
#define ALL_CPUS_PRINTF(_name, _fmt, _val) \
 do {  \
  Proc_Printf(buf, len, "%8s", _name); \
  for (i=0; i < numPCPUs; i++) { \
    Proc_Printf(buf, len, _fmt, _val); \
  } \
  Proc_Printf(buf, len, "\n"); \
 } while (0)

/*
 *-----------------------------------------------------------------------------
 *
 * SMPCpuInfoProcRead --
 *
 *      Proc read callback to display cpu info for all processors
 *      (like linux's /proc/cpuinfo)
 *
 * Results:
 *      Returns VMK_OK
 *
 * Side effects:
 *      Writes to proc buffer
 *
 *-----------------------------------------------------------------------------
 */
int
SMPCpuInfoProcRead(Proc_Entry *entry, char *buf, int *len)
{
   unsigned i;
   *len = 0;

   ALL_CPUS_PRINTF("pcpu", "            %02u", i);
   Proc_Printf(buf, len, "\n");
   ALL_CPUS_PRINTF("family", "            %02u",
                   CPUID_FAMILY(cpuids[i].id1.version));
   ALL_CPUS_PRINTF("model", "            %02u",
                   CPUID_MODEL(cpuids[i].id1.version));
   ALL_CPUS_PRINTF("type", "            %02u",
                   CPUID_TYPE(cpuids[i].id1.version));
   ALL_CPUS_PRINTF("stepping", "            %02u",
                   CPUID_STEPPING(cpuids[i].id1.version));
   ALL_CPUS_PRINTF("cpuKhz", "       %7d", (uint32)(prdas[i]->cpuHzEstimate / 1000));
   ALL_CPUS_PRINTF("busKhz", "       %7d", (uint32)(prdas[i]->busHzEstimate / 1000));
   ALL_CPUS_PRINTF("name", "%14s", cpuids[i].id0.name);
   ALL_CPUS_PRINTF("ebx", "    0x%08x", cpuids[i].id1.ebx);
   ALL_CPUS_PRINTF("ecxFeat", "    0x%08x", cpuids[i].id1.ecxFeatures);
   ALL_CPUS_PRINTF("edxFeat", "    0x%08x", cpuids[i].id1.edxFeatures);
   ALL_CPUS_PRINTF("initApic", "    0x%08x", initialAPICIDs[i]);
   ALL_CPUS_PRINTF("apicID", "    0x%08x", apicIDs[i]);

   return (VMK_OK);
}

 
