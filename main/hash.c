/* **********************************************************
 * Copyright 2001 VMware, Inc.  All rights reserved. -- VMware Confidential
 * $Id$
 * **********************************************************/

/*
 * hash.c --
 *
 *	All the code for this file lives in lib/shared/hash.h.
 */

#include "vmware.h"
#include "hash.h"

/*
 * Wrappers
 */

// arbitrary constant
#define	HASH_INIT_VALUE	(42)

// 64-bit hash for array of "nBytes" bytes
uint64 
Hash_Bytes(uint8 *key, uint32 nBytes)
{
   return(hash3(key, nBytes, HASH_INIT_VALUE));
}

uint64 
Hash_BytesSlow(uint8 *key, uint32 nBytes)
{
   return(hash(key, nBytes, HASH_INIT_VALUE));
}

// 64-bit hash for array of "nQuads" uint64s
uint64 
Hash_Quads(uint64 *key, uint32 nQuads)
{
   return(hash2(key, nQuads, HASH_INIT_VALUE));
}

// 64-bit hash for one 4K page
uint64 
Hash_Page(void *addr)
{
   return(hash2((uint64 *) addr, PAGE_SIZE / sizeof(uint64), HASH_INIT_VALUE));
}

