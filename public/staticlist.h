/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * unorderedlist.h --
 *
 *	Other c/h files should #include this file, possibly multiple
 *      times, to generate functions and structures for unordered,
 *      statically allocated lists. Because these are statically allocated,
 *      they obviously can't grow beyond the predefined LIST_SIZE items.
 *      After the desired code is generated, all of the preprocessor parameters
 *      will be undef'ed to clean up.
 *
 *      You MUST #define the following parameters before including:
 *         LIST_ITEM_TYPE   -- the type of item being stored in the list
 *         LIST_NAME        -- this will be the typedef'ed name of the generated struct
 *         LIST_SIZE        -- the maximum size of the list
 *
 *      You MAY optionally #define the following parameters:
 *         LIST_EQUALITY_OP -- a function "Bool equalityop(LIST_ITEM_TYPE A, LIST_ITEM_TYPE B)" that
 *                             returns a nonzero value iff (A == B). If this variable is undefined,
 *                             the == operator is used as the equality op.
 *         LIST_IDX_FIELD   -- a stucture field within *LIST_ITEM_TYPE that should hold the index
 *                             of this item in the list. Whenever an item has its index changed,
 *                             item->LIST_IDX_FIELD is guaranteed to be updated with the new value.
 *                             If this variable is undefined, no field will be updated and you
 *                             will need to use RemoveByData to remove items from the list.
 *         LIST_NULL_ITEM   -- when an item is removed from the list, the last slot in the list
 *                             will be filled with LIST_NULL_ITEM, which defaults to NULL.
 *
 *      This will generate the following functions:
 *         LIST_NAMEAdd(LIST_NAME *list, LIST_ITEM_TYPE item):
 *                             Appends "item" to "list", possibly updating its LIST_IDX_FIELD with
 *                             its new position in the list.
 *
 *         LIST_NAMERemoveByIndex(LIST_NAME *list, index index):
 *                             Removes the item at position "index" from "list", swapping the last
 *                             item of the list into the hole.
 *
 *         LIST_NAMERemoveByData(LIST_NAME *list, LIST_ITEM_TYPE data):
 *                             Removes ALL items in list for which (LIST_EQUALITY_OP(item, data) != 0)
 *
 *         LIST_NAMERemove(LIST_NAME *list, LIST_ITEM_TYPE data):
 *                             Only generated if LIST_IDX_FIELD is defined. Expands to
 *                             LIST_NAMERemoveByIndex(list, data->LIST_IDX_FIELD)
 *
 *
 *      It will also generate an anonymous struct, typedef-aliased to "LIST_NAME", with the fields:
 *         int len                          -- current number of items in the list
 *         LIST_ITEM_TYPE list[LIST_SIZE]   -- holds the actual entries of the list.
 */

// after an item is removed from the list, we'll set its LIST_IDX_FIELD to "INVALID_INDEX"
#ifndef INVALID_INDEX
#define INVALID_INDEX (-1)
#endif

#ifndef LIST_NULL_ITEM
#define LIST_NULL_ITEM (NULL)
#endif

// default equality operator
#ifndef LIST_EQUALITY_OP
#define LIST_EQUALITY_OP(A,B) (A == B)
#endif

// generate the structure definition
typedef struct {                                        
   LIST_ITEM_TYPE list[LIST_SIZE];                                     
   int len;                                             
} LIST_NAME ;                                                


/* Generate the functions. See file header for documentation. */

static INLINE void                                      
XCONC(LIST_NAME, Add)(LIST_NAME *list, LIST_ITEM_TYPE item)                      
{                                                       
   ASSERT(list->len < LIST_SIZE);
   /* place item in next array slot */                  
   list->list[list->len] = item;
#ifdef LIST_IDX_FIELD
   item->LIST_IDX_FIELD = list->len;
#endif
   list->len++;                                         
}                                                       
                                                        
static INLINE void                                      
XCONC(LIST_NAME, RemoveByIndex)(LIST_NAME *list, int index)            
{                                                       
   LIST_ITEM_TYPE swap;                                           
   LIST_ITEM_TYPE prev;                                           
   ASSERT(index < LIST_SIZE);                                
   ASSERT(index != INVALID_INDEX);
   
   /* remove item, compact by swapping last element into "hole" */ 
   swap = list->list[list->len - 1];                    
   prev = list->list[index];                            
   list->list[index] = swap;                            
                                                        
#ifdef LIST_IDX_FIELD
   swap->LIST_IDX_FIELD = index;                     
   prev->LIST_IDX_FIELD = INVALID_INDEX;                 
#endif                                                  
   /* shrink array */                                   
   list->len--;
   list->list[list->len] = LIST_NULL_ITEM;
}

static INLINE void
XCONC(LIST_NAME, RemoveByData)(LIST_NAME *list, LIST_ITEM_TYPE data)
{
   int i=0;
   while (i < list->len) {
      if (LIST_EQUALITY_OP(list->list[i], data)) {
         XCONC(LIST_NAME, RemoveByIndex)(list, i);
         // somebody new has been swapped in here, so retry this slot
      } else {
         i++;
      }
   }
}

#ifdef LIST_IDX_FIELD
static INLINE void
XCONC(LIST_NAME, Remove)(LIST_NAME *list, LIST_ITEM_TYPE data)
{
   ASSERT(LIST_EQUALITY_OP(list->list[data->LIST_IDX_FIELD], data));
   XCONC(LIST_NAME, RemoveByIndex)(list, data->LIST_IDX_FIELD);
}
#endif

#undef LIST_IDX_FIELD
#undef LIST_ITEM_TYPE
#undef LIST_NAME
#undef LIST_SIZE
#undef LIST_EQUALITY_OP
#undef LIST_NULL_ITEM
