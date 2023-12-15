/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/* 
 * userFile.h - 
 *	UserWorld file objects
 */

#ifndef VMKERNEL_USER_USERFILE_H
#define VMKERNEL_USER_USERFILE_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "vm_basic_types.h"
#include "vmkernel.h"

typedef struct UserFile_Cache {
   /*
    * Optional read-ahead/write-behind cache.
    * Not coherent if the file is opened twice or readMPN/writeMPN
    * are used on the same open file; see PR 44754.
    */
   void *buffer;    // cached data
   Bool valid;      // buffer is valid (must be FALSE if buffer == NULL)
   Bool dirty;      // buffer reflects changes not yet written to disk
   Bool eofValid;   // eof is valid
   Bool eofDirty;   // eof reflects changes not yet written to disk
   uint64 offset;   // offset of buffer within file (512-byte aligned)
   uint32 length;   // length of valid part of buffer (512-byte aligned)
   uint64 eof;      // end of file offset
} UserFile_Cache;

typedef struct UserFile_ObjInfo {
   FSS_ObjectID oid;
   FS_FileHandleID handle;
   UserFile_Cache cache;
} UserFile_ObjInfo;

struct User_CartelInfo;

extern void UserFile_CartelInit(User_CartelInfo *uci);

extern void UserFile_CartelCleanup(User_CartelInfo *uci);

extern UserObj* UserFile_OpenVMFSRoot(struct User_CartelInfo *uci,
                                      uint32 openFlags);

extern void UserFile_Sync(User_CartelInfo* uci);

extern VMK_ReturnStatus
   UserFile_RenameOpenFile(UserObj *obj,
                           UserObj *newParent, const char *newArc);

#endif /* VMKERNEL_USER_USEROBJ_H */
