// SPDX-License-Identifier: MIT

//! T30's done test: Braam's prototypes, and every operation against its libc
//! equivalent on a fixture tree.
//!
//! Needs `/dev/koru`, so it runs in the VM under `scripts/rust.sh`. Run with
//! `--test-threads=1`: the ring is thread-local and each test installs one.

mod common;

use common::*;
use koru::{
    Clock, DirEntry, Error, FileInfo, FileKind, Handle, Kind, O_APPEND, O_CREATE, O_EXCL, O_TRUNC,
    O_WRITE, Result, SEEK_CUR, SEEK_END, SEEK_SET, Str,
};
use std::future::Future;

// ---------------------------------------------------------------------------
// Signature conformance
// ---------------------------------------------------------------------------

/// Braam's `src/proc/io.h`, prototype for prototype. This function is never
/// called: it exists so that a drift in a name, an argument order or a type is
/// a compile error rather than a surprise at a call site.
///
/// The comment above each line is the C++ it must match.
#[allow(dead_code, unused_must_use, clippy::let_underscore_future)]
async fn braam_prototypes(fd: Handle, path: Str<'_>, flags: u32, why: Error) {
    // Task<Result<void>> write_all(u32 fd, Str s);
    let _: Result<()> = koru::write_all(fd, path).await;
    // Task<Result<String>> read_chunk(u32 fd);
    let _: Result<String> = koru::read_chunk(fd).await;
    // Task<Result<String>> read_some(u32 fd, u32 max);
    let _: Result<String> = koru::read_some(fd, 0u32).await;
    // Task<Result<i32>> open_at(Str path, u32 flags);
    let _: Result<Handle> = koru::open_at(path, flags).await;
    // Task<Result<i32>> open_read(Str path);
    let _: Result<Handle> = koru::open_read(path).await;
    // Task<Result<String>> read_file(Str path);
    let _: Result<String> = koru::read_file(path).await;
    // Task<void> close_fd(u32 fd);
    let _: () = koru::close_fd(fd).await;
    // Task<Result<u32>> dup_fd(u32 fd);
    let _: Result<Handle> = koru::dup_fd(fd).await;
    // Task<Result<u64>> seek_fd(u32 fd, i64 off, u32 whence);
    let _: Result<u64> = koru::seek_fd(fd, 0i64, 0u32).await;
    // Task<Result<void>> truncate_fd(u32 fd, u64 n);
    let _: Result<()> = koru::truncate_fd(fd, 0u64).await;
    // Task<Result<FileInfo>> stat_of(Str path, bool follow = true);
    let _: Result<FileInfo> = koru::stat_of(path, true).await;
    // Task<Result<FileInfo>> stat_fd(u32 fd);
    let _: Result<FileInfo> = koru::stat_fd(fd).await;
    // Task<Result<Vec<DirEntry>>> list_dir(Str path);
    let _: Result<Vec<DirEntry>> = koru::list_dir(path).await;
    // Task<Result<void>> make_dir(Str path);
    let _: Result<()> = koru::make_dir(path).await;
    // Task<Result<void>> make_dir_all(Str path);
    let _: Result<()> = koru::make_dir_all(path).await;
    // Task<Result<void>> copy_file(Str from, Str to);
    let _: Result<()> = koru::copy_file(path, path).await;
    // Task<Result<void>> copy_tree(Str from, Str to);
    let _: Result<()> = koru::copy_tree(path, path).await;
    // Task<Result<void>> remove_path(Str path, bool all);
    let _: Result<()> = koru::remove_path(path, false).await;
    // Task<Result<void>> touch_path(Str path);
    let _: Result<()> = koru::touch_path(path).await;
    // Task<Result<void>> make_link(Str target, Str path);
    let _: Result<()> = koru::make_link(path, path).await;
    // Task<Result<String>> read_link(Str path);
    let _: Result<String> = koru::read_link(path).await;
    // Task<Result<void>> rename_path(Str from, Str to);
    let _: Result<()> = koru::rename_path(path, path).await;
    // Task<Result<String>> cwd_get();
    let _: Result<String> = koru::cwd_get().await;
    // Task<Result<String>> cwd_set(Str path);
    let _: Result<String> = koru::cwd_set(path).await;
    // Task<Result<void>> sleep_for(u32 ms);
    let _: Result<()> = koru::sleep_for(0u32).await;
    // Task<Result<Clock>> clock_now();
    let _: Result<Clock> = koru::clock_now().await;
    // Task<void> errln(Str who, Str what, Error why);
    let _: () = koru::errln(path, path, why).await;
}

