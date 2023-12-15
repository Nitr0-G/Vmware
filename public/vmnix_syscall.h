
#ifndef VMNIX_SYSCALL_H
#define VMNIX_SYSCALL_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "vmnix_syscall_dist.h"
#include "vm_basic_defs.h"
#include "vm_version.h"
#include "vmkernel_ext.h"
#include "fs_ext.h"
#include "scsi_ext.h"
#include "vscsi_ext.h"
#include "world_ext.h"
#include "net_public.h"
#include "sched_ext.h"
#include "memsched_defs.h"
#include "migrate_ext.h"
#include "vm_atomic.h"
#include "rateconv.h"
#include "vmkevent_ext.h"
#include "hardware_public.h"
#include "userProxy_ext.h"
#include "vcpuid.h"
#include "conduit_dist.h"
#include "CnDev_def.h"
#include "conduit_def.h"
#include "sharedAreaDesc.h"
#include "util_ext.h"
#include "vmkcfgopts_public.h"

#define SCSI_MAX_TARGETS        128
// Partition 0 is used to indicate whole tgt/lun
// fdisk will allow 16 usable partitions, and at most 1 extended partition
#define VMNIX_MAX_PARTITIONS     18
#define VMNIX_INQUIRY_LENGTH    256


/*
 * enumerate vmnix syscalls
 */
enum {
#define VMX_VMNIX_CALL(name, _ignore...) _PLACEHOLDER_VMNIX_##name,
#include "vmnix_sctable_dist.h"
#define VMX_VMNIX_CALL(name, _ignore...) VMNIX_##name,
#define VMX_VMNIX_EXTERN_CALL(name, _ignore...) VMNIX_##name,
#include "vmnix_sctable.h"
};

#define VMNIX_WORLD_NAME_LENGTH		64
#define VMNIX_SHAREDAREA_NAME_LEN       128

#define VMNIXNET_CREATE	  	(SIOCDEVPRIVATE + 8)
#define VMNIXNET_GET_MAC_ADDR	(SIOCDEVPRIVATE + 9)
#define VMNIXNET_SET_MAC_ADDR	(SIOCDEVPRIVATE + 10)

typedef struct VMnix_LoaderArgs {
   char			*buf;
   unsigned long	start;
   unsigned long	endReadOnly;
   unsigned long	startWritable;
   unsigned long	end;
   unsigned long	entry;
   VMnix_ConfigOptions  configOptions;
} VMnix_LoaderArgs;

typedef void (*VMnix_Entry)(void *args);

#define VMNIX_GROUP_LEADER	0x01
#define VMNIX_USER_WORLD	0x02

typedef struct VMnix_CreateWorldArgs {
   uint32	        flags;
   World_ID	        groupLeader;
   int		        vcpuid;
   SharedAreaArgs       sharedAreaArgs;
   char                 name[VMNIX_WORLD_NAME_LENGTH];
   Sched_ClientConfig   sched;
} VMnix_CreateWorldArgs;

typedef struct VMnix_BindWorldArgs {
   World_ID	groupLeader;
   int		vcpuid;
} VMnix_BindWorldArgs;

typedef struct VMnix_RunWorldArgs {
   World_ID             worldID;
   VMnix_Entry          start;
} VMnix_RunWorldArgs;

typedef struct VMnix_SetWorldArgArgs {
   World_ID     worldID;
   char 	arg[256];
} VMnix_SetWorldArgArgs;

typedef struct VMnix_ReadPageArgs {
   World_ID		worldID;
   unsigned long 	page;
} VMnix_ReadPageArgs;

typedef struct VMnix_ReadStackArgs {
   World_ID             worldID;
   unsigned long        page;
   VA                  *vAddr;
} VMnix_ReadStackArgs;

typedef struct VMnix_AddPageArgs {
   World_ID		worldID;
   unsigned long	vpn;
   unsigned long	mpn;
   int			readOnly;
} VMnix_AddPageArgs;

typedef struct VMnix_ReadRegsResult {
   long ebx;
   long ecx;
   long edx;
   long esi;
   long edi;
   long ebp;
   long eax;
   int  cs;   
   int  ds;
   int  es;
   int  ss;   
   long eip;
   long eflags;
   long esp;
} VMnix_ReadRegsResult;

typedef struct VMnix_SetMMapLastArgs {
   World_ID     worldID;
   uint32	endMapOffset;
} VMnix_SetMMapLastArgs;

typedef struct VMnix_NetConnectArgs {
   World_ID		worldID;
   char 		name[VMNIX_DEVICE_NAME_LENGTH];
} VMnix_NetConnectArgs;

typedef struct VMnix_NetConnectResult {
   Net_PortID	portID;
} VMnix_NetConnectResult;

typedef struct VMnix_NetPortEnableArgs {
   Net_PortID   portID;
   uint32       paddr;
   uint32       length;
} VMnix_NetPortEnableArgs;

