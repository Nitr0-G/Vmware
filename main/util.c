/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * util.c - vmkernel utility functions
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "vm_asm.h"
#include "x86.h"
#include "vmkernel.h"
#include "splock.h"
#include "prda.h"
#include "world.h"
#include "mod_loader.h"
#include "kseg.h"
#include "util.h"
#include "pagetable.h"
#include "timer.h"
#include "rpc.h"
#include "memalloc.h"
#include "net.h"
#include "kvmap.h"
#include "helper.h"
#include "host.h"
#include "user.h"

#define LOGLEVEL_MODULE Util
#define LOGLEVEL_MODULE_LEN 4
#include "log.h"

static uint32 utilRand;
static int64 timeStampOffset;

/*
 *----------------------------------------------------------------------
 *
 * Util_Init--
 *
 *      Initialize this module
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      utilRand is initialized.
 *
 *----------------------------------------------------------------------
 */
void
Util_Init(void)
{
   utilRand = Util_RandSeed();
}

/*
 *----------------------------------------------------------------------
 *
 * Util_FastRand --
 *
 *	Generates the next random number in the pseudo-random sequence 
 *	defined by the multiplicative linear congruential generator 
 *	S' = 16807 * S mod (2^31 - 1).
 *	This is the ACM "minimal standard random number generator".  
 *	Based on method described by D.G. Carta in CACM, January 1990. 
 *	Usage: provide previous random number as the seed for next one.
 *
 * Precondition:
 *      0 < seed && seed < UTIL_FASTRAND_SEED_MAX
 *
 * Results:
 *	A random number.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
uint32
Util_FastRand(uint32 seed)
{
   uint64 product    = 33614 * (uint64)seed;
   uint32 product_lo = (uint32)(product & 0xffffffff) >> 1;
   uint32 product_hi = product >> 32;
   int32  test       = product_lo + product_hi;
   ASSERT(0 < seed && seed < UTIL_FASTRAND_SEED_MAX);
   return (test > 0) ? test : (test & UTIL_FASTRAND_SEED_MAX) + 1;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Util_RandSeed --
 *
 *     Returns a reasonable seed for use with Util_FastRand
 *
 * Results:
 *     Returns a reasonable seed for use with Util_FastRand
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
uint32
Util_RandSeed(void)
{
   return ((RDTSC() * (MY_PCPU + 1)) % (UTIL_FASTRAND_SEED_MAX - 1)) + 1;
}

/*
 *----------------------------------------------------------------------
 *
 * Util_Udelay --
 *
 *      Delay for uSecs microseconds.
 *
 * Results: 
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
Util_Udelay(uint32 uSecs)
{
   TSCCycles target = RDTSC() + Timer_USToTSC(uSecs);

   while (1) {
      PAUSE();
      if (RDTSC() >= target) {
         break;
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * Util_VerifyVPN --
 *
 *      Verify that the given virtual page is accessible for reading
 *      or writing (if write == TRUE).  This can verify any VPN.  
 *
 * Results: 
 *      TRUE if the address is accessible, FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Bool
Util_VerifyVPN(VPN vpn, Bool write)
{
   Bool valid = FALSE;
   Bool writable = FALSE;
   KSEG_Pair *pairDir, *pairTable;
   VMK_PTE *pageTable;
   VMK_PDE *pageDir;
   VA vaddr = VPN_2_VA(vpn);
   LA laddr = VMK_VA_2_LA(vaddr);
   MA cr3;

   if (vaddr > VMK_VA_END) {
      return FALSE;
   }

   GET_CR3(cr3);
   pageDir = PT_GetPageDir(cr3, laddr, &pairDir);
   if (pageDir) {
      if (PTE_PRESENT(pageDir[ADDR_PDE_BITS(laddr)])) {
         if (pageDir[ADDR_PDE_BITS(laddr)] & PTE_PS) {
            // all large pages are OK (we don't map uncached stuff in large pages)
            ASSERT(VMK_IsValidMPN(VMK_PTE_2_MPN(pageDir[ADDR_PDE_BITS(laddr)])));
            valid = TRUE;
            writable = PTE_WRITEABLE(pageDir[ADDR_PDE_BITS(laddr)]);
         } else {
            pageTable = PT_GetPageTableInDir(pageDir, laddr, &pairTable);
            if (pageTable) {
               if (PTE_PRESENT(pageTable[ADDR_PTE_BITS(laddr)])) {
                  MPN mpn = VMK_PTE_2_MPN(pageTable[ADDR_PTE_BITS(laddr)]);
                  if (VMK_IsValidMPN(mpn)) {
                     valid = TRUE;
                     writable = PTE_WRITEABLE(pageTable[ADDR_PTE_BITS(laddr)]);
                  }
               }
               PT_ReleasePageTable(pageTable, pairTable);
            }
         }
      }
      PT_ReleasePageDir(pageDir, pairDir);
   }

   if (valid && (!write || writable)) {
      return TRUE;
   } else {
      return FALSE;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * UtilGetStackVal --
 *
 *      Return the value at the given address.  Address is checked to
 *      see if it is a valid *stack* address (for any potential
 *      vmkernel stack). Return 0 if the address isn't in the stack
 *      addr range.
 *
 *	Can only inspect the host world's stack from the host world.
 *
 * Results: 
 *      The value at *addr, 0 if addr isn't accessible.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static uint32 
UtilGetStackVal(VA addr)
{
   if (CpuSched_IsHostWorld()) {
      if (addr < VMK_HOST_STACK_BASE || addr >= VMK_HOST_STACK_TOP - 8) {
//	 Warning("Bad stack address of 0x%x for host world", addr);
	 return 0;
      }   
   } else {
      /* Mis-aligned addresses are not real stack references. */
      if ((addr & 0x3) != 0) {
         return 0;
      }

      /* Eliminate addresses that are outside the vmkernel stack region. */
      if ((VA_2_VPN(addr) < VMK_FIRST_STACK_VPN)
          || (VA_2_VPN(addr) > VMK_LAST_STACK_VPN)) {
         return 0;
      }

      /* If the addr has no MPN, don't dereference it. */
      if (World_GetStackMPN(addr) == INVALID_MPN) {
         return 0;
      }
   }

   return *(uint32 *)addr;
}

