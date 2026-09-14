// SPDX-License-Identifier: MIT

//! Braam's `proc/screen.h`: the terminal from inside a program. A grid of its
//! own, and the calls that claim the real one and blit onto it.
//!
//! The transport is invisible above this file, which is what makes koru's
//! screen a compatible replacement rather than a lookalike: `ProcScreen` keeps
//! its six methods, and what is underneath them is a Unix socket adopted into
//! the ring rather than a syscall into a browser kernel.
//!
//! Three rules hold here, and doc/Notes.md says why each is forced:
//!
//!   - **The handshake is synchronous POSIX; everything after it is koru.**
//!   - **The grid is ordinary heap, never an arena slot.** A slot with an op in
//!     flight belongs to the kernel, so a grid living in one would be
//!     unpaintable for the length of every blit. The damage is packed into a
//!     slot instead, which costs one copy this project has never paid before.
//!   - **Exactly one write is in flight on the socket.** `WRITE` is deferred to
//!     a workqueue and koru has no op linking, so two concurrent writes on one
//!     handle have no order, and on a stream socket that braids two frames into
//!     permanent corruption. Senders queue behind the one in flight.
//!
//! The byte channel at the end is the other connection: no frames past its
//! handshake, and `install` adopts it as stdout.

use crate::future::Handle;
use crate::grid::{Grid, Pane, Rect};
use crate::ks_abi::*;
use crate::ops;
use crate::rt;
use crate::vocab::{Error, Result};
use koru_sys::error::{EINVAL, Errno};
use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::future::poll_fn;
use std::os::fd::{AsRawFd, RawFd};
use std::rc::Rc;
use std::task::{Poll, Waker};

/// A key, as a program sees it. There are no control characters: `^C` is `'c'`
/// with [`MOD_CTRL`], and the reader decides what that means.
#[derive(Copy, Clone, Default, PartialEq, Eq, Debug)]
pub struct Key {
    pub code: u32,
    pub mods: u32,
}

impl Key {
    /// A codepoint that draws, with no modifier that would make it a command.
    pub fn printable(&self) -> bool {
        self.code >= 0x20
            && self.code != 0x7f
            && self.code < KEY_NAMED
            && self.mods & (MOD_CTRL | MOD_ALT | MOD_META) == 0
    }
}

// Braam's names for the protocol's key numbering, so a program says what it
// says on Braam.
pub const MOD_SHIFT: u32 = KS_MOD_SHIFT;
pub const MOD_CTRL: u32 = KS_MOD_CTRL;
pub const MOD_ALT: u32 = KS_MOD_ALT;
pub const MOD_META: u32 = KS_MOD_META;

pub const KEY_NAMED: u32 = KS_KEY_NAMED;
pub const KEY_ENTER: u32 = KS_KEY_ENTER;
pub const KEY_BACKSPACE: u32 = KS_KEY_BACKSPACE;
pub const KEY_TAB: u32 = KS_KEY_TAB;
pub const KEY_ESCAPE: u32 = KS_KEY_ESCAPE;
pub const KEY_DELETE: u32 = KS_KEY_DELETE;
pub const KEY_INSERT: u32 = KS_KEY_INSERT;
pub const KEY_UP: u32 = KS_KEY_UP;
pub const KEY_DOWN: u32 = KS_KEY_DOWN;
pub const KEY_LEFT: u32 = KS_KEY_LEFT;
pub const KEY_RIGHT: u32 = KS_KEY_RIGHT;
pub const KEY_HOME: u32 = KS_KEY_HOME;
pub const KEY_END: u32 = KS_KEY_END;
pub const KEY_PAGE_UP: u32 = KS_KEY_PAGE_UP;
pub const KEY_PAGE_DOWN: u32 = KS_KEY_PAGE_DOWN;

// ---------------------------------------------------------------------------
// The connection
// ---------------------------------------------------------------------------

/// One reply: the header, and whatever payload came with it.
#[derive(Clone, Debug)]
struct Reply {
    flags: u16,
    res: i32,
    body: Vec<u8>,
}

