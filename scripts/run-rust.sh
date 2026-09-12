#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Host-side runner for the Rust suite. Boots the dev kernel in a VM and runs
# scripts/rust.sh inside it. Never load the module on the host.
#
#   scripts/run-rust.sh                 the whole suite
#   scripts/run-rust.sh cancel read     only tests whose name matches
#   KDIR=/path/to/tree scripts/run-rust.sh
#
# It does not build. Do that first:
#   make -C ../kernel-dev/linux-source-7.1 M=$PWD/kernel LLVM=1
#   (cd rust && cargo test -p koru-sys --no-run)
#
# Exits non-zero unless the suite passed, so it is usable as a gate.

ROOT=$(cd "$(dirname "$0")/.." && pwd)
KDIR=${KDIR:-$ROOT/../kernel-dev/linux-source-7.1}
MEMORY=${MEMORY:-2G}
CPUS=${CPUS:-4}
TIMEOUT=${TIMEOUT:-300}

if [ ! -d "$KDIR" ]; then
	echo "no kernel tree at $KDIR; set KDIR" >&2
	exit 1
fi
if [ ! -f "$ROOT/kernel/koru.ko" ]; then
	echo "kernel/koru.ko is not built" >&2
	exit 1
fi

# Ask cargo where the binary is rather than globbing target/debug/deps, where
# stale hashes accumulate and a newest-match would survive a failed build.
# The abi_dump bin is emitted by this same command, in an order that varies,
# so select the test target by kind: picking the bin runs nothing and exits 0.
# Not "test":true, which a bin target also carries.
BIN=$(cd "$ROOT/rust" && cargo test -p koru-sys --test kernel --no-run \
	--message-format=json 2>/dev/null \
	| grep -F '"kind":["test"]' \
	| tr ',' '\n' \
	| sed -n 's/.*"executable":"\([^"]*\)".*/\1/p' \
	| tail -1)
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
	echo "no test binary; run: (cd rust && cargo test -p koru-sys --no-run)" >&2
	exit 1
fi

cd "$ROOT" || exit 1

out=$(timeout "$TIMEOUT" vng --run "$KDIR" --user root --memory "$MEMORY" --cpus "$CPUS" \
	--exec "BIN=$BIN WANT_PASSED=$WANT_PASSED sh scripts/rust.sh $*" 2>&1)

verdict=$(echo "$out" | grep -oE 'KORU-RUST-(PASS|FAIL)' | tail -1)
case "$verdict" in
KORU-RUST-PASS)
	echo "$out" | grep -E '^test result:'
	echo "KORU-RUST-PASS"
	exit 0
	;;
*)
	echo "${verdict:-NO VERDICT (guest died or hung?)}"
	echo "--- last section reached ---"
	echo "$out" | grep -E '^=== ' | tail -3
	echo "--- failures ---"
	echo "$out" | grep -E -e "^(test .* FAILED|failures:|thread '|assertion)" \
		-e 'NO KASAN|NO /proc|NO kmemleak|TOO FEW|SKIPPED|UNEXPECTED' | head -30
	echo "--- tail ---"
	echo "$out" | tail -30
	exit 1
	;;
esac
