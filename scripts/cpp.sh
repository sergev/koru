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
# CHECKBIN is the suite binary and FLOOR the number of cases it must run;
# DEMO and RUSTDEMO are T15's demo in each language, which must print the same
# bytes. All four are passed in by the runner.

ROOT=$(cd "$(dirname "$0")/.." && pwd)
KO=${KO:-$ROOT/kernel/koru.ko}
CHECKBIN=${CHECKBIN:-$ROOT/build/koru_cpp_check}
FLOOR=${FLOOR:-72}
DEMO=${DEMO:-}
RUSTDEMO=${RUSTDEMO:-}
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
