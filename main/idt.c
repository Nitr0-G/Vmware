/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * idt.c --
 *
 *	This module manages the vmkernel's interrupt/exception handling
 */


#include "vm_types.h"
#include "vm_libc.h"
#include "libc.h"
#include "vm_asm.h"
#include "x86.h"
#include "vmkernel.h"
#include "vmkemit.h"
#include "vmnix_if.h"
#include "kvmap.h"
#include "memmap.h"
#include "world.h"
#include "sched.h"
#include "sched_sysacct.h"
#include "idt.h"
#include "debug.h"
#include "util.h"
#include "host.h"
#include "chipset.h"
#include "x86.h"
#include "timer.h"
#include "bh.h"
#include "nmi.h"
#include "memalloc.h"
#include "net.h"
#include "bluescreen.h"
#include "dump.h"
#include "user.h"
#include "mce.h"
#include "watchpoint.h"
#include "serial.h"
#include "idt_ext.h"
#include "util.h"
#include "it.h"
#include "eventhisto.h"
#include "common.h"
#include "apic.h"
#include "trace.h"
#include "log_int.h"

#define LOGLEVEL_MODULE IDT
#include "log.h"

#define IDT_HANDLER_TABLE_LEN (12*1024)
typedef void (*GateHandler)(VMKExcFrame *regs);

static void IDTSetupDFHandler(MA pageRootMA);

Bool wantBreakpoint;

#define DEFAULT_IDT_SIZE  (sizeof(Gate) * IDT_NUM_VECTORS)
static Gate *defaultIDT;
static Gate *defaultUserIDT;

static Proc_Entry idtProcEntry;

static Bool IDTShouldPSODOnException(uint32 vector);

static void IDTIntrHandler(VMKExcFrame *regs);
static void IDTReturnPrepare(uint32 vector, VMKExcFrame *regs);

uint64 intrCounts[MAX_PCPUS][IDT_NUM_VECTORS];

typedef struct IDTHandlerInfo {
   IDT_Handler            func;
   const char            *name;
   void                  *clientData;
   struct IDTHandlerInfo *next;
} IDTHandlerInfo;

typedef struct IDTVectorInfo {
   uint8           setup;	// user mask
   uint8           enabled;	// user mask
   uint8           exclusive;	// user mask
   uint8           flags;
   IRQ             irq;		// COS irq for this vector
   IDTHandlerInfo *handlers;	// handlers for vmk devices using this vector
   PCPU            destPcpu;	// pcpu this vector is steered to
} IDTVectorInfo;

static IDTVectorInfo vecInfo[IDT_NUM_VECTORS];

static IDT_DebugHandler debugHandlers[IDT_FIRST_EXTERNAL_VECTOR];

Bool idtExcHasErrorCode[IDT_FIRST_EXTERNAL_VECTOR] = {
   FALSE, FALSE, FALSE, FALSE,
   FALSE, FALSE, FALSE, FALSE,
   TRUE,  FALSE, TRUE,  TRUE,
   TRUE,  TRUE,  TRUE,  FALSE,
   FALSE, TRUE,  FALSE, FALSE,
   FALSE, FALSE, FALSE, FALSE,
   FALSE, FALSE, FALSE, FALSE,
   FALSE, FALSE, FALSE, FALSE
};

static Task dfTask;
static uint8 doubleFaultStack[PAGE_SIZE];

/*
 * There is no cross-vector data to synchronize so we could have one
 * lock per vector if need be. Usage is low enough for now that one
 * global lock looks adequate.
 */
static SP_SpinLockIRQ idtLock;

// Flag to assert we don't enable interrupts before init is done
static Bool initDone = FALSE;


/*
 *----------------------------------------------------------------------
 *
 * IDTIsDoubleFaultStack --
 *
 *	Check if the given address is on the double fault stack
 *
 * Results:
 *      TRUE if on the double fault stack, FALSE otherwise
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
IDTIsDoubleFaultStack(VA addr)
{
   if ((addr > (VA)&doubleFaultStack[0]) ||
       (addr < (VA)&doubleFaultStack[PAGE_SIZE])) {
      return TRUE;
   } else {
      return FALSE;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * IDT_NmiHandler --
 *
 *      Handles an NMI, looking at the previously running task's state.
 *
 *	This is called by the nmi entry point in common.S.  The NMI
 *	task is set up in WorldASInit.  The NMI task segment is set up
 *	in World_Init.  The NMI task gate interrupt handler is set up
 *	in IDT_Init.
 *
 *	The big picture of it all is that an NMI interrupt triggers a
 *	task switch to the NMI task.  That saves the state of the
 *	running task to that task's task segment.  The new state of
 *	the CPU is loaded from the NMI's task segment.  The new task
 *	starts in common.S, which calls this function.  Once this
 *	function returns, a task-return-iret is done to return to the
 *	interrupted task.  This whole process leaves the interrupted
 *	task in exactly the same state except that the CR0_TS bit is
 *	set, and some local debug register information is lost.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Processes the NMI.
 *
 *----------------------------------------------------------------------
 */

void 
IDT_NmiHandler(void)
{
   NMIContext nmiContext;
   Task *task;
   Bool inHostWorld;

   myPRDA.inNMI = TRUE;

   asm("cld");

   inHostWorld = CpuSched_HostWorldCmp(MY_RUNNING_WORLD);

   /*
    * Get the task state that was interrupted, extract nmiContext info
    */
   if (inHostWorld) {
      /* We took an NMI in the COS. We must also be on the vmkernel
         task (with vmkernel IDT) because that's the only way to
         get to this handler. */
      task = Host_GetVMKTask();
   } else {
      task = (Task*)VPN_2_VA(TASK_PAGE_START);
   }
   NMI_TaskToNMIContext(task, &nmiContext);

   if (User_SegInUsermode(task->cs)) {
      ASSERT(!inHostWorld);
      nmiContext.source = NMI_FROM_USERMODE;
      NMI_PatchTask(task);
   } else {
      nmiContext.source = NMI_FROM_VMKERNEL;
      ASSERT(VMK_IsVMKEip(task->eip) || MY_RUNNING_WORLD->nmisInMonitor);
   }

   NMI_Interrupt(&nmiContext);
   
   myPRDA.inNMI = FALSE;
   return;
}


/*
 *----------------------------------------------------------------------
 *
 * IDTDoublefaultHandler --
 *
 *      Handle a double fault exception.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
IDTDoubleFaultHandler(void)
{
   VMKFullExcFrame fullFrame;
   World_Handle *faultingWorld = NULL;
   Task *task;
   Bool ksegMapped = FALSE;

   /*
    * Currently we're on hostworld CR3, so PRDA references go to CPU 0, so
    * need to find the current CPU from APIC. Also, we shouldn't run code
    * that modifies PRDA until we've switched back to the original cr3.
    * That means nothing that might acquire locks and modify
    * PRDA->numLocksHeld, such as Log+Warning+SysAlert.
    */
   Serial_Printf("Double fault on pcpu %d\n", APIC_GetPCPU());

   // find the right cr3
   if (prdas != NULL && prdas[APIC_GetPCPU()] != NULL) {
      faultingWorld = prdas[APIC_GetPCPU()]->runningWorld;
   }

   if ((faultingWorld != NULL) && 
       (faultingWorld->pageRootMA != 0) &&
       !World_IsHOSTWorld(faultingWorld)) {
      Serial_Printf("Switching to fauling world context (%Lx)\n",
                    faultingWorld->pageRootMA);
      SET_CR3(faultingWorld->pageRootMA);
   }
   Panic_MarkCPUInPanic(); //should be done before any log/warning/sysalert
   SysAlert("pcpu %d", APIC_GetPCPU());

   if ((faultingWorld == NULL) || World_IsHOSTWorld(faultingWorld)) {
      // either very early init or host world
      SysAlert("using host task (world=%p)", faultingWorld);
      task = Host_GetVMKTask();
      // no need to set cr3 since we're already in host world context
   } else {
      if (faultingWorld->pageRootMA == 0) {
         SysAlert("Faulting world page root MA is zero!!");   
      }

      task = (Task *)Kseg_DebugMap(faultingWorld->taskMPN);
      ksegMapped = TRUE;
   }

   SysAlert("VMK DF handler: eip=0x%x esp= 0x%x ebp=0x%x", 
            task->eip, task->esp, task->ebp);
   Util_TaskToVMKFrame(EXC_DF, task, &fullFrame);
   if (ksegMapped) {
      Kseg_DebugMapRestore();
}
   
#ifdef VMX86_DEBUG
   BlueScreen_Post("Double Fault.  EIP and EBP unreliable.\n", &fullFrame);
#else
   BlueScreen_Post("Exception #8.\n", &fullFrame);
#endif

   Debug_Break();
}

/*
 *----------------------------------------------------------------------
 *
 * IDT_WantBreakpoint --
 *
 *      Save the fact that we want a breakpoint when the current 
 *	interrupt handler finishes.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      wantBreakpoint is set to TRUE.
 *
 *----------------------------------------------------------------------
 */
void
IDT_WantBreakpoint(void)
{
   Warning("Asking for breakpoint ra=%p", __builtin_return_address(0));
   wantBreakpoint = TRUE;
}

