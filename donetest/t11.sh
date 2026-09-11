#!/bin/sh
# T11 done test, run inside the virtme-ng guest.
# Done test: a cancelled DELAY_NS returns at once with the target ECANCELED.

. "$(dirname "$0")/common.sh"

dt_begin "T11" "CANCEL"
dt_insmod
dt_run "test/t11_cancel"
dt_finish
