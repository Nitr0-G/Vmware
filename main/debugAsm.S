/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

#include "vmkernel_asm.h"
#include "asmdefn.sinc"

	.text
 	.align 16

/*
 *----------------------------------------------------------------------
 *
 * MOVE_ESP_TO_REGISTERS --
 *
 *	When we first enter the debugger, we're still on the faulting
 *	world's stack.  Since we enter the debugger from idt.c, all
 *	the register data for the faulting world has been pushed on the
 *	stack.  Right now esp currently points to a pointer to the
 *	VMKExcFrame for the faulting world (which is located somewhere
 *	on the stack).  So, we want to dereference that pointer, then
 *	subtract the size of VMKExcRegs, which will give us a pointer to
 *	the VMKFullExcFrame.  Finally, we set esp to that pointer so
 *	that the SAVE_EXC_* macros can pop off the register data.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	Changes esp.
 *
 *----------------------------------------------------------------------
 */

#define MOVE_ESP_TO_REGISTERS()					\
	movl (%esp), %eax;					\
	subl $__sizeof_VMKExcRegs, %eax;			\
	movl %eax, %esp;


/*
 *----------------------------------------------------------------------
 *
 * SAVE_EXC_REGISTERS, SAVE_EXC_FRAME --
 *
 *	These macros are used to pop registers data off the stack and
 *	into the registers variable.  When we first break into the
 *	debugger, our stack has a VMKFullExcFrame on it.  So we pop off
 *	data in that order.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	Removes the VMKFullExcFrame from the stack.
 *
 *----------------------------------------------------------------------
 */

#define SAVE_SEG(reg, _addr)					\
	movw reg, _addr;					\
	movw $0, _addr + 2;

#define SAVE_REG(reg, _addr)					\
	movl reg, _addr;

#define POP_AND_SAVE_SEG(_addr)					\
	popl %eax;						\
	movl %eax, _addr;					\
	movw $0, _addr + 2;

#define POP_AND_SAVE_REG(_addr)					\
	popl %eax;						\
	movl %eax, _addr;

#define SAVE_EXC_REGISTERS(registers)				\
	POP_AND_SAVE_SEG(registers + DebugRegisterFile_es)	\
	POP_AND_SAVE_SEG(registers + DebugRegisterFile_ds)	\
	POP_AND_SAVE_SEG(registers + DebugRegisterFile_fs)	\
	POP_AND_SAVE_SEG(registers + DebugRegisterFile_gs)	\
	POP_AND_SAVE_REG(registers + DebugRegisterFile_eax)	\
	POP_AND_SAVE_REG(registers + DebugRegisterFile_ecx)	\
	POP_AND_SAVE_REG(registers + DebugRegisterFile_edx)	\
	POP_AND_SAVE_REG(registers + DebugRegisterFile_ebx)	\
	POP_AND_SAVE_REG(registers + DebugRegisterFile_ebp)	\
	POP_AND_SAVE_REG(registers + DebugRegisterFile_esi)	\
	POP_AND_SAVE_REG(registers + DebugRegisterFile_edi)

#define SAVE_EXC_FRAME(registers)				\
	/* Remove unused elements from front of VMKExcFrame. */ \
	addl $8, %esp;						\
	POP_AND_SAVE_REG(gdb_i386errcode)			\
	POP_AND_SAVE_REG(registers + DebugRegisterFile_eip)	\
	POP_AND_SAVE_SEG(registers + DebugRegisterFile_cs)	\
	POP_AND_SAVE_REG(registers + DebugRegisterFile_eflags)	\
	SAVE_REG(%esp, registers + DebugRegisterFile_esp)	\
	SAVE_SEG(%ss, registers + DebugRegisterFile_ss)



/*
 *----------------------------------------------------------------------
 *
 * DebugEntry --
 *
 *	First checks to see if this world should enter the debugger,
 *	then either does or waits for the current debugger to exit.
 *
 *	This function is run on the faulting world's stack.  So, we
 *	shouldn't perform any operations that modify esp (except for
 *	popping off the register data on the stack).  We do have free
 *	reign over the other registers though.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

.globl DebugEntry
DebugEntry:
	/* If PRDA isn't initialized, just jump straight into debugger. */
	movl prdaInitialized, %eax
	cmpl $0, %eax
	je setup_debugger

	/* Get address of PRDA. */
	movl $VMK_FIRST_PRDA_VPN * PAGE_SIZE, %eax
	/* Add PRDA_runningWorld to get runningWorld offset in PRDA. */
	addl $PRDA_runningWorld, %eax
	/* Deference the memory address to get the runningWorld pointer. */
	movl (%eax), %eax

	/*
	 * Check if MY_RUNNING_WORLD equals worldInDebugger, and if so,
	 * go directly into the debugger.
	 */
	subl worldInDebugger, %eax
	cmpl $0, %eax
	je setup_debugger

	/*
	 * Atomically exchange eax and debugInDebugger.  After the
	 * xchg instruction, eax will hold the old value of
	 * debugInDebugger.  If eax is zero, it means no one's in the
	 * debugger yet, so we should feel free to enter it.
	 */
	movl $1, %eax
	lock
	xchg %eax, debugInDebugger
	cmpl $0, %eax
	je setup_debugger

	/* Wait for debugger to exit. */
