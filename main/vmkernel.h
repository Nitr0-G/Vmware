/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

#ifndef _VMKERNEL_H
#define _VMKERNEL_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include <stdarg.h>

#include "vmware.h"
#include "vmkernel_dist.h"
#include "vcpuid.h"
#include "vmk_layout.h"
#include "vmnix_if.h"
#include "vmk_stubs.h"
#include "config.h"
#include "vmkernel_ext.h"
#include "vmkstress_dist.h"
#include "vmkcalls_vmcore.h"
#include "vmkcalls_public.h"

// memory debugging
#ifdef	VMX86_DEBUG
#define	DEBUG_MEM_ENABLE	(CONFIG_OPTION(DEBUG_MEM_ENABLE))
#else
#define	DEBUG_MEM_ENABLE	(0)
#endif	

typedef unsigned long long VMK_PDPTE;
typedef unsigned long long VMK_PDE;
typedef unsigned long long VMK_PTE;
#define FMTPT FMT64
#define VMK_MAKE_PTE(mpn, avail, flags) PAE_MAKE_PTE(mpn, avail, flags)
#define VMK_MAKE_PDE(mpn, avail, flags) PAE_MAKE_PDE(mpn, avail, flags)
#define VMK_PTE_2_MPN(pte)      PAE_PTE_2_PFN(pte)
#define VMK_PDE_2_MPN(pde)      PAE_PTE_2_PFN(pde)
#define VMK_PTE_2_FLAGS(pte)    ((uint32)(pte) & PTE_FLAGS)

#define ASM __asm__ __volatile__
#define INLINE inline

#define PTE_KERNEL   (PTE_P|PTE_RW)

extern unsigned debugRegs[];

#define SHARED_DATA_ADD(field, type, addr) do { \
   DEBUG_ONLY(extern void *_end); \
   ASSERT((VA)(addr) < (VA)&_end); \
   field = (type)((char *)(addr) + VMNIX_VMM_FIRST_LINEAR_ADDR); \
} while (FALSE)

extern uint32 numPCPUs;

/*
 * Add a small bit of sanity to a confusing & confused world.
 *
 * The following macros ensure that the sum of the sizes of all the arguements 
 * passed to a vmm->vmk call on the monitor side matches the sum of the sizes 
 * of all the arguements processed by one of the PROCESS_X_ARGS macros below.  
 * 
 * As a bonus check the major versions of the vmm and vmk are verified.
 * [The explicit check for version mismatch comes too late in the monitor
 * init sequence to be completely useful.]
 */

#ifdef VMX86_DEVEL
#define PROCESS_ARGS_BEFORE_CHECK() \
   uint32  _beforeMagic = va_arg(args, uint32);
#define PROCESS_ARGS_AFTER_CHECK()                                              \
   uint32  _afterMagic = va_arg(args, uint32);                                  \
   if (_beforeMagic != VMMVMK_BEFORE_ARG_MAGIC ||                               \
          _afterMagic != VMMVMK_AFTER_ARG_MAGIC) {                              \
      if (_afterMagic == VMMVMK_AFTER_ARG_MAGIC &&                              \
          (_beforeMagic >> 16)  == (VMMVMK_BEFORE_ARG_MAGIC >> 16)) {           \
         World_Panic(MY_RUNNING_WORLD,                                          \
                     "vmm->vmk major version number mismatch. vmm = %u "        \
                     "vmk = %u\n", _beforeMagic & 0xffff,                       \
                     VMMVMK_BEFORE_ARG_MAGIC & 0xffff);                         \
         return VMK_VERSION_MISMATCH_MAJOR;                                     \
      } else  {                                                                 \
         World_Panic(MY_RUNNING_WORLD,                                          \
                    "vmware-vmx vs vmkernel version mismatch.  Are you sure "   \
                    "you're running the correct vmx?\n\n"                       \
                    "The following applies to vmkernel developers:\n"           \
                    "vmm->vmk call argument passing error. Make sure "          \
                    "the number of arguments passed to VMK_Call() in the "      \
                    "monitor is the same number declared in the vmkernel "      \
                    "handler.\n");                                              \
         return VMK_VERSION_MISMATCH_MAJOR;                                     \
      }                                                                         \
   }
#else
#define PROCESS_ARGS_BEFORE_CHECK()
#define PROCESS_ARGS_AFTER_CHECK()
#endif

#define PROCESS_0_ARGS(f) \
   PROCESS_ARGS_BEFORE_CHECK() \
   PROCESS_ARGS_AFTER_CHECK() \
   ASSERT(function == f);

#define PROCESS_1_ARG(f, t1, a1) \
   PROCESS_ARGS_BEFORE_CHECK() \
   t1 a1 = va_arg(args, t1); \
   PROCESS_ARGS_AFTER_CHECK() \
   ASSERT(function == f);

