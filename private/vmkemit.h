/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/



/*
 * vmkemit.h --
 *
 * Code emission macros for base x86 arhictecture used by vmkernel.
 *
 * All macros increment the memptr variable, which must be defined either 
 * as a local or global variable, and point to some buffer.
 */


#ifndef _VMK_EMIT_H_
#define _VMK_EMIT_H_

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "x86.h"


/*
 * Emit location pointer
 */

typedef uint8 *EmitPtr;


/*
 *----------------------------------------------------------------------
 *
 * MNEM -- opcode mnemonics
 *
 *----------------------------------------------------------------------
 */
#define NO_SEGMENT_OVERRIDE  -1
#define MNEM_PREFIX_CS     0x2e
#define MNEM_PREFIX_SS     0x36
#define MNEM_PREFIX_DS     0x3e
#define MNEM_PREFIX_ES     0x26
#define MNEM_PREFIX_FS     0x64
#define MNEM_PREFIX_GS     0x65

#define MNEM_PREFIX_OPSIZE 0x66
#define MNEM_PREFIX_ASIZE  0x67
#define MNEM_PREFIX_LOCK   0xF0
#define MNEM_PREFIX_REPN   0xf2
#define MNEM_PREFIX_REP    0xf3

#define MNEM_TEST_IMM8     0xf6
#define MNEM_TEST_IMMV     0xf7

#define MNEM_OPCODE_ESC    0x0f  /* Two byte instruction escape ("prefix"). */
#define MNEM_ADC           0x13
#define MNEM_ADD           0x03
#define MNEM_CMP           0x3b
#define MNEM_CMP_EAX       0x3d
#define MNEM_SUB           0x2b
#define MNEM_NOT8          0xf6
#define MNEM_NOT           0xf7

#define MNEM_PUSH_EAX      0x50
#define MNEM_PUSH_ECX      0x51
#define MNEM_PUSH_EDX      0x52
#define MNEM_PUSH_EBX      0x53
#define MNEM_PUSH_ESP      0x54
#define MNEM_PUSH_EBP      0x55
#define MNEM_PUSH_ESI      0x56
#define MNEM_PUSH_EDI      0x57

#define MNEM_POP_EAX       0x58
#define MNEM_POP_ECX       0x59
#define MNEM_POP_EDX       0x5A
#define MNEM_POP_EBX       0x5B
#define MNEM_POP_ESP       0x5C
#define MNEM_POP_EBP       0x5D
#define MNEM_POP_ESI       0x5E
#define MNEM_POP_EDI       0x5F
#define MNEM_POP_MEM       0x8F

#define MNEM_NOP           0x90
#define MNEM_MOVE_REG_RM   0x89
#define MNEM_MOVE_RM_REG   0x8b

#define MNEM_PUSH          0x68
#define MNEM_PUSHF         0x9c
#define MNEM_POPF          0x9d
#define MNEM_PUSHA         0x60
#define MNEM_POPA          0x61

#define MNEM_JCC_JA        0x77
#define MNEM_JCC_JAE       0x73
#define MNEM_JCC_JB        0x72
#define MNEM_JCC_JBE       0x76
#define MNEM_JCC_JC        0x72
#define MNEM_JCC_JCXZ      0xe3
#define MNEM_JCC_JECXZ     0xe3
#define MNEM_JCC_JE        0x74
#define MNEM_JCC_JG        0x7f
#define MNEM_JCC_JGE       0x7d
#define MNEM_JCC_JL        0x7c
#define MNEM_JCC_JLE       0x7e
#define MNEM_JCC_JNA       0x76
#define MNEM_JCC_JNAE      0x72
#define MNEM_JCC_JNB       0x73
#define MNEM_JCC_JNBE      0x77
#define MNEM_JCC_JNC       0x73
#define MNEM_JCC_JNE       0x75
#define MNEM_JCC_JNG       0x7e
#define MNEM_JCC_JNGE      0x7c
#define MNEM_JCC_JNL       0x7d
#define MNEM_JCC_JNLE      0x7f
#define MNEM_JCC_JNO       0x71
#define MNEM_JCC_JNP       0x7b
#define MNEM_JCC_JNS       0x79
#define MNEM_JCC_JNZ       0x75
#define MNEM_JCC_JO        0x70
#define MNEM_JCC_JP        0x7a
#define MNEM_JCC_JPE       0x7a
#define MNEM_JCC_JPO       0x7b
#define MNEM_JCC_JS        0x78
#define MNEM_JCC_JZ        0x74

/* FPU opcodes */
#define MNEM_FNSAVE        0xdd    /* MNEM_FNSAVE:  0xdd/6 */
#define MNEM_FRSTOR        0xdd    /* MNEM_FRSTOR:  0xdd/4 */
#define MNEM_FXRSTOR       0xae    /* MNEM_FXRSTOR: 0xae/1 */
#define MNEM_FXSAVE        0xae    /* MNEM_FXSAVE:  0xae/0 */
#define MNEM_FWAIT         0x9b

#define MNEM_LONG_JCC_JB   0x820f
#define MNEM_LONG_JCC_JC   0x820f
#define MNEM_LONG_JCC_JNC  0x830f
#define MNEM_LONG_JCC_JZ   0x840f
#define MNEM_LONG_JCC_JNE  0x850f
#define MNEM_LONG_JCC_JE   0x840f
#define MNEM_LONG_JCC_JNZ  0x850f
#define MNEM_LONG_JCC_JBE  0x860f
#define MNEM_LONG_JCC_JA   0x870f
#define MNEM_LONG_JCC_JS   0x880f
#define MNEM_RDTSC_EDXEAX  0x310f
#define MNEM_RDPMC_EDXEAX  0x330f
#define MNEM_CMPXCHG8      0xB00f
#define MNEM_CMPXCHG       0xB10f
#define MNEM_CMPXCHG8B     0xc70f

#define MNEM_MOV_STORE_RM_8        0x88
#define MNEM_MOV_STORE_RM_32       0x89
#define MNEM_MOV_LOAD_RM_8         0x8a
#define MNEM_MOV_LOAD_RM_32        0x8b
#define MNEM_MOV_STORE_IMM_RM_8    0xc6
#define MNEM_MOV_STORE_IMM_RM_32   0xc7
#define MNEM_OR_REG8_RM8           0x08

#define MNEM_FNSAVE                0xdd
#define MNEM_FWAIT                 0x9b

#define MNEM_MOV_LOAD_AL_MOFF_8    0xa0
#define MNEM_MOV_LOAD_EAX_MOFF_32  0xa1
#define MNEM_MOV_STORE_MOFF_AL_8   0xa2
#define MNEM_MOV_STORE_MOFF_EAX_32 0xa3
/*
 * opcode with rep prefix
 */
#define MNEM_MOVSB         0xa4
#define MNEM_MOVS          0xa5
#define MNEM_CMPSB         0xa6
#define MNEM_CMPS          0xa7
#define MNEM_STOSB         0xaa
#define MNEM_STOS          0xab
#define MNEM_LODSB         0xac
#define MNEM_LODS          0xad
#define MNEM_SCASB         0xae
#define MNEM_SCAS          0xaf

#define MNEM_CALL_NEAR     0xe8
#define MNEM_JUMP_LONG     0xe9
#define MNEM_JUMP_FAR      0xea
#define MNEM_JUMP_SHORT    0xeb
#define MNEM_JUMP_INDIRECT 0xff
#define MNEM_CALL_INDIRECT 0xff

#define MNEM_LEA           0x8d
#define MNEM_LOOP          0xe2
#define MNEM_LOOPZ         0xe1
#define MNEM_LOOPNZ        0xe0

#define MNEM_RET           0xc3
#define MNEM_RET_IMM       0xc2
#define MNEM_CLI           0xfa
#define MNEM_STI           0xfb
#define MNEM_INT3          0xcc
#define MNEM_INTO          0xce
#define MNEM_INTN          0xcd
#define MNEM_IRET          0xcf
#define MNEM_RETFAR        0xcb
#define MNEM_RETFAR_IMM16  0xca
#define MNEM_FARCALL_AP    0x9a
#define MNEM_HLT           0xf4


/* In/Out mnemonics. */
#define MNEM_IN_AL_IMM     0xe4
#define MNEM_IN_EAX_IMM    0xe5
#define MNEM_IN_AL_DX      0xec
#define MNEM_IN_EAX_DX     0xed
#define MNEM_OUT_AL_IMM    0xe6
#define MNEM_OUT_EAX_IMM   0xe7
#define MNEM_OUT_AL_DX     0xee
#define MNEM_OUT_EAX_DX    0xef
#define MNEM_INSB          0x6c
#define MNEM_INSD          0x6d
#define MNEM_OUTSB         0x6e
#define MNEM_OUTSD         0x6f
/*
 * 2-byte mnemonics
 */

#define MNEM_REP_STOS          (MNEM_PREFIX_REP | (MNEM_STOS << 8))
#define MNEM_REP_MOVS          (MNEM_PREFIX_REP | (MNEM_MOVS << 8))
#define MNEM_MOVUPS_TO_MODRM   0x110f
#define MNEM_MOVUPS_FROM_MODRM 0x100f
#define MNEM_MOVNTPS           0x2b0f
#define MNEM_MOVSX8            0xbe0f
#define MNEM_MOVSX16           0xbf0f
#define MNEM_MOVZX8            0xb60f
#define MNEM_MOVZX16           0xb70f
#define MNEM_SETO              0x900f
#define MNEM_SYSENTER          0x340f
#define MNEM_SYSEXIT           0x350f
#define MNEM_UD2               0x0b0f
#define MNEM_LSL               0x030f

/*
 *----------------------------------------------------------------------
 *
 * EMIT -- emission macros (memptr implicit)
 *
 * We try to adopt the following naming convention: For the macros
 * that are named EMITxx_yyy, the value "xx" represents the current
 * codesize.  All operands are assumed to have the same size as the
 * codesize, unless otherwise specified.  The macros named EMIT_yyy
 * should work for any codesize.
 * 
 *---------------------------------------------------------------------- 
 */

#define EMIT_OPERAND_IF_16(_codeSize) {                                       \
  if ((_codeSize) == SIZE_16BIT) EMIT_OPERAND_OVERRIDE;                       \
}
#define EMIT_ADDRESS_IF_16(_codeSize) {                                       \
  if ((_codeSize) == SIZE_16BIT) EMIT_ADDRESS_OVERRIDE;                       \
}
#define EMIT_OPERAND_IF_32(_codeSize) {                                       \
  if ((_codeSize) == SIZE_32BIT) EMIT_OPERAND_OVERRIDE;                       \
}
#define EMIT_ADDRESS_IF_32(_codeSize) {                                       \
  if ((_codeSize) == SIZE_32BIT) EMIT_ADDRESS_OVERRIDE;                       \
}

/* Combined emission of <A> and <OP>. For more compact (monitor) code. */
#define EMIT_A_OP_OVERRIDE                                                    \
   EMIT_WORD16(MNEM_PREFIX_OPSIZE << 8 | MNEM_PREFIX_ASIZE)

#define EMIT_A_OP_OVERRIDE_IF(_e) do { if (_e) EMIT_A_OP_OVERRIDE; } while (0)
#define EMIT_A_OP_OVERRIDE_IF_16(_s) EMIT_A_OP_OVERRIDE_IF((_s) == SIZE_16BIT)
#define EMIT_A_OP_OVERRIDE_IF_32(_s) EMIT_A_OP_OVERRIDE_IF((_s) == SIZE_32BIT)


static INLINE uint8
Emit_BuildModrmByte(int mod, int nnn, int rm) 
{
   ASSERT(0 <= mod && mod < 4 && 
          0 <= nnn && nnn < 8 &&
          0 <= rm  && rm  < 8);
   return (mod << 6) | (nnn << 3) | rm;
}

static INLINE uint8
Emit_BuildSibByte(int s, int i, int b) 
{
   ASSERT(0 <= s && s < 4 &&
          0 <= i && i < 8 &&
          0 <= b && b < 8);
   return (s << 6) | (i << 3) | b;
}


/*
 * Because memptr can be referenced by the argument of those macros, we must
 * evaluate the argument in a statement that is distinct from the increment
 * statement, in order for those macros to have defined semantics wrt C
 * sequence point rules --hpreg
 *
 * Note: I tried to use the do {...} while (0) construction for those, but
 *       they have a cost with gcc 2.7.2.3
 */

#define EMIT_BYTE(_a)   {                                                     \
   /* An explicit cast to a uint8 has a cost with gcc 2.7.2.3 !!! --hpreg */  \
   uint8  _z =         (_a);                                                  \
                                                                              \
   *((uint8  *)memptr)++ = _z;                                                \
}

#define EMIT_WORD16(_a) {                                                     \
   uint16 _z = (uint16)(_a);                                                  \
                                                                              \
   *((uint16 *)memptr)++ = _z;                                                \
}

#define EMIT_WORD32(_a) {                                                     \
   uint32 _z = (uint32)(_a);                                                  \
                                                                              \
   *((uint32 *)memptr)++ = _z;                                                \
}

