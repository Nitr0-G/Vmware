/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * vga.c - VGA handling
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "vm_asm.h"
#include "vmkernel.h"
#include "kvmap.h"
#include "host.h"
#include "vga.h"
#include "vgaFont8x8.h"

#define LOGLEVEL_MODULE VGA
#define LOGLEVEL_MODULE_LEN 3
#include "log.h"



/*
 * Screen descriptor
 */
typedef enum VGAScreenState {
   VGA_UNUSED = 0,
   VGA_SIMPLE,          // Simple screen
   VGA_EXTENDED,        // Primary screen of extended screen
   VGA_EXTENSION        // Secondary screen of extended screen
} VGAScreenState;

typedef struct VGAScreen {
   VGAScreenState       state;
   uint8                numRows;
   uint8                charHeight;
   const uint8 **       font;
} VGAScreen;

static uint8 vgaFontCOS[256*VGA_CHAR_HEIGHT];
static const uint8 *vgaFontSimple;
static const uint8 *vgaFontExtended;

const VGAScreen vgaSimple = {VGA_SIMPLE, VGA_NUM_ROWS,
                             VGA_CHAR_HEIGHT, &vgaFontSimple};
const VGAScreen vgaExtended = {VGA_EXTENDED, VGA_NUM_ROWS*VGA_EXTENSION_FACTOR,
                               VGA_CHAR_HEIGHT/VGA_EXTENSION_FACTOR,
                               &vgaFontExtended};
const VGAScreen vgaExtension = {VGA_EXTENSION};

static VGAScreen screens[VGA_MAP_MAX / VGA_SCREEN_SIZE_IN_BYTES];
static uint32 vgaNumScreens; // number of usable screens based on VGA aperture


/*
 * vgaCOSLockOut is used to lock out COS (VGA_SCREEN_COS) with
 * the routines VGALocOutCOS() and VGAUnlockCOS(). Once locked out,
 * COS is guaranteed not to touch VGA registers, it still will
 * be able to directly access its slice of the VGA buffer.
 *
 * vgaLock is only used to guarantee atomicity of VGA register accesses.
 * VGA_Display(), VGA_Cursor() and VGA_Blank() access VGA registers.
 * It is expected that use of those functions will be synchronized at
 * a higher level.
 *
 * VGA_Putfb(), VGA_Clear() and VGA_Scroll() access a given slice
 * of the VGA buffer. Concurrent calling of the functions on different
 * slices is safe. Concurrent calling of the functions on the same
 * slice is expected to be synchronized at a higher level.
 *
 * VGA_Alloc() is expected to be synchronized at a higher level.
 */
static SP_SpinLockIRQ vgaLock;	// register access atomicity
Atomic_uint32 vgaCOSLockOut = {VGA_COS_LOCKOUT_FREE};	// COS lock out


static uint32 vgaCurScr;	// screen currently driving video output
static uint16 *vgaVideo;	// mapped VGA video buffer

#define pos(row, col, scr)	(vgaVideo + VGA_POS(row, col, scr))
#define first(scr)		(vgaVideo + VGA_FIRST(scr))
#define last(scr)		(vgaVideo + VGA_LAST(scr, \
                                            screens[scr].state == VGA_EXTENDED))



/*
 * The palette is made up of 16 colors. Each color is RGB-coded (3*6 bits).
 * For the first 8 colors, we use the ANSI ordering and define medium
 * colors. The next 8 colors are the same ones brighter.
 */
#define NUM_COLORS	16

typedef struct {
   uint8 red;
   uint8 green;
   uint8 blue;
} vgaPalette[NUM_COLORS];

static vgaPalette vgaPaletteDefault =
				{{0,0,0}, {42,0,0}, {0,42,0}, {42,42,0},
				 {0,0,42},{42,0,42},{0,42,42},{42,42,42},
				 {21,21,21},{63,21,21}, {21,63,21},{63,63,21},
				 {21,21,63},{63,21,63},{21,63,63},{63,63,63}};

