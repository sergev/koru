// SPDX-License-Identifier: MIT

//! Canonical text dump of the koru ABI, Rust side. `cpp/tools/abi_dump.cpp`
//! emits the same bytes; `scripts/abi.sh` diffs them. The grammar starts here.
//!
//! One record per line, LF, trailing newline, every number decimal — ioctl
//! numbers included, so a hex mismatch cannot fail the diff for nothing.
//!
//! A record dropped from *both* sides diffs clean, so each struct declares its
//! field count and checks that the field sizes sum to `sizeof`.

use koru_sys::abi::{
    Cqe, KoruEnter, KoruParams, Sqe, handle_generation, handle_index, make_handle,
};
use koru_sys::abi::{
    KORU_ABI_VERSION, KORU_CQE_F_MORE, KORU_DEFAULT_HANDLES, KORU_ENTER_FLAGS_ALL, KORU_IOC_TYPE,
    KORU_MAGIC, KORU_MAX_ARENA_BYTES, KORU_MAX_CQ_ENTRIES, KORU_MAX_DELAY_NS, KORU_MAX_HANDLES,
    KORU_MAX_SLOT_COUNT, KORU_MAX_SLOT_SIZE, KORU_MAX_SQ_ENTRIES, KORU_NR_ENTER,
    KORU_NR_GET_PARAMS, KORU_NR_SETUP, KORU_O_ACCMODE, KORU_O_DIRECTORY, KORU_O_NOFOLLOW,
    KORU_O_NONBLOCK, KORU_O_RDONLY, KORU_O_RDWR, KORU_O_WRONLY, KORU_OP_CANCEL, KORU_OP_CHECKSUM,
    KORU_OP_CLOSE, KORU_OP_DELAY_NS, KORU_OP_NOP, KORU_OP_OPEN, KORU_OP_READ, KORU_OP_WRITE,
    KORU_OPEN_FLAGS_ALL, KORU_SETUP_FLAGS_ALL, KORU_SQE_FLAGS_ALL,
};
use koru_sys::error::{KORU_ERRNOS, Kind};
use koru_sys::ring::DEV_KORU;
use koru_sys::sys::{KORU_IOC_ENTER, KORU_IOC_GET_PARAMS, KORU_IOC_SETUP};
use std::fmt::Write as _;
use std::mem::offset_of;

/// Bumped when the grammar changes, which is an edit to both emitters.
const DUMP_VERSION: u32 = 1;

#[derive(Default)]
struct Counts {
    consts: usize,
    ioctls: usize,
    opcodes: usize,
    structs: usize,
    fields: usize,
    handles: usize,
    kinds: usize,
    errnos: usize,
}

/// One line per field. The type name is spelled out, not derived: it is the
/// only thing here that catches `res` turning unsigned.
macro_rules! fields {
    ($ty:ty, $sname:literal, [$($f:ident : $t:literal),+ $(,)?]) => {{
        let v = <$ty>::default();
        let mut body = String::new();
        let mut n = 0usize;
        let mut sum = 0usize;
        $(
            let size = size_of_val(&v.$f);
            let _ = writeln!(
                body,
                "field {}.{} offset {} size {} type {}",
                $sname,
                stringify!($f),
                offset_of!($ty, $f),
                size,
                $t
            );
            n += 1;
            sum += size;
        )+
        (body, n, sum)
    }};
}

fn emit_struct(
    out: &mut String,
    c: &mut Counts,
    name: &str,
    size: usize,
    align: usize,
    body: String,
    n: usize,
    sum: usize,
) {
    // Catches a field dropped from both emitters at once.
    assert_eq!(
        sum, size,
        "{name}: field sizes sum to {sum}, sizeof is {size}"
    );
    let _ = writeln!(out, "struct {name} size {size} align {align} fields {n}");
    out.push_str(&body);
    c.structs += 1;
    c.fields += n;
}

