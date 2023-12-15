/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

#include "user_int.h"
#include "debug.h"
#include "net.h"
#include "world.h"
#include "util.h"
#include "cpuid_info.h"
#include "uwvmkDispatch.h"
#include "host.h"
#include "userProcDebug.h"
#include "userMem.h"
#include "user_layout.h"
#include "kvmap.h"

#define LOGLEVEL_MODULE UserDebug
#include "userLog.h"

/*
 * Size for the input/output buffers.  Should be at least NUM_REG_BYTES * 2 so
 * that register packets can be sent.
 */
#define BUFMAX          400

/*
 * Number of registers.
 */
#define NUM_REGS        16

/*
 * Number of bytes of registers.
 */
#define NUM_REG_BYTES   (NUM_REGS * 4)


enum regnames { GDB_EAX, GDB_ECX, GDB_EDX, GDB_EBX, GDB_ESP, GDB_EBP, GDB_ESI,
		GDB_EDI,
		GDB_PC /* also known as eip */,
		GDB_PS /* also known as eflags */,
		GDB_CS, GDB_SS, GDB_DS, GDB_ES, GDB_FS, GDB_GS };

static const char hexchars[]="0123456789abcdef";

static int  UserDebugHex(char ch);
static void UserDebugSerialize(char* dest, char* src, int length);
static void UserDebugDeserialize(char* dest, char* src, int length);
static int  UserDebugDeserializeInt(int* dest, char** src, Bool bigEndian);

static VMK_ReturnStatus UserDebugCopyIn(char* dest, UserVA src, int length);
static VMK_ReturnStatus UserDebugCopyOut(UserVA dest, char* src, int length);

static VMK_ReturnStatus UserDebugPutPacket(const char *buffer);
static VMK_ReturnStatus UserDebugGetPacket(char *buffer);


static void UserDebugReasonForHalt(uint32 vector, char* output);

static void UserDebugReadRegisters(char* output);
static void UserDebugWriteRegisters(char* input, char* output);
static void UserDebugSetRegister(char* input, char* output);

static void UserDebugReadMemory(char* input, char* output);
static void UserDebugWriteMemory(char* input, char* output);

static void UserDebugStepContinueDetach(char* input);

static void UserDebugGetThreadInfo(char* input, char* output);
static void UserDebugGetExtraThreadInfo(char* input, char* output);
static void UserDebugSetThread(char* input, char* output);
static void UserDebugThreadAlive(char* input, char* output);
static void UserDebugCurrentThread(char* input, char* output);

static void UserDebugCreateThreadList(void);
static void UserDebugUpdateThreadList(void);

static void UserDebugMainLoop(uint32 vector);

static NORETURN void UserDebugCartelShutdown(uint32 vector);
static Bool UserDebugEntry(uint32 vector);

/*
 *----------------------------------------------------------------------
 *
 * UserDebugHex --
 *
 *      Converts an ascii hex character to its binary representation.
 *                        
 * Results:
 *      None.
 *                        
 * Side effects:
 *      None.
 *          
 *----------------------------------------------------------------------
 */
static int
UserDebugHex(char ch)
{
   if ((ch >= 'a') && (ch <= 'f')) return (ch-'a'+10);
   if ((ch >= '0') && (ch <= '9')) return (ch-'0');
   if ((ch >= 'A') && (ch <= 'F')) return (ch-'A'+10);
 
   return (-1);
}


/*
 *----------------------------------------------------------------------
 *
 * UserDebugSerialize --
 *
 *      Converts binary data to its ASCII hex representation.
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
UserDebugSerialize(char* dest, char* src, int length)
{
   int i;

   for (i = 0; i < length; i++) {
      unsigned char ch = (unsigned char)src[i];

      dest[i*2] = hexchars[ch >> 4];
      dest[i*2+1] = hexchars[ch % 16];
   }

   dest[length*2] = 0;
}


/*
 *----------------------------------------------------------------------
 *
 * UserDebugDeserialize --
 *
 *      Converts an ASCII hex string to its binary form.
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
UserDebugDeserialize(char* dest, char* src, int length)
{
   int i;

   for (i = 0; i < length; i++) {
      unsigned char ch;

      ch = UserDebugHex(*src) << 4;
      src++;
      ch = ch + UserDebugHex(*src);
      src++;

      dest[i] = (char)ch;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * UserDebugDeserializeInt --
 *
 *      Converts an ASCII hex string to its integer binary form, taking
 *	into account endianness.
 *                        
 * Results:
 *      None.
 *                        
 * Side effects:
 *      None.
 *          
 *----------------------------------------------------------------------
 */
static int
UserDebugDeserializeInt(int* dest, char** src, Bool bigEndian)
{
   int numChars;
   unsigned char tmp[8];

   /*
    * Iterate until we come across a non-hex character.
    */
   while(**src && UserDebugHex(**src) < 0) {
      (*src)++;
   }

   if (**src == 0) {
      return 0;
   }

   memset(tmp, '0', sizeof(tmp));
   numChars = 0;
   *dest = 0;

   if (bigEndian) {
      while (**src) {
         int hexValue = UserDebugHex(**src);
	 if (hexValue < 0) {
	    break;
	 }
	 *dest = (*dest << 4) | hexValue;
	 numChars++;
	 (*src)++;
      }
   } else {
      int i;

      /*
       * For little-endian strings, we need to convert to big-endian and right
       * justify.
       */
      for (numChars = 0; numChars < 8; numChars += 2) {
	 char ch = **src;

	 if (ch == 0 || UserDebugHex(ch) < 0) {
	    break;
	 }

	 (*src)++;
	 tmp[7 - numChars - 1] = ch;

	 ch = **src;
	 if (ch == 0 || UserDebugHex(ch) < 0) {
	    // XXX: Technically, this should be an error.
	    break;
	 }

	 (*src)++;
	 tmp[7 - numChars] = ch;
      }

      /*
       * Finally convert the ASCII representation to an actual number.
       */
      for (i = 8 - numChars; i < 8; i++) {
	 *dest = (*dest << 4) | UserDebugHex(tmp[i]);
      }
   }

   return numChars;
}


