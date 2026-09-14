#!/bin/sh
# SPDX-License-Identifier: MIT
#
# The guest side of the C++ binding's device suite. Runs inside the virtme-ng
# guest, never on the host. Mirrors rust.sh; check.sh is left alone, because it
# carries every pass condition for the kernel itself.
#
# Prints exactly one KORU-CPP-PASS or KORU-CPP-FAIL marker, which
# scripts/run-cpp.sh greps for. Every check below gates that marker.
#
# CHECKBIN is the suite binary and FLOOR the number of cases it must run. The
# rest are one program per binding, which must print the same bytes: T15's demo
# (T42), Braam's hello world (T44) and Braam's `date` (T47). All are passed in
# by the runner.

ROOT=$(cd "$(dirname "$0")/.." && pwd)
KO=${KO:-$ROOT/kernel/koru.ko}
CHECKBIN=${CHECKBIN:-$ROOT/build/koru_cpp_check}
FLOOR=${FLOOR:-72}
DEMO=${DEMO:-}
RUSTDEMO=${RUSTDEMO:-}
HELLO=${HELLO:-}
RUSTHELLO=${RUSTHELLO:-}
DATE=${DATE:-}
RUSTDATE=${RUSTDATE:-}
FILTERS="$*"
OUT=/tmp/koru-cpp-out
ERR=/tmp/koru-cpp-err
ROUT=/tmp/koru-rs-out
RERR=/tmp/koru-rs-err
DATA=/tmp/koru-cpp-data

fail=0

want_eq() {
	if [ "$2" = "$3" ]; then
		echo "ok: $1"
	else
		echo "MISMATCH $1: got [$2] want [$3]"
		fail=1
	fi
}

same_file() {
	if cmp -s "$2" "$3"; then
		echo "ok: $1"
	else
		echo "MISMATCH $1:"
		cmp "$2" "$3" | head -3
		fail=1
	fi
}

fence() { echo "koru-check: $1" > /dev/kmsg 2>/dev/null; echo; echo "=== $1 ==="; }

echo "=== koru c++ suite ==="
uname -r
mount -t debugfs none /sys/kernel/debug 2>/dev/null

fence "environment"
if [ ! -x "$CHECKBIN" ]; then
	echo "NOT BUILT: $CHECKBIN"
	echo "run: scripts/run-cpp.sh, which builds it"
	echo
	echo "=== KORU-CPP-FAIL ==="
	exit 1
fi
dmesg | grep -qi 'KernelAddressSanitizer' || { echo "NO KASAN"; fail=1; }
[ -e /sys/kernel/debug/kmemleak ] || { echo "NO kmemleak"; fail=1; }
[ "$fail" -eq 0 ] && echo "KASAN and kmemleak both live"

fence "insmod"
insmod "$KO" || { echo "INSMOD FAILED"; echo; echo "=== KORU-CPP-FAIL ==="; exit 1; }

fence "suite"
out=$("$CHECKBIN" $FILTERS 2>&1)
rc=$?
echo "$out"
echo "suite exit: $rc"
[ "$rc" -eq 0 ] || fail=1

# The suite's own verdict line, because a binary that ran nothing also exits 0.
echo "$out" | grep -q '^OK: 0 failure(s)$' || { echo "NO VERDICT LINE"; fail=1; }

# A filter matching nothing runs no cases and reports no failures, which is the
# same hole the Rust runner's floor closes. It only means anything unfiltered.
ran=$(echo "$out" | sed -n 's/^cases: \([0-9]*\) run.*/\1/p' | tail -1)
ran=${ran:-0}
if [ -z "$FILTERS" ]; then
	echo "cases run: $ran (want at least $FLOOR)"
	[ "$ran" -ge "$FLOOR" ] || { echo "TOO FEW CASES RAN"; fail=1; }
else
	echo "cases run: $ran (filtered; no floor)"
fi

# A skip is not a pass.
skips=$(echo "$out" | grep 'KORU-CPP-SKIP')
[ -z "$skips" ] || { echo "SKIPPED, which is not a pass:"; echo "$skips"; fail=1; }