static vgaPalette vgaPaletteBlank =
				{{0,0,0},{0,0,0},{0,0,0},{0,0,0},
				 {0,0,0},{0,0,0},{0,0,0},{0,0,0},
				 {0,0,0},{0,0,0},{0,0,0},{0,0,0},
				 {0,0,0},{0,0,0},{0,0,0},{0,0,0}};


static void VGALoadPalette(vgaPalette palette);
static void VGALoadFont(const uint8 *font, uint8 height);
static void VGASaveFont(uint8 *font, uint8 height);


/*
 *----------------------------------------------------------------------
 *
 * VGA_Init --
 *
 * 	Initialize VGA module
 *
 * Results:
 * 	None
 *
 * Side Effects:
 * 	VGA video buffer is mapped
 *
 *----------------------------------------------------------------------
 */
void
VGA_Init(VMnix_Info *vmnixInfo, VMnix_SharedData *sharedData)
{
   KVMap_MPNRange range;


   Log("start %x end %x %s", vmnixInfo->vgaStart, vmnixInfo->vgaEnd,
       vmnixInfo->vgaExtended ? "extended" : "");

   // Set up locks
   SP_InitLockIRQ("vgaLck", &vgaLock, SP_RANK_IRQ_LEAF);
   SHARED_DATA_ADD(sharedData->vgaCOSLockOut, Atomic_uint32 *, &vgaCOSLockOut);

   // Map VGA video buffer
   range.startMPN = MA_2_MPN(vmnixInfo->vgaStart);
   range.numMPNs = MA_2_MPN(vmnixInfo->vgaEnd)-MA_2_MPN(vmnixInfo->vgaStart)+1;
   vgaVideo = KVMap_MapMPNs(range.numMPNs, &range, 1, 0); // XXX TLB_UNCACHED
   ASSERT(vgaVideo);
   Log("%p", vgaVideo);

   // Actual number of screens available
   ASSERT(vmnixInfo->vgaEnd - vmnixInfo->vgaStart <= VGA_MAP_MAX);
   vgaNumScreens = (vmnixInfo->vgaEnd - vmnixInfo->vgaStart) /
                         VGA_SCREEN_SIZE_IN_BYTES;
   Log("%d screens", vgaNumScreens);

   /*
    * If COS is not extended, its font yields a 25x80 display. We'll need to
    * load another font to yield a 50x80 display and so we need to save it to
    * switch between the two.
    * NOTE: Apparently newer graphics cards do not support using another font
    * than font 0 (or they need extra steps I'm unaware of) so we cannot simply
    * load the fonts concurrently and switch the font pointer.
    * If COS is extended, its font already yields a 50x80 display and we don't
    * offer a normal 25x80 capability as it is presumably unwanted.
    */

   // Set up screen descriptors
   memset(screens, 0, vgaNumScreens * sizeof(VGAScreen));

   // Set up COS screen properties
   if (vmnixInfo->vgaExtended) {
      vgaFontSimple = NULL;
      vgaFontExtended = NULL;
      screens[VGA_SCREEN_COS] = vgaExtended;
      screens[VGA_SCREEN_COS+1] = vgaExtension;
   } else {
      ASSERT(sizeof(vgaFontCOS)/256 == VGA_CHAR_HEIGHT);
      VGASaveFont(vgaFontCOS, sizeof(vgaFontCOS)/256);
      vgaFontSimple = vgaFontCOS;
      vgaFontExtended = vgaFont8x8;
      screens[VGA_SCREEN_COS] = vgaSimple;
   }

   // COS screen is up
   vgaCurScr = VGA_SCREEN_COS;
}


