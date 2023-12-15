/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

#include "vm_types.h"
#include "vm_libc.h"
#include "vm_asm.h"
#include "x86.h"
#include "vmkernel.h"
#include "vmnix_if.h"
#include "memmap.h"
#include "world.h"
#include "host.h"
#include "util.h"
#include "memalloc.h"
#include "vmkstats.h"
#include "list.h"
#include "vmnix_syscall.h"
#include "helper.h"
#include "mod_loader.h"
#include "vmk_scsi.h"
#include "vmkevent.h"
#include "bluescreen.h"
#include "heap_public.h"
#include "statusterm.h"

#define LOGLEVEL_MODULE Mod
#define LOGLEVEL_MODULE_LEN 3
#include "log.h"

typedef int (*ModFunc)(void);

typedef struct ModuleSymbol {
   struct ModuleSymbol	*nextInList;
   struct ModuleSymbol	*nextInHash;

   char 		*name;
   uint32		value;
   uint32		size;
   int			info;
   int			moduleID;
   int			symbolNum;
} ModuleSymbol;

static ModuleSymbol		*symbolList;	// symbols exported by modules
static ModuleSymbol		*localSymbolList; // un-exported symbols
static uint32			curSymbolNum = 1;
static ModuleSymbol		*nextSymbol;

#define	MAX_SYMBOL_INFO_SIZE	512 * 1024
#define SYMBOL_HASH_TABLE_SIZE	1024

static ModuleSymbol	*symbolHashTable[SYMBOL_HASH_TABLE_SIZE];

typedef struct ModuleSymbolMemInfo {
   char			*bufferStart;
   char 		*bufferNext;
   int32		bufferLength;
   int32		numSymbols;
   int32		maxSymbols;
} ModuleSymbolMemInfo;

typedef struct ModuleInfo {
   List_Links		links;

   uint32 		id;
   void			*privID; // whatever the module code uses to id itself
   void			*readOnlyBaseAddr;
   void			*writableBaseAddr;
   uint32		readOnlyLength;
   uint32		writableLength;
   Bool			loaded;
   Bool			inList;
   Bool			symbolsPresent;// TRUE if module has exported symbols
   ModuleSymbolMemInfo	symMemInfo;
   ModFunc		initFunc;
   ModFunc		cleanupFunc;
   ModFunc		earlyInitFunc;
   ModFunc		lateCleanupFunc;
   ModLoadCBFunc	preUnloadFunc;
   void			*preUnloadFuncData;
   ModLoadCBFunc	postInitFunc;        // used if initialization succeeds
   void			*postInitFuncData;
   ModLoadCBFunc	postInitFailureFunc; // used if initialization fails
   void			*postInitFailureData;
   ModDevCBFunc		postInsertFunc;
   ModDevCBFunc		preRemoveFunc;
   int32		useCount;
   char			modName[VMNIX_MODULE_NAME_LENGTH];
   VA			textBase;
   VA			dataBase;
   VA			bssBase;
   char                 versionInfo[MOD_VERSION_STRING_LENGTH];
   Heap_ID		heap;
} ModuleInfo;

static ModuleSymbolMemInfo	modSymMemInfo;
static List_Links 		moduleList;
static int 			nextModuleID = 1;
static SP_SpinLockIRQ 		modLock;

static uint32 NameHash(const char *name);
static ModuleSymbol *ModSymbolHTFind(const char *name);
static void ModSymbolHTAdd(ModuleSymbol *symbol);
static void ModSymbolHTRemove(ModuleSymbol *symbol);
static void ModAddLocalSymbol(char *name, uint32 value, uint32 moduleID,
			      ModuleSymbol *symbol);
static VMK_ReturnStatus ModAllocSymbolMem(VMnix_SymArgs *args, 
					  ModuleSymbolMemInfo *symMemInfo);
void
Mod_Init(void)
{
   List_Init(&moduleList);
   SP_InitLockIRQ("ModLock", &modLock, SP_RANK_MODLOCK);
}


ModuleInfo *
ModFind(int moduleID)
{
   List_Links *elt;

   LIST_FORALL(&moduleList, elt) {
      ModuleInfo *mi = (ModuleInfo *)elt;

      if (mi->id == moduleID) {
	 return (ModuleInfo *)mi;
      }
   }
   return NULL;
}


