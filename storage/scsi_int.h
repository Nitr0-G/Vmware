/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * scsi_int.h --
 *
 *      Definitions internal to the VMKernel SCSI module.
 */

#ifndef _SCSI_INT_H_
#define _SCSI_INT_H_

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

/* Evaluate path busy retry count */
#define SCSI_EVALUATE_RETRY_COUNT 5

/* Number of buckets in the adapter hash table */
#define HASH_BUCKETS  19

extern SP_SpinLock scsiLock;
extern SCSI_Adapter *adapterHashTable[HASH_BUCKETS]; // protected by scsiLock
extern Bool rescanInProgress;			     // protected by scsiLock

extern uint8 zeroSenseBuffer[];

/*
 * Ranking of some vmkernel locks [fixed by indicated functions]
 *
 * 1 - vscsi handle locks
 * 2 - scsiLock
 * 3 - all adapter locks [SCSI_CreateTarget]
 * 4 - handleArrayLock [SCSI_DoCommandComplete]
 * 4 - all targetList locks [SCSISchedQAlloc]
 * LEAF - scsiDelayLock [SCSIDelayCompletion (higher than handleArrayLock)]
 * LEAF - worldLock [SCSIAllocHandleTarg (higher than handleArrayLock)]
 */

#define SP_RANK_SCSIDELAY   (SP_RANK_LEAF)
#define SP_RANK_TARGETLIST  (SP_RANK_SCSIDELAY - 1)
#define SP_RANK_HANDLEARRAY (SP_RANK_TARGETLIST - 1)
#define SP_RANK_ADAPTER     (SP_RANK_HANDLEARRAY - 1)
#define SP_RANK_SCSILOCK    (SP_RANK_ADAPTER - 1)
#define SP_RANK_HANDLE      (SP_RANK_HANDLEARRAY - 1)

#if SP_RANK_SCSILOCK < SP_RANK_SCSI_LOWEST
#error "Lowest rank in SCSI should be >= SP_RANK_SCSI_LOWEST."
#endif

/*
 * Provide logs for the SCSI error path available in all build types.
 * The logs are conditional on release builds and depend on a dynamic
 * configuration option.
 * These logs should never be placed in the performance path.
 */
#ifndef VMX86_LOG
#define SCSICondRelLog(cond, _args...) \
   do {                            \
      if (CONFIG_OPTION(cond)) {   \
         Log(_args);               \
      }                            \
   } while (FALSE)
#else
#define SCSICondRelLog(cond, _args...) Log(_args)
#endif

/*
 * Command structure for a SCSI Reserve command.
 */
typedef struct {
   uint8 opcode:8;	// operation code
   uint8 ext:1,
      tid:3,
      tparty:1,
      lun:3;		// logical unit number
   uint8    resid;
   uint16   extlen;
   uint8    control;
} SCSIReserveCmd;

static inline uint64 
ByteSwap64(uint64 in)
{
   return ((in >> 56) & 0x00000000000000ffLL) |
          ((in >> 40) & 0x000000000000ff00LL) |
          ((in >> 24) & 0x0000000000ff0000LL) |
          ((in >>  8) & 0x00000000ff000000LL) | 
          ((in <<  8) & 0x000000ff00000000LL) |
          ((in << 24) & 0x0000ff0000000000LL) |
          ((in << 40) & 0x00ff000000000000LL) |
          ((in << 56) & 0xff00000000000000LL);
}

static inline uint32 
ByteSwapLong(uint32 in)
{
   return ((in >> 24) & 0x000000ff) | ((in >> 8) & 0x0000ff00) | 
          ((in << 8) & 0x00ff0000) | ((in << 24) & 0xff000000);
}

static inline uint16
ByteSwapShort(uint16 in)
{
   return ((in >> 8) & 0x00ff) | ((in << 8) & 0xff00);
}

static inline void
SCSIInitResultID(SCSI_Handle *handle, Async_Token *token,
		 SCSI_ResultID *rid)
{
   rid->target = handle->target;
   rid->partition = handle->partition;
   rid->handleID = handle->handleID;
   rid->token = token;
   rid->cmd = NULL;
   rid->path = NULL;
}

/*
 * 'delayCmds > 0' indicates a failed command has been put back on the
 * target queue, and that command must be issued from a world context,
 * because a failover that requires synchronous commands will be
 * happening.  'delayCmds > 0' prevents any queued commands from being
 * issued in the bottom-half context.  Since the failed command is on the
 * queue, it also causes new commands to be queued up behind it rather
 * than being issued.  No requests will be issued on the target until the
 * corresponding helper request calls SCSIExecQueuedCommand with the
 * 'override' flag.
 */
static inline void
SCSIIncDelayCmds(SCSI_Target *target)
{
   ASSERT(SP_IsLocked(&target->adapter->lock));
   ASSERT(target->delayCmds < 50 );
   target->delayCmds++;
}

static inline void
SCSIDecDelayCmds(SCSI_Target *target)
{
   ASSERT(SP_IsLocked(&target->adapter->lock));
   ASSERT(target->delayCmds > 0 );
   target->delayCmds--;
}

static inline int
SCSIDelayCmdsCount(SCSI_Target *target)
{ 
   ASSERT(SP_IsLocked(&target->adapter->lock));
   return( target->delayCmds );
}

