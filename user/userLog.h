/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * userLog.h --
 *
 *	UserWorld logging infrastructure.  Used like main/log.h.
 *
 * Deciphering the capitalization:
 *
 *	The UWLOG/UWLOGFor macros are logging-build-only macros, and
 *	will evaporate in CPP during a release build. (Except for the
 *	UWLOG_SyscallUnsupported and UWLOG_SyscallUnimplemented
 *	macros.)
 *
 *	UWLog/UWLogFor are all-build logging macros.  In release
 *	builds they don't have quite the same amount of information in
 *	the standard prefix, but they do show up.  They should be used
 *	primarily to dump state information during a crash.  They
 *	should also be relatively infrequent.
 *
 *	Use UWLOG_Enabled(logLevel) to test if logging is enabled in
 *	the current cartel||syscall||module.
 *
 * See http://vmweb.vmware.com/~mts/WebSite/guide/programming/printlog.html
 * for guidelines on when to use which macros.
 */

#ifndef VMKERNEL_USER_USERLOG_H
#define VMKERNEL_USER_USERLOG_H

#ifndef LOGLEVEL_MODULE
#error All user modules must define LOGLEVEL_MODULE
#endif

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "log.h"

struct VMKFullUserExcFrame;
struct User_LogContext;
struct User_ThreadInfo;

#define UWLOG_NOSYSCALL ((uint32)-1) /* see UWLOG_SetContextSyscall */
#define UWLOG_INTERRUPT ((uint32)-1) /* see UWLOG_SetContextException */

extern VMK_ReturnStatus UserLog_ThreadInit(struct User_ThreadInfo* uti);

// Log prefix components:
#define UWLOGModStr XSTR(LOGLEVEL_MODULE)
#define UWLOGFuncStr _LogFnName
#if VMX86_LOG
#define UWLOGFuncFmt "%s"
#else
#define UWLOGFuncFmt "%d"
#endif

/*
 * Current thread's logging prefix.  See UWLOG_SetContext*.  Includes
 * trailing ": " if not empty.  "" in non-logging builds.
 */
static INLINE const char*
_UWLOGContext(World_Handle* w)
{
#if VMX86_LOG
   if (w == NULL) {
      return "<null>: ";
   } else if (World_IsUSERWorld(w)) {
      if (w->userThreadInfo != NULL) {
         return w->userThreadInfo->logContext.prefix;
      } else {
         return "<n/a>: ";
      }
   } else if (World_IsHELPERWorld(w)) {
      return "<hlpr>: ";
   } else if (World_IsHOSTWorld(w)) {
      return "<host>: ";
   }
   // Other world types
   return "<other>: ";
#else
   return "";
#endif   
}


/*
 * Userworld-specific variant of VmWarn.
 */
