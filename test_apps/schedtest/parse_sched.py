#!/usr/bin/python
#
# Copyright 2003, VMware, Inc.
#
"""
Basic routines to parse /proc/vmware/sched cpu.

The most important function is load_data(), which
returns a SchedData object corresponding to the current
scheduler data
"""

import os, os.path, sys, re
from string import split

class LineData:
    """
    LineData encapsulates a single line from a larger table,
    matching headers with values
    """
    def __init__(self, headers=None, cur_data=None):
        self.data = { }
        if headers is None and cur_data is None:
            return        
        if len(headers) != len(cur_data):
            raise "Mismatched data and headers"
        for i in range(0,len(headers)):
            self.data[headers[i]] = cur_data[i]

    def __repr__(self):
        mystr = ""
        for k in self.data.keys():
            mystr = mystr + k + " = "  + self.data[k] + ","
        return mystr

    def __getattr__(self, attr):
        if (self.data.has_key(attr)):
            return self.data[attr]
        else:
            return 0
    def __sub__(self,other):
        newguy = LineData()
        for attr in data.keys():
            if self.data[attr].isdigit():
                s = int(self.data[attr])
                o = int(other.data[attr])
                newguy.data[attr] = s - o
            elif self.data[attr].find(".") >= 0:
                s = float(self.data[attr])
                o = float(other.data[attr])
                newguy.data[attr] = s - o
            else:
                # not a numeric type, can't add/subtract
                newguy.data[attr] = "N/A"
        return newguy


class SchedData:
    """SchedData is simply a wrapper for a vcpu_list and a pcpu_list"""
    def __init__(self, vcpu_list, pcpu_list, vcpulines=None, global_data=None):
        self.vcpu_list = vcpu_list
        self.pcpu_list = pcpu_list
        self.vcpulines = vcpulines
        self.global_data = global_data
    def __repr__(self):
        results = "".join(self.vcpulines)
        return results


def filter_vcpu_name(vcpu_list, vname):
    """Returns a list of vcpus in vcpu_list with names containing the substring vname"""
    newList = filter(lambda v: vcpu.name.find(vname) > 0,  vcpu_list)

def find_vcpu(vcpu_list,vcpu_num):
    f = lambda vc,num=vcpu_num: (vc.vcpu == num)
    remaining = filter(f, vcpu_list)
    if len(remaining) > 0:
        return remaining[0]
    else:
        return 

def set_shares(vm,shares):
    vmfile = open("/proc/vmware/vm/%d/cpu/shares" % (vm), "w")
    vmfile.write(shares)
    
def set_affinity(vm,affin):
    vmfile = open("/proc/vmware/vm/%d/cpu/affinity" % (vm), "w")
    vmfile.write(affin)

def make_verbose():
    configfile = open("/proc/vmware/config/CpuProcVerbose", 'w')
    configfile.write("1")
    
def reset_stats():
    schedfile = open('/proc/vmware/sched/reset-stats', 'w')
    schedfile.write('reset')
    schedfile.close()

def load_data(filename='/proc/vmware/sched/cpu-verbose', verbose=1):
    """
    Returns a SchedData object corresponding to the current scheduler data

    This function parses the 'cpu-verbose' proc node, so it may take a little
    while, especially with large numbers of vcpus. 
    """
    assert os.path.exists(filename), "Are you sure you have vmkernel loaded?"
    vcpu_list = []
    pcpu_list = []
    
    notblank = re.compile('\s*\d+')
    
    infile = open(filename, 'r')
    vcpulines = []
    if verbose:
        global_headers = split( infile.readline() )
        global_line = split( infile.readline() )
        global_data = LineData(global_headers, global_line)
        
        infile.readline() # blank

        # skip over cell data
        curline = infile.readline()
        while notblank.match(infile.readline()):
            pass
    
        pcpu_headers = split( infile.readline() )
        
        curline = infile.readline()
        
        while notblank.match(curline):
            pcpu_list.append( LineData(pcpu_headers, split(curline)) )
            curline = infile.readline()

    # vcpus are in both verbose and non-verbose
    vcpu_headers = split( infile.readline() )
    curline = infile.readline()
    while notblank.match(curline):
        try:
            vcpulines.append( curline )
            item = LineData(vcpu_headers, split(curline))
            vcpu_list.append( item  )
        except:
            print "Failed to parse: ", curline

        curline = infile.readline()

    return SchedData(vcpu_list, pcpu_list, vcpulines, global_data)


def parse_table_file(filename):
    """Splits a table into a series of LineData objects."""
    
    f = open(filename, 'r')
    lines = f.readlines()
    
    item_list = []
    headers = split(lines[0])
    for curline in lines[1:]:
        if notblank.match(curline):
            try:
                item = LineData(headers, split(curline))
                item_list.append(item)
            except:
                pass
    return item_list


if __name__ == "__main__":
    make_verbose()
    sched = load_data()    
    print "%7s:\t%8s\t%6s\t%4s\t%12s" % ("smp-cpu", "name", "affin", "cpu", "usedsec")
    for v in sched.vcpu_list:
        print "%3d-%3d:\t%8s\t%6s\t%4d\t%8.3f" %\
              (int(v.vm), int(v.vcpu), v.name, v.affinity, int(v.cpu), float(v.usedsec))