#define SCSI_QHEAD 1
#define SCSI_QTAIL 0
#define SCSI_QPRIORITY 1
#define SCSI_QREGULAR 0

/* Functions implemented in  vmk_scsi.c */
VMK_ReturnStatus SCSIValidatePartitionTable(SCSI_Adapter *adapter, 
                                            SCSI_Target *target);
VMK_ReturnStatus SCSISyncCommand(SCSI_Handle *handle,SCSI_Command *cmd,
				 SCSI_Path *path, Bool useHandleWorldId);
void SCSISetupResetCommand(SCSI_Handle *handle, SCSI_Command *cmd,
			   SCSI_ResultID *ridp);
SCSI_Handle *SCSIAllocHandleTarg(SCSI_Target *target, World_ID worldID, 
				 uint32 partition);
Bool SCSIExecQueuedCommand(SCSI_Target *target, Bool thisTarget,
			   Bool override, Bool asyncCantBlock);
void SCSIHandleDestroy(SCSI_Handle *handle);
void SCSICheckForCachedSense(uint8 *senseBuffer, SCSI_Command *cmd,
			     SCSI_Status *scsiStatus, SCSISenseData *sense,
                             uint32 *bytesXferred, Bool *done);
void SCSIAbortCommand(SCSI_Handle *handle, World_ID worldID,
		      SCSI_Command *cmd, VMK_ReturnStatus *result);
void SCSIResetCommand(SCSI_Handle *handle, World_ID worldID,
		      SCSI_Command *cmd, VMK_ReturnStatus *result);
void SCSIProcPrintHdr(char* buffer, int* lenp);
void SCSIProcPrintStats(SCSI_Stats *stats, char* buffer, int* lenp);
struct SCSI_Handle *SCSI_HandleFind(SCSI_HandleID handleID);
void SCSI_HandleRelease(struct SCSI_Handle *handle);
void SCSI_InitialErrorCheckOfCommand(SCSI_Command *cmd,
                                     Bool activeReservation, SCSI_Status *scsiStatus,
                                     SCSISenseData *sense, Bool *done);
void SCSI_GetXferData(SCSI_Command *cmd, uint8 devClass, uint32 blockSize);
/* Functions implemented in mpath.c */
void SCSIAddPath(SCSI_Target *target, SCSI_Adapter *adapter, uint32 tid,
		 uint32 lun);
Bool SCSIRemovePath(SCSI_Target *target, SCSI_Adapter *adapter, uint32 tid,
		    uint32 lun);
Bool SCSITargetHasPath(SCSI_Target *target, SCSI_Adapter *adapter, uint32 tid,
		    uint32 lun);
void SCSIMarkPathStandby(SCSI_Path *path);
void SCSIMarkPathOn(SCSI_Path *path);
void SCSIMarkPathDead(SCSI_Path *path);
void SCSIMarkPathUndead(SCSI_Path *path);
Bool SCSISetTargetType(SCSI_Target *target,VMnix_TargetInfo *targetInfo);
Bool SCSIHasWorkingPath(SCSI_Handle *handle);
void SCSIChoosePath(SCSI_Handle *handle, SCSI_ResultID *rid);
Bool SCSIDeviceNotReady(SCSI_Target *target, SCSI_Status status, SCSISenseData *senseBuffer);
Bool SCSIDeviceIgnore(SCSI_Target *target);
Bool SCSIPathDead(SCSI_Target *target, SCSI_Status status, SCSISenseData *senseBuffer);
VMK_ReturnStatus SCSIParsePathCommand(SCSI_Target *target, char *p);
void SCSIRequestHelperFailover(SCSI_Target *target);
void SCSIStateChangeCallback(void *deviceName);
void SCSIMarkPathOnIfValid(SCSI_Target *target, SCSI_ResultID *rid);
VMK_ReturnStatus SCSIDGCStartRegistration(SCSI_Handle *handle, SCSI_Command *cmd);

/* Functions implemented in disksched.c */
VMK_ReturnStatus SCSISchedIssued(SCSI_Adapter *adapter, SCSI_Target *target,
				 SCSI_Handle *handle, SCSI_Command *cmd,
				 SCSI_ResultID *rid);
void SCSISchedDone(SCSI_Adapter *adapter, SCSI_Target *target,
		   SCSI_ResultID *rid);
SCSI_SchedQElem *SCSISchedQFind(SCSI_Target *target, World_ID worldID);
SCSI_SchedQElem *SCSISchedQAlloc(SCSI_Target *target, World_ID worldID);
void SCSISchedQFree(SCSI_Target *target, SCSI_SchedQElem *sPtr);

SCSI_QElem *SCSIQElemAlloc(void);
void SCSIQElemFree(SCSI_QElem *elem);
void SCSIQElemEnqueue(SCSI_Target *target, SCSI_QElem *elem, int qhead, int priority);
SCSI_QElem *SCSIQElemDequeue(SCSI_Target *target);
SCSI_QElem *SCSIDetachQElem(SCSI_Handle *handle, World_ID worldID,
			    SCSI_Command *cmd, Bool findAny);
void SCSIProcPrintPerVM(char *page, int *len, SCSI_Target *target);

#endif // _SCSI_INT_H_
