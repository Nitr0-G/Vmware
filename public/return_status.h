/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * return_status.h --
 *
 *      VMkernel return status codes.
 *
 */

#ifndef _RETURN_STATUS_H_
#define _RETURN_STATUS_H_

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMMEXT
#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMK_MODULE
#define INCLUDE_ALLOW_DISTRIBUTE
#define INCLUDE_ALLOW_VMCORE
#include "includeCheck.h"

/*
 * vmkernel error codes and translation to Unix error codes
 *
 * The table below gives the name, description, and corresponding Unix
 * error code for each VMK error code.  The Unix error code is used
 * when a VMK error propagates up to a user world through the
 * Linux-compatible system call interface and we need to translate it.
 *
 * There is also a mechanism to wrap a Linux error code opaquely
 * inside a VMK error code.  When the COS proxy generates an error, it
 * starts out as a Linux error code in a COS process, propagates into
 * the vmkernel where it needs to be translated to a VMK error code,
 * and then goes out to a user world where it needs to be a Unix error
 * code again.  The vmkernel does not have to understand these errors
 * other than to know that a nonzero value is an error, so we make
 * them opaque for simplicity.  The COS proxy calls
 * VMK_WrapLinuxError, which adds the absolute value of (nonzero)
 * Linux error codes to VMK_GENERIC_LINUX_ERROR.  User_TranslateStatus
 * undoes this transformation on the way out.
 *
 * XXX Currently there is no need to translate VMK error codes to BSD
 * error codes, but the macros used with this table could be easily
 * extended to do so.  We do translate BSD error codes to VMK error
 * codes in vmkernel/networking/lib/support.c, using a case statement.
 * See PR 35564 for comments on how this could be improved.
 *
 * VMK_FAILURE and VMK_GENERIC_LINUX_ERROR must be at the start and
 * end, and must be defined with specific values using
 * DEFINE_VMK_ERR_AT; see return_status.c.
 *
 * All the values should be positive because we return these directly as
 * _vmnix call return values (at least for sysinfo).  A negative value
 * there could get interpretted as a linux error code.
 *
 */

#define LINUX_OK   0
#define FREEBSD_OK 0

