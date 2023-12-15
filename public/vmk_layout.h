/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/


#ifndef _VMK_LAYOUT_H
#define _VMK_LAYOUT_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMMEXT
#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMCORE
#include "includeCheck.h"

#define VMK_PDPTE_MASK          PDPTOFF_MASK
#define VMK_PDPTE_SHIFT         30

#define VMK_PDE_MASK            0x1ff
#define VMK_PDE_SHIFT           21

#define VMK_PTE_MASK            0x1ff
#define VMK_PTE_SHIFT           12

#define VMK_NUM_PDPTES          (VMK_PDPTE_MASK + 1)
#define VMK_PDES_PER_PDPTE      (VMK_PDE_MASK + 1)
#define VMK_PTES_PER_PDE        (VMK_PTE_MASK + 1)
#define PDE_SIZE                VMK_PTES_PER_PDE * PAGE_SIZE

#define ADDR_PDPTE_BITS(x)      (((x) >> VMK_PDPTE_SHIFT) & VMK_PDPTE_MASK)
#define ADDR_PDE_BITS(x)        (((x) >> VMK_PDE_SHIFT) & VMK_PDE_MASK)
#define ADDR_PTE_BITS(x)        (((x) >> VMK_PTE_SHIFT) & VMK_PTE_MASK)
#define ADDR_PGOFFSET_BITS(x)   ((x) & (PAGE_SIZE - 1))
#define PTBITS_ADDR(pdpt,pd,pt) ((((pdpt) & VMK_PDPTE_MASK) << VMK_PDPTE_SHIFT) | \
                                 (((pd) & VMK_PDE_MASK) << VMK_PDE_SHIFT) | \
                                 (((pt) & VMK_PTE_MASK) << VMK_PTE_SHIFT))


#define VMM_FIRST_LINEAR_ADDR 	0xffc00000  // 0 minus 4MB

#define VMM_FIRST_VPN   	0
#define VMM_NUM_PAGES           1024

/*
 * code+heap+kvmap+stacks+xmap are sized to fit in a single page directory
 * that is shared by all vmkernel worlds.  This way any changes to the page
 * directory itself show up in all worlds immediately.
 * prda+kseg are CPU specific, so they can be different for different
 * worlds, and therefore they cannot fit in the same page directory.
 */
#define VMK_NUM_CODEHEAP_PDES   18  //    4 -   40MB (VA) (LA = VA - 4MB)
#define VMK_NUM_MAP_PDES        4   //   40 -   48MB
#define VMK_NUM_STACK_PDES      8   //   48 -   64MB
#define VMK_NUM_XMAP_PDES       482 //   64 - 1028MB
#define VMK_NUM_PRDA_PDES       1   // 1028 - 1030MB
#define VMK_NUM_KSEG_PDES       2   // 1030 - 1034MB

#define VMK_NUM_CODE_PAGES	1024
#define VMK_NUM_CODE_PDES	(VMK_NUM_CODE_PAGES / VMK_PTES_PER_PDE)
#define VMK_NUM_CODEHEAP_PAGES  (VMK_NUM_CODEHEAP_PDES * VMK_PTES_PER_PDE)

#define VMK_FIRST_MAP_PDE       (VMK_NUM_CODEHEAP_PDES)
#define VMK_FIRST_STACK_PDE     (VMK_FIRST_MAP_PDE + VMK_NUM_MAP_PDES)
#define VMK_FIRST_XMAP_PDE      (VMK_FIRST_STACK_PDE + VMK_NUM_STACK_PDES)
#define VMK_FIRST_PRDA_PDE      (VMK_PDES_PER_PDPTE)
#define VMK_FIRST_KSEG_PDE      (VMK_FIRST_PRDA_PDE + VMK_NUM_PRDA_PDES)

#if (VMK_FIRST_XMAP_PDE + VMK_NUM_XMAP_PDES > VMK_PDES_PER_PDPTE)
#error "code+map+stack+xmap must fit in 1 pagedir"
#endif

#define VMK_NUM_PDES            (VMK_FIRST_KSEG_PDE + VMK_NUM_KSEG_PDES)
// COS only sees codedata and kvmap
#define VMK_NUM_HOST_PDES       (VMK_FIRST_MAP_PDE + VMK_NUM_MAP_PDES)

#define VMK_FIRST_LINEAR_ADDR 	0x0
#define VMK_FIRST_PDOFF         ADDR_PDE_BITS(VMK_FIRST_LINEAR_ADDR)

#define VMK_FIRST_VPN  		VMM_NUM_PAGES
#define VMK_FIRST_ADDR		(VPN_2_VA(VMK_FIRST_VPN))
#define	VMK_CODE_START		(VMK_FIRST_ADDR + PAGE_SIZE)
   
#define VMK_CODE_LENGTH		(VMK_NUM_CODE_PAGES * PAGE_SIZE)
   
#define VMK_HOST_STACK_PAGES	3
#define VMK_HOST_STACK_BASE	(VMK_FIRST_VPN + VMK_NUM_CODE_PAGES) * PAGE_SIZE
#define VMK_HOST_STACK_TOP	(VMK_HOST_STACK_BASE + VMK_HOST_STACK_PAGES * PAGE_SIZE)

