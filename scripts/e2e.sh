#!/bin/sh
# SPDX-License-Identifier: MIT
#
# The guest side of the screen's end-to-end checks: auto-spawn, the lifecycle,
# and `less` painting a real window. Runs inside the virtme-ng guest, never on
# the host, and prints exactly one KORU-E2E-PASS or KORU-E2E-FAIL marker.
#
# Everything here needs three things the other gates do not: the module (the
# clients are koru programs), SDL's offscreen driver (the daemon owns a window)
# and a writable runtime directory (the socket).
#
# PROBE, LESS and DAEMON are absolute paths, passed in by scripts/run-e2e.sh.

ROOT=$(cd "$(dirname "$0")/.." && pwd)
KO=${KO:-$ROOT/kernel/koru.ko}
PROBE=${PROBE:-}
LESS=${LESS:-}
DAEMON=${DAEMON:-$ROOT/build/koru-screen}
PIXEL=${PIXEL:-$ROOT/build/ks_pixel}
RUN=/tmp/koru-e2e
SOCK=$RUN/koru-screen.sock

fail=0
fence() { echo; echo "=== $1 ==="; }

want_eq() {
	if [ "$2" = "$3" ]; then
		echo "ok: $1"
	else
		echo "MISMATCH $1: got [$2] want [$3]"
		fail=1
	fi
}

want() {
	if [ "$2" -eq 0 ]; then
		echo "ok: $1"
	else
		echo "FAILED: $1"
		fail=1
	fi
}

# The daemon by its exact command line, never by a name: this script's own
# environment carries the daemon's path, so `pgrep -f koru-screen` counts the
# shell running the script as well and every count is one too many.
daemons() {
	pgrep -c -x -f "$DAEMON" 2>/dev/null || echo 0
}

daemon_pid() {
	pgrep -x -f "$DAEMON" 2>/dev/null | head -1
}

reset_run() {
	pkill -x -f "$DAEMON" 2>/dev/null
	sleep 0.2
	rm -rf $RUN
	mkdir -p $RUN
	chmod 700 $RUN
}

export SDL_VIDEODRIVER=offscreen
export KORU_SCREEN_SOCK=$SOCK
export KORU_SCREEN_BIN=$DAEMON
export XDG_RUNTIME_DIR=$RUN

echo "=== koru screen end to end ==="
uname -r

fence "environment"
for f in "$PROBE" "$LESS" "$DAEMON"; do
	if [ ! -x "$f" ]; then
		echo "NOT BUILT: $f"
		fail=1
	fi
done
[ "$fail" -eq 0 ] || { echo; echo "=== KORU-E2E-FAIL ==="; exit 1; }

insmod "$KO" || { echo "INSMOD FAILED"; echo; echo "=== KORU-E2E-FAIL ==="; exit 1; }

# --------------------------------------------------------------- the spawn race
#
# Twenty clients at once against no daemon. Exactly one daemon must exist
# afterwards and all twenty must connect; without the flock, more than one
# appears and the losers' sockets are unlinked under the winner.
fence "spawn race"
reset_run
# One file per client, never one shared: twenty processes appending to one
# file braid their lines together, and the count comes out short for a reason
# that has nothing to do with the spawn. It is the single-writer rule again,
# in the harness this time.
i=0
while [ $i -lt 20 ]; do
	"$PROBE" connect >/dev/null 2>$RUN/race.$i.err &
	i=$((i + 1))
done
wait
connected=$(grep -l '^connected ' $RUN/race.*.err 2>/dev/null | wc -l)
want_eq "twenty clients connected" "$connected" "20"
want_eq "exactly one daemon" "$(daemons)" "1"
[ -S "$SOCK" ] && echo "ok: the socket is where the clients found it" || {
	echo "FAILED: no socket"
	fail=1
}

# --------------------------------------------------- a live daemon is not stale
#
# The same twenty against a daemon that is already running: its pid must not
# change, which is the rule "only the lock holder may unlink" protecting a
# socket that is alive.
fence "a live daemon survives twenty clients"
was=$(daemon_pid)
i=0
while [ $i -lt 20 ]; do
	"$PROBE" connect >/dev/null 2>$RUN/live.$i.err &
	i=$((i + 1))
done
wait
want_eq "twenty more connected" "$(grep -l '^connected ' $RUN/live.*.err 2>/dev/null | wc -l)" "20"
want_eq "the daemon is the same one" "$(daemon_pid)" "$was"
want_eq "and there is still one" "$(daemons)" "1"

