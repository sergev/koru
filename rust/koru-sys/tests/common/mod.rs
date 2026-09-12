// SPDX-License-Identifier: MIT

//! Helpers for the device suite. Every `.rs` directly in `tests/` is its own
//! binary, so shared code lives here.

#![allow(dead_code)]

use koru_sys::abi::*;
use koru_sys::error::Errno;
use koru_sys::ring::{Arena, EnterError, Ring, SetupConfig};
use koru_sys::sys;
use std::io;

/// The geometry most sections use. 64 KB slots are deliberate: a deferred READ
/// or CHECKSUM over a whole slot runs long enough to make race windows common.
pub const SQ: u32 = 64;
pub const CQ: u32 = 128;
pub const SLOT: u32 = 65536;
pub const SLOTS: u32 = 24;
pub const HANDLES: u32 = 32;

/// Its own path, so a concurrent C run cannot collide.
pub const PATFILE: &str = "/tmp/koru-check-rs-pattern";
pub const PATSIZE: usize = 65536;

/// The WRITE tests' own target, created and removed by them.
pub const WRFILE: &str = "/tmp/koru-check-rs-write";

pub fn shared_config() -> SetupConfig {
    SetupConfig::new(SQ, CQ, SLOT, SLOTS, HANDLES)
}

/// A configured and mapped ring.
pub struct Mapped {
    pub ring: Ring,
    pub arena: Arena,
}

impl Mapped {
    pub fn new(cfg: &SetupConfig) -> Mapped {
        let ring = Ring::with_config(cfg).expect("SETUP");
        let arena = ring.mmap().expect("mmap");
        Mapped { ring, arena }
    }

    pub fn shared() -> Mapped {
        Mapped::new(&shared_config())
    }

    pub fn slot_size(&self) -> u32 {
        self.arena.slot_size()
    }

    pub fn slot_count(&self) -> u32 {
        self.arena.slot_count()
    }

    pub fn handle_count(&self) -> u32 {
        self.ring.params().handle_count
    }

    /// Whole slot as bytes. No op may name it.
    pub fn slot(&self, i: u32) -> &mut [u8] {
        unsafe { self.arena.slot_mut(i) }
    }

    pub fn fill(&self, i: u32, f: impl Fn(usize) -> u8) {
        let s = self.slot(i);
        for (j, b) in s.iter_mut().enumerate() {
            *b = f(j);
        }
    }

    /// Zero the slot, copy the path without its NUL, return the byte count.
    pub fn put_path(&self, slot: u32, path: &str) -> u32 {
        let s = self.slot(slot);
        s.fill(0);
        s[..path.len()].copy_from_slice(path.as_bytes());
        path.len() as u32
    }

    pub fn run_one(&self, sqe: &Sqe) -> i64 {
        self.ring
            .run_one(sqe)
            .expect("ENTER")
            .expect("no completion")
    }

    pub fn open_path(&self, slot: u32, path: &str, flags: u32) -> i64 {
        let n = self.put_path(slot, path);
        self.run_one(&Sqe::open(0x100, slot, 0, n, flags))
    }

    pub fn close_handle(&self, handle: u32) -> i64 {
        self.run_one(&Sqe::close(0x101, handle))
    }

    pub fn read_into(&self, handle: u32, slot: u32, off: u64, len: u32) -> i64 {
        self.run_one(&Sqe::read(0x102, handle, slot, off, len))
    }

    pub fn write_from(&self, handle: u32, slot: u32, off: u64, len: u32) -> i64 {
        self.run_one(&Sqe::write(0x103, handle, slot, off, len))
    }

    /// Assert nothing was left in flight.
    pub fn assert_quiesced(&self) {
        let left = self.ring.quiesce().expect("quiesce");
        assert_eq!(left, 0, "left {left} completion(s) in flight");
    }
}

pub fn find_cqe(cq: &[Cqe], user_data: u64) -> &Cqe {
    cq.iter()
        .find(|c| c.user_data == user_data)
        .unwrap_or_else(|| panic!("no CQE with user_data {user_data:#x}"))
}

/// Assert the exact errno, never just that the call failed.
#[track_caller]
pub fn expect_errno<T>(r: io::Result<T>, want: Errno, what: &str) {
    match r {
        Ok(_) => panic!("{what}: succeeded, expected {want:?}"),
        Err(e) => {
            let got = Errno(e.raw_os_error().unwrap_or(0));
            assert_eq!(got, want, "{what}");
        }
    }
}

#[track_caller]
pub fn expect_enter_errno<T>(r: Result<T, EnterError>, want: Errno, what: &str) {
    match r {
        Ok(_) => panic!("{what}: succeeded, expected {want:?}"),
        Err(e) => assert_eq!(e.errno, want, "{what}"),
    }
}

pub fn pattern_byte(i: usize) -> u8 {
    (i.wrapping_mul(31)
        .wrapping_add((i >> 8) * 7)
        .wrapping_add(11)) as u8
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

pub fn make_pattern_file(path: &str, n: usize) -> io::Result<()> {
    let buf: Vec<u8> = (0..n).map(pattern_byte).collect();
    std::fs::write(path, &buf)
}

/// Field 1 of /proc/sys/fs/file-nr: allocated struct files.
pub fn file_nr() -> i64 {
    std::fs::read_to_string("/proc/sys/fs/file-nr")
        .ok()
        .and_then(|s| s.split_whitespace().next()?.parse().ok())
        .unwrap_or(-1)
}

/// `fput` can be deferred to task work, so let it settle first.
pub fn file_nr_settled() -> i64 {
    std::thread::sleep(std::time::Duration::from_millis(100));
    file_nr()
}

pub fn count_fds() -> usize {
    std::fs::read_dir("/proc/self/fd")
        .map(|d| d.count())
        .unwrap_or(0)
}

/// A skip is not a pass. The gate script greps for this and fails the run.
pub fn skip(what: &str, why: &str) {
    println!("KORU-RS-SKIP {what}: {why}");
}

pub fn is_root() -> bool {
    unsafe { sys::geteuid() == 0 }
}

extern "C" fn alarm_die(_sig: i32) {
    unsafe { sys::_exit(99) };
}

/// Arm a watchdog. A test that can hang must call this; stdout is line
/// buffered and uncaptured, so a hang stays diagnosable.
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

pub fn now_ms() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_millis() as u64
}

/// Create the pattern file once per process.
pub fn ensure_pattern_file() {
    use std::sync::Once;
    static ONCE: Once = Once::new();
    ONCE.call_once(|| {
        make_pattern_file(PATFILE, PATSIZE).expect("pattern file");
    });
}
