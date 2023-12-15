/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * vmk_scsi.h --
 *
 *      SCSI support in the vmkernel.
 */

#ifndef _VMK_SCSI_H_
#define _VMK_SCSI_H_

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

/* Vendor Specific defines */
/*
 * DGC Clarion (EMC) Related defines
 */
#define  DGC_INQ_DATA_LEN     66
#define  INQ_VENDOR_OFFSET    8
/* The Trespass command needs 4 bytes for Mode page Header
 * and 8 bytes for Block descriptor and 4 bytes for the 
 * mode page data
 */
#define	TRESPASS_LEN	(4 + 4 + 8)

/*
 * Vendor unique commands to send/receive registration data
 * 0xEE - Advanced Array Setup Command
 * 0xEF - Advanced Array Query Command
 */
#define	DGC_AAS_CMD			0xee
#define	DGC_AAQ_CMD			0xef


/*
 * IBM FAStT Related defines
 */
#define FASTT_RCP_PAGE_NUM  	        0x2C
#define FASTT_RCP_SUBPAGE_NUM  	        0x1

/*
 * Assuming a MODE_SENSE10 command.
 * Offset of the page in the mode parameter list
 * 8 byte mode parameter hdr + 8 byte block descriptor
 */
#define FASTT_MODE_SENSE_PAGE_OFFSET        16
/*
 * offset of the page in the mode parameter list + 1 byte page code + 1 byte page length
 */
#define FASTT_RCP_DATA_OFFSET_FROM_PAGE    (FASTT_MODE_SENSE_PAGE_OFFSET + 2)
/*
 * offset of the page in the mode parameter list + 1 byte page code + 1 byte subpage code + 2 byte page length 
 */
#define FASTT_RCP_DATA_OFFSET_FROM_SUBPAGE (FASTT_MODE_SENSE_PAGE_OFFSET + 4)
/*
 * Maximum number of LUNS supported in FAStT SIS Release 5.3
 */
#define FASTT_V53_MAX_SUPPORTED_LUNS       32
/*
 * Maximum number of LUNS supported in FAStt SIS Release 5.4
 */
#define FASTT_V54_MAX_SUPPORTED_LUNS       256
/*
 * The basic data in the Redundant Controller Page:
 *   Controller Serial Number           : 16 bytes
 *   Alternate Controller Serial Number : 16 bytes
 *   RDAC Mode bits                     : 2 bytes
 *   Alternate RDAC Mode bits           : 2 bytes
 *   Quiescence Timeout                 : 1 byte
 *   RDAC Options                       : 1 byte
 */
#define FASTT_RCP_BASE_DATA_LEN         38 
#define FASTT_RCP_RESERVED_BYTES        2
#define FASTT_RCP_MAX_DATA_LEN	       (FASTT_RCP_DATA_OFFSET_FROM_SUBPAGE + \
                                        FASTT_RCP_BASE_DATA_LEN + \
                                        FASTT_V54_MAX_SUPPORTED_LUNS + \
                                        FASTT_RCP_RESERVED_BYTES)

#define FASTT_RCP_V53_DATA_LEN	   	0x68

#define FASTT_UCR_LEN			0x40
#define FASTT_UCR_BUFFER_ID		0xEE
#define FASTT_CTRL_SERIAL_NUMBER_LEN	16

/*
 * Byte in the User Configurable Region of the FAStT SAN that
 * contains the setting for Automatic Volume Transfer
 */
#define FASTT_UCR_AVT_BYTE		0x33
/*
 * Bit in the SCSI_FASTT_UCR_AVT_BYTE of the User Configurable Region of 
 * the FAStT SAN that contains the setting for Automatic Volume Transfer
 */
#define FASTT_UCR_AVT_MASK		0x40

/*
 * Byte offsets in the FAStT Redundant Controller Page for the
 * primary and alternate controller status
 */
#define FASTT_RCP_DATA_RDAC_SN_OFFSET           0
#define FASTT_RCP_DATA_ARDAC_SN_OFFSET	        16
#define FASTT_RCP_DATA_RDAC_MODE_BYTE1_OFFSET   32
#define FASTT_RCP_DATA_RDAC_MODE_BYTE2_OFFSET   33
#define FASTT_RCP_DATA_ARDAC_MODE_BYTE1_OFFSET  34
#define FASTT_RCP_DATA_ARDAC_MODE_BYTE2_OFFSET  35
#define FASTT_RCP_DATA_LUN_INFO_OFFSET          38

/* Partition table (extended partition) related constants and macro */
#define DOS_EXTENDED_PARTITION	 0x05
#define LINUX_EXTENDED_PARTITION 0x85
#define WIN98_EXTENDED_PARTITION 0x0f

