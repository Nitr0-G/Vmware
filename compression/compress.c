/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * compress.c --
 *
 *	Interface to zlib compression routines.
 */

#include "vmkernel.h"
#include "vm_libc.h"
#include "compress.h"

#define LOGLEVEL_MODULE Compress
#include "log.h"


/*
 *----------------------------------------------------------------------
 *
 * Compress_Start --
 *
 *      Initialize the given compression stream context.  Allow the user to
 *      override default memoroy allocation and free routines.
 *
 * Results:
 *      VMK_ReturnStatus
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Compress_Start(CompressContext *cc, alloc_func allocFn, free_func freeFn,
              uint8* buf, uint32 bufSize, OutputFunc *fn, void *fnArg)
{
   int err;

   memset(cc, 0, sizeof (CompressContext));
   cc->buf = buf;
   cc->bufSize = bufSize;
   cc->fn = fn;
   cc->fnArg = fnArg;

   cc->zStream.zalloc = allocFn;
   cc->zStream.zfree = freeFn;
   cc->zStream.opaque = NULL;

   err = deflateInit(&cc->zStream, Z_DEFAULT_COMPRESSION);
   if (err != Z_OK) {
      Warning("deflateInit returned %d:%s", err, zError(err));
      return VMK_FAILURE;
   }

   cc->zStream.next_out = buf;
   cc->zStream.avail_out = bufSize;

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * CompressCheckOutput --
 *
 *      Check if the compression stream output is full, and if so, flush it
 *      out by calling the stream's output function.  Also, reset the
 *      stream to the start of the output buffer.
 *      If the boolean flush is TRUE, the buffer may not actually be full, but
 *      someone requested a flush, so don't reset the buffer and inform the
 *      output function that this is a partial write.
 *
 * Results:
 *      VMK_ReturnStatus
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE VMK_ReturnStatus
CompressCheckOutput(CompressContext *cc, Bool flush)
{
   if (cc->zStream.avail_out == 0) {
      VMK_ReturnStatus status = (*cc->fn)(cc->fnArg, FALSE);
      if (status != VMK_OK) {
         return status;
      }
      
      cc->zStream.next_out = cc->buf;
      cc->zStream.avail_out = cc->bufSize;
   } else if (flush) {
      VMK_ReturnStatus status = (*cc->fn)(cc->fnArg, TRUE);
      if (status != VMK_OK) {
         return status;
      }
   }

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Compress_AppendData --
 *
 *      Add the given data to the compression stream.
 *
 * Results:
 *      VMK_ReturnStatus
 *
 * Side effects:
 *      May end up flushing output buffer
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Compress_AppendData(CompressContext *cc, uint8* addr, uint32 size)
{
   cc->zStream.next_in = addr;
   cc->zStream.avail_in = size;
   ASSERT(cc->zStream.avail_out != 0);

   LOG(1, "pre in=(%p,%d,%ld) out=(%p,%d,%ld)",
       cc->zStream.next_in, cc->zStream.avail_in, cc->zStream.total_in,
       cc->zStream.next_out, cc->zStream.avail_out, cc->zStream.total_out);
   
   while (cc->zStream.avail_in != 0) {
      VMK_ReturnStatus status;
      int err = deflate(&cc->zStream, Z_NO_FLUSH);
      if (err != Z_OK) {
         Warning("deflate returned %d:%s", err, zError(err));
         return VMK_FAILURE;
      }

      status = CompressCheckOutput(cc, FALSE);
      if (status != VMK_OK) {
         return status;
      }
   }

   LOG(1, "post in=(%p,%d,%ld) out=(%p,%d,%ld)",
       cc->zStream.next_in, cc->zStream.avail_in, cc->zStream.total_in,
       cc->zStream.next_out, cc->zStream.avail_out, cc->zStream.total_out);

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Compress_Flush --
 *
 *      Flush the compressoin buffers to output device.
 *      Also, write the total compressed size in the compressedSize parameter.
 *
 * Results:
 *      VMK_ReturnStatus
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Compress_Flush(CompressContext *cc, uint32 *compressedSize)
{
   VMK_ReturnStatus status;
   int err;

   LOG(1, "pre in=(%p,%d,%ld) out=(%p,%d,%ld)",
       cc->zStream.next_in, cc->zStream.avail_in, cc->zStream.total_in,
       cc->zStream.next_out, cc->zStream.avail_out, cc->zStream.total_out);
   
   while (1) {
      Bool done = FALSE;
      err = deflate(&cc->zStream, Z_SYNC_FLUSH);
   
      if (err != Z_OK) {
         Warning("deflate returned %d:%s", err, zError(err));
         return VMK_FAILURE;
      }
      if (cc->zStream.avail_out != 0) {
         /*
          * there is some remaining space in the output buffer, so there's
          * no need to call deflate again.  But, before exiting, we have to
          * write out the compressed data already in the buffer.
          */
         done = TRUE;
      }
      status = CompressCheckOutput(cc, TRUE);
      if (status != VMK_OK) {
         return status;
      }
      if (done) {
         break;
      }
   }
   if (compressedSize) {
      *compressedSize = cc->zStream.total_out;
   }

   LOG(1, "post in=(%p,%d,%ld) out=(%p,%d,%ld)",
       cc->zStream.next_in, cc->zStream.avail_in, cc->zStream.total_in,
       cc->zStream.next_out, cc->zStream.avail_out, cc->zStream.total_out);

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Compress_Finish --
 *
 *      Mark the end of the compression stream, flush all buffers, and
 *      release any allocated memory.  Also, write the total compressed
 *      size in the compressedSize parameter.
 *
 * Results:
 *      VMK_ReturnStatus
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Compress_Finish(CompressContext *cc, uint32 *compressedSize)
{
   VMK_ReturnStatus status;
   int err;

   LOG(1, "pre in=(%p,%d,%ld) out=(%p,%d,%ld)",
       cc->zStream.next_in, cc->zStream.avail_in, cc->zStream.total_in,
       cc->zStream.next_out, cc->zStream.avail_out, cc->zStream.total_out);
   
   // set compressedSize so far (we may fail while flushing data)
   if (compressedSize) {
      *compressedSize = cc->zStream.total_out;
   }
   while (1) {
      err = deflate(&cc->zStream, Z_FINISH);
      if ((err != Z_OK) && (err != Z_STREAM_END)) {
         Warning("deflate returned %d:%s", err, zError(err));
         return VMK_FAILURE;
      }

      status = CompressCheckOutput(cc, TRUE);
      if (status != VMK_OK) {
         return status;
      }
      if (err == Z_STREAM_END) {
         break;
      }
   }
   err = deflateEnd(&cc->zStream);
   if (err != Z_OK) {
      Warning("deflateEnd returned %d:%s", err, zError(err));
      return VMK_FAILURE;
   }
   
   if (compressedSize) {
      *compressedSize = cc->zStream.total_out;
   }

   LOG(1, "post in=(%p,%d,%ld) out=(%p,%d,%ld)",
       cc->zStream.next_in, cc->zStream.avail_in, cc->zStream.total_in,
       cc->zStream.next_out, cc->zStream.avail_out, cc->zStream.total_out);

   return VMK_OK;
}

