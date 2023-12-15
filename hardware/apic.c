/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * apic.c --
 *
 *	This module manages the local apic.
 */


#include "vm_types.h"
#include "vm_libc.h"
#include "vmkernel.h"
#include "kvmap.h"
#include "apic_int.h"
#include "host.h"
#include "util.h"
#include "smp_int.h"
#include "timer.h"
#include "nmi.h"
#include "idt.h"
#include "ioapic.h"
#include "mps.h"
#include "chipset_int.h"

#define LOGLEVEL_MODULE APIC
#define LOGLEVEL_MODULE_LEN 4
#include "log.h"


/*
 * Globals 
 */
struct APIC apicInfo;
struct APIC *apic = NULL;

/*
 * Converting APIC IDs to PCPUNum, where PCPUNum = [0..numPCPUs),
 * and logical IDs
 */
#define APICID_RANGE	256
uint32 apicID_range = 0;
static uint32 apicID_mask;
static PCPU apicPCPUmap[APICID_RANGE];
static uint32 apicLogID[APICID_RANGE];

static uint32 apicHostSVR;
static uint32 apicHostLVT0;
static uint32 apicHostLVT1;
static uint32 apicHostERRLVT;
static uint32 apicHostPCLVT;
static uint32 apicHostTHERMLVT;
static uint32 apicHostTPR;
static uint32 apicHostLDR;
static uint32 apicHostDFR;
static uint32 apicHostTIMERLVT;

static uint32 apicSelfIntVector;

static Bool apicInitialized = FALSE;

static VMK_ReturnStatus APICMasterInit(ICType hostICType, VMnix_SharedData *sharedData, Bool realNMI);
static VMK_ReturnStatus APICEnable(Bool isBSP, ICType hostICType, Bool realNMI);
static Bool APICSetupFastTimer(void);
static void APICEnableFastTimer(void);
static void APICTimerIntHandler(void *clientData, uint32 vector);
static void APICNoOpIntHandler(void *clientData, uint32 vector);
static void APICThermalIntHandler(void *clientData, uint32 vector);
static void APICLint1IntHandler(void *clientData, uint32 vector);
static void APICErrorIntHandler(void *clientData, uint32 vector);
static void APICSpuriousIntHandler(void *clientData, uint32 vector);
static void APICIPIIntHandler(void *clientData, uint32 vector);
static void APICSendIPI(uint32 dest, uint32 mode);



/*
 *----------------------------------------------------------------------
 *
 * APIC_Init --
 *
 *      Initialize the apic module.
 *      Called only in the BSP.
 *
 * Results:
 *      VMK_OK on success, error code otherwise.
 *
 * Side effects:
 *      APIC is set up.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
APIC_Init(ICType hostICType,
	  VMnix_ConfigOptions *vmnixOptions,
	  VMnix_SharedData *sharedData)
{
   uint32 i;
   VMK_ReturnStatus status;

   apicID_mask = (cpuType==CPUTYPE_INTEL_PENTIUM4)? XAPIC_ID_MASK:APIC_ID_BITS;
   apicID_range = (apicID_mask>>APIC_ID_SHIFT); // top value for broadcast only
   ASSERT(apicID_range <= APICID_RANGE);

   for (i = 0; i < apicID_range; i++) {
      apicPCPUmap[i] = INVALID_PCPU;
      apicLogID[i] = 0;
   }

   apic = &apicInfo;
   apic->baseAddr = 0;
   apic->reg = 0;

   /* 
    * Logical IDs are based on the order CPUs are discovered in the MPS/ACPI
    * table, i.e. pcpunum, and the apicLogID[] array is filled then, except
    * for certain machines, such as IBM VIGILs for which a custom mapping is
    * mandatory.
    */
   if (MPS_Signature == IBM_X440) {
      Log("Computing IBM Vigil specific Logical ID settings");
      for (i = 0; i < 0x40; i++) {
	 apicLogID[i] = (i & 0x0f0) + (1 << (i & 0x03));
      }
   }

   /*
    * Set the destination mode based on user request and constraints.
    */
   apic->destMode = APIC_DESTMODE_PHYS;
   apic->flatFormat = TRUE;
   if (vmnixOptions->logicalApicId) {
      if (MPS_Signature == IBM_X440) {
	 apic->destMode = APIC_DESTMODE_LOGICAL;
         apic->flatFormat = FALSE;
      } else if (numPCPUs > 8) {
	 Warning("Cannot use flat logical mode with more than 8 CPUs");
      } else {
	 apic->destMode = APIC_DESTMODE_LOGICAL;
      }
   }
   
   Log("Using %s %s mode for destination",
	      apic->flatFormat ? "flat" : "clustered",
	      apic->destMode == APIC_DESTMODE_LOGICAL ? "logical" : "physical");

   status = APICMasterInit(hostICType, sharedData, vmnixOptions->realNMI);
   if (status == VMK_OK) {
      apicInitialized = TRUE;
   }
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * APIC_FindID --
 *
 *      Return the APIC ID of a CPU based on its PCPUNum
 *
 * Results:
 *      valid id if found, -1 otherwise
 *
 * Side effects:
 *      none
 *
 *----------------------------------------------------------------------
 */
int
APIC_FindID(PCPU pcpuNum)
{
  int i;

  for (i = 0; i < apicID_range; i++) {
    if (apicPCPUmap[i] != INVALID_PCPU && apicPCPUmap[i] == pcpuNum) {
      return i;
    }
  }
  return -1;
}

/*
 *----------------------------------------------------------------------
 *
 * APICAddtoPCPUMap --
 *
 *      Add the apicID to the PCPU map
 *
 * Results:
 *      corresponding pcpuNum
 *
 * Side effects:
 *      apicPCPUmap 
 *
 *----------------------------------------------------------------------
 */
static PCPU
APICAddtoPCPUMap(int apicID)
{
   PCPU pcpuNum;

   ASSERT(apicID < apicID_range);
   ASSERT(apicPCPUmap[apicID] == INVALID_PCPU);

   pcpuNum = SMP_GetPCPUNum(apicID);

   ASSERT(pcpuNum != INVALID_PCPU && pcpuNum < numPCPUs);
   apicPCPUmap[apicID] = pcpuNum;

   return pcpuNum;
}
 

