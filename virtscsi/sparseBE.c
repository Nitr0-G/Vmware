/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * These are the SCSI functions that are related to virtual SCSI
 * adapters/handles, which are used by virtual machines to access a VMFS COW
 * file. 
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "vmkernel.h"
#include "libc.h"
#include "splock.h"
#include "vmk_scsi.h"
#include "scsi_int.h"
#include "fsSwitch.h"
#include "fs_ext.h"
#include "vmnix_if_dist.h"
#include "util.h"
#include "host_dist.h" //For hostWorld ASSERT in SCSI_OpenVirtualDevice
#include "helper.h"
#include "config.h"
#include "world.h"
#include "vscsi_int.h" 
#include "cow.h" 

#define LOGLEVEL_MODULE VSCSICow
#define LOGLEVEL_MODULE_LEN 8
#include "log.h"

static void VSCSI_COWRegister(void);
static VMK_ReturnStatus VSCSI_COWOpen(VSCSI_DevDescriptor *desc, 
                                      World_ID worldID, SCSIVirtInfo *info);
static void VSCSI_COWClose(SCSIVirtInfo *virtInfo);
static VMK_ReturnStatus VSCSI_IssueCOWAsyncMsh(SCSIVirtInfo *info, SCSI_Command *cmd,
                                               uint64 ioOffset, uint32 length, Bool isRead,
                                               SCSI_ResultID *resultID, int lengthByte);
static VMK_ReturnStatus VSCSI_IssueCOWAsync(SCSIVirtInfo *info, SCSI_Command *cmd,
                                            uint64 ioOffset, uint32 length, Bool isRead,
                                            SCSI_ResultID *resultID, int lengthByte);
static void VSCSI_COWResetTarget(SCSIVirtInfo *virtInfo, SCSI_Command *cmd, 
                                 VMK_ReturnStatus *result);
static void VSCSI_COWAbortCommand(SCSIVirtInfo *virtInfo, SCSI_Command *cmd, 
                                  VMK_ReturnStatus *result);
static VMK_ReturnStatus VSCSI_COWCommand(SCSIVirtInfo *virtInfo, SCSI_Command *cmd, 
                                         SCSI_ResultID *rid, World_ID worldID);

static void VSCSI_IssueCOWAsyncUmsh(void *data);
static VMK_ReturnStatus VSCSI_COWGetCapacityInfo(VSCSI_DevDescriptor *desc,
                                                 VSCSI_CapacityInfo *capInfo);

struct VSCSI_Ops VSCSI_COWOps = {
   VSCSI_COWOpen,
   VSCSI_COWCommand,
   VSCSI_COWGetCapacityInfo,
   VSCSI_COWClose,
   VSCSI_COWResetTarget,
   VSCSI_COWAbortCommand
};

/*
 *-----------------------------------------------------------------------------
 * 
 * VSCSI_COWOpen
 *
 * Open the list of File Handles the unique Handle corresponds to.
 * 
 * Results : Open the cow related info for the valid Redo Logs.
 * Side Effects : None
 *-----------------------------------------------------------------------------
 */ 
static VMK_ReturnStatus 
VSCSI_COWOpen(VSCSI_DevDescriptor *desc, World_ID worldID, SCSIVirtInfo *info)
{

   LOG(1, "Starting");
   /*
    * This is just a NOP.  The cow handle we've been passed is already open and ready for
    * use.  We don't need to actually do anything.
    */

   /*
    * XXX I don't think we properly close the cow handles if the
    * vmm is kill -9'd.
    */
   ASSERT(desc->type == VSCSI_COW);
   return VMK_OK;
}

/*
 *-----------------------------------------------------------------------------
 * 
 * VSCSI_COWGetCapacityInfo
 *
 * Get the capacity info of the Base disk(Just a Hack for the target info in
 * Virtual Adapter)
 * 
 * Results : Attributes of the Base Disk are returned 
 * Side Effects : None
 *-----------------------------------------------------------------------------
 */ 
static VMK_ReturnStatus 
VSCSI_COWGetCapacityInfo(VSCSI_DevDescriptor *desc, 
                         VSCSI_CapacityInfo *capInfo)
{
   ASSERT(desc->type == VSCSI_COW);
   LOG(1, "Starting");
   COW_GetCapacity(desc->u.cid, &capInfo->length, &capInfo->diskBlockSize);
   return VMK_OK; 
}