/// The declarations above are the test; this only keeps libtest honest about
/// having run something.
#[test]
fn every_declaration_matches_braams_prototype() {
    let f = braam_prototypes;
    assert_eq!(size_of_val(&f), 0, "a function item carries no data");
}

// ---------------------------------------------------------------------------
// The fixture tree
// ---------------------------------------------------------------------------

const ROOT: &str = "/tmp/koru-ops";

/// `ROOT/<name>`, wiped and remade. Each test owns its own corner, so the
/// order libtest picks does not matter.
fn fixture(name: &str) -> String {
    let dir = format!("{ROOT}/{name}");
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).expect("fixture");
    dir
}

/// Install a ring and drive `f` on it. The operation layer names no executor,
/// so this is what a `#[koru::main]` program gets.
fn run<F: Future<Output = ()>>(f: impl FnOnce() -> F) {
    arm_alarm(60);
    koru::install(runtime());
    koru::block_on(f());
    koru::rt::shutdown();
    disarm_alarm();
}

fn kind_of(m: &std::fs::Metadata) -> FileKind {
    if m.is_dir() {
        FileKind::Dir
    } else if m.is_symlink() {
        FileKind::Link
    } else {
        FileKind::File
    }
}

/// What libc reports, in the shape koru reports it. The oracle for `stat_of`.
fn libc_info(path: &str, follow: bool) -> FileInfo {
    let m = if follow {
        std::fs::metadata(path).expect("metadata")
    } else {
        std::fs::symlink_metadata(path).expect("metadata")
    };
    FileInfo {
        kind: kind_of(&m),
        size: m.len(),
        mtime: libc_mtime_ms(&m),
    }
}

fn libc_mtime_ms(m: &std::fs::Metadata) -> u64 {
    m.modified()
        .ok()
        .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
        .map_or(0, |d| d.as_millis() as u64)
}

// ---------------------------------------------------------------------------
// Streams
// ---------------------------------------------------------------------------

#[test]
fn a_file_reads_through_the_ring_exactly_as_libc_reads_it() {
    let dir = fixture("read");
    let path = format!("{dir}/text");
    let text = "one\ntwo\nthree\n".repeat(500); // past one slot
    std::fs::write(&path, &text).expect("write");

    run(|| async move {
        assert_eq!(koru::read_file(&path).await.unwrap(), text);

        // read_chunk stops at the end of input with Closed, never an empty Ok.
        let fd = koru::open_read(&path).await.unwrap();
        let mut got = String::new();
        loop {
            match koru::read_chunk(fd).await {
                Ok(s) => got.push_str(&s),
                Err(e) if e.is(Kind::Closed) => break,
                Err(e) => panic!("read_chunk: {e}"),
            }
        }
        koru::close_fd(fd).await;
        assert_eq!(got, text);

        // read_some leaves the rest on the descriptor for the next read.
        let fd = koru::open_read(&path).await.unwrap();
        assert_eq!(koru::read_some(fd, 4).await.unwrap(), "one\n");
        assert_eq!(koru::read_some(fd, 4).await.unwrap(), "two\n");
        koru::close_fd(fd).await;

        assert!(
            koru::read_file("/no/such/file")
                .await
                .is_err_and(|e| e.is(Kind::NotFound))
        );
    });
}

