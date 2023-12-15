/* **********************************************************
 * Copyright (C) 2004 VMware, Inc. All Rights Reserved.
 * -- VMware Confidential.
 * **********************************************************/


/*
 * iochain.h -
 *
 *    Implements the iochain API. IOChains allow hooks to be associated with each
 *    port. These hooks typically perform a specific task. Functionality on the
 *    fast, common path are statically compiled.
 */

#ifndef _IOCHAIN_H_
#define _IOCHAIN_H_

typedef enum {
   IO_CHAIN_RANK_PRE_FILTER,
   IO_CHAIN_RANK_FILTER,
   IO_CHAIN_RANK_POST_FILTER,
   IO_CHAIN_RANK_QUEUE,
   IO_CHAIN_RANK_POST_QUEUE,
   IO_CHAIN_RANK_TERMINAL,
   IO_CHAIN_RANK_INVALID
} IOChainRank;

#define MAX_CHAIN_RANKS IO_CHAIN_RANK_INVALID

typedef void *IOChainData;

/* All IOChain hooks have the following prototype */
typedef VMK_ReturnStatus (*IOChainFn)(struct Port *, IOChainData, struct PktList *);
typedef VMK_ReturnStatus (*IOChainRemove)(IOChainData);
typedef VMK_ReturnStatus (*IOChainInsert)(IOChainData);

#define MAX_IOCHAIN_FN_NAME_LEN 32

/* each io chain is made up of zero or more links */
typedef struct IOChainLink {
   List_Links    chainLinks;        // first field of this struct. 
   IOChainFn     ioChainFn;         // The hook 
   IOChainRemove ioChainRemove;     // remove notification
   IOChainData   ioChainData;       // Data for the hook 
   IOChainRank   rank;              // the rank of this link
   Bool          modifiesPktList;   // Does this link modify the packet list 
   char          ioChainFnName[MAX_IOCHAIN_FN_NAME_LEN]; // fn symbol
} IOChainLink;


typedef struct IOChainStats {
   uint64  starts;        // number of times the chain was started 
   uint64  resumes;       // number of times the chain was resumed
   uint64  errors;        // number of chain errors (ie not packet errors)
   uint64  pktsStarted;   // packets given to this chain 
   uint64  pktsPassed;    // packets successfully transferred
   uint64  pktsFiltered;  // packets dropped by filters on this chain
   uint64  pktsQueued;    // inc'd every time any packet is queued on this chain
   uint64  pktsDropped;   // packets dropped for reasons other than filtering
} IOChainStats;

typedef struct IOChain {
   List_Links     chainHeads[MAX_CHAIN_RANKS];  // multi-level queue
   IOChainStats   stats;                        // stats for this chain
   IOChainLink   *startLink;                    // the entry point of the chain
   Net_PortID     portID;                       // port assoc if any
   uint32         modifiesPktList;              // number of links in this chain that
                                                //    modify the packet list. 
} IOChain;

void             IOChain_Init(IOChain *chain, Net_PortID portID);
VMK_ReturnStatus IOChain_InsertLink(IOChain *chain, IOChainLink *link);
void             IOChain_RemoveLink(IOChain *chain, IOChainLink *link);
VMK_ReturnStatus IOChain_InsertCall(IOChain *chain, IOChainRank rank, IOChainFn fn,
                                    IOChainInsert insert, IOChainRemove remove,
                                    IOChainData data, Bool modifiesPktList, 
                                    IOChainLink **iocl);
void             IOChain_RemoveCall(IOChain *chain, IOChainFn fn);
void             IOChain_ReleaseChain(IOChain *chain);
VMK_ReturnStatus IOChain_Resume(struct Port *port, IOChain *chain, IOChainLink *prevLink,
                                struct PktList *pktList);


/*
 *----------------------------------------------------------------------------
 *
 * IOChain_AllocLink --
 *
 *    Allocate and initialize an iochain link structure.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */
 
static INLINE IOChainLink *
IOChain_AllocLink(IOChainRank rank)
{
   IOChainLink *link = Mem_Alloc(sizeof(IOChainLink));
   if (link != NULL) {
      memset(link, 0, sizeof(IOChainLink));
      link->rank = rank;
   }

   return link;
}


/*
 *----------------------------------------------------------------------------
 *
 * IOChain_FreeLink --
 *
 *    Free the iochain link structure.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */
 
static INLINE void
IOChain_FreeLink(IOChainLink *link)
{
   ASSERT(link->chainLinks.prevPtr == NULL);
   ASSERT(link->chainLinks.nextPtr == NULL);

   Mem_Free(link);
}


/*
 *----------------------------------------------------------------------------
 *
 * IOChain_StatInc --
 *
 *    Increment the stat by inc.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */
 
static INLINE void
IOChain_StatInc(uint64 *stat, unsigned int inc)
{
   *stat += inc;
}


/*
 *----------------------------------------------------------------------------
 *
 * IOChain_Start --
 *
 *    Go through the chain, invoking the hooks at each link.
 *
 * Results:
 *    VMK_ReturnStatus indicating the outcome.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE VMK_ReturnStatus
IOChain_Start(struct Port *port, IOChain *chain, struct PktList *pktList)
{
   return IOChain_Resume(port, chain, NULL, pktList);
}


/*
 *----------------------------------------------------------------------------
 *
 *  IOChain_IsPktListModified --
 *
 *    Does any link within this chain modify the packet list?
 *
 *  Results:
 *    TRUE if the packet list modified anywhere along the line, FALSE
 *    otherwise.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */
 
static inline Bool
IOChain_IsPktListModified(IOChain *chain)
{
   ASSERT(chain);

   return chain->modifiesPktList;
}

#endif //_IOCHAIN_H_
