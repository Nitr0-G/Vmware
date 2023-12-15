/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * sched_ext.h --
 *
 *	External defines for the scheduler.
 */

#ifndef _SCHED_EXT_H
#define _SCHED_EXT_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "vcpuid.h"
#include "world_ext.h"

/*
 * Constants
 */

// unconfigured
#define	SCHED_CONFIG_NONE		(-1)

// special share values
#define	SCHED_CONFIG_SHARES_LOW		(-2)
#define	SCHED_CONFIG_SHARES_NORMAL	(-3)
#define	SCHED_CONFIG_SHARES_HIGH	(-4)
#define SCHED_CONFIG_SHARES_SPECIAL(x)	((x) < 0)

// network filter config
#define	SCHED_CONFIG_NF_LEN		(32)

// scheduler group names
#define	SCHED_GROUP_NAME_LEN		(32)
// invalid group name
#define	SCHED_GROUP_NAME_INVALID	"invalid"
// predefined group names
#define	SCHED_GROUP_NAME_ROOT		"host"
#define	SCHED_GROUP_NAME_IDLE		"idle"
#define	SCHED_GROUP_NAME_SYSTEM	        "system"
#define	SCHED_GROUP_NAME_LOCAL	        "local"
#define	SCHED_GROUP_NAME_CLUSTER	"cluster"
#define	SCHED_GROUP_NAME_CONSOLE	"console"
#define	SCHED_GROUP_NAME_UW_NURSERY	"uwnursery"
#define	SCHED_GROUP_NAME_MEMSCHED	"memsched"
#define	SCHED_GROUP_NAME_HELPER	        "helper"
#define	SCHED_GROUP_NAME_DRIVERS        "drivers"
#define	SCHED_GROUP_NAME_VMKSTATS	"vmkstats"

// scheduler group identifiers
#define	SCHED_GROUP_ID_INVALID		((Sched_GroupID) -1)

// scheduler group limits
#define	SCHED_GROUPS_MAX_LG		(9)
#define	SCHED_GROUPS_MAX		(1 << SCHED_GROUPS_MAX_LG)
#define	SCHED_GROUPS_MASK		(SCHED_GROUPS_MAX - 1)
#define	SCHED_GROUP_MEMBERS_MAX		(256)
#define	SCHED_GROUP_PATH_LEN		(8)

// string length limits
#define SCHED_COLORAFFINITY_LEN		(256)

/*
 * Types
 */

// simple types
typedef int64  CpuSchedVtime;
typedef uint32 CpuSchedStride;
typedef uint32 CpuMask;

// describes if/how a vsmp is allowed
// to share packages in a hyperthreaded system
typedef enum {
   // share a package with anybody:
   CPUSCHED_HT_SHARE_ANY = 0,
   // only share a package with vcpus from same vsmp:
   CPUSCHED_HT_SHARE_INTERNALLY,
   // always take a whole package for each vcpu:
   CPUSCHED_HT_SHARE_NONE
} Sched_HTSharing;

// scheduler group identifier
typedef uint32 Sched_GroupID;

// root-to-leaf path, terminated by SCHED_INVALID_GROUP_ID
typedef struct {
   Sched_GroupID level[SCHED_GROUP_PATH_LEN];
} Sched_GroupPath;

#define SCHEDUNITS(EN,S)
#define SCHED_UNITS_LIST \
  SCHEDUNITS(SCHED_UNITS_PERCENT, "pct") \
  SCHEDUNITS(SCHED_UNITS_MHZ, "mhz") \
  SCHEDUNITS(SCHED_UNITS_BSHARES, "bshares") \
  SCHEDUNITS(SCHED_UNITS_MB, "mb") \
  SCHEDUNITS(SCHED_UNITS_PAGES, "pages") \
  SCHEDUNITS(SCHED_UNITS_INVALID, "invalid")

#undef SCHEDUNITS
#define SCHEDUNITS(EN, S) EN,

typedef enum {
   SCHED_UNITS_LIST
} Sched_Units;

typedef struct {
   int32 min;
   int32 max;
   int32 shares;
   int32 minLimit;
   int32 hardMax;
   Sched_Units units;
} Sched_Alloc;

typedef struct {
   uint32 numVcpus;
   Sched_Alloc alloc;
   Sched_HTSharing htSharing;
   CpuMask vcpuAffinity[MAX_VCPUS];
} Sched_CpuClientConfig;

typedef struct {
   int32  maxBalloon;
   uint32 nodeAffinity;
   char colorAffinity[SCHED_COLORAFFINITY_LEN];
   uint32 numAnon;      // in pages
   uint32 numOverhead;  // in pages
   Bool   pShare;       // enable page sharing?

   Bool   resuming;
} Sched_MemClientConfig;

typedef struct {
   // scheduler group
   char groupName[SCHED_GROUP_NAME_LEN];
   Bool createContainer;                // create container group?

   Sched_Alloc cpu;   
   Sched_Alloc mem;
} Sched_GroupConfig;

typedef struct {
   Sched_GroupConfig group;

   // cpu config
   Sched_CpuClientConfig cpu;

   // mem config
   Sched_MemClientConfig mem;

   // net config
   char   netFilterClass[SCHED_CONFIG_NF_LEN];
   char   netFilterArgs[SCHED_CONFIG_NF_LEN];
   
   // Allow world to use software TOE
   Bool toeEnabled;
} Sched_ClientConfig;

// affinity masks
#define CPUSCHED_AFFINITY_NONE		(0xffffffff)
#define CPUSCHED_AFFINITY(pcpu)		(1 << (pcpu))


#endif
