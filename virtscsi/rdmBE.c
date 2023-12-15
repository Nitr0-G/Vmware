/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * These are the SCSI functions that are related to virtual SCSI
 * adapters/handles, which are used by virtual machines to access a RDM
 * in passthrough mode
 */

#include "vm_types.h"
#include "vm_libc.h"
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
#include "vmk_scsi_dist.h"
#include "fsClientLib.h"
#include "host.h"
#include "scsi_int.h"

#define LOGLEVEL_MODULE VSCSIRdm
#define LOGLEVEL_MODULE_LEN 8
#include "log.h"

static void VSCSI_RDMPRegister(void);
static VMK_ReturnStatus VSCSI_RDMPOpen(VSCSI_DevDescriptor *desc, World_ID worldID, 
                                       SCSIVirtInfo *info);
static VMK_ReturnStatus VSCSI_RDMPCommand(SCSIVirtInfo *info, SCSI_Command *cmd, 
                     SCSI_ResultID *rid, World_ID worldID);
static void VSCSI_RDMPClose(SCSIVirtInfo *virtInfo); 
static void VSCSI_RDMPResetTarget(SCSIVirtInfo *virtInfo, SCSI_Command *cmd, 
                                   VMK_ReturnStatus *result);
static void VSCSI_RDMPAbortCommand(SCSIVirtInfo *virtInfo, SCSI_Command *cmd, 
                                   VMK_ReturnStatus *result);
static VMK_ReturnStatus VSCSI_RDMPGetCapacityInfo(VSCSI_DevDescriptor *desc, 
                                                  VSCSI_CapacityInfo *capInfo);

VSCSI_Ops VSCSI_RDMPOps = {
   VSCSI_RDMPOpen,
   VSCSI_RDMPCommand,
   VSCSI_RDMPGetCapacityInfo,
   VSCSI_RDMPClose,
   VSCSI_RDMPResetTarget,
   VSCSI_RDMPAbortCommand
};

/*
 * Open the scsi device the RDM in passthrough mode corresponds to.
 * Side Effects : None
 */
static VMK_ReturnStatus 
VSCSI_RDMPOpen(VSCSI_DevDescriptor *desc, World_ID worldID, SCSIVirtInfo *virtInfo)
{
   VMK_ReturnStatus status = VMK_OK;
   FS_FileAttributes attrs;

   ASSERT(desc->type == VSCSI_RDMP);
   LOG(1, "Incoming File handle %Ld", desc->u.fid);

   if (desc->u.fid != FS_INVALID_FILE_HANDLE) {
      status = FSClient_GetFileAttributes(desc->u.fid, &attrs);
      if (status != VMK_OK) {
         Warning("FSClient_GetFileAttributes failed with status %d", status);
         return status;
      }
   } else {
      attrs.flags = 0;
      attrs.length = 0;
      attrs.diskBlockSize = DISK_SECTOR_SIZE;
   }

   if (attrs.flags & FS_RAWDISK_MAPPING) {
      SCSI_Handle *scsiHandle;
      ASSERT(attrs.rdmRawHandleID != FS_INVALID_FILE_HANDLE);
      scsiHandle = SCSI_HandleFind(attrs.rdmRawHandleID);
      if (scsiHandle == NULL) {
         return VMK_INVALID_TARGET;
      } else {
         virtInfo->privateData = scsiHandle; 
      }
      SCSI_HandleRelease(scsiHandle);
   }
   return status; 
}
  
/*
 * Get the capacity Info for the RDM in passthrough mode.
 * Side Effects : None
 */
static VMK_ReturnStatus 
VSCSI_RDMPGetCapacityInfo(VSCSI_DevDescriptor *desc, VSCSI_CapacityInfo *capInfo)
{
   FS_FileAttributes attrs;
   VMK_ReturnStatus status = VMK_OK;

   LOG(1, "Incoming File handle %Ld", desc->u.fid);

   ASSERT(desc->type == VSCSI_RDMP);
   if (desc->u.fid != FS_INVALID_FILE_HANDLE) {
      status = FSClient_GetFileAttributes(desc->u.fid, &attrs);
      if (status != VMK_OK) {
         Warning("FSClient_GetFileAttributes failed with status %d", status);
         return status;
      } else {
         capInfo->diskBlockSize = attrs.diskBlockSize; 
         capInfo->length = attrs.length; 
      }
   } else {
      capInfo->length = 0;
      capInfo->diskBlockSize = DISK_SECTOR_SIZE;
      status = VMK_INVALID_TARGET;
   }
   return status;
}

/*
 * Process the command for the RDM in the passthrough mode
 *
 * Side Effects : None
 */
