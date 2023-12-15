/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * smp.h --
 *
 *	Host SMP specific functions.
 */

#ifndef _SMP_H
#define _SMP_H

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_USERLEVEL

#include "includeCheck.h"


EXTERN VMK_ReturnStatus SMP_Init(VMnix_Init *vmnixInit, 
                     VMnix_ConfigOptions *vmnixOptions,
                     VMnix_AcpiInfo *acpiInfo);
EXTERN TSCRelCycles SMP_BootAPs(VMnix_Init *vmnixInit);
EXTERN void SMP_StartAPs(void);
EXTERN void SMP_StopAPs(void);
EXTERN void SMP_SlaveHaltCheck(PCPU pcpuNum);


// hyperthreading support

// The PCPU type refers to a logical processor in a hyperthreaded system,
// while the term "package" refers to the physical chip that may support
// two logical processors. 
#define SMP_MAX_CPUS_PER_PACKAGE (2)

typedef struct {
   Bool hyperTwins;
   int numLogical;
   int baseApicID;
   PCPU logicalCpus[SMP_MAX_CPUS_PER_PACKAGE];
   int  apicId[SMP_MAX_CPUS_PER_PACKAGE];
} SMP_PackageInfo;

struct SMP_HTInfo {
   Bool htEnabled;
   uint32 numPackages;
   SMP_PackageInfo packages[MAX_PCPUS];
   PCPU cpuToPkgMap[MAX_PCPUS];
   uint8 logicalPerPackage;
};

EXTERN struct SMP_HTInfo hyperthreading;

EXTERN SMP_PackageInfo* SMP_GetPackageInfo(PCPU p);
EXTERN PCPU SMP_GetPartnerPCPU(PCPU p);
EXTERN uint8 SMP_GetHTThreadNum(PCPU p);
EXTERN uint8 SMP_LogicalCPUPerPackage(void);


/*
 *-----------------------------------------------------------------------------
 *
 * SMP_GetPackageNum --
 *
 *     Returns the PCPU number of the first hypertwin on "p"'s package
 *     which can be used as a unique identifier for this package.
 *
 * Results:
 *     Returns the PCPU number of the first hypertwin on "p"'s package
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE uint32 SMP_GetPackageNum(PCPU p) {
   return SMP_GetPackageInfo(p)->logicalCpus[0];
}

/*
 *-----------------------------------------------------------------------------
 *
 * SMP_HTEnabled --
 *
 *     Returns TRUE iff hyperthreading is active
 *
 * Results:
 *     Returns TRUE iff hyperthreading is active
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE Bool 
SMP_HTEnabled(void) {
   return hyperthreading.htEnabled;
}

#define MY_PARTNER_PRDA (prdas[SMP_GetPartnerPCPU(MY_PCPU)])
#define MY_PARTNER_PCPU (SMP_GetPartnerPCPU(MY_PCPU))

#endif