#define PROCESS_2_ARGS(f, t1, a1, t2, a2) \
   PROCESS_ARGS_BEFORE_CHECK() \
   t1 a1 = va_arg(args, t1); \
   t2 a2 = va_arg(args, t2); \
   PROCESS_ARGS_AFTER_CHECK() \
   ASSERT(function == f);

#define PROCESS_3_ARGS(f, t1, a1, t2, a2, t3, a3) \
   PROCESS_ARGS_BEFORE_CHECK() \
   t1 a1 = va_arg(args, t1); \
   t2 a2 = va_arg(args, t2); \
   t3 a3 = va_arg(args, t3); \
   PROCESS_ARGS_AFTER_CHECK() \
   ASSERT(function == f);

#define PROCESS_4_ARGS(f, t1, a1, t2, a2, t3, a3, t4, a4) \
   PROCESS_ARGS_BEFORE_CHECK() \
   t1 a1 = va_arg(args, t1); \
   t2 a2 = va_arg(args, t2); \
   t3 a3 = va_arg(args, t3); \
   t4 a4 = va_arg(args, t4); \
   PROCESS_ARGS_AFTER_CHECK() \
   ASSERT(function == f);

#define PROCESS_5_ARGS(f, t1, a1, t2, a2, t3, a3, t4, a4, t5, a5) \
   PROCESS_ARGS_BEFORE_CHECK() \
   t1 a1 = va_arg(args, t1); \
   t2 a2 = va_arg(args, t2); \
   t3 a3 = va_arg(args, t3); \
   t4 a4 = va_arg(args, t4); \
   t5 a5 = va_arg(args, t5); \
   PROCESS_ARGS_AFTER_CHECK() \
   ASSERT(function == f);

#define PROCESS_6_ARGS(f, t1, a1, t2, a2, t3, a3, t4, a4, t5, a5, t6, a6) \
   PROCESS_ARGS_BEFORE_CHECK() \
   t1 a1 = va_arg(args, t1); \
   t2 a2 = va_arg(args, t2); \
   t3 a3 = va_arg(args, t3); \
   t4 a4 = va_arg(args, t4); \
   t5 a5 = va_arg(args, t5); \
   t6 a6 = va_arg(args, t6); \
   PROCESS_ARGS_AFTER_CHECK() \
   ASSERT(function == f);

#define PROCESS_7_ARGS(f, t1, a1, t2, a2, t3, a3, t4, a4, t5, a5, t6, a6, t7, a7) \
   PROCESS_ARGS_BEFORE_CHECK() \
   t1 a1 = va_arg(args, t1); \
   t2 a2 = va_arg(args, t2); \
   t3 a3 = va_arg(args, t3); \
   t4 a4 = va_arg(args, t4); \
   t5 a5 = va_arg(args, t5); \
   t6 a6 = va_arg(args, t6); \
   t7 a7 = va_arg(args, t7); \
   PROCESS_ARGS_AFTER_CHECK() \
   ASSERT(function == f);

typedef void (*VMKFreeFunc)(void *addr);

typedef enum {
   CPUTYPE_INTEL_P6,
   CPUTYPE_INTEL_PENTIUM4,
   CPUTYPE_AMD_ATHLON,
   CPUTYPE_AMD_DURON,
   CPUTYPE_OTHER,
   CPUTYPE_UNSUPPORTED
} CPUType;


extern CPUType cpuType;

extern Bool vmkernelLoaded;
extern Bool vmkernelInEarlyInit;

/* Unique identifier for this vmkernel/Console OS. */
extern Identity cosIdentity;

extern Bool VMK_IsValidMPN(MPN mpn);

static INLINE Bool
VMK_IsVMKEip(VA eip)
{
   return (eip >= VMK_CODE_START) &&
      (eip < VMK_CODE_START + PAGES_2_BYTES(VMK_NUM_CODE_PAGES));
}

static INLINE Bool
VMK_IsVMKStack(VA stackAddr)
{
   return (VA_2_VPN(stackAddr) >= VMK_FIRST_STACK_VPN) &&
      (VA_2_VPN(stackAddr) <= VMK_LAST_STACK_VPN);
}

extern VMK_ReturnStatus InitVMKernel(VMnix_InitArgs *args);

#if defined(VMKERNEL) && defined(VMX86_DEBUG) && defined(VMX86_DEVEL)
/*
 * Redefine ASSERT_IFNOT in obj builds so that we can make the Nth
 * ASSERT check fail for VMK_STRESS_ASSERT option.
 */
extern Bool VMK_CheckAssertStress(void);

#undef ASSERT_IFNOT
#define ASSERT_IFNOT(cond, panic) \
   (UNLIKELY(!(cond) || \
             (VMK_STRESS_DEBUG_OPTION(ASSERT_STRESS) && \
              VMK_CheckAssertStress())) ? \
      (panic) : 0)
#endif

#endif