# ------------------------------------------------------------- a stale socket
#
# Kill the daemon without letting it clean up. The socket file is still there
# and nothing is listening: the next client must unlink it and start one.
fence "a stale socket is replaced"
kill -9 "$was" 2>/dev/null
sleep 0.3
[ -S "$SOCK" ] && echo "ok: the socket outlived the daemon" || {
	echo "FAILED: the socket went with it, so this case tests nothing"
	fail=1
}
"$PROBE" connect >/dev/null 2>$RUN/stale.err
want "a client replaced the stale socket" $?
want_eq "one daemon again" "$(daemons)" "1"
[ "$(daemon_pid)" != "$was" ] && echo "ok: a new daemon" || {
	echo "FAILED: the old pid is still there"
	fail=1
}

# ------------------------------------------------------------- daemon death
#
# A client holding the screen when the daemon is killed must report a dead
# connection and leave, not hang. Exit status 2 is the probe's word for it.
fence "a client survives the daemon"
"$PROBE" hold 4000 >/dev/null 2>$RUN/dead.err &
client=$!
sleep 0.6
kill -9 "$(daemon_pid)" 2>/dev/null
wait $client
rc=$?
want_eq "the client exited non-zero" "$rc" "2"
grep -q "closed" $RUN/dead.err && echo "ok: it said the connection was closed" || {
	echo "FAILED: what it said:"
	cat $RUN/dead.err
	fail=1
}

# -------------------------------------------------------------- client death
#
# Kill a client mid-blit. The daemon must take its claims back, serve the next
# client, and log no protocol error.
fence "the daemon survives a client"
reset_run
"$PROBE" hold 5000 >/dev/null 2>$RUN/held.err &
victim=$!
sleep 0.8
want_eq "the victim is holding the screen" "$(grep -c '^connected ' $RUN/held.err)" "1"
kill -9 $victim 2>/dev/null
wait $victim 2>/dev/null
sleep 0.3
"$PROBE" connect >/dev/null 2>$RUN/next.err
want "the next client was served" $?
want_eq "the daemon is still there" "$(daemons)" "1"
want_eq "and it logged nothing" "$(cat $RUN/daemon.log 2>/dev/null | wc -l)" "0"

# ----------------------------------------------------------------- less
#
# The deliverable: Braam's pager, painting a real window through the daemon.
# The keys are pushed in by the daemon's own event path, so this asserts that
# `less` ran and repainted, and the pixels are T35's business.
fence "less"
reset_run
seq 1 200 | sed 's/^/line /' >$RUN/fixture.txt
KORU_SCREEN_SNAP=$RUN/less.bmp "$LESS" $RUN/fixture.txt >/dev/null 2>$RUN/less.err &
pager=$!
sleep 1.5
if kill -0 $pager 2>/dev/null; then
	echo "ok: less is up and painting"
else
	echo "FAILED: less exited early:"
	cat $RUN/less.err
	fail=1
fi
want_eq "one daemon for the pager" "$(daemons)" "1"

# What it painted, in pixels. The status line is the assertion that matters:
# `less` paints it black on cyan, which nothing else in this run does, and a
# pager that ran but drew nothing would leave it the background colour.
if [ -f "$RUN/less.bmp" ] && [ -x "$PIXEL" ]; then
	size=$("$PIXEL" $RUN/less.bmp size)
	echo "   window $size"
	rows=${size#*x}
	cols=${size%x*}
	# The cell size is the font's, times the daemon's scale of 2: 16x20.
	last=$((rows - 20))
	want_eq "the status line is cyan" "$("$PIXEL" $RUN/less.bmp modal 0 $last "$cols" 20)" "00cdcd"
	body_ink=$("$PIXEL" $RUN/less.bmp ink 0 0 "$cols" 20)
	[ "$body_ink" -gt 0 ] && echo "ok: the first body row has text on it" || {
		echo "FAILED: the body row is blank"
		fail=1
	}
else
	echo "FAILED: no snapshot from the daemon"
	fail=1
fi
kill -9 $pager 2>/dev/null
wait $pager 2>/dev/null

# `less` with no daemon to be had is `cat`, which is the other half of its
# first decision and needs no window at all.
fence "less with no screen"
pkill -x -f "$DAEMON" 2>/dev/null
sleep 0.2
rm -rf $RUN/koru-screen.sock
out=$(KORU_SCREEN_BIN=/nonexistent "$LESS" $RUN/fixture.txt 2>$RUN/cat.err | tail -1)
want_eq "it catted the file instead" "$out" "line 200"

pkill -x -f "$DAEMON" 2>/dev/null

fence "verdict"
if [ "$fail" -eq 0 ]; then
	echo "=== KORU-E2E-PASS ==="
else
	echo "=== KORU-E2E-FAIL ==="
fi