/*
 *----------------------------------------------------------------------
 *
 * ModAddrOutsideRO --
 *
 *      Checks to see if a given address is outside of the read only region as
 *      described by the passed-in ModuleInfo.
 *
 * Results:
 *      Bool. True if outside RO region, False if inside RO region.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Bool
ModAddrOutsideRO(ModuleInfo *m, char *addr)
{
   return (addr < (char *)m->readOnlyBaseAddr ||
           addr >= (char *)m->readOnlyBaseAddr + m->readOnlyLength);
}

Bool
Mod_GetName(int moduleID, char *modName)
{
   SP_IRQL prevIRQL;
   ModuleInfo *mi;
   Bool success = TRUE;

   prevIRQL = SP_LockIRQ(&modLock, SP_IRQL_KERNEL);

   mi = ModFind(moduleID);
   if (mi == NULL) {
      modName[0] = '\0';
      success = FALSE;
   } else {
      memcpy(modName, mi->modName, VMNIX_MODULE_NAME_LENGTH);
   }
      
   SP_UnlockIRQ(&modLock, prevIRQL);

   return success;
}


VMK_ReturnStatus
Mod_Alloc(VMnix_ModAllocArgs *args, VMnix_ModAllocResult *result)
{
   ModuleInfo *mi = NULL;
   List_Links *elt;
   VMK_ReturnStatus status = VMK_OK;   
   SP_IRQL prevIRQL;

   StatusTerm_Printf("Loading module %s ...\n", args->modName);

   memset(result, 0, sizeof(*result));

   prevIRQL = SP_LockIRQ(&modLock, SP_IRQL_KERNEL);

   LIST_FORALL(&moduleList, elt) {
      mi = (ModuleInfo *)elt;

      if (strcmp(args->modName, mi->modName) == 0) {
	 status = VMK_BUSY;
	 break;
      }
   }

   Log("Starting load for module: %s R/O length: 0x%lx R/W length: 0x%lx",
       args->modName, args->moduleReadOnlySize, 
       args->moduleWritableSize);
   if (status == VMK_OK) {
      result->readOnlyLoadAddr = MemRO_Alloc(args->moduleReadOnlySize);
      result->writableLoadAddr = Mem_Alloc(args->moduleWritableSize);
      if ((result->readOnlyLoadAddr == NULL && args->moduleReadOnlySize != 0) || 
          (result->writableLoadAddr == NULL && args->moduleWritableSize != 0)) {
	 Warning("Less than %ld bytes free to load module",
	         args->moduleReadOnlySize + args->moduleWritableSize);
	 status = VMK_NO_RESOURCES;
      } else {
	 if (args->moduleReadOnlySize != 0) {
	    MemRO_ChangeProtection(MEMRO_WRITABLE);
	    memset(result->readOnlyLoadAddr, 0, args->moduleReadOnlySize);
	    MemRO_ChangeProtection(MEMRO_READONLY);
	 }
	 if (args->moduleWritableSize != 0) {
	    memset(result->writableLoadAddr, 0, args->moduleWritableSize);
	 }
	 mi = (ModuleInfo *)Mem_Alloc(sizeof(ModuleInfo));
	 if (mi == NULL) {
	    Warning("Couldn't alloc module info struct");
	    status = VMK_NO_RESOURCES;
	 }
      }
   }

   if (status != VMK_OK) {
      if (result->readOnlyLoadAddr != NULL) {
	 MemRO_Free(result->readOnlyLoadAddr);
      }
      if (result->writableLoadAddr != NULL) {
	 Mem_Free(result->writableLoadAddr);
      }
   } else {
      memset(mi, 0, sizeof(*mi));
      mi->id = nextModuleID++;
      mi->readOnlyBaseAddr = result->readOnlyLoadAddr;
      mi->writableBaseAddr = result->writableLoadAddr;
      mi->readOnlyLength = args->moduleReadOnlySize;
      mi->writableLength = args->moduleWritableSize;
      strcpy(mi->modName, args->modName);

      result->moduleID = mi->id;

      List_Insert(&mi->links, LIST_ATREAR(&moduleList));
      mi->inList = TRUE;
      status = VMK_OK;
   }

   SP_UnlockIRQ(&modLock, prevIRQL);

   return status;
}

/*
 * Free up module information, including code/data space and
 * local and exported symbols
 */
static void
ModFree(ModuleInfo *m)
{
   ModuleSymbol *curSym, *prevSym, *nextSym;

   if (m->inList) {
      List_Remove(&m->links);
   }
   if (m->symbolsPresent) {

      for (prevSym = NULL, curSym = symbolList;
	   curSym != NULL;
	   curSym = nextSym) {
	 nextSym = curSym->nextInList;
	 if (curSym->moduleID == m->id) {
	    if (prevSym == NULL) {
	       symbolList = nextSym;
	    } else {
	       prevSym->nextInList = nextSym;
	    }

	    ModSymbolHTRemove(curSym);
	 } else {
	    prevSym = curSym;
	 }
      }
      nextSymbol = NULL;
   }

   for (prevSym = NULL, curSym = localSymbolList; curSym != NULL;
	curSym = nextSym) {
      nextSym = curSym->nextInList;
      if (curSym->moduleID == m->id) {
	 if (prevSym == NULL) {
	    localSymbolList = nextSym;
	 } else {
	    prevSym->nextInList = nextSym;
	 }
      } else {
	 prevSym = curSym;
      }
   }

   if (m->symMemInfo.bufferStart != NULL) {
      Mem_Free(m->symMemInfo.bufferStart);
   }
   LOG(0, "Freeing %p, %p", m->readOnlyBaseAddr, m->writableBaseAddr);   
   if (m->readOnlyBaseAddr != NULL) { 
      MemRO_Free(m->readOnlyBaseAddr);
   }
   if (m->writableBaseAddr != NULL) {
      Mem_Free(m->writableBaseAddr);
   }
   Mem_Free(m);
}

VMK_ReturnStatus
Mod_PutPage(int moduleID, void *addr, void *data)
{
   VMK_ReturnStatus status;
   ModuleInfo *m;
   SP_IRQL prevIRQL;

   prevIRQL = SP_LockIRQ(&modLock, SP_IRQL_KERNEL);

   m = ModFind(moduleID);
   if (m == NULL) {
      status = VMK_NOT_FOUND;
   } else if (((char *)addr >= (char *)m->readOnlyBaseAddr &&
               (char *)addr + PAGE_SIZE <= (char *)m->readOnlyBaseAddr + m->readOnlyLength)) {
      MemRO_ChangeProtection(MEMRO_WRITABLE);
      memcpy(addr, data, PAGE_SIZE);
      MemRO_ChangeProtection(MEMRO_READONLY);
      status = VMK_OK;
   } else if (((char *)addr >= (char *)m->writableBaseAddr &&
               (char *)addr + PAGE_SIZE <= (char *)m->writableBaseAddr + m->writableLength)) {
      memcpy(addr, data, PAGE_SIZE);
      status = VMK_OK;	       
   } else {
      Warning("Invalid address");
      status = VMK_BAD_PARAM;
   }

   SP_UnlockIRQ(&modLock, prevIRQL);

   return status;
}

static ModuleInfo *modBeingLoaded;

