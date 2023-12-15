/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

#include "vmkernel.h"
#include "identity.h"
#include "world.h"

#define LOGLEVEL_MODULE Identity
#define LOGLEVEL_MODULE_LEN 8
#include "log.h"

/*
 *----------------------------------------------------------------------
 *
 * Identity_WorldInit --
 *
 *      Per-world initialization of identity state.  The default state
 *      for a new world is appropriate for a kernel thread: all uids
 *      and primary gids are 0 (root), and there are no supplementary
 *      gids.
 *
 * Results:
 *	VMK_OK
 *
 * Side effects:
 *      world->ident is initialized
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Identity_WorldInit(World_Handle *world, World_InitArgs *args)
{
   memset(&world->ident, 0, sizeof(world->ident));
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Identity_WorldCleanup --
 *
 *      Per-world cleanup of identity state
 *
 * Results:
 *	VMK_OK
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
Identity_WorldCleanup(World_Handle *world)
{
   // Nothing needs to be cleaned up.
}


/*
 *----------------------------------------------------------------------
 *
 * Identity_Copy --
 *
 *      Copy an identity
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      Modifies *dst
 *
 *----------------------------------------------------------------------
 */
void
Identity_Copy(Identity *dst, const Identity *src)
{
   dst->ruid = src->ruid;
   dst->euid = src->euid;
   dst->suid = src->suid;
   dst->rgid = src->rgid;
   dst->egid = src->egid;
   dst->sgid = src->sgid;
   ASSERT(src->ngids <= IDENTITY_NGROUPS_MAX);
   memcpy(&dst->gids, &src->gids, sizeof(Identity_GroupID) * src->ngids);
   dst->ngids = src->ngids;
}