typedef struct VMnix_NetPortDisableArgs {
   Net_PortID portID;
} VMnix_NetPortDisableArgs;

typedef struct VMnix_NetDisconnectArgs {
   World_ID 	  worldID;
   Net_PortID	  portID; 
} VMnix_NetDisconnectArgs;

typedef struct VMnix_SetMACAddrArgs {
   World_ID 		worldID;
   Net_PortID		portID;
   unsigned char 	macAddr[6];
} VMnix_SetMACAddrArgs;

typedef struct VMnix_OpenSCSIDevArgs {
   World_ID worldID;
   char name[VMNIX_DEVICE_NAME_LENGTH];
   uint32 targetID;
   uint32 lun;
   uint32 partition;
   uint32 shares;
   uint32 flags;
} VMnix_OpenSCSIDevArgs;

typedef struct VMnix_OpenSCSIDevIntResult {
   SCSI_HandleID handleID;
   int16 cmplMapIndex;
} VMnix_OpenSCSIDevIntResult;

typedef struct VMnix_CloseSCSIDevArgs {
   World_ID worldID;
   SCSI_HandleID handleID;
} VMnix_CloseSCSIDevArgs;


typedef struct VMnix_FileGetPhysLayoutResult {
   uint8 diskIdType;
   uint8 diskIdLength;
   uint8 diskId[SCSI_DISK_ID_LEN];
   uint32 lun;
   uint64 start;
   uint64 length;
} VMnix_FileGetPhysLayoutResult;

typedef struct VMnix_FileGetPhysLayoutArgs {
   FS_FileHandleID fileHandleID;
   uint64 offset;
} VMnix_FileGetPhysLayoutArgs;


typedef struct VMnix_FileOpenArgs {
   FSS_ObjectID oid;
   int flags;
   uint16 uid; // uid/gid/mode are only used for fileopen_create or replace
   uint16 gid;
   uint16 mode;
} VMnix_FileOpenArgs;

typedef struct VMnix_FileRemoveArgs {
   FSS_ObjectID dirOID;
   char fileName[FS_MAX_FILE_NAME_LENGTH];
} VMnix_FileRemoveArgs;

typedef struct VMnix_FileOpenResult {
   FS_FileHandleID handleID;
} VMnix_FileOpenResult;

typedef struct VMnix_FileCloseArgs {
   FS_FileHandleID handleID;
} VMnix_FileCloseArgs;

typedef struct VMnix_LookupMPNArgs {
   World_ID worldID;
   VPN userVPN;
} VMnix_LookupMPNArgs;

typedef struct VMnix_GetNextAnonPageArgs{
   World_ID worldID;
   MPN inMPN;
} VMnix_GetNextAnonPageArgs;

typedef struct VMnix_GetNextAnonPageResult {
   MPN mpn;
} VMnix_GetNextAnonPageResult;

typedef struct VMnix_DevArgs {
   unsigned 	int bus;
   unsigned 	int slot;
   unsigned 	int func;
   Bool		toVMKernel;
   Bool		hotplug;
   char		name[VMNIX_DEVICE_NAME_LENGTH];
} VMnix_DevArgs;

typedef struct VMnix_DevResult {
   Bool		  present;
   Bool		  VMKernel;
   unsigned short vendor;
   unsigned short device;
   unsigned short subVendor;
   unsigned short subDevice;
   char		  name[VMNIX_DEVICE_NAME_LENGTH];
   char		  description[80];
} VMnix_DevResult;

typedef struct VMnix_RegisterSCSISpecArgs {
   char   resourcePath[FS_MAX_PATH_NAME_LENGTH];
   Bool   exclusive; // exclusive -> r/w, non-exclusive: r/o
} VMnix_RegisterSCSISpecArgs;

typedef struct VMnix_NetInfoArgs {
   void 	*arg1;
   void 	*arg2;
   void 	*data;
   uint32 	dataLength;
   World_ID	worldID;
} VMnix_NetInfoArgs;

#define VMNIX_NET_GET_ADAPTER_LIST		1
#define VMNIX_NET_GET_ADAPTER_STATS		2
#define VMNIX_NET_GET_MAC_ADDRS			3
#define VMNIX_NET_GET_ADAPTER_HANDLE_STATS	4

typedef struct VMnix_AdapterInfo {
   char vmkName[VMNIX_DEVICE_NAME_LENGTH];
   char cosName[VMNIX_DEVICE_NAME_LENGTH];
   uint32 qDepth;
   char driverName[VMNIX_MODULE_NAME_LENGTH];
} VMnix_AdapterInfo;

typedef struct VMnix_AdapterListArgs {
   uint32 maxEntries;
} VMnix_AdapterListArgs; 

typedef struct VMnix_AdapterListResult {
   uint32 numAdapters;
   uint32 numReturned;
   VMnix_AdapterInfo list[1];
} VMnix_AdapterListResult;

