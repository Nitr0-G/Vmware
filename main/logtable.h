/* **********************************************************
 * Copyright 2002 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * Table of log levels, to be included as needed (typically twice:
 * for the enum declaration, and for a string table for printing).
 *
 * The first parameter to LOGLEVEL_DEF is the loglevel name, the second
 * is the default log level.
 */

LOGLEVEL_DEF(Action, 0)
LOGLEVEL_DEF(Alloc, 0)
LOGLEVEL_DEF(APIC, 0)
LOGLEVEL_DEF(BH, 0)
LOGLEVEL_DEF(Bluescreen, 0)
LOGLEVEL_DEF(Chipset, 0)
LOGLEVEL_DEF(Cow, 0)
LOGLEVEL_DEF(CpuSched, 0)
LOGLEVEL_DEF(Dump, 0)
LOGLEVEL_DEF(DVGA, 0)
LOGLEVEL_DEF(FS, 0)
LOGLEVEL_DEF(FS1, 0)
LOGLEVEL_DEF(FS2, 0)	
LOGLEVEL_DEF(FS3, 0)
LOGLEVEL_DEF(Vol3, 0)
LOGLEVEL_DEF(SFD3, 0)
LOGLEVEL_DEF(DIR3, 0)
LOGLEVEL_DEF(FIL3, 0)
LOGLEVEL_DEF(RDM3, 0)
LOGLEVEL_DEF(FSS, 0)
LOGLEVEL_DEF(VC, 0)
LOGLEVEL_DEF(OC, 0)
LOGLEVEL_DEF(FSN, 0)
LOGLEVEL_DEF(FDS, 0)
LOGLEVEL_DEF(FSDisk, 0)
LOGLEVEL_DEF(FSMem, 0)
LOGLEVEL_DEF(HeapMgr, 0)
LOGLEVEL_DEF(Helper, 0)
LOGLEVEL_DEF(Host, 0)
LOGLEVEL_DEF(IDT, 0)
LOGLEVEL_DEF(Im, 0)
LOGLEVEL_DEF(Init, 0)
LOGLEVEL_DEF(IOAPIC, 0)
LOGLEVEL_DEF(IRQ, 0)
LOGLEVEL_DEF(IT, 0)
LOGLEVEL_DEF(Kseg, 0)
LOGLEVEL_DEF(KVMap, 0)
LOGLEVEL_DEF(Log, 0)
LOGLEVEL_DEF(LinuxFileDesc, 0)
LOGLEVEL_DEF(LinuxIdent, 0)
LOGLEVEL_DEF(LinuxMem, 0)
LOGLEVEL_DEF(LinuxSignal, 0)
LOGLEVEL_DEF(LinuxSocket, 0)
LOGLEVEL_DEF(LinuxThread, 0)
LOGLEVEL_DEF(LinuxTime, 0)
LOGLEVEL_DEF(MCE, 0)
LOGLEVEL_DEF(Mem, 0)
LOGLEVEL_DEF(MemMap, 0)
LOGLEVEL_DEF(MemSched, 0)
LOGLEVEL_DEF(Mod, 0)
LOGLEVEL_DEF(MPage, 0)
LOGLEVEL_DEF(Net, 0)
LOGLEVEL_DEF(NF, 0)
LOGLEVEL_DEF(NMI, 0)
LOGLEVEL_DEF(NUMA, 0)
LOGLEVEL_DEF(Partition, 0)
LOGLEVEL_DEF(PCI, 0)
LOGLEVEL_DEF(PIC, 0)
LOGLEVEL_DEF(Post, 0)
LOGLEVEL_DEF(Proc, 0)
LOGLEVEL_DEF(PShare, 0)
LOGLEVEL_DEF(RPC, 0)
LOGLEVEL_DEF(RTC, 0)
LOGLEVEL_DEF(SCSI, 0)
LOGLEVEL_DEF(Serial, 0)
LOGLEVEL_DEF(SMP, 0)
LOGLEVEL_DEF(SP, 0)
LOGLEVEL_DEF(Swap, 0)
LOGLEVEL_DEF(ThermMon, 0)
LOGLEVEL_DEF(Timer, 0)
LOGLEVEL_DEF(TimeStamp, 0)
LOGLEVEL_DEF(TLB, 0)
LOGLEVEL_DEF(User, 0)
LOGLEVEL_DEF(UserDebug, 0)
LOGLEVEL_DEF(UserDump, 0)
LOGLEVEL_DEF(UserFile, 0)
LOGLEVEL_DEF(UserHost, 0)
LOGLEVEL_DEF(UserIdent, 0)
LOGLEVEL_DEF(UserInit, 0)
LOGLEVEL_DEF(UserLinux, 0)
LOGLEVEL_DEF(UserLog, 0)
LOGLEVEL_DEF(UserMem, 0)
LOGLEVEL_DEF(UserObj, 0)
LOGLEVEL_DEF(UserPipe, 0)
LOGLEVEL_DEF(UserProxy,0)
LOGLEVEL_DEF(UserPTE, 0)
LOGLEVEL_DEF(UserRoot, 0)
LOGLEVEL_DEF(UserRpc, 0)
LOGLEVEL_DEF(UserSig, 0)
LOGLEVEL_DEF(UserStat, 0)
LOGLEVEL_DEF(UserStdio, 0)
LOGLEVEL_DEF(UserSocket, 0)
LOGLEVEL_DEF(UserSocketInet, 0)
LOGLEVEL_DEF(UserSocketUnix, 0)
LOGLEVEL_DEF(UserTerm, 0)
LOGLEVEL_DEF(UserThread, 0)
LOGLEVEL_DEF(UserTime, 0)
LOGLEVEL_DEF(UserProcDebug, 0)
LOGLEVEL_DEF(UWVMKDispatch, 0)
LOGLEVEL_DEF(UWVMKSyscall, 0)
LOGLEVEL_DEF(Util, 0)
LOGLEVEL_DEF(Vmkperf, 0)
LOGLEVEL_DEF(VmkStats, 0)
LOGLEVEL_DEF(VmkEvent, 0)
LOGLEVEL_DEF(Watchpoint, 0)
LOGLEVEL_DEF(World, 0)
LOGLEVEL_DEF(NetTOE, 0)
LOGLEVEL_DEF(Histogram, 0)
LOGLEVEL_DEF(Parse, 0)
LOGLEVEL_DEF(NUMASched, 0)
LOGLEVEL_DEF(VmkTag, 0)
LOGLEVEL_DEF(XMap, 0)
LOGLEVEL_DEF(NetDiscover, 0)
LOGLEVEL_DEF(NetTest, 0)
LOGLEVEL_DEF(VmkStress, 0)
LOGLEVEL_DEF(Buddy, 0)
LOGLEVEL_DEF(Tcpip,0)
LOGLEVEL_DEF(TcpipLoader,0)
LOGLEVEL_DEF(PT, 0)
LOGLEVEL_DEF(Compress,0)
LOGLEVEL_DEF(VSI, 0)
LOGLEVEL_DEF(CpuMetrics, 0)
LOGLEVEL_DEF(MemMetrics, 0)
LOGLEVEL_DEF(Tso, 0)
LOGLEVEL_DEF(Vlan, 0)
LOGLEVEL_DEF(Migrate, 0)
LOGLEVEL_DEF(MigrateLog, 0)
LOGLEVEL_DEF(MigrateNet, 0)
LOGLEVEL_DEF(MigrateQueue, 0)
LOGLEVEL_DEF(EventHisto, 0)
LOGLEVEL_DEF(SharedArea, 0)
LOGLEVEL_DEF(Trace, 0)
LOGLEVEL_DEF(Uplink, 0)
LOGLEVEL_DEF(NFS, 0)
LOGLEVEL_DEF(Heap, 0)
LOGLEVEL_DEF(ISA, 0)
LOGLEVEL_DEF(MPS, 0)
LOGLEVEL_DEF(Acpi, 0)
LOGLEVEL_DEF(Sched, 0)
LOGLEVEL_DEF(Summit, 0)
LOGLEVEL_DEF(Conduit, 0)
LOGLEVEL_DEF(CnDev, 0)
LOGLEVEL_DEF(ConduitDefault, 0)
LOGLEVEL_DEF(Infiniband, 0)
LOGLEVEL_DEF(VMKKBD, 0)
LOGLEVEL_DEF(VGA, 0)
LOGLEVEL_DEF(Keyboard, 0)
LOGLEVEL_DEF(Term, 0)
LOGLEVEL_DEF(LogTerm, 0)
LOGLEVEL_DEF(VSCSI, 0)
LOGLEVEL_DEF(VSCSIFs, 0)
LOGLEVEL_DEF(VSCSICow, 0)
LOGLEVEL_DEF(VSCSIRaw, 0)
LOGLEVEL_DEF(VSCSIRdm, 0)
LOGLEVEL_DEF(Heartbeat, 0)
LOGLEVEL_DEF(Config, 0)
#undef LOGLEVEL_DEF
