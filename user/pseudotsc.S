/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/
#include "asmdefn.sinc"

                .text
/*
 *----------------------------------------------------------------------
 *
 * User_PTSCGet --
 *
 *      Convert a real TSC value on the current PCPU to a pseudo-TSC
 *      that is approximately consistent across all PCPUs.  The
 *      pseudo-TSC runs at approximately the rate of PCPU 0's TSC.
 *      This version of PTSC_Get is copied by the vmkernel into the
 *      ktext page of each user cartel.  From there it is run by the
 *      VMX.  The vmkernel causes the block of code between
 *      BeginAtomic and EndAtomic to be run atomically by backing up
 *      the EIP to the beginning of the block if it interrupts the
 *      user world while the EIP is in the block.
 *
 *      XXX: This code is almost identical to the monitor's
 *      pseudotsc.S.  Combine them somehow?
 *
 * Results:
 *      64-bit timestamp.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

// The assembly code below is a translated and optimized version of
// the following (pseudo) C code:
// 
// uint64
// User_PTSCGet(void)
// {
//    register RateConv_Params params;
//    register uint64 pseudoTSC;
//    extern User_ThreadData *tdata;
// 
//    CriticalSection {
//       params = tdata->pseudoTSCConv;
//       pseudoTSC = RDTSC();
//    };
// 
//    if (!RateConv_IsIdentity(&params)) {
//       pseudoTSC = RateConv_Unsigned(&params, pseudoTSC);
//    } 
// 
//    return pseudoTSC;
// }

                .align  16
                .global User_PTSCGet
                .global User_PTSCGet_CriticalSection
                .global User_PTSCGet_CriticalSectionEnd
                .global User_PTSCGet_End
	
User_PTSCGet:
                push    %edi
                push    %esi
                push    %ebx
User_PTSCGet_CriticalSection:
                mov     $VMK_USER_FIRST_TDATA_VADDR, %ecx

                mov     User_ThreadData_pseudoTSCConv + RateConv_Params_add(%ecx), %ebx
                mov     User_ThreadData_pseudoTSCConv + RateConv_Params_add + 4(%ecx), %esi
                mov     User_ThreadData_pseudoTSCConv + RateConv_Params_mult(%ecx), %edi
                mov     User_ThreadData_pseudoTSCConv + RateConv_Params_shift(%ecx), %ecx
                rdtsc  
User_PTSCGet_CriticalSectionEnd:
                cmp     $0x1, %edi      /* RateConv_IsIdentity? */
                je      identity

                push    %ebx            /* Save add field to free up temp registers */
                push    %esi	
	
                mov     %eax, %esi      /* %edx%eax = Mul64x3264(%edx%eax, %edi, %ecx) */
                mov     %edx, %eax
                mul     %edi
                xchg    %eax, %esi
                mov     %edx, %ebx
                mul     %edi
                add     %esi, %edx
                adc     $0, %ebx
                cmp     $32, %ecx
                jl      shift_lt_32

                mov     %edx, %eax
                mov     %ebx, %edx
                shrd    %cl, %edx, %eax
                shr     %cl, %edx
                jmp     shift_done
shift_lt_32:    
                shrd    %cl, %edx, %eax
                shrd    %cl, %ebx, %edx
shift_done:     	
                pop     %esi            /* Restore add field */
                pop     %ebx

                add     %ebx, %eax      /* Add it in */
                adc     %esi, %edx
identity:
                pop     %ebx
                pop     %esi
                pop     %edi
                ret
User_PTSCGet_End:
