/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. 
 * -- VMware Confidential
 * **********************************************************/

/*
 * iochain.c  --
 *
 *    Implements the iochain API. IOChains allow hooks to be associated with each
 *    port. These hooks typically perform a specific task. Functionality on the
 *    fast, common path are statically compiled.
 *
 *    When reading this code, remember that "input" and "output" are used wrt
 *    the portset.  ie a transmit from a VM to a physical network consists of
 *    first an *input* to the VM's port on the vswitch and then an *output*
 *    on the uplink port of the vswitch.
 */

#include "vmkernel.h"
#define LOGLEVEL_MODULE_LEN 0
#define LOGLEVEL_MODULE Net
#include "log.h"

#include "mod_loader.h" // for Mod_LookupSymbolSafe

#include "net_int.h"

/*
 *----------------------------------------------------------------------------
 *
 * IOChainDump --
 *
 *    dump verbose debug info.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */
 
static void
IOChainDump(IOChain *chain)
{
   int i;
   List_Links *curEntry;
   IOChainLink *link;
   ASSERT(chain);
   LOG(5, "chain = %p modifiesPktList = %d startLink = %s", 
       chain, chain->modifiesPktList, 
       chain->startLink ? chain->startLink->ioChainFnName : "<NULL>");
   for (i = 0; i < MAX_CHAIN_RANKS; i++) {
      LIST_FORALL(&chain->chainHeads[i], curEntry) {
         link = (IOChainLink *)curEntry;
         LOG(5, "%d: link = %p ioChainFn = %s <%p> ioChainData = %p",
             i, link, link->ioChainFnName, link->ioChainFn, link->ioChainData);
      }
   }
}


/*
 *----------------------------------------------------------------------------
 *
 * IOChain_Init --
 *
 *    Initialize the IOChain.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */
 
void
IOChain_Init(IOChain *chain, PortID portID)
{
   int i;

   ASSERT(chain);
   ASSERT((portID == NET_INVALID_PORT_ID) ||
          Portset_LockedExclHint(Portset_FindByPortID(portID)));

   memset(chain, 0, sizeof(IOChain));
   for (i = 0; i < MAX_CHAIN_RANKS; i++) {
      List_Init(&chain->chainHeads[i]);
   }
   chain->portID = portID;
}