/*
 *----------------------------------------------------------------------
 *
 * VGA_Alloc --
 *
 *      Allocate one or two screens
 *
 * Results:
 *      screen number of the lone or primary screen
 *      VGA_INVALID_SCREEN if not enough resources
 *
 * Side Effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
uint32
VGA_Alloc(Bool extended, uint32 *numRows, uint32 *numCols)
{
   uint32 scr;

   // If COS is extended, everybody is
   extended = extended || (screens[VGA_SCREEN_COS].state == VGA_EXTENDED);

   for (scr = 0; scr < vgaNumScreens; scr++) {
      if (screens[scr].state == VGA_UNUSED) {
         if (!extended) {
            screens[scr] = vgaSimple;
            Log("%d", scr);
            break;
         }
         if ((scr+1 < vgaNumScreens) && (screens[scr+1].state == VGA_UNUSED)) {
            screens[scr] = vgaExtended;
            screens[scr+1] = vgaExtension;
            Log("%d,%d", scr, scr+1);
            break;
         }
      }
   }

   if (scr == vgaNumScreens) {
      return VGA_SCREEN_INVALID;
   }

   *numRows = screens[scr].numRows;
   *numCols = VGA_NUM_COLS;

   return scr;
}


/*
 *----------------------------------------------------------------------
 *
 * VGALockOutCOS --
 *
 * 	Prevent COS from touching VGA
 *
 * Results:
 * 	None
 *
 * Side Effects:
 * 	vgaCOSLock is acquired
 *
 *----------------------------------------------------------------------
 */
static void
VGALockOutCOS(void)
{
   extern uint64 cpuHzEstimate;
   uint32 eflags;
   Bool doEnable;
   uint32 lockOut;
   uint64 start;

   // Disable interrupts
   SAVE_FLAGS(eflags);
   if (eflags & EFLAGS_IF) {
      CLEAR_INTERRUPTS();
      doEnable = TRUE;
   } else {
      doEnable = FALSE;
   }

   /*
    * Contention should only come from COS and should be very
    * limited as the protected regions are small. If we spin out
    * it is very likely that COS is dead.
    */
   start = RDTSC();
   do {
      lockOut = Atomic_ReadIfEqualWrite(&vgaCOSLockOut,
		   VGA_COS_LOCKOUT_FREE, VGA_COS_LOCKOUT_ON);

      COMPILER_MEM_BARRIER();

      switch (lockOut) {
      case VGA_COS_LOCKOUT_FREE:
         // We got it
         break;
      case VGA_COS_LOCKOUT_BUSY:
         // COS is currently accessing VGA
	 PAUSE();
	 if (RDTSC() - start > cpuHzEstimate/16) { // 1/16 s
	    Panic("VGA LockOut busy");
	 }
         break;
      case VGA_COS_LOCKOUT_ON:
         // We should never contend with ourself
         Panic("VGA LockOut contention");
         break;
      default:
         Panic("VGA LockOut corruption");
         break;
      }
   } while (lockOut != VGA_COS_LOCKOUT_FREE);

   if (doEnable) {
      ENABLE_INTERRUPTS();
   }
}


/*
 *----------------------------------------------------------------------
 *
 * VGAUnlockCOS --
 *
 * 	Allow COS to touch VGA
 *
 * Results:
 * 	None
 *
 * Side Effects:
 * 	vgaCOSLock is released and VMNIX_VGA_INTERRUPT is sent to COS
 *
 *----------------------------------------------------------------------
 */
static void
VGAUnlockCOS(void)
{
   ASSERT(Atomic_Read(&vgaCOSLockOut) == VGA_COS_LOCKOUT_ON);
   COMPILER_MEM_BARRIER();
   Atomic_Write(&vgaCOSLockOut, VGA_COS_LOCKOUT_FREE);

   Host_InterruptVMnix(VMNIX_VGA_INTERRUPT);
}


