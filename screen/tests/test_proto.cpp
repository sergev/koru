// SPDX-License-Identifier: MIT
//
// T36's done test: the rejection matrix, the framing and the claim lifetime,
// all driven through `feed()` with no socket anywhere. That is the point of a
// pure feed — every case here would otherwise need a client, a daemon and a
// race to reproduce.
//
// After every case: exactly one reply per accepted frame. C1, transposed.

#include "harness.h"
#include "proto.h"

#include <cstring>
#include <vector>

namespace {

Server *srv;
Conn *cn;

// ---------------------------------------------------------------- building

std::vector<uint8_t> frame(uint32_t op, uint32_t seq, uint16_t flags, const void *payload,
                           size_t n)
{
    std::vector<uint8_t> f(sizeof(ks_head) + n);
    ks_head h{};
    h.len   = uint32_t(f.size());
    h.op    = uint16_t(op);
    h.flags = flags;
    h.seq   = seq;
    h.res   = 0;
    memcpy(f.data(), &h, sizeof(h));
    if (n)
        memcpy(f.data() + sizeof(h), payload, n);
    return f;
}

std::vector<uint8_t> blit_frame(uint32_t seq, const ks_blit &b, size_t cells)
{
    std::vector<uint8_t> body(sizeof(ks_blit) + cells * sizeof(ks_cell));
    memcpy(body.data(), &b, sizeof(b));
    for (size_t i = 0; i < cells; i++) {
        ks_cell c{ 'x', KS_COLOR_WHITE, KS_COLOR_BLACK, 0, 0 };
        memcpy(body.data() + sizeof(ks_blit) + i * sizeof(ks_cell), &c, sizeof(c));
    }
    return frame(KS_OP_BLIT, seq, 0, body.data(), body.size());
}

// ----------------------------------------------------------------- reading

// The replies waiting, as whole frames. Drains what it read, so a case never
// sees the one before it.
std::vector<ks_head> replies(std::vector<std::vector<uint8_t>> *bodies = nullptr)
{
    size_t n         = 0;
    const uint8_t *p = conn_out(*cn, n);
    std::vector<uint8_t> all(p, p + n);
    conn_drain(*cn, n);

    std::vector<ks_head> out;
    size_t at = 0;
    while (at + sizeof(ks_head) <= all.size()) {
        ks_head h{};
        memcpy(&h, all.data() + at, sizeof(h));
        if (h.len < sizeof(ks_head) || at + h.len > all.size())
            break;
        out.push_back(h);
        if (bodies)
            bodies->push_back(std::vector<uint8_t>(all.begin() + at + sizeof(ks_head),
                                                   all.begin() + at + h.len));
        at += h.len;
    }
    CHECK_EQ(at, all.size()); // no trailing rubbish, ever
    return out;
}

// A reply by index, zeroed where there is none: a perturbation that makes the
// server answer too few times must fail an assertion rather than index past a
// vector and take the suite down with it.
ks_head at(const std::vector<ks_head> &r, size_t i)
{
    return i < r.size() ? r[i] : ks_head{};
}

// A reply's payload, checked before it is read: a server that answers with no
// payload where one is owed must fail an assertion, not crash the suite.
template <class T>
bool body_of(const std::vector<std::vector<uint8_t>> &bodies, size_t i, T &out)
{
    if (i >= bodies.size() || bodies[i].size() != sizeof(T)) {
        test_check(false, "the reply's payload is the size its op says",
                   __FILE_NAME__, __LINE__);
        return false;
    }
    memcpy(&out, bodies[i].data(), sizeof(T));
    return true;
}

void send(const std::vector<uint8_t> &f)
{
    feed(*cn, f.data(), f.size());
}

// A fresh server, a fresh connection, the handshake done.
void fresh(u32 cols = 8, u32 rows = 4)
{
    if (cn)
        conn_free(cn);
    if (srv)
        server_free(srv);
    screen_reset(t0());
    CHECK(screen_resize(t0(), cols, rows));
    srv = server_new(&t0());
    CHECK(srv != nullptr);
    cn = conn_new(*srv);
    CHECK(cn != nullptr);

    ks_hello hello{ KS_MAGIC, KS_ABI_VERSION };
    send(frame(KS_OP_HELLO, 1, 0, &hello, sizeof(hello)));
    std::vector<ks_head> r = replies();
    CHECK_EQ(r.size(), 1u);
    CHECK_EQ(at(r, 0).res, 0);
}

void take_screen()
{
    send(frame(KS_OP_SCREEN_CLAIM, 2, KS_F_TAKE, nullptr, 0));
    std::vector<ks_head> r = replies();
    CHECK_EQ(r.size(), 1u);
    CHECK_EQ(at(r, 0).res, 0);
    CHECK(conn_has_screen(*cn));
}

void take_keys()
{
    send(frame(KS_OP_KEY_CLAIM, 3, KS_F_TAKE, nullptr, 0));
    std::vector<ks_head> r = replies();
    CHECK_EQ(r.size(), 1u);
    CHECK_EQ(at(r, 0).res, 0);
    CHECK(conn_has_keys(*cn));
}

// One rejected frame: exactly one reply, that errno, and the connection is
// still usable afterwards unless the rejection was a protocol error.
void refused(const std::vector<uint8_t> &f, int32_t err, const char *what)
{
    uint64_t was = conn_stats(*cn).replies;
    send(f);
    std::vector<ks_head> r = replies();
    if (r.size() != 1) {
        test_check(false, what, __FILE_NAME__, __LINE__);
        return;
    }
    test_check_eq(u64(-at(r, 0).res), u64(err), what, __FILE_NAME__, __LINE__);
    test_check_eq(conn_stats(*cn).replies - was, 1, "exactly one reply", __FILE_NAME__, __LINE__);
    if (err == KS_EPROTO)
        test_check(conn_closed(*cn), "a protocol error closes", __FILE_NAME__, __LINE__);
    else
        test_check(!conn_closed(*cn), "content is not a protocol error", __FILE_NAME__, __LINE__);
}

char32_t cell_at(u32 x, u32 y)
{
    return screen_cells(t0())[size_t(y) * screen(t0()).cols + x].ch;
}

} // namespace

