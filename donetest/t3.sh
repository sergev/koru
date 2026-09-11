#!/bin/sh
# T3 done test, run inside the virtme-ng guest.
# Done test: params round-trip; version mismatch rejected; second SETUP is
# EBUSY.

. "$(dirname "$0")/common.sh"

dt_begin "T3" "SETUP / GET_PARAMS"
dt_insmod
dt_run "test/t3_setup"
dt_finish
