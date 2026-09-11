#!/bin/sh
# T7 done test, run inside the virtme-ng guest.
# Done test: the mmap'd arena, CHECKSUM, and the MAP_SHARED/fork/flags matrix.

. "$(dirname "$0")/common.sh"

dt_begin "T7" "arena mmap, CHECKSUM"
dt_insmod
dt_run "test/t7_arena"
dt_finish
