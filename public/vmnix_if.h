/* **********************************************************
 * Copyright 2002 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/
	
/*
 * vmnix_if.h --
 *
 *	Some of the stuff exported to the vmnix module.
 *
 */

#ifndef _VMNIX_IF_H
#define _VMNIX_IF_H

#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "vm_atomic.h"
#include "vmnix.h"
#include "vmnix_acpi.h"
#include "vmnix_if_dist.h"
#include "rpc_types.h"
#include "return_status.h"
#include "world_ext.h"
#include "scsi_ext.h"
#include "helper_ext.h"
#include "pci_ext.h"
#include "vmk_layout.h"
#include "identity.h"
#include "vmkcfgopts_public.h"

/*
 * COS currently only uses < 50 entries, but we start our usage at 2000.
 * VMNIX_VMK_TSS_DESC is the task when host world is running in vmkernel.
 * VMNIX_VMK_DF_TSS_DESC is the double fault task for host world.
 * VMNIX_VMK_CODE_SEG and VMNIX_VMK_DATA_SEG are the "transition" code and
 * data segments.  "transition" is this is little piece of code that we run
 * in the COS pagetable/task that does the actual task switch call to the
 * vmkernel.
 * Once we're in the vmkernel pagetable/task, we use the standard vmkernel
 * code and data segments (DEFAULT_CS_DESC and DEFAULT_DS_DESC).  These are
 * currently located at index 2050 and 2051.  They have to be less than
 * VMNIX_VMK_LAST_DESC and this is asserted in Host_SetGDTEntry.
 */
#define VMNIX_VMK_FIRST_DESC	2000
#define VMNIX_VMK_TSS_DESC	(VMNIX_VMK_FIRST_DESC)
#define VMNIX_VMK_DF_TSS_DESC	(VMNIX_VMK_FIRST_DESC + 1)
#define VMNIX_VMK_CODE_SEG   	(VMNIX_VMK_FIRST_DESC + 2)
#define VMNIX_VMK_DATA_SEG   	(VMNIX_VMK_FIRST_DESC + 3)
#define VMNIX_VMK_NMI_TSS_DESC  MON_VMK_NMI_TASK

#define VMNIX_VMK_LAST_DESC	2100

#define VMNIX_VMK_CS		(VMNIX_VMK_CODE_SEG << 3)
#define VMNIX_VMK_DS		(VMNIX_VMK_DATA_SEG << 3)
#define VMNIX_VMK_SS		VMNIX_VMK_DS
#define VMNIX_VMK_TSS_SEL	(VMNIX_VMK_TSS_DESC << 3)

/* NOTE: If reducing this number, be sure to check FIXADDR_TOP and
 * PKMAP_BASE in linux-server. 
 */

#define VMNIX_VMM_FIRST_LINEAR_ADDR 	0xfcc00000
#define VMNIX_KVA_START                 0xc0000000
#define VMNIX_KVA_END                   VMNIX_VMM_FIRST_LINEAR_ADDR 

#define VMNIX_VMK_FIRST_LINEAR_ADDR	(VMNIX_VMM_FIRST_LINEAR_ADDR + (VMM_NUM_PAGES * PAGE_SIZE))

#define VMNIX_VMK_STACK_LINEAR_ADDR 	(VMNIX_VMK_FIRST_LINEAR_ADDR + (VMK_NUM_CODE_PAGES * PAGE_SIZE))
#define VMNIX_VMK_STACK_TOP_LA		(VMNIX_VMK_STACK_LINEAR_ADDR + (VMK_HOST_STACK_PAGES * PAGE_SIZE) - 16)

#define VMNIX_VMK_MAP_LINEAR_ADDR       (VMNIX_VMK_FIRST_LINEAR_ADDR + (VMK_FIRST_MAP_PDE * PDE_SIZE))

#define HOST_IDT_PAGE                   1
#define HOST_IDT_LINEAR_ADDR            (VMNIX_VMM_FIRST_LINEAR_ADDR + HOST_IDT_PAGE * PAGE_SIZE)

#define VMNIX_VMK_CODE_START	(VMNIX_VMK_FIRST_LINEAR_ADDR + PAGE_SIZE)
#define VMNIX_VMK_DATA_START	(VMNIX_VMK_FIRST_LINEAR_ADDR + VMK_NUM_CODE_PAGES * PAGE_SIZE + VMK_HOST_STACK_PAGES * PAGE_SIZE)

#define HOSTVA_2_VMKVA(_vaddr) (_vaddr - VMNIX_VMM_FIRST_LINEAR_ADDR)
#define VMKVA_2_HOSTVA(_vaddr) (_vaddr + VMNIX_VMM_FIRST_LINEAR_ADDR)

