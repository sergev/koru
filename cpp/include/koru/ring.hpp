// SPDX-License-Identifier: MIT
//
// The ring: `SETUP`, `ENTER`, and the mmap'd arena, with a destructor on each.
// The C++ half of rust/sys/src/ring.rs, and it shares no code with it: the
// second binding exists to show the ABI is language-neutral.
//
// Move-only throughout. A `Ring` owns its descriptor and an `Arena` owns its
// mapping, and the mapping deliberately does **not** borrow the ring: it stays
// valid after close(fd), which is a property of the kernel's mmap and a thing
// the suite asserts.

#ifndef KORU_RING_HPP
#define KORU_RING_HPP

#include <koru/error.hpp>
#include <koru/result.hpp>
#include <koru_abi.h>

#include <cstdint>
#include <span>

namespace koru {

/// What to ask `SETUP` for. Zero means "kernel default" for `cq_entries` and
/// `handle_count`.
struct SetupConfig {
    uint32_t sq_entries   = 0;
    uint32_t cq_entries   = 0;
    uint32_t slot_size    = 0;
    uint32_t slot_count   = 0;
    uint32_t handle_count = 0;
    uint32_t flags        = 0;

    koru_params to_params() const;
};

/// What `ENTER` wrote back. Valid on the error path too.
struct Progress {
    uint32_t submitted = 0;
    uint32_t completed = 0;

    friend bool operator==(Progress a, Progress b)
    {
        return a.submitted == b.submitted && a.completed == b.completed;
    }
};

/// A successful `ENTER`. `consumed` is the ioctl return, SQEs consumed, never
/// the completion count.
struct Entered {
    uint32_t consumed = 0;
    Progress progress;

    /// The CQEs actually written into the caller's array.
    std::span<const koru_cqe> cqes(std::span<const koru_cqe> cq) const
    {
        return cq.subspan(0, progress.completed);
    }
};

/// A failed `ENTER`, carrying the writeback: on `EINTR` the kernel still says
/// what it consumed and completed.
struct EnterError {
    Errno err;
    Progress progress;

    bool is_interrupted() const { return err == Errno(EINTR); }
};

/// Drops the writeback: keep the `EnterError` itself to resubmit.
inline Error to_error(EnterError e)
{
    return Error::from_errno(e.err);
}

using EnterResult = result<Entered, EnterError>;

/// The mmap'd arena.
class Arena {
public:
    Arena() = default;
    ~Arena();

    Arena(const Arena &)            = delete;
    Arena &operator=(const Arena &) = delete;
    Arena(Arena &&o) noexcept;
    Arena &operator=(Arena &&o) noexcept;

    static result<Arena> map(int fd, size_t len, uint32_t slot_size, uint32_t slot_count);

    bool mapped() const { return ptr_ != nullptr; }
    size_t len() const { return len_; }
    uint32_t slot_size() const { return slot_size_; }
    uint32_t slot_count() const { return slot_count_; }
    uint8_t *data() const { return ptr_; }

    /// One slot. Out of range is an empty span, never a pointer past the end.
    ///
    /// No op naming the slot may be in flight: the kernel writes there from a
    /// kworker. Nothing in C++ can enforce that, which is what the buffer pool
    /// is advisory about and the kernel's `slot_busy` bitmap is not.
    std::span<uint8_t> slot(uint32_t i) const;

    result<void> madvise(int advice);

    /// Unmap explicitly, reporting the errno.
    result<void> unmap();

private:
    uint8_t *ptr_        = nullptr;
    size_t len_          = 0;
    uint32_t slot_size_  = 0;
    uint32_t slot_count_ = 0;
};

class Ring {
public:
    Ring() = default;
    ~Ring();

    Ring(const Ring &)            = delete;
    Ring &operator=(const Ring &) = delete;
    Ring(Ring &&o) noexcept;
    Ring &operator=(Ring &&o) noexcept;

    /// Open the device. No `SETUP`.
    static result<Ring> open(const char *path = KORU_DEV);

    /// Open and configure.
    static result<Ring> with_config(const SetupConfig &cfg);

    int fd() const { return fd_; }
    bool is_open() const { return fd_ >= 0; }
    bool configured() const { return configured_; }

    /// Close early, reporting the errno. The destructor does it silently.
    result<void> close();

    /// `SETUP`. One shot per fd; a second is `EBUSY`.
    result<void> setup(const SetupConfig &cfg);

    /// `SETUP` with a caller-built struct, for the rejection matrix.
    result<void> setup_raw(koru_params &p);

