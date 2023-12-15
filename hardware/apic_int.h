/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * apic_int.h --
 *
 *	This is the internal header file for apic module.
 */

#ifndef _APIC_INT_H
#define _APIC_INT_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "apic.h"


EXTERN VMK_ReturnStatus APIC_Init(ICType hostICType, VMnix_ConfigOptions *vmnixOptions, VMnix_SharedData *sharedData);
EXTERN VMK_ReturnStatus APIC_SlaveInit(void);
EXTERN void APIC_RestoreHostSetup(void);
EXTERN Bool APIC_KickAP(int apicID, uint32 eip);
EXTERN Bool APIC_GetDestInfo(PCPU pcpuNum, uint32 *dest, uint32 *destMode);
EXTERN void APIC_SetTimer(uint32 inital, uint32 *currentVal);
EXTERN void APIC_Dump(char *buffer, int *len);
EXTERN Bool APIC_GetInServiceVector(uint32 *vector);
EXTERN Bool APIC_IsPendingVector(uint32 vector);
EXTERN void APIC_CheckAckVector(uint32 vector);
EXTERN int APIC_FindID(PCPU pcpuNum);
EXTERN VMK_ReturnStatus APIC_GetCurPCPUApicID(int *apicID);



/*
 * This is the state for the apic module.
 */
typedef struct APIC {
   ApicReg      *reg;
   MA           baseAddr;
   Bool         flatFormat;
   uint32       destMode;
} APICInfo;

EXTERN struct APIC *apic;



/*
 *----------------------------------------------------------------------
 *
 * APIC_AckVector --
 *
 *      Acknowledge the most recent interrupt
 *
 * Results:
 *      None
 *
 * Side effects:
 *      APIC hardware gets EOI.
 *
 *----------------------------------------------------------------------
 */
static INLINE void 
APIC_AckVector(uint32 vector)
{
#ifdef VMX86_DEBUG
   APIC_CheckAckVector(vector);
#endif

   apic->reg[APICR_EOI][0] = 0;
}
   
#endif
