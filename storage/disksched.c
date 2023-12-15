/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * These are the SCSI functions that are specifically related to
 * disk scheduling and queueing SCSI commands that can't immediately be
 * issued.
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "libc.h"
#include "splock.h"
#include "vmk_scsi.h"
#include "scsi_int.h"
#include "parse.h"
#include "fsSwitch.h"
#include "config.h"
#include "world.h"

#define LOGLEVEL_MODULE SCSI
#define LOGLEVEL_MODULE_LEN 4
#include "log.h"

// struct used for printing qinfo stats for proc node
typedef struct SCSI_QInfo { 	       
  uint32 active;
  uint32 qlen;
  uint64 vt;
} SCSI_QInfo;

// struct used for delayed proc node registration to avoid deadload
typedef struct SCSISharesRegisterCBInfo {
   World_ID worldID;
   SCSI_SchedQElem *sPtr;
} SCSISharesRegisterCBInfo;

// Constants and variables related to disk BW scheduling
#define SCSI_SCHED_STRIDE1 (500 * 1000000)
#define SCSI_SCHED_COSTUNIT 4096
// max data xfer size per cmd for deciding max lag of lvt 
#define SCSI_SCHED_MAX_SIZE 64*1024                                        
#ifdef VMX86_DEBUG
uint32 SCSISchedDebugVal = 0;
#define SCSI_SCHED_DEBUG SCSISchedDebugVal
#else
#define SCSI_SCHED_DEBUG  0
#endif
//#define SCSI_SCHED_NODBW 1

static void SCSISharesRegisterCallback(void *data, Timer_AbsCycles timestamp);
static SCSI_SchedQElem * SCSIQPolicy(SCSI_Target *target);
static int SCSIProcSharesRead(Proc_Entry *entry, char *buffer, int *len);
static int SCSIProcSharesWrite(Proc_Entry *entry, char *buffer, int *len);
static void SCSIProcPrintQInfo(SCSI_QInfo *stats, char* buffer, int* lenp) ;
static void SCSIProcPrintQHdr(char* buffer, int* lenp);

/*
 *----------------------------------------------------------------------
 *
 *  SCSISchedUpdateGlobalShares--
 *
 *      Called when a world transitions between active and passive
 *      to adjust global shares and stride
 *
 * Results: 
 *      None
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
static INLINE void
SCSISchedUpdateGlobalShares(SCSI_Target *target, int32 shares) 
{
   target->gShares += shares;
   if (target->gShares == 0) {
      target->gStride = 0;
   } else {
      ASSERT(target->gShares > 0);
      target->gStride = SCSI_SCHED_STRIDE1/target->gShares;
   }
}

/*
 *----------------------------------------------------------------------
 *
 *  SCSISchedAdjustVT --
 *
 *      Called when a world becomes active on a target
 *      to cap its virtual time within a threshold of the
 *      Global virtual time.
 *
 * Results: 
 *	None
 *
 * Side effects:
 *	
 *
 *----------------------------------------------------------------------
 */
static INLINE void
SCSISchedAdjustVT(SCSI_Target *target, SCSI_SchedQElem *sPtr)
{
   uint64 diff =
      CONFIG_OPTION(DISK_ISSUE_QUANTUM)*(1+SCSI_SCHED_MAX_SIZE/4096)*sPtr->stride;
   uint64 olvt = 0;

   if (SCSI_SCHED_DEBUG > 1) {
      olvt = sPtr->lvt;
   }

   if (sPtr->lvt + diff < target->gvt) {
      sPtr->lvt = target->gvt - diff;
   } else if (target->gvt + diff < sPtr->lvt) {
      sPtr->lvt = target->gvt + diff;
   }

   if (SCSI_SCHED_DEBUG > 1) {
        static uint32 count = 0;
        if (!(++count & 0xff)) {
           Warning("SCSISchedAdjustVT: (%u) world %d  olvt %Lu  lvt %Lu  diff %Lu gvt %Lu",
                   count, sPtr->worldID, olvt, sPtr->lvt, diff, target->gvt);
        }
     }
}


