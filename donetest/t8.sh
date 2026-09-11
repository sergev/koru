#!/bin/sh
# T8 done test, run inside the virtme-ng guest.
# Done test: two concurrent ops on one slot; the slot frees when the CQE posts.

. "$(dirname "$0")/common.sh"

dt_begin "T8" "slot exclusivity"
dt_insmod
dt_run "test/t8_slots"
dt_finish
