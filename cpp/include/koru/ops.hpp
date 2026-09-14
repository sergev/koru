// SPDX-License-Identifier: MIT
//
// Braam's operation layer, its signatures unchanged. The C++ half of
// rust/runtime/src/ops.rs, and it shares no code with it: each one is a slot
// acquisition, a submit, an await and a copy out.
//
// Three things are koru's rather than Braam's, and doc/Notes.md says why:
// `seek_fd` is pure userspace bookkeeping, `dup_fd` is a reference count on
// one handle, and `truncate_fd` goes by the path the handle was opened with.
//
// **Every `Str` argument must outlive the task.** A coroutine copies its
// parameters into the frame, and a view copied there still points at the
// caller's bytes — so `co_await read_file(String(p))` is a dangling read where
// `co_await read_file(p)` is not. Braam's surface has the same rule for the
// same reason; the Rust binding gets it from the borrow checker instead.

#ifndef KORU_OPS_HPP
#define KORU_OPS_HPP

#include <koru/rt.hpp>
#include <koru/task.hpp>
#include <koru/vocab.hpp>

#include <cstdio>  // SEEK_SET and its two kin are macros here
#include <fcntl.h> // and O_TRUNC, O_APPEND and O_EXCL here
#include <vector>

// Braam has no libc, so those six names are constants there. Here they are
// macros first, and a macro wins every lookup — so the names are taken back.
// The three `SEEK_*` values agree on both sides and are checked before the
// name goes; the three `O_*` deliberately do not, which is the whole reason
// `open_flags` translates.
//
// A source that wants POSIX's own `open(2)` flags must say so before this
// header, or use libkoru's `open_at`. doc/Notes.md records the trade.
static_assert(SEEK_SET == 0 && SEEK_CUR == 1 && SEEK_END == 2,
              "lseek's three whences are not the values Braam gives them");
#undef SEEK_SET
#undef SEEK_CUR
#undef SEEK_END
#undef O_TRUNC
#undef O_APPEND
#undef O_EXCL

namespace koru {

// ---------------------------------------------------------------------------
// Braam's constants
// ---------------------------------------------------------------------------

// Open flags, Braam's own bit values rather than koru's access-mode word, so a
// Braam call site compiles unchanged. `open_at` translates.

inline constexpr u32 O_READ   = 1;
inline constexpr u32 O_WRITE  = 2;
inline constexpr u32 O_CREATE = 4;
inline constexpr u32 O_TRUNC  = 8;
inline constexpr u32 O_APPEND = 16;
/// With [`O_CREATE`]; an existing name is `Exists`.
inline constexpr u32 O_EXCL = 32;

/// A bit outside this is `Invalid`, so a flag koru does not know is refused
/// rather than dropped.
inline constexpr u32 O_ALL = O_READ | O_WRITE | O_CREATE | O_TRUNC | O_APPEND | O_EXCL;

/// What a created file gets, before the umask. Braam's filesystem has no modes
/// and so no argument for one; this is `creat(2)`'s.
inline constexpr u64 CREATE_MODE = 0666;

inline constexpr u32 SEEK_SET = 0;
inline constexpr u32 SEEK_CUR = 1;
inline constexpr u32 SEEK_END = 2;

/// The largest position there is: the wire's offset is a signed 64-bit.
inline constexpr u64 SEEK_MAX = (u64(1) << 63) - 1;

/// What one read yields when the caller names no length, and the ceiling on
/// one that does. Braam's numbers.
inline constexpr u32 CHUNK    = 512;
inline constexpr u32 READ_MAX = 65536 - 4;

// ---------------------------------------------------------------------------
// Braam's records
// ---------------------------------------------------------------------------

/// What a listing says about an entry, never resolving a link. koru reports
/// more kinds than Braam's three, and everything that is neither a directory
/// nor a symlink reports as a file.
enum class FileKind { File, Dir, Link };

/// `mtime` is milliseconds since the epoch, 0 where none was reported.
struct FileInfo {
    FileKind kind = FileKind::File;
    u64 size      = 0;
    u64 mtime     = 0;

    friend bool operator==(const FileInfo &a, const FileInfo &b)
    {
        return a.kind == b.kind && a.size == b.size && a.mtime == b.mtime;
    }
};

struct DirEntry {
    String name;
    FileKind kind = FileKind::File;
    u64 size      = 0;
    u64 mtime     = 0;

