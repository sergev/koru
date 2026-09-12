// SPDX-License-Identifier: MIT

//! The runtime suite: futures, the op slab and the executor.
//!
//! Needs `/dev/koru`, so it runs in the VM under `scripts/rust.sh`. Run with
//! `--test-threads=1`: every test opens its own ring, and several assert on
//! the pool's free count.

mod common;

use common::*;
use koru::{Errno, Handle, Kind};
use koru_sys::abi::KORU_O_RDONLY;
use koru_sys::error::{EBADF, EINVAL};
use std::cell::RefCell;
use std::rc::Rc;

/// The deliverable for the phase: a file read through the ring while timers
/// complete out of order.
#[test]
fn the_demo_reads_a_file_while_timers_complete_out_of_order() {
    arm_alarm(30);
    ensure_data_file();
    let rt = runtime();
    let order = Rc::new(RefCell::new(Vec::new()));

    // Armed longest first, so submission order and completion order differ.
    for ms in [30u64, 10, 20] {
        let rt2 = rt.clone();
        let order2 = Rc::clone(&order);
        rt.spawn(async move {
            rt2.delay(ms * MS).await.expect("delay");
            order2.borrow_mut().push(ms);
        });
    }

    let text = rt.block_on(async {
        let slot = rt.acquire().expect("a slot");
        let (h, slot) = rt.open(slot, DATAFILE, KORU_O_RDONLY).await;
        let h = h.expect("open");
        let (n, slot) = rt.read(h, slot, 0, DATASIZE as u32).await;
        let n = n.expect("read");
        let text: Vec<u8> = slot[..n].to_vec();
        rt.close(h).await.expect("close");
        text
    });
    rt.run();
    disarm_alarm();

    // The printed bytes are the file and nothing else: T42's C++ demo has to
    // match them byte for byte.
    println!("{}", String::from_utf8_lossy(&text));

    assert_eq!(text, std::fs::read(DATAFILE).expect("read back"));
    assert_eq!(
        *order.borrow(),
        vec![10, 20, 30],
        "timers resolved in submission order, so nothing ran concurrently"
    );
    rt.drain();
    assert_eq!(rt.stats().inflight, 0);
    assert_eq!(rt.pool().free_count(), SLOTS as usize);
}

#[test]
fn a_nop_completes() {
    let rt = runtime();
    rt.block_on(async { rt.nop().await }).expect("nop");
    assert_eq!(rt.stats().cqes_reaped, 1);
}

/// OPEN runs inline in the submitting ENTER, so one turn both submits and
/// reaps it. A second ENTER here would mean the executor parked needlessly.
#[test]
fn an_inline_open_costs_exactly_one_enter() {
    ensure_data_file();
    let rt = runtime();
    let h = rt.block_on(async {
        let slot = rt.acquire().expect("a slot");
        let (h, _slot) = rt.open(slot, DATAFILE, KORU_O_RDONLY).await;
        h.expect("open")
    });
    assert_eq!(rt.stats().enters, 1, "an inline op should not park");
    rt.block_on(async { rt.close(h).await }).expect("close");
}

#[test]
fn a_read_returns_the_file_bytes() {
    ensure_data_file();
    let rt = runtime();
    let want = std::fs::read(DATAFILE).expect("read back");
    rt.block_on(async {
        let slot = rt.acquire().expect("a slot");
        let (h, slot) = rt.open(slot, DATAFILE, KORU_O_RDONLY).await;
        let h = h.expect("open");
        let (n, slot) = rt.read(h, slot, 0, DATASIZE as u32).await;
        assert_eq!(n.expect("read"), DATASIZE);
        assert_eq!(&slot[..DATASIZE], &want[..]);
        // Past the end is a short read of zero, not an error: mapping that to
        // Closed is T30's job.
        let (n, _slot) = rt.read(h, slot, DATASIZE as u64, 64).await;
        assert_eq!(n.expect("read past the end"), 0);
        rt.close(h).await.expect("close");
    });
}

/// The slot comes back even when the op fails, or a failing read would leak
/// one slot of `slot_count` for good.
#[test]
fn a_failed_op_still_returns_its_slot() {
    let rt = runtime();
    rt.block_on(async {
        let slot = rt.acquire().expect("a slot");
        let (res, slot) = rt.read(Handle(0), slot, 0, 64).await;
        let e = res.expect_err("a zero handle");
        assert_eq!(e.raw(), EBADF);
        assert_eq!(e.kind(), Kind::Invalid);
        assert_eq!(rt.pool().free_count(), SLOTS as usize - 1);
        drop(slot);
        assert_eq!(rt.pool().free_count(), SLOTS as usize);
    });
}

