/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * config_dist.h --
 *
 *	vmkernel configuration options set from the host.
 */

#ifndef _CONFIG_DIST_H
#define _CONFIG_DIST_H

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_DISTRIBUTE
#define INCLUDE_ALLOW_VMK_MODULE
#define INCLUDE_ALLOW_MODULE
#include "includeCheck.h"

#include "vmware.h"

// XXX these do NOT belong here.  They lead to exactly the problem that this change is fixing.
VMK_ReturnStatus Init_VMKernelIDCallback(Bool write, Bool changed, int idx);

/*
 * Usage:
 *
 *   For integer config options: 
 *      D(_n, macroName, nodeName, min, max, default, help, <callback>, <isHidden>)
 *
 *   For string config options: 
 *      S(_n, macroName, name, default, valid, help, <callback>, <isHidden>) 
 *         'valid' is a string containing the list of valid input characters for 
 *         the option.  The special token "**" allows any input. 
 *          
 *
 *   <option> denotes an optional parameter
 *
 *   Notes: the _n needs to be present to pass down the module and moduleName.
 */


#define CONFIG_IRQ_OPTS(_n...)            /*Name                  min    max    default*/\
   D(_n, IRQ_ROUTING_POLICY,               RoutingPolicy,        0,     2,     1, "policy for interrupt routing, 1 for idle-routing, 2 for random, 0 to disable moving IRQs")\
   D(_n, IRQ_BEST_VCPU_ROUTING,            BestVcpuRouting,      0,     1,     1, "")\
   D(_n, IRQ_VECTOR_CACHE_BONUS_PCT,       VectorCacheBonusPct,  0,   100,    10, "percent bias towards keeping interrupt routed to current pcpu", NULL, TRUE )\
   D(_n, IRQ_REBALANCE_PERIOD,             IRQRebalancePeriod,  10, 20000,   500, "time in ms between attempts to rebalance interrupts")\
   D(_n, IRQ_MAX_LOAD_PCT,                 IRQMaxLoadPct,        0,   100,    70, "maximum percentage of a cpu's resources that should be devoted to interrupts")\

#define CONFIG_MISC_OPTS(_n...)/*Name                  min    max    default*/\
   D(_n, LOG_TO_FILE,          LogToFile,             0,        1,     1, "Send vmkernel log messages to /var/log/vmkernel")\
   D(_n, LOG_TO_SERIAL,        LogToSerial,           0,        1,     1, "Send vmkernel log messages to the serial port")\
   D(_n, LOG_WLD_PREFIX,       LogWldPrefix,          0,        1,     1, "Including running world on every log statement")\
   D(_n, MINIMAL_PANIC,        MinimalPanic,          0,        1,     0, "Don't attempt to coredump after PSODing")\
   D(_n, BLUESCREEN_TIMEOUT,   BlueScreenTimeout,     0,    65535,     0, "timeout in seconds, 0 is no timeout")\
   D(_n, HEARTBEAT_TIMEOUT,    HeartbeatTimeout,      1,    86400,  OBJ_BUILD ? 20 : 60, "Timeout[1sec - 86400secs], for sending NMI to the locked CPU")\
   D(_n, HEARTBEAT_INTERVAL,   HeartbeatInterval,   100, 86400000, 10000, "Interval[100 - 86400000msec] to check CPU lockups")\
   D(_n, DEBUG_MEM_ENABLE,     DebugMemEnable,        0,        1,  OBJ_BUILD ? 1 : 0, "Enable memory debugging, 0 to disable")\
   D(_n, ENABLE_HIGH_DMA,      EnableHighDMA,         0,        1,     0, "Enable DMA above 4GB")\
   D(_n, VMKPERF_PER_WORLD,    VmkperfPerWorld,       0,        1,     0, "should performance counters be maintained per-world [0-1]")\
   D(_n, TIMER_HARD_PERIOD,    TimerHardPeriod,       1,  1000000,  1000, "Hard timer interrupt period in microseconds", Timer_UpdateConfig, FALSE)\
   D(_n, TIMER_MIN_GUEST_PERIOD,TimerMinGuestPeriod,100,  1000000,   100, "Minimum period for guest timer callbacks in microseconds")\
   D(_n, KVMAP_ENTRIES_MIN,    MemAdmitMapEntriesMin, 0,      100,    30, "free KVMap entries required to power on VM, [0-100]")\
   D(_n, KVMAP_ENTRIES_LOW,    MemMapEntriesLow,      0,     1024,   200, "Point at which to start conserving KVMap entries, [0-1024]")\
   D(_n, KVMAP_GUARD_UNCACHED, KVMapGuardUncached,    0,        1,     1, "use guard pages around uncached kvmap mappings, [0-1]")\
   D(_n, PSOD_ON_COS_PANIC,    PsodOnCosPanic,        0,        1,     1, "PSOD vmkernel on Service Console panic / oops [0-1]")\
   D(_n, SERIAL_PORT,          SerialPort,            1,        2,     1, "Which serial port to use for logging")\
   D(_n, SERIAL_BAUD_RATE,     SerialBaudRate, SERIAL_MIN_BAUD_RATE, SERIAL_MAX_BAUD_RATE, SERIAL_MAX_BAUD_RATE, "Baud rate")\
   S(_n, PROC_VERBOSE,         ProcVerbose,           "",    "**",  "option unused", Proc_VerboseConfigChange)\
   S(_n, COS_COREFILE,         CosCorefile,           "",    "**",  "Full path of vmfs file to use for Service Console core dumps")\
   D(_n, WATCHDOG_BACKTRACE,   WatchdogBacktrace,     0,       10,     0, "Backtrace on every nth watchdog [0-10]")\
   S(_n, HOSTNAME,             HostName,             "unknown", "**", "Host name", StatusTerm_HostNameCallback)\
   D(_n, IPADDRESS,            VMKernelID,            0,  0xffffffff, 0, "Host IP address", Init_VMKernelIDCallback)\
   D(_n, SHOW_PROGRESS,        ShowProgress,          0,        0,     0, "Stop progress display", StatusTerm_StopShowingProgress)\

