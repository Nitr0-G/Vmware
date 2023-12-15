/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. 
 * **********************************************************/

/*
 * vmktag_dist.h --
 *
 *	debug tagging util
 */

#ifndef _VMKTAG_DIST_H_
#define _VMKTAG_DIST_H_

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_DISTRIBUTE
#define INCLUDE_ALLOW_VMK_MODULE
#include "includeCheck.h"

#include "splock.h"

#ifdef VMX86_DEBUG
#define VMKTAGS_ENABLED
#endif // VMX86_DEBUG

#ifdef VMKTAGS_ENABLED

/*
 * This is a set of macros to do simple tagging of structs as they 
 * change state and/or are passed around to different functions and/or
 * modules.  All macros compile out to nothing unless VMKTAGS_ENABLED
 * is defined.
 *
 * The tagged structs are kept in a list which can be dumped from
 * /proc/vmware/VmkTag_<list>/list. Counts for each tag are also 
 * maintained and can be dumped from /proc/vmware/VmkTag_<list>/counts.  
 * Counts maintained per tag: 
 *
 *       current  - the current number of "active" structs with 
 *                  the tag marked
 *
 *       single   - the number times the tag has been marked 
 *
 *       multiple - the number of times the tag was re-marked 
 *                  (ie already marked)
 * 
 *
 * VMKTAG_HOOK(list)
 *
 *    Declare a tag hook in a struct which is to be tagged, this will
 *    reserve enough space for implementation data and for the number of 
 *    defined tag bits for the given list.
 * 
 *
 * VMKTAG_BEGIN(list, structPtr)
 *    
 *    "Activate" the tags for structPtr so structPtr's tags will show
 *    up in list's proc node, all tag marks are cleared.
 *
 *
 * VMKTAG_END(structPtr)
 *
 *     Deactivate structPtr's tags so they will no longer show up in the 
 *     the proc node.  structPtr will be removed from it's tag list.
 *     For all tags marked on structPtr, the "current" counter is decremented.
 *
 *
 * VMKTAG_MARK(structPtr, tag)
 *
 *     Mark tag on structPtr.  If the tag is already marked the "multiple" 
 *     counter is incremented, otherwise the "single" and "current" counters 
 *     are incremented.
 *
 *
 * VMKTAG_IS_MARKED(structPtr, tag)
 *
 *     TRUE iff tag is marked on structPtr.
 * 
 *
 * VMKTAG_LOG(structPtr)
 *
 *     Print the tags marked on structPtr to the log
 *
 *
 * VMKTAG_ONLY(x)
 *
 *     do/don't compile x dependant on VMKTAGS_ENABLED
 *
 * 
 *
 * Creating new tags and taglists:
 *
 * Add an entry to VMKTAG_LIST_DEFS for each type of struct 
 * you want to be able to tag then put VMKTAG_HOOK(<entry>); 
 * in the struct definition to get the tag hook conditionally 
 * compiled in.  For example, for the PKT list, I did:
 *
 * typedef struct NetPktMemHdr {
 *    // ... existing struct fields ...
 *    VMKTAG_HOOK(PKT); // space for tags if enabled
 * } NetPktMemHdr;
 *
 * Then create a list of the tag names you want to use as in 
 * PKT_VMKTAG_DEFS below.  Now you can use the above listed 
 * macros on all the structs of the type you declared the
 * tag hook in.
 *
 */

#define VMKTAG_LIST_DEFS                         \
   VMKTAG_LIST_DECL(PKT)                         \