/*
 *----------------------------------------------------------------------
 *
 * IDT_DefaultTaskInit --
 *
 *      Initializes a task to the default vmkernel context, no
 *      interrupt redirection mask, and the specified eip, esp, and
 *      cr3.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      task is modified.
 *
 *----------------------------------------------------------------------
 */
void
IDT_DefaultTaskInit(Task* task, uint32 eip, uint32 esp, MA cr3) 
{
   memset(task, 0, sizeof(Task));
   task->esp0 = esp;
   task->ss0 = DEFAULT_SS;
   task->esp1 = esp;
   task->ss1 = DEFAULT_SS;
   task->esp2 = esp;
   task->ss2 = DEFAULT_SS;
   task->cr3 = cr3;
   task->esp = task->esp0;
   task->es = DEFAULT_ES;
   task->cs = DEFAULT_CS;
   task->ss = DEFAULT_SS;
   task->ds = DEFAULT_DS;
   task->eip = eip;
   task->IOMapBase = sizeof(Task);
}


/*
 *----------------------------------------------------------------------
 *
 * IDTEmitSaveState --
 *
 *      Emit the code to save state in an exception handler.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      *memptr is updated to contain the emitted instructions.
 *
 *----------------------------------------------------------------------
 */
static EmitPtr
IDTEmitSaveState(EmitPtr memptr)
{
   EMIT_CLD;
   EMIT_PUSH_EDI;
   EMIT_PUSH_ESI;
   EMIT_PUSH_EBP;
   EMIT_PUSH_EBX;
   EMIT_PUSH_EDX;
   EMIT_PUSH_ECX;
   EMIT_PUSH_EAX;
   EMIT_PUSH_GS;
   EMIT_PUSH_FS;
   EMIT_PUSH_DS;
   EMIT_PUSH_ES;

   return memptr;
}

/*
 *----------------------------------------------------------------------
 *
 * IDTGenerateHandler --
 *
 *      Emit the code for an exception handler.
 *
 * Results:
 *      A pointer to the emitted code.
 *
 * Side effects:
 *      *memptr is updated to contain the emitted instructions.
 *
 *----------------------------------------------------------------------
 */
static void *
IDTGenerateHandler(int gateNum,         // 0x0-0xff
                   GateHandler handler, // C Handler
	           int push,         	// push a word on the stack?
		   int pushValue,    	// value to push, ignored if !push
		   int ds,	     	// Data segment register to switch to.
		   void *commonCode, 	// Location of common code.
		   EmitPtr *memptrPtr)  	// IN-OUT
{
   EmitPtr memptr = *memptrPtr;
   void	*codeAddr = *memptrPtr;


   if (push) {
      /* Push error code using push imm32 or push imm8 depending on
       * the pushValue */
      if ((pushValue & 0xffffff80) != 0) { 
         EMIT_PUSH_IMM(pushValue);
      } else {
         EMIT32_PUSH_IMM8(pushValue);
      }
   }

   EMIT_PUSH_IMM(gateNum);
   EMIT32_PUSH_IMM8(0);     // push an "error code" to make handler
                            // stack look like interrupt stack.
   memptr = IDTEmitSaveState(memptr);

   /*
    * Put the data segment into EDX, the handler into ECS and jump to the common 
    * code.
    */
   EMIT32_LOAD_REG_IMM(REG_EDX, ds);
   EMIT32_LOAD_REG_IMM(REG_ECX, handler);

   EMIT32_JUMP_IMM(commonCode);

   *memptrPtr = memptr;

   return codeAddr;
}

/*
 *----------------------------------------------------------------------
 *
 * IDTDefineGate --
 *
 *      Add a gate to the IDT for the given vector.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The IDT is updated.
 *
 *----------------------------------------------------------------------
 */
static void
IDTDefineGate(Gate *idt, 
              uint32 vector,         // 0x0-0xff
              GateHandler handler,  // C Handler
	      int push,         // push a word on the stack?
	      int pushValue,    // value to push, ignored if !push
	      int cs,		// Code segment to run in.
	      int ds,	     	// Data segment register to switch to.
	      void *commonCode, // Location of common code.
	      int gateType,
	      int dpl,
	      EmitPtr *memptrPtr)   // IN-OUT
{
   Gate *g;
   void *codeAddr;
   if (commonCode == NULL) {
      codeAddr = handler;
   } else {
      codeAddr = IDTGenerateHandler(vector, handler, push, pushValue, 
                                    ds, commonCode, memptrPtr);
   }

   g = &idt[vector];
   g->segment = cs;
   g->offset_lo = (uint32)(codeAddr) & 0xffff;
   g->offset_hi = (uint32)(codeAddr) >> 16;
   g->type = gateType;
   g->DPL = dpl;
   g->present = 1;
}


/*
 *----------------------------------------------------------------------
 *
 * IDTDefineTaskGate --
 *
 *      Add a task gate to the IDT for the given vector.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The IDT is updated.
 *
 *----------------------------------------------------------------------
 */
static void 
IDTDefineTaskGate(Gate *idt,
		  uint32 vector,
		  int taskSegment)  //Task segment to use
{
   idt[vector].segment = MAKE_SELECTOR(taskSegment, 0, 0);
   idt[vector].type = TASK_GATE;
   idt[vector].present = 1;
}


/*
 *----------------------------------------------------------------------
 *
 * Int?_* --
 *
 *      Handlers for all handled exceptions below vector 32.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The IDT is updated.
 *
 *----------------------------------------------------------------------
 */
static void 
Int0_Div(VMKExcFrame *regs)
{
   if (User_SegInUsermode(regs->cs)) {
      User_Exception(MY_RUNNING_WORLD, 0, regs);   
      NOT_REACHED();      
   }

   IDTReturnPrepare(0, regs);
}

static void 
Int1_Trap(VMKExcFrame *regs)
{
   uint32 dr6;

   if (User_SegInUsermode(regs->cs)) {
      User_Exception(MY_RUNNING_WORLD, 1, regs);   
      NOT_REACHED();      
   }

   GET_DR6(dr6);

   switch (Watchpoint_Check(regs)) {
   case WATCHPOINT_ACTION_CONTINUE:
      CommonRet(regs);
      NOT_REACHED();
   case WATCHPOINT_ACTION_BREAK:
      Warning("Debug exception @ 0x%x dr6=0x%x", regs->eip, dr6);
      Dump_RequestLiveDump();
      break;
   default:
      break;
   }

   IDTReturnPrepare(1, regs);
}

static void 
Int3_Breakpoint(VMKExcFrame *regs)
{
   if (User_SegInUsermode(regs->cs)) {
      User_Exception(MY_RUNNING_WORLD, 3, regs);   
      NOT_REACHED();      
   }

   IDTReturnPrepare(3, regs);
}

static void 
Int5_BoundRangeExceeded(VMKExcFrame *regs)
{
   if (User_SegInUsermode(regs->cs)) {
      User_Exception(MY_RUNNING_WORLD, 5, regs);   
      NOT_REACHED();      
   }

   IDTReturnPrepare(5, regs);
}

static void 
Int6_IllegalInstr(VMKExcFrame *regs)
{
   if (User_SegInUsermode(regs->cs)) {
      User_Exception(MY_RUNNING_WORLD, 6, regs);   
      NOT_REACHED();      
   }

   IDTReturnPrepare(6, regs);
}

static void 
Int7_DeviceNotAvailable(VMKExcFrame *regs)
{
   if (User_SegInUsermode(regs->cs)) {
      User_Exception(MY_RUNNING_WORLD, 7, regs);   
      NOT_REACHED();      
   }

   IDTReturnPrepare(7, regs);
}

static void 
Int10_InvalidTSS(VMKExcFrame *regs)
{
   IDTReturnPrepare(10, regs);
}

static void 
Int11_SegmentNotPresent(VMKExcFrame *regs)
{
   IDTReturnPrepare(11, regs);
}

static void 
Int12_StackFault(VMKExcFrame *regs)
{
   IDTReturnPrepare(12, regs);
}

static void 
Int13_GP(VMKExcFrame *regs)
{
   World_Handle *world = PRDA_GetRunningWorldSafe();
   /* If a non-priv fault, without a kernel-provided handler, invoke User_Exception. */
   if (User_SegInUsermode(regs->cs) || 
       ((world != NULL) && (world->userLongJumpPC != NULL))) {
      User_Exception(MY_RUNNING_WORLD, 13, regs);   
      NOT_REACHED();      
   }

   IDTReturnPrepare(13, regs);
}

static void 
Int14_PF(VMKExcFrame *regs)
{
   World_Handle *world = PRDA_GetRunningWorldSafe();

   /* If a non-priv fault, without a kernel-provided handler, invoke User_Exception. */
   if (User_SegInUsermode(regs->cs) || 
       ((world != NULL) && (world->userLongJumpPC != NULL))) {
      User_Exception(MY_RUNNING_WORLD, 14, regs);
      NOT_REACHED();
   }

   IDTReturnPrepare(14, regs);
}

static void 
Int16_FloatingPoint(VMKExcFrame *regs)
{
   if (User_SegInUsermode(regs->cs)) {
      User_Exception(MY_RUNNING_WORLD, 16, regs);   
      NOT_REACHED();      
   }

   IDTReturnPrepare(16, regs);
}

static void 
Int17_AlignmentCheck(VMKExcFrame *regs)
{
   IDTReturnPrepare(17, regs);
}