#[test]
fn open_rejects_a_bad_path_without_spending_a_submission() {
    let rt = runtime();
    rt.block_on(async {
        for (path, what) in [("", "an empty path"), ("a\0b", "an embedded NUL")] {
            let slot = rt.acquire().expect("a slot");
            let (res, _slot) = rt.open(slot, path, KORU_O_RDONLY).await;
            assert_eq!(res.expect_err(what).raw(), EINVAL, "{what}");
        }
    });
    assert_eq!(rt.stats().sqes_submitted, 0, "nothing should have gone out");
    assert_eq!(rt.pool().free_count(), SLOTS as usize);
}

/// A future built and never awaited must cost nothing at all.
#[test]
fn an_unawaited_future_submits_nothing_and_returns_its_slot() {
    let rt = runtime();
    let slot = rt.acquire().expect("a slot");
    assert_eq!(rt.pool().free_count(), SLOTS as usize - 1);
    drop(rt.read(Handle(1), slot, 0, 64));
    assert_eq!(rt.pool().free_count(), SLOTS as usize);
    assert_eq!(rt.stats().sqes_submitted, 0);
    assert_eq!(rt.stats().cancels_submitted, 0);
}

/// Dropping an op the kernel has never seen must drop its SQE, not cancel it:
/// a CANCEL would answer -ENOENT and no completion for the target would ever
/// arrive, so the entry and its slot would leak for the life of the process.
#[test]
fn dropping_a_queued_future_submits_no_cancel_and_frees_its_slot() {
    arm_alarm(30);
    let rt = runtime();
    rt.block_on(async {
        let slot = rt.acquire().expect("a slot");
        let index = slot.index();
        let mut read = rt.read(Handle(1), slot, 0, 64);
        assert!(
            PollOnce(&mut read).await.is_none(),
            "the first poll only queues"
        );
        drop(read);
        assert_eq!(
            rt.pool().free_count(),
            SLOTS as usize,
            "slot came straight back"
        );

        // The dropped SQE must never reach the kernel, or the next user of
        // that slot index gets -EBUSY from the slot_busy bitmap.
        let again = rt.acquire().expect("a slot");
        assert_eq!(again.index(), index, "the same index, reused");
        let (res, _slot) = rt.checksum(again, 0, 4096).await;
        assert!(res.is_ok(), "a stale SQE poisoned the slot: {res:?}");
    });
    disarm_alarm();
    assert_eq!(rt.stats().cancels_submitted, 0);
}

/// Dropping an op the kernel has consumed must cancel it and keep its slot,
/// because the kernel releases its own claim only when it posts the CQE.
#[test]
fn dropping_a_live_future_cancels_it_and_holds_its_slot() {
    arm_alarm(30);
    let rt = runtime();
    rt.block_on(async {
        let slot = rt.acquire().expect("a slot");
        let mut checksum = rt.checksum(slot, 0, SLOT);
        assert!(PollOnce(&mut checksum).await.is_none());
        // Any completed op forces a turn, which consumes the queued CHECKSUM.
        rt.nop().await.expect("nop");
        assert!(rt.stats().inflight > 0, "the checksum is in flight");

        drop(checksum);
        assert_eq!(rt.stats().cancels_submitted, 1);
        assert_eq!(
            rt.pool().free_count(),
            SLOTS as usize - 1,
            "the slot must not return before the target's CQE"
        );
    });
    rt.drain();
    disarm_alarm();
    assert_eq!(rt.stats().inflight, 0);
    assert_eq!(rt.pool().free_count(), SLOTS as usize);
}

/// A cancelled delay completes -ECANCELED at its target, and the CANCEL's own
/// completion is discarded rather than waking anything.
#[test]
fn a_cancelled_op_is_dequeued_rather_than_waited_out() {
    arm_alarm(30);
    let rt = runtime();
    let t0 = std::time::Instant::now();
    rt.block_on(async {
        let mut long = rt.delay(3600 * 1000 * MS);
        assert!(PollOnce(&mut long).await.is_none());
        rt.nop().await.expect("nop");
        drop(long);
    });
    rt.drain();
    disarm_alarm();
    assert!(
        t0.elapsed().as_secs() < 5,
        "an hour-long delay was waited out rather than cancelled"
    );
    assert_eq!(rt.stats().inflight, 0);
}