/*
 * Console-os segments
 */
#define __VMNIX_CS	0x60
#define __VMNIX_DS	0x68

#define NUM_DEBUG_REGS      	24

/*
 * An interrupt source is called an irq by COS.
 * COS has no type for it though, it uses 'unsigned int'.
 */
typedef uint32	IRQ;

// From linux/asm/irq.h
#ifndef NR_IRQS
#define NR_IRQS                       224
#endif

/*
 * ISA IRQs
 */
#define TIMER_IRQ	0
#define KEYBOARD_IRQ	1
#define CASCADE_IRQ	2
#define SERIAL2_IRQ	3
#define SERIAL_IRQ	4
#define FLOPPY_IRQ	6
#define RTC_IRQ		8
#define MOUSE_IRQ	12
#define FPU_IRQ		13
#define IDE0_IRQ	14
#define IDE1_IRQ	15
#define NUM_ISA_IRQS	16


/*
 * Enumerate vmkernel system calls that are 
 * dispatched via syscallTable[] in host.c.
 */
#define VMNIX_VMK_CALL(name, dummy, _ignore...) \
        _SYSCALL_##name,
#define VMX_VMK_PASSTHROUGH(dummy, name) \
        _SYSCALL_##name,
enum {
#include "vmk_sctable.h"
_SYSCALL_NUM_SYSCALLS
};

/*
 * For VMNIX we want a unused irq, so we use the cascade IRQ which cannot
 * be used by real devices.
 */
#define VMNIX_IRQ	CASCADE_IRQ

#define MAX_BIOS_IDE_DRIVES 2 // same as MAX_DRIVES in drivers/block/ide.h
#define DRIVE_INFO_SIZE    16 // this is the amount of info reported by
                              // the BIOS and checked by the ide driver.

/*
 * hostIC is responsible for triggering interrupts on the host.
 *
 * Locking in hostIC:
 *
 * The only field that can get updated from any world is the pending field.
 * This is the set of pending interrupts. Whenever someone wants to trigger
 * an interrupt on the host they update this field. To update this field
 * the pendingLock must be held.
 *
 * The inService field is only accessed in the host world (HOST_PCPU, either
 * in vmnix or vmkernel) with interrupts disabled so no locking is needed.
 */
typedef enum {
   ICTYPE_PIC = 0,
   ICTYPE_IOAPIC,
   ICTYPE_UNKNOWN,
} ICType;

#define NUM_ICTYPES	ICTYPE_UNKNOWN

#define IRQ_PRESENT	0x01		// COS has irq defined even if unused
#define IRQ_SETUP	0x02		// irq forwarding has been set up
#define IRQ_ISA		0x04		// interrupt is ISA (non-PCI)

#define IRQ_COS_USED	0x01		// irq was used when vmkernel loaded
#define IRQ_COS_DISABLED 0x02		// irq was disabled when vmkernel loaded

typedef uint32 IrqSlice;
#define IRQS_PER_SLICE		(sizeof(IrqSlice)*8)
#define NR_SLICES_NEEDED(x)	(((x) + IRQS_PER_SLICE-1)/IRQS_PER_SLICE)
#define NR_IRQSLICES		NR_SLICES_NEEDED(NR_IRQS)
typedef struct HostIC {
   IrqSlice pending[NR_IRQSLICES];	// pending irqs for COS
   int      numirqs;			// number of irqs seen by COS
   int      numirqslices;		// number of irqslices seen by COS
   int      inService;			// an IRQ is being triggered for COS
   ICType   type;			// type of IC used
   uint32   cosVector[NR_IRQS];		// vector used by COS for each irq
   uint32   flags[NR_IRQS];		// flags about use of each irq by COS
   uint32   vmkVector[NR_IRQS];		// vector used by vmkernel for each irq
} HostIC;

typedef volatile uint32 ApicReg[4];

/*
 * Time in 10ms units (called jiffies) since the vmkernel was loaded.
 */
typedef struct HostTime {
   unsigned long currentTime;  // time in jiffies since vmkernel load
   unsigned long lastTime;     // currentTime at last COS jiffies update
} HostTime;

/*
 *  Shared proc system info.
 */

#define VMNIXPROC_INITIAL_ENTRIES 512

#define VMNIXPROC_SHARED_ENTRIES 64
#define VMNIXPROC_BUF_SIZE       VMNIX_PROC_READ_LENGTH  
#define VMNIXPROC_MAX_NAME       64