/*
 *-----------------------------------------------------------------------------
 * 
 * VSCSI_COWClose
 * Fix it to return the status.. now just Hack
 *
 * Close all the File Handles the COWHandle corresponds to.
 * 
 * Results : All the redo logs and Base disk will be closed. 
 * Side Effects : None
 *-----------------------------------------------------------------------------
 */ 
static void 
VSCSI_COWClose(SCSIVirtInfo *virtInfo)
{
   /*
    * XXX I don't think we properly close the cow handles if the
    * vmm is kill -9'd.
    */

   LOG(1, "Starting");
   ASSERT(virtInfo->devDesc.type == VSCSI_COW);
   COW_CloseHierarchy(virtInfo->devDesc.u.cid);
}

/*
 *-----------------------------------------------------------------------------
 * 
 * VSCSI_COWResetTarget
 * Reset all the File Handles the cow handle corresponds to.
 *
 * 
 * Results : 
 * Side Effects : None
 *-----------------------------------------------------------------------------
 */ 
void 
VSCSI_COWResetTarget(SCSIVirtInfo *virtInfo, SCSI_Command *cmd, VMK_ReturnStatus *result)
{
   COW_HandleID handleID = virtInfo->devDesc.u.cid;
   VMK_ReturnStatus status;

   ASSERT(virtInfo->devDesc.type == VSCSI_COW);

   LOG(1, "Starting: %"FMT64"d", handleID);
   ASSERT(handleID != COW_INVALID_HANDLE);

   status = COW_ResetTarget(handleID, virtInfo->worldID, cmd);

   if(status != VMK_OK) {
      Warning("COW_ResetTarget failed with status %d", status);
   }
   *result = status;
}

/*
 *-----------------------------------------------------------------------------
 * 
 * VSCSI_COWAbortCommand
 * Abort All the outstanding commands on the File Handles corresponding 
 * COW Handle
 * 
 * Results : 
 * Side Effects : None
 *-----------------------------------------------------------------------------
 */ 
void 
VSCSI_COWAbortCommand(SCSIVirtInfo *virtInfo, SCSI_Command *cmd, VMK_ReturnStatus *result)
{
   COW_HandleID handleID = virtInfo->devDesc.u.cid;
   VMK_ReturnStatus status;
   ASSERT(virtInfo->devDesc.type == VSCSI_COW);

   LOG(1, "Starting");
   ASSERT(handleID != COW_INVALID_HANDLE);

   status = COW_AbortCommand(handleID, cmd);

   if(status != VMK_OK) {
      Warning("COW_ResetTarget failed with status %d", status);
   }
   *result = status;
}

/*
 * Invoked to do a SCSI command on a virtual SCSI disk (a COW file).
 * Results : Will do async IO on the redo Log where the block is found.
 */
