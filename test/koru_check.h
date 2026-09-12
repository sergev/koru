/* SPDX-License-Identifier: MIT */
/* Internals shared between koru_check's translation units. */

#ifndef KORU_CHECK_H
#define KORU_CHECK_H

#include "koru_test.h"

/* The shared ring. See doc/Notes.md for why it is shared and why 64 KB. */
#define SHARED_SQ      64u
#define SHARED_CQ      128u
#define SHARED_SLOT    65536u
#define SHARED_SLOTS   24u
#define SHARED_HANDLES 32u

extern struct koru_ring R;

/* Written at startup, one whole slot long. */
#define PATFILE "/tmp/koru-check-pattern"
#define PATSIZE SHARED_SLOT

#define HOSTNAME "/etc/hostname"

/* The WRITE section's own target. Created and unlinked by that section. */
#define WRFILE "/tmp/koru-check-write"

/* koru_check.c */
void sec_smoke(void);
void sec_setup(void);
void sec_ioctl(void);
void sec_mmap(void);
void sec_enter(void);
void sec_slots(void);

/* koru_ops.c */
void sec_checksum(void);
void sec_open(void);
void sec_read(void);
void sec_write(void);
void sec_nonblock(void);
void sec_stat(void);
void sec_path(void);
void sec_adopt(void);
void sec_poll(void);
void sec_delay(void);
void sec_cancel(void);
void sec_signals(void);
void sec_creds(void);

/* koru_race.c */
void sec_devchurn(void);
void sec_ringchurn(void);
void sec_handles(void);
void sec_races(void);

/* koru_fuzz.c. sec_fuzz forks; fuzz_main is the child. */
void sec_fuzz(void);
int fuzz_main(unsigned secs, uint64_t seed);

#endif /* KORU_CHECK_H */