    result<koru_params> get_params() const;

    /// The values `SETUP` returned. Zeroed before a successful `SETUP`.
    const koru_params &params() const { return params_; }

    /// Map the arena. One shot in the kernel; not gated here, so the second
    /// call's `EBUSY` stays testable.
    result<Arena> mmap() const;

    /// `ENTER`. A short `submitted` or `completed` is normal, not an error:
    /// admission control produces the first, a timeout or quiescence the
    /// second. `timeout_ns` of 0 means no cap, which is why there is no
    /// "zero timeout" spelling here.
    EnterResult enter(std::span<const koru_sqe> sq, std::span<koru_cqe> cq, uint32_t min_complete,
                      uint64_t timeout_ns = 0) const;

    /// `ENTER` with a caller-built struct, for the rejection matrix. The two
    /// addresses are bare integers the kernel will read and write.
    EnterResult enter_raw(koru_enter &e) const;

    /// Raw ioctl, for the dispatch matrix.
    result<int> ioctl_raw(unsigned long request, void *arg) const;

    /// Submit without reaping.
    EnterResult submit(std::span<const koru_sqe> sq) const;

    /// Reap without submitting. `min_complete` decides whether this waits at
    /// all; `timeout_ns` only caps the wait, and 0 means no cap.
    EnterResult reap(std::span<koru_cqe> cq, uint32_t min_complete, uint64_t timeout_ns = 0) const;

    /// Submit one SQE and wait for one completion. A completion count of 0 is
    /// possible — a cancelled or deferred op — so the CQE is out-parameter and
    /// the count is the answer.
    EnterResult run_one(const koru_sqe &sqe, koru_cqe &out) const;

    /// Drain completions until three consecutive empty rounds, returning how
    /// many were found. One empty reap is not enough: on a busy ring that is
    /// just the timeout.
    result<uint32_t, EnterError> quiesce() const;

private:
    int fd_             = -1;
    koru_params params_ = {};
    bool configured_    = false;
};

// ---------------------------------------------------------------------------
// SQE constructors
// ---------------------------------------------------------------------------
//
// Each zeroes everything its opcode does not read, because a field an opcode
// ignores must be zero. Only T4-T11's opcodes are here; the rest arrive with
// the operation layer.

namespace sqe {

koru_sqe nop(uint64_t user_data);
koru_sqe delay_ns(uint64_t user_data, uint64_t ns);
koru_sqe checksum(uint64_t user_data, uint32_t slot, uint64_t off, uint32_t len);
/// `handle` carries the open flags, not a handle.
koru_sqe open(uint64_t user_data, uint32_t slot, uint64_t off, uint32_t len, uint32_t flags);
koru_sqe close(uint64_t user_data, uint32_t handle);
koru_sqe read(uint64_t user_data, uint32_t handle, uint32_t slot, uint64_t off, uint32_t len);
koru_sqe write(uint64_t user_data, uint32_t handle, uint32_t slot, uint64_t off, uint32_t len);
/// `off` is the target's `user_data`, not an offset.
koru_sqe cancel(uint64_t user_data, uint64_t target);
/// `off` is the descriptor to adopt. It grants no authority the caller lacks.
koru_sqe adopt_fd(uint64_t user_data, int fd);

// The rest of the opcodes, which arrive with the operation layer.

/// Single-shot. A file on no waitqueue completes at once, with 0 where none of
/// the asked-for events can ever come.
koru_sqe poll_add(uint64_t user_data, uint32_t handle, uint32_t events);
/// `off` is a within-slot offset and must be 8-aligned; `len` is the caller's
/// buffer size, which is also its version negotiation.
koru_sqe stat(uint64_t user_data, uint32_t handle, uint32_t slot, uint64_t off, uint32_t len);
/// `off` is a resume cookie, 0 for the beginning.
koru_sqe readdir(uint64_t user_data, uint32_t handle, uint32_t slot, uint64_t off, uint32_t len);
/// Every path op: the caller has already put the path at `off` and any
/// argument after it. `handle` is zero on all but `MKDIR`, which spends it on
/// the mode.
koru_sqe path(uint8_t op, uint64_t user_data, uint32_t slot, uint64_t off, uint32_t len,
              uint32_t handle = 0);

} // namespace sqe

/// Where an opcode's argument goes: after the path, rounded up to eight.
/// Mirrors koru_sys::ring::arg_offset.
uint64_t arg_offset(uint64_t off, uint32_t len);

} // namespace koru

#endif // KORU_RING_HPP