static VMK_ReturnStatus 
VSCSI_COWCommand(SCSIVirtInfo *virtInfo, SCSI_Command *cmd, SCSI_ResultID *rid, 
                 World_ID worldID)
{
   SCSI_Status scsiStatus;
   Bool done = FALSE;   
   Bool activeReservation = FALSE;
   VMK_ReturnStatus status = VMK_OK;
   SCSISenseData senseBuffer;
   uint32 bytesXferred = 0;

   LOG(1, "Starting");
   if (cmd->type == SCSI_ABORT_COMMAND) {
      VSCSI_COWAbortCommand(virtInfo, cmd, &status);
      return status;
   }
   
   if (cmd->type == SCSI_RESET_COMMAND) {
      VSCSI_COWResetTarget(virtInfo, cmd, &status);
      return status;
   }
   
   ASSERT(cmd->type == SCSI_QUEUE_COMMAND);
   ASSERT(rid->partition == 0);

   SCSI_InitialErrorCheckOfCommand(cmd, activeReservation, &scsiStatus,
                                    &senseBuffer, &done);

   /* We may have cached sense (true for all devices including RAW disks) */
   if (!done) {
      SCSICheckForCachedSense((uint8 *)&virtInfo->sense, cmd, &scsiStatus,
                              &senseBuffer, &bytesXferred, &done);
   }
   if (done) {
      VSCSI_DoCommandComplete(rid, scsiStatus, (uint8 *)&senseBuffer, 0, 0);
      return VMK_OK;
   }

   /*
    * Note: For a READ_CAPACITY command, we will get the capacity of the
    * virtual target created in SCSI_OpenVirtualDevice(), which has a size
    * equal to the VMFS file length.
    */
   VSCSI_GenericCommand(virtInfo, cmd, &scsiStatus, &senseBuffer, &done);
   if (done) {
      VSCSI_DoCommandComplete(rid, scsiStatus, (uint8 *)&senseBuffer, 0, 0);
   } else {
      uint32 diskBlockSize;
      Bool isRead;

      diskBlockSize = virtInfo->blockSize;

      isRead = cmd->cdb[0] == SCSI_CMD_READ10 ||
	       cmd->cdb[0] == SCSI_CMD_READ6  ||
               cmd->cdb[0] == SCSI_CMD_READ16;

      switch (cmd->cdb[0]) {
	 case SCSI_CMD_READ10:
	 case SCSI_CMD_WRITE10: {      
	    SCSIReadWrite10Cmd *rwCmd = (SCSIReadWrite10Cmd *)cmd->cdb;
	    uint64 offset = ByteSwapLong(rwCmd->lbn) * (uint64)diskBlockSize;
	    uint32 length = ByteSwapShort(rwCmd->length) * diskBlockSize;

	    if(World_IsHOSTWorld(MY_RUNNING_WORLD)) {
               status = VSCSI_IssueCOWAsyncMsh(virtInfo, cmd, offset,
                                               length, isRead, rid, 7);
	    } else {
               status = VSCSI_IssueCOWAsync(virtInfo, cmd, offset,
                                            length, isRead, rid, 7);
	    }
	    break;
	 }
	 case SCSI_CMD_READ6:
	 case SCSI_CMD_WRITE6: {
	    uint8* rw = (uint8*) cmd->cdb;
	    uint64 offset = (((rw[1]&0x1f)<<16)|(rw[2]<<8)|rw[3]) * (uint64)diskBlockSize;
	    uint32 length = ((rw[4] == 0 ? 256 : rw[4])) * diskBlockSize;

	    if(World_IsHOSTWorld(MY_RUNNING_WORLD)) {
               status = VSCSI_IssueCOWAsyncMsh(virtInfo, cmd, offset,
                                               length, isRead, rid, 4);
	    } else {
               status = VSCSI_IssueCOWAsync(virtInfo, cmd, offset,
                                            length, isRead, rid, 4);
	    }
	    break;
	 }
	 case SCSI_CMD_READ16:
	 case SCSI_CMD_WRITE16: {      
	    SCSIReadWrite16Cmd *rwCmd = (SCSIReadWrite16Cmd *)cmd->cdb;
	    uint64 offset = ByteSwap64(rwCmd->lbn) * diskBlockSize;
	    uint32 length = ByteSwapLong(rwCmd->length) * diskBlockSize;
   
	    if(World_IsHOSTWorld(MY_RUNNING_WORLD)) {
	       status = VSCSI_IssueCOWAsyncMsh(virtInfo, cmd, offset,
                                               length, isRead, rid, 10);
            } else {
               status = VSCSI_IssueCOWAsync(virtInfo, cmd, offset,
                                            length, isRead, rid, 10);
	    }
	    break;
	 }
	 default:
	    Warning("command 0x%x isn't implemented", cmd->cdb[0]);
	    NOT_IMPLEMENTED();
      }
   }

   return status;
}
/*
 * Issue an asynchronous SCSI command to a virtual disk which is a
 * COW file.
 */