#define VMNIX_SCSIADAPTERLIST_RESULT_SIZE(_num) \
   sizeof(VMnix_AdapterListResult) + ((_num - 1) * sizeof(VMnix_AdapterInfo))

typedef struct VMnix_PartitionInfo {
   uint32 number;
   uint32 start;          
   uint32 nsect;
   uint32 type;
} VMnix_PartitionInfo; 

typedef struct VMnix_TargetInfoArgs {
   char diskName[VMNIX_DEVICE_NAME_LENGTH];
   uint32 targetID;
   uint32 lun;
   uint32 partition;
} VMnix_TargetInfoArgs;

typedef struct VMnix_TargetInfo {
   int16		targetID;
   int16		lun;
   uint8		inquiryInfo[VMNIX_INQUIRY_LENGTH];
   uint8                devClass;
   int32		queueDepth;
   uint32		blockSize;
   uint32		numBlocks;
   SCSI_Geometry        geometry;
   VMnix_PartitionInfo  partitionInfo[VMNIX_MAX_PARTITIONS];
   int32		numPartitions;
   SCSI_DiskId		diskId;
   Bool                 invalid;
} VMnix_TargetInfo;

typedef struct VMnix_TargetInfo VMnix_LUNInfo;

typedef struct VMnix_LUNListArgs {
   char adapterName[VMNIX_DEVICE_NAME_LENGTH];
   uint32 maxEntries;
} VMnix_LUNListArgs; 

typedef struct VMnix_LUNListResult {
   uint32 numLUNs;
   uint32 numReturned;
   VMnix_LUNInfo list[1];
} VMnix_LUNListResult; 

#define VMNIX_LUNLIST_RESULT_SIZE(_num) \
   sizeof(VMnix_LUNListResult) + ((_num - 1) * sizeof(VMnix_LUNInfo))

typedef struct VMnix_LUNPath {
   char adapterName[VMNIX_DEVICE_NAME_LENGTH];
   uint16 targetID;
   uint16 lun;
   uint8 state;
   Bool active;
   Bool preferred;
} VMnix_LUNPath;

typedef struct VMnix_LUNPathArgs {
   char adapterName[VMNIX_DEVICE_NAME_LENGTH];
   uint16 targetID;
   uint16 lun;
   uint32 maxEntries;
} VMnix_LUNPathArgs; 

typedef struct VMnix_LUNPathResult {
   uint32 numPaths;
   uint32 numReturned;
   uint8 pathPolicy;
   VMnix_LUNPath list[1];
} VMnix_LUNPathResult; 

#define VMNIX_LUNPATHLIST_RESULT_SIZE(_num) \
   sizeof(VMnix_LUNPathResult) + ((_num - 1) * sizeof(VMnix_LUNPath))

typedef struct VMnix_PartitionStats {
   uint32 number;
   SCSI_Stats stats;
} VMnix_PartitionStats;

typedef struct VMnix_LUNStatsResult {
   SCSI_Stats stats;
   int32 numPartitions;
   VMnix_PartitionStats partitionStats[VMNIX_MAX_PARTITIONS];
} VMnix_LUNStatsResult;

typedef struct VMnix_TargetInfoArgs VMnix_LUNStatsArgs;

typedef struct VMnix_GetCapacityResult {
   uint32 diskBlockSize;
   uint32 numDiskBlocks;
   uint32 heads;
   uint32 sectors;
   uint32 cylinders;
   uint32 startSector;
} VMnix_GetCapacityResult;

typedef struct VMnix_ConvertToFS2Args {
   char volumeName[FS_MAX_VOLUME_NAME_LENGTH];
} VMnix_ConvertToFS2Args;

typedef struct VMnix_FSCreateArgs {
   char fsType[FSS_MAX_FSTYPE_LENGTH];
   char deviceName[VMNIX_DEVICE_NAME_LENGTH];
   uint32 fileBlockSize;
   uint32 numFiles;
} VMnix_FSCreateArgs;

typedef struct VMnix_FSExtendArgs {
   char volumeName[FS_MAX_VOLUME_NAME_LENGTH];
   char extVolumeName[FS_MAX_VOLUME_NAME_LENGTH];
   uint32 numFiles;
} VMnix_FSExtendArgs;

typedef struct VMnix_FSGetAttrArgs {
   FSS_ObjectID oid;  /* OID of an object within the volume */
   uint32 maxPartitions;
} VMnix_FSGetAttrArgs;

typedef struct VMnix_FSSetAttrArgs {
   char volumeName[FS_MAX_VOLUME_NAME_LENGTH];
   int flags;
   char fsName[FS_MAX_FS_NAME_LENGTH];
   int mode;
} VMnix_FSSetAttrArgs;

typedef struct VMnix_PEAddress {
   char peName[FS_MAX_VOLUME_NAME_LENGTH];
} VMnix_PEAddress;

#define VMNIX_PARTITION_ARR_SIZE(numParts)      \
   (sizeof(VMnix_PartitionListResult) +         \
    (((numParts) > 0 ? ((numParts) - 1) : 0) * sizeof(VMnix_PEAddress)))