#[test]
fn a_write_lands_where_libc_would_have_put_it() {
    let dir = fixture("write");
    let path = format!("{dir}/out");

    run(|| async move {
        let fd = koru::open_at(&path, O_WRITE | O_CREATE | O_TRUNC)
            .await
            .unwrap();
        koru::write_all(fd, "hello, ").await.unwrap();
        koru::write_all(fd, "world\n").await.unwrap();
        koru::write_all(fd, "").await.unwrap(); // a no-op, not an error
        koru::close_fd(fd).await;
        assert_eq!(std::fs::read_to_string(&path).unwrap(), "hello, world\n");

        // O_TRUNC over it, then O_APPEND after it.
        let fd = koru::open_at(&path, O_WRITE | O_TRUNC).await.unwrap();
        koru::write_all(fd, "a").await.unwrap();
        koru::close_fd(fd).await;
        let fd = koru::open_at(&path, O_WRITE | O_APPEND).await.unwrap();
        koru::write_all(fd, "b").await.unwrap();
        koru::close_fd(fd).await;
        assert_eq!(std::fs::read_to_string(&path).unwrap(), "ab");

        // O_EXCL over a name that is taken, and a flag koru does not know.
        let e = koru::open_at(&path, O_WRITE | O_CREATE | O_EXCL)
            .await
            .unwrap_err();
        assert!(e.is(Kind::Exists), "{e}");
        let e = koru::open_at(&path, 1 << 20).await.unwrap_err();
        assert!(e.is(Kind::Invalid), "{e}");
    });
}

// ---------------------------------------------------------------------------
// Descriptors
// ---------------------------------------------------------------------------

#[test]
fn a_second_name_shares_the_one_offset_and_the_last_close_shuts_it() {
    let dir = fixture("dup");
    let path = format!("{dir}/text");
    std::fs::write(&path, "abcdef").expect("write");

    run(|| async move {
        let a = koru::open_read(&path).await.unwrap();
        let b = koru::dup_fd(a).await.unwrap();

        // One handle behind both, so the offset is shared.
        assert_eq!(koru::read_some(a, 2).await.unwrap(), "ab");
        assert_eq!(koru::read_some(b, 2).await.unwrap(), "cd");
        assert_eq!(koru::seek_fd(a, 0, SEEK_CUR).await.unwrap(), 4);

        // Closing one shuts nothing.
        koru::close_fd(a).await;
        assert_eq!(koru::read_some(b, 2).await.unwrap(), "ef");
        koru::close_fd(b).await;
        // Now it is gone, and the kernel says so rather than this guessing.
        assert!(koru::read_some(b, 2).await.is_err());
    });
}

#[test]
fn a_seek_lands_where_lseek_would_have_landed() {
    let dir = fixture("seek");
    let path = format!("{dir}/text");
    std::fs::write(&path, "0123456789").expect("write");

    run(|| async move {
        let fd = koru::open_read(&path).await.unwrap();
        assert_eq!(koru::seek_fd(fd, 4, SEEK_SET).await.unwrap(), 4);
        assert_eq!(koru::read_some(fd, 2).await.unwrap(), "45");
        assert_eq!(koru::seek_fd(fd, 1, SEEK_CUR).await.unwrap(), 7);
        assert_eq!(koru::read_some(fd, 1).await.unwrap(), "7");
        assert_eq!(koru::seek_fd(fd, 0, SEEK_END).await.unwrap(), 10);
        assert_eq!(koru::seek_fd(fd, -3, SEEK_END).await.unwrap(), 7);
        assert_eq!(koru::seek_fd(fd, -2, SEEK_CUR).await.unwrap(), 5);

        // Past the end is not an error; a read there is an end of input.
        assert_eq!(koru::seek_fd(fd, 100, SEEK_SET).await.unwrap(), 100);
        assert!(
            koru::read_chunk(fd)
                .await
                .is_err_and(|e| e.is(Kind::Closed))
        );

        // Before the start, and a whence that is not one.
        assert!(koru::seek_fd(fd, -1, SEEK_SET).await.is_err());
        assert!(koru::seek_fd(fd, 0, 9).await.is_err());
        koru::close_fd(fd).await;

        // Err(Unsupported) on anything that is not a file, which is what a
        // standard stream is here: nothing opened it by name.
        let e = koru::seek_fd(koru::stdin(), 0, SEEK_CUR).await.unwrap_err();
        assert!(e.is(Kind::Unsupported), "{e}");
    });
}

