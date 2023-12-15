#!/exit14/home/johnz/bin/python
#
#
# Copyright 2003, VMware Inc.
#
# TODO:
#
#  - attempt to provoke hangs, stuck vcpus [cpuBurn?]
#  - stress affinity, reallocation, other corner-cases
#  - SMP affinity 
#
#  - friendly UI
#  - log all scheduler info in case of failure (-V option)
#

"""
    This module provides a framework for scheduler regression testing

    Run it with the '-h' parameter to get help.
"""

__author__ = "John Zedlewski (johnz@vmware.com)"
__version__ = "1.0"
__copyright__ = "VMware, Inc. 2003"
USAGE = \
 """
    Usage: runschedtests [options] (all | testsuitenames)

    Options:
         -h           help (this message)
         -v           verbose (print detailed result info even if test succeeds)
         -V           very verbose (print full scheduler info on failure, implies -v)
         -n TESTLIST  Only run numbered comma-delimited tests in TESTSLIST, 
                      e.g -n 1,2,3 to run tests 1, 2 and 3, but no others
         -t RUNTIME   Run each test for RUNTIME seconds (default 50)"""

# append the USAGE string to our docstring so we can read it with pydoc, etc.
__doc__ += "\n" + USAGE

import sys, os, os.path, time, operator, pprint, getopt, threading, commands
import parse_sched, schedtests, re, tempfile, types, random, shutil

RUN_TIME = 50
PAUSE_TIME = 1.0
TEST_SIZE_FACTOR = 1

def get_max_vmid():
    """Finds the highest VMID of any currently running VM"""
    import glob
    g = glob.glob("/proc/vmware/vm/*")
    maxNode = max([int(os.path.split(x)[-1]) for x in g])
    sched = parse_sched.load_data();
    maxVM = max([int(v.vm) for v in sched.vcpu_list])
    return min(maxVM, maxNode)


def get_num_cpus():
    """returns tuple of (physical, logical) number of cpus"""
    f = open("/proc/vmware/sched/ncpus", "r")
    lines = f.readlines()
    if len(lines) == 1:
        # old-style, non-ht-aware ncpus proc node
        cpus = int(lines[0])
        return (cpus, cpus)
    else:
        logical = int(lines[0].split()[0])
        phys = int(lines[1].split()[0])
        return (phys, logical)
    f.close()

class TestError:
    """ Basic exception class thrown by tests """
    def __init__(self, msg, sched, conf):
        self.msg = msg
        # sched holds a SchedData object
        self.sched = sched
        self.conf = conf
    def __repr__(self):
        return self.msg

class VM:
    """Base class to encapsulate a VM, must be subclassed to be useful"""
    formatHeader = "shares   baseshr   vcpus   wait    run   usedsec"
    def __init__(self):
        self.schedData = None
        self.leader = None
        self.vcpus = []
        self.htsharing = None
    def getVMDir(self):
        return "/proc/vmware/vm/%d/" % self.vmid
    def format(self):
        """ pretty-print this VM (doesn't include newline) """
        format = "%6d   %6d   %5d  %5d  %5d      %3.3f" \
                     % (self.getShares(),
                        self.getBaseShares(),
                        self.numvcpus,
                        self.wait,
                        self.run,
                        self.getUsedSec())

        return format
    
    def setShares(self, shares):
        """Actually write the new shares to the proc node"""
        f = open(self.getVMDir() + "cpu/shares", "w")
        f.write(str(shares))
        f.close()
        self.shares = shares
    def setAffinity(self, affinStr):
        """Actually write the new affinity string to the proc node"""
        f = open(self.getVMDir() + "cpu/affinity", "w")
        f.write(affinStr)
        f.close()
        self.affinStr = affinStr
    def setSchedData(self, schedData):
        """Analyzes and stores data returned by parse_sched.load_data()"""
        self.schedData = schedData
        vcpus = schedData.vcpu_list
        self.vcpus = []
        for v in vcpus:
            if int(v.vm) == self.vmid:
                self.vcpus.append(v)
                if int(v.vm) == int (v.vcpu):
                    self.leader = v
        if len(self.vcpus) == 0:
            print "no vcpus found for vm", self.vmid
            for v in vcpus:
                print v.vm, v.vcpu, v.name
        # assert len(self.vcpus) > 0, "No vcpus found, vm " + str(self.vmid)
        assert self.leader is not None
    def getVcpuVal(self, valname, typefun=int):
        assert self.schedData, "Must set schedData before querying"
        return reduce(operator.add, [typefun(x.data[valname]) for x in self.vcpus])
    def getVMVal(self, valname, typefun=int):
        return typefun(self.leader.data[valname])
    def getShares(self):
        assert self.schedData, "Must set schedData before querying"
        return self.getVMVal("shares")
    def getBaseShares(self):
        assert self.schedData, "Must set schedData before querying"
        return self.getVMVal("base")
    def getMigs(self):
        return self.getVcpuVal("pmigs")
    def getSwitch(self):
        return self.getVcpuVal("switch")
    def getUsedSec(self):
        return self.getVcpuVal("usedsec", float)

