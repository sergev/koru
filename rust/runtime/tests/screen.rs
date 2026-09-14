// SPDX-License-Identifier: MIT
//
// T37's done test: the real screen client on a real ring, against a fake
// daemon this test speaks itself over a `socketpair`.
//
// The fake is a thread with the other end, so it can answer *while* the client
// is parked in `ENTER` — which is the only way to get a reply out of order, to
// stop reading until the socket fills, or to go away mid-frame.
//
// Needs `/dev/koru`, so it runs in the VM under `scripts/rust.sh`. Run with
// `--test-threads=1`: the ring is thread-local and each test installs one.

mod common;

use common::*;
use koru::{Kind, Screen};
use std::cell::Cell;
use std::io::{Read, Write};
use std::os::unix::net::UnixStream;
use std::rc::Rc;
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::mpsc::{Receiver, Sender, channel};
use std::thread::JoinHandle;
use std::time::Duration;

// ---------------------------------------------------------------------------
// The wire, as the test speaks it
// ---------------------------------------------------------------------------

const OP_HELLO: u16 = 1;
const OP_KEY_CLAIM: u16 = 2;
const OP_KEY_READ: u16 = 3;
const OP_SCREEN_CLAIM: u16 = 4;
const OP_BLIT: u16 = 5;
const MAGIC: u32 = 0x7263_736b;
const VERSION: u32 = 1;
const F_STALE: u16 = 1;
const EINTR: i32 = 4;

#[derive(Clone, Debug)]
struct Frame {
    op: u16,
    seq: u32,
    body: Vec<u8>,
}

impl Frame {
    fn word(&self, i: usize) -> u32 {
        u32::from_le_bytes(self.body[i * 4..i * 4 + 4].try_into().unwrap())
    }

    /// A blit's rectangle: x, y, w, h.
    fn rect(&self) -> (u32, u32, u32, u32) {
        (self.word(0), self.word(1), self.word(2), self.word(3))
    }

    fn cells(&self) -> usize {
        (self.body.len() - 40) / 8
    }
}

fn encode(op: u16, flags: u16, seq: u32, res: i32, body: &[u8]) -> Vec<u8> {
    let mut f = Vec::with_capacity(16 + body.len());
    f.extend_from_slice(&((16 + body.len()) as u32).to_le_bytes());
    f.extend_from_slice(&op.to_le_bytes());
    f.extend_from_slice(&flags.to_le_bytes());
    f.extend_from_slice(&seq.to_le_bytes());
    f.extend_from_slice(&res.to_le_bytes());
    f.extend_from_slice(body);
    f
}

fn words(vs: &[u32]) -> Vec<u8> {
    let mut b = Vec::new();
    for v in vs {
        b.extend_from_slice(&v.to_le_bytes());
    }
    b
}

// ---------------------------------------------------------------------------
// The fake daemon
// ---------------------------------------------------------------------------

/// The fake daemon: two threads over one socket.
///
/// **Two, not one.** A single thread that both read and wrote would block in
/// its read while a reply waited behind it in the queue — the client waiting
/// for that reply, the fake waiting for a frame, and neither moving. Reading
/// and writing are independent on a socket, so they are independent here.
enum Cmd {
    Send(Vec<u8>),
    Hangup,
}

struct Fake {
    cmds: Sender<Cmd>,
    frames: Receiver<Frame>,
    /// Milliseconds the reader waits before its next read: the test's way of
    /// saying "stop reading", which is what fills the socket buffer.
    pause: Arc<AtomicU64>,
    threads: Vec<JoinHandle<()>>,
}