#define EMIT_WORD(_x)   EMIT_WORD32(_x)


#define EMIT_MODRM(_mod,_nnn,_rm) EMIT_BYTE(Emit_BuildModrmByte(_mod,_nnn,_rm))

#define MODRM_RM_SIB    4
#define MODRM_RM_DISP32 5
#define MODRM_RM_DISP16 6

#define EMIT_MODRM_MEM(_reg,_rm,_disp) {                                      \
   int _disp1 = (_disp);                                                      \
   if (_disp1 == 0) {                                                         \
      EMIT_MODRM(0,_reg,_rm);                                                 \
   } else if (_disp1 >= -128 && _disp1 < 128) {                               \
      EMIT_MODRM(1,_reg,_rm); EMIT_BYTE(_disp1);                              \
   } else {                                                                   \
      EMIT_MODRM(2,_reg,_rm); EMIT_WORD(_disp1);                              \
   }                                                                          \
}

#define EMIT_MODRM_REG(_reg,_rm)  EMIT_MODRM(3,_reg,_rm)

#define EMIT32_MODRM_FIXEDMEM(_reg,_addr) {                                   \
   EMIT_MODRM(0,_reg,MODRM_RM_DISP32);                                        \
   EMIT_WORD(_addr);                                                          \
}

#define EMIT16_MODRM_FIXEDMEM(_reg,_addr) {                                   \
   EMIT_MODRM(0,_reg,MODRM_RM_DISP16);                                        \
   EMIT_WORD16((VA)(_addr));                                                  \
}

#define EMIT32_MODRM_INDMEM(_reg, _regAddr) {                                 \
   EMIT_MODRM(0,_reg,_regAddr);                                               \
}

#define EMIT_SIB(_s,_i,_b) EMIT_BYTE(Emit_BuildSibByte(_s,_i,_b))

#define SIB_SCALE_1 0x0
#define SIB_SCALE_2 0x1
#define SIB_SCALE_4 0x2
#define SIB_SCALE_8 0x3

/* NB: to set full 32 bits of "_reg" in 16 bit code must prefix with <op>. */
#define EMIT16_MOVZX_REG_ABS16(_reg, _addr) {                                 \
   EMIT_WORD16(MNEM_MOVZX16);                                                 \
   EMIT16_MODRM_FIXEDMEM(_reg, _addr);                                        \
}

#define EMIT32_MOVZX_REG_ABS16(_reg, _addr) {                                 \
   EMIT_WORD16(MNEM_MOVZX16);                                                 \
   EMIT32_MODRM_FIXEDMEM(_reg, _addr);                                        \
}

#define EMIT_MOVZX_REG_ABS16(_codeSize, _reg, _addr) {                        \
   if ((_codeSize) == SIZE_16BIT) {                                           \
      EMIT16_MOVZX_REG_ABS16(_reg, _addr);                                    \
   } else {                                                                   \
      EMIT32_MOVZX_REG_ABS16(_reg, _addr);                                    \
   }                                                                          \
}

#define EMIT16_MOVZX_REG_ABS8(_reg, _addr) {                                  \
   EMIT_WORD16(MNEM_MOVZX8);                                                  \
   EMIT16_MODRM_FIXEDMEM(_reg, _addr);                                        \
}

#define EMIT32_MOVZX_REG_ABS8(_reg, _addr) {                                  \
   EMIT_WORD16(MNEM_MOVZX8);                                                  \
   EMIT32_MODRM_FIXEDMEM(_reg, _addr);                                        \
}

#define EMIT_MOVZX_REG_ABS8(_codeSize, _reg, _addr) {                         \
   if ((_codeSize) == SIZE_16BIT) {                                           \
      EMIT16_MOVZX_REG_ABS8(_reg, _addr);                                     \
   } else {                                                                   \
      EMIT32_MOVZX_REG_ABS8(_reg, _addr);                                     \
   }                                                                          \
}

#define EMIT_MOVZX_REG8_TO_REG(srcReg8, dstReg) {                             \
   EMIT_WORD16(MNEM_MOVZX8);                                                  \
   EMIT_MODRM(3, dstReg, srcReg8);                                            \
}

#define EMIT_MOVZX_REG16_TO_REG(srcReg16, dstReg) {                           \
   EMIT_WORD16(MNEM_MOVZX16);                                                 \
   EMIT_MODRM(3, dstReg, srcReg16);                                           \
}

#define EMIT32_ZEROEXTEND_REGISTER(_reg) {                                    \
   EMIT_MOVZX_REG16_TO_REG(_reg, _reg);                                       \
}

#define EMIT_PUSH_IMM(_imm32) {                                               \
   EMIT_BYTE(MNEM_PUSH);                                                      \
   EMIT_WORD(_imm32);                                                         \
}

#define EMIT32_PUSH_IMM8(_imm8) {                                             \
   EMIT_BYTE(0x6a);                                                           \
   EMIT_BYTE(_imm8);                                                          \
}

#define EMIT16_PUSH_IMM(_imm16) {                                             \
   EMIT_BYTE(MNEM_PUSH);                                                      \
   EMIT_WORD16(_imm16);                                                       \
}

#define EMIT32_PUSH_FIXEDMEM(_addr) {                                         \
   EMIT_BYTE(0xff);                                                           \
   EMIT32_MODRM_FIXEDMEM(6,(VA)(_addr));                                      \
}

#define EMIT16_PUSH_FIXEDMEM(_addr) {                                         \
   EMIT_BYTE(0xff);                                                           \
   EMIT16_MODRM_FIXEDMEM(6,(VA)(_addr));                                      \
}

#define EMIT_PUSH_FIXEDMEM(_codeSize, _addr) {                                \
   VA _addr1 = (VA)(_addr);                                                   \
   if ((_codeSize) == SIZE_16BIT) {                                           \
      ASSERT(((uint32)_addr1 & 0xffff) == (uint32)_addr1);                    \
      EMIT16_PUSH_FIXEDMEM(_addr1);                                           \
   } else {                                                                   \
      EMIT32_PUSH_FIXEDMEM(_addr1);                                           \
   }                                                                          \
}

#define EMIT32_POP_FIXEDMEM(_addr) {                                          \
   EMIT_BYTE(MNEM_POP_MEM);                                                   \
   EMIT32_MODRM_FIXEDMEM(0, (VA)(_addr));                                     \
}

/* pop disp(baseReg) */
#define EMIT32_POP_MEM(_disp, _baseReg) {                                     \
   EMIT_BYTE(MNEM_POP_MEM);                                                   \
   EMIT_MODRM_MEM(0, _baseReg, _disp);                                        \
}

#define EMIT32_CALL_IMM(_f) {                                                 \
   int _disp = (Reg32)(_f) - (Reg32)memptr - 5;                               \
   EMIT_BYTE(MNEM_CALL_NEAR);                                                 \
   EMIT_WORD32(_disp);                                                        \
}

#define EMIT32_NEARCALL_INDMEM(_reg) {                                        \
   EMIT_BYTE(MNEM_CALL_INDIRECT);                                             \
   EMIT32_MODRM_INDMEM(2,_reg);                                               \
}
   
#define EMIT_CALL_REGIND(_reg)  {                                             \
   EMIT_BYTE(0xff);                                                           \
   EMIT_MODRM_REG(2,_reg);                                                    \
}

#define EMIT32_FARCALL_IMM(_seg, _disp) {                                     \
   EMIT_BYTE(MNEM_FARCALL_AP);                                                \
   EMIT_WORD32(_disp);                                                        \
   EMIT_WORD16(_seg);                                                         \
}

#define EMIT32_JUMP_IMM(_f) {                                                 \
   int32 _disp = (Reg32)(_f) - (Reg32)memptr - 5;                             \
   EMIT_BYTE(MNEM_JUMP_LONG);                                                 \
   EMIT_WORD32(_disp);                                                        \
}

/* _jcc is one of MNEM_LONG_JCC_JNZ etc. */
#define EMIT32_JCC_IMM(_jcc, _f) {                                            \
   int32 _disp = (Reg32)(_f) - (Reg32)memptr - 6;                             \
   EMIT_WORD16(_jcc);                                                         \
   EMIT_WORD32(_disp);                                                        \
}

#define EMIT_JUMP_IMM(_opSize, _f) {                                          \
   EMIT_OPERAND_IF_16(_opSize);                                               \
   EMIT32_JUMP_IMM(_f);                                                       \
}

#define EMIT32_JUMP_MEMIND(_addr) {                                           \
   EMIT_BYTE(MNEM_JUMP_INDIRECT);                                             \
   EMIT32_MODRM_FIXEDMEM(4,_addr);                                            \
}

#define EMIT16_JUMP_MEMIND(_addr) {                                           \
   uint32 _memInd = (uint32) (_addr);                                         \
   ASSERT(_memInd < 0x10000);                                                 \
   EMIT_OPERAND_OVERRIDE;                                                     \
   EMIT_BYTE(MNEM_JUMP_INDIRECT);                                             \
   EMIT16_MODRM_FIXEDMEM(4,_memInd);                                          \
}

#define EMIT_JUMP_MEMIND(_opSize, _addr) {                                    \
   if ((_opSize) == SIZE_32BIT) {                                             \
      EMIT32_JUMP_MEMIND(_addr);                                              \
   } else {                                                                   \
      EMIT16_JUMP_MEMIND(_addr);                                              \
   }                                                                          \
}

#define EMIT32_JUMP_REGIND(_reg) {                                            \
   EMIT_BYTE(MNEM_JUMP_INDIRECT);                                             \
   EMIT_MODRM_REG(4,_reg);                                                    \
}

#define EMIT32_FARJUMP_IMM(_seg, _disp) {                                     \
   EMIT_BYTE(MNEM_JUMP_FAR);                                                  \
   EMIT_WORD32(_disp);                                                        \
   EMIT_WORD16(_seg);                                                         \
}

#define EMIT16_FARJUMP_IMM(_seg, _disp) {                                     \
   EMIT_BYTE(MNEM_JUMP_FAR);                                                  \
   EMIT_WORD16(_disp);                                                        \
   EMIT_WORD16(_seg);                                                         \
}

#define EMIT_FARJUMP_IMM(_opSize, _seg, _disp) {                              \
   if ((_opSize) == SIZE_32BIT) {                                             \
      EMIT32_FARJUMP_IMM(_seg, _disp);                                        \
   } else {                                                                   \
      EMIT16_FARJUMP_IMM(_seg, _disp);                                        \
   }                                                                          \
}

#define EMIT32_NEARJUMP_INDMEM(_reg) {                                        \
   EMIT_BYTE(MNEM_JUMP_INDIRECT);                                             \
   EMIT32_MODRM_INDMEM(4,_reg);                                               \
}
   
#define EMIT32_FARJUMP_INDMEM(_reg) {                                         \
   EMIT_BYTE(MNEM_JUMP_INDIRECT);                                             \
   EMIT32_MODRM_INDMEM(5,_reg);                                               \
}
   
#define EMIT_PUSH_FROM_SP(_off) {                                             \
   ASSERT((_off) >= 0 && (_off) < 128);                                       \
   EMIT_BYTE(0xff);                                                           \
   EMIT_MODRM(1,6,4); /* SIB escape */                                        \
   EMIT_SIB(0,4/*none*/,REG_ESP);                                             \
   EMIT_BYTE(_off);                                                           \
}


#define EMIT_PUSH_ARCHREG(_reg) {                                             \
   EMIT_BYTE(0xff);                                                           \
   EMIT_MODRM(3, 6, _reg);                                                    \
}

#define EMIT_JCC_SHORT(_cSz, _jcc, _disp) {                                   \
   int _disp1 = (_disp);                                                      \
   EMIT_OPERAND_IF_16(_cSz);                                                  \
   ASSERT(_disp1 >= -128 && _disp1 <= 127);                                   \
   EMIT_BYTE(_jcc);                                                           \
   EMIT_BYTE(_disp1);                                                         \
}

#define EMIT_JCC_LONG(_cSz, _jcc, _disp) {                                    \
   EMIT_OPERAND_IF_16(_cSz);                                                  \
   EMIT_BYTE(0x0f);                                                           \
   EMIT_BYTE((_jcc) + 16);                                                    \
   EMIT_WORD(_disp);                                                          \
}

#define EMIT_JUMP_SHORT(_cSz, _disp) EMIT_JCC_SHORT(_cSz,MNEM_JUMP_SHORT, _disp)
#define EMIT_JNE_SHORT(_cSz,  _disp) EMIT_JCC_SHORT(_cSz, MNEM_JCC_JNE, _disp)
#define EMIT_JNZ_SHORT(_cSz,  _disp) EMIT_JCC_SHORT(_cSz, MNEM_JCC_JNZ, _disp)
#define EMIT_JE_SHORT(_cSz,   _disp) EMIT_JCC_SHORT(_cSz, MNEM_JCC_JE,  _disp)
#define EMIT_JZ_SHORT(_cSz,   _disp) EMIT_JCC_SHORT(_cSz, MNEM_JCC_JE,  _disp)
#define EMIT_JNC_SHORT(_cSz,  _disp) EMIT_JCC_SHORT(_cSz, MNEM_JCC_JNC, _disp)
#define EMIT_JC_SHORT(_cSz,   _disp) EMIT_JCC_SHORT(_cSz, MNEM_JCC_JC,  _disp)

