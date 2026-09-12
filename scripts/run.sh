#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Host-side runner. Boots the dev kernel in a VM and runs the check inside it.
# Never load the module on the host.
#
#   scripts/run.sh                      the whole check
#   scripts/run.sh open read cancel     just those sections
#   KORU_SEED=12345 scripts/run.sh      replay a fuzz failure
#   KDIR=/path/to/tree scripts/run.sh   a kernel tree somewhere else
#
# It does not build. Do that first:
#   make -C ../kernel-dev/linux-source-7.1 M=$PWD/kernel LLVM=1
#   make -C test
#
# Exits non-zero unless the check passed, so it is usable as a gate.

ROOT=$(cd "$(dirname "$0")/.." && pwd)
KDIR=${KDIR:-$ROOT/../kernel-dev/linux-source-7.1}
MEMORY=${MEMORY:-2G}
CPUS=${CPUS:-4}
TIMEOUT=${TIMEOUT:-180}

if [ ! -d "$KDIR" ]; then
	echo "no kernel tree at $KDIR; set KDIR" >&2
	exit 1
fi
if [ ! -x "$ROOT/test/koru_check" ]; then
	echo "test/koru_check is not built; run: make -C test" >&2
	exit 1
fi
if [ ! -f "$ROOT/kernel/koru.ko" ]; then
	echo "kernel/koru.ko is not built" >&2
	exit 1
fi

# The guest inherits this directory, so the relative path below resolves.
cd "$ROOT" || exit 1

# A hung guest would otherwise hang this script for ever.
out=$(timeout "$TIMEOUT" vng --run "$KDIR" --user root --memory "$MEMORY" --cpus "$CPUS" \
	--exec "KORU_SEED=$KORU_SEED sh scripts/check.sh $*" 2>&1)

verdict=$(echo "$out" | grep -oE 'KORU-CHECK-(PASS|FAIL)' | tail -1)
case "$verdict" in
KORU-CHECK-PASS)
	echo "$out" | grep -E '^(FAILED|OK):'
	echo "KORU-CHECK-PASS"
	exit 0
	;;
*)
	# The last banner says where it got to; a crash leaves no verdict at all.
	echo "${verdict:-NO VERDICT (guest died or hung?)}"
	echo "--- last section reached ---"
	echo "$out" | grep -E '^== SECTION |^=== ' | tail -3
	echo "--- failing checks ---"
	echo "$out" | grep -E 'FAIL|UNEXPECTED|NO KASAN|NO /proc|NO kmemleak' | head -30
	echo "--- tail ---"
	echo "$out" | tail -30
	exit 1
	;;
esac