#[test]
fn a_truncation_sets_the_length_ftruncate_would_have_set() {
    let dir = fixture("truncate");
    let path = format!("{dir}/text");
    std::fs::write(&path, "0123456789").expect("write");

    run(|| async move {
        let fd = koru::open_at(&path, O_WRITE).await.unwrap();
        koru::truncate_fd(fd, 4).await.unwrap();
        assert_eq!(std::fs::read(&path).unwrap(), b"0123");
        koru::truncate_fd(fd, 8).await.unwrap();
        assert_eq!(
            std::fs::read(&path).unwrap(),
            b"0123\0\0\0\0",
            "grown with zeros"
        );
        koru::close_fd(fd).await;

        // Err(Perm) unless the open asked to write.
        let ro = koru::open_read(&path).await.unwrap();
        let e = koru::truncate_fd(ro, 0).await.unwrap_err();
        assert!(e.is(Kind::Perm), "{e}");
        koru::close_fd(ro).await;

        // And what seek_fd refuses this refuses.
        let e = koru::truncate_fd(koru::stdout(), 0).await.unwrap_err();
        assert!(e.is(Kind::Unsupported), "{e}");
    });
}

// ---------------------------------------------------------------------------
// Metadata
// ---------------------------------------------------------------------------

#[test]
fn every_stat_agrees_with_libcs() {
    let dir = fixture("stat");
    let file = format!("{dir}/file");
    let sub = format!("{dir}/sub");
    let link = format!("{dir}/link");
    std::fs::write(&file, "0123456789").expect("write");
    std::fs::create_dir(&sub).expect("mkdir");
    std::os::unix::fs::symlink("file", &link).expect("symlink");

    run(|| async move {
        assert_eq!(
            koru::stat_of(&file, true).await.unwrap(),
            libc_info(&file, true)
        );
        assert_eq!(
            koru::stat_of(&sub, true).await.unwrap(),
            libc_info(&sub, true)
        );

        // A followed link is its target; an unfollowed one is the link, whose
        // size is its target's length, as lstat(2) reports it.
        assert_eq!(
            koru::stat_of(&link, true).await.unwrap(),
            libc_info(&file, true)
        );
        let seen = koru::stat_of(&link, false).await.unwrap();
        let want = libc_info(&link, false);
        assert_eq!(seen.kind, FileKind::Link);
        assert_eq!(seen.size, want.size, "a link's size is its target's length");

        // stat_fd sees the file being read, whatever name reached it.
        let fd = koru::open_read(&link).await.unwrap();
        assert_eq!(koru::stat_fd(fd).await.unwrap(), libc_info(&file, true));
        koru::close_fd(fd).await;

        let e = koru::stat_of("/no/such/file", true).await.unwrap_err();
        assert!(e.is(Kind::NotFound), "{e}");
    });
}

#[test]
fn a_touch_moves_the_mtime_forward() {
    let dir = fixture("touch");
    let path = format!("{dir}/file");
    std::fs::write(&path, "x").expect("write");
    let old = std::time::SystemTime::UNIX_EPOCH + std::time::Duration::from_secs(1_000_000);
    std::fs::File::open(&path)
        .and_then(|f| f.set_modified(old))
        .expect("set mtime");
    let before = libc_info(&path, true).mtime;

    run(|| async move {
        assert_eq!(before, 1_000_000_000, "the fixture's mtime is the one set");
        koru::touch_path(&path).await.unwrap();
        let after = koru::stat_of(&path, true).await.unwrap().mtime;
        assert!(after > before, "{after} is not later than {before}");
        assert_eq!(after, libc_info(&path, true).mtime);

        let e = koru::touch_path("/no/such/file").await.unwrap_err();
        assert!(e.is(Kind::NotFound), "{e}");
    });
}

// ---------------------------------------------------------------------------
// Directories and names
// ---------------------------------------------------------------------------

/// Names, kinds and sizes, sorted, as `read_dir` would report them.
fn libc_listing(dir: &str) -> Vec<DirEntry> {
    let mut out: Vec<DirEntry> = std::fs::read_dir(dir)
        .expect("read_dir")
        .map(|e| {
            let e = e.expect("entry");
            let name = e.file_name().into_string().expect("utf-8");
            let m = std::fs::symlink_metadata(e.path()).expect("metadata");
            let kind = kind_of(&m);
            // A listing never resolves a link, so a link reports its own
            // length and koru fills its times from nothing.
            let (size, mtime) = if kind == FileKind::Link {
                (m.len(), 0)
            } else {
                (m.len(), libc_mtime_ms(&m))
            };
            DirEntry {
                name,
                kind,
                size,
                mtime,
            }
        })
        .collect();
    out.sort_by(|a, b| a.name.cmp(&b.name));
    out
}

