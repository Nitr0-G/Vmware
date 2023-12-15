/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * vscsi_int.h --
 * vscsi interface for the device operations
 *
 */

#ifndef _VSCSI_INT_H_
#define _VSCSI_INT_H_

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "vmk_scsi_dist.h"

/* Extra info for virtual SCSI adapters 
 * (i.e. refers to a VMFS file, COW File, RDM or Raw Disk). 
 * Save all the info on opening the file, in case we are doing a
 * "lazy" open, because the VMFS is on a disk that is currently
 * reserved by another host.
 */

#define SCSI_VIRT_MAGIC		0xa9c25ba1

#define SCSI_ASYNC_INCR		32

typedef struct VSCSI_CapacityInfo {
   uint64 length; 
   uint32 diskBlockSize; 
} VSCSI_CapacityInfo;

struct SCSIVirtInfo;

typedef struct VSCSI_Ops {
   VMK_ReturnStatus (*VSCSI_VirtOpen) (VSCSI_DevDescriptor *desc, World_ID worldID, struct SCSIVirtInfo *info);
   VMK_ReturnStatus (*VSCSI_VirtCommand)(struct SCSIVirtInfo *vInfo, SCSI_Command *cmd, SCSI_ResultID *rid, World_ID worldID);
   VMK_ReturnStatus (*VSCSI_GetCapacityInfo)(VSCSI_DevDescriptor *desc, VSCSI_CapacityInfo *capInfo);
   void (*VSCSI_VirtClose) (struct SCSIVirtInfo *vInfo); 
   void (*VSCSI_VirtResetTarget)(struct SCSIVirtInfo *vInfo, SCSI_Command *cmd,
                                  VMK_ReturnStatus *result);
   void (*VSCSI_VirtAbortCommand)(struct SCSIVirtInfo *vInfo, SCSI_Command *cmd, 
                                  VMK_ReturnStatus *result);
} VSCSI_Ops;

struct VSCSI_Handle;

typedef struct SCSIVirtInfo {
   VSCSI_DevDescriptor  devDesc; // handle to underlying object for the virt adapter
   struct VSCSI_Handle	*handle;
   VSCSI_Ops            *devOps;
   SCSISenseData	sense;
   RWSemaphore		rwlock;	// Lock to allow fid to be changed.
   Async_Token          *resultListHead; // List of tokens of completed cmds+
   Async_Token          *resultListTail; // Tail of list+
   SCSI_Command         *sgExtCmd;       //Command with large SGArray
   World_ID             worldID;
   SCSI_ResetState	resetState;     // reset state
   void                 *privateData;   // XXXprivate data for each backend; 
   uint32		resetRetries;   // number of retries for this reset request%
   uint64		resetTSC;       // TSC of the next reset or reset retry%
   uint32		resetFlags;     // Flags for SCSIPostCmdCompletion
   uint32		blockSize;	// size of blocks on target=
   uint32		numBlocks;	// number of blocks on target=
   uint32               actionIndex;       // action invoked when cmd completes=
   uint16               sgMax; //max for current SG allocation
   uint8		devClass;	// device class for the vscsi devices 
} SCSIVirtInfo;

typedef struct SCSIVirtAsyncInfo {
   uint32 		magic;
   SCSIVirtInfo	 	*info;
   uint32 		serialNumber;
   uint32		savedFlags;
   Async_Callback	savedCallback;
} SCSIVirtAsyncInfo;

/*
 * A data structure to capture all the information necessary to
 * perform an I/O request on a virt. SCSI device in a helper world.
 * Used by SCSIIssueFSAsyncUmsh/Msh
 */
typedef struct {
   SCSIVirtInfo *info;
   SCSI_Command *cmd;
   uint64 ioOffset;
   uint32 length;
   Bool isRead;
   SCSI_ResultID resultID;
   int lengthByte;
} SCSI_AsyncCosArgs;

typedef struct VSCSI_Handle {
   VSCSI_HandleID handleID;          //handleID of VSCSI_Handle
   SCSIVirtInfo   *info;             //virtinfo corresponding to the VSCSI_Handle
   VSCSI_DevType  devType;           //device type, FS, COW, RDM, RawDisk
   int32          refCount;          //refCount on the VSCSI_Handle
   int32          pendCom;          //Number of outstanding commands on the handle.
   uint16         flags;             //SCSI_HANDLE_* flags
   uint8          virtualAdapterID;  //XXX Revisit
   uint8          virtualTargetID;
   SP_SpinLock    lock;              //Lock to protect the members of VSCSI_Handle
} VSCSI_Handle;

VMK_ReturnStatus VSCSI_RegisterDevice(VSCSI_DevType devType, VSCSI_Ops *devOps);
VMK_ReturnStatus VSCSI_OpenDevice(VSCSI_HandleID uniqueID, World_ID worldID,
                                  const char *deviceName, SCSI_HandleID *handleID,
                                  VSCSI_Ops **devOps);

void VSCSI_GenericCommand(SCSIVirtInfo *virtInfo, SCSI_Command *cmd,
                          SCSI_Status *scsiStatus, SCSISenseData *sense,
                          Bool *done);
void VSCSI_DoCommandComplete(SCSI_ResultID *rid, SCSI_Status status, 
                             uint8 *senseBuffer, uint32 bytesXferred, uint32 flags); 

//Storage device initialization function. Will be here until storage
//devices become modules.
void VSCSI_COWInit(void);
void VSCSI_FSInit(void);
void VSCSI_RawDiskInit(void);
void VSCSI_RDMPInit(void);
void VSCSI_VirtAsyncDone(Async_Token *token);
VSCSI_Handle *VSCSI_HandleFind(VSCSI_HandleID handleID);
void VSCSI_HandleRelease(VSCSI_Handle *handle);
#endif /* end of vscsi_int.h */ 