/* "Safe" maximum number of partitions to request. */
#define VMNIX_PLIST_DEF_MAX_PARTITIONS 32

typedef struct VMnix_PartitionListResult {
   uint32		diskBlockSize;
   uint64		numDiskBlocks;
   uint32		fileBlockSize;
   uint32		numFileBlocks;
   uint32		numFileBlocksUsed;

   uint32               mtime;
   uint32               ctime;
   uint32               atime;
   Bool			readOnly;
   uint32 		versionNumber;
   uint8		minorVersion;
   FSS_ObjectID         rootDirOID;
   char			name[FS_MAX_FS_NAME_LENGTH];

   /* VMFS-specific information follows. */
   UUID 		uuid;
   int			config;
   uint8 		numPhyExtents;
   uint8		numPhyExtentsReturned;

   /*
    * 'peAddresses' is the beginning of an n-element array. When
    * allocating, use VMNIX_PARTITION_ARR_SIZE() to compute the total
    * size of the structure.
    */
   uint32               ioctlMaxPartitions;  // used for COS ioctl() arg. passing
   VMnix_PEAddress 	peAddresses[1];
} VMnix_PartitionListResult;

typedef struct VMnix_FileLookupArgs {
   FSS_ObjectID dirOID;
   char fileName[FS_MAX_FILE_NAME_LENGTH];
} VMnix_FileLookupArgs;

typedef struct VMnix_FileLookupResult {
   FSS_ObjectID oid;
   FS_FileAttributes attrs;
} VMnix_FileLookupResult;

typedef struct VMnix_ReaddirArgs {
   FSS_ObjectID dirOID;
   uint32 maxDirEntries;
} VMnix_ReaddirArgs;

typedef struct VMnix_FileDesc {
   uint64		length;
   uint32		fsBlockSize;	// Block size of file system
   uint32		numBlocks;	// Number of blocks used by file
   FS_DescriptorFlags	flags;
   uint32               descNum;
   uint32               mtime;
   uint32               ctime;
   uint32               atime;
   uint16               uid;
   uint16               gid;
   uint16               mode;
} VMnix_FileDesc;

typedef struct VMnix_DirEntry {
   char 	        fileName[FS_MAX_FILE_NAME_LENGTH];
   FS_DescriptorFlags	flags;
   uint32               descNum;
} VMnix_DirEntry;

#define VMNIX_READDIR_RESULT_SIZE(numDents) \
   (sizeof(VMnix_ReaddirResult) +  \
   ((numDents) - 1) * sizeof(VMnix_DirEntry))

typedef struct VMnix_ReaddirResult {
   uint32		totalNumDirEntries;
   uint32		numDirEntriesReturned;
   uint32               mtime;
   uint32               ctime;
   uint32               atime;
   VMnix_DirEntry	dirent[1];
} VMnix_ReaddirResult;

typedef struct VMnix_FSDumpArgs {
   char path[FS_MAX_PATH_NAME_LENGTH];
   Bool verbose;
} VMnix_FSDumpArgs;

typedef struct VMnix_MapRawDiskArgs {
   char   filePath[FS_MAX_PATH_NAME_LENGTH];
   char   rawDiskName[VMNIX_DEVICE_NAME_LENGTH];
   uint32 rawTargetID;
   uint32 rawLun;
   uint32 rawPartition;  
   uint16 uid;
   uint16 gid;
   uint16 mode;
} VMnix_MapRawDiskArgs;

typedef struct VMnix_QueryRawDiskArgs {
   char resourcePath[FS_MAX_PATH_NAME_LENGTH];
} VMnix_QueryRawDiskArgs;

typedef struct VMnix_QueryRawDiskResult {
   SCSI_DiskId diskId;
   char rawDiskName[VMNIX_DEVICE_NAME_LENGTH];
   uint32 rawTargetID;
   uint32 rawLun;
   uint32 rawPartition;
} VMnix_QueryRawDiskResult;

typedef struct VMnix_FileCreateArgs {
   FSS_ObjectID dirOID;
   char fileName[FS_MAX_FILE_NAME_LENGTH];
   uint64 length;
   uint16 uid;
   uint16 gid;
   uint16 mode;
   uint32 createFlags;
} VMnix_FileCreateArgs;

typedef struct VMnix_VSCSICreateDevArgs {
   World_ID             wid;
   VSCSI_DevDescriptor  desc;
} VMnix_VSCSICreateDevArgs;

typedef struct VMnix_VSCSIDestroyDevArgs {
   World_ID         wid;
   VSCSI_HandleID   vscsiID;
} VMnix_VSCSIDestroyDevArgs;

typedef VSCSI_HandleID VMnix_VSCSICreateDevResult;

typedef struct VMnix_COWOpenHierarchyArgs {
   FS_FileHandleID fids[COW_MAX_REDO_LOG];
   int numFids;
} VMnix_COWOpenHierarchyArgs;

