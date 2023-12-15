/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * mod_loader_dist.c --
 *
 *	module loader functionality for the outside world.
 *
 */ 

/*
 * Note that all include files should be either public or dist header files,
 * except for vmware.h.
 */

/* vmware header */
#include "vmware.h"

/* dist headers */
#include "heap_dist.h"

/* public headers */
#include "vm_libc.h"
#include "mod_loader_public.h"
#include "vmk_stubs.h"

void
vmk_ModLoaderSetHeapID(uint32 moduleID, vmk_HeapID heap)
{
   Mod_SetHeapID(moduleID, heap);
}

vmk_HeapID
vmk_ModLoaderGetHeapID(uint32 moduleID)
{
   return (vmk_HeapID)Mod_GetHeapID(moduleID);
}

uint32
vmk_ModLoaderGetCurrentID(void)
{
   return Mod_GetCurrentID();
}

int
vmk_ModLoaderGetLockRanking(void)
{
   return SP_RANK_MODLOCK;
}

int                                                                                       
vmk_ModLoaderGetVersionStringLength(void)                                                 
{                                                                                         
   return MOD_VERSION_STRING_LENGTH;                                                      
}

int
vmk_ModLoaderIncUseCount(uint32 moduleID)
{
   if (Mod_IncUseCount(moduleID) != VMK_OK) {
      return -1;
   }

   return 0;
}

int
vmk_ModLoaderDecUseCount(uint32 moduleID)
{
   if (Mod_DecUseCount(moduleID) != VMK_OK) {
      return -1;
   }

   return 0;
}

void
vmk_ModLoaderSetModuleVersionInt(char *fmt, ...)
{
   char info[MOD_VERSION_STRING_LENGTH];

   va_list args;
   va_start(args, fmt);
   vsnprintf(info, sizeof info, fmt, args);
   va_end(args);

   Mod_SetModuleVersionExt(info, MOD_VERSION_STRING_LENGTH);
}

void
vmk_ModLoaderRegisterDriver(void *linuxDriver) 
{
   Mod_RegisterDriver(linuxDriver);
}

void
vmk_ModLoaderSetCurrent(void *linuxDriver)
{
   Mod_SetCurrent(linuxDriver);
}

void
vmk_ModLoaderResetCurrent(void)
{
   Mod_ResetCurrent();
}

void
vmk_ModLoaderDoPostInsert(void *linuxDriver, struct PCI_Device *dev)
{
   Mod_DoPostInsert(linuxDriver, dev);
}

void
vmk_ModLoaderDoPreRemove(void *linuxDriver, struct PCI_Device *dev)
{
   Mod_DoPreRemove(linuxDriver, dev);
}








