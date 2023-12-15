/************************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. 
 * -- VMware Confidential
 ************************************************************/

/*
 * heartbeat.h --
 *
 *   This is the header for the heartbeat reliability item.
 *
 */

#ifndef  _HEARTBEAT_H
#define _HEARTBEAT_H
#define INCLUDE_ALLOW_VMKERNEL

void Heartbeat_Init(void);
VMK_ReturnStatus Heartbeat_WorldInit(World_Handle *);
void Heartbeat_WorldCleanup(World_Handle *);

#endif