#define EMIT_LEA_REG_REG_DISP8(_dst, _disp8, _src) {                          \
   EMIT_BYTE(MNEM_LEA);                                                       \
   EMIT_MODRM(1, _dst, _src);                                                 \
   EMIT_BYTE(_disp8);                                                         \
}

#define EMIT32_LEA_REG_REG_DISP32(_dst, _disp32, _src) {                      \
   EMIT_BYTE(MNEM_LEA);                                                       \
   EMIT_MODRM(2, _dst, _src);                                                 \
   EMIT_WORD(_disp32);                                                        \
}

#define EMIT_LOOP(_cSz, _disp) {                                              \
   int _disp1 = (_disp);                                                      \
   EMIT_OPERAND_IF_16(_cSz);                                                  \
   ASSERT(_disp1 >= -128 && _disp1 <= 127);                                   \
   EMIT_BYTE(0xe2);                                                           \
   EMIT_BYTE(_disp1);                                                         \
}

#define EMIT32_STORE_AL_ABS(_addr)  { EMIT_BYTE(0xa2); EMIT_WORD(_addr); }
#define EMIT32_LOAD_AL_ABS(_addr)   { EMIT_BYTE(0xa0); EMIT_WORD(_addr); }


#define EMIT16_LOAD_REG_ABS(_reg, _addr) {                                    \
   uint32 _add = (uint32) (_addr);                                            \
   ASSERT(_add < 0x10000);                                                    \
   if ((_reg) == REG_EAX) {                                                   \
      EMIT_BYTE(0xa1); EMIT_WORD16((VA)(_add));                               \
   } else {                                                                   \
      EMIT_BYTE(0x8b); EMIT16_MODRM_FIXEDMEM(_reg, _add);                     \
   }                                                                          \
}

#define EMIT32_LOAD_REG_ABS(_reg, _addr) {                                    \
   if ((_reg) == REG_EAX) {                                                   \
      EMIT_BYTE(0xa1); EMIT_WORD(_addr);                                      \
   } else {                                                                   \
      EMIT_BYTE(0x8b); EMIT32_MODRM_FIXEDMEM(_reg, _addr);                    \
   }                                                                          \
}

#define EMIT_LOAD_REG_ABS(_aSize, _reg, _addr) {                              \
   if ((_aSize) == SIZE_16BIT) {                                              \
      EMIT16_LOAD_REG_ABS(_reg, _addr);                                       \
   } else {                                                                   \
      EMIT32_LOAD_REG_ABS(_reg, _addr);                                       \
   }                                                                          \
}

#define EMIT16_STORE_REG_ABS(_reg, _addr) {                                   \
   if ((_reg) == REG_EAX) {                                                   \
      EMIT_BYTE(0xa3); EMIT_WORD16((VA)(_addr));                              \
   } else {                                                                   \
      EMIT_BYTE(0x89); EMIT16_MODRM_FIXEDMEM(_reg, _addr);                    \
   }                                                                          \
}

#define EMIT32_STORE_REG_ABS(_reg, _addr) {                                   \
   if ((_reg) == REG_EAX) {                                                   \
      EMIT_BYTE(0xa3); EMIT_WORD(_addr);                                      \
   } else {                                                                   \
      EMIT_BYTE(0x89); EMIT32_MODRM_FIXEDMEM(_reg, _addr);                    \
   }                                                                          \
}

#define EMIT_STORE_REG_ABS(_aSize, _reg, _addr) {                             \
   if ((_aSize) == SIZE_16BIT) {                                              \
      EMIT16_STORE_REG_ABS(_reg, _addr);                                      \
   } else {                                                                   \
      EMIT32_STORE_REG_ABS(_reg, _addr);                                      \
   }                                                                          \
}

#define EMIT_LOAD_REG8_ABS(_codeSize, _reg, _addr) {                          \
   if ((_codeSize) == SIZE_16BIT) {                                           \
      EMIT16_LOAD_REG8_ABS(_reg, _addr);                                      \
   } else {                                                                   \
      EMIT32_LOAD_REG8_ABS(_reg, _addr);                                      \
   }                                                                          \
}

#define EMIT16_LOAD_REG8_ABS(_reg, _addr) {                                   \
   if ((_reg) == REG8_AL) {                                                   \
      EMIT_BYTE(0xa0);                                                        \
      EMIT_WORD16((VA)(_addr));                                               \
   } else {                                                                   \
      EMIT_BYTE(MNEM_MOV_LOAD_RM_8);                                          \
      EMIT16_MODRM_FIXEDMEM(_reg, _addr);                                     \
   }                                                                          \
}   

#define EMIT32_LOAD_REG8_ABS( _reg, _abs) {                                   \
   if ((_reg) == REG8_AL) {                                                   \
      EMIT32_LOAD_AL_ABS(_abs);                                               \
   } else {                                                                   \
      EMIT32int_MODRM_FIXEDMEM_REG(MNEM_MOV_LOAD_RM_8,_abs,_reg);             \
   }                                                                          \
}

#define EMIT32_LOAD_REG8_MEM(_reg, _disp, _base) {                            \
   EMIT_BYTE(MNEM_MOV_LOAD_RM_8);                                             \
   EMIT_MODRM_MEM(_reg, _base, _disp);                                        \
}   

#define EMIT_STORE_REG8_ABS(_codeSize, _reg, _addr) {                         \
   if ((_codeSize) == SIZE_16BIT) {                                           \
      EMIT16_STORE_REG8_ABS(_reg, _addr);                                     \
   } else {                                                                   \
      EMIT32_STORE_REG8_ABS(_reg, _addr);                                     \
   }                                                                          \
}

#define EMIT16_STORE_REG8_ABS(_reg, _addr) {                                  \
   if ((_reg) == REG8_AL) {                                                   \
      EMIT_BYTE(0xa2);                                                        \
      EMIT_WORD16((VA)(_addr));                                               \
   } else {                                                                   \
      EMIT_BYTE(MNEM_MOV_STORE_RM_8);                                         \
      EMIT16_MODRM_FIXEDMEM(_reg, _addr);                                     \
   }                                                                          \
}   

#define EMIT32_STORE_REG8_ABS(_reg, _abs) {                                   \
   if ((_reg) == REG8_AL) {                                                   \
      EMIT32_STORE_AL_ABS(_abs);                                              \
   } else {                                                                   \
      EMIT32int_MODRM_FIXEDMEM_REG(MNEM_MOV_STORE_RM_8,_abs,_reg);            \
   }                                                                          \
}

#define EMIT16_STORE_FROM_AH(_addr) {                                         \
   EMIT_BYTE(0x88); EMIT16_MODRM_FIXEDMEM(4, _addr);                          \
}

#define EMIT32_STORE_FROM_AH(_addr) {                                         \
   EMIT_BYTE(0x88); EMIT32_MODRM_FIXEDMEM(4, _addr);                          \
}

#define EMIT_STORE_FROM_AH(_aSize, _addr) {                                   \
   if ((_aSize) == SIZE_16BIT) {                                              \
      EMIT16_STORE_FROM_AH(_addr)                                             \
   } else {                                                                   \
      EMIT32_STORE_FROM_AH(_addr);                                            \
   }                                                                          \
}

#define EMIT32_STORE_REG_TO_REG(_reg, _off, _base) {                          \
   ASSERT((_off) >= 0 && (_off) < 128);                                       \
   EMIT_BYTE(0x89);                                                           \
   EMIT_MODRM(1, _reg, _base);                                                \
   EMIT_BYTE(_off);                                                           \
}

#define EMIT32_ADD_REG_ABS(_reg, _addr) {                                     \
   EMIT_BYTE(MNEM_ADD); EMIT32_MODRM_FIXEDMEM(_reg, _addr);                   \
}

#define EMIT16_ADD_REG_ABS(_reg, _addr) {                                     \
   EMIT_BYTE(MNEM_ADD); EMIT16_MODRM_FIXEDMEM(_reg, _addr);                   \
}

#define EMIT32_SUB_REG_ABS(_reg, _addr) {                                     \
   EMIT_BYTE(MNEM_SUB); EMIT32_MODRM_FIXEDMEM(_reg, _addr);                   \
}

#define EMIT16_SUB_REG_ABS(_reg, _addr) {                                     \
   EMIT_BYTE(MNEM_SUB); EMIT16_MODRM_FIXEDMEM(_reg, _addr);                   \
}

#define EMIT32_XADD_REG_ABS(_reg, _addr) {                                    \
   EMIT_BYTE(0x0f); EMIT_BYTE(0xC1); EMIT32_MODRM_FIXEDMEM(_reg, _addr);      \
}

#define EMIT32_XCHG_REG_ABS(_reg, _addr) {                                    \
   EMIT_BYTE(0x87); EMIT32_MODRM_FIXEDMEM(_reg, _addr);                       \
}

#define EMIT_XCHG_REG_REG(_reg1, _reg2) {                                     \
   if ((_reg1) != (_reg2)) {                                                  \
      EMIT_BYTE(0x87); EMIT_MODRM_REG(_reg1, _reg2);                          \
   }                                                                          \
}

#define EMIT32_SUB_REG8_ABS8(_reg, _addr) {                                   \
   EMIT_BYTE(0x2a); EMIT32_MODRM_FIXEDMEM(_reg, _addr);                       \
}

#define EMIT16_SUB_REG8_ABS8(_reg, _addr) {                                   \
   EMIT_BYTE(0x2a); EMIT16_MODRM_FIXEDMEM(_reg, _addr);                       \
}

#define EMIT_SUB_REG8_ABS8(_codeSize, _reg, _addr) {                          \
   if ((_codeSize) == SIZE_16BIT) {                                           \
      EMIT16_SUB_REG8_ABS8(_reg, _addr);                                      \
   } else {                                                                   \
      EMIT32_SUB_REG8_ABS8(_reg, _addr);                                      \
   }                                                                          \
}

#define EMIT32_STORE_IMM16_ABS(_imm,_addr) {                                  \
   EMIT_OPERAND_OVERRIDE;                                                     \
   EMIT_BYTE(0xc7);                                                           \
   EMIT32_MODRM_FIXEDMEM(0,_addr);                                            \
   EMIT_WORD16(_imm);                                                         \
}

#define EMIT32_STORE_IMM_ABS(_imm,_addr) {                                    \
   EMIT_BYTE(0xc7);                                                           \
   EMIT32_MODRM_FIXEDMEM(0,_addr);                                            \
   EMIT_WORD(_imm);                                                           \
}

#define EMIT16_STORE_IMM_ABS(_imm,_addr) {                                    \
   ASSERT(((VA)(_addr) & 0xffff) == (VA)(_addr));                             \
   EMIT_BYTE(0xc7);                                                           \
   EMIT_MODRM(0,0,6);                                                         \
   EMIT_WORD16((VA)(_addr));                                                  \
   EMIT_WORD16(_imm);                                                         \
}

#define EMIT16_STORE_IMM32_ABS(_imm,_addr) {                                  \
   ASSERT(((VA)(_addr) & 0xffff) == (VA)(_addr));                             \
   EMIT_OPERAND_OVERRIDE;                                                     \
   EMIT_BYTE(0xc7);                                                           \
   EMIT_MODRM(0,0,6);                                                         \
   EMIT_WORD16((VA)(_addr));                                                  \
   EMIT_WORD32(_imm);                                                         \
}

#define EMIT32_STORE_IMM8_ABS(_imm,_addr) {                                   \
   EMIT_BYTE(0xc6);                                                           \
   EMIT32_MODRM_FIXEDMEM(0,_addr);                                            \
   EMIT_BYTE(_imm);                                                           \
}

#define EMIT16_STORE_IMM8_ABS(_imm,_addr) {                                   \
   ASSERT(((VA)(_addr) & 0xffff) == (VA)(_addr));                             \
   EMIT_BYTE(0xc6);                                                           \
   EMIT_MODRM(0,0,6);                                                         \
   EMIT_WORD16((VA)(_addr));                                                  \
   EMIT_BYTE(_imm);                                                           \
}

#define EMIT_STORE_IMM8_ABS(_codeSize, _imm, _addr) {                         \
  if (_codeSize == SIZE_16BIT) {                                              \
     EMIT16_STORE_IMM8_ABS(_imm, _addr);                                      \
  } else {                                                                    \
     EMIT32_STORE_IMM8_ABS(_imm, _addr);                                      \
  }                                                                           \
}         

#define EMIT_STORE_IMM_ABS(_codeSize, _imm, _addr) {                          \
  if (_codeSize == SIZE_16BIT) {                                              \
     EMIT16_STORE_IMM_ABS(_imm, _addr);                                       \
  } else {                                                                    \
     EMIT32_STORE_IMM_ABS(_imm, _addr);                                       \
  }                                                                           \
}

