/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * cosdump.h --
 *
 *      console os core dump 
 */

#ifndef _VMK_COS_DUMP_H_
#define _VMK_COS_DUMP_H_

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

VMK_ReturnStatus CosDump_Core(MA hostCR3, VA hdr);
VMK_ReturnStatus CosDump_LogBuffer(const VA hostLogBuf, 
                                   uint32 logEnd, uint32 logBufLen, 
                                   uint32 maxDumpLen, MA cr3);
VMK_ReturnStatus CosDump_BacktraceToPSOD(const VA hostLogBuf, 
                                         uint32 logEnd, uint32 logBufLen, 
                                         uint32 maxDumpLen, MA cr3);
#endif
