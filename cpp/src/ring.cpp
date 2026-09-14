// SPDX-License-Identifier: MIT

#include <koru/ring.hpp>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace koru {

namespace {

Error last_error()
{
    return Error::from_errno(Errno::last());
}

EnterError last_enter_error(const koru_enter &e)
{
    // Read the writeback before anything else, or EINTR loses it.
    return EnterError{ Errno::last(), Progress{ e.submitted, e.completed } };
}

} // namespace

koru_params SetupConfig::to_params() const
{
    koru_params p  = {};
    p.magic        = KORU_MAGIC;
    p.abi_version  = KORU_ABI_VERSION;
    p.flags        = flags;
    p.sq_entries   = sq_entries;
    p.cq_entries   = cq_entries;
    p.slot_size    = slot_size;
    p.slot_count   = slot_count;
    p.handle_count = handle_count;
    return p;
}

// ---------------------------------------------------------------------------
// Arena
// ---------------------------------------------------------------------------

Arena::Arena(Arena &&o) noexcept
    : ptr_(o.ptr_), len_(o.len_), slot_size_(o.slot_size_), slot_count_(o.slot_count_)
{
    o.ptr_ = nullptr;
    o.len_ = 0;
}

Arena &Arena::operator=(Arena &&o) noexcept
{
    if (this != &o) {
        if (ptr_)
            ::munmap(ptr_, len_);
        ptr_        = o.ptr_;
        len_        = o.len_;
        slot_size_  = o.slot_size_;
        slot_count_ = o.slot_count_;
        o.ptr_      = nullptr;
        o.len_      = 0;
    }
    return *this;
}

Arena::~Arena()
{
    if (ptr_)
        ::munmap(ptr_, len_);
}

result<Arena> Arena::map(int fd, size_t len, uint32_t slot_size, uint32_t slot_count)
{
    void *p = ::mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED)
        return last_error();
    Arena a;
    a.ptr_        = static_cast<uint8_t *>(p);
    a.len_        = len;
    a.slot_size_  = slot_size;
    a.slot_count_ = slot_count;
    return a;
}

std::span<uint8_t> Arena::slot(uint32_t i) const
{
    if (!ptr_ || i >= slot_count_)
        return {};
    return std::span<uint8_t>(ptr_ + size_t(i) * slot_size_, slot_size_);
}

result<void> Arena::madvise(int advice)
{
    if (::madvise(ptr_, len_, advice) < 0)
        return last_error();
    return {};
}

result<void> Arena::unmap()
{
    if (!ptr_)
        return {};
    int rc = ::munmap(ptr_, len_);
    ptr_   = nullptr;
    len_   = 0;
    if (rc < 0)
        return last_error();
    return {};
}

// ---------------------------------------------------------------------------
// Ring
// ---------------------------------------------------------------------------

Ring::Ring(Ring &&o) noexcept : fd_(o.fd_), params_(o.params_), configured_(o.configured_)
{
    o.fd_ = -1;
}

Ring &Ring::operator=(Ring &&o) noexcept
{
    if (this != &o) {
        if (fd_ >= 0)
            ::close(fd_);
        fd_         = o.fd_;
        params_     = o.params_;
        configured_ = o.configured_;
        o.fd_       = -1;
    }
    return *this;
}

Ring::~Ring()
{
    if (fd_ >= 0)
        ::close(fd_);
}

result<Ring> Ring::open(const char *path)
{
    int fd = ::open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return last_error();
    Ring r;
    r.fd_ = fd;
    return r;
}

result<Ring> Ring::with_config(const SetupConfig &cfg)
{
    result<Ring> r = Ring::open();
    if (!r)
        return r;
    Ring ring = std::move(r).take();
    if (result<void> s = ring.setup(cfg); !s)
        return s.error();
    return ring;
}

result<void> Ring::close()
{
    if (fd_ < 0)
        return {};
    int rc = ::close(fd_);
    fd_    = -1;
    if (rc < 0)
        return last_error();
    return {};
}

result<void> Ring::setup(const SetupConfig &cfg)
{
    koru_params p = cfg.to_params();
    return setup_raw(p);
}

result<void> Ring::setup_raw(koru_params &p)
{
    if (::ioctl(fd_, KORU_IOC_SETUP, &p) < 0)
        return last_error();
    params_     = p;
    configured_ = true;
    return {};
}

result<koru_params> Ring::get_params() const
{
    koru_params p = {};
    if (::ioctl(fd_, KORU_IOC_GET_PARAMS, &p) < 0)
        return last_error();
    return p;
}

result<Arena> Ring::mmap() const
{
    return Arena::map(fd_, size_t(params_.arena_size), params_.slot_size, params_.slot_count);
}

EnterResult Ring::enter(std::span<const koru_sqe> sq, std::span<koru_cqe> cq, uint32_t min_complete,
                        uint64_t timeout_ns) const
{
    if (min_complete > cq.size())
        return EnterError{ Errno(EINVAL), Progress{} };
    koru_enter e   = {};
    e.sq_addr      = uint64_t(uintptr_t(sq.data()));
    e.cq_addr      = uint64_t(uintptr_t(cq.data()));
    e.timeout_ns   = timeout_ns;
    e.to_submit    = uint32_t(sq.size());
    e.cq_space     = uint32_t(cq.size());
    e.min_complete = min_complete;
    return enter_raw(e);
}

EnterResult Ring::enter_raw(koru_enter &e) const
{
    int r = ::ioctl(fd_, KORU_IOC_ENTER, &e);
    if (r < 0)
        return last_enter_error(e);
    return Entered{ uint32_t(r), Progress{ e.submitted, e.completed } };
}

