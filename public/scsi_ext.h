/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * scsi_ext.h --
 *
 *      SCSI support in the vmkernel.
 */

#ifndef _SCSI_EXT_H_
#define _SCSI_EXT_H_

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMMEXT
#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMK_MODULE
#define INCLUDE_ALLOW_DISTRIBUTE
#include "includeCheck.h"

#include "scattergather.h"

#define SCSI_DEV_NAME_LENGTH	32
#define SCSI_DRIVER_NAME_LENGTH	32

#define MAX_SCSI_ADAPTERS	16
#define SCSI_MAX_TARGET_ID      255 
#define SCSI_MAX_LUN_NUM        255 

#define SCSI_MAX_CMD_LENGTH		16
#define SCSI_SENSE_BUFFER_LENGTH	20

typedef int32 SCSI_HandleID;
typedef uint32 SCSI_Status;

typedef enum {
   SCSI_QUEUE_COMMAND,
   SCSI_ABORT_COMMAND,
   SCSI_RESET_COMMAND,
   SCSI_VIRT_RESET_COMMAND,
   SCSI_DUMP_COMMAND,
} SCSI_CommandType;

typedef struct SCSI_Result {
   SCSI_CommandType type;
   uint32	serialNumber;
   uint32	serialNumber1; /* Used only by monitor side now */
   SCSI_Status	status;
   uint8	senseBuffer[SCSI_SENSE_BUFFER_LENGTH];
   uint32       bytesXferred;
} SCSI_Result;

/*
 * flags for the SCSI_Command.flags field.
 */
// Issue command immediately, even if other commands are queued on target.
// Don't do any disk scheduling.
#define SCSI_CMD_BYPASSES_QUEUE			0x01
// Don't do a failover if there is a DID_NO_CONNECT or not-ready error
#define SCSI_CMD_IGNORE_FAILURE			0x02
// Do minimal retries
#define SCSI_CMD_LOW_LEVEL			0x04
// Don't print out SCSI errors
#define SCSI_CMD_PRINT_NO_ERRORS		0x08
// Use LUN Reset instead of the full device reset
#define SCSI_CMD_USE_LUNRESET			0x10
// Can only be set with BYPASSES_QUEUE flag, allows VMK_WOULD_BLOCK
// status to be returned from SCSISyncCommand().
#define SCSI_CMD_RETURN_WOULD_BLOCK		0x20

typedef struct SCSI_Command {
   SCSI_CommandType		type;
   uint32			serialNumber;
   uint32			serialNumber1; /* Used only by monitor side now */
   uint32			originSN;
   int32			originHandleID; // The FIRST handle to which command was issued
   uint8			cdb[SCSI_MAX_CMD_LENGTH];
   uint32			cdbLength;
   int32			abortReason;
   uint32			resetFlags;
   uint32			flags;
   uint32			dataLength;
   uint32			sectorPos;
   /* 
    * sgArr must go last since extra elements will be appended on the end.
    */
   SG_Array			sgArr;
} SCSI_Command;

/*
 * SCSI Device parameters that are returned to 
 * monitor side callers from VMKernel.
 */
typedef struct SCSIDevParam {
   uint8  devClass;
   uint32 numBlocks;
   uint32 blockSize;
} SCSIDevParam;

// sgType is the structure the physical adapter uses to specify sg lists
typedef struct sgType {
   uint32    len;    // length of data segment
   void*     addr;   // physical address of data segment
} sgType;

// Next two structures needed to keep track of pages that have been pinned.
typedef struct sgPinType {
   unsigned  pages;    // number of pinned pages
   BPN       first;    // BPN of the first page
} sgPinType;

typedef struct SGPinArrType {
   uint32 sgLen;
   sgPinType sg[0];
} SGPinArrType;


/*
 * SCSI command passed by the linux host to the vmkernel.
 */
#define VMNIX_SG_MAX 128

