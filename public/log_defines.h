/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * log_defines.h --
 *
 *	vmkernel logging macros
 */

#ifndef _LOG_DEFINES_H
#define _LOG_DEFINES_H

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_DISTRIBUTE
#define INCLUDE_ALLOW_VMK_MODULE
#define INCLUDE_ALLOW_MODULE
#include "includeCheck.h"


#if !defined(LOGLEVEL_MODULE)
#error LOGLEVEL_MODULE must be defined \
       to use Warning, Log, SysAlert, LOG, etc.
/* 
 * LOGLEVEL_MODULE should be the name of the module as will be printed
 * in the log, and show up in /proc/vmware/loglevels. 
 *
 * LOGLEVEL_MODULE_LEN is the length of the module name prefix that should
 * (by the coding convention) appear at the beginning of every function
 * name in the file.  The macros below use this to chop the prefix off when
 * printing the function name.  If your module doesn't following coding
 * convention, or for some reason you don't want this the followin options
 * are available:
 *
 * 1) set LOGLEVEL_MODULE_LEN to 0 (like linux_stubs.c)
 * 2) Don't use this file.  Instead use the underscore varients
 *    (_LOG, _Log, _Warning).
 * 
 * If you don't set LOGLEVEL_MODULE_LEN, this header will compute the
 * length automatically, based on the sizeof the LOGLEVEL_MODULE constant.
 */
#endif

#if !defined(LOGLEVEL_MODULE_LEN)
// sizeof computes length of char array, including \0, so subtract 1
#define LOGLEVEL_MODULE_LEN (sizeof(XSTR(LOGLEVEL_MODULE)) - 1)
#endif // LOGLEVEL_MODULE


#ifndef VMX86_LOG
#define LOG(_min, _fmt...)
#define _LOG(_min, _fmt... )
#define VMLOG(_min, _vmID, _fmt...)
#define _LogFnName __LINE__
#define _LogVMFmtPrefix XSTR(LOGLEVEL_MODULE)  ": vm %d: %d: "
#define _LogFmtPrefix XSTR(LOGLEVEL_MODULE) ": %d: "
#define LOGLEVEL()              0
#else /* !VMX86_LOG */
#define LOGLEVELINDEX(name)	LOGLEVEL_##name
#define LOGLEVELNAME(_name)	 (logLevelPtr[LOGLEVELINDEX(_name)])
#define LOGLEVEL()		 LOGLEVELNAME(LOGLEVEL_MODULE)
#define DOLOGNAME(_name, _min)	 (LOGLEVELNAME(_name) >= _min)
#define DOLOG(_min)           DOLOGNAME(LOGLEVEL_MODULE, _min)

#define _LogFnName (__FUNCTION__ + ((__FUNCTION__[LOGLEVEL_MODULE_LEN] == '_') \
                   ? LOGLEVEL_MODULE_LEN + 1 : LOGLEVEL_MODULE_LEN))
#define _LogVMFmtPrefix XSTR(LOGLEVEL_MODULE)  ": vm %d: %s: "
#define _LogFmtPrefix XSTR(LOGLEVEL_MODULE) ": %s: "

#define LOGNAME(_name, _min, _fmt, _args...)    \
   do {                                         \
      if (DOLOGNAME(_name, _min)) {             \
         _Log(_fmt , ##_args);                  \
      }                                         \
   } while (FALSE)

#define _LOG(_min, _fmt, _args...) LOGNAME(LOGLEVEL_MODULE,_min,_fmt , ##_args)

#define LOG(_min, _fmt, _args...) \
   _LOG(_min, _LogPrefix _LogFmtPrefix _fmt "\n", _LogFnName , ##_args)

#define VMLOG(_min, _vmID, _fmt, _args...) \
   _LOG(_min, _LogPrefix _LogVMFmtPrefix _fmt "\n", _vmID, _LogFnName , ##_args)
#endif /* VMX86_LOG */

#define Log(_fmt, _args...)  \
   _Log(_LogPrefix _LogFmtPrefix _fmt "\n", _LogFnName , ##_args);

#define VmLog(_vmID, _fmt, _args...)  \
   _Log(_LogPrefix _LogVMFmtPrefix _fmt "\n", _vmID, _LogFnName , ##_args);

#define Warning(_fmt, _args...) \
   _Warning(_WarningPrefix _LogFmtPrefix _fmt "\n", _LogFnName , ##_args)

#define VmWarn(_vmID, _fmt, _args...) \
   _Warning(_WarningPrefix _LogVMFmtPrefix _fmt "\n", _vmID, _LogFnName , ##_args)

#define WarnVmNotFound(_vmID)                 \
   VmWarn(_vmID, "vm not found")

#define SysAlert(_fmt, _args...) \
   _SysAlert(_SysAlertPrefix _LogFmtPrefix _fmt "\n", _LogFnName , ##_args)
#endif
