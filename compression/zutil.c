/* zutil.c -- target dependent utility functions for the compression library
 * Copyright (C) 1995-2002 Jean-loup Gailly.
 * For conditions of distribution and use, see copyright notice in zlib.h 
 */

/* @(#) $Id$ */

#include "zutil.h"
#include "memalloc.h"

struct internal_state      {int dummy;}; /* for buggy compilers */

#ifndef STDC
extern void exit OF((int));
#endif

const char *z_errmsg[10] = {
"need dictionary",     /* Z_NEED_DICT       2  */
"stream end",          /* Z_STREAM_END      1  */
"",                    /* Z_OK              0  */
"file error",          /* Z_ERRNO         (-1) */
"stream error",        /* Z_STREAM_ERROR  (-2) */
"data error",          /* Z_DATA_ERROR    (-3) */
"insufficient memory", /* Z_MEM_ERROR     (-4) */
"buffer error",        /* Z_BUF_ERROR     (-5) */
"incompatible version",/* Z_VERSION_ERROR (-6) */
""};


const char * ZEXPORT zlibVersion()
{
    return ZLIB_VERSION;
}

#ifdef DEBUG

#  ifndef verbose
#    define verbose 0
#  endif
int z_verbose = verbose;

void z_error (m)
    char *m;
{
    fprintf(stderr, "%s\n", m);
    exit(1);
}
#endif

/* exported to allow conversion of error code to string for compress() and
 * uncompress()
 */
const char * ZEXPORT zError(err)
    int err;
{
    return ERR_MSG(err);
}


#include "vmkernel_dist.h"
voidpf zcalloc (opaque, items, size)
    voidpf opaque;
    unsigned items;
    unsigned size;
{
   uint32 totalSize = items * size;
   uint8 *p;

   if (opaque) items += size - size; /* make compiler happy */
   p = (uint8*)Mem_Alloc(totalSize);
   if (p) {
      memset(p, 0, totalSize);
   }
#ifdef VMX86_DEBUG
   _Log("Compress: zcalloc %p size=%d ra=%p:%p\n", p, totalSize,
        __builtin_return_address(0), __builtin_return_address(1));
#endif

   return (voidpf)p;
}

void  zcfree (opaque, ptr)
    voidpf opaque;
    voidpf ptr;
{
#ifdef VMX86_DEBUG
   _Log("Compress: zfree %p\n", ptr);
#endif
   Mem_Free(ptr);
   if (opaque) return; /* make compiler happy */
}


