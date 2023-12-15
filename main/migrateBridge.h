/* **********************************************************
 * Copyright 2002 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * migrateBridge.h --
 *
 *      Migration bridge header.
 */

#ifndef _MIGRATESTTUB_H_
#define _MIGRATESTTUB_H_

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMK_MODULE
#include "includeCheck.h"

#include "migrate_ext.h"
#include "moduleCommon.h"

struct Async_Token;

#define MIG_MODULE_VERSION MAKE_VERSION(3,0)

/*
 * Entry points to the migration module.
 *
 * Migrate_<name> calls are mapped to Migrate_<name> definitions
 * in the migration module.  If the migration module isn't loaded,
 * the default return value is returned.
 *
 * Usage: Op(<name>, return type, default return value, # args, arg list)
 */

#define MIGRATE_BRIDGE_FUNCTION_GENERATOR()                                                \
   OP(WorldCleanup, void,, 1, World_Handle *, world)                                       \
   OP(NukePageInt, void,, 2, World_Handle *,world, PPN, page)                              \
   OP(MarkCheckpoint, void,, 1, VMnix_MarkCheckpointArgs *, hostArgs)                      \
   OP(Enable, VMK_ReturnStatus, VMK_MODULE_NOT_LOADED, 3,                                  \
      Bool, write, Bool, valueChanged, int, index)                                         \
   OP(ReadPage, VMK_ReturnStatus, VMK_NOT_SUPPORTED, 4,                                    \
      World_Handle *,world, uint64, offset, MPN, mpn, struct Async_Token *,token)          \
   OP(PreCopy, VMKERNEL_ENTRY, VMK_MODULE_NOT_LOADED, 2, uint32, fn, va_list, args)        \
   OP(PreCopyWrite, VMKERNEL_ENTRY, VMK_MODULE_NOT_LOADED, 2, uint32, fn, va_list, args)   \
   OP(PreCopyDone, VMKERNEL_ENTRY, VMK_MODULE_NOT_LOADED, 2, uint32, fn, va_list, args)    \
   OP(GetFailure, VMKERNEL_ENTRY, VMK_MODULE_NOT_LOADED, 2, uint32, fn, va_list, args)     \
   OP(RestoreDone, VMKERNEL_ENTRY, VMK_OK, 2, uint32, fn, va_list, args)                   \
   OP(PreCopyStart, VMKERNEL_ENTRY, VMK_MODULE_NOT_LOADED, 2, uint32, fn, va_list, args)   \
   OP(Continue, VMKERNEL_ENTRY, VMK_MODULE_NOT_LOADED, 2, uint32, fn, va_list, args)       \
   OP(ReadCptData, VMK_ReturnStatus, VMK_MODULE_NOT_LOADED, 2,                             \
      VMnix_MigCptDataArgs *, args, Util_BufferType, bufType)                              \
   OP(WriteCptData, VMK_ReturnStatus, VMK_MODULE_NOT_LOADED, 2,                            \
      VMnix_MigCptDataArgs *, args, Util_BufferType, bufType)                              \
   OP(ToBegin,VMK_ReturnStatus, VMK_MODULE_NOT_LOADED, 2,                                  \
      World_ID, toWorldID, VMnix_MigrateProgressResult *, progress)                        \
   OP(CheckProgress,VMK_ReturnStatus, VMK_MODULE_NOT_LOADED, 2,                            \
      VMnix_MigrateProgressArgs *, args,                                                   \
      VMnix_MigrateProgressResult *, progress)                                             \
   OP(SetParameters, VMK_ReturnStatus, VMK_MODULE_NOT_LOADED, 1,                           \
      VMnix_MigrationArgs *, hostArgs)                                                     \
   OP(SaveMemory, VMK_ReturnStatus, VMK_MODULE_NOT_LOADED, 1, World_ID, worldID)           \
   OP(MemSchedDeferred, Bool, FALSE, 3, World_Handle *, world, uint32,                     \
      min, Bool, automin)                                                                  \


/*
 * Define prototypes for the migration module entry points
 */

#define OP(_name, _retType, _retval, _nArgs, _args...)          \
   extern _retType Migrate_##_name(SEL_ARGS##_nArgs(foo,_args));
MIGRATE_BRIDGE_FUNCTION_GENERATOR()
#undef OP


/*
 * Migration module function table typedef
 */
#define OP(_name, _retType, _retval, _nArgs, _args...) \
   _retType (*_name)(SEL_ARGS##_nArgs(foo,_args));
typedef struct {
   MIGRATE_BRIDGE_FUNCTION_GENERATOR()
} MigrateBridgeFnTable;
#undef OP


extern VMK_ReturnStatus MigrateBridge_RegisterFunctions(uint32 moduleVersion, 
                                                        MigrateBridgeFnTable *fns);

#define MI(world) (World_VMMGroup(world)->migrateInfo)

/*
 *----------------------------------------------------------------------
 *
 * MigrateBridge_NukePage --
 *
 *      Net code modifies guest memory in a way that bypasses the
 *      traces set up in the monitor.  This is will confuse any attempt 
 *      to do a memory checksum comparison, and will have correctness 
 *      issues as well.
 *
 *      This function is special cased because it is called from the 
 *      fast networking path, and I didn't want to slow that code down 
 *      with an indirect function call.
 *
 * Results: 
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE void
MigrateBridge_NukePage(World_Handle *world, PPN page)
{
   if (UNLIKELY(MI(world) != NULL)) {
      Migrate_NukePageInt(world, page);
   }
}
#endif
