#include "asmdefn.sinc"

#define vmkDebug(f) vmkDebug + vmkDebugInfo_##f
#define _regs(r) (World_State_regs + r * 4)
#define _sregs(r) (World_State_segRegs + r * 2)
#define _cr(r) (World_State_CR + r * 4)
#define wstate(field) (World_Handle_savedState + World_State_##field)
#define wstate_regs(reg) (World_Handle_savedState + _regs(reg))
#define wstate_sregs(sreg) (World_Handle_savedState + _sregs(sreg))
#define wstate_cr(cr_num) (World_Handle_savedState + _cr(cr_num))
#define shared_sysenter_msr(msr) (VMK_SharedData_vmmSysenter + SysenterState_##msr)

                /* test_cpuid: Test the 'cpuidFeature' word for a capability
                 *
                 *  feature: A bitmask of the feature which should be tested
                 *
                 *  This macro performs a 'test' instruction of the
                 *  'feature' mask against 'cpuidFeature'.
                 *
                 * post:  Z -> the feature is not available
                 * post: !Z -> the feature is available
                 *
                 * NOTE: This macro perform a small optimization: if
                 * the value to be tested fits within a single byte,
                 * then a single byte will be tested.  If the value is
                 * not a byte, it will be tested as a full 4-byte
                 * word.
                 */
                .macro          test_cpuid feature
                .ifeq           \feature
                .err            /* feature must be non-zero */
                .exitm
                .endif
	
                .ifeq           \feature ^ (\feature & 0xff000000)
                testb           $\feature >> 24, cpuidFeatures + 3
                .exitm
                .endif

                .ifeq           \feature ^ (\feature & 0x00ff0000)
                testb           $\feature >> 16, cpuidFeatures + 2
                .exitm
                .endif

                .ifeq           \feature ^ (\feature & 0x0000ff00)
                testb           $\feature >> 8, cpuidFeatures + 1
                .exitm
                .endif

                .ifeq           \feature ^ (\feature & 0x000000ff)
                testb           $\feature, cpuidFeatures
                .exitm
                .endif
	
                testl           $\feature, cpuidFeatures
                .endm
	
                /* save_cr: save control register
                 *
                 * cr   : control register number
                 * reg  : GP register to use for the save
                 * state: GP register containing the state pointer
                 * pre  : reg != state
                 * post : reg = %cr\cr
                 */
                .macro          save_cr cr, reg, state
                mov             %cr\cr, \reg
                movl            \reg, wstate_cr(\cr)(\state)
                .endm
	
                /* restore_cr: load a saved control register
                 *
                 * cr : control register number
                 * reg  : GP register to use for the restore
                 * state: GP register containing the state pointer
                 * pre  : reg != state
                 * post : reg = %cr\cr
                 */
                .macro          restore_cr cr, reg, state
                movl            wstate_cr(\cr)(\state), \reg
                mov             \reg, %cr\cr
                .endm
	
                /* check_if: Check value of interrupt flag
                 *
                 * This macro traps if the interrupt flag is going to
                 * be enabled in an interrupt handler.
                 *
                 * state: GP register containing the state pointer
                 * reg  : GP register to use for the testing
                 * pre  : \state != \reg
                 * post : \reg destroyed
                 */
                .macro          check_if state reg
#if defined(VMX86_DEBUG)	
                .extern         vmkDebug
                testl           $EFLAGS_IF, wstate(eflags)(\state)
                jz              if_ok_\@
                mov             vmkDebug(inIntHandler), \reg
                test            \reg, \reg
                movzbl          (\reg), \reg
                test            \reg, \reg
                jz              if_ok_\@
                /* attempted to enable interrupts in interrupt handler */
                int3
#endif
if_ok_\@:        
                .endm
	
                /* __cli: Save return EIP and execute CLI
                 *
                 *  When building a debug version of the VMkernel, the
                 *  return EIP is saved to indicate the location where
                 *  a CLI instruction is executed.
                 * 
                 * reg   : GP scratch register
                 * eipreg: GP register used to hold return eip
                 * post  : \reg destroyed
                 * post  : \eipreg destroyed
                 */
                .macro          __cli reg, eipreg
#if defined(VMX86_DEBUG)
#define RETURN_EIP 4(%ebp)
                .extern         vmkDebug
                mov             vmkDebug(lastClrIntrRA), \reg
                test            \reg, \reg
                jz              do_cli_\@
                mov             RETURN_EIP, \eipreg
                mov             \eipreg, (\reg)
#undef RETURN_EIP
#endif
do_cli_\@:      cli
                .endm


                .global         WorldDoSwitch
                .text
