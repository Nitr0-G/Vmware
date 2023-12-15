/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * conduitBridge.h --
 *
 *      Conduit bridge header.
 */

#ifndef _CONDUITSTTUB_H_
#define _CONDUITSTTUB_H_

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMK_MODULE
#include "includeCheck.h"

#include "conduit_ext.h"
#include "moduleCommon.h"

struct Async_Token;

#define CONDUIT_MODULE_VERSION MAKE_VERSION(1,0)

/*
 * Entry points to the conduit module.
 *
 * Conduit_<name> calls are mapped to Conduit_<name> definitions
 * in the conduit module.  If the conduit module isn't loaded,
 * the default return value is returned.
 *
 * Usage: Op(<name>, return type, default return value, # args, arg list)
 */

#define CONDUIT_BRIDGE_FUNCTION_GENERATOR()                                                \
   OP(WorldInit, VMK_ReturnStatus, VMK_OK, 2,                                              \
      World_Handle *, world, World_InitArgs *, args)                                       \
   OP(WorldPreCleanup, void,, 1, World_Handle *, world)                                    \
   OP(WorldCleanup, void,, 1, World_Handle *, world)                                       \
   OP(DeviceMemory, VMK_ReturnStatus, VMK_NOT_SUPPORTED, 2,                                \
      Conduit_HandleID,  handleID, Conduit_DeviceMemoryCmd *, hostArgs)                    \
   OP(CreateAdapter, VMK_ReturnStatus, VMK_NOT_SUPPORTED, 3,                               \
      VMnix_CreateConduitAdapArgs *, args, Conduit_ClientType, clientType,                 \
      Conduit_HandleID *, result)                                                          \
   OP(Enable, VMK_ReturnStatus, VMK_NOT_SUPPORTED, 3,                                      \
      Conduit_HandleID, AdapterHandleID, World_ID, worldID,                                \
      Conduit_HandleEnableArgs *, args)                                                    \
   OP(VMXDisable, VMK_ReturnStatus, VMK_NOT_SUPPORTED, 1, Conduit_HandleID, HostHandleID)  \
   OP(HostGetConduitVersion, uint32, 0, 1, Conduit_HandleID *, hostHandleID)               \
   OP(HostNewPipe, VMK_ReturnStatus, VMK_NOT_SUPPORTED, 3, Conduit_HandleID, handleID,     \
      Conduit_ClientType, clientType, Conduit_OpenPipeArgs *, openArgs)                    \
   OP(Transmit, VMK_ReturnStatus, VMK_NOT_SUPPORTED, 2, Conduit_HandleID, handleID,        \
      World_Handle *, world)                                                               \
   OP(DevInfo, VMK_ReturnStatus, VMK_NOT_SUPPORTED, 3,                              \
      Conduit_HandleID, handleID, World_ID, worldID, CnDev_Record *, rec)                  \
   OP(HostRemovePipe, VMK_ReturnStatus, VMK_NOT_SUPPORTED, 3,                              \
      Conduit_HandleID, hostHandleID, World_ID, worldID, Conduit_HandleID, pipeID)         \
   OP(RemoveAdapter, void,, 2, World_ID, worldID, Conduit_HandleID, handleID)              \
   OP(CnDevConfigDeviceForWorld, VMK_ReturnStatus, VMK_NOT_SUPPORTED, 3,                   \
      VMnix_ConduitConfigDevForWorldArgs *, configArgs,                                    \
      CnDev_Numerics *, nbuf, CnDev_Strings *, sbuf)                                       \
   OP(GetBackingStore, VMK_ReturnStatus, VMK_NOT_SUPPORTED, 3,                             \
      World_Handle *, world, uint32, offset, MPN *, allocMPN)                              \
   OP(GetCapabilities,VMKERNEL_ENTRY, VMK_MODULE_NOT_LOADED, 2,                            \
      uint32, fn, va_list, args)                                                           \
   OP(SignalDev, VMKERNEL_ENTRY, VMK_MODULE_NOT_LOADED, 2,                                 \
      uint32, fn, va_list, args)                                                           \
   OP(VMMTransmit, VMKERNEL_ENTRY, VMK_MODULE_NOT_LOADED, 2,                               \
      uint32, fn, va_list, args)                                                           \
   OP(LockPage, VMKERNEL_ENTRY, VMK_MODULE_NOT_LOADED, 2,                                  \
      uint32, fn, va_list, args)                                                           \
   OP(ModuleEnable, VMK_ReturnStatus, VMK_MODULE_NOT_LOADED, 3,                            \
      Bool, write, Bool, valueChanged, int, index)                                         \



	 


/*
 * Define prototypes for the Conduit module entry points
 */

#define OP(_name, _retType, _retval, _nArgs, _args...)          \
   extern _retType Conduit_##_name(SEL_ARGS##_nArgs(foo,_args));
CONDUIT_BRIDGE_FUNCTION_GENERATOR()
#undef OP


/*
 * Conduit module function table typedef
 */
#define OP(_name, _retType, _retval, _nArgs, _args...) \
   _retType (*_name)(SEL_ARGS##_nArgs(foo,_args));
typedef struct {
   CONDUIT_BRIDGE_FUNCTION_GENERATOR()
} ConduitBridgeFnTable;
#undef OP


extern VMK_ReturnStatus ConduitBridge_RegisterFunctions(uint32 moduleVersion, 
                                                        ConduitBridgeFnTable *fns);

#endif
