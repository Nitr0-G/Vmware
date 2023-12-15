/* **********************************************************
 * Copyright 2000 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

#ifndef _VMKSTATS_H_
#define _VMKSTATS_H_

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"
#include "nmi.h"

#ifdef VMX86_NMIS_IN_MONITOR
#define VMX86_ENABLE_VMKSTATS 1
#endif

struct World_Handle;

/*
 * Operations
 */
extern void VMKStats_Init(void);
extern void VMKStats_LateInit(void);
extern void VMKStats_Sample(NMIContext *task);
extern void VMKStats_ModuleLoaded(char *modName,
                                  uint64 imageId,
                                  uint32 baseAddr,
                                  uint32 size,
                                  uint32 initFunc,
                                  uint32 cleanupFunc);
extern void VMKStats_ModuleUnloaded(char *modName);
extern void VMKStats_BinaryTranslation(void);
extern void VMKStats_DirectExecution(void);
extern void VMKStats_COS(void);

#endif
