#!/bin/sh
# T2 done test, run inside the virtme-ng guest.
# Done test: 10,000 open/close iterations; kmemleak reports nothing.
#
# Scans twice, before and after unload. The post-unload scan is the
# authoritative one: anything still unreferenced once the module is gone is a
# genuine leak with nobody left to free it.

. "$(dirname "$0")/common.sh"

DEV=/dev/koru
N=10000

dt_begin "T2" "10k open/close, kmemleak"
dt_insmod

echo
echo "=== device node ==="
# The node ships 0600 root:root.
perm=$(stat -c '%a %U %G' "$DEV" 2>/dev/null)
echo "stat: $perm"
if [ "$perm" = "600 root root" ]; then
	echo "perms OK"
else
	echo "UNEXPECTED PERMS: $perm"
	dt_fail=1
fi

echo
echo "=== $N open/close iterations ==="
fail=0
i=0
while [ $i -lt $N ]; do
	if exec 3<"$DEV"; then
		exec 3<&-
	else
		fail=$((fail + 1))
	fi
	i=$((i + 1))
done
echo "iterations: $i  failures: $fail"
if [ "$fail" -ne 0 ]; then
	echo "OPEN/CLOSE FAILURES: $fail"
	dt_fail=1
fi

echo "(scan with the module still loaded)"
dt_kmemleak

dt_rmmod
[ -e "$DEV" ] && { echo "$DEV STILL PRESENT AFTER RMMOD"; dt_fail=1; } \
	|| echo "device node gone"

echo "(scan after unload: the authoritative one)"
dt_kmemleak

dt_taint

echo
echo "=== init/exit messages ==="
dmesg | grep 'koru:'

dt_splat
dt_verdict COMPLETE
