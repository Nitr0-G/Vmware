/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * mod_loader_dist.h --
 *
 *	Module loader module.
 */

#ifndef _MOD_LOADER_DIST_H
#define _MOD_LOADER_DIST_H

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMK_MODULE
#define INCLUDE_ALLOW_DISTRIBUTE
#include "includeCheck.h"

#include "heap_dist.h"

/*
 * Once we have some sort of EXPORT_SYMBOL functionality, these will all be
 * prefaced with that.
 */

extern void vmk_ModLoaderSetHeapID(uint32 moduleID, vmk_HeapID heap);
extern vmk_HeapID vmk_ModLoaderGetHeapID(uint32 moduleID);

extern uint32 vmk_ModLoaderGetCurrentID(void);

extern int vmk_ModLoaderGetLockRanking(void);
extern int vmk_ModLoaderGetVersionStringLength(void);

extern int vmk_ModLoaderIncUseCount(uint32 moduleID);
extern int vmk_ModLoaderDecUseCount(uint32 moduleID);

extern void vmk_ModLoaderSetModuleVersionInt(char *fmt, ...);

#define vmk_ModLoaderSetModuleVersion(_fmt, _args...) \
    vmk_ModLoaderSetModuleVersionInt("%d: " _fmt, BUILD_NUMBER_NUMERIC , ##_args)

struct PCI_Device;

/* 
 * XXX This whole file is somewhat sketchy at the moment to avoid having to edit pci code
 * (pci.c) in the console-os. These functions all have vmkdriver wrappers listed at the
 * bottom, but the following externs need to stick around until pci.c is edited. Then
 * they should be moved to mod_loader_public.h.
 */

extern void Mod_RegisterDriver(void *linuxDrv);
extern void Mod_SetCurrent(void *linuxDrv);
extern void Mod_ResetCurrent(void);
extern void Mod_DoPostInsert(void *linuxDrv, struct PCI_Device *dev);
extern void Mod_DoPreRemove(void *linuxDrv, struct PCI_Device *dev);

/*
 * The following are what pci.c should eventually used.
 */


extern void vmk_ModLoaderRegisterDriver(void *linuxDriver);
extern void vmk_ModLoaderSetCurrent(void *linuxDriver);
extern void vmk_ModLoaderResetCurrent(void);
extern void vmk_ModLoaderDoPostInsert(void *linuxDriver, struct PCI_Device *dev);
extern void vmk_ModLoaderDoPreRemove(void *linuxDriver, struct PCI_Device *dev);

#endif // _MOD_LOADER_DIST_H