/*
 *----------------------------------------------------------------------
 *
 * Util_Backtrace --
 *
 *      Backtrace the stack and call outputFunc to print it out.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
Util_Backtrace(Reg32 pc, Reg32 ebp, Util_OutputFunc outputFunc, Bool verbose)
{
   int maxDepth;
   int maxArgs;   
   int length = 0;
   char btStr[100];
   char argsStr[32];

   if (verbose) {
      outputFunc("Backtrace for CPU #%d, ebp=0x%x, worldID=%d\n", 
                 PRDA_GetPCPUNumSafe(), ebp,
	         PRDA_GetRunningWorldIDSafe());
   }

   if (verbose) {
      maxDepth = 20;
      maxArgs = 5;
   } else {
      maxDepth = 10;
      maxArgs = 3;
   }

   for (length = 0; length < maxDepth; length++) {
      int i;   
      uint32 offset;
      char *name;
      if (Mod_LookupPC(pc, &name, &offset)) {
         snprintf(btStr, sizeof btStr, "%#x:[0x%x]%s+0x%x(", ebp,  pc, name, offset);
      } else {
	 snprintf(btStr, sizeof btStr, "%#x:[0x%x](", ebp,  pc);
      }

      for (i = 0; i < maxArgs; i++) {
	 if (i > 0) {
	    snprintf(argsStr, sizeof argsStr, ", 0x%x", UtilGetStackVal(ebp + 8 + i * 4));
	 } else {
	    snprintf(argsStr, sizeof argsStr, "0x%x", UtilGetStackVal(ebp + 8));
	 }
         if ((strlen(btStr) + strlen(argsStr)) < sizeof btStr) {
            strcat(btStr, argsStr);
         } else {
            break;
         }
      }
      outputFunc("%s)\n", btStr);

      pc = UtilGetStackVal(ebp + 4);      
      ebp = UtilGetStackVal(ebp);
      if (ebp == 0) {
	 break;
      }	 	 
   }

}

/*
 *----------------------------------------------------------------------
 *
 * Util_SetTimeStampOffset --
 *
 *      Sets timeStampOffset to the value passed.  Used only by the 
 *      migration code for primitive inter-vmkernel time synchronization.
 *
 * Results: 
 *      None.
 *
 * Side effects:
 *      All timestamps generade by Util_FormatTimestamp will be offset
 *      by a constant factor.
 *
 *----------------------------------------------------------------------
 */
