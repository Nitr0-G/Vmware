INCLUDE += -I$(SRCROOT)/vmkernel/filesystems

SUBDIR_FILES = fsSwitch.c fsDeviceSwitch.c fsNameSpace.c cow.c \
	       volumeCache.c diskDriver.c objectCache.c fsClientLib.c
