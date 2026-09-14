#!/bin/sh
# SPDX-License-Identifier: MIT
#
# The guest side of the Rust suites. Runs inside the virtme-ng guest, never on
# the host. Mirrors check.sh; that script is left alone because it carries every
# pass condition for the kernel itself.
#
# Prints exactly one KORU-RUST-PASS or KORU-RUST-FAIL marker, which
# scripts/run-rust.sh greps for. Every check below gates that marker.
#
# SUITES is `name|binary|floor` per suite and EXAMPLES is `name|binary`, both
# built by the runner on the host.

ROOT=$(cd "$(dirname "$0")/.." && pwd)
KO=${KO:-$ROOT/kernel/koru.ko}
SUITES=${SUITES:-}
EXAMPLES=${EXAMPLES:-}
FILTERS="$*"
OUT=/tmp/koru-ex-out
ERR=/tmp/koru-ex-err
DATA=/tmp/koru-ex-data

fail=0
fence() { echo "koru-check: $1" > /dev/kmsg 2>/dev/null; echo; echo "=== $1 ==="; }

# One suite: run it, then the two gates that exist because `cargo test` can
# succeed without proving anything.
run_suite() {
	name=$1
	bin=$2
	floor=$3
	fence "suite $name"
	if [ ! -x "$bin" ]; then
		echo "NOT BUILT: $bin"
		echo "run: (cd rust && cargo test --workspace --no-run)"
		fail=1
		return
	fi
	out=$("$bin" --test-threads=1 --nocapture $FILTERS 2>&1)
	rc=$?
	echo "$out"
	echo "$name exit: $rc"
	[ "$rc" -eq 0 ] || fail=1

	# A filter matching nothing exits 0. The floor is the answer to that, and
	# it only means anything on an unfiltered run: a filter is expected to
	# match one suite's tests and none of another's.
	passed=$(echo "$out" | sed -n 's/^test result: ok\. \([0-9]*\) passed.*/\1/p' | tail -1)
	passed=${passed:-0}
	if [ -z "$FILTERS" ]; then
		echo "$name passed: $passed (want at least $floor)"
		[ "$passed" -ge "$floor" ] || { echo "TOO FEW TESTS RAN"; fail=1; }
	else
		echo "$name passed: $passed (filtered; no floor)"
	fi

	# A skip is indistinguishable from a pass. Not anchored: --nocapture makes
	# libtest prefix the line with the test name.
	skips=$(echo "$out" | grep 'KORU-RS-SKIP')
	[ -z "$skips" ] || { echo "SKIPPED, which is not a pass:"; echo "$skips"; fail=1; }
}

want_eq() {
	if [ "$2" = "$3" ]; then
		echo "ok: $1"
	else
		echo "MISMATCH $1: got [$2] want [$3]"
		fail=1
	fi
}

# Braam's hello world. Its stdout arrives two ways and both matter: a pipe is
# blocking, which koru refuses until the runtime re-opens it, and a regular
# file takes three writes at three offsets, which is the position bookkeeping.
example_hello() {
	want_eq "hello through a pipe" "$("$1" 2>$ERR)" "Hello, world!"
	want_eq "hello with an argument" "$("$1" koru 2>$ERR)" "Hello, koru!"
	"$1" file >$OUT 2>$ERR
	want_eq "hello exit status" "$?" "0"
	want_eq "hello to a regular file" "$(cat $OUT)" "Hello, file!"
}

# T15's demo on the ambient ring: the file on stdout, the timer order on
# stderr, and the timers must not have resolved in the order they were armed.
example_read_file() {
	printf 'one\ntwo\nthree\n' >$DATA
	"$1" $DATA >$OUT 2>$ERR
	want_eq "read_file exit status" "$?" "0"
	if cmp -s $OUT $DATA; then
		echo "ok: read_file wrote the file and nothing else"
	else
		echo "MISMATCH read_file bytes"
		fail=1
	fi
	want_eq "read_file timer order" "$(cat $ERR)" "10 20 30"
	want_eq "read_file through a pipe" "$("$1" $DATA 2>$ERR | cat)" "$(cat $DATA)"
}