/*
 *----------------------------------------------------------------------------
 *
 * IOChainRebuild --
 *
 *    Find the start of an IOChain, and save the pointer.  Called to 
 *    recompute the start after a link is inserted or removed.
 *
 *    TODO: in future might collapse the chain into a single list or
 *          otherwise optimize it.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static void
IOChainRebuild(IOChain *chain)
{
   int i;

   ASSERT((chain->portID == NET_INVALID_PORT_ID) ||
          Portset_LockedExclHint(Portset_FindByPortID(chain->portID)));
          
   chain->startLink = NULL;
   for (i = 0; i < MAX_CHAIN_RANKS; i++) {
      if (!List_IsEmpty(&chain->chainHeads[i])) {
         chain->startLink = (IOChainLink *)List_First(&chain->chainHeads[i]);
         return;
      }
   }
}


/*
 *----------------------------------------------------------------------------
 *
 * IOChain_InsertLink --
 *
 *    Insert the given link at the head of the chain determined by the given
 *    rank.
 *
 *  Results:
 *    None.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

VMK_ReturnStatus
IOChain_InsertLink(IOChain *chain, IOChainLink *link)
{
   VMK_ReturnStatus ret = VMK_OK;

   ASSERT(chain);
   ASSERT(link);
   ASSERT(link->rank < MAX_CHAIN_RANKS);
   ASSERT(link->rank != IO_CHAIN_RANK_INVALID);
   ASSERT((link->rank != IO_CHAIN_RANK_TERMINAL) || 
          List_IsEmpty(&chain->chainHeads[IO_CHAIN_RANK_TERMINAL]));
   ASSERT((chain->portID == NET_INVALID_PORT_ID) ||
          Portset_LockedExclHint(Portset_FindByPortID(chain->portID)));        

   if (UNLIKELY(VMK_STRESS_DEBUG_COUNTER(NET_IOCHAIN_INSERT_FAIL))) {
      return VMK_FAILURE;
   }

   if (link != NULL) {
      uint32 unused;

      List_InitElement(&link->chainLinks);
      List_Insert(&link->chainLinks, LIST_ATFRONT(&chain->chainHeads[link->rank]));
      if (link->modifiesPktList) {
         chain->modifiesPktList++;
      }

      if(!Mod_LookupSymbolSafe((uint32)link->ioChainFn, MAX_IOCHAIN_FN_NAME_LEN - 1,
                               link->ioChainFnName, &unused)) {
         snprintf(link->ioChainFnName, MAX_IOCHAIN_FN_NAME_LEN - 1, 
                  "<%p>", link->ioChainFn);
      }
      link->ioChainFnName[MAX_IOCHAIN_FN_NAME_LEN - 1] = 0;

      IOChainDump(chain);

      IOChainRebuild(chain);
   } else {
      ret = VMK_NO_RESOURCES;
   }
   return ret;
}


/*
 *----------------------------------------------------------------------------
 *
 * IOChain_RemoveLink --
 *
 *    Find the link and remove it from the given chain.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

void
IOChain_RemoveLink(IOChain *chain, IOChainLink *targetLink)
{
   int i;
   List_Links *curEntry;
   IOChainLink *link;

   ASSERT(chain);
   ASSERT(targetLink);
   ASSERT((chain->portID == NET_INVALID_PORT_ID) ||
          Portset_LockedExclHint(Portset_FindByPortID(chain->portID)));        

   for (i = 0; i < MAX_CHAIN_RANKS; i++) {
      LIST_FORALL(&chain->chainHeads[i], curEntry) {
         link = (IOChainLink *)curEntry;
         if (targetLink == link) {
            List_Remove(curEntry);
            if (link->modifiesPktList) {
               chain->modifiesPktList--;
            }

            if (link->ioChainRemove) {
               link->ioChainRemove(link->ioChainData);
            }

            link->chainLinks.prevPtr = NULL;
            link->chainLinks.nextPtr = NULL;

            IOChainRebuild(chain);
            return;
         }
      }
   }
}


/*
 *----------------------------------------------------------------------------
 *
 * IOChain_InsertCall --
 *
 *    Insert the given link at the head of the chain determined by the given
 *    rank.
 *
 *  Results:
 *    None.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

VMK_ReturnStatus
IOChain_InsertCall(IOChain *chain, IOChainRank rank, IOChainFn fn,
                   IOChainInsert insert, IOChainRemove remove,
                   IOChainData data, Bool modifiesPktList,
                   IOChainLink **iocl)
{
   VMK_ReturnStatus status;
   IOChainLink *link = IOChain_AllocLink(rank);

   if (iocl != NULL) {
      *iocl = NULL;
   }

   if (link == NULL) {
      return VMK_NO_RESOURCES;
   }

   link->ioChainFn = fn;
   link->ioChainRemove = remove;
   link->ioChainData = data;
   link->modifiesPktList = modifiesPktList;

   status = IOChain_InsertLink(chain, link);

   if (status == VMK_OK) {
      if (insert) {
         insert(link->ioChainData);
      }
      if (iocl != NULL) {
         *iocl = link;
      }
   } else {
      IOChain_FreeLink(link);
   }

   return status;
}

/*
 *----------------------------------------------------------------------------
 *
 * IOChain_RemoveCall --
 *
 *    Find the link that corresponds to the given function and remove it from
 *    the given chain.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

void
IOChain_RemoveCall(IOChain *chain, IOChainFn fn)
{
   int i;
   List_Links *curEntry;
   IOChainLink *link;
   ASSERT(chain);
   ASSERT(fn);
   for (i = 0; i < MAX_CHAIN_RANKS; i++) {
      curEntry = List_First(&chain->chainHeads[i]);
      while (!List_IsAtEnd(&chain->chainHeads[i], curEntry)) {
         link = (IOChainLink *)curEntry;
         curEntry = List_Next(curEntry);
         if (link->ioChainFn == fn) {
            IOChain_RemoveLink(chain, link);
            IOChain_FreeLink(link);
         }
      }
   }
}


/*
 *----------------------------------------------------------------------------
 *
 * IOChain_ReleaseChain --
 *
 *    Free all the links in the IOChain.
 *
 *  Results:
 *    None.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */
 
void
IOChain_ReleaseChain(IOChain *chain)
{
   int i;
   List_Links *curEntry;
   ASSERT(chain);
   for (i = 0; i < MAX_CHAIN_RANKS; i++) {
      while (!List_IsEmpty(&chain->chainHeads[i])) {
         curEntry = List_First(&chain->chainHeads[i]);
         IOChain_RemoveLink(chain, (IOChainLink *)curEntry);
         IOChain_FreeLink((IOChainLink *)curEntry);
      }
   }
}


/*
 *----------------------------------------------------------------------------
 *
 * IOChain_Resume --
 *
 *    Go through the chain, invoking the hooks at each link after the
 *    indicated starting link.
 *
 * Results:
 *    VMK_ReturnStatus indicating the outcome.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

VMK_ReturnStatus
IOChain_Resume(struct Port *port, IOChain *chain, IOChainLink *prevLink,
               struct PktList *pktList)
{
   int i;
   List_Links *curEntry;
   IOChainLink *chainLink;
   VMK_ReturnStatus ret = VMK_OK;
   PktList clonedList;

   ASSERT(port);
   ASSERT(chain);
   ASSERT(pktList);
   ASSERT((chain->portID == NET_INVALID_PORT_ID) ||
          Portset_LockedHint(Portset_FindByPortID(chain->portID)));        

   PktList_Init(&clonedList);

   // see if we're resuming or starting
   if (prevLink == NULL) {

      IOChain_StatInc(&chain->stats.starts, 1);
      IOChain_StatInc(&chain->stats.pktsStarted, PktList_Count(pktList));

      if (chain->startLink != NULL) {
         // potentially clone the list before starting
         if (chain->modifiesPktList &&  !pktList->mayModify) {
            ret = PktList_Clone(pktList, &clonedList);
            if (ret == VMK_OK) {
               clonedList.mayModify = TRUE;
               pktList = &clonedList;
            } else {
               goto done;
            }         
         }
         LOG(20, "starting chain %p at %s for %u pkts", 
             chain, chain->startLink->ioChainFnName, PktList_Count(pktList));
         curEntry = (List_Links *)chain->startLink;
         i = chain->startLink->rank;
      } else {
         // empty chain, nothing to do.
         LOG(20, "empty chain %p", chain);
         ret = VMK_OK;
         goto done;
      }
   } else {
      IOChain_StatInc(&chain->stats.resumes, 1);
      curEntry = prevLink->chainLinks.nextPtr;   
      i = prevLink->rank;
      LOG(20, "resuming chain %p from %s for %u pkts", 
          chain, prevLink->ioChainFnName, PktList_Count(pktList));
   }
   
   ASSERT(i < MAX_CHAIN_RANKS);

   if (VMK_STRESS_DEBUG_COUNTER(NET_IOCHAIN_RESUME_FAIL)) {
      ret = VMK_FAILURE;
      goto done;
   }

   do {
      unsigned int pktsIn = PktList_Count(pktList);
      LOG(20, "processing rank %u", i);
      LIST_ITER(&chain->chainHeads[i], curEntry) {
         chainLink = (IOChainLink *)curEntry;
	 LOG(20, "call %s", chainLink->ioChainFnName);
         if (!PktList_IsEmpty(pktList)) {
            ASSERT(chainLink);
            ASSERT(chainLink->ioChainFn);
            LOG(20, "calling link %u:%s", i,  chainLink->ioChainFnName);
            ret = chainLink->ioChainFn(port, chainLink->ioChainData, pktList);
            if (UNLIKELY(ret != VMK_OK)) {
               /* some errors may be non-fatal. */
	       LOG(3, "%s: %s", chainLink->ioChainFnName, VMK_ReturnStatusToString(ret));
               goto done;
            }
         } else {
            LOG(20, "no more pkts");
            goto done;
         }
      }

      // check if we lost any 
      if (UNLIKELY(pktsIn > PktList_Count(pktList))) {
         unsigned int pktsEaten = pktsIn - PktList_Count(pktList);

         switch (i) {
         case IO_CHAIN_RANK_TERMINAL:
            // if the terminal rank eats it we've done our job
            IOChain_StatInc(&chain->stats.pktsPassed, pktsEaten);
            break;
         case IO_CHAIN_RANK_FILTER:
            // filter rank expected to eat some
            IOChain_StatInc(&chain->stats.pktsFiltered, pktsEaten);
            break;
         case IO_CHAIN_RANK_QUEUE:
            // these will be fed back in later
            IOChain_StatInc(&chain->stats.pktsQueued, pktsEaten);
            break;
         default:
            // noone else is supposed to eat them
            IOChain_StatInc(&chain->stats.pktsDropped, pktsEaten);            
         }
      }

      i++;
      curEntry = List_First(&chain->chainHeads[i]);
   } while (i < MAX_CHAIN_RANKS);

done:

   if (ret == VMK_OK) {
      /*
       * the terminal rank doesn't have to eat them, so anything
       * that made it all the way through is considered passed
       */
      IOChain_StatInc(&chain->stats.pktsPassed, PktList_Count(pktList));
   } else {
      IOChain_StatInc(&chain->stats.errors, 1);
      IOChain_StatInc(&chain->stats.pktsDropped, PktList_Count(pktList));
   }

   // free up any clones we allocated above
   PktList_CompleteAll(&clonedList);

   return ret;
}