/*
 *----------------------------------------------------------------------
 *
 * VGA_Display --
 *
 *      Display a screen, i.e. make it the actual video output
 *
 * Results:
 *      None
 *
 * Side Effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
void
VGA_Display(uint32 scr)
{
   SP_IRQL prevIRQL;
   uint16 relpos = VGA_FIRST(scr);


   ASSERT(scr < vgaNumScreens);
   ASSERT((screens[scr].state == VGA_SIMPLE) ||
          (screens[scr].state == VGA_EXTENDED));

   // Nothing to do if the requested screen is already up
   if (scr == vgaCurScr) {
      return;
   }

   /*
    * If the current screen is COS', we need to lock COS out first
    * since it will soon lose access. We also need to load our palette
    */
   if (vgaCurScr == VGA_SCREEN_COS) {
      VGALockOutCOS();
      VGALoadPalette(vgaPaletteDefault);
   }

   /*
    * Bring up the new screen, either directly if it is not COS'
    * or indirectly otherwise (we could bring it up here but we
    * still have to rely on COS to reload its palette so it might
    * just as well bring up the screen).
    * NOTE: vmkernel is always responsible for the font.
    */
   if (*screens[scr].font != *screens[vgaCurScr].font) {
      VGALoadFont(*screens[scr].font, screens[scr].charHeight);
   }
   if (scr == VGA_SCREEN_COS) {
      VGAUnlockCOS();
   } else {
      prevIRQL = SP_LockIRQ(&vgaLock, SP_IRQL_KERNEL);

      // Set the start of display to the start of this screen
      OUTB(VGA_CRTC_INDEX, VGA_CRTC_START);
      OUTB(VGA_CRTC_VALUE, relpos>>8);
      OUTB(VGA_CRTC_INDEX, VGA_CRTC_START+1);
      OUTB(VGA_CRTC_VALUE, relpos & 0xFF);

      SP_UnlockIRQ(&vgaLock, prevIRQL);
   }

   vgaCurScr = scr;
}


/*
 *----------------------------------------------------------------------
 *
 * VGA_Putfb --
 *
 *      Put fat characters
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
void
VGA_Putfb(uint32 scr, uint32 row, uint32 col, uint16 *fatBuf, uint32 len)
{
   uint16 *video = pos(row, col, scr);

   ASSERT((scr < vgaNumScreens) && (scr != VGA_SCREEN_COS));
   ASSERT((screens[scr].state == VGA_SIMPLE) ||
          (screens[scr].state == VGA_EXTENDED));
   ASSERT(video + len <= last(scr) + 1);

   while (len--) {
       *video++ = *fatBuf++;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * VGA_Clear --
 *
 * 	Clear a region with a specific fat character
 *
 * Results:
 * 	None
 *
 * Side Effects:
 * 	None
 *
 *----------------------------------------------------------------------
 */
