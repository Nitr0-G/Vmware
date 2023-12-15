/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * objectCache.h --
 *
 *    File system object cache
 */


#ifndef _OBJECTCACHE_H
#define _OBJECTCACHE_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "return_status.h"
#include "fs_ext.h"
#include "fss_int.h"
#include "fsSwitch.h"


#define OC_DEFAULT_NUM_BUCKETS 8192

#define OC_VOLUMES 0x1
#define OC_OBJECTS 0x2


VMK_ReturnStatus OC_Init(void);

void OC_Lock(uint32 type);
void OC_Unlock(uint32 type);

VMK_ReturnStatus OC_LookupObject(FSS_ObjectID *oid,
				 Bool reserveIfFound, Bool getLock,
				 ObjDescriptorInt **desc);
VMK_ReturnStatus OC_ReserveObject(FSS_ObjectID *oid, ObjDescriptorInt **desc);
VMK_ReturnStatus OC_ReleaseObject(ObjDescriptorInt *desc);

VMK_ReturnStatus OC_LookupVolume(FSS_ObjectID *oid, Bool reserveIfFound,
				 ObjDescriptorInt **desc);
VMK_ReturnStatus OC_ReserveVolume(FSS_ObjectID *oid, ObjDescriptorInt **desc);
VMK_ReturnStatus OC_ReleaseVolume(ObjDescriptorInt *desc);

VMK_ReturnStatus OC_CreateObjectDesc(ObjDescriptorInt **desc);
VMK_ReturnStatus OC_DestroyObjectDesc(ObjDescriptorInt *desc);

// use is discouraged
VMK_ReturnStatus OC_InsertVolume(ObjDescriptorInt *desc);
VMK_ReturnStatus OC_RemoveVolume(ObjDescriptorInt *desc, Bool getLock);



#endif