VMK_ReturnStatus
Mod_LoadProbe(int moduleID)
{
   VMK_ReturnStatus status;
   ModuleInfo *m;
   SP_IRQL prevIRQL;

   prevIRQL = SP_LockIRQ(&modLock, SP_IRQL_KERNEL);

   m = ModFind(moduleID);
   if (m == NULL) {
      status = VMK_IO_ERROR;
   } else if (! m->loaded) {
      status = VMK_STATUS_PENDING;
   } else {
      status = VMK_OK;
   }

   SP_UnlockIRQ(&modLock, prevIRQL);
   return status;
}

static void
ModInitModule(void *data)
{
   int initStatus = 0, earlyInitStatus = 0;
   ModuleInfo *m;
   SP_IRQL prevIRQL;
   VmkEvent_VmkLoadModArgs arg;


   /*
    * Make sure interrupts are enabled before we load the module
    * because it may wait for an interrupt to happen before the 
    * load is successful.
    */
   ASSERT_HAS_INTERRUPTS();

   m = modBeingLoaded;
   
   Log("mainHeap avail before: %d", Mem_Avail());

   ASSERT( (m->earlyInitFunc != NULL && m->lateCleanupFunc != NULL) ||
	   (m->earlyInitFunc == NULL && m->lateCleanupFunc == NULL) );

   if (m->earlyInitFunc != NULL) {
      LOG(0, "Calling earlyInitFunc %p", m->earlyInitFunc); 
      earlyInitStatus = m->earlyInitFunc();
      if (earlyInitStatus == 0) {
	 LOG(0, "Early Initialization for %s succeeded.", m->modName);
      } else {
	 Warning("Early Initialization for %s failed.", m->modName);
	 initStatus = -1;
      }
   } 

   if (earlyInitStatus == 0) {

      initStatus = m->initFunc();

      if (initStatus == 0) {
	 Log("Initialization for %s succeeded.", m->modName);
	 /* inform vmkstats module (for mapping PC samples) */
	 VMKStats_ModuleLoaded(m->modName,
	       0,
	       (uint32) m->readOnlyBaseAddr,
	       m->readOnlyLength,
	       (uint32) m->initFunc,
	       (uint32) m->cleanupFunc);

	 if (m->postInitFunc != NULL) {
	    LOG(0, "Calling postInitFunc %p", m->postInitFunc);
	    m->postInitFunc(m->postInitFuncData);
	 }   
      } else {
	 Warning("Initialization for %s failed.", m->modName);
	 if (m->postInitFailureFunc != NULL) {
	    LOG(0, "Calling postInitFailureFunc %p", m->postInitFailureFunc);
	    m->postInitFailureFunc(m->postInitFailureData);
	 }
      }
   }

   Log("mainHeap avail after: %d", Mem_Avail());
   
   prevIRQL = SP_LockIRQ(&modLock, SP_IRQL_KERNEL);
   m->useCount--; //useCount was incremented in Mod_LoadDone

   if (initStatus == 0) {
      /* 
       * Setting this flag allows the console OS to proceed from the module 
       * loading call, so do this only when module loading is completely done. 
       */
      m->loaded = TRUE;
      if (m->privID == NULL) {
	 Log("no private ID set"); 
      }
   } else {
      ModFree(m);
   }
   modBeingLoaded = NULL;
   SP_UnlockIRQ(&modLock, prevIRQL);

   arg.load = 1;
   strncpy(arg.name, m->modName, VMNIX_MODULE_NAME_LENGTH);
   VmkEvent_PostHostAgentMsg(VMKEVENT_MODULE_LOAD, &arg, sizeof(arg));

   if (initStatus == 0) {
      StatusTerm_Printf("Module loaded successfully.\n\n");
   } else {
      StatusTerm_Printf("Module failed to load.\n\n");
   }
}
   
VMK_ReturnStatus
Mod_LoadDone(VMnix_ModLoadDoneArgs *args)
{
   ModuleInfo *m;
   VMK_ReturnStatus status = VMK_OK;   
   SP_IRQL prevIRQL;


   prevIRQL = SP_LockIRQ(&modLock, SP_IRQL_KERNEL);

   m = ModFind(args->moduleID);
   if (m == NULL) {
      LOG(0, "Module %d not found", args->moduleID);
      status = VMK_NOT_FOUND;
   } else if (ModAddrOutsideRO(m, (char *)args->initFunc)) {
      status = VMK_BAD_PARAM;
   } else if (ModAddrOutsideRO(m, (char *)args->cleanupFunc)) { 
      status = VMK_BAD_PARAM;
   } else if ((args->earlyInitFunc != NULL ) && 
	      ModAddrOutsideRO(m, (char *)args->earlyInitFunc)) { 
      status = VMK_BAD_PARAM;
   } else if ((args->lateCleanupFunc != NULL ) && 
	      ModAddrOutsideRO(m, (char *)args->lateCleanupFunc)) {
      status = VMK_BAD_PARAM;
   }

   if (status == VMK_OK) {
      if (modBeingLoaded != NULL) {
	 Warning("Can only load one module at once");
	 status = VMK_BUSY;
      } else {
	 modBeingLoaded = m;
         m->useCount++; //don't allow unload while initializing
      }
   }

   SP_UnlockIRQ(&modLock, prevIRQL);

   if (status != VMK_OK) {
      return status;
   }

   Log("Load done, starting initialization for %s "
       "initFunc: %p text: 0x%x data: 0x%x bss: 0x%x",
       m->modName, args->initFunc, args->textBase, args->dataBase, args->bssBase);
   m->initFunc = args->initFunc;
   m->cleanupFunc = args->cleanupFunc;
   m->earlyInitFunc = args->earlyInitFunc;
   m->lateCleanupFunc = args->lateCleanupFunc;
   m->textBase = args->textBase;
   m->dataBase = args->dataBase;
   m->bssBase = args->bssBase;

   Helper_Request(HELPER_MISC_QUEUE, ModInitModule, NULL);

   return VMK_OK;
}

