/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

#ifndef _VMK_STUBS_H
#define _VMK_STUBS_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMMEXT
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMCORE
#include "includeCheck.h"

#include "action_ext.h"
#include "return_status.h"
#include "rateconv.h"
#include "vmk_basic_types.h"
#include "x86perfctr.h"
#include "stats_shared.h"

#define VMMVMK_MAX_STATS  200

/* XXX This should be deleted and SharedArea_Alloc should be used...*/
typedef struct VMK_SharedData {
   int sizeofSharedData; // to verify VMM/VMK agree on sizeof this struct
   Action_Info actions;
   StatsEntry monitorStats[VMMVMK_MAX_STATS];
   uint32 statsTotalBusyTicks;
   uint32 statsTotalWaitTicks;
   uint32 statsTicks;    //used by the monitor to see if time has passed.
   uint32 shadowDR[8];   // to reduce save/restore of DRs
   SysenterState vmm32Sysenter;
   SysenterState vmm64Sysenter;
   RateConv_Params pseudoTSCConv;
   uint8 htThreadNum; // only valid when VMM NMI profiling is on
} VMK_SharedData;

typedef void (*VMK_CallFunc)(uint32 function, va_list args, 
                             VMK_ReturnStatus *status);

void VMKCall(uint32 function, va_list args, VMK_ReturnStatus *status);

typedef struct VMK_MonitorInitArgs {
   VMK_CallFunc           call  __attribute__((regparm(0)));
   VA                     stackTop;
   VM_PAE_PTE             vmkIDTPTE;
   DTR32                  vmkIDTR;
   MA                     vmkCR3;
   World_ID               worldID;
} VMK_MonitorInitArgs;

/* 
 * Version checking for the vmm<->vmkernel interface.
 * If the major number is different the vmm will fail to load
 * If minor is different, a warning will be printed.
 * Please change the major version if your change is going to break
 * backward compatibility.
 * Change the minor version if your change is compatible, but perhaps
 * you want to dynamically check the minor version and do different things
 */
#define MAKE_VMMVMK_VERSION(major,minor) (((major) << 16) | (minor))
#define VMMVMK_VERSION_MAJOR(version) ((version) >> 16)
#define VMMVMK_VERSION_MINOR(version) ((version) & 0xffff)
#define VMMVMK_VERSION MAKE_VMMVMK_VERSION(45,0)

/*
 * magic values used to verify that the number of parameters passed
 * to a vmm->vmk call on the monitor side match the number of parameters
 * expected by the vmkernel side.  see vmkernel.h for more details.
 *
 * The current vmm<->vmk interface version is part of the before magic.  This
 * makes version checking far more robust.  [ie one can re-order the vmkcalls
 * at will, and still get nice, helpful version mismatch errors].
 */
#define  VMMVMK_BEFORE_ARG_MAGIC  (0x12340000 | (VMMVMK_VERSION_MAJOR(VMMVMK_VERSION) & 0xffff))
#define  VMMVMK_AFTER_ARG_MAGIC 0x87654321

#endif
