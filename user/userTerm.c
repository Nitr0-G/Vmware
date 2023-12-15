/* **********************************************************
 * Copyright 2004 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * userTerm.c --
 *
 *      Userworld interface to terminals
 */

#include "user_int.h"
#include "userObj.h"
#include "term.h"
#include "linuxSerial.h"

#define LOGLEVEL_MODULE     UserTerm
#include "userLog.h"



static VMK_ReturnStatus UserTermRead(UserObj* obj, UserVA userData,
                                      uint64 offset, uint32 length,
                                      uint32 *bytesRead);
static VMK_ReturnStatus UserTermWrite(UserObj* obj, UserVAConst userData,
                                       uint64 offset, uint32 length,
                                       uint32 *bytesWritten);
static VMK_ReturnStatus UserTermStat(UserObj* obj, LinuxStat64* statbuf);
static VMK_ReturnStatus UserTermIoctl(UserObj* obj, uint32 cmd,
                                       LinuxIoctlArgType type, uint32 size,
                                       void *userData,
                                       uint32 *result);
static VMK_ReturnStatus UserTermToString(UserObj *obj, char *string,
					 int length);


static UserObj_Methods termMethods = USEROBJ_METHODS(
   (UserObj_OpenMethod) UserObj_BadParam,
   (UserObj_CloseMethod) UserObj_Nop,
   UserTermRead,
   (UserObj_ReadMPNMethod) UserObj_BadParam,
   UserTermWrite,
   (UserObj_WriteMPNMethod) UserObj_BadParam,
   UserTermStat,
   (UserObj_ChmodMethod) UserObj_BadParam,
   (UserObj_ChownMethod) UserObj_BadParam,
   (UserObj_TruncateMethod) UserObj_BadParam,
   (UserObj_UtimeMethod) UserObj_BadParam,
   (UserObj_StatFSMethod) UserObj_BadParam,
   (UserObj_PollMethod) UserObj_BadParam,	// XXX todo if needed
   (UserObj_UnlinkMethod) UserObj_NotADirectory,
   (UserObj_MkdirMethod) UserObj_NotADirectory,
   (UserObj_RmdirMethod) UserObj_NotADirectory,
   (UserObj_GetNameMethod) UserObj_NotADirectory,
   (UserObj_ReadSymLinkMethod) UserObj_NotADirectory,
   (UserObj_MakeSymLinkMethod) UserObj_NotADirectory,
   (UserObj_MakeHardLinkMethod) UserObj_NotADirectory,
   (UserObj_RenameMethod) UserObj_NotADirectory,
   (UserObj_MknodMethod) UserObj_NotADirectory,
   (UserObj_FcntlMethod) UserObj_BadParam,
   (UserObj_FsyncMethod) UserObj_BadParam,
   (UserObj_ReadDirMethod) UserObj_NotADirectory,
   UserTermIoctl,
   UserTermToString,
   (UserObj_BindMethod) UserObj_NotASocket,
   (UserObj_ConnectMethod) UserObj_NotASocket,
   (UserObj_SocketpairMethod) UserObj_NotASocket,
   (UserObj_AcceptMethod) UserObj_NotASocket,
   (UserObj_GetSocketNameMethod) UserObj_NotASocket,
   (UserObj_ListenMethod) UserObj_NotASocket,
   (UserObj_SetsockoptMethod) UserObj_NotASocket,
   (UserObj_GetsockoptMethod) UserObj_NotASocket,
   (UserObj_SendmsgMethod) UserObj_NotASocket,
   (UserObj_RecvmsgMethod) UserObj_NotASocket,
   (UserObj_GetPeerNameMethod) UserObj_NotASocket,
   (UserObj_ShutdownMethod) UserObj_NotASocket
);


#define USERTERM_STDIN	0
#define USERTERM_STDOUT	1
#define USERTERM_STDERR	2

#define USERTERM_MAX_INPUT	256	// Buffer keyboard input
#define USERTERM_MAX_OUTPUT	4096	// No reason for a limit except to
					// catch bad parameters


static struct {
   uint32      term;
   SP_SpinLock lock;
   int         inputSize;
   char        input[USERTERM_MAX_INPUT];
} userTerm = {TERM_INVALID, {{0}}, 0, {0}};


static void UserTermInputCallback(const char *txt);

static const Term_AllocArgs userTermArgs =
                     {FALSE, TRUE, {ANSI_WHITE, ANSI_BLACK, FALSE, 0},
                      TERM_INPUT_ASYNC_LINE, UserTermInputCallback,
                      NULL, NULL, TERM_ALT_FN_FOR_USER};


/*
 *-----------------------------------------------------------------------------
 *
 * UserTermStart --
 *
 * 	Setup and display userworld terminal
 *
 * Results:
 * 	TRUE if succesful, FALSE otherwise
 *
 * Side Effects:
 * 	None
 *
 *-----------------------------------------------------------------------------
 */
