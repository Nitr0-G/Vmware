/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

#include "vmkernel_asm.h"

/*
 * void UserDoCopyIn(Reg seg, void *dest, VA src, int len)
 * AND
 * void UserCopyInDone(void)
 *
 * Copy len bytes from src into dest, using the given segment.
 *
 * The UserCopyInDone entrypoint jumps in to the stack cleanup
 * and should only be used by fault handlers that trigger during
 * the reads during the copy loop.
 */
ENTRY(UserDoCopyIn)
   pushl %ebp
   movl %esp, %ebp

   pushl %ebx
   pushl %ecx
   pushl %edx
   pushl %fs

   movl PARAM1, %eax
   movl PARAM2, %ebx
   movl PARAM3, %ecx
   movl PARAM4, %edx

   movl %eax, %fs

   testl %edx, %edx
   jle 2f   
1:
   movb %fs:(%ecx), %al
   movb %al, (%ebx)
   incl %ecx
   incl %ebx
   decl %edx
   testl %edx, %edx
   jg 1b

2:
.globl UserDoCopyInDone
UserDoCopyInDone:
   popl %fs
   popl %edx
   popl %ecx
   popl %ebx

   leave
   ret

		
/*
 * void UserDoCopyInString(Reg seg,      // IN
 *	                   void *dest,   // IN
 *                         VA src,       // IN
 *                         int* maxLen)  // IN/OUT
 * AND
 * void UserCopyInStringDone(void)
 *
 * Copy at most *maxLen bytes from src into dest, using the given segment,
 * stop at the first 0 byte (string terminator).  If there are no
 * faults, the number of bytes copied will be returned in maxLen
 *
 * Caller MUST ENSURE INITIAL *MAXLEN IS > 1 (there must always be room
 * for the trailing NULL).
 *
 * If no 0 byte is found, *maxlen bytes will be copied and the
 * last byte will be overwritten with a 0.
 *
 * Before returning, *maxLen is updated with the number of bytes copied.
 *
 * The UserCopyInStringDone entrypoint jumps in to the stack cleanup
 * and should only be used by fault handlers that trigger during
 * the reads during the copy loop.
 */
ENTRY(UserDoCopyInString)
   pushl %ebp
   movl %esp, %ebp

   pushl %ebx
   pushl %ecx
   pushl %edx
   pushl %fs

   movl PARAM1, %eax  /* segment */
   movl %eax, %fs
	
   movl PARAM2, %ebx  /* kernel dest */
	
   movl PARAM3, %ecx  /* user src */
	
   movl PARAM4, %eax  /* length addr, deref to get length */
   movl (%eax), %edx
	
	
   decl %edx               /* pre-decrement to reserve a space for trailing \0 */
   testl %edx, %edx        /* Bail if insufficient space */
   jle 2f
	
1:
   movb %fs:(%ecx), %al    /* copy 1 byte from user segment into destination */
   movb %al, (%ebx)	   
	
   testb %al, %al          /* Exit loop if copied the 0 byte.  Already pre-reserved */
   jz   2f	           /* space for 0, so don't change counters. */
	
   incl %ecx               /* inc user addr */
   incl %ebx               /* inc kernel dest */
   decl %edx               /* dec bytes remaining */
	
   testl %edx, %edx        /* Restart loop if bytes remain in kernel buffer */
   jg 1b
	
   /* 
    * Fell through, so we ran out of room in the kernel buffer
    *
    * Terminate kernel-buffer with 0 byte.  Reserved space for this at the
    * top.
    */

   movb $0, (%ebx)
	
2:
   /*
    * Successful exit path, copied a null-terminated string. Compute
    * number of bytes copied by subtracting bytes remaining from original
    * length, store in output parameter.  (Length includes trailing 0.)
    */	
	
   movl PARAM4, %eax  /* addr of length var */
   subl %edx, (%eax)
	

.globl UserDoCopyInStringDone
UserDoCopyInStringDone:
   popl %fs
   popl %edx
   popl %ecx
   popl %ebx

   leave
   ret


/*
 * void UserDoCopyOut(Reg seg, VA dest, const void *src, int len)
 * AND
 * void UserCopyOutDone(void)
 *
 * Like UserDoCopyIn, but in the other direction.
 */
ENTRY(UserDoCopyOut)
   pushl %ebp
   movl %esp, %ebp

   pushl %ebx
   pushl %ecx
   pushl %edx
   pushl %fs

   movl PARAM1, %eax
   movl PARAM2, %ebx
   movl PARAM3, %ecx
   movl PARAM4, %edx

   movl %eax, %fs

   testl %edx, %edx
   jle 2f   
1:
   movb (%ecx), %al
   movb %al, %fs:(%ebx)
   incl %ecx
   incl %ebx
   decl %edx
   testl %edx, %edx
   jg 1b

2:
.globl UserDoCopyOutDone
UserDoCopyOutDone:
   popl %fs
   popl %edx
   popl %ecx
   popl %ebx

   leave
   ret
