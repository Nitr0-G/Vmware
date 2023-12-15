/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * vmkernel_dist.h --
 *
 *	general vmkernel defs.
 */

#ifndef _VMKERNEL_DIST_H
#define _VMKERNEL_DIST_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_DISTRIBUTE
#define INCLUDE_ALLOW_VMK_MODULE
#define INCLUDE_ALLOW_MODULE
#include "includeCheck.h"


#define CEIL(_a, _b)        (((_a)+(_b)-1)/(_b))
#define FLOOR(_a, _b)       ((_a)/(_b))
#define ALIGN_DOWN(_a, _b)  (FLOOR((_a),(_b)) * (_b))
#define ALIGN_UP(_a, _b)    (CEIL((_a),(_b)) * (_b))

// physical cpu numbers
typedef uint32 PCPU;
#define INVALID_PCPU		((PCPU) -1)

#define MAX_PCPUS      32
#define MAX_PCPUS_BITS  5  // MAX_PCPUS <= (1 << MAX_PCPUS_BITS)
#define MAX_PCPUS_MASK  ((1 << MAX_PCPUS_BITS) - 1)

//XXX use accessors for these
extern uint32 cpuKhzEstimate;                      // from vmkernel.h
#define cpuMhzEstimate (cpuKhzEstimate/1000)

/* seconds since 1970 according to Console OS*/
extern uint32 consoleOSTime;                            //from vmkernel.h

#if (defined(VMKERNEL) ||  defined(VMKERNEL_MODULE)) && defined(VMX86_DEBUG)
/* 
 * On non DEBUG builds {ENABLE|CLEAR}_INTERRUPTS are defined in 
 * vmx/public/vm_ams.h.  Because we want to do crazy stuff, special
 * DEBUG only version are defined here.
 */
typedef struct {
   void *volatile *lastClrIntrRA;
   volatile Bool  *inIntHandler;
} vmkDebugInfo;

extern vmkDebugInfo vmkDebug;

#define CLEAR_INTERRUPTS() do {                                 \
    if (vmkDebug.lastClrIntrRA) {                               \
       *vmkDebug.lastClrIntrRA = __builtin_return_address(0);   \
    }                                                           \
    __asm__ __volatile__ ("cli": : :"memory");                  \
} while(0)

#define ENABLE_INTERRUPTS() do {                                \
   if (vmkDebug.inIntHandler && *vmkDebug.inIntHandler) {       \
      Panic("Attempted to enable interrupts from "              \
            "within an interrupt handler.\n");                  \
    }                                                           \
   __asm__ __volatile__ ("sti": : :"memory");                   \
} while(0)

#define RESTORE_FLAGS(f) do {                                                   \
   if (((f) & EFLAGS_IF) && vmkDebug.inIntHandler && *vmkDebug.inIntHandler) {  \
      Panic("Attempted to enable interrupts from "                              \
            "within an interrupt handler (via RESTORE_FLAGS).\n");              \
   }                                                                            \
   _Set_flags(f);                                                              \
} while(0)
#endif

/* functions in init.c for converting physical and virtual addresses */
extern VA VMK_MA2VA(MA maddr);
extern MA VMK_VA2MA(VA vaddr);

/* Accessors in init.c for vmkernelID */
extern int VMK_GetVMKernelID(void);
extern Bool VMK_CheckVMKernelID(void);

/* #define VMK_OLD_PARTITION_TYPE  0xff obsolete */
#define VMK_PARTITION_TYPE      0xfb
#define VMK_DUMP_PARTITION_TYPE 0xfc

/*
 * silence some compiler warnings due to the fact that
 * some of the apps (vmkfstools) include disklib.h as well
 * as vmkernel_dist.h -- both of which define DISK_SECTOR_SIZE
 */
#define VMK_DISK_SECTOR_SIZE 512
#ifdef DISK_SECTOR_SIZE
  #if (VMK_DISK_SECTOR_SIZE != DISK_SECTOR_SIZE)
     #error "VMK_DISK_SECTOR_SIZE should equal DISK_SECTOR_SIZE."
  #endif
#else
#define DISK_SECTOR_SIZE VMK_DISK_SECTOR_SIZE
#endif
/* Convert from 32-bit disk sector number to 64-bit disk byte address */
#define SECTORS_TO_BYTES(s)  ((uint64)(s) * (uint64)DISK_SECTOR_SIZE)

EXTERN void _Log(const char *, ...) PRINTF_DECL(1, 2);
EXTERN void _Warning(const char *, ...) PRINTF_DECL(1, 2);
EXTERN void _SysAlert(const char *, ...) PRINTF_DECL(1, 2);
#endif
