/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * userFile.c --
 *
 *      Userworld interface to VMFS files and (pseudo) directories.
 *      The directories implemented include /vmfs (which holds all
 *      VMFS filesystems accessible on the machine) and a subdirectory
 *      of /vmfs for each filesystem.  Currently each filesystem
 *      appears twice in /vmfs, once under its numeric colon-separated
 *      name and once under its user-friendly name.  In the future the
 *      user-friendly name should be manifested as a symlink to the
 *      numeric name, but that's not implemented yet.  VMFS
 *      filesystems currently have no internal directories.
 */

#include "user_int.h"
#include "fsSwitch.h"
#include "userObj.h"
#include "vmnix_syscall.h" // for VMnix_FSListResult
#include "iocontrols.h"    // for FS_MAGIC_NUMBER
#include "vmnix_if_dist.h" // for VMNIX_FILE_NAME_LENGTH
#include "fs_dist.h"
#include "userFile.h"
#include "helper.h"
#include "libc.h"
#include "fsClientLib.h"

#define LOGLEVEL_MODULE     UserFile
#include "userLog.h"

static VMK_ReturnStatus UserFileOpen(UserObj* obj, const char *arc,
                                     uint32 flags, LinuxMode mode,
                                     UserObj **objOut);

static VMK_ReturnStatus UserFileClose(UserObj* obj,
                                      User_CartelInfo* uci);

static VMK_ReturnStatus UserFileRead(UserObj* obj, UserVA userData,
                                     uint64 offset, uint32 length,
                                     uint32 *bytesRead);

static VMK_ReturnStatus UserFileReadMPN(UserObj* obj, MPN mpn,
                                        uint64 offset, uint32 *bytesRead);

static VMK_ReturnStatus UserFileWrite(UserObj* obj, UserVAConst userData,
                                      uint64 offset, uint32 length,
                                      uint32 *bytesWritten);

static VMK_ReturnStatus UserFileWriteMPN(UserObj* obj, MPN mpn,
                                         uint64 offset, uint32 *bytesWritten);

static VMK_ReturnStatus UserFileStat(UserObj* obj, LinuxStat64* statbuf);

static VMK_ReturnStatus UserFileChmod(UserObj* obj, LinuxMode mode);

static VMK_ReturnStatus UserFileChown(UserObj* obj, Identity_UserID owner,
                                      Identity_GroupID group);

static VMK_ReturnStatus UserFileTruncate(UserObj* obj, uint64 size);

static VMK_ReturnStatus UserFileUnlink(UserObj* parent, const char* arc);

static VMK_ReturnStatus UserFileStatFS(UserObj* obj, LinuxStatFS64* statbuf);

static VMK_ReturnStatus UserFileRename(UserObj* newDir, const char *newArc,
                                       UserObj* oldDir, const char *oldArc);

static VMK_ReturnStatus UserFileFcntl(UserObj* obj, uint32 cmd, uint32 arg);
static VMK_ReturnStatus UserFileFsync(UserObj* obj, Bool dataOnly);
static VMK_ReturnStatus UserFileIoctl(UserObj* obj, uint32 cmd,
                                      LinuxIoctlArgType type, uint32 size,
                                      void *userData,
                                      uint32 *result);
static VMK_ReturnStatus UserFileReaddir (UserObj* obj,
                                         UserVA /* LinuxDirent64* */ userData,
                                         uint32 length, uint32* bytesRead);
static VMK_ReturnStatus UserFileMkdir(UserObj* obj, const char *arc,
                                      LinuxMode mode);
static VMK_ReturnStatus UserFileRmdir(UserObj* obj, const char *arc);
static VMK_ReturnStatus UserFileGetName(UserObj* obj, char* arc,
                                        uint32 length);
static VMK_ReturnStatus UserFileToString(UserObj *obj, char *string,
					 int length);

static void UserFileTimerCallback(void *data, Timer_AbsCycles timestamp);
static void UserFileTimerHelper(void *data);
static VMK_ReturnStatus UserFileCacheEof(UserObj *obj);

/* Methods on the /vmfs subtree */
static UserObj_Methods vmfsMethods = USEROBJ_METHODS(
   UserFileOpen,
   UserFileClose,
   UserFileRead,
   UserFileReadMPN,
   UserFileWrite,
   UserFileWriteMPN,
   UserFileStat,
   UserFileChmod,
   UserFileChown,
   UserFileTruncate,
   (UserObj_UtimeMethod) UserObj_Nop, // XXX FSS provides no way to implement
   UserFileStatFS,
   (UserObj_PollMethod) UserObj_Nop,
   UserFileUnlink,
   UserFileMkdir,
   UserFileRmdir,
   UserFileGetName,
   (UserObj_ReadSymLinkMethod) UserObj_NotImplemented,
   (UserObj_MakeSymLinkMethod) UserObj_NotImplemented,
   (UserObj_MakeHardLinkMethod) UserObj_NotImplemented,
   UserFileRename,
   (UserObj_MknodMethod) UserObj_NotImplemented,
   UserFileFcntl,
   UserFileFsync,
   UserFileReaddir,
   UserFileIoctl,
   UserFileToString,
   (UserObj_BindMethod) UserObj_NotASocket,
   (UserObj_ConnectMethod) UserObj_NotASocket,
   (UserObj_SocketpairMethod) UserObj_NotASocket,
   (UserObj_AcceptMethod) UserObj_NotASocket,
   (UserObj_GetSocketNameMethod) UserObj_NotASocket,
   (UserObj_ListenMethod) UserObj_NotASocket,
   (UserObj_SetsockoptMethod) UserObj_NotASocket,
   (UserObj_GetsockoptMethod) UserObj_NotASocket,
   (UserObj_SendmsgMethod) UserObj_NotASocket,
   (UserObj_RecvmsgMethod) UserObj_NotASocket,
   (UserObj_GetPeerNameMethod) UserObj_NotASocket,
   (UserObj_ShutdownMethod) UserObj_NotASocket
);

// For file read/write:
#define BUFFER_SIZE (16 * DISK_SECTOR_SIZE) // 8 KB

// For stat:
// The following blocksize is used for /vmfs itself.
#define VMFS_DEFAULT_BLOCKSIZE (1024 * 1024)
// XXX How does vmkfs.c get this value into the st_dev field?
#define VMFS_STAT_DEV 14

//XXX: Code duplication! This code exists in vmkfs.c too.
#define USERFILE_MODE(flags, mode)	(UserFileGetType(flags) | \
                                         ((mode) & (LINUX_MODE_IRWXU | \
                                                    LINUX_MODE_IRWXG | \
                                                    LINUX_MODE_IRWXO | \
                                                    LINUX_MODE_ISVTX)))

static LinuxMode
UserFileGetType(FS_DescriptorFlags descFlags)
{
   if (descFlags & FS_DIRECTORY) {
      return LINUX_MODE_IFDIR;
   }
   if (descFlags & FS_LINK) {
      return LINUX_MODE_IFLNK;
   }
   return LINUX_MODE_IFREG;  
}

/*
 *----------------------------------------------------------------------
 *
 * UserFileCreateObj --
 *
 *      Create a UserObj for an open file.
 *
 * Results:
 *      Pointer to the new UserObj, or NULL if out of memory.
 *
 * Side effects:
 *      UserObj is allocated with refcount = 1.
 *
 *----------------------------------------------------------------------
 */