#define EMIT_STORE_IMM32_ABS(_codeSize, _imm, _addr) {                        \
  if (_codeSize == SIZE_16BIT) {                                              \
     EMIT16_STORE_IMM32_ABS(_imm, _addr);                                     \
  } else {                                                                    \
     EMIT32_STORE_IMM_ABS(_imm, _addr);                                       \
  }                                                                           \
}

#define EMIT_RDTSC_EDXEAX()   EMIT_WORD16(MNEM_RDTSC_EDXEAX)

#define EMIT_SUB_MEM_OFF_EBP(_reg, _off) {                                    \
   ASSERT((_off) >= 0 && (_off) < 128);                                       \
   EMIT_BYTE(MNEM_SUB);                                                       \
   EMIT_MODRM(1, _reg, REG_EBP);                                              \
   EMIT_BYTE(_off);                                                           \
}

#define EMIT_SBB_MEM_OFF_EBP(_reg, _off) {                                    \
   ASSERT((_off) >= 0 && (_off) < 128);                                       \
   EMIT_BYTE(0x1b);                                                           \
   EMIT_MODRM(1, _reg, REG_EBP);                                              \
   EMIT_BYTE(_off);                                                           \
}

#define EMIT_ADD_MEM_OFF_EBP(_reg,_off) {                                     \
   ASSERT((_off) >= 0 && (_off) < 128);                                       \
   EMIT_BYTE(MNEM_ADD);                                                       \
   EMIT_MODRM(1, _reg, REG_EBP);                                              \
   EMIT_BYTE(_off);                                                           \
}

#define EMIT_ADD_TO_MEM_OFF_EBP(_reg,_off) {                                  \
   ASSERT((_off) >= 0 && (_off) < 128);                                       \
   EMIT_BYTE(0x01);                                                           \
   EMIT_MODRM(1, _reg, REG_EBP);                                              \
   EMIT_BYTE(_off);                                                           \
}

#define EMIT_ADD_IMM8_TO_REG(_reg,_imm) {                                     \
   ASSERT((int)(_imm) >=- 128 && (int)(_imm) <= 127);                         \
   EMIT_BYTE(0x83);                                                           \
   EMIT_MODRM(0x3,0,(_reg));                                                  \
   EMIT_BYTE((int8)(_imm));                                                   \
}

#define EMIT_SUB_IMM8_FROM_REG(_reg, _imm) {                                  \
   ASSERT((int)(_imm) >= -128 && (int)(_imm) <= 127);                         \
   EMIT_BYTE(0x83);                                                           \
   EMIT_MODRM(0x3, 5, (_reg));                                                \
   EMIT_BYTE((int8)(_imm));                                                   \
}

#define EMIT16_SUB_REG_IMM(_reg, _imm) {                                      \
   if ((_reg) == REG_EAX) {                                                   \
      EMIT_BYTE(0x2d);                                                        \
   } else {                                                                   \
      EMIT_BYTE(0x81);                                                        \
      EMIT_MODRM(0x3, 5, (_reg));                                             \
   }                                                                          \
   EMIT_WORD16(_imm);                                                         \
}

#define EMIT32_SUB_REG_IMM(_reg, _imm) {                                      \
   if ((_reg) == REG_EAX) {                                                   \
      EMIT_BYTE(0x2d);                                                        \
   } else {                                                                   \
      EMIT_BYTE(0x81);                                                        \
      EMIT_MODRM(0x3, 5, (_reg));                                             \
   }                                                                          \
   EMIT_WORD32(_imm);                                                         \
}

#define EMIT_SUB_REG_IMM(_codeSize, _reg, _imm) {                             \
   if ((_codeSize) == SIZE_16BIT) {                                           \
      EMIT16_SUB_REG_IMM(_reg, _imm);                                         \
   } else {                                                                   \
      EMIT32_SUB_REG_IMM(_reg, _imm);                                         \
   }                                                                          \
}

#define EMIT_SUB_REG_IMM8(_reg, _imm) {                                       \
   EMIT_BYTE(0x83);                                                           \
   EMIT_MODRM(0x3, 5, (_reg));                                                \
   EMIT_BYTE(_imm);                                                           \
}

#define EMIT_ADC_MEM_OFF_EBP(_reg,_off) {                                  \
   ASSERT((_off) >= 0 && (_off) < 128);                                       \
   EMIT_BYTE(MNEM_ADC);                                                       \
   EMIT_MODRM(1,_reg,REG_EBP);                                                \
   EMIT_BYTE(_off);                                                           \
}

#define EMIT_ADC_TO_MEM_OFF_EBP(_reg,_off) {                                  \
   ASSERT((_off) >= 0 && (_off) < 128);                                       \
   EMIT_BYTE(0x11);                                                           \
   EMIT_MODRM(1,_reg,REG_EBP);                                                \
   EMIT_BYTE(_off);                                                           \
}

#define EMIT_STORE_IMM_TO_MEM_OFF_EBP(_imm,_off) {                            \
   ASSERT((_off) >= 0 && (_off) < 128);                                       \
   EMIT_BYTE(0xc6);                                                           \
   EMIT_MODRM_MEM(0,REG_EBP,_off);                                            \
   EMIT_BYTE(_imm);                                                           \
}

#define EMIT32_LOAD_REG_IMM(_reg,_imm) {                                      \
   EMIT_BYTE(0xb8 + (_reg));                                                  \
   EMIT_WORD(_imm);                                                           \
}

#define EMIT16_LOAD_REG_IMM(_reg,_imm) {                                      \
   EMIT_BYTE(0xb8 + (_reg));                                                  \
   EMIT_WORD16(_imm);                                                         \
}

#define EMIT_LOAD_REG_IMM(_oSize, _reg, _imm) {                               \
   if ((_oSize) == SIZE_16BIT) {                                              \
      EMIT16_LOAD_REG_IMM(_reg, _imm);                                        \
   } else {                                                                   \
      EMIT32_LOAD_REG_IMM(_reg, _imm);                                        \
   }                                                                          \
}

#define EMIT_MOVE_REG_REG(_src, _dst) {                                       \
   EMIT_BYTE(0x89);                                                           \
   EMIT_MODRM_REG(_src, _dst);                                                \
}

#define EMIT_MOVE_REG8_REG8(_src, _dst) {                                     \
   EMIT_BYTE(MNEM_MOV_STORE_RM_8);                                            \
   EMIT_MODRM_REG(_src, _dst);                                                \
}

#define EMIT_MOVE_MEM_REG(_reg,_disp,_base) {                                 \
   EMIT_BYTE(0x8b);                                                           \
   EMIT_MODRM_MEM(_reg,_base,_disp);                                          \
}

#define EMIT_MOVE_REG8_MEM(_reg, _disp, _base) {                              \
   EMIT_BYTE(0x88);                                                           \
   EMIT_MODRM_MEM(_reg,_base,_disp);                                          \
}

#define EMIT_MOVE_REG_MEM(_reg,_disp,_base) {                                 \
   EMIT_BYTE(0x89);                                                           \
   EMIT_MODRM_MEM(_reg,_base,_disp);                                          \
}

#define EMIT_MOVE_IMM_MEM(_imm,_base,_disp) {                                 \
   EMIT_BYTE(0xc7);                                                           \
   EMIT_MODRM_MEM(0,_base,_disp);                                             \
   EMIT_WORD(_imm);                                                           \
}


#define EMIT_NOP      EMIT_BYTE(MNEM_NOP)
#define EMIT_PUSHA    EMIT_BYTE(MNEM_PUSHA)
#define EMIT_POPA     EMIT_BYTE(MNEM_POPA)

#define EMIT_PUSH_REG(_reg) EMIT_BYTE(MNEM_PUSH_EAX + (_reg))
#define EMIT_PUSH_EAX       EMIT_BYTE(MNEM_PUSH_EAX)
#define EMIT_PUSH_ECX       EMIT_BYTE(MNEM_PUSH_ECX)
#define EMIT_PUSH_EDX       EMIT_BYTE(MNEM_PUSH_EDX)
#define EMIT_PUSH_EBX       EMIT_BYTE(MNEM_PUSH_EBX)
#define EMIT_PUSH_ESP       EMIT_BYTE(MNEM_PUSH_ESP)
#define EMIT_PUSH_EBP       EMIT_BYTE(MNEM_PUSH_EBP)
#define EMIT_PUSH_ESI       EMIT_BYTE(MNEM_PUSH_ESI)
#define EMIT_PUSH_EDI       EMIT_BYTE(MNEM_PUSH_EDI)

#define EMIT_POP_REG(_reg) EMIT_BYTE(MNEM_POP_EAX + (_reg))
#define EMIT_POP_EAX       EMIT_BYTE(MNEM_POP_EAX)
#define EMIT_POP_ECX       EMIT_BYTE(MNEM_POP_ECX)
#define EMIT_POP_EDX       EMIT_BYTE(MNEM_POP_EDX)
#define EMIT_POP_EBX       EMIT_BYTE(MNEM_POP_EBX)
#define EMIT_POP_ESP       EMIT_BYTE(MNEM_POP_ESP)
#define EMIT_POP_EBP       EMIT_BYTE(MNEM_POP_EBP)
#define EMIT_POP_ESI       EMIT_BYTE(MNEM_POP_ESI)
#define EMIT_POP_EDI       EMIT_BYTE(MNEM_POP_EDI)

#define EMIT_PUSH_CS  EMIT_BYTE(0x0e)
#define EMIT_PUSH_SS  EMIT_BYTE(0x16)
#define EMIT_PUSH_DS  EMIT_BYTE(0x1e)
#define EMIT_PUSH_ES  EMIT_BYTE(0x06)
#define EMIT_PUSH_FS  EMIT_WORD16(0xa00f)
#define EMIT_PUSH_GS  EMIT_WORD16(0xa80f)

#define EMIT_POP_DS   EMIT_BYTE(0x1f)
#define EMIT_POP_ES   EMIT_BYTE(0x07)
#define EMIT_POP_SS   EMIT_BYTE(0x17)
#define EMIT_POP_FS   EMIT_WORD16(0xa10f)
#define EMIT_POP_GS   EMIT_WORD16(0xa90f)


#define EMIT_LEAVE    EMIT_BYTE(0xc9)
#define EMIT_RET      EMIT_BYTE(MNEM_RET)
#define EMIT_FARRET   EMIT_BYTE(MNEM_RETFAR)
#define EMIT_IRET     EMIT_BYTE(0xcf)
#define EMIT_PUSHF    EMIT_BYTE(0x9c)
#define EMIT_POPF     EMIT_BYTE(0x9d)
#define EMIT_SAHF     EMIT_BYTE(0x9e)
#define EMIT_LAHF     EMIT_BYTE(0x9f)
#define EMIT_CLI      EMIT_BYTE(MNEM_CLI)
#define EMIT_STI      EMIT_BYTE(MNEM_STI)

#define EMIT_CLD      EMIT_BYTE(0xfc)
#define EMIT_STD      EMIT_BYTE(0xfd)

/* Use the 1-byte opcodes for increment/decrement register. On amd's x86-64
 * architecture these opcodes are redefined as prefixes but this doesnt matter
 * as long as we only run in 32-bit compatibility mode. */
#define EMIT_INC_REG(_reg)  { EMIT_BYTE(0x40 + (_reg)); }
#define EMIT_DEC_REG(_reg)  { EMIT_BYTE(0x48 + (_reg)); }
#define EMIT_INC_MEM(_addr) { EMIT_BYTE(0xff); EMIT32_MODRM_FIXEDMEM(0, _addr); }
#define EMIT_DEC_MEM(_addr) { EMIT_BYTE(0xff); EMIT32_MODRM_FIXEDMEM(1, _addr); }

#define EMIT_NOT_REG8(_reg)  { EMIT_BYTE(0xf6); EMIT_MODRM_REG(2, _reg); }
#define EMIT_NOT_REG(_reg)   { EMIT_BYTE(0xf7); EMIT_MODRM_REG(2, _reg); }

#define EMIT_RET_FRAME(_frameSize) {                                          \
   EMIT_BYTE(MNEM_RET_IMM);                                                   \
   EMIT_WORD16(_frameSize);                                                   \
}

#define EMIT_POP_ABS(_addr) {                                                 \
   /* pop m32 */                                                              \
   EMIT_BYTE(0x8f); EMIT_MODRM(0,0,5);                                        \
   EMIT_WORD((Reg32)(_addr));                                                 \
}

#define EMIT_LOAD_FROM_EAX(_reg,_off) {                                       \
   ASSERT((_off) >= 0 && (_off) < 128);                                       \
   EMIT_BYTE(0x8b);                                                           \
   EMIT_MODRM(1,_reg,REG_EAX);                                                \
   EMIT_BYTE(_off);                                                           \
}

#define EMIT_STORE_FROM_EAX(_reg,_off) {                                      \
   ASSERT((_off) >= 0 && (_off) < 128);                                       \
   EMIT_BYTE(0x89);                                                           \
   EMIT_MODRM(1,_reg,REG_EAX);                                                \
   EMIT_BYTE(_off);                                                           \
}