//                VMK error code name           Description                                   Unix name
//                ===================           ===========                                   =========
#define VMK_ERROR_CODES \
   DEFINE_VMK_ERR_AT(VMK_OK,                    "Success", 0,                                 OK          )\
   DEFINE_VMK_ERR_AT(VMK_FAILURE,               "Failure", 0x0bad0001,                        EINVAL      )\
   DEFINE_VMK_ERR(VMK_WOULD_BLOCK,              "Would block",                                EAGAIN      )\
   DEFINE_VMK_ERR(VMK_NOT_FOUND,                "Not found",                                  ENOENT      )\
   DEFINE_VMK_ERR(VMK_BUSY,                     "Busy",                                       EBUSY       )\
   DEFINE_VMK_ERR(VMK_EXISTS,                   "Already exists",                             EEXIST      )\
   DEFINE_VMK_ERR(VMK_LIMIT_EXCEEDED,           "Limit exceeded",                             EFBIG       )\
   DEFINE_VMK_ERR(VMK_BAD_PARAM,                "Bad parameter",                              EINVAL      )\
   DEFINE_VMK_ERR(VMK_METADATA_READ_ERROR,      "Metadata read error",                        EIO         )\
   DEFINE_VMK_ERR(VMK_METADATA_WRITE_ERROR,     "Metadata write error",                       EIO         )\
   DEFINE_VMK_ERR(VMK_IO_ERROR,                 "I/O error",                                  EIO         )\
   DEFINE_VMK_ERR(VMK_READ_ERROR,               "Read error",                                 EIO         )\
   DEFINE_VMK_ERR(VMK_WRITE_ERROR,              "Write error",                                EIO         )\
   DEFINE_VMK_ERR(VMK_INVALID_NAME,             "Invalid name",                               ENAMETOOLONG)\
   DEFINE_VMK_ERR(VMK_INVALID_HANDLE,           "Invalid handle",                             EBADF       )\
   DEFINE_VMK_ERR(VMK_INVALID_ADAPTER,          "No such SCSI adapter",                       ENODEV      )\
   DEFINE_VMK_ERR(VMK_INVALID_TARGET,           "No such target on adapter",                  ENODEV      )\
   DEFINE_VMK_ERR(VMK_INVALID_PARTITION,        "No such partition on target",                ENXIO       )\
   DEFINE_VMK_ERR(VMK_INVALID_TYPE,             "Partition does not have correct type",       ENXIO       )\
   DEFINE_VMK_ERR(VMK_INVALID_FS,               "No filesystem on the device",                ENXIO       )\
   DEFINE_VMK_ERR(VMK_INVALID_MEMMAP,           "Memory map mismatch",                        EFAULT      )\
   DEFINE_VMK_ERR(VMK_NO_MEMORY,                "Out of memory",                              ENOMEM      )\
   DEFINE_VMK_ERR(VMK_NO_MEMORY_RETRY,          "Out of memory (ok to retry)",                ENOMEM      )\
   DEFINE_VMK_ERR(VMK_NO_RESOURCES,             "Out of resources",                           ENOMEM      )\
   DEFINE_VMK_ERR(VMK_NO_FREE_HANDLES,          "No free handles",                            EMFILE      )\
   DEFINE_VMK_ERR(VMK_NUM_HANDLES_EXCEEDED,     "Exceeded maximum number of allowed handles", ENFILE      )\
   DEFINE_VMK_ERR(VMK_NO_FREE_PTR_BLOCKS,       "No free pointer blocks",                     ENOSPC      )\
   DEFINE_VMK_ERR(VMK_NO_FREE_DATA_BLOCKS,      "No free data blocks",                        ENOSPC      )\
   DEFINE_VMK_ERR(VMK_STATUS_PENDING,           "Status pending",                             EAGAIN      )\
   DEFINE_VMK_ERR(VMK_STATUS_FREE,              "Status free",                                EAGAIN      )\
   DEFINE_VMK_ERR(VMK_UNSUPPORTED_CPU,          "Unsupported CPU",                            ENODEV      )\
   DEFINE_VMK_ERR(VMK_NOT_SUPPORTED,            "Not supported",                              ENOSYS      )\
   DEFINE_VMK_ERR(VMK_TIMEOUT,                  "Timeout",                                    ETIMEDOUT   )\
   DEFINE_VMK_ERR(VMK_READ_ONLY,                "Read only",                                  EROFS       )\
   DEFINE_VMK_ERR(VMK_RESERVATION_CONFLICT,     "SCSI reservation conflict",                  EAGAIN      )\
   DEFINE_VMK_ERR(VMK_FS_LOCKED,                "File system locked",                         EADDRINUSE  )\
   DEFINE_VMK_ERR(VMK_NOT_ENOUGH_SLOTS,         "Out of slots",                               ENFILE      )\
   DEFINE_VMK_ERR(VMK_INVALID_ADDRESS,          "Invalid address",                            EFAULT      )\
   DEFINE_VMK_ERR(VMK_NOT_SHARED,               "Not shared",                                 ENOMEM      )\
   DEFINE_VMK_ERR(VMK_SHARED,                   "Page is shared",                             ENOMEM      )\
   DEFINE_VMK_ERR(VMK_KSEG_PAIR_FLUSHED,        "Kseg pair flushed",                          ENOMEM      )\
   DEFINE_VMK_ERR(VMK_MAX_ASYNCIO_PENDING,      "Max async I/O requests pending",             ENOMEM      )\
   DEFINE_VMK_ERR(VMK_VERSION_MISMATCH_MINOR,   "Minor version mismatch",                     ENOSYS      )\
   DEFINE_VMK_ERR(VMK_VERSION_MISMATCH_MAJOR,   "Major version mismatch",                     ENOSYS      )\
   DEFINE_VMK_ERR(VMK_CONTINUE_TO_SWAP,         "Continue swapping",                          EAGAIN      )\
   DEFINE_VMK_ERR(VMK_IS_CONNECTED,             "Already connected",                          EINVAL      )\
   DEFINE_VMK_ERR(VMK_IS_DISCONNECTED,          "Already disconnected",                       ENOTCONN    )\
   DEFINE_VMK_ERR(VMK_NOT_INITIALIZED,          "Not initialized",                            EINVAL      )\
   DEFINE_VMK_ERR(VMK_WAIT_INTERRUPTED,         "Wait interrupted",                           EINTR       )\
   DEFINE_VMK_ERR(VMK_NAME_TOO_LONG,            "Name too long",                              ENAMETOOLONG)\
   DEFINE_VMK_ERR(VMK_MISSING_FS_PES,           "VMFS volume missing physical extents",       ENOTDIR     )\
   DEFINE_VMK_ERR(VMK_NICTEAMING_VALID_MASTER,  "NIC teaming master valid",                   EINVAL      )\
   DEFINE_VMK_ERR(VMK_NICTEAMING_SLAVE,         "NIC teaming slave",                          EEXIST      )\
   DEFINE_VMK_ERR(VMK_NICTEAMING_REGULAR_VMNIC, "NIC teaming regular VMNIC",                  EINVAL      )\
   DEFINE_VMK_ERR(VMK_ABORT_NOT_RUNNING,        "Abort not running",                          ECANCELED   )\
   DEFINE_VMK_ERR(VMK_NOT_READY,                "Not ready",                                  EIO         )\
   DEFINE_VMK_ERR(VMK_CHECKSUM_MISMATCH,        "Checksum mismatch",                          EIO         )\
   DEFINE_VMK_ERR(VMK_VLAN_NO_HW_ACCEL,         "VLan HW Acceleration not supported",         EINVAL      )\
   DEFINE_VMK_ERR(VMK_NO_VLAN_SUPPORT,          "VLan is not supported in vmkernel",          EOPNOTSUPP  )\
   DEFINE_VMK_ERR(VMK_NOT_VLAN_HANDLE,          "Not a VLan handle",                          EINVAL      )\
   DEFINE_VMK_ERR(VMK_BAD_VLANID,               "Couldn't retrieve VLan id",                  EBADF       )\
   DEFINE_VMK_ERR(VMK_MIG_PROTO_ERROR,          "Migration protocol error",                   EINVAL      )\
   DEFINE_VMK_ERR(VMK_NO_CONNECT,               "No connection",                              EIO         )\
   DEFINE_VMK_ERR(VMK_SEGMENT_OVERLAP,          "Segment overlap",                            EINVAL      )\
   DEFINE_VMK_ERR(VMK_BAD_MPS,                  "Error parsing MPS Table",                    EIO         )\
   DEFINE_VMK_ERR(VMK_BAD_ACPI,                 "Error parsing ACPI Table",                   EIO         )\
   DEFINE_VMK_ERR(VMK_RESUME_ERROR,             "Failed to resume VM",                        EIO         )\
   DEFINE_VMK_ERR(VMK_NO_ADDRESS_SPACE,         "Insufficient address space for operation",   ENOMEM      )\
   DEFINE_VMK_ERR(VMK_BAD_ADDR_RANGE,           "Bad address range",                          EINVAL      )\
   DEFINE_VMK_ERR(VMK_ENETDOWN,                 "Network is down",                            ENETDOWN    )\
   DEFINE_VMK_ERR(VMK_ENETUNREACH,              "Network unreachable",                        ENETUNREACH )\
   DEFINE_VMK_ERR(VMK_ENETRESET,                "Network dropped connection on reset",        ENETRESET   )\
   DEFINE_VMK_ERR(VMK_ECONNABORTED,             "Software caused connection abort",           ECONNABORTED)\
   DEFINE_VMK_ERR(VMK_ECONNRESET,               "Connection reset by peer",                   ECONNRESET  )\
   DEFINE_VMK_ERR(VMK_ENOTCONN,                 "Socket is not connected",                    ENOTCONN    )\
   DEFINE_VMK_ERR(VMK_ESHUTDOWN,                "Can't send after socket shutdown",           ESHUTDOWN   )\
   DEFINE_VMK_ERR(VMK_ETOOMANYREFS,             "Too many references: can't splice",          ETOOMANYREFS)\
   DEFINE_VMK_ERR(VMK_ECONNREFUSED,             "Connection refused",                         ECONNREFUSED)\
   DEFINE_VMK_ERR(VMK_EHOSTDOWN,                "Host is down",                               EHOSTDOWN   )\
   DEFINE_VMK_ERR(VMK_EHOSTUNREACH,             "No route to host",                           EHOSTUNREACH)\
   DEFINE_VMK_ERR(VMK_EADDRINUSE,               "Address already in use",                     EADDRINUSE  )\
   DEFINE_VMK_ERR(VMK_BROKEN_PIPE,              "Broken pipe",                                EPIPE       )\
   DEFINE_VMK_ERR(VMK_NOT_A_DIRECTORY,          "Not a directory",                            ENOTDIR     )\
   DEFINE_VMK_ERR(VMK_IS_A_DIRECTORY,           "Is a directory",                             EISDIR      )\
   DEFINE_VMK_ERR(VMK_NOT_EMPTY,                "Directory not empty",                        ENOTEMPTY   )\
   DEFINE_VMK_ERR(VMK_NOT_IMPLEMENTED,          "Not implemented",                            ENOSYS      )\
   DEFINE_VMK_ERR(VMK_NO_SIGNAL_HANDLER,        "No signal handler",                          EINVAL      )\
   DEFINE_VMK_ERR(VMK_FATAL_SIGNAL_BLOCKED,     "Fatal signal blocked",                       EINVAL      )\
   DEFINE_VMK_ERR(VMK_NO_ACCESS,                "Permission denied",                          EACCES      )\
   DEFINE_VMK_ERR(VMK_NO_PERMISSION,            "Operation not permitted",                    EPERM       )\
   DEFINE_VMK_ERR(VMK_UNDEFINED_SYSCALL,        "Undefined syscall",                          ENOSYS      )\
   DEFINE_VMK_ERR(VMK_RESULT_TOO_LARGE,         "Result too large",                           ERANGE      )\
   DEFINE_VMK_ERR(VMK_VLAN_FILTERED,            "Pkts dropped because of VLAN (support) mismatch", ERANGE )\
   DEFINE_VMK_ERR(VMK_BAD_EXCFRAME,             "Unsafe exception frame",                     EFAULT      )\
   DEFINE_VMK_ERR(VMK_MODULE_NOT_LOADED,        "Necessary module isn't loaded",              ENODEV      )\
   DEFINE_VMK_ERR(VMK_NO_SUCH_ZOMBIE,           "No dead world by that name",                 ECHILD      )\
   DEFINE_VMK_ERR(VMK_IS_A_SYMLINK,             "Is a symbolic link",                         ELOOP       )\
   DEFINE_VMK_ERR(VMK_CROSS_DEVICE_LINK,        "Cross-device link" ,                         EXDEV       )\
   DEFINE_VMK_ERR(VMK_NOT_A_SOCKET,		"Not a socket",				      ENOTSOCK    )\
   DEFINE_VMK_ERR(VMK_ILLEGAL_SEEK,		"Illegal seek",				      ESPIPE      )\
   DEFINE_VMK_ERR(VMK_ADDRFAM_UNSUPP,		"Unsupported address family",		      EAFNOSUPPORT)\
   DEFINE_VMK_ERR(VMK_ALREADY_CONNECTED,	"Already connected",			      EISCONN	  )\
   DEFINE_VMK_ERR(VMK_DEATH_PENDING,            "World is marked for death",		      ENOENT	  )\
   DEFINE_VMK_ERR(VMK_CPU_ADMIT_FAILED,         "Admission check failed for cpu resource",    ENOSPC	  )\
   DEFINE_VMK_ERR(VMK_MEM_ADMIT_FAILED,         "Admission check failed for memory resource", ENOSPC	  )\
