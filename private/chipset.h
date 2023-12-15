/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * chipset.h --
 *
 *	This is the header file for chipset module.
 */


#ifndef _CHIPSET_H
#define _CHIPSET_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "vmnix_if.h"
#include "vm_basic_types.h"
#include "vmkernel_dist.h"

typedef struct Chipset_ICFunctions {
   void (*maskAndAckVector)(uint32 vector);
   void (*unmaskVector)(uint32 vector);
   void (*maskVector)(uint32 vector);
   void (*ackVector)(uint32 vector);
   Bool (*getInServiceLocally)(uint32 *vector);
   void (*restoreHostSetup)(void);
   Bool (*steerVector)(uint32 vector, PCPU pcpuNum);
   void (*maskAll)(void);
   void (*dump)(char *buffer, int *len);
   Bool (*posted)(uint32 vector);
   Bool (*pendingLocally)(uint32 vector);
   Bool (*spurious)(uint32 vector);
   Bool (*goodTrigger)(uint32 vector, Bool edge);
} Chipset_ICFunctions;


extern Chipset_ICFunctions *Chipset_ICFuncs;
extern Bool chipsetInitialized;

struct VMnix_AcpiInfo;
EXTERN VMK_ReturnStatus
Chipset_Init(VMnix_Init *vmnixInit, VMnix_Info *vmnixInfo, VMnix_ConfigOptions *vmnixOptions, 
             VMnix_SharedData *sharedData, struct VMnix_AcpiInfo *acpiInfo);
EXTERN void
Chipset_LateInit(void);

EXTERN INLINE void
Chipset_MaskAndAckVector(uint32 vector)
{
   Chipset_ICFuncs->maskAndAckVector(vector);
}

EXTERN INLINE void
Chipset_UnmaskVector(uint32 vector)
{
   Chipset_ICFuncs->unmaskVector(vector);
}

EXTERN INLINE void
Chipset_MaskVector(uint32 vector)
{
   Chipset_ICFuncs->maskVector(vector);
}

EXTERN INLINE void
Chipset_AckVector(uint32 vector)
{
   Chipset_ICFuncs->ackVector(vector);
}

EXTERN INLINE Bool
Chipset_GetInServiceLocally(uint32 *vector)
{
   return Chipset_ICFuncs->getInServiceLocally(vector);
}


EXTERN INLINE void
Chipset_RestoreHostSetup(void)
{
   /*
    * This function may get called even if chipset is not properly setup if
    * there are errors during vmkernel load, so need to first check if
    * chipset functions are setup.
    */
   if ((Chipset_ICFuncs != NULL) && chipsetInitialized) {
      Chipset_ICFuncs->restoreHostSetup();
   }
}

EXTERN INLINE Bool
Chipset_SteerVector(uint32 vector, PCPU pcpuNum)
{
   return Chipset_ICFuncs->steerVector(vector, pcpuNum);
}

EXTERN INLINE void
Chipset_MaskAll(void)
{
   Chipset_ICFuncs->maskAll();
}

EXTERN INLINE void
Chipset_Dump(void)
{
   Chipset_ICFuncs->dump(NULL, NULL);
}

EXTERN INLINE Bool
Chipset_Posted(uint32 vector)
{
   return Chipset_ICFuncs->posted(vector);
}

EXTERN INLINE Bool
Chipset_InServiceLocally(uint32 vector)
{
   uint32 isrVector;
   Bool inService;

   inService = Chipset_GetInServiceLocally(&isrVector);
   if (!inService) {
      return FALSE;
   } else {
      return vector == isrVector;
   }
}

EXTERN INLINE Bool
Chipset_PendingLocally(uint32 vector)
{
   return Chipset_ICFuncs->pendingLocally(vector);
}

EXTERN INLINE Bool
Chipset_Spurious(uint32 vector)
{
   return Chipset_ICFuncs->spurious(vector);
}

EXTERN INLINE Bool
Chipset_GoodTrigger(uint32 vector, Bool edge)
{
   return Chipset_ICFuncs->goodTrigger(vector, edge);
}

#endif