#define UWWarn(fmt, args...)                            \
   _Warning("%s%s: " UWLOGFuncFmt ": " fmt "\n",        \
            _UWLOGContext(MY_RUNNING_WORLD),            \
            UWLOGModStr, UWLOGFuncStr , ## args)


/*
 * Warn about an unsupported syscall
 */
#define UWLOG_SyscallUnsupported(fmt, args...)  \
   do {                                         \
      UWWarn("unsupported: " fmt , ## args);    \
      UWLOG_StackTraceCurrent(1);               \
   } while (0)


/*
 * Warn about an unimplemented syscall
 */
#define UWLOG_SyscallUnimplemented(fmt, args...)        \
   do {                                                 \
      UWWarn("UNIMPLEMENTED!  " fmt , ## args);         \
      UWLOG_StackTraceCurrent(1);                       \
   } while (0)

/*
 * Remainder of header file is logging functions that are turned off
 * in slimmer builds.
 */

#ifdef VMX86_LOG

EXTERN void UserLog_StackTrace(const struct VMKFullUserExcFrame* frame);
EXTERN void UserLog_FullExcFrame(const char* label1,
                                 const char* label2,
                                 const struct VMKFullUserExcFrame* ctx1,
                                 const struct VMKFullUserExcFrame* ctx2);
EXTERN void UserLog_DumpBuffer(uint8* buf, unsigned length);


/*
 * Lookup current syscall in per-syscall logging control table.
 */
static INLINE Bool
_UWLOGDoCurrentSyscall(void)
{
   /* NOT IMPLEMENTED. */
   return FALSE;
}


/*
 * Lookup current cartel in logging controls (to allow per-cartel
 * logging).
 */
static INLINE Bool
_UWLOGDoCurrentCartel(void)
{
   /* NOT IMPLEMENTED */
   return FALSE;
}

/*
 * Check if the current context and given level imply logging is
 * enabled.  DOLOG is from vmkernel/public/log_defines.h
 */
static INLINE Bool
UWLOG_Enabled(int logLevel) {
   return (DOLOG(logLevel)
           || _UWLOGDoCurrentSyscall()
           || _UWLOGDoCurrentCartel());
}

/*
 * If UserWorld logging is enabled, print given fmt via _Log.
 */
#define _UWLOGDoLog(logLevel, fmt, args...)     \
   do {                                         \
      if (UWLOG_Enabled(logLevel)) {            \
         _Log(fmt , ## args);                   \
      }                                         \
   } while(0) 


/*
 * Generic logging on behalf of a specific (different) world.
 */
#define UWLOGFor(logLevel, world, fmt, args...)                 \
   do {                                                         \
      if ((world) == MY_RUNNING_WORLD) {                        \
         UWLOG(logLevel, fmt , ## args);                        \
      } else {                                                  \
         _UWLOGDoLog((logLevel),                                \
              "for %d: %s%s: " UWLOGFuncFmt ": " fmt "\n",      \
              (world) ? (world->worldID) : (INVALID_WORLD_ID),  \
              _UWLOGContext(world),                             \
              UWLOGModStr, UWLOGFuncStr , ## args);             \
      }                                                         \
   } while(0)
      

/*
 * Generic logging for the current world.
 */
#define UWLOG(logLevel, fmt, args...)                   \
   _UWLOGDoLog((logLevel),                              \
              "%s%s: " UWLOGFuncFmt ": " fmt "\n",      \
              _UWLOGContext(MY_RUNNING_WORLD),          \
              UWLOGModStr, UWLOGFuncStr , ## args)

/*
 * UWLog in logging builds is just UWLOG(0, ...).  But, it also shows
 * up in non-logging builds.
 */
#define UWLog(fmt, args...) \
   UWLOG(0, fmt , ## args)


/*
 * See UWLog
 */
#define UWLogFor(world, fmt, args...) \
   UWLOGFor(0, world, fmt , ## args)


/*
 * Log user-mode stack trace from given fullFrame
 */
static INLINE void
UWLOG_StackTrace(int logLevel, struct VMKFullUserExcFrame* fullFrame)
{
   if (UWLOG_Enabled(logLevel)) {
      UserLog_StackTrace(fullFrame);
   }
}


/*
 * Log user-mode stack trace from current world's current syscall
 * exceptionFrame.
 */
#define UWLOG_StackTraceCurrent(logLevel)                               \
   do {                                                                 \
      if (UWLOG_Enabled((logLevel))) {                                  \
         if (World_IsUSERWorld(MY_RUNNING_WORLD)) {                     \
            if ((MY_USER_THREAD_INFO != NULL)                           \
                && (MY_USER_THREAD_INFO->exceptionFrame != NULL)) {     \
               UserLog_StackTrace(MY_USER_THREAD_INFO->exceptionFrame); \
            } else {                                                    \
               UWLOG((logLevel), "<no active UW syscall for stack trace>"); \
            }                                                           \
         } else {                                                       \
            UWLOG((logLevel), "Current world NOT A USERWORLD.");        \
         }                                                              \
      }                                                                 \
   } while (0)


/*
 * Log given ExcFrames (2nd may be NULL)
 */
static INLINE void
UWLOG_FullExcFrame(int logLevel,
                   const char* label1, const char* label2, 
                   const struct VMKFullUserExcFrame* ctx1,
                   const struct VMKFullUserExcFrame* ctx2)
{
   if (UWLOG_Enabled(logLevel)) {
      UserLog_FullExcFrame(label1, label2, ctx1, ctx2);
   }
}


/*
 * Log a hex-dump of buffer
 */
static INLINE void
UWLOG_DumpBuffer(int logLevel, uint8* buf, unsigned length)
{
   if (UWLOG_Enabled(logLevel)) {
      UserLog_DumpBuffer(buf, length);
   }
}


/*
 * Log entry to a linux emulation syscall
 */
#define UWLOG_SyscallEnter(fmt, args...) \
   UWLOG(1, "<enter>: " fmt , ## args)


EXTERN const char* UWLOG_ReturnStatusToString(VMK_ReturnStatus status);
EXTERN void UWLOG_SetContextSyscall(Bool linuxCall, uint32 syscallnum);
EXTERN void UWLOG_SetContextException(int info);
EXTERN void UWLOG_ClearContext(void);

#else
/*
 * No-op versions of UWLOG statements for non-debug builds.
 */
#define UWLOG_Enabled(l)                            (0)
#define UWLOG(l, f, a...)                           ((void)0)
#define UWLOGFor(l, w, f, a...)                     ((void)0)
#define UWLOG_StackTrace(l, e)                      ((void)0)
#define UWLOG_StackTraceCurrent(l)                  ((void)0)
#define UWLOG_FullExcFrame(l, l1, l2, c1, c2)       ((void)0)
#define UWLOG_DumpBuffer(l, b, s)                   ((void)0)
#define UWLOG_SyscallEnter(f, a...)                 ((void)0)
#define UWLOG_ReturnStatusToString(s)               VMK_ReturnStatusToString(s)
#define UWLOG_SetContextSyscall(l, n)               ((void)0)
#define UWLOG_SetContextException(i)                ((void)0)
#define UWLOG_ClearContext()                        ((void)0)
#define UWLog(fmt, args...) \
	Log(fmt , ## args)
#define UWLogFor(world, fmt, args...) \
	Log("for %d: " fmt, ((world)?(world)->worldID:-1) , ## args)

#endif /* VMX86_LOG */

#endif /* VMKERNEL_USER_USERLOG_H */
