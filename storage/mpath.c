/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * These are the SCSI functions that are specifically related to
 * multipathing in the vmkernel.
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "libc.h"
#include "splock.h"
#include "vmk_scsi.h"
#include "scsi_int.h"
#include "helper.h"
#include "host_dist.h"
#include "mod_loader.h"
#include "world.h"
#include "memmap.h"

#define LOGLEVEL_MODULE SCSI
#define LOGLEVEL_MODULE_LEN 4
#include "log.h"

static void SCSIRequestHelperFailoverInt(void *target);

/*
 * Add a path to the list of paths to a specified target.
 * Requires that adapter lock is held, unless target is being initialized.
 */
void
SCSIAddPath(SCSI_Target *target, SCSI_Adapter *adapter, uint32 tid, uint32 lun)
{
   SCSI_Path *path, *p;

   path = Mem_Alloc(sizeof(SCSI_Path));
   ASSERT(path != NULL);
   path->adapter = adapter;
   path->id = tid;
   path->lun = lun;
   path->state = SCSI_PATH_ON;
   path->active = 0;
   path->flags = 0;
   path->next = NULL;
   path->target = target;
   if (target->paths == NULL) {
      target->paths = path;
   }
   else {
      for (p = target->paths; p->next != NULL; p = p->next) {
	 ;
      }
      p->next = path;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * SCSIRemovePath
 *
 *      Remove the path specified by adapter/tid/lun from the path list 
 *      for the given target. This routine must be called with the
 *      target's adapter lock held.
 *
 * Results: 
 *	TRUE if the path was removed
 *	FALSE otherwise
 *
 * Side effects:
 *	none
 *
 *----------------------------------------------------------------------
 */
Bool
SCSIRemovePath(SCSI_Target *target, SCSI_Adapter *adapter, uint32 tid,
	       uint32 lun)
{
   SCSI_Path *path, *ppath;

   ASSERT(SP_IsLocked(&target->adapter->lock));
   ppath = NULL;

   /*
    * Cannot remove a secondary path from the target if it is active.
    * A command from EvaluateAdapter or the failover code may be pending
    * on the secondary path.
    */
   if (target->refCount > 0) {
      return FALSE;
   }

   for (path = target->paths; path != NULL; path = path->next) {
      if (path->adapter == adapter && path->id == tid && path->lun == lun) {
	 if (ppath == NULL) {
	    // Assert that we're not removing the last path.
	    ASSERT(path->next != NULL);
	    target->paths = path->next;
	 }
	 else {
	    ppath->next = path->next;
	 }
	 if (target->activePath == path) {
	    target->activePath = target->paths;
	 }
	 if (target->preferredPath == path) {
	    target->preferredPath = target->paths;
	 }
	 Mem_Free(path);
	 return TRUE;
      }
      ppath = path;
   }
   return FALSE;
}


/*
 *----------------------------------------------------------------------
 *
 * SCSITargetHasPath
 * 
 *	Check if the given target contains a path with the given
 *	adapter/tid/lun.
 *
 * Results: 
 *	TRUE if the target contains such a path
 *	FALSE otherwise
 *
 * Side effects:
 *	none
 *
 *----------------------------------------------------------------------
 */
Bool
SCSITargetHasPath(SCSI_Target *target, SCSI_Adapter *adapter, uint32 tid,
	       uint32 lun)
{
   SCSI_Path *path;

   ASSERT(SP_IsLocked(&target->adapter->lock));

   for (path = target->paths; path != NULL; path = path->next) {
      if (path->adapter == adapter && path->id == tid && path->lun == lun) {
	 return TRUE;
      }
   }
   return FALSE;
}

/*
 * Mark a path as standby.  The path appears to be working but the target
 * device (storage controller) may have to be activated before being used.
 */
void 
SCSIMarkPathStandby(SCSI_Path *path)
{
   ASSERT(path != NULL);
   ASSERT(path->target != NULL);
   ASSERT(path->target->adapter != NULL);
   ASSERT(SP_IsLocked(&path->target->adapter->lock));
   ASSERT( path->target->flags & SCSI_SUPPORTS_MANUAL_SWITCHOVER );
   if (path->state != SCSI_PATH_STANDBY) {
      SCSICondRelLog(SCSI_LOG_MULTI_PATH, "Marking path %s:%d:%d as standby",
		     path->adapter->name, path->id, path->lun);
      path->state = SCSI_PATH_STANDBY;
      if (path->target->flags & SCSI_DEV_SVC) {
         path->notreadyCount = 0;
      }
   } else if (path->target->flags & SCSI_DEV_SVC) {
      if (path->notreadyCount < 
          (CONFIG_OPTION(DISK_SVC_NOT_READY_RETRIES))) {
         path->notreadyCount++;
      } else {
         Warning("NotReady SVC path %s:%d:%d has been retried %d times. Marking as dead.",
	    path->adapter->name, path->id, path->lun, path->notreadyCount);
         SCSIMarkPathDead(path);
      }
   }
}


/*
 * Mark a path as available.  It is enabled and working, and the target
 * device is active.
 */
void 
SCSIMarkPathOn(SCSI_Path *path)
{ 
   ASSERT(SP_IsLocked(&path->target->adapter->lock)); 
   if ( path->state != SCSI_PATH_ON) {
      SCSICondRelLog(SCSI_LOG_MULTI_PATH,"Marking path %s:%d:%d as on",
		     path->adapter->name, path->id, path->lun);
      path->state = SCSI_PATH_ON;
   }
}

/*
 * Mark a path as dead.
 */
void
SCSIMarkPathDead(SCSI_Path *path)
{
   ASSERT(SP_IsLocked(&path->target->adapter->lock));
   SCSICondRelLog(SCSI_LOG_MULTI_PATH,"Marking path %s:%d:%d as dead",
		  path->adapter->name, path->id, path->lun);
   path->state = SCSI_PATH_DEAD; 
   /* make sure we re-register when this path comes back */
   path->flags &= ~SCSI_PATH_REGISTRATION_DONE;
}

/*
 * Mark a path that was dead as working now.
 */
void
SCSIMarkPathUndead(SCSI_Path *path)
{ 
   ASSERT(SP_IsLocked(&path->target->adapter->lock));
   ASSERT(path->state == SCSI_PATH_DEAD);

   if (path->target->flags & SCSI_SUPPORTS_MANUAL_SWITCHOVER) {
      SCSIMarkPathStandby(path);
   } else {
      SCSIMarkPathOn(path);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * SCSIIsActivePassiveSANDevice --
 *
 *      Determine if the SAN device supports Active/Passive Path failover 
 *	Storage Processors
 *
 *
 * Results: 
 *	TRUE if the modelName indicates a SAN device with Active/Passive 
 *	Storage Processors
 *
 * Side effects:
 *	none
 *
 *----------------------------------------------------------------------
 */
static Bool
SCSIIsActivePassiveSANDevice(char *modelName)
{
   char *configString;
   char *remainingStr;
   char *thisAPModelName;
   char *allAPModelNames;

   configString = (char *)Config_GetStringOption(CONFIG_DISK_ACTIVE_PASSIVE_FAILOVER_SANS);
   if ((configString == NULL) || (strlen(configString) == 0)) {
      return( FALSE );
   }
   
   /*
    * The configString cannot be modified, so make a copy of it
    * to pass to strchr().
    */
   allAPModelNames = (char *)Mem_Alloc(strlen(configString) + 1);
   if (allAPModelNames == NULL) {
      Warning("No memory.");
      return( FALSE );
   }
   memset(allAPModelNames, 0, strlen(configString) + 1);
   memcpy(allAPModelNames, configString, strlen(configString));

   remainingStr = allAPModelNames;
   while (remainingStr != NULL) {
      thisAPModelName = remainingStr; 

      remainingStr = strchr(remainingStr, ':'); 
      if (remainingStr != NULL ) {
         *remainingStr = (char)NULL;
	 remainingStr++;
      } 

      if (strncmp(modelName, thisAPModelName,strlen(thisAPModelName)) == 0) {
         Mem_Free(allAPModelNames);
         return( TRUE );
      } 
   }
   Mem_Free(allAPModelNames);
   return( FALSE );
} 

/*
 *----------------------------------------------------------------------
 *
 * SCSIFAStTDevice --
 *
 *      Determine if the SAN device is part of the IBM FAStT family. 
 *
 * Results: 
 *	TRUE if the modelName indicates a IBM FAStT SAN
 *
 * Side effects:
 *	none
 *
 *----------------------------------------------------------------------
 */
static Bool
SCSIFAStTDevice(char *vendorName, char *modelName)
{
   /*
    * IBM FastT
    */
   if ((strncmp(vendorName, "IBM", 3) == 0) &&
       ((strncmp(modelName, "1742", 4) == 0) ||	/* FAStT 700/900 */
        (strncmp(modelName, "3542", 4) == 0) ||	/* FAStT 200 */
        (strncmp(modelName, "3552", 4) == 0) ||	/* FAStT 500 */
        (strncmp(modelName, "1722", 4) == 0)   	/* FAStT 600 */ 
       )
      ) {
      return( TRUE );
   }

   /*
    * StorageTek - behaves like the FAStT
    */
   if (((strncmp(vendorName, "STK", 3) == 0) ||
        (strncmp(vendorName, "LSI", 3) == 0)) &&
       ((strncmp(modelName, "OPENstorage 9176", 16) == 0) ||
        (strncmp(modelName, "OPENstorage D173", 16) == 0) ||
        (strncmp(modelName, "OPENstorage D178", 16) == 0) ||
        (strncmp(modelName, "OPENstorage D210", 16) == 0) ||
        (strncmp(modelName, "OPENstorage D220", 16) == 0) ||
        (strncmp(modelName, "OPENstorage D240", 16) == 0) ||
        (strncmp(modelName, "OPENstorage D280", 16) == 0) ||
        (strncmp(modelName, "BladeCtlr BC82", 14) == 0) ||
        (strncmp(modelName, "BladeCtlr BC84", 14) == 0) ||
        (strncmp(modelName, "BladeCtlr BC88", 14) == 0) ||
        (strncmp(modelName, "BladeCtlr B210", 14) == 0) ||
        (strncmp(modelName, "BladeCtlr B220", 14) == 0) ||
        (strncmp(modelName, "BladeCtlr B240", 14) == 0) ||
        (strncmp(modelName, "BladeCtlr B280", 14) == 0))
      ) {
      return( TRUE );
   }

   return( FALSE );
} 

/*
 *----------------------------------------------------------------------
 *
 * SCSISVCDevice --
 *
 *      Determine if the SAN device is an IBM SVC
 *
 * Results: 
 *	TRUE if the modelName indicates a IBM SVC
 *
 * Side effects:
 *	none
 *
 *----------------------------------------------------------------------
 */
static Bool 
SCSISVCDevice(char *vendorName, char *modelName)
{
   /*
    * IBM SVC
    */
   if (((strncmp(vendorName, "IBM", 3) == 0) &&
        (strncmp(modelName, "2145", 4) == 0)
       )
      ) {
      return( TRUE );
   }
   return( FALSE );
}

/*
 *----------------------------------------------------------------------
 *
 * SCSIFAStTModeSenseCommand --
 *
 *    Issue the MODE SENSE command to the IBM FAStT device 
 * attached to the given path and request the contents of the 
 * give page (pageCode) and sub page (subPageCode). Use the
 * 10 byte version of the MODE SENSE command because it is necessary
 * to support pages with a size greater than 0xFF bytes.
 *
 * Results: 
 *      contents of the given page/subpage are returned in mp.
 *	VMK_OK on success
 *	VMK_* on error
 *
 * Side effects:
 *	none
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
SCSIFAStTModeSenseCommand(SCSI_Handle *handle, SCSI_Path *path, 
int pageCode, int subPageCode, int mpLen, unsigned char *mp)
{
   SCSI_Command *cmd = NULL;
   VMK_ReturnStatus status;
 
   ASSERT(handle != NULL); 

   cmd = Mem_Alloc(sizeof(SCSI_Command));
   ASSERT(cmd);
   memset(cmd, 0, sizeof(SCSI_Command));

   cmd->sgArr.length = 1;
   cmd->sgArr.addrType = SG_MACH_ADDR;
   cmd->sgArr.sg[0].addr = VMK_VA2MA((VA)mp);
   cmd->sgArr.sg[0].length = mpLen;
   cmd->cdbLength = 10;
   cmd->dataLength = 0; 

   cmd->cdb[0] = SCSI_CMD_MODE_SENSE10;
   cmd->cdb[1] = 0x0;                          // return block descriptors 
   cmd->cdb[2] = pageCode;
   cmd->cdb[3] = subPageCode;
   cmd->cdb[4] = 0x0;
   cmd->cdb[5] = 0x0;
   cmd->cdb[6] = 0x0;
   cmd->cdb[7] = (mpLen >> 8) & 0xFF;          // MSB
   cmd->cdb[8] = mpLen & 0xFF; 	               // LSB
   cmd->cdb[9] = 0x0;

   cmd->flags = SCSI_CMD_BYPASSES_QUEUE | SCSI_CMD_IGNORE_FAILURE; 
   status = SCSISyncCommand(handle, cmd, path, TRUE);
   Mem_Free(cmd); 

   if ((status != VMK_OK) &&
       (status != VMK_NOT_READY)) {
      ASSERT( status != VMK_WOULD_BLOCK );
      Warning("SCSIFAStTModeSenseCommand on %s:%d:%d returned %s", 
         path->adapter->name, path->id, path->lun,
	 VMK_ReturnStatusToString(status));
   }
   return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * SCSIFAStTModeSelectCommand --
 *
 *     Issue the MODE SELECT command to the IBM FAStT device 
 * attached to the given path. Use the 10 byte version of the
 * MODE SENSE command because it is necessary to support pages with
 * a size greater than 0xFF bytes.
 *
 * Results:       
 *	VMK_OK on success
 *	VMK_* on error
 *
 * Side effects:
 *	none
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
SCSIFAStTModeSelectCommand(SCSI_Handle *handle, SCSI_Path *path, 
int len, unsigned char *mp)
{ 
   SCSI_Command *cmd = NULL;
   VMK_ReturnStatus status;
 
   ASSERT(handle != NULL); 

   cmd = Mem_Alloc(sizeof(SCSI_Command));
   ASSERT(cmd);
   memset(cmd, 0, sizeof(SCSI_Command));

   cmd->sgArr.length = 1;
   cmd->sgArr.addrType = SG_MACH_ADDR;
   cmd->sgArr.sg[0].addr = VMK_VA2MA((VA)mp);
   cmd->sgArr.sg[0].length = len;
   cmd->cdbLength = 10; 
   cmd->dataLength = 0; 
 
   cmd->cdb[0] = SCSI_CMD_MODE_SELECT10; 
   cmd->cdb[1] = 0x0;                         // PF and SP = 0
   cmd->cdb[2] = 0x0;
   cmd->cdb[3] = 0x0;
   cmd->cdb[4] = 0x0;
   cmd->cdb[5] = 0x0;
   cmd->cdb[6] = 0x0; 
   cmd->cdb[7] = (len >> 8) & 0xFF;           // MSB
   cmd->cdb[8] = len & 0xFF;                  // LSB
   cmd->cdb[9] = 0x0;

   cmd->flags = SCSI_CMD_BYPASSES_QUEUE | SCSI_CMD_IGNORE_FAILURE; 
   status = SCSISyncCommand(handle, cmd, path, TRUE);
   Mem_Free(cmd);
  
   if ((status != VMK_OK) &&
       (status != VMK_NOT_READY)) {
      ASSERT( status != VMK_WOULD_BLOCK );
      Warning("SCSIFAStModeSelectCommand on %s:%d:%d returned %s", 
         path->adapter->name, path->id, path->lun,
	 VMK_ReturnStatusToString(status));
   }
   return(status);
}

/*
 * SCSIFAStTGetRedundantControllerData
 *
 *    Return the contents of the Redundant Controller page
 * on a FASTT device. If the target supports V54 of the 
 * spec., then 256 LUNs are supported per HBA and the page/
 * subpage (0x2C/0x1) version of the MODE SENSE command needs to 
 * be issued inorder to read the RDC page. If the target supports
 * V53 of the spec, then 32 LUNs are supported per HBA and the
 * basic page (0x2C) version of the MODE SENSE command needs to
 * be issued.
 * 
 * Results:       
 *      the pageDataStartOffset return parameter contains 
 *         the byte offset in the buffer of the start of 
 *         the Redundant Controller data
 *	VMK_OK on success
 *	VMK_* on error
 *
 * Side effects:
 *	none
 */
static VMK_ReturnStatus
SCSIFAStTGetRedundantControllerData(
SCSI_Handle *handle, SCSI_Path *path,
unsigned char *buffer, int bufferLen, int *pageDataStartOffset)
{ 
   VMK_ReturnStatus status;

   ASSERT(bufferLen >= FASTT_RCP_MAX_DATA_LEN);
   ASSERT(bufferLen >= (FASTT_RCP_DATA_OFFSET_FROM_PAGE + FASTT_RCP_V53_DATA_LEN));

   if (path->target->flags & SCSI_DEV_FASTT_V54) {
      if ((status = SCSIFAStTModeSenseCommand(handle, path, 
          FASTT_RCP_PAGE_NUM, FASTT_RCP_SUBPAGE_NUM,
          FASTT_RCP_MAX_DATA_LEN, buffer)) == VMK_OK) {
         *pageDataStartOffset = FASTT_RCP_DATA_OFFSET_FROM_SUBPAGE;
      } else {
         Warning("Could not get Redundant controller info using V5.4 for device %s:%d:%d", 
	    path->adapter->name, path->id, path->lun);
      }
   } else { 
      if ((status = SCSIFAStTModeSenseCommand(handle, path, 
	  FASTT_RCP_PAGE_NUM,  0x0,
	  FASTT_RCP_DATA_OFFSET_FROM_PAGE + FASTT_RCP_V53_DATA_LEN,
	  buffer)) == VMK_OK) {

         *pageDataStartOffset = FASTT_RCP_DATA_OFFSET_FROM_PAGE;
         ASSERT(buffer[FASTT_RCP_DATA_OFFSET_FROM_PAGE-1] == FASTT_RCP_V53_DATA_LEN);
      } else {
         Warning("Could not get Redundant controller info using V5.3 for device %s:%d:%d", 
	    path->adapter->name, path->id, path->lun);
      }
   }
   return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * SCSIFAStTReadUserConfigRegion --
 *
 *     Issue the READ BUFFER command to the IBM FAStT device 
 * attached to the given path and request the contents of the 
 * User Configurable Region of the non-volatile ram. The *mp
 * parameter must be at least FASTT_USR_LEN in length.
 *
 * Results: 
 *      contents of the User Configurable Region are returned in mp.
 *	VMK_OK on success
 *	VMK_* on error
 *
 * Side effects:
 *	none
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
SCSIFAStTReadUserConfigRegion(SCSI_Handle *handle, SCSI_Path *path, char *mp)
{
   SCSI_Command *cmd = NULL;
   VMK_ReturnStatus status;
 
   ASSERT(handle != NULL); 

   cmd = Mem_Alloc(sizeof(SCSI_Command));
   ASSERT(cmd);
   memset(cmd, 0, sizeof(SCSI_Command));

   cmd->sgArr.length = 1;
   cmd->sgArr.addrType = SG_MACH_ADDR;
   cmd->sgArr.sg[0].addr = VMK_VA2MA((VA)mp);
   cmd->sgArr.sg[0].length = FASTT_UCR_LEN;
   cmd->cdbLength = 10;
   cmd->dataLength = 0; 
   
   cmd->cdb[0] = SCSI_CMD_READ_BUFFER;
   cmd->cdb[1] = 0x02 | ((path->lun << 5) & (0xE0));    // 010b - data mode
   cmd->cdb[2] = FASTT_UCR_BUFFER_ID; 
   cmd->cdb[8] = FASTT_UCR_LEN;

   cmd->flags = SCSI_CMD_BYPASSES_QUEUE | SCSI_CMD_IGNORE_FAILURE; 
   status = SCSISyncCommand(handle, cmd, path, TRUE);
   Mem_Free(cmd);
   
   if ((status != VMK_OK) &&
       (status != VMK_NOT_READY)) {
      ASSERT( status != VMK_WOULD_BLOCK );
      Warning("SCSIFAStReadUserConfigCommand on %s:%d:%d returned %s", 
         path->adapter->name, path->id, path->lun,
	 VMK_ReturnStatusToString(status));
   }
   return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * SCSIFAStTIsInAutoVolumeTransferMode --
 *
 *	Read the User Configurable Region of NVSRAM and determine
 * if the SAN has been configured to run in Auto-Volume Transfer mode.
 * Note: AVT mode is specific to host type.
 *
 * Results: 
 *	TRUE if the device is in AVT mode or there is an error reading the NVSRAM
 *	FALSE otherwise
 *
 * Side effects:
 *	none
 *
 *----------------------------------------------------------------------
 */
static Bool
SCSIFAStTIsInAutoVolumeTransferMode(SCSI_Handle *handle, SCSI_Path *path)
{ 
   Bool 		ret = TRUE;
   VMK_ReturnStatus 	status;
   unsigned char 	*ucrp = (unsigned char *)Mem_Alloc(FASTT_UCR_LEN);

   ASSERT(ucrp != NULL);
   memset(ucrp, 0xFF, FASTT_UCR_LEN);

   if ((status = SCSIFAStTReadUserConfigRegion( handle, path, ucrp)) != VMK_OK) {
      Warning("Could not read user configurable region from FAStT device %s:%d:%d (%s).",
         path->adapter->name, path->id, path->lun, VMK_ReturnStatusToString(status));
   } else if (!(ucrp[FASTT_UCR_AVT_BYTE] & FASTT_UCR_AVT_MASK)) {
      ret = FALSE;
   } 
   Mem_Free(ucrp);
   return(ret); 
}

/*
 *----------------------------------------------------------------------
 *
 * SCSIFAStTIsInDualActiveMode --
 *
 *	Read the Redundant Controller Page and check if the device is
 * in Dual Active mode, if there are two controllers.
 *
 * Results: 
 *	TRUE if the device is Dual Active, or there is only one controller
 *	FALSE otherwise
 *
 * Side effects:
 *	none
 *
 *----------------------------------------------------------------------
 */
static Bool
SCSIFAStTIsInDualActiveMode(SCSI_Handle *handle, SCSI_Path *path)
{ 
   VMK_ReturnStatus status;
   Bool ret = FALSE;
   int offset = 0;
   unsigned char *rcData = (unsigned char *)Mem_Alloc(FASTT_RCP_MAX_DATA_LEN);

   ASSERT(rcData != NULL);
   memset(rcData, 0xFF, FASTT_RCP_MAX_DATA_LEN);

   if ((status = SCSIFAStTGetRedundantControllerData(
      handle, path, rcData, FASTT_RCP_MAX_DATA_LEN, &offset)) != VMK_OK) {
      Warning("Could not read sense data for %s:%d:%d", path->adapter->name, path->id, path->lun);
   } else {

      if ( (rcData[offset + FASTT_RCP_DATA_RDAC_MODE_BYTE1_OFFSET]  == 0x00) &&
           (rcData[offset + FASTT_RCP_DATA_ARDAC_MODE_BYTE1_OFFSET] == 0x00) ) {
         Log("There is only a single controller present for adapter %s", path->adapter->name);
	 ret = TRUE;
      } else if ( (rcData[offset + FASTT_RCP_DATA_RDAC_MODE_BYTE1_OFFSET]  == 0x01) &&
                  (rcData[offset + FASTT_RCP_DATA_ARDAC_MODE_BYTE1_OFFSET] == 0x01) ) {
         if ( (rcData[offset + FASTT_RCP_DATA_RDAC_MODE_BYTE2_OFFSET]  == 0x02) &&
              (rcData[offset + FASTT_RCP_DATA_ARDAC_MODE_BYTE2_OFFSET] == 0x02) ) {
            Log("Dual Controllers active for adapter %s", path->adapter->name);
	    ret = TRUE;
         } 
      }

      if (ret == FALSE) {
         Warning("Unrecognized controller setup for adapter %s.", path->adapter->name);
	 Warning("Mode[34] = 0x%x, Mode[35] = 0x%x, Mode[36] = 0x%x, Mode[37] = 0x%x",
            rcData[offset + FASTT_RCP_DATA_RDAC_MODE_BYTE1_OFFSET],
            rcData[offset + FASTT_RCP_DATA_RDAC_MODE_BYTE2_OFFSET],
            rcData[offset + FASTT_RCP_DATA_ARDAC_MODE_BYTE1_OFFSET],
            rcData[offset + FASTT_RCP_DATA_ARDAC_MODE_BYTE2_OFFSET]);
      }
   } 
   Mem_Free(rcData);
   return(ret);
}

/*
 *----------------------------------------------------------------------
 *
 * SCSIFAStTUsingPreferredController --
 *
 *	Read the Redundant Controller page and check if the LUN specified
 * by the given path is using the preferred controller.
 *
 * Results: 
 *	TRUE if the path is using the preferred controller
 *	FALSE otherwise
 *
 * Side effects:
 *	none
 *
 *----------------------------------------------------------------------
 */
static Bool
SCSIFAStTLUNUsingPreferredController(SCSI_Handle *handle, SCSI_Path *path)
{
   Bool ret = FALSE;
   int offset = 0;
   VMK_ReturnStatus status = VMK_IO_ERROR;
   
   unsigned char *rcData = (unsigned char *)Mem_Alloc(FASTT_RCP_MAX_DATA_LEN);
   ASSERT(rcData != NULL);
   memset(rcData, 0xFF, FASTT_RCP_MAX_DATA_LEN);

   if (path->target->flags & SCSI_DEV_FASTT_V54) {
      if (path->lun > (FASTT_V54_MAX_SUPPORTED_LUNS - 1)) {
          Warning("LUN %d is too large for the v54 FAStT device at %s:%d:%d",
	       path->lun, path->adapter->name, path->id, path->lun);
          Mem_Free(rcData);
          return(ret);
      }
   } else {
      if (path->lun > (FASTT_V53_MAX_SUPPORTED_LUNS - 1)) {
          Warning("LUN %d is too large for the v53 FAStT device at %s:%d:%d",
	       path->lun, path->adapter->name, path->id, path->lun);
          Mem_Free(rcData);
          return(ret);
      }
   }

   if ((status = SCSIFAStTGetRedundantControllerData(handle, path,
	       rcData, FASTT_RCP_MAX_DATA_LEN, &offset)) != VMK_OK) { 
      Warning("Could not read sense data for %s:%d:%d", path->adapter->name, path->id, path->lun);
   } else {
#ifdef VMX86_DEBUG
      int i;
      char ctrlSN[FASTT_CTRL_SERIAL_NUMBER_LEN];
      char altCtrlSN[FASTT_CTRL_SERIAL_NUMBER_LEN];

      for (i = 0; i< FASTT_CTRL_SERIAL_NUMBER_LEN; i++) {
         ctrlSN[i] = rcData[offset + FASTT_RCP_DATA_RDAC_SN_OFFSET + i]; 
      } 
      for (i = 0; i< FASTT_CTRL_SERIAL_NUMBER_LEN; i++) {
         altCtrlSN[i] = rcData[offset+ FASTT_RCP_DATA_ARDAC_SN_OFFSET + i]; 
      } 
#endif
      if (rcData[offset + FASTT_RCP_DATA_LUN_INFO_OFFSET + path->lun] & 0x01) {
         LOG(1,"Path %s:%d:%d uses the primary controller '\'%s\' as preferred for lun %d",
           path->adapter->name, path->id, path->lun, ctrlSN,  path->lun);
         ret = TRUE;
      } else {
         LOG(1,"Path %s:%d:%d uses the alternate  controller \'%s\' as preferred for lun %d",
            path->adapter->name, path->id, path->lun, altCtrlSN, path->lun);
      }
   } 
   Mem_Free(rcData);
   return(ret);

}

/*
 *----------------------------------------------------------------------
 *
 * SCSIFAStTSetPreferredController --
 * 
 * 	Set the primary controller for this adapter as the preferred controller
 * for this lun.
 *
 * Results: 
 *	VMK_OK on success
 *
 * Side effects:
 *	none
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
SCSIFAStTSetPreferredController(SCSI_Handle *handle)
{ 
   int offset = 0;
   VMK_ReturnStatus status = VMK_IO_ERROR;
   SCSI_Path *path = handle->target->activePath;

   unsigned char *rcData = (unsigned char *)Mem_Alloc(FASTT_RCP_MAX_DATA_LEN);
   ASSERT(rcData != NULL);
   memset(rcData, 0x0, FASTT_RCP_MAX_DATA_LEN);

   if (path->target->flags & SCSI_DEV_FASTT_V54) { 
      if ( path->lun > (FASTT_V54_MAX_SUPPORTED_LUNS - 1)) {
         Warning("LUN %d is too large for the v54 FAStT device at %s:%d:%d",
	       path->lun, path->adapter->name, path->id, path->lun);
         return status;
      }
   } else {
      if ( path->lun > (FASTT_V53_MAX_SUPPORTED_LUNS - 1)) {
         Warning("LUN %d is too large for the v53 FAStT device at %s:%d:%d",
	       path->lun, path->adapter->name, path->id, path->lun);
         return status;
      }
   }

   if ((status = SCSIFAStTGetRedundantControllerData(handle, path, 
	       rcData, FASTT_RCP_MAX_DATA_LEN, &offset)) != VMK_OK) {
      Warning("Could not read sense data for %s:%d:%d", path->adapter->name, path->id, path->lun);
   } else { 
      ASSERT(rcData[7] == 0x08);	 // len of block descriptor
      ASSERT(rcData[14] == 0x02);	 // page length (0x200)

      /*
       * cause this controller to remain in Dual Active mode
       */
      rcData[offset + FASTT_RCP_DATA_RDAC_MODE_BYTE2_OFFSET] = 0x02; 

      /* 
       * cause this controller to have preferred ownership of this lun
       */
      rcData[offset + FASTT_RCP_DATA_LUN_INFO_OFFSET + path->lun] = 0x81;

      if (path->target->flags & SCSI_DEV_FASTT_V54) {
         /*
	  * The FAStT is using SIS V5.4
	  */
         ASSERT(offset == FASTT_RCP_DATA_OFFSET_FROM_SUBPAGE);
         ASSERT(rcData[offset - 3] == FASTT_RCP_SUBPAGE_NUM);
	 /*
	  * Length does not include the 2 bytes that hold the length field.
	  */
         ASSERT(rcData[0] == (((FASTT_RCP_MAX_DATA_LEN - 2) >> 8) & 0xFF));
         ASSERT(rcData[1] ==  ((FASTT_RCP_MAX_DATA_LEN - 2) & 0xFF));

         /* 
          * setup sense buffer for writing
	  * set mode page number and the SPF bit 
	  * to indicate sub-page format
	  */
         rcData[offset - 4] = 0x40 | FASTT_RCP_PAGE_NUM; 
         if ((status = SCSIFAStTModeSelectCommand( handle, path, 
	      FASTT_RCP_MAX_DATA_LEN, rcData)) != VMK_OK) {
            Warning("Could not write sense data for %s:%d:%d", path->adapter->name, path->id, path->lun);
         }
      } else {
         /*
	  * The FAStT is using SIS V5.3
	  */
         ASSERT(offset == FASTT_RCP_DATA_OFFSET_FROM_PAGE);
	 /*
	  * Length does not include the 2 bytes that hold the length field.
	  */
         ASSERT(rcData[0] == 
	    (((FASTT_RCP_DATA_OFFSET_FROM_PAGE + FASTT_RCP_V53_DATA_LEN - 2) >> 8) & 0xFF));
         ASSERT(rcData[1] == 
	    ((FASTT_RCP_DATA_OFFSET_FROM_PAGE + FASTT_RCP_V53_DATA_LEN - 2) & 0xFF));

         /*
          * setup sense buffer for writing
	  * set mode page number and clear the SPF bit
	  * to indicate base 0 page format
          */
         rcData[offset - 2] = FASTT_RCP_PAGE_NUM;
         if ((status = SCSIFAStTModeSelectCommand( handle, path,
	      FASTT_RCP_DATA_OFFSET_FROM_PAGE + FASTT_RCP_V53_DATA_LEN, rcData)) != VMK_OK) {
            Warning("Could not write sense data for %s:%d:%d", path->adapter->name, path->id, path->lun);
         }
      }
   } 
   Mem_Free(rcData);

   LOG(0,"Set Controller on %s:%d:%d returned %s",
      handle->target->activePath->adapter->name, 
      handle->target->activePath->id, handle->target->activePath->lun,
      VMK_ReturnStatusToString(status));

   return(status); 
}

#ifdef FASTT_DEBUG

/*
 *----------------------------------------------------------------------
 *
 * SCSIFAStTWriteUserConfigRegion --
 *
 *     Issue the WRITE BUFFER command to the IBM FAStT device 
 * attached to the given path and write the contents of the mp buffer to the
 * User Configurable Region of the non-volatile ram. The *mp
 * parameter must be at least FASTT_USR_LEN in length.
 *
 * Results:
 *	VMK_IO on success
 *	VMK_* on error
 *
 * Side effects:
 *	none
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
SCSIFAStTWriteUserConfigRegion(SCSI_Handle *handle, SCSI_Path *path, char *mp)
{
   SCSI_Command *cmd = NULL;
   VMK_ReturnStatus status;
 
   ASSERT(handle != NULL); 

   cmd = Mem_Alloc(sizeof(SCSI_Command));
   ASSERT(cmd);
   memset(cmd, 0, sizeof(SCSI_Command));

   cmd->sgArr.length = 1;
   cmd->sgArr.addrType = SG_MACH_ADDR;
   cmd->sgArr.sg[0].addr = VMK_VA2MA((VA)mp);
   cmd->sgArr.sg[0].length = FASTT_UCR_LEN;
   cmd->cdbLength = 10;
   cmd->dataLength = 0;

   cmd->cdb[0] = SCSI_CMD_WRITE_BUFFER;
   cmd->cdb[1] = 0x02 | ((path->lun << 5) & (0xE0)); 
   cmd->cdb[2] = FASTT_UCR_BUFFER_ID;
   cmd->cdb[8] = FASTT_UCR_LEN;

   cmd->flags = SCSI_CMD_BYPASSES_QUEUE | SCSI_CMD_IGNORE_FAILURE; 
   status = SCSISyncCommand(handle, cmd, path, TRUE);
   Mem_Free(cmd); 
 
   if ((status != VMK_OK) &&
       (status != VMK_NOT_READY)) {
      ASSERT( status != VMK_WOULD_BLOCK );
      Warning("SCSIFAStTWriteUserConfigCommand on %s:%d:%d returned: %s",
         path->adapter->name, path->id, path->lun,
         VMK_ReturnStatusToString(status));
   }
   return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * SCSIFAStTConfigAutoVolumeTransferMode --
 *
 *	Enable or disable AVT mode on the given SAN depending upon the
 * value of the "on" parameter.
 *
 * Results: 
 *	VMK_IO on success
 *	VMK_* otherwise
 *
 * Side effects:
 *	none
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
SCSIFAStTConfigAutoVolumeTransferMode(SCSI_Handle *handle, SCSI_Path *path, Bool on)
{ 
   VMK_ReturnStatus status = VMK_IO_ERROR;
   unsigned char *ucrp;
   ucrp = (unsigned char *)Mem_Alloc(FASTT_UCR_LEN);

   ASSERT(ucrp != NULL);
   memset(ucrp, 0xFF, FASTT_UCR_LEN);

   if ((status = SCSIFAStTReadUserConfigRegion( handle, path, ucrp)) != VMK_OK) {
      Warning("Could not read user configurable region from FAStT device %s:%d:%d (%s). ",
         path->adapter->name, path->id, path->lun, VMK_ReturnStatusToString(status));
   } else {
      if (on) {
         ucrp[FASTT_UCR_AVT_BYTE] |= FASTT_UCR_AVT_MASK;
      } else {
         ucrp[FASTT_UCR_AVT_BYTE] &= ~FASTT_UCR_AVT_MASK;
      }
      if ((status = SCSIFAStTWriteUserConfigRegion( handle, path, ucrp)) != VMK_OK) {
         Warning("Could not write user configurable region from FAStT device %s:%d:%d (%s).",
            path->adapter->name, path->id, path->lun, VMK_ReturnStatusToString(status));
      }
   }
   Mem_Free(ucrp);
   return(status);
} 

/*
 *----------------------------------------------------------------------
 *
 * SCSIFAStTConfigDualActiveMode --
 *
 *	Configure the specified device to use or not use Dual Active mode
 * depending on the setting of "on".
 * NOTE:
 * 	Issuing this command will cause all the LUNs to have their preferred
 * controller switched to the primary.
 *
 * Results: 
 *	VMK_OK if the device was configured
 *	VMK_* otherwise
 *
 * Side effects:
 *	none
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
SCSIFAStTConfigDualActiveMode(SCSI_Handle *handle, SCSI_Path *path, Bool on)
{
   VMK_ReturnStatus status = VMK_IO_ERROR;
   unsigned char *rcData = (unsigned char *)Mem_Alloc(FASTT_RCP_MAX_DATA_LEN);
   int offset = 0;

   ASSERT(rcData != NULL);
   memset(rcData, 0xFF, FASTT_RCP_MAX_DATA_LEN);

   if ((status = SCSIFAStTGetRedundantControllerData(handle, path,
        rcData, FASTT_RCP_MAX_DATA_LEN, &offset)) != VMK_OK) {
      Warning("Could not read sense data for %s:%d:%d", path->adapter->name, path->id, path->lun);
   } else {
      ASSERT(rcData[7] == 0x08);	 // len of block descriptor
      ASSERT(rcData[14] == 0x02);	 // page length (0x200)

      if (on) {
         /*
	  * Set to Dual Active Mode.
	  */
         rcData[offset + FASTT_RCP_DATA_RDAC_MODE_BYTE2_OFFSET] = 0x02;
         if (rcData[offset + FASTT_RCP_DATA_ARDAC_MODE_BYTE2_OFFSET] == 0x04) {
            /*
             * the alternate controller is in Reset, release it
	     */
            rcData[offset + FASTT_RCP_DATA_ARDAC_MODE_BYTE2_OFFSET] = 0x08;	
         } else {
            rcData[offset + FASTT_RCP_DATA_ARDAC_MODE_BYTE2_OFFSET] = 0x0;
         } 
      } else {
	 /* 
	  * transfer ownership of all LUNs to this controller
	  */
         rcData[offset + FASTT_RCP_DATA_RDAC_MODE_BYTE2_OFFSET] = 0x01;
	 /*
	  * set the alternate controller to Reset
	  */
         rcData[offset + FASTT_RCP_DATA_ARDAC_MODE_BYTE2_OFFSET] = 0x0C;
      }

      if (path->target->flags & SCSI_DEV_FASTT_V54) {
         /*
	  * The FAStT is using SIS V5.4
	  */
         ASSERT(offset == FASTT_RCP_DATA_OFFSET_FROM_SUBPAGE);
         ASSERT(rcData[0] == (((FASTT_RCP_MAX_DATA_LEN - 1) >> 8) & 0xFF));
         ASSERT(rcData[1] ==  ((FASTT_RCP_MAX_DATA_LEN - 1) & 0xFF));
         ASSERT(rcData[offset - 3] == FASTT_RCP_SUBPAGE_NUM);

         /* 
          * setup sense buffer for writing
	  * set mode page number and the SPF bit
	  */
         rcData[offset - 4] = 0x40 | FASTT_RCP_PAGE_NUM; 
         if ((status = SCSIFAStTModeSelectCommand( handle, path, 
	      FASTT_RCP_MAX_DATA_LEN, rcData)) != VMK_OK) {
            Warning("Could not write sense data for %s:%d:%d", path->adapter->name, path->id, path->lun);
         }
      } else {
         /*
	  * The FAStT is using SIS V5.3
	  */
         ASSERT(offset == FASTT_RCP_DATA_OFFSET_FROM_PAGE);
         ASSERT(rcData[0] == 
	    (((FASTT_RCP_DATA_OFFSET_FROM_PAGE + FASTT_RCP_V53_DATA_LEN - 1) >> 8) & 0xFF));
         ASSERT(rcData[1] == 
	    ((FASTT_RCP_DATA_OFFSET_FROM_PAGE + FASTT_RCP_V53_DATA_LEN - 1) & 0xFF));

         /*
          * setup sense buffer for writing
	  * set mode page number (clear the SP bit)
          */
         rcData[offset - 2] = FASTT_RCP_PAGE_NUM;
         if ((status = SCSIFAStTModeSelectCommand( handle, path,
	      FASTT_RCP_DATA_OFFSET_FROM_PAGE + FASTT_RCP_V53_DATA_LEN, rcData)) != VMK_OK) {
            Warning("Could not write sense data for %s:%d:%d", path->adapter->name, path->id, path->lun);
         }
      }
   } 
   Mem_Free(rcData);
   return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * SCSIFAStTVerifySettings -- 
 *
 *
 *	Check that the device is either NOT in AVT mode  or IS in Dual-Active mode. 
 * Depending on the setting of DISK_USE_AVT_ON_FASTT config. value.
 * If the device is not in the correct mode, issue a warning and try to correct the setting. 
 * 
 * Note:  the AVT and Dual Active settings are local to the "host type". Host type
 * is specified with the FAStT configuration tool on a per HBA basis.
 * HBAs on ESX are set to the "LNX" (Linux) host type by default.
 *
 * Results: 
 *	none	
 *
 * Side effects:
 *	none
 *
 *----------------------------------------------------------------------
 */ 
void
SCSIFAStTVerifySettings(SCSI_Handle *handle, SCSI_Path *path)
{
   if (SCSIFAStTIsInAutoVolumeTransferMode( handle, path) == FALSE) {
      Warning("The IBM FAStT SAN on %s is not configured in Auto-Volume Transfer mode.", 
            path->adapter->name);
      Warning("The user has requested that the FAStT be run in AVT mode.");
      Warning("The SAN will be reconfigured to perform Auto-Volume Transfer");
      if (SCSIFAStTConfigAutoVolumeTransferMode( handle, path, TRUE) != VMK_OK) {
         Warning("Could not enable Auto-Volume Transfer.");
      } else if (SCSIFAStTIsInAutoVolumeTransferMode( handle, path) == TRUE) {
         Warning("Auto-Volume Transfer mode is still disabled after enable attempt.");
	 Warning("Path failover will not work correctly.");
      }
   } else {
      if (SCSIFAStTIsInAutoVolumeTransferMode( handle, path) == TRUE) {
         Warning("The IBM FAStT SAN on %s is configured in Auto-Volume Transfer mode.", 
            path->adapter->name);
         Warning("ESX cannot support automatic path failover with the disk array in this mode.");
         Warning("The disk array will be reconfigured not to perform Auto-Volume Transfer");
         Warning("Auto-Volume Transfer still enabled after disable attempt.");
	 Warning("Path failover will not work correctly for adapter %s.", path->adapter->name);
      }
   }

   /*
    * Verify that the FAStT SAN is in Dual Active Mode, if there are two
    * controllers in the system.
    */
   if (SCSIFAStTIsInDualActiveMode(handle, path) != TRUE) { 
         Warning("The IBM FAStT SAN on %s is not configured in Dual Active controller mode.", 
            path->adapter->name);
         Warning("ESX cannot support automatic path failover without the SAN in this mode.");
         Warning("The SAN will be reconfigured to Dual Active controller mode");
      if (SCSIFAStTConfigDualActiveMode(handle, path, TRUE) != VMK_OK) {
         Warning("Could not reconfigure device to Dual Active controller mode.");
      } else if (SCSIFAStTIsInDualActiveMode(handle, path) != TRUE) {
         Warning("Dual Active Mode still not enabled after enable attempt.");
         Warning("Path failover will not work correctly for adapter %s.", path->adapter->name);
      }
   } 
} 
#endif

/*
 *----------------------------------------------------------------------
 *
 * SCSISetTargetType --
 *
 *      Extract the model name for the target and determine if manual
 *      failover is required. SAN devices that have Active/Passive Paths
 *      require manual failover.  Assumes that target is new (not in use
 *      yet), so locking of the target struct is not required.  Also
 *      requires that partition table has already been loaded for target.
 *
 * Results: 
 *	TRUE
 *
 * Side effects:
 *	none
 *
 *----------------------------------------------------------------------
 */ 
Bool
SCSISetTargetType(SCSI_Target *target,VMnix_TargetInfo *targetInfo)
{
   int i;
   uint8 *data = (char *)targetInfo->inquiryInfo;
   char modelName[SCSI_MODEL_LENGTH+1];
   char vendorName[SCSI_VENDOR_LENGTH+1];
   char *p = NULL;
   unsigned char *buffer = NULL;
   SCSI_Path *path = target->activePath;
   SCSI_Handle *handle = NULL;

   ASSERT(path);

   p = vendorName;
   for (i = SCSI_VENDOR_OFFSET; i < SCSI_VENDOR_OFFSET + SCSI_VENDOR_LENGTH; i++) { 
      if (data[i] >= 0x20 && i < data[4] + 5) {
	 *p = data[i];
      }	else { 
	 *p = ' ';
      } 
      p++;
   }
   *p ='\0';

   p = modelName;
   for (i = SCSI_MODEL_OFFSET; i < SCSI_MODEL_OFFSET + SCSI_MODEL_LENGTH; i++) { 
      if (data[i] >= 0x20 && i < data[4] + 5) {
	 *p = data[i];
      }	else { 
	 *p = ' ';
      } 
      p++;
   }
   *p ='\0';
 
   if (SCSIIsActivePassiveSANDevice(modelName) == TRUE) {
      target->flags |= SCSI_SUPPORTS_MANUAL_SWITCHOVER;
      target->flags |= SCSI_MUST_USE_MRU_POLICY;
      Log("Device %s:%d:%d user configured as AP.", 
            target->adapter->name, target->id, target->lun);
   } else if (SCSIFAStTDevice(vendorName, modelName)) {
      /*
       * Check if FAStT is in AVT mode.
       * If not, then ESX needs to do manual switchover
       */
      ASSERT(target->partitionTable);
      ASSERT(target->numPartitions > 0);
  
      /*
       * Bump target refCount for the handle
       */
      target->refCount++;
      SP_Lock(&scsiLock);
      handle = SCSIAllocHandleTarg(path->target, hostWorld->worldID, 0);
      SP_Unlock(&scsiLock);
      ASSERT(handle != NULL);

      /*
       * Check if the FAStT supports v54 of the spec.
       * This is done by trying to issue a MODE_SENSE subpage command
       * which is only supported by v54.
       */
      buffer = (unsigned char *)Mem_Alloc(FASTT_RCP_MAX_DATA_LEN);
      ASSERT(buffer);
      target->flags |= SCSI_DEV_FASTT;
      if (SCSIFAStTModeSenseCommand(handle, path, 
          FASTT_RCP_PAGE_NUM, FASTT_RCP_SUBPAGE_NUM,
          FASTT_RCP_MAX_DATA_LEN, buffer) == VMK_OK) {
         Log("Device %s:%d:%d is attached to a V54 FAStT SAN.", 
              target->adapter->name, target->id, target->lun);
         target->flags |= SCSI_DEV_FASTT_V54;
      } else {
         Log("Device %s:%d:%d is attached to a V53 FAStT SAN.", 
            target->adapter->name, target->id, target->lun);
      }
      Mem_Free(buffer);

#ifdef FASTT_DEBUG
      if (SCSIFAStTConfigAutoVolumeTransferMode( handle, path, FALSE) != VMK_OK) {
         Warning("Could not enable Auto-Volume Transfer.");
      } 
#endif 
      if (SCSIFAStTIsInAutoVolumeTransferMode( handle, path) == FALSE) {
         target->flags |= SCSI_SUPPORTS_MANUAL_SWITCHOVER; 
         target->flags |= SCSI_MUST_USE_MRU_POLICY; 
         /*
          * If the FAStT is not in AVT mode, then verify that the FAStT SAN is in Dual Active Mode, 
          * if there are two controllers in the system.
          */ 
         if (SCSIFAStTIsInDualActiveMode(handle, path) == FALSE) {
            Warning("The IBM FAStT device on %s:%d:%d is not configured in Dual Active controller mode.", 
               target->adapter->name, target->id, target->lun);
            Warning("ESX cannot support path failover without the disk array in this mode.");
         } else {
            Log("The IBM FAStT device on %s:%d:%d is not configured in Auto-Volume Transfer mode. " 
	       "ESX will handle path failover to passive controllers as necessary.",
               target->adapter->name, target->id, target->lun);
         }
      } else {
         Log("The IBM FAStT device on %s:%d:%d is configured in Auto-Volume Transfer mode. " 
	     "There may be path contention if more than one ESX system is configured to access the disk array.",
            target->adapter->name, target->id, target->lun);
      } 
      SCSIHandleDestroy(handle);
   } else if (SCSISVCDevice(vendorName, modelName)) {
      /*
       * The IBM SVC is an Active/Active Array, but when it
       * returns a NOT_READY check status a different path should be tried 
       * so the device is marked as MANUAL_SWITCHOVER with MRU policy
       */
      target->flags |= SCSI_DEV_SVC;
      target->flags |= SCSI_SUPPORTS_MANUAL_SWITCHOVER; 
      target->flags |= SCSI_MUST_USE_MRU_POLICY;
      Log("Device %s:%d:%d is attached to an IBM SVC.", 
            target->adapter->name, target->id, target->lun);
   } else if (strncmp(vendorName,"DGC ",4) == 0) {
        /*
         * Not in the AP list, check for DGC
         * DGC is the Clariion vendor name for all models
         */
      target->flags |= SCSI_SUPPORTS_MANUAL_SWITCHOVER; 
      target->flags |= SCSI_DEV_DGC;
      target->flags |= SCSI_MUST_USE_MRU_POLICY;

      /*
       * LUNZ can only exist on a Clariion Array at a LUN id of 0
       */
      if ((target->lun == 0) && (strncmp(modelName, "LUNZ", 4) == 0)) {
         target->flags |= SCSI_DEV_PSEUDO_DISK;
      }
      Log("Device %s:%d:%d is attached to an EMC Clariion SAN.", 
            target->adapter->name, target->id, target->lun);
   } else if ((strncmp(vendorName,"DEC ",4) == 0) && 
              (strncmp(modelName,"HSG80 ",6) == 0)) {
      target->flags |= SCSI_SUPPORTS_MANUAL_SWITCHOVER; 
      target->flags |= SCSI_DEV_HSG80;
      target->flags |= SCSI_MUST_USE_MRU_POLICY;
      Log("Device %s:%d:%d is attached to a DEC HSG80 SAN.", 
            target->adapter->name, target->id, target->lun);
   } else if (strncmp(modelName, "MSA1000", 7) == 0) { 
      target->flags |= SCSI_SUPPORTS_MANUAL_SWITCHOVER; 
      target->flags |= SCSI_DEV_MSA;
      target->flags |= SCSI_MUST_USE_MRU_POLICY;
      Log("Device %s:%d:%d is attached to an HP MSA1000 SAN.", 
            target->adapter->name, target->id, target->lun);
   } else if (strncmp(modelName, "HSV1", 4) == 0) { 
      target->flags |= SCSI_SUPPORTS_MANUAL_SWITCHOVER; 
      target->flags |= SCSI_DEV_HSV;
      target->flags |= SCSI_MUST_USE_MRU_POLICY;
      Log("Device %s:%d:%d is attached to an HP HSV SAN.", 
            target->adapter->name, target->id, target->lun);
   } else {
      Log("Device %s:%d:%d has not been identified as being attached "
          " to an active/passive SAN. It is either attached to an"
	  " active/active SAN or is a local device.",
          target->adapter->name, target->id, target->lun);
   }

   return(TRUE);
}

/*
 *----------------------------------------------------------------------
 *
 * SCSIDoFailover
 *
 *    This function is used to run SCSIExecQueuedCommand() in a world 
 *    context rather than in a bottom-half. The failover actions may 
 *    require that SCSI commands be issued while changing adapters. This
 *    routine tries to exec a queued only once. If the queued command cannot 
 *    be exec'd, then another helper world task is scheduled. Long running
 *    helper world tasks can prevent Timers from executing on time.
 *
 * Results:
 * 	none
 *
 * Side effects:
 *   	none
 *----------------------------------------------------------------------
 */
static void
SCSIDoFailover(void *data)
{
   SCSI_Target *target = (SCSI_Target *)data;

   Bool stillQueued = SCSIExecQueuedCommand(target, TRUE, TRUE, FALSE);

   SP_Lock(&target->adapter->lock);
   ASSERT(SCSIDelayCmdsCount(target) > 0);
   if (stillQueued) {
      Warning("Could not exec queued command to cause failover for target %s:%d:%d." 
	      " Rescheduling. Current failover count = %d",
	        target->adapter->name, target->id, target->lun, 
	        SCSIDelayCmdsCount(target));
   } else {
      SCSIDecDelayCmds(target);
   }

   if (SCSIDelayCmdsCount(target) > 0) {
      SCSIRequestHelperFailoverInt(target);
   }
   SP_Unlock(&target->adapter->lock);
}


/* 
 * Time to wait before trying to queue helper request to perform
 * failover.
 */
#define SCSI_FAILOVER_RETRY_DELAY_TIME 1000 

static void
SCSIRequestHelperFailoverInt(void *target)
{ 
   VMK_ReturnStatus rs;

   /*
    * Retry queueing a helper request for failover.
    */ 
   rs = Helper_Request(HELPER_FAILOVER_QUEUE, SCSIDoFailover, target );
   if (rs != VMK_OK) {
      Warning("Could not issue helper world request from retry. Failover being delayed again.");
      Timer_Add(MY_PCPU, (Timer_Callback) SCSIRequestHelperFailoverInt,
		SCSI_FAILOVER_RETRY_DELAY_TIME, TIMER_ONE_SHOT, target);
   } else {
      LOG(0,"Helper world request queued successfully.");
   }
}

/*
 *----------------------------------------------------------------------
 *
 * SCSIRequestHelperFailover --
 *
 *      If necessary, make a helper request to call SCSIDoFailover.
 *	The helper request will automatically be retried if the helper
 *	request fails.
 *
 * Results: 
 *	none
 *
 * Side effects:
 *	may kickoff SCSIDoFailover request
 *
 *----------------------------------------------------------------------
 */
void
SCSIRequestHelperFailover(SCSI_Target *target)
{
   if ((target->flags & SCSI_SUPPORTS_MANUAL_SWITCHOVER) ||
       CONFIG_OPTION(DISK_RESET_ON_FAILOVER) ||
       (target->flags & SCSI_RESERVED_LOCAL) ||
       target->pendingReserves > 0) {
      SCSIIncDelayCmds(target); 

      if (SCSIDelayCmdsCount(target) == 1) {
	 LOG(0, "Schedule Failover helper world for target %s:%d:%d." 
	     " Active failover count for path = %d",
	     target->adapter->name, target->id, 
	     target->lun, SCSIDelayCmdsCount(target));
	 SCSIRequestHelperFailoverInt(target);
      } else {
	 LOG(0, "Failover helper world for target %s:%d:%d already active." 
	     " Active failover count for count = %d",
	     target->adapter->name, target->id, target->lun, 
	     SCSIDelayCmdsCount(target));
      }
   }
}

/*
 * Path a SCSI path specification (e.g. vmhba1:0:5).  If found, return
 * TRUE, set *name, *id, and *lun appropriately, and set *end to the
 * end of the path name.
 */
static Bool
SCSIParsePath(char *path, char **name, uint32 *id, uint32 *lun, char **end)
{
   char *s;
   int val;

   if (!path) {
      return FALSE;
   }
   s = path;
   while (*s != ':' && *s != 0) {
      s++;
   }
   if (*s == 0) {
      return FALSE;
   }
   *name = Mem_Alloc(s - path + 1);
   ASSERT(*name != NULL);
   memcpy(*name, path, s-path);
   *(*name + (s-path)) = '\0';
   s++;

   val = 0;
   while (*s >= '0' && *s <= '9') {
      val = val*10 + (*s - '0');
      s++;
   }
   if (*s != ':') {
      Mem_Free(*name);
      return FALSE;
   }
   *id = val;
   s++;

   val = 0;
   while (*s >= '0' && *s <= '9') {
      val = val*10 + (*s - '0');
      s++;
   }
   if (*s != '\0' && *s != '\n' && *s != ' ') {
      Mem_Free(*name);
      return FALSE;
   }
   *lun = val;
   *end = s;
   return TRUE;
}

/*
 * Return TRUE if the first word in string p matches the word 'match',
 * ignoring white space.  If successful, set *end to the end of the word.
 */
static Bool
SCSIWordMatch(char *p, char *match, char **end)
{
   while (*p == ' ') {
      p++;
   }
   *end = p;
   if (strncmp(p, match, strlen(match)) == 0) {
      p += strlen(match);
      if (*p == ' ' || *p == '=' || *p == '\n' || *p == '\0') {
	 while (*p == ' ' || *p == '=') {
	    p++;
	 }
	 *end = p;
	 return TRUE;
      }
   }
   return FALSE;
}

/*
 * Search for a specified path among all the paths to a target.
 * Return the path, or NULL if not found.
 */
static SCSI_Path *
SCSIFindPath(SCSI_Target *target, char *adapterName, uint32 id, uint32 lun)
{
   SCSI_Path *path;

   ASSERT(SP_IsLocked(&target->adapter->lock));
   for (path = target->paths; path != NULL; path = path->next) {
      if (strcmp(path->adapter->name, adapterName) == 0 &&
	  path->id == id && path->lun == lun) {
	 return path;
      }
   }
   return NULL;
}

/*
 * Search for a path among all targets on all adapters.  If found,
 * increase the refCount of the target it is found on, and return the
 * target.  Only used during a rescan to avoid rescanning a path to an
 * active target.
 */
SCSI_Target *
SCSI_FindPathAll(char *adapterName, uint32 id, uint32 lun)
{
   int i;
   SCSI_Adapter *a;
   SCSI_Target *t;
   SCSI_Path *p;

   SP_Lock(&scsiLock);
   for (i = 0; i < HASH_BUCKETS; i++) {
      for (a = adapterHashTable[i]; a != NULL; a = a->next) {
	 SP_Lock(&a->lock);
	 for (t = a->targets; t != NULL; t = t->next) {
	    p = SCSIFindPath(t, adapterName, id, lun);
	    if (p != NULL) {
	       t->refCount++;
	       SP_Unlock(&a->lock);
	       SP_Unlock(&scsiLock);
	       return t;
	    }
	 }
	 SP_Unlock(&a->lock);
      }
   }
   SP_Unlock(&scsiLock);
   return NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * SCSIGetNumberOfPathsWithState --
 *
 *    Scan the paths to the given target and count those paths that
 *    are in the give state.
 *
 * Results:
 *    the count of the paths to the given target in the given state
 *
 * Side effects:
 *    none 
 *----------------------------------------------------------------------
 */
static int
SCSIGetNumberOfPathsWithState(SCSI_Target *target, int state)
{
    SCSI_Path *path;
    int count = 0;

    ASSERT(SP_IsLocked(&target->adapter->lock));
    for (path = target->paths; path != NULL; path = path->next) {
       if ( path->state == state ) {
          count++;
       }
    }
    return( count );
}

/*
 * Return TRUE if target of specified handle has a path that is working and
 * enabled.
 */
Bool
SCSIHasWorkingPath(SCSI_Handle *handle)
{
   SCSI_Path *path; 

   ASSERT(SP_IsLocked(&handle->target->adapter->lock));
   for (path = handle->target->paths; path != NULL; path = path->next) {
       LOG(5,"SCSIHasWorkingPath path state = 0x%x, PATH = %s:%d:%d", 
	path->state, path->adapter->name, path->id, path->lun);

      if (path->state == SCSI_PATH_STANDBY) { 
         LOG(5,"SCSIHasWorkingPath returned TRUE - FOUND STANDBY PATH");
         return TRUE;
      }

      if (path->state == SCSI_PATH_ON) { 
         LOG(5,"SCSIHasWorkingPath returned TRUE - FOUND ON PATH");
	 return TRUE;
      }
   } 
   LOG(5,"SCSIHasWorkingPath returned FALSE");
   return FALSE;
}

/*
 *-----------------------------------------------------------------------------
 *
 * SCSICheckUnitReadyCommand
 *
 *   Issue CheckUnitReady command directly to the adapter.
 *   Set the cmd flags such that the command will jump to the front of the queue, 
 *   and not be retried if the device returns an error, except in the case of a
 *   VMK_WOULD_BLOCK.  If the *path parameter is not NULL, the command will be 
 *   issued on the specified data path.
 *
 *   This routine can be called during a path failover condition. The
 *   "allowWouldBlock" parameter will be set to FALSE in this case causing the 
 *   the command to be retried in the event of a BUSY/VMK_WOULD_BLOCK condition.
 *
 * Return:
 *	0 is the unit is ready
 *	1 if the unit is not ready
 *	2 unit is not connected (dead path)
 *	3 request was not sent because it would have been queued
 *	-1 on error
 * 
 * Side effects:
 *    none
 *
 *-----------------------------------------------------------------------------
 */
static int
SCSICheckUnitReadyCommand(SCSI_Handle *handle, SCSI_Path *path, Bool allowWouldBlock)
{ 
   SCSI_Command *cmd; 
   VMK_ReturnStatus status; 
   int returnValue = -1;

   cmd = (SCSI_Command *)Mem_Alloc(sizeof(SCSI_Command));

   ASSERT(cmd != NULL);
   memset(cmd, 0, sizeof(SCSI_Command));
   cmd->type = SCSI_QUEUE_COMMAND; 
   cmd->cdb[0] = SCSI_CMD_TEST_UNIT_READY;
   cmd->cdbLength = 6;
   cmd->flags = SCSI_CMD_IGNORE_FAILURE | SCSI_CMD_PRINT_NO_ERRORS | 
	        SCSI_CMD_BYPASSES_QUEUE;

   if (allowWouldBlock) {
      cmd->flags |=  SCSI_CMD_RETURN_WOULD_BLOCK;
   }
 
   status = SCSISyncCommand(handle, cmd, path, TRUE); 

   Mem_Free(cmd);
   if (status == VMK_OK) {
      returnValue =  0; 
   } else if (status == VMK_RESERVATION_CONFLICT) { 
      SCSICondRelLog(SCSI_LOG_MULTI_PATH,
         "CheckUnitReady on device %s:%d:%d convert reservation conflict to ok", 
         path->adapter->name, path->id, path->lun);
      returnValue =  0; 
   } else if ( status == VMK_NOT_READY) {
      returnValue =  1;
   } else if ( status == VMK_NO_CONNECT) {
      returnValue =  2;
   } else if ( status == VMK_WOULD_BLOCK) {
      ASSERT(allowWouldBlock);
      returnValue =  3;
   } else {
      /*
       * Certain VMK_IO_ERROR returns are valid for non-disk devices.
       * For example, a Medium Not Present check condition for a tape device
       * will cause a VMK_IO_ERROR to be returned from SCSISyncCommand().
       */
      if (path->target->devClass == SCSI_CLASS_DISK) { 
         SCSICondRelLog(SCSI_LOG_MULTI_PATH,
            "CheckUnitReady on device %s:%d:%d  returned: %s", 
            path->adapter->name, path->id, path->lun,
            VMK_ReturnStatusToString(status));
      }
   }
   
   LOG(1,"CheckUnitReady on %s:%d:%d returned %s",
      path->adapter->name, path->id, path->lun,
      VMK_ReturnStatusToString(status));

   return(returnValue); 
}


#if 0
/*
 * For DGC Clariion, issue an Inquiry Page 0xC0 command directly to the
 * adapter.  Set the cmd flags such that the command will not be retried
 * if the device returns a NOT READY error. If the *path parameter is not
 * NULL, the command will be issued on the specified data path.
 *
 *   This routine can be called during a path failover condition. The
 *   "bypass" parameter will be set to TRUE in this case causing the 
 *   Inquiry Page C0 to jump to the head of the request queue.
 *
 * Return:
 *	0 is the unit is ready
 *	1 if the unit is not ready
 *	2 unit is not connected (dead path)
 *	-1 on error
 */
static int
SCSIDGCInquiryC0Command(SCSI_Handle *handle, SCSI_Path *path, Bool bypass)
{ 
   SCSI_Command *cmd; 
   unsigned char *response = NULL;
   VMK_ReturnStatus status; 
   int returnValue = -1; 

   cmd = (SCSI_Command *)Mem_Alloc(sizeof(SCSI_Command));

   ASSERT(cmd != NULL);
   memset(cmd, 0, sizeof(SCSI_Command));
   cmd->type = SCSI_QUEUE_COMMAND; 
  /*
   * the Clariion must be handled differently since a TEST UNIT is
   * not appropriate. Issue an INQUIRY for page 0xC0 instead.
   */
   cmd->cdb[0] = SCSI_CMD_INQUIRY;
   cmd->cdb[1] = 0x1;   // EVPD = 1
   cmd->cdb[2] = 0xC0;
   cmd->cdb[4] = DGC_INQ_DATA_LEN;
   response = (unsigned char *)Mem_Alloc(DGC_INQ_DATA_LEN);
   ASSERT(response != NULL);

   cmd->cdbLength = 6;
   cmd->dataLength = DGC_INQ_DATA_LEN;
   cmd->sgArr.length = 1;
   cmd->sgArr.addrType = SG_MACH_ADDR;
   cmd->sgArr.sg[0].addr = VMK_VA2MA(VA)(response));
   cmd->sgArr.sg[0].length = DGC_INQ_DATA_LEN;
   
   cmd->flags = SCSI_CMD_IGNORE_FAILURE | SCSI_CMD_PRINT_NO_ERRORS;
   if (bypass) {
      cmd->flags |= SCSI_CMD_BYPASSES_QUEUE;
   }
   status = SCSISyncCommand(handle, cmd, path, TRUE);

   Mem_Free(response);
   Mem_Free(cmd);
   if (status == VMK_OK) {
      returnValue =  0;
   } else if ( status == VMK_NOT_READY) {
      returnValue =  1;
   } else if ( status == VMK_NO_CONNECT) {
      returnValue =  2;
   } else {
      ASSERT( status != VMK_WOULD_BLOCK );
      Warning("DGCInquiryC0 returned: %s", VMK_ReturnStatusToString(status));
   }
   
   LOG(1,"DGCInquiryC0 on %s:%d:%d returned %s",
      path->adapter->name, path->id, path->lun,
      VMK_ReturnStatusToString(status));

   return(returnValue); 
}
#endif

/*
 * SCSICheckPathReady()
 * 
 * 	Issue a CheckUnitReady command to the device using
 * the specified path. For some devices, this may not be sufficient
 * to indicate that the path is active.
 *
 * On the IBM FAStT, if both controllers of a Dual Active setup are
 * present and available, the TUR command will return success for a
 * commmand issued to the alternate controller, but READ/WRITE commands
 * to the controller will fail. Need to verify that the specified path
 * reflects the primary controller for the LUN.
 *
 * Results: 
 *	0 is the unit is ready
 *	1 if the unit is not ready
 *	2 unit is not connected (dead path)
 *	3 request was not sent because it would have been queued
 *	-1 on error
 *
 * Side effects:
 *	None.
 *
 */
static int
SCSICheckPathReady(SCSI_Handle *handle, SCSI_Path *path, Bool allowWouldBlock)
{ 
   int status;

   status = SCSICheckUnitReadyCommand(handle, path, allowWouldBlock);
   if ((status == 0) &&
       ((path->target->flags & SCSI_DEV_FASTT) &&
        (path->target->flags & SCSI_SUPPORTS_MANUAL_SWITCHOVER))){
      if (SCSIFAStTLUNUsingPreferredController(handle,path) != TRUE) {
         status = 1;
      }
   }
   return(status);
}

/*
 * Issue a SCSI START_UNIT command to the active path with the start bit
 * set to 1.  Set the cmd flags such that the command will not be retried
 * if the device returns a NOT READY error.
 *
 * Return:
 *	VMK_OK if the device responds sucessfully
 */
static VMK_ReturnStatus
SCSIStartUnitCommand(SCSI_Handle *handle)
{ 
   SCSI_Command *cmd; 
   VMK_ReturnStatus status;

   cmd = (SCSI_Command *)Mem_Alloc(sizeof(SCSI_Command));

   ASSERT(cmd != NULL);
   memset(cmd, 0, sizeof(SCSI_Command));
   cmd->type = SCSI_QUEUE_COMMAND;
   cmd->cdb[0] = SCSI_CMD_START_UNIT;
   cmd->cdb[4] = 0x1;		/* the 1 bit means to start the device (0 means to stop the device) */ 
   cmd->cdbLength = 6;
   cmd->flags |= SCSI_CMD_BYPASSES_QUEUE | SCSI_CMD_IGNORE_FAILURE;
   status = SCSISyncCommand(handle, cmd, handle->target->activePath, TRUE);
   Mem_Free(cmd);

   if ((status != VMK_OK) &&
       (status != VMK_NOT_READY)) {
      ASSERT(status != VMK_WOULD_BLOCK);
      Warning("StartUnitCommand on %s:%d:%d returned %s", 
         handle->target->activePath->adapter->name, 
         handle->target->activePath->id, handle->target->activePath->lun,
	 VMK_ReturnStatusToString(status));
   }

   return(status); 
}

/*
 * SCSIDGCStartRegistration
 *
 * Start the DGC Registration process for this target.
 *
 * Return:
 *	VMK_OK if this path sucessfully executed the command
 * VMK_NO_MEMORY if memory alloc had a problem
 */
VMK_ReturnStatus
SCSIDGCStartRegistration(SCSI_Handle *handle, SCSI_Command *cmd)
{ 
   SCSI_Target *tgt = handle->target;
   SCSI_Path   *path;
   KSEG_Pair   *pair;
   void        *ptr, *vptr;
   uint32       len = 0;

   LOG(1, "AAS command info [%d:%d] (%x,%x,%x) type=%d, len=%d, p=%x, off=%d",
       tgt->id, tgt->lun,
       (unsigned int)cmd->cdb[6],
       (unsigned int)cmd->cdb[7],
       (unsigned int)cmd->cdb[8],
       cmd->sgArr.addrType,
       cmd->sgArr.sg[0].length,
       (unsigned int)cmd->sgArr.sg[0].addr, 
       (int)cmd->sgArr.sg[0].offset);
   /* check for prior invocation, since we believe the registration commands
    * are the same for each target, don't allocate or copy memory again
    */
   if (tgt->vendorDataLen) {
      LOG(0, "Prior AAS command received [%d:%d][%d, %x]",
          tgt->id, tgt->lun,
          tgt->vendorDataLen, (unsigned int)tgt->vendorData);
      /* clear all the registration flags */
      for (path = tgt->paths; path; path = path->next) {
         path->flags &= ~SCSI_PATH_REGISTRATION_DONE;
      }
      /* must be same length */
      ASSERT(cmd->sgArr.sg[0].length == tgt->vendorDataLen);
   }
   else {
      /* save the cmd data in the target struct */
      len = cmd->sgArr.sg[0].length;
      ptr = Kseg_GetPtrFromMA(cmd->sgArr.sg[0].addr,
              len, &pair);
      vptr = Mem_Alloc(len);
      if ((vptr == NULL) || (ptr == NULL)) {
         if (ptr)
           Kseg_ReleasePtr(pair);
         Warning("AAS command - memory error [%d:%d][%x, %x]",
            tgt->id, tgt->lun,
            (unsigned int)ptr, (unsigned int)vptr);
         return (VMK_NO_MEMORY);
      }
      memcpy(vptr, ptr, len);
      tgt->vendorData = vptr;
      tgt->vendorDataLen = len;
      Kseg_ReleasePtr(pair);
   }

   /* kick the target scanner to make it bark (the ASPCA has nothing on me)*/
   SCSIStateChangeCallback(NULL);

   return (VMK_OK);
}


/*
 * SCSIDGCRegistrationCommand
 *
 * Issue a DGC Registration command (Advanced Array Setup Command) to
 * this path. The AAS command is a Vendor Unique command with a
 * string of CTLDs sent as data.
 * Set the cmd flags such that the command will not be retried if
 * the device returns a NOT READY error. 
 *
 * Return:
 *	VMK_OK if this path sucessfully executed the command
 */
static VMK_ReturnStatus
SCSIDGCRegistrationCommand(SCSI_Handle *handle, SCSI_Path *path)
{ 
   SCSI_Command *cmd;
   SCSI_Target  *target;
   VMK_ReturnStatus status;

   target = path->target;
   cmd = (SCSI_Command *)Mem_Alloc(sizeof(SCSI_Command));
   ASSERT(cmd != NULL);
   memset(cmd, 0, sizeof(SCSI_Command));

   cmd->type = SCSI_QUEUE_COMMAND;
   cmd->cdb[0] = DGC_AAS_CMD;
   cmd->cdb[2] = 0x1;   // Database ID
   ASSERT(target->vendorDataLen < 255);
   cmd->cdb[8] = target->vendorDataLen;
   cmd->cdbLength = 10;
   cmd->dataLength = target->vendorDataLen;
   cmd->sgArr.length = 1;
   cmd->sgArr.addrType = SG_MACH_ADDR;
   cmd->sgArr.sg[0].addr = VMK_VA2MA((VA)(target->vendorData));
   cmd->sgArr.sg[0].length = target->vendorDataLen;
   
   cmd->flags = SCSI_CMD_BYPASSES_QUEUE | SCSI_CMD_IGNORE_FAILURE; 
   status = SCSISyncCommand(handle, cmd, path, TRUE);
   Mem_Free(cmd); 

   if ((status != VMK_OK) &&
       (status != VMK_NOT_READY)) {
      ASSERT(status != VMK_WOULD_BLOCK);
      Warning("SCSIDGCRegistrationCommand on %s:%d:%d returned %s",
         handle->target->activePath->adapter->name, 
         handle->target->activePath->id, handle->target->activePath->lun,
	 VMK_ReturnStatusToString(status));
   }

   return(status); 
}


/*
 * Issue a DGC Trespass command (Mode Select) to this LUN. A trespass
 * command is a Mode Select command with page 0x22 sent as data.
 * Set the cmd flags such that the command will not be retried if
 * the device returns a NOT READY error. 
 *
 * Return:
 *	VMK_OK if the device responds sucessfully
 */
static VMK_ReturnStatus
SCSIDGCTrespassCommand(SCSI_Handle *handle)
{ 
   SCSI_Command *cmd;
   VMK_ReturnStatus status;
   unsigned char  *tp;

   cmd = (SCSI_Command *)Mem_Alloc(sizeof(SCSI_Command));
   ASSERT(cmd != NULL);
   memset(cmd, 0, sizeof(SCSI_Command));

   tp = (unsigned char *)Mem_Alloc(TRESPASS_LEN);
   ASSERT(tp != NULL);
   memset(tp, 0, TRESPASS_LEN);
   tp[3] = 0x8;	    // Mode Page Header - Block Descriptor Length
   tp[10]= 0x2;       // Block Descriptor - block size (0x200)
   tp[12] = 0x22;     // Trespass page code
   tp[13] = 0x2;      // page length = 2
   tp[14] = 0x1;      // HR = 0, TP = 1
 //tp[15] = 0xff;     // 0xff = trespass LUN this is sent to **THIS SHOULD HAVE WORKED**
   tp[15] = handle->target->activePath->lun;     // trespass this LUN
   
   cmd->type = SCSI_QUEUE_COMMAND;
   cmd->cdb[0] = SCSI_CMD_MODE_SELECT;
   cmd->cdb[1] = 0x0;   // PF=0, SP=0
   cmd->cdb[4] = TRESPASS_LEN;
   cmd->cdbLength = 6;
   cmd->dataLength = TRESPASS_LEN;
   cmd->sgArr.length = 1;
   cmd->sgArr.addrType = SG_MACH_ADDR;
   cmd->sgArr.sg[0].addr = VMK_VA2MA((VA)(tp));
   cmd->sgArr.sg[0].length = TRESPASS_LEN;
   
   cmd->flags = SCSI_CMD_BYPASSES_QUEUE | SCSI_CMD_IGNORE_FAILURE; 
   status = SCSISyncCommand(handle, cmd, handle->target->activePath, TRUE); 
   Mem_Free(tp); 
   Mem_Free(cmd); 

   if ((status != VMK_OK) &&
       (status != VMK_NOT_READY)) {
      ASSERT(status != VMK_WOULD_BLOCK);
      Warning("DGCTrespassCommand on %s:%d:%d returned %s", 
         handle->target->activePath->adapter->name, 
         handle->target->activePath->id, handle->target->activePath->lun,
	 VMK_ReturnStatusToString(status));
   }

   return(status); 
}


/*
 * SCSIActivatePath
 *
 * 	Start the SCSI path. Usually this is a SCSI START_UNIT command,
 * but the EMC Clariion requires a TRESPASS command, and the IBM FAStT
 * in A/P mode requires a MODE_SELECT command. The SVC does not require
 * a command to use a different path, a NOT READY status just means switch
 * paths.
 *
 * Return:
 *	VMK_OK if the device responds sucessfully
 *
 * Side effects:
 *      none
 */
static VMK_ReturnStatus
SCSIActivatePath(SCSI_Handle *handle)
{
   if (handle->target->flags & SCSI_DEV_SVC) {
      return(VMK_OK);
   } else if (handle->target->flags & SCSI_DEV_DGC) {
      return(SCSIDGCTrespassCommand(handle));
   } else if ((handle->target->flags & SCSI_DEV_FASTT) &&
	      (handle->target->flags & SCSI_SUPPORTS_MANUAL_SWITCHOVER)) {
      return(SCSIFAStTSetPreferredController(handle));
   } else {
      return(SCSIStartUnitCommand(handle));
   }
}

/*
 *  SCSIPullLunsToStandByDevice
 *
 *  Issue the SCSI commands to cause the active/passive device to switch 
 *  to the controller specified by handle->target->activePath.
 *
 *  Returns:
 *     TRUE if the manual switchover was successful
 *     FALSE otherwise
 *
 *  Side effects:
 *     none
 */
static Bool
SCSIPullLunsToStandbyDevice(SCSI_Handle *handle)
{  
    Bool pullOverWorked = FALSE;
    SCSI_Path *activePath; 
    VMK_ReturnStatus status;
    int result = -1;
    
    activePath = handle->target->activePath; 
    result = SCSICheckPathReady(handle, activePath, FALSE);

    if ( result == 1) { 
        if ((status = SCSIActivatePath(handle)) == VMK_OK) { 
            if (SCSICheckPathReady(handle, activePath, FALSE) == 0) { 
               pullOverWorked = TRUE; 
	    } else {
               Warning("Could not switchover to %s:%d:%d. Check Unit Ready Command failed AFTER Start Unit.",
		   activePath->adapter->name, activePath->id, activePath->lun);
	    } 
	} else { 
           Warning("Could not switchover to %s:%d:%d. Start Unit Command failed with %s",
		   activePath->adapter->name, activePath->id, activePath->lun,
                   VMK_ReturnStatusToString(status));
        }
    } else if ( result == 0) {
        /* 
         * This is a valid case. The situtation is probably that the
         * system booted and the 2nd device was controlling the
         * luns. I/O requests to the 1st device will return with a CHECK
         * CONDITION - device NOT READY and the code will end up
         * here. Since the 2nd device is already active, just issue the
         * I/O requests to it.
         */
        Warning("Did not switchover to %s:%d:%d. Check Unit Ready Command returned READY instead of NOT READY for standby controller .",
		   activePath->adapter->name, activePath->id, activePath->lun);
	pullOverWorked = TRUE;
    } else { 
        Warning("Could not switchover to %s:%d:%d. Check Unit Ready Command returned an error instead of NOT READY for standby controller .", activePath->adapter->name, activePath->id, activePath->lun);
    }
    return( pullOverWorked );
}

/*
 *----------------------------------------------------------------------
 *
 * SCSISelectPathWithState
 *
 * 	This routine is called only from the SCSIChoosePath() routine during
 * 	path failover. Search the list of paths to the specified target and 
 * 	select a path in the specified state, either SCSI_PATH_ON or
 *      SCSI_PATH_STANDBY.
 *
 *	If the target SAN requires manual switchover (i.e. the
 *	SCSI_SUPPORTS_MANUAL_SWITHOVER flag is set) First look for a path
 *	in the specified state that we have not yet tried to switchover to
 *	in a while. This will prevent thrashing between SPs.  If no such
 *	path is found then just clear the SCSI_PATH_FAILOVER_TRIED flag on
 *	all paths and start again.
 *
 * Results: 
 *	A pointer to a path in the specified state or the initial path if
 *	such a path does not exist
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static SCSI_Path *
SCSISelectPathWithState(SCSI_Path *path, SCSI_Target *target, int state)
{ 
   SCSI_Path *initPath = path;
   Bool found = FALSE;
   Bool foundPathInCorrectState = FALSE;

   /*
    * find a path in the given state that has not yet been tried for failover.
    */
   if (target->flags & SCSI_SUPPORTS_MANUAL_SWITCHOVER) {
      do { 
         if ((path->state == state) && 
	     (!(path->flags & SCSI_PATH_FAILOVER_TRIED))) {
            LOG(0,"Selecting path: %s:%d:%d. Failover has not yet been tried.", 
	       path->adapter->name, path->id, path->lun);
            found = TRUE;
	    foundPathInCorrectState = TRUE;
            break;
         } else if (path->state == state) {
            LOG(0,"Skipping path: %s:%d:%d. Proper state but failover already tried on this path.", 
	       path->adapter->name, path->id, path->lun);
	    foundPathInCorrectState = TRUE;
         }

         path = path->next;
         if (path == NULL) {
            path = target->paths;
         }
      } while (path != initPath); 

      if ((!found) && (foundPathInCorrectState)) {
         LOG(1,"Clear SCSI_PATH_FAILOVER_TRIED flags and start again.");
         do {
	    path->flags &= ~SCSI_PATH_FAILOVER_TRIED;
            path = path->next;
            if (path == NULL) {
               path = target->paths;
            }
         } while (path != initPath); 
      }
   }

   if (!found) {
      /*
       * If all of the paths in this state have already been tried, 
       * then select any path in the correct state.
       */
      do { 
         if (path->state == state){
            break;
         }
         path = path->next;
         if (path == NULL) {
            path = target->paths;
         }
      } while (path != initPath);
   } 
   return( path );
}


/*
 *----------------------------------------------------------------------
 *
 * SCSILocateReadyPath
 *
 * 	This routine is called only from the SCSIChoosePath() routine during
 * 	path failover. Search the list of paths to the specified target and 
 * 	select a path that responds READY to a TEST_UNIT_READY command. 
 * 	First search for a path that has not been tried recently - the
 * 	SCSI_PATH_FAILOVER_TRIED flag is off. With the EMC Clarrion there
 * 	are conditions where a path returns READY, but does not accept I/O
 * 	requests so be sure to cycle thru all READY paths.
 * 	If such a READY path cannot be found, search all paths.
 *      
 * Results: 
 *	A pointer to a path that is READY or NULL if such a path does not exist
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static SCSI_Path *
SCSILocateReadyPath(SCSI_Handle *handle, SCSI_Path *path, SCSI_Target *target)
{ 
   SCSI_Path *initPath = path;
   Bool found = FALSE;
   int  ret;

   /*
    * look for a path that has not been tried recently and 
    * returns READY to a TUR command
    */
   if (target->flags & SCSI_SUPPORTS_MANUAL_SWITCHOVER) {
      do {
         if ((path->state != SCSI_PATH_OFF) &&
             (!(path->flags & SCSI_PATH_FAILOVER_TRIED)) &&
             ((ret = SCSICheckPathReady(handle, path, FALSE)) == 0)) {
            found = TRUE;
            break;
         }
         path = path->next;
         if (path == NULL) {
            path = target->paths;
         }
      } while (path != initPath);
   }

   if (!found) {
      /*
       * look for any path that returns READY to a TUR command
       */
      do {
      if ((path->state != SCSI_PATH_OFF) &&
             ((ret = SCSICheckPathReady(handle, path, FALSE)) == 0)) {
            found = TRUE;
            break;
         }

         path = path->next;
         if (path == NULL) {
            path = target->paths;
         }
      } while (path != initPath);
   }

   if (!found) {
      return( NULL ); 
   }
   return( path );
}