#define EMIT_LOAD_FROM_SP(_reg,_off) {                                        \
   ASSERT((_off) >= 0 && (_off) < 128);                                       \
   EMIT_BYTE(0x8b);                                                           \
   EMIT_MODRM(1,_reg,4); /* SIB escape */                                     \
   EMIT_SIB(0,4/*none*/,REG_ESP);                                             \
   EMIT_BYTE(_off);                                                           \
}

#define EMIT_SAVE_FROM_SP(_reg,_off) {                                        \
   ASSERT((_off) >= 0 && (_off) < 128);                                       \
   EMIT_BYTE(0x89);                                                           \
   EMIT_MODRM(1,_reg,4); /* SIB escape */                                     \
   EMIT_SIB(0,4/*none*/,REG_ESP);                                             \
   EMIT_BYTE(_off);                                                           \
}

#define EMIT_ADJUST_ESP(_off) {                                               \
   ASSERT((_off) >= 0 && (_off) < 128);                                       \
   EMIT_BYTE(0x83); /* add imm8 */                                            \
   EMIT_MODRM(3,0,REG_ESP);                                                   \
   EMIT_BYTE(_off);                                                           \
}


#define EMIT_TEST_IMM8_FROM_SP(_imm8, _off) {                                 \
   ASSERT((_off)  >= 0 && (_off)  < 128);                                     \
   ASSERT((_imm8) >= 0 && (_imm8) < 128);                                     \
   EMIT_BYTE(0xf6);                                                           \
   EMIT_MODRM(1,0,4); /* SIB escape */                                        \
   EMIT_SIB(0,4/*none*/,REG_ESP);                                             \
   EMIT_BYTE(_off);                                                           \
   EMIT_BYTE(_imm8);                                                          \
}

#define EMIT32_TEST_IMM8_ABS(_imm8, _abs) {                                   \
   ASSERT((_imm8) >=- 128 && (_imm8) <= 127);                                 \
   EMIT_BYTE(0xf6);                                                           \
   EMIT32_MODRM_FIXEDMEM(0, _abs);                                            \
   EMIT_BYTE(_imm8);                                                          \
}

/* test _disp[_base], _imm8 */
#define EMIT32_TEST_IMM8_MEM(_imm8, _disp, _base) {                           \
   EMIT_BYTE(0xf6);                                                           \
   EMIT_MODRM_MEM(0, _base, _disp);                                           \
   EMIT_BYTE(_imm8);                                                          \
}

#define EMIT32_CMP_REG_FROM_SP(_reg,_off) {                                   \
   ASSERT((_off) >= 0 && (_off) < 128);                                       \
   EMIT_BYTE(0x39);                                                           \
   EMIT_MODRM(1,_reg,4); /* SIB escape */                                     \
   EMIT_SIB(0,4/*none*/,REG_ESP);                                             \
   EMIT_BYTE(_off);                                                           \
}

/*
 * Note that the operand override below is in fact necessary, as implausible
 * as that may seem. Without it, protected mode stores of segment registers
 * zero out the high order 16 bits of the destination. Contrast the footnote
 * for this form of the MOV instruction in Intel's Pentium Processor Family
 * development manual:
 *
 *      "* In 32-bit mode, use 16-bit operand size prefix ...."
 *
 * with that found in more recent manuals:
 *
 *      "** In 32-bit mode, the assembler may require the use of the 16-bit
 *      operand size prefix ...."
 *
 * Confusingly, the second quote makes one think that the use of such an
 * operand really is optional; the sense of the footnote is the opposite.
 *
 * Also see below...
 */
#define EMIT_SAVE_SEGMENT(_seg,_disp,_base) {                                 \
   EMIT_OPERAND_OVERRIDE; EMIT_BYTE(0x8c);                                    \
   if ((_base) == MODRM_RM_DISP32) {                                          \
      EMIT32_MODRM_FIXEDMEM(_seg,_disp);                                      \
   } else {                                                                   \
      EMIT_MODRM_MEM(_seg,_base,(VA)(_disp));                                 \
   }                                                                          \
}

/*
 * On the other hand, the manual is mute on the subject of loading segment
 * registers from memory in 32-bit mode. Somewhat scarily, one assembler I
 * tried does emit the operand override byte before seg<-memory loads, but
 * experimentally these loads appear to work correctly.
 *
 * Since these EMIT's appear on, e.g., the fast system call path, saving the
 * cycle and leaving out the operand override seems wise.
 *
 * kma 11/30/2000
 */
#define EMIT_LOAD_SEGMENT(_seg,_disp,_base) {                                 \
   EMIT_BYTE(0x8e);                                                           \
   if ((_base) == MODRM_RM_DISP32) {                                          \
      EMIT32_MODRM_FIXEDMEM(_seg,(VA)(_disp));                                \
   } else {                                                                   \
      EMIT_MODRM_MEM(_seg,_base,(VA)(_disp));                                 \
   }                                                                          \
}

#define EMIT_LOAD_SEGMENT_REG(_dstSeg, _srcReg) {                             \
   EMIT_BYTE(0x8e);                                                           \
   EMIT_MODRM_REG(_dstSeg, _srcReg);                                          \
}

#define EMIT32_AND_IMM32_TO_SP(_imm32,_off) {                                 \
   ASSERT((_off) >= 0 && (_off) < 128);                                       \
   EMIT_BYTE(0x81);                                                           \
   EMIT_MODRM(1,4,4); /* SIB escape */                                        \
   EMIT_SIB(0,4/*none*/,REG_ESP);                                             \
   EMIT_BYTE(_off);                                                           \
   EMIT_WORD(_imm32);                                                         \
}

#define EMIT32_AND_IMM32_TO_REG(_imm,_off,_base) {                            \
   ASSERT((_off) >= 0 && (_off) < 128);                                       \
   EMIT_BYTE(0x81);                                                           \
   EMIT_MODRM(1,4,_base);                                                     \
   EMIT_BYTE(_off);                                                           \
   EMIT_WORD(_imm);                                                           \
}

#define EMIT32_ADD_IMM8_TO_REG(_imm,_off,_base) {                             \
   ASSERT((_imm) >= 0 && (_imm) < 128);                                       \
   ASSERT((_off) >= 0 && (_off) < 128);                                       \
   EMIT_BYTE(0x83);                                                           \
   EMIT_MODRM(1,0,_base);                                                     \
   EMIT_BYTE(_off);                                                           \
   EMIT_BYTE(_imm);                                                           \
}

#define EMIT32_OPCODE_DISP8_FROM_REG(_opcode,_reg,_disp8,_basereg) {          \
   ASSERT((_disp8) >= -128 && (_disp8) <= 127);                               \
   EMIT_BYTE(_opcode);                                                        \
   EMIT_MODRM(1,_reg,_basereg);                                               \
   EMIT_BYTE(_disp8);                                                         \
}

#define EMIT32_TEST_IMM32_FROM_SP(_imm32,_off) {                              \
   ASSERT((_off) >= 0 && (_off) < 128);                                       \
   EMIT_BYTE(0xf7);                                                           \
   EMIT_MODRM(1,0,4); /* SIB escape */                                        \
   EMIT_SIB(0,4/*none*/,REG_ESP);                                             \
   EMIT_BYTE(_off);                                                           \
   EMIT_WORD(_imm32);                                                         \
}

#define EMIT_SAVE_SEGMENT_REG(_srcSeg, _dstReg) {                             \
   EMIT_BYTE(0x8c);                                                           \
   EMIT_MODRM_REG(_srcSeg, _dstReg);                                          \
}

#define EMIT_LSS_SS_ESP(_addr) {                                              \
   EMIT_BYTE(0x0f); EMIT_BYTE(0xb2);                                          \
   EMIT32_MODRM_FIXEDMEM(REG_ESP,_addr);                                      \
}


#define EMIT32int_OP_REG_IMM(_first,_mod,_reg,_imm) {                         \
   EMIT_BYTE(_first);                                                         \
   EMIT_MODRM_REG(_mod,_reg);                                                 \
   EMIT_WORD(_imm);                                                           \
}

#define EMITint_OP_REG_IMM8(_first, _mod, _reg, _imm8) {                      \
   EMIT_BYTE(_first);                                                         \
   EMIT_MODRM_REG(_mod, _reg);                                                \
   EMIT_BYTE(_imm8);                                                          \
}

#define EMIT_AND_REG_IMM8(_reg,_imm)  EMITint_OP_REG_IMM8(0x83, 4, _reg, _imm)
#define EMIT_OR_REG_IMM8(_reg,_imm)   EMITint_OP_REG_IMM8(0x83, 1, _reg, _imm)
#define EMIT_ADD_REG_IMM8(_reg,_imm)  EMITint_OP_REG_IMM8(0x83, 0, _reg, _imm)
#define EMIT_CMP_REG_IMM8(_reg,_imm)  EMITint_OP_REG_IMM8(0x83, 7, _reg, _imm)

#define EMIT32_AND_REG_IMM(_reg,_imm)   EMIT32int_OP_REG_IMM(0x81,4,_reg,_imm)
#define EMIT32_AND_EAX_IMM(_imm)        { EMIT_BYTE(0x25); EMIT_WORD(_imm); }
#define EMIT32_OR_REG_IMM(_reg,_imm)    EMIT32int_OP_REG_IMM(0x81,1,_reg,_imm)
#define EMIT32_OR_EAX_IMM(_imm)         { EMIT_BYTE(0x0d); EMIT_WORD(_imm); }
#define EMIT32_ADD_REG_IMM(_reg,_imm)   EMIT32int_OP_REG_IMM(0x81,0,_reg,_imm)
#define EMIT32_ADD_EAX_IMM(_imm)        { EMIT_BYTE(0x05); EMIT_WORD(_imm); }
#define EMIT32_CMP_REG_IMM(_reg,_imm)   EMIT32int_OP_REG_IMM(0x81,7,_reg,_imm)
#define EMIT32_XOR_REG_IMM(_reg,_imm)   EMIT32int_OP_REG_IMM(0x81,6,_reg,_imm)

#define EMIT32_CMP_EAX_IMM(_imm) { EMIT_BYTE(MNEM_CMP_EAX); EMIT_WORD(_imm);   }
#define EMIT16_CMP_EAX_IMM(_imm) { EMIT_BYTE(MNEM_CMP_EAX); EMIT_WORD16(_imm); }

#define EMIT16_ADD_EAX_IMM(_imm) { EMIT_BYTE(0x05); EMIT_WORD16(_imm); }
#define EMIT16_AND_EAX_IMM(_imm) { EMIT_BYTE(0x25); EMIT_WORD16(_imm); }
#define EMIT16_OR_EAX_IMM(_imm)  { EMIT_BYTE(0x0d); EMIT_WORD16(_imm); }

#define EMIT16_AND_REG_IMM(_reg,_imm)  {                                      \
   EMIT_BYTE(0x81);                                                           \
   EMIT_MODRM_REG(4,_reg);                                                    \
   EMIT_WORD16(_imm);                                                         \
}

#define EMIT_ADD_AL_IMM8(_imm) { EMIT_BYTE(0x04); EMIT_BYTE(_imm); }
#define EMIT_ADD_AH_IMM8(_imm) {                                              \
   EMIT_BYTE(0x80);                                                           \
   EMIT_MODRM_REG(0, REG8_AH);                                                \
   EMIT_BYTE((_imm));                                                         \
}

#define EMIT_AND_REG8_IMM8(_reg, _imm)  {                                     \
   if ((_reg) == REG_EAX) {                                                   \
      EMIT_BYTE(0x24);                                                        \
   } else {                                                                   \
      EMIT_BYTE(0x80);                                                        \
      EMIT_MODRM_REG(4,_reg);                                                 \
   }                                                                          \
   EMIT_BYTE(_imm);                                                           \
}

#define EMIT_ADDC_REG8_IMM8(_reg, _imm)  {                                    \
   if ((_reg) == REG_EAX) {                                                   \
      EMIT_BYTE(0x14);                                                        \
   } else {                                                                   \
      EMIT_BYTE(0x80);                                                        \
      EMIT_MODRM_REG(2,_reg);                                                 \
   }                                                                          \
   EMIT_BYTE(_imm);                                                           \
}

#define EMIT_CMP_REG8_IMM8(_reg, _imm)  {                                     \
   if ((_reg) == REG_EAX) {                                                   \
      EMIT_BYTE(0x3c);                                                        \
   } else {                                                                   \
      EMIT_BYTE(0x80);                                                        \
      EMIT_MODRM_REG(7,_reg);                                                 \
   }                                                                          \
   EMIT_BYTE(_imm);                                                           \
}

#define EMIT16_OR_REG_IMM(_reg,_imm) {                                        \
   EMIT_BYTE(0x81);                                                           \
   EMIT_MODRM_REG(1,_reg);                                                    \
   EMIT_WORD16(_imm);                                                         \
}