typedef struct VMnix_COWOpenHierarchyResult {
   COW_HandleID cowHandleID;
} VMnix_COWOpenHierarchyResult;

typedef struct VMnix_COWCombineArgs {
   COW_HandleID cowHandleID;
   int linkOffsetFromBottom;
} VMnix_COWCombineArgs;

typedef struct VMnix_COWGetFIDAndLBNArgs {
   COW_HandleID cowHandle;
   uint64 blockOffset;
} VMnix_COWGetFIDAndLBNArgs;

typedef struct VMnix_COWGetFIDAndLBNResult {
   FS_FileHandleID fileHandle;
   uint64 actualBlockNumber; 
   uint32 length;
} VMnix_COWGetFIDAndLBNResult;

typedef struct VMnix_AddRedoLogArgs {
   VSCSI_HandleID handleID;
   FS_FileHandleID handle;
} VMnix_AddRedoLogArgs;

typedef struct VMnix_FileAttrArgs {
   FSS_ObjectID oid;
} VMnix_FileAttrArgs;

typedef VMnix_FileDesc VMnix_FileAttrResult;

typedef struct VMnix_FileSetAttrArgs {
   FSS_ObjectID oid;
   uint32 generation;
   uint64 length;
   Bool cowFile;
   Bool swapFile;
   int opFlags;
   uint16 uid;
   uint16 gid;
   uint16 mode;
   ToolsVersion toolsVersion;
   Bool diskImage;
   uint32 virtualHWVersion;
} VMnix_FileSetAttrArgs;

typedef struct VMnix_ActivateSwapFileArgs {
   char filePath[FS_MAX_PATH_NAME_LENGTH];
} VMnix_ActivateSwapFileArgs;

typedef struct VMnix_FileIOArgs {
   FSS_ObjectID oid;
   uint64 offset;
   uint32 length;
   uint64 buf;
   Bool isRead;
} VMnix_FileIOArgs;

typedef struct VMnix_FilePhysMemIOArgs {
   FS_FileHandleID handleID;
   uint64 offset;
   World_ID worldID;
   Bool read;
   int startPercent, endPercent;
} VMnix_FilePhysMemIOArgs;

typedef struct VMnix_MarkCheckpointArgs {
   World_ID worldID;
   Bool wakeup;
   Bool start;
} VMnix_MarkCheckpointArgs;

#define MAX_MIG_DATA_IO_SIZE		16384

typedef struct VMnix_MigrationArgs {
   uint64 ts;
   uint32 srcIpAddr;
   uint32 destIpAddr;
   World_ID worldID;
   World_ID destWorldID;
   Bool grabResources;
} VMnix_MigrationArgs;

typedef struct VMnix_MigCptDataArgs {
   World_ID worldID;
   int	offset;
   int 	size;
   void *data;
   Bool completed;
} VMnix_MigCptDataArgs;

typedef struct VMnix_MigrateProgressArgs {
   World_ID worldID;
   uint64 ts;
   uint32 srcVmkIpAddr;
} VMnix_MigrateProgressArgs;

typedef struct VMnix_MigrateProgressResult {
   MigrateState state;
   VMK_ReturnStatus errorCode;
   int preCopyPhase;
   int pagesModified;
   int pagesXferred;
} VMnix_MigrateProgressResult;

typedef struct VMnix_FileRenameArgs {
   FSS_ObjectID oldDirOID;
   char oldFileName[FS_MAX_FILE_NAME_LENGTH];
   FSS_ObjectID newDirOID;
   char newFileName[FS_MAX_FILE_NAME_LENGTH];
} VMnix_FileRenameArgs;

typedef struct VMnix_LUNReserveArgs {
   char diskName[VMNIX_DEVICE_NAME_LENGTH];
   uint32 targetID;
   uint32 lun;
   uint32 partition;
   Bool reserve;
   Bool reset;
   Bool lunreset;
} VMnix_LUNReserveArgs;

typedef VMnix_SetWorldArgArgs VMnix_SetWorldWDArgs;

typedef struct VMnix_DoMemMapArgs {
   World_ID worldID;
   VA startUserVA;
} VMnix_DoMemMapArgs;

typedef struct VMnix_MapSharedArea {
   World_ID worldID;
   VA startUserVA;
   uint32 length;
} VMnix_MapSharedArea;

typedef struct VMnix_ProcArgs {
   char adapName[VMNIX_DEVICE_NAME_LENGTH];
   char *vmkBuf;
   uint32 offset;
   uint32 count;
   int isWrite;
} VMnix_ProcArgs;

typedef struct VMnix_ProcResult {
   uint32 nbytes; 
} VMnix_ProcResult;

typedef struct VMnix_NICStatsArgs {
   char devName[VMNIX_DEVICE_NAME_LENGTH];
   uint32 resultLen;
} VMnix_NICStatsArgs;

