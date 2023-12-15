/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * userDump.h --
 *      UserWorld dumper.
 */

#ifndef VMKERNEL_USER_USERDUMP_H
#define VMKERNEL_USER_USERDUMP_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "vm_basic_types.h"
#include "vmkernel.h"
#include "compress.h"

struct UserObj;
struct UserDump_Header;

typedef struct UserDump_DumpData {
   CompressContext compressContext;
   UserObj *obj;
   uint8 *buffer;
   MPN mpn;
} UserDump_DumpData;

VMK_ReturnStatus UserDump_CartelInit(struct User_CartelInfo *uci);
VMK_ReturnStatus UserDump_CartelCleanup(struct User_CartelInfo *uci);

extern VMK_ReturnStatus UserDump_CoreDump(void);
extern void UserDump_ReleaseDumper(void);
extern VMK_ReturnStatus UserDump_Write(struct UserDump_DumpData *dumpData,
				       uint8 *buffer, int length);
extern VMK_ReturnStatus UserDump_WriteUserRange(struct World_Handle* world,
						struct UserDump_DumpData *dumpData,
						UserVA startVA, UserVA endVA);
extern uint32 UserDump_Seek(struct UserObj* obj, int32 offset, int whence);
extern void UserDump_WaitForDumper(void);
extern Bool UserDump_DumpInProgress(void);

extern VMK_ReturnStatus UserDump_DebugCoreDump(void);

#endif
