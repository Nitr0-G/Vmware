/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * vmk_scsi_dist.h --
 *
 *      SCSI support in the vmkernel.
 */

#ifndef _VMK_SCSI_DIST_H_
#define _VMK_SCSI_DIST_H_

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_DISTRIBUTE
#define INCLUDE_ALLOW_VMK_MODULE
#include "includeCheck.h"
#include "proc_dist.h"
#include "world_dist.h"
#include "vm_atomic.h"
#include "kseg_dist.h"
#include "idt_dist.h"
#include "scsi_ext.h"
#include "scsi_defs.h"
#include "async_io.h"
#include "partition_dist.h"

/* Constants for adapter->flags */
#define SCSI_PROC_ENTRY_ADDED       0x01 // Whether proc entry made yet
#define SCSI_SHARED_DEVICE          0x02 // Shared with console OS
#define SCSI_VIRT_DEVICE            0x04 // Accesses a VMFS file
#define SCSI_BLOCK_DEVICE           0x08 // underlying block device

/* Constants for target->flags */ 
// Set if the target has a "passive" controller that must be manually activated
#define SCSI_SUPPORTS_MANUAL_SWITCHOVER	   0x00000001
// Set when the target's active controller is in the process of being switched.
#define SCSI_MANUAL_SWITCHOVER_UNDERWAY	   0x00000002
// SAN device requires MRU policy to prevent thrashing
#define SCSI_MUST_USE_MRU_POLICY	   0x00000004
// Set if the target is SCSI-reserved by the local machine/HBA for any
// reason (may not be accurate if we haven't seen the power-on/reset check
// condition on a path yet).
#define SCSI_RESERVED_LOCAL		   0x00000008
#define SCSI_DONT_RETRY_ON_RESERV_CONFLICT 0x00000010

#define SCSI_DEV_UNKN			   0x00000000	// Generic device type
#define SCSI_DEV_HSV			   0x00010000	// HP/Compaq HSV SAN (EVA)
#define SCSI_DEV_MSA			   0x00020000	// HP/Compaq MSA - Modular SAN Array
#define SCSI_DEV_DGC			   0x00040000	// EMC Clariion (FC4700 or CX)
#define SCSI_DEV_FASTT			   0x00080000	// IBM FAStT
#define SCSI_DEV_SVC			   0x00100000	// IBM SVC 
#define SCSI_DEV_HSG80			   0x00200000	// DEC/COMPAQ/HP HSG80
#define SCSI_DEV_FASTT_V54		   0x00400000	// IBM FAStT supporting V54 spec

#define	SCSI_DEV_PSEUDO_DISK		   0x01000000	// pseudo devices like the Clariion LUNZ device

/* Default values for target->blockSize and target->blockShift */
#define DEFAULT_PSEUDO_DISK_BLOCK_SIZE	        512 

/*
 * Flags for SCSI_DoCommandComplete.
 */
#define SCSI_DEC_CMD_PENDING		0x01	// Decrement issued cmd counts
#define SCSI_FREE_CMD			0x02	// Free up rid->cmd

typedef struct SCSI_ResultID {
   SCSI_HandleID	handleID;   	// SCSI handle issuing command
   struct SCSI_Target	*target;	// Target specified by handle
   uint32		partition;	// Partition specified by handle
   uint32		serialNumber;
   Async_Token		*token;
   // Following fields are NULL if SCSI command was never actually issued
   struct SCSI_Path	*path;		// path used to issue command
   SCSI_Command		*cmd;		// command that was issued
} SCSI_ResultID;

struct SCSI_Handle;
struct SCSI_SchedQElem;

// Information on a single SCSI partition
typedef struct SCSI_Partition {
   Partition_Entry 	entry;
   struct SCSI_Handle	*handle;	// open handle on ptn, if any
   uint32               flags;          // union of current open handles' flags
   uint32               nReaders;       // num current rd-only handles 
   uint32               nWriters;       // num current writable handles 
   SCSI_Stats		stats;   
   World_ID		reserveID;	// world that has reservation on
					// this partition
} SCSI_Partition;

/* Constants for path->states */ 
#define SCSI_PATH_ON		0	// Path is working and being used
#define SCSI_PATH_OFF		1	// Path is working, not being used
#define SCSI_PATH_DEAD		2	// Path is not working
#define SCSI_PATH_STANDBY	3	// Path is available, but target SP
					// is in passive mode

