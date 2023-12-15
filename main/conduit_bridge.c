/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * conduit_bridge.c --
 *
 *      vmkernel function boilerplate for conduit subsystem 
 *	module interfaces.
 */

#include "vm_types.h"
#include "vmkernel.h"
#include "world.h"
#include "conduit_bridge.h"
#include "vm_libc.h"

#define LOGLEVEL_MODULE Conduit
#define LOGLEVEL_MODULE_LEN 8
#include "log.h"



static ConduitBridgeFnTable conduitFns;

/*
 *----------------------------------------------------------------------
 *
 * Conduit_<name>  --
 *
 *    Generated bridge functions that the vmkernel calls through
 *    to access the Conduit module.  If the Conduit module isn't
 *    loaded, the default return value specifid in the function 
 *    generator table will be returned.
 *
 *    These calls are handled by the Conduit_<name> functions
 *    in modules/vmkernel/conduit/conduit.c
 *
 * Results: 
 *	result of function called.
 *
 * Side effects:
 *      You thought system behavior was complex before loadable modules.
 *
 *----------------------------------------------------------------------
 */

#define OP(_name, _retType, _retval, _nArgs, _args...)          \
_retType Conduit_##_name(JOIN_ARGS##_nArgs(_args))              \
{                                                               \
   if (conduitFns._name) {                                      \
      return conduitFns._name(SEL_ARGS##_nArgs(_args));         \
   }                                                            \
   return _retval;                                              \
}

CONDUIT_BRIDGE_FUNCTION_GENERATOR()
#undef OP

static uint32 vmkernelConduitModVersion = CONDUIT_MODULE_VERSION;


/*
 *----------------------------------------------------------------------
 *
 * ConduitBridge_RegisterFunctions  --
 *
 *      Updates the conduit function table to point into the
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
ConduitBridge_RegisterFunctions(uint32 moduleVersion, ConduitBridgeFnTable *fns)
{
   if (VERSION_MAJOR(moduleVersion) != 
       VERSION_MAJOR(vmkernelConduitModVersion)) {
      Log("Major version mismatch vmk: %d module: %d",
          VERSION_MAJOR(vmkernelConduitModVersion),
          VERSION_MAJOR(moduleVersion));
      return VMK_VERSION_MISMATCH_MAJOR;
   }

   if (VERSION_MINOR(moduleVersion) != 
       VERSION_MINOR(vmkernelConduitModVersion)) {
      Log("Minor version mismatch vmk: %d.%d module: %d.%d",
          VERSION_MAJOR(vmkernelConduitModVersion),
          VERSION_MINOR(vmkernelConduitModVersion),
          VERSION_MAJOR(moduleVersion),
          VERSION_MINOR(moduleVersion));
   } else {
      Log("Registering conduit module version %d.%d", 
          VERSION_MAJOR(vmkernelConduitModVersion),
          VERSION_MINOR(vmkernelConduitModVersion));
   }

   if (fns) {
      conduitFns = *fns;
   } else {
      memset(&conduitFns, 0, sizeof(conduitFns));
   }

   return VMK_OK;
}



