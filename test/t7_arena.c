// SPDX-License-Identifier: GPL-2.0
//
// T7 done test: the mmap'd arena and CHECKSUM.
// Structs mirror kernel/xring_abi.rs by hand until T16.

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
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

#define XRING_OP_DELAY_NS 1
#define XRING_OP_CHECKSUM 6

#define SLOT_SIZE 4096u
#define SLOT_COUNT 8u
#define ARENA (SLOT_SIZE * SLOT_COUNT)
#define MS 1000000ull

static int failures;

static void check(int ok, const char *what)
{
	printf("%-58s %s\n", what, ok ? "PASS" : "FAIL");
	if (!ok)
		failures++;
}

static void check_errno(int ret, int want, const char *what)
{
	if (ret >= 0) {
		printf("%-58s FAIL (succeeded, expected %s)\n", what, strerror(want));
		failures++;
	} else if (errno != want) {
		printf("%-58s FAIL (got %s, expected %s)\n", what, strerror(errno),
		       strerror(want));
		failures++;
	} else {
		printf("%-58s PASS\n", what);
	}
}

/* Must match the kernel's FNV-1a, masked to 63 bits. */
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

static int setup_ring(int fd, uint32_t slot_size, uint32_t slot_count)
{
	struct xring_params p;

	memset(&p, 0, sizeof(p));
	p.magic = XRING_MAGIC;
	p.abi_version = XRING_ABI_VERSION;
	p.sq_entries = 32;
	p.cq_entries = 64;
	p.slot_size = slot_size;
	p.slot_count = slot_count;
	return ioctl(fd, XRING_IOC_SETUP, &p);
}

static int open_ring(void)
{
	int fd = open(XRING_DEV, O_RDWR);

	if (fd < 0) {
		perror("open " XRING_DEV);
		return -1;
	}
	if (setup_ring(fd, SLOT_SIZE, SLOT_COUNT) != 0) {
		perror("SETUP");
		close(fd);
		return -1;
	}
	return fd;
}

/* Run one SQE to completion and return its res. */
static int64_t run_one(int fd, struct xring_sqe *s, int *ioctl_ret)
{
	struct xring_cqe c;
	struct xring_enter e;
	int r;

	memset(&c, 0, sizeof(c));
	memset(&e, 0, sizeof(e));
	e.sq_addr = (uint64_t)(uintptr_t)s;
	e.to_submit = 1;
	e.cq_addr = (uint64_t)(uintptr_t)&c;
	e.cq_space = 1;
	e.min_complete = 1;
	r = ioctl(fd, XRING_IOC_ENTER, &e);
	if (ioctl_ret)
		*ioctl_ret = r;
	if (r < 0 || e.completed != 1)
		return INT64_MIN;
	return c.res;
}

