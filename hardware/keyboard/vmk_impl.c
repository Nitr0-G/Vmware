/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * vmk_impl.c --
 *
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "vmkernel_dist.h"
#include "vmkernel.h"
#include "misc.h"
#include "vmk_impl.h"
#include "util.h"
#include "memalloc_dist.h"

#define LOGLEVEL_MODULE VMKKBD
#define LOGLEVEL_MODULE_LEN 0
#include "log.h"

void
clist_alloc_cblocks(clist *clistp, 
                    UNUSED_PARAM(u_int maxSize), 
                    UNUSED_PARAM(u_int reservedSize))
{
   memset(clistp, 0, sizeof(*clistp));
   clistp->numChars = MAX_KBD_CHAR;
}

u_int 
clist_inc_index(clist *clistp, 
                u_int ndx) 
{
   return (ndx + 1) % clistp->numChars;
}

u_int
q_to_b(clist *clistp,
       char *buf,
       u_int size)
{
   uint32 numCopied = 0;
   uint32 i;
   for (i = 0; i < size; i++) {
      if (clistp->drain == clistp->fill) {
         // LOG(0, "no character list empty");
         return numCopied;
      }
      buf[i] = clistp->c[clistp->drain];
      clistp->drain = clist_inc_index(clistp, clistp->drain);
      numCopied++;
      LOG(1, "Keyboard char <%c>", buf[i]);
   }
   return numCopied;
}


int 
putc(int chr, 
     clist *clistp)
{
   uint32 nextFill = clist_inc_index(clistp, clistp->fill);

   LOG(1, "adding char 0x%x <%c>", chr, chr);
   // check if circular buffer is full
   if (nextFill == clistp->drain) {
      Warning("character buffer is full, discarding character %c",
              chr);
      return VMK_LIMIT_EXCEEDED;
   }
   clistp->c[clistp->fill] = chr;
   if (chr & TTY_QUOTE) {
      uint32 ndx = clistp->fill / BITS_PER_BYTE;
      uint32 bit = clistp->fill % BITS_PER_BYTE;
      uint32 mask = 1 << bit;
      clistp->q[ndx] |= mask;
   }
   clistp->fill = nextFill;
   return VMK_OK;
}

void
DELAY(int n) {
   Util_Udelay(n);
}

int 
vmk_kbd_printf(const char *fmt, ...)
{
   int i;
   char buffer[256];

   va_list args;

   va_start(args, fmt);
   i = vsnprintf(buffer, sizeof buffer, fmt, args);
   va_end(args);

   if (strlen(buffer) && (buffer[strlen(buffer)-1] == '\n')) {
      buffer[strlen(buffer)-1] = '\0';
   }
   _Log("%s: %s\n", XSTR(LOGLEVEL_MODULE), buffer);

   return 0;
}

int 
vmk_kbd_log(int level, const char *fmt, ...)
{
   int i;
   char buffer[256];

   va_list args;

   va_start(args, fmt);
   i = vsnprintf(buffer, sizeof buffer, fmt, args);
   va_end(args);

   if (strlen(buffer) && (buffer[strlen(buffer)-1] == '\n')) {
      buffer[strlen(buffer)-1] = '\0';
   }
   _LOG(level, "%s: %s\n", XSTR(LOGLEVEL_MODULE), buffer);

   return 0;
}

void *
vmk_kbd_malloc(int n)
{
   return Mem_Alloc(n);
}

void
vmk_kbd_free(void *p)
{
   Mem_Free(p);
}

void *
vmk_kbd_memset(void *s, 
               int c, 
               size_t n)
{
   return memset(s, c, n);
}
void *
vmk_kbd_memcpy(void *dst, 
               void *src, 
               size_t n)
{
   return memcpy(dst, src, n);
}

int 
vmk_kbd_strcmp(const char *s1, 
               const char *s2)
{
   return strcmp(s1, s2);
}

u_int8_t
vmk_kbd_INB(u_int port) 
{
   return INB(port);
}

void
vmk_kbd_OUTB(u_int port, 
             u_int8_t value)
{
   OUTB(port, value);
}
