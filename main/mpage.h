/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

#ifndef	_MPAGE_H
#define	_MPAGE_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

typedef uint8 MPage_Tag;
typedef struct {
   MPage_Tag tag;
   uint8 opaque[15];
} MPage __attribute__ ((packed));

// MPage tags
#define MPAGE_TAG_INVALID         0x0
#define MPAGE_TAG_PSHARE_REGULAR  0x1
#define MPAGE_TAG_PSHARE_HINT     0x2
#define MPAGE_TAG_ANON_MPN        0x3

/* initialization interfaces -- exported to memmap.c */

extern uint32 MPage_GetNumContMPNs(MPN minMPN, MPN maxMPN, Bool hotAdd);
extern VMK_ReturnStatus MPage_AssignContMPNs(MPN minMPN, MPN maxMPN, 
                                             Bool hotAdd, uint32 reqSize, 
                                             MPN startMPN);


/* public interfaces for general use */

extern MPage *MPage_Map(MPN mpn, KSEG_Pair **pair);
extern void MPage_Unmap(KSEG_Pair *pair);

extern uint32 MPage_GetNumMachinePages(void); 
extern uint32 MPage_GetNumOverheadPages(void);

#endif
