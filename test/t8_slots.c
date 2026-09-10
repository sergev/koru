// SPDX-License-Identifier: GPL-2.0
//
// T8 done test: kernel-enforced slot exclusivity.

#include "koru_test.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define SLOT_SIZE 4096u
#define SLOT_COUNT 80u /* > 64, so the bitmap spans two words */
#define ARENA ((uint64_t)SLOT_SIZE * SLOT_COUNT)

/* Find a completion by user_data. */
static const struct koru_cqe *find(const struct koru_cqe *cq, unsigned n, uint64_t ud)
{
	unsigned i;

	for (i = 0; i < n; i++)
		if (cq[i].user_data == ud)
			return &cq[i];
	return NULL;
}

int main(void)
{
	struct koru_sqe sq[8];
	struct koru_cqe cq[8];
	const struct koru_cqe *a, *b;
	uint8_t *arena, *pattern;
	unsigned completed;
	int fd, ret;
	unsigned i;

	test_begin(120);

	pattern = malloc(SLOT_SIZE);
	if (!pattern)
		return 1;
	for (i = 0; i < SLOT_SIZE; i++)
		pattern[i] = (uint8_t)(i * 17 + 3);

	fd = open_ring(32, 64, SLOT_SIZE, SLOT_COUNT);
	if (fd < 0)
		return 1;
	arena = mmap(NULL, ARENA, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (arena == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	memcpy(arena + 3 * SLOT_SIZE, pattern, SLOT_SIZE);

	/* 1. Two ops naming one slot: the second is refused. */
	sqe_checksum(&sq[0], 3, 0, SLOT_SIZE, 0xaa);
	sqe_checksum(&sq[1], 3, 0, SLOT_SIZE, 0xbb);
	ret = submit(fd, sq, 2, cq, 2, 2, &completed);
	check(ret == 2, "two ops on one slot: both SQEs consumed");
	check(completed == 2, "  both complete (C1 holds)");
	a = find(cq, completed, 0xaa);
	b = find(cq, completed, 0xbb);
	check(a && b, "  each completion carries its own user_data");
	if (a && b) {
		int one_ok = (a->res >= 0) ^ (b->res >= 0);
		int one_busy = (a->res == -EBUSY) ^ (b->res == -EBUSY);
		check(one_ok && one_busy, "  exactly one succeeds, the other gets -EBUSY");
		if (a->res >= 0)
			check(a->res == fnv1a(pattern, SLOT_SIZE), "  the winner's checksum is right");
		else
			check(b->res == fnv1a(pattern, SLOT_SIZE), "  the winner's checksum is right");
	}

	/* 2. The slot frees when the CQE posts: reusable straight away. */
	sqe_checksum(&sq[0], 3, 0, SLOT_SIZE, 0xcc);
	ret = submit(fd, sq, 1, cq, 1, 1, &completed);
	check(ret == 1 && completed == 1 && cq[0].res == fnv1a(pattern, SLOT_SIZE),
	      "the slot is free again once its completion posted");

	/* 3. Different slots do not collide. */
	memcpy(arena + 4 * SLOT_SIZE, pattern, SLOT_SIZE);
	sqe_checksum(&sq[0], 3, 0, SLOT_SIZE, 0x10);
	sqe_checksum(&sq[1], 4, 0, SLOT_SIZE, 0x11);
	ret = submit(fd, sq, 2, cq, 2, 2, &completed);
	a = find(cq, completed, 0x10);
	b = find(cq, completed, 0x11);
	check(ret == 2 && completed == 2 && a && b && a->res >= 0 && b->res >= 0,
	      "two ops on different slots both succeed");

	/* 4. A refused op must not release the slot the winner holds. */
	sqe_checksum(&sq[0], 3, 0, SLOT_SIZE, 0x20);
	sqe_checksum(&sq[1], 3, 0, SLOT_SIZE, 0x21);
	sqe_checksum(&sq[2], 3, 0, SLOT_SIZE, 0x22);
	ret = submit(fd, sq, 3, cq, 3, 3, &completed);
	int ok = 0, busy = 0;
	for (i = 0; i < completed; i++) {
		if (cq[i].res >= 0)
			ok++;
		else if (cq[i].res == -EBUSY)
			busy++;
	}
	check(ret == 3 && completed == 3 && ok == 1 && busy == 2,
	      "three on one slot: one succeeds, two get -EBUSY");

	/* 5. Slots past the first bitmap word work too. */
	memcpy(arena + 70 * SLOT_SIZE, pattern, SLOT_SIZE);
	sqe_checksum(&sq[0], 70, 0, SLOT_SIZE, 0x30);
	sqe_checksum(&sq[1], 70, 0, SLOT_SIZE, 0x31);
	ret = submit(fd, sq, 2, cq, 2, 2, &completed);
	a = find(cq, completed, 0x30);
	b = find(cq, completed, 0x31);
	check(ret == 2 && completed == 2 && a && b && ((a->res >= 0) ^ (b->res >= 0)),
	      "slot 70 (second bitmap word) is tracked separately");
	sqe_checksum(&sq[0], 6, 0, SLOT_SIZE, 0x32); /* same bit position, first word */
	sqe_checksum(&sq[1], 70, 0, SLOT_SIZE, 0x33);
	ret = submit(fd, sq, 2, cq, 2, 2, &completed);
	a = find(cq, completed, 0x32);
	b = find(cq, completed, 0x33);
	check(ret == 2 && completed == 2 && a && b && a->res >= 0 && b->res >= 0,
	      "  slots 6 and 70 do not alias each other");

	/* 6. A rejected op leaves no slot stuck busy. */
	sqe_checksum(&sq[0], 5, 0, SLOT_SIZE, 0x40);
	sq[0].off = SLOT_SIZE; /* off + len past the end */
	ret = submit(fd, sq, 1, cq, 1, 1, &completed);
	check(ret == 1 && completed == 1 && cq[0].res == -EINVAL,
	      "an out-of-range op is rejected before claiming a slot");
	sqe_checksum(&sq[0], 5, 0, SLOT_SIZE, 0x41);
	ret = submit(fd, sq, 1, cq, 1, 1, &completed);
	check(ret == 1 && completed == 1 && cq[0].res >= 0, "  and slot 5 is still usable");

	/* 7. Every slot can be held at once. */
	for (i = 0; i < 8; i++)
		sqe_checksum(&sq[i], i, 0, SLOT_SIZE, 0x50 + i);
	ret = submit(fd, sq, 8, cq, 8, 8, &completed);
	ok = 0;
	for (i = 0; i < completed; i++)
		if (cq[i].res >= 0)
			ok++;
	check(ret == 8 && completed == 8 && ok == 8, "eight distinct slots all succeed at once");

	munmap(arena, ARENA);
	close(fd);
	free(pattern);
	return test_end();
}
