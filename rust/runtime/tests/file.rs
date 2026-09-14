// SPDX-License-Identifier: MIT

//! T31's done test: the buffered `File`'s fast path **measured**, its sticky
//! error, and the three iterators.
//!
//! Needs `/dev/koru`, so it runs in the VM under `scripts/rust.sh`. Run with
//! `--test-threads=1`: the ring is thread-local and each test installs one.

mod common;

use common::*;
use koru::{
    Args, Buffering, Error, FILE_BUF, File, FileKind, FileMode, Handle, Input, Kind, LineReader,
    TreeWalk,
};
use std::future::Future;

const ROOT: &str = "/tmp/koru-file";

fn fixture(name: &str) -> String {
    let dir = format!("{ROOT}/{name}");
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).expect("fixture");
    dir
}

fn run<F: Future<Output = ()>>(f: impl FnOnce() -> F) {
    arm_alarm(60);
    koru::install(runtime());
    koru::block_on(f());
    koru::rt::shutdown();
    disarm_alarm();
}

/// `ENTER` calls since the mark. The reactor's own counter, not a guess.
fn enters() -> u64 {
    koru::rt::current().stats().enters
}

// ---------------------------------------------------------------------------
// The fast path, measured
// ---------------------------------------------------------------------------

/// The point of the whole buffer. A version that suspends on every character
/// still produces the right text and would pass any behavioural test, so this
/// asserts an **exact** `ENTER` count instead.
#[test]
fn reading_a_rune_at_a_time_costs_one_enter_per_refill_and_no_more() {
    let dir = fixture("fast");
    let path = format!("{dir}/big");
    // Whole multiples of nothing: the last refill is a short one.
    const N: usize = FILE_BUF * 20 + 37;
    let text: String = (0..N).map(|i| (b'a' + (i % 26) as u8) as char).collect();
    std::fs::write(&path, &text).expect("write");

    run(|| async move {
        let mut f = File::open(&path, FileMode::Read).await.expect("open");

        // From here, and not before: the open itself costs ops of its own.
        let mark = enters();
        let mut got = String::new();
        loop {
            match f.get().await {
                Ok(c) => got.push(c),
                Err(e) if e.is(Kind::Closed) => break,
                Err(e) => panic!("get: {e}"),
            }
        }
        let spent = enters() - mark;
        f.close().await.expect("close");

        assert_eq!(got, text, "the bytes are the file's");

        // Twenty full blocks, a short one, and the read that reports the end.
        let refills = N.div_ceil(FILE_BUF) + 1;
        assert_eq!(
            spent, refills as u64,
            "{N} runes cost {spent} ENTERs; one per refill is {refills}"
        );
        assert!(
            spent * 100 < N as u64,
            "the buffer is not doing anything: {spent} for {N}"
        );
    });
}

/// The contrast, so the number above means something: unbuffered, every
/// character really is a syscall.
#[test]
fn an_unbuffered_read_costs_one_enter_per_call() {
    let dir = fixture("slow");
    let path = format!("{dir}/small");
    const N: usize = 64;
    std::fs::write(&path, "x".repeat(N)).expect("write");

    run(|| async move {
        let mut f = File::open(&path, FileMode::Read).await.expect("open");
        f.set_buffering(Buffering::None);

        let mark = enters();
        let mut one = [0u8; 1];
        for _ in 0..N {
            assert_eq!(f.read(&mut one).await.expect("read"), 1);
        }
        let spent = enters() - mark;
        f.close().await.expect("close");

        assert_eq!(spent, N as u64, "one ENTER per unbuffered read");
    });
}

/// A write is the same bet the other way: the block goes out once.
#[test]
fn writing_a_rune_at_a_time_costs_one_enter_per_block() {
    let dir = fixture("fast-write");
    let path = format!("{dir}/out");
    const N: usize = FILE_BUF * 4;

    run(|| async move {
        let mut f = File::open(&path, FileMode::Write).await.expect("open");
        f.set_buffering(Buffering::Full);

        let mark = enters();
        for i in 0..N {
            f.put((b'a' + (i % 26) as u8) as char).await.expect("put");
        }
        f.flush().await.expect("flush");
        let spent = enters() - mark;
        f.close().await.expect("close");

        assert_eq!(spent, (N / FILE_BUF) as u64, "one ENTER per full block");
        assert_eq!(std::fs::metadata(&path).unwrap().len(), N as u64);
    });
}

