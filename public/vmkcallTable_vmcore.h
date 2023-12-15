/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * vmkcallTable_vmcore.h --
 *
 *	List of vmkernel calls made by the monitor. This file may be included
 *      more than once with VMKCALL defined differently each time.  
 *      Consider updating VMMVMK_VERSION in vmk_stubs.h if you add entries. 
 *      Also, consider updating vmview since each vmkcall gets a kstat.  
 *    
 *      It is required that the size of this table match the #define in
 *      vmkcalls.h to allow modules to create combined tables from these 
 *      definitions.  When the unused slots are taken, it will be necessary to
 *      bump up VMK_DEV_MIN_FUNCTION_ID in vmkcalls.h.
 */

#define INCLUDE_ALLOW_VMCORE
#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

/*
 * Usage: VMKCALL(<vmkcall name>, <interrupt state>, <stats accounting>, <explanation>)
 */
VMKCALL(VMK_NULL,                     VMK_IF_INTERRUPTS_DONT_KNOW,       NULL,                     VMKCall, "") // must be first
VMKCALL(VMK_CHECK_VERSION,            VMK_IF_INTERRUPTS_OFF,             VMKCheckVersion,          VMKCall, "") // must be second
VMKCALL(VMK_RPC_CONNECT,              VMK_IF_INTERRUPTS_OFF,             World_Connect,            VMKCall, "") 
VMKCALL(VMK_RPC_CALL,                 VMK_IF_INTERRUPTS_DONT_KNOW,       DoRPCCall,                VMKCall, "") 
VMKCALL(VMK_STOP_NMIS,                VMK_IF_INTERRUPTS_OFF,             StopNMIs,                 VMKCall, "") 
VMKCALL(VMK_START_NMIS,               VMK_IF_INTERRUPTS_OFF,             StartNMIs,                VMKCall, "") 
VMKCALL(VMK_EXIT,                     VMK_IF_INTERRUPTS_DONT_KNOW,       VMKExit,                  VMKCall, "") 
VMKCALL(VMK_ALLOC_ANON_PAGE,          VMK_IF_INTERRUPTS_DONT_KNOW,       Alloc_AnonPage,           VMKCall, "") 
VMKCALL(VMK_VCPU_HALT,                VMK_IF_INTERRUPTS_ON,              CpuSched_VcpuHalt,        Halt,    "") 
VMKCALL(VMK_TIMER_INFO,               VMK_IF_INTERRUPTS_ON,              Timer_Info,               VMKCall, "") 
VMKCALL(VMK_GET_SERIAL_PORT,          VMK_IF_INTERRUPTS_ON,              Serial_GetPort,           VMKCall, "") 
VMKCALL(VMK_COW_SHARE_PAGES,          VMK_IF_INTERRUPTS_ON,              Alloc_COWSharePages,      VMKCall, "") 
VMKCALL(VMK_COW_COPY_PAGE,            VMK_IF_INTERRUPTS_ON,              Alloc_COWCopyPage,        VMKCall, "") 
VMKCALL(VMK_MODE_LOCK_PAGE,           VMK_IF_INTERRUPTS_DONT_KNOW,       Alloc_ModeLockPage,       VMKCall, "") 
VMKCALL(VMK_COW_P2M_UPDATE,           VMK_IF_INTERRUPTS_ON,              Alloc_COWP2MUpdateGet,   VMKCall, "") 
VMKCALL(VMK_COW_HINT_UPDATES,         VMK_IF_INTERRUPTS_ON,              Alloc_COWGetHintUpdates,  VMKCall, "") 
VMKCALL(VMK_COW_CHECK_PAGES,          VMK_IF_INTERRUPTS_ON,              Alloc_COWCheckPages,      VMKCall, "") 
VMKCALL(VMK_COW_REMOVE_HINT,          VMK_IF_INTERRUPTS_ON,              Alloc_COWRemoveHint,      VMKCall, "") 
VMKCALL(VMK_VECTOR_IS_FREE,           VMK_IF_INTERRUPTS_OFF,             IDT_VMKVectorIsFree,      VMKCall, "") 
VMKCALL(VMK_SWAPOUT_PAGES,            VMK_IF_INTERRUPTS_ON,              Swap_SwapOutPages,        VMKCall, "") 
VMKCALL(VMK_MEM_VMM_STARTED,          VMK_IF_INTERRUPTS_ON,              MemSched_MonitorStarted,  VMKCall, "") 
VMKCALL(VMK_REMAP_BATCH_PAGES,        VMK_IF_INTERRUPTS_ON,              Alloc_RemapBatchPages,    VMKCall, "") 
VMKCALL(VMK_REMAP_BATCH_PICKUP,       VMK_IF_INTERRUPTS_ON,              Alloc_RemapBatchPickup,   VMKCall, "") 
VMKCALL(VMK_NUMA_GET_SYS_INFO,        VMK_IF_INTERRUPTS_DONT_KNOW,       NUMA_GetSystemInfo,       VMKCall, "") 
VMKCALL(VMK_NUMA_GET_NODE_INFO,       VMK_IF_INTERRUPTS_DONT_KNOW,       NUMA_GetNodeInfo,         VMKCall, "") 
VMKCALL(VMK_INIT,                     VMK_IF_INTERRUPTS_OFF,             VMKInit,                  VMKCall, "") 
VMKCALL(VMK_INIT_ACTIONS,             VMK_IF_INTERRUPTS_DONT_KNOW,       Action_InitVMKActions,    VMKCall, "") 
VMKCALL(VMK_ENABLE_ACTIONS,           VMK_IF_INTERRUPTS_DONT_KNOW,       Action_EnableVMKActions,  VMKCall, "") 
VMKCALL(VMK_DISABLE_ACTIONS,          VMK_IF_INTERRUPTS_DONT_KNOW,       Action_DisableVMKActions, VMKCall, "")
VMKCALL(VMK_SEMAPHORE_SIGNAL,         VMK_IF_INTERRUPTS_ON,              DoSemaphoreSignal,        VMKCall, "")
VMKCALL(VMK_CAN_BALLOON_PAGE,         VMK_IF_INTERRUPTS_ON,              Alloc_CanBalloonPage,     VMKCall, "")
VMKCALL(VMK_BALLOON_REL_PAGES,        VMK_IF_INTERRUPTS_ON,              Alloc_BalloonReleasePages,VMKCall, "") 
VMKCALL(VMK_GET_INT_INFO,             VMK_IF_INTERRUPTS_DONT_KNOW,       IDT_VMKGetIntInfo,        VMKCall, "") 
VMKCALL(VMK_VMM_UNKNOWN10,            VMK_IF_INTERRUPTS_DONT_KNOW,       NULL,                     VMKCall, "")
VMKCALL(VMK_SEMAPHORE_WAIT,           VMK_IF_INTERRUPTS_ON,              DoSemaphoreWait,          VMKCall, "") 
VMKCALL(VMK_CREATE_CHANNEL,           VMK_IF_INTERRUPTS_ON,              Action_CreateChannel,     VMKCall, "") 
VMKCALL(VMK_COW_P2M_UPDATE_DONE,      VMK_IF_INTERRUPTS_ON,              Alloc_COWP2MUpdateDone,  VMKCall, "") 
VMKCALL(VMK_ALLOC_RELEASE_ANON_PAGE,  VMK_IF_INTERRUPTS_DONT_KNOW,       Alloc_ReleaseAnonPage,    VMKCall, "") 
VMKCALL(VMK_ACTION_NOTIFY_VCPU,       VMK_IF_INTERRUPTS_DONT_KNOW,       CpuSched_ActionNotifyVcpu,VMKCall, "") 
VMKCALL(VMK_VMM_SERIAL_LOGGING,       VMK_IF_INTERRUPTS_DONT_KNOW,       Log_VMMLog,               VMKCall, "") 
VMKCALL(VMK_PANIC,                    VMK_IF_INTERRUPTS_DONT_KNOW,       World_VMMPanic,           VMKCall, "") 
VMKCALL(VMK_NOP,                      VMK_IF_INTERRUPTS_DONT_KNOW,       Init_NOPCall,             VMKCall, "")
VMKCALL(VMK_SETUP_DF,                 VMK_IF_INTERRUPTS_OFF,             IDT_SetupVMMDFHandler,    VMKCall, "") 
VMKCALL(VMK_UPDATE_ANON_MEM_RESERVED, VMK_IF_INTERRUPTS_DONT_KNOW,       Alloc_UpdateAnonReserved,    VMKCall, "") 
VMKCALL(VMK_SWAP_NUM_PAGES,           VMK_IF_INTERRUPTS_ON,              Swap_GetNumPagesToSwap,   VMKCall, "") 
VMKCALL(VMK_VMM_INTORMCE,             VMK_IF_INTERRUPTS_OFF,             IDT_VMMIntOrMCE,          VMKCall, "") 
VMKCALL(VMK_LOOKUP_MPN,               VMK_IF_INTERRUPTS_DONT_KNOW,       Alloc_LookupMPNFromWorld, VMKCall, "") 
VMKCALL(VMK_VMM_AS_INIT,              VMK_IF_INTERRUPTS_OFF,             World_VmmASInit,          VMKCall, "") 
VMKCALL(VMK_GET_PERFCTR_CONFIG,       VMK_IF_INTERRUPTS_OFF,             GetPerfCtrConfig,             VMKCall, "")
VMKCALL(VMK_VMM_MAINMEM_INIT,         VMK_IF_INTERRUPTS_DONT_KNOW,       World_InitMainMem,        VMKCall, "")
/*
 * USE VMK_VMM_UNKNOWN ENTRIES TO ADD NEW VMKCalls's
 * 
 * All the VMK_VMM_UNKNOWN calls are unclaimed and intended for future
 * expansion of the table without breaking vmcore/vmkernel
 * compatibility.  When these entries are exhausted, it is necessary
 * to add another set and break compatiblity.  These slots need to be
 * explicitly defined to avoid shifting VMKCALL id's and still
 * allowing stats code to directly index into a table of vmkcall.
 *
 * Please use the VMK_VMM_UNKNOWN10 above (and delete this comment)
 * before using those below. 
 */