/* Constants for path->flags */ 
// Failover to this path has already been tried, and failed
#define SCSI_PATH_FAILOVER_TRIED 	0x01
// A outstanding reservation has been made via this path.  It may actually
// have actually been cleared by a SCSI reset already, but we won't know
// it until the next I/O by this path (when we get the power-on/reset
// check condition).
#define SCSI_PATH_RESERVED_LOCAL	0x02
// The registration command for this path has been sent.
#define SCSI_PATH_REGISTRATION_DONE     0x04

/*
 * Fields marked with '*' are protected by scsiLock, fields marked with
 * '+' are protected by the adapter lock, fields marked with '=' are
 * constant once initialized.
 */

typedef struct SCSI_Path {
   struct SCSI_Path	*next;		// Next path for a target+
   struct SCSI_Path	*deadPathNext;	// Next path on deadPathList
   struct SCSI_Adapter	*adapter;	// HBA that path uses=
   uint16		id;		// SCSI id that path goes to=
   uint16		lun;		// LUN that path goes to=
   uint16		state;		// State of the path+
   uint16		active;		// number of commands outstanding+
   uint16	        flags;		// more path state
   uint16		notreadyCount;	// some SANs (SVC) require that commands that
                                        // complete with a NOT READY status be retried
   struct SCSI_Target	*target;	// Target (LUN) that this path goes to=
} SCSI_Path;

typedef enum {
   SCSI_PATH_FIXED,		//Use preferred path,failback to it if possible
   SCSI_PATH_MRU,		// Use most recently used path, don't failback
   SCSI_PATH_ROUND_ROBIN,	// Round robin among all good paths
} SCSI_PathPolicy;

// Information on a single SCSI target (LUN)
typedef struct SCSI_Target {
   struct SCSI_Target     *next;	// additional targets on adapter+
   struct SCSI_Adapter    *adapter;	// primary adapter holding target=
   SCSI_Path		  *paths;	// paths to this target+
   SCSI_Path		  *activePath;	// most recently used path+
   SCSI_Path		  *preferredPath; // preferred path (use if alive) +
   SCSI_PathPolicy	  policy;	// policy to decide path to issue on
   uint16                 id;		// target id number=
   uint16                 lun;		// lun number=
   uint16                 maxQDepth;	// max outstanding cmds for device=
   uint16                 curQDepth;	// current max outstanding cmds+

   // blockSize and numBlocks are non-zero if and only if target is a
   // block device (i.e. a disk, WORM, or optical device).
   uint32		  blockSize;	// size of blocks on target=
   uint32		  numBlocks;	// number of blocks on target=
   SCSI_Geometry          geometry;     // disk geometry= 
   SCSI_Partition 	  *partitionTable;// array of info on partitions*
					// 0th element represents whole target
   uint16		  numPartitions;// # of partitions
   uint16		  blockShift;	// log of size of blocks=
   int16		  useCount;	// # of open SCSI_Handles on target+
   int16		  refCount;	// ref count by SCSI_FindTarget()+
   struct SCSI_SchedQElem *schedQ;	// list of worlds using this target+
   struct SCSI_SchedQElem *lastWorldIssued;// last world that issued a command+
   uint8                  devClass;	// Fields set by SCSIGetAttrs()=
   uint8                  qControlState; // State for multiVM detection+
   uint16                 lastNReq;	// # of consecutive commands issued 
                                        // by this world+
   uint16                 qControlCount; // # of VM sched switches at
                                         // which we lower the Q depth+
   uint32                 lastReqSector;// sector number of last req+
   uint16                 qcount;	// number of commands queued+
   uint16                 active;	// number of commands outstanding+
   uint32                 gShares;	// global shares+
   uint64                 gStride;	// Global stride+
   uint64                 gvt;		// global virtual time+
   Proc_Entry             procEntry;
   SCSI_DiskId            diskId;
   SCSI_Stats             stats;        // stats for this target since detect 
   // See comment on delayCmds at SCSIIncDelayCmds().
   int			  delayCmds;	// while > 0, queue most commands
   uint32		  flags;	// flags defined above+
   // Reservations by SCSI_ReservePhysTarget() that are in progress+
   int32		  pendingReserves;
   struct SCSI_Target	  *rescanNext;  // next target to be
                                        // rescanned. Protected by
                                        // 'rescanInProgress'
   void                   *vendorData;   // misc data pointer
   uint32                 vendorDataLen; // size of data area
} SCSI_Target;


typedef enum {
	PATH_EVAL_OFF,		        // not evaluating paths
	PATH_EVAL_REQUESTED,	        // requested evaluation
	PATH_EVAL_ON,		        // currently evaluating paths
	PATH_EVAL_RETRY     	        // continue evaluating paths
} SCSIPathEvalState;

