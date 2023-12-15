/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * Table of system calls, to be included as needed (typically twice:
 * for the enum declaration, and for a string table for printing).
 *
 * Note:  When you add a new syscall or change an existing one, make
 * sure to update the version number in vmnix_syscall_dist.h.  Also
 * update the version if you change the order of calls in the list 
 * below.
 *
 * Usage: VMX_VMNIX_CALL(syscall name, handler name, vmk loaded?)
 *    syscall name:  name of the syscall.  VMNIX_ will automaticaly
 *                   be prepended
 *    handler name: Name of handler function in vmnix/module.c
 *    vmk loaded:   Does the vmkernel need to be loaded for this
 *                  system call to succeed.
 *
 * The system calls listed below are those referenced by the public
 * source code.
 */

#ifndef VMKERNEL_NOT_REQUIRED 
#define VMKERNEL_NOT_REQUIRED FALSE
#define VMKERNEL_REQUIRED TRUE
#endif


//System calls which don't require the vmkernel to be loaded
VMX_VMNIX_CALL(VERIFY_VERSION, VMnixVerifyVersion, VMKERNEL_NOT_REQUIRED)      //must be first  (for version checking to work)

//System calls which require the vmkernel to be loaded
VMX_VMNIX_CALL(MOD_ALLOC, ModAlloc, VMKERNEL_REQUIRED)
VMX_VMNIX_CALL(MOD_PUT_PAGE, ModPutPage, VMKERNEL_REQUIRED)
VMX_VMNIX_CALL(MOD_LOAD_DONE, ModLoadDone, VMKERNEL_REQUIRED)
VMX_VMNIX_CALL(MOD_UNLOAD, ModUnload, VMKERNEL_REQUIRED)
VMX_VMNIX_CALL(MOD_LIST, ModList, VMKERNEL_REQUIRED)
VMX_VMNIX_CALL(MOD_ADD_SYMBOL, ModAddSymbol, VMKERNEL_REQUIRED)
VMX_VMNIX_CALL(MOD_GET_SYMBOL, ModGetSymbol, VMKERNEL_REQUIRED)
#undef VMX_VMNIX_CALL
