/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential

 * **********************************************************/

/*
 * fsNameSpace.c --
 *
 *      VMKernel file system namespace management functions.
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "x86.h"
#include "vmkernel.h"
#include "libc.h"
#include "fsNameSpace.h"
#include "objectCache.h"
#include "fss_int.h"

#define LOGLEVEL_MODULE FSN
#define LOGLEVEL_MODULE_LEN 3
#include "log.h"

#define VMFS_SLASH_STR	"vmfs/"


static VMK_ReturnStatus FSNPathWalk(const char *path, uint32 pathLen,
				    FSS_ObjectID *parentOID, char *childName);
static VMK_ReturnStatus FSNPathWalkRec(FSS_ObjectID *startOID, const char *path,
				       uint32 pathLen,
				       uint32 recLevel,
				       FSS_ObjectID *parentOID,
				       char *childName);
static VMK_ReturnStatus FSNCheckObjType(FSS_ObjectID *oid, FS_ObjectType type,
					Bool *isType);
static VMK_ReturnStatus FSNGetToken(const char *pos, uint32 bytesRem,
				    char *tokenBuf, uint32 *bytesScanned);


/*
 *-----------------------------------------------------------------------------
 *
 * FSS_LookupPath --
 *    Given a path, looks up the OID corresponding to the last element
 *    on the path. Standard UNIX file system paths are understood.
 *    Relative paths are unsupported. The FSS not yet supports symlinks.
 *
 *    'path' should be null-terminated and not exceed FS_MAX_PATH_NAME_LENGTH.
 *    'tailOID' should point to a buffer of sufficient size.
 *
 * Results:
 *    Returns VMK_OK on success, VMK_NOT_FOUND if at least one element
 *    in the path does not exist, or a VMK error code.
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
FSS_LookupPath(const char *path,
	       FSS_ObjectID *tailOID)
{
   VMK_ReturnStatus status;
   FSS_ObjectID parentOID;
   char *childName = NULL;

   if (strlen(path) > FS_MAX_PATH_NAME_LENGTH) {
      return VMK_BAD_PARAM;
   }

   childName = (char *) Mem_Alloc(FS_MAX_FILE_NAME_LENGTH);
   if (childName == NULL) {
      return VMK_NO_MEMORY;
   }

   status = FSNPathWalk(path, FS_MAX_PATH_NAME_LENGTH, &parentOID, childName);
   if (status != VMK_OK) {
      goto done;
   }

   if (childName[0] != '\0') {
      /* There were at least two elements in the path. */
      status = FSS_Lookup(&parentOID, childName, tailOID);
   } else {
      /* There was one element in the path, the virtual root. */
      FSS_CopyOID(tailOID, &parentOID);
      status = VMK_OK;
   }

 done:
   Mem_Free(childName);
   return status;
}


/*
 *-----------------------------------------------------------------------------
 *
 * FSS_CreateFilePath --
 *    Create a file or directory on a VMFS volume. createFlags
 *    can specify that the file must not exist yet (FS_CREATE_CAN_EXIST),
 *    the file should be a COW (FS_CREATE_COW), and/or file is a virtual
 *    disk image (FS_CREATE_DISK_IMAGE). createFlags also controls if the lazy
 *    zero mechanism should bypass this file (FS_COW_FILE or FS_NO_LAZYZERO)
 *
 *    'data' can be used to pass in any other values to the FS
 *    implementation. For example, while creating a raw disk mapping,
 *    'data' is used to pass in the vmhba name of the raw disk/partition to
 *    be mapped.
 *
 * Results:
 *    On success, returns VMK_OK and copies the OID of the created
 *    file into 'oid'. Otherwise, returns a VMK error code.
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
FSS_CreateFilePath(const char *filePath, uint32 createFlags, 
                   void *data, FSS_ObjectID *oid)
{
   VMK_ReturnStatus status;
   FSS_ObjectID parentOID;
   char *childName = NULL;

   if (strlen(filePath) > FS_MAX_PATH_NAME_LENGTH) {
      return VMK_BAD_PARAM;
   }

   childName = (char *) Mem_Alloc(FS_MAX_FILE_NAME_LENGTH);
   if (childName == NULL) {
      return VMK_NO_MEMORY;
   }

   status = FSNPathWalk(filePath, FS_MAX_PATH_NAME_LENGTH, &parentOID, childName);
   if (status != VMK_OK) {
      goto done;
   }

   if (FSS_IsVMFSRootOID(&parentOID)) {
      /* We only allow volume root directories at the top level. */
      status = VMK_BAD_PARAM;
      goto done;
   }

   status = FSS_CreateFile(&parentOID, childName, createFlags, data, oid);

 done:
   Mem_Free(childName);
   return status;
}


