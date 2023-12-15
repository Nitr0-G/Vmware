/* **********************************************************
 * Copyright 2002 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * Table of log levels, to be included as needed (typically twice:
 * for the enum declaration, and for a string table for printing).
 *
 * The first parameter to LOGLEVEL_DEF is the loglevel name, the second
 * is the default log level.
 */

LOGLEVEL_DEF(LinBlock, 0)
LOGLEVEL_DEF(LinChar,  0)
LOGLEVEL_DEF(LinNet, 0)
LOGLEVEL_DEF(LinProc, 0)
LOGLEVEL_DEF(LinSCSI, 0)
LOGLEVEL_DEF(LinStubs, 0)
LOGLEVEL_DEF(LinStress, 0)
LOGLEVEL_DEF(Bond, 0)
LOGLEVEL_DEF(LinPCI, 0)
LOGLEVEL_DEF(Tcpip_Support,0)
LOGLEVEL_DEF(LinSoftIrq, 0)

#undef LOGLEVEL_DEF