static void
Int18_MachineCheck(VMKExcFrame *regs)
{
   MCE_Handle_Exception();
   IDTReturnPrepare(18, regs);
}

static void 
Int19_XF(VMKExcFrame *regs)
{
   IDTReturnPrepare(19, regs);
}

/*
 *----------------------------------------------------------------------
 *
 * IDT_Init --
 *
 *      Initialize internal IDT data structures.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Internal state is initialized.
 *
 *----------------------------------------------------------------------
 */
void
IDT_Init(VMnix_SharedData *sharedData)
{
   char *idtHandlers;
   EmitPtr memptr;
   uint32 ds;
   uint32 cs;
   int i;

   SP_InitLockIRQ("idtLock", &idtLock, SP_RANK_IRQ_MEMTIMER+1); // above itLock

   memset(intrCounts, 0, sizeof(intrCounts[0][0])*MAX_PCPUS*IDT_NUM_VECTORS);
   memset(vecInfo, 0, sizeof(vecInfo[0])*IDT_NUM_VECTORS);
   memset(debugHandlers, 0, sizeof(IDT_DebugHandler)*IDT_FIRST_EXTERNAL_VECTOR);

   for (i = 0; i < IDT_NUM_VECTORS; i++) {
      vecInfo[i].destPcpu = HOST_PCPU;
   }
   
   cs = DEFAULT_CS;
   ds = DEFAULT_DS;

   /*
    * Initialize the default IDT for when worlds run.  We allocate this IDT
    * from code/readonly region to prevent host resets if IDT is corrupted.
    * All IDT modifications must be surrounded by
    * MemRO_ChangeProtection(MEMRO_WRITABLE/MEMRO_READONLY).
    */
   defaultIDT = MemRO_Alloc(DEFAULT_IDT_SIZE);
   ASSERT_NOT_IMPLEMENTED(defaultIDT != NULL && (((uint32)defaultIDT) & PAGE_MASK) == 0);

   idtHandlers = MemRO_Alloc(IDT_HANDLER_TABLE_LEN);
   ASSERT_NOT_IMPLEMENTED(idtHandlers != NULL);
   memptr = (EmitPtr)idtHandlers;

   MemRO_ChangeProtection(MEMRO_WRITABLE);

   IDTDefineGate(defaultIDT, 0, Int0_Div, 1, 0, cs, ds, CommonTrap, 
                 INTER_GATE, 0, &memptr);
   IDTDefineGate(defaultIDT, 1, Int1_Trap, 1, 0, cs, ds, CommonTrap, 
                 INTER_GATE, 0, &memptr);
   IDTDefineGate(defaultIDT, 3, Int3_Breakpoint, 1, 0, cs, ds, CommonTrap, 
                 INTER_GATE, 0, &memptr);
   IDTDefineGate(defaultIDT, 5, Int5_BoundRangeExceeded, 1, 0, cs, ds, CommonTrap, 
                 INTER_GATE, 0, &memptr);
   IDTDefineGate(defaultIDT, 6, Int6_IllegalInstr, 1, 0, cs, ds, CommonTrap, 
                 INTER_GATE, 0, &memptr);
   IDTDefineGate(defaultIDT, 7, Int7_DeviceNotAvailable, 1, 0, cs, ds, CommonTrap,
                 INTER_GATE, 0, &memptr);
   IDTDefineGate(defaultIDT, 10, Int10_InvalidTSS, 0, 0, cs, ds, CommonTrap, 
                 INTER_GATE, 0, &memptr);
   IDTDefineGate(defaultIDT, 11, Int11_SegmentNotPresent, 0, 0, cs, ds, CommonTrap, 
                 INTER_GATE, 0, &memptr);
   IDTDefineGate(defaultIDT, 12, Int12_StackFault, 0, 0, cs, ds, CommonTrap, 
                 INTER_GATE, 0, &memptr);
   IDTDefineGate(defaultIDT, 13, Int13_GP, 0, 0, cs, ds, CommonTrap, 
                 INTER_GATE, 0, &memptr);
   IDTDefineGate(defaultIDT, 14, Int14_PF, 0, 0, cs, ds, CommonTrap, 
                 INTER_GATE, 0, &memptr);
   IDTDefineGate(defaultIDT, 16, Int16_FloatingPoint, 1, 0, cs, ds, CommonTrap, 
                 INTER_GATE, 0, &memptr);
   IDTDefineGate(defaultIDT, 17, Int17_AlignmentCheck, 0, 0, cs, ds, CommonTrap, 
                 INTER_GATE, 0, &memptr);
   IDTDefineGate(defaultIDT, 18, Int18_MachineCheck, 1, 0, cs, ds, CommonTrap,
                 INTER_GATE, 0, &memptr);
   IDTDefineGate(defaultIDT, 19, Int19_XF, 1, 0, cs, ds, CommonTrap, 
                 INTER_GATE, 0, &memptr);

   /*
    * Set up the NMI task gate handler.
    */
   IDTDefineTaskGate(defaultIDT, EXC_NMI, DEFAULT_NMI_TSS_DESC);

   /* XXX what about vectors above 19, but below IDT_FIRST_EXTERNAL_VECTOR (32)? */

   ASSERT(IDT_VMKSYSCALL_VECTOR >= IDT_FIRST_EXTERNAL_VECTOR);
   ASSERT(IDT_VMKSYSCALL_VECTOR < IDT_NUM_VECTORS);
   ASSERT(IDT_LINUXSYSCALL_VECTOR >= IDT_FIRST_EXTERNAL_VECTOR);
   ASSERT(IDT_LINUXSYSCALL_VECTOR < IDT_NUM_VECTORS);
   ASSERT(IDT_FIRST_EXTERNAL_VECTOR <= IDT_NUM_VECTORS);

   /*
    * Define default handlers
    */
   for (i = IDT_FIRST_EXTERNAL_VECTOR; i < IDT_NUM_VECTORS; i++) {
      switch(i) 
      {
      case IDT_VMKSYSCALL_VECTOR:
         /*
          * Define the handler for VMK syscalls.
          */
         IDTDefineGate(defaultIDT, IDT_VMKSYSCALL_VECTOR,
                       User_UWVMKSyscallHandler, 1,
                       IDT_VMKSYSCALL_VECTOR, cs, ds, CommonTrap,
                       INTER_GATE, 3, &memptr);
         break;
      case IDT_LINUXSYSCALL_VECTOR:
         /*
          * Define the handler for Linux-compatibility syscalls.
          */
         IDTDefineGate(defaultIDT, IDT_LINUXSYSCALL_VECTOR,
                       User_LinuxSyscallHandler, 1,
                       IDT_LINUXSYSCALL_VECTOR, cs, ds, CommonTrap,
                       INTER_GATE, 3, &memptr);
         break;
      default:
         /*
          * All others go straight to IDTIntrHandler.
          */
	 IDTDefineGate(defaultIDT, i, IDTIntrHandler, 1, 
		       i, cs, ds, CommonIntr, 
		       INTER_GATE, 0, &memptr);
         break;
      }
   }

   ASSERT((char*)memptr < idtHandlers + IDT_HANDLER_TABLE_LEN);

   /*
    * The double fault handler needs a page table that is valid for system
    * worlds.  Let it use the host world's vmkernel pagetable since the
    * host world can never go away until we unload the vmkernel.
    */
   IDTSetupDFHandler(Host_GetVMKPageRoot());

   /*
    * Default user IDT is identical to the defaultIDT, except ...
    */
   defaultUserIDT = MemRO_Alloc(DEFAULT_IDT_SIZE);
   ASSERT_NOT_IMPLEMENTED(defaultUserIDT != NULL);
   memcpy(defaultUserIDT, defaultIDT, DEFAULT_IDT_SIZE);

   /*
    * ... in the default user idt the int3
    * gate runs at ipl 3, rather than 0, so that int3's work in the user worlds.
    * If this change were not made, the int3 in the user world result in a gp.
    */
   defaultUserIDT[EXC_BP].DPL = 3;

   MemRO_ChangeProtection(MEMRO_READONLY);

   /*
    * Need to set up the default GDT entry for the double fault
    * handler now, during init time.
    */

   World_SetDefaultGDTEntry(DEFAULT_DF_TSS_DESC,
			    (uint32)&dfTask - VMK_FIRST_ADDR, 
			    sizeof(dfTask) - 1,
		            TASK_DESC,
		            0, 0, 1, 1, 0);

   Host_SetIDT(MA_2_MPN(VMK_VA2MA((VA)defaultIDT)), TRUE);
}

/*
 *----------------------------------------------------------------------
 *
 * IDT_GetVMKIDTPTE --
 *     
 * Results:
 *     Return a PTE for the vmkernel IDT, which is subsequently used to 
 *     map the vmkernel IDT when we make a vmkcall/take an interrupt from
 *     a vmm world.
 *
 * Side Effects:
 *     None.
 *
 *----------------------------------------------------------------------
 */
VM_PAE_PTE
IDT_GetVMKIDTPTE(void)
{
   VMK_PTE pte;
   const MPN idtMPN = MA_2_MPN(VMK_VA2MA((VA)defaultIDT));
   ASSERT_NOT_IMPLEMENTED(defaultIDT);
   ASSERT(sizeof(VM_PAE_PTE) == sizeof(VMK_PTE));
   pte = VMK_MAKE_PTE(idtMPN, 0, PTE_P|PTE_A);
   return pte;
}

