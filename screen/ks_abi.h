// SPDX-License-Identifier: MIT
//
// The koru-screen protocol. Canonical: the daemon is the server and the server
// owns the protocol, so this file is the original and rust/runtime/src/ks_abi.rs
// is the mirror. scripts/abi.sh diffs the two through the ks_dump binaries.
//
// A client connects to the daemon's Unix socket and ADOPT_FDs it into its ring.
// There are two connections: this framed protocol on one, and a raw ANSI byte
// stream on the other, which is what stdout is.
//
// koru's discipline throughout: nothing has padding, reserved fields must be
// zero, unknown flag bits are rejected, and every rejection has an exact errno.
//
// `seq` is the client's and is echoed. It is NOT koru's `user_data`: a cookie
// names the pump's READ, which can complete carrying three replies or half of
// one, while `seq` names a request. Conflating them is the first mistake a
// second implementer will make.
//
// Valid C11 and C++20 both. Every constant is a #define, never a static const:
// an unused static const is -Wunused-const-variable in C, which is -Werror.

#ifndef KS_ABI_H
#define KS_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define KS_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#define KS_ALIGNOF(type)            alignof(type)
#else
#define KS_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#define KS_ALIGNOF(type)            _Alignof(type)
#endif

// Spells "kscr" little-endian, as KORU_MAGIC spells "koru".
#define KS_MAGIC 0x7263736bu

// Bumped on any incompatible change to this file. A client whose version the
// daemon does not know is refused at KS_OP_HELLO, before anything is claimed.
#define KS_ABI_VERSION 1u

// The socket's basename, under $XDG_RUNTIME_DIR unless $KORU_SCREEN_SOCK names
// a path. The daemon binds a temporary name beside it and renames into place,
// so a half-initialised socket is never connectable.
#define KS_SOCK_NAME "koru-screen.sock"

// ---------------------------------------------------------------- the frame
//
// Every frame is this header and then that op's payload. `len` is the whole
// frame and must be a multiple of KS_ALIGN, which keeps a blit's cells
// 8-aligned so the daemon reads them in place.

#define KS_ALIGN 8u

struct ks_head {
    uint32_t len;   // the whole frame, a multiple of KS_ALIGN
    uint16_t op;    // KS_OP_*
    uint16_t flags; // this op's mask; a bit outside it is EINVAL
    uint32_t seq;   // client-chosen, echoed; 0 is reserved
    int32_t res;    // request: must be 0. reply: 0, or -errno
};

KS_STATIC_ASSERT(sizeof(struct ks_head) == 16, "the header is 16 bytes");
KS_STATIC_ASSERT(sizeof(struct ks_head) % KS_ALIGN == 0, "the header is 8-aligned");

// Reserved for unsolicited frames, which is what keeps multiplexing additive.
// A request carrying it is EINVAL.
#define KS_SEQ_UNSOLICITED 0u

// The largest frame either side will read or write. A client bands a blit into
// frames no larger than this, and the daemon closes on anything above it.
#define KS_MAX_FRAME 65536u

// --------------------------------------------------------------- the errnos
//
// The daemon has no koru dependency, so the numbers are spelled here rather
// than included from koru_errno.h. asm-generic values, which is what the errno
// table in rust/sys/src/error.rs uses.
//
// Which one goes with which rejection, and it is exact:
//
//   -EPROTO   the frame does not parse — a bad `len`, a non-zero `res`, a
//             `seq` of 0, or a HELLO that is not first. The stream has
//             desynchronised, so this one closes the connection.
//   -EINVAL   the frame parses and its content is wrong: an unknown flag bit,
//             a non-zero reserved field, a rectangle outside the geometry the
//             frame itself declares, a run count above KS_ECHO_RUNS_MAX.
//   -ENOSYS   an op at or above KS_OP_MAX, and KS_OP_TERM_OPEN until it exists.
//   -EPERM    a claim somebody else holds.
//   -EBUSY    a second KEY_READ parked on one connection.
//   -EINTR    a parked KEY_READ answered by a resize rather than a key.
//   -ENOTTY   an op that needs a claim this connection does not hold.