wait_for_debugger:
	pause
	cmpb $0, debugInDebugger
	jne wait_for_debugger
	call CommonRet

	/*
	 * Prepare to enter the debugger.  This entails setting up a new
	 * stack.  Which stack we use depends on whether we cleanly
	 * exited the debugger last time.  If we did, just use the regular
	 * stack.  Otherwise, since we want to be able to debug the
	 * debugger, use a backup stack.
	 */
setup_debugger:
	/* Save the exception vector across the stack switch. */
	popl %ecx

	MOVE_ESP_TO_REGISTERS()

	/*
	 * We do slightly different initialization depending on whether we
	 * successfully exited the debugger.
	 */
	cmpb $0, cleanExit
	jne use_normal_stack

	/* Not a clean exit.  Use backup register file and stack. */
	SAVE_EXC_REGISTERS(backupRegisters)
	SAVE_EXC_FRAME(backupRegisters)
	movl backupStackPtr, %esp
	jmp start_debugging

	/* Clean exit, we can use the regular register file and stack. */
use_normal_stack:
	SAVE_EXC_REGISTERS(defaultRegisters)
	SAVE_EXC_FRAME(defaultRegisters)
	movl defaultStackPtr, %esp

	/* We're set up.  Push on the exception vector and start debugging. */
start_debugging:
	movl $0, %ebp
	pushl %ecx
	call DebugHandleException
	/* Will never return. */


/*
 *----------------------------------------------------------------------
 *
 * DebugCatchException --
 *
 *	Below we define the low level exception handlers that are run
 *	when the debugger is first called.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

#define MAKE_EXCEPTION_HANDLER(num)				\
   .globl DebugCatchException##num;				\
   DebugCatchException##num:					\
   	pushl $num;						\
	jmp DebugEntry;

/*
 * We special case the fault handlers.  The idea here is that since DebugGetChar
 * and DebugSetChar perform operations on memory locations specified by the
 * user, there's a good chance we may take a fault.  To prevent against
 * re-entering the debugger, we instead redirect the fault handling to
 * DebugHandleMemFault.  It will change the return eip to a function that will
 * indicate the failure gracefully.
 */
#define MAKE_EXCEPTION_HANDLER_FAULT(num)			\
   .globl DebugCatchException##num;				\
   DebugCatchException##num:					\
   	cmpl $0, mem_fault_routine;				\
	jne DebugHandleMemFault;				\
	pushl $num;						\
	jmp DebugEntry;

MAKE_EXCEPTION_HANDLER(0)
MAKE_EXCEPTION_HANDLER(1)
MAKE_EXCEPTION_HANDLER(3)
MAKE_EXCEPTION_HANDLER(4)
MAKE_EXCEPTION_HANDLER(5)
MAKE_EXCEPTION_HANDLER(6)
MAKE_EXCEPTION_HANDLER(7)
MAKE_EXCEPTION_HANDLER(8)
MAKE_EXCEPTION_HANDLER(9)
MAKE_EXCEPTION_HANDLER(10)
MAKE_EXCEPTION_HANDLER(12)
MAKE_EXCEPTION_HANDLER(16)

MAKE_EXCEPTION_HANDLER_FAULT(11)
MAKE_EXCEPTION_HANDLER_FAULT(13)
MAKE_EXCEPTION_HANDLER_FAULT(14)


/*
 *----------------------------------------------------------------------
 *
 * DebugReturnToProg --
 *
 *	Returns execution to the faulting code.  Restores the program's
 *	registers (including the stack) and returns.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      If debugInCall is not set, debugInDebugger is set to FALSE.
 *
 *----------------------------------------------------------------------
 */