/*
 *----------------------------------------------------------------------
 *
 * IDTSetupDFHandler --
 *
 *      Setup the double fault handler.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      dfTask and the defaultIDT are modified.
 *
 *----------------------------------------------------------------------
 */
void
IDTSetupDFHandler(MA pageRootMA)
{ 
   /*
    * Setup the double fault task.
    */
   IDT_DefaultTaskInit(&dfTask, 
		       (uint32) IDTDoubleFaultHandler, 
		       (uint32)(doubleFaultStack + PAGE_SIZE - 4),
		       pageRootMA);
   
   IDTDefineTaskGate(defaultIDT, EXC_DF, DEFAULT_DF_TSS_DESC);
}


/*
 *----------------------------------------------------------------------
 *
 * IDT_GetDefaultIDT --
 *
 *      Set the *dtr to contain the limit and offset of the default IDT.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      *dtr is updated.
 *
 *----------------------------------------------------------------------
 */
void
IDT_GetDefaultIDT(DTR32 *dtr)
{
   dtr->limit = DEFAULT_IDT_SIZE - 1;
   dtr->offset = VMK_VA_2_LA((VA)defaultIDT);
}


/*
 *----------------------------------------------------------------------
 *
 * IDT_GetDefaulUserIDT --
 *
 *      Set the *dtr to contain the limit and offset of the default user
 *	IDT.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      *dtr is updated.
 *
 *----------------------------------------------------------------------
 */
void
IDT_GetDefaultUserIDT(DTR32 *dtr)
{
   dtr->limit = DEFAULT_IDT_SIZE - 1;
   dtr->offset = VMK_VA_2_LA((VA)defaultUserIDT);
}


/*
 *----------------------------------------------------------------------
 *
 * IDT_VMKVectorIsFree --
 *
 *      Return whether this is an OK vector.  Exported interface to VMM
 *
 * Results:
 *      TRUE if vector is free.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMKERNEL_ENTRY
IDT_VMKVectorIsFree(DECLARE_1_ARG(VMK_VECTOR_IS_FREE, uint32, vector))
{
   PROCESS_1_ARG(VMK_VECTOR_IS_FREE, uint32, vector);

   if ((vector >= IDT_FIRST_MONITOR_VECTOR) &&
       (vector < IDT_LAST_DEVICE_VECTOR) &&
       ((vector & IDT_MONITOR_VECTOR_MASK) == 0) &&
       (vector != IDT_LINUXSYSCALL_VECTOR) &&
       (vector != IDT_VMKSYSCALL_VECTOR)) {
      ASSERT(vecInfo[vector].setup == 0);
      return VMK_OK;
   } else {
      return VMK_FAILURE;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * IDT_VMKGetIntInfo
 *
 *      Return interrupt info to the VMM.
 *
 * Results:
 *      VMK_OK.
 *
 * Side effects:
 *      Fills in the supplied structure with interrupt information.
 *
 *----------------------------------------------------------------------
 */
