/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * vmkcallTable_public.h --
 *
 *	List of vmkernel calls made from outside vmcore.
 *      This file may be included more than once with VMKCALL macro defined
 *      differently each time.  If you add more entrees here, don't forget to 
 *      update vmview since each vmkcall gets a kstat.  Consider updating 
 *      VMMVMK_VERSION in vmk_stubs.h if you add entries.
 *
 *      Note that ordering is important with respect to vmkcalls_vmm_defs.h.  
 *      To build tables from these definitions, vmkcalls_vmm_defs.h must be 
 *      included first to make sure the indices line up.
 */
#define INCLUDE_ALLOW_VMMEXT
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMCORE
#include "includeCheck.h"

/*
 * Usage: VMKCALL(<vmkcall name>, <interrupt state>, <stats accounting>, <explanation>)
 */

VMKCALL(VMK_SCSI_COMMAND,                   VMK_IF_INTERRUPTS_ON,              VSCSI_ExecuteCommand,     VMKCall, "") 
VMKCALL(VMK_SCSI_GET_DEV_PARAM,             VMK_IF_INTERRUPTS_ON,              VSCSI_GetDeviceParam,     VMKCall, "") 
VMKCALL(VMK_SCSI_CMD_COMPLETE,              VMK_IF_INTERRUPTS_ON,              VSCSI_CmdComplete,        VMKCall, "") 
VMKCALL(VMK_SCSI_WAIT_FOR_CIF,              VMK_IF_INTERRUPTS_ON,              VSCSI_WaitForCIF,         VMKCall, "") 
VMKCALL(VMK_SCSI_ACCUM_SG,                  VMK_IF_INTERRUPTS_ON,              VSCSI_AccumulateSG,       VMKCall, "") 
VMKCALL(VMK_MIGRATE_PRE_COPY,               VMK_IF_INTERRUPTS_ON,              Migrate_PreCopy,          VMKCall, "") 
VMKCALL(VMK_MIGRATE_PRE_COPY_WRITE,         VMK_IF_INTERRUPTS_ON,              Migrate_PreCopyWrite,     VMKCall, "") 
VMKCALL(VMK_MIGRATE_PRE_COPY_DONE,          VMK_IF_INTERRUPTS_ON,              Migrate_PreCopyDone,      VMKCall, "") 
VMKCALL(VMK_MIGRATE_GET_FAILURE,            VMK_IF_INTERRUPTS_ON,              Migrate_GetFailure,       VMKCall, "") 
VMKCALL(VMK_REGISTER_SCSI_DEVICE,           VMK_IF_INTERRUPTS_ON,              VSCSI_RegisterVMMDevice,  VMKCall, "") 
VMKCALL(VMK_MIGRATE_RESTORE_DONE,           VMK_IF_INTERRUPTS_ON,              Migrate_RestoreDone,      VMKCall, "") 
VMKCALL(VMK_MIGRATE_CONTINUE,               VMK_IF_INTERRUPTS_ON,              Migrate_Continue,         VMKCall, "") 
VMKCALL(VMK_PRECOPY_START,                  VMK_IF_INTERRUPTS_ON,              Migrate_PreCopyStart,     VMKCall, "") 
VMKCALL(VMK_CONDUIT_GET_CAPABILITIES,       VMK_IF_INTERRUPTS_ON,              Conduit_GetCapabilities,  VMKCall, "")
VMKCALL(VMK_CONDUIT_SIGNAL_DEV,             VMK_IF_INTERRUPTS_ON,              Conduit_SignalDev,        VMKCall, "")
VMKCALL(VMK_CONDUIT_SEND,                   VMK_IF_INTERRUPTS_ON,              Conduit_VMMTransmit,      VMKCall, "")
VMKCALL(VMK_CONDUIT_LOCK_PAGE,              VMK_IF_INTERRUPTS_ON,              Conduit_LockPage,         VMKCall, "")
VMKCALL(VMK_NET_VMM_PORT_ENABLE_VLANCE,     VMK_IF_INTERRUPTS_ON,              Net_VMMPortEnableVlance,  VMKCall, "") 
VMKCALL(VMK_NET_VMM_VLANCE_TX,              VMK_IF_INTERRUPTS_ON,              Net_VMMVlanceTx,          VMKCall, "")
VMKCALL(VMK_NET_VMM_VLANCE_RXDMA,           VMK_IF_INTERRUPTS_ON,              Net_VMMVlanceRxDMA,       VMKCall, "")
VMKCALL(VMK_NET_VMM_PORT_ENABLE_VMXNET,     VMK_IF_INTERRUPTS_ON,              Net_VMMPortEnableVmxnet,  VMKCall, "") 
VMKCALL(VMK_NET_VMM_PORT_DISABLE,           VMK_IF_INTERRUPTS_DONT_KNOW,       Net_VMMPortDisable,       VMKCall, "") 
VMKCALL(VMK_NET_VMM_VMXNET_TX,              VMK_IF_INTERRUPTS_ON,              Net_VMMVmxnetTx,          VMKCall, "")
VMKCALL(VMK_NET_VMM_DISCONNECT,             VMK_IF_INTERRUPTS_ON,              Net_VMMDisconnect,     VMKCall, "") 
VMKCALL(VMK_NET_VMM_VLANCE_UPDATE_IFF,      VMK_IF_INTERRUPTS_ON,              Net_VMMVlanceUpdateIFF,            VMKCall, "") 
VMKCALL(VMK_NET_VMM_VLANCE_UPDATE_LADRF,    VMK_IF_INTERRUPTS_ON,              Net_VMMVlanceUpdateLADRF,          VMKCall, "") 
VMKCALL(VMK_NET_VMM_VLANCE_UPDATE_MAC,      VMK_IF_INTERRUPTS_ON,              Net_VMMVlanceUpdateMAC,          VMKCall, "") 
VMKCALL(VMK_NET_VMM_VMXNET_UPDATE_ETH_FRP,  VMK_IF_INTERRUPTS_ON,              Net_VMMVmxnetUpdateEthFRP,   VMKCall, "")
VMKCALL(VMK_NET_PIN_VMXNET_TX_BUFFERS,      VMK_IF_INTERRUPTS_ON,              Net_VMMPinVmxnetTxBuffers,       VMKCall, "") 
VMKCALL(VMK_NET_VMM_GET_PORT_CAPABILITIES,  VMK_IF_INTERRUPTS_ON,              Net_VMMGetPortCapabilities,      VMKCall, "") 