#define SCSI_ISEXTENDEDPARTITION(p)  ((p)->type == DOS_EXTENDED_PARTITION ||  \
          			      (p)->type == WIN98_EXTENDED_PARTITION ||\
			    	      (p)->type == LINUX_EXTENDED_PARTITION)

#define SCSI_PTABLE_SECTOR_OFFSET    446

/* 16-byte structure representing a partitiontable entry on disk. At most 4
 * such entries can be stored starting SCSI_PTABLE_SECTOR_OFFSET in a
 * diskBlock (sector) on disk */
typedef struct Partition {
   uint8 boot_ind;	// 0x80 - active
   uint8 startHead;	// Starting head
   uint8 startSector;	// Starting sector
   uint8 startCylinder;	// Starting cylinder
   uint8 type;		// Partition type
   uint8 endHead;	// Ending head
   uint8 endSector;	// Ending sector
   uint8 endCylinder;	// Ending cylinder
   uint32 firstSector;	// Starting sector counting from 0.
   uint32 numSectors;	// Number of sectors.
} __attribute__((packed)) Partition;

/* Given the partition table sector (read from disk), get a pointer to
 * the first partition entry
 */
#define SCSI_FIRSTPTABLEENTRY(sector) ((Partition *)(((uint8 *)(sector)) + \
                                       SCSI_PTABLE_SECTOR_OFFSET));

struct SCSI_Adapter;
struct World_Handle;
struct World_InitArgs;

typedef enum { SCSI_RESET_NONE,        // No reset 
	       SCSI_RESET_BUSY,        // The reset request is beeing serviced by one of the reset handler worlds
	       SCSI_RESET_DRAINING,    // Waiting for all I/Os to drain before completing the handle reset
	       SCSI_RESET_REQUESTED    // Need to perform a SCSI_Handle reset at the earliest opportunity
} SCSI_ResetState;

// disk scheduling shares
#define	SCSI_SCHED_SHARES_MIN		(1)
#define	SCSI_SCHED_SHARES_MAX		(100000)
#define	SCSI_SCHED_SHARES_LOW		(CONFIG_OPTION(DISK_SHARES_LOW))
#define	SCSI_SCHED_SHARES_NORMAL	(CONFIG_OPTION(DISK_SHARES_NORMAL))
#define	SCSI_SCHED_SHARES_HIGH		(CONFIG_OPTION(DISK_SHARES_HIGH))

// Constants of SCSI_Handle flags field
#define SCSI_HANDLE_HOSTOPEN            0x00000001	// Opened by host
#define SCSI_HANDLE_READONLY            0x00000002	
#define SCSI_HANDLE_CLOSING             0x00000004	// No more ops allowed
// SG extension in progress
#define SCSI_HANDLE_EXTSG               0x00000008	
// Multiple writers can open this SCSI device
#define SCSI_HANDLE_MULTIPLE_WRITERS	0x00000010
// Reserves, releases, and bus resets should be passed to physical bus
#define SCSI_HANDLE_PHYSICAL_RESERVE	0x00000020
// An IDE device in the guest, so call the IDE monitor action
#define SCSI_HANDLE_IDE                 0x00000040

/* 
 * Retry count values for low-level (scanning) commands,
 * do minimal retries on busy and no retries on reservation conflicts.
 */
#define SCSI_LOW_LEVEL_CMD_MAX_RETRIES 5
#define SCSI_LOW_LEVEL_CONFLICT_MAX_RETRIES 1

/*
 * Number of retries when we get a SCSI_HOST_ERROR returned from the
 * driver. This error type usually will not be recovererable, but we
 * want to retry a few times just to make sure.
 */
#define SCSI_ERROR_MAX_RETRIES 3

/*
 * Number of times we are willing to retry a synchronous command if we get
 * BUS_BUSY returned from the driver (which happens during failover, or
 * bus reset, or link up/down).  Please note that this has nothing to do
 * with SCSI_TIMEOUT, since we may get BUS_BUSY returned way before the
 * timeout expires, which is actually the case when we do failover. --
 * thor
*/
#define SCSI_BUSY_MAX_RETRIES 1000

/*
 * Number of milliseconds to sleep before retrying whenever we fail a
 * synchronous cmd due to a busy error.  We do not want to retry
 * immediately since the driver may be resetting, re-establishing a link,
 * or doing a failover.
 */
#define SCSI_BUSY_SLEEP_TIME 50

/*
 * Number of milliseconds to sleep before retrying whenever we fail a
 * synchronous cmd due to a reservation conflict.  If another host has
 * reserved the disk to get an FS or file lock, we want to wait a bit so
 * it has a chance to get the lock and release the disk.
 */
#define SCSI_CONFLICT_SLEEP_TIME 50

