/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * idt.h --
 *
 *	Brief description of this file.
 */

#ifndef _IDT_H
#define _IDT_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"
#include "idt_dist.h"
#include "vmkernel_ext.h"

/*
 * IDT vector usage:
 * 	x00-x1f	-- processor exceptions
 * 	x20-xdf -- device interrupts
 * 	xe0-xff -- processor interrupts
 *
 * Except vectors x30, x38, x40, x48, ..., xd0, xd8 used by monitor or
 * 	x80	-- linux system call
 * 	x90	-- vmkernel system call
 *
 * NOTE: On PIIIs, the local APIC can only accept at most two interrupts
 * per range. If a third one comes in while two are already pending, it will
 * be rejected, i.e. the message on the APIC bus will be NACK'ed and the
 * sender will retry. If the sender is also a local APIC, it will keep its
 * busy bit set as long as it has to retry until its message is accepted
 * by its target. Since we have to wait for the busy bit to clear before
 * using the local APIC to send an IPI, we can potentially deadlock.
 * It is therefore very important not to use more than two vectors per
 * range for processor interrupts. Since the APIC thermal, lint1, error
 * and spurious interrupts are local, we do not count them.
 * For device interrupts, it is also desirable (see IOAPICAllocateVector())
 * but not crucial.
 */
#define IDT_FIRST_MONITOR_VECTOR 0x30
#define IDT_MONITOR_VECTOR_MASK	0x07

#define IDT_LINUXSYSCALL_VECTOR	0x80
#define IDT_VMKSYSCALL_VECTOR	0x90

#define IDT_LAST_DEVICE_VECTOR	0xDF
#define IDT_APICTIMER_VECTOR	IDT_LAST_DEVICE_VECTOR // see below
#define IDT_MONITOR_IPI_VECTOR  0xE1
#define IDT_RESCHED_VECTOR	0xE9
#define IDT_TLBINV_VECTOR	0xF1
#define IDT_NOOP_VECTOR		0xF9
#define IDT_APICTHERMAL_VECTOR  0xFC
#define IDT_APICLINT1_VECTOR    0xFD
#define IDT_APICERROR_VECTOR    0xFE
#define IDT_APICSPURIOUS_VECTOR 0xFF

/*
 * Flags for vector info
 */
#define IDT_EDGE                0x01    // edge-triggered interrupt
#define IDT_ISA                 0x02    // ISA (or rather non-PCI) interrupt


typedef void (*IDT_DebugHandler)(void);

struct DTR32;

extern uint64 intrCounts[MAX_PCPUS][IDT_NUM_VECTORS];

extern void IDT_Init(VMnix_SharedData *sharedData);
extern void IDT_LateInit(void);
extern void IDT_GetDefaultIDT(struct DTR32 *dtr);
extern void IDT_GetDefaultUserIDT(struct DTR32 *dtr);
extern void *IDT_GetHandler(uint32 vector);

extern void IDT_RegisterDebugHandler(uint32 vector, IDT_DebugHandler h);
extern void IDT_WantBreakpoint(void);
extern void IDT_WantDump(void);
extern void IDT_Cleanup(void);

struct VMKExcFrame;

extern uint32 IDT_HandleException(struct VMKExcFrame *regs);
extern void IDT_HandleInterrupt(struct VMKExcFrame *regs);
extern void IDT_CheckInterrupt(void);
extern VM_PAE_PTE IDT_GetVMKIDTPTE(void);

VMKERNEL_ENTRY IDT_VMMIntOrMCE(DECLARE_ARGS(VMK_VMM_INTORMCE));
VMKERNEL_ENTRY IDT_VMKGetIntInfo(DECLARE_ARGS(VMK_GET_INT_INFO));

extern Bool IDT_VectorSetHostIRQ(uint32 vector, IRQ irq, uint32 flags);
extern Bool IDT_VectorSetDestination(uint32 vector, PCPU pcpuNum);
extern VMKERNEL_ENTRY IDT_SetupVMMDFHandler(DECLARE_ARGS(VMK_SETUP_DF));

extern void IDT_NmiHandler(void);
extern void IDT_DefaultTaskInit(Task* task, uint32 eip, uint32 esp, MA cr3);

extern VMKERNEL_ENTRY
IDT_VMKVectorIsFree(DECLARE_1_ARG(VMK_VECTOR_IS_FREE, uint32, vector));

extern Bool idtExcHasErrorCode[IDT_FIRST_EXTERNAL_VECTOR];

static INLINE Bool
IDT_ExcHasErrorCode(int gateNum)
{
   if (gateNum < IDT_FIRST_EXTERNAL_VECTOR) {
      return idtExcHasErrorCode[gateNum];
   } else {
      return FALSE;
   }
}

static INLINE Bool
IDT_VectorIsException(uint32 vector)
{
   return (vector < IDT_FIRST_EXTERNAL_VECTOR);
}

static INLINE Bool
IDT_VectorIsInterrupt(uint32 vector)
{
   return ((vector >= IDT_FIRST_EXTERNAL_VECTOR) &&
           (vector < IDT_NUM_VECTORS));
}

/*
 * The APIC timer is strictly speaking a device but is lumped with
 * processor interrupts because it's directly connected to a processor.
 * It does not go through any external IC.
 */

static INLINE Bool
IDT_VectorIsDevInterrupt(uint32 vector)
{
   return ((vector >= IDT_FIRST_EXTERNAL_VECTOR) &&
           (vector < IDT_LAST_DEVICE_VECTOR));
}

static INLINE Bool
IDT_VectorIsProcInterrupt(uint32 vector)
{
   return ((vector >= IDT_LAST_DEVICE_VECTOR) &&
           (vector < IDT_NUM_VECTORS));
}

void IDT_UnshareInterrupts(void);

#endif