void
Util_SetTimeStampOffset(uint64 offset)
{
   timeStampOffset = offset;
}


/*
 *----------------------------------------------------------------------
 *
 * Util_FormatTimestamp --
 *
 *      Formats current system uptime, and writes formatted string
 *	into "buf", up to "bufLen" characters.
 *
 * Results: 
 *      Writes formatted timestamp value info "buf".
 *	Returns number of characters written.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
int
Util_FormatTimestamp(char *buf, int bufLen)
{
   int len;

   if (!Timer_Initialized()) {
      static TSCCycles firstTS = 0xffffffffffffffffULL;
      /* 
       * Haven't yet initialized timer subsystem,
       * so just print out raw TSC value.
       */

      TSCCycles now = RDTSC();
      if (now < firstTS) {
         firstTS = now;
      }
      len = snprintf(buf, bufLen, "TSC: %"FMT64"u", now - firstTS);
   } else {
      uint32 msec, sec, min, hrs, days;
      uint64 nowMS;

      nowMS = Timer_SysUptime();
      msec = nowMS % 1000;
      sec  = nowMS / 1000;

      // convert into days, hours, minutes, seconds
      days = sec / 86400;
      sec  = sec % 86400;
      hrs  = sec / 3600;
      sec  = sec % 3600;
      min  = sec / 60;
      sec  = sec % 60;

      // format timestamp
      len = snprintf(buf, bufLen, "%u:%02u:%02u:%02u.%03u",
                     days, hrs, min, sec, msec);
   }
   return(MIN(len, bufLen));
}

/*
 *----------------------------------------------------------------------
 *
 * Util_CopySGData --
 *
 *      Copy from/to a scatter-gather array from/to a data buffer.  
 *	Only virtual and machine address scatter-gather arrays are
 *	supported.
 *
 * Results: 
 *      TRUE if the copy succeeded, FALSE if it failed because something
 *	couldn't be mapped or the scatter-gather type isn't supported.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Bool
Util_CopySGData(void *data, 		/* The data buffer to copy from/to. */
	        SG_Array *sgArr, 	/* The scatter-gather array to copy from/to. */
		Util_CopySGDir dir,	/* The direction to copy the data. */
	        int index, 		/* Which element in the scatter-gather array
					 * to start copying from/to. */
		int offset, 		/* The offset in the element indentified by
					 * index to start copying at. */
		int length)		/* The total number of bytes to copy. */
{
   int toCopy;
   int bytesLeft = length;

   for (; index < sgArr->length && bytesLeft > 0; index++) {
      if (offset >= sgArr->sg[index].length) {
	 Warning("Bad scatter-gather array offset");
	 return FALSE;
      }
      toCopy = sgArr->sg[index].length - offset;
      if (toCopy > bytesLeft) {
	 toCopy = bytesLeft;
      }
      bytesLeft -= toCopy;      
      if (sgArr->addrType == SG_VIRT_ADDR) {	 
	 if (dir == UTIL_COPY_FROM_SG) {
	    memcpy(data, (void *)((VA)(sgArr->sg[index].addr + offset)), toCopy);
	 } else {
	    memcpy((void *)((VA)(sgArr->sg[index].addr + offset)), data, toCopy);
	 }
      } else if (sgArr->addrType == SG_MACH_ADDR) {
	 while (toCopy > 0) {
	    int ksegToCopy;
	    KSEG_Pair *pair;
	    void *sgData;
	    if (toCopy > PAGE_SIZE) {
	       ksegToCopy = PAGE_SIZE;
	    } else {
	       ksegToCopy = toCopy;
	    }
	    sgData = Kseg_GetPtrFromMA(sgArr->sg[index].addr + offset, 
				       ksegToCopy, &pair);
	    if (sgData == NULL) {
	       return FALSE;
	    }
	    if (dir == UTIL_COPY_FROM_SG) {
	       memcpy(data, sgData, ksegToCopy);
	    } else {
	       memcpy(sgData, data, ksegToCopy);
	    }
	    Kseg_ReleasePtr(pair);
	    data += ksegToCopy;
	    toCopy -= ksegToCopy;
	    offset += ksegToCopy;
	 }
      } else {
	 return FALSE;
      }      
      offset = 0;
   }

   return TRUE;
}

