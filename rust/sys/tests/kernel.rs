// SPDX-License-Identifier: MIT

//! The device suite: T4-T11 re-expressed against `koru-sys`.
//!
//! Needs `/dev/koru` and root, so it runs in the VM under `scripts/rust.sh`.
//! Run with `--test-threads=1`: several tests fork, and several read
//! process-global counters.

mod common;

use common::*;
use koru_sys::abi::*;
use koru_sys::error::*;
use koru_sys::ring::{Ring, SetupConfig};
use koru_sys::sys;
use std::ffi::c_void;
use std::io::{Read, Write};
use std::os::fd::AsRawFd;
use std::time::{Duration, Instant};

/// One millisecond, in nanoseconds.
const MS: u64 = 1_000_000;

#[test]
fn smoke_nop_completes() {
    let m = Mapped::shared();
    assert_eq!(m.run_one(&Sqe::nop(0x01)), 0);
    m.assert_quiesced();
}

#[test]
fn smoke_arena_comes_back_zeroed_from_setup() {
    let m = Mapped::shared();
    assert!(m.slot(1).iter().all(|&b| b == 0));
}

#[test]
fn smoke_checksum_matches_the_host() {
    let m = Mapped::shared();
    m.fill(0, |_| 0xa5);
    let want = fnv1a(&m.slot(0)[..4096]);
    assert_eq!(m.run_one(&Sqe::checksum(0x02, 0, 0, 4096)), want);
    m.assert_quiesced();
}

#[test]
fn smoke_the_device_is_there() {
    // Fails loudly rather than skipping: a suite that passes without the
    // device proves nothing.
    Ring::open().expect("/dev/koru");
}

// ---------------------------------------------------------------------------
// T4 - ENTER
// ---------------------------------------------------------------------------

/// Every opcode plus two that do not exist.
const ALL_OPCODES: [u8; 16] = [
    KORU_OP_NOP,
    KORU_OP_DELAY_NS,
    KORU_OP_OPEN,
    KORU_OP_READ,
    KORU_OP_CLOSE,
    KORU_OP_CANCEL,
    KORU_OP_CHECKSUM,
    KORU_OP_WRITE,
    KORU_OP_ADOPT_FD,
    KORU_OP_POLL_ADD,
    KORU_OP_STAT,
    KORU_OP_TRUNCATE,
    KORU_OP_UTIMES,
    KORU_OP_READLINK,
    14,
    200,
];

#[test]
fn enter_returns_the_count_of_sqes_consumed() {
    let m = Mapped::shared();
    let sq: Vec<Sqe> = (0..8).map(|i| Sqe::nop(0x1000 + i)).collect();
    let mut cq = [Cqe::default(); 16];
    let r = m.ring.enter(&sq, &mut cq, 8, None).expect("ENTER");

    assert_eq!(r.consumed, 8);
    assert_eq!(r.progress.completed, 8);
    assert_eq!(r.progress.submitted, 8);
    // E1 reports the two independently, so agreement is worth asserting.
    assert_eq!(r.consumed, r.progress.submitted);

    for (i, c) in r.cqes(&cq).iter().enumerate() {
        assert_eq!(c.user_data, 0x1000 + i as u64);
        assert_eq!(c.res, 0);
        assert_eq!(c.flags, 0);
        assert_eq!(c.rsvd0, 0);
        assert_eq!(c.extra, 0);
    }
    m.assert_quiesced();
}

#[test]
fn enter_an_unknown_opcode_is_a_cqe_not_an_ioctl_error() {
    // Rule E1.
    let m = Mapped::shared();
    let sq = [Sqe {
        opcode: 200,
        user_data: 0x2000,
        ..Sqe::default()
    }];
    let mut cq = [Cqe::default(); 1];
    let r = m
        .ring
        .enter(&sq, &mut cq, 1, None)
        .expect("ENTER must succeed");
    assert_eq!(r.consumed, 1);
    assert_eq!(cq[0].user_data, 0x2000);
    assert_eq!(cq[0].res, -(EINVAL.0 as i64));
    m.assert_quiesced();
}

#[test]
fn enter_a_field_the_opcode_does_not_read_must_be_zero() {
    let m = Mapped::shared();
    let s = Sqe {
        len: 1,
        ..Sqe::nop(0x2001)
    };
    assert_eq!(m.run_one(&s), -(EINVAL.0 as i64));
    m.assert_quiesced();
}

#[test]
fn enter_a_non_zero_sqe_rsvd0_is_einval_for_every_opcode() {
    let m = Mapped::shared();
    for op in ALL_OPCODES {
        let s = Sqe {
            opcode: op,
            rsvd0: 1,
            user_data: 0x2100,
            ..Sqe::default()
        };
        assert_eq!(m.run_one(&s), -(EINVAL.0 as i64), "opcode {op}");
    }
    m.assert_quiesced();
}

#[test]
fn enter_an_unknown_sqe_flag_bit_is_einval_for_every_opcode() {
    let m = Mapped::shared();
    for op in ALL_OPCODES {
        let s = Sqe {
            opcode: op,
            flags: 1,
            user_data: 0x2200,
            ..Sqe::default()
        };
        assert_eq!(m.run_one(&s), -(EINVAL.0 as i64), "opcode {op}");
    }
    m.assert_quiesced();
}

#[test]
fn enter_a_mixed_batch_completes_every_sqe() {
    // C1: a malformed SQE is still consumed and still produces one CQE.
    let m = Mapped::shared();
    let sq: Vec<Sqe> = (0..8)
        .map(|i| {
            let ud = 0x3000 + i;
            if i % 2 == 1 {
                Sqe {
                    opcode: 250,
                    user_data: ud,
                    ..Sqe::default()
                }
            } else {
                Sqe::nop(ud)
            }
        })
        .collect();
    let mut cq = [Cqe::default(); 8];
    let r = m.ring.enter(&sq, &mut cq, 8, None).expect("ENTER");
    assert_eq!(r.consumed, 8);
    assert_eq!(r.progress.completed, 8);

    for i in 0..8u64 {
        let c = find_cqe(&cq, 0x3000 + i);
        let want = if i % 2 == 1 { -(EINVAL.0 as i64) } else { 0 };
        assert_eq!(c.res, want, "cqe {i}");
    }
    m.assert_quiesced();
}

#[test]
fn enter_a_short_cq_space_leaves_the_rest_queued() {
    let m = Mapped::shared();
    let sq: Vec<Sqe> = (0..8).map(|i| Sqe::nop(0x4000 + i)).collect();
    let mut cq = [Cqe::default(); 3];
    let r = m.ring.enter(&sq, &mut cq, 3, None).expect("ENTER");
    assert_eq!(r.consumed, 8, "all 8 are consumed");
    assert_eq!(r.progress.completed, 3, "only 3 fit");

    let mut cq2 = [Cqe::default(); 16];
    let r2 = m.ring.enter(&[], &mut cq2, 5, None).expect("ENTER");
    assert_eq!(r2.consumed, 0);
    assert_eq!(r2.progress.completed, 5, "the other 5 arrive later");
    for (i, c) in r2.cqes(&cq2).iter().enumerate() {
        assert_eq!(c.user_data, 0x4003 + i as u64);
    }
    m.assert_quiesced();
}

#[test]
fn enter_rejection_matrix() {
    let m = Mapped::shared();
    let p = *m.ring.params();
    let mut cq = [Cqe::default(); 4];

    // Each needs a field the safe API cannot express, so go through enter_raw.
    let base = || KoruEnter {
        cq_addr: 0,
        cq_space: 0,
        ..KoruEnter::default()
    };

    let mut e = KoruEnter { flags: 1, ..base() };
    expect_enter_errno(
        unsafe { m.ring.enter_raw(&mut e) },
        EINVAL,
        "an unknown ENTER flag",
    );

    let mut e = KoruEnter {
        reserved: [0, 1],
        ..base()
    };
    expect_enter_errno(
        unsafe { m.ring.enter_raw(&mut e) },
        EINVAL,
        "a non-zero reserved word",
    );

    let mut e = KoruEnter {
        to_submit: p.sq_entries + 1,
        ..base()
    };
    expect_enter_errno(
        unsafe { m.ring.enter_raw(&mut e) },
        EINVAL,
        "to_submit past sq_entries",
    );

    let mut e = KoruEnter {
        cq_space: p.cq_entries + 1,
        ..base()
    };
    expect_enter_errno(
        unsafe { m.ring.enter_raw(&mut e) },
        EINVAL,
        "cq_space past cq_entries",
    );

    let mut e = KoruEnter {
        cq_addr: cq.as_mut_ptr() as u64,
        cq_space: 2,
        min_complete: 3,
        ..KoruEnter::default()
    };
    expect_enter_errno(
        unsafe { m.ring.enter_raw(&mut e) },
        EINVAL,
        "min_complete past cq_space",
    );

    let mut e = KoruEnter {
        sq_addr: 0x10,
        to_submit: 1,
        ..base()
    };
    expect_enter_errno(
        unsafe { m.ring.enter_raw(&mut e) },
        EFAULT,
        "an unmapped sq_addr",
    );

    m.assert_quiesced();
}

#[test]
fn enter_before_setup_is_einval() {
    let r = Ring::open().expect("open");
    let mut e = KoruEnter::default();
    expect_enter_errno(
        unsafe { r.enter_raw(&mut e) },
        EINVAL,
        "ENTER on an unconfigured fd",
    );
}

/// Submit in batches, never reaping, until the ring stops consuming. Returns
/// the total consumed, which admission control bounds at `cq_entries`.
fn fill_cq(m: &Mapped, delay_ns: u64) -> u32 {
    let mut total = 0;
    loop {
        let sq: Vec<Sqe> = (0..8)
            .map(|i| {
                let ud = 0x5000 + total as u64 + i;
                if delay_ns > 0 {
                    Sqe::delay_ns(ud, delay_ns)
                } else {
                    Sqe::nop(ud)
                }
            })
            .collect();
        let r = m.ring.enter(&sq, &mut [], 0, None).expect("ENTER");
        total += r.consumed;
        if r.consumed < 8 {
            return total;
        }
    }
}

#[test]
fn enter_admission_control_stops_at_a_short_count() {
    let m = Mapped::new(&SetupConfig::new(64, 128, 4096, 8, 8));
    let want = m.ring.params().cq_entries;
    assert_eq!(
        fill_cq(&m, 0),
        want,
        "submitting past cq_entries stops short"
    );
    m.ring.quiesce().expect("drain");
}

#[test]
fn enter_in_flight_work_counts_against_cq_entries() {
    let m = Mapped::new(&SetupConfig::new(64, 128, 4096, 8, 8));
    let want = m.ring.params().cq_entries;
    assert_eq!(fill_cq(&m, 2 * MS), want);
    m.ring.quiesce().expect("drain");
}

// ---------------------------------------------------------------------------
// T5 - DELAY_NS and the blocking wait
// ---------------------------------------------------------------------------

#[test]
fn delay_four_run_concurrently_and_enter_waits() {
    let m = Mapped::shared();
    let sq: Vec<Sqe> = (0..4).map(|i| Sqe::delay_ns(0x1000 + i, 50 * MS)).collect();
    let mut cq = [Cqe::default(); 8];

    let t0 = Instant::now();
    let r = m.ring.enter(&sq, &mut cq, 4, None).expect("ENTER");
    let dt = t0.elapsed().as_millis() as u64;

    assert_eq!(r.consumed, 4);
    assert_eq!(r.progress.submitted, 4);
    assert_eq!(r.progress.completed, 4);
    assert!(dt >= 40, "ENTER returned after {dt}ms without waiting");
    assert!(dt < 150, "4 x 50ms took {dt}ms: they ran serially");

    for i in 0..4u64 {
        assert_eq!(find_cqe(&cq, 0x1000 + i).res, 0);
    }
    m.assert_quiesced();
}

#[test]
fn delay_a_short_timeout_reaps_nothing_and_returns_at_once() {
    let m = Mapped::shared();
    let sq = [Sqe::delay_ns(0x2000, 2000 * MS)];
    let mut cq = [Cqe::default(); 4];

    let t0 = Instant::now();
    let r = m
        .ring
        .enter(&sq, &mut cq, 1, Some(Duration::from_millis(10)))
        .expect("ENTER");
    let dt = t0.elapsed().as_millis() as u64;
    assert_eq!(r.consumed, 1);
    assert_eq!(r.progress.completed, 0, "a 10ms cap against a 2s delay");
    assert!(dt < 500, "waited {dt}ms, not the timeout");

    // min_complete 0 returns at once even with work in flight.
    let t0 = Instant::now();
    let r = m.ring.enter(&[], &mut cq, 0, None).expect("ENTER");
    assert_eq!(r.consumed, 0);
    assert!(t0.elapsed().as_millis() < 200);

    // And the delay can be cancelled rather than waited out.
    let r = m
        .ring
        .enter(&[Sqe::cancel(0x2001, 0x2000)], &mut cq, 2, None)
        .expect("ENTER");
    assert_eq!(r.consumed, 1);
    assert_eq!(r.progress.completed, 2, "the cancel and its target");
    m.assert_quiesced();
}

#[test]
fn delay_min_complete_past_what_can_arrive_returns_short() {
    let m = Mapped::shared();
    let mut cq = [Cqe::default(); 8];

    let t0 = Instant::now();
    let r = m
        .ring
        .enter(&[Sqe::nop(0x2100)], &mut cq, 3, None)
        .expect("ENTER");
    let dt = t0.elapsed().as_millis() as u64;
    assert_eq!(r.consumed, 1);
    assert_eq!(r.progress.completed, 1);
    assert!(dt < 500, "slept {dt}ms waiting for what cannot arrive");
    m.assert_quiesced();
}

#[test]
fn delay_an_idle_ring_does_not_sleep_for_ever() {
    // Unreachable means inflight == 0, not len == 0 && inflight == 0.
    let m = Mapped::shared();
    let mut cq = [Cqe::default(); 8];
    let t0 = Instant::now();
    let r = m.ring.enter(&[], &mut cq, 1, None).expect("ENTER");
    let dt = t0.elapsed().as_millis() as u64;
    assert_eq!(r.progress.completed, 0);
    assert!(dt < 200, "an idle ring slept {dt}ms");
}