def stop_timer_worlds(pause=True):
    f = open(TimerWorld.path, "w")
    f.write("stop")
    f.close()
    if pause:
        time.sleep(PAUSE_TIME)

    
class TestWorld(VM):
    """Base class for testworlds. Subclasses must specify the path
       to the appropriate /proc node to start/stop this testworld type"""
    def __init__(self, run, wait, shares, numvcpus, min=0, max=0, affinity=None):
        VM.__init__(self)
        self.run = run
        self.wait = wait
        self.shares = shares
        self.numvcpus = numvcpus
        self.path = None
        self.min = min
        if max == 0:
            self.max = 100 * numvcpus
        else:
            self.max = max
        self.affinity = affinity
    def configMin(self, min):
        """Write new 'min' cpu share to proc node"""
        minfile = self.getVMDir() + "cpu/min"
        f = open(minfile, "w")
        f.write(str(min))
    def readMin(self):
        minfile = self.getVMDir() + "cpu/min"
        f = open(minfile, "r")
        m = int(f.readline())
        self.min = m
        f.close()
        return m
    def configHTSharing(self):
        f = open(self.getVMDir() + "cpu/hyperthreading", "w")
        f.write(self.htsharing)
        f.close()

    def shortFormat(self):
        return "%05d-%1d-%02dr-%02dw" % (self.shares,
                                       self.numvcpus,
                                       self.run,
                                       self.wait)

    def configMax(self, max):
        """Write new 'max' cpu share to proc node"""
        maxfile = self.getVMDir() + "cpu/max"
        f = open(maxfile, "w")
        f.write(str(max))
        f.close()
    def configAffinity(self):
        affin = self.affinity
        affinfile = self.getVMDir() + "cpu/affinity"
        if type(affin) == types.ListType:
            affinstr = ""
            for i in range(0, self.numvcpus):
                affinstr += str(i) + ":" + str(affin[i]) + ";"
        elif self.numvcpus == 1:
            affinstr = str(affin)
        else:
            affinstr = "all:" + affin + ";"
        f = open(affinfile, "w")
        f.write(affinstr)
        f.close()
    def start(self, pause=True):
        """Begin execution of this testworld"""
        assert self.path, "Must subclass TestWorld"
        oldmax = get_max_vmid()
        f = open(self.path, "w")
        f.write("start %d %d %d %d" % (self.numvcpus, self.shares, self.run, self.wait))
        if pause:
            time.sleep(PAUSE_TIME)
        f.close()
        
        self.vmid = get_max_vmid()
        # spin until the proc nodes for the new world show up
        spincount = 0
        while self.vmid == oldmax:
            spincount += 1
            time.sleep(PAUSE_TIME)
            self.vmid = get_max_vmid()
            if spincount > 5:
                raise "Failed to create new testworld: " + pprint.pformat(self)
        if self.min != 0:
            self.configMin(self.min)
        if self.max != self.numvcpus * 100:
            self.configMax(self.max)
        if self.affinity is not None:
            self.configAffinity()
        if self.htsharing is not None:
            self.configHTSharing()
        
        assert self.vmid != oldmax, "couldn't create new world"

    def stopall(self, pause=True):
        stop_timer_worlds(pause)

class TimerWorld(TestWorld):
    path = "/proc/vmware/testworlds/timer-based"
    def __init__(self, run, wait, shares=1000, numvcpus=1, min=0, max=0, affinity=None):
        TestWorld.__init__(self, run, wait, shares, numvcpus, min, max, affinity)
        self.path = TimerWorld.path
    def shortFormat(self):
        return "timer-" + TestWorld.shortFormat(self)

