/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * testworlds_ext.h -- 
 *
 *	External definitions for testworlds.
 */

#ifndef	_TESTWORLDS_EXT_H
#define	_TESTWORLDS_EXT_H

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMK_MODULE
#include "includeCheck.h"

#define TESTWORLDS_SUCCESS    (0)
#define TESTWORLDS_FAILURE   (-1)

#define TESTWORLDS_MAX_NAME_LEN (64)

// callback type for start/stop
typedef void (*TestWorldCallback)(int argc, char **argv);
// callback type for proc read handler
typedef int (*TestWorldReadCallback)(Proc_Entry *e, char *buf, int *len);

typedef struct {
   char*                  name;  // will appear as name of proc node
   int                    numVCPUs;
   Proc_Entry*            procEnt; 
   TestWorldCallback      startFunc;
   TestWorldCallback      stopFunc;
   TestWorldReadCallback  readFunc;
   Bool                   wantNewWorld; // TRUE to automatically start new world
} TestWorldType;

void TestWorlds_RegisterType(TestWorldType* testType);
void TestWorlds_UnregisterType(TestWorldType* testType);

World_GroupID
TestWorlds_NewVsmp(CpuSched_StartFunc sf,
                   void *data,
                   char *vcpuNames[],
                   char *groupName,
                   uint32 nshares,
                   uint8 numVcpus);
#endif
