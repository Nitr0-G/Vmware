/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * vscsi.h --
 *
 *      SCSI virtualization in the vmkernel.
 */

#ifndef _VSCSI_H_
#define _VSCSI_H_

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "list.h"
#include "scattergather.h"
#include "scsi_ext.h"
#include "scsi_defs.h"
#include "splock.h"
#include "semaphore_ext.h"
#include "async_io.h"
#include "kseg_dist.h"
#include "partition.h"
#include "return_status.h"
#include "proc_dist.h"
#include "vmk_scsi_dist.h"
#include "vmnix_if.h"
#include "vmkernel_ext.h"

VMKERNEL_ENTRY VSCSI_GetDeviceParam(DECLARE_ARGS(VMK_SCSI_GET_DEV_PARAM));
VMKERNEL_ENTRY VSCSI_RegisterVMMDevice(DECLARE_ARGS(VMK_REGISTER_SCSI_DEVICE));
VMKERNEL_ENTRY VSCSI_AccumulateSG(DECLARE_ARGS(VMK_SCSI_ACCUM_SG));
VMKERNEL_ENTRY VSCSI_ExecuteCommand(DECLARE_ARGS(VMK_SCSI_COMMAND));
VMKERNEL_ENTRY VSCSI_CmdComplete(DECLARE_ARGS(VMK_SCSI_CMD_COMPLETE));
VMKERNEL_ENTRY VSCSI_WaitForCIF(DECLARE_ARGS(VMK_SCSI_WAIT_FOR_CIF));

void VSCSI_Init(void);
VMK_ReturnStatus VSCSI_CreateDevice(World_ID worldID, 
                                    VSCSI_DevDescriptor *desc,
                                    VSCSI_HandleID *handleID);
VMK_ReturnStatus VSCSI_DestroyDevice(World_ID worldID, 
                                     VSCSI_HandleID handleID);
#endif //_VSCSI_H_
