#!/bin/sh
# T6 done test, run inside the virtme-ng guest.
# Done test: close(fd) with work in flight, kill -9 a blocked task, and rmmod
# refused while an fd is open rather than silently freeing module text.

. "$(dirname "$0")/common.sh"

TEST=$DT_ROOT/test/t6_teardown

dt_begin "T6" "teardown, module pinning"
dt_insmod
dt_run "test/t6_teardown"

echo
echo "=== rmmod while an fd is open ==="
"$TEST" hold > /tmp/hold.out 2>&1 &
hold_pid=$!
# Wait for READY rather than guessing an interval.
n=0
while [ $n -lt 100 ] && ! grep -q READY /tmp/hold.out 2>/dev/null; do
	n=$((n + 1))
done
if rmmod koru 2>/dev/null; then
	echo "rmmod with an open fd: SUCCEEDED (should be refused)"
	dt_fail=1
	insmod "$KO"
else
	echo "rmmod with an open fd: refused, correct"
fi
wait $hold_pid

echo
echo "=== close(fd) releases queued work ==="
# T12 made release() cancel whatever is still queued, so closing the fd frees
# the ring instead of leaving it pinned for the length of the longest delay.
# This assertion used to be the opposite: rmmod had to be refused here. The
# timing matters as much as the outcome, since waiting the delays out would also
# end with rmmod succeeding.
"$TEST" pending > /tmp/pending.out 2>&1
i=0
while [ $i -lt 40 ]; do
	rmmod koru 2>/dev/null && break
	sleep 1
	i=$((i + 1))
done
if lsmod | grep -q '^koru'; then
	echo "still loaded after ${i}s: close(fd) did not release the queued work"
	dt_fail=1
elif [ $i -gt 2 ]; then
	echo "rmmod took ${i}s: the delays were waited out, not cancelled"
	dt_fail=1
else
	echo "rmmod succeeded after ${i}s: queued work was cancelled at close"
fi

# The module is already unloaded, so the standard tail's rmmod would fail.
dt_kmemleak
dt_taint
dt_splat
dt_verdict