#define EMIT_CMP_REG_IMM(_codeSize, _reg, _imm) {                             \
   if ((_codeSize) == SIZE_32BIT) {                                           \
      if ((_reg) == REG_EAX) {                                                \
         EMIT32_CMP_EAX_IMM(_imm);                                            \
      } else {                                                                \
         EMIT32_CMP_REG_IMM(_reg, _imm);                                      \
      }                                                                       \
   } else {                                                                   \
      if ((_reg) == REG_EAX) {                                                \
         EMIT16_CMP_EAX_IMM(_imm);                                            \
      } else {                                                                \
         EMIT16_CMP_REG_IMM(_reg, _imm);                                      \
      }                                                                       \
   }                                                                          \
}

#define EMIT_AND_REG_IMM(_codeSize, _reg, _imm) {                             \
   if ((_codeSize) == SIZE_32BIT) {                                           \
      if ((_reg) == REG_EAX) {                                                \
         EMIT32_AND_EAX_IMM(_imm);                                            \
      } else {                                                                \
         EMIT32_AND_REG_IMM(_reg, _imm);                                      \
      }                                                                       \
   } else {                                                                   \
      if ((_reg) == REG_EAX) {                                                \
         EMIT16_AND_EAX_IMM(_imm);                                            \
      } else {                                                                \
         EMIT16_AND_REG_IMM(_reg, _imm);                                      \
      }                                                                       \
   }                                                                          \
}

#define EMIT_OR_REG_IMM(_codeSize, _reg, _imm) {                              \
   if ((_codeSize) == SIZE_32BIT) {                                           \
      if ((_reg) == REG_EAX) {                                                \
         EMIT32_OR_EAX_IMM(_imm);                                             \
      } else {                                                                \
         EMIT32_OR_REG_IMM(_reg, _imm);                                       \
      }                                                                       \
   } else {                                                                   \
      if ((_reg) == REG_EAX) {                                                \
         EMIT16_OR_EAX_IMM(_imm);                                             \
      } else {                                                                \
         EMIT16_OR_REG_IMM(_reg, _imm);                                       \
      }                                                                       \
   }                                                                          \
}

#define EMIT_SHR_REG_IMM(_reg,_imm) {                                         \
   EMIT_BYTE(0xc1);                                                           \
   EMIT_MODRM_REG(5,_reg);                                                    \
   EMIT_BYTE(_imm);                                                           \
}

#define EMIT_SHL_REG_IMM(_reg,_imm) {                                         \
   EMIT_BYTE(0xc1);                                                           \
   EMIT_MODRM_REG(4,_reg);                                                    \
   EMIT_BYTE(_imm);                                                           \
}

#define EMIT16_CMP_REG_IMM(_reg,_imm) {                                       \
   EMIT_BYTE(0x81);                                                           \
   EMIT_MODRM_REG(7,_reg);                                                    \
   EMIT_WORD16(_imm);                                                         \
}

/*
 * Sets flags according to (_rega - _regb)
 */
#define EMIT_CMP_REG_REG(_rega, _regb) {                                      \
   EMIT_BYTE(0x39);                                                           \
   EMIT_MODRM_REG(_regb,_rega);                                               \
}

/*
 * Sets flags according to (_rega - _regb)
 */
#define EMIT_CMP_REG8_REG8(_rega, _regb) {                                    \
   EMIT_BYTE(0x38);                                                           \
   EMIT_MODRM_REG(_regb,_rega);                                               \
}

// direction unclear
#define EMIT_TEST_REG_REG(_rega, _regb) {                                     \
   EMIT_BYTE(0x85);                                                           \
   EMIT_MODRM_REG(_rega,_regb);                                               \
}

#define EMIT_TEST_REG8_REG8(_src, _dst) {                                     \
   EMIT_BYTE(0x84);                                                           \
   EMIT_MODRM_REG(_src, _dst);                                                \
}

#define EMIT32_TEST_REG_IMM(_reg, _imm) {                                     \
   EMIT_BYTE(0xf7);                                                           \
   EMIT_MODRM_REG(0, _reg);                                                   \
   EMIT_WORD(_imm);                                                           \
}

#define EMIT16_TEST_REG_IMM(_reg, _imm) {                                     \
   EMIT_BYTE(0xf7);                                                           \
   EMIT_MODRM_REG(0, _reg);                                                   \
   EMIT_WORD16(_imm);                                                         \
}


#define EMIT_TEST_REG8_IMM8(_reg,_imm) {                                      \
   if ((_reg) == REG_EAX) {                                                   \
      EMIT_BYTE(0xa8);                                                        \
   } else {                                                                   \
      EMIT_BYTE(0xf6);                                                        \
      EMIT_MODRM_REG(0,_reg);                                                 \
   }                                                                          \
   EMIT_BYTE(_imm);                                                           \
}


#define EMIT_ADD_REG_IMM(_reg,_imm)   EMIT32_ADD_REG_IMM(_reg,_imm) // XX bad

#define EMIT16_ADD_REG_IMM(_reg,_imm) {                                       \
   EMIT_BYTE(0x81);                                                           \
   EMIT_MODRM_REG(0,_reg);                                                    \
   EMIT_WORD16(_imm);                                                         \
}

#define EMIT_LOAD_REG8_IMM8(_reg8, _imm8) {                                   \
   EMIT_BYTE(0xb0 + (_reg8));                                                 \
   EMIT_BYTE(_imm8);                                                          \
}

#define EMIT_OR_REG8_IMM8(_reg, _imm) {                                       \
   if ((_reg) == REG_EAX) {                                                   \
      EMIT_BYTE(0x0c);                                                        \
   } else {                                                                   \
      EMIT_BYTE(0x80);                                                        \
      EMIT_MODRM_REG(1,_reg);                                                 \
   }                                                                          \
   EMIT_BYTE(_imm);                                                           \
}
   
#define EMIT32_OR_DISP8_IMM(_reg, _disp8, _imm32) {                           \
   EMIT_BYTE(0x81);                                                           \
   EMIT_MODRM(1, 1, 4);                                                       \
   EMIT_SIB(0, 4, _reg);                                                      \
   EMIT_BYTE(_disp8);                                                         \
   EMIT_WORD32(_imm32);                                                       \
}

#define EMIT_OR_REG_REG(_src,_dst) {                                          \
   EMIT_BYTE(0x0b);                                                           \
   EMIT_MODRM_REG(_dst,_src);                                                 \
}

#define EMIT_OR_REG8_REG8(_src,_dst) {                                        \
   EMIT_BYTE(0x0a);                                                           \
   EMIT_MODRM_REG(_dst,_src);                                                 \
}

#define EMIT_SHL_REG8_IMM8(_reg, _imm) {                                      \
   if ((_imm) == 1) {                                                         \
      EMIT_BYTE(0xd0);                                                        \
   } else {                                                                   \
      EMIT_BYTE(0xc0);                                                        \
   }                                                                          \
   EMIT_MODRM_REG(4, _reg);                                                   \
   if ((_imm) != 1) {                                                         \
      EMIT_BYTE(_imm);                                                        \
   }                                                                          \
}

#define EMIT_SHR_REG8_IMM8(_reg, _imm) {                                      \
   if ((_imm) == 1) {                                                         \
      EMIT_BYTE(0xd0);                                                        \
   } else {                                                                   \
      EMIT_BYTE(0xc0);                                                        \
   }                                                                          \
   EMIT_MODRM_REG(5, _reg);                                                   \
   if ((_imm) != 1) {                                                         \
      EMIT_BYTE(_imm);                                                        \
   }                                                                          \
}

#define EMIT_ADD_REG_REG(_src,_dst) {                                         \
   EMIT_BYTE(MNEM_ADD);                                                       \
   EMIT_MODRM(0x3,_dst,_src);                                                 \
}

#define EMIT_XOR_REG_REG(_src,_dst) {                                         \
   EMIT_BYTE(0x33);                                                           \
   EMIT_MODRM(0x3,_dst,_src);                                                 \
}

#define EMIT_XOR_REG8_REG8(_src,_dst) {                                       \
   EMIT_BYTE(0x32);                                                           \
   EMIT_MODRM(0x3,_dst,_src);                                                 \
}

#define EMIT32_SUB_REG_REG(_src,_dst) {                                       \
   EMIT_BYTE(MNEM_SUB);                                                       \
   EMIT_MODRM(0x3,_dst,_src);                                                 \
}

#define EMIT32int_MODRM_FIXEDMEM_IMM(_mnem,_reg,_abs,_imm) {                  \
   EMIT_BYTE(_mnem);                                                          \
   EMIT32_MODRM_FIXEDMEM(_reg,(VA)(_abs));                                    \
   EMIT_WORD(_imm);                                                           \
}

#define EMIT16int_MODRM_FIXEDMEM_IMM(_mnem,_reg,_abs,_imm) {                  \
   EMIT_BYTE(_mnem);                                                          \
   EMIT16_MODRM_FIXEDMEM(_reg,(VA)(_abs));                                    \
   EMIT_WORD16(_imm);                                                         \
}

#define EMIT32int_MODRM_FIXEDMEM_IMM8(_mnem,_reg,_abs,_imm) {                 \
   EMIT_BYTE(_mnem);                                                          \
   EMIT32_MODRM_FIXEDMEM(_reg,(VA)(_abs));                                    \
   EMIT_BYTE(_imm);                                                           \
}

#define EMIT16int_MODRM_FIXEDMEM_IMM8(_mnem,_reg,_abs,_imm) {                 \
   EMIT_BYTE(_mnem);                                                          \
   EMIT16_MODRM_FIXEDMEM(_reg,(VA)(_abs));                                    \
   EMIT_BYTE(_imm);                                                           \
}

#define EMIT16int_MODRM_FIXEDMEM_IMM32(_mnem,_reg,_abs,_imm) {                \
   EMIT_BYTE(_mnem);                                                          \
   EMIT16_MODRM_FIXEDMEM(_reg,(VA)(_abs));                                    \
   EMIT_WORD(_imm);                                                           \
}

#define EMIT32int_MODRM_FIXEDMEM_REG(_mnem,_abs,_reg) {                       \
   EMIT_BYTE(_mnem);                                                          \
   ASSERT(sizeof _abs == 4);                                                  \
   EMIT32_MODRM_FIXEDMEM(_reg,(VA)(_abs));                                    \
}

#define EMIT16int_MODRM_FIXEDMEM_REG(_mnem,_abs,_reg) {                       \
   EMIT_BYTE(_mnem);                                                          \
   EMIT16_MODRM_FIXEDMEM(_reg,(VA)(_abs));                                    \
}


/* 
 * For all the following, EMITxx(destination, source)
 */

#define EMIT16_BT_FIXEDMEM_IMM(_abs,_imm) {                                   \
   EMIT_BYTE(MNEM_OPCODE_ESC);                                                \
   EMIT16int_MODRM_FIXEDMEM_IMM8(0xba,4,_abs,_imm)                            \
}

#define EMIT16_TEST_FIXEDMEM_IMM(_abs,_imm)                                   \
   EMIT16int_MODRM_FIXEDMEM_IMM(0xf7,0,_abs,_imm)

#define EMIT16_TEST_FIXEDMEM_IMM32(_abs,_imm)                                 \
   EMIT16int_MODRM_FIXEDMEM_IMM32(0xf7,0,_abs,_imm)

#define EMIT16_TEST_FIXEDMEM8_IMM8(_abs,_imm)                                 \
   EMIT16int_MODRM_FIXEDMEM_IMM8(0xf6,0,_abs,_imm)

#define EMIT16_OR_FIXEDMEM8_IMM8(_abs,_imm)                                   \
   EMIT16int_MODRM_FIXEDMEM_IMM8(0x80,1,_abs,_imm)

#define EMIT16_OR_FIXEDMEM_IMM(_abs,_imm)                                     \
   EMIT16int_MODRM_FIXEDMEM_IMM(0x81,1,_abs,_imm)

#define EMIT16_OR_FIXEDMEM_IMM32(_abs,_imm)                                   \
   EMIT16int_MODRM_FIXEDMEM_IMM32(0x81,1,_abs,_imm)

#define EMIT16_AND_FIXEDMEM8_IMM8(_abs,_imm)                                  \
   EMIT16int_MODRM_FIXEDMEM_IMM8(0x80,4,_abs,_imm)

#define EMIT16_AND_FIXEDMEM_IMM(_abs,_imm)                                    \
   EMIT16int_MODRM_FIXEDMEM_IMM(0x81,4,_abs,_imm)

#define EMIT16_AND_FIXEDMEM_IMM32(_abs,_imm)                                  \
   EMIT16int_MODRM_FIXEDMEM_IMM32(0x81,4,_abs,_imm)

#define EMIT16_ADD_FIXEDMEM_IMM(_abs,_imm)                                    \
   EMIT16int_MODRM_FIXEDMEM_IMM(0x81,0,_abs,_imm)

#define EMIT16_CMP_FIXEDMEM8_IMM8(_abs,_imm)                                  \
   EMIT16int_MODRM_FIXEDMEM_IMM8(0x80,7,_abs,_imm)

#define EMIT16_CMP_FIXEDMEM_IMM(_abs,_imm)                                    \
   EMIT16int_MODRM_FIXEDMEM_IMM(0x81,7,_abs,_imm)

