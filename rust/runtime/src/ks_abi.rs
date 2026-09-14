// SPDX-License-Identifier: MIT

//! The koru-screen protocol, Rust side. Mirrors `screen/ks_abi.h`, which is
//! canonical; `ks_dump` diffs the two. Pure layout and validation arithmetic:
//! no ring, no socket, no koru-sys.
//!
//! `seq` is this protocol's space, **not** koru's `user_data`. doc/Notes.md
//! says why they cannot be the same number.

#![allow(dead_code)]

/// Spells "kscr" little-endian, as `KORU_MAGIC` spells "koru".
pub const KS_MAGIC: u32 = 0x7263_736b;

/// Bumped on any incompatible change. Checked at [`KS_OP_HELLO`], before
/// anything is claimed.
pub const KS_ABI_VERSION: u32 = 1;

/// The socket's basename, under `$XDG_RUNTIME_DIR` unless `$KORU_SCREEN_SOCK`
/// names a path.
pub const KS_SOCK_NAME: &str = "koru-screen.sock";

/// Every frame's length is a multiple of this, which keeps a blit's cells
/// 8-aligned.
pub const KS_ALIGN: u32 = 8;

/// Reserved for unsolicited frames. A request carrying it is `-EPROTO`.
pub const KS_SEQ_UNSOLICITED: u32 = 0;

/// The largest frame either side will read or write.
pub const KS_MAX_FRAME: u32 = 65536;

// ---------------------------------------------------------------------------
// The errnos
// ---------------------------------------------------------------------------
//
// asm-generic values, spelled out: the daemon has no koru dependency, so the
// header cannot include the errno table and the mirror must not either.

pub const KS_EINTR: i32 = 4;
pub const KS_EBUSY: i32 = 16;
pub const KS_EINVAL: i32 = 22;
pub const KS_ENOTTY: i32 = 25;
pub const KS_EPROTO: i32 = 71;
pub const KS_ENOSYS: i32 = 38;
pub const KS_EPERM: i32 = 1;

// ---------------------------------------------------------------------------
// The ops
// ---------------------------------------------------------------------------

pub const KS_OP_HELLO: u32 = 1;
pub const KS_OP_KEY_CLAIM: u32 = 2;
pub const KS_OP_KEY_READ: u32 = 3;
pub const KS_OP_SCREEN_CLAIM: u32 = 4;
pub const KS_OP_BLIT: u32 = 5;
pub const KS_OP_CURSOR: u32 = 6;
pub const KS_OP_ECHO: u32 = 7;
pub const KS_OP_STYLE: u32 = 8;
pub const KS_OP_SCREEN_CLEAR: u32 = 9;
pub const KS_OP_TTY: u32 = 10;
pub const KS_OP_TERM_OPEN: u32 = 11;

/// One past the last. An op at or above it is `-ENOSYS`, never `-EINVAL`.
pub const KS_OP_MAX: u32 = 12;

// ---------------------------------------------------------------------------
// The flags
// ---------------------------------------------------------------------------

pub const KS_F_TAKE: u16 = 1;
pub const KS_F_SET: u16 = 1;
/// A `BLIT` reply's: the geometry moved, so nothing was drawn.
pub const KS_F_STALE: u16 = 1;

pub const KS_F_ECHO_SHOW: u16 = 1;
pub const KS_F_ECHO_FRESH: u16 = 2;
pub const KS_F_ECHO_END: u16 = 4;

/// Every flag this op accepts on a *request*. [`KS_F_STALE`] is a reply's.
pub const fn ks_flags_all(op: u32) -> u16 {
    match op {
        KS_OP_KEY_CLAIM | KS_OP_SCREEN_CLAIM => KS_F_TAKE,
        KS_OP_CURSOR => KS_F_SET,
        KS_OP_ECHO => KS_F_ECHO_SHOW | KS_F_ECHO_FRESH | KS_F_ECHO_END,
        _ => 0,
    }
}

// ---------------------------------------------------------------------------
// The payloads
// ---------------------------------------------------------------------------

/// Every frame is this and then that op's payload.
#[repr(C)]
#[derive(Copy, Clone, Default, Debug, PartialEq, Eq)]
pub struct KsHead {
    /// The whole frame, a multiple of [`KS_ALIGN`].
    pub len: u32,
    pub op: u16,
    /// A bit outside this op's mask is `-EINVAL`.
    pub flags: u16,
    /// Client-chosen, echoed; 0 is reserved.
    pub seq: u32,
    /// Request: must be 0. Reply: 0, or `-errno`.
    pub res: i32,
}

