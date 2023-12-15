/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/************************************************************
 *
 *  generic_scsi.c
 *
 ************************************************************/

#include "vm_types.h"
#include "x86.h"
#include "vm_libc.h"
#include "vmkernel.h"
#include "splock.h"
#include "action.h"
#include "vm_asm.h"
#include "sched.h"
#include "vmk_scsi.h"
#include "world.h"
#include "memalloc.h"
#include "scsi_defs.h"
#include "host.h"
#include "mod_loader.h"
#include "config.h"
#include "timer.h"
#include "util.h"
#include "pci.h"
#include "scsi_ioctl.h"
#include "parse.h"
#include "memmap.h"
#include "libc.h"
#include "sched_sysacct.h"
#include "scsi_int.h"
#include "helper.h"
#include "vmkevent.h"
#include "vmksysinfoInt.h"
#include "sharedArea.h"
#include "vm_device_version.h"
#include "volumeCache.h"

#define LOGLEVEL_MODULE SCSI
#define LOGLEVEL_MODULE_LEN 4
#include "log.h"

static void SCSI_GenericCommand(SCSI_Handle *handle, SCSI_Command *cmd, 
                           SCSI_Status *scsiStatus, SCSISenseData *sense,
		           Bool *done);

/*
 *----------------------------------------------------------------------
 *
 * SCSI_InitialErrorCheckOfCommand
 *
 *    Weed out virtual SCSI commands that are badly formed, not supported,
 *    ignored, or otherwise cannot be issued at the current time.  This
 *    includes commands that cannot be issued while a reservation is active.
 *    This routine should be called in VSCSI_VirtCommand() implementations
 *    before calling VSCSI_GenericCommand().  This routine was added as a
 *    fix for PR 24482.  Any code that is added to check SCSI command
 *    validity must be added to this routine rather than either of those
 *    routines.
 * 
 * Results:
 *
 *	*done is set to TRUE if the command is complete and
 *	VSCSI_DoCommandComplete() must be called with the returned contents
 *	of scsiStatus and sense.  It is set to FALSE if the command still
 *	needs to be executed, in which case scsiStatus and sense are not set.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
void
SCSI_InitialErrorCheckOfCommand(SCSI_Command *cmd,
                                Bool activeReservation, 
                                SCSI_Status *scsiStatus,
                                SCSISenseData *sense, 
                                Bool *done)
{ 
   uint32 hostStatus = SCSI_HOST_OK;
   uint32 deviceStatus = SDSTAT_GOOD; 

   memset(sense, 0, sizeof(*sense));
   *done = TRUE;

   /*
    * Check for error conditions in the commands, or unsupported commands
    */
   switch (cmd->cdb[0]) {
   case SCSI_CMD_INQUIRY: {
      SCSIInquiryCmd* inqCmd = (SCSIInquiryCmd*) cmd->cdb;
      if (inqCmd->evdp || inqCmd->cmddt) {
         Log("INQUIRY request with %s set", inqCmd->evdp ? "EVDP" : "CmdDt");
         SCSI_IllegalRequest(sense, TRUE, 1);
         deviceStatus = SDSTAT_CHECK;
      } else { 
         /*
          * Valid command, it will be processed by the caller.
          */
         *done = FALSE; 
      }
      break;
   }
   case SCSI_CMD_REQUEST_SENSE: {
      /*
       * Valid command, it will be processed by the caller.
       */
      *done = FALSE;
      break;
   }
   case SCSI_CMD_READ_CAPACITY: {
      SCSIReadCapacityCmd* cdb = (SCSIReadCapacityCmd*) cmd->cdb;	 
      uint32 length = SG_TotalLength(&cmd->sgArr);

      if (cdb->rel || cdb->pmi || length < sizeof(SCSIReadCapacityResponse)) {
         SCSI_IllegalRequest(sense, TRUE, (cdb->rel) ? 1 : 8);
         deviceStatus = SDSTAT_CHECK;
      } else { 
         /*
          * Valid command, it will be processed by the caller.
          */
         *done = FALSE;
      }
      break;
   }
   case SCSI_CMD_READ_CAPACITY16: {
      SCSIReadCapacity16Cmd* cdb = (SCSIReadCapacity16Cmd*) cmd->cdb;	 
      uint32 length = SG_TotalLength(&cmd->sgArr);

      if (cdb->action != 0x10 || length < sizeof(SCSIReadCapacity16Response)) {
	 SCSI_IllegalRequest(sense, TRUE, 1);
	 deviceStatus = SDSTAT_CHECK;
      } else if (cdb->rel || cdb->pmi) {
	 SCSI_IllegalRequest(sense, TRUE, 14);
	 deviceStatus = SDSTAT_CHECK;
      } else { 
         /*
          * Valid command, it will be processed by the caller.
          */
         *done = FALSE;
      }
      break;
   }
   case SCSI_CMD_FORMAT_UNIT:
   case SCSI_CMD_VERIFY:
   case SCSI_CMD_SYNC_CACHE:	 
   case SCSI_CMD_TEST_UNIT_READY:
   case SCSI_CMD_START_UNIT:
      /*
       * These commands are treated as noops. Mark them as completed
       * with good status.
       */
      if (cmd->cdb[0] == SCSI_CMD_START_UNIT) {
         if (cmd->cdb[4] & 0x01) {
            Log("START_UNIT cmd issued to virt disk");
         } else {
            /*
	     * We may want to reject the STOP_UNIT command because
	     * the virt disk is not being stopped. 
	     */
            Log("STOP_UNIT cmd issued to virt dis");
         }
      }
      break;
   case SCSI_CMD_MODE_SENSE: {
      // if this causes performance issues, we may need to revisit
      // this and give resonable replies to some of the page requests
      LOG_ONLY(SCSIModeSenseCmd* cdb = (SCSIModeSenseCmd*) cmd->cdb;)
      LOG(0, "SCSI_CMD_MODE_SENSE for pagecode (0x%x) pagectl (0x%x)",
	  cdb->page, cdb->pcf);
      SCSI_IllegalRequest(sense, TRUE, 2);
      deviceStatus = SDSTAT_CHECK;
      break;
   }
   case SCSI_CMD_READ10:
   case SCSI_CMD_WRITE10:
   case SCSI_CMD_READ6:
   case SCSI_CMD_WRITE6:
   case SCSI_CMD_READ16:
   case SCSI_CMD_WRITE16:
      /* Limit checks are performed in VSCSI_GenericCommand, since
       * we do not yet have the true block numbers for RAW
       * partitions yet. Actual read/write is done by the caller.
       */
      *done = FALSE;
      break;
   case SCSI_CMD_RESERVE_UNIT: 
   case SCSI_CMD_RELEASE_UNIT: 
      /*
       * Valid commands. They will be processed by the caller.
       */
      *done = FALSE;
      break; 
   case SCSI_CMD_READ_BUFFER:
   case SCSI_CMD_WRITE_BUFFER:
   case SCSI_CMD_MEDIUM_REMOVAL:
      /*
       * do not log invalid opcode message for these commands
       */
      SCSI_InvalidOpcode(sense, TRUE);
      deviceStatus = SDSTAT_CHECK;
      break;
   default:
      /* 
       * Generate an invalid opcode error for the rest
       * of the SCSI commands.
       */
      Log("Invalid Opcode (0x%x) ", cmd->cdb[0]);

      SCSI_InvalidOpcode(sense, TRUE);
      deviceStatus = SDSTAT_CHECK;
      break;
   }

   /*
    * If the command is valid, check if it can be issued while 
    * a reservation is active.
    */ 
   if ( (*done == FALSE) && (activeReservation == TRUE) ) {   
      if ((cmd->cdb[0] == SCSI_CMD_TEST_UNIT_READY)  ||
        (cmd->cdb[0] == SCSI_CMD_INQUIRY)          ||
        (cmd->cdb[0] == SCSI_CMD_REQUEST_SENSE)) {
	/*
	 * These are valid commands during a reservation. They will be 
	 * processed by the caller.
	 */
         LOG(1, "SCSI Command 0x%x is reserved, command allowed", cmd->cdb[0]);
      } else {
         LOG(1, "SCSI Command 0x%x, issued  while diskd is reserved, command rejected", 
	    cmd->cdb[0]);
         deviceStatus = SDSTAT_RESERVATION_CONFLICT;
	 *done = TRUE;
      }
   } 

   if (*done == TRUE) {
      *scsiStatus = SCSI_MAKE_STATUS(hostStatus, deviceStatus); 
   }
   return;
}

