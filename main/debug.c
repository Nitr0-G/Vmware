/****************************************************************************

		THIS SOFTWARE IS NOT COPYRIGHTED

   HP offers the following for use in the public domain.  HP makes no
   warranty with regard to the software or it's performance and the
   user accepts the software "AS IS" with all faults.

   HP DISCLAIMS ANY WARRANTIES, EXPRESS OR IMPLIED, WITH REGARD
   TO THIS SOFTWARE INCLUDING BUT NOT LIMITED TO THE WARRANTIES
   OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.

****************************************************************************/

/****************************************************************************
 *  Header: remcom.c,v 1.34 91/03/09 12:29:49 glenne Exp $
 *
 *  Module name: remcom.c $
 *  Revision: 1.34 $
 *  Date: 91/03/09 12:29:49 $
 *  Contributor:     Lake Stevens Instrument Division$
 *
 *  Description:     low level support for gdb debugger. $
 *
 *  Considerations:  only works on target hardware $
 *
 *  Written by:      Glenn Engel $
 *  ModuleState:     Experimental $
 *
 *  NOTES:           See Below $
 *
 *  Modified for 386 by Jim Kingdon, Cygnus Support.
 *
 *  To enable debugger support, two things need to happen.  One, a
 *  call to set_debug_traps() is necessary in order to allow any breakpoints
 *  or error conditions to be properly intercepted and reported to gdb.
 *  Two, a breakpoint needs to be generated to begin communication.  This
 *  is most easily accomplished by a call to breakpoint().  Breakpoint()
 *  simulates a breakpoint by executing a trap #1.
 *
 *  The external function IDT_RegisterDebugHandler() is
 *  used to attach a specific handler to a specific 386 vector number.
 *  It should use the same privilege level it runs at.  It should
 *  install it as an interrupt gate so that interrupts are masked
 *  while the handler runs.
 *  Also, need to assign exceptionHook and oldExceptionHook.
 *
 *  Because gdb will sometimes write to the stack area to execute function
 *  calls, this program cannot rely on using the supervisor stack so it
 *  uses it's own stack area reserved in the int array debugStack.
 *
 *************
 *
 *    The following gdb commands are supported:
 *
 * command          function                               Return value
 *
 *    g             return the value of the CPU registers  hex data or ENN
 *    G             set the value of the CPU registers     OK or ENN
 *
 *    mAA..AA,LLLL  Read LLLL bytes at address AA..AA      hex data or ENN
 *    MAA..AA,LLLL: Write LLLL bytes at address AA.AA      OK or ENN
 *
 *    c             Resume at current address              SNN    (signal NN)
 *    cAA..AA       Continue at address AA..AA             SNN
 *
 *    s             Step one instruction                   SNN
 *    sAA..AA       Step one instruction from AA..AA       SNN
 *
 *    k             kill
 *
 *    ?             What was the last sigval ?             SNN   (signal NN)
 *
 * All commands and responses are sent with a packet which includes a
 * checksum.  A packet consists of
 *
 * $<packet info>#<checksum>.
 *
 * where
 * <packet info> :: <characters representing the command or response>
 * <checksum>    :: < two hex digits computed as modulo 256 sum of <packetinfo>>
 *
 * When a packet is received, it is first acknowledged with either '+' or '-'.
 * '+' indicates a successful transfer.  '-' indicates a failed transfer.
 *
 * Example:
 *
 * Host:                  Reply:
 * $m0,10#2a               +$00010203040506070809101112131415#42
 *
 ****************************************************************************/

#include "vm_types.h"
#include "vm_asm.h"
#include "vm_libc.h"
#include "vmkernel.h"
#include "idt.h"
#include "serial.h"
#include "host.h"
#include "sched.h"
#include "nmi.h"
#include "netDebug.h"
#include "util.h"
#include "bluescreen.h"
#include "pagetable.h"
#include "sched.h"
#include "world.h"
#include "tlb.h"
#include "memalloc.h"
#include "debug.h"
#include "serial_ext.h"
#include "kseg.h"
#include "debugAsm.h"
#include "user.h"
#include "term.h"
#include "keyboard.h"
#include "debugterm.h"

/*
 * The log function for the debugger.  Conforms to standard log-level semantics
 * (ie, level 0 is always printed, otherwise, log messages are printed if their
 * level is <= the current log level.
 *
 * Set debugLogLevel at compile time or through the debugger to change log
 * levels.
 */
#define DEBUG_LOG(level, fmt, args...)		\
	if (level <= debugLogLevel) {		\
	   _Log(fmt , ## args);			\
	}

static Task *debugTask;

/*
 * A special assert macro is required here to release the network lock.  Upon
 * assertion fail, an exception is generated that causes the debugger to break
 * back into itself.  As it restarts, the debugger will attempt to re-acquire
 * the network lock.  Thus, we must free it here.
 */
#define DEBUG_PANIC(fmt, args...) {                                         \
   Debug_CnxStop(&kernCtx);                                                 \
   Panic(fmt "\ndbgWld: %p lDbgWld: %p iThd:%d cT:%d ot:%d nTh:%d\n",       \
	 ## args, worldInDebugger.val, lastWorldInDebugger.val,             \
	 initialGDBThread, csTarget, otherTarget, numThreads);              \
}

#define DEBUG_ASSERT(_c) {                                                  \
   if (! (_c)) {                                                            \
      DEBUG_PANIC("DEBUG_ASSERT failed: vmkernel/main/debug.c:%d: %s",      \
                  __LINE__, #_c);                                           \
   }                                                                        \
}

#define DEBUG_ASSERT_IS_VALID_THREAD(threadId) {			    \
   if (!DEBUG_IS_VALID_THREAD(threadId)) {                                  \
      DEBUG_PANIC("DEBUG_ASSERT failed: vmkernel/main/debug.c:%d: "         \
		  "Invalid thread id: %d", __LINE__, threadId);             \
   }                                                                        \
}
  
#define DEBUG_INVALID_THREAD	(Thread_ID)-2
#define DEBUG_ALL_THREADS	(Thread_ID)-1
#define DEBUG_ANY_THREAD	(Thread_ID)0
#define DEBUG_IS_VALID_THREAD(n) (n >= 1  && n <= numThreads)

#define DEBUG_INVALID_WORLD	INVALID_WORLD_ID

/*
 * When wantReset is TRUE, KEYBOARD_CMD_RESET is sent to the KEYBOARD_CMD_PORT
 * port, causing the machine to reset.
 */
#define KEYBOARD_CMD_PORT	0x64
#define KEYBOARD_CMD_RESET	0xfe

/*
 * Defines the maximum number of characters for in/out buffers.  The minimum
 * size for these buffers must be at least sizeof(DebugRegisterFile) * 2 for
 * register get/set packets.
 */
#define BUFMAX		400
/*
 * The maximum message length is BUFMAX minus the space needed for the hash,
 * which is 3 characters (#XX).
 */
#define MAX_MESSAGE_LEN	BUFMAX - 3

/*
 * Debug stack stuff.
 */
#define NUM_STACK_PAGES	3
#define STACK_SIZE	(NUM_STACK_PAGES * PAGE_SIZE)

/*
 * The maximum number of simultaneous mappings the user can create.
 */
#define MAX_KSEG_MAPPINGS	4

/*
 * Some useful asm define's.
 */
#define BREAKPOINT()	asm("int $3");

/*
 * We need to check if GDB is sending us a breakpoint instruction.
 */
#define BP_INSTRUCTION		0xcc

#define THREAD_NAME_LENGTH	WORLD_NAME_LENGTH

/*
 * Define _start.  We'll need this when deciding if we're doing a function
 * evaluation or a normal continue.
 */
extern void* _start;

/*
 * Return from exception actions
 */
typedef enum { step, cont, detach } rfeAction;

static const char hexchars[]="0123456789abcdef";

static Bool	debugEverInDebugger;
       Bool	debugInCall;
       Bool	debugInDebugger;

/*
 * Log-level for the debugger.
 */
unsigned debugLogLevel = 0;

/*
 * Automatically trapping into the debugger can be disabled for
 * UserWorlds.
 */
static Bool	userWorldDebugEnabled;

/*
 * Boolean flag. != 0 means we've been initialized.
 */
static char	initialized;
static Bool	serialDebugging;

/*
 * Put the error code here just in case the user cares.
 */
int gdb_i386errcode;

DebugRegisterFile defaultRegisters;
DebugRegisterFile backupRegisters;

/*
 * This should match the order of the registers defined in DebugRegisterFile.
 */
static const char* regstrings[] = { "EAX", "ECX", "EDX", "EBX", "ESP", "EBP",
				    "ESI", "EDI", "EIP", "EFLAGS", "CS", "SS",
				    "DS", "ES", "FS", "GS" };

static int defaultStack[STACK_SIZE/sizeof(int)];
static int backupStack[STACK_SIZE/sizeof(int)];
int *defaultStackPtr;
int *backupStackPtr;

/*
 * Abstract world.  Abstract worlds are used to represent threads that aren't
 * real worlds.  This way, they'll look just like all the other worlds when
 * typing 'info threads' in GDB.  We only keep enough info here for what GDB
 * wants; namely, a set of registers and a name.
 */
typedef struct AbstractWorld_Handle {
   struct AbstractWorld_Handle *next;

   char worldName[THREAD_NAME_LENGTH];
   DebugRegisterFile regs;
} AbstractWorld_Handle;

static AbstractWorld_Handle *abstractWorlds;

/*
 * Pre-allocate an abstract world for the debugger (in case we ASSERT fail in
 * it), plus an extra world for either the cos (in case we crash in it) or to
 * hold the register data if we crash before the PRDA is initialized (in which
 * case there are no real worlds yet).
 */
static AbstractWorld_Handle debuggerWorld;
static AbstractWorld_Handle extraWorld;
static Bool cosPanic;

typedef enum {
   THREADTYPE_UNUSED,
   THREADTYPE_REALWORLD,
   THREADTYPE_ABSTRACTWORLD,
   THREADTYPE_PLACEHOLDER
} ThreadType;

typedef int Thread_ID;

typedef union {
   /*
    * Used for THREADTYPE_REALWORLD type.
    */
   World_Handle *realWorld;
   /*
    * Used for THREADTYPE_ABSTRACTWORLD type.
    */
   AbstractWorld_Handle  *abstractWorld;
   /*
    * Used by world-type agnostic functions.
    */
   void *val;
} WorldData;

/*
 * Maps gdb's thread id to a real or abstract world handle.
 */
typedef struct Thread_Handle {
   ThreadType type;
   WorldData data;
} Thread_Handle;

static Thread_Handle threadMap[MAX_WORLDS+1];
static int numThreads;

/*
 * Holds the world handle of the world that was executing when the debugger
 * broke in.  This world's state will need to be reinstated before continuing.
 */
WorldData worldInDebugger;
static ThreadType worldInDebuggerType;

