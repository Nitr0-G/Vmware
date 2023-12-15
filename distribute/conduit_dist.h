/* **********************************************************
 * Copyright 2004 Vmware, Inc.  All rights reserved. -- Vmware Confidential
 * **********************************************************/

#ifndef _CONDUIT_DIST_H_
#define _CONDUIT_DIST_H_



#include "conduit_def.h"

#define INCLUDE_ALLOW_VMMEXT
#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMK_MODULE
#define INCLUDE_ALLOW_DISTRIBUTE
#define INCLUDE_ALLOW_USERLEVEL
#include "includeCheck.h"


typedef uint32 Conduit_HandleID;


typedef struct ConduitInfo {
   uint32		version;	/* magic number for this version */
   uint32		headerSize;	/* buffer handling behavior fields */
   uint32		type;
   uint32		flags;
   Conduit_BufTopo	bufTopo;
   uint32		target;
   void 		*pseudoDevInfo;
   uint32		pDevSize;
   uint32		exclusive;	/* bool for underlying dev connection */
} ConduitInfo;


#define CONDUIT_CI_TRBUF_OFFSET(buffer) \
	(uint32)(((ConduitInfo *)(buffer))->headerSize) 

#define CONDUIT_CI_RCVBUF_OFFSET(buffer) \
	(uint32)(((ConduitInfo *)(buffer))->headerSize +  \
		(buffer->bufTopo.numXmitBuffers * buffer->bufTopo.xmitBufSize)) 


// arguments for ConduitHandleEnable call
typedef struct Conduit_HandleEnableArgs {
   PA pAddr;
   uint32 pLen;
   uint8 *checkpointBuffer;
   int32  checkpointLength;
   uint32 flags;
   uint32 vmkChannelPending;
   uint32 vmkChannelIntr;
} Conduit_HandleEnableArgs;


// arguments for Conduit_NewPipe call
typedef struct Conduit_NewPipeArgs {
   Conduit_DriverData 	*pipe;
   char			dev_name[CONDUIT_DEV_NAME_LENGTH];
   uint32 		returnStatus;
   Conduit_HandleID 	handleID;
   PA 			pAddr;
   uint32 		pLen;
   uint32		flags;
   uint8		*checkpointBuffer;
   int32		checkpointLength;
   uint32		vmkChannelPending;
   uint32		vmkChannelIntr;
   Conduit_DriverData	dd;
} Conduit_NewPipeArgs;



#define CONDUIT_REMOVE_PIPE_SHARED_BUFFER 0x1


typedef enum {
   CONDUIT_HANDLE_HOST,
   CONDUIT_HANDLE_VMM,
   CONDUIT_HANDLE_RAW
} Conduit_ClientType;



typedef struct Conduit_Directory Conduit_Directory;
typedef struct Conduit_AdapterDevMem Conduit_AdapterDevMem;


typedef enum {
   CONDUIT_LOCK_PAGE = 1,
   CONDUIT_UNLOCK_PAGE,
} Conduit_LockPageFlags;





#endif /* _CONDUIT_DIST_H_ */