static VMK_ReturnStatus 
VSCSI_RDMPCommand(SCSIVirtInfo *virtInfo, SCSI_Command *command, SCSI_ResultID *resultID,
                  World_ID worldID)
{
   SCSI_Handle *handle;
   VMK_ReturnStatus status;
   SG_Elem *sgElem;
   SCSI_Command *cmd;
   uint32 dataLength;
   uint32 sgLen;
   int i;
   uint32 cmdLength;
   SCSI_ResultID rid;
   SCSI_Adapter *adapter;
   SCSI_Target *target;
   uint32 offset;
   SG_Array *inSGArr = &command->sgArr; 
   Async_Token *token = resultID->token; 
   SCSIVirtAsyncInfo *asyncInfo;
   uint32 tokenOffset;

   ASSERT(virtInfo->devDesc.type == VSCSI_RDMP);
   //handle = (SCSI_Handle *)virtInfo->privateData;
   handle = SCSI_HandleFind(((SCSI_Handle *)virtInfo->privateData)->handleID);

   if (handle == NULL) {
      return VMK_INVALID_HANDLE;
   }
   adapter = handle->adapter;
   target = handle->target;

   if (handle->partition >= target->numPartitions) {
      Warning("SCSI IO to partition %d, np %d",
	      handle->partition, target->numPartitions);
      SCSI_HandleRelease(handle);
      return VMK_INVALID_PARTITION;
   }
   if (handle->partition != 0 &&
       target->partitionTable[handle->partition].entry.numSectors == 0) {
      Warning("SCSI IO to non-existent partition %d, np %d",
	      handle->partition, target->numPartitions);
      SCSI_HandleRelease(handle);
      return VMK_INVALID_PARTITION;
   }

   sgLen = inSGArr->length;
   cmdLength = sizeof(SCSI_Command) + (sgLen - SG_DEFAULT_LENGTH) * sizeof(SG_Elem);
   cmd = (SCSI_Command *)Mem_Alloc(cmdLength);
   ASSERT_NOT_IMPLEMENTED(cmd != NULL);
   memset(cmd, 0, cmdLength);

   tokenOffset = ALIGN_UP(token->callerPrivateUsed, SCSI_ASYNC_INCR);
   ASSERT_NOT_IMPLEMENTED(tokenOffset + sizeof(SCSIVirtAsyncInfo) <= ASYNC_MAX_PRIVATE);
   asyncInfo = (SCSIVirtAsyncInfo *)&token->callerPrivate[tokenOffset];
   token->callerPrivateUsed = tokenOffset + sizeof(SCSIVirtAsyncInfo);
   asyncInfo->magic = SCSI_VIRT_MAGIC;
   asyncInfo->serialNumber = command->serialNumber;
   asyncInfo->info = virtInfo;
   asyncInfo->savedCallback = token->callback;
   asyncInfo->savedFlags = token->flags;

   cmd->type = SCSI_QUEUE_COMMAND;
   cmd->sgArr.length = sgLen;
   if (inSGArr->addrType == SG_PHYS_ADDR) {
      cmd->sgArr.addrType = SG_PHYS_ADDR;
   } else {
      cmd->sgArr.addrType = SG_MACH_ADDR;
   }

   token->cmd = cmd;
   token->flags = ASYNC_CALLBACK;
   token->callback = VSCSI_VirtAsyncDone;

   dataLength = 0;
   sgElem = &cmd->sgArr.sg[0];
   for (i = 0; i < inSGArr->length; i++) {
      switch (inSGArr->addrType) {
      case SG_VIRT_ADDR: {
	 /*
	  * `startAddr' references memory allocated by Mem_Alloc in
	  * fs.c or partition.c.  Since we know the machine pages are
	  * allocated contiguously, we only need to translate the
	  * starting page.
	  */
	 VA startAddr = (VA)inSGArr->sg[i].addr;
	 sgElem->addr = VMK_VA2MA(startAddr);
	 sgElem->length = inSGArr->sg[i].length;
	 /*
          * Some SCSI adapters cannot handle DMA to non-aligned buffers
          */
         ASSERT((sgElem->addr & 0x07) == 0);
	 break;
      }
      case SG_MACH_ADDR:
      case SG_PHYS_ADDR:
	 *sgElem = inSGArr->sg[i];
	 break;
      default:
	 NOT_REACHED();
      }
      sgElem++;
      dataLength += inSGArr->sg[i].length;
   }

   offset = inSGArr->sg[0].offset >> target->blockShift;

   /*
    * Verify that the disk offset and length fall within the bounds of 
    * the given partition
    */ 
   if (handle->partition != 0) { 
      if (offset > target->partitionTable[handle->partition].entry.numSectors) {
         Mem_Free(cmd); 
         SCSI_HandleRelease(handle);
         LOG(0, "%p: IO error (offset off end of partition)", inSGArr);
         return VMK_IO_ERROR;
      }
      if ((offset + (dataLength >> target->blockShift)) >
           target->partitionTable[handle->partition].entry.numSectors) {
	 /*
          * truncate the I/O operation to fall with the the partition boundaries
          */
         LOG(0, "%p: IO truncated (off end of partition)", inSGArr);
         dataLength = (target->partitionTable[handle->partition].entry.numSectors - offset) << target->blockShift;
      }
   } 

   if (handle->partition != 0) {
      offset += target->partitionTable[handle->partition].entry.startSector;
   }
   cmd->cdbLength = command->cdbLength;
   cmd->dataLength = dataLength;

   SP_Lock(&adapter->lock);      
   handle->serialNumber++;
   cmd->serialNumber = handle->serialNumber;
   if (token->originHandleID) {
      cmd->originHandleID = token->originHandleID;
      cmd->originSN = token->originSN;
   }
   else {
      cmd->originHandleID = handle->handleID;
      cmd->originSN = cmd->serialNumber;
   }
   SP_Unlock(&adapter->lock);   
   SCSIInitResultID(handle, token, &rid);

   rid.serialNumber = cmd->serialNumber;

   ASSERT(token->resID != -1);
   if (World_IsHELPERWorld(MY_RUNNING_WORLD)) {
         token->resID = Host_GetWorldID();
   }
   Async_RefToken(token);
   ASSERT(token->resID == handle->worldID || handle->worldID == Host_GetWorldID());

   SCSI_GetXferData(cmd, handle->target->devClass, handle->target->blockSize);

   status = SCSIIssueCommand(handle, cmd, &rid);

   if (status == VMK_WOULD_BLOCK) {
      // IssueCommand has queued it. The caller need not do anything
      status = VMK_OK;
   }
   return status;
}