/*
 *-----------------------------------------------------------------------------
 *
 * FSS_OpenFilePath --
 *    Resolves 'filePath', and calls FSS_OpenFile() on the object
 *    named by the last element in the path.
 *
 * Results:
 *    Returns VMK_OK on success, otherwise a VMK error code.
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
FSS_OpenFilePath(const char *filePath, uint32 flags,
                 FS_FileHandleID *fileHandleID)
{
   VMK_ReturnStatus status;
   FSS_ObjectID fileOID;

   if (strlen(filePath) > FS_MAX_PATH_NAME_LENGTH) {
      return VMK_BAD_PARAM;
   }

   status = FSS_LookupPath(filePath, &fileOID);
   if (status != VMK_OK) {
      return status;
   }

   return FSS_OpenFile(&fileOID, flags, fileHandleID);
}


/*
 *-----------------------------------------------------------------------------
 *
 * FSS_DumpPath --
 *    Dump metadata of object named by 'path' onto serial line. What
 *    exactly is dumped is left to FS implementations.
 *
 * Results:
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
FSS_DumpPath(const char *path, Bool verbose)
{
   VMK_ReturnStatus status;
   FSS_ObjectID     oid;

   status = FSS_LookupPath(path, &oid);
   if (status != VMK_OK) {
      return status;
   }

   return FSS_Dump(&oid, verbose);
}


/*
 *-----------------------------------------------------------------------------
 *
 * FSN_ObjNameCacheLookup --
 *    Look up 'name' in the object's name cache. If found, copy the
 *    corresponding OID into 'oid'.
 *
 *    'oid' must point to a buffer of sufficient size.
 *    'desc->nameCacheLock' must be held by the caller.
 *
 * Results:
 *    Returns VMK_OK if the name was found, and copies the OID it maps
 *    to into 'oid. Otherwise, returns VMK_NOT_FOUND.
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
FSN_ObjNameCacheLookup(ObjDescriptorInt *desc,
		      const char *name,
		      FSS_ObjectID *oid)
{
   int i;
   FileDescriptorInt *fd;

   ASSERT(desc->objType == OBJ_DIRECTORY);

   fd = FILEDESC(desc);
   
   ASSERT(SP_IsLocked(&fd->nameCacheLock) == TRUE);

   for (i = 0; i < OBJ_NAME_CACHE_SIZE; i++) {
      if (strncmp(name, fd->nameCache[i].name, FS_MAX_FILE_NAME_LENGTH) == 0) {
	 FSS_CopyOID(oid, &fd->nameCache[i].oid);
	 return VMK_OK;
      }
   }

   return VMK_NOT_FOUND;
}

/*
 *-----------------------------------------------------------------------------
 *
 * FSNCheckObjType --
 *    Checks whether the object named by 'oid' is of type 'type.
 *
 * Results:
 *    If the object is of type 'type', sets '*isType' to TRUE,
 *    otherwise sets it to FALSE. Returns VMK_OK on success of the
 *    query, otherwise returns a VMK error code.
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
FSNCheckObjType(FSS_ObjectID *oid, FS_ObjectType type, Bool *isType)
{
   VMK_ReturnStatus status;
   ObjDescriptorInt *obj;

   // XXX Temporary hack until VC becomes a registered file system.
   if (FSS_IsVMFSRootOID(oid)) {
      if (type == OBJ_DIRECTORY) {
	 *isType = TRUE;
      } else {
	 *isType = FALSE;
      }
      return VMK_OK;
   }

   status = OC_ReserveObject(oid, &obj);
   if (status != VMK_OK) {
      return status;
   }

   if (obj->objType == type) {
      *isType = TRUE;
   } else {
      *isType = FALSE;
   }

   return VMK_OK;
}


/*
 *-----------------------------------------------------------------------------
 *
 * FSNPathWalk --
 *    Resolve an absolute path starting at the virtual root. Copies
 *    the OID of the next-to-last element into 'parentOID' and the
 *    name of the last element into 'childName'.
 *
 *    "." and ".." are returned if they are the last element.
 *
 *    "/" refers to the virtual root, in which case the virtual root's
 *    OID is copied into 'parentOID' and '*childName' is set to '\0'.
 *
 * Results:
 *    VMK_OK if the path was successfully resolved, i.e. no lookup
 *    failures or other errors occurred.
 *
 *    VMK_NOT_A_DIRECTORY if path resolution succeeded but the object
 *    named by 'parentOID' is not a directory.
 * 
 *    A VMK error code otherwise.
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
FSNPathWalk(const char *path,         // IN: path string
	    uint32 pathLen,           // IN: maximum bytes to read from 'path'
	    FSS_ObjectID *parentOID,  // OUT: OID of last element
	    char *childName)          // OUT: name of last element or '\0'
{
   VMK_ReturnStatus status;
   FSS_ObjectID rootOID;

   if (!path || !parentOID || !childName) {
      return VMK_BAD_PARAM;
   }
   if (*path != '/') {
      return VMK_BAD_PARAM;
   }

   FSS_MakeVMFSRootOID(&rootOID);
   status = FSNPathWalkRec(&rootOID, path, pathLen, 0,
			   parentOID, childName);
   if (status != VMK_OK) {
      return status;
   }

   return VMK_OK;
}

/*
 *-----------------------------------------------------------------------------
 *
 * FSNPathWalkRec --
 *    Resolve a path relative to 'startOID'. Copies the OID of the
 *    next-to-last element into 'parentOID' and the name of the last
 *    element into 'childName'. The OID copied into 'parentOID' is
 *    guaranteed not to be the OID of a symlink (if the next-to-last
 *    element is a symlink, it is resolved). 
 *
 *    'startOID' must be the OID of a directory or the virtual root.
 *
 *    "." and ".." are returned if they are the last element. If they
 *    occur elsewhere in the path, they are resolved as per normal.
 *
 *    "/" refers to the virtual root, in which case the virtual root's
 *    OID is copied into 'parentOID' and '*childName' is set to '\0'.
 *
 *    Multiple slashes ('/') are treated as a single slash. Trailing
 *    slashes are ignored.
 *
 * Results:
 *    VMK_OK if the path was successfully resolved, i.e. no lookup
 *    failures or other errors occurred.
 *
 *    VMK_NOT_A_DIRECTORY if path resolution succeeded but the object
 *    named by 'parentOID' is not a directory.
 * 
 *    A VMK error code otherwise.
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
FSNPathWalkRec(FSS_ObjectID *startOID, const char *path,
	       uint32 pathLen,
	       uint32 recLevel,
	       FSS_ObjectID *parentOID,
	       char *childName)
{
   VMK_ReturnStatus status;
   int i;
   uint32 scanned = 0, totalScanned = 0;
   uint32 level = 0;
   Bool isType;
   FSS_ObjectID *oids = NULL;
   char *tokenBuf[2];

   if (pathLen < 1 || pathLen > FS_MAX_PATH_NAME_LENGTH) {
      return VMK_BAD_PARAM;
   }
   /* 'startOID' must refer to a directory. */
   status = FSNCheckObjType(startOID, OBJ_DIRECTORY, &isType);
   if (status != VMK_OK) {
      return status;
   } else if (isType == FALSE) {
      return VMK_BAD_PARAM;
   }

   // XXX: this will turn into a recursion depth check when symlinks arrive.
   ASSERT(recLevel == 0);

   oids = (FSS_ObjectID *) Mem_Alloc(2 * sizeof(FSS_ObjectID));
   if (oids == NULL) {
      return VMK_NO_MEMORY;
   }

   tokenBuf[0] = tokenBuf[1] = NULL;
   for (i = 0; i < 2; i++) {
      tokenBuf[i] = (char *) Mem_Alloc(FS_MAX_FILE_NAME_LENGTH);
      if (tokenBuf[i] == NULL) {
	 status = VMK_NO_MEMORY;
	 goto done;
      }
   }

   /* Initialize the starting point. */
   if (path[0] == '/') {
      /* Start at the virtual root. */
      FSS_MakeVMFSRootOID(&oids[0]);
   } else {
      /* Start at the specified directory. */
      FSS_CopyOID(&oids[0], startOID);
   }

   status = FSNGetToken((char *)path, pathLen, tokenBuf[1], &scanned);
   if (status == VMK_NOT_FOUND) {
      /* No tokens found. Two possibilities: "/" or empty path. */
      if (path[0] == '/') {
	 FSS_MakeVMFSRootOID(parentOID);
	 *childName = '\0';
	 status = VMK_OK;
      } else {
	 status = VMK_NOT_FOUND;
      }

      goto done;
   }

   totalScanned += scanned;
   level = 2;

   /*
    * Walk the path.
    *
    * Termination: Input string has finite length. FSNGetToken()
    * returns VMK_NOT_FOUND if no token is found or if 
    * 'bytesRem == 0'. Otherwise, 'scanned > 0' thus decreasing
    * 'pathLen - totalScanned'.
    */
   while (1) {
      status = FSNGetToken((char *)path + totalScanned, pathLen - totalScanned,
			   tokenBuf[level%2], &scanned);

      if (status == VMK_NOT_FOUND) {
	 /* No more tokens. */
	 status = VMK_OK;
	 break;
      } else {
	 /* Another token. Resolve OID of last token and proceed. */
	 status = FSS_Lookup(&oids[(level-2)%2], tokenBuf[(level-1)%2],
			     &oids[(level-1)%2]);
	 if (status != VMK_OK) {
	    goto done;
	 }

#if 0	 
	 status = FSNCheckObjType(&oids[(level-1)%2], OBJ_SYMLINK, &isType);
	 if (status != VMK_OK) {
	    goto done;
	 }

	 if (isType) {
	    /*
	     * Resolve the (OID, name) that the symlink points to. If symlink
	     * contains "/", root OID goes into oids[(level-1)%2] and
	     * 'tokenBuf' is untouched. Otherwise, 'OID' goes into
	     * oids[(level-1)%2] and 'name' goes into tokenBuf[level%2].
	     *
	     * oids[(level-2)%2], the first argument to FSNPathWalkRec() below,
	     * is guaranteed not to be the OID of a symlink, because:
	     *  1) Recursion always starts at a directory (virtual root or CWD).
	     *  2) For level > 1, oids[level%2] was produced by a call to
	     *     FSNPathWalkRec(oids[(level-1)%2]).
	     */
	    FSNPathWalkRec(&oids[(level-2)%2], symLinkContents,
			   lengthOf(symLinkContents), recLevel+1,
			   &oids[(level-1)%2], tmpBuf);
	 }
#endif
      }

      totalScanned += scanned;
      level++;
   }

   ASSERT(status == VMK_OK);
   /* Verify that 'parentOID' refers to a directory. */
   status = FSNCheckObjType(&oids[(level-2)%2], OBJ_DIRECTORY, &isType);
   if (status != VMK_OK) {
      goto done;
   } else if (isType == FALSE) {
      status = VMK_NOT_A_DIRECTORY;
      goto done;
   }

   /* Return OID of next-to-last element and name of last element. */
   FSS_CopyOID(parentOID, &oids[(level-2)%2]);
   strcpy(childName, tokenBuf[(level-1)%2]);

 done:
   if (oids) {
      Mem_Free(oids);
   }
   for (i = 0; i < 2; i++) {
      if (tokenBuf[i] != NULL) {
	 Mem_Free(tokenBuf[i]);
      }
   }
   return status;
}


