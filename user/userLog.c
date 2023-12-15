/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * userLog.c --
 *
 *	UserWorld logging infrastructure.
 */

#include "user_int.h"
#include "uwvmkDispatch.h"
#include "user_layout.h"
#include "trace.h"

#define LOGLEVEL_MODULE UserLog
#include "userLog.h"

#if VMX86_LOG
/*
 * List of linux syscall names for pretty logging.
 */
const char* linuxSyscallNames[] = {
   [0] = "setup",
   [1] = "exit",
   [2] = "fork",
   [3] = "read",
   [4] = "write",
   [5] = "open",
   [6] = "close",
   [7] = "waitpid",
   [8] = "creat",
   [9] = "link",
   [10] = "unlink",
   [11] = "execve",
   [12] = "chdir",
   [13] = "time",
   [14] = "mknod",
   [15] = "chmod",
   [16] = "lchown16",
   [17] = "oldbreak",
   [18] = "stat",
   [19] = "lseek",
   [20] = "getpid",
   [21] = "mount",
   [22] = "oldumount",
   [23] = "setuid16",
   [24] = "getuid16",
   [25] = "stime",
   [26] = "ptrace",
   [27] = "alarm",
   [28] = "fstat",
   [29] = "pause",
   [30] = "utime",
   [31] = "oldstty",
   [32] = "oldgtty",
   [33] = "access",
   [34] = "nice",
   [35] = "ftime",
   [36] = "sync",
   [37] = "kill",
   [38] = "rename",
   [39] = "mkdir",
   [40] = "rmdir",
   [41] = "dup",
   [42] = "pipe",
   [43] = "times",
   [44] = "oldprof", 
   [45] = "brk",
   [46] = "setgid16",
   [47] = "getgid16",
   [48] = "signal",
   [49] = "geteuid16",
   [50] = "getegid16",
   [51] = "acct",
   [52] = "umount",
   [53] = "oldlock",
   [54] = "ioctl",
   [55] = "fcntl",
   [56] = "mpx",
   [57] = "setpgid",
   [58] = "oldulimit",
   [59] = "olduname",
   [60] = "umask",
   [61] = "chroot",
   [62] = "ustat",
   [63] = "dup2",
   [64] = "getppid",
   [65] = "getpgrp",
   [66] = "setsid",
   [67] = "sigaction",
   [68] = "sgetmask",
   [69] = "ssetmask",
   [70] = "setreuid16",
   [71] = "setregid16",
   [72] = "sigsuspend",
   [73] = "sigpending",
   [74] = "oldsethostname",
   [75] = "setrlimit",
   [76] = "oldgetrlimit",
   [77] = "getrusage",
   [78] = "gettimeofday",
   [79] = "settimeofday",
   [80] = "getgroups16",
   [81] = "setgroups16",
   [82] = "oldselect",
   [83] = "symlink",
   [84] = "oldstat",
   [85] = "readlink",
   [86] = "uselib",
   [87] = "swapon",
   [88] = "reboot",
   [89] = "readdir",
   [90] = "oldmmap",
   [91] = "munmap",
   [92] = "truncate",
   [93] = "oftruncate",
   [94] = "fchmod",
   [95] = "fchown",
   [96] = "getpriority",
   [97] = "setpriority",
   [98] = "oldprofil",
   [99] = "statfs",
   [100] = "fstatfs",
   [101] = "ioperm",
   [102] = "socketcall",
   [103] = "syslog",
   [104] = "setitimer",
   [105] = "getitimer",
   [106] = "newstat",
   [107] = "newlstat",
   [108] = "newfstat",
   [109] = "uname",
   [110] = "iopl",
   [111] = "vhangup",
   [112] = "idle",
   [113] = "vm86old",
   [114] = "wait4",
   [115] = "swapoff",
   [116] = "sysinfo",
   [117] = "ipc",
   [118] = "fsync",
   [119] = "sigreturn",
   [120] = "clone",
   [121] = "setdomainname",
   [122] = "newuname",
   [123] = "modify_ldt",
   [124] = "adjtimex",
   [125] = "mprotect",
   [126] = "sigprocmask",
   [127] = "create_module",
   [128] = "init_module",
   [129] = "delete_module",
   [130] = "get_kernel_syms",
   [131] = "quotactl",
   [132] = "getpgid",
   [133] = "fchdir",
   [134] = "bdflush",
   [135] = "sysfs",
   [136] = "personality",
   [137] = "afssyscall",
   [138] = "setfsuid16",
   [139] = "setfsgid16",
   [140] = "llseek",
   [141] = "getdents",
   [142] = "select",
   [143] = "flock",
   [144] = "msync",
   [145] = "readv",
   [146] = "writev",
   [147] = "getsid",
   [148] = "fdatasync",
   [149] = "sysctl",
   [150] = "mlock",
   [151] = "munlock",
   [152] = "mlockall",
   [153] = "munlockall",
   [154] = "sched_setparam",
   [155] = "sched_getparam",
   [156] = "sched_setscheduler",
   [157] = "sched_getscheduler",
   [158] = "sched_yield",
   [159] = "sched_get_priority_max",
   [160] = "sched_get_priority_min",
   [161] = "sched_rr_get_interval",
   [162] = "nanosleep",
   [163] = "mremap",
   [164] = "setresuid16",
   [165] = "getresuid16",
   [166] = "vm86",
   [167] = "query_module",
   [168] = "poll",
   [169] = "nfsservctl",
   [170] = "setresgid16",
   [171] = "getresgid16",
   [172] = "prctl",
   [173] = "rt_sigreturn",
   [174] = "rt_sigaction",
   [175] = "rt_sigprocmask",
   [176] = "rt_sigpending",
   [177] = "rt_sigtimedwait",
   [178] = "rt_sigqueueinfo",
   [179] = "rt_sigsuspend",
   [180] = "pread",
   [181] = "pwrite",
   [182] = "chown16",
   [183] = "getcwd",
   [184] = "capget",
   [185] = "capset",
   [186] = "sigaltstack",
   [187] = "sendfile",
   [188] = "getpmsg",
   [189] = "putpmsg",
   [190] = "vfork",
   [191] = "getrlimit",
   [192] = "mmap",
   [193] = "truncate64",
   [194] = "ftruncate64",
   [195] = "stat64",
   [196] = "lstat64",
   [197] = "fstat64",
   [198] = "lchown",
   [199] = "getuid",
   [200] = "getgid",
   [201] = "geteuid",
   [202] = "getegid",
   [203] = "setreuid",
   [204] = "setregid",
   [205] = "getgroups",
   [206] = "setgroups",
   [207] = "sys-207",
   [208] = "setresuid",
   [209] = "getresuid",
   [210] = "setresgid",
   [211] = "getresgid",
   [212] = "chown",
   [213] = "setuid",
   [214] = "setgid",
   [215] = "setfsuid",
   [216] = "setfsgid",
   [217] = "pivot_root",
   [218] = "mincore",
   [219] = "madvise",
   [220] = "getdents64",
   [221] = "fcntl64",
   [222] = "tux1",
   [223] = "tux2",
   [224] = "gettid",
   [225] = "readahead",
   [226] = "setxattr",
   [227] = "lsetxattr",
   [228] = "fsetxattr",
   [229] = "getxattr",
   [230] = "lgetxattr",
   [231] = "fgetxattr",
   [232] = "listxattr",
   [233] = "llistxattr",
   [234] = "flistxattr",
   [235] = "removexattr",
   [236] = "lremovexattr",
   [237] = "fremovexattr",
   [238] = "tkill",
   [239] = "sendfile64",
   [240] = "futex",
   [241] = "sched_setaffinity",
   [242] = "sched_getaffinity",
};

