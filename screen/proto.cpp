// SPDX-License-Identifier: MIT

#include "proto.h"

#include "ansi.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <new>
#include <vector>

using std::max;
using std::min;

// ---------------------------------------------------------------------------
// The state
// ---------------------------------------------------------------------------

// The alternate screen, saved while a client holds it. Braam's ~FullScreen
// restores the scrolling screen when the process record goes; here it is a
// member, so the same thing happens when the connection does — on EOF, on a
// kill, on a panic, because the kernel closes the socket either way.
struct Saved {
    bool held = false;
    std::vector<Cell> cells;
    u32 cols = 0, rows = 0;
    u32 cursor_x = 0, cursor_y = 0, cursor_on = 0;
    u8 fg = 0, bg = 0, attrs = 0;
};

struct Server {
    Term *t        = nullptr;
    Conn *keys     = nullptr; // who holds the raw keys
    Conn *screen   = nullptr; // who holds the alternate screen
    std::vector<Conn *> conns;
};

struct Conn {
    Server *s = nullptr;

    std::vector<uint8_t> in;
    std::vector<uint8_t> out;
    size_t sent = 0; // bytes of `out` already written

    bool hello  = false;
    bool closed = false;

    bool parked   = false; // a KEY_READ waiting for a key
    uint32_t park_seq = 0;

    Saved saved; // the screen this connection took, if it holds it
    ConnStats stats;
};

