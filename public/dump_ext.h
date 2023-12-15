/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * dump_ext.h --
 *
 *      external vmkernel core dump definitons.
 */

#ifndef _VMK_DUMP_EXT_H_
#define _VMK_DUMP_EXT_H_

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"
#include "world_ext.h"
#include "hardware_public.h"

#define DUMP_NAME_LENGTH	64
#define DUMP_MULTIPLE		512
#define DUMP_TYPE_KERNEL	0x1000
#define DUMP_TYPE_USER		0x2000
#define DUMP_TYPE_MASK		0xf000
/*
 * You should bump the appropriate version number if you make changes to the
 * structures here (or to the way the dumps are written).
 */
#define DUMP_VERSION_KERNEL	0x6
#define DUMP_VERSION_USER	0x5
#define DUMP_VERSION_MASK	0xfff

#define DUMP_TYPE_CHECK(dumpType, type) \
	(((dumpType) & DUMP_TYPE_MASK) == (type))
#define DUMP_VERSION_CHECK(dumpVersion, version) \
	(((dumpVersion) & DUMP_VERSION_MASK) == (version))

// vmkernel coredump is limited to 100MB (after compression)
#define VMKERNEL_DUMP_SIZE (100*1024*1024)

/*
 * Length of executable name buffer.  512 is an arbitrary number.  However, it
 * must be less than a page, as that's what the UserWorld dump header is
 * constrained to.
 */
#define DUMP_EXEC_NAME_LENGTH	512

struct VMKFullExcFrame;

/*
 * XXX: The 'version' element of the two structs below needs to stay as the
 * first element.
 */

typedef struct Dump_Info {
   uint32 version;
   Hardware_DMIUUID uuid;
   uint32 startOffset;
   uint32 dumpSize;
   uint32 readCount;
   uint32 regOffset;
   uint32 logOffset;
   uint32 stackOffset;
   VPN    stackStartVPN;
   uint32 stackNumMPNs;
   uint32 stack2Offset;
   VPN    stack2StartVPN;
   uint32 stack2NumMPNs;
   uint32 codeDataOffset;
   uint32 vmmOffset;
   uint32 kvmapOffset;
   uint32 prdaOffset;
   uint32 xmapOffset;
   
   uint32 logLength;
   uint32 logEnd;

   /*
    * Number of Dump_WorldData structures in core dump file.
    */
   uint32 regEntries;
} Dump_Info;

typedef struct UserDump_Header {
   uint32 version;

   uint32 startOffset;

   uint32 objTypesSize;
   uint32 mapTypesSize;

   uint32 objEntries;
   uint32 regEntries;
   uint32 mmapElements;
   uint32 heapRegions;

   /*
    * The executable that produced this dump.
    */
   char executableName[DUMP_EXEC_NAME_LENGTH];
} UserDump_Header;

typedef struct UserDump_Thread {
   World_ID worldID;
   uint32 uti;
} UserDump_Thread;

typedef struct UserDump_PtrTable {
   uint32 userCartelInfo;
   uint32 worldGroup;
   int numThreads;
   UserDump_Thread threadList[0];
} UserDump_PtrTable;

/*
 * PROT_EXEC/WRITE/READ must match the PF_X/W/R flags used in ELF core dumps.
 * See /usr/include/elf.h.
 */
#define USERDUMPMMAP_FLAGS_PROT_EXEC  0x1
#define USERDUMPMMAP_FLAGS_PROT_WRITE 0x2
#define USERDUMPMMAP_FLAGS_PROT_READ  0x4
#define USERDUMPMMAP_FLAGS_PROT_MASK  (USERDUMPMMAP_FLAGS_PROT_EXEC | \
				       USERDUMPMMAP_FLAGS_PROT_WRITE | \
				       USERDUMPMMAP_FLAGS_PROT_READ)
#define USERDUMPMMAP_FLAGS_PCD        0x8  // PCD = page cache disabled
#define USERDUMPMMAP_FLAGS_PINNED     0x10

typedef struct UserDump_MMap {
   /*
    * mmap region type.
    */
   int type;

   /*
    * Starting virtual address and length of region.
    */
   uint32 va;
   uint32 length;

   /*
    * Page protections and whether this region is pinned.
    */
   uint32 flags;

   /*
    * Offset within core file.
    */
   uint32 offset;

   /*
    * File-backed mmap info.
    */
   uint64 filePgOffset;
   uint32 obj;
} UserDump_MMap;

typedef struct UserDump_ObjEntry {
   /*
    * Object pointer.  This is used both as a unique reference for this object
    * as well as the ability to look the object up in the heap (when the heap is
    * dumped.
    */
   uint32 obj;

   /*
    * Index of this object in the file descriptor table if it was present,
    * otherwise -1.
    */
   int fd;

   /*
    * Type of object (UserObj_Type).
    */
   int type;

   /*
    * String representation of this object.
    *
    * XXX: Arbitrary length.  Really, it should be 4096 to be able to hold a
    * full path.  However, most paths easily fit within 500 characters.  So, to
    * conserve space in our core dumps, we set the length to 500.
    */
   char description[512 - (2 * sizeof(int)) - sizeof(uint32)];
} UserDump_ObjEntry;

typedef struct UserDump_HeapRange {
   VA start;
   uint32 length;
} UserDump_HeapRange;

typedef struct Dump_Registers {
   uint32 eax;
   uint32 ecx;
   uint32 edx;
   uint32 ebx;
   uint32 esp;
   uint32 ebp;
   uint32 esi;
   uint32 edi;
   uint32 eip;
   uint32 eflags;
   uint32 cs;
   uint32 ss;
   uint32 ds;
   uint32 es;
   uint32 fs;
   uint32 gs;
} Dump_Registers;

typedef struct Dump_WorldData {
   Dump_Registers regs;
   uint32         signal;
   World_ID       id;
   char           name[DUMP_NAME_LENGTH];
} Dump_WorldData;

/*
 *----------------------------------------------------------------------
 *
 * DumpHashUUID --
 *
 *      Hash the full UUID into a 8 bit value.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE uint8 DumpHashUUID(Hardware_DMIUUID *uuid)
{
   uint8 *p;
   uint8 hash = 0;

   for (p = (uint8*)uuid; p < ((uint8*)uuid) + sizeof *uuid; p++) {
      hash ^= *p;
   }
   return hash;
}

#endif