#[test]
fn delay_rejection_matrix() {
    let m = Mapped::shared();
    let bad = -(EINVAL.0 as i64);
    assert_eq!(
        m.run_one(&Sqe {
            len: 1,
            ..Sqe::delay_ns(0x2200, MS)
        }),
        bad,
        "len"
    );
    assert_eq!(
        m.run_one(&Sqe {
            handle: 9,
            ..Sqe::delay_ns(0x2201, MS)
        }),
        bad,
        "handle"
    );
    assert_eq!(
        m.run_one(&Sqe {
            slot: 1,
            ..Sqe::delay_ns(0x2202, MS)
        }),
        bad,
        "slot"
    );
    m.assert_quiesced();
}

#[test]
fn delay_the_cap_is_accepted_and_one_past_it_is_not() {
    let m = Mapped::shared();
    let cap = m.ring.get_params().expect("GET_PARAMS").max_delay_ns;
    assert!(cap > 0);

    let mut cq = [Cqe::default(); 4];
    let r = m
        .ring
        .enter(&[Sqe::delay_ns(0x2300, cap)], &mut [], 0, None)
        .expect("ENTER");
    assert_eq!(r.consumed, 1, "a delay at the cap is accepted");

    let r = m
        .ring
        .enter(&[Sqe::cancel(0x2301, 0x2300)], &mut cq, 2, None)
        .expect("ENTER");
    assert_eq!(
        r.progress.completed, 2,
        "and can be cancelled rather than waited out"
    );

    assert_eq!(
        m.run_one(&Sqe::delay_ns(0x2302, cap + 1)),
        -(EINVAL.0 as i64)
    );
    m.assert_quiesced();
}

// ---------------------------------------------------------------------------
// T5 - signals during a blocking ENTER
// ---------------------------------------------------------------------------

extern "C" fn noop_handler(_sig: i32) {}

/// Fork a child that blocks in ENTER on a long delay, signal it, and return its
/// wait status.
///
/// The child body is async-signal-safe by construction: no panic, no println,
/// and it leaves through `_exit` so the parent's buffers are never flushed
/// twice. Its verdict travels as an exit code.
fn blocked_child(sig: i32, delay_ns: u64, handler: bool) -> i32 {
    let (mut rd, mut wr) = std::io::pipe().expect("pipe");

    let pid = unsafe { sys::fork() };
    assert!(pid >= 0, "fork");
    if pid == 0 {
        let code = unsafe {
            if handler {
                sys::signal(sys::SIGINT, noop_handler as *const () as usize);
            }
            drop(rd);
            match Ring::with_config(&SetupConfig::new(32, 64, 4096, 8, 8)) {
                Err(_) => 2,
                Ok(r) => {
                    let mut cq = [Cqe::default(); 4];
                    // Readiness first: the parent waits for this before signalling.
                    let _ = wr.write_all(b"x");
                    let _ = wr.flush();
                    match r.enter(&[Sqe::delay_ns(0x7000, delay_ns)], &mut cq, 1, None) {
                        Ok(_) => 3,
                        Err(e) if e.errno != EINTR => 4,
                        Err(e) if e.progress.submitted != 1 => 5,
                        Err(_) => 0,
                    }
                }
            }
        };
        unsafe { sys::_exit(code) };
    }

    drop(wr);
    let mut b = [0u8; 1];
    rd.read_exact(&mut b).expect("child readiness");
    std::thread::sleep(Duration::from_millis(150));
    unsafe { sys::kill(pid, sig) };

    let mut status = 0;
    unsafe { sys::waitpid(pid, &mut status, 0) };
    status
}

#[test]
fn signals_sigint_during_enter_is_eintr_with_submitted_intact() {
    arm_alarm(60);
    let status = blocked_child(sys::SIGINT, 30_000 * MS, true);
    disarm_alarm();
    assert!(sys::wifexited(status), "child was killed, not interrupted");
    // 2 setup, 3 unexpected success, 4 wrong errno, 5 submitted not written back.
    assert_eq!(sys::wexitstatus(status), 0, "child verdict");
}

#[test]
fn signals_sigkill_during_enter_kills_the_task_with_no_d_state() {
    arm_alarm(60);
    let status = blocked_child(sys::SIGKILL, 60_000 * MS, false);
    disarm_alarm();
    assert!(sys::wifsignaled(status), "child survived SIGKILL");
    assert_eq!(sys::wtermsig(status), sys::SIGKILL);
}

// ---------------------------------------------------------------------------
// T6 - teardown torture
// ---------------------------------------------------------------------------

#[test]
fn ringchurn_three_hundred_lifecycles_most_with_work_still_queued() {
    ensure_pattern_file();
    let mut in_flight = 0;

    for i in 0..300u32 {
        let cfg = SetupConfig::new(32, 64, 4096, 4 + i % 5, 8);
        let m = Mapped::new(&cfg);
        match i % 3 {
            0 => {
                let sq = [Sqe::nop(0x10), Sqe::checksum(0x11, 0, 0, 4096)];
                let mut cq = [Cqe::default(); 2];
                let r = m.ring.enter(&sq, &mut cq, 2, None).expect("ENTER");
                assert_eq!(r.consumed, 2, "round {i}");
                assert_eq!(r.progress.completed, 2, "round {i}");
            }
            1 => {
                let h = m.open_path(0, PATFILE, KORU_O_RDONLY);
                assert!(h > 0, "round {i}: OPEN gave {h}");
                let r = m
                    .ring
                    .enter(&[Sqe::read(0x12, h as u32, 1, 0, 4096)], &mut [], 0, None)
                    .expect("ENTER");
                assert_eq!(r.consumed, 1);
                in_flight += 1;
            }
            _ => {
                let r = m
                    .ring
                    .enter(&[Sqe::delay_ns(0x13, 500 * MS)], &mut [], 0, None)
                    .expect("ENTER");
                assert_eq!(r.consumed, 1);
                in_flight += 1;
            }
        }
        // Dropped here: munmap then close, with work still queued.
    }
    assert!(
        in_flight >= 150,
        "only {in_flight} torn down with work queued"
    );
}

// ---------------------------------------------------------------------------
// T7 - the arena
// ---------------------------------------------------------------------------

const M_SLOT: u32 = 4096;
const M_COUNT: u32 = 8;
const M_ARENA: usize = (M_SLOT * M_COUNT) as usize;

fn mmap_config() -> SetupConfig {
    SetupConfig::new(32, 64, M_SLOT, M_COUNT, 8)
}

/// A raw mmap attempt, so the rejection matrix can ask for things the safe API
/// will not express.
fn try_mmap(ring: &Ring, len: usize, flags: i32, offset: i64) -> Result<*mut c_void, Errno> {
    let p = unsafe {
        sys::mmap(
            std::ptr::null_mut(),
            len,
            sys::PROT_READ | sys::PROT_WRITE,
            flags,
            ring.as_raw_fd(),
            offset,
        )
    };
    if p == sys::MAP_FAILED {
        Err(Errno::last())
    } else {
        Ok(p)
    }
}

fn region_in_maps() -> bool {
    std::fs::read_to_string("/proc/self/maps")
        .map(|s| s.contains("/dev/koru"))
        .unwrap_or(false)
}

#[test]
fn mmap_rejection_matrix() {
    let unconfigured = Ring::open().expect("open");
    assert_eq!(
        try_mmap(&unconfigured, M_ARENA, sys::MAP_SHARED, 0).err(),
        Some(EINVAL),
        "mmap before SETUP"
    );
    drop(unconfigured);

    let ring = Ring::with_config(&mmap_config()).expect("SETUP");
    // MAP_PRIVATE would silently give copy-on-write, the failure worth guarding
    // hardest.
    assert_eq!(
        try_mmap(&ring, M_ARENA, sys::MAP_PRIVATE, 0).err(),
        Some(EINVAL),
        "MAP_PRIVATE"
    );
    assert_eq!(
        try_mmap(&ring, M_ARENA - 4096, sys::MAP_SHARED, 0).err(),
        Some(EINVAL),
        "one page short"
    );
    assert_eq!(
        try_mmap(&ring, M_ARENA + 4096, sys::MAP_SHARED, 0).err(),
        Some(EINVAL),
        "one page long"
    );
    assert_eq!(
        try_mmap(&ring, M_ARENA, sys::MAP_SHARED, 4096).err(),
        Some(EINVAL),
        "non-zero offset"
    );

    let arena = ring.mmap().expect("an exact MAP_SHARED mapping");
    expect_errno(ring.mmap().map(|_| ()), EBUSY, "a second mmap");
    assert!(region_in_maps(), "the region shows in /proc/self/maps");
    drop(arena);
}

#[test]
fn mmap_is_not_inherited_across_fork() {
    let ring = Ring::with_config(&mmap_config()).expect("SETUP");
    let arena = ring.mmap().expect("mmap");

    // VM_DONTCOPY is permanent: MADV_DOFORK refuses on VM_SPECIAL, which
    // VM_MIXEDMAP and VM_DONTEXPAND already put us in.
    assert!(
        arena.madvise(sys::MADV_DOFORK).is_err(),
        "madvise(MADV_DOFORK) must be refused"
    );

    let pid = unsafe { sys::fork() };
    assert!(pid >= 0, "fork");
    if pid == 0 {
        let code = if region_in_maps() { 1 } else { 0 };
        unsafe { sys::_exit(code) };
    }
    let mut status = 0;
    unsafe { sys::waitpid(pid, &mut status, 0) };
    assert!(
        sys::wifexited(status) && sys::wexitstatus(status) == 0,
        "the child inherited it"
    );
}

#[test]
fn mmap_munmap_with_an_op_in_flight() {
    let ring = Ring::with_config(&mmap_config()).expect("SETUP");
    let arena = ring.mmap().expect("mmap");

    let r = ring
        .enter(&[Sqe::delay_ns(0xd1, 100 * MS)], &mut [], 0, None)
        .expect("ENTER");
    assert_eq!(r.consumed, 1, "a delay is in flight");
    arena.unmap().expect("munmap with an op in flight");
    assert_eq!(
        ring.quiesce().expect("quiesce"),
        1,
        "the op still lands afterwards"
    );
}

#[test]
fn mmap_survives_close_of_the_ring_fd() {
    // vm_insert_page takes its own reference, so the mapping outlives the fd.
    // Arena deliberately does not borrow Ring, or this would not compile.
    let ring = Ring::with_config(&mmap_config()).expect("SETUP");
    let arena = ring.mmap().expect("mmap");
    let want: Vec<u8> = (0..M_SLOT as usize).map(pattern_byte).collect();
    unsafe { arena.slot_mut(2) }.copy_from_slice(&want);

    drop(ring);
    assert_eq!(
        unsafe { arena.slot(2) },
        &want[..],
        "the mapping still reads after close(fd)"
    );
    arena.unmap().expect("munmap after close(fd)");
}

// ---------------------------------------------------------------------------
// T7 - CHECKSUM
// ---------------------------------------------------------------------------

#[test]
fn checksum_matches_fnv1a_on_the_host() {
    let m = Mapped::shared();
    let n = m.slot_size() as usize;
    let pattern: Vec<u8> = (0..n).map(|i| (i * 31 + 7) as u8).collect();
    let want = fnv1a(&pattern);

    for slot in [0u32, 3, m.slot_count() - 1] {
        m.slot(slot).copy_from_slice(&pattern);
        assert_eq!(
            m.run_one(&Sqe::checksum(0x40, slot, 0, n as u32)),
            want,
            "slot {slot}"
        );
    }
    assert_ne!(
        m.run_one(&Sqe::checksum(0x41, 2, 0, n as u32)),
        want,
        "an untouched slot"
    );

    let rot: Vec<u8> = pattern.iter().cycle().skip(1).take(n).copied().collect();
    m.slot(1).copy_from_slice(&rot);
    assert_eq!(
        m.run_one(&Sqe::checksum(0x42, 1, 0, n as u32)),
        fnv1a(&rot),
        "a rotated pattern"
    );

    assert_eq!(
        m.run_one(&Sqe::checksum(0x43, 3, 100, 1000)),
        fnv1a(&pattern[100..1100]),
        "a sub-range"
    );
    assert_eq!(
        m.run_one(&Sqe::checksum(0x44, 3, 0, 0)),
        fnv1a(&[]),
        "a zero-length checksum"
    );
    m.assert_quiesced();
}

#[test]
fn checksum_rejection_matrix() {
    let m = Mapped::shared();
    let n = m.slot_size() as u64;
    let bad = -(EINVAL.0 as i64);
    assert_eq!(
        m.run_one(&Sqe::checksum(0x45, 0, n - 8, 16)),
        bad,
        "off + len past the slot"
    );
    assert_eq!(
        m.run_one(&Sqe::checksum(0x46, 0, u64::MAX, 16)),
        bad,
        "off + len overflows"
    );
    assert_eq!(
        m.run_one(&Sqe {
            handle: 1,
            ..Sqe::checksum(0x47, 0, 0, 16)
        }),
        bad,
        "handle"
    );
    m.assert_quiesced();
}

// ---------------------------------------------------------------------------
// T8 - kernel-enforced slot exclusivity
// ---------------------------------------------------------------------------

/// 64 KB so a deferred CHECKSUM cannot finish between two dispatches in one
/// submit loop, which is what makes a collision deterministic rather than a
/// race the test usually wins.
const S_SLOT: u32 = 65536;
/// 80 slots so the busy bitmap spans two 64-bit words.
const S_COUNT: u32 = 80;

/// Submit n CHECKSUMs naming the same slot in one batch and return their
/// results in submission order.
fn checksum_batch(m: &Mapped, slots: &[u32], len: u32) -> Vec<i64> {
    let sq: Vec<Sqe> = slots
        .iter()
        .enumerate()
        .map(|(i, &s)| Sqe::checksum(0xaa00 + i as u64, s, 0, len))
        .collect();
    let mut cq = vec![Cqe::default(); slots.len()];
    let r = m
        .ring
        .enter(&sq, &mut cq, slots.len() as u32, None)
        .expect("ENTER");
    assert_eq!(r.consumed as usize, slots.len(), "all consumed");
    assert_eq!(r.progress.completed as usize, slots.len(), "all completed");
    (0..slots.len())
        .map(|i| find_cqe(&cq, 0xaa00 + i as u64).res)
        .collect()
}