#define KS_EINTR  4
#define KS_EBUSY  16
#define KS_EINVAL 22
#define KS_ENOTTY 25
#define KS_EPROTO 71
#define KS_ENOSYS 38
#define KS_EPERM  1

// ------------------------------------------------------------------ the ops
//
// One op per call of Braam's terminal surface. The reply's `op` and `seq` are
// the request's, so a client matches on `seq` alone.

#define KS_OP_HELLO        1u  // the handshake; must be first, exactly once
#define KS_OP_KEY_CLAIM    2u  // take the raw keys, or give them back
#define KS_OP_KEY_READ     3u  // the next key; parks until one arrives
#define KS_OP_SCREEN_CLAIM 4u  // take the alternate screen, or give it back
#define KS_OP_BLIT         5u  // the cells that changed, and the cursor
#define KS_OP_CURSOR       6u  // the scrolling screen's cursor, get or set
#define KS_OP_ECHO         7u  // a line editor's whole repaint, in one frame
#define KS_OP_STYLE        8u  // the colours the next byte-channel write paints
#define KS_OP_SCREEN_CLEAR 9u  // blank the scrolling screen, home its cursor
#define KS_OP_TTY          10u // is this a terminal, and how big
#define KS_OP_TERM_OPEN    11u // reserved for multiplexing; answers -ENOSYS

// One past the last. An op at or above it is ENOSYS, never EINVAL: an op this
// daemon does not know may be one a later daemon does.
#define KS_OP_MAX 12u

// ---------------------------------------------------------------- the flags
//
// Per op: the same bit means different things on different ops, and one table
// would let a client set a bit that op never reads.

#define KS_F_TAKE  1u // KEY_CLAIM, SCREEN_CLAIM: take it, else give it back
#define KS_F_SET   1u // CURSOR: set it, else only ask
#define KS_F_STALE 1u // BLIT reply: the geometry moved, so nothing was drawn

// HELLO: this connection is the byte channel. The handshake is the last frame
// on it; everything after is ANSI bytes, and the daemon answers nothing.
#define KS_F_BYTES 1u

// ECHO's three, Braam's SYS_ECHO_*.
#define KS_F_ECHO_SHOW  1u // leave the cursor on
#define KS_F_ECHO_FRESH 2u // start from a fresh line
#define KS_F_ECHO_END   4u // the last frame of this edit

// Every flag this op accepts on a *request*. KS_F_STALE is a reply's, so no
// request mask carries it.
static inline uint16_t ks_flags_all(uint32_t op)
{
    switch (op) {
    case KS_OP_HELLO:
        return (uint16_t)KS_F_BYTES;
    case KS_OP_KEY_CLAIM:
    case KS_OP_SCREEN_CLAIM:
        return (uint16_t)KS_F_TAKE;
    case KS_OP_CURSOR:
        return (uint16_t)KS_F_SET;
    case KS_OP_ECHO:
        return (uint16_t)(KS_F_ECHO_SHOW | KS_F_ECHO_FRESH | KS_F_ECHO_END);
    default:
        return 0;
    }
}

// ------------------------------------------------------------ the payloads
//
// Every one is a multiple of KS_ALIGN, so `len` is a multiple of it whatever
// the op. Nothing here has padding the compiler chose.

// KS_OP_HELLO's request. Nothing else may precede it on a connection.
struct ks_hello {
    uint32_t magic;   // KS_MAGIC
    uint32_t version; // KS_ABI_VERSION; anything else is -EPROTO
};

// Its reply: what the client may assume for the life of the connection.
struct ks_hello_rep {
    uint32_t magic;
    uint32_t version;
    uint32_t cols, rows;         // the geometry now
    uint32_t max_cols, max_rows; // the grid can never exceed these
    uint32_t max_frame;          // KS_MAX_FRAME, so a client bands to what it is told
    uint32_t features;           // 0 for now; a set bit is a capability
};

