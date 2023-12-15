/* **********************************************************
 * Copyright 2003 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * linuxIdent.c -
 *	Linux user/group identity (real, effective, saved) syscalls.
 */

#include "user_int.h"
#include "linuxIdent.h"

#define LOGLEVEL_MODULE LinuxIdent
#include "userLog.h"
#include "userProxy_ext.h"
#include "userProxy.h"
#include "userIdent.h"
#include "linuxAPI.h"

/*
 *-----------------------------------------------------------------------------
 * 
 * LinuxIdent_Getuid - 
 *	Handler for linux syscall 199 
 * Support: 100%
 * Error case: 100%
 * Results:
 *      Returns the real uid of the thread.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
LinuxUID
LinuxIdent_Getuid(void)
{
   UWLOG_SyscallEnter("(void)");
   return MY_RUNNING_WORLD->ident.ruid;
}

/*
 *-----------------------------------------------------------------------------
 * 
 * LinuxIdent_Getgid - 
 *	Handler for linux syscall 200 
 * Support: 100%
 * Error case: 100%
 * Results:
 *      Returns the real primary gid of the thread.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
LinuxGID
LinuxIdent_Getgid(void)
{
   UWLOG_SyscallEnter("(void)");
   return MY_RUNNING_WORLD->ident.rgid;
}

/*
 *-----------------------------------------------------------------------------
 * 
 * LinuxIdent_Geteuid - 
 *	Handler for linux syscall 201 
 * Support: 100%
 * Error case: 100%
 * Results:
 *      Returns the effective uid of the thread.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
LinuxUID
LinuxIdent_Geteuid(void)
{
   UWLOG_SyscallEnter("(void)");
   return MY_RUNNING_WORLD->ident.euid;
}


/*
 *-----------------------------------------------------------------------------
 * 
 * LinuxIdent_Getegid - 
 *	Handler for linux syscall 202 
 * Support: 100%
 * Error case: 100%
 * Results:
 *      Returns the effective primary gid of the thread.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
LinuxGID
LinuxIdent_Getegid(void)
{
   UWLOG_SyscallEnter("(void)");
   return MY_RUNNING_WORLD->ident.egid;
}


/*
 *-----------------------------------------------------------------------------
 * 
 * LinuxIdent_Setuid - 
 *	Handler for linux syscall 213 
 * Support: 100%
 * Error case: 100%
 * Results:
 *      0 or Linux error code.
 *
 * Side effects:
 *      If the current effective uid is root, set the effective, real,
 *      and saved uids to uid.  Othewise if uid is equal to the ruid
 *      or suid, set the euid to uid.  Otherwise return EPERM.
 *
 *-----------------------------------------------------------------------------
 */
int
LinuxIdent_Setuid(LinuxUID uid)
{
   Identity* ident = &MY_RUNNING_WORLD->ident;
   VMK_ReturnStatus status;
   int rc = 0;

   UWLOG_SyscallEnter("(uid=%d)", uid);

   if (ident->euid == 0) {

      status = UserProxy_Setresuid(MY_USER_CARTEL_INFO, uid, uid, uid);
      if (status == VMK_OK) {
         ident->euid = ident->ruid = ident->suid = uid;
      } else {
         rc = User_TranslateStatus(status);
      }

   } else if (uid == ident->ruid || uid == ident->suid) {

      status = UserProxy_Setresuid(MY_USER_CARTEL_INFO, -1, uid, -1);
      if (status == VMK_OK) {
         ident->euid = uid;
      } else {
         rc = User_TranslateStatus(status);
      }

   } else {
      rc = LINUX_EPERM;
   }

   return rc;
}


/*
 *-----------------------------------------------------------------------------
 * 
 * LinuxIdent_Setgid - 
 *	Handler for linux syscall 214 
 * Support: 100%
 * Error case: 100%
 * Results:
 *      0 or Linux error code.
 *
 * Side effects:
 *      If the current effective uid is root, set the effective, real,
 *      and saved gids to gid.  Othewise if gid is equal to the rgid
 *      or sgid, set the egid to gid.  Otherwise return EPERM.
 *
 *-----------------------------------------------------------------------------
 */
