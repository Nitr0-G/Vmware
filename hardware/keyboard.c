/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * keyboard.c --
 *
 *    vmkernel interface to the keyboard driver	
 */
#include "vm_basic_types.h"
#include "vm_basic_defs.h"
#include "vmkernel.h"
#include "list.h"
#include "keyboard.h"
#include "isa.h"
#include "idt.h"
#include "bh.h"

#define VMK_KBD
#include "keyboard/misc.h"
#include "keyboard/kbio.h"
#include "keyboard/atkbdreg.h"
#include "keyboard/kbdreg.h"

#define LOGLEVEL_MODULE Keyboard
#define LOGLEVEL_MODULE_LEN 8
#include "log.h"

struct keyboard;
static struct keyboard *vmkKBD = NULL;
static Bool vmkInitialized = FALSE;

static SP_SpinLockIRQ keyboardIRQLock;
static SP_SpinLock keyboardLock;
static Bool keyboardIntSetup = FALSE;
static uint32 keyboardBHNum;
static Keyboard_Callback keyboardCallback = NULL;
static uint32 keyboardVector;
static Keyboard_Audience keyboardAudience = KEYBOARD_COS;
static char keyboardHostChar = 0;

static void KeyboardDoSetAudience(void);
static void KeyboardIntrHandler(void *clientData, uint32 vector);
static void KeyboardBH(UNUSED_PARAM(void *clientData));


/*
 *-----------------------------------------------------------
 *
 * Keyboard_EarlyInit --
 *
 *    Initialize the vmkernel keyboard driver.
 *
 * Results:
 *    none.
 *
 * Side effects:
 *    The keyboard is probed and all the driver structures are
 *    initialized.
 *    The keyboard is usable in poll mode.
 *
 *-----------------------------------------------------------
 */
void
Keyboard_EarlyInit(void)
{
   Log("");

   ASSERT(!vmkInitialized);
   
   // Around idt lock and it lock
   SP_InitLockIRQ("kbdIRQLck", &keyboardIRQLock, SP_RANK_IRQ_MEMTIMER-1);

   // Around everything
   SP_InitLock("kbdLck", &keyboardLock, SP_RANK_LOWEST);

   // Configure the low level 'AT' keyboard driver
   if (atkbd_configure(&vmkKBD) != VMK_OK) {
      Warning("Couldn't configure keyboard");
      return;
   }

   vmkInitialized = TRUE;
}

/*
 *-----------------------------------------------------------
 *
 * Keyboard_Init --
 *
 *    Initialize the keyboard interrupt setup
 *
 * Results:
 *    none.
 * 
 * Side Effects:
 *    The keyboard is usable in interrupt mode
 *
 *-----------------------------------------------------------
 */
void
Keyboard_Init(void)
{
   Bool ok;

   Log("");

   ASSERT(vmkInitialized);
   ASSERT(!keyboardIntSetup);

   // Get the vector
   keyboardVector = ISA_GetDeviceVector(KEYBOARD_IRQ);
   if (keyboardVector == 0) {
      Warning("Couldn't map irq %d", KEYBOARD_IRQ);
      return;
   }

   // Register the bottom half
   keyboardBHNum = BH_Register(KeyboardBH, NULL);

   // Hook up the interrupt handler
   ok = IDT_VectorAddHandler(keyboardVector, KeyboardIntrHandler,
				NULL, FALSE, "keyboard", IDT_EDGE|IDT_ISA);
   if (!ok) {
      Warning("Couldn't register irq %d at vector 0x%x",
				KEYBOARD_IRQ, keyboardVector);
      return;
   }

   keyboardIntSetup = TRUE;

   // Set the correct audience
   KeyboardDoSetAudience();
}

/*
 *-----------------------------------------------------------
 *
 * KeyboardDoSetAudience --
 *
 *    Set who gets the keyboard interrupts
 *
 * Results:
 *    none.
 *
 * Side Effects:
 *    Keyboard interrupt is enabled/disabled for host/vmkernel
 *
 *-----------------------------------------------------------
 */
static void
KeyboardDoSetAudience(void)
{
   ASSERT(keyboardIntSetup);

   // Let's first disable for both
   IDT_VectorDisable(keyboardVector, IDT_HOST);
   IDT_VectorDisable(keyboardVector, IDT_VMK);

   // Now re-enable for the correct one
   switch (keyboardAudience) {
   case KEYBOARD_NONE:
      break;
   case KEYBOARD_COS:
      IDT_VectorEnable(keyboardVector, IDT_HOST);
      break;
   case KEYBOARD_VMK:
      IDT_VectorEnable(keyboardVector, IDT_VMK);
      break;
   default:
      ASSERT(FALSE);
      break;
   }
}

