/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * linuxAPI.h -
 *	Defines the linux system call API
 */

#ifndef VMKERNEL_USER_LINUXAPI_H
#define VMKERNEL_USER_LINUXAPI_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "vm_basic_types.h"
#include "vmkernel.h"
#include "compatErrno.h"
#include "vmkpoll.h"
#include "userProxy_ext.h"
#include "uwvmkAPI.h"

/*
 * Linux has this peculiarity in some of its syscalls (such as read or write) in
 * that they take in a size in bytes as an unsigned integer, then return the
 * number of bytes actually used an a signed integer.  Thus you can legally pass
 * in a value > 2 gigs, but it can't return that it successfully did something
 * with that > 2 gigs.  So, Linux says that for functions such as this, if you
 * pass in a number bigger than the one defined below, the results are
 * unspecified (see read(2)).
 *
 * Thus, for certain functions, if we see they've passed in a number greater
 * than this, we immediately return EINVAL, since most likely it's a bug or done
 * with malicious intent.
 *
 * Note also that there are some functions such as mmap, which don't fall prey
 * to this exact problem.  For mmap, glibc only considers the return value an
 * error if it's between -4096 and -1.
 */
#define LINUX_SSIZE_MAX		2147483647

typedef size_t LinuxSizeT;

typedef int32 LinuxPid;
typedef int LinuxFd;

// deprecated 16-bit identity types:
typedef uint16 LinuxUID16;
typedef uint16 LinuxGID16;
// Identity types:
typedef uint32 LinuxUID;
typedef uint32 LinuxGID;
#define LINUX_NGROUPS_MAX USERPROXY_NGROUPS_MAX  // max supplementary groups

// signal handler constants
#define LINUX_SIG_DFL ((UserVA)0)
#define LINUX_SIG_IGN ((UserVA)1)

// time (13)
typedef int32 LinuxTimeT;


// access (33)
#define LINUX_R_OK 4
#define LINUX_W_OK 2
#define LINUX_X_OK 1
#define LINUX_F_OK 0


// ioctl (54)
typedef unsigned LinuxDirection;
#define LINUX_IOCTL_CMD_MASK    0xffff
#define LINUX_IOCTL_SIZE_MASK   0x3fff
#define LINUX_IOCTL_DIR_MASK    0x3

#define LINUX_IOCTL_CMD_SHIFT    0
#define LINUX_IOCTL_SIZE_SHIFT  16
#define LINUX_IOCTL_DIR_SHIFT   30

#define LINUX_IOCTL_DIR_NONE    0
#define LINUX_IOCTL_DIR_WRITE   1
#define LINUX_IOCTL_DIR_READ    2

#define LINUX_IOCTL_CMD(cmd)    (((cmd) >> LINUX_IOCTL_CMD_SHIFT) & \
                                 LINUX_IOCTL_CMD_MASK)
#define LINUX_IOCTL_SIZE(cmd)   (((cmd) >> LINUX_IOCTL_SIZE_SHIFT) & \
                                 LINUX_IOCTL_SIZE_MASK)
#define LINUX_IOCTL_DIR(cmd)    (((cmd) >> LINUX_IOCTL_DIR_SHIFT) & \
                                 LINUX_IOCTL_DIR_MASK)

/*
 * ioctl() argument types
 */
typedef enum {
   LINUX_IOCTL_ARG_CONST     = 0x1, // ioctl() arg is a constant
   LINUX_IOCTL_ARG_PTR       = 0x2, // ioctl() arg is a pointer
   LINUX_IOCTL_ARG_PACKED    = 0x3  // ioctl() arg is a pointer packed w/ data
} LinuxIoctlArgType;

typedef struct LinuxIoctlPackedDataArg {
   uint32 offset;
   uint32 length;
} LinuxIoctlPackedDataArg;

typedef struct LinuxIoctlPackedData {
   void *buf;
   uint32 nPacked;
   uint32 bufSize;
   LinuxIoctlPackedDataArg *packedArg;
} LinuxIoctlPackedData;

// gettimeofday (78), settimeofday (79)
typedef struct {
   int tz_minuteswest;
   int tz_dsttime;
} LinuxTimezone;