typedef enum {
   VMNIXPROC_ACTION_NEW_FILE = 0,
   VMNIXPROC_ACTION_NEW_DIR,
   VMNIXPROC_ACTION_DELETE,
   VMNIXPROC_ACTION_REALLOC,
   VMNIXPROC_ACTION_DUMPTREE,
} VMnixProc_Action;

typedef struct VMnixProc_EntryShared {
   VMnixProc_Action     action;
   int32                data;
   int32                parent;
   uint32               guid;
   Bool                 cyclic;
   char                 name[VMNIXPROC_MAX_NAME];
} VMnixProc_EntryShared;

typedef struct VMnixProc_RequestQueue {
   VMnixProc_EntryShared entries[VMNIXPROC_SHARED_ENTRIES];
   volatile uint32       head;
   volatile uint32       tail;
} VMnixProc_RequestQueue;

typedef struct VMnixProc_Shared {
   VMnixProc_RequestQueue reqQueue;
   char                  buffer[VMNIXPROC_BUF_SIZE];
   int32                 guard;
   int32                 len;
   uint32                activeGuid;      // guid of last proc call
   int32		 offset;          // offset into proc data to read
   Bool                  overflowQueued;  // are there entries waiting to be copied into
                                          // the shared queue
} VMnixProc_Shared;

#define VMNIX_VMKDEV_MAXREQ 256

typedef enum {
   VMNIX_VMKDEV_NONE, 
   VMNIX_VMKDEV_SCSI, 
   VMNIX_VMKDEV_BLOCK,
   VMNIX_VMKDEV_DISK,
   VMNIX_VMKDEV_NET,
   VMNIX_VMKDEV_CHAR,
   VMNIX_VMKDEV_MKNOD,
   VMNIX_VMKSTOR_DRIVER,
   VMNIX_VMKSTOR_DEVICE,
   VMNIX_VMKDEV_MAX,
} VMnix_VMKDevType;

typedef enum {
   VMNIX_VMKDEV_ACTION_NONE = 0,
   VMNIX_VMKDEV_ACTION_REGISTER,
   VMNIX_VMKDEV_ACTION_UNREGISTER
} VMnix_VMKDevAction;


/* vmnics reflect the following in the 
 * cos to keep ifconfig happy.
 */
typedef struct {
   unsigned long base_addr;
   unsigned int irq;
   unsigned long mem_start;
   unsigned long mem_end;
   unsigned short gflags;
   unsigned short flags;
   unsigned char dma;
} VMnix_CosVmnicInfo;

typedef struct VMnix_VMKDevInfo {
   VMnix_VMKDevType   type;
   char               vmkName[VMNIX_DEVICE_NAME_LENGTH]; 
   union {
      char            drv[VMNIX_MODULE_NAME_LENGTH]; 
      char            hostDev[VMNIX_DEVICE_NAME_LENGTH];
   } name;
   char               majorName[VMNIX_DEVICE_NAME_LENGTH];
   uint64             data;
   VMnix_VMKDevAction action; 
} VMnix_VMKDevInfo;

typedef struct VMnix_VMKDevShared {
   VMnix_VMKDevInfo queue[VMNIX_VMKDEV_MAXREQ];
   volatile uint32 qHead;
   volatile uint32 qTail;
} VMnix_VMKDevShared;

/*
 * Flags for indicating what condition has caused a vmkernel -> COS interrupt
 * Used for *VMnix_Shared_Data.interruptCause
 */
typedef enum VMnix_InterruptCause {
   VMNIX_NET_INTERRUPT = 0,
   VMNIX_NET_PACKET_PENDING,
   VMNIX_VMNIC_STATE_CHANGED,
   VMNIX_SCSI_INTERRUPT,
   VMNIX_LOG_DATA_PENDING,
   VMNIX_PROC_STATUS_CHANGE,
   VMNIX_MKDEV_EVENT,
   VMNIX_RPC_EVENT,
   VMNIX_HELPERCOMMAND_COMPLETE,
   VMNIX_CONDUIT_INTERRUPT,
   VMNIX_VGA_INTERRUPT,
   // Add more interrupt causes here
   // If you want a callback function for your interrupt, register it in
   // module.c
   VMNIX_NUM_INTERRUPT_CAUSES
} VMnix_InterruptCause;

/*
 * This structure contains pointers to data the can be read from vmnix module.
 */