# T32's program shell. The usage helpers are the point: which stream the block
# goes to and what the status is, neither of which a library test can see. The
# calendar's oracle is the host's own date(1).
example_date() {
	"$1" -h >$OUT 2>$ERR
	want_eq "date -h status" "$?" "0"
	want_eq "date -h to stdout" "$(head -1 $OUT)" "Usage:"
	want_eq "date -h says nothing on stderr" "$(cat $ERR)" ""
	"$1" --help >$OUT 2>$ERR
	want_eq "date --help status" "$?" "0"
	want_eq "date --help to stdout" "$(head -1 $OUT)" "Usage:"

	"$1" -x >$OUT 2>$ERR
	want_eq "date -x status" "$?" "2"
	want_eq "date -x to stderr" "$(head -1 $ERR)" "Usage:"
	want_eq "date -x says nothing on stdout" "$(cat $OUT)" ""
	"$1" a b >$OUT 2>$ERR
	want_eq "date with two operands" "$?" "2"

	before=$(date -u '+%a %b %d %H:%M')
	got=$("$1" -u 2>$ERR)
	after=$(date -u '+%a %b %d %H:%M')
	date_agrees "date -u" "$before" "$got" "$after"
	want_eq "date -u year" "${got##* }" "$(date -u +%Y)"
	want_eq "date -u zone" "$(echo "$got" | awk '{print $(NF-1)}')" "+0000"

	# The local zone, which is tz.rs reading the file date(1) reads.
	before=$(date '+%a %b %d %H:%M')
	loc=$("$1" 2>$ERR)
	st=$?
	after=$(date '+%a %b %d %H:%M')
	want_eq "date status" "$st" "0"
	date_agrees "date local" "$before" "$loc" "$after"
	want_eq "date local zone" "$(echo "$loc" | awk '{print $(NF-1)}')" "$(date +%z)"
}

# Ours to the minute, against the host's read either side of it, so a second
# boundary cannot make it flake.
date_agrees() {
	case "$3" in
	"$2"* | "$4"*) echo "ok: $1 agrees with the host's" ;;
	*)
		echo "MISMATCH $1: got [$3] want [$2]"
		fail=1
		;;
	esac
}

run_example() {
	name=$1
	bin=$2
	fence "example $name"
	if [ ! -x "$bin" ]; then
		echo "NOT BUILT: $bin"
		echo "run: (cd rust && cargo build --examples)"
		fail=1
		return
	fi
	case $name in
	hello) example_hello "$bin" ;;
	read_file) example_read_file "$bin" ;;
	date) example_date "$bin" ;;
	*)
		echo "UNKNOWN EXAMPLE: $name"
		fail=1
		;;
	esac
}

echo "=== koru rust suites ==="
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

if [ -z "$SUITES" ]; then
	echo "NO SUITES"
	echo
	echo "=== KORU-RUST-FAIL ==="
	exit 1
fi
for suite in $SUITES; do
	name=${suite%%|*}
	rest=${suite#*|}
	bin=${rest%%|*}
	floor=${rest##*|}
	run_suite "$name" "$bin" "$floor"
done

# The entry's own test: #[koru::main] replaces main, so libtest cannot call
# one. A filter is a test-name filter, so the examples are skipped with one.
if [ -z "$FILTERS" ]; then
	if [ -z "$EXAMPLES" ]; then
		echo
		echo "NO EXAMPLES"
		fail=1
	fi
	for ex in $EXAMPLES; do
		run_example "${ex%%|*}" "${ex#*|}"
	done
else
	echo
	echo "=== examples skipped: filtered run ==="
fi

fence "rmmod"
rmmod koru || { echo "RMMOD FAILED"; fail=1; }
[ -e /dev/koru ] && { echo "/dev/koru STILL PRESENT AFTER RMMOD"; fail=1; } \
	|| echo "device node gone"

# libtest orders tests by name, so these suites cannot promise the heavy-first
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
