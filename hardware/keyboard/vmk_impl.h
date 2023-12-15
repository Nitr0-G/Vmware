/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * vmk_impl.h --
 *
 */
#ifndef _VMK_IMPL_H
#define _VMK_IMPL_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#define MAX_KBD_CHAR    128
#define BITS_PER_BYTE   8  

typedef struct clist {
   char c[MAX_KBD_CHAR];                 // character storage
   char q[MAX_KBD_CHAR/BITS_PER_BYTE];   // bit map identifying quoted 
                                         // quoted charactes 
   u_int numChars;
   u_int fill;      
   u_int drain;
} clist;

void DELAY(int n);
u_int q_to_b(struct clist *clistp, char *buf, u_int size);
void clist_alloc_cblocks(struct clist *clistp, u_int maxSize, u_int reservedSize);
int putc(int chr, struct clist *clistp);

typedef u_int UINT32;
static inline int spltty(void) { return 0;}
static inline void splx(int dummy) {}
int vmk_kbd_printf(const char *fmt, ...);
int vmk_kbd_log(int level, const char *fmt, ...);
void *vmk_kbd_malloc(int n);
void vmk_kbd_free(void *p);
u_int8_t vmk_kbd_INB(u_int port);
void vmk_kbd_OUTB(u_int port, u_int8_t value);
void *vmk_kbd_memset(void *s, int c, size_t n);
void *vmk_kbd_memcpy(void *dst, void *src, size_t n);
int vmk_kbd_strcmp(const char *s1, const char *s2);

#define log vmk_kbd_log
#define printf vmk_kbd_printf
#define MIN(_a, _b)   (((_a) < (_b)) ? (_a) : (_b))
#define imin   MIN
#define bcopy(src,dst,size)            vmk_kbd_memcpy(dst, src, size)
#define malloc(arg1, ignore1, ignore2) vmk_kbd_malloc(arg1)
#define free(arg1, ignore)             vmk_kbd_free(arg1)
#define bzero(ptr, size)               vmk_kbd_memset(ptr, 0, size)
#define NOT_VMK_KBD(arg...)
#endif
