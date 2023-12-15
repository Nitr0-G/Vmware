/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * tlb.c -
 *
 *   This module manages the low level aspects of the virtual to
 *   machine mappings of the vmkernel. It is responsible for keeping
 *   the virtual to physical mapping all physical cpus consistent.
 */

#include "vm_types.h"
#include "x86.h"
#include "vm_asm.h"
#include "vmkernel.h"
#include "vmnix_if.h"
#include "apic.h"
#include "splock.h"
#include "prda.h"
#include "tlb.h"
#include "idt.h"
#include "sched.h"
#include "util.h"
#include "post.h"
#include "pagetable.h"
#include "kvmap_dist.h"

#define LOGLEVEL_MODULE TLB
#include "log.h"

/*
 *  This structure holds the static state of the TLB module.
 */

static struct TLBState {
   VMK_PTE         *master;
   MPN             firstPageDir; // all vmkernel pagetables share first page directory
   int             isSMP;
   SP_SpinLockIRQ  invLock;
   SP_Barrier      barrier;
} tlbState;

/*
 *  On the IM we send the vpn to update.
 *  If this is a tlb flush we send FLUSH_VPN.
 */
#define FLUSH_VPN		((VPN) -1)

/*
 *  Special constant to specify all pcpus.
 */
#define	TLB_PCPU_BROADCAST	INVALID_PCPU

// time to wait for print log message if no response to invalidation request
#if defined(VMX86_DEBUG) && defined(VMX86_DEVEL)
#define TLB_INVAL_WAIT_MS 20
#else
#define TLB_INVAL_WAIT_MS 100
#endif

#ifdef TLB_POST_FIXED
static Bool TLBPost(void *clientData, int id, SP_SpinLock *lock, SP_Barrier *barrier);
#endif

static void TLBDoInvalidate(VPN vpn, PCPU pcpuNum);
static void TLBInvalidateHandler(void *clientData, uint32 vector);

/*
 *----------------------------------------------------------------------
 *
 * TLB_EarlyInit --
 *
 *      Initialize the tlb module. This early initialization sets up this
 *      so we can manage the local tlb. 
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
TLB_EarlyInit(VMnix_Init *vmnixInit)  // IN: vmnix init block
{
   tlbState.master = (VMK_PTE*)vmnixInit->mapPDirAddr;
   tlbState.firstPageDir = INVALID_MPN;

   tlbState.isSMP = 0;
}

/*
 *----------------------------------------------------------------------
 *
 * TLB_LateInit --
 *
 *      Do late initialization of the TLB module.
 *
 * Results: 
 *      VMK_OK if successful, VMK_NO_RESOURCES otherwise
 *      
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
TLB_LateInit(void)
{
   SP_InitLockIRQ("tlbInvLock", &tlbState.invLock, SP_RANK_IRQ_BLOCK);
   SP_InitBarrier("tlbBarrier", numPCPUs, &tlbState.barrier);
   if (!IDT_VectorAddHandler(IDT_TLBINV_VECTOR, TLBInvalidateHandler,
			   NULL, FALSE, "tlb", 0)) {
      Log("Couldn't register tlb invalidate interrupt handler");
      return VMK_NO_RESOURCES;
   }
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TLB_LocalInit --
 *
 *      Initialize the tlb module. This late initialize sets up
 *      things on this particular physcial cpu.
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
TLB_LocalInit(void)
{
   int myPCPUNum = APIC_GetPCPU();

   SP_SpinBarrierNoYield(&tlbState.barrier);

   // do stuff here
   
   if (myPCPUNum == 0) {
      /* MASTER */
      if (numPCPUs > 1) {
	 tlbState.isSMP = 1;
      }
#ifdef TLB_POST_FIXED
      /*
       * The TLBPost currently unmaps the first page of the kvmap
       * region which causes BlueScreen posts to fail.
       */
      POST_Register("TLB", TLBPost, NULL);
#endif
   }

   SP_SpinBarrierNoYield(&tlbState.barrier);
}


/*
 *----------------------------------------------------------------------
 *
 * TLB_Validate --
 *
 *      Validate the virtual to machine mapping in the TLB. The following
 *      flags override the default behavior:
 *
 *      TLB_UNCACHED  - Normally the mapping is setup to be a cached mapping
 *                      this means access to the machine page can be mapped in
 *                      the processor cache. This flags will make the mapping
 *                      an uncached mapping.
 *
 *      TLB_LOCALONLY - Normally the mapping is updated on all physical cpus.
 *                      With this flag only the local cpu is updated. The mapping
 *                      on other physical cpus will be undefined until the next
 *                      global validate or invalidate.
 *
 * Results: 
 *      None.
 *      
 * Side effects:
 *      IMs may be sent.
 *
 *----------------------------------------------------------------------
 */

