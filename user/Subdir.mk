SUBDIR_FILES = user.c userLinux.c userDebug.c \
	userObj.c userSig.c userMem.c userThread.c \
	userPipe.c userFile.c userInit.c \
	userSocket.c userLog.c userProxy.c userDump.c \
	userIdent.c userSigDispatch.S userPTE.c \
	linuxFileDesc.c linuxMem.c linuxSignal.c \
	linuxSocket.c linuxThread.c linuxIdent.c \
	uwvmkDispatch.c uwvmkSyscall.c \
	userSocketInet.c userVMKRpc.c userStat.c \
	userTime.c linuxTime.c pseudotsc.S userProcDebug.c \
	userSocketUnix.c userTerm.c userCopy.S

# This is needed so that pseudotsc.S can #include "asmdefn.sinc", 
# a generated file made by vmkernel/main/genasmdefn.c.
INCLUDE += -Ivmkernel/main