int
LinuxIdent_Setgid(LinuxGID gid)
{
   Identity* ident = &MY_RUNNING_WORLD->ident;
   VMK_ReturnStatus status;
   int rc = 0;

   UWLOG_SyscallEnter("(gid=%d)", gid);

   if (ident->euid == 0) {

      status = UserProxy_Setresgid(MY_USER_CARTEL_INFO, gid, gid, gid);
      if (status == VMK_OK) {
         ident->egid = ident->rgid = ident->sgid = gid;
      } else {
         rc = User_TranslateStatus(status);
      }

   } else if (gid == ident->rgid || gid == ident->sgid) {

      status = UserProxy_Setresgid(MY_USER_CARTEL_INFO, -1, gid, -1);
      if (status == VMK_OK) {
         ident->egid = gid;
      } else {
         rc = User_TranslateStatus(status);
      }

   } else {
      rc = LINUX_EPERM;
   }

   return rc;
}


/*
 *-----------------------------------------------------------------------------
 * 
 * LinuxIdent_Getgroups - 
 *	Handler for linux syscall 205 
 * Support: 100%
 * Error case: 100%
 * Results:
 *      Number of supplementary group ids possessed by this thread
 *      or Linux error code.
 *
 * Side effects:
 *      If size > 0, the supplementary group ids are returned in
 *      grouplist, or else EINVAL if there are more than size group ids.
 *      It is unspecified whether or not the effective gid is included.
 *      (Currently we don't add it in.)
 *      If size == 0, only the number of group ids is returned.
 *
 *-----------------------------------------------------------------------------
 */
int
LinuxIdent_Getgroups(int32 ngids,
                     UserVA /* LinuxGID* */ userGIDs)
{
   Identity* ident = &MY_RUNNING_WORLD->ident;
   VMK_ReturnStatus status = VMK_OK;
   int rc;

   UWLOG_SyscallEnter("(ngids=%d, userGIDs@%#x)", ngids, userGIDs);

   if (ngids == 0) {
      rc = ident->ngids;

   } else if (ngids < ident->ngids) {
      rc = LINUX_EINVAL;

   } else {
      if (ident->ngids != 0) {
	 status = User_CopyOut(userGIDs, ident->gids,
			       sizeof(LinuxGID) * ident->ngids);
      }

      if (status == VMK_OK) {
         rc = ident->ngids;
      } else {
         rc = User_TranslateStatus(status);
      }
   }

   return rc;
}


/*
 *-----------------------------------------------------------------------------
 * 
 * LinuxIdent_Setgroups - 
 *	Handler for linux syscall 206 
 * Support: 100%
 * Error case: 100%
 * Results:
 *      0 or Linux error code.
 *
 * Side effects:
 *      Sets the supplementary group ids for this thread.
 *
 *-----------------------------------------------------------------------------
 */
int
LinuxIdent_Setgroups(uint32 ngids,
                     UserVAConst /* const LinuxGID* */ userGIDs)
{
   Identity* ident = &MY_RUNNING_WORLD->ident;
   LinuxGID gids[LINUX_NGROUPS_MAX];
   VMK_ReturnStatus status;

   UWLOG_SyscallEnter("(ngids=%d, userGIDs@%#x)", ngids, userGIDs);

   if (ngids > LINUX_NGROUPS_MAX) {
      return LINUX_EINVAL;
   }

   if (ngids != 0) {
      status = User_CopyIn(gids, userGIDs, sizeof(LinuxGID) * ngids);
      if (status != VMK_OK) {
	 return User_TranslateStatus(status);
      }
   }

   if (ident->euid != 0) {
      return LINUX_EPERM;
   }

   status = UserProxy_Setgroups(MY_USER_CARTEL_INFO, ngids, gids);
   if (status != VMK_OK) {
      return User_TranslateStatus(status);
   }

   memcpy(ident->gids, gids, sizeof(LinuxGID) * ngids);
   ident->ngids = ngids;

   return 0;
}


/*
 *-----------------------------------------------------------------------------
 * 
 * LinuxIdent_Setresuid - 
 *	Handler for linux syscall 208 
 * Support: 100%
 * Error case: 100%
 * Results:
 *      0 or Linux error code.
 *
 * Side effects:
 *      Sets the real, effective, and saved uid of this thread.
 *      If one of the parameters is -1, the corresponding uid is not changed.
 *      Root may set each uid to any value.  Other users may set each uid
 *      to the old value of any of the three uids.
 *
 *-----------------------------------------------------------------------------
 */
