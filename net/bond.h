/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. 
 * -- VMware Confidential
 * **********************************************************/

/* *****************************************************************
 *
 * bond.h --
 *
 *     Header file for bond.c      
 *
 *****************************************************************
 *
 */

#ifndef _BOND_H_
#define _BOND_H_

#define MAX_SLAVE_NUM                           4
#define INVALID_SLAVE_NUM                       0x100
#define PROTOTYPE_BOND_PORT_NUM                 64
#define NICTEAMING_MAX_SLAVE_NUM	        10

typedef struct Slave
{
   int index;
   int uplinkPort;
   char uplinkName[VMNIX_DEVICE_NAME_LENGTH];
   Bool connected;
   // XXX: to do: add a list of handles hashed onto this slave now
} Slave;

typedef struct Bond
{
   List_Links           listLinks;

   uint32               totalSlaveCount;

   // as an upper dev (e.g., portset)
   Slave                slave[MAX_SLAVE_NUM];

   // as a bottom dev (e.g., nic)
   char devName[VMNIX_DEVICE_NAME_LENGTH];
   void *uplinkDev;
   Proc_Entry           *configEntry;
   Portset              *portset;
   Bool                 inList;
   uint8                refCount;
} Bond;

typedef struct BondList {
   List_Links bondList;
} BondList;

VMK_ReturnStatus Bond_ModInit(void);
VMK_ReturnStatus Bond_ModCleanup(void);
#endif
