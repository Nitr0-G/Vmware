/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

#include "vmkernel_asm.h"

#define IDT_LINUXSYSCALL_VECTOR		0x80
#define LINUXSYSCALL_NR_rt_sigreturn    173

/*
 *----------------------------------------------------------------------
 *
 * UserSigDispatch --
 *
 *	Expects the stack to be all setup for the handler.  Calls the
 *	handler and (if) the handler returns passes the saved context 
 *	to the sigreturn syscall (see UserSig_ReturnFromHandler).  Since 
 * 	the savedcontext will be restored, the sigreturn system call 
 * 	won't return (normally) it will restore to the context that 
 *	caused the signal to be dispatched.
 *
 *      Note: The body of this function must be completely 
 *	relocatable.
 *	
 * Results:
 *	None.
 *
 * Side effects:
 *	Calls given handler then restores saved context
 *
 *----------------------------------------------------------------------
 */
ENTRY(UserSigDispatch)
	// Setup code is in UserSigDispatchInFrame.
	
	// Kernel setup some registers to make this simpler:
	// Put the handler address in %eax
	// Put the UserSigRestoreContext address in %esi

	// Kernel setup stack for signal handler:
	// Pushed ebp/eip to keep call stack walkable
	// Pushed the full UserSigRestoreContext
	// Pushed the signum

	// Call signal handler
	call	*%eax

	// Setup registers for sigreturn syscall:
	// %eax: syscall number
	// %ebx: addr of UserSigRestoreContext
	movl	$LINUXSYSCALL_NR_rt_sigreturn, %eax
	movl	%esi, %ebx 

	// Call rt_sigreturn syscall
	int	$IDT_LINUXSYSCALL_VECTOR
	
	// Does not return....

	// This label lets us know how big UserSigDispatch is:	
	.globl UserSigDispatchEnd
UserSigDispatchEnd:
	nop
	
