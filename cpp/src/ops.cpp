// SPDX-License-Identifier: MIT

#include <koru/ops.hpp>

#include <koru_abi.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <utility>

namespace koru {

namespace {

/// How long a `DELAY_NS` may be. `sleep_for` chunks anything longer.
constexpr u64 MAX_DELAY_NS = 3600ull * 1000 * 1000 * 1000;

/// Longest path the kernel takes. Checked here for a clearer error.
constexpr size_t PATH_MAX_LEN = 4096;

result<void> ok()
{
    return result<void>();
}

Span<u8> bytes_of(Str s)
{
    return Span<u8>(reinterpret_cast<const u8 *>(s.data()), s.size());
}

Error einval()
{
    return Error::from_errno(Errno(EINVAL));
}

/// Wait for a non-blocking handle to be usable. A regular file is on no
/// waitqueue and completes at once, which is why the caller retries rather
/// than trusting the mask.
task<result<void>> ready(Handle fd, u32 events)
{
    Completion c        = co_await reactor().submit(sqe::poll_add(0, fd, events));
    result<u64, Errno> v = from_res(c.res);
    if (!v.ok())
        co_return as_error(v.error());
    co_return ok();
}

/// One path op: the path at slot offset 0, `arg` after it where every path
/// op's argument goes. `handle` is zero on all but `MKDIR`.
task<result<std::pair<size_t, u64>>> path_op(u8 op, Str path, Span<u8> arg, u32 handle)
{
    BufSlot slot = co_await detail::acquire();
    size_t at    = size_t(arg_offset(0, u32(path.size())));
    if (path.empty() || path.size() >= PATH_MAX_LEN || at + arg.size() > slot.size())
        co_return einval();

    u8 *b = slot.bytes().data();
    memcpy(b, path.data(), path.size());
    if (!arg.empty())
        memcpy(b + at, arg.data(), arg.size());
    u32 index = slot.index();

    Completion c = co_await reactor().submit(
        sqe::path(op, 0, index, 0, u32(path.size()), handle), std::move(slot));
    result<u64, Errno> res = from_res(c.res);
    if (!res.ok())
        co_return as_error(res.error());
    co_return std::pair<size_t, u64>(size_t(res.value()), c.extra);
}

/// A two-path op: both paths back to back with one NUL between them.
task<result<void>> pair_op(u8 op, Str a, Str b)
{
    BufSlot slot = co_await detail::acquire();
    size_t n     = a.size() + 1 + b.size();
    if (a.empty() || b.empty() || n > slot.size())
        co_return einval();

    u8 *p = slot.bytes().data();
    memcpy(p, a.data(), a.size());
    p[a.size()] = 0;
    memcpy(p + a.size() + 1, b.data(), b.size());
    u32 index = slot.index();

    Completion c =
        co_await reactor().submit(sqe::path(op, 0, index, 0, u32(n), 0), std::move(slot));
    result<u64, Errno> res = from_res(c.res);
    if (!res.ok())
        co_return as_error(res.error());
    co_return ok();
}

/// `open_at` without the flag translation, for the places that want koru's own.
task<result<Handle>> open_at_koru(Str path, u32 flags)
{
    BufSlot slot = co_await detail::acquire();
    if (path.empty() || path.size() >= PATH_MAX_LEN || path.size() > slot.size())
        co_return einval();
    memcpy(slot.bytes().data(), path.data(), path.size());
    u32 index = slot.index();

    Completion c = co_await reactor().submit(sqe::open(0, index, 0, u32(path.size()), flags),
                                             std::move(slot));
    result<u64, Errno> res = from_res(c.res);
    if (!res.ok())
        co_return as_error(res.error());
    co_return Handle(res.value());
}

/// `STATX_AT` into `out`. The struct replaces the path it was given, so the
/// slot must hold both and the read-back is from offset 0.
task<result<koru_stat>> stat_at(Str path)
{
    BufSlot slot = co_await detail::acquire();
    if (path.empty() || path.size() > slot.size() || sizeof(koru_stat) > slot.size())
        co_return einval();
    memcpy(slot.bytes().data(), path.data(), path.size());
    u32 index = slot.index();

    Completion c = co_await reactor().submit(
        sqe::path(KORU_OP_STATX_AT, 0, index, 0, u32(path.size()), 0), std::move(slot));
    result<u64, Errno> res = from_res(c.res);
    if (!res.ok())
        co_return as_error(res.error());

    koru_stat st = {};
    size_t n     = size_t(res.value());
    if (n > sizeof(st))
        n = sizeof(st);
    memcpy(&st, c.slot.bytes().data(), n);
    co_return st;
}

FileInfo info_of(const koru_stat &s)
{
    return FileInfo{ detail::kind_of_mode(s.mode), s.size, detail::ms_of(s.mtime_sec, s.mtime_nsec) };
}

/// A directory where one may already be.
task<result<void>> dir_over(Str path)
{
    result<void> made = co_await make_dir(path);
    if (!made.ok() && !(made.error() == Kind::Exists))
        co_return made.error();
    if (made.ok())
        co_return ok();
    CO_LET(i, co_await stat_of(path, false));
    if (i.kind != FileKind::Dir)
        co_return Error::from_errno(Errno(EEXIST));
    co_return ok();
}

/// A link over a name that is taken: the old name goes, whatever it was.
task<result<void>> link_over(Str target, Str path)
{
    result<void> made = co_await make_link(target, path);
    if (made.ok() || !(made.error() == Kind::Exists))
        co_return made;
    CO_OK(co_await remove_path(path, true));
    co_return co_await make_link(target, path);
}

} // namespace

// ---------------------------------------------------------------------------
// The pure half
// ---------------------------------------------------------------------------

namespace detail {

String diag_line(Str who, Str what, Error why)
{
    String out(who);
    if (!what.empty()) {
        out += ": ";
        out += String(what);
    }
    out += ": ";
    out += kind_name(why.kind());
    out += "\n";
    return out;
}

String join(Str dir, Str name)
{
    String out(dir);
    if (!out.empty() && out.back() != '/')
        out += '/';
    out += String(name);
    return out;
}

FileKind kind_of_mode(u64 mode)
{
    switch (mode & KORU_S_IFMT) {
    case KORU_S_IFDIR:
        return FileKind::Dir;
    case KORU_S_IFLNK:
        return FileKind::Link;
    default:
        return FileKind::File;
    }
}

/// Milliseconds since the epoch, 0 for anything before it: Braam's `mtime` is
/// unsigned and its filesystems keep none.
u64 ms_of(i64 sec, u64 nsec)
{
    if (sec < 0)
        return 0;
    return u64(sec) * 1000 + nsec / 1000000;
}

/// Braam's open flags to koru's. The access mode is a pair of bits there and a
/// two-bit field here, which is the one place the two disagree.
result<std::pair<u32, u64>> open_flags(u32 flags)
{
    if (flags & ~O_ALL)
        return einval();

    u32 out;
    switch (flags & (O_READ | O_WRITE)) {
    case O_READ | O_WRITE:
        out = KORU_O_RDWR;
        break;
    case O_WRITE:
        out = KORU_O_WRONLY;
        break;
    default:
        out = KORU_O_RDONLY;
        break;
    }
    if (flags & O_CREATE)
        out |= KORU_O_CREAT;
    if (flags & O_TRUNC)
        out |= KORU_O_TRUNC;
    if (flags & O_APPEND)
        out |= KORU_O_APPEND;
    if (flags & O_EXCL) {
        // koru refuses O_EXCL alone; Braam's is meaningless without O_CREATE.
        if (!(flags & O_CREATE))
            return einval();
        out |= KORU_O_EXCL;
    }
    return std::pair<u32, u64>(out, CREATE_MODE);
}

/// One record header, its name, and on to the next. A malformed run stops the
/// walk rather than guessing: the kernel emits whole records or none.
void parse_dirents(Span<u8> buf, std::vector<std::pair<String, u8>> &out)
{
    constexpr size_t head = sizeof(koru_dirent);
    size_t at             = 0;
    while (buf.size() - at >= head) {
        koru_dirent d = {};
        memcpy(&d, buf.data() + at, head);
        size_t reclen = d.reclen, namelen = d.namelen;
        if (reclen < head + namelen || at + reclen > buf.size())
            return;
        Str name(reinterpret_cast<const char *>(buf.data() + at + head), namelen);
        if (name != "." && name != "..")
            out.emplace_back(String(name), u8(d.dtype & KORU_DT_MASK));
        at += reclen;
    }
}

// ---------------------------------------------------------------------------
// The plumbing
// ---------------------------------------------------------------------------

/// A slot, waiting for one if the pool is out. Yielding is enough because a
/// slot comes back when some other task's op completes, which is progress the
/// executor makes on its own.
task<BufSlot> acquire()
{
    Reactor &r = reactor();
    for (;;) {
        BufSlot s = r.pool().acquire();
        if (s.held())
            co_return std::move(s);
        co_await yield(r);
    }
}

task<result<size_t>> write_once(Handle fd, Span<u8> s)
{
    fd = std_fd(fd);
    for (;;) {
        BufSlot slot = co_await acquire();
        size_t n     = s.size() < slot.size() ? s.size() : slot.size();
        memcpy(slot.bytes().data(), s.data(), n);
        u64 off   = position(fd);
        u32 index = slot.index();

        Completion c = co_await reactor().submit(sqe::write(0, fd, index, off, u32(n)),
                                                 std::move(slot));
        c.slot.release();
        result<u64, Errno> res = from_res(c.res);
        if (res.ok()) {
            advance(fd, res.value());
            co_return size_t(res.value());
        }
        if (res.error() != Errno(EAGAIN))
            co_return as_error(res.error());
        CO_OK(co_await ready(fd, KORU_POLL_OUT));
    }
}

task<result<void>> write_bytes(Handle fd, Span<u8> s)
{
    fd = std_fd(fd);
    size_t at = 0;
    // The kernel refuses a zero-length write, and Braam's write_all of an
    // empty string is a no-op rather than an error.
    while (at < s.size()) {
        CO_LET(w, co_await write_once(fd, s.subspan(at)));
        if (w == 0)
            co_return Error::closed();
        at += w;
    }
    co_return ok();
}

task<result<size_t>> read_into(Handle fd, String &out, u32 max)
{
    fd = std_fd(fd);
    u32 want = max == 0 ? CHUNK : (max < READ_MAX ? max : READ_MAX);
    for (;;) {
        BufSlot slot = co_await acquire();
        u32 n        = want < slot.size() ? want : u32(slot.size());
        u64 off      = position(fd);
        u32 index    = slot.index();

        Completion c = co_await reactor().submit(sqe::read(0, fd, index, off, n), std::move(slot));
        result<u64, Errno> res = from_res(c.res);
        if (res.ok()) {
            size_t got = size_t(res.value());
            out.append(reinterpret_cast<const char *>(c.slot.bytes().data()), got);
            advance(fd, got);
            co_return got;
        }
        if (res.error() != Errno(EAGAIN))
            co_return as_error(res.error());
        c.slot.release();
        CO_OK(co_await ready(fd, KORU_POLL_IN));
    }
}

task<bool> is_console(Handle fd)
{
    fd = std_fd(fd);
    // The byte channel is a socket, so `STAT` cannot answer for it: only the
    // runtime knows which socket is the terminal.
    if (is_screen(fd))
        co_return true;
    BufSlot slot = reactor().pool().acquire();
    if (!slot.held())
        co_return false;
    u32 index = slot.index();

    Completion c = co_await reactor().submit(
        sqe::stat(0, fd, index, 0, u32(sizeof(koru_stat))), std::move(slot));
    if (c.res < 0)
        co_return false;
    koru_stat st = {};
    size_t n     = size_t(c.res);
    if (n > sizeof(st))
        n = sizeof(st);
    memcpy(&st, c.slot.bytes().data(), n);
    co_return (st.mode & KORU_S_IFMT) == KORU_S_IFCHR;
}

} // namespace detail

// ---------------------------------------------------------------------------
// Streams
// ---------------------------------------------------------------------------

task<result<void>> write_all(Handle fd, Str s)
{
    fd = std_fd(fd);
    co_return co_await detail::write_bytes(fd, bytes_of(s));
}

task<result<String>> read_some(Handle fd, u32 max)
{
    fd = std_fd(fd);
    String buf;
    CO_LET(n, co_await detail::read_into(fd, buf, max));
    if (n == 0)
        co_return Error::closed(); // end of input
    co_return std::move(buf);
}

task<result<String>> read_chunk(Handle fd)
{
    fd = std_fd(fd);
    co_return co_await read_some(fd, READ_MAX);
}

task<result<String>> read_file(Str path)
{
    CO_LET(fd, co_await open_read(path));
    String buf;
    result<void> bad = ok();
    for (;;) {
        result<size_t> n = co_await detail::read_into(fd, buf, READ_MAX);
        if (!n.ok()) {
            bad = n.error();
            break;
        }
        if (n.value() == 0)
            break;
    }
    co_await close_fd(fd);
    if (!bad.ok())
        co_return bad.error();
    co_return std::move(buf);
}

bool next_line(Str &rest, Str &line)
{
    if (rest.empty())
        return false;
    line = rest.split('\n', rest);
    return true;
}

Str next_field(Str &line)
{
    size_t skip = 0;
    while (skip < line.size() && (line[skip] == ' ' || line[skip] == '\t'))
        skip++;
    line = line.substr(skip);

    size_t n = 0;
    while (n < line.size() && line[n] != ' ' && line[n] != '\t')
        n++;
    Str out = line.substr(0, n);
    line    = line.substr(n);
    return out;
}

task<void> errln(Str who, Str what, Error why)
{
    String line = detail::diag_line(who, what, why);
    co_await write_all(err_fd(), line);
}

// ---------------------------------------------------------------------------
// Descriptors
// ---------------------------------------------------------------------------

task<result<i32>> open_at(Str path, u32 flags)
{
    std::pair<u32, u64> f = CO_TRY(detail::open_flags(flags));
    u32 kflags            = f.first;
    u64 mode              = f.second;

    BufSlot slot = co_await detail::acquire();
    size_t need  = (kflags & KORU_O_CREAT)
                       ? size_t(arg_offset(0, u32(path.size()))) + sizeof(u64)
                       : path.size();
    if (path.empty() || path.size() >= PATH_MAX_LEN || need > slot.size() ||
        path.find('\0') != Str::npos)
        co_return einval();

    u8 *b = slot.bytes().data();
    memcpy(b, path.data(), path.size());
    if (kflags & KORU_O_CREAT)
        memcpy(b + arg_offset(0, u32(path.size())), &mode, sizeof(mode));
    u32 index = slot.index();

    Completion c = co_await reactor().submit(sqe::open(0, index, 0, u32(path.size()), kflags),
                                             std::move(slot));
    c.slot.release();
    result<u64, Errno> res = from_res(c.res);
    if (!res.ok())
        co_return as_error(res.error());
    Handle h = Handle(res.value());

    // Only the creator knows whether an offset means anything on it, and this
    // is the one place a handle learns its own path.
    result<FileInfo> info = co_await stat_fd(h);
    bool seekable         = info.ok() && info.value().kind == FileKind::File;
    register_opened(h, seekable, (flags & O_WRITE) != 0, String(path));
    co_return i32(h);
}

task<result<i32>> open_read(Str path)
{
    co_return co_await open_at(path, O_READ);
}

task<void> close_fd(Handle fd)
{
    fd = std_fd(fd);
    if (release_handle(fd))
        co_await reactor().submit(sqe::close(0, fd));
}

task<result<Handle>> dup_fd(Handle fd)
{
    fd = std_fd(fd);
    if (retain_handle(fd))
        co_return fd;
    co_return einval();
}

task<result<u64>> seek_fd(Handle fd, i64 off, u32 whence)
{
    fd = std_fd(fd);
    if (!is_seekable(fd))
        co_return Error::from_errno(Errno(EOPNOTSUPP));

    u64 base = 0;
    switch (whence) {
    case SEEK_SET:
        base = 0;
        break;
    case SEEK_CUR:
        base = position(fd);
        break;
    case SEEK_END: {
        CO_LET(info, co_await stat_fd(fd));
        base = info.size;
        break;
    }
    default:
        co_return einval();
    }

    u64 at;
    if (off < 0) {
        // Negated in u64: -INT64_MIN does not fit an i64.
        u64 back = u64(-(off + 1)) + 1;
        if (back > base)
            co_return einval();
        at = base - back;
    } else {
        if (u64(off) > SEEK_MAX - base)
            co_return einval();
        at = base + u64(off);
    }
    seek_to(fd, at);
    co_return at;
}

task<result<void>> truncate_fd(Handle fd, u64 n)
{
    fd = std_fd(fd);
    if (!is_seekable(fd))
        co_return Error::from_errno(Errno(EOPNOTSUPP));
    if (!is_writable(fd))
        co_return Error::from_errno(Errno(EPERM));
    // koru truncates by **path**, so this needs the one `open_at` recorded.
    Option<String> path = handle_path(fd);
    if (!path)
        co_return Error::from_errno(Errno(EOPNOTSUPP));

    Span<u8> arg(reinterpret_cast<const u8 *>(&n), sizeof(n));
    CO_OK(co_await path_op(KORU_OP_TRUNCATE, *path, arg, 0));
    co_return ok();
}

// ---------------------------------------------------------------------------
// Metadata
// ---------------------------------------------------------------------------

task<result<FileInfo>> stat_of(Str path, bool follow)
{
    if (!follow) {
        // koru has no `lstat`: `STATX_AT` always follows. A successful
        // `READLINK` is what says "this is a symlink", and its length is the
        // size `lstat(2)` would report — so only a link's own mtime is lost.
        result<String> target = co_await read_link(path);
        if (target.ok())
            co_return FileInfo{ FileKind::Link, u64(target.value().size()), 0 };
        if (target.error().raw() != Errno(EINVAL))
            co_return target.error();
    }
    CO_LET(st, co_await stat_at(path));
    co_return info_of(st);
}

task<result<FileInfo>> stat_fd(Handle fd)
{
    fd = std_fd(fd);
    BufSlot slot = co_await detail::acquire();
    u32 index    = slot.index();

    Completion c = co_await reactor().submit(
        sqe::stat(0, fd, index, 0, u32(sizeof(koru_stat))), std::move(slot));
    result<u64, Errno> res = from_res(c.res);
    if (!res.ok())
        co_return as_error(res.error());

    koru_stat st = {};
    size_t n     = size_t(res.value());
    if (n > sizeof(st))
        n = sizeof(st);
    memcpy(&st, c.slot.bytes().data(), n);
    co_return info_of(st);
}

// ---------------------------------------------------------------------------
// Directories
// ---------------------------------------------------------------------------

task<result<Vec<DirEntry>>> list_dir(Str path)
{
    CO_LET(fd, co_await open_at_koru(path, KORU_O_RDONLY | KORU_O_DIRECTORY));
    std::vector<std::pair<String, u8>> names;
    u64 cookie       = 0;
    result<void> bad = ok();

    for (;;) {
        BufSlot slot = co_await detail::acquire();
        u32 len      = u32(slot.size());
        u32 index    = slot.index();
        Completion c =
            co_await reactor().submit(sqe::readdir(0, fd, index, cookie, len), std::move(slot));
        result<u64, Errno> res = from_res(c.res);
        if (!res.ok()) {
            bad = as_error(res.error());
            break;
        }
        if (res.value() == 0)
            break;
        detail::parse_dirents(Span<u8>(c.slot.bytes().data(), size_t(res.value())), names);
        cookie = c.extra;
    }
    co_await reactor().submit(sqe::close(0, fd));
    forget_handle(fd);
    if (!bad.ok())
        co_return bad.error();

    std::vector<DirEntry> out;
    out.reserve(names.size());
    for (const std::pair<String, u8> &n : names) {
        // A listing never resolves a link, so the type comes from the record —
        // which is the directory's own view — and a link's size is its
        // target's length.
        FileKind kind = n.second == KORU_DT_DIR   ? FileKind::Dir
                        : n.second == KORU_DT_LNK ? FileKind::Link
                                                  : FileKind::File;
        String full   = detail::join(path, n.first);
        // A vanished entry keeps its name and its type and reports no size: a
        // listing is a snapshot, and racing `rm` is not the caller's error.
        result<FileInfo> info = co_await stat_of(full, kind != FileKind::Link);
        DirEntry e;
        e.name  = n.first;
        e.kind  = kind;
        e.size  = info.ok() ? info.value().size : 0;
        e.mtime = info.ok() ? info.value().mtime : 0;
        out.push_back(std::move(e));
    }
    co_return std::move(out);
}

task<result<void>> make_dir(Str path)
{
    CO_OK(co_await path_op(KORU_OP_MKDIR, path, Span<u8>(), KORU_MKDIR_MODE_ALL));
    co_return ok();
}

task<result<void>> make_dir_all(Str path)
{
    String so_far;
    if (!path.empty() && path.front() == '/')
        so_far += '/';
    bool stood = false; // the last component was already there

    size_t at = 0;
    while (at <= path.size()) {
        size_t sep = path.find('/', at);
        Str name   = path.substr(at, sep == Str::npos ? Str::npos : sep - at);
        at         = sep == Str::npos ? path.size() + 1 : sep + 1;
        if (name.empty() || name == ".")
            continue;
        if (!so_far.empty() && so_far.back() != '/')
            so_far += '/';
        so_far += String(name);

        result<void> made = co_await make_dir(so_far);
        if (made.ok())
            stood = false;
        else if (made.error() == Kind::Exists)
            stood = true;
        else
            co_return made.error();
    }

    // Only the leaf is told apart; a file lower down fails the component below.
    if (stood) {
        CO_LET(i, co_await stat_of(so_far, false));
        if (i.kind != FileKind::Dir)
            co_return Error::from_errno(Errno(EEXIST));
    }
    co_return ok();
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

task<result<void>> remove_path(Str path, bool all)
{
    result<std::pair<size_t, u64>> gone = co_await path_op(KORU_OP_UNLINK, path, Span<u8>(), 0);
    if (gone.ok())
        co_return ok();
    if (!(gone.error() == Kind::IsDir))
        co_return gone.error();

    if (all) {
        // Depth first, over an explicit stack: a deep tree must not be a deep
        // chain of coroutine frames.
        std::vector<std::pair<String, bool>> stack;
        stack.emplace_back(String(path), false);
        while (!stack.empty()) {
            std::pair<String, bool> top = std::move(stack.back());
            stack.pop_back();
            if (top.second) {
                CO_OK(co_await path_op(KORU_OP_RMDIR, top.first, Span<u8>(), 0));
                continue;
            }
            stack.emplace_back(top.first, true);
            CO_LET(ents, co_await list_dir(top.first));
            for (const DirEntry &e : ents) {
                String full = detail::join(top.first, e.name);
                if (e.kind == FileKind::Dir)
                    stack.emplace_back(std::move(full), false);
                else
                    CO_OK(co_await path_op(KORU_OP_UNLINK, full, Span<u8>(), 0));
            }
        }
        co_return ok();
    }
    CO_OK(co_await path_op(KORU_OP_RMDIR, path, Span<u8>(), 0));
    co_return ok();
}

task<result<void>> touch_path(Str path)
{
    koru_times t  = {};
    t.atime_sec   = 0;
    t.atime_nsec  = KORU_UTIME_OMIT;
    t.mtime_sec   = 0;
    t.mtime_nsec  = KORU_UTIME_NOW;
    Span<u8> arg(reinterpret_cast<const u8 *>(&t), sizeof(t));
    CO_OK(co_await path_op(KORU_OP_UTIMES, path, arg, 0));
    co_return ok();
}

task<result<void>> make_link(Str target, Str path)
{
    co_return co_await pair_op(KORU_OP_SYMLINK, target, path);
}

task<result<String>> read_link(Str path)
{
    BufSlot slot = co_await detail::acquire();
    if (path.empty() || path.size() > slot.size())
        co_return einval();
    memcpy(slot.bytes().data(), path.data(), path.size());
    u32 index = slot.index();

    // The target replaces the path it was given, at the same offset.
    Completion c = co_await reactor().submit(
        sqe::path(KORU_OP_READLINK, 0, index, 0, u32(path.size()), 0), std::move(slot));
    result<u64, Errno> res = from_res(c.res);
    if (!res.ok())
        co_return as_error(res.error());
    co_return String(reinterpret_cast<const char *>(c.slot.bytes().data()), size_t(res.value()));
}

task<result<void>> rename_path(Str from, Str to)
{
    co_return co_await pair_op(KORU_OP_RENAME, from, to);
}

// ---------------------------------------------------------------------------
// Copying
// ---------------------------------------------------------------------------

task<result<void>> copy_file(Str from, Str to)
{
    CO_LET(src, co_await open_read(from));
    result<Handle> ds = co_await open_at(to, O_WRITE | O_CREATE | O_TRUNC);
    if (!ds.ok()) {
        co_await close_fd(src);
        co_return ds.error();
    }
    Handle dst = ds.value();

    u64 at           = 0;
    result<void> bad = ok();
    for (;;) {
        BufSlot slot = co_await detail::acquire();
        u32 len      = u32(slot.size());
        u32 index    = slot.index();
        Completion c =
            co_await reactor().submit(sqe::read(0, src, index, at, len), std::move(slot));
        result<u64, Errno> res = from_res(c.res);
        if (!res.ok()) {
            bad = as_error(res.error());
            break;
        }
        size_t n = size_t(res.value());
        if (n == 0)
            break;

        // The same slot back out, so the bytes are never copied twice.
        BufSlot back = std::move(c.slot);
        size_t left  = 0;
        while (left < n) {
            u32 bi       = back.index();
            Completion w = co_await reactor().submit(
                sqe::write(0, dst, bi, at + left, u32(n - left)), std::move(back));
            back                  = std::move(w.slot);
            result<u64, Errno> wr = from_res(w.res);
            if (!wr.ok()) {
                bad = as_error(wr.error());
                break;
            }
            if (wr.value() == 0) {
                bad = Error::closed();
                break;
            }
            // A short write leaves the rest at the front of the slot.
            size_t wrote = size_t(wr.value());
            left += wrote;
            if (left < n)
                memmove(back.bytes().data(), back.bytes().data() + wrote, n - left);
        }
        back.release();
        if (!bad.ok())
            break;
        at += n;
    }

    co_await close_fd(src);
    co_await close_fd(dst);
    co_return bad;
}

task<result<void>> copy_tree(Str from, Str to)
{
    CO_OK(co_await dir_over(to));

    Str root = from;
    while (root.size() > 1 && root.back() == '/')
        root.remove_suffix(1);
    if (root.empty())
        root = "/";

    std::vector<String> stack;
    stack.emplace_back(); // relative to `root`, "" being it

    while (!stack.empty()) {
        String rel = std::move(stack.back());
        stack.pop_back();
        String src = rel.empty() ? String(root) : detail::join(root, rel);

        CO_LET(ents, co_await list_dir(src));
        for (const DirEntry &e : ents) {
            String under = rel.empty() ? e.name : detail::join(rel, e.name);
            String dst   = detail::join(to, under);
            String full  = detail::join(src, e.name);
            switch (e.kind) {
            case FileKind::Dir:
                CO_OK(co_await dir_over(dst));
                stack.push_back(std::move(under));
                break;
            case FileKind::Link: {
                CO_LET(target, co_await read_link(full));
                CO_OK(co_await link_over(target, dst));
                break;
            }
            case FileKind::File:
                CO_OK(co_await copy_file(full, dst));
                break;
            }
        }
    }
    co_return ok();
}

// ---------------------------------------------------------------------------
// The process
// ---------------------------------------------------------------------------

task<result<String>> cwd_get()
{
    char buf[PATH_MAX_LEN];
    if (!getcwd(buf, sizeof(buf)))
        co_return Error::from_errno(Errno::last());
    co_return String(buf);
}

task<result<String>> cwd_set(Str path)
{
    String p(path);
    if (chdir(p.c_str()) != 0)
        co_return Error::from_errno(Errno::last());
    co_return co_await cwd_get();
}

task<result<void>> sleep_for(u32 ms)
{
    u64 left = u64(ms) * 1000000;
    while (left > 0) {
        u64 step             = left < MAX_DELAY_NS ? left : MAX_DELAY_NS;
        Completion c         = co_await reactor().submit(sqe::delay_ns(0, step));
        result<u64, Errno> r = from_res(c.res);
        if (!r.ok())
            co_return as_error(r.error());
        left -= step;
    }
    co_return ok();
}

task<result<Clock>> clock_now()
{
    struct timespec ts = {};
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        co_return Error::from_errno(Errno::last());

    // `tm_gmtoff` is ordinary POSIX, as `cwd_get`'s `getcwd` is. The Rust
    // binding reads the TZif file by hand instead, because std has no API for
    // this and that crate forbids `unsafe`; the answer is the same.
    struct tm local = {};
    time_t secs     = ts.tv_sec;
    i32 tz          = localtime_r(&secs, &local) ? i32(local.tm_gmtoff / 60) : 0;

    co_return Clock{ u64(ts.tv_sec) * 1000 + u64(ts.tv_nsec) / 1000000, tz };
}

} // namespace koru