#define BLOCKS_PER_MBYTE 2048

/*
 *----------------------------------------------------------------------
 *
 * SCSIRoundRobinPolicy
 *
 * Given the current active path 'activePath', choose another path based on a
 * round-robin (load-balancing) policy.  Return activePath if it is not time to
 * change or no good candidate is available.
 *
 * Possible policies:
 *  - switch on every Mbyte of bandwidth to the target (policy below)
 *  - switch on each new command to hba or target
 *  - choose hba with max of (queue_depth - outstanding commands)
 *  - choose hba with minimum total bandwidth on outstanding cmds
 *  - choose new hba only if bandwidth to the current hba is at maximum
 *----------------------------------------------------------------------
 */
static SCSI_Path *
SCSIRoundRobinPolicy(SCSI_Path *activePath, SCSI_ResultID *rid)
{
   SCSI_Target *target = activePath->target;

   uint32 blocks = target->stats.blocksRead + target->stats.blocksWritten;

   /* See if we should try to change paths.  Change to a new path
    * for every Mbyte read or written on the target. */
   if (FLOOR(blocks, BLOCKS_PER_MBYTE) !=
       FLOOR(blocks + rid->cmd->dataLength/DISK_SECTOR_SIZE,
	     BLOCKS_PER_MBYTE)) {
      SCSI_Path *path = activePath;

      /* If so, look for another ON path with different adapter, but
       * same target id. */
      do { 
	 path = path->next;
	 if (path == NULL) {
	    path = target->paths;
	 }
	 if (path->state == SCSI_PATH_ON && path->id == activePath->id) {
	    break;
	 }
      } while (path != activePath);
      activePath = path;
   }
   return activePath;
}


