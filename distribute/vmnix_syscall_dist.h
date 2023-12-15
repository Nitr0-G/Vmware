/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * vmnix_syscall_dist.h --
 *
 *      module for interfacing with vmnix module
 */

#ifndef VMNIX_SYSCALL_DIST_H
#define VMNIX_SYSCALL_DIST_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_VMK_MODULE
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_DISTRIBUTE
#include "includeCheck.h"

#include "vmnix_if_dist.h"

typedef struct VMnix_ModAllocArgs {
   unsigned long moduleReadOnlySize;
   unsigned long moduleWritableSize;
   char modName[VMNIX_MODULE_NAME_LENGTH];
} VMnix_ModAllocArgs;


typedef struct VMnix_ModAllocResult {
   int 	moduleID;
   void	*readOnlyLoadAddr;
   void *writableLoadAddr;
} VMnix_ModAllocResult;


#define SCSI_SHARED_MAX_SLOTS 4
typedef struct VMnix_ModLoadDoneArgs {
   int		moduleID;
   void		*initFunc;
   void		*cleanupFunc;
   void		*earlyInitFunc;
   void		*lateCleanupFunc;
   VA		textBase;
   VA		dataBase;
   VA		bssBase;
   Bool		deviceOptions;
   int          nSlots;
   struct {
      unsigned int bus;
      unsigned int slot;
      unsigned int func;
   } pciInfo[SCSI_SHARED_MAX_SLOTS];
} VMnix_ModLoadDoneArgs;

typedef struct VMnix_ModUnloadArgs {
   int moduleID;
} VMnix_ModUnloadArgs;

typedef struct VMnix_SymArgs {
   char			*name;
   uint32		nameLength;
   uint32		value;
   uint32		size;
   int			info;
   int			moduleID;
   int			nextSymbolNum;
   Bool			global;
   int			numSymbols;
   int			namesLength;
} VMnix_SymArgs;

typedef struct VMnix_ModPutPageArgs {
   int 			moduleID;
   void			*addr;
   void			*data;
} VMnix_ModPutPageArgs;


typedef struct VMnix_ModDesc {
   char 		modName[VMNIX_MODULE_NAME_LENGTH];
   void			*readOnlyLoadAddr;
   void			*writableLoadAddr;
   unsigned long	readOnlyLength;
   unsigned long	writableLength;
   void			*initFunc;
   void			*cleanupFunc;
   void			*earlyInitFunc;
   void			*lateCleanupFunc;
   VA			textBase;
   VA			dataBase;
   VA			bssBase;
   int			moduleID;
   int			loaded;
   int			useCount;
} VMnix_ModDesc;


typedef struct VMnix_ModListResult {
   int			numModules;
   VMnix_ModDesc	desc[1];
} VMnix_ModListResult;


/*
 * generate vmnix system call numbers
 */
#define VMX_VMNIX_CALL(name, _ignore...)\
        VMNIX_##name,
enum {
#include "vmnix_sctable_dist.h"
};


/* Userlevel <-> vmnix module versioning:
 *
 * If the major numbers are different, then the vmx (vmkfstools, etc) will 
 * fail to run.  If minor is different, a warning will be printed.
 *
 * Please change the major version if your change is going to break
 * compatibility.  Change the minor version if your change is compatible, 
 * but perhaps you want to dynamically check the minor version and do 
 * different things. 
 */

#define MAKE_VMX_VMNIX_VERSION(major,minor) (((major) << 16) | (minor))
#define VMX_VMNIX_VERSION_MAJOR(version) ((version) >> 16)
#define VMX_VMNIX_VERSION_MINOR(version) ((version) & 0xffff)
#define VMX_VMNIX_VERSION MAKE_VMX_VMNIX_VERSION(46,0)

#define VMNIX_CHECK_VERSION(sysFn, msgFn) do {                       \
   uint32 _version = VMX_VMNIX_VERSION;                              \
   uint32 _result;                                                   \
   _result = sysFn(VMNIX_VERIFY_VERSION, (char *)&_version,          \
                   sizeof(_version), NULL, 0 );                      \
   if (_result != 0) {                                               \
      if (errno == ENOSYS) {                                         \
         msgFn("Version check failed, vmnix module not loaded?\n");  \
      } else if (errno == EPERM) {                                   \
         msgFn("Userlevel <-> vmnix module version mismatch\n");     \
      } else if (errno == EACCES) {                                  \
         msgFn("Permission denied\n");                               \
      } else {                                                       \
         msgFn("Error %d\n", errno);                                 \
      }                                                              \
      exit(-1);                                                      \
   }                                                                 \
}while (0)      

// Flags for VMNIX_OPEN_SCSI_DEV
#define SCSI_OPEN_DUMP			1   // Open core dump partn. 0xfc
#define SCSI_OPEN_HOST			2   // Open for host, not VMM
#define SCSI_OPEN_MULTIPLE_WRITERS	32  // Allow multiple VMs to open 
					    // SCSI device
#define SCSI_OPEN_PHYSICAL_RESERVE	128 // Pass through SCSI reserve,
					    // reset to physical bus
#endif
