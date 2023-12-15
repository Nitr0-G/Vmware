/* **********************************************************
 * Copyright (C) 2004 VMware, Inc. All Rights Reserved.
 * -- VMware Confidential.
 * **********************************************************/


/* UplinkTable.h --
 *
 *      Implements the uplink data structures.
 *
 */

#ifndef _UPLINK_TABLE_H_
#define _UPLINK_TABLE_H_

typedef struct UplinkTable {
   List_Links uplinks;
} UplinkTable;


/*
 *----------------------------------------------------------------------------
 *
 *  UplinkTable_Init --
 *
 *    Initialize the uplink table data structure.
 *
 *  Results:
 *    None.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE void
UplinkTable_Init(UplinkTable *uplinkTable)
{
   ASSERT(uplinkTable);
   List_Init(&uplinkTable->uplinks);
}


/*
 *----------------------------------------------------------------------------
 *
 *  UplinkTable_Free --
 *
 *    Free all the entries in the given uplink table.
 *
 *  Results:
 *    None.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE void
UplinkTable_Free(UplinkTable *uplinkTable)
{
   List_Links *curEntry;
   ASSERT(uplinkTable);
   while (!List_IsEmpty(&uplinkTable->uplinks)) {
      curEntry = List_First(&uplinkTable->uplinks);
      List_Remove(curEntry);
      Mem_Free(curEntry);
   }
}


/*
 *----------------------------------------------------------------------------
 *
 *  UplinkTable_Find --
 *
 *    Search for the given device in the specified uplink table.
 *
 *  Results:
 *    VMK_OK if the device was found. VMK_FAILURE otherwise. The UplinkDevice
 *    is returned in the parameter device.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE VMK_ReturnStatus
UplinkTable_Find(UplinkTable *uplinkTable, const char *devName,
                 UplinkDevice **device)
{
   VMK_ReturnStatus ret = VMK_FAILURE;
   List_Links *entry;
   UplinkDevice *dev;
   ASSERT(uplinkTable);
   ASSERT(device);
   *device = NULL;
   LIST_FORALL(&uplinkTable->uplinks, entry) {
      dev = (UplinkDevice *)entry;
      if (strcmp(dev->devName, devName) == 0) {
         *device = dev;
         ret = VMK_OK;
         break;
      }
   }
   return ret;
}


/*
 *----------------------------------------------------------------------------
 *
 *  UplinkTable_Add --
 *
 *    Add the device to the uplink table.
 *
 *  Results:
 *    None.
 *
 *  Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE void
UplinkTable_Add(UplinkTable *uplinkTable, UplinkDevice *dev)
{
   ASSERT(uplinkTable);
   ASSERT(dev);
   List_InitElement(&dev->listLinks);
   List_Insert(&dev->listLinks, LIST_ATFRONT(&uplinkTable->uplinks));
}


#endif //_UPLINK_TABLE_H_
