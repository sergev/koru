// SPDX-License-Identifier: GPL-2.0
//
// T7 done test: the mmap'd arena and CHECKSUM.

#include "xring_test.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define SLOT_SIZE 4096u
#define SLOT_COUNT 8u
#define ARENA (SLOT_SIZE * SLOT_COUNT)

static int region_in_maps(void)
{
	char line[512];
	FILE *f = fopen("/proc/self/maps", "r");
	int found = 0;

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f))
		if (strstr(line, "/dev/xring"))
			found = 1;
	fclose(f);
	return found;
}

int main(void)
{
	struct xring_sqe s;
	uint8_t *arena, *pattern;
	int fd, ret;
	int64_t res;
	unsigned i;

	test_begin(120);

	pattern = malloc(SLOT_SIZE);
	if (!pattern)
		return 1;
	for (i = 0; i < SLOT_SIZE; i++)
		pattern[i] = (uint8_t)(i * 31 + 7);

	/* 1. mmap before SETUP is refused. */
	fd = open_dev();
	if (fd < 0)
		return 1;
	arena = mmap(NULL, ARENA, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	check(arena == MAP_FAILED && errno == EINVAL, "mmap before SETUP returns EINVAL");
	close(fd);

	/* 2. SETUP rejects a slot_size that is not a page multiple. */
	fd = open_dev();
	if (fd < 0)
		return 1;
	check_errno(setup_ring(fd, 32, 64, 100, 4), EINVAL, "SETUP with slot_size 100 returns EINVAL");
	check_errno(setup_ring(fd, 32, 64, 4097, 4), EINVAL, "SETUP with slot_size 4097 returns EINVAL");
	check(setup_ring(fd, 32, 64, SLOT_SIZE, SLOT_COUNT) == 0, "SETUP with a page multiple succeeds");

	/* 3. The mmap validation matrix. */
	arena = mmap(NULL, ARENA, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	check(arena == MAP_FAILED && errno == EINVAL, "MAP_PRIVATE returns EINVAL (no silent COW)");

	arena = mmap(NULL, ARENA - 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	check(arena == MAP_FAILED && errno == EINVAL, "a mapping one page short returns EINVAL");

	arena = mmap(NULL, ARENA + 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	check(arena == MAP_FAILED && errno == EINVAL, "a mapping one page long returns EINVAL");

	arena = mmap(NULL, ARENA, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 4096);
	check(arena == MAP_FAILED && errno == EINVAL, "a non-zero file offset returns EINVAL");

	arena = mmap(NULL, ARENA, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	check(arena != MAP_FAILED, "an exact MAP_SHARED mapping succeeds");
	if (arena == MAP_FAILED) {
		printf("cannot continue without the arena\n");
		return 1;
	}

	uint8_t *second = mmap(NULL, ARENA, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	check(second == MAP_FAILED && errno == EBUSY, "a second mmap returns EBUSY");

	/* 4. Pattern in slot 3 checksums correctly. */
	memcpy(arena + 3 * SLOT_SIZE, pattern, SLOT_SIZE);
	sqe_checksum(&s, 3, 0, SLOT_SIZE, 0xc0de);
	res = run_one(fd, &s);
	check(res == fnv1a(pattern, SLOT_SIZE), "pattern written to slot 3 checksums correctly");

	/* The kernel really read slot 3, not some other slot. */
	sqe_checksum(&s, 2, 0, SLOT_SIZE, 0xc0de);
	res = run_one(fd, &s);
	check(res != INT64_MIN && res != fnv1a(pattern, SLOT_SIZE),
	      "  an untouched slot checksums differently");

	memcpy(arena + 0 * SLOT_SIZE, pattern, SLOT_SIZE);
	sqe_checksum(&s, 0, 0, SLOT_SIZE, 0xc0de);
	res = run_one(fd, &s);
	check(res == fnv1a(pattern, SLOT_SIZE), "  slot 0 too");

	memcpy(arena + (SLOT_COUNT - 1) * SLOT_SIZE, pattern, SLOT_SIZE);
	sqe_checksum(&s, SLOT_COUNT - 1, 0, SLOT_SIZE, 0xc0de);
	res = run_one(fd, &s);
	check(res == fnv1a(pattern, SLOT_SIZE), "  and the last slot");

	/* Position-sensitive: a rotated pattern must not match. */
	uint8_t *rot = malloc(SLOT_SIZE);
	memcpy(rot, pattern + 1, SLOT_SIZE - 1);
	rot[SLOT_SIZE - 1] = pattern[0];
	memcpy(arena + 1 * SLOT_SIZE, rot, SLOT_SIZE);
	sqe_checksum(&s, 1, 0, SLOT_SIZE, 0xc0de);
	res = run_one(fd, &s);
	check(res == fnv1a(rot, SLOT_SIZE) && res != fnv1a(pattern, SLOT_SIZE),
	      "  a rotated pattern gives a different checksum");

	/* A sub-range, to exercise off and len. */
	sqe_checksum(&s, 3, 100, 1000, 0xc0de);
	res = run_one(fd, &s);
	check(res == fnv1a(pattern + 100, 1000), "a sub-range at off 100 len 1000 matches");

	/* 5. CHECKSUM bounds. */
	sqe_checksum(&s, SLOT_COUNT, 0, 16, 0xc0de);
	check(run_one(fd, &s) == -EINVAL, "slot out of range yields -EINVAL");

	sqe_checksum(&s, 0, SLOT_SIZE - 8, 16, 0xc0de);
	check(run_one(fd, &s) == -EINVAL, "off + len past the slot end yields -EINVAL");

	sqe_checksum(&s, 0, UINT64_MAX, 16, 0xc0de);
	check(run_one(fd, &s) == -EINVAL, "off + len overflow yields -EINVAL");

	sqe_checksum(&s, 0, 0, 16, 0xc0de);
	s.handle = 1;
	check(run_one(fd, &s) == -EINVAL, "non-zero handle yields -EINVAL");

	/* 6. The arena must not survive fork, and must not be made to. */
	check(region_in_maps() == 1, "the parent's maps show the region");
	ret = madvise(arena, ARENA, MADV_DOFORK);
	check(ret != 0, "madvise(MADV_DOFORK) is refused");

	pid_t pid = fork();
	if (pid == 0)
		_exit(region_in_maps() == 0 ? 0 : 1);
	int status = 0;
	waitpid(pid, &status, 0);
	check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	      "a forked child's maps lack the region");

	/* 7. munmap with an op in flight. */
	sqe_delay(&s, 0xd1, 300 * MS);
	struct xring_enter e;
	enter_init(&e, &s, 1, NULL, 0);
	ret = ioctl(fd, XRING_IOC_ENTER, &e);
	check(ret == 1, "a delay is in flight");
	check(munmap(arena, ARENA) == 0, "munmap with an op in flight succeeds");
	usleep(500000);
	sqe_checksum(&s, 0, 0, 16, 0xc0de);
	check(run_one(fd, &s) >= 0, "  the ring still works after the op lands");
	close(fd);

	/* 8. close(fd) with the mapping still up, then munmap. */
	fd = open_ring(32, 64, SLOT_SIZE, SLOT_COUNT);
	if (fd < 0)
		return 1;
	arena = mmap(NULL, ARENA, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	check(arena != MAP_FAILED, "a fresh ring maps again");
	if (arena != MAP_FAILED) {
		memcpy(arena + 2 * SLOT_SIZE, pattern, SLOT_SIZE);
		close(fd);
		/* The pages are kernel-owned and the mapping holds its own
		 * reference, so this must still read back. */
		check(memcmp(arena + 2 * SLOT_SIZE, pattern, SLOT_SIZE) == 0,
		      "the mapping still reads back after close(fd)");
		check(munmap(arena, ARENA) == 0, "munmap after close(fd) succeeds");
	}

	free(pattern);
	free(rot);
	return test_end();
}