/* Add new error codes above this comment.  The one below must be last. */ \
   DEFINE_VMK_ERR_AT(VMK_GENERIC_LINUX_ERROR,   "Generic service console error", 0x2bad0000,  EIO         )
// please don't add ERR_AT with negative value. See comment above.



/*
 * types
 */ 
#define DEFINE_VMK_ERR(_err, _str, _uerr) _err,
#define DEFINE_VMK_ERR_AT(_err, _str, _val, _uerr) _err = _val,
typedef enum {
   VMK_ERROR_CODES
} VMK_ReturnStatus;
#undef DEFINE_VMK_ERR
#undef DEFINE_VMK_ERR_AT

/*
 * operations
 */

extern const char *VMK_ReturnStatusToString(VMK_ReturnStatus status);

/*
 *----------------------------------------------------------------------
 *
 * VMK_WrapLinuxError --
 *
 *      Wrap a Linux errno value inside a VMK_ReturnStatus value.  The
 *      status value is opaque to the vmkernel, except that 0 (no
 *      error) is guaranteed to translate to VMK_OK.  This routine is
 *      for use by the COS proxy to pass errors back through the
 *      vmkernel to a user world.
 *
 *      This is a macro instead of a static inline because
 *      return_status.h gets #included both from places where "INLINE"
 *      is not defined and from places where "inline" is wrong.  Ugh.
 *
 * Results:
 *      Opaque VMK_ReturnStatus.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
#define VMK_WrapLinuxError(error) \
   ((error) == 0 ? VMK_OK : \
    (error) <  0 ? VMK_GENERIC_LINUX_ERROR - (error) : \
                   VMK_GENERIC_LINUX_ERROR + (error))

#endif