# T42's done test. Two runtimes that share no code, on one unchanged ABI,
# printing the same bytes: this is the language-neutrality claim at its
# sharpest, and a filtered run skips it because it is not a case.
if [ -z "$FILTERS" ]; then
	fence "the demo"
	printf 'one\ntwo\nthree\n' >$DATA
	if [ ! -x "$DEMO" ]; then
		echo "NOT BUILT: $DEMO"
		fail=1
	else
		timeout 60 "$DEMO" $DATA >$OUT 2>$ERR
		want_eq "the C++ demo exited 0" "$?" "0"
		same_file "it wrote the file and nothing else" "$OUT" "$DATA"
		want_eq "and the timers completed out of order" "$(cat $ERR)" "10 20 30"
		# Through a pipe as well: koru refuses a blocking non-regular file, so
		# this is the re-open before the adopt, not a second write path.
		want_eq "the same through a pipe" "$(timeout 60 "$DEMO" $DATA 2>/dev/null | cat)" \
			"$(cat $DATA)"
	fi
	if [ ! -x "$RUSTDEMO" ]; then
		echo "NOT BUILT: $RUSTDEMO"
		fail=1
	else
		timeout 60 "$RUSTDEMO" $DATA >$ROUT 2>$RERR
		want_eq "the Rust demo exited 0" "$?" "0"
		same_file "the two demos agree on stdout" "$OUT" "$ROUT"
		same_file "and on stderr" "$ERR" "$RERR"
	fi
fi

# T44's and T47's. The two bindings' programs must be indistinguishable on
# both streams and in their exit status — the same claim T42's demo makes, on
# the surface a program is actually written against.
#
# Everything is redirected: the virtio console a VM is run on is one of the
# descriptors `/proc/self/fd` cannot re-open, so koru adopts the raw one and
# refuses a write to it. Both bindings fail there in exactly the same way,
# which the last case below is what says.
if [ -z "$FILTERS" ]; then
	fence "the surface"
	for pair in "hello:$HELLO:$RUSTHELLO" "date:$DATE:$RUSTDATE"; do
		name=${pair%%:*}
		rest=${pair#*:}
		cpp=${rest%%:*}
		rs=${rest#*:}
		if [ ! -x "$cpp" ] || [ ! -x "$rs" ]; then
			echo "NOT BUILT: $cpp or $rs"
			fail=1
		fi
	done

	"$HELLO" >$OUT 2>$ERR
	want_eq "hello exit status" "$?" "0"
	want_eq "hello to a regular file" "$(cat $OUT)" "Hello, world!"
	want_eq "hello with an argument" "$("$HELLO" koru 2>$ERR)" "Hello, koru!"
	"$RUSTHELLO" >$ROUT 2>$RERR
	same_file "the two hello worlds agree on stdout" "$OUT" "$ROUT"
	same_file "and on stderr" "$ERR" "$RERR"

	# The usage helpers: which stream the block goes to and what the status
	# is, neither of which a library test can see.
	"$DATE" -h >$OUT 2>$ERR
	want_eq "date -h status" "$?" "0"
	want_eq "date -h to stdout" "$(head -1 $OUT)" "Usage:"
	want_eq "date -h says nothing on stderr" "$(cat $ERR)" ""
	"$RUSTDATE" -h >$ROUT 2>$RERR
	same_file "the two usage blocks are the same bytes" "$OUT" "$ROUT"

	"$DATE" -x >$OUT 2>$ERR
	want_eq "date -x status" "$?" "2"
	want_eq "date -x to stderr" "$(head -1 $ERR)" "Usage:"
	want_eq "date -x says nothing on stdout" "$(cat $OUT)" ""
	"$DATE" a b >$OUT 2>$ERR
	want_eq "date with two operands" "$?" "2"

	# To the minute, against the host's read either side of it, so a second
	# boundary cannot make it flake.
	before=$(date -u '+%a %b %d %H:%M')
	got=$("$DATE" -u 2>$ERR)
	after=$(date -u '+%a %b %d %H:%M')
	case "$got" in
	"$before"* | "$after"*) echo "ok: date -u agrees with the host's" ;;
	*) echo "MISMATCH date -u: got [$got] want [$before]"; fail=1 ;;
	esac
	want_eq "date -u zone" "$(echo "$got" | awk '{print $(NF-1)}')" "+0000"
	want_eq "date -u year" "${got##* }" "$(date -u +%Y)"

	# A program whose stdout cannot be re-opened fails the same way in both
	# bindings, message and status alike. That is the console a VM is run on,
	# and it is the sharpest form of the claim: the two agree when they work
	# and when they do not.
	"$HELLO" 2>$ERR >/dev/console
	cst=$?
	"$RUSTHELLO" 2>$RERR >/dev/console
	rst=$?
	want_eq "both bindings refuse an unadoptable stdout" "$cst" "$rst"
fi

fence "rmmod"
rmmod koru || { echo "RMMOD FAILED"; fail=1; }
[ -e /dev/koru ] && { echo "/dev/koru STILL PRESENT AFTER RMMOD"; fail=1; } \
	|| echo "device node gone"

# The cases run in registration order rather than heavy-first, so pay the flat
# five seconds kmemleak's minimum object age needs. Leak coverage of the kernel
# itself belongs to check.sh, which is unchanged.
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
	echo "=== KORU-CPP-PASS ==="
else
	echo "=== KORU-CPP-FAIL ==="
fi
