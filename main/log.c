/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * log.c --
 *
 *	Implementations of Log, Warning, and Panic, and /proc
 *	interface for setting log levels.
 *      Also includes Log_Event for fast, in-memory-only event monitoring.
 */

#include <stdarg.h>

#include "vm_types.h"
#include "vm_libc.h"
#include "vm_asm.h"
#include "x86.h"
#include "vmkernel.h"
#include "vmnix_if.h"
#include "debug.h"
#include "rpc.h"
#include "sched.h"
#include "util.h"
#include "idt.h"
#include "serial.h"
#include "config.h"
#include "proc.h"
#include "libc.h"
#include "bluescreen.h"
#include "helper.h"
#include "nmi.h"
#include "host.h"
#include "dump.h"
#include "action.h"
#include "timer.h"
#include "netDebug.h"
#include "bh.h"
#include "memalloc.h"
#include "heapsort.h"
#include "vmkevent.h"
#include "parse.h"
#include "log_int.h"
#include "logterm.h"
#include "ansi.h"
#include "statusterm.h"

#define LOGLEVEL_MODULE Log
#include "log.h"

// message prefixes
#define PREFIX_NONE	0
#define PREFIX_LOG	PREFIX_NONE
#define PREFIX_WARNING	1
#define PREFIX_SYSALERT	2
static const char *logPrefix[] = {NULL, "WARNING: ", "ALERT: "};
static const char *logColor[] = {NULL, ANSI_ATTR_SEQ_REVERSE, ANSI_ATTR_SEQ_FORE_RED_BRIGHT};

static SP_SpinLockIRQ logLock;
static uint32 logBHNum;
static uint32 sysAlertBHNum;
static Bool systemInPanic = FALSE;

typedef struct Log_Descriptor {
   char *name;
   int defaultVal;
   Proc_Entry entry;
} Log_Descriptor;

Log_Descriptor logDesc[] = {
#define LOGLEVEL_DEF(name,  dflt) {#name, dflt},
#include "logtable_dist.h"
#define LOGLEVEL_DEF(name,  dflt) {#name, dflt},
#include "logtable.h"
};

#define NUM_LOG_DESC  (sizeof(logDesc)/sizeof(Log_Descriptor))

static Proc_Entry logDir;

/* Current log level for each module */
int logLevelPtr[NUM_LOG_DESC];

static void LogWarning(const char *fmt, va_list args, int logType);
static int LogRead(Proc_Entry *entry, char *buffer, int *len);
static int LogWrite(Proc_Entry *entry, char *buffer, int *len);
static int LogFormatString(char *buffer, uint32 bufLen, const char *fmt, va_list args,
                           Bool addPrefix, int logType);
static void LogEventInit(void);
static void LogEventEarlyInit(void);

/* Max allowed number of characters in a single Log() call. */
#define MAX_LOG_SIZE		VMK_LOG_ENTRY_SIZE

char logBuffer[VMK_LOG_BUFFER_SIZE];
uint32 firstLogChar;
/* Offset in logBuffer where next log entry will go. */
uint32 nextLogChar;

extern Bool vmkernelLoaded;
#define SYSALERT_BUFFERS 10
#define SYSALERT_BUFFER_LENGTH 81  /* number of charters on vga screen + 1 for '\0' */
struct {
   char msg[SYSALERT_BUFFER_LENGTH];
   Bool alertNotPosted;
} sysAlertBuf[SYSALERT_BUFFERS];
static Atomic_uint32 curSysAlertBuf;

/* Equivalent to nextLogChar in an extrapolated flat buffer */
static uint32 logRunningPos = 0;


/*
 *----------------------------------------------------------------------
 *
 * Log_EarlyInit --
 *
 *      Set the default levels of the various log modules early in the
 *	vmkernel boot.
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
Log_EarlyInit(VMnix_ConfigOptions *vmnixOptions, VMnix_SharedData *sharedData,
              VMnix_SharedData *hostSharedData)
{
   int i;

   SP_InitLockIRQ("logLck", &logLock, SP_RANK_LOG);

   for (i = 0; i < NUM_LOG_DESC; i++) {
      logLevelPtr[i] = logDesc[i].defaultVal;
   }
   memset(logBuffer, 0, VMK_LOG_BUFFER_SIZE);

   /*
    * Explicitly copy the shared data now so that if the vmkernel fails to
    * load, the log buffer can be dumped by DumpVMKLogBuffer() in module.c 
    */
#define LOG_SPECIAL_SHARED_DATA_ADD(_field, _size, _var) \
   SHARED_DATA_ADD(sharedData->_field, _size, _var); \
   CopyToHost(&(hostSharedData->_field), &(sharedData->_field), sizeof(_size))

   LOG_SPECIAL_SHARED_DATA_ADD(logBuffer, char *, logBuffer);
   LOG_SPECIAL_SHARED_DATA_ADD(firstLogChar, int *, &firstLogChar);
   LOG_SPECIAL_SHARED_DATA_ADD(nextLogChar, int *, &nextLogChar);
   LOG_SPECIAL_SHARED_DATA_ADD(fileLoggingEnabled, int *, &CONFIG_OPTION(LOG_TO_FILE));

   sharedData->logBufferLength = VMK_LOG_BUFFER_SIZE;
   CopyToHost(&hostSharedData->logBufferLength, &sharedData->logBufferLength, 
              sizeof(sharedData->logBufferLength));
#undef LOG_SPECIAL_SHARED_DATA_ADD

   LogEventEarlyInit();
}

/*
 *----------------------------------------------------------------------
 *
 * LogInterruptVmnixBH --
 *
 *      Bottom-half handler to generate VMnix interrupt safely from
 *	Log module.  Need to run as bottom-half in order to prevent
 *	deadlock involving the CpuSched and Host PICPending locks.
 *
 * Results:
 *      None. 
 *
 * Side effects:
 *      Interrupts VMnix.
 *
 *----------------------------------------------------------------------
 */
static void
LogInterruptVmnixBH(void *ignore)
{
   Host_InterruptVMnix(VMNIX_LOG_DATA_PENDING);
}

/*
 *----------------------------------------------------------------------
 *
 * LogSysAlertBH --
 *
 *      Posts a vmkevent message to serverd for all new alerts in the
 *      sysAlertBuf.  If there are more outstanding alerts than 
 *      entries in sysAlertBuf, the messages posted could be garbled.
 *
 * Results: 
 *      None.
 *
 * Side effects:
 *      Causes serverd to log a message.
 *
 *----------------------------------------------------------------------
 */
