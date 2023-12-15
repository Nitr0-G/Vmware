/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential

 * **********************************************************/

/*
 * fsNameSpace.h --
 *
 *      VMKernel file system namespace management functions.
 */

#ifndef _FSNAMESPACE_H
#define _FSNAMESPACE_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "return_status.h"
#include "fs_ext.h"

VMK_ReturnStatus FSS_LookupPath(const char *path, FSS_ObjectID *tailOID);
VMK_ReturnStatus FSS_CreateFilePath(const char *filePath, uint32 createFlags, 
				    void *data, FSS_ObjectID *oid);
VMK_ReturnStatus FSS_OpenFilePath(const char *filePath, uint32 flags,
				  FS_FileHandleID *fileHandleID);
VMK_ReturnStatus FSS_DumpPath(const char *path, Bool verbose);

typedef enum {
   FSN_TOKEN_INVALID,
   FSN_TOKEN_VOLUME_ROOT,
   FSN_TOKEN_DIR_OR_FILE,
   FSN_TOKEN_DIR,
} FSN_TokenType;

const char *FSN_AbsPathNTokenizer(const char *s, const char *nextToken,
                                  uint32 tokenLength, char *token,
                                  FSN_TokenType *tokenType);


#endif //_FSNAMESPACE_H