// mmap (90, 192)
// mmap prot flags
#define LINUX_MMAP_PROT_NONE       0x00
#define LINUX_MMAP_PROT_READ       0x01
#define LINUX_MMAP_PROT_WRITE      0x02
#define LINUX_MMAP_PROT_EXEC       0x04
#define LINUX_MMAP_PROT_ALL        (LINUX_MMAP_PROT_READ | \
				    LINUX_MMAP_PROT_WRITE | \
				    LINUX_MMAP_PROT_EXEC)
// mmap flags
#define LINUX_MMAP_SHARED      0x00000001
#define LINUX_MMAP_PRIVATE     0x00000002
#define LINUX_MMAP_FIXED       0x00000010
#define LINUX_MMAP_ANONYMOUS   0x00000020
#define LINUX_MMAP_GROWSDOWN   0x00000100
#define LINUX_MMAP_EXECUTABLE  0x00001000
#define LINUX_MMAP_LOCKED      0x00002000
#define LINUX_MMAP_NORESERVE   0x00004000

//mremap flags
#define LINUX_MREMAP_MAYMOVE   0x00000001

// statfs (99), fstatfs (100)

typedef struct LinuxFSID {
   int32 val[2];
} LinuxFSID;

typedef struct LinuxStatFS {
   int32 f_type;
   int32 f_bsize;
   int32 f_blocks;
   int32 f_bfree;
   int32 f_bavail;
   int32 f_files;
   int32 f_ffree;
   LinuxFSID f_fsid;
   int32 f_namelen;
   int32 f_spare[6];
} LinuxStatFS;


// statfs64 (268), fstatfs64 (269)

typedef struct LinuxStatFS64 {
   int32 f_type;
   int32 f_bsize;
   int64 f_blocks;
   int64 f_bfree;
   int64 f_bavail;
   int64 f_files;
   int64 f_ffree;
   LinuxFSID f_fsid;
   int32 f_namelen;
   int32 f_spare[6];
} LinuxStatFS64;


// stat64 (195), lstat64 (196), fstat64 (197)

typedef uint32 LinuxMode;
#define LINUX_MODE_IFMT		0170000
#define LINUX_MODE_IFSOCK	0140000
#define LINUX_MODE_IFLNK	0120000
#define LINUX_MODE_IFREG	0100000
#define LINUX_MODE_IFBLK	0060000
#define LINUX_MODE_IFDIR	0040000
#define LINUX_MODE_IFCHR	0020000
#define LINUX_MODE_IFIFO	0010000
#define LINUX_MODE_ISUID	0004000
#define LINUX_MODE_ISGID	0002000
#define LINUX_MODE_ISVTX	0001000
#define LINUX_MODE_IRWXU	00700
#define LINUX_MODE_IRUSR	00400
#define LINUX_MODE_IWUSR	00200
#define LINUX_MODE_IXUSR	00100
#define LINUX_MODE_IRWXG	00070
#define LINUX_MODE_IRGRP	00040
#define LINUX_MODE_IWGRP	00020
#define LINUX_MODE_IXGRP	00010
#define LINUX_MODE_IRWXO	00007
#define LINUX_MODE_IROTH	00004
#define LINUX_MODE_IWOTH	00002
#define LINUX_MODE_IXOTH	00001

typedef struct LinuxStat64 {
   uint64 st_dev;
   uint32 __pad1;
   uint32 st_ino32; // low 32 bits of ino
   LinuxMode st_mode;
   uint32 st_nlink;
   LinuxUID st_uid;
   LinuxGID st_gid;
   uint64 st_rdev;
   uint32 __pad2;
   int64 st_size;
   uint32 st_blksize;
   uint64 st_blocks;
   int32 st_atime;
   int32 __pad3;
   int32 st_mtime;
   int32 __pad4;
   int32 st_ctime;
   int32 __pad5;
   uint64 st_ino;
} LinuxStat64;


// nanosleep (162)
typedef struct LinuxTimespec {
   int32	seconds;
   uint32	nanoseconds;
} LinuxTimespec;


// Poll (168)
typedef struct LinuxPollfd {
   LinuxFd fd;
   uint16 inEvents;
   uint16 outEvents;
} LinuxPollfd;

#define LINUX_POLLFLAG_IN      0x0001
#define LINUX_POLLFLAG_PRI     0x0002
#define LINUX_POLLFLAG_OUT     0x0004
#define LINUX_POLLFLAG_ERR     0x0008
#define LINUX_POLLFLAG_HUP     0x0010
#define LINUX_POLLFLAG_NVAL    0x0020

