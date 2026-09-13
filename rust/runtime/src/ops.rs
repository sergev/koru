// SPDX-License-Identifier: MIT

//! Braam's operation layer, its signatures unchanged. Each one is a slot
//! acquisition, a submit, an await and a copy out.
//!
//! Three things are koru's rather than Braam's, and doc/Notes.md says why:
//! `seek_fd` is pure userspace bookkeeping, `dup_fd` is a reference count on
//! one handle, and `truncate_fd` goes by the path the handle was opened with.

use crate::future::Handle;
use crate::rt;
use crate::tz;
use crate::vocab::{Error, Kind, Result, Str};
use koru_sys::BufSlot;
use koru_sys::abi::{
    KORU_DT_DIR, KORU_DT_LNK, KORU_DT_MASK, KORU_MKDIR_MODE_ALL, KORU_O_APPEND, KORU_O_CREAT,
    KORU_O_DIRECTORY, KORU_O_EXCL, KORU_O_RDONLY, KORU_O_RDWR, KORU_O_TRUNC, KORU_O_WRONLY,
    KORU_OP_MKDIR, KORU_OP_READLINK, KORU_OP_RENAME, KORU_OP_RMDIR, KORU_OP_STATX_AT,
    KORU_OP_SYMLINK, KORU_OP_TRUNCATE, KORU_OP_UNLINK, KORU_OP_UTIMES, KORU_POLL_IN, KORU_POLL_OUT,
    KORU_S_IFDIR, KORU_S_IFLNK, KORU_S_IFMT, KORU_UTIME_NOW, KORU_UTIME_OMIT, KoruDirent, KoruStat,
    KoruTimes,
};
use koru_sys::error::{EAGAIN, EEXIST, EINVAL, EOPNOTSUPP, EPERM};
use koru_sys::ring::arg_offset;
use std::future::poll_fn;
use std::task::Poll;

// ---------------------------------------------------------------------------
// Braam's constants
// ---------------------------------------------------------------------------

// Open flags, Braam's own bit values rather than koru's access-mode word, so a
// Braam call site compiles unchanged. `open_at` translates.

pub const O_READ: u32 = 1;
pub const O_WRITE: u32 = 2;
pub const O_CREATE: u32 = 4;
pub const O_TRUNC: u32 = 8;
pub const O_APPEND: u32 = 16;
/// With [`O_CREATE`]; an existing name is `Err(Exists)`.
pub const O_EXCL: u32 = 32;

/// A bit outside this is `Err(Invalid)`, so a flag koru does not know is
/// refused rather than dropped.
pub const O_ALL: u32 = O_READ | O_WRITE | O_CREATE | O_TRUNC | O_APPEND | O_EXCL;

/// What a created file gets, before the umask. Braam's filesystem has no modes
/// and so no argument for one; this is `creat(2)`'s.
pub const CREATE_MODE: u64 = 0o666;

pub const SEEK_SET: u32 = 0;
pub const SEEK_CUR: u32 = 1;
pub const SEEK_END: u32 = 2;

/// The largest position there is: the wire's offset is a signed 64-bit.
pub const SEEK_MAX: u64 = (1 << 63) - 1;

/// What one read yields when the caller names no length, and the ceiling on
/// one that does. Braam's numbers.
pub const CHUNK: u32 = 512;
pub const READ_MAX: u32 = 65536 - 4;

/// How long a `DELAY_NS` may be. `sleep_for` chunks anything longer.
const MAX_DELAY_NS: u64 = 3_600_000_000_000;

// ---------------------------------------------------------------------------
// Braam's records
// ---------------------------------------------------------------------------

/// What a listing says about an entry, never resolving a link. koru reports
/// more kinds than Braam's three, and everything that is neither a directory
/// nor a symlink reports as a file.
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum FileKind {
    File,
    Dir,
    Link,
}

/// `mtime` is milliseconds since the epoch, 0 where none was reported.
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub struct FileInfo {
    pub kind: FileKind,
    pub size: u64,
    pub mtime: u64,
}

#[derive(Clone, PartialEq, Eq, Debug)]
pub struct DirEntry {
    pub name: String,
    pub kind: FileKind,
    pub size: u64,
    pub mtime: u64,
}

/// The wall clock: milliseconds since the epoch, and the local offset from UTC.
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub struct Clock {
    pub epoch_ms: u64,
    pub tz_min: i32,
}

// ---------------------------------------------------------------------------
// Streams
// ---------------------------------------------------------------------------