/*
 *----------------------------------------------------------------------
 *
 * APICEnable --
 *
 *      Initialize the local apic registers to enable it.
 *
 * Results:
 *      VMK_OK on success, error code otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
APICEnable(Bool isBSP, ICType hostICType, Bool realNMI)
{
   uint8 apicID;
   uint32 reg, version;
   uint32 apicMSR;
   PCPU pcpuNum;
   int i;


   /*
    * The COS APIC setup is different from ours. Rather than track the
    * differences, we save what we change and restore it when unloading.
    *
    * NOTE: This only applies to the BSP, since COS is UP.
    */
 
   /*
    * Enable the APIC in the MSR
    */
   ASM("rdmsr" :
       "=a" (apicMSR) :
       "c" (APIC_BASE_MSR) :
       "edx");

   if (!(apicMSR & APIC_MSR_ENABLED)) {
      apicMSR |= APIC_MSR_ENABLED;
      ASM("wrmsr" :
          :
          "a" (apicMSR),
          "d" (0),
          "c" (APIC_BASE_MSR));
   }

   /*
    * Initialize baseaddr and reg if not already
    */
   if (apic->baseAddr == 0) {
      apic->baseAddr = (MA)(apicMSR & APIC_MSR_BASEMASK);
      // need to map uncached cf. Intel 7.6.6p7-21vol3
      apic->reg = KVMap_MapMPN(MA_2_MPN(apicMSR & APIC_MSR_BASEMASK), TLB_UNCACHED);
   }

   if (!apic->reg) {
      return VMK_NO_RESOURCES;
   }

   /*
    * Make sure we are looking a a Pentium-class APIC
    */
   version = (apic->reg[APICR_VERSION][0] & 0xFF);
   if ((version & 0xF0) != 0x10) {
      Warning("unsupported version found: 0x%x", version);
      return VMK_UNSUPPORTED_CPU;
   }

   /*
    * Set up the spurious interrupt vector and enable the APIC
    */
   reg = apic->reg[APICR_SVR][0];
   if (isBSP) {
      apicHostSVR = reg;
   }
   reg |= APIC_SVR_APICENABLE;	/* Enable APIC (bit==1) */
   reg &= ~APIC_SVR_FOCUSCHECK;	/* don't care but needs to be 0 on P4 & above */
   reg |= IDT_APICSPURIOUS_VECTOR; /* Set spurious int vector */
   apic->reg[APICR_SVR][0] = reg;

   /*
    * Set up local interrupt pins
    */
   if (!isBSP) {
      apic->reg[APICR_LVT0][0] = APIC_VTE_MASK | APIC_VTE_MODE_EXTINT;
      apic->reg[APICR_LVT1][0] = APIC_VTE_MASK | APIC_VTE_MODE_NMI;
   } else {
      apicHostLVT0 = apic->reg[APICR_LVT0][0];
      if (Chipset_ICType == ICTYPE_PIC) {
	 ASSERT(hostICType == ICTYPE_PIC);
	 Log("enabling LINT0 as ExtINT for PIC interrupts");
         apic->reg[APICR_LVT0][0] = APIC_VTE_MODE_EXTINT;
      } else {
	 ASSERT(Chipset_ICType == ICTYPE_IOAPIC);
	 apic->reg[APICR_LVT0][0] = APIC_VTE_MASK | APIC_VTE_MODE_EXTINT;
      }
      apicHostLVT1 = apic->reg[APICR_LVT1][0];
      if (realNMI) {
	 Log("enabling LINT1 as NMI");
	 apic->reg[APICR_LVT1][0] = APIC_VTE_MODE_NMI;
      } else {
	 Log("enabling LINT1 as normal interrupt");
	 apic->reg[APICR_LVT1][0] = IDT_APICLINT1_VECTOR;
      }
   }

   /*
    * Setup error vector
    */
   apic->reg[APICR_ESR][0] = 0;
   if (isBSP) {
      apicHostERRLVT = apic->reg[APICR_ERRLVT][0];
   }
   apic->reg[APICR_ERRLVT][0] = IDT_APICERROR_VECTOR;

   /*
    * Mask perf vector by default
    *
    * NOTE: When writing to an LVT register, the vector has to be valid.
    * Simply writing the mask bit (and therefore using a null vector) will
    * generate an APIC error.
    */
   if (isBSP) {
      apicHostPCLVT = apic->reg[APICR_PCLVT][0];
   }
   apic->reg[APICR_PCLVT][0] = APIC_VTE_MASK | IDT_NOOP_VECTOR;

   /*
    * Setup thermal vector
    */
   if (cpuType == CPUTYPE_INTEL_PENTIUM4) {
      if (isBSP) {
	 apicHostTHERMLVT = apic->reg[APICR_THERMLVT][0];
      }
      apic->reg[APICR_THERMLVT][0] = IDT_APICTHERMAL_VECTOR;
   }

   /*
    * Set Task Priority to 'accept all'
    */
   reg = apic->reg[APICR_TPR][0];
   if (isBSP) {
      apicHostTPR = reg;
   }
   reg &= ~APIC_PR_MASK;
   apic->reg[APICR_TPR][0] = reg;

   Util_Udelay(100);

   /*
    * Get apicID from register.  Reset to 0 if it's all 1's
    */
   apicID = (apic->reg[APICR_ID][0] & apicID_mask) >> APIC_ID_SHIFT;
   if ((apicID == (apicID_mask>>APIC_ID_SHIFT)) && (numPCPUs == 1)) {
      // some Athlons do this
      uint32 reg = apic->reg[APICR_ID][0];
      reg &= ~apicID_mask;
      Warning("Initializing APIC id to 0");
      apic->reg[APICR_ID][0] = reg;
      apicID = (apic->reg[APICR_ID][0] & apicID_mask) >> APIC_ID_SHIFT;
   }
      
   pcpuNum = APICAddtoPCPUMap(apicID);
   if (isBSP) {
      ASSERT(pcpuNum == HOST_PCPU);
   } else {
      ASSERT(pcpuNum != HOST_PCPU);
   }

   /*
    * Program LDR with the value from apicLogID[] if set or with
    * pcpunum otherwise.
    *
    * NOTE: See PR 20336.
    * In logical mode, if several cpus have the same LDR but only one cpu
    * is enabled, some chipsets will nevertheless get confused and not deliver
    * an interrupt targeted to that LDR to the lone enabled CPU.
    * IOAPIC COS is using logical mode with only BSP enabled so we need to
    * make sure no AP has the same LDR as BSP when we unload the vmkernel.
    * The proper place to take care of that is APIC_RestoreHostSetup() but it's
    * easier to just make sure here.
    */
   reg = apic->reg[APICR_LDR][0];
   if (isBSP) {
      apicHostLDR = reg;
      ASSERT_NOT_IMPLEMENTED(hostICType == ICTYPE_PIC ||
				(apicHostLDR >> APIC_LDR_SHIFT) == 1);
   }
   reg &= ~APIC_LDR_BITS;
   if (apicLogID[apicID] == 0) {
      if (pcpuNum < 8) {
         apicLogID[apicID] = 1 << pcpuNum;
      } else {
	 ASSERT(apic->destMode != APIC_DESTMODE_LOGICAL);
	 apicLogID[apicID] = 1 << 7;
      }
   }
   ASSERT_NOT_IMPLEMENTED(hostICType == ICTYPE_PIC || isBSP ||
				(apicLogID[apicID] != 1));
   reg |= (apicLogID[apicID] << APIC_LDR_SHIFT);
   apic->reg[APICR_LDR][0] = reg;

   reg = apic->reg[APICR_DFR][0];
   if (isBSP) {
      apicHostDFR = reg;
   }
   if (apic->flatFormat) {
      reg |= (0xF << 28);
   }
   else {
      reg &= ~(0xF << 28);
   }
   apic->reg[APICR_DFR][0] = reg;

   LOG(0, "apicID=%02X logID=%02X LDR=%08X DFR=%08X",
       apicID,
       apicLogID[apicID],
       apic->reg[APICR_LDR][0],
       apic->reg[APICR_DFR][0]);

   /*
    * There definitely should not be any interrupts being serviced.
    * There also should not be any pending interrupts since they
    * should have been drained by the vmnix module.
    */
   APIC_Dump(NULL, NULL);
   for (i = 0; i < 8; i++) {
      reg = apic->reg[APICR_ISR+i][0];
      ASSERT(reg == 0);
      reg = apic->reg[APICR_IRR+i][0];
      ASSERT(reg == 0);
   }

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * APICMasterInit --
 *
 *      Enable the local apic for the BSP.
 *
 * Results:
 *      VMK_OK on success, error code otherwise.
 *
 * Side effects:
 *      Local APIC is set up.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