#define MAX_IOAPICS	8
typedef struct VMnix_SharedData {
   struct RPC_CnxList	*cnxList;

   HostIC               *hostIC;
   volatile uint32      *ioapicLock;
   int32                *apicSelfIntVector;

   HostTime             *hostTime;
   VMnixProc_Shared     *proc;
   
   unsigned             *statCounters;
   unsigned             *configOption;
   unsigned             *debugRegs;

   int			*vmkernelBroken;
   unsigned long        *cachedIRQMask;

   // Flags to figure out what caused a VMnixInterrupt
   Atomic_uint32        *interruptCause;

   Atomic_uint32        *scsiCmplBitmaps;

   uint32               *cpuKhzEstimate;

   char			*logBuffer;
   int			logBufferLength;
   int			*firstLogChar;
   int			*nextLogChar;
   int                  *fileLoggingEnabled;

   // Ring buffer for helper world command completion notification
   int                  helperBufferLength;
   VMK_WakeupToken      *helperBuffer;
   int                  *helperBufferHead;
   int                  *helperBufferTail;

   uint32               *consoleOSTime;     // sec since '70 according to 
                                            // consoleOS
   uint32               *numCPUsUsed;       // how many of the physical cpus 
                                            // are being used by the vmkernel
   uint8                *logicalPerPackage; // number of "hyperthreads" per 
                                            // physical package
                                            // (1 on a non-hyperthread system)
   struct CPUIDSummary  *cpuids;            // cpuids of all used physical processors
   VMnix_VMKDevShared   *vmkDev;
   Helper_RequestHandle *activeIoctlHandle;
   Atomic_uint32        *vgaCOSLockOut;
   int			*vgaCOSConsole;
   Identity		*cosIdentity;
} VMnix_SharedData;

typedef struct VMnix_irq {
   int used;			// a COS device is using this irq
   int vector;			// interrupt vector used by COS for this irq
   int ic;			// IC number this irq is connected to
   int pin;			// pin on the IC this irq is connected to
} VMnix_irq;

typedef struct VMnix_Info {
   uint8 hostFuncs[PCI_NUM_BUSES][PCI_NUM_SLOTS]; // bitmap of COS seen funcs
   unsigned ICType;		// type of IC in use
   unsigned numirqs;		// number or irqs seen
   VMnix_irq irq[NR_IRQS];	// features of each irq
   uint32 vgaStart;             // start of VGA aperture
   uint32 vgaEnd;               // end of VGA aperture
   Bool vgaExtended;            // VGA in 50x80 mode
} VMnix_Info;

typedef struct VMnix_StartupArgs {
   VMnix_Init 		*initBlock;
   VMnix_Info		*vmnixInfo;
   VMnix_SharedData 	*sharedData;
   VMnix_ConfigOptions 	*configOptions;
   VA			endReadOnly;
   uint32               vmnixKernelVersion;  // cos kernel version number
   uint32               vmnixInterfaceNumber;// Userevel <-> vmnix version number
   uint32               vmnixBuildNumber;    // Paranoid version checking.
   int                  numVMKSyscalls;      // Number of vmkernel system calls the vmnix
                                             // module thinks there are.  This will catch
                                             // developers who run with outdated vmnix modules.
} VMnix_StartupArgs;

struct VMnix_AcpiInfo;
typedef struct VMnix_InitArgs {
   struct VMnix_AcpiInfo *acpiInfo;
} VMnix_InitArgs;

#define VMNIX_COPYSERV_NAME "copyServ"
typedef enum {
   VMNIX_COPY_FROM_USER = 0,
   VMNIX_COPY_TO_USER,
   VMNIX_MAX_COPY_OPS
} VMnix_CopyServOp;

typedef struct VMnix_CopyServArgs {
   VMnix_CopyServOp op;
   const void *src;
   void *dst;
   unsigned long len;
} VMnix_CopyServArgs;

typedef struct VMnix_CopyServResult {
   Bool success;
} VMnix_CopyServResult;

/*
 *  stat counters
 */

typedef enum {
   VMNIX_STAT_TOTALTIMER = 0,
   VMNIX_STAT_IDLE,
   VMNIX_STAT_VMKERNELCALL,
   VMNIX_STAT_HANDLEEXC,
   VMNIX_STAT_HANDLEINTR,
   VMNIX_STAT_RETURNHIDDEN,
   VMNIX_STAT_RETURNEXC,
   VMNIX_STAT_LASTEXC = VMNIX_STAT_RETURNEXC + 32,
   VMNIX_STAT_RETURNINTR,
   VMNIX_STAT_LASTINTR = VMNIX_STAT_RETURNINTR + NR_IRQS-1,
   VMNIX_STAT_NUM
} StatCounter;


extern void VMnix_SetIdentity(void);

/*
 * Helper macros for dealing with var args
 */

