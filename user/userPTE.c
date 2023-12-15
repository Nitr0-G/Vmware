/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * userPTE.c --
 *
 *	Page Table Entry manipulations
 */

#include "user_int.h"
#include "userPTE.h"

#define LOGLEVEL_MODULE     UserPTE
#include "userLog.h"


/*
 *----------------------------------------------------------------------
 *
 * UserPTE_GetFlags --
 *
 *      Get the original PTE flags installed.
 *
 * Results:
 *	return PTE flags
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
uint32
UserPTE_GetFlags(const UserPTE *pte)
{
   uint32 pteFlags;

   if (UserPTE_IsPresent(pte)) {
      pteFlags = pte->u.livePte.flags;
      /*
       * If this PTE is present, take into account whether the writable
       * bit is saved.
       */
      if (pte->u.livePte.rw_save) {
         pteFlags |= PTE_RW;
      }
   } else {
      pteFlags = pte->u.cachedPte.savedFlags;
      ASSERT((pteFlags & PTE_P) != 0);
   }

   return pteFlags;
}


/*
 *----------------------------------------------------------------------
 *
 * UserPTE_GetMPN --
 *
 *      Get the mpn installed in the pte.
 *
 * Results:
 *	Return mpn installed in the pte.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
MPN 
UserPTE_GetMPN(const UserPTE *pte)
{
   MPN mpn;

   if (UserPTE_IsPresent(pte)) {
      mpn = (MPN)pte->u.livePte.pfn;
   } else if (UserPTE_IsSwapping(pte)) {
      /*
       * During swapping, we either set data to real MPN (swap-out)
       * or we set data to INVALID_MPN (swap-in).
       */
      mpn = (MPN)pte->u.cachedPte.data;
   } else {
      /*
       * In all other cases (such as page swapped out), mpn is invalid.
       */
      mpn = INVALID_MPN;
   }

   return mpn;
}


/*
 *----------------------------------------------------------------------
 *
 * UserPTE_EnableWrite--
 *
 *      Set PTE_RW in the pte.
 *
 * Results:
 *	Return TRUE if a subsequent TLB_FLUSH is required.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
Bool
UserPTE_EnableWrite(UserPTE *pte)
{
   UserPTE newPteInfo = *pte;

   if (UserPTE_IsPresent(pte)) {
      /*
       * If this page is not already hardware write enabled, set 
       * livePte.rw_save to indicate that the page is writeable.
       * We will enable it lazily.
       */
      if (!UserPTE_HdWriteEnabled(pte)) {
         newPteInfo.u.livePte.rw_save = 1;
      }
   } else if (UserPTE_InSwap(pte)) {
      // non-present PTE, safe to modify
      newPteInfo.u.cachedPte.savedFlags |= PTE_RW;
   } else {
      NOT_REACHED();
   }

   UserPTE_SetImmed(pte, newPteInfo.u.raw); 
   return FALSE;
}


/*
 *----------------------------------------------------------------------
 *
 * UserPTE_DisableWrite--
 *
 *      Clear PTE_RW in the pte.
 *
 * Results:
 *	Return TRUE if a subsequent TLB_FLUSH is required.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
Bool
UserPTE_DisableWrite(UserPTE *pte)
{
   UserPTE newPteInfo = *pte;
   Bool needFlush = FALSE;

   if (UserPTE_IsPresent(pte)) {
      newPteInfo.u.livePte.rw_save = 0;
      if (UserPTE_HdWriteEnabled(pte)) {
         newPteInfo.u.livePte.flags &= ~PTE_RW;
         needFlush = TRUE;
      }
   } else if (UserPTE_InSwap(pte)) {
      newPteInfo.u.cachedPte.savedFlags &= ~PTE_RW;
   } else {
      NOT_REACHED();
   }

   UserPTE_SetImmed(pte, newPteInfo.u.raw); 
   return needFlush;
}


