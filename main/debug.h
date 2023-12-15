/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * debug.h --
 *
 *     Debugger support module.
 */

#ifndef _DEBUG_H
#define _DEBUG_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#define DEBUG_MAX_DESC_LEN	64

typedef enum {
   DEBUG_CNX_SERIAL,
   DEBUG_CNX_NET,
   DEBUG_CNX_FILE,
   DEBUG_CNX_PROC
} Debug_CnxType;

struct Debug_Context;

typedef struct Debug_CnxFunctions {
   /*
    * Binds to a pre-specified address.
    */
   VMK_ReturnStatus (*start)(struct Debug_Context* dbgCtx);
   /*
    * Returns a string naming the device and/or address the debugger is
    * listening on.
    */
   VMK_ReturnStatus (*listeningOn)(struct Debug_Context* dbgCtx, char* desc,
				   int length);
   /*
    * Reads a character from the input stream.
    */
   VMK_ReturnStatus (*getChar)(struct Debug_Context* dbgCtx, unsigned char* ch);
   /*
    * Writes a character to the output stream.
    */
   VMK_ReturnStatus (*putChar)(struct Debug_Context* dbgCtx, unsigned char ch);
   /*
    * Flushes the output stream.
    */
   VMK_ReturnStatus (*flush)(struct Debug_Context* dbgCtx);
   /*
    * Releases bound address.
    */
   VMK_ReturnStatus (*stop)(struct Debug_Context* dbgCtx);
   /*
    * Check whether a character is available from the input stream and return
    * it if so.
    */
   VMK_ReturnStatus (*pollChar)(struct Debug_Context* dbgCtx,unsigned char* ch);
   /*
    * Cleans up the connection.
    */
   VMK_ReturnStatus (*cleanup)(struct Debug_Context* dbgCtx);
} Debug_CnxFunctions;

typedef struct Debug_Context {
   Bool	kernelDebugger;
   void* cnxData;
   Debug_CnxFunctions* functions;
} Debug_Context;

extern void Debug_Init(void);
extern Bool Debug_IsInitialized(void);
extern void Debug_Break(void);
extern void Debug_PutString(char *s);
extern void Debug_PutLenString(char *s, int len);
extern void Debug_SetAlternateFrame(VMKFullExcFrame *f);
extern void Debug_SetCosGetCharFn(int (*fn)(void*));
extern void Debug_SetSerialDebugging(Bool wantIt);
extern Bool Debug_CheckSerial(void);
extern Bool Debug_UWDebuggerIsEnabled(void);
extern void Debug_UWDebuggerEnable(Bool enable);

extern void Debug_AddCOSPanicBacktrace(struct VMKFullExcFrame *fullFrame);

/*
 * Register data for the debugger.
 */
typedef struct DebugRegisterFile {
   uint32 eax;
   uint32 ecx;
   uint32 edx;
   uint32 ebx;
   uint32 esp;
   uint32 ebp;
   uint32 esi;
   uint32 edi;
   uint32 eip;
   uint32 eflags;
   uint32 cs;
   uint32 ss;
   uint32 ds;
   uint32 es;
   uint32 fs;
   uint32 gs;
} DebugRegisterFile;

/*
 * Connection framework.
 */
extern VMK_ReturnStatus Debug_CnxInit(Debug_Context* dbgCtx, Debug_CnxType type,
				      Bool kernDbg);
extern VMK_ReturnStatus Debug_CnxStart(Debug_Context* dbgCtx);
extern VMK_ReturnStatus Debug_ListeningOn(Debug_Context* dbgCtx, char* desc,
					  int length);
extern VMK_ReturnStatus Debug_CnxStop(Debug_Context* dbgCtx);
extern VMK_ReturnStatus Debug_PutChar(Debug_Context* dbgCtx, unsigned char ch);
extern VMK_ReturnStatus Debug_GetChar(Debug_Context* dbgCtx, unsigned char* ch);
extern VMK_ReturnStatus Debug_PollChar(Debug_Context* dbgCtx,unsigned char* ch);
extern VMK_ReturnStatus Debug_Flush(Debug_Context* dbgCtx);
extern VMK_ReturnStatus Debug_CnxCleanup(Debug_Context* dbgCtx);

/*
 * Serial implementation.
 */
extern VMK_ReturnStatus Debug_SerialCnxInit(Debug_Context* dbgCtx);

// public defs needed for vmklinux module
#include "vmk_debug.h"

#endif // _DEBUG_H
