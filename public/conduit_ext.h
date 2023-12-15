/* **********************************************************
 * Copyright 2004 Vmware, Inc.  All rights reserved. -- Vmware Confidential
 * **********************************************************/

#ifndef _CONDUIT_EXT_H_
#define _CONDUIT_EXT_H_



#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMK_MODULE
#include "includeCheck.h"

#include "conduit_dist.h"
#include "splock.h"
#include "list.h"


/*
 *
 *  Conduit_WorldInfo is embedded in the World_GroupInfo structure.
 *  where it provides a hard point for each world to get at per-world 
 *  conduit structures.
 *
 *  All conduit devices are kept in system global structures, connections
 *  to these device however are world specific.  Connections to conduit
 *  devices are embodied in conduit objects.  Pointers to these objects
 *  are kept in per-world Conduit_Directories.  Each world is capable of
 *  opening conduit adapters.  These adapters provide conduit card support
 *  including access to an emulated bus memory region.  This memory is
 *  used by conduit objects allowing shared memory contact between the
 *  conduit client and its device.  Conduit_AdapterDevMem points to the 
 *  structures associated with conduit adapter bus/card memory support. 
 *  The num/max/cur Handles fields are used to interact with the conduit
 *  directory.
 *
 */

typedef struct Conduit_WorldInfo {
   List_Links               conduitWorlds; /* Conduit_WorldInfo *next */
   int                      numHandles;
   int                      maxHandle;
   int                      curHandle;
   Bool                     enabled;
   SP_RWLock                adapterLock;
   Conduit_Directory        *conduits;
   List_Links               adapterDev; /* Conduit_AdapterDevMem  adapterDev */
   MPN                      unmappedFrame;
} Conduit_WorldInfo;


#endif /* _CONDUIT_EXT_H_ */
