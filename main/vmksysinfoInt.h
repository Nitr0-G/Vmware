/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * vmksysinfoInt.h  -
 *
 *	Generate prototypes for the control handlers.
 *	Also, generate wrapper functions that invoke these
 *	handlers after a simple check.
 */
#ifndef _VMKSYSINFO_INT_H_
#define _VMKSYSINFO_INT_H_
#include "vmksysinfo_table.h"

/* This is the signature for the wrappers */
typedef VMK_ReturnStatus (*SetHandler)(void *, unsigned int);
typedef VMK_ReturnStatus (*GetHandler)(void *, unsigned int, void *, unsigned int);

#define DECL_SET_HANDLER(funcId, dispatchFunc, type1, args1)                  \
	extern VMK_ReturnStatus dispatchFunc(type1 *args1);

#define DECL_GET_HANDLER(funcId, dispatchFunc, type1, args1, type2, args2)    \
	extern VMK_ReturnStatus dispatchFunc(type1 *args1, type2 *args2,      \
                                             unsigned long outArgsLen);

// generate extern prototypes for the actual handlers.
VMKSYSINFO_DISPATCH_TABLE

#undef DECL_SET_HANDLER
#undef DECL_GET_HANDLER

/* The _##dispatchFunc functions will not be inlined as their
 * addresses are used in the jump table. However, still declaring
 * them as inline just in case some other function calls them directly.
 */
#define DECL_SET_HANDLER(funcId, dispatchFunc, type1, args1)                  \
	static inline VMK_ReturnStatus _##dispatchFunc(void *args1,           \
					    unsigned int argsLen)             \
	{                                                                     \
	   ASSERT(argsLen >= sizeof(type1));                                  \
	   if (argsLen < sizeof(type1)) {                                     \
	      return VMK_BAD_PARAM;                                           \
	   }                                                                  \
	   return dispatchFunc((type1 *)args1);                               \
	}

#define DECL_GET_HANDLER(funcId, dispatchFunc, type1, args1, type2, args2)    \
	static inline VMK_ReturnStatus _##dispatchFunc(void *args1,           \
					    unsigned int inArgsLen,           \
					    void *args2,                      \
					    unsigned int outArgsLen)          \
	{                                                                     \
	   if (!args1 && inArgsLen != 0) {                                    \
	      NOT_REACHED();                                                  \
              return VMK_BAD_PARAM;                                           \
           }else if(inArgsLen < sizeof(type1)) {                              \
              NOT_REACHED();                                                  \
              return VMK_BAD_PARAM;                                           \
	   }                                                                  \
	   ASSERT(outArgsLen >= sizeof(type2));                               \
	   if (outArgsLen < sizeof(type2)) {                                  \
	      return VMK_BAD_PARAM;                                           \
	   }                                                                  \
	   return dispatchFunc((type1 *)args1, (type2 *)args2, outArgsLen);   \
	}

/*
 * generate simple wrappers that, after a preliminary check, typecast the (void *)
 * param to what the corresponding handler expects.
 */
VMKSYSINFO_DISPATCH_TABLE

#undef DECL_GET_HANDLER
#undef DECL_SET_HANDLER
#endif //_VMKSYSINFO_INT_H_
