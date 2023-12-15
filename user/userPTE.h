/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/* 
 * userPTE.h - 
 *
 *	UserWorld page table entries and their accessors.
 *
 */

#ifndef VMKERNEL_USER_USERPTE_H
#define VMKERNEL_USER_USERPTE_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "vm_basic_types.h"
#include "vmkernel.h"
#include "pagetable.h"

/* Present PTE entry definition (used for hardware pagetable)  */
typedef struct {
   uint64 flags:        9;      // x86 defined
   uint64 pshared:      1;      // page sharing enabled
   uint64 rw_save:      1;      // saved PTE_RW bit
   uint64 pinned:       1;      // page not swappable 
   uint64 pfn:          24;     // x86 defined page number
   uint64 rsvd:         28;     // x86 reserved
} UserPTE_Live;

/* Non-Present PTE entry definition (used for caching PTE information) */
typedef struct {
   uint32 present:      1;      // must be 0
   uint32 swapped:      1;      // page swapped out
   uint32 used:         1;      // page used
   uint32 swapping:     1;      // page being swapped
   uint32 savedFlags:   9;      // saved PTE.flags
   uint32 pad:          19;     // unused bits
   uint32 data;                 // used for swap slot, pointer, etc.
} UserPTE_Cached;

/*
 * We use PTE to store various information and property of a page.
 * The following rules generally applies to all PTEs in the pagetable.
 *
 * If pteInfo.u.present == 1, pteInfo.u.livePte is defined.
 * If pteInfo.u.present == 0, pteInfo.u.cachedPte is defined.
 * If pteInfo.raw == 0, the entry is empty.
 */
typedef struct {
   union {
      uint64          present:   1;
      UserPTE_Live    livePte;
      UserPTE_Cached  cachedPte;
      VMK_PTE         raw;
   } u;
} UserPTE;

#define VMK_PTE_CLEAR_ACCESS(_pte)      (_pte & (~(uint64)PTE_A))

// protection bits defined in Usermem MMap.
#define USERMEM_PTE_PROT        (PTE_P | PTE_RW | PTE_PCD)


/*
 *----------------------------------------------------------------------
 *
 * UserPTE_IsPresent --
 *
 *      Test if pte is present.
 *
 * Results:
 *	TRUE if pte is present, FALSE otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */ 
static INLINE Bool
UserPTE_IsPresent(const UserPTE *pte)
{
   return (pte->u.present != 0);
}


/*
 *----------------------------------------------------------------------
 *
 * UserPTE_IsPShared --
 *
 *      Test if pte is pshared.
 *
 * Results:
 *	TRUE if pte is pshared, FALSE otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
UserPTE_IsPShared(const UserPTE *pte)
{
   return (pte->u.present != 0) && (pte->u.livePte.pshared != 0);
}


/*
 *----------------------------------------------------------------------
 *
 * UserPTE_IsPinned --
 *
 *      Test if pte is pinned.
 *
 * Results:
 *	TRUE if pte is pinned, FALSE otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
UserPTE_IsPinned(const UserPTE *pte)
{
   return (pte->u.present != 0) && (pte->u.livePte.pinned != 0);
}


/*
 *----------------------------------------------------------------------
 *
 * UserPTE_IsMapped --
 *
 *      Test if pte is mapped to either an mpn or swap page.
 *
 * Results:
 *	TRUE if pte is mapped, FALSE otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
UserPTE_IsMapped(const UserPTE *pte)
{
   return (pte->u.present != 0) || (pte->u.cachedPte.swapped != 0) || 
          (pte->u.cachedPte.swapping != 0);
}


/*
 *----------------------------------------------------------------------
 *
 * UserPTE_IsSwapping --
 *
 *      Test if pte contains a page being swapped in or out.
 *
 * Results:
 *	TRUE if the page is being swapped, FALSE otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
UserPTE_IsSwapping(const UserPTE *pte)
{
   return (pte->u.present == 0) && (pte->u.cachedPte.swapping != 0);
}


/*
 *----------------------------------------------------------------------
 *
 * UserPTE_IsSwapped --
 *
 *      Test if pte contains a page that has been swapped out.
 *
 * Results:
 *	TRUE if the page has been swapped out, FALSE otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
UserPTE_IsSwapped(const UserPTE *pte)
{
   return (pte->u.present == 0) && (pte->u.cachedPte.swapped != 0);
}


/*
 *----------------------------------------------------------------------
 *
 * UserPTE_InSwap --
 *
 *      Test if a page is swapped out or in the process of swapping.
 *
 * Results:
 *	TRUE if the page is in swap, FALSE otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
UserPTE_InSwap(const UserPTE *pte)
{
   return (pte->u.present == 0) && 
          ((pte->u.cachedPte.swapping != 0) || (pte->u.cachedPte.swapped != 0));
}


/*
 *----------------------------------------------------------------------
 *
 * UserPTE_GetSwapSlot --
 *
 *      Get the swap slot installed in the pte.
 *
 * Results:
 *	return PTE flags
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE uint32
UserPTE_GetSwapSlot(const UserPTE *pte)
{
   ASSERT(UserPTE_IsSwapped(pte));
   return pte->u.cachedPte.data;
}


/*
 *----------------------------------------------------------------------
 *
 * UserPTE_SetImmed --
 *
 *      Atomically set raw PTE to givev value without hardware conflict.
 *
 * Results:
 *	PTE modified.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
UserPTE_SetImmed(UserPTE *pte, VMK_PTE pteVal)
{
   PT_SET(&pte->u.raw, pteVal);
}


/*
 *----------------------------------------------------------------------
 *
 * UserPTE_Set --
 *
 *      Set value to a PTE (present).
 *
 * Results:
 *	pte modified
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
UserPTE_Set(UserPTE *pte,     // OUT
            MPN mpn,          // IN
            uint32 pteFlags,  // IN
            Bool pinned,      // IN
            Bool delayRW)     // IN
{
   UserPTE newPteInfo;

   /*
    * Lazy write enabling.
    *
    * This is to enable pshare for unmodified RW pages.
    * 
    * We don't set PTE_RW in PTE initially, but rather store it in
    * "pte.rw_save". When a write occurs on the page, we receive a 
    * write protection fault. At that time, we set PTE_RW bit.
    */
   if (delayRW) {
      newPteInfo.u.livePte.flags = (pteFlags & ~PTE_RW) | PTE_A;
      newPteInfo.u.livePte.rw_save = ((pteFlags & PTE_RW) != 0);
   } else {
      newPteInfo.u.livePte.flags = pteFlags | PTE_A;
      newPteInfo.u.livePte.rw_save = 0;
   }
   ASSERT(newPteInfo.u.present == 1);
   ASSERT((newPteInfo.u.livePte.flags & PTE_US) != 0);
   newPteInfo.u.livePte.pshared = 0;
   newPteInfo.u.livePte.pinned = pinned;
   newPteInfo.u.livePte.pfn = mpn;
   newPteInfo.u.livePte.rsvd = 0;

   UserPTE_SetImmed(pte, newPteInfo.u.raw);
}


