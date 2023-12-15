/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * vmkernel_asm.h --
 *
 *	Utility macros for vmkernel asm functions.
 *
 * 	NOTE: Do NOT put constants shared between C and ASM code here,
 * 	they go in genasmdefn.c
 */

#ifndef _VMKERNEL_ASM_H_
#define _VMKERNEL_ASM_H_

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#define PARAM1	8(%ebp)
#define	PARAM2	12(%ebp)
#define	PARAM3	16(%ebp)
#define	PARAM4	20(%ebp)
#define	PARAM5	24(%ebp)
#define	PARAM6	28(%ebp)
#define	PARAM7	32(%ebp)

#define ENTRY(name) \
  .globl name; \
  .align 16,0x90; \
  name##:

/* See struct VMKExcRegs in vmkernel_ext.h */
#define SAVE_REGS() \
        cld;         \
        pushl   %edi;  \
        pushl   %esi;  \
        pushl   %ebp;  \
        pushl   %ebx;  \
        pushl   %edx;  \
        pushl   %ecx;  \
        pushl   %eax;  \
        pushl   %gs;  \
        pushl   %fs;  \
        pushl   %ds;  \
        pushl   %es;

/* See struct VMKExcRegs in vmkernel_ext.h */
#define RESTORE_REGS() \
        popl    %es;  \
        popl    %ds;  \
        popl    %fs;  \
        popl    %gs;  \
        popl    %eax; \
        popl    %ecx; \
        popl    %edx; \
        popl    %ebx; \
        popl    %ebp; \
        popl    %esi; \
        popl    %edi;


#endif // ifndef _VMKERNEL_ASM_H_