/*
 *----------------------------------------------------------------------
 *
 * UtilDoHostUserCopy --
 *
 *      Copy to/from a console OS user space buffer. This is a blocking call
 *      which uses the RPC mechanism to get the vmnixmod module to copy on its
 *      behalf.
 *
 * Results: 
 *      TRUE on success, FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Bool
UtilDoHostUserCopy(void *dst,
                   const void *src,
                   unsigned long copyLen,
                   VMnix_CopyServOp direction)
{
   VMK_ReturnStatus status;
   RPC_Token token;
   RPC_Connection cnxID;
   VMnix_CopyServArgs args;
   VMnix_CopyServResult result;
   uint32 resLen = sizeof(VMnix_CopyServResult);
   Bool rc = TRUE;
   Helper_RequestHandle rh, ih;

   /* 
    * This is a synchronous/blocking call which should only be handled in a
    * helper world. i.e., original function must have been a helper request.
    * Because copying depends on the COS process doing the ioctl, make sure the
    * ioctl context is correct.
    */

   ASSERT(World_IsHELPERWorld(CpuSched_GetCurrentWorld()));
   rh = Helper_GetActiveRequestHandle(); 
   ih = Host_GetActiveIoctlHandle();
   if (rh != ih) {
      LOG(0, "Not current ioctl (rh=%d ih=%d).", rh, ih);
      return FALSE;
   }

   // Connect to copy service RPC channel on vmnix. 
   status = RPC_Connect(VMNIX_COPYSERV_NAME, &cnxID);
   if (status != VMK_OK) {
      Warning ("RPC_Connect failed: %d.", status); 
      return FALSE;
   }
   
   // Send parameters.
   args.src = src;
   args.dst = dst;
   args.len = copyLen;
   status = RPC_Send(cnxID, direction, RPC_REPLY_EXPECTED,
                     (char *) &args, sizeof(args), UTIL_VMKERNEL_BUFFER,
                     &token);
   if (status != VMK_OK) {
      Warning ("RPC_Send failed: %d.", status); 
      rc = FALSE;
      goto done;
   }

   // Wait for reply.
   status = RPC_GetReply(cnxID, token, RPC_CAN_BLOCK, (char *) &result, &resLen,
                         UTIL_VMKERNEL_BUFFER, INVALID_WORLD_ID);
   if (status != VMK_OK) {
      Warning ("RPC_GetReply failed: %d.", status); 
      rc = FALSE;
      goto done;
   }
   ASSERT(resLen == sizeof(VMnix_CopyServResult));
   if (!result.success) {
      Warning ("copy on host failed: src=%p dst=%p len=%lu dir=%d.", src, dst, copyLen, direction); 
      rc = FALSE;
   }

done:
   RPC_Disconnect(cnxID);
   return rc;
}

/*
 *----------------------------------------------------------------------
 *
 * Util_Copy {To, From} LinuxUser --
 *
 *      Wrappers for the console OS userspace copy function.  The vmkernel
 *      src/dst can be on the world's stack, which is not accessible from the 
 *      host, so use an intermediate buffer on the vmkernel heap for vmk src/dst.
 *
 * Results: 
 *      TRUE/FALSE for success/failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Bool
Util_CopyToLinuxUser(void *hostUserBuf,
                     const void *vmkBuf,
                     unsigned long len)
{
   uint8 *tmpBuf;
   Bool ok;

   tmpBuf = Mem_Alloc(len);
   if (tmpBuf == NULL) {
      Warning("No memory?"); 
      return FALSE;
   }
   memcpy(tmpBuf, vmkBuf, len);
   ok = UtilDoHostUserCopy(hostUserBuf, tmpBuf, len, VMNIX_COPY_TO_USER);    
   if (ok) {
      memcpy((void *)vmkBuf, tmpBuf, len);
   } 
   Mem_Free(tmpBuf);
   return ok;
}

Bool
Util_CopyFromLinuxUser(void *vmkBuf,
                       const void *hostUserBuf,
                       unsigned long len)
{
   uint8 *tmpBuf;
   Bool ok;

   tmpBuf = Mem_Alloc(len);
   if (tmpBuf == NULL) {
      Warning("No memory?"); 
      return FALSE;
   }
   memcpy(tmpBuf, vmkBuf, len);
   ok = UtilDoHostUserCopy(tmpBuf, hostUserBuf, len, VMNIX_COPY_FROM_USER);    
   if (ok) {
      memcpy(vmkBuf, tmpBuf, len);
   }
   Mem_Free(tmpBuf);
   return  ok;
}

/*
 *----------------------------------------------------------------------
 *
 * Util_CreateVMKFrame --
 *
 *      Create a fake vmk excFrame from a gate/eip/ebp.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      *fullFrame is modified. 
 *
 *----------------------------------------------------------------------
 */

