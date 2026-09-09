// SPDX-License-Identifier: GPL-2.0
//
// T8 done test: kernel-enforced slot exclusivity.
// Structs mirror kernel/xring_abi.rs by hand until T16.

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define XRING_DEV "/dev/xring"
#define XRING_MAGIC 0x676e7278u
#define XRING_ABI_VERSION 1u

struct xring_params {
	uint32_t magic, abi_version, flags, sq_entries;
	uint32_t cq_entries, slot_size, slot_count, configured;
	uint64_t features, arena_size;
	uint32_t max_sq_entries, max_cq_entries, max_slot_size, max_slot_count;
	uint64_t max_arena_bytes;
	uint64_t reserved[4];
};

struct xring_sqe {
	uint8_t opcode, flags;
	uint16_t rsvd0;
	uint32_t len;
	uint64_t off;
	uint64_t user_data;
	uint32_t slot, handle;
};

struct xring_cqe {
	uint64_t user_data;
	int64_t res;
	uint32_t flags, rsvd0;
	uint64_t extra;
};

struct xring_enter {
	uint64_t sq_addr, cq_addr, timeout_ns;
	uint32_t to_submit, cq_space, min_complete, flags;
	uint32_t completed, submitted;
	uint64_t reserved[2];
};

#define XRING_IOC_SETUP _IOWR('x', 0x00, struct xring_params)
#define XRING_IOC_ENTER _IOWR('x', 0x02, struct xring_enter)

#define XRING_OP_CHECKSUM 6

#define SLOT_SIZE 4096u
#define SLOT_COUNT 80u /* > 64, so the bitmap spans two words */
#define ARENA ((uint64_t)SLOT_SIZE * SLOT_COUNT)

static int failures;

static void check(int ok, const char *what)
{
	printf("%-58s %s\n", what, ok ? "PASS" : "FAIL");
	if (!ok)
		failures++;
}

static int64_t fnv1a(const uint8_t *p, size_t n)
{
	uint64_t h = 0xcbf29ce484222325ull;
	size_t i;

	for (i = 0; i < n; i++) {
		h ^= p[i];
		h *= 0x100000001b3ull;
	}
	return (int64_t)(h & 0x7fffffffffffffffull);
}

static int open_ring(void)
{
	struct xring_params p;
	int fd = open(XRING_DEV, O_RDWR);

	if (fd < 0) {
		perror("open " XRING_DEV);
		return -1;
	}
	memset(&p, 0, sizeof(p));
	p.magic = XRING_MAGIC;
	p.abi_version = XRING_ABI_VERSION;
	p.sq_entries = 32;
	p.cq_entries = 64;
	p.slot_size = SLOT_SIZE;
	p.slot_count = SLOT_COUNT;
	if (ioctl(fd, XRING_IOC_SETUP, &p) != 0) {
		perror("SETUP");
		close(fd);
		return -1;
	}
	return fd;
}

static void checksum_sqe(struct xring_sqe *s, uint32_t slot, uint64_t user_data)
{
	memset(s, 0, sizeof(*s));
	s->opcode = XRING_OP_CHECKSUM;
	s->slot = slot;
	s->len = SLOT_SIZE;
	s->user_data = user_data;
}

/* Submit n SQEs and reap up to n completions, waiting for min. */
static int submit(int fd, struct xring_sqe *sq, unsigned n, struct xring_cqe *cq, unsigned min,
		  unsigned *completed)
{
	struct xring_enter e;
	int r;

	memset(&e, 0, sizeof(e));
	e.sq_addr = (uint64_t)(uintptr_t)sq;
	e.to_submit = n;
	e.cq_addr = (uint64_t)(uintptr_t)cq;
	e.cq_space = n;
	e.min_complete = min;
	r = ioctl(fd, XRING_IOC_ENTER, &e);
	if (completed)
		*completed = e.completed;
	return r;
}

/* Find a completion by user_data. */
static const struct xring_cqe *find(const struct xring_cqe *cq, unsigned n, uint64_t ud)
{
	unsigned i;

	for (i = 0; i < n; i++)
		if (cq[i].user_data == ud)
			return &cq[i];
	return NULL;
}