static void
LogSysAlertBH(void *unused)
{
   int i;

   if (Panic_IsSystemInPanic() || CONFIG_OPTION(MINIMAL_PANIC)) {
      /* 
       * No point in posting a message to serverd if we are already
       * panicking -- and it might cause further trouble.
       */
      return;
   }

   for (i = 0; i < SYSALERT_BUFFERS; i++) {
      if (sysAlertBuf[i].alertNotPosted) {
         sysAlertBuf[i].alertNotPosted = FALSE;
         VmkEvent_PostAlert(VMK_ALERT_SYSALERT, "%s", sysAlertBuf[i].msg);
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * Log_Init --
 *
 *      Initialization routine for log subsystem.
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
Log_Init(void)
{
   int i;

   LogEventInit(); // must be before proc_register

   // register handler to generate vmnix interrupts
   logBHNum = BH_Register(LogInterruptVmnixBH, NULL);

   // register handler to send sysalerts to serverd 
   sysAlertBHNum = BH_Register(LogSysAlertBH, NULL);

   if (vmx86_log) {
      logDir.read = NULL;
      logDir.write = NULL;
      logDir.parent = NULL;
      logDir.private = 0;

      Proc_Register(&logDir, "loglevels", TRUE);

      for (i = 0; i < NUM_LOG_DESC; i++) {
         logDesc[i].entry.read = LogRead;
         logDesc[i].entry.write = LogWrite;
         logDesc[i].entry.parent = &logDir;
         logDesc[i].entry.canBlock = FALSE;
         logDesc[i].entry.private = (void *)i;
         Proc_Register(&logDesc[i].entry, logDesc[i].name, FALSE);
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * LogRead --
 *
 *      Callback for read operation on log proc entry.
 *
 * Results: 
 *      VMK_OK, value of log option in buffer
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
LogRead(Proc_Entry *entry,
        char       *buffer,
        int        *len)
{
   int indx = (int)entry->private;
   // Config_Descriptor *desc = &configDesc[indx];  
   *len = 0;
   Proc_Printf(buffer, len, "%d\n", logLevelPtr[indx]);
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * LogWrite --
 *
 *      Callback for write operation on log proc entry.
 *
 * Results: 
 *      VMK_OK, VMK_BAD_PARAM
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
LogWrite(Proc_Entry *entry,
         char       *buffer,
         int        *len)
{
   int indx = (int)entry->private;
   Log_Descriptor *desc = &logDesc[indx];  
   unsigned val;
   char *endp;
   
   if (!strncmp(buffer, "default", 7)) {
      val = desc->defaultVal;
      endp = buffer + 7;
   } else {
      val = simple_strtoul(buffer, &endp, 0);
   }

   Log("Setting loglevel for module '%s' to %d", desc->name, val);

   while (endp < (buffer+*len)) {
      if ((*endp != '\n') && (*endp != ' ')) {
         return VMK_BAD_PARAM;
      }
      endp++;
   }
   
   logLevelPtr[indx] = (int) val;

   return VMK_OK;
}


/*
 * Copy string 'str' into the logBuffer at the current writing point
 * (nextLogChar).  Wrap around in the logBuffer as necessary.
 */
static void
BufferString(const char *str)
{
   while (*str != 0) {
      logBuffer[nextLogChar++] = *str++;
      if (nextLogChar == VMK_LOG_BUFFER_SIZE) {
	 nextLogChar = 0;
      }
      logRunningPos++;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * LogPutLenString --
 *
 *      Write the string stored in logBuffer to debugger or serial log.
 *      Guards against writing to the serial port if currently using 
 *      the serial debugger.
 *
 *      If locked is true, then we can't call Debug_PutString --
 *      it potentially calls into the networking code.
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
LogPutLenString(int start, int end, Bool serial)
{
   Bool usingSerialDebugger = Debug_SerialDebugging() && Debug_InDebugger();

   if (!usingSerialDebugger && serial &&
       CONFIG_OPTION(LOG_TO_SERIAL)) {
      Serial_PutLenString(&logBuffer[start], end-start);
   } /*else if (usingSerialDebugger && !serial) {
      // XXX: This code doesn't do anything anyway, and only serves to break
      // serial debugging in some cases. -kit
      Debug_PutLenString(&logBuffer[start], end-start);
   }*/
}

/*
 *----------------------------------------------------------------------
 *
 * LogPutString --
 *
 *      Write the string stored in logBuffer to the log.  Properly
 *      handles logs that wrap around the buffer.
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
LogPutString(int startOffset, int savedNextLogChar, Bool serial)
{
   if (startOffset < savedNextLogChar) {
      LogPutLenString(startOffset, savedNextLogChar, serial);
   } else if (startOffset > savedNextLogChar) {
      LogPutLenString(startOffset, VMK_LOG_BUFFER_SIZE, serial);
      LogPutLenString(0, savedNextLogChar, serial);
   }
   // if ==, do nothing.
}


void
Panic(const char *fmt, ...)
{
   va_list args;
   uint32 *p = (uint32 *)&fmt;   
   char buffer[MAX_LOG_SIZE];

   /*
    * This first case should only be executed when we're first coming in from a
    * direct Panic call or ASSERT failure, without already being in panic.  If
    * we were already in panic (perhaps because we took an exception), then we
    * go to the case below.
    */
   if (!Panic_IsSystemInPanic()) {
      int i;
      VMKFullExcFrame excFrame;

      /*
       * Marks the cpu in panic and disables preemption.
       */
      Panic_MarkCPUInPanic();
      CLEAR_INTERRUPTS();
      NMI_Disable();

      if ((PRDA_GetRunningWorldSafe() != NULL) && 
          World_IsVMMWorld(PRDA_GetRunningWorldSafe())) {
         World_ResetDefaultDT();
      }

      va_start(args, fmt);
      i = vsnprintf(buffer, sizeof buffer, fmt, args);
      va_end(args);

      Serial_PutString(buffer);

      WriteLEDs(5);

      Util_Backtrace(*(p - 1), *(p - 2), _Log, TRUE);

      memset(&excFrame, 0, sizeof(excFrame));
      excFrame.frame.eip = *(p - 1);
      excFrame.regs.ebp = *(p - 2);

      BlueScreen_Post(buffer, &excFrame);

      if (Debug_IsInitialized()) {
	 snprintf(buffer, sizeof buffer, "Waiting for debugger... (world %u)\n",
		  PRDA_GetRunningWorldIDSafe());
	 BlueScreen_Append(buffer);
	 Debug_Break();
      }
   } else if (!Debug_IsInitialized()) {
      WriteLEDs(6);
   } else if (Panic_IsCPUInPanic()) {
      /* Print 2nd panic header */
      snprintf(buffer, sizeof buffer,
               "Second panic on same CPU (world %u): eip=%p\n",
               PRDA_GetRunningWorldIDSafe(), __builtin_return_address(0));
      BlueScreen_Append(buffer);

      /* Print initial panic message (contains \n) */
      va_start(args, fmt);
      vsnprintf(buffer, sizeof buffer, fmt, args);
      va_end(args);
      BlueScreen_Append(buffer);
      
      /* Print standard waiting for debugger message: */
      BlueScreen_Append("Waiting for debugger...\n");
      Debug_Break();
   } else {
      Panic_MarkCPUInPanic();
      snprintf(buffer, sizeof buffer, "Panic from another CPU (world %u): eip=%p\n",
               PRDA_GetRunningWorldIDSafe(), __builtin_return_address(0));
      BlueScreen_Append(buffer);

      /* Print the triggering panic message */
      va_start(args, fmt);
      vsnprintf(buffer, sizeof buffer, fmt, args);
      va_end(args);
      BlueScreen_Append(buffer);

      /* Backtrace this stack too */
      Util_Backtrace(*(p - 1), *(p - 2), _Log, TRUE);

      /*
       * Save our registers back to the world handle so that if the debugger is
       * running it can see our current state.
       */
      if (PRDA_GetRunningWorldSafe() != NULL) {
         World_Switch(MY_RUNNING_WORLD, MY_RUNNING_WORLD);
      }
   }


   CLEAR_INTERRUPTS(); 
   __asm__("hlt" ::);

   while (1) {
      /* make compiler happy */
      ;
   }
}


void 
_Log(const char *fmt, ...)
{
   va_list args;
   va_start(args, fmt);   

   LogWarning(fmt, args, PREFIX_LOG);

   va_end(args);
}

void
_Warning(const char *fmt, ...)
{
   va_list args;
   va_start(args, fmt);   

   LogWarning(fmt, args, PREFIX_WARNING);

   va_end(args);
}


/*
 *----------------------------------------------------------------------
 *
 * SysAlertVarArgs --
 *
 *      Log a SysAlert to the vmkernel log, as well as the special
 *      SysAlert buffer.  If more than SYSALERT_BUFFERS sysalerts
 *      happen simultaneously we might get garbled text in the
 *      SysAlert buffer (not a big deal).
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
SysAlertVarArgs(const char *fmt, va_list args)
{
   uint32 bufNum = Atomic_FetchAndAdd(&curSysAlertBuf, 1) % SYSALERT_BUFFERS;

   /*
    * first do a serial printf because LogWarning is too complicated and
    * could cause assert fails/exceptions if bad stuff has already
    * happened, which is likely because someone is calling SysAlert.
    */
   Serial_PrintfVarArgs(fmt, args);
   LogFormatString(sysAlertBuf[bufNum].msg, SYSALERT_BUFFER_LENGTH, fmt, 
                   args, TRUE, PREFIX_NONE);
   StatusTerm_PrintAlert(sysAlertBuf[bufNum].msg);
   LogWarning(fmt, args, PREFIX_SYSALERT);
   sysAlertBuf[bufNum].alertNotPosted = TRUE;
   if (vmkernelLoaded) {
      BH_SetGlobal(sysAlertBHNum);
   }
}

void
_SysAlert(const char *fmt, ...)
{
   va_list args;
   va_start(args, fmt);   

   SysAlertVarArgs(fmt, args);

   va_end(args);
}


/*
 *----------------------------------------------------------------------
 *
 * LogFormatString --
 *      
 *      Write a log message into buffer, applying the normal
 *      log prefixes.
 *
 * Results: 
 *      The number of characters written to buffer.  Buffer
 *      will always be null terminated.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
LogFormatString(char *buffer, uint32 bufLen, const char *fmt, va_list args,
                Bool addPrefix, int logType) 
{
   int len = 0;

   if (addPrefix) {
      
      if (logColor[logType] != NULL) {
	 len += snprintf(buffer + len, bufLen - len, "%s", logColor[logType]);
	 len = MIN(len, bufLen);
      }

      len += Util_FormatTimestamp(buffer + len, bufLen - len);
      len += snprintf(buffer + len, bufLen - len, " cpu%d",
                      PRDA_GetPCPUNumSafe());
      len = MIN(len, bufLen);

      if (CONFIG_OPTION(LOG_WLD_PREFIX)) {
         len += snprintf(buffer + len, bufLen - len, ":%d)",
                         PRDA_GetRunningWorldIDSafe());
         len = MIN(len, bufLen);
      } else {
         len += snprintf(buffer + len, bufLen - len, ")");
         len = MIN(len, bufLen);
      }

      if (logType != PREFIX_NONE) {
         len += snprintf(buffer + len, bufLen - len, "%s", logPrefix[logType]);
         len = MIN(len, bufLen);
      }
   }

   len += vsnprintf(buffer + len, bufLen - len, fmt, args);
   len = MIN(len, bufLen);

   if (addPrefix && (logColor[logType] != NULL)) {
      // suffix needs to be placed before ending newline
      while (len && buffer[len-1] == '\n') {
         len--;
      }
      if (bufLen - len < strlen(ANSI_ATTR_SEQ_RESET) + 2) {
         len = bufLen - strlen(ANSI_ATTR_SEQ_RESET) - 2;
      }
      len += snprintf(buffer + len, bufLen - len, "%s\n", ANSI_ATTR_SEQ_RESET);
      len = MIN(len, bufLen);
   }

   return len;
}

static void
LogWarning(const char *fmt, va_list args, int logType)
{
   char buffer[MAX_LOG_SIZE];
   SP_IRQL prevIRQL = -1;
   Bool inNMIandLocked;
   Bool locked = FALSE;
   int len = 0;
   int startOffset;
   int savedNextLogChar = -1;
   int fmtLen = strlen(fmt);
   Bool addPrefix;


   addPrefix = (fmtLen > 1) && (fmt[fmtLen - 1] == '\n');
   len = LogFormatString(buffer, sizeof buffer, fmt, args, addPrefix, logType);

   inNMIandLocked = (vmkernelLoaded && myPRDA.inNMI && SP_IsLockedIRQ(&logLock));
   if (!inNMIandLocked && !Debug_InDebugger()) {
      /*
       * Acquiring loglock in NMI handler may cause lock rank violation if
       * the CPU was already holding the lock stats lock.  So, use trylock
       * in NMI handlers.
       */
      if (vmkernelLoaded && myPRDA.inNMI) {
         prevIRQL = SP_TryLockIRQ(&logLock, SP_IRQL_KERNEL, &locked);
      } else {
         prevIRQL = SP_LockIRQ(&logLock, SP_IRQL_KERNEL);
         locked = TRUE;
      }
   }

   if (locked) {
      startOffset = nextLogChar;
      BufferString(buffer);

      savedNextLogChar = nextLogChar;   

      // kick vmnix if logging to file
      if (CONFIG_OPTION(LOG_TO_FILE)) {
         if (vmkernelLoaded && !myPRDA.inNMI && locked) {
            BH_SetLocalPCPU(logBHNum);
         }
      }

      LogPutString(startOffset, savedNextLogChar, TRUE);
      LogTerm_CatchUp();

      SP_UnlockIRQ(&logLock, prevIRQL);

      if (!CONFIG_OPTION(MINIMAL_PANIC)) {
         LogPutString(startOffset, savedNextLogChar, FALSE);

         if (savedNextLogChar != -1) {
            NetLog_Queue(savedNextLogChar, len + 1);   
         }
      }
   } else if (!Debug_SerialDebugging() || !Debug_InDebugger()) {
      Serial_Printf("%s", buffer);
   }
}


/*
 * Send more contiguous log entries from the log buffer using
 * NetLog_Send() and starting from prevNextLogChar.  If prevNextLogChar is
 * -1, start from right after nextLogChar or from the beginning of the
 * log buffer.  Send at most maxSize characters.
 */
void
Log_SendMore(int prevNextLogChar, int maxSize)
{
   int offset;
   int length;

   SP_IRQL prevIRQL = SP_LockIRQ(&logLock, SP_IRQL_KERNEL);

   if (prevNextLogChar == -1) {
      if (logBuffer[nextLogChar] != 0) {
	 offset = nextLogChar;
	 length = VMK_LOG_BUFFER_SIZE - nextLogChar;
      } else {
	 offset = 0;
	 length = nextLogChar;
      }
   } else {
      offset = prevNextLogChar;
      if (prevNextLogChar < nextLogChar) {
	 length = nextLogChar - prevNextLogChar;
      } else if (prevNextLogChar > nextLogChar) {
	 length = VMK_LOG_BUFFER_SIZE - prevNextLogChar;
      } else {
	 length = 0;
      }
   }
   if (length > maxSize) {
      length = maxSize;
   }         

   SP_UnlockIRQ(&logLock, prevIRQL);   

   if (length > 0) {
      NetLog_Send((offset + length) == VMK_LOG_BUFFER_SIZE ? 0 : offset + length,
                  logBuffer + offset, length);
   }
}

#ifdef	VMX86_ENABLE_EVENTLOG

#define	EVENT_LOG_MAX	(256)
#define	EVENT_LOG_MASK	(EVENT_LOG_MAX - 1)
#define PROC_NAME_SIZE  (256)

typedef struct {
   TSCCycles timeStamp;
   PCPU pcpu;
   World_ID runningWorldID;
   const char *eventName;
   int64 eventData;
} LogEventEntry;

typedef struct {
   SP_SpinLockIRQ bufLock;
   LogEventEntry log[EVENT_LOG_MAX];
   uint32 next;
   Proc_Entry proc;
} LogEventBuffer;

// event log
static int LogEventBufProcRead(Proc_Entry *entry, char *buf, int *len);

static Proc_Entry logGlobalEventProc;
static Proc_Entry eventLogTypesProcEnt;

static LogEventBuffer logEventBufPcpu[MAX_PCPUS];
static Bool logEventBufDrain;
Bool eventLogActiveTypes[EVENTLOG_MAX_TYPE];

const char *eventLogTypeNames[] = {
   "cpusched",
   "cpusched-cosched",
   "cpusched-halting",
   "timer",
   "testworlds",
   "vmkstats",
   "other",
   "INVALID"
};

/*
 *----------------------------------------------------------------------
 *
 * Log_EventInt --
 *
 *      Add event identified by "eventName" and "eventData" to the
 *	event log buffer.   The "eventName" string is not copied,
 *	so the caller must preserve its contents (e.g. by using
 *	a compile-time constant).
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      Modifies statistics event log.
 *
 *----------------------------------------------------------------------
 */
void Log_EventInt(const char *eventName, int64 eventData, EventLogType eventType)
{
   LogEventEntry *pcpuEntry;
   LogEventBuffer *buf;
   World_Handle *current;
   SP_IRQL prevIRQL;
   PCPU myPCPU;
   TSCCycles now;

   // avoid updates while reading
   if (logEventBufDrain) {
      return;
   }

   // current context
   current = MY_RUNNING_WORLD;
   now = RDTSC();
   myPCPU = MY_PCPU;

   // acquire per-pcpu indicies under lock
   prevIRQL = SP_LockIRQ(&logEventBufPcpu[myPCPU].bufLock, SP_IRQL_KERNEL);
   buf = &logEventBufPcpu[myPCPU];
   buf->next = (buf->next + 1) & EVENT_LOG_MASK;
   pcpuEntry = &buf->log[buf->next];
   SP_UnlockIRQ(&logEventBufPcpu[myPCPU].bufLock, prevIRQL);

   // fill in data
   pcpuEntry->timeStamp = now;
   pcpuEntry->pcpu = myPCPU;
   pcpuEntry->runningWorldID = current ? current->worldID : INVALID_WORLD_ID;
   pcpuEntry->eventName = eventName;
   pcpuEntry->eventData = eventData;
}

static void 
LogEventPrintEvent(LogEventEntry *e, uint64* timeStamp, char* buf, int* len) {
      uint64 delta = 0;

      // Compute elapsed time betweeen consecutive events
      if (*timeStamp != 0) {
         delta = e->timeStamp - *timeStamp;
      }
      *timeStamp = e->timeStamp;

      // format log entry
      Proc_Printf(buf, len,
                  "%-14s %18Ld "
                  "%3d %3d %16Ld %10Ld\n",
                  (e->eventName == NULL) ? "" : e->eventName,
                  e->eventData,
                  e->pcpu,
                  e->runningWorldID,
                  e->timeStamp,
                  delta);
}

/*
 *----------------------------------------------------------------------
 *
 * LogEventBufProcRead --
 *
 *      Format current event buffer contents.
 *
 * Results:
 *      Writes formatted event log into "buf".
 *      Sets "len" to the number of bytes written.
 *	Returns VMK_OK.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
LogEventBufProcRead(Proc_Entry *entry,
                         char       *buf,
                         int        *len)
{
   LogEventBuffer *eventBuf = (LogEventBuffer *) entry->private;
   uint64 timeStamp;
   int i;

   // initialize
   *len = 0;
   timeStamp = 0;

   // log header
   Proc_Printf(buf, len,
               "event                        data "
               "cpu run        timestamp      delta\n");

   // avoid updates
   logEventBufDrain = TRUE;

   // log events
   for (i = 0; i < EVENT_LOG_MAX; i++) {
      LogEventEntry *e = &eventBuf->log[i];
      LogEventPrintEvent(e, &timeStamp, buf, len);
   }

   // allow updates
   logEventBufDrain = FALSE;

   // everything OK
   return(VMK_OK);
}

/*
 *-----------------------------------------------------------------------------
 *
 * LogEventSorter --
 *
 *  Simple comparator function for heapsort. Compares on timestamp field.
 *
 * Results:
 *     Returns 1, 0, or -1 depending on whether data1 is greater than, equal to
 *     or less than data2, respectively
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static int LogEventSorter(const void* data1, const void* data2)
{
   LogEventEntry *ev1 = (LogEventEntry*) data1;
   LogEventEntry *ev2 = (LogEventEntry*) data2;

   if (ev1->timeStamp < ev2->timeStamp) {
      return -1;
   } else if (ev1->timeStamp > ev2->timeStamp) {
      return 1;
   } else {
      return 0;
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * LogGlobalEventBufProcRead --
 *
 *  Proc read handler to display aggregate event log (containing info from 
 *  all cpus).
 *
 * Results:
 *     Returns VMK_OK or VMK_NO_MEMORY
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static int
LogGlobalEventBufProcRead(Proc_Entry *entry,
                         char       *buf,
                         int        *len)
{
   int i, totalEntries;
   uint64 timeStamp;
   LogEventEntry *eventEntries;
   LogEventEntry temp;
   SP_IRQL prevIRQL;

   *len = 0;
   totalEntries = numPCPUs * EVENT_LOG_MAX;
   timeStamp = 0;
   eventEntries = Mem_Alloc(sizeof(LogEventEntry) * totalEntries);
   
   if (eventEntries == NULL) {
      return (VMK_NO_MEMORY);
   }

   memset(eventEntries, 0, sizeof(LogEventEntry) * totalEntries);
   logEventBufDrain = TRUE;

   Proc_Printf(buf, len,
               "event                      data "
               "cpu run        timestamp      delta\n");

   // copy the per-cpu buffers into a single buffer  
   for (i=0; i < numPCPUs; i++) {
      prevIRQL = SP_LockIRQ(&logEventBufPcpu[i].bufLock, SP_IRQL_KERNEL);
      memcpy(&eventEntries[i * EVENT_LOG_MAX], &logEventBufPcpu[i].log, EVENT_LOG_MAX * sizeof(LogEventEntry));
      SP_UnlockIRQ(&logEventBufPcpu[i].bufLock, prevIRQL);
   }

   heapsort(eventEntries, totalEntries, sizeof(LogEventEntry), LogEventSorter, &temp);

   for (i=totalEntries - EVENT_LOG_MAX; i < totalEntries; i++) {
      LogEventEntry *e = &eventEntries[i];
      uint64 delta;

      // compute elapsed time betweeen consecutive events
      if (timeStamp == 0) {
         delta = 0;
      } else {
         delta = e->timeStamp - timeStamp;
      }
      timeStamp = e->timeStamp;

      // format log entry
      Proc_Printf(buf, len,
                  "%-14s %18Ld "
                  "%3d %3d %16Ld %10Ld\n",
                  (e->eventName == NULL) ? "" : e->eventName,
                  e->eventData,
                  e->pcpu,
                  e->runningWorldID,
                  e->timeStamp,
                  delta);

   }
   Mem_Free(eventEntries);

   logEventBufDrain = FALSE;

   return (VMK_OK);
}

/*
 *-----------------------------------------------------------------------------
 *
 * LogGlobalEventBufProcWrite --
 *
 *     Proc write handler for /proc/vmware/eventlog
 *     Allows users to reset buffers with "reset" command
 *
 * Results:
 *     Returns VMK_OK on success, else VMK_BAD_PARAM
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static int 
LogGlobalEventBufProcWrite(Proc_Entry *entry, char *buffer, int *len)
{
   PCPU p;
   if (!strncmp(buffer, "reset", 5)) {
      // reset it
      for (p = 0; p < numPCPUs; p++) {
         LogEventBuffer *buf = &logEventBufPcpu[p];
         SP_IRQL prevIRQL;

         prevIRQL = SP_LockIRQ(&buf->bufLock, SP_IRQL_KERNEL);
         memset(buf->log, 0, sizeof(LogEventEntry) * EVENT_LOG_MAX);
         buf->next = 0;
         SP_UnlockIRQ(&buf->bufLock, prevIRQL);
      }

      Log("reset eventlog data");
      return (VMK_OK);
   } else {
      Warning("command not understood");
      return (VMK_BAD_PARAM);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * Log_EventLogSetTypeActive --
 *
 *     Depending on the "activate" parameter, this will enable or disable
 *     logging for the specified event type "eventType"
 *
 * Results:
 *     None
 *
 * Side effects:
 *     Logging will begin or end for the specified event type
 *
 *-----------------------------------------------------------------------------
 */
void
Log_EventLogSetTypeActive(EventLogType eventType, Bool activate)
{
   ASSERT(eventType < EVENTLOG_MAX_TYPE);
   if (eventType < EVENTLOG_MAX_TYPE) {
      LOG(0, "set type active: %s", eventLogTypeNames[eventType]);
      eventLogActiveTypes[eventType] = activate;
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * LogEventLogTypesProcWrite --
 *
 *     Proc write handler for /proc/vmware/eventlogtypes
 *     Handles "start" and "stop" commands
 *
 * Results:
 *     Returns VMK_OK on success, else VMK_BAD_PARAM
 *
 * Side effects:
 *     May start or stop logging for a particular event type
 *
 *-----------------------------------------------------------------------------
 */
static int
LogEventLogTypesProcWrite(Proc_Entry *entry, char *buffer, int *len)
{ 
   int argc, i, j;
   Bool activate;
   char *argv[EVENTLOG_MAX_TYPE + 1];

   argc = Parse_Args(buffer, argv, EVENTLOG_MAX_TYPE + 1);

   if (argc < 2) {
      Log("not enough arguments");
      return (VMK_BAD_PARAM);
   }

   if (!strcmp(argv[0], "start")) {
      activate = TRUE;
   } else if (!strcmp(argv[0], "stop")) {
      activate = FALSE;
   } else {
      Log("command %s not understood", argv[0]);
      return (VMK_BAD_PARAM);
   }

   for (i=1; i < argc; i++) {
      char *name = argv[i];
      
      for (j=0; j < EVENTLOG_MAX_TYPE; j++) {
         if (!strcmp(name, eventLogTypeNames[j])) {
            LOG(1, "set %s activation to %s",
                name, activate ? "TRUE" : "FALSE");
            Log_EventLogSetTypeActive(j, activate);
            break;
         }
      }
      if (j == EVENTLOG_MAX_TYPE) {
         Log("eventlog type %s not found", name);
      }
   }
  
   return(VMK_OK);
}


/*
 *-----------------------------------------------------------------------------
 *
 * LogEventLogTypesProcRead --
 *
 *     Read handler for /proc/vmware/eventlogtypes
 *     Displays all known types and their current states (active or not)
 *
 * Results:
 *     Returns VMK_OK
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
static int
LogEventLogTypesProcRead(Proc_Entry *entry, char *buffer, int *len)
{
   int i;
   *len = 0;

   for (i=0; i < EVENTLOG_MAX_TYPE; i++) {
      Proc_Printf(buffer, len, "%-24s  %3s\n",
                  eventLogTypeNames[i],
                  eventLogActiveTypes[i] ?
                  "ON" : "OFF");
   }
   return(VMK_OK);
}

#endif // VMX86_ENABLE_EVENTLOG

/*
 *----------------------------------------------------------------------
 *
 * LogEventEarlyInit --
 *
 *	Early initialization for event logging.
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
LogEventEarlyInit(void)
{
#ifdef	VMX86_ENABLE_EVENTLOG
   int i;
   
   // initialize event log state
   for (i = 0; i < MAX_PCPUS; i++) {
      char nameBuf[128];
      snprintf(nameBuf, sizeof nameBuf, "eventlog-%d", i);
      memset(&logEventBufPcpu[i], 0, sizeof(LogEventBuffer));
      SP_InitLockIRQ(nameBuf, &logEventBufPcpu[i].bufLock, SP_RANK_LOG_EVENT);
   }
   logEventBufDrain = FALSE;   
#endif
}

/*
 *----------------------------------------------------------------------
 *
 * LogEventInit --
 *
 *	Initialization for event logging.
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
LogEventInit(void)
{
#ifdef	VMX86_ENABLE_EVENTLOG
   LogEventBuffer *buf;
   int i;

   // setup global proc node
   Proc_InitEntry(&logGlobalEventProc);
   logGlobalEventProc.parent = NULL;
   logGlobalEventProc.read = LogGlobalEventBufProcRead;
   logGlobalEventProc.write = LogGlobalEventBufProcWrite;
   logGlobalEventProc.private = NULL;
   Proc_RegisterHidden(&logGlobalEventProc, "eventlog", FALSE);

   // register per-pcpu "eventlog.<pcpu>" procfs entries
   for (i = 0; i < numPCPUs; i++) {
      char nameBuf[PROC_NAME_SIZE];
      snprintf(nameBuf, sizeof nameBuf, "eventlog.%d", i);
      buf = &logEventBufPcpu[i];
      Proc_InitEntry(&buf->proc);
      buf->proc.parent = NULL;
      buf->proc.read = LogEventBufProcRead;
      buf->proc.private = buf;
      Proc_RegisterHidden(&buf->proc, nameBuf, FALSE);
   }

   // register "types" proc node
   Log("init eventlogtype proc entry");
   Proc_InitEntry(&eventLogTypesProcEnt);
   eventLogTypesProcEnt.parent = NULL;
   eventLogTypesProcEnt.read = LogEventLogTypesProcRead;
   eventLogTypesProcEnt.write = LogEventLogTypesProcWrite;
   Proc_RegisterHidden(&eventLogTypesProcEnt, "eventlogtypes", FALSE);
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * Panic_MarkCPUInPanic
 *
 *     Marking this CPU and the whole system inPanic.  This is used to stop
 *     other panics on the same CPU and also to quiesce other CPUs.
 *
 * Results:
 *     None.
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
void
Panic_MarkCPUInPanic(void)
{
   LogTerm_OffScreen(); // Avoid recursion in case logterm is causing Panic

   systemInPanic = TRUE;
   if (!PRDA_IsInitialized()) {
      return;
   }

   if (!Panic_IsCPUInPanic()) {
      myPRDA.inPanic = TRUE;
      myPRDA.worldInPanic = MY_RUNNING_WORLD;

      /*
       * Automatically disable preemption upon Panic.  We do this because all
       * code run after this will be kernel code that expects that preemption is
       * disabled.  And if we enter panic/bluescreen code through idt.c, there's
       * a good chance that preemption has not yet been disabled.
       */
      CpuSched_DisablePreemption();
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * Panic_IsCPUInPanic
 *
 *     Is the current CPU in panic?
 *
 * Results:
 *     TRUE if CPU is in panic mode, FALSE otherwise
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
Bool
Panic_IsCPUInPanic(void)
{
   if (!PRDA_IsInitialized()) {
      return FALSE;
   }

   return myPRDA.inPanic;
}

/*
 *-----------------------------------------------------------------------------
 *
 * Panic_IsSystemInPanic
 *
 *     Has the vmkernel panic'ed?
 *
 * Results:
 *     TRUE if any CPU has panic'ed, FALSE otherwise
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
Bool
Panic_IsSystemInPanic(void)
{
   return systemInPanic;
}

/*
 *-----------------------------------------------------------------------------
 *
 * Log_VMMLog
 *
 *     Handle serial logging requests from the vmm.  This needs needs to
 *     behave similar to Log() in lib/user/log.c -- otherwise messages
 *     will get garbled [it mostly works as is].  All the cruft below
 *     tries to deal with this difference as gracefully as possible.
 *
 * Results:
 *     None.
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
VMKERNEL_ENTRY
Log_VMMLog(DECLARE_ARGS(VMK_VMM_SERIAL_LOGGING)) 
{
   PROCESS_1_ARG(VMK_VMM_SERIAL_LOGGING, char *, str);
   char buffer[MAX_LOG_SIZE];
   static Bool needTag = TRUE;
   Bool eolFound;
   int len;
   char *p;

   while (1) {
      eolFound = ((p = strchr(str, '\n')) != NULL);
      if (eolFound) {
	 len = p - str;
      } else {
	 len = strlen(str);
      }
      if (len == 0) {
	 break;
      }

      ASSERT(len < sizeof buffer);

      strncpy(buffer, str, len);
      buffer[len] = '\0';

      if (needTag && eolFound) {
         VmLog(MY_RUNNING_WORLD->worldID, "%s", buffer);
      } else { 
         if (needTag) {
            char tsBuf[20];
            Util_FormatTimestamp(tsBuf, sizeof(tsBuf));
            _Log("%s cpu%d) VMM %d: %s", tsBuf, PRDA_GetPCPUNumSafe(), 
                 MY_RUNNING_WORLD->worldID, buffer);
         } else {
            _Log("%s%s", buffer, eolFound? "\n" : "");
         }
      }
      needTag = eolFound;

      str += eolFound ? len + 1 : len;
   }

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Log_PrintSysAlertBuffer --
 *
 *      Print the nAlerts most recent SysAlerts using the supplied 
 *      print function.  Makes a best effort attempt to print the 
 *      buffer in the correct order.  (SysAlerts that occur during 
 *      the printing of the buffer will mess up the order).
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
Log_PrintSysAlertBuffer(void (*printFn)(const char *text), int nAlerts)
{
   int i;
   int nextSlot = Atomic_Read(&curSysAlertBuf);

   nAlerts = MIN(nAlerts, SYSALERT_BUFFERS);
   for (i = 0; i < nAlerts; i++) {
      int curBuf =  nextSlot + i + (SYSALERT_BUFFERS - nAlerts);

      /* be paranoid & force null termination */
      sysAlertBuf[curBuf % SYSALERT_BUFFERS].msg[SYSALERT_BUFFER_LENGTH -1] = '\0';
      printFn(sysAlertBuf[curBuf % SYSALERT_BUFFERS].msg);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * Log_GetNextEntry --
 *
 * 	Return the next entry
 * 	len 0 means no next entry
 *
 * Results:
 * 	FALSE if the starting point is invalid
 *      TRUE otherwise
 *
 * Side Effects:
 * 	None
 *
 *-----------------------------------------------------------------------------
 */
Bool
Log_GetNextEntry(uint32 *entry, char *buffer, uint32 *len, Bool locked)
{
   SP_IRQL prevIRQL = SP_IRQL_KERNEL;
   uint32 earliest;
   uint32 nextEnd;
   uint32 nextStart;
   uint32 limit;
   uint32 actualLen;


   ASSERT(*len > 0);

   if (!locked) {
      prevIRQL = SP_LockIRQ(&logLock, SP_IRQL_KERNEL);
   }

   /*
    * Since we are really dealing with a circular buffer, we have to
    * check the point beyond which we cannot go back in the extrapolated
    * flat buffer.
    */
   if (logRunningPos < VMK_LOG_BUFFER_SIZE) {
      earliest = 0;
   } else {
      earliest = logRunningPos - VMK_LOG_BUFFER_SIZE;
   }

   /*
    * Check that we are inside the current window and if not return
    * failure.
    *
    * Stricly speaking, the window is [earliest, logRunningPos[.
    * We expand it on the left side to allow Log_GetEarliestEntry() to
    * work. That shouldn't cause any problem.
    */
   if ((*entry + 1 < earliest) || (*entry >= logRunningPos)) {
      *len = 0;
      if (!locked) {
         SP_UnlockIRQ(&logLock, prevIRQL);
      }
      return FALSE;
   }

   /*
    * *entry points to the end of the current entry, we have to search
    * forward for a '\n' and we assume that entries are never larger than
    * MAX_LOG_SIZE.
    *
    * A special case is made for Log_GetEarliestEntry() where we search
    * the successor of the one before the earliest. In case the earliest
    * would be the first one, *entry would be negative but we cannot have
    * negative numbers so we use 0. This has no bad side-effects assuming
    * the very first log entry is not empty.
    */
   if (*entry == 0) {
      nextStart = *entry;
   } else {
      nextStart = *entry + 1;
   }
   if (*entry + MAX_LOG_SIZE >= logRunningPos) {
      limit = logRunningPos;
   } else {
      limit = *entry + MAX_LOG_SIZE + 1;
   }

   ASSERT(nextStart <= limit);
   for (nextEnd = nextStart;
	 (nextEnd != limit) && (logBuffer[nextEnd % VMK_LOG_BUFFER_SIZE]!='\n');
	nextEnd++);

   /*
    * If we could not find a '\n', it is either because the entry is larger
    * than the expected limit or is not terminated yet. In both cases,
    * we cannot find a successor.
    */
   if (logBuffer[nextEnd % VMK_LOG_BUFFER_SIZE] != '\n') {
      *len = 0;
      if (!locked) {
         SP_UnlockIRQ(&logLock, prevIRQL);
      }
      return TRUE;
   }

   /*
    * Copy the entry without the '\n' and return it.
    */
   actualLen = nextEnd - nextStart;
   if (actualLen == 0) {
      // We need something, fake an entry made of a single blank character
      *buffer = ' ';
      *len = 1;
   } else {
      if (actualLen <= *len) {
         // Copy the whole entry
         *len = actualLen;
      } else {
         // Truncate the entry
      }
      memcpy(buffer, &logBuffer[nextStart % VMK_LOG_BUFFER_SIZE], *len);
   }
   *entry = nextEnd;

   if (!locked) {
      SP_UnlockIRQ(&logLock, prevIRQL);
   }
   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Log_GetPrevEntry --
 *
 * 	Return the previous entry
 * 	len 0 means no previous entry
 *
 * Results:
 * 	FALSE if the starting point is invalid
 * 	TRUE otherwise
 *
 * Side Effects:
 * 	None
 *
 *-----------------------------------------------------------------------------
 */
Bool
Log_GetPrevEntry(uint32 *entry, char *buffer, uint32 *len)
{
   SP_IRQL prevIRQL;
   uint32 earliest;
   uint32 prevEnd;
   uint32 prevStart;
   uint32 limit;
   uint32 actualLen;


   ASSERT(*len > 0);

   prevIRQL = SP_LockIRQ(&logLock, SP_IRQL_KERNEL);

   /*
    * Since we are really dealing with a circular buffer, we have to
    * check the point beyond which we cannot go back in the extrapolated
    * flat buffer.
    */
   if (logRunningPos < VMK_LOG_BUFFER_SIZE) {
      earliest = 0;
   } else {
      earliest = logRunningPos - VMK_LOG_BUFFER_SIZE;
   }

   /*
    * Check that we are inside the current window and if not return
    * failure.
    *
    * Stricly speaking, the window is [earliest, logRunningPos[.
    * We open it on the left side because if the end of the entry is at
    * the very start, it obviously has no predecessor.
    * We close it on the right side to allow Log_GetLatestEntry() to
    * work. That shouldn't cause any problem.
    */
   if ((*entry <= earliest) || (*entry > logRunningPos)) {
      *len = 0;
      SP_UnlockIRQ(&logLock, prevIRQL);
      return FALSE;
   }

   /*
    * *entry points to the end of the current entry, we have to search back
    * for a '\n' and we assume that entries are never larger than
    * MAX_LOG_SIZE.
    */
   if (*entry > earliest + MAX_LOG_SIZE) {
      limit = *entry - MAX_LOG_SIZE;
   } else {
      limit = earliest;
   }

   ASSERT(*entry > limit);
   for (prevEnd = *entry - 1;
	 (logBuffer[prevEnd % VMK_LOG_BUFFER_SIZE]!='\n') && (prevEnd != limit);
	prevEnd--);
 
   /*
    * If we could not find a '\n', it is either because the current entry
    * is the first one or is larger than the expected limit. In both cases,
    * we cannot find a predecessor.
    */
   if (logBuffer[prevEnd % VMK_LOG_BUFFER_SIZE] != '\n') {
      *len = 0;
      SP_UnlockIRQ(&logLock, prevIRQL);
      return TRUE;
   }

   /*
    * We now have to search back for a '\n' again to find the start of
    * the requested entry.
    */
   if (prevEnd > earliest + MAX_LOG_SIZE) {
      limit = prevEnd - MAX_LOG_SIZE;
   } else {
      limit = earliest;
   }

   prevStart = prevEnd;
   while (prevStart > limit) {
      prevStart--;
      if (logBuffer[prevStart % VMK_LOG_BUFFER_SIZE] == '\n') {
         break;
      }
   }

   /*
    * If we could not find a '\n', it is either because the entry is the
    * first one and we should return it or is larger than the expected limit
    * and we fail.
    */
   if (logBuffer[prevStart % VMK_LOG_BUFFER_SIZE] != '\n') {
      if (prevStart > earliest) {
	 *len = 0;
	 SP_UnlockIRQ(&logLock, prevIRQL);
	 return TRUE;
      }
   } else {
      if (prevStart == prevEnd) { // Empty entry
      } else { // We are on the previous '\n', move forward
         prevStart++;
      }
   }

   /*
    * Copy the entry without the '\n' and return it.
    */
   actualLen = prevEnd - prevStart;
   if (actualLen == 0) {
      // We need something, fake an entry made of a single blank character
      *buffer = ' ';
      *len = 1;
   } else {
      if (actualLen <= *len) {
         // Copy the whole entry
	 *len = actualLen;
      } else {
	 // Truncate the entry
      }
      memcpy(buffer, &logBuffer[prevStart % VMK_LOG_BUFFER_SIZE], *len);
   }
   *entry = prevEnd;

   SP_UnlockIRQ(&logLock, prevIRQL);
   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Log_GetLatestEntry --
 *
 * 	Return the most recent entry as of this call
 * 	len 0 means no entry
 *
 * Results:
 * 	None
 *
 * Side Effects:
 * 	None
 *
 *-----------------------------------------------------------------------------
 */
void
Log_GetLatestEntry(uint32 *entry, char *buffer, uint32 *len)
{
   Bool ok;

   /*
    * logRunningPos points to one past the last character logged
    * so it is conceptually part of the entry following the latest
    * entry so we return its predecessor.
    */
   if (logRunningPos == 0) { // empty log
      *len = 0;
      return;
   }
   *entry = logRunningPos; // Sample current value
   ok = Log_GetPrevEntry(entry, buffer, len);
   ASSERT(ok);
}


/*
 *-----------------------------------------------------------------------------
 *
 * Log_GetEarliestEntry --
 *
 * 	Return the least recent entry as of this call
 * 	len 0 means no entry
 *
 * Results:
 * 	None
 *
 * Side Effects:
 * 	None
 *
 *-----------------------------------------------------------------------------
 */
void
Log_GetEarliestEntry(uint32 *entry, char *buffer, uint32 *len)
{
   SP_IRQL prevIRQL;
   uint32 earliest;
   Bool ok;


   prevIRQL = SP_LockIRQ(&logLock, SP_IRQL_KERNEL);

   /*
    * Since we are really dealing with a circular buffer, we have to
    * check the point beyond which we cannot go back in the extrapolated
    * flat buffer.
    */
   if (logRunningPos < VMK_LOG_BUFFER_SIZE) {
      earliest = 0;
   } else {
      earliest = logRunningPos - VMK_LOG_BUFFER_SIZE;
   }

   /*
    * earliest-1 is conceptually part of the entry preceding the earliest
    * entry so we return its successor.
    */
   *entry = earliest ? earliest - 1 : 0;
   ok = Log_GetNextEntry(entry, buffer, len, TRUE);
   ASSERT(ok);

   SP_UnlockIRQ(&logLock, prevIRQL);
}