static Bool
UserTermStart(void)
{
   uint32 numRows;
   uint32 numCols;

   if (userTerm.term == TERM_INVALID) {
      userTerm.term = Term_Alloc(&userTermArgs, &numRows, &numCols);
      if (userTerm.term == TERM_INVALID) {
         return FALSE;
      }
      SP_InitLock("userTermLck", &userTerm.lock, SP_RANK_LEAF);
   }

   /*
    * Bring terminal on screen.
    */
   Term_Display(userTerm.term);

   return TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * UserTerm_CreateSpecialFds --
 *
 * 	Create the objects for the special (stdin, stdout, stderr)
 * 	file descriptors.
 *
 * Results:
 * 	VMK_OK if successful
 * 	VMK_NOT_FOUND if the caller is not a userworld
 * 	VMK_FAILURE if the user terminal is not available
 *
 *
 * Side Effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */

VMK_ReturnStatus
UserTerm_CreateSpecialFds(World_Handle *world)
{
   User_CartelInfo *uci = world->userCartelInfo;
   int fd;

   ASSERT(world != MY_RUNNING_WORLD);

   if (! World_IsUSERWorld(world)) {
      return VMK_NOT_FOUND;
   }

   // Make sure user terminal is available
   if (!UserTermStart()) {
      return VMK_FAILURE;
   }

   // stdin
   fd = UserObj_FDAdd(uci, USEROBJ_TYPE_TERM, (UserObj_Data)USERTERM_STDIN,
			&termMethods, USEROBJ_OPEN_RDONLY);
   ASSERT_NOT_IMPLEMENTED(fd == USERTERM_STDIN);

   // stdout
   fd = UserObj_FDAdd(uci, USEROBJ_TYPE_TERM, (UserObj_Data)USERTERM_STDOUT,
			&termMethods, USEROBJ_OPEN_WRONLY);
   ASSERT_NOT_IMPLEMENTED(fd == USERTERM_STDOUT);

   // stderr
   fd = UserObj_FDAdd(uci, USEROBJ_TYPE_TERM, (UserObj_Data)USERTERM_STDERR,
			&termMethods, USEROBJ_OPEN_WRONLY);
   ASSERT_NOT_IMPLEMENTED(fd == USERTERM_STDERR);

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserTermInputCallback --
 *
 *      Callback on input events
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
UserTermInputCallback(const char *txt)
{
   uint32 length = strlen(txt);


   /*
    * Buffer the input and wake up any waiters. If there is not enough
    * space left, the new input is dropped.
    * '\0' is used to separate the lines.
    */
   SP_Lock(&userTerm.lock);
   if (userTerm.inputSize + length + 1 <= USERTERM_MAX_INPUT) {
      memcpy(&userTerm.input[userTerm.inputSize], txt, length);
      userTerm.inputSize += length;
      userTerm.input[userTerm.inputSize++] = '\0';
      CpuSched_Wakeup((uint32)userTerm.input);
   }
   SP_Unlock(&userTerm.lock);
}


/*
 *----------------------------------------------------------------------
 *
 * UserTermRead --
 *
 *      Read up to length bytes from the terminal.
 *	Sets bytesRead to number of bytes actually read.
 *	BytesRead is undefined if an error is returned.
 *
 * Results:
 *      VMK_OK if some bytes were successfully read.  Error code
 *      otherwise.  *bytesRead is set to number of bytes read.
 *
 * Side effects:
 *      Bytes are read from the object.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserTermRead(UserObj* obj,
              UserVA userData,
              uint64 offset,
              uint32 length,
              uint32 *bytesRead)
{
   char data[USERTERM_MAX_INPUT];
   uint32 actualLength;
   VMK_ReturnStatus status;

   ASSERT(userTerm.term != TERM_INVALID);

   if (!UserObj_IsOpenForRead(obj)) {
      return VMK_INVALID_HANDLE;
   }

   if (length == 0) {
      *bytesRead = 0;
      return VMK_OK;
   }

   /*
    * Input is buffered line by line so if the buffer is not empty, there
    * is at least one line, return it otherwise wait on input and try
    * again.
    */
   for (;;) {

      SP_Lock(&userTerm.lock);

      // Nothing available yet
      if (userTerm.inputSize == 0) {
	 CpuSched_Wait((uint32)userTerm.input, CPUSCHED_WAIT_UW_TERM, &userTerm.lock);
	 continue;
      }

      // Get first available line
      actualLength = strlen(userTerm.input) + 1; // ending '\0'
      ASSERT(actualLength <= userTerm.inputSize);

      // Truncate as needed
      if (actualLength <= length) {
	 ASSERT(actualLength <= sizeof(data));
	 memcpy(data, userTerm.input, actualLength-1); // without the '\0'
	 data[actualLength-1] = '\n'; // tack on the '\n'
      } else {
	 actualLength = length;
	 ASSERT(actualLength <= sizeof(data));
	 memcpy(data, userTerm.input, actualLength); // partial line
      }

      // Remove from the buffered input
      ASSERT((actualLength < USERTERM_MAX_INPUT) || (userTerm.inputSize - actualLength == 0));
      memcpy(userTerm.input, &userTerm.input[actualLength], userTerm.inputSize - actualLength);
      userTerm.inputSize -= actualLength;
      
      SP_Unlock(&userTerm.lock);
      break;
   }

   status = User_CopyOut(userData, data, actualLength);
   ASSERT(status == VMK_OK);
   *bytesRead = actualLength;
   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserTermWrite --
 *
 *      Write up to length bytes on the terminal.
 *	Sets bytesWritten to number of bytes actually written.
 *	bytesWritten is undefined if an error is returned.
 *
 * Results:
 *      VMK_OK if some bytes were successfully written.  Error code
 *      otherwise.  *bytesWritten is set to number of bytes written.
 *
 * Side effects:
 *      Bytes are written to the object.
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserTermWrite(UserObj* obj,
               UserVAConst userData,
               uint64 offset,
               uint32 length,
               uint32 *bytesWritten)
{
   char data[128];
   uint32 actualLength;
   VMK_ReturnStatus status;

   ASSERT(userTerm.term != TERM_INVALID);

   if (!UserObj_IsOpenForWrite(obj)) {
      return VMK_INVALID_HANDLE;
   }

   /*
    * No reason for a limit except to catch bad parameters.
    */
   length = MIN(length, USERTERM_MAX_OUTPUT);
   *bytesWritten = length;

   /*
    * Output everything by chunks.
    */
   while (length > 0) {

      // Buffer one chunk
      actualLength = MIN(length, sizeof(data)-1);
      status = User_CopyIn(data, userData, actualLength);
      ASSERT(status == VMK_OK);
      data[actualLength] = '\0';
      length -= actualLength;
      userData += actualLength;

      // Output
      SP_Lock(&userTerm.lock);
      Term_Printf(userTerm.term, 0, "%s", data);
      SP_Unlock(&userTerm.lock);
   }

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserTermStat --
 *
 * 	Get stats for given object.
 * 	This is only needed to make glibc happy.
 *
 * Results:
 * 	VMK_OK
 *
 * Side effects:
 * 	none
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserTermStat(UserObj* obj, LinuxStat64* statbuf)
{
   int id = obj->data.stdioID;
 
   ASSERT(userTerm.term != TERM_INVALID);

   memset(statbuf, 0, sizeof *statbuf);

   /*
    * glibc uses this to realize it is dealing with a terminal
    */
   statbuf->st_mode = LINUX_MODE_IFCHR;
   statbuf->st_rdev = 0x88FF; // PTS 255, could be any

   /*
    * Optimize by giving out our size limit
    */
   switch (id) {
   case USERTERM_STDIN:
      statbuf->st_blksize = USERTERM_MAX_INPUT;
      break;
   case USERTERM_STDOUT:
   case USERTERM_STDERR:
      statbuf->st_blksize = USERTERM_MAX_OUTPUT;
      break;
   default:
      ASSERT(FALSE);
      break;
   }

   return VMK_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * UserTermIoctl --
 *
 *      This is only needed to make glibc happy.
 *
 * Results:
 *      VMK_OK
 *
 * Side effects:
 *      none
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserTermIoctl(UserObj* obj, uint32 cmd, LinuxIoctlArgType type,
              uint32 size, void *userData, uint32 *result)
{
   struct Linux_termios termios;
   DEBUG_ONLY(int id = obj->data.stdioID);
   VMK_ReturnStatus status;

   ASSERT(userTerm.term != TERM_INVALID);
   ASSERT(size == sizeof termios);

   if (type != LINUX_IOCTL_ARG_PTR) {
      UWWarn("Invalid args: cmd = %d, type = %d, size = %d",
             LINUX_IOCTL_CMD(cmd), type, size);
      return VMK_NOT_SUPPORTED;
   }

   switch(LINUX_IOCTL_CMD(cmd)) {
   case LINUX_TCGETS:               /* Get termios struct */
      memset(&termios, 0, sizeof termios);
      *result = 0;
      status = User_CopyOut((VA)userData, &termios, sizeof termios);
      UWLOG(0, "TCGETS for %d: %x", id, status);
      return status;
   default:
      UWWarn("Invalid args: cmd = %d, type = %d, size = %d",
             LINUX_IOCTL_CMD(cmd), type, size);
      return VMK_NOT_SUPPORTED;
   }
   NOT_REACHED();
}


/*
 *----------------------------------------------------------------------
 *
 * UserTermToString --
 *
 *	Returns a string representation of this object.
 *
 * Results:
 *      VMK_OK.
 *
 * Side effects:
 *      none
 *
 *----------------------------------------------------------------------
 */

static VMK_ReturnStatus
UserTermToString(UserObj *obj, char *string, int length)
{
   snprintf(string, length, "term: %d inputSize: %d", userTerm.term,
	    userTerm.inputSize);
   return VMK_OK;
}
