#include "vmkernel_asm.h"
#include "asmdefn.sinc"

.text

/*
 * Routine for entering the vmkernel from the host, because of an
 * interrupt, exception, or system call.
 * HostEntry is a interrupt gate that executes in COS pagetable using 
 * the VMNIX_VMK_xxx segments.
 * This function will perform a task switch to vmkernel context
 * Here's what the host stack looks like at this point:       
 *     Offset:    Value
 *          0     handler
 *          4     gateNum
 *          8     errorCode
 *         12     eip
 *         16     cs
 *         20     eflags
 */
ENTRY(HostEntry)
	cld
	pushl %eax
	pushl %ebx
	
	// store current CR3 on stack, vmkernel side will write to task segment
        movl %cr3, %eax
	pushl %eax

	// save current CR0 so we can restore TS bit after task switch
        movl %cr0, %eax
	pushl %eax
	
	movl 8(%esp), %ebx
	movl 12(%esp), %eax

        // call gate that does a task switch to vmkernel task
        call $VMNIX_VMK_TSS_SEL, $0
	
        // this label helps us catch bad fs and gs 
	.globl HostEntryTaskReturn
HostEntryTaskReturn:
	jmp *%eax
	
/*
 * Macro to restore cr0 to get proper TS bit and eax and ebx after
 * returning from a task-switch to the vmkernel.  The macro first
 * restores cr0, then skips over the saved cr3 value and finally
 * restores eax and ebx.  These values were pushed onto the stack
 * by HostEntry (see above).
 */
#define RESTORE_TS_EAX_EBX() \
        popl %eax;           \
        movl %eax, %cr0;     \
	addl $4, %esp;       \
        popl %ebx;           \
        popl %eax;
        	
// return to host as if nothing happened
ENTRY(HostAsmRetHidden)
        RESTORE_TS_EAX_EBX()
	// remove error code, gate, handler from stack
	addl $12, %esp

       	iret

// return to host and generate a trap gate without errorcode
ENTRY(HostAsmRetGenTrap)
        RESTORE_TS_EAX_EBX()	
	
	addl $4, %esp
        sti
       	lret
	
// return to host and generate a interrupt gate without errorcode
ENTRY(HostAsmRetGenIntr)
        RESTORE_TS_EAX_EBX()	
	
	addl $4, %esp
       	lret
	
// return to host and generate a trap gate with errorcode
ENTRY(HostAsmRetGenTrapErr)
        RESTORE_TS_EAX_EBX()	
	
        sti
       	lret
	
// return to host and generate a interrupt gate with errorcode
ENTRY(HostAsmRetGenIntrErr)
        RESTORE_TS_EAX_EBX()	
	
       	lret


/*
 * This function runs in the vmkernel side of the task switch when
 * entering vmkernel from COS.
 */
ENTRY(HostAsmVMKEntry)
	call HostVMKEntry
        iret
        jmp HostAsmVMKEntry
        

/*
 * extern void CopyFromHostInt(void *dst, const void *src, uint32 length);
 * Copy length bytes from a host address space buffer specified by src
 * into a vmkernel buffer specified by dst.
 * Although we overwrite ds and es, we don't have to clear interrupts
 * because all the gates idt.c set the DS register for the duration of
 * the gate handler.
 * This function uses rep movsb, which has been highly optimized by Intel
 * to do block copies.
*/
ENTRY(CopyFromHostInt)
        pushl %ebp
        movl %esp, %ebp

        pushl %ds
	pushl %esi
	pushl %es
        pushl %edi
	pushl %ecx
	
	// setup arguments for rep movsb
	movl %ds, %eax
        movl %eax, %es
        movl $__VMNIX_DS, %eax
	movl %eax, %ds
        
	movl PARAM1, %edi
        movl PARAM2, %esi
        movl PARAM3, %ecx
	
        cld
	rep movsb
	
	popl %ecx
	popl %edi
	pop %es
        popl %esi
        pop %ds
	
        leave
        ret

/*
 *----------------------------------------------------------------------
 *
 * Host_GetCharDebug --
 *
 *      Should only be used by the debugger!  Copies a character from the 
 *      host at addr, and returns the value.  Will only work if the 
 *      PSOD happened in the host world, _and_ the debugger broke into
 *      cpu 0.
 *
 *	It would be nice to use CopyFromHostInt() here, but DebugHandleMemFault
 *	can't cleanup up properly from faults in that function.  (doesn't
 *	know that it needs to restore %ds).
 *
 * Results:
 *      The chararter at addr in the host.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
ENTRY(Host_GetCharDebug)
        pushl %ebp
        mov %esp, %ebp
        push %fs
        movl $__VMNIX_DS, %eax
        movl %eax, %fs
        movl 0x8(%ebp), %eax
        movb %fs:(%eax), %al
        pop %fs
        popl %ebp
        ret

/*
 * extern void CopyToHostInt(void *dst, const void *src, uint32 length);
 * Copy length bytes from a vmkernel address space buffer specified by src
 * into a host address space buffer specified by dst.
 * Although we overwrite ds and es, we don't have to clear interrupts
 * because all the gates idt.c set the DS register for the duration of
 * the gate handler.
 * This function uses rep movsb, which has been highly optimized by Intel
 * to do block copies.
*/
	
ENTRY(CopyToHostInt)
        pushl %ebp
        movl %esp, %ebp

        pushl %ds
	pushl %esi
	pushl %es
        pushl %edi
	pushl %ecx
	
	// setup arguments for rep movsb
	// don't need to set ds since it's already correct
        movl $__VMNIX_DS, %eax
	movl %eax, %es
        
	movl PARAM1, %edi
        movl PARAM2, %esi
        movl PARAM3, %ecx
	
        cld
	rep movsb
	
	popl %ecx
	popl %edi
	pop %es
        popl %esi
        pop %ds
	
        leave
        ret


ENTRY(BackToHost)
   iret