#[test]
fn slots_one_op_wins_and_the_rest_get_ebusy() {
    let m = Mapped::new(&SetupConfig::new(32, 64, S_SLOT, S_COUNT, 8));
    let pattern: Vec<u8> = (0..S_SLOT as usize).map(|i| (i * 17 + 3) as u8).collect();
    let want = fnv1a(&pattern);
    let busy = -(EBUSY.0 as i64);
    m.slot(3).copy_from_slice(&pattern);

    let res = checksum_batch(&m, &[3, 3], S_SLOT);
    assert_eq!(
        res.iter().filter(|&&r| r == want).count(),
        1,
        "exactly one succeeds"
    );
    assert_eq!(
        res.iter().filter(|&&r| r == busy).count(),
        1,
        "the other gets -EBUSY"
    );

    // Released in the same critical section that posts the CQE, so it is free
    // exactly when userspace can see the completion.
    assert_eq!(
        m.run_one(&Sqe::checksum(0xab, 3, 0, S_SLOT)),
        want,
        "the slot is free again"
    );

    let res = checksum_batch(&m, &[3, 3, 3], S_SLOT);
    assert_eq!(
        res.iter().filter(|&&r| r == want).count(),
        1,
        "three on one slot: one wins"
    );
    assert_eq!(
        res.iter().filter(|&&r| r == busy).count(),
        2,
        "two get -EBUSY"
    );
    m.assert_quiesced();
}

#[test]
fn slots_distinct_slots_never_collide() {
    let m = Mapped::new(&SetupConfig::new(32, 64, S_SLOT, S_COUNT, 8));
    let busy = -(EBUSY.0 as i64);

    assert!(
        checksum_batch(&m, &[3, 4], S_SLOT)
            .iter()
            .all(|&r| r != busy),
        "slots 3 and 4"
    );
    // Slot 70 lives in the second bitmap word.
    let res = checksum_batch(&m, &[70, 70], S_SLOT);
    assert_eq!(
        res.iter().filter(|&&r| r == busy).count(),
        1,
        "slot 70 is tracked too"
    );
    assert!(
        checksum_batch(&m, &[6, 70], S_SLOT)
            .iter()
            .all(|&r| r != busy),
        "6 and 70 do not alias"
    );

    let all: Vec<u32> = (0..8).collect();
    assert!(
        checksum_batch(&m, &all, S_SLOT).iter().all(|&r| r != busy),
        "eight distinct slots"
    );
    m.assert_quiesced();
}

#[test]
fn slots_out_of_range_indices_are_rejected() {
    // slot_try_acquire does not bounds-check itself, so a missing guard here is
    // a Rust bounds panic in the kernel. The bitmap is whole 64-bit words, so
    // slot_count + 1 is still inside it.
    let m = Mapped::new(&SetupConfig::new(32, 64, S_SLOT, S_COUNT, 8));
    let bad = -(EINVAL.0 as i64);
    for slot in [S_COUNT, S_COUNT + 1, u32::MAX] {
        assert_eq!(
            m.run_one(&Sqe::checksum(0xac, slot, 0, 16)),
            bad,
            "slot {slot}"
        );
    }
    // Rejected before it claims, so the slot stays usable.
    assert_eq!(
        m.run_one(&Sqe::checksum(0xad, 5, S_SLOT as u64, 16)),
        bad,
        "off past the slot"
    );
    assert!(
        m.run_one(&Sqe::checksum(0xae, 5, 0, S_SLOT)) >= 0,
        "slot 5 is still usable"
    );
    m.assert_quiesced();
}

// ---------------------------------------------------------------------------
// T9 - OPEN, CLOSE and the generational handle table
// ---------------------------------------------------------------------------

#[test]
fn open_handle_matrix() {
    let m = Mapped::shared();
    let ebadf = -(EBADF.0 as i64);

    let h = m.open_path(0, "/etc/hostname", KORU_O_RDONLY);
    assert!(h > 0, "OPEN gave {h}");
    let (idx, generation) = (handle_index(h as u32), handle_generation(h as u32));
    assert!(generation != 0, "generation must never be 0");
    assert!((idx as u32) < m.handle_count(), "index inside the table");

    assert_eq!(m.close_handle(h as u32), 0);
    assert_eq!(m.close_handle(h as u32), ebadf, "a double CLOSE");
    assert_eq!(m.close_handle(0), ebadf, "handle 0");
    assert_eq!(
        m.close_handle(make_handle(idx, generation.wrapping_add(7))),
        ebadf,
        "a stale generation"
    );
    assert_eq!(
        m.close_handle(make_handle(m.handle_count() as u16, 1)),
        ebadf,
        "an index past the table"
    );

    // A reused index must come back with a new generation, which is what makes
    // the retired handle reject.
    let h2 = m.open_path(0, "/etc/hostname", KORU_O_RDONLY);
    assert!(h2 > 0);
    assert_eq!(handle_index(h2 as u32), idx, "the index is reused");
    assert_ne!(
        handle_generation(h2 as u32),
        generation,
        "with a new generation"
    );
    assert_eq!(
        m.close_handle(h as u32),
        ebadf,
        "the old handle is still stale"
    );
    assert_eq!(m.close_handle(h2 as u32), 0);
    m.assert_quiesced();
}

#[test]
fn open_close_rejects_fields_it_does_not_read() {
    let m = Mapped::shared();
    let h = m.open_path(0, "/etc/hostname", KORU_O_RDONLY) as u32;
    let bad = -(EINVAL.0 as i64);
    assert_eq!(
        m.run_one(&Sqe {
            len: 1,
            ..Sqe::close(0x50, h)
        }),
        bad,
        "len"
    );
    assert_eq!(
        m.run_one(&Sqe {
            off: 1,
            ..Sqe::close(0x51, h)
        }),
        bad,
        "off"
    );
    assert_eq!(
        m.run_one(&Sqe {
            slot: 1,
            ..Sqe::close(0x52, h)
        }),
        bad,
        "slot"
    );
    assert_eq!(m.close_handle(h), 0, "and the handle survived all three");
    m.assert_quiesced();
}

#[test]
fn open_path_matrix() {
    let m = Mapped::shared();
    let bad = -(EINVAL.0 as i64);
    let n = m.slot_size();

    assert_eq!(
        m.open_path(0, "/no/such/path/here", KORU_O_RDONLY),
        -(ENOENT.0 as i64)
    );

    // An embedded NUL in the path bytes.
    let len = m.put_path(0, "/etc/hostname");
    m.slot(0)[3] = 0;
    assert_eq!(
        m.run_one(&Sqe::open(0x60, 0, 0, len, KORU_O_RDONLY)),
        bad,
        "an embedded NUL"
    );

    m.put_path(0, "/etc/hostname");
    assert_eq!(
        m.run_one(&Sqe::open(0x61, 0, 0, 0, KORU_O_RDONLY)),
        bad,
        "a zero-length path"
    );
    assert_eq!(
        m.run_one(&Sqe::open(0x62, 0, 0, n + 1, KORU_O_RDONLY)),
        bad,
        "len past the slot"
    );
    assert_eq!(
        m.run_one(&Sqe::open(0x63, 0, (n - 4) as u64, 8, KORU_O_RDONLY)),
        bad,
        "off + len past the slot"
    );
    assert_eq!(
        m.run_one(&Sqe::open(0x64, m.slot_count(), 0, 8, KORU_O_RDONLY)),
        bad,
        "slot past the arena"
    );

    // PATH_MAX is the boundary: 4095 resolves and fails, 4096 is refused.
    let long: String = std::iter::repeat_n("/a", 2047).collect::<String>() + "/";
    assert_eq!(long.len(), 4095);
    assert_eq!(
        m.open_path(0, &long, KORU_O_RDONLY),
        -(ENOENT.0 as i64),
        "a PATH_MAX-1 path resolves"
    );
    let longer = long + "a";
    assert_eq!(
        m.open_path(0, &longer, KORU_O_RDONLY),
        bad,
        "a PATH_MAX path is refused"
    );
    m.assert_quiesced();
}

#[test]
fn open_flag_matrix() {
    let m = Mapped::shared();
    let bad = -(EINVAL.0 as i64);

    assert_eq!(
        m.open_path(0, "/etc/hostname", 1 << 8),
        bad,
        "an unknown open flag bit"
    );
    assert_eq!(
        m.open_path(0, "/etc/hostname", KORU_O_ACCMODE),
        bad,
        "access mode 3"
    );

    let h = m.open_path(0, "/etc", KORU_O_RDONLY | KORU_O_DIRECTORY);
    assert!(h > 0, "a directory with KORU_O_DIRECTORY");
    assert_eq!(m.close_handle(h as u32), 0);

    assert_eq!(
        m.open_path(0, "/etc/hostname", KORU_O_DIRECTORY),
        -(ENOTDIR.0 as i64)
    );
    assert_eq!(m.open_path(0, "/etc", KORU_O_WRONLY), -(EISDIR.0 as i64));
    // Since T18 OPEN gates on nothing: READ and WRITE carry the type rule.
    let d = m.open_path(0, "/dev/null", KORU_O_RDONLY);
    assert!(d > 0, "a device node yields a handle");
    assert_eq!(m.read_into(d as u32, 1, 0, 64), bad, "but READ refuses it");
    assert_eq!(m.close_handle(d as u32), 0);

    let link = "/tmp/koru-check-rs-symlink";
    let _ = std::fs::remove_file(link);
    if std::os::unix::fs::symlink("/etc/hostname", link).is_ok() {
        let h = m.open_path(0, link, KORU_O_RDONLY);
        assert!(h > 0, "a symlink is followed by default");
        assert_eq!(m.close_handle(h as u32), 0);
        assert_eq!(m.open_path(0, link, KORU_O_NOFOLLOW), -(ELOOP.0 as i64));
        let _ = std::fs::remove_file(link);
    } else {
        skip("open_flag_matrix symlink", "could not create the symlink");
    }
    m.assert_quiesced();
}

#[test]
fn open_holds_its_slot_only_for_the_path_snapshot() {
    let m = Mapped::shared();
    let len = m.put_path(1, "/etc/hostname");
    let sq = [
        Sqe::checksum(0xa0, 1, 0, m.slot_size()),
        Sqe::open(0xa1, 1, 0, len, KORU_O_RDONLY),
    ];
    let mut cq = [Cqe::default(); 2];
    let r = m.ring.enter(&sq, &mut cq, 2, None).expect("ENTER");
    assert_eq!(r.progress.completed, 2);
    assert!(find_cqe(&cq, 0xa0).res >= 0, "the CHECKSUM wins the slot");
    assert_eq!(
        find_cqe(&cq, 0xa1).res,
        -(EBUSY.0 as i64),
        "the OPEN is refused it"
    );
    m.assert_quiesced();
}

#[test]
fn open_exhausts_the_handle_table_with_emfile() {
    let m = Mapped::new(&SetupConfig::new(32, 64, 8192, 4, 8));
    let n = m.handle_count();
    let held: Vec<i64> = (0..n)
        .map(|_| m.open_path(0, "/etc/hostname", KORU_O_RDONLY))
        .collect();
    assert!(
        held.iter().all(|&h| h > 0),
        "the whole table can be held at once"
    );
    assert_eq!(
        m.open_path(0, "/etc/hostname", KORU_O_RDONLY),
        -(EMFILE.0 as i64),
        "one past it"
    );

    let retired = held[3] as u32;
    assert_eq!(m.close_handle(retired), 0);
    let fresh = m.open_path(0, "/etc/hostname", KORU_O_RDONLY);
    assert!(fresh > 0);
    assert_eq!(
        handle_index(fresh as u32),
        handle_index(retired),
        "the index is reused"
    );
    assert_eq!(
        m.close_handle(retired),
        -(EBADF.0 as i64),
        "the retired handle stays stale"
    );
    m.assert_quiesced();
}

#[test]
fn handles_five_hundred_open_close_pairs_leak_nothing() {
    let fds_before = count_fds();
    let files_before = file_nr_settled();
    let m = Mapped::shared();

    for _ in 0..500 {
        let h = m.open_path(0, "/etc/hostname", KORU_O_RDONLY);
        assert!(h > 0);
        assert_eq!(m.close_handle(h as u32), 0);
    }
    drop(m);

    // filp_open installs no descriptor, so /proc/<pid>/fd sees nothing either
    // way; field 1 of file-nr is the instrument that can.
    assert_eq!(count_fds(), fds_before, "/proc/self/fd is unchanged");
    let leaked = file_nr_settled() - files_before;
    assert!(leaked < 64, "{leaked} struct files leaked");
}

#[test]
fn handles_release_drains_the_table_at_close_not_after_the_delay() {
    let before = file_nr_settled();
    let m = Mapped::new(&SetupConfig::new(32, 64, 8192, 4, 8));
    let len = m.put_path(0, "/etc/hostname");

    let sq = [
        Sqe::delay_ns(0xb0, 2000 * MS),
        Sqe::open(0xb1, 0, 0, len, KORU_O_RDONLY),
    ];
    let mut cq = [Cqe::default(); 4];
    let r = m.ring.enter(&sq, &mut cq, 1, None).expect("ENTER");
    assert_eq!(r.consumed, 2);
    assert_eq!(
        r.progress.completed, 1,
        "the OPEN lands, the delay is still queued"
    );
    assert!(
        find_cqe(&cq[..1], 0xb1).res > 0,
        "a handle opened with a 2s delay still queued"
    );

    let during = file_nr_settled();
    assert!(
        during >= before + 2,
        "the handle and the ring fd are both counted"
    );

    let t0 = Instant::now();
    drop(m);
    let dt = t0.elapsed().as_millis() as u64;
    assert!(
        dt < 500,
        "close took {dt}ms: the delay was waited out, not cancelled"
    );
    assert!(
        file_nr_settled() <= before,
        "closing the ring drops the handle at once"
    );
}

// ---------------------------------------------------------------------------
// T9 - credentials
// ---------------------------------------------------------------------------