/*
 *----------------------------------------------------------------------
 * SCSIResetOnPath() 
 *
 *	Send a reset down a SCSI path
 *
 * Results:
 *	none
 * 
 * Side Effects: 
 * 	none
 *----------------------------------------------------------------------
 */
static void
SCSIResetOnPath(SCSI_Handle *handle, SCSI_Adapter *adapter, SCSI_Path *path)
{
   SCSI_Command cmd;
   SCSI_ResultID rid;
   VMK_ReturnStatus status;
   
   /* 
    * If the adapter changed and the config option is set, then issue a
    * reset directly on this path to clear any reservations held on the
    * failed adapter, so this adapter can issue commands. 
    */
   SCSISetupResetCommand(handle, &cmd, &rid);
   rid.cmd = &cmd;
   rid.path = path;
   
   status = adapter->command(path->adapter->clientData, &cmd, &rid,
                             handle->worldID);

   if (status != VMK_OK) {
      Warning("Reset during HBA failover returns %d", status);
   }
}


/*
 *----------------------------------------------------------------------
 * SCSIChoosePath() 
 *
 *    Choose a path to issue the next command for a handle.  Change
 *    target->activePath as necessary and fill-in rid->path with the chosen
 *    path. 
 *    Some path changes require a manual switchover or a SCSI reset.
 *    If this routine is called from the context of a Helper World  
 *    where it is safe to block, then the reset and manual switchover
 *    can be performed. If the active path needs to be changed, but this
 *    routine is called from a context where it is not possible to perform the
 *    change, then delay the path change activity and use the current active path.
 *
 *    Note:
 *       rid->cmd should be initialized so it can be used by the round-robin policy.
 *
 * Results:
 *	none
 * 
 * Side Effects: 
 * 	none
 *----------------------------------------------------------------------
 */
