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

#include "vm_types.h"
#include "vm_libc.h"
#include "fsClientLib.h"
#include "fs_ext.h"
#include "fsSwitch.h"
#include "fsNameSpace.h"
#include "objectCache.h"


VMK_ReturnStatus
FSClient_ReopenFile(FS_FileHandleID fileHandleID, uint32 flags,
		    FS_FileHandleID *newFileHandleID)
{
   VMK_ReturnStatus status;
   FSS_ObjectID     *oid;

   oid = (FSS_ObjectID *) Mem_Alloc(sizeof(FSS_ObjectID));
   if (oid == NULL) {
      return VMK_NO_MEMORY;
   }

   status = FSS_LookupFileHandle(fileHandleID, oid);
   if (status != VMK_OK) {
      goto done;
   }

   status = FSS_OpenFile(oid, flags, newFileHandleID);

 done:
   Mem_Free(oid);
   return status;   
}


VMK_ReturnStatus
FSClient_GetFileAttributes(FS_FileHandleID fileHandleID,
			   FS_FileAttributes *attrs)
{
   VMK_ReturnStatus status;
   FSS_ObjectID     *oid;

   oid = (FSS_ObjectID *) Mem_Alloc(sizeof(FSS_ObjectID));
   if (oid == NULL) {
      return VMK_NO_MEMORY;
   }

   status = FSS_LookupFileHandle(fileHandleID, oid);
   if (status != VMK_OK) {
      goto done;
   }

   status = FSS_GetFileAttributes(oid, attrs);

 done:
   Mem_Free(oid);
   return status;
}

VMK_ReturnStatus
FSClient_SetFileAttributes(FS_FileHandleID fileHandleID, uint16 opFlags,
			   const FS_FileAttributes *attrs)
{
   VMK_ReturnStatus status;
   FSS_ObjectID     *oid;

   oid = (FSS_ObjectID *) Mem_Alloc(sizeof(FSS_ObjectID));
   if (oid == NULL) {
      return VMK_NO_MEMORY;
   }

   status = FSS_LookupFileHandle(fileHandleID, oid);
   if (status != VMK_OK) {
      goto done;
   }

   status = FSS_SetFileAttributes(oid, opFlags, attrs);

 done:
   Mem_Free(oid);
   return status;
}