/// nobody's uid and gid from /etc/passwd, so the forked child needs no
/// allocating libc call of its own.
fn nobody_ids() -> (u32, u32) {
    std::fs::read_to_string("/etc/passwd")
        .ok()
        .and_then(|s| {
            s.lines().find(|l| l.starts_with("nobody:")).and_then(|l| {
                let f: Vec<&str> = l.split(':').collect();
                Some((f.get(2)?.parse().ok()?, f.get(3)?.parse().ok()?))
            })
        })
        .unwrap_or((65534, 65534))
}

#[test]
fn creds_an_unprivileged_submitter_gets_eacces() {
    // OPEN runs inline in the submitting task, so the check uses the caller's
    // credentials. Deferred to a kworker it would resolve as root.
    if !is_root() {
        skip("creds", "not root");
        return;
    }
    let shadow = std::fs::metadata("/etc/shadow");
    match shadow {
        Ok(md) => {
            use std::os::unix::fs::PermissionsExt;
            if md.permissions().mode() & 0o004 != 0 {
                skip("creds", "/etc/shadow is world-readable");
                return;
            }
        }
        Err(_) => {
            skip("creds", "no /etc/shadow");
            return;
        }
    }

    let (uid, gid) = nobody_ids();
    // The arena is VM_DONTCOPY, so the parent must not map: the child maps it
    // itself after the fork.
    let ring = Ring::with_config(&SetupConfig::new(32, 64, 8192, 4, 8)).expect("SETUP");

    let pid = unsafe { sys::fork() };
    assert!(pid >= 0, "fork");
    if pid == 0 {
        let code = unsafe {
            match ring.mmap() {
                Err(_) => 2,
                Ok(arena) => {
                    sys::setgroups(0, std::ptr::null());
                    if sys::setresgid(gid, gid, gid) != 0 || sys::setresuid(uid, uid, uid) != 0 {
                        3
                    } else if sys::geteuid() == 0 {
                        4
                    } else {
                        let m = Mapped { ring, arena };
                        if m.open_path(0, "/etc/shadow", KORU_O_RDONLY) != -(EACCES.0 as i64) {
                            5
                        } else if m.open_path(0, "/etc/hostname", KORU_O_RDONLY) <= 0 {
                            6
                        } else {
                            0
                        }
                    }
                }
            }
        };
        unsafe { sys::_exit(code) };
    }

    let mut status = 0;
    unsafe { sys::waitpid(pid, &mut status, 0) };
    assert!(sys::wifexited(status), "the child died");
    // 2 mmap, 3 setresuid, 4 still root, 5 shadow not EACCES, 6 hostname failed.
    assert_eq!(sys::wexitstatus(status), 0, "child verdict");
}

// ---------------------------------------------------------------------------
// T10 - READ into a slot
// ---------------------------------------------------------------------------

#[test]
fn read_matches_read_2_byte_for_byte() {
    let m = Mapped::shared();
    let want = match std::fs::read("/etc/hostname") {
        Ok(v) if !v.is_empty() => v,
        _ => {
            skip("read", "/etc/hostname is unreadable or empty");
            return;
        }
    };
    let h = m.open_path(0, "/etc/hostname", KORU_O_RDONLY);
    assert!(h > 0);
    let n = m.read_into(h as u32, 1, 0, m.slot_size());
    assert_eq!(n, want.len() as i64);
    assert_eq!(&m.slot(1)[..n as usize], &want[..]);
    assert_eq!(m.close_handle(h as u32), 0);
    m.assert_quiesced();
}

#[test]
fn read_reproduces_the_pattern_at_any_offset() {
    ensure_pattern_file();
    let m = Mapped::shared();
    let n = m.slot_size();
    let h = m.open_path(0, PATFILE, KORU_O_RDONLY) as u32;
    assert!(h > 0);

    assert_eq!(m.read_into(h, 1, 0, n), PATSIZE as i64, "a multi-page READ");
    assert!(
        m.slot(1)
            .iter()
            .enumerate()
            .all(|(i, &b)| b == pattern_byte(i))
    );

    let off = 4097u64;
    let len = n - off as u32;
    assert_eq!(
        m.read_into(h, 2, off, len),
        len as i64,
        "an unaligned file offset"
    );
    assert!(
        m.slot(2)[..len as usize]
            .iter()
            .enumerate()
            .all(|(i, &b)| b == pattern_byte(i + off as usize))
    );

    // A short read at EOF is a result, not an error.
    assert_eq!(
        m.read_into(h, 3, (PATSIZE - 100) as u64, n),
        100,
        "a read running past EOF"
    );
    assert!(
        m.slot(3)[..100]
            .iter()
            .enumerate()
            .all(|(i, &b)| b == pattern_byte(i + PATSIZE - 100))
    );
    assert_eq!(m.read_into(h, 3, PATSIZE as u64, n), 0, "a read at EOF");
    assert_eq!(
        m.read_into(h, 3, (PATSIZE + 4096) as u64, n),
        0,
        "a read past EOF"
    );
    assert_eq!(m.read_into(h, 4, 123, 1), 1, "a one-byte read");
    assert_eq!(m.slot(4)[0], pattern_byte(123));

    assert_eq!(m.close_handle(h), 0);
    m.assert_quiesced();
}

#[test]
fn read_rejection_matrix() {
    ensure_pattern_file();
    let m = Mapped::shared();
    let n = m.slot_size();
    let h = m.open_path(0, PATFILE, KORU_O_RDONLY) as u32;
    let ebadf = -(EBADF.0 as i64);
    let bad = -(EINVAL.0 as i64);

    assert_eq!(m.read_into(0, 1, 0, n), ebadf, "handle 0");
    assert_eq!(
        m.read_into(h + (1 << 16), 1, 0, n),
        ebadf,
        "a stale generation"
    );
    assert_eq!(
        m.read_into(make_handle(m.handle_count() as u16, 1), 1, 0, n),
        ebadf,
        "an index past the table"
    );
    assert_eq!(m.read_into(h, 1, 0, 0), bad, "len 0");
    assert_eq!(m.read_into(h, 1, 0, n + 1), bad, "len past the slot");
    assert_eq!(
        m.read_into(h, m.slot_count(), 0, n),
        bad,
        "slot past the arena"
    );
    assert_eq!(m.read_into(h, 1, 1 << 63, n), bad, "a negative file offset");

    // READ holds its slot for the whole deferred op.
    let sq = [Sqe::checksum(0x70, 5, 0, n), Sqe::read(0x71, h, 5, 0, n)];
    let mut cq = [Cqe::default(); 2];
    let r = m.ring.enter(&sq, &mut cq, 2, None).expect("ENTER");
    assert_eq!(r.progress.completed, 2);
    assert_eq!(
        find_cqe(&cq, 0x71).res,
        -(EBUSY.0 as i64),
        "two ops on one slot"
    );

    assert_eq!(m.close_handle(h), 0);
    assert_eq!(m.read_into(h, 1, 0, n), ebadf, "a closed handle");

    // Rejected before kernel_read can warn; the gate script greps for that text.
    let d = m.open_path(0, "/etc", KORU_O_RDONLY | KORU_O_DIRECTORY) as u32;
    assert!(d > 0);
    assert_eq!(m.read_into(d, 1, 0, n), bad, "READ of a directory");
    assert_eq!(m.close_handle(d), 0);
    m.assert_quiesced();
}

#[test]
fn read_race_four_hundred_closes_against_an_in_flight_read() {
    // The ARef<File> is resolved at submit time, so a CLOSE racing the op
    // cannot free it. 64 KB slots make the window the common case.
    ensure_pattern_file();
    arm_alarm(120);
    let m = Mapped::shared();
    let n = m.slot_size();
    let mut close_first = 0;

    for round in 0..400 {
        let h = m.open_path(0, PATFILE, KORU_O_RDONLY) as u32;
        assert!(h > 0, "round {round}");
        let sq = [Sqe::read(0xb0, h, 1, 0, n), Sqe::close(0xb1, h)];
        let mut cq = [Cqe::default(); 2];
        let r = m.ring.enter(&sq, &mut cq, 2, None).expect("ENTER");
        assert_eq!(r.progress.completed, 2, "round {round}: C1");
        assert_eq!(
            find_cqe(&cq, 0xb0).res,
            PATSIZE as i64,
            "round {round}: the READ"
        );
        assert_eq!(find_cqe(&cq, 0xb1).res, 0, "round {round}: the CLOSE");
        if cq[0].user_data == 0xb1 {
            close_first += 1;
        }
    }
    disarm_alarm();

    assert!(
        m.slot(1)
            .iter()
            .enumerate()
            .all(|(i, &b)| b == pattern_byte(i)),
        "the last one still read the right bytes"
    );
    assert!(
        close_first >= 350,
        "the CLOSE landed first only {close_first} times of 400"
    );
    m.assert_quiesced();
}

// ---------------------------------------------------------------------------
// T17 - WRITE
// ---------------------------------------------------------------------------

/// A fresh writable file per test, removed when the guard drops.
struct Scratch(String);

impl Scratch {
    fn new(tag: &str) -> Scratch {
        let path = format!("{WRFILE}-{tag}");
        std::fs::write(&path, b"").expect("scratch file");
        Scratch(path)
    }

    fn path(&self) -> &str {
        &self.0
    }

    fn bytes(&self) -> Vec<u8> {
        std::fs::read(&self.0).expect("read back")
    }
}

impl Drop for Scratch {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.0);
    }
}

#[test]
fn write_lands_byte_for_byte() {
    let f = Scratch::new("bytes");
    let m = Mapped::shared();
    let n = m.slot_size();
    let h = m.open_path(0, f.path(), KORU_O_WRONLY) as u32;
    assert!(h > 0, "OPEN for writing");

    m.fill(1, |j| pattern_byte(j));
    assert_eq!(m.write_from(h, 1, 0, n), i64::from(n), "a multi-page WRITE");

    let got = f.bytes();
    assert_eq!(got.len(), n as usize);
    for (j, b) in got.iter().enumerate() {
        assert_eq!(*b, pattern_byte(j), "byte {j}");
    }
    assert_eq!(m.close_handle(h), 0);
    m.assert_quiesced();
}

#[test]
fn write_places_bytes_at_any_offset() {
    let f = Scratch::new("offset");
    let m = Mapped::shared();
    let h = m.open_path(0, f.path(), KORU_O_WRONLY) as u32;
    assert!(h > 0);

    // Into a hole: the file is empty, so everything below stays sparse.
    let off = 1u64 << 20;
    m.fill(1, |j| pattern_byte(j + off as usize));
    assert_eq!(m.write_from(h, 1, off, 4096), 4096);
    assert_eq!(m.write_from(h, 1, 4097, 1), 1, "a one-byte WRITE");

    let got = f.bytes();
    assert_eq!(
        got.len(),
        off as usize + 4096,
        "the file grew to the offset"
    );
    for j in 0..4096usize {
        assert_eq!(got[off as usize + j], pattern_byte(j + off as usize), "{j}");
    }
    assert_eq!(got[4097], pattern_byte(off as usize), "the odd-offset byte");
    assert_eq!(got[0], 0, "the hole below it reads as zero");
    assert_eq!(m.close_handle(h), 0);
    m.assert_quiesced();
}

#[test]
fn write_rejection_matrix() {
    let f = Scratch::new("reject");
    let m = Mapped::shared();
    let n = m.slot_size();
    let h = m.open_path(0, f.path(), KORU_O_WRONLY) as u32;
    assert!(h > 0);
    let ebadf = -(EBADF.0 as i64);
    let bad = -(EINVAL.0 as i64);

    assert_eq!(m.write_from(0, 1, 0, n), ebadf, "handle 0");
    assert_eq!(
        m.write_from(h + (1 << 16), 1, 0, n),
        ebadf,
        "a stale generation"
    );
    assert_eq!(
        m.write_from(make_handle(m.handle_count() as u16, 1), 1, 0, n),
        ebadf,
        "an index past the table"
    );
    assert_eq!(m.write_from(h, 1, 0, 0), bad, "len 0");
    assert_eq!(m.write_from(h, 1, 0, n + 1), bad, "len past the slot");
    assert_eq!(
        m.write_from(h, m.slot_count(), 0, n),
        bad,
        "slot past the arena"
    );
    assert_eq!(
        m.write_from(h, 1, 1 << 63, n),
        bad,
        "a negative file offset"
    );

    // No FMODE_WRITE on a read-only handle.
    let ro = m.open_path(0, f.path(), KORU_O_RDONLY) as u32;
    assert!(ro > 0);
    assert_eq!(m.write_from(ro, 1, 0, n), ebadf, "a read-only handle");
    assert_eq!(m.close_handle(ro), 0);

    // WRITE holds its slot for the whole deferred op.
    let sq = [Sqe::checksum(0x70, 5, 0, n), Sqe::write(0x71, h, 5, 0, n)];
    let mut cq = [Cqe::default(); 2];
    let r = m.ring.enter(&sq, &mut cq, 2, None).expect("ENTER");
    assert_eq!(r.progress.completed, 2);
    assert_eq!(
        find_cqe(&cq, 0x71).res,
        -(EBUSY.0 as i64),
        "two ops on one slot"
    );

    assert_eq!(m.close_handle(h), 0);
    assert_eq!(m.write_from(h, 1, 0, n), ebadf, "a closed handle");
    m.assert_quiesced();
}

/// A cancelled WRITE must release its slot, which is the only thing that says
/// KORU_OP_WRITE reached OpWork::held_slot.
#[test]
fn write_releases_its_slot_when_cancelled() {
    let f = Scratch::new("cancel");
    let m = Mapped::shared();
    let n = m.slot_size();
    let h = m.open_path(0, f.path(), KORU_O_WRONLY) as u32;
    assert!(h > 0);

    m.fill(6, |j| pattern_byte(j));
    let r = m
        .ring
        .enter(&[Sqe::write(0x60, h, 6, 0, n)], &mut [], 0, None)
        .expect("ENTER");
    assert_eq!(r.consumed, 1, "the WRITE is queued");

    let mut cq = [Cqe::default(); 4];
    let r = m
        .ring
        .enter(&[Sqe::cancel(0x61, 0x60)], &mut cq, 2, None)
        .expect("ENTER");
    assert_eq!(r.progress.completed, 2, "both complete");

    // Not -EBUSY: the slot came back whether the cancel won or the write ran.
    assert!(
        m.run_one(&Sqe::checksum(0x62, 6, 0, n)) >= 0,
        "the cancelled WRITE never released its slot"
    );
    assert_eq!(m.close_handle(h), 0);
    m.assert_quiesced();
}

