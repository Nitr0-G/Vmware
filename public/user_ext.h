/* **********************************************************
 * Copyright 2000 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * user_ext.h --
 *
 *	External definitions for the user module.
 */


#ifndef _USER_EXT_H
#define _USER_EXT_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "rateconv.h"

#define MAX_DESC_LEN			64

#define USER_MAX_FNAME_LENGTH		256
#define USER_MAX_STRING_LENGTH		128
#define USER_MAX_DUMPNAME_LENGTH	256

typedef enum {
   USER_MSG_PREEXIT,
   USER_MSG_POSTEXIT,
   USER_MSG_BREAK,
   USER_MSG_ERROR,
   USER_MSG_END
} User_MessageType;

/*
 * Used by the vmkernel to tell the proxy that the UserWorld has exited.  If
 * it's exiting because of some exception, register state is also passed along.
 */
typedef struct User_PostExitInfo {
   User_MessageType type;
   int32	status;
   Bool		wasException;
   Bool		coreDump;
   char		coreDumpName[USER_MAX_DUMPNAME_LENGTH];
   uint32	exceptionType;
   uint32	cs;   
   uint32	eip;
   uint32	ss;
   uint32	esp;
   uint32 	ds;
   uint32 	es;   
   uint32 	fs;
   uint32 	gs;
   uint32 	eax;
   uint32 	ebx;   
   uint32 	ecx;
   uint32 	edx;
   uint32 	ebp;
   uint32 	esi;
   uint32 	edi;
} User_PostExitInfo;

/*
 * Used by the UserWorld debugger to tell the proxy/user that a debugging
 * session has started.  The 'listeningOn' field is used to tell the user what
 * port/ip address/etc the debugger is listening on.
 */
typedef struct User_DebuggerInfo {
   User_MessageType type;
   char		listeningOn[MAX_DESC_LEN];
} User_DebuggerInfo;

/*
 * Used by User_WorldStart to tell the proxy about errors it encountered.  One
 * peculiarity about Linux error messages is that they're 'opaque' inside the
 * vmkernel.  That is, they really only have meaning in Linux (see
 * vmkernel/public/return_status.h).  So, instead of just concatenating a string
 * version of the errno in the vmkernel, we have to pass the errno out and have
 * the proxy do it.  If err is non-zero, it is converted to a string error,
 * otherwise it is ignored.
 */
typedef struct User_ErrorMsg {
   User_MessageType type;
   int		err;
   char		str[USER_MAX_STRING_LENGTH];
} User_ErrorMsg;

/*
 * Used by User_CartelKill to indicate to the proxy that the cartel is
 * shutting down and it should kick all threads out of the proxy.
 */
typedef struct User_PreExitMsg {
   User_MessageType type;
} User_PreExitMsg;

#endif
