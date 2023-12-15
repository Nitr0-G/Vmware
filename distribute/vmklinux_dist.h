
/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * vmklinux_dist.h --
 *
 *      Prototypes for functions used in device drivers compiled for vmkernel.
 */

#ifndef _VMKLINUX_DIST_H_
#define _VMKLINUX_DIST_H_

#if __GNUC__
#ifndef __STRICT_ANSI__
#ifdef VM_IA64
typedef unsigned long vmk_uint64;
typedef long vmk_int64;
#else
typedef unsigned long long vmk_uint64;
typedef long long vmk_int64;
#endif
#endif
#else
#error - Need compiler define for int64/uint64
#endif
typedef unsigned int       vmk_uint32;
typedef unsigned short     vmk_uint16;
typedef unsigned char      vmk_uint8;


/*
 * Versioning between vmkernel and drivers.
 */

#define  VMK_VERSION_OK            0
#define  VMK_VERSION_NOT_SUPPORTED 1
#define  VMK_VERSION_MINOR         2

#define MAKE_VMKDRIVER_VERSION(major,minor) (((major) << 16) | (minor))
#define VMKDRIVER_VERSION_MAJOR(version)    ((version) >> 16)
#define VMKDRIVER_VERSION_MINOR(version)    ((version) & 0xffff)
#define VMKDRIVER_VERSION                   MAKE_VMKDRIVER_VERSION(5,0)

/*
 * Linux stubs functions.
 */

extern void* phys_to_kmap(vmk_uint64 maddr, vmk_uint32 len, void** pptr);
extern void phys_to_kmapFree(void* vaddr, void* pptr);
extern inline void vmk_p2v_memcpy(void *dst, vmk_uint64 src, vmk_uint32 length);
extern inline void vmk_v2p_memcpy(vmk_uint64 dst, void *src, vmk_uint32 length);
extern char *simple_strstr(const char *s1, const char *s2);

/*
 * SCSI device functions.
 */

#ifdef SCSI_DRIVER

extern struct Scsi_Host * vmk_scsi_register(Scsi_Host_Template *t, int j,
				vmk_uint16 bus, vmk_uint16 devfn);
extern void scsi_register_uinfo(struct Scsi_Host *h, vmk_uint16 bus,
                                vmk_uint16 devfn, void *key);
extern void scsi_set_max_xfer(struct Scsi_Host *hostPtr, vmk_uint32 maxXfer);
extern void vmk_lock_scsihostlist(void);
extern void vmk_unlock_scsihostlist(void);
extern void vmk_verify_memory_for_io(vmk_uint64 addr,vmk_uint64 len);
extern void scsi_state_change(struct Scsi_Host *);
#endif // SCSI_DRIVER

/*
 * Block device functions.
 */

#ifdef BLOCK_DRIVER

struct block_device_operations;

extern void vmk_block_init_start(void);
extern void vmk_block_init_done(void);
extern int vmk_register_blkdev(vmk_uint32 major, const char *name,
                               struct block_device_operations *ops,
		               int bus, int devfn, void *data,
                               spinlock_t *iorl);
extern void vmk_block_register_key(vmk_uint32 major, void *key);
extern void vmk_block_register_sglimit(vmk_uint32 major, int sgSize, 
                                       int maxXfer);

#endif // BLOCK_DRIVER

#endif // _VMKLINUX_DIST_H_