int
LinuxIdent_Setresuid(LinuxUID ruid, LinuxUID euid, LinuxUID suid)
{
   Identity* ident = &MY_RUNNING_WORLD->ident;
   int rc = 0;
   VMK_ReturnStatus status;

   UWLOG_SyscallEnter("(ruid=%d, euid=%d, suid=%d).", ruid, euid, suid);

   if (ident->euid != 0) {
      if ((ruid != -1 && ruid != ident->ruid &&
           ruid != ident->euid && ruid != ident->suid) ||
          (euid != -1 && euid != ident->ruid &&
           euid != ident->euid && euid != ident->suid) ||
          (suid != -1 && suid != ident->ruid &&
           suid != ident->euid && suid != ident->suid)) {
         UWLOG(1, " -> EPERM");
         return LINUX_EPERM;
      }
   }

   status = UserProxy_Setresuid(MY_USER_CARTEL_INFO, ruid, euid, suid);
   if (status != VMK_OK) {
      UWLOG(1, " -> %s", UWLOG_ReturnStatusToString(status));
      return User_TranslateStatus(status);
   }

   if (ruid != -1) {
      ident->ruid = ruid;
   }
   if (euid != -1) {
      ident->euid = euid;
   }
   if (suid != -1) {
      ident->suid = suid;
   }

   return rc;
}


/*
 *-----------------------------------------------------------------------------
 * 
 * LinuxIdent_Getresuid - 
 *	Handler for linux syscall 209 
 * Support: 100%
 * Error case: 100%
 * Results:
 *      0 or Linux error code.
 *
 * Side effects:
 *      Returns the real, effective, and saved uid of this thread
 *      at *ruid, *euid, *suid respectively.
 *
 *-----------------------------------------------------------------------------
 */
int
LinuxIdent_Getresuid(UserVA /* LinuxUID* */ userRuid,
                     UserVA /* LinuxUID* */ userEuid,
                     UserVA /* LinuxUID* */ userSuid)
{
   Identity* ident = &MY_RUNNING_WORLD->ident;
   VMK_ReturnStatus status = VMK_OK;

   UWLOG_SyscallEnter("(...)");

   if (userRuid != 0) {
      status = User_CopyOut(userRuid, &ident->ruid, sizeof(LinuxUID));
   }
   if ((status == VMK_OK) && (userEuid != 0)) {
      status = User_CopyOut(userEuid, &ident->euid, sizeof(LinuxUID));
   }
   if ((status == VMK_OK) && (userSuid != 0)) {
      status = User_CopyOut(userSuid, &ident->suid, sizeof(LinuxUID));
   }

   return User_TranslateStatus(status);
}

/*
 *-----------------------------------------------------------------------------
 * 
 * LinuxIdent_Setresgid - 
 *	Handler for linux syscall 210 
 * Support: 100%
 * Error case: 100%
 * Results:
 *      0 or Linux error code.
 *
 * Side effects:
 *      Sets the real, effective, and saved gid of this thread.
 *      If one of the parameters is -1, the corresponding gid is not changed.
 *      Root may set each gid to any value.  Other users may set each gid
 *      to the old value of any of the three gids.
 *
 *-----------------------------------------------------------------------------
 */
int
LinuxIdent_Setresgid(LinuxGID rgid, LinuxGID egid, LinuxGID sgid)
{
   Identity* ident = &MY_RUNNING_WORLD->ident;
   int rc = 0;
   VMK_ReturnStatus status;

   UWLOG_SyscallEnter("(rgid=%d, egid=%d, sgid=%d).", rgid, egid, sgid);

   if (ident->euid != 0) {
      if ((rgid != -1 && rgid != ident->rgid &&
           rgid != ident->egid && rgid != ident->sgid) ||
          (egid != -1 && egid != ident->rgid &&
           egid != ident->egid && egid != ident->sgid) ||
          (sgid != -1 && sgid != ident->rgid &&
           sgid != ident->egid && sgid != ident->sgid)) {
         UWLOG(1, " -> EPERM");
         return LINUX_EPERM;
      }
   }

   status = UserProxy_Setresgid(MY_USER_CARTEL_INFO, rgid, egid, sgid);
   if (status != VMK_OK) {
      UWLOG(1, " -> %s", UWLOG_ReturnStatusToString(status));
      return User_TranslateStatus(status);
   }

   if (rgid != -1) {
      ident->rgid = rgid;
   }
   if (egid != -1) {
      ident->egid = egid;
   }
   if (sgid != -1) {
      ident->sgid = sgid;
   }

   return rc;
}