/*
 *-----------------------------------------------------------------------------
 *
 * FSNGetToken --
 *    Extracts the next token from the string pointed to by 'pos' and
 *    copies it into 'tokenBuf'. No more than 'bytesRem' bytes will be
 *    read from the input string.
 *
 *    Returns VMK_OK if a token was extracted, VMK_NOT_FOUND if none,
 *    or VMK_NAME_TOO_LONG if the token was too long.
 *
 * Results:
 *    'tokenBuf' contains the token extracted, if any. 'tokenBuf' is
 *    null-terminated.
 *
 *    'bytesScanned' is set to the number of bytes read from the input
 *    string, not including a trailing '\0'.
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */
static VMK_ReturnStatus
FSNGetToken(const char *pos,       // IN: initial position
	    uint32 bytesRem,       // IN: maximum bytes to read from '*pos'
	    char *tokenBuf,        // IN: pointer to buffer of
	                           //     length FS_MAX_FILE_NAME_LENGTH bytes
	    uint32 *bytesScanned)  // OUT: bytes read
{
   char *currPos;
   uint32 scanned = 0, remaining = 0, copied = 0;

   currPos = (char *) pos;
   remaining = bytesRem;

   while (remaining > 0 && copied < (FS_MAX_FILE_NAME_LENGTH - 1)) {
      switch (*currPos)
	 {
	 case '/':   /* skip if leading, stop if trailing */
	    if (copied > 0) {
	       remaining = 1;
	    }
	    break;
	 case '\0':  /* stop */
	    remaining = 1;
	    scanned--;  /* Don't include trailing '\0' in count. */
	    break;
	 default:    /* copy */
	    tokenBuf[copied] = *currPos;
	    copied++;
	 }

      remaining--;
      scanned++;
      currPos++;
   }

   *bytesScanned = scanned;

   tokenBuf[copied] = '\0';

   if (copied == FS_MAX_FILE_NAME_LENGTH - 1) {
      /* If we copied FS_MAX_FILE_NAME_LENGTH - 1 bytes and the next character
       * is not a terminating token, the name is too long. */
      if (remaining > 0 && *currPos != '/' && *currPos != '\0') {
	 return VMK_NAME_TOO_LONG;
      }
   } else if (copied == 0) {
      return VMK_NOT_FOUND;
   }

   return VMK_OK;
}