// ---------------------------------------------------------------------------
// T18 - KORU_O_NONBLOCK
// ---------------------------------------------------------------------------

/// A FIFO removed when the guard drops.
struct Fifo(String);

impl Fifo {
    fn new(tag: &str) -> Option<Fifo> {
        let path = format!("/tmp/koru-check-rs-fifo-{tag}");
        let _ = std::fs::remove_file(&path);
        let c = std::ffi::CString::new(path.as_str()).expect("path");
        // SAFETY: `c` is NUL-terminated and lives across the call.
        if unsafe { sys::mkfifo(c.as_ptr(), 0o600) } != 0 {
            return None;
        }
        Some(Fifo(path))
    }

    fn path(&self) -> &str {
        &self.0
    }

    /// Both peers at once, so nothing below blocks and no fork is needed.
    fn peer(&self) -> std::fs::File {
        use std::os::unix::fs::OpenOptionsExt;
        std::fs::OpenOptions::new()
            .read(true)
            .write(true)
            .custom_flags(0o4000) // O_NONBLOCK
            .open(&self.0)
            .expect("peer")
    }
}

impl Drop for Fifo {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.0);
    }
}

/// Without the flag this blocks in `filp_open` for ever, so the alarm is the
/// only way out and a failure here is a hang.
#[test]
fn nonblock_opens_a_peerless_fifo_at_once() {
    arm_alarm(30);
    let Some(f) = Fifo::new("peerless") else {
        skip("nonblock_opens_a_peerless_fifo_at_once", "cannot mkfifo");
        return;
    };
    let m = Mapped::shared();

    let t0 = Instant::now();
    let h = m.open_path(0, f.path(), KORU_O_RDONLY | KORU_O_NONBLOCK);
    assert!(h > 0, "a peerless FIFO opens non-blocking");
    assert!(t0.elapsed() < Duration::from_millis(500), "it waited");

    // No writer is EOF, not EAGAIN: pipe_read checks writers first.
    assert_eq!(m.read_into(h as u32, 1, 0, 64), 0, "no writer is 0");
    assert_eq!(m.close_handle(h as u32), 0);

    assert_eq!(
        m.open_path(0, f.path(), KORU_O_WRONLY | KORU_O_NONBLOCK),
        -(ENXIO.0 as i64),
        "write-only with no reader"
    );
    disarm_alarm();
    m.assert_quiesced();
}

#[test]
fn nonblock_read_and_write_a_fifo() {
    arm_alarm(30);
    let Some(f) = Fifo::new("rw") else {
        skip("nonblock_read_and_write_a_fifo", "cannot mkfifo");
        return;
    };
    let mut peer = f.peer();
    let m = Mapped::shared();

    let h = m.open_path(0, f.path(), KORU_O_RDONLY | KORU_O_NONBLOCK);
    assert!(h > 0);
    assert_eq!(
        m.read_into(h as u32, 1, 0, 64),
        -(EAGAIN.0 as i64),
        "a writer exists and the pipe is empty"
    );

    peer.write_all(b"koru").expect("peer write");
    assert_eq!(m.read_into(h as u32, 1, 0, 64), 4);
    assert_eq!(&m.slot(1)[..4], b"koru");

    // A FIFO has no FMODE_LSEEK, so `off` names nothing on it.
    assert_eq!(
        m.read_into(h as u32, 1, 1, 64),
        -(EINVAL.0 as i64),
        "a non-zero off on an unseekable READ"
    );
    assert_eq!(m.close_handle(h as u32), 0);

    let w = m.open_path(0, f.path(), KORU_O_WRONLY | KORU_O_NONBLOCK);
    assert!(w > 0);
    m.slot(2)[..4].copy_from_slice(b"ring");
    assert_eq!(m.write_from(w as u32, 2, 0, 4), 4);
    let mut buf = [0u8; 8];
    assert_eq!(peer.read(&mut buf).expect("peer read"), 4);
    assert_eq!(&buf[..4], b"ring");
    assert_eq!(
        m.write_from(w as u32, 2, 1, 4),
        -(EINVAL.0 as i64),
        "a non-zero off on an unseekable WRITE"
    );
    assert_eq!(m.close_handle(w as u32), 0);
    disarm_alarm();
    m.assert_quiesced();
}

/// The gate lives on READ and WRITE, not on OPEN. Delete it and this hangs in a
/// kworker rather than failing.
#[test]
fn a_blocking_handle_to_a_fifo_is_refused_by_read() {
    arm_alarm(30);
    let Some(f) = Fifo::new("gate") else {
        skip(
            "a_blocking_handle_to_a_fifo_is_refused_by_read",
            "cannot mkfifo",
        );
        return;
    };
    let _peer = f.peer();
    let m = Mapped::shared();

    let h = m.open_path(0, f.path(), KORU_O_RDONLY);
    assert!(h > 0, "the peer makes a blocking open possible");
    assert_eq!(
        m.read_into(h as u32, 1, 0, 64),
        -(EINVAL.0 as i64),
        "READ without the flag"
    );
    assert_eq!(m.close_handle(h as u32), 0);
    disarm_alarm();
    m.assert_quiesced();
}

/// Non-blocking clears the file-type check, so only the `f_op` shape guard is
/// left to reject a directory.
#[test]
fn the_f_op_guard_rejects_a_directory_on_its_own() {
    let m = Mapped::shared();
    let h = m.open_path(
        0,
        "/etc",
        KORU_O_RDONLY | KORU_O_DIRECTORY | KORU_O_NONBLOCK,
    );
    assert!(h > 0, "a directory opens non-blocking");
    assert_eq!(m.read_into(h as u32, 1, 0, 64), -(EINVAL.0 as i64));
    assert_eq!(m.close_handle(h as u32), 0);
    m.assert_quiesced();
}

// ---------------------------------------------------------------------------
// T19 - ADOPT_FD
// ---------------------------------------------------------------------------

#[test]
fn adopt_gives_a_handle_that_outlives_the_descriptor() {
    ensure_pattern_file();
    let m = Mapped::shared();

    let f = std::fs::File::open(PATFILE).expect("open");
    let fd = f.as_raw_fd();
    let h = m.adopt(fd);
    assert!(h > 0, "adopt a descriptor");

    // The reference is ours: an fget takes a real one.
    drop(f);
    assert_eq!(m.read_into(h as u32, 1, 0, 64), 64, "READ after close(2)");
    for (j, b) in m.slot(1)[..64].iter().enumerate() {
        assert_eq!(*b, pattern_byte(j), "byte {j}");
    }
    assert_eq!(m.close_handle(h as u32), 0);
    m.assert_quiesced();
}

/// Adopting a koru fd would put an `Arc<RingCtx>` in a ring's own handle table,
/// so `release` would never run and the module would never unload.
#[test]
fn adopt_refuses_any_koru_descriptor() {
    let m = Mapped::shared();
    let eloop = -(ELOOP.0 as i64);

    assert_eq!(m.adopt(m.ring.as_raw_fd()), eloop, "our own ring");

    let other = Ring::with_config(&SetupConfig::new(32, 64, 4096, 4, 8)).expect("SETUP");
    assert_eq!(m.adopt(other.as_raw_fd()), eloop, "a second ring");
    m.assert_quiesced();
}

#[test]
fn adopt_rejection_matrix() {
    ensure_pattern_file();
    let m = Mapped::shared();
    let bad = -(EINVAL.0 as i64);
    let ebadf = -(EBADF.0 as i64);

    assert_eq!(m.adopt(9999), ebadf, "an out-of-range fd");
    let fd = {
        let f = std::fs::File::open(PATFILE).expect("open");
        f.as_raw_fd()
    };
    assert_eq!(m.adopt(fd), ebadf, "a closed fd");

    let mut s = Sqe::adopt_fd(0x400, 0);
    s.off = i32::MAX as u64 + 1;
    assert_eq!(m.run_one(&s), bad, "an fd past INT32_MAX");
    for (mutate, what) in [
        ((|s: &mut Sqe| s.len = 1) as fn(&mut Sqe), "a non-zero len"),
        (|s: &mut Sqe| s.slot = 1, "a non-zero slot"),
        (|s: &mut Sqe| s.handle = 1, "a non-zero handle"),
    ] {
        let mut s = Sqe::adopt_fd(0x401, 0);
        mutate(&mut s);
        assert_eq!(m.run_one(&s), bad, "{what}");
    }
    m.assert_quiesced();
}

/// The mirror of `OPEN`'s creds test, asserting the opposite outcome. `fget`
/// checks nothing, so a task that already holds the descriptor keeps it. If
/// this ever starts failing, something began re-checking at use time.
#[test]
fn creds_an_unprivileged_child_adopts_a_root_only_fd() {
    if !is_root() {
        skip("adopt creds", "not root");
        return;
    }
    let Ok(shadow) = std::fs::File::open("/etc/shadow") else {
        skip("adopt creds", "no /etc/shadow");
        return;
    };
    let (uid, gid) = nobody_ids();
    let fd = shadow.as_raw_fd();
    // VM_DONTCOPY: the parent must not map, the child maps after the fork.
    let ring = Ring::with_config(&SetupConfig::new(32, 64, 8192, 4, 8)).expect("SETUP");

    let pid = unsafe { sys::fork() };
    assert!(pid >= 0, "fork");
    if pid == 0 {
        let code = unsafe {
            match ring.mmap() {
                Err(_) => 2,
                Ok(arena) => {
                    sys::setgroups(0, std::ptr::null());
                    if sys::setresgid(gid, gid, gid) != 0 || sys::setresuid(uid, uid, uid) != 0 {
                        3
                    } else if sys::geteuid() == 0 {
                        4
                    } else {
                        let m = Mapped { ring, arena };
                        // OPEN would be EACCES; ADOPT_FD must not be.
                        if m.open_path(0, "/etc/shadow", KORU_O_RDONLY) != -(EACCES.0 as i64) {
                            5
                        } else if m.adopt(fd) <= 0 {
                            6
                        } else {
                            0
                        }
                    }
                }
            }
        };
        unsafe { sys::_exit(code) };
    }

    let mut status = 0;
    unsafe { sys::waitpid(pid, &mut status, 0) };
    assert!(sys::wifexited(status), "the child died");
    // 2 mmap, 3 setresuid, 4 still root, 5 OPEN not EACCES, 6 adopt failed.
    assert_eq!(sys::wexitstatus(status), 0, "child verdict");
}

// ---------------------------------------------------------------------------
// T11 - CANCEL
// ---------------------------------------------------------------------------

#[test]
fn cancel_a_queued_delay_returns_at_once() {
    let m = Mapped::shared();
    let r = m
        .ring
        .enter(&[Sqe::delay_ns(0x10, 2000 * MS)], &mut [], 0, None)
        .expect("ENTER");
    assert_eq!(r.consumed, 1, "a 2s DELAY_NS is queued");

    let mut cq = [Cqe::default(); 4];
    let t0 = Instant::now();
    let r = m
        .ring
        .enter(&[Sqe::cancel(0x11, 0x10)], &mut cq, 2, None)
        .expect("ENTER");
    let dt = t0.elapsed().as_millis() as u64;

    assert_eq!(r.consumed, 1);
    assert_eq!(
        r.progress.completed, 2,
        "the CANCEL and its target both complete (C1)"
    );
    assert_eq!(find_cqe(&cq, 0x11).res, 0, "the canceller");
    assert_eq!(find_cqe(&cq, 0x10).res, -(ECANCELED.0 as i64), "the target");
    assert!(dt < 500, "took {dt}ms: the delay was waited out");

    assert_eq!(
        m.run_one(&Sqe::cancel(0x12, 0x10)),
        -(ENOENT.0 as i64),
        "a second cancel"
    );
    m.assert_quiesced();
}

#[test]
fn cancel_of_something_not_in_flight_is_enoent() {
    let m = Mapped::shared();
    let enoent = -(ENOENT.0 as i64);
    assert_eq!(
        m.run_one(&Sqe::cancel(0x20, 0xdeadbeef)),
        enoent,
        "no such op"
    );
    assert_eq!(m.run_one(&Sqe::nop(0x21)), 0);
    assert_eq!(
        m.run_one(&Sqe::cancel(0x22, 0x21)),
        enoent,
        "an op that already completed"
    );
    m.assert_quiesced();
}

#[test]
fn cancel_rejects_fields_it_does_not_read() {
    let m = Mapped::shared();
    let bad = -(EINVAL.0 as i64);
    assert_eq!(
        m.run_one(&Sqe {
            len: 1,
            ..Sqe::cancel(0x30, 1)
        }),
        bad,
        "len"
    );
    assert_eq!(
        m.run_one(&Sqe {
            slot: 1,
            ..Sqe::cancel(0x31, 1)
        }),
        bad,
        "slot"
    );
    assert_eq!(
        m.run_one(&Sqe {
            handle: 1,
            ..Sqe::cancel(0x32, 1)
        }),
        bad,
        "handle"
    );
    m.assert_quiesced();
}

#[test]
fn cancel_releases_the_slot_the_target_held() {
    let m = Mapped::shared();
    let n = m.slot_size();
    let r = m
        .ring
        .enter(&[Sqe::checksum(0x40, 2, 0, n)], &mut [], 0, None)
        .expect("ENTER");
    assert_eq!(r.consumed, 1);

    let mut cq = [Cqe::default(); 4];
    let r = m
        .ring
        .enter(&[Sqe::cancel(0x41, 0x40)], &mut cq, 2, None)
        .expect("ENTER");
    assert_eq!(r.progress.completed, 2);
    assert!(
        m.run_one(&Sqe::checksum(0x42, 2, 0, n)) >= 0,
        "the slot was released"
    );
    m.assert_quiesced();
}

