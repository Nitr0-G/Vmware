/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. 
 * **********************************************************/

/*
 * vmktag.c --
 *
 *	debug tagging util
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "vmkernel.h"
#include "proc.h"
#include "memalloc.h"
#include "vmktag_dist.h"

#ifdef VMKTAGS_ENABLED

#define LOGLEVEL_MODULE VmkTag
#define LOGLEVEL_MODULE_LEN 6
#include "log.h"


/*
 * generate tag name array for each list
 */
#define VMKTAG_DECL(_t) #_t,
#define VMKTAG_LIST_DECL(_tl) \
      char *VmkTag_##_tl##_Names[] = { _tl##_VMKTAG_DEFS };
VMKTAG_LIST_DEFS
#undef VMKTAG_DECL
#undef VMKTAG_LIST_DECL

/*
 * generate an array of VmkTagList structs
 */
#define VMKTAG_LIST_DECL(_tl) {    \
   #_tl,                           \
   VMKTAG_NUM_##_tl##_TAGS,        \
   VmkTag_##_tl##_Names            \
},                         
VmkTagList vmkTagLists[] = { VMKTAG_LIST_DEFS };
#undef VMKTAG_DECL
#undef VMKTAG_LIST_DECL

void 
VmkTag_Log(VmkTagHook *th)
{
   VmkTagList *tl = &vmkTagLists[th->list];

   ASSERT(th->magic == VMKTAG_MAGIC);

   VMKTAG_FOREACH(th, _Log("%s ", tl->tagNames[_t]));
   _Log("\n");
}

static int
VmkTagProcListRead(Proc_Entry  *entry,
                   char        *page,
                   int         *len)
{
   VmkTagHook *th;
   VmkTagList *tl = (VmkTagList *)entry->private;

   *len = 0;
   
   SP_LockIRQ(&tl->lock, SP_IRQL_KERNEL);
   for (th = tl->head; th != NULL; th = th->next) {
      VMKTAG_FOREACH(th, Proc_Printf(page, len, "%s ", tl->tagNames[_t]));
      Proc_Printf(page, len, "\n");
   }
   SP_UnlockIRQ(&tl->lock, SP_GetPrevIRQ(&tl->lock));

   return VMK_OK;
}

static int
VmkTagProcCountsRead(Proc_Entry  *entry,
                     char        *page,
                     int         *len)
{
   int t;
   VmkTagList *tl = (VmkTagList *)entry->private;

   *len = 0;

   Proc_Printf(page, len, "%-40s %10s %10s %10s\n\n", 
               "tag name", 
               "current", 
               "single", 
               "multiple"); 
   
   for (t = 0; t < tl->numTags; t++) { 
      Proc_Printf(page, len, "%-40s %10u %10u %10u\n", 
                  tl->tagNames[t], 
                  tl->tagCountsCur[t], 
                  tl->tagCountsTot[t], 
                  tl->tagCountsMul[t]);
   }

   return VMK_OK;
}

static int
VmkTagProcCountsWrite(Proc_Entry  *entry,
                      char        *page,
                      int         *len)
{
   VmkTagList *tl = (VmkTagList *)entry->private;

   if (strncmp(page, "reset", 5) == 0) {
      memset(tl->tagCountsTot, 0, tl->numTags * sizeof(uint32));
      memset(tl->tagCountsMul, 0, tl->numTags * sizeof(uint32));
   }

   return VMK_OK;
}

void
VmkTag_Init(void)
{
   int i;

   for (i = 0; i < VMKTAG_NUM_LISTS; i++) {
      VmkTagList *tl = &vmkTagLists[i];
      char buf[40];

      snprintf(buf, sizeof(buf), "VmkTag_%s", tl->name);

      SP_InitLockIRQ(buf, &tl->lock, SP_RANK_VMKTAG);

      tl->tagCountsTot = Mem_Alloc(tl->numTags * sizeof(uint32));
      tl->tagCountsCur = Mem_Alloc(tl->numTags * sizeof(uint32));
      tl->tagCountsMul = Mem_Alloc(tl->numTags * sizeof(uint32));
      
      ASSERT(tl->tagCountsTot && tl->tagCountsCur && tl->tagCountsMul);

      memset(tl->tagCountsTot, 0, tl->numTags * sizeof(uint32));
      memset(tl->tagCountsCur, 0, tl->numTags * sizeof(uint32));
      memset(tl->tagCountsMul, 0, tl->numTags * sizeof(uint32));

      Proc_InitEntry(&tl->procDirEntry);
      tl->procCountsEntry.private = tl;      
      Proc_RegisterHidden(&tl->procDirEntry, buf, TRUE);

      Proc_InitEntry(&tl->procCountsEntry);
      tl->procCountsEntry.parent = &tl->procDirEntry;
      tl->procCountsEntry.write = VmkTagProcCountsWrite;
      tl->procCountsEntry.read = VmkTagProcCountsRead;      
      tl->procCountsEntry.private = tl;      
      Proc_RegisterHidden(&tl->procCountsEntry, "counts", FALSE);

      Proc_InitEntry(&tl->procListEntry);
      tl->procListEntry.parent = &tl->procDirEntry;
      tl->procListEntry.read = VmkTagProcListRead;      
      tl->procListEntry.private = tl;      
      Proc_RegisterHidden(&tl->procListEntry, "list", FALSE);
   }
}

#endif // VMKTAGS_ENABLED
