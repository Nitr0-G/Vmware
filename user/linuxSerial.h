/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * linuxSerial.h:
 *
 *    Linux-compatible serial port support.
 */

#ifndef _VMKERNEL_USER_LINUX_SERIAL_H
#define _VMKERNEL_USER_LINUX_SERIAL_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

/*
 * Terminal ioctls (byte 0x54)
 */
#define LINUX_TCGETS                  0x5401   /* get termios struct */
#define LINUX_TCSETS                  0x5402   /* set termios struct */
#define LINUX_TIOCMGET                0x5415   /* get status of control lines */
#define LINUX_TIOCMBIS                0x5416   /* enable RTS, DTR or loopback regs */
#define LINUX_TIOCMBIC                0x5417   /* disable RTS, DTR or loopback regs */
#define LINUX_FIONBIO                 0x5421   /* set non-blocking io */
#define LINUX_TIOCSBRK                0x5427   /* set break (BSD compatibility) */
#define LINUX_TIOCCBRK                0x5428   /* clear break (BSD compatibility) */

struct Linux_termios {
   unsigned int  c_iflag;                      /* input mode flags */
   unsigned int  c_oflag;                      /* output mode flags */
   unsigned int  c_cflag;                      /* control mode flags */
   unsigned int  c_lflag;                      /* local mode flags */
   unsigned char c_line;                       /* line discipline */
   unsigned char c_cc[32];                     /* control characters */
   unsigned int  c_ispeed;                     /* input speed */
   unsigned int  c_ospeed;                     /* output speed */
};

#endif // VMKERNEL_USER_LINUX_SERIAL_H