#[repr(C)]
#[derive(Copy, Clone, Default, Debug, PartialEq, Eq)]
pub struct KsHello {
    pub magic: u32,
    pub version: u32,
}

#[repr(C)]
#[derive(Copy, Clone, Default, Debug, PartialEq, Eq)]
pub struct KsHelloRep {
    pub magic: u32,
    pub version: u32,
    pub cols: u32,
    pub rows: u32,
    pub max_cols: u32,
    pub max_rows: u32,
    pub max_frame: u32,
    pub features: u32,
}

/// The geometry every terminal reply carries, so a resize needs no event.
#[repr(C)]
#[derive(Copy, Clone, Default, Debug, PartialEq, Eq)]
pub struct KsGeom {
    pub cols: u32,
    pub rows: u32,
}

/// `KEY_READ`'s reply. `^C` is `'c'` with [`KS_MOD_CTRL`], never byte 3.
#[repr(C)]
#[derive(Copy, Clone, Default, Debug, PartialEq, Eq)]
pub struct KsKey {
    pub code: u32,
    pub mods: u32,
    pub cols: u32,
    pub rows: u32,
}

/// `BLIT`'s payload header, followed by `w * h` cells, row by row. `cols` and
/// `rows` are the geometry the client believed when it packed this.
#[repr(C)]
#[derive(Copy, Clone, Default, Debug, PartialEq, Eq)]
pub struct KsBlit {
    pub x: u32,
    pub y: u32,
    pub w: u32,
    pub h: u32,
    pub cursor_x: u32,
    pub cursor_y: u32,
    pub cursor_on: u32,
    pub cols: u32,
    pub rows: u32,
    /// Must be zero, and pads the cells to [`KS_ALIGN`].
    pub rsvd0: u32,
}

/// Braam's `Cell`, byte for byte.
#[repr(C)]
#[derive(Copy, Clone, Default, Debug, PartialEq, Eq)]
pub struct KsCell {
    /// 0 is blank.
    pub ch: u32,
    pub fg: u8,
    pub bg: u8,
    pub attrs: u8,
    pub rsvd0: u8,
}

#[repr(C)]
#[derive(Copy, Clone, Default, Debug, PartialEq, Eq)]
pub struct KsCursorReq {
    pub x: u32,
    pub y: u32,
    pub on: u32,
    pub rsvd0: u32,
}

/// `CURSOR`'s reply, and `ECHO`'s. `scrolled` is 0 from `CURSOR`.
#[repr(C)]
#[derive(Copy, Clone, Default, Debug, PartialEq, Eq)]
pub struct KsCursor {
    pub x: u32,
    pub y: u32,
    pub on: u32,
    pub cols: u32,
    pub rows: u32,
    pub scrolled: u32,
}

#[repr(C)]
#[derive(Copy, Clone, Default, Debug, PartialEq, Eq)]
pub struct KsEcho {
    pub x: u32,
    pub y: u32,
    pub cur: u32,
    pub runs: u32,
}

#[repr(C)]
#[derive(Copy, Clone, Default, Debug, PartialEq, Eq)]
pub struct KsRun {
    /// [`ks_style_pack`], or [`KS_STYLE_KEEP`].
    pub style: u32,
    pub len: u32,
}

#[repr(C)]
#[derive(Copy, Clone, Default, Debug, PartialEq, Eq)]
pub struct KsStyle {
    pub style: u32,
    pub rsvd0: u32,
}

#[repr(C)]
#[derive(Copy, Clone, Default, Debug, PartialEq, Eq)]
pub struct KsTty {
    pub flags: u32,
    pub cols: u32,
    pub rows: u32,
    pub rsvd0: u32,
}

pub const KS_TTY_CONSOLE: u32 = 1;
pub const KS_ECHO_RUNS_MAX: u32 = 8;

// Size alone would not catch two fields being swapped, so assert every offset.
const _: () = assert!(size_of::<KsHead>() == 16);
const _: () = assert!(align_of::<KsHead>() == 4);
const _: () = assert!(core::mem::offset_of!(KsHead, len) == 0);
const _: () = assert!(core::mem::offset_of!(KsHead, op) == 4);
const _: () = assert!(core::mem::offset_of!(KsHead, flags) == 6);
const _: () = assert!(core::mem::offset_of!(KsHead, seq) == 8);
const _: () = assert!(core::mem::offset_of!(KsHead, res) == 12);
const _: () = assert!((size_of::<KsHead>() as u32).is_multiple_of(KS_ALIGN));