VMKERNEL_ENTRY
IDT_VMKGetIntInfo(DECLARE_1_ARG(VMK_GET_INT_INFO, 
                                VMKIntInfo*, intData))
{
   PROCESS_1_ARG(VMK_GET_INT_INFO, VMKIntInfo*, intData);

   intData->monIPIVector = IDT_MONITOR_IPI_VECTOR;
   intData->vmkTimerVector = IDT_APICTIMER_VECTOR;

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * IDT_VectorSetHostIRQ --
 *
 *      Set up irq forwarding for a given vector
 *
 * Results:
 *      TRUE if successful
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Bool
IDT_VectorSetHostIRQ(uint32 vector, IRQ irq, uint32 flags)
{
   SP_IRQL prevIRQL;
   Bool success = TRUE;

   Log("0x%x irq %d flags 0x%x", vector, irq, flags);

   ASSERT(IDT_VectorIsDevInterrupt(vector));

   prevIRQL = SP_LockIRQ(&idtLock, SP_IRQL_KERNEL);

   if (vecInfo[vector].exclusive & IDT_HOST) {
      Warning("Exclusive use set up already");
      success = FALSE;

   } else if (vecInfo[vector].setup & IDT_HOST) {
      // If it has been set up already, the flags and irq must match
      ASSERT(vecInfo[vector].flags == flags);
      ASSERT(vecInfo[vector].irq == irq);

   } else {
      vecInfo[vector].setup |= IDT_HOST;
      ASSERT(!(vecInfo[vector].enabled & IDT_HOST));
      // XXX We don't pass exclusive use info from COS, so we assume PCI is
      // always sharable and ISA never
      vecInfo[vector].exclusive |= (flags & IDT_ISA) ? IDT_HOST : 0;
      ASSERT(vecInfo[vector].irq == 0);
      vecInfo[vector].irq = irq;
      // Flags must match if already set up for vmkernel
      if (vecInfo[vector].setup & IDT_VMK) {
	 ASSERT(vecInfo[vector].flags == flags);
      }
      vecInfo[vector].flags = flags;
   }

   SP_UnlockIRQ(&idtLock, prevIRQL);
   return success;
}


/*
 *----------------------------------------------------------------------
 *
 * IDT_VectorAddHandler --
 *
 *      Add an interrupt handler for the given vector.
 *
 * Results:
 *      TRUE if successful, FALSE otherwise
 *
 * Side effects:
 *      The list of interrupts handlers is updated.
 *
 *----------------------------------------------------------------------
 */

Bool
IDT_VectorAddHandler(uint32 vector, IDT_Handler h, void *data, Bool sharable, const char *name, uint32 flags)
{
   SP_IRQL prevIRQL;
   Bool success = FALSE;
   IDTHandlerInfo *handler;

   Log ("0x%x <%s> %s, flags 0x%x", vector, name ? name : "",
				sharable ? "sharable" : "exclusive", flags);
   
   Trace_RegisterCustomTag(TRACE_INTERRUPT, vector, name);
   ASSERT(IDT_VectorIsInterrupt(vector));
   ASSERT(h != NULL);
   ASSERT(name != NULL);

   DEBUG_ONLY(EventHisto_Register((uint32)h));
   prevIRQL = SP_LockIRQ(&idtLock, SP_IRQL_KERNEL);

   if (vecInfo[vector].exclusive & IDT_VMK) {
      // There is already a handler that requires exclusive use
      Warning("Exclusive use set up already");

   } else if (sharable || (vecInfo[vector].handlers == NULL)) {
      // There is no handler yet or sharing is possible
      handler = Mem_Alloc(sizeof(IDTHandlerInfo));
      if (handler == NULL) {
	 Warning("could not allocate memory");
      } else {
	 handler->func = h;
	 handler->clientData = data;
	 handler->name = name;
	 handler->next = vecInfo[vector].handlers;
	 vecInfo[vector].handlers = handler;

	 if (handler->next) {
	    // There was a handler already, nothing more to do
	    ASSERT(vecInfo[vector].setup & IDT_VMK);
	    ASSERT(vecInfo[vector].flags == flags);
	 } else {
	    vecInfo[vector].setup |= IDT_VMK;
	    vecInfo[vector].exclusive |= sharable ? 0 : IDT_VMK;
	    // Flags must match if already set up for COS
	    if (vecInfo[vector].setup & IDT_HOST) {
	       ASSERT(vecInfo[vector].flags == flags);
	    }
	    vecInfo[vector].flags = flags;
	    // Processor interrupts are automatically enabled
	    if (IDT_VectorIsProcInterrupt(vector)) {
	       vecInfo[vector].enabled |= IDT_VMK;
	    }
	 }

	 success = TRUE;
      }

   } else {
      // There is already a handler and the new one wants exclusive use
      Warning("Unavailable for exclusive use");
   }

   SP_UnlockIRQ(&idtLock, prevIRQL);
      
   return success;
}


/*
 *----------------------------------------------------------------------
 *
 * IDT_VectorEnable --
 *
 *      Enable a vector for a user (host, vmkernel)
 *
 * Results:
 *      None
 *
 * Side effects:
 *      vector is unmasked in the IC if needed
 *
 *----------------------------------------------------------------------
 */

void
IDT_VectorEnable(uint32 vector, uint8 user)
{
   SP_IRQL prevIRQL;
   Bool ok;
   uint8 other = user == IDT_VMK ? IDT_HOST : IDT_VMK;


   Log("0x%x for %s", vector, user == IDT_VMK ? "vmkernel" : "host");

   ASSERT(initDone);
   ASSERT(IDT_VectorIsDevInterrupt(vector));

   prevIRQL = SP_LockIRQ(&idtLock, SP_IRQL_KERNEL);

   ASSERT(vecInfo[vector].setup & user);

   // The vector should not be already in exclusive use by other
   if ((vecInfo[vector].enabled & other)&&(vecInfo[vector].exclusive & other)) {
      Warning("Cannot enable, already in exclusive use by other");
      SP_UnlockIRQ(&idtLock, prevIRQL);
      return;
   }

   switch (user) {
   case IDT_VMK:
      break;
   case IDT_HOST:
      // We need to steer the vector back to HOST_PCPU
      if (vecInfo[vector].destPcpu != HOST_PCPU) {
         ok = Chipset_SteerVector(vector, HOST_PCPU);
         ASSERT(ok);
	 vecInfo[vector].destPcpu = HOST_PCPU;
      }
      break;
   default:
      NOT_REACHED();
   }

   vecInfo[vector].enabled |= user;

   if (vecInfo[vector].enabled != 0) {
      Chipset_UnmaskVector(vector);
   }

   SP_UnlockIRQ(&idtLock, prevIRQL);

   if (user == IDT_HOST) {
      IT_NotifyHostSharing(vector, TRUE);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * IDT_VectorDisable --
 *
 *      Disable a vector for a user (host, vmkernel)
 *
 * Results:
 *      None
 *
 * Side effects:
 *      vector is masked in the IC if needed
 *
 *----------------------------------------------------------------------
 */

void
IDT_VectorDisable(uint32 vector, uint8 user)
{
   SP_IRQL prevIRQL;


   Log("0x%x for %s", vector, user == IDT_VMK ? "vmkernel" : "host");

   ASSERT(initDone);
   ASSERT(IDT_VectorIsDevInterrupt(vector));

   prevIRQL = SP_LockIRQ(&idtLock, SP_IRQL_KERNEL);

   ASSERT(vecInfo[vector].setup & user);

   switch (user) {
   case IDT_VMK:
      break;
   case IDT_HOST:
      break;
   default:
      NOT_REACHED();
   }

   vecInfo[vector].enabled &= ~user;

   if (vecInfo[vector].enabled == 0) {
      Chipset_MaskVector(vector);
   } else if (user == IDT_HOST) {
      /*
       * An interrupt for this vector may have happened and if it is used
       * by the host and is a level-triggered interrupt, it would have been
       * masked in the IC by IDTDoInterrupt(). We rely on its being unmasked
       * in the IC eventually by COS in VMnixEndIRQ(), but since COS is
       * disabling it, it won't get it. So we have to unmask it here as
       * it should be (enabled is not 0, so someone other than COS is also
       * using it).
       */
      Chipset_UnmaskVector(vector);
   }

   SP_UnlockIRQ(&idtLock, prevIRQL);

   if (user == IDT_HOST) {
      IT_NotifyHostSharing(vector, FALSE);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * IDT_VectorSync --
 *
 * 	Check that at one point after this function is called, no pcpu is
 * 	in the vmkernel handler for a given vector.
 *
 * 	This function is only valid for vectors associated to level-
 * 	triggered interrupts when using the IOAPIC but that is the
 *	case for vmkernel handlers.
 *
 * Results:
 * 	None.
 *
 * Side Effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
void
IDT_VectorSync(uint32 vector)
{
   int timeout;

   Log("0x%x", vector);

   ASSERT(IDT_VectorIsDevInterrupt(vector));
   ASSERT(vecInfo[vector].setup & IDT_VMK);
   ASSERT(!(vecInfo[vector].flags & IDT_EDGE));

   /*
    * Before waiting, let's see if we are lucky enough that the vector is
    * not posted according to the system IC.
    */
   if (!Chipset_Posted(vector)) {
      Log("Not posted (maybe in transit)");
      // Wait a bit in case it is in transit
      Util_Udelay(10);
      if (!Chipset_Posted(vector)) {
         return;
      }
      Log("Posted (was in transit)");
   }

   if (INTERRUPTS_ENABLED()) {
      /*
       * If interrupts are enabled, we obviously are not in the handler,
       * and we can wait for other pcpus.
       */
   } else if (Chipset_InServiceLocally(vector)) {
      /*
       * We are currently in the handler, we obviously can't wait on ourself.
       */
      ASSERT(myPRDA.inInterruptHandler);
      Warning("Cannot sync from own interrupt handler");
      return;
   } else if (Chipset_PendingLocally(vector)) {
      /*
       * The vector has been posted for us, so no other pcpu can be in
       * the handler since a vector is only posted once. Moreover we
       * are not in the handler ourself, so nobody is.
       */
      Log("Pending locally");
      return;
   }

   /*
    * We simply wait for the IC to no longer have the vector posted.
    * Since we ack after executing the handlers, that guarantees the
    * handlers have been exited.
    * Since we have to poll, it is possible that we miss the transition
    * and that the vector is posted again. If it's for us then we know
    * there was a transition since we checked earlier it was not, so
    * we can leave (we have to anyway because if interrupts are disabled,
    * we would never get into the handler so we would never ack it
    * and cause a new transition).
    */ 
   timeout = 0;
   while ((timeout++ < 1000) &&
	  Chipset_Posted(vector) &&
	  !Chipset_PendingLocally(vector)) {
      Util_Udelay(1);
   }
   if (timeout == 1000) { // XXX Is 1ms enough ???
      Warning("0x%x still not sync'ed after 1 ms", vector);
      ASSERT_BUG_DEBUGONLY(48431, FALSE);
   }
}


/*
 *----------------------------------------------------------------------
 * IDT_VectorSyncAll --
 *
 *	Wait till no pcpu is in any vmkernel handlers.
 *
 *	This function is deprecated and will be removed eventually.
 *	It does not try any harder that just waiting for 1ms.
 *
 * Results:
 *	None.
 *
 * Side Effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
void
IDT_VectorSyncAll(void)
{
   Log("");

   Util_Udelay(1000);
}


/*
 *----------------------------------------------------------------------
 *
 * IDT_VectorSetDestination --
 *
 *      Steer execution of the handler for the given vector onto
 *	a specific processor.
 *
 * Results:
 *      Success
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
Bool
IDT_VectorSetDestination(uint32 vector, PCPU pcpuNum)
{
   SP_IRQL prevIRQL;
   Bool success = FALSE;

   LOG(1, "0x%x to pcpu %d", vector, pcpuNum);

   ASSERT(IDT_VectorIsDevInterrupt(vector));

   prevIRQL = SP_LockIRQ(&idtLock, SP_IRQL_KERNEL);

   ASSERT(vecInfo[vector].setup & IDT_VMK);

   if (vecInfo[vector].destPcpu != pcpuNum) {

      // vector is currently steered to another pcpu
      if (vecInfo[vector].enabled & IDT_HOST) {
         // vector is used by COS, it must stay on HOST_PCPU
         ASSERT(vecInfo[vector].destPcpu == HOST_PCPU);
         Warning("cannot steer host shared vector 0x%x to pcpu %d",
                        vector, pcpuNum);
      } else {
         success = Chipset_SteerVector(vector, pcpuNum);
      }
   } else {

      // vector is already steered to the correct pcpu
      success = TRUE;

   }

   if (success) {
      vecInfo[vector].destPcpu = pcpuNum;
   }

   SP_UnlockIRQ(&idtLock, prevIRQL);

   return success;
}


/*
 *----------------------------------------------------------------------
 *
 * IDT_VectorRemoveHandler --
 *
 *      Remove an interrupt handler for the given vector.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The list of interrupts handlers is updated.
 *
 *----------------------------------------------------------------------
 */
void
IDT_VectorRemoveHandler(uint32 vector, void *data)
{
   IDTHandlerInfo *handler; 
   IDTHandlerInfo *prev;
   SP_IRQL prevIRQL;
   Bool wasEnabled;

   Log("0x%x", vector);
   
   ASSERT(initDone);
   ASSERT(IDT_VectorIsDevInterrupt(vector));

   prevIRQL = SP_LockIRQ(&idtLock, SP_IRQL_KERNEL);

   ASSERT(vecInfo[vector].setup & IDT_VMK);

   // Mask the interrupt and sync to ensure that nobody can enter
   // or still be in a handler for that vector
   wasEnabled = ((vecInfo[vector].enabled & IDT_VMK) != 0);
   vecInfo[vector].enabled &= ~IDT_VMK;
   Chipset_MaskVector(vector);
   IDT_VectorSync(vector);

   handler = vecInfo[vector].handlers;
   prev = NULL;
   while (handler != NULL) {
      if (handler->clientData == data) {
	 if (prev != NULL) {
	    prev->next = handler->next;
	 } else {
	    vecInfo[vector].handlers = handler->next;
	 }
	 break;
      }
      prev = handler;
      handler = handler->next;
   }

   if (handler == NULL) {
      SysAlert("no matching handler found (0x%x)", (uint32)data);
   } else {
      ASSERT(handler->name != NULL);
      Log("<%s>", handler->name ? handler->name : "");
      if (vecInfo[vector].handlers == NULL) {
	 wasEnabled = FALSE; // nobody left, no need to reenable in any case
         vecInfo[vector].exclusive &= ~IDT_VMK;
      }
      Mem_Free(handler);
   }

   // Restore interrupt mask state
   if (wasEnabled) {
      vecInfo[vector].enabled |= IDT_VMK;
   }
   if (vecInfo[vector].enabled) {
      Chipset_UnmaskVector(vector);
   }

   SP_UnlockIRQ(&idtLock, prevIRQL);
}

/*
 *----------------------------------------------------------------------
 *
 * IDT_RegisterDebugHandler --
 *
 *      Register a handler for the given vector.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The ID
 *
 *----------------------------------------------------------------------
 */
void
IDT_RegisterDebugHandler(uint32 vector, IDT_DebugHandler h)
{
   ASSERT(IDT_VectorIsException(vector));
   ASSERT(debugHandlers[vector] == NULL);

   debugHandlers[vector] = h;
}

/*
 *----------------------------------------------------------------------
 *
 * IDT_HandleException --
 *
 *      Handle an exception.
 *
 * Results:
 *      debug handler eip.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
uint32
IDT_HandleException(VMKExcFrame *regs)
{
   uint32 vector = regs->u.in.gateNum;
   return (uint32)debugHandlers[vector];
}

/*
 *----------------------------------------------------------------------
 *
 * IDTCheckDebugger --
 *
 *      Spin waiting for other CPUs to get out of the debugger.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void 
IDTCheckDebugger(void)
{
   if (UNLIKELY(Debug_InDebugger() || Panic_IsSystemInPanic())) {
      /*
       * Ensure that the world's savedState is up to date.
       */
      if (!CONFIG_OPTION(MINIMAL_PANIC)) {
         World_Switch(MY_RUNNING_WORLD, MY_RUNNING_WORLD);
      }

      while (Debug_InDebugger() || Panic_IsSystemInPanic()) {
      }
   }
}

#ifdef VMX86_DEBUG
/*
 *----------------------------------------------------------------------
 *
 * IDTCheckIntType --
 *
 * 	Check that the type of interrupt we received is the one we expect
 *
 * Results:
 * 	None.
 *
 * Side Effects.
 * 	None.
 *
 *----------------------------------------------------------------------
 */
static void
IDTCheckIntType(uint32 vector)
{
   Bool edge;

   /*
    * This code was added because there is supposedly a bug in P3
    * regarding this and we were getting weird behavior on P3. We
    * haven't seen it fail in a while and the weird behavior stopped
    * probably because of fixes in the IPI code.
    */

   // It has to be an interrupt coming from an external IC.
   if (!IDT_VectorIsDevInterrupt(vector)) {
      return;
   }

   // We can only know what to expect if we have set up the vector
   if (vecInfo[vector].setup == 0) {
      return;
   }

   edge = ((vecInfo[vector].flags & IDT_EDGE) != 0);
   if (!Chipset_GoodTrigger(vector, edge)) {
      SysAlert("Vector 0x%x: %s expected", vector, edge ? "edge" : "level");
      Host_DumpIntrInfo();
      ASSERT(FALSE); // XXX
   }
}
#endif

/*
 *----------------------------------------------------------------------
 *
 * IDTCheckUnexpectedInt --
 *
 * 	Check an interrupt we have no handler for
 *
 * Results:
 * 	None.
 *
 * Side Effects:
 * 	The interrupt may be masked
 *
 *----------------------------------------------------------------------
 */
static uint8
IDTCheckUnexpectedInt(uint32 vector)
{
   SP_IRQL prevIRQL;
   uint8 enabled;
   Bool spurious = FALSE;

   ASSERT(IDT_VectorIsDevInterrupt(vector));

   /*
    * This may be a spurious interrupt due to some chipset idiosyncracy
    * that can be safely ignored.
    * Chipset_Spurious() will check and mask it if so but we need to make
    * sure someone is not racing with us enabling the interrupt otherwise
    * we could end up with the interrupt masked while the system thinks
    * it is unmasked. So we recheck the flag under the appropriate lock
    * and we return it to avoid losing the interrupt if it is now enabled.
    */
   prevIRQL = SP_LockIRQ(&idtLock, SP_IRQL_KERNEL);
   enabled = vecInfo[vector].enabled;
   if (enabled == 0) {
      spurious = Chipset_Spurious(vector);
   }
   SP_UnlockIRQ(&idtLock, prevIRQL);
   if ((enabled != 0) || spurious) {
      return enabled;
   }

   /*
    * We may get unexpected interrupts on vectors we have set up due
    * to a race with handler removal (the handler was removed but the
    * interrupt was already in flight) but we should never get any for
    * vectors that have not been set up.
    */
   if (vecInfo[vector].setup != 0) {
      Log("0x%x received but no handler", vector);
   } else {
      SysAlert("0x%x received but not set up", vector);
      Host_DumpIntrInfo();
      DEBUG_ONLY(ASSERT(FALSE));
   }

   return enabled;
}


/*
 *----------------------------------------------------------------------
 *
 * IDTDoInterrupt --
 *
 *      Handle an interrupt. This function will spin if necessary
 *	waiting for all	other CPUs to get out of the debugger.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
IDTDoInterrupt(uint32 vector)
{
   uint8 enabled;
   Bool mask = FALSE;
   IDTHandlerInfo *handler;

   ASSERT(VMK_IsVMKStack((VA)&vector) || 
          !World_IsVMMWorld(MY_RUNNING_WORLD));
   ASSERT(IDT_VectorIsInterrupt(vector));
   ASSERT(!myPRDA.inInterruptHandler);
   SP_AssertNoIRQLocksHeld();
   ASSERT_NO_INTERRUPTS();

   myPRDA.inInterruptHandler = TRUE;

   intrCounts[MY_PCPU][vector]++;

   IDTCheckDebugger();

#ifdef VMX86_DEBUG
   // Make sure we agree with hardware wrt. edge vs. level
   IDTCheckIntType(vector);
#endif

   enabled = vecInfo[vector].enabled; // snapshot

   // No handler, check the interrupt (this may mask it)
   if (enabled == 0) {
      enabled = IDTCheckUnexpectedInt(vector);
   }

   /*
    * Edge-triggered interrupts must be ack'ed before the handlers are run
    * because the handler may cause the device to interrupt again and that
    * interrupt will be lost by the CPU since it would still be pending.
    *
    * Level-triggered interrupts don't have to be ack'ed now because they
    * remain asserted and will only be delivered when the CPU acks the pending
    * one. If we ack them here though we would also have to mask them because
    * the handler has not run yet so the device has not been ack'ed, the
    * current level is still asserted and the same interrupt would be posted
    * to the CPU again. It's easier to simply ack after the handlers have run.
    */
   if (vecInfo[vector].flags & IDT_EDGE) {
      Chipset_AckVector(vector);
   }

   // Invoke vmkernel handlers
   if (enabled & IDT_VMK) {
      Trace_EventLocal(TRACE_INTERRUPT_DEVICE, vector, vector);
      for (handler = vecInfo[vector].handlers; handler != NULL; handler = handler->next) {
         DEBUG_ONLY(TSCCycles startTsc);
         Bool sysServ = Sched_SysServiceStart(NULL, vector);
	 ASSERT(handler->func != NULL);
         DEBUG_ONLY(startTsc = EventHisto_StartSample());
	 (handler->func)(handler->clientData, vector);
         DEBUG_ONLY(EventHisto_EndSample((uint32)handler->func, startTsc));
         if (sysServ) {
            Sched_SysServiceDone();
         }
      }
   }

   // Invoke host handlers
   if (enabled & IDT_HOST) {
      if (MY_PCPU == HOST_PCPU) {
	 /*
          * We set the irq pending for the host which will eventually run
	  * the corresponding host handlers when the irq is delivered.
          */
         Host_SetPendingIRQ(vecInfo[vector].irq);
	 mask = TRUE;
      } else {
	 /*
	  * The vector must have been steered away and the interrupt was
	  * already pending before it got steered back.
	  * We do not set the interrupt pending for the host as the host
	  * may currently be running and racing with this (e.g. it is
	  * in the process of disabling the interrupt).
	  * This has to be a level-triggered interrupt and since we ack
	  * it below, it will still be there and immediately reposted
	  * this time to HOST_PCPU.
	  */
	 ASSERT(vecInfo[vector].destPcpu == HOST_PCPU);
	 ASSERT(!(vecInfo[vector].flags & IDT_EDGE));
	 Log("0x%x for host on pcpu %d", vector, MY_PCPU);
      }
   }

   /*
    * We can now ack level-triggered interrupts. If there are host handlers
    * for the interrupt then they have not run yet, so unfortunately we also
    * need to mask the interrupt as explained above. We cannot defer ack'ing
    * till the host handlers have run because we don't support nesting
    * interrupts so we have to ack before leaving this routine.
    */
   if (!(vecInfo[vector].flags & IDT_EDGE)) {
      if (mask) {
	 Chipset_MaskAndAckVector(vector);
      } else {
	 Chipset_AckVector(vector);
      }
   }

   myPRDA.inInterruptHandler = FALSE;
}

/*
 *----------------------------------------------------------------------
 *
 * IDTInterruptDuringHalt --
 *
 *      Returns TRUE iff the exception frame "regs" indicates
 *	that the CpuSched idle loop HLT instruction was interrupted.
 *
 * Results:
 *      Returns TRUE iff the exception frame "regs" indicates
 *	that the CpuSched idle loop HLT instruction was interrupted.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
IDTInterruptDuringHalt(const VMKExcFrame *regs)
{
   return((regs->cs == DEFAULT_CS) &&
          (regs->eip == CpuSched_EIPAfterHLT));
}

/*
 *----------------------------------------------------------------------
 *
 * IDT_HandleInterrupt --
 *
 *      Handle an interrupt.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
IDT_HandleInterrupt(VMKExcFrame *regs)
{
   World_Handle *interruptedWorld = MY_RUNNING_WORLD;
   Bool preemptible = CpuSched_DisablePreemption();
   DEBUG_ONLY(TSCCycles startTsc = RDTSC());

   ASSERT(regs->u.in.gateNum != EXC_NMI); //NMIs are done with a task gate

#ifdef VMX86_DEBUG
   if (regs->u.in.gateNum == IDT_APICERROR_VECTOR) {
      Warning("APIC Error at 0x%x:0x%x", regs->cs, regs->eip);
   }
#endif
   
   // notify scheduler if interrupt caused us to exit CpuSched HLT
   if (IDTInterruptDuringHalt(regs)) {
      CpuSched_IdleHaltEnd(TRUE);
   }

   if (UNLIKELY(Dump_LiveDumpRequested())) {
      Dump_LiveDump((VMKFullExcFrame *)((VA)regs - sizeof(VMKExcRegs)));
   }

   IDTDoInterrupt(regs->u.in.gateNum);
   
   if (preemptible) {
      if (interruptedWorld->deathPending) {
         VMLOG(0, interruptedWorld->worldID,
               "deathPending set, descheduling world.");
         World_Exit(VMK_OK);
      }
      
      BH_Check(TRUE); // May switch worlds 
      
      /*
       * If we are returning to an interrupted UserWorld, and it
       * was running user-mode code, then let user module determine
       * if there are any pending items for the world (signals,
       * death, debugger, etc).
       */
      if (World_IsUSERWorld(MY_RUNNING_WORLD)
          && User_SegInUsermode(regs->cs)) {
         User_InterruptCheck(MY_RUNNING_WORLD, regs);
      }
      
      CpuSched_RestorePreemption(preemptible);
      
   }
      
   DEBUG_ONLY(if (initDone) {
      EventHisto_AddSample((uint32)IDT_HandleInterrupt, RDTSC() - startTsc);
         });
}