class BasicWorld(TestWorld):
    def __init__(self, run, wait, shares=1000, numvcpus=1, min=0, max=0, affinity=None):
        TestWorld.__init__(self, run, wait, shares, numvcpus, min, max, affinity)
        self.path = "/proc/vmware/testworlds/basic"
    def shortFormat(self):
        return "basic-" + TestWorld.shortFormat(self)


class BasicTest:
    """Base class for tests, must be subclassed to be interesting"""
    def __init__(self, vms):
        self.name = str(self.__class__)
        self.vms = vms * TEST_SIZE_FACTOR
        self.alwaysVerbose = 0
    def setup(self):
        """Launch all VMs associated with this test"""
        try:
            for v in self.vms:
                v.start()
        except:
            # second try to start the test
            self.shutdown()
            time.sleep(PAUSE_TIME)
            for v in self.vms:
                v.start()
    def run(self):
        """Reset scheduler stats and run for RUN_TIME"""
        parse_sched.reset_stats()
        time.sleep(RUN_TIME)
    def results(self):
        """No effect, you must subclass BasicTest"""
        assert 0, "Not implemented"
    def format_results(self, resmap):
        resstr = ""
        klst = resmap.keys()
        klst.sort()
        for k in klst:
            resstr += "%-8s %5.3f" % (k, resmap[k])
            resstr += "\n"
        return resstr
    def shutdown(self):
        """Stops all timer/basic worlds"""
        stop_timer_worlds()


class FairnessError:
    def __init__(self, desc, vm, fairness, sched):
        self.desc = desc
        self.vm = vm
        self.fairness = fairness
        self.sched = sched
    def __repr__(self):
        return "fairness = %2.3f for VM %d" % (self.fairness, self.vm.vmid)

class VmkstatsTest(BasicTest):
    """
    Uses vmkstats to determine time taken by locks, busy loop, idle time, and
    scheduler over the course of the test run. Obviously, vmkstats must be
    compiled into the kernel for this to work.
    """
    dumpDir = "/tmp/vmkstatsdump/"
    vmkstatsControlFile = "/proc/vmware/vmkstats/command"
    vmkstatsCommand = "/exit14/home/johnz/bora/support/scripts/vmkstatsview -dumpDir " + dumpDir
    vmkstatsDumpCommand = "/exit14/home/johnz/bora/support/scripts/vmkstats_dump.pl -o " + dumpDir
    
    def __init__(self, vms, name=""):
        BasicTest.__init__(self, vms)
        self.name = name
        self.alwaysVerbose = True
        
    def run(self):
        assert os.path.exists(VmkstatsTest.vmkstatsControlFile), "Vmkstats not enabled!"
        # clean tempdir
        if os.path.exists(VmkstatsTest.dumpDir):
            shutil.rmtree(VmkstatsTest.dumpDir)

        # clear and then start vmkstats profiling
        f = open(VmkstatsTest.vmkstatsControlFile, "w")
        f.write("stop")
        f.close()
        f = open(VmkstatsTest.vmkstatsControlFile, "w")
        f.write("reset")
        f.close()
        f = open(VmkstatsTest.vmkstatsControlFile, "w")
        f.write("start")
        f.close()

        # actually run the test
        BasicTest.run(self)
        
        # dump stats to tempfile
        status, output = commands.getstatusoutput(VmkstatsTest.vmkstatsDumpCommand)
        
    def results(self):
        assert os.path.exists(VmkstatsTest.vmkstatsControlFile), "Vmkstats not enabled!"
        self.results = {}
        regexTuples = [ (re.compile("([\d\.]+)\%\s+[\d,]+\s+([\d\.]+)\%\s+SP_WaitLockIRQ"), "locks"),
                        (re.compile("([\d\.]+)\%\s+[\d,]+\s+([\d\.]+)\%\s+Util_Udelay"), "udelay"),
                        (re.compile("([\d\.]+)\%\s+[\d,]+\s+([\d\.]+)\%\s+(CpuSched_IdleLoop|SMP_SlaveHaltCheck|CpuSchedBusyWait)"), "idle"),
                        (re.compile("([\d\.]+)\%\s+[\d,]+\s+([\d\.]+)\%\s+CpuSched"), "sched"),]

        for t in regexTuples:
            self.results[t[1]] = 0.0

        # actually run vmkstatsdump so we can parse the output
        outtext = commands.getoutput(VmkstatsTest.vmkstatsCommand)

        # try to match each line against regexps, increasing cumulative
        # time for function type with each match
        for l in outtext.split("\n"):
            for t in regexTuples:
                reg, name = t
                m = reg.search(l)
                if m:
                    self.results[name] += float(m.group(1))
                    break

        resmap = {}
        for t in regexTuples:
            reg, name = t
            resmap[name] = self.results[name]
        return resmap, []
    
    def getTestElements(self):
        return ["locks", "udelay", "idle", "sched"]
        

