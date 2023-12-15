/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/


#ifndef _USER_LAYOUT_H
#define _USER_LAYOUT_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMCORE
#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "rateconv.h"

/*
 * User World memory layout.	 (Not to scale.)
 *
 * LINEAR ADDRESSES: See vmk_layout.h

 * VIRTUAL ADDRESSES: UserWorlds get just under 3GB of address space
 * (1GB of space is needed by the VMKernel, plus 4MB the vmkernel
 * reserves for idt, gdt, etc).	 We statically partition the remaining
 * address space among the following concerns:
 *   text and heap                        : 131MB
 *   initial stack                        : 1MB
 *   mmap (includes overhead/anon memory) : ~2.8GB
 *
 * The text and heap are at the low end, while the stack starts at the high
 * end.  The mmap area fills the space between the heap and stack.
 *
 * Note: VMK_USER_FIRST_TEXT_VADDR is so high to be compatible with
 * the linux toolchain.  We could move it down towards VA 0, or stick
 * something else in the first 130MB of the address space...
 *
 * Two special pages are below VMK_USER_FIRST_TEXT_VADDR.  The ktext
 * page is a page of read-only code provided by the vmkernel.  The
 * tdata page is a thread-specific page of read-only data provided by
 * the vmkernel.  The tdata page is the only page accessible in user
 * mode that differs between the threads in a cartel.  The entire page
 * table that it resides in is per-thread and cannot be used for any
 * cartel-wide pages.
 *
 * The user level Data Segment spans the entire range of VMK_USER_MAX_PAGES.
 * The user level Code Segment extends from VMK_USER_FIRST_VADDR up to
 * VMK_USER_LAST_MMAP_TEXT_VADDR. This is done so that the user level
 * stacks and pthread stacks are not executable. Because of this 
 * division, the portion of mmap region which is in the Code Segment
 * gets around 1GB or the address and nearly 1.8GB is reserved for mmaping
 * user data pages.
 *
 * This is how the user virtual address space is divvied up when in
 * user mode:
 *
 *		 0 +-------------+ 
 *		   .		 .
 *		   .		 . (unmapped, wasted VAs)
 *		   .		 .
 *	0x07d00000 +-------------+ VMK_USER_FIRST_KTEXT_VADDR
 *		   |		 |
 *		   |		 | (kernel text mapped into userworld)
 *		   |		 |
 *	0x07d00fff +-------------+ VMK_USER_LAST_KTEXT_VADDR
 *		   .		 .
 *		   .		 . (unmapped, wasted VAs)
 *		   .		 .
 *	0x07e00000 +-------------+ VMK_USER_FIRST_TDATA_VADDR
 *		   |		 |
 *		   |		 | (thread-specific data page)
 *		   |		 |
 *	0x07e00fff +-------------+ VMK_USER_LAST_TDATA_VADDR
 *		   .		 .
 *		   .		 . (unmapped, wasted VAs in
 *                 .             .  thread-specific page table)
 *		   .		 .
 *	0x07ffffff +-------------+ VMK_USER_LAST_TDATA_PT_VADDR
 *	0x08000000 +-------------+ VMK_USER_FIRST_TEXT_VADDR
 *		   |		 |
 *		   |		 | (text)
 *		   |		 |
 *		   |		 | <--- uci->mem.dataStart
 *		   |		 |
 *		   |		 | (heap: 130MB - text size)
 *		   |		 |
 *	0x10000000 +-------------+ end heap/start mmap text (VMK_USER_FIRST_MMAP_TEXT_VADDR)
 *		   |		 | (mmap text region: 16MB)
 *	0x11000000 +-------------+ end mmap text/start mmap data (VMK_USER_FIRST_MMAP_DATA_VADDR)
 *		   |		 |
 *		   |		 |
 *		   |		 | (mmap data region: ~2.8GB)
 *		   |             |
 *		   |             |
 *		   + --- --- --- + end of mmap region (VMK_USER_LAST_MMAP_DATA_VADDR)
 *		   |		 | (1 page)
 *	0xbf500000 + --- --- --- + VMK_USER_MIN_STACK_ADDR 
 *		   |		 |
 *		   |	  ^	 | (1 MB)
 *		   | <mainstack> |
 *	0xbf5fffff +-------------+ VMK_USER_LAST_ADDR  (4GB - 1030MB - 4MB)
 *		   .		 .
 *		   .		 . (unmapped, beyond end of usermode segment)
 *		   .		 .
 *	0xffffffff +-------------+ 
 *	    
 */

#define VMK_USER_MAX_HEAP_PAGES		32768 // 131 MB
#define VMK_USER_MAX_STACK_PAGES 	256   // 1 MB
#define VMK_USER_MAX_KTEXT_PAGES 	1
#define VMK_USER_MAX_TDATA_PAGES 	1