VMK_ReturnStatus
Mod_Unload(int moduleID)
{
   ModuleInfo *m;
   VMK_ReturnStatus status = VMK_OK;   
   SP_IRQL prevIRQL;
   VmkEvent_VmkLoadModArgs arg;
   PCI_Device *dev;

   prevIRQL = SP_LockIRQ(&modLock, SP_IRQL_KERNEL);

   LOG(1, "%d", moduleID);

   m = ModFind(moduleID);
   if (m == NULL) {
      LOG(0, "Module %d not found", moduleID);
      status = VMK_NOT_FOUND;
   } else if (m->useCount > 0) {
      LOG(0, "Use count = %d", m->useCount);
      status = VMK_BUSY;
   } else {
      status = VMK_OK;
      m->loaded = FALSE;
      ASSERT(m->inList);
      List_Remove(&m->links);
      m->inList = FALSE;
   }

   SP_UnlockIRQ(&modLock, prevIRQL);

   if (status != VMK_OK) {
      return status;
   }

   for (dev = PCI_GetFirstDevice(); dev != NULL; dev = PCI_GetNextDevice(dev)) {
      if (dev->moduleID == moduleID) {
	 dev->moduleID = MOD_ID_NONE;
      }
   }

   VMKStats_ModuleUnloaded(m->modName);

   if (m->preUnloadFunc != NULL) {
      LOG(0, "Calling pre-unload func");
      m->preUnloadFunc(m->preUnloadFuncData);
   }

   if (m->cleanupFunc != NULL) {
      LOG(0, "Calling cleanup");
      m->cleanupFunc();
   } else {
      LOG(0, "!Calling cleanup"); 
   }

   if (m->lateCleanupFunc != NULL) {
      LOG(0, "Calling lateCleanup");
      m->lateCleanupFunc();
   } else {
      LOG(0, "!Calling lateCleanup");
   }

   prevIRQL = SP_LockIRQ(&modLock, SP_IRQL_KERNEL);

   // Prepare arguments for vmkevent.
   arg.load = 0;
   strncpy(arg.name, m->modName, VMNIX_MODULE_NAME_LENGTH);

   ModFree(m);
   SP_UnlockIRQ(&modLock, prevIRQL);

   VmkEvent_PostHostAgentMsg(VMKEVENT_MODULE_LOAD, &arg, sizeof(arg));
   return VMK_OK;
}

void
Mod_List(int maxModules, VMnix_ModListResult *list)
{
   VMnix_ModDesc desc;
   List_Links *elt;
   int numModules = 0;   
   SP_IRQL prevIRQL;

   prevIRQL = SP_LockIRQ(&modLock, SP_IRQL_KERNEL);

   LIST_FORALL(&moduleList, elt) {
      ModuleInfo *mi = (ModuleInfo *)elt;
      strcpy(desc.modName, mi->modName);
      desc.readOnlyLoadAddr = mi->readOnlyBaseAddr;
      desc.writableLoadAddr = mi->writableBaseAddr;
      desc.readOnlyLength = mi->readOnlyLength;
      desc.writableLength = mi->writableLength;
      desc.initFunc = mi->initFunc;
      desc.cleanupFunc = mi->cleanupFunc;
      desc.earlyInitFunc = mi->earlyInitFunc;
      desc.lateCleanupFunc = mi->lateCleanupFunc;
      desc.moduleID = mi->id;
      desc.loaded = mi->loaded;
      desc.textBase = mi->textBase;
      desc.dataBase = mi->dataBase;
      desc.bssBase = mi->bssBase;
      desc.useCount = mi->useCount;
      memcpy(&(list->desc[numModules]), &desc, sizeof(desc));
      numModules++;
      if (numModules == maxModules) {
	 break;
      }
   }

   list->numModules = numModules;

   SP_UnlockIRQ(&modLock, prevIRQL);
}

void
Mod_Dump(void)
{
   List_Links *elt;
   SP_IRQL prevIRQL;

   prevIRQL = SP_LockIRQ(&modLock, SP_IRQL_KERNEL);

   Log("%-15s%-11s%-11s%-11s%-11s%-11s",
       "Name", "R/O Addr", "R/W Addr", "Text", "Data", "BSS");
   LIST_FORALL(&moduleList, elt) {
      ModuleInfo *mi = (ModuleInfo *)elt;

      Log("%-15s0x%-9p0x%-9p0x%-9x0x%-9x0x%-9x",
	  mi->modName,
	  mi->readOnlyBaseAddr,
	  mi->writableBaseAddr,
          mi->textBase,
	  mi->dataBase,
	  mi->bssBase);
   }

   SP_UnlockIRQ(&modLock, prevIRQL);
}

static ModuleInfo *modCurrent;

uint32
Mod_GetCurrentID(void)
{
   if (modBeingLoaded != NULL) {
      return modBeingLoaded->id;
   } else if (modCurrent != NULL) {
      return modCurrent->id;
   } else {
      return 0;
   }
}

void
Mod_Cleanup(void)
{
   List_Links *elt;
   ModuleInfo *mi;

   for (elt = List_Last(&moduleList); !List_IsAtEnd(&moduleList,elt);
	elt = List_Prev(elt)) {
      mi = (ModuleInfo *)elt;
      if (mi->cleanupFunc != NULL) {
         LOG(0, "Calling cleanup func for module %s", mi->modName);
         mi->cleanupFunc();
      }
      if (mi->lateCleanupFunc != NULL) {
	 LOG(0, "Calling lateCleanup func for module %s", mi->modName);
	 mi->lateCleanupFunc();
      }
   }
}

VMK_ReturnStatus
Mod_IncUseCount(int moduleID)
{
   VMK_ReturnStatus status;
   ModuleInfo *m;
   SP_IRQL prevIRQL;

   prevIRQL = SP_LockIRQ(&modLock, SP_IRQL_KERNEL);

   m = ModFind(moduleID);
   if (m == NULL) {
      status = VMK_NOT_FOUND;
   } else {
      m->useCount++;
      status = VMK_OK;
   }

   SP_UnlockIRQ(&modLock, prevIRQL);

   return status;
}

