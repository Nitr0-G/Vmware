/* **********************************************************
 * Copyright (C) 2004 VMware, Inc. All Rights Reserved
 * - VMware Confidential.
 * **********************************************************/

/*
 * cos_vmkdev_public.h
 *
 *    Interface to vmkernel networking client for the COS.
 */


#ifndef _COS_VMKDEV_PUBLIC_H_
#define _COS_VMKDEV_PUBLIC_H_

//XXX: fix version number
static const uint32 COSVMKDEV_VERSION = 0xBADC0DE;

static const uint32 COSVMKDEV_MAX_TX_RINGS     = 128;
static const uint32 COSVMKDEV_MAX_RX_RINGS     = 128;
static const uint32 COSVMKDEV_MAX_STATE_RANGES = 3;

typedef enum COSVMKDev_RxState {
   COSVMKDEV_RX_INVALID,
   COSVMKDEV_RX_AVAIL,
   COSVMKDEV_RX_USED,
} COSVMKDev_RxState;

typedef enum COSVMKDev_TxState {
   COSVMKDEV_TX_INVALID,
   COSVMKDEV_TX_AVAIL,
   COSVMKDEV_TX_START,
   COSVMKDEV_TX_IN_PROGRESS,
   COSVMKDEV_TX_DONE,
} COSVMKDev_TxState;

typedef enum COSVMKDev_Status {
   COSVMKDEV_TX_FAILED,
   COSVMKDEV_TX_OK,
   COSVMKDEV_RX_FAILED,
   COSVMKDEV_RX_OK,
} COSVMKDev_Status;

typedef struct COSVMKDev_RxEntry {
   MA                 maddr;    // MA of the data buffer
   uint32             bufLen;   // length of the buffer
   uint32             dataLen;  // length of actual data
   void              *ctx;      // context for the COS driver
   COSVMKDev_RxState  rxState;  // track ownership/progress of the frame
   COSVMKDev_Status   status;   // what happened to this entry?
} COSVMKDev_RxEntry;


typedef struct COSVMKDev_TxEntry {
   NetSG_Array        sg;       // data buffers to be transmitted
   uint32             dataLen;  // total length of data in the sg
   void              *ctx;      // COS driver's context for this frame
   COSVMKDev_TxState  txState;  // track ownership/progress of the frame
   COSVMKDev_Status   status;   // what happened to this entry?
} COSVMKDev_TxEntry;

#define NUM_COSVMKDEV_EXPL_MULTICAST 16
typedef struct COSVMKDev_State {
   uint32      version;         // version for sanity checking
   uint32      length;          // length of the shared data
   uint8       macAddr[6];      // unicast MAC for RX filter
   uint32      ifflags;         // bsd style interface flags
   uint16      numMulticast;    // *total* number (including LADRF)
   uint8       multicastAddrs[NUM_COSVMKDEV_EXPL_MULTICAST][6];
   uint32      ladrf[2];        // used only on overflow of the above array
   uint32      numRxBuffers;    // total number of receive buffers
   uint32      numTxBuffers;    // total number of transmit buffers
   uint32      txRingOffset;    // start of the transmit ring
   uint32      rxRingOffset;    // start of the receive ring
   Bool        stopQueue;       // is the host's (COS's) queue stopped
} COSVMKDev_State;


#endif // _COS_VMKDEV_PUBLIC_H_
