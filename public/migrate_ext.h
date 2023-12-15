/* **********************************************************
 * Copyright 2000 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * migrate_ext.h --
 *
 *	External definitions for the migration module.
 */


#ifndef _MIGRATE_EXT_H
#define _MIGRATE_EXT_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMCORE // Can go away after dependency on vmnix_syscall.h goes away.
#define INCLUDE_ALLOW_VMMEXT
#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "return_status.h"

#define MIGRATE_MAX_MSG_DATA_LENGTH	32768

#define FMT_IP "<%u.%u.%u.%u>"
#define FMT_IP_ARGS(ip) \
   ((ip) >> 24) & 0xff, ((ip) >> 16) & 0xff, ((ip) >> 8) & 0xff, (ip) & 0xff

typedef enum {
   MIGRATE_PROGRESS_FAILURE,
   MIGRATE_PROGRESS_UPDATE,
   MIGRATE_PROGRESS_SUSPEND,
   MIGRATE_PROGRESS_POWEROFF,
   MIGRATE_PROGRESS_CONTINUE,
   MIGRATE_PROGRESS_RESUMED_OK,
} MigrateProgressMsg;

typedef struct MigrateCallBlock{
   VMK_ReturnStatus     status;
   int                  preCopyPhase;
   int                  pagesSent;
   int                  pagesTotal;
} MigrateCallBlock;

typedef enum {
   MIGRATE_PRECOPY_START,
} MigrateChannelMsgs;

typedef enum {
   MIGRATE_NOT_INITIALIZED = 0,
   MIGRATE_OFF_REQUESTED,
   MIGRATE_ON_REQUESTED,
   MIGRATE_PRECOPY,
   MIGRATE_QUIESCE,
   MIGRATE_CPTXFER,
   MIGRATE_CPTLOAD,
   MIGRATE_PAGEIN,
   MIGRATE_COMPLETE,
   MIGRATE_FAILED,
} MigrateState;

#endif