void
SCSIChoosePath(SCSI_Handle *handle, SCSI_ResultID *rid)
{
   SCSI_Target *target = handle->target;
   SCSI_Adapter *adapter = target->adapter;
   SCSI_Path *path;
   Bool doReset = FALSE;
   Bool helperWorldSafeToBlock = FALSE;
   Bool pullLuns = FALSE;

   if (rid && rid->token && (!(rid->token->flags & ASYNC_CANT_BLOCK))) {
      if (World_IsSafeToBlock() && World_IsHELPERWorld(MY_RUNNING_WORLD)) {
         helperWorldSafeToBlock = TRUE;
      }
   }

   SP_Lock(&adapter->lock);
   
   /*
    * It is possible for a thread to initiate a path failover from
    * other than the designated helper world. 
    *
    * The DelayCmdsCount will be incremented when a failover process is 
    * initiated. This will prevent SCSIIssueCommand() from sending commands
    * to the target until the failover is complete. However, if the activepath
    * for a target gets set to DEAD/STANDBY state by some method other than the
    * SCSI_DoCommandComplete() routine then it is possible for a thread other
    * than the failover world to call SCSIChoosePath() and intiate the path 
    * failover. The activePath target state can be set directly by the user 
    * thru the target proc node, or from the SCSIEvaluateAdapterTargets() 
    * routine when the path has been determined not to be working.
    *
    * So if failover is underway, just return the current path.
    */
   if (target->flags & SCSI_MANUAL_SWITCHOVER_UNDERWAY) {
      rid->path = target->activePath;
      SP_Unlock(&adapter->lock);
      LOG(0,"Failover underway, using current path for target %s:%d:%d",
	     rid->path->adapter->name, rid->path->id, rid->path->lun);
      return;
   }
   
   /*
    * Select the path to the target based on the following criteria:
    *
    *   1) with the FIXED policy, use the preferred path if it is in the
    *      ON or STANDBY state
    *   2) then use the active path if it is in the ON
    *   3) then select any path in the ON state
    *   4) then select any path in the STANDBY state
    * 
    *   1) with the MRU policy, use the active path if it is in the ON state 
    *   2) if active/active disk array, select any path in ON state
    *   3) if active/passive disk array, explicitly test all paths to see
    *      if they are ON (i.e. are working and go to a ready controller)
    *   4) then select any path in the STANDBY state
    *
    *   1) with the ROUND_ROBIN policy, if enough bandwidth has gone on
    *      the current path, switch to the next path that is in the ON
    *      state and has the same target id as the current path.
    *   2) then select any path in the ON state
    *   3) then select any path in the STANDBY state
    *
    */
   if ((target->policy == SCSI_PATH_FIXED) &&
       ((target->preferredPath->state == SCSI_PATH_ON) || 
        (target->preferredPath->state == SCSI_PATH_STANDBY))) {
      path = target->preferredPath;
   } else {
      /* 
       * Use the activePath if the target policy is SCSI_PATH_MRU or
       * as the first fallback if the target policy is SCSI_PATH_FIXED
       * or if not changing path during SCSI_PATH_ROUND_ROBIN.
       */
      path = target->activePath;

      if (target->policy == SCSI_PATH_ROUND_ROBIN &&
	  !(target->flags & SCSI_RESERVED_LOCAL) &&
	  !(target->pendingReserves > 0)) {
	 /* Don't do a round-robin path switch if the target is
	  * currently reserved by the local host. */
	 path = SCSIRoundRobinPolicy(path, rid);
      }

      if ( (path->state != SCSI_PATH_ON) &&
           (target->policy == SCSI_PATH_MRU) &&
	   (target->flags & SCSI_SUPPORTS_MANUAL_SWITCHOVER) &&
           helperWorldSafeToBlock ) {
	 
         SCSI_Path *workingPath = NULL;
         /*
          * It may not be necessary to do a manual switchover. There could
          * be another working path. Look for it. This prevents thrashing
          * when two hosts are attached to a single SAN and they are both
          * trying to do failovers. A side effect of this code is that the
          * activePath may not be set to the preferredPath, eventhough the
          * preferred path is operational.
	  *
	  * The adapter lock has to be released here in order to issue the
	  * TEST_UNIT_READY commands.  It is necessary to re-check if
	  * another thread has raced thru and started a switchover after
	  * the lock is re-obtained.
	  */ 
         SP_Unlock(&adapter->lock);
         workingPath = SCSILocateReadyPath(handle, target->activePath, target); 
         SP_Lock(&adapter->lock); 
         if (target->flags & SCSI_MANUAL_SWITCHOVER_UNDERWAY) {
           SP_Unlock(&adapter->lock);
           LOG(0,"SCSIChoosePath - SWITCHOVER UNDERWAY");
	   rid->path = target->activePath;
	   return;
         } 

         if (workingPath != NULL) {
            /*
	     * Found a path that was already working. It is not necessary
	     * to do a manual switchover.
	     */
            path = workingPath; 

	    /*
	     * This path is READY so it should be in the ON state
	     */
	    SCSIMarkPathOn(path);
         } else if (target->flags & SCSI_DEV_SVC) {
           Log("SCSIChoosePath - None of the paths to SVC device %s:%d:%d are working",
	      path->adapter->name, path->id, path->lun);
         }
      }
        
      if (path->state != SCSI_PATH_ON) {
	 /* If chosen path is not on, look next for any 'on' path and
	  * then for any 'standby' path. */
	 path = SCSISelectPathWithState(path, target, SCSI_PATH_ON); 
	 if (path->state != SCSI_PATH_ON) {
	    path = SCSISelectPathWithState(path, target, SCSI_PATH_STANDBY); 
	 }
      }
   }

   /*
    * Determine if any actions are necessary to use the selected path.
    */
   if ( (path != target->activePath) || (path->state == SCSI_PATH_STANDBY) ) {
      ASSERT(path->state == SCSI_PATH_ON || path->state == SCSI_PATH_STANDBY);

      if (path->adapter != target->activePath->adapter &&
	  (CONFIG_OPTION(DISK_RESET_ON_FAILOVER) ||
	   (target->flags & SCSI_RESERVED_LOCAL) ||
	   target->pendingReserves > 0)) {
	 /* Want to do a bus reset during HBA failover. */
	 doReset = TRUE;
      }
      if ((doReset || (target->flags & SCSI_SUPPORTS_MANUAL_SWITCHOVER)) &&
	  !helperWorldSafeToBlock) {
	 /*
	  * If it's not safe to do a failover (because we need to do a bus
	  * reset or a start unit command), then just issue command on
	  * current path.  Failover will occur next time SCSIChoosePath is
	  * called from a safe context.
	  */
	  Warning("Delaying failover to path %s:%d:%d",
	     path->adapter->name, path->id, path->lun);
          SP_Unlock(&adapter->lock);
          rid->path = target->activePath;
          return;
      } else {
	 if (target->flags & SCSI_SUPPORTS_MANUAL_SWITCHOVER){
	    target->flags |= SCSI_MANUAL_SWITCHOVER_UNDERWAY;
            pullLuns = TRUE;
	    if (target->activePath->state == SCSI_PATH_ON) {
	       SCSIMarkPathStandby(target->activePath);
	    }
	    Warning("Manual switchover to path %s:%d:%d begins.",
		    path->adapter->name, path->id, path->lun);
	 } 
	 if (target->policy != SCSI_PATH_ROUND_ROBIN) {
	    Log("Changing active path to %s:%d:%d",
		path->adapter->name, path->id, path->lun);
	 }
	 target->activePath = path;
      }
   }
    
   SP_Unlock(&adapter->lock);

   if (doReset) {
      SCSIResetOnPath(handle, adapter, path);
   } 

   if (pullLuns) {
      Bool success;

      ASSERT(helperWorldSafeToBlock);
      success = SCSIPullLunsToStandbyDevice(handle);
      SP_Lock(&adapter->lock); 
      if (success) {
         Warning("Manual switchover to %s:%d:%d completed successfully.",
		 path->adapter->name, path->id, path->lun);
	 /*
	  * The active path is now in ON state. 
	  */
	 SCSIMarkPathOn(target->activePath);
      } else {
         Warning("Manual switchover to %s:%d:%d completed unsuccessfully.",
		 path->adapter->name, path->id, path->lun); 
         /*
	  * If the path is in DEAD state, leave it alone. There are no
	  * working paths to the target.  Otherwise, set the path to
	  * STANDBY. On the next I/O request, the code will select a
	  * different STANDBY path and retry the switchover process.
	  */
         if (target->activePath->state != SCSI_PATH_DEAD) {
	    SCSIMarkPathStandby(target->activePath);
	 }
      } 

      ASSERT( target->flags & SCSI_MANUAL_SWITCHOVER_UNDERWAY);
      target->flags &= ~(SCSI_MANUAL_SWITCHOVER_UNDERWAY);
      target->activePath->flags |= SCSI_PATH_FAILOVER_TRIED;
      SP_Unlock(&adapter->lock); 
   }
   rid->path = path;
}

