/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * Table of events sent from the vmkernel to serverd to be included
 * as needed.
 */

/*
 * vmkernel was loaded/unloaded.  In practice, this event isn't sent from the
 * vmkernel since the event channel can't exist unless the vmkernel is already
 * loaded.  Also, unloading the vmkernel breaks the channel.  However, this is
 * a valid vmkernel event.  For completeness, we define a message here.
 */
UEVENT(VMK_LOAD)

/* Module was loaded/unloaded. */
UEVENT(MODULE_LOAD)

/* Network settings were changed */
UEVENT(NETWORK)

/* VMFS volume appeared/dissappeared */
UEVENT(VMFS)

/* General alert coming from the vmkernel */
UEVENT(ALERT)

/* HBA device rescan indicates new or missing disks */
UEVENT(UPDATE_DISKS)

/* Progress update on a VMotion */
UEVENT(MIGRATE_PROGRESS)

/* A disk commit has finished */
UEVENT(COMMIT_DONE)

/* Create a vmm core dump */
UEVENT(REQUEST_VMMCOREDUMP)

/* Request for the vmx to execute some tcl code */
UEVENT(REQUEST_TCLCMD)

/* receiver should exit */
UEVENT(EXIT)

/* receiver should panic */
UEVENT(PANIC)

/* Create a vmx core dump */
UEVENT(REQUEST_VMXCOREDUMP)

#undef UEVENT
