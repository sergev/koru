# SPDX-License-Identifier: GPL-2.0
#
# Shared guest-side harness for the done tests. Sourced, never run directly.
#
# Keep this file small and obvious. Every done test's pass condition goes
# through it, so a bug here weakens all of them at once.

# Derived from the calling script's own path. An absolute path here is what
# silently broke all thirteen scripts when the project was renamed.
DT_ROOT=$(cd "$(dirname "$0")/.." && pwd)

# Overridable so a negative control can point at a deliberately broken build.
KO=${KO:-$DT_ROOT/kernel/koru.ko}

dt_fail=0
dt_rc=0

# dt_begin <tag> <one-line description>
dt_begin() {
	DT_TAG=$1
	echo "=== $DT_TAG done test: $2 ==="
	uname -r
	mount -t debugfs none /sys/kernel/debug 2>/dev/null
}

dt_insmod() {
	echo
	echo "=== insmod ==="
	insmod "$KO" || { echo "INSMOD FAILED"; exit 1; }
	ls -l /dev/koru
}

# dt_run <path under the repo root> [args...]
dt_run() {
	prog=$1
	shift
	name=$(basename "$prog")
	echo
	echo "=== $name ==="
	"$DT_ROOT/$prog" "$@"
	dt_rc=$?
	echo "$name exit: $dt_rc"
	[ "$dt_rc" -eq 0 ] || dt_fail=1
}

# kmemleak never reports an object younger than MSECS_MIN_AGE (5 s), so an
# immediate scan is blind however badly the code leaks. Sleep past that age,
# then scan twice: the first pass after heavy allocation is not settled.
dt_kmemleak() {
	echo
	echo "=== kmemleak ==="
	sleep 8
	echo scan > /sys/kernel/debug/kmemleak
	echo scan > /sys/kernel/debug/kmemleak
	dt_leaks=$(grep -c 'unreferenced object' /sys/kernel/debug/kmemleak 2>/dev/null)
	dt_leaks=${dt_leaks:-0}
	echo "unreferenced objects: $dt_leaks"
	[ "$dt_leaks" -eq 0 ] || { head -30 /sys/kernel/debug/kmemleak; dt_fail=1; }
}

# A leaked module reference shows up here and nowhere else.
dt_rmmod() {
	echo
	echo "=== rmmod ==="
	rmmod koru
	rc=$?
	echo "rmmod rc=$rc"
	[ "$rc" -eq 0 ] || dt_fail=1
}

# Expected taint is exactly TAINT_OOT_MODULE. Module signing is off in this
# kernel, so bit 13 must never appear; anything but 4096 is a finding.
dt_taint() {
	echo
	echo "=== taint ==="
	t=$(cat /proc/sys/kernel/tainted)
	if [ "$t" = "4096" ]; then
		echo "taint OK: $t"
	else
		echo "UNEXPECTED TAINT: $t (expected 4096)"
		dt_fail=1
	fi
}

# A lockdep or KASAN splat must fail the run, not merely print. T10 shipped a
# real deadlock behind a PASS because this check was only advisory. The pattern
# also matches `not supported for file`, which is a pr_warn_ratelimited rather
# than a WARN and so matched nothing before.
dt_splat() {
	echo
	echo "=== oops/warnings/KASAN/lockdep ==="
	splat=$(dmesg | grep -iE 'BUG:|WARNING:|Oops|general protection|KASAN:|possible circular locking|INFO: task|not supported for file' | grep -v 'Platform Limit')
	[ -z "$splat" ] || { echo "$splat" | head -20; dt_fail=1; }
	echo "(end)"
}

# dt_verdict [word]  -- the word printed on success; FAIL otherwise.
#
# The trailing marker is what donetest/run.sh greps for, so every script must
# print exactly one.
dt_verdict() {
	echo
	if [ "$dt_fail" -eq 0 ]; then
		echo "=== $DT_TAG-DONE-TEST-${1:-PASS} ==="
	else
		echo "=== $DT_TAG-DONE-TEST-FAIL ==="
	fi
}

# The standard tail: everything after the test binary has run.
dt_finish() {
	dt_kmemleak
	dt_rmmod
	dt_taint
	dt_splat
	dt_verdict "$@"
}