/*
 *----------------------------------------------------------------------
 * SCSILogPath() 
 *
 *	Print log output.
 *
 * Results:
 *	none
 * 
 * Side Effects: 
 * 	none
 *----------------------------------------------------------------------
 */
static inline void
SCSILogPathState(SCSI_Path *path, int wasState, int isState)
{
   LOG(1,"%s:%d:%d PATH OLD STATE: %s, NEW STATE: %s", path->adapter->name, path->id, path->lun,
      (wasState == SCSI_PATH_ON) ? "on" : (wasState == SCSI_PATH_STANDBY) ? "standby" : (wasState == SCSI_PATH_OFF) ? "off" : "dead",
      (isState == SCSI_PATH_ON) ? "on" : (isState == SCSI_PATH_STANDBY) ? "standby" : (isState == SCSI_PATH_OFF) ? "off" : "dead"); 
} 

/*
 *----------------------------------------------------------------------
 * SCSIExtractSenseData
 *
 *	Apply the SCSISenseData template to the sense buffer and
 *	extract the key, asc, and ascq fields. This is more readable
 *	then using senseBuffer[2], senseBuffer[12], and senseBuffer[13]. 
 *
 * Note:
 *	Check for valid contents of the senseBuffer. The "error" field
 * 	should be 0x70 or 0x71. The check for 0x0 is necessary because sometimes
 *	this routine is called with a zero buffer.
 *
 * Results: 
 *	TRUE if the sense data was valid and the extraction was successful
 *	If TRUE then output parameters sense_key, asc, ascq contain 
 *	contain extracted sensedata, otherwise their values are undefined.
 *----------------------------------------------------------------------
 */