#define CONFIG_NET_OPTS(_n...)      /*Name                      min    max    default*/ \
   D(_n, NET_ESX2_COMPAT,           NetESX2Compat,            0,       1,     1, "support ESX-2 style clients?") \
   D(_n, NET_MAX_PORT_RX_QUEUE,     MaxPortRxQueueLen,        1,     500,    50, "Max length of the rx queue for virtual ports whose clients support queueing" )\
   D(_n, NET_MAX_NETIF_RX_QUEUE,    MaxNetifRxQueueLen,       1,    1000,   100, "Max length of the rx queue for the physical NICs" )\
   D(_n, NET_MAX_NETIF_TX_QUEUE,    MaxNetifTxQueueLen,       1,    1000,   100, "Max length of the tx queue for the physical NICs" )\
   D(_n, NET_VMM_TX_COPYBREAK,      GuestTxCopyBreak,         60,     -1,    64, "transmits smaller than this will be copied rather than mapped" )\
   D(_n, NET_USE_PROC,              UseProc,                  0,       1,     1, "whether or not to populate /proc/vmware/net [0 = disabled, 1 = enabled]")\
   D(_n, NET_COPIES_BEFORE_REMAP,   CopiesBeforeRemap,        0,     100,    10, "copies before remapping, 0 to disable" )\
   D(_n, NET_CLUSTER_HALT_CHECK,    ClusterHaltCheck,         0,       1,     1, "1 to check for clustered tx/rx packets on halt")\
   D(_n, NET_NOTIFY_SWITCH,         NotifySwitch,             0,       1,     1, "Broadcasts an arp request on net handle enable [0 = disabled, 1 = enabled]")\

