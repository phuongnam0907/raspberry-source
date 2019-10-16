#!/bin/bash

USR_LIST=`top -b -n 1 | grep "CPU\|Cpu" | awk '{print $2}' | head -n 1`
cpu_usr=${USR_LIST//[,.%]/}

SYS_LIST=`top -b -n 1 | grep "CPU\|Cpu" | awk '{print $4}' | head -n 1`
cpu_sys=${SYS_LIST//[,.%]/}

echo $(($cpu_usr + $cpu_sys))