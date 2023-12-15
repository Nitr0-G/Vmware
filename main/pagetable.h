/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * pagetable.h --
 *
 *	Pagetable manipulation functions
 */

#ifndef _PAGETABLE_H
#define _PAGETABLE_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "kseg.h"

static INLINE void
PT_SET(VMK_PTE* pPTE, VMK_PTE val)
{
#ifdef VM_I386
   /*
    * PTEs are 64bits wide, but we don't have a 64bit write instruction,
    * except for compare-and-swap-8bytes in a loop, which is expensive and
    * overkill.  All we really need is to ensure that whenever the PTE has
    * an invalid value (a mix of old and new) the PTE is marked not
    * present, so the CPU won't read the contents. So, we first make PTE
    * present bit false, by writing 0 to the lower part of PTE, then write
    * the top part, and finally make it valid by writing the lower part
    * (which includes the present bit).
    */
   volatile uint32 *addr = (volatile uint32*)pPTE;
   addr[0] = 0;
   addr[1] = val >> 32;
   addr[0] = val & 0xffffffff;
#else
#error "PT_SET is not defined for non-IA-32 architectures."
#endif
}

// for storing random data in PTEs that are not valid
static INLINE void
PT_SET_DATA(VMK_PTE* pPTE, uint64 data)
{
   // make sure data doesn't have present bit set
   ASSERT(!PTE_PRESENT(data));
   PT_SET(pPTE, data);
}

static INLINE void
PT_INVAL(VMK_PTE* pPTE)
{
#ifdef VM_I386
   /*
    * See comment in PT_SET.  However this case is simpler because we don't
    * need to reset the top half of PTE.
    */
   volatile uint32 *addr = (volatile uint32*)pPTE;
   addr[0] = 0;
#else
#error "PT_INVAL is not defined for non-IA-32 architectures."
#endif
}

extern void PT_ReleasePageRoot(VMK_PDPTE *pageRoot);
extern void PT_FreePageRoot(MA pageRootMA);
extern VMK_PDPTE* PT_CopyPageRoot(MA srcPageRootMA, MA *pDestPageRootMA, MPN firstPageDir);
extern VMK_PDPTE* PT_AllocPageRoot(MA *pPTRootMA, MPN firstPageDir);

extern void PT_ReleasePageDir(VMK_PDE *pageDir, KSEG_Pair *pair);
extern VMK_PDE* PT_GetPageDir(MA pageRootMA, LA laddr, KSEG_Pair **pair);

extern void PT_ReleasePageTable(VMK_PTE *pageTable, KSEG_Pair *pair);
extern VMK_PTE* PT_GetPageTable(MA pageRootMA, LA laddr, KSEG_Pair **pair);
extern VMK_PTE* PT_GetPageTableInDir(VMK_PDE* pageDir, LA laddr, KSEG_Pair **pair);
extern VMK_PTE* PT_AllocPageTable(MA pageRootMA, LA addr, int flags, KSEG_Pair **pair, MPN *pPTableMPN);
extern VMK_PTE* PT_AllocPageTableInDir(VMK_PDE* pageDir, LA laddr, int flags, KSEG_Pair **pair, MPN *outPTableMPN);
extern void PT_LogPageRoot(MA pageRootMA, LA start, LA end);

extern void PT_AddPageTable(LA addr, MPN pageTableMPN);

#endif