/// Race a CANCEL against its target. Returns (cancelled, already, not_found).
fn cancel_race(m: &Mapped, delay_ns: u64, rounds: u32) -> (u32, u32, u32) {
    let (mut ok, mut already, mut enoent) = (0, 0, 0);
    for round in 0..rounds {
        let r = m
            .ring
            .enter(&[Sqe::delay_ns(0x30, delay_ns)], &mut [], 0, None)
            .expect("ENTER");
        assert_eq!(r.consumed, 1, "round {round}");

        let mut cq = [Cqe::default(); 4];
        let r = m
            .ring
            .enter(&[Sqe::cancel(0x31, 0x30)], &mut cq, 2, None)
            .expect("ENTER");
        assert_eq!(r.consumed, 1, "round {round}");
        assert_eq!(
            r.progress.completed, 2,
            "round {round}: every SQE yields exactly one CQE"
        );

        match find_cqe(&cq, 0x31).res {
            0 => ok += 1,
            v if v == -(EALREADY.0 as i64) => already += 1,
            v if v == -(ENOENT.0 as i64) => enoent += 1,
            v => panic!("round {round}: the canceller returned {v}"),
        }
        let target = find_cqe(&cq, 0x30).res;
        assert!(
            target == 0 || target == -(ECANCELED.0 as i64),
            "round {round}: the target returned {target}"
        );
    }
    assert_eq!(ok + already + enoent, rounds, "every round accounted for");
    (ok, already, enoent)
}

#[test]
fn cancel_race_against_an_armed_timer() {
    arm_alarm(120);
    let m = Mapped::shared();
    let (ok, _, _) = cancel_race(&m, 2 * MS, 300);
    disarm_alarm();
    assert!(ok >= 250, "the cancel won only {ok} of 300");
    m.assert_quiesced();
}

#[test]
fn cancel_race_against_the_worker() {
    arm_alarm(120);
    let m = Mapped::shared();
    let (_, _, enoent) = cancel_race(&m, 0, 300);
    disarm_alarm();
    assert!(enoent >= 50, "the worker won only {enoent} of 300");
    m.assert_quiesced();
}

#[test]
fn cancel_race_against_a_running_checksum() {
    // run() unregisters after complete(), so a cancel during execution reports
    // -EALREADY rather than -ENOENT. A whole-slot 64 KB CHECKSUM is what makes
    // that window reachable at all; it is counted but not gated, because it
    // lands one to three times per thousand.
    arm_alarm(180);
    let m = Mapped::shared();
    let n = m.slot_size();
    let (mut ok, mut already, mut enoent) = (0, 0, 0);

    for round in 0..1000 {
        let r = m
            .ring
            .enter(&[Sqe::checksum(0x50, 3, 0, n)], &mut [], 0, None)
            .expect("ENTER");
        assert_eq!(r.consumed, 1, "round {round}");
        let mut cq = [Cqe::default(); 4];
        let r = m
            .ring
            .enter(&[Sqe::cancel(0x51, 0x50)], &mut cq, 2, None)
            .expect("ENTER");
        assert_eq!(r.progress.completed, 2, "round {round}: C1 holds");
        match find_cqe(&cq, 0x51).res {
            0 => ok += 1,
            v if v == -(EALREADY.0 as i64) => already += 1,
            v if v == -(ENOENT.0 as i64) => enoent += 1,
            v => panic!("round {round}: the canceller returned {v}"),
        }
    }
    disarm_alarm();
    println!("    cancel vs running CHECKSUM: {ok} cancelled, {already} already, {enoent} missed");
    assert!(ok >= 150, "the cancel won only {ok} of 1000");
    assert!(enoent >= 20, "the CHECKSUM won only {enoent} of 1000");
    m.assert_quiesced();
}

#[test]
fn cancel_leak_two_hundred_open_read_cancel_close_rounds() {
    // Skipping the reference drop CANCEL owes on a true return leaks the op and
    // wedges rmmod; doing it on a false return is a use-after-free.
    ensure_pattern_file();
    arm_alarm(120);
    let before = file_nr_settled();
    let m = Mapped::shared();
    let n = m.slot_size();

    for round in 0..200 {
        let h = m.open_path(0, PATFILE, KORU_O_RDONLY) as u32;
        assert!(h > 0, "round {round}");
        let r = m
            .ring
            .enter(&[Sqe::read(0x60, h, 1, 0, n)], &mut [], 0, None)
            .expect("ENTER");
        assert_eq!(r.consumed, 1, "round {round}");
        let mut cq = [Cqe::default(); 4];
        let r = m
            .ring
            .enter(&[Sqe::cancel(0x61, 0x60)], &mut cq, 2, None)
            .expect("ENTER");
        assert_eq!(r.progress.completed, 2, "round {round}");
        assert_eq!(m.close_handle(h), 0, "round {round}");
    }
    disarm_alarm();
    m.assert_quiesced();
    drop(m);

    let leaked = file_nr_settled() - before;
    assert!(
        leaked < 64,
        "{leaked} struct files leaked: an OpWork kept its file reference"
    );
}

// ---------------------------------------------------------------------------
// T3 - SETUP and GET_PARAMS
// ---------------------------------------------------------------------------
//
// Not strictly T4-T11, but T13 owns the ioctl numbers and the typed wrappers,
// and a wrong direction bit is the likeliest defect in a hand-written _IOWR.

fn good_request() -> KoruParams {
    SetupConfig::new(64, 128, 4096, 32, 0).to_params()
}

#[test]
fn setup_get_params_works_before_setup() {
    let r = Ring::open().expect("open");
    let p = r.get_params().expect("GET_PARAMS before SETUP");
    assert_eq!(p.magic, KORU_MAGIC);
    assert_eq!(p.abi_version, KORU_ABI_VERSION);
    assert_eq!(p.configured, 0);
    assert_eq!(p.handle_count, 0);
    // Exact, not merely non-zero. scripts/abi.sh diffs the two userspace
    // mirrors against each other and cannot see the kernel; this is the only
    // assertion tying either of them to the canonical kernel/koru_abi.rs.
    for (got, want, what) in [
        (
            p.max_sq_entries as u64,
            KORU_MAX_SQ_ENTRIES as u64,
            "max_sq_entries",
        ),
        (
            p.max_cq_entries as u64,
            KORU_MAX_CQ_ENTRIES as u64,
            "max_cq_entries",
        ),
        (
            p.max_slot_size as u64,
            KORU_MAX_SLOT_SIZE as u64,
            "max_slot_size",
        ),
        (
            p.max_slot_count as u64,
            KORU_MAX_SLOT_COUNT as u64,
            "max_slot_count",
        ),
        (p.max_arena_bytes, KORU_MAX_ARENA_BYTES, "max_arena_bytes"),
        (p.max_handles as u64, KORU_MAX_HANDLES as u64, "max_handles"),
        (p.max_delay_ns, KORU_MAX_DELAY_NS, "max_delay_ns"),
    ] {
        assert_eq!(got, want, "{what} disagrees with the ABI mirror");
    }
}

#[test]
fn setup_rejection_matrix() {
    // All on one fd: a rejected SETUP must not consume the one shot.
    let mut r = Ring::open().expect("open");
    let caps = r.get_params().expect("GET_PARAMS");

    let cases: Vec<(KoruParams, Errno, &str)> = vec![
        (
            KoruParams {
                magic: 0xdeadbeef,
                ..good_request()
            },
            EPROTO,
            "bad magic is EPROTO, not EINVAL",
        ),
        (
            KoruParams {
                abi_version: 999,
                ..good_request()
            },
            EPROTO,
            "bad abi_version",
        ),
        (
            KoruParams {
                flags: 1,
                ..good_request()
            },
            EINVAL,
            "an unknown SETUP flag bit",
        ),
        (
            KoruParams {
                reserved: [0, 1],
                ..good_request()
            },
            EINVAL,
            "a non-zero reserved word",
        ),
        (
            KoruParams {
                sq_entries: 0,
                ..good_request()
            },
            EINVAL,
            "sq_entries 0",
        ),
        (
            KoruParams {
                sq_entries: caps.max_sq_entries + 1,
                ..good_request()
            },
            EINVAL,
            "sq_entries past the cap",
        ),
        (
            KoruParams {
                cq_entries: caps.max_cq_entries + 1,
                ..good_request()
            },
            EINVAL,
            "cq_entries past the cap",
        ),
        (
            KoruParams {
                cq_entries: 32,
                ..good_request()
            },
            EINVAL,
            "cq_entries below sq_entries",
        ),
        (
            KoruParams {
                slot_size: 0,
                ..good_request()
            },
            EINVAL,
            "slot_size 0",
        ),
        (
            KoruParams {
                slot_size: 100,
                ..good_request()
            },
            EINVAL,
            "slot_size 100",
        ),
        (
            KoruParams {
                slot_size: 4097,
                ..good_request()
            },
            EINVAL,
            "slot_size 4097",
        ),
        (
            KoruParams {
                slot_size: caps.max_slot_size + 1,
                ..good_request()
            },
            EINVAL,
            "slot_size past the cap",
        ),
        (
            KoruParams {
                slot_count: 0,
                ..good_request()
            },
            EINVAL,
            "slot_count 0",
        ),
        (
            KoruParams {
                slot_count: caps.max_slot_count + 1,
                ..good_request()
            },
            EINVAL,
            "slot_count past the cap",
        ),
        (
            KoruParams {
                slot_size: caps.max_slot_size,
                slot_count: caps.max_slot_count,
                ..good_request()
            },
            EINVAL,
            "an arena past the cap is rejected, not clamped",
        ),
        (
            KoruParams {
                handle_count: caps.max_handles + 1,
                ..good_request()
            },
            EINVAL,
            "handle_count past the cap",
        ),
    ];
    for (mut p, want, what) in cases {
        expect_errno(r.setup_raw(&mut p), want, what);
    }

    // The one shot survived all sixteen.
    let mut p = good_request();
    r.setup_raw(&mut p)
        .expect("SETUP still works after every rejection");
    assert_eq!(
        (p.sq_entries, p.cq_entries),
        (64, 128),
        "sq and cq entries round-trip"
    );
    assert_eq!(
        (p.slot_size, p.slot_count),
        (4096, 32),
        "slot size and count round-trip"
    );
    assert_eq!(
        p.arena_size,
        4096 * 32,
        "arena_size is slot_size * slot_count"
    );
    assert_eq!(p.configured, 1);
    assert_eq!(
        p.handle_count, KORU_DEFAULT_HANDLES,
        "handle_count 0 became the default the mirror names"
    );

    let q = r.get_params().expect("GET_PARAMS after SETUP");
    assert_eq!(q, p, "GET_PARAMS returns what SETUP returned");

    let mut again = good_request();
    expect_errno(r.setup_raw(&mut again), EBUSY, "a second SETUP");
}

#[test]
fn setup_zero_fields_take_their_defaults() {
    let mut r = Ring::open().expect("open");
    let mut p = KoruParams {
        cq_entries: 0,
        ..good_request()
    };
    r.setup_raw(&mut p).expect("SETUP");
    assert_eq!(
        p.cq_entries, p.sq_entries,
        "cq_entries 0 becomes sq_entries"
    );

    let mut r = Ring::open().expect("open");
    let mut p = KoruParams {
        handle_count: 7,
        ..good_request()
    };
    r.setup_raw(&mut p).expect("SETUP");
    assert_eq!(p.handle_count, 7, "an explicit handle_count is echoed back");
}

// ---------------------------------------------------------------------------
// T3 - ioctl dispatch
// ---------------------------------------------------------------------------
//
// ENOTTY means no such command; EPROTO means our command with the wrong struct
// size or direction, which is version skew. Keeping them apart is the point.

#[test]
fn ioctl_dispatch_matrix() {
    let r = Ring::with_config(&SetupConfig::new(32, 64, 4096, 8, 8)).expect("SETUP");
    let mut p = KoruParams::default();
    let arg = (&mut p as *mut KoruParams).cast::<c_void>();
    let psize = size_of::<KoruParams>();
    let esize = size_of::<KoruEnter>();
    let rw = sys::_IOC_READ | sys::_IOC_WRITE;

    let call = |req: u32, arg: *mut c_void| unsafe { r.ioctl_raw(req, arg) };

    expect_errno(
        call(sys::ioc(sys::_IOC_NONE, 'x' as u32, 0x7f, 0), arg),
        ENOTTY,
        "an unknown ioctl",
    );
    for dir in 0..4u32 {
        for nr in 0..5u32 {
            expect_errno(
                call(sys::ioc(dir, 'z' as u32, nr, psize), arg),
                ENOTTY,
                "a foreign type byte",
            );
        }
    }
    for nr in 3..8u32 {
        expect_errno(
            call(sys::ioc(rw, KORU_IOC_TYPE, nr, psize), arg),
            ENOTTY,
            "our type byte, unknown number",
        );
    }

    expect_errno(
        call(sys::ioc(rw, KORU_IOC_TYPE, KORU_NR_SETUP, psize + 8), arg),
        EPROTO,
        "SETUP with the wrong size",
    );
    expect_errno(
        call(
            sys::ioc(sys::_IOC_READ, KORU_IOC_TYPE, KORU_NR_SETUP, psize),
            arg,
        ),
        EPROTO,
        "SETUP encoded read-only",
    );
    expect_errno(
        call(
            sys::ioc(sys::_IOC_WRITE, KORU_IOC_TYPE, KORU_NR_GET_PARAMS, psize),
            arg,
        ),
        EPROTO,
        "GET_PARAMS encoded write-only",
    );
    expect_errno(
        call(
            sys::ioc(sys::_IOC_NONE, KORU_IOC_TYPE, KORU_NR_GET_PARAMS, psize),
            arg,
        ),
        EPROTO,
        "GET_PARAMS with no direction",
    );
    expect_errno(
        call(
            sys::ioc(sys::_IOC_WRITE, KORU_IOC_TYPE, KORU_NR_ENTER, esize),
            arg,
        ),
        EPROTO,
        "ENTER encoded write-only",
    );

    let bogus = 0x10 as *mut c_void;
    expect_errno(
        call(sys::KORU_IOC_GET_PARAMS, bogus),
        EFAULT,
        "GET_PARAMS with a bad pointer",
    );
    expect_errno(
        call(sys::KORU_IOC_ENTER, bogus),
        EFAULT,
        "ENTER with a bad pointer",
    );
    expect_errno(
        call(sys::KORU_IOC_SETUP, bogus),
        EFAULT,
        "SETUP with a bad pointer",
    );
}

