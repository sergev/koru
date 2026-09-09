// SPDX-License-Identifier: GPL-2.0
//
// T3 done test: SETUP and GET_PARAMS.
//
// Interim harness. The struct and ioctl numbers below are declared by hand and
// MUST agree with kernel/xring_abi.rs, which is canonical. At T16 this file
// switches to including user/cpp/include/xring_abi.h, and the conformance test
// takes over the job of keeping the two in step.

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define XRING_DEV "/dev/xring"

#define XRING_MAGIC 0x676e7278u /* "xrng" */
#define XRING_ABI_VERSION 1u

struct xring_params {
	/* in */
	uint32_t magic;
	uint32_t abi_version;
	uint32_t flags;
	/* in/out */
	uint32_t sq_entries;
	uint32_t cq_entries;
	uint32_t slot_size;
	uint32_t slot_count;
	/* out */
	uint32_t configured;
	uint64_t features;
	uint64_t arena_size;
	uint32_t max_sq_entries;
	uint32_t max_cq_entries;
	uint32_t max_slot_size;
	uint32_t max_slot_count;
	uint64_t max_arena_bytes;
	/* in: must be zero */
	uint64_t reserved[4];
};

_Static_assert(sizeof(struct xring_params) == 104, "params size must match kernel");

#define XRING_IOC_SETUP _IOWR('x', 0x00, struct xring_params)
#define XRING_IOC_GET_PARAMS _IOR('x', 0x01, struct xring_params)

static int failures;

static void check(int ok, const char *what)
{
	printf("%-58s %s\n", what, ok ? "PASS" : "FAIL");
	if (!ok)
		failures++;
}

/* Assert the ioctl failed with exactly this errno, not merely that it failed. */
static void check_errno(int ret, int want, const char *what)
{
	if (ret == 0) {
		printf("%-58s FAIL (succeeded, expected %s)\n", what, strerror(want));
		failures++;
		return;
	}
	if (errno != want) {
		printf("%-58s FAIL (got %s, expected %s)\n", what, strerror(errno),
		       strerror(want));
		failures++;
		return;
	}
	printf("%-58s PASS\n", what);
}

/* A request that must succeed, so each rejection test starts from a valid base. */
static void good_request(struct xring_params *p)
{
	memset(p, 0, sizeof(*p));
	p->magic = XRING_MAGIC;
	p->abi_version = XRING_ABI_VERSION;
	p->sq_entries = 64;
	p->cq_entries = 128;
	p->slot_size = 4096;
	p->slot_count = 32;
}

/* Each rejection test needs a pristine fd: a successful SETUP is one-shot. */
static int open_dev(void)
{
	int fd = open(XRING_DEV, O_RDWR);
	if (fd < 0) {
		perror("open " XRING_DEV);
		failures++;
	}
	return fd;
}

/* Run one SETUP that is expected to fail, on its own fd. */
static void reject(void (*mutate)(struct xring_params *), int want, const char *what)
{
	struct xring_params p;
	int fd = open_dev();
	if (fd < 0)
		return;
	good_request(&p);
	mutate(&p);
	check_errno(ioctl(fd, XRING_IOC_SETUP, &p), want, what);
	close(fd);
}

static void bad_magic(struct xring_params *p) { p->magic = 0xdeadbeef; }
static void bad_version(struct xring_params *p) { p->abi_version = 999; }
static void bad_flags(struct xring_params *p) { p->flags = 1; }
static void bad_reserved(struct xring_params *p) { p->reserved[2] = 1; }
static void zero_sq(struct xring_params *p) { p->sq_entries = 0; }
static void zero_slot_size(struct xring_params *p) { p->slot_size = 0; }
static void cq_too_shallow(struct xring_params *p)
{
	p->sq_entries = 64;
	p->cq_entries = 32;
}

