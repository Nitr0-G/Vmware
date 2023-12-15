/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * util.h --
 *
 *	Utilities.
 */

#ifndef _UTIL_H
#define _UTIL_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "vm_asm.h"
#include "vm_libc.h"
#include "util_dist.h"
#include "return_status.h"
#include "timer_dist.h"

struct UUID; //To avoid including util_ext.h

#define UTIL_FASTRAND_SEED_MAX (0x7fffffff)
typedef void (*Util_OutputFunc)(const char *fmt, ...);

extern Bool Util_CopyMA(MA dst, MA src, uint32 length);

extern void Util_Udelay(uint32 uSecs);
extern void Util_Init(void);
extern void Util_Backtrace(Reg32 pc, Reg32 ebp, Util_OutputFunc outputFunc,
                           Bool verbose);

extern int Util_FormatTimestamp(char *buf, int bufLen);
extern void Util_SetTimeStampOffset(uint64 then);
extern void Util_Init(void);
struct VMKFullExcFrame;

extern void Util_CreateVMKFrame(uint32 gate, Reg32 eip, Reg32 ebp,
                                struct VMKFullExcFrame *fullFrame);
extern void Util_TaskToVMKFrame(uint32 gate, Task *task, 
                                struct VMKFullExcFrame *fullFrame);
extern void Util_CreateUUID(struct UUID *uuid);
extern Bool Util_VerifyVPN(VPN vpn, Bool write);

typedef enum {
   UTIL_VMKERNEL_BUFFER,
   UTIL_USERWORLD_BUFFER,
   UTIL_HOST_BUFFER
} Util_BufferType;

extern VMK_ReturnStatus Util_CopyIn(void *dest, const void *src,
                                    uint32 length, Util_BufferType bufType);
extern VMK_ReturnStatus Util_CopyOut(void *dest, const void *src,
                                     uint32 length, Util_BufferType bufType);

extern VMK_ReturnStatus Util_ZeroMPN(MPN mpn);
extern Bool Util_Memset(SG_AddrType addrType, uint64 addr, int c, uint32 length);
extern Bool Util_Memcpy(SG_AddrType destAddrType, uint64 dest, 
                         SG_AddrType srcAddrType, uint64 src, uint32 length);

Bool Util_GetCosVPNContents(VPN vpn, MA cr3, char *outBuf);
Bool Util_CopyFromHost(void *dst, VA src, uint32 length, MA cr3);

static INLINE void
Util_ZeroPage(void* page) {
   memset(page, 0, PAGE_SIZE);
}

static INLINE Bool 
Util_IsZeroPage(void *data) {
   static const int count = PAGE_SIZE / sizeof(uint32);
   uint32 *data32 = (uint32 *) data;
   int i;

   for (i = 0; i < count; i++) {
      if (data32[i] != 0) {
         return(FALSE);
      }
   }

   return(TRUE);
}

static INLINE void
WriteLEDs(uint32 val)
{
   uint32 mask = 0;

   if (val & 0x1) {
      mask |= 0x1;
   }

   if (val & 0x2) {
      mask |= 0x4;
   }

   if (val & 0x4) {
      mask |= 0x2;
   }

   while (1) {
      val = INB(0x64);
      if (!(val & 0x2)) {
         break;
      }
   }
   
   OUTB(0x60, 0xed);

   while (1) {
      val = INB(0x64);
      if (!(val & 0x2)) {
         break;
      }
   }
   
   OUTB(0x60, mask);
}


/*
 *-----------------------------------------------------------------------------
 *
 * Util_BitPopCount --
 *
 *     Returns the number of bits set to 1 in the word "val"
 *
 *     Comes from www.aggregate.org/MAGIC, which specifies that this
 *     code "may be used for any purpose". Virtually identical version (but
 *     in assembly) appears in AMD reference manual.
 *
 * Results:
 *     Returns the number of bits set to 1 in the word "value"
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE uint8
Util_BitPopCount(uint32 val)
{
        val -= ((val >> 1) & 0x55555555);
        val = (((val >> 2) & 0x33333333) + (val & 0x33333333));
        val = (((val >> 4) + val) & 0x0f0f0f0f);
        val += (val >> 8);
        val += (val >> 16);
        return(val & 0x0000003f);
}

/*
 *---------------------------------------------------------------
 *
 * Util_RoundupToPowerOfTwo --
 * 
 *    Utility function to round the given number to nearest power
 *    of 2.
 *
 * Results:
 *    returns the number rounded up to the nearest power of 2
 *
 * Side Effects:
 *    none.
 *
 *---------------------------------------------------------------
 */
static INLINE uint32
Util_RoundupToPowerOfTwo(uint32 n)
{
   // subtract one for power-of-2 input
   n--;         
   // "smear" steps, repeat log2(nbits) times
   n |= n >> 1; 
   n |= n >> 2;
   n |= n >> 4;
   n |= n >> 8;
   n |= n >> 16;
   // add 1 to reach next power of 2 (or 0)
   n++;       
   return n;
}


/*
 *---------------------------------------------------------------
 *
 * Util_RounddownToPowerOfTwo --
 * 
 *    Utility function to round the given number down to nearest power
 *    of 2.
 *
 * Results:
 *    returns the number rounded down to the nearest power of 2
 *
 * Side Effects:
 *    none.
 *
 *---------------------------------------------------------------
 */
static INLINE uint32
Util_RounddownToPowerOfTwo(uint32 n)
{
   ASSERT(n > 0);
   return (1 << (fls(n) - 1));
}


/*
 *---------------------------------------------------------------
 *
 * Util_IsPowerOf2 --
 * 
 *    Utility function to determine if the given number is a 
 *    power of 2.
 *
 * Results:
 *    TRUE if number is power of 2,
 *    FALSE otherwise
 *
 * Side Effects:
 *    none.
 *
 *---------------------------------------------------------------
 */
static INLINE Bool
Util_IsPowerOf2(uint32 n)
{
   return ((n & (n - 1)) == 0);
}


#endif