/*
 * USE VMK_UNKNOWN ENTRIES TO ADD NEW VMKCalls's
 * 
 * All the VMK_UNKNOWN calls are unclaimed and intended for future
 * expansion of the table without breaking vmcore/vmkernel
 * compatibility.  When these entries are exhausted, it is necessary
 * to add another set and break compatiblity.
 */ 
VMKCALL(VMK_UNKNOWN1,             VMK_IF_INTERRUPTS_DONT_KNOW,       NULL,                     VMKCall, "")
VMKCALL(VMK_UNKNOWN2,             VMK_IF_INTERRUPTS_DONT_KNOW,       NULL,                     VMKCall, "")
VMKCALL(VMK_UNKNOWN3,             VMK_IF_INTERRUPTS_DONT_KNOW,       NULL,                     VMKCall, "")
VMKCALL(VMK_UNKNOWN4,             VMK_IF_INTERRUPTS_DONT_KNOW,       NULL,                     VMKCall, "")
VMKCALL(VMK_UNKNOWN5,             VMK_IF_INTERRUPTS_DONT_KNOW,       NULL,                     VMKCall, "")
VMKCALL(VMK_UNKNOWN6,             VMK_IF_INTERRUPTS_DONT_KNOW,       NULL,                     VMKCall, "")
VMKCALL(VMK_UNKNOWN7,             VMK_IF_INTERRUPTS_DONT_KNOW,       NULL,                     VMKCall, "")
VMKCALL(VMK_UNKNOWN8,             VMK_IF_INTERRUPTS_DONT_KNOW,       NULL,                     VMKCall, "")
VMKCALL(VMK_UNKNOWN9,             VMK_IF_INTERRUPTS_DONT_KNOW,       NULL,                     VMKCall, "")
VMKCALL(VMK_UNKNOWN10,            VMK_IF_INTERRUPTS_DONT_KNOW,       NULL,                     VMKCall, "")
VMKCALL(VMK_UNKNOWN11,            VMK_IF_INTERRUPTS_DONT_KNOW,       NULL,                     VMKCall, "")
VMKCALL(VMK_UNKNOWN12,            VMK_IF_INTERRUPTS_DONT_KNOW,       NULL,                     VMKCall, "")
VMKCALL(VMK_UNKNOWN13,            VMK_IF_INTERRUPTS_DONT_KNOW,       NULL,                     VMKCall, "")
VMKCALL(VMK_UNKNOWN14,            VMK_IF_INTERRUPTS_DONT_KNOW,       NULL,                     VMKCall, "")
VMKCALL(VMK_UNKNOWN15,            VMK_IF_INTERRUPTS_DONT_KNOW,       NULL,                     VMKCall, "")

// must be last
VMKCALL(VMK_MAX_FUNCTION_ID,      VMK_IF_INTERRUPTS_DONT_KNOW,       NULL,                     VMKCall, "")
