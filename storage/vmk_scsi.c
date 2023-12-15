/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/************************************************************
 *
 *  vmk_scsi.c
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
#include "scsi_vmware.h"

#define LOGLEVEL_MODULE SCSI
#include "log.h"

/* Useful for debugging/performance monitoring.  If set, it forces all I/O
 * (even those < 4GB) to use the PAE copy mechanism.  */
#define IO_FORCE_COPY (VMK_STRESS_RELEASE_OPTION(IO_FORCE_COPY))

// Initialize the scsiCmdInfo array for vmkernel (see scsi_defs.h)
SCSICmdInfo scsiCmdInfo[] = { SCSI_CMD_INFO_DATA };

/* Time in milliseconds before vmkernel will timeout waiting for a
 * synchronous SCSI command to complete. */
#define SCSI_TIMEOUT			40000
#ifdef DELAY_TEST
#define SCSI_CMD_TIMEDOUT		0x80
#endif

#define TIMEOUT_RETRIES 4

/* Max number of characters the devNum can occupy in decimal
 * notation (9 ==> 10^10-1 > MAX_INT)
 */
#define SCSI_DEVNUM_MAX_CHAR 9

// Per-adapter bitmaps to tell COS for which tgt/lun completions are pending.
Atomic_uint32 scsiCmplBitmaps[MAX_SCSI_ADAPTERS];

#ifdef VMX86_DEVEL
static DropCmdType dropScsiCmd = DROP_NONE; // drop a scsi command, test & dbg. 
#endif

/*
 * vmklinux callback for char device ioctls.
 */
SCSICharDevIoctlFn scsiCharDevIoctl;

/* Protects adapterHashTable[], VMFSPartitionList, numSCSITargets, and
 * numSCSIAdapters.
 */
SP_SpinLock	scsiLock;

SCSI_Adapter *adapterHashTable[HASH_BUCKETS];

#define MAX_SCSI_HANDLES 	256
#define SCSI_DEVICE_NAME_LEN	20
#define SCSI_HANDLE_MASK	0xff
static SCSI_Handle *handleArray[MAX_SCSI_HANDLES];
static SP_SpinLock handleArrayLock;

/* How many times we've gone around handleArray[] allocating handles. */
static uint32 handleGeneration = 1;
/* Next location in handleArray[] to look for available handle */
static uint32 nextHandle = 0;

/* Number of adapters created by SCSI_CreateDevice() and not yet destroyed
 * (doesn't include adapters created for virtual SCSI handles). Protected
 * by scsiLock */
static int numSCSIAdapters;

/* Number of targets that have been created and not yet removed. Protected
 * by scsiLock */
static int numSCSITargets;

typedef struct ScsiTimeOut {
   Async_Token *token;
   SCSI_HandleID handleID;
   Bool isRead;
} ScsiTimeOut;


/*
 * This is used to pass in a zeroed-out sense buffer to
 * SCSI_DoCommandComplete() when the result is not a check condition.  We
 * may also pass in a senseBuffer from the stack that was filled in by
 * SCSIGenericCommand(), a senseBuffer from a lower-level token, or the
 * senseBuffer from an actual SCSI result from linux_scsi.c.
 */
uint8 zeroSenseBuffer[SCSI_SENSE_BUFFER_LENGTH];


/* TRUE if a scan of a SCSI adapter (and by induction, of all VMFS
 * partitions) is in progress.*/
Bool rescanInProgress = FALSE;

COSLunList *cosLunListHead = NULL;
SP_SpinLock cosLunListLock;

static VMK_ReturnStatus SCSIDoGetCapacity(SCSI_Handle *handle,
		                           uint32 *diskBlockSize, 
					   uint32 *numDiskBlocks);
static void SCSITargetFree(SCSI_Target *target, Bool adapterFree);
static void SCSIAdapterFree(SCSI_Adapter *adapter);
static VMK_ReturnStatus SCSIGetAttrs(SCSI_Handle *handle);
static VMK_ReturnStatus SCSIUpdatePTable(SCSI_Handle *handle,
					 SCSI_Target *target);

static Bool SCSITargetConflict(SCSI_Target *target, uint32 partition,
			       int flags);

static void ScsiProcInit(void);
static void ScsiProcCleanup(void);
static void ScsiProcAddAdapter(SCSI_Adapter *adapter);
static void ScsiProcRemoveAdapter(SCSI_Adapter *adapter);

static VMK_ReturnStatus SCSISplitSGCommand(SCSI_Handle *handle, SCSI_Command *cmd,
                                           SCSI_ResultID *rid, Bool cmdIsPAEOK);
static void SplitAsyncDone(Async_Token *nToken);
static void SCSIQueueCommand(SCSI_Handle *handle, SCSI_Command *cmd, 
			     VMK_ReturnStatus *result, uint32 flags);
static void SCSIPostCmdCompletion(SCSI_Handle *handle, Async_Token *token);
static Bool SCSIIsCmdPAEOK(SCSI_Adapter *adapter, SCSI_Command *cmd);
static void SCSIUpdateCmdLatency(SCSI_Target *target, SCSI_Handle *handle,
                                 Async_Token *token);
static int ScsiProcAdapStatsRead(Proc_Entry *, char *, int *);
static int ScsiProcAdapStatsWrite(Proc_Entry *, char *, int *);
static int ScsiProcTargRead(Proc_Entry *, char *, int *);
static int ScsiProcTargWrite(Proc_Entry *, char *, int *);
static VMK_ReturnStatus SCSIDoGetTargetInfo(SCSI_Adapter *, uint32, uint32, 
                                            VMnix_TargetInfo *, Bool);
static VMK_ReturnStatus SCSIDoGetTargetInfoInt(SCSI_Adapter *adapter,
                                               SCSI_Target *target, 
                                               VMnix_TargetInfo *targetInfo,
                                               Bool validatePartitionTable);
static Bool SCSIWillClobberActivePTable(const SCSI_Handle *handle, 
                                        const SCSI_Command *cmd);
static void SCSIExecuteCommandInt(SCSI_HandleID handleID, SCSI_Command *cmd, 
				  VMK_ReturnStatus *result, uint32 flags);

/*
 * Return TRUE if the indicated SCSI status and sensebuffer indicate a
 * power-on or reset device status.
 */
static INLINE Bool
SCSIPowerOnOrReset(SCSI_Status status, uint8 *senseBuffer)
{
   return (SCSI_DEVICE_STATUS(status) == SDSTAT_CHECK &&
	   senseBuffer[2] == SCSI_SENSE_KEY_UNIT_ATTENTION &&
	   senseBuffer[12] == SCSI_ASC_POWER_ON_OR_RESET &&
	   senseBuffer[13] <= 3);
}

/*
 * Given a SCSI_HandleID, return the corresponding SCSI_Handle (after
 * increasing its refcount).  Must eventually be followed by a call to
 * SCSI_HandleRelease().
 */
INLINE SCSI_Handle *
SCSI_HandleFind(SCSI_HandleID handleID)
{
   SCSI_Handle *handle;

   SP_Lock(&handleArrayLock);
   handle = handleArray[handleID & SCSI_HANDLE_MASK];
   if (handle != NULL) {
      if (handle->handleID != handleID) {
	 handle = NULL;
      } else {
	 ASSERT(handle->refCount >= 1);
	 handle->refCount++;
      }
   }
   SP_Unlock(&handleArrayLock);
   return handle;
}

/*
 *----------------------------------------------------------------------
 *
 * NameHash --
 *
 *      Primitive hash function on the device name.
 *
 * Results: 
 *	A hash value.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static uint32 
NameHash(const char *name)
{
   uint32 sum = 0;
   while (*name != 0) {
      sum += *name;
      name++;
   }

   return sum;
}

/*
 *----------------------------------------------------------------------
 *
 * SCSI_Init --
 *
 *      Initialize data structures.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	Data structures are initialized.
 *
 *----------------------------------------------------------------------
 */
void
SCSI_Init(VMnix_SharedData *sharedData)
{
   SP_InitLock("scsiLck", &scsiLock, SP_RANK_SCSILOCK);
   SP_InitLock("handleArrayLock", &handleArrayLock, SP_RANK_HANDLEARRAY);
   SP_InitLock("cosLunListLock", &cosLunListLock, SP_RANK_LEAF);

   SHARED_DATA_ADD(sharedData->scsiCmplBitmaps, Atomic_uint32 *, scsiCmplBitmaps);
   ScsiProcInit();
   memset(zeroSenseBuffer, 0, sizeof(zeroSenseBuffer));
}

/*
 *----------------------------------------------------------------------
 *
 * SCSIFindAdapter --
 *
 *      Find a scsi adapter by name.
 *
 * Results: 
 *	The matching SCSI adapter.  Requires that 'scsiLock' is held.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static SCSI_Adapter *
SCSIFindAdapter(const char *name)
{
   SCSI_Adapter *adapter;
   uint32 hash = NameHash(name);

   ASSERT(SP_IsLocked(&scsiLock));

   for (adapter = adapterHashTable[hash % HASH_BUCKETS];
        adapter != NULL && strcmp(adapter->name, name) != 0;
	adapter = adapter->next) {
   }

   return adapter;
}

/*
 *----------------------------------------------------------------------
 *
 * Scsi_FindAdapName --
 *
 *      Find the name of a scsi adapter given bus and device/function
 *
 * Results: 
 *	Status is bad if adapter not found
 *
 * Side effects:
 *	adapName is a pointer to the name (must be copied)
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Scsi_FindAdapName(uint32 bus, uint32 devfn, char **adapName)
{
   SCSI_Adapter *adapter;
   int i;

   SP_Lock(&scsiLock);
   for (i = 0; i < HASH_BUCKETS; i++) {
      for (adapter = adapterHashTable[i]; adapter != NULL;
           adapter = adapter->next) {
	 if (adapter->bus == bus && adapter->devfn == devfn) {
	    *adapName = adapter->name;
	    LOG(0, "Found %s at bus %d dev/fn %x", adapter->name, bus, devfn);
	    SP_Unlock(&scsiLock);
	    return(VMK_OK);
	 }
      }
   }
   Warning("Not found: adapter at bus %d dev/fn %d ", bus, devfn);
   SP_Unlock(&scsiLock);
   return(VMK_NOT_FOUND);
}

/*
 *-----------------------------------------------------------------------------
 *
 *  SCSI_GetAdapterList --
 *
 *      Syscall; return a list of adapters and their info. 
 *
 * Results:
 *      VMK_OK - adapter list is in result->list. 
 *
 * Side effects:
 *      None. 
 *
 *-----------------------------------------------------------------------------
 */