impl Reply {
    /// The payload as a record, where it is the size that record wants.
    fn payload<const N: usize>(&self) -> Option<[u8; N]> {
        self.body.get(..N)?.try_into().ok()
    }
}

struct Conn {
    ctrl: Handle,
    replies: RefCell<HashMap<u32, Reply>>,
    wakers: RefCell<HashMap<u32, Waker>>,
    /// Whoever is waiting for the socket to be writable, in arrival order.
    senders: RefCell<Vec<Waker>>,
    writing: Cell<bool>,
    next_seq: Cell<u32>,
    /// Sticky: once the far end has gone, every call answers with it and
    /// nothing touches the wire again.
    dead: RefCell<Option<Error>>,
    cols: Cell<u32>,
    rows: Cell<u32>,
    max_frame: Cell<u32>,
}

impl Conn {
    fn seq(&self) -> u32 {
        // Never 0: that is the protocol's reserved value for an unsolicited
        // frame, and a request carrying it is refused.
        let s = self.next_seq.get().wrapping_add(1).max(1);
        self.next_seq.set(s);
        s
    }

    /// Files a reply and wakes whoever is waiting for it.
    fn deliver(&self, seq: u32, r: Reply) {
        self.replies.borrow_mut().insert(seq, r);
        if let Some(w) = self.wakers.borrow_mut().remove(&seq) {
            w.wake();
        }
    }

    /// The far end has gone. Everyone parked is answered, and every later call
    /// is answered without a syscall.
    fn kill(&self, e: Error) {
        if self.dead.borrow().is_none() {
            *self.dead.borrow_mut() = Some(e);
        }
        let wakers: Vec<Waker> = self.wakers.borrow_mut().drain().map(|(_, w)| w).collect();
        for w in wakers {
            w.wake();
        }
        let senders: Vec<Waker> = std::mem::take(&mut *self.senders.borrow_mut());
        for w in senders {
            w.wake();
        }
    }

    fn error(&self) -> Option<Error> {
        *self.dead.borrow()
    }
}

/// The pump: the one task that reads the socket. Everything else waits for a
/// `seq` it filed, which is what keeps a reply that arrives out of order from
/// waking the wrong caller.
async fn pump(conn: Rc<Conn>) {
    let mut buf: Vec<u8> = Vec::new();
    loop {
        let mut got = Vec::new();
        match ops::read_into(conn.ctrl, &mut got, 0).await {
            Ok(0) => {
                conn.kill(Error::closed());
                return;
            }
            Ok(_) => buf.extend_from_slice(&got),
            Err(e) => {
                conn.kill(e);
                return;
            }
        }

        // One read can carry three whole replies or half of one: this is the
        // place the plan says a second implementer conflates `seq` with a koru
        // cookie, and the frames are why they cannot be the same number.
        let mut at = 0;
        while buf.len() - at >= 16 {
            let len = u32::from_le_bytes(buf[at..at + 4].try_into().unwrap()) as usize;
            if len < 16 || len > KS_MAX_FRAME as usize {
                conn.kill(Error::from_errno(koru_sys::error::EPROTO));
                return;
            }
            if buf.len() - at < len {
                break;
            }
            let seq = u32::from_le_bytes(buf[at + 8..at + 12].try_into().unwrap());
            let reply = Reply {
                flags: u16::from_le_bytes(buf[at + 6..at + 8].try_into().unwrap()),
                res: i32::from_le_bytes(buf[at + 12..at + 16].try_into().unwrap()),
                body: buf[at + 16..at + len].to_vec(),
            };
            conn.deliver(seq, reply);
            at += len;
        }
        buf.drain(..at);
    }
}

