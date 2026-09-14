// SPDX-License-Identifier: MIT

//! The ambient ring, the runtime entry and the at-exit hook.
//!
//! A Braam program never names an executor, so the ring is thread-local and
//! the free functions find it. Everything here that looks like a POSIX detour
//! — `/proc/self/fd`, the flags in `/proc/self/fdinfo` — is the runtime's
//! bounded preamble, and doc/Notes.md says why each one is there.

use crate::args::Args;
use crate::future::Handle;
use crate::vocab::{Kind, Result};
use crate::{Runtime, SetupConfig};
use std::cell::RefCell;
use std::collections::HashMap;
use std::future::Future;
use std::os::fd::AsRawFd;
use std::os::unix::fs::OpenOptionsExt;
use std::pin::Pin;

/// asm-generic values, like the errno table's. See the guard in `sys.rs`.
const O_ACCMODE: i32 = 0o3;
const O_WRONLY: i32 = 0o1;
const O_RDWR: i32 = 0o2;
const O_NONBLOCK: i32 = 0o4000;
const O_NOCTTY: i32 = 0o400;

/// A 64 KiB slot is Braam's own read budget, and eight of them is half a
/// megabyte of arena — enough that a program never waits for one.
pub fn default_config() -> SetupConfig {
    SetupConfig::new(64, 128, 64 * 1024, 8, 64)
}

thread_local! {
    static AMBIENT: RefCell<Option<Ambient>> = const { RefCell::new(None) };
    static HOOKS: RefCell<Vec<Hook>> = const { RefCell::new(Vec::new()) };
}

type Hook = Box<dyn FnOnce() -> Pin<Box<dyn Future<Output = ()>>>>;

struct Ambient {
    rt: Runtime,
    std: [Handle; 3],
    /// The screen's byte channel, where stdout is one. `Handle(0)` otherwise.
    screen: Handle,
    pos: HashMap<u32, Pos>,
}

/// Userspace's own bookkeeping: `READ` and `WRITE` carry an explicit offset
/// and never touch `f_pos`, so a stream's position lives here. `seek_fd`
/// exposes it.
///
/// `refs` is what makes `dup_fd` possible with no `DUP` opcode: both names are
/// the one handle, so the offset is shared and the `CLOSE` waits for the last
/// of them. `path` is what makes `truncate_fd` possible with only a path-named
/// `TRUNCATE`; see doc/Notes.md for what that costs.
#[derive(Clone)]
struct Pos {
    seekable: bool,
    off: u64,
    refs: u32,
    writable: bool,
    path: Option<String>,
}

// ---------------------------------------------------------------------------
// The ambient ring
// ---------------------------------------------------------------------------

/// Make `rt` this thread's ring and adopt the standard streams into it.
/// Replaces any previous one, which is what lets a test install its own.
pub fn install(rt: Runtime) {
    crate::file::reset_std();
    AMBIENT.with(|c| {
        *c.borrow_mut() = Some(Ambient {
            rt: rt.clone(),
            std: [Handle(0); 3],
            screen: Handle(0),
            pos: HashMap::new(),
        })
    });
    for fd in 0..3i32 {
        // stdout is the screen's byte channel where there is one to have, so
        // the terminal koru was started from is never adopted in its place.
        let screen = if fd == 1 {
            crate::screen::adopt_byte_channel(&rt)
        } else {
            None
        };
        let (h, seekable) = match screen {
            Some(h) => (h, false),
            None => adopt_std(&rt, fd),
        };
        if screen.is_some() {
            AMBIENT.with(|c| {
                if let Some(a) = c.borrow_mut().as_mut() {
                    a.screen = h;
                }
            });
        }
        if h.0 != 0 {
            // No path: nothing here was opened by name, so `truncate_fd`
            // refuses a standard stream as Braam's `seek_fd` refuses one.
            register_opened(h, seekable, fd != 0, None);
        }
        AMBIENT.with(|c| {
            if let Some(a) = c.borrow_mut().as_mut() {
                a.std[fd as usize] = h;
            }
        });
    }
}

/// This thread's ring. Cheap: a `Runtime` is a handle, not the ring.
pub fn current() -> Runtime {
    try_current().expect("no ambient ring: use #[koru::main], or koru::install")
}

pub fn try_current() -> Option<Runtime> {
    AMBIENT.with(|c| c.borrow().as_ref().map(|a| a.rt.clone()))
}

pub fn stdin() -> Handle {
    std_handle(0)
}

pub fn stdout() -> Handle {
    std_handle(1)
}

pub fn stderr() -> Handle {
    std_handle(2)
}

/// True where `h` is the screen's byte channel. It is a socket, so nothing
/// about the handle itself says it is a console; only this does.
pub(crate) fn is_screen(h: Handle) -> bool {
    h.0 != 0 && AMBIENT.with(|c| c.borrow().as_ref().is_some_and(|a| a.screen == h))
}

