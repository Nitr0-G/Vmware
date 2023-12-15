#
# Copyright 2003, VMware Inc.
#
#

"""
    This module defines scheduler regression tests

    It can not be run directly.
"""


__author__ = "John Zedlewski (johnz@vmware.com)"

__version__ = "1.0"
__copyright__ = "VMware, Inc. 2003"


from runschedtests import *
from copy import deepcopy

def gen_worlds(numWorlds, exampleWorld, variedShares=0):
    """
    returns a list of 'numWorlds' worlds, which are deep-copy
    clones of 'exampleWorld.' If 'variedShares' is 1, then each
    world's shares are set to 1000*(listIndex+1).
    """    
    vms = []
    # start i at 1 so we can give (i * 1000) to vms
    for i in range(1, numWorlds + 1):
        newWorld = deepcopy(exampleWorld)
        if variedShares:
            newWorld.shares = i * 1000
        vms.append(newWorld)
    return vms

def get_perfstats_tests():
    perfstatsTests = []

    vms = []
    for i in range(1,4):
        vms.append(TimerWorld(3,1, shares= i * 1000, numvcpus = 2))
        vms.append(TimerWorld(3,1, shares= i * 1000, numvcpus = 1))

    perfstatsTests.append( PerfStatsTest(vms, "3x3-1 dual, 3x3-1 uni  timerworld VMs") )

    cpuDual = gen_worlds(13, BasicWorld(10, 0, numvcpus=2))
    perfstatsTests.append( PerfStatsTest(cpuDual, "13 cpu-bound dual-cpu basic VMs") )

    cpuUni = gen_worlds(13, BasicWorld(10, 0, numvcpus=1))
    perfstatsTests.append( PerfStatsTest(cpuUni, "13 cpu-bound basic VMs") )

    timerUni = gen_worlds(21, TimerWorld(5, 5, numvcpus=1))
    perfstatsTests.append( PerfStatsTest(timerUni, "21 5-5 uni timerworld VMs") )

    timerDual = gen_worlds(11, TimerWorld(5, 5, numvcpus=2)) 
    perfstatsTests.append( PerfStatsTest(timerDual, "11 5-5 dual-cpu timerworld VMs") )

    return perfstatsTests
                          

def get_minmax_tests():
    minmaxTests = []
    vms = []
    for i in range(0,3):
        newworld =  BasicWorld(10, 0, shares=1000, numvcpus=1)
        newworld.min = 40
        vms.append(newworld)
    # throw in a big guy to try and mess things up
    vms.append(BasicWorld(10,0, shares=8000, numvcpus=1))

    minmaxTests.append(MinMaxTest(vms, "basicMin"))

    vms = []
    for i in range(0,3):
        newworld =  BasicWorld(10, 0, shares=1000, numvcpus=1)
        newworld.max = 25
        vms.append(newworld)
    minmaxTests.append(MinMaxTest(vms, "underCommitMax"))

    vms = []
    for i in range(1,5):
        newworld = TimerWorld(5, 5, shares=i * 1000, numvcpus=1)
        newworld.min = 20
        newworld.max = 40
        vms.append(newworld)
    minmaxTests.append(MinMaxTest(vms, "4uni-MinMax+variedShares"))

    dual1 = TimerWorld(10, 5, shares=1000, numvcpus=2)
    dual1.min=120
    dual2 = TimerWorld(10, 5, shares=3000, numvcpus=2)
    
    uni1 = TimerWorld(10, 5, shares=2000, numvcpus=1)
    uni2 = TimerWorld(10, 5, shares=6000, numvcpus=1)
    uni2.max = 50

    vms = [dual1,dual2,uni1,uni2]
    minmaxTests.append(MinMaxTest(vms, "uni-Dual-minmax-mix"))
    
    return minmaxTests

def get_affinity_tests():
    affinTests = []
    vms = [ BasicWorld(10, 0, shares=1000, affinity=1),
               BasicWorld(10, 0, shares=4000, affinity=1),
               BasicWorld(10, 0, shares=4000, affinity=1),
               BasicWorld(10, 0, shares=4000, affinity=1),
               BasicWorld(10, 0, shares=12000, affinity=1) ]

    affinTests.append(FairnessTest(vms, "1pcpuAffinityFairness", maxUnfairness=.15))

    vms = [ TimerWorld(10, 2, shares=1000, affinity=1),
            TimerWorld(10, 2, shares=5000, affinity=1),
            TimerWorld(10, 2, shares=10000, affinity=1),
            TimerWorld(10, 2, shares=5000, affinity=0),
            TimerWorld(10, 2, shares=5000, affinity=0),
            TimerWorld(10, 2, shares=6000, affinity=0), ]
    
    affinTests.append(FairnessTest(vms, "2pcpuBalancedAffinity"))

    vms = [ TimerWorld(10, 2, numvcpus=2, shares=1000, affinity=[0,1]),
            TimerWorld(10, 2, numvcpus=2, shares=5000, affinity=[1,0]),
            TimerWorld(10, 2, numvcpus=2, shares=10000, affinity=[0,1]) ]
    
    affinTests.append(FairnessTest(vms, "2wayBalancedAffinity"))

    return affinTests

