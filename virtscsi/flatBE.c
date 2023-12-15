/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * These are the SCSI functions that are related to virtual SCSI
 * adapters/handles, which are used by virtual machines to access a VMFS
 * file
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
#include "host.h"
#include "vmk_scsi_dist.h" 
#include "vscsi_int.h" 
#include "fsClientLib.h"

#define LOGLEVEL_MODULE VSCSIFs
#define LOGLEVEL_MODULE_LEN 5
#include "log.h"

static void VSCSI_FSRegister(void);
static VMK_ReturnStatus VSCSI_FSOpen(VSCSI_DevDescriptor *desc, World_ID worldID, 
                                     SCSIVirtInfo *info);
static VMK_ReturnStatus VSCSI_FSGetCapacityInfo(VSCSI_DevDescriptor *desc, 
                                                VSCSI_CapacityInfo *capInfo);
static void VSCSI_FSClose(SCSIVirtInfo *virtInfo); 
static void VSCSI_FSResetTarget(SCSIVirtInfo *virtInfo, SCSI_Command *cmd, 
                                VMK_ReturnStatus *result);
static void VSCSI_FSAbortCommand(SCSIVirtInfo *virtInfo, SCSI_Command *cmd, 
                                 VMK_ReturnStatus *result);
static VMK_ReturnStatus VSCSI_FSCommand(SCSIVirtInfo *virtInfo, SCSI_Command *cmd, 
                                        SCSI_ResultID *rid, World_ID worldID);
static void VSCSI_IssueFSAsyncUmsh(void *data);

static VMK_ReturnStatus VSCSI_IssueFSAsyncMsh(SCSIVirtInfo *info, SCSI_Command *cmd,
                                              uint64 ioOffset, uint32 length, Bool isRead, 
                                              SCSI_ResultID *resultID, 
                                              int lengthByte);

static VMK_ReturnStatus VSCSI_IssueFSAsync(SCSIVirtInfo *info, SCSI_Command *cmd,
                                           uint64 ioOffset, uint32 length, Bool isRead,
                                           SCSI_ResultID *resultID, int lengthByte);

VSCSI_Ops VSCSI_FSOps = {
   VSCSI_FSOpen,
   VSCSI_FSCommand,
   VSCSI_FSGetCapacityInfo, //Hack for the Adapter for vscsi
   VSCSI_FSClose,
   VSCSI_FSResetTarget,
   VSCSI_FSAbortCommand
};

/*
 *-----------------------------------------------------------------------------
 * 
 * VSCSI_FSOpen
 *
 * Open the File the virtual disk corresponds to. 
 * 
 * Results :
 * Side Effects : None
 *-----------------------------------------------------------------------------
 */ 
static VMK_ReturnStatus 
VSCSI_FSOpen(VSCSI_DevDescriptor *desc, World_ID worldID, SCSIVirtInfo *info)
{ 
   ASSERT(desc->type == VSCSI_FS);
   // Already virt_scsi has the handle to the opened File
   return VMK_OK; 
}

/*
 *-----------------------------------------------------------------------------
 * 
 * VSCSI_FSGetCapacityInfo
 *
 * Get the length and diskBlockSize for the File
 * 
 * Side Effects : None
 *-----------------------------------------------------------------------------
 */ 
static VMK_ReturnStatus 
VSCSI_FSGetCapacityInfo(VSCSI_DevDescriptor *desc, VSCSI_CapacityInfo *capInfo)
{
   VMK_ReturnStatus     status = VMK_OK;
   FS_FileAttributes	attrs;

   ASSERT(desc->type == VSCSI_FS);
   if (desc->u.fid != FS_INVALID_FILE_HANDLE) {
      status = FSClient_GetFileAttributes(desc->u.fid, &attrs);
      if (status != VMK_OK) {
	 return status;
      } else {
         capInfo->length = attrs.length;
         capInfo->diskBlockSize = attrs.diskBlockSize;
      }
   } else {
      capInfo->length = 0;
      capInfo->diskBlockSize = DISK_SECTOR_SIZE;
   }
   return status;
}

/*
 *-----------------------------------------------------------------------------
 *
 * VSCSI_FSClose --
 *
 *      Does nothing.  The underlying file will get closed by linux / (userworld
 *      infrastructure).
 * 
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */ 
static void 
VSCSI_FSClose(SCSIVirtInfo *virtInfo)
{
   LOG(1, "Starting");
}