/*
 * Holds the world handle of the worldInDebugger from the last time we entered
 * the debugger.
 */
static WorldData lastWorldInDebugger;

/*
 * Holds the gdb thread id of the world that broke into the debugger.
 */
static Thread_ID initialGDBThread;
/*
 * Holds the gdb thread id that is the target for all Continue/Step operations.
 */
static Thread_ID csTarget;
/*
 * Holds the gdb thread id that is the target for all other operations.
 */
static Thread_ID otherTarget;

/*
 * The kernel debugger's connection context.
 */
static Debug_Context kernCtx;

/*
 * These are variables that can be set by the user of the debugger to allow for
 * easy remote resets and vmkernel unloads.
 */
int wantReset = 0;
int unloadVMK = 0;

/*
 * Host_GetCharDebug can only access cos addresses from within
 * the hostworld (ie the PSOD happened in the hostworld, and
 * the debugger is running in the context of the hostworld).  
 */
static int (*cosGetCharFn)(void*) = Host_GetCharDebug;
/*
 * The input and output buffers used to communicate with gdb.  Only
 * DebugHandleException should directly use these.  All other functions are
 * passed pointers to them by DebugHandleException.
 *
 * These variables were moved here from DebugHandleException primarily to reduce
 * the stack size for that function (800 bytes here!).
 */
static char inputBuffer[BUFMAX];
static char outputBuffer[BUFMAX];

/*
 * Holds the information for mappings the user has made from gdb.
 */
static int numMappings;
static KSEG_Pair* ksegMappings[MAX_KSEG_MAPPINGS];

/*
 * Save whether the current world was preemptible or not when it entered the
 * debugger.  Because we may call some functions that ASSERT that the current
 * world is not preemptible, we have to disable it for the duration of the
 * debugging session, then restore it to its previous state as we exit.
 */
static Bool preemptible;

/*
 * Set to TRUE when we cleanly exit from the debugger.  We cleanly exit when we
 * iret back to normal kernel code through DebugReturnToProg.  We don't cleanly
 * exit when we fail an DEBUG_ASSERT, hit a DEBUG_PANIC, or SEGV or otherwise
 * fault.
 */
Bool cleanExit = TRUE;

/*
 * Address of a routine to return to if we get a memory fault.
 */
void (*volatile mem_fault_routine)(void) = NULL;

/*
 * Indicate to caller of mem2hex or hex2mem that there has been an error.
 */
static volatile int mem_err;


static void DebugReturnFromException (rfeAction action, int addr);

/*
 * The DebugThread* functions deal with converting a thread id to either a real
 * world id or an abstract world handle and performing some operation on that
 * object.  They are the only functions in this file that use any World_*
 * functions.
 */
static void DebugThreadGetRegisters(Thread_ID, DebugRegisterFile*);
static void DebugThreadSaveRegisters(Thread_ID, DebugRegisterFile*);
static Bool DebugThreadAdjustAddrForVMMStack(Thread_ID, int*);
static char* DebugThreadGetName(Thread_ID);
static World_ID DebugThreadGetWorldID(Thread_ID);
static void DebugThreadAddAbstractWorld(AbstractWorld_Handle *world, char *name,
					DebugRegisterFile *regs);
static void DebugThreadCreateMappings(void);
static Thread_ID DebugThreadFindWorld(ThreadType type, void *val);
static void DebugThreadInitThreadState(Bool wasCosPanic);
static void DebugThreadUpdateThreadState(void);
static void DebugThreadDumpMappings(int logLevel);
static void DebugThreadInit(void);

static void DebugReadMemory (char*, char*);
static void DebugWriteMemory (char*, char*);
static void DebugReadRegisters (char*, char*);
static void DebugWriteRegisters (char*, char*);
static void DebugGetThreadInfo (char*, char*, Bool);
static void DebugGetExtraThreadInfo (char*, char*);
static void DebugSetThread (char*, char*);
static void DebugThreadAlive (char*, char*);
static void DebugCurrentThread (char*, char*);
static void DebugStepContDetach (char*, char*);
static void DebugReasonForHalt (int, char*);
static void DebugMapMA (char*, char*);
static void DebugUnmapMAs (char*);
static void DebugPrintCnxInfo (Bool);
//void DebugHandleException (int);
void Debug_Break(void);

int hex(char ch);
int hexToInt(char **ptr, int *intValue);
int hexTo64bitInt(char **ptr, uint64 *intValue);
static VMK_ReturnStatus DebugGetPacket(char *buffer);
static VMK_ReturnStatus DebugPutPacket(const char *buffer);
static VMK_ReturnStatus DebugPutPacketAsync(const char *buffer);
char* mem2hex(char *mem, char *buf, int count, int may_fault);
char* hex2mem(char *buf, char *mem, int count, int may_fault);
int computeSignal (int exceptionVector);

static VMK_ReturnStatus DebugSerialStart(Debug_Context* dbgCtx);
static VMK_ReturnStatus DebugSerialListeningOn(Debug_Context* dbgCtx,
					       char* desc, int length);
static VMK_ReturnStatus DebugSerialGetChar(Debug_Context* dbgCtx,
					   unsigned char* ch);
static VMK_ReturnStatus DebugSerialPutChar(Debug_Context* dbgCtx,
					   unsigned char ch);
static VMK_ReturnStatus DebugSerialFlush(Debug_Context* dbgCtx);
static VMK_ReturnStatus DebugSerialStop(Debug_Context* dbgCtx);
static VMK_ReturnStatus DebugSerialPollChar(Debug_Context* dbgCtx,
					unsigned char* ch);
static VMK_ReturnStatus DebugSerialCleanup(Debug_Context* dbgCtx);

static Debug_CnxFunctions DebugSerialCnxFunctions = {
   DebugSerialStart,
   DebugSerialListeningOn,
   DebugSerialGetChar,
   DebugSerialPutChar,
   DebugSerialFlush,
   DebugSerialStop,
   DebugSerialPollChar,
   DebugSerialCleanup
};

void Debug_PutString(char *s);

void waitabit(void);


/*
 *----------------------------------------------------------------------
 *
 * DebugReturnFromException --
 *
 *	Completes the process of continuing/single-stepping.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Begins execution of code specified in defaultRegisters.eip.
 *
 *----------------------------------------------------------------------
 */

static void
DebugReturnFromException (rfeAction action, int addr)
{
   int n;

   if (wantReset) {
      /*
       * Bye bye.
       */
      OUTB(KEYBOARD_CMD_PORT, KEYBOARD_CMD_RESET);
   } else if (unloadVMK) {
      /*
       * XXX: This doesn't seem to unload the vmkernel, it just locks my 
       * machine.
       */
      Host_Broken();
   } else if (!debugInCall) {
      DEBUG_LOG(2, "done with exception\n");

      /*
       * We need to restore the state of the registers to their state before
       * we entered the debugger.  Thus we need to swap back to the initial
       * thread that we entered in.
       */ 
      DebugThreadSaveRegisters(otherTarget, &defaultRegisters);
      DebugThreadGetRegisters(initialGDBThread, &defaultRegisters);
   }

   /*
    * We shouldn't be trying to run with an abstract world as the debugger world
    */
   DEBUG_ASSERT(worldInDebuggerType == THREADTYPE_REALWORLD);

   lastWorldInDebugger = worldInDebugger;

   /*
    * Clear the trace bit.
    */
   defaultRegisters.eflags &= ~EFLAGS_TF;

   /*
    * Now set the trace bit if we're stepping.
    */
   if (action == step) {
      defaultRegisters.eflags |= EFLAGS_TF;
   }

   /*
    * Change the PC to reflect the address we want to resume at.
    */
   if (0 != addr) {
      defaultRegisters.eip = addr;
   }

   for (n = 0; n < sizeof defaultRegisters / sizeof(uint32); n++) {
      DEBUG_LOG(2, "%s: 0x%x\n", regstrings[n], ((uint32*)&defaultRegisters)[n]);
   }

   Debug_CnxStop(&kernCtx);

   Kseg_DebugMapRestore();

   MemRO_ChangeProtection(MEMRO_READONLY); 

   NMI_Enable();

   CpuSched_RestorePreemption(preemptible);

   DebugReturnToProg();

   NOT_REACHED();
}
 

void
set_mem_err(void)
{
   mem_err = 1;
}


/*
 * Convert the memory pointed to by mem into hex, placing result in buf.
 * Return a pointer to the last char put in buf (null).
 *
 * If may_fault is non-zero, then we should set mem_err in response to a fault;
 * if zero treat a fault like any other fault in the stub.
 */
char* mem2hex(char *mem, char *buf, int count, int may_fault)
{
   int i;
   unsigned char ch;

   if (may_fault)
      mem_fault_routine = set_mem_err;

   for(i = 0; i < count; i++) {
      if ((uint32)mem >= VMNIX_KVA_START && (uint32)mem < VMNIX_KVA_END) {
         /*
          * Looks like a COS kernel address.   On a vmkernel psod we'll 
          * only be able to access cos addresses if we died in the host world
          * and the debugger is running in the context of the host world.
          *
          * After an oops / panic in the cos a different accessor function
          * is used and we should be able to access cos addresses from any
          * cpu.
          */
          ch = cosGetCharFn(mem++);
      } else {
         ch = DebugGetChar(mem++);
      }

      if(may_fault && mem_err) {
	 return buf;
      }

      *buf++ = hexchars[ch >> 4];
      *buf++ = hexchars[ch % 16];
   }

   *buf = 0;

   if (may_fault)
      mem_fault_routine = NULL;

   return buf;
}

/*
 * Convert the hex array pointed to by buf into binary to be placed in mem.
 * Return a pointer to the character AFTER the last byte written.
 */
char* hex2mem(char *buf, char *mem, int count, int may_fault)
{
   int i;
   unsigned char ch;

   if(may_fault) {
      mem_fault_routine = set_mem_err;
   }

   for(i = 0; i < count; i++) {
      ch = hex(*buf++) << 4;
      ch = ch + hex(*buf++);

      DebugSetChar(mem++, ch);

      if(may_fault && mem_err) {
	 return mem;
      }
   }

   if(may_fault)
      mem_fault_routine = NULL;

   return mem;
}




/*
 * A few random helper functions.
 */

int hex(char ch)
{
   if ((ch >= 'a') && (ch <= 'f')) return (ch-'a'+10);
   if ((ch >= '0') && (ch <= '9')) return (ch-'0');
   if ((ch >= 'A') && (ch <= 'F')) return (ch-'A'+10);

   return (-1);
}

/*
 * Converts a hex character string to an integer value.  It increments *ptr to
 * the character after the last char of hex converted.
 */
int hexToInt(char **ptr, int *intValue)
{
    int numChars = 0;
    int hexValue;

    *intValue = 0;

    while(**ptr)
    {
        hexValue = hex(**ptr);
        if (hexValue >= 0)
        {
            *intValue = (*intValue << 4) | hexValue;
            numChars ++;
        }
        else
            break;

        (*ptr)++;
    }

    return (numChars);
}