/*
 *----------------------------------------------------------------------
 *
 * UserDebugCopyIn --
 *
 *      Copy count bytes starting at mem from userspace, then convert
 *	from binary to ascii hex.
 *                        
 * Results:
 *      None.
 *                        
 * Side effects:
 *      None.
 *          
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserDebugCopyIn(char* dest, UserVA src, int length)
{ 
   char tmp[BUFMAX];

   VMK_ReturnStatus status;

   ASSERT(length < BUFMAX);

   UWLOG(2, "dest: %p  src: %x  len: %d", dest, src, length);

   status = User_CopyIn(tmp, src, length);
   
   if (status == VMK_OK) {
      UserDebugSerialize(dest, tmp, length);
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserDebugCopyOut --
 *
 *      First converts ascii hex string in buf to binary then copies out
 *	count bytes to mem in userspace.
 *                        
 * Results:
 *      None.
 *                        
 * Side effects:
 *      None.
 *          
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserDebugCopyOut(UserVA dest, char* src, int length)
{  
   char tmp[BUFMAX];

   ASSERT(length < BUFMAX);

   UWLOG(2, "dest: %x  src: %p  len: %d", dest, src, length);

   UserDebugDeserialize(tmp, src, length);

   return User_CopyOut(dest, (const void*)tmp, length);
}


/*
 *----------------------------------------------------------------------
 *
 * UserDebugPutPacket --
 *
 *      Sends a packet of data to a remote gdb.
 *                        
 * Results:
 *      None.
 *                        
 * Side effects:
 *      None.
 *          
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserDebugPutPacket(const char *buffer)
{
   VMK_ReturnStatus status;
   unsigned char checksum;
   int count;
   char ch;
   Debug_Context* dbgCtx = &MY_RUNNING_WORLD->userCartelInfo->debugger.dbgCtx;

   /*
    * Format: $<packet info>#<checksum>
    */
   do { 
      status = Debug_PutChar(dbgCtx, '$');
      if (status != VMK_OK) {
         return status;
      }
      checksum = 0;
      count = 0;

      while ((ch = buffer[count])) {
         status = Debug_PutChar(dbgCtx, ch);
	 if (status != VMK_OK) {
	    return status;
	 }
	 checksum += ch;
	 count += 1;
      }

      status = Debug_PutChar(dbgCtx, '#');
      if (status == VMK_OK) {
         status = Debug_PutChar(dbgCtx, hexchars[checksum >> 4]);
      }
      if (status == VMK_OK) {
         status = Debug_PutChar(dbgCtx, hexchars[checksum % 16]);
      }
      if (status == VMK_OK) {
         status = Debug_Flush(dbgCtx);
      }
      if (status == VMK_OK) {
         status = Debug_GetChar(dbgCtx, &ch);
      }
      if (status != VMK_OK) {
         return status;
      }
      ch = ch & 0x7f;
   } while (ch != '+');

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserDebugGetPacket --
 *
 *      Receives a packet of data from a remote gdb.
 *                        
 * Results:
 *      None.
 *                        
 * Side effects:
 *      None.
 *          
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserDebugGetPacket(char *buffer)
{
   VMK_ReturnStatus status;
   unsigned char checksum;
   unsigned char xmitcsum;
   int i;
   int count;
   char ch;
   Debug_Context* dbgCtx = &MY_RUNNING_WORLD->userCartelInfo->debugger.dbgCtx;

   do {
      ch = 0;
      /*
       * Wait around for the start character, ignore all other characters.
       */
      while (ch != '$') {
         status = Debug_GetChar(dbgCtx, &ch);
	 if (status != VMK_OK) {
	    return status;
	 }
      }
      checksum = 0;
      xmitcsum = -1;

      count = 0;

      /*
       * Now, read until a # or end of buffer is found.
       */
      while (count < BUFMAX) {
         status = Debug_GetChar(dbgCtx, &ch);
	 if (status != VMK_OK) {
	    return status;
	 }
	 if (ch == '#') {
	    break;
	 }
	 checksum = checksum + ch;
	 buffer[count] = ch;
	 count = count + 1;
      }
      buffer[count] = 0;

      if (ch == '#') {
         status = Debug_GetChar(dbgCtx, &ch);
	 if (status != VMK_OK) {
	    return status;
	 }
	 xmitcsum = UserDebugHex(ch/* & 0x7f*/) << 4;
	 status = Debug_GetChar(dbgCtx, &ch);
	 if (status != VMK_OK) {
	    return status;
	 }
	 xmitcsum += UserDebugHex(ch/* & 0x7f*/);

	 if (checksum != xmitcsum) {
	    UWLOG(0, "bad checksum.  My count = 0x%x, sent=0x%x. buf=%s",
		  checksum, xmitcsum, buffer);
	 }

	 if (checksum != xmitcsum) {
	    // Failed checksum
	    status = Debug_PutChar(dbgCtx, '-');
	 } else {
	    // Successful transfer
	    status = Debug_PutChar(dbgCtx, '+');
	    // If a sequence char is present, reply with the sequence ID.
	    if (status == VMK_OK && buffer[2] == ':') {
	       status = Debug_PutChar(dbgCtx, buffer[0]);
	       if (status == VMK_OK) {
	          Debug_PutChar(dbgCtx, buffer[1]);
	       }
	       if (status == VMK_OK) {
		  // Remove sequence chars from buffer.
		  count = strlen(buffer);
		  for (i = 3; i <= count; i++) {
		     buffer[i-3] = buffer[i];
		  }
	       }
	    }
	 }
	 if (status == VMK_OK) {
	    Debug_Flush(dbgCtx);
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
 * UserDebugReasonForHalt --
 *
 *      Returns the UNIX signal value based on the exception that
 *      occurred.
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
UserDebugReasonForHalt(uint32 vector, char* output)
{
   int sigval;
   
   /*
    * First convert the Intel processor exception vector to a UNIX signal
    * number.
    */
   sigval = UserSig_FromIntelException(vector);
   
   output[0] = 'S';
   output[1] = hexchars[sigval >> 4];
   output[2] = hexchars[sigval % 16];
   output[3] = 0;
}


/*
 *----------------------------------------------------------------------
 *
 * UserDebugReadRegisters --
 *
 *      Copies the value of the registers to the output buffer.
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
UserDebugReadRegisters(char* output)
{
   World_Handle* currentWorld = MY_RUNNING_WORLD;
   UserDebug_State* dbg = &currentWorld->userCartelInfo->debugger;
   Reg32 registers[NUM_REGS];

   // Copy the register data into gdb's format.
   registers[GDB_EAX] = dbg->currentUserState->regs.eax;
   registers[GDB_ECX] = dbg->currentUserState->regs.ecx;
   registers[GDB_EDX] = dbg->currentUserState->regs.edx;
   registers[GDB_EBX] = dbg->currentUserState->regs.ebx;
   registers[GDB_ESP] = dbg->currentUserState->frame.esp;
   registers[GDB_EBP] = dbg->currentUserState->regs.ebp;
   registers[GDB_ESI] = dbg->currentUserState->regs.esi;
   registers[GDB_EDI] = dbg->currentUserState->regs.edi;
   registers[GDB_PC] = dbg->currentUserState->frame.eip;
   registers[GDB_PS] = dbg->currentUserState->frame.eflags;
   registers[GDB_CS] = (dbg->currentUserState->frame.__csu << 16) |
			dbg->currentUserState->frame.cs;
   registers[GDB_SS] = (dbg->currentUserState->frame.__ssu << 16) |
			dbg->currentUserState->frame.ss;
   registers[GDB_DS] = dbg->currentUserState->regs.ds;
   registers[GDB_ES] = dbg->currentUserState->regs.es;
   registers[GDB_FS] = dbg->currentUserState->regs.fs;
   registers[GDB_GS] = dbg->currentUserState->regs.gs;

   UserDebugSerialize(output, (char*)registers, NUM_REG_BYTES);
}


/*
 *----------------------------------------------------------------------
 *
 * UserDebugWriteRegisters --
 *
 *      Sets the value of the registers to the specified value.
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
UserDebugWriteRegisters(char* input, char* output)
{
   UserDebug_State* dbg = &MY_RUNNING_WORLD->userCartelInfo->debugger;
   Reg32 registers[NUM_REGS];

   input++;

   UserDebugDeserialize((char*)registers, input, NUM_REG_BYTES);

   dbg->currentUserState->regs.eax = registers[GDB_EAX];
   dbg->currentUserState->regs.ecx = registers[GDB_ECX];
   dbg->currentUserState->regs.edx = registers[GDB_EDX];
   dbg->currentUserState->regs.ebx = registers[GDB_EBX];
   dbg->currentUserState->frame.esp = registers[GDB_ESP];
   dbg->currentUserState->regs.ebp = registers[GDB_EBP];
   dbg->currentUserState->regs.esi = registers[GDB_ESI];
   dbg->currentUserState->regs.edi = registers[GDB_EDI];
   dbg->currentUserState->frame.eip = registers[GDB_PC];
   dbg->currentUserState->frame.eflags = registers[GDB_PS];
   dbg->currentUserState->frame.cs = (uint16)((uint32)registers[GDB_CS] & 0xffff);
   dbg->currentUserState->frame.__csu = (uint16)((uint32)registers[GDB_CS] >> 16);
   dbg->currentUserState->frame.ss = (uint16)((uint32)registers[GDB_SS] & 0xffff);
   dbg->currentUserState->frame.__ssu = (uint16)((uint32)registers[GDB_SS] >> 16);
   dbg->currentUserState->regs.ds = registers[GDB_DS];
   dbg->currentUserState->regs.es = registers[GDB_ES];
   dbg->currentUserState->regs.fs = registers[GDB_FS];
   dbg->currentUserState->regs.gs = registers[GDB_GS];
   
   strcpy(output, "OK");
}

/*
 *----------------------------------------------------------------------
 *                        
 * UserDebugSetRegister --
 *                  
 *      Sets the given register to the specified value.
 *
 *	Format: P<regno>=<value>
 *		regno - big endian
 *		value - little endian
 *
 *	Errors:
 		E01 - invalid format
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
UserDebugSetRegister(char* input, char* output)
{
   UserDebug_State* dbg = &MY_RUNNING_WORLD->userCartelInfo->debugger;
   int regno;
   int reg;
   int err = 0;
   
   input++;
   
   if (UserDebugDeserializeInt(&regno, &input, TRUE) == 0) {
      err = 1;
   }
   if (!err && *(input++) != '=') {
      err = 1;
   }
   /*
    * XXX: Occasionally, gdb likes to change random registers (such as foseg).
    * Not sure why, but not doing it and replying OK seems to work just fine.
    * So I've removed the check here and the assert in the switch statement.
    */
   //if (!err && (regno < 0 || regno >= NUM_REGS)) err = 1;
   if (!err && UserDebugDeserializeInt(&reg, &input, FALSE) == 0) {
      err = 1;
   }
   
   if (err == 1) {
      strcpy(output, "E01");
      return;
   }

   switch (regno) {
      case GDB_EAX:
	 dbg->currentUserState->regs.eax = reg; 
	 break;
      case GDB_ECX:
	 dbg->currentUserState->regs.ecx = reg;
	 break;
      case GDB_EDX:
	 dbg->currentUserState->regs.edx = reg;
	 break;
      case GDB_EBX:
	 dbg->currentUserState->regs.ebx = reg;
	 break;
      case GDB_ESP:
	 dbg->currentUserState->frame.esp = reg;
	 break;
      case GDB_EBP:
	 dbg->currentUserState->regs.ebp = reg;
	 break;
      case GDB_ESI:
	 dbg->currentUserState->regs.esi = reg;
	 break;
      case GDB_EDI:
	 dbg->currentUserState->regs.edi = reg;
	 break;
      case GDB_PC:
	 dbg->currentUserState->frame.eip = reg;
	 break;
      case GDB_PS:
	 dbg->currentUserState->frame.eflags = reg;
	 break;
      case GDB_CS:
	 dbg->currentUserState->frame.cs = (uint16)(reg & 0xffff);
	 dbg->currentUserState->frame.__csu = (uint16)(reg >> 16);
	 break;
      case GDB_SS:
	 dbg->currentUserState->frame.ss = (uint16)(reg & 0xffff);
	 dbg->currentUserState->frame.__ssu = (uint16)(reg >> 16);
	 break;
      case GDB_DS:
	 dbg->currentUserState->regs.ds = reg;
	 break;
      case GDB_ES:
	 dbg->currentUserState->regs.es = reg;
	 break;
      case GDB_FS:
	 dbg->currentUserState->regs.fs = reg;
	 break;
      case GDB_GS:
	 dbg->currentUserState->regs.gs = reg;
	 break;
      default:
         //ASSERT("Invalid register.");
	 break;
   }
         
   strcpy(output, "OK");
}


/*
 *----------------------------------------------------------------------
 *                        
 * UserDebugReadMemory --
 *                  
 *      Reads the given data from the specified address.  mem2hex
 *      will take care of any exceptions that occur from that operation.
 *
 *	Format: m<addr>,<len>
 *		addr - big endian
 *		len - big endian
 *
 *	Errors:
 *		E05 - invalid format
 *		E06 - unable to copy in data from userspace
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
UserDebugReadMemory(char* input, char* output)
{
   int err = 0;
   int addr;
   int length;
   VMK_ReturnStatus status;

   input++;
   
   if (UserDebugDeserializeInt(&addr, &input, TRUE) == 0) {
      err = 1;
   }
   if (!err && *(input++) != ',') {
      err = 1;
   }
   if (!err && UserDebugDeserializeInt(&length, &input, TRUE) == 0) {
      err = 1;
   }
   
   if (err == 1) {
      strcpy(output, "E05");
      return;
   }

   status = UserDebugCopyIn(output, (UserVA)addr, length);

   if (status != VMK_OK) {
      strcpy(output, "E06");
      UWLOG(0, "debug: m - memory fault at %#x, len %d", (UserVA)addr, length);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * UserDebugIsTryingToWriteBP --
 *
 *	Checks whether GDB is trying to write a breakpoint instruction
 *	and failed because of access permissions.  Because all UserWorld
 *	code is now read-only, trying to write a breakpoint will fail.
 *	This function will tell us whether we need to force the write.
 *
 * Results:
 *	TRUE if GDB was trying to write a breakpoint, FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
UserDebugIsTryingToWriteBP(VMK_ReturnStatus status, int addr, int length)
{
   /*
    * We should get an access violation.  If the page is just unmapped, there's
    * nothing we can do about it.
    */
   if (status != VMK_NO_ACCESS) {
      return FALSE;
   }

   /*
    * The length of a breakpoint instruction is 1 byte.  Note that we don't
    * explicitly check to make sure this write is actually a breakpoint
    * instruction (0xcc).  We do this because when GDB is finished with the
    * breakpoint, it will write back the old value that was originally there, so
    * we want to allow it to write anything so long as it's only one byte long.
    */
   if (length != 1) {
      UWLOG(1, "Not allowing write because length (%d) != 1", length);
      return FALSE;
   }

   /*
    * Finally, this write must be in the code segment.
    */
   if (!VMK_USER_IS_ADDR_IN_CODE_SEGMENT(addr)) {
      UWLOG(1, "Not allowing write because address (%d) not in code segment",
	    addr);
      return FALSE;
   }

   return TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * UserDebugWriteBP --
 *
 *	Writes out a breakpoint instruction at the given address.  If
 *	necessary, it will also change the protections of the current
 *	page as well as fault it in.
 *
 * Results:
 *	VMK_OK on success, otherwise on failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserDebugWriteBP(VMK_ReturnStatus oldStatus, int addr, char *input, int length)
{
   VMK_ReturnStatus status = VMK_OK;
   Bool resetProtections = FALSE;
   int pgAlignAddr = addr & ~(PAGE_SIZE - 1);
   MPN mpn;

   if (oldStatus == VMK_NO_ACCESS) {
      /*
       * Looks like we don't have write access.  Let's temporarily give
       * ourselves write access while we touch the page.
       */
      status = UserMem_Protect(MY_RUNNING_WORLD, pgAlignAddr, PAGE_SIZE,
			       LINUX_MMAP_PROT_READ | LINUX_MMAP_PROT_WRITE |
			       LINUX_MMAP_PROT_EXEC);
      if (status != VMK_OK) {
	 UWWarn("Error making addr %#x writeable: %s", pgAlignAddr,
	       UWLOG_ReturnStatusToString(status));
	 return status;
      }

      resetProtections = TRUE;
   }

   /*
    * Since we're not sure if the page that we're trying to access is even
    * mapped in yet, call User_GetPageMPN to touch the page and return a
    * MPN for us to use.
    *
    * XXX: There is a problem with this approach.  Things should work now
    * but need to be fixed before release.  See bug 49109.
    */
   status = User_GetPageMPN(MY_RUNNING_WORLD, VA_2_VPN(addr),
			    USER_PAGE_NOT_PINNED, &mpn);
   if (status == VMK_OK) {
      uint8 *page;

      page = KVMap_MapMPN(mpn, TLB_LOCALONLY);
      if (page != NULL) {
	 /*
	  * Now that we have this MPN mapped to a page in kernel space,
	  * simply call UserDebugDeserialize to write the breakpoint out
	  * to memory.
	  */
	 UserDebugDeserialize(page + PAGE_OFFSET(addr), input, length);
	 KVMap_FreePages(page);
      } else {
	 UWLOG(0, "KVMap_MapMPN failed.");
	 status = VMK_NO_RESOURCES;
      }
   } else {
      UWLOG(0, "User_GetPageMPN failed for addr %#x: %s\n", addr,
	    UWLOG_ReturnStatusToString(status));
   }

   if (resetProtections) {
      VMK_ReturnStatus tmpStatus;
      tmpStatus = UserMem_Protect(MY_RUNNING_WORLD, pgAlignAddr, PAGE_SIZE,
				  LINUX_MMAP_PROT_READ | LINUX_MMAP_PROT_EXEC);
      if (tmpStatus != VMK_OK) {
	 /*
	  * Failing to make the page non-writeable isn't a fatal error, so
	  * just print out a message and act like it never happened.
	  */
	 UWWarn("Error making addr %#x non-writeable: %s", pgAlignAddr,
	       UWLOG_ReturnStatusToString(tmpStatus));
      }
      /*
       * Drop tmpStatus.
       */
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *                        
 * UserDebugWriteMemory --
 *                  
 *      Writes the given data to the specified address.  hex2mem
 *      will take care of any exceptions that occur from that operation.
 *                        
 *	Format: M<addr>,<len>:<value>
 *		addr - big endian
 *		len - big endian
 *		value - little endian
 *
 *	Errors:
 *		E07 - invalid format
 *		E08 - unable to copy out data to userspace
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
UserDebugWriteMemory(char* input, char* output)
{
   int err = 0;
   int addr;
   int length;
   VMK_ReturnStatus status;

   input++;

   if (UserDebugDeserializeInt(&addr, &input, TRUE) == 0) {
      err = 1;
   }
   if (!err && *(input++) != ',') {
      err = 1;
   }
   if (!err && UserDebugDeserializeInt(&length, &input, TRUE) == 0) {
      err = 1;
   }
   if (!err && *(input++) != ':') {
      err = 1;
   }

   if (err == 1) {
      strcpy(output, "E07");
      return;
   }

   /*
    * Since <value> is little endian, we can directly copy it to memory.
    */
   status = UserDebugCopyOut((UserVA)addr, input, length);

   /*
    * Check if we're trying to write in a breakpoint in the code segment.
    */
   if (UserDebugIsTryingToWriteBP(status, addr, length)) {
      status = UserDebugWriteBP(status, addr, input, length);
   }

   if (status != VMK_OK) {
      strcpy(output, "E08");
      UWLOG(0, "debug: M - memory fault at %#x, len %d", (UserVA)addr, length);
   } else {
      strcpy(output, "OK");
   }
}


/*
 *----------------------------------------------------------------------
 *                        
 * UserDebugStepContinueDetach --
 *                  
 *      Resumes execution of the debugged program.
 *
 *	Format: (c|s)<addr> or D
 *		addr - big endian
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
UserDebugStepContinueDetach(char* input)
{
   UserDebug_State* dbg = &MY_RUNNING_WORLD->userCartelInfo->debugger;
   int stepping;
   int addr;

   if (input[0] == 'D') {
      UserDebugPutPacket("OK");
   }
  
   stepping = (input[0] == 's');
   input++;
   
   if (UserDebugDeserializeInt(&addr, &input, TRUE)) {
      dbg->currentUserState->frame.eip = (Reg32)addr;
   }
   
   /*
    * Clear the trace bit.
    */
   dbg->currentUserState->frame.eflags &= 0xfffffeff;
   
   /*
    * Set the trace bit if we're steppin.
    */
   if (stepping) {
      dbg->currentUserState->frame.eflags |= 0x100;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * UserDebugGetThreadInfo --
 *
 *      Format: qfThreadInfo
 *
 *      Returns a list of active worlds' world ids in the output buffer.
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
UserDebugGetThreadInfo(char* input, char* output)
{
   UserDebug_State* dbg = &MY_RUNNING_WORLD->userCartelInfo->debugger;
   int n;
   
   *output = 'm';
   output++; 
  
   for(n = 1; n < dbg->numWorlds + 1; n++) {
      output[0] = hexchars[n >> 4];
      output[1] = hexchars[n % 16];
      output += 2;
      
      if(dbg->numWorlds != n) {
         *output = ',';                                          
         output++;
      }
   }
}


/*
 *----------------------------------------------------------------------
 * 
 * UserDebugGetExtraThreadInfo --
 *
 *      Format: qThreadExtraInfo,<id>
 *		id - big endian
 *      
 *      Returns a printable string description for the given thread id.
 *
 *	Errors:
 *		E50 - invalid format, unable to read <id>
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
UserDebugGetExtraThreadInfo(char* input, char* output)
{
   UserDebug_State* dbg = &MY_RUNNING_WORLD->userCartelInfo->debugger;
   Thread_ID threadID;
   World_Handle* world;
   char worldName[WORLD_NAME_LENGTH+1];
 
   /*
    * Advance past 'qThreadExtraInfo,'.
    */
   input += 17;
   if (!UserDebugDeserializeInt(&threadID, &input, TRUE)) {
      strcpy (output, "E50");
      return;
   }

   world = World_Find(dbg->threadToWorldMap[threadID]);
   ASSERT(NULL != world);

   /*
    * If this world is the world that the debugger broke into, mark that for the
    * user.
    */
   if(dbg->threadToWorldMap[threadID] == dbg->initialWorld) {
      snprintf(worldName, sizeof(worldName), "#%d %.20s", dbg->initialWorld,
      	       world->worldName);
   } else {
      snprintf(worldName, sizeof(worldName), "%d %.20s",
      	       dbg->threadToWorldMap[threadID], world->worldName);
   }

   World_Release(world);

   UserDebugSerialize(output, worldName, strlen(worldName));
}


/*
 *----------------------------------------------------------------------
 *
 * UserDebugSetThread --
 *
 *      Format: H<c><t>
 *		t - big endian
 *
 *      <c> specifies which operations should be affected, either 'c'
 *      for step and continue or 'g' for all other operations.
 *      <t> is the thread id.  If <t> is 0, pick any thread.  If <c> is
 *      'c', then the thread id can be -1, which applies the operations
 *      to all threads.
 *
 *	Errors:
 *		E60 - <c> is neither 'c' or 'g'
 *		E61 - negative value for <t> when <c> is not 'c'
 *		E62 - <t> is neither -1 or thread id of active world
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      If <c> is 'c', changes csTarget to the value of <t>.  Otherwise,
 *      changes otherTarget to value of <t>.
 *
 *----------------------------------------------------------------------
 */
static void
UserDebugSetThread(char* input, char* output)
{
   UserDebug_State* dbg = &MY_RUNNING_WORLD->userCartelInfo->debugger;
   Thread_ID threadID;
   
   if (input[1] != 'c' && input[1] != 'g') {
      strcpy(output, "E60");
      return;
   }

   if (input[2] == '-' && input[3] == '1') {
      if (input[1] == 'c') {
         threadID = -1;
      } else {
         strcpy(output, "E62");
         return;
      }
   } else {
      char* ptr = input + 2;

      if (!UserDebugDeserializeInt(&threadID, &ptr, TRUE)) {
         strcpy(output, "E61");
         return;
      }
   }

   /*
    * If they specify zero, we can pick any thread.
    */
   if (threadID == 0) {
      threadID = dbg->initialThread;
   }

   /*
    * The threadID must be that of an active world or -1.
    */
   if(threadID != -1 && !UserThread_IsPeerDebug(dbg->threadToWorldMap[threadID])) {
      strcpy(output, "E62");
      return;
   }
   
   if (input[1] == 'c') {
      dbg->targetContStep = threadID;
   } else if (dbg->targetOther != threadID) {
      User_ThreadInfo* uti;
      World_Handle* world;

      world = World_Find(dbg->threadToWorldMap[threadID]);
      ASSERT(world != NULL);

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
      uti = world->userThreadInfo;
      ASSERT_BUG(36090, uti->exceptionFrame != NULL);
      dbg->currentUserState = uti->exceptionFrame;

      World_Release(world);

      dbg->targetOther = threadID;
   }

   strcpy (output, "OK");
}


/*
 *----------------------------------------------------------------------
 *
 * UserDebugThreadAlive --
 *
 *      Format: T<id>
 *		id - big endian
 *
 *      Returns OK in the output buffer if the specified world exists
 *      and is active.
 *
 *	Errors:
 *		E70 - invalid format (unable to read <id>) or <id> is not a
 *		      thread id of an active world
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
UserDebugThreadAlive(char* input, char* output)
{
   UserDebug_State* dbg = &MY_RUNNING_WORLD->userCartelInfo->debugger;
   Thread_ID threadID;
   
   input++;
   
   if (UserDebugDeserializeInt(&threadID, &input, TRUE) &&
       UserThread_IsPeerDebug(dbg->threadToWorldMap[threadID])) {
      strcpy (output, "OK");
   } else {
      strcpy (output, "E70");
   }
}


/*
 *----------------------------------------------------------------------
 *
 * UserDebugCurrentThread --
 *
 *      Format: qC
 *
 *      Returns the current world (thread) id.
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
UserDebugCurrentThread(char* input, char* output)
{
   UserDebug_State* dbg = &MY_RUNNING_WORLD->userCartelInfo->debugger;

   /*
    * The qC command is generally used only when gdb doesn't know which thread
    * is the active one.  This happens when you first break into the debugger.
    */
   output[0] = 'Q';
   output[1] = 'C';
   output[2] = hexchars[dbg->initialThread >> 4];
   output[3] = hexchars[dbg->initialThread % 16];
}


/*
 *----------------------------------------------------------------------
 *
 * UserDebugCreateThreadList --
 *
 *	Generates the threadToWorldMap that this stub uses to translate between
 *	gdb thread ids and world ids.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	threadToWorldMap is created.
 *          
 *----------------------------------------------------------------------
 */
static void
UserDebugCreateThreadList(void)
{
   UserDebug_State* dbg = &MY_RUNNING_WORLD->userCartelInfo->debugger;
   int i;

   UWLOG(1, "Creating thread list...");

   for (i = 0; i < ARRAYSIZE(dbg->threadToWorldMap); i++) {
      dbg->threadToWorldMap[i] = INVALID_WORLD_ID;
   }

   dbg->numWorlds = UserThread_NumPeersDebug();
   i = UserThread_GetPeersDebug(dbg->threadToWorldMap+1);
   ASSERT(i == dbg->numWorlds);
   for (i = 1; i < dbg->numWorlds + 1; i++) {
      if (dbg->threadToWorldMap[i] == dbg->initialWorld) {
	 dbg->initialThread = i;
	 break;
      }
   }

   for (i = 1; i < dbg->numWorlds + 1; i++) {
      UWLOG(1, "thread %d -> world %d", i, dbg->threadToWorldMap[i]);
   }
   UWLOG(1, "initialThread: %d initalWorld: %d", dbg->initialThread,
	 dbg->initialWorld);

   ASSERT(dbg->initialThread != -1);
   dbg->targetContStep = dbg->targetOther = dbg->initialThread;
}


/*
 *----------------------------------------------------------------------
 *
 * UserDebugUpdateThreadList --
 *
 *	Ensures the thread to world mapping is correct by checking for any
 *	worlds that have been created or destroyed since the last time the
 *	debugger ran.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	threadToWorldMap might be updated.
 *          
 *----------------------------------------------------------------------
 */
static void
UserDebugUpdateThreadList(void)
{
   UserDebug_State* dbg = &MY_RUNNING_WORLD->userCartelInfo->debugger;
   World_ID worldList[USER_MAX_ACTIVE_PEERS];
   Bool worldFound[USER_MAX_ACTIVE_PEERS+1];
   int origNumWorlds = dbg->numWorlds;
   int newNumWorlds;
   int i;

   UWLOG(1, "Updating thread list...");

   /*
    * We need to sync up our list of worlds with what actually
    * exists (ie, some worlds may have been created or have died
    * since we last left the debugger).
    */
   memset(worldFound, 0, sizeof(worldFound));
   newNumWorlds = UserThread_GetPeersDebug(worldList);

   /*
    * Add worlds that were created.
    */
   for (i = 0; i < newNumWorlds; i++) {
      int n;
      Bool needToAdd = TRUE;

      for (n = 1; n < origNumWorlds + 1; n++) {
	 if (dbg->threadToWorldMap[n] == worldList[i]) {
	    needToAdd = FALSE;
	    worldFound[n] = TRUE;
	    break;
	 }
      }

      if (needToAdd) {
	 dbg->threadToWorldMap[++dbg->numWorlds] = worldList[i];
      }
   }

   /*
    * Remove worlds that were destroyed.
    */
   for (i = 1; i < origNumWorlds + 1; i++) {
      if (!worldFound[i]) {
	 /*
	  * Swap this world's position with one from the end, updating
	  * targetOther and targetContStep as necessary.
	  */
	 if (dbg->targetOther == dbg->numWorlds) {
	    dbg->targetOther = i;
	 }
	 if (dbg->targetContStep == dbg->numWorlds) {
	    dbg->targetContStep = i;
	 }

	 dbg->threadToWorldMap[i] =
	    dbg->threadToWorldMap[dbg->numWorlds];
	 dbg->threadToWorldMap[dbg->numWorlds] = INVALID_WORLD_ID;
	 dbg->numWorlds--;
      }
   }

   /*    
    * If the current world does not map to gdb's active thread, we
    * need to make it map.  So we just swap whatever world was
    * mapped to gdb's active thread with the current world.  Now the
    * current world maps to gdb's active thread.
    */             
   if (dbg->threadToWorldMap[dbg->targetOther] != dbg->initialWorld) {
      Bool found = FALSE;

      for(i = 1; i < dbg->numWorlds + 1; i++) {
	 if(dbg->threadToWorldMap[i] == dbg->initialWorld) {
	    dbg->threadToWorldMap[i] =
	       dbg->threadToWorldMap[dbg->targetOther];
	    dbg->threadToWorldMap[dbg->targetOther] =
	       dbg->initialWorld;
	    found = TRUE;
	    break;
	 }
      }

      /*
       * If we still haven't found the world, we have a problem.
       */
      ASSERT(found);
   }
}


/*
 *----------------------------------------------------------------------
 *                        
 * UserDebugMainLoop --
 *                  
 *      Handles high-level communication between this debugging stub and
 *      a remote gdb.
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
UserDebugMainLoop(uint32 vector)
{
   UserDebug_State* dbg = &MY_RUNNING_WORLD->userCartelInfo->debugger;
   User_ThreadInfo* uti = MY_RUNNING_WORLD->userThreadInfo;
   Bool firstCommand = TRUE;

   ASSERT(uti->exceptionFrame != NULL);
   dbg->initialWorld = MY_RUNNING_WORLD->worldID;
   dbg->currentUserState = uti->exceptionFrame;

   /*
    * Immediately reply with the error number.  Normally gdb won't even see
    * this, however if the user typed 'continue' in gdb, it will wait until we
    * sent it a message before it does anything.  So this is here to kick gdb
    * back into action in the case we're returning from a continue.
    */
   UserDebugReasonForHalt(vector, dbg->outBuffer);
   UserDebugPutPacket(dbg->outBuffer);

   while (TRUE) {
      memset(dbg->outBuffer, 0, BUFMAX);
      
      // XXX: Drop return value for now.
      UserDebugGetPacket(dbg->inBuffer);
      
      UWLOG(1, "debug: received: \"%s\"", dbg->inBuffer);
      
      switch (dbg->inBuffer[0]) {
         case '?':
            UserDebugReasonForHalt(vector, dbg->outBuffer);
            break;

         case 'g':
	    if (firstCommand) {
	       UserDebugUpdateThreadList();
	    }               
 
            UserDebugReadRegisters(dbg->outBuffer);
            break;

         case 'G':
            UserDebugWriteRegisters(dbg->inBuffer, dbg->outBuffer);
            break;

         case 'P':
            UserDebugSetRegister(dbg->inBuffer, dbg->outBuffer);
            break;

         case 'm':
            UserDebugReadMemory(dbg->inBuffer, dbg->outBuffer);
            break;

         case 'M':
            UserDebugWriteMemory(dbg->inBuffer, dbg->outBuffer);
            break;

         case 's':
         case 'c':
	 case 'D':
            UserDebugStepContinueDetach(dbg->inBuffer);
	    return;

         case 'k':
            UserDebugCartelShutdown(vector);
	    NOT_REACHED();
            break;

	 case 'q' :
	    switch (dbg->inBuffer[1]) {
	       case 'C' :   
		  UserDebugCurrentThread(dbg->inBuffer, dbg->outBuffer);
		  break;    

	       case 'f' :
		  if (strcmp(dbg->inBuffer, "qfThreadInfo") == 0) {
		     UserDebugGetThreadInfo(dbg->inBuffer, dbg->outBuffer);
		  }         
		  break;

	       case 's' :
		  if (strcmp(dbg->inBuffer, "qsThreadInfo") == 0) {
		     strcpy(dbg->outBuffer, "l");
		  }
		  break;

	       case 'T' :
		  if (strncmp(dbg->inBuffer, "qThreadExtraInfo", 16) == 0) {
		     UserDebugGetExtraThreadInfo(dbg->inBuffer, dbg->outBuffer);
		  }
		  break;
	    }
	    break;

	 case 'H' :
	    /*
	     * So in the current version of gdb, if Hc-1 is the first command
	     * given, then gdb has just been started (ie, it's not returning
	     * from a continue or whatever), thus we need to reset our
	     * variables.
	     */
	    if(firstCommand && strcmp(dbg->inBuffer, "Hc-1") == 0) {
	       UserDebugCreateThreadList();
	    }

	    UserDebugSetThread(dbg->inBuffer, dbg->outBuffer);
	    break;

	 case 'T' :
	    UserDebugThreadAlive(dbg->inBuffer, dbg->outBuffer);
	    break;

	 default:
	    UWLOG(0, "debug: unsupported command: %s\n", dbg->inBuffer);
	    break;
      }

      UWLOG(1, "debug: sending: \"%s\"", dbg->outBuffer);

      // XXX: Drop return value for now.
      UserDebugPutPacket(dbg->outBuffer);

      firstCommand = FALSE;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * UserDebug_ReportListeningOn --
 * 
 *	Send a string representing what device and/or address the
 *	debugger is listening on.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	An RPC message may be sent to the COS.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
UserDebug_ReportListeningOn(void)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
   UserDebug_State* dbg = &uci->debugger;
   User_DebuggerInfo di;
   VMK_ReturnStatus status;
   char cnxName[20];
   RPC_Connection cnxID;

   di.type = USER_MSG_BREAK;
   status = Debug_ListeningOn(&dbg->dbgCtx, di.listeningOn, MAX_DESC_LEN);
   ASSERT(status == VMK_OK);

   snprintf(cnxName, sizeof(cnxName), "Status.%d", uci->cartelID);
   status = RPC_Connect(cnxName, &cnxID);
   if (status == VMK_OK) {
      RPC_Token token;
      status = RPC_Send(cnxID, 0, 0, (char *)&di, sizeof(di),
			UTIL_VMKERNEL_BUFFER, &token);

      if (status != VMK_OK) {
         UWLOG(0, "Couldn't send message to vmkload_app, status %#x:%s", status,
	       VMK_ReturnStatusToString(status));
      }

      RPC_Disconnect(cnxID);
   } else {
      UWLOG(0, "Couldn't connect to vmkload_app, status %#x:%s", status,
      	    VMK_ReturnStatusToString(status));
   }

   return status;
}


/*
 *----------------------------------------------------------------------
 *
 * UserDebugCartelShutdown --
 * 
 *	This is a centralized function to kill the current cartel from
 *	the userworld debugger.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	So much death...
 *
 *----------------------------------------------------------------------
 */
static void
UserDebugCartelShutdown(uint32 vector)
{
   UserDebug_State* dbg = &MY_RUNNING_WORLD->userCartelInfo->debugger;
   static const int exitCode = CARTEL_EXIT_SYSERR_BASE + LINUX_SIGTRAP;

   /*
    * First stop the debugger.
    */
   Debug_CnxStop(&dbg->dbgCtx);

   /*
    * Now start up the other threads that were waiting on us (so they can
    * cleanly exit).
    */
   dbg->inDebugger = FALSE;
   CpuSched_Wakeup((uint32)&dbg->lock);

   /*
    * We don't really want to dump core (because we were in the
    * debugger already), so we just set the shutdown state
    * appropriately.
    */
   User_CartelShutdown(exitCode, FALSE, NULL);

   /*
    * Clean termination point from perspective of the kernel because
    * entering the debugger is a clean point.
    */
   World_Exit(VMK_OK);
   NOT_REACHED();
}


/*
 *----------------------------------------------------------------------
 *
 * UserDebug_WaitForDebugger --
 * 
 *	Barrier for threads to sit at until the debugging is completed
 *	(only one thread should be active, it acts as the proxy for
 *	the remote debugger.)
 *
 *	This uninterruptible wait can only be broken out of by the
 *	debugger (or by it going away).
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Waits.
 *
 *----------------------------------------------------------------------
 */
static void
UserDebug_WaitForDebugger(UserDebug_State* dbg)
{
   UWLOG(1, "world waiting for debugger...");
   while (dbg->inDebugger) {
      CpuSched_Wait((uint32)&dbg->lock, CPUSCHED_WAIT_UW_DEBUGGER, NULL);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * UserDebug_Entry --
 * 
 *	Calls UserDebugEntry.  If we can't connect to gdb (ie, UserDebugEntry
 *	returns false), we see if someone else has already broken into the
 *	debugger, and if so, wait for it to finish.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Bool
UserDebug_Entry(uint32 vector)
{
   UserDebug_State* dbg = &MY_USER_CARTEL_INFO->debugger;

   ASSERT(MY_USER_THREAD_INFO->exceptionFrame != NULL);

   if (UserDebugEntry(vector)) {
      return TRUE;
   }

   if (dbg->inDebugger) {
      UserDebug_WaitForDebugger(dbg);
   }

   return FALSE;
}


/*
 *----------------------------------------------------------------------
 *
 * UserDebugEntry --
 * 
 *      Entry point for the UserWorld debugger.  Initializes network
 *	connections.
 *
 * Results:
 *	TRUE if we were able to connect to gdb, FALSE otherwise.
 *
 * Side effects:
 *	Potentially many.  GDB is all-powerful.
 *
 *----------------------------------------------------------------------
 */
static Bool
UserDebugEntry(uint32 vector)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;
#ifdef VMX86_LOG
   User_ThreadInfo* uti = MY_RUNNING_WORLD->userThreadInfo;
#endif
   UserDebug_State* dbg = &uci->debugger;
   Bool master = FALSE;

   /*
    * Only let one through.
    */
   SP_Lock(&dbg->lock);
   if (!dbg->inDebugger) {
      UWLOG(1, "First debugger!");
      dbg->inDebugger = TRUE;
      dbg->everInDebugger = TRUE;
      dbg->wantBreakpoint = FALSE;
      master = TRUE;
   }
   SP_Unlock(&dbg->lock);
   
   if (!master) {
      UWLOG(1, "Already another debugger!");
      return FALSE;
   }

   UWLOG(0, "Preparing to enter user world debugger...");

   // XXX: Sleep for a bit?

   /*
    * So there used to be all this crazy logic to try and make sure all worlds
    * in this cartel are blocked waiting for the debugger to finish.  However,
    * all that effort isn't really necessary.  If a world is currently in a
    * syscall, then its state has already been saved (it's saved upon syscall
    * entry).  If a world is off running usercode, it'll break into the debugger
    * on the next timer interrupt in the worst case.  Thus by the time the user
    * connects, all worlds' state should be saved and consistent.
    */

   UWLOG(0,
         "\nWorld State:\n"
	 "eax: 0x%x\tecx: 0x%x\tedx: 0x%x\tebx: 0x%x\n"
	 "esp: 0x%x\tebp: 0x%x\tesi: 0x%x\tedi: 0x%x\n"
	 "ds:  0x%x\tes:  0x%x\tfs:  0x%x\tgs:  0x%x",
	 uti->exceptionFrame->regs.eax, uti->exceptionFrame->regs.ecx,
	 uti->exceptionFrame->regs.edx, uti->exceptionFrame->regs.ebx,
	 uti->exceptionFrame->frame.esp, uti->exceptionFrame->regs.ebp,
	 uti->exceptionFrame->regs.esi, uti->exceptionFrame->regs.edi,
	 uti->exceptionFrame->regs.ds, uti->exceptionFrame->regs.es,
	 uti->exceptionFrame->regs.fs, uti->exceptionFrame->regs.gs);
   UWLOG(0,
         "\nvector=%d, eflags=0x%x, eip=0x%x, cs=0x%x, error=%d",
	 vector, uti->exceptionFrame->frame.eflags, uti->exceptionFrame->frame.eip,
	 uti->exceptionFrame->frame.cs, uti->exceptionFrame->frame.errorCode);

   if (Debug_CnxStart(&dbg->dbgCtx) != VMK_OK) {
      UWLOG(0, "could not start net debugger!");

      /*
       * Wake up anyone that's waiting on us and return.
       */
      dbg->inDebugger = FALSE;
      CpuSched_Wakeup((uint32)&dbg->lock);

      return FALSE;
   }

   /*
    * Tell the user what ip we're on.
    */
   UserDebug_ReportListeningOn();

   UserDebugMainLoop(vector);

   Debug_CnxStop(&dbg->dbgCtx);

   dbg->inDebugger = FALSE;
   CpuSched_Wakeup((uint32)&dbg->lock);

   return TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * UserDebug_InDebuggerCheck --
 * 
 *      Deschedules the current world until the user world debugger
 *	exits.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	May enter the debugger, may block.
 *
 *----------------------------------------------------------------------
 */
void
UserDebug_InDebuggerCheck(void)
{
   UserDebug_State* dbg = &MY_RUNNING_WORLD->userCartelInfo->debugger;

   if (dbg->wantBreakpoint) {
      if (UserDebugEntry(EXC_BP)) {
         return;
      }
   }
   
   if (dbg->inDebugger) {
      UserDebug_WaitForDebugger(dbg);
   }
}


/*
 * Code for generating an int 90.  Refer to userSig.c:UserSigDispatch for more
 * details on these 'magic' functions.
 */
static const uint8 UserDebugBreakMagic[] = {
   0xcd, 0x90,		// cd 90	int $0x90
};
   
/*
 *----------------------------------------------------------------------
 *
 * UserDebug_InDebuggerCheckFromInterrupt --
 * 
 *	Munges the userworld's stack and registers such that when we iret, it
 *	will int 90 to the BreakIntoDebugger syscall.
 *
 * Results:
 *	TRUE if inDebugger or wantBreakpoint is set, FALSE otherwise or if we
 *	hit a snag while copying out data.
 *
 * Side effects:
 *	Lots of state may be changed so that the BreakIntoDebugger
 *	UWVMK syscall will be called upon return to usermode.
 *
 *----------------------------------------------------------------------
 */
Bool
UserDebug_InDebuggerCheckFromInterrupt(VMKExcFrame* excFrame)
{
   User_CartelInfo* uci = MY_RUNNING_WORLD->userCartelInfo;

   if (uci->debugger.inDebugger || uci->debugger.wantBreakpoint ||
       UserDump_DumpInProgress()) {
      VMK_ReturnStatus status;
      VMKFullUserExcFrame* fullFrame = VMKEXCFRAME_TO_FULLUSERFRAME(excFrame);
      UserVA esp;
      UserVA storedFullFrame;

      esp = (UserVA)fullFrame->frame.esp;

      // Copy out the full frame.
      status = UserSig_CopyChunk(&esp, sizeof(VMKFullUserExcFrame),
				 fullFrame, "user fullframe");
      if (status != VMK_OK) {
	 return FALSE;
      }
      storedFullFrame = esp;

      // Munge registers to do the right thing.
      fullFrame->frame.eip = (Reg32)uci->debugger.debugMagicStubEntry;
      fullFrame->frame.esp = (Reg32)esp;
      fullFrame->regs.eax = (Reg32)UWVMKSYSCALL_SYSCALLNUM_BreakIntoDebugger;
      fullFrame->regs.ebx = (Reg32)storedFullFrame;

      return TRUE;
   }

   return FALSE;
}

/*
 *----------------------------------------------------------------------
 *
 * UserDebug_Init --
 *
 *      Creates the parent proc node entry - uwdebug, under which 
 *      per-cartel proc nodes will be created for debugging userworlds 
 *      from the COS.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
UserDebug_Init(void)
{
   Proc_InitEntry(&procDebugDir);
   Proc_RegisterHidden(&procDebugDir, "uwdebug", TRUE);
}

/*
 *----------------------------------------------------------------------
 *
 * UserDebug_CartelInit --
 * 
 *	Initializes the debugger state.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserDebug_CartelInit(User_CartelInfo* uci)
{
   VMK_ReturnStatus status;
   UserDebug_State * const dbg = &uci->debugger;

   memset(dbg, 0, sizeof *dbg);

   /*
    * Perform all the potentially failing operations first before initializing
    * anything else.
    */
   dbg->inBuffer = User_HeapAlloc(uci, BUFMAX);
   if (dbg->inBuffer == NULL) {
      UWLOG(0, "Failed to allocate memory for debugger input buffer.\n");
      status = VMK_NO_MEMORY;
      goto error;
   }

   dbg->outBuffer = User_HeapAlloc(uci, BUFMAX);
   if (dbg->outBuffer == NULL) {
      UWLOG(0, "Failed to allocate memory for debugger output buffer.\n");
      status = VMK_NO_MEMORY;
      goto error;
   }

   status = UserMem_AddToKText(&uci->mem, UserDebugBreakMagic,
			       sizeof(UserDebugBreakMagic),
			       &dbg->debugMagicStubEntry);

   if (status != VMK_OK) {
      UWLOG(0, "AddToKText failed: %s", UWLOG_ReturnStatusToString(status));
      goto error;
   }

   dbg->inDebugger = FALSE;

   memset(dbg->threadToWorldMap, 0, sizeof(dbg->threadToWorldMap));
   dbg->initialWorld = INVALID_WORLD_ID;
   dbg->initialThread = -1;

   dbg->targetContStep = -2;
   dbg->targetOther = -2;

   SP_InitLock("UserDebug_State", &dbg->lock, SP_RANK_LEAF);
   
   Debug_CnxInit(&dbg->dbgCtx, DEBUG_CNX_PROC, FALSE);

   return VMK_OK;

error:
   ASSERT(status != VMK_OK);

   if (dbg->inBuffer != NULL) {
      User_HeapFree(uci, dbg->inBuffer);
      dbg->inBuffer = NULL;
   }
   if (dbg->outBuffer != NULL) {
      User_HeapFree(uci, dbg->outBuffer);
      dbg->outBuffer = NULL;
   }

   return status;
}



/*
 *----------------------------------------------------------------------
 *
 * UserDebug_CartelCleanup --
 * 
 *	Cleans up the debugger state.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserDebug_CartelCleanup(User_CartelInfo* uci)
{
   UserDebug_State* const dbg = &uci->debugger;

   Debug_CnxStop(&dbg->dbgCtx);
   Debug_CnxCleanup(&dbg->dbgCtx);

   User_HeapFree(uci, dbg->inBuffer);
   User_HeapFree(uci, dbg->outBuffer);
   SP_CleanupLock(&dbg->lock);

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserDebug_WantBreakpoint --
 * 
 *      Enables userworld debugging. If wantBreakpointNow is TRUE, 
 *	sets a flag in the user debugger state such that on the next
 *	interrupt, the specified world's cartel will break into the
 *	debugger.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
UserDebug_WantBreakpoint(VMnix_WantBreakpointArgs* hostArgs)
{
   VMnix_WantBreakpointArgs args;
   World_Handle* world;
      
   CopyFromHost(&args, hostArgs, sizeof(args));

   // Enable Userworld debugger
   Debug_UWDebuggerEnable(TRUE);
    
   if (args.wantBreakpointNow) {
      world = World_Find(args.worldID);
      if (world == NULL) {
         UWLOG(0, "World %d not found", args.worldID);
         return VMK_NOT_FOUND;
      }

      UWLOGFor(1, world, "COS breaking into this world!");
      if (!World_IsUSERWorld(world)) {
         World_Release(world);
         VMLOG(0, args.worldID, "Not userworld");
         return VMK_BAD_PARAM;
      }
      ASSERT(world->userCartelInfo != NULL);

      SysAlert("Asynchronously breaking into UserWorld %d.\n", world->worldID);

      world->userCartelInfo->debugger.wantBreakpoint = TRUE;
   
      World_Release(world);
   }

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserDebug_EverInDebugger --
 * 
 *	Checks if debugger for this userworld has been run before.
 *
 * Results:
 *	Returns TRUE if the debugger for this userworld has been run,
 *	FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Bool
UserDebug_EverInDebugger(void)
{
   UserDebug_State* dbg = &MY_RUNNING_WORLD->userCartelInfo->debugger;
   return dbg->everInDebugger;
}