int main(void)
{
	struct xring_params p, q;
	int fd;

	/* 1. GET_PARAMS before SETUP reports caps and configured == 0. */
	fd = open_dev();
	if (fd < 0)
		return 1;
	memset(&p, 0, sizeof(p));
	check(ioctl(fd, XRING_IOC_GET_PARAMS, &p) == 0, "GET_PARAMS before SETUP succeeds");
	check(p.magic == XRING_MAGIC, "  magic reported");
	check(p.abi_version == XRING_ABI_VERSION, "  abi_version reported");
	check(p.configured == 0, "  configured == 0 before SETUP");
	check(p.max_sq_entries > 0 && p.max_slot_size > 0 && p.max_slot_count > 0 &&
		      p.max_arena_bytes > 0,
	      "  caps are non-zero");

	/* 2. A valid SETUP round-trips. */
	good_request(&p);
	check(ioctl(fd, XRING_IOC_SETUP, &p) == 0, "SETUP with a valid request succeeds");
	check(p.sq_entries == 64 && p.cq_entries == 128, "  sq/cq entries round-trip");
	check(p.slot_size == 4096 && p.slot_count == 32, "  slot size/count round-trip");
	check(p.arena_size == (uint64_t)4096 * 32, "  arena_size is slot_size * slot_count");
	check(p.configured == 1, "  configured == 1 after SETUP");

	/* 3. GET_PARAMS after SETUP agrees. */
	memset(&q, 0, sizeof(q));
	check(ioctl(fd, XRING_IOC_GET_PARAMS, &q) == 0, "GET_PARAMS after SETUP succeeds");
	check(memcmp(&p, &q, sizeof(p)) == 0, "  reports the same struct SETUP returned");

	/* 4. A second SETUP on the same fd is refused. */
	good_request(&p);
	check_errno(ioctl(fd, XRING_IOC_SETUP, &p), EBUSY, "second SETUP returns EBUSY");

	/* 5. An unknown ioctl on a configured fd. */
	check_errno(ioctl(fd, _IO('x', 0x7f)), ENOTTY, "unknown ioctl returns ENOTTY");
	close(fd);

	/* 6. Identity failures are EPROTO, distinct from bad values. */
	reject(bad_magic, EPROTO, "bad magic returns EPROTO");
	reject(bad_version, EPROTO, "bad abi_version returns EPROTO");

	/* 7. Bad values are EINVAL. */
	reject(bad_flags, EINVAL, "unknown flag bit returns EINVAL");
	reject(bad_reserved, EINVAL, "non-zero reserved field returns EINVAL");
	reject(zero_sq, EINVAL, "sq_entries == 0 returns EINVAL");
	reject(zero_slot_size, EINVAL, "slot_size == 0 returns EINVAL");
	reject(cq_too_shallow, EINVAL, "cq_entries < sq_entries returns EINVAL");

	/* 8. Over-cap requests are rejected, not clamped. */
	fd = open_dev();
	if (fd < 0)
		return 1;
	memset(&p, 0, sizeof(p));
	if (ioctl(fd, XRING_IOC_GET_PARAMS, &p) != 0) {
		perror("GET_PARAMS");
		return 1;
	}
	uint32_t max_sq = p.max_sq_entries, max_slot_size = p.max_slot_size;
	uint32_t max_slot_count = p.max_slot_count;

	good_request(&p);
	p.sq_entries = max_sq + 1;
	check_errno(ioctl(fd, XRING_IOC_SETUP, &p), EINVAL, "sq_entries over cap returns EINVAL");

	good_request(&p);
	p.slot_size = max_slot_size;
	p.slot_count = max_slot_count;
	check_errno(ioctl(fd, XRING_IOC_SETUP, &p), EINVAL,
		    "arena over cap returns EINVAL (not clamped)");

	/* 9. Those failures must NOT have consumed the one-shot. */
	good_request(&p);
	check(ioctl(fd, XRING_IOC_SETUP, &p) == 0, "SETUP still works after rejected attempts");
	close(fd);

	printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
	return failures ? 1 : 0;
}