#define EMIT16_CMP_FIXEDMEM_REG(_abs,_reg)                                    \
   EMIT16int_MODRM_FIXEDMEM_REG(0x39,_abs,_reg)

#define EMIT16_OR_REG_FIXEDMEM(_reg,_abs)                                     \
   EMIT16int_MODRM_FIXEDMEM_REG(0xb,_abs,_reg)

#define EMIT32_BT_FIXEDMEM_IMM(_abs,_imm) {                                   \
   EMIT_BYTE(MNEM_OPCODE_ESC);                                                \
   EMIT32int_MODRM_FIXEDMEM_IMM8(0xba,4,_abs,_imm)                            \
}

#define EMIT32_BT_FIXEDMEM_REG(_abs, _reg) {                                  \
   EMIT_BYTE(MNEM_OPCODE_ESC);                                                \
   EMIT_BYTE(0xa3);                                                           \
   EMIT32_MODRM_FIXEDMEM(_reg, _abs);                                         \
}

#define EMIT32_TEST_FIXEDMEM8_IMM8(_abs,_imm)                                 \
   EMIT32int_MODRM_FIXEDMEM_IMM8(0xf6,0,_abs,_imm)

#define EMIT32_TEST_FIXEDMEM_IMM(_abs,_imm)                                   \
   EMIT32int_MODRM_FIXEDMEM_IMM(0xf7,0,_abs,_imm)

#define EMIT32_OR_FIXEDMEM8_IMM8(_abs,_imm)                                   \
   EMIT32int_MODRM_FIXEDMEM_IMM8(0x80,1,_abs,_imm)

#define EMIT32_OR_FIXEDMEM_IMM(_abs,_imm)                                     \
   EMIT32int_MODRM_FIXEDMEM_IMM(0x81,1,_abs,_imm)

#define EMIT32_AND_FIXEDMEM8_IMM8(_abs,_imm)                                  \
   EMIT32int_MODRM_FIXEDMEM_IMM8(0x80,4,_abs,_imm)

#define EMIT32_AND_FIXEDMEM_IMM(_abs,_imm)                                    \
   EMIT32int_MODRM_FIXEDMEM_IMM(0x81,4,_abs,_imm)

#define EMIT32_ADD_FIXEDMEM8_IMM8(_abs,_imm)                                  \
   EMIT32int_MODRM_FIXEDMEM_IMM8(0x80,0,_abs,_imm)

#define EMIT32_ADD_FIXEDMEM_IMM(_abs,_imm)                                    \
   EMIT32int_MODRM_FIXEDMEM_IMM(0x81,0,_abs,_imm)

#define EMIT32_ADD_FIXEDMEM_IMM8(_abs,_imm)                                   \
   EMIT32int_MODRM_FIXEDMEM_IMM8(0x83,0,_abs,_imm)

#define EMIT32_SUB_FIXEDMEM_IMM(_abs,_imm)                                    \
   EMIT32int_MODRM_FIXEDMEM_IMM(0x81,5,_abs,_imm)

#define EMIT32_SUB_FIXEDMEM8_IMM8(_abs,_imm)                                  \
   EMIT32int_MODRM_FIXEDMEM_IMM8(0x80,5,_abs,_imm)

#define EMIT32_SUB_FIXEDMEM_IMM8(_abs,_imm)                                   \
   EMIT32int_MODRM_FIXEDMEM_IMM8(0x83,5,_abs,_imm)

#define EMIT32_CMP_FIXEDMEM8_IMM8(_abs,_imm)                                  \
   EMIT32int_MODRM_FIXEDMEM_IMM8(0x80,7,_abs,_imm)

#define EMIT32_CMP_FIXEDMEM_IMM(_abs,_imm)                                    \
   EMIT32int_MODRM_FIXEDMEM_IMM(0x81,7,_abs,_imm)

#define EMIT32_CMP_FIXEDMEM_IMM8(_abs,_imm)                                   \
   EMIT32int_MODRM_FIXEDMEM_IMM8(0x83,7,_abs,_imm)

/* Compute "*_abs - _reg" and throw away result. */
#define EMIT32_CMP_FIXEDMEM_REG(_abs, _reg)                                   \
   EMIT32int_MODRM_FIXEDMEM_REG(0x39, _abs, _reg)

/* Compute "_reg - *_abs" and throw away result. */
#define EMIT32_CMP_REG_FIXEDMEM(_reg, _abs)                                   \
   EMIT32int_MODRM_FIXEDMEM_REG(0x3b, _abs, _reg)

#define EMIT32_OR_FIXEDMEM_REG(_abs,_reg)                                     \
   EMIT32int_MODRM_FIXEDMEM_REG(0x09,_abs,_reg)

#define EMIT32_CMP_FIXEDMEM8_REG8(_abs,_reg)                                  \
   EMIT32int_MODRM_FIXEDMEM_REG(0x38,_abs,_reg)

#define EMIT32_AND_REG8_FIXEDMEM8(_reg,_abs)                                  \
   EMIT32int_MODRM_FIXEDMEM_REG(0x22,_abs,_reg)

#define EMIT32_AND_REG_FIXEDMEM(_reg,_abs)                                    \
   EMIT32int_MODRM_FIXEDMEM_REG(0x23,_abs,_reg)

#define EMIT32_AND_FIXEDMEM_REG(_reg,_abs)                                    \
   EMIT32int_MODRM_FIXEDMEM_REG(0x21,_abs,_reg)

#define EMIT32_OR_REG_FIXEDMEM(_reg,_abs)                                     \
   EMIT32int_MODRM_FIXEDMEM_REG(0x0b,_abs,_reg)

#define EMIT32_XOR_REG_FIXEDMEM(_reg,_abs)                                    \
   EMIT32int_MODRM_FIXEDMEM_REG(0x33,_abs,_reg)

#define EMIT32_XOR_FIXEDMEM_REG(_reg,_abs)                                    \
   EMIT32int_MODRM_FIXEDMEM_REG(0x31,_abs,_reg)

#define EMIT32_LOAD_SEG_FIXEDMEM(_seg,_abs)                                   \
   EMIT32int_MODRM_FIXEDMEM_REG(0x8e,_abs,_seg)

#define EMIT_AND_FIXEDMEM8_IMM8(_codeSize, _abs, _imm) {                      \
   if ((_codeSize) == SIZE_16BIT) {                                           \
      EMIT16_AND_FIXEDMEM8_IMM8(_abs, _imm);                                  \
   } else {                                                                   \
      EMIT32_AND_FIXEDMEM8_IMM8(_abs, _imm);                                  \
   }                                                                          \
}

#define EMIT_OR_FIXEDMEM8_IMM8(_codeSize, _abs, _imm) {                       \
   if ((_codeSize) == SIZE_16BIT) {                                           \
      EMIT16_OR_FIXEDMEM8_IMM8(_abs, _imm);                                   \
   } else {                                                                   \
      EMIT32_OR_FIXEDMEM8_IMM8(_abs, _imm);                                   \
   }                                                                          \
}

#define EMIT_TEST_FIXEDMEM8_IMM8(_codeSize, _abs, _imm) {                     \
   if ((_codeSize) == SIZE_16BIT) {                                           \
      EMIT16_TEST_FIXEDMEM8_IMM8(_abs, _imm);                                 \
   } else {                                                                   \
      EMIT32_TEST_FIXEDMEM8_IMM8(_abs, _imm);                                 \
   }                                                                          \
}

#define EMIT_CMP_FIXEDMEM8_IMM8(_codeSize, _abs, _imm) {                      \
   if ((_codeSize) == SIZE_16BIT) {                                           \
      EMIT16_CMP_FIXEDMEM8_IMM8(_abs, _imm);                                  \
   } else {                                                                   \
      EMIT32_CMP_FIXEDMEM8_IMM8(_abs, _imm);                                  \
   }                                                                          \
}

#define EMIT_OR_REG_FIXEDMEM(_codeSize, _reg, _abs) {                         \
   if ((_codeSize) == SIZE_16BIT) {                                           \
      EMIT16_OR_REG_FIXEDMEM(_reg, _abs);                                     \
   } else {                                                                   \
      EMIT32_OR_REG_FIXEDMEM(_reg, _abs);                                     \
   }                                                                          \
}

#define EMIT_SAL_REGISTER(_reg,_shift) {                                      \
   if ((_shift) == 1) {                                                       \
      EMIT_BYTE(0xd1); EMIT_MODRM_REG(4,_reg);                                \
   } else {                                                                   \
      EMIT_BYTE(0xc1); EMIT_MODRM_REG(4,_reg); EMIT_BYTE(_shift);             \
   }                                                                          \
}

#define EMIT_SHR_REGISTER(_reg,_shift) {                                      \
   if ((_shift) == 1) {                                                       \
      EMIT_BYTE(0xd1); EMIT_MODRM_REG(5,_reg);                                \
   } else {                                                                   \
      EMIT_BYTE(0xc1); EMIT_MODRM_REG(5,_reg); EMIT_BYTE(_shift);             \
   }                                                                          \
}

/* cmp _reg, _disp(_base) */
#define EMIT32_CMP_REG_MEM(_reg, _disp, _base) {                              \
   EMIT_BYTE(MNEM_CMP);                                                       \
   EMIT_MODRM_MEM(_reg, _base, _disp);                                        \
}

/* Initialize branch displacement to -2 (infinite loop),
   to catch missing resolves. */
#define PREPARE_SHORT_BRANCH(_codeSize, _instptr,_jcc) {                      \
   EMIT_OPERAND_IF_16(_codeSize);                                             \
   _instptr = memptr;                                                         \
   EMIT_BYTE(_jcc);                                                           \
   EMIT_BYTE(-2);                                                             \
}

#define RESOLVE_SHORT_BRANCH(_instptr) {                                      \
   TCA _instptr1 = (_instptr);                                                \
   int _disp = memptr - (_instptr1 + 2);                                      \
   ASSERT(_disp >= -128 && _disp <= 127);                                     \
   _instptr1[1] = _disp;                                                      \
}

#define PREPARE32_LONG_JUMPIMM(_instptr) {                                    \
   _instptr = memptr;                                                         \
   *(uint8*)memptr = MNEM_JUMP_LONG;                                          \
   memptr += 5;                                                               \
}

#define RESOLVE32_LONG_JUMPIMM(_instptr) {                                    \
   int disp = memptr - (_instptr) - 5;                                        \
   *((uint32*)(_instptr + 1)) = disp;                                         \
}

#define PREPARE_LONG_JUMPIMM(_cSz, _instptr) {                                \
   EMIT_OPERAND_IF_16(_cSz);                                                  \
   PREPARE32_LONG_JUMPIMM(_instptr);                                          \
}

#define RESOLVE_LONG_JUMPIMM(_instptr)                                        \
   RESOLVE32_LONG_JUMPIMM(_instptr)

#define PREPARE32_LONG_BRANCH(_instptr,_jcc) {                                \
   _instptr = memptr;                                                         \
   *(uint16*)memptr = (_jcc);                                                 \
   memptr += 6;                                                               \
}

#define RESOLVE32_LONG_BRANCH(_instptr) {                                     \
   int disp = memptr - (_instptr) - 6;                                        \
   *((uint32*)(_instptr + 2)) = disp;                                         \
}


#define PREPARE_LONG_BRANCH(_codeSize,_instptr,_jcc) {                        \
   EMIT_OPERAND_IF_16(_codeSize);                                             \
   PREPARE32_LONG_BRANCH(_instptr,_jcc);                                      \
}

#define RESOLVE_LONG_BRANCH(_instptr)                                         \
   RESOLVE32_LONG_BRANCH(_instptr)

#define RESOLVE32_BRANCH(_instptr,_isLong) {                                  \
   if (_isLong) {                                                             \
      RESOLVE32_LONG_BRANCH(_instptr);                                        \
   } else {                                                                   \
      RESOLVE_SHORT_BRANCH(_instptr);                                         \
   }                                                                          \
}

#define EMIT_SIB_DISP32(_codeSize,_opcode,_modrm,_sib,_base) {                \
   EMIT_USING_GS;                                                             \
   EMIT_ADDRESS_IF_16(_codeSize);                                             \
   EMIT_BYTE(_opcode);                                                        \
   EMIT_BYTE(_modrm);                                                         \
   EMIT_BYTE(_sib);                                                           \
   EMIT_WORD(_base);                                                          \
}


#define EMIT_USING_FS         EMIT_BYTE(MNEM_PREFIX_FS)
#define EMIT_USING_CS         EMIT_BYTE(MNEM_PREFIX_CS)
#define EMIT_USING_DS         EMIT_BYTE(MNEM_PREFIX_DS)
#define EMIT_USING_GS         EMIT_BYTE(MNEM_PREFIX_GS)
#define EMIT_USING_ES         EMIT_BYTE(MNEM_PREFIX_ES)
#define EMIT_USING_SS         EMIT_BYTE(MNEM_PREFIX_SS)
#define EMIT_OPERAND_OVERRIDE EMIT_BYTE(MNEM_PREFIX_OPSIZE)
#define EMIT_ADDRESS_OVERRIDE EMIT_BYTE(MNEM_PREFIX_ASIZE)
#define EMIT_OPSIZE_OVERRIDE  EMIT_BYTE(MNEM_PREFIX_OPSIZE)