/*
 *----------------------------------------------------------------------
 *
 * World_Handle *
 * WorldDoSwitch(World_State     *restore   [eax], 
 *               World_State     *save      [edx],
 *               WorldSwitchKind  kind      [ecx])
 *
 *   This routine performs a switch betwen Worlds.  It is written in
 *   assembly because register & stack access cannot be guaranteed
 *   when using GCC.
 *
 * Results:
 *      Return PREVIOUS.  This avoids having to resort to global
 *      variables to access the previous world in the new context.
 *      Since the return register (%eax) and the first regparms
 *      register are the same, a properly declared StartWorld function
 *      (called at the first switch to a new world) can access the 
 *      previous world as a parameter.
 *
 * Side effects:
 *	Much! Entire CPU state is exchanged. Root page table changed. 
 *
 * GCC ABI says:
 *   callee saved: ebp, esi, edi, ebx
 *   caller saved: eax, ecx, edx
 *
 *----------------------------------------------------------------------
 */
#define _restoreTmp   %ebp
#define _saveTmp      %esi
#define FPU_DISABLE_BITS  (CR0_TS  | CR0_EM)
#define CR0_MUTABLE       (CR0_TS  | CR0_EM  | CR0_MP | CR0_AM)
#define CR4_MUTABLE       (CR4_PGE | CR4_TSD | CR4_DE | CR4_VME | CR4_PVI | \
                           CR4_PCE |CR4_OSFXSR |CR4_OSXMMEXCPT)
WorldDoSwitch:  /* (World_Handle  *restore,     [%eax]
                 *  World_Handle  *save,        [%edx])
                 *
                 *  returns 'save' as the previous world
                 */
                /*
                 * Technically we don't need to save eax, ecx, edx
                 * (and in fact we don't restore them), but we want
                 * to have them in the saved state for debugging.
                 * Also note that we restore %ecx into %eax later.
                 */
                mov             %eax, wstate_regs(REG_EAX)(%edx)
                mov             %ecx, wstate_regs(REG_ECX)(%edx)
                mov             %edx, wstate_regs(REG_EDX)(%edx)
	
                mov             %ebx, wstate_regs(REG_EBX)(%edx)
                mov             %esp, wstate_regs(REG_ESP)(%edx)
                mov             %ebp, wstate_regs(REG_EBP)(%edx)
                mov             %esi, wstate_regs(REG_ESI)(%edx)
                mov             %edi, wstate_regs(REG_EDI)(%edx)
                pushf
                popl            wstate(eflags)(%edx)
 
                __cli           reg=%ecx, eipreg=%ebx

                mov             %es, wstate_sregs(SEG_ES)(%edx)
                mov             %cs, wstate_sregs(SEG_CS)(%edx)
                mov             %ss, wstate_sregs(SEG_SS)(%edx)
                mov             %ds, wstate_sregs(SEG_DS)(%edx)
                mov             %fs, wstate_sregs(SEG_FS)(%edx)
                mov             %gs, wstate_sregs(SEG_GS)(%edx)

                sldt            wstate_sregs(SEG_LDTR)(%edx)
                str             wstate_sregs(SEG_TR)(%edx)

                save_cr         cr=0, state=%edx, reg=%ebx
                and             $~FPU_DISABLE_BITS, %ebx
                mov             %ebx, %cr0
	
                save_cr         cr=2, state=%edx, reg=%ecx
                save_cr         cr=3, state=%edx, reg=%ecx

                /* After saving %cr4, disable PGE so full TLB is
                 * flushed.  Set OSFXSER so FX(SAVE|RESTORE) for SIMD
                 * state
                 */
                save_cr         cr=4, state=%edx, reg=%ebp
                and             $~CR4_PGE, %ebp
                test_cpuid      CPUID_FEATURE_COMMON_ID1EDX_XMM
                jz              no_osfxsr
                or              $CR4_OSFXSR, %ebp
no_osfxsr:      
                mov             %ebp, %cr4
                movl            $WorldSwitch_out_label, wstate(eip)(%edx)

                sidt            wstate(IDTR)(%edx)
                sgdt            wstate(GDTR)(%edx)
                mov             wstate(fpuSaveAreaOffset)(%edx), %ecx
                fxsave          wstate(fpuSaveAreaMem)(%edx, %ecx, 1)