/// Sends one whole frame, with exactly one write in flight on the socket.
async fn send(conn: &Rc<Conn>, frame: &[u8]) -> Result<()> {
    if let Some(e) = conn.error() {
        return Err(e);
    }

    // The token. A sender that finds one in flight parks; the one that
    // finishes wakes the next. Nothing here is a lock: the runtime is single
    // threaded, and this is an ordering rule, not a mutual exclusion one.
    poll_fn(|cx| {
        if conn.error().is_some() || !conn.writing.get() {
            conn.writing.set(true);
            Poll::Ready(())
        } else {
            conn.senders.borrow_mut().push(cx.waker().clone());
            Poll::Pending
        }
    })
    .await;

    let out = match conn.error() {
        Some(e) => Err(e),
        None => ops::write_bytes(conn.ctrl, frame).await,
    };

    conn.writing.set(false);
    if let Some(w) = {
        let mut q = conn.senders.borrow_mut();
        if q.is_empty() {
            None
        } else {
            Some(q.remove(0))
        }
    } {
        w.wake();
    }
    if let Err(e) = out {
        conn.kill(e);
        return Err(e);
    }
    Ok(())
}

/// Sends a request and waits for the reply that carries its `seq`, whatever
/// its result is. The caller maps it: `-EINTR` on a key read carries a payload
/// and every other error does not, so one of them must see the raw reply.
async fn request_raw(conn: &Rc<Conn>, op: u32, flags: u16, payload: &[u8]) -> Result<Reply> {
    let seq = conn.seq();
    let mut frame = Vec::with_capacity(16 + payload.len());
    frame.extend_from_slice(&((16 + payload.len()) as u32).to_le_bytes());
    frame.extend_from_slice(&(op as u16).to_le_bytes());
    frame.extend_from_slice(&flags.to_le_bytes());
    frame.extend_from_slice(&seq.to_le_bytes());
    frame.extend_from_slice(&0i32.to_le_bytes());
    frame.extend_from_slice(payload);
    send(conn, &frame).await?;

    let reply = poll_fn(|cx| {
        if let Some(r) = conn.replies.borrow_mut().remove(&seq) {
            return Poll::Ready(Ok(r));
        }
        if let Some(e) = conn.error() {
            return Poll::Ready(Err(e));
        }
        conn.wakers.borrow_mut().insert(seq, cx.waker().clone());
        Poll::Pending
    })
    .await?;

    Ok(reply)
}

/// The same, with a negative result turned into an error. This protocol's
/// errnos are koru's errnos: one table, so a program sees the same names on
/// either side of the socket.
async fn request(conn: &Rc<Conn>, op: u32, flags: u16, payload: &[u8]) -> Result<Reply> {
    let r = request_raw(conn, op, flags, payload).await?;
    if r.res < 0 {
        return Err(Error::from_errno(Errno(-r.res)));
    }
    Ok(r)
}

// ---------------------------------------------------------------------------
// ProcScreen
// ---------------------------------------------------------------------------

/// The key half of a screen, on its own so that a parked read and a blit can
/// be in flight at once.
///
/// Braam's `ProcScreen` is one object and its coroutines interleave on it
/// freely; Rust will not lend it twice, and a program that waits for a key
/// while it paints is the ordinary shape of a full-screen program. So the
/// reader is a handle of its own — cheap, cloneable, and holding only the
/// connection. The geometry a reply carries is recorded on the connection, and
/// the screen takes it up at the next `grid`, `root` or `flush`.
#[derive(Clone)]
pub struct Keys {
    conn: Rc<Conn>,
}

impl Keys {
    /// The next key. `Err(Intr)` is a resize that arrived with no key behind
    /// it; the geometry is already recorded, so the caller repaints and asks
    /// again.
    pub async fn next(&self) -> Result<Key> {
        let r = request_raw(&self.conn, KS_OP_KEY_READ, 0, &[]).await?;
        let b: [u8; 16] = match r.payload() {
            // -EINTR is the only negative result that carries a payload, and
            // this is why: a resize has to be answered, not signalled, so the
            // new geometry rides on the refusal itself.
            Some(b) => b,
            None if r.res < 0 => return Err(Error::from_errno(Errno(-r.res))),
            None => return Err(Error::from_errno(EINVAL)),
        };
        let word = |i: usize| u32::from_le_bytes(b[i * 4..i * 4 + 4].try_into().unwrap());
        if word(2) != 0 && word(3) != 0 {
            self.conn.cols.set(word(2));
            self.conn.rows.set(word(3));
        }
        if r.res < 0 {
            return Err(Error::from_errno(Errno(-r.res)));
        }
        Ok(Key {
            code: word(0),
            mods: word(1),
        })
    }
}

