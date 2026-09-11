#!/bin/sh
# T0 done test, run inside the virtme-ng guest.
# Done test: `modprobe rust_minimal` loads and unloads cleanly, and KASAN,
# lockdep and kmemleak are actually live rather than merely configured.
#
# The only done test that does not involve koru at all: it validates the dev
# kernel itself, so run it after any kernel config change.

. "$(dirname "$0")/common.sh"

dt_begin "T0" "the dev kernel itself"

echo
echo "=== 1. modprobe rust_minimal ==="
modprobe rust_minimal; echo "modprobe rc=$?"
lsmod | grep rust_minimal || echo "NOT LISTED IN lsmod"
rmmod rust_minimal; echo "rmmod rc=$?"
lsmod | grep -q rust_minimal && echo "STILL LOADED AFTER RMMOD" || echo "unloaded ok"

echo
echo "=== 2. init/exit messages ==="
dmesg | grep -i rust_minimal

echo
echo "=== 3. KASAN live ==="
dmesg | grep -i 'KernelAddressSanitizer' || echo "NO KASAN INIT LINE"

echo
echo "=== 4. lockdep live ==="
if [ -e /proc/lockdep ]; then
	echo "/proc/lockdep present"
	grep -i 'lockdep' /proc/lockdep_stats 2>/dev/null | head -2
else
	echo "NO /proc/lockdep"
fi

echo
echo "=== 5. kmemleak scan ==="
if [ -e /sys/kernel/debug/kmemleak ]; then
	echo scan > /sys/kernel/debug/kmemleak
	echo "scan rc=$?"
	n=$(grep -c 'unreferenced object' /sys/kernel/debug/kmemleak 2>/dev/null)
	echo "unreferenced objects: ${n:-0}"
else
	echo "NO /sys/kernel/debug/kmemleak"
fi

echo
echo "=== 6. taint ==="
# Not dt_taint: no out-of-tree module is loaded here, so 0 is the right answer.
cat /proc/sys/kernel/tainted

dt_splat
dt_verdict COMPLETE
