
#ifndef _RPC_CALLS_H
#define _RPC_CALLS_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMMEXT
#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMCORE
#include "includeCheck.h"

/*
 * RPC functions
 */
#define RPC_FUNCTION_LOG	0
#define RPC_FUNCTION_WARNING	1
#define RPC_FUNCTION_PANIC	2
#define RPC_FUNCTION_TEST1	3
#define RPC_FUNCTION_TEST2	4
#define RPC_FUNCTION_OPEN_LOG	5
#define RPC_FUNCTION_CLOSE_LOG	6

typedef struct RPC_OpenLogArgs {
   int		truncate;
   char 	fileName[0];
} RPC_OpenLogArgs;

#endif
