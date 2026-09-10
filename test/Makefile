# SPDX-License-Identifier: GPL-2.0
#
# Interim C test harness for the kernel module. Built on the host, run inside
# the virtme-ng guest, where this directory is visible at the same path.
#
# The real test suites arrive with the bindings: Rust at T13, C++ at T17.

CFLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -D_GNU_SOURCE

TESTS := t3_setup t4_enter t5_delay t6_teardown t7_arena t8_slots t9_handles t10_read
COMMON := koru_test.c koru_test.h

all: $(TESTS)

$(TESTS): %: %.c $(COMMON)
	$(CC) $(CFLAGS) -o $@ $< koru_test.c

clean:
	rm -f $(TESTS)

.PHONY: all clean