/*
 *-----------------------------------------------------------------------------
 * 
 * LinuxIdent_Getresgid - 
 *	Handler for linux syscall 211 
 * Support: 100%
 * Error case: 100%
 * Results:
 *      0 or Linux error code.
 *
 * Side effects:
 *      Returns the real, effective, and saved gid of this thread
 *      at *rgid, *egid, *sgid respectively.
 *
 *-----------------------------------------------------------------------------
 */
int
LinuxIdent_Getresgid(UserVA /* LinuxUID* */ userRgid,
                     UserVA /* LinuxUID* */ userEgid,
                     UserVA /* LinuxUID* */ userSgid)
{
   Identity* ident = &MY_RUNNING_WORLD->ident;
   VMK_ReturnStatus status = VMK_OK;

   UWLOG_SyscallEnter("(...)");

   if (userRgid != 0) {
      status = User_CopyOut(userRgid, &ident->rgid, sizeof(LinuxGID));
   }
   if ((status == VMK_OK) && (userEgid != 0)) {
      status = User_CopyOut(userEgid, &ident->egid, sizeof(LinuxGID));
   }
   if ((status == VMK_OK) && (userSgid != 0)) {
      status = User_CopyOut(userSgid, &ident->sgid, sizeof(LinuxGID));
   }

   return User_TranslateStatus(status);
}


/*
 *-----------------------------------------------------------------------------
 * 
 * LinuxIdent_Setreuid - 
 *	Handler for linux syscall 203 
 * Support: 90%
 *      Implemented as setresuid(ruid, euid, -1).  This is slightly
 *      more permissive than the Linux version, if the Linux man pages
 *      are to be believed.
 * Error case: 100%
 * Results:
 *      0 or Linux error code.
 *
 * Side effects:
 *      See LinuxIdent_Setresuid.
 *
 *-----------------------------------------------------------------------------
 */
int
LinuxIdent_Setreuid(LinuxUID ruid, LinuxUID euid)
{
   return LinuxIdent_Setresuid(ruid, euid, -1);
}


/*
 *-----------------------------------------------------------------------------
 * 
 * LinuxIdent_Setregid - 
 *	Handler for linux syscall 204 
 * Support: 90%
 *      Implemented as setresgid(rgid, egid, -1).  This is slightly
 *      more permissive than the Linux version, if the Linux man pages
 *      are to be believed.
 * Error case: 100%
 * Results:
 *      0 or Linux error code.
 *
 * Side effects:
 *      See LinuxIdent_Setresgid.
 *
 *-----------------------------------------------------------------------------
 */
int
LinuxIdent_Setregid(LinuxGID rgid, LinuxGID egid)
{
   return LinuxIdent_Setresgid(rgid, egid, -1);
}


/*
 * --------------------------
 *
 * Beyond here lie only stale, deprecated identity functions.
 *
 * --------------------------
 */



/*
 *-----------------------------------------------------------------------------
 * 
 * LinuxIdent_Setuid16 - 
 *	Handler for linux syscall 23 
 * Support: 0% (obsolete)
 * Error case:
 * Results:
 *
 *
 * Side effects:
 *
 *
 *-----------------------------------------------------------------------------
 */
int
LinuxIdent_Setuid16(LinuxUID16 uid)
{
   UWLOG_SyscallUnsupported("use setresuid (uid=%d)",
         uid);
   return LINUX_ENOSYS;
}


/*
 *-----------------------------------------------------------------------------
 * 
 * LinuxIdent_Getuid16 - 
 *	Handler for linux syscall 24 
 * Support: 0% (obsolete)
 * Error case:
 * Results:
 *
 *
 * Side effects:
 *
 *
 *-----------------------------------------------------------------------------
 */