typedef struct VMnix_SCSIDevIoctlArgs {
   SCSI_HandleID handleID;
   uint32 cmd;
   uint32 userArgsPtr;
   uint32 hostFileFlags;
} VMnix_SCSIDevIoctlArgs; 

typedef struct VMnix_IoctlResult {
   int32 drvErr;
} VMnix_IoctlResult; 

typedef VMnix_IoctlResult VMnix_SCSIDevIoctlResult;

typedef struct VMnix_CharDevIoctlArgs {
   uint32 major;
   uint32 minor;
   uint32 cmd;
   uint32 userArgsPtr;
   uint32 hostFileFlags;
} VMnix_CharDevIoctlArgs; 

typedef VMnix_IoctlResult VMnix_CharDevIoctlResult;

typedef struct VMnix_NetDevIoctlArgs {
   char devName[VMNIX_DEVICE_NAME_LENGTH];
   int cmd;
   char *vmkBuf;
} VMnix_NetDevIoctlArgs;

typedef VMnix_IoctlResult VMnix_NetDevIoctlResult; 

typedef struct VMnix_DelaySCSICmds {
   World_ID worldID;
   uint32 delay;
} VMnix_DelaySCSICmds;

typedef struct VMnix_VMXInfoArgs {
   World_ID     worldID;
   char         cfgPath[WORLD_MAX_CONFIGFILE_SIZE];
   char		uuidString[WORLD_MAX_UUIDTEXT_SIZE];
   char		displayName[WORLD_MAX_DISPLAYNAME_SIZE];
   uint32       vmxPID;
} VMnix_VMXInfoArgs;

typedef struct VMnix_ScanAdapterArgs {
   char adapterName[VMNIX_DEVICE_NAME_LENGTH];
   Bool VMFSScanOnly;
} VMnix_ScanAdapterArgs;

typedef struct VMnix_HotAddMemory {
   uint64 start;
   uint64 size;
   uint32 attrib;
} VMnix_HotAddMemory;

typedef struct VMnix_CreateConduitAdapArgs {
   World_ID		worldID;
   char 		name[VMNIX_DEVICE_NAME_LENGTH];
   ConduitInfo		conduitInfo;
} VMnix_CreateConduitAdapArgs;

/* count bits for virtual adapter number */
#define CONDUIT_OPEN_VADAPTER_MASK 0xf

typedef struct VMnix_CreateConduitAdapResult {
   Conduit_HandleID	handleID;
} VMnix_CreateConduitAdapResult;

typedef struct VMnix_RemoveConduitAdapArgs {
   World_ID	 	worldID;
   Conduit_HandleID	handleID; 
} VMnix_RemoveConduitAdapArgs;

typedef struct VMnix_ConduitEnableArgs {
   World_ID		 	worldID;
   Conduit_HandleID		handleID;
   Conduit_HandleEnableArgs	*args;
} VMnix_ConduitEnableArgs;

typedef struct VMnix_ConduitDeviceMemoryArgs {
   Conduit_HandleID		handleID;
   Conduit_DeviceMemoryCmd	cmd;
} VMnix_ConduitDeviceMemoryArgs;

typedef struct VMnix_ConduitRemovePipeArgs {
   Conduit_HandleID	handleID;
   World_ID		worldID;
   Conduit_HandleID	pipeID;
} VMnix_ConduitRemovePipeArgs;

typedef struct VMnix_ConduitNewPipeArgs {
   Conduit_HandleID	handleID;
   Conduit_OpenPipeArgs	args;
} VMnix_ConduitNewPipeArgs;

typedef struct VMnix_ConduitSendArgs {
   World_ID	 	worldID;
   Conduit_HandleID	handleID; 
} VMnix_ConduitSendArgs;

typedef struct VMnix_ConduitLockPageArgs {
   PageNum			p;
   Conduit_LockPageFlags	flags;
   MPN				mpn;
   World_ID			worldID;
} VMnix_ConduitLockPageArgs;

typedef struct VMnix_ConduitDevInfoArgs {
   World_ID		worldID;
   Conduit_HandleID	handleID;
   CnDev_Record		rec;
} VMnix_ConduitDevInfoArgs;

typedef struct VMnix_ConduitConfigDevForWorldArgs {
   World_ID		worldID;
   Conduit_HandleID	conduit;
   uint32		devID;
   uint32		flags;
   uint32		numNumerics;
   uint32		numStrings;
   CnDev_Numerics	*nBuf;
   CnDev_Strings	*sBuf;
} VMnix_ConduitConfigDevForWorldArgs;

typedef struct VMnix_ConduitConfigDevForWorldArgsReply {
   uint32		flags;
   uint32		numNumerics;
   uint32		numStrings;
   CnDev_Numerics	*nBuf;
   CnDev_Strings	*sBuf;
} VMnix_ConduitConfigDevForWorldArgsReply;

typedef struct Vmnix_SysInfoOldInfo {
   unsigned int funcId;
   unsigned int bufLen;
}Vmnix_SysInfoOldInfo;

