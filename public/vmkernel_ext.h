/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

#ifndef _VMKERNEL_EXT_H
#define _VMKERNEL_EXT_H

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_USERLEVEL
#include "includeCheck.h"

#include <stdarg.h>

/*
 * See also SAVE_REGS and RESTORE_REGS in vmkernel_asm.h
 */
typedef struct VMKExcRegs {
   uint32 es;
   uint32 ds;
   uint32 fs;
   uint32 gs;
   uint32 eax;
   uint32 ecx;
   uint32 edx;
   uint32 ebx;
   uint32 ebp;
   uint32 esi;
   uint32 edi;
} VMKExcRegs;

typedef struct VMKExcFrame {
   union {
      struct {
         uint32      handler;
         uint32      gateNum;
      } in;
      struct {
         uint32      eip;
         uint32      cs;
      } out;
   } u;
   uint32       errorCode;
   uint32       eip;
   uint16	cs, __csu;
   uint32	eflags;
   uint32	hostESP;
} VMKExcFrame;

typedef struct VMKFullExcFrame {
   VMKExcRegs regs;
   VMKExcFrame frame;
} VMKFullExcFrame;

typedef struct VMKUserExcFrame {
   uint32       errorCode;
   uint32       eip;
   uint16	cs, __csu;
   uint32	eflags;
   uint32	esp;
   uint16	ss, __ssu;
} VMKUserExcFrame;

/*
 * A pointer to a VMKFullExcFrame may be cast to a VMKFullUserExcFrame
 * if you've trapped into the kernel from usermode (and are looking at
 * the bits the processor/common.S pushed on the stack).
 */
typedef struct VMKFullUserExcFrame {
   VMKExcRegs      regs;       // pushed by CommonTrap
   uint32          pushValue;  // generally == gateNum
   uint32          gateNum;
   VMKUserExcFrame frame;      // state pushed by processor
} VMKFullUserExcFrame;

#define VMKERNEL_ENTRY VMK_ReturnStatus

#define DECLARE_ARGS(f)\
   uint32 function, va_list args

/* 
 * These are kind of silly.  The main reason for their existence is
 * to make one type in the arguments multiple times.  (as kinshuk said:
 * "like verifying a password").  If you prefer to live dangerously, feel 
 * free to use the DECLARE_ARGS macro above.   --greG
 */

#define DECLARE_0_ARGS(f)\
   uint32 function, va_list args
#define DECLARE_1_ARG(f, t1, a1) \
   uint32 function, va_list args
#define DECLARE_2_ARGS(f, t1, a1, t2, a2) \
   uint32 function, va_list args
#define DECLARE_3_ARGS(f, t1, a1, t2, a2, t3, a3) \
   uint32 function, va_list args
#define DECLARE_4_ARGS(f, t1, a1, t2, a2, t3, a3, t4, a4) \
   uint32 function, va_list args
#define DECLARE_5_ARGS(f, t1, a1, t2, a2, t3, a3, t4, a4, t5, a5) \
   uint32 function, va_list args
#define DECLARE_6_ARGS(f, t1, a1, t2, a2, t3, a3, t4, a4, t5, a5, t6, a6) \
   uint32 function, va_list args
#define DECLARE_7_ARGS(f, t1, a1, t2, a2, t3, a3, t4, a4, t5, a5, t6, a6, t7, a7) \
   uint32 function, va_list args

#endif