/// The painting half of a screen, on its own for the reason [`Keys`] is: two
/// tasks may paint at once, and the connection serialises them.
#[derive(Clone)]
pub struct Painter {
    conn: Rc<Conn>,
}

impl Painter {
    /// Sends `d` of `g` as blit frames, banded to what one frame holds.
    /// `Ok(false)` is a stale blit: the daemon has resized, nothing was drawn,
    /// and its reply carried the new geometry.
    pub async fn blit(&self, g: &Grid, d: Rect) -> Result<bool> {
        if d.w == 0 || d.h == 0 {
            return Ok(true);
        }
        let rows = rows_per_band(self.conn.max_frame.get(), d.w)?;
        let mut y = d.y;
        while y < d.y + d.h {
            let h = rows.min(d.y + d.h - y);
            let band = Rect {
                x: d.x,
                y,
                w: d.w,
                h,
            };
            let r = request(&self.conn, KS_OP_BLIT, 0, &pack_blit(g, band)).await?;
            if r.flags & KS_F_STALE != 0 {
                let b: [u8; 8] = r.payload().ok_or_else(|| Error::from_errno(EINVAL))?;
                self.conn
                    .cols
                    .set(u32::from_le_bytes(b[0..4].try_into().unwrap()));
                self.conn
                    .rows
                    .set(u32::from_le_bytes(b[4..8].try_into().unwrap()));
                return Ok(false);
            }
            y += h;
        }
        Ok(true)
    }
}

/// How many rows of a band this many cells wide fit one frame. At least one,
/// or the geometry could not be sent at all — which is what [`KS_MIN_SLOT`]
/// exists to guarantee.
fn rows_per_band(max_frame: u32, w: u32) -> Result<u32> {
    let room = max_frame as usize - 16 - 40;
    let row = w as usize * 8;
    if row == 0 || row > room {
        return Err(Error::from_errno(EINVAL));
    }
    Ok((room / row) as u32)
}

/// One blit frame's payload: the header, then the cells row by row. Pure, so
/// it is tested without a socket.
pub fn pack_blit(g: &Grid, d: Rect) -> Vec<u8> {
    let mut out = Vec::with_capacity(40 + (d.w * d.h) as usize * 8);
    for v in [
        d.x,
        d.y,
        d.w,
        d.h,
        g.cursor_x,
        g.cursor_y,
        u32::from(g.cursor_on),
        g.cols(),
        g.rows(),
        0,
    ] {
        out.extend_from_slice(&v.to_le_bytes());
    }
    for y in d.y..d.y + d.h {
        for x in d.x..d.x + d.w {
            let c = g.at(x, y).copied().unwrap_or_default();
            out.extend_from_slice(&c.ch.to_le_bytes());
            out.extend_from_slice(&[c.fg, c.bg, c.attrs, 0]);
        }
    }
    out
}

/// The terminal, from inside a process: a grid of its own, and the calls that
/// claim the real one and blit onto it.
pub struct Screen {
    conn: Rc<Conn>,
    grid: Grid,
    keys: bool,
    screen: bool,
    /// Kept, never read: dropping it closes the descriptor, which is what
    /// gives the claims back — the daemon keys them to the connection.
    #[allow(dead_code)]
    sock: Option<std::os::unix::net::UnixStream>,
}

impl Screen {
    /// Connects to the daemon, starting one if there is none, and shakes
    /// hands.
    ///
    /// Everything up to the `ADOPT_FD` is ordinary POSIX — the bounded
    /// preamble the plan allows, and where the spawn race has to live anyway.
    pub async fn connect() -> Result<Screen> {
        let sock = connect_or_spawn(&sock_path())?;
        Screen::own(sock).await
    }

    /// The same over a descriptor the caller keeps. This is what a test hands
    /// one end of a `socketpair`: `ADOPT_FD` takes a reference of its own, so
    /// the caller's descriptor stays the caller's to close.
    pub async fn adopt(fd: RawFd) -> Result<Screen> {
        Screen::build(fd, None).await
    }