#define VSI_CALLINFO_MAGIC 0x4d475349
typedef struct VSI_CallInfo {
   uint32 magic;
   uint32 nodeID;
   uint32 nInstanceArgs;

   // used by SET calls
   void *inputList;
   uint32 nInputArgs;
   uint32 inputArgsSize;

   // used by GETLIST calls
   uint32 nInstanceOutParams;
   uint32 outBytesToCopy;
} VSI_CallInfo;

typedef struct VMnix_SetMPNContents {
   uint8 buf[PAGE_SIZE];
   MPN	 mpn;
} VMnix_SetMPNContents;

typedef struct VMnix_SetBreakArgs {
   World_ID     worldID;
   uint32       brk;
} VMnix_SetBreakArgs;

typedef struct VMnix_SetLoaderArgs {
   World_ID	worldID;
   uint32	phdr;
   uint32	phent;
   uint32	phnum;
   uint32	base;
   uint32	entry;
} VMnix_SetLoaderArgs;

typedef struct VMnix_ForwardSignalArgs {
   World_ID	worldID;
   int		sig;
} VMnix_ForwardSignalArgs;

typedef struct VMnix_UserMapSectionArgs {
   World_ID	worldID;
   VA		addr;
   uint32	length;
   uint32	prot;
   uint32	flags;
   int		id;
   uint64	offset;
   VA		zeroAddr;
} VMnix_UserMapSectionArgs;

typedef struct VMnix_UserMapFileArgs {
   World_ID	worldID;
   int		id;
   /*
    * I'd like to use #define's from user_ext.h.  Unfortunately, it seems like
    * it'll be more trouble than its worth.
    */
   char		name[256];
} VMnix_UserMapFileArgs;

typedef struct VMnix_SetDumpArgs {
   char adapName[VMNIX_DEVICE_NAME_LENGTH];
   uint32 targetID;
   uint32 lun;
   uint32 partition;
} VMnix_SetDumpArgs;

typedef struct VMnix_HardwareInfoArgs {
} VMnix_HardwareInfoArgs;

typedef struct VMnix_HardwareInfoResult {
   Hardware_DMIUUID DMIUUID;
} VMnix_HardwareInfoResult;

typedef struct VMnix_SwapInfoArgs {
   uint32 fileIndex;
} VMnix_SwapInfoArgs;

typedef struct VMnix_SwapInfoResult {
   Bool valid;
   uint32 fileID;
   char filePath[FS_MAX_PATH_NAME_LENGTH];
   uint32 sizeMB;
   uint32 usedSizeMB;
} VMnix_SwapInfoResult;

typedef struct VMnix_ProxyObjReadyArgs {
   World_ID	cartelID;
   uint32	fileHandle;
   UserProxyPollCacheUpdate pcUpdate;
} VMnix_ProxyObjReadyArgs;

typedef struct VMnix_SetWorldIdentityArgs {
   World_ID     worldID;
   uint32       umask;  // not part of identity, but convenient to pass here
   uint32       ruid, euid, suid;
   uint32       rgid, egid, sgid;
   uint32       ngids;
   uint32       gids[USERPROXY_NGROUPS_MAX];
} VMnix_SetWorldIdentityArgs;

typedef struct VMnix_SetWorldDumpArgs {
   World_ID     worldID;
   uint32       enabled; 
} VMnix_SetWorldDumpArgs;

typedef struct VMnix_SetMaxEnvVarsArgs {
   World_ID     worldID;
   uint32       maxEnvVars;
} VMnix_SetMaxEnvVarsArgs;

typedef struct VMnix_AddEnvVarArgs {
   World_ID	worldID;
   char		*envVar;
   uint32	length;
} VMnix_AddEnvVarArgs;

typedef struct VMnix_CreateSpecialFdsArgs {
   World_ID	worldID;
   UserProxyObjType inType;
   UserProxyObjType outType;
   UserProxyObjType errType;
   Bool		vmkTerminal;
} VMnix_CreateSpecialFdsArgs;

typedef struct VMnix_MemMapInfoArgs {
} VMnix_MemMapInfoArgs;

typedef struct VMnix_MemMapInfoResult {
   uint32 totalPages;
   uint32 totalKernelPages;
   uint32 totalLowReservedPages;
   uint32 totalFreePages;
} VMnix_MemMapInfoResult;

typedef struct VMnix_CosPanicArgs {
   char hostMsg[256];
   uint32 logEnd;
   uint32 logBufLen;
   VA hdr;
   uint32 hdrLen;
   VA hostLogBuf;
   VMKFullExcFrame excFrame;
} VMnix_CosPanicArgs;

typedef struct VMnix_WantBreakpointArgs {
   World_ID worldID;
   Bool wantBreakpointNow;
}VMnix_WantBreakpointArgs;