static Bool 
SCSIExtractSenseData(SCSISenseData *senseBuffer, unsigned char *sense_key, unsigned char *asc, unsigned char *ascq)
{ 

   if ((senseBuffer->error == SCSI_SENSE_ERROR_CURCMD)  ||
       (senseBuffer->error == SCSI_SENSE_ERROR_PREVCMD) ||
       (senseBuffer->error == 0x0)) {
      *sense_key = senseBuffer->key;
      *asc = (senseBuffer->optLen >= 5) ? senseBuffer->code : 0;
      *ascq = (senseBuffer->optLen >= 6) ?  senseBuffer->xcode : 0;
      return( TRUE ); 
   } else {
      LOG(0,"Invalid sense buffer:  error = 0x%x, valid = 0x%x, segment =  0x%x, key = 0x%x",
         senseBuffer->error, senseBuffer->valid, senseBuffer->segment, senseBuffer->key); 
      return( FALSE); 
   }
}

/*
 *----------------------------------------------------------------------
 * SCSIDeviceNotReady
 *
 *      Query the check condition sense data and determine if the
 *      device is waiting for a START UNIT command to be issued.
 *	This is only the case if the device supports multipath with
 *  	manual failover. The devices that support manual failover have
 *	different ways of reporting the not ready condition.	
 *	
 *	
 * Results:
 *	TRUE if the device is waiting for a START UNIT command 
 * 
 *----------------------------------------------------------------------
 */