VMK_ReturnStatus
Mod_DecUseCount(int moduleID)
{
   VMK_ReturnStatus status;
   ModuleInfo *m;
   SP_IRQL prevIRQL;

   prevIRQL = SP_LockIRQ(&modLock, SP_IRQL_KERNEL);

   m = ModFind(moduleID);
   if (m == NULL) {
      status = VMK_NOT_FOUND;
   } else {
      m->useCount--;
      ASSERT(m->useCount >= 0);
      status = VMK_OK;
   }

   SP_UnlockIRQ(&modLock, prevIRQL);

   return status;
}

VMK_ReturnStatus
Mod_GetUseCount(int moduleID, uint32 *useCount)
{
   VMK_ReturnStatus status = VMK_NOT_FOUND;
   ModuleInfo *m;
   SP_IRQL prevIRQL;

   prevIRQL = SP_LockIRQ(&modLock, SP_IRQL_KERNEL);

   m = ModFind(moduleID);
   if (m != NULL) {
      *useCount = m->useCount;
      status = VMK_OK;
   }

   SP_UnlockIRQ(&modLock, prevIRQL);

   return status;
}


VMK_ReturnStatus
Mod_AddSym(VMnix_SymArgs *args)
{
   ModuleSymbol *symbol;   
   ModuleSymbolMemInfo *symMemInfo;
   char *name = NULL;
   ModuleInfo *mi = NULL;   
   VMK_ReturnStatus status = VMK_OK;   
   SP_IRQL prevIRQL;


   prevIRQL = SP_LockIRQ(&modLock, SP_IRQL_KERNEL);


   if (args->moduleID != 0) {
      mi = ModFind(args->moduleID);
      if (mi == NULL) {
	 status = VMK_BAD_PARAM;
	 goto exit;
      }
   }

   if (args->moduleID == 0) {
      if (modSymMemInfo.bufferLength == 0) {
	 status = ModAllocSymbolMem(args, &modSymMemInfo);
	 if (status != VMK_OK) {
	    goto exit;
	 }
      }
      symMemInfo = &modSymMemInfo;
   } else {      
      if (mi->symMemInfo.bufferLength == 0) {
	 status = ModAllocSymbolMem(args, &mi->symMemInfo);
	 if (status != VMK_OK) {
	    goto exit;
	 }
      }
      symMemInfo = &mi->symMemInfo;
   }

   if (symMemInfo->numSymbols + 1 > symMemInfo->maxSymbols) {
      Warning("Adding more symbols than claimed (%d)", 
	      symMemInfo->maxSymbols);
      status = VMK_LIMIT_EXCEEDED;
      goto exit;
   }

   if (symMemInfo->bufferNext + args->nameLength + 1 + sizeof(ModuleSymbol) > 
       symMemInfo->bufferStart + symMemInfo->bufferLength + 1) {
      Warning("No room for symbol %s", name);
      status = VMK_LIMIT_EXCEEDED;
      goto exit;
   }

   symMemInfo->numSymbols++;
   name = symMemInfo->bufferNext;
   symMemInfo->bufferNext += args->nameLength + 1;
   symbol = (ModuleSymbol *)(symMemInfo->bufferNext);
   symMemInfo->bufferNext += sizeof(ModuleSymbol);

   memcpy(name, args->name, args->nameLength);
   name[args->nameLength] = 0;

   if (!args->global) {
      ModAddLocalSymbol(name, args->value, args->moduleID, symbol);
   } else {
      if (ModSymbolHTFind(name) != NULL) {
	 status = VMK_EXISTS;
         goto exit;
      }
   
      symbol->name = name;
      symbol->value = args->value;
      symbol->size = args->size;
      symbol->info = args->info;
      symbol->moduleID = args->moduleID;
      symbol->symbolNum = curSymbolNum++;
   
      symbol->nextInList = symbolList;
      symbolList = symbol;
   
      ModSymbolHTAdd(symbol);
   
      name = NULL;
   
      if (mi != NULL) {
	 mi->symbolsPresent = TRUE;
      }
   }

exit:

   SP_UnlockIRQ(&modLock, prevIRQL);

   return status;
}

static void
ModAddLocalSymbol(char *name, uint32 value, uint32 moduleID, ModuleSymbol *symbol)
{
   symbol->name = name;
   symbol->value = value;
   symbol->moduleID = moduleID;

   symbol->nextInList = localSymbolList;
   localSymbolList = symbol;
}