/*
 *-----------------------------------------------------------------------------
 * 
 * VSCSI_FSResetTarget --
 *    Abort all commands to the specified target, and reset the device(s)
 *    that it corresponds to. 
 * 
 * Results:
 *    Result returned by FSS_ResetCommand.
 *
 * Side Effects:
 *    None.
 *
 *-----------------------------------------------------------------------------
 */
static void 
VSCSI_FSResetTarget(SCSIVirtInfo *virtInfo, SCSI_Command *cmd, VMK_ReturnStatus *result)
{
   FS_FileHandleID handleID = virtInfo->devDesc.u.fid;
   VMK_ReturnStatus status;
   
   ASSERT(virtInfo->devDesc.type == VSCSI_FS);
   LOG(3, "Resetting target (command sn %u)\n", cmd->serialNumber);

   // Pass the reset to the file system switch.
   *result = FSS_ResetCommand(handleID, cmd);

   // Finally, release the reservation on this file (this may actually
   // trigger a physical reset if you are doing clustering)
   status = FSS_ReleaseFile(handleID, virtInfo->worldID, TRUE);
   if (status != VMK_OK) {
      Warning("Failed to release file after reset of virtual target...");
   }

}

/*
 *-----------------------------------------------------------------------------
 * 
 * VSCSI_FSAbortCommand --
 *    Abort the given command on the device(s) that the file corresponds to.
 * 
 * Results:
 *    Result returned by FSS_AbortCommand.
 *
 * Side effects:
 *    None.
 *
 *-----------------------------------------------------------------------------
 */ 
static void
VSCSI_FSAbortCommand(SCSIVirtInfo *virtInfo, SCSI_Command *cmd, VMK_ReturnStatus *result)
{
   FS_FileHandleID fhid = virtInfo->devDesc.u.fid;
   ASSERT(virtInfo->devDesc.type == VSCSI_FS);

   LOG(3, "Aborting command (sn %u)\n", cmd->serialNumber);

   // Pass the abort to the filesystem switch layer.
   *result = FSS_AbortCommand(fhid, cmd);
}

/*
 * Invoked to do a SCSI command on a virtual SCSI disk which is a file.
 */