fn main() {
    let mut out = String::new();
    let mut c = Counts::default();

    let _ = writeln!(out, "abi-dump {DUMP_VERSION}");
    let _ = writeln!(out, "dev {DEV_KORU}");

    let konst = |out: &mut String, c: &mut Counts, name: &str, ty: &str, v: u64| {
        let _ = writeln!(out, "const {name} {ty} {v}");
        c.consts += 1;
    };

    konst(&mut out, &mut c, "KORU_MAGIC", "u32", KORU_MAGIC as u64);
    konst(
        &mut out,
        &mut c,
        "KORU_ABI_VERSION",
        "u32",
        KORU_ABI_VERSION as u64,
    );
    konst(
        &mut out,
        &mut c,
        "KORU_IOC_TYPE",
        "u32",
        KORU_IOC_TYPE as u64,
    );
    konst(
        &mut out,
        &mut c,
        "KORU_NR_SETUP",
        "u32",
        KORU_NR_SETUP as u64,
    );
    konst(
        &mut out,
        &mut c,
        "KORU_NR_GET_PARAMS",
        "u32",
        KORU_NR_GET_PARAMS as u64,
    );
    konst(
        &mut out,
        &mut c,
        "KORU_NR_ENTER",
        "u32",
        KORU_NR_ENTER as u64,
    );
    konst(
        &mut out,
        &mut c,
        "KORU_SETUP_FLAGS_ALL",
        "u32",
        KORU_SETUP_FLAGS_ALL as u64,
    );
    konst(
        &mut out,
        &mut c,
        "KORU_ENTER_FLAGS_ALL",
        "u32",
        KORU_ENTER_FLAGS_ALL as u64,
    );
    konst(
        &mut out,
        &mut c,
        "KORU_SQE_FLAGS_ALL",
        "u8",
        KORU_SQE_FLAGS_ALL as u64,
    );
    konst(
        &mut out,
        &mut c,
        "KORU_MAX_SQ_ENTRIES",
        "u32",
        KORU_MAX_SQ_ENTRIES as u64,
    );
    konst(
        &mut out,
        &mut c,
        "KORU_MAX_CQ_ENTRIES",
        "u32",
        KORU_MAX_CQ_ENTRIES as u64,
    );
    konst(
        &mut out,
        &mut c,
        "KORU_MAX_SLOT_SIZE",
        "u32",
        KORU_MAX_SLOT_SIZE as u64,
    );
    konst(
        &mut out,
        &mut c,
        "KORU_MAX_SLOT_COUNT",
        "u32",
        KORU_MAX_SLOT_COUNT as u64,
    );
    konst(
        &mut out,
        &mut c,
        "KORU_MAX_ARENA_BYTES",
        "u64",
        KORU_MAX_ARENA_BYTES,
    );
    konst(
        &mut out,
        &mut c,
        "KORU_MAX_HANDLES",
        "u32",
        KORU_MAX_HANDLES as u64,
    );
    konst(
        &mut out,
        &mut c,
        "KORU_DEFAULT_HANDLES",
        "u32",
        KORU_DEFAULT_HANDLES as u64,
    );
    konst(
        &mut out,
        &mut c,
        "KORU_MAX_DELAY_NS",
        "u64",
        KORU_MAX_DELAY_NS,
    );
    konst(
        &mut out,
        &mut c,
        "KORU_O_ACCMODE",
        "u32",
        KORU_O_ACCMODE as u64,
    );
    konst(
        &mut out,
        &mut c,
        "KORU_O_RDONLY",
        "u32",
        KORU_O_RDONLY as u64,
    );
    konst(
        &mut out,
        &mut c,
        "KORU_O_WRONLY",
        "u32",
        KORU_O_WRONLY as u64,
    );
    konst(&mut out, &mut c, "KORU_O_RDWR", "u32", KORU_O_RDWR as u64);
    konst(
        &mut out,
        &mut c,
        "KORU_O_NOFOLLOW",
        "u32",
        KORU_O_NOFOLLOW as u64,
    );
    konst(
        &mut out,
        &mut c,
        "KORU_O_DIRECTORY",
        "u32",
        KORU_O_DIRECTORY as u64,
    );
    konst(
        &mut out,
        &mut c,
        "KORU_O_NONBLOCK",
        "u32",
        KORU_O_NONBLOCK as u64,
    );
    konst(
        &mut out,
        &mut c,
        "KORU_OPEN_FLAGS_ALL",
        "u32",
        KORU_OPEN_FLAGS_ALL as u64,
    );
    konst(
        &mut out,
        &mut c,
        "KORU_CQE_F_MORE",
        "u32",
        KORU_CQE_F_MORE as u64,
    );

    for (name, v) in [
        ("KORU_IOC_SETUP", KORU_IOC_SETUP),
        ("KORU_IOC_GET_PARAMS", KORU_IOC_GET_PARAMS),
        ("KORU_IOC_ENTER", KORU_IOC_ENTER),
    ] {
        let _ = writeln!(out, "ioctl {name} {v}");
        c.ioctls += 1;
    }

    for (name, v) in [
        ("KORU_OP_NOP", KORU_OP_NOP),
        ("KORU_OP_DELAY_NS", KORU_OP_DELAY_NS),
        ("KORU_OP_OPEN", KORU_OP_OPEN),
        ("KORU_OP_READ", KORU_OP_READ),
        ("KORU_OP_CLOSE", KORU_OP_CLOSE),
        ("KORU_OP_CANCEL", KORU_OP_CANCEL),
        ("KORU_OP_CHECKSUM", KORU_OP_CHECKSUM),
        ("KORU_OP_WRITE", KORU_OP_WRITE),
    ] {
        let _ = writeln!(out, "opcode {name} {v}");
        c.opcodes += 1;
    }

    let (body, n, sum) = fields!(KoruParams, "koru_params", [
        magic: "u32",
        abi_version: "u32",
        flags: "u32",
        sq_entries: "u32",
        cq_entries: "u32",
        slot_size: "u32",
        slot_count: "u32",
        configured: "u32",
        features: "u64",
        arena_size: "u64",
        max_sq_entries: "u32",
        max_cq_entries: "u32",
        max_slot_size: "u32",
        max_slot_count: "u32",
        max_arena_bytes: "u64",
        handle_count: "u32",
        max_handles: "u32",
        max_delay_ns: "u64",
        reserved: "u64[2]",
    ]);
    emit_struct(
        &mut out,
        &mut c,
        "koru_params",
        size_of::<KoruParams>(),
        align_of::<KoruParams>(),
        body,
        n,
        sum,
    );

    let (body, n, sum) = fields!(Sqe, "koru_sqe", [
        opcode: "u8",
        flags: "u8",
        rsvd0: "u16",
        len: "u32",
        off: "u64",
        user_data: "u64",
        slot: "u32",
        handle: "u32",
    ]);
    emit_struct(
        &mut out,
        &mut c,
        "koru_sqe",
        size_of::<Sqe>(),
        align_of::<Sqe>(),
        body,
        n,
        sum,
    );

    let (body, n, sum) = fields!(Cqe, "koru_cqe", [
        user_data: "u64",
        res: "i64",
        flags: "u32",
        rsvd0: "u32",
        extra: "u64",
    ]);
    emit_struct(
        &mut out,
        &mut c,
        "koru_cqe",
        size_of::<Cqe>(),
        align_of::<Cqe>(),
        body,
        n,
        sum,
    );

    let (body, n, sum) = fields!(KoruEnter, "koru_enter", [
        sq_addr: "u64",
        cq_addr: "u64",
        timeout_ns: "u64",
        to_submit: "u32",
        cq_space: "u32",
        min_complete: "u32",
        flags: "u32",
        completed: "u32",
        submitted: "u32",
        reserved: "u64[2]",
    ]);
    emit_struct(
        &mut out,
        &mut c,
        "koru_enter",
        size_of::<KoruEnter>(),
        align_of::<KoruEnter>(),
        body,
        n,
        sum,
    );

    // Evaluated vectors, so the const fns and the C macros are diffed too.
    for (index, generation) in [(0u16, 1u16), (7, 3), (4095, 1), (65535, 65535)] {
        let h = make_handle(index, generation);
        let _ = writeln!(
            out,
            "handle {index} {generation} {h} {} {}",
            handle_index(h),
            handle_generation(h)
        );
        c.handles += 1;
    }

    // Closed has no errno preimage, so only this loop emits it.
    for k in [
        Kind::Invalid,
        Kind::NoMemory,
        Kind::NotFound,
        Kind::Exists,
        Kind::NotDir,
        Kind::IsDir,
        Kind::Perm,
        Kind::Io,
        Kind::Cancelled,
        Kind::Again,
        Kind::Unsupported,
        Kind::Closed,
        Kind::NotEmpty,
        Kind::Loop,
        Kind::Intr,
    ] {
        let _ = writeln!(out, "kind {k:?} {}", k as u8);
        c.kinds += 1;
    }

    // Assert the order rather than sorting: a misplaced row must fail.
    let mut prev = 0i32;
    for d in KORU_ERRNOS {
        assert!(d.errno.0 > prev, "{} is out of order or duplicated", d.name);
        prev = d.errno.0;
        let _ = writeln!(
            out,
            "errno {} {} {:?} {}",
            d.name, d.errno.0, d.kind, d.kind as u8
        );
        c.errnos += 1;
    }

    let _ = writeln!(
        out,
        "counts consts {} ioctls {} opcodes {} structs {} fields {} handles {} kinds {} errnos {}",
        c.consts, c.ioctls, c.opcodes, c.structs, c.fields, c.handles, c.kinds, c.errnos
    );

    print!("{out}");
}
