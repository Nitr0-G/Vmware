/*                                                -*-buffer-read-only: t-*-
 * **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. 
 * **********************************************************
 */

#ifndef UWVMKSYSCALL_GENERATED_UWVMK_DIST_H
#define UWVMKSYSCALL_GENERATED_UWVMK_DIST_H

#define INCLUDE_ALLOW_DISTRIBUTE
#include "includeCheck.h"

#include <errno.h>
#include "vm_basic_types.h"

static int VMKernel_GetSyscallVersion(uint32* version);
static int VMKernel_MemTestMap(MPN * inOutMPN,
                               uint32 * numPages,
                               void ** addr);
static int VMKernel_SysAlert(const char * msg);

#define UWVMKSYSCALL_CHECKSUM 0x8117266a

/*
 * UWVMKSyscall syscall numbers.
 */
typedef enum {
    UWVMKSYSCALL_SYSCALLNUM_GetSyscallVersion               = 0,
    UWVMKSYSCALL_SYSCALLNUM_MemTestMap                      = 24,
    UWVMKSYSCALL_SYSCALLNUM_SysAlert                        = 48,
    UWVMKSYSCALL_SYSCALLNUM_MAX                             = 51,
} UWVMKSyscall_Number;




/*
 *----------------------------------------------------------------------
 *
 * VMKernel_GetSyscallVersion --
 *
 * Generated automatically from the prototype:
 *   GetSyscallVersion(uint32* version +OUT)
 *
 *----------------------------------------------------------------------
 */
static inline int
VMKernel_GetSyscallVersion(uint32* version)
{
    uint32 rc = 0;
    uint32 linuxrc = 0;
    __asm__ volatile (
        "movl %2, %%eax\n"  \
        "int $0x90\n"         
        : /*output:*/         
        "=a" (rc),            
        "=b" (linuxrc)        
        : /*input:*/          
        "i" (UWVMKSYSCALL_SYSCALLNUM_GetSyscallVersion)
        ,"b" (version)
    );
    if (linuxrc != 0) {
       errno = -linuxrc;
    }
    return rc;
}


/*
 *----------------------------------------------------------------------
 *
 * VMKernel_MemTestMap --
 *
 * Generated automatically from the prototype:
 *   MemTestMap(MPN *inOutMPN, uint32 *numPages, void **addr)
 *
 *----------------------------------------------------------------------
 */
static inline int
VMKernel_MemTestMap(MPN * inOutMPN,
                    uint32 * numPages,
                    void ** addr)
{
    uint32 rc = 0;
    uint32 linuxrc = 0;
    __asm__ volatile (
        "movl %2, %%eax\n"  \
        "int $0x90\n"         
        : /*output:*/         
        "=a" (rc),            
        "=b" (linuxrc)        
        : /*input:*/          
        "i" (UWVMKSYSCALL_SYSCALLNUM_MemTestMap)
        ,"b" (inOutMPN)
        ,"c" (numPages)
        ,"d" (addr)
    );
    if (linuxrc != 0) {
       errno = -linuxrc;
    }
    return rc;
}


/*
 *----------------------------------------------------------------------
 *
 * VMKernel_SysAlert --
 *
 * Generated automatically from the prototype:
 *   SysAlert(const char *msg +STRING[81])
 *
 *----------------------------------------------------------------------
 */
static inline int
VMKernel_SysAlert(const char * msg)
{
    uint32 rc = 0;
    uint32 linuxrc = 0;
    __asm__ volatile (
        "movl %2, %%eax\n"  \
        "int $0x90\n"         
        : /*output:*/         
        "=a" (rc),            
        "=b" (linuxrc)        
        : /*input:*/          
        "i" (UWVMKSYSCALL_SYSCALLNUM_SysAlert)
        ,"b" (msg)
    );
    if (linuxrc != 0) {
       errno = -linuxrc;
    }
    return rc;
}
#endif /* UWVMKSYSCALL_GENERATED_UWVMK_DIST_H */
