#!/bin/sh
# SPDX-License-Identifier: MIT
#
# The guest side of the check. Runs inside the virtme-ng guest, never on the
# host: a module bug panics the machine.
#
# Prints exactly one KORU-CHECK-PASS or KORU-CHECK-FAIL marker, which
# scripts/run.sh greps for. Every check below gates that marker.

# Derived from this script's own path. An absolute path here is what silently
# broke all thirteen of the scripts this replaced when the project was renamed.
ROOT=$(cd "$(dirname "$0")/.." && pwd)
KO=${KO:-$ROOT/kernel/koru.ko}
BIN=$ROOT/test/koru_check

fail=0
fence() { echo "koru-check: $1" > /dev/kmsg 2>/dev/null; echo; echo "=== $1 ==="; }

echo "=== koru check ==="
uname -r
mount -t debugfs none /sys/kernel/debug 2>/dev/null

# A missing debug facility must fail the run, not quietly turn it into a
# non-debug one.
fence "environment"
dmesg | grep -qi 'KernelAddressSanitizer' || { echo "NO KASAN"; fail=1; }
[ -e /proc/lockdep ] || { echo "NO /proc/lockdep"; fail=1; }
[ -e /sys/kernel/debug/kmemleak ] || { echo "NO kmemleak"; fail=1; }
[ "$fail" -eq 0 ] && echo "KASAN, lockdep and kmemleak all live"

fence "insmod"
insmod "$KO" || { echo "INSMOD FAILED"; echo; echo "=== KORU-CHECK-FAIL ==="; exit 1; }
perm=$(stat -c '%a %U %G' /dev/koru 2>/dev/null)
if [ "$perm" = "600 root root" ]; then
	echo "/dev/koru $perm"
else
	echo "UNEXPECTED PERMS: $perm"
	fail=1
fi
modinfo "$KO" | grep -E '^(name|license|description|vermagic)'

fence "koru_check"
[ -x "$BIN" ] || { echo "NOT BUILT: $BIN (run make -C test)"; echo; echo "=== KORU-CHECK-FAIL ==="; exit 1; }
out=$("$BIN" "$@" 2>&1)
rc=$?
echo "$out"
echo "koru_check exit: $rc"
[ "$rc" -eq 0 ] || fail=1
heavy_end=$(echo "$out" | sed -n 's/^KORU-HEAVY-END-MS //p' | tail -1)

# The two properties that need a second process driving rmmod from outside.
fence "module pinning"
"$BIN" hold > /tmp/koru-hold.out 2>&1 &
hold_pid=$!
n=0
while [ $n -lt 2000 ] && ! grep -q READY /tmp/koru-hold.out 2>/dev/null; do
	n=$((n + 1))
done
if rmmod koru 2>/dev/null; then
	echo "rmmod with an open fd SUCCEEDED (must be refused)"
	fail=1
	insmod "$KO"
else
	echo "rmmod with an open fd: refused"
fi
kill -9 $hold_pid 2>/dev/null
wait $hold_pid 2>/dev/null

fence "release cancels queued work"
# The timing is the assertion: waiting the delays out would also end with rmmod
# succeeding. This step unloads the module, so there is no separate rmmod below.
"$BIN" pending > /tmp/koru-pending.out 2>&1
i=0
while [ $i -lt 20 ]; do
	rmmod koru 2>/dev/null && break
	sleep 1
	i=$((i + 1))
done
if lsmod | grep -q '^koru'; then
	echo "still loaded after ${i}s: close(fd) did not release the queued work"
	fail=1
elif [ $i -gt 2 ]; then
	echo "rmmod took ${i}s: the delays were waited out, not cancelled"
	fail=1
else
	echo "rmmod succeeded after ${i}s: queued work was cancelled at close"
fi
[ -e /dev/koru ] && { echo "/dev/koru STILL PRESENT AFTER RMMOD"; fail=1; } \
	|| echo "device node gone"

# kmemleak reports nothing about an object younger than MSECS_MIN_AGE (5 s), so
# top the window up from the binary's own heavy-phase stamp rather than paying a
# flat sleep. The scan runs twice: the first pass after heavy allocation is not
# settled.
fence "kmemleak"
if [ -n "$heavy_end" ]; then
	now=$(date +%s%3N)
	age=$((now - heavy_end))
	echo "heavy phase ended ${age}ms ago"
	if [ "$age" -lt 5500 ]; then
		top=$(((5500 - age + 999) / 1000))
		echo "topping the window up by ${top}s"
		sleep "$top"
	fi
else
	echo "no heavy-phase stamp; sleeping the full window"
	sleep 6
fi
echo scan > /sys/kernel/debug/kmemleak
echo scan > /sys/kernel/debug/kmemleak
leaks=$(grep -c 'unreferenced object' /sys/kernel/debug/kmemleak 2>/dev/null)
leaks=${leaks:-0}
echo "unreferenced objects: $leaks"
[ "$leaks" -eq 0 ] || { head -30 /sys/kernel/debug/kmemleak; fail=1; }

# TAINT_OOT_MODULE is bit 12. Module signing is off in this kernel, so bit 13
# must never appear; anything but 4096 is a finding.
fence "taint"
t=$(cat /proc/sys/kernel/tainted)
if [ "$t" = "4096" ]; then
	echo "taint OK: $t"
else
	echo "UNEXPECTED TAINT: $t (expected 4096)"
	fail=1
fi

# This gates the verdict. T10 once shipped a real lockdep deadlock behind a PASS
# because the check only printed. The last pattern is a pr_warn_ratelimited
# rather than a WARN, so it matches nothing without being named.
fence "dmesg scan"
splat=$(dmesg | grep -v 'koru-check:' \
	| grep -iE 'BUG:|WARNING:|Oops|general protection|KASAN:|possible circular locking|INFO: task|not supported for file' \
	| grep -v 'Platform Limit')
[ -z "$splat" ] || { echo "$splat" | head -20; fail=1; }
echo "(end)"

echo
if [ "$fail" -eq 0 ]; then
	echo "=== KORU-CHECK-PASS ==="
else
	echo "=== KORU-CHECK-FAIL ==="
fi