namespace {

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

void put(Conn &c, const void *p, size_t n)
{
    const uint8_t *b = static_cast<const uint8_t *>(p);
    c.out.insert(c.out.end(), b, b + n);
}

// One reply: the request's op and seq, a result, and a payload of `n` bytes.
// Everything that answers goes through here, so the count and the length rule
// are in one place.
void reply(Conn &c, uint32_t op, uint32_t seq, int32_t res, uint16_t flags, const void *payload,
           size_t n)
{
    ks_head h{};
    h.len   = uint32_t(sizeof(ks_head) + n);
    h.op    = uint16_t(op);
    h.flags = flags;
    h.seq   = seq;
    h.res   = res;
    put(c, &h, sizeof(h));
    if (n)
        put(c, payload, n);
    c.stats.replies++;
}

void reply_err(Conn &c, uint32_t op, uint32_t seq, int32_t err)
{
    reply(c, op, seq, -err, 0, nullptr, 0);
    c.stats.bad++;
}

// The one frame that ends a connection. It is a reply like any other, so the
// count still holds, and nothing is read after it.
void protocol_error(Conn &c, uint32_t op, uint32_t seq)
{
    reply(c, op, seq, -KS_EPROTO, 0, nullptr, 0);
    c.stats.bad++;
    c.closed = true;
}

ks_geom geom_of(const Term &t)
{
    ks_geom g{};
    g.cols = screen(t).cols;
    g.rows = screen(t).rows;
    return g;
}

// ---------------------------------------------------------------------------
// The claims
// ---------------------------------------------------------------------------

void screen_save(Conn &c)
{
    Term &t         = *c.s->t;
    const Screen &g = screen(t);
    c.saved.held    = true;
    c.saved.cols    = g.cols;
    c.saved.rows    = g.rows;
    c.saved.cursor_x = g.cursor_x;
    c.saved.cursor_y = g.cursor_y;
    c.saved.cursor_on = g.cursor_on;
    screen_style_get(t, c.saved.fg, c.saved.bg, c.saved.attrs);

    const Cell *cells = screen_cells(t);
    c.saved.cells.assign(cells, cells + size_t(g.cols) * g.rows);

    // The alternate screen starts blank, with the parser's sticky state as a
    // fresh terminal has it.
    screen_ansi_reset(t);
    screen_clear(t);
}

void screen_restore(Conn &c)
{
    if (!c.saved.held)
        return;
    Term &t = *c.s->t;
    c.saved.held = false;

    // The grid may have been resized under the claim. Restore what still fits,
    // which is what a terminal emulator does when the alternate screen goes.
    screen_ansi_reset(t);
    screen_clear(t);
    const Screen &g = screen(t);
    u32 cols = min(g.cols, c.saved.cols), rows = min(g.rows, c.saved.rows);
    Cell *cells = screen_cells(t);
    for (u32 y = 0; y < rows; y++)
        memcpy(cells + size_t(y) * g.cols, &c.saved.cells[size_t(y) * c.saved.cols],
               size_t(cols) * sizeof(Cell));
    screen_touch(t, 0, 0, g.cols, g.rows);
    screen_style(t, c.saved.fg, c.saved.bg, c.saved.attrs);
    screen_move(t, c.saved.cursor_x, c.saved.cursor_y);
    screen_cursor(t, c.saved.cursor_on != 0);
    c.saved.cells.clear();
}

// ---------------------------------------------------------------------------
// The ops
// ---------------------------------------------------------------------------

// Every op but HELLO and TTY needs the screen claim or the key claim; which one
// is a property of the op, so it is answered in one place.
bool needs_screen(uint32_t op)
{
    return op == KS_OP_BLIT || op == KS_OP_SCREEN_CLEAR;
}

bool needs_keys(uint32_t op)
{
    return op == KS_OP_KEY_READ;
}

void do_key_claim(Conn &c, const ks_head &h)
{
    Server &s  = *c.s;
    bool take  = (h.flags & KS_F_TAKE) != 0;
    if (take) {
        if (s.keys && s.keys != &c) {
            reply_err(c, h.op, h.seq, KS_EPERM);
            return;
        }
        s.keys = &c;
    } else if (s.keys == &c) {
        s.keys = nullptr;
    }
    ks_geom g = geom_of(*s.t);
    reply(c, h.op, h.seq, 0, 0, &g, sizeof(g));
}

void do_screen_claim(Conn &c, const ks_head &h)
{
    Server &s = *c.s;
    bool take = (h.flags & KS_F_TAKE) != 0;
    if (take) {
        if (s.screen && s.screen != &c) {
            reply_err(c, h.op, h.seq, KS_EPERM);
            return;
        }
        if (s.screen != &c) {
            s.screen = &c;
            screen_save(c);
        }
    } else if (s.screen == &c) {
        s.screen = nullptr;
        screen_restore(c);
    }
    ks_geom g = geom_of(*s.t);
    reply(c, h.op, h.seq, 0, 0, &g, sizeof(g));
}

void do_key_read(Conn &c, const ks_head &h)
{
    if (c.parked) {
        // One parked read per connection: a second would make the reply order
        // ambiguous, and a client that wants two keys wants them in order.
        reply_err(c, h.op, h.seq, KS_EBUSY);
        return;
    }
    c.parked   = true;
    c.park_seq = h.seq;
}

void do_blit(Conn &c, const ks_head &h, const uint8_t *body, size_t n)
{
    Term &t = *c.s->t;
    if (n < sizeof(ks_blit)) {
        reply_err(c, h.op, h.seq, KS_EINVAL);
        return;
    }
    ks_blit b{};
    memcpy(&b, body, sizeof(b));
    if (b.rsvd0 != 0) {
        reply_err(c, h.op, h.seq, KS_EINVAL);
        return;
    }

    // The cells must be exactly as many as the rectangle says, whatever the
    // geometry turns out to be: a frame whose length disagrees with its own
    // header is malformed content, not a stale frame.
    size_t want = size_t(b.w) * b.h * sizeof(ks_cell);
    if (want / sizeof(ks_cell) != size_t(b.w) * b.h || n != sizeof(ks_blit) + want) {
        reply_err(c, h.op, h.seq, KS_EINVAL);
        return;
    }

    const Screen &g = screen(t);
    ks_geom now     = geom_of(t);
    if (b.cols != g.cols || b.rows != g.rows) {
        // The client packed this against a geometry that has since moved. It
        // is not wrong, it is late: draw nothing and say so, so the client
        // repaints rather than treating a resize as an error.
        reply(c, h.op, h.seq, 0, KS_F_STALE, &now, sizeof(now));
        return;
    }
    if (b.x + b.w > g.cols || b.y + b.h > g.rows || b.x + b.w < b.x || b.y + b.h < b.y) {
        // Matching geometry and an impossible rectangle is a real bug.
        reply_err(c, h.op, h.seq, KS_EINVAL);
        return;
    }

    const ks_cell *src = reinterpret_cast<const ks_cell *>(body + sizeof(ks_blit));
    Cell *cells        = screen_cells(t);
    for (u32 y = 0; y < b.h; y++)
        memcpy(cells + size_t(b.y + y) * g.cols + b.x, src + size_t(y) * b.w,
               size_t(b.w) * sizeof(Cell));

    // screen_touch is what makes the cells drawable — it runs rune_safe over
    // them — and what tells the renderer they changed. Nothing else does.
    screen_touch(t, b.x, b.y, b.w, b.h);
    screen_move(t, b.cursor_x, b.cursor_y);
    screen_cursor(t, b.cursor_on != 0);
    reply(c, h.op, h.seq, 0, 0, &now, sizeof(now));
}

void do_cursor(Conn &c, const ks_head &h, const uint8_t *body, size_t n)
{
    Term &t = *c.s->t;
    if (n != sizeof(ks_cursor_req)) {
        reply_err(c, h.op, h.seq, KS_EINVAL);
        return;
    }
    ks_cursor_req q{};
    memcpy(&q, body, sizeof(q));
    if (q.rsvd0 != 0) {
        reply_err(c, h.op, h.seq, KS_EINVAL);
        return;
    }
    if (h.flags & KS_F_SET) {
        // A set is refused while somebody holds the alternate screen: the
        // scrolling screen is not on show, so moving its cursor is a surprise.
        if (c.s->screen && c.s->screen != &c) {
            reply_err(c, h.op, h.seq, KS_EPERM);
            return;
        }
        screen_move(t, q.x, q.y);
        screen_cursor(t, q.on != 0);
    }
    ks_cursor r{};
    r.x    = screen(t).cursor_x;
    r.y    = screen(t).cursor_y;
    r.on   = screen_cursor_on(t) ? 1 : 0;
    r.cols = screen(t).cols;
    r.rows = screen(t).rows;
    reply(c, h.op, h.seq, 0, 0, &r, sizeof(r));
}

// Braam's Sys::Echo, whose whole point is that a line editor's repaint is one
// call: the anchor, a run per colour, the bytes, and where to leave the cursor.
void do_echo(Conn &c, const ks_head &h, const uint8_t *body, size_t n)
{
    Term &t = *c.s->t;
    if (n < sizeof(ks_echo) || !screen(t).cols) {
        reply_err(c, h.op, h.seq, KS_EINVAL);
        return;
    }
    ks_echo e{};
    memcpy(&e, body, sizeof(e));

    // The whole shape before a cell moves, so a malformed payload paints
    // nothing. A u64 sum: eight u32 lengths overflow one.
    if (e.runs > KS_ECHO_RUNS_MAX) {
        reply_err(c, h.op, h.seq, KS_EINVAL);
        return;
    }
    size_t table = size_t(e.runs) * sizeof(ks_run);
    if (n < sizeof(ks_echo) + table) {
        reply_err(c, h.op, h.seq, KS_EINVAL);
        return;
    }
    std::vector<ks_run> runs(e.runs);
    uint64_t want = uint64_t(sizeof(ks_echo)) + table;
    for (u32 i = 0; i < e.runs; i++) {
        memcpy(&runs[i], body + sizeof(ks_echo) + i * sizeof(ks_run), sizeof(ks_run));
        want += runs[i].len;
    }
    // The frame is padded to KS_ALIGN, so the bytes end where the runs say and
    // the padding after them must be zero.
    if (want > n || n - want >= KS_ALIGN) {
        reply_err(c, h.op, h.seq, KS_EINVAL);
        return;
    }
    for (size_t i = want; i < n; i++)
        if (body[i] != 0) {
            reply_err(c, h.op, h.seq, KS_EINVAL);
            return;
        }

    // Dark for the write, whatever it is left as: nothing between here and the
    // placement below is ever presented.
    screen_cursor(t, false);
    uint64_t was = screen_scrolled(t);

    if (h.flags & KS_F_ECHO_FRESH) {
        // The anchor is wherever the cursor is, on a row of its own, and the
        // newline goes ahead of any run's style so a scroll blanks the new row
        // in the default colour rather than the prompt's.
        if (screen(t).cursor_x != 0)
            screen_write(t, "\n");
    } else {
        screen_move(t, e.x, e.y);
    }

    size_t at = sizeof(ks_echo) + table;
    for (u32 i = 0; i < e.runs; i++) {
        // A run naming no colour paints in the sticky one; one with no bytes
        // only sets the colour.
        if (runs[i].style != KS_STYLE_KEEP)
            screen_style(t, ks_style_fg(runs[i].style), ks_style_bg(runs[i].style),
                         ks_style_attrs(runs[i].style));
        if (runs[i].len)
            screen_write(t, Str(reinterpret_cast<const char *>(body + at), runs[i].len));
        at += runs[i].len;
    }

    u32 scrolled = u32(screen_scrolled(t) - was);
    if (h.flags & KS_F_ECHO_END) {
        // The deferred wrap column is not one screen_move can name, so a write
        // that filled a row is carried to the next. That can scroll, so the
        // count is taken again after it.
        if (screen(t).cursor_x >= screen(t).cols) {
            screen_write(t, "\n");
            scrolled = u32(screen_scrolled(t) - was);
        }
    } else {
        // Where the caller wants the cursor left, measured from the anchor and
        // carried up by whatever the write scrolled under it.
        u32 off = e.x + e.cur;
        u32 row = e.y + off / screen(t).cols;
        screen_move(t, off % screen(t).cols, row >= scrolled ? row - scrolled : 0);
    }
    screen_cursor(t, (h.flags & KS_F_ECHO_SHOW) != 0);

    ks_cursor r{};
    r.x        = screen(t).cursor_x;
    r.y        = screen(t).cursor_y;
    r.on       = screen_cursor_on(t) ? 1 : 0;
    r.cols     = screen(t).cols;
    r.rows     = screen(t).rows;
    r.scrolled = scrolled;
    reply(c, h.op, h.seq, 0, 0, &r, sizeof(r));
}

void do_style(Conn &c, const ks_head &h, const uint8_t *body, size_t n)
{
    if (n != sizeof(ks_style)) {
        reply_err(c, h.op, h.seq, KS_EINVAL);
        return;
    }
    ks_style st{};
    memcpy(&st, body, sizeof(st));
    if (st.rsvd0 != 0 || st.style == KS_STYLE_KEEP) {
        reply_err(c, h.op, h.seq, KS_EINVAL);
        return;
    }
    screen_style(*c.s->t, ks_style_fg(st.style), ks_style_bg(st.style),
                 ks_style_attrs(st.style));
    reply(c, h.op, h.seq, 0, 0, nullptr, 0);
}

void do_hello(Conn &c, const ks_head &h, const uint8_t *body, size_t n)
{
    if (n != sizeof(ks_hello)) {
        reply_err(c, h.op, h.seq, KS_EINVAL);
        return;
    }
    ks_hello q{};
    memcpy(&q, body, sizeof(q));
    if (q.magic != KS_MAGIC || q.version != KS_ABI_VERSION) {
        // Not content: a peer that does not know this protocol cannot be
        // talked to at all, so this ends the connection.
        protocol_error(c, h.op, h.seq);
        return;
    }
    c.hello = true;

    ks_hello_rep r{};
    r.magic     = KS_MAGIC;
    r.version   = KS_ABI_VERSION;
    r.cols      = screen(*c.s->t).cols;
    r.rows      = screen(*c.s->t).rows;
    r.max_cols  = KS_MAX_COLS;
    r.max_rows  = KS_MAX_ROWS;
    r.max_frame = KS_MAX_FRAME;
    r.features  = 0;
    reply(c, h.op, h.seq, 0, 0, &r, sizeof(r));
}

void do_tty(Conn &c, const ks_head &h)
{
    ks_tty r{};
    r.flags = KS_TTY_CONSOLE;
    r.cols  = screen(*c.s->t).cols;
    r.rows  = screen(*c.s->t).rows;
    reply(c, h.op, h.seq, 0, 0, &r, sizeof(r));
}

// One whole frame. Everything above it has decided that `len` is sane and the
// bytes are all here.
void dispatch(Conn &c, const ks_head &h, const uint8_t *body, size_t n)
{
    c.stats.frames++;

    // The handshake is first, exactly once, and nothing works before it: a
    // client and a daemon of different versions must not paint anything.
    if (h.op == KS_OP_HELLO) {
        if (c.hello)
            protocol_error(c, h.op, h.seq);
        else
            do_hello(c, h, body, n);
        return;
    }
    if (!c.hello) {
        protocol_error(c, h.op, h.seq);
        return;
    }

    if (h.op >= KS_OP_MAX || h.op == KS_OP_TERM_OPEN) {
        // An op this daemon does not know may be one a later daemon does, so
        // it is ENOSYS and never EINVAL.
        reply_err(c, h.op, h.seq, KS_ENOSYS);
        return;
    }
    if (h.flags & ~ks_flags_all(h.op)) {
        reply_err(c, h.op, h.seq, KS_EINVAL);
        return;
    }
    uint32_t fixed = ks_req_len(h.op);
    if (fixed != KS_LEN_VARIABLE && h.len != fixed) {
        reply_err(c, h.op, h.seq, KS_EINVAL);
        return;
    }
    if ((needs_screen(h.op) && c.s->screen != &c) || (needs_keys(h.op) && c.s->keys != &c)) {
        reply_err(c, h.op, h.seq, KS_ENOTTY);
        return;
    }

    switch (h.op) {
    case KS_OP_KEY_CLAIM:
        do_key_claim(c, h);
        break;
    case KS_OP_KEY_READ:
        do_key_read(c, h);
        break;
    case KS_OP_SCREEN_CLAIM:
        do_screen_claim(c, h);
        break;
    case KS_OP_BLIT:
        do_blit(c, h, body, n);
        break;
    case KS_OP_CURSOR:
        do_cursor(c, h, body, n);
        break;
    case KS_OP_ECHO:
        do_echo(c, h, body, n);
        break;
    case KS_OP_STYLE:
        do_style(c, h, body, n);
        break;
    case KS_OP_SCREEN_CLEAR:
        screen_clear(*c.s->t);
        reply(c, h.op, h.seq, 0, 0, nullptr, 0);
        break;
    case KS_OP_TTY:
        do_tty(c, h);
        break;
    default:
        reply_err(c, h.op, h.seq, KS_ENOSYS);
        break;
    }
}

// A key into a parked read. The geometry rides on it, so a resize needs no
// event of its own.
void answer_key(Conn &c, uint32_t code, uint32_t mods, int32_t res)
{
    ks_key k{};
    k.code = code;
    k.mods = mods;
    k.cols = screen(*c.s->t).cols;
    k.rows = screen(*c.s->t).rows;
    uint32_t seq = c.park_seq;
    c.parked     = false;
    c.park_seq   = 0;
    reply(c, KS_OP_KEY_READ, seq, res, 0, &k, sizeof(k));
    if (res != 0)
        c.stats.bad++;
}

} // namespace