void
TLB_Validate(VPN       vpn,   // IN: the virtual page to validate
             MPN       mpn,   // IN: the machine page to map
             unsigned  flags) // IN: flags as described above
{
   VMK_PTE pte;

   ASSERT(vpn >= VMK_FIRST_MAP_VPN && vpn <= VMK_LAST_MAP_VPN);

   if (flags & TLB_UNCACHED) {
      pte = VMK_MAKE_PTE(mpn, 0, PTE_KERNEL | PTE_PWT | PTE_PCD);	
   } else {
      pte = VMK_MAKE_PTE(mpn, 0, PTE_KERNEL);	 
   }

   PT_SET(&tlbState.master[vpn - VMK_FIRST_MAP_VPN], pte);

   TLB_INVALIDATE_PAGE(VPN_2_VA(vpn));

   if (!(flags & TLB_LOCALONLY) && tlbState.isSMP) {
      TLBDoInvalidate(vpn, TLB_PCPU_BROADCAST);
   }
}

void
TLB_LocalValidateRange(VA vaddr, uint32 length, MA maddr)
{
   VA endVA;
   VMK_PTE *master;

   ASSERT(vaddr >= VMK_FIRST_MAP_ADDR);

   endVA = (vaddr + length - 1) & ~PAGE_MASK;
   vaddr &= ~PAGE_MASK;
   master = &tlbState.master[VA_2_VPN(vaddr) - VMK_FIRST_MAP_VPN];
   maddr &= ~PAGE_MASK;

   while (1) {
      PT_SET(master, maddr | PTE_KERNEL);
      TLB_INVALIDATE_PAGE(vaddr);

      vaddr += PAGE_SIZE;
      if (vaddr > endVA) {
	 break;
      }
      maddr += PAGE_SIZE;
      master++;
   }
}

void
TLB_LocalValidate(VPN       vpn,   // IN: the virtual page to validate
                  MPN       mpn)   // IN: the machine page to map
{
   VMK_PTE pte;

   ASSERT(vpn >= VMK_FIRST_MAP_VPN && vpn <= VMK_LAST_MAP_VPN);

   pte = VMK_MAKE_PTE(mpn, 0, PTE_KERNEL);	 
   PT_SET(&tlbState.master[vpn - VMK_FIRST_MAP_VPN], pte);
   TLB_INVALIDATE_PAGE(VPN_2_VA(vpn));
}