// The geometry, which every terminal reply carries so that a resize needs no
// event to subscribe to. KEY_CLAIM, SCREEN_CLAIM and BLIT reply with it.
struct ks_geom {
    uint32_t cols, rows;
};

// KS_OP_KEY_READ's reply. There are no control characters: ^C is 'c' with
// KS_MOD_CTRL, and the reader decides what that means.
struct ks_key {
    uint32_t code, mods;
    uint32_t cols, rows;
};

// KS_OP_BLIT's payload header, followed by w*h cells, row by row.
//
// `cols` and `rows` are the geometry the client believed when it packed this.
// A blit whose geometry matches but whose rectangle is out of range is a real
// bug and is -EINVAL; one whose geometry differs raced a resize, so the daemon
// draws nothing and replies 0 with KS_F_STALE.
struct ks_blit {
    uint32_t x, y, w, h;
    uint32_t cursor_x, cursor_y, cursor_on;
    uint32_t cols, rows;
    uint32_t rsvd0; // must be zero, and pads the cells to KS_ALIGN
};

// The cell, Braam's, byte for byte.
struct ks_cell {
    uint32_t ch;   // 0 is blank
    uint8_t fg, bg;// indices into the renderer's palette
    uint8_t attrs; // KS_ATTR_*
    uint8_t rsvd0; // pads to the stride the renderer assumes
};

KS_STATIC_ASSERT(sizeof(struct ks_cell) == 8, "the renderer strides by 8 bytes");

// Cells are 8-aligned inside the frame, which is the whole reason ks_blit
// carries a padding word. Removing it must fire this.
KS_STATIC_ASSERT((sizeof(struct ks_head) + sizeof(struct ks_blit)) % KS_ALIGN == 0,
                 "a blit's cells are 8-aligned");

// KS_OP_CURSOR's request, and KS_OP_STYLE's.
struct ks_cursor_req {
    uint32_t x, y;
    uint32_t on;
    uint32_t rsvd0; // must be zero
};

// KS_OP_CURSOR's reply, and KS_OP_ECHO's: where the cursor landed, the
// geometry, and how far the write scrolled the anchor row up — the one thing
// a caller could not work out for itself. `scrolled` is 0 from CURSOR.
struct ks_cursor {
    uint32_t x, y;
    uint32_t on;
    uint32_t cols, rows;
    uint32_t scrolled;
};

// KS_OP_ECHO's payload header, then `runs` ks_run headers, then their bytes end
// to end, zero-padded to KS_ALIGN. Every header precedes every byte, so the
// shape is checkable in one pass.
struct ks_echo {
    uint32_t x, y;
    uint32_t cur;
    uint32_t runs;
};

struct ks_run {
    uint32_t style; // ks_style_pack, or KS_STYLE_KEEP
    uint32_t len;   // bytes of this run
};

// KS_OP_STYLE's payload: the colours the next write to the byte channel paints
// with. Sticky, so a program that colours something puts the default back.
struct ks_style {
    uint32_t style; // ks_style_pack; KS_STYLE_KEEP is -EINVAL here
    uint32_t rsvd0; // must be zero
};

// KS_OP_TTY's reply. `flags` is what tells a terminal from a pipe: the grid is
// cells, so there is no escape sequence to ask with.
struct ks_tty {
    uint32_t flags; // KS_TTY_*
    uint32_t cols, rows;
    uint32_t rsvd0;
};

#define KS_TTY_CONSOLE 1u // this connection's byte channel is the cell grid

#define KS_ECHO_RUNS_MAX 8u

// ------------------------------------------------------------- frame sizes
//
// One place per op, so a validation that disagrees with an encoder is a diff
// rather than a hang.

#define KS_LEN_VARIABLE 0xffffffffu // BLIT and ECHO, whose length is their content
#define KS_LEN_NONE     0u          // no such op

