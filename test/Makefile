# SPDX-License-Identifier: GPL-2.0
#
# Interim C test harness for the kernel module. Built on the host, run inside
# the virtme-ng guest, where this directory is visible at the same path.
#
# The real test suites arrive with the bindings: Rust at T13, C++ at T17.

CFLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -D_GNU_SOURCE

TESTS := t3_setup t4_enter

all: $(TESTS)

%: %.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TESTS)

.PHONY: all clean