void
VGA_Clear(uint32 scr, uint32 row, uint32 col, uint32 numRows, uint32 numCols, uint16 clearFatChar)
{
   uint16 *video = pos(row, col, scr);
   uint32 len;

   ASSERT((scr < vgaNumScreens) && (scr != VGA_SCREEN_COS));
   ASSERT((screens[scr].state == VGA_SIMPLE) ||
          (screens[scr].state == VGA_EXTENDED));
   ASSERT(numRows >= 1);

   if (numRows == 1) { // Part of line
      ASSERT(numCols >= 1);
      ASSERT(col + numCols <= VGA_NUM_COLS);
      len = numCols;
   } else { // Whole lines
      ASSERT(col == 0);
      ASSERT(numCols == VGA_NUM_COLS);
      ASSERT(row + numRows <= screens[scr].numRows);
      len = numRows*VGA_NUM_COLS;
   }

   while (len--) {
      *video++ = clearFatChar;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * VGA_Cursor --
 *
 * 	Display/Hide cursor
 *
 * Results:
 * 	None
 *
 * Side Effects:
 * 	None
 *
 *----------------------------------------------------------------------
 */
void
VGA_Cursor(uint32 scr, uint32 row, uint32 col, uint8 height)
{
   static uint8 curHeight = -1;
   const uint8 spacingHeight = 2/(screens[scr].state == VGA_EXTENDED ?
                                  VGA_EXTENSION_FACTOR : 1);
   SP_IRQL prevIRQL;
   uint8 curr;
   uint16 relpos = VGA_POS(row, col, scr);

   ASSERT(scr < vgaNumScreens);
   ASSERT((scr != VGA_SCREEN_COS) || (height == 0));
   ASSERT((screens[scr].state == VGA_SIMPLE) ||
          (screens[scr].state == VGA_EXTENDED));
   ASSERT((row < screens[scr].numRows) && (col < VGA_NUM_COLS));
   ASSERT(height < screens[scr].charHeight);

   // To hide the cursor, move it beyond the end of the visible area
   if (height == 0) {
      height = curHeight;
      relpos = VGA_LAST(scr, screens[scr].state == VGA_EXTENDED)+1;
   } else { // XXX only underline cursor for now
      height = spacingHeight;
   }

   prevIRQL = SP_LockIRQ(&vgaLock, SP_IRQL_KERNEL);

   // Place the cursor
   OUTB(VGA_CRTC_INDEX, VGA_CRTC_CURSOR_POS);
   OUTB(VGA_CRTC_VALUE, relpos >> 8);
   OUTB(VGA_CRTC_INDEX, VGA_CRTC_CURSOR_POS+1);
   OUTB(VGA_CRTC_VALUE, relpos & 0xFF);

   // Set shape
   if (height != curHeight) {

      // Set top according to height
      OUTB(VGA_CRTC_INDEX, VGA_CRTC_CURSOR);
      curr = INB(VGA_CRTC_VALUE);
      curr &= ~31; // preserve property bits
      curr |= screens[scr].charHeight - spacingHeight - height;
      OUTB(VGA_CRTC_INDEX, VGA_CRTC_CURSOR);
      OUTB(VGA_CRTC_VALUE, curr);

      // Set bottom
      OUTB(VGA_CRTC_INDEX, VGA_CRTC_CURSOR+1);
      curr = INB(VGA_CRTC_VALUE);
      curr &= ~31; // preserve property bits
      curr |= screens[scr].charHeight - spacingHeight - 1;
      OUTB(VGA_CRTC_INDEX, VGA_CRTC_CURSOR);
      OUTB(VGA_CRTC_VALUE, curr);

      curHeight = height;
   }

   SP_UnlockIRQ(&vgaLock, prevIRQL);
}


/*
 *----------------------------------------------------------------------
 *
 * VGA_Scroll --
 *
 * 	Scroll a region
 *
 * Results:
 * 	None
 *
 * Side Effects:
 * 	None
 *
 *----------------------------------------------------------------------
 */
void
VGA_Scroll(uint32 scr, uint32 topRow, uint32 bottomRow, uint32 numRows, Bool up, uint16 clearFatChar)
{
   uint16 *videoDst;
   uint16 *videoSrc;
   uint32 len;

   ASSERT((scr < vgaNumScreens) && (scr != VGA_SCREEN_COS));
   ASSERT((screens[scr].state == VGA_SIMPLE) ||
          (screens[scr].state == VGA_EXTENDED));
   ASSERT((topRow < screens[scr].numRows)&&(bottomRow <= screens[scr].numRows));
   ASSERT(bottomRow > topRow);
   ASSERT(numRows <= (bottomRow - topRow));
   
   if (up) {
      videoDst = pos(topRow, 0, scr);
      videoSrc = pos(topRow + numRows, 0, scr);
      len = (bottomRow - topRow - numRows)*VGA_NUM_COLS;
      while (len--) {
         *videoDst++ = *videoSrc++;
      }
      len = numRows*VGA_NUM_COLS;
      while (len--) {
         *videoDst++ = clearFatChar;
      }
   } else {
      videoDst = pos(bottomRow, 0, scr) - 1;
      videoSrc = pos(bottomRow - numRows, 0, scr) - 1;
      len = (bottomRow - topRow - numRows)*VGA_NUM_COLS;
      while (len--) {
	 *videoDst-- = *videoSrc--;
      }
      len = numRows*VGA_NUM_COLS;
      while (len--) {
	 *videoDst-- = clearFatChar;
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * VGALoadPalette --
 *
 * 	Load a palette
 *
 * Results:
 * 	None
 *
 * Side Effects:
 * 	None
 *
 *----------------------------------------------------------------------
 */
static void
VGALoadPalette(vgaPalette palette)
{
   SP_IRQL prevIRQL;
   int color;

   prevIRQL = SP_LockIRQ(&vgaLock, SP_IRQL_KERNEL);

   OUTB(VGA_PEL_WRITE_INDEX, 0);
   for (color = 0; color < NUM_COLORS; color++) {
      OUTB(VGA_PEL_VALUE, palette[color].red);
      OUTB(VGA_PEL_VALUE, palette[color].green);
      OUTB(VGA_PEL_VALUE, palette[color].blue);
   }

   SP_UnlockIRQ(&vgaLock, prevIRQL);
}


/*
 *----------------------------------------------------------------------
 *
 * VGA_Blank --
 *
 * 	Blank by loading all-black palette
 *
 * Results:
 * 	None
 *
 * Side Effects:
 * 	None
 *
 *----------------------------------------------------------------------
 */
void
VGA_Blank(void)
{
   VGALoadPalette(vgaPaletteBlank);
}


/*
 *----------------------------------------------------------------------
 *
 * VGAPrepareFontOp --
 *
 *      Prepare for Save/Load font
 *
 * Results:
 *      None
 *
 * Side Effects:
 *      VGA hardware is set up to save/load font
 *
 *----------------------------------------------------------------------
 */
static INLINE void
VGAPrepareFontOp(void)
{
   SP_IRQL prevIRQL;
   uint8 reg;

   /*
    * The font is used by the hardware to draw the screen, so it has
    * to be reset to allow access to the font buffer by the CPU.
    */

   prevIRQL = SP_LockIRQ(&vgaLock, SP_IRQL_KERNEL);

   // Synchronous reset
   OUTB(VGA_SEQUENCER_INDEX, VGA_SEQUENCER_RESET);
   OUTB(VGA_SEQUENCER_VALUE, 0x01);
   // Restrict CPU access to plane 2 which contains fonts
   OUTB(VGA_SEQUENCER_INDEX, VGA_SEQUENCER_MAPMASK);
   OUTB(VGA_SEQUENCER_VALUE, 1<<2);
   // Enable sequential access
   OUTB(VGA_SEQUENCER_INDEX, VGA_SEQUENCER_MEMMODE);
   OUTB(VGA_SEQUENCER_VALUE, 0x07);
   // Done with reset
   OUTB(VGA_SEQUENCER_INDEX, VGA_SEQUENCER_RESET);
   OUTB(VGA_SEQUENCER_VALUE, 0x03);
   // Select plane 2 for read mode 0
   OUTB(VGA_GRAPHICS_INDEX, VGA_GRAPHICS_MAPSELECT);
   OUTB(VGA_GRAPHICS_VALUE, 2);
   // Enable sequential addressing in the plane and read mode 0
   OUTB(VGA_GRAPHICS_INDEX, VGA_GRAPHICS_MODE);
   OUTB(VGA_GRAPHICS_VALUE, 0x00);
   // Enable sequential access
   OUTB(VGA_GRAPHICS_INDEX, VGA_GRAPHICS_MISC);
   reg = INB(VGA_GRAPHICS_VALUE);
   reg &= ~0x02;
   OUTB(VGA_GRAPHICS_INDEX, VGA_GRAPHICS_MISC);
   OUTB(VGA_GRAPHICS_VALUE, reg);

   SP_UnlockIRQ(&vgaLock, prevIRQL);
}


/*
 *----------------------------------------------------------------------
 *
 * VGAConcludeFontOp --
 *
 *      Conclude after Save/Load font
 *
 * Results:
 *      None
 *
 * Side Effects:
 *      VGA hardware is set up for normal operation
 *
 *----------------------------------------------------------------------
 */
static INLINE void
VGAConcludeFontOp(uint8 height)
{
   SP_IRQL prevIRQL;
   uint8 reg;

   prevIRQL = SP_LockIRQ(&vgaLock, SP_IRQL_KERNEL);

   // Synchronous reset
   OUTB(VGA_SEQUENCER_INDEX, VGA_SEQUENCER_RESET);
   OUTB(VGA_SEQUENCER_VALUE, 0x01);
   // Grant CPU access to planes 0 and 1 which contain characters and attributes
   OUTB(VGA_SEQUENCER_INDEX, VGA_SEQUENCER_MAPMASK);
   OUTB(VGA_SEQUENCER_VALUE, (1<<0)|(1<<1));
   // Enable interleaved access
   OUTB(VGA_SEQUENCER_INDEX, VGA_SEQUENCER_MEMMODE);
   OUTB(VGA_SEQUENCER_VALUE, 0x03);
   // Done with reset
   OUTB(VGA_SEQUENCER_INDEX, VGA_SEQUENCER_RESET);
   OUTB(VGA_SEQUENCER_VALUE, 0x03);
   // Select plane 0 for read mode 0 (default)
   OUTB(VGA_GRAPHICS_INDEX, VGA_GRAPHICS_MAPSELECT);
   OUTB(VGA_GRAPHICS_VALUE, 0);
   // Enable interleaved addessing
   OUTB(VGA_GRAPHICS_INDEX, VGA_GRAPHICS_MODE);
   OUTB(VGA_GRAPHICS_VALUE, 0x10);
   // Enable interleaved addessing
   OUTB(VGA_GRAPHICS_INDEX, VGA_GRAPHICS_MISC);
   reg = INB(VGA_GRAPHICS_VALUE);
   reg |= 0x02;
   OUTB(VGA_GRAPHICS_INDEX, VGA_GRAPHICS_MISC);
   OUTB(VGA_GRAPHICS_VALUE, reg);

   if (height) {
      /*
       * Adjust displayed font height.
       * NOTE: Since the fonts are exact multiple, nothing changes except the
       * font height, e.g. total number of scan lines or end of display stay
       * the same).
       */
      OUTB(VGA_CRTC_INDEX, VGA_CRTC_MAXSCANLINES);
      reg = INB(VGA_CRTC_VALUE);
      reg = (reg & 0xE0) | (height-1);
      OUTB(VGA_CRTC_INDEX, VGA_CRTC_MAXSCANLINES);
      OUTB(VGA_CRTC_VALUE, reg);
   }

   SP_UnlockIRQ(&vgaLock, prevIRQL);
}


/*
 *----------------------------------------------------------------------
 *
 * VGASaveFont --
 *
 *      Save font
 *
 * Results:
 *      None
 *
 * Side Effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
static void
VGASaveFont(uint8 *font, uint8 height)
{
   int i;
   uint8 *vgaFont = (uint8 *)vgaVideo;


   Log("%d pixels high", height);
   ASSERT(height == VGA_CHAR_HEIGHT);
   ASSERT(font != NULL);

   VGAPrepareFontOp();

   // Save the font
   for (i = 0; i < 256; i++) {
      /*
       * Each character takes up 32 bytes in the video font plane
       * (one byte per pixel of height).
       */
      memcpy(font, vgaFont, height);
      font += height;
      vgaFont += 32;
   }

   VGAConcludeFontOp(0);
}


/*
 *----------------------------------------------------------------------
 *
 * VGALoadFont --
 *
 *      Load Font
 *
 * Results:
 *      None
 *
 * Side Effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
static void
VGALoadFont(const uint8 *font, uint8 height)
{
   int i;
   uint8 *vgaFont = (uint8 *)vgaVideo;


   ASSERT((height == VGA_CHAR_HEIGHT) ||
          (height == VGA_CHAR_HEIGHT/VGA_EXTENSION_FACTOR));
   ASSERT(font != NULL);

   VGAPrepareFontOp();

   // Load the font
   for (i = 0; i < 256; i++) {
      /*
       * Each character takes up 32 bytes in the video font plane
       * (one byte per pixel of height).
       */
      memcpy(vgaFont, font, height);
      font += height;
      vgaFont += 32;
   }

   VGAConcludeFontOp(height);
}
