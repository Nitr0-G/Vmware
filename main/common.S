/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

#include "vmkernel_asm.h"
#include "asmdefn.sinc"
	
.text
	
ENTRY(CommonIntr)
      mov	%dx, %ds
      mov	%dx, %es

      movl	%esp, %eax
      addl	$__sizeof_VMKExcRegs, %eax
      pushl	%eax

      call	*%ecx
      popl	%eax

      RESTORE_REGS()

      addl      $12, %esp
      iret


ENTRY(CommonTrap)      
      mov	%dx, %ds
      mov	%dx, %es

      movl	%esp, %eax
      addl	$__sizeof_VMKExcRegs, %eax
      pushl	%eax

      call	*%ecx

ENTRY(CommonRet)
	movl    4(%esp),%eax
        subl    $__sizeof_VMKExcRegs,%eax
	movl	%eax, %esp
        RESTORE_REGS()
        addl    $12, %esp
        iret

ENTRY(CommonRetDebug)
	addl	$4, %esp	# Remove return eip.
	popl	%eax		# Get handler pointer.
	jmp	*%eax		# Enter the debugger.

/*
 * Main loop of NMI handler task.
 */
ENTRY(CommonNmiHandler)
        call    IDT_NmiHandler
        iret
        jmp     CommonNmiHandler

/*
 * The NMI handler patches the interrupted task to run this code with a
 * fake stack if the task was at CPL3. This clears TS, which is set by
 * the hardware task switch.
 */
ENTRY(CommonNMIIret)
        clts
        iret

/*	
 * Entry point of the vmkernel.  It is the caller's responsibility to save ebx.
 */

ENTRY(_start)
   /*
    * We are running on our stack but using Linux's code, stack segment
    * and data segment.  Save Linux's segment ids and then switch to our segments.
    */
   pushl %ebp
   movl %esp, %ebp

   pushl %ss
   pushl %ebp
   pushl %ds   
   pushl %es

   pushl $VMNIX_VMK_CS
   pushl $_start2
   lret

_start2:

   /*
    * We are switching the stack from being in the Console OS's address space
    * to being in the vmkernel address space.  We need to preserve the
    * offset in the stack but change the base to be that of the stack
    * in the vmkernel.  I am masking with 16 bits for the offset when
    * 13 bits will do for an 8K stack.  However, since the stack is
    * at least 2 Mbyte aligned (a pdir for PAE) the extra bits don't matter.
    * We can safely increase the size of the stack to 64K without having 
    * to change this mask.
    *
    * Switching the stack pointer is a little tricky if we want to be NMI-safe
    * here.  If real NMIs are possible and we handle them through an interrupt
    * gate, then they use the existing CPL0 stack, so we must not set it to
    * an invalid address by changing %ss and %esp separately.  We use the lss
    * instruction instead.  This instruction must take its operands from 
    * memory, so we push thme onto the old stack.  We don't have to pop them
    * again because we compute the new %esp value using the %esp value from
    * before the pushes.
    */
   movl %esp, %eax
   andl $0xffff, %eax
   addl $VMK_HOST_STACK_BASE, %eax
   pushl $VMNIX_VMK_SS
   pushl %eax
   lss (%esp), %esp
   andl $0xffff, %ebp
   addl $VMK_HOST_STACK_BASE , %ebp

   movl $VMNIX_VMK_DS, %eax
   movl %eax, %ds
   movl %eax, %es

   movl 8(%ebp), %eax
   pushl %eax   

   call InitEarlyVMKernel
	
   addl $4, %esp

   /*
    * Switch back to Linux's view of the world.
    */
   popl %es		# Restore es
   popl %ds		# Restore ds
   lss (%esp), %esp     # Restore %ss and %esp

   popl %ebp

   popl %ebx

   pushl $__VMNIX_CS
   pushl %ebx

   lret

	
/*
 * void
 * StartUserWorld(VMKUserExcFrame *excFrame, // IN
 *                            uint32 segSel) // IN
 *
 *	Start a new user world by setting up the registers for the new
 *	world.  To perform the switch into the user-mode world, we use
 *	an iret which changes most of the state we're concerned with
 *	in one shot. Interestingly, we use the given VMKUserExcFrame *as
 *	the stack* for the iret, then that switches to the stack specified
 *      in excFrame.	 
 *
 *      Also clears all the registers in the new context.  Don't leak kernel
 *      pointers.  SysV i386 ABI spec says edx is to be registered
 * 	with atexit() in new process, so make that null.  %cs, %ds
 * 	and %ss are defined in caller (part of excFrame state).
 *
 * BUG: Switching to a 36 byte stack is not NMI safe.
 *
 * Results:
 * 	Does not return
 *
 * Side effects:
 *	Context is switched to user mode on the
 * 	user mode stack in excFrame->esp, at the
 *	excFrame->eip, etc.
 *
 */
ENTRY(StartUserWorld)
   pushl %ebp
   movl %esp, %ebp

   movl PARAM1, %ebx
   movl PARAM2, %eax

   cli

   clts               /* vmkstats NMI profiling could've set TS */
   finit

   mov	%ax, %ds      /* can't access kernel data after this */
   mov	%ax, %es      /* can't access kernel stack after this */

   mov  %ebx, %esp    /* switch stack to PARAM1 */
   popl %eax	      /* eat the errorcode */

   xorl %eax, %eax    /* Don't leak kernel registers into user mode */
   xorl %ebx, %ebx
   xorl %ecx, %ecx
   xorl %edx, %edx
   xorl %esi, %esi
   xorl %edi, %edi
   xorl %ebp, %ebp

   iret

	
/*
 * StartSlaveWorld
 *
 * The emitted code in SMPSlaveInit calls this function by doing a far return.
 * This function is supposed to switch to slave world's page table and call
 * start function (both values on the stack as described below
 *     Offset:    Value
 *          0     slave world's cr3
 *          4     slave world's start function pointer
 */
ENTRY(StartSlaveWorld)
        popl %eax
        movl %eax, %cr3
	ret
        
