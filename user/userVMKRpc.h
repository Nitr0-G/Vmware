/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/* 
 * userVMKRpc.h - 
 *      UserWorld RPC objects
 */
  
#ifndef VMKERNEL_USER_USERVMKRPC_H
#define VMKERNEL_USER_USERVMKRPC_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "vm_basic_types.h"
#include "vmkernel.h"
#include "rpc.h"

struct User_CartelInfo;

extern VMK_ReturnStatus UserVMKRpc_Create(struct User_CartelInfo* uci,
                                          const char* cnxName,
                                          LinuxFd* cnxFD,
				          RPC_Connection* cnxID);

extern VMK_ReturnStatus UserVMKRpc_GetIDForFD(struct User_CartelInfo* uci,
					      LinuxFd fd,
					      RPC_Connection* id);

#endif
