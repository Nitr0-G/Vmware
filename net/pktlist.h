/* **********************************************************
 * Copyright (C) 2004 VMware, Inc. All Rights Reserved
 * -- VMware Confidential
 * **********************************************************/


#ifndef _PKTLIST_H_
#define _PKTLIST_H_
#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "memalloc.h"
#include "pkt.h"
#include "net_pktlist.h"
#include "vmkstress.h"

#ifdef NO_PKTLIST_INLINE
#undef INLINE
#define INLINE
#endif

/*
 *----------------------------------------------------------------------------
 *
 * PktList_AddToHead --
 *
 *    Add an entry to the head of the given list.
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
PktList_AddToHead(PktList *list, PktHandle *entry)
{
   ASSERT(list && entry);
   List_InitElement(&entry->pktLinks);
   List_Insert(&entry->pktLinks, LIST_ATFRONT(&list->pktList));
   list->numPktsInList++;
}


/*
 *----------------------------------------------------------------------------
 *
 * PktList_InsertAfter --
 *
 *    Insert after a given entry.
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
PktList_InsertAfter(PktList *list, PktHandle *targetEntry,
                    PktHandle *newEntry)
{
   ASSERT(list && targetEntry && newEntry);
   List_InitElement(&newEntry->pktLinks);
   List_Insert(&newEntry->pktLinks, LIST_AFTER(&targetEntry->pktLinks));
   list->numPktsInList++;
}


/*
 *----------------------------------------------------------------------------
 *
 * PktList_InsertBefore --
 *
 *    Insert before a given entry.
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
PktList_InsertBefore(PktList *list, PktHandle *targetEntry,
                     PktHandle *newEntry)
{
   ASSERT(list);
   ASSERT(targetEntry);
   ASSERT(newEntry);
   List_InitElement(&newEntry->pktLinks);
   List_Insert(&newEntry->pktLinks, LIST_BEFORE(&targetEntry->pktLinks));
   list->numPktsInList++;
}


/*
 *----------------------------------------------------------------------------
 *
 * PktList_Replace --
 *
 *    Replace the given entry with a new entry.
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
PktList_Replace(PktList *list, PktHandle *targetEntry,
                PktHandle *newEntry)
{
   PktList_InsertAfter(list, targetEntry, newEntry);
   PktList_Remove(list, targetEntry);
}



/*
 *----------------------------------------------------------------------------
 *
 * PktList_ReleaseAll --
 *
 *    Free up all the entries in the list.
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
PktList_ReleaseAll(PktList *list)
{
   List_Links *curEntry;
   ASSERT(list);
   while (!List_IsEmpty(&list->pktList)) {
      curEntry = List_First(&list->pktList);
      PktList_Remove(list, (PktHandle *)curEntry);
      Pkt_Release((PktHandle *)curEntry);
   }
   ASSERT(List_IsEmpty(&list->pktList));
}

/*
 *----------------------------------------------------------------------------
 *
 * PktList_Split --
 *
 *    Split the given list at the given entry. The entry at which the split
 *    occurs is put in the second list.
 *
 * Results:
 *    Pointer to a second list. The given list is modified to contain only
 *    entries upto the target entry.
 *
 * Side effects:
 *    The packet count of the given pktlist is changed.
 *
 *----------------------------------------------------------------------------
 */

static INLINE void
PktList_Split(PktList *list, PktList *list2,
              PktHandle *entry)
{
   PktHandle *curEntry = entry;
   PktHandle *nextEntry;

   ASSERT(list);
   ASSERT(list2);
   ASSERT(entry);

   PktList_Init(list2);
   while (curEntry != NULL) {
      nextEntry = PktList_GetNext(list, (PktHandle *)curEntry);
      PktList_Remove(list, (PktHandle *)curEntry);
      PktList_AddToHead(list2, (PktHandle *)curEntry);
      curEntry = nextEntry;
   }
}



/*
 *----------------------------------------------------------------------------
 *
 * PktList_CloneN --
 *
 *    Clone each packet in srcList, up to limit.
 *
 * Results:
 *    VMK_ReturnStatus, if VMK_OK, dstList contains limit clones.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE VMK_ReturnStatus
PktList_CloneN(PktList *srcList, PktList *dstList, uint32 limit)
{
   PktHandle *cloneEntry;
   List_Links *origEntry;
   VMK_ReturnStatus status = VMK_OK;

   if (UNLIKELY(VMK_STRESS_DEBUG_COUNTER(NET_PKTLIST_CLONE_FAIL))) {
      return VMK_FAILURE;
   }
   
   PktList_Init(dstList);
   LIST_FORALL(&srcList->pktList, origEntry) {
      if (UNLIKELY(PktList_Count(dstList) == limit)) {
         break;
      }
      cloneEntry = Pkt_Clone((PktHandle *)origEntry);
      if (cloneEntry) {
         PktList_AddToTail(dstList, cloneEntry);
      } else {
         PktList_ReleaseAll(dstList);
         // XXX should propagate status up through Pkt_Clone()
         status = VMK_NO_RESOURCES; 
         break;
      }
   }

   return status;
}


/*
 *----------------------------------------------------------------------------
 *
 * PktList_Clone --
 *
 *    Clone each packet in srcList.
 *
 * Results:
 *    VMK_ReturnStatus.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE VMK_ReturnStatus
PktList_Clone(PktList *srcList, PktList *dstList)
{
   return PktList_CloneN(srcList, dstList, -1);
}


/*
 *----------------------------------------------------------------------------
 *
 * PktList_Copy --
 *
 *    Create a copy of the given list.
 *
 * Results:
 *    A copy of the given list.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE PktList *
PktList_Copy(PktList *list)
{
   PktHandle *copy;
   List_Links *origEntry;
   PktList *copyList = Mem_Alloc(sizeof(PktList));

   if (UNLIKELY(VMK_STRESS_DEBUG_COUNTER(NET_PKTLIST_COPY_FAIL))) {
      if (copyList) {
         Mem_Free(copyList);
         copyList = NULL;
      }
   }
   
   if (copyList) {
      PktList_Init(copyList);
      LIST_FORALL(&list->pktList, origEntry) {
         copy = Pkt_CopyWithDescriptor((PktHandle *)origEntry);
         if (copy) {
            PktList_AddToHead(copyList, copy);
         } else {
            PktList_ReleaseAll(copyList);
            copyList = NULL;
            break;
         }
      }
   } else {
      /* failed to allocate memory for list */
   }
   return copyList;
}

#ifdef NO_PKTLIST_INLINE
#undef INLINE
#define INLINE inline
#endif

#endif //_PKTLIST_H_