    /// The connection owns the socket, so dropping the screen closes it —
    /// which is what releases the claims, because the daemon keys them to the
    /// connection and not to a process.
    pub async fn own(sock: std::os::unix::net::UnixStream) -> Result<Screen> {
        let fd = sock.as_raw_fd();
        Screen::build(fd, Some(sock)).await
    }

    async fn build(fd: RawFd, sock: Option<std::os::unix::net::UnixStream>) -> Result<Screen> {
        let rt = rt::current();
        let ctrl = rt.adopt(fd).await?;
        rt::register_handle(ctrl, false);

        let conn = Rc::new(Conn {
            ctrl,
            replies: RefCell::new(HashMap::new()),
            wakers: RefCell::new(HashMap::new()),
            senders: RefCell::new(Vec::new()),
            writing: Cell::new(false),
            next_seq: Cell::new(0),
            dead: RefCell::new(None),
            cols: Cell::new(0),
            rows: Cell::new(0),
            max_frame: Cell::new(KS_MAX_FRAME),
        });
        rt::spawn(pump(Rc::clone(&conn)));

        let mut hello = Vec::new();
        hello.extend_from_slice(&KS_MAGIC.to_le_bytes());
        hello.extend_from_slice(&KS_ABI_VERSION.to_le_bytes());
        let r = request(&conn, KS_OP_HELLO, 0, &hello).await?;
        let b: [u8; 32] = r.payload().ok_or_else(|| Error::from_errno(EINVAL))?;
        let word = |i: usize| u32::from_le_bytes(b[i * 4..i * 4 + 4].try_into().unwrap());
        if word(0) != KS_MAGIC || word(1) != KS_ABI_VERSION {
            return Err(Error::from_errno(koru_sys::error::EPROTO));
        }
        conn.cols.set(word(2));
        conn.rows.set(word(3));
        conn.max_frame.set(word(6).min(KS_MAX_FRAME));

        Ok(Screen {
            conn,
            grid: Grid::default(),
            keys: false,
            screen: false,
            sock,
        })
    }

    /// Binds this to a screen other than the process's own. Reserved for the
    /// multiplexing this plan defers, so the daemon answers `-ENOSYS` and the
    /// program sees `Kind::Unsupported`.
    pub async fn attach(&mut self, _term_id: u32) -> Result<()> {
        request(&self.conn, KS_OP_TERM_OPEN, 0, &[]).await?;
        Ok(())
    }

    /// Claims the keyboard. Do this before reading stdin, so what is typed
    /// while a slow pipe fills is queued rather than echoed.
    pub async fn take_keys(&mut self) -> Result<()> {
        let r = request(&self.conn, KS_OP_KEY_CLAIM, KS_F_TAKE, &[]).await?;
        self.keys = true;
        self.geom_from(&r)
    }

    /// Takes the alternate screen, and sizes the grid to it.
    pub async fn take_screen(&mut self) -> Result<()> {
        let r = request(&self.conn, KS_OP_SCREEN_CLAIM, KS_F_TAKE, &[]).await?;
        self.screen = true;
        self.geom_from(&r)
    }

    /// A painter, which a program blits through while it waits for a key.
    pub fn painter(&self) -> Painter {
        Painter {
            conn: Rc::clone(&self.conn),
        }
    }

    /// A reader for the keys, which a program parks on while it paints.
    pub fn keys(&self) -> Keys {
        Keys {
            conn: Rc::clone(&self.conn),
        }
    }

    pub fn grid(&mut self) -> &mut Grid {
        self.sync();
        &mut self.grid
    }

    pub fn root(&mut self) -> Pane {
        self.sync();
        Pane::of(&self.grid)
    }

    /// The whole grid minus the bottom row, and that row.
    pub fn body(&mut self) -> Pane {
        let r = self.root();
        if r.height() > 1 {
            r.top(r.height() - 1)
        } else {
            r
        }
    }

    pub fn status(&mut self) -> Pane {
        self.root().bottom(1)
    }