/*
 * Converts a hex character string to a 64-bit integer value.  It increments
 * *ptr to the character after the last char of hex converted.
 */
int hexTo64bitInt(char **ptr, uint64 *intValue)
{
    int numChars = 0;
    int hexValue;

    *intValue = 0;

    while(**ptr)
    {
        hexValue = hex(**ptr);
        if (hexValue >= 0)
        {
            *intValue = (*intValue << 4) | hexValue;
            numChars ++;
        }
        else
            break;

        (*ptr)++;
    }

    return (numChars);
}


/*
 *----------------------------------------------------------------------
 *
 * DebugIntToHexString --
 *
 *	Converts the given integer into a hex string representation.
 *
 *	Note that the string returned is always an even number of
 *	integers, because gdb expects as such, for whatever reason.  So,
 *	in the event that the number has an odd number of digits, we
 *	just pad it with a zero.
 *
 * Results:
 *	A pointer into the given buffer where the string is stored.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

char*
DebugIntToHexString(int i, char *buffer, int length)
{
   int ret;

   ret = snprintf(buffer + 1, length - 1, "%x", i);
   DEBUG_ASSERT(ret < length - 1);
   if (ret % 2 != 0) {
      buffer[0] = '0';
      return buffer;
   } else {
      return buffer + 1;
   }
}


/*
 * Takes the 386 exception vector and attempts to translate this number into a
 * unix compatible signal value.
 */
int computeSignal (int exceptionVector)
{
   int sigval;
   switch (exceptionVector) {
      case 0 : sigval = 8; break;	/* divide by zero */
      case 1 : sigval = 5; break;	/* debug exception */
      case 3 : sigval = 5; break;	/* breakpoint */
      case 4 : sigval = 16; break;	/* into instruction (overflow) */
      case 5 : sigval = 16; break;	/* bound instruction */
      case 6 : sigval = 4; break;	/* Invalid opcode */
      case 7 : sigval = 8; break;	/* coprocessor not available */
      case 8 : sigval = 7; break;	/* double fault */
      case 9 : sigval = 11; break;	/* coprocessor segment overrun */
      case 10 : sigval = 11; break;	/* Invalid TSS */
      case 11 : sigval = 11; break;	/* Segment not present */
      case 12 : sigval = 11; break;	/* stack exception */
      case 13 : sigval = 11; break;	/* general protection */
      case 14 : sigval = 11; break;	/* page fault */
      case 16 : sigval = 7; break;	/* coprocessor error */
      default:
	 sigval = 7;			/* "software generated" */
   }
   return (sigval);
}




/*
 * The following functions deal with remote user input.  They process and
 * execute commands according to the GDB remote debugging protocol, specified in
 * the GDB Internals document, distributed with gdb.
 */

/*
 *----------------------------------------------------------------------
 *
 * DebugThreadGetRegisters --
 *
 *	Copies the registers from the given real or abstract world to an
 *      array that can be used by the debugger.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
DebugThreadGetRegisters(Thread_ID threadId,		// IN
		        DebugRegisterFile *regs)	// OUT
{
   Thread_Handle *thread;

   DEBUG_ASSERT_IS_VALID_THREAD(threadId);
   DEBUG_ASSERT(regs != NULL);

   thread = &threadMap[threadId];
   switch(thread->type) {
      case THREADTYPE_REALWORLD: {
         World_Handle *world = thread->data.realWorld;
	 DEBUG_ASSERT(world != NULL);
	 regs->eax = world->savedState.regs[REG_EAX];
	 regs->ecx = world->savedState.regs[REG_ECX];
	 regs->edx = world->savedState.regs[REG_EDX];
	 regs->ebx = world->savedState.regs[REG_EBX];
	 regs->esp = world->savedState.regs[REG_ESP];
	 regs->ebp = world->savedState.regs[REG_EBP];
	 regs->esi = world->savedState.regs[REG_ESI];
	 regs->edi = world->savedState.regs[REG_EDI];
	 regs->eip = world->savedState.eip;
	 regs->eflags = world->savedState.eflags;
	 regs->cs = (uint32)world->savedState.segRegs[SEG_CS];
	 regs->ss = (uint32)world->savedState.segRegs[SEG_SS];
	 regs->ds = (uint32)world->savedState.segRegs[SEG_DS];
	 regs->es = (uint32)world->savedState.segRegs[SEG_ES];
	 regs->fs = (uint32)world->savedState.segRegs[SEG_FS];
	 regs->gs = (uint32)world->savedState.segRegs[SEG_GS];
	 break;
      }
      case THREADTYPE_ABSTRACTWORLD: {
         AbstractWorld_Handle *world = thread->data.abstractWorld;
	 DEBUG_ASSERT(world != NULL);
	 memcpy(regs, &world->regs, sizeof *regs);
	 break;
      }
      case THREADTYPE_PLACEHOLDER:
         memset(regs, 0, sizeof *regs);
	 break;
      default:
         DEBUG_PANIC("DebugThreadGetRegisters: Invalid thread type (%d) for "
		     "thread %d.", thread->type, threadId);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * DebugThreadSaveRegisters --
 *
 *      Copies the registers from the debugger into the given real or
 *	abstract world.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
DebugThreadSaveRegisters(Thread_ID threadId,		// IN
			 DebugRegisterFile *regs)	// IN
{
   Thread_Handle *thread;

   DEBUG_ASSERT_IS_VALID_THREAD(threadId);
   DEBUG_ASSERT(regs != NULL);

   thread = &threadMap[threadId];
   switch(thread->type) {
      case THREADTYPE_REALWORLD: {
         World_Handle *world = thread->data.realWorld;
	 DEBUG_ASSERT(world != NULL);
	 world->savedState.regs[REG_EAX]   = regs->eax;
	 world->savedState.regs[REG_ECX]   = regs->ecx;
	 world->savedState.regs[REG_EDX]   = regs->edx;
	 world->savedState.regs[REG_EBX]   = regs->ebx;
	 world->savedState.regs[REG_ESP]   = regs->esp;
	 world->savedState.regs[REG_EBP]   = regs->ebp;
	 world->savedState.regs[REG_ESI]   = regs->esi;
	 world->savedState.regs[REG_EDI]   = regs->edi;
	 world->savedState.eip	           = regs->eip;
	 world->savedState.eflags	   = regs->eflags;
	 world->savedState.segRegs[SEG_CS] = (Selector)regs->cs;
	 world->savedState.segRegs[SEG_SS] = (Selector)regs->ss;
	 world->savedState.segRegs[SEG_DS] = (Selector)regs->ds;
	 world->savedState.segRegs[SEG_ES] = (Selector)regs->es;
	 world->savedState.segRegs[SEG_FS] = (Selector)regs->fs;
	 world->savedState.segRegs[SEG_GS] = (Selector)regs->gs;
	 break;
      }
      case THREADTYPE_ABSTRACTWORLD: {
         AbstractWorld_Handle *world = thread->data.abstractWorld;
	 DEBUG_ASSERT(world != NULL);
	 memcpy(&world->regs, regs, sizeof world->regs);
	 break;
      }
      case THREADTYPE_PLACEHOLDER:
         break;
      default:
         DEBUG_PANIC("DebugThreadSaveRegisters: Invalid thread type (%d) for "
		     "thread %d.", thread->type, threadId);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * DebugThreadAdjustAddrForVMMStack --
 *
 *	If this thread is a real world and addr points into the world's
 *	vmm stack, we redirect the addr to access the mappedStack.
 *
 *	We have to do this because all worlds use the same address for
 *	their VMM stacks and we're not bothering to switch page tables as
 *	we examine different worlds.
 *
 * Results:
 *	TRUE if we were successful or if a redirect wasn't necessary,
 *	FALSE if we tried to redirect, but there is no mappedStack.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Bool
DebugThreadAdjustAddrForVMMStack(Thread_ID threadId, int *addr)
{
   Thread_Handle *thread;

   DEBUG_ASSERT_IS_VALID_THREAD(otherTarget);
   DEBUG_ASSERT(addr != NULL);

   thread = &threadMap[threadId];
   switch (thread->type) {
      case THREADTYPE_REALWORLD: {
	 World_Handle *world = thread->data.realWorld;
	 DEBUG_ASSERT(world != NULL);

	 if (World_IsVMMWorld(world)) {
	    World_VmmInfo *vmmInfo = World_VMM(world);
	    int i;

	    for (i = 0; i < WORLD_VMM_NUM_STACKS; i++) {
	       if (vmmInfo->vmmStackInfo[i].stackBase <= *addr &&
		   *addr < vmmInfo->vmmStackInfo[i].stackTop) {
		  if (vmmInfo->vmmStackInfo[i].mappedStack) {
		     *addr = *addr - vmmInfo->vmmStackInfo[i].stackBase +
			     (uintptr_t)vmmInfo->vmmStackInfo[i].mappedStack;
		  } else {
		     return FALSE;
		  }
	       }
	    }
	 }

         return TRUE;
      }
      case THREADTYPE_ABSTRACTWORLD:
         return TRUE;
      case THREADTYPE_PLACEHOLDER:
         return TRUE;
      default:
         DEBUG_PANIC("DebugThreadAdjustAddrForVMMStack: Invalid thread type "
		     "(%d) for thread %d.", thread->type, threadId);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * DebugThreadGetName --
 *
 *	Returns the name of the real or abstract world associated with
 *	the given thread.
 *
 * Results:
 *	As above.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static char*
DebugThreadGetName(Thread_ID threadId)
{
   Thread_Handle *thread;

   DEBUG_ASSERT_IS_VALID_THREAD(otherTarget);

   thread = &threadMap[threadId];
   switch (thread->type) {
      case THREADTYPE_REALWORLD: {
	 World_Handle *world = thread->data.realWorld;
	 DEBUG_ASSERT(world != NULL);
         return world->worldName;
      }
      case THREADTYPE_ABSTRACTWORLD: {
         AbstractWorld_Handle *world = thread->data.abstractWorld;
	 DEBUG_ASSERT(world != NULL);
	 return world->worldName;
      }
      case THREADTYPE_PLACEHOLDER:
         return "Placeholder world";
      default:
         DEBUG_PANIC("DebugThreadGetName: Invalid thread type (%d) for thread "
		     "%d.", thread->type, threadId);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * DebugThreadGetWorldID --
 *
 *	Returns the a valid world id if the given thread corresponds to
 *	a real world or INVALID_WORLD_ID for an abstract world.
 *
 * Results:
 *	As above.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static World_ID
DebugThreadGetWorldID(Thread_ID threadId)
{
   Thread_Handle *thread;

   DEBUG_ASSERT_IS_VALID_THREAD(otherTarget);

   thread = &threadMap[threadId];
   switch (thread->type) {
      case THREADTYPE_REALWORLD: {
	 World_Handle *world = thread->data.realWorld;
	 DEBUG_ASSERT(world != NULL);
         return world->worldID;
      }
      case THREADTYPE_ABSTRACTWORLD:
      case THREADTYPE_PLACEHOLDER:
         return INVALID_WORLD_ID;
      default:
         DEBUG_PANIC("DebugThreadGetWorldID: Invalid thread type (%d) for "
		     "thread %d.", thread->type, threadId);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * DebugThreadAddAbstractWorld --
 *
 *	Fills the given abstract world struct with the given data,
 *	and adds the new abstract world to the thread map.
 *
 * Results:
 *	The Thread_ID that was allocated for this abstract world.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
DebugThreadAddAbstractWorld(AbstractWorld_Handle *world, char *name,
			    DebugRegisterFile *regs)
{
   DEBUG_ASSERT(world != NULL);

   /*
    * First set up the world struct.
    */
   snprintf(world->worldName, sizeof world->worldName, "%s", name);
   memcpy(&world->regs, regs, sizeof world->regs);

   world->next = abstractWorlds;
   abstractWorlds = world;
}