VMKCALL(VMK_VMM_UNKNOWN9,             VMK_IF_INTERRUPTS_DONT_KNOW,       NULL,                     VMKCall, "")
VMKCALL(VMK_VMM_UNKNOWN8,             VMK_IF_INTERRUPTS_DONT_KNOW,       NULL,                     VMKCall, "")
VMKCALL(VMK_VMM_UNKNOWN7,             VMK_IF_INTERRUPTS_DONT_KNOW,       NULL,                     VMKCall, "")
VMKCALL(VMK_VMM_UNKNOWN6,             VMK_IF_INTERRUPTS_DONT_KNOW,       NULL,                     VMKCall, "")
VMKCALL(VMK_VMM_UNKNOWN5,             VMK_IF_INTERRUPTS_DONT_KNOW,       NULL,                     VMKCall, "")
VMKCALL(VMK_VMM_UNKNOWN4,             VMK_IF_INTERRUPTS_DONT_KNOW,       NULL,                     VMKCall, "")
VMKCALL(VMK_VMM_UNKNOWN3,             VMK_IF_INTERRUPTS_DONT_KNOW,       NULL,                     VMKCall, "")
VMKCALL(VMK_VMM_UNKNOWN2,             VMK_IF_INTERRUPTS_DONT_KNOW,       NULL,                     VMKCall, "")
VMKCALL(VMK_VMM_UNKNOWN1,             VMK_IF_INTERRUPTS_DONT_KNOW,       NULL,                     VMKCall, "")
VMKCALL(VMK_VMM_UNKNOWN0,             VMK_IF_INTERRUPTS_DONT_KNOW,       NULL,                     VMKCall, "")

// must be last
VMKCALL(VMK_VMM_MAX_FUNCTION_ID,      VMK_IF_INTERRUPTS_DONT_KNOW,       NULL,                     VMKCall, "")
