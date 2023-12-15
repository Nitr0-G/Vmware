/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 *
 * **********************************************************/

/*
 * fsClientLib.c --
 *
 *      Convenience wrappers around FSS-exported functions. Originally
 *      intended for in-VMKernel clients like the swapper. More
 *      sophisticated file system consumers should use FSS-exported
 *      functions directly.
 */

#ifndef _FSCLIENTLIB_H
#define _FSCLIENTLIB_H


#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "return_status.h"
#include "fs_ext.h"


VMK_ReturnStatus FSClient_ReopenFile(FS_FileHandleID fileHandleID, uint32 flags,
				     FS_FileHandleID *newFileHandleID);
VMK_ReturnStatus FSClient_GetFileAttributes(FS_FileHandleID fileHandleID,
					    FS_FileAttributes *attrs);
VMK_ReturnStatus FSClient_SetFileAttributes(FS_FileHandleID fileHandleID,
					    uint16 opFlags,
					    const FS_FileAttributes *attrs);


#endif
