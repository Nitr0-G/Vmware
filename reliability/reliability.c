/************************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. 
 * -- VMware Confidential
 ***********************************************************/

/*
 * reliability.c --
 *
 *      This module calls the init functions of various reliability
 *      modules.
 *
 */

#include "vmkernel.h"
#include "proc_dist.h"
#include "world.h" 
#include "heartbeat.h"


/* Function declarations */

VMK_ReturnStatus Reliability_WorldInit(World_Handle *, World_InitArgs *);
void Reliability_WorldCleanup(World_Handle *);
void Reliability_Init(void);

/*
 *----------------------------------------------------------------------
 *
 * Reliability_WorldInit --
 *
 *      Initialize the reliability data associated with VMM worlds.
 *      Calls various WorldInit functions of various reliability
 *      items.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
Reliability_WorldInit(World_Handle *world, 
                      World_InitArgs *args)
{
   /* initialize heartbeat data for the world */
   return Heartbeat_WorldInit(world);
}


/*
 *----------------------------------------------------------------------
 *
 * Reliability_WorldCleanup --
 *
 *      Cleanup the reliability data associated with the "world".
 *      Calls various WorldCleanup functions of various reliability
 *      items.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
Reliability_WorldCleanup(World_Handle *world)
{
   /* clean up heartbeat data for the world */
   Heartbeat_WorldCleanup(world);
}


/*
 *----------------------------------------------------------------------
 *
 * Reliability_Init --
 *
 *      Calls the init functions of various reliabilty items,each
 *      item implementing a reliabilty feature.
 * 
 * Results:
 *      none
 *
 * Side effects: 
 *      none
 *----------------------------------------------------------------------
 */

void
Reliability_Init(void)
{

   /* Initialise the reliability items */
   Heartbeat_Init();    //detects cpu lockups
}
