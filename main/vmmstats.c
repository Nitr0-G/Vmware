/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * vmmstats.c --
 *
 *	Allow access to monitor stats via /proc nodes
 *
 */

/*
 * Includes
 */

#include "vmkernel.h"
#include "vmmstats.h"
#include "world.h"
#include "proc.h"
#include "stats_shared.h"

/*
 *-----------------------------------------------------------------------------
 *
 * VMMStatsWorldProcRead --
 *
 *     Proc read handler to display data for the vcpu's monitor statistics.
 *     The printed results do not include names, only stat numbers. You
 *     must post-process the output to get descriptive name.
 *
 *     Note that the world (stored in entry->private) MUST be a VMM world.
 *
 * Results:
 *     Returns VMK_OK on success, or VMK_BAD_PARAM if the world does not exist
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static int
VMMStatsWorldProcRead(Proc_Entry *entry, char *buffer, int *len)
{
   int i;
   World_Handle *world;
   StatsEntry *monitorStats;

   *len = 0;

   // get the world handle and make sure it doesn't go away
   world = (World_Handle*) entry->private;
   if (World_Find(world->worldID) == NULL) {
      // this world has been deallocated
      return (VMK_BAD_PARAM);
   }

   ASSERT(World_IsVMMWorld(world));
   monitorStats = world->vmkSharedData->monitorStats;
 
   for (i=0; i < VMMVMK_MAX_STATS; i++) {
      Proc_Printf(buffer, len, "%3d  %10u\n", 
                  i,
                  monitorStats[i].count);
   }
   
   World_Release(world);

   return(VMK_OK);
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMMStats_WorldInit --
 *
 *     Setup the vmmstats proc node for this world
 *
 * Results:
 *     Returns VMK_OK
 *
 * Side effects:
 *     Creates /proc/vmware/<vmid>/.vmmstats
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus 
VMMStats_WorldInit(World_Handle *world, World_InitArgs *args)
{
   if (World_IsVMMWorld(world)) {
      Proc_InitEntry(&World_VMM(world)->procVMMStats);
      World_VMM(world)->procVMMStats.parent = &world->procWorldDir;
      World_VMM(world)->procVMMStats.read = VMMStatsWorldProcRead;
      World_VMM(world)->procVMMStats.private = (void*)world;
      Proc_Register(&World_VMM(world)->procVMMStats, ".vmmstats", FALSE);   
   }

   return VMK_OK;
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMMStats_WorldCleanup --
 *
 *     Uninstalls vmmstats proc node for this world
 *
 * Results:
 *     None
 *
 * Side effects:
 *     Removes /proc/vmware/<vmid>/.vmmstats
 *
 *-----------------------------------------------------------------------------
 */
void 
VMMStats_WorldCleanup(World_Handle *world)
{
   if (World_IsVMMWorld(world)) {
      Proc_Remove(&World_VMM(world)->procVMMStats);
   }
}