/// Writes all of `s`, retrying a short write.
///
/// The offset is userspace's own bookkeeping, because `WRITE` never touches
/// `f_pos`: a seekable handle advances, a stream stays at 0.
pub async fn write_all(fd: Handle, s: Str<'_>) -> Result<()> {
    let rt = rt::current();
    let mut left = s.as_bytes();

    // The kernel refuses a zero-length write, and Braam's write_all of an
    // empty string is a no-op rather than an error.
    while !left.is_empty() {
        let mut slot = acquire().await;
        let n = left.len().min(slot.len());
        slot[..n].copy_from_slice(&left[..n]);

        let off = rt::position(fd);
        let (res, back) = rt.write(fd, slot, off, n as u32).await;
        drop(back);

        match res {
            Ok(0) => return Err(Error::closed()),
            Ok(wrote) => {
                rt::advance(fd, wrote as u64);
                left = &left[wrote..];
            }
            Err(e) if e.raw() == EAGAIN => ready(fd, KORU_POLL_OUT).await?,
            Err(e) => return Err(e),
        }
    }
    Ok(())
}

/// One chunk, or `Err(Closed)` at end of input.
pub async fn read_chunk(fd: Handle) -> Result<String> {
    read_some(fd, READ_MAX).await
}

/// At most `max` bytes, clamped to [`READ_MAX`]; 0 means [`CHUNK`]. What is
/// left stays on the descriptor, so the next read serves it first.
///
/// Rust's `String` enforces the UTF-8 Braam's only declares, so bytes that are
/// not UTF-8 are `Err(Invalid)` here. `copy_file` moves bytes through the arena
/// and is unaffected.
pub async fn read_some(fd: Handle, max: u32) -> Result<String> {
    let mut buf = Vec::new();
    if read_into(fd, &mut buf, max).await? == 0 {
        return Err(Error::closed()); // end of input
    }
    String::from_utf8(buf).map_err(|_| Error::from_errno(EINVAL))
}

/// One read into `out`, returning the count. Zero is end of input, which is
/// `READ`'s own `res == 0` and the one thing callers map differently.
async fn read_into(fd: Handle, out: &mut Vec<u8>, max: u32) -> Result<usize> {
    let rt = rt::current();
    let want = match max {
        0 => CHUNK,
        n => n.min(READ_MAX),
    };
    loop {
        let slot = acquire().await;
        let n = want.min(slot.len() as u32);
        let off = rt::position(fd);
        let (res, slot) = rt.read(fd, slot, off, n).await;
        match res {
            Ok(got) => {
                out.extend_from_slice(&slot[..got]);
                rt::advance(fd, got as u64);
                return Ok(got);
            }
            Err(e) if e.raw() == EAGAIN => {
                drop(slot);
                ready(fd, KORU_POLL_IN).await?;
            }
            Err(e) => return Err(e),
        }
    }
}

/// A whole file, read through the ring.
pub async fn read_file(path: Str<'_>) -> Result<String> {
    let fd = open_read(path).await?;
    let mut buf = Vec::new();
    let mut bad = Ok(());
    loop {
        match read_into(fd, &mut buf, READ_MAX).await {
            Ok(0) => break,
            Ok(_) => {}
            Err(e) => {
                bad = Err(e);
                break;
            }
        }
    }
    close_fd(fd).await;
    bad?;
    String::from_utf8(buf).map_err(|_| Error::from_errno(EINVAL))
}

/// A diagnostic on stderr: "who: what: why".
pub async fn errln(who: Str<'_>, what: Str<'_>, why: Error) {
    let _ = write_all(rt::stderr(), &diag_line(who, what, why)).await;
}

/// Braam's `error_name`, not `Display`: the OS message says more, but T49
/// compares the two bindings' output byte for byte and only the name is shared.
fn diag_line(who: Str<'_>, what: Str<'_>, why: Error) -> String {
    let why = why.kind().name();
    if what.is_empty() {
        format!("{who}: {why}\n")
    } else {
        format!("{who}: {what}: {why}\n")
    }
}

// ---------------------------------------------------------------------------
// Descriptors
// ---------------------------------------------------------------------------

/// Opens `path` with the `O_*` flags above.
pub async fn open_at(path: Str<'_>, flags: u32) -> Result<Handle> {
    let rt = rt::current();
    let (koru_flags, mode) = open_flags(flags)?;
    let slot = acquire().await;
    let (res, back) = rt.open(slot, path, koru_flags, mode).await;
    drop(back);
    let h = res?;

    // Only the creator knows whether an offset means anything on it, and this
    // is the one place a handle learns its own path.
    let info = stat_fd(h).await;
    let seekable = info.is_ok_and(|i| i.kind == FileKind::File);
    rt::register_opened(h, seekable, flags & O_WRITE != 0, Some(path.to_string()));
    Ok(h)
}

