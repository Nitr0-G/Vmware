/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * vga.h --
 *
 *	VGA specific functions.
 */

#ifndef _VGA_H
#define _VGA_H

#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "vmnix_if.h"
#include "vga_public.h"
#include "ansi.h"


#define VGA_SCREEN_INVALID      ((uint32)-1)

extern void VGA_Init(VMnix_Info *vmnixInfo, VMnix_SharedData *sharedData);
extern uint32 VGA_Alloc(Bool extended, uint32 *numRows, uint32 *numCols);
extern void VGA_Display(uint32 scr);
extern void VGA_Putfb(uint32 scr, uint32 row, uint32 col, uint16 *fatBuf, uint32 len);
extern void VGA_Clear(uint32 scr, uint32 row, uint32 col, uint32 numRows, uint32 numCols, uint16 clearFatChar);
extern void VGA_Cursor(uint32 scr, uint32 row, uint32 col, uint8 height);
extern void VGA_Scroll(uint32 scr, uint32 topRow, uint32 bottomRow, uint32 numRows, Bool up, uint16 clearFatChar);
extern void VGA_Blank(void);



/*
 * In the VGA video buffer, each character takes up two bytes. The top byte
 * describes the colors (front and back), the bottom byte is the actual
 * character.
 *
 * For the colors, we define them so that the second set of 8 is a brighter
 * version of the first set of 8 in order to use the top bit as a brightness
 * indicator.
 */

/*
 *----------------------------------------------------------------------
 *
 * VGA_MakeAttribute --
 *
 * 	Make the synthetic VGA attribute byte
 *
 * Results:
 * 	The synthetic VGA attribute byte
 *
 * Side Effects:
 * 	None
 *
 *----------------------------------------------------------------------
 */
static INLINE uint8
VGA_MakeAttribute(Ansi_Attr *ansiAttr)
{
   return (ansiAttr->fore & 7) |
	  (ansiAttr->bright & 1)<<3 |
	  (ansiAttr->back & 7)<<4;
}

/*
 *----------------------------------------------------------------------
 *
 * VGA_MakeFatChar --
 *
 * 	Make the VGA 'fat' character (made up of its attribute and glyph)
 *
 * Results:
 * 	The VGA 'fat' character
 *
 * Side Effects:
 * 	None
 *
 *----------------------------------------------------------------------
 */
static INLINE uint16
VGA_MakeFatChar(uint8 c, uint8 attr)
{
   return c | attr<<8;
}


#endif
