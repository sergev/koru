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