pub async fn open_read(path: Str<'_>) -> Result<Handle> {
    open_at(path, O_READ).await
}

/// Retires the handle. Braam's returns nothing: there is no answer a program
/// could act on, and the kernel frees the file either way. A handle `dup_fd`
/// named twice closes on the second call, not the first.
pub async fn close_fd(fd: Handle) {
    if rt::release_handle(fd) {
        let _ = rt::current().close(fd).await;
    }
}

/// A second name for the same open thing. koru has no `DUP` opcode and needs
/// none: one handle behind both names is exactly Braam's "a file's offset is
/// shared and closing one shuts nothing".
pub async fn dup_fd(fd: Handle) -> Result<Handle> {
    if rt::retain_handle(fd) {
        Ok(fd)
    } else {
        Err(Error::from_errno(EINVAL))
    }
}

/// The read/write position, in `lseek(2)`'s three forms, reporting where it
/// landed. `Err(Unsupported)` on anything that is not a file.
///
/// **This is the one place koru's design makes a POSIX concept unnecessary
/// rather than merely different.** `READ` and `WRITE` carry an explicit file
/// offset and never touch `f_pos`, so there is nothing in the kernel to move:
/// a seek is an assignment to a number this process owns.
pub async fn seek_fd(fd: Handle, off: i64, whence: u32) -> Result<u64> {
    if !rt::is_seekable(fd) {
        return Err(Error::from_errno(EOPNOTSUPP));
    }
    let base = match whence {
        SEEK_SET => 0,
        SEEK_CUR => rt::position(fd),
        SEEK_END => stat_fd(fd).await?.size,
        _ => return Err(Error::from_errno(EINVAL)),
    };
    // Negated in u64: -i64::MIN does not fit an i64.
    let at = if off < 0 {
        let back = (-(off + 1)) as u64 + 1;
        base.checked_sub(back)
    } else {
        (off as u64 <= SEEK_MAX - base).then(|| base + off as u64)
    };
    let at = at.ok_or_else(|| Error::from_errno(EINVAL))?;
    rt::seek_to(fd, at);
    Ok(at)
}

/// The file's length, set. `Err(Perm)` unless the open asked for [`O_WRITE`],
/// and what `seek_fd` refuses this refuses.
///
/// koru truncates by **path**, so this needs the one `open_at` recorded: a
/// handle opened by nobody, or a file renamed since, is the difference from
/// `ftruncate(2)`. doc/Notes.md has it.
pub async fn truncate_fd(fd: Handle, n: u64) -> Result<()> {
    if !rt::is_seekable(fd) {
        return Err(Error::from_errno(EOPNOTSUPP));
    }
    if !rt::is_writable(fd) {
        return Err(Error::from_errno(EPERM));
    }
    let path = rt::handle_path(fd).ok_or_else(|| Error::from_errno(EOPNOTSUPP))?;
    truncate_path(&path, n).await
}

async fn truncate_path(path: Str<'_>, n: u64) -> Result<()> {
    path_op(KORU_OP_TRUNCATE, path, &n.to_ne_bytes(), 0)
        .await
        .map(|_| ())
}

// ---------------------------------------------------------------------------
// Metadata
// ---------------------------------------------------------------------------

/// `follow` false reports a symbolic link itself rather than its target.
///
/// koru has no `lstat`: `STATX_AT` always follows. A successful `READLINK` is
/// what says "this is a symlink", and its length is the size `lstat(2)` would
/// report — so only a link's own mtime is lost, and it reads as 0.
pub async fn stat_of(path: Str<'_>, follow: bool) -> Result<FileInfo> {
    if !follow {
        match read_link(path).await {
            Ok(target) => {
                return Ok(FileInfo {
                    kind: FileKind::Link,
                    size: target.len() as u64,
                    mtime: 0,
                });
            }
            // Not a symlink, so following it changes nothing.
            Err(e) if e.raw() == EINVAL => {}
            Err(e) => return Err(e),
        }
    }
    let mut buf = [0u8; size_of::<KoruStat>()];
    stat_at(path, &mut buf).await?;
    Ok(info_of(&buf))
}

