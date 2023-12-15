/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * libc.c --
 *
 *	Libc functions that we need.  We currently get some from
 *	vm_libc.h as inlines.  The right solution is to have a library
 *	that the vmkernel and the monitor can link with to get
 *	functions that we don't want to inline.
 */

#include <stddef.h>	// for size_t definition
#include "vm_basic_types.h"
#include "libc.h"

char *strncpy(char *dest, const char *src, size_t count)
{
   char *ret = dest;

   while (*src != 0 && count > 0) {
      *dest++ = *src++;
      count--;
   }

   if (count > 0) {
      *dest = 0;
   }

   return ret;
}

int strcmp(const char *s1, const char *s2)
{
   int res;

   while (1) {
      res = *s1 - *s2;
      if (res != 0 || *s1 == 0) {
	 break;
      }
      s1++;
      s2++;      
   }

   return res;
}


void mcount(void)
{
}


//XXX stolen from frobos

int
strnlen(const char *s, size_t n)
{
   const char *p = s;
   
   while (*p != '\0' && n > 0) {
      p++; n--;
   }
   return p - s;
}

static char inetAddrBuffer[20];

char *inet_ntoa(unsigned int inAddr)
{
   snprintf(inetAddrBuffer, sizeof inetAddrBuffer, "%d.%d.%d.%d", 
           inAddr >> 24, (inAddr >> 16) & 0xff, (inAddr >> 8) & 0xff, 
	   inAddr & 0xff);
   return inetAddrBuffer;
}


/*
 * Check whether "cp" is a valid ascii representation
 * of an Internet address and convert to a binary address.
 * Returns 1 if the address is valid, 0 if not.
 * This replaces inet_addr, the return value from which
 * cannot distinguish between failure and a local broadcast address.
 */
int
inet_aton(char *cp, unsigned int *addr)
{
        unsigned long val;
        int base, n;
        char c;
        unsigned int parts[4];
        unsigned int *pp = parts;

        for (;;) {
                /*
                 * Collect number up to ``.''.
                 * Values are specified as for C:
                 * 0x=hex, 0=octal, other=decimal.
                 */
                val = 0; base = 10;
                if (*cp == '0') {
                        if (*++cp == 'x' || *cp == 'X')
                                base = 16, cp++;
                        else
                                base = 8;
                }
                while ((c = *cp) != '\0') {
                        if (isascii(c) && isdigit(c)) {
                                val = (val * base) + (c - '0');
                                cp++;
                                continue;
                        }
                        if (base == 16 && isascii(c) && isxdigit(c)) {
                                val = (val << 4) +
                                        (c + 10 - (islower(c) ? 'a' : 'A'));
                                cp++;
                                continue;
                        }
                        break;
                }
                if (*cp == '.') {
                        /*
                         * Internet format:
                         *      a.b.c.d
                         *      a.b.c   (with c treated as 16-bits)
                         *      a.b     (with b treated as 24 bits)
                         */
                        if (pp >= parts + 3 || val > 0xff)
                                return (0);
                        *pp++ = val, cp++;
                } else
                        break;
        }
        /*
         * Check for trailing characters.
         */
        if (*cp && (!isascii(*cp) || !isspace(*cp)))
                return (0);
        /*
         * Concoct the address according to
         * the number of parts specified.
         */
        n = pp - parts + 1;
        switch (n) {

        case 0:
                return (0);             /* initial nondigit */

        case 1:                         /* a -- 32 bits */
                break;

        case 2:                         /* a.b -- 8.24 bits */
                if (val > 0xffffff)
                        return (0);
                val |= parts[0] << 24;
                break;

        case 3:                         /* a.b.c -- 8.8.16 bits */
                if (val > 0xffff)
                        return (0);
                val |= (parts[0] << 24) | (parts[1] << 16);
                break;

        case 4:                         /* a.b.c.d -- 8.8.8.8 bits */
                if (val > 0xff)
                        return (0);
                val |= (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8);
                break;
        }
        if (addr)
                *addr  = htonl(val);
        return (1);
}


char *
strchr(char *s, int c)
{
   for (;*s != (char) c; s++) {
      if (*s == '\0') {
         return NULL;
      }
   }
   
   return s;
}


char *
strrchr(char *s, int c)
{
   char *found = NULL;

   for (;*s != '\0'; s++) {
      if (*s == (char)c) {
         found = s;
      }
   }
   
   return found;
}