VMK_ReturnStatus
SCSI_AdapterList(VMnix_AdapterListArgs *args,
                 VMnix_AdapterListResult *result)
{
   SCSI_Adapter *adapter;
   int i;
   uint32 count = 0;

   SP_Lock(&scsiLock);
   for (i = 0; i < HASH_BUCKETS; i++) {
      for (adapter = adapterHashTable[i]; adapter != NULL; adapter = adapter->next) {
	 if (count < args->maxEntries) { 
            strncpy(result->list[count].vmkName, adapter->name, 
                    VMNIX_DEVICE_NAME_LENGTH);  
            result->list[count].qDepth = *adapter->qDepthPtr;
            strncpy(result->list[count].driverName, adapter->driverName, 
                    VMNIX_MODULE_NAME_LENGTH);  
	 }
         count++;
      }
   }
   result->numReturned = (count > args->maxEntries) ? args->maxEntries : count;
   result->numAdapters = count;
   SP_Unlock(&scsiLock);
   return VMK_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 *  SCSI_GetLUNList --
 *
 *      Syscall; return a list of luns (and their partitions) on specified adapter. 
 *
 * Results:
 *      VMK_BUSY - rescan in progress. 
 *      VMK_NOT_FOUND - no such adapter. 
 *      VMK_OK - lun list is in result->list. 
 *
 * Side effects:
 *      None. 
 *
 *-----------------------------------------------------------------------------
 */

VMK_ReturnStatus
SCSI_GetLUNList(VMnix_LUNListArgs *args, 
                VMnix_LUNListResult *result)
{
   SCSI_Adapter *adapter;
   SCSI_Target *target; 
   uint32 count;
   VMK_ReturnStatus status = VMK_OK;
   struct TmpTarget { 
      SCSI_Target *target; 
      struct TmpTarget *next; 
   } *tmpTargetList = NULL, *t;

   SP_Lock(&scsiLock);
   if (rescanInProgress) {
      SP_Unlock(&scsiLock);
      return VMK_BUSY;
   }
   adapter = SCSIFindAdapter(args->adapterName);
   if (!adapter) {
      SP_Unlock(&scsiLock);
      return VMK_NOT_FOUND;
   }
   count = 0;
   /* 
    * This is ugly: we can't call SCSI_GetTargetInfo[Int] when traversing the
    * list because we are holding adapter->lock. So inc refcount on target
    * while creating a temp target list. 
    */
   SP_Lock(&adapter->lock);
   for (target = adapter->targets; target != NULL; target = target->next) {
      target->refCount++;
      t = Mem_Alloc(sizeof(struct TmpTarget));
      ASSERT(t != NULL);
      t->target = target;
      t->next = tmpTargetList;
      tmpTargetList = t;
   }
   SP_Unlock(&adapter->lock);

   for (t = tmpTargetList; t != NULL; t = t->next) {
      if (count < args->maxEntries) {
         status = SCSIDoGetTargetInfoInt(adapter, t->target, &result->list[count], TRUE);
         result->list[count].invalid = (status != VMK_OK); 
         if (status != VMK_OK) {
            Log("target %s:%d:%d info error 0x%x", adapter->name,
                t->target->id, t->target->lun, status);
         }
      }
      count++;
   }
   
   // Free temp list.
   SP_Lock(&adapter->lock);
   while (tmpTargetList) {
      t = tmpTargetList;
      tmpTargetList = t->next;
      t->target->refCount--;
      Mem_Free(t);
   }
   SP_Unlock(&adapter->lock);
   result->numReturned = (count > args->maxEntries) ? args->maxEntries : count;
   result->numLUNs = count;
   SP_Unlock(&scsiLock);
   return status;
}

/*
 *-----------------------------------------------------------------------------
 *
 *  SCSI_GetLUNPaths --
 *
 *      Syscall; return a list of paths to the specified LUN. 
 *
 * Results:
 *      VMK_BUSY - rescan in progress. 
 *      VMK_NOT_FOUND - no such adapter or target/lun. 
 *      VMK_OK - path list is in result->list. 
 *
 * Side effects:
 *      None. 
 *
 *-----------------------------------------------------------------------------
 */

VMK_ReturnStatus
SCSI_GetLUNPaths(VMnix_LUNPathArgs *args, 
                 VMnix_LUNPathResult *result,
                 unsigned long resultLen)
{
   SCSI_Adapter *adapter;
   SCSI_Target *target;
   SCSI_Path *p;
   uint32 count;
   VMK_ReturnStatus status = VMK_OK;

   SP_Lock(&scsiLock);
   if (rescanInProgress) {
      SP_Unlock(&scsiLock);
      return VMK_BUSY;
   }
   adapter = SCSIFindAdapter(args->adapterName);
   if (!adapter) {
      SP_Unlock(&scsiLock);
      return VMK_NOT_FOUND;
   }
   target = SCSI_FindTarget(adapter, args->targetID, args->lun, TRUE);
   if (!target) {
      SP_Unlock(&scsiLock);
      return VMK_NOT_FOUND;
   }
   SP_Lock(&adapter->lock); 
   count = 0;
   result->pathPolicy = target->policy;
   for (p = target->paths; p != NULL; p = p->next) {
      if (count < args->maxEntries) {
         strncpy(result->list[count].adapterName, p->adapter->name,
                 VMNIX_DEVICE_NAME_LENGTH);  // adapter -this- path goes through.
         result->list[count].targetID = p->id; // target -this- path goes to.
         result->list[count].lun = p->lun; // lun # seen on -this- path.
         result->list[count].state = p->state; 
         result->list[count].active = (p == target->activePath); 
         result->list[count].preferred = (p == target->preferredPath); 
      }
      count++;
   }
   SP_Unlock(&adapter->lock); 
   SCSI_ReleaseTarget(target, TRUE);
   result->numReturned = (count > args->maxEntries) ? args->maxEntries : count;
   result->numPaths = count;
   SP_Unlock(&scsiLock);
   return status;
}

/*
 *-----------------------------------------------------------------------------
 *
 *  SCSI_GetAdapterStats --
 *
 *      Used both as a sysinfo handler and internal call. 
 *
 * Results:
 *       VMK_ReturnStatus. 
 *
 * Side effects:
 *      None. 
 *
 *-----------------------------------------------------------------------------
 */

VMK_ReturnStatus 
SCSI_GetAdapterStats(char *name, 
                     SCSI_Stats *stats,
                     unsigned long resultLen)
{
   VMK_ReturnStatus status;
   SCSI_Adapter *adapter;

   SP_Lock(&scsiLock);

   adapter = SCSIFindAdapter(name);
   if (adapter == NULL) {
      status = VMK_NOT_FOUND;
   } else {
      *stats = adapter->stats;
      status = VMK_OK;
   }

   SP_Unlock(&scsiLock);

   return status;
}

/*
 *-----------------------------------------------------------------------------
 *
 *  SCSI_GetLUNStats --
 *
 *      Sysinfo call to get stats for LUN and its partitions. 
 *
 * Results:
 *      VMK_ReturnStatus. 
 *
 * Side effects:
 *      None. 
 *
 *-----------------------------------------------------------------------------
 */

VMK_ReturnStatus
SCSI_GetLUNStats(VMnix_LUNStatsArgs *args, 
                 VMnix_LUNStatsResult *result,
                 unsigned long resultLen)
{
   SCSI_Adapter *adapter;
   SCSI_Target *target;
   VMK_ReturnStatus status = VMK_OK;
   uint16 j;

   SP_Lock(&scsiLock);
   if (rescanInProgress) {
      SP_Unlock(&scsiLock);
      return VMK_BUSY;
   }
   adapter = SCSIFindAdapter(args->diskName);
   if (!adapter) {
      SP_Unlock(&scsiLock);
      return VMK_NOT_FOUND;
   }
   if (adapter->openInProgress) {
      SP_Unlock(&scsiLock);
      return VMK_BUSY;
   }
   target = SCSI_FindTarget(adapter, args->targetID, args->lun, TRUE);
   if (!target) {
      SP_Unlock(&scsiLock);
      return VMK_NOT_FOUND;
   }
   SP_Lock(&adapter->lock); 
   result->stats = target->stats;
   result->numPartitions = 0;
   for (j = 0; j < target->numPartitions; j++) {
      SCSI_Partition *part = &target->partitionTable[j];
      if (part->entry.numSectors == 0) {
         continue;
      } else if (result->numPartitions >= VMNIX_MAX_PARTITIONS) {
         status = VMK_NO_RESOURCES;
         break;
      } else {
         VMnix_PartitionStats *ps = &result->partitionStats[result->numPartitions];
         ps->number = j;
         ps->stats = part->stats;
         result->numPartitions++;
      }
   }
   SP_Unlock(&adapter->lock); 
   SCSI_ReleaseTarget(target, TRUE);
   SP_Unlock(&scsiLock);
   return status;
}

/* 
 *---------------------------------------------------------------------- 
 * 
 * SCSI_SparseLUNSupport -- 
 * 
 *      Checks if we support sparse luns
 * 
 * Results:  
 *      returns true if we want to check for sparse LUNs
 *
 * 
 * Side effects: 
 *     none
 * 
 *---------------------------------------------------------------------- 
 */ 
Bool
SCSI_SparseLUNSupport(char *name, int devNum)
{
   /* 
    * If a lun mask exists for this name:devNum, override 
    * sparse luns, otherwise, return the config option.
    */
   return(SCSI_IsLunMasked(name, devNum, 0) || 
         (CONFIG_OPTION(DISK_SUPPORT_SPARSE_LUN)));

}

/* 
 *---------------------------------------------------------------------- 
 * 
 * SCSI_GetMaxLUN -- 
 * 
 *      Checks what the current max lun number is
 * 
 * Results:  
 *      returns the currently configure max lun number
 *
 * 
 * Side effects: 
 *     none
 * 
 *---------------------------------------------------------------------- 
 */ 
uint32
SCSI_GetMaxLUN(char *name, int devNum, int hostMaxLun)
{
   /* Check if lun masking is enabled for this name:devNum */
   return(SCSI_IsLunMasked(name, devNum, 0) ? MIN(255, hostMaxLun) :
            MIN(CONFIG_OPTION(DISK_MAX_LUN), hostMaxLun));
}

/* 
 *---------------------------------------------------------------------- 
 * 
 * SCSI_IsLunMasked -- 
 * 
 *      Determine if a particular lun is contained in the list of 
 *      luns to mask.    
 * 
 * Results:  
 *      
 *      If "lun" is non-zero -- Returns TRUE if "lun" is to be masked, 
 *      FALSE otherwise.
 *      
 *      If "lun" is zero -- Returns TRUE if a lun mask exists for this 
 *      name:devNum, FALSE otherwise (note that we never mask lun 0).
 * 
 * Side effects: 
 *     none
 * 
 *---------------------------------------------------------------------- 
 */ 
Bool 
SCSI_IsLunMasked(char *name, int devNum, int lun) 
{
   char *mask; 
   char buf[SCSI_DEV_NAME_LENGTH + SCSI_DEVNUM_MAX_CHAR + 3], *lun_list;
   Bool isMasked;

   /* 
    * This is necessary in the case that we are using the default config
    * value, which is allocated from rodata (Parse_Consolidate_String
    * modifies the mask in place).
    */
   mask = Mem_Alloc(strlen((char *)Config_GetStringOption(CONFIG_DISK_MASK_LUNS)) + 1);
   if (mask == NULL) {
      return(FALSE);
   }
   strcpy(mask, (char *)Config_GetStringOption(CONFIG_DISK_MASK_LUNS));
   
   /* Get rid of the spaces */
   Parse_ConsolidateString(mask);
   /* Compose name and devNum as a -string- of format name:devNum */
   snprintf(buf, SCSI_DEV_NAME_LENGTH + SCSI_DEVNUM_MAX_CHAR + 2, "%s:%d:", name, devNum);

   /* Use it to find the correct lun list.  Check for the 
    * wilecard character in its possible positions (Brute force). 
    * The order ensures that first we select the mask that mataches
    * exactly the name:devNum regardless of any wildcards, then we
    * select on name:*, then *:devNum, then *:*
    */
   if((lun_list = simple_strstr(mask, buf)) == NULL) {
      snprintf(buf, SCSI_DEV_NAME_LENGTH + 1 + 2, "%s:%s:", name, "*");
      if((lun_list = simple_strstr(mask, buf)) == NULL) {
         snprintf(buf, 1 + SCSI_DEVNUM_MAX_CHAR + 2, "%s:%d:", "*", devNum);
         if((lun_list = simple_strstr(mask, buf)) == NULL) {
            if((lun_list = simple_strstr(mask, "*:*:")) == NULL) {
               Mem_Free(mask);
               return(FALSE);
            }
         }
      }
   }

   /* If being called on lun 0, this means we only want to know if the
    * name:devNum has a mask or now
    */
   if (!lun) {
      Mem_Free(mask);
      return(TRUE);
   }
   
   /* Get to the list portion itself */
   lun_list = simple_strstr(lun_list, ":") + sizeof(char);
   lun_list = simple_strstr(lun_list, ":") + sizeof(char);

   isMasked = Parse_RangeList(lun_list, lun);
   Mem_Free(mask);
   return(isMasked);
}

/* 
 *---------------------------------------------------------------------- 
 * 
 * SCSI_UseDeviceReset -- 
 * 
 *      Check if SCSI device reset should be used (rather than SCSI bus
 *	reset) to reset an individual SCSI device.
 * 
 * Results:  
 *      returns TRUE if device reset should be used.
 *
 * 
 * Side effects: 
 *     none
 * 
 *---------------------------------------------------------------------- 
 */ 
Bool
SCSI_UseDeviceReset()
{
   return (CONFIG_OPTION(DISK_USE_DEVICE_RESET));
}

/* 
 *---------------------------------------------------------------------- 
 * 
 * SCSI_UseLunReset -- 
 * 
 *      Check if SCSI LUN reset should be used (rather than SCSI device 
 *      or bus reset) to reset an individual SCSI device. It should
 *      be noted that this option overrides the "UseDeviceReset" option.
 * 
 * Results:  
 *      returns TRUE if LUN reset should be used.
 *
 * 
 * Side effects: 
 *     none
 * 
 *---------------------------------------------------------------------- 
 */ 
Bool
SCSI_UseLunReset()
{
   return (CONFIG_OPTION(DISK_USE_LUN_RESET));
}

/*
 *----------------------------------------------------------------------
 *
 * SCSI_CreateDevice --
 *
 *      Create a SCSI adapter device.
 *
 * Results: 
 *	The newly created SCSI adapter, NULL if it already exists or
 *	we've create the max number of SCSI adapters.
 *
 * Side effects:
 *	A new device is created and added to the hash table.
 *
 *----------------------------------------------------------------------
 */
SCSI_Adapter *
SCSI_CreateDevice(const char *name, void *clientData, int moduleID)
{
   uint32 	index;
   SCSI_Adapter	*adapter = NULL;
   uint32 	nameLength = strlen(name);

   SP_Lock(&scsiLock);

   if (SCSIFindAdapter(name)) {
      SP_Unlock(&scsiLock);
      return NULL;
   }

   ASSERT(nameLength < SCSI_DEV_NAME_LENGTH);
   ASSERT(name[0] != 0);

   index = NameHash(name) % HASH_BUCKETS;

   if (numSCSIAdapters >= MAX_SCSI_ADAPTERS) {
      Warning("Unable to create device - max HBAs already reached.");
      SP_Unlock(&scsiLock);
      return NULL;
   }
   else {
      adapter = (SCSI_Adapter *)Mem_Alloc(sizeof(SCSI_Adapter));
      ASSERT_NOT_IMPLEMENTED(adapter != NULL);
      memset(adapter, 0, sizeof(*adapter));

      adapter->next = adapterHashTable[index];
      adapterHashTable[index] = adapter;

      memcpy(adapter->name, name, nameLength + 1);
      Mod_GetName(moduleID, adapter->driverName);

      adapter->clientData = clientData;
      adapter->moduleID = moduleID;
      adapter->cosCmplBitmapPtr = &scsiCmplBitmaps[numSCSIAdapters];
      adapter->configModified = FALSE;
      adapter->pathEvalState = PATH_EVAL_OFF;

      SP_InitLock("adapterLck", &adapter->lock, SP_RANK_ADAPTER);
      numSCSIAdapters++;
   }
   SP_Unlock(&scsiLock);

   ScsiProcAddAdapter(adapter);

   return adapter;
}

/*
 * Return TRUE if two disk ids are equal.
 */
Bool
SCSI_DiskIdsEqual(SCSI_DiskId *id1, SCSI_DiskId *id2)
{
   if (id1->type == id2->type && id1->type != VMWARE_SCSI_ID_UNIQUE &&
       id1->len == id2->len && id1->lun == id2->lun &&
       memcmp(id1->id, id2->id, id1->len) == 0) {
      return TRUE;
   }
   else {
      return FALSE;
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * SCSI_ResolveDiskId --
 *    Given a SCSI disk ID, return the adaptername, targetID, LUN# for
 *    the disk/LUN.
 *
 * Results:
 *    'adapterName', 'targetID', 'lun' contain vmhba information corresponding
 *    to the given disk ID 'id'. In case no such LUN is found
 *    adapterName = "", targetID = 0.
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
void
SCSI_ResolveDiskId(SCSI_DiskId *id, char *adapterName,
                   uint32 *targetID, uint32 *lun)
{
   SCSI_Adapter *adapter;
   SCSI_Target *target;
   int i;

   SP_Lock(&scsiLock);
   for (i = 0; i < HASH_BUCKETS; i++) {
      for (adapter = adapterHashTable[i]; adapter != NULL;
	   adapter = adapter->next) {
         SP_Lock(&adapter->lock);
         for (target = adapter->targets; target != NULL;
              target = target->next) {
            if (SCSI_DiskIdsEqual(id, &target->diskId)) {
               strcpy(adapterName, adapter->name);
               *targetID = target->id;
               *lun = target->lun;
               SP_Unlock(&adapter->lock);
               SP_Unlock(&scsiLock);
               return;
            }
         }
         SP_Unlock(&adapter->lock);
      }
   }  
   SP_Unlock(&scsiLock);
   adapterName[0] = '\0';
   *targetID = 0;
   *lun = 0;
}

/*
 *----------------------------------------------------------------------
 *
 * SCSI_CreateTarget --
 *
 *      Create a target/LUN for a SCSI adapter device or add a new path to
 *	an existing target.
 *
 * Results: 
 *	none
 *
 * Side effects:
 *	A new target device is created and added to the adapter.
 *
 *----------------------------------------------------------------------
 */
void
SCSI_CreateTarget(SCSI_Adapter *adapter, uint32 tid, 
                  uint32 lun, uint8 qdepth, SCSI_DiskId *diskId, Bool isPseudoDevice)
{
   SCSI_Target *target, *ptr;
   // needs to fit a targetID:lun
   char buf[12];
   int i;
   SCSI_Adapter *a;
   SCSI_Target *t;
   VMnix_TargetInfo *targetInfo;
   VMK_ReturnStatus status;
   Bool adapterMatch = FALSE;
 
   SP_Lock(&scsiLock);
   for (i = 0; i < HASH_BUCKETS; i++) {
      for (a = adapterHashTable[i]; a != NULL; a = a->next) {
	 SP_Lock(&a->lock);
	 adapterMatch = strcmp(a->name, adapter->name) == 0; 
	 for (t = a->targets; t != NULL; t = t->next) {
	    if ((t->id == tid) && (t->lun == lun) && adapterMatch) {
	       /*
		* We have a new target with the same id and lun as 
		* an existing target on this adapter. This can occur if the
		* adapter supports multiple SCSI buses and there are targets
		* on different buses with matching id and lun. VMWare does not
		* currently support adapters with multiple SCSI buses. The only
		* exception to this is when an adapter with multiple SCSI buses
		* has a different PCI function number for each bus, and it that
		* case each bus will look like a separate adapter.
		*
		* See PR #28658 for a complete discussion.
		*/
	       Warning("There is more than one target on adapter %s with an id of %d and a LUN of %d. Ignoring target.",
	          adapter->name, tid, lun);

	       SP_Unlock(&a->lock);
	       SP_Unlock(&scsiLock);
	       return;
            }

	    if (SCSI_DiskIdsEqual(&t->diskId, diskId)) {
	       /*
		* We are seeing a disk id that we've seen before, so this
		* is just a different path to the same target.
                */
	       /* XXX We should increase the maxQDepth of the target if we
		* are going to run in round-robin mode. */
	       SCSIAddPath(t, adapter, tid, lun);
	       if (isPseudoDevice) {
	          /*
	           * Have to check for PSEUDO device when adding each path -
	           * 1st path may not have been active and the info to determine
	           * if the target is a pseudo device could not be obtained
	           */
                  t->flags |= SCSI_DEV_PSEUDO_DISK;
               }
	       a->configModified = TRUE;
	       SP_Unlock(&a->lock);
	       SP_Unlock(&scsiLock);
	       return;
	    }
	 }
	 SP_Unlock(&a->lock);
      }
   }
   if (numSCSITargets >= SCSI_MAX_TARGETS) {
      /* Don't allow creation of more than SCSI_MAX_TARGETS targets,
       * since that's the max number of vsd devices that can be mapped
       * (for use by MUI) in module.c. */
      SP_Unlock(&scsiLock);
      return;
   }
   numSCSITargets++;
   SP_Unlock(&scsiLock);

   // Allocate space for the target information
   target = (SCSI_Target *) Mem_Alloc (sizeof(SCSI_Target));
   ASSERT(target);
   memset(target, 0, sizeof(*target));

   target->id = tid;
   target->lun = lun;
   target->adapter = adapter;
   target->maxQDepth = qdepth;
   target->curQDepth = qdepth;
   target->next = NULL;
   target->rescanNext = NULL;

   // add target to the list on the adapter
   SP_Lock(&adapter->lock);
   if (adapter->targets == NULL) {
      adapter->targets = target;
      adapter->numTargets = 1;
   } else {
      for (ptr = adapter->targets; ptr->next != NULL; ptr = ptr->next){
         continue;
      }
      ptr->next = target;
      adapter->numTargets++;
   }
   adapter->configModified = TRUE; 
   
   SCSIAddPath(target, adapter, tid, lun);
   target->activePath = target->preferredPath = target->paths;
   target->policy = SCSI_PATH_FIXED;
   memcpy(&target->diskId, diskId, sizeof(*diskId));

   targetInfo = (VMnix_TargetInfo *)Mem_Alloc(sizeof(VMnix_TargetInfo)); 
   memset(targetInfo, 0, sizeof(VMnix_TargetInfo)); 
   SP_Unlock(&adapter->lock);

   if (isPseudoDevice) {
      target->flags |= SCSI_DEV_PSEUDO_DISK;
   }

   SP_Lock(&scsiLock); 
   status = SCSIDoGetTargetInfo(adapter, tid, lun, targetInfo, TRUE);
   SP_Unlock(&scsiLock); 
   ASSERT(status == VMK_OK);

   /*
    * Determine if the target supports MANUAL SWITCHOVER.
    */
   SCSISetTargetType(target, targetInfo);
   if (target->flags & SCSI_MUST_USE_MRU_POLICY) {
      Log("Setting default path policy to MRU on target %s:%d:%d ",
         target->adapter->name, target->id, target->lun);
      target->policy = SCSI_PATH_MRU;
   } 

   /*
    * At this point it is not certain if the given path to the target is
    * working. It may be the standby half of an MANUAL SWITCHOVER system.
    * SCSIAddPath() has marked the path as ON anyway. It will be changed
    * to STANDBY if I/O fails.
    */
   Mem_Free(targetInfo);

   target->procEntry.read = ScsiProcTargRead;
   target->procEntry.write = ScsiProcTargWrite;
   target->procEntry.parent = &adapter->adapProcEntry;
   target->procEntry.canBlock = FALSE;
   target->procEntry.private = (void *) ((target->id<<16)|(target->lun));

   snprintf(buf, sizeof buf, "%d:%d", target->id, target->lun);
   Proc_Register(&target->procEntry, buf, FALSE);
   // For dedicated devices only
   if ( !(adapter->flags & SCSI_SHARED_DEVICE)) {
      Host_VMnixVMKDev(VMNIX_VMKDEV_DISK, 
                       adapter->name, NULL, NULL,
                       (target->id<<16)|target->lun, 
                       TRUE);
   }
   // Register both shared and dedicated disks with the FS device switch.
   FSDisk_RegisterDevice(adapter->name, target->id, target->lun,
                         target->numBlocks, target->blockSize);
}

/*
 *----------------------------------------------------------------------
 *
 * SCSI_RemoveTarget --
 *
 *      Remove a target/LUN for a SCSI adapter device or a secondary path
 *	to a target.
 *
 * Results: 
 *	TRUE if the target was removed
 *	FALSE otherwise (it was busy)
 *
 * Side effects:
 *	A target device is removed from the adapter.
 *
 *----------------------------------------------------------------------
 */
Bool
SCSI_RemoveTarget(SCSI_Adapter *adapter,
                  uint32 tid,
                  uint32 lun,
                  Bool modUnload)
{ 
   SCSI_Target *target, *ptr;
   SCSI_SchedQElem **pTargetList, *targetList, *sPtr; 

   SP_Lock(&adapter->lock);
   target = SCSI_FindTarget(adapter, tid, lun, FALSE);
   if ((target != NULL) && (target->refCount > 1)) {
      SCSI_ReleaseTarget(target, FALSE);
      SP_Unlock(&adapter->lock);
      return FALSE;
   }

   if (target == NULL) {
      int i;
      SCSI_Adapter *a;
      SCSI_Target *t;

      SP_Unlock(&adapter->lock);
      // Check if there is a secondary path matching this adapter:target:lun.
      // If so, remove it, since the path has now disappeared. 
      SP_Lock(&scsiLock);
      for (i = 0; i < HASH_BUCKETS; i++) {
	 for (a = adapterHashTable[i]; a != NULL; a = a->next) {
	    SP_Lock(&a->lock);
	    for (t = a->targets; t != NULL; t = t->next) {
	       if (SCSITargetHasPath(t, adapter, tid, lun)) {
		  /*
		   * This target contains the path specified by 
		   * adapter/tid/lun. Try to remove the path.
		   */
                  Bool retValue = FALSE;
	          if (SCSIRemovePath(t, adapter, tid, lun)) {
                     a->configModified = TRUE; 
		     retValue = TRUE;
	          } else {
                     Warning("Cannot remove path %s:%d:%d. Target %s:%d:%d is active.",
                        adapter->name, tid, lun, t->adapter->name, t->id, t->lun);
		  }
		  SP_Unlock(&a->lock);
		  SP_Unlock(&scsiLock);
		  return(retValue);
	       }
	    }
	    SP_Unlock(&a->lock);
	 }
      }
      SP_Unlock(&scsiLock);
      /*
       * No matching path. Assume it was already removed.
       */
      return TRUE;
   }
      
   ASSERT(target);
   ASSERT(target->refCount == 1);

   /*
    * There is a race condition between a world exiting and a target being removed:
    * For example:
    *   - a vm is using a file on the target device
    *   - the target device has been physically removed from the system
    *   - the vm is hung, so the user powers down the vm
    *   - the vm drops the refcount on the target, but has not yet called
    *     SCSI_WorldCleanup()
    *   - the user runs vmkfstools -s vmhba
    *   - the target struct cannot be removed before the world gets to remove
    *     its SCSI_SchedQElems     
    */
   if (target->schedQ != NULL) {
      for (sPtr = target->schedQ; sPtr; sPtr = sPtr->next) {
         if (sPtr->worldID != Host_GetWorldID()) {
            Warning("Cannot remove target. World %d has not completely released the device.",
               sPtr->worldID);
            SCSI_ReleaseTarget(target, FALSE);
            SP_Unlock(&adapter->lock);
            return FALSE;
         }
      }
   }

   // Remove the target from the adapter list
   if (adapter->targets == target) {
      adapter->targets = target->next;
   }
   else {
      for (ptr = adapter->targets; ptr->next != NULL; ptr = ptr->next) {
	 if (ptr->next == target) {
	    ptr->next = target->next;
	    break;
	 }
      }
   }

   adapter->configModified = TRUE;
   adapter->numTargets--;
   SCSI_ReleaseTarget(target, FALSE);
   SP_Unlock(&adapter->lock);

   // Remove any SchedQElem for this target from the list on the console world.
   SP_Lock(&target->adapter->lock);
   SP_Lock(&hostWorld->scsiState->targetListLock);
   pTargetList = &(hostWorld->scsiState->targetList);
   while ((targetList = *pTargetList) != NULL) {
      if (targetList->target == target) {
         *pTargetList = targetList->nextInWorld;
         SCSISchedQFree(targetList->target, targetList);
      }
      else {
         pTargetList = &(targetList->nextInWorld);
      }
   }
   SP_Unlock(&hostWorld->scsiState->targetListLock);
   SP_Unlock(&target->adapter->lock);

   Proc_Remove(&target->procEntry);
   // For dedicated devices only
   if ( !(adapter->flags & SCSI_SHARED_DEVICE)) {
      Host_VMnixVMKDev(VMNIX_VMKDEV_DISK, 
                       adapter->name, NULL, NULL,
                       (target->id<<16)|target->lun, 
                       FALSE);
   }
   FSDisk_UnregisterDevice(adapter->name, target->id, target->lun);
   SCSITargetFree(target, modUnload);

   SP_Lock(&scsiLock);
   numSCSITargets--;
   SP_Unlock(&scsiLock);
   return TRUE;
}
/* 
 * ---------------------------------------------------------------------- 
 *
 * SCSI_FindTarget --
 *
 *      Find the target structure given a SCSI adapter device,
 *      targetID, and lun. Currently we just search the list, but
 *      if this turns into a performance problem, we can use a
 *      hash table.
 *
 * Results: 
 *	Pointer to the target structure if found, else NULL
 *
 * Side effects:
 *	Increments reference count.
 *
 *----------------------------------------------------------------------
 */
SCSI_Target *
SCSI_FindTarget(SCSI_Adapter *adapter, uint32 tid, uint32 lun, Bool lock)
{
   SCSI_Target *target;

   if (lock) {
      SP_Lock(&adapter->lock);
   }
   else {
      ASSERT(SP_IsLocked(&adapter->lock));
   }
   for(target = adapter->targets; target != NULL; target = target->next) {
      if ((target->id == tid) && (target->lun == lun)) {
	 target->refCount++;
         break;
      }
   }
   if (lock) {
      SP_Unlock(&adapter->lock);
   }
   return (target);
}

void
SCSI_ReleaseTarget(SCSI_Target *target, Bool lock)
{
   if (lock) {
      SP_Lock(&target->adapter->lock);
   }
   else {
      ASSERT(SP_IsLocked(&target->adapter->lock));
   }
   target->refCount--;
   ASSERT(target->refCount >= 0);
   if (lock) {
      SP_Unlock(&target->adapter->lock);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * SCSI_DestroyDevice --
 *
 *      Destroy a SCSI adapter device.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	This SCSI adapter is freed and removed from the hash table.
 *
 *----------------------------------------------------------------------
 */
void
SCSI_DestroyDevice(SCSI_Adapter *adapter)
{
   uint32 	index;
   SCSI_Adapter *cur, *prev;
   SCSI_Target	*target;
   
   SP_Lock(&scsiLock);

   SP_Lock(&adapter->lock);
   if (adapter->openCount != 0) {
      Warning("Attempt to destroy adapter(%s) while openCount=%d",
              adapter->name, adapter->openCount);
      SP_Unlock(&adapter->lock);
      SP_Unlock(&scsiLock);
      return;
   }

   /*
    * Should never get here if there is a path evaluation underway.
    */
   ASSERT(adapter->pathEvalState == PATH_EVAL_OFF);
   SP_Unlock(&adapter->lock);

   index = NameHash(adapter->name) % HASH_BUCKETS;
   for (cur = adapterHashTable[index], prev = NULL;
        cur != NULL && cur != adapter;
	prev = cur, cur = cur->next) {
   }

   ASSERT(cur != NULL);

   if (prev == NULL) {
      adapterHashTable[index] = adapter->next;
   } else {
      prev->next = adapter->next;
   } 
   ScsiProcRemoveAdapter(adapter);

   numSCSIAdapters--;
   for (target = adapter->targets; target != NULL; target = target->next) {
      numSCSITargets--;
   }
   SP_Unlock(&scsiLock);
   SCSIAdapterFree(adapter);

   SCSI_RescanFSUpcall();
}


/*
 *----------------------------------------------------------------------
 *
 * SCSIAllocHandleTarg --
 *
 *      Allocate a handle for the SCSI device specified by the target and
 *	partition. 
 *
 * Results: 
 *	An id that can be used for future access.
 *
 * Side effects:
 *	A new adapter handle is allocated.
 *
 *----------------------------------------------------------------------
 */
SCSI_Handle *
SCSIAllocHandleTarg(SCSI_Target *target,
		    World_ID worldID,
		    uint32 partition)
{
   SCSI_Adapter *adapter;
   SCSI_Handle *handle;
   int index;

   SP_Lock(&handleArrayLock);
   adapter = target->adapter;
   if (adapter->moduleID != 0) {
      VMK_ReturnStatus status = Mod_IncUseCount(adapter->moduleID);
      if (status != VMK_OK) {
	 Warning("Couldn't increment module count, error %d", status);
	 SP_Unlock(&handleArrayLock);
	 return NULL;
      }
   }

   for (index = nextHandle; index < MAX_SCSI_HANDLES; index++) {
      if (handleArray[index] == NULL) {
	 break;
      }
   }
   if (index == MAX_SCSI_HANDLES) {
      handleGeneration++;
      for (index = 0; index < nextHandle; index++) {
	 if (handleArray[index] == NULL) {
	    break;
	 }
      }

      if (index == nextHandle) {
	 Warning("Out of scsi handles");
	 nextHandle = 0;
	 handle = NULL;
	 goto exit;
      }
   }

   nextHandle = index + 1;
   if (nextHandle == MAX_SCSI_HANDLES) {
      nextHandle = 0;
      handleGeneration++;
   }

   handle = (SCSI_Handle *)Mem_Alloc(sizeof(SCSI_Handle));
   if (handle == NULL) {
      goto exit;
   }
   memset(handle, 0, sizeof(*handle));
   handle->adapter = adapter;
   handle->worldID = worldID;
   handle->partition = partition;
   handle->handleID = handleGeneration * MAX_SCSI_HANDLES + index;

   handle->target = target;
   handle->refCount = 1;
   ASSERT(SP_IsLocked(&scsiLock));
   adapter->openCount++;

   handleArray[index] = handle;

   if (target->partitionTable[partition].handle == NULL) {
      /* Save handle with partition entry to indicate this partition
       * is locked and to allow re-reading of partition table (for
       * partition == 0). */
      target->partitionTable[partition].handle = handle;
   }

exit:
   if (handle == NULL && adapter->moduleID != 0) {
      Mod_DecUseCount(adapter->moduleID);
   }
   SP_Unlock(&handleArrayLock);
   if (handle != NULL) {
      /* Must release handleArrayLock before getting adapter lock. */
      SP_Lock(&adapter->lock);
      target->useCount++;
      SP_Unlock(&adapter->lock);
   }
      
   return handle;
}

/*
 * Allocate a SCSI handle for the specified (targetID, lun, partition).
 */
static SCSI_Handle *
SCSIAllocHandle(SCSI_Adapter *adapter, World_ID worldID, uint32 targetID,
		uint32 lun, uint32 partition)
{
   SCSI_Target *target;
   SCSI_Handle *handle;

   ASSERT(SP_IsLocked(&scsiLock));
   target = SCSI_FindTarget(adapter, targetID, lun, TRUE);
   ASSERT(target);
   handle = SCSIAllocHandleTarg(target, worldID, partition);
   if (handle == NULL) {
      SCSI_ReleaseTarget(target, TRUE);
   }
   return handle;
}

/*
 *----------------------------------------------------------------------
 *
 * SCSI_OpenDevice --
 *
 *      Open the named SCSI adapter:targetID:lun:partition.
 *
 * Results: 
 *	An id that can be used for future access.
 *
 * Side effects:
 *	A new SCSI handle is allocated.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
SCSI_OpenDevice(World_ID worldID,	// IN: ID of world using handle
		const char *name,	// IN: name of the scsi device
		uint32 targetID,	// IN: target disk on the scsi adapter
		uint32 lun,	        // IN: lun of the device
		uint32 partition,	// IN: partition number on target disk
		int flags,
		SCSI_HandleID *handleID)// OUT: a handle to the open device
{
   SCSI_Adapter *adapter;
   VMK_ReturnStatus status;
   SCSI_Handle *handle;
   SCSI_Partition *entry;
   SCSI_Target *target = NULL;
   Bool writable;

   if (targetID > SCSI_MAX_TARGET_ID) {
      return VMK_INVALID_TARGET;
   } else if (partition >= VMNIX_MAX_PARTITIONS) {
      return VMK_INVALID_PARTITION;
   }

   SP_Lock(&scsiLock);
   adapter = SCSIFindAdapter(name);
   if (adapter == NULL) {
      status = VMK_INVALID_ADAPTER;
      Warning("Couldn't find device %s", name);
      goto exit;
   } else if (adapter->openInProgress) {
      status = VMK_BUSY;
      goto exit;
   }

   target = SCSI_FindTarget(adapter, targetID, lun, TRUE);
   if (!target) {
      // No such target
      status = VMK_INVALID_TARGET;
      goto exit;
   }

   /* Whenever opening a SCSI_Handle, reread the partition table on
    * the specified target, in case Linux user changed it
    * underneath us. 
    */
   status = SCSIValidatePartitionTable(adapter, target);

   if (status != VMK_OK) {
      if (status == VMK_RESERVATION_CONFLICT &&
          target->devClass == SCSI_CLASS_DISK &&
          (flags & SCSI_OPEN_PHYSICAL_RESERVE) != 0) {
         /* Do a "lazy" open of the SCSI device, because another
          * host has the disk reserved. */
         //Option cannot be used to open core dump partitions.
         ASSERT((flags & SCSI_OPEN_DUMP) == 0 &&
                target->partitionTable[partition].entry.type !=
                VMK_DUMP_PARTITION_TYPE);
         handle = SCSIAllocHandleTarg(target, worldID, partition);
         if (handle == NULL) {
            status = VMK_NO_RESOURCES;
            goto exit;
         }
         handle->flags = (SCSI_HANDLE_MULTIPLE_WRITERS |
                          SCSI_HANDLE_PHYSICAL_RESERVE);
         // XXX Can we force the mult_writers flag like this? 
         target->partitionTable[partition].nWriters++; 
         target->partitionTable[partition].flags |= SCSI_HANDLE_MULTIPLE_WRITERS;
         *handleID = handle->handleID;
         status = VMK_OK;
         Warning("%s:%d:%d:%d with reservation conflict",
                  adapter->name, targetID, lun, partition);
      }
      goto exit;
   }

   // Validate partition.
   if (partition >= target->numPartitions) {
      status = VMK_INVALID_PARTITION;
      goto exit;
   }

   entry = &target->partitionTable[partition];
   if (partition != 0 && ((target->devClass != SCSI_CLASS_DISK))) {
      status = VMK_INVALID_PARTITION;
      goto exit;
   } else if ((target->devClass == SCSI_CLASS_DISK) &&
              (partition != 0) &&
	      (entry->entry.numSectors == 0)) {
      status = VMK_INVALID_PARTITION;
      goto exit;
   }

   // Check for conflicts.
   writable = TRUE;
   if (flags & SCSI_OPEN_HOST) {
      // Open from host.
      if (partition != 0 && entry->entry.type == VMK_PARTITION_TYPE) {
         status = VMK_INVALID_TYPE;
         goto exit;
      } else if (SCSI_ISEXTENDEDPARTITION(&entry->entry)
                 || SCSITargetConflict(target, partition, flags)) {
         // Host gets to open in read-only mode on conflict.
         writable = FALSE;
      }
   } else {
      // Open from a VM or from a VMKernel component like Dump, FSS, etc.
      if (flags & SCSI_OPEN_DUMP) {
         // Open from core dump code inside the VMKernel
         if (partition == 0 || entry->entry.type != VMK_DUMP_PARTITION_TYPE) {
            status = VMK_INVALID_TYPE;
            goto exit;
         }
      } else if (partition != 0 && entry->entry.type != VMK_PARTITION_TYPE) {
         status = VMK_INVALID_TYPE;
         goto exit;
      }
      if (SCSITargetConflict(target, partition, flags)) {
         // VM or VMKernel can't open for writing if there is any conflict.
         status = VMK_BUSY;
         goto exit;
      }
   }

   // Create handle.
   handle = SCSIAllocHandleTarg(target, worldID, partition);
   if (handle == NULL) {
      status = VMK_NO_RESOURCES;
   } else {
      if (!writable) {
         LOG(0, "hid=0x%x (%s:%d:%d:%d) is read-only", 
                 handle->handleID, name, targetID, lun, partition);
         handle->flags |= SCSI_HANDLE_READONLY;
         entry->flags |= SCSI_HANDLE_READONLY;
         entry->nReaders++; 
      } else { 
         entry->nWriters++; 
      }
      if (flags & SCSI_OPEN_HOST) {
         handle->flags |= SCSI_HANDLE_HOSTOPEN;
      }
      if (flags & SCSI_OPEN_MULTIPLE_WRITERS) {
         handle->flags |= SCSI_HANDLE_MULTIPLE_WRITERS;
         entry->flags |= SCSI_HANDLE_MULTIPLE_WRITERS;
      }
      if (flags & SCSI_OPEN_PHYSICAL_RESERVE) {
         handle->flags |= SCSI_HANDLE_PHYSICAL_RESERVE;
      }
      *handleID = handle->handleID;
   }

exit:
   VMLOG(1, worldID, "%s:%d:%d:%d status=0x%x h=0x%x ra0=%p, ra1=%p",
         name, targetID, lun, partition, status, *handleID, 
         __builtin_return_address(0), __builtin_return_address(1));

   if (status != VMK_OK && target != NULL) {
      SCSI_ReleaseTarget(target, TRUE);
   }
   SP_Unlock(&scsiLock);
   return status;
}

/*
 * Return TRUE if opening specified partition conflicts with something
 * that is already opened on the target.  Conflict happens if opening
 * a partition that is already open, if opening a whole target and a
 * partition of the target is open, or if opening a partition and the
 * whole target is open.
 */
static Bool
SCSITargetConflict(SCSI_Target *target, // IN:
                   uint32 partition,    // IN: must be a valid partition #
                   int flags)           // IN: flags passed to open()
{
   SCSI_Partition *pTable = target->partitionTable;

   ASSERT(SP_IsLocked(&scsiLock));

   LOG(1, "pn=%d, oF=0x%x, nR=%d, nW=%d, pF=0x%x", partition, flags,
           pTable[partition].nReaders, pTable[partition].nWriters,
           pTable[partition].flags); 

   if (target->devClass != SCSI_CLASS_DISK) {
      // Can only open non-disk devices at partition 0.
      if (partition != 0) {
         Warning("opening non-zero partition of non-disk device");
         ASSERT(partition == 0);
      }
      // Allow multiple opens if SCSI passthrough locking is turned off 
      if (CONFIG_OPTION(SCSI_PASSTHROUGH_LOCKING) == 0) {
	 return FALSE;
      } 
   }

   if (pTable[partition].nReaders == 0 
         && pTable[partition].nWriters == 0 ) {

      /* Can't add foll. condition to COS opens (SCSI_OPEN_HOST) because
       * it will cause the COS to lock itself out of writing to the ptable,
       * if it has already opened another partition on the same
       * disk. However, we should still prevent VMs from opening the entire
       * disk (raw disk mode) when partitions on the disk are busy.
       */
      // Opening whole target for VM, when partition has been opened.
      if (partition == 0 && target->useCount > 0 &&
          (flags & SCSI_OPEN_HOST) == 0) {       
         return TRUE;
      }

      /* 
      * Can't add following condition because host always opens the 
      * whole target when using shared SCSI; its absence allows 
      * VM-p0/Host-pN & Host-p0/VM-pN opens to succeed. The problem is 
      * determining "noneOfTheHandlesIsHost." (add an "nHostHandles" field.)

      // Opening partition when whole target has been opened.
      openFromHost = flags & SCSI_OPEN_HOST; 
      if (partition != 0 && pTable[0].nWriters > 0  // basic rule 
            && (openFromHost || noneOfTheHandlesIsHost)) {       
         return TRUE;
      }
      */

      // Default
      return FALSE;
   } else {

      // Existing handles are read-only.
      if (pTable[partition].nWriters == 0) {
         ASSERT(pTable[partition].flags & SCSI_HANDLE_READONLY);
         ASSERT(!(pTable[partition].flags & SCSI_HANDLE_MULTIPLE_WRITERS));
         return FALSE;
      }

      // Both requested open and existing handles allow multiple writers. 
      if (pTable[partition].nWriters > 0 
          && (flags & SCSI_OPEN_MULTIPLE_WRITERS) 
          && (pTable[partition].flags & SCSI_HANDLE_MULTIPLE_WRITERS)) {
         return FALSE;
      }

      // Default
      return TRUE;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * SCSI_CloseDevice --
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
VMK_ReturnStatus
SCSI_CloseDevice(World_ID worldID, 
                 SCSI_HandleID handleID)
{   
   VMK_ReturnStatus status = VMK_OK;
   SCSI_Handle *handle;

   SP_Lock(&handleArrayLock);

   handle = handleArray[handleID & SCSI_HANDLE_MASK];
   if (handle == NULL) {
      VmWarn(worldID, "Can't find handle 0x%x", handleID);
      status = VMK_NOT_FOUND;
   } else if ((handle->handleID != handleID || handle->worldID != worldID)) { 
      LOG(0, "handleID (%d ?= %d) worldID (%d ?= %d)",
	      handle->handleID, handleID, handle->worldID, worldID);   
      handle = NULL;
      status = VMK_BAD_PARAM;
   } else {
      handleArray[handleID & SCSI_HANDLE_MASK] = NULL;
   }

   SP_Unlock(&handleArrayLock);

   if (handle != NULL) {
      SCSI_Partition *scsiPartn;

      VMLOG(1, worldID, "%s:%d:%d:%d, handle 0x%x, "
            "refCount %d, ra0=%p, ra1=%p",
	    handle->adapter->name, handle->target->id,
            handle->target->lun, handle->partition, handleID,
            handle->refCount, __builtin_return_address(0),
            __builtin_return_address(1));
      if (handle->pendCom > 0) {
         VmWarn(worldID, "closing handle 0x%x with %d pending cmds", 
                handleID, handle->pendCom);
      }
      SP_Lock(&scsiLock);
      scsiPartn = &handle->target->partitionTable[handle->partition];
      if (handle->flags & SCSI_HANDLE_READONLY) {
         ASSERT(scsiPartn->nReaders > 0);
         if (--scsiPartn->nReaders == 0) {
            scsiPartn->flags &= ~SCSI_HANDLE_READONLY;
         }
      } else {
         ASSERT(scsiPartn->nWriters > 0);
         if (--scsiPartn->nWriters == 0) {
            scsiPartn->flags &= ~SCSI_HANDLE_MULTIPLE_WRITERS;
         }
      }
      SP_Unlock(&scsiLock);
      SCSI_HandleRelease(handle);
   }
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * SCSI_ExecuteHostCommand --
 *
 *      Entry point for SCSI commands from the Service Console.
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
SCSI_ExecuteHostCommand(SCSI_HandleID handleID,
                       SCSI_Command *cmd, 
                       VMK_ReturnStatus *result)
{
   SCSIExecuteCommandInt(handleID, cmd, result, 
                         ASYNC_HOST_INTERRUPT | ASYNC_ENQUEUE);
}

/*
 *----------------------------------------------------------------------
 *
 * SCSIExecuteCommandInt --
 *
 *      Demultiplex SCSI commands. 
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
SCSIExecuteCommandInt(SCSI_HandleID handleID,
                      SCSI_Command *cmd, 
                      VMK_ReturnStatus *result,
                      uint32 flags)
{
   SCSI_Handle *handle;

   handle = SCSI_HandleFind(handleID);
   if (handle == NULL) {
      Warning("Couldn't find handle 0x%x", handleID);
      *result = VMK_INVALID_HANDLE;
      return;
   }

   if (handle->flags & SCSI_HANDLE_CLOSING) {
      SCSI_HandleRelease(handle);
      *result = VMK_INVALID_HANDLE;
      Warning("SCSI command on closing handle 0x%x", handleID);
      return;
   }

   if ((handle->flags & SCSI_HANDLE_READONLY)
       && (cmd->cdb[0] == SCSI_CMD_WRITE6 
           || cmd->cdb[0] == SCSI_CMD_WRITE10)) {
       SCSI_HandleRelease(handle);
       *result = VMK_READ_ONLY;
       Warning("Write cmd; read-only handle 0x%x.", handleID);
       return;
   }

   if (SCSIWillClobberActivePTable(handle, cmd)) {
       SCSI_HandleRelease(handle);
       *result = VMK_READ_ONLY;
       Warning("Can't clobber active ptable for LUN %s:%d:%d",
               handle->adapter->name,
               handle->target->id, handle->target->lun);
       return;
   }

   // We need to save away the original serial number together with the
   // handleID, since this pair is globally unique (to be used for abort
   // and reset handling). It will also help us to only clean up commands
   // for this world when getting a reset.
   cmd->originHandleID = handle->handleID;
   cmd->originSN = cmd->serialNumber;

   switch (cmd->type) {
   case SCSI_QUEUE_COMMAND:
      SCSIQueueCommand(handle, cmd, result, flags);
      break;
   case SCSI_ABORT_COMMAND:
      SCSIAbortCommand(handle, handle->worldID, cmd, result);
      break;
   case SCSI_RESET_COMMAND:
      SCSIResetCommand(handle, handle->worldID, cmd, result);
      break;
   default:
      Warning("Invalid SCSI cmd type (0x%x) from %s", cmd->type, "COS");    
      ASSERT(FALSE);
   }
   if (*result != VMK_OK) {
      Warning("SCSI command failed on handle 0x%x with result %#x", handleID, *result);
   }
   SCSI_HandleRelease(handle);
}

/*
 *----------------------------------------------------------------------
 *
 * SCSI_AbortCommand --
 *    External interface to SCSIAbortCommand.
 *
 * Results: 
 *    Result of SCSIAbortCommand.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
SCSI_AbortCommand(SCSI_HandleID handleID,
		  SCSI_Command *cmd)
{
   VMK_ReturnStatus status;
   SCSI_Handle *handle = SCSI_HandleFind(handleID);

   if (handle == NULL) {
      return VMK_INVALID_HANDLE;
   }

   SCSIAbortCommand(handle, handle->worldID, cmd, &status);
   SCSI_HandleRelease(handle);

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * SCSIAbortCommand --
 *
 *      Abort a SCSI command.
 *      The vmkernel queue will be searched to handle the abort at this 
 *      level itself if possible.
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
SCSIAbortCommand(SCSI_Handle *handle,
		 World_ID worldID,
		 SCSI_Command *cmd,
		 VMK_ReturnStatus *result)
{
   SCSI_Adapter *adapter = handle->adapter;
   SCSI_ResultID rid;
   SCSI_QElem *elem;
   SCSI_Target *target = handle->target;
   Bool finished = FALSE;
   SCSI_Path *path, *initPath;
   VMK_ReturnStatus abortStatus;
   uint16 pathActiveCount = 0;

   LOG(0, "handle 0x%x sno %u", handle->handleID, cmd->serialNumber);

   *result = VMK_ABORT_NOT_RUNNING; // So far we have'nt aborted anything

   /* Here are the issues involved in aborting a command:
      o One command issued from a guest may be split into any number
        of smaller commands by the FS code and SCSISplitSGCommand
      o Any of these commands may reside on the vmkernel queue or it may
        have been issued to an adapter
      o Since we have multipathing in the vmkernel, we need to look at
        all possible paths to a target.
      o We could have failovers happen during the abort, which means that an
        aborting command could potentially be reissued if we do not avoid it
      
      To deal with the first issue, we keep an originating serial number for
      all commands, which will be inherited whenever we split up commands in
      filecode and SCSISplitSGCommand etc.
      
      To deal with the second and third issue, we send aborts down those
      paths with  an "active" count of greater than 0. 

      For the fourth issue, we will have a list for each adapter that will
      tell us what commands we are aborting. The failover code will check
      against these before ever failing over a command.

      With this in mind, here is the scheme for aborting a command:
      1) Put the serial number for an aborting command on an abort list and
         keep the failover code from retrying commands on this list
      2) Go through the queue and delete ANY command that matches originSN
      3) Always pass the abort down to the lower level, since some of the
         commands could have been split and only one or more actually queued
         to the adapter. Issue the abort to ALL paths, since we do not know
         where it was issued. The aborts here should either return VMK_ABORT_
         NOT_RUNNING or hopefully VMK_OK (succes). If a matching command was
         not successfully aborted (which means ABORT_PENDING or ABORT_SUCCESS),
         the abort has to be marked unsuccessful to the guest/COS.
      3) ONLY return VMK_SCSI_ABORT_NOT_RUNNING if no commands were aborted
         from point 2 or 3 above!!!
      4) Remove the serial numbers inserted in point 1 above
   */

   // 1) Add this command to the abort/reset list for each adapter on path

   // 2) Delete all entries (fragments / command) from queue
   SP_Lock(&adapter->lock);
   
   while ((elem = SCSIDetachQElem(handle, worldID, cmd, FALSE)) && !finished) {
      SP_Unlock(&adapter->lock);
      // If it was the originating command there is nothing more to do...
      // NB: We already know worldID match, so only look at serialNumber
      if (elem->cmd->serialNumber == cmd->serialNumber) {
         finished = TRUE;
      }
      
      // return this command as aborted
      SCSIInitResultID(handle, elem->token, &rid);
      rid.serialNumber = cmd->serialNumber;
      SCSI_DoCommandComplete(&rid, SCSI_HOST_ABORT << 16, zeroSenseBuffer, 0, 0);
      
      // update command stats
      rid.cmd = cmd;
      rid.path = handle->target->activePath;
      SCSI_UpdateCmdStats(cmd, &rid, worldID);
      
      /*
       * For a queued command, a token is allocated in SCSIQueueCommand 
       * and the handle ref'd in the same function. Also, SCSIIssueCommand
       * would have copied the original command. We must undo all these when
       * aborting such a command.
       */
      Async_ReleaseToken(elem->token);
      SCSI_HandleRelease(elem->handle); 
      Mem_Free(elem->cmd);
      SCSIQElemFree(elem);

      *result = VMK_OK; // At least we have success on this level
      SP_Lock(&adapter->lock);
   }

   SP_Unlock(&adapter->lock);
   
   // 3) Pass abort down to all paths unless we know we are done...  
   SP_Lock(&adapter->lock);

   if (!finished) {
      // send abort down to all possible paths
      initPath = target->activePath;
      path = initPath;
      do {
         SCSIInitResultID(handle, NULL, &rid);
         rid.cmd = cmd;
         rid.path = path;
   
         // the active field is protected by the lock of the target's primary adapter
         // a target can have paths on multiple adapters. 
	 pathActiveCount = path->active;

         SP_Unlock(&adapter->lock);

	 if ((path == initPath) || (pathActiveCount > 0)) {
            abortStatus = path->adapter->command(path->adapter->clientData,
                    cmd, &rid, handle->worldID);
	 } else {
            abortStatus = VMK_OK;
         }

         SP_Lock(&adapter->lock);

         // Inherit status if this is the first time we get one...
         if (*result == VMK_ABORT_NOT_RUNNING) {
            *result = abortStatus;
         }

         // However, in case of errors we will owerwrite any previous status
         // This means that we are not able to complete the abort, but we
         // try to finish up anyway
         if (abortStatus == VMK_FAILURE) {
            *result = VMK_FAILURE;
         }
         path = path->next;
         if (path == NULL) {
            path = target->paths;
         }
      } while (path != initPath);
   }
   SP_Unlock(&adapter->lock);
   
   // 4) Remove this command from the abort/reset lists

}


/*
 *----------------------------------------------------------------------
 *
 * SCSIAbortTimedOutCommand --
 *
 *      Issue an abort to terminate a command that has timed out.
 *      If the cmd is still running and the abort command cannot be
 *      issued, delay and then try the abort again. This routine does
 *      not return until the command has been successfully aborted.
 *      If it returns sooner, then the pending command may complete
 *      after the SCSI cmd structure has been freed or reused, causing
 *      memory corruption.
 *
 * Results: 
 *	VMK_OK or VMK_ABORT_NOT_RUNNING
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
SCSIAbortTimedOutCommand(SCSI_Handle *handle, Async_Token *token, char *deviceName) 
{
   SCSI_Command cmd;
   Bool aborted = FALSE;
   int abortTry = 1;
   VMK_ReturnStatus status;

   while (!aborted) {
      memset(&cmd, 0, sizeof(cmd));
      cmd.type = SCSI_ABORT_COMMAND;
      cmd.serialNumber = token->originSN;
      cmd.originSN = token->originSN;
      cmd.originHandleID = token->originHandleID;

      Warning("%s Abort cmd due to timeout, s/n=%d, attempt %d",
         deviceName, cmd.serialNumber, abortTry);
     
      SCSIAbortCommand(handle, token->resID, &cmd, &status);
      if ((status != VMK_OK) && (status != VMK_ABORT_NOT_RUNNING)) {
         Warning("%s Abort cmd on timeout failed, s/n=%d, attempt %d", 
	    deviceName, cmd.serialNumber, abortTry);
	 abortTry++;
         CpuSched_Sleep(SCSI_BUSY_SLEEP_TIME);
      } else {
         Warning("%s Abort cmd on timeout succeeded, s/n=%d, attempt %d",
            deviceName, cmd.serialNumber, abortTry);
         aborted = TRUE;
      }
   }
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * SCSI_ResetCommand --
 *    External interface to SCSIResetCommand.
 *
 * Results: 
 *    Result of SCSIResetCommand.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
SCSI_ResetCommand(SCSI_HandleID handleID,
		  SCSI_Command *cmd)
{
   VMK_ReturnStatus status;
   SCSI_Handle *handle = SCSI_HandleFind(handleID);

   if (handle == NULL) {
      return VMK_INVALID_HANDLE;
   }

   SCSIResetCommand(handle, handle->worldID, cmd, &status);
   SCSI_HandleRelease(handle);

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * SCSIResetCommand --
 *
 *      Handle a SCSI reset. 
 *      The vmkernel queue will be searched and all commands completed
 *      with reset status before we pass the reset to linux_scsi.
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
SCSIResetCommand(SCSI_Handle *handle, World_ID worldID,
		 SCSI_Command *cmd,
		 VMK_ReturnStatus *result)
{
   SCSI_Adapter *adapter = handle->adapter;
   SCSI_ResultID rid;
   SCSI_QElem *elem;
   SCSI_Target *target = handle->target;
   SCSI_Path *path, *initPath;
   VMK_ReturnStatus resetStatus = VMK_OK;
   uint16 pathActiveCount = 0;

   *result = VMK_OK;

   // 1) Add this command to the abort/reset list for each adapter on path

   // 2) Delete all entries (fragments / command) from queue
   SP_Lock(&adapter->lock);
   
   while ((elem = SCSIDetachQElem(handle, worldID, cmd, TRUE))) {
      SP_Unlock(&adapter->lock);
      
      // return this command as reset
      SCSIInitResultID(handle, elem->token, &rid);
      rid.serialNumber = elem->cmd->serialNumber;
      SCSI_DoCommandComplete(&rid, SCSI_HOST_RESET << 16, zeroSenseBuffer, 0, 0);
      
      // update command stats
      rid.cmd = cmd;
      rid.path = handle->target->activePath;
      SCSI_UpdateCmdStats(cmd, &rid, worldID);
      
      /*
       * For a queued command, a token is allocated in SCSIQueueCommand 
       * and the handle ref'd in the same function. Also, SCSIIssueCommand
       * would have copied the original command. We must undo all these when
       * resetting such a command.
       */
      Async_ReleaseToken(elem->token);
      SCSI_HandleRelease(elem->handle); 
      Mem_Free(elem->cmd);
      SCSIQElemFree(elem);
      SP_Lock(&adapter->lock);
   }

   SP_Unlock(&adapter->lock);
   
   // 3) Pass reset down to all paths unless we know we are done... 
   if (SCSI_UseLunReset()) {
      cmd->flags |= SCSI_CMD_USE_LUNRESET;
   }

   SP_Lock(&adapter->lock);

   // send reset down to all possible paths
   initPath = target->activePath;
   path = initPath;
   do {
      // Issue the reset for this path
      SCSIInitResultID(handle, NULL, &rid);
      rid.cmd = cmd;
      rid.path = path;
      
      // the active field is protected by the lock of the target's primary adapter
      // a target can have paths on multiple adapters. 
      pathActiveCount = path->active;

      SP_Unlock(&adapter->lock);

      if ( (path == initPath) || (pathActiveCount > 0)) {
         resetStatus = path->adapter->command(path->adapter->clientData,
            cmd, &rid, handle->worldID);
      } else {
         resetStatus = VMK_OK;
      }
      
      SP_Lock(&adapter->lock);
      if (resetStatus == VMK_FAILURE) {
         *result = VMK_FAILURE;
      }
      path = path->next;
      if (path == NULL) {
         path = target->paths;
      }
   } while (path != initPath);

   SP_Unlock(&adapter->lock);

   // 4) Remove this command from the abort/reset lists

}

/*
 *----------------------------------------------------------------------
 *
 * SCSIQueueCommand --
 *
 *      Send a command to the adapter. The command may be queued in a  
 *      vmkernel scheduling queue if the adapter is busy.
 *      Note on tokens: A token is allocated in this function for each command.
 *      This token should be released AFTER successfully issuing the command 
 *      (VMK_OK, not VMK_WOULD_BLOCK), or AFTER SCSI_DoCommandComplete() on an 
 *      early return.  If the command was issued, the device layer will hold
 *      another ref count on the token, so the token won't actually be freed.
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
SCSIQueueCommand(SCSI_Handle *handle,
		 SCSI_Command *cmd,
		 VMK_ReturnStatus *result, 
		 uint32 flags)
{
   Async_Token *token;   
   SCSI_Adapter *adapter;
   SCSI_Target *target;
   SCSI_ResultID rid;
   SCSI_Status scsiStatus;
   Bool cmdIsPAEOK;
   SCSISenseData senseBuffer; 
   SCSI_Command *nCmd;
   uint32 size;

   size = sizeof(SCSI_Command);
   if(cmd->sgArr.length > SG_DEFAULT_LENGTH) {
       size += (cmd->sgArr.length - SG_DEFAULT_LENGTH) * sizeof(SG_Elem);
   }
   nCmd = (SCSI_Command *)Mem_Alloc(size);
   ASSERT(nCmd);
   memcpy(nCmd, cmd, size);
   cmd = nCmd;
   
   /*
    * Increment the refcount on the handle.  This ref count will be released
    * if the command is issued in SCSIIssueCommand(), but will remain if
    * command is queued.
    */
   SCSI_HandleFind(handle->handleID);

   token = Async_AllocToken(flags);
   ASSERT_NOT_IMPLEMENTED(token != NULL);

   token->resID = handle->worldID;
   adapter = handle->adapter;
   target = handle->target;

   SCSIInitResultID(handle, token, &rid);
   rid.serialNumber = cmd->serialNumber;
   token->originSN = cmd->serialNumber;
   token->originSN1 = cmd->serialNumber1;
   token->cmd = cmd;

   // Increment the pending commands count
   SP_Lock(&adapter->lock);
   handle->pendCom++;
   SP_Unlock(&adapter->lock);

   /* Check for a special vendor command from DGC
    * Drop out early after saving the data
    */
   if ((cmd->cdb[0] == DGC_AAS_CMD) && (handle->target->flags & SCSI_DEV_DGC)) {
      
      *result = VMK_OK;
      /* Start the registration process for this target */
      if (SCSIDGCStartRegistration(handle, cmd) != VMK_OK) {
         scsiStatus = SCSI_MAKE_STATUS(SCSI_HOST_ERROR, SDSTAT_GOOD);
         SCSI_DoCommandComplete(&rid, scsiStatus, (uint8 *)&senseBuffer, 0, 0);
         goto exit;
      }
      
      /* make a clean exit from this detour */
      scsiStatus = SCSI_MAKE_STATUS(SCSI_HOST_OK, SDSTAT_GOOD);
      SCSI_DoCommandComplete(&rid, scsiStatus, (uint8 *)&senseBuffer,
            handle->target->vendorDataLen, 0);
      goto exit;
   }
   
   cmdIsPAEOK = SCSIIsCmdPAEOK(adapter, cmd);
   if ((adapter->sgSize == 0) ||                   // block device
       ((cmd->sgArr.length <= adapter->sgSize) &&
        (cmd->dataLength <= adapter->maxXfer) &&   // small command
        cmdIsPAEOK)) {
      *result = SCSIIssueCommand(handle, cmd, &rid);
      if (*result == VMK_WOULD_BLOCK) {
         // Issue command has queued it. The caller need not do anything
         *result = VMK_OK;
      }
   } else {
      *result = SCSISplitSGCommand(handle, cmd, &rid, cmdIsPAEOK);
   }
   return;

exit:
   Async_ReleaseToken(token);
   SCSI_HandleRelease(handle);
   return;
}

/*
 *----------------------------------------------------------------------
 *
 * SCSIIssueCommand --
 *
 *	Called to issue a SCSI command to the underlying adapter.
 *	SCSIIssueCommand() will fill in the path and cmd fields of the
 *	SCSI_ResultID rid.
 *
 * Results: 
 *	VMK_OK if the issue of the command was successful
 *      VMK_WOULD_BLOCK if the command was queued in the vmkernel
 *      status from the lower level for any other error
 *
 * Side effects:
 *	If it would have blocked, then the command is queued rather than issued.
 *
 *---------------------------------------------------------------------- 
 */
VMK_ReturnStatus
SCSIIssueCommand(SCSI_Handle *handle, 
                 SCSI_Command *cmd, 
                 SCSI_ResultID *rid)
{
   Async_Token *token;   
   SCSI_Adapter *adapter;
   SCSI_Target *target;
   VMK_ReturnStatus status;
   Bool qEmpty = TRUE;
   Bool cmdSentToDriver = FALSE;
   Bool asyncCantBlock = FALSE;
   adapter = handle->adapter;
   target = handle->target;
   token = rid->token;

   rid->cmd = cmd;

   //
   // This code path is being called from a thread where it is 
   // not safe to block. Swap is probably taking place.
   if ((token) && (token->flags & ASYNC_CANT_BLOCK)) {
      asyncCantBlock = TRUE;
   }

   status = VMK_OK;
   SP_Lock(&adapter->lock);

   /*
    * Don't issue the command if there are already commands in the queue
    * or the system is in the midst of a path failover,
    * unless the cmd specifies SCSI_CMD_BYPASSES_QUEUE.
    */
   if (((target->qcount != 0) || 
        (SCSIDelayCmdsCount(target) > 0)) &&
       !(cmd->flags & SCSI_CMD_BYPASSES_QUEUE)) {
      SCSI_SchedQElem *sPtr; 
     
      // Make sure SCSI_SchedQElem is allocated, so SCSIQElemAlloc()
      // call below will not fail.
      sPtr = SCSISchedQFind(target,  rid->token->resID);
      if (!sPtr) {
         sPtr = SCSISchedQAlloc(target, rid->token->resID) ;
      } 
      SP_Unlock(&adapter->lock);
      ASSERT(sPtr);
      ASSERT(sPtr->worldID == rid->token->resID);
      // This request must be queued as it cannot be allowed
      // to precede previous requests from the same VM ??
      // the current test is stronger than necessary, 
      // it currently covers requests from any vm
      status = VMK_WOULD_BLOCK;
      qEmpty = FALSE;
   } else {
      status = SCSISchedIssued(adapter, target, handle, cmd, rid);
      SP_Unlock(&adapter->lock);
      if (status != VMK_WOULD_BLOCK) {
	 if (rid->path == NULL) {
	    /*
             * If rid->path is not NULL, then the TEST_UNIT_READY command
             * is being sent down a specific path to a multipath setup.
             */
	    SCSIChoosePath(handle, rid);
	    /*
             * Do not clear ASYNC_CANT_BLOCK here. It may be needed
             * in SCSI_DoCommandComplete().
             */ 
         } 
         SP_Lock(&adapter->lock);
         /*
          * The active field is protected by the lock of the target's
          * primary adapter.
          */
         rid->path->active++;
         SP_Unlock(&adapter->lock); 

         ASSERT(cmd->originSN != 0);
         LOG(10,"ScsiIssueCommand - %s:%d:%d",rid->path->adapter->name, rid->path->id, rid->path->lun);
         // We can get VMK_WOULD_BLOCK here, since the driver could be
         // blocked, the error handler could be running, etc. If so, just
         // queue up the command as usual.
         status = adapter->command(rid->path->adapter->clientData, cmd,
                                   rid, handle->worldID);
	 // This check is only a warning because of a race condition.
	 // See PR 31759 for details.
	 if (status != VMK_OK && (cmd->flags & SCSI_CMD_BYPASSES_QUEUE)) {
            Warning("Target %s:%d:%d returns status 0x%x for command marked with Q BYPASS.",
		    rid->path->adapter->name, rid->path->id, rid->path->lun, status);
	 }
         cmdSentToDriver = TRUE;
      }
   }

   ASSERT(token->refCount >= 1);					
   if (status == VMK_OK) {
      Async_ReleaseToken(token);
      SCSI_HandleRelease(handle);
      status = VMK_OK;
   } else if (status == VMK_WOULD_BLOCK) {
      SCSI_QElem *qElem;
      
      if (cmd->flags & SCSI_CMD_LOW_LEVEL) {
	 // If a low-level command would block, just retry it after a
	 // small sleep, since we don't want to use any of the vmkernel
	 // queuing mechanism.
	 ASSERT(cmd->flags & SCSI_CMD_BYPASSES_QUEUE);
	 CpuSched_Sleep(5000);
	 SCSI_DoCommandComplete(rid,
				SCSI_MAKE_STATUS(SCSI_HOST_OK, SDSTAT_BUSY),
				zeroSenseBuffer, 0,
				SCSI_DEC_CMD_PENDING | SCSI_FREE_CMD);
	 Async_ReleaseToken(token);
	 SCSI_HandleRelease(handle);
	 status = VMK_OK;
      } else if (cmd->flags & SCSI_CMD_BYPASSES_QUEUE) {
         //
         // return VMK_WOULD_BLOCK status to the caller,
	 // but do not queue the command
         Warning("Target %s:%d:%d returns WOULD BLOCK status for command marked with Q BYPASS.",
	    rid->path->adapter->name, rid->path->id, rid->path->lun);

	 Async_ReleaseToken(token);
         SP_Lock(&adapter->lock);
	 if (cmdSentToDriver) {
            // this is only called to decrement the rid->path->active count
            SCSISchedDone(adapter, target, rid);
	 }
         SP_Unlock(&adapter->lock);
	 SCSI_HandleRelease(handle);
	 //
	 // delay to let target clear
	 CpuSched_Sleep(5000);
      } else {
	 // Need to queue it for later
	 qElem = SCSIQElemAlloc();
	 ASSERT(qElem);
      
	 // save the cmd, handle, and the token
	 qElem->cmd = cmd;
	 qElem->handle = handle;
	 qElem->token = token;

	 //
	 // If is safe to clear the ASYNC_CANT_BLOCK flag now.
	 // This command is being queued and may be issued
	 // in the context of a thread where it is safe to block./
	 if ((token) && (token->flags & ASYNC_CANT_BLOCK)) {
            token->flags &= ~ASYNC_CANT_BLOCK;
         }
     
	 SP_Lock(&adapter->lock);
	 ASSERT(!(cmd->flags & SCSI_CMD_BYPASSES_QUEUE));
	 SCSIQElemEnqueue(target, qElem, SCSI_QTAIL, SCSI_QREGULAR);
	 SP_Unlock(&adapter->lock);
      }
   } else {
      Warning("return status 0x%x", status);
      Async_ReleaseToken(token);
      SP_Lock(&adapter->lock);
      SCSISchedDone(adapter, target, rid);
      handle->pendCom--;
      SP_Unlock(&adapter->lock);
      SCSI_HandleRelease(handle);
   }

   if (!qEmpty) {
      SCSIExecQueuedCommand(target, TRUE, FALSE, asyncCantBlock);
   }
   return status;
}

#define ASYNC_SPLIT_MAGIC	0x5347
#define ASYNC_SPLIT_FLAG_OK     0
#define ASYNC_SPLIT_FLAG_ERROR  1

/* For commands that need to be split, this structure is stored in the
 * original (parent) command token's callerPrivate area
 */
typedef struct SCSISplitParentInfo {
   uint16 		magic;
   uint16		flags;
   uint32 		serialNumber;
   SCSI_Handle          *handle;
   uint32               needed;
   uint32               handled;
} SCSISplitParentInfo;

typedef struct PAECopySG {
   MA origMA;
   MA paeMA;
   uint32 length;
} PAECopySG;

/* For commands that need to be split, this structure is stored in the
 * new (children) commands token's callerPrivate area
 */
typedef struct SCSISplitChildInfo {
   Async_Token	*token;
   PAECopySG	*paeCopySG;
   uint32	sgLen;
   Bool		paeCopyAfterIO;
   uint16       cIndex;
} SCSISplitChildInfo;

/*
 *----------------------------------------------------------------------
 *
 * ReduceSGArray --
 *
 *      Reduces the size of the given SG array by given number of bytes.
 *
 * Results: 
 *	None
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
ReduceSGArray(SG_Array *sgArr, uint32 bytesToReduce)
{
   int i;
   uint32 bytes = bytesToReduce;

   for (i = sgArr->length - 1; i >= 0 && bytes > 0; i--) {
      if (sgArr->sg[i].length > bytes) {
         sgArr->sg[i].length -= bytes;
         break;
      } else {
         bytes -= sgArr->sg[i].length;
         sgArr->sg[i].length = 0;
         sgArr->length--;
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * SCSIIsCmdPAEOK
 *
 *      Checks to see if the given command's memory regions are valid
 *      to give to the adapter.  Memory above 4GB can't be given to
 *      adapters that don't support PAE.
 *
 * Results: 
 *	TRUE if OK, FALSE otherwise
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static Bool
SCSIIsCmdPAEOK(SCSI_Adapter *adapter, SCSI_Command *cmd)
{
   int i;

   if (IO_FORCE_COPY) {
      return FALSE;
   }

   // machine doesn't have more than 4GB
   if (IsLowMPN(MemMap_GetLastValidMPN())) {
      return TRUE;
   }

   // if adapter supports PAE, the command's memory regions don't matter
   if (SCSIAdapterIsPAECapable(adapter)) {
      return TRUE;
   }

   // if the cmd doesn't transfer any data, no problem.
   if (SCSI_CMD_GET_XFERTYPE(cmd->cdb[0]) == SCSI_XFER_NONE) {
      if (cmd->sgArr.length != 0) {
         LOG(0, "Zeroing out sgarray for cmd(0x%x)", cmd->cdb[0]);
         cmd->sgArr.length = 0;
      }
      return TRUE;
   }

   // check the SG list for high addresses
   ASSERT(cmd->sgArr.addrType == SG_MACH_ADDR);
   for (i = 0; i < cmd->sgArr.length; i++) {
      // if any SG entry uses high memory, the cmd is no good
      if (IsHighMA(cmd->sgArr.sg[i].addr + cmd->sgArr.sg[i].length - 1)) {
         return FALSE;
      }
#if 0
      // also fix 4GB border crossings?
      if ((cmd->sgArr.sg[i].addr >> 32) !=
          ((cmd->sgArr.sg[i].addr + cmd->sgArr.sg[i].length)>> 32)) {
         return FALSE;
      }
#endif
   }

   return TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * SCSICmdUseLowMem
 *
 *      Go through all the command's SG entries and for ones that are
 *      using high memory addresses, allocate a page and switch the
 *      SG address to point to it. 
 *      Copy the data if paeCopyBeforeIO is set.
 *
 * Results: 
 *	TRUE if successful, FALSE otherwise
 *
 * Side effects:
 *	None.
 *
 *---------------------------------------------------------------------- */
static Bool
SCSICmdUseLowMem(SCSI_Command *nCmd, SCSISplitChildInfo *childInfo, Bool paeCopyBeforeIO)
{
   int sgEntry;
   Bool failed = FALSE;

   childInfo->paeCopySG = Mem_Alloc(nCmd->sgArr.length * sizeof(PAECopySG));
   ASSERT_NOT_IMPLEMENTED(childInfo->paeCopySG);

   for (sgEntry = 0; sgEntry < nCmd->sgArr.length; sgEntry++) {
      childInfo->paeCopySG[sgEntry].origMA = nCmd->sgArr.sg[sgEntry].addr;
      childInfo->paeCopySG[sgEntry].length = nCmd->sgArr.sg[sgEntry].length;
      childInfo->paeCopySG[sgEntry].paeMA = 0;

      if (IO_FORCE_COPY || IsHighMA(nCmd->sgArr.sg[sgEntry].addr)) {
         MPN mpn;
         MA maddr;
         uint32 pageOffset;

         // allocate a low page and copy the data for writes
         // for reads the data will be copied by SplitAsyncDone
         ASSERT(nCmd->sgArr.sg[sgEntry].length <= PAGE_SIZE);

         /*
          * Allocate pages from the LOWRESERVED pool, if we fail
          * return failure and callers should deal with this case. 
          * We should see failures only in extremely rare cases, if
          * at all becuase the LOWRESEVED pool is sized to suffice
          * most big/high memory configurations.
          */
         mpn = MemMap_AllocKernelPage(MM_NODE_ANY, MM_COLOR_ANY, MM_TYPE_LOWRESERVED);
         if(mpn == INVALID_MPN) {
            // OK, we failed to allocate memory. Clean up at end...
            failed = TRUE;
            break;
         }

         maddr = MPN_2_MA(mpn);

         /* if the original sg entry's start addr was not page aligned and
          * the entire entry fits in the same page, then use the same
          * unaligned offset for the new address so that the end address
          * has the same alignment. */
         pageOffset = PAGE_OFFSET(nCmd->sgArr.sg[sgEntry].addr);
         if ((pageOffset != 0) &&
             ((pageOffset + nCmd->sgArr.sg[sgEntry].length) <= PAGE_SIZE)) {
            maddr += pageOffset;
         }

         nCmd->sgArr.sg[sgEntry].addr = maddr;
         childInfo->paeCopySG[sgEntry].paeMA = maddr;
         if (paeCopyBeforeIO) {
            if (!Util_CopyMA(childInfo->paeCopySG[sgEntry].paeMA,
                             childInfo->paeCopySG[sgEntry].origMA,
                             nCmd->sgArr.sg[sgEntry].length)) {
               Warning("copy failed");
               // OK, we failed to copy from high to low mem. Clean up at end...
               failed = TRUE;
               break;
            }
         }
      }
   }
   
   if(failed) {
      int i;
      
      // free all the mpns allocated above
      for (i = 0; i <= sgEntry; i++) {
         if (childInfo->paeCopySG[i].paeMA) {
            MemMap_FreeKernelPage(MA_2_MPN(childInfo->paeCopySG[i].paeMA));
         }
      }
      
      Mem_Free(childInfo->paeCopySG);
      return FALSE;
   }

   return TRUE;
}

/*
 *----------------------------------------------------------------------
 *
 * SCSISplitSGCommand --
 *
 *      Called to split a command into N commands because the sg list
 *      is too long for this adapter, or the Xfer size is to large for
 *      a single command, or command uses memory above 4GB and the
 *      adapter doesn't handle it.  The last case requires splitting
 *      because we allocate a page at a time, so each SG entry can be
 *      at most a page.
 *
 * Results: 
 *	VMK_OK always. All other cases are ASSERTs
 *
 * Side effects:
 *      Information is added to the original token to track the original
 *      command and its children.
 *
 *---------------------------------------------------------------------- 
 */
VMK_ReturnStatus
SCSISplitSGCommand(SCSI_Handle *handle, 
                   SCSI_Command *cmd, 
                   SCSI_ResultID *rid, 
                   Bool cmdIsPAEOK)
{
   Async_Token *token;   
   SCSI_Adapter *adapter;
   SCSI_Target *target;
   VMK_ReturnStatus status;
   SCSISplitParentInfo *ac = NULL;
   SCSISplitChildInfo *childInfo;
   uint32 acOffset;
   uint32 sgSrcEntryUsed;
   int sgElemSrc;
   SCSI_ResultID nRid;
   int i = 0;
   uint32 diskOffset = 0;
   uint32 maxEntrySize = -1;
   Bool paeCopyBeforeIO = FALSE, paeCopyAfterIO = FALSE, paeCopyOnly = FALSE;

   /* for commands that need data copying, each SG entry can only be
    * at max one page because we don't have a way to allocate contiguous
    * machine pages.  This is generally not a problem because the
    * PPN->MPN mapping is usually not contiguous anyway. */
   if (!cmdIsPAEOK) {
      maxEntrySize = PAGE_SIZE;
   }

   adapter = handle->adapter;
   target = handle->target;
   token = rid->token;

   /* Non-block devices may use a block size that is not always 512 bytes, so
    * we can't easily set the length of the new commands the split will
    * generate.  */
   if ((target->blockSize == 0) && (cmdIsPAEOK)) {
      Warning("Cannot split request to non-block device");
      status = VMK_NOT_SUPPORTED;
      goto error;
   }
 
   switch (cmd->cdb[0]) {
   case SCSI_CMD_READ10:
   case SCSI_CMD_WRITE10: {      
      SCSIReadWrite10Cmd *oCdb = (SCSIReadWrite10Cmd *)cmd->cdb;
      diskOffset = ByteSwapLong(oCdb->lbn);
      break;
   }
   case SCSI_CMD_READ6:
   case SCSI_CMD_WRITE6: {
      uint8* orw = (uint8*) cmd->cdb;
      diskOffset = (((((uint32)orw[1])&0x1f)<<16)|(((uint32)orw[2])<<8)|orw[3]);
      break;
   }
   default:
      // must be here because cmd is not OK for PAE.
      if (cmdIsPAEOK) {
         Warning("command 0x%x isn't implemented", cmd->cdb[0]);
         status = VMK_NOT_SUPPORTED;
         goto error;
      }
      paeCopyOnly = TRUE;
   }

   // check the data transfer direction to figure out when to copy the data
   if (!cmdIsPAEOK) {
      switch (SCSI_CMD_GET_XFERTYPE(cmd->cdb[0])) {
      case SCSI_XFER_AUTO:
         paeCopyBeforeIO = paeCopyAfterIO = TRUE;
         break;
      case SCSI_XFER_TOHOST:
         paeCopyAfterIO = TRUE;
         break;
      case SCSI_XFER_TODEVICE:
         paeCopyBeforeIO = TRUE;
         break;
      default:
         Panic("SCSISplitSGCommand: non-PAE compliant Cmd(0x%x) has bad xfertype(%d)\n",
               cmd->cdb[0], SCSI_CMD_GET_XFERTYPE(cmd->cdb[0]));
      }
   }

//   for (i = 0; i < cmd->sgArr.length; i++) {
//      Warning("%i (%x,%x)", i, cmd->sgArr.sg[i].addr, cmd->sgArr.sg[i].length);
//   }

   ASSERT(cmd->sgArr.addrType == SG_MACH_ADDR);
   acOffset = token->callerPrivateUsed;
   ASSERT_NOT_IMPLEMENTED(acOffset + sizeof(SCSISplitParentInfo) <= ASYNC_MAX_PRIVATE);
   ac = (SCSISplitParentInfo *)&token->callerPrivate[acOffset];
   token->callerPrivateUsed += sizeof(SCSISplitParentInfo);
   ac->magic = ASYNC_SPLIT_MAGIC;
   ac->serialNumber = cmd->serialNumber;
   ac->handle = handle;
   ac->handled = 0;
   ac->needed = ~(0x0);
   ac->flags = ASYNC_SPLIT_FLAG_OK;

   memcpy(&nRid, rid, sizeof(SCSI_ResultID));

   /* sgSrcEntryUsed contains the # of bytes of the current source SG entry 
    * (sgElemSrc) that have already been processed. */
   sgSrcEntryUsed = 0;
   sgElemSrc = 0;
   for (i = 0; TRUE; i++) {
      uint32 dstCmdLength, sgElemDst, sgRem;
      SCSI_Command *nCmd;
      uint32 size, nblocks;
      Async_Token *nToken;   

      // create a new scsi command structure and do a partial copy
      size = sizeof(SCSI_Command);
      if (adapter->sgSize > SG_DEFAULT_LENGTH) {
         size += (adapter->sgSize - SG_DEFAULT_LENGTH) * sizeof(SG_Elem);
      }
      nCmd = (SCSI_Command *)Mem_Alloc(size);
      ASSERT(nCmd);
      memcpy(nCmd, cmd, sizeof(SCSI_Command));

      // Now modify the SG elements
      dstCmdLength = 0;
      sgElemDst = 0;

      /*
       * misaligned addresses
       *      I encountered a case where the lengths of the initial and last
       *      elements in the list are not multiples of blocksize, though
       *      the total length is. Hence the ugly code for splitting
       */
      // Inner loop checks four conditions
      // Dest SG, Source SG, Xfer size, and maxEntrySize
      while ((sgElemDst < adapter->sgSize) 
             && (sgElemSrc < cmd->sgArr.length)
             && (dstCmdLength < adapter->maxXfer)) {
         nCmd->sgArr.sg[sgElemDst].offset = 0;
         nCmd->sgArr.sg[sgElemDst].addr = 
            cmd->sgArr.sg[sgElemSrc].addr + sgSrcEntryUsed;
         nCmd->sgArr.sg[sgElemDst].length =
            MIN(cmd->sgArr.sg[sgElemSrc].length - sgSrcEntryUsed, maxEntrySize);

         if ((dstCmdLength + nCmd->sgArr.sg[sgElemDst].length) > adapter->maxXfer) {
            nCmd->sgArr.sg[sgElemDst].length = adapter->maxXfer - dstCmdLength;
         }

         dstCmdLength += nCmd->sgArr.sg[sgElemDst].length;
         if ((nCmd->sgArr.sg[sgElemDst].length + sgSrcEntryUsed) ==
             cmd->sgArr.sg[sgElemSrc].length) {
            sgSrcEntryUsed = 0;
            sgElemSrc++;
         } else {
            sgSrcEntryUsed += nCmd->sgArr.sg[sgElemDst].length;
            ASSERT(sgSrcEntryUsed < cmd->sgArr.sg[sgElemSrc].length);
         }
         sgElemDst++;
      }
      nCmd->sgArr.length = sgElemDst;

      if ((target->blockSize == 0) || paeCopyOnly) {
         if (sgElemSrc != cmd->sgArr.length) {
            Warning("Can't split cmd(%d) for class(%d).",
                    cmd->cdb[0], target->devClass);
            ASSERT(i == 0);
            Mem_Free(nCmd);
            status = VMK_IO_ERROR;
            goto error;
         }
         nblocks = 0;
         sgRem = 0;
      } else {
         // Adjust datalength for unaligned SG
         nblocks = dstCmdLength/target->blockSize;
         sgRem = dstCmdLength - (nblocks*target->blockSize);
         
         nCmd->dataLength = dstCmdLength - sgRem;
         nCmd->sectorPos = diskOffset;
      }

      if (sgRem != 0) {
         /* remove/adjust item(s) from the end of the SG list until we have
          * removed sgRem amount of memory buffer space.  Usually only
          * the last item will have to be removed/adjusted.
          */
         ReduceSGArray(&nCmd->sgArr, sgRem);
         sgElemDst = nCmd->sgArr.length;
         if (sgSrcEntryUsed >= sgRem) {
            sgSrcEntryUsed -= sgRem;
         } else {
            sgRem -= sgSrcEntryUsed;
            sgSrcEntryUsed = 0;

            for (sgElemSrc--; sgElemSrc >= 0; sgElemSrc--) {
               if (cmd->sgArr.sg[sgElemSrc].length >= sgRem) {
                  sgSrcEntryUsed = cmd->sgArr.sg[sgElemSrc].length - sgRem;
                  break;
               } else {
                  sgRem -= cmd->sgArr.sg[sgElemSrc].length;
                  sgSrcEntryUsed = 0;
               }
            }
         }
         ASSERT(sgElemSrc >= 0);
         if (((sgElemSrc == 0) && (sgSrcEntryUsed == 0)) || (sgElemDst == 0)) {
            Warning("sgElemSrc=%d,used=%d sgElemDst=%d",
                    sgElemSrc, sgSrcEntryUsed, sgElemDst);
            Mem_Free(nCmd);
            status = VMK_OK; /* Will be changed to VMK_IO_ERROR if i==0 */
            goto error;
         }
      }

      // finalize the split count
      if (sgElemSrc == cmd->sgArr.length) {
         SP_Lock(&token->lock); /* Protect against races with SplitAsyncDone */
         ac->needed = i + 1;
         SP_Unlock(&token->lock);
      }

      if (target->blockSize != 0) {
         // now set the offset and length
         switch (cmd->cdb[0]) {
         case SCSI_CMD_READ10:
         case SCSI_CMD_WRITE10: 
         {      
            SCSIReadWrite10Cmd *nCdb = (SCSIReadWrite10Cmd *)nCmd->cdb;
            nCdb->lbn = ByteSwapLong(diskOffset);
            nCdb->length = ByteSwapShort(nblocks);
            break;
         }
         case SCSI_CMD_READ6:
         case SCSI_CMD_WRITE6: 
         {
            uint8* nrw = (uint8*) nCmd->cdb;
            nrw[1] = ((diskOffset>>16) & 0x1f);
            nrw[2] = ((diskOffset>>8) & 0xff);
            nrw[3] = (diskOffset & 0xff);
            nrw[4] = nblocks;
            break;
         }
         default:
            break;
         }
      }
      diskOffset += nblocks;

      // Create a new token
      nToken = Async_AllocToken(ASYNC_CALLBACK);
      ASSERT_NOT_IMPLEMENTED(nToken != NULL);
      nToken->resID = token->resID;
      // Propagate the ASYNC_CANT_BLOCK flag
      if (token->flags & ASYNC_CANT_BLOCK) {
         nToken->flags |= ASYNC_CANT_BLOCK;
      }
      ASSERT(token->resID == handle->worldID || handle->worldID == Host_GetWorldID());

      nToken->clientData = (void *)acOffset;
      nToken->callerPrivateUsed = sizeof(SCSISplitChildInfo);
      ASSERT_NOT_IMPLEMENTED(nToken->callerPrivateUsed <= ASYNC_MAX_PRIVATE);
      childInfo = (SCSISplitChildInfo *)nToken->callerPrivate;
      childInfo->token = token;
      childInfo->paeCopySG = NULL;
      childInfo->sgLen = nCmd->sgArr.length;
      childInfo->paeCopyAfterIO = paeCopyAfterIO;
      childInfo->cIndex = i;

      // do the data copying for the commands that need it
      if (!cmdIsPAEOK) {
         if (!SCSICmdUseLowMem(nCmd, childInfo, paeCopyBeforeIO)) {
            Warning("Can't allocate low mem for I/O");
            Mem_Free(nCmd);
            Async_ReleaseToken(nToken);
            status = VMK_OK; /* Will be changed to VMK_IO_ERROR if i==0 */
            goto error;
         }
      }

      // reference count on the original
      Async_RefToken(token);   
      nToken->callback = SplitAsyncDone;

      // get new serial number
      SP_Lock(&adapter->lock);      
      nCmd->serialNumber = ++(handle->serialNumber);
      nCmd->originSN = cmd->originSN;
      nCmd->originHandleID = cmd->originHandleID;
      SP_Unlock(&adapter->lock);   

      // update the rid
      nRid.token = nToken;
      nRid.serialNumber = nCmd->serialNumber;
      nToken->cmd = nCmd;

      // finally issue the command
      // increment ref count as issueCmd will call handleRelease
      SCSI_HandleFind(handle->handleID);
      Async_RefToken(nToken);
      status = SCSIIssueCommand(handle, nCmd, &nRid);
      ASSERT ((status == VMK_OK) || (status == VMK_WOULD_BLOCK));

      // All done, quit the loop
      if (sgElemSrc == cmd->sgArr.length) {
         break;
      }
   }

   // The command will not be needed for potentially reissuing.
   // Free it if it's a clone, let the caller free it if it's the original.
   SCSI_HandleRelease(handle);
   return(VMK_OK);

error:
   if (i == 0) {
      /* no child commands have been issued yet, 
       * so error out the original command */
      SP_Lock(&adapter->lock);
      handle->pendCom--;
      SP_Unlock(&adapter->lock);
      Async_ReleaseToken(token);

      if (status == VMK_OK) {
         status = VMK_IO_ERROR;
      }
   } else {
      /* Some commands were already issued - we need to take special care
       * in the case where all commands issued so far have already completed.
       */
      SP_Lock(&token->lock); /* Protect against races with SplitAsyncDone. */
      if (ac->handled == i) {
         /* All children completed, but since ac->handled would never have reached
          * ac->needed, we need to do the final work of SplitAsyncDone here...
          */
         SCSI_ResultID rid;
         SCSI_Result *parentResult = (SCSI_Result *)token->result;
         
         ASSERT(ac->handled != ac->needed);
         SP_Unlock(&token->lock);
         SCSIInitResultID(ac->handle, token, &rid);
         rid.serialNumber = ac->serialNumber;
         /* Complete the command with error - we should still return VMK_OK! */
         LOG(3, "Completing command with error: %u", rid.serialNumber);
         SCSI_DoCommandComplete(&rid,
                                SCSI_MAKE_STATUS(SCSI_HOST_ERROR, SDSTAT_GOOD),
                                parentResult->senseBuffer, 0, 0);
         Async_ReleaseToken(token);
      } else {
         /* Let the last issued child complete the parent with error... */
         ac->flags = ASYNC_SPLIT_FLAG_ERROR;
         ac->needed = i;
         SP_Unlock(&token->lock);
      }
   }
   SCSI_HandleRelease(handle);
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * SplitAsyncDone --
 *
 *      Collect all the children of a split command. If it is the last
 *      then call SCSI_DoCommandComplete() with the original token
 *
 * Results: 
 *	none
 *
 * Side effects:
 *	
 *
 *----------------------------------------------------------------------
 */
static void
SplitAsyncDone(Async_Token *childToken)
{
   int32 tokenOffset;
   SCSISplitChildInfo *childInfo =
      (SCSISplitChildInfo *)childToken->callerPrivate;
   Async_Token *parentToken =  childInfo->token;
   SCSI_Result *childResult = (SCSI_Result *)childToken->result;
   SCSI_Result *parentResult = (SCSI_Result *)parentToken->result;
   SCSISplitParentInfo *ac;      
   SCSI_ResultID rid;
   int i;
   Bool done = FALSE;
   uint32 howmany;

   tokenOffset = (uint32)childToken->clientData;
   ASSERT (tokenOffset >= 0);

   /* We could be racing with SCSISplitSGCommand or other instances of
    * SplitAsyncDone (on another CPU) when accessing the parent token,
    * so grab the parent tokens lock while messing with it.
    */
   ASSERT(parentToken);
   SP_Lock(&parentToken->lock);

   ASSERT(tokenOffset + sizeof(SCSISplitParentInfo) <= ASYNC_MAX_PRIVATE);
   ac = (SCSISplitParentInfo *)&parentToken->callerPrivate[tokenOffset];
   ASSERT(ac->magic == ASYNC_SPLIT_MAGIC);
   ASSERT(ac->handled <= ac->needed);
   /*
    * Save the SCSI_Result data (status & sense buffer) if this is the
    * first command back, and also if this is the first command with a
    * SCSI error.
    */
   howmany = parentResult->bytesXferred;
   if (ac->handled == 0 ||
       (childResult->status != 0 && parentResult->status == 0)) {
      memcpy(parentResult, childResult, sizeof(SCSI_Result));
   }
   /* 
    * Only READ/WRITE cmds are split; if any of the children err, 
    * the parent cmd must be failed, so bytesXferred = 0.
    */
   if (parentResult->status == 0 && childResult->status == 0) {
      parentResult->bytesXferred = howmany + childResult->bytesXferred;
   } else {
      parentResult->bytesXferred = 0; 
   }
   ac->handled++;

   /* if this is a command using low memory for I/O, copy the data for
    * reads, and free the low memory for reads and writes */
   if (childInfo->paeCopySG) {
      for (i = 0; i < childInfo->sgLen; i++) {
         if (childInfo->paeCopySG[i].paeMA) {
            ASSERT(IO_FORCE_COPY || IsHighMA(childInfo->paeCopySG[i].origMA));
            if (childInfo->paeCopyAfterIO) {
               if (!Util_CopyMA(childInfo->paeCopySG[i].origMA,
                                childInfo->paeCopySG[i].paeMA,
                                childInfo->paeCopySG[i].length)) {
                  Warning("copy failed");
               }
            }
            MemMap_FreeKernelPage(MA_2_MPN(childInfo->paeCopySG[i].paeMA));
         }
      }
      Mem_Free(childInfo->paeCopySG);
   }

#ifdef VMX86_DEBUG
   /* Assert here to preserve the state of the token for debugging */
   if (parentToken->refCount < 2) {
      Warning("parentToken: %p, childToken: %p", parentToken, childToken);
      ASSERT_BUG(27389, 0);
   }
#endif

   ASSERT(parentToken->refCount > 1);

   if (ac->handled == ac->needed) {
      done = TRUE;
   }

   /* OK, we are done racing with SCSISplitSGCommand, so drop lock */
   SP_Unlock(&parentToken->lock);

   //the childtoken has two refcounts when we issued it. 
   //One was freed by SCSIIssueCommand.  
   //Another one is created in vmklinux and freed by SCSI_DoCommandComplete and 
   //we finally free the remaining one here.
   Async_ReleaseToken(childToken);
   Async_ReleaseToken(parentToken);

   if (!done) {
      return;
   }
   
   /* This was the last child, so no more races mocking with parent.
    * Now we just need to complete the parent and release its token.
    * Please notice that this causes us to do a recursive call to
    * SCSI_DoCommandComplete. The same is true for FS code...
    */
   parentToken->callerPrivateUsed -= sizeof(SCSISplitParentInfo);

   SCSIInitResultID(ac->handle, parentToken, &rid);
   rid.serialNumber = ac->serialNumber;
   
   if (ac->flags == ASYNC_SPLIT_FLAG_ERROR) {
      LOG(3, "ASYNC_SPLIT_FLAG_ERROR: %u", rid.serialNumber); 
      SCSI_DoCommandComplete(&rid,
                             SCSI_MAKE_STATUS(SCSI_HOST_ERROR, SDSTAT_GOOD),
                             parentResult->senseBuffer, 0, 0);
   } else {
      SCSI_DoCommandComplete(&rid, parentResult->status,
			     parentResult->senseBuffer, 
                             parentResult->bytesXferred, 0);
   }

   Async_ReleaseToken(parentToken);
}

/*
 *----------------------------------------------------------------------
 *
 * SCSIExecQueuedCommand --
 *
 *      Send a previously queued SCSI command to the hardware adapter.
 *      If thisTarget is TRUE, then we only check the specified target.
 *      Else we check all targets, but this target last (for fairness)
 *	Don't send any commands to a target if target->delayCmds > 0,
 *	unless override is set.  If override is set, decrement the
 *	delayCmds counter after executing a command on that target.
 *	If asyncCantBlock is set, then set the ASYNC_CANT_BLOCK flag
 *	in the token structure to prevent SCSIChoosePath() from blocking.
 *	Clear the flag after calling SCSIChoosePath(). If the cmd is
 *	requeued, it may not be necessary to use the ASYNC_CANT_BLOCK flag
 *	the next time this cmd is issued.
 *
 * Results: 
 *	TRUE is queued command could not be issued, FALSE if the command
 *	was issued
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
Bool
SCSIExecQueuedCommand(SCSI_Target *target, Bool thisTarget, Bool override, Bool asyncCantBlock)
{
   Async_Token *token;   
   SCSI_Handle *handle;
   SCSI_ResultID rid;
   VMK_ReturnStatus status;
   SCSI_Command *cmd;
   SCSI_Target *startTarget;
   SCSI_QElem *qElem;
   World_Handle *world;
   SCSI_Adapter *adapter;
   Bool requeued = FALSE;

   adapter = target->adapter;
   // Quick check of queue count without the adapter lock.
   if (adapter->qCount == 0) {
      return requeued;
   }
   startTarget = target;

   // thisTarget must be set if override is set
   ASSERT((!override) || (thisTarget));

   SP_Lock(&adapter->lock);
   while (1) {
      Bool sysServ;
      
      if (!thisTarget) {
         target = target->next;
	 if (target == NULL) {
	    target = adapter->targets;
	 }
      }

      if ( (target->qcount == 0) || 
	   ((SCSIDelayCmdsCount(target) > 0) && (!override))) {
         /* Nothing more queued for this target */
         goto checkDone;
      }

      sysServ = Sched_SysServiceStart(NULL, adapter->intrHandlerVector);

      qElem = SCSIQElemDequeue(target);
      ASSERT(qElem);
      token = qElem->token;
      cmd = qElem->cmd;
      ASSERT(!(cmd->flags & SCSI_CMD_BYPASSES_QUEUE));
      handle = qElem->handle;
      /* Make sure target can't disappear when we release the adapter lock */
      target->refCount++;
      
      /* properly account bottomhalf time to the world that initiated 
       * this command */
      world = World_Find(token->resID);
      if (world) {
         Sched_SysServiceWorld(world);
         World_Release(world);
      }

      SCSIInitResultID(handle, token, &rid);
      rid.serialNumber = cmd->serialNumber;
      status = SCSISchedIssued(adapter, target, handle, cmd, &rid);

      if (status == VMK_WOULD_BLOCK) {
         LOG(1, "%d still blocked", target->id);
         /* Can't issue, put it back on the queue */

	 // If is safe to clear the ASYNC_CANT_BLOCK flag now.
	 // This command is being queued and may be issued
	 // in the context of a thread where it is safe to block./
	 if ((token) && (token->flags & ASYNC_CANT_BLOCK)) {
            token->flags &= ~ASYNC_CANT_BLOCK;
         }
         SCSIQElemEnqueue(target, qElem, SCSI_QHEAD, SCSI_QREGULAR);
	 requeued = TRUE;
      } else {
	 SP_Unlock(&adapter->lock);
	 rid.cmd = cmd;
	 if (asyncCantBlock) {
            token->flags |= ASYNC_CANT_BLOCK;
	 } else {
            token->flags &= ~ASYNC_CANT_BLOCK;
         }
	 SCSIChoosePath(handle, &rid);
	 /*
          * Do not clear ASYNC_CANT_BLOCK here. It may be needed
          * in SCSI_DoCommandComplete().
          */

         SP_Lock(&adapter->lock); 
         /*
          * The active field is protected by the lock of the target's
          * primary adapter.
          */
         rid.path->active++;
         SP_Unlock(&adapter->lock);

         ASSERT(cmd->originSN != 0);
         status = adapter->command(rid.path->adapter->clientData, cmd, &rid, 
				   handle->worldID);
         ASSERT((status == VMK_OK) || (status == VMK_WOULD_BLOCK));

         /* 
	  * Check for VMK_WOULD_BLOCK again - as soon as the adapter->lock
	  * is dropped, the driver could get in a WOULD_BLOCK state,
	  * because of error handling.
          */

         if (status == VMK_WOULD_BLOCK) {
            SP_Lock(&adapter->lock);
	    LOG(0, "Got VMK_WOULD_BLOCK from driver");
            // Release the slot we got from SCSISchedIssued
	    SCSISchedDone(adapter, target, &rid);

	    // If is safe to clear the ASYNC_CANT_BLOCK flag now.
	    // This command is being queued and may be issued
	    // in the context of a thread where it is safe to block./
	    if ((token) && (token->flags & ASYNC_CANT_BLOCK)) {
               token->flags &= ~ASYNC_CANT_BLOCK;
            }
            SCSIQElemEnqueue(target, qElem, SCSI_QHEAD, SCSI_QREGULAR);
            SP_Unlock(&adapter->lock);
	    requeued = TRUE;
         } else {
            ASSERT(token->refCount >= 1);
            Async_ReleaseToken(token);
            SCSIQElemFree(qElem);
            
            if (status != VMK_OK) {
               uint32 scsiStatus;
               LOG(0, "Failed with return status 0x%x", status);
               // have to put a result on the result queue
               scsiStatus = SCSI_MAKE_STATUS(SCSI_HOST_ERROR, SDSTAT_GOOD);
               SCSI_DoCommandComplete(&rid, scsiStatus, zeroSenseBuffer, 0,
                                      SCSI_DEC_CMD_PENDING | SCSI_FREE_CMD);
            }
            SCSI_HandleRelease(handle);
         }
         SP_Lock(&adapter->lock);

      }
      target->refCount--;

      if (sysServ) {
         Sched_SysServiceDone();
      }

checkDone:
      // Will exit if all targets have been checked or 
      // after checking only the specified target if thisTarget == TRUE
      if (target == startTarget) {
	 SP_Unlock(&adapter->lock);
         return requeued;
      }
   }
   return requeued;
}

/*
 *----------------------------------------------------------------------
 *
 * SCSI_CmdCompleteInt --
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
VMK_ReturnStatus
SCSI_CmdCompleteInt(SCSI_HandleID handleID, SCSI_Result *outResult, Bool *more)
{
   Bool found = FALSE;
   SCSI_Handle *handle;

   handle = SCSI_HandleFind(handleID);
   if (handle == NULL) {
      *more = FALSE;
      return VMK_NOT_FOUND;
   }

   ASSERT(handle->handleID == handleID);

   SP_Lock(&handle->adapter->lock);

   if (handle->resultListHead != NULL) {
      SCSI_Result *result;   
      Async_Token *token = handle->resultListHead;

      handle->resultListHead = token->nextForCallee;
      if (handle->resultListHead == NULL) {
	 handle->resultListTail = NULL;
      }

      result = (SCSI_Result *)token->result;
      *outResult = *result;
      ASSERT(result->serialNumber == token->originSN);
      outResult->serialNumber = token->originSN;
      outResult->serialNumber1 = token->originSN1;
      found = TRUE;

      ASSERT(result->type == SCSI_QUEUE_COMMAND);

      Async_ReleaseToken(token);
   }

   *more = handle->resultListHead != NULL;

   SP_Unlock(&handle->adapter->lock);

   SCSI_HandleRelease(handle);

   return (found)? VMK_OK : VMK_NOT_FOUND;
}

/*
 *----------------------------------------------------------------------
 *
 * SCSIPostCmdCompletion --
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
SCSIPostCmdCompletion(SCSI_Handle *handle, Async_Token *token)
{
   ASSERT(SP_IsLocked(&handle->adapter->lock));
   ASSERT(SP_IsLocked(&handleArrayLock));

   /* 
    * A default host cmds, unless overridden by
    * ASYNC_CALLBACK for some reason (e.g., split). 
    * Put the cmd's token on the completed list for this handle.  The list
    * is eventually processed in SCSI_CmdCompleteInt().
    */

   if (token->flags & ASYNC_ENQUEUE) {
      ASSERT(!(token->flags & ASYNC_CALLBACK));
      Async_RefToken(token);	 
      if (handle->resultListHead == NULL) {
	 handle->resultListHead = token;
	 handle->resultListTail = token;
      } else {
	 handle->resultListTail->nextForCallee = token;
	 handle->resultListTail = token;
      }
      token->nextForCallee = NULL;
      handle->pendCom--;
   }      

   /*
    * Interrupt host if the command was queued by the host.
    */
   if (token->flags & ASYNC_HOST_INTERRUPT) {
      ASSERT(!(token->flags & ASYNC_POST_ACTION));
      if (NULL != handle->adapter->cosCmplBitmapPtr) {
	 Atomic_Or((handle->adapter->cosCmplBitmapPtr),
		   1 << VMNIX_TARGET_LUN_HASH(handle->target->id, handle->target->lun));
      }
      Host_InterruptVMnix(VMNIX_SCSI_INTERRUPT);
   }

   Async_IODone(token);
}

