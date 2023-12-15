/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * scsi_ioctl.h --
 *
 *      SCSI ioctls used by vmkernel:vmk_scsi, and device drivers 
 *      modules/vmkernel/block/ide-cd.c
 */

#ifndef _SCSI_IOCTL_H_
#define _SCSI_IOCTL_H_

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_DISTRIBUTE
#define INCLUDE_ALLOW_VMK_MODULE
#include "includeCheck.h"

/* vmk_scsi:SCSI_Ioctl commands */
#define VMK_SIOCTL_IDE           1
#define VMK_SIOCTL_ATAPI_PACKET  2

typedef struct SioctlAtapiPacket {
   char cmd[12];     // IN
   char *buffer;     // IN
   unsigned int buflen;    // IN/OUT
   unsigned char sense_key;  // OUT
   unsigned char asc;        // OUT
   unsigned char ascq;       // OUT
} SioctlAtapiPacket;

// new ioctl for ide-cd driver to export a packet command interface
#define VMK_IDE_CD_PACKETCMD 0x5678

#endif // _SCSI_IOCTL_H_