const _: () = assert!(size_of::<KsHello>() == 8);
const _: () = assert!(size_of::<KsHelloRep>() == 32);
const _: () = assert!(size_of::<KsGeom>() == 8);
const _: () = assert!(size_of::<KsKey>() == 16);
const _: () = assert!(size_of::<KsBlit>() == 40);
const _: () = assert!(core::mem::offset_of!(KsBlit, cols) == 28);
const _: () = assert!(core::mem::offset_of!(KsBlit, rsvd0) == 36);
const _: () = assert!(size_of::<KsCursorReq>() == 16);
const _: () = assert!(size_of::<KsCursor>() == 24);
const _: () = assert!(size_of::<KsEcho>() == 16);
const _: () = assert!(size_of::<KsRun>() == 8);
const _: () = assert!(size_of::<KsStyle>() == 8);
const _: () = assert!(size_of::<KsTty>() == 16);

// "the renderer strides by 8 bytes", which is Braam's own assertion text.
const _: () = assert!(size_of::<KsCell>() == 8);
const _: () = assert!(core::mem::offset_of!(KsCell, fg) == 4);
const _: () = assert!(core::mem::offset_of!(KsCell, rsvd0) == 7);

// Cells are 8-aligned inside the frame, which is why `KsBlit` carries a
// padding word. Removing it must fire this.
const _: () =
    assert!(((size_of::<KsHead>() + size_of::<KsBlit>()) as u32).is_multiple_of(KS_ALIGN));

// Every payload is a multiple of KS_ALIGN, so `len` is one whatever the op.
const _: () = assert!((size_of::<KsHelloRep>() as u32).is_multiple_of(KS_ALIGN));
const _: () = assert!((size_of::<KsCursor>() as u32).is_multiple_of(KS_ALIGN));

// ---------------------------------------------------------------------------
// Frame sizes
// ---------------------------------------------------------------------------

/// `BLIT` and `ECHO`, whose length is their content.
pub const KS_LEN_VARIABLE: u32 = 0xffff_ffff;
/// No such op.
pub const KS_LEN_NONE: u32 = 0;

const HEAD: u32 = size_of::<KsHead>() as u32;

pub const fn ks_req_len(op: u32) -> u32 {
    match op {
        KS_OP_HELLO => HEAD + size_of::<KsHello>() as u32,
        KS_OP_KEY_CLAIM | KS_OP_KEY_READ | KS_OP_SCREEN_CLAIM | KS_OP_SCREEN_CLEAR | KS_OP_TTY
        | KS_OP_TERM_OPEN => HEAD,
        KS_OP_BLIT | KS_OP_ECHO => KS_LEN_VARIABLE,
        KS_OP_CURSOR => HEAD + size_of::<KsCursorReq>() as u32,
        KS_OP_STYLE => HEAD + size_of::<KsStyle>() as u32,
        _ => KS_LEN_NONE,
    }
}

pub const fn ks_rep_len(op: u32) -> u32 {
    match op {
        KS_OP_HELLO => HEAD + size_of::<KsHelloRep>() as u32,
        KS_OP_KEY_CLAIM | KS_OP_SCREEN_CLAIM | KS_OP_BLIT => HEAD + size_of::<KsGeom>() as u32,
        KS_OP_KEY_READ => HEAD + size_of::<KsKey>() as u32,
        KS_OP_CURSOR | KS_OP_ECHO => HEAD + size_of::<KsCursor>() as u32,
        KS_OP_STYLE | KS_OP_SCREEN_CLEAR | KS_OP_TERM_OPEN => HEAD,
        KS_OP_TTY => HEAD + size_of::<KsTty>() as u32,
        _ => KS_LEN_NONE,
    }
}

/// A failed request replies with the header alone. The one exception is
/// `KEY_READ`'s `-EINTR`, which carries a whole [`KsKey`] so that a resize is
/// answered rather than signalled — the only negative `res` with a payload.
pub const fn ks_err_len(op: u32, res: i32) -> u32 {
    if op == KS_OP_KEY_READ && res == -KS_EINTR {
        HEAD + size_of::<KsKey>() as u32
    } else {
        HEAD
    }
}

// ---------------------------------------------------------------------------
// The cells
// ---------------------------------------------------------------------------

pub const KS_ATTR_BOLD: u8 = 1;
pub const KS_ATTR_UNDERLINE: u8 = 2;
pub const KS_ATTR_REVERSE: u8 = 4;
pub const KS_ATTRS_ALL: u8 = KS_ATTR_BOLD | KS_ATTR_UNDERLINE | KS_ATTR_REVERSE;

