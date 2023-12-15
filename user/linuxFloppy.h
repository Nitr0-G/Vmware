/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * linuxFloppy.h:
 *
 *    Linux-compatible floppy support.
 */

#ifndef _VMKERNEL_USER_LINUX_FLOPPY_H
#define _VMKERNEL_USER_LINUX_FLOPPY_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

/*
 * Floppy disk ioctls (byte 0x02)
 */
#define LINUX_FLOPPY_FDGETPRM         0x0204   /* Get drive parameters */
#define LINUX_FLOPPY_FDGETDRVTYP      0x020f   /* Get drive type (name) */
#define LINUX_FLOPPY_FDGETDRVSTAT     0x0212   /* Get drive state */
#define LINUX_FLOPPY_FDPOLLDRVSTAT    0x0213   /* Poll drive state */
#define LINUX_FLOPPY_FDFLUSH          0x024b   /* Flush drive */
#define LINUX_FLOPPY_FDRESET          0x0254   /* Reset drive */
#define LINUX_FLOPPY_FDRAWCMD         0x0258   /* Raw floppy command */


/*
 * Floppy reset modes
 */
enum LinuxFloppy_reset_mode {
        LINUX_FLOPPY_FD_RESET_IF_NEEDED,
        LINUX_FLOPPY_FD_RESET_IF_RAWCMD,
        LINUX_FLOPPY_FD_RESET_ALWAYS
};

/*
 * Floppy drive name
 */
typedef char LinuxFloppy_drive_name[16];

/*
 * Floppy drive current state (readonly)
 */
struct LinuxFloppy_drive_struct {
   unsigned long flags;
   unsigned long spinup_date;
   unsigned long select_date;
   unsigned long first_read_date;
   short probed_format;
   short track;
   short maxblock;
   short maxtrack;
   int generation;
   int keep_data;
   int fd_ref;
   int fd_device;
   unsigned long last_checked;
   char *dmabuf;
   int bufblocks;
};

/*
 * Floppy drive parameters
 */
struct LinuxFloppy_struct {
   unsigned int size;
   unsigned int sect;
   unsigned int head;
   unsigned int track;
   unsigned int stretch;
   unsigned char gap;
   unsigned char rate;
   unsigned char spec1;
   unsigned char fmt_gap;
   const char *name;
};

/*
 * Floppy raw command
 */
struct LinuxFloppy_raw_cmd {
   unsigned int flags;
   void *data;
   char *kernel_data;
   struct LinuxFloppy_raw_cmd *next;
   long length;
   long phys_length;
   int buffer_length;
   unsigned char rate;
   unsigned char cmd_count;
   unsigned char cmd[16];
   unsigned char reply_count;
   unsigned char reply[16];
   int track;
   int resultcode;
   int reserved1;
   int reserved2;
};

#endif // VMKERNEL_USER_LINUX_FLOPPY_H