impl Fake {
    /// A socketpair: the client's end comes back, and the fake keeps the other.
    fn start(cols: u32, rows: u32) -> (Fake, UnixStream) {
        let (mine, theirs) = UnixStream::pair().expect("socketpair");
        let writer_sock = mine.try_clone().expect("a writing end of our own");
        let (cmds, cmd_rx) = channel::<Cmd>();
        let (frame_tx, frames) = channel::<Frame>();
        let pause = Arc::new(AtomicU64::new(0));

        // The reader: every frame the client sends, and the handshake, which
        // every case needs and none is about.
        let reader = {
            let pause = Arc::clone(&pause);
            let cmds = cmds.clone();
            std::thread::spawn(move || {
                let mut sock = mine;
                let mut buf: Vec<u8> = Vec::new();
                let Some(h) = read_frame(&mut sock, &mut buf) else {
                    return;
                };
                assert_eq!(h.op, OP_HELLO);
                let rep = words(&[MAGIC, VERSION, cols, rows, 512, 256, 65536, 0]);
                let _ = cmds.send(Cmd::Send(encode(OP_HELLO, 0, h.seq, 0, &rep)));

                // Chunked, and the pause is checked before **every read**,
                // not before every frame: a pause that only took effect at a
                // frame boundary would let the reader drain the socket for as
                // long as the client kept sending, and the buffer would never
                // fill. The first version of this test did exactly that, and
                // its backpressure case proved nothing.
                loop {
                    let ms = pause.swap(0, Ordering::SeqCst);
                    if ms > 0 {
                        std::thread::sleep(Duration::from_millis(ms));
                    }
                    let mut chunk = [0u8; 4096];
                    match sock.read(&mut chunk) {
                        Ok(0) | Err(_) => return,
                        Ok(n) => buf.extend_from_slice(&chunk[..n]),
                    }
                    while let Some(f) = take_frame(&mut buf) {
                        if frame_tx.send(f).is_err() {
                            return;
                        }
                    }
                }
            })
        };

        // The writer: replies, and the hangup.
        let writer = std::thread::spawn(move || {
            let mut sock = writer_sock;
            while let Ok(cmd) = cmd_rx.recv() {
                match cmd {
                    Cmd::Send(bytes) => {
                        if sock.write_all(&bytes).is_err() {
                            return;
                        }
                    }
                    Cmd::Hangup => {
                        let _ = sock.shutdown(std::net::Shutdown::Both);
                        return;
                    }
                }
            }
        });

        (
            Fake {
                cmds,
                frames,
                pause,
                threads: vec![reader, writer],
            },
            theirs,
        )
    }

    /// The next frame the client sent, or `None` if it sent none in time.
    ///
    /// **Asynchronous on purpose.** A blocking receive would hold the whole
    /// thread, and the client is single-threaded: the task that owes the frame
    /// would never be polled and the test would deadlock against itself.
    /// Waiting through `sleep_for` lets the executor run everything else,
    /// which is what the fake is waiting for.
    async fn take(&self, tries: u32) -> Option<Frame> {
        for _ in 0..tries {
            if let Ok(f) = self.frames.try_recv() {
                return Some(f);
            }
            let _ = koru::sleep_for(1).await;
        }
        None
    }

    fn reply(&self, f: &Frame, res: i32, flags: u16, body: Vec<u8>) {
        let _ = self
            .cmds
            .send(Cmd::Send(encode(f.op, flags, f.seq, res, &body)));
    }

    /// Stops reading for a while, so the socket buffer fills under the client.
    fn stop_reading(&self, ms: u64) {
        self.pause.store(ms, Ordering::SeqCst);
    }

    fn hangup(&self) {
        let _ = self.cmds.send(Cmd::Hangup);
    }
}

impl Drop for Fake {
    fn drop(&mut self) {
        let _ = self.cmds.send(Cmd::Hangup);
        for t in self.threads.drain(..) {
            let _ = t.join();
        }
    }
}

/// One whole frame out of what has been read, if there is one. The length
/// checks are the fake's own: a stream that has been braided fails here, which
/// is what the single-writer case rests on.
fn take_frame(buf: &mut Vec<u8>) -> Option<Frame> {
    if buf.len() < 16 {
        return None;
    }
    let len = u32::from_le_bytes(buf[0..4].try_into().unwrap()) as usize;
    assert!((16..=65536).contains(&len), "the client sent len {len}");
    assert_eq!(len % 8, 0, "a frame's length is a multiple of eight");
    if buf.len() < len {
        return None;
    }
    let f = Frame {
        op: u16::from_le_bytes(buf[4..6].try_into().unwrap()),
        seq: u32::from_le_bytes(buf[8..12].try_into().unwrap()),
        body: buf[16..len].to_vec(),
    };
    buf.drain(..len);
    Some(f)
}