#define EMIT_INT3        EMIT_BYTE(MNEM_INT3)
#define EMIT_INTO        EMIT_BYTE(MNEM_INTO)
#define EMIT_INTN(_n)  { EMIT_BYTE(MNEM_INTN); EMIT_BYTE(_n); }

#define EMIT_SYSENTER    EMIT_WORD16(MNEM_SYSENTER)
#define EMIT_SYSEXIT     EMIT_WORD16(MNEM_SYSEXIT)

/* ****************************************************************
 * system-level stuff
 * ****************************************************************/

#define EMIT_LAR_REG(_srcReg, _dstReg) {                                      \
   EMIT_WORD16(MNEM_LAR); EMIT_MODRM_REG(_srcReg, _dstReg);                   \
}

#define EMIT_SGDT(_disp,_base) {                                              \
   EMIT_BYTE(0xf); EMIT_BYTE(0x1);  EMIT_MODRM_MEM(0,_base,_disp)             \
}

#define EMIT_SIDT(_disp,_base) {                                              \
   EMIT_BYTE(0xf); EMIT_BYTE(0x1);  EMIT_MODRM_MEM(1,_base,_disp);            \
}

#define EMIT_STR(_disp,_base) {                                               \
   EMIT_BYTE(0xf); EMIT_BYTE(0x0); EMIT_MODRM_MEM(1,_base,_disp);             \
}

#define EMIT_STR_REG(_reg) {                                                  \
   EMIT_BYTE(0xf); EMIT_BYTE(0x0); EMIT_MODRM(1,3,_reg);                      \
}

#define EMIT_LGDT(_disp,_base) {                                              \
   EMIT_BYTE(0xf); EMIT_BYTE(0x1);  EMIT_MODRM_MEM(2,_base,_disp)             \
}

#define EMIT_LLDT(_disp,_base) {                                              \
   EMIT_BYTE(0xf); EMIT_BYTE(0x0);  EMIT_MODRM_MEM(2,_base,_disp)             \
}

#define EMIT_LLDT_REG(_reg) {                                                 \
   EMIT_BYTE(0xf); EMIT_BYTE(0x0);  EMIT_MODRM(3,2,_reg)                      \
}

#define EMIT_SLDT_REG(_reg) {                                                 \
   EMIT_BYTE(0xf); EMIT_BYTE(0x0);  EMIT_MODRM(3,0,_reg)                      \
}

#define EMIT16_LGDT_ABS(_addr) {                                              \
   EMIT_BYTE(0xf); EMIT_BYTE(0x1);  EMIT_BYTE(0x16); EMIT_WORD16(_addr);      \
}

#define EMIT_LIDT(_disp,_base) {                                              \
   EMIT_BYTE(0xf); EMIT_BYTE(0x1);  EMIT_MODRM_MEM(3,_base,_disp)             \
}


#define EMIT_LTR(_disp,_base) {                                               \
   EMIT_BYTE(0xf); EMIT_BYTE(0x0); EMIT_MODRM_MEM(3,_base,_disp);             \
}

#define EMIT_MOVE_TO_CR(_reg,_cr) {                                           \
   EMIT_BYTE(0xf); EMIT_BYTE(0x22); EMIT_MODRM(3,_cr,_reg);                   \
}

#define EMIT_MOVE_FROM_CR(_reg,_cr) {                                         \
   EMIT_BYTE(0xf); EMIT_BYTE(0x20); EMIT_MODRM(3,_cr,_reg);                   \
}

#define EMIT_MOVE_TO_DR(_reg,_dr) {                                           \
   EMIT_BYTE(0xf); EMIT_BYTE(0x23); EMIT_MODRM(3,_dr,_reg);                   \
}

#define EMIT_MOVE_FROM_DR(_reg,_dr) {                                         \
   EMIT_BYTE(0xf); EMIT_BYTE(0x21); EMIT_MODRM(3,_dr,_reg);                   \
}

#define EMIT_CLTS { EMIT_BYTE(0x0f); EMIT_BYTE(0x06); }

#define EMIT_ENABLE_INTERRUPTS()  EMIT_BYTE(0xfb)
#define EMIT_DISABLE_INTERRUPTS() EMIT_BYTE(0xfa)

#define EMIT_IN_AL_DX             EMIT_BYTE(MNEM_IN_AL_DX);            

#define EMIT_IN_AX_DX(_oSize) {                                               \
   if ((_oSize) == SIZE_32BIT) {                                              \
      EMIT_OPERAND_OVERRIDE;                                                  \
   }                                                                          \
   EMIT_BYTE(MNEM_IN_EAX_DX);                                                 \
}

#define EMIT_IN_EAX_DX(_oSize) {                                              \
   if ((_oSize) == SIZE_16BIT) {                                              \
      EMIT_OPERAND_OVERRIDE;                                                  \
   }                                                                          \
   EMIT_BYTE(MNEM_IN_EAX_DX);                                                 \
}

#define EMIT_IN_DX(_oSize, _opSize) {                                         \
   if (_opSize == SIZE_8BIT) {                                                \
      EMIT_IN_AL_DX;                                                          \
   } else if (_opSize == SIZE_16BIT) {                                        \
      EMIT_IN_AX_DX(_oSize);                                                  \
   } else {                                                                   \
      EMIT_IN_EAX_DX(_oSize);                                                 \
   }                                                                          \
}

#define EMIT_OUT_AL_DX            EMIT_BYTE(MNEM_OUT_AL_DX);

#define EMIT_OUT_AX_DX(_oSize) {                                              \
   if ((_oSize) == SIZE_32BIT) {                                              \
      EMIT_OPERAND_OVERRIDE;                                                  \
   }                                                                          \
   EMIT_BYTE(MNEM_OUT_EAX_DX);                                                \
}

#define EMIT_OUT_EAX_DX(_oSize) {                                             \
   if ((_oSize) == SIZE_16BIT) {                                              \
      EMIT_OPERAND_OVERRIDE;                                                  \
   }                                                                          \
   EMIT_BYTE(MNEM_OUT_EAX_DX);                                                \
}

#define EMIT_OUT_DX(_oSize, _opSize) {                                        \
   if (_opSize == SIZE_8BIT) {                                                \
      EMIT_OUT_AL_DX;                                                         \
   } else if (_opSize == SIZE_16BIT) {                                        \
      EMIT_OUT_AX_DX(_oSize);                                                 \
   } else {                                                                   \
      EMIT_OUT_EAX_DX(_oSize);                                                \
   }                                                                          \
}

#define EMIT_IN_AL(_addr) {                                                   \
   EMIT_BYTE(MNEM_IN_AL_IMM);                                                 \
   EMIT_BYTE((_addr));                                                        \
}

#define EMIT_IN_AX(_oSize, _addr) {                                           \
   if ((_oSize) == SIZE_32BIT) {                                              \
      EMIT_OPERAND_OVERRIDE;                                                  \
   }                                                                          \
   EMIT_BYTE(MNEM_IN_EAX_IMM);                                                \
   EMIT_BYTE((_addr));                                                        \
}

#define EMIT_IN_EAX(_oSize, _addr) {                                          \
   if ((_oSize) == SIZE_16BIT) {                                              \
       EMIT_OPERAND_OVERRIDE;                                                 \
   }                                                                          \
   EMIT_BYTE(MNEM_IN_EAX_IMM);                                                \
   EMIT_BYTE((_addr));                                                        \
}

#define EMIT_IN(_oSize, _addr, _opSize) {                                     \
   if ((_opSize) == SIZE_8BIT) {                                              \
      EMIT_IN_AL(_addr);                                                      \
   } else if ((_opSize) == SIZE_16BIT) {                                      \
      EMIT_IN_AX(_oSize,_addr);                                               \
   } else {                                                                   \
      EMIT_IN_EAX(_oSize,_addr);                                              \
   }                                                                          \
}

#define EMIT_OUT_AL(_addr) {                                                  \
   EMIT_BYTE(MNEM_OUT_AL_IMM);                                                \
   EMIT_BYTE((_addr));                                                        \
}

#define EMIT_OUT_AX(_oSize, _addr) {                                          \
   if ((_oSize) == SIZE_32BIT) {                                              \
      EMIT_OPERAND_OVERRIDE;                                                  \
   }                                                                          \
   EMIT_BYTE(MNEM_OUT_EAX_IMM);                                               \
   EMIT_BYTE((_addr));                                                        \
}

#define EMIT_OUT_EAX(_oSize, _addr) {                                         \
   if ((_oSize) == SIZE_16BIT) {                                              \
      EMIT_OPERAND_OVERRIDE;                                                  \
   }                                                                          \
   EMIT_BYTE(MNEM_OUT_EAX_IMM);                                               \
   EMIT_BYTE((_addr));                                                        \
}

#define EMIT_OUT(_oSize, _addr, _opSize) {                                    \
   if (_opSize == SIZE_8BIT) {                                                \
      EMIT_OUT_AL(_addr);                                                     \
   } else if (_opSize == SIZE_16BIT) {                                        \
      EMIT_OUT_AX(_oSize, _addr);                                             \
   } else {                                                                   \
      EMIT_OUT_EAX(_oSize, _addr);                                            \
   }                                                                          \
}

#define EMIT16_SETO_ABS(_addr) {                                              \
   EMIT_WORD16(MNEM_SETO);                                                    \
   EMIT16_MODRM_FIXEDMEM(0, _addr)                                            \
}

#define EMIT32_SETO_ABS(_addr) {                                              \
   EMIT_WORD16(MNEM_SETO);                                                    \
   EMIT32_MODRM_FIXEDMEM(0, _addr)                                            \
}

#define EMIT_SETO_ABS(_codeSize, _addr) {                                     \
   if ((_codeSize) == SIZE_16BIT) {                                           \
      EMIT16_SETO_ABS(_addr);                                                 \
   } else {                                                                   \
      EMIT32_SETO_ABS(_addr);                                                 \
   }                                                                          \
}

#define EMIT_SETO_REG8(_reg) {                                                \
   EMIT_WORD16(MNEM_SETO);                                                    \
   EMIT_MODRM_REG(_reg, 0);                                                   \
}

#define EMIT_INC_IMM8_ABS(_cSz, _addr) {                                      \
   ASSERT(((uint32)_addr & ~0xffff) == 0);                                    \
   EMIT_BYTE(0xfe);                                                           \
   if (_cSz == SIZE_32BIT) {                                                  \
      EMIT32_MODRM_FIXEDMEM(0, _addr);                                        \
   } else {                                                                   \
      EMIT16_MODRM_FIXEDMEM(0, _addr);                                        \
   }                                                                          \
}

#define EMIT_DEC_IMM8_ABS(_cSz, _addr) {                                      \
   ASSERT(((uint32)_addr & ~0xffff) == 0);                                    \
   EMIT_BYTE(0xfe);                                                           \
   if (_cSz == SIZE_32BIT) {                                                  \
      EMIT32_MODRM_FIXEDMEM(1, _addr);                                        \
   } else {                                                                   \
      EMIT16_MODRM_FIXEDMEM(1, _addr);                                        \
   }                                                                          \
}

/* 
 * Conditional moves.
 */
#define COND_CMOVA   0x47
#define COND_CMOVAE  0x43
#define COND_CMOVB   0x42
#define COND_CMOVBE  0x46
#define COND_CMOVC   0x42
#define COND_CMOVE   0x44
#define COND_CMOVG   0x4f
#define COND_CMOVGE  0x4d
#define COND_CMOVL   0x4c
#define COND_CMOVLE  0x4e
#define COND_CMOVNA  0x46
#define COND_CMOVNAE 0x42
#define COND_CMOVNB  0x43
#define COND_CMOVNBE 0x47
#define COND_CMOVNC  0x43
#define COND_CMOVNE  0x45
#define COND_CMOVNG  0x4e
#define COND_CMOVNGE 0x4c
#define COND_CMOVNL  0x4d
#define COND_CMOVNLE 0x4f
#define COND_CMOVNO  0x41
#define COND_CMOVNP  0x4b
#define COND_CMOVNS  0x49
#define COND_CMOVNZ  0x45
#define COND_CMOVO   0x40
#define COND_CMOVP   0x4a
#define COND_CMOVPE  0x4a
#define COND_CMOVPO  0x4b
#define COND_CMOVS   0x48
#define COND_CMOVZ   0x44

#define EMIT_CMOVE_REG_REG(_cond, _src, _dst) {                                \
   EMIT_BYTE(0x0f);                                                            \
   EMIT_BYTE(_cond);                                                           \
   EMIT_MODRM_REG(_dst, _src);                                                 \
}

#define EMIT32_CMOVE_MEM_REG(_cond, _reg, _disp) {                             \
   EMIT_BYTE(0x0f);                                                            \
   EMIT_BYTE(_cond);                                                           \
   EMIT_MODRM(0, _reg, 5);                                                     \
   EMIT_WORD(_disp);                                                           \
}

#endif /* _VMK_EMIT_H_ */
