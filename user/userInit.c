/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * userInit.c --
 *
 *	Manage the creation and initialization of a new Cartel.
 *
 *	Note: many of the prototypes for these functions are in
 *	vmkernel/private/user.h, because they are called from the
 *	host.
 */

#include "vm_types.h"
#include "user_int.h"

#define LOGLEVEL_MODULE UserInit
#include "userLog.h"
   

/*
 *----------------------------------------------------------------------
 *
 * UserInit_CartelInit --
 *
 *	Setup per-cartel structures for init module.
 *
 * Results:
 *      VMK_OK or VMK_NO_MEMORY
 *
 * Side effects:
 *	Allocates some memory
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserInit_CartelInit(User_CartelInfo* uci)
{
   VMK_ReturnStatus status;
   ASSERT(uci);

   uci->args.envInfo = User_HeapAlloc(uci, sizeof(User_EnvInfo));
   if (uci->args.envInfo == NULL) {
      status = VMK_NO_MEMORY;
   } else {
      memset(uci->args.envInfo, 0, sizeof(User_EnvInfo));
      status = VMK_OK;
   }
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserInit_CartelCleanup --
 *
 *	Free any allocations for initial args and other setup
 *	information.
 *
 * Results:
 *      VMK_OK
 *
 * Side effects:
 *	Free any init-time memory that might be left.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserInit_CartelCleanup(User_CartelInfo* uci)
{
   ASSERT(uci);

   if (uci->args.workingDirName != NULL) {
      User_HeapFree(uci, uci->args.workingDirName);
      uci->args.workingDirName = NULL;
   }

   if (uci->args.mapHead != NULL) {
      User_MapInfo* mapInfo;
      User_MapInfo* nextMapInfo;

      for (mapInfo = uci->args.mapHead; mapInfo != NULL; ) {
	 nextMapInfo = mapInfo->next;
	 User_HeapFree(uci, mapInfo);
	 mapInfo = nextMapInfo;
      }

      uci->args.mapHead = NULL;
   }

   if (uci->args.fileHead != NULL) {
      User_FileInfo* fileInfo;
      User_FileInfo* nextFileInfo;

      for (fileInfo = uci->args.fileHead; fileInfo != NULL; ) {
	 if (fileInfo->obj != NULL) {
	    (void) UserObj_Release(uci, fileInfo->obj);
	 }

	 nextFileInfo = fileInfo->next;
	 User_HeapFree(uci, fileInfo);
	 fileInfo = nextFileInfo;
      }

      uci->args.fileHead = NULL;
   }

   if (uci->args.head != NULL) {
      User_Arg* arg;
      User_Arg* next;

      for (arg = uci->args.head; arg != NULL; ) {
	 next = arg->next;

	 User_HeapFree(uci, arg->arg);
	 User_HeapFree(uci, arg);

	 arg = next;
      }

      uci->args.head = NULL;
   }

   if (uci->args.envInfo != NULL) {
      while (uci->args.envInfo->numVars > 0) {
	 uci->args.envInfo->numVars--;
	 ASSERT(uci->args.envInfo->environ[uci->args.envInfo->numVars]);
	 User_HeapFree(uci,
		       uci->args.envInfo->environ[uci->args.envInfo->numVars]);
      }

      if (uci->args.envInfo->environ != NULL) {
         User_HeapFree(uci, uci->args.envInfo->environ);
      }

      User_HeapFree(uci, uci->args.envInfo);
      uci->args.envInfo = NULL;
   }

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserInitIsNewUserWorld --
 *	
 *      Test if the given world is a "new" UserWorld for a new cartel.
 *      
 * Results:
 *	An appropriate return code if world is invalid.
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
static INLINE VMK_ReturnStatus
UserInitIsNewUserWorld(World_Handle* world)
{
   /*
    * This check should already have been done by the host.c code.
    */
   ASSERT(World_IsUSERWorld(world));

   if (World_CpuSchedRunState(world) != CPUSCHED_NEW) {
      UWLOGFor(0, world, "World is already running.");
      return VMK_BUSY;
   }

   if (world->userCartelInfo->cartelID != world->worldID) {
      UWLOGFor(0, world, "World is new, but its not the cartel leader.");
      return VMK_BUSY;
   }

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserInit_AddArg --
 *	
 *      Add the given char arg to the initial arguments for a new
 *      cartel.  Caller ensures null termination.  Caller owns str.
 *      
 * Results:
 *	VMK_OK if world is appropriate and there are sufficient
 *	resources to store the arg for later.  Error otherwise.
 *
 * Side effects:
 *	Arg stored for later
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserInit_AddArg(World_Handle* world, const char* str)
{
   VMK_ReturnStatus status;
   User_CartelInfo* uci = world->userCartelInfo;
   User_Arg* arg;
   
   ASSERT(world != MY_RUNNING_WORLD);
   status = UserInitIsNewUserWorld(world);
   if (status != VMK_OK) {
      return status;
   }

   arg = (User_Arg *)User_HeapAlloc(uci, sizeof(User_Arg));
   if (arg == NULL) {
      return VMK_NO_MEMORY;
   }

   arg->length = strlen(str) + 1;
   arg->arg = User_HeapAlloc(uci, arg->length);
   arg->next = NULL;
   
   if (arg->arg == NULL) {
      User_HeapFree(uci, arg);
      return VMK_NO_MEMORY;
   }

   strncpy(arg->arg, str, arg->length);
   UWLOGFor(1, world, "%s", arg->arg);
         
   if (uci->args.head == NULL) {
      uci->args.head = uci->args.tail = arg;
   } else {
      uci->args.tail->next = arg;
      uci->args.tail = arg;
   }
   uci->args.num++;

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserInit_SetBreak --
 *	
 *      Initialize the break (start of heap) for a new world.
 *      
 * Results:
 *	VMK_OK if world is okay and break is set correctly
 *
 * Side effects:
 *	Break of new world is modified
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserInit_SetBreak(World_Handle* world, UserVA brk)
{
   VMK_ReturnStatus status;

   ASSERT(world != MY_RUNNING_WORLD);
   status = UserInitIsNewUserWorld(world);
   if (status != VMK_OK) {
      return status;
   }
      
   UWLOGFor(1, world, "%#x", brk);

   status = UserMem_SetDataStart(world, &brk);
   if (status == VMK_OK) {
      status = UserMem_SetDataEnd(world, brk);
   }
   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserInit_SetLoaderInfo --
 *	
 *      Set the information needed by the in-kernel dynamic loader to
 *      start the new cartel.
 *      
 * Results:
 *	VMK_OK if world is a new UserWorld, error otherwise
 *
 * Side effects:
 *	Magical loader bits are saved off.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserInit_SetLoaderInfo(World_Handle* world,
                       uint32 phdr,
                       uint32 phent,
                       uint32 phnum,
                       uint32 base,
                       uint32 entry)
{
   User_InitArgs* initArgs;
   VMK_ReturnStatus status;

   ASSERT(world != MY_RUNNING_WORLD);
   status = UserInitIsNewUserWorld(world);
   if (status != VMK_OK) {
      return status;
   }
      
   UWLOGFor(1, world, "phdr=%d, phent=%d, phnum=%d, base=%#x, entry=%#x",
            phdr, phent, phnum, base, entry);

   initArgs = &world->userCartelInfo->args;
   initArgs->ldInfo.phdr = phdr;
   initArgs->ldInfo.phent = phent;
   initArgs->ldInfo.phnum = phnum;
   initArgs->ldInfo.base = base;
   initArgs->ldInfo.entry = entry;

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserInit_AddMapFile --
 *	
 *      Store the given file and fid (proxy-relative fd), for use when
 *      actually starting this cartel.  Caller ensures fname is
 *      null-terminated, and caller owns fname.
 *      
 * Results:
 *	VMK_OK if world is a new UserWorld and there are sufficient
 *	resources to store the fid/fname, and the name isn't too long.
 *
 * Side effects:
 *	fid and fname saved for later
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserInit_AddMapFile(World_Handle* world,
                    int fid,
                    const char* fname)
{
   User_CartelInfo* uci;
   User_FileInfo* fileInfo;
   User_InitArgs* initArgs;
   VMK_ReturnStatus status;
   int rc;

   ASSERT(world != MY_RUNNING_WORLD);
   status = UserInitIsNewUserWorld(world);
   if (status != VMK_OK) {
      return status;
   }
      
   UWLOGFor(1, world, "fid=%d, fname=%s", fid, fname);

   uci = world->userCartelInfo;
   fileInfo = User_HeapAlloc(uci, sizeof(*fileInfo));
   if (fileInfo == NULL) {
      return VMK_NO_MEMORY;
   }

   fileInfo->id = fid;
   fileInfo->obj = NULL;
   rc = snprintf(fileInfo->name, sizeof fileInfo->name, "%s", fname);

   if (rc >= sizeof fileInfo->name) {
      User_HeapFree(uci, fileInfo);
      return VMK_NAME_TOO_LONG;
   }
      
   initArgs = &uci->args;
   fileInfo->next = initArgs->fileHead;
   initArgs->fileHead = fileInfo;

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserInit_AddMapSection --
 *	
 *      Add information for mmap'ing a section of a file in the new
 *      cartel.
 *      
 * Results:
 *	VMK_OK if given world is a new UserWorld cartel, and the given
 *	mmap data is sane.
 *
 * Side effects:
 *	Data is stored for later.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserInit_AddMapSection(World_Handle* world,
                       VA addr,
                       uint32 length,
                       uint32 prot,
                       int flags,
                       int id,
                       uint64 offset,
                       uint32 zeroAddr)
{
   User_CartelInfo* uci;
   User_InitArgs* initArgs;
   User_MapInfo* mapInfo;
   VMK_ReturnStatus status;

   ASSERT(world != MY_RUNNING_WORLD);
   status = UserInitIsNewUserWorld(world);
   if (status != VMK_OK) {
      return status;
   }
      
   UWLOGFor(1, world,
            "addr=%#x, len=%d, prot=%#x, flags=%#x, id=%d, offset=%"FMT64"x",
            addr, length, prot, flags, id, offset);

   uci = world->userCartelInfo;
   initArgs = &uci->args;
   mapInfo = User_HeapAlloc(uci, sizeof(*mapInfo));
   if (mapInfo == NULL) {
      status = VMK_NO_MEMORY;
   } else if (addr == 0) {
      UWLOGFor(0, world, "Bad address (zero).");
      status = VMK_BAD_PARAM;
   } else if (PAGE_OFFSET(addr) != 0) {
      UWLOGFor(0, world, "Bad address (not page aligned).");
      status = VMK_BAD_PARAM;
   } else if (length == 0) {
      UWLOGFor(0, world, "Bad length (zero).");
      status = VMK_BAD_PARAM;
   } else if (!(flags & LINUX_MMAP_FIXED)) {
      UWLOGFor(0, world, "MMAP_FIXED not specified.");
      status = VMK_BAD_PARAM;
   } else if (id < 0) {
      UWLOGFor(0, world, "Bad cos fd.");
      status = VMK_BAD_PARAM;
   } else {
      mapInfo->next = NULL;
      mapInfo->addr = addr;
      mapInfo->length = length;
      mapInfo->prot = prot;
      mapInfo->flags = flags;
      mapInfo->id = id;
      mapInfo->offset = offset;
      mapInfo->zeroAddr = zeroAddr;
      
      if (!initArgs->mapHead) {
         initArgs->mapHead = mapInfo;
      }
      
      if (initArgs->mapTail) {
         initArgs->mapTail->next = mapInfo;
      }
      
      initArgs->mapTail = mapInfo;
   }
   
   if (status != VMK_OK && mapInfo != NULL) {
      User_HeapFree(uci, mapInfo);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserInit_SetWorldWD --
 *	
 *      Save the name of the working directory for a new cartel for
 *      later.  Not for changing the WD once the world is running.
 *	Caller owns dirname and ensures it is null terminated.
 *      
 * Results:
 *	VMK_OK if world is a new cartel world, and space to copy the
 *	name can be allocated, error code otherwise
 *
 * Side effects:
 *	Name is saved off for later.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserInit_SetWorldWD(World_Handle* world,
                    const char* dirname)
{
   VMK_ReturnStatus status;
   User_CartelInfo* uci;

   ASSERT(world != MY_RUNNING_WORLD);
   status = UserInitIsNewUserWorld(world);
   if (status != VMK_OK) {
      return status;
   }
      
   UWLOGFor(1, world, "dir=%s", dirname);

   uci = world->userCartelInfo;
   ASSERT(uci != NULL);

   // Copy the name for lookup later.
   uci->args.workingDirName = User_HeapAlloc(uci, strlen(dirname)+1);
   if (uci->args.workingDirName == NULL) {
      UWLOGFor(0, world, "heap's already full?!");
      status = VMK_NO_MEMORY;
   } else {
      strcpy(uci->args.workingDirName, dirname);
      status = VMK_OK;
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserInit_SetIdentity --
 *	
 *      Set the initial identity values for the first thread in a new
 *      cartel.  Caller owns gids pointer and ensures there are at
 *      least ngids entries in it.
 *      
 * Results:
 *	VMK_OK if world is a new cartel, error code otherwise
 *
 * Side effects:
 *	cartel identity initialized
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserInit_SetIdentity(World_Handle* world,
                     uint32 umask,
                     uint32 ruid, uint32 euid, uint32 suid,
                     uint32 rgid, uint32 egid, uint32 sgid,
                     uint32 ngids, uint32* gids)
{
   User_CartelInfo *uci;
   Identity *ident;
   VMK_ReturnStatus status;

   ASSERT(world != MY_RUNNING_WORLD);
   status = UserInitIsNewUserWorld(world);
   if (status != VMK_OK) {
      return status;
   }

   UWLOGFor(1, world, "umask=%#x ruid=%d, euid=%d, suid=%d ...",
            umask, ruid, euid, suid);
      
   uci = world->userCartelInfo;
   ident = &world->ident;
   uci->fdState.umask = umask;
   ident->ruid = ruid;
   ident->euid = euid;
   ident->suid = suid;
   ident->rgid = rgid;
   ident->egid = egid;
   ident->sgid = sgid;
   ngids = MIN(ngids, ARRAYSIZE(ident->gids));
   ASSERT(sizeof(uint32) == sizeof(Identity_GroupID));
   memcpy(ident->gids, gids, ngids * sizeof(Identity_GroupID));
   ident->ngids = ngids;
   
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserInit_SetDumpFlag --
 *	
 *      Enable or disable coredumps in the new cartel.
 *      
 * Results:
 *	VMK_OK, unless world is not a new cartel world.
 *
 * Side effects:
 *	Core dumps are enabled or disabled
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserInit_SetDumpFlag(World_Handle* world,
                     Bool enabled)
{
   VMK_ReturnStatus status;
   User_CartelInfo *uci;

   ASSERT(world != MY_RUNNING_WORLD);
   status = UserInitIsNewUserWorld(world);
   if (status != VMK_OK) {
      return status;
   }
      
   UWLOGFor(1, world, "%s", enabled ? "Enabled" : "Disabled");

   uci = world->userCartelInfo;
   uci->coreDump.enabled = enabled;

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserInit_SetMaxEnvVars --
 *
 *	Sets the maximum number of environment variables that can be
 *	declared at startup and allocates a table for them.
 *
 * Results:
 *	VMK_OK on success, or VMK_LIMIT_EXCEEDED or VMK_NO_MEMORY on error.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserInit_SetMaxEnvVars(World_Handle *world, int maxEnvVars)
{
   User_CartelInfo *uci = world->userCartelInfo;
   User_EnvInfo *envInfo = uci->args.envInfo;

   if (envInfo == NULL) {
      UWWarn("envInfo has not been allocated.");
      return VMK_NOT_FOUND;
   }

   if (envInfo->environ != NULL) {
      UWWarn("SetNumEnvVars already called!  Ignoring extraneous call.");
      return VMK_BAD_PARAM;
   }

   envInfo->environ = User_HeapAlloc(uci, maxEnvVars * sizeof(char*));
   if (envInfo->environ == NULL) {
      return VMK_NO_MEMORY;
   }

   envInfo->maxVars = maxEnvVars;

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserInit_AddEnvVar --
 *
 *	Adds an environment variable to this UserWorld's environment.
 *
 * Results:
 *	VMK_OK on success, or VMK_LIMIT_EXCEEDED or VMK_NO_MEMORY on error.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserInit_AddEnvVar(World_Handle *world, char *tmpEnvVar, uint32 length)
{
   User_CartelInfo *uci = world->userCartelInfo;
   User_EnvInfo *envInfo = uci->args.envInfo;
   char *envVar;

   if (envInfo == NULL) {
      UWWarn("envInfo has not been allocated.");
      return VMK_NOT_FOUND;
   }

   if (length > USERWORLD_HEAP_MAXALLOC_SIZE) {
      UWWarn("Environment variable too long (%d vs %d).", length,
	     USERWORLD_HEAP_MAXALLOC_SIZE);
      return VMK_LIMIT_EXCEEDED;
   }

   if (envInfo->environ == NULL) {
      UWWarn("SetMaxEnvVars has not been called yet.");
      return VMK_NOT_FOUND;
   }

   if (envInfo->numVars >= envInfo->maxVars) {
      UWWarn("Too many environment variables declared!");
      return VMK_LIMIT_EXCEEDED;
   }

   envVar = User_HeapAlloc(uci, length);
   if (envVar == NULL) {
      return VMK_NO_MEMORY;
   }

   snprintf(envVar, length, "%s", tmpEnvVar);
   envInfo->environ[envInfo->numVars] = envVar;
   envInfo->numVars++;

   UWLOGFor(2, world, "Adding env var: [%d] %s", envInfo->numVars - 1,
	    envInfo->environ[envInfo->numVars - 1]);

   return VMK_OK;
}