/*
 *----------------------------------------------------------------------
 *
 * IDT_CheckInterrupt --
 *
 * 	Check if an interrupt should have been handled and do it
 *
 * Results:
 * 	None.
 *
 * Side Effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
void
IDT_CheckInterrupt(void)
{
   uint32 vector;
   ASSERT_NO_INTERRUPTS();
   ASSERT(!myPRDA.inInterruptHandler);

   /*
    * We don't have the interrupt context, get the vector from the chipset.
    *
    * NOTE: Only "normal" interrupts can be recovered from the chipset.
    * NMI, SMI, INIT, start-up, or INIT-deassert interrupts cannot.
    * They do not require any specific action and will simply be lost.
    */
   if (!Chipset_GetInServiceLocally(&vector)) {
      Log("No vector in service");
      return;
   }

   Log("Vector 0x%2x is in service", vector);
   ASSERT(IDT_VectorIsInterrupt(vector));
   /* Process the interrupt. */
   IDTDoInterrupt(vector);
}


/*
 *----------------------------------------------------------------------
 *
 * IDTIntrHandler --
 *
 *      The generic interrupt handler.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The PIC is modified to ACK the interrupt.
 *
 *----------------------------------------------------------------------
 */
static void 
IDTIntrHandler(VMKExcFrame *regs)
{
   IDT_HandleInterrupt(regs);

   if (wantBreakpoint && debugHandlers[EXC_BP] != NULL) {
      wantBreakpoint = FALSE;
      Debug_Break();
      // __asm__("jmp %0" : : "g" ((void *) debugHandlers[EXC_BP]));
   }

   if (User_SegInUsermode(regs->cs)) {
      asm("clts");
   }
}