/*
 * Create a table mapping each (positive) linux errno to a string
 * constant.  See UWLOG_ReturnStatusToString.
 */
#define LC(ERRNO) [-LINUX_##ERRNO] = "Wrapped(linux " XSTR(ERRNO) ")"
const char* linuxStatusCodeNames[] = {
   LC(EPERM),LC(ENOENT),LC(ESRCH),LC(EINTR),LC(EIO),LC(ENXIO),LC(E2BIG),
   LC(ENOEXEC),LC(EBADF),LC(ECHILD),LC(EDEADLK),LC(ENOMEM),LC(EACCES),
   LC(EFAULT),LC(ENOTBLK),LC(EBUSY),LC(EEXIST),LC(EXDEV),LC(ENODEV),
   LC(ENOTDIR),LC(EISDIR),LC(EINVAL),LC(ENFILE),LC(EMFILE),LC(ENOTTY),
   LC(ETXTBSY),LC(EFBIG),LC(ENOSPC),LC(ESPIPE),LC(EROFS),LC(EMLINK),LC(EPIPE),
   LC(EDOM),LC(ERANGE),LC(EAGAIN),LC(EWOULDBLOCK),LC(EINPROGRESS),LC(EALREADY),
   LC(ENOTSOCK),LC(EDESTADDRREQ),LC(EMSGSIZE),LC(EPROTOTYPE),LC(ENOPROTOOPT),
   LC(EPROTONOSUPPORT),LC(ESOCKTNOSUPPORT),LC(EOPNOTSUPP),LC(EPFNOSUPPORT),
   LC(EAFNOSUPPORT),LC(EADDRINUSE),LC(EADDRNOTAVAIL),LC(ENETDOWN),
   LC(ENETUNREACH),LC(ENETRESET),LC(ECONNABORTED),LC(ECONNRESET),LC(ENOBUFS),
   LC(EISCONN),LC(ENOTCONN),LC(ESHUTDOWN),LC(ETOOMANYREFS),LC(ETIMEDOUT),
   LC(ECONNREFUSED),LC(ELOOP),LC(ENAMETOOLONG),LC(EHOSTDOWN),LC(EHOSTUNREACH),
   LC(ENOTEMPTY),LC(EUSERS),LC(EDQUOT),LC(ESTALE),LC(EREMOTE),
   LC(EBADRPC),LC(ERPCMISMATCH),LC(EPROGUNAVAIL),LC(EPROGMISMATCH),
   LC(EPROCUNAVAIL),LC(ENOLCK),LC(ENOSYS),LC(EFTYPE),LC(EAUTH),LC(ENEEDAUTH),
   LC(EIDRM),LC(ENOMSG),LC(EOVERFLOW),LC(ECANCELED),LC(EILSEQ),LC(ERESTARTSYS),
};
#undef LC

