/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * libc.h --
 *
 *	Declaration of some libc utility routines.
 */

#ifndef _VMK_LIBC_H
#define _VMK_LIBC_H

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMK_MODULE
#include "includeCheck.h"

#define isspace(c)      ((c) == ' ' || ((c) >= '\t' && (c) <= '\r'))
#define isascii(c)      (((c) & ~0x7f) == 0)
#define isprint(c)      ((c) >= ' ' && (c) <= '~')
#define isupper(c)      ((c) >= 'A' && (c) <= 'Z')
#define islower(c)      ((c) >= 'a' && (c) <= 'z')
#define isalpha(c)      (isupper(c) || islower(c))
#define isdigit(c)      ((c) >= '0' && (c) <= '9')
#define isxdigit(c)     (isdigit(c) \
                          || ((c) >= 'A' && (c) <= 'F') \
                          || ((c) >= 'a' && (c) <= 'f'))
#define toupper(c)      ((c) - 0x20 * (((c) >= 'a') && ((c) <= 'z')))
#define tolower(c)      ((c) + 0x20 * (((c) >= 'A') && ((c) <= 'Z')))
#define hex2ascii(hex)	(hex2ascii_data[hex])

#ifndef ntohl
static inline uint32 
ntohl(uint32 in)
{
   return ((in >> 24) & 0x000000ff) | ((in >> 8) & 0x0000ff00) | 
          ((in << 8) & 0x00ff0000) | ((in << 24) & 0xff000000);
}
#endif

#ifndef htonl
static inline uint32 
htonl(uint32 in)
{
   return ntohl(in);
}
#endif

#ifndef ntohs
static inline uint16
ntohs(uint16 in)
{
   return ((in >> 8) & 0x00ff) | ((in << 8) & 0xff00);
}
#endif

#ifndef htons
static inline uint16
htons(uint16 in)
{
   return ntohs(in);
}
#endif

extern int inet_aton(char *cp, unsigned int *addr);
extern char *inet_ntoa(unsigned int inAddr);
/* Intrinsic of gcc compiler: */
extern int memcmp(const void *s1, const void *s2, size_t n);

/* From libc.c: */
extern char *strncpy(char *dest, const char *src, size_t count);
extern int strcmp(const char *s1, const char *s2);
extern int strnlen(const char *s, size_t n);

/* From vsprintf.c */
extern unsigned long simple_strtoul(const char *nptr, char **endptr, int base);
extern long simple_strtol(const char *nptr, char **endptr, int base);
extern char *simple_strstr(const char *s1, const char *s2);
extern char *strchr(const char *s, int c);
extern char *strrchr(const char *s, int c);


/*
 * Include <stdarg.h> so that we can define snprintf and vsnprintf to actually
 * match the implementation.  This also matches the linux kernel header file
 * definition making life easier for the vmklinux module.  Note that
 * stdarg.h is a compiler header file (not an OS or distro header) so
 * its not so scary to be pulling into the kernel.
 */
#include <stdarg.h>

/*
 * Note that both of these methods return the number of bytes required by
 * fmt which may be more than the number of bytes used.
 */
extern int snprintf(char* buf, size_t size, const char* fmt, ...) PRINTF_DECL(3,4);
extern int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap) PRINTF_DECL(3,0);

typedef void (*Printf_PutCharFunc)(const int ch, void* cookie);
extern int Printf_WithFunc(char const *fmt,
                           Printf_PutCharFunc func, void *cookie,
                           va_list ap) PRINTF_DECL(1,0);




extern int inet_aton(char *cp, uint32 *addr);
extern char *inet_ntoa(uint32 inAddr);


#endif // _VMK_LIBC_H