static VMK_ReturnStatus 
VSCSI_FSCommand(SCSIVirtInfo *virtInfo, SCSI_Command *cmd, SCSI_ResultID *rid, 
		World_ID worldID)
{
   SCSI_Status scsiStatus;
   Bool done;
   Bool activeReservation;
   VMK_ReturnStatus status = VMK_OK;
   SCSISenseData senseBuffer;
   uint32 bytesXferred = 0;

   ASSERT(virtInfo->devDesc.type == VSCSI_FS);
   ASSERT(cmd->type == SCSI_QUEUE_COMMAND);
   ASSERT(rid->partition == 0);

   memset(&senseBuffer, 0, sizeof(SCSISenseData));

   /*
    * Don't allow most SCSI operations if virtual disk (VMFS file)
    * is reserved by another VM.
    */
   activeReservation =
      (FSS_ReserveFile(virtInfo->devDesc.u.fid, worldID, TRUE) ==
       VMK_RESERVATION_CONFLICT);

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

   switch (cmd->cdb[0]) {
      case SCSI_CMD_RESERVE_UNIT: {
	 DEBUG_ONLY(SCSIReserveCmd *cdb = (SCSIReserveCmd *)cmd->cdb);
	 ASSERT(cdb->opcode == SCSI_CMD_RESERVE_UNIT);
	 ASSERT(cdb->tparty == 0);
	 ASSERT(cdb->lun == 0);
	 ASSERT(cdb->ext == 0);

         status = FSS_ReserveFile(virtInfo->devDesc.u.fid, virtInfo->worldID, FALSE);
         if (status != VMK_OK) {
	    Warning("can't reserve by world %d", virtInfo->worldID);
	    scsiStatus = SCSI_MAKE_STATUS(SCSI_HOST_OK, SDSTAT_RESERVATION_CONFLICT);
         } else {
	    scsiStatus = SCSI_MAKE_STATUS(SCSI_HOST_OK, SDSTAT_GOOD);
	 }
         VSCSI_DoCommandComplete(rid, scsiStatus, (uint8 *)&senseBuffer, 0, 0);
	 return status;
      }

      case SCSI_CMD_RELEASE_UNIT: {
	 DEBUG_ONLY(SCSIReserveCmd *cdb = (SCSIReserveCmd *)cmd->cdb);
	 ASSERT(cdb->opcode == SCSI_CMD_RELEASE_UNIT);
	 ASSERT(cdb->tparty == 0);
	 ASSERT(cdb->lun == 0);
	 ASSERT(cdb->ext == 0);

	 FSS_ReleaseFile(virtInfo->devDesc.u.fid, virtInfo->worldID, FALSE);
	 scsiStatus = SCSI_MAKE_STATUS(SCSI_HOST_OK, SDSTAT_GOOD);
         VSCSI_DoCommandComplete(rid, scsiStatus, (uint8 *)&senseBuffer, 0, 0);
         return status;
      }

      default:
	 break;
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
	       status = VSCSI_IssueFSAsyncMsh(virtInfo, cmd, offset,
					    length, isRead, rid, 7);
	    } else {
	       status = VSCSI_IssueFSAsync(virtInfo, cmd, offset,
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
	       status = VSCSI_IssueFSAsyncMsh(virtInfo, cmd, offset,
					    length, isRead, rid, 4);
	    } else {
	       status = VSCSI_IssueFSAsync(virtInfo, cmd, offset,
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
	       status = VSCSI_IssueFSAsyncMsh(virtInfo, cmd, offset,
					    length, isRead, rid, 10);
	    } else {
	       status = VSCSI_IssueFSAsync(virtInfo, cmd, offset,
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
 * Issue an asynchronous read/write SCSI command to a virtual disk which is
 * a file.
 */
static VMK_ReturnStatus
VSCSI_IssueFSAsync(SCSIVirtInfo *info, SCSI_Command *cmd,
                   uint64 ioOffset, uint32 length, Bool isRead,
                   SCSI_ResultID *resultID, int lengthByte)
{
   VMK_ReturnStatus status;
   SCSIVirtAsyncInfo *asyncInfo;
   FS_FileHandleID handleID = info->devDesc.u.fid;
   uint32 bytesSeen;
   uint32 tokenOffset;
   int i;
   Async_Token *token = resultID->token;   
   ASSERT(info->devDesc.type == VSCSI_FS);

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
      Warning("scatter-gather says length %d, op says %d",
	      bytesSeen, length);
      status = VMK_BAD_PARAM;
   } else {
      token->originSN = cmd->originSN;
      token->originHandleID = cmd->originHandleID;
      status = FSS_AsyncFileIO(handleID, &cmd->sgArr, token,
                               (isRead) ? FS_READ_OP : FS_WRITE_OP);
   }

   if (status != VMK_OK) {
      SCSISenseData sense;
      uint8 *senseBuffer = (uint8 *)&info->sense;
      uint32 deviceStatus = SDSTAT_CHECK;

      memset(&sense, 0, sizeof(sense));
      ASSERT(token->refCount >= 1);
      Warning("fd %Ld status=%#x", handleID, status);
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
            Warning("Unknown vmk error 0x%x", status);
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
VSCSI_IssueFSAsyncUmsh(void *data)
{
   SCSI_AsyncCosArgs *args = (SCSI_AsyncCosArgs*)data;
   VMK_ReturnStatus status;

   status = VSCSI_IssueFSAsync(args->info, args->cmd,
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
VSCSI_IssueFSAsyncMsh(SCSIVirtInfo *info, SCSI_Command *cmd,
		    uint64 ioOffset, uint32 length, Bool isRead,
		    SCSI_ResultID *resultID, int lengthByte)
{
   SCSI_AsyncCosArgs *args;
   uint32 size;
   SCSI_Command *nCmd;

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

   return Helper_Request(HELPER_MISC_QUEUE, &VSCSI_IssueFSAsyncUmsh, 
			 (void*)args);
}

/*
 * FS registration functions for the vscsi switch
 */
void 
VSCSI_FSInit(void)
{
    VSCSI_FSRegister();
}

static void 
VSCSI_FSRegister(void)
{
   VMK_ReturnStatus status;

   status = VSCSI_RegisterDevice(VSCSI_FS, &VSCSI_FSOps);
   if (status != VMK_OK) {
      Warning("failed");
   }
}