// ---------------------------------------------------------------------------
// The surface
// ---------------------------------------------------------------------------

Server *server_new(Term *t)
{
    Server *s = new (std::nothrow) Server();
    if (s)
        s->t = t;
    return s;
}

void server_free(Server *s)
{
    if (!s)
        return;
    while (!s->conns.empty())
        conn_free(s->conns.back());
    delete s;
}

Term &server_term(Server &s)
{
    return *s.t;
}

Conn *conn_new(Server &s)
{
    Conn *c = new (std::nothrow) Conn();
    if (!c)
        return nullptr;
    c->s = &s;
    s.conns.push_back(c);
    return c;
}

void conn_free(Conn *c)
{
    if (!c)
        return;
    Server &s = *c->s;
    if (s.keys == c)
        s.keys = nullptr;
    if (s.screen == c) {
        s.screen = nullptr;
        screen_restore(*c);
    }
    s.conns.erase(std::remove(s.conns.begin(), s.conns.end(), c), s.conns.end());
    delete c;
}

void conn_eof(Conn &c)
{
    c.closed = true;
    // A parked read is owed nothing: the client is gone, and there is no
    // socket left to write to.
    c.parked = false;
}

bool feed(Conn &c, const uint8_t *data, size_t n)
{
    if (c.closed)
        return false;
    c.in.insert(c.in.end(), data, data + n);

    for (;;) {
        if (c.in.size() < sizeof(ks_head))
            break;
        ks_head h{};
        memcpy(&h, c.in.data(), sizeof(h));

        // Frame-level: a length that cannot be trusted leaves no way to find
        // the next boundary, and a seq of 0 or a non-zero res says the peer is
        // not speaking this protocol. One -EPROTO, and the connection ends.
        if (h.len < sizeof(ks_head) || h.len > KS_MAX_FRAME || h.len % KS_ALIGN != 0 ||
            h.seq == KS_SEQ_UNSOLICITED || h.res != 0) {
            // It is answered, so it counts: the invariant is over frames that
            // get a reply, and a malformed one gets exactly one like the rest.
            c.stats.frames++;
            protocol_error(c, h.op, h.seq);
            return false;
        }
        if (c.in.size() < h.len)
            break; // the rest of it is in the next read

        dispatch(c, h, c.in.data() + sizeof(ks_head), h.len - sizeof(ks_head));
        c.in.erase(c.in.begin(), c.in.begin() + h.len);
        if (c.closed)
            return false;
    }
    return true;
}