/*
 *-----------------------------------------------------------------------------
 *
 * FSN_AbsPathNTokenizer (deprecated) --
 *    Given a string, 's', and a starting position within that string,
 *    'nextToken', copies the next token found to 'token'. The copied
 *    token is null-terminated.
 *
 *    A token is a sequence of bytes whose length is strictly less
 *    than 'tokenLength', and where no byte is an ASCII '/' or '\0'.
 *    Thus, no more than 'tokenLength' bytes will be written to 'token'.
 *
 * Results:
 *
 * Side effects:
 *
 *-----------------------------------------------------------------------------
 */

const char *
FSN_AbsPathNTokenizer(const char *s, const char *nextToken, uint32 tokenLength,
                      char *token, FSN_TokenType *tokenType)
{
   uint32 count = 0;
   char *d = token;

   if (*s == '\0') {
      *tokenType = FSN_TOKEN_INVALID;
      return NULL;  
   }

   if (nextToken == NULL) {
      /* Look for volume name at the beginning of the path string */

      /* Ignore "/vmfs/" prefix, if any. */
      if ( *s == '/') {
         int len = strlen(VMFS_SLASH_STR);
         
         s++;
         if (strncmp(s, VMFS_SLASH_STR, len) == 0) {
            s += len;
         }
      }

      while (*s != '/' && *s != '\0' && count != (tokenLength - 1)) {
         *d++ = *s++;
         count++;
      }
      *d = '\0';

      *tokenType = FSN_TOKEN_VOLUME_ROOT;
   } else {
      s = nextToken;
      while (*s != '/' && *s != '\0' && count != (tokenLength - 1)) {
         *d++ = *s++;
         count++;
      }
      *d = '\0';
      *tokenType = (*s == '/') ? FSN_TOKEN_DIR : FSN_TOKEN_DIR_OR_FILE;
   }
   /* make sure we didn't get a string that ends with a '/' */
   if (*s == '/') {
      s++;
      count++;
   }

   if ((count == tokenLength - 1) && (*s != '/' || *s != '\0')) {
      *tokenType = FSN_TOKEN_INVALID;
      return NULL;
   }
   LOG(3, "Token: %s, Next: %s", token, s); 
   return (*s == '\0') ? NULL : s;
}
