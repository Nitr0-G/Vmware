/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * serial.c --
 *
 *	This file talks to the serial port for debugging.  It is hardwired
 *	to use COM1 right now.
 */
#include "vm_types.h"
#include "vm_libc.h"
#include "vm_asm.h"
#include "x86.h"
#include "vmkernel.h"
#include "vmnix_if.h"
#include "kvmap.h"
#include "memmap.h"
#include "world.h"
#include "idt.h"
#include "serial.h"
#include "config.h"
#include "bluescreen.h"
#include "serial_ext.h"
#include "libc.h"
#include "isa.h"
#include "debug.h"
#include "dump.h"

#define LOGLEVEL_MODULE Serial
#define LOGLEVEL_MODULE_LEN 6
#include "log.h"

/*
 * Definitions for the different registers.
 */
#define UART_RX		0	/* In:  Receive buffer (DLAB=0) */
#define UART_TX		0	/* Out: Transmit buffer (DLAB=0) */
#define UART_DLL	0	/* Out: Divisor Latch Low (DLAB=1) */
#define UART_DLM	1	/* Out: Divisor Latch High (DLAB=1) */
#define UART_IER	1	/* Out: Interrupt Enable Register */
#define UART_IIR	2	/* In:  Interrupt ID Register */
#define UART_FCR	2	/* Out: FIFO Control Register */
#define UART_EFR	2	/* I/O: Extended Features Register */
				/* (DLAB=1, 16C660 only) */
#define UART_LCR	3	/* Out: Line Control Register */
#define UART_MCR	4	/* Out: Modem Control Register */
#define UART_LSR	5	/* In:  Line Status Register */
#define UART_MSR	6	/* In:  Modem Status Register */
#define UART_SCR	7	/* I/O: Scratch Register */

/*
 * These are the definitions for the FIFO Control Register
 */
#define UART_FCR_ENABLE_FIFO	0x01 /* Enable the FIFO */
#define UART_FCR_CLEAR_RCVR	0x02 /* Clear the RCVR FIFO */
#define UART_FCR_CLEAR_XMIT	0x04 /* Clear the XMIT FIFO */
#define UART_FCR_DMA_SELECT	0x08 /* For DMA applications */
#define UART_FCR_TRIGGER_MASK	0xC0 /* Mask for the FIFO trigger range */
#define UART_FCR_TRIGGER_1	0x00 /* Mask for trigger set at 1 */
#define UART_FCR_TRIGGER_4	0x40 /* Mask for trigger set at 4 */
#define UART_FCR_TRIGGER_8	0x80 /* Mask for trigger set at 8 */
#define UART_FCR_TRIGGER_14	0xC0 /* Mask for trigger set at 14 */

/*
 * These are the definitions for the Line Control Register
 * 
 * Note: if the word length is 5 bits (UART_LCR_WLEN5), then setting 
 * UART_LCR_STOP will select 1.5 stop bits, not 2 stop bits.
 */
#define UART_LCR_DLAB	0x80	/* Divisor latch access bit */
#define UART_LCR_SBC	0x40	/* Set break control */
#define UART_LCR_SPAR	0x20	/* Stick parity (?) */
#define UART_LCR_EPAR	0x10	/* Even parity select */
#define UART_LCR_PARITY	0x08	/* Parity Enable */
#define UART_LCR_STOP	0x04	/* Stop bits: 0=1 stop bit, 1= 2 stop bits */
#define UART_LCR_WLEN5  0x00	/* Wordlength: 5 bits */
#define UART_LCR_WLEN6  0x01	/* Wordlength: 6 bits */
#define UART_LCR_WLEN7  0x02	/* Wordlength: 7 bits */
#define UART_LCR_WLEN8  0x03	/* Wordlength: 8 bits */

/*
 * These are the definitions for the Line Status Register
 */
#define UART_LSR_TEMT	0x40	/* Transmitter empty */
#define UART_LSR_THRE	0x20	/* Transmit-hold-register empty */
#define UART_LSR_BI	0x10	/* Break interrupt indicator */
#define UART_LSR_FE	0x08	/* Frame error indicator */
#define UART_LSR_PE	0x04	/* Parity error indicator */
#define UART_LSR_OE	0x02	/* Overrun error indicator */
#define UART_LSR_DR	0x01	/* Receiver data ready */

/*
 * These are the definitions for the Interrupt Identification Register
 */
#define UART_IIR_NO_INT	0x01	/* No interrupts pending */
#define UART_IIR_ID	0x06	/* Mask for the interrupt ID */