typedef enum SCSI_RescanResultType {
   SCSI_RESCAN_EXISTING_DISK_NO_CHANGE,
   SCSI_RESCAN_EXISTING_DISK_CHANGED,
   SCSI_RESCAN_EXISTING_DISK_REMOVED,
   SCSI_RESCAN_EXISTING_DISK_DISAPPEARED_BUT_BUSY,
   SCSI_RESCAN_NONEXISTENT_DISK_NO_CHANGE,
   SCSI_RESCAN_NONEXISTENT_DISK_NOW_EXISTS,
   SCSI_RESCAN_ERROR
} SCSI_RescanResultType;

typedef struct SCSI_Adapter {
   struct SCSI_Adapter 	*next;		// next adapter in hash table*
   SP_SpinLock		lock;
   char 		name[SCSI_DEV_NAME_LENGTH]; // name=
   char 		driverName[SCSI_DRIVER_NAME_LENGTH]; // drivername=
   int32		openCount;	// # of open SCSI_Handles on adapter*
   uint8                numTargets;     // number of active targets+
   SCSI_Target		*targets;	// queue of target pointers+
   int32		asyncInProgress;// pending cmds on whole adapter+
   uint32               *qDepthPtr;	// points to can_queue in scsi_host=
   uint32		flags;		// flags=
   Bool			openInProgress;	//TRUE if scanning ptn tbls of targets*
   SCSI_Stats		stats;		// stats+
   uint32               qCount;		// count of commands queued on targets+
   int			moduleID;	// id of module running this adapter=
   Atomic_uint32       *cosCmplBitmapPtr; // atomically updated
   uint16               bus;		// PCI bus=
   uint16               devfn;		// PCI devfn=
   Proc_Entry           adapProcEntry;	// protected by proc lock
   Proc_Entry           statsProcEntry;	// protected by proc lock
   IDT_Handler		intrHandler;	// For use in dumping core=
   void			*intrHandlerData;// For use in dumping core=
   int			intrHandlerVector;//For use in dumping core=
   int			sgSize;		// max # of scatter/gather entries=
   int			maxXfer;	// max size of single Xfer=
   Bool                 paeCapable;	// PAE capable=
   void 		*clientData;
   SCSIPathEvalState	pathEvalState;	// the current evaluation state+
   Bool			configModified;	// the devices on the adapter have changed+

   VMK_ReturnStatus (*command)(void *clientData, SCSI_Command *cmd,
                               SCSI_ResultID *rid,
                               World_ID worldID);
   Bool (*getInfo)(void *handle, uint32 targetID, uint32 lun, SCSI_Info *info,
                   uint8 *inquiryData, uint32 inquiryDataLength);
   void (*close)(void *clientData);
   VMK_ReturnStatus (*procInfo)(void* clientData, char* buf, uint32 offset,
                                uint32 count, uint32* nbytes, int isWrite);
   void (*dumpQueue)(void *clientData);
   void (*getGeometry)(void *handle, uint32 targetID, uint32 lun, 
                     uint32 nBlocks, uint8 *pTableBuf, uint32 bufSize,
                     SCSI_Geometry *geo);
   VMK_ReturnStatus (*sioctl)(void *handle, uint32 targetID, uint32 lun,
                             uint32 cmd, void *ptr);
   VMK_ReturnStatus (*ioctl)(void *handle, uint32 targetID, uint32 lun,
                             uint32 fileFlags, uint32 cmd, uint32 userArgsPtr, 
                             int32 *drvErr);
   SCSI_RescanResultType (*rescan)(void *clientData, int devNum, int lun); 
} SCSI_Adapter;

typedef struct COSLunList {
   struct COSLunList *next;
   uint16 bus;
   uint16 devfn;
   uint32 *tgtLunList;
   uint16 numTgtLuns;
} COSLunList;

typedef VMK_ReturnStatus (*SCSICharDevIoctlFn)(uint32 major, uint32 minor, 
                                               uint32 flags, uint32 cmd, 
                                               uint32 userArgsPtr, int32 *result);
extern void SCSI_CreateTarget(SCSI_Adapter *adapter, uint32 tid, 
                              uint32 lun, uint8 qDepth, SCSI_DiskId *diskId, Bool isPseudoDevice);
extern Bool SCSI_RemoveTarget(SCSI_Adapter *adapter, uint32 tid, uint32 lun,
                              Bool modUnload);
