/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * vga_public.c --
 *
 *      Public definitions for VGA module.
 */
#ifndef  _VGA_PUBLIC_H
#define _VGA_PUBLIC_H

#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "vm_basic_defs.h"


// Constants
#define VGA_ATTRIBUTE_INDEX     0x3C0   // also used for write value, reset by
#define VGA_ATTRIBUTE_WVALUE    0x3C0   // reading VGA_STATUS
#define VGA_ATTRIBUTE_RVALUE    0x3C1   // only for read value
#define VGA_SEQUENCER_INDEX     0x3C4
#define VGA_SEQUENCER_VALUE     0x3C5
#define VGA_PEL_READ_INDEX      0x3C7   // Palette
#define VGA_PEL_WRITE_INDEX     0x3C8
#define VGA_PEL_VALUE           0x3C9
#define VGA_GRAPHICS_INDEX      0x3CE
#define VGA_GRAPHICS_VALUE      0x3CF
#define VGA_CRTC_INDEX          0x3D4   // Cathode Ray Tube Controller
#define VGA_CRTC_VALUE          0x3D5
#define VGA_STATUS              0x3DA

#define VGA_ATTRIBUTE_COLOR     18      // Color plane enable
#define VGA_ATTRIBUTE_OUTPUT    32      // Screen output enable

#define VGA_SEQUENCER_RESET     0       // Reset
#define VGA_SEQUENCER_MAPMASK   2       // Planes accessible by CPU
#define VGA_SEQUENCER_CHARMAP   3       // Select character map
#define VGA_SEQUENCER_MEMMODE   4       // Memory mode for CPU access

#define VGA_GRAPHICS_MAPSELECT  4       // Plane to read from in read mode 0
#define VGA_GRAPHICS_MODE       5       // Data transform between CPU and video
#define VGA_GRAPHICS_MISC       6       // Video aperture

#define VGA_CRTC_MAXSCANLINES   9       // Character height
#define VGA_CRTC_CURSOR         10      // Cursor shape
#define VGA_CRTC_START          12      // Video-displayed buffer start
#define VGA_CRTC_CURSOR_POS     14      // Cursor position

#define VGA_START_MAP0          0xA0000
#define VGA_END_MAP0            0xC0000
#define VGA_START_MAP1          0xA0000
#define VGA_END_MAP1            0xB0000
#define VGA_START_MAP2          0xB0000
#define VGA_END_MAP2            0xB8000
#define VGA_START_MAP3          0xB8000
#define VGA_END_MAP3            0xC0000
#define VGA_MAP_MAX             (VGA_END_MAP0-VGA_START_MAP0)


/*
 * The VGA video buffer is divided into independant screens to minimize
 * locking. A screen is 25x80 16-pixels high characters. Two screens can
 * be used together to provide an extended screen of 50x80 8-pixels high
 * characters.
 */
#define VGA_NUM_ROWS            25
#define VGA_NUM_COLS            80
#define VGA_CHAR_HEIGHT         16
#define VGA_EXTENSION_FACTOR    2

#define VGA_SCREEN_SIZE_IN_BYTES \
		ROUNDUP((VGA_NUM_ROWS*VGA_NUM_COLS)*sizeof(uint16), PAGE_SIZE)
#define VGA_SCREEN_SIZE		(VGA_SCREEN_SIZE_IN_BYTES/sizeof(uint16))

#define VGA_SCREEN_COS		0	// first screen goes to COS

#define VGA_POS(row, col, scr)	((col) + (row)*VGA_NUM_COLS + \
				    (scr)*VGA_SCREEN_SIZE)
#define VGA_FIRST(scr)		VGA_POS(0, 0, scr)
#define VGA_LAST(scr, extended)	VGA_POS(VGA_NUM_ROWS * \
                                        ((extended)?VGA_EXTENSION_FACTOR:1)-1, \
                                        VGA_NUM_COLS-1, scr)

// VGA access marshalling
#define VGA_COS_LOCKOUT_FREE	0
#define VGA_COS_LOCKOUT_BUSY	1
#define VGA_COS_LOCKOUT_ON	2


#endif