static UserObj*
UserFileCreateObj(User_CartelInfo* uci, const FSS_ObjectID *oid,
                  FS_FileHandleID handle, uint32 flags)
{
   UserFile_ObjInfo *vmfsObject;
   UserObj *obj;

   // A VMFS object may not have a file handle, but it should definitely
   // have an object ID
   ASSERT(oid != NULL);

   vmfsObject = User_HeapAlloc(uci, sizeof(UserFile_ObjInfo));
   if (vmfsObject == NULL) {
      return NULL;
   }
   FSS_CopyOID(&vmfsObject->oid, oid);
   vmfsObject->handle = handle;
   vmfsObject->cache.buffer = NULL;
   vmfsObject->cache.valid = FALSE;
   vmfsObject->cache.dirty = FALSE;
   vmfsObject->cache.eofValid = FALSE;
   vmfsObject->cache.eofDirty = FALSE;

   obj = UserObj_Create(uci, USEROBJ_TYPE_FILE,
                        (UserObj_Data) vmfsObject, &vmfsMethods, flags);
   if (obj == NULL) {
      User_HeapFree(uci, vmfsObject);
   }
   return obj;
}

static void
UserFileDestroyObj(User_CartelInfo *uci, UserObj *obj)
{
   ASSERT(obj != NULL);
   ASSERT(obj->type == USEROBJ_TYPE_FILE);
   ASSERT(obj->data.vmfsObject->handle == FS_INVALID_FILE_HANDLE);

   User_HeapFree(uci, obj->data.vmfsObject);
   obj->type = USEROBJ_TYPE_NONE;
   obj->data.raw = 0;
}

/*
 *----------------------------------------------------------------------
 *
 * UserFileOpen --
 *
 *      Open the specified arc relative to the specified VMFS and
 *      return a new UserObj.
 *
 * Results:
 *      VMK_OK if the object can be found, error condition otherwise
 *
 * Side effects:
 *      A new file may be created.
 *      UserObj for the object is allocated with refcount = 1.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserFileOpen(UserObj* parent, const char* arc,
             uint32 flags, LinuxMode mode, UserObj** objOut)
{
   User_CartelInfo* uci = MY_USER_CARTEL_INFO;
   /*const*/ Identity* ident = &MY_RUNNING_WORLD->ident;
   uint16 faFlags = 0;
   FS_FileAttributes fa;
   FS_FileHandleID fhid = FS_INVALID_FILE_HANDLE;
   VMK_ReturnStatus status;
   uint32 ofFlags;
   Bool knownZeroLength = FALSE;
   FSS_ObjectID foid;
   Bool isRegFile = FALSE;

   UWLOG(1, "(arc=%s, flags=%#x, mode=%#x)", arc, flags, mode);

   // Here we should verify that the invoking user has search
   // permission on this directory.  Currently, though, the root
   // directory of a VMFS always has search permission for everyone.
   if (strcmp(arc, ".") == 0 || strcmp(arc, "") == 0) {
      FS_FileHandleID newFileHandleID = parent->data.vmfsObject->handle;

      if (parent->data.vmfsObject->handle != FS_INVALID_FILE_HANDLE) {
         status = FSClient_ReopenFile(parent->data.vmfsObject->handle, 
                                      flags, &newFileHandleID);
         if (status != VMK_OK) {
            return status;
         }
      }
      *objOut = UserFileCreateObj(uci, &parent->data.vmfsObject->oid,
                                  newFileHandleID, flags);
      if (*objOut == NULL) {
         FSS_CloseFile(newFileHandleID);
         return VMK_NO_MEMORY;
      }
      return VMK_OK;
   }

   
   if (strcmp(arc, "..") == 0) {
      if (FSS_IsVMFSRootOID(&parent->data.vmfsObject->oid)) {
         UserObj *root;
         
         // Get a reference to the shared root ("/") object.  This object
         // always has flags = USEROBJ_OPEN_STAT, and should never be
         // returned as a final lookup result because fcntl(F_SETFL)
         // would change the field for everyone.
         status = UserProxy_OpenRoot(uci, &root);
         if (status != VMK_OK) {
            return status;
         }
         
         // Open our own copy of "/" with the caller's specified flags.
         status = root->methods->open(root, "", flags, mode, objOut);
         (void) UserObj_Release(uci, root);
         return status;
      } else {
         // XXX: This needs to be generalized for VMFS directories
         *objOut = UserFile_OpenVMFSRoot(uci, flags);
         if (*objOut == NULL) {
            return VMK_NO_RESOURCES;
         }
         return VMK_OK;
      }
   }

   if ((flags & USEROBJ_OPEN_VMFSFILE) != 0) {
      ofFlags = FS_OPEN_FLAGS_EXTRACT(flags);
   } else {
      switch (flags & USEROBJ_OPEN_FOR) {
      case USEROBJ_OPEN_STAT:
         ofFlags = 0;
         break;
      case USEROBJ_OPEN_RDONLY:
         ofFlags = FILEOPEN_READ;
         break;
      case USEROBJ_OPEN_WRONLY:
      case USEROBJ_OPEN_OWNER:
         ofFlags = FILEOPEN_WRITE | FILEOPEN_READ; //XXX: cache needs R-M-W
         break;
      case USEROBJ_OPEN_RDWR:
         ofFlags = FILEOPEN_READ | FILEOPEN_WRITE;
         break;
      default:
         ASSERT(FALSE); // should not be possible
         return VMK_BAD_PARAM;
      }
   }

   status = FSS_Lookup(&parent->data.vmfsObject->oid, arc, &foid);

   if (status == VMK_OK) {
      // File exists already.  Is that OK?
      if ((flags & (USEROBJ_OPEN_CREATE|USEROBJ_OPEN_EXCLUSIVE)) ==
          (USEROBJ_OPEN_CREATE|USEROBJ_OPEN_EXCLUSIVE)) {
         return VMK_EXISTS;
      }
      // Check if user has permission to open this file
      status = FSS_GetFileAttributes(&foid, &fa);
      if (status != VMK_OK) {
         return status;
      }

      status = UserIdent_CheckAccess(ident, flags,
                                     fa.uid, fa.gid, fa.mode);
      if (status != VMK_OK) {
         return status;
      }

      isRegFile = (UserFileGetType(fa.flags) == LINUX_MODE_IFREG) ?
         TRUE : FALSE;
   } else if (status == VMK_NOT_FOUND && (flags & USEROBJ_OPEN_CREATE)) {
       uint32 cfFlags = 0;
      // File not found; try to create it

      if (!(flags & USEROBJ_OPEN_EXCLUSIVE)) {
         // In case of a race with someone else creating the file...
         cfFlags |= FS_CREATE_CAN_EXIST;
      }
      status = FSS_CreateFile(&parent->data.vmfsObject->oid, arc,
                              cfFlags, NULL, &foid);
      if (status != VMK_OK) {
         UWLOG(2, "Create %s in "FS_OID_FMTSTR" returned %s",
               arc, FS_OID_VAARGS(&parent->data.vmfsObject->oid),
               VMK_ReturnStatusToString(status));
         return status;
      }
      
      isRegFile = TRUE;

      UWLOG(2, "Created %s, ofFlags are %x", arc, ofFlags);

      faFlags |= FILEATTR_SET_PERMISSIONS;
      fa.uid = ident->euid;
      // Here we should check if the setgid bit of this directory
      // is set, and if so, set fa.gid to the directory's gid.
      // Currently, though, the root directory of a VMFS can't have
      // its setgid bit set.
      fa.gid = ident->egid;
      fa.mode = mode;
      knownZeroLength = TRUE;
   } else {
      UWLOG(2, "Lookup "FS_OID_FMTSTR" returned %s",
            FS_OID_VAARGS(&parent->data.vmfsObject->oid),
            VMK_ReturnStatusToString(status));
      return status;
   }

   ASSERT(status == VMK_OK);

   if (flags & USEROBJ_OPEN_TRUNCATE) {
      faFlags |= FILEATTR_SET_LENGTH;
      fa.length = 0;
      knownZeroLength = TRUE;
   }

   if (faFlags) {
      status = FSS_SetFileAttributes(&foid, faFlags, &fa);
      if (status != VMK_OK) {
         UWLOG(2, "SetFileAttributes on "FS_OID_FMTSTR" returned %s",
               FS_OID_VAARGS(&foid), VMK_ReturnStatusToString(status));
         return status;
      }
   }

   if (ofFlags && isRegFile) {
      status = FSS_OpenFile(&foid, ofFlags, &fhid);
      if (status != VMK_OK) {
         UWLOG(2, "Open on "FS_OID_FMTSTR" returned %s",
               FS_OID_VAARGS(&foid), VMK_ReturnStatusToString(status));
         return status;
      }
   }

   *objOut = UserFileCreateObj(uci, &foid, fhid, flags);
   if (*objOut == NULL) {
      if (fhid != FS_INVALID_FILE_HANDLE) {
         (void) FSS_CloseFile(fhid);
      }
      return VMK_NO_MEMORY;
   }

   if (knownZeroLength) {
      UserFile_Cache *const cache = &(*objOut)->data.vmfsObject->cache;
      cache->eof = 0;
      cache->eofValid = TRUE;
      cache->eofDirty = FALSE;
   }

   UWLOG(1, "returning OID "FS_OID_FMTSTR" handle %"FMT64"d", 
         FS_OID_VAARGS(&foid), fhid);
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserFileClose --
 *
 *      Close the underlying file handle in obj.   Must not be null.
 *
 * Results:
 *      VMK_OK or error status from the underlying io object.
 *
 * Side effects:
 *      The file handle is closed.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserFileClose(UserObj* obj, User_CartelInfo* uci)
{
   VMK_ReturnStatus status = VMK_OK, status2 = VMK_OK;

   ASSERT(obj != NULL);
   ASSERT(obj->type == USEROBJ_TYPE_FILE);
   if (obj->data.vmfsObject->handle != FS_INVALID_FILE_HANDLE) {
      status = obj->methods->fsync(obj, FALSE);
      if (obj->data.vmfsObject->cache.buffer != NULL) {
         User_HeapFree(uci, obj->data.vmfsObject->cache.buffer);
      }
      status2 = FSS_CloseFile(obj->data.vmfsObject->handle);
      obj->data.vmfsObject->handle = FS_INVALID_FILE_HANDLE;
   } else {
      ASSERT(obj->data.vmfsObject->cache.buffer == NULL);
   }
   UserFileDestroyObj(uci, obj);
   return status || status2;
}