#define PKT_VMKTAG_DEFS                          \
   VMKTAG_DECL(PKT_ALLOC)                        \
   VMKTAG_DECL(PKT_ALLOC_PRIV)                   \
   VMKTAG_DECL(PKT_DO_ALLOC_SKB)                 \
   VMKTAG_DECL(PKT_DO_TRANSMIT)                  \
   VMKTAG_DECL(PKT_TOE_HARD_TRANSMIT)            \
   VMKTAG_DECL(PKT_HARD_TRANSMIT)                \
   VMKTAG_DECL(PKT_LOCAL_TRANSMIT)               \
   VMKTAG_DECL(PKT_COPY_PACKET)                  \
   VMKTAG_DECL(PKT_LOCAL_TOE)                    \
   VMKTAG_DECL(PKT_TX_FAIL_OR_LOCAL)             \
   VMKTAG_DECL(PKT_TX_CLEAR_NOTIFY)              \
   VMKTAG_DECL(PKT_START_XMIT)                   \
   VMKTAG_DECL(PKT_HARD_START_XMIT)              \
   VMKTAG_DECL(PKT_NETIF_RX)                     \
   VMKTAG_DECL(PKT_QUEUE_RX)                     \
   VMKTAG_DECL(PKT_QUEUE_BH)                     \
   VMKTAG_DECL(PKT_RX)                           \
   VMKTAG_DECL(PKT_RX_UNICAST)                   \
   VMKTAG_DECL(PKT_RX_MULTICAST)                 \
   VMKTAG_DECL(PKT_RX_BROADCAST)                 \
   VMKTAG_DECL(PKT_RX_PROMISC)                   \
   VMKTAG_DECL(PKT_RX_EXCLUSIVE)                 \
   VMKTAG_DECL(PKT_DO_RX)                        \
   VMKTAG_DECL(PKT_DO_RX_NOT_ENBL)               \
   VMKTAG_DECL(PKT_APPEND)                       \
   VMKTAG_DECL(PKT_KFREE_SKB)                    \
   VMKTAG_DECL(PKT_RETURN_XMIT)                  \
   VMKTAG_DECL(PKT_FREE_FUNC)                    \
   VMKTAG_DECL(PKT_FREE_TXRET)                   \
   VMKTAG_DECL(PKT_FREE)                         \
   VMKTAG_DECL(PKT_VLAN_XMIT_SW_TAGGING)         \
   VMKTAG_DECL(PKT_VLAN_XMIT_HW_TAGGING)         \
   VMKTAG_DECL(PKT_VLAN_RECV_NO_VLAN_HDR)        \
   VMKTAG_DECL(PKT_VLAN_RECV_SW_UNTAGGING)       \
   VMKTAG_DECL(PKT_VLAN_RECV_ON_NO_VID_SUPPORT)  \
   VMKTAG_DECL(PKT_VLAN_RECV_NON_VLAN_HANDLE)    \
   VMKTAG_DECL(PKT_VLAN_RECV_NO_VLAN_CAPABILITY) \
   VMKTAG_DECL(PKT_VLAN_RECV_VID_MISMATCH)       \
   VMKTAG_DECL(PKT_VLAN_RECV_VID_HW_ACCCEL)      \
   VMKTAG_DECL(PKT_VLAN_RECV_VID_SW_UNTAG)       \
   VMKTAG_DECL(PKT_VLAN_HANDLE_NO_VID)           \
   VMKTAG_DECL(PKT_NON_VLAN)                     \
   VMKTAG_DECL(PKT_NICTEAMING_BEACON)            \


/*********************************************************************************
 * you probably don't need to change anything below here.
 ********************************************************************************/

#define VMKTAG_HOOK(_l)             _l##VmkTagHook _vmkTagHook
#define VMKTAG_BEGIN(_l, _x)        VmkTag_Begin(VMKTAG_LIST_##_l, &((_x)->_vmkTagHook.th))
#define VMKTAG_END(_x)              VmkTag_End(&((_x)->_vmkTagHook.th))
#define VMKTAG_MARK(_x, _t)         VmkTag_Mark(&((_x)->_vmkTagHook.th), (VMKTAG_##_t))
#define VMKTAG_IS_MARKED(_x, _t)    VmkTag_IsMarked(&((_x)->_vmkTagHook.th), (VMKTAG_##_t))
#define VMKTAG_LOG(_x)              VmkTag_Log(&((_x)->_vmkTagHook.th))
#define VMKTAG_ONLY(_x)             _x

#define ffs(_x) __builtin_ffs(_x)

typedef struct VmkTagHook {
#define VMKTAG_MAGIC 0xacdc
   uint16            magic;
   uint16            list;
   struct VmkTagHook *next;
   struct VmkTagHook *prev;
} VmkTagHook;


typedef struct VmkTagList {
   char              *name;
   uint32            numTags;
   char              **tagNames;
   VmkTagHook        *head;
   uint32            *tagCountsTot;
   uint32            *tagCountsCur;
   uint32            *tagCountsMul;
   SP_SpinLockIRQ    lock;
   Proc_Entry        procDirEntry;
   Proc_Entry        procCountsEntry;
   Proc_Entry        procListEntry;
} VmkTagList;


/**************************************************************************
 *
 * generate enum of all VmkTagListNums
 */
#define VMKTAG_LIST_DECL(_tl) VMKTAG_LIST_##_tl,
typedef enum VmkTagListNum {
   VMKTAG_LIST_DEFS
   VMKTAG_NUM_LISTS
} VmkTagListNum;
#undef VMKTAG_LIST_DECL

/**************************************************************************
 *
 * generate XXXTagHook struct defs and XXXVmkTag enums for each VmkTagList
 */

// enum of a set of vmktags
#define VMKTAG_DECL(_t) VMKTAG_##_t,
#define VMKTAG_ENUM(_tl)     \
typedef enum _tl##VmkTag {   \
   _tl##_VMKTAG_DEFS         \
   VMKTAG_NUM_##_tl##_TAGS   \
} _tl##VmkTag;               \

#define VMKTAG_ARRAY_SIZE(_tl) ((VMKTAG_NUM_##_tl##_TAGS + 31) / 32)