/*
 *----------------------------------------------------------------------
 *
 * IDTReturnPrepare --
 *
 *      Prepare to return from an exception.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Code is emitted to allow the return to occur.
 *
 *----------------------------------------------------------------------
 */
static void
IDTReturnPrepare(uint32 vector, VMKExcFrame *regs)
{
   VMKFullExcFrame *fullFrame = (VMKFullExcFrame *)((VA)regs - sizeof(VMKExcRegs));
   if (IDT_VectorIsException(vector)) {
      World_Handle *world = PRDA_GetRunningWorldSafe();

      if ((world != NULL) && World_IsVMMWorld(world) &&
          !VMK_IsVMKStack((VA)&vector) && 
          !IDTIsDoubleFaultStack((VA)&vector) &&
          !NMI_IsNMIStack((VA)&vector, world)) {
         ASSERT(regs->eip >= 0 && regs->eip < MAX_MONITOR_VA);
         /* During init the vmm runs with the vmkernel idt for a while. */
         World_Panic(MY_RUNNING_WORLD, "VMM Fault %d @ 0x%x\n", 
                     vector, regs->eip);
         World_Exit(VMK_OK);
      } else {
         if (SELECTOR_RPL(regs->cs) == 3) {
            User_Exception(MY_RUNNING_WORLD, vector, regs);
            NOT_REACHED();
         }

	 /*
	  * Check if we should PSOD, and if so, do it.
	  */
	 if (IDTShouldPSODOnException(vector)) {
	    BlueScreen_PostException(fullFrame);

	    /*
	     * Set the keyboard LEDs to three to signify that we're PSOD'ing
	     * because of a fatal exception.
	     */
            WriteLEDs(3);
	 }

         wantBreakpoint = FALSE;
	
	 /*
	  * Make sure debug handlers have been initialized.
	  */
         if (debugHandlers[vector] == NULL) {
            Panic("Fault %d at eip 0x%x in world %d.  No debug handler set.\n",
		  vector, regs->eip, world ? world->worldID : INVALID_WORLD_ID);
	 }

	 /*
	  * Now enter the debugger.
	  */
	 CommonRetDebug(debugHandlers[regs->u.in.gateNum], regs);      
      }
   } else {
      CommonRet(regs);
   }
   NOT_REACHED();
}

/*
 *----------------------------------------------------------------------
 *
 * IDT_VMMIntOrMCE --
 *
 *      Handle a generic forwarded interrupt from the guest.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Host pending interrupts may be set.
 *
 *----------------------------------------------------------------------
 */