void test_proto()
{
    test_begin("proto");

    // ---------------------------------------------------------- the handshake
    //
    // Nothing works before it, and it happens once.
    fresh();
    CHECK_EQ(conn_stats(*cn).frames, 1u);

    fresh();
    ks_hello hello{ KS_MAGIC, KS_ABI_VERSION };
    refused(frame(KS_OP_HELLO, 9, 0, &hello, sizeof(hello)), KS_EPROTO, "a second handshake");

    fresh();
    conn_free(cn);
    cn = conn_new(*srv);
    refused(frame(KS_OP_TTY, 4, 0, nullptr, 0), KS_EPROTO, "an op before the handshake");

    fresh();
    conn_free(cn);
    cn = conn_new(*srv);
    ks_hello bad{ KS_MAGIC, KS_ABI_VERSION + 1 };
    refused(frame(KS_OP_HELLO, 1, 0, &bad, sizeof(bad)), KS_EPROTO, "a version we do not know");

    // ------------------------------------------------------ the frame itself
    //
    // Everything here is a length or a reserved field: the stream cannot be
    // resynchronised, so each is one -EPROTO and the end of the connection.
    {
        fresh();
        std::vector<uint8_t> f = frame(KS_OP_TTY, 5, 0, nullptr, 0);
        // Eight, not twelve: a length below the header must be refused *by*
        // the lower bound, and twelve would be caught by the alignment rule
        // whatever the bound did.
        uint32_t len = 8;
        memcpy(f.data(), &len, 4);
        refused(f, KS_EPROTO, "a len below the header");
    }
    {
        fresh();
        std::vector<uint8_t> f = frame(KS_OP_TTY, 5, 0, nullptr, 0);
        uint32_t len           = KS_MAX_FRAME + 8;
        memcpy(f.data(), &len, 4);
        refused(f, KS_EPROTO, "a len above the cap");
    }
    {
        fresh();
        std::vector<uint8_t> f = frame(KS_OP_TTY, 5, 0, nullptr, 0);
        uint32_t len           = 20;
        memcpy(f.data(), &len, 4);
        refused(f, KS_EPROTO, "a len that is not a multiple of eight");
    }
    {
        fresh();
        refused(frame(KS_OP_TTY, KS_SEQ_UNSOLICITED, 0, nullptr, 0), KS_EPROTO,
                "the reserved seq");
    }
    {
        fresh();
        std::vector<uint8_t> f = frame(KS_OP_TTY, 5, 0, nullptr, 0);
        int32_t res            = -1;
        memcpy(f.data() + 12, &res, 4);
        refused(f, KS_EPROTO, "a request carrying a result");
    }

    // ---------------------------------------------------------- the content
    //
    // Each of these parses. The connection carries on afterwards, which is the
    // half of E1 that a byte stream can keep.
    fresh();
    refused(frame(KS_OP_MAX, 6, 0, nullptr, 0), KS_ENOSYS, "an op past the table");
    refused(frame(KS_OP_TERM_OPEN, 7, 0, nullptr, 0), KS_ENOSYS, "the reserved op");
    refused(frame(KS_OP_TTY, 8, 1, nullptr, 0), KS_EINVAL, "a flag bit this op does not take");
    refused(frame(KS_OP_KEY_CLAIM, 9, 2, nullptr, 0), KS_EINVAL, "a flag bit outside the mask");

    {
        // A fixed-length op with the wrong length.
        ks_cursor_req q{};
        std::vector<uint8_t> f = frame(KS_OP_CURSOR, 10, 0, &q, sizeof(q) - 8);
        refused(f, KS_EINVAL, "a fixed op with the wrong length");
    }
    {
        ks_cursor_req q{};
        q.rsvd0 = 1;
        refused(frame(KS_OP_CURSOR, 11, 0, &q, sizeof(q)), KS_EINVAL,
                "a non-zero reserved field");
    }
    {
        ks_style st{};
        st.style = KS_STYLE_KEEP;
        refused(frame(KS_OP_STYLE, 12, 0, &st, sizeof(st)), KS_EINVAL,
                "the sticky sentinel as a style");
    }

    // A claim somebody else holds, and an op that needs one this connection
    // does not have.
    fresh();
    refused(frame(KS_OP_SCREEN_CLEAR, 13, 0, nullptr, 0), KS_ENOTTY, "a blit without the claim");
    refused(frame(KS_OP_KEY_READ, 14, 0, nullptr, 0), KS_ENOTTY, "a key read without the keys");
    {
        take_screen();
        Conn *other = conn_new(*srv);
        Conn *was   = cn;
        cn          = other;
        ks_hello h2{ KS_MAGIC, KS_ABI_VERSION };
        send(frame(KS_OP_HELLO, 1, 0, &h2, sizeof(h2)));
        replies();
        refused(frame(KS_OP_SCREEN_CLAIM, 2, KS_F_TAKE, nullptr, 0), KS_EPERM,
                "a screen somebody else holds");
        cn = was;
        conn_free(other);
    }

    // A second parked key read.
    fresh();
    take_keys();
    send(frame(KS_OP_KEY_READ, 20, 0, nullptr, 0));
    CHECK_EQ(replies().size(), 0u); // parked: no reply until a key arrives
    refused(frame(KS_OP_KEY_READ, 21, 0, nullptr, 0), KS_EBUSY, "a second parked key read");
    server_key(*srv, 'c', KS_MOD_CTRL);
    {
        std::vector<std::vector<uint8_t>> bodies;
        std::vector<ks_head> r = replies(&bodies);
        CHECK_EQ(r.size(), 1u);
        CHECK_EQ(at(r, 0).seq, 20u);
        CHECK_EQ(at(r, 0).res, 0);
        ks_key k{};
        CHECK(body_of(bodies, 0, k));
        CHECK_EQ(k.code, u32('c'));
        CHECK_EQ(k.mods, u32(KS_MOD_CTRL));
        CHECK_EQ(k.cols, 8u);
    }

    // ------------------------------------------------------------- the blit
    fresh();
    take_screen();
    {
        ks_blit b{};
        b.x = 1; b.y = 1; b.w = 2; b.h = 1;
        b.cols = 8; b.rows = 4;
        send(blit_frame(30, b, 2));
        std::vector<ks_head> r = replies();
        CHECK_EQ(r.size(), 1u);
        CHECK_EQ(at(r, 0).res, 0);
        CHECK_EQ(at(r, 0).flags, 0u);
        CHECK_EQ(cell_at(1, 1), 'x');
        CHECK_EQ(cell_at(3, 1), 0u);
    }
    {
        // The rectangle runs off a geometry it agrees with: a real bug.
        ks_blit b{};
        b.x = 7; b.y = 0; b.w = 4; b.h = 1;
        b.cols = 8; b.rows = 4;
        refused(blit_frame(31, b, 4), KS_EINVAL, "a blit past its own geometry");
    }
    {
        // The cells do not match the rectangle.
        ks_blit b{};
        b.x = 0; b.y = 0; b.w = 2; b.h = 2;
        b.cols = 8; b.rows = 4;
        refused(blit_frame(32, b, 3), KS_EINVAL, "a blit whose cells do not fit its rectangle");
    }
    {
        ks_blit b{};
        b.x = 0; b.y = 0; b.w = 1; b.h = 1;
        b.cols = 8; b.rows = 4;
        b.rsvd0 = 7;
        refused(blit_frame(33, b, 1), KS_EINVAL, "a blit with a reserved field set");
    }
    {
        // Stale: the client packed it against a geometry that has moved. Not
        // an error — draw nothing, say so, and let it repaint.
        ks_blit b{};
        b.x = 0; b.y = 0; b.w = 2; b.h = 1;
        b.cols = 40; b.rows = 12;
        char32_t was = cell_at(0, 0);
        send(blit_frame(34, b, 2));
        std::vector<std::vector<uint8_t>> bodies;
        std::vector<ks_head> r = replies(&bodies);
        CHECK_EQ(r.size(), 1u);
        CHECK_EQ(at(r, 0).res, 0);
        CHECK_EQ(at(r, 0).flags, u32(KS_F_STALE));
        CHECK_EQ(cell_at(0, 0), was); // nothing was drawn
        ks_geom g{};
        CHECK(body_of(bodies, 0, g));
        CHECK_EQ(g.cols, 8u); // and it carries the geometry to repaint against
        CHECK_EQ(g.rows, 4u);
    }

    // ----------------------------------------------------------- the resize
    //
    // A parked key read is answered -EINTR *with* a payload: the daemon owns
    // the reply and cannot invent a key. It is the only negative res that
    // carries one.
    fresh();
    take_keys();
    send(frame(KS_OP_KEY_READ, 40, 0, nullptr, 0));
    CHECK_EQ(replies().size(), 0u);
    server_resize(*srv, 20, 6);
    {
        std::vector<std::vector<uint8_t>> bodies;
        std::vector<ks_head> r = replies(&bodies);
        CHECK_EQ(r.size(), 1u);
        CHECK_EQ(at(r, 0).seq, 40u);
        CHECK_EQ(-at(r, 0).res, KS_EINTR);
        CHECK_EQ(at(r, 0).len, ks_err_len(KS_OP_KEY_READ, -KS_EINTR));
        ks_key k{};
        CHECK(body_of(bodies, 0, k));
        CHECK_EQ(k.code, 0u); // no key came with it
        CHECK_EQ(k.cols, 20u);
        CHECK_EQ(k.rows, 6u);
    }

    // ---------------------------------------------------------- the framing
    //
    // One frame split three ways, and two frames in one read, are both the
    // same as a whole frame.
    fresh();
    {
        // A frame with a payload, so the middle piece is a *whole header and
        // no body*: that is the partial case a server gets wrong, and a split
        // that stops short of the header never reaches the code that handles
        // it.
        ks_cursor_req q{};
        std::vector<uint8_t> f = frame(KS_OP_CURSOR, 50, 0, &q, sizeof(q));
        feed(*cn, f.data(), 4);
        CHECK_EQ(replies().size(), 0u);
        feed(*cn, f.data() + 4, 16); // the header is complete, the body is not
        CHECK_EQ(replies().size(), 0u);
        CHECK(!conn_closed(*cn));
        feed(*cn, f.data() + 20, f.size() - 20);
        std::vector<ks_head> r = replies();
        CHECK_EQ(r.size(), 1u);
        CHECK_EQ(at(r, 0).seq, 50u);
        CHECK_EQ(at(r, 0).res, 0);
    }
    {
        std::vector<uint8_t> a = frame(KS_OP_TTY, 51, 0, nullptr, 0);
        std::vector<uint8_t> b = frame(KS_OP_TTY, 52, 0, nullptr, 0);
        a.insert(a.end(), b.begin(), b.end());
        feed(*cn, a.data(), a.size());
        std::vector<ks_head> r = replies();
        CHECK_EQ(r.size(), 2u);
        CHECK_EQ(at(r, 0).seq, 51u);
        CHECK_EQ(at(r, 1).seq, 52u);
    }
    {
        // EOF in the middle of a frame is a client that went away, not a
        // protocol error: nothing was accepted, so nothing is owed a reply.
        uint64_t was           = conn_stats(*cn).replies;
        std::vector<uint8_t> f = frame(KS_OP_TTY, 53, 0, nullptr, 0);
        feed(*cn, f.data(), 8);
        conn_eof(*cn);
        CHECK_EQ(conn_stats(*cn).replies, was);
        CHECK(conn_closed(*cn));
    }

    // ------------------------------------------------------ the claim's life
    //
    // Take the screen, paint on it, drop the connection: the cells that were
    // there before are back. This is Braam's ~FullScreen, and it holds on a
    // kill because the kernel closes the socket.
    fresh();
    screen_write(t0(), "before");
    CHECK_EQ(cell_at(0, 0), 'b');
    take_screen();
    CHECK_EQ(cell_at(0, 0), 0u); // the alternate screen starts blank
    {
        ks_blit b{};
        b.x = 0; b.y = 0; b.w = 3; b.h = 1;
        b.cols = 8; b.rows = 4;
        send(blit_frame(60, b, 3));
        replies();
        CHECK_EQ(cell_at(0, 0), 'x');
    }
    conn_free(cn);
    cn = nullptr;
    CHECK_EQ(cell_at(0, 0), 'b'); // the scrolling screen came back
    CHECK_EQ(cell_at(5, 0), 'e');

    // Giving it back explicitly restores it too, and the connection lives on.
    fresh();
    screen_write(t0(), "kept");
    take_screen();
    send(frame(KS_OP_SCREEN_CLAIM, 61, 0, nullptr, 0)); // no KS_F_TAKE: give it back
    {
        std::vector<ks_head> r = replies();
        CHECK_EQ(r.size(), 1u);
        CHECK_EQ(at(r, 0).res, 0);
    }
    CHECK(!conn_has_screen(*cn));
    CHECK_EQ(cell_at(0, 0), 'k');
    CHECK(!conn_closed(*cn));

    // --------------------------------------------------------------- echo
    //
    // Implemented rather than stubbed: koru has no shell to call it, but feed
    // is pure, so it can be driven and it can be made falsifiable.
    fresh(16, 3);
    {
        std::vector<uint8_t> body(sizeof(ks_echo) + 2 * sizeof(ks_run) + 8);
        ks_echo e{};
        e.x = 0; e.y = 0; e.cur = 5; e.runs = 2;
        memcpy(body.data(), &e, sizeof(e));
        ks_run r0{ ks_style_pack(KS_COLOR_RED, KS_COLOR_BLACK, 0), 3 };
        ks_run r1{ KS_STYLE_KEEP, 5 };
        memcpy(body.data() + sizeof(e), &r0, sizeof(r0));
        memcpy(body.data() + sizeof(e) + sizeof(r0), &r1, sizeof(r1));
        memcpy(body.data() + sizeof(e) + 2 * sizeof(ks_run), "$ hello", 7);
        body[sizeof(e) + 2 * sizeof(ks_run) + 7] = 0; // the pad, which must be zero

        send(frame(KS_OP_ECHO, 70, KS_F_ECHO_SHOW, body.data(), body.size()));
        std::vector<std::vector<uint8_t>> bodies;
        std::vector<ks_head> rep = replies(&bodies);
        CHECK_EQ(rep.size(), 1u);
        CHECK_EQ(at(rep, 0).res, 0);
        CHECK_EQ(cell_at(0, 0), '$');
        CHECK_EQ(cell_at(2, 0), 'h');
        CHECK_EQ(screen_cells(t0())[0].fg, u8(KS_COLOR_RED));
        ks_cursor cur{};
        CHECK(body_of(bodies, 0, cur));
        CHECK_EQ(cur.x, 5u); // cur cells past the anchor
        CHECK_EQ(cur.y, 0u);
        CHECK_EQ(cur.on, 1u);
        CHECK_EQ(cur.scrolled, 0u);
    }
    {
        // A run count past the maximum, and a length table that does not add
        // up: both paint nothing.
        char32_t was = cell_at(0, 0);
        ks_echo e{};
        e.runs = KS_ECHO_RUNS_MAX + 1;
        std::vector<uint8_t> body(sizeof(e) + (KS_ECHO_RUNS_MAX + 1) * sizeof(ks_run));
        memcpy(body.data(), &e, sizeof(e));
        refused(frame(KS_OP_ECHO, 71, 0, body.data(), body.size()), KS_EINVAL,
                "an echo with too many runs");
        CHECK_EQ(cell_at(0, 0), was);

        std::vector<uint8_t> b2(sizeof(ks_echo) + sizeof(ks_run) + 8);
        ks_echo e2{};
        e2.runs = 1;
        memcpy(b2.data(), &e2, sizeof(e2));
        ks_run r{ KS_STYLE_KEEP, 99 }; // more bytes than the frame holds
        memcpy(b2.data() + sizeof(e2), &r, sizeof(r));
        refused(frame(KS_OP_ECHO, 72, 0, b2.data(), b2.size()), KS_EINVAL,
                "an echo whose runs do not add up");
        CHECK_EQ(cell_at(0, 0), was);
    }

    // --------------------------------------------------------- the byte channel
    //
    // T38b: the second connection, whose far end is the parser. Its handshake
    // is its last frame; the bytes after it are not frames and are answered
    // with nothing at all.
    fresh(16, 3);
    {
        Conn *ctrl = cn;
        ks_hello hi{ KS_MAGIC, KS_ABI_VERSION };

        cn = conn_new(*srv);
        CHECK(cn != nullptr);
        send(frame(KS_OP_HELLO, 1, KS_F_BYTES, &hi, sizeof(hi)));
        {
            std::vector<ks_head> r = replies();
            CHECK_EQ(r.size(), 1u);
            CHECK_EQ(at(r, 0).res, 0);
        }
        CHECK(conn_is_bytes(*cn));

        // Bytes, not a frame: they paint, and nothing is written back.
        feed(*cn, reinterpret_cast<const uint8_t *>("hi"), 2);
        CHECK_EQ(cell_at(0, 0), 'h');
        CHECK_EQ(cell_at(1, 0), 'i');
        CHECK_EQ(replies().size(), 0u);
        CHECK_EQ(conn_stats(*cn).frames, 1u);
        CHECK_EQ(conn_stats(*cn).replies, 1u);
        CHECK(!conn_closed(*cn));

        // A frame it would have answered before its handshake is text now.
        std::vector<uint8_t> f = frame(KS_OP_TTY, 2, 0, nullptr, 0);
        feed(*cn, f.data(), f.size());
        CHECK_EQ(replies().size(), 0u);
        CHECK_EQ(conn_stats(*cn).frames, 1u);
        conn_free(cn);

        // The handshake and the bytes behind it can share one read.
        screen_clear(t0());
        cn                     = conn_new(*srv);
        std::vector<uint8_t> h = frame(KS_OP_HELLO, 1, KS_F_BYTES, &hi, sizeof(hi));
        h.insert(h.end(), { 'a', 'b' });
        feed(*cn, h.data(), h.size());
        CHECK_EQ(replies().size(), 1u);
        CHECK_EQ(cell_at(0, 0), 'a');
        CHECK_EQ(cell_at(1, 0), 'b');
        conn_free(cn);

        // A flag on HELLO that names nothing is content, not a kind.
        cn = conn_new(*srv);
        refused(frame(KS_OP_HELLO, 1, 0x8000, &hi, sizeof(hi)), KS_EINVAL,
                "a hello flag this daemon does not know");
        CHECK(!conn_is_bytes(*cn));
        conn_free(cn);

        // The ordering rule: while somebody holds the alternate screen the
        // bytes are the *scrolling* screen's, so they wait for it rather than
        // landing in the middle of a blit.
        cn = ctrl;
        screen_clear(t0());
        take_screen();
        Conn *bytes = conn_new(*srv);
        feed(*bytes, h.data(), sizeof(ks_head) + sizeof(hi)); // its handshake alone
        feed(*bytes, reinterpret_cast<const uint8_t *>("held"), 4);
        CHECK_EQ(cell_at(0, 0), 0u); // the alternate screen is untouched
        CHECK_EQ(server_deferred(*srv), 4u);

        send(frame(KS_OP_SCREEN_CLAIM, 80, 0, nullptr, 0)); // give it back
        replies();
        CHECK(!conn_has_screen(*cn));
        CHECK_EQ(server_deferred(*srv), 0u);
        CHECK_EQ(cell_at(0, 0), 'h');
        CHECK_EQ(cell_at(3, 0), 'd');

        // What is held is bounded, and it is the tail that is kept: a program
        // printing into a claimed screen must not grow the daemon without end.
        take_screen();
        std::vector<uint8_t> flood(1u << 20, ' ');
        feed(*bytes, flood.data(), flood.size());
        CHECK(server_deferred(*srv) < flood.size());
        feed(*bytes, reinterpret_cast<const uint8_t *>("\rEND"), 4);
        send(frame(KS_OP_SCREEN_CLAIM, 81, 0, nullptr, 0));
        replies();
        CHECK_EQ(server_deferred(*srv), 0u);
        CHECK_EQ(cell_at(0, 2), 'E'); // the tail, on the row the flood left
        conn_free(bytes);
    }

    // Every case above asserted its own replies; this is the invariant over
    // all of them, which is what C1 transposed actually says — with the one
    // term for a parked read, whose reply is owed and not yet written.
    CHECK_EQ(conn_stats(*cn).replies + (conn_parked(*cn) ? 1 : 0), conn_stats(*cn).frames);
    CHECK(!conn_parked(*cn));

    conn_free(cn);
    cn = nullptr;
    server_free(srv);
    srv = nullptr;
}