void
Util_CreateVMKFrame(uint32 gate,
                    Reg32 eip,
                    Reg32 ebp,
                    struct VMKFullExcFrame *fullFrame)
{
   memset(fullFrame, 0xFF, sizeof(*fullFrame));
   fullFrame->frame.u.in.gateNum = gate;
   fullFrame->frame.eip = eip;
   fullFrame->regs.ebp = ebp;   
}

/*
 *----------------------------------------------------------------------
 *
 * Util_TaskToVMKFrame --
 *
 *      Translate from a task gate to a vmkernel exception frame.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      *fullFrame is modified. 
 *
 *----------------------------------------------------------------------
 */
void
Util_TaskToVMKFrame(uint32 gate, Task *task, VMKFullExcFrame *fullFrame)
{
   memset(fullFrame, 0, sizeof(*fullFrame));
   fullFrame->frame.u.in.gateNum = gate;
   fullFrame->frame.eip = task->eip;
   fullFrame->frame.cs = task->cs;
   fullFrame->frame.eflags = task->eflags;
   fullFrame->regs.es = task->es;
   fullFrame->regs.ds = task->ds;
   fullFrame->regs.fs = task->fs;
   fullFrame->regs.gs = task->gs;
   fullFrame->regs.eax = task->eax;
   fullFrame->regs.ebx = task->ebx;
   fullFrame->regs.ecx = task->ecx;
   fullFrame->regs.edx = task->edx;
   fullFrame->regs.ebp = task->ebp;
   fullFrame->regs.esi = task->esi;
   fullFrame->regs.edi = task->edi;
}

/*
 *-----------------------------------------------------------------------------
 *
 * Util_CreateUUID --
 *   Create a UUID based on a MAC address, current COS time, TSC,
 *   and a random value.
 *
 * Results:
 *   A UUID is returned to the caller in 'uuid'.
 *
 *-----------------------------------------------------------------------------
 */
void
Util_CreateUUID(UUID *uuid)
{
   VMK_ReturnStatus status;
   static uint32 prevCOSTime;
   static uint32 prevTSC;
   uint32 curCOSTime;
   uint32 curTSC;

   ASSERT(uuid != NULL);
   
   status = Net_GetMACAddrForUUID(uuid->macAddr);
   if (status != VMK_OK) {
      /* The error case shouldn't arise on practical ESX servers.
       * But if it does, fill up eaddr with some junk
       */
      *((uint32 *)uuid->macAddr) = Util_FastRand(hostWorld->worldID);
   }

   
   do {
      curCOSTime = consoleOSTime;
      curTSC = (uint32)RDTSC();
   } while (curTSC == prevTSC);      
   
   if (curCOSTime < prevCOSTime) {
      LOG(0, "COS time moved back");
   }
   prevCOSTime = curCOSTime;
   prevTSC = curTSC;

   /* Construct a uuid with the information we've gathered
    * plus a few constants. */
   uuid->timeLo = curCOSTime;
   uuid->timeHi = curTSC;
   uuid->rand = (uint16)utilRand;
   utilRand = Util_FastRand(utilRand);
   LOG(0, "Created UUID %08x-%08x-%04x-%02x%02x%02x%02x%02x%02x",
       uuid->timeLo, uuid->timeHi, uuid->rand, uuid->macAddr[0],
       uuid->macAddr[1], uuid->macAddr[2], uuid->macAddr[3],
       uuid->macAddr[4], uuid->macAddr[5]);
}


/*
 *----------------------------------------------------------------------
 *
 * Util_CopyMA --
 *
 *      Copies data from srcMA to destMA of size length (at most page size),
 *      but the regions don't have to be aligned to page boundary
 *
 * Results: 
 *	TRUE if successful, FALSE otherwise
 *
 *----------------------------------------------------------------------
 */
