/* **********************************************************
 * Copyright 2001 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * pshare.h --
 *
 *	Header file for transparent page sharing.
 */

#ifndef	_PSHARE_H
#define	_PSHARE_H

#define  SP_RANK_PSHARE SP_RANK_IRQ_LEAF

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

/*
 * includes
 */

#include "pshare_ext.h"

/*
 * constants
 */

// nonexistent index
#define	PSHARE_MPN_NULL (0x0)

/*
 * operations
 */

// initialization
extern void PShare_EarlyInit(Bool enabled);
extern void PShare_LateInit(void);
extern Bool PShare_IsEnabled(void);

extern uint64 PShare_HashPage(MPN mpn);
extern uint64 PShare_HashToNodeHash(uint64 hash, NUMA_Node nodeNum);

// insert operations
extern VMK_ReturnStatus PShare_Add(uint64 key,
                                   MPN mpn,
                                   MPN *mpnShared,
                                   uint32 *count);
extern VMK_ReturnStatus PShare_AddIfShared(uint64 key,
                                           MPN mpn,
                                           MPN *mpnShared,
                                           uint32 *count,
                                           uint32 *mpnHint);

// remove operations
extern VMK_ReturnStatus PShare_Remove(uint64 key, MPN mpn, uint32 *count);
extern VMK_ReturnStatus PShare_RemoveIfUnshared(uint64 key, MPN mpn);

// lookup operations
extern VMK_ReturnStatus PShare_LookupByMPN(MPN mpn, uint64 *key, uint32 *count);
extern VMK_ReturnStatus PShare_LookupByKey(uint64 key,
                                           MPN *mpn,
                                           uint32 *count);
extern Bool PShare_IsZeroKey(uint64 key);

extern Bool PShare_IsZeroMPN(MPN mpn);

// hint operations
extern Bool PShare_HintKeyMatch(uint64 hintKey, uint64 key);
extern VMK_ReturnStatus PShare_AddHint(uint64 key,
                                       MPN mpn,
                                       World_ID worldID,
                                       PPN ppn);
extern VMK_ReturnStatus PShare_RemoveHint(MPN mpn,
                                          World_ID worldID,
                                          PPN ppn);
extern VMK_ReturnStatus PShare_LookupHint(MPN mpn,
                                          uint64 *key,
                                          World_ID *worldID,
                                          PPN *ppn);
extern uint32 PShare_GetNumContMPNs(MPN minMPN, MPN maxMPN, Bool hotAdd);
extern VMK_ReturnStatus PShare_AssignContMPNs(MPN minMPN, MPN maxMPN, 
                                              Bool hotAdd, uint32 reqSize, 
                                              MPN startMPN);

// statistics operations
extern void PShare_TotalShared(uint32 *nCOW,
                               uint32 *nCOW1,
                               uint32 *nUsed,
                               uint32 *nHint);

extern void PShare_ReportCollision(uint64 key, World_ID worldID, PPN ppn);

#endif