/*
 *----------------------------------------------------------------------
 *
 * DebugThreadSwapThreadMapping --
 *
 *	Exchange the data for two thread entries, effectively swapping
 *	which worlds each thread maps to.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
DebugThreadSwapThreadMapping(Thread_ID thread1, Thread_ID thread2)
{
   ThreadType tmpType = threadMap[thread1].type;
   WorldData tmpData = { .val = threadMap[thread1].data.val };

   threadMap[thread1].type = threadMap[thread2].type;
   threadMap[thread1].data.val = threadMap[thread2].data.val;

   threadMap[thread2].type = tmpType;
   threadMap[thread2].data.val = tmpData.val;

}


/*
 *----------------------------------------------------------------------
 *
 * DebugThreadCreateMappings --
 *
 *	Creates the mapping of gdb threads to real and abstract worlds.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
DebugThreadCreateMappings(void)
{
   static World_ID worldList[MAX_WORLDS];
   AbstractWorld_Handle *cur;
   int n;

   /*
    * Clear out threadMap.
    */
   memset(threadMap, 0, sizeof threadMap);

   /*
    * Add in the real worlds.
    */
   numThreads = MAX_WORLDS;
   memset(worldList, 0, sizeof(worldList));
   World_AllWorldsDebug(&worldList[1], &numThreads);
   for (n = 1; n < numThreads + 1; n++) {
      threadMap[n].type = THREADTYPE_REALWORLD;
      threadMap[n].data.realWorld = World_FindDebug(worldList[n]);
   }

   /*
    * Add in the abstract worlds.
    */
   for (cur = abstractWorlds; cur != NULL; cur = cur->next) {
      numThreads++;
      threadMap[numThreads].type = THREADTYPE_ABSTRACTWORLD;
      threadMap[numThreads].data.abstractWorld = cur;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * DebugThreadFindWorld --
 *
 *	Finds the thread associated with the given world.
 *
 * Results:
 *	The Thread_ID of the thread, or DEBUG_INVALID_THREAD if not found.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static Thread_ID
DebugThreadFindWorld(ThreadType type, void *val)
{
   int n;

   for (n = 1; n < numThreads + 1; n++) {
      if (threadMap[n].type == type && threadMap[n].data.val == val) {
         return (Thread_ID)n;
      }
   }

   return DEBUG_INVALID_THREAD;
}


/*
 *----------------------------------------------------------------------
 *
 * DebugThreadInitThreadState--
 *
 *	Creates the inital mapping of gdb threads to world ids.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
DebugThreadInitThreadState(Bool wasCosPanic)
{
   /*
    * When we first enter the debugger, the debugging world should always be
    * a real world, unless the PRDA has not been initialized, or if we took a
    * COS panic.
    */
   DEBUG_ASSERT(PRDA_GetRunningWorldSafe() == NULL || wasCosPanic ||
		worldInDebuggerType == THREADTYPE_REALWORLD);

   /*
    * Reset our variables.
    */
   initialGDBThread = DEBUG_INVALID_THREAD;
   otherTarget = DEBUG_INVALID_THREAD;
   csTarget = DEBUG_INVALID_THREAD;

   /*
    * Create the thread -> world mapping.
    */
   DebugThreadCreateMappings();
}