result<int> Ring::ioctl_raw(unsigned long request, void *arg) const
{
    int r = ::ioctl(fd_, request, arg);
    if (r < 0)
        return last_error();
    return r;
}

EnterResult Ring::submit(std::span<const koru_sqe> sq) const
{
    return enter(sq, {}, 0);
}

EnterResult Ring::reap(std::span<koru_cqe> cq, uint32_t min_complete, uint64_t timeout_ns) const
{
    return enter({}, cq, min_complete, timeout_ns);
}

EnterResult Ring::run_one(const koru_sqe &s, koru_cqe &out) const
{
    out = koru_cqe{};
    return enter(std::span<const koru_sqe>(&s, 1), std::span<koru_cqe>(&out, 1), 1);
}

result<uint32_t, EnterError> Ring::quiesce() const
{
    uint32_t space = params_.cq_entries;
    if (space == 0)
        space = 1;
    if (space > 128)
        space = 128;
    std::vector<koru_cqe> cq(space);
    uint32_t found = 0, empty = 0;
    for (int i = 0; i < 64; i++) {
        EnterResult r = reap(cq, 1, 50 * 1000 * 1000);
        if (!r)
            return r.error();
        if (r.value().progress.completed == 0) {
            if (++empty == 3)
                break;
        } else {
            empty = 0;
            found += r.value().progress.completed;
        }
    }
    return found;
}

// ---------------------------------------------------------------------------
// SQE constructors
// ---------------------------------------------------------------------------

namespace sqe {

koru_sqe nop(uint64_t user_data)
{
    koru_sqe s  = {};
    s.opcode    = KORU_OP_NOP;
    s.user_data = user_data;
    return s;
}

koru_sqe delay_ns(uint64_t user_data, uint64_t ns)
{
    koru_sqe s  = {};
    s.opcode    = KORU_OP_DELAY_NS;
    s.off       = ns;
    s.user_data = user_data;
    return s;
}

koru_sqe checksum(uint64_t user_data, uint32_t slot, uint64_t off, uint32_t len)
{
    koru_sqe s  = {};
    s.opcode    = KORU_OP_CHECKSUM;
    s.len       = len;
    s.off       = off;
    s.user_data = user_data;
    s.slot      = slot;
    return s;
}

koru_sqe open(uint64_t user_data, uint32_t slot, uint64_t off, uint32_t len, uint32_t flags)
{
    koru_sqe s  = {};
    s.opcode    = KORU_OP_OPEN;
    s.len       = len;
    s.off       = off;
    s.user_data = user_data;
    s.slot      = slot;
    s.handle    = flags;
    return s;
}

koru_sqe close(uint64_t user_data, uint32_t handle)
{
    koru_sqe s  = {};
    s.opcode    = KORU_OP_CLOSE;
    s.user_data = user_data;
    s.handle    = handle;
    return s;
}

koru_sqe read(uint64_t user_data, uint32_t handle, uint32_t slot, uint64_t off, uint32_t len)
{
    koru_sqe s  = {};
    s.opcode    = KORU_OP_READ;
    s.len       = len;
    s.off       = off;
    s.user_data = user_data;
    s.slot      = slot;
    s.handle    = handle;
    return s;
}

koru_sqe write(uint64_t user_data, uint32_t handle, uint32_t slot, uint64_t off, uint32_t len)
{
    koru_sqe s  = {};
    s.opcode    = KORU_OP_WRITE;
    s.len       = len;
    s.off       = off;
    s.user_data = user_data;
    s.slot      = slot;
    s.handle    = handle;
    return s;
}

koru_sqe cancel(uint64_t user_data, uint64_t target)
{
    koru_sqe s  = {};
    s.opcode    = KORU_OP_CANCEL;
    s.off       = target;
    s.user_data = user_data;
    return s;
}

koru_sqe adopt_fd(uint64_t user_data, int fd)
{
    koru_sqe s = {};
    s.opcode = KORU_OP_ADOPT_FD;
    s.off = uint64_t(fd);
    s.user_data = user_data;
    return s;
}

koru_sqe poll_add(uint64_t user_data, uint32_t handle, uint32_t events)
{
    koru_sqe s  = {};
    s.opcode    = KORU_OP_POLL_ADD;
    s.len       = events;
    s.user_data = user_data;
    s.handle    = handle;
    return s;
}

koru_sqe stat(uint64_t user_data, uint32_t handle, uint32_t slot, uint64_t off, uint32_t len)
{
    koru_sqe s  = {};
    s.opcode    = KORU_OP_STAT;
    s.len       = len;
    s.off       = off;
    s.user_data = user_data;
    s.slot      = slot;
    s.handle    = handle;
    return s;
}

koru_sqe readdir(uint64_t user_data, uint32_t handle, uint32_t slot, uint64_t off, uint32_t len)
{
    koru_sqe s  = {};
    s.opcode    = KORU_OP_READDIR;
    s.len       = len;
    s.off       = off;
    s.user_data = user_data;
    s.slot      = slot;
    s.handle    = handle;
    return s;
}

koru_sqe path(uint8_t op, uint64_t user_data, uint32_t slot, uint64_t off, uint32_t len,
              uint32_t handle)
{
    koru_sqe s  = {};
    s.opcode    = op;
    s.len       = len;
    s.off       = off;
    s.user_data = user_data;
    s.slot      = slot;
    s.handle    = handle;
    return s;
}

} // namespace sqe

uint64_t arg_offset(uint64_t off, uint32_t len)
{
    return (off + len + 7) & ~uint64_t(7);
}

} // namespace koru
