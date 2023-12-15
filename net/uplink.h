/* **********************************************************
 * Copyright (C) 2004 VMware, Inc. All Rights Reserved
 * -- VMware Confidential.
 * **********************************************************/


/*
 * uplink.h --
 *
 *    Implementation of the uplink calls. The uplink layer goes between the
 *    portset and the device to which it is connected.
 */

#ifndef _UPLINK_H_
#define _UPLINK_H_
#include "return_status.h"
#include "net_driver.h"
#include "list.h"

typedef enum UplinkStatus {
   UPLINK_DOWN,
   UPLINK_UP,
} UplinkStatus;


/* Device specific data that needs to be passed up to the portset */
typedef struct UplinkData {
   size_t         pktHdrSize;
   size_t         maxSGLength;
   IDT_Handler    intrHandler;
   void          *intrHandlerData;
#define INVALID_VECTOR -1
   int            intrHandlerVector;
} UplinkData;

// four distinct namespaces
typedef enum DeviceType {
   DEVICE_TYPE_DEVICE_LEAF      = 0x1,
   DEVICE_TYPE_DEVICE_BOND      = 0x2,
   DEVICE_TYPE_DEVICE_UNKNOWN   = 0x3, // LEAF | BOND
   DEVICE_TYPE_PORTSET_TOPLEVEL = 0x4,
   DEVICE_TYPE_PORTSET_BOND     = 0x8,
} DeviceType;

typedef VMK_ReturnStatus NotifyFn(PortID, UplinkData *,
                                  UplinkStatus);

typedef struct UplinkConnectArgs {
   char devName[VMNIX_DEVICE_NAME_LENGTH];
   void *uplinkImpl; /* pointer to uplink specific context */
   Net_Functions *functions;
   size_t pktHdrSize;
   size_t maxSGLength;
   DeviceType type;
   int32 moduleID;   /* valid only for leaf(vmnic) devices */
} UplinkConnectArgs;

typedef struct UplinkDevice {
   void          *netDevice; // the vmklinux device

   // name of the vmkernel device (vmnic0, bond0, ...)
   char           devName[VMNIX_DEVICE_NAME_LENGTH];

   enum DeviceFlags {
      DEVICE_AVAILABLE = 0x1,    // set if device hasn't been claimed by a portset.
      DEVICE_PRESENT   = 0x2,    // set if the device is present and initialized
      DEVICE_OPENED    = 0x4,    // has dev->functions->open been called?
      /* has the portset been notified of the device coming up? */
      DEVICE_EVENT_NOTIFIED = 0x8,
   } flags;

   PortID uplinkPort;        // The uplink port associated with the device
   NotifyFn *notifyFn;       // The notification function
   Net_Functions *functions; // device specific functions

   /*
    * Portset visible data passed as paramater to the portset notification
    * function.
    */
   UplinkData     uplinkData;
   int32          moduleID;      // vmkmodule id for this device
   uint32         hwCap;         // hw capabilities of the device
   uint32         swCap;         // sw capabilities of the device
} UplinkDevice;


typedef UplinkDevice NetUplinkDevice; // external name

VMK_ReturnStatus Uplink_DeviceConnected(const char *, void *, int32,
                                        Net_Functions *, size_t, size_t,
                                        void **);
VMK_ReturnStatus Uplink_Register(PortID, char *, DeviceType, NotifyFn, UplinkData **);
VMK_ReturnStatus Uplink_Unregister(PortID, char *);
void Uplink_DeviceDisconnected(char *);
VMK_ReturnStatus Uplink_ModEarlyInit(void);
VMK_ReturnStatus Uplink_ModCleanup(void);
VMK_ReturnStatus Uplink_ModInit(void);
void Uplink_PostModuleInit(void *data);
void Uplink_RegisterCallbacks(UplinkDevice *);
void Uplink_SetupIRQ(void *, uint32, IDT_Handler, void *);
void * Uplink_GetImpl(const char *);
void Uplink_DoDeviceDisconnected(UplinkDevice *dev);
VMK_ReturnStatus Uplink_SetDeviceConnected(UplinkConnectArgs *args, void **uplinkDev);
void Uplink_DeviceOpen(UplinkDevice *dev);
VMK_ReturnStatus Uplink_RequestCapability(PortID, uint32);
void Uplink_RemoveCapability(PortID, uint32);

#endif //_UPLINK_H_