// ---------------------------------------------------------------------------
// The sticky error
// ---------------------------------------------------------------------------

/// A read error mid-stream leaves `failed()` true and `err()` exact, and the
/// next call answers out of the field rather than asking the kernel again.
#[test]
fn an_error_mid_stream_sticks_and_costs_nothing_to_read_back() {
    let dir = fixture("sticky");
    let path = format!("{dir}/text");
    std::fs::write(&path, "a".repeat(FILE_BUF * 3)).expect("write");

    run(|| async move {
        // Opened by hand so the handle can be retired underneath the File,
        // which is what makes the failure land mid-stream rather than at the
        // first read.
        let h = koru::open_read(&path).await.expect("open");
        let mut f = File::of(h, FileMode::Read);

        assert_eq!(f.get().await.expect("the first rune"), 'a');
        assert!(f.clean(), "nothing has gone wrong yet");

        koru::close_fd(h).await;

        // Drain what is buffered, then the refill fails.
        let bad = loop {
            match f.get().await {
                Ok(_) => continue,
                Err(e) => break e,
            }
        };
        assert!(f.failed(), "an error, not an end of input");
        assert!(!f.eof());
        assert_eq!(f.err(), Some(bad), "err() is the one that happened");

        // Sticky: the second call is the field, not another syscall.
        let mark = enters();
        assert_eq!(f.get().await.unwrap_err(), bad);
        assert_eq!(f.err(), Some(bad), "and the first error is kept");
        assert_eq!(enters(), mark, "a stuck stream asks the kernel nothing");

        f.clear_err();
        assert!(f.clean(), "cleared on request, and only on request");
    });
}

/// An end of input is not a failure, which is the other half of the idiom.
#[test]
fn an_end_of_input_is_eof_and_not_a_failure() {
    let dir = fixture("eof");
    let path = format!("{dir}/short");
    std::fs::write(&path, "ab").expect("write");

    run(|| async move {
        let mut f = File::open(&path, FileMode::Read).await.expect("open");
        assert_eq!(f.get().await.unwrap(), 'a');
        assert_eq!(f.get().await.unwrap(), 'b');
        assert!(f.get().await.unwrap_err().is(Kind::Closed));
        assert!(f.eof(), "an end of input");
        assert!(!f.failed(), "which is not a failure");
        assert!(!f.clean(), "but is not clean either");
        f.close().await.expect("close");
    });
}

