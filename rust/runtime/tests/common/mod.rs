// SPDX-License-Identifier: MIT

//! Helpers for the runtime suite. Every `.rs` directly in `tests/` is its own
//! binary, so shared code lives here.

#![allow(dead_code)]

use koru::{Runtime, SetupConfig};
use koru_sys::sys;
use std::future::Future;
use std::pin::Pin;
use std::task::{Context, Poll};

/// Deliberately few slots, so recycling and exhaustion are reachable in a
/// test rather than theoretical.
pub const SQ: u32 = 32;
pub const CQ: u32 = 64;
pub const SLOT: u32 = 65536;
pub const SLOTS: u32 = 4;
pub const HANDLES: u32 = 16;

pub const MS: u64 = 1_000_000;

/// Its own path, so a concurrent koru-sys run cannot collide.
pub const DATAFILE: &str = "/tmp/koru-rt-data";
pub const DATASIZE: usize = 8192;

/// A whole slot, so a deferred READ runs long enough for a race window to be
/// wide. The same reason the koru-sys suite uses 64 KB slots.
pub const BIGFILE: &str = "/tmp/koru-rt-big";
pub const BIGSIZE: usize = SLOT as usize;

pub fn config() -> SetupConfig {
    SetupConfig::new(SQ, CQ, SLOT, SLOTS, HANDLES)
}

pub fn runtime() -> Runtime {
    Runtime::new(&config()).expect("/dev/koru")
}

pub fn pattern_byte(i: usize) -> u8 {
    (i.wrapping_mul(31).wrapping_add(7)) as u8
}

/// Create the data file once per process.
pub fn ensure_data_file() {
    use std::sync::Once;
    static ONCE: Once = Once::new();
    ONCE.call_once(|| {
        let buf: Vec<u8> = (0..DATASIZE).map(pattern_byte).collect();
        std::fs::write(DATAFILE, &buf).expect("data file");
    });
}

/// Create the big data file once per process.
pub fn ensure_big_file() {
    use std::sync::Once;
    static ONCE: Once = Once::new();
    ONCE.call_once(|| {
        let buf: Vec<u8> = (0..BIGSIZE).map(pattern_byte).collect();
        std::fs::write(BIGFILE, &buf).expect("big data file");
    });
}

/// The runner forwards these unconditionally, so empty means unset.
fn env_num(name: &str) -> Option<u64> {
    let v = std::env::var(name).ok()?;
    let v = v.trim();
    if v.is_empty() {
        return None;
    }
    Some(
        v.parse()
            .unwrap_or_else(|_| panic!("{name} is not a number")),
    )
}

/// Iteration count for a race loop. Low by default so the everyday gate stays
/// fast; `KORU_ITERS=100000` is the full count T16's done test names.
pub fn iters(default: u32) -> u32 {
    env_num("KORU_ITERS").map_or(default, |n| n as u32)
}

/// Seed for a race loop's jitter, echoed by the caller so a failure replays.
pub fn seed() -> u64 {
    env_num("KORU_SEED").unwrap_or_else(|| {
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| u64::from(d.subsec_nanos()) | 1)
            .unwrap_or(1)
    })
}

/// xorshift64*. Deterministic given the seed, which is the whole point.
pub struct Rng(u64);

impl Rng {
    pub fn new(seed: u64) -> Rng {
        Rng(if seed == 0 { 1 } else { seed })
    }

    pub fn next(&mut self) -> u64 {
        let mut x = self.0;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        self.0 = x;
        x.wrapping_mul(0x2545_f491_4f6c_dd1d)
    }

    /// Uniform in `0..n`, which is close enough for a jitter.
    pub fn below(&mut self, n: u64) -> u64 {
        if n == 0 { 0 } else { self.next() % n }
    }
}

/// Must match the kernel's exactly, mask included.
pub fn fnv1a(p: &[u8]) -> i64 {
    let mut h: u64 = 0xcbf2_9ce4_8422_2325;
    for &b in p {
        h ^= b as u64;
        h = h.wrapping_mul(0x0000_0100_0000_01b3);
    }
    (h & 0x7fff_ffff_ffff_ffff) as i64
}

/// Poll a future exactly once. `None` means it is pending, which for a koru op
/// means it is now registered and queued but not yet submitted.
pub struct PollOnce<'a, F>(pub &'a mut F);

impl<F: Future + Unpin> Future for PollOnce<'_, F> {
    type Output = Option<F::Output>;

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        match Pin::new(&mut *self.get_mut().0).poll(cx) {
            Poll::Ready(v) => Poll::Ready(Some(v)),
            Poll::Pending => Poll::Ready(None),
        }
    }
}

extern "C" fn alarm_die(_sig: i32) {
    unsafe { sys::_exit(99) };
}

/// A test that can hang must arm this; stdout is line buffered and uncaptured,
/// so a hang stays diagnosable.
pub fn arm_alarm(secs: u32) {
    unsafe {
        sys::signal(sys::SIGALRM, alarm_die as *const () as usize);
        sys::alarm(secs);
    }
}

pub fn disarm_alarm() {
    unsafe {
        sys::alarm(0);
    }
}

/// Does nothing but interrupt whatever is parked.
pub extern "C" fn sigalrm_noop(_sig: i32) {}

pub fn interrupt_in(secs: u32) {
    unsafe {
        sys::signal(sys::SIGALRM, sigalrm_noop as *const () as usize);
        sys::alarm(secs);
    }
}

/// A skip is not a pass. The gate script greps for this and fails the run.
pub fn skip(what: &str, why: &str) {
    println!("KORU-RS-SKIP {what}: {why}");
}