typedef struct VMnix_FDS_MakeDevArgs {
   char name[VMNIX_DEVICE_NAME_LENGTH];
   char type[8];
   uint32 memBlockSize;
   uint32 numDiskBlocks;
   char* imagePtr;
} VMnix_FDS_MakeDevArgs;

typedef struct VMnix_SetExecNameArgs {
   World_ID worldID;
   /*
    * We really should use DUMP_EXEC_NAME_LENGTH instead of 512, but
    * unfortunately including dump_ext.h in this file causes lots of
    * compilation problems.  So leave as 512.
    */
   char execName[512];
} VMnix_SetExecNameArgs;

typedef struct VMnix_FDS_OpenDevArgs {
   char devName[VMNIX_DEVICE_NAME_LENGTH];
} VMnix_FDS_OpenDevArgs;

typedef struct VMnix_FDS_OpenDevResult {
   void *cookie; 
} VMnix_FDS_OpenDevResult;

typedef struct VMnix_FDS_CloseDevArgs {
   void *cookie; 
} VMnix_FDS_CloseDevArgs;

typedef struct VMnix_FDS_IOArgs {
   void *cookie;
   uint64 offset;
   uint32 length;
   uint64 cosBufMA;
   Bool isRead;
} VMnix_FDS_IOArgs;

typedef struct VMnix_FDS_IoctlArgs {
   void *cookie;
   uint32 cmd;
   void *result;
   uint32 resultSize;
} VMnix_FDS_IoctlArgs; 

typedef struct VMnix_SetCosPidArgs {
   World_ID worldID;
   int cosPid;
} VMnix_SetCosPidArgs;

// Flags for VMNIX_FILE_SET_ATTR
// Update the mask below (FILEATTR_FLAG_MASK) if you add/remove a flag.
#define FILEATTR_SET_COW		1
#define FILEATTR_SET_SWAP               2
#define FILEATTR_SET_GENERATION		4
#define FILEATTR_SET_LENGTH		8
#define FILEATTR_SET_PERMISSIONS	16
#define FILEATTR_SET_TOOLSVERSION	32
#define FILEATTR_SET_DISK_IMAGE         64
#define FILEATTR_SET_VIRTUALHWVERSION	128
#define FILEATTR_UPGRADEABLE_LOCK	256	//This is not a pure file
                                                //attribute. Its a locking
                                                //hint to FSx_SetFileAttributes()

#define FILEATTR_FLAG_MASK                                            \
   (FILEATTR_SET_COW | FILEATTR_SET_SWAP | FILEATTR_SET_GENERATION |  \
    FILEATTR_SET_LENGTH | FILEATTR_SET_PERMISSIONS |                  \
    FILEATTR_SET_TOOLSVERSION | FILEATTR_SET_DISK_IMAGE |             \
    FILEATTR_SET_VIRTUALHWVERSION | FILEATTR_UPGRADEABLE_LOCK)

// special-purpose VMFS ioctl commands
#define IOCTLCMD_FILE_GET_ATTR          202
#define IOCTLCMD_FILE_SET_ATTR          203
#define IOCTLCMD_FILE_GET_HANDLE        204

// Flags for VMNIX_FS_SET_ATTR
#define FSATTR_SET_NAME			1
#define FSATTR_SET_MODE			2

// Mode for VMNIX_FS_SET_ATTR
// Disk is private to a single server
#define FS_MODE_PRIVATE                       0       // Private to single server
// Disk is accessible by multiple servers, will be used to share
// virtual disks among VMs
#define FS_MODE_SHARED                        1
// Recover from a crash
#define FS_MODE_RECOVER                       2
// Go back to writable mode from the read-only mode of shared disks.
#define FS_MODE_WRITABLE              3
// Disk is accessible by multiple servers, but should only be accessed
// by one server at a time.
#define FS_MODE_PUBLIC                        4

#if SCSI_OPEN_MULTIPLE_WRITERS != FILEOPEN_WRITE
#error "SCSI_OPEN_MULTIPLE_WRITERS and FILEOPEN_WRITE should match!"
#endif

#if SCSI_OPEN_PHYSICAL_RESERVE != FILEOPEN_PHYSICAL_RESERVE
#error "SCSI_OPEN_PHYSICAL_RESERVE and FILEOPEN_PHYSICAL_RESERVE should match!"
#endif

#define FS_MAX_COMMIT_FRACTION		1000

typedef enum {
   FDS_IOCTL_INVALID = 0,

   //No data
   FDS_IOCTL_RESERVE_DEVICE,
   FDS_IOCTL_RELEASE_DEVICE,
   FDS_IOCTL_RESET_DEVICE,
   FDS_IOCTL_TIMEDWAIT,
   FDS_IOCTL_ABORT_COMMAND,
   FDS_IOCTL_RESET_COMMAND,

   //Get (query)
   FDS_IOCTL_GET_CAPACITY,
   FDS_IOCTL_GET_TARGETINFO,
   FDS_IOCTL_GET_PARTITION,

   //Set

} FDS_IoctlCmdType;


#endif