/* 
 * Update the RESERVED_LOCAL flag for a target based on the state of all
 * its paths.
 */
static void
SCSIUpdateReservedFlag(SCSI_Target *target)
{
   SCSI_Path *p;

   for (p = target->paths; p; p = p->next) {
      if ((p->flags & SCSI_PATH_RESERVED_LOCAL) != 0) {
	 target->flags |= SCSI_RESERVED_LOCAL;
	 return;
      }
   }
   target->flags &= ~SCSI_RESERVED_LOCAL;
}

/*
 * Update the RESERVED_LOCAL flag for a target and its paths, based on the
 * result of the current SCSI command.
 */
static void
SCSICheckReservedState(SCSI_Target *target, SCSI_Path *path,
		       SCSI_Command *cmd, SCSI_Status status,
		       uint8 *senseBuffer)
{
   SCSI_Path *adapterpath;

   // Get the first path on the target's list that has the same adapter as
   // 'path's adapter, since reservation status is per adapter.
   for (adapterpath = target->paths; adapterpath; adapterpath =
	   adapterpath->next) {
      if (adapterpath->adapter == path->adapter) {
	 break;
      }
   }
   ASSERT(adapterpath != NULL);

   if (!(adapterpath->flags & SCSI_PATH_RESERVED_LOCAL)) {
      if (cmd->cdb[0] == SCSI_CMD_RESERVE_UNIT && status == 0) {
	 /* Remember that a reserve to this target succeeded. */
	 adapterpath->flags |= SCSI_PATH_RESERVED_LOCAL;
	 SCSIUpdateReservedFlag(target);
      }
   } else if (cmd->cdb[0] == SCSI_CMD_RELEASE_UNIT && status == 0) {
      /* Clear the flag indicating that this target is reserved. */
      adapterpath->flags &= ~SCSI_PATH_RESERVED_LOCAL;
      SCSIUpdateReservedFlag(target);
   } else if (SCSIPowerOnOrReset(status, senseBuffer)) {
      /* The reservation on this target was released by a SCSI reset. */
      adapterpath->flags &= ~SCSI_PATH_RESERVED_LOCAL;
      SCSIUpdateReservedFlag(target);
   } else if (SCSI_DEVICE_STATUS(status) == SDSTAT_RESERVATION_CONFLICT) {
      adapterpath->flags &= ~SCSI_PATH_RESERVED_LOCAL;
      SCSIUpdateReservedFlag(target);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * SCSI_DoCommandComplete --
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
SCSI_DoCommandComplete(SCSI_ResultID *rid, 
                       SCSI_Status status, 
                       uint8 *senseBuffer,
                       uint32 bytesXferred,
		       uint32 flags) 
{
   SCSI_Handle *handle;
   SCSI_Target *target = rid->target;
   Async_Token *token = rid->token;   
   SCSI_Adapter *adapter = target->adapter;
   World_Handle *world;
   Bool execQ = FALSE;
   Bool cb = FALSE;
   SCSI_Result *result;
   Bool cmdFailed = FALSE;
   Bool doFailover = FALSE; 
   Bool lowLevelCmd = FALSE;
   Bool asyncCantBlock = FALSE;
   SCSI_Target *queueTarget = NULL;
   uint32 serializerToken;
   uint32 eflags;

   ASSERT((rid->path == NULL) || (rid->path->target->adapter == adapter));

   /* Work done in this BH should be attributed to the world this SCSI
    * command belongs to. Provide the actual world ID later.
    */
   Sched_SysServiceStart(NULL, adapter->intrHandlerVector);

   //
   // This code path is being called from a thread where it is 
   // not safe to block. Swap is probably taking place.
   if ((token) && (token->flags & ASYNC_CANT_BLOCK)) {
      asyncCantBlock = TRUE;
   }
   SP_Lock(&adapter->lock);

   /* The following serialized region is protected by the adapter lock.
    * I have verified that we do not drop this lock and regrab it
    * anywhere, since we have done so in the past. Now that we allow
    * BHs on multiple CPUs we need to make sure that we never introduce
    * it again, we will assert at the end of this function that the RA
    * (from the last SP_Lock call, which is above) is untouched at the
    * end of the serialized region.
    */
   serializerToken = SP_GetLockRA(&adapter->lock);

   if (rid->partition >= target->numPartitions) {
      Warning("accessing partition %d, >= num partitions %d",
	      rid->partition, target->numPartitions);
   }

   /*
    * A LOW LEVEL command has a mock handle and target. Do not try to
    * reference these after SCSIPostCmdCompletion() is called. They
    * are not protected by refCount and will be immediately freed
    * after the call.
    */
   if ((rid->cmd != NULL) &&
       (rid->cmd->flags & SCSI_CMD_LOW_LEVEL)) {
      lowLevelCmd = TRUE;
   }

   if (rid->path != NULL &&
       rid->path->state == SCSI_PATH_DEAD &&
       SCSI_HOST_STATUS(status) != SCSI_HOST_NO_CONNECT &&
       SCSI_HOST_STATUS(status) != SCSI_HOST_BUS_BUSY) {
      /* If we issued a command that didn't
       * return a NO_CONNECT, then mark this path as alive again. */
      SCSIMarkPathUndead(rid->path);
   }
  
   if (rid->path != NULL && rid->cmd != NULL) {
      SCSICheckReservedState(target, rid->path, rid->cmd, status, senseBuffer);
   }

   world = World_Find(token->resID);
   if (!world) {
      // If world doesn't exist any more, then just bail, since disk
      // scheduling code will not work.  The command must have taken a
      // long time to come back, and the world was killed in the meantime.
      // The handle may still exist, even though the world is gone,
      // if the handle is a SCSI handle to a file system used by
      // several worlds.
      goto unlockAndQueue;
   }
   Sched_SysServiceWorld(world);
   World_Release(world);

   /*
    * Ensure that the handle doesn't go away by holding the
    * handleArrayLock.  We do this rather than using SCSI_HandleFind(),
    * because then we might have to close the SCSI handle and/or adapter
    * in SCSI_HandleRelease(), and we can't do that in a bottom half.
    */
   ASSERT(rid->handleID != -1);
   SP_Lock(&handleArrayLock);
   handle = handleArray[rid->handleID & SCSI_HANDLE_MASK];

   SCSIUpdateCmdLatency(target, handle, rid->token);

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

   if (handle == NULL) {
      Warning("No active handle for target %d", target->id);
      SP_Unlock(&handleArrayLock);
      goto unlockAndQueue;
   } else if (handle->handleID != rid->handleID) {
      Warning("Handle IDs don't match %d != %d",
	      rid->handleID, handle->handleID);
      SP_Unlock(&handleArrayLock);
      goto unlockAndQueue;
   }

   result = (SCSI_Result *)token->result;

   if (status != 0) {
      if ((token != NULL) && (token->cmd != NULL) && (!(token->cmd->flags & SCSI_CMD_PRINT_NO_ERRORS))) {
         if ((handle != NULL) && (handle->target != NULL)) {
            SCSI_Path *path = handle->target->activePath;

            if (rid->path != NULL) {
               path = rid->path;
            }

            if ((path != NULL) && (path->adapter != NULL)) { 
               Warning("%s:%d:%d:%d status = %d/%d 0x%x 0x%x 0x%x",
                  path->adapter->name, path->id,
                  path->lun, handle->partition, 
	          SCSI_DEVICE_STATUS(status), SCSI_HOST_STATUS(status),
                  senseBuffer[2], senseBuffer[12], senseBuffer[13]);
            }
         }
      }
   }

   if ((rid->path != NULL)                                 &&
       (!(rid->cmd->flags & SCSI_CMD_IGNORE_FAILURE))      &&
       (!lowLevelCmd)) {
      /*
       * Check for conditions that may require a path failover.
       */
      if (SCSIPathDead(target, status, (SCSISenseData *)senseBuffer)) {
         /*
	  * Mark the current path as dead (if not already marked dead),
	  * and reissue the command on another path.
	  */
	 cmdFailed = TRUE;
	 if (rid->path->state != SCSI_PATH_DEAD) {
	    SCSIMarkPathDead(rid->path);
	    if (!SCSIHasWorkingPath(handle)) { 
               Warning("None of the paths to target %s:%d:%d are working.",
	          rid->path->adapter->name, rid->path->id, rid->path->lun);
	    }
	    doFailover = TRUE;
	 } else if ((SCSIDelayCmdsCount(rid->path->target) == 0)) {
	    doFailover = TRUE;
         }
      } else if (SCSIDeviceNotReady(target, status, (SCSISenseData *)senseBuffer) && 
                 (!SCSIDeviceIgnore(target))) {
	 /*
	  * The NOT_READY condition is returned when an I/O has been
	  * issued to a target that supports MANUAL SWITCHOVER but the
	  * path appears to be in the standby state. Setting the path to
	  * STANDBY and re-issuing the command will cause SCSIChoosePath
	  * to initiate the failover procedure.
	  */ 
	 doFailover = TRUE;
	 SCSIMarkPathStandby(rid->path);
	 cmdFailed = TRUE;
      }
   }

   if (cmdFailed) {
      if (SCSIHasWorkingPath(handle)) { 
         SCSI_QElem *qElem;
      
         /*
          * Requeue the failed request.
          */
         qElem = SCSIQElemAlloc();
         ASSERT(qElem);
         handle->refCount++;
         SP_Unlock(&handleArrayLock);

         Async_RefToken(token);
         qElem->cmd = rid->cmd;
         qElem->handle = handle;
         qElem->token = token;
         if ((flags & SCSI_DEC_CMD_PENDING) != 0) {
            SCSISchedDone(adapter, target, rid);
         }
         /*
          * Since this is a failed I/O, place it on the priority queue
          * so that it will be re-issued before any I/O that hasn't
          * been issued at all yet.
          */
         ASSERT(!(rid->cmd->flags & SCSI_CMD_BYPASSES_QUEUE));

	 // If is safe to clear the ASYNC_CANT_BLOCK flag now.
	 // This command is being queued and may be issued
	 // in the context of a thread where it is safe to block./
	 if ((token) && (token->flags & ASYNC_CANT_BLOCK)) {
            token->flags &= ~ASYNC_CANT_BLOCK;
         }
         SCSIQElemEnqueue(target, qElem, SCSI_QTAIL, SCSI_QPRIORITY);
         if (adapter->qCount) {
            queueTarget = target;
            queueTarget->refCount++;
            execQ = TRUE;
         }
         if (doFailover) {
            /*
             * Possibly use a helper request to re-execute the failed
             * command, since SCSIChoosePath may need to issue some
             * synchronous SCSI commands to perform the actual
             * failover.
             */
            SCSIRequestHelperFailover(target);
         }
         
         /*
          * The failed command has been requeued, so skip past all the
          * code that indicates that the command has completed.
          */
         goto unlockEnd;
      }
   }

   /*
    * The I/O to this path was successful. Under most conditions
    * the path state can be changed to ON.
    * This condition occurs when initially a path returns a
    * check condition, but later responds to an I/O 
    */
   if ((status == 0) &&
       (rid->path != NULL)    &&
       (rid->path->state == SCSI_PATH_STANDBY)) {
      SCSIMarkPathOnIfValid(target, rid);
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
    * SCSIPostCmdCompletion calls IODone.
    */
#ifdef DELAY_TEST
   if (!(rid->cmd->flags & SCSI_CMD_TIMEDOUT))
#endif
      SCSIPostCmdCompletion(handle, token);

   SP_Unlock(&handleArrayLock);

   /* 
    * Callback on cmd completion is requested for split commands and FS reads
    * from a virtual disk.
    */
   if (token->flags & ASYNC_CALLBACK) {
      ASSERT(!(token->flags & ASYNC_ENQUEUE));
      ASSERT(token->callback != NULL);
      Async_RefToken(token);
      cb = TRUE;
   }

   // tell disk scheduling code that this req is done
   if (((flags&SCSI_DEC_CMD_PENDING) != 0) && (!lowLevelCmd)) {
      SCSISchedDone(adapter, target, rid);
   }

unlockAndQueue:
   if (adapter->qCount) {
     if (lowLevelCmd) {
        queueTarget = adapter->targets;
     } else {
        queueTarget = target;
     }
     queueTarget->refCount++;
     execQ = TRUE;
   }

unlockEnd:
   /* Make sure that noone dropped and regrabbed the adapter lock above */
   ASSERT(serializerToken == SP_GetLockRA(&adapter->lock));
   SP_Unlock(&adapter->lock);

   // Tokens which wanted a callback.
   if (cb) {
      token->callback(token);
      Async_ReleaseToken(token);
   }

   SAVE_FLAGS(eflags);
   CLEAR_INTERRUPTS();
   Sched_SysServiceDone();
   RESTORE_FLAGS(eflags);

   // Issue queued command, if needed.
   if (execQ) {
      SCSIExecQueuedCommand(queueTarget, FALSE, FALSE, asyncCantBlock);
      SCSI_ReleaseTarget(queueTarget, TRUE);
   }
}

/*
 *----------------------------------------------------------------------
 *
 *  SCSI_ActiveHandles--
 *
 *      Checks to see if there are any active handles for this world
 *      This function is related to SCSI_WorldCleanup
 *
 * Results: 
 *	TRUE if any handles have pending commands.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
Bool
SCSI_ActiveHandles(World_ID worldID)
{
   int i;
   for (i = 0; i < MAX_SCSI_HANDLES; i++) {
     if (handleArray[i] != NULL && handleArray[i]->worldID == worldID
         && handleArray[i]->pendCom > 0) {
        Warning("handle (%d) %p still in use by monitor (%d) %d commands", 
                i, handleArray[i], handleArray[i]->worldID, handleArray[i]->pendCom);
        handleArray[i]->flags |= SCSI_HANDLE_CLOSING;
        return TRUE;
     }
   }
   return FALSE;
}


/*
 *----------------------------------------------------------------------
 *
 * SCSI_WorldInit --
 *
 *      Creates the /proc entry for the world
 *      Initializes the pointer to the list of target associations
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
SCSI_WorldInit(World_Handle *world, World_InitArgs *args)
{
   ASSERT(world);

   VMLOG(1, world->worldID, "RegisterWorld");

   // Alloc the space for SCSI info in the world
   world->scsiState = (WorldScsiState *)World_Alloc(world, sizeof(WorldScsiState));
   if (!world->scsiState) {
      return VMK_NO_MEMORY;
   }

   SP_InitLock("targetListLock", &world->scsiState->targetListLock, SP_RANK_TARGETLIST);
   world->scsiState->targetList = NULL;

   // "disk" directory
   memset(&world->scsiState->procWorldDiskDir, 0, 
          sizeof(world->scsiState->procWorldDiskDir));
   world->scsiState->procWorldDiskDir.parent = &world->procWorldDir;
   Proc_Register(&world->scsiState->procWorldDiskDir, "disk", TRUE);   

   if (World_IsVMMWorld(world)) {
      World_VMMGroup(world)->scsiCompletionVector = SharedArea_Alloc(world,
                                                                "scsiCompletionVector",
                                                                sizeof(Atomic_uint32) *
                                                                SCSI_MAX_CONTROLLERS);
   }
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * SCSI_WorldCleanup --
 *
 *      Close all handles for this world.
 *      Release the proc entry
 *      Free all associations to targets
 *      Free the world scsi state
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
SCSI_WorldCleanup(World_Handle *world)
{
   int i;
   SCSI_SchedQElem      *targetList;
   World_ID worldID = world->worldID;

   VMLOG(1, worldID, "CleanupWorld");

   for (i = 0; i < MAX_SCSI_HANDLES; i++) {
     if (handleArray[i] != NULL && handleArray[i]->worldID == worldID) {
	SCSI_CloseDevice(worldID, handleArray[i]->handleID);
     }
   }

   // clean up target associations
   while (world->scsiState->targetList) {
      SCSI_Target *target;

      targetList = world->scsiState->targetList;
      world->scsiState->targetList = targetList->nextInWorld;
      target = targetList->target;
      ASSERT(targetList->worldID == world->worldID);

      SP_Lock(&target->adapter->lock);
      SCSISchedQFree(target, targetList);
      SP_Unlock(&target->adapter->lock);
   }

   SP_CleanupLock(&world->scsiState->targetListLock);
   Proc_Remove(&world->scsiState->procWorldDiskDir);
   World_Free(world, world->scsiState);
   world->scsiState = 0;
}

/*
 * SCSITimeout is called if the timer pops on a blocking read or write
 * It prints a warning, generates a timeout status and unblocks the
 * world waiting.
 */
static void
SCSITimeout(void *info, UNUSED_PARAM(Timer_AbsCycles timestamp))
{
   ScsiTimeOut *timeO = (ScsiTimeOut *) info;
   Async_Token *token = timeO->token;

   Warning("%s of handleID 0x%x", timeO->isRead?"READ":"WRITE",
	   timeO->handleID);
 
   Async_IOTimedOut(token);
   Mem_Free(timeO);
}

/*
 *----------------------------------------------------------------------
 *
 * SCSIHandleSyncReservationConflict --
 *
 *      Handle reservation conflicts on synchronous I/Os
 *
 *	Retry the I/O a number of times after a small delay to give the other
 *	initiator a chance to complete its atomic operation. If the device is
 *	still reserved after the last retry, the device is probably reserved for
 *	the long term. Flag it to prevent retries of subsequent synchronous I/Os 
 *	on reservation conflict until a synchronous I/O completes with a
 *	different SCSI status.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	None.
 *----------------------------------------------------------------------
 */
static INLINE void
SCSIHandleSyncReservationConflict(SCSI_HandleID handleID,
			      VMK_ReturnStatus status,
			      int *conflictRetries, uint8 opCode)
{
   SCSI_Handle *handle = SCSI_HandleFind(handleID);

   if (handle == NULL) {
      return;
   }

   if (status == VMK_RESERVATION_CONFLICT) {
      if (handle->target->flags & SCSI_DONT_RETRY_ON_RESERV_CONFLICT) {
	 *conflictRetries = 0;
      } else if (--*conflictRetries) {
	 CpuSched_Sleep(SCSI_CONFLICT_SLEEP_TIME);
      } else {
	 // The device is apparently under long term reservation.
	 // Remember not to retry subsequent synchronous I/Os on
	 // reservation conflict until an I/O completes with a
	 // different SCSI status.
	 LOG(1,"Disabling retries on reservation conflict"
	     " for %u - %u:%u (0x%02x)", handleID,
	     handle->target->id, handle->target->lun, opCode);
	 SP_Lock(&handle->adapter->lock);      
	 handle->target->flags |= SCSI_DONT_RETRY_ON_RESERV_CONFLICT;
	 SP_Unlock(&handle->adapter->lock);
      }   
   } else {
      // The device has been released. We can now resume retries on
      // reservation conflict to deal with short term reservations.
      // NB: detecting whether the device has been released is problematic
      // because the list of commands sensitive to SCSI reservations varies
      // between devices. Reads and Writes are safe bets however, and will
      // occur sooner or later.
      SP_Lock(&handle->adapter->lock);      
      if ((handle->target->flags & SCSI_DONT_RETRY_ON_RESERV_CONFLICT) &&
	  (opCode == SCSI_CMD_READ10 || opCode == SCSI_CMD_WRITE10)) {
	 LOG(1,"Reenabling retries on reservation conflict"
	     " for %u - %u:%u (0x%02x)", handleID,
	     handle->target->id, handle->target->lun, opCode);
	 handle->target->flags &= ~SCSI_DONT_RETRY_ON_RESERV_CONFLICT;
      }
      SP_Unlock(&handle->adapter->lock);
   }

   SCSI_HandleRelease(handle);
}

/*
 * Do blocking scatter-gather reads or writes.
 */
VMK_ReturnStatus
SCSI_SGIO(SCSI_HandleID handleID,
          SG_Array *sgArr, 
          Bool isRead)
{   
   VMK_ReturnStatus status = VMK_IO_ERROR;
   Async_Token *token;
   int retries, conflictRetries, errorRetries;
   Timer_AbsCycles maxTime, now;
   SCSI_RetryStatus rstatus = RSTATUS_NO_RETRY;
   SCSI_Handle *handle = SCSI_HandleFind(handleID);

   if (UNLIKELY(handle == NULL)) {
      status = VMK_NOT_FOUND;
      Warning("returns %#x for unknown device", status);
      return status;
   }

   token = Async_AllocToken(0);
   ASSERT_NOT_IMPLEMENTED(token != NULL);
   token->resID = handle->worldID;

   errorRetries = SCSI_ERROR_MAX_RETRIES;
   retries = SCSI_BUSY_MAX_RETRIES;
   conflictRetries = CONFIG_OPTION(SCSI_CONFLICT_RETRIES)+1;
   now = Timer_GetCycles();
   maxTime = now + (SCSI_TIMEOUT/1000) * Timer_CyclesPerSecond() * TIMEOUT_RETRIES;
   while (retries-- && conflictRetries && errorRetries && now < maxTime) {
      status = SCSI_AsyncIO(handleID, sgArr, isRead, token); 
      if (status != VMK_OK) {
	 break;
      }
      ASSERT(token->cmd);

      status = SCSI_TimedWait(handleID, token, &rstatus);
      SCSIHandleSyncReservationConflict(handleID, status, &conflictRetries,
					isRead ? SCSI_CMD_READ10 :
					         SCSI_CMD_WRITE10);
      if (status == VMK_OK || rstatus == RSTATUS_NO_RETRY) {
	 break;
      }
      if (rstatus == RSTATUS_ERROR) {
         errorRetries--;
      }

      now = Timer_GetCycles();
   }

   Async_ReleaseToken(token);
   if (status != VMK_OK) {
      SCSI_Adapter *adapter = handle->adapter;
      SCSI_Target  *target  = handle->target;

      if ((adapter != NULL) && (target != NULL)) {
         Warning("returns %#x for %s:%d:%d", status, adapter->name, target->id, target->lun);
      } else {
         Warning("returns %#x for unknown device", status);
      }
   }

   SCSI_HandleRelease(handle);
   return status;
}

/*
 * Do non-blocking scatter-gather reads or writes. This routine is used by 
 * the VMFS code, the "vsd" devices, and for reading partition table info.
 *
 * NOTE: token->resID must be set to indicate consumer of the disk bandwidth.
 */
VMK_ReturnStatus
SCSI_AsyncIO(SCSI_HandleID handleID, 
             SG_Array *inSGArr, 
             Bool isRead,
	     Async_Token *token) 
{   
   SG_Elem *sgElem;
   SCSI_Command *cmd;
   SCSIReadWrite10Cmd *rwCmd;
   uint32 dataLength;
   uint32 sgLen;
   int i;
   uint32 cmdLength;
   VMK_ReturnStatus status;
   SCSI_ResultID rid;
   SCSI_Handle *handle;
   SCSI_Adapter *adapter;
   SCSI_Target *target;
   uint32 offset;
   Bool cmdIsPAEOK;

   /*
    * This code to dump out the disk blocks accessed comes
    * handy when debugging filesystem performance problems.
    * Just keep it around for future use...
    */
   if (LOGLEVEL() > 3) {
      const char* typeStr = ((inSGArr->addrType == SG_VIRT_ADDR) ? "virt"
                             : ((inSGArr->addrType == SG_MACH_ADDR) ? "mach"
                                : ((inSGArr->addrType == SG_PHYS_ADDR) ? "phys"
                                   : "unk")));
      char op = isRead ? 'r' : 'w';
      
      for (i=0; i<inSGArr->length; i++) {
	 unsigned int sblk, eblk;
	 sblk = inSGArr->sg[i].offset/512;
	 eblk = inSGArr->sg[i].length/512;
	 Log("%p, %c addr=%"FMT64"x (%s) blk@%d, len=%d",
             inSGArr, op, inSGArr->sg[i].addr, typeStr, sblk, eblk);
      }
   }

   handle = SCSI_HandleFind(handleID);
   if (handle == NULL) {
      LOG(1, "%p: invalid handleID: %d", inSGArr, handleID);
      return VMK_INVALID_HANDLE;
   }
   if (!isRead && (handle->flags & SCSI_HANDLE_READONLY)) {
      SCSI_HandleRelease(handle);
      Warning("Can't write through read-only handle 0x%x", handleID);
      return VMK_READ_ONLY;
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
   token->cmd = cmd;

   cmd->type = SCSI_QUEUE_COMMAND;
   cmd->sgArr.length = sgLen;
   if (inSGArr->addrType == SG_PHYS_ADDR) {
      cmd->sgArr.addrType = SG_PHYS_ADDR;
   } else {
      cmd->sgArr.addrType = SG_MACH_ADDR;
   }

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
	 //Warning("%s of %d bytes", isRead ? "read" : "wri", bytesLeft);
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

   rwCmd = (SCSIReadWrite10Cmd *)cmd->cdb;
   if (isRead) {
      rwCmd->opcode = SCSI_CMD_READ10;
   } else {
      rwCmd->opcode = SCSI_CMD_WRITE10;
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
   rwCmd->lbn = ByteSwapLong(offset);
   rwCmd->length = ByteSwapShort(CEIL(dataLength, target->blockSize));
   cmd->cdbLength = sizeof(SCSIReadWrite10Cmd);
   cmd->dataLength = dataLength;

   SP_Lock(&adapter->lock);      
   handle->serialNumber++;
   cmd->serialNumber = handle->serialNumber;
   SP_Unlock(&adapter->lock);   

   if (token->originHandleID == 0) {
      ASSERT(token->originSN == 0);
      token->originHandleID = handleID;
      token->originSN = cmd->serialNumber;
   }

   ASSERT(token->originSN && token->originHandleID);

   cmd->originHandleID = token->originHandleID;
   cmd->originSN = token->originSN;
   SCSIInitResultID(handle, token, &rid);

   rid.serialNumber = cmd->serialNumber;

   ASSERT(token->resID != -1);
   if (World_IsHELPERWorld(MY_RUNNING_WORLD)) {
         token->resID = Host_GetWorldID();
   }
   Async_RefToken(token);
   ASSERT(token->resID == handle->worldID || handle->worldID == Host_GetWorldID());

   SCSI_GetXferData(cmd, handle->target->devClass, handle->target->blockSize);
   cmdIsPAEOK = SCSIIsCmdPAEOK(adapter, cmd);
   if ((adapter->sgSize == 0) ||                   // block device
       ((cmd->sgArr.length <= adapter->sgSize) &&
        (cmd->dataLength <= adapter->maxXfer) &&   // small command
        cmdIsPAEOK)) {
      status = SCSIIssueCommand(handle, cmd, &rid);
   } else {
      status = SCSISplitSGCommand(handle, cmd, &rid, cmdIsPAEOK);
   }

   if (status == VMK_WOULD_BLOCK) {
      // IssueCommand has queued it. The caller need not do anything
      status = VMK_OK;
   }
   return status;
}

/*
 * Do a blocking SCSI read into vmkernel memory.
 */
VMK_ReturnStatus
SCSI_Read(SCSI_HandleID handleID, uint64 offset, void *data, uint32 length)
{
   SG_Array sgArr;
   sgArr.length = 1;
   sgArr.addrType = SG_VIRT_ADDR;
   sgArr.sg[0].addr = (VA)data;
   sgArr.sg[0].offset = offset;
   sgArr.sg[0].length = length;

   return SCSI_SGIO(handleID, &sgArr, TRUE);
}

/*
 * Return the capacity of the disk backing the specified SCSI partition.
 *
 * This function is called from coredump path, so please don't add any disk
 * reads or other weirdo things to this function.
 */
VMK_ReturnStatus 
SCSI_GetCapacity(SCSI_HandleID handleID, VMnix_GetCapacityResult *result)
{
   SCSI_Target *target;
   SCSI_Handle *handle = SCSI_HandleFind(handleID);   

   if (handle == NULL) {
      return VMK_INVALID_HANDLE;
   }

   target = handle->target;
   if (target->devClass == SCSI_CLASS_DISK ||
       target->devClass == SCSI_CLASS_OPTICAL ||
       target->devClass == SCSI_CLASS_WORM) {
      ASSERT(target->blockSize != 0);
      result->diskBlockSize = target->blockSize;
      result->numDiskBlocks = target->partitionTable[handle->partition].entry.numSectors;
   }

   SCSI_HandleRelease(handle);

   return VMK_OK;
}

/*
 * Return the capacity and geometry of the disk backing the specified SCSI
 * partition.
 */
VMK_ReturnStatus 
SCSI_GetGeometry(SCSI_HandleID handleID, VMnix_GetCapacityResult *result)
{
   SCSI_Target *target;
   SCSI_Handle *handle = SCSI_HandleFind(handleID);   

   if (handle == NULL) {
      return VMK_INVALID_HANDLE;
   }

   target = handle->target;
   ASSERT(target->blockSize != 0);

   result->diskBlockSize = target->blockSize;
   result->numDiskBlocks = target->partitionTable[handle->partition].entry.numSectors;
   result->startSector = target->partitionTable[handle->partition].entry.startSector;  
   result->cylinders = target->geometry.cylinders;
   result->heads = target->geometry.heads;
   result->sectors = target->geometry.sectors;

   SCSI_HandleRelease(handle);

   return VMK_OK;
}

void 
SCSI_RegisterIRQ(void *a, uint32 vector, IDT_Handler h, void *handlerData)
{
   SCSI_Adapter *adapter = (SCSI_Adapter *)a;
   adapter->intrHandler = h;
   adapter->intrHandlerData = handlerData;
   adapter->intrHandlerVector = vector;
}


/*
 *-----------------------------------------------------------------------------
 *
 *  SCSI_Dump --
 *
 *      Issue single command to write "length" size "data"  to the dump
 *      partition at "offset." Poll the device by calling its interrupt handler.
 *      (Interrupts are disabled, and we can't trust anything to work correctly
 *      at this point anyway.)
 *
 * Results:
 *      VMK_OK;
 *      VMK_TIMEOUT - command didn't complete;
 *      other error - from vmklinux.
 *
 * Side effects:
 *      No read-only checks on handle. 
 *
 *-----------------------------------------------------------------------------
 */

VMK_ReturnStatus
SCSI_Dump(SCSI_HandleID handleID, 
          uint64 offset,
          uint64 data, 
          uint32 length, 
          Bool isMachAddr)
{   
   static Async_Token token;
   static SCSI_Command dumpCmd;
   SCSI_Command *cmd;
   SCSIReadWrite10Cmd *rwCmd;
   VMK_ReturnStatus status;
   SCSI_ResultID rid;
   SCSI_Adapter *adapter;
   SCSI_Target *target;
   int count = 0;
   int retries;
   SCSI_Handle *handle;
   uint32 offsetBlocks;

   handle = SCSI_HandleFind(handleID);
   if (handle == NULL) {
      return VMK_INVALID_HANDLE;
   }

   adapter = handle->adapter;

   cmd = &dumpCmd;
   memset(cmd, 0, sizeof(dumpCmd));

   cmd->type = SCSI_DUMP_COMMAND;
   cmd->sgArr.length = 1;
   cmd->sgArr.addrType = SG_MACH_ADDR;
   cmd->sgArr.sg[0].length = length;   
   if (isMachAddr) {
      cmd->sgArr.sg[0].addr = data;
   } else {
      cmd->sgArr.sg[0].addr = VMK_VA2MA((VA)data);
   }
   MemMap_SetIOProtectionRange(cmd->sgArr.sg[0].addr, length, MMIOPROT_IO_ENABLE);

   rwCmd = (SCSIReadWrite10Cmd *)cmd->cdb;
   rwCmd->opcode = SCSI_CMD_WRITE10;
   target = handle->target;
   offsetBlocks = offset >> target->blockShift;
   if (handle->partition != 0) {
      if ((offsetBlocks + (length >> target->blockShift)) >
          target->partitionTable[handle->partition].entry.numSectors) {
         SCSI_HandleRelease(handle);
         return VMK_LIMIT_EXCEEDED;
      }
      offsetBlocks += target->partitionTable[handle->partition].entry.startSector;
   }
   rwCmd->lbn = ByteSwapLong(offsetBlocks);
   rwCmd->length = ByteSwapShort(CEIL(length, target->blockSize));
   cmd->cdbLength = sizeof(SCSIReadWrite10Cmd);

   handle->serialNumber++;

   cmd->serialNumber = handle->serialNumber;
   cmd->originSN = cmd->serialNumber;
   cmd->originHandleID = 0;

   memset(&token, 0, sizeof(token));
   SP_InitLock("tokenLck", &token.lock, SP_RANK_DUMP_TOKEN);
   token.refCount = 1;
   token.flags = ASYNC_DUMPING;

   SCSIInitResultID(handle, &token, &rid);
   rid.cmd = cmd;
   rid.serialNumber = cmd->serialNumber;
   SCSIChoosePath(handle, &rid);

   // Retry on VMK_WOULD_BLOCK; polling may have completed outstanding commands.
   retries = 3;
   do {
      /* We don't need to copy cmd here, because SCSILinuxDumpCommand() and
       * LinuxBlockDumpCommand() don't call SCSI_DoCommandComplete(). 
       */
      status = adapter->command(rid.path->adapter->clientData, cmd,
                                &rid, handle->worldID);
      count = 0; 
      // Always poll. Even if the call to adapter->command returned
      // VMK_WOULD_BLOCK it may complete other cmds. and free up cmd. slots
      while (!(token.flags & ASYNC_IO_DONE) && count < 1000) {
         adapter->intrHandler(adapter->intrHandlerData, 
                              adapter->intrHandlerVector);
         Util_Udelay(5000);
         count++;
      }
   } while ((status == VMK_WOULD_BLOCK) && (--retries > 0)); 

   if (status == VMK_OK) { 
      if (!(token.flags & ASYNC_IO_DONE)) {
         Warning("Write @ offset 0x%x timed out (c=%d)", offsetBlocks, count);
         status = VMK_TIMEOUT;
      } else {
         SCSI_Result *result = (SCSI_Result *)&token.result;
         SCSI_Status sstatus = result->status;

         if (sstatus) {
            Warning("Write @ offset 0x%x completed with status %d/%d", offsetBlocks,
                    SCSI_DEVICE_STATUS(sstatus), SCSI_HOST_STATUS(sstatus));
            status = VMK_IO_ERROR;
         }
      }
   } else {
      Warning("Write @ offset 0x%x returned status %d (c=%d)", offsetBlocks, status,
      count);
   }

   SP_CleanupLock(&token.lock);
   SCSI_HandleRelease(handle);
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * SCSIDoGetTargetType --
 *
 *      Does a SCSI Inquiry command to get the target type
 *
 * Results:
 *      Status of the command,VMK_OK if it succeeds
 *
 * Side effects:
 *      devClass is set to the device class of the target
 *
 *----------------------------------------------------------------------
 */ 
static VMK_ReturnStatus 
SCSIDoGetTargetType(SCSI_Handle *handle, uint8 *devClass)
{
   SCSIInquiryCmd *iCmd;
   SCSIInquiryResponse *iResponse;
   VMK_ReturnStatus status;
   SCSI_Command *cmd;
   SCSI_Info *info;

   info = (SCSI_Info *)Mem_Alloc(sizeof(SCSI_Info));
   ASSERT(info != NULL);
   iResponse = (SCSIInquiryResponse *)Mem_Alloc(sizeof(SCSIInquiryResponse));
   ASSERT(iResponse != NULL);
   memset(iResponse, 0, sizeof(SCSIInquiryResponse));
   ASSERT(handle != NULL);
   if (!handle->adapter->getInfo(handle->adapter->clientData,
                                 handle->target->id,
				 handle->target->lun, 
                                info, (uint8 *)iResponse, sizeof(*iResponse))) {
      cmd = (SCSI_Command *)Mem_Alloc(sizeof(SCSI_Command));
      ASSERT(cmd != NULL);
      memset(cmd, 0, sizeof(SCSI_Command));
      cmd->type = SCSI_QUEUE_COMMAND;
      cmd->sgArr.length = 1;
      cmd->sgArr.addrType = SG_MACH_ADDR;
      cmd->sgArr.sg[0].addr = VMK_VA2MA((VA)iResponse);
      cmd->sgArr.sg[0].length = sizeof(*iResponse);
   
      iCmd = (SCSIInquiryCmd *)cmd->cdb;
      memset(iCmd, 0, sizeof(*iCmd));
      iCmd->opcode = SCSI_CMD_INQUIRY;
      iCmd->len = sizeof(*iResponse);
      cmd->cdbLength = sizeof(SCSIInquiryCmd);
      cmd->dataLength = 36; // Minimum response length for inquiry
   
      status = SCSISyncCommand(handle, cmd, NULL, FALSE);
      Mem_Free(cmd);
   } else {
      status = VMK_OK;
   }
   if (status == VMK_OK) {
      *devClass = iResponse->devclass;
      LOG(1, "%s:%d:%d class %x qual %x", 
          handle->adapter->name, handle->target->id, handle->target->lun, 
          iResponse->devclass, iResponse->pqual);
   } else {
      Warning("%s:%d:%d status = %d",
	      handle->adapter->name, handle->target->id, handle->target->lun,
	      status);
   }
   Mem_Free(iResponse);
   Mem_Free(info);
   return status;
}

/*
 *-----------------------------------------------------------------------------
 *
 * SCSI_ReadGeometry --
 *
 *      Read geometry for target with given "handle". 
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None. 
 *
 *-----------------------------------------------------------------------------
 */

void 
SCSI_ReadGeometry(SCSI_Handle *handle, // IN 
                  uint8 *mbrBuf,       // IN: buffer containing MBR.
                  uint32 bufSize)      // IN 
{
   SCSI_Target *target = handle->target;

   if (handle->adapter->getGeometry) {
      handle->adapter->getGeometry(handle->adapter->clientData,
                                   target->id, target->lun,
                                   target->numBlocks, mbrBuf, bufSize,
                                   &target->geometry);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * SCSIDoGetCapacity --
 *
 *      Return capacity info ("diskBlockSize" and "numDiskBlocks") for "handle". 
 *
 * Results:
 *      VMK_ReturnStatus.
 *
 * Side effects:
 *      None. 
 *
 *-----------------------------------------------------------------------------
 */

static VMK_ReturnStatus 
SCSIDoGetCapacity(SCSI_Handle *handle, uint32 *diskBlockSize, uint32 *numDiskBlocks)
{
   SCSIReadCapacityResponse *response;
   SCSIReadCapacityCmd *rcCmd;
   VMK_ReturnStatus status;
   SCSI_Command *cmd;

   response = (SCSIReadCapacityResponse *)Mem_Alloc(sizeof(SCSIReadCapacityResponse));
   ASSERT(response != NULL);
   memset(response, 0, sizeof(SCSIReadCapacityResponse));

   cmd = (SCSI_Command *)Mem_Alloc(sizeof(SCSI_Command));
   ASSERT(cmd != NULL);
   memset(cmd, 0, sizeof(SCSI_Command));
   cmd->type = SCSI_QUEUE_COMMAND;
   cmd->sgArr.length = 1;
   cmd->sgArr.addrType = SG_MACH_ADDR;
   cmd->sgArr.sg[0].addr = VMK_VA2MA((VA)response);
   cmd->sgArr.sg[0].length = sizeof(*response);

   rcCmd = (SCSIReadCapacityCmd *)cmd->cdb;
   memset(rcCmd, 0, sizeof(*rcCmd));
   rcCmd->opcode = SCSI_CMD_READ_CAPACITY;
   cmd->cdbLength = sizeof(SCSIReadCapacityCmd);
   cmd->dataLength = 8;  // Only acceptable size for response

   status = SCSISyncCommand(handle, cmd, NULL, FALSE);
   Mem_Free(cmd);

   if ((status == VMK_OK) && (response->blocksize)) {
      *diskBlockSize = ByteSwapLong(response->blocksize); 
      *numDiskBlocks = (uint32)((uint32)ByteSwapLong(response->lbn) + (uint32)1);

      LOG(1, "%s:%d:%d  numDiskBlocks = %u (0x%x), diskBlockSize = %u",
	  handle->adapter->name, handle->target->id, handle->target->lun, 
          *numDiskBlocks, *numDiskBlocks, *diskBlockSize);
   }
   else {
      if (response->blocksize == 0)
            status = VMK_IO_ERROR;
      Warning("Failed for %s:%d:%d status = %d",
	      handle->adapter->name, handle->target->id, handle->target->lun,
	      status);
   }
   Mem_Free(response);
   return status;
} 

/*
 *----------------------------------------------------------------------
 *
 * SCSI_Cleanup --
 *
 *      Close all scsi devices while unloading the vmkernel.  This code 
 *	just closes devices without regard for operations in progress and 
 *	such.  This needs to be called after all worlds are killed and
 *	with all interrupts masked.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	All scsi devices are closed.
 *
 *----------------------------------------------------------------------
 */
void
SCSI_Cleanup(void)
{
   int i;

   for (i = 0; i < HASH_BUCKETS; i++) {
      SCSI_Adapter *adapter, *adapterNext;
      for (adapter = adapterHashTable[i];
           adapter != NULL;
	   adapter = adapterNext) {
         // Warning: The adapter is freed as a result of the close
         adapterNext = adapter->next;
	 LOG(0, "closing SCSI adapter %s", adapter->name);
         ScsiProcRemoveAdapter(adapter);
	 adapter->close(adapter->clientData);
      }
   }
   //The VMFS partition cache is invalid at this point but we don't care.
   ScsiProcCleanup();
   SP_CleanupLock(&cosLunListLock);
}

/*
 * Get the target type, capacity, and partition table of the target
 * associated with 'handle'.
 */
static VMK_ReturnStatus
SCSIGetAttrs(SCSI_Handle *handle)
{
   VMK_ReturnStatus status;
   SCSI_Target *target;
   uint32 shift;

   target = handle->target;
   status = SCSIDoGetTargetType(handle, &target->devClass);

   if (status != VMK_OK) {
      return status;
   }

   // Get the blockSize and numBlocks only if it is a block device.
   // Non-block devices should only be used for raw access (not VMFS) and
   // do not support splitting of SCSI commands.
   if (target->devClass == SCSI_CLASS_DISK    ||
       target->devClass == SCSI_CLASS_OPTICAL ||
       target->devClass == SCSI_CLASS_WORM) {

      if (target->flags & SCSI_DEV_PSEUDO_DISK) {
         /*
          * Set default values
          */
         target->blockSize = DEFAULT_PSEUDO_DISK_BLOCK_SIZE;
         target->numBlocks =  0;
      } else {
         status = SCSIDoGetCapacity(handle, &target->blockSize, &target->numBlocks);
         if (status != VMK_OK) {
            return status;
         }
      }

      for (shift = 0; shift < 32; shift++) {
         if ((target->blockSize & (1 << shift)) != 0) {
            break;
         }
      }
      target->blockShift = shift;
      ASSERT((1 << target->blockShift) == target->blockSize);
      // Recover in a release build if target->blockSize is bad.
      if ((1 << target->blockShift) != target->blockSize) {
	 target->blockSize = 0;
	 target->blockShift = 0;
      }
   }

   // Get the partition table only if a disk.
   if ((target->devClass == SCSI_CLASS_DISK) && !(target->flags & SCSI_DEV_PSEUDO_DISK)) {
      status = SCSIUpdatePTable(handle, target);
   }
   
   return status;
}

/*
 * Release a SCSI handle.  If the ref count goes to zero, then
 * actually free the handle.
 */
void
SCSI_HandleRelease(SCSI_Handle *handle)
{
   Async_Token *token, *next;
   SCSI_Target *target;
   SCSI_Adapter *adapter = handle->adapter; 

   ASSERT(handle != NULL);

   SP_Lock(&handleArrayLock);

   handle->refCount--;
   ASSERT(handle->refCount >= 0);

   if (handle->refCount > 0) {
      SP_Unlock(&handleArrayLock);
      return;
   }
   SP_Unlock(&handleArrayLock);

   ASSERT(handleArray[handle->handleID & SCSI_HANDLE_MASK] != handle);

   SP_Lock(&adapter->lock);

   target = handle->target;

   // Handles may also not match if handle is a second handle on
   // the same partition, either because it was opened in read-only
   // mode by the host, or because SCSI passthrough locking was
   // turned off.
   if (target->partitionTable[handle->partition].handle == handle) {
      target->partitionTable[handle->partition].handle = NULL;
   }

   // Release the ref count and use count on target handle.
   SCSI_ReleaseTarget(target, FALSE);
   target->useCount--;
   ASSERT(target->useCount >= 0);
   SP_Unlock(&adapter->lock);

   SP_Lock(&scsiLock);
   adapter->openCount--;
   ASSERT(adapter->openCount >= 0);
   SP_Unlock(&scsiLock);

   if (adapter->moduleID != 0) {
      Mod_DecUseCount(adapter->moduleID);
   }

   for (token = handle->resultListHead; token != NULL; token = next) {
      next = token->nextForCallee;
      Async_ReleaseToken(token);
   }
   Mem_Free(handle);
}

void
SCSIHandleDestroy(SCSI_Handle *handle)
{
   handleArray[handle->handleID & SCSI_HANDLE_MASK] = NULL;
   SCSI_HandleRelease(handle);
}

/*
 * Free up the memory associated with a target, including the alternate
 * paths.  Create new targets if there are alternate paths that aren't
 * getting removed.  If 'modUnload' is TRUE, we are removing the entire driver 
 * module, otherwise we are just removing a single target that has disappeared.
 */
static void
SCSITargetFree(SCSI_Target *target, 
               Bool modUnload)
{
   SCSI_Path *path, *npath;

   for (path = target->paths; path != NULL; path = npath) {
      npath = path->next;
      if ((modUnload && path->adapter->moduleID != target->adapter->moduleID) 
          || (!modUnload && (path->adapter != target->adapter ||
			     path->id != target->id))) {
	 SCSI_CreateTarget(path->adapter, path->id, path->lun,
			   target->maxQDepth, &target->diskId, target->flags & SCSI_DEV_PSEUDO_DISK);
      }
      Mem_Free(path);
   }
   /* 
    * Rescan may try to remove a target which was created above when deleting
    * another path; this target won't have partitionTable allocated yet,
    * because partition scan happens after rescan is done.
    */
   if (target->partitionTable) {
      Mem_Free(target->partitionTable);
   } else {
      ASSERT(!modUnload);
   }
   ASSERT(target->schedQ == NULL);
   if (target->vendorData) {
        Mem_Free(target->vendorData);
   }
   Mem_Free(target);
}

/*
 * Free the allocated memory of an adapter. 
 */
static void
SCSIAdapterFree(SCSI_Adapter *adapter)
{
   SP_CleanupLock(&adapter->lock);
   Mem_Free(adapter);
}

/*
 * Update the partition table info of a target by rereading the disk.
 * See partition.c for the layout of target->partitionTable. 
 */
static VMK_ReturnStatus
SCSIUpdatePTable(SCSI_Handle *handle, SCSI_Target *target)
{
   VMK_ReturnStatus status;
   Partition_Table *partitionTable;

   partitionTable = (Partition_Table *)Mem_Alloc(sizeof(Partition_Table));
   if (partitionTable == NULL) {
      return VMK_NO_RESOURCES;
   }

   ASSERT(target->partitionTable != NULL);
   status = Partition_ReadTable(handle, partitionTable);
   if (status == VMK_OK) {
      SCSI_Partition *newPTable, *oldPTable;   
      int onp, np = partitionTable->numPartitions;

      ASSERT(np >= 1 && np < VMNIX_MAX_PARTITIONS);   

      newPTable = (SCSI_Partition *)Mem_Alloc(np * sizeof(SCSI_Partition));
      if (newPTable == NULL) {
	 status = VMK_NO_RESOURCES;
      } else {
	 int i;      

	 memset(newPTable, 0, np * sizeof(SCSI_Partition));
         oldPTable = target->partitionTable;
	 newPTable[0].stats = oldPTable[0].stats;
         onp = target->numPartitions;
	 for (i = 0; i < np; i++) {
            // Check if this is a valid entry
            if (partitionTable->entries[i].numSectors != 0) {
               // Get the partition number
               int j = partitionTable->entries[i].number;
               ASSERT(j < np);

               newPTable[j].entry = partitionTable->entries[i];
               if (j < onp) {
                  // Copy open handles from the old PTable
                  newPTable[j].handle = oldPTable[j].handle;
                  newPTable[j].stats = oldPTable[j].stats;
                  newPTable[j].nReaders = oldPTable[j].nReaders;
                  newPTable[j].nWriters = oldPTable[j].nWriters;
                  newPTable[j].flags = oldPTable[j].flags;
               }
              LOG(3, "pt[%d]: start %d num %d type %d number %d",
                      i, 
                      partitionTable->entries[i].startSector,
                      partitionTable->entries[i].numSectors,
                      partitionTable->entries[i].type,
                      partitionTable->entries[i].number);
            }
	 }   
	 newPTable[0].handle = oldPTable[0].handle;
         newPTable[0].nReaders = oldPTable[0].nReaders;
         newPTable[0].nWriters = oldPTable[0].nWriters;
         newPTable[0].flags = oldPTable[0].flags;

	 /*
	  * Update the target with the new partition table.  XXX This
	  * is a race, since the target's partition table may be
	  * accessed at any time by open SCSI handles for this target.
	  * We should either add some locking, or not update the
	  * partition table when there are open handles.  For now, we
	  * build the new partition table, and then change over to the
	  * new one with these two stores.
	  */
	 target->partitionTable = newPTable;
	 target->numPartitions = np;
	 Mem_Free(oldPTable);
      }
   }

   Mem_Free(partitionTable);
   return status;
}

VMK_ReturnStatus
SCSI_QueryHandle(SCSI_HandleID hid, char **name, uint32 *targetID, 
                 uint32 *lun, uint32 *partition,  uint32 *partitionType)
{
   SCSI_Handle *handle = SCSI_HandleFind(hid);
   if (handle == NULL) {
      return VMK_INVALID_HANDLE;
   }

   *name = handle->adapter->name;
   *targetID = handle->target->id;
   *lun = handle->target->lun;
   *partition = handle->partition;

   if (partitionType != NULL) {
      *partitionType = handle->target->partitionTable[handle->partition].entry.type;
   }

   SCSI_HandleRelease(handle);

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * SCSIDoGetTargetInfo --
 *
 *	Query the hardware to get the information for a specified target/LUN
 *	on the named adapter.
 *      REQUIRES: scsiLock to be held by caller
 *
 * Results:
 *      none
 *
 * Side effects:
 *      fills in partition info in targetInfo
 *      status is returned in targetInfo->status:
 *       VMK_NOT_FOUND if named adapter doesn't exist or target not found
 *       VMK_NO_RESOURCES if too many partitions found
 *       result of SCSIValidatePartitionTable if a partition is found
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
SCSIDoGetTargetInfoInt(SCSI_Adapter *adapter,
                       SCSI_Target *target,
                       VMnix_TargetInfo *targetInfo,
                       Bool validatePartitionTable)
{
   SCSI_Info scsiInfo;   
   int j;
   VMK_ReturnStatus status = VMK_OK;

   ASSERT(SP_IsLocked(&scsiLock));
   if (!adapter->getInfo(adapter->clientData, target->id, target->lun, &scsiInfo,
                         targetInfo->inquiryInfo, VMNIX_INQUIRY_LENGTH)) {
      Warning("Could not get info for %s targ%d lun%d",
              adapter->name, target->id, target->lun);
      status = VMK_NOT_FOUND;
      return status;
   }

   targetInfo->targetID = target->id;
   targetInfo->lun = target->lun;
   targetInfo->queueDepth = scsiInfo.queueDepth;

   if (validatePartitionTable) {
      status = SCSIValidatePartitionTable(adapter, target);
      if (status != VMK_OK) {
	 return status;
      }
   }

   targetInfo->numPartitions = 0;
   for (j = 0; j < target->numPartitions; j++) {
      if (target->partitionTable[j].entry.numSectors == 0) {
         continue;
      } else if (targetInfo->numPartitions >= VMNIX_MAX_PARTITIONS) {
         status = VMK_NO_RESOURCES;
         break;
      } else {
         VMnix_PartitionInfo *pi =
            &targetInfo->partitionInfo[targetInfo->numPartitions];
         pi->number = j;
         pi->start = target->partitionTable[j].entry.startSector;
         pi->nsect = target->partitionTable[j].entry.numSectors;
         pi->type = target->partitionTable[j].entry.type;
/*
         if (target->partitionTable[j].handle != NULL) {
            pi->worldID = target->partitionTable[j].handle->worldID;
         } else {
            pi->worldID = INVALID_WORLD_ID;
         }
*/
         targetInfo->numPartitions++;
      }
   }

   targetInfo->geometry = target->geometry;
   targetInfo->blockSize = target->blockSize;
   targetInfo->numBlocks = target->numBlocks;
   targetInfo->devClass = target->devClass;
   memcpy(&targetInfo->diskId, &target->diskId, sizeof(SCSI_DiskId));

   return status;
}

static VMK_ReturnStatus
SCSIDoGetTargetInfo(SCSI_Adapter *adapter,
		    uint32 targetID,
                    uint32 lun,
		    VMnix_TargetInfo *targetInfo,
		    Bool validatePartitionTable)
{
   VMK_ReturnStatus status = VMK_OK;
   SCSI_Target *target;

   ASSERT(SP_IsLocked(&scsiLock));

   target = SCSI_FindTarget(adapter, targetID, lun, TRUE);
   if (target == NULL) {
      return(VMK_NOT_FOUND);
   }
   status = SCSIDoGetTargetInfoInt(adapter, target, targetInfo, 
                                   validatePartitionTable);
   SCSI_ReleaseTarget(target, TRUE);
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * SCSI_GetTargetInfo --
 *
 *	Given a vmhba name, target, and lun, get target information
 *
 * Results:
 *      Target information is returned in targetInfo
 *
 * Side effects:
 *      calls SCSIDoGetTargetInfo to fill in partition info in targetInfo
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
SCSI_GetTargetInfo(const char *name, uint32 targetID, uint32 lun,
                    VMnix_TargetInfo *targetInfo)
{
   VMK_ReturnStatus status = VMK_OK;   
   SCSI_Adapter *adapter;

   SP_Lock(&scsiLock);

   adapter = SCSIFindAdapter(name);
   if (adapter == NULL) {
      SP_Unlock(&scsiLock);
      return VMK_NOT_FOUND;
   } else if (adapter->openInProgress) {
      status = VMK_BUSY;
   } else {
      status = SCSIDoGetTargetInfo(adapter, targetID, lun, targetInfo, TRUE);
   }
   
   SP_Unlock(&scsiLock);

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * SCSIValidatePartitionTable --
 *
 *	Probe the SCSI target on adapter for its partition table
 *	Requires that 'scsiLock' is held.
 *
 * Results:
 *      VMK_NO_RESOURCES if can't allocate a handle for the target,
 *      return status from SCSIGetAttrs otherwise
 *
 * Side effects:
 *      plenty
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
SCSIValidatePartitionTable(SCSI_Adapter *adapter, SCSI_Target *target)
{
   VMK_ReturnStatus status;
   SCSI_Handle *handle;
   uint32 targetID = target->id;
   uint32 lun = target->lun;

   ASSERT(SP_IsLocked(&scsiLock));
   while (adapter->openInProgress) {
      CpuSched_Wait((uint32)&adapter->openInProgress,
                    CPUSCHED_WAIT_SCSI,
                    &scsiLock);
      SP_Lock(&scsiLock);
   }

   /* Create a dummy handle to read the partition table. */
   if (target->partitionTable == NULL) {
      target->partitionTable = (SCSI_Partition *)Mem_Alloc(sizeof(SCSI_Partition));
      if (target->partitionTable == NULL) {
         return VMK_NO_MEMORY;
      }
      memset(target->partitionTable, 0, sizeof(SCSI_Partition));
      target->numPartitions = 1;

      handle = SCSIAllocHandle(adapter, Host_GetWorldID(), targetID, lun, 0);
      if (handle == NULL) {
	 Mem_Free(target->partitionTable);
	 target->partitionTable = NULL;
         target->numPartitions = 0;
         return VMK_NO_RESOURCES;
      }
   } else {
      handle = SCSIAllocHandle(adapter, Host_GetWorldID(), targetID, lun, 0);
      if (handle == NULL) {
         return VMK_NO_RESOURCES;
      }
   }

   adapter->openInProgress = TRUE;
   SP_Unlock(&scsiLock);

   status = SCSIGetAttrs(handle);

   /* Delete the dummy handle */
   SCSIHandleDestroy(handle);

   SP_Lock(&scsiLock);
   adapter->openInProgress = FALSE;
   CpuSched_Wakeup((uint32)&adapter->openInProgress);

   return status;
}

/* 
 * The next few routines provide support for the /proc/vmware/scsi entries
 */
   
Proc_Entry scsiProcDir;

/*
 *----------------------------------------------------------------------
 *
 *  ScsiProcInit--
 *
 *       Sets up the /proc/vmware/scsi entry
 *
 * Results: 
 *	none
 *
 * Side effects:
 *	none
 *
 *----------------------------------------------------------------------
 */
static void 
ScsiProcInit(void)
{
   char *buf = "scsi";

   scsiProcDir.read = NULL;
   scsiProcDir.write = NULL;
   scsiProcDir.parent = NULL;
   scsiProcDir.private = NULL;
   Proc_Register(&scsiProcDir, buf, TRUE);
}


/*
 *----------------------------------------------------------------------
 *
 * ScsiProcCleanup --
 *
 *       Remove the /proc/vmware/scsi entry
 *
 * Results: 
 *	none
 *
 * Side effects:
 *	none
 *
 *----------------------------------------------------------------------
 */
static void 
ScsiProcCleanup(void)
{
   Proc_Remove(&scsiProcDir);
}


/*
 *----------------------------------------------------------------------
 *
 * ScsiProcAddAdapter --
 *
 *       Sets up the /proc/vmware/scsi/scsi<n> entry
 *
 * Results: 
 *	none
 *
 * Side effects:
 *	none
 *
 *----------------------------------------------------------------------
 */
static void 
ScsiProcAddAdapter(SCSI_Adapter *adapter)
{
   adapter->adapProcEntry.read = NULL;
   adapter->adapProcEntry.write = NULL;
   adapter->adapProcEntry.parent = &scsiProcDir;
   adapter->adapProcEntry.private = adapter;
   Proc_Register(&adapter->adapProcEntry, adapter->name, TRUE);

   adapter->statsProcEntry.read = ScsiProcAdapStatsRead;
   adapter->statsProcEntry.write = ScsiProcAdapStatsWrite;
   adapter->statsProcEntry.parent = &adapter->adapProcEntry;
   adapter->statsProcEntry.canBlock = FALSE;
   adapter->statsProcEntry.private = adapter;
   Proc_Register(&adapter->statsProcEntry, "stats", FALSE);
}


/*
 *----------------------------------------------------------------------
 *
 * ScsiProcRemoveAdapter --
 *
 *       Remove the /proc/vmware/scsi/scsi<n> entries
 *
 * Results: 
 *	none
 *
 * Side effects:
 *	none
 *
 *----------------------------------------------------------------------
 */
static void 
ScsiProcRemoveAdapter(SCSI_Adapter *adapter)
{
   SCSI_Target *target;

   for (target = adapter->targets; target != NULL; target = target->next) {
      Proc_Remove(&target->procEntry);
   }
   Proc_Remove(&adapter->statsProcEntry);
   Proc_Remove(&adapter->adapProcEntry);
}


/*
 *----------------------------------------------------------------------
 *
 * ScsiProcAdapStatsRead --
 *
 *       handles read to the /proc/vmware/scsi/scsi<n> entry
 *
 * Results: 
 *	none
 *
 * Side effects:
 *	none
 *
 *----------------------------------------------------------------------
 */
static int
ScsiProcAdapStatsRead(Proc_Entry  *entry,
                      char        *page,
                      int         *len)
{
   SCSI_Adapter *adapter = entry->private;
   SCSI_Stats stats;
   VMK_ReturnStatus status;
   *len = 0;

   if ((status = SCSI_GetAdapterStats(adapter->name, &stats, sizeof(stats))) != VMK_OK) {
      return (status);
   }

   Proc_Printf(page, len, "%s: ", adapter->name);
   if (adapter->flags & SCSI_SHARED_DEVICE) {
      Proc_Printf(page, len, "shared with Service Console\n");
   } else {
      Proc_Printf(page, len, "not shared\n");
   }

   Proc_Printf(page, len, "PCI info for %s: " PCI_DEVICE_BUS_ADDRESS "\n\n",
		  adapter->name, 
		  adapter->bus, PCI_SLOT(adapter->devfn), 
		  PCI_FUNC(adapter->devfn));

   SCSIProcPrintHdr(page, len);
   Proc_Printf(page, len, "\n");
   SCSIProcPrintStats(&stats, page, len);
   Proc_Printf(page, len, "\n");

   return(VMK_OK);
}

static int
ScsiProcAdapStatsWrite(Proc_Entry  *entry,
		       char        *page,
		       int         *lenp)
{
#ifdef VMX86_DEBUG
   static const char *MagicSequence = "crash me 0123456789";

   if (!strncmp(page, MagicSequence, 19) ) {
      Warning("Magic sequence encountered.  lenp=%p, *lenp=%d.  Assert failing.", lenp, *lenp);
      ASSERT_NOT_IMPLEMENTED(0);
   }
   if (!strncmp(page, MagicSequence, 8) ) {
      volatile int *foo = NULL;
      Warning("Magic sequence encountered.  Causing an exception");
      *foo = 0xabc;
   }
#endif

#ifdef VMX86_DEVEL
   if (strcmp(page, "dropHost\n") == 0) {
      Warning("dropHost recd.");
      dropScsiCmd = DROP_HOST_CMD;
   } else if (strcmp(page, "dropAny\n") == 0) {
      Warning("dropAny recd.");
      dropScsiCmd = DROP_ANY_CMD;
   }
#endif
   return(VMK_OK);
}

#ifdef VMX86_DEVEL
/*
 *----------------------------------------------------------------------
 *
 * SCSI_DropCmd --
 *
 *      Query, and optionally reset, the value of "dropScsiCmd."
 *
 *      Write "dropHost\n" or "dropAny\n"  to /proc/vmware/scsi/vmhba?/stats to
 *      cause a single command to be dropped. The strings induce dropping a
 *      consoleOS or any command respectively. See ProcWriteHandler above.
 *
 * Results: 
 *      Value of dropScsiCmd at time of call.
 *
 * Side effects:
 *      May reset dropScsiCmd to DROP_NONE.	
 *
 *----------------------------------------------------------------------
 */
DropCmdType
SCSI_DropCmd(Bool reset)
{
   DropCmdType old = dropScsiCmd;
   if (reset) {
      dropScsiCmd = DROP_NONE;
   } 
   return old;
}

#endif

#define MAX_SCSI_DEVICE_CODE 14

const char *const scsiDeviceTypes[MAX_SCSI_DEVICE_CODE] =
{
    "Direct-Access           ",
    "Sequential-Access       ",
    "Printer                 ",
    "Processor               ",
    "WORM                    ",
    "CD-ROM                  ",
    "Scanner                 ",
    "Optical Device          ",
    "Medium Changer          ",
    "Communications          ",
    "Unknown                 ",
    "Unknown                 ",
    "Storage Array Controller",
    "Enclosure               ",
};

/*
 *----------------------------------------------------------------------
 *
 * ScsiFormatInquiry --
 *
 *      Utility function to format Scsi inquiry data
 *
 * Results: 
 *	None
 *
 * Side effects:
 *	none
 *
 *----------------------------------------------------------------------
 */
static void
ScsiFormatInquiry(SCSI_DiskId *diskId, char *buffer, int* len)
{ 
   char vendor[SCSI_VENDOR_LENGTH + 1];
   char model[SCSI_MODEL_LENGTH + 1];
   char revision[SCSI_REVISION_LENGTH + 1];
   char *p;
   int  i;

   /*
    * extract printable characters
    */
   p = vendor;
   for (i = 0; i < SCSI_VENDOR_LENGTH; i++) {
      if (diskId->vendor[i] >= 0x20 && diskId->vendor[i] <= 0x7E) {
         *p = diskId->vendor[i];
      } else {
         *p = ' ';
      }
      p++;
   }
   *p = '\0';

   p = model;
   for (i = 0; i < SCSI_MODEL_LENGTH; i++) {
      if (diskId->model[i] >= 0x20 && diskId->model[i] <= 0x7E) {
         *p = diskId->model[i];
      } else {
         *p = ' ';
      }
      p++;
   }
   *p = '\0';
   
   p = revision;
   for (i = 0; i < SCSI_REVISION_LENGTH; i++) {
      if (diskId->revision[i] >= 0x20 && diskId->revision[i] <= 0x7E) {
         *p = diskId->revision[i];
      } else {
         *p = ' ';
      }
      p++;
   }
   *p = '\0';

   Proc_Printf(buffer, len, "Vendor: %s  Model: %s  Rev: %s\n",
               vendor, model, revision);
   Proc_Printf(buffer, len, "Type:   %s ", 
                diskId->deviceType < MAX_SCSI_DEVICE_CODE ? scsiDeviceTypes[diskId->deviceType] : "Unknown ");
   Proc_Printf(buffer, len, "                 ANSI SCSI revision: %02x\n", diskId->scsiLevel);
}


/*
 *----------------------------------------------------------------------
 *
 * ScsiProcTargRead --
 *
 *       Handles read to the /proc/vmware/scsi/vmhba<n>/<tgt>:<lun> entry
 *
 * Results: 
 *	none
 *
 * Side effects:
 *	none
 *
 *----------------------------------------------------------------------
 */
static int
ScsiProcTargRead(Proc_Entry  *entry,
                 char        *page,
                 int         *len)
{
   int i;
   int numValidPartitions = 0;
   SCSI_Adapter *adapter = entry->parent->private;
   uint16 targetID = (((int) (entry->private))>>16)&0x00ffff;
   uint16 lun = ((int) (entry->private))&0x00ffff;
   uint64 mbytes = 0;
   uint32 unsNumBlocks = 0, unsBlockSize = 0;
   VMK_ReturnStatus status = VMK_OK;
   SCSI_Partition *partition = NULL;
   SCSI_Target* target = NULL; 
   SCSI_Path *path = NULL;

   target = SCSI_FindTarget(adapter, targetID, lun, TRUE);
   if (target == NULL) {
      status = VMK_INVALID_HANDLE;
      goto exit;
   }

   /* Start of proc node output data */
   *len = 0; 
   ScsiFormatInquiry(&target->diskId, page, len);

   /*
    * Convert signed 32bit values to unsigned 32bit values  
    * before converting to unsigned 64 bit values so as not 
    * to trip over sign extension problems.
    */
   unsBlockSize = target->blockSize;
   unsNumBlocks = target->numBlocks;
   mbytes = (uint64)unsBlockSize * (uint64)unsNumBlocks;
   mbytes = (uint64)(mbytes >> 20); 
   
   Proc_Printf(page, len, "Id: ");
   if (target->diskId.type == VMWARE_SCSI_ID_UNIQUE) {
      Proc_Printf(page, len, "unique id"); 
   } else {
      for (i = 0; i < target->diskId.len; i++) {
         Proc_Printf(page, len, "%x ", target->diskId.id[i]); 
      }
   }
   Proc_Printf(page, len, "\n");
   if (target->flags & SCSI_DEV_PSEUDO_DISK) {
      Proc_Printf(page, len, "Pseudo Device\n");
   }
   Proc_Printf(page, len, "Size:   %Ld Mbytes\n", (uint64)mbytes);
   Proc_Printf(page, len, "Queue Depth: %d\n", target->maxQDepth);

   Proc_Printf(page, len , "\n\nPartition Info:\n");
   Proc_Printf(page, len, "Block size: %d\nNum Blocks: %u\n\n",
                 target->blockSize, target->numBlocks);

   //
   // target->numParitions is the count of all the parition slots on the disk.
   // We are only interested in displaying the partitions that contain non-zero
   // information. Partition 0 is a special partition that contains all the sectors
   // on the disk.
   for (i = 0; i < target->numPartitions; i++) {
      partition = &target->partitionTable[i]; 
      if (partition->entry.numSectors > 0 ) {
         numValidPartitions++;
      }
   }

   if (numValidPartitions > 1) {
      Proc_Printf(page, len, "%8s: %8s %8s %8s\n",
                     " num", "Start", "Size", "Type"); 

      for (i = 1; i <target->numPartitions; i++) {
         partition = &target->partitionTable[i];
         if (partition->entry.numSectors > 0) {	       
            Proc_Printf(page, len, "%8d: %8d %8d %8x\n", 
               partition->entry.number, partition->entry.startSector, 
	       partition->entry.numSectors, partition->entry.type);
         }
      }
   }
   Proc_Printf(page, len, "\n\n");

   if (numValidPartitions == 1 && target->partitionTable[0].entry.number == 0) {
      SCSI_Stats *stats = &target->partitionTable[0].stats;
      SCSIProcPrintHdr(page, len);
      Proc_Printf(page, len, "\n");
      SCSIProcPrintStats(stats, page, len);
      Proc_Printf(page, len, "\n");
   } else {
      int i;
      Proc_Printf(page, len, "%9s %6s ", "Partition", "VM");
      SCSIProcPrintHdr(page, len);
      Proc_Printf(page, len, "\n");

      for (i = 0; i < target->numPartitions; i++) {
         partition = &target->partitionTable[i];
	 if (partition->entry.numSectors > 0) {
	    SCSI_Stats *stats = &partition->stats;
	    if (stats->commands > 0 || partition->handle != NULL) {
	       Proc_Printf(page, len, "%9d ", partition->entry.number);
	       if (partition->handle != NULL) {
	          Proc_Printf(page, len, "%6d ", partition->handle->worldID);
	       } else {
	          Proc_Printf(page, len, "%6s ", "-"); 
               }        
               SCSIProcPrintStats(stats, page, len);
               Proc_Printf(page, len, "\n");
	    }
         }
      }
   }

   SP_Lock(&adapter->lock); 
   SCSIProcPrintPerVM(page, len, target);

   // failover path config
   Proc_Printf(page, len, "\nPaths:%s\n",
      target->policy == SCSI_PATH_ROUND_ROBIN ? "rr" :
      (target->policy == SCSI_PATH_MRU ? "mru" : "fixed"));
   for (path = target->paths; path != NULL; path = path->next) {
      Proc_Printf(page, len, "  %s:%d:%d %s%s%s\n", path->adapter->name,
		     path->id, path->lun,
		     path->state == SCSI_PATH_ON ? "on" : (
#if defined(VMX86_DEVEL) || defined(VMX86_DEBUG)
		     path->state == SCSI_PATH_STANDBY ? "standby" :
#else
		     path->state == SCSI_PATH_STANDBY ? "on" :
#endif
		     (path->state == SCSI_PATH_OFF ? "off" : "dead")),
		     path == target->activePath ? "*" : "",
		     path == target->preferredPath ? "#" : "");
   }
   Proc_Printf(page, len, "\nActive: %d  Queued: %d\n", target->active,
      target->qcount); 

   SP_Unlock(&adapter->lock);
   SCSI_ReleaseTarget(target, TRUE);

exit:
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * ScsiProcTargWrite --
 *
 *       Assert fails when the magic sequence "crash me 0123456789" is
 *       is written to the proc node
 *
 * Results: 
 *	none
 *
 * Side effects:
 *	none
 *
 *----------------------------------------------------------------------
 */
static int
ScsiProcTargWrite(Proc_Entry  *entry,
		  char        *page,
		  int         *lenp)
{
   SCSI_Adapter *adapter;
   uint32 targetID;
   uint32 lun;
   SCSI_Target *target;
   char *p;
   int status;

   adapter = entry->parent->private;
   targetID = (((int) (entry->private))>>16)&0x00ffff;
   lun = ((int) (entry->private))&0x00ffff;

   SP_Lock(&adapter->lock);
   target = SCSI_FindTarget(adapter, targetID, lun, FALSE);
   if (target == NULL) {
      SP_Unlock(&adapter->lock);
      return VMK_INVALID_HANDLE;
   }
   p = page;
   status = SCSIParsePathCommand(target, p);
   SCSI_ReleaseTarget(target, FALSE);
   SP_Unlock(&adapter->lock);
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * SCSI_AdapProcInfo --
 *
 *      Call the proc node handler for an adapter to fulfill a proc read/write 
 *      on the console.
 *
 * Results: 
 *      VMK_INVALID_ADAPTER if said adapter doesn't exist.
 *      Return value of the vmklinux/adapter proc handler.	
 *
 * Side effects:
 *      None.	
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus 
SCSI_AdapProcInfo(char* adapName, // IN: adapter to operate on
                  char* buf,      // IN/OUT: result buffer
                  uint32 offset,  // IN: offset in to proc file
                  uint32 count,   // IN: how many bytes to read/write
		  uint32* nbytes,  // OUT: how many actually read/written
		  int isWrite)    // IN: whether write or read 
{
   SCSI_Adapter *adapter;
   VMK_ReturnStatus status;

   SP_Lock(&scsiLock);
   adapter = SCSIFindAdapter(adapName);
   if (adapter == NULL) {
      SP_Unlock(&scsiLock);
      Warning("Unknown adapter %s", adapName);
      return VMK_INVALID_ADAPTER;
   }
   SP_Unlock(&scsiLock);
   status = adapter->procInfo(adapter->clientData, buf, offset, count,
			      nbytes, isWrite);
   LOG(2, "status=%d, nbytes=%d", status, *nbytes);  
   return status;
}


/*
 *-----------------------------------------------------------------------------
 *
 * SCSI_GetCmplMapIndex --
 *
 *      Return index into the shared area completion bitmaps for the shared
 *      adapter corresponding to "handleID."
 *
 * Results:
 *      Index. 
 *
 * Side effects:
 *      None. 
 *
 *-----------------------------------------------------------------------------
 */

int16
SCSI_GetCmplMapIndex(SCSI_HandleID handleID)
{
   SCSI_Handle *handle;

   handle = SCSI_HandleFind(handleID);
   if (handle == NULL) {
      Warning("Couldn't find handle 0x%x", handleID);
      return -1;
   } else {
      int16 index;
      Atomic_uint32 *bp = handle->adapter->cosCmplBitmapPtr;

      ASSERT((bp >= scsiCmplBitmaps) && 
             (bp <= &scsiCmplBitmaps[MAX_SCSI_ADAPTERS -1]));
      index = bp - scsiCmplBitmaps; 
      SCSI_HandleRelease(handle);
      return index;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * SCSI_HostVMK* --
 *
 *      Notify vmnixmod about un/register of vmkernel devices. 
 *
 * Results: 
 *      None.
 *
 * Side effects:
 *      None.	
 *
 *----------------------------------------------------------------------
 */

void
SCSI_HostVMKSCSIHost(SCSI_Adapter *adapter, // IN: 
                     const char* procName,  // IN: specified in driver template
                     Bool reg)              // IN: register/unregister
{
   Host_VMnixVMKDev(VMNIX_VMKDEV_SCSI, adapter->name, procName, NULL, 0, reg);
}

void
SCSI_HostVMKBlockDevice(SCSI_Adapter *adapter, // IN: 
                        const char *name,      // IN: device name
                        const char *majName,   // IN: major name
                        uint16 major,          // IN: major # of device on host
                        uint16 minorShift,     // IN: minor -> targetID conv.
                        Bool reg)              // IN: register/unregister
{
   Host_VMnixVMKDev(VMNIX_VMKDEV_BLOCK, adapter->name, name, majName,
                    ((major << 16) | (minorShift & 0xffff)), reg);
}

void
SCSI_HostVMKCharDevice(const char *name, // IN: device name. 
                       uint32 major,     // IN: major # of this device on host
                       Bool reg)         // IN: register/unregister
{
   Host_VMnixVMKDev(VMNIX_VMKDEV_CHAR, name, NULL, NULL, major, reg);
}

void
SCSI_HostVMKMknod(const char *name,   // IN: device name. 
                  const char *parent, // IN: parent dir's name
                  uint32 devNo,       // IN: kdev_t
                  Bool reg)           // IN: register/unregister
{
   Host_VMnixVMKDev(VMNIX_VMKDEV_MKNOD, name, parent, NULL, devNo, reg);
}

/*
 * Return the class of a SCSI target, given an open SCSI handle for
 * the target.
 */
uint32
SCSI_GetTargetClass(SCSI_HandleID handleID)
{
   SCSI_Handle *handle;

   handle = SCSI_HandleFind(handleID);
   if (handle == NULL) {
      Warning("Couldn't find handle 0x%x", handleID);
      return SCSI_CLASS_UNKNOWN;
   } else {
      uint8 class;
      class = handle->target->devClass;
      SCSI_HandleRelease(handle);
      return class;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * SCSI_HostIoctl
 *
 *      Handle an ioctl from the Service Console: forward it to the driver.
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
SCSI_HostIoctl(SCSI_HandleID handleID,  // IN: handle on device
               uint32 hostFileFlags,    // IN: file flags for device on host
               uint32 cmd,              // IN: ioctl cmd
               uint32 userArgsPtr,      // IN/OUT: ptr to COS user space args 
               int32 *result)           // OUT: ioctl result
{
   SCSI_Handle *handle;
   VMK_ReturnStatus status;

   handle = SCSI_HandleFind(handleID);
   if (handle == NULL) {
      Warning("handle 0x%x not found", handleID);
      return VMK_INVALID_HANDLE; 
   }

   LOG(2, "hid=0x%x, cmd=0x%x flags=0x%x uargs=%p", 
       handleID, cmd, hostFileFlags, (void*)userArgsPtr); 
   status = handle->adapter->ioctl(handle->adapter->clientData,
                                   handle->target->id, 
                                   handle->target->lun,
                                   hostFileFlags,
                                   cmd,
                                   userArgsPtr,
                                   result);
   SCSI_HandleRelease(handle);
   return status;
}


/*
 *-----------------------------------------------------------------------------
 *
 *  SCSI_Un/RegisterCharDevIoctl --
 *
 *      vmklinux registers the ioctl handler for char devices. 
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

void
SCSI_RegisterCharDevIoctl(SCSICharDevIoctlFn ioctlFn) // IN: callback 
{
   scsiCharDevIoctl = ioctlFn;   
}

void
SCSI_UnregisterCharDevIoctl(void) 
{
   scsiCharDevIoctl = NULL;   
}

/*
 *-----------------------------------------------------------------------------
 *
 *  SCSI_HostCharDevIoctl --
 *
 *      Ioctl on a char device registered by a vmkernel driver. Forward the call
 *      to the driver (via vmklinux).
 *
 * Results:
 *      VMK_ReturnStatus from vmklinux, or
 *      VMK_NOT_SUPPORTED - no ioctl handler registered.
 *
 * Side effects:
 *      None. 
 *
 *-----------------------------------------------------------------------------
 */

VMK_ReturnStatus
SCSI_HostCharDevIoctl(uint32 major,         // IN: 
                      uint32 minor,         // IN:
                      uint32 hostFileFlags, // IN: file flags for device on host
                      uint32 cmd,           // IN: ioctl cmd
                      uint32 userArgsPtr,   // IN/OUT: ptr to COS user space args 
                      int32 *result)        // OUT: ioctl result
{
   VMK_ReturnStatus status;

   LOG(1, "M=%d m=%d flags=0x%x cmd=0x%x uargs=0x%x",
       major, minor, hostFileFlags, cmd, userArgsPtr);
   if (!scsiCharDevIoctl) {
      return VMK_NOT_SUPPORTED;
   }
   status = scsiCharDevIoctl(major, minor, hostFileFlags, cmd, userArgsPtr, result);
   return status;
}

/*
 * Reserve or release a physical disk.
 */
VMK_ReturnStatus
SCSI_ReservePhysTarget(SCSI_HandleID handleID, Bool reserve)
{
   SCSI_Handle *handle;
   SCSI_Command *cmd;
   SCSIReserveCmd *rCmd;
   VMK_ReturnStatus status;

   handle = SCSI_HandleFind(handleID);
   if (handle == NULL) {
      Warning("Couldn't find handle 0x%x", handleID);
      return VMK_INVALID_HANDLE;
   }

   cmd = (SCSI_Command *)Mem_Alloc(sizeof(SCSI_Command));
   ASSERT_NOT_IMPLEMENTED(cmd != NULL);
   memset(cmd, 0, sizeof(SCSI_Command));
   cmd->type = SCSI_QUEUE_COMMAND;

   rCmd = (SCSIReserveCmd *)cmd->cdb;
   rCmd->opcode = reserve ? SCSI_CMD_RESERVE_UNIT : SCSI_CMD_RELEASE_UNIT;
   cmd->cdbLength = sizeof(SCSIReserveCmd);
   cmd->dataLength = 0; // No data is transferred in response to this command

   SP_Lock(&handle->adapter->lock);
   if (reserve) {
      handle->target->pendingReserves++;
   }
   SP_Unlock(&handle->adapter->lock);

   status = SCSISyncCommand(handle, cmd, NULL, FALSE);

   SP_Lock(&handle->adapter->lock);
   if (reserve) {
      handle->target->pendingReserves--;
   }
   SP_Unlock(&handle->adapter->lock);

   Mem_Free(cmd);
   SCSI_HandleRelease(handle);
   return status;
}

/*
 * Setup cmd and token for a reset command.
 */
void
SCSISetupResetCommand(SCSI_Handle *handle, SCSI_Command *cmd,
		      SCSI_ResultID *ridp)
{
   memset(cmd, 0, sizeof(SCSI_Command));
   cmd->type = SCSI_RESET_COMMAND;

   SCSIInitResultID(handle, NULL, ridp);
   SP_Lock(&handle->adapter->lock);      
   handle->serialNumber++;
   cmd->serialNumber = handle->serialNumber;
   SP_Unlock(&handle->adapter->lock);   
   cmd->originSN = cmd->serialNumber;
   cmd->originHandleID = handle->handleID;
   ridp->serialNumber = cmd->serialNumber;
}

/*
 * Do a hard reset on a physical bus or a LUN.
 */
VMK_ReturnStatus
SCSI_ResetPhysBus(SCSI_HandleID handleID, Bool lunreset)
{
   SCSI_Handle *handle;
   SCSI_Command cmd;
   SCSI_ResultID rid;
   VMK_ReturnStatus status;
   
   handle = SCSI_HandleFind(handleID);
   if (handle == NULL) {
      Warning("Couldn't find handle 0x%x", handleID);
      return VMK_INVALID_HANDLE;
   }
   
   ASSERT(handle->worldID == World_GetVmmLeaderID(MY_RUNNING_WORLD) ||
	  handle->worldID == Host_GetWorldID());
   
   LOG(0, "resetting bus of %s", handle->adapter->name);
   
   SCSISetupResetCommand(handle, &cmd, &rid);
   
   /* User requested a LUN Reset, pass on a flag that tells
    * the driver to use LUN resets instead of the usual full
    * device reset.
    */
   if (lunreset == TRUE) {
      cmd.flags |= SCSI_CMD_USE_LUNRESET;
   }
   
   rid.cmd = &cmd;
   SCSIChoosePath(handle, &rid);

   // The active field is protected by the lock of the target's primary adapter
   SP_Lock(&handle->target->adapter->lock);
   rid.path->active++;
   SP_Unlock(&handle->target->adapter->lock);
   
   status = rid.path->adapter->command(rid.path->adapter->clientData,
				       &cmd, &rid, handle->worldID);
   
   SP_Lock(&handle->target->adapter->lock);
   rid.path->active--;
   SP_Unlock(&handle->target->adapter->lock);
   
   if (status != VMK_OK) {
      struct SCSI_Target *target = handle->target;
      
      Warning("Reset failed on %s:%u:%u:%u, status=0x%x",
	      target->adapter->name, target->id, target->lun,
	      handle->partition, status);
   }

   SCSI_HandleRelease(handle);

   return status;
}
   
/*
 * SCSISyncCommand
 *
 *
 *       Issue a SCSI command to the physical disk specified by handle, and 
 *   wait for the response.  If the *path parameter is not NULL, then force
 *   the command to be issued on the given data path. Note, that if the
 *   command is queued the *path parameter will have no effect. Currently,
 *   the *path parameter is set only in the CheckUnitReady code, and the
 *   request will not be queued. Retry the command if:
 *       - there was a check condition
 *       - the command times out and successfully aborted
 *       - the device queue is full and SCSI_CMD_BYPASSES_QUEUE was set in 
 *         the cmd flags, but SCSI_CMD_RETURN_WOULD_BLOCK was not set
 *   If the useHandleWorldId is TRUE then set
 *   the token resID to the worldID of the handle.  These sync commands are
 *   issued from the path failover code.
 *
 * Results:
 *    VMK_OK on success, error code on failure
 *
 *
 * Side Effects:
 *    none
 */
VMK_ReturnStatus
SCSISyncCommand(SCSI_Handle *handle, SCSI_Command *cmd, SCSI_Path *path, Bool useHandleWorldId)
{
   SCSI_ResultID rid;
   VMK_ReturnStatus status = VMK_IO_ERROR;
   int retries, conflictRetries, errorRetries;
   Timer_AbsCycles maxTime, now;
   Async_Token *token;
   SCSI_RetryStatus rstatus = RSTATUS_NO_RETRY;

   token = Async_AllocToken(0);
   ASSERT_NOT_IMPLEMENTED(token != NULL);
   token->resID = handle->worldID;
   if (World_IsHELPERWorld(MY_RUNNING_WORLD)) {
      token->resID = Host_GetWorldID();
   }
   if (useHandleWorldId){
      token->resID = handle->worldID;
   }
   ASSERT(token->resID == handle->worldID || handle->worldID == hostWorld->worldID);
   ASSERT(path == NULL || (cmd->flags & SCSI_CMD_BYPASSES_QUEUE));

   ASSERT((!(cmd->flags & SCSI_CMD_RETURN_WOULD_BLOCK)) ||
          (cmd->flags & SCSI_CMD_BYPASSES_QUEUE));
   SCSIInitResultID(handle, token, &rid);
   rid.path = path;

   errorRetries = SCSI_ERROR_MAX_RETRIES;
   if (cmd->flags & SCSI_CMD_LOW_LEVEL) {
      /* For low-level (scanning) command, do minimal retries on busy and
       * no retries on reservation conflicts. */
      retries = SCSI_LOW_LEVEL_CMD_MAX_RETRIES;
      conflictRetries = SCSI_LOW_LEVEL_CONFLICT_MAX_RETRIES;
   } else {
      retries = SCSI_BUSY_MAX_RETRIES;
      conflictRetries = CONFIG_OPTION(SCSI_CONFLICT_RETRIES)+1;
   }
   now = Timer_GetCycles();
   maxTime = now + (SCSI_TIMEOUT/1000) * Timer_CyclesPerSecond() * TIMEOUT_RETRIES;
   SCSI_Command *nCmd;
   uint32 size;

   size = sizeof(SCSI_Command);
   if(cmd->sgArr.length > SG_DEFAULT_LENGTH) {
       size += (cmd->sgArr.length - SG_DEFAULT_LENGTH) * sizeof(SG_Elem);
   }
   nCmd = (SCSI_Command *)Mem_Alloc(size);
   ASSERT(nCmd);

   while (retries-- && conflictRetries && errorRetries && now < maxTime) {
      memcpy(nCmd, cmd, size);
      cmd = nCmd;

      SP_Lock(&handle->adapter->lock);      
      handle->serialNumber++;
      cmd->serialNumber = handle->serialNumber;
      SP_Unlock(&handle->adapter->lock);

      cmd->originSN = cmd->serialNumber;
      cmd->originHandleID = handle->handleID;
      token->originSN = cmd->originSN;
      token->originHandleID = cmd->originHandleID;
   
      rid.serialNumber = cmd->serialNumber;

      // Need to get an additional ref on the token and the handle
      // because SCSIIssueCommand will release them
      SCSI_HandleFind(handle->handleID);   
      Async_RefToken(token);
          
      status = SCSIIssueCommand(handle, cmd, &rid);
      if (status == VMK_WOULD_BLOCK) {
         if (cmd->flags & SCSI_CMD_RETURN_WOULD_BLOCK) {
            /*
	     * Return VMK_WOULD_BLOCK to the caller.
	     */
            break;
         } else if (cmd->flags & SCSI_CMD_BYPASSES_QUEUE) {
            /*
	     * Issue command has not queued the request.
             * Try to issue it again.
	     */
            now = Timer_GetCycles();
            continue;
         } else {
	    /* 
             * Issue command has queued the request.
             */
	    status = VMK_OK;
         }
      } else if (status != VMK_OK) {
	 break;
      }
     
      ASSERT(status == VMK_OK);

      /*
       * The command has been issued, wait for it to complete.
       */
      token->cmd = cmd;
      status = SCSI_TimedWait(handle->handleID, token, &rstatus);
      SCSIHandleSyncReservationConflict(handle->handleID, status,
					&conflictRetries, cmd->cdb[0]);
      if (status == VMK_OK || rstatus == RSTATUS_NO_RETRY) { 
         break;
      }
      if (rstatus == RSTATUS_ERROR) {
         errorRetries--;
      }
      now = Timer_GetCycles();
   }

   Async_ReleaseToken(token);
   if (status != VMK_OK && !(cmd->flags & SCSI_CMD_PRINT_NO_ERRORS)) {
      Warning(" returns error: \"%s\". Code: 0x%x.", 
	      VMK_ReturnStatusToString(status), status);
   }
   ASSERT((status != VMK_WOULD_BLOCK) ||
          ((cmd->flags & SCSI_CMD_RETURN_WOULD_BLOCK)));
          
   return status;
}

/*
 * Wait for a SCSI command indicated by token (and issued on handle
 * handleID) to complete.  Abort it if it doesn't complete within
 * SCSI_TIMEOUT seconds.  Decode the error status and determine if the
 * SCSI command needs to be retried (because of a check/busy condition),
 * and return the retryStatus in *rstatus. This will help the caller figure
 * out if and why it needs to retry the command. Also, the caller can pass
 * in the retry status from a previous call to SCSI_TimedWait, to help this
 * function throttle VMK logs.
 */
VMK_ReturnStatus
SCSI_TimedWait(SCSI_HandleID handleID, Async_Token *token,
               SCSI_RetryStatus *rstatus)
{
   SCSI_Result *result;
   VMK_ReturnStatus status;
   SCSI_Handle *handle;
   Timer_Handle th;
   ScsiTimeOut *sgioTimeout;
   int		cmdResultStatus;
   char deviceName[SCSI_DEVICE_NAME_LEN];

#ifdef DELAY_TEST
   static int ioCount = 0;
   int	timeoutTime = 0;
#endif 

   // This is a blocking call
   ASSERT(World_IsSafeToBlock());

waitforabort:
   sgioTimeout = Mem_Alloc(sizeof(ScsiTimeOut));
   ASSERT_NOT_IMPLEMENTED(sgioTimeout != NULL);
   sgioTimeout->token = token;
   sgioTimeout->handleID = handleID;
   sgioTimeout->isRead = TRUE;
  
#ifdef DELAY_TEST
   ioCount++;
   if  ((ioCount % 5000) == 0) {
      cmd->flags |= SCSI_CMD_TIMEDOUT;
      timeoutTime = 120 * 1000;
   } else {
      cmd->flags &= ~SCSI_CMD_TIMEDOUT;
      timeoutTime = SCSI_TIMEOUT; 	/* 40 * 1000 */
   }

   handle = SCSI_HandleFind(handleID);
   if (handle != NULL) { 
      Warning("DELAY_TEST %s:%d:%d:%d ************************* LONG I/O TIME", 
         handle->adapter->name, handle->target->activePath->id, 
	 handle->target->activePath->lun, handle->partition);
      SCSI_HandleRelease(handle);
   } else {
      Warning("DELAY_TEST ************************* LONG I/O TIME");
   }

   th = Timer_Add(MY_PCPU, SCSITimeout, timeoutTime, TIMER_ONE_SHOT,
                  (void *)sgioTimeout);
#else 
   th = Timer_Add(MY_PCPU, SCSITimeout, SCSI_TIMEOUT, TIMER_ONE_SHOT,
                  (void *)sgioTimeout);
#endif
   Async_WaitForIO(token);

   // Remove the timer and free the sgioTimeout if timer never fired. 
   if (Timer_RemoveSync(th)) {
      Mem_Free(sgioTimeout);
   }

   /*
    * We need the originSN and originHandleID for aborts.  We can't use the
    * token->cmd for that because some code paths don't set this field (e.g.
    * SCSISplitSGCommand) and because of a race between the abort and the
    * completion path.
    *
    * Don't check before Async_WaitForIO returns because of a race with
    * SCSIIssueCommand, where these fields are set, for asynchronous IOs
    * with waiters (e.g. the VMFS renew lock).
    */
   ASSERT(token->originSN && token->originHandleID);

   result = (SCSI_Result *)token->result;

   SP_Lock(&token->lock);
   ASSERT(token->flags & (ASYNC_IO_DONE | ASYNC_IO_TIMEDOUT));
   if (token->flags & ASYNC_IO_DONE) {
      cmdResultStatus = result->status;
   } else {
      cmdResultStatus = SCSI_HOST_TIMEOUT << 16;
   }
   token->flags &= ~(ASYNC_IO_TIMEDOUT | ASYNC_IO_DONE);
   SP_Unlock(&token->lock); 

   if (cmdResultStatus == 0) {
      return VMK_OK;
   }

   handle = SCSI_HandleFind(handleID);
   if (handle != NULL) {
      snprintf(deviceName, SCSI_DEVICE_NAME_LEN,"%s:%d:%d:%d ",
             handle->target->activePath->adapter->name, 
	     handle->target->activePath->id,
             handle->target->activePath->lun,
             handle->partition);

      if ((*rstatus != RSTATUS_RESV_CONFLICT) &&
	  !(token->cmd->flags && SCSI_CMD_PRINT_NO_ERRORS)) {
         //XXX should really increment a stat here and have
         //it be visible from /proc/vmware somewhere
	 Log("%s:%d:%d:%d status = %d/%d 0x%x 0x%x 0x%x",
             handle->target->activePath->adapter->name, handle->target->activePath->id,
             handle->target->activePath->lun,
             handle->partition, SCSI_DEVICE_STATUS(cmdResultStatus),
             SCSI_HOST_STATUS(cmdResultStatus),
             result->senseBuffer[2], result->senseBuffer[12],
             result->senseBuffer[13]);

      }
   }
   else {
      Warning("Invalid target");
      *rstatus = RSTATUS_NO_RETRY;
      return VMK_INVALID_TARGET;
   }

   if (SCSI_DEVICE_STATUS(cmdResultStatus) == SDSTAT_RESERVATION_CONFLICT){
      status = VMK_RESERVATION_CONFLICT;
      /* Sleep and then retry a few times if we get a reservation conflict,
       * because another machine may be reserving the disk briefly
       * during FSOpenInt(). */
      *rstatus = RSTATUS_RESV_CONFLICT;
   }
   else if (SCSI_HOST_STATUS(cmdResultStatus) == SCSI_HOST_BUS_BUSY ||
            SCSI_DEVICE_STATUS(cmdResultStatus) == SDSTAT_BUSY ||
	    SCSI_HOST_STATUS(cmdResultStatus) == SCSI_HOST_RESET ) {
      Log("%s Retry (busy)", deviceName);
      CpuSched_Sleep(SCSI_BUSY_SLEEP_TIME);
      status = VMK_BUSY;
      *rstatus = RSTATUS_BUSY;
   }
   else if (SCSIPowerOnOrReset(cmdResultStatus, result->senseBuffer) ||
	    (SCSI_DEVICE_STATUS(cmdResultStatus) == SDSTAT_CHECK &&
	     result->senseBuffer[2] == SCSI_SENSE_KEY_UNIT_ATTENTION &&
	     CONFIG_OPTION(DISK_RETRY_UNIT_ATTENTION))) {
      /* Retry all unit attention sense codes if config variable is set
       * (particularly useful for the IBM FAStT disk array, which can
       * return a bunch of vendor-specific unit attention codes). */
      if ((handle->target->flags & SCSI_DEV_FASTT) &&
          (!(handle->target->flags & SCSI_SUPPORTS_MANUAL_SWITCHOVER)) &&
	  (result->senseBuffer[12] == SCSI_ASC_QUIESCENCE_HAS_BEEN_ACHIEVED) &&
	  (result->senseBuffer[13] == SCSI_ASCQ_QUIESCENCE_HAS_BEEN_ACHIEVED)) {
         Warning("FAStT SAN is path thrashing with another system. Check AVT setting.");
      }
      Log("%s Retry (unit attn)", deviceName);
      status = VMK_IO_ERROR;
      *rstatus = RSTATUS_UNIT_ATTN;
   }
   else if ( (SCSI_DEVICE_STATUS(cmdResultStatus) == SDSTAT_CHECK)  &&
	     (result->senseBuffer[2] == SCSI_SENSE_KEY_ABORTED_CMD)) {
      /*
       * retry aborted cmds
       */
      Log("%s Retry (aborted cmd)", deviceName);
      status = VMK_IO_ERROR;
      *rstatus = RSTATUS_CMD_ABORTED;
   }
   else if (SCSIDeviceNotReady(handle->target, cmdResultStatus, (SCSISenseData *)(result->senseBuffer))) {
        /* 
         * Return a special status code for the NOT_READY condition.
         */
        if (!(token->cmd->flags & SCSI_CMD_PRINT_NO_ERRORS)) {
           Warning("%s not ready", deviceName); 
	}
        if (SCSIDeviceIgnore(handle->target)) {
           status = VMK_OK;
        } else {
           status = VMK_NOT_READY;
           *rstatus = RSTATUS_NO_RETRY;
        }
   }
   else if (SCSI_HOST_STATUS(cmdResultStatus) == SCSI_HOST_TIMEOUT) {
      /*
       * Retry on timeouts, but first send down an abort for the cmd.
       * that timed out. A new command will be reissued by the caller.
       */
      Log("%s Retry (abort after timeout)", deviceName);
      status = SCSIAbortTimedOutCommand(handle, token, deviceName);
      ASSERT((status == VMK_OK) || (status == VMK_ABORT_NOT_RUNNING));
      SCSI_HandleRelease(handle);
      goto waitforabort;
   } else if (SCSI_HOST_STATUS(cmdResultStatus) == SCSI_HOST_ABORT) {
      Log("%s Retry (timedout and aborted)", deviceName);
      status = VMK_INVALID_TARGET;
      *rstatus = RSTATUS_HOST_ABORT;  // retry on timeouts...  
   } else if (SCSIPathDead(handle->target, cmdResultStatus, (SCSISenseData *)(result->senseBuffer))) {
      status = VMK_NO_CONNECT;
      *rstatus = RSTATUS_NO_RETRY; 
   } else if (SCSI_HOST_STATUS(cmdResultStatus) == SCSI_HOST_ERROR) {
      /* errors are a special case, should retry these 3 - 5 times only */
      Log("%s Retry (error)", deviceName);
      status = VMK_IO_ERROR;
      *rstatus = RSTATUS_ERROR; 
   } else if ((SCSI_DEVICE_STATUS(cmdResultStatus) == SDSTAT_CHECK) &&
              (result->senseBuffer[2] == SCSI_SENSE_KEY_DATA_PROTECT) &&
              (result->senseBuffer[12] == SCSI_ASC_WRITE_PROTECTED)) {
      Log("%s Write protected (no retry)", deviceName);
      status = VMK_IO_ERROR;
      *rstatus = RSTATUS_NO_RETRY;
   } else {
      status = VMK_IO_ERROR;
      *rstatus = RSTATUS_NO_RETRY;
   }
   SCSI_HandleRelease(handle);
   return status;
}

/*
 * SCSI_SendRescanEvent() 
 *
 * 	Send a notification to the serverd process to indicate that
 * the number/type of disks attached to the system has changed.
 *
 */
static VMK_ReturnStatus
SCSI_SendRescanEvent(Bool addedDisks)
{
    VmkEvent_VmkUpdateDisksArgs arg;
    
    Log("Disks have been added or removed from the system.");
    arg.newDisks = addedDisks;
    VmkEvent_PostHostAgentMsg(VMKEVENT_UPDATE_DISKS, &arg, sizeof(arg));
    return VMK_OK;
}

/*
 * Rescan adapter for changes in disk configuration. 
 *
 *
 * The linux_scsi.c code will enforce any MaxLUN or MaskLUNs limitations.
 *
 */
VMK_ReturnStatus
SCSI_Rescan(char *adapterName)
{
   SCSI_Adapter *adapter;
   SCSI_Info info;
   SCSI_RescanResultType r;
   uint32 targetID, lun;
   Bool sparseLUNSupport;
   Bool diskConfigHasChanged = FALSE;
   Bool addedNewDisks = FALSE;
   VMK_ReturnStatus status;
   
   SP_Lock(&scsiLock);
   if (rescanInProgress) {
      SP_Unlock(&scsiLock);
      return VMK_BUSY;
   }
   adapter = SCSIFindAdapter(adapterName);
   if (adapter == NULL) {
      SP_Unlock(&scsiLock);
      return VMK_INVALID_ADAPTER;
   }

   if (adapter->rescan == NULL) {
      /* This is no error as block devices leave this field NULL. */
      Log("Adapter %s does not support rescanning", adapter->name);
      SP_Unlock(&scsiLock);
      return VMK_OK;
   }

   if (adapter->moduleID != 0) {
      status = Mod_IncUseCount(adapter->moduleID);
      if (status != VMK_OK) {
         SP_Unlock(&scsiLock);
         Warning("Couldn't increment module count, error %s",VMK_ReturnStatusToString(status)); 
         return VMK_INVALID_ADAPTER;
      }
   }

   rescanInProgress = TRUE;
   SP_Unlock(&scsiLock);

   Log("Starting rescan of adapter %s", adapter->name);

   (adapter->getInfo)(adapter->clientData, 255, 255, &info, NULL, 0);

   for (targetID = 0; targetID < info.maxID; targetID++) {
      sparseLUNSupport = SCSI_SparseLUNSupport(adapter->name, targetID);

      for (lun = 0; lun < info.maxLUN; lun++) {
	 r = (adapter->rescan)(adapter->clientData, targetID, lun);
         if ((r == SCSI_RESCAN_EXISTING_DISK_CHANGED) ||
             (r == SCSI_RESCAN_EXISTING_DISK_REMOVED) ||
             (r == SCSI_RESCAN_EXISTING_DISK_DISAPPEARED_BUT_BUSY)) {
            diskConfigHasChanged = TRUE;
         } else if (r == SCSI_RESCAN_NONEXISTENT_DISK_NOW_EXISTS) {
            diskConfigHasChanged = TRUE;
            addedNewDisks = TRUE;
         }

	 if (((r == SCSI_RESCAN_NONEXISTENT_DISK_NO_CHANGE) || 
	      (r == SCSI_RESCAN_ERROR)) && 
	     (!sparseLUNSupport)) {
            /*
	     * quit if the lun is missing and there is no sparse lun support
	     */
	    break;
	 }
      }
   }
      
   SP_Lock(&scsiLock);
   rescanInProgress = FALSE;
   CpuSched_Wakeup((uint32)&rescanInProgress);
   SP_Unlock(&scsiLock);
   Log("Finished rescan of adapter %s", adapter->name);

   // We may have seen some new paths, so check their state.
   SCSI_StateChange(adapterName);

   if (diskConfigHasChanged) {
      SCSI_SendRescanEvent(addedNewDisks);
   }

   if (adapter->moduleID != 0) {
      Mod_DecUseCount(adapter->moduleID);
   }
   return VMK_OK;
}

/*
 * Increment stats for a command that is being issued.
 * Stats to update: adapter, target, partition/fs, world on target's schedQ
 * For better accuracy of issueTSC this function should be called
 * just before the command is sent to driver.
 */
void
SCSI_UpdateCmdStats(SCSI_Command *cmd, SCSI_ResultID *rid, World_ID worldID)
{
   SCSI_Target *target = rid->path->target;
   SCSI_Adapter *adapter = target->adapter;
   SCSI_SchedQElem *sPtr;
   SCSI_Partition *partition;

   /* Get the lock for the primary path to the target */
   SP_Lock(&adapter->lock);

#ifdef SCSI_SCHED_NODBW
   worldID = Host_GetWorldID();
#endif

   sPtr = SCSISchedQFind(target, worldID);
   if (!sPtr) {
      goto exit;
   }

   // Adapter and world/target stats include control commands.
   adapter->stats.commands++;
   target->stats.commands++;
   sPtr->stats.commands++;
   if (cmd->type == SCSI_ABORT_COMMAND) {
      adapter->stats.aborts++;
      target->stats.aborts++;
      sPtr->stats.aborts++;
      goto exit;
   }
   if (cmd->type == SCSI_RESET_COMMAND) {
      adapter->stats.resets++;
      target->stats.resets++;
      sPtr->stats.resets++;
      goto exit;
   }

   // Only data commands for partition stats.
   ASSERT(rid);
   partition = &target->partitionTable[rid->partition];
   partition->stats.commands++;

   if (rid->token->callback == SplitAsyncDone) {
      SCSISplitChildInfo *childInfo = (SCSISplitChildInfo *)rid->token->callerPrivate;
      if (childInfo->paeCopySG) {
         adapter->stats.paeCopies++;
         target->stats.paeCopies++;
         partition->stats.paeCopies++;
         sPtr->stats.paeCopies++;
         if (childInfo->cIndex == 0) {
            // count once for the original command
            adapter->stats.paeCmds++;
            target->stats.paeCmds++;
            partition->stats.paeCmds++;
            sPtr->stats.paeCmds++;
         }
      } else {
         adapter->stats.splitCopies++;
         target->stats.splitCopies++;
         partition->stats.splitCopies++;
         sPtr->stats.splitCopies++;
         if (childInfo->cIndex == 0) {
            // count once for the original command
            adapter->stats.splitCmds++;
            target->stats.splitCmds++;
            partition->stats.splitCmds++;
            sPtr->stats.splitCmds++;
         }
      }
   }

   switch (cmd->cdb[0]) {
      case SCSI_CMD_READ10: {
	 SCSIReadWrite10Cmd *rwCmd = (SCSIReadWrite10Cmd *)cmd->cdb;
	 uint32 length = ByteSwapShort(rwCmd->length);
	 adapter->stats.blocksRead += length;
	 target->stats.blocksRead += length;
	 partition->stats.blocksRead += length;
	 sPtr->stats.blocksRead += length;
         adapter->stats.readOps++;
         target->stats.readOps++;
         partition->stats.readOps++;
         sPtr->stats.readOps++;
	 break;
      }
      case SCSI_CMD_WRITE10: {      
	 SCSIReadWrite10Cmd *rwCmd = (SCSIReadWrite10Cmd *)cmd->cdb;
	 uint32 length = ByteSwapShort(rwCmd->length);
	 adapter->stats.blocksWritten += length;
	 target->stats.blocksWritten += length;
	 partition->stats.blocksWritten += length;
	 sPtr->stats.blocksWritten += length;
         adapter->stats.writeOps++;
         target->stats.writeOps++;
         partition->stats.writeOps++;
         sPtr->stats.writeOps++;
	 break;
      }
      case SCSI_CMD_READ6: {
	 uint8* rw = (uint8*) cmd->cdb;
	 uint32 length = ((rw[4] == 0 ? 256 : rw[4]));
	 adapter->stats.blocksRead += length;
	 target->stats.blocksRead += length;
	 partition->stats.blocksRead += length;
	 sPtr->stats.blocksRead += length;
         adapter->stats.readOps++;
         target->stats.readOps++;
         partition->stats.readOps++;
         sPtr->stats.readOps++;
	 break;
      }
      case SCSI_CMD_WRITE6: {
	 uint8* rw = (uint8*) cmd->cdb;
	 uint32 length = ((rw[4] == 0 ? 256 : rw[4]));
	 adapter->stats.blocksWritten += length;
	 target->stats.blocksWritten += length;
	 partition->stats.blocksWritten += length;	 
	 sPtr->stats.blocksWritten += length;
         adapter->stats.writeOps++;
         target->stats.writeOps++;
         partition->stats.writeOps++;
         sPtr->stats.writeOps++;
	 break;
      }
      case SCSI_CMD_READ16: {
	 SCSIReadWrite16Cmd *rwCmd = (SCSIReadWrite16Cmd *)cmd->cdb;
	 uint32 length = ByteSwapLong(rwCmd->length);
	 adapter->stats.blocksRead += length;
	 target->stats.blocksRead += length;
	 partition->stats.blocksRead += length;
	 sPtr->stats.blocksRead += length;
         adapter->stats.readOps++;
         target->stats.readOps++;
         partition->stats.readOps++;
         sPtr->stats.readOps++;
	 break;
      }
      case SCSI_CMD_WRITE16: {      
	 SCSIReadWrite16Cmd *rwCmd = (SCSIReadWrite16Cmd *)cmd->cdb;
	 uint32 length = ByteSwapLong(rwCmd->length);
	 adapter->stats.blocksWritten += length;
	 target->stats.blocksWritten += length;
	 partition->stats.blocksWritten += length;
	 sPtr->stats.blocksWritten += length;
         adapter->stats.writeOps++;
         target->stats.writeOps++;
         partition->stats.writeOps++;
         sPtr->stats.writeOps++;
	 break;
      }
   }

exit:
   SP_Unlock(&adapter->lock);
}

/*
 * Increment latency stats for a command that just completed
 * Three sets of stats, one for the adapter, one for the partition/fs,
 * and one for the world/target
 */
static void
SCSIUpdateCmdLatency(SCSI_Target *target, SCSI_Handle *handle,
                     Async_Token *token)
{
   SCSI_SchedQElem *sPtr;
   int64 diff;

   ASSERT(SP_IsLocked(&target->adapter->lock));
   diff = RDTSC() - token->startTSC;
   if (diff < 0) {
      diff = 0;
   }

   if (handle == NULL) {
      return;
   }

   sPtr = SCSISchedQFind(target, token->resID);
   if (!sPtr) {
      return;
   }

   if (token->issueTSC != 0) {
      SCSI_Partition *partition = &target->partitionTable[handle->partition];
      SCSI_Adapter *adapter = target->adapter;

      sPtr->stats.totalTime += diff;
      partition->stats.totalTime += diff;
      adapter->stats.totalTime += diff;
      target->stats.totalTime += diff;
      sPtr->stats.issueTime += token->issueTSC - token->startTSC;
      partition->stats.issueTime += token->issueTSC - token->startTSC;
      adapter->stats.issueTime += token->issueTSC - token->startTSC;
      target->stats.issueTime += token->issueTSC - token->startTSC;
   }
}

/*
 * Utility routines to print proc info.
 */
void
SCSIProcPrintHdr(char* buffer, 
                 int* lenp) 
{
   Proc_Printf(buffer, lenp, "%10s %10s %10s %10s %10s %10s %10s %10s %10s %10s %12s %10s %10s",
               "cmds", "reads", "KBread", "writes", "KBwritten", "cmdsAbrt", 
               "busRst", "paeCmds", "paeCopies", "splitCmds", "splitCopies", 
               "issueAvg", "totalAvg");
}

void
SCSIProcPrintStats(SCSI_Stats *stats,
                   char* buffer, 
                   int* lenp) 
{
   // the max here avoids div by zero errors
   uint64 divCmds = MAX(stats->commands, 1);
   Proc_Printf(buffer, lenp, "%10u %10u %10u %10u %10u %10u %10u %10u %10u %10u  %12u %10u %10u",
               stats->commands, stats->readOps, stats->blocksRead/2,
               stats->writeOps, stats->blocksWritten/2, stats->aborts,
               stats->resets, stats->paeCmds, stats->paeCopies,
               stats->splitCmds, stats->splitCopies,
               (uint32)(stats->issueTime/divCmds),
               (uint32)(stats->totalTime/divCmds));
}

#define SCSI_DEFAULT_QUEUE_DEPTH 16

/*
 * Send a synchronous SCSI command to a (id, lun) that does not currently
 * have a SCSI_Target data structure in the vmkernel.  Create the
 * necessary data structures and then destroy afterwards.  Use the
 * appropriate flags so that there is no failover or disk bandwidth
 * processing.
 */
VMK_ReturnStatus
SCSI_SendCommand(SCSI_Adapter *adapter, uint32 id, uint32 lun,
		 uint8 *cdb, int len, char *scsiResult, int resultLen)
{
   SCSI_Target *target;
   SCSI_Path *path;
   SCSI_Handle *handle;
   VMK_ReturnStatus status;
   SCSI_Command *cmd;

   target = Mem_Alloc(sizeof(SCSI_Target));
   ASSERT(target);
   memset(target, 0, sizeof(SCSI_Target));
   target->adapter = adapter;
   target->id = id;
   target->lun = lun;
   /* Need some queue depth >=1 so a command can be issued. */
   target->curQDepth = SCSI_DEFAULT_QUEUE_DEPTH;
   target->refCount = 1;
   
   target->partitionTable = Mem_Alloc(sizeof(SCSI_Partition));
   ASSERT(target->partitionTable);
   memset(target->partitionTable, 0, sizeof(SCSI_Partition));
   target->numPartitions = 1;

   path = Mem_Alloc(sizeof(SCSI_Path));
   ASSERT(path);
   memset(path, 0, sizeof(SCSI_Path));
   path->adapter = adapter;
   path->id = id;
   path->lun = lun;
   path->state = SCSI_PATH_ON;
   path->target = target;
   target->activePath = path;
   target->paths = path;

   SP_Lock(&scsiLock);
   handle = SCSIAllocHandleTarg(target, Host_GetWorldID(), 0);
   ASSERT(handle);
   SP_Unlock(&scsiLock);

   cmd = Mem_Alloc(sizeof(SCSI_Command));
   ASSERT(cmd);
   memset(cmd, 0, sizeof(SCSI_Command));
   
   cmd->sgArr.length = 1;
   cmd->sgArr.addrType = SG_MACH_ADDR;
   cmd->sgArr.sg[0].addr = VMK_VA2MA((VA)scsiResult);
   cmd->sgArr.sg[0].length = resultLen;
   cmd->cdbLength = len;
   cmd->dataLength = 0;
   // This avoids path processing like marking the path dead and disk
   // scheduling processing.
   cmd->flags = SCSI_CMD_LOW_LEVEL | SCSI_CMD_PRINT_NO_ERRORS |
      SCSI_CMD_IGNORE_FAILURE | SCSI_CMD_BYPASSES_QUEUE;
   memcpy(cmd->cdb, cdb, len);

   status = SCSISyncCommand(handle, cmd, path, FALSE);
   ASSERT(status != VMK_WOULD_BLOCK);
   SCSIHandleDestroy(handle);
   Mem_Free(cmd);
   ASSERT(target->schedQ == NULL);
   Mem_Free(target->partitionTable);
   Mem_Free(target);
   Mem_Free(path);
   return status;
}

/*
 *-----------------------------------------------------------------------------
 *
 * SCSIFindLBNDataInSG --
 *    Given a SG Array, find the SG_Elem corresponding to a sector
 *    starting given absolute 'lbnInBytes' on disk. 'sgOffset' is the
 *    absolute starting disk offset for sgArr. Make the data in this
 *    SG_Elem available in a buffer.
 *
 * Results:
 *    Pointer to a sector-sized data region starting 'lbn', if it is
 *    a part of any SG_Elem(s) of the SG_Array. NULL if no such SG_Elem
 *    is found.
 *
 * Side effects:
 *    None.
 *
 *-----------------------------------------------------------------------------
 */
static uint8*
SCSIFindLBNDataInSG(const SG_Array *sgArr,
                    uint64 sgOffset, uint64 lbnInBytes)
{
   int i = 0;
   uint64 curOffset;
   uint32 sgElemOffset;
   uint8 *data = NULL;

   //lbnInBytes should be a multiple of disk sector size
   ASSERT(lbnInBytes % 512 == 0);
   
   curOffset = sgOffset;
   for (i = 0; i < sgArr->length; i++) {


      if (curOffset + sgArr->sg[i].length <= lbnInBytes) {
         curOffset += sgArr->sg[i].length;
         continue;
      }

      data = Mem_Alloc(512);
      if (data == NULL) {
         break;
      }

      sgElemOffset = (uint32)(lbnInBytes - curOffset);
      ASSERT(sgElemOffset + 512 <= sgArr->sg[i].length);
      switch (sgArr->addrType) {
      case SG_MACH_ADDR:
      {
         KSEG_Pair *pair;
         void *vaddr;
         
         vaddr = Kseg_GetPtrFromMA((MA)sgArr->sg[i].addr + (MA)sgElemOffset,
                                   PAGE_SIZE, &pair);
         if (vaddr == NULL) {
            Mem_Free(data);
            Warning("Failed to map MPN");
            return NULL;
         }
         memcpy(data, vaddr, 512);
         Kseg_ReleasePtr(pair);
         break;
      }
      case SG_VIRT_ADDR:
      {
         uint8 *sgData = (uint8 *)((VA)sgArr->sg[i].addr);
         memcpy(data, &sgData[sgElemOffset], 512);
         break;
      }
      default:
         NOT_REACHED();
      }
      break;
   }
   return data;
}

INLINE void
SCSIReleaseLBNDataInSG(uint8 *data)
{
   ASSERT(data);
   Mem_Free(data);
}

/*
 *-----------------------------------------------------------------------------
 *
 * SCSIWillClobberActivePTable --
 *    Check if the SCSI command 'cmd' would overwrite an *active*
 *    partition entry on the target corresponding to the open
 *    SCSI handle 'handle'. The function does 2 things:
 *    1. Checks if the command will overwrite any partition entry in the
 *       primary, extended, or nested-extended partition table.
 *    2. Checks if there are any active partitions on the target.
 * 
 *    Assumes that 'handle' (and in turn, target) is locked.
 *
 *    This function implicitly guards against the possibility that the host
 *    opens a handle to an extended partition and then tries to clobber
 *    it. In the future, opens to extended partitions will be trapped in
 *    SCSI_OpenDevice. Anyways, this possibility is limited to block
 *    devices.
 *
 * Results:
 *    TRUE if 'cmd' will clobber an active partition table.    
 *
 * Side effects:
 *    None.
 *
 *-----------------------------------------------------------------------------
 */
static Bool
SCSIWillClobberActivePTable(const SCSI_Handle *handle, 
                            const SCSI_Command *cmd)
{
   int i;
   uint64 blockOffset;
   uint32 numBlocks;
   Bool foundActivePartition = FALSE;
   Bool overlapsPTable = FALSE;

   ASSERT(handle);
   ASSERT(cmd);

   if (cmd->cdb[0] == SCSI_CMD_WRITE10) {
      SCSIReadWrite10Cmd *rwCmd = (SCSIReadWrite10Cmd *)cmd->cdb;

      blockOffset = ByteSwapLong(rwCmd->lbn);
      numBlocks = ByteSwapShort(rwCmd->length);
   } else if (cmd->cdb[0] == SCSI_CMD_WRITE6) {
      uint8* rw = (uint8*) cmd->cdb;

      blockOffset = (((rw[1]&0x1f)<<16)|(rw[2]<<8)|rw[3]);
      numBlocks = ((rw[4] == 0 ? 256 : rw[4]));
   } else {
      return FALSE;
   }

   /* Calculate absolue LBN on LUN */
   blockOffset += 
      handle->target->partitionTable[handle->partition].entry.startSector;
   LOG(7, "Write %u blocks starting LBN %"FMT64"u",
       numBlocks, blockOffset);

   for (i = 1; i < handle->target->numPartitions; i++) {
      SCSI_Partition *sp = &(handle->target->partitionTable[i]);
      /* Scan through the partition table looking for
       * 1. IO overlap with partition table entry
       * 2. active partitions
       */
      if (!overlapsPTable && blockOffset < sp->entry.ptableLBN + 1 &&
          blockOffset + numBlocks > sp->entry.ptableLBN) {
         /* Write clobbers primary, extended or nested-extended
          * partition table. This check should ignore any writes before
          * SCSI_PTABLE_SECTOR_OFFSET that don't really change existing
          * partition entries in the MBR.
          */
         LOG(2, "Write to ptable (starting %u) that stores entry %d at %d",
             sp->entry.ptableLBN, sp->entry.number, sp->entry.ptableIndex);
         ASSERT(sp->entry.ptableLBN >=
                handle->target->partitionTable[handle->partition].entry.startSector);
         
         if (sp->entry.ptableLBN == 0) {
            /* MBR write. Needs fine grained checks. */
            uint8 *ptableSector = NULL;

            ptableSector = SCSIFindLBNDataInSG(&cmd->sgArr, 
                                               (uint64)blockOffset * 512,
                                               (uint64)sp->entry.ptableLBN * 512);
            if (ptableSector) {
               uint8 j = sp->entry.ptableIndex;
               Partition *p = SCSI_FIRSTPTABLEENTRY(ptableSector);
               
               ASSERT(j < 4);
               if (p[j].firstSector + sp->entry.ptableLBN != sp->entry.startSector ||
                   p[j].numSectors != sp->entry.numSectors ||
                   p[j].type != sp->entry.type) {
                  LOG(2, "Attempt to overwrite entry %d, type %#x",
                      j, p[j].type);
                  LOG(3, "Old: %u %u %#x. New: %u %u %#x.",
                      sp->entry.startSector, sp->entry.numSectors, sp->entry.type,
                      p[j].firstSector, p[j].numSectors, p[j].type);
                  overlapsPTable = TRUE;
                  if (foundActivePartition) {
                     SCSIReleaseLBNDataInSG(ptableSector);
                     return TRUE;
                  }
               }
               SCSIReleaseLBNDataInSG(ptableSector);
               ptableSector = NULL;
            }
         } else {
            /* This write overlaps an extended or nested-extended partition
             * table. So don't worry about granularity here. Also, it
             * becomes too complicated since logical partitions inside
             * nested-extended partitions may be destroyed by writing to
             * any one of the ancestor (nested-)extended partitions.
             */
            overlapsPTable = TRUE;
            if (foundActivePartition) {
               return TRUE;
            }           
         }
      }
      if ((sp->entry.type == VMK_PARTITION_TYPE ||
           sp->entry.type == VMK_DUMP_PARTITION_TYPE) &&
          (sp->nReaders > 0 || sp->nWriters > 0)) {
         foundActivePartition = TRUE;
         if (overlapsPTable) {
            LOG(2, "Active partn %d, type %#x, nR = %d, nW = %d",
                sp->entry.number, sp->entry.type,
                sp->nReaders, sp->nWriters);
            return TRUE;
         }
      }
   }
   return FALSE;
}

/*
 *-----------------------------------------------------------------------------
 *
 * SCSI_IsHandleToPAEAdapter --
 *    Wrapper around SCSIAdapterIsPAECapable() for modules that don't
 *    understand/can't access the SCSI_Handle and SCSI_Adapter structs.
 *    Query an open device handle on whether it is PAE capable.
 *
 * Results:
 *    TRUE is the handle belongs to an adapter that is PAE capable, FALSE
 *    otherwise.
 *
 *-----------------------------------------------------------------------------
 */
Bool
SCSI_IsHandleToPAEAdapter(SCSI_HandleID handleID)
{
   SCSI_Handle *handle;
   Bool retval;

   handle = SCSI_HandleFind(handleID);
   if (handle == NULL) {
      return FALSE;
   }
   retval = SCSIAdapterIsPAECapable(handle->adapter);
   SCSI_HandleRelease(handle);
   return retval;
}

/*
 *-----------------------------------------------------------------------------
 *
 *  Add, Get, Free COSLunList --
 *
 *      Routines to deal with list of COS recognized luns on a given adapter.
 *      Entries are single-use; COS Adds, linux_scsi Gets and Frees during
 *      adapter scan. Therefore, there is unlikely to be more than a few entries
 *      in the list at any time.
 *
 * Results:
 *       
 *
 * Side effects:
 *      
 *
 *-----------------------------------------------------------------------------
 */

VMK_ReturnStatus
SCSI_AddCOSLunList(uint16 bus, uint16 devfn, uint32 *tgtLuns, uint16 numLuns)
{
   COSLunList *ent; 

   ent = Mem_Alloc(sizeof(COSLunList));
   if (!ent) {
      return VMK_NO_MEMORY;
   }
   memset(ent, 0, sizeof(COSLunList));
   ent->bus = bus;
   ent->devfn = devfn;
   ent->numTgtLuns = numLuns;
   if (numLuns > 0) {
      ASSERT(tgtLuns != NULL);
      ent->tgtLunList = Mem_Alloc(numLuns * sizeof(uint32));
      if (!ent->tgtLunList) {
         Mem_Free(ent);
         return VMK_NO_MEMORY;
      }
      CopyFromHost(ent->tgtLunList, tgtLuns, numLuns * sizeof(uint32));
   }

   SP_Lock(&cosLunListLock);
   Log("Adding %d COS-recognized luns to %#x:%#x.", numLuns, bus, devfn);
   ent->next = cosLunListHead;
   cosLunListHead = ent;
   SP_Unlock(&cosLunListLock);
   return VMK_OK;
}

COSLunList*
SCSI_GetCOSLunList(SCSI_Adapter *adapter)
{
   COSLunList *curr, **prev;

   SP_Lock(&cosLunListLock);
   for (prev = &cosLunListHead, curr = cosLunListHead; curr; 
        prev = &curr->next, curr = curr->next) {
      if (curr->bus == adapter->bus && curr->devfn == adapter->devfn) {
         Log("Found COS-recognized luns for %x:%x.", adapter->bus, adapter->devfn);
         *prev = curr->next; 
         curr->next = NULL;
         break;
      }
   }
   SP_Unlock(&cosLunListLock);
   return curr;
}

void
SCSI_FreeCOSLunList(COSLunList *list)
{
   ASSERT(list->next == NULL);
   if (list->tgtLunList) {
      Mem_Free(list->tgtLunList);
   }
   Mem_Free(list);
}

VMK_ReturnStatus
SCSI_RescanDevices(void *driverData)
{
   SCSI_Target       *target;
   SCSI_TargetList   *rescanTargetList;
   SCSI_TargetList   *targetListEntry;

   if (driverData != NULL) {
      SCSI_Rescan(driverData);
   }

   /* First remove cached information for ALL adapters and then re-build
    * the cache.
    */
   SP_Lock(&scsiLock);
   if (rescanInProgress) {
      SP_Unlock(&scsiLock);
      return VMK_BUSY;
   }

   if( SCSI_ObtainRegisteredTargetsList(&rescanTargetList) != VMK_OK ) {
      SP_Unlock(&scsiLock);
      return VMK_BUSY;
   }

   rescanInProgress = TRUE;
  

   /* We shouldn't SCSIValidatePartitionTable() here because
    * 1. For unopened FS volumes, SCSI_OpenDevice() (part of FSS_Open)
    *    will call it.
    * 2. For opened FS volumes, the ptable protection mechanism,
    *    SCSIWillClobberActivePTable, will make sure that the ptable is
    *    not modified under the open FS reference. We don't care if
    *    remote ESX servers modify the ptable, to prevent the open FS
    *    volume from becoming a dangling reference.
    */
   //XXX This SCSIValidatePartitionTable() call should be removed.
   for (targetListEntry = rescanTargetList; 
        targetListEntry != NULL;
        targetListEntry = targetListEntry->next) {
      SCSIValidatePartitionTable(targetListEntry->target->adapter, targetListEntry->target);
   }
   SP_Unlock(&scsiLock);

   /* Rebuild VMFS partition information cache */
   for (targetListEntry  = rescanTargetList; 
        targetListEntry != NULL;
        targetListEntry  = targetListEntry->next) {
      int i;
      target = targetListEntry->target; 
      for (i = 0; i < target->numPartitions; i++) {
         if (target->partitionTable[i].entry.type == VMK_PARTITION_TYPE) {
            char volumeName[FS_MAX_VOLUME_NAME_LENGTH];
            
            snprintf(volumeName, sizeof(volumeName), "%s:%d:%d:%d",
                     target->adapter->name, target->id, target->lun, i);
            FSS_Probe(volumeName, TRUE);
         }
      }
   }

   SCSI_FreeRegisteredTargetsList(rescanTargetList);
   SP_Lock(&scsiLock);
   rescanInProgress = FALSE;
   CpuSched_Wakeup((uint32)&rescanInProgress);
   SP_Unlock(&scsiLock);
   return VMK_OK;
}

void
SCSI_RescanFSUpcall(void)
{
   VC_RescanVolumes(SCSI_DISK_DRIVER_STRING, NULL);
}


/*
 *-----------------------------------------------------------------------------
 * SCSI_ObtainRegisteredTargetsList --
 *      Gets the list of targets registered in the system.
 *      Note: scsiLock should be held while calling this function and
 *            SCSI_FreeObtainedTargetsList() should be called once 
 *            you are done with the returned list.
 * Results:
 *      VMK_OK if all is well, VMK_NO_MEMORY_RETRY otherwise. If
 *      VMK_NO_MEMORY_RETRY is returned the caller should re-issue 
 *      this function call  at a later point of time to get the 
 *      list of targets.
 * Side effects:
 *      *targetList is modified to point the start of the 
 *      list and VMK_OK is returned. In case of an error 
 *      *targetList is set to NULL and VMK_NO_MEMORY_RETRY is returned.
 *-----------------------------------------------------------------------------
 */

VMK_ReturnStatus
SCSI_ObtainRegisteredTargetsList(SCSI_TargetList **targetList)
{
   int j;
   SCSI_Adapter *adapter;
   SCSI_Target *target;
   SCSI_TargetList  *prevRegisteredTarget = NULL;
   SCSI_TargetList  *nextRegisteredTarget = NULL;
   VMK_ReturnStatus status;

   *targetList = NULL;
   for (j = 0; j < HASH_BUCKETS; j++) {
      for (adapter = adapterHashTable[j]; adapter != NULL;
	   adapter = adapter->next) {

         SP_Lock(&adapter->lock);
         for (target = adapter->targets; target != NULL;
              target = target->next) {
            if (target == adapter->targets) {
               /*
                * Increment use count if adapter has at least one
                * target to make sure that adapter is not
                * destroyed while we do the scan.
                */
               status = Mod_IncUseCount(adapter->moduleID);
               if (status != VMK_OK) {
                  break; //skip this adapter
               }
            }
            nextRegisteredTarget = (SCSI_TargetList *) 
                                    Mem_Alloc(sizeof(SCSI_TargetList));

            if(nextRegisteredTarget == NULL) {
              LOG(0,"Error!.Unable to allocate memory for SCSI_TargetList entry."\
                    "Freeing allocated target list entries. Retry");
              SP_Unlock(&adapter->lock);
              status = Mod_DecUseCount(adapter->moduleID);
              if (status != VMK_OK) {
                 Warning("Mod_DecUseCount: moduleId = %d status = %x",
                          adapter->moduleID,status);
              }
              SCSI_FreeRegisteredTargetsList(*targetList);
              return VMK_NO_MEMORY_RETRY;
            }

            target->refCount++;
            nextRegisteredTarget->target = target;
            nextRegisteredTarget->next   = NULL;

            if(prevRegisteredTarget == NULL) {
               *targetList    = nextRegisteredTarget;
            } else {
               prevRegisteredTarget->next = nextRegisteredTarget;
            }
            prevRegisteredTarget = nextRegisteredTarget;
         }
         SP_Unlock(&adapter->lock);
      }
   }

   return VMK_OK;
}


/*
 *-----------------------------------------------------------------------------
 * SCSI_FreeRegisteredTargetsList --
 *  
 *      Frees the memory allocated to the list containing the registered
 *      targets. A call to SCSI_ObtainRegisteredTargetsList() usually
 *      precedes this funtion call. 
 * Results:
 *      *registeredTargeList is made to point to NULL and is no longer
 *      reliable.
 * Side effects:
 *      None.      
 *-----------------------------------------------------------------------------
 */

void
SCSI_FreeRegisteredTargetsList(SCSI_TargetList *targetList)
{
   VMK_ReturnStatus status;
   SCSI_TargetList  *nextRegisteredTarget;

   if (targetList) {
      SP_Lock(&targetList->target->adapter->lock);
   }
   while( targetList != NULL) {
      targetList->target->refCount--;
      nextRegisteredTarget  = targetList->next;

      if(nextRegisteredTarget) {
         if (nextRegisteredTarget->target->adapter != 
             targetList->target->adapter) {

            SP_Unlock(&targetList->target->adapter->lock);
            status = Mod_DecUseCount(targetList->target->adapter->moduleID);
            if (status != VMK_OK) {
               Warning("Mod_DecUseCount: moduleId = %d status = %x",
                       targetList->target->adapter->moduleID,
                       status);
            }
            SP_Lock(&nextRegisteredTarget->target->adapter->lock);
         }
      } else {
         SP_Unlock(&targetList->target->adapter->lock);
         status = Mod_DecUseCount(targetList->target->adapter->moduleID);
         if (status != VMK_OK) {
            Warning("Mod_DecUseCount: moduleId = %d status = %x",
                     targetList->target->adapter->moduleID,
                     status);
         }
      }
      Mem_Free(targetList); 
      targetList  = nextRegisteredTarget;
  }
}