/// The same, off an open descriptor: the size is the one being read.
pub async fn stat_fd(fd: Handle) -> Result<FileInfo> {
    let rt = rt::current();
    let slot = acquire().await;
    let len = size_of::<KoruStat>() as u32;
    let (res, slot) = rt.stat(fd, slot, 0, len).await;
    let (n, _) = res?;
    let out = info_of(&slot[..n]);
    drop(slot);
    Ok(out)
}

/// `STATX_AT` into `out`. The struct replaces the path it was given, so the
/// slot must hold both and the read-back is from offset 0.
async fn stat_at(path: Str<'_>, out: &mut [u8]) -> Result<()> {
    let rt = rt::current();
    let slot = acquire().await;
    if path.len() > slot.len() || size_of::<KoruStat>() > slot.len() {
        return Err(Error::from_errno(EINVAL));
    }
    let (res, slot) = {
        let mut slot = slot;
        slot[..path.len()].copy_from_slice(path.as_bytes());
        rt.path_op(KORU_OP_STATX_AT, slot, 0, path.len() as u32, 0)
            .await
    };
    let (n, _) = res?;
    let n = n.min(out.len());
    out[..n].copy_from_slice(&slot[..n]);
    drop(slot);
    Ok(())
}

fn info_of(bytes: &[u8]) -> FileInfo {
    let s = KoruStat::read_from(bytes).unwrap_or_default();
    FileInfo {
        kind: kind_of_mode(s.mode),
        size: s.size,
        mtime: ms_of(s.mtime_sec, s.mtime_nsec),
    }
}

fn kind_of_mode(mode: u64) -> FileKind {
    match mode & KORU_S_IFMT {
        KORU_S_IFDIR => FileKind::Dir,
        KORU_S_IFLNK => FileKind::Link,
        _ => FileKind::File,
    }
}

/// Milliseconds since the epoch, 0 for anything before it: Braam's `mtime` is
/// unsigned and its filesystems keep none.
fn ms_of(sec: i64, nsec: u64) -> u64 {
    if sec < 0 {
        return 0;
    }
    (sec as u64) * 1000 + nsec / 1_000_000
}

// ---------------------------------------------------------------------------
// Directories
// ---------------------------------------------------------------------------

/// Every entry but `.` and `..`, with a stat apiece: `READDIR` reports a name
/// and a type, and Braam's `DirEntry` also carries a size and an mtime.
///
/// A listing never resolves a link, so the type comes from the record — which
/// is the directory's own view — and a link's size is its target's length.
pub async fn list_dir(path: Str<'_>) -> Result<Vec<DirEntry>> {
    let rt = rt::current();
    let fd = open_at_koru(path, KORU_O_RDONLY | KORU_O_DIRECTORY).await?;
    let mut names: Vec<(String, u8)> = Vec::new();
    let mut cookie = 0u64;
    let mut bad = Ok(());

    loop {
        let slot = acquire().await;
        let len = slot.len() as u32;
        let (res, slot) = rt.readdir(fd, slot, cookie, len).await;
        match res {
            Ok((0, _)) => {
                drop(slot);
                break;
            }
            Ok((n, next)) => {
                parse_dirents(&slot[..n], &mut names);
                drop(slot);
                cookie = next;
            }
            Err(e) => {
                drop(slot);
                bad = Err(e);
                break;
            }
        }
    }
    let _ = rt.close(fd).await;
    bad?;

    let mut out = Vec::with_capacity(names.len());
    for (name, dtype) in names {
        let full = join(path, &name);
        out.push(entry_of(name, dtype, &full).await);
    }
    Ok(out)
}

/// One record header, its name, and on to the next. A malformed run stops the
/// walk rather than guessing: the kernel emits whole records or none.
fn parse_dirents(buf: &[u8], out: &mut Vec<(String, u8)>) {
    let mut at = 0usize;
    while let Some(d) = KoruDirent::read_from(&buf[at..]) {
        let head = size_of::<KoruDirent>();
        let (reclen, namelen) = (d.reclen as usize, d.namelen as usize);
        if reclen < head + namelen || at + reclen > buf.len() {
            return;
        }
        let name = &buf[at + head..at + head + namelen];
        if name != b"." && name != b".." {
            if let Ok(s) = std::str::from_utf8(name) {
                out.push((s.to_string(), d.dtype & KORU_DT_MASK));
            }
        }
        at += reclen;
    }
}

/// A vanished entry keeps its name and its type and reports no size: a listing
/// is a snapshot, and racing `rm` is not the caller's error.
async fn entry_of(name: String, dtype: u8, full: Str<'_>) -> DirEntry {
    let kind = match dtype {
        KORU_DT_DIR => FileKind::Dir,
        KORU_DT_LNK => FileKind::Link,
        _ => FileKind::File,
    };
    let info = stat_of(full, kind != FileKind::Link).await;
    DirEntry {
        name,
        kind,
        size: info.map_or(0, |i| i.size),
        mtime: info.map_or(0, |i| i.mtime),
    }
}