    friend bool operator==(const DirEntry &a, const DirEntry &b)
    {
        return a.name == b.name && a.kind == b.kind && a.size == b.size && a.mtime == b.mtime;
    }
};

/// The wall clock: milliseconds since the epoch, and the local offset from UTC.
struct Clock {
    u64 epoch_ms = 0;
    i32 tz_min   = 0;
};

// ---------------------------------------------------------------------------
// Streams
// ---------------------------------------------------------------------------

/// Writes all of `s`, retrying a short write. The offset is userspace's own
/// bookkeeping, because `WRITE` never touches `f_pos`.
task<result<void>> write_all(Handle fd, Str s);

/// One chunk, or `Closed` at end of input.
task<result<String>> read_chunk(Handle fd);

/// At most `max` bytes, clamped to [`READ_MAX`]; 0 means [`CHUNK`]. What is
/// left stays on the descriptor, so the next read serves it first.
task<result<String>> read_some(Handle fd, u32 max);

/// A whole file, read through the ring.
task<result<String>> read_file(Str path);

/// A diagnostic on stderr: "who: what: why".
task<void> errln(Str who, Str what, Error why);

// ---------------------------------------------------------------------------
// Descriptors
// ---------------------------------------------------------------------------

/// Opens `path` with the `O_*` flags above.
task<result<Handle>> open_at(Str path, u32 flags);
task<result<Handle>> open_read(Str path);

/// Retires the handle. Braam's returns nothing: there is no answer a program
/// could act on, and the kernel frees the file either way.
task<void> close_fd(Handle fd);

/// A second name for the same open thing. koru has no `DUP` opcode and needs
/// none: one handle behind both names is exactly Braam's "a file's offset is
/// shared and closing one shuts nothing".
task<result<Handle>> dup_fd(Handle fd);

/// The read/write position, in `lseek(2)`'s three forms, reporting where it
/// landed. `Unsupported` on anything that is not a file.
task<result<u64>> seek_fd(Handle fd, i64 off, u32 whence);

/// The file's length, set. `Perm` unless the open asked for [`O_WRITE`], and
/// what `seek_fd` refuses this refuses.
task<result<void>> truncate_fd(Handle fd, u64 n);

// ---------------------------------------------------------------------------
// Metadata
// ---------------------------------------------------------------------------

/// `follow` false reports a symbolic link itself rather than its target.
task<result<FileInfo>> stat_of(Str path, bool follow);
task<result<FileInfo>> stat_fd(Handle fd);

// ---------------------------------------------------------------------------
// Directories and names
// ---------------------------------------------------------------------------

/// Every entry but `.` and `..`, with a stat apiece.
task<result<std::vector<DirEntry>>> list_dir(Str path);

task<result<void>> make_dir(Str path);

/// Every missing component of `path`. An existing directory is no error;
/// anything else in the leaf's place is `Exists`.
task<result<void>> make_dir_all(Str path);

/// Removes the file `path` names, or with `all` the whole tree under it.
task<result<void>> remove_path(Str path, bool all);

/// Moves an existing file's mtime to now, leaving its atime alone.
task<result<void>> touch_path(Str path);

/// Creates `path` as a symbolic link to `target`, kept as written.
task<result<void>> make_link(Str target, Str path);

/// The target of a symbolic link, unresolved. `Invalid` for anything else.
task<result<String>> read_link(Str path);

/// Renames `from` to `to`, following neither and replacing the destination.
task<result<void>> rename_path(Str from, Str to);

// ---------------------------------------------------------------------------
// Copying
// ---------------------------------------------------------------------------

/// One file's bytes into another, which is created or truncated.
task<result<void>> copy_file(Str from, Str to);

/// A whole tree. `to` may already be a directory and the two merge.
task<result<void>> copy_tree(Str from, Str to);

// ---------------------------------------------------------------------------
// The process
// ---------------------------------------------------------------------------

/// This process's own working directory. Ordinary POSIX calls: koru has no
/// opcode for it and a per-process property is not a per-ring one.
task<result<String>> cwd_get();
task<result<String>> cwd_set(Str path);

/// Parks for `ms`. `DELAY_NS` caps at one hour, so a longer sleep is chunked.
task<result<void>> sleep_for(u32 ms);

/// The wall clock. `tz_min` is read out of the host's zone file.
task<result<Clock>> clock_now();

// ---------------------------------------------------------------------------
// What the layers above this one use
// ---------------------------------------------------------------------------

namespace detail {

/// The same as `write_all` for bytes a `Str` could not hold: a buffered
/// stream's block, and the halves of a rune a `put` split across two calls.
task<result<void>> write_bytes(Handle fd, Span<u8> s);

/// One `WRITE`, waiting out an `EAGAIN`. A short write is a result, not an
/// error: the position advances by what it took.
task<result<size_t>> write_once(Handle fd, Span<u8> s);

/// One read appended to `out`, returning the count. Zero is end of input,
/// which is `READ`'s own `res == 0` and the one thing callers map differently.
task<result<size_t>> read_into(Handle fd, String &out, u32 max);

/// Whether `fd` is the console: the screen's byte channel, or a character
/// device. `Buffering::Auto` asks.
task<bool> is_console(Handle fd);

/// A slot, waiting for one if the pool is out.
task<BufSlot> acquire();

/// Braam's `error_name`, not the OS message: T49 compares the two bindings'
/// output byte for byte and only the name is shared.
String diag_line(Str who, Str what, Error why);

String join(Str dir, Str name);
FileKind kind_of_mode(u64 mode);
u64 ms_of(i64 sec, u64 nsec);
result<std::pair<u32, u64>> open_flags(u32 flags);
void parse_dirents(Span<u8> buf, std::vector<std::pair<String, u8>> &out);

} // namespace detail

} // namespace koru

#endif // KORU_OPS_HPP