/*
 *-----------------------------------------------------------
 *
 * Keyboard_SetAudience --
 *
 *    Set who gets the keyboard interrupts
 *
 * Results:
 *    none.
 *
 * Side Effects:
 *    Keyboard interrupt is enabled/disabled for host/vmkernel
 *
 *-----------------------------------------------------------
 */
void
Keyboard_SetAudience(Keyboard_Audience audience)
{
   SP_IRQL prevIRQL;

   prevIRQL = SP_LockIRQ(&keyboardIRQLock, SP_IRQL_KERNEL);

   if (audience != keyboardAudience) { // Change requested
      /*
       * If the keyboard interrupt has not been set up yet, we cannot
       * set the correct audience, so just record what's wanted.
       */
      keyboardAudience = audience;
      if (keyboardIntSetup) {
	 KeyboardDoSetAudience();
      }
   }

   SP_UnlockIRQ(&keyboardIRQLock, prevIRQL);
}

/*
 *-----------------------------------------------------------
 *
 * Keyboard_SetCallback --
 *
 *    Set callback on keyboard events
 *
 * Results:
 *    none.
 *
 * Side Effects:
 *    none.
 *
 *-----------------------------------------------------------
 */
void
Keyboard_SetCallback(Keyboard_Callback callback)
{
   SP_Lock(&keyboardLock);
   keyboardCallback = callback;
   SP_Unlock(&keyboardLock);
}


/*
 *-----------------------------------------------------------
 *
 * KeyboardIntrHandler --
 *    
 *    Keyboard interrupt handler.
 *
 * Results:
 *    none.
 *
 * Side effects:
 *    Schedule the bottom half
 *
 *-----------------------------------------------------------
 */
static void
KeyboardIntrHandler(UNUSED_PARAM(void *clientData), UNUSED_PARAM(uint32 vector))
{
   ASSERT(vmkInitialized);
   atkbd_intr(vmkKBD, NULL);
   BH_SetLocalPCPU(keyboardBHNum);
}

/*
 *-----------------------------------------------------------
 *
 * KeyboardBH --
 *
 *    Keyboard bottom half
 *
 * Results:
 *    none.
 *
 * Side effects:
 *    none.
 *
 *-----------------------------------------------------------
 */
static void
KeyboardBH(UNUSED_PARAM(void *clientData))
{
   SP_Lock(&keyboardLock);
   if (keyboardCallback) {
      keyboardCallback();
   }
   SP_Unlock(&keyboardLock);
}

/*
 *-----------------------------------------------------------
 *
 * Keyboard_Read --
 *    
 *    Get the next available character
 *
 * Results:
 *    The next available character is returned or 0 if none available
 *
 * Side effects:
 *    none.
 *
 *-----------------------------------------------------------
 */
char
Keyboard_Read(void)
{
   char c;
   
   /*
    * This should be called only from the callback function so
    * keyboardLock is held.
    */
   ASSERT(SP_IsLocked(&keyboardLock));

   /*
    * If there is a host key, it is returned first.
    */
   if (keyboardHostChar) {
      c = keyboardHostChar;
      keyboardHostChar = 0;
   } else {
      ASSERT(vmkInitialized);
      if (kbd_getchars(&c, 1) == 0) { // No available character
         c = 0;
      }
   }

   return c;
}

/*
 *-----------------------------------------------------------
 *
 * Keyboard_Poll --
 *
 * 	Poll keyboard for the next available character
 *
 * Results:
 * 	The next available character is returned or 0 if none available
 *
 * Side Effects:
 * 	An interrupt is simulated to poll the keyboard
 *
 *-----------------------------------------------------------
 */
char
Keyboard_Poll(void)
{
   char c;

   // No reason to call this function if interrupts are enabled
   ASSERT_NO_INTERRUPTS();

   if (kbd_getchars(&c, 1) == 0) { // No available character
      // Simulate an interrupt
      ASSERT(vmkInitialized);
      atkbd_intr(vmkKBD, NULL);
      if (kbd_getchars(&c, 1) == 0) { // Still no available character
	 c = 0;
      }
   }

   return c;
}

/*
 *-----------------------------------------------------------
 *
 * Keyboard_ForwardKeyFromHost --
 *
 * 	While in charge of the keyboard, the host received a key
 * 	that we want to process instead.
 *
 * Results:
 * 	None
 *
 * Side Effects:
 * 	None
 *
 *-----------------------------------------------------------
 */
void
Keyboard_ForwardKeyFromHost(char c)
{
   /*
    * We are going to call the keyboard BH and it cannot be done
    * in an interrupt context.
    */
   ASSERT_HAS_INTERRUPTS();

   SP_Lock(&keyboardLock);
   // We don't buffer more than one char, the keyboard is really slow
   keyboardHostChar = c;
   if (keyboardCallback) {
      keyboardCallback();
   }
   SP_Unlock(&keyboardLock);
}