// This set of parameters are for controlling the scsi queue out to the
// adapter. If only one VM is active to a target we want the queue to
// be the max supported by the adapter. If there are multiple VMs active
// to a target, we want to shrink the queue to give us better control over
// disk bandwidth allocation.
// Thresholds to decide when to shrink or expand the outstanding queue
// depth. Every time we see a different VM being scheduled we increment
// qControlCount. The queue is shrunk when this count equals
// scsiQControlVMSwitches. If a single VM is able to schedule
// scsiQControlReqCount continuous requests, we assume that only
// one VM is active and increase the queue size. Also, reset multiVMCount to
// zero any way
#define scsiQControlReqCount CONFIG_OPTION(DISK_QCONTROL_REQS)
#define scsiQControlVMSwitches  CONFIG_OPTION(DISK_QCONTROL_SWITCHES)

#define scsiQControlOneVM 0
#define scsiQControlManyVM   1

/*
 *----------------------------------------------------------------------
 *
 * SCSISchedIssued --
 *
 *      Called to check if a command can be issued to the driver.
 *	If so, increments active counts, and updates the LVT and
 *      lastIssued info for target.
 *      May change queue depth based on how many VMs have been issuing
 *	to target.
 *      
 *      Called with adapter lock held.
 *
 * Results: 
 *	VMK_WOULD_BLOCK if there is no space at the target or adapter
 *      else VMK_OK
 *
 * Side effects:
 *      Increments adapter, target and sched counts
 *	
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
SCSISchedIssued(SCSI_Adapter *adapter, SCSI_Target *target,
		SCSI_Handle *handle, SCSI_Command *cmd, SCSI_ResultID *rid)
{
   SCSI_SchedQElem *sPtr;
   World_ID worldID = rid->token->resID;
   VMK_ReturnStatus retval;

   ASSERT(SP_IsLocked(&adapter->lock));
#ifdef SCSI_SCHED_NODBW
   worldID = hostWorld->worldID;
#endif 

   // Check if the command doesn't use queue. 
   if (cmd->flags & SCSI_CMD_BYPASSES_QUEUE) {
      return VMK_OK;
   }

   // Check if this command can be issued or must be queued.
   if (adapter->asyncInProgress >= *(adapter->qDepthPtr) ||
       target->active >= target->curQDepth) {
	LOG(2,"SCSISchedIssued - command cannot be issued, "
              "asyncInProgress = %d, qDepth = %d, active = %d, curQDepth = %d",
	    adapter->asyncInProgress, *(adapter->qDepthPtr),
	    target->active, target->curQDepth );
      retval = VMK_WOULD_BLOCK;
   } else {
      retval = VMK_OK;
   }

   // Allocate disk sched elem.
   sPtr = SCSISchedQFind(target, worldID);
   if (!sPtr) {
      sPtr = SCSISchedQAlloc(target, worldID);
   }
   ASSERT(sPtr);

   if (retval == VMK_WOULD_BLOCK) {
      return retval;
   }

   if (!sPtr->active) {
      // We just got active again
      sPtr->active = TRUE;
      SCSISchedUpdateGlobalShares(target, sPtr->shares);
      SCSISchedAdjustVT(target, sPtr);
   }


   if (SCSI_SCHED_DEBUG > 1) {
      Warning("targ %d (%d) for world %d [m%Lu][q%u][a%d] sn %u",
              target->id, target->lastNReq,
              worldID, sPtr->lvt, sPtr->queued, sPtr->cif,
              cmd->serialNumber);
   }


   // update the usage information 
   adapter->asyncInProgress++;
   target->active++;
   sPtr->cif++;
   sPtr->lvt += (1 + cmd->dataLength/SCSI_SCHED_COSTUNIT)*sPtr->stride;
   target->gvt += (1 + cmd->dataLength/SCSI_SCHED_COSTUNIT)*target->gStride;

   // update last issued state
   if (target->lastWorldIssued == sPtr) {
      target->lastNReq++;
      // Seems like only one active VM
      // Check to see if we should expand the queue
      
      if (target->lastNReq == scsiQControlReqCount) {
         if (target->qControlState == scsiQControlManyVM) {
            // Time to increase the queue size
            target->curQDepth = target->maxQDepth;
            if (SCSI_SCHED_DEBUG > 1) {
               Log("increasing queue depth to max for %s:%d:%d (%d) (n%d) (mc%d)",
                   target->adapter->name, target->id, target->lun,
                   sPtr->worldID, target->lastNReq, target->qControlCount);
            }
            target->qControlCount = 0;
            target->qControlState = scsiQControlOneVM;
         } else {
            // Hit the high threshold after (one or more) switches. 
            // Reset the count 
            target->qControlCount = 0;
         }
      }
   } else {
      // We have more than one VM active on the target
      // Check to see if we need to shrink the queue
      if ((target->qControlState == scsiQControlOneVM) &&
      (++(target->qControlCount) == scsiQControlVMSwitches)) {
         if (SCSI_SCHED_DEBUG > 1) {
            Log("reducing queue depth to min for %s:%d:%d (%d->%d) (n%d) (mc%d)",
                target->adapter->name, target->id, target->lun,
                target->lastWorldIssued ? target->lastWorldIssued->worldID : 0,
		sPtr->worldID, target->lastNReq, target->qControlCount);
         }

         target->curQDepth = CONFIG_OPTION(DISK_CIF)>target->maxQDepth?target->maxQDepth:CONFIG_OPTION(DISK_CIF);
         target->qControlState = scsiQControlManyVM;
         target->qControlCount = 0;
      }
      target->lastWorldIssued = sPtr;
      target->lastNReq = 1;

   }
   if (cmd->sectorPos) {
      target->lastReqSector = cmd->sectorPos + cmd->dataLength/DISK_SECTOR_SIZE;
   }
   
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * SCSISchedDone --
 *
 *      Called when a command is done to decrement the active counts
 *
 * Results: 
 *	None
 *
 * Side effects:
 *	
 *
 *----------------------------------------------------------------------
 */