Bool
Util_CopyMA(MA destMA, MA srcMA, uint32 length)
{
   void *dest, *src;
   KSEG_Pair *pairDest, *pairSrc;

   /* This function is used for PAE data copying, which copies at most a
    * page at a time because memmap allocates single pages */
   ASSERT(length <= PAGE_SIZE);

   // XXX might be better to use kvmap to avoid thrashing kseg cache with
   // pages that are unlikely to be used again.
   dest = Kseg_GetPtrFromMA(destMA, length, &pairDest);
   if (dest == NULL) {
      return FALSE;
   }
   src = Kseg_GetPtrFromMA(srcMA, length, &pairSrc);
   if (src == NULL) {
      Kseg_ReleasePtr(pairDest);
      return FALSE;
   }

   memcpy(dest, src, length);
   Kseg_ReleasePtr(pairDest);
   Kseg_ReleasePtr(pairSrc);
   return TRUE;
}

/*
 *----------------------------------------------------------------------
 *
 * Util_ZeroMPN --
 *
 *      Zero the given mpn.  This function will map it and call
 *      Util_ZeroPage.  
 *
 * Results: 
 *	VMK_OK if successful, VMK_NO_ADDRESS_SPACE if mapping fails
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Util_ZeroMPN(MPN mpn)
{
   void* kpageptr;

   ASSERT(mpn != INVALID_MPN);
   
   kpageptr = KVMap_MapMPN(mpn, TLB_LOCALONLY);
   if (! kpageptr) {
      return VMK_NO_ADDRESS_SPACE;
   }
   
   Util_ZeroPage(kpageptr);
   KVMap_FreePages(kpageptr);

   return VMK_OK;
}

/*
 *-------------------------------------------------------------------
 *
 * Util_Memset --
 *
 *      Do a memset() on the specified memory region in the current
 *      world. If working on MAs or PAs, map and unmap the underlying
 *      machine pages into the VMkernel address space.
 *
 * Results:
 *      Returns TRUE on success, FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *-------------------------------------------------------------------
 */
Bool
Util_Memset(SG_AddrType addrType, uint64 addr, int c, uint32 length)
{
   uint64 end = addr + length;
   VMK_ReturnStatus vmkStatus;

   if (addrType == SG_VIRT_ADDR) {
      VA startAddr = (VA)addr;
      memset((void *)startAddr, c, length);
      return TRUE;
   }

   while (addr < end) {
      void *vaddr;
      uint32 offset = (uint32)(addr & PAGE_MASK);
      uint32 len = PAGE_SIZE - offset;
      KSEG_Pair *pair;

      if (addrType == SG_PHYS_ADDR) {
	 World_Handle *world = MY_VMM_GROUP_LEADER;
	 ASSERT_HAS_INTERRUPTS();
	 vaddr = Kseg_GetPtrFromPA(world, (PA)addr, PAGE_SIZE, TRUE,
				   &pair, &vmkStatus);
      } else {
         ASSERT(addrType == SG_MACH_ADDR);
         vaddr = Kseg_GetPtrFromMA((MA)addr, PAGE_SIZE, &pair);
      }
      if (vaddr == NULL) {
	 Warning("Failed to map PPN/MPN");
	 return FALSE;
      }
      if (addr + len >= end) {
	 len = end - addr;
      }
      memset(vaddr, c, len);
      Kseg_ReleasePtr(pair);
      addr += len;
   }
   return TRUE;
}

/*
 *-------------------------------------------------------------------
 *
 * Util_Memcpy --
 *
 *      Do a memcpy() on the specified memory region in the current world. If
 *      working on MAs or PAs, map and unmap the underlying machine pages into
 *      the VMkernel address space. The input argument, length, should be the
 *      length of data in terms of destAddr units. If destAddr is a measure of
 *      MAs, then the length is number of MAs to be copied.
 *
 * Results:
 *      Returns TRUE on success, FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *-------------------------------------------------------------------
 */