APICMasterInit(ICType hostICType, VMnix_SharedData *sharedData, Bool realNMI)
{
   VMK_ReturnStatus status;
   Bool registered;

   status = APICEnable(TRUE, hostICType, realNMI);
   if (status != VMK_OK) {
      Warning("master APIC enable failed");
      return status;
   }

   /*
    * Setup No Op, Thermal, LINT1, Error and Spurious interrupt handlers
    */
   registered = IDT_VectorAddHandler(IDT_NOOP_VECTOR,
		   	APICNoOpIntHandler, NULL, FALSE, "noop", 0);
   if (! registered) {
      return VMK_NO_RESOURCES;
   }
   registered = IDT_VectorAddHandler(IDT_APICTHERMAL_VECTOR,
			APICThermalIntHandler, NULL, FALSE, "thermal", 0);
   if (! registered) {
      return VMK_NO_RESOURCES;
   }
   registered = IDT_VectorAddHandler(IDT_APICLINT1_VECTOR,
			APICLint1IntHandler, NULL, FALSE, "lint1", 0);
   if (! registered) {
      return VMK_NO_RESOURCES;
   }
   registered = IDT_VectorAddHandler(IDT_APICERROR_VECTOR,
		   	APICErrorIntHandler, NULL, FALSE, "error", 0);
   if (! registered) {
     return VMK_NO_RESOURCES;
   }
   registered = IDT_VectorAddHandler(IDT_APICSPURIOUS_VECTOR,
		   	APICSpuriousIntHandler, NULL, FALSE, "spurious", 0);
   if (! registered) {
     return VMK_NO_RESOURCES;
   }

   /*
    * Setup monitor IPI handler
    */
   registered = IDT_VectorAddHandler(IDT_MONITOR_IPI_VECTOR,
                                     APICIPIIntHandler, NULL, FALSE, 
                                     "monitor", 0);
   if (! registered) {
      return VMK_NO_RESOURCES;
   }

   /*
    * For the benefit of the vmnix module
    */
   apicSelfIntVector = IDT_NOOP_VECTOR;
   SHARED_DATA_ADD(sharedData->apicSelfIntVector, int32 *, &apicSelfIntVector);

   if (! APICSetupFastTimer()) {
     return VMK_NO_RESOURCES;
   }

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * APIC_SlaveInit --
 *
 *      Enable the local apic for a slave cpu.
 *      Assumes that APICEnable has already been executed on master
 *      so that the apic pointers are already set up.
 *
 * Results:
 *      VMK_OK on success, error code otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
APIC_SlaveInit(void)
{
   VMK_ReturnStatus status;

   // NOTE: Last two parameters are ignored for slave cpus
   status = APICEnable(FALSE, ICTYPE_UNKNOWN, TRUE);
   if (status != VMK_OK) {
      Warning("slave APIC enable failed");
      return status;
   }

   APICEnableFastTimer();

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * APIC_RestoreHostSetup
 *
 *      Return the local apic for this cpu to a state acceptable by COS.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      APIC state
 *
 *----------------------------------------------------------------------
 */

void
APIC_RestoreHostSetup(void)
{
   NMI_Disallow();

   /*
    * The COS APIC setup is different from ours. Rather than track the
    * differences, we saved what we changed and restore it here.
    *
    * NOTE: We checked in APICSetupFastTimer() that COS is not using TIMER_LVT.
    * Unfortunately there may be an APIC timer interrupt in flight, so
    * even though by restoring we are disabling, the pending interrupt will
    * still be delivered.
    *
    * NOTE: When writing to an LVT register, the vector has to be valid.
    * Simply writing the mask bit (and therefore using a null vector) will
    * generate an APIC error.
    * We use the vector that COS has for CASCADE_IRQ since this is harmless.
    */
   Log("DFR %08x, LDR %08x", apicHostDFR, apicHostLDR);
   apic->reg[APICR_DFR][0] = apicHostDFR;
   apic->reg[APICR_LDR][0] = apicHostLDR;

   Log("TPR %08x, SVR %08x",  apicHostTPR, apicHostSVR);
   apic->reg[APICR_TPR][0] = apicHostTPR;
   apic->reg[APICR_SVR][0] = apicHostSVR;

   Log("LVT0 %08x, LVT1 %08x", apicHostLVT0, apicHostLVT1);
   apic->reg[APICR_LVT0][0] = apicHostLVT0;
   apic->reg[APICR_LVT1][0] = apicHostLVT1;

   Log("ERRLVT %08x, TIMERLVT %08x", apicHostERRLVT, apicHostTIMERLVT);
   apic->reg[APICR_ERRLVT][0] = apicHostERRLVT;
   apic->reg[APICR_TIMERLVT][0] = APIC_VTE_MASK | (CASCADE_IRQ+0x20);

   Log("PCLVT %08x, THERMLVT %08x(%spresent)", apicHostPCLVT, apicHostTHERMLVT,
		   cpuType == CPUTYPE_INTEL_PENTIUM4 ? "" : "not ");
   // They may have been in the default state (masked with null vector).
   // We cannot restore it (see NOTE above).
   if (apicHostPCLVT == APIC_VTE_MASK)  {
      apic->reg[APICR_PCLVT][0] = APIC_VTE_MASK | (CASCADE_IRQ+0x20);
   } else {
      apic->reg[APICR_PCLVT][0] = apicHostPCLVT;
   }
   if (cpuType == CPUTYPE_INTEL_PENTIUM4) {
      if (apicHostTHERMLVT == APIC_VTE_MASK) {
	 apic->reg[APICR_THERMLVT][0] = APIC_VTE_MASK | (CASCADE_IRQ+0x20);
      } else {
         apic->reg[APICR_THERMLVT][0] = apicHostTHERMLVT;
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * APICMakeIPIMode --
 *
 * 	Builds an IPI mode from its components
 *
 * Results:
 * 	The value that should go into APIC ICRLO
 *
 * Side Effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE uint32
APICMakeIPIMode(uint32 vector,
		uint32 delMode,
		uint32 destMode,
		uint32 level,
		uint32 trigger,
		uint32 destShorthand)
{
   return vector | (delMode << APIC_ICRLO_DELMODE_OFFSET) |
	  (destMode << APIC_ICRLO_DESTMODE_OFFSET) |
	  (level << APIC_ICRLO_LEVEL_OFFSET) |
	  (trigger << APIC_ICRLO_TRIGGER_OFFSET) |
	  (destShorthand << APIC_ICRLO_DEST_OFFSET);
}

/*
 *----------------------------------------------------------------------
 *
 * APICSendInitIPI --
 *
 *      Send an Init IPI to dest using destMode
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Target pcpu is initialized
 *
 *----------------------------------------------------------------------
 */
static void
APICSendInitIPI(uint32 dest,
	        uint32 destMode)
{
   uint32 timeout;
   uint32 send_status;
   uint32 reg;
   uint32 mode;

   // clear the error register
   reg = apic->reg[APICR_SVR][0];
   apic->reg[APICR_ESR][0] = 0;
   apic->reg[APICR_ESR][0] = 0;    /* 2nd write clears */
   reg = apic->reg[APICR_ESR][0];

   // Send an INIT IPI
   mode = APICMakeIPIMode(0, APIC_DELMODE_INIT, destMode, APIC_POLARITY_LOW,
		   APIC_TRIGGER_EDGE, APIC_DEST_DEST);
   APICSendIPI(dest, mode);

   // Wait for completion
   timeout = 0;
   do {
      Util_Udelay(100);
      send_status = apic->reg[APICR_ICRLO][0] & APIC_ICRLO_STATUS_MASK;
   } while (send_status && (timeout++ < 1000));

   Util_Udelay(10*1000);

   // Send an INIT Level De-Assert IPI
   mode = APICMakeIPIMode(0, APIC_DELMODE_INIT, destMode, APIC_POLARITY_HIGH,
		   APIC_TRIGGER_LEVEL, APIC_DEST_ALL_INC);
   APICSendIPI(dest, mode);
   
   // Wait for completion
   timeout = 0;
   do {
      Util_Udelay(100);
      send_status = apic->reg[APICR_ICRLO][0] & APIC_ICRLO_STATUS_MASK;
   } while (send_status && (timeout++ < 1000));

   // read the error register for any IPI errors
   reg = apic->reg[APICR_SVR][0];
   apic->reg[APICR_ESR][0] = 0;
   reg = apic->reg[APICR_ESR][0] & 0xEF;

   if (send_status) {
      Warning("INIT IPI never delivered???");
   }
   if (reg) {
      Warning("INIT IPI delivery error (0x%x).", reg);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * APICSendStartupIPI --
 *
 *      Send a startup IPI to destAPICId
 *
 * Results:
 *      TRUE if IPI was successful
 *
 * Side effects:
 *      Target pcpu is started
 *
 *----------------------------------------------------------------------
 */

static Bool
APICSendStartupIPI(uint32 dest,
		   uint32 destMode,
		   uint32 eip)
{
   int i;
   int timeout;
   uint32 reg;
   uint32 send_status;
   uint32 mode;

   for (i = 0; i < 2; i++) {
      // clear the error register
      reg = apic->reg[APICR_SVR][0];
      apic->reg[APICR_ESR][0] = 0;
      apic->reg[APICR_ESR][0] = 0;    /* 2nd write clears */
      reg = apic->reg[APICR_ESR][0];

      // send the startup IPI
      mode = APICMakeIPIMode(eip>>12, APIC_DELMODE_STARTUP, destMode,
		      APIC_POLARITY_LOW, APIC_TRIGGER_EDGE, APIC_DEST_DEST);
      APICSendIPI(dest, mode);

      // wait for completion, or error status
      timeout = 0;
      do {
	 send_status = apic->reg[APICR_ICRLO][0] & APIC_ICRLO_STATUS_MASK;
	 Util_Udelay(100);
      } while (send_status && (timeout++ < 1000));

      Util_Udelay(200);

      // read the error register for any IPI errors
      reg = apic->reg[APICR_SVR][0];
      apic->reg[APICR_ESR][0] = 0;
      reg = apic->reg[APICR_ESR][0] & 0xEF;

      if (send_status || reg) {
	 break;
      }
   }

   if (send_status) {
      Warning("APIC startup IPI never delivered???");
   }
   if (reg) {
      Warning("APIC startup IPI delivery error (0x%x).", reg);
   }

   return ((send_status == 0) && (reg == 0));
}



/*
 *----------------------------------------------------------------------
 *
 * APICSendIPI --
 *
 *      Send an IPI to dest using the specified mode
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Target pcpu is interrupted
 *
 *----------------------------------------------------------------------
 */
static void
APICSendIPI(uint32 dest,
	    uint32 mode)
{
   uint32 eflags;
   uint32 send_status;
   int timeout;
   int i;
   uint32 reg;

   SAVE_FLAGS(eflags);
   if (eflags & EFLAGS_IF) {
      CLEAR_INTERRUPTS();
   }

   send_status = apic->reg[APICR_ICRLO][0] & APIC_ICRLO_STATUS_MASK;
   for (i = 0; send_status; i++) {
      timeout = 0;
      do {
	 if (eflags & EFLAGS_IF) {
	    RESTORE_FLAGS(eflags);
	 }
	 Util_Udelay(1);
	 if (eflags & EFLAGS_IF) {
	    CLEAR_INTERRUPTS();
	 }
	 send_status = apic->reg[APICR_ICRLO][0] & APIC_ICRLO_STATUS_MASK;
      } while (send_status && (timeout++ < 1000));
      if (send_status) {
	 Warning("APIC on pcpu %d still busy for IPI after %dms (%x,%x)",
			 APIC_GetPCPU(), i, dest, mode);
      }
   }

   reg = apic->reg[APICR_ICRHI][0];
   reg = (reg & APIC_ICRHI_RESERVED) | (dest << APIC_ICRHI_DEST_OFFSET);
   apic->reg[APICR_ICRHI][0] = reg;

   reg = apic->reg[APICR_ICRLO][0];
   reg = (reg & APIC_ICRLO_RESERVED) | mode;
   apic->reg[APICR_ICRLO][0] = reg;

   if (eflags & EFLAGS_IF) {
      RESTORE_FLAGS(eflags);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * APIC_KickAP --
 *
 *      Start the Application Processor by sending it the necessary
 *      IPIs. 
 *
 * Results:
 *      TRUE if IPIs were sent without any trouble, FALSE otherwise
 *
 * Side effects:
 *      Target AP initializes and starts executing
 *
 *----------------------------------------------------------------------
 */
Bool
APIC_KickAP(int apicID,
            uint32 eip)
{
   uint32 reg;

   /*
    * Clear any APIC errors
    */
   reg = apic->reg[APICR_SVR][0];
   apic->reg[APICR_ESR][0] = 0;
   apic->reg[APICR_ESR][0] = 0;    /* 2nd write clears */
   reg = apic->reg[APICR_ESR][0];

   APICSendInitIPI(apicID, APIC_DESTMODE_PHYS);

   return (APICSendStartupIPI(apicID, APIC_DESTMODE_PHYS, eip));
}

/*
 *----------------------------------------------------------------------
 *
 * APICGetDest --
 *
 * 	Get destination based on APIC ID.
 *
 * Results:
 * 	Destination to use in APIC ICRHI
 *
 * Side Effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE uint32
APICGetDest(int apicID)
{
   ASSERT(apicID != -1);
   return (apic->destMode == APIC_DESTMODE_LOGICAL) ? apicLogID[apicID]:apicID;
}

/*
 *----------------------------------------------------------------------
 *
 * APIC_SendIPI --
 *
 *      Send an IPI to processor pcpuNum that generates vector
 *
 * Results:
 *      None
 *
 * Side effects:
 *      remote processor gets interrupt at vector
 *
 *----------------------------------------------------------------------
 */
void
APIC_SendIPI(PCPU pcpuNum, uint32 vector)
{
   uint32 mode;
   int apicID = APIC_FindID(pcpuNum);

   mode = APICMakeIPIMode(vector, APIC_DELMODE_FIXED, apic->destMode,
		   APIC_POLARITY_LOW, APIC_TRIGGER_EDGE, APIC_DEST_DEST);
   APICSendIPI(APICGetDest(apicID), mode);
}

/*
 *----------------------------------------------------------------------
 *
 * APIC_BroadcastIPI --
 *
 *      Broadcast an IPI to all that generates vector.
 *      Note: With DEST_ALL_EXC, the destination mode is ignored.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      remote processor gets interrupt at vector
 *
 *----------------------------------------------------------------------
 */
void
APIC_BroadcastIPI(uint32 vector)
{
   uint32 mode;

   mode = APICMakeIPIMode(vector, APIC_DELMODE_FIXED, APIC_DESTMODE_PHYS,
		   APIC_POLARITY_LOW, APIC_TRIGGER_EDGE, APIC_DEST_ALL_EXC);
   APICSendIPI(0, mode);
}

/*
 *----------------------------------------------------------------------
 *
 * APIC_SendNMI --
 *
 * 	Send an NMI to processor pcpuNum.
 * 	Note: With APIC_DELMODE_NMI, the vector is ignored.
 *
 * Results:
 * 	None
 *
 * Side effects:
 * 	remote processor gets NMI
 * 	
 *----------------------------------------------------------------------
 */
void
APIC_SendNMI(PCPU pcpuNum)
{
   uint32 mode;
   int apicID = APIC_FindID(pcpuNum);

   if (apicID != -1) {
      mode = APICMakeIPIMode(0, APIC_DELMODE_NMI, apic->destMode,
		      APIC_POLARITY_LOW, APIC_TRIGGER_EDGE, APIC_DEST_DEST);
      APICSendIPI(APICGetDest(apicID), mode);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * APIC_BroadcastNMI --
 *
 *      Broadcast an NMI to all.
 *      Note: With APIC_DELMODE_NMI, the vector is ignored.
 *      Note: With DEST_ALL_EXC, the destination mode is ignored.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      remote processors get NMI
 *
 *----------------------------------------------------------------------
 */
void
APIC_BroadcastNMI(void)
{
   uint32 mode;

   mode = APICMakeIPIMode(0, APIC_DELMODE_NMI, APIC_DESTMODE_PHYS,
                   APIC_POLARITY_LOW, APIC_TRIGGER_EDGE, APIC_DEST_ALL_EXC);
   APICSendIPI(0, mode);
}

/*
 *----------------------------------------------------------------------
 *
 * APICGetID --
 *
 *      Return this apic's physical ID from the ID register.
 *
 * Results:
 *      This apic's ID.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
APICGetID(void)
{
   volatile int id;
   if (apic == NULL || apic->reg == NULL) {
      return 0;
   } else {
      id = ((apic->reg[APICR_ID][0] & apicID_mask) >> APIC_ID_SHIFT);
      ASSERT(id != (apicID_mask >> APIC_ID_SHIFT));      
      return id;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * APIC_GetPCPU --
 *
 *      Return the callers pcpu number using the APIC id
 *
 * Results:
 *      Caller's pcpu
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
PCPU
APIC_GetPCPU(void)
{
   PCPU pcpuNum = apicPCPUmap[APICGetID()];
   if (!apicInitialized) {
      return HOST_PCPU;
   }
   ASSERT(pcpuNum != INVALID_PCPU && pcpuNum < numPCPUs);
   return pcpuNum;
}

/*
 *----------------------------------------------------------------------
 *
 * APICNoOpIntHandler --
 *
 *      Handle a No Op interrupt.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
APICNoOpIntHandler(UNUSED_PARAM(void *clientData), uint32 vector)
{
}

/*
 *----------------------------------------------------------------------
 *
 * APICThermalIntHandler --
 *
 * 	Handle a Thermal interrupt
 *
 * Results:
 * 	None.
 *
 * Side Effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
static void
APICThermalIntHandler(void *clientData, uint32 vector)
{
   SysAlert("Thermal interrupt on pcpu %d", APIC_GetPCPU());
}

/*
 *----------------------------------------------------------------------
 *
 * APICLint1IntHandler --
 *
 * 	Handle a Lint1 APIC interrupt
 *
 * Results:
 * 	None.
 *
 * Side effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
static void
APICLint1IntHandler(void *clientData, uint32 vector)
{
   // This would have been a motherboard NMI
   SysAlert("Lint1 interrupt on pcpu %d (port x61 contains 0x%x)",
	      		   APIC_GetPCPU(), INB(0x61));
   NMI_Pending = TRUE;
}

/*
 *----------------------------------------------------------------------
 *
 * APICSpuriousIntHandler --
 *
 *      Handle a spurious APIC interrupt
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
APICSpuriousIntHandler(UNUSED_PARAM(void *clientData), uint32 vector)
{
#ifdef VMX86_DEBUG
   Warning("on %d  - shouldn't occur", APICGetID());
#endif
}

/*
 *----------------------------------------------------------------------
 *
 * APICErrorIntHandler --
 *
 *      Handle an APIC error interrupt
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
APICErrorIntHandler(UNUSED_PARAM(void *clientData), uint32 vector)
{
   uint32 error;
   int logging;


   // write any value to load ESR with error value
   apic->reg[APICR_ESR][0] = 0;
   
   // read to get error value
   error = apic->reg[APICR_ESR][0];

   // write twice any value to clear
   apic->reg[APICR_ESR][0] = 0;
   apic->reg[APICR_ESR][0] = 0;

   // apic errors should not happen, report as an alert by default
   logging = 2;

   // this may not be unexpected depending on chipset
   switch (MPS_Signature) {
   case P3_IOAPIC_0X11:
      if (error & 0x03) {
	 // Send/Receive Checksum Error
	 IOAPIC_ResetPins(TRUE);
	 logging = 1; // report as a warning (bad chipset)
      };
      break;
   case IBM_X440:
      if (error == 0x80) {
	 // Illegal Register Address - presumably due to chipset bug
#ifdef VMX86_DEBUG
	 logging = 1; // report as a warning (bad chipset)
#else
	 logging = 0; // don't report (bad chipset but too frequent)
#endif
      }
      break;
   default:
      break;
   }

   switch (logging) {
   case 0:
      break;
   case 1:
      Warning("APICID 0x%02X - ESR = 0x%x", APICGetID(), error);
      break;
   case 2:
      SysAlert("APICID 0x%02X - ESR = 0x%x", APICGetID(), error);
      ASSERT(FALSE); // XXX
      break;
   default:
      NOT_REACHED();
   }
}


/*
 *----------------------------------------------------------------------
 *
 * APICIPIIntHandler --
 *
 *      Handle an IPI interrupt
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
APICIPIIntHandler(UNUSED_PARAM(void *clientData), uint32 vector)
{
}


/*
 *----------------------------------------------------------------------
 *
 * APIC_GetBaseMA --
 *
 *      Return the base machine address for the APIC.
 *
 * Results:
 *      The base machine address for the APIC.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
MA
APIC_GetBaseMA(void)
{
   if (apic == NULL) {
      return 0;
   } else {
      return apic->baseAddr;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * APICSetupFastTimer --
 *
 *      Setup the timer on the apic to call us back for performance
 *      stuff.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

// APIC timer defaults to 1ms period
#define FASTTIMER_HZ	(1000)

static Bool
APICSetupFastTimer(void)
{
   Bool registered;

   apicHostTIMERLVT = apic->reg[APICR_TIMERLVT][0];
   ASSERT_NOT_IMPLEMENTED(apicHostTIMERLVT & APIC_VTE_MASK);

   Log("using 0x%x for APIC timer", IDT_APICTIMER_VECTOR);

   registered = IDT_VectorAddHandler(IDT_APICTIMER_VECTOR,
			APICTimerIntHandler, NULL, FALSE, "timer", 0);
   if (! registered) {
     return FALSE;
   }

   APICEnableFastTimer();
   return TRUE;
}

static void
APICEnableFastTimer(void)
{
   apic->reg[APICR_DIVIDER][0] = APIC_DIVIDER_BY_1;
   apic->reg[APICR_TIMERLVT][0] = APIC_VTE_TIMERMODE | APIC_VTE_MODE_FIXED | IDT_APICTIMER_VECTOR;
   apic->reg[APICR_INITCNT][0] = (busHzEstimate / FASTTIMER_HZ);
}   

/*
 *----------------------------------------------------------------------
 *
 * APIC_SetTimer --
 *
 *      Set the initial countdown timer on the local APIC to "initial"
 *	bus cycles.  If "current" is non-NULL, it is set to the value
 *      of the current countdown timer, in bus cycles.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Modifies local APIC registers, timer interrupt rate.
 *
 *----------------------------------------------------------------------
 */
void
APIC_SetTimer(uint32 initial, uint32 *current)
{
   // save bus cycles left in current countdown
   if (current != NULL) {
      *current = apic->reg[APICR_CURCNT][0];
   }

   // start new countdown
   apic->reg[APICR_INITCNT][0] = initial;

}

/*
 *----------------------------------------------------------------------
 *
 * APICTimerIntHandler --
 *
 *      Interrupt callback for APIC timer interrupts.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
APICTimerIntHandler(UNUSED_PARAM(void *clientData), uint32 vector)
{
   myPRDA.currentTicks++;

#if	0
   {
      // debugging 
      static const int logPeriod = ((1 << 13) - 1);
      static uint32 intrCount[MAX_PCPUS];
      int pcpu = MY_PCPU;
      
      if ((intrCount[pcpu]++ & logPeriod) == 0) {
         LOG(0, "pcpu=%d, count=%u", pcpu, intrCount[pcpu]);
      }
   }
#endif

   // local timer interrupts
   Timer_Interrupt();

   // update stats
   STAT_INC(VMNIX_STAT_TOTALTIMER);
}

/*
 *----------------------------------------------------------------------
 *
 * APIC_SelfInterrupt --
 *
 *      Interrupt this processor with the given vector
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      An interrupt is scheduled for this processor.
 *
 *----------------------------------------------------------------------
 */
void
APIC_SelfInterrupt(uint32 vector)
{
   uint32 mode;

   mode = APICMakeIPIMode(vector, APIC_DELMODE_FIXED,
				APIC_DESTMODE_PHYS, APIC_POLARITY_LOW,
				APIC_TRIGGER_EDGE, APIC_DEST_LOCAL);
   APICSendIPI(0, mode);
}

/*
 *----------------------------------------------------------------------
 *
 * APIC_Dump
 *
 * 	Output the state of the local APIC to the log or to a proc node
 * 	if buffer is not NULL
 *
 * Results:
 * 	None.
 *
 * Side effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
void
APIC_Dump(char *buffer, int *len)
{
   int i;
   uint32 reg;

   if (buffer) {
      Proc_Printf(buffer, len, "APIC interrupt state:\n");
   }

   for (i = 0; i < 8; i++) {
      reg = apic->reg[APICR_IRR+i][0];
      if (reg) {
	 if (buffer) {
	    Proc_Printf(buffer, len, "IRR[%d] = 0x%08x\n", i, reg);
	 } else {
	    Log("IRR[%d] = 0x%08x", i, reg);
	 }
      }
   }
   for (i = 0; i < 8; i++) {
      reg = apic->reg[APICR_ISR+i][0];
      if (reg) {
	 if (buffer) {
	    Proc_Printf(buffer, len, "ISR[%d] = 0x%08x\n", i, reg);
	 } else {
	    Log("ISR[%d] = 0x%08x", i, reg);
	 }
      }
   }
   for (i = 0; i < 8; i++) {
      reg = apic->reg[APICR_TMR+i][0];
      if (reg) {
	 if (buffer) {
	    Proc_Printf(buffer, len, "TMR[%d] = 0x%08x\n", i, reg);
	 } else {
	    Log("TMR[%d] = 0x%08x", i, reg);
	 }
      }
   }

}

/*
 *----------------------------------------------------------------------
 *
 * APIC_GetInServiceVector --
 *
 * 	Get the currently in service vector
 *
 * Results:
 * 	TRUE if a vector is in service
 *
 * Side Effects:
 * 	None
 *
 *----------------------------------------------------------------------
 */
Bool
APIC_GetInServiceVector(uint32 *vector)
{
#define NUM_ISR 8
   uint32 ISR[NUM_ISR];
   int i, j;
   Bool multiple = FALSE;
   uint32 eflags;


   /*
    * Get a self consistent ISR from the APIC.
    */
   SAVE_FLAGS(eflags);
   if (eflags & EFLAGS_IF) {
      CLEAR_INTERRUPTS();
   }
   for (i = 0; i < NUM_ISR; i++) {
      ISR[i] = apic->reg[APICR_ISR+i][0];
   }
   if (eflags & EFLAGS_IF) {
      RESTORE_FLAGS(eflags);
   }

   /*
    * Parse the ISR for the currently in service vector.
    * Highest priority is highest bit.
    */
   for (i = NUM_ISR-1; i >= 0; i--) {
      if (ISR[i]) {
	 for (j = i-1; j >= 0; j--) {
	    if (ISR[j]) {
	       multiple = TRUE;
	       break;
	    }
	 }
	 *vector = ((i+1)*sizeof(ISR[0])*8) - 1;
	 while ((ISR[i] & 0x80000000) == 0) {
	    (*vector)--;
	    ISR[i] <<= 1;
	 }
	 if (ISR[i] != 0x80000000) {
	    multiple = TRUE;
	 }
	 break;
      }
   }

   if (multiple) {
      SysAlert("Several interrupts are in service at once");
      APIC_Dump(NULL, NULL);
   }

   return (i >= 0);
}


/*
 *----------------------------------------------------------------------
 *
 * APIC_IsPendingVector --
 *
 * 	Check if a vector is waiting to be serviced
 *
 * Results:
 * 	TRUE if pending
 *
 * Side Effects:
 * 	None
 *
 *----------------------------------------------------------------------
 */
Bool
APIC_IsPendingVector(uint32 vector)
{
   int IRRNum = vector / 0x20;
   int IRRBit = 1 << (vector % 0x20);

   /*
    * Check the IRR bit from APIC.
    */
   return (apic->reg[APICR_IRR+IRRNum][0] & IRRBit) != 0;
}


#ifdef VMX86_DEBUG
/*
 *----------------------------------------------------------------------
 *
 * APIC_CheckAckVector --
 *
 *      Check that vector to be acknowledged is in service
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
void 
APIC_CheckAckVector(uint32 vector)
{
   Bool inService;
   uint32 isrVector;

   inService = APIC_GetInServiceVector(&isrVector);

   if (!inService) { // harmless
      if ((MPS_Signature == IBM_X440) || (MPS_Signature == IBM_RELENTLESS)) {
	 // only machines so far exhibiting this weird behavior (PR 23397)
	 if (vector == IDT_APICTIMER_VECTOR) {
	    // most commonly affected vector
	    Log("Ack'ing 0x%x (TIMER) but nothing in service", vector);
	 } else {
	    SysAlert("Ack'ing 0x%x but nothing in service", vector);
	 }
      } else {
	 Panic("Ack'ing 0x%x but nothing in service", vector);
      }
   } else if (vector != isrVector) {
      Panic("Ack'ing 0x%x but 0x%x is in service", vector, isrVector);
   }
}
#endif

/*
 *----------------------------------------------------------------------
 *
 * APIC_HzEstimate --
 *
 *      Measure the speed of the CPU clock (via the TSC) and the
 *      system bus clock (via the local APIC timer), using the PIT
 *      timer as a reference.  The estimate seems to be good to about
 *      +/- 200 Hz.  We measure both at the same time so that we can
 *      be sure to compute the clock multiplier accurately.  See PR
 *      34866.
 *
 * Results:
 *      CPU speed in *cpuHz and bus speed in *busHz.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
#define APIC_LARGE_COUNT (1<<30)
void
APIC_HzEstimate(volatile uint64 *cpuHz, volatile uint64 *busHz)
{
   uint32 endAPICCount, oldDiv;
   uint64 beginTSC, endTSC;

   HZ_ESTIMATE_BEGIN(4);

   // read the apic divider, and set the divider to be 1
   oldDiv = apic->reg[APICR_DIVIDER][0];
   apic->reg[APICR_DIVIDER][0] = APIC_DIVIDER_BY_1;

   // set the apic timer to a large value
   apic->reg[APICR_INITCNT][0] = APIC_LARGE_COUNT;

   // read the TSC
   beginTSC = RDTSC();

   HZ_ESTIMATE_DELAY;

   // read the TSC again for the end count
   endTSC = RDTSC();

   // read the APIC counter again for the end count
   endAPICCount = apic->reg[APICR_CURCNT][0];

   // reset the divider
   apic->reg[APICR_DIVIDER][0] = oldDiv;

   *cpuHz = HZ_ESTIMATE_COMPUTE(endTSC - beginTSC);
   *busHz = HZ_ESTIMATE_COMPUTE(APIC_LARGE_COUNT - endAPICCount);

   HZ_ESTIMATE_END;
}


/*
 *----------------------------------------------------------------------
 *
 * APIC_GetDestInfo
 *
 * 	Get destination info for IPI based on pcpu
 *
 * Results:
 *	FALSE if pcpu is incorrect, TRUE otherwise.
 *
 * Side Effects:
 * 	*dest and *destMode are filled.
 *
 *----------------------------------------------------------------------
 */
Bool
APIC_GetDestInfo(PCPU pcpuNum, uint32 *dest, uint32 *destMode)
{
   int apicID = APIC_FindID(pcpuNum);

   if (apicID == -1) {
      return FALSE;
   }

   *dest = APICGetDest(apicID);
   *destMode = apic->destMode;

   return TRUE;
}

/*
 *----------------------------------------------------------------------
 *
 * APIC_PerfCtrSetNMI
 *
 * 	Set the interrupt mode to NMI for performance counters
 *
 * Results:
 * 	None
 *
 * Side Effects:
 * 	Performance counters LVT is set to NMI mode and enabled.
 *
 *----------------------------------------------------------------------
 */
void
APIC_PerfCtrSetNMI(void)
{
   apic->reg[APICR_PCLVT][0] = APIC_VTE_MODE_NMI;
}

/*
 *----------------------------------------------------------------------
 *
 * APIC_PerfCtrMask
 *
 * 	Disable interrupt generation for performance counters
 * 	NOTE: This can be called with COS address space so no kseg/prda
 * 	available
 *
 * Results:
 * 	TRUE if the interrupt generation used to be enabled
 *
 * Side Effects:
 * 	Performance counters LVT is masked.
 *
 *----------------------------------------------------------------------
 */
Bool
APIC_PerfCtrMask(void)
{
   Bool enabled;
   volatile uint32 forceRead;

   if ((apic == NULL) || (apic->reg == NULL)) {
      return FALSE;
   }
   enabled = !(apic->reg[APICR_PCLVT][0] & APIC_VTE_MASK);
   apic->reg[APICR_PCLVT][0] |= APIC_VTE_MASK;

   forceRead = apic->reg[APICR_PCLVT][0];

   return enabled;
}

/*
 *----------------------------------------------------------------------
 *
 * APIC_PerfCtrUnmask
 *
 * 	Enable interrupt generation for performance counters
 * 	NOTE: This can be called with COS address space so no kseg/prda
 * 	available
 *
 * Results:
 * 	None
 *
 * Side Effects:
 * 	Performance counters LVT is unmasked.
 *
 *----------------------------------------------------------------------
 */
void
APIC_PerfCtrUnmask(void)
{
   if ((apic == NULL) || (apic->reg == NULL)) {
      return;
   }
   apic->reg[APICR_PCLVT][0] &= ~APIC_VTE_MASK;
}


/*
 *----------------------------------------------------------------------
 *
 * APIC_GetCurPCPUApicID
 *
 *      Get the apicID for the current processor, sets 'apicID' to the
 *    local apic id on success.
 *
 * Results:
 *    VMK_OK on success, error code otherwise.
 *
 * Side Effects:
 *      local apic is enabled, if required.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
APIC_GetCurPCPUApicID(int *apicID)
{
   uint32 apicMSR;
   uint32 apicIDMask;
   apicIDMask = (cpuType==CPUTYPE_INTEL_PENTIUM4)? XAPIC_ID_MASK:APIC_ID_BITS;
   struct APIC info;
   uint32 version;

   ASM("rdmsr" :
       "=a" (apicMSR) :
       "c" (APIC_BASE_MSR) :
       "edx");

   if (!(apicMSR & APIC_MSR_ENABLED)) {
      Log("APIC is disabled...enabling");
      apicMSR |= APIC_MSR_ENABLED;
      ASM("wrmsr" :
          :
          "a" (apicMSR),
          "d" (0),
          "c" (APIC_BASE_MSR));
   } 

   info.reg = KVMap_MapMPN(MA_2_MPN(apicMSR & APIC_MSR_BASEMASK), TLB_UNCACHED);
   version = (info.reg[APICR_VERSION][0] & 0xFF);
   if ((version & 0xF0) != 0x10) {
      Warning("unsupported version found: 0x%x", version);
      KVMap_FreePages(info.reg);
      return VMK_UNSUPPORTED_CPU;
   }

   *apicID = (info.reg[APICR_ID][0] & apicIDMask) >> APIC_ID_SHIFT;
   KVMap_FreePages(info.reg);
   return VMK_OK;
}
