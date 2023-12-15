/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * mod_loader_public.h --
 *
 *	Module loader module.
 */


#ifndef _MOD_LOADER_PUBLIC_H
#define _MOD_LOADER_PUBLIC_H

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMK_MODULE
#include "includeCheck.h"

#include "splock.h"
#include "pci_dist.h"
#include "heap_public.h"

/*
 * lock rank
 */
#define SP_RANK_MODLOCK  (SP_RANK_IRQ_MEMTIMER)

#define MOD_ID_NONE	0
#define MOD_ID_UNKNOWN	-1
#define MOD_VERSION_STRING_LENGTH 64

extern void Mod_SetHeapID(uint32 moduleID, Heap_ID heap);
extern Heap_ID Mod_GetHeapID(uint32 moduleID);

extern uint32 Mod_GetCurrentID(void);
extern VMK_ReturnStatus Mod_IncUseCount(int moduleID);
extern VMK_ReturnStatus Mod_DecUseCount(int moduleID);
extern void Mod_RegisterDriver(void *linuxDrv);
extern void Mod_SetCurrent(void *linuxDrv);
extern void Mod_ResetCurrent(void);
extern void Mod_DoPostInsert(void *linuxDrv, PCI_Device *dev);
extern void Mod_DoPreRemove(void *linuxDrv, PCI_Device *dev);
extern void Mod_SetModuleVersionInt(char *fmt, ...);
extern void Mod_SetModuleVersionExt(char *info, uint32 len);
#define Mod_SetModuleVersion(_fmt, _args...) \
    Mod_SetModuleVersionInt("%d: " _fmt, BUILD_NUMBER_NUMERIC , ##_args)
#endif