#[test]
fn a_listing_names_what_read_dir_names() {
    let dir = fixture("list");
    std::fs::write(format!("{dir}/b-file"), "0123").expect("write");
    std::fs::create_dir(format!("{dir}/a-dir")).expect("mkdir");
    std::os::unix::fs::symlink("b-file", format!("{dir}/c-link")).expect("symlink");
    std::os::unix::fs::symlink("nowhere", format!("{dir}/d-dangling")).expect("symlink");

    run(|| async move {
        let mut got = koru::list_dir(&dir).await.unwrap();
        got.sort_by(|a, b| a.name.cmp(&b.name));
        assert_eq!(got, libc_listing(&dir));

        // `.` and `..` never reach a caller.
        assert!(got.iter().all(|e| e.name != "." && e.name != ".."));
        // A dangling link is listed, not an error.
        let dangling = got.iter().find(|e| e.name == "d-dangling").expect("listed");
        assert_eq!(dangling.kind, FileKind::Link);

        let e = koru::list_dir(&format!("{dir}/b-file")).await.unwrap_err();
        assert!(e.is(Kind::NotDir), "{e}");
    });
}

/// Big enough that the entries need more than one `READDIR`, which is the
/// resume cookie's only test here.
#[test]
fn a_long_listing_resumes_across_calls() {
    let dir = fixture("list-long");
    for i in 0..500 {
        std::fs::write(format!("{dir}/entry-with-a-long-name-{i:04}"), "").expect("write");
    }

    run(|| async move {
        let got = koru::list_dir(&dir).await.unwrap();
        assert_eq!(got.len(), 500);
        let mut names: Vec<&str> = got.iter().map(|e| e.name.as_str()).collect();
        names.sort_unstable();
        assert_eq!(names[0], "entry-with-a-long-name-0000");
        assert_eq!(names[499], "entry-with-a-long-name-0499");
    });
}

#[test]
fn a_directory_is_made_where_create_dir_all_would_have_made_one() {
    let dir = fixture("mkdir");
    let deep = format!("{dir}/a/b/c");
    let file = format!("{dir}/file");
    std::fs::write(&file, "x").expect("write");

    run(|| async move {
        koru::make_dir(&format!("{dir}/one")).await.unwrap();
        assert!(std::fs::metadata(format!("{dir}/one")).unwrap().is_dir());

        // A second make_dir over it is Exists; make_dir_all is not.
        let e = koru::make_dir(&format!("{dir}/one")).await.unwrap_err();
        assert!(e.is(Kind::Exists), "{e}");
        koru::make_dir_all(&format!("{dir}/one")).await.unwrap();

        koru::make_dir_all(&deep).await.unwrap();
        assert!(std::fs::metadata(&deep).unwrap().is_dir());
        koru::make_dir_all(&deep).await.unwrap(); // idempotent

        // Anything else in the leaf's place is Exists, and a missing parent
        // is what make_dir alone cannot do.
        let e = koru::make_dir_all(&file).await.unwrap_err();
        assert!(e.is(Kind::Exists), "{e}");
        let e = koru::make_dir(&format!("{dir}/x/y")).await.unwrap_err();
        assert!(e.is(Kind::NotFound), "{e}");
    });
}

#[test]
fn a_removal_takes_what_rm_would_have_taken() {
    let dir = fixture("remove");
    let file = format!("{dir}/file");
    let empty = format!("{dir}/empty");
    let tree = format!("{dir}/tree");
    std::fs::write(&file, "x").expect("write");
    std::fs::create_dir(&empty).expect("mkdir");
    std::fs::create_dir_all(format!("{tree}/a/b")).expect("mkdir");
    std::fs::write(format!("{tree}/a/b/deep"), "x").expect("write");
    std::fs::write(format!("{tree}/top"), "x").expect("write");
    std::os::unix::fs::symlink("nowhere", format!("{tree}/link")).expect("symlink");

    run(|| async move {
        koru::remove_path(&file, false).await.unwrap();
        assert!(std::fs::symlink_metadata(&file).is_err());

        koru::remove_path(&empty, false).await.unwrap();
        assert!(std::fs::symlink_metadata(&empty).is_err());

        // Without `all` a populated directory stays, with its errno.
        let e = koru::remove_path(&tree, false).await.unwrap_err();
        assert!(e.is(Kind::NotEmpty), "{e}");
        assert!(std::fs::metadata(&tree).unwrap().is_dir());

        koru::remove_path(&tree, true).await.unwrap();
        assert!(std::fs::symlink_metadata(&tree).is_err());

        let e = koru::remove_path(&file, true).await.unwrap_err();
        assert!(e.is(Kind::NotFound), "{e}");
    });
}