int main(void)
{
	struct xring_sqe sq[8];
	struct xring_cqe cq[8];
	const struct xring_cqe *a, *b;
	uint8_t *arena, *pattern;
	unsigned completed;
	int fd, ret;
	unsigned i;

	setvbuf(stdout, NULL, _IOLBF, 0);

	pattern = malloc(SLOT_SIZE);
	if (!pattern)
		return 1;
	for (i = 0; i < SLOT_SIZE; i++)
		pattern[i] = (uint8_t)(i * 17 + 3);

	fd = open_ring();
	if (fd < 0)
		return 1;
	arena = mmap(NULL, ARENA, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (arena == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	memcpy(arena + 3 * SLOT_SIZE, pattern, SLOT_SIZE);

	/* 1. Two ops naming one slot: the second is refused. */
	checksum_sqe(&sq[0], 3, 0xaa);
	checksum_sqe(&sq[1], 3, 0xbb);
	ret = submit(fd, sq, 2, cq, 2, &completed);
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
	checksum_sqe(&sq[0], 3, 0xcc);
	ret = submit(fd, sq, 1, cq, 1, &completed);
	check(ret == 1 && completed == 1 && cq[0].res == fnv1a(pattern, SLOT_SIZE),
	      "the slot is free again once its completion posted");

	/* 3. Different slots do not collide. */
	memcpy(arena + 4 * SLOT_SIZE, pattern, SLOT_SIZE);
	checksum_sqe(&sq[0], 3, 0x10);
	checksum_sqe(&sq[1], 4, 0x11);
	ret = submit(fd, sq, 2, cq, 2, &completed);
	a = find(cq, completed, 0x10);
	b = find(cq, completed, 0x11);
	check(ret == 2 && completed == 2 && a && b && a->res >= 0 && b->res >= 0,
	      "two ops on different slots both succeed");

	/* 4. A refused op must not release the slot the winner holds. */
	checksum_sqe(&sq[0], 3, 0x20);
	checksum_sqe(&sq[1], 3, 0x21);
	checksum_sqe(&sq[2], 3, 0x22);
	ret = submit(fd, sq, 3, cq, 3, &completed);
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
	checksum_sqe(&sq[0], 70, 0x30);
	checksum_sqe(&sq[1], 70, 0x31);
	ret = submit(fd, sq, 2, cq, 2, &completed);
	a = find(cq, completed, 0x30);
	b = find(cq, completed, 0x31);
	check(ret == 2 && completed == 2 && a && b && ((a->res >= 0) ^ (b->res >= 0)),
	      "slot 70 (second bitmap word) is tracked separately");
	checksum_sqe(&sq[0], 6, 0x32); /* same bit position, first word */
	checksum_sqe(&sq[1], 70, 0x33);
	ret = submit(fd, sq, 2, cq, 2, &completed);
	a = find(cq, completed, 0x32);
	b = find(cq, completed, 0x33);
	check(ret == 2 && completed == 2 && a && b && a->res >= 0 && b->res >= 0,
	      "  slots 6 and 70 do not alias each other");

	/* 6. A rejected op leaves no slot stuck busy. */
	checksum_sqe(&sq[0], 5, 0x40);
	sq[0].off = SLOT_SIZE; /* off + len past the end */
	ret = submit(fd, sq, 1, cq, 1, &completed);
	check(ret == 1 && completed == 1 && cq[0].res == -EINVAL,
	      "an out-of-range op is rejected before claiming a slot");
	checksum_sqe(&sq[0], 5, 0x41);
	ret = submit(fd, sq, 1, cq, 1, &completed);
	check(ret == 1 && completed == 1 && cq[0].res >= 0, "  and slot 5 is still usable");

	/* 7. Every slot can be held at once. */
	for (i = 0; i < 8; i++)
		checksum_sqe(&sq[i], i, 0x50 + i);
	ret = submit(fd, sq, 8, cq, 8, &completed);
	ok = 0;
	for (i = 0; i < completed; i++)
		if (cq[i].res >= 0)
			ok++;
	check(ret == 8 && completed == 8 && ok == 8, "eight distinct slots all succeed at once");

	munmap(arena, ARENA);
	close(fd);
	free(pattern);
	printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
	return failures ? 1 : 0;
}