#ifdef ESX3_NETWORKING_NOT_DONE_YET
#ifdef ESX2_SUPPORT
   D(_n, NET_VALIDATE_GUEST_BUFFERS,       ValidateGuestBuffers,       0,     1,        1, NULL, NULL, TRUE) \
   D(_n, NET_RXCLUSTER_TIMER_CPU,          RXClusterTimerCPU,          0,     1024,     0, "which cpu to run the timer on, 0 for migratory timer")\
   D(_n, NET_RXCLUSTER_THRESH_ON ,         RXClusterThreshOn,          0,   500000,  4000, "interrupts/sec to activate clustering")\
   D(_n, NET_RXCLUSTER_DELAY_ON,           RXClusterDelayOn,           0,     1000,     2, "weighting factor for activating clustering")\
   D(_n, NET_RXCLUSTER_THRESH_OFF,         RXClusterThreshOff,         0,   500000,  2000, "interrupts/sec to deactivate clustering")\
   D(_n, NET_RXCLUSTER_DELAY_OFF,          RXClusterDelayOff,          0,     1000,    10, "weighting factor for deactivating clustering")\
   D(_n, NET_RXCLUSTER_DELAY_TINC,         RXClusterDelayTInc,         0,    10000,     0, "increment timer freq this often")\
   D(_n, NET_RXCLUSTER_DELAY_TDEC,         RXClusterDelayTDec,         0,    10000,  1000, "decrement timer freq this often")\
   D(_n, NET_RXCLUSTER_TMINFREQ,           RXClusterTMinFreq,          1,       13,     6, "min timer freq as a power of two [1-13]")\
   D(_n, NET_RXCLUSTER_TMAXFREQ,           RXClusterTMaxFreq,          1,       13,    10, "max timer freq as a power of two [1-13]")\
   D(_n, NET_RXCLUSTER_CPUSAMPLE_PERIOD,   RXClusterCPUSamplePeriod,   1,    10000,  1000, "CPU %idle sample period (msec) [1-10000]")\
   D(_n, NET_RXCLUSTER_TMIGRATE_THRESH,    RXClusterTMigrateThresh,    1,      100,    20, "timer CPU migration threshold (delta %idle)")\
   D(_n, NET_RXCLUSTER_TMIGRATE_DELAY,     RXClusterTMigrateDelay,     1,      100,     5, "timer CPU migration intervals [1-100]")\
   D(_n, NET_RECV_COPY_LENGTH,             RecvCopyLength,             0,     1600,   200, "")\
   D(_n, NET_MAX_RECV_PACKETS,             MaxRecvPackets,            32,      256,    64, "")\
   D(_n, NET_MAX_PRIVATE_SKBS,             MaxPrivateSKBs,            64,     1024,   256, "")\
   D(_n, NET_COPIES_BEFORE_REMAP,          CopiesBeforeRemap,          0,      100,    10, "copies before remapping, 0 to disable" )\
   D(_n, NET_MAX_MALLOC_PKTS,              MaxMallocPackets,           8,     1024,   256, "maximum number of packets that can be allocated via the memory allocator" )\
   D(_n, NET_PROC_VERBOSE,                 ProcVerbose,                0,        1,     0, "verbose procfs output for networking, 0 to disable" )\
   D(_n, NET_PKTS_PER_XMIT_INT,            PktsPerXmitInterrupt,       1,     1000,     5, "packets to transmit before raising completion interrupt" )\
   D(_n, NET_DEFER_XMIT_INT_IF_KEEPING,    PktsDeferXmitIntIfKeeping,  1,     1000,     6, "defer xmit completion interrupts if this many packets still pending" )\
   D(_n, NET_XMIT_INT_TIMEOUT,             XmitInterruptTimeout,       1,      100,    10, "timeout in milliseconds to check for completed transmits" )\
   D(_n, NET_XMIT_INT_IF_STOPPED,          XmitInterruptIfStopped,     0,        1,     0, "raise an interrupt if a xmit packet is returned and the guest has stopped transmitting" )\
   D(_n, NET_DISABLE_WATCHDOG,             DisableWatchdog,            0,        1,     0, "Disable network watchdog timeout handler")\
   D(_n, NET_RECV_CLUSTER_QUEUE_MAX,       RecvClusterQueueMax,        0,      100,    10, "")\
   D(_n, NET_RECV_CLUSTER_ON_COUNT,        RecvClusterOnCount,         1,      100,    40, "")\
   D(_n, NET_RECV_CLUSTER_OFF_COUNT,       RecvClusterOffCount,        1,      100,    30, "")\
   D(_n, NET_RECV_CLUSTER_QUEUE_DYN_MAX,   RecvClusterDynQueueMax,     0,      100,    20, "") \
   D(_n, NET_RECV_CLUSTER_DYN_ADJ_RATE,    RecvClusterDynAdjRate,      0,     1000,    50, "") \
   D(_n, NET_RECV_CLUSTER_TIMEOUT_MS,      RecvClusterTimeoutMS,       1,      100,    10, "")\
   D(_n, NET_RECV_CLUSTER_TIMEOUT_COUNT,   RecvClusterTimeoutCount,    1,      100,     1, "")\
   D(_n, NET_XMIT_CLUSTER_QUEUE_MAX,       XmitClusterQueueMax,        0,      100,    10, "")\
   D(_n, NET_XMIT_CLUSTER_ON_COUNT,        XmitClusterOnCount,         1,      100,    40, "")\
   D(_n, NET_XMIT_CLUSTER_OFF_COUNT,       XmitClusterOffCount,        1,      100,    30, "")\
   D(_n, NET_XMIT_CLUSTER_TIMEOUT_MS,      XmitClusterTimeoutMS,       1,      100,    10, "")\
   D(_n, NET_XMIT_CLUSTER_TIMEOUT_COUNT,   XmitClusterTimeoutCount,    1,      100,     1, "")\
   D(_n, NET_TCPIP_LOG,                    TCPIPLog,                   0,        1,     0, "TCPIP logging, 0 to disable" )\
   D(_n, NET_ALLOW_TSO,                    TcpSegmentationOffload,     0,        1,     1, "Allow TSO, 0 to disable" )\
   D(_n, NET_VLAN_TRUNKING,                VlanTrunking,               0,        1,     1, "1 to enable 802.1Q VLAN Tagging by vmkernel.", Net_VlanUpdateSwitchConfig )\
   D(_n, NET_VERBOSE_BOND_CONFIG_OUTPUT,   VerboseBondConfigOuput,     0,        1,     0, "1 to enable switch failover verbose config output", NULL, TRUE) \
   D(_n, NET_SWITCH_FAILOVER_THRESHOLD,    SwitchFailoverThreshold,    0,     1000,     2, "NIC Teaming switch failover threshold, a positive number to enable switch failover policy", Net_BondSwitchFailoverThresholdConfig )\
   D(_n, NET_NICTEAMING_BEACON_INTERVAL,   SwitchFailoverBeaconInterval,   1,   60,     1, "NIC Teaming switch failover beacon interval [1 - 60]", )\
   D(_n, NET_SFO_ETHER_TYPE,               SwitchFailoverBeaconEtherType,  0x6000,0x9000, 0x9000, "NIC Teaming switch failover beacon Ether Type [0x6000 - 0x9000]", )\
   D(_n, NET_BOND_BCAST_ON_STP_CHANGE,     NicTeamingBroadcastOnSTPChange, 0,    1,     0, "1 to enable NIC Teaming broadcast upon external spanning tree topology change." )\