/// The same, blocking until a whole frame is there. The handshake only.
fn read_frame(sock: &mut UnixStream, buf: &mut Vec<u8>) -> Option<Frame> {
    loop {
        if let Some(f) = take_frame(buf) {
            return Some(f);
        }
        let mut chunk = [0u8; 8192];
        match sock.read(&mut chunk) {
            Ok(0) => return None,
            Ok(n) => buf.extend_from_slice(&chunk[..n]),
            Err(_) => return None,
        }
    }
}

// ---------------------------------------------------------------------------
// The harness
// ---------------------------------------------------------------------------

/// Every frame the client has sent, collected by a task of its own.
///
/// This is the shape the whole suite needs: a client call is a *lazy* future
/// that sends nothing until it is polled, so a test that waits for the frame
/// before awaiting the call waits for ever. The collector runs alongside, the
/// test awaits the call, and the frames turn up here.
type Frames = Rc<std::cell::RefCell<Vec<Frame>>>;

fn collect(fake: &Rc<Fake>) -> Frames {
    let got: Frames = Rc::new(std::cell::RefCell::new(Vec::new()));
    let f = Rc::clone(fake);
    let into = Rc::clone(&got);
    koru::spawn(async move {
        while let Some(frame) = f.take(4000).await {
            into.borrow_mut().push(frame);
        }
    });
    got
}

/// The `i`-th frame the client sent, once it has sent it.
async fn frame(got: &Frames, i: usize) -> Frame {
    for _ in 0..4000 {
        if let Some(f) = got.borrow().get(i) {
            return f.clone();
        }
        let _ = koru::sleep_for(1).await;
    }
    panic!("the client sent no frame {i}");
}

fn with_screen<F, Fut>(cols: u32, rows: u32, f: F)
where
    F: FnOnce(Rc<Fake>, Screen) -> Fut,
    Fut: std::future::Future<Output = ()>,
{
    arm_alarm(60);
    koru::install(runtime());

    let (fake, ours) = Fake::start(cols, rows);
    ours.set_nonblocking(true).expect("non-blocking");
    let fake = Rc::new(fake);

    koru::block_on(async move {
        let screen = Screen::own(ours).await.expect("the handshake");
        f(fake, screen).await;
    });
    koru::rt::shutdown();
    disarm_alarm();
}

/// Claims the screen, which every painting case needs first. The answer comes
/// from a task of its own, because the claim is not sent until it is awaited.
async fn claim(fake: &Rc<Fake>, got: &Frames, screen: &mut Screen, cols: u32, rows: u32) {
    let at = got.borrow().len();
    answer(fake, got, at, 0, 0, words(&[cols, rows]));
    screen.take_screen().await.expect("the screen claim");
    assert_eq!(frame(got, at).await.op, OP_SCREEN_CLAIM);
}

/// Answers the `i`-th frame once it arrives, from a task of its own.
fn answer(fake: &Rc<Fake>, got: &Frames, i: usize, res: i32, flags: u16, body: Vec<u8>) {
    let fake = Rc::clone(fake);
    let got = Rc::clone(got);
    koru::spawn(async move {
        let f = frame(&got, i).await;
        fake.reply(&f, res, flags, body);
    });
}

// ---------------------------------------------------------------------------
// The cases
// ---------------------------------------------------------------------------

