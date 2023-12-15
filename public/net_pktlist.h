/* **********************************************************
 * Copyright (C) 2004 VMware, Inc. All Rights Reserved
 * -- VMware Confidential.
 * **********************************************************/


/*
 * net_pktlist.h --
 *
 */

#ifndef _NET_PKTLIST_PUBLIC_H_
#define _NET_PKTLIST_PUBLIC_H_

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMK_MODULE
#define INCLUDE_ALLOW_DISTRIBUTE
#include "includeCheck.h"

struct PktList {
   Bool         mayModify;
   List_Links   pktList;
   size_t       numPktsInList;
};


#ifdef NO_PKTLIST_INLINE
#undef INLINE
#define INLINE
#endif

/*
 *----------------------------------------------------------------------------
 *
 * PktList_Init --
 *
 *    Initialise the packet list.
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
PktList_Init(PktList *pktList)
{
   ASSERT(pktList);
   pktList->mayModify = FALSE;
   List_Init(&pktList->pktList);
   pktList->numPktsInList = 0;
}

/*
 *----------------------------------------------------------------------------
 *
 * PktList_Count --
 *
 *    Return the number of packets in the list.
 *
 * Results:
 *    The number of packets in the list.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE size_t
PktList_Count(PktList *pktList)
{
   ASSERT(pktList);
   return pktList->numPktsInList;
}


/*
 *----------------------------------------------------------------------------
 *
 * PktList_AddToTail --
 *
 *    Add to the tail of the given list.
 *
 *
 * Results:
 *    Nothing.
 *
 * Side effects:
 *    list's packet count is incremented.
 *
 *----------------------------------------------------------------------------
 */

static INLINE void
PktList_AddToTail(PktList *list, PktHandle *entry)
{
   ASSERT(list && entry);
   List_InitElement(&entry->pktLinks);
   List_Insert(&entry->pktLinks, LIST_ATREAR(&list->pktList));
   list->numPktsInList++;
}


/*
 *----------------------------------------------------------------------------
 *
 * PktList_GetHead --
 *
 *    Returns the head of the list.
 *
 * Results:
 *    Head of the list if it exists, NULL if the list is empty.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE PktHandle *
PktList_GetHead(PktList *pktList)
{
   PktHandle *head;
   ASSERT(pktList);
   head = (PktHandle *)List_First(&pktList->pktList);
   if ((List_Links *)head != &pktList->pktList) {
      return head;
   } else {
      return NULL;
   }
}


/*
 *----------------------------------------------------------------------------
 *
 * PktList_GetTail --
 *
 *    Returns the tail of the list.
 *
 * Results:
 *    Tail of the list if it exists. NULL if the list is empty.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE PktHandle *
PktList_GetTail(PktList *pktList)
{
   PktHandle *tail;
   ASSERT(pktList);
   tail = (PktHandle *)List_Last(&pktList->pktList);
   if ((List_Links *)tail != &pktList->pktList) {
      return tail;
   } else {
      return NULL;
   }
}


/*
 *----------------------------------------------------------------------------
 *
 * PktList_GetNext --
 *
 *    Return the element following entry in the list.
 *
 * Results:
 *    The next element in the list, if it exists. NULL if the end of the list
 *    has been reached.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE PktHandle *
PktList_GetNext(PktList *list, PktHandle *entry)
{
   PktHandle *ret;
   ASSERT(list && entry);
   ret = (PktHandle *)List_Next(&entry->pktLinks);
   if ((List_Links *)ret != &list->pktList) {
      return ret;
   } else {
      return NULL;
   }
}


/*
 *----------------------------------------------------------------------------
 *
 * PktList_GetPrev --
 *
 *    Get the element in the list preceding the given entry.
 *
 * Results:
 *    The previous element in the list. NULL if the head of the list has been
 *    reached.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE PktHandle *
PktList_GetPrev(PktList *list, PktHandle *entry)
{
   PktHandle *ret;
   ASSERT(list && entry);
   ret = (PktHandle *)List_Prev(&entry->pktLinks);
   if ((List_Links *)ret != &list->pktList) {
      return ret;
   } else {
      return NULL;
   }
}


/*
 *----------------------------------------------------------------------------
 *
 * PktList_Remove --
 *
 *    Remove the given entry from the list.
 *
 * Results:
 *    Nothing.
 *
 * Side effects:
 *    list's packet count is decremented.
 *
 *----------------------------------------------------------------------------
 */

static INLINE void
PktList_Remove(PktList *list, PktHandle *entry)
{
   ASSERT(list);
   ASSERT(entry);
   ASSERT(&list->pktList != &entry->pktLinks); /* don't remove header */

   List_Remove(&entry->pktLinks);
   list->numPktsInList--;
}


/*
 *----------------------------------------------------------------------------
 *
 * PktList_IsEmpty --
 *
 *    Is the packet list empty?
 *
 * Results:
 *    TRUE if the packet list is empty. FALSE otherwise.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static inline Bool
PktList_IsEmpty(PktList *list)
{
   ASSERT(list);
   return List_IsEmpty(&list->pktList);
}


/*
 *----------------------------------------------------------------------------
 *
 * PktList_Join --
 *
 *    Join two lists.
 *
 * Results:
 *    Nothing.
 *
 * Side effects:
 *    The second list is emptied.
 *
 *----------------------------------------------------------------------------
 */

static INLINE void
PktList_Join(PktList *list, PktList *list2)
{
   PktHandle *headEntry;
   ASSERT(list);
   ASSERT(list2);

   while ((headEntry = PktList_GetHead(list2)) != NULL) {
      PktList_Remove(list2, headEntry);
      PktList_AddToTail(list, headEntry);
   }
}


/*
 *----------------------------------------------------------------------------
 *
 * PktList_AppendN --
 *
 *    Removes the first N elements of the srcList and appends it to the
 *    dstList.
 *
 * Results:
 *    Nothing.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE void
PktList_AppendN(PktList *dstList, PktList *srcList, uint32 numPkts)
{
   while (numPkts-- > 0 && !PktList_IsEmpty(srcList)) {
      PktHandle *curPkt = PktList_GetHead(srcList);
      PktList_Remove(srcList, curPkt);
      PktList_AddToTail(dstList, curPkt);
   }
}


/*
 *----------------------------------------------------------------------------
 *
 * PktList_CompleteAll --
 *
 *    Complete and free up all the entries in the list.
 *
 * Results:
 *    Nothing.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE void
PktList_CompleteAll(PktList *list)
{
   List_Links *curEntry;
   ASSERT(list);
   while (!List_IsEmpty(&list->pktList)) {
      PktHandle *pkt;

      curEntry = List_First(&list->pktList);
      pkt = (PktHandle *)curEntry;

      PktList_Remove(list, pkt);
      Pkt_Complete(pkt);
   }
   ASSERT(List_IsEmpty(&list->pktList));
}

#ifdef NO_PKTLIST_INLINE
#undef INLINE
#define INLINE inline
#endif

#endif //_NET_PKTLIST_PUBLIC_H_

