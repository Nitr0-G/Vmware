/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * vmksysinfo_table.h -
 *	Declare handlers for vmksysinfo get/set requests.
 */

#ifndef _VMKSYSINFO_TABLE_H_
#define _VMKSYSINFO_TABLE_H_

#include "vmkevent_ext.h"
#include "trace_ext.h"
typedef struct PageDirectory {
#define NUM_ENTRIES_IN_DIR_PAGE 1021
   uint32 numPageEntries;
   uint32 numEntriesUsed;
   void *pages[NUM_ENTRIES_IN_DIR_PAGE];
   struct PageDirectory *nextDirPage;
} PageDirectory __attribute__((packed));

/*
 * To add a new handler, use the following prototype -
 *
 *	DECL_SET_HANDLER(VMKSYSINFO_DESC, Handler, type1, param1) \
 * or
 *	DECL_GET_HANDLER(VMKSYSINFO_DESC, Handler,     \
 *                       type1, param1, type2, param2) \
 *
 * The handlers then have the prototype -
 *
 *	extern VMK_ReturnStatus Handler(type1 *param1); for set handlers
 * and
 *	 extern VMK_ReturnStatus Handler(type1 *param1, type2 *param2,
 *					 unsigned long param2Len);
 *	for get handlers. param1 is always an input param (coming from
 *	COS user space). param2, if present, is the output param in which
 *	the user is expecting info.
 *	GET/SET determines whether the user wants information from or has
 *	information for us.
 */
#define VMKSYSINFO_DISPATCH_TABLE                                             \
        DECL_GET_HANDLER(TRACE_DATA, Trace_GetBatchData, int, index,          \
                           Trace_DataBuffer, outBuffer)                       \
        DECL_GET_HANDLER(TRACE_METADATA, Trace_GetMetadata, int, unused,      \
                           Trace_MetadataBuffer, outBuffer)                   \
        DECL_GET_HANDLER(TRACE_EVENTDEFS, Trace_GetEventDefs, int, unused,      \
                           Trace_EventDefBuffer, outBuffer)                   \
        DECL_GET_HANDLER(TRACE_EVENTCLASSES, Trace_GetEventClasses, int, unused,      \
                           Trace_ClassDefBuffer, outBuffer)                   \
        DECL_GET_HANDLER(TRACE_WORLDDESCS, Trace_GetWorldDescs, int, unused,      \
                           Trace_WorldDescBuffer, outBuffer)                   \
        DECL_GET_HANDLER(TRACE_CUSTOMTAGS, Trace_GetCustomTags, int, unused,      \
                           Trace_CustomTagBuffer, outBuffer)                   \
	DECL_GET_HANDLER(LUN_PATHS, SCSI_GetLUNPaths, VMnix_LUNPathArgs, args,  \
			 VMnix_LUNPathResult, result)        \
	DECL_GET_HANDLER(HARDWARE_INFO, Hardware_GetInfo, \
			 VMnix_HardwareInfoArgs, args, \
			 VMnix_HardwareInfoResult, result) \
	DECL_GET_HANDLER(SWAP_INFO, Swap_GetInfo, \
			 VMnix_SwapInfoArgs, args, \
			 VMnix_SwapInfoResult, result) \
	DECL_GET_HANDLER(ADAPTER_STATS, SCSI_GetAdapterStats, char, name,  \
			 SCSI_Stats, result)        \
	DECL_GET_HANDLER(LUN_STATS, SCSI_GetLUNStats, VMnix_LUNStatsArgs, args,  \
			 VMnix_LUNStatsResult, result)        \
 	DECL_GET_HANDLER(MEMMAP_INFO, MemMap_GetInfo,    \
 			 VMnix_MemMapInfoArgs, args,     \
 			 VMnix_MemMapInfoResult, result) \

#endif //_VMKSYSINFO_TABLE_H_
