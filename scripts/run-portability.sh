#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Host-side runner for T49's portability suite. Boots the dev kernel in a VM
# and runs scripts/portability.sh inside it. Never load the module on the host.
#
#   scripts/run-portability.sh              every case
#   scripts/run-portability.sh cut uniq     only cases whose name matches
#   KDIR=/path/to/tree scripts/run-portability.sh
#
# It builds its own sanitized directory, as run-cpp.sh does and for the same
# reason: the done test is that Braam's programs are correct *and* clean under
# ASan and UBSan, and `build/` is the plain one the ABI diff uses.
#
# The module is not built here:
#   make -C ../kernel-dev/linux-source-7.1 M=$PWD/kernel LLVM=1

ROOT=$(cd "$(dirname "$0")/.." && pwd)
KDIR=${KDIR:-$ROOT/../kernel-dev/linux-source-7.1}
BUILD=${BUILD:-$ROOT/build-port}
CXX=${CXX:-g++}
CC=${CC:-gcc}
MEMORY=${MEMORY:-2G}
CPUS=${CPUS:-4}
TIMEOUT=${TIMEOUT:-900}

# The number of cases an unfiltered run must reach. A filter matching nothing
# runs none and exits 0, so this is what makes the verdict mean something.
FLOOR=${FLOOR:-118}

if [ ! -d "$KDIR" ]; then
	echo "no kernel tree at $KDIR; set KDIR" >&2
	exit 1
fi
if [ ! -f "$ROOT/kernel/koru.ko" ]; then
	echo "kernel/koru.ko is not built" >&2
	exit 1
fi

cmake -B "$BUILD" -S "$ROOT" -DCMAKE_CXX_COMPILER="$CXX" -DCMAKE_C_COMPILER="$CC" \
	-DKORU_SANITIZE=ON >/dev/null || exit 1
cmake --build "$BUILD" --target koru_cmd -j"$(nproc)" >/dev/null || {
	cmake --build "$BUILD" --target koru_cmd
	exit 1
}

# Ask the binary, not the cache: a stale directory configured without the
# sanitizers would quietly take the second half of the done test away.
if ! nm -C "$BUILD/cmd_cat" 2>/dev/null | grep -q '__asan_'; then
	echo "$BUILD/cmd_cat is not sanitized; remove $BUILD and re-run" >&2
	exit 1
fi

# The Rust twins, located through cargo's JSON rather than by globbing
# target/debug, where a stale binary would outlive a failed build and pass.
find_rust_example() {
	bin=$(cd "$ROOT/rust" && cargo build -p koru --example "$1" \
		--message-format=json 2>/dev/null |
		grep -F '"kind":["example"]' |
		tr ',' '\n' |
		sed -n 's/.*"executable":"\([^"]*\)".*/\1/p' | tail -1)
	if [ -z "$bin" ] || [ ! -x "$bin" ]; then
		echo "no Rust $1 example to compare against" >&2
		echo "run: (cd rust && cargo build --examples)" >&2
		exit 1
	fi
	echo "$bin"
}

RS_ECHO=$(find_rust_example echo) || exit 1
RS_BASENAME=$(find_rust_example basename) || exit 1
RS_CAT=$(find_rust_example cat) || exit 1
RS_GREP=$(find_rust_example grep) || exit 1
RS_UNIQ=$(find_rust_example uniq) || exit 1

cd "$ROOT" || exit 1

out=$(timeout "$TIMEOUT" vng --run "$KDIR" --user root --memory "$MEMORY" --cpus "$CPUS" \
	--exec "BIN='$BUILD' FLOOR='$FLOOR' \
		RS_ECHO='$RS_ECHO' RS_BASENAME='$RS_BASENAME' RS_CAT='$RS_CAT' \
		RS_GREP='$RS_GREP' RS_UNIQ='$RS_UNIQ' \
		sh scripts/portability.sh $*" 2>&1)

verdict=$(echo "$out" | grep -oE 'KORU-PORT-(PASS|FAIL)' | tail -1)
case "$verdict" in
KORU-PORT-PASS)
	echo "$out" | grep -E '^(=== |cases run: )'
	echo "KORU-PORT-PASS"
	exit 0
	;;
*)
	echo "${verdict:-NO VERDICT (guest died or hung?)}"
	echo "--- last section reached ---"
	echo "$out" | grep -E '^=== ' | tail -3
	echo "--- failures ---"
	echo "$out" | grep -A4 -E '^FAIL |TOO FEW|NOT BUILT|KERNEL SPLAT' | head -60
	echo "--- tail ---"
	echo "$out" | tail -20
	exit 1
	;;
esac
