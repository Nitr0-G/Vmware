/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * log_int.h --
 *
 *	vmkernel internal interface to log module 
 */

#ifndef _LOG_INT_H
#define _LOG_INT_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"


#define VMK_LOG_ENTRY_SIZE              256
#define VMK_LOG_BUFFER_SIZE		(128 * 1024)
extern char logBuffer[VMK_LOG_BUFFER_SIZE];
extern uint32 firstLogChar;
extern uint32 nextLogChar;

struct VMnix_ConfigOptions;
struct World_Handle;
struct VMnix_SharedData;

void Log_EarlyInit(struct VMnix_ConfigOptions *vmnixOptions, 
                   struct VMnix_SharedData *sharedData,
                   struct VMnix_SharedData *hostSharedData);
void Log_Init(void);
void Log_SendMore(int prevNextLogChar, int maxSize);
void Panic_MarkCPUInPanic(void);
void Log_PrintSysAlertBuffer(void (*printFn)(const char *text), int nAlerts);
Bool Log_GetNextEntry(uint32 *entry, char *buffer, uint32 *len, Bool locked);
Bool Log_GetPrevEntry(uint32 *entry, char *buffer, uint32 *len);
void Log_GetLatestEntry(uint32 *entry, char *buffer, uint32 *len);
void Log_GetEarliestEntry(uint32 *entry, char *buffer, uint32 *len);

Bool Panic_IsCPUInPanic(void);

#endif
