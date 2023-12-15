/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * These are the SCSI functions that are related to virtual SCSI
 * adapters/handles, which are used by virtual machines to access a VMFS
 * file or a disk partition as if it was a full SCSI disk.
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "libc.h"
#include "vmkernel.h"
#include "splock.h"
#include "vmk_scsi.h"
#include "scsi_int.h"
#include "fsSwitch.h"
#include "fs_ext.h"
#include "vmnix_if_dist.h"
#include "util.h"
#include "helper.h"
#include "sched_sysacct.h"
#include "config.h"
#include "world.h"
#include "host.h"
#include "vscsi_int.h"
#include "vscsi_ext.h"
#include "vscsi.h"
#include "action.h"

#define LOGLEVEL_MODULE VSCSI
#include "log.h"

static VMK_ReturnStatus VSCSIVirtOpen(VSCSI_DevDescriptor *desc, 
                                      World_ID worldID,
                                      SCSIVirtInfo *info);
static void VSCSIExecuteCommandInt(VSCSI_HandleID handleID,
                                   SCSI_Command *cmd, 
                                   VMK_ReturnStatus *result,
                                   uint32 flags,
                                   SGPinArrType* lPtr);
static void VSCSIHandleCommand(VSCSI_Handle *handle,
		               SCSI_Command *cmd,
		               VMK_ReturnStatus *result, 
		               uint32 flags,
		               SGPinArrType *lPtr);
static VMK_ReturnStatus VSCSICmdCompleteInt(VSCSI_HandleID handleID, 
                                            SCSI_Result *outResult,
                                            SGPinArrType *lPtr, 
                                            Bool *more);
static void VSCSIPostCmdCompletion(VSCSI_Handle *handle, Async_Token *token);
static void VSCSIAbortCommand(VSCSI_Handle *handle,
		              SCSI_Command *cmd,
		              VMK_ReturnStatus *result);
static void VSCSIResetHandle(VSCSI_Handle *handle,
		             SCSI_Command *cmd,
		             uint32 flags,
		             VMK_ReturnStatus *result);
static VMK_ReturnStatus VSCSICreateResetWorld(const char *name, 
                                              CpuSched_StartFunc startFunction);
static void VSCSIResetWorldFunc(void *data);
static void VSCSIResetWatchdog(void *data);
static VMK_ReturnStatus VSCSICloseDevice(World_ID worldID, VSCSI_HandleID handleID);

// Registered VSCSI Device
typedef struct VSCSIRegisteredDevice {   
   VSCSI_DevType devType;
   VSCSI_Ops *devOps;   
   struct VSCSIRegisteredDevice *next;
} VSCSIRegisteredDevice;

/*
 * Max VSCSI handles for the virt layer 
 */
#define VSCSI_MAX_HANDLES       256
#define VSCSI_HANDLE_MASK       0xff
/*
 * List of the VSCSI Handles for the virt_scsi layer..
 */
static VSCSI_Handle *vscsiHandleArray[VSCSI_MAX_HANDLES];
static SP_SpinLock vscsiHandleArrayLock;

/*
 * How many times we've gone around vscsiHandleArray[] allocating handles. 
 */
static uint32 vscsiHandleGeneration = 1;
/* 
 * Next location in vscsiHandleArray[] to look for available handle 
 */
static uint32 nextHandle = 0;

/*
 * Due to bugs in win2k SP3, msgina.dll fails to load if some commands
 * complete too quickly (see PRs 18237 and 19244).  So, we delay command
 * completion notifications if user asks to by setting delaySCSICmdsUsec
 * in the world structure.  The value is the minimum VM "virtual" time before
 * posting a notification.  We approximate virtual time by looking at time
 * VM spends running on a processor.  This could be off if we're experiencing
 * high CPU virtualization overheads. To compensate, use a higher delay
 * value.
 *
 * Some OSes also retry IOs too quickly for some SCSI statuses. This generates
 * a huge IO load on ESX and makes the VM somewhat unresponsive. Defering IO
 * completion by a couple hundred milliseconds solves the problem.
 *
 * Here are some state variables for delaying scsi completions.
 */

typedef enum { SCSI_DELAY_COMPLETION_ESX_TIME,
	       SCSI_DELAY_COMPLETION_VM_TIME } VSCSIDelayTimeType ;

// list for delayed completions
typedef struct VSCSIDelayQueue {
   SCSI_ResultID rid;
   struct VSCSIDelayQueue *next;
   World_ID worldID;
   uint64 time;
   VSCSIDelayTimeType timeType;
} VSCSIDelayQueue;

static VSCSIDelayQueue *delayQueueFirst, *delayQueueLast;

// CPU where we run the timer to post the delayed notifications
static int delayQueueCPU = MAX_PCPUS;
static Timer_Handle delayQueueTimer;

// lock to protect delayed notification state
static SP_SpinLock vscsiDelayLock;

// period for the timer that finishes up delayed notifications
#define SCSI_CMD_DELAY_PERIOD_MS  1

//List of registered VSCSI Devices
static VSCSIRegisteredDevice *vscsiDeviceList = NULL;

static VSCSI_Handle* VSCSIAllocHandle(SCSIVirtInfo *virtInfo, World_ID worldID);
static void VSCSIDelayCheckQueue(void *dummy, UNUSED_PARAM(Timer_AbsCycles timestamp));
static void VSCSIDelayCompletion(SCSI_ResultID *rid, World_Handle *world,
		                 VSCSIDelayTimeType timeType, uint64 time);

/*
 *----------------------------------------------------------------------
 *
 * VSCSI_RegisterVMMDevice
 *
 *      vmkernel call from the monitor to store information regarding the 
 *      adapter in the scsi handle.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMKERNEL_ENTRY
VSCSI_RegisterVMMDevice(DECLARE_ARGS(VMK_REGISTER_SCSI_DEVICE))
{
   PROCESS_4_ARGS(VMK_REGISTER_SCSI_DEVICE, 
                  VSCSI_HandleID, handleID,
                  uint32, channelID,
                  uint32, virtualAdapterID, 
                  uint32, virtualTargetID);
   VSCSI_Handle *handle;
   SCSIVirtInfo *virtInfo;

   handle = VSCSI_HandleFind(handleID);
   virtInfo = handle->info;
   
   if (handle == NULL) {
      Warning("Couldn't find handle 0x%x", handleID);
      return VMK_NOT_FOUND;
   }

   virtInfo->actionIndex = channelID;
   handle->virtualAdapterID = virtualAdapterID; 
   handle->virtualTargetID = virtualTargetID;
   LOG(0, "ai = %d, vAdapt = %d, vTarget = %d", virtInfo->actionIndex, 
       handle->virtualAdapterID, handle->virtualTargetID);

   VSCSI_HandleRelease(handle);
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * VSCSI_GetDeviceParam --
 *
 *      Returns device parameters such as devClass, capacity and block size
 *      to the caller.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMKERNEL_ENTRY
VSCSI_GetDeviceParam(DECLARE_ARGS(VMK_SCSI_GET_DEV_PARAM))
{
   VMK_ReturnStatus result = VMK_INVALID_HANDLE;
   SCSIVirtInfo     *virtInfo;
   PROCESS_2_ARGS(VMK_SCSI_GET_DEV_PARAM, VSCSI_HandleID, handleID,
	          SCSIDevParam*, param);

   VSCSI_Handle *handle;

   handle = VSCSI_HandleFind(handleID);
   if (handle == NULL) {
      Warning("Couldn't find handle 0x%x", handleID);
   } else {
      virtInfo = handle->info;
      ASSERT(virtInfo);
      param->devClass = virtInfo->devClass;
      param->blockSize = virtInfo->blockSize; 
      param->numBlocks = virtInfo->numBlocks;
      result = VMK_OK;
      VSCSI_HandleRelease(handle);
   }

   return result;
}

/*
 *----------------------------------------------------------------------
 *
 * SCSI_AccumulateSG --
 *
 *      Accumulate a SG buffer that is larger than default (128) entries
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	The buffer is created and hung off the handle. A flag is set in the
 *      handle, and this is cleared only when the command is finally issued.
 *
 *----------------------------------------------------------------------
 */