#define VMK_FIRST_MAP_VPN	(VMK_FIRST_VPN + (VMK_FIRST_MAP_PDE * VMK_PTES_PER_PDE))
#define VMK_LAST_MAP_VPN	(VMK_FIRST_MAP_VPN + VMK_NUM_MAP_PDES * VMK_PTES_PER_PDE - 1)
#define VMK_FIRST_MAP_ADDR	(VPN_2_VA(VMK_FIRST_MAP_VPN))

#define VMK_FIRST_STACK_VPN	(VMK_FIRST_VPN + (VMK_FIRST_STACK_PDE * VMK_PTES_PER_PDE))
#define VMK_LAST_STACK_VPN	(VMK_FIRST_STACK_VPN + VMK_NUM_STACK_PDES * VMK_PTES_PER_PDE - 1)

#define VMK_FIRST_STACK_ADDR	(VPN_2_VA(VMK_FIRST_STACK_VPN))

#define VMK_FIRST_PRDA_VPN	(VMK_FIRST_VPN + (VMK_FIRST_PRDA_PDE * VMK_PTES_PER_PDE))
#define VMK_FIRST_PRDA_ADDR	(VPN_2_VA(VMK_FIRST_PRDA_VPN))

#define VMK_FIRST_KSEG_VPN	(VMK_FIRST_VPN + (VMK_FIRST_KSEG_PDE * VMK_PTES_PER_PDE))
#define VMK_FIRST_KSEG_ADDR	(VPN_2_VA(VMK_FIRST_KSEG_VPN))

#define VMK_FIRST_XMAP_VPN	(VMK_FIRST_VPN + (VMK_FIRST_XMAP_PDE * VMK_PTES_PER_PDE))
#define VMK_FIRST_XMAP_ADDR	(VPN_2_VA(VMK_FIRST_XMAP_VPN))
#define VMK_XMAP_LENGTH         (VMK_NUM_XMAP_PDES*PDE_SIZE)

#define VMM_VMK_PAGES	    	(VMM_NUM_PAGES + VMK_PTES_PER_PDE * VMK_NUM_PDES)

#define VMK_KVMAP_BASE		VMK_FIRST_MAP_ADDR
#define VMK_KVMAP_PAGES		(VMK_NUM_MAP_PDES * VMK_PTES_PER_PDE)
#define VMK_KVMAP_LENGTH	(PAGES_2_BYTES(VMK_KVMAP_PAGES))

#define VMK_KSEG_PTABLE_ADDR	(VMK_FIRST_PRDA_ADDR + 2 * PAGE_SIZE)
// using 3+ instead of 2+ to leave space for an empty page to check for
// out of range errors.
#define VMK_KSEG_PTR_BASE	(VMK_FIRST_PRDA_ADDR + (3 + VMK_NUM_KSEG_PDES) * PAGE_SIZE)

#define VMK_KSEG_MAP_BASE	(VMK_FIRST_KSEG_ADDR)
#define VMK_KSEG_MAP_LENGTH	(PAGES_2_BYTES(VMK_NUM_KSEG_PDES * VMK_PTES_PER_PDE))

#define VMK_VA_END		(VMK_FIRST_ADDR + ((VMK_NUM_PDES) * PDE_SIZE))

#define VMK_VA_2_LA(va)         ((va) + VMM_FIRST_LINEAR_ADDR)
#define VMK_LA_2_VA(la)         ((la) - VMM_FIRST_LINEAR_ADDR)

#define VMK_NUM_STACKPAGES_PER_WORLD    3

/*
 * Linear addresses in a user world.
 *
 *		 0 +-------------+ 
 *		   .		 .
 *		   .		 . <vmkernel/kmap/xmap/heap/stacks/etc>
 *		   .		 . (1GB of address space)
 *		   .		 .
 *	0x40600000 +-------------+ VMK_USER_FIRST_LADDR
 *		   |		 |
 *		   |		 | <userworld as defined in user_layout.h>
 *		   |		 | (3GB - 4MB of address space)
 *		   |		 |
 *	0xffbfffff +-------------+ VMK_USER_LAST_LADDR
 *		   .		 .
 *		   .		 . <vmm>
 *		   .		 . (4MB of address space)
 *		   .		 .
 *	0xffffffff +-------------+ 
 */

#define _ONEGB 1024U*1024U*1024U
#define _ONEGB_PAGES BYTES_2_PAGES(_ONEGB)

#define VMK_USER_FIRST_LADDR		(VMK_VA_2_LA(VMK_VA_END))
#define VMK_USER_FIRST_LPN		LA_2_LPN(VMK_USER_FIRST_LADDR)
#define VMK_USER_MAX_PAGES		((4 * _ONEGB_PAGES) - VMM_NUM_PAGES - VMK_USER_FIRST_LPN)
#define VMK_USER_LAST_LADDR		(LPN_2_LA(VMK_USER_VPN_2_LPN(VMK_USER_LAST_VPN)) + (PAGE_SIZE - 1))

#define VMK_USER_VPN_2_LPN(vpn)		((vpn) + VMK_USER_FIRST_LPN)
#define VMK_USER_LPN_2_VPN(lpn)		((lpn) - VMK_USER_FIRST_LPN)
#define VMK_USER_VA_2_LA(va)		((va) + VMK_USER_FIRST_LADDR)
#define VMK_USER_LA_2_VA(la)		((la) - VMK_USER_FIRST_LADDR)

#endif