VMK_ReturnStatus
Mod_GetSym(VMnix_SymArgs *args)
{
   uint32 nextSymbolNum;
   VMK_ReturnStatus status = VMK_OK;
   SP_IRQL prevIRQL;

   nextSymbolNum = args->nextSymbolNum;

   prevIRQL = SP_LockIRQ(&modLock, SP_IRQL_KERNEL);

   if (nextSymbolNum == 0) {
      if (symbolList == NULL) {
	 nextSymbol = NULL;      
	 status = VMK_WOULD_BLOCK;
      } else {
	 nextSymbol = symbolList;
      }
   } else if (nextSymbol == NULL) {
      status = VMK_WOULD_BLOCK;
   } else if (nextSymbol->symbolNum != nextSymbolNum) {
      Warning("Unexpected symbol number");
      status = VMK_IO_ERROR;
   }

   if (status == VMK_OK) {
      int nameLength = strlen(nextSymbol->name) + 1;
      if (args->nameLength < nameLength) {
	 Warning("Name length too short");
	 status = VMK_BAD_PARAM;
      } else {
	 memcpy(args->name, nextSymbol->name, nameLength);
	 args->nameLength = nameLength;	 
	 args->value = nextSymbol->value;
	 args->size = nextSymbol->size;
	 args->info = nextSymbol->info;
	 args->moduleID = nextSymbol->moduleID;

	 nextSymbol = nextSymbol->nextInList;
	 if (nextSymbol == NULL) {
	    args->nextSymbolNum = 0xffffffff;
	 } else {
	    args->nextSymbolNum = nextSymbol->symbolNum;
	 }
      }
   }

   SP_UnlockIRQ(&modLock, prevIRQL);

   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * NameHash --
 *
 *      Primitive hash function on the symbol name.
 *
 * Results: 
 *	A hash value.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static uint32 
NameHash(const char *name)
{
   uint32 sum = 0;
   while (*name != 0) {
      sum += *name;
      name++;
   }

   return sum;
}

static ModuleSymbol *
ModSymbolHTFind(const char *name)
{
   ModuleSymbol *curSym;
   int index = NameHash(name) % SYMBOL_HASH_TABLE_SIZE;

   for (curSym = symbolHashTable[index];
        curSym != NULL && strcmp(curSym->name, name) != 0;
	curSym = curSym->nextInHash) {
   }

   return curSym;
}

static void
ModSymbolHTAdd(ModuleSymbol *symbol)
{
   int index = NameHash(symbol->name) % SYMBOL_HASH_TABLE_SIZE;

   symbolList->nextInHash = symbolHashTable[index];
   symbolHashTable[index] = symbolList;
}

static void
ModSymbolHTRemove(ModuleSymbol *symbol)
{
   ModuleSymbol *prevSym, *curSym;

   int index = NameHash(symbol->name) % SYMBOL_HASH_TABLE_SIZE;

   for (prevSym = NULL, curSym = symbolHashTable[index];
        curSym != NULL && curSym != symbol;
	curSym = curSym->nextInHash) {
   }

   if (curSym != NULL) {
      if (prevSym == NULL) {
	 symbolHashTable[index] = symbol->nextInHash;
      } else {
	 prevSym->nextInHash = symbol->nextInHash;
      }
      return;
   }

   NOT_IMPLEMENTED();
}

/*
 *----------------------------------------------------------------------
 *
 * Mod_LookupPC --
 *
 *      Search the symbol list for the symbol that is the closest to
 *	the given pc.  If something is found then TRUE is returned,
 *	*name points to the name of the symbol, and *offset contains the
 *	offset that the pc is from the symbols value.
 *
 *	** IMPORTANT: This routine is designed to be only called during
 *	              debugging backtraces.  As a result there is no locking
 *		      and a pointer to internal data is returned.  It is not
 *		      safe to call this function at any other time.
 *
 * Results: 
 *	TRUE if any symbol is found and FALSE otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
Bool
Mod_LookupPC(uint32 pc, char **name, uint32 *offset)
{
   ModuleSymbol	*curSym, *closestSym;
   ModuleSymbol fakeSym;
   List_Links *elt;

   // being called before moduleList is initialized (very early panic)
   if (List_First(&moduleList) == NULL) {
      return FALSE;
   }

   fakeSym.value = 0;
   fakeSym.name = "Unknown Function";
   closestSym = &fakeSym;

   LIST_FORALL(&moduleList, elt) {
      ModuleInfo *mi = (ModuleInfo *)elt;
      LOG(3, "Comparing %d against %s\n", pc, mi->modName);
      if (pc >= (uint32)mi->readOnlyBaseAddr && 
          pc < (uint32)mi->readOnlyBaseAddr + mi->readOnlyLength) {
         LOG(3, "Setting fakeSym to %s\n", mi->modName);
         fakeSym.value = (uint32)mi->readOnlyBaseAddr;
         fakeSym.name = mi->modName;
      }
   }

   for (curSym = symbolList; curSym != NULL; curSym = curSym->nextInList) {
      if (pc >= curSym->value && closestSym->value < curSym->value) {
         LOG(3, "Changing symbol for 0x%x from %s to %s. curSym->value = 0x%x, closestSym->value = 0x%x",
             pc, closestSym->name, curSym->name, curSym->value, closestSym->value);
         closestSym = curSym;
      }
   }

   for (curSym = localSymbolList; curSym != NULL; curSym = curSym->nextInList) {
      if (pc >= curSym->value && closestSym->value < curSym->value) {
         LOG(3, "Changing symbol for 0x%x from %s to %s. curSym->value = 0x%x, closestSym->value = 0x%x",
             pc, closestSym->name, curSym->name, curSym->value, closestSym->value);
         closestSym = curSym;
      }
   }

   if (closestSym->value != 0) {
      *name = closestSym->name;
      *offset = pc - closestSym->value;
      return TRUE;
   } else {
      return FALSE;
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * Mod_LookupSymbolSafe --
 *
 *      Finds the symbol with the address closest to "pc." Unlike Mod_LookupPC,
 *      this includes locking and is safe to call at runtime.  Copies at most
 *      namelen characters of the symbol name into name, or writes "unknown"
 *      and returns FALSE if the symbol could not be found.
 *
 * Results:
 *      Returns TRUE if a symbol is found, FALSE otherwise. Copies symbol
 *      name into "name" and sets *offset to offset of pc within symbol.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
Bool
Mod_LookupSymbolSafe(uint32 pc, int namelen, char *name, uint32 *offset)
{
   char *symname;
   Bool res;
   SP_LockIRQ(&modLock, SP_IRQL_KERNEL);

   res = Mod_LookupPC(pc, &symname, offset);
   if (res) {
      LOG(2, "name for 0x%x is %s", pc, symname);
      strncpy(name, symname, namelen);
   }
   SP_UnlockIRQ(&modLock, SP_GetPrevIRQ(&modLock));

   return (res);
}

/*
 *----------------------------------------------------------------------
 *
 * Mod_RegisterPreUnloadFunc --
 *
 *      Save a function to call before calling the module unload function.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	The corresponding module info's preUnloadFunc field is updated.
 *
 *----------------------------------------------------------------------
 */
void
Mod_RegisterPreUnloadFunc(int moduleID, ModLoadCBFunc f, void *data)
{
   ModuleInfo *m;
   SP_IRQL prevIRQL;

   prevIRQL = SP_LockIRQ(&modLock, SP_IRQL_KERNEL);

   m = ModFind(moduleID);
   if (m == NULL) {
      Warning("Couldn't find module %d", moduleID);
   } else {
      m->preUnloadFunc = f;
      m->preUnloadFuncData = data;
   }

   SP_UnlockIRQ(&modLock, prevIRQL);
}


/*
 *----------------------------------------------------------------------
 *
 * Mod_RegisterPostInitFunc --
 *
 *      Save a function to call after calling the module init function.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	The corresponding module info's postInitFunc field is updated.
 *
 *----------------------------------------------------------------------
 */
void
Mod_RegisterPostInitFunc(int moduleID, ModLoadCBFunc initFunc, void *data, 
                         ModLoadCBFunc initFailureFunc, void *etherDevData)
{
   ModuleInfo *m;
   SP_IRQL prevIRQL;

   prevIRQL = SP_LockIRQ(&modLock, SP_IRQL_KERNEL);

   m = ModFind(moduleID);
   if (m == NULL) {
      Warning("Couldn't find module %d", moduleID);
   } else {
      m->postInitFunc = initFunc;
      m->postInitFuncData = data;
      m->postInitFailureFunc = initFailureFunc;
      m->postInitFailureData = etherDevData;
   }

   SP_UnlockIRQ(&modLock, prevIRQL);
}


/*
 *----------------------------------------------------------------------
 *
 * Mod_RegisterDevCBFuncs --
 *
 * 	Save functions to call after a device has been inserted/removed
 *
 * Results:
 * 	None.
 *
 * Side Effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
void
Mod_RegisterDevCBFuncs(int moduleID, ModDevCBFunc postInsertFunc, ModDevCBFunc preRemoveFunc)
{
   ModuleInfo *m;
   SP_IRQL prevIRQL;

   prevIRQL = SP_LockIRQ(&modLock, SP_IRQL_KERNEL);

   m = ModFind(moduleID);
   if (m == NULL) {
      Warning("Couldn't find module %d", moduleID);
   } else {
      m->postInsertFunc = postInsertFunc;
      m->preRemoveFunc = preRemoveFunc;
   }

   SP_UnlockIRQ(&modLock, prevIRQL);
}

/*
 *----------------------------------------------------------------------
 *
 * ModAllocSymbolMem --
 *
 *      Allocate memory to hold symbols information.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	Memory is allocated an *sysMemInfo is updated.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
ModAllocSymbolMem(VMnix_SymArgs *args, ModuleSymbolMemInfo *symMemInfo)
{
   int length;

   if (args->numSymbols == 0) {
      return VMK_BAD_PARAM;
   }

   /* 
    * Allocate space for symbol info, the names, and then 1 byte extra for each
    * symbol to null terminate the symbol.
    */
   length = args->numSymbols * sizeof(ModuleSymbol) + 
	    (args->namesLength + args->numSymbols);
   Log("Allocating %d bytes for %d symbols, %d of names",
	   length, args->numSymbols, args->namesLength);
   if (length > MAX_SYMBOL_INFO_SIZE) {
      Warning("Too much space for symbols");
      return VMK_LIMIT_EXCEEDED;
   }
   symMemInfo->bufferStart = (char *)Mem_Alloc(length);
   if (symMemInfo->bufferStart == NULL) {
      Warning("Couldn't allocate space for symbols");
      return VMK_NO_MEMORY;
   }

   symMemInfo->bufferLength = length;
   symMemInfo->bufferNext = symMemInfo->bufferStart;
   symMemInfo->numSymbols = 0;
   symMemInfo->maxSymbols = args->numSymbols;

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Mod_DumpSymbols --
 *
 *      Dump all of the symbols that we have to the log.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
void Mod_DumpSymbols(void)
{
   ModuleSymbol *curSym;

   Log("GLOBAL SYMBOLS:");
   for (curSym = symbolList; curSym != NULL; curSym = curSym->nextInList) {
      Log("  %-20s 0x%x", curSym->name, curSym->value);
   }
   Log("LOCAL SYMBOLS:");
   for (curSym = localSymbolList; curSym != NULL; curSym = curSym->nextInList) {
      Log("  %-20s 0x%x", curSym->name, curSym->value);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * ModPrivIDToModule --
 *
 * 	Return the module associated with a module code own id
 *
 * Results:
 * 	module if successful, NULL otherwise
 *
 * Side Effects:
 * 	None.
 *
 * ----------------------------------------------------------------------
 */
static ModuleInfo *
ModPrivIDToModule(void *privID)
{
   List_Links *elt;

   LIST_FORALL(&moduleList, elt) {
      ModuleInfo *mi = (ModuleInfo *)elt;

      if (mi->privID == privID) {
         return (ModuleInfo *)mi;
      }
   }
   return NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * Mod_RegisterDriver --
 *
 * 	Associate the module code own id with the currently loading
 * 	vmkernel module.
 *
 * Results:
 * 	None.
 *
 * Side Effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
void
Mod_RegisterDriver(void *privID)
{
   ASSERT(modBeingLoaded != NULL);
   ASSERT(modBeingLoaded->privID == NULL);
   ASSERT(ModPrivIDToModule(privID) == NULL);
   modBeingLoaded->privID = privID;
}

/*
 *----------------------------------------------------------------------
 *
 * Mod_SetCurrent --
 *
 * 	Set a module to be the current one to remedy lack of context
 *
 * Results:
 * 	None.
 *
 * Side Effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
void
Mod_SetCurrent(void *privID)
{
   modCurrent = ModPrivIDToModule(privID);
}

/*
 *----------------------------------------------------------------------
 *
 * Mod_ResetCurrent --
 *
 * 	Reset the current module to none
 *
 * Results:
 *	None.
 *
 * Side Effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
void
Mod_ResetCurrent(void)
{
   modCurrent = NULL;
}
   
/*
 *----------------------------------------------------------------------
 *
 * Mod_DoPostInsert --
 * 
 * 	Invoke a module-specific function after a device has been inserted
 *
 * Results:
 * 	None.
 *
 * Side Effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
void
Mod_DoPostInsert(void *privID, PCI_Device *dev)
{
   ModuleInfo *mi;

   ASSERT(dev->moduleID == MOD_ID_NONE);

   if (modBeingLoaded) {
      dev->moduleID = modBeingLoaded->id;
      Log("modBeingLoaded: post insert not done here");
      return;
   }

   mi = ModPrivIDToModule(privID);
   if (mi == NULL) {
      dev->moduleID = MOD_ID_UNKNOWN;
      Warning("No module found (maybe old style driver)");
      return;
   }

   dev->moduleID = mi->id;
   if (mi->postInsertFunc != NULL) {
      Log("Calling post-insert func");
      mi->postInsertFunc(mi->id, dev);
   } else {
      Log("No post-insert func");
   }
}

/*
 *----------------------------------------------------------------------
 *
 * Mod_DoPreRemove --
 *
 * 	Invoke a module-specific function before a device is removed
 *
 * Results:
 * 	None.
 *
 * Side Effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
void
Mod_DoPreRemove(void *privID, PCI_Device *dev)
{
   ModuleInfo *mi;

   ASSERT(modBeingLoaded == NULL);
   ASSERT(dev->moduleID != MOD_ID_NONE);

   dev->moduleID = MOD_ID_NONE;

   mi = ModPrivIDToModule(privID);
   if (mi == NULL) {
      Warning("No module found (maybe old style driver)");
      return;
   }

   if (mi->preRemoveFunc != NULL) {
      Log("Calling pre-remove func");
      mi->preRemoveFunc(mi->id, dev);
   } else {
       Log("No pre-remove func");
   }
}

/*
 *----------------------------------------------------------------------
 *
 * Mod_SetModuleVersionInt --
 *
 * 	Store the driver version string in the module info.
 *
 * 	Mest be called from the init_module function of the driver.
 *
 * Results:
 * 	None.
 *
 * Side Effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
void
Mod_SetModuleVersionInt(char *fmt, ...)
{
   ModuleInfo *mi = modBeingLoaded;

   if (mi) {
      va_list args;
      va_start(args, fmt);
      vsnprintf(mi->versionInfo, sizeof mi->versionInfo, fmt, args);
      va_end(args);
      LOG(0, "Version for %s is %s", mi->modName, mi->versionInfo);
   } else {
      ASSERT(FALSE);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * Mod_SetModuleVersionExt --
 *
 * 	Store the driver version string in the module info.
 *
 * 	Mest be called from the init_module function of the driver.
 *
 * Results:
 * 	None.
 *
 * Side Effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
void
Mod_SetModuleVersionExt(char *info, uint32 len)
{
   ModuleInfo *mi = modBeingLoaded;

   if (mi) {
      snprintf(mi->versionInfo, sizeof mi->versionInfo, "%s", info);
      LOG(0, "Version for %s is %s", mi->modName, mi->versionInfo);
   } else {
      ASSERT(FALSE);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * Mod_ProcPrintVersionInfo --
 *
 *      Prints out "<modname>:<version>" info for all loaded modules.
 *
 * Results:
 * 	None.
 *
 * Side Effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
void
Mod_ProcPrintVersionInfo(char *page, int *lenp)
{
   List_Links *elt;
   SP_IRQL prevIRQL;

   prevIRQL = SP_LockIRQ(&modLock, SP_IRQL_KERNEL);

   LIST_FORALL(&moduleList, elt) {
      ModuleInfo *mi = (ModuleInfo *)elt;
      Proc_Printf(page, lenp, "   %-32s build %s\n", mi->modName, mi->versionInfo);
   }
   SP_UnlockIRQ(&modLock, prevIRQL);
}

/*
 *----------------------------------------------------------------------
 *
 * Mod_ListPrint --
 *
 *	Prints out the list of loaded modules.  Since this is called
 *	only from bluescreen.c and may be called *before* Mod_Init is
 *	called, we perform a check to make sure moduleList has been
 *	initialized and if it hasn't, bail.
 *
 * Results:
 * 	None.
 *
 * Side Effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
void
Mod_ListPrint(void)
{
   List_Links *elt;

   if (moduleList.nextPtr == NULL && moduleList.prevPtr == NULL) {
      _Log("No modules loaded yet.\n");
      return;
   }

   LIST_FORALL(&moduleList, elt) {
      ModuleInfo *mi = (ModuleInfo *)elt;   
      _Log("%-20s %d -s .data %d -s .bss %d\n",
           mi->modName, mi->textBase, mi->dataBase, mi->bssBase);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * Mod_SetHeapID --
 *
 *	Sets the Heap ID field in the linked list for a particular module. Used to get
 *	around the init_etherdev module heap allocation problem. See init_etherdev in
 *	vmklinux.
 *
 * Results:
 * 	None.
 *
 * Side Effects:
 * 	Sets the Heap ID.
 *
 *----------------------------------------------------------------------
 */
void
Mod_SetHeapID(uint32 moduleID, vmk_HeapID heap) {

   ModuleInfo *mi = ModFind(moduleID);

   ASSERT(mi != NULL);

   mi->heap = heap;
}

/*
 *----------------------------------------------------------------------
 *
 * Mod_GetHeapID --
 *
 *	Gets the Heap ID field in the linked list of modules for a particular module ID.
 *	Also used to get around the init_etherdev problem in vmklinux.
 *
 * Results:
 * 	Heap ID.
 *
 * Side Effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
vmk_HeapID
Mod_GetHeapID(uint32 moduleID) {
   
   ModuleInfo *mi = ModFind(moduleID);

   ASSERT(mi != NULL);

   return mi->heap;
}
