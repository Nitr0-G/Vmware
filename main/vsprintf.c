/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*-
 * Copyright (c) 1986, 1988, 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 * (c) UNIX System Laboratories, Inc.
 * All or some portions of this file are derived from material licensed
 * to the University of California by American Telephone and Telegraph
 * Co. or Unix System Laboratories, Inc. and are reproduced herein with
 * the permission of UNIX System Laboratories, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#include <stdarg.h>
#include "vm_types.h"
#include "vm_libc.h"
#include "libc.h"

typedef unsigned char		u_char;
typedef unsigned short		u_short;
typedef unsigned int		u_int;
typedef unsigned long		u_long;

char const hex2ascii_data[] = "0123456789abcdefghijklmnopqrstuvwxyz";

#define ULONG_MAX	4294967295UL
#define NBBY		8
#define MAXNBUF		(sizeof(long) * NBBY + 1)

typedef struct vsnprintfBuf {
   char	* str;
   size_t remain;
} vsnprintfBuf;

/*
 * Callback for implementing vsnprintf.
 */
static void
vsnprintfPutChar(const int ch, void *cookie) {
   vsnprintfBuf* const info = (vsnprintfBuf*)cookie;

   ASSERT(info != NULL);

   if (info->remain >= 2) {
      *(info->str) = ch;
      info->str++;
      info->remain--;
   }
}


/*
 * Scaled down version of vsnprintf(3).
 */
int
vsnprintf(char *str, size_t size, const char *format, va_list ap)
{
   vsnprintfBuf info;
   int retval;

   info.str = str;
   info.remain = size;
   retval = Printf_WithFunc(format, vsnprintfPutChar, &info, ap);
   if (info.remain >= 1)
      *info.str++ = '\0';
   return retval;
}

/*
 * Scaled down version of snprintf(3).
 */
int
snprintf(char *str, size_t size, const char *format, ...)
{
	int retval;
	va_list ap;

	va_start(ap, format);
	retval = vsnprintf(str, size, format, ap);
	va_end(ap);
	return(retval);
}

/*
 * Put a NUL-terminated ASCII number (base <= 16) in a buffer in reverse
 * order; return an optional length and a pointer to the last character
 * written in the buffer (i.e., the first character of the string).
 * The buffer pointed to by `nbuf' must have length >= MAXNBUF.
 */
static char *
ksprintn(char *nbuf, register uint64 ul, register int base, register int *lenp)
{
	register char *p;

	p = nbuf;
	*p = '\0';
	do {
		*++p = hex2ascii(ul % base);
	} while (ul /= base);
	if (lenp)
		*lenp = p - nbuf;
	return (p);
}

/*
 * Scaled down version of printf(3).
 *
 * Two additional formats:
 *
 * The format %b is supported to decode error registers.
 * Its usage is:
 *
 *	printf("reg=%b\n", regval, "<base><arg>*");
 *
 * where <base> is the output base expressed as a control character, e.g.
 * \10 gives octal; \20 gives hex.  Each arg is a sequence of characters,
 * the first of which gives the bit number to be inspected (origin 1), and
 * the next characters (up to a control character, i.e. a character <= 32),
 * give the name of the register.  Thus:
 *
 *	printf("reg=%b\n", 3, "\10\2BITTWO\1BITONE\n");
 *
 * would produce output:
 *
 *	reg=3<BITTWO,BITONE>
 *
 * XXX:  %D  -- Hexdump, takes pointer and separator string:
 *		("%6D", ptr, ":")   -> XX:XX:XX:XX:XX:XX
 *		("%*D", len, ptr, " " -> XX XX XX XX ...
 *
 * Returns:
 *	Number of characters required by the format string
 */
