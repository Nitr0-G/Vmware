/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. 
 * -- VMware Confidential
 * **********************************************************/

/*
 * reliability.h --
 *
 *   This is the header for the reliability module.
 *
 */

#ifndef _RELIABILITY_H
#define _RELIABILITY_H
#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

EXTERN void Reliability_Init(void);
EXTERN VMK_ReturnStatus Reliability_WorldInit(World_Handle *, World_InitArgs *);
EXTERN void Reliability_WorldCleanup(World_Handle *);

#endif