static inline uint32_t ks_req_len(uint32_t op)
{
    switch (op) {
    case KS_OP_HELLO:
        return (uint32_t)(sizeof(struct ks_head) + sizeof(struct ks_hello));
    case KS_OP_KEY_CLAIM:
    case KS_OP_KEY_READ:
    case KS_OP_SCREEN_CLAIM:
    case KS_OP_SCREEN_CLEAR:
    case KS_OP_TTY:
    case KS_OP_TERM_OPEN:
        return (uint32_t)sizeof(struct ks_head);
    case KS_OP_BLIT:
    case KS_OP_ECHO:
        return KS_LEN_VARIABLE;
    case KS_OP_CURSOR:
        return (uint32_t)(sizeof(struct ks_head) + sizeof(struct ks_cursor_req));
    case KS_OP_STYLE:
        return (uint32_t)(sizeof(struct ks_head) + sizeof(struct ks_style));
    default:
        return KS_LEN_NONE;
    }
}

static inline uint32_t ks_rep_len(uint32_t op)
{
    switch (op) {
    case KS_OP_HELLO:
        return (uint32_t)(sizeof(struct ks_head) + sizeof(struct ks_hello_rep));
    case KS_OP_KEY_CLAIM:
    case KS_OP_SCREEN_CLAIM:
    case KS_OP_BLIT:
        return (uint32_t)(sizeof(struct ks_head) + sizeof(struct ks_geom));
    case KS_OP_KEY_READ:
        return (uint32_t)(sizeof(struct ks_head) + sizeof(struct ks_key));
    case KS_OP_CURSOR:
    case KS_OP_ECHO:
        return (uint32_t)(sizeof(struct ks_head) + sizeof(struct ks_cursor));
    case KS_OP_STYLE:
    case KS_OP_SCREEN_CLEAR:
    case KS_OP_TERM_OPEN:
        return (uint32_t)sizeof(struct ks_head);
    case KS_OP_TTY:
        return (uint32_t)(sizeof(struct ks_head) + sizeof(struct ks_tty));
    default:
        return KS_LEN_NONE;
    }
}

// A failed request replies with the header alone. The one exception is
// KS_OP_KEY_READ's -EINTR, which carries a whole ks_key so that a resize is
// answered rather than signalled: -EINTR is the only negative res with a
// payload, and a client may assert that.
static inline uint32_t ks_err_len(uint32_t op, int32_t res)
{
    if (op == KS_OP_KEY_READ && res == -KS_EINTR)
        return (uint32_t)(sizeof(struct ks_head) + sizeof(struct ks_key));
    return (uint32_t)sizeof(struct ks_head);
}

// ---------------------------------------------------------------- the cells
//
// The palette indices and attributes are Braam's screen.h, and they are wire
// format here because a cell carries them.

#define KS_ATTR_BOLD      1u
#define KS_ATTR_UNDERLINE 2u
#define KS_ATTR_REVERSE   4u
#define KS_ATTRS_ALL      (KS_ATTR_BOLD | KS_ATTR_UNDERLINE | KS_ATTR_REVERSE)

#define KS_COLOR_BLACK   0u
#define KS_COLOR_RED     1u
#define KS_COLOR_GREEN   2u
#define KS_COLOR_YELLOW  3u
#define KS_COLOR_BLUE    4u
#define KS_COLOR_MAGENTA 5u
#define KS_COLOR_CYAN    6u
#define KS_COLOR_WHITE   7u
#define KS_COLOR_BRIGHT  8u // added to any of the above
#define KS_COLORS        16u

// The grid is clamped to these, so a bad geometry cannot overflow a size
// computation or exhaust the heap. 1 MiB of cells at the maximum.
#define KS_MAX_COLS 512u
#define KS_MAX_ROWS 256u

// A maximum-width row of cells is exactly one page, so a client whose slot is
// two pages can always send a whole row and banding cannot get stuck.
#define KS_PAGE_SIZE 4096u
#define KS_MIN_SLOT  8192u