/*
 * Emulate a SCSI command on the virtual SCSI device specified by the handle,
 * if it is not a read or a write.  The handle references either a VMFS
 * file or a partition of a disk. Return *done = TRUE if the command has
 * handled by this function (i.e. was not a read or write).
 *
 * NOTE:
 * 	SCSI cmd error checking should not be done in this routine. It should
 * 	be performed in the SCSI_InitialErrorCheckOfCommand() routine. That 
 * 	routine	is common to the virtual and physical device paths.
 */
void
SCSI_GenericCommand(SCSI_Handle *handle, SCSI_Command *cmd,
                     SCSI_Status *scsiStatus, SCSISenseData *sense, Bool *done)
{
   uint32 hostStatus = SCSI_HOST_OK;
   uint32 deviceStatus = SDSTAT_GOOD;

   *done = TRUE;
   memset(sense, 0, sizeof(*sense));

   ASSERT(handle->target->lun == 0);

   switch (cmd->cdb[0]) {
      case SCSI_CMD_INQUIRY: {
	 DEBUG_ONLY(SCSIInquiryCmd* inqCmd = (SCSIInquiryCmd*) cmd->cdb);
	 uint32 length = SG_TotalLength(&cmd->sgArr);

         SCSIInquiryResponse inqResponse;
         uint32 copyLength = length < sizeof(inqResponse) ? 
                              length : sizeof(inqResponse);

	 ASSERT(!(inqCmd->evdp || inqCmd->cmddt));

         memset(&inqResponse, 0, sizeof(inqResponse));
         inqResponse.pqual = SCSI_PQUAL_CONNECTED;
	 inqResponse.devclass = SCSI_CLASS_DISK;
	 inqResponse.ansi = SCSI_ANSI_SCSI2;

         // account two reserved bytes
         inqResponse.optlen += 2;

	 inqResponse.rmb = FALSE;
	 inqResponse.rel = FALSE;	// rel. addr. w/ linked cmds 
	 inqResponse.w32 = TRUE;	// 32-bit wide SCSI
	 inqResponse.w16 = TRUE;	// 16-bit wide SCSI
	 inqResponse.sync = TRUE;	// synchronous transfers
	 inqResponse.link = FALSE; 	// linked commands (XXX not yet)
	 inqResponse.que = TRUE;	// tagged commands
	 inqResponse.sftr = TRUE;	// soft reset on RESET condition
	 inqResponse.optlen += 2;

	 memcpy(inqResponse.manufacturer, "VMware            ", 
		sizeof(inqResponse.manufacturer));
	 inqResponse.optlen += sizeof (inqResponse.manufacturer);

	 memcpy(inqResponse.product, "Virtual disk            ", 
                sizeof (inqResponse.product));
	 inqResponse.optlen += sizeof(inqResponse.product);

	 memcpy(inqResponse.revision, "1.0             ", 
                sizeof(inqResponse.revision));
	 inqResponse.optlen += sizeof(inqResponse.revision);

         if (copyLength) {
	    if (!Util_CopySGData(&inqResponse, &cmd->sgArr, UTIL_COPY_TO_SG, 
                                 0, 0, copyLength)) {
	       SCSI_IllegalRequest(sense, TRUE, 4);
	       deviceStatus = SDSTAT_CHECK;
	    }
         }
	 break;
      }
      case SCSI_CMD_REQUEST_SENSE: {
	 uint32 length = SG_TotalLength(&cmd->sgArr);

         memset(sense, 0, sizeof(*sense));
         LOG(0, "SENSE REQUEST w/o valid sense data available");
         if (length > 0) {
            Util_CopySGData(sense, &cmd->sgArr, UTIL_COPY_TO_SG, 0, 0,
			    length < sizeof(*sense) ? length : sizeof(*sense));
         }
	 break;
      }
      case SCSI_CMD_READ_CAPACITY: {
	 SCSI_Target *target;
	 uint64 lastSector;

	 DEBUG_ONLY(SCSIReadCapacityCmd* cdb = (SCSIReadCapacityCmd*) cmd->cdb);	 
	 DEBUG_ONLY(uint32 length = SG_TotalLength(&cmd->sgArr));

	 ASSERT(!(cdb->rel || cdb->pmi || cdb->lbn || length < sizeof(SCSIReadCapacityResponse)));

	 SCSIReadCapacityResponse cp;
	 target = handle->target;
	 cp.blocksize = ByteSwapLong(target->blockSize);
	 lastSector = target->partitionTable[handle->partition].entry.numSectors - 1;
	 cp.lbn = ByteSwapLong(MIN(lastSector,SCSI_READ_CAPACITY_MAX_LBN));
	 if (!Util_CopySGData(&cp, &cmd->sgArr, UTIL_COPY_TO_SG, 0, 0,
			        sizeof(SCSIReadCapacityResponse))) {
	    SCSI_IllegalRequest(sense, TRUE, 1);
	    deviceStatus = SDSTAT_CHECK;
	 }
	 break;
      }
      case SCSI_CMD_READ_CAPACITY16: {
	 SCSI_Target *target;
	 SCSIReadCapacity16Response cp;
	 DEBUG_ONLY(SCSIReadCapacity16Cmd* cdb = (SCSIReadCapacity16Cmd*) cmd->cdb);	 
	 DEBUG_ONLY(uint32 length = SG_TotalLength(&cmd->sgArr));

	 ASSERT(!(cdb->action != 0x10 ||
		  cdb->rel ||
		  cdb->pmi ||
		  length < sizeof(SCSIReadCapacity16Response))); 

	 target = handle->target;
	 cp.blocksize = ByteSwapLong(target->blockSize);
	 cp.lbn = ByteSwap64(target->partitionTable[handle->partition].entry.numSectors - 1);

	 if (!Util_CopySGData(&cp, &cmd->sgArr, UTIL_COPY_TO_SG, 0, 0,
			        sizeof(SCSIReadCapacity16Response))) {
	    SCSI_IllegalRequest(sense, TRUE, 1);
	    deviceStatus = SDSTAT_CHECK;
	 }
	 break;
      }
      case SCSI_CMD_READ10:
      case SCSI_CMD_WRITE10: {
            SCSI_Target *target = handle->target;
            SCSIReadWrite10Cmd *rwCmd = (SCSIReadWrite10Cmd *)cmd->cdb;
	    /* Make blockOffset be uint64 so there won't be any overflow
	     * if numBlocks is large and blockOffset is close to 4G. */
            uint64 blockOffset = ByteSwapLong(rwCmd->lbn);
            uint32 numBlocks = ByteSwapShort(rwCmd->length);
            uint32 partEndSector = target->partitionTable[handle->partition].entry.startSector +
               target->partitionTable[handle->partition].entry.numSectors;

	    /* Make sure access does go past end of partition. */
            if (blockOffset + numBlocks > partEndSector) {
               Warning("%s10 past end of virtual device on %s:%d:%d:%d",
                       cmd->cdb[0]==SCSI_CMD_READ10 ? "READ" : "WRITE",
                       handle->adapter->name, target->id, target->lun,
		       handle->partition);
               SCSI_IllegalRequest(sense, TRUE, 1);
               deviceStatus = SDSTAT_CHECK;
            }
            else {
               /* The actual read/write is done by caller */
               *done = FALSE;
            }
            break;
      }
      case SCSI_CMD_READ6:
      case SCSI_CMD_WRITE6: {
            SCSI_Target *target = handle->target;
            uint8* rw = (uint8*) cmd->cdb;
            uint32 blockOffset = (((rw[1]&0x1f)<<16)|(rw[2]<<8)|rw[3]);
            uint32 numBlocks = ((rw[4] == 0 ? 256 : rw[4]));
            /* This is the number of blocks we report as a reply to READ CAPACITY */
            uint32 partEndSector = target->partitionTable[handle->partition].entry.startSector +
               target->partitionTable[handle->partition].entry.numSectors;
            
            /* only allow access to sectors 0 through partEndSector-1 */
            if (blockOffset + numBlocks > partEndSector) {
               Warning("%s6 past end of virtual device on %s:%d:%d:%d",
                       cmd->cdb[0]==SCSI_CMD_READ6 ? "READ" : "WRITE",
                       handle->adapter->name, target->id, target->lun, handle->partition);
               SCSI_IllegalRequest(sense, TRUE, 1);
               deviceStatus = SDSTAT_CHECK;
            }
            else {
               /* The actual read/write is done by caller */
               *done = FALSE;
            }
            break;
      }
      case SCSI_CMD_READ16:
      case SCSI_CMD_WRITE16: {
            SCSI_Target *target = handle->target;
            SCSIReadWrite16Cmd *rwCmd = (SCSIReadWrite16Cmd *)cmd->cdb;
            uint64 blockOffset = ByteSwap64(rwCmd->lbn);
            uint32 numBlocks = ByteSwapLong(rwCmd->length);
            uint64 partEndSector = target->partitionTable[handle->partition].entry.startSector +
               target->partitionTable[handle->partition].entry.numSectors;

	    if (blockOffset + numBlocks > partEndSector) {
	       /* Make sure access does not go past end of partition. */
               Warning("%s16 past end of virtual device on %s:%u:%u:%u",
                       cmd->cdb[0]==SCSI_CMD_READ16 ? "READ" : "WRITE",
                       handle->adapter->name, target->id, target->lun,
		       handle->partition);
               SCSI_IllegalRequest(sense, TRUE, 2);
               deviceStatus = SDSTAT_CHECK;
            } else if (rwCmd->rel) {
	       /* We don't support linked commands */
               SCSI_IllegalRequest(sense, TRUE, 1);
               deviceStatus = SDSTAT_CHECK;
	    }
            else {
               /* The actual read/write is done by caller */
               *done = FALSE;
            }
            break;
      }
      case SCSI_CMD_RESERVE_UNIT: {
	 DEBUG_ONLY(SCSIReserveCmd *cdb = (SCSIReserveCmd *)cmd->cdb);
	 ASSERT(cdb->opcode == SCSI_CMD_RESERVE_UNIT);
	 ASSERT(cdb->tparty == 0);
	 ASSERT(cdb->lun == 0);
	 ASSERT(cdb->ext == 0);
	 break;
      }

      case SCSI_CMD_RELEASE_UNIT: {
	 DEBUG_ONLY(SCSIReserveCmd *cdb = (SCSIReserveCmd *)cmd->cdb);
	 ASSERT(cdb->opcode == SCSI_CMD_RELEASE_UNIT);
	 ASSERT(cdb->tparty == 0);
	 ASSERT(cdb->lun == 0);
	 ASSERT(cdb->ext == 0);
	 break;
      }

      default:
         Log("Invalid Opcode (0x%x) for for %s:%d:%d",
             cmd->cdb[0], handle->adapter->name, 
             handle->target->id, handle->target->lun);
         NOT_IMPLEMENTED();
   }

   *scsiStatus = SCSI_MAKE_STATUS(hostStatus, deviceStatus);
}