static void UserLogSetPrefix(User_LogContext* ctx, const char* fmt, ...);
#endif

/*
 *----------------------------------------------------------------------
 *
 * UserLog_ThreadInit --
 *	
 *	Initialize per-thread log context (no active syscall, set prefix
 *	to <init>).
 *
 * Results:
 *	VMK_OK
 *
 * Side effects:
 *	Initialize given uti.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserLog_ThreadInit(User_ThreadInfo* uti)
{
#if VMX86_LOG
   User_LogContext* ctx = &uti->logContext;

   ctx->linuxCall = TRUE;
   ctx->syscallNum = UWLOG_NOSYSCALL;
   ctx->prefix[0] = '\0';
   ctx->oprefix[0] = '\0';
   UserLogSetPrefix(ctx, "<init>: ");
#endif   
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserLogCopyIntIn --
 *	
 *	Utility function for UserLog_StackTrace.  Copies a single
 *	32-bit quantity from the given user VA and returns it.  Returns
 *	0 if there is a problem (and sets *status).
 *
 * Results:
 *	Value from the user address space at the given address.
 *
 * Side effects:
 *	May fault user pages in.
 *
 *----------------------------------------------------------------------
 */
static uint32
UserLogCopyIntIn(UserVA userSrc, VMK_ReturnStatus* status)
{
   uint32 kdest;

   if ((userSrc < VMK_USER_FIRST_TEXT_VADDR)
       || (userSrc > VMK_USER_LAST_VADDR)) {
      *status = VMK_INVALID_ADDRESS;
      kdest = 0;
   } else {
      *status = User_CopyIn(&kdest, userSrc, sizeof(uint32));
      if (*status != VMK_OK) {
         kdest = 0;
      }
   }
   return kdest;
}   


/*
 *----------------------------------------------------------------------
 *
 * UserLog_StackTrace --
 *
 *	Log a stack trace from the given exception frame (i.e., a trace
 *	of the user-mode code).
 *
 *	Since this copies data from user-mode via User_CopyIn, it must
 *	be invoked in a blocking-friendly context.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	A stack trace is written to the VMLOG.
 *
 *----------------------------------------------------------------------
 */