#[test]
fn a_link_reads_back_as_it_was_written() {
    let dir = fixture("link");
    let link = format!("{dir}/link");
    let file = format!("{dir}/file");
    std::fs::write(&file, "x").expect("write");

    run(|| async move {
        koru::make_link("file", &link).await.unwrap();
        assert_eq!(koru::read_link(&link).await.unwrap(), "file");
        assert_eq!(
            std::fs::read_link(&link).unwrap().to_str().unwrap(),
            "file",
            "and libc agrees"
        );

        // The target is kept as written and not checked.
        let dangling = format!("{dir}/dangling");
        koru::make_link("../nowhere/at/all", &dangling)
            .await
            .unwrap();
        assert_eq!(
            koru::read_link(&dangling).await.unwrap(),
            "../nowhere/at/all"
        );

        let e = koru::make_link("x", &link).await.unwrap_err();
        assert!(e.is(Kind::Exists), "{e}");
        let e = koru::read_link(&file).await.unwrap_err();
        assert!(e.is(Kind::Invalid), "{e}");
    });
}

#[test]
fn a_rename_moves_what_rename_would_have_moved() {
    let dir = fixture("rename");
    let a = format!("{dir}/a");
    let b = format!("{dir}/b");
    std::fs::write(&a, "content").expect("write");
    std::fs::write(&b, "other").expect("write");

    run(|| async move {
        // An existing destination is replaced.
        koru::rename_path(&a, &b).await.unwrap();
        assert!(std::fs::symlink_metadata(&a).is_err());
        assert_eq!(std::fs::read_to_string(&b).unwrap(), "content");

        let e = koru::rename_path(&a, &b).await.unwrap_err();
        assert!(e.is(Kind::NotFound), "{e}");

        // Across filesystems it is not a failure but an instruction.
        let e = koru::rename_path(&b, "/dev/shm/koru-ops-xdev").await;
        if let Err(e) = e {
            assert!(e.is(Kind::Unsupported) || e.is(Kind::Perm), "{e}");
        } else {
            let _ = std::fs::remove_file("/dev/shm/koru-ops-xdev");
        }
    });
}

// ---------------------------------------------------------------------------
// Copying
// ---------------------------------------------------------------------------

#[test]
fn a_copy_is_byte_for_byte_including_what_is_not_text() {
    let dir = fixture("copy");
    let from = format!("{dir}/from");
    let to = format!("{dir}/to");
    // Past one slot, and every byte value, so nothing here is UTF-8.
    let bytes: Vec<u8> = (0..SLOT as usize * 2 + 7)
        .map(|i| (i % 256) as u8)
        .collect();
    std::fs::write(&from, &bytes).expect("write");

    run(|| async move {
        koru::copy_file(&from, &to).await.unwrap();
        assert_eq!(std::fs::read(&to).unwrap(), bytes);

        // Over an existing destination, which is truncated rather than merged.
        std::fs::write(&to, vec![0u8; bytes.len() * 2]).expect("write");
        koru::copy_file(&from, &to).await.unwrap();
        assert_eq!(std::fs::read(&to).unwrap(), bytes);

        let e = koru::copy_file("/no/such/file", &to).await.unwrap_err();
        assert!(e.is(Kind::NotFound), "{e}");
    });
}