VMKERNEL_ENTRY
IDT_VMMIntOrMCE(DECLARE_ARGS(VMK_VMM_INTORMCE))
{
   PROCESS_2_ARGS(VMK_VMM_INTORMCE, uint32, vector, Reg32, eip);
   ASSERT(VMK_IsVMKStack((VA)&vector));

   if (UNLIKELY(Dump_LiveDumpRequested())) {
      VMKFullExcFrame fullFrame;
      Reg32 ebp = (Reg32)__builtin_frame_address(0);
      Util_CreateVMKFrame(vector, eip, ebp, &fullFrame);
      Dump_LiveDump(&fullFrame);
   }

   ASSERT(vector != EXC_NMI); // the monitor does not forward NMIs
   
   if (UNLIKELY(vector == EXC_MC)) {
      MCE_Handle_Exception();
   } else {
      IDTDoInterrupt(vector);
   }
   
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * IDTShouldPSODOnException -- 
 *
 *	Returns whether we should PSOD based on the exception we took, in
 *	addition to other considerations.
 *
 * Results:
 *	As defined below.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static Bool
IDTShouldPSODOnException(uint32 vector)
{
   /*
    * While we could combine this into a giant if, it's easiest to understand
    * when each case is separated out.
    */

   ASSERT(IDT_VectorIsException(vector));

   /*
    * We shouldn't PSOD if we're already in the debugger.
    */
   if (Debug_InDebugger()) {
      return FALSE;
   }

   /*
    * A request for a "live" dump in the context of an exception makes it a
    * fatal dump (ie, PSOD).  So return TRUE if a live dump has been requested.
    */
   if (Dump_LiveDumpRequested()) {
      return TRUE;
   }

   /*
    * If we hit an int1 (watchpoint exception) or int3 (breakpoint exception),
    * we don't want to bluescreen.
    */
   if (vector == EXC_DB || vector == EXC_BP) {
      return FALSE;
   }

   /*
    * For all other exceptions, we should PSOD.
    */
   return TRUE;
}

/*
 *----------------------------------------------------------------------
 *
 * IDTHandleVMMDF -- 
 *    Handler for doublefaults in vmm worlds. Note that the actual
 *    task is per-vmm-world (mapped@TASK_PAGE_START).
 *
 *    XXX: The stack ought to be per-world.
 *
 * Results:
 *      VMK_ReturnStatus.
 *
 * Side effects:
 *      None.
 *----------------------------------------------------------------------
 */
void
IDTHandleVMMDF(void)
{
   Task *task = (Task *)VPN_2_VA(TASK_PAGE_START);
   asm("cld");
   Serial_PutString("Double fault\n"); // print before dereferencing anything
   CpuSched_DisablePreemption();
   ASSERT(!VMK_IsVMKEip(task->eip));
   World_Panic(MY_RUNNING_WORLD, "VMM DoubleFault @ 0x%x (0x%x, 0x%x)\n",
               task->eip, task->esp, task->ebp);
   World_Exit(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * IDTSetupCurVMMWorldDFPageTable 
 *      Construct pagetable for the DF handler. We duplicate the monitor
 *      pagedirs & tables, but use the original pde to map in the vmkernel.
 *      (The argument is: This is sufficient because the monitor pagetables 
 *      are more perhaps errorprone because of all the manipulations). 
 *
 *      We use KVMap in all the operations below, because we don't want
 *      these mpns to be hanging around in the kseg cache. The point 
 *      is for these to be pretty much inaccessible.
 *
 *      The monitor allocs the MPNs and passes it down so that we don't
 *      have to worry about freeing them - it automagically happens thru
 *      alloc.
 *
 * Results:
 *      Duplicate pagetable root.
 *
 *----------------------------------------------------------------------
 */
static void
IDTSetupCurVMMWorldDFPageTable(MPN pdptMPN, MPN *tables)
{
   MPN pdirMPN = tables[0], ptMPN1 = tables[1], ptMPN2 = tables[2];
   MA curCR3;
   void *dst, *src;
   VMK_PDE *p;
   GET_CR3(curCR3);
   ASSERT((curCR3 & PAGE_MASK) == 0);
   ASSERT(MAX_MONITOR_VA == (4 * 1024 * 1024 - 1));
   ASSERT(IsLowMPN(pdptMPN));
   LA laddr;

   dst = KVMap_MapMPN(pdptMPN, TLB_LOCALONLY);
   src = KVMap_MapMPN(MA_2_MPN(curCR3), TLB_LOCALONLY);
   ASSERT(dst && src);
   /* 
    * We want to duplicate the monitor pdpte (3rd pdpte), but all entries 
    * must be valid. Plus, we want to create a root in which the vmkernel 
    * is mapped in (0th ppdte).
    */ 
   memcpy(dst, src, sizeof(VMK_PDPTE) * NUM_PAE_PDIRS);
   /* Point 1st level to copy. */
   ((VMK_PDPTE *)dst)[MON_PAE_PDPTE] = MAKE_PDPTE(pdirMPN, 0, PTE_P);
   KVMap_FreePages(dst);
   KVMap_FreePages(src);

   dst = KVMap_MapMPN(pdirMPN, TLB_LOCALONLY);
   ASSERT(dst);
   Util_ZeroPage(dst);
   /* Point 2nd levels to copy. */
   ((VMK_PDE *)dst)[MON_PAE_PDINDEX1] = VMK_MAKE_PDE(ptMPN1, 0, PTE_P|PTE_A|PTE_RW);
   ((VMK_PDE *)dst)[MON_PAE_PDINDEX2] = VMK_MAKE_PDE(ptMPN2, 0, PTE_P|PTE_A|PTE_RW);
   KVMap_FreePages(dst);

   /* Copy 3rd level. */
   laddr = VMM_FIRST_LINEAR_ADDR;
   p = PT_GetPageDir(curCR3, laddr, NULL);
   ASSERT(p);
   if (!Util_CopyMA(MPN_2_MA(ptMPN1),
                    MPN_2_MA(VMK_PTE_2_MPN(p[ADDR_PDE_BITS(laddr)])),
                    PAGE_SIZE)) {
      World_Panic(MY_RUNNING_WORLD, "Out of kvmap constructing DF handler?\n");
   }
   PT_ReleasePageDir(p, NULL);

   laddr += PDE_SIZE;
   p = PT_GetPageDir(curCR3, laddr, NULL);
   ASSERT(p);
   if (!Util_CopyMA(MPN_2_MA(ptMPN2),
                    MPN_2_MA(VMK_PTE_2_MPN(p[ADDR_PDE_BITS(laddr)])),
                    PAGE_SIZE)) {
      World_Panic(MY_RUNNING_WORLD, "Out of kvmap constructing DF handler?\n");
   }
   PT_ReleasePageDir(p, NULL);
}

/*
 *----------------------------------------------------------------------
 *
 * IDT_SetupVMMDFHandler -- VMK_SETUP_DF
 *      Setup the double fault handler params for a vmm world. We want
 *      to do only minimal work on a DF. So we set it up such that the
 *      DF task gate switches directly to a pagetable which has the 
 *      vmkernel mapped in, and we use the doubleFaultStack so that
 *      there's no switching of stacks. This gets us into the vmkernel
 *      on a valid stack with no code executed, running IDTHandleVMMDF.
 *
 *      XXX: The doubleFaultStack ought to be per-world.
 *
 * Results:
 *      VMK_ReturnStatus.
 *
 * Side effects:
 *      None.
 *----------------------------------------------------------------------
 */
VMKERNEL_ENTRY
IDT_SetupVMMDFHandler(DECLARE_ARGS(VMK_SETUP_DF))
{
   PROCESS_1_ARG(VMK_SETUP_DF, SetupDF *, sdf);
   sdf->esp = (uint32)(doubleFaultStack + PAGE_SIZE - 4); //Use same stack for vmm/non-vmm worlds.
   sdf->eip = (uint32)IDTHandleVMMDF;
   IDTSetupCurVMMWorldDFPageTable(sdf->root, sdf->mpns);
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * IDTProcRead --
 * 
 *	Callback for read operation on /proc/vmware/interrupts
 *
 * Results:
 * 	VMK_OK
 *
 * Side Effects:
 * 	none
 *
 *----------------------------------------------------------------------
 */
static int
IDTProcRead(Proc_Entry *entry, char *buffer, int *len)
{
   uint32 vector;
   Bool intrSeen;
   PCPU pcpu;
   SP_IRQL prevIRQL;

   *len = 0;
   Proc_Printf(buffer, len, "Vector ");
   for (pcpu = 0; pcpu < numPCPUs; pcpu++) {
      Proc_Printf(buffer, len, "   PCPU %2d ", pcpu);
   }
   Proc_Printf(buffer, len, "\n");

   for (vector = IDT_FIRST_EXTERNAL_VECTOR; vector<IDT_NUM_VECTORS; vector++) {

      // Has an interrupt been seen for this vector on any PCPU ?
      intrSeen = FALSE;
      for (pcpu = 0; pcpu < numPCPUs; pcpu++) {
	  if (intrCounts[pcpu][vector]) {
	     // We may miss it if the counter wrapped, but that's unimportant
	     intrSeen = TRUE;
	     break;
	  }
      }

      prevIRQL = SP_LockIRQ(&idtLock, SP_IRQL_KERNEL);

      // Is the vector potentially used ?
      if ((vecInfo[vector].setup != 0) || intrSeen) {

	 Proc_Printf(buffer, len, "0x%2x:  ", vector);

	 // Occurrences by pcpu
	 for (pcpu = 0; pcpu < numPCPUs; pcpu++) {
	    Proc_Printf(buffer, len, "%10Lu ", intrCounts[pcpu][vector]);
	 }

	 // Is it potentially used by COS ?
	 if (vecInfo[vector].setup & IDT_HOST) {

	    if (!(vecInfo[vector].enabled & IDT_HOST)) {
	       // yes but not actually used now
	       Proc_Printf(buffer, len, "<");
	    }

	    Proc_Printf(buffer, len, "COS irq %d (%s %s)", 
		 vecInfo[vector].irq,
		 vecInfo[vector].flags & IDT_ISA ? "ISA" : "PCI",
		 vecInfo[vector].flags & IDT_EDGE ? "edge" : "level");

	    if (!(vecInfo[vector].enabled & IDT_HOST)) {
	       Proc_Printf(buffer, len, ">");
	    }

	 }

	 // Is it potentially used by vmkernel ?
	 if (vecInfo[vector].setup & IDT_VMK) {

	    if (vecInfo[vector].setup & IDT_HOST) {
	       Proc_Printf(buffer, len, ", ");
	    }

	    if (!(vecInfo[vector].enabled & IDT_VMK)) {
	       // yes but not actually used now
	       Proc_Printf(buffer, len, "<VMK device>");
	    } else {
	       IDTHandlerInfo *handler = vecInfo[vector].handlers;
	       ASSERT(handler != NULL);
	       for (;;) {
		  Proc_Printf(buffer, len, "VMK ");
		  ASSERT(handler->name != NULL);
		  Proc_Printf(buffer, len,
			 handler->name != NULL ? handler->name : "device");
		  handler = handler->next;
		  if (handler != NULL) {
		     Proc_Printf(buffer, len, ", ");
		  } else {
		     break;
		  }
	       }
	    }
	 }

	 Proc_Printf(buffer, len, "\n");

      }

      SP_UnlockIRQ(&idtLock, prevIRQL);
      
   }

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * IDT_LateInit --
 *
 * 	Perform late initialization of the IDT module.
 *
 * Results:
 * 	none
 *
 * Side Effects:
 * 	node /proc/vmware/interrupts is created
 *
 *----------------------------------------------------------------------
 */
void
IDT_LateInit(void)
{
   Proc_InitEntry(&idtProcEntry);
   idtProcEntry.parent = NULL;
   idtProcEntry.read = IDTProcRead;
   idtProcEntry.private = NULL;
   Proc_Register(&idtProcEntry, "interrupts", FALSE);

   DEBUG_ONLY(EventHisto_Register((uint32)IDT_HandleInterrupt));
      
   initDone = TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * IDT_UnshareInterrupts --
 *
 * 	Unshare any interrupts shared with the console os.
 *
 * Results:
 * 	none
 *
 * Side Effects:
 * 	The console os will no longer work.  My cause an interrupt storm
 * 	if a cos device that shares an interrupt with a vmkernel device
 * 	generates an interrupt, since there will no longer be anynone 
 * 	to ack it.
 *
 *----------------------------------------------------------------------
 */

void
IDT_UnshareInterrupts(void)
{
   int i;

   for (i = 0; i < IDT_NUM_VECTORS; i++) {
      if (IDT_VectorIsDevInterrupt(i) &&
          vecInfo[i].setup & IDT_HOST) {
         IDT_VectorDisable(i, IDT_HOST);
      }
   }
}