#define SEL_ARGS0(...) void
#define SEL_ARGS1(a,b,...) b
#define SEL_ARGS2(a,b,...) b, SEL_ARGS1(__VA_ARGS__)
#define SEL_ARGS3(a,b,...) b, SEL_ARGS2(__VA_ARGS__)
#define SEL_ARGS4(a,b,...) b, SEL_ARGS3(__VA_ARGS__)
#define SEL_ARGS5(a,b,...) b, SEL_ARGS4(__VA_ARGS__)

#define JOIN_ARGS0(...)
#define JOIN_ARGS1(a,b,...) a b
#define JOIN_ARGS2(a,b,...) a b, JOIN_ARGS1(__VA_ARGS__)
#define JOIN_ARGS3(a,b,...) a b, JOIN_ARGS2(__VA_ARGS__)
#define JOIN_ARGS4(a,b,...) a b, JOIN_ARGS3(__VA_ARGS__)
#define JOIN_ARGS5(a,b,...) a b, JOIN_ARGS4(__VA_ARGS__)

/*
 *  vmkernel entry point prototype
 */

typedef void (*InitFunc)(VMnix_StartupArgs *startupArgs);

#define _vmk_syscall_return(type, res) \
do { \
	return (type) (res); \
} while (0)


#define _proto_vmk_syscall(_nArgs, _retType, _name, _args...)        \
        _retType _name(SEL_ARGS##_nArgs(foo, _args));

#define _vmk_syscall0(type,name) \
type name(void) \
{ \
long __res; \
VMnix_SetIdentity(); \
__asm__ volatile ("int $0x90" \
	: "=a" (__res) \
	: "0" (_SYSCALL##name)); \
_vmk_syscall_return(type,__res); \
}

#define _vmk_syscall1(type,name,type1,arg1) \
type name(type1 arg1) \
{ \
long __res; \
VMnix_SetIdentity(); \
__asm__ volatile ("int $0x90" \
	: "=a" (__res) \
	: "0" (_SYSCALL##name),"b" ((long)(arg1))); \
_vmk_syscall_return(type,__res); \
}

#define _vmk_syscall2(type,name,type1,arg1,type2,arg2) \
type name(type1 arg1,type2 arg2) \
{ \
long __res; \
VMnix_SetIdentity(); \
__asm__ volatile ("int $0x90" \
	: "=a" (__res) \
	: "0" (_SYSCALL##name),"b" ((long)(arg1)),"c" ((long)(arg2))); \
_vmk_syscall_return(type,__res); \
}

#define _vmk_syscall3(type,name,type1,arg1,type2,arg2,type3,arg3) \
type name(type1 arg1,type2 arg2,type3 arg3) \
{ \
long __res; \
VMnix_SetIdentity(); \
__asm__ volatile ("int $0x90" \
	: "=a" (__res) \
	: "0" (_SYSCALL##name),"b" ((long)(arg1)),"c" ((long)(arg2)), \
		  "d" ((long)(arg3))); \
_vmk_syscall_return(type,__res); \
}

#define _vmk_syscall4(type,name,type1,arg1,type2,arg2,type3,arg3,type4,arg4) \
type name (type1 arg1, type2 arg2, type3 arg3, type4 arg4) \
{ \
long __res; \
VMnix_SetIdentity(); \
__asm__ volatile ("int $0x90" \
	: "=a" (__res) \
	: "0" (_SYSCALL##name),"b" ((long)(arg1)),"c" ((long)(arg2)), \
	  "d" ((long)(arg3)),"S" ((long)(arg4))); \
_vmk_syscall_return(type,__res); \
} 

#define _vmk_syscall5(type,name,type1,arg1,type2,arg2,type3,arg3,type4,arg4,type5,arg5) \
type name (type1 arg1, type2 arg2, type3 arg3, type4 arg4, type5 arg5) \
{ \
long __res; \
VMnix_SetIdentity(); \
__asm__ volatile ("int $0x90" \
	: "=a" (__res) \
	: "0" (_SYSCALL##name),"b" ((long)(arg1)),"c" ((long)(arg2)), \
	  "d" ((long)(arg3)),"S" ((long)(arg4)), "D" ((long)(arg5))); \
_vmk_syscall_return(type,__res); \
} 

#define VMNIX_INTERRUPTS_BITS 32 

/* Hash defining which bit is set for a given target/lun pair in the 
 * per shared adapter interruptsPending bitmap.  
 */
#define VMNIX_TARGET_LUN_HASH(_target, _lun) ((_target + 15 * _lun) % VMNIX_INTERRUPTS_BITS)

#endif