do_world_switch:
                /* World Switch happens here */
                restore_cr      cr=3, state=%eax, reg=%ecx
                restore_cr      cr=2, state=%eax, reg=%ecx
	
                mov             wstate(fpuSaveAreaOffset)(%eax), %ecx
                fxrstor         wstate(fpuSaveAreaMem)(%eax, %ecx, 1)

                /* inv: %ebx = %cr0 which was saved above
                 * inv: %ebp = %cr4 which was saved above
                 */
                mov             wstate_cr(4)(%eax), %ecx
                and             $CR4_MUTABLE, %ecx
                and             $~CR4_MUTABLE, %ebp
                or              %ecx, %ebp
                mov             %ebp, %cr4
	
                mov             wstate_cr(0)(%eax), %ecx
                and             $CR0_MUTABLE, %ecx
                and             $~CR0_MUTABLE, %ebx
                or              %ecx, %ebx
                mov             %ebx, %cr0

                lidt            wstate(IDTR)(%eax)
                lgdt            wstate(GDTR)(%eax)
                lldt            wstate_sregs(SEG_LDTR)(%eax)
                ltr             wstate_sregs(SEG_TR)(%eax)

                test_cpuid      CPUID_FEATURE_COMMON_ID1EDX_SEP
                mov             %eax, _restoreTmp
                jz              no_sysenter_cpu_support

                /* Manage sysenter msrs while switching worlds.
                 *
                 *  1. VMM  <-> VMM    (optimize writes of necessary msrs)
                 *  2. VMM  <-> NVMM   (set cs to 0)
                 *  3. NVMM <-> VMM    (write necessary msrs)
                 *  4. NVMM <-> NVMM   (nop [cs already has 0])
                 */
                mov             %edx, _saveTmp
                mov             wstate_regs(REG_ECX)(%edx), %ecx
                xor             %edx, %edx
                mov             wstate_regs(REG_EAX)(_saveTmp), %edi
                mov             wstate_regs(REG_EDX)(_saveTmp), %ebx
                mov             World_Handle_vmkSharedData(%edi), %edi
                mov             World_Handle_vmkSharedData(%ebx), %ebx

                /* inv: %edi = restore->vmkSharedData
                 * inv: %ebx = save->vmkSharedData
                 * inv: %ecx = kind of world switch
                 */
                xor             %eax, %eax
                test            %ecx, %ecx
                mov             $MSR_SYSENTER_CS, %ecx
                jz              vmm_to_nvmm
                jnp             nvmm_to_vmm
                js              nvmm_to_nvmm
                // fall through for vmm_to_vmm

vmm_to_vmm:
                mov             shared_sysenter_msr(cs)(%edi), %eax
                cmp             shared_sysenter_msr(cs)(%ebx), %eax
                jne             opt_load_cs
        
opt_check_esp:  inc             %ecx
                mov             shared_sysenter_msr(esp)(%edi), %eax
                cmp             shared_sysenter_msr(esp)(%ebx), %eax
                jne             opt_load_esp
	
opt_check_eip:  inc             %ecx
                mov             shared_sysenter_msr(eip)(%edi), %eax
                cmp             shared_sysenter_msr(eip)(%ebx), %eax
                jne             write_eip_msr
                jmp             sysenter_loaded
	
opt_load_cs:    wrmsr
                test            %eax, %eax      /* guest using sysenter? */
                jz              sysenter_loaded /* no -> skip {eip, esp} load */
                jmp             opt_check_esp	
	
opt_load_esp:   wrmsr
                jmp             opt_check_eip

nvmm_to_vmm:    /* inv: %ecx = MSR_SYSENTER_CS */
                mov             shared_sysenter_msr(cs)(%edi), %eax
                test            %eax, %eax      /* guest using sysenter? */
                jz              sysenter_loaded /* no -> skip {cs, eip, esp} */
                wrmsr
                inc             %ecx
                mov             shared_sysenter_msr(esp)(%edi), %eax
                wrmsr
                inc             %ecx
                mov             shared_sysenter_msr(eip)(%edi), %eax
	
vmm_to_nvmm:    /* this label -> %eax = 0, ecx = MSR_SYSENTER_CS */
write_eip_msr:  wrmsr
	
nvmm_to_nvmm:                   /* nop. cs msr already 0 */
sysenter_loaded:
                mov             _saveTmp, %edx

no_sysenter_cpu_support:
                mov             _restoreTmp, %ecx
	
                mov             wstate_regs(REG_EDX)(%edx), %eax
                mov             wstate_sregs(SEG_ES)(%ecx), %es
                mov             wstate_sregs(SEG_SS)(%ecx), %ss
                mov             wstate_sregs(SEG_DS)(%ecx), %ds
                mov             wstate_sregs(SEG_FS)(%ecx), %fs
                mov             wstate_sregs(SEG_GS)(%ecx), %gs
                mov             wstate_regs(REG_EBX)(%ecx), %ebx
                mov             wstate_regs(REG_ESP)(%ecx), %esp
                mov             wstate_regs(REG_EBP)(%ecx), %ebp
                mov             wstate_regs(REG_EDI)(%ecx), %edi
                mov             wstate_regs(REG_ESI)(%ecx), %esi
                push            wstate_sregs(SEG_CS)(%ecx)
                push            wstate(eip)(%ecx)
                lret
	
WorldSwitch_out_label:
                check_if        state=%ecx, reg=%edx
                pushl           wstate(eflags)(%ecx)
                popf

                ret


#undef FPU_DISABLE_BITS
#undef CR0_MUTABLE
#undef CR4_MUTABLE