void
TLB_DumpPT(void)
{
   int i;
   for (i = 0; i < 1024; i++) {
      LOG(0, "PT[%d] = 0x%"FMTPT"x", i, tlbState.master[i]);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * TLB_Invalidate --
 *
 *      Invalidate the virtual to machine mapping in the TLB. The following
 *      flags override the default behavior:
 *
 *      TLB_LOCALONLY - Normally the mapping is invalidated on all physical cpus.
 *                      With this flag only the local cpu is updated. The mapping
 *                      on other physical cpus will be undefined until the next
 *                      global validate or invalidate.
 *
 * Results: 
 *      None.
 *      
 * Side effects:
 *      IMs may be sent.
 *
 *----------------------------------------------------------------------
 */

void
TLB_Invalidate(VPN       vpn,   // IN: the virtual page to invalidate
               unsigned  flags) // IN: flags as described above
{
   ASSERT(vpn >= VMK_FIRST_MAP_VPN && vpn <= VMK_LAST_MAP_VPN);

   PT_INVAL(&tlbState.master[vpn - VMK_FIRST_MAP_VPN]);

   TLB_INVALIDATE_PAGE(VPN_2_VA(vpn));

   if (!(flags & TLB_LOCALONLY) && tlbState.isSMP) {
      TLBDoInvalidate(vpn, TLB_PCPU_BROADCAST);
   }   
}

/*
 *----------------------------------------------------------------------
 *
 * TLB_Flush --
 *
 *      Invalidate the virtual to machine mapping in the TLB. The following
 *      flags override the default behavior:
 *
 *      TLB_LOCALONLY - Normally the tlb is flushed on all physical cpus.
 *                      With this flag only the local cpu is updated. The tlbs
 *                      on other physical cpus will be undefined until the next
 *                      global flush.
 *
 * Results: 
 *      None.
 *      
 * Side effects:
 *      IMs may be sent.
 *
 *----------------------------------------------------------------------
 */
void
TLB_Flush(unsigned flags) // IN: flags as described above
{
   TLB_FLUSH();

   if (!(flags & TLB_LOCALONLY) && (tlbState.isSMP)) {
      TLBDoInvalidate(FLUSH_VPN, TLB_PCPU_BROADCAST);
   }   
}

/*
 *----------------------------------------------------------------------
 *
 * TLB_FlushPCPU --
 *
 *      Invalidate all virtual to machine mappings in the TLB
 *	on processor "pcpuNum".  No values for "flags" change
 *	the default behavior.
 *
 * Results: 
 *      None.
 *      
 * Side effects:
 *      IMs may be sent to "pcpuNum".
 *
 *----------------------------------------------------------------------
 */
void
TLB_FlushPCPU(PCPU pcpuNum, UNUSED_PARAM(unsigned flags))
{
   if (pcpuNum == myPRDA.pcpuNum) {
      TLB_FLUSH();
   } else {
      TLBDoInvalidate(FLUSH_VPN, pcpuNum);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * TLB_SetVMKernelPDir
 *
 *      Record the page directory page that's shared across all vmkernel worlds
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
TLB_SetVMKernelPDir(MPN pageDir)
{
   ASSERT(tlbState.firstPageDir == INVALID_MPN);
   tlbState.firstPageDir = pageDir;
}

/*
 *----------------------------------------------------------------------
 *
 * TLB_GetVMKernelPDir
 *
 *      Return the page directory page shared by all vmkernel worlds
 *
 * Results: 
 *      MPN of the page directory page
 *      
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
MPN
TLB_GetVMKernelPDir(void)
{
   return tlbState.firstPageDir;
}

#ifdef TLB_POST_FIXED

/*
 *----------------------------------------------------------------------
 *
 * TLBPost
 *
 *      Perform a power on test of TLB invalidation
 *
 * Results:
 *      FALSE if error detected, TRUE otherwise
 *
 * Side effects:
 *      
 *
 *----------------------------------------------------------------------
 */

Bool
TLBPost(void *clientData, int id, SP_SpinLock *lock, SP_Barrier *barrier)
{
   // TODO: page in a range of pages

   // TODO: measure time to access a range of pages
   
   // locally flush the TLB entry for VMK_FIRST_MAP_VPN
   TLB_Invalidate(VMK_FIRST_MAP_VPN, TLB_LOCALONLY);

   // TODO: measure time to access VMK_FIRST_MAP_VPN

   SP_SpinBarrier(barrier);

   // locally flush the entire TLB
   TLB_Flush(TLB_LOCALONLY);

   // TODO: measure time to access a range of pages

   SP_SpinBarrier(barrier);

   // globally flush the TLB entry for VMK_FIRST_MAP_VPN
   if (id == 0) {
      TLB_Invalidate(VMK_FIRST_MAP_VPN, 0);
   }

   // TODO: measure time to access VMK_FIRST_MAP_VPN

   SP_SpinBarrier(barrier);

   // globally flush the entire TLB
   if (id == 0) {
      TLB_Flush(0);
   }

   // TODO: measure time to access a range of pages

   SP_SpinBarrier(barrier);

   return TRUE;
}

#endif

/*
 *---------------------------------------------------------------------
 *
 * TLB_GetMPN --
 *
 *      Return the mpn at this virtual address.
 *
 * Results:
 *      The mpn at this virtual address.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
MPN
TLB_GetMPN(VA va)
{
   VMK_PTE pte;
   VPN vpn = VA_2_VPN(va);

   ASSERT(vpn >= VMK_FIRST_MAP_VPN && vpn <= VMK_LAST_MAP_VPN);

   pte = tlbState.master[vpn - VMK_FIRST_MAP_VPN];
   if (PTE_PRESENT(pte)) {
      return VMK_PTE_2_MPN(pte);
   } else {
      return INVALID_MPN;
   }
}

static volatile Bool invalidateInProgress;
static volatile VPN flushVPN;
static volatile int flushCount;
static uint64 flushGen;
static uint64 perCPUFlushGen[MAX_PCPUS];

/*
 *----------------------------------------------------------------------
 *
 * TLBDoInvalidate --
 *
 *      Invalidate the given virtual page number "vpn" on the specified
 *	"pcpuNum", or on all CPUs if "pcpuNum" is TLB_PCPU_BROADCAST.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
TLBDoInvalidate(VPN vpn, PCPU pcpuNum)
{
   int numIter;
   SP_IRQL prevIRQL;

   if (vmkernelLoaded) {
      /*
       * We can't have interrupts disabled because we may block in this function.
       */
      ASSERT_HAS_INTERRUPTS();
      ASSERT(World_IsSafeToBlock());
   } else {
      /*
       * If we have not loaded the vmkernel yet then we can have interrupts
       * disabled.  However, if we are loading the vmkernel we are single threaded
       * so invalidateInProgress cannot be set.
       */
      ASSERT_NOT_IMPLEMENTED(!invalidateInProgress);
   }

   prevIRQL = SP_LockIRQ(&tlbState.invLock, SP_IRQL_KERNEL);

   while (invalidateInProgress) {
      LOG(0, "Waiting for another invalidate to finish ...");
      CpuSched_WaitIRQ((uint32)&invalidateInProgress,
                       CPUSCHED_WAIT_TLB,
                       &tlbState.invLock,
                       prevIRQL);
      SP_LockIRQ(&tlbState.invLock, SP_IRQL_KERNEL);
      LOG(0, "Trying to start invalidate again ...");
   }

   invalidateInProgress = TRUE;

   if (pcpuNum == TLB_PCPU_BROADCAST) {
      flushCount = numPCPUs - 1;
   } else {
      flushCount = 1;
   }
   flushVPN = vpn;
   flushGen++;

   SP_UnlockIRQ(&tlbState.invLock, prevIRQL);

   numIter = 0;
   do {
      int count = 0;
      
      if (pcpuNum == TLB_PCPU_BROADCAST) {
         LOG(1, "Sending Broadcast IPI from CPU %d", myPRDA.pcpuNum);
         APIC_BroadcastIPI(IDT_TLBINV_VECTOR);
      } else {
         LOG(1, "Sending IPI to CPU %d from CPU %d", pcpuNum, myPRDA.pcpuNum);
         APIC_SendIPI(pcpuNum, IDT_TLBINV_VECTOR);
      }

      do {
	 Util_Udelay(1);
      } while (count++ < TLB_INVAL_WAIT_MS*1000 && flushCount > 0);
      if (flushCount > 0) {
         LOG(0, "After %d milliseconds %d CPUs still not done", 
             TLB_INVAL_WAIT_MS, flushCount);
      }
      numIter++;
   } while (flushCount > 0 && numIter++ < 50);

   if (flushCount > 0) {
      // broadcast NMI so we get a backtrace of where all CPUs are.
      APIC_BroadcastNMI();
      Panic("TLBDoInvalidate: Timeout with %d CPUs left\n", flushCount);
   }

   SP_LockIRQ(&tlbState.invLock, SP_IRQL_KERNEL);

   invalidateInProgress = FALSE;

   CpuSched_Wakeup((uint32)&invalidateInProgress);

   SP_UnlockIRQ(&tlbState.invLock, prevIRQL);
}

/*
 *----------------------------------------------------------------------
 *
 * TLBInvalidateHandler --
 *
 *      Interrupt handler for the TLB invalidation on each CPU.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The proper TLB entry is invalidated and the perCPUFLushGen entry
 *	for this CPU is updated.
 *
 *----------------------------------------------------------------------
 */
static void
TLBInvalidateHandler(UNUSED_PARAM(void *clientData), UNUSED_PARAM(uint32 vector))
{
   SP_IRQL prevIRQL;

   prevIRQL = SP_LockIRQ(&tlbState.invLock, SP_IRQL_KERNEL);

   LOG(1, "cpu=%d myGen=%Lu flushGen=%Lu vpn=%d flushCount=%d",
       myPRDA.pcpuNum, perCPUFlushGen[myPRDA.pcpuNum], 
       flushGen, flushVPN, flushCount);
   if (invalidateInProgress && perCPUFlushGen[myPRDA.pcpuNum] < flushGen) {
      VPN vpn;

      perCPUFlushGen[myPRDA.pcpuNum] = flushGen;
      flushCount--;   
      vpn = flushVPN;
      SP_UnlockIRQ(&tlbState.invLock, prevIRQL);

      if (vpn == FLUSH_VPN) {
	 TLB_FLUSH();
      } else {
	 TLB_INVALIDATE_PAGE(VPN_2_VA(vpn));
      }
   } else {
      SP_UnlockIRQ(&tlbState.invLock, prevIRQL);
   }
}
