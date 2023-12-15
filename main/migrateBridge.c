/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * migrateBridge.c --
 *
 *      vmkernel bridge for hot migration.
 */

#include "vm_types.h"
#include "vmkernel.h"
#include "world.h"
#include "migrateBridge.h"
#include "vm_libc.h"

#define LOGLEVEL_MODULE Migrate
#define LOGLEVEL_MODULE_LEN 13
#include "log.h"


static MigrateBridgeFnTable migFns;

/*
 *----------------------------------------------------------------------
 *
 * Migrate_<name>  --
 *
 *    Generated bridge functions that the vmkernel calls through
 *    to access the migration module.  If the migration module isn't
 *    loaded, the default return value specifid in the function 
 *    generator table will be returned.
 *
 *    These calls are handled by the Migrate_<name> functions
 *    in modules/vmkernel/migration/migrate.c
 *
 * Results: 
 *	result of function called.
 *
 * Side effects:
 *      myriad.
 *
 *----------------------------------------------------------------------
 */
#define OP(_name, _retType, _retval, _nArgs, _args...)          \
_retType Migrate_##_name(JOIN_ARGS##_nArgs(_args))              \
{                                                               \
   if (migFns._name) {                                          \
      return migFns._name(SEL_ARGS##_nArgs(_args));             \
   }                                                            \
   return _retval;                                              \
}

MIGRATE_BRIDGE_FUNCTION_GENERATOR()
#undef OP

static uint32 vmkernelMigModVersion = MIG_MODULE_VERSION;


/*
 *----------------------------------------------------------------------
 *
 * MigrateBridge_RegisterFunctions  --
 *
 *      Updates the migration function table to point into the
 *      module.
 *
 *
 *      XXX this needs to be much smarter.  See bug 37227
 *
 * Results: 
 *	A VMK_ReturnStatus value.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus 
MigrateBridge_RegisterFunctions(uint32 moduleVersion, MigrateBridgeFnTable *fns)
{
   if (VERSION_MAJOR(moduleVersion) != 
       VERSION_MAJOR(vmkernelMigModVersion)) {
      Log("Major version mismatch vmk: %d module: %d",
          VERSION_MAJOR(vmkernelMigModVersion),
          VERSION_MAJOR(moduleVersion));
      return VMK_VERSION_MISMATCH_MAJOR;
   }

   if (VERSION_MINOR(moduleVersion) != 
       VERSION_MINOR(vmkernelMigModVersion)) {
      Log("Minor version mismatch vmk: %d.%d module: %d.%d",
          VERSION_MAJOR(vmkernelMigModVersion),
          VERSION_MINOR(vmkernelMigModVersion),
          VERSION_MAJOR(moduleVersion),
          VERSION_MINOR(moduleVersion));
   } else {
      Log("Registering migration module version %d.%d", 
          VERSION_MAJOR(vmkernelMigModVersion),
          VERSION_MINOR(vmkernelMigModVersion));
   }

   if (fns) {
      migFns = *fns;
   } else {
      memset(&migFns, 0, sizeof(migFns));
   }

   return VMK_OK;
}
