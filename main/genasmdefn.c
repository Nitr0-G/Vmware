/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <time.h>
#include <getopt.h>
#include <assert.h>

#include "vm_types.h"
#include "vm_libc.h"
#include "x86.h"
#include "vmkernel.h"
#include "vmnix_if.h"
#include "kvmap.h"
#include "memmap.h"
#include "world.h"
#include "rateconv.h"
#include "user_layout.h"
#include "debug.h"

#define DEFINE(fp, bufsz, fmt, val) \
{ char buf[bufsz]; snprintf(buf, bufsz, fmt, val), define(fp, #val, buf); }

static void define(FILE *fp,
                   char const * const symbol,
                   char const * const value)
{
   fprintf(fp, "#define %s %s\n", symbol, value);
}

static void comment(FILE *fp, char const * const name)
{
   fprintf(fp, "/**** %s */\n", name);
}

static void fileHeader(FILE *fp, char const * const fileName)
{
   fprintf(fp,
           "/* %s */\n"
           "/* This file is generated.  DO NOT EDIT. */\n\n", fileName);
}

static void fileFooter(FILE *fp)
{
   fprintf(fp, "\n\n");
}

static void struct_field(FILE *fp,
                         char const * const field,
                         int offset_after)
{
   fprintf(fp, "\t\t.struct\t%d\n%s:\n", offset_after, field);
}

static void struct_size(FILE *fp, char const * const type, unsigned int size)
{
   /* Do not parenthesize the expression representing the size of the
    * structure because gas has a defect where only the integer
    * literal 1, 2, 4 or 8 can be used as a scalling factor.  Adding
    * the '(' & ')' will not work.
    */
   fprintf(fp, "#define __sizeof_%s %u\n", type, size);
}

static void external_reference(FILE *fp,
                               char const * const var,
                               char const * const type,
                               char const * const definition)
{
   fprintf(fp, "\t\t.extern\t%s /* %s [%s] */\n",
           var, type, definition);
}

static void outputRegisterNames(FILE *fp)
{
   comment(fp, "Register Names");
   DEFINE(fp, 32, "%#x", REG_NULL);
   DEFINE(fp, 32, "%#x", REG_EAX);
   DEFINE(fp, 32, "%#x", REG_ECX);
   DEFINE(fp, 32, "%#x", REG_EDX);
   DEFINE(fp, 32, "%#x", REG_EBX);
   DEFINE(fp, 32, "%#x", REG_ESP);
   DEFINE(fp, 32, "%#x", REG_EBP);
   DEFINE(fp, 32, "%#x", REG_ESI);
   DEFINE(fp, 32, "%#x", REG_EDI);

   DEFINE(fp, 32, "%#x", SEG_CS);
   DEFINE(fp, 32, "%#x", SEG_DS);
   DEFINE(fp, 32, "%#x", SEG_ES);
   DEFINE(fp, 32, "%#x", SEG_SS);
   DEFINE(fp, 32, "%#x", SEG_FS);
   DEFINE(fp, 32, "%#x", SEG_GS);
   DEFINE(fp, 32, "%#x", SEG_TR);
   DEFINE(fp, 32, "%#x", SEG_LDTR);
   comment(fp, "end Register Names");
}

static void outputCPUID(FILE *fp)
{
   DEFINE(fp, 32, "%#x", CPUID_FEATURE_COMMON_ID1EDX_XMM);
   DEFINE(fp, 32, "%#x", CPUID_FEATURE_COMMON_ID1EDX_FXSAVE);
   DEFINE(fp, 32, "%#x", CPUID_FEATURE_COMMON_ID1EDX_SEP);
}

static void outputCR(FILE *fp)
{
   DEFINE(fp, 32, "%#x", CR4_OSFXSR);
   DEFINE(fp, 32, "%#x", CR4_PGE);
   DEFINE(fp, 32, "%#x", CR4_DE);
   DEFINE(fp, 32, "%#x", CR4_PCE);
   DEFINE(fp, 32, "%#x", CR4_OSXMMEXCPT);
   DEFINE(fp, 32, "%#x", CR4_TSD);
   DEFINE(fp, 32, "%#x", CR4_VME);
   DEFINE(fp, 32, "%#x", CR4_PVI);
   DEFINE(fp, 32, "%#x", CR0_TS);
   DEFINE(fp, 32, "%#x", CR0_EM);
   DEFINE(fp, 32, "%#x", CR0_MP);
   DEFINE(fp, 32, "%#x", CR0_AM);
}

static void outputEFLAGS(FILE *fp)
{
   comment(fp, "EFLAGS (x86.h)");
   DEFINE(fp, 32, "%#x", EFLAGS_CF);
   DEFINE(fp, 32, "%#x", EFLAGS_SET);
   DEFINE(fp, 32, "%#x", EFLAGS_PF);
   DEFINE(fp, 32, "%#x", EFLAGS_AF);
   DEFINE(fp, 32, "%#x", EFLAGS_ZF);
   DEFINE(fp, 32, "%#x", EFLAGS_SF);
   DEFINE(fp, 32, "%#x", EFLAGS_TF);
   DEFINE(fp, 32, "%#x", EFLAGS_IF);
   DEFINE(fp, 32, "%#x", EFLAGS_DF);
   DEFINE(fp, 32, "%#x", EFLAGS_OF);
   DEFINE(fp, 32, "%#x", EFLAGS_IOPL);
   DEFINE(fp, 32, "%#x", EFLAGS_NT);
   DEFINE(fp, 32, "%#x", EFLAGS_RF);
   DEFINE(fp, 32, "%#x", EFLAGS_VM);
   DEFINE(fp, 32, "%#x", EFLAGS_AC);
   DEFINE(fp, 32, "%#x", EFLAGS_VIF);
   DEFINE(fp, 32, "%#x", EFLAGS_VIP);
   DEFINE(fp, 32, "%#x", EFLAGS_ID);
   DEFINE(fp, 32, "%#x", EFLAGS_ALL);
   DEFINE(fp, 32, "%#x", EFLAGS_REAL_32);
   DEFINE(fp, 32, "%#x", EFLAGS_V8086_32);
   DEFINE(fp, 32, "%#x", EFLAGS_ALL_16);
   DEFINE(fp, 32, "%#x", EFLAGS_REAL_16);
   DEFINE(fp, 32, "%#x", EFLAGS_V8086_16);
   DEFINE(fp, 32, "%#x", EFLAGS_CLEAR_ON_EXC);
   DEFINE(fp, 32, "%#x", EFLAGS_IOPL_SHIFT);
   DEFINE(fp, 32, "%#x", EFLAGS_PRIV);
   DEFINE(fp, 32, "%#x", EFLAGS_USER);
   comment(fp, "end EFLAGS (x86.h)");
}

static void outputMSR(FILE *fp)
{
   DEFINE(fp, 32, "%#x", MSR_SYSENTER_CS);
   DEFINE(fp, 32, "%#x", MSR_SYSENTER_EIP);
   DEFINE(fp, 32, "%#x", MSR_SYSENTER_ESP);
}

static void outputWorld(FILE *fp)
{
   comment(fp, "World_State");
   struct_field(fp, "World_State_regs", offsetof(World_State, regs));
   struct_field(fp, "World_State_segRegs", offsetof(World_State, segRegs));
   struct_field(fp, "World_State_DR", offsetof(World_State, DR));
   struct_field(fp, "World_State_CR", offsetof(World_State, CR));
   struct_field(fp, "World_State_eip", offsetof(World_State, eip));
   struct_field(fp, "World_State_eflags", offsetof(World_State, eflags));
   struct_field(fp, "World_State_IDTR", offsetof(World_State, IDTR));
   struct_field(fp, "World_State_GDTR", offsetof(World_State, GDTR));
   struct_field(fp, "World_State_fpuSaveAreaOffset", offsetof(World_State, fpuSaveAreaOffset));
   struct_field(fp, "World_State_fpuSaveAreaMem", offsetof(World_State, fpuSaveAreaMem));
   struct_size(fp, "World_State", sizeof(World_State));

   external_reference(fp, "cpuidFeatures", "uint32", "vmkernel/main/world.c");
   comment(fp, "end World_State");

   comment(fp, "World_Handle");
   struct_field(fp, "World_Handle_savedState", offsetof(World_Handle, savedState));
   struct_field(fp, "World_Handle_vmkSharedData", offsetof(World_Handle, vmkSharedData));
   struct_size(fp, "World_Handle", sizeof(World_Handle));
   comment(fp, "end World_Handle");
}

static void outputVMKDebug(FILE *fp)
{
#if defined(VMX86_DEBUG)
   struct_field(fp, "vmkDebugInfo_lastClrIntrRA",
                 offsetof(vmkDebugInfo, lastClrIntrRA));
    struct_field(fp, "vmkDebugInfo_inIntHandler",
                 offsetof(vmkDebugInfo, inIntHandler));
#endif
}

static void outputSysenter(FILE *fp)
{
   struct_field(fp, "SysenterState_cs",
                offsetof(SysenterState, requestedCS));
   struct_field(fp, "SysenterState_eip",
                offsetof(SysenterState, hw.sysenterRIP));
   struct_field(fp, "SysenterState_esp",
                offsetof(SysenterState, hw.sysenterRSP));
}

static void outputVMKSharedData(FILE *fp)
{
   struct_field(fp, "VMK_SharedData_vmmSysenter",
                offsetof(VMK_SharedData, vmm32Sysenter));
}

static void outputRateConvParams(FILE *fp)
{
   struct_field(fp, "RateConv_Params_mult",
                offsetof(RateConv_Params, mult));
   struct_field(fp, "RateConv_Params_shift",
                offsetof(RateConv_Params, shift));
   struct_field(fp, "RateConv_Params_add",
                offsetof(RateConv_Params, add));
}

static void outputUserThreadData(FILE *fp)
{
   struct_field(fp, "User_ThreadData_pseudoTSCConv",
                offsetof(User_ThreadData, pseudoTSCConv));
   DEFINE(fp, 32, "%#x", VMK_USER_FIRST_TDATA_VADDR);
}

static void outputDebugAsm(FILE *fp)
{
   struct_field(fp, "PRDA_runningWorld", offsetof(struct PRDA, runningWorld));
   struct_field(fp, "DebugRegisterFile_eax", offsetof(DebugRegisterFile, eax));
   struct_field(fp, "DebugRegisterFile_ecx", offsetof(DebugRegisterFile, ecx));
   struct_field(fp, "DebugRegisterFile_edx", offsetof(DebugRegisterFile, edx));
   struct_field(fp, "DebugRegisterFile_ebx", offsetof(DebugRegisterFile, ebx));
   struct_field(fp, "DebugRegisterFile_esp", offsetof(DebugRegisterFile, esp));
   struct_field(fp, "DebugRegisterFile_ebp", offsetof(DebugRegisterFile, ebp));
   struct_field(fp, "DebugRegisterFile_esi", offsetof(DebugRegisterFile, esi));
   struct_field(fp, "DebugRegisterFile_edi", offsetof(DebugRegisterFile, edi));
   struct_field(fp, "DebugRegisterFile_eip", offsetof(DebugRegisterFile, eip));
   struct_field(fp, "DebugRegisterFile_eflags",
		offsetof(DebugRegisterFile, eflags));
   struct_field(fp, "DebugRegisterFile_cs", offsetof(DebugRegisterFile, cs));
   struct_field(fp, "DebugRegisterFile_ss", offsetof(DebugRegisterFile, ss));
   struct_field(fp, "DebugRegisterFile_ds", offsetof(DebugRegisterFile, ds));
   struct_field(fp, "DebugRegisterFile_es", offsetof(DebugRegisterFile, es));
   struct_field(fp, "DebugRegisterFile_fs", offsetof(DebugRegisterFile, fs));
   struct_field(fp, "DebugRegisterFile_gs", offsetof(DebugRegisterFile, gs));
   DEFINE(fp, 32, "%#x", VMK_FIRST_PRDA_VPN);
}

static void outputBasicDefs(FILE *fp)
{
   comment(fp, "Basic defs");
   DEFINE(fp, 32, "%#x", PAGE_SIZE);
   struct_size(fp, "VMKExcRegs", sizeof(VMKExcRegs));
   DEFINE(fp, 32, "%#x", CR4_PGE);
   DEFINE(fp, 32, "%#x", VMK_HOST_STACK_BASE);
   DEFINE(fp, 32, "%#x", VMNIX_VMK_SS);
   DEFINE(fp, 32, "%#x", VMNIX_VMK_DS);
   DEFINE(fp, 32, "%#x", VMNIX_VMK_CS);
   DEFINE(fp, 32, "%#x", VMNIX_VMK_TSS_SEL);
   DEFINE(fp, 32, "%#x", __VMNIX_CS);
   DEFINE(fp, 32, "%#x", __VMNIX_DS);
}


static void writeFile(FILE *fp, const char * const fileName)
{
   fileHeader(fp, fileName);
   outputRegisterNames(fp);
   outputWorld(fp);
   outputSysenter(fp);
   outputBasicDefs(fp);
   outputVMKSharedData(fp);
   outputCPUID(fp);
   outputCR(fp);
   outputEFLAGS(fp);
   outputVMKDebug(fp);
   outputMSR(fp);
   outputRateConvParams(fp);
   outputUserThreadData(fp);
   outputDebugAsm(fp);
   fileFooter(fp);
}


int main(int argc, char *argv[], UNUSED_PARAM(char *envp[]))
{
   static struct option long_options[] = {
      { "help"       , no_argument      , NULL, 256 },
      { "output"     , required_argument, NULL, 257 },
      { NULL         , no_argument      , NULL, 0   }
   };

   char *outputPathname = NULL;
   int   c;
   int   option_index;
   FILE *fp;
   
   while (1) {
      c = getopt_long(argc, argv, "", long_options, &option_index);
      if (c == -1)
         break;

      switch (c) {
      case 256:
         fprintf(stderr,
                 "%-10.10s: output assembly definitions\n"
                 "--help    : this message\n"
                 "--output  : set output filename\n"
                 "\n", argv[0]);
         break;

      case 257:
         outputPathname = malloc(strlen(optarg) + 1);
         strcpy(outputPathname, optarg);
         break;

      default:
         printf ("?? getopt returned character code 0%o ??\n", c);
         exit(-1);
      }
   }


   if (outputPathname == NULL) {
      fprintf(stderr, "%s: output filename not set\n", argv[0]);
      exit(-1);
   }

   fp = fopen(outputPathname, "w");

   if (fp != NULL) {
      writeFile(fp, outputPathname);
   }
   else {
      fprintf(stderr, "%s: unable to open output file '%s'\n",
              argv[0], outputPathname);
   }
   
   free(outputPathname);
   fclose(fp);
   return 0;
}