/// Out of order: a key read parks, a blit goes out behind it, and the blit is
/// answered first. `flush` must return while the key read is still parked,
/// which is the whole reason the pump demultiplexes by `seq` rather than
/// answering whoever asked first.
#[test]
fn a_reply_out_of_order_wakes_the_caller_that_asked_for_it() {
    with_screen(8, 4, |fake, mut screen| async move {
        let got = collect(&fake);
        claim(&fake, &got, &mut screen, 8, 4).await;

        answer(&fake, &got, 1, 0, 0, words(&[8, 4]));
        screen.take_keys().await.expect("the key claim");
        assert_eq!(frame(&got, 1).await.op, OP_KEY_CLAIM);

        // The key read parks: frame 2, and nothing answers it until the end.
        let keys = screen.keys();
        let done = Rc::new(Cell::new(false));
        {
            let done = Rc::clone(&done);
            koru::spawn(async move {
                let k = keys.next().await.expect("the key");
                assert_eq!((k.code, k.mods), ('c' as u32, koru::MOD_CTRL));
                done.set(true);
            });
        }
        let parked = frame(&got, 2).await;
        assert_eq!(parked.op, OP_KEY_READ);

        // A blit, sent after it and answered before it.
        let mut p = screen.root();
        p.write(screen.grid(), "x");
        answer(&fake, &got, 3, 0, 0, words(&[8, 4]));
        screen.flush().await.expect("the flush");
        assert_eq!(frame(&got, 3).await.op, OP_BLIT);
        assert!(!done.get(), "the key read is still parked");

        // And only now the key.
        fake.reply(&parked, 0, 0, words(&['c' as u32, koru::MOD_CTRL, 8, 4]));
        for _ in 0..200 {
            if done.get() {
                break;
            }
            let _ = koru::sleep_for(1).await;
        }
        assert!(done.get(), "the parked read was answered");
    });
}

/// The single-writer rule. Several tasks paint at once, and the fake must see
/// whole frames — never two braided together.
///
/// **The writes have to be short for the braid to exist at all.** One `WRITE`
/// of a whole frame is atomic on a stream socket, so two concurrent
/// single-shot writes cannot interleave and a small blit proves nothing: the
/// first version of this test passed with the serialisation deleted, and so
/// did the second with two full repaints. Eight painters against a reader that
/// has stopped overfill the socket buffer, every writer gets a short write,
/// and the bytes of one frame are then split around another's.
#[test]
fn several_blits_at_once_are_whole_frames() {
    const PAINTERS: usize = 8;

    with_screen(512, 256, |fake, mut screen| async move {
        let got = collect(&fake);
        claim(&fake, &got, &mut screen, 512, 256).await;

        // One grid per painter, each filled with a letter of its own, so a
        // braid mixes two letters inside one frame.
        let mut grids = Vec::new();
        for i in 0..PAINTERS {
            let mut g = koru::Grid::default();
            g.resize(512, 256);
            let ch = char::from(b'a' + i as u8);
            let mut p = koru::Pane::of(&g);
            for y in 0..256 {
                p.move_to(0, y);
                for _ in 0..512 {
                    p.put(&mut g, ch);
                }
            }
            grids.push(g);
        }

        // The reader stops, so the buffer fills and the writes go short;
        // everything is answered as it is read afterwards.
        fake.stop_reading(200);
        serve_all(&fake, &got, 1, 512, 256);

        let whole = koru::Rect {
            x: 0,
            y: 0,
            w: 512,
            h: 256,
        };
        let done = Rc::new(Cell::new(0usize));
        for g in grids {
            let painter = screen.painter();
            let done = Rc::clone(&done);
            koru::spawn(async move {
                painter.blit(&g, whole).await.expect("a painter");
                done.set(done.get() + 1);
            });
        }
        for _ in 0..20000 {
            if done.get() == PAINTERS {
                break;
            }
            let _ = koru::sleep_for(1).await;
        }
        assert_eq!(done.get(), PAINTERS, "every painter finished");

        // Every frame is one grid's: a braid mixes two letters inside one, and
        // the fake's own length check fails before that.
        let frames = got.borrow();
        assert!(frames.len() > PAINTERS, "eight repaints are many frames");
        let mut seen = std::collections::BTreeSet::new();
        for f in frames.iter().skip(1) {
            assert_eq!(f.op, OP_BLIT);
            let first = f.body[40];
            for i in 0..f.cells() {
                assert_eq!(f.body[40 + i * 8], first, "a frame carries one grid");
            }
            seen.insert(first);
        }
        assert_eq!(seen.len(), PAINTERS, "every painter's letter arrived whole");
    });
}