int
LinuxIdent_Getuid16(void)
{
   UWLOG_SyscallUnsupported("use getresuid");
   return LINUX_ENOSYS;
}


/*
 *-----------------------------------------------------------------------------
 * 
 * LinuxIdent_Setgid16 - 
 *	Handler for linux syscall 46 
 * Support: 0% (obsolete)
 * Error case:
 * Results:
 *
 *
 * Side effects:
 *
 *
 *-----------------------------------------------------------------------------
 */
int
LinuxIdent_Setgid16(LinuxGID16 gid)
{
   UWLOG_SyscallUnsupported("use setresgid (gid=%d)",
         gid);
   return LINUX_ENOSYS;
}


/*
 *-----------------------------------------------------------------------------
 * 
 * LinuxIdent_Getgid16 - 
 *	Handler for linux syscall 47 
 * Support: 0% (obsolete)
 * Error case:
 * Results:
 *
 *
 * Side effects:
 *
 *
 *-----------------------------------------------------------------------------
 */
int
LinuxIdent_Getgid16(void)
{
   UWLOG_SyscallUnsupported("use getresgid");
   return LINUX_ENOSYS;
}


/*
 *-----------------------------------------------------------------------------
 * 
 * LinuxIdent_Geteuid16 - 
 *	Handler for linux syscall 49 
 * Support: 0% (obsolete)
 * Error case:
 * Results:
 *
 *
 * Side effects:
 *
 *
 *-----------------------------------------------------------------------------
 */
int
LinuxIdent_Geteuid16(void)
{
   UWLOG_SyscallUnsupported("use getresuid");
   return LINUX_ENOSYS;
}


/*
 *-----------------------------------------------------------------------------
 * 
 * LinuxIdent_Getegid16 - 
 *	Handler for linux syscall 50 
 * Support: 0% (obsolete)
 * Error case:
 * Results:
 *
 *
 * Side effects:
 *
 *
 *-----------------------------------------------------------------------------
 */
int
LinuxIdent_Getegid16(void)
{
   UWLOG_SyscallUnsupported("use getresgid");
   return LINUX_ENOSYS;
}


/*
 *-----------------------------------------------------------------------------
 * 
 * LinuxIdent_Setreuid16 - 
 *	Handler for linux syscall 70 
 * Support: 0% (obsolete)
 * Error case:
 * Results:
 *
 *
 * Side effects:
 *
 *
 *-----------------------------------------------------------------------------
 */
int
LinuxIdent_Setreuid16(LinuxUID16 ruid, LinuxUID16 euid)
{
   UWLOG_SyscallUnsupported("use setresuid (r=%d, e=%d)",
         ruid, euid);
   return LINUX_ENOSYS;
}


/*
 *-----------------------------------------------------------------------------
 * 
 * LinuxIdent_Setregid16 - 
 *	Handler for linux syscall 71 
 * Support: 0% (obsolete)
 * Error case:
 * Results:
 *
 *
 * Side effects:
 *
 *
 *-----------------------------------------------------------------------------
 */
int
LinuxIdent_Setregid16(LinuxGID16 rgid, LinuxGID16 egid)
{
   UWLOG_SyscallUnsupported("use setresgid (r=%d, e=%d)",
         rgid, egid);
   return LINUX_ENOSYS;
}


/*
 *-----------------------------------------------------------------------------
 * 
 * LinuxIdent_Getgroups16 - 
 *	Handler for linux syscall 80 
 * Support: 0% (obsolete)
 * Error case:
 * Results:
 *
 *
 * Side effects:
 *
 *
 *-----------------------------------------------------------------------------
 */
int
LinuxIdent_Getgroups16(uint32 gidsetsize,
                     UserVA /* LinuxGID16* */ gidset)
{
   UWLOG_SyscallUnsupported("use getgroups");
   return LINUX_ENOSYS;
}


/*
 *-----------------------------------------------------------------------------
 * 
 * LinuxIdent_Setgroups16 - 
 *	Handler for linux syscall 81 
 * Support: 0% (obsolete)
 * Error case:
 * Results:
 *
 *
 * Side effects:
 *
 *
 *-----------------------------------------------------------------------------
 */