KS_STATIC_ASSERT(KS_MAX_COLS * sizeof(struct ks_cell) == KS_PAGE_SIZE,
                 "a maximum-width row of cells is one page");
KS_STATIC_ASSERT(sizeof(struct ks_head) + sizeof(struct ks_blit) +
                         KS_MAX_COLS * sizeof(struct ks_cell) <=
                     KS_MIN_SLOT,
                 "a one-row band fits the smallest slot a screen client may use");
KS_STATIC_ASSERT(KS_MIN_SLOT <= KS_MAX_FRAME, "a band a client can send, the daemon can read");

// The style word ks_run and ks_style carry: Braam's sys_style_pack.
#define KS_STYLE_KEEP 0xffffffffu

static inline uint32_t ks_style_pack(uint8_t fg, uint8_t bg, uint8_t attrs)
{
    return (uint32_t)fg | ((uint32_t)bg << 8) | ((uint32_t)attrs << 16);
}

static inline uint8_t ks_style_fg(uint32_t style)
{
    return (uint8_t)(style & 0xffu);
}

static inline uint8_t ks_style_bg(uint32_t style)
{
    return (uint8_t)((style >> 8) & 0xffu);
}

static inline uint8_t ks_style_attrs(uint32_t style)
{
    return (uint8_t)((style >> 16) & 0xffu);
}

// ----------------------------------------------------------------- the keys
//
// A key is {code, mods}. Printable keys carry their codepoint; named keys sit
// above the Unicode range, so the two can never collide.

#define KS_MOD_SHIFT 1u
#define KS_MOD_CTRL  2u
#define KS_MOD_ALT   4u
#define KS_MOD_META  8u
#define KS_MODS_ALL  (KS_MOD_SHIFT | KS_MOD_CTRL | KS_MOD_ALT | KS_MOD_META)

#define KS_KEY_NAMED     0x110000u
#define KS_KEY_ENTER     (KS_KEY_NAMED + 0u)
#define KS_KEY_BACKSPACE (KS_KEY_NAMED + 1u)
#define KS_KEY_TAB       (KS_KEY_NAMED + 2u)
#define KS_KEY_ESCAPE    (KS_KEY_NAMED + 3u)
#define KS_KEY_DELETE    (KS_KEY_NAMED + 4u)
#define KS_KEY_INSERT    (KS_KEY_NAMED + 5u)
#define KS_KEY_UP        (KS_KEY_NAMED + 6u)
#define KS_KEY_DOWN      (KS_KEY_NAMED + 7u)
#define KS_KEY_LEFT      (KS_KEY_NAMED + 8u)
#define KS_KEY_RIGHT     (KS_KEY_NAMED + 9u)
#define KS_KEY_HOME      (KS_KEY_NAMED + 10u)
#define KS_KEY_END       (KS_KEY_NAMED + 11u)
#define KS_KEY_PAGE_UP   (KS_KEY_NAMED + 12u)
#define KS_KEY_PAGE_DOWN (KS_KEY_NAMED + 13u)
#define KS_KEY_F1        (KS_KEY_NAMED + 14u)
#define KS_KEY_F2        (KS_KEY_NAMED + 15u)
#define KS_KEY_F3        (KS_KEY_NAMED + 16u)
#define KS_KEY_F4        (KS_KEY_NAMED + 17u)
#define KS_KEY_F5        (KS_KEY_NAMED + 18u)
#define KS_KEY_F6        (KS_KEY_NAMED + 19u)
#define KS_KEY_F7        (KS_KEY_NAMED + 20u)
#define KS_KEY_F8        (KS_KEY_NAMED + 21u)
#define KS_KEY_F9        (KS_KEY_NAMED + 22u)
#define KS_KEY_F10       (KS_KEY_NAMED + 23u)
#define KS_KEY_F11       (KS_KEY_NAMED + 24u)
#define KS_KEY_F12       (KS_KEY_NAMED + 25u)
#define KS_KEY_MAX       (KS_KEY_F12 + 1u)

#endif // KS_ABI_H
