#!/bin/sh
# T1 done test, run inside the virtme-ng guest.
# Done test: insmod/rmmod cycle, init/exit prints, no unexpected taint.
#
# TAINT_OOT_MODULE is bit 12 (include/linux/panic.h), so 4096 is expected and is
# the only bit that may appear. CONFIG_MODULE_SIG is off in this kernel, so
# TAINT_UNSIGNED_MODULE (bit 13, 8192) must never show up.

. "$(dirname "$0")/common.sh"

dt_begin "T1" "insmod/rmmod koru.ko"

echo
echo "=== taint before ==="
cat /proc/sys/kernel/tainted

echo
echo "=== insmod ==="
insmod "$KO"; echo "insmod rc=$?"
lsmod | grep '^koru' || echo "NOT LISTED IN lsmod"

echo
echo "=== modinfo ==="
modinfo "$KO" | grep -E '^(name|license|description|vermagic)'

dt_taint
dt_rmmod
lsmod | grep -q '^koru' && echo "STILL LOADED AFTER RMMOD" || echo "unloaded ok"

echo
echo "=== init/exit messages ==="
dmesg | grep 'koru:'

dt_splat
dt_verdict COMPLETE