int
LinuxIdent_Setgroups16(uint32 gidsetsize,
                       UserVA /* LinuxGID16* */ gidset)
{
   UWLOG_SyscallUnsupported("use setgroups");
   return LINUX_ENOSYS;
}


/*
 *-----------------------------------------------------------------------------
 * 
 * LinuxIdent_Setresuid16 - 
 *	Handler for linux syscall 164 
 * Support: 0% (obsolete)
 * Error case:
 * Results:
 *
 *
 * Side effects:
 *
 *
 *-----------------------------------------------------------------------------
 */
int
LinuxIdent_Setresuid16(LinuxUID16 ruid, LinuxUID16 euid, LinuxUID16 suid)
{
   UWLOG_SyscallUnsupported("use setresuid (r=%d, e=%d, s=%d)",
         ruid, euid, suid);
   return LINUX_ENOSYS;
}


/*
 *-----------------------------------------------------------------------------
 * 
 * LinuxIdent_Getresuid16 - 
 *	Handler for linux syscall 165 
 * Support: 0% (obsolete)
 * Error case:
 * Results:
 *
 *
 * Side effects:
 *
 *
 *-----------------------------------------------------------------------------
 */
int
LinuxIdent_Getresuid16(UserVA /* LinuxUID16* */ ruid,
                       UserVA /* LinuxUID16* */ euid,
                       UserVA /* LinuxUID16* */ suid)
{
   UWLOG_SyscallUnsupported("use getresuid");
   return LINUX_ENOSYS;
}


/*
 *-----------------------------------------------------------------------------
 * 
 * LinuxIdent_Setresgid16 - 
 *	Handler for linux syscall 170 
 * Support: 0% (obsolete)
 * Error case:
 * Results:
 *
 *
 * Side effects:
 *
 *
 *-----------------------------------------------------------------------------
 */
int
LinuxIdent_Setresgid16(LinuxGID16 rgid, LinuxGID16 egid, LinuxGID16 sgid)
{
   UWLOG_SyscallUnsupported("use setresgid (r=%d, e=%d, s=%d)",
         rgid, egid, sgid);
   return LINUX_ENOSYS;
}

/*
 *-----------------------------------------------------------------------------
 * 
 * LinuxIdent_Getresgid16 - 
 *	Handler for linux syscall 171 
 * Support: 0% (obsolete)
 * Error case:
 * Results:
 *
 *
 * Side effects:
 *
 *
 *-----------------------------------------------------------------------------
 */
int
LinuxIdent_Getresgid16(UserVA /* LinuxGID16* */ rgid,
                       UserVA /* LinuxGID16* */ egid,
                       UserVA /* LinuxGID16* */ sgid)
{
   UWLOG_SyscallUnsupported("use getresgid");
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * LinuxIdent_Setfsuid16 - 
 *	Handler for linux syscall 138 
 * Support: 0% (obsolete)
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
int
LinuxIdent_Setfsuid16(LinuxUID16 uid)
{
   UWLOG_SyscallUnsupported("use 32-bit version uid=%d", uid);
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * LinuxIdent_Setfsgid16 - 
 *	Handler for linux syscall 139 
 * Support: 0% (obsolete)
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
int
LinuxIdent_Setfsgid16(LinuxGID16 gid)
{
   UWLOG_SyscallUnsupported("use 32-bit version");
   return LINUX_ENOSYS;
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxIdent_Setfsuid - 
 *	Handler for linux syscall 215 
 * Support: 0% (obscure, linux-specific)
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
int
LinuxIdent_Setfsuid(LinuxUID uid)
{
   UWLOG_SyscallUnsupported("uid=%d", uid);
   return LINUX_ENOSYS;
}

/*
 *----------------------------------------------------------------------
 *
 * LinuxIdent_Setfsgid - 
 *	Handler for linux syscall 216 
 * Support: 0% (obscure, linux-specific)
 * Error case:
 *
 * Results:
 *
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */
int
LinuxIdent_Setfsgid(LinuxGID gid)
{
   UWLOG_SyscallUnsupported("gid=%d", gid);
   return LINUX_ENOSYS;
}