/// Banding: a full repaint of the largest grid there is, through a 64 KiB
/// frame. The frames must tile the damage exactly once — no cell twice, none
/// missed — and each must fit what the daemon said it would read.
#[test]
fn a_full_repaint_bands_into_frames_that_tile_the_damage() {
    with_screen(512, 256, |fake, mut screen| async move {
        let got = collect(&fake);
        claim(&fake, &got, &mut screen, 512, 256).await;

        // Everything is damaged: the claim sized the grid. Answering is a task
        // of its own, because the flush does not return until every band has.
        serve_blits(&fake, &got, 1, 512, 256);
        screen.flush().await.expect("the flush");

        let frames = got.borrow().len() - 1;
        assert!(
            frames > 1,
            "a full repaint of this size cannot be one frame"
        );

        let mut covered = vec![0u8; 512 * 256];
        for f in got.borrow().iter().skip(1) {
            assert_eq!(f.op, OP_BLIT);
            let (x, y, w, h) = f.rect();
            assert_eq!(f.cells(), (w * h) as usize);
            assert!(16 + 40 + f.cells() * 8 <= 65536, "a band fits one frame");
            for row in y..y + h {
                for col in x..x + w {
                    covered[(row * 512 + col) as usize] += 1;
                }
            }
        }
        assert!(covered.iter().all(|&n| n == 1), "every cell exactly once");
    });
}

/// Backpressure. The fake stops reading until the socket buffer fills; the
/// client must make progress through `POLL_ADD` rather than spinning, which is
/// asserted as a bounded `ENTER` count. It is also the first time koru's
/// softirq wake path runs on a socket rather than a FIFO.
///
/// **It takes more than one painter to fill a socket.** The banding loop waits
/// for each band's reply before sending the next, so a single painter never
/// has more than one frame in flight and a 208 KiB buffer never fills: the
/// first version of this test measured a writer that was waiting for replies,
/// not for room, and a spinning one passed it.
#[test]
fn a_full_socket_parks_the_writer_rather_than_spinning() {
    const PAINTERS: usize = 8;
    const STOPPED_MS: u64 = 300;

    with_screen(512, 256, |fake, mut screen| async move {
        let got = collect(&fake);
        claim(&fake, &got, &mut screen, 512, 256).await;

        let mut g = koru::Grid::default();
        g.resize(512, 256);
        let g = Rc::new(g);
        let whole = koru::Rect {
            x: 0,
            y: 0,
            w: 512,
            h: 256,
        };

        fake.stop_reading(STOPPED_MS);
        serve_all(&fake, &got, 1, 512, 256);

        let before = koru::rt::current().stats().enters;
        let began = std::time::Instant::now();
        let done = Rc::new(Cell::new(0usize));
        for _ in 0..PAINTERS {
            let painter = screen.painter();
            let g = Rc::clone(&g);
            let done = Rc::clone(&done);
            koru::spawn(async move {
                painter.blit(&g, whole).await.expect("a painter");
                done.set(done.get() + 1);
            });
        }
        for _ in 0..20000 {
            if done.get() == PAINTERS {
                break;
            }
            let _ = koru::sleep_for(1).await;
        }
        assert_eq!(done.get(), PAINTERS, "every painter finished");

        let took = began.elapsed();
        let enters = koru::rt::current().stats().enters - before;
        let frames = (got.borrow().len() - 1) as u64;

        // The painters cannot have finished before the reader came back, or
        // the socket never filled and this case tested nothing.
        assert!(
            took.as_millis() as u64 >= STOPPED_MS - 50,
            "the painters finished in {took:?}: the socket never filled"
        );

        // A handful of ENTERs per frame is generous: a slot, the write, the
        // poll and the reply. A writer that retried instead of polling would
        // spend the whole stopped window making syscalls.
        assert!(
            enters <= frames * 12 + 512,
            "{enters} ENTERs for {frames} frames in {took:?}: the writer is spinning"
        );
    });
}