/*
 * Close the raw disk Mapping in passthrough mode
 */
static void 
VSCSI_RDMPClose(SCSIVirtInfo *virtInfo) 
{
   VMK_ReturnStatus status;
   SCSI_Handle *handle = (SCSI_Handle *)virtInfo->privateData;

   ASSERT(virtInfo->devDesc.type == VSCSI_RDMP);
   ASSERT(handle != NULL);

   status = SCSI_CloseDevice(virtInfo->worldID, handle->handleID);
   if (status != VMK_OK) {
      Warning("SCSI_CloseFile failed with status %d", status);
   }
   virtInfo->privateData = NULL;
}

/*
 * Reset the scsi device corresponding to the RDM in passthrough
 */
static void 
VSCSI_RDMPResetTarget(SCSIVirtInfo *virtInfo, SCSI_Command *cmd, VMK_ReturnStatus *result)
{
   uint64 handleID = virtInfo->devDesc.u.fid;
   SCSI_Handle *handle = (SCSI_Handle *)virtInfo->privateData;

   ASSERT(virtInfo->devDesc.type == VSCSI_RDMP);
   // If handleID == -1, we are using clustering on an unopened disk, which means
   // that we do not have an opened handle. We reset physically for clustering.
   ASSERT(handleID != FS_INVALID_FILE_HANDLE);
   if (handleID == FS_INVALID_FILE_HANDLE) {
      *result = VMK_INVALID_TARGET;
      return;
   }
   if (handle == NULL) {
      *result = VMK_INVALID_TARGET;
      return;
   }

   cmd->type = SCSI_RESET_COMMAND;
   SCSIResetCommand(handle, virtInfo->worldID, cmd, result);
   
   //SCSI_HandleRelease(handle);
}

/*
 * Abort all the outstanding commands on the RDM in passthrough mode
 */
static void 
VSCSI_RDMPAbortCommand(SCSIVirtInfo *virtInfo, SCSI_Command *cmd, VMK_ReturnStatus *result)
{
   uint64 handleID = virtInfo->devDesc.u.fid;
   SCSI_Handle *handle = (SCSI_Handle *)virtInfo->privateData;

   ASSERT(virtInfo->devDesc.type == VSCSI_RDMP);
   ASSERT(handleID != FS_INVALID_FILE_HANDLE);
   if (handleID == FS_INVALID_FILE_HANDLE) {
      *result = VMK_INVALID_TARGET;
      return;
   }
   if (handle == NULL) {
      *result = VMK_INVALID_TARGET;
      return;
   }

   SCSIAbortCommand(handle, virtInfo->worldID, cmd, result);
   //SCSI_HandleRelease(handle);
}

/*
 * RDM in passthrough mode registration for the vscsi switch
 */
void 
VSCSI_RDMPInit(void)
{
    VSCSI_RDMPRegister();
}

static void 
VSCSI_RDMPRegister(void)
{
   VMK_ReturnStatus status;

   status = VSCSI_RegisterDevice(VSCSI_RDMP, &VSCSI_RDMPOps);
   if (status != VMK_OK) {
      Warning("failed");
   }
}
