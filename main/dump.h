/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * dump.h --
 *
 *      vmkernel core dump 
 */

#ifndef _VMK_DUMP_H_
#define _VMK_DUMP_H_

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "world_ext.h"
#include "dump_ext.h"
#include "prda.h"

struct NetDebug_Cnx;

extern void Dump_Init(void);
extern void Dump_RequestLiveDump(void);
extern VMK_ReturnStatus Dump_SetPartition(const char *adapName,
                                          uint32 targetID, uint32 lun,
                                          uint32 partition);
extern void Dump_Dump(VMKFullExcFrame *regs);
extern void Dump_LiveDump(VMKFullExcFrame *regs);
extern VMK_ReturnStatus Dump_Range(VA vaddr, uint32 size, const char *errorMsg);
extern void Dumper_PktFunc(struct NetDebug_Cnx* cnx, uint8 *srcMACAddr,
			   uint32 srcIPAddr, uint32 srcUDPPort,
	                   void *data, uint32 length);
extern void Dump_SetIPAddr(uint32 ipAddr);
extern uint32 Dump_GetIPAddr(void);
extern Bool Dump_IsEnabled(void);

static INLINE VMK_ReturnStatus
Dump_Page(VA va, const char *errorMsg)
{
   return Dump_Range(va, PAGE_SIZE, errorMsg);
}

/*
 *----------------------------------------------------------------------
 *
 * Dump_LiveDumpRequested --
 *
 *	Returns whether a user requested a "live" dump.
 *
 * Results:
 *	myPRDA.wantDump.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
Dump_LiveDumpRequested(void)
{
   return (PRDA_IsInitialized() && myPRDA.wantDump);
}
#endif