// uname (122)
#define LINUX_UTSNAME_LENGTH   USERPROXY_UTSNAME_LENGTH
typedef struct LinuxUtsName {
   char sysname[LINUX_UTSNAME_LENGTH];
   char nodename[LINUX_UTSNAME_LENGTH];
   char release[LINUX_UTSNAME_LENGTH];
   char version[LINUX_UTSNAME_LENGTH];
   char machine[LINUX_UTSNAME_LENGTH];
   char domainname[LINUX_UTSNAME_LENGTH];
} LinuxUtsName;

/*
 *----------------------------------------------------------------------
 *
 * User_LinuxToVMKPollFlags --
 *
 *	Convert Linux poll flags to VMK-style poll flags.
 *
 * Results:
 *	VMKPollEvent.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE VMKPollEvent
User_LinuxToVMKPollFlags(short linuxEvents)
{
   VMKPollEvent events = 0;

   ASSERT(LINUX_POLLFLAG_IN == VMKPOLL_READ);
   ASSERT(LINUX_POLLFLAG_OUT == VMKPOLL_WRITE);
   ASSERT(LINUX_POLLFLAG_ERR == VMKPOLL_RDHUP);
   ASSERT(LINUX_POLLFLAG_HUP == VMKPOLL_WRHUP);
   ASSERT(LINUX_POLLFLAG_NVAL == VMKPOLL_INVALID);

   /* Trim out unsupported linux events like _PRI */
   events = linuxEvents & (VMKPOLL_READ|VMKPOLL_WRITE
                           |VMKPOLL_RDHUP|VMKPOLL_WRHUP|VMKPOLL_INVALID);
   if ((linuxEvents & (~events)) != 0) {
      // Bah, UWLOG not defined yet...  (No "module" for header files)
      //UWLOG(0, "Ignored linux poll flag %#x", (linuxEvents & (~events)));
   }

   return events;
}

/*
 *----------------------------------------------------------------------
 *
 * User_VMKToLinuxPollFlags --
 *
 *	Convert VMK-style poll flags to Linux poll flags.
 *
 * Results:
 *	Linux poll flags
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static INLINE short
User_VMKToLinuxPollFlags(VMKPollEvent events)
{
   ASSERT(LINUX_POLLFLAG_IN == VMKPOLL_READ);
   ASSERT(LINUX_POLLFLAG_OUT == VMKPOLL_WRITE);
   ASSERT(LINUX_POLLFLAG_ERR == VMKPOLL_RDHUP);
   ASSERT(LINUX_POLLFLAG_HUP == VMKPOLL_WRHUP);
   ASSERT(LINUX_POLLFLAG_NVAL == VMKPOLL_INVALID);

   ASSERT((events & ~(VMKPOLL_READ|VMKPOLL_WRITE
                      |VMKPOLL_RDHUP|VMKPOLL_WRHUP|VMKPOLL_INVALID))
          == 0);
   ASSERT((events & 0xFFFF0000) == 0);

   return (short)events;
}

/*
 * select(82)
 */
typedef long int LinuxFdMask;

#define LINUX_FD_SETSIZE    1024
#define LINUX_NFDBITS       (8 * sizeof (LinuxFdMask))
#define LINUX_FDELT(d)      ((d) / LINUX_NFDBITS)
#define LINUX_FDS_LONGS(nr) (((nr) + LINUX_NFDBITS - 1)/ LINUX_NFDBITS)
#define LINUX_FDS_BYTES(nr) (LINUX_FDS_LONGS(nr) * sizeof(long))

typedef struct {
   LinuxFdMask fdsBits[LINUX_FD_SETSIZE / LINUX_NFDBITS];
} LinuxFdSet;

#define LINUX_FD_ZERO(fdsp) \
   do { \
      unsigned int __i; \
      for (__i = 0; __i < sizeof(LinuxFdSet) / sizeof(LinuxFdMask); __i++) { \
         (fdsp)->fdsBits[__i] = 0; \
      } \
   } while (0)

#define LINUX_FD_SET(fd, fdsp) \
   ((fdsp)->fdsBits[LINUX_FDELT(fd)] |= (0x1 << ((int)(fd) % LINUX_NFDBITS)))

#define LINUX_FD_CLR(fd, fdsp) \
   ((fdsp)->fdsBits[LINUX_FDELT(fd)] &= ~(0x1 << ((int)(fd) % LINUX_NFDBITS)))