// ---------------------------------------------------------------------------
// T22 - POLL_ADD
// ---------------------------------------------------------------------------

/// Every regular file: no `poll` method, so nothing could ever wake it and it
/// answers from the default mask at once.
#[test]
fn poll_on_a_regular_file_answers_at_once() {
    ensure_pattern_file();
    let m = Mapped::shared();
    let h = m.open_path(0, PATFILE, KORU_O_RDONLY);
    assert!(h > 0, "open");

    let ready = m.run_one(&Sqe::poll_add(
        0x500,
        h as u32,
        KORU_POLL_IN | KORU_POLL_OUT,
    ));
    assert_eq!(ready, (KORU_POLL_IN | KORU_POLL_OUT) as i64);
    // Nothing in the default mask, so the honest answer is an empty one rather
    // than a wait that could never end.
    assert_eq!(
        m.run_one(&Sqe::poll_add(0x501, h as u32, KORU_POLL_RDHUP)),
        0
    );

    assert_eq!(m.close_handle(h as u32), 0);
    m.assert_quiesced();
}

#[test]
fn poll_rejection_matrix() {
    ensure_pattern_file();
    let m = Mapped::shared();
    let h = m.open_path(0, PATFILE, KORU_O_RDONLY) as u32;
    let einval = -(EINVAL.0 as i64);

    let mut s = Sqe::poll_add(0x510, h, KORU_POLL_IN);
    s.off = 1;
    assert_eq!(m.run_one(&s), einval, "a non-zero off");
    let mut s = Sqe::poll_add(0x511, h, KORU_POLL_IN);
    s.slot = 1;
    assert_eq!(m.run_one(&s), einval, "a non-zero slot");
    assert_eq!(
        m.run_one(&Sqe::poll_add(0x512, h, 0)),
        einval,
        "an empty mask"
    );
    assert_eq!(
        m.run_one(&Sqe::poll_add(0x513, h, KORU_POLL_EVENTS_ALL + 1)),
        einval,
        "an unknown event bit"
    );
    assert_eq!(
        m.run_one(&Sqe::poll_add(0x514, 0, KORU_POLL_IN)),
        -(EBADF.0 as i64),
        "a zero handle"
    );

    assert_eq!(m.close_handle(h), 0);
    m.assert_quiesced();
}

/// The deliverable: a poll that waits, and a wake that arrives through the
/// socket layer rather than inside anybody's `write`.
#[test]
fn poll_on_a_socket_waits_for_its_event() {
    use std::io::Write;
    use std::os::unix::net::UnixStream;

    let m = Mapped::shared();
    let (mine, mut theirs) = UnixStream::pair().expect("socketpair");
    mine.set_nonblocking(true).expect("O_NONBLOCK");
    let h = m.adopt(mine.as_raw_fd());
    assert!(h > 0, "adopt");

    let mut cq = vec![Cqe::default(); 4];
    let armed = m
        .ring
        .submit(&[Sqe::poll_add(0x520, h as u32, KORU_POLL_IN)])
        .expect("ENTER");
    assert_eq!(armed.consumed, 1);
    assert_eq!(
        armed.progress.completed, 0,
        "an armed poll completes nothing"
    );

    // Nothing readable, so nothing may arrive.
    let quiet = m
        .ring
        .enter(&[], &mut cq, 1, Some(Duration::from_millis(100)))
        .expect("ENTER");
    assert_eq!(quiet.progress.completed, 0, "it stays armed");

    theirs.write_all(b"k").expect("write");
    let woke = m
        .ring
        .enter(&[], &mut cq, 1, Some(Duration::from_millis(1000)))
        .expect("ENTER");
    assert_eq!(woke.progress.completed, 1, "the wake completes it");
    let c = find_cqe(woke.cqes(&cq), 0x520);
    assert_eq!(c.res, KORU_POLL_IN as i64);

    assert_eq!(m.close_handle(h as u32), 0);
    m.assert_quiesced();
}

/// The token is what keeps this to one completion: without it the wake would
/// post a second one and break C1.
#[test]
fn a_cancelled_poll_completes_once_and_never_again() {
    use std::io::Write;
    use std::os::unix::net::UnixStream;

    let m = Mapped::shared();
    let (mine, mut theirs) = UnixStream::pair().expect("socketpair");
    mine.set_nonblocking(true).expect("O_NONBLOCK");
    let h = m.adopt(mine.as_raw_fd());
    assert!(h > 0, "adopt");

    let mut cq = vec![Cqe::default(); 4];
    m.ring
        .submit(&[Sqe::poll_add(0x530, h as u32, KORU_POLL_IN)])
        .expect("ENTER");

    let done = m
        .ring
        .enter(&[Sqe::cancel(0x531, 0x530)], &mut cq, 2, None)
        .expect("ENTER");
    assert_eq!(done.progress.completed, 2, "the poll and its cancel (C1)");
    assert_eq!(find_cqe(done.cqes(&cq), 0x530).res, -(ECANCELED.0 as i64));
    assert_eq!(find_cqe(done.cqes(&cq), 0x531).res, 0);

    // Without `remove_wait_queue` this walks a freed entry; without the token
    // it is a second completion for an op that already has one.
    theirs.write_all(b"k").expect("write");
    let after = m
        .ring
        .enter(&[], &mut cq, 1, Some(Duration::from_millis(100)))
        .expect("ENTER");
    assert_eq!(after.progress.completed, 0, "no CQE at all");

    assert_eq!(m.close_handle(h as u32), 0);
    m.assert_quiesced();
}

// ---------------------------------------------------------------------------
// T23 - STAT into a slot
// ---------------------------------------------------------------------------

/// A byte no field of a real `KoruStat` is likely to be, pre-filled so an
/// unwritten byte is visible.
const POISON: u8 = 0xa5;

// `st_dev` is `new_encode_dev`'d: minor's low eight bits, then major, then the
// rest of minor. From include/linux/kdev_t.h.
fn dev_major(dev: u64) -> u64 {
    (dev >> 8) & 0xfff
}

fn dev_minor(dev: u64) -> u64 {
    (dev & 0xff) | ((dev >> 12) & !0xff)
}

fn poison_slot(m: &Mapped, slot: u32) {
    m.slot(slot)[..size_of::<KoruStat>() + 8].fill(POISON);
}

/// Every field against `fstat(2)`, and every other byte zero. A partly filled
/// struct shows up here as poison read back as a kernel-reported value.
#[test]
fn stat_matches_fstat_field_for_field() {
    use std::os::linux::fs::MetadataExt;

    ensure_pattern_file();
    let m = Mapped::shared();
    let h = m.open_path(0, PATFILE, KORU_O_RDONLY) as u32;
    assert!(h > 0, "OPEN");

    poison_slot(&m, 1);
    let (res, extra) = m.stat_into(h, 1, 0, size_of::<KoruStat>() as u32);
    let md = std::fs::metadata(PATFILE).expect("metadata");
    assert_eq!(res, size_of::<KoruStat>() as i64, "res is the whole struct");

    let got = KoruStat::read_from(m.slot(1)).expect("a whole struct");
    assert_eq!(got.ino, md.st_ino(), "ino");
    assert_eq!(got.size, md.st_size(), "size");
    assert_eq!(got.blocks, md.st_blocks(), "blocks");
    assert_eq!(got.blksize, md.st_blksize(), "blksize");
    assert_eq!(got.nlink, md.st_nlink(), "nlink");
    assert_eq!(got.mode, u64::from(md.st_mode()), "mode");
    assert_eq!(got.mode & KORU_S_IFMT, KORU_S_IFREG, "a regular file");
    assert_eq!(got.uid, u64::from(md.st_uid()), "uid");
    assert_eq!(got.gid, u64::from(md.st_gid()), "gid");
    assert_eq!(got.dev_major, dev_major(md.st_dev()), "dev_major");
    assert_eq!(got.dev_minor, dev_minor(md.st_dev()), "dev_minor");
    assert_eq!(got.rdev_major, dev_major(md.st_rdev()), "rdev_major");
    assert_eq!(got.rdev_minor, dev_minor(md.st_rdev()), "rdev_minor");
    assert_eq!(got.atime_sec, md.st_atime(), "atime_sec");
    assert_eq!(got.atime_nsec, md.st_atime_nsec() as u64, "atime_nsec");
    assert_eq!(got.mtime_sec, md.st_mtime(), "mtime_sec");
    assert_eq!(got.mtime_nsec, md.st_mtime_nsec() as u64, "mtime_nsec");
    assert_eq!(got.ctime_sec, md.st_ctime(), "ctime_sec");
    assert_eq!(got.ctime_nsec, md.st_ctime_nsec() as u64, "ctime_nsec");
    assert_eq!(got.reserved, [0u64; 12], "reserved must read as zero");

    // `extra` is the first non-zero one koru has ever posted.
    assert_eq!(extra & !KORU_STAT_ALL, 0, "no bit outside KORU_STAT_ALL");
    let basic = KORU_STAT_ALL & !KORU_STAT_BTIME;
    assert_eq!(extra & basic, basic, "every field but btime was reported");
    assert_ne!(extra & KORU_STAT_BTIME, 0, "tmpfs reports a creation time");
    // The mask is koru's own; the statx one must not read as valid here.
    assert_ne!(extra, 4095, "the statx mask passed through untranslated");

    assert_eq!(m.close_handle(h), 0);
    m.assert_quiesced();
}

/// A device node, where `rdev` and the type bits are not both zero.
#[test]
fn stat_reports_a_device_node() {
    use std::os::linux::fs::MetadataExt;

    let m = Mapped::shared();
    let h = m.open_path(0, "/dev/null", KORU_O_RDONLY) as u32;
    assert!(h > 0, "OPEN /dev/null");

    poison_slot(&m, 1);
    let (res, _) = m.stat_into(h, 1, 0, size_of::<KoruStat>() as u32);
    assert_eq!(res, size_of::<KoruStat>() as i64);

    let md = std::fs::metadata("/dev/null").expect("metadata");
    let got = KoruStat::read_from(m.slot(1)).expect("a whole struct");
    assert_eq!(got.mode & KORU_S_IFMT, KORU_S_IFCHR, "a character device");
    assert_eq!(got.rdev_major, dev_major(md.st_rdev()), "rdev_major");
    assert_eq!(got.rdev_minor, dev_minor(md.st_rdev()), "rdev_minor");
    assert_ne!(got.rdev_major, 0, "and it is not the all-zero answer");

    assert_eq!(m.close_handle(h), 0);
    m.assert_quiesced();
}

/// `len` is the caller's buffer size and doubles as version negotiation.
#[test]
fn stat_clamps_to_len_and_writes_no_further() {
    ensure_pattern_file();
    let m = Mapped::shared();
    let h = m.open_path(0, PATFILE, KORU_O_RDONLY) as u32;
    assert!(h > 0, "OPEN");
    let full = size_of::<KoruStat>();

    poison_slot(&m, 1);
    assert_eq!(m.stat_into(h, 1, 0, 64).0, 64, "a short len returns itself");
    assert_eq!(m.slot(1)[64], POISON, "the byte after it is untouched");
    assert_ne!(m.slot(1)[0], POISON, "and the prefix really was written");

    poison_slot(&m, 1);
    assert_eq!(m.stat_into(h, 1, 0, 4096).0, full as i64, "clamped");
    assert_eq!(m.slot(1)[full], POISON, "nothing beyond the struct");

    assert_eq!(m.close_handle(h), 0);
    m.assert_quiesced();
}

/// Slots are whole pages, so a destination at 3968 spans two of them and
/// `write_slot`'s split is what has to get it right.
#[test]
fn stat_crosses_a_page_boundary_intact() {
    ensure_pattern_file();
    let m = Mapped::shared();
    let h = m.open_path(0, PATFILE, KORU_O_RDONLY) as u32;
    assert!(h > 0, "OPEN");

    poison_slot(&m, 1);
    let flat = m.stat_into(h, 1, 0, size_of::<KoruStat>() as u32).0;
    let want = KoruStat::read_from(m.slot(1)).expect("a whole struct");

    const OFF: usize = 3968;
    m.slot(2)[OFF..OFF + size_of::<KoruStat>() + 8].fill(POISON);
    let split = m
        .stat_into(h, 2, OFF as u64, size_of::<KoruStat>() as u32)
        .0;
    let got = KoruStat::read_from(&m.slot(2)[OFF..]).expect("a whole struct");

    assert_eq!(flat, split, "the same count either side of a page boundary");
    assert_eq!(got.ino, want.ino, "ino, written across the split");
    assert_eq!(got.size, want.size, "size");
    assert_eq!(got.reserved, [0u64; 12], "and the tail page too");

    assert_eq!(m.close_handle(h), 0);
    m.assert_quiesced();
}

#[test]
fn stat_rejection_matrix() {
    ensure_pattern_file();
    let m = Mapped::shared();
    let h = m.open_path(0, PATFILE, KORU_O_RDONLY) as u32;
    assert!(h > 0, "OPEN");
    let full = size_of::<KoruStat>() as u32;
    let (bad, ebadf) = (-(EINVAL.0 as i64), -(EBADF.0 as i64));

    let cases: [(&str, Sqe, i64); 8] = [
        ("an unaligned off", Sqe::stat(0x600, h, 1, 4, full), bad),
        ("a zero len", Sqe::stat(0x601, h, 1, 0, 0), bad),
        (
            "off + len past the slot",
            Sqe::stat(0x602, h, 1, u64::from(m.slot_size() - 8), 16),
            bad,
        ),
        (
            "an off + len that overflows",
            Sqe::stat(0x603, h, 1, u64::MAX & !7, 16),
            bad,
        ),
        (
            "a slot past the arena",
            Sqe::stat(0x604, h, m.slot_count(), 0, 64),
            bad,
        ),
        ("handle 0", Sqe::stat(0x605, 0, 1, 0, 64), ebadf),
        (
            "a stale generation",
            Sqe::stat(0x606, h + (1 << 16), 1, 0, 64),
            ebadf,
        ),
        (
            "an index past the table",
            Sqe::stat(0x607, m.handle_count() | (1 << 16), 1, 0, 64),
            ebadf,
        ),
    ];
    for (what, sqe, want) in cases {
        assert_eq!(m.run_one(&sqe), want, "{what}");
    }

    assert_eq!(m.close_handle(h), 0);
    assert_eq!(m.stat_into(h, 1, 0, 64).0, ebadf, "a closed handle");
    m.assert_quiesced();
}