pub const KS_COLOR_BLACK: u8 = 0;
pub const KS_COLOR_RED: u8 = 1;
pub const KS_COLOR_GREEN: u8 = 2;
pub const KS_COLOR_YELLOW: u8 = 3;
pub const KS_COLOR_BLUE: u8 = 4;
pub const KS_COLOR_MAGENTA: u8 = 5;
pub const KS_COLOR_CYAN: u8 = 6;
pub const KS_COLOR_WHITE: u8 = 7;
/// Added to any of the above.
pub const KS_COLOR_BRIGHT: u8 = 8;
pub const KS_COLORS: u32 = 16;

pub const KS_MAX_COLS: u32 = 512;
pub const KS_MAX_ROWS: u32 = 256;

/// A maximum-width row of cells is exactly one page, so a client whose slot is
/// [`KS_MIN_SLOT`] can always send a whole row and banding cannot get stuck.
pub const KS_PAGE_SIZE: u32 = 4096;
pub const KS_MIN_SLOT: u32 = 8192;

const _: () = assert!(KS_MAX_COLS * size_of::<KsCell>() as u32 == KS_PAGE_SIZE);
const _: () = assert!(
    HEAD + size_of::<KsBlit>() as u32 + KS_MAX_COLS * size_of::<KsCell>() as u32 <= KS_MIN_SLOT
);
const _: () = assert!(KS_MIN_SLOT <= KS_MAX_FRAME);

pub const KS_STYLE_KEEP: u32 = 0xffff_ffff;

pub const fn ks_style_pack(fg: u8, bg: u8, attrs: u8) -> u32 {
    (fg as u32) | ((bg as u32) << 8) | ((attrs as u32) << 16)
}

pub const fn ks_style_fg(style: u32) -> u8 {
    (style & 0xff) as u8
}

pub const fn ks_style_bg(style: u32) -> u8 {
    ((style >> 8) & 0xff) as u8
}

pub const fn ks_style_attrs(style: u32) -> u8 {
    ((style >> 16) & 0xff) as u8
}

// ---------------------------------------------------------------------------
// The keys
// ---------------------------------------------------------------------------

pub const KS_MOD_SHIFT: u32 = 1;
pub const KS_MOD_CTRL: u32 = 2;
pub const KS_MOD_ALT: u32 = 4;
pub const KS_MOD_META: u32 = 8;
pub const KS_MODS_ALL: u32 = KS_MOD_SHIFT | KS_MOD_CTRL | KS_MOD_ALT | KS_MOD_META;

/// Named keys sit above the Unicode range, so a codepoint and a name can never
/// collide.
pub const KS_KEY_NAMED: u32 = 0x11_0000;
pub const KS_KEY_ENTER: u32 = KS_KEY_NAMED;
pub const KS_KEY_BACKSPACE: u32 = KS_KEY_NAMED + 1;
pub const KS_KEY_TAB: u32 = KS_KEY_NAMED + 2;
pub const KS_KEY_ESCAPE: u32 = KS_KEY_NAMED + 3;
pub const KS_KEY_DELETE: u32 = KS_KEY_NAMED + 4;
pub const KS_KEY_INSERT: u32 = KS_KEY_NAMED + 5;
pub const KS_KEY_UP: u32 = KS_KEY_NAMED + 6;
pub const KS_KEY_DOWN: u32 = KS_KEY_NAMED + 7;
pub const KS_KEY_LEFT: u32 = KS_KEY_NAMED + 8;
pub const KS_KEY_RIGHT: u32 = KS_KEY_NAMED + 9;
pub const KS_KEY_HOME: u32 = KS_KEY_NAMED + 10;
pub const KS_KEY_END: u32 = KS_KEY_NAMED + 11;
pub const KS_KEY_PAGE_UP: u32 = KS_KEY_NAMED + 12;
pub const KS_KEY_PAGE_DOWN: u32 = KS_KEY_NAMED + 13;
pub const KS_KEY_F1: u32 = KS_KEY_NAMED + 14;
pub const KS_KEY_F2: u32 = KS_KEY_NAMED + 15;
pub const KS_KEY_F3: u32 = KS_KEY_NAMED + 16;
pub const KS_KEY_F4: u32 = KS_KEY_NAMED + 17;
pub const KS_KEY_F5: u32 = KS_KEY_NAMED + 18;
pub const KS_KEY_F6: u32 = KS_KEY_NAMED + 19;
pub const KS_KEY_F7: u32 = KS_KEY_NAMED + 20;
pub const KS_KEY_F8: u32 = KS_KEY_NAMED + 21;
pub const KS_KEY_F9: u32 = KS_KEY_NAMED + 22;
pub const KS_KEY_F10: u32 = KS_KEY_NAMED + 23;
pub const KS_KEY_F11: u32 = KS_KEY_NAMED + 24;
pub const KS_KEY_F12: u32 = KS_KEY_NAMED + 25;
pub const KS_KEY_MAX: u32 = KS_KEY_F12 + 1;