/// `Handle(0)` where the descriptor could not be adopted — never a valid
/// handle, so the kernel answers `EBADF` rather than this guessing.
fn std_handle(i: usize) -> Handle {
    AMBIENT.with(|c| c.borrow().as_ref().map_or(Handle(0), |a| a.std[i]))
}

/// Drive `fut` on the ambient ring. Spawned tasks run alongside it.
pub fn block_on<F: Future>(fut: F) -> F::Output {
    current().block_on(fut)
}

/// Braam's `proc_spawn`, without its eight-task ceiling: the program ends when
/// the root task returns, whatever the others are doing. The output is
/// dropped, so both `async {}` and Braam's `Task<i32>` shape fit.
pub fn spawn<F: Future + 'static>(fut: F) {
    current().spawn(async move {
        let _ = fut.await;
    });
}

/// Run `f` after the program's own future resolves, last registered first.
/// A destructor cannot await, so this is where a buffered writer flushes.
pub fn at_exit<F, Fut>(f: F)
where
    F: FnOnce() -> Fut + 'static,
    Fut: Future<Output = ()> + 'static,
{
    HOOKS.with(|h| h.borrow_mut().push(Box::new(move || Box::pin(f()))));
}

/// Run the hooks, drop the spawned tasks and clear the ring. A task still
/// waiting on an op is cancelled rather than waited for, which is Braam's
/// rule: the program ends when the root task returns.
pub fn shutdown() {
    while let Some(hook) = HOOKS.with(|h| h.borrow_mut().pop()) {
        if let Some(rt) = try_current() {
            rt.block_on(hook());
        }
    }
    if let Some(rt) = try_current() {
        rt.drop_tasks();
    }
    AMBIENT.with(|c| c.borrow_mut().take());
}

// ---------------------------------------------------------------------------
// Stream positions
// ---------------------------------------------------------------------------

/// Where the next write to `h` goes. An unregistered handle counts as a
/// stream: `off` 0, which is the only offset an unseekable file accepts.
pub(crate) fn position(h: Handle) -> u64 {
    AMBIENT.with(|c| {
        c.borrow()
            .as_ref()
            .and_then(|a| a.pos.get(&h.0))
            .filter(|p| p.seekable)
            .map_or(0, |p| p.off)
    })
}

pub(crate) fn advance(h: Handle, n: u64) {
    AMBIENT.with(|c| {
        if let Some(p) = c.borrow_mut().as_mut().and_then(|a| a.pos.get_mut(&h.0)) {
            p.off += n;
        }
    });
}

/// `seek_fd`'s half of the bookkeeping. Silent on an unregistered handle, as
/// `position` is: it reads back as the 0 an unseekable file demands.
pub(crate) fn seek_to(h: Handle, off: u64) {
    AMBIENT.with(|c| {
        if let Some(p) = c.borrow_mut().as_mut().and_then(|a| a.pos.get_mut(&h.0)) {
            p.off = off;
        }
    });
}

pub(crate) fn is_seekable(h: Handle) -> bool {
    with_pos(h, |p| p.seekable).unwrap_or(false)
}

pub(crate) fn is_writable(h: Handle) -> bool {
    with_pos(h, |p| p.writable).unwrap_or(false)
}

/// The path `open_at` named, for the operations koru has only by path.
pub(crate) fn handle_path(h: Handle) -> Option<String> {
    with_pos(h, |p| p.path.clone()).flatten()
}

fn with_pos<T>(h: Handle, f: impl Fn(&Pos) -> T) -> Option<T> {
    AMBIENT.with(|c| c.borrow().as_ref().and_then(|a| a.pos.get(&h.0)).map(f))
}

/// Called wherever a handle is created, because only its creator knows
/// whether an offset means anything on it: `open_at` does it for an opened
/// path, and `install` for the standard streams.
pub fn register_handle(h: Handle, seekable: bool) {
    register_opened(h, seekable, false, None);
}

pub(crate) fn register_opened(h: Handle, seekable: bool, writable: bool, path: Option<String>) {
    AMBIENT.with(|c| {
        if let Some(a) = c.borrow_mut().as_mut() {
            a.pos.insert(
                h.0,
                Pos {
                    seekable,
                    off: 0,
                    refs: 1,
                    writable,
                    path,
                },
            );
        }
    });
}

/// One more name for the same handle. False where there is no record, which is
/// every handle the runtime did not create.
pub(crate) fn retain_handle(h: Handle) -> bool {
    AMBIENT.with(
        |c| match c.borrow_mut().as_mut().and_then(|a| a.pos.get_mut(&h.0)) {
            Some(p) => {
                p.refs += 1;
                true
            }
            None => false,
        },
    )
}