def get_affinitytorture_tests():
    tortureTests = []
    exampleUni = BasicWorld(10, 0, shares=1000)
    vms = gen_worlds(9, exampleUni)
    tortureTests.append(AffinityTortureTest(vms, "9unisCpubndTorture"))
    
    exampleSmp = BasicWorld(10, 0, shares=1000, numvcpus=2)
    vms = gen_worlds(5, exampleSmp)
    tortureTests.append(AffinityTortureTest(vms, "5dualsCpubndTorture"))

    exampleSmp = TimerWorld(5, 5, shares=1000, numvcpus=2)
    vms = gen_worlds(5, exampleSmp)
    
    exampleUni = TimerWorld(8, 2, shares=2000, numvcpus=1)
    vms.extend(gen_worlds(4, exampleUni))
    tortureTests.append(AffinityTortureTest(vms, "mixedUniSmpTorture"))

    return tortureTests

def get_vmkstats_tests():
    vmkstatsTests = []
    vms = []
    for i in range(1,14):
        vms.append(BasicWorld(10, 0, shares= i * 1000, numvcpus = 1))

    timerDual = gen_worlds(11, TimerWorld(1, 1, numvcpus=2)) 
    vmkstatsTests.append( VmkstatsTest(timerDual, "12 1-1 dual-cpu timerworld VMs, vmkstats") )
    
    cpuUni = gen_worlds(13, BasicWorld(10, 0, numvcpus=1))
    vmkstatsTests.append( VmkstatsTest(cpuUni, "13 cpu-bound basic VMs, vmkstats") )

    timerDual = gen_worlds(13, TimerWorld(5, 5, numvcpus=2))
    vmkstatsTests.append( VmkstatsTest(timerDual, "13 5-5 dual timerworlds, vmkstats") )

    timerUni = gen_worlds(13, TimerWorld(5, 5, numvcpus=1))
    vmkstatsTests.append( VmkstatsTest(timerUni, "13 5-5 uni timerworlds, vmkstats") )

    timerUni = gen_worlds(21, TimerWorld(5, 5, numvcpus=1))
    vmkstatsTests.append( VmkstatsTest(timerUni, "21 5-5 uni timerworld VMs, vmkstats") )

    return vmkstatsTests

def get_htsharing_tests():
    if not os.path.exists("/proc/vmware/sched/hyperthreading"):
        return []
    htsharingtests = []
    vms = [ TimerWorld(5,5), TimerWorld(5,5), TimerWorld(5,5),
            TimerWorld(5,5,numvcpus=2), TimerWorld(5,5,numvcpus=2), TimerWorld(5,5,numvcpus=2),]
    vms[3].htsharing = "internal"
    vms[4].htsharing = "internal"

    htsharingtests.append( FairnessTest(vms, "3x5-5unis, 3x5-5duals, some 'internal' htsharing") )

    vms = [ TimerWorld(5,5), TimerWorld(5,5), TimerWorld(5,5),
            TimerWorld(5,5,numvcpus=2), TimerWorld(5,5,numvcpus=2), TimerWorld(5,5,numvcpus=2),]
    vms[0].htsharing = "none"
    vms[3].htsharing = "none"

    htsharingtests.append( FairnessTest(vms, "3x5-5unis, 3x5-5duals, some 'none' htsharing") )
    return htsharingtests

    
def get_fairness_tests():
    fairnessTests = []

    cpuUni = gen_worlds(13, BasicWorld(10, 0, numvcpus=1), variedShares=1)
    fairnessTests.append( FairnessTest(cpuUni, "13 cpu-bound basic VMs, varied shares") )

    timerUni = gen_worlds(13, TimerWorld(10, 10, numvcpus=1), variedShares=1)
    fairnessTests.append( FairnessTest(timerUni, "13x10-10VMs, varied shares") )

    vms = []
    for i in range(1,5):
        vms.append(BasicWorld(10,0,shares= i * 1000, numvcpus = 1))
        vms.append(BasicWorld(10,0,shares= i * 1000, numvcpus = 2))
    fairnessTests.append( FairnessTest(vms, "cpu bound, 4 dual, 4 uni, varied shares") )
                
    
    vms = [ TimerWorld(10,0), TimerWorld(10,0), TimerWorld(10,0) ]
    fairnessTests.append( FairnessTest(vms, "3x10-0VMs, same shares") )

    vms = [ TimerWorld(1,4, 1000), TimerWorld(1,4, 2000), TimerWorld(1, 4, 3000), TimerWorld(1, 4, 3000),
            TimerWorld(1,4, 1000), TimerWorld(1,4, 2000), TimerWorld(1, 4, 3000),
            TimerWorld(1,4, 1000), TimerWorld(1,4, 2000), TimerWorld(1, 4, 3000) ]
    fairnessTests.append( FairnessTest(vms, "10VMs excessWaiting", maxUnfairness=.4) )

    vms = [ TimerWorld(5,5), TimerWorld(5,5), TimerWorld(5,5),
            TimerWorld(5,5,numvcpus=2), TimerWorld(5,5,numvcpus=2), TimerWorld(5,5,numvcpus=2),]
    fairnessTests.append( FairnessTest(vms, "3x5-5unis, 3x5-5duals, same shares") )

    return fairnessTests


# dictionary mapping test suite names to functions
# the functions can be called to return a list of test objects

all_tests = {
    "fairness" : get_fairness_tests,
    "perfstats" : get_perfstats_tests,
    "minmax" : get_minmax_tests,
    "affinity" : get_affinity_tests,
    "vmkstats" : get_vmkstats_tests,
    "affintorture" : get_affinitytorture_tests,
    "htsharing" : get_htsharing_tests
    }


def get_tests(name):
    return all_tests[name]()

def get_all_tests():
    res = []
    for t in all_tests.keys():
        res.extend(all_tests[t]())
    return res

def get_suite_names():
    return all_tests.keys()