int
Printf_WithFunc(char const *fmt, Printf_PutCharFunc func, void *cookie, va_list ap)
{
#define PCHAR(c) { (*func)(c,cookie); charCount++; }
	char nbuf[MAXNBUF];
	char *p, *q;
	u_char *up;
	int ch, n;
	uint64 ul;
	int base, lflag, llflag, tmp, width, ladjust, sharpflag, neg, sign, dot;
	int dwidth;
	char padc;
	int charCount = 0;
        int radix = 10;

        ASSERT(func);

	if (fmt == NULL)
		fmt = "(fmt null)";

	for (;;) {
		padc = ' ';
		width = 0;
		while ((ch = (u_char)*fmt++) != '%') {
			if (ch == '\0') 
				return charCount;
			PCHAR(ch);
		}
		lflag = 0; llflag = 0; ladjust = 0; sharpflag = 0; neg = 0;
		sign = 0; dot = 0; dwidth = 0;
reswitch:	switch (ch = (u_char)*fmt++) {
		case '.':
			dot = 1;
			goto reswitch;
		case '#':
			sharpflag = 1;
			goto reswitch;
		case '+':
			sign = 1;
			goto reswitch;
		case '-':
			ladjust = 1;
			goto reswitch;
		case '%':
			PCHAR(ch);
			break;
		case '*':
			if (!dot) {
				width = va_arg(ap, int);
				if (width < 0) {
					ladjust = !ladjust;
					width = -width;
				}
			} else {
				dwidth = va_arg(ap, int);
			}
			goto reswitch;
		case '0':
			if (!dot) {
				padc = '0';
				goto reswitch;
			}
		case '1': case '2': case '3': case '4':
		case '5': case '6': case '7': case '8': case '9':
				for (n = 0;; ++fmt) {
					n = n * 10 + ch - '0';
					ch = *fmt;
					if (ch < '0' || ch > '9')
						break;
				}
			if (dot)
				dwidth = n;
			else
				width = n;
			goto reswitch;
		case 'b':
			ul = va_arg(ap, int);
			p = va_arg(ap, char *);
			for (q = ksprintn(nbuf, ul, *p++, NULL); *q;)
				PCHAR(*q--);

			if (!ul)
				break;

			for (tmp = 0; *p;) {
				n = *p++;
				if (ul & (1 << (n - 1))) {
					PCHAR(tmp ? ',' : '<');
					for (; (n = *p) > ' '; ++p)
						PCHAR(n);
					tmp = 1;
				} else
					for (; *p > ' '; ++p)
						continue;
			}
			if (tmp)
				PCHAR('>');
			break;
		case 'c':
			PCHAR(va_arg(ap, int));
			break;
		case 'D':
			up = va_arg(ap, u_char *);
			p = va_arg(ap, char *);
			if (!width)
				width = 16;
			while(width--) {
				PCHAR(hex2ascii(*up >> 4));
				PCHAR(hex2ascii(*up & 0x0f));
				up++;
				if (width)
					for (q=p;*q;q++)
						PCHAR(*q);
			}
			break;
		case 'd':
			ul = lflag ? (llflag ? va_arg(ap, int64) : va_arg(ap, long))
			   : va_arg(ap, int);
			sign = 1;
			base = 10;
			goto number;
		case 'L':
      		        llflag = 1;
		case 'l':
			if (lflag)
				/* handle the %llx case too */
				llflag = 1;
			lflag = 1;
			goto reswitch;
		case 'o':
			ul = lflag ? (llflag ? va_arg(ap, uint64) : va_arg(ap, u_long))
			   : va_arg(ap, u_int);
			base = 8;
			goto nosign;
		case 'p':
			ul = (u_long)va_arg(ap, void *);
			base = 16;
			sharpflag = (width == 0);
			goto nosign;
		case 'n':
		case 'r':
			ul = lflag ? (llflag ? va_arg(ap, uint64) : va_arg(ap, u_long))
			   : sign ? (u_long)va_arg(ap, int) : va_arg(ap, u_int);
			base = radix;
			goto number;
		case 's':
			p = va_arg(ap, char *);
			if (p == NULL)
				p = "(null)";
			if (!dot)
				n = strlen (p);
			else
				for (n = 0; n < dwidth && p[n]; n++)
					continue;

			width -= n;

			if (!ladjust && width > 0)
				while (width--)
					PCHAR(padc);
			while (n--)
				PCHAR(*p++);
			if (ladjust && width > 0)
				while (width--)
					PCHAR(padc);
			break;
		case 'u':
			ul = lflag ? (llflag ? va_arg(ap, uint64) : va_arg(ap, u_long))
			   : va_arg(ap, u_int);
			base = 10;
			goto nosign;
		case 'x':
		case 'X':
			ul = lflag ? (llflag ? va_arg(ap, uint64) : va_arg(ap, u_long))
			   : va_arg(ap, u_int);
			base = 16;
			goto nosign;
		case 'z':
			ul = lflag ? (llflag ? va_arg(ap, uint64) : va_arg(ap, u_long))
			   : sign ? (u_long)va_arg(ap, int) : va_arg(ap, u_int);
			base = 16;
			goto number;
nosign:			sign = 0;
number:			if (sign && (int64)ul < 0LL) {
				neg = 1;
				ul = -(int64)ul;
			}
			p = ksprintn(nbuf, ul, base, &tmp);
                        /*
                         * The original code prepended the 0x or 0 for every
                         * value except 0.  That's really annoying, though
                         * not so annoying as the libs that print (nil)
                         * instead of a number.  --Jeremy.
                         *
			 * if (sharpflag && ul != 0) {
                         */
			if (sharpflag) {
				if (base == 8)
					tmp++;
				else if (base == 16)
					tmp += 2;
			}
			if (neg)
				tmp++;

			if (!ladjust && width && (width -= tmp) > 0)
				while (width--)
					PCHAR(padc);
			if (neg)
				PCHAR('-');
                        /*
                         * The original code prepended the 0x or 0 for every
                         * value except 0.  That's really annoying, though
                         * not so annoying as the libs that print (nil)
                         * instead of a number.  --Jeremy.
                         *
			 * if (sharpflag && ul != 0) {
                         */
			if (sharpflag) {
				if (base == 8) {
					PCHAR('0');
				} else if (base == 16) {
					PCHAR('0');
					PCHAR('x');
				}
			}

			while (*p)
				PCHAR(*p--);

			if (ladjust && width && (width -= tmp) > 0)
				while (width--)
					PCHAR(padc);

			break;
		default:
			PCHAR('%');
			if (lflag)
				PCHAR('l');
			PCHAR(ch);
			break;
		}
	}
#undef PCHAR
}