#[cfg(test)]
mod tests {
    use super::*;

    /// T33's three shape assertions, named out loud. They are `const _`
    /// above, so a breakage is a compile error, not a failure here.
    #[test]
    fn the_three_shape_assertions_hold() {
        assert_eq!(size_of::<KsCell>(), 8, "the renderer strides by 8 bytes");
        assert_eq!((size_of::<KsHead>() + size_of::<KsBlit>()) % 8, 0);
        assert_eq!(KS_MAX_COLS * size_of::<KsCell>() as u32, KS_PAGE_SIZE);
    }

    /// An op outside the table has neither length, which is what makes
    /// "unknown op" one answer rather than two.
    #[test]
    fn every_op_has_a_request_and_a_reply_length() {
        for op in 1..KS_OP_MAX {
            assert_ne!(ks_req_len(op), KS_LEN_NONE, "op {op} has no request length");
            assert_ne!(ks_rep_len(op), KS_LEN_NONE, "op {op} has no reply length");
            if ks_req_len(op) != KS_LEN_VARIABLE {
                assert_eq!(ks_req_len(op) % KS_ALIGN, 0, "op {op} request");
            }
            assert_eq!(ks_rep_len(op) % KS_ALIGN, 0, "op {op} reply");
            assert!(ks_rep_len(op) <= KS_MAX_FRAME);
        }
        for op in [0, KS_OP_MAX, KS_OP_MAX + 1, 0xffff] {
            assert_eq!(ks_req_len(op), KS_LEN_NONE);
            assert_eq!(ks_rep_len(op), KS_LEN_NONE);
            assert_eq!(ks_flags_all(op), 0);
        }
    }

    /// `-EINTR` on a parked key read is the only negative `res` carrying a
    /// payload, and a client may assert that.
    #[test]
    fn only_a_parked_key_reads_eintr_carries_a_payload() {
        let head = size_of::<KsHead>() as u32;
        assert_eq!(ks_err_len(KS_OP_KEY_READ, -KS_EINTR), head + 16);
        assert_eq!(ks_err_len(KS_OP_KEY_READ, -KS_EINVAL), head);
        for op in 0..=KS_OP_MAX {
            if op != KS_OP_KEY_READ {
                assert_eq!(ks_err_len(op, -KS_EINTR), head, "op {op}");
            }
            assert_eq!(ks_err_len(op, -KS_EPROTO), head, "op {op}");
        }
    }

    #[test]
    fn a_style_word_round_trips_through_its_accessors() {
        for (fg, bg, attrs) in [(0u8, 0u8, 0u8), (1, 0, 1), (15, 4, 7), (255, 255, 255)] {
            let s = ks_style_pack(fg, bg, attrs);
            assert_eq!(
                (ks_style_fg(s), ks_style_bg(s), ks_style_attrs(s)),
                (fg, bg, attrs)
            );
            assert_ne!(s, KS_STYLE_KEEP, "a packed style is never the sentinel");
        }
    }

    /// Per op on purpose: a bit accepted everywhere is a bit a client can set
    /// on an op that never reads it.
    #[test]
    fn only_the_ops_that_read_a_flag_accept_one() {
        assert_eq!(ks_flags_all(KS_OP_KEY_CLAIM), KS_F_TAKE);
        assert_eq!(ks_flags_all(KS_OP_SCREEN_CLAIM), KS_F_TAKE);
        assert_eq!(ks_flags_all(KS_OP_CURSOR), KS_F_SET);
        assert_eq!(
            ks_flags_all(KS_OP_ECHO),
            KS_F_ECHO_SHOW | KS_F_ECHO_FRESH | KS_F_ECHO_END
        );
        for op in [
            KS_OP_HELLO,
            KS_OP_KEY_READ,
            KS_OP_BLIT,
            KS_OP_STYLE,
            KS_OP_SCREEN_CLEAR,
            KS_OP_TTY,
            KS_OP_TERM_OPEN,
        ] {
            assert_eq!(ks_flags_all(op), 0, "op {op} takes no flags");
        }
    }
}