#endif // ESX2_SUPPORT
#endif // ESX3_NETWORKING_NOT_DONE_YET

#define CONFIG_MEM_OPTS(_n...)       /*Name                    min    max  default*/ \
   D(_n, MEM_BALANCE_PERIOD,          BalancePeriod,         0,     120,   15, "period in seconds [1-120], 0 to disable", MemSched_Reconfig )\
   D(_n, MEM_SAMPLE_PERIOD,           SamplePeriod,          0,     180,   60, "period in seconds [1-180], 0 to disable", MemSched_ReconfigSample )\
   D(_n, MEM_SAMPLE_SIZE,             SampleSize,            1,     100,  100, "Sample set size in pages [1-100]", MemSched_ReconfigSample, TRUE )\
   D(_n, MEM_SAMPLE_HISTORY,          SampleHistory,         1,       4,    4, "history in periods [1-4]", MemSched_ReconfigSample, TRUE )\
   D(_n, MEM_IDLE_TAX,                IdleTax,               0,      99,   75, "idle memory tax rate [0-99]", MemSched_Reconfig )\
   D(_n, MEM_SHARE_SCAN_VM,           ShareScanVM,           0,    1000,   50, "per-VM page scans in pages/sec [1-1000], 0 to disable", MemSched_ReconfigPShare )\
   D(_n, MEM_SHARE_SCAN_TOTAL,        ShareScanTotal,        0,   10000,   200, "total page scans in pages/sec [1-10000], 0 to disable", MemSched_ReconfigPShare)\
   D(_n, MEM_SHARE_CHECK_VM,          ShareCheckVM,          0,    1000, OBJ_BUILD ? 20 : 0, "per-VM page checks in pages/sec [1-1000], 0 to disable", MemSched_ReconfigPShare, TRUE)\
   D(_n, MEM_SHARE_CHECK_TOTAL,       ShareCheckTotal,       0,   10000, OBJ_BUILD ?  100 : 0, "total page checks in pages/sec [1-10000], 0 to disable", MemSched_ReconfigPShare, TRUE)\
   D(_n, MEM_CTL_MAX_NT4,             CtlMaxNT4,             0,     192,   128, "vmmemctl limit for Windows NT4 VM, in MB [0-192]")\
   D(_n, MEM_CTL_MAX_NT5,             CtlMaxNT5,             0,    8192,  2048, "vmmemctl limit for Windows 2000 or Windows 2003 VM, in MB [0-8192]")\
   D(_n, MEM_CTL_MAX_LINUX,           CtlMaxLinux,           0,    8192,   768, "vmmemctl limit for Linux VM, in MB [0-8192]")\
   D(_n, MEM_CTL_MAX_BSD,             CtlMaxBSD,             0,    8192,  2048, "vmmemctl limit for BSD VM, in MB [0-8192]")\
   D(_n, MEM_CTL_MAX_PERCENT,         CtlMaxPercent,         0,      50,    50, "vmmemctl limit as percentage of VM max size [0-50]")\
   D(_n, MEM_ALLOC_HIGH_THRESHOLD,    AllocHighThreshold,    1,    4096,   768, "Threshold (in MB) at which we start allocating memory above 4GB")\
   D(_n, MEM_ADMIT_HEAP_MIN,          AdmitHeapMin,        256,   10240,  1024, "free heap space required to power on VM, in KB [256-10240]")\
   D(_n, MEM_MIN_FREE,                MinFreePct,            6,      24,     6, "Minimum percent of memory that should be kept free",  MemSched_UpdateMinFree, FALSE)\
   D(_n, MEM_NUM_P2M_BUF_MPNS,        ShareCOSBufSize,       2,       8,     2, "Specify number of MPNs to be used by COW P2M buffer [2-8]")\
   D(_n, MEM_SWAP_SANITY_CHECKS,      SwapCheck,             0,       1,     0, "Enable swap stress testing [0-1]", Swap_UpdateDoSanityChecks, TRUE)\
   D(_n, MEM_SWAP_COW_PAGES,          SwapSharedStress,      0,       1,     0, "Enable swapping of shared pages for stress testing [0-1]", NULL, TRUE)\
   D(_n, MEM_SWAP_MAX_COW_REF_COUNT,  SwapShared,            0,     100,     2, "Set the max ref count of a swappable shared page [0-100]")\
   D(_n, MEM_SWAP_IO_RETRY,           SwapIORetry,           0,  100000,  5000, "Number of retries for swap-in operation on I/O failures")\