Bool
Util_Memcpy(SG_AddrType destAddrType, uint64 destAddr, SG_AddrType srcAddrType, 
            uint64 srcAddr, uint32 length)
{
   uint64 end = destAddr + length;
   VMK_ReturnStatus vmkStatus;

   if (destAddrType == SG_VIRT_ADDR && srcAddrType == SG_VIRT_ADDR) {
      memcpy((void*) (VA) destAddr, (void*) (VA) srcAddr, length);
      return TRUE;
   }

   while (destAddr < end) {
      char *srcVAddr, *destVAddr;
      uint32 offset, len;
      KSEG_Pair *srcPair, *destPair;

      if (destAddrType == SG_PHYS_ADDR) {
	 World_Handle *world = MY_VMM_GROUP_LEADER;
	 if (world == NULL) {
	    Warning("Group leader couldn't be found");
	    return FALSE;
	 } else {
	    ASSERT_HAS_INTERRUPTS();
	    destVAddr = Kseg_GetPtrFromPA(world, (PA)destAddr, PAGE_SIZE, TRUE,
				      &destPair, &vmkStatus);
	 }
      } 
      else if (destAddrType == SG_MACH_ADDR){
         destVAddr = Kseg_GetPtrFromMA((MA)destAddr, PAGE_SIZE, &destPair);
      } else {
         ASSERT(destAddrType == SG_VIRT_ADDR);
         destVAddr = (char*) (VA) destAddr;
      }

      if (srcAddrType == SG_PHYS_ADDR) {
	 World_Handle *world = MY_VMM_GROUP_LEADER;
	 if (world == NULL) {
	    Warning("Group leader couldn't be found");
	    return FALSE;
	 } else {
	    ASSERT_HAS_INTERRUPTS();
	    srcVAddr = Kseg_GetPtrFromPA(world, (PA)srcAddr, PAGE_SIZE, TRUE,
				      &srcPair, &vmkStatus);
	 }
      }
      else if (srcAddrType == SG_MACH_ADDR) {
         srcVAddr = Kseg_GetPtrFromMA((MA) srcAddr, PAGE_SIZE, &srcPair);
      }  else {
         ASSERT(srcAddrType == SG_VIRT_ADDR);
         srcVAddr = (char*) (VA) srcAddr;
      }

      if (srcVAddr == NULL || destVAddr == NULL) {
	 Warning("Failed to map PPN/MPN or invalid destination or source address");
	 return FALSE;
      }

      offset = MAX((VA)(destAddr & PAGE_MASK), (VA)(srcAddr & PAGE_MASK));
      offset = destAddr & PAGE_MASK;
      len = PAGE_SIZE - offset;

      if (destAddr + len >= end) {
	 len = end - destAddr;
      }

      memcpy(destVAddr, srcVAddr, len);
      
      if(srcAddrType != SG_VIRT_ADDR)
         Kseg_ReleasePtr(srcPair);

      if(destAddrType != SG_VIRT_ADDR)
         Kseg_ReleasePtr(destPair);

      destAddr += len;
      srcAddr += len;
   }
   return TRUE;
}