void
UserLog_StackTrace(const VMKFullUserExcFrame* fullFrame)
{
   static const int maxDepth = 50;
   static const int maxArgs = 5;
   UserVA userEip = fullFrame->frame.eip;
   UserVA userEbp = fullFrame->regs.ebp;
   VMK_ReturnStatus status = VMK_OK;
   int framenum;

   /*
    * Print at most maxDepth frames.  We'll bail early if we hit the
    * top the stack.  (We assume the saved ebp in the first frame on
    * the stack is going to be 0.)
    */
   for (framenum = 0; framenum < maxDepth; framenum++) {
      const char* prefix = "";
      char btStr[128];
      size_t offset = 0;
      int argnum;

      offset += snprintf(btStr+offset, sizeof btStr - offset,
                         "%#x:[%#x](", userEbp, userEip);
      offset = MIN(offset, sizeof btStr);

      /*
       * Print the arguments pushed on the stack.  Assume they're all
       * 32-bit arguments.  Blindly assumes there are maxArgs arguments
       * to each function...
       */
      for (argnum = 0; argnum < maxArgs; argnum++) {
         UserVA userArgAddr = userEbp + ((2 + argnum) * sizeof(uint32));
         UserVA userArgVal = UserLogCopyIntIn(userArgAddr, &status);

         if (status != VMK_OK) {
            break;
         }

         offset += snprintf(btStr+offset, sizeof btStr - offset,
                            "%s%#x", prefix, userArgVal);
         offset = MIN(offset, sizeof btStr);
         prefix = ", ";
      }
      
      offset += snprintf(btStr+offset, sizeof btStr - offset,
                         ")");
      offset = MIN(offset, sizeof btStr);

      UWLOG(0, "%s", btStr);

      /* Get the next frame */
      userEip = UserLogCopyIntIn(userEbp + sizeof(uint32), &status);
      if (status == VMK_OK) {
         userEbp = UserLogCopyIntIn(userEbp, &status);
      }

      if (status != VMK_OK) {
         break;
      }

      if (0 == userEbp) {
         break;
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * UserLog_FullExcFrame --
 *
 *	Log the given stack frame(s).  Logged side-by-side if two are
 *	given.  The second may be null (label and ctx)
 *
 *	Since this copies data from user-mode via User_CopyIn, it must
 *	be invoked in a blocking-friendly context.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	User-mode register state is written to the log.
 *
 *----------------------------------------------------------------------
 */
void
UserLog_FullExcFrame(const char* label1,
                     const char* label2,
                     const struct VMKFullUserExcFrame* ctx1,
                     const struct VMKFullUserExcFrame* ctx2)
{
   unsigned int i;
#ifdef VMX86_LOG
   const uint32* regs1 = (const uint32*)ctx1;
   const uint32* regs2 = (const uint32*)ctx2;
   static const char* fields[] = {
      "es", "ds", "fs", "gs",
      "eax", "ecx", "edx", "ebx",
      "ebp", "esi", "edi", "pushValue",
      "gateNum", "*errorCode", "*eip", "*cs+__csu",
      "*eflags", "*esp", "*ss+__ssu",
   };
#endif

   ASSERT(ARRAYSIZE(fields) == (sizeof(VMKFullUserExcFrame)/ sizeof(uint32)));
   ASSERT(label1 != NULL);
   ASSERT(ctx1 != NULL);
   if (label2 == NULL) {
      label2 = "";
   }

   UWLOG(0, " [--] %10s  %s", label1, label2);
   for (i = 0; i < (sizeof(VMKFullUserExcFrame) / sizeof(uint32)); i++) {
      if (ctx2 == NULL) {
         UWLOG(0, " [%2d] 0x%08x (%s)",
               i, regs1[i], fields[i]);
      } else {
         UWLOG(0, " [%2d] 0x%08x  0x%08x  (%s)",
               i, regs1[i], regs2[i], fields[i]);
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * UserLog_DumpBuffer --
 *
 *	Hex-dump the given buffer into the LOG.  Output will look
 *	something like this:
 *
 * UserLog: DumpBuffer:   [   0]: 57 61 73 74 65 20 73 6f 6d 65 20 73 70 61 63 65 20 77 69 74 
 * UserLog: DumpBuffer:   [  20]: 68 20 63 6f 6e 73 74 61 6e 74 20 74 65 78 74 3b 20 50 6c 75 
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Buffer is written to the log.
 *
 *----------------------------------------------------------------------
 */
void
UserLog_DumpBuffer(uint8* buffer,
                   unsigned length)
{
   static const int stride = 20; 
   int dumpLine;

   ASSERT(buffer != NULL);

   UWLOG(0, "buf=%p, length=%u", buffer, length);
   for (dumpLine = 0; dumpLine < length; dumpLine += stride) {
      const int lineMax = MIN(stride, length - dumpLine);
      char dumpBuf[3 * stride + 1];
      int dumpOffset;
      int remain = sizeof dumpBuf;
      char *dumpBufPtr = dumpBuf;

      for (dumpOffset = 0; dumpOffset < lineMax; dumpOffset++) {
         unsigned rc = snprintf(dumpBufPtr, remain, "%02x ",
                                *(buffer+dumpLine+dumpOffset));
         rc = MIN(rc, remain);
         dumpBufPtr += rc;
         remain -= rc;
      }
      UWLOG(0, "  [%4d]: %s", dumpLine, dumpBuf);
   }
}


#if VMX86_LOG
/*
 *----------------------------------------------------------------------
 *
 * UserLogSetPrefix --
 *	
 *	Printf-like function for setting the prefix string in the
 *	given context.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Writes to given context
 *
 *----------------------------------------------------------------------
 */
void
UserLogSetPrefix(User_LogContext* ctx, const char* fmt, ...)
{
   va_list args;
   // save old prefix
   snprintf(ctx->oprefix, sizeof ctx->oprefix, "%s", ctx->prefix);
   va_start (args, fmt);
   vsnprintf(ctx->prefix, sizeof ctx->prefix, fmt, args);
   va_end(args);
}


/*
 *----------------------------------------------------------------------
 *
 * UWLOG_SetContextException --
 *
 *	Set the logging context for an exception vector (the given
 *	'info').
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Changes the logging prefix for the current world.
 *
 *----------------------------------------------------------------------
 */
void
UWLOG_SetContextException(int info)
{
   World_Handle* const current = MY_RUNNING_WORLD;
   User_LogContext* const ctx = &current->userThreadInfo->logContext;

   ctx->syscallNum = UWLOG_NOSYSCALL;
   // Decode the only two that are likely into pleasant strings
   switch(info) {
   case EXC_PF:
      UserLogSetPrefix(ctx, "EXC_PF: ");
      break;
   case EXC_GP:
      UserLogSetPrefix(ctx, "EXC_GP: ");
      break;
   case UWLOG_INTERRUPT:
      UserLogSetPrefix(ctx, "<intr>: ");
      break;
   default:
      UserLogSetPrefix(ctx, "EXC_%d: ", info);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * UWLOG_SetContext --
 *
 *	Set the 
 *
 * Results:
 *	None
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
void
UWLOG_SetContextSyscall(Bool linuxCall, uint32 syscallNum)
{
   World_Handle* const current = MY_RUNNING_WORLD;
   User_LogContext* const ctx = &current->userThreadInfo->logContext;

   ctx->linuxCall = linuxCall;
   ctx->syscallNum = syscallNum;
   ASSERT(syscallNum != UWLOG_NOSYSCALL);
   if (linuxCall) {
      if (syscallNum < ARRAYSIZE(linuxSyscallNames)) {
         UserLogSetPrefix(ctx, "%s: ",
                          linuxSyscallNames[syscallNum]);
      } else {
         UserLogSetPrefix(ctx, "linux-%d: ", syscallNum);
      }
   } else {
      const char* uwvmkName = UWVMKSyscall_GetName(syscallNum);
      if (uwvmkName != NULL) {
         UserLogSetPrefix(ctx, "%s: ", uwvmkName);
      } else {
         UserLogSetPrefix(ctx, "uwvmk-%d: ", syscallNum);
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * UWLOG_ReturnStatusToString --
 *
 *	Wrapper for VMK_ReturnStatusToString that also understands
 *	wrapped linux error codes.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Current world's logging context reset.
 *
 *----------------------------------------------------------------------
 */
const char* 
UWLOG_ReturnStatusToString(VMK_ReturnStatus status)
{
   if (status > VMK_GENERIC_LINUX_ERROR) {
      int errno = (status - VMK_GENERIC_LINUX_ERROR);
      if ((errno > 0)
          && (errno < ARRAYSIZE(linuxStatusCodeNames))
          && (linuxStatusCodeNames[errno] != NULL)) {
         return linuxStatusCodeNames[errno];
      }
   }

   return VMK_ReturnStatusToString(status);
}


/*
 *----------------------------------------------------------------------
 *
 * UWLOG_ClearContext --
 *
 *	Clear the current world logging context.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Current world's logging context reset.
 *
 *----------------------------------------------------------------------
 */
void
UWLOG_ClearContext(void)
{
   World_Handle* const current = MY_RUNNING_WORLD;
   User_LogContext* const ctx = &current->userThreadInfo->logContext;

   ctx->syscallNum = UWLOG_NOSYSCALL;
   if (ctx->oprefix[0] != '\0') {
      ASSERT(sizeof ctx->prefix == sizeof ctx->oprefix);
      memcpy(ctx->prefix, ctx->oprefix, sizeof ctx->prefix);
      ctx->oprefix[0] = '\0';
   } else {
      UserLogSetPrefix(ctx, "<unk>: ");
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * UWLOG_SetupSyscallTraceNames --
 *
 *      Registers custom trace tags corresponding to all userworld syscalls
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Registers custom trace tags.
 *
 *-----------------------------------------------------------------------------
 */
void
UWLOG_SetupSyscallTraceNames(void)
{
   int i;
   for (i=0; i < 243; i++) {
      Trace_RegisterCustomTag(TRACE_UWSYSCALL, i, linuxSyscallNames[i]);
   }
}

#endif /* VMX86_LOG */