// typedef for the hooks for a set of vmktags
#define VMKTAG_HOOK_STRUCT(_tl)                     \
typedef struct _tl##VmkTagHook {                    \
   VmkTagHook  th;                                  \
   uint32      vmkTag[VMKTAG_ARRAY_SIZE(_tl)];      \
} _tl##VmkTagHook;                                  

// do both of the above for a set of vmktags
#define VMKTAG_LIST_DECL(_tl)    \
   VMKTAG_ENUM(_tl)              \
   VMKTAG_HOOK_STRUCT(_tl)

// do the above for all the sets of vmktags
VMKTAG_LIST_DEFS

#undef VMKTAG_DECL
#undef VMKTAG_LIST_DECL
#undef VMKTAG_ARRAY_SIZE

/**************************************************************************/



#define VMKTAG_ARRAY_SIZE(_tl) ((_tl->numTags + 31) / 32)

extern VmkTagList vmkTagLists[];

// not an inline bc we don't want a fn pointer for _x
#define VMKTAG_FOREACH(_th, _x)                        \
{                                                      \
   uint32 _i, _bit;                                    \
   VmkTagList *_tl = &vmkTagLists[_th->list];          \
   uint32 *_tags = (uint32 *)(_th + 1);                \
                                                       \
   for (_i = 0; _i < VMKTAG_ARRAY_SIZE(_tl); _i++) {   \
      uint32 _bits = _tags[_i];                        \
      while((_bit = ffs(_bits)) != 0) {                \
         uint32 _t;                                    \
                                                       \
         _bit--;                                       \
         _t = (_i << 5) + _bit;                        \
         _x; /* do whatever */                         \
         _bits &= ~(1 << _bit);                        \
      }                                                \
   }                                                   \
}                    

void VmkTag_Log(VmkTagHook *th);
void VmkTag_LogList(VmkTagHook *list);
void VmkTag_Init(void);

static INLINE void
VmkTag_Begin(VmkTagListNum list, VmkTagHook *th)
{
   int i;
   VmkTagList *tl = &vmkTagLists[list];
   uint32 *tags = (uint32 *)(th + 1);
   th->magic = VMKTAG_MAGIC;

   for (i = 0; i < VMKTAG_ARRAY_SIZE(tl); i++) {
      tags[i] = 0;
   }

   th->prev = NULL;

   SP_LockIRQ(&tl->lock, SP_IRQL_KERNEL);
   th->next = tl->head;
   tl->head = th;
   if (th->next) {
      th->next->prev = th;
   }
   SP_UnlockIRQ(&tl->lock, SP_GetPrevIRQ(&tl->lock));
}

static INLINE void 
VmkTag_End(VmkTagHook *th)
{
   VmkTagList *tl = &vmkTagLists[th->list];

   ASSERT(th->magic == VMKTAG_MAGIC);
   th->magic = 0;

   SP_LockIRQ(&tl->lock, SP_IRQL_KERNEL);

   // remove these tags from current count
   VMKTAG_FOREACH(th, tl->tagCountsCur[_t]--);  

   if (th->next) {
      th->next->prev = th->prev;
   }   
   if (th->prev) {
      ASSERT(th != tl->head);
      th->prev->next = th->next;
   } else {
      ASSERT(th == tl->head);
      tl->head = th->next;
   }

   SP_UnlockIRQ(&tl->lock, SP_GetPrevIRQ(&tl->lock));
}

static INLINE Bool
VmkTag_IsMarked(VmkTagHook *th, uint32 tag)
{
   uint32 *tags = (uint32 *)(th + 1);
   
   ASSERT(th->magic == VMKTAG_MAGIC);

   return ((tags[tag >> 5] & (1 << (tag & 31))) != 0);
}

static INLINE void 
VmkTag_Mark(VmkTagHook *th, uint32 tag)
{
   uint32 *tags = (uint32 *)(th + 1);
   VmkTagList *tl = &vmkTagLists[th->list];

   ASSERT_BUG(38592, (th->magic == VMKTAG_MAGIC));

   SP_LockIRQ(&tl->lock, SP_IRQL_KERNEL);
   
   if (!VmkTag_IsMarked(th, tag)) {
      tags[tag >> 5] |= (1 << (tag & 31));
      tl->tagCountsCur[tag]++;
      tl->tagCountsTot[tag]++;
   } else {
      tl->tagCountsMul[tag]++;
   }
   
   SP_UnlockIRQ(&tl->lock, SP_GetPrevIRQ(&tl->lock));
}

#else // ! VMKTAGS_ENABLED

#define VMKTAG_HOOK(_l)             
#define VMKTAG_BEGIN(_l, _x)        
#define VMKTAG_END(_x)              
#define VMKTAG_MARK(_x, _t)         
#define VMKTAG_IS_MARKED(_x, _t)    
#define VMKTAG_LOG(_x)              
#define VMKTAG_ONLY(_x)             

#define VmkTag_Init()

#endif // (!) VMKTAGS_ENABLED

#endif // _VMKTAG_DIST_H_