/*
 *----------------------------------------------------------------------
 *
 * Util_CopyIn --
 *
 *      Smart copier function -- copies from vmkernel, a userworld, or
 *      the host world.
 *
 * Results:
 *      VMK_OK on success, otherwise on failure.
 *
 * Side effects:
 *	Modifies *dest.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Util_CopyIn(void *dest, const void *src, uint32 length,
            Util_BufferType bufType)
{
   VMK_ReturnStatus status = VMK_OK;

   switch(bufType) {
   case UTIL_VMKERNEL_BUFFER:
      memcpy(dest, src, length);
      break;
   case UTIL_USERWORLD_BUFFER:
      ASSERT(World_IsUSERWorld(MY_RUNNING_WORLD));
      status = User_CopyIn(dest, (UserVA)src, length);
      break;
   case UTIL_HOST_BUFFER:
      ASSERT(World_IsHOSTWorld(MY_RUNNING_WORLD));
      CopyFromHost(dest, src, length);
      break;
   default:
      NOT_IMPLEMENTED();
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Util_CopyOut --
 *
 *      Smart copier function -- copies to vmkernel, a userworld, or
 *      the host world.
 *
 * Results:
 *      VMK_OK on success, otherwise on failure.
 *
 * Side effects:
 *	Modifies *dest.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Util_CopyOut(void *dest, const void *src, uint32 length,
             Util_BufferType bufType)
{
   VMK_ReturnStatus status = VMK_OK;

   switch(bufType) {
   case UTIL_VMKERNEL_BUFFER:
      memcpy(dest, src, length);
      break;
   case UTIL_USERWORLD_BUFFER:
      ASSERT(World_IsUSERWorld(MY_RUNNING_WORLD));
      status = User_CopyOut((UserVA)dest, src, length);
      break;
   case UTIL_HOST_BUFFER:
      ASSERT(World_IsHOSTWorld(MY_RUNNING_WORLD));
      CopyToHost(dest, src, length);
      break;
   default:
      NOT_IMPLEMENTED();
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Util_GetCosVPNContents --
 *
 *      Copies the contents of vpn in the COS into outBuf.
 *
 *      Caller must supply cr3 for the console os.
 *
 * Results:
 *      FALSE is VA is unmapped or unsafe to read.
 *      TRUE otherwise
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Bool
Util_GetCosVPNContents(VPN vpn, MA cr3, char *outBuf)
{
   Bool valid = FALSE;
   MPN mpn = INVALID_MPN;
   KSEG_Pair *pairDir, *pairTable;
   VMK_PTE *pageTable;
   VMK_PDE *pageDir;
   LA laddr = VPN_2_VA(vpn) /*la = va in cos*/;

   pageDir = PT_GetPageDir(cr3, laddr, &pairDir);
   if (pageDir) {
      if (PTE_PRESENT(pageDir[ADDR_PDE_BITS(laddr)])) {
         if (pageDir[ADDR_PDE_BITS(laddr)] & PTE_PS) {
            MA baseMpn = VMK_PTE_2_MPN(pageDir[ADDR_PDE_BITS(laddr)]);
            uint32 offset = (laddr & (PDE_SIZE - 1)) >> PAGE_SHIFT;
            // Check for & ignore uncached pages.
            valid = !(pageDir[ADDR_PTE_BITS(laddr)] & PTE_PCD);
            mpn = baseMpn + offset;
         } else {
            pageTable = PT_GetPageTableInDir(pageDir, laddr, &pairTable);
            if (pageTable) {
               if (PTE_PRESENT(pageTable[ADDR_PTE_BITS(laddr)])) {
                  mpn = VMK_PTE_2_MPN(pageTable[ADDR_PTE_BITS(laddr)]);
                  // Check for & ignore uncached pages.
                  valid = !(pageTable[ADDR_PTE_BITS(laddr)] & PTE_PCD);
               }
               PT_ReleasePageTable(pageTable, pairTable);
            }
         }
      }
      PT_ReleasePageDir(pageDir, pairDir);
   }

   if (valid) {
      KSEG_Pair *pair;
      void *data = Kseg_MapMPN(mpn, &pair);
      ASSERT(data);
      memcpy(outBuf, data, PAGE_SIZE);
      Kseg_ReleasePtr(pair);
      LOG(5, "%#x mpn is %#x", (uint32)laddr, (uint32)mpn);
   }
   return valid;
}


/*
 *----------------------------------------------------------------------
 *
 * Util_CopyFromHost --
 *
 *      Slower version of CopyFromHost that can be run from any context.
 *
 *      Caller must supply cr3 for the console os.
 *
 * Results:
 *      TRUE on success, FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Bool
Util_CopyFromHost(void *dst, VA src, uint32 length, MA cr3)
{
   Bool valid = FALSE;
   char *buffer;

   if (src < VMNIX_KVA_START || src >= VMNIX_KVA_END) {
      return FALSE;
   }

   buffer = Mem_Alloc(PAGE_SIZE);
   if (!buffer) {
      return FALSE;
   }

   while(length > 0) {
      int nBytes;
      if (!(valid = Util_GetCosVPNContents(VA_2_VPN(src), cr3, buffer))) {
         break;
      }
      nBytes = MIN(PAGE_SIZE - ADDR_PGOFFSET_BITS(src), length);
      LOG(5, "dst: %#x, src: %#x, buf: %#x, buf+off: %#x, nBytes: %#x", (uint32)dst, (uint32)src, 
          (uint32)buffer, (uint32)(buffer + ADDR_PGOFFSET_BITS(src)), nBytes);
      memcpy(dst, buffer + ADDR_PGOFFSET_BITS(src), nBytes);
      src += nBytes;
      dst += nBytes;
      length -= nBytes;
   }

   Mem_Free(buffer);
   return valid;
}