class PerfStatsTest(BasicTest):
    """
    Displays basic scheduler statistics (e.g. migrations, yields, idle time)
    at the end of the workload run
    """
    
    def __init__(self, vms, name=""):
        BasicTest.__init__(self, vms)
        self.name = name
    def results(self):
        sched = parse_sched.load_data()
        numVcpus, totalMigs, totalSwitch = 0, 0, 0
        for v in self.vms:
            v.setSchedData(sched)
            totalMigs += v.getMigs()
            totalSwitch += v.getSwitch()
            numVcpus += v.numvcpus
        totalYields = reduce(operator.add, [int(p.data["yield"]) for p in sched.pcpu_list])
        idleTotal = 0.0
        numPcpus = len(sched.pcpu_list)
        nidle = 0
        for v in sched.vcpu_list:
            if v.name.find("idle") >= 0:
                nidle += 1
                idleTotal += float(v.usedsec)
        assert nidle == numPcpus

        resmap = {}
        resmap["switch"] = totalSwitch / RUN_TIME
        resmap["pmigs"] = totalMigs / RUN_TIME
        resmap["yields"] = totalYields / RUN_TIME
        resmap["idle"] = idleTotal / RUN_TIME

        return resmap, []
    
    def getTestElements(self):
        return ["switch", "pmigs", "yields", "idle"]


def checkAffinity(vm):
    """Verify that this vm is running on a pcpu permitted by its affinity"""
    errs = []
    res = True
    if vm.affinity == None:
        return True, ""
    elif type(vm.affinity) == types.StringType:
        allaffin = vm.affinity.split(",")
        for v in vm.vcpus:
            p = v.cpu
            if p not in allaffin:
                errs.append("affinity=%s, running on pcpu %d\n" % (str(vm.affinity), p))
                res = False
    else:
        if type(vm.affinity) == types.IntType:
            affinlst = [vm.affinity]
        else:
            affinlst = vm.affinity
        for i in range(0,vm.numvcpus):
            p = int(vm.vcpus[i].cpu)
            if p != affinlst[i]:
                errs.append("affinity=%s, affinlist[i]=%d, vcpu=%d, running on pcpu: %d\n" % (str(vm.affinity), affinlst[i], i, p))
                res = False
    return res, errs
        
    
class AffinityTortureTest(BasicTest):
    def __init__(self, vms, name="", numreps=10):
        BasicTest.__init__(self, vms)
        self.name = name
        self.numreps = numreps
        self.errs = []
        self.ncpus = get_num_cpus()
    def results(self):
        resmap = { "affintorture" : 0.0 }
        return resmap, self.errs
    def randAffinity(self, numvcpus):
        if random.randrange(0,4) == 0:
            # affinity to all cpus
            return ",".join(map(str, range(0,self.ncpus[1])))
        else:
            # unique, random pcpu for each vcpu
            r = range(0, self.ncpus[1])
            res = []
            for i in range(0,numvcpus):
                c = random.choice(r)
                res.append(c)
                r.remove(c)
            return res
    def run(self):
        for i in range(0,self.numreps):
            parse_sched.reset_stats()
            sleeptime = (random.random() + 0.5) * RUN_TIME / float(self.numreps)
            time.sleep(sleeptime)
            data = parse_sched.load_data()
            for v in self.vms:
                v.setSchedData(data)
                res, newerrs = checkAffinity(v)
                if not res:
                    self.errs.extend(newerrs)
                if v.getVcpuVal("usedsec", float) < 0.01:
                    self.errs.append("vm " + v.vcpu + " stuck, usedsec = " + str(v.usedsec))
                oldaffin = v.affinity
                v.affinity = self.randAffinity(v.numvcpus)
                v.configAffinity()
    def getTestElements(self):
        return []
            
        