extern SCSI_Adapter *SCSI_CreateDevice(const char *name, void *clientData, int moduleID);
extern void SCSI_DestroyDevice(SCSI_Adapter *);
extern void SCSI_HostVMKSCSIHost(SCSI_Adapter *adapter, const char *name, 
                                 Bool reg);
extern void SCSI_HostVMKBlockDevice(SCSI_Adapter *adapter, const char *name, 
                                    const char* majName, uint16 major, 
                                    uint16 minShift, Bool reg);
extern void SCSI_RegisterCharDevIoctl(SCSICharDevIoctlFn fn);
extern void SCSI_UnregisterCharDevIoctl(void);
extern void SCSI_HostVMKCharDevice(const char *name, uint32 major, Bool reg);
extern void SCSI_HostVMKMknod(const char *name, const char* parent, 
                              uint32 devNo, Bool reg);
extern void SCSI_RegisterIRQ(void *a, uint32 vector, IDT_Handler h, void *handlerData);	
extern void SCSI_UpdateCmdStats(SCSI_Command *cmd, SCSI_ResultID *rid,
				World_ID worldID);

extern VMK_ReturnStatus SCSI_Read(SCSI_HandleID handleID, uint64 offset, 
			           void *data, uint32 length);
extern VMK_ReturnStatus SCSI_CloseDevice(World_ID worldID, SCSI_HandleID handleID);
extern void SCSI_DoCommandComplete(SCSI_ResultID *rid, SCSI_Status status,
                                   uint8 *senseBuffer, uint32 xferred, uint32 flags);
extern SCSI_Target *SCSI_FindTarget(SCSI_Adapter *adapter, uint32 tid,
				    uint32 lun, Bool lock);
extern void SCSI_ReleaseTarget(SCSI_Target *target, Bool lock);
extern VMK_ReturnStatus SCSI_OpenDevice(World_ID worldID, const char *name,
					uint32 target, uint32 lun, 
                                        uint32 partition, int flags, 
                                        SCSI_HandleID *handleID);

extern void SCSI_GenericCommandID(SCSI_HandleID handleID, SCSI_Command *cmd, 
                                  SCSI_Status *scsiStatus,
				  SCSISenseData *sense, Bool *done);

extern void SCSI_RescanFSUpcall(void);
extern void SCSI_IllegalRequest(SCSISenseData *sense, Bool isCommand, uint16 byteOffset);
extern void SCSI_InvalidOpcode(SCSISenseData *sense, Bool isCommand);
extern void SCSI_ProcSANDeviceConfigChange(Bool, Bool, int);
extern VMK_ReturnStatus SCSI_SendCommand(SCSI_Adapter *adapter, uint32 id,
					 uint32 lun, uint8 *cmd, int len,
					 char *scsiResult, int resultLen);
extern Bool SCSI_DiskIdsEqual(SCSI_DiskId *id1, SCSI_DiskId *id2);
extern SCSI_Target *SCSI_FindPathAll(char *adapterName, uint32 id, uint32 lun);
extern Bool SCSI_IsHandleToPAEAdapter(SCSI_HandleID handleID);

#include "config_dist.h"
/*
 *----------------------------------------------------------------------
 *
 * SCSIAdapterIsPAECapable
 *
 *      Checks to see if the adapter supports PAE
 *
 * Results: 
 *	TRUE if adapter supports PAE, FALSE otherwise
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
SCSIAdapterIsPAECapable(SCSI_Adapter *adapter)
{
   if (Config_GetOption(CONFIG_ENABLE_HIGH_DMA) && adapter->paeCapable) {
      return TRUE;
   }

   return FALSE;
}
#ifdef VMX86_DEVEL
typedef enum {
   DROP_NONE,
   DROP_HOST_CMD,
   DROP_ANY_CMD
} DropCmdType;
extern DropCmdType SCSI_DropCmd(Bool reset);
#endif

uint32 SCSI_GetMaxLUN(char *name, int devNum, int hostMaxLun);
Bool SCSI_IsLunMasked(char *name, int devNum, int lun);
Bool SCSI_SparseLUNSupport(char *name, int devNum);
Bool SCSI_UseDeviceReset(void);
Bool SCSI_UseLunReset(void);
void SCSI_StateChange(char *);
extern VMK_ReturnStatus SCSI_AddCOSLunList(uint16 bus, uint16 devfn, 
                                          uint32 *tgtLuns, uint16 numLuns);
extern COSLunList* SCSI_GetCOSLunList(SCSI_Adapter *adapter);
extern void SCSI_FreeCOSLunList(COSLunList *list);
#endif
