/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * compress.h --
 *
 *      vmkernel compression module interface (uses zlib)
 */

#ifndef _VMK_COMPRESS_H_
#define _VMK_COMPRESS_H_

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "zlib.h"

typedef VMK_ReturnStatus (OutputFunc)(void *arg, Bool partial);

typedef struct CompressContext {
   z_stream zStream;
   uint8 *buf;
   uint32 bufSize;
   OutputFunc *fn;
   void *fnArg;
} CompressContext;

extern VMK_ReturnStatus Compress_Start(CompressContext *cc,
                                       alloc_func allocFn, free_func freeFn,
                                       uint8* buf, uint32 bufSize,
                                       OutputFunc *fn, void *fnArg);
extern VMK_ReturnStatus Compress_AppendData(CompressContext *cc,
                                            uint8* addr, uint32 size);

extern VMK_ReturnStatus Compress_Flush(CompressContext *cc, uint32 *compressedSize);

extern VMK_ReturnStatus Compress_Finish(CompressContext *cc, uint32 *compressedSize);

#endif // _VMK_COMPRESS_H_