class MinMaxTest(BasicTest):
    """
    Basically an enhanced version of FairnessTest that takes into account
    the 'min' and 'max' values of the VM
    """
    
    def __init__(self, vms, name="", maxErr=0.1):
        BasicTest.__init__(self, vms)
        self.name = name
        self.maxErr = maxErr
        phys, logical = get_num_cpus()
        if phys == (logical/2):
            # hyperthreading on
            self.baseShares = 20000.0
        else:
            # no HT
            self.baseShares = 10000.0
    def results(self):
        err = []
        resmap = {}
        sched = parse_sched.load_data()
        for i in range(0, len(self.vms)):
            v = self.vms[i]
            v.setSchedData(sched)
            amin = v.getVMVal("min")
            amax = v.getVMVal("max")
            uptime = v.getVMVal("uptime", float)
            
            minUsage = float(uptime * amin) / 100.0
            maxUsage = float(uptime * amax) / 100.0
            
            usedsec = v.getVcpuVal("usedsec", float)
            if amin and usedsec < minUsage * (1.0 - self.maxErr):
                err.append( 
                    TestError("below min usage: %-3.3f used / %-3.3f min" % (usedsec, minUsage),
                              sched, self)
                           )
            if amax and usedsec > maxUsage * (1.0 + self.maxErr):
                err.append(
                    TestError("exeeded max usage: %-3.3f used / %-3.3f max" % (usedsec, maxUsage),
                              sched, self)
                              )
            if minUsage == 0.0:
                minRatio = 0.0
            else:
                minRatio = usedsec / minUsage
            if maxUsage == 0.0:
                maxRatio = 0.0
            else:
                maxRatio = usedsec / maxUsage
            resmap["minratio-" + v.shortFormat() + "#" + str(i)] = minRatio
            resmap["maxratio-" + v.shortFormat() + "#" + str(i)] = maxRatio
        return resmap, err
    
    def getTestElements(self):
        lst = []
        for i in range(0, len(self.vms)):
            v = self.vms[i]
            lst.append("minratio-" + v.shortFormat() + "#" + str(i))
            lst.append("maxratio-" + v.shortFormat() + "#" + str(i))
        return lst
                

class FairnessTest(BasicTest):
    """
    Run the specified workload and compare per-vm usedsec
    against the 'fair' usedsec that should be achieved.
    If '|1.0 - observedFairness| > maxUnfairness', raise
    an error.
    Theoretical fairness is determined by dividing a VM's
    allocated shares by the total number of shares in the
    workload group and multiplying by the total number of
    usedsec within the workload group. Thus, other worlds,
    such as the console, are not considered. This (relative)
    measure of fairness seems to be the most accurate.metric.
    
    Note that min/max values are not taken into account in
    this test. Nor or base-share distortions, including
    infeasible allocations. For example, if you give
    a 1-way VM enough shares that it deserves more than
    1 cpu's full time, this test will probably register
    an error.
    """
    def __init__(self, vms, name="", maxUnfairness=0.1):
        BasicTest.__init__(self, vms)
        self.name = name
        self.maxUnfairness = maxUnfairness

    def results(self):
        resultstr = ""
        errors = []
        sched = parse_sched.load_data()
        totalUsed = 0.0
        totalShares = 0
        for v in self.vms:
            v.setSchedData(sched)
            totalShares += v.shares
            totalUsed += v.getUsedSec()

        assert totalUsed > 0.0
        assert totalShares > 0
        resmap = {}
        for i in range(0, len(self.vms)):
            v = self.vms[i]
            assert v.shares > 0
            shareFraction = float(v.shares) / float(totalShares)
            fairness = (v.getUsedSec() / totalUsed) / shareFraction
            if abs(fairness - 1.0) > self.maxUnfairness:
                errors.append(FairnessError("unfairness limit exceeded", v, fairness, sched))

            resmap[v.shortFormat() + "#" + (str(self.vms.index(v)))] = fairness
        
        return (resmap, errors)
    
    def getTestElements(self):
        lst = []
        for i in range(0, len(self.vms)):
            v = self.vms[i]
            lst.append(v.shortFormat() + "#" + str(i))
        return lst