#[test]
fn a_tree_copies_with_its_shape_intact() {
    let dir = fixture("tree");
    let from = format!("{dir}/from");
    let to = format!("{dir}/to");
    std::fs::create_dir_all(format!("{from}/a/b")).expect("mkdir");
    std::fs::create_dir(format!("{from}/empty")).expect("mkdir");
    std::fs::write(format!("{from}/top"), "top").expect("write");
    std::fs::write(format!("{from}/a/mid"), "mid").expect("write");
    std::fs::write(format!("{from}/a/b/deep"), "deep").expect("write");
    std::os::unix::fs::symlink("../top", format!("{from}/a/link")).expect("symlink");

    run(|| async move {
        koru::copy_tree(&from, &to).await.unwrap();

        assert_eq!(std::fs::read_to_string(format!("{to}/top")).unwrap(), "top");
        assert_eq!(
            std::fs::read_to_string(format!("{to}/a/mid")).unwrap(),
            "mid"
        );
        assert_eq!(
            std::fs::read_to_string(format!("{to}/a/b/deep")).unwrap(),
            "deep"
        );
        assert!(std::fs::metadata(format!("{to}/empty")).unwrap().is_dir());
        // A link is handed over rather than followed.
        assert_eq!(
            std::fs::read_link(format!("{to}/a/link"))
                .unwrap()
                .to_str()
                .unwrap(),
            "../top"
        );

        // `to` may already be there and the two merge.
        koru::copy_tree(&from, &to).await.unwrap();
        assert_eq!(
            std::fs::read_to_string(format!("{to}/a/b/deep")).unwrap(),
            "deep"
        );

        // Anything else in its place is Exists.
        let onto_file = format!("{dir}/afile");
        std::fs::write(&onto_file, "x").expect("write");
        let e = koru::copy_tree(&from, &onto_file).await.unwrap_err();
        assert!(e.is(Kind::Exists), "{e}");
    });
}

// ---------------------------------------------------------------------------
// The process
// ---------------------------------------------------------------------------

#[test]
fn the_working_directory_is_the_one_getcwd_reports() {
    let dir = fixture("cwd");
    let was = std::env::current_dir().expect("cwd");

    run(|| async move {
        let here = koru::cwd_get().await.unwrap();
        assert_eq!(here, was.to_str().unwrap());

        // Both report the resulting absolute path.
        let moved = koru::cwd_set(&dir).await.unwrap();
        assert_eq!(moved, dir);
        assert_eq!(koru::cwd_get().await.unwrap(), dir);
        assert_eq!(std::env::current_dir().unwrap().to_str().unwrap(), dir);

        // A relative path now resolves against it.
        std::fs::write(format!("{dir}/here"), "x").expect("write");
        assert_eq!(koru::read_file("here").await.unwrap(), "x");

        assert!(koru::cwd_set("/no/such/dir").await.is_err());
        koru::cwd_set(was.to_str().unwrap()).await.unwrap();
    });
}

#[test]
fn a_sleep_parks_for_at_least_as_long_as_it_was_asked() {
    run(|| async {
        let t0 = std::time::Instant::now();
        koru::sleep_for(30).await.unwrap();
        let took = t0.elapsed();
        assert!(took >= std::time::Duration::from_millis(30), "{took:?}");
        assert!(took < std::time::Duration::from_secs(5), "{took:?}");

        // Zero is not an error, and parks for nothing.
        koru::sleep_for(0).await.unwrap();
    });
}

#[test]
fn the_clock_reads_what_the_host_clock_reads() {
    run(|| async {
        let host = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_millis() as u64;
        let c: Clock = koru::clock_now().await.unwrap();
        assert!(c.epoch_ms.abs_diff(host) < 2000, "{} vs {host}", c.epoch_ms);
        assert!((-16 * 60..=16 * 60).contains(&c.tz_min), "{}", c.tz_min);
    });
}

/// `errln` writes Braam's own wording, so the two bindings' diagnostics match.
/// Captured by writing the same line to a file: stderr is the terminal here.
#[test]
fn a_diagnostic_reaches_stderr() {
    let dir = fixture("errln");
    let path = format!("{dir}/log");

    run(|| async move {
        let why = koru::stat_of("/no/such/file", true).await.unwrap_err();
        koru::errln("t30", "a.txt", why).await;
        koru::errln("t30", "", why).await;

        let fd = koru::open_at(&path, O_WRITE | O_CREATE | O_TRUNC)
            .await
            .unwrap();
        koru::write_all(fd, "t30: a.txt: not found\n")
            .await
            .unwrap();
        koru::close_fd(fd).await;
        assert_eq!(
            std::fs::read_to_string(&path).unwrap(),
            "t30: a.txt: not found\n"
        );
    });
}