static void checksum_sqe(struct xring_sqe *s, uint32_t slot, uint64_t off, uint32_t len)
{
	memset(s, 0, sizeof(*s));
	s->opcode = XRING_OP_CHECKSUM;
	s->slot = slot;
	s->off = off;
	s->len = len;
	s->user_data = 0xc0de;
}

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

	setvbuf(stdout, NULL, _IOLBF, 0);

	pattern = malloc(SLOT_SIZE);
	if (!pattern)
		return 1;
	for (i = 0; i < SLOT_SIZE; i++)
		pattern[i] = (uint8_t)(i * 31 + 7);

	/* 1. mmap before SETUP is refused. */
	fd = open(XRING_DEV, O_RDWR);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	arena = mmap(NULL, ARENA, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	check(arena == MAP_FAILED && errno == EINVAL, "mmap before SETUP returns EINVAL");
	close(fd);

	/* 2. SETUP rejects a slot_size that is not a page multiple. */
	fd = open(XRING_DEV, O_RDWR);
	if (fd < 0)
		return 1;
	check_errno(setup_ring(fd, 100, 4), EINVAL, "SETUP with slot_size 100 returns EINVAL");
	check_errno(setup_ring(fd, 4097, 4), EINVAL, "SETUP with slot_size 4097 returns EINVAL");
	check(setup_ring(fd, SLOT_SIZE, SLOT_COUNT) == 0, "SETUP with a page multiple succeeds");

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
	checksum_sqe(&s, 3, 0, SLOT_SIZE);
	res = run_one(fd, &s, NULL);
	check(res == fnv1a(pattern, SLOT_SIZE), "pattern written to slot 3 checksums correctly");

	/* The kernel really read slot 3, not some other slot. */
	checksum_sqe(&s, 2, 0, SLOT_SIZE);
	res = run_one(fd, &s, NULL);
	check(res != INT64_MIN && res != fnv1a(pattern, SLOT_SIZE),
	      "  an untouched slot checksums differently");

	memcpy(arena + 0 * SLOT_SIZE, pattern, SLOT_SIZE);
	checksum_sqe(&s, 0, 0, SLOT_SIZE);
	res = run_one(fd, &s, NULL);
	check(res == fnv1a(pattern, SLOT_SIZE), "  slot 0 too");

	memcpy(arena + (SLOT_COUNT - 1) * SLOT_SIZE, pattern, SLOT_SIZE);
	checksum_sqe(&s, SLOT_COUNT - 1, 0, SLOT_SIZE);
	res = run_one(fd, &s, NULL);
	check(res == fnv1a(pattern, SLOT_SIZE), "  and the last slot");

	/* Position-sensitive: a rotated pattern must not match. */
	uint8_t *rot = malloc(SLOT_SIZE);
	memcpy(rot, pattern + 1, SLOT_SIZE - 1);
	rot[SLOT_SIZE - 1] = pattern[0];
	memcpy(arena + 1 * SLOT_SIZE, rot, SLOT_SIZE);
	checksum_sqe(&s, 1, 0, SLOT_SIZE);
	res = run_one(fd, &s, NULL);
	check(res == fnv1a(rot, SLOT_SIZE) && res != fnv1a(pattern, SLOT_SIZE),
	      "  a rotated pattern gives a different checksum");

	/* A sub-range, to exercise off and len. */
	checksum_sqe(&s, 3, 100, 1000);
	res = run_one(fd, &s, NULL);
	check(res == fnv1a(pattern + 100, 1000), "a sub-range at off 100 len 1000 matches");

	/* 5. CHECKSUM bounds. */
	checksum_sqe(&s, SLOT_COUNT, 0, 16);
	check(run_one(fd, &s, NULL) == -EINVAL, "slot out of range yields -EINVAL");

	checksum_sqe(&s, 0, SLOT_SIZE - 8, 16);
	check(run_one(fd, &s, NULL) == -EINVAL, "off + len past the slot end yields -EINVAL");

	checksum_sqe(&s, 0, UINT64_MAX, 16);
	check(run_one(fd, &s, NULL) == -EINVAL, "off + len overflow yields -EINVAL");

	checksum_sqe(&s, 0, 0, 16);
	s.handle = 1;
	check(run_one(fd, &s, NULL) == -EINVAL, "non-zero handle yields -EINVAL");

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
	memset(&s, 0, sizeof(s));
	s.opcode = XRING_OP_DELAY_NS;
	s.off = 300 * MS;
	struct xring_enter e;
	memset(&e, 0, sizeof(e));
	e.sq_addr = (uint64_t)(uintptr_t)&s;
	e.to_submit = 1;
	ret = ioctl(fd, XRING_IOC_ENTER, &e);
	check(ret == 1, "a delay is in flight");
	check(munmap(arena, ARENA) == 0, "munmap with an op in flight succeeds");
	usleep(500000);
	checksum_sqe(&s, 0, 0, 16);
	check(run_one(fd, &s, NULL) >= 0, "  the ring still works after the op lands");
	close(fd);

	/* 8. close(fd) with the mapping still up, then munmap. */
	fd = open_ring();
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
	printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
	return failures ? 1 : 0;
}