#define VMK_USER_FIRST_VPN		((VPN)0)
#define VMK_USER_FIRST_KTEXT_VPN	((VPN)0x7d00)
#define VMK_USER_LAST_KTEXT_VPN		(VMK_USER_FIRST_KTEXT_VPN + VMK_USER_MAX_KTEXT_PAGES - 1)
#define VMK_USER_FIRST_TDATA_VPN        ((VPN)0x7e00)
#define VMK_USER_LAST_TDATA_VPN		(VMK_USER_FIRST_TDATA_VPN + VMK_USER_MAX_TDATA_PAGES - 1)
#define VMK_USER_LAST_TDATA_PT_VPN      (VMK_USER_FIRST_TDATA_VPN + VMK_PTES_PER_PDE - 1)
#define VMK_USER_FIRST_TEXT_VPN		((VPN)0x8000) // That's what Linux uses.
#define VMK_USER_LAST_VPN		(VMK_USER_FIRST_VPN + VMK_USER_MAX_PAGES - 1)
#define VMK_USER_FIRST_KTEXT_VADDR	(VPN_2_VA(VMK_USER_FIRST_KTEXT_VPN))
#define VMK_USER_LAST_KTEXT_VADDR	(VPN_2_VA(VMK_USER_LAST_KTEXT_VPN) + (PAGE_SIZE - 1))
#define VMK_USER_FIRST_TDATA_VADDR	(VPN_2_VA(VMK_USER_FIRST_TDATA_VPN))
#define VMK_USER_LAST_TDATA_VADDR	(VPN_2_VA(VMK_USER_LAST_TDATA_VPN) + (PAGE_SIZE - 1))
#define VMK_USER_LAST_TDATA_PT_VADDR	(VPN_2_VA(VMK_USER_LAST_TDATA_PT_VPN) + (PAGE_SIZE - 1))
#define VMK_USER_FIRST_TEXT_VADDR	(VPN_2_VA(VMK_USER_FIRST_TEXT_VPN))
 
#define VMK_USER_FIRST_MMAP_TEXT_VADDR	(VPN_2_VA((VMK_USER_FIRST_TEXT_VPN + VMK_USER_MAX_HEAP_PAGES)))
#define VMK_USER_LAST_MMAP_TEXT_VADDR	(VMK_USER_FIRST_MMAP_DATA_VADDR  - 1)
#define VMK_USER_LAST_TEXT_VADDR	VMK_USER_LAST_MMAP_TEXT_VADDR
#define VMK_USER_MAX_MMAP_TEXT_PAGES    4096 // 16MB space for MMAP text  
 
#define VMK_USER_FIRST_MMAP_DATA_VADDR  (VMK_USER_FIRST_MMAP_TEXT_VADDR + VMK_USER_MAX_MMAP_TEXT_PAGES * PAGE_SIZE)
#define VMK_USER_LAST_MMAP_DATA_VADDR	(VMK_USER_MIN_STACK_VADDR - PAGE_SIZE - 1)
#define VMK_USER_MAX_MMAP_DATA_PAGES    ((VA_2_VPN(VMK_USER_LAST_MMAP_DATA_VADDR)) - (VA_2_VPN(VMK_USER_FIRST_MMAP_DATA_VADDR)) + 1)

#define VMK_USER_MAX_CODE_SEG_PAGES     ((VA_2_VPN(VMK_USER_LAST_MMAP_TEXT_VADDR)) + 1)

#define VMK_USER_MIN_STACK_VADDR	((VPN_2_VA(VMK_USER_LAST_VPN - VMK_USER_MAX_STACK_PAGES + 1)))
#define VMK_USER_LAST_VADDR		((VPN_2_VA(VMK_USER_LAST_VPN)) + (PAGE_SIZE - 1))

#define VMK_USER_IS_ADDR_IN_CODE_SEGMENT(addr) \
 	(addr >= VMK_USER_FIRST_TEXT_VADDR && \
	 addr < VMK_USER_LAST_MMAP_TEXT_VADDR)


/*
 * Structure of the thread-specific data (tdata) page provided by the
 * vmkernel to userworlds.  The location of the page is defined above
 * as VMK_USER_FIRST_TDATA_VADDR.
 */
#define USER_THREADDATA_MAGIC 0x5ca1ab1e
#define USER_THREADDATA_MINOR_VERSION 1
#define USER_THREADDATA_MAJOR_VERSION 1
typedef struct User_ThreadData {
   uint32 magic; // =USER_THREADDATA_MAGIC

   /* Structure version number. Change the major version if you
    * rearrange the struct and thus break compatibility of new
    * vmkernels with old userspace software.  Change the minor version
    * if you add fields and thus break compatibilitity of old
    * vmkernels with new userspace software that uses the new fields.
    */
   uint16 minorVersion;
   uint16 majorVersion;

   uint32 tid; // Linux-style task ID (=process ID) of this thread

   uint64 (*pseudoTSCGet)(void);  // PTSC_Get function (in the ktext page)
   RateConv_Params pseudoTSCConv; // Parameters for PTSC_Get
   
} User_ThreadData;


#endif
