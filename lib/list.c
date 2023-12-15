
/* 
 * List_Init.c --
 *
 *	Source code for the List_Init library procedure.
 *
 * Copyright 1988 Regents of the University of California
 * Permission to use, copy, modify, and distribute this
 * software and its documentation for any purpose and without
 * fee is hereby granted, provided that the above copyright
 * notice appear in all copies.  The University of California
 * makes no representations about the suitability of this
 * software for any purpose.  It is provided "as is" without
 * express or implied warranty.
 */

#include "vmware.h"
#include "list.h"

/*
 * ----------------------------------------------------------------------------
 *
 * List_Init --
 *
 *	Initialize a header pointer to point to an empty list.  The List_Links
 *	structure must already be allocated.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The header's pointers are modified to point to itself.
 *
 * ----------------------------------------------------------------------------
 */
void
List_Init(List_Links *headerPtr)
{
    if (headerPtr == NULL) {
	Panic("List_Init: invalid header pointer.\n");
    }
    headerPtr->nextPtr = headerPtr;
    headerPtr->prevPtr = headerPtr;
}

/*
 * ----------------------------------------------------------------------------
 *
 * List_Insert --
 *
 *	Insert the list element pointed to by itemPtr into a List after 
 *	destPtr.  Perform a primitive test for self-looping by returning
 *	failure if the list element is being inserted next to itself.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The list containing destPtr is modified to contain itemPtr.
 *
 * ----------------------------------------------------------------------------
 */
void
List_Insert(List_Links *itemPtr, List_Links *destPtr)
{
    if (itemPtr == NULL || destPtr == NULL) {
	Panic("List_Insert: itemPtr or destPtr is NULL.\n");
	return;
    }
    if (itemPtr == destPtr) {
	Panic("List_Insert: trying to insert something after itself.\n");
	return;
    }
    itemPtr->nextPtr = destPtr->nextPtr;
    itemPtr->prevPtr = destPtr;
    destPtr->nextPtr->prevPtr = itemPtr;
    destPtr->nextPtr = itemPtr;
}


/*
 * ----------------------------------------------------------------------------
 *
 * List_Remove --
 *
 *	Remove a list element from the list in which it is contained.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The given structure is removed from its containing list.
 *
 * ----------------------------------------------------------------------------
 */
void
List_Remove(List_Links *itemPtr)
{
    if (itemPtr == NULL || !itemPtr || itemPtr == itemPtr->nextPtr) {
	Panic("List_Remove: invalid item to remove: itemPtr = %p\n", itemPtr);
    }
    if (itemPtr->prevPtr->nextPtr != itemPtr ||
	itemPtr->nextPtr->prevPtr != itemPtr) {
	Panic("List_Remove: item's pointers are invalid.\n");
    }
    itemPtr->prevPtr->nextPtr = itemPtr->nextPtr;
    itemPtr->nextPtr->prevPtr = itemPtr->prevPtr;
}