/*
 * Convert a string to an unsigned long integer.
 *
 * Ignores `locale' stuff.  Assumes that the upper and lower case
 * alphabets and digits are each contiguous.
 */
unsigned long
simple_strtoul(const char *nptr, char **endptr, register int base)
{
	register const char *s = nptr;
	register unsigned long acc;
	register unsigned char c;
	register unsigned long cutoff;
	register int neg = 0, any, cutlim;

	/*
	 * See strtol for comments as to the logic used.
	 */
	do {
		c = *s++;
	} while (isspace(c));
	if (c == '-') {
		neg = 1;
		c = *s++;
	} else if (c == '+')
		c = *s++;
	if ((base == 0 || base == 16) &&
	    c == '0' && (*s == 'x' || *s == 'X')) {
		c = s[1];
		s += 2;
		base = 16;
	}
	if (base == 0)
		base = c == '0' ? 8 : 10;
	cutoff = (unsigned long)ULONG_MAX / (unsigned long)base;
	cutlim = (unsigned long)ULONG_MAX % (unsigned long)base;
	for (acc = 0, any = 0;; c = *s++) {
		if (!isascii(c))
			break;
		if (isdigit(c))
			c -= '0';
		else if (isalpha(c))
			c -= isupper(c) ? 'A' - 10 : 'a' - 10;
		else
			break;
		if (c >= base)
			break;
		if (any < 0 || acc > cutoff || (acc == cutoff && c > cutlim))
			any = -1;
		else {
			any = 1;
			acc *= base;
			acc += c;
		}
	}
	if (any < 0) {
		acc = ULONG_MAX;
		// errno = ERANGE;
	} else if (neg)
		acc = -acc;
	if (endptr != 0)
		*endptr = (char *)(any ? s - 1 : nptr);
	return (acc);
}

/*
 * Convert a string to a signed long integer.
 *
 * Ignores `locale' stuff.  Assumes that the upper and lower case
 * alphabets and digits are each contiguous.
 */
long
simple_strtol(const char *nptr, char **endptr, register int base)
{
   /* simple_strtoul actually does handle negative numbers, but just
    * returns it as a unsigned long */
   return (long)simple_strtoul(nptr, endptr, base);
}

/*
 *----------------------------------------------------------------------
 *
 * simple_strstr --
 *
 *      Routine to check if s1 contains s2
 *
 * Results: 
 *	Returns the first occurrence of s2 in s1, or NULL if
 *	s2 is not found in s1.
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */
char *
simple_strstr(const char *s1, const char *s2)
{
   char *p1, *p2, *backtrack;

   if ((s1 == NULL) || (s2 == NULL)) {
      return(NULL);
   }
   backtrack = p1 = (char*)s1;
   p2 = (char*)s2;
   while ((*p1 != '\0') && (*p2 != '\0')) {
      if (*p1 != *p2) {
         p1 = ++backtrack;
         p2 = (char*)s2;
      } else {
         p1++;
         p2++;
      }
   }
   if (*p2 == '\0') {
      return (backtrack);
   } else {
      return NULL;
   }

}
