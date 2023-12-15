/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * vmksysinfo.h -
 *
 *	Generate enumerations for the control handlers.
 */
#ifndef _VMK_CONTROL_H_
#define _VMK_CONTROL_H_
#include "vmksysinfo_table.h"
#include "return_status.h"

#define DECL_SET_HANDLER(funcId, dispatchFunc, type1, args1)                  \
     VMKSYSINFO_SET_##funcId,

#define DECL_GET_HANDLER(funcId, dispatchFunc, type1, args1, type2, args2)

typedef enum {
   VMKSYSINFO_SET_NONE,
   VMKSYSINFO_DISPATCH_TABLE
   MAX_SYSINFO_SET_DESC
} VmkSysInfoSetDesc;

#undef DECL_SET_HANDLER
#undef DECL_GET_HANDLER

#define DECL_GET_HANDLER(funcId, dispatchFunc, type1, args1, type2, args2)    \
     VMKSYSINFO_GET_##funcId,

#define DECL_SET_HANDLER(funcId, dispatchFunc, type1, args1)

typedef enum {
   VMKSYSINFO_GET_NONE = MAX_SYSINFO_SET_DESC,
   VMKSYSINFO_DISPATCH_TABLE
   MAX_SYSINFO_GET_DESC
} VmkSysInfoGetDesc;

extern VMK_ReturnStatus VSI_GetInfoOld(void *inBuf, void *outBuf, int outBufLen);
extern VMK_ReturnStatus VSI_SetInfoOld(char *inBuf);
		 
extern VMK_ReturnStatus VSI_GetInfo(void *infoHost, void *outBuf, int outBufLen);
extern VMK_ReturnStatus VSI_SetInfo(void *infoHost, void *inputArgsHost);
extern VMK_ReturnStatus VSI_GetList(void *infoHost, void *outBuf, int outBufLen);
		 
#undef DECL_GET_HANDLER
#undef DECL_SET_HANDLER
#endif