#define CONFIG_CPU_OPTS(_n...)            /*Name                     min    max    default*/ \
   D(_n, CPU_PCPU_MIGRATE_PERIOD,          MigratePeriod,            0,   5000,    20, "milliseconds between opportunities to migrate across cpus",\
                                                                                         CpuSched_UpdateConfig)\
   D(_n, CPU_CELL_MIGRATE_PERIOD,          CellMigratePeriod,        0,  60000,  1000, "milliseconds between opportunities to migrate across cells",\
                                                                                          CpuSched_UpdateConfig)\
   D(_n, CPU_RUNNER_MOVE_PERIOD,           RunnerMovePeriod,         0,  60000,   200, "milliseconds between opportunities to move currently-running vcpu",\
                                                                                          CpuSched_UpdateConfig, TRUE)\
   D(_n, CPU_MIGRATE_CHANCE,               MigrateChance,            0,     64,     5, "inverse of probability of migration between mig periods [0 for no chance]", CpuSched_UpdateConfig, TRUE)\
   D(_n, CPU_WAKEUP_MIGRATE_IDLE,          WakeupMigrateIdle,        0,      1,     0, "migrate to idle package on wakeup, 0 to disable")\
   D(_n, CPU_CREDIT_AGE_PERIOD,            CreditAgePeriod,        500,  10000,  3000, "period in milliseconds [500-10000]", CpuSched_UpdateConfig )\
   D(_n, CPU_COS_WARP_PERIOD,              ConsoleOSWarpPeriod,      0,    100,    20, "period in milliseconds [0-100]", CpuSched_UpdateConfig )\
   D(_n, CPU_BOUND_LAG_QUANTA,             BoundLagQuanta,           1,    100,     8, "number of global quanta before bound lag [1-100]", CpuSched_UpdateConfig )\
   D(_n, CPU_HALTING_IDLE_MS_PENALTY,      HaltingIdleMsecPenalty ,  0,    100,    20, "ms to add to partner's vtime for halting idle world (HT only) [0-100]", \
                                                                                         CpuSched_UpdateConfig, TRUE )\
   D(_n, CPU_PREEMPTION_BONUS,             PreemptionBonus,          0,    500,    20, "ms to subtract from running vcpu's vtime to make preemption harder [0-500]", \
                                                                                         CpuSched_UpdateConfig, TRUE )\
   D(_n, CPU_MOVE_CURRENT_RUNNER,          MoveCurrentRunner,        0,      1,     1, "allow the idle world to preempt and move a currently-running pcpu", \
                                                                                         CpuSched_UpdateConfig, TRUE )\
   D(_n, CPU_COSCHED_CACHE_AFFINITY_BONUS, CoschedCacheAffinBonus ,  0,    500,    20, "ms to add to pcpu's preempt vtime for cache affinity (HT only) [0-100]",\
                                                                                          CpuSched_UpdateConfig, TRUE )\
   D(_n, CPU_COS_MIN_CPU,                  ConsoleMinCpu,            0,    100,     8, "min percentage of CPU 0 to dedicate to console [0-100]",\
                                                                                         CpuSched_UpdateCOSMin )\
   D(_n, CPU_QUANTUM,                      Quantum,                  1,   1000,    50, "quantum in milliseconds [1-1000]", CpuSched_UpdateConfig )\
   D(_n, CPU_IDLE_QUANTUM,                 IdleQuantum,              1,   1000,    10, "idle world quantum in milliseconds [1-1000]", CpuSched_UpdateConfig, TRUE)\
   D(_n, CPU_IDLE_SWITCH_OPT,              IdleSwitchOpt,            0,      1,     1, "idle switch optimization, 0 to disable", CpuSched_UpdateConfig, TRUE)\
   D(_n, CPU_IDLE_CONSOLE_OPT,             IdleConsoleOpt,           0,      1,     1, "idle console switch optimization, 0 to disable", CpuSched_UpdateConfig, TRUE)\
   D(_n, CPU_HALTING_IDLE,                 IdleHalts,                0,      1,     1, "halt in idle loop on HT systems, 0 to disable", CpuSched_UpdateConfig, TRUE)\
   D(_n, CPU_YIELD_THROTTLE_USEC,          YieldThrottleUsec,        0,   2000,   100, "min microseconds to wait between calls to throttled yield", CpuSched_UpdateConfig, TRUE)\
   D(_n, CPU_SCHEDULER_DEBUG,              SchedulerDebug,           0,      1,     0, "extra debugging support for scheduler, 0 to disable", CpuSched_UpdateConfig, TRUE)\
   D(_n, CPU_SKEW_SAMPLE_USEC,             SkewSampleUsec,         100,  50000,  1000, "interval between vsmp skew tests", CpuSched_UpdateConfig)\
   D(_n, CPU_SKEW_SAMPLE_THRESHOLD,        StrictSkewThreshold,      0,     50,     3, "number of skew samples allowed before co-deschedule, only applies if not relaxed skew", CpuSched_UpdateConfig,TRUE)\
   D(_n, CPU_INTRASKEW_THRESHOLD,          IntraSkewThreshold,       1,    100,  DEBUG_BUILD ? 10 : 5, "number of intra-vsmp skew samples before co-descheduling", CpuSched_UpdateConfig, TRUE)\
   D(_n, CPU_RELAXED_COSCHED,              RelaxedCoSched,           0,      1,     1, "1 to allow relaxed coscheduling, 0 to be strict", CpuSched_UpdateConfig,TRUE)\
   D(_n, CPU_AFFINITY_MINADMIT,            AffinityMinAdmitCheck,    0,      1,     1, "consider affinity constraints when performing cpu min admission control check, 0 to disable",\
                                                                                         CpuSched_UpdateConfig, TRUE)\
   D(_n, CPU_IDLE_VTIME_INTERRUPT_PENALTY, IdleVtimeInterruptPenalty,0,     100,   25, "vtime penalty in ms per level of interrupt load", CpuSched_UpdateConfig, TRUE)\
   D(_n, CPU_MACHINE_CLEAR_THRESH, MachineClearThreshold,       0,   10000,  100, "machine clears per million cycles to trigger quarantine", NULL, TRUE)\
   D(_n, CPU_IDLE_PACKAGE_REBALANCE_PERIOD, IdlePackageRebalancePeriod,       0,   100000,  541, "usec between chances to rebalance idle packages (0 to disable, 100000 max)", CpuSched_UpdateConfig)\
   D(_n, CPU_RESCHED_OPT, ReschedOpt,       0,   3,  2, "When to invoke the scheduler after vcpu wakeup [0:always, 1:preemptible, 2:defer, 3:never]", CpuSched_UpdateConfig, TRUE) \
   D(_n, CPU_RESCHED_DEFER_TIME, ReschedDeferTime,       1,   50,  10, "If ReschedOpt==2, how many ticks to wait before resched", CpuSched_UpdateConfig, TRUE) \
   D(_n, CPU_VTIME_RESET_LG,		   VtimeResetLg,            34,     61,    61, "vtime reset threshold (log2) to avoid wraparound [34-61]", CpuSched_UpdateConfig, TRUE) \
   D(_n, CPU_LOAD_HISTORY_SAMPLE_PERIOD,   LoadHistorySamplePeriod, 100, 10000,   6000, "load history sample period in milliseconds [100-10000]" )