/// A whole-slot CHECKSUM is slow enough to still hold the slot when the STAT
/// behind it is dispatched.
#[test]
fn stat_is_refused_a_slot_another_op_holds() {
    ensure_pattern_file();
    let m = Mapped::shared();
    let h = m.open_path(0, PATFILE, KORU_O_RDONLY) as u32;
    assert!(h > 0, "OPEN");
    let n = m.slot_size();

    let sq = [
        Sqe::checksum(0x610, 5, 0, n),
        Sqe::stat(0x611, h, 5, 0, size_of::<KoruStat>() as u32),
    ];
    let mut cq = [Cqe::default(); 2];
    let r = m.ring.enter(&sq, &mut cq, 2, None).expect("ENTER");
    assert_eq!(r.progress.completed, 2, "both complete");

    let refused = find_cqe(&cq, 0x611);
    assert_eq!(refused.res, -(EBUSY.0 as i64), "the STAT behind it");
    assert_eq!(refused.extra, 0, "a refused STAT reports no mask");

    assert_eq!(m.close_handle(h), 0);
    m.assert_quiesced();
}

/// The only thing that says KORU_OP_STAT reached OpWork::held_slot.
#[test]
fn stat_releases_its_slot_when_cancelled() {
    ensure_pattern_file();
    let m = Mapped::shared();
    let h = m.open_path(0, PATFILE, KORU_O_RDONLY) as u32;
    assert!(h > 0, "OPEN");
    let n = m.slot_size();

    let r = m
        .ring
        .enter(
            &[Sqe::stat(0x620, h, 6, 0, size_of::<KoruStat>() as u32)],
            &mut [],
            0,
            None,
        )
        .expect("ENTER");
    assert_eq!(r.consumed, 1, "the STAT is queued");

    let mut cq = [Cqe::default(); 4];
    let r = m
        .ring
        .enter(&[Sqe::cancel(0x621, 0x620)], &mut cq, 2, None)
        .expect("ENTER");
    assert_eq!(r.progress.completed, 2, "both complete");

    // Not -EBUSY: the slot came back whether the cancel won or the stat ran.
    assert!(
        m.run_one(&Sqe::checksum(0x622, 6, 0, n)) >= 0,
        "the cancelled STAT never released its slot"
    );
    assert_eq!(m.close_handle(h), 0);
    m.assert_quiesced();
}

// ---------------------------------------------------------------------------
// T24 - TRUNCATE, UTIMES and READLINK
// ---------------------------------------------------------------------------

/// A path op on slot 1: path at offset 0, argument after it.
fn path_op(m: &Mapped, opcode: u8, path: &str, arg: &[u8]) -> i64 {
    let n = m.put_path(1, path);
    let at = koru_sys::ring::arg_offset(0, n) as usize;
    m.slot(1)[at..at + arg.len()].copy_from_slice(arg);
    m.run_one(&Sqe::path(opcode, 0x700, 1, 0, n))
}

fn truncate(m: &Mapped, path: &str, size: u64) -> i64 {
    path_op(m, KORU_OP_TRUNCATE, path, &size.to_ne_bytes())
}

fn utimes(m: &Mapped, path: &str, t: &KoruTimes) -> i64 {
    path_op(m, KORU_OP_UTIMES, path, &t.as_bytes())
}

fn readlink(m: &Mapped, path: &str) -> i64 {
    path_op(m, KORU_OP_READLINK, path, &[])
}

/// A symlink removed when the guard drops.
struct Symlink(String);

impl Symlink {
    fn new(tag: &str, target: &str) -> Option<Symlink> {
        let path = format!("/tmp/koru-check-rs-link-{tag}");
        let _ = std::fs::remove_file(&path);
        std::os::unix::fs::symlink(target, &path).ok()?;
        Some(Symlink(path))
    }

    fn path(&self) -> &str {
        &self.0
    }
}

impl Drop for Symlink {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.0);
    }
}

#[test]
fn truncate_sets_the_length_stat_reports() {
    let f = Scratch::new("truncate");
    std::fs::write(f.path(), vec![0x5au8; 8192]).expect("fill");
    let m = Mapped::shared();

    assert_eq!(truncate(&m, f.path(), 100), 0, "shrink");
    assert_eq!(f.bytes().len(), 100, "stat(2) agrees");
    assert_eq!(truncate(&m, f.path(), 1 << 20), 0, "extend");
    let got = f.bytes();
    assert_eq!(got.len(), 1 << 20, "stat(2) agrees again");
    assert!(got[..100].iter().all(|&b| b == 0x5a), "the head survived");
    assert!(
        got[100..].iter().all(|&b| b == 0),
        "the hole reads as zeros"
    );
    assert_eq!(truncate(&m, f.path(), 0), 0, "and down to nothing");
    assert!(f.bytes().is_empty());
    m.assert_quiesced();
}

#[test]
fn truncate_follows_a_final_symlink() {
    let f = Scratch::new("trunclink");
    std::fs::write(f.path(), vec![1u8; 4096]).expect("fill");
    let Some(link) = Symlink::new("trunc", f.path()) else {
        skip("truncate", "cannot symlink");
        return;
    };
    let m = Mapped::shared();

    assert_eq!(truncate(&m, link.path(), 64), 0, "through the link");
    assert_eq!(f.bytes().len(), 64, "the target is what changed");
    m.assert_quiesced();
}

#[test]
fn truncate_rejection_matrix() {
    let f = Scratch::new("truncbad");
    let m = Mapped::shared();
    let bad = -(EINVAL.0 as i64);

    // The first two are `vfs_truncate`'s own.
    assert_eq!(truncate(&m, "/etc", 0), -(EISDIR.0 as i64), "a directory");
    assert_eq!(truncate(&m, "/dev/null", 0), bad, "a device node");
    assert_eq!(
        truncate(&m, "/no/such/path", 0),
        -(ENOENT.0 as i64),
        "a missing path"
    );
    assert_eq!(truncate(&m, f.path(), 1 << 63), bad, "a negative length");

    let n = m.put_path(1, f.path());
    let mut s = Sqe::path(KORU_OP_TRUNCATE, 0x710, 1, 0, n);
    s.handle = 1;
    assert_eq!(m.run_one(&s), bad, "a non-zero handle");
    assert_eq!(
        m.run_one(&Sqe::path(KORU_OP_TRUNCATE, 0x711, 1, 0, 0)),
        bad,
        "a zero-length path"
    );
    assert_eq!(
        m.run_one(&Sqe::path(KORU_OP_TRUNCATE, 0x712, m.slot_count(), 0, 8)),
        bad,
        "a slot past the arena"
    );
    // The argument has to fit after the path, not merely the path itself. The
    // path here is real and in range, so only the argument bound can refuse.
    // "/run/xyz" is exactly eight characters.
    std::fs::write("/run/xyz", b"").expect("short path");
    let off = u64::from(m.slot_size() - 12);
    let at = off as usize;
    m.slot(1)[at..at + 8].copy_from_slice(b"/run/xyz");
    assert_eq!(
        m.run_one(&Sqe::path(KORU_OP_TRUNCATE, 0x713, 1, off, 8)),
        bad,
        "an argument past the slot end"
    );
    let _ = std::fs::remove_file("/run/xyz");
    m.assert_quiesced();
}

#[test]
fn utimes_sets_the_times_stat_reports() {
    use std::os::linux::fs::MetadataExt;

    let f = Scratch::new("utimes");
    std::fs::write(f.path(), b"x").expect("fill");
    let m = Mapped::shared();

    let t = KoruTimes {
        atime_sec: 1_000_000_000,
        atime_nsec: 123_456_789,
        mtime_sec: 1_100_000_000,
        mtime_nsec: 987_654_321,
    };
    assert_eq!(utimes(&m, f.path(), &t), 0, "UTIMES");
    let md = std::fs::metadata(f.path()).expect("metadata");
    assert_eq!(md.st_atime(), t.atime_sec, "atime seconds");
    assert_eq!(md.st_atime_nsec(), t.atime_nsec, "atime nanoseconds");
    assert_eq!(md.st_mtime(), t.mtime_sec, "mtime seconds");
    assert_eq!(md.st_mtime_nsec(), t.mtime_nsec, "mtime nanoseconds");

    // The kernel's own sentinels, checked by `vfs_utimes`.
    let omit = KoruTimes {
        atime_nsec: KORU_UTIME_OMIT,
        mtime_sec: 1_200_000_000,
        mtime_nsec: 1,
        ..t
    };
    assert_eq!(utimes(&m, f.path(), &omit), 0, "UTIME_OMIT");
    let md = std::fs::metadata(f.path()).expect("metadata");
    assert_eq!(md.st_atime_nsec(), t.atime_nsec, "the atime is untouched");
    assert_eq!(md.st_mtime(), 1_200_000_000, "while the mtime moved");

    let bad = KoruTimes {
        atime_nsec: 1_000_000_000,
        ..omit
    };
    assert_eq!(
        utimes(&m, f.path(), &bad),
        -(EINVAL.0 as i64),
        "a nanosecond field past 999999999"
    );
    assert_eq!(
        utimes(&m, "/no/such/path", &t),
        -(ENOENT.0 as i64),
        "a missing path"
    );
    m.assert_quiesced();
}

#[test]
fn readlink_reproduces_readlink_2_without_following() {
    // Two links deep: following would give neither answer.
    let Some(inner) = Symlink::new("inner", "/etc/hostname") else {
        skip("readlink", "cannot symlink");
        return;
    };
    let Some(outer) = Symlink::new("outer", inner.path()) else {
        skip("readlink", "cannot symlink");
        return;
    };
    let m = Mapped::shared();

    let want = std::fs::read_link(outer.path()).expect("readlink(2)");
    let want = want.to_str().expect("utf8").as_bytes();
    let res = readlink(&m, outer.path());
    assert_eq!(res, want.len() as i64, "res is the length without the NUL");
    assert_eq!(&m.slot(1)[..want.len()], want, "and the slot holds it");
    assert_eq!(m.slot(1)[want.len()], 0, "NUL-terminated");
    assert_eq!(
        want,
        inner.path().as_bytes(),
        "the first target, not the last"
    );
    m.assert_quiesced();
}

#[test]
fn readlink_rejection_matrix() {
    let m = Mapped::shared();
    let bad = -(EINVAL.0 as i64);

    assert_eq!(readlink(&m, "/etc/hostname"), bad, "a regular file");
    assert_eq!(readlink(&m, "/etc"), bad, "a directory");
    assert_eq!(
        readlink(&m, "/no/such/path"),
        -(ENOENT.0 as i64),
        "a missing path"
    );

    // Longer than tmpfs's inline limit, so the target is page-backed too.
    let long: String = std::iter::repeat_n("/abcdefgh", 24).collect();
    let Some(link) = Symlink::new("reject", &long) else {
        skip("readlink", "cannot symlink");
        return;
    };
    let n = m.put_path(1, link.path());
    let mut s = Sqe::path(KORU_OP_READLINK, 0x720, 1, 0, n);
    s.handle = 1;
    assert_eq!(m.run_one(&s), bad, "a non-zero handle");

    // The answer replaces the path, so the room is the rest of the slot.
    // Truncating a path silently is how a wrong path gets used.
    let off = u64::from(m.slot_size()) - 64;
    let at = off as usize;
    m.slot(1)[at..at + link.path().len()].copy_from_slice(link.path().as_bytes());
    assert_eq!(
        m.run_one(&Sqe::path(KORU_OP_READLINK, 0x721, 1, off, n)),
        -(ENAMETOOLONG.0 as i64),
        "a target that does not fit"
    );
    m.assert_quiesced();
}

/// The whole inline-because-of-creds rule, for something other than `OPEN`.
/// Deferred to a kworker every one of these would run as root in the initial
/// namespaces and each refusal below would become a success.
#[test]
fn creds_an_unprivileged_path_op_is_refused() {
    if !is_root() {
        skip("path creds", "not root");
        return;
    }
    let f = Scratch::new("pathcreds");
    std::fs::write(f.path(), b"x").expect("fill");
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(f.path(), PermissionsExt::from_mode(0o600)).expect("chmod");
    }

    let (uid, gid) = nobody_ids();
    // The arena is VM_DONTCOPY, so the child maps it after the fork.
    let ring = Ring::with_config(&SetupConfig::new(32, 64, 8192, 4, 8)).expect("SETUP");
    let path = f.path().to_string();

    let pid = unsafe { sys::fork() };
    assert!(pid >= 0, "fork");
    if pid == 0 {
        let code = unsafe {
            match ring.mmap() {
                Err(_) => 2,
                Ok(arena) => {
                    sys::setgroups(0, std::ptr::null());
                    if sys::setresgid(gid, gid, gid) != 0 || sys::setresuid(uid, uid, uid) != 0 {
                        3
                    } else if sys::geteuid() == 0 {
                        4
                    } else {
                        let m = Mapped { ring, arena };
                        let touch = KoruTimes {
                            atime_nsec: KORU_UTIME_NOW,
                            mtime_nsec: KORU_UTIME_NOW,
                            ..KoruTimes::default()
                        };
                        let named = KoruTimes {
                            atime_sec: 1,
                            mtime_sec: 1,
                            ..KoruTimes::default()
                        };
                        if truncate(&m, &path, 0) != -(EACCES.0 as i64) {
                            5
                        // Named times need ownership; a touch needs only write.
                        } else if utimes(&m, &path, &named) != -(EPERM.0 as i64) {
                            6
                        } else if utimes(&m, &path, &touch) != -(EACCES.0 as i64) {
                            7
                        } else {
                            0
                        }
                    }
                }
            }
        };
        unsafe { sys::_exit(code) };
    }

    let mut status = 0;
    unsafe { sys::waitpid(pid, &mut status, 0) };
    assert!(sys::wifexited(status), "the child died");
    // 5 truncate, 6 named utimes, 7 touch.
    assert_eq!(sys::wexitstatus(status), 0, "child verdict");
}