Bool
SCSIDeviceNotReady(SCSI_Target *target, SCSI_Status status, SCSISenseData *senseBuffer)
{
   unsigned char sense_key = 0;
   unsigned char asc = 0;
   unsigned char ascq = 0;

   if ((target->flags & SCSI_SUPPORTS_MANUAL_SWITCHOVER) &&
       (SCSI_DEVICE_STATUS(status) == SDSTAT_CHECK)) {
      if ( SCSIExtractSenseData(senseBuffer, &sense_key, &asc, &ascq) ) {
         /* Clariion case */
         if ((target->flags & SCSI_DEV_DGC) && 
            ((sense_key == SCSI_SENSE_KEY_NOT_READY) ||
             (sense_key == SCSI_SENSE_KEY_ILLEGAL_REQUEST )) &&
            (asc == SCSI_ASC_LU_NOT_READY ) &&
            (ascq == SCSI_ASC_LU_NOT_READY_ASCQ_MANUAL_INTERVENTION_REQUIRED) ) {
            return( TRUE );
         }
	 /* IBM FAStT case */
	 if ((target->flags & SCSI_DEV_FASTT) &&
            (sense_key == SCSI_SENSE_KEY_ILLEGAL_REQUEST) &&
            (asc == SCSI_ASC_INVALID_REQ_DUE_TO_CURRENT_LU_OWNERSHIP) &&
            (ascq == SCSI_ASCQ_INVALID_REQ_DUE_TO_CURRENT_LU_OWNERSHIP)) {
            return( TRUE );
         }
         if ( (sense_key == SCSI_SENSE_KEY_NOT_READY ) ||
              ((sense_key == SCSI_SENSE_KEY_ILLEGAL_REQUEST )  &&
               (asc == SCSI_ASC_LU_NOT_READY)  &&
               (ascq == SCSI_ASC_LU_NOT_READY_ASCQ_INIT_CMD_REQUIRED)
	      )
             ) {
            return( TRUE );
         }
      }
   }
   return( FALSE );
}


/*
 *----------------------------------------------------------------------
 * SCSIPathDead() 
 *
 *	Determine if the path is broken or missing.
 *	If a path returns a NO_CONNECT status it is considered DEAD.
 *
 * Results:
 *	TRUE if the device is broken or missing.
 *
 * Side Effects:
 * 	none
 *----------------------------------------------------------------------
 */
Bool
SCSIPathDead(SCSI_Target *target, SCSI_Status status, SCSISenseData *senseBuffer)
{
   if (SCSI_HOST_STATUS(status) == SCSI_HOST_NO_CONNECT) {
      return( TRUE );
   }
   return( FALSE );
}

/*
 *----------------------------------------------------------------------
 * SCSIDeviceIgnore
 *
 *      Check if the device is a gatekeeper LUN that does not respond
 *      as a disk device.
 *
 * Results:
 *      TRUE if the device can be ignored
 *
 *----------------------------------------------------------------------
 */
Bool
SCSIDeviceIgnore(SCSI_Target *target)
{
   if (target->flags & SCSI_DEV_PSEUDO_DISK) {
      return( TRUE );
   }
   return( FALSE );
}

/*
 *----------------------------------------------------------------------
 *
 * SCSIEvaluateAdapterTargets --
 *
 *      Evaluate the state of each path to each target on this adapter.
 *      This will keep the path states current without having to rely on
 *	pending I/O operations.
 *
 * Results:
 *	none
 *
 * Side effects:
 *	the state of the target paths may change
 *
 *----------------------------------------------------------------------
 */
static void
SCSIEvaluateAdapterTargets(void *data)
{ 
   SCSI_Adapter *adapter = (SCSI_Adapter*)data;
   SCSI_Target *target;
   SCSI_Handle *handle;
   SCSI_Path *path;
   VMK_ReturnStatus status;
   int pathRetryCount = 0;

   LOG(1,"Start path evaluation for adapter: %s.", adapter->name);

   SP_Lock(&adapter->lock);
starteval:
   adapter->pathEvalState = PATH_EVAL_ON;
   adapter->configModified = FALSE;

   for (target = adapter->targets; target != NULL; target = target->next) {
      SCSI_FindTarget(adapter, target->id, target->lun, FALSE);
      SP_Unlock(&adapter->lock);

      if (target->partitionTable == NULL) {
	 /* Target was just created, so read the ptn table before evaluating */
	 SP_Lock(&scsiLock);
	 status = SCSIValidatePartitionTable(adapter, target);
	 SP_Unlock(&scsiLock);
	 if (status != VMK_OK) {
	    SP_Lock(&adapter->lock);
	    if (adapter->configModified) {
	       /*
		* Adapter configuration has changed, so target may not be valid
		* anymore.  Restart evaluation.
		*/
	       SCSI_ReleaseTarget(target, FALSE);
	       goto starteval;
	    } else {
	       SCSI_ReleaseTarget(target, FALSE);
	       continue;
	    }
	 }
      }

      SP_Lock(&scsiLock);
      handle = SCSIAllocHandleTarg(target, hostWorld->worldID, 0);
      SP_Unlock(&scsiLock);
      SP_Lock(&adapter->lock);

      for (path = target->paths; path != NULL; path = path->next) {
         pathRetryCount = SCSI_EVALUATE_RETRY_COUNT;
patheval:
	 if (path->active != 0) {
	    /*
	     * Do not need to evaluate this path if there are I/O requests pending.
	     * If the path is dead it will be marked dead when the I/Os complete.
	     */
	    LOG(1,"Cannot evaluate state of path with active i/o - %s:%d:%d",
		path->adapter->name, path->id, path->lun);
	 } else if (path->state != SCSI_PATH_OFF) {
	    LOG(1,"Can evaluate state of path - %s:%d:%d",
		path->adapter->name, path->id, path->lun);
	    SP_Unlock(&adapter->lock);
	    status = SCSICheckPathReady(handle, path, TRUE);
	    SP_Lock(&adapter->lock);
	    if (adapter->configModified) {
	       /*
		* Restart evaluation:
		*  - adapter configuration has changed (rescan is underway)
		*/
               LOG(0,"Restart evaluation. Configuration changed. %s:%d:%d", path->adapter->name, path->id, path->lun);
	       SP_Unlock(&adapter->lock);
	       SCSIHandleDestroy(handle);
	       SP_Lock(&adapter->lock);
	       goto starteval;
	    }
	    if (status == 3) {
               if (pathRetryCount > 0) {
	          /* 
		   * The TEST_UNIT_READY command could not be issued because the path was busy
		   * and would have had to be queued. Retry.
		   */
                  LOG(1,"Reevaluate path %s:%d:%d. Target path busy, reissue evaluate command.", 
		     path->adapter->name, path->id, path->lun);
	          pathRetryCount--;
	          goto patheval;
               } else {
                  LOG(0,"Path %s:%d:%d is busy. Path state not updated during this evaluation pass.", 
		     path->adapter->name, path->id, path->lun);
		  /* adapter->pathEvalState = PATH_EVAL_RETRY;*/
               }
	    } else if ((status == 0) && (path->state != SCSI_PATH_ON)) {
	       LOG(1,"%s:%d:%d Evaluated path state is ON",
		   path->adapter->name, path->id, path->lun);
	       SCSILogPathState(path, path->state, SCSI_PATH_ON);
	       SCSIMarkPathOn(path);
	    } else if ((status == 1) && (path->state != SCSI_PATH_STANDBY)) {
	       LOG(1,"%s:%d:%d Evaluated path state is STANDBY",
		   path->adapter->name, path->id, path->lun);
	       SCSILogPathState(path, path->state, SCSI_PATH_STANDBY);
	       SCSIMarkPathStandby(path);
	    } else if (status == 2 && (path->state != SCSI_PATH_DEAD)) {
	       LOG(1,"%s:%d:%d Evaluated path state is DEAD",
		   path->adapter->name, path->id, path->lun);
	       SCSILogPathState(path, path->state, SCSI_PATH_DEAD);
	       SCSIMarkPathDead(path);
	    }
	 }
	 /* check for DGC path registration requests
	  * when we are done evaluating this path
	  */
	 if ((target->flags & SCSI_DEV_DGC) &&
	      (target->vendorData) &&
	      !(path->flags & SCSI_PATH_REGISTRATION_DONE) &&
	      (path->state != SCSI_PATH_DEAD) ) {
	        LOG(0,"%s:%d:%d DGC Path Registration starting",
		   path->adapter->name, path->id, path->lun);
	        SP_Unlock(&adapter->lock);
	        status = SCSIDGCRegistrationCommand(handle, path);
	        SP_Lock(&adapter->lock);
	        if (status == VMK_OK)
	                path->flags |= SCSI_PATH_REGISTRATION_DONE;
	 }
         if (adapter->configModified) {
                /*
                 * Adapter configuration has changed, so target may not be valid
                 * anymore.  Restart evaluation.
                 */
	        SP_Unlock(&adapter->lock);
	        SCSIHandleDestroy(handle);
	        SP_Lock(&adapter->lock);
                goto starteval;
         }
      }
      SP_Unlock(&adapter->lock);
      SCSIHandleDestroy(handle);
      SP_Lock(&adapter->lock);
      if (adapter->configModified) {
	 /*
	  * Adapter configuration has changed, so target may not be valid
	  * anymore.  Restart evaluation.
	  */
	 goto starteval;
      }
   }

   /*
    * Redo evaluation if a state change came in while we were evaluating.
    */
   ASSERT(adapter->pathEvalState != PATH_EVAL_REQUESTED);
   if (adapter->pathEvalState == PATH_EVAL_RETRY) {
      adapter->pathEvalState = PATH_EVAL_ON;
      goto starteval;
   }
   adapter->pathEvalState = PATH_EVAL_OFF;
   SP_Unlock(&adapter->lock);

   LOG(1,"End path evaluation for adapter: %s.", adapter->name);

   if (adapter->moduleID != 0) { 
      Mod_DecUseCount(adapter->moduleID);
   }
   return;
}

