#!/bin/sh
export PATH=/bin
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev 2>/dev/null
echo "===GUEST_START==="
echo "BOOT_OK_KERNEL=$(uname -r)"
echo "GUEST_MODEL=$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2)"
echo -n "GUEST_CET_FLAGS="
grep -m1 flags /proc/cpuinfo | tr ' ' '\n' | grep -iE '^(ibt|shstk)$' | tr '\n' ' '
echo ""
echo "GUEST_USERNS_NS=$(ls -l /proc/self/ns/user 2>/dev/null | wc -l)"
echo "GUEST_SLAB_VIRTUAL=$(grep -c slab_virtual /proc/slabinfo 2>/dev/null || echo 0)"
echo "GUEST_IO_URING=$(grep -c io_uring /proc/kallsyms 2>/dev/null || echo 0)"
echo "GUEST_UNPRIV_BPF=$(cat /proc/sys/kernel/unprivileged_bpf_disabled 2>/dev/null || echo na)"
echo "GUEST_DMESG_CET=$(dmesg 2>/dev/null | grep -ci 'Control-flow Enforcement')"
echo "===GUEST_END==="
poweroff -f