.globl DebugReturnToProg
DebugReturnToProg:
	/* Restore the registers. */
	movw defaultRegisters + DebugRegisterFile_ss, %ss
	movl defaultRegisters + DebugRegisterFile_esp, %esp
	movl defaultRegisters + DebugRegisterFile_ecx, %ecx
	movl defaultRegisters + DebugRegisterFile_edx, %edx
	movl defaultRegisters + DebugRegisterFile_ebx, %ebx
	movl defaultRegisters + DebugRegisterFile_ebp, %ebp
	movl defaultRegisters + DebugRegisterFile_esi, %esi
	movl defaultRegisters + DebugRegisterFile_edi, %edi
	movw defaultRegisters + DebugRegisterFile_ds, %ds
	movw defaultRegisters + DebugRegisterFile_es, %es
	movw defaultRegisters + DebugRegisterFile_fs, %fs
	movw defaultRegisters + DebugRegisterFile_gs, %gs

	/* Put eflags, cs, and eip back on the stack. */
	movl defaultRegisters + DebugRegisterFile_eflags, %eax
	pushl %eax

	movl defaultRegisters + DebugRegisterFile_cs, %eax
	pushl %eax

	movl defaultRegisters + DebugRegisterFile_eip, %eax
	pushl %eax

	movl defaultRegisters + DebugRegisterFile_eax, %eax

	/*
	 * If debug in call is set, we don't want to change worldInDebugger or
	 * debugInDebugger, as we'll soon re-enter the debugger.
	 */
	cmpb $0, debugInCall
	jne restore_done

	/*
	 * It looks like we're actually leaving the debugger.  So, clear
	 * worldInDebugger and debugInDebugger.  We must save setting
	 * debugInDebugger for last because once it's turned off, other
	 * worlds on other processors will start executing again and may
	 * quickly hit a breakpoint or watchpoint.  Thus we must be ready to
	 * re-enter the debugger immediately.
	 */
	movl $0, worldInDebugger
	movb $0, debugInDebugger

restore_done:
	/*
	 * At this point nothing else can go wrong in the debugger, so
	 * we can set cleanExit to TRUE.
	 */
	movb $1, cleanExit

	iret


/*
 *----------------------------------------------------------------------
 *
 * DebugHandleMemFault --
 *
 *	Saves the error code and cleans up the stack.  Since we know we
 *	faulted in debugger code (DebugGet/SetChar), we don't want to
 *	return there, as we'd just fault again.  So instead we return to
 *	the mem_fault_routine.  Because we set the eip to the beginning
 *	of mem_fault_routine, it's just like the function was called by
 *	whomever called DebugGet/SetChar because the return address is
 *	at the top of the stack.  Thus, when mem_fault_routine returns,
 *	it will return to the place where DebugGet/SetChar was called.
 *	That calling function will then check if a memory error has
 *	occurred (set by the mem_fault_routine) and if so, return with
 *	an error code.  Thus the debugger can access any memory it
 *	likes and not die in the process.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	Munges the stack.
 *
 *----------------------------------------------------------------------
 */

.globl DebugHandleMemFault
DebugHandleMemFault:
	/* First, restore all the registers from the VMKExcRegs struct. */
	MOVE_ESP_TO_REGISTERS()
	RESTORE_REGS()

	/* Move esp past the unused union at the front of VMKExcFrame. */
	addl $8, %esp

	/*
	 * Note: Because we're setting eip to the beginning of a new function,
	 * we can freely use eax, ecx, and edx, as those registers "belong"
	 * to the callee function according to the Sys V ABI.
	 */

	/* Save the error code. */
	popl %eax
	movl %eax, gdb_i386errcode

	/*
	 * Save eip, cs, and eflags, replacing eip with a pointer to the
	 * memory fault routine.
	 */
	popl %eax
	movl mem_fault_routine, %eax
	popl %ecx
	popl %edx

	/* Leave the DebugGet/SetChar frame. */
	leave

	/* Push (the new) eip, cs, eflags back on the stack. */
	pushl %edx
	pushl %ecx
	pushl %eax

	/* Zero the mem fault routine. */
	movl $0, %eax
	movl %eax, mem_fault_routine

	iret


/*
 *----------------------------------------------------------------------
 *
 * DebugGet/SetChar --
 *
 *	Read/Writes a single character from/to the given address.  The
 *	functions are coded in assembly because the order of the
 *	operations is extremely important.  We need to know whether the
 *	"pop %ebp" comes before the dereference or after it, so we can
 *	know whether we need to remove the frame ourselves in the
 *	memory fault handler.  It seems that gcc can switch the order of
 *	the pop and the deference depending on what compilation options
 *	are given.  Thus it's safest just to do it explicitly ourselves.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	One byte is read or written.
 *
 *----------------------------------------------------------------------
 */

/*
 * int DebugGetChar(char *addr)
 * {
 *    return *addr;
 * }
 */
.globl DebugGetChar
DebugGetChar:
	push %ebp
	mov %esp, %ebp
	mov 8(%ebp), %eax
	movsbl (%eax), %eax
	pop %ebp
	ret

/*
 * void DebugSetChar(char *addr, int val)
 * {
 *    *addr = val;
 * }
 */
.globl DebugSetChar
DebugSetChar:
	push %ebp
	mov %esp, %ebp
	mov 12(%ebp), %edx
	mov 8(%ebp), %eax
	mov %dl, (%eax)
	pop %ebp
	ret