    /// Takes up a geometry a key reply reported. The grid is resized and marked
    /// whole, which is what a program repaints from.
    fn sync(&mut self) {
        let (cols, rows) = (self.conn.cols.get(), self.conn.rows.get());
        if cols != 0 && rows != 0 && (self.grid.cols() != cols || self.grid.rows() != rows) {
            self.grid.resize(cols, rows);
        }
    }

    /// Sends the cells that changed, and the cursor with them.
    ///
    /// A maximum blit does not fit an arena slot — 512x256 cells is exactly
    /// 1 MiB and the headers push it over — so the damage is banded into as
    /// many frames as fit. Banding is provably unstuck: a maximum-width row is
    /// one page and a slot is at least two.
    pub async fn flush(&mut self) -> Result<()> {
        if let Some(e) = self.conn.error() {
            return Err(e);
        }
        self.sync();
        let d = self.grid.take_damage();
        if !self.painter().blit(&self.grid, d).await? {
            // The daemon resized under us and its reply carried the new
            // geometry: take it up, which damages the whole grid, and let the
            // caller flush again rather than sending bands of a grid that no
            // longer exists.
            self.sync();
        }
        Ok(())
    }

    /// The next key. A resize arrives with it: the geometry rides on every
    /// reply, and the grid is resized and marked whole when it changes.
    ///
    /// `Err(Intr)` is a resize that arrived with no key behind it. The grid is
    /// already the new shape; the caller repaints and asks again.
    pub async fn next_key(&mut self) -> Result<Key> {
        let got = self.keys().next().await;
        self.sync(); // the reply's geometry, taken up before the caller paints
        got
    }

    /// What the daemon last said the terminal is, without asking again.
    pub fn geometry(&self) -> (u32, u32) {
        (self.conn.cols.get(), self.conn.rows.get())
    }

    // ---------------------------------------------------------------- inside

    fn geom_from(&mut self, r: &Reply) -> Result<()> {
        let b: [u8; 8] = r.payload().ok_or_else(|| Error::from_errno(EINVAL))?;
        let cols = u32::from_le_bytes(b[0..4].try_into().unwrap());
        let rows = u32::from_le_bytes(b[4..8].try_into().unwrap());
        self.resize(cols, rows)
    }

    fn resize(&mut self, cols: u32, rows: u32) -> Result<()> {
        if cols == 0 || rows == 0 || cols > KS_MAX_COLS || rows > KS_MAX_ROWS {
            return Err(Error::from_errno(EINVAL));
        }
        self.conn.cols.set(cols);
        self.conn.rows.set(rows);
        if self.grid.cols() != cols || self.grid.rows() != rows {
            self.grid.resize(cols, rows);
        }
        Ok(())
    }
}

// ---------------------------------------------------------------------------
// The byte channel
// ---------------------------------------------------------------------------