#define LINUX_FD_ISSET(fd, fdsp) \
   (((fdsp)->fdsBits[LINUX_FDELT(fd)] >> ((int)(fd) % LINUX_NFDBITS)) & 0x1)

/*
 * gettimeofday(78) and settimeofday(79)
 */
typedef struct {
   long tv_sec;
   long tv_usec;
} LinuxTimeval;

/*
 * Linux fcntl() command constants
 */
#define LINUX_FCNTL_CMD_DUPFD 	0
#define LINUX_FCNTL_CMD_GETFD 	1
#define LINUX_FCNTL_CMD_SETFD 	2
#define LINUX_FCNTL_CMD_GETFL 	3
#define LINUX_FCNTL_CMD_SETFL 	4

/*
 * None of these are currently used, but we may need them in the future.
 */
#define LINUX_FCNTL_CMD_GETLK 	5
#define LINUX_FCNTL_CMD_SETLK 	6
#define LINUX_FCNTL_CMD_SETLKW  7
#define LINUX_FCNTL_CMD_GETOWN  9
#define LINUX_FCNTL_CMD_SETOWN  8

#define LINUX_FCNTL_BIT_CHANGED(old, new, bit) (((old) & (bit)) ^ ((new) & (bit)))

// All system calls that accept pathnames
/*
 * Maximum length of a pathname.  Posix requires at least 255.
 * Real linux allows 4096.  If we increase this, we need to change 
 * LinuxFileDesc_Open not to stack allocate its buffer.
 */
#define LINUX_PATH_MAX         USERPROXY_PATH_MAX
/*
 * Maximum length of one arc in a pathname.  Posix only requires 14,
 * for historical reasons, but that won't do.  Linux allows 255.
 */
#define LINUX_ARC_MAX         255


// readv (145), writev (146)
#define LINUX_MAX_IOVEC        USERPROXY_MAX_IOVEC
typedef struct LinuxIovec {
   UserVA base;
   uint32 length;
} LinuxIovec;

// getdents64 (220)
typedef struct LinuxDirent64 {
   uint64 d_ino;
   int64  d_off;
   uint16 d_reclen;
   uint8  d_type;
   char   d_name[LINUX_ARC_MAX + 1];
} LinuxDirent64;


/*
 * For getpriority and setpriority (96/97)
 */
typedef enum {
   LINUX_PRIO_PROCESS = 0,
   LINUX_PRIO_PGRP    = 1,
   LINUX_PRIO_USER    = 2,
} UserLinux_PriorityWhich;

#define LINUX_GETPRIORITY_OFFSET 20


/*
 * For setitimer (104) and getitimer (105)
 */
typedef enum LinuxITimerWhich {
   LINUX_ITIMER_REAL = 0,
   LINUX_ITIMER_VIRTUAL = 1,
   LINUX_ITIMER_PROF = 2
} LinuxITimerWhich;

typedef struct LinuxITimerVal {
   LinuxTimeval interval;
   LinuxTimeval value;
} LinuxITimerVal;



/*
 * Socket definitions.
 */

/*
 * send/recv flags.
 *
 * We only need one for internal use.  The vmx doesn't seem to use any.
 */
#define LINUX_SOCKET_MSG_DONTWAIT    0x40

/*
 * set/getsockopt levels.
 */
typedef enum {
   LINUX_SOCKET_SOL_SOCKET = 1,
   LINUX_SOCKET_SOL_TCP    = 6,
   LINUX_SOCKET_SOL_UDP    = 17,
} LinuxSocketSockOptLevel;

/*
 * Socket control message types.
 */
#define LINUX_SOCKET_SCM_RIGHTS      0x01

/*
 * set/getsockopt names we need to support for LINUX_SOCKET_SOL_SOCKET.
 */
typedef enum {
   LINUX_SOCKET_SO_REUSEADDR =  2,
   LINUX_SOCKET_SO_ERROR     =  4,
   LINUX_SOCKET_SO_SNDBUF    =  7,
   LINUX_SOCKET_SO_RCVBUF    =  8,
   LINUX_SOCKET_SO_KEEPALIVE =  9,
   LINUX_SOCKET_SO_LINGER    = 13,
} LinuxSocketSockOptSocketNames;

typedef enum {
   LINUX_SOCKETFAMILY_UNIX = 1,
   LINUX_SOCKETFAMILY_INET = 2,
   LINUX_SOCKETFAMILY_VMK  = PF_VMKUNIX
} LinuxSocketFamily;