#define UART_IIR_MSI	0x00	/* Modem status interrupt */
#define UART_IIR_THRI	0x02	/* Transmitter holding register empty */
#define UART_IIR_RDI	0x04	/* Receiver data interrupt */
#define UART_IIR_RLSI	0x06	/* Receiver line status interrupt */

/*
 * These are the definitions for the Interrupt Enable Register
 */
#define UART_IER_MSI	0x08	/* Enable Modem status interrupt */
#define UART_IER_RLSI	0x04	/* Enable receiver line status interrupt */
#define UART_IER_THRI	0x02	/* Enable Transmitter holding register int. */
#define UART_IER_RDI	0x01	/* Enable receiver data interrupt */

/*
 * These are the definitions for the Modem Control Register
 */
#define UART_MCR_LOOP	0x10	/* Enable loopback test mode */
#define UART_MCR_OUT2	0x08	/* Out2 complement */
#define UART_MCR_OUT1	0x04	/* Out1 complement */
#define UART_MCR_RTS	0x02	/* RTS complement */
#define UART_MCR_DTR	0x01	/* DTR complement */

/*
 * These are the definitions for the Modem Status Register
 */
#define UART_MSR_DCD	0x80	/* Data Carrier Detect */
#define UART_MSR_RI	0x40	/* Ring Indicator */
#define UART_MSR_DSR	0x20	/* Data Set Ready */
#define UART_MSR_CTS	0x10	/* Clear to Send */
#define UART_MSR_DDCD	0x08	/* Delta DCD */
#define UART_MSR_TERI	0x04	/* Trailing edge ring indicator */
#define UART_MSR_DDSR	0x02	/* Delta DSR */
#define UART_MSR_DCTS	0x01	/* Delta CTS */
#define UART_MSR_ANY_DELTA 0x0F	/* Any of the delta bits! */

#define COM1_PORT           0x3f8
#define COM2_PORT           0x2f8
#define COM3_PORT           0x3e0
#define COM4_PORT           0x2e8

extern Bool vmkernelLoaded;

static void SerialInterrupt(void *clientData, uint32 vector);
static void SerialPrintfPutc(int ch, void* cookie);

static uint16 comPort;
static Bool serialIRQInitialized = FALSE;
static Bool serialPortInitialized = FALSE;

/*
 *----------------------------------------------------------------------
 *
 * SerialInitPort --
 *
 *      Reset and initialize the COM port hardware
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      COM1/COM2 is initialized.
 *
 *----------------------------------------------------------------------
 */
