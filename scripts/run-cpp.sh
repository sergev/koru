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
# It builds its own directory, because the suite wants ASan and UBSan and
# `build/` is the plain one the ABI diff uses: T40's claim is that a destroyed
# frame is never resumed, and without a sanitizer that claim has no instrument.
# The module and the Rust demo are not built here:
#   make -C ../kernel-dev/linux-source-7.1 M=$PWD/kernel LLVM=1
#   (cd rust && cargo build --examples)
#
# Exits non-zero unless everything passed, so it is usable as a gate.

ROOT=$(cd "$(dirname "$0")/.." && pwd)
KDIR=${KDIR:-$ROOT/../kernel-dev/linux-source-7.1}
BUILD=${BUILD:-$ROOT/build-cpp}
CXX=${CXX:-g++}
CC=${CC:-gcc}
MEMORY=${MEMORY:-2G}
CPUS=${CPUS:-4}
TIMEOUT=${TIMEOUT:-900}

# The number of cases an unfiltered run must reach. It is the whole of the
# T4-T11 matrix plus the T3 matrices, and it equals the Rust suite's count for
# the same sections: raise it when a case is added.
FLOOR=${FLOOR:-71}

if [ ! -d "$KDIR" ]; then
	echo "no kernel tree at $KDIR; set KDIR" >&2
	exit 1
fi
if [ ! -f "$ROOT/kernel/koru.ko" ]; then
	echo "kernel/koru.ko is not built" >&2
	exit 1
fi
# Per-target sanitizer flags, never CMAKE_CXX_FLAGS, for the reason the top of
# CMakeLists.txt records.
cmake -B "$BUILD" -S "$ROOT" -DCMAKE_CXX_COMPILER="$CXX" -DCMAKE_C_COMPILER="$CC" \
	-DKORU_SANITIZE=ON >/dev/null || exit 1
cmake --build "$BUILD" --target koru_cpp_check koru_cpp_unit cpp_read_file >/dev/null || {
	cmake --build "$BUILD" --target koru_cpp_check koru_cpp_unit cpp_read_file
	exit 1
}

# The sanitizers are the instrument T40's claim rests on, and a stale build
# directory whose cache says OFF would quietly take them away. Ask the binary,
# not the cache.
if ! nm -C "$BUILD/koru_cpp_check" 2>/dev/null | grep -q '__asan_'; then
	echo "$BUILD/koru_cpp_check is not sanitized; remove $BUILD and re-run" >&2
	exit 1
fi

# The device-free half first: it needs no VM, and a failure there is a failure
# of the same library.
"$BUILD/koru_cpp_unit" >/dev/null 2>&1 || {
	echo "the host-side unit cases failed:"
	"$BUILD/koru_cpp_unit"
	exit 1
}

# T42's demo has to be compared against the Rust one, so ask cargo where that
# is rather than globbing target/debug, where a stale binary would outlive a
# failed build and pass.
RUSTDEMO=$(cd "$ROOT/rust" && cargo build -p koru --example read_file \
	--message-format=json 2>/dev/null |
	grep -F '"kind":["example"]' |
	tr ',' '\n' |
	sed -n 's/.*"executable":"\([^"]*\)".*/\1/p' | tail -1)
if [ -z "$RUSTDEMO" ] || [ ! -x "$RUSTDEMO" ]; then
	echo "no Rust read_file example to compare against" >&2
	echo "run: (cd rust && cargo build --examples)" >&2
	exit 1
fi

cd "$ROOT" || exit 1

out=$(timeout "$TIMEOUT" vng --run "$KDIR" --user root --memory "$MEMORY" --cpus "$CPUS" \
	--exec "CHECKBIN='$BUILD/koru_cpp_check' FLOOR='$FLOOR' \
		DEMO='$BUILD/cpp_read_file' RUSTDEMO='$RUSTDEMO' sh scripts/cpp.sh $*" 2>&1)

verdict=$(echo "$out" | grep -oE 'KORU-CPP-(PASS|FAIL)' | tail -1)
case "$verdict" in
KORU-CPP-PASS)
	echo "$out" | grep -E '^(=== |cases: |cases run: |OK: |ok: )'
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