/*
 *----------------------------------------------------------------------
 *
 *  SCSI_GetXferData --
 *
 *      Fill in length of the transfer indicated by a SCSI command,
 *	and also the sector position.  Both are set to zero for a
 *	non-block device, or a non-read/write command.
 *
 * Results: 
 *	None
 *
 * Side effects:
 *	Fills in cmd->dataLength and cmd->sectorPos
 *
 *----------------------------------------------------------------------
 */
void
SCSI_GetXferData(SCSI_Command *cmd, uint8 devClass, uint32 blockSize)
{
   cmd->dataLength = 0;
   cmd->sectorPos = 0;

   // Note that for non-block devices, target->blockSize is zero, so the
   // transfer length returned will typically be 0, except in the case of
   // SCSI_CLASS_TAPE.

   switch (cmd->cdb[0]) {
   case SCSI_CMD_READ10:
   case SCSI_CMD_WRITE10: {      
      SCSIReadWrite10Cmd *nCdb = (SCSIReadWrite10Cmd *)cmd->cdb;
      cmd->dataLength = blockSize * (uint32)(ByteSwapShort( nCdb->length));
      cmd->sectorPos = ByteSwapLong(nCdb->lbn);
      break;
   }
   case SCSI_CMD_READ6:
   case SCSI_CMD_WRITE6: {
      uint8* nrw = (uint8*) cmd->cdb;
      if (devClass != SCSI_CLASS_TAPE) {
         cmd->dataLength = blockSize * (uint32)((nrw[4] == 0 ? 256 : nrw[4]));
	 cmd->sectorPos = (((((uint32)nrw[1])&0x1f)<<16)|(((uint32)nrw[2])<<8)|nrw[3]);
      } else if (!(nrw[1] & 0x1)) {
	 // Sequential devices (tape) have a special format for the READ
	 // command.
         cmd->dataLength = ((((uint32)nrw[2])<<16) + (((uint32)nrw[3])<<8) + nrw[4]);
      }
      break;
   }
   case SCSI_CMD_READ16:
   case SCSI_CMD_WRITE16: {      
      if (devClass != SCSI_CLASS_TAPE) {
	 SCSIReadWrite16Cmd *nCdb = (SCSIReadWrite16Cmd *)cmd->cdb;
	 cmd->dataLength = blockSize * (uint32)(ByteSwapLong(nCdb->length));
	 cmd->sectorPos = ByteSwap64(nCdb->lbn);
      } else if (!(cmd->cdb[1] & 0x1)) {
	 // Sequential devices (i.e. tapes) have a special format for READ16
	 cmd->dataLength = (((uint32)cmd->cdb[12]) << 16) +
	                   (((uint32)cmd->cdb[13]) << 8) + cmd->cdb[14];
      }
      break;
   }

   // All other commands we do not care
   default:
      ;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * SCSI_GenericCommandID --
 *
 *	This function is solely called from linux_block to force
 *	emulation of a SCSI command for RAW block devices (which
 *	can only be disks).
 *	The call path so far will be SCSIQueueCommand ->
 *	SCSIIssueCommand -> LinuxBlockCommand -> SCSI_GenericCommandID.
 *	Thus we have already called SCSICheckForCachedSense(), but not
 *	VSCSI_InitialErrorCheckOfCommand and VSCSI_GenericCommand since
 *	the command was for a RAW device (see SCSIQueueCommand).
 *
 * Results: 
 *      If the command was handled, *done will be set TRUE, which
 *      will always be the case since VSCSI_InitialErrorCheckOfCommand()
 *      will return IllegalCommand for all unknown commands.
 *
 *---------------------------------------------------------------------- 
 */
void
SCSI_GenericCommandID(SCSI_HandleID handleID, SCSI_Command *cmd, 
                      SCSI_Status *scsiStatus, SCSISenseData *sense,
		      Bool *done)
{
   SCSI_Handle *handle = SCSI_HandleFind(handleID);
   if (handle == NULL) {
      Warning("Couldn't find handle 0x%x", handleID);
      *done = FALSE;
      return;
   }

   /* We have already checked for cached sense (see funcion description) */
   SCSI_InitialErrorCheckOfCommand(cmd, FALSE, scsiStatus, sense, done);
   if (!*done) {
      SCSI_GenericCommand(handle, cmd, scsiStatus, sense, done);
   }
   SCSI_HandleRelease(handle);
}

/*
 *----------------------------------------------------------------------
 *
 * SCSICheckForCachedSense
 *
 *    Emulate REQUEST_SENSE for a virtual disk if we have valid sense data
 *    in the handle since it means we obtained it earlier without passing it
 *    to the guest. If the handle does not have any valid sense, we let the
 *    command pass and either the RAW device or SCSIGenericCommand() will
 *    then deal with it.
 *
 *    XXX: What if guest had autosense enabled - then we should not do
 *    this!!! The saving of sense should move to the emulation layer!!!
 *
 * Results:
 *      *done is set to TRUE if the command is complete and
 *      VSCSI_DoCommandComplete() must be called with the returned contents
 *      of scsiStatus and sense.  It is set to FALSE if the command still
 *      needs to be executed.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 *
 *
 */
void
SCSICheckForCachedSense(uint8 *senseBuffer, SCSI_Command *cmd,
                        SCSI_Status *scsiStatus, SCSISenseData *sense,
                        uint32 *bytesXferred, Bool *done)
{
   memset(sense, 0, sizeof(*sense));
   *bytesXferred = 0;

   if ((cmd->cdb[0] == SCSI_CMD_REQUEST_SENSE) && (senseBuffer[0] != 0)) {
      uint32 length = SG_TotalLength(&cmd->sgArr);

      /* Clear sense data after it is obtained - see comment below */
      senseBuffer[0] = 0;
      if (sense->optLen + 8 < length) {
         length = sense->optLen + 8;
      }
      *bytesXferred = length < sizeof(*sense) ? length : sizeof(*sense);
      Util_CopySGData(sense, &cmd->sgArr, UTIL_COPY_TO_SG, 0, 0, *bytesXferred);

      *done = TRUE;
      *scsiStatus = SCSI_MAKE_STATUS(SCSI_HOST_OK, SDSTAT_GOOD);
      return;
   }

   /* The SCSI spec says "Sense data shall be cleared upon receipt of any
    * subsequent I/O process (including REQUEST SENSE) to the same I_T_x
    * nexus.". Avoid a memset for each command by simply clearing the
    * first sense byte (matching the check above).
    */
   senseBuffer[0] = 0;
   *done = FALSE;
   return;
}

/*
 * Set the sense buffer with info indicating an illegal SCSI request.
 * You must also return a device status of SDSTAT_CHECK for the SCSI
 * command in order for the sense buffer to be examined.
 */
void
SCSI_IllegalRequest(SCSISenseData *sense, Bool isCommand, uint16 byteOffset)
{
   sense->valid = TRUE;
   sense->error = SCSI_SENSE_ERROR_CURCMD;
   sense->key = SCSI_SENSE_KEY_ILLEGAL_REQUEST;
   sense->optLen = 10;	       // 10 bytes gets the SKSV info
   sense->code = 0x24;	       // error in command block
   sense->xcode = 0;
   sense->sksv = TRUE;	       // sense key specific data is valid
   sense->cd = isCommand;
   sense->epos = byteOffset;
}

/*
 * Set the sense buffer with info indicating an illegal SCSI opcode.
 * You must also return a device status of SDSTAT_CHECK for the SCSI
 * command in order for the sense buffer to be examined.
 */
void
SCSI_InvalidOpcode(SCSISenseData *sense, Bool isCommand)
{
   sense->valid = TRUE;
   sense->error = SCSI_SENSE_ERROR_CURCMD;
   sense->key = SCSI_SENSE_KEY_ILLEGAL_REQUEST;
   sense->optLen = 10;	       // 10 bytes gets the SKSV info
   sense->code = 0x20;	       // invalid opcode
   sense->xcode = 0;
   sense->sksv = TRUE;	       // sense key specific data is valid
   sense->cd = isCommand;
   sense->epos = 0;
}
