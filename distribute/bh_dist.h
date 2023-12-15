/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * bh_dist.h --
 *
 *	Bottom half handlers.
 */


#ifndef _BH_DIST_H
#define _BH_DIST_H

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMK_MODULE
#define INCLUDE_ALLOW_DISTRIBUTE
#include "includeCheck.h"

typedef void (*BH_Handler)(void *clientData);

uint32 BH_Register(BH_Handler handler, void *clientData);
void BH_SetLocalPCPU(uint32 bhNum);
void BH_Check(Bool canReschedule);

EXTERN void BH_SetLinuxBHList(void *data);  //XXX perf crit?
EXTERN void *BH_GetLinuxBHList(void);
#endif
