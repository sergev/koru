// SPDX-License-Identifier: MIT

//! Canonical text dump of the screen protocol, Rust side.
//! `screen/tools/ks_dump.cpp` emits the same bytes; `scripts/abi.sh` diffs
//! them. The grammar is T14's, plus `opvec` and `style`, which evaluate the
//! validation helpers rather than the constants they are built from.

use koru::ks_abi::*;
use std::fmt::Write as _;
use std::mem::offset_of;

/// Bumped when the grammar changes, which is an edit to both emitters.
const DUMP_VERSION: u32 = 1;

#[derive(Default)]
struct Counts {
    consts: usize,
    ops: usize,
    opvecs: usize,
    styles: usize,
    structs: usize,
    fields: usize,
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

macro_rules! emit {
    ($out:expr, $c:expr, $ty:ty, $name:literal, [$($f:ident : $t:literal),+ $(,)?]) => {{
        let (body, n, sum) = fields!($ty, $name, [$($f: $t),+]);
        emit_struct($out, $c, $name, size_of::<$ty>(), align_of::<$ty>(), body, n, sum);
    }};
}

fn main() {
    let mut out = String::new();
    let mut c = Counts::default();

    let _ = writeln!(out, "ks-dump {DUMP_VERSION}");
    let _ = writeln!(out, "sock {KS_SOCK_NAME}");

    let mut konst = |out: &mut String, name: &str, ty: &str, v: u64| {
        let _ = writeln!(out, "const {name} {ty} {v}");
        c.consts += 1;
    };

    konst(&mut out, "KS_MAGIC", "u32", KS_MAGIC as u64);
    konst(&mut out, "KS_ABI_VERSION", "u32", KS_ABI_VERSION as u64);
    konst(&mut out, "KS_ALIGN", "u32", KS_ALIGN as u64);
    konst(
        &mut out,
        "KS_SEQ_UNSOLICITED",
        "u32",
        KS_SEQ_UNSOLICITED as u64,
    );
    konst(&mut out, "KS_MAX_FRAME", "u32", KS_MAX_FRAME as u64);
    konst(&mut out, "KS_OP_MAX", "u32", KS_OP_MAX as u64);
    konst(&mut out, "KS_F_TAKE", "u16", KS_F_TAKE as u64);
    konst(&mut out, "KS_F_SET", "u16", KS_F_SET as u64);
    konst(&mut out, "KS_F_STALE", "u16", KS_F_STALE as u64);
    konst(&mut out, "KS_F_BYTES", "u16", KS_F_BYTES as u64);
    konst(&mut out, "KS_F_ECHO_SHOW", "u16", KS_F_ECHO_SHOW as u64);
    konst(&mut out, "KS_F_ECHO_FRESH", "u16", KS_F_ECHO_FRESH as u64);
    konst(&mut out, "KS_F_ECHO_END", "u16", KS_F_ECHO_END as u64);
    konst(&mut out, "KS_LEN_VARIABLE", "u32", KS_LEN_VARIABLE as u64);
    konst(&mut out, "KS_LEN_NONE", "u32", KS_LEN_NONE as u64);
    konst(&mut out, "KS_TTY_CONSOLE", "u32", KS_TTY_CONSOLE as u64);
    konst(&mut out, "KS_ECHO_RUNS_MAX", "u32", KS_ECHO_RUNS_MAX as u64);
    konst(&mut out, "KS_ATTR_BOLD", "u8", KS_ATTR_BOLD as u64);
    konst(
        &mut out,
        "KS_ATTR_UNDERLINE",
        "u8",
        KS_ATTR_UNDERLINE as u64,
    );
    konst(&mut out, "KS_ATTR_REVERSE", "u8", KS_ATTR_REVERSE as u64);
    konst(&mut out, "KS_ATTRS_ALL", "u8", KS_ATTRS_ALL as u64);
    konst(&mut out, "KS_COLOR_BLACK", "u8", KS_COLOR_BLACK as u64);
    konst(&mut out, "KS_COLOR_RED", "u8", KS_COLOR_RED as u64);
    konst(&mut out, "KS_COLOR_GREEN", "u8", KS_COLOR_GREEN as u64);
    konst(&mut out, "KS_COLOR_YELLOW", "u8", KS_COLOR_YELLOW as u64);
    konst(&mut out, "KS_COLOR_BLUE", "u8", KS_COLOR_BLUE as u64);
    konst(&mut out, "KS_COLOR_MAGENTA", "u8", KS_COLOR_MAGENTA as u64);
    konst(&mut out, "KS_COLOR_CYAN", "u8", KS_COLOR_CYAN as u64);
    konst(&mut out, "KS_COLOR_WHITE", "u8", KS_COLOR_WHITE as u64);
    konst(&mut out, "KS_COLOR_BRIGHT", "u8", KS_COLOR_BRIGHT as u64);
    konst(&mut out, "KS_COLORS", "u32", KS_COLORS as u64);
    konst(&mut out, "KS_MAX_COLS", "u32", KS_MAX_COLS as u64);
    konst(&mut out, "KS_MAX_ROWS", "u32", KS_MAX_ROWS as u64);
    konst(&mut out, "KS_PAGE_SIZE", "u32", KS_PAGE_SIZE as u64);
    konst(&mut out, "KS_MIN_SLOT", "u32", KS_MIN_SLOT as u64);
    konst(&mut out, "KS_STYLE_KEEP", "u32", KS_STYLE_KEEP as u64);
    konst(&mut out, "KS_MOD_SHIFT", "u32", KS_MOD_SHIFT as u64);
    konst(&mut out, "KS_MOD_CTRL", "u32", KS_MOD_CTRL as u64);
    konst(&mut out, "KS_MOD_ALT", "u32", KS_MOD_ALT as u64);
    konst(&mut out, "KS_MOD_META", "u32", KS_MOD_META as u64);
    konst(&mut out, "KS_MODS_ALL", "u32", KS_MODS_ALL as u64);
    konst(&mut out, "KS_KEY_NAMED", "u32", KS_KEY_NAMED as u64);
    konst(&mut out, "KS_KEY_MAX", "u32", KS_KEY_MAX as u64);
    konst(&mut out, "KS_EINTR", "i32", KS_EINTR as u64);
    konst(&mut out, "KS_EBUSY", "i32", KS_EBUSY as u64);
    konst(&mut out, "KS_EINVAL", "i32", KS_EINVAL as u64);
    konst(&mut out, "KS_ENOTTY", "i32", KS_ENOTTY as u64);
    konst(&mut out, "KS_EPROTO", "i32", KS_EPROTO as u64);
    konst(&mut out, "KS_ENOSYS", "i32", KS_ENOSYS as u64);
    konst(&mut out, "KS_EPERM", "i32", KS_EPERM as u64);

    for (name, v) in [
        ("KS_OP_HELLO", KS_OP_HELLO),
        ("KS_OP_KEY_CLAIM", KS_OP_KEY_CLAIM),
        ("KS_OP_KEY_READ", KS_OP_KEY_READ),
        ("KS_OP_SCREEN_CLAIM", KS_OP_SCREEN_CLAIM),
        ("KS_OP_BLIT", KS_OP_BLIT),
        ("KS_OP_CURSOR", KS_OP_CURSOR),
        ("KS_OP_ECHO", KS_OP_ECHO),
        ("KS_OP_STYLE", KS_OP_STYLE),
        ("KS_OP_SCREEN_CLEAR", KS_OP_SCREEN_CLEAR),
        ("KS_OP_TTY", KS_OP_TTY),
        ("KS_OP_TERM_OPEN", KS_OP_TERM_OPEN),
    ] {
        let _ = writeln!(out, "op {name} {v}");
        c.ops += 1;
    }

    // The named keys, every one, because a client that renumbers them draws
    // the wrong thing rather than failing.
    for (name, v) in [
        ("KS_KEY_ENTER", KS_KEY_ENTER),
        ("KS_KEY_BACKSPACE", KS_KEY_BACKSPACE),
        ("KS_KEY_TAB", KS_KEY_TAB),
        ("KS_KEY_ESCAPE", KS_KEY_ESCAPE),
        ("KS_KEY_DELETE", KS_KEY_DELETE),
        ("KS_KEY_INSERT", KS_KEY_INSERT),
        ("KS_KEY_UP", KS_KEY_UP),
        ("KS_KEY_DOWN", KS_KEY_DOWN),
        ("KS_KEY_LEFT", KS_KEY_LEFT),
        ("KS_KEY_RIGHT", KS_KEY_RIGHT),
        ("KS_KEY_HOME", KS_KEY_HOME),
        ("KS_KEY_END", KS_KEY_END),
        ("KS_KEY_PAGE_UP", KS_KEY_PAGE_UP),
        ("KS_KEY_PAGE_DOWN", KS_KEY_PAGE_DOWN),
        ("KS_KEY_F1", KS_KEY_F1),
        ("KS_KEY_F2", KS_KEY_F2),
        ("KS_KEY_F3", KS_KEY_F3),
        ("KS_KEY_F4", KS_KEY_F4),
        ("KS_KEY_F5", KS_KEY_F5),
        ("KS_KEY_F6", KS_KEY_F6),
        ("KS_KEY_F7", KS_KEY_F7),
        ("KS_KEY_F8", KS_KEY_F8),
        ("KS_KEY_F9", KS_KEY_F9),
        ("KS_KEY_F10", KS_KEY_F10),
        ("KS_KEY_F11", KS_KEY_F11),
        ("KS_KEY_F12", KS_KEY_F12),
    ] {
        let _ = writeln!(out, "key {name} {v}");
        c.ops += 1;
    }

    emit!(&mut out, &mut c, KsHead, "ks_head", [
        len: "u32",
        op: "u16",
        flags: "u16",
        seq: "u32",
        res: "i32",
    ]);
    emit!(&mut out, &mut c, KsHello, "ks_hello", [
        magic: "u32",
        version: "u32",
    ]);
    emit!(&mut out, &mut c, KsHelloRep, "ks_hello_rep", [
        magic: "u32",
        version: "u32",
        cols: "u32",
        rows: "u32",
        max_cols: "u32",
        max_rows: "u32",
        max_frame: "u32",
        features: "u32",
    ]);
    emit!(&mut out, &mut c, KsGeom, "ks_geom", [
        cols: "u32",
        rows: "u32",
    ]);
    emit!(&mut out, &mut c, KsKey, "ks_key", [
        code: "u32",
        mods: "u32",
        cols: "u32",
        rows: "u32",
    ]);
    emit!(&mut out, &mut c, KsBlit, "ks_blit", [
        x: "u32",
        y: "u32",
        w: "u32",
        h: "u32",
        cursor_x: "u32",
        cursor_y: "u32",
        cursor_on: "u32",
        cols: "u32",
        rows: "u32",
        rsvd0: "u32",
    ]);
    emit!(&mut out, &mut c, KsCell, "ks_cell", [
        ch: "u32",
        fg: "u8",
        bg: "u8",
        attrs: "u8",
        rsvd0: "u8",
    ]);
    emit!(&mut out, &mut c, KsCursorReq, "ks_cursor_req", [
        x: "u32",
        y: "u32",
        on: "u32",
        rsvd0: "u32",
    ]);
    emit!(&mut out, &mut c, KsCursor, "ks_cursor", [
        x: "u32",
        y: "u32",
        on: "u32",
        cols: "u32",
        rows: "u32",
        scrolled: "u32",
    ]);
    emit!(&mut out, &mut c, KsEcho, "ks_echo", [
        x: "u32",
        y: "u32",
        cur: "u32",
        runs: "u32",
    ]);
    emit!(&mut out, &mut c, KsRun, "ks_run", [
        style: "u32",
        len: "u32",
    ]);
    emit!(&mut out, &mut c, KsStyle, "ks_style", [
        style: "u32",
        rsvd0: "u32",
    ]);
    emit!(&mut out, &mut c, KsTty, "ks_tty", [
        flags: "u32",
        cols: "u32",
        rows: "u32",
        rsvd0: "u32",
    ]);

    // Evaluated over every op number and one past the end, so the validation
    // helpers are diffed rather than the constants they are built from.
    for op in 0..=KS_OP_MAX {
        let _ = writeln!(
            out,
            "opvec {op} flags {} req {} rep {} err {} eintr {}",
            ks_flags_all(op),
            ks_req_len(op),
            ks_rep_len(op),
            ks_err_len(op, -KS_EINVAL),
            ks_err_len(op, -KS_EINTR)
        );
        c.opvecs += 1;
    }

    for (fg, bg, attrs) in [
        (0u8, 0u8, 0u8),
        (KS_COLOR_RED, KS_COLOR_BLACK, KS_ATTR_BOLD),
        (
            KS_COLOR_WHITE + KS_COLOR_BRIGHT,
            KS_COLOR_BLUE,
            KS_ATTRS_ALL,
        ),
        (255, 255, 255),
    ] {
        let s = ks_style_pack(fg, bg, attrs);
        let _ = writeln!(
            out,
            "style {fg} {bg} {attrs} packed {s} fg {} bg {} attrs {}",
            ks_style_fg(s),
            ks_style_bg(s),
            ks_style_attrs(s)
        );
        c.styles += 1;
    }

    let _ = writeln!(
        out,
        "counts consts {} ops {} opvecs {} styles {} structs {} fields {}",
        c.consts, c.ops, c.opvecs, c.styles, c.structs, c.fields
    );

    print!("{out}");
}
