#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Host-side runner for the C++ binding's device suite. Boots the dev kernel in
# a VM and runs scripts/cpp.sh inside it. Never load the module on the host.
#
#   scripts/run-cpp.sh                 every case
#   scripts/run-cpp.sh cancel read     only cases whose name matches
#   KDIR=/path/to/tree scripts/run-cpp.sh
#
# It does not build. Do that first:
#   make -C ../kernel-dev/linux-source-7.1 M=$PWD/kernel LLVM=1
#   cmake -B build && cmake --build build
#
# Exits non-zero unless everything passed, so it is usable as a gate.

ROOT=$(cd "$(dirname "$0")/.." && pwd)
KDIR=${KDIR:-$ROOT/../kernel-dev/linux-source-7.1}
BUILD=${BUILD:-$ROOT/build}
MEMORY=${MEMORY:-2G}
CPUS=${CPUS:-4}
TIMEOUT=${TIMEOUT:-900}

# The number of cases an unfiltered run must reach. It is the whole of the
# T4-T11 matrix plus the T3 matrices, and it equals the Rust suite's count for
# the same sections: raise it when a case is added.
FLOOR=${FLOOR:-62}

if [ ! -d "$KDIR" ]; then
	echo "no kernel tree at $KDIR; set KDIR" >&2
	exit 1
fi
if [ ! -f "$ROOT/kernel/koru.ko" ]; then
	echo "kernel/koru.ko is not built" >&2
	exit 1
fi
if [ ! -x "$BUILD/koru_cpp_check" ]; then
	echo "$BUILD/koru_cpp_check is not built: cmake -B build && cmake --build build" >&2
	exit 1
fi

cd "$ROOT" || exit 1

out=$(timeout "$TIMEOUT" vng --run "$KDIR" --user root --memory "$MEMORY" --cpus "$CPUS" \
	--exec "CHECKBIN='$BUILD/koru_cpp_check' FLOOR='$FLOOR' sh scripts/cpp.sh $*" 2>&1)

verdict=$(echo "$out" | grep -oE 'KORU-CPP-(PASS|FAIL)' | tail -1)
case "$verdict" in
KORU-CPP-PASS)
	echo "$out" | grep -E '^(=== |cases: |cases run: |OK: )'
	echo "KORU-CPP-PASS"
	exit 0
	;;
*)
	echo "${verdict:-NO VERDICT (guest died or hung?)}"
	echo "--- last section reached ---"
	echo "$out" | grep -E '^=== ' | tail -3
	echo "--- failures ---"
	echo "$out" | grep -E 'FAIL |TOO FEW|SKIPPED|UNEXPECTED|NOT BUILT|NO VERDICT|TIMEOUT' | head -30
	echo "--- tail ---"
	echo "$out" | tail -30
	exit 1
	;;
esac