/*
 * Fields marked with '*' are protected by scsiLock, fields marked with
 * '+' are protected by the adapter lock, fields marked with '=' are
 * constant once initialized, fields marked by % are protected by the
 * handleArrayLock ?? means locking still needs to be checked.
 */

// Handle to a partition of a SCSI target or to an entire SCSI target
// (represented as partition 0), used by a single world
typedef struct SCSI_Handle {
   struct SCSI_Adapter	*adapter;	// should equal target->adapter
   struct SCSI_Target	*target;
   uint32		partition;	// 0 represents whole target=
   World_ID	        worldID;	// world ID=
   Async_Token		*resultListHead; // List of tokens of completed cmds+
   Async_Token		*resultListTail; // Tail of list+ Used only by COS
   SCSI_HandleID	handleID;	// id of handle=
   uint32		serialNumber;	// next serial number+
   int32		refCount;	// # of outstanding SCSIHandleFind()
					// accesses PLUS one (for open of
					// device itself)%
   int32                pendCom;        // pending commands on this handle+
   uint16               flags;		// SCSI_HANDLE_* flags ??
} SCSI_Handle;

// Element of a command queue, created when adapter has too many
// outstanding commands.
typedef struct SCSI_QElem {
   struct SCSI_QElem    *next;
   SCSI_Handle          *handle;
   Async_Token          *token;
   SCSI_Command         *cmd;
} SCSI_QElem;

// Per-world, per-target cmd queue and accounting data for disk BW scheduling
typedef struct SCSI_SchedQElem {
   struct SCSI_SchedQElem       *next; // list for the target+
   struct SCSI_SchedQElem       *nextInWorld; // list for the world
					// prot by targetListLock
   struct SCSI_Target   *target;	// target for this Q element=
   Bool                 active;		// TRUE if cmds queued or in flight+
   uint16               queued;		// number of cmds queued+
   int16                cif;            // number of cmds in flight+
   uint64               stride;
   uint32               shares;
   uint64               lvt;
   World_ID             worldID;
   SCSI_QElem           *reqQueHead;	// queue of regular cmds for this target/world+
   SCSI_QElem           *reqQueTail;
   SCSI_QElem           *priReqQueHead;	// queue of priority cmds for this target/world+
   SCSI_QElem           *priReqQueTail;
   Proc_Entry           procShares;	// /proc/vmware/vm/<ID>/disk/shares
   SCSI_Stats		stats;		// stats for this world+
} SCSI_SchedQElem;

struct SCSI_Adapter;

#define SCSI_MAX_XFER 16*1024*1024

typedef struct WorldScsiState {
   Proc_Entry           procWorldDiskDir;
   SCSI_SchedQElem      *targetList;	// list of targets opened by this world
   SP_SpinLock		targetListLock;	// lock when accessing targetList field
}  WorldScsiState;

typedef enum {
   RSTATUS_NO_RETRY = 0,
   RSTATUS_RESV_CONFLICT = 0x00c0de01,	/* reservation conflict */
   RSTATUS_BUSY,			/* busy */
   RSTATUS_UNIT_ATTN,			/* unit attention */
   RSTATUS_CMD_ABORTED,			/* aborted command */
   RSTATUS_HOST_TIMEOUT,		/* timeout & failed abort */
   RSTATUS_HOST_ABORT,			/* timeout & abort */
   RSTATUS_ERROR              /* error */
} SCSI_RetryStatus;
/* Primarily to get a list of all the targets in the machine. */
typedef struct SCSI_TargetList {
   SCSI_Target            *target; 
   struct SCSI_TargetList *next;
} SCSI_TargetList;

#define SCSI_DISK_DRIVER_STRING		"disk"

extern void SCSI_Init(VMnix_SharedData *sharedData);
extern void SCSI_WorldCleanup(struct World_Handle *world);

extern VMK_ReturnStatus SCSI_GetCapacity(SCSI_HandleID handleID,
			                 VMnix_GetCapacityResult *result);
extern VMK_ReturnStatus SCSI_GetGeometry(SCSI_HandleID handleID,
			                 VMnix_GetCapacityResult *result);
extern void SCSI_ReadGeometry(SCSI_Handle *handle, uint8 *pTableBuf, uint32 bufSize);
extern VMK_ReturnStatus SCSI_SGIO(SCSI_HandleID handleID, SG_Array *sgArr,
				  Bool isRead);
extern VMK_ReturnStatus SCSI_AsyncIO(SCSI_HandleID handleID, SG_Array *sgArr,
				     Bool isRead, Async_Token *token);

extern VMK_ReturnStatus SCSI_SetDiskShares(SCSI_HandleID handleID, 
                                           World_ID worldID, int32 shares);
