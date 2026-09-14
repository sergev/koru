// SPDX-License-Identifier: MIT
//
// Helpers for the device suite: the shared geometry, a configured and mapped
// ring, and the assertions every section repeats.
//
// This mirrors rust/sys/tests/common/mod.rs case for case, and shares no code
// with it. The two suites even use different fixture paths, so one can run
// while the other does.

#ifndef KORU_TESTS_COMMON_HPP
#define KORU_TESTS_COMMON_HPP

#include "harness.hpp"

#include <koru/pool.hpp>
#include <koru/ring.hpp>

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

/// The geometry most sections use. 64 KB slots are deliberate: a deferred READ
/// or CHECKSUM over a whole slot runs long enough to make race windows common.
inline constexpr uint32_t SQ      = 64;
inline constexpr uint32_t CQ      = 128;
inline constexpr uint32_t SLOT    = 65536;
inline constexpr uint32_t SLOTS   = 24;
inline constexpr uint32_t HANDLES = 32;

/// Its own path, so a concurrent Rust run cannot collide.
inline constexpr const char *PATFILE = "/tmp/koru-check-cpp-pattern";
inline constexpr size_t PATSIZE      = 65536;

koru::SetupConfig shared_config();

/// One SQE's answer: the two halves of a CQE a caller usually wants.
struct Res {
    int64_t res    = 0;
    uint64_t extra = 0;
};

/// A configured and mapped ring.
struct Mapped {
    koru::Ring ring;
    koru::Arena arena;

    static Mapped make(const koru::SetupConfig &cfg);
    static Mapped shared() { return make(shared_config()); }

    uint32_t slot_size() const { return arena.slot_size(); }
    uint32_t slot_count() const { return arena.slot_count(); }
    uint32_t handle_count() const { return ring.params().handle_count; }

    /// Whole slot as bytes. No op may name it.
    std::span<uint8_t> slot(uint32_t i) const { return arena.slot(i); }

    template <class F>
    void fill(uint32_t i, F f) const
    {
        std::span<uint8_t> s = slot(i);
        for (size_t j = 0; j < s.size(); j++)
            s[j] = f(j);
    }

    /// Zero the slot, copy the path without its NUL, return the byte count.
    uint32_t put_path(uint32_t slot, const char *path) const;

    /// One SQE to completion, keeping the CQE's `extra`.
    Res run_one_extra(const koru_sqe &s) const;
    int64_t run_one(const koru_sqe &s) const { return run_one_extra(s).res; }

    int64_t open_path(uint32_t slot, const char *path, uint32_t flags) const;
    int64_t close_handle(uint32_t handle) const;
    int64_t read_into(uint32_t handle, uint32_t slot, uint64_t off, uint32_t len) const;
    int64_t write_from(uint32_t handle, uint32_t slot, uint64_t off, uint32_t len) const;

    /// `KORU_O_CREAT`'s mode, after the path where a path op's argument goes.
    int64_t create_path(uint32_t slot, const char *path, uint32_t flags, uint64_t mode) const;

    /// A deferred op and one behind it that should be refused the slot it
    /// holds, submitted `rounds` times. Returns the second SQE's answer per
    /// round.
    ///
    /// Rounding is not decoration: a whole-slot CHECKSUM in a kworker can be
    /// done before the submit loop reaches the second SQE, and then the second
    /// one wins fairly. One run in ten misses the window on the first try, so
    /// a single pair is a flaky gate. See doc/Notes.md.
    template <class F>
    std::vector<Res> slot_race(uint64_t rounds, F build) const
    {
        std::vector<Res> out;
        for (uint64_t r = 0; r < rounds; r++) {
            uint64_t first = 0xaa0000 + r * 2, second = 0xaa0001 + r * 2;
            koru_sqe sq[2];
            build(first, second, sq);
            koru_cqe cq[2]      = {};
            koru::EnterResult e = ring.enter(sq, cq, 2);
            if (!CHECK(e.ok()))
                break;
            if (!CHECK_EQ(e.value().progress.completed, 2))
                break;
            CHECK(find_cqe(cq, first).res >= 0); // the first op wins
            const koru_cqe &c = find_cqe(cq, second);
            out.push_back(Res{ c.res, c.extra });
        }
        return out;
    }

    /// Assert nothing was left in flight.
    void assert_quiesced() const;

    static const koru_cqe &find_cqe(std::span<const koru_cqe> cq, uint64_t user_data);
};

const koru_cqe &find_cqe(std::span<const koru_cqe> cq, uint64_t user_data);

/// Assert the exact errno, never just that the call failed.
void expect_enter_errno(const koru::EnterResult &r, int want, const char *what);

template <class T>
void expect_errno(const koru::result<T> &r, int want, const char *what)
{
    if (r.ok()) {
        FAILF("%s: succeeded, expected errno %d", what, want);
        return;
    }
    if (r.error().raw().value != want)
        FAILF("%s: errno %d, want %d", what, r.error().raw().value, want);
}

uint8_t pattern_byte(size_t i);

/// Must match the kernel's exactly, mask included.
int64_t fnv1a(std::span<const uint8_t> p);

bool make_pattern_file(const char *path, size_t n);
void ensure_pattern_file();

/// Field 1 of /proc/sys/fs/file-nr: allocated struct files.
int64_t file_nr();
/// `fput` can be deferred to task work, so let it settle first.
int64_t file_nr_settled();

size_t count_fds();
bool is_root();
uint64_t now_ms();
void sleep_ms(unsigned ms);

#endif // KORU_TESTS_COMMON_HPP
