#!/exit14/home/johnz/bin/python
#
# Test min admission control with affinity
#

from runschedtests import *
import sys

print "basic test, 2x 75% unis on same cpu [should fail]"
stop_timer_worlds()
tw1 = BasicWorld(10, 0, 1000)
tw2 = BasicWorld(10, 0, 1000)

tw1.start()
tw2.start()

tw1.setAffinity("1")
tw2.setAffinity("1")

tw1.configMin(75)
tw2.configMin(75)

if tw2.readMin() != 0:
    print "ERROR: allowed two 75% unis on same cpu"


stop_timer_worlds()

print "simple overcommit, 100% uni per pcpu [should fail]"

tws = []
ncpus = get_num_cpus()[1]
numLogical = get_num_cpus()[1] / get_num_cpus()[0]

for i in range(0, ncpus):
    tws.append(BasicWorld(10, 2, 1000, min=100))
    tws[-1].start()

if tws[-1].readMin() == 100:
    print "ERROR: we were able to overcommit our mins"

stop_timer_worlds()


tws = []
if ncpus < 4:
    sys.exit(0)


print "overlapping 2-ways, shouldn't be able to coschedule [should fail]"
tw1 = BasicWorld(10, 0, 1000, numvcpus=2, affinity="1,2")
tw2 = BasicWorld(10, 0, 1000, numvcpus=2, affinity="2,3")
tw1.start()
tw2.start()

tw1.configMin(180)
tw2.configMin(100)

if tw2.readMin() != 0 and tw1.readMin != 0:
    print "ERROR: allowed allocation that prevents coscheduling"
    sys.exit(-1)

stop_timer_worlds()

print "three duals with 1,2,3 affinity, 100% cpu min each [should succeed]"
tw1 = BasicWorld(10, 0, 1000, numvcpus=2, affinity="1,2,3")
tw2 = BasicWorld(10, 0, 1000, numvcpus=2, affinity="1,2,3")
tw3 = BasicWorld(10, 0, 1000, numvcpus=2, affinity="1,2,3")

tw1.start()
tw2.start()
tw3.start()

tw1.configMin(100 / numLogical)
tw2.configMin(100 / numLogical)
tw3.configMin(100 / numLogical)

if tw3.readMin() != (100 / numLogical):
    print "ERROR: three duals should have succeeded!"
    sys.exit(-1)

stop_timer_worlds()
if numLogical > 1:
    sys.exit()

# incorrect on HT systems
print "more smp affinity [should fail]"
tw1 = BasicWorld(10, 0, 1000, numvcpus=2, affinity="0,1")
tw2 = BasicWorld(10, 0, 1000, numvcpus=2, affinity="0,2")
tw3 = BasicWorld(10, 0, 1000, numvcpus=2, affinity="1,3")

tw1.start()
tw2.start()
tw3.start()

tw1.configMin(100 / numLogical)
tw2.configMin(100 / numLogical)
tw3.configMin(120 / numLogical)

if tw3.readMin() != 0:
    print "ERROR: first two cpus should be overcommitted!"
    sys.exit(-1)

stop_timer_worlds()