/*
 *----------------------------------------------------------------------
 *
 * UserPTE_SetPinned --
 *
 *      Set a PTE to pinned.
 *
 * Results:
 *	pte modified
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
UserPTE_SetPinned(UserPTE *pte)    // IN/OUT
{
   UserPTE newPteInfo = *pte;

   ASSERT(!UserPTE_IsPShared(pte));
   ASSERT(newPteInfo.u.present == 1);
   newPteInfo.u.livePte.pinned = 1;
   UserPTE_SetImmed(pte, newPteInfo.u.raw);
}


/*
 *----------------------------------------------------------------------
 *
 * UserPTE_SetPShare --
 *
 *      Set a PTE to pshared and install the new mpn. 
 *
 * Results:
 *	pte modified
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
UserPTE_SetPShare(UserPTE *pte,    // IN/OUT
                    MPN mpnShared)      // IN
{
   UserPTE newPteInfo = *pte;

   ASSERT(!UserPTE_IsPinned(pte));
   ASSERT(newPteInfo.u.present == 1);
   newPteInfo.u.livePte.pshared = 1;
   newPteInfo.u.livePte.pfn = mpnShared;    // update the page number

   UserPTE_SetImmed(pte, newPteInfo.u.raw);
}


/*
 *----------------------------------------------------------------------
 *
 * UserPTE_SetSwap --
 *
 *      Set the PTE (non-present) to indicate a page swapped-out.
 *
 * Results:
 *	pte modified
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
UserPTE_SetSwap(UserPTE *pte,            // OUT
                  uint32 swapFileSlot,     // IN
                  uint32 pteFlags)         // IN
{
   UserPTE newPteInfo;
   
   newPteInfo.u.present = 0;
   newPteInfo.u.cachedPte.swapped = 1;
   newPteInfo.u.cachedPte.used = 1;
   newPteInfo.u.cachedPte.swapping = 0;
   newPteInfo.u.cachedPte.savedFlags = pteFlags;
   newPteInfo.u.cachedPte.data = swapFileSlot;

   UserPTE_SetImmed(pte, newPteInfo.u.raw); 
}


/*
 *----------------------------------------------------------------------
 *
 * UserPTE_SetSwapBusy --
 *
 *      Set the PTE (non-present) to indicate a page being swapped.
 *      When passing in INVALID_MPN, we're swapping in, otherwise,
 *      we're swapping out.
 *
 * Results:
 *	pte modified
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
UserPTE_SetSwapBusy(UserPTE *pte,  // OUT
                      MPN mpn,          // IN
                      uint32 pteFlags)  // IN
{
   UserPTE newPteInfo;
 
   ASSERT(!UserPTE_IsPinned(pte));
   newPteInfo.u.present = 0;
   newPteInfo.u.cachedPte.swapped = 0;
   newPteInfo.u.cachedPte.used = 1;
   newPteInfo.u.cachedPte.swapping = 1;
   newPteInfo.u.cachedPte.savedFlags = pteFlags;
   newPteInfo.u.cachedPte.data = mpn;

   UserPTE_SetImmed(pte, newPteInfo.u.raw); 
}


/*
 *----------------------------------------------------------------------
 *
 * UserPTE_HdWriteEnabled--
 *      Is the page's PTE_RW bit set?
 *
 * Results:
 *	Return TRUE if PTE_RW is enabled on the page, FALSE otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
UserPTE_HdWriteEnabled(const UserPTE *pte)
{
   ASSERT(UserPTE_IsPresent(pte)); 
   return PTE_WRITEABLE(pte->u.livePte.flags);
}


/*
 *----------------------------------------------------------------------
 *
 * UserPTE_IsWritable--
 *      Is the page writable?
 *
 * Results:
 *	Return TRUE if either PTE_RW or pte.rw_save is enabled on the page.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
UserPTE_IsWritable(const UserPTE *pte)
{
   ASSERT(UserPTE_IsPresent(pte)); 
   return UserPTE_HdWriteEnabled(pte) || (pte->u.livePte.rw_save != 0);
}


/*
 *----------------------------------------------------------------------
 *
 * UserPTE_IsInUse --
 *
 *	Test if given pte is in use by an mmap region.  Also return the
 *	stored data pointer and permissions.
 *
 * Results:
 *	TRUE if in use, FALSE otherwise, and the data pointer and
 *	permissions.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
UserPTE_IsInUse(const UserPTE *pte)
{
   ASSERT(!UserPTE_IsMapped(pte));
   return pte->u.cachedPte.used;
}


/*
 *----------------------------------------------------------------------
 *
 * UserPTE_GetProt --
 *
 *	Get the permissions of the PTE.
 *
 * Results:
 *	Return the permissions.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE uint32
UserPTE_GetProt(const UserPTE *pte)
{
   ASSERT(!UserPTE_IsMapped(pte));
   ASSERT(UserPTE_IsInUse(pte));
   ASSERT((pte->u.cachedPte.savedFlags & ~USERMEM_PTE_PROT) == 0);
   return pte->u.cachedPte.savedFlags;
}


/*
 *----------------------------------------------------------------------
 *
 * UserPTE_GetPtr --
 *
 *	Get the pointer stored in the PTE.
 *
 * Results:
 *	Return the pointer.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void *
UserPTE_GetPtr(const UserPTE *pte)
{
   ASSERT(!UserPTE_IsMapped(pte));
   ASSERT(UserPTE_IsInUse(pte));
   return (void *)pte->u.cachedPte.data;
}


/*
 *----------------------------------------------------------------------
 *
 * UserPTE_SetInUse --
 *
 *	Mark the given pte as in use, save the permissions, and also
 *	store the given data pointer in the pte.  We can do this since
 *	the pte doesn't contain a valid mapping.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
UserPTE_SetInUse(UserPTE *pte,
                 uint32 prot,
                 const void * data)
{
   UserPTE newPteInfo;

   ASSERT(!UserPTE_IsMapped(pte));
   ASSERT((prot & ~USERMEM_PTE_PROT) == 0);

   newPteInfo.u.raw = 0;
   newPteInfo.u.cachedPte.used = 1;
   newPteInfo.u.cachedPte.savedFlags = prot;
   newPteInfo.u.cachedPte.data = (uint32)data;

   UserPTE_SetImmed(pte, newPteInfo.u.raw); 
}


/*
 *----------------------------------------------------------------------
 *
 * UserPTE_Clear --
 *
 *	Clear the given PTE entry.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
UserPTE_Clear(UserPTE *pte)
{
   UserPTE_SetImmed(pte, 0);
}


/*
 *----------------------------------------------------------------------
 *
 * UserPTE_For --
 *
 *	Get the PTE in the page table at a given address.
 *
 * Results:
 *	Return the PTE entry.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE UserPTE *
UserPTE_For(VMK_PTE *pageTable, LA la)
{
   return (UserPTE *)&pageTable[ADDR_PTE_BITS(la)];
}

uint32 UserPTE_GetFlags(const UserPTE *pte);
MPN UserPTE_GetMPN(const UserPTE *pte);
Bool UserPTE_EnableWrite(UserPTE *pte);
Bool UserPTE_DisableWrite(UserPTE *pte);

#endif /* VMKERNEL_USER_USERPTE_H */
