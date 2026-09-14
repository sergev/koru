// SPDX-License-Identifier: MIT

#include "common.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <unistd.h>

koru::SetupConfig shared_config()
{
    koru::SetupConfig c;
    c.sq_entries   = SQ;
    c.cq_entries   = CQ;
    c.slot_size    = SLOT;
    c.slot_count   = SLOTS;
    c.handle_count = HANDLES;
    return c;
}

Mapped Mapped::make(const koru::SetupConfig &cfg)
{
    koru::result<koru::Ring> r = koru::Ring::with_config(cfg);
    if (!r)
        koru::fail("SETUP: the device is not there, so nothing below proves anything");
    Mapped m;
    m.ring                      = std::move(r).take();
    koru::result<koru::Arena> a = m.ring.mmap();
    if (!a)
        koru::fail("mmap");
    m.arena = std::move(a).take();
    return m;
}

uint32_t Mapped::put_path(uint32_t slot, const char *path) const
{
    std::span<uint8_t> s = arena.slot(slot);
    memset(s.data(), 0, s.size());
    size_t n = strlen(path);
    memcpy(s.data(), path, n);
    return uint32_t(n);
}

Res Mapped::run_one_extra(const koru_sqe &s) const
{
    koru_cqe cq         = {};
    koru::EnterResult e = ring.run_one(s, cq);
    if (!e) {
        FAILF("ENTER: %s", e.error().err.message());
        return Res{};
    }
    if (e.value().progress.completed != 1) {
        FAILF("no completion");
        return Res{};
    }
    return Res{ cq.res, cq.extra };
}

int64_t Mapped::open_path(uint32_t slot, const char *path, uint32_t flags) const
{
    uint32_t n = put_path(slot, path);
    return run_one(koru::sqe::open(0x100, slot, 0, n, flags));
}

int64_t Mapped::close_handle(uint32_t handle) const
{
    return run_one(koru::sqe::close(0x101, handle));
}

int64_t Mapped::read_into(uint32_t handle, uint32_t slot, uint64_t off, uint32_t len) const
{
    return run_one(koru::sqe::read(0x102, handle, slot, off, len));
}

int64_t Mapped::write_from(uint32_t handle, uint32_t slot, uint64_t off, uint32_t len) const
{
    return run_one(koru::sqe::write(0x103, handle, slot, off, len));
}

int64_t Mapped::create_path(uint32_t slot, const char *path, uint32_t flags, uint64_t mode) const
{
    uint32_t n  = put_path(slot, path);
    uint64_t at = koru::arg_offset(0, n);
    memcpy(arena.slot(slot).data() + at, &mode, sizeof(mode));
    return run_one(koru::sqe::open(0x108, slot, 0, n, flags));
}

void Mapped::assert_quiesced() const
{
    koru::result<uint32_t, koru::EnterError> left = ring.quiesce();
    if (!left) {
        FAILF("quiesce: %s", left.error().err.message());
        return;
    }
    if (left.value() != 0)
        FAILF("left %u completion(s) in flight", left.value());
}

const koru_cqe &Mapped::find_cqe(std::span<const koru_cqe> cq, uint64_t user_data)
{
    return ::find_cqe(cq, user_data);
}

const koru_cqe &find_cqe(std::span<const koru_cqe> cq, uint64_t user_data)
{
    for (const koru_cqe &c : cq)
        if (c.user_data == user_data)
            return c;
    static koru_cqe none = {};
    FAILF("no CQE with user_data %#llx", (unsigned long long)user_data);
    none = koru_cqe{};
    return none;
}

void expect_enter_errno(const koru::EnterResult &r, int want, const char *what)
{
    if (r.ok()) {
        FAILF("%s: ENTER succeeded, expected errno %d", what, want);
        return;
    }
    if (r.error().err.value != want)
        FAILF("%s: errno %d, want %d", what, r.error().err.value, want);
}

uint8_t pattern_byte(size_t i)
{
    return uint8_t(i * 31 + (i >> 8) * 7 + 11);
}

int64_t fnv1a(std::span<const uint8_t> p)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint8_t b : p) {
        h ^= b;
        h *= 0x00000100000001b3ull;
    }
    return int64_t(h & 0x7fffffffffffffffull);
}

bool make_pattern_file(const char *path, size_t n)
{
    std::vector<uint8_t> buf(n);
    for (size_t i = 0; i < n; i++)
        buf[i] = pattern_byte(i);
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    bool ok = fwrite(buf.data(), 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

void ensure_pattern_file()
{
    static bool done = false;
    if (done)
        return;
    if (!make_pattern_file(PATFILE, PATSIZE))
        koru::fail("could not write the pattern file");
    done = true;
}

int64_t file_nr()
{
    FILE *f = fopen("/proc/sys/fs/file-nr", "r");
    if (!f)
        return -1;
    long long v = -1;
    if (fscanf(f, "%lld", &v) != 1)
        v = -1;
    fclose(f);
    return v;
}

int64_t file_nr_settled()
{
    sleep_ms(100);
    return file_nr();
}

size_t count_fds()
{
    DIR *d = opendir("/proc/self/fd");
    if (!d)
        return 0;
    size_t n = 0;
    while (readdir(d))
        n++;
    closedir(d);
    return n;
}

bool is_root()
{
    return geteuid() == 0;
}

uint64_t now_ms()
{
    timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000 + uint64_t(ts.tv_nsec) / 1000000;
}

void sleep_ms(unsigned ms)
{
    timespec ts = { time_t(ms / 1000), long(ms % 1000) * 1000000 };
    nanosleep(&ts, nullptr);
}