static VMK_ReturnStatus
VSCSI_IssueCOWAsync(SCSIVirtInfo *info, SCSI_Command *cmd,
                 uint64 ioOffset, uint32 length, Bool isRead,
		 SCSI_ResultID *resultID, int lengthByte)
{
   VMK_ReturnStatus status;
   SCSIVirtAsyncInfo *asyncInfo;
   uint32 bytesSeen;
   uint32 tokenOffset;
   int i;
   Async_Token *token = resultID->token;   
   ASSERT(info->devDesc.type == VSCSI_COW);

   LOG(1, "Starting: offset = %"FMT64"d, len = %d2 isread = %d", ioOffset, length, isRead);
   tokenOffset = ALIGN_UP(token->callerPrivateUsed, SCSI_ASYNC_INCR);
   ASSERT_NOT_IMPLEMENTED(tokenOffset + sizeof(SCSIVirtAsyncInfo) <= ASYNC_MAX_PRIVATE);
   asyncInfo = (SCSIVirtAsyncInfo *)&token->callerPrivate[tokenOffset];
   token->callerPrivateUsed = tokenOffset + sizeof(SCSIVirtAsyncInfo);
   asyncInfo->magic = SCSI_VIRT_MAGIC;
   asyncInfo->serialNumber = cmd->serialNumber;
   asyncInfo->info = info;
   asyncInfo->savedCallback = token->callback;
   asyncInfo->savedFlags = token->flags;

   bytesSeen = 0;
   for (i = 0; i < cmd->sgArr.length; i++) {
      cmd->sgArr.sg[i].offset = ioOffset + bytesSeen;
      bytesSeen += cmd->sgArr.sg[i].length;
   }

   token->flags = ASYNC_CALLBACK;
   token->callback = VSCSI_VirtAsyncDone;

   /*
    * Get read lock on virtual SCSI device.  Suspends the world if
    * online commit (which will get write lock) is occurring.
    */
   Semaphore_BeginRead(&info->rwlock);
   if (bytesSeen != length) {
      status = VMK_BAD_PARAM;
   } else {
      token->originSN = cmd->originSN;
      token->originHandleID = cmd->originHandleID;
      status = COW_AsyncFileIO(info->devDesc.u.cid, &cmd->sgArr, token,
                               (isRead) ? FS_READ_OP : FS_WRITE_OP);
   }

   if (status != VMK_OK) {
      SCSISenseData sense;
      uint8 *senseBuffer = (uint8 *)&info->sense;
      uint32 deviceStatus = SDSTAT_CHECK;

      memset(&sense, 0, sizeof(sense));
      ASSERT(token->refCount >= 1);
      sense.valid = TRUE;
      sense.error = SCSI_SENSE_ERROR_CURCMD;
      switch (status) {
         case VMK_NO_FREE_PTR_BLOCKS:
         case VMK_NO_FREE_DATA_BLOCKS:
	    // Also return error to monitor itself, so don't set
	    // status to VMK_OK.
            sense.key = SCSI_SENSE_KEY_VOLUME_OVERFLOW;
            break;
         case VMK_LIMIT_EXCEEDED:
         case VMK_BAD_PARAM:
            SCSI_IllegalRequest(&sense, TRUE, lengthByte);
	    status = VMK_OK;
            break;
         case VMK_NO_CONNECT:
         case VMK_NOT_READY:
         case VMK_METADATA_READ_ERROR:
         case VMK_METADATA_WRITE_ERROR:
         case VMK_READ_ERROR:
         case VMK_WRITE_ERROR:
         case VMK_IO_ERROR:
            sense.key = SCSI_SENSE_KEY_MEDIUM_ERROR;
	    status = VMK_OK;
            break;
         case VMK_NO_MEMORY:
            /* Should not get here.. Enforce on beta builds. */
            ASSERT(0);
            /* And if we do on release builds, try to buy time by 
	     * attempting a retry of the operation...
             */
            /* It is not enough to set sense.valid - see
               note in the definition of SCSISenseData */
            senseBuffer[0] = 0;
	    deviceStatus = SDSTAT_BUSY;
	    status = VMK_OK;
            break;
	 case VMK_RESERVATION_CONFLICT:
            /* It is not enough to set sense.valid - see
               note in the definition of SCSISenseData */
            senseBuffer[0] = 0;
	    deviceStatus = SDSTAT_RESERVATION_CONFLICT;
	    status = VMK_OK;
	    break;
	 case VMK_FS_LOCKED:
            /* It is not enough to set sense.valid - see
               note in the definition of SCSISenseData */
            senseBuffer[0] = 0;
	    /* Force retry of operation if FS was locked when trying to
	     * extend COWfile. */
	    deviceStatus = SDSTAT_BUSY;
	    status = VMK_OK;
	    break;
         case VMK_INVALID_HANDLE:
            break;
         default:
            /* Should not get here.. Enforce on beta builds. */
            ASSERT(0);
            /* And if we do on release builds, try to recover by 
	     * attempting a retry of the operation...
             */
            sense.key = SCSI_SENSE_KEY_MEDIUM_ERROR;
	    status = VMK_OK;
            break;
      }

      token->flags = asyncInfo->savedFlags;
      token->callback = asyncInfo->savedCallback;      

      Semaphore_EndRead(&info->rwlock);
      VSCSI_DoCommandComplete(resultID, SCSI_MAKE_STATUS(SCSI_HOST_OK, deviceStatus),
			     (uint8 *)&sense, 0, 0);
   }
   return status;
}



