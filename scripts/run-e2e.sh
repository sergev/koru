#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Host-side runner for the screen's end-to-end checks. Boots the dev kernel in
# a VM and runs scripts/e2e.sh inside it. Never load the module on the host.
#
#   scripts/run-e2e.sh
#   KDIR=/path/to/tree scripts/run-e2e.sh
#
# It does not build. Do that first:
#   make -C ../kernel-dev/linux-source-7.1 M=$PWD/kernel LLVM=1
#   cmake -B build && cmake --build build
#   (cd rust && cargo build --examples)
#
# Exits non-zero unless everything passed, so it is usable as a gate.

ROOT=$(cd "$(dirname "$0")/.." && pwd)
KDIR=${KDIR:-$ROOT/../kernel-dev/linux-source-7.1}
MEMORY=${MEMORY:-2G}
CPUS=${CPUS:-4}
TIMEOUT=${TIMEOUT:-600}

if [ ! -d "$KDIR" ]; then
	echo "no kernel tree at $KDIR; set KDIR" >&2
	exit 1
fi
if [ ! -f "$ROOT/kernel/koru.ko" ]; then
	echo "kernel/koru.ko is not built" >&2
	exit 1
fi
if [ ! -x "$ROOT/build/koru-screen" ]; then
	echo "the daemon is not built: cmake -B build && cmake --build build" >&2
	exit 1
fi
for prog in cmd_less cmd_edit; do
	if [ ! -x "$ROOT/build/$prog" ]; then
		echo "$prog is not built: cmake -B build && cmake --build build" >&2
		exit 1
	fi
done

# Ask cargo where the examples are, rather than globbing target/debug, where a
# stale binary would outlive a failed build and pass.
find_example() {
	bin=$(cd "$ROOT/rust" && cargo build -p koru --example "$1" --message-format=json 2>/dev/null |
		grep -F '"kind":["example"]' |
		tr ',' '\n' |
		sed -n 's/.*"executable":"\([^"]*\)".*/\1/p' | tail -1)
	if [ -z "$bin" ] || [ ! -x "$bin" ]; then
		echo "no example binary for $1" >&2
		echo "run: (cd rust && cargo build --examples)" >&2
		exit 1
	fi
	echo "$bin"
}

PROBE=$(find_example screen_probe) || exit 1
LESS=$(find_example less) || exit 1
HELLO=$(find_example hello) || exit 1
DATE=$(find_example date) || exit 1

cd "$ROOT" || exit 1

out=$(timeout "$TIMEOUT" vng --run "$KDIR" --user root --memory "$MEMORY" --cpus "$CPUS" \
	--exec "PROBE='$PROBE' LESS='$LESS' HELLO='$HELLO' DATE='$DATE' \
		DAEMON='$ROOT/build/koru-screen' CPPLESS='$ROOT/build/cmd_less' \
		CPPEDIT='$ROOT/build/cmd_edit' \
		PIXEL='$ROOT/build/ks_pixel' sh scripts/e2e.sh" 2>&1)

verdict=$(echo "$out" | grep -oE 'KORU-E2E-(PASS|FAIL)' | tail -1)
case "$verdict" in
KORU-E2E-PASS)
	echo "$out" | grep -E '^(=== |ok: )'
	echo "KORU-E2E-PASS"
	exit 0
	;;
*)
	echo "${verdict:-NO VERDICT (guest died or hung?)}"
	echo "--- what happened ---"
	echo "$out" | grep -E '^(=== |ok: |FAILED|MISMATCH|NOT BUILT|INSMOD)' | tail -40
	echo "--- tail ---"
	echo "$out" | tail -20
	exit 1
	;;
esac