typedef enum {
   LINUX_SOCKETTYPE_STREAM = 1,
   LINUX_SOCKETTYPE_DATAGRAM = 2,
   LINUX_SOCKETTYPE_RAW = 3
} LinuxSocketType;

typedef enum {
   LINUX_SOCKETPROTO_DEFAULT = 0,
   LINUX_SOCKETPROTO_TCP = 6,
   LINUX_SOCKETPROTO_UDP = 17
} LinuxSocketProtocol;

typedef struct LinuxSocketName {
   short            family;
   char             data[108];
} LinuxSocketName;

/*
 * Linux message header used to pass data between sockets.
 *
 * The most interesting parts of this are the control and controlLen fields.
 * Control points to optional out-of-band information that can be passed along
 * with the real payload.  This out-of-band info could be such things as file
 * descriptor or credential passing.  While control is a void*, it should only
 * take data of type ControlMsgHdr*.  So why not make it a ControlMsgHdr*?
 * Well, ControlMsgHdr is really a variable length struct.  The 'length' field
 * dictates how large the struct and trailing data are.  Thus, 'control' can't
 * be a ControlMsgHdr* because you can't treat it as a uniformly sized array.
 * Instead, Linux provides you with all these fancy functions to do the job,
 * which we've emulated below.
 */
typedef struct LinuxMsgHdr {
   LinuxSocketName* name;
   uint32           nameLen;
   LinuxIovec*      iov;
   uint32           iovLen;
   void*            control;
   uint32           controlLen;
   uint32           flags;
} LinuxMsgHdr;

/*
 * LinuxMsgHdr.control is a packed array of these.  These are variable
 * length, thus the hoop jumping to unpack it. 
 */
typedef struct LinuxControlMsgHdr {
   uint32	    length;
   uint32	    level;
   uint32	    type;
   uint8            data[];
} LinuxControlMsgHdr;

/*
 * Gets the first ControlMsgHdr from the LinuxMsgHdr.  If there isn't any
 * control information, NULL is returned.
 */
static INLINE LinuxControlMsgHdr*
LinuxAPI_CmsgFirstHdr(LinuxMsgHdr* msg)
{
   if (msg->controlLen > sizeof(LinuxControlMsgHdr)) {
      return (LinuxControlMsgHdr*)msg->control;
   } else {
      return NULL;
   }
}

/*
 * Rounds up to where the next ControlMsgHdr should start (ie, word aligned).
 */
static INLINE int
LinuxAPI_CmsgAlign(int len)
{
   return ALIGN_UP(len, sizeof(uint32));
}

/*
 * Adds the given len to the size of the control message header and rounds up.
 */
static INLINE int
LinuxAPI_CmsgLen(int len)
{
   return LinuxAPI_CmsgAlign(sizeof (LinuxControlMsgHdr) + len);
}

/*
 * Returns the next ControlMsgHdr from the LinuxMsgHdr after the given
 * ControlMsgHdr.  Returns NULL if there isn't another ControlMsgHdr.
 */
static INLINE LinuxControlMsgHdr*
LinuxAPI_CmsgNextHdr(LinuxMsgHdr* msg, LinuxControlMsgHdr* cmsg)
{
   LinuxControlMsgHdr* nextCmsg = NULL;

   nextCmsg = (LinuxControlMsgHdr*)(((unsigned char*)cmsg) +
				    LinuxAPI_CmsgAlign(cmsg->length));
   if ((unsigned long)((char*)(nextCmsg + 1) - (char*)msg->control) >
       msg->controlLen) {
      return (LinuxControlMsgHdr*)NULL;
   }

   return nextCmsg;
}


/*
 * World ID <-> UserWorld pid conversion.
 *
 * UserWorlds need to have pid's that are easily distinguishable from console os
 * (Linux) pid's, yet bear some resemblance to their vmkernel world id
 * counterpart.  Since Linux allocates pids up to about 32,000, we should choose
 * to start UserWorld pids above that number.  Thus, a UserWorld's pid is simply
 * its kernel thread's world id plus 100,000.
 */

#define LINUX_PID_OFFSET      (LinuxPid)100000
#define INVALID_LINUX_PID     (LinuxPid)-1

#endif /* VMKERNEL_USER_LINUXAPI_H */