static void 
SerialInitPort(uint16 port)
{
   int dummy;
   int baudRateDLL;

   comPort = port;
   /*
    * Disable and clear the FIFOs
    */
   OUTB(UART_FCR + port, UART_FCR_CLEAR_RCVR | UART_FCR_CLEAR_XMIT);

   /*
    * Set to 8 bits, 1 stop bit, no parity, break control on
    */
   OUTB(UART_LCR + port, UART_LCR_WLEN8 | UART_LCR_SBC);

   /* 
    * Set baud rate.  The formula is:
    *
    *     baud rate = 115200 / (UART_DLM << 8 | UART_DLL)
    */
   if (CONFIG_OPTION(SERIAL_BAUD_RATE) < SERIAL_MIN_BAUD_RATE) {
      CONFIG_OPTION(SERIAL_BAUD_RATE) = SERIAL_MAX_BAUD_RATE;
   }
   baudRateDLL = 115200 / CONFIG_OPTION(SERIAL_BAUD_RATE);
   if (baudRateDLL == 0) {
      /*
       * This should never happen, but just to be safe ...
       */
      baudRateDLL = 1;
   }

   OUTB(UART_LCR + port, UART_LCR_DLAB | UART_LCR_WLEN8);
   OUTB(UART_DLM + port, 0x00);
   OUTB(UART_DLL + port, baudRateDLL);
   OUTB(UART_LCR + port, UART_LCR_WLEN8);

   dummy = INB(UART_LSR + port);
   dummy = INB(UART_RX + port);

   /*
    * Enable the FIFOs.
    */
   OUTB(UART_FCR + port, UART_FCR_ENABLE_FIFO | UART_FCR_TRIGGER_8);

   /* 
    * Enable recv interrupts.
    */
   OUTB(UART_MCR + port, UART_MCR_OUT2);
   OUTB(UART_IER + port, UART_IER_RDI);

   serialPortInitialized = TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * SerialRegisterIRQ --
 *
 *      register the serial port interrupt handler at IRQ irq
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      irq handler tables
 *
 *----------------------------------------------------------------------
 */
static void
SerialRegisterIRQ(IRQ irq)
{
   uint32 vector;
   Bool ok;

   vector = ISA_GetDeviceVector(irq);
   if (vector == 0) {
      Warning("Couldn't map irq %d", irq);
      return;
   }

   ok = IDT_VectorAddHandler(vector, SerialInterrupt, NULL, FALSE, "serial",
				IDT_EDGE|IDT_ISA);
   serialIRQInitialized = TRUE;
   if (!ok) {
      Warning("Couldn't register irq %d at vector 0x%x", irq, vector);
      return;
   }

   // Since it's ISA, it's exclusive so we may need to steal it from COS
   IDT_VectorDisable(vector, IDT_HOST);
   IDT_VectorEnable(vector, IDT_VMK);
}


/*
 *----------------------------------------------------------------------
 *
 * Serial_EarlyInit --
 *
 *      Initialize the serial line for debugging.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      COM1/COM2 is initialized.
 *
 *----------------------------------------------------------------------
 */
void
Serial_EarlyInit(VMnix_ConfigOptions *vmnixOptions)
{
   uint16 port;

   if (vmnixOptions->serialPort == 0) {
      return;
   }

   CONFIG_OPTION(SERIAL_PORT) = vmnixOptions->serialPort;
   port = (vmnixOptions->serialPort == 2) ? COM2_PORT : COM1_PORT;

   if (vmnixOptions->baudRate <= SERIAL_MAX_BAUD_RATE && 
       vmnixOptions->baudRate >= SERIAL_MIN_BAUD_RATE &&
       vmnixOptions->baudRate % SERIAL_MIN_BAUD_RATE == 0) {
      CONFIG_OPTION(SERIAL_BAUD_RATE) = vmnixOptions->baudRate;
   } else {
      CONFIG_OPTION(SERIAL_BAUD_RATE) = SERIAL_MAX_BAUD_RATE;
   }

   SerialInitPort(port);
}


/*
 *----------------------------------------------------------------------
 *
 * Serial_LateInit --
 *
 *      Initialize the serial line for debugging.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      COM1/COM2 is initialized.
 *
 *----------------------------------------------------------------------
 */
void
Serial_LateInit(VMnix_ConfigOptions *vmnixOptions)
{   
   IRQ irq;

   if (vmnixOptions->serialPort == 0) {
      return;
   }

   irq = (vmnixOptions->serialPort == 2) ? SERIAL2_IRQ : SERIAL_IRQ;
   SerialRegisterIRQ(irq);

   Log("using COM%d", vmnixOptions->serialPort);
}


/*
 *----------------------------------------------------------------------
 *
 * Serial_OpenPort --
 *
 *      Emergency initialization of the serial line for debugging.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      COM1/COM2 is initialized.
 *
 *----------------------------------------------------------------------
 */
void
Serial_OpenPort(uint32 portNum)
{
   if (!serialPortInitialized) {
      uint16 portAddr = (portNum == 2) ? COM2_PORT : COM1_PORT;
      SerialInitPort(portAddr);
   }

   if (!serialIRQInitialized && vmkernelLoaded) {
      IRQ irq = (portNum == 2) ? SERIAL2_IRQ : SERIAL_IRQ;
      SerialRegisterIRQ(irq);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * SerialInterrupt --
 *
 *      Handle an interrupt on the serial port.
 *
 * Results:
 *      Tell the caller to unmask the interrupt.
 *
 * Side effects:
 *      Characters may be pulled off off of COM1.
 *
 *----------------------------------------------------------------------
 */
void
SerialInterrupt(UNUSED_PARAM(void *clientData),
                UNUSED_PARAM(uint32 vector))
{
   if (!(INB(UART_IIR + comPort) & UART_IIR_NO_INT)) {
      while (INB(UART_LSR + comPort) & UART_LSR_DR) {
	 uint8 ch = INB(UART_RX + comPort);
	 if (ch == SERIAL_FORCE_BREAKPOINT) {
	    IDT_WantBreakpoint();
	 } else if (ch == SERIAL_FORCE_DUMP_AND_BREAK) {
	    IDT_WantBreakpoint();
	    Dump_RequestLiveDump();
	 } else if (ch == SERIAL_FORCE_DUMP) {
	    Dump_RequestLiveDump();
	 } else if (ch == SERIAL_WANT_SERIAL) {
	    Debug_SetSerialDebugging(TRUE);
	 }
      }   
   }
}

/*
 *----------------------------------------------------------------------
 *
 * Serial_GetChar --
 *
 *      Wait for the next character to show up on the serial port, read it,
 *	and return it.
 *
 * Results:
 *      The next character from COM1.
 *
 * Side effects:
 *      A character will be taken from COM1.
 *
 *----------------------------------------------------------------------
 */
unsigned char
Serial_GetChar(void)
{
   if (serialPortInitialized) {
      unsigned char ch;
      while (!(INB(UART_LSR + comPort) & UART_LSR_DR)) {
      }

      ch = INB(UART_RX + comPort);

      return ch;
   } else {
      return '0';
   }
}

/*
 *----------------------------------------------------------------------
 *
 * Serial_PollChar --
 *
 *      Return the next character if one is available.
 *
 * Results:
 *      The next character from COM1.
 *
 * Side effects:
 *      A character will be taken from COM1.
 *
 *----------------------------------------------------------------------
 */
unsigned char
Serial_PollChar(void)
{
   unsigned char ch = 0;
   if (serialPortInitialized) {
      if (INB(UART_LSR + comPort) & UART_LSR_DR) {
	 ch = INB(UART_RX + comPort);
      }
   }

   return ch;
}

/*
 *----------------------------------------------------------------------
 *
 * Serial_PutChar --
 *
 *      Write a character to the serial port.
 *
 * Results:
 *      TRUE if successful, FALSE otherwise
 *
 * Side effects:
 *      A character will be written to the debug port
 *
 *----------------------------------------------------------------------
 */
Bool
Serial_PutChar(unsigned char ch)
{
   if (serialPortInitialized) {
      while (!(INB(UART_LSR + comPort) & UART_LSR_THRE)) {
      }
      OUTB(UART_TX + comPort, ch);
   }
   return TRUE;
}

/*
 *----------------------------------------------------------------------
 *
 * Serial_PutLenString --
 *
 *      Write a string to the serial port.
 *
 * Results:
 *      TRUE if successful, FALSE otherwise
 *
 * Side effects:
 *      The string be written to the debug port
 *
 *----------------------------------------------------------------------
 */ 
void
Serial_PutLenString(const unsigned char *str, int len)
{
   int i;

   for (i = 0; (i < len) && (str[i] != 0); i++) {
      Serial_PutChar(str[i]);
      if (str[i] == '\n') {
         Serial_PutChar('\r');
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * Serial_PutString --
 *
 *      Write a null terminated string to the serial port.
 *
 * Results:
 *      TRUE if successful, FALSE otherwise
 *
 * Side effects:
 *      The string be written to the debug port
 *
 *----------------------------------------------------------------------
 */ 
void
Serial_PutString(const unsigned char *str)
{
   Serial_PutLenString(str, strlen(str));
}

/*
 *----------------------------------------------------------------------
 *
 * Serial_GetPort --
 *
 *      Return the com port that is being used.
 *
 * Results:
 *      IRQ
 *
 * Side effects:
 *      none
 *
 *----------------------------------------------------------------------
 */
VMKERNEL_ENTRY
Serial_GetPort(DECLARE_1_ARG(VMK_GET_SERIAL_PORT, int32 *, port))
{
   PROCESS_1_ARG(VMK_GET_SERIAL_PORT, int32 *, port);

   if (serialPortInitialized) {
      *port = comPort;
   } else {
      *port = -1;
   }
   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * SerialPrintfPutc --
 *      Helper function for Serial_PrintfVarArgs.  Callback for
 *      printing out a single character to serial port.
 *	Does "\n" => "\n\r" conversion, too.
 *
 * 	Todo: could use the cookie to turn conversion on/off.
 *
 * Results:
 *	None
 *
 * Side effects:
 *      Outputs given character to serial port
 *
 *----------------------------------------------------------------------
 */
static void
SerialPrintfPutc(int ch, void* cookie)
{
   Serial_PutChar((unsigned char)ch);
   if (ch == '\n') {
      Serial_PutChar((unsigned char)'\r');
   }      
}

/*
 *----------------------------------------------------------------------
 *
 * Serial_PrintfVarArgs --
 *      Formatted serial output
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      Outputs characters to serial port
 *
 *----------------------------------------------------------------------
 */
void
Serial_PrintfVarArgs(const char *fmt, va_list args)
{
   Printf_WithFunc(fmt, SerialPrintfPutc, NULL, args);
}

/*
 *----------------------------------------------------------------------
 *
 * Serial_Printf --
 *      Formatted serial output.
 *
 * Results:
 *	None
 *
 * Side effects:
 *      Outputs characters to serial port
 *
 *----------------------------------------------------------------------
 */
void 
Serial_Printf(const char *fmt, ...)
{
   va_list args;
   va_start(args, fmt);
   Serial_PrintfVarArgs(fmt, args);
   va_end(args);
}

