/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * mod_loader.h --
 *
 *	Module loader module.
 */

#ifndef _MOD_LOADER_H
#define _MOD_LOADER_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "return_status.h"
#include "mod_loader_public.h"

struct VMnix_StartupArgs;
extern void Mod_Init(void);
struct VMnix_ModAllocArgs;
struct VMnix_ModAllocResult;
extern VMK_ReturnStatus Mod_Alloc(struct VMnix_ModAllocArgs *args,
                                  struct VMnix_ModAllocResult *result);
extern VMK_ReturnStatus Mod_PutPage(int moduleID, void *addr, void *data);
struct VMnix_ModLoadDoneArgs;
extern VMK_ReturnStatus Mod_LoadDone(struct VMnix_ModLoadDoneArgs *args);
extern VMK_ReturnStatus Mod_LoadProbe(int moduleID);
struct VMnix_ModListResult;
extern void Mod_List(int maxModules, struct VMnix_ModListResult *result);
extern void Mod_Cleanup(void);
extern VMK_ReturnStatus Mod_Unload(int moduleID);
extern VMK_ReturnStatus Mod_GetUseCount(int moduleID, uint32 *useCount);
extern Bool Mod_GetName(int moduleID, char *modName);
struct VMnix_SymArgs;
extern VMK_ReturnStatus Mod_AddSym(struct VMnix_SymArgs *args);
extern VMK_ReturnStatus Mod_GetSym(struct VMnix_SymArgs *args);
extern Bool Mod_LookupPC(uint32 pc, char **name, uint32 *offset);
typedef void (*ModLoadCBFunc)(void *);
extern void Mod_RegisterPreUnloadFunc(int moduleID, ModLoadCBFunc f, void *data);
extern void Mod_RegisterPostInitFunc(int moduleID, ModLoadCBFunc initFunc, void *data, 
              ModLoadCBFunc initFailureFunc, void *etherDevData);

typedef void (*ModDevCBFunc)(int moduleID, PCI_Device *dev);
extern void Mod_RegisterDevCBFuncs(int moduleID, ModDevCBFunc postInsertFunc, ModDevCBFunc preRemoveFunc);
void Mod_ProcPrintVersionInfo(char *page, int *lenp);
Bool Mod_LookupSymbolSafe(uint32 pc, int namelen, char *name, uint32 *offset);
void Mod_ListPrint(void);

#endif
