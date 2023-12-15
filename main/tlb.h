/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * tlb.h --
 *
 *	This is the header file for routines to flush TLB entries.
 */

#ifndef _TLB_H
#define _TLB_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"
#include "pagetable.h"

/*
 *  Flush the hardware TLB.
 */

#define TLB_FLUSH()  ASM("movl %%cr3,%%eax\n\t" \
                         "movl %%eax,%%cr3\n\t" \
                         : : : "eax");

struct VMnix_Init;

extern void TLB_EarlyInit(struct VMnix_Init *vmnixInit);

extern VMK_ReturnStatus TLB_LateInit(void);

extern void TLB_LocalInit(void);

extern void TLB_Validate(VPN vpn, MPN mpn, unsigned flags);

extern void TLB_LocalValidate(VPN vpn, MPN mpn);

extern void TLB_LocalValidateRange(VA vaddr, uint32 length, MA maddr);

extern void TLB_Invalidate(VPN vpn, unsigned flags);

extern void TLB_Flush(unsigned flags);
extern void TLB_FlushPCPU(PCPU pcpuNum, unsigned flags);

extern void TLB_SetVMKernelPDir(MPN pageDir);
extern MPN TLB_GetVMKernelPDir(void);

extern MPN TLB_GetMPN(VA va);

#endif