/// The daemon's other connection: the one whose far end is the terminal's
/// parser rather than the protocol server. It *is* stdout, so `write_all` stays
/// a plain `WRITE` with no framing in the way.
///
/// `None` unless both halves of the question say yes: this program's stdout is
/// the terminal koru was started from, and a daemon is already listening.
/// Neither is negotiable. A redirected stdout is the user's own instruction and
/// stays where it points; and **this never spawns a daemon**, because `install`
/// runs before any program has asked for a screen and a window nobody wanted is
/// worse than no window.
///
/// The handshake is synchronous POSIX, as [`Screen::connect`]'s preamble is:
/// there is nothing to read on this connection afterwards, so a pump task and
/// a `seq` map would be machinery for one round trip.
pub(crate) fn adopt_byte_channel(rt: &crate::Runtime) -> Option<Handle> {
    use std::io::{IsTerminal, Read, Write};

    if !std::io::stdout().is_terminal() {
        return None;
    }
    let mut s = std::os::unix::net::UnixStream::connect(sock_path()).ok()?;
    let wait = std::time::Duration::from_secs(2);
    s.set_read_timeout(Some(wait)).ok()?;
    s.set_write_timeout(Some(wait)).ok()?;

    // The lengths are the protocol's own arithmetic, never a number spelled
    // here: a payload that grows must not have to be found by hand.
    let mut frame = Vec::with_capacity(ks_req_len(KS_OP_HELLO) as usize);
    frame.extend_from_slice(&ks_req_len(KS_OP_HELLO).to_le_bytes());
    frame.extend_from_slice(&(KS_OP_HELLO as u16).to_le_bytes());
    frame.extend_from_slice(&KS_F_BYTES.to_le_bytes());
    frame.extend_from_slice(&1u32.to_le_bytes()); // seq, and the only one
    frame.extend_from_slice(&0i32.to_le_bytes());
    frame.extend_from_slice(&KS_MAGIC.to_le_bytes());
    frame.extend_from_slice(&KS_ABI_VERSION.to_le_bytes());
    s.write_all(&frame).ok()?;

    // A daemon that does not know this flag answers -EINVAL, and one that does
    // not know the version closes: either way the program keeps the stdout it
    // was given.
    let len = ks_rep_len(KS_OP_HELLO) as usize;
    let mut rep = vec![0u8; len];
    s.read_exact(&mut rep).ok()?;
    let word = |i: usize| u32::from_le_bytes(rep[i..i + 4].try_into().unwrap());
    if word(0) as usize != len || i32::from_le_bytes(rep[12..16].try_into().unwrap()) != 0 {
        return None;
    }
    if word(16) != KS_MAGIC || word(20) != KS_ABI_VERSION {
        return None;
    }

    // koru admits a non-regular file only when it was opened non-blocking, and
    // ADOPT_FD takes a reference of its own: ours goes on the way out.
    s.set_nonblocking(true).ok()?;
    rt.block_on(rt.adopt(s.as_raw_fd())).ok()
}

/// Connects, and starts a daemon if nothing answers.
///
/// The race is settled by an exclusive `flock` on a file beside the socket:
///
///   1. try to connect — the ordinary case, with a daemon already running;
///   2. take the lock, waiting for whoever holds it;
///   3. **try to connect again**, because the winner may have finished while
///      we waited;
///   4. only then, unlink a socket nothing is listening on and spawn.
///
/// Step 4 is what the lock is really for: **only the lock holder may unlink**.
/// Without that rule, twenty clients racing a *live* daemon would each decide
/// its socket was stale and remove it.
fn connect_or_spawn(path: &str) -> Result<std::os::unix::net::UnixStream> {
    if let Some(s) = try_connect(path) {
        return ready_socket(s);
    }

    let lock = lock_file(path)?;
    // Waiting for the lock is the whole of the queue: whoever holds it is
    // either spawning or about to give up.
    for _ in 0..2000 {
        match koru_sys::sys::flock_try(std::os::fd::AsFd::as_fd(&lock)) {
            Ok(true) => break,
            Ok(false) => std::thread::sleep(std::time::Duration::from_millis(5)),
            Err(e) => return Err(e.into()),
        }
    }

    if let Some(s) = try_connect(path) {
        return ready_socket(s); // the winner got there while we waited
    }

    // Nothing is listening. A socket that is still there is stale, and this
    // process holds the lock, so it is the one allowed to say so.
    let _ = std::fs::remove_file(path);
    spawn_daemon(path)?;

    // The daemon binds a temporary name and renames it into place, so a socket
    // that exists is one that answers; there is nothing to do but wait for it.
    for _ in 0..2000 {
        if let Some(s) = try_connect(path) {
            return ready_socket(s);
        }
        std::thread::sleep(std::time::Duration::from_millis(5));
    }
    Err(Error::from_errno(koru_sys::error::ETIMEDOUT))
}

/// One connect, retried briefly on a refusal.
///
/// **A refusal is not proof of a dead daemon.** A listening socket whose accept
/// queue is full answers `ECONNREFUSED` too, and twenty clients starting at
/// once are exactly that case: without this, some of them would decide a live
/// daemon's socket was stale.
fn try_connect(path: &str) -> Option<std::os::unix::net::UnixStream> {
    for _ in 0..40 {
        match std::os::unix::net::UnixStream::connect(path) {
            Ok(s) => return Some(s),
            Err(e) if e.kind() == std::io::ErrorKind::ConnectionRefused => {
                std::thread::sleep(std::time::Duration::from_millis(5));
            }
            Err(_) => return None, // no socket at all: nothing to wait for
        }
    }
    None
}

