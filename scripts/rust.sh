#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# The guest side of the Rust suite. Runs inside the virtme-ng guest, never on
# the host. Mirrors check.sh; that script is left alone because it carries every
# pass condition for the kernel itself.
#
# Prints exactly one KORU-RUST-PASS or KORU-RUST-FAIL marker, which
# scripts/run-rust.sh greps for. Every check below gates that marker.
#
# BIN is the test binary, built on the host and passed in by the runner.

ROOT=$(cd "$(dirname "$0")/.." && pwd)
KO=${KO:-$ROOT/kernel/koru.ko}
BIN=${BIN:-}
# A filter matching nothing exits 0, so an unfiltered run must report at least
# this many passes. Raise it when a test is added.
WANT_PASSED=${WANT_PASSED:-58}

fail=0
fence() { echo "koru-check: $1" > /dev/kmsg 2>/dev/null; echo; echo "=== $1 ==="; }

echo "=== koru rust suite ==="
uname -r
mount -t debugfs none /sys/kernel/debug 2>/dev/null

fence "environment"
dmesg | grep -qi 'KernelAddressSanitizer' || { echo "NO KASAN"; fail=1; }
[ -e /proc/lockdep ] || { echo "NO /proc/lockdep"; fail=1; }
[ -e /sys/kernel/debug/kmemleak ] || { echo "NO kmemleak"; fail=1; }
[ "$fail" -eq 0 ] && echo "KASAN, lockdep and kmemleak all live"

fence "insmod"
insmod "$KO" || { echo "INSMOD FAILED"; echo; echo "=== KORU-RUST-FAIL ==="; exit 1; }
perm=$(stat -c '%a %U %G' /dev/koru 2>/dev/null)
if [ "$perm" = "600 root root" ]; then
	echo "/dev/koru $perm"
else
	echo "UNEXPECTED PERMS: $perm"
	fail=1
fi

fence "koru-sys"
if [ ! -x "$BIN" ]; then
	echo "NOT BUILT: $BIN"
	echo "run: (cd rust && cargo test -p koru-sys --no-run)"
	echo
	echo "=== KORU-RUST-FAIL ==="
	exit 1
fi
out=$("$BIN" --test-threads=1 --nocapture "$@" 2>&1)
rc=$?
echo "$out"
echo "koru-sys exit: $rc"
[ "$rc" -eq 0 ] || fail=1

# Two ways this suite can pass without proving anything.
passed=$(echo "$out" | sed -n 's/^test result: ok\. \([0-9]*\) passed.*/\1/p' | tail -1)
passed=${passed:-0}
echo "tests passed: $passed (want at least $WANT_PASSED)"
[ "$passed" -ge "$WANT_PASSED" ] || { echo "TOO FEW TESTS RAN"; fail=1; }
# Not anchored: --nocapture makes libtest prefix the line with the test name.
skips=$(echo "$out" | grep 'KORU-RS-SKIP')
[ -z "$skips" ] || { echo "SKIPPED, which is not a pass:"; echo "$skips"; fail=1; }

fence "rmmod"
rmmod koru || { echo "RMMOD FAILED"; fail=1; }
[ -e /dev/koru ] && { echo "/dev/koru STILL PRESENT AFTER RMMOD"; fail=1; } \
	|| echo "device node gone"

# libtest orders tests by name, so this suite cannot promise the heavy-first
# ordering check.sh earns its window with. Pay the flat five seconds instead;
# leak coverage of the kernel belongs to check.sh, which is unchanged.
fence "kmemleak"
sleep 6
echo scan > /sys/kernel/debug/kmemleak
echo scan > /sys/kernel/debug/kmemleak
leaks=$(grep -c 'unreferenced object' /sys/kernel/debug/kmemleak 2>/dev/null)
leaks=${leaks:-0}
echo "unreferenced objects: $leaks"
[ "$leaks" -eq 0 ] || { head -30 /sys/kernel/debug/kmemleak; fail=1; }

fence "taint"
t=$(cat /proc/sys/kernel/tainted)
if [ "$t" = "4096" ]; then
	echo "taint OK: $t"
else
	echo "UNEXPECTED TAINT: $t (expected 4096)"
	fail=1
fi

# This gates the verdict, for the reason check.sh records.
fence "dmesg scan"
splat=$(dmesg | grep -v 'koru-check:' \
	| grep -iE 'BUG:|WARNING:|Oops|general protection|KASAN:|possible circular locking|INFO: task|not supported for file' \
	| grep -v 'Platform Limit')
[ -z "$splat" ] || { echo "$splat" | head -20; fail=1; }
echo "(end)"

echo
if [ "$fail" -eq 0 ]; then
	echo "=== KORU-RUST-PASS ==="
else
	echo "=== KORU-RUST-FAIL ==="
fi