VMKERNEL_ENTRY
VSCSI_AccumulateSG(DECLARE_ARGS(VMK_SCSI_ACCUM_SG))
{
   VSCSI_Handle *handle;
   SCSIVirtInfo *virtInfo;
   SCSI_Command *extCmd;
   int size;
   VMK_ReturnStatus result;
   PROCESS_2_ARGS(VMK_SCSI_ACCUM_SG, VSCSI_HandleID, handleID,
	          SCSI_Command *, cmd);

   result = VMK_OK;

   handle = VSCSI_HandleFind(handleID);
   if (handle == NULL) {
      Warning("Couldn't find handle 0x%x", handleID);
      return VMK_INVALID_HANDLE;
   }

   if (handle->flags & SCSI_HANDLE_CLOSING) {
      Warning("SCSI accumulate SG on closing handle 0x%x", handleID);
      VSCSI_HandleRelease(handle);
      return VMK_INVALID_HANDLE;
   }

   virtInfo = handle->info;

   /* If this is the first in a series, allocate and initialize an Ext Cmd */
   if (!(handle->flags & SCSI_HANDLE_EXTSG)) {
      size = sizeof(SCSI_Command) + 
         ((2*cmd->sgArr.length)-SG_DEFAULT_LENGTH) * sizeof (SG_Elem);
      extCmd = (SCSI_Command *) Mem_Alloc(size);
      if (!extCmd) {
         Warning("Allocate new Cmd, No mem, len=%d",
                 cmd->sgArr.length);
         VSCSI_HandleRelease(handle);
         return VMK_NO_MEMORY;
      }
         
      handle->flags |= SCSI_HANDLE_EXTSG;
      virtInfo->sgExtCmd = extCmd;
      virtInfo->sgMax = 2*cmd->sgArr.length;
      // now copy the SG array
      size = sizeof(SG_Array);
      if (cmd->sgArr.length > SG_DEFAULT_LENGTH) {
         size += (cmd->sgArr.length - SG_DEFAULT_LENGTH) 
            * sizeof(SG_Elem);
      }
      memcpy(&(extCmd->sgArr), &(cmd->sgArr), size);
   } else {
      void *from;
      void *to;
              
      extCmd = virtInfo->sgExtCmd;

      // Do we already have an extended command going with enough space
      if (!((extCmd->sgArr.length + cmd->sgArr.length) > virtInfo->sgMax)) {
         from = &(cmd->sgArr.sg[0]);
         to = &(extCmd->sgArr.sg[extCmd->sgArr.length]);
         // now copy the SG array
         size = cmd->sgArr.length * sizeof(SG_Elem);
         memcpy(to, from, size);
         // update the sg length
         extCmd->sgArr.length += cmd->sgArr.length;
      } else {
         // Not enough space in the existing command, reallocate
         SCSI_Command *oldExtCmd = extCmd;

        size = sizeof(SCSI_Command) 
            + ((2*cmd->sgArr.length) + virtInfo->sgMax - SG_DEFAULT_LENGTH) 
            * sizeof (SG_Elem);
         extCmd = (SCSI_Command *) Mem_Alloc(size);
         if (!extCmd) {
            Warning("Reallocate command, No mem, len=%d",
                    cmd->sgArr.length);
            handle->flags &= ~(SCSI_HANDLE_EXTSG);
            virtInfo->sgExtCmd = (void *)0;
            virtInfo->sgMax = 0;
            Mem_Free(oldExtCmd);
            VSCSI_HandleRelease(handle);
            return VMK_NO_MEMORY;
         }
         virtInfo->sgExtCmd = extCmd;
         virtInfo->sgMax += 2*cmd->sgArr.length;

         // now copy the old SG array  first and free it
         size = sizeof(SG_Array);
         if (oldExtCmd->sgArr.length > SG_DEFAULT_LENGTH) {
            size += (oldExtCmd->sgArr.length - SG_DEFAULT_LENGTH) 
               * sizeof(SG_Elem);
         }
         memcpy(&(extCmd->sgArr), &(oldExtCmd->sgArr), size);
         Mem_Free(oldExtCmd);

         // now copy the additional SG elements
         from = &(cmd->sgArr.sg[0]);
         to = &(extCmd->sgArr.sg[extCmd->sgArr.length]);
         size = cmd->sgArr.length * sizeof(SG_Elem);
         memcpy(to, from, size);
         // update the sg length
         extCmd->sgArr.length += cmd->sgArr.length;
      }
   }
   VSCSI_HandleRelease(handle);
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * VSCSI_ExecuteCommand --
 *
 *      Send a SCSI command to the virtual adapter.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMKERNEL_ENTRY
VSCSI_ExecuteCommand(DECLARE_ARGS(VMK_SCSI_COMMAND))
{
   VMK_ReturnStatus result;
   PROCESS_3_ARGS(VMK_SCSI_COMMAND, VSCSI_HandleID, handleID,
	          SCSI_Command *, cmd, SGPinArrType *, lPtr);

   VSCSIExecuteCommandInt(handleID, cmd, &result,
                          ASYNC_POST_ACTION | ASYNC_ENQUEUE, lPtr);
   return result;
}

/*
 *----------------------------------------------------------------------
 *
 * VSCSIExecuteCommandInt --
 *
 *      Demultiplex virtual SCSI commands. 
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	None. 
 *
 *----------------------------------------------------------------------
 */
static void
VSCSIExecuteCommandInt(VSCSI_HandleID handleID,
                      SCSI_Command *cmd, 
                      VMK_ReturnStatus *result,
                      uint32 flags,
                      SGPinArrType* lPtr)
{
   VSCSI_Handle *handle;
   SCSIVirtInfo *virtInfo;

   handle = VSCSI_HandleFind(handleID);
   if (handle == NULL) {
      Warning("Couldn't find handle 0x%x", handleID);
      *result = VMK_INVALID_HANDLE;
      return;
   }

   virtInfo = handle->info;
   if (handle->flags & SCSI_HANDLE_CLOSING) {
      VSCSI_HandleRelease(handle);
      *result = VMK_INVALID_HANDLE;
      Warning("SCSI command on closing handle 0x%x", handleID);
      return;
   }

   //Command from VM
   ASSERT((flags & ASYNC_POST_ACTION) != 0);
   ASSERT(!(handle->flags & SCSI_HANDLE_READONLY));
   ASSERT(virtInfo->actionIndex != ACTION_INVALID);
   
   // We need to save away the original serial number together with the
   // handleID, since this pair is globally unique (to be used for abort
   // and reset handling). It will also help us to only clean up commands
   // for this world when getting a reset.
   cmd->originHandleID = handle->handleID;
   cmd->originSN = cmd->serialNumber;

   switch (cmd->type) {
   case SCSI_QUEUE_COMMAND:
      VSCSIHandleCommand(handle, cmd, result, flags, lPtr);
      break;
   case SCSI_ABORT_COMMAND:
      VSCSIAbortCommand(handle, cmd, result);
      break;
   case SCSI_RESET_COMMAND:
      VSCSIResetHandle(handle, cmd, flags, result);
      break;
   default:
      Warning("Invalid SCSI cmd type (0x%x) from %s", cmd->type, "VM");    
      ASSERT(FALSE);
   }
   VSCSI_HandleRelease(handle);
}

/*
 *----------------------------------------------------------------------
 *
 * VSCSIHandleCommand --
 *
 *      Handle the SCSI command for the  virtual adapter. 
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	None. 
 *
 *----------------------------------------------------------------------
 */
static void
VSCSIHandleCommand(VSCSI_Handle *handle,
		 SCSI_Command *cmd,
		 VMK_ReturnStatus *result, 
		 uint32 flags,
		 SGPinArrType *lPtr)
{
   Async_Token *token;   
   SCSI_ResultID rid;
   SCSI_Command *extCmd = 0;
   SCSIVirtInfo *virtInfo = handle->info;

   if (handle->flags & SCSI_HANDLE_EXTSG) {
      extCmd = virtInfo->sgExtCmd;
   }

   token = Async_AllocToken(flags);
   ASSERT_NOT_IMPLEMENTED(token != NULL);
   // Copy the ppns passed through lPtr
   if (lPtr && lPtr->sgLen != 0) {
      int len = lPtr->sgLen;
      int size = sizeof(SGPinArrType) + len*sizeof(sgPinType);
      token->sgList = Mem_Alloc(size);
      ASSERT(token->sgList);
      memcpy(token->sgList, lPtr, size);
   } else {
      token->sgList = 0;
   }

   memset(&rid, 0, sizeof(SCSI_ResultID));
   rid.handleID = handle->handleID;
   rid.serialNumber = cmd->serialNumber;
   rid.token = token; 

   token->resID = virtInfo->worldID;
   token->originSN = cmd->serialNumber;
   token->originSN1 = cmd->serialNumber1;

   // Increment the pending commands count
   SP_Lock(&handle->lock);
   handle->pendCom++;
   SP_Unlock(&handle->lock);

   // if there is an extended command switch it now.
   if (extCmd) {
      memcpy(extCmd, cmd, sizeof(SCSI_Command)-sizeof(SG_Array));
      cmd = extCmd;
   }
      
   SCSI_GetXferData(cmd, virtInfo->devClass, virtInfo->blockSize);
   *result = virtInfo->devOps->VSCSI_VirtCommand(virtInfo, cmd, &rid, virtInfo->worldID);

   ASSERT(token->refCount >= 1);					
   if (*result != VMK_OK) {
      Warning("return status 0x%x", *result);
      goto exit;
   }
   Async_ReleaseToken(token);

   // Reset extCmd flags if necessary
   if (extCmd) {
      // the memory will be freed as part of issuing the command
      handle->flags &= ~(SCSI_HANDLE_EXTSG);
      virtInfo->sgExtCmd = 0;
      virtInfo->sgMax = 0;
   }
   return;

exit:
   // Reset extCmd flags if necessary
   if (extCmd) {
      Mem_Free(extCmd);
      handle->flags &= ~(SCSI_HANDLE_EXTSG);
      virtInfo->sgExtCmd = 0;
      virtInfo->sgMax = 0;
   }
   Async_ReleaseToken(token);
   return;
}

/*
 *----------------------------------------------------------------------
 *
 * VSCSIAbortCommand --
 *
 *      Abort a SCSI command for the virtual Disk.
 *      Pass on the abort command down to vscsi backend
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
VSCSIAbortCommand(VSCSI_Handle *handle,
		 SCSI_Command *cmd,
		 VMK_ReturnStatus *result)
{
   SCSIVirtInfo *virtInfo = handle->info;

   LOG(0, "handle 0x%x sno %u", handle->handleID, cmd->serialNumber);
   virtInfo->devOps->VSCSI_VirtAbortCommand(virtInfo, cmd, result);
}

/*
 * ---------------------------------------------------------------------
 *  SCSITCDiff--
 *
 *      Compute the difference between two Timer_AbsCycles quantities.
 *
 * Results: 
 *	None
 *
 * Side effects:
 *      None
 *----------------------------------------------------------------------
 */
static inline Timer_RelCycles
SCSITCDiff(Timer_AbsCycles tsc1, Timer_AbsCycles tsc2) {
   return tsc1 - tsc2;
}

static int resetHandlerWorldsCount; // Protected by the handleArrayLock

/*
 *----------------------------------------------------------------------
 *
 * VSCSIResetHandle --
 *
 *      Schedule a handle reset. It will be processed asynchronously by
 *      one of the reset handler worlds.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
VSCSIResetHandle(VSCSI_Handle *handle,
		 SCSI_Command *cmd,
                 uint32 flags,
		 VMK_ReturnStatus *result)
{
   SCSIVirtInfo *virtInfo = handle->info;

   LOG(0, "handle 0x%x sno %u", handle->handleID, cmd->serialNumber);

   SP_Lock(&vscsiHandleArrayLock);
   if (virtInfo->resetState == SCSI_RESET_NONE) {
      virtInfo->resetState = SCSI_RESET_BUSY;
      SP_Unlock(&vscsiHandleArrayLock);

      // Make sure the handle doesn't go away until all IOs have been drained
      VSCSI_HandleFind(handle->handleID);

      Log("Reset request on handle %d (%d outstanding commands)",
	  handle->handleID, handle->pendCom);

      SP_Lock(&handle->lock);
      handle->pendCom++;
      SP_Unlock(&handle->lock);

      SP_Lock(&vscsiHandleArrayLock);
      virtInfo->resetRetries = 0;
      virtInfo->resetTSC = Timer_GetCycles(); // The first try is due now
      virtInfo->resetState = SCSI_RESET_REQUESTED;
      virtInfo->resetFlags = flags;

      CpuSched_Wakeup((uint32)&resetHandlerWorldsCount);
   } else {
      Warning("Ignoring double reset on handle %d", handle->handleID);
      ASSERT(FALSE);
   }
   SP_Unlock(&vscsiHandleArrayLock);

   *result = VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * VSCSIResetComplete --
 *
 *      Send a handle reset completion notification.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
VSCSIResetComplete(VSCSI_Handle *handle)
{
   Async_Token *token;   
   SCSI_Result *result;
   SCSIVirtInfo *virtInfo = handle->info;

   ASSERT(SP_IsLocked(&vscsiHandleArrayLock));
   ASSERT(virtInfo->resetState == SCSI_RESET_DRAINING ||
	  virtInfo->resetState == SCSI_RESET_REQUESTED);

   Log("Completing reset on handle %d (%d outstanding commands)",
       handle->handleID, handle->pendCom - 1);

   virtInfo->resetState = SCSI_RESET_BUSY;
   SP_Unlock(&vscsiHandleArrayLock);
   
   token = Async_AllocToken(virtInfo->resetFlags);
   ASSERT_NOT_IMPLEMENTED(token != NULL);
   token->flags = virtInfo->resetFlags;

   result = (SCSI_Result *)token->result;
   result->type = SCSI_RESET_COMMAND;
   result->status = SCSI_MAKE_STATUS(SCSI_HOST_OK, SDSTAT_GOOD);

   VSCSIPostCmdCompletion(handle, token);

   Async_ReleaseToken(token);
   VSCSI_HandleRelease(handle);
   
   SP_Lock(&vscsiHandleArrayLock);
   ASSERT(virtInfo->resetState == SCSI_RESET_BUSY);
   virtInfo->resetState = SCSI_RESET_NONE;
}

/*
 *----------------------------------------------------------------------
 *
 * VSCSIResetWorldFunc --
 *
 *      Wake up at regular intervals to perform resets and reset retries.
 *      Die if the reset load becomes too low and the number of reset
 *      handler worlds is above minimum.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	May issue SCSI aborts, lun, device or bus resets.
 *
 *----------------------------------------------------------------------
 */
static void
VSCSIResetWorldFunc(void *data)
{
   int handleIndex;
   int skipCount;
   Timer_AbsCycles lastActionTSC;

   // Don't pre-empt this world since it acquires spinlocks
   CpuSched_DisablePreemption();

   lastActionTSC = Timer_GetCycles();
   SP_Lock(&vscsiHandleArrayLock);

   Log("Starting reset handler world %d/%d",
       MY_RUNNING_WORLD->worldID, resetHandlerWorldsCount);

   for (handleIndex = 0, skipCount = 0; TRUE; handleIndex++) {
      Timer_AbsCycles now = Timer_GetCycles();
      VSCSI_Handle *handle;

      if (++skipCount == VSCSI_MAX_HANDLES) {
	 uint32 expiresMsecs = CONFIG_OPTION(DISK_RESET_WORLD_EXPIRES) * 1000;

	 // All mature resets have been serviced. Destroy this world if it's
	 // been inactive for too long and there are too many reset handler
	 // worlds. Otherwise, just snooze.
	 ASSERT(CONFIG_OPTION(DISK_MIN_RESET_WORLDS) > 0);
	 if (resetHandlerWorldsCount > CONFIG_OPTION(DISK_MIN_RESET_WORLDS) &&
	     expiresMsecs &&
	     Timer_TCToMS(SCSITCDiff(now, lastActionTSC)) > expiresMsecs) {
	    break;
	 }

	 // Wait for the reset monitor to wake us up
	 if (expiresMsecs) {
	    CpuSched_TimedWait((uint32)&resetHandlerWorldsCount,
			       CPUSCHED_WAIT_SCSI, &vscsiHandleArrayLock,
			       expiresMsecs);
	 } else {
	    CpuSched_Wait((uint32)&resetHandlerWorldsCount,
			  CPUSCHED_WAIT_SCSI, &vscsiHandleArrayLock);
	 }

	 SP_Lock(&vscsiHandleArrayLock);

	 skipCount = 0;
      }

      handle = vscsiHandleArray[handleIndex & VSCSI_HANDLE_MASK];

      if (handle == NULL)
	 continue;
      SCSIVirtInfo *virtInfo = handle->info;
      
      if (virtInfo->resetState != SCSI_RESET_NONE) {
	 LOG(2, "Handle %d - state %d", handle->handleID, virtInfo->resetState);
      }

      switch (virtInfo->resetState) {
      case SCSI_RESET_NONE:
      case SCSI_RESET_BUSY:
	 break;
      case SCSI_RESET_DRAINING:
	 // If all IOs have been drained, complete the handle reset.
	 if (handle->pendCom == 1) {
	    VSCSIResetComplete(handle);
	    break;
	 }

	 // Otherwise, check if it's time for a reset retry.
	 if (SCSITCDiff(virtInfo->resetTSC, now) > 0) {
	    break;
	 }

	 // Fall through;
      case SCSI_RESET_REQUESTED: {
	 SCSI_Command cmd;
	 VMK_ReturnStatus result;

	 if (CONFIG_OPTION(DISK_RESET_MAX_RETRIES) &&
	     virtInfo->resetRetries > CONFIG_OPTION(DISK_RESET_MAX_RETRIES)) {
	    // The max number of retries has been exceeded, complete the handle
	    // reset with an error
	    Warning("Max number of reset retries exceeded (%d) on handle %d. "
		    "Completing bus reset with %d outstanding IOs.",
		    virtInfo->resetRetries, handle->handleID, handle->pendCom);
	    VSCSIResetComplete(handle);
	    break;
	 }
	 
	 virtInfo->resetState = SCSI_RESET_BUSY;
	 SP_Unlock(&vscsiHandleArrayLock);
	 
	 memset(&cmd, 0, sizeof(cmd));
	 cmd.type = SCSI_RESET_COMMAND;
	 cmd.originHandleID = handle->handleID;
	 Log("Resetting handle %d [%d/%d]", handle->handleID,
	     virtInfo->resetRetries, CONFIG_OPTION(DISK_RESET_MAX_RETRIES));

         // XXX Prepare the cmd properly.
         virtInfo->devOps->VSCSI_VirtResetTarget(virtInfo, &cmd, &result);
         LOG(0, "handle 0x%x sno %u", handle->handleID, cmd.serialNumber);
	 
	 SP_Lock(&vscsiHandleArrayLock);
	 ASSERT(virtInfo->resetState == SCSI_RESET_BUSY);
	 virtInfo->resetState = SCSI_RESET_DRAINING;
	 virtInfo->resetRetries++;
	 virtInfo->resetTSC =
	    now + Timer_TCToMS(CONFIG_OPTION(DISK_RESET_PERIOD) * 1000);
	 lastActionTSC = Timer_GetCycles();
	 skipCount = -1;

	 if (handle->pendCom == 1) {
	    VSCSIResetComplete(handle);
	    break;
	 }
      }
	 break;
      default:
	 NOT_REACHED();
	 break;
      }
   }

   resetHandlerWorldsCount--;
   Log("Stopping reset handler world %d/%d",
       MY_RUNNING_WORLD->worldID, resetHandlerWorldsCount);

   SP_Unlock(&vscsiHandleArrayLock);

   World_Exit(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * VSCSICreateResetWorld --
 *
 *      Create a new reset world
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
VSCSICreateResetWorld(const char *name, CpuSched_StartFunc startFunction)
{
   World_Handle *world;
   VMK_ReturnStatus status;
   World_InitArgs args;
   Sched_ClientConfig sched;

   Sched_ConfigInit(&sched, SCHED_GROUP_NAME_DRIVERS);
   World_ConfigArgs(&args, name, WORLD_SYSTEM, WORLD_GROUP_DEFAULT, &sched);

   status = World_New(&args, &world);

   if (status == VMK_OK) {
      Sched_Add(world, startFunction, NULL);
   }

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * VSCSIResetWatchdog --
 *
 *      Monitor the progress of resets and reset retries:
 *      - wake up reset handler worlds when resets are due
 *      - spawn new reset handler worlds when reset are grossly overdue
 *      - log messages when resets are overdue or taking too long.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	May spawn new reset handler worlds
 *
 *----------------------------------------------------------------------
 */
static void
VSCSIResetWatchdog(void *data)
{
   int handleIndex;
   Timer_AbsCycles *lastLogTSC;

   // Don't pre-empt this world since it acquires spinlocks
   CpuSched_DisablePreemption();

   lastLogTSC = Mem_Alloc(sizeof(Timer_AbsCycles) * VSCSI_MAX_HANDLES);
   if (lastLogTSC == NULL) {
      Panic("Failed to allocate lastLogTSC[]");
   }
   
   Log("Starting reset watchdog world %d", MY_RUNNING_WORLD->worldID);

   for (handleIndex = 0; handleIndex < VSCSI_MAX_HANDLES; handleIndex++) {
      lastLogTSC[handleIndex] = Timer_GetCycles();
   }

   while(TRUE) {
      int handleIndex;
      int needNewResetHandlerWorld = 0;
      int resetNeedsService = 0;

      SP_Lock(&vscsiHandleArrayLock);
      for (handleIndex = 0; handleIndex < VSCSI_MAX_HANDLES; handleIndex++) {
	 VSCSI_Handle *handle;
	 Timer_AbsCycles now;

	 handle = vscsiHandleArray[handleIndex & VSCSI_HANDLE_MASK];
	 if (handle == NULL)
	    continue;
         SCSIVirtInfo *virtInfo = handle->info;
	 
	 now =  Timer_GetCycles();

	 switch (virtInfo->resetState) {
	 case SCSI_RESET_NONE:
	    break;
	 case SCSI_RESET_REQUESTED:
	 case SCSI_RESET_DRAINING:
	    // Remember to wake up a reset world if some reset needs servicing
	    if (SCSITCDiff(virtInfo->resetTSC, now) > 0) {
	       resetNeedsService++;
	    }

	    // Create a new reset world if a reset is grossly overdue
	    if (Timer_TCToMS(SCSITCDiff(now, virtInfo->resetTSC)) >
		CONFIG_OPTION(DISK_MAX_RESET_LATENCY)) {
	       needNewResetHandlerWorld++;

	       if (Timer_TCToMS(SCSITCDiff(now, lastLogTSC[handleIndex])) >
		   CONFIG_OPTION(DISK_OVERDUE_RESET_LOG_PERIOD) * 1000) {
		  Warning("Retry %d on handle %d overdue by %d seconds",
			  virtInfo->resetRetries, handle->handleID,
			  (int)(Timer_TCToMS(SCSITCDiff(now, virtInfo->resetTSC) / 1000)));
		  lastLogTSC[handleIndex] = Timer_GetCycles();
	       }
	    }
	    break;
	 case SCSI_RESET_BUSY:
	    if (Timer_TCToMS(SCSITCDiff(now, virtInfo->resetTSC)) >
		CONFIG_OPTION(DISK_MAX_RESET_LATENCY) &&
		Timer_TCToMS(SCSITCDiff(now, lastLogTSC[handleIndex])) >
		CONFIG_OPTION(DISK_OVERDUE_RESET_LOG_PERIOD) * 1000) {
	       Warning("Retry %d on handle %d still in progress after %d seconds",
		       virtInfo->resetRetries, handle->handleID,
		       (int)(Timer_TCToMS(SCSITCDiff(now, virtInfo->resetTSC) / 1000)));
	       lastLogTSC[handleIndex] = Timer_GetCycles();
	    }
	    break;
	 default:
	    NOT_REACHED();
	 }
      }

      // Spawn a new reset world if some resets are overdue and the max number
      // of reset worlds has not been reached yet.
      ASSERT(CONFIG_OPTION(DISK_MAX_RESET_WORLDS) > 0);
      if (needNewResetHandlerWorld &&
	  resetHandlerWorldsCount < CONFIG_OPTION(DISK_MAX_RESET_WORLDS)) {
	 VMK_ReturnStatus status;

	 resetHandlerWorldsCount++;

	 SP_Unlock(&vscsiHandleArrayLock);
	 status = VSCSICreateResetWorld("ResetHandler", VSCSIResetWorldFunc);
	 SP_Lock(&vscsiHandleArrayLock);
	 
	 if (status != VMK_OK) {
	    Warning("Failed to create new reset handler world."
		    " %d resets overdue.", needNewResetHandlerWorld);
	    resetHandlerWorldsCount--;
	 }
      }

      SP_Unlock(&vscsiHandleArrayLock);
	    
      if (resetNeedsService) {
	 CpuSched_Wakeup((uint32)&resetHandlerWorldsCount);
      }

      CpuSched_Sleep(CONFIG_OPTION(DISK_RESET_LATENCY));
   }

   NOT_REACHED();
}

/*
 *----------------------------------------------------------------------
 *
 * SCSIResetInit --
 *
 *      Create the reset watchdog and a reset handler world
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
void
SCSI_ResetInit(void)
{
   ASSERT(resetHandlerWorldsCount == 0);
   resetHandlerWorldsCount = 1;

   if (VSCSICreateResetWorld("reset-handler", VSCSIResetWorldFunc) != VMK_OK) {
      Panic("Could not create reset handler world");
   }
   
   if (VSCSICreateResetWorld("reset-watchdog", VSCSIResetWatchdog) != VMK_OK) {
      Panic("Could not create reset watchdog world");
   }
}

/*
 *----------------------------------------------------------------------
 *
 * VSCSI_CmdComplete --
 *
 *      Return completed command information.
 *
 * Results: 
 *	TRUE if there is a completed command to return information about.
 *
 * Side effects:
 *	*outResult contains the information about the completed command.
 *	*more is TRUE if there are more completed commands to be
 *	processed.
 *
 *----------------------------------------------------------------------
 */
VMKERNEL_ENTRY
VSCSI_CmdComplete(DECLARE_ARGS(VMK_SCSI_CMD_COMPLETE))
{
   PROCESS_4_ARGS(VMK_SCSI_CMD_COMPLETE, VSCSI_HandleID, handleID,
                  SCSI_Result *, outResult, SGPinArrType *, lPtr,
                  Bool *, more);

   return VSCSICmdCompleteInt(handleID, outResult, lPtr, more);
}

/*
 *----------------------------------------------------------------------
 *
 * VSCSICmdCompleteInt --
 *
 *      Return a completed command to the guest OS
 *
 * Results: 
 *	VMK_OK if there was a completed command to return
 *
 * Side effects:
 *	Fill in *outResult with the SCSI_Result of the completed IO
 *	Set *more to TRUE if there are more completed commands to be
 *	processed.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
VSCSICmdCompleteInt(VSCSI_HandleID handleID, SCSI_Result *outResult,
                    SGPinArrType *lPtr, Bool *more)
{
   Bool found = FALSE;
   VSCSI_Handle *handle;
   SCSIVirtInfo *virtInfo;

   handle = VSCSI_HandleFind(handleID);
   if (handle == NULL) {
      *more = FALSE;
      return VMK_NOT_FOUND;
   }

   virtInfo = handle->info;
   ASSERT(handle->handleID == handleID);

   SP_Lock(&handle->lock);

   if (virtInfo->resultListHead != NULL) {
      SCSI_Result *result;   
      Async_Token *token = virtInfo->resultListHead;

      virtInfo->resultListHead = token->nextForCallee;
      if (virtInfo->resultListHead == NULL) {
	 virtInfo->resultListTail = NULL;
      }

      result = (SCSI_Result *)token->result;
      *outResult = *result;
      ASSERT(result->serialNumber == token->originSN);
      outResult->serialNumber = token->originSN;
      outResult->serialNumber1 = token->originSN1;
      found = TRUE;

      ASSERT(result->type == SCSI_QUEUE_COMMAND ||
	     result->type == SCSI_RESET_COMMAND);

      // Save sense data in handle in case REQUEST_SENSE is called later.
      memcpy(&(virtInfo->sense), result->senseBuffer, sizeof(virtInfo->sense));

      // pass back the saved ppns to the monitor
      if (token->sgList) {
         if (lPtr) {
            int len = ((SGPinArrType *)(token->sgList))->sgLen;
            memcpy(lPtr, token->sgList, 
                   (sizeof(SGPinArrType) + len*sizeof(sgPinType)));
         }
         Mem_Free(token->sgList);
      } else {
         if (lPtr) {
            lPtr->sgLen = 0;
         }
      }
      Async_ReleaseToken(token);
   }

   *more = virtInfo->resultListHead != NULL;
   SP_Unlock(&handle->lock);
   VSCSI_HandleRelease(handle);

   return (found)? VMK_OK : VMK_NOT_FOUND;
}

/*
 *----------------------------------------------------------------------
 *
 * VSCSI_WaitForCIF --
 *
 *      Wait for all commands issued on this handle to come back.
 *
 * Parameters:
 *      timeOut is in seconds
 *
 * Results: 
 *	VMK_NOT_FOUND if the handle could not be looked up,
 *      VMK_OK otherwise
 *
 * Side effects:
 *	We sleep with the VMs adapter (device) lock
 *
 *----------------------------------------------------------------------
 */
VMKERNEL_ENTRY
VSCSI_WaitForCIF(DECLARE_ARGS(VMK_SCSI_WAIT_FOR_CIF))
{
   VSCSI_Handle *handle;

   PROCESS_1_ARG(VMK_SCSI_WAIT_FOR_CIF, VSCSI_HandleID, handleID);

   handle = VSCSI_HandleFind(handleID);
   if (handle == NULL) {
      Warning("handleID %d not found", handleID);
      return VMK_NOT_FOUND;
   }

   LOG(0, "pendCom = %d", handle->pendCom);

   while (TRUE) {
      if (handle->pendCom == 0) {
         break;
      }
      /* sleep for a little while */
      CpuSched_Sleep(100);
   }

   VSCSI_HandleRelease(handle);
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * VSCSI_CreateDevice --
 *
 *	Create a virtual SCSI handle that actually accesses the specified
 *	storage resource referenced by "opaqueID".
 *	If fid is -1, then we are doing a "lazy" open, because the disk
 *	containing the file is reserved by another host.  In this case, we
 *	mainly just save away the info so we can open the file and fill in
 *	the virtual adapter later. For raw disk mappings (RDMs), check
 *	'doPassthru' to determine whether the RDM should be opened
 *	with raw LUN SCSI semantics, or with VMFS virtual SCSI disk
 *	semantics.
 *
 * Results: 
 *	The new virtual SCSI handle.
 *
 * Side effects:
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
VSCSI_CreateDevice(World_ID worldID, 
                   VSCSI_DevDescriptor *desc,
                   VSCSI_HandleID *outHandleID)
{
   SCSIVirtInfo 	*virtInfo;
   VSCSI_Handle 	*handle;
   VMK_ReturnStatus 	status = VMK_OK;   
   VSCSI_CapacityInfo	capInfo;

   virtInfo = (SCSIVirtInfo *)Mem_Alloc(sizeof(SCSIVirtInfo));
   if (virtInfo == NULL) {
      return VMK_NO_RESOURCES;
   }
   memset(virtInfo, 0, sizeof(SCSIVirtInfo));

   LOG(1, "internal handle=%Ld", *(uint64 *)&desc->u);

   Semaphore_RWInit("virtLock", &virtInfo->rwlock);
   memcpy(&virtInfo->devDesc, desc, sizeof(VSCSI_DevDescriptor));

   handle = VSCSIAllocHandle(virtInfo, worldID);
   if (handle == NULL) {
      Semaphore_RWCleanup(&virtInfo->rwlock);
      Mem_Free(virtInfo);
      LOG(1, "VSCSIAllocHandle failed: %#x", status);
      return VMK_NO_RESOURCES;
   }

   /*
    * Open the underlying device which can be File,COWFile, RawDisk or RDMP
    * And Get the devOps Vector
    */
   status = VSCSIVirtOpen(desc, worldID, virtInfo);
   if (status != VMK_OK) {
      Semaphore_RWCleanup(&virtInfo->rwlock);
      Mem_Free(virtInfo);
      return status;
   }

   status = virtInfo->devOps->VSCSI_GetCapacityInfo(desc, &capInfo);
   if (status != VMK_OK) {
      LOG(1, "GetCapacity failed: %#x", status);
      Semaphore_RWCleanup(&virtInfo->rwlock);
      Mem_Free(virtInfo);
      return status;
   }

   virtInfo->handle = handle;
   virtInfo->worldID = worldID;
   virtInfo->actionIndex = desc->vmkChannel;
   virtInfo->blockSize = capInfo.diskBlockSize;
   virtInfo->numBlocks = (capInfo.length + DISK_SECTOR_SIZE - 1) / DISK_SECTOR_SIZE;
   handle->info = virtInfo;

   *outHandleID = handle->handleID;

   LOG(1, "Returning vscsi handle %d", *outHandleID);
   return VMK_OK;
}

/*
 *
 * VSCSI_VirtAsyncDone :
 *
 * Callback function invoked when async file operations started in VSCSI backends.
 * 
 * Results : Command Completion.
 *
 * SideEffects : None.
 * 
 */
void
VSCSI_VirtAsyncDone(Async_Token *token)
{
   int i;
   SCSI_ResultID rid;
   SCSI_Result *fsResult = NULL;
   SCSIVirtInfo *info = NULL;
   SCSIVirtAsyncInfo *asyncInfo = NULL;
   for (i = 0; 
	i + sizeof(SCSIVirtAsyncInfo) <= ASYNC_MAX_PRIVATE;
	i += SCSI_ASYNC_INCR) {
      asyncInfo = (SCSIVirtAsyncInfo *)&token->callerPrivate[i];
      if (asyncInfo->magic == SCSI_VIRT_MAGIC) {
	 break;
      } else {
	 asyncInfo = NULL;
      }
   }
   LOG(1, "VSCSI_VirtAsyncDone");

   ASSERT(asyncInfo != NULL);

   info = asyncInfo->info;
   fsResult = (SCSI_Result *)token->result;
   VSCSI_Handle *handle = info->handle;

   token->callback = asyncInfo->savedCallback;
   token->flags = asyncInfo->savedFlags;
   token->callerPrivateUsed -= sizeof(SCSIVirtAsyncInfo);

   memset(&rid, 0, sizeof(SCSI_ResultID));
   rid.handleID = handle->handleID;
   rid.token = token; 
   rid.serialNumber = asyncInfo->serialNumber;

   Semaphore_EndRead(&info->rwlock);
   VSCSI_DoCommandComplete(&rid, fsResult->status, fsResult->senseBuffer, 0, 0);
}

/*
 *----------------------------------------------------------------------
 *
 * VSCSI_DoCommandComplete --
 *
 *      Handle a completed command from the driver.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	The result queue for the appropriate handle is updated.
 *
 * Note: Caller should hold no adapter locks. Can be called from a bottom-half. 
 *
 *----------------------------------------------------------------------
 */
void
VSCSI_DoCommandComplete(SCSI_ResultID *rid, 
                       SCSI_Status status, 
                       uint8 *senseBuffer,
                       uint32 bytesXferred,
		       uint32 flags) 
{
   VSCSI_Handle *handle;
   SCSIVirtInfo *virtInfo;
   Async_Token *token = rid->token;   
   Bool delayCompletion = FALSE;
   Bool cb = FALSE;
   SCSI_Result *result;
 
   LOG(1, "VSCSI_DoCommandComplete");

   ASSERT(rid->handleID != -1);
   handle = VSCSI_HandleFind(rid->handleID);
   ASSERT(handle != NULL);
   virtInfo = handle->info;

   DEBUG_ONLY(
   // Log the timings of aborted IOs on non release builds.
   if (SCSI_HOST_STATUS(status) == SCSI_HOST_ABORT ||
       SCSI_HOST_STATUS(status) == SCSI_HOST_RESET) {
      int64 started = (RDTSC() - token->startTSC) / cpuMhzEstimate;
      int64 issued = token->issueTSC ?
	 (RDTSC() - token->issueTSC) / cpuMhzEstimate : -1000;
      
      LOG(0,"Aborted H-%u:SN-%u [%u:%u] %ld.%03ums, %ld.%03ums",
	  token->originHandleID, token->originSN,
	  rid->handleID, rid->serialNumber,
	  (long)(started / 1000), (int)(started % 1000),
	  (long)(issued / 1000), (int)(issued % 1000));
   }
   )

   if (handle->handleID != rid->handleID) {
      Warning("Handle IDs don't match %d != %d",
	      rid->handleID, handle->handleID);
      if (token->sgList) {
         Mem_Free(token->sgList);
      }
      goto unlockEnd;
   }

   /* The handle's world id should be the same as the token's resID for
    * raw disk access, for a virtual SCSI adapter, or for synchronous
    * reads/writes.  
    *
    * The handle's world will be the console world for
    * handles used in opening/creating a VMFS
    */ 
   ASSERT(virtInfo->worldID == token->resID);

   result = (SCSI_Result *)token->result;

   if (SCSI_DEVICE_STATUS(status) == SDSTAT_RESERVATION_CONFLICT) {
      ASSERT(virtInfo->devDesc.type == VSCSI_FS);
      FS_FileHandleID fid = virtInfo->devDesc.u.fid;
      if (fid != FS_INVALID_FILE_HANDLE && !FSS_IsMultiWriter(fid)) {
	 /* Don't let the guest see a SCSI reservation conflict (which is
	  * likely due to VMFS locking) when accessing a VMFS file unless
	  * we are doing clustering (multi-writer access to VMFS file).
	  */
	 LOG(0, "Converting reservation conflict to busy");
	 status = SDSTAT_BUSY;
      }
   }

   result->serialNumber = rid->serialNumber;
   result->status = status;
   result->bytesXferred = bytesXferred;
   result->type = SCSI_QUEUE_COMMAND;

   // The sense data may already be in the token's SCSI_Result, and we are
   // just passing in a ptr to that sense buffer.
   if (result->senseBuffer != senseBuffer) {
      memcpy(result->senseBuffer, senseBuffer, SCSI_SENSE_BUFFER_LENGTH);
   }

   /*
    * We check the time on VCPU0, which may not be the current VCPU
    * because we send the interrupt only to VCPU0.  Also, since
    * delaySCSICmds is used during bootup, we're likely to be on VCPU0
    * anyway.
    */
   if (token->flags & ASYNC_POST_ACTION) {
      World_Handle *world = World_Find(virtInfo->worldID);
      ASSERT(world != NULL);
      if (world != NULL) {
         if (World_VMMGroup(world)->delaySCSICmdsUsec &&
             ((CpuSched_VcpuUsageUsec(world) - token->startVmTime) <
              World_VMMGroup(world)->delaySCSICmdsUsec)) {
            VSCSIDelayCompletion(rid,
                                world, SCSI_DELAY_COMPLETION_VM_TIME,
                                World_VMMGroup(world)->delaySCSICmdsUsec + 
                                token->startVmTime);
            delayCompletion = TRUE;
         } else if (CONFIG_OPTION(DISK_DELAY_ON_BUSY) &&
	            (SCSI_DEVICE_STATUS(result->status) == SDSTAT_BUSY ||
	 	     SCSI_HOST_STATUS(result->status) == SCSI_HOST_BUS_BUSY ||
	  	     SCSI_HOST_STATUS(result->status) == SCSI_HOST_NO_CONNECT)) {
            /*
             * Some guest OSes (e.g. Linux, Windows 2000) retry immediately on status
             * BUSY. To avoid storms of retried IOs, let's delay the completion a bit
             * for all statuses that are returned as BUSY to the guest OS.
             */
            VSCSIDelayCompletion(rid, world,
			        SCSI_DELAY_COMPLETION_ESX_TIME, Timer_GetCycles());
            delayCompletion = TRUE;
         }
         World_Release(world);
      }
   }

   if (!delayCompletion) { 
      /*
       * VSCSIPostCmdCompletion calls IODone.
       */
#ifdef DELAY_TEST
      if (!(rid->cmd->flags & SCSI_CMD_TIMEDOUT))
#endif
         VSCSIPostCmdCompletion(handle, token);
   }


   /* 
    * Callback on cmd completion is requested for split commands and FS reads
    * from a virtual disk.
    */
   if (token->flags & ASYNC_CALLBACK) {

      ASSERT(!(token->flags & ASYNC_ENQUEUE));
      ASSERT(token->callback != NULL);
      ASSERT(delayCompletion == FALSE);

      Async_RefToken(token);
      cb = TRUE;
   }

unlockEnd:
   if (handle) { 
      VSCSI_HandleRelease(handle);
   }

   // Tokens which wanted a callback.
   if (cb) {
      token->callback(token);
      Async_ReleaseToken(token);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * VSCSIPostCmdCompletion --
 *
 *      Enqueue the completed command on a result queue and post the
 *      necessary completion notices.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
VSCSIPostCmdCompletion(VSCSI_Handle *handle, Async_Token *token)
{
   SCSIVirtInfo *virtInfo = handle->info;

   if (token->flags & ASYNC_ENQUEUE) {
      ASSERT(!(token->flags & ASYNC_CALLBACK));
      Async_RefToken(token);	 
      SP_Lock(&handle->lock);
      if (virtInfo->resultListHead == NULL) {
	 virtInfo->resultListHead = token;
	 virtInfo->resultListTail = token;
      } else {
	 virtInfo->resultListTail->nextForCallee = token;
	 virtInfo->resultListTail = token;
      }
      token->nextForCallee = NULL;
      handle->pendCom--;
      SP_Unlock(&handle->lock);
   }      

   if (token->flags & ASYNC_POST_ACTION) {
      World_Handle *world = World_Find(virtInfo->worldID);
      ASSERT(!(token->flags & ASYNC_HOST_INTERRUPT));
      ASSERT(virtInfo->actionIndex != ACTION_INVALID);
      ASSERT(world != NULL && World_IsVMMWorld(world));
      if (world != NULL) {
         Atomic_Or(&World_VMMGroup(world)->scsiCompletionVector[handle->virtualAdapterID],
                   1 << handle->virtualTargetID);
         Action_Post(world, virtInfo->actionIndex);
         World_Release(world);
      }
   }

   Async_IODone(token);
}

/*
 *----------------------------------------------------------------------
 *
 * VSCSIDelayCheckQueue
 *
 *      Check the delayed completion queue.
 *
 *      If there are SCSI_DELAY_COMPLETION_VM_TIME commands that have been
 *      delayed long enough, or SCSI_DELAY_COMPLETION_ESX_TIME commands
 *      (regardless of the time they've been delayed), post the notification.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	scsi delay queue timer may get removed.
 *
 *----------------------------------------------------------------------
 */
static void
VSCSIDelayCheckQueue(void *dummy, UNUSED_PARAM(Timer_AbsCycles timestamp))
{
   VSCSIDelayQueue *sdq, *nextSDQ, *prevSDQ;
   VSCSIDelayQueue *localQueue = NULL, *localQueueTail = NULL;
   VSCSI_Handle *handle;
   World_Handle *world;

   SP_Lock(&vscsiDelayLock);

   prevSDQ = NULL;
   for (sdq = delayQueueFirst; sdq != NULL; sdq = nextSDQ) {
      Bool doneSDQ = FALSE;
      nextSDQ = sdq->next;
      world = World_Find(sdq->worldID);

      if (world == NULL) {
         doneSDQ = TRUE;
      } else if (sdq->timeType == SCSI_DELAY_COMPLETION_VM_TIME &&
		 CpuSched_VcpuUsageUsec(world) > sdq->time) {
         doneSDQ = TRUE;
         /* we can't call postcmdcompletion here because we're holding the
          * vscsiDelayLock and post needs the adapterlock, and
          * doCommandComplete holds and calls VSCSIDelayCompletion, which
          * grabs the delay lock.
          * So, we just put this sdq on a separate queue (localQueue) and 
          * handle it at the end of this function.
          */
      } else if (sdq->timeType == SCSI_DELAY_COMPLETION_ESX_TIME) {
	 doneSDQ = TRUE;
      }

      if (world != NULL) {
         World_Release(world);
      }

      if (doneSDQ) {
         // remove from delay queue
         if (prevSDQ) {
            ASSERT(prevSDQ->next == sdq);
            prevSDQ->next = sdq->next;
         } else {
            ASSERT(delayQueueFirst == sdq);
            delayQueueFirst = sdq->next;
         }
         if (delayQueueLast == sdq) {
            delayQueueLast = prevSDQ;
         }

         // put on local queue
         sdq->next = NULL;
         if (localQueue == NULL) {
            localQueue = localQueueTail = sdq;
         } else {
            localQueueTail->next = sdq;
            localQueueTail = sdq;
         }
      } else {
         prevSDQ = sdq;
      }
   }
   if (delayQueueFirst == NULL) {
      ASSERT(delayQueueLast == NULL);
      Timer_Remove(delayQueueTimer);
      delayQueueCPU = MAX_PCPUS;
   }
   SP_Unlock(&vscsiDelayLock);

   // process the local queue
   for (sdq = localQueue; sdq != NULL; sdq = nextSDQ) {
      nextSDQ = sdq->next;
      handle = VSCSI_HandleFind(sdq->rid.handleID);
      if (handle != NULL) {
         Async_Token *token = sdq->rid.token;
         VSCSIPostCmdCompletion(handle, token);
         Async_ReleaseToken(token); // undo ref from VSCSIDelayCompletion
         VSCSI_HandleRelease(handle);
      }
      Mem_Free(sdq);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * SCSIDelayCompletion --
 *
 *      Delay the completion notification for this command until
 *      the VM reaches the given time (SCSI_DELAY_COMPLETION_VM_TIME)
 *      or until the next delay timer tick (SCSI_DELAY_COMPLETION_ESX_TIME).
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	Queue the completion notification and set a timer
 *
 *----------------------------------------------------------------------
 */
static void
VSCSIDelayCompletion(SCSI_ResultID *rid, World_Handle *world,
		    VSCSIDelayTimeType timeType, uint64 time)
{
   Async_Token *token = rid->token;
   VSCSIDelayQueue *sdq = Mem_Alloc(sizeof(VSCSIDelayQueue));

   ASSERT(token->flags & ASYNC_ENQUEUE);
   ASSERT(token->flags & ASYNC_POST_ACTION);

   Async_RefToken(token);
   memcpy(&sdq->rid, rid, sizeof(SCSI_ResultID));
   sdq->next = NULL;
   sdq->time = time;
   sdq->timeType = timeType;
   sdq->worldID = world->worldID;
         
   SP_Lock(&vscsiDelayLock);
   if (delayQueueCPU == MAX_PCPUS) {
      delayQueueCPU = MY_PCPU;
   }
   if (delayQueueLast != NULL) {
      delayQueueLast->next = sdq;
   } else {
      int32 delay = World_VMMGroup(world)->delaySCSICmdsUsec ?
	 SCSI_CMD_DELAY_PERIOD_MS : CONFIG_OPTION(DISK_DELAY_ON_BUSY);

      delayQueueFirst = sdq;
      delayQueueTimer = Timer_Add(delayQueueCPU, VSCSIDelayCheckQueue,
                                  delay, TIMER_PERIODIC, NULL);
   }
   delayQueueLast = sdq;
   SP_Unlock(&vscsiDelayLock);
}

/*
 * Emulate a SCSI command on the virtual SCSI device specified by virtInfo,
 * if it is not a read or a write.  Return *done = TRUE with scsiStatus and
 * sense filled in if the command was handled by this function (i.e. was not
 * a read or write).
 *
 * NOTE:
 * 	SCSI cmd error checking should not be done in this routine. It should
 * 	be performed in the SCSI_InitialErrorCheckOfCommand() routine.
 */
void
VSCSI_GenericCommand(SCSIVirtInfo *virtInfo, SCSI_Command *cmd,
                     SCSI_Status *scsiStatus, SCSISenseData *sense, Bool *done)
{
   uint32 hostStatus = SCSI_HOST_OK;
   uint32 deviceStatus = SDSTAT_GOOD;
   VSCSI_CapacityInfo	capInfo;
   VMK_ReturnStatus status;

   *done = TRUE;

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
	 uint64 lastSector;
	 DEBUG_ONLY(SCSIReadCapacityCmd* cdb = (SCSIReadCapacityCmd*) cmd->cdb);
	 DEBUG_ONLY(uint32 length = SG_TotalLength(&cmd->sgArr));

	 ASSERT(!(cdb->rel || cdb->pmi || cdb->lbn || length < sizeof(SCSIReadCapacityResponse)));

	 SCSIReadCapacityResponse cp;
         status = virtInfo->devOps->VSCSI_GetCapacityInfo(&virtInfo->devDesc, &capInfo);
         if (status != VMK_OK) {
            Warning("%s : Could not get capacity for virtual device", "READ_CAPACITY");
            SCSI_IllegalRequest(sense, TRUE, 1);
            deviceStatus = SDSTAT_CHECK;
         }
	 cp.blocksize = ByteSwapLong(capInfo.diskBlockSize);
	 lastSector = (capInfo.length + DISK_SECTOR_SIZE - 1)/DISK_SECTOR_SIZE; 
	 cp.lbn = ByteSwapLong(MIN(lastSector,SCSI_READ_CAPACITY_MAX_LBN));
	 if (!Util_CopySGData(&cp, &cmd->sgArr, UTIL_COPY_TO_SG, 0, 0,
			        sizeof(SCSIReadCapacityResponse))) {
	    SCSI_IllegalRequest(sense, TRUE, 1);
	    deviceStatus = SDSTAT_CHECK;
	 }
	 break;
      }
      case SCSI_CMD_READ_CAPACITY16: {
	 uint64 lastSector;
	 SCSIReadCapacity16Response cp;
	 DEBUG_ONLY(SCSIReadCapacity16Cmd* cdb = (SCSIReadCapacity16Cmd*) cmd->cdb);	 
	 DEBUG_ONLY(uint32 length = SG_TotalLength(&cmd->sgArr));

	 ASSERT(!(cdb->action != 0x10 ||
		  cdb->rel ||
		  cdb->pmi ||
		  length < sizeof(SCSIReadCapacity16Response))); 
         status = virtInfo->devOps->VSCSI_GetCapacityInfo(&virtInfo->devDesc, &capInfo);
         if (status != VMK_OK) {
            Warning("%s : Could not get capacity for virtual device", "READ_CAPACITY16");
            SCSI_IllegalRequest(sense, TRUE, 1);
            deviceStatus = SDSTAT_CHECK;
         }
	 cp.blocksize = ByteSwapLong(capInfo.diskBlockSize);
	 lastSector = (capInfo.length + DISK_SECTOR_SIZE - 1)/DISK_SECTOR_SIZE; 

	 if (!Util_CopySGData(&cp, &cmd->sgArr, UTIL_COPY_TO_SG, 0, 0,
			        sizeof(SCSIReadCapacity16Response))) {
	    SCSI_IllegalRequest(sense, TRUE, 1);
	    deviceStatus = SDSTAT_CHECK;
	 }
	 break;
      }
      case SCSI_CMD_READ10:
      case SCSI_CMD_WRITE10: {
            SCSIReadWrite10Cmd *rwCmd = (SCSIReadWrite10Cmd *)cmd->cdb;
	    /* Make blockOffset be uint64 so there won't be any overflow
	     * if numBlocks is large and blockOffset is close to 4G. */
            uint64 blockOffset = ByteSwapLong(rwCmd->lbn);
            uint32 numBlocks = ByteSwapShort(rwCmd->length);
            uint32 partEndSector = virtInfo->numBlocks;  

	    /* Make sure access does go past end of partition. */
            if (blockOffset + numBlocks > partEndSector) {
               Warning("%s10 past end of virtual device ",
                       cmd->cdb[0]==SCSI_CMD_READ10 ? "READ" : "WRITE");
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
            uint8* rw = (uint8*) cmd->cdb;
            uint32 blockOffset = (((rw[1]&0x1f)<<16)|(rw[2]<<8)|rw[3]);
            uint32 numBlocks = ((rw[4] == 0 ? 256 : rw[4]));
            /* This is the number of blocks we report as a reply to READ CAPACITY */
            uint32 partEndSector = virtInfo->numBlocks;  
            
            /* only allow access to sectors 0 through partEndSector-1 */
            if (blockOffset + numBlocks > partEndSector) {
               Warning("%s6 past end of virtual device ",
                       cmd->cdb[0]==SCSI_CMD_READ6 ? "READ" : "WRITE");
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
            SCSIReadWrite16Cmd *rwCmd = (SCSIReadWrite16Cmd *)cmd->cdb;
            uint64 blockOffset = ByteSwap64(rwCmd->lbn);
            uint32 numBlocks = ByteSwapLong(rwCmd->length);
            uint32 partEndSector = virtInfo->numBlocks;  

	    if (blockOffset + numBlocks > partEndSector) {
	       /* Make sure access does not go past end of partition. */
               Warning("%s16 past end of virtual device ",
                       cmd->cdb[0]==SCSI_CMD_READ16 ? "READ" : "WRITE");
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
      default:
         /*
	  * Invalid operations for virtual devices
          * should be caught in SCSI_InitialErrorCheckOfCommand().
	  */
         Log("Invalid Opcode (0x%x)", cmd->cdb[0]);
         NOT_IMPLEMENTED();
   }
   *scsiStatus = SCSI_MAKE_STATUS(hostStatus, deviceStatus);
}

/*
 * VSCSI switch initialization functions.
 */
void 
VSCSI_Init(void)
{
   SP_InitLock("vscsihandleArrayLock", &vscsiHandleArrayLock, SP_RANK_HANDLEARRAY);
   SP_InitLock("vscsiDelayLock", &vscsiDelayLock, SP_RANK_SCSIDELAY);

   VSCSI_FSInit();
   VSCSI_COWInit(); 
   VSCSI_RawDiskInit();
   VSCSI_RDMPInit();
}

VMK_ReturnStatus
VSCSI_RegisterDevice(VSCSI_DevType devType, VSCSI_Ops *devOps)
{
   VSCSIRegisteredDevice *device;

   ASSERT((devType) < VSCSI_MAX_DEVTYPE);
   /* Force the underlying layer to implement all device functions. It
    * doesn't matter if they are no-ops, but the handlers should be present.
    */
   ASSERT(devOps->VSCSI_VirtOpen && devOps->VSCSI_VirtCommand &&
          devOps->VSCSI_VirtClose && 
          devOps->VSCSI_VirtResetTarget && devOps->VSCSI_VirtAbortCommand);

   device = (VSCSIRegisteredDevice *) Mem_Alloc(sizeof(*device));
   if (device == NULL) {
      return VMK_NO_MEMORY;
   }
   device->devType = devType;
   device->devOps = devOps;
   device->next = vscsiDeviceList;
   vscsiDeviceList = device;

   Log("%d", devType);
   return VMK_OK;
}

static VMK_ReturnStatus
VSCSIVirtOpen(VSCSI_DevDescriptor *desc, World_ID worldID, SCSIVirtInfo *info)
{
   VSCSIRegisteredDevice *device = vscsiDeviceList;

   while (device != NULL) {
      VMK_ReturnStatus status;

      if (device->devType == desc->type) {
         status = device->devOps->VSCSI_VirtOpen(desc, worldID, info);
         if (status == VMK_OK) {
            info->devOps = device->devOps;
            return status;
         } else {
            LOG(1, "devOps handler failed:%#x", status);
	    return status;
         }
      } else
         device = device->next;
   }
   LOG(1, "failed");
   return VMK_BAD_PARAM;
}

/*
 *----------------------------------------------------------------------
 *
 * VSCSI_DestroyDevice
 *
 *      Destroys a VSCSI device
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
VSCSI_DestroyDevice(World_ID worldID, VSCSI_HandleID handleID)
{
   return VSCSICloseDevice(worldID, handleID);
}

/*
 * Allocate a VSCSI handle for the specified (virtInfo, worldID).
 */
static VSCSI_Handle *
VSCSIAllocHandle(SCSIVirtInfo *virtInfo, World_ID worldID)
{
   VSCSI_Handle *handle;
   int index;

   ASSERT(virtInfo);

   SP_Lock(&vscsiHandleArrayLock);

   for (index = nextHandle; index < VSCSI_MAX_HANDLES; index++) {
      if (vscsiHandleArray[index] == NULL) {
	 break;
      }
   }
   if (index == VSCSI_MAX_HANDLES) {
      vscsiHandleGeneration++;
      for (index = 0; index < nextHandle; index++) {
	 if (vscsiHandleArray[index] == NULL) {
	    break;
	 }
      }

      if (index == nextHandle) {
	 Warning("Out of vscsi handles");
	 nextHandle = 0;
	 handle = NULL;
	 goto exit;
      }
   }

   nextHandle = index + 1;
   if (nextHandle == VSCSI_MAX_HANDLES) {
      nextHandle = 0;
      vscsiHandleGeneration++;
   }

   handle = (VSCSI_Handle *)Mem_Alloc(sizeof(VSCSI_Handle));
   if (handle == NULL) {
      goto exit;
   }
   memset(handle, 0, sizeof(*handle));
   virtInfo->worldID = worldID;
   handle->info = virtInfo;
   handle->handleID = vscsiHandleGeneration * VSCSI_MAX_HANDLES + index;
   handle->refCount = 1;
   SP_InitLock("vscsiHandle", &handle->lock, SP_RANK_HANDLE);

   vscsiHandleArray[index] = handle;

exit:
   SP_Unlock(&vscsiHandleArrayLock);
   return handle;
}

/*
 * Given a VSCSI_HandleID, return the corresponding VSCSI_Handle (after
 * increasing its refcount).  Must eventually be followed by a call to
 * VSCSI_HandleRelease().
 */
VSCSI_Handle *
VSCSI_HandleFind(VSCSI_HandleID handleID)
{
   VSCSI_Handle *handle;

   SP_Lock(&vscsiHandleArrayLock);
   handle = vscsiHandleArray[handleID & VSCSI_HANDLE_MASK];
   if (handle != NULL) {
      if (handle->handleID != handleID) {
         handle = NULL;
      } else {
         ASSERT(handle->refCount >= 1);
         handle->refCount++;
      }
   }
   SP_Unlock(&vscsiHandleArrayLock);
   return handle;
}

void
VSCSI_HandleRelease(VSCSI_Handle *handle)
{
   SCSIVirtInfo *virtInfo = handle->info;
   Async_Token *token, *next;

   ASSERT(handle != NULL);

   SP_Lock(&vscsiHandleArrayLock);

   handle->refCount--;
   ASSERT(handle->refCount >= 0);

   if (handle->refCount > 0) {
      SP_Unlock(&vscsiHandleArrayLock);
      return;
   }
   SP_Unlock(&vscsiHandleArrayLock);

   for (token = virtInfo->resultListHead; token != NULL; token = next) {
      next = token->nextForCallee;
      Async_ReleaseToken(token);
   }

   // If there was an extended command that got abandoned
   // then free it now.
   if (handle->flags & SCSI_HANDLE_EXTSG) {
      ASSERT(virtInfo->sgExtCmd);
      Mem_Free(virtInfo->sgExtCmd);
      virtInfo->sgExtCmd = 0;
      virtInfo->sgMax = 0;
      handle->flags &= ~(SCSI_HANDLE_EXTSG);
   }
   Mem_Free(handle);
}
/*
 *----------------------------------------------------------------------
 *
 * VSCSICloseDevice --
 *
 *      Close the SCSI device named by the handle id.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	A device handle is freed.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
VSCSICloseDevice(World_ID worldID, 
                 VSCSI_HandleID handleID)
{   
   VMK_ReturnStatus status = VMK_OK;
   VSCSI_Handle *handle;
   SCSIVirtInfo *virtInfo;

   SP_Lock(&vscsiHandleArrayLock);

   handle = vscsiHandleArray[handleID & VSCSI_HANDLE_MASK];
   if (handle == NULL) {
      SP_Unlock(&vscsiHandleArrayLock);
      VmWarn(worldID, "Can't find handle 0x%x", handleID);
      status = VMK_NOT_FOUND;
      return status;
   }
   virtInfo = handle->info;

   if ((handle->handleID != handleID || virtInfo->worldID != worldID)) { 
      LOG(0, "handleID (%d ?= %d) worldID (%d ?= %d)",
	      handle->handleID, handleID, virtInfo->worldID, worldID);   
      handle = NULL;
      status = VMK_BAD_PARAM;
   } else {
      vscsiHandleArray[handleID & VSCSI_HANDLE_MASK] = NULL;
   }

   SP_Unlock(&vscsiHandleArrayLock);

   if (handle != NULL) {
      if (handle->pendCom > 0) {
         VmWarn(worldID, "closing handle 0x%x with %d pending cmds", 
                handleID, handle->pendCom);
      }
      virtInfo->devOps->VSCSI_VirtClose(virtInfo);
      Semaphore_RWCleanup(&virtInfo->rwlock);
      Mem_Free(virtInfo);
   }
   return status;
}