/// Drops one name. True when the kernel handle should now be closed, which an
/// unrecorded handle also is: nothing else is holding it.
pub(crate) fn release_handle(h: Handle) -> bool {
    AMBIENT.with(|c| {
        let mut b = c.borrow_mut();
        let Some(a) = b.as_mut() else { return true };
        match a.pos.get_mut(&h.0) {
            Some(p) if p.refs > 1 => {
                p.refs -= 1;
                false
            }
            _ => {
                a.pos.remove(&h.0);
                true
            }
        }
    })
}

pub fn forget_handle(h: Handle) {
    AMBIENT.with(|c| {
        if let Some(a) = c.borrow_mut().as_mut() {
            a.pos.remove(&h.0);
        }
    });
}

// ---------------------------------------------------------------------------
// The standard streams
// ---------------------------------------------------------------------------

/// Adopt descriptor `fd`, re-opening it first where koru could not use it as
/// it stands: the kernel admits a non-regular file only when it was opened
/// non-blocking, and koru never sets that bit on a descriptor it did not open.
fn adopt_std(rt: &Runtime, fd: i32) -> (Handle, bool) {
    let seekable = std::fs::metadata(fd_path(fd)).is_ok_and(|m| m.is_file());
    let flags = fd_flags(fd);
    let reopened = match flags {
        Some(f) if !seekable && f & O_NONBLOCK == 0 => reopen(fd, f),
        _ => None,
    };
    let target = reopened.as_ref().map_or(fd, |f| f.as_raw_fd());
    // ADOPT_FD takes a reference of its own, so the re-opened descriptor is
    // closed on the way out of this function.
    let h = rt.block_on(rt.adopt(target)).unwrap_or(Handle(0));
    (h, seekable)
}

/// A file description of our own, so `O_NONBLOCK` never reaches the one the
/// parent shares. Fails on anything `/proc/self/fd` cannot re-open — the
/// virtio console a VM is run on is one — and then the raw descriptor is
/// adopted and a write to it is refused, which is honest.
fn reopen(fd: i32, flags: i32) -> Option<std::fs::File> {
    let mode = flags & O_ACCMODE;
    std::fs::OpenOptions::new()
        .read(mode != O_WRONLY)
        .write(mode == O_WRONLY || mode == O_RDWR)
        .custom_flags(O_NONBLOCK | O_NOCTTY)
        .open(fd_path(fd))
        .ok()
}

fn fd_path(fd: i32) -> String {
    format!("/proc/self/fd/{fd}")
}

/// The open flags, octal, out of `/proc/self/fdinfo`. `fcntl` would need
/// `unsafe`, and this crate forbids it.
fn fd_flags(fd: i32) -> Option<i32> {
    let text = std::fs::read_to_string(format!("/proc/self/fdinfo/{fd}")).ok()?;
    let line = text.lines().find_map(|l| l.strip_prefix("flags:"))?;
    i32::from_str_radix(line.trim(), 8).ok()
}

// ---------------------------------------------------------------------------
// The entry
// ---------------------------------------------------------------------------

/// What a program's `main` may return. Braam's exit status is an `i32` and
/// `Error::Cancelled` is `^C`, which is 130.
pub trait Exit {
    fn status(self) -> i32;
}

impl Exit for i32 {
    fn status(self) -> i32 {
        self
    }
}

impl Exit for () {
    fn status(self) -> i32 {
        0
    }
}

impl<T: Exit> Exit for Result<T> {
    fn status(self) -> i32 {
        match self {
            Ok(v) => v.status(),
            Err(e) if e.is(Kind::Cancelled) => 130,
            Err(e) => {
                eprintln!("{}: {e}", program_name());
                1
            }
        }
    }
}

fn program_name() -> String {
    std::env::args().next().unwrap_or_else(|| "koru".into())
}

/// What `#[koru::main]` expands into: open the ring, run the program, run the
/// hooks, and hand back the exit status.
pub fn entry<F, Fut, T>(main: F) -> i32
where
    F: FnOnce(Args) -> Fut,
    Fut: Future<Output = T>,
    T: Exit,
{
    let rt = match Runtime::new(&default_config()) {
        Ok(rt) => rt,
        Err(e) => {
            eprintln!("{}: /dev/koru: {e}", program_name());
            return 1;
        }
    };
    install(rt);
    let status = block_on(main(Args::from_env())).status();
    shutdown();
    status
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::vocab::{Errno, Error};

    #[test]
    fn an_exit_status_is_braams() {
        assert_eq!(3.status(), 3);
        assert_eq!(().status(), 0);
        assert_eq!(Ok::<i32, Error>(2).status(), 2);
        assert_eq!(Ok::<(), Error>(()).status(), 0);
        // ^C is 130, and every other failure is 1.
        assert_eq!(Err::<i32, Error>(Error::from(Errno(125))).status(), 130);
        assert_eq!(Err::<i32, Error>(Error::from(Errno(2))).status(), 1);
    }

    /// No ring, no panic: the free functions are the ones that insist.
    #[test]
    fn there_is_no_ambient_ring_until_one_is_installed() {
        assert!(try_current().is_none());
        assert_eq!(stdout().0, 0);
    }
}