/*
 * Function to run in a helper world to perform I/O on a virtual SCSI
 * device on behalf of the COS. - This has to be pushed into a helper
 * world because the COS cannot be blocked in the vmkernel.
 */
static void 
VSCSI_IssueCOWAsyncUmsh(void *data)
{
   SCSI_AsyncCosArgs *args = (SCSI_AsyncCosArgs*)data;
   VMK_ReturnStatus status;

   LOG(1, "Starting");
   status = VSCSI_IssueCOWAsync(args->info, args->cmd,
			     args->ioOffset, args->length,
			     args->isRead, &args->resultID,
			     args->lengthByte);
   Mem_Free(args->cmd);
   Async_ReleaseToken(args->resultID.token);
   Mem_Free(args);
   return;   
}


/*
 * SCSI command processing for the COS in a helper world: We
 * have to hold on to all the information ourselves as things
 * move on in the vmkernel and various data structures we need in the
 * helper world might go away underneath us.
 */
static VMK_ReturnStatus
VSCSI_IssueCOWAsyncMsh(SCSIVirtInfo *info, SCSI_Command *cmd,
		    uint64 ioOffset, uint32 length, Bool isRead,
		    SCSI_ResultID *resultID, int lengthByte)
{
   SCSI_AsyncCosArgs *args;
   uint32 size;
   SCSI_Command *nCmd;

   LOG(1, "Starting");
   /*
    * get a copy of the SCSI command structure since it might get
    * free from underneath us.
    */
   size = sizeof(SCSI_Command);
   if(cmd->sgArr.length > SG_DEFAULT_LENGTH) {
      size += (cmd->sgArr.length - SG_DEFAULT_LENGTH) * sizeof(SG_Elem);
   }
   nCmd = (SCSI_Command *)Mem_Alloc(size);
   if (NULL == nCmd) {
      return VMK_NO_MEMORY;
   }
   memcpy(nCmd, cmd, size);
   cmd = nCmd;

   args = Mem_Alloc(sizeof(*args));
   if (NULL == args) {
      Mem_Free(cmd);
      return VMK_NO_MEMORY;
   }

   /*
    * resultID might get freed underneath us...
    */
   memset(args, 0, sizeof(*args));
   args->resultID = *resultID;
   args->resultID.cmd = cmd;

   args->info = info;
   args->cmd = cmd;
   
   args->ioOffset = ioOffset;
   args->length = length;
   args->isRead = isRead;
   args->lengthByte = lengthByte;

   /*
    * Make sure we hold on to the token until the helper world is
    * done with the request.
    */
   Async_RefToken(resultID->token);

   return Helper_Request(HELPER_MISC_QUEUE, &VSCSI_IssueCOWAsyncUmsh, 
			 (void*)args);
}

/*
 * COW Initialization function
 */
void 
VSCSI_COWInit(void)
{
    VSCSI_COWRegister();
}

/*
 * Registration of the COW backend to the vscsi switch
 */
static void 
VSCSI_COWRegister(void)
{
   VMK_ReturnStatus status;

   status = VSCSI_RegisterDevice(VSCSI_COW, &VSCSI_COWOps);
   if (status != 0) {
      Warning("failed:%#x", status);
   }
}