#define CONFIG_NUMA_OPTS(_n...       /*Name                     min    max   default*/) \
   D(_n, NUMA_DEBUG,                  Debug,                   0,      4,     0, "level of NUMA scheduler debugging information to print [0-4]")\
   D(_n, NUMA_REBALANCE_PERIOD,       RebalancePeriod,       100,  60000,  2000, "frequency of NUMA node rebalancing, in milliseconds")\
   D(_n, NUMA_MIG_THRESHOLD,          MigImbalanceThreshold,   1,  10000,    75, "minimum deviation in owed ms between nodes, per second, to trigger migration")\
   D(_n, NUMA_SWP_LOCALITY_THRESHOLD, SwapLocalityThreshold,   1,    200,    20, "minimum memory locality improvement to trigger node swap", NULL, TRUE)\
   D(_n, NUMA_MONMIG_HISTORY,         MonMigHistory,           1,     20,    17, "minimum local history to trigger monitor node migration", NULL, TRUE)\
   D(_n, NUMA_MONMIG_LOCALITY,        MonMigLocality,          1,     99,    80, "max percent remote overhead memory to trigger monitor node migration", NULL, TRUE)\
   D(_n, NUMA_ROUND_ROBIN,            RoundRobin,              0,      1,     1, "1 to use round-robin initial placement algorithm, 0 to place on node with most free memory", NULL, TRUE)\
   D(_n, NUMA_REBALANCE,              Rebalance,               0,      1,     1, "1 to use NUMASched rebalancer, 0 to disallow it")\
   D(_n, NUMA_AUTO_MEMAFFINITY,       AutoMemAffinity,         0,      1,     1, "1 to set mem affinity automatically based on cpu affinity, 0 to disable")\
   D(_n, NUMA_PAGE_MIG,               PageMig,                 0,      1,     1, "1 to permit NUMASched to manipulate page migration, 0 to disallow it")\
   D(_n, NUMA_MONMIG_TIME,            MonMigTime,              0,   3600,    20, "minimum time (in seconds) to allow for monitor migration", NULL, TRUE)\
   D(_n, NUMA_MIN_MIGRATE_INTERVAL,   MinMigInterval,          0,   3600,     2, "minimum time (in seconds) between node migrations", NULL, TRUE)\

 #define CONFIG_SCSI_OPTS(_n...)     /*Name                  min  max  default*/ \
   D(_n, SCSI_PASSTHROUGH_LOCKING,    PassthroughLocking,  0,    1,    1, "")\
   D(_n, SCSI_CONFLICT_RETRIES,       ConflictRetries,     0,  100,    4, "Maximum number of retries when encountering reservation conflict" )\
   D(_n, SCSI_LOG_MULTI_PATH,         LogMultiPath,        0,    1,    0, "Log path state changes")\

