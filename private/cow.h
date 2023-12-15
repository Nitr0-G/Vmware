/**********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/
/*
 * cow.h --
 *
 *    copy-on-write mechanism for vmkernel disks
 */
#ifndef _COW_H
#define _COW_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

void COW_Init(void);
VMK_ReturnStatus COW_AsyncFileIO(COW_HandleID cowHandleID, 
                                 SG_Array *sgArr, Async_Token *token, 
                                 IO_Flags ioFlags);
VMK_ReturnStatus COW_CommitFile(COW_HandleID cowHandleID, 
                                int level, int startFraction, int endFraction);
VMK_ReturnStatus COW_ParentFileHandle(COW_HandleID cowHandle,
                                      FS_FileHandleID fileHandle, 
                                      FS_FileHandleID *parentHandle);
VMK_ReturnStatus COW_GetFileHandles(COW_HandleID cowHandle, 
                                    FS_FileHandleID *handleList, int *validHandles);
VMK_ReturnStatus 
COW_GetBlockOffsetAndFileHandle(COW_HandleID cowHandleID, uint32 sector,
                                FS_FileHandleID *fid, uint64 *actualBlockNumber, 
				uint32 *length);
VMK_ReturnStatus COW_GetCapacity(COW_HandleID, uint64 *lengthInBytes, uint32 *diskBlockSize);
VMK_ReturnStatus COW_CloseHierarchy(COW_HandleID cowHandleID);
VMK_ReturnStatus COW_OpenHierarchy(FS_FileHandleID *fids, int numFds, COW_HandleID *hidOut);
VMK_ReturnStatus COW_Combine(COW_HandleID *cid, int linkOffsetFromBottom);
VMK_ReturnStatus COW_ResetTarget(COW_HandleID handleID, World_ID worldID, SCSI_Command *cmd);
VMK_ReturnStatus COW_AbortCommand(COW_HandleID handleID, SCSI_Command *cmd);
#endif //#ifndef _COW_H