def run_batch_tests(testList, verbose=0):
    for num in range(0,len(testList)):
        test = testList[num]

        # actually run the test
        test.setup()
        test.run()
        res, errors = test.results()
        test.shutdown()

        typename = str(test.__class__).split(".")[-1]
        suitename = test.name

        # indicate whether suite passed or failed
        if len(errors) == 0:
            print "STATUS=PASS"
        else:
            print "STATUS=FAIL"

        # store result for each element
        elems = test.getTestElements()
        for e in elems:
            if res.has_key(e):
                print "%s = %f" % (e, res[e])


def run_tests(testList, verbose=0, logPrefix=None):
    """
    Executes each test in testlist, optionally logging results
    to files preceeded by the prefix 'logPrefix'.
    If 'verbose' is 1, prints result data to stdout.
    """
    for num in range(0,len(testList)):
        # run all tests if no limits specified
        test = testList[num]

        test.setup()
        test.run()
        res, errors = test.results()
        test.shutdown()

        if len(errors) == 0:
            print "Test [%d]: %s PASSED" % (num, test.name)
        else:
            print "Test [%d]: %s FAILED" % (num, test.name)
            for err in errors:
                errstr = "ERROR (%s): *** %s ***" % (test.name, err)
                print errstr
        if len(errors) > 0 and verbose > 1:
            print errors[0].sched

        # note that tests (such as vmkstats) may ALWAYS want
        # to print verbose results
        if verbose or len(errors) > 0 or test.alwaysVerbose:
            print test.format_results(res)

        # separator
        print
        print "-" * 100
        print


class CpuBurner(threading.Thread):
    """Class for a thread that burns cpu time until self.die is set"""
    def run(self):
        self.die = 0
        x = 1
        while self.die != 1:
            x = (x * 2) - 1


def show_help():
    print USAGE
    print
    print "Available test suite names:"
    suitenames = schedtests.get_suite_names()
    for t in suitenames:
        print "    ", t
    sys.exit(-1)


# main function
if __name__ == "__main__":
    stop_timer_worlds()

    try:
        opts, args = getopt.getopt(sys.argv[1:], "Nht:n:vVBS:b")
    except:
        print "invalid options"
        show_help()

    verbose = 0
    testLimits = None
    burner = None
    batchTest = False
    printName = 0

    # basic option parsing
    for o in opts:
        optname, val = o
        if optname == "-h" or optname == "-?":
            show_help()
        elif optname == "-v":
            assert verbose == 0, "Cannot combine -v and -V"
            verbose = 1
        elif optname == "-N":
            printName = 1
        elif optname == "-t":
            try:
                RUN_TIME = int(val)
            except:
                print "Invalid format for runtime"
                sys.exit(-1)
        elif optname == "-S":
            TEST_SIZE_FACTOR = int(val)
            print "multiply test sizes by ", TEST_SIZE_FACTOR
        elif optname == "-B":
            print "will use cpu-burner thread in console"
            burner = CpuBurner()
        elif optname == "-V":
            assert verbose == 0, "Cannot combine -v and -V"
            verbose = 2
        elif optname == "-n":
            testLimits = map(int, val.split(","))
        elif optname == "-b":
            batchTest = True
        else:
            print "unknown option: ", optname
            show_help()

    # generate all requested tests (but don't start anything)
    if "all" in args:
        returnedTests = schedtests.get_all_tests()
    else:
        returnedTests = []
        for t in args:
            returnedTests.extend(schedtests.get_tests(t))

    # filter tests based on -n TESTLIST option
    testsToRun = []
    for num in range(0, len(returnedTests)):
        if testLimits is None or num in testLimits:
            testsToRun.append(returnedTests[num])

    if len(testsToRun) == 0:
        print "no tests selected!"
        show_help()
    else:
        if burner:
            burner.start()
            
        # actually do the testing
        if batchTest:
            run_batch_tests(testsToRun)
        elif printName:
            for t in testsToRun:
                print t.name
        else:
            run_tests(testsToRun, verbose, testLimits)
        
        if burner:
            burner.die = True
        
    