#define CONFIG_DISK_OPTS(_n...)            /*Name                    min     max   default*/ \
   D(_n, DISK_SHARES_NORMAL,               SharesNormal,           100,    10000,  1000, "shares for normal/default disk priority [100-10000]")\
   D(_n, DISK_SHARES_HIGH,                 SharesHigh,             100,    10000,  2000, "shares for high disk priority [100-10000]")\
   D(_n, DISK_SHARES_LOW,                  SharesLow,              100,    10000,   500, "shares for low disk priority [100-10000]")\
   D(_n, DISK_SECTOR_DIFF,                 SectorMaxDiff,            0,  2000000,  2000, "Distance in sectors at which disk BW sched affinity stops" )\
   D(_n, DISK_ISSUE_QUANTUM,               SchedQuantum,             1,       64,     8, "Number of consecutive requests from one World" )\
   D(_n, DISK_CIF,                         SchedNumReqOutstanding,   1,      256,    16, "Number of outstanding commands to a target with competing worlds" )\
   D(_n, DISK_QCONTROL_REQS,               SchedQControlSeqReqs,     0,     2048,   128, "Number of consecutive requests from a VM required to raise the "\
                                                                                           "outstanding commands to max" )\
   D(_n, DISK_QCONTROL_SWITCHES,           SchedQControlVMSwitches,  0,     2048,     6, "Number of switches between commands issued by different "\
                                                                                           "VMs required to reduce outstanding commands to CONFIG_DISK_CIF" )\
   D(_n, DISK_MAX_LUN,                     MaxLUN,                   1,      256,     8, "Maximum number of LUNs per target that we scan for" )\
   D(_n, DISK_SUPPORT_SPARSE_LUN,          SupportSparseLUN,         0,        1,     1, "Support for sparse LUNs if set to one" )\
   D(_n, DISK_USE_REPORT_LUN,              UseReportLUN,             0,        1,     1, "Use the REPORT LUN command to speed up scanning for devices" )\
   D(_n, DISK_USE_DEVICE_RESET,            UseDeviceReset,           0,        1,     1, "Use device reset (instead of bus reset) to reset a SCSI device" )\
   D(_n, DISK_USE_LUN_RESET,               UseLunReset,              0,        1,     1, "Use LUN reset (instead of device/bus reset) to reset a SCSI device" )\
   D(_n, DISK_RETRY_UNIT_ATTENTION,        RetryUnitAttention,       0,        1,     1, "Retry all SCSI commands that return a unit attention error" )\
   D(_n, DISK_RESET_ON_FAILOVER,           ResetOnFailover,          0,        1,     0, "Issue a SCSI reset when failing over to an alternate HBA" )\
   S(_n, DISK_MASK_LUNS,                   MaskLUNs,                "", "**", "LUN's to mask from kernel. Format: <adapter>:<target>:<comma separated LUN range list>") \
   S(_n, DISK_ACTIVE_PASSIVE_FAILOVER_SANS,SANDevicesWithAPFailover, "", "**",\
                                                                                            "SAN devices with Active/Passive Path Failover. Format: "\
                                                                                            "<16 Character Device Id>:<16 Character Device Id>:...") \
   D(_n, DISK_PATH_EVAL_TIME,              PathEvalTime,             30,   1500,    300, "The number of seconds between FC path evaluations" )\
   D(_n, DISK_SVC_NOT_READY_RETRIES,       SVCNotReadyRetryCount,    50,   5000,    100, "The number of times to retry on an SVC path that reports NOT READY status" )\
   D(_n, DISK_DELAY_ON_BUSY,               DelayOnBusy,               0,   5000,    400, "Delay in milliseconds for completion of commands with a BUSY status" )\
   D(_n, DISK_RESET_LATENCY,               ResetLatency,           100,   600000,    1000, "Delay in milliseconds between reset thread wake-ups" )\
   D(_n, DISK_MAX_RESET_LATENCY,           MaxResetLatency,        500,   600000,    2000, "Delay in milliseconds before logging warnings and spawning new reset worlds if a reset is overdue or taking too long" )\
   D(_n, DISK_RESET_PERIOD,                ResetPeriod,              1,     3600,      30, "Delay in seconds between bus resets retries" )\
   D(_n, DISK_RESET_MAX_RETRIES,           ResetMaxRetries,          0,    10000,       0, "Max number of bus reset retries (0=infinite)" )\
   D(_n, DISK_MIN_RESET_WORLDS,            ResetThreadMin,           1,       16,       1, "Min number of reset handler threads" )\
   D(_n, DISK_MAX_RESET_WORLDS,            ResetThreadMax,           1,       16,      16, "Max number of reset handler threads" )\
   D(_n, DISK_RESET_WORLD_EXPIRES,         ResetThreadExpires,       0,    86400,    1800, "Life in seconds of an inactive reset handle thread" )\
   D(_n, DISK_OVERDUE_RESET_LOG_PERIOD,    ResetOverdueLogPeriod,   10,    86400,      60, "Delay in seconds between logs of overdue reset" )\


