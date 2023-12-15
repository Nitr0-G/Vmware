/**********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * vmk_nfs.h --
 *
 *      NFS client headers
 */

#ifndef _VMK__NFS_H_
#define _VMK__NFS_H_

#include "scsi_ext.h"
#include "scattergather.h"
#include "async_io.h"

typedef enum {
   LCK_GRANTED = 0,
   LCK_DENIED,
   LCK_DENIED_NOLOCKS,
   LCK_BLOCKED,
   LCK_DENIED_GRACE_PERIOD,
   LCK_DEADLOCK,
   LCK_ROFS,
   LCK_STALE_FH,
   LCK_BIG,
   LCK_FAILED
} NLM_Status;

typedef enum {
   NFS_OK = 0,
   NFS_ERR_PERM = 1,
   NFS_ERR_NOENT = 2,
   NFS_ERR_IO = 5,
   NFS_ERR_NXIO = 6,
   NFS_ERR_ACCESS = 13,
   NFS_ERR_EXIST = 17,
   NFS_ERR_XDEV = 18,
   NFS_ERR_NODEV = 19,
   NFS_ERR_NOTDIR = 20,
   NFS_ERR_ISDIR = 21,
   NFS_ERR_INVAL = 22,
   NFS_ERR_FBIG = 27,
   NFS_ERR_NOSPC = 28,
   NFS_ERR_ROFS = 30,
   NFS_ERR_MLINK = 31,
   NFS_ERR_NAMETOOLONG = 63,
   NFS_ERR_NOTEMPTY = 66,
   NFS_ERR_DQUOT = 69,
   NFS_ERR_STALE = 70,
   NFS_ERR_REMOTE = 71,
   NFS_ERR_BADHANDLE = 10001,
   NFS_ERR_NOT_SYNC = 10002,
   NFS_ERR_BAD_COOKIE = 10003,
   NFS_ERR_NOT_SUPP = 10004,
   NFS_ERR_TOOSMALL = 10005,
   NFS_ERR_SERVERFAULT = 10006,
   NFS_ERR_BADTYPE = 10007,
   NFS_ERR_JUKEBOX = 10008
} NFS_Status;

#define NFS_FHSIZE 64

typedef struct NFS_FileHandle { 
   uint32			length;
   char          		handle[NFS_FHSIZE];
   struct MountPointEntry 	*mpe;
   struct SunRPC_Client		*readClient;
   struct SunRPC_Client		*stdClient;
   struct SunRPC_Client		*nlmClient;
} NFS_FileHandle;

typedef enum NFS_FileType {
   NFS_REG = 1,
   NFS_DIR,
   NFS_BLK,
   NFS_CHR,
   NFS_LNK,
   NFS_SOCK,
   NFS_FIFO
} NFS_FileType;

typedef struct NFS_SpecData {
   uint32	specdata1;
   uint32 	specdata2;
} NFS_SpecData;

typedef struct NFS_Time {
   uint32	seconds;
   uint32	nseconds;
} NFS_Time;

typedef uint64 NFS_Size;

typedef uint64 NFS_FileID;

typedef struct NFS_FileAttributes {
   NFS_FileType	type;
   uint32	mode;
   uint32	nlink;
   uint32	uid;
   uint32	gid;
   NFS_Size	size;
   NFS_Size	used;
   NFS_SpecData	rdev;
   uint64	fsid;
   NFS_FileID	fileID;
   NFS_Time	atime;
   NFS_Time	mtime;
   NFS_Time	ctime;
} __attribute__ ((packed)) NFS_FileAttributes;

#define NFS_SET_ATTR_MODE	0x01
#define NFS_SET_ATTR_UID	0x02
#define NFS_SET_ATTR_GID	0x04
#define NFS_SET_ATTR_SIZE	0x08
#define NFS_SET_ATTR_ATIME	0x10
#define NFS_SET_ATTR_MTIME	0x11

typedef struct NFS_AsyncResult {
   SCSI_Result	scsiResult;
   NFS_Status	status;
   uint32 	bytesTransferred;
} NFS_AsyncResult;

#endif