/// A stale blit is not an error: the daemon resized under us, so the client
/// takes the new geometry and repaints rather than failing.
#[test]
fn a_stale_blit_resizes_the_grid_instead_of_failing() {
    with_screen(8, 4, |fake, mut screen| async move {
        let got = collect(&fake);
        claim(&fake, &got, &mut screen, 8, 4).await;
        let mut p = screen.root();
        p.write(screen.grid(), "x");

        answer(&fake, &got, 1, 0, F_STALE, words(&[20, 6]));
        screen.flush().await.expect("a stale blit is not an error");

        assert_eq!(screen.geometry(), (20, 6));
        assert_eq!(screen.grid().cols(), 20);
        assert_eq!(
            screen.grid().damage().w,
            20,
            "the new grid is damaged whole, so the next flush repaints"
        );
    });
}

/// A resize with no key behind it answers the parked read with `-EINTR` and a
/// payload, which is the only negative result that carries one.
#[test]
fn a_resize_answers_a_parked_key_read_with_the_new_geometry() {
    with_screen(8, 4, |fake, mut screen| async move {
        let got = collect(&fake);
        claim(&fake, &got, &mut screen, 8, 4).await;

        answer(&fake, &got, 1, -EINTR, 0, words(&[0, 0, 20, 6]));
        let e = screen.next_key().await.expect_err("a resize is Err(Intr)");
        assert!(e.is(Kind::Intr), "{e}");
        assert_eq!(frame(&got, 1).await.op, OP_KEY_READ);

        assert_eq!(screen.geometry(), (20, 6));
        assert_eq!(screen.grid().rows(), 6, "the grid is already the new shape");
    });
}

/// Failure: the daemon goes away. Every parked caller is answered, every later
/// call answers without touching the wire, and nothing hangs — which is why
/// the alarm is armed and stdout is line-buffered.
#[test]
fn a_daemon_that_goes_away_completes_everyone_and_never_hangs() {
    with_screen(8, 4, |fake, mut screen| async move {
        let got = collect(&fake);
        claim(&fake, &got, &mut screen, 8, 4).await;

        let keys = screen.keys();
        let parked = Rc::new(Cell::new(false));
        {
            let parked = Rc::clone(&parked);
            koru::spawn(async move {
                let e = keys.next().await.expect_err("the daemon went away");
                assert!(e.is(Kind::Closed), "{e}");
                parked.set(true);
            });
        }
        assert_eq!(frame(&got, 1).await.op, OP_KEY_READ); // never answered

        fake.hangup();
        for _ in 0..500 {
            if parked.get() {
                break;
            }
            let _ = koru::sleep_for(1).await;
        }
        assert!(parked.get(), "the parked read was completed");

        // And every later call answers from the connection, not the wire.
        let mut p = screen.root();
        p.write(screen.grid(), "x");
        let e = screen.flush().await.expect_err("the connection is gone");
        assert!(e.is(Kind::Closed), "{e}");
        let e = screen.next_key().await.expect_err("and stays gone");
        assert!(e.is(Kind::Closed), "{e}");
    });
}

/// Answers every blit from `at` on, so a banded flush can finish.
fn serve_blits(fake: &Rc<Fake>, got: &Frames, at: usize, cols: u32, rows: u32) {
    let fake = Rc::clone(fake);
    let got = Rc::clone(got);
    koru::spawn(async move {
        let mut i = at;
        loop {
            let f = frame(&got, i).await;
            fake.reply(&f, 0, 0, words(&[cols, rows]));
            let (_, y, _, h) = f.rect();
            if y + h >= rows {
                return;
            }
            i += 1;
        }
    });
}

/// The same, with no idea how many frames there will be: two painters mean
/// two streams of bands, and which arrives when is the point.
fn serve_all(fake: &Rc<Fake>, got: &Frames, at: usize, cols: u32, rows: u32) {
    let fake = Rc::clone(fake);
    let got = Rc::clone(got);
    koru::spawn(async move {
        let mut i = at;
        loop {
            let f = frame(&got, i).await;
            fake.reply(&f, 0, 0, words(&[cols, rows]));
            i += 1;
        }
    });
}