void
SCSISchedDone(SCSI_Adapter *adapter, SCSI_Target *target, SCSI_ResultID *rid)
{
   SCSI_SchedQElem *ptr;
   World_ID worldID = rid->token->resID;

#ifdef SCSI_SCHED_NODBW
   worldID = hostWorld->worldID;
#endif

   ASSERT(SP_IsLocked(&adapter->lock));
   if (rid->path != NULL) { 
      ASSERT(rid->path->active > 0);
      rid->path->active--;
   }
   if (rid->cmd && (rid->cmd->flags & SCSI_CMD_BYPASSES_QUEUE)) {
      return;
   }
   ASSERT(target->schedQ);

   ptr = SCSISchedQFind(target,  worldID);
   ASSERT(ptr);
   ASSERT(ptr->cif > 0);
   ASSERT(adapter->asyncInProgress > 0);
   ASSERT(target->active > 0);
   ASSERT(ptr->cif > 0);
   adapter->asyncInProgress--;
   target->active--;
   ptr->cif--;

   // if we are now inactive adjust global shares
   if (!ptr->cif && !ptr->queued) {
      ptr->active = FALSE;
      SCSISchedUpdateGlobalShares(target, (-ptr->shares));
   }

   if (SCSI_SCHED_DEBUG > 1) {
      Warning("SchedDone: targ %d (%d) for world %d [m%Lu][q%u][a%d] sn %u",
              target->id, target->lastNReq,
              worldID, ptr->lvt, ptr->queued, ptr->cif, 
              rid->serialNumber);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * SCSISchedQAlloc --
 *
 *      Allocate a sched element corresponding to the specified world
 *      for the specified target
 *
 * Results: 
 *	A new initialized element
 *
 * Side effects:
 *	none
 *
 *----------------------------------------------------------------------
 */
SCSI_SchedQElem *
SCSISchedQAlloc(SCSI_Target *target, World_ID worldID) 
{
   SCSI_SchedQElem *sPtr;
   World_Handle *world;
   SCSISharesRegisterCBInfo *cbInfo;

   ASSERT(SP_IsLocked(&target->adapter->lock));
   if (SCSI_SCHED_DEBUG) {
      LOG(0, "vm %d: SchedQAlloc: target %d", worldID, target->id);
   }
   sPtr = (SCSI_SchedQElem *)Mem_Alloc(sizeof(SCSI_SchedQElem));
   if (!sPtr) {
      // Out of memory
      ASSERT(0);
      return NULL;
   }
   memset(sPtr, 0, sizeof(SCSI_SchedQElem));
   sPtr->worldID = worldID;
   sPtr->target = target;
   sPtr->next = target->schedQ;
   target->schedQ = sPtr;

   // queue on target list for the world 
   world = World_Find(worldID);
   ASSERT(world != NULL);
   if (world == NULL) {
      Mem_Free(sPtr);
      return(NULL);
   }
   SP_Lock(&world->scsiState->targetListLock);
   sPtr->nextInWorld = world->scsiState->targetList;
   world->scsiState->targetList = sPtr;
   SP_Unlock(&world->scsiState->targetListLock);
   World_Release(world);

   // initialize fields
   sPtr->active = FALSE;
   sPtr->lvt = target->gvt;
   sPtr->cif = 0;
   sPtr->queued = 0;
   sPtr->reqQueHead = NULL;
   sPtr->reqQueTail = NULL;
   sPtr->priReqQueHead = NULL;
   sPtr->priReqQueTail = NULL;
   sPtr->shares = SCSI_SCHED_SHARES_NORMAL;
   sPtr->stride = SCSI_SCHED_STRIDE1/sPtr->shares;
   
   /* We can't directly register the shares proc node here because we
    * are holding the adapterlock, and proc node registration will
    * require also grabbing the proc lock.  This can create deadlock
    * because adapter lock is grabbed after proc lock when reading
    * proc nodes. So, we register the proc node from a timer callback.
    */
   // initialize to NULL in case register fails, in which case we don't
   // want to unregister
   sPtr->procShares.parent = NULL;
   cbInfo = (SCSISharesRegisterCBInfo*)Mem_Alloc(sizeof(SCSISharesRegisterCBInfo));
   if (cbInfo == NULL) {
      return sPtr;
   }

   cbInfo->worldID = worldID;
   cbInfo->sPtr = sPtr;
   Timer_Add(MY_PCPU, SCSISharesRegisterCallback, 1, TIMER_ONE_SHOT,
             (void *)cbInfo);

   return(sPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * SCSISchedQFind --
 *
 *      Find the sched element corresponding to the specified world
 *      for the specified target
 *
 * Results: 
 *	The element if it exists, else NULL
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
SCSI_SchedQElem *
SCSISchedQFind(SCSI_Target *target, World_ID worldID) 
{
   SCSI_SchedQElem *sPtr;
   
   ASSERT(SP_IsLocked(&target->adapter->lock));
   // First search for a match
   sPtr = target->schedQ;
   for (; sPtr; sPtr=sPtr->next) {
      if (sPtr->worldID == worldID) {
         return(sPtr);
      }
   }
   LOG(1, "not found for world %d, target %d", worldID, target->id);
   return(NULL);
}



/*
 *----------------------------------------------------------------------
 *
 * SCSISchedQFree --
 *
 *      Free the specified sched queue element
 *
 * Results: 
 *	None
 *
 * Side effects:
 *	
 *
 *----------------------------------------------------------------------
 */
void
SCSISchedQFree(SCSI_Target *target, SCSI_SchedQElem *sPtr) 
{
   SCSI_SchedQElem *ptr;
   SCSI_QElem **head;
   SCSI_QElem **tail;
   SCSI_QElem *elem = 0;
   SCSI_ResultID rid;

   ASSERT(SP_IsLocked(&target->adapter->lock));
   // Windows seems to shutdown with a rewind command outstanding 
   // This avoids a purple screen when that happens. Should be fixed
   // later by aborting the outstanding commands.
   if (sPtr->active) {
      VmWarn(sPtr->worldID, "target %d with outstanding commands, cif = %d, queue=%d",
             target->id, sPtr->cif, sPtr->queued);     
   }

   if (SCSI_SCHED_DEBUG) {
      VMLOG(0, sPtr->worldID, "SchedQFree: target %d", target->id);
   }

   ptr = target->schedQ;
   if (sPtr == target->schedQ) {
      target->schedQ = sPtr->next;
   } else {
      for (; ptr; ptr=ptr->next) {
         if (ptr->next == sPtr) {
            ptr->next = sPtr->next;
            break;
         }
      }
   }
   //
   // Set the Q counters correctly so that
   // SCSI_DoCmdComplete() will not try to start a queued command
   // for this world.
   ASSERT(target->qcount >= sPtr->queued);
   ASSERT(target->adapter->qCount >= sPtr->queued);
   target->qcount -= sPtr->queued; 
   target->adapter->qCount -= sPtr->queued;

   if (target->lastWorldIssued == sPtr) {
      target->lastWorldIssued = NULL;
   }

   // Found a matching element
   ASSERT(ptr);

   // remove the proc element
   if (sPtr->procShares.parent) {
      Proc_Remove(&(sPtr->procShares));
   }

   /*
    * SCSI_QElems may be left on the queue if the cable is pulled and the VM
    * is exiting. Since the device is gone the I/Os cannot be completed, free the 
    * QElems and reduce the appropriate queue counts.
    */
   while(sPtr->queued > 0) {
      head = &sPtr->priReqQueHead;
      tail = &sPtr->priReqQueTail;

      if (*head == NULL) { 
        head = &sPtr->reqQueHead;
        tail = &sPtr->reqQueTail;
      }
      ASSERT(*head != NULL); 
     
      elem = *head;
      if(*head == *tail) {
        // queue is now empty
        *head = *tail = 0;
      } else {
        *head = elem->next;
      }

      ASSERT(sPtr->queued > 0);
      sPtr->queued--;

      SP_Unlock(&target->adapter->lock);

      /*
       * Complete the commands. This code is similar to that
       * found in SCSIAbortCommand/SCSIResetCommand.
       */
      SCSIInitResultID(elem->handle, elem->token, &rid);
      rid.serialNumber = elem->cmd->serialNumber;
      SCSI_DoCommandComplete(&rid, SCSI_HOST_ERROR << 16, zeroSenseBuffer, 0, 0);

      rid.cmd = elem->cmd;
      rid.path = target->activePath;
      SCSI_UpdateCmdStats(elem->cmd, &rid, sPtr->worldID);

      Async_ReleaseToken(elem->token);
      SCSI_HandleRelease(elem->handle);
      Mem_Free(elem->cmd);
      SCSIQElemFree(elem);
      
      SP_Lock(&target->adapter->lock);
   }

   ASSERT(sPtr->queued == 0);
   Mem_Free(sPtr);
}

/*
 * Registers the /proc/vmware/VM/#/disk/vmhba#:target:lun node
 * that currently just has the shares in it
 */
static void 
SCSIProcTargetSharesRegister(World_ID worldID, SCSI_SchedQElem *sPtr, uint32 shares)
{
   char buf[64]; // this is enough space assuming adapname vmhba+16chars

   World_Handle *world = World_Find(worldID);
   if (!world) {
      WarnVmNotFound(worldID); 
      return;
   }

   // "disk/adapname:target:lun" entry
   memset(&sPtr->procShares, 0, sizeof(sPtr->procShares));
   
   sPtr->procShares.parent = &world->scsiState->procWorldDiskDir;
   sPtr->procShares.read = SCSIProcSharesRead;
   sPtr->procShares.write = SCSIProcSharesWrite;
   sPtr->procShares.private = sPtr;   
   snprintf(buf, sizeof buf, "%s:%d:%d", 
           sPtr->target->adapter->name, sPtr->target->id, sPtr->target->lun); 
   Proc_Register(&(sPtr->procShares), buf, FALSE);
   World_Release(world);
}


/*
 * Returns the number of shares when someone reads the
 * /proc/vmware/VM/#/disk/vmhba#:target:lun node
 */
static int
SCSIProcSharesRead(Proc_Entry *entry,
                   char       *buffer,
                   int        *len)
{
   SCSI_SchedQElem *sPtr = (SCSI_SchedQElem *) entry->private;
   SCSI_Stats *stats = &sPtr->stats;
   SCSI_QInfo qinfo;

   *len = 0;

   Proc_Printf(buffer, len, "    shares ");
   SCSIProcPrintHdr(buffer, len);
   SCSIProcPrintQHdr(buffer, len);
   Proc_Printf(buffer, len, "\n");

   Proc_Printf(buffer, len, "%10d ", sPtr->shares);
   SCSIProcPrintStats(stats, buffer, len);
 
   qinfo.active = sPtr->cif;
   qinfo.qlen = sPtr->queued;
   qinfo.vt = sPtr->lvt;
   SCSIProcPrintQInfo(&qinfo, buffer, len);
   Proc_Printf(buffer, len, "\n");
   
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * SCSIParseShares --
 *
 *      Parses "buf" as a disk shares value.  The special values
 *	"high", "normal", and "low" are converted into appropriate
 *	corresponding numeric values.
 *
 * Results:
 *      Returns VMK_OK and stores value in "shares" on success.
 *	Returns VMK_BAD_PARAM if "buf" could not be parsed.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
SCSIParseShares(const char *buf, uint32 *shares)
{
   // parse special values: high, normal, low
   if (strcmp(buf, "high") == 0) {
      *shares = SCSI_SCHED_SHARES_HIGH;
      return(VMK_OK);
   } else if (strcmp(buf, "normal") == 0) {
      *shares = SCSI_SCHED_SHARES_NORMAL;
      return(VMK_OK);
   } else if (strcmp(buf, "low") == 0) {
      *shares = SCSI_SCHED_SHARES_LOW;
      return(VMK_OK);
   }

   // parse numeric value
   return(Parse_Int(buf, strlen(buf), shares));
}

/*
 * Sets the number of shares when someone writes the
 * /proc/vmware/VM/#/disk/vmhba#:target:lun node
 */
static int
SCSIProcSharesWrite(Proc_Entry *entry,
                    char       *buffer,
                    int        *len)
{
   SCSI_SchedQElem *sPtr = (SCSI_SchedQElem *) entry->private;
   World_ID worldID = sPtr->worldID;
   int32 shares;
   char *argv[2];
   int argc;
   
   // parse buffer into args (assumes OK to overwrite)
   argc = Parse_Args(buffer, argv, 2);
   if (argc != 1) {
      VmWarn(worldID, "invalid shares: unable to parse");
      return(VMK_BAD_PARAM);
   }

   // parse shares
   if (SCSIParseShares(argv[0], &shares) != VMK_OK) {
      VmWarn(worldID, "invalid shares: unable to parse");
      return(VMK_BAD_PARAM);
   }

   // fail if outside valid range
   if ((shares < SCSI_SCHED_SHARES_MIN) || (shares > SCSI_SCHED_SHARES_MAX)) {
      VmWarn(worldID, "invalid shares: %u", shares);
      return(VMK_BAD_PARAM);
   }
   
   VMLOG(0, worldID,
         "changing shares for %s:%u:%u from %u to %u",
         sPtr->target->adapter->name, sPtr->target->id, sPtr->target->lun,
         sPtr->shares, shares);

   SP_Lock(&(sPtr->target->adapter->lock));
   // Update global and local fields
   if (sPtr->active) {
      SCSISchedUpdateGlobalShares(sPtr->target, (shares - sPtr->shares));
   }
   sPtr->shares = shares;
   sPtr->stride = SCSI_SCHED_STRIDE1/shares;
   SP_Unlock(&(sPtr->target->adapter->lock));

   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * SCSISharesRegisterCallback --
 *
 *      Timer callback to register shares proc node
 *
 * Results: 
 *	None
 *
 * Side effects:
 *	Register proc node
 *
 *----------------------------------------------------------------------
 */
static void
SCSISharesRegisterCallback(void *data, UNUSED_PARAM(Timer_AbsCycles timestamp))
{
   SCSISharesRegisterCBInfo *cbInfo = (SCSISharesRegisterCBInfo *)data;
   SCSIProcTargetSharesRegister(cbInfo->worldID, cbInfo->sPtr,
                                cbInfo->sPtr->shares);
   Mem_Free(cbInfo);
}

void
SCSIProcPrintPerVM(char *page, int *len, SCSI_Target *target)
{
   SCSI_SchedQElem *sPtr;
   SCSI_Stats *stats;
   uint32 totShares = 0;
   SCSI_QInfo qinfo;

      // stats for VMs active on this target
   Proc_Printf(page, len, "\n %6s %7s", "VM", "Shares"); 
   SCSIProcPrintHdr(page, len);
   SCSIProcPrintQHdr(page, len);
   Proc_Printf(page, len, "\n");
   for (sPtr = target->schedQ; sPtr; sPtr=sPtr->next) {
      stats = &sPtr->stats;
      Proc_Printf(page, len, " %6u %7u", sPtr->worldID, sPtr->shares);
      SCSIProcPrintStats(stats, page, len);
      qinfo.active = sPtr->cif;
      qinfo.qlen = sPtr->queued;
      qinfo.vt = sPtr->lvt;
      SCSIProcPrintQInfo(&qinfo, page, len);
      Proc_Printf(page, len, "\n");
      totShares += sPtr->shares; 
   }

   // target cumulative stats
   Proc_Printf(page, len, " %6s %7u", "Total", totShares); 
   SCSIProcPrintStats(&target->stats, page, len);
   qinfo.active = target->active;
   qinfo.qlen = target->qcount;
   qinfo.vt = target->gvt;
   SCSIProcPrintQInfo(&qinfo, page, len);
   Proc_Printf(page, len, "\n");
}

static void
SCSIProcPrintQHdr(char* buffer, 
                 int* lenp) 
{
   Proc_Printf(buffer, lenp, " %10s %10s %17s ",
                             "active", "queued", "virtTime");
}

static void
SCSIProcPrintQInfo(SCSI_QInfo *stats,
                   char* buffer, 
                   int* lenp) 
{
   Proc_Printf(buffer, lenp, " %10u %10u %17Lu ", 	       
               stats->active, stats->qlen, stats->vt);
}


/*
 * The following functions are for dealing with SCSI_QElems, which are
 * used in queuing up SCSI commands that can't be issued immediately.
 */

SCSI_QElem *
SCSIQElemAlloc(void)
{
  SCSI_QElem *tmpElem;
  tmpElem = (SCSI_QElem *)Mem_Alloc(sizeof(SCSI_QElem));

  return(tmpElem);
}


void
SCSIQElemFree(SCSI_QElem *elem)
{
   Mem_Free(elem);
}


/*
 * Add a SCSI_QElem to one of the target command queues. If priority is
 * TRUE, use the priority command queue, otherwise the reqular commmand
 * queue. The priority queue is typically used for requeueing failed I/O
 * requests so that they will be issued quickly.  If qhead is TRUE, place
 * the command at the beginning of the specified command queue, otherwise
 * at its tail.  Requires that the adapter lock is held.
 */
void
SCSIQElemEnqueue(SCSI_Target *target, SCSI_QElem *elem, int qhead, int priority)
{
  SCSI_QElem **head;
  SCSI_QElem **tail;
  SCSI_SchedQElem *sPtr;
  World_ID worldID = elem->token->resID;

   ASSERT(SP_IsLocked(&target->adapter->lock));
#ifdef SCSI_SCHED_NODBW
   worldID = hostWorld->worldID;
#endif

  sPtr = SCSISchedQFind(target,  worldID);
  ASSERT(sPtr);

  if (SCSI_SCHED_DEBUG > 1) {
     Warning ("Elem %p for world %d target %d",
              sPtr, worldID, target->id);
  }

  if (priority == SCSI_QPRIORITY) {
     head = &sPtr->priReqQueHead;
     tail = &sPtr->priReqQueTail;
  } else {
     head = &sPtr->reqQueHead;
     tail = &sPtr->reqQueTail;
  }

  // increment counts
  target->adapter->qCount++;
  target->qcount++;
  sPtr->queued++;

  if (!sPtr->active) {
     // We just got active again
     sPtr->active = TRUE;
     SCSISchedUpdateGlobalShares(target, sPtr->shares);
     SCSISchedAdjustVT(target, sPtr);
  }

  elem->next = NULL;
  if(!qhead) {
    if (*tail) {
      (*tail)->next = elem;
      *tail = elem;
    } else {
      *head = *tail = elem;
    }
  } else {
    if (*head) {
      elem->next = *head;
      *head = elem;
    } else {
      *head = *tail = elem;
    }
  }
}

/*
 * Remove a SCSI_QElem from the appropriate world queue based on what
 * SCSIQPolicy returns.  Requires that the adapter lock is held.
 */
SCSI_QElem *
SCSIQElemDequeue(SCSI_Target *target)
{
  SCSI_QElem **head;
  SCSI_QElem **tail;
  SCSI_QElem *res = 0;
  SCSI_SchedQElem *sPtr;

  ASSERT(SP_IsLocked(&target->adapter->lock));
  sPtr = SCSIQPolicy(target);
  ASSERT(sPtr || !(target->qcount));

  if (SCSI_SCHED_DEBUG > 1) {
     Warning ("Policy returned %p for world %d",
              sPtr, sPtr->worldID);
  }

  // if there is something on the queue return it
  if (sPtr) {
     head = &sPtr->priReqQueHead;
     tail = &sPtr->priReqQueTail;
     if(*head == NULL) { 
        head = &sPtr->reqQueHead;
        tail = &sPtr->reqQueTail;
     }
     
     if(*head  != 0) {
        res = *head;
        if(*head == *tail) {
           // queue is now empty
           *head = *tail = 0;
        } else {
           *head = res->next;
        }
     }

     target->qcount--;
     sPtr->queued--;
     target->adapter->qCount--;
  }

  return(res);
}

/*
 *----------------------------------------------------------------------
 *
 *  SCSIQPolicy--
 *
 *      Policy module to decide the next world to issue a request
 *      Currently it is completely per target
 *
 * Results: 
 *	sched element for world that will issue the next request
 *      NULL if there are no requests for this target
 *
 * Side effects:
 *	
 *
 *----------------------------------------------------------------------
 */
static SCSI_SchedQElem *
SCSIQPolicy(SCSI_Target *target)
{
  SCSI_SchedQElem *sPtr = NULL;
  SCSI_SchedQElem *last = target->lastWorldIssued;
  uint32          nextCmdSectorPos = -1;		

  
  ASSERT(SP_IsLocked(&target->adapter->lock)); 

  // This is the current "policy module"
  // Issue from the last issued world if it meets a bunch of criteria
  if ( last && last->queued ) {
     if (last->priReqQueHead != NULL) {
        nextCmdSectorPos = last->priReqQueHead->cmd->sectorPos;
     } else {
        nextCmdSectorPos = last->reqQueHead->cmd->sectorPos;
     }

     if ((nextCmdSectorPos > target->lastReqSector)
       && ((target->lastReqSector + CONFIG_OPTION(DISK_SECTOR_DIFF)) > 
           nextCmdSectorPos)
       && target->lastNReq < CONFIG_OPTION(DISK_ISSUE_QUANTUM)) {
        if (SCSI_SCHED_DEBUG > 1) {
           Warning("QElemDequeue: last world %d (%d) [m%Lu][q%u]",
                last->worldID, target->lastNReq, 
                last->lvt,  last->queued);
        }
        sPtr = last;
     } 
  }

  if (sPtr == NULL) {
     uint64 min = 0xffffffffffffffffLL;
     SCSI_SchedQElem *ptr = target->schedQ;
      for (; ptr; ptr=ptr->next) {
         if (ptr->queued && (ptr->lvt < min)) {
            ASSERT(ptr->active);
            sPtr = ptr;
            min = ptr->lvt;
         }
      }
  }
  return(sPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * SCSIDetachQElem --
 *
 *      Find "cmd" (or fragment of it) on the sched queue for the handle.
 *
 *      The function will either return the Command searched (in that
 *      case cmd->serialNumber will match) or any fragment of the command
 *      if it was split (in which case cmd->originSN will match).
 *
 *      If the argument findAny is true, we will return the first element
 *      found that matches the handle.
 *
 * Results: 
 *	SchedQElem if found, 
 *      NULL otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

SCSI_QElem*
SCSIDetachQElem(SCSI_Handle *handle, World_ID worldID,
                SCSI_Command *cmd, Bool findAny)
{
   SCSI_SchedQElem *qPtr;
   SCSI_QElem **qHead, **qTail;
   SCSI_QElem *curr, *prev, *ret;
   SCSI_Target *target;
   Bool foundElem = FALSE;

   ret = NULL;
   
   ASSERT(handle);

   target = handle->target;

   ASSERT(SP_IsLocked(&target->adapter->lock));
  
   // Find the correct queue
   qPtr = SCSISchedQFind(target, worldID);
   if (qPtr) {
      int i;
      LOG(3, "found q for target %p world %d", target, worldID); 
      for (i = SCSI_QREGULAR; i <= SCSI_QPRIORITY; i++ ) {
         ASSERT(foundElem == FALSE);
         if (i == SCSI_QPRIORITY) {
            qHead = &qPtr->priReqQueHead;
            qTail = &qPtr->priReqQueTail;
         } else if (i == SCSI_QREGULAR) {
            qHead = &qPtr->reqQueHead;
            qTail = &qPtr->reqQueTail;
         } else {
            qHead = NULL;
            qTail = NULL;
         }

         for (curr = *qHead, prev = NULL;
              curr; 
              prev = curr, curr = curr->next) {
            if (findAny && (curr->cmd->originHandleID == cmd->originHandleID)) {
               foundElem = TRUE;
            }
            else if (((curr->cmd->serialNumber == cmd->serialNumber) &&
                      (curr->cmd->originHandleID == cmd->originHandleID))
                     || ((curr->cmd->originSN == cmd->serialNumber) &&
                         (curr->cmd->originHandleID == cmd->originHandleID))) {
               foundElem = TRUE;
            }
            if (foundElem) { 
               // remove elem from schedQ
               ret = curr;
               if (curr == *qHead) {
                  ASSERT(prev == NULL);
                  *qHead = curr->next;
               } else {
                  prev->next = curr->next;
               }
               if (curr == *qTail) {
                  ASSERT(curr->next == NULL);
                  *qTail = prev;
               }
               target->qcount--;
               qPtr->queued--;
               target->adapter->qCount--;
               return ret;
            }
         }
      }
   }
   
   return ret;
}