/*
 *----------------------------------------------------------------------
 *
 * DebugThreadUpdateThreadState --
 *
 *	Recreates the thread -> world mappings, accounting for some of
 *	GDB's idiosyncrasies.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
DebugThreadUpdateThreadState(void)
{
   int n;
   int origNumThreads = numThreads;
   Thread_ID threadId;

   /*
    * Re-generate the thread -> world mappings.
    */
   DebugThreadCreateMappings();

   /*
    * We can't ever let the number of threads drop.  It must either stay
    * constant or increase.  The reason for this has to do with the fact that
    * gdb caches its notion of the thread state.  So if there are now less real
    * and abstract worlds than before, add in placeholder worlds.
    */
   if (numThreads < origNumThreads &&
       (otherTarget > numThreads || csTarget > numThreads)) {
      for (n = numThreads + 1; n < MAX(otherTarget, csTarget); n++) {
         threadMap[n].type = THREADTYPE_PLACEHOLDER;
	 threadMap[n].data.val = NULL;
      }

      numThreads = MAX(otherTarget, csTarget);
   }

   DEBUG_ASSERT_IS_VALID_THREAD(otherTarget);
   /*
    * Since we recreated our thread -> world mapping, we need to find which
    * thread id worldInDebugger is at and set initialGDBThread as such.
    */
   threadId = DebugThreadFindWorld(worldInDebuggerType, worldInDebugger.val);
   if (DEBUG_IS_VALID_THREAD(threadId)) {
      DebugThreadSwapThreadMapping(threadId, otherTarget);
   } else {
      /*
       * Dump thread mappings.
       */
      DebugThreadDumpMappings(0);
      DEBUG_ASSERT_IS_VALID_THREAD(threadId);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * DebugThreadDumpMappings --
 *
 *	Print out the thread id -> world id mappings.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
DebugThreadDumpMappings(int logLevel)
{
   int i;

   DEBUG_LOG(logLevel, "Dumping thread mappings:\n");

   for (i = 1; i < numThreads + 1; i++) {
      switch (threadMap[i].type) {
         case THREADTYPE_REALWORLD:
	    DEBUG_LOG(logLevel, "thread %d -> real world %p\n", i,
		      threadMap[i].data.val);
	    break;
         case THREADTYPE_ABSTRACTWORLD:
	    DEBUG_LOG(logLevel, "thread %d -> abstract world %p\n", i,
		      threadMap[i].data.val);
	    break;
	 case THREADTYPE_PLACEHOLDER:
	    DEBUG_LOG(logLevel, "thread %d -> placeholder\n", i);
	    break;
	 default:
	    DEBUG_LOG(logLevel, "thread %d -> unknown world %p\n", i,
		      threadMap[i].data.val);
	    break;
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * DebugThreadInit --
 *
 *	Initialize thread state.  This is called when entering the
 *	debugger.  It recreates the thread -> world mappings, adding
 *	in new abstract worlds for the debugger and cos as necessary.
 *	It also sets worldInDebugger.
 *
 *	Try and reduce the amount of DEBUG_ASSERTs in this function and
 *	the functions it calls.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static void
DebugThreadInit(void)
{
   Bool wasCosPanic = FALSE;

   if (!cleanExit) {
      DEBUG_LOG(0, "Debugger fault detected.  Using backup stack!\n");

      if (!debugEverInDebugger) {
         DEBUG_LOG(0, "Odd, debugEverInDebugger is FALSE.\n");
	 debugEverInDebugger = TRUE;
      }

      /*
       * Since we failed to cleanly exit the debugger last time, it means
       * there's a problem with the debugger.  So, we want to be able to debug
       * the debugger.  Thus, the first thing we do is create an abstract world
       * to hold the state of the debugger at the time it crashed.  Note that
       * the state of the debugger is located in the backupRegisters, while the
       * state of the world being debugged at the time is still in
       * defaultRegisters.
       */
      DebugThreadAddAbstractWorld(&debuggerWorld, "DebugWorld",
				  &backupRegisters);

      /*
       * If otherTarget is valid, then we want to save defaultRegisters back to
       * the world.
       *
       * If, however, we didn't get far enough along to initialize otherTarget,
       * then we just overwrite the registers for the world we were debugging.
       */
      if (DEBUG_IS_VALID_THREAD(otherTarget)) {
	 /*
	  * Before we do anything else, we should save the state of the world we
	  * were inspecting at the time of the crash.
	  */
	 DebugThreadSaveRegisters(otherTarget, &defaultRegisters);
      }

      /*
       * Finally, to complete the transition to debugging the abstract world
       * rather than the one we were inspecting, we have to copy the abstract
       * world's registers in backupRegisters to defaultRegisters.
       */
      memcpy(&defaultRegisters, &backupRegisters, sizeof defaultRegisters);

      worldInDebugger.abstractWorld = &debuggerWorld;
      worldInDebuggerType = THREADTYPE_ABSTRACTWORLD;
   } else if (cosPanic) {
      if (DEBUG_IS_VALID_THREAD(otherTarget)) {
	 DebugThreadSaveRegisters(otherTarget, &defaultRegisters);
      }

      memcpy(&defaultRegisters, &extraWorld.regs, sizeof defaultRegisters);

      wasCosPanic = TRUE;
      cosPanic = FALSE;

      worldInDebugger.abstractWorld = &extraWorld;
      worldInDebuggerType = THREADTYPE_ABSTRACTWORLD;
   } else if (PRDA_GetRunningWorldSafe() == NULL) {
      DEBUG_LOG(0, "Entered debugger before PRDA or MY_RUNNING_WORLD was "
		   "initialized.  Faking a world.\n");
      
      DebugThreadAddAbstractWorld(&extraWorld, "InitialWorld",
				  &defaultRegisters);

      worldInDebugger.abstractWorld = &extraWorld;
      worldInDebuggerType = THREADTYPE_ABSTRACTWORLD;
   } else {
      worldInDebugger.realWorld = PRDA_GetRunningWorldSafe();
      worldInDebuggerType = THREADTYPE_REALWORLD;
   }

   /*
    * When we enter the debugger for the very first time, we want to initialize
    * and create the thread id -> world mapping.  For all subsequent entries
    * into the debugger, we simply update the thread mapping, adding in new
    * worlds and removing dead ones.
    */
   if (!debugEverInDebugger) {
      DebugThreadInitThreadState(wasCosPanic);
   } else {
      DebugThreadUpdateThreadState();
   }

   /*
    * Find the initial thread id.
    */
   initialGDBThread = DebugThreadFindWorld(worldInDebuggerType,
					   worldInDebugger.val);
   DEBUG_ASSERT_IS_VALID_THREAD(initialGDBThread);
   DEBUG_ASSERT(otherTarget == DEBUG_INVALID_THREAD ||
		otherTarget == initialGDBThread);

   DebugThreadDumpMappings(2);
}


/*
 *----------------------------------------------------------------------
 *
 * Debug_AddCOSPanicBacktrace --
 *
 *	Add the COS as an abstract world.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

void
Debug_AddCOSPanicBacktrace(VMKFullExcFrame *fullFrame)
{
   DebugRegisterFile regs;

   regs.eax = fullFrame->regs.eax;
   regs.ecx = fullFrame->regs.ecx;
   regs.edx = fullFrame->regs.edx;
   regs.ebx = fullFrame->regs.ebx;
   regs.esp = fullFrame->frame.hostESP;
   regs.ebp = fullFrame->regs.ebp;
   regs.esi = fullFrame->regs.esi;
   regs.edi = fullFrame->regs.edi;
   regs.eip = fullFrame->frame.eip;
   regs.eflags = fullFrame->frame.eflags;
   regs.cs = fullFrame->frame.cs;
   // %ss isn't saved, so just use %ds here.
   regs.ss = fullFrame->regs.ds;
   regs.ds = fullFrame->regs.ds;
   regs.es = fullFrame->regs.es;
   regs.fs = fullFrame->regs.fs;
   regs.gs = fullFrame->regs.gs;

   DebugThreadAddAbstractWorld(&extraWorld, "COS vmnix", &regs);
   cosPanic = TRUE;
}



/*
 *----------------------------------------------------------------------
 *
 * DebugIsSettingUpFunctionEval --
 *
 *	Figures out whether GDB is trying to set up a function
 *	evaluation.  We want to figure this out so we'll know what to do
 *	when GDB issues the continue command.
 *
 *	When GDB wants to perform a function evaluation, it will muck with
 *	the registers and stack to emulate a function call, changing eip
 *	to point to the function to be evaluated.  The return address
 *	it pushes on is _start.  It also sets a breakpoint at _start so
 *	that control will return to the debugger when the function
 *	evaluation is done.  Normally when the continue command is issued,
 *	we switch back to initialGDBThread's registers before resuming
 *	execution.  However in this case, we don't want to do that, as
 *	the function would not be evaluated as it should be.
 *
 *	Moreover, when evaluating a function, we do not want to resume
 *	execution on any processor besides the one the debugger is
 *	running on.
 *
 *	Thus when we see that GDB is writing a one-byte value (0xcc) at
 *	_start, we know it's about to evaluate a function.  So we set
 *	debugInCall to TRUE so that we'll be able to handle the function
 *	evaluation correctly.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static INLINE Bool
DebugIsSettingUpFunctionEval(int value, int length, int addr)
{
   return ((unsigned)value == BP_INSTRUCTION && length == 1 &&
	   (void**)addr == &_start);
}


/*
 *----------------------------------------------------------------------
 *
 * DebugReadMemory --
 *
 *	Format: m<addr>,<length>
 *
 *      Reads 'length' bytes starting at address 'addr' into output
 *      buffer.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
DebugReadMemory(char* input, char* output)
{
   Bool		 fail   = FALSE;
   int		 addr;
   int		 length;

   DEBUG_ASSERT(input != NULL && output != NULL);
   DEBUG_ASSERT(input[0] == 'm');

   input++;

   if(hexToInt(&input, &addr) == 0) fail = TRUE;
   if(!fail && *(input++) != ',') fail = TRUE;
   if(!fail && hexToInt (&input, &length) == 0) fail = TRUE;

   if (fail) {
      DEBUG_LOG(1, "m - invalid input\n");
      strcpy (output, "E00");
      return;
   }

   mem_err = !DebugThreadAdjustAddrForVMMStack(otherTarget, &addr);
   if (!mem_err) {
      mem2hex((char*)addr, output, length, 1);
   }

   if (mem_err){
      DEBUG_LOG(1, "m - memory fault\n");
      strcpy (output, "E01");
   }
}


/*
 *----------------------------------------------------------------------
 *
 * DebugWriteMemory --
 *
 *	Format: M<addr>,<length>
 *
 *      Writes 'length' bytes from input buffer starting at address
 *      'addr'.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Modifies memory.
 *
 *----------------------------------------------------------------------
 */

static void
DebugWriteMemory(char* input, char* output)
{
   Bool		 fail   = FALSE;
   int		 addr;
   int		 length;
   int		 value;

   DEBUG_ASSERT(input != NULL && output != NULL);
   DEBUG_ASSERT(input[0] == 'M');

   input++;

   if (hexToInt (&input, &addr) == 0) fail = TRUE;
   if (!fail && *(input++) != ',') fail = TRUE;
   if (!fail && hexToInt (&input, &length) == 0) fail = TRUE;
   if (!fail && *(input++) != ':') fail = TRUE;

   if (fail) {
      DEBUG_LOG(1, "M - invalid input\n");
      strcpy (output, "E10");
      return;
   }

   mem_err = !DebugThreadAdjustAddrForVMMStack(otherTarget, &addr);
   if (!mem_err) {
      hex2mem(input, (char*)addr, length, 1);
   }

   /*
    * Read in the value being written (or up to the first 4 bytes of it anyway).
    */
   hexToInt(&input, &value);

   /*
    * Now see if GDB is setting up to evaluate a function.
    */
   if (DebugIsSettingUpFunctionEval(value, length, addr)) {
      DEBUG_LOG(2, "Setting debugInCall to TRUE.\n");
      debugInCall = TRUE;
   }

   if (mem_err) {
      DEBUG_LOG(1, "M - memory fault\n");
      strcpy (output, "E11");
   } else {
      strcpy (output, "OK");
   }
}


/*
 *----------------------------------------------------------------------
 *
 * DebugReadRegisters --
 *
 *	Format: g
 *
 *      Writes the value of the registers for the current world into the
 *	output buffer.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
DebugReadRegisters (char* input, char* output)
{
   DEBUG_ASSERT(input != NULL && output != NULL);
   DEBUG_ASSERT(input[0] == 'g');

   if (!DEBUG_IS_VALID_THREAD(otherTarget)) {
      strcpy (output, "E20");
      return;
   }

   mem_err = 0;

   mem2hex((char*)&defaultRegisters, output, sizeof defaultRegisters, 0);

   if (mem_err){
      DEBUG_LOG(1, "g - memory fault\n");
      strcpy (output, "E21");
   }
}


/*
 *----------------------------------------------------------------------
 *
 * DebugWriteRegisters --
 *
 *	Format: G<register data>
 *
 *	Writes the value of the registers given in the input buffer to
 *	the registers of the current world.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Modifies the current world's registers. 
 *
 *----------------------------------------------------------------------
 */

static void
DebugWriteRegisters(char* input, char* output)
{
   DEBUG_ASSERT(input != NULL && output != NULL);
   DEBUG_ASSERT(input[0] == 'G');

   if(!DEBUG_IS_VALID_THREAD(otherTarget)) {
      strcpy (output, "E30");
      return;
   }

   mem_err = 0;

   hex2mem(&input[1], (char*)&defaultRegisters, sizeof defaultRegisters, 0);

   if (mem_err){
      DEBUG_LOG(1, "G - memory fault\n");
      strcpy (output, "E31");
   } else {
      strcpy (output, "OK");
   }
}


/*
 *----------------------------------------------------------------------
 *
 * DebugGetThreadInfo --
 *
 *	Format: qfThreadInfo or qsThreadInfo
 *
 *	Returns a list of active worlds' world ids in the output buffer.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
DebugGetThreadInfo(char* input, char* output, Bool cont)
{
   static int curThread = 1;
   int charsLeft = MAX_MESSAGE_LEN;
   int ret;
   Bool first = TRUE;
   char buffer[10];
   char *ptr;

   DEBUG_ASSERT(input != NULL && output != NULL);
   DEBUG_ASSERT(strcmp(input, "qfThreadInfo") == 0 ||
		strcmp(input, "qsThreadInfo") == 0);

   if (!cont) {
      curThread = 1;
   } else if (curThread > numThreads) {
      /*
       * If we've written out all the threads, reply with only an 'l'.
       */
      *output = 'l';
      return;
   }

   /*
    * By definition we begin this message with a 'm'.
    */
   *output = 'm';
   output++;
   charsLeft--;

   /*
    * Now print out each thread id, preceding it with a comma if it isn't the
    * first one in the list.
    */
   for (; curThread < numThreads + 1; curThread++) {
      ptr = DebugIntToHexString(curThread, buffer, sizeof buffer);
      ret = snprintf(output, charsLeft, "%s%s", first ? "" : ",", ptr);
      if (ret < 0 || ret >= charsLeft) {
         *output = 0;
	 break;
      }
      output += ret;
      charsLeft -= ret;

      first = FALSE;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * DebugGetExtraThreadInfo --
 *
 *	Format: qThreadExtraInfo,<id>
 *
 *	Returns a printable string description for the given thread id.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
DebugGetExtraThreadInfo(char* input, char* output)
{
   int		 threadId;
   char		 threadName[THREAD_NAME_LENGTH+1];

   DEBUG_ASSERT(input != NULL && output != NULL);
   DEBUG_ASSERT(strncmp(input, "qThreadExtraInfo", 16) == 0);

   input += 17;
   if (!hexToInt(&input, &threadId)) {
      strcpy (output, "E50");
      return;
   }

   /*
    * If this thread is the thread that the debugger broke into, mark that for
    * the user.
    */
   if(threadId == initialGDBThread) {
      snprintf(threadName, sizeof threadName, "#%d %.20s",
      	       DebugThreadGetWorldID(threadId), DebugThreadGetName(threadId));
   } else {
      snprintf(threadName, sizeof threadName, "%d %.20s",
      	       DebugThreadGetWorldID(threadId), DebugThreadGetName(threadId));
   }

   mem_err = 0;

   mem2hex(threadName, output, strlen(threadName), 0);

   if (mem_err){
      DEBUG_LOG(1, "qThreadExtraInfo - memory fault\n");
      strcpy (output, "E51");
   }
}


/*
 *----------------------------------------------------------------------
 *
 * DebugSetThread --
 *
 *	Format: H<c><t>
 *
 *	'c' specifies which operations should be affected, either 'c'
 *	for step and continue or 'g' for all other operations.
 *	't' is the thread id.  If 't' is 0, pick any thread.  If 'c' is
 *	'c', then the thread id can be -1, which applies the operations
 *	to all threads.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      If 'c' is 'c', changes csTarget to the value of 't'.  Otherwise,
 *	changes otherTarget to value of 't'.
 *
 *----------------------------------------------------------------------
 */

static void
DebugSetThread(char* input, char* output)
{
   Thread_ID threadId;

   DEBUG_ASSERT(input != NULL && output != NULL);
   DEBUG_ASSERT(input[0] == 'H');

   if (input[1] != 'c' && input[1] != 'g') {
      strcpy (output, "E60");
      return;
   }

   if (input[2] == '-' && input[3] == '1') {
      if (input[1] == 'c') {
         threadId = -1;
      } else {
         strcpy(output, "E62");
         return;
      }
   } else {
      char* ptr = input + 2;

      if (!hexToInt (&ptr, &threadId)) {
	 strcpy (output, "E61");
	 return;
      }
   }

   /*
    * If they specify DEBUG_ANY_THREAD (0), we'll just use the initial thread.
    */
   if (threadId == DEBUG_ANY_THREAD) {
      DEBUG_ASSERT_IS_VALID_THREAD(initialGDBThread);
      threadId = initialGDBThread;
   }

   /*
    * The threadId must be a valid thread or DEBUG_ALL_THREADS (-1).
    */
   if (threadId != DEBUG_ALL_THREADS && !DEBUG_IS_VALID_THREAD(threadId)) {
      strcpy (output, "E62");
      return;
   }

   if (input[1] == 'c') {
      /*
       * threadId may be -1, which means all threads.
       */
      csTarget = threadId;

      DEBUG_ASSERT(csTarget != DEBUG_INVALID_THREAD);
   } else if (otherTarget != threadId) {
      if (!DEBUG_IS_VALID_THREAD(threadId)) {
         strcpy(output, "E63");
	 return;
      }

      /*
       * Whenever we change threads, we need to swap out the active registers.
       * We do this for several reasons, but the most important is that gdb
       * likes to scribble on the registers before it does such things as
       * evaluate functions and then reset the registers to their original value
       * afterwards.  Thus gdb expects the registers it writes to be the active
       * registers during the evaluation.  Because this protocol only deals with
       * primitive commands, we can't see the bigger picture of what gdb is
       * doing.  So we just swap the registers now so that gdb can do whatever
       * it wants and we don't have to care.
       */
      if (!DEBUG_IS_VALID_THREAD(otherTarget)) {
         DEBUG_ASSERT_IS_VALID_THREAD(initialGDBThread);
	 otherTarget = initialGDBThread;
      }

      DebugThreadSaveRegisters(otherTarget, &defaultRegisters);
      otherTarget = threadId;
      DebugThreadGetRegisters(otherTarget, &defaultRegisters);

      DEBUG_ASSERT_IS_VALID_THREAD(otherTarget);
   }

   DEBUG_ASSERT_IS_VALID_THREAD(initialGDBThread);

   strcpy (output, "OK");
}


/*
 *----------------------------------------------------------------------
 *
 * DebugThreadAlive --
 *
 *	Format: T<id>
 *
 *	Returns OK in the output buffer if the specified world exists
 *	and is active.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static void
DebugThreadAlive(char* input, char* output)
{
   int threadId;

   DEBUG_ASSERT(input != NULL && output != NULL);
   DEBUG_ASSERT(input[0] == 'T');

   input++;
   
   if (hexToInt (&input, &threadId) && DEBUG_IS_VALID_THREAD(threadId)) {
      strcpy (output, "OK");
   } else {
      strcpy (output, "E70");
   }
}


/*
 *----------------------------------------------------------------------
 *
 * DebugCurrentThread --
 *
 *	Format: qC
 *
 *	Returns the current world (thread) id.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static void
DebugCurrentThread(char* input, char* output)
{
   char buffer[10];
   char *ptr;

   DEBUG_ASSERT(input != NULL && output != NULL);
   DEBUG_ASSERT(input[0] == 'q' && input[1] == 'C');
   DEBUG_ASSERT_IS_VALID_THREAD(initialGDBThread);

   /*
    * The qC command is generally used only when gdb doesn't know which thread
    * is the active one.  This happens when you first break into the debugger.
    */

   ptr = DebugIntToHexString(initialGDBThread, buffer, sizeof buffer);
   snprintf(output, MAX_MESSAGE_LEN, "QC%s", ptr);
}


/*
 *----------------------------------------------------------------------
 *
 * DebugStepContDetach --
 *
 *	Format: s(<addr>) or c(<addr>) or D
 *
 *	Either single steps or continues at given address if specified,
 *	otherwise at current address.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	Starts the process of exiting the debugger.
 *
 *----------------------------------------------------------------------
 */

static void
DebugStepContDetach(char* input, char* output)
{
   int addr;
   rfeAction action;

   DEBUG_ASSERT(input != NULL && output != NULL);

   /*
    * Don't allow the user to try and start executing code again if the world
    * running the debugger isn't a real world.  However, if the user is asking
    * for a reboot, let them proceed.
    */
   if (worldInDebuggerType != THREADTYPE_REALWORLD && !wantReset) {
      _Log("DebugStepContDetach: Error: Can't resume execution because world "
	   "running debugger is not a 'real world'.\n");
      Debug_PutString("Can't resume execution because debugger is not running "
		      "in a valid world.\nReturning sig 11 to gdb.");
      snprintf(output, BUFMAX, "S11");
      return;
   }

   /*
    * Free all the mappings we're made in this debugging session.
    */
   DebugUnmapMAs(NULL);

   switch (input[0]) {
       case 's':
           action = step;
           break;
       case 'c':
           action = cont;
           break;
       case 'D':
           action = detach;
           DebugPutPacket("OK");
           break;
       default:
	   DEBUG_PANIC("DebugStepContDetach: Invalid input: %c", input[0]);
   }

   input++;

   if (!hexToInt (&input, &addr)) {
      addr = 0;
   }

   DebugReturnFromException (action, addr);

   NOT_REACHED();
}


/*
 *----------------------------------------------------------------------
 *
 * DebugReasonForHalt --
 *
 *	Format: ?
 *
 *	Returns the reason the target halted, ie, the last signal
 *	received.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static void
DebugReasonForHalt(int exceptionVector, char* output)
{
   int sigval;
   char buffer[10];
   char *ptr;

   DEBUG_ASSERT(output != NULL);

   sigval = computeSignal (exceptionVector);

   ptr = DebugIntToHexString(sigval, buffer, sizeof buffer);
   snprintf(output, MAX_MESSAGE_LEN, "S%s", ptr);
}


/*
 *----------------------------------------------------------------------
 *
 * DebugMapMA --
 *
 *	Format: YM<mpn>
 *
 *	Maps the given MA and returns a VA.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	A page is mapped.
 *
 *----------------------------------------------------------------------
 */

static void
DebugMapMA(char* input, char* output)
{
   MA ma;
   KSEG_Pair* pair;
   void* addr;

   DEBUG_ASSERT(input != NULL && output != NULL);
   DEBUG_ASSERT(input[0] == 'Y' && input[1] == 'M');

   input += 2;

   /*
    * Technically they shouldn't be prepending '0x' in front of a hex number
    * because all numbers are treated as hex by default, but we won't penalize
    * them.
    */
   if (input[0] == '0' && input[1] == 'x') {
      input += 2;
   }

   if (hexTo64bitInt(&input, &ma) == 0) {
      snprintf(output, BUFMAX, "Invalid ma.");
      return;
   }

   if (numMappings == MAX_KSEG_MAPPINGS) {
      snprintf(output, BUFMAX, "Too many mappings!");
      return;
   }

   addr = Kseg_GetPtrFromMA(ma, PAGE_SIZE, &pair);
   if (addr == NULL) {
      snprintf(output, BUFMAX, "Kseg_GetPtrFromMA failed.");
      return;
   }

   ksegMappings[numMappings] = pair;
   numMappings++;

   snprintf(output, BUFMAX, "ma: %#"FMT64"x mapped to va: %p", ma, addr);
}


/*
 *----------------------------------------------------------------------
 *
 * DebugUnmapMAs --
 *
 *	Format: Ym
 *
 *	Unmaps all MAs mapped in this debugging session.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	Pages are unmapped.
 *
 *----------------------------------------------------------------------
 */

static void
DebugUnmapMAs(char* output)
{
   int i;

   for (i = 0; i < numMappings; i++) {
      Kseg_ReleasePtr(ksegMappings[i]);
      ksegMappings[i] = NULL;
   }
   numMappings = 0;

   /*
    * output can be NULL because this function is called when we
    * continue/step/detach, and thus don't need to return anything to gdb.
    */
   if (output != NULL) {
      snprintf(output, BUFMAX, "Done.");
   }
}


/*
 *----------------------------------------------------------------------
 *
 * DebugPrintCnxInfo --
 *
 *	Informs the user about what interface we're listening on by
 *	printing to the log and bluescreen.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
void
DebugPrintCnxInfo(Bool changing)
{
   char buf[100];
   char desc[DEBUG_MAX_DESC_LEN];

   Debug_ListeningOn(&kernCtx, desc, DEBUG_MAX_DESC_LEN);
   if (changing) {
      snprintf(buf, sizeof(buf), "Debugger is switching to listening on %s ...\n", desc);
   } else {
      snprintf(buf, sizeof(buf), "Debugger is listening on %s ...\n", desc);
   }

   if (BlueScreen_Posted()) {
      BlueScreen_Append(buf);
   }

   _Log(buf);
}


/*
 *----------------------------------------------------------------------
 *
 * DebugHandleException --
 *
 *	Does all command processing for interfacing to gdb.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
void
DebugHandleException(int exceptionVector)
{
   VMK_ReturnStatus status;
   int		n;

   /*
    * Disable preemption before we call any other kernel code.
    */
   preemptible = CpuSched_DisablePreemption();

   /*
    * Set up the thread -> world mapping state.
    */
   DebugThreadInit();

   debugEverInDebugger = TRUE;
   debugInCall = FALSE;
   cleanExit = FALSE;

   DEBUG_LOG(2, "Entering DebugHandleException...\n");
   for(n = 0; n < sizeof defaultRegisters / sizeof(uint32); n++) {
      DEBUG_LOG(2, "%s: 0x%x\n", regstrings[n], ((uint32*)&defaultRegisters)[n]);
   }
   DEBUG_LOG(2, "current world: %d\n", PRDA_GetRunningWorldIDSafe());

   MemRO_ChangeProtection(MEMRO_WRITABLE);

   /*
    * Flush the TLB because the mappedStack that exists in the world structure
    * is not validated across all CPUs.  We need to make sure that the CPU where
    * we took the trap doesn't have some old stale mapping for a mappedStack
    * that we might be interested in.
    */
   TLB_FLUSH();

   NMI_Disable();

//#define DEBUG_NET_DEBUG
#ifdef DEBUG_NET_DEBUG
   /*
    * In normal cases the debugger fails over to serial withot this, 
    * but not when the network debugger generates an exception, and 
    * since I break the network debugger every time I change something 
    * in net.c, I'll leave this here for my convienience. -wal
    */
   {
      static volatile int first = 1;
      if (first) {
	 first = 0;
	 status = Debug_CnxStart(&kernCtx);
      } else {
	 NetDebug_Shutdown(&kernCtx);

	 Debug_SetSerialDebugging(TRUE);
	 status = Debug_CnxStart(&kernCtx);
	 ASSERT(status == VMK_OK);
      }
   }
#else // ! DEBUG_NET_DEBUG
   status = Debug_CnxStart(&kernCtx);
#endif // (!) DEBUG_NET_DEBUG

   if (status != VMK_OK) {
      /*
       * Fall back to serial debugging.
       */
      Debug_SetSerialDebugging(TRUE);
      status = Debug_CnxStart(&kernCtx);
      ASSERT(status == VMK_OK);
   }

   if (PRDA_GetRunningWorldIDSafe() != 0) {
      debugTask = (Task *)Kseg_DebugMap(MY_RUNNING_WORLD->taskMPN);
   }

   /*
    * Tell the user what connection we're listening on.
    */
   DebugPrintCnxInfo(FALSE);

   DEBUG_LOG(2, "vector=%d, eflags=0x%x, eip=0x%x\n",
	     exceptionVector, defaultRegisters.eflags, defaultRegisters.eip);

   /*
    * Immediately reply with the error number.  Normally gdb won't even see
    * this, however if the user typed 'continue' in gdb, it will wait until we
    * sent it a message before it does anything.  So this is here to kick gdb
    * back into action in the case we're returning from a continue.
    *
    * Do not block waiting for reply to allow user to trigger the local
    * debugger by using the keyboard.
    */
   DebugReasonForHalt(exceptionVector, outputBuffer);
   DebugPutPacketAsync(outputBuffer);

   /*
    * If possible, let the user have a chance to use the local debugger.
    */
   if (BlueScreen_Posted()) {

      BlueScreen_Append("Press Escape to enter local debugger\n");

      while (TRUE) {
	 unsigned char ch;

	 /*
	  * If the programmed debugger is active, go for it.
	  */
	 status = Debug_PollChar(&kernCtx, &ch);
	 if ((status == VMK_OK) && (ch != 0)) {
	    /*
	     * It would be nice if we could remove the message instead.
	     */
	    BlueScreen_Append("Remote debugger activated. "
			      "Local debugger no longer available\n");
	    /*
	     * The debugger must have acknowledged the packet we sent otherwise
	     * resend it synchronously now.
	     */
	    if (ch == '-') {
	       DebugPutPacket(outputBuffer);
	    }
	    break;
	 }

	 /*
	  * Check keyboard activity for access to local debugger
	  */
	 if (Keyboard_Poll() == KEYBOARD_KEY_ESCAPE) {
	    DebugTerm_DisplayForBluescreen(); // synchronous session
	    BlueScreen_On();
	 }

      }

   }

   while (1==1) {
      memset(outputBuffer, 0, BUFMAX);

      status = DebugGetPacket(inputBuffer);
      if (status != VMK_OK) {
         /*
	  * Special case VMK_WAIT_INTERRUPTED because it's only returned when
	  * we're switching from a network to a serial connection.
	  */
         if (status != VMK_WAIT_INTERRUPTED) {
	    _Log("Error receiving packet: %s (%d)\n",
		 VMK_ReturnStatusToString(status), status);
         }

	 continue;
      }

      DEBUG_LOG(3, "incoming packet: %s\n", inputBuffer);

      switch (inputBuffer[0]){

      case '?' :
	 DebugReasonForHalt (exceptionVector, outputBuffer);
	 break;

      case 'g' :
         DebugReadRegisters (inputBuffer, outputBuffer);
	 break;

      case 'G' :
         DebugWriteRegisters (inputBuffer, outputBuffer);
	 break;

      case 'm' :
	 DebugReadMemory (inputBuffer, outputBuffer);
	 break;

      case 'M' :
         DebugWriteMemory (inputBuffer, outputBuffer);
	 break;

      case 's' :
      case 'c' :
      case 'D' :
	 DebugStepContDetach (inputBuffer, outputBuffer);
	 break;

      /*
       * k
       * Kill request.  The protocol isn't clear about what to kill though, ie
       * which thread? all of them?
       *
       * For right now let's not do anything.  -kit
       */
      case 'k' :
	 break;

      /*
       * q
       * General query.  Specifically, we care about thread query packets.
       */
      case 'q' :
         switch (inputBuffer[1]) {
	    case 'C' :
	       DebugCurrentThread (inputBuffer, outputBuffer);
	       break;

	    case 'f' :
	       if (strcmp (inputBuffer, "qfThreadInfo") == 0) {
	          DebugGetThreadInfo (inputBuffer, outputBuffer, FALSE);
	       }
	       break;

	    case 's' :
	       if (strcmp (inputBuffer, "qsThreadInfo") == 0) {
	          DebugGetThreadInfo (inputBuffer, outputBuffer, TRUE);
	       }
	       break;

	    case 'T' :
	       if (strncmp (inputBuffer, "qThreadExtraInfo", 16) == 0) {
	          DebugGetExtraThreadInfo (inputBuffer, outputBuffer);
	       }
	       break;
	 } /* switch */
	 break;

      case 'H' :
         DebugSetThread (inputBuffer, outputBuffer);
	 break;

      case 'T' :
         DebugThreadAlive (inputBuffer, outputBuffer);
	 break;

      /*
       * Y
       * We use 'Y' for our own special vmkernel needs.
       */
      case 'Y':
         switch (inputBuffer[1]) {
	    case 'M':
	       DebugMapMA(inputBuffer, outputBuffer);
	       break;

	    case 'm':
	       DebugUnmapMAs(outputBuffer);
	       break;
	 }
	 break;
      } /* switch */
     
      DEBUG_LOG(3, "outgoing packet: %s\n", outputBuffer);

      /* reply to the request */
      DebugPutPacket (outputBuffer);
   }
}



/* this function is used to set up exception handlers for tracing and
   breakpoints */
void
Debug_Init(void)
{
   defaultStackPtr = &defaultStack[STACK_SIZE/sizeof(int) - 1];
   backupStackPtr = &backupStack[STACK_SIZE/sizeof(int) - 1];

   IDT_RegisterDebugHandler (0, DebugCatchException0);
   IDT_RegisterDebugHandler (1, DebugCatchException1);
   IDT_RegisterDebugHandler (3, DebugCatchException3);
   IDT_RegisterDebugHandler (4, DebugCatchException4);
   IDT_RegisterDebugHandler (5, DebugCatchException5);
   IDT_RegisterDebugHandler (6, DebugCatchException6);
   IDT_RegisterDebugHandler (7, DebugCatchException7);
   IDT_RegisterDebugHandler (8, DebugCatchException8);
   IDT_RegisterDebugHandler (9, DebugCatchException9);
   IDT_RegisterDebugHandler (10, DebugCatchException10);
   IDT_RegisterDebugHandler (11, DebugCatchException11);
   IDT_RegisterDebugHandler (12, DebugCatchException12);
   IDT_RegisterDebugHandler (13, DebugCatchException13);
   IDT_RegisterDebugHandler (14, DebugCatchException14);
   IDT_RegisterDebugHandler (16, DebugCatchException16);

   initialized = 1;
   Debug_CnxInit(&kernCtx, DEBUG_CNX_SERIAL, TRUE);
   serialDebugging = TRUE;
}

Bool 
Debug_IsInitialized(void)
{
   return initialized;
}

/* This function will generate a breakpoint exception.  It is used at the
   beginning of a program to sync up with a debugger and can be used
   otherwise as a quick means to stop program execution and "break" into
   the debugger. */

void
Debug_Break(void)
{
   if (initialized) {
      BREAKPOINT();
   }
   waitabit();
}

int waitlimit = 1000000;

void
waitabit()
{
   int i;
   for (i = 0; i < waitlimit; i++) ;
}

void 
Debug_PutLenString(char *s, int len)
{
   int i;
   char tmp[4];

   for (i = 0; (i < len) && (s[i] != 0); i++) {
      tmp[0] = 'O';
      tmp[1] = hexchars[s[i] >> 4];
      tmp[2] =  hexchars[s[i] % 16];
      tmp[3] = 0;
      DebugPutPacket(tmp);
   }
}

void 
Debug_PutString(char *s)
{
   Debug_PutLenString(s, strlen(s));
}

Bool 
Debug_SerialDebugging(void)
{
   return serialDebugging;
}

/*
 *----------------------------------------------------------------------
 *
 * Debug_SetSerialDebugging --
 *
 *	Changes the debugger's connection setup based on the argument.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	May stop the old connection and start the new one.
 *
 *----------------------------------------------------------------------
 */
void
Debug_SetSerialDebugging(Bool wantIt)
{
   if ((serialDebugging && !wantIt) || (!serialDebugging && wantIt)) {
      VMK_ReturnStatus status;

      /*
       * First, stop the current connection.
       * It's ok if it fails (we may not have started it yet).
       */
      Debug_CnxStop(&kernCtx);

      if (wantIt) {
         /*
	  * Select serial.
	  */
	 status = Debug_CnxInit(&kernCtx, DEBUG_CNX_SERIAL, TRUE);
      } else {
         /*
	  * Select net.
	  */
	 status = Debug_CnxInit(&kernCtx, DEBUG_CNX_NET, TRUE);
      }
      /*
       * Can't DEBUG_ASSERT here since we're swapping connections.
       */
      ASSERT(status == VMK_OK);
        
      serialDebugging = wantIt;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * Debug_CheckSerial --
 *
 *	Called from netDebug.c:NetDebugGetChar.  It checks if the user
 *	called 'vmkdebug wantserial' while we're waiting for a network
 *	packet.  If so, it will call SetSerialDebugging to switch to the
 *	desired connection.
 *
 * Results:
 *	TRUE if SERIAL_WANT_SERIAL is received, FALSE otherwise.
 *
 * Side effects:
 *	Connection may be changed.
 *
 *----------------------------------------------------------------------
 */
Bool
Debug_CheckSerial(void)
{
   unsigned char ch = Serial_PollChar();

   if (ch == SERIAL_WANT_SERIAL) {
      VMK_ReturnStatus status;

      Debug_SetSerialDebugging(TRUE);
      status = Debug_CnxStart(&kernCtx);
      ASSERT(status == VMK_OK);

      /*
       * Inform the user that we're listening on a different connection.
       */
      DebugPrintCnxInfo(TRUE);

      return TRUE;
   }

   /*
    * This is only called when we're spinning in netDebug.c waiting for a
    * network packet.  Thus it's ok to drop characters from the serial port
    * (nothing else should be communicating over it).
    */

   return FALSE;
}

Bool
Debug_EverInDebugger(void)
{
   return debugEverInDebugger;
}

Bool
Debug_InCall(void)
{
   return debugInCall;
}

/*
 *----------------------------------------------------------------------
 *
 * DebugGetPacket --
 *
 *	Scans for the sequence $<data>#<checksum> in the incoming data stream.
 *
 * Results:
 *	The message that came across the wire.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
DebugGetPacket(char *buffer)
{
   VMK_ReturnStatus status;
   unsigned char checksum;
   unsigned char xmitcsum;
   int i;
   int count;
   char ch;

   do {
      ch = 0;
      /* wait around for the start character, ignore all other characters */
      while (ch != '$') {
	 status = Debug_GetChar(&kernCtx, &ch);
	 if (status != VMK_OK) {
	    return status;
	 }
      }
      checksum = 0;
      xmitcsum = -1;

      count = 0;

      /* now, read until a # or end of buffer is found */
      while (count < BUFMAX - 1) {
	 status = Debug_GetChar(&kernCtx, &ch);
	 if (status != VMK_OK) {
	    return status;
	 }

	 if (ch == '#') break;
	 checksum = checksum + ch;
	 buffer[count] = ch;
	 count = count + 1;
      }
      buffer[count] = 0;

      if (ch == '#') {
	 status = Debug_GetChar(&kernCtx, &ch);
	 if (status != VMK_OK) {
	    return status;
	 }
	 xmitcsum = hex(ch) << 4;

	 status = Debug_GetChar(&kernCtx, &ch);
	 if (status != VMK_OK) {
	    return status;
	 }
	 xmitcsum += hex(ch);

	 if (checksum != xmitcsum) {
	    _Log ("bad checksum.  My count = 0x%x, sent=0x%x. buf=%s\n",
		  checksum, xmitcsum, buffer);
	    status = Debug_PutChar(&kernCtx, '-');  /* failed checksum */
	 } else {
	    status = Debug_PutChar(&kernCtx, '+');  /* successful transfer */
	    /* if a sequence char is present, reply the sequence ID */
	    if (status == VMK_OK && buffer[2] == ':') {
	       status = Debug_PutChar (&kernCtx, buffer[0]);
	       if (status == VMK_OK) {
		  status = Debug_PutChar (&kernCtx, buffer[1]);
	       }
	       if (status == VMK_OK) {
		  /* remove sequence chars from buffer */
		  count = strlen(buffer);
		  for (i=3; i <= count; i++) buffer[i-3] = buffer[i];
	       }
	    }
	 }
	 if (status == VMK_OK) {
	    status = Debug_Flush(&kernCtx);
	 }
	 if (status != VMK_OK) {
	    return status;
	 }
      }
   } while (checksum != xmitcsum);

   return VMK_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * DebugDoPutPacket --
 *
 *	Sends packet in buffer, adding checksum.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	A message is sent.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
DebugDoPutPacket(const char *buffer, Bool async)
{
   VMK_ReturnStatus status;
   unsigned char checksum;
   int  count;
   char ch;

   /*  $<packet info>#<checksum>. */
   do {
      status = Debug_PutChar(&kernCtx, '$');
      if (status != VMK_OK) {
	 return status;
      }
      checksum = 0;
      count = 0;

      while ((ch = buffer[count])) {
	 status = Debug_PutChar(&kernCtx, ch);
	 if (status != VMK_OK) {
	    return status;
	 }
	 checksum += ch;
	 count += 1;
	 if (count == BUFMAX) {
	    return VMK_LIMIT_EXCEEDED;
	 }
      }

      status = Debug_PutChar(&kernCtx, '#');
      if (status == VMK_OK) {
         status = Debug_PutChar(&kernCtx, hexchars[checksum >> 4]);
      }
      if (status == VMK_OK) {
         status = Debug_PutChar(&kernCtx, hexchars[checksum % 16]);
      }
      if (status == VMK_OK) {
         status = Debug_Flush(&kernCtx);
      }
      if (status == VMK_OK) {
	 if (async) {
	    return status;
	 } else {
            status = Debug_GetChar(&kernCtx, &ch);
	 }
      }
      if (status != VMK_OK) {
         return status;
      }
   } while (ch != '+');

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * DebugPutPacket --
 *
 * 	Sends packet in buffer, adding checksum and waiting for reply.
 *
 * Results:
 * 	None.
 *
 * Side Effects:
 * 	A message is sent.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
DebugPutPacket(const char *buffer) {
   return DebugDoPutPacket(buffer, FALSE);
}


/*
 *----------------------------------------------------------------------
 *
 * DebugPutPacketAsync --
 *
 * 	Sends packet in buffer, adding checksum and not waiting for reply.
 *
 * Results:
 * 	None.
 *
 * Side Effects:
 * 	A message is sent.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
DebugPutPacketAsync(const char *buffer) {
   return DebugDoPutPacket(buffer, TRUE);
}


/*
 *----------------------------------------------------------------------
 *
 * Debug connection interface.
 *
 *----------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 *
 * Debug_CnxInit --
 *
 *	Initializes the debugger context.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Debug_CnxInit(Debug_Context* dbgCtx, Debug_CnxType type, Bool kernDbg)
{
   VMK_ReturnStatus status;
   dbgCtx->kernelDebugger = kernDbg;

   if (type == DEBUG_CNX_SERIAL) {
      status = Debug_SerialCnxInit(dbgCtx);
   } else if (type == DEBUG_CNX_NET) {
      status = NetDebug_DebugCnxInit(dbgCtx);
   } else if (type == DEBUG_CNX_PROC) {
      status = UserProcDebug_DebugCnxInit(dbgCtx);
   } else { 
      NOT_IMPLEMENTED();
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Debug_CnxStart --
 *
 *	Starts up this debugger connection.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Debug_CnxStart(Debug_Context* dbgCtx)
{
   return dbgCtx->functions->start(dbgCtx);
}


/*
 *----------------------------------------------------------------------
 *
 * Debug_ListeningOn --
 *
 *	Returns a string indicating the device and/or address the debugger is
 *	listening on.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Debug_ListeningOn(Debug_Context* dbgCtx, char* desc, int length)
{
   return dbgCtx->functions->listeningOn(dbgCtx, desc, length);
}


/*
 *----------------------------------------------------------------------
 *  
 * Debug_PutChar --
 *
 *	Sends one character (although it may also be queued until
 *	Debug_Flush is called).
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Debug_PutChar(Debug_Context* dbgCtx, unsigned char ch)
{
   return dbgCtx->functions->putChar(dbgCtx, ch);
}


/*
 *----------------------------------------------------------------------
 *  
 * Debug_Flush --
 *
 *	Flush any queued characters to the output stream.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Debug_Flush(Debug_Context* dbgCtx)
{
   return dbgCtx->functions->flush(dbgCtx);
}


/*
 *----------------------------------------------------------------------
 *  
 * Debug_GetChar --
 *
 *	Gets a character from the network buffer or the serial port.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Debug_GetChar(Debug_Context* dbgCtx, unsigned char* ch)
{
   return dbgCtx->functions->getChar(dbgCtx, ch);
}


/*
 *----------------------------------------------------------------------
 *
 * Debug_PollChar --
 *
 * 	Checks whether a character is available and if so returns it.
 * 	(character 0 is retruned if nothing is available).
 *
 * Results:
 * 	None.
 *
 * Side Effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Debug_PollChar(Debug_Context* dbgCtx, unsigned char* ch)
{
   return dbgCtx->functions->pollChar(dbgCtx, ch);
}


/*
 *----------------------------------------------------------------------
 *
 * Debug_CnxStop --
 *
 *	Stops this debugger connection.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Debug_CnxStop(Debug_Context* dbgCtx)
{
   return dbgCtx->functions->stop(dbgCtx);
}


/*
 *----------------------------------------------------------------------
 *
 * Debug_CnxCleanup --
 *
 *	Cleans up this debugger connection.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Debug_CnxCleanup(Debug_Context* dbgCtx)
{
   return dbgCtx->functions->cleanup(dbgCtx);
}


/*
 *----------------------------------------------------------------------
 *
 * Serial Debugger interface.
 *
 *----------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 *  
 * Debug_SerialCnxInit --
 *
 *	Initializes the serial connection.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Debug_SerialCnxInit(Debug_Context* dbgCtx)
{
   if (!dbgCtx->kernelDebugger) {
      _Log("Currently serial debugging is only supported for the kernel "
      	   "debugger!\n");
      return VMK_FAILURE;
   }

   dbgCtx->functions = &DebugSerialCnxFunctions;

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *  
 * DebugSerialStart --
 *
 *	Open up the serial port.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
DebugSerialStart(Debug_Context* dbgCtx)
{
   Serial_OpenPort(1);

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *  
 * DebugSerialListeningOn --
 *
 *	Return a string saying we're listening on the serial port.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
DebugSerialListeningOn(Debug_Context* dbgCtx, char* desc, int length)
{
   snprintf(desc, length, "serial port");

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *  
 * DebugSerialGetChar--
 *
 *	Simply calls Serial_GetChar.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
DebugSerialGetChar(Debug_Context* dbgCtx, unsigned char* ch)
{
   char tmp;
   tmp = Serial_GetChar();
   *ch = tmp & 0x7f;

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *  
 * DebugSerialPutChar--
 *
 *	Simply calls Serial_PutChar.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
DebugSerialPutChar(Debug_Context* dbgCtx, unsigned char ch)
{
   Serial_PutChar(ch);

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *  
 * DebugSerialFlush --
 *
 *	No-op for serial.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
DebugSerialFlush(Debug_Context* dbgCtx)
{
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *  
 * DebugSerialStop --
 *
 *	No-op for serial.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
DebugSerialStop(Debug_Context* dbgCtx)
{
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * DebugSerialPollChar --
 *
 * 	Check whether a character is available and return it if so
 * 	(character 0 is returned if nothing is available)
 *
 * Results:
 * 	None.
 *
 * Side Effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
DebugSerialPollChar(Debug_Context* dbgCtx, unsigned char *ch)
{
   char tmp;
   tmp = Serial_PollChar();
   *ch = tmp & 0x7f;

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *  
 * DebugSerialCleanup --
 *
 *	No-op for serial.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
DebugSerialCleanup(Debug_Context* dbgCtx)
{
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Debug_UWDebuggerIsEnabled --
 * 
 *	Checks whether userworld debugging is enabled.
 *
 * Results:
 *	Returns TRUE if the userworld debugging flag is enabled, FALSE
 *	otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Bool
Debug_UWDebuggerIsEnabled(void)
{
   return userWorldDebugEnabled;
}


/*
 *----------------------------------------------------------------------
 *
 * Debug_UWDebuggerEnable --
 * 
 *	Turns userworld debugging on or off.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	userWorldDebugEnabled is modified.
 *
 *----------------------------------------------------------------------
 */
void
Debug_UWDebuggerEnable(Bool enable)
{
   userWorldDebugEnabled = enable;
}


/*
 *----------------------------------------------------------------------
 *
 * Debug_SetCosGetCharFn --
 *
 *      Set the getchar function used to access cos memory.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

void
Debug_SetCosGetCharFn(int (*fn)(void*))
{
   _Log("Setting cosGetCharFn from 0x%p to 0x%p", cosGetCharFn, fn);
   cosGetCharFn = fn;
}
