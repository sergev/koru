#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Host-side runner. Boots the dev kernel in a VM and runs done tests inside it,
# printing one verdict line each. Never load the module on the host.
#
#   donetest/run.sh              every test, in order
#   donetest/run.sh 9 10 11      just those
#   DT_ARGS="30 999" donetest/run.sh 12     pass arguments to a single test
#   KDIR=/path/to/tree donetest/run.sh      a kernel tree somewhere else
#
# Exits non-zero if any test did not pass, so it is usable as a gate.

ROOT=$(cd "$(dirname "$0")/.." && pwd)
KDIR=${KDIR:-$ROOT/../kernel-dev/linux-source-7.1}
MEMORY=${MEMORY:-4G}
CPUS=${CPUS:-4}

if [ ! -d "$KDIR" ]; then
	echo "no kernel tree at $KDIR; set KDIR" >&2
	exit 1
fi

# The guest inherits this directory, so the relative paths below resolve.
cd "$ROOT" || exit 1

[ $# -gt 0 ] || set -- 0 1 2 3 4 5 6 7 8 9 10 11 12

status=0
for n in "$@"; do
	if [ ! -f "donetest/t$n.sh" ]; then
		echo "T$n: no such done test" >&2
		status=1
		continue
	fi
	out=$(vng --run "$KDIR" --user root --memory "$MEMORY" --cpus "$CPUS" \
		--exec "sh donetest/t$n.sh $DT_ARGS" 2>&1)
	verdict=$(echo "$out" | grep -oE 'T[0-9]+-DONE-TEST-[A-Z]+' | tail -1)
	case "$verdict" in
	*-PASS | *-COMPLETE)
		printf 'T%-3s %s\n' "$n" "$verdict"
		;;
	*)
		# No verdict at all means the guest died or the test hung.
		printf 'T%-3s %s\n' "$n" "${verdict:-NO VERDICT (guest died or hung?)}"
		echo "$out" | tail -30
		status=1
		;;
	esac
done
exit $status