typedef struct HostSCSI_Command {
   SCSI_Command command;
   SG_Elem sgArray[VMNIX_SG_MAX - SG_DEFAULT_LENGTH];
} HostSCSI_Command;

typedef struct SCSI_Info {
   uint32		maxID;
   uint32		maxLUN;
   uint32		queueDepth;
   uint32		sgTableSize;
   uint32		cmdPerLUN;
   uint32		scsiID;
} SCSI_Info;

/*
 * SCSI host error codes (these match the DID_* codes in drivers/scsi/scsi.h)
 */
#define SCSI_HOST_OK		0x0
#define SCSI_HOST_NO_CONNECT	0x1
#define SCSI_HOST_BUS_BUSY	0x2
#define SCSI_HOST_TIMEOUT	0x3
#define SCSI_HOST_BAD_TARGET	0x4
#define SCSI_HOST_ABORT		0x5
#define SCSI_HOST_PARITY	0x6
#define SCSI_HOST_ERROR		0x7
#define SCSI_HOST_RESET		0x8
#define SCSI_HOST_BAD_INTR	0x9
#define SCSI_HOST_PASSTHROUGH	0xa
#define SCSI_HOST_SOFT_ERROR	0xb

/*
 * The device error codes are defined in scsi_defs.h (SDSTAT_*)
 */

/* 
 * Extract the status fields.
 */
#define SCSI_HOST_STATUS(status) ((status >> 16) & 0xff)
#define SCSI_DEVICE_STATUS(status) (status & 0xff)

/*
 * Make a external scsi error code.
 */
#define SCSI_MAKE_STATUS(hostStatus, devStatus) ((hostStatus << 16) | devStatus)

typedef struct SCSI_Stats {
   uint32	commands;
   uint32	blocksRead;
   uint32	blocksWritten;
   uint32	aborts;   
   uint32	resets;
   uint32	readOps;
   uint32	writeOps;
   uint32       paeCmds;
   uint32       paeCopies;
   uint32       splitCmds;
   uint32       splitCopies;
   uint64       issueTime;
   uint64       totalTime;
} SCSI_Stats;


/* Max length of our disk id */
#define SCSI_DISK_ID_LEN	44

/* Length of vendor name in SCSI inquiry */
#define SCSI_VENDOR_LENGTH		8
/* Offset of vendor name in SCSI inquiry */
#define SCSI_VENDOR_OFFSET		8
/* Default vendor string, must be at least SCSI_VENDOR_OFFSET characters */
#define SCSI_DEFAULT_VENDOR_STR		"VMware   "
/* Length of model name in SCSI inquiry */
#define SCSI_MODEL_LENGTH		16
/* Offset of model name in SCSI inquiry */
#define SCSI_MODEL_OFFSET		16
/* Default model string, must be at least SCSI_MODEL_OFFSET characters */
#define SCSI_DEFAULT_MODEL_STR		"Virtual disk    " 
/* Length of revision in SCSI inquiry */
#define SCSI_REVISION_LENGTH		4
/* Offset of revision in SCSI inquiry */
#define SCSI_REVISION_OFFSET		32
/* Default revision string, must be at least SCSI_REVISION_OFFSET characters */
#define SCSI_DEFAULT_REVISION_STR	"1.0  " 

/* This is the indentification information for a SCSI disk. Do not change
 * this data structure. Some on-disk data structures depend on it.
 */
typedef struct SCSI_DiskId {
   uint8		type;
   uint8		len;
   uint16		lun;
   uint8		deviceType;
   uint8		scsiLevel;
   char			vendor[SCSI_VENDOR_LENGTH];
   char			model[SCSI_MODEL_LENGTH];
   char			revision[SCSI_REVISION_LENGTH];
   uint8		id[SCSI_DISK_ID_LEN];
} __attribute__ ((packed)) SCSI_DiskId;

typedef struct SCSI_Geometry {
   uint32 cylinders;
   uint32 heads;
   uint32 sectors;
} SCSI_Geometry; 

#endif