#define CONFIG_FS_OPTS(_n...) /*Name              min  max  default*/ \
   D(_n, FS_LOCK_RETRIES,      LockRetries,      0,  100,  15, "Maximum number of retries when encountering file system lock" )\

#define CONFIG_CONDUIT_OPTS(_n...)      /*Name                  min     max   default*/ \
   D(_n, CONDUIT_ENABLED,                Enabled,               0,      1,    0, "Enable use of shared memory conduits", Conduit_ModuleEnable)  

#define CONFIG_MIGRATE_OPTS(_n...)      /*Name                  min     max   default*/ \
   D(_n, MIGRATE_ENABLED,                Enabled,               0,      1,    0, "Enable hot migration support", Migrate_Enable)  \
   D(_n, MIGRATE_PRE_COPY_MAX_STOP,      PreCopyLeftMB,         1,   1024,   16, "Maximum modified memory left over after pre-copy is done")\
   D(_n, MIGRATE_PRE_COPY_MIN_PROGRESS,  PreCopyMinProgressMB,  1,   1024,   16, "Minimum reduction in modified memory after a pre-copy iteration")\
   D(_n, MIGRATE_NET_TIMEOUT,            NetTimeout,            1,   3600,   10, "Timeout for migration network operations")\
   D(_n, MIGRATE_MEM_CHECKSUM,           MemChksum,             0,      1,    0, "Checksum VM's memory while migrating")\
   D(_n, MIGRATE_TS_MASTER,              TSMaster,              0,      1,    0, "Pseudo-synchronize clocks for migration to/from this machine[0, 1]")\
   D(_n, MIGRATE_RESERVE_MIN,            MinReservation,        0,    100,   30, "Reserve a percentage of a cpu for use by migration helper worlds[0, 100]")\
   D(_n, MIGRATE_PAGEIN_TIMEOUT,         PageInTimeout,        10,  18000,  180, "Time in seconds to wait for pagein to finish [10-1800]")\
   D(_n, MIGRATE_PAGEIN_PROGRESS,        PageInProgress,        5,  18000,   15, "Time in seconds after which a pagin will be killed if there is no progress[5-1800]")\

#define CONFIG_USER_OPTS(_n...)         /*Name                    min     max    default */ \
   D(_n, USER_SOCKET_INET_TCPIP,         SocketInetTCPStack,      0,      1,     0, "UserSocketInet uses TCP/IP stack, 1 to enable", NULL, TRUE) \

/*
 * List of all config modules, and the the subdirectory name.
 */

#define CONFIG_MODULES_LIST \
   CONFIG_DEFMODULE(IRQ,          Irq)\
   CONFIG_DEFMODULE(MISC,         Misc)\
   CONFIG_DEFMODULE(NET,          Net)\
   CONFIG_DEFMODULE(MEM,          Mem)\
   CONFIG_DEFMODULE(CPU,          Cpu)\
   CONFIG_DEFMODULE(NUMA,         Numa)\
   CONFIG_DEFMODULE(DISK,         Disk)\
   CONFIG_DEFMODULE(FS,           FileSystem)\
   CONFIG_DEFMODULE(CONDUIT,      Conduit)\
   CONFIG_DEFMODULE(MIGRATE,      Migrate)\
   CONFIG_DEFMODULE(SCSI,         Scsi)\
   CONFIG_DEFMODULE(USER,         User)\

/*
 * Define the CONFIG_<FOO> enum types
 */
#define CONFIG_DEFMODULE(_module, _moduleName) CONFIG_##_module##_OPTS(_##_module, _moduleName)
#define D(_module, _moduleName, _macro, _ignore...) CONFIG_##_macro,
#define S(unused...) 

typedef enum {
   CONFIG_MODULES_LIST
   CONFIG_NUM_INT
} ConfigOptions;

#undef D
#undef S
#define D(unused...) 
#define S(_module, _moduleName, _macro, _ignore...)  CONFIG_##_macro,

typedef enum {
   CONFIG_SPACER = CONFIG_NUM_INT - 1,
   CONFIG_MODULES_LIST
   CONFIG_TOTAL_NUM
} ConfigStrOptions;

#undef D
#undef S

unsigned Config_GetOption(ConfigOptions opt);
char *Config_GetStringOption(ConfigStrOptions opt);
#endif