pub async fn make_dir(path: Str<'_>) -> Result<()> {
    path_op(KORU_OP_MKDIR, path, &[], KORU_MKDIR_MODE_ALL)
        .await
        .map(|_| ())
}

/// Every missing component of `path`. An existing directory is no error;
/// anything else in the leaf's place is `Err(Exists)`.
pub async fn make_dir_all(path: Str<'_>) -> Result<()> {
    let mut so_far = String::new();
    if path.starts_with('/') {
        so_far.push('/');
    }
    let mut stood = false; // the last component was already there

    for name in path.split('/') {
        if name.is_empty() || name == "." {
            continue;
        }
        if !so_far.is_empty() && !so_far.ends_with('/') {
            so_far.push('/');
        }
        so_far.push_str(name);
        match make_dir(&so_far).await {
            Ok(()) => stood = false,
            Err(e) if e.is(Kind::Exists) => stood = true,
            Err(e) => return Err(e),
        }
    }

    // Only the leaf is told apart; a file lower down fails the component below.
    if stood && stat_of(&so_far, false).await?.kind != FileKind::Dir {
        return Err(Error::from_errno(EEXIST));
    }
    Ok(())
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

/// Removes the file `path` names, or with `all` the whole tree under it.
pub async fn remove_path(path: Str<'_>, all: bool) -> Result<()> {
    match path_op(KORU_OP_UNLINK, path, &[], 0).await {
        Ok(_) => return Ok(()),
        Err(e) if e.is(Kind::IsDir) => {}
        Err(e) => return Err(e),
    }
    if all {
        // Depth first, over an explicit stack: a deep tree must not be a deep
        // chain of coroutine frames.
        let mut stack = vec![(path.to_string(), false)];
        while let Some((dir, listed)) = stack.pop() {
            if listed {
                path_op(KORU_OP_RMDIR, &dir, &[], 0).await?;
                continue;
            }
            stack.push((dir.clone(), true));
            for e in list_dir(&dir).await? {
                let full = join(&dir, &e.name);
                if e.kind == FileKind::Dir {
                    stack.push((full, false));
                } else {
                    path_op(KORU_OP_UNLINK, &full, &[], 0).await?;
                }
            }
        }
        return Ok(());
    }
    path_op(KORU_OP_RMDIR, path, &[], 0).await.map(|_| ())
}

/// Moves an existing file's mtime to now, leaving its atime alone.
pub async fn touch_path(path: Str<'_>) -> Result<()> {
    let t = KoruTimes {
        atime_sec: 0,
        atime_nsec: KORU_UTIME_OMIT,
        mtime_sec: 0,
        mtime_nsec: KORU_UTIME_NOW,
    };
    path_op(KORU_OP_UTIMES, path, &t.as_bytes(), 0)
        .await
        .map(|_| ())
}

/// Creates `path` as a symbolic link to `target`. The target is kept as
/// written and not checked, so a link may point at nothing.
pub async fn make_link(target: Str<'_>, path: Str<'_>) -> Result<()> {
    pair_op(KORU_OP_SYMLINK, target, path).await
}

/// The target of a symbolic link, unresolved. `Err(Invalid)` for anything else.
pub async fn read_link(path: Str<'_>) -> Result<String> {
    let rt = rt::current();
    let slot = acquire().await;
    if path.is_empty() || path.len() > slot.len() {
        return Err(Error::from_errno(EINVAL));
    }
    // The target replaces the path it was given, at the same offset.
    let (res, slot) = {
        let mut slot = slot;
        slot[..path.len()].copy_from_slice(path.as_bytes());
        rt.path_op(KORU_OP_READLINK, slot, 0, path.len() as u32, 0)
            .await
    };
    let (n, _) = res?;
    let out = std::str::from_utf8(&slot[..n])
        .map(str::to_string)
        .map_err(|_| Error::from_errno(EINVAL));
    drop(slot);
    out
}

/// Renames `from` to `to`, following neither and replacing the destination.
/// `Err(Unsupported)` is not a failure but an instruction: the store cannot
/// move this, so copy and remove instead.
pub async fn rename_path(from: Str<'_>, to: Str<'_>) -> Result<()> {
    pair_op(KORU_OP_RENAME, from, to).await
}

// ---------------------------------------------------------------------------
// Copying
// ---------------------------------------------------------------------------

/// One file's bytes into another, which is created or truncated. Byte for
/// byte, through the arena: nothing here is text.
pub async fn copy_file(from: Str<'_>, to: Str<'_>) -> Result<()> {
    let rt = rt::current();
    let src = open_read(from).await?;
    let dst = match open_at(to, O_WRITE | O_CREATE | O_TRUNC).await {
        Ok(h) => h,
        Err(e) => {
            close_fd(src).await;
            return Err(e);
        }
    };

    let mut at = 0u64;
    let mut bad = Ok(());
    loop {
        let slot = acquire().await;
        let len = slot.len() as u32;
        let (res, slot) = rt.read(src, slot, at, len).await;
        let n = match res {
            Ok(0) => {
                drop(slot);
                break;
            }
            Ok(n) => n,
            Err(e) => {
                drop(slot);
                bad = Err(e);
                break;
            }
        };
        // The same slot back out, so the bytes are never copied twice.
        let mut left = 0usize;
        let mut slot = slot;
        while left < n {
            let (res, back) = rt
                .write(dst, slot, at + left as u64, (n - left) as u32)
                .await;
            slot = back;
            match res {
                Ok(0) => {
                    bad = Err(Error::closed());
                    break;
                }
                Ok(w) => {
                    // A short write leaves the rest at the front of the slot.
                    left += w;
                    if left < n {
                        slot.copy_within(w..n, 0);
                    }
                }
                Err(e) => {
                    bad = Err(e);
                    break;
                }
            }
        }
        drop(slot);
        if bad.is_err() {
            break;
        }
        at += n as u64;
    }

    close_fd(src).await;
    close_fd(dst).await;
    bad
}

/// A whole tree. `to` may already be a directory and the two merge; anything
/// else in its place is `Err(Exists)`. Pre-order, over an explicit stack.
pub async fn copy_tree(from: Str<'_>, to: Str<'_>) -> Result<()> {
    dir_over(to).await?;

    let root = from.trim_end_matches('/');
    let root = if root.is_empty() { "/" } else { root };
    let mut stack = vec![String::new()]; // relative to `root`, "" being it

    while let Some(rel) = stack.pop() {
        let src = if rel.is_empty() {
            root.to_string()
        } else {
            join(root, &rel)
        };
        for e in list_dir(&src).await? {
            let under = if rel.is_empty() {
                e.name.clone()
            } else {
                join(&rel, &e.name)
            };
            let dst = join(to, &under);
            match e.kind {
                FileKind::Dir => {
                    dir_over(&dst).await?;
                    stack.push(under);
                }
                FileKind::Link => {
                    let target = read_link(&join(&src, &e.name)).await?;
                    link_over(&target, &dst).await?;
                }
                FileKind::File => copy_file(&join(&src, &e.name), &dst).await?,
            }
        }
    }
    Ok(())
}

/// A directory where one may already be.
async fn dir_over(path: Str<'_>) -> Result<()> {
    match make_dir(path).await {
        Err(e) if e.is(Kind::Exists) => {}
        r => return r,
    }
    if stat_of(path, false).await?.kind != FileKind::Dir {
        return Err(Error::from_errno(EEXIST));
    }
    Ok(())
}

/// A link over a name that is taken: the old name goes, whatever it was.
async fn link_over(target: Str<'_>, path: Str<'_>) -> Result<()> {
    match make_link(target, path).await {
        Err(e) if e.is(Kind::Exists) => {}
        r => return r,
    }
    remove_path(path, true).await?;
    make_link(target, path).await
}

// ---------------------------------------------------------------------------
// The process
// ---------------------------------------------------------------------------

/// This process's own working directory, which every relative path above
/// resolves against.
///
/// Ordinary POSIX calls: koru has no opcode for the working directory, and a
/// per-process property is not a per-ring one. The plan says so.
pub async fn cwd_get() -> Result<String> {
    let d = std::env::current_dir()?;
    d.into_os_string()
        .into_string()
        .map_err(|_| Error::from_errno(EINVAL))
}

pub async fn cwd_set(path: Str<'_>) -> Result<String> {
    std::env::set_current_dir(path)?;
    cwd_get().await
}

/// Parks for `ms`. `DELAY_NS` caps at one hour, so a longer sleep is chunked.
pub async fn sleep_for(ms: u32) -> Result<()> {
    let rt = rt::current();
    let mut left = u64::from(ms) * 1_000_000;
    while left > 0 {
        let step = left.min(MAX_DELAY_NS);
        rt.delay(step).await?;
        left -= step;
    }
    Ok(())
}

/// The wall clock. `tz_min` is read out of the host's zone file; it is 0 when
/// that cannot be read, which is what UTC looks like anyway.
pub async fn clock_now() -> Result<Clock> {
    let since = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_err(|_| Error::from_errno(EINVAL))?;
    Ok(Clock {
        epoch_ms: since.as_millis() as u64,
        tz_min: tz::local_offset_min(since.as_secs() as i64),
    })
}

// ---------------------------------------------------------------------------
// The plumbing
// ---------------------------------------------------------------------------

/// A slot, waiting for one if the pool is out. Yielding is enough because a
/// slot comes back when some other task's op completes, which is progress the
/// executor makes on its own.
async fn acquire() -> BufSlot {
    let rt = rt::current();
    poll_fn(move |cx| match rt.acquire() {
        Some(slot) => Poll::Ready(slot),
        None => {
            cx.waker().wake_by_ref();
            Poll::Pending
        }
    })
    .await
}

/// Wait for a non-blocking handle to be usable. A regular file is on no
/// waitqueue and completes at once, which is why the caller retries rather
/// than trusting the mask.
async fn ready(fd: Handle, events: u32) -> Result<()> {
    rt::current().poll_add(fd, events).await.map(|_| ())
}

/// One path op: the path at slot offset 0, `arg` after it where every path op's
/// argument goes. `handle` is zero on all but `MKDIR`.
async fn path_op(op: u8, path: Str<'_>, arg: &[u8], handle: u32) -> Result<(usize, u64)> {
    let rt = rt::current();
    let slot = acquire().await;
    let at = arg_offset(0, path.len() as u32) as usize;
    if path.is_empty() || at + arg.len() > slot.len() {
        return Err(Error::from_errno(EINVAL));
    }
    let (res, back) = {
        let mut slot = slot;
        slot[..path.len()].copy_from_slice(path.as_bytes());
        slot[at..at + arg.len()].copy_from_slice(arg);
        rt.path_op(op, slot, 0, path.len() as u32, handle).await
    };
    drop(back);
    res
}

/// A two-path op: both paths back to back with one NUL between them.
async fn pair_op(op: u8, a: Str<'_>, b: Str<'_>) -> Result<()> {
    let rt = rt::current();
    let slot = acquire().await;
    let n = a.len() + 1 + b.len();
    if a.is_empty() || b.is_empty() || n > slot.len() {
        return Err(Error::from_errno(EINVAL));
    }
    let (res, back) = {
        let mut slot = slot;
        slot[..a.len()].copy_from_slice(a.as_bytes());
        slot[a.len()] = 0;
        slot[a.len() + 1..n].copy_from_slice(b.as_bytes());
        rt.path_op(op, slot, 0, n as u32, 0).await
    };
    drop(back);
    res.map(|_| ())
}

/// `open_at` without the flag translation, for the places that want koru's own.
async fn open_at_koru(path: Str<'_>, flags: u32) -> Result<Handle> {
    let rt = rt::current();
    let slot = acquire().await;
    let (res, back) = rt.open(slot, path, flags, 0).await;
    drop(back);
    res
}

/// Braam's open flags to koru's. The access mode is a pair of bits there and a
/// two-bit field here, which is the one place the two disagree.
fn open_flags(flags: u32) -> Result<(u32, u64)> {
    if flags & !O_ALL != 0 {
        return Err(Error::from_errno(EINVAL));
    }
    let mut out = match flags & (O_READ | O_WRITE) {
        x if x == O_READ | O_WRITE => KORU_O_RDWR,
        O_WRITE => KORU_O_WRONLY,
        _ => KORU_O_RDONLY,
    };
    if flags & O_CREATE != 0 {
        out |= KORU_O_CREAT;
    }
    if flags & O_TRUNC != 0 {
        out |= KORU_O_TRUNC;
    }
    if flags & O_APPEND != 0 {
        out |= KORU_O_APPEND;
    }
    if flags & O_EXCL != 0 {
        // koru refuses O_EXCL alone; Braam's is meaningless without O_CREATE.
        if flags & O_CREATE == 0 {
            return Err(Error::from_errno(EINVAL));
        }
        out |= KORU_O_EXCL;
    }
    Ok((out, CREATE_MODE))
}

fn join(dir: Str<'_>, name: Str<'_>) -> String {
    if dir.ends_with('/') {
        format!("{dir}{name}")
    } else {
        format!("{dir}/{name}")
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use koru_sys::error::ENOENT;

    #[test]
    fn braams_open_flags_become_korus() {
        assert_eq!(open_flags(O_READ).unwrap().0, KORU_O_RDONLY);
        assert_eq!(open_flags(0).unwrap().0, KORU_O_RDONLY);
        assert_eq!(open_flags(O_WRITE).unwrap().0, KORU_O_WRONLY);
        assert_eq!(open_flags(O_READ | O_WRITE).unwrap().0, KORU_O_RDWR);
        assert_eq!(
            open_flags(O_WRITE | O_CREATE | O_TRUNC).unwrap().0,
            KORU_O_WRONLY | KORU_O_CREAT | KORU_O_TRUNC
        );
        assert_eq!(
            open_flags(O_WRITE | O_APPEND).unwrap().0,
            KORU_O_WRONLY | KORU_O_APPEND
        );
        // A bit koru does not know, and one that cannot act.
        assert!(open_flags(1 << 31).is_err());
        assert!(open_flags(O_WRITE | O_EXCL).is_err());
        assert!(open_flags(O_WRITE | O_CREATE | O_EXCL).is_ok());
    }

    /// Braam's own wording: "who: what: why", and no empty field.
    #[test]
    fn a_diagnostic_reads_who_what_why() {
        let e = Error::from_errno(ENOENT);
        assert_eq!(diag_line("wc", "a.txt", e), "wc: a.txt: not found\n");
        assert_eq!(diag_line("wc", "", e), "wc: not found\n");
    }

    #[test]
    fn a_path_joins_without_doubling_the_separator() {
        assert_eq!(join("/a", "b"), "/a/b");
        assert_eq!(join("/a/", "b"), "/a/b");
        assert_eq!(join("/", "b"), "/b");
    }

    #[test]
    fn an_mtime_is_milliseconds_and_never_negative() {
        assert_eq!(ms_of(1, 500_000_000), 1500);
        assert_eq!(ms_of(0, 0), 0);
        assert_eq!(ms_of(-1, 0), 0, "a date before the epoch reports none");
    }

    /// The three kinds Braam has, off koru's whole `S_IFMT`.
    #[test]
    fn every_file_type_lands_in_one_of_braams_three_kinds() {
        use koru_sys::abi::{KORU_S_IFCHR, KORU_S_IFIFO, KORU_S_IFREG, KORU_S_IFSOCK};
        assert_eq!(kind_of_mode(KORU_S_IFDIR | 0o755), FileKind::Dir);
        assert_eq!(kind_of_mode(KORU_S_IFLNK | 0o777), FileKind::Link);
        assert_eq!(kind_of_mode(KORU_S_IFREG | 0o644), FileKind::File);
        assert_eq!(kind_of_mode(KORU_S_IFIFO), FileKind::File);
        assert_eq!(kind_of_mode(KORU_S_IFCHR), FileKind::File);
        assert_eq!(kind_of_mode(KORU_S_IFSOCK), FileKind::File);
    }

    /// `.` and `..` never reach a caller, and a short record stops the walk.
    #[test]
    fn a_dirent_run_parses_to_its_names() {
        let mut buf = Vec::new();
        for (name, dtype) in [("a", KORU_DT_REG), (".", KORU_DT_DIR), ("bb", KORU_DT_DIR)] {
            let head = size_of::<KoruDirent>();
            let reclen = (head + name.len() + 1).next_multiple_of(8);
            let at = buf.len();
            buf.resize(at + reclen, 0);
            // The header by hand, as the kernel lays it out.
            buf[at..at + 8].copy_from_slice(&1u64.to_ne_bytes());
            buf[at + 8..at + 16].copy_from_slice(&1u64.to_ne_bytes());
            buf[at + 16..at + 18].copy_from_slice(&(reclen as u16).to_ne_bytes());
            buf[at + 18..at + 20].copy_from_slice(&(name.len() as u16).to_ne_bytes());
            buf[at + 20] = dtype;
            buf[at + head..at + head + name.len()].copy_from_slice(name.as_bytes());
        }
        let mut out = Vec::new();
        parse_dirents(&buf, &mut out);
        assert_eq!(
            out,
            vec![("a".into(), KORU_DT_REG), ("bb".into(), KORU_DT_DIR)]
        );

        // A truncated last record is dropped, not guessed at.
        let mut out = Vec::new();
        parse_dirents(&buf[..buf.len() - 8], &mut out);
        assert_eq!(out, vec![("a".into(), KORU_DT_REG)]);
    }

    use koru_sys::abi::KORU_DT_REG;
}