/// Braam's rule, and the reason `Error::Cancelled` exists: `^C` is 130.
#[test]
fn a_cancelled_stream_maps_to_exit_status_130() {
    use koru::rt::Exit;
    let cancelled = Error::from(koru::Errno(125));
    assert!(cancelled.is(Kind::Cancelled));
    assert_eq!(Err::<(), Error>(cancelled).status(), 130);
    // And everything else a stream can fail with is 1.
    assert_eq!(Err::<(), Error>(Error::from(koru::Errno(5))).status(), 1);
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

#[test]
fn a_rune_is_a_codepoint_and_not_a_byte() {
    let dir = fixture("runes");
    let path = format!("{dir}/utf8");
    // Long enough that a rune straddles a block boundary.
    let text = "a√©‚òÉūüåć".repeat(400);
    std::fs::write(&path, &text).expect("write");

    run(|| async move {
        let mut f = File::open(&path, FileMode::Read).await.expect("open");
        let mut got = String::new();
        while let Ok(c) = f.get().await {
            got.push(c);
        }
        f.close().await.expect("close");
        assert_eq!(got, text);
        assert_eq!(
            got.chars().count(),
            text.chars().count(),
            "runes, not bytes"
        );
    });
}

/// A sequence the file ends in the middle of is one replacement, not a hang.
#[test]
fn a_truncated_sequence_at_the_end_is_one_replacement() {
    let dir = fixture("truncated");
    let path = format!("{dir}/cut");
    let mut bytes = b"ab".to_vec();
    bytes.extend_from_slice(&"‚òÉ".as_bytes()[..2]); // two of three
    std::fs::write(&path, &bytes).expect("write");

    run(|| async move {
        let mut f = File::open(&path, FileMode::Read).await.expect("open");
        assert_eq!(f.get().await.unwrap(), 'a');
        assert_eq!(f.get().await.unwrap(), 'b');
        assert_eq!(f.get().await.unwrap(), '\u{fffd}');
        assert_eq!(f.get().await.unwrap(), '\u{fffd}');
        assert!(f.get().await.unwrap_err().is(Kind::Closed));
        f.close().await.expect("close");
    });
}

#[test]
fn unget_is_seen_by_every_reader_after_it() {
    let dir = fixture("unget");
    let path = format!("{dir}/text");
    std::fs::write(&path, "bcd\nx").expect("write");

    run(|| async move {
        let mut f = File::open(&path, FileMode::Read).await.expect("open");
        assert_eq!(f.get().await.unwrap(), 'b');
        assert!(f.unget('a'));
        assert_eq!(f.get().await.unwrap(), 'a', "get sees it");

        // Braam's unget puts back what it is given, in front of what remains
        // — it does not restore what was taken.
        assert!(f.unget('a'));
        let mut line = String::new();
        assert!(f.getline(&mut line, false).await.unwrap());
        assert_eq!(line, "acd", "and so does getline");
        f.close().await.expect("close");
    });
}

#[test]
fn a_line_spans_as_many_refills_as_it_needs() {
    let dir = fixture("lines");
    let path = format!("{dir}/text");
    let long = "z".repeat(FILE_BUF * 3 + 7);
    std::fs::write(&path, format!("one\n\n{long}\nlast")).expect("write");

    run(|| async move {
        let mut f = File::open(&path, FileMode::Read).await.expect("open");
        let mut line = String::new();

        assert!(f.getline(&mut line, false).await.unwrap());
        assert_eq!(line, "one");
        assert!(f.getline(&mut line, false).await.unwrap());
        assert_eq!(line, "", "an empty line is a line");
        assert!(f.getline(&mut line, false).await.unwrap());
        assert_eq!(line, long, "one longer than the block");
        assert!(f.getline(&mut line, false).await.unwrap());
        assert_eq!(line, "last", "a final fragment with no newline");
        assert!(
            !f.getline(&mut line, false).await.unwrap(),
            "and then no more"
        );
        assert!(f.eof());
        assert_eq!(line, "", "which leaves nothing behind");
        f.close().await.expect("close");
    });
}

#[test]
fn getline_keeps_the_newline_when_asked() {
    let dir = fixture("keepnl");
    let path = format!("{dir}/text");
    std::fs::write(&path, "one\ntwo").expect("write");

    run(|| async move {
        let mut f = File::open(&path, FileMode::Read).await.expect("open");
        let mut line = String::new();
        assert!(f.getline(&mut line, true).await.unwrap());
        assert_eq!(line, "one\n");
        assert!(f.getline(&mut line, true).await.unwrap());
        assert_eq!(line, "two", "there is none on the last");
        f.close().await.expect("close");
    });
}

/// A span bigger than the block goes straight through it.
#[test]
fn a_big_read_bypasses_the_buffer() {
    let dir = fixture("bigread");
    let path = format!("{dir}/data");
    let bytes: Vec<u8> = (0..FILE_BUF * 8).map(|i| (i % 251) as u8).collect();
    std::fs::write(&path, &bytes).expect("write");

    run(|| async move {
        let mut f = File::open(&path, FileMode::Read).await.expect("open");
        let mut got = Vec::new();
        let mut chunk = vec![0u8; FILE_BUF * 4];
        loop {
            match f.read(&mut chunk).await {
                Ok(n) => got.extend_from_slice(&chunk[..n]),
                Err(e) if e.is(Kind::Closed) => break,
                Err(e) => panic!("read: {e}"),
            }
        }
        f.close().await.expect("close");
        assert_eq!(got, bytes, "byte for byte, whatever is not text");
    });
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

#[test]
fn every_buffering_mode_puts_the_same_bytes_out() {
    let dir = fixture("modes");

    run(|| async move {
        for (how, name) in [
            (Buffering::None, "none"),
            (Buffering::Line, "line"),
            (Buffering::Full, "full"),
        ] {
            let path = format!("{dir}/{name}");
            let mut f = File::open(&path, FileMode::Write).await.expect("open");
            f.set_buffering(how);
            f.write("one\n").await.expect("write");
            f.put('t').await.expect("put");
            f.write("wo").await.expect("write");
            f.close().await.expect("close");
            assert_eq!(
                std::fs::read_to_string(&path).unwrap(),
                "one\ntwo",
                "{name}"
            );
        }
    });
}

/// The mode's whole point: `Line` is out at the newline, `Full` is not.
#[test]
fn line_buffering_flushes_at_the_newline_and_full_does_not() {
    let dir = fixture("linebuf");
    let line = format!("{dir}/line");
    let full = format!("{dir}/full");

    run(|| async move {
        let mut a = File::open(&line, FileMode::Write).await.expect("open");
        a.set_buffering(Buffering::Line);
        a.write("out\n").await.expect("write");
        assert_eq!(
            std::fs::read_to_string(&line).unwrap(),
            "out\n",
            "Line is out already"
        );

        let mut b = File::open(&full, FileMode::Write).await.expect("open");
        b.set_buffering(Buffering::Full);
        b.write("out\n").await.expect("write");
        assert_eq!(
            std::fs::read_to_string(&full).unwrap(),
            "",
            "Full is still in the buffer"
        );
        b.flush().await.expect("flush");
        assert_eq!(std::fs::read_to_string(&full).unwrap(), "out\n");

        a.close().await.expect("close");
        b.close().await.expect("close");
    });
}

/// The documented promise, and the reason `at_exit` exists.
#[test]
fn dropping_a_file_neither_flushes_nor_closes() {
    let dir = fixture("drop");
    let path = format!("{dir}/out");

    run(|| async move {
        let h = {
            let mut f = File::open(&path, FileMode::Write).await.expect("open");
            f.set_buffering(Buffering::Full);
            f.write("lost").await.expect("write");
            let h = f.fd();
            drop(f);
            h
        };
        assert_eq!(
            std::fs::read_to_string(&path).unwrap(),
            "",
            "a destructor cannot await, so nothing was flushed"
        );
        // And the handle is still live, which is the other half of it.
        assert!(koru::stat_fd(h).await.is_ok(), "nor was it closed");
        koru::close_fd(h).await;
    });
}

#[test]
fn an_append_mode_file_lands_after_what_was_there() {
    let dir = fixture("append");
    let path = format!("{dir}/log");
    std::fs::write(&path, "old\n").expect("write");

    run(|| async move {
        let mut f = File::open(&path, FileMode::Append).await.expect("open");
        f.write("new\n").await.expect("write");
        f.close().await.expect("close");
        assert_eq!(std::fs::read_to_string(&path).unwrap(), "old\nnew\n");
    });
}

// ---------------------------------------------------------------------------
// Seeking, closing, detaching
// ---------------------------------------------------------------------------

#[test]
fn a_seek_discards_the_read_ahead_rather_than_double_counting_it() {
    let dir = fixture("seek");
    let path = format!("{dir}/text");
    std::fs::write(&path, "0123456789").expect("write");

    run(|| async move {
        let mut f = File::open(&path, FileMode::Read).await.expect("open");
        assert_eq!(f.get().await.unwrap(), '0'); // reads ahead to the end

        // SEEK_CUR is one past what the caller has taken, not one past what
        // the descriptor has read.
        assert_eq!(f.seek(0, koru::SEEK_CUR).await.unwrap(), 1);
        assert_eq!(f.get().await.unwrap(), '1');

        assert_eq!(f.seek(4, koru::SEEK_SET).await.unwrap(), 4);
        assert_eq!(f.get().await.unwrap(), '4');
        assert_eq!(f.seek(-2, koru::SEEK_END).await.unwrap(), 8);
        assert_eq!(f.get().await.unwrap(), '8');

        // Past the end, and back: the end of input clears on a seek.
        assert_eq!(f.seek(100, koru::SEEK_SET).await.unwrap(), 100);
        assert!(f.get().await.unwrap_err().is(Kind::Closed));
        assert!(f.eof());
        assert_eq!(f.seek(0, koru::SEEK_SET).await.unwrap(), 0);
        assert!(f.clean(), "a seek clears an end of input");
        assert_eq!(f.get().await.unwrap(), '0');
        f.close().await.expect("close");
    });
}

#[test]
fn close_flushes_and_detach_winds_the_read_ahead_back() {
    let dir = fixture("detach");
    let path = format!("{dir}/text");
    std::fs::write(&path, "0123456789").expect("write");

    run(|| async move {
        let mut f = File::open(&path, FileMode::Read).await.expect("open");
        assert_eq!(f.get().await.unwrap(), '0');

        // The descriptor is at 10; detach winds it back to 1.
        let h = f.detach().await.expect("detach");
        assert_eq!(koru::seek_fd(h, 0, koru::SEEK_CUR).await.unwrap(), 1);
        assert!(!f.clean(), "a detached File is spent");
        // It did not own the handle after detaching, so this is ours.
        koru::close_fd(h).await;

        // close, on the other hand, retires the handle it opened.
        let mut g = File::open(&path, FileMode::Write).await.expect("open");
        g.write("x").await.expect("write");
        let gh = g.fd();
        g.close().await.expect("close");
        assert_eq!(std::fs::read_to_string(&path).unwrap(), "x", "flushed");
        assert!(koru::stat_fd(gh).await.is_err(), "and the handle is gone");
        g.close().await.expect("a second close is a no-op");
    });
}

// ---------------------------------------------------------------------------
// Scanning
// ---------------------------------------------------------------------------

#[test]
fn the_scanners_read_scanfs_conversions_off_a_stream() {
    let dir = fixture("scan");
    let path = format!("{dir}/fields");
    std::fs::write(&path, "  42 -7 0x1f 0755 0b101 word  rest:tail").expect("write");

    run(|| async move {
        let mut f = File::open(&path, FileMode::Read).await.expect("open");
        let mut s = String::new();

        assert_eq!(
            f.scan_i64(10, 0).await.unwrap(),
            42,
            "leading space skipped"
        );
        assert_eq!(f.scan_i64(10, 0).await.unwrap(), -7);
        assert_eq!(f.scan_u64(0, 0).await.unwrap(), 0x1f, "base 0 reads 0x");
        assert_eq!(f.scan_u64(0, 0).await.unwrap(), 0o755, "and a bare 0");
        assert_eq!(f.scan_u64(0, 0).await.unwrap(), 0b101, "and 0b");

        assert!(f.scan_token(&mut s, 0).await.unwrap());
        assert_eq!(s, "word");

        // scan_until does not skip leading space, so the two spaces are it.
        assert!(f.scan_until(&mut s, ":", 0).await.unwrap());
        assert_eq!(s, "  rest");
        assert!(f.scan_lit(b':').await.unwrap());
        assert!(!f.scan_lit(b':').await.unwrap(), "not there, not taken");
        assert!(f.scan_token(&mut s, 0).await.unwrap());
        assert_eq!(s, "tail");

        assert!(!f.scan_token(&mut s, 0).await.unwrap(), "end of input");
        f.close().await.expect("close");
    });
}

#[test]
fn a_field_width_stops_a_scanner_short() {
    let dir = fixture("width");
    let path = format!("{dir}/fields");
    std::fs::write(&path, "123456 abcdef").expect("write");

    run(|| async move {
        let mut f = File::open(&path, FileMode::Read).await.expect("open");
        let mut s = String::new();
        assert_eq!(f.scan_i64(10, 3).await.unwrap(), 123);
        assert_eq!(f.scan_i64(10, 0).await.unwrap(), 456, "the rest is a field");
        assert!(f.scan_token(&mut s, 3).await.unwrap());
        assert_eq!(s, "abc");
        assert!(f.scan_token(&mut s, 0).await.unwrap());
        assert_eq!(s, "def");
        f.close().await.expect("close");
    });
}

/// The one-pass rule, stated in the header and worth an assertion.
#[test]
fn a_number_that_is_not_one_is_invalid_and_does_not_back_up() {
    let dir = fixture("onepass");
    let path = format!("{dir}/bad");
    std::fs::write(&path, "  -x 0xzz").expect("write");

    run(|| async move {
        let mut f = File::open(&path, FileMode::Read).await.expect("open");
        let e = f.scan_i64(10, 0).await.unwrap_err();
        assert!(e.is(Kind::Invalid), "{e}");
        assert!(f.failed(), "and it is sticky");
        f.clear_err();

        // The space and the sign are spent; 'x' is what is left.
        let mut s = String::new();
        assert!(f.scan_token(&mut s, 0).await.unwrap());
        assert_eq!(s, "x", "nothing was restored");

        let e = f.scan_u64(0, 0).await.unwrap_err();
        assert!(e.is(Kind::Invalid), "a 0x with no hex digit: {e}");
        f.close().await.expect("close");
    });
}

// ---------------------------------------------------------------------------
// The standard streams
// ---------------------------------------------------------------------------

/// Braam hands back a `File&`; Rust hands back a name for the slot. Either
/// way there is exactly one buffer behind it.
#[test]
fn the_standard_streams_are_one_buffer_each() {
    run(|| async {
        // stderr is unbuffered, so this is out before anything that follows.
        koru::write_err("")
            .await
            .expect("an empty write is a no-op");

        let out = File::stdout();
        assert!(out.clean());
        assert_eq!(out, File::stdout(), "the same slot, not a new stream");

        // Taking it out for one call and putting it back leaves it usable.
        out.write("").await.expect("write");
        assert!(out.clean(), "and still clean");
        out.flush().await.expect("flush");
    });
}

// ---------------------------------------------------------------------------
// Input and LineReader
// ---------------------------------------------------------------------------

#[test]
fn an_input_reads_the_files_named_on_a_command_line_as_one_stream() {
    let dir = fixture("input");
    let a = format!("{dir}/a");
    let b = format!("{dir}/b");
    std::fs::write(&a, "one\ntwo\n").expect("write");
    std::fs::write(&b, "three\nfour").expect("write");

    run(|| async move {
        let paths = Args::new(vec![a.clone(), b.clone()]);
        let mut lines = LineReader::new(Input::new(paths, Handle(0), "t31"));
        let mut got = Vec::new();
        let mut line = String::new();
        while lines.next(&mut line).await.expect("next") {
            got.push(line.clone());
        }
        assert_eq!(got, ["one", "two", "three", "four"]);

        // A File over the same Input: the same concatenation, rune by rune.
        let paths = Args::new(vec![a, b]);
        let mut f = File::over(Input::new(paths, Handle(0), "t31"));
        let mut text = String::new();
        while let Ok(c) = f.get().await {
            text.push(c);
        }
        assert_eq!(text, "one\ntwo\nthree\nfour");
    });
}

/// A rune split across a chunk boundary is what makes `Input::read` bytes.
#[test]
fn an_input_carries_a_rune_across_a_chunk_boundary() {
    let dir = fixture("straddle");
    let path = format!("{dir}/utf8");
    // Bigger than one READ, and a cycle whose length shares no factor with
    // the read size: "aé☃" alone is six bytes and divides it exactly, so no
    // rune would ever straddle and the carry would go untested.
    let text = "aé☃x".repeat(40_000);
    std::fs::write(&path, &text).expect("write");
    assert_ne!(
        text.len() % koru::READ_MAX as usize % 7,
        0,
        "the fixture must put a boundary inside a sequence"
    );

    run(|| async move {
        let paths = Args::new(vec![path.clone()]);
        let mut f = File::over(Input::new(paths, Handle(0), "t31"));
        let mut got = String::new();
        while let Ok(c) = f.get().await {
            got.push(c);
        }
        assert_eq!(got.chars().count(), text.chars().count());
        assert_eq!(got, text);

        // And read_chunk itself hands back the fragment rather than failing.
        let h = koru::open_read(&path).await.expect("open");
        let mut all = Vec::new();
        while let Ok(chunk) = koru::read_chunk(h).await {
            all.extend_from_slice(&chunk);
        }
        koru::close_fd(h).await;
        assert_eq!(all, text.as_bytes());
    });
}

/// No path named: the fallback handle is the stream.
#[test]
fn an_input_with_no_paths_reads_its_fallback() {
    let dir = fixture("fallback");
    let path = format!("{dir}/text");
    std::fs::write(&path, "from the fallback\n").expect("write");

    run(|| async move {
        let h = koru::open_read(&path).await.expect("open");
        let mut lines = LineReader::new(Input::new(Args::new(Vec::new()), h, "t31"));
        let mut line = String::new();
        assert!(lines.next(&mut line).await.unwrap());
        assert_eq!(line, "from the fallback");
        assert!(!lines.next(&mut line).await.unwrap());
        koru::close_fd(h).await;
    });
}

/// A file that will not open is reported once and comes back as its own error.
#[test]
fn an_input_reports_a_file_it_cannot_open_and_gives_up_on_it() {
    let dir = fixture("badinput");
    let good = format!("{dir}/good");
    std::fs::write(&good, "x\n").expect("write");

    run(|| async move {
        let paths = Args::new(vec!["/no/such/file".into(), good]);
        let mut input = Input::new(paths, Handle(0), "t31");
        let e = input.read().await.unwrap_err();
        assert!(e.is(Kind::NotFound), "{e}");
        // Spent: a further read is not a retry, and does not reach the second
        // path either — the caller is expected to stop.
        assert!(input.read().await.unwrap_err().is(Kind::Closed));
    });
}

// ---------------------------------------------------------------------------
// TreeWalk
// ---------------------------------------------------------------------------

#[test]
fn a_tree_walks_pre_order_and_hands_a_link_over_rather_than_following_it() {
    let dir = fixture("walk");
    let root = format!("{dir}/root");
    std::fs::create_dir_all(format!("{root}/a/b")).expect("mkdir");
    std::fs::create_dir(format!("{root}/empty")).expect("mkdir");
    std::fs::write(format!("{root}/top"), "top").expect("write");
    std::fs::write(format!("{root}/a/mid"), "mid").expect("write");
    std::fs::write(format!("{root}/a/b/deep"), "deep").expect("write");
    std::os::unix::fs::symlink("../top", format!("{root}/a/link")).expect("symlink");

    run(|| async move {
        let mut w = TreeWalk::new(&root);
        let mut seen: Vec<(String, FileKind)> = Vec::new();
        let mut path = String::new();
        let mut e = koru::DirEntry {
            name: String::new(),
            kind: FileKind::File,
            size: 0,
            mtime: 0,
        };
        while w.next(&mut path, &mut e).await.expect("next") {
            // The root itself is never reported, and root_len splices on.
            assert!(path.starts_with(&root));
            seen.push((path[w.root_len()..].to_string(), e.kind));
        }
        seen.sort_by(|a, b| a.0.cmp(&b.0));

        assert_eq!(
            seen,
            vec![
                ("/a".to_string(), FileKind::Dir),
                ("/a/b".to_string(), FileKind::Dir),
                ("/a/b/deep".to_string(), FileKind::File),
                ("/a/link".to_string(), FileKind::Link),
                ("/a/mid".to_string(), FileKind::File),
                ("/empty".to_string(), FileKind::Dir),
                ("/top".to_string(), FileKind::File),
            ],
            "every entry once, a link handed over, an empty directory reported"
        );
    });
}

/// A directory that will not list names itself, and the walk goes on.
#[test]
fn a_directory_that_will_not_list_names_itself_in_at() {
    let dir = fixture("walkbad");
    let root = format!("{dir}/root");
    std::fs::create_dir_all(format!("{root}/closed")).expect("mkdir");
    std::fs::write(format!("{root}/open"), "x").expect("write");

    run(|| async move {
        let mut w = TreeWalk::new(&root);
        let mut path = String::new();
        let mut e = koru::DirEntry {
            name: String::new(),
            kind: FileKind::File,
            size: 0,
            mtime: 0,
        };

        // Take the first entries, then make the subdirectory unreadable.
        let mut seen = Vec::new();
        let mut failed = None;
        loop {
            // Unreadable only once the walk has reported it and is about to
            // descend, which is the case `at()` is for.
            if seen.contains(&"closed".to_string()) {
                let _ = std::fs::set_permissions(
                    format!("{root}/closed"),
                    std::os::unix::fs::PermissionsExt::from_mode(0o000),
                );
            }
            match w.next(&mut path, &mut e).await {
                Ok(true) => seen.push(e.name.clone()),
                Ok(false) => break,
                Err(err) => {
                    failed = Some((w.at().to_string(), err));
                    break;
                }
            }
        }
        let _ = std::fs::set_permissions(
            format!("{root}/closed"),
            std::os::unix::fs::PermissionsExt::from_mode(0o755),
        );

        if koru::rt::current().stats().enters > 0 && failed.is_some() {
            let (at, err) = failed.unwrap();
            assert!(at.ends_with("/closed"), "at() names the directory: {at}");
            assert!(err.is(Kind::Perm), "{err}");
        } else {
            // Running as root, where no directory refuses to be listed.
            assert!(seen.contains(&"open".to_string()));
        }
    });
}