/// koru admits a non-regular file only when it was opened non-blocking, and it
/// never sets that bit on a descriptor it did not open.
fn ready_socket(s: std::os::unix::net::UnixStream) -> Result<std::os::unix::net::UnixStream> {
    s.set_nonblocking(true)?;
    Ok(s)
}

/// The lock file beside the socket. Its directory is checked here: a runtime
/// directory that is not ours, or that anyone may write to, is not somewhere
/// to put a socket other programs will trust.
fn lock_file(path: &str) -> Result<std::fs::File> {
    use std::os::unix::fs::{MetadataExt, OpenOptionsExt, PermissionsExt};

    let dir = std::path::Path::new(path)
        .parent()
        .ok_or_else(|| Error::from_errno(EINVAL))?;
    let meta = std::fs::metadata(dir)?;
    if !meta.is_dir() {
        return Err(Error::from_errno(koru_sys::error::ENOTDIR));
    }
    // `/tmp` is the fallback and is 1777, so the check is on the *socket's*
    // owner rather than the directory's mode there; under $XDG_RUNTIME_DIR it
    // is both.
    if meta.uid() != users_own_uid() {
        return Err(Error::from_errno(koru_sys::error::EPERM));
    }
    if dir != std::path::Path::new("/tmp") && meta.permissions().mode() & 0o077 != 0 {
        return Err(Error::from_errno(koru_sys::error::EPERM));
    }

    let lock = format!("{path}.lock");
    Ok(std::fs::OpenOptions::new()
        .create(true)
        .read(true)
        .write(true)
        .truncate(false)
        .mode(0o600)
        .open(lock)?)
}

fn users_own_uid() -> u32 {
    // SAFETY-free: `geteuid` takes nothing and cannot fail. koru-sys owns the
    // declaration, because this crate forbids `unsafe`.
    koru_sys::sys::euid()
}

/// Starts the daemon, detached: it outlives the client that started it, and a
/// window that vanished between two commands would not be a terminal.
fn spawn_daemon(path: &str) -> Result<()> {
    let bin = std::env::var("KORU_SCREEN_BIN").unwrap_or_else(|_| "koru-screen".to_string());
    std::process::Command::new(bin)
        .env("KORU_SCREEN_SOCK", path)
        .stdin(std::process::Stdio::null())
        .stdout(std::process::Stdio::null())
        .spawn()?;
    Ok(())
}

/// Where the daemon's socket is. `$KORU_SCREEN_SOCK` names one outright;
/// otherwise it is the protocol's own name under `$XDG_RUNTIME_DIR`.
pub fn sock_path() -> String {
    if let Ok(p) = std::env::var("KORU_SCREEN_SOCK") {
        if !p.is_empty() {
            return p;
        }
    }
    let dir = std::env::var("XDG_RUNTIME_DIR").unwrap_or_default();
    let dir = if dir.is_empty() {
        "/tmp".to_string()
    } else {
        dir
    };
    format!("{dir}/{KS_SOCK_NAME}")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_key_is_printable_only_without_a_command_modifier() {
        assert!(
            Key {
                code: 'a' as u32,
                mods: 0
            }
            .printable()
        );
        assert!(
            Key {
                code: 'A' as u32,
                mods: MOD_SHIFT
            }
            .printable(),
            "shift is part of the character, not a command"
        );
        assert!(
            !Key {
                code: 'c' as u32,
                mods: MOD_CTRL
            }
            .printable()
        );
        assert!(
            !Key {
                code: KEY_UP,
                mods: 0
            }
            .printable()
        );
        assert!(
            !Key {
                code: 0x7f,
                mods: 0
            }
            .printable()
        );
    }

    #[test]
    fn the_socket_path_prefers_the_environments_own() {
        // Not a race with other tests: both variables are read, never written.
        let want = std::env::var("KORU_SCREEN_SOCK").ok();
        let got = sock_path();
        match want {
            Some(p) if !p.is_empty() => assert_eq!(got, p),
            _ => assert!(got.ends_with(KS_SOCK_NAME)),
        }
    }
}
