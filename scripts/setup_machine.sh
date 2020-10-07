#!/bin/bash
# run with sudo

# needed for the iokernel's shared memory
sysctl -w kernel.shm_rmid_forced=1
sysctl -w kernel.shmmax=18446744073692774399
sysctl -w vm.hugetlb_shm_group=27
sysctl -w vm.max_map_count=16777216
sysctl -w net.core.somaxconn=3072

# set up the ksched module
rmmod ksched
rm /dev/ksched
insmod $(dirname $0)/../ksched/build/ksched.ko
mknod /dev/ksched c 280 0
chmod uga+rwx /dev/ksched

# reserve huge pages
echo 5192 > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages
echo 0 > /sys/devices/system/node/node1/hugepages/hugepages-2048kB/nr_hugepages
for n in /sys/devices/system/node/node[2-9]; do
	echo 0 > $n/hugepages/hugepages-2048kB/nr_hugepages
done

# load msr module
modprobe msr

for i in {0..63}; do sudo wrmsr -p$i 0x1a0 0x4000850089; done

for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo performance > $f
done