/* 
 * Time to wait before trying to queue helper request to evaluate
 * path states.
 */
#define SCSI_PATH_EVALUATION_RETRY_DELAY_TIME 1000 

/*
 *----------------------------------------------------------------------
 * SCSIStartAdapterEvaluation() 
 *
 *	Perform mechanics of making helper request to run
 *	SCSIEvaluateAdapterTargets() routine.
 *
 * Results:
 *	none
 * 
 * Side Effects: 
 * 	makes helper request
 *----------------------------------------------------------------------
 */
static void
SCSIStartAdapterEvaluation(void *data, UNUSED_PARAM(Timer_AbsCycles timestamp))
{ 
   VMK_ReturnStatus rs; 
   SCSI_Adapter *adapter = (SCSI_Adapter *)data;

   rs = Helper_Request(HELPER_PATHEVAL_QUEUE, SCSIEvaluateAdapterTargets,
		       adapter);
   if (rs != VMK_OK) {
      Warning("Could not issue helper world request. Schedule path evaluation later."); 
      Timer_Add(MY_PCPU, SCSIStartAdapterEvaluation,
		SCSI_PATH_EVALUATION_RETRY_DELAY_TIME, TIMER_ONE_SHOT,
		adapter);
   }
}

#define SCSI_STATE_CHANGE_DELAY 4000

/*
 *----------------------------------------------------------------------
 * SCSIStateChangeCallback
 *
 * Set timer to start helper world to evaluate path state for each
 * adapter, if it hasn't already been requested or started.  If this entry
 * point is called while a path evaluation is underway, the evaluation
 * will run one more time.
 *
 * NOTE:
 *	All adapters must be re-evaluated each time there is a StateChange.
 *	Frequently, a StateChange is reported on an adapter which is not
 *	the primary adapter for a target. However, there have been changes
 *	to the path of the primary adapter.
 *
 * Results: 
 * 	none
 *	
 * Side Effects:
 * 	path states may be changed
 *
 *----------------------------------------------------------------------
 */
void
SCSIStateChangeCallback(void *deviceName)
{ 
   int i;
   SCSI_Adapter *adapter; 

   SP_Lock(&scsiLock);
   for (i = 0; i < HASH_BUCKETS; i++) {
      for (adapter = adapterHashTable[i]; adapter != NULL;
	   adapter = adapter->next) {
         SP_Lock(&adapter->lock);
	 if (adapter->pathEvalState == PATH_EVAL_OFF) {
            /*
             * Prevent the driver from being unloaded during evaluation.
             */
            if (adapter->moduleID != 0) {
               VMK_ReturnStatus status = Mod_IncUseCount(adapter->moduleID);
               if (status != VMK_OK) {
                  LOG(0,"Could not increment module count. Error: %s", 
                      VMK_ReturnStatusToString(status));
               } else {
	          adapter->pathEvalState = PATH_EVAL_REQUESTED;
	          Timer_Add(MY_PCPU, SCSIStartAdapterEvaluation,
	             SCSI_STATE_CHANGE_DELAY, TIMER_ONE_SHOT, adapter);
	       }
            } else {
               Warning("Cannot evaluate paths of adapter without module.\n");
            }
         } else if (adapter->pathEvalState == PATH_EVAL_ON) {
	       adapter->pathEvalState = PATH_EVAL_RETRY;
         }
         SP_Unlock(&adapter->lock);
      }
   }
   SP_Unlock(&scsiLock); 
   return;
} 

/*
 * Indicate if periodic path evaluation has been started
 */
Bool periodicAdapterEvaluationStarted = FALSE; 

/*
 *----------------------------------------------------------------------
 *
 * SCSIPeriodicCallback
 * 
 *	This function is to provide a level of abstraction
 * so that the user can dynamically change the path evaluation interval.
 * It invokes the SCSIStateChangeCallback() and then reschedules itself
 * using the current path evaluation time.
 *
 * Results:
 *      none
 *
 * Side effects:
 *	schedule next periodic callback
 *
 *----------------------------------------------------------------------
 */
static void
SCSIPeriodicCallback(void *deviceName, UNUSED_PARAM(Timer_AbsCycles timestamp))
{ 
   SCSIStateChangeCallback(deviceName);
   Timer_Add(MY_PCPU, SCSIPeriodicCallback, 
       CONFIG_OPTION(DISK_PATH_EVAL_TIME) * 1000, TIMER_ONE_SHOT, NULL);
}

/*
 *----------------------------------------------------------------------
 *
 * SCSI_StateChange
 *
 *    Function called as a result of drivers calling scsi_state_change() to
 * indicate that there has been an RSCN on the SAN or a link up event for an 
 * HBA. This may be called from an interrupt handler, so invoke 
 * SCSIStateChangeCallback via a timer.
 *    The periodic path evaluation is required for support of the IBM SVC
 * array and will be kicked off at the time of the first FC state change.
 *
 * Results:
 *      none
 *
 * Side effects:
 *	startup periodic path evalution if necessary
 *
 *----------------------------------------------------------------------
 */
void
SCSI_StateChange(char *deviceName)
{
   Timer_Add(MY_PCPU, (Timer_Callback) SCSIStateChangeCallback,
             0, TIMER_ONE_SHOT, deviceName);

   if (periodicAdapterEvaluationStarted == TRUE) {
      return;
   }
   periodicAdapterEvaluationStarted = TRUE;
   Timer_Add(MY_PCPU, SCSIPeriodicCallback, 
       CONFIG_OPTION(DISK_PATH_EVAL_TIME) * 1000,
       TIMER_ONE_SHOT, NULL);
}

VMK_ReturnStatus
SCSIParsePathCommand(SCSI_Target *target, char *p)
{
   VMK_ReturnStatus status = VMK_OK;
   char *p1;

   while (TRUE) {
      if (SCSIWordMatch(p, "policy", &p)) {
	 if (SCSIWordMatch(p, "rr", &p)) {
	    target->policy = SCSI_PATH_ROUND_ROBIN;
	    continue;
	 }
	 else if (SCSIWordMatch(p, "fixed", &p)) {
	    target->policy = SCSI_PATH_FIXED;
	 }
	 else if (SCSIWordMatch(p, "mru", &p)) {
	    target->policy = SCSI_PATH_MRU;
	    continue;
	 }
      }
      else if (SCSIWordMatch(p, "pathon", &p1) ||
	       SCSIWordMatch(p, "pathoff", &p1) ||
	       SCSIWordMatch(p, "active", &p1) ||
	       SCSIWordMatch(p, "preferred", &p1)) {
	 char *name;
	 uint32 pathid;
	 uint32 pathlun;
	 SCSI_Path *path;

	 if (SCSIParsePath(p1, &name, &pathid, &pathlun, &p1)) {
	    path = SCSIFindPath(target, name, pathid, pathlun);
	    Mem_Free(name);
	    if (path) {
	       if (strncmp(p, "pathon", 6) == 0) {
		  if (path->state == SCSI_PATH_OFF) {
		     /* Can only turn path on if it is off. */
		     if (target->flags & SCSI_SUPPORTS_MANUAL_SWITCHOVER) {
                        /*
			 * If this is a manual switchover device, the state
			 * cannot be set directly to on.
			 */
		        path->state = SCSI_PATH_STANDBY;
		     } else {
		        path->state = SCSI_PATH_ON;
		     }
		  }
#ifdef VMX86_DEVEL
		  else if ((path->state == SCSI_PATH_STANDBY) ||
		           (path->state == SCSI_PATH_DEAD)) {
		     path->state = SCSI_PATH_ON;
		  }
#endif
	       }
	       else if (strncmp(p, "pathoff", 7) == 0) {
		  if (path->state == SCSI_PATH_ON ||
		      path->state == SCSI_PATH_STANDBY ||
		      path->state == SCSI_PATH_DEAD) {
		     /* 
		      * Prevent the user from turning off the last working path to a target.
		      * If none of the paths to a target seem to be working, then prevent the 
		      * user from turning all the paths to a target OFF.
		      * One path must be available, even if it is in DEAD state.
		      * PR #23706 
		      */
                     if (((path->state == SCSI_PATH_ON) || (path->state == SCSI_PATH_STANDBY)) &&
			 ((SCSIGetNumberOfPathsWithState(target, SCSI_PATH_STANDBY) +
                           SCSIGetNumberOfPathsWithState(target, SCSI_PATH_ON)) == 1)) {
	                /*
                         * Last working path to a target. The rest are DEAD or OFF.
			 */
		        status = VMK_NO_RESOURCES;
                     } else if ((SCSIGetNumberOfPathsWithState(target, SCSI_PATH_ON) +
				 SCSIGetNumberOfPathsWithState(target, SCSI_PATH_STANDBY) +
				 SCSIGetNumberOfPathsWithState(target, SCSI_PATH_DEAD)) == 1) {
	                /*
                         * Last path to a target that is not OFF.
			 */
		        status = VMK_NO_RESOURCES;
		     } else {
#ifndef VMX86_DEVEL
		  	/*
		  	 * The active path cannot be turned off in a
		  	 * release build while I/O is pending to the
		  	 * device. PR #23707.
		   	 */
			if ((path == target->activePath) && (target->active > 0)) {
		  		status = VMK_BUSY; 
			} else
#endif
		           path->state = SCSI_PATH_OFF;
		     }
		  }
	       }
	       else if (strncmp(p, "preferred", 9) == 0) {
#ifndef VMX86_DEVEL
	          /*
		   * The active path cannot be switched in a release build
		   * while I/O is pending to the device, PR #23707.
		   */
		  if ((target->activePath == target->preferredPath) && (target->active > 0)) {
		     status = VMK_BUSY; 
		  } else 
#endif
		  {
		     target->preferredPath = path;
		  }
	       }
	       else if (strncmp(p, "active", 5) == 0) {
#ifndef VMX86_DEVEL
		  /*
		   * The active path cannot be switched is a release build
		   * while I/O is pending to the device, PR #23707.
		   */
                  if (target->active > 0 ) {
		     status = VMK_BUSY;
                  } else 
#endif
	             target->activePath = path;
               }
	       else {
		  status = VMK_BAD_PARAM;
	       }
	       p = p1;
	       continue;
	    }
	 }
      }
      break;
   }
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * SCSIMarkPathOnIfValid
 *
 *	If the path is in STANDY_BY state and a command has successfully  
 * completed, then the path is working and the state should be changed
 * to ON. There are a few expections to this rule: 
 *   - on all SAN devices the INQUIRY command will return successfully when
 *     issued on the pasive path
 *   - on the FAStT SAN device the TEST_UNIT_READY, MODE_SENSE, 
 *     MODE_SELECT, and READ_CAPACITY commands will return successfully
 *     when issued on the passive path.
 *
 * Results:
 *      none
 *
 * Side effects:
 *	none
 *
 *----------------------------------------------------------------------
 */
void
SCSIMarkPathOnIfValid(SCSI_Target *target, SCSI_ResultID *rid)
{
   ASSERT(rid->path);
   ASSERT(rid->path->state == SCSI_PATH_STANDBY);
   if (target->flags & SCSI_MANUAL_SWITCHOVER_UNDERWAY) {
      return;
   }

   if (rid->cmd) {
      if (rid->cmd->cdb[0] == SCSI_CMD_INQUIRY) {
         LOG(1,"INQUIRY cmd - do not set path ON");
         return;
      } else if ((target->flags & SCSI_DEV_FASTT) && 
         ((rid->cmd->cdb[0] == SCSI_CMD_TEST_UNIT_READY) ||
          (rid->cmd->cdb[0] == SCSI_CMD_MODE_SENSE)      ||
          (rid->cmd->cdb[0] == SCSI_CMD_MODE_SELECT)     ||
          (rid->cmd->cdb[0] == SCSI_CMD_MODE_SENSE10)	 ||
          (rid->cmd->cdb[0] == SCSI_CMD_MODE_SELECT10)	 ||
          (rid->cmd->cdb[0] == SCSI_CMD_READ_CAPACITY))) {
         LOG(1,"TUR or MODE_SENSE cmd to a FASTtT LUN - do not set path ON");
         return;
      }
      LOG(1,"CMD 0x%x on TARGET %s:%d:%d succeeds - mark path on", rid->cmd->cdb[0],
         rid->path->adapter->name, rid->path->id, rid->path->lun);
   }

   SCSIMarkPathOn(rid->path);
}
