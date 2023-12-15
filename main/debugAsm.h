/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * debugAsm.h --
 *
 *     Functions exported by debugAsm.S.
 */

#ifndef _DEBUGASM_H_
#define _DEBUGASM_H_

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

extern void DebugReturnToProg(void);
extern void DebugHandleMemFault(void);
extern int  DebugGetChar(char *addr);
extern void DebugSetChar(char *addr, int val);

extern void DebugCatchException0(void);
extern void DebugCatchException1(void);
/* no DebugCatchException2 */
extern void DebugCatchException3(void);
extern void DebugCatchException4(void);
extern void DebugCatchException5(void);
extern void DebugCatchException6(void);
extern void DebugCatchException7(void);
extern void DebugCatchException8(void);
extern void DebugCatchException9(void);
extern void DebugCatchException10(void);
extern void DebugCatchException11(void);
extern void DebugCatchException12(void);
extern void DebugCatchException13(void);
extern void DebugCatchException14(void);
/* no DebugCatchException15 */
extern void DebugCatchException16(void);

#endif