/*
 *----------------------------------------------------------------------
 *
 * UserFileRead --
 *
 *      Read up to length bytes at the specified offset in the given
 *	file.  Sets bytesRead to number of bytes actually
 * 	read.  BytesRead is undefined if an error is returned.
 *
 * Results:
 *      VMK_OK if successful; error code otherwise.  *bytesRead is set
 *      to number of bytes read.  We signal reading entirely beyond
 *      the end of file by returning 0 bytes but no error code; this
 *      is the specified behavior of the read() system call we're
 *      implementing.
 *
 * Side effects:
 *      Bytes are read from the object.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserFileRead(UserObj* obj,
             UserVA userData,
             uint64 offset,
             uint32 length,
             uint32 *bytesRead)
{
   VMK_ReturnStatus status = VMK_OK;
   uint32 chunkRead = 0, usable;
   uint64 chunkOffset = 0;
   UserFile_Cache *const cache = &obj->data.vmfsObject->cache;

   ASSERT(obj != NULL);
   ASSERT(obj->type == USEROBJ_TYPE_FILE);
   ASSERT(bytesRead != NULL);
   *bytesRead = 0;

   if (obj->data.vmfsObject->handle == FS_INVALID_FILE_HANDLE) {
      return VMK_INVALID_HANDLE;
   }

   if (length == 0) {
      return VMK_OK;
   }

   status = UserFileCacheEof(obj);
   if (status != VMK_OK) {
      return status;
   }
   ASSERT(cache->eofValid);

   /* Check if reading entirely beyond end of file */
   if (offset >= cache->eof) {
      return VMK_OK;
   }
      
   /* Check if reading partly beyond end of file */
   if (cache->eof - offset < length) {
      length = cache->eof - offset;
   }

   /* Satisfy initial portion of read from cache if possible */
   if (cache->valid &&
       cache->offset <= offset && offset < cache->offset + cache->length) {
      ASSERT(cache->buffer);
      // usable = amount of user data found in cache
      usable = MIN(length, (cache->offset + cache->length) - offset);
      UWLOG(6, "read cache hit, %u bytes at offset %Lu", usable, offset);
      status = User_CopyOut(userData,
                            cache->buffer + (offset - cache->offset), usable);
      if (status != VMK_OK) {
         UWLOG(1, "User_CopyOut(0x%x, %p, %d) returned %s",
               userData, cache->buffer + (offset - cache->offset), usable,
               VMK_ReturnStatusToString(status));
         return status;
      }
      length -= usable;
      offset += usable;
      userData += usable;
      *bytesRead += usable;

      /* Done yet? */
      if (length == 0) {
         return VMK_OK;
      }
   }

   /* Allocate buffer if we don't already have one */
   if (cache->buffer == NULL) {
      cache->buffer = User_HeapAlloc(MY_USER_CARTEL_INFO, BUFFER_SIZE);
      if (cache->buffer == NULL) {
         return VMK_NO_RESOURCES;
      }
   }

   /* Flush cache if dirty */
   if (cache->dirty) {
      status = obj->methods->fsync(obj, TRUE);
      if (status != VMK_OK) {
         UWLOG(0, "Fsync failed: %s", VMK_ReturnStatusToString(status));
         return status;
      }
   }

   /* Satisfy remaining portion in one or more reads of BUFFER_SIZE or less */
   while (length) {
      uint32 chunkLength, prepad;

      /* Compute sizes for this iteration */
      // prepad = distance from preceding sector boundary to start of user data
      prepad = offset & (DISK_SECTOR_SIZE - 1);
      // chunkOffset = offset in file where we will begin reading 
      chunkOffset = offset - prepad;
      // chunkLength = amount to read, including padding
      chunkLength = MIN(ALIGN_UP(cache->eof, DISK_SECTOR_SIZE) - chunkOffset,
                        BUFFER_SIZE);
      // usable = amount of user data read
      usable = MIN(chunkLength - prepad, length);
      UWLOG(6, "read cache miss, %u bytes at offset %Lu", usable, offset);
      UWLOG(7, "chunkLength=%d, prepad=%d", chunkLength, prepad);

      ASSERT(cache->buffer && !cache->dirty);

      /* Do read */
      status = FSS_BufferIO(obj->data.vmfsObject->handle,
                            chunkOffset, (uint64)(unsigned long)cache->buffer,
                            chunkLength, FS_READ_OP, SG_VIRT_ADDR,
                            &chunkRead);

      if (status != VMK_OK) {
         UWLOG(2, "FSS_BufferIO on handle %"FMT64"d returned %s",
               obj->data.vmfsObject->handle, VMK_ReturnStatusToString(status));
         // Conservatively assume the cache is trash
         chunkRead = 0;
         cache->valid = FALSE;
         cache->dirty = FALSE;
         break;
      }

      UWLOG_DumpBuffer(8, cache->buffer, chunkRead);

      if (chunkRead != chunkLength) {
         // A short read can happen only if we are mistaken about
         // cache->eof; i.e., it has changed since we asked for it
         // above.
         ASSERT(chunkRead < chunkLength);
         // The FSS interface works in DISK_SECTOR_SIZE units, so the
         // read must be short by at least a whole sector.
         ASSERT(chunkRead <= chunkLength - DISK_SECTOR_SIZE);
         if (chunkRead <= prepad) {
            break;
         }
         usable = chunkRead - prepad;
         length = usable; // force loop exit
      }

      /* Copy out user data */
      status = User_CopyOut(userData, cache->buffer + prepad, usable);
      if (status != VMK_OK) {
         UWLOG(1, "User_CopyOut(0x%x, %p, %d) returned %s",
               userData, cache->buffer + prepad, usable,
               VMK_ReturnStatusToString(status));
         break;
      }

      length -= usable;
      offset += usable;
      userData += usable;
      *bytesRead += usable;
   }

   /* Save results of last read in cache */
   if (chunkRead != 0) {
      ASSERT(cache->buffer);
      cache->valid = TRUE;
      cache->dirty = FALSE;
      cache->offset = chunkOffset;
      cache->length = chunkRead;
   }
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * UserFileReadMPN --
 *
 *      Read up to PAGE_SIZE bytes at the specified offset in the given
 *      file.  Sets bytesRead to number of bytes actually read.
 *      BytesRead is undefined if an error is returned.  Remaining bytes on
 *      the page are not touched.
 *
 * Results:
 *      VMK_OK if successful; error code otherwise.  *bytesRead is set
 *      to the number of bytes read.  We signal reading entirely
 *      beyond the end of file by returning 0 bytes but no error code;
 *      this is fairly arbitrary, but is consistent with the read method.
 *
 * Side effects:
 *      Bytes are read from the object.
 *
 * Bugs:
 *      Ignores the cache that may be associated with the file.
 *      See PR 44754.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserFileReadMPN(UserObj* obj,
                MPN mpn,
                uint64 offset,
                uint32 *bytesRead)
{
   VMK_ReturnStatus status = VMK_OK;
   FS_FileAttributes fa;
   SG_Array sgArr;
   uint32 length = PAGE_SIZE;

   ASSERT(obj != NULL);
   ASSERT(obj->type == USEROBJ_TYPE_FILE);
   ASSERT(bytesRead != NULL);
   *bytesRead = 0;

   if (obj->data.vmfsObject->handle == FS_INVALID_FILE_HANDLE) {
      return VMK_INVALID_HANDLE;
   }

   UWLOG(3, "obj=%p, mpn=%#x offset=%"FMT64"d", obj, mpn, offset);

   status = FSS_GetFileAttributes(&obj->data.vmfsObject->oid, &fa);
   if (status != VMK_OK) {
      UWLOG(0, "FSS_GetFileAttributes failed: %s",
            UWLOG_ReturnStatusToString(status));
      return status;
   }

   /* Check if reading entirely beyond end of file */
   if (offset >= fa.length) {
      UWLOG(1, "offset past end of file (offset=%"FMT64"d len=%"FMT64"d).",
            offset, fa.length);
      return VMK_OK;
   }
      
   if (fa.length - offset < length) {
      length = ALIGN_UP(fa.length - offset, DISK_SECTOR_SIZE);
   }

   UWLOG(3, "Reading %d bytes (file len=%"FMT64"d)", length, fa.length);

   sgArr.length = 1;
   sgArr.addrType = SG_MACH_ADDR;
   sgArr.sg[0].offset = offset;
   sgArr.sg[0].addr = MPN_2_MA(mpn);
   sgArr.sg[0].length = length;

   /* Do read */
   status = FSS_SGFileIO(obj->data.vmfsObject->handle, &sgArr,
                         FS_READ_OP, bytesRead);

   if (status != VMK_OK) {
      UWLOG(2, "FSS_SGFileIO on handle %"FMT64"d returned %s", 
            obj->data.vmfsObject->handle, VMK_ReturnStatusToString(status));
   }
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserFileWrite --
 *
 *      Write up to length bytes at the given offset in the given
 *	file.  Sets bytesWritten to number of bytes actually
 * 	written.  bytesWritten is undefined if an error is returned.
 *
 * Results:
 *      VMK_OK if some bytes were successfully written.  Error code
 *      otherwise.  *bytesWritten is set to number of bytes written.
 *
 * Side effects:
 *      Bytes are written to the object.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserFileWrite(UserObj* obj,
              UserVAConst userData,
              uint64 offset,
              uint32 length,
              uint32 *bytesWritten)
{
   VMK_ReturnStatus status = VMK_OK;
   uint32 postpad = 0;
   UserFile_Cache *const cache = &obj->data.vmfsObject->cache;

   ASSERT(obj != NULL);
   ASSERT(obj->type == USEROBJ_TYPE_FILE);
   ASSERT(bytesWritten != NULL);
   *bytesWritten = 0;

   if (obj->data.vmfsObject->handle == FS_INVALID_FILE_HANDLE) {
      return VMK_INVALID_HANDLE;
   }

   UWLOG(2, "obj=%p, userData@%#x, offset=%"FMT64"u, length=%u",
         obj, userData, offset, length);

   if (length == 0) {
      return VMK_OK;
   }

   status = UserFileCacheEof(obj);
   if (status != VMK_OK) {
      return status;
   }
   ASSERT(cache->eofValid);

   /*
    * If we're in append mode, set the offset to the end of the file.
    */
   if (obj->openFlags & USEROBJ_OPEN_APPEND) {
      UWLOG(3, "appending");
      offset = cache->eof;
   }
      
   if (cache->buffer == NULL) {
      /* Allocate buffer */
      cache->buffer = User_HeapAlloc(MY_USER_CARTEL_INFO, BUFFER_SIZE);
      if (cache->buffer == NULL) {
         return VMK_NO_RESOURCES;
      }
   }

   while (length) {
      uint32 chunkLength, padRead, usable, prepad;

      /* Write initial portion into existing cache if possible */
      if (cache->valid &&
          cache->offset <= offset && offset < cache->offset + cache->length) {
         // usable = amount of user data to write into cache this iteration
         usable = MIN(length, cache->length - (offset - cache->offset));
         UWLOG(6, "write cache hit, %u bytes at offset %Lu", usable, offset);
         cache->dirty = TRUE;
         status = User_CopyIn(cache->buffer + (offset - cache->offset),
                              userData, usable);
         if (status != VMK_OK) {
            UWLOG(1, "User_CopyIn(0x%x, %p, %d) returned %s",
                  userData, cache->buffer + (offset - cache->offset), usable,
                  VMK_ReturnStatusToString(status));
            break;
         }

      } else {
         /* Compute sizes */
         // prepad = distance from preceding sector boundary to user data
         prepad = offset & (DISK_SECTOR_SIZE - 1);
         chunkLength = MIN(length + prepad, BUFFER_SIZE);
         // usable = amount of user data used in this iteration
         usable = MIN(length, chunkLength - prepad);
         UWLOG(6, "write cache miss, %u bytes at offset %Lu", usable, offset);
         // postpad = distance from end of user data to next sector boundary
         // The computation below gives us the 2's complement of the low
         // order bits of the end address, which is what we need.
         postpad = (-chunkLength) & (DISK_SECTOR_SIZE - 1);
         // chunkLength = amount to write in this iteration, including padding
         chunkLength += postpad;

         /* Flush old buffer contents */
         status = obj->methods->fsync(obj, TRUE);
         if (status != VMK_OK) {
            break;
         }
         ASSERT(!cache->dirty);
         cache->valid = FALSE;

         /* Initialize prepad with old data, if any */
         if (prepad) {
            status = FSS_BufferIO(obj->data.vmfsObject->handle,
                                  offset - prepad,
                                  (uint64)(unsigned long)cache->buffer,
                                  DISK_SECTOR_SIZE, FS_READ_OP, SG_VIRT_ADDR,
                                  &padRead);
            if (status == VMK_LIMIT_EXCEEDED) {
               memset(cache->buffer, 0, DISK_SECTOR_SIZE);
            } else if (status != VMK_OK) {
               UWLOG(1, "FSS_BufferIO on %"FMT64"d returned %s",
                     obj->data.vmfsObject->handle,
                     VMK_ReturnStatusToString(status));
               break;
            } else {
               ASSERT(padRead == DISK_SECTOR_SIZE);
            }
         }

         /* Initialize postpad with old data, if any */
         if (postpad && (!prepad || chunkLength > DISK_SECTOR_SIZE)) {
            ASSERT(length == usable); // postpad is used only on last iteration
            status = FSS_BufferIO(obj->data.vmfsObject->handle,
                                  offset + usable + postpad - DISK_SECTOR_SIZE,
                                  (uint64)(unsigned long)(cache->buffer+chunkLength - DISK_SECTOR_SIZE),
                                  DISK_SECTOR_SIZE, FS_READ_OP, SG_VIRT_ADDR,
                                  &padRead);
            if (status == VMK_LIMIT_EXCEEDED) {
               memset(cache->buffer + chunkLength - DISK_SECTOR_SIZE, 0,
                      DISK_SECTOR_SIZE);
            } else if (status != VMK_OK) {
               UWLOG(1, "FSS_BufferIO on %"FMT64"d returned %s",
                     obj->data.vmfsObject->handle, 
                     VMK_ReturnStatusToString(status));
               break;
            } else {
               ASSERT(padRead == DISK_SECTOR_SIZE);
            }
         }

         /* Copy in user data */
         status = User_CopyIn(cache->buffer + prepad, userData, usable);
         if (status != VMK_OK) {
            UWLOG(1, "User_CopyIn(%p, 0x%x, %d) returned %s",
                  cache->buffer + prepad, userData, chunkLength,
                  VMK_ReturnStatusToString(status));
            break;
         }

         cache->valid = TRUE;
         cache->dirty = TRUE;
         cache->offset = offset - prepad;
         cache->length = prepad + usable + postpad;
      }

      length -= usable;
      offset += usable;
      userData += usable;
      *bytesWritten += usable;

      /* Update EOF byte pointer if needed. */
      ASSERT(cache->eofValid);
      if (status == VMK_OK && offset > cache->eof) {
         cache->eof = offset;
         cache->eofDirty = TRUE;
      }
   }

   if (obj->openFlags & USEROBJ_OPEN_SYNC) {
      status = obj->methods->fsync(obj, TRUE);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserFileWriteMPN --
 *
 *      Write PAGE_SIZE bytes at the current offset in the given
 *      UserObj.  Sets bytesWritten to number of bytes actually written.
 *      BytesWritten is undefined if an error is returned.
 *
 * Results:
 *      VMK_OK if some bytes were successfully written.  Error code
 *      otherwise.  *bytesWritten is set to number of bytes written.
 *
 * Side effects:
 *      Bytes are written to the object.
 *
 * Bugs:
 *      Ignores the cache that may be associated with the file.
 *      See PR 44754.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserFileWriteMPN(UserObj* obj,
                MPN mpn,
		uint64 offset,
                uint32 *bytesWritten)
{
   VMK_ReturnStatus status = VMK_OK;
   SG_Array sgArr;
   uint32 length = PAGE_SIZE;

   ASSERT(obj != NULL);
   ASSERT(obj->type == USEROBJ_TYPE_FILE);
   ASSERT(bytesWritten != NULL);
   *bytesWritten = 0;

   if (obj->data.vmfsObject->handle == FS_INVALID_FILE_HANDLE) {
      return VMK_INVALID_HANDLE;
   }

   sgArr.length = 1;
   sgArr.addrType = SG_MACH_ADDR;
   sgArr.sg[0].offset = offset;
   sgArr.sg[0].addr = MPN_2_MA(mpn);
   sgArr.sg[0].length = length;

   /* Do write */
   status = FSS_SGFileIO(obj->data.vmfsObject->handle,
                         &sgArr, FS_WRITE_OP, bytesWritten);

   if (status != VMK_OK) {
      UWLOG(2, "FSS_SGFileIO returned %s", VMK_ReturnStatusToString(status));
   }
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserFileStat --
 *
 *      Stat a file in a VMFS.
 *
 * Results:
 *      VMK_OK or error condition.  Data returned in statbuf.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserFileStat(UserObj* obj, LinuxStat64* statbuf)
{
   VMK_ReturnStatus status = VMK_OK;
   FS_FileAttributes fa;
   UserFile_Cache *const cache = &obj->data.vmfsObject->cache;

   UWLOG(3, FS_OID_FMTSTR, FS_OID_VAARGS(&obj->data.vmfsObject->oid));
   memset(statbuf, 0, sizeof(*statbuf));

   ASSERT(obj->type == USEROBJ_TYPE_FILE);
   status = FSS_GetFileAttributes(&obj->data.vmfsObject->oid, &fa);
   if (status != VMK_OK) {
      UWLOG(0, "GetFileAttributes failed: %#x", status);
      return status;
   }

   UWLOG(2, "OID "FS_OID_FMTSTR", len %Lu, dskBS %u, fsBS %u, "
         "flg %#x, gen %u, descN %d, mtm %u, ctm %u, atm %u, "
         "uid %u, gid %u, mode %u, tVer %d, vhwVer %u",
         FS_OID_VAARGS(&obj->data.vmfsObject->oid),
         fa.length, fa.diskBlockSize, 
         fa.fsBlockSize, fa.flags, fa.generation, fa.descNum,
         fa.mtime, fa.ctime, fa.atime,
         fa.uid, fa.gid, fa.mode, fa.toolsVersion, fa.virtualHWVersion);

   if (!cache->eofValid) {
      UWLOG(2, "updating cached eof to %Ld", fa.length);
      //XXX this is wrong.  We need to completely hide the disk tail from userFile.c: bug 48557
      if (fa.flags & FS_NOT_ESX_DISK_IMAGE) {
         cache->eof = fa.length;
      } else {
         cache->eof = fa.length + FS_DISK_TAIL_SIZE;
         //UWWarn("Mucking about with disk tail.  Fix me! bug 48557");
      }
      cache->eofValid = TRUE;
      cache->eofDirty = FALSE;
   }

   /*
    * The following (mostly?) matches what the COS interface to /vmfs
    * fills in here; see bora/modules/vmnix/vmkfs.c.
    */
   statbuf->st_dev = VMFS_STAT_DEV;
   statbuf->st_ino32 = fa.descNum; //XXX: VMFS will not provide i_nos
   				   // that are unique across all volumes
   statbuf->st_mode = USERFILE_MODE(fa.flags, fa.mode);
   statbuf->st_nlink = 1;
   statbuf->st_uid = fa.uid;
   statbuf->st_gid = fa.gid;
   statbuf->st_rdev = 0;
   statbuf->st_size = cache->eof;
   statbuf->st_blksize = VMFS_DEFAULT_BLOCKSIZE;
   statbuf->st_blocks =
      (fa.length + fa.fsBlockSize - 1) / fa.fsBlockSize
      * (fa.fsBlockSize / DISK_SECTOR_SIZE);
   statbuf->st_atime = fa.atime;
   statbuf->st_mtime = fa.mtime;
   statbuf->st_ctime = fa.ctime;

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserFileChmod --
 *
 *      Change access control mode bits of obj.
 *
 * Results:
 *      VMK_OK or error.
 *
 * Side effects:
 *      Modifies attributes.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserFileChmod(UserObj* obj, LinuxMode mode)
{
   VMK_ReturnStatus status;
   FS_FileAttributes fa;

   status = FSS_GetFileAttributes(&obj->data.vmfsObject->oid, &fa);
   if (status != VMK_OK) {
      return status;
   }

   status = UserIdent_CheckAccess(&MY_RUNNING_WORLD->ident,
                                  USEROBJ_OPEN_OWNER, fa.uid, fa.gid, fa.mode);
   if (status != VMK_OK) {
      return status;
   }

   fa.mode = mode;

   return FSS_SetFileAttributes(&obj->data.vmfsObject->oid,
                                FILEATTR_SET_PERMISSIONS, &fa);
}


/*
 *----------------------------------------------------------------------
 *
 * UserFileChown --
 *
 *      Change owner and/or group of obj.  -1 => no change.
 *
 * Results:
 *      VMK_OK or error.
 *
 * Side effects:
 *      Modifies attributes.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserFileChown(UserObj* obj, Identity_UserID owner, Identity_GroupID group)
{
   VMK_ReturnStatus status;
   FS_FileAttributes fa;

   status = FSS_GetFileAttributes(&obj->data.vmfsObject->oid, &fa);
   if (status != VMK_OK) {
      return status;
   }

   if (owner != (uint32) -1) {
      status = UserIdent_CheckAccess(&MY_RUNNING_WORLD->ident,
                                     USEROBJ_OPEN_OWNER,
                                     fa.uid, fa.gid, fa.mode);
      if (status != VMK_OK) {
         return status;
      }
      fa.uid = owner;
   }

   if (group != (uint32) -1) {
      status = UserIdent_CheckAccess(&MY_RUNNING_WORLD->ident,
                                     USEROBJ_OPEN_GROUP,
                                     fa.uid, group, fa.mode);
      if (status != VMK_OK) {
         return status;
      }
      fa.gid = group;
   }

   return FSS_SetFileAttributes(&obj->data.vmfsObject->oid,
                                FILEATTR_SET_PERMISSIONS, &fa);
}


/*
 *----------------------------------------------------------------------
 *
 * UserFileTruncate --
 *
 *      Change size of obj.  Caller is assumed to have checked that
 *      obj is open for write. The change will make it to disk at the
 *      next fsync. XXX: are we ready to wait that long?
 *
 * Results:
 *      VMK_OK or error.
 *
 * Side effects:
 *      Modifies attributes.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserFileTruncate(UserObj* obj, uint64 size)
{
   UserFile_Cache *const cache = &obj->data.vmfsObject->cache;

   UWLOG(2, "changing cached eof to %Ld", size);
   cache->eof = size;
   cache->eofValid = TRUE;
   cache->eofDirty = TRUE;
   if (cache->valid && cache->offset + cache->length > cache->eof) {
      if (cache->offset < cache->eof) {
         cache->length = cache->eof - cache->offset;
      } else {
         cache->valid = cache->dirty = FALSE;
      }
   }
         
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserFileUnlink --
 *
 *      Relative to the specified VMFS, check that the specified arc
 *      is bound to a file, and if so, unlink it.
 *
 * Results:
 *      VMK_OK or error condition.
 *
 * Side effects:
 *      As above.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserFileUnlink(UserObj* parent, const char* arc)
{
   VMK_ReturnStatus status;
   FS_FileAttributes fa;
   FSS_ObjectID foid;

   UWLOG(1, "(arc=%s)", arc);

   if (strcmp(arc, ".") == 0 || strcmp(arc, "") == 0 ||
       strcmp(arc, "..") == 0) {
      return VMK_NO_ACCESS;
   }

   // VMFS directories always have the sticky bit set and are always
   // owned by root, so you have to be the owner of a file or root to
   // unlink it.
   status = FSS_Lookup(&parent->data.vmfsObject->oid, arc, &foid);
   if (status != VMK_OK) {
      return status;
   }

   status = FSS_GetFileAttributes(&foid, &fa);
   status = UserIdent_CheckAccess(&MY_RUNNING_WORLD->ident,
                                  USEROBJ_OPEN_OWNER, fa.uid, fa.gid, fa.mode);
   if (status != VMK_OK) {
      return status;
   }
   return FSS_RemoveFile(&parent->data.vmfsObject->oid, arc);
}

/*
 *----------------------------------------------------------------------
 *
 * UserFileRename --
 *
 *      Rename for the VMFS.
 *
 * Results:
 *      VMK_OK or error condition.
 *
 * Side effects:
 *      Renames a file.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserFileRename(UserObj* newDir, const char *newArc,
               UserObj* oldDir, const char *oldArc)
{
   VMK_ReturnStatus status;
   FS_FileAttributes fa;
   FSS_ObjectID oldFoid;

   if (oldDir->type != USEROBJ_TYPE_FILE ||
       newDir->type != USEROBJ_TYPE_FILE ||
       !FSS_IsValidOID(&oldDir->data.vmfsObject->oid)) {
      return VMK_CROSS_DEVICE_LINK;
   }

   /*
    * Renaming something to the same name it already had is a no-op.
    * Need to check for this so that we don't delete the object by
    * mistake.
    */
   if (FSS_OIDIsEqual(&oldDir->data.vmfsObject->oid,
                      &newDir->data.vmfsObject->oid) == 0 &&
       strcmp(newArc, oldArc) == 0) {
      return VMK_OK;
   }

   /*
    * VMFS directories always have the sticky bit set and are always
    * owned by root, so you have to be the owner of a file or root to
    * rename it.
    */
   status = FSS_Lookup(&oldDir->data.vmfsObject->oid, oldArc,
                       &oldFoid);
   if (status != VMK_OK) {
      return status;
   }

   status = FSS_GetFileAttributes(&oldFoid, &fa);
   if (status != VMK_OK) {
      goto close;
   }
   status = UserIdent_CheckAccess(&MY_RUNNING_WORLD->ident,
                                  USEROBJ_OPEN_OWNER, fa.uid, fa.gid, fa.mode);
   if (status != VMK_OK) {
      goto close;
   }

   /*
    * Delete existing file named (newDir, newArc) if it exists.
    *
    * XXX Unfortunately, at this point I don't know if we are 100%
    * sure FSS_RenameFile can't fail.  We have eliminated the obvious
    * error possibilites: (1) The file to be renamed exists, because
    * we've opened it already. (2) We have permission to rename that
    * file.  (3) If newArc isn't a legal name (i.e., too long), there
    * can't be an existing file by that name, so trying to remove it
    * is harmless.
    *
    * Also, to be Posixly correct, the new object has to replace the
    * old one atomically -- that is, if the new name was previously
    * bound, there must not be a window where it is neither bound to
    * the old nor the new name.
    *
    * To deal with these issues, it would be better if FSS_RenameFile
    * would handle the atomic replacement of the old file for us.
    *
    * --mann
    */
   status = UserFileUnlink(newDir, newArc);
   if (status != VMK_OK && status != VMK_NOT_FOUND) {
      goto close;
   }

   /* Do the rename. */
   status = FSS_RenameFile(&oldDir->data.vmfsObject->oid, oldArc,
                           &newDir->data.vmfsObject->oid, newArc);

 close:
   return status;
}

/*
 *----------------------------------------------------------------------
 *
 * UserFileFcntl --
 *
 *	Performs miscellaneous operations of the given fd.
 *
 * Results:
 *      VMK_OK or error condition.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserFileFcntl(UserObj* obj, uint32 cmd, uint32 arg)
{
   if (cmd == LINUX_FCNTL_CMD_SETFL) {
      /*
       * Changing either append or non-block are both nops.
       */
      if (LINUX_FCNTL_BIT_CHANGED(obj->openFlags, arg, USEROBJ_OPEN_APPEND) ||
          LINUX_FCNTL_BIT_CHANGED(obj->openFlags, arg, USEROBJ_OPEN_NONBLOCK)) {
	 return VMK_OK;
      } else {
	 return UserObj_NotImplemented(obj);
      }
   } else {
      return UserObj_NotImplemented(obj);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * UserFileFsync --
 *
 *	Force buffered writes on obj to disk.
 *
 * Results:
 *      VMK_OK or error condition.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserFileFsync(UserObj* obj, Bool dataOnly)
{
   UserFile_Cache *const cache = &obj->data.vmfsObject->cache;
   uint64 offset;
   void *buffer;
   uint32 length, written;
   VMK_ReturnStatus status = VMK_OK;

   UWLOG(2, "obj=%p %s", obj, dataOnly?"(data only)":"(meta + data)");

   if (cache->valid && cache->dirty) {
      offset = cache->offset;
      buffer = cache->buffer;
      length = cache->length;
   
      while (length) {
         UWLOG(2, "writing %u bytes at offset %Lu", length, offset);
         ASSERT((length & (DISK_SECTOR_SIZE - 1)) == 0 &&
                (offset & (DISK_SECTOR_SIZE - 1)) == 0);
         status = FSS_BufferIO(obj->data.vmfsObject->handle, offset,
                               (uint64)(unsigned long)buffer, length,
                               FS_WRITE_OP, SG_VIRT_ADDR, &written);
         if (status != VMK_OK) {
            UWLOG(0, "FSS_BufferIO returned %s",
                  VMK_ReturnStatusToString(status));
            cache->valid = FALSE;
            cache->dirty = FALSE;
            cache->eofValid = FALSE;
            cache->eofDirty = FALSE;
            break;
         }
         offset += written;
         buffer += written;
         length -= written;
      }
      cache->dirty = FALSE;
   }

   if (status == VMK_OK && cache->eofValid && cache->eofDirty) {
      FS_FileAttributes fa;

      fa.length = cache->eof;
      UWLOG(2, "updating on-disk eof to %Lu", cache->eof);
      status = FSS_SetFileAttributes(&obj->data.vmfsObject->oid,
                                     FILEATTR_SET_LENGTH, &fa);
      if (status != VMK_OK) {
         UWLOG(0, "FSS_SetFileAttributes returned %s",
               VMK_ReturnStatusToString(status));
         cache->valid = FALSE;
         cache->dirty = FALSE;
         cache->eofValid = FALSE;
      }
      cache->eofDirty = FALSE;
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserFileIoctl --
 *
 *	Universal escape for type-specific operations -- ugh
 *
 * Results:
 *      VMK_OK or error condition.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserFileIoctl(UserObj* obj, uint32 cmd, LinuxIoctlArgType type,
              uint32 size, void *userData, uint32 *result)
{
   if (type != LINUX_IOCTL_ARG_PTR) {
      UWWarn("Invalid args: cmd = %d, type = %d, size = %d", 
             LINUX_IOCTL_CMD(cmd), type, size);
      return VMK_NOT_SUPPORTED;
   }

   switch(LINUX_IOCTL_CMD(cmd)) {
   case IOCTLCMD_VMFS_GET_FILE_HANDLE:
   {
      VMK_ReturnStatus status;
      FS_FileAttributes fa;

      if (size != sizeof(obj->data.vmfsObject->handle)) {
         return VMK_BAD_PARAM;
      }
      status = FSS_GetFileAttributes(&obj->data.vmfsObject->oid, &fa);
      if (status != VMK_OK) {
         return status;
      }
      
      if (fa.flags & FS_NOT_ESX_DISK_IMAGE) {
         UWWarn("GET_FILE_HANDLE ioctl no non disk");
         return VMK_BAD_PARAM;
      }
      
      status = User_CopyOut((VA)userData, &obj->data.vmfsObject->handle,
                            sizeof obj->data.vmfsObject->handle);
      return status;

   }
   case IOCTLCMD_VMFS_GET_FREE_SPACE:
   {
      VMK_ReturnStatus status;
      VMnix_PartitionListResult *result;
      const int maxPartitions = VMNIX_PLIST_DEF_MAX_PARTITIONS;
      uint64 bytesFree;

      if (size != sizeof(bytesFree)) {
         return VMK_BAD_PARAM;
      }

      result = (VMnix_PartitionListResult *)
         User_HeapAlloc(MY_USER_CARTEL_INFO, 
                        VMNIX_PARTITION_ARR_SIZE(maxPartitions));
      if (result == NULL) {
         return VMK_NO_MEMORY;
      }

      status = FSS_GetAttributes(&obj->data.vmfsObject->oid,
                                 maxPartitions, result);
      if (status == VMK_OK) {
         bytesFree = (((uint64)(result->numFileBlocks - 
                                result->numFileBlocksUsed)) *
                      result->fileBlockSize);
         status = User_CopyOut((VA)userData, &bytesFree, sizeof(bytesFree));
      }
      User_HeapFree(MY_USER_CARTEL_INFO, result);
      return status;
   }
   default:
      UWWarn("Invalid args: cmd = %d, type = %d, size = %d", 
             LINUX_IOCTL_CMD(cmd), type, size);
      return VMK_NOT_SUPPORTED;
   }
   NOT_REACHED();
}


/*
 *----------------------------------------------------------------------
 *
 * UserFile_Sync --
 *
 *	Force buffered writes on all files to disk.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
void
UserFile_Sync(User_CartelInfo* uci)
{
   int i;
   VMK_ReturnStatus status;
   UserObj* obj;

   /*
    * Strategy: loop through all open fds and call fsync on those that
    * are VMFS files.  This is the most efficient way to do it with
    * our simple per-open-file cache.
    */
   for (i = 0; i < USEROBJ_MAX_HANDLES; i++) {
      status = UserObj_Find(uci, i, &obj);
      if (status == VMK_OK) {
         if (obj->type == USEROBJ_TYPE_FILE) {
            Semaphore_Lock(&obj->sema);
            (void) obj->methods->fsync(obj, FALSE);
            Semaphore_Unlock(&obj->sema);
         }
         (void) UserObj_Release(uci, obj);
      }
   }
}

static VMK_ReturnStatus
UserFileReaddir (UserObj* obj,
                 UserVA /* LinuxDirent64* */ userData,
                 uint32 length, uint32* bytesRead)
{
   UWWarn("Not implemented. Yet.");
   return VMK_NOT_IMPLEMENTED;
}

static VMK_ReturnStatus 
UserFileMkdir(UserObj* obj, const char *arc,
              LinuxMode mode)
{
   UWWarn("Not implemented. Yet.");
   return VMK_NOT_IMPLEMENTED;
}

static VMK_ReturnStatus 
UserFileRmdir(UserObj* obj, const char *arc)
{
   UWWarn("Not implemented. Yet.");
   return VMK_NOT_IMPLEMENTED;
}

/*
 *----------------------------------------------------------------------
 *
 * UserFileGetName --
 *
 *      Get the name of a VMFS relative to /vmfs.
 *
 * Results:
 *      VMK_OK.  Data returned in arc.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserFileGetName(UserObj* obj, char* arc, uint32 length)
{
   VMK_ReturnStatus status;
   FS_FileAttributes attrs;
   
   if (length < sizeof(attrs.fileName)) {
      ASSERT(0);
      return VMK_NAME_TOO_LONG;
   }

   status = FSS_GetFileAttributes(&obj->data.vmfsObject->oid, &attrs);
   if (status != VMK_OK) {
      UWLOG(0, "GetFileAttributes failed on "FS_OID_FMTSTR,
            FS_OID_VAARGS(&obj->data.vmfsObject->oid));
      return status;
   }

   if (strlen(attrs.fileName) >= sizeof(attrs.fileName)) {
      ASSERT(0);
      return VMK_NAME_TOO_LONG;
   }
   strncpy(arc, attrs.fileName, sizeof(attrs.fileName));
   UWLOG(2, "%s for "FS_OID_FMTSTR, 
         arc, FS_OID_VAARGS(&obj->data.vmfsObject->oid));
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * UserFileToString --
 *
 *	Returns a string representation of this object, namely the oid
 *	and same cache info.
 *
 * Results:
 *      VMK_OK.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserFileToString(UserObj *obj, char *string, int length)
{
   FSS_ObjectID *fssoid = &obj->data.vmfsObject->oid;
   UserFile_Cache *cache = &obj->data.vmfsObject->cache;
   int len;
   int i;

   len = snprintf(string, length, "fstype: %d oid: ", fssoid->fsTypeNum);

   for (i = 0; len < length && i < (fssoid->oid.length / sizeof(uint32)); i++) {
      len += snprintf(string + len, length - len, "%x",
		      ((uint32*)fssoid->oid.data)[i]);
   }

   if (len < length) {
      len += snprintf(string + len, length - len,
		      " cache: %s,%s off: %Lx len: %Lx eof: %Lx",
		      cache->valid ? "VLD" : "!VLD",
		      cache->dirty ? "DRT" : "!DRT", cache->offset,
		      cache->offset, cache->eof);
   }

   if (len >= length) {
      UWLOG(1, "Description string too long (%d vs %d).  Truncating.", len,
	    length);
   }
      
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * UserFileStatFS --
 *
 *      StatFS for the VMFS.
 *
 * Results:
 *      VMK_OK or error condition.  Data returned in statbuf.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserFileStatFS(UserObj* obj, LinuxStatFS64* statbuf)
{
   memset(statbuf, 0, sizeof(*statbuf));
   /*
    * The following matches what the COS interface to /vmfs
    * fills in here; see bora/modules/vmnix/vmkfs.c.
    */
   statbuf->f_type = FS_MAGIC_NUMBER;
   statbuf->f_bsize = VMFS_DEFAULT_BLOCKSIZE;
   statbuf->f_fsid.val[0] = (FS_MAGIC_NUMBER >> 16) & 0xffff;
   statbuf->f_fsid.val[1] =  FS_MAGIC_NUMBER        & 0xffff;
   statbuf->f_namelen = FS_MAX_FILE_NAME_LENGTH;
   statbuf->f_blocks  = 1000000;
   statbuf->f_bfree   = 1000000;
   statbuf->f_bavail  = 1000000;
   statbuf->f_files   = 0;
   statbuf->f_ffree   = 0;

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * UserFileCacheEof --
 *
 *      Make sure we have the eof offset for obj cached.
 *
 * Results:
 *	VMK_OK or error.
 *
 * Side effects:
 *      Updates cache.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserFileCacheEof(UserObj *obj)
{   
   UserFile_Cache *const cache = &obj->data.vmfsObject->cache;
   VMK_ReturnStatus status;

   if (!cache->eofValid) {
      FS_FileAttributes fa;

      status = FSS_GetFileAttributes(&obj->data.vmfsObject->oid, &fa);
      if (status != VMK_OK) {
         return status;
      }
      //XXX this is wrong.  We need to completely hide the disk tail from userFile.c: bug 48557
      if (fa.flags & FS_NOT_ESX_DISK_IMAGE) {
         cache->eof = fa.length;
      } else {
         cache->eof = fa.length + FS_DISK_TAIL_SIZE;
         //UWWarn("Mucking about with disk tail.  Fix me! bug 48557");
      }
      UWLOG(2, "updating cached eof to %Ld", cache->eof);
      cache->eofValid = TRUE;
      cache->eofDirty = FALSE;
   }

   return VMK_OK;
} 
     

/*
 *----------------------------------------------------------------------
 *
 * UserFile_CartelInit --
 *
 *      Per-cartel initialization.
 *
 * Results:
 *	None
 *
 * Side effects:
 *      Start uci->fileTimer.
 *
 *----------------------------------------------------------------------
 */
void
UserFile_CartelInit(User_CartelInfo* uci)
{
   uci->fdState.fileTimer =
      Timer_Add(MY_PCPU, UserFileTimerCallback,
                60000 /* 60 sec */, TIMER_PERIODIC, (void *) uci->cartelID);
}


/*
 *----------------------------------------------------------------------
 *
 * UserFile_CartelCleanup --
 *
 *      Per-cartel cleanup
 *
 * Results:
 *	None
 *
 * Side effects:
 *      Sync all open files.
 *      Stop uci->fileTimer.
 *
 *----------------------------------------------------------------------
 */
void
UserFile_CartelCleanup(User_CartelInfo* uci)
{
   UserFile_Sync(uci);
   Timer_RemoveSync(uci->fdState.fileTimer);
}


/*
 *-----------------------------------------------------------------------------
 *
 * UserFileTimerHelper --
 *
 *      Helper world callback to flush dirty cached data to disk.
 *
 * Results:
 *      VMK_OK or error.
 *
 * Side effects:
 *      As above.
 *
 *-----------------------------------------------------------------------------
 */
static void
UserFileTimerHelper(void *data)
{
   World_Handle *world;

   world = World_Find((World_ID) data);
   if (world != NULL) {
      if (world->userCartelInfo != NULL) {
         UserFile_Sync(world->userCartelInfo);
      }
      World_Release(world);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * UserFileTimerCallback --
 *
 *      Timer callback to flush dirty cached data to disk.
 *
 * Results:
 *      VMK_OK or error.
 *
 * Side effects:
 *      As above.
 *
 *-----------------------------------------------------------------------------
 */
static void
UserFileTimerCallback(void *data, UNUSED_PARAM(Timer_AbsCycles timestamp))
{
   Helper_Request(HELPER_MISC_QUEUE, UserFileTimerHelper, data);
}

/*
 *----------------------------------------------------------------------
 *
 * UserFile_OpenVMFSRoot --
 *
 *      Open the "/vmfs" directory.
 *
 * Results:
 *      UserObj for /vmfs.
 *
 *----------------------------------------------------------------------
 */
UserObj*
UserFile_OpenVMFSRoot(User_CartelInfo *uci, uint32 openFlags)
{
   FSS_ObjectID rootOID;
   
   FSS_MakeVMFSRootOID(&rootOID);
   
   return UserFileCreateObj(uci, &rootOID, FS_INVALID_FILE_HANDLE,
                            openFlags);
}