extern VMK_ReturnStatus SCSI_OpenDeviceStatus(World_ID worldID, SCSI_HandleID handleID);
extern int16 SCSI_GetCmplMapIndex(SCSI_HandleID handleID);
extern uint32 SCSI_GetTargetClass(SCSI_HandleID handleID);
extern void SCSI_Cleanup(void);
extern VMK_ReturnStatus SCSI_WorldInit(struct World_Handle *world,
                                       struct World_InitArgs *args);
extern VMK_ReturnStatus SCSI_RereadPTable(SCSI_HandleID handleID);
extern VMK_ReturnStatus SCSI_QueryHandle(SCSI_HandleID, char **name, uint32 *targetID,
                                         uint32 *lun, uint32 *partition,
					 uint32 *partitionType);

extern VMK_ReturnStatus SCSI_GetTargetInfo(const char *name, uint32 targetID,
                                           uint32 lun, VMnix_TargetInfo *info);
extern VMK_ReturnStatus SCSI_AdapterList(VMnix_AdapterListArgs *args,
                                        VMnix_AdapterListResult *result);
extern VMK_ReturnStatus SCSI_GetLUNList(VMnix_LUNListArgs *args,
                                        VMnix_LUNListResult *result);
extern void SCSI_ExecuteHostCommand(SCSI_HandleID handleID,
				    SCSI_Command *cmd, 
				    VMK_ReturnStatus *result);
extern VMK_ReturnStatus SCSI_CmdCompleteInt(SCSI_HandleID handleID,
                                            SCSI_Result *outResult, 
                                            Bool *more);
extern void SCSI_UpdateAdapters(void);
extern VMK_ReturnStatus Scsi_FindAdapName(uint32 bus, uint32 devfn, char **adapName);
extern VMK_ReturnStatus SCSI_Dump(SCSI_HandleID handleID, uint64 offset, 
                                  uint64 data, uint32 length, Bool isMachAddr);

extern VMK_ReturnStatus SCSI_HostIoctl(SCSI_HandleID handleID,
                                       uint32 hostFileFlags,
                                       uint32 cmd,
                                       uint32 userArgsPtr,
                                       int32 *result);

extern VMK_ReturnStatus SCSI_HostCharDevIoctl(uint32 major,
                                              uint32 minor,
                                              uint32 hostFileFlags,
                                              uint32 cmd,
                                              uint32 userArgsPtr,
                                              int32 *result);

Bool SCSI_ActiveHandles(World_ID worldID);
extern VMK_ReturnStatus SCSI_ChangeFd(SCSI_HandleID handleID,
				      FS_FileHandleID fileHandleID,
				      FS_FileHandleID *oldHandle);
extern VMK_ReturnStatus SCSI_ReservePhysTarget(SCSI_HandleID handleID,
					       Bool reserve);
extern VMK_ReturnStatus SCSI_ResetPhysBus(SCSI_HandleID handleID, Bool lunreset);

extern VMK_ReturnStatus SCSI_AdapProcInfo(char* adapName, char* buf,
                                          uint32 offset, uint32 count, 
                                          uint32* nread, int isWrite);
extern Bool SCSI_DiskIdsEqual(SCSI_DiskId *id1, SCSI_DiskId *id2);
extern void SCSI_ResolveDiskId(SCSI_DiskId *id, char *adapterName, 
                                  uint32 *targetID, uint32 *lun);
extern VMK_ReturnStatus SCSI_TimedWait(SCSI_HandleID handleID,
                                       Async_Token *token,
                                       SCSI_RetryStatus *retry);
extern void SCSI_ResetInit(void);
VMK_ReturnStatus SCSI_RescanDevices(void *driverData);
extern void VSCSI_Init(void);
extern VMK_ReturnStatus SCSIIssueCommand(SCSI_Handle *handle, SCSI_Command *cmd, 
                                         SCSI_ResultID *rid);

extern VMK_ReturnStatus SCSI_AbortCommand(SCSI_HandleID handleID,
					  SCSI_Command *cmd);
extern VMK_ReturnStatus SCSI_ResetCommand(SCSI_HandleID handleID,
					  SCSI_Command *cmd);

extern void FSDisk_RegisterDevice(const char* adapterName, uint16 targetID,
                                  uint16 lun, uint32 numBlocks, uint32 blockSize);
extern void FSDisk_UnregisterDevice(const char* adapterName, uint16 targetID, 
                                    uint16 lun);

extern VMK_ReturnStatus SCSI_ObtainRegisteredTargetsList(SCSI_TargetList **registeredTargetList);
extern void SCSI_FreeRegisteredTargetsList(SCSI_TargetList *registeredTargetList);
#endif