const uint8_t *conn_out(const Conn &c, size_t &n)
{
    n = c.out.size() - c.sent;
    return c.out.data() + c.sent;
}

void conn_drain(Conn &c, size_t n)
{
    c.sent = min(c.sent + n, c.out.size());
    if (c.sent == c.out.size()) {
        c.out.clear();
        c.sent = 0;
    }
}

bool conn_closed(const Conn &c)
{
    return c.closed;
}

const ConnStats &conn_stats(const Conn &c)
{
    return c.stats;
}

bool conn_has_keys(const Conn &c)
{
    return c.s->keys == &c;
}

bool conn_has_screen(const Conn &c)
{
    return c.s->screen == &c;
}

bool conn_parked(const Conn &c)
{
    return c.parked;
}

void server_key(Server &s, uint32_t code, uint32_t mods)
{
    // Only the holder of the keys reads them, and only a parked read takes
    // one: a keystroke nobody is waiting for is dropped rather than queued for
    // ever. A client that wants every key keeps a read parked.
    if (s.keys && s.keys->parked)
        answer_key(*s.keys, code, mods, 0);
}

void server_resize(Server &s, uint32_t cols, uint32_t rows)
{
    if (!screen_resize(*s.t, cols, rows))
        return;
    // A parked read is answered -EINTR *with* a payload: the daemon owns the
    // reply and cannot invent a key, and a client that asked for one has to
    // learn the new shape. This is the only negative res that carries one.
    for (Conn *c : s.conns)
        if (c->parked)
            answer_key(*c, 0, 0, -KS_EINTR);
}