/// Admission control gives a short submit, and the tail must stay queued: it
/// is not `Live`, so dropping it must not cancel an op the kernel never saw.
#[test]
fn a_batch_past_the_queue_depth_still_completes_every_op() {
    arm_alarm(60);
    let rt = runtime();
    let n = (CQ as usize) * 3;
    let done = Rc::new(std::cell::Cell::new(0usize));
    for _ in 0..n {
        let rt2 = rt.clone();
        let done2 = Rc::clone(&done);
        rt.spawn(async move {
            rt2.delay(MS).await.expect("delay");
            done2.set(done2.get() + 1);
        });
    }
    rt.run();
    disarm_alarm();
    assert_eq!(done.get(), n, "an op was lost in the unsubmitted tail");
    assert_eq!(rt.stats().sqes_submitted, n as u64);
    assert_eq!(rt.stats().inflight, 0);
}

#[test]
fn a_checksum_matches_the_host() {
    let rt = runtime();
    rt.block_on(async {
        let mut slot = rt.acquire().expect("a slot");
        for (i, b) in slot.iter_mut().enumerate() {
            *b = pattern_byte(i);
        }
        let want = fnv1a(&slot[..4096]);
        let (res, _slot) = rt.checksum(slot, 0, 4096).await;
        assert_eq!(res.expect("checksum") as i64, want);
    });
}

#[test]
fn a_retired_handle_is_rejected() {
    ensure_data_file();
    let rt = runtime();
    rt.block_on(async {
        let slot = rt.acquire().expect("a slot");
        let (h, _slot) = rt.open(slot, DATAFILE, KORU_O_RDONLY).await;
        let h = h.expect("open");
        rt.close(h).await.expect("close");
        let e = rt.close(h).await.expect_err("a second close");
        assert_eq!(e.raw(), EBADF);
    });
}

/// The kernel returns rather than sleeps when nothing is in flight, so an
/// executor that parks here would spin instead of waiting.
#[test]
#[should_panic(expected = "no completion can ever arrive")]
fn the_executor_refuses_to_park_when_nothing_can_arrive() {
    arm_alarm(30);
    let rt = runtime();
    rt.block_on(std::future::pending::<()>());
}

/// `ENTER` returns -EINTR on a signal, with the writeback intact. The
/// executor must resume from it rather than lose it.
///
/// Forked, because libtest runs each test on a spawned thread and a
/// process-directed alarm would land on the main one, not the parked one.
#[test]
fn a_signal_during_a_park_does_not_lose_the_writeback() {
    arm_alarm(60);
    let pid = unsafe { koru_sys::sys::fork() };
    if pid == 0 {
        // A panic in a forked libtest thread has no harness left to report
        // to, so the child would exit 0 and the verdict below would be
        // vacuous. Catch it and leave through _exit, which also avoids
        // flushing the parent's inherited stdout buffer.
        let code = std::panic::catch_unwind(eintr_child).unwrap_or(6);
        unsafe { koru_sys::sys::_exit(code) };
    }
    let mut status = 0;
    unsafe { koru_sys::sys::waitpid(pid, &mut status, 0) };
    disarm_alarm();
    assert!(
        koru_sys::sys::wifexited(status),
        "child died rather than exiting"
    );
    // 2 setup, 3 delay failed, 4 no EINTR, 5 cut short, 6 panicked.
    assert_eq!(koru_sys::sys::wexitstatus(status), 0, "child verdict");
}

fn eintr_child() -> i32 {
    let Ok(rt) = koru::Runtime::new(&config()) else {
        return 2;
    };
    interrupt_in(1);
    let t0 = std::time::Instant::now();
    if rt.block_on(async { rt.delay(2500 * MS).await }).is_err() {
        return 3;
    }
    if rt.stats().eintrs == 0 {
        return 4;
    }
    if t0.elapsed().as_millis() < 2400 {
        return 5;
    }
    0
}

#[test]
fn errno_names_survive_the_round_trip() {
    let rt = runtime();
    rt.block_on(async {
        let slot = rt.acquire().expect("a slot");
        let (res, _slot) = rt.read(Handle(0), slot, 0, 64).await;
        let e = res.expect_err("a zero handle");
        assert_eq!(e.raw(), EBADF);
        assert_eq!(e.raw().name(), Some("EBADF"));
        assert_ne!(e.raw(), Errno(0));
    });
}
