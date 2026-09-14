// SPDX-License-Identifier: MIT
//
// The protocol server's fuzz oracle: hostile bytes into `feed()`, and after
// every call the two invariants that must hold whatever arrived.
//
//   C1 transposed — every accepted frame produced exactly one reply, and the
//   replies are whole frames whose lengths tile the output exactly. A parked
//   KEY_READ is the one frame whose reply is owed rather than written, so the
//   sum carries it: this oracle found that exception on its first input.
//   T34's grid invariants — a blit cannot move the cursor, the damage or the
//   region outside the grid, whatever it claimed about its own geometry.
//
// Two drivers over one LLVMFuzzerTestOneInput, as the parser's has: libFuzzer
// where clang's runtime is installed, and a PRNG that needs nothing.
//
//   scripts/screen.sh
//   KS_SEED=12345 ./build-asan/ks_proto_rand

#include "proto.h"
#include "screen.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

Term *term;
Server *srv;

void die(const char *what)
{
    fprintf(stderr, "ks_proto_fuzz: %s\n", what);
    abort();
}

// The replies, as frames. A reply that is not a whole frame, or a byte outside
// one, is the failure this exists to catch.
u32 count_replies(Conn &c)
{
    size_t n         = 0;
    const uint8_t *p = conn_out(c, n);
    u32 frames       = 0;
    size_t at        = 0;
    while (at < n) {
        if (n - at < sizeof(ks_head))
            die("a reply is shorter than a header");
        ks_head h{};
        memcpy(&h, p + at, sizeof(h));
        if (h.len < sizeof(ks_head) || h.len % KS_ALIGN != 0 || h.len > KS_MAX_FRAME)
            die("a reply's own length is not a legal frame");
        if (at + h.len > n)
            die("a reply runs past the buffer");
        // -EINTR on a parked key read is the only negative result that carries
        // a payload, and a client is allowed to assert it.
        if (h.res < 0 && h.len != ks_err_len(h.op, h.res))
            die("an error reply carries a payload it should not");
        at += h.len;
        frames++;
    }
    conn_drain(c, n);
    return frames;
}

void check(Conn &c, Term &t)
{
    const Screen &g = screen(t);
    if (!g.cols || !g.rows || g.cols > SCREEN_MAX_COLS || g.rows > SCREEN_MAX_ROWS)
        die("the geometry left its bounds");
    if (g.cursor_x > g.cols || g.cursor_y >= g.rows)
        die("the cursor left the grid");
    u32 rtop, rbot;
    screen_region_stored(t, rtop, rbot);
    if (rtop > rbot || rbot >= g.rows)
        die("the region left the grid");

    Rect d = screen_damage(t);
    if (d.w && (d.x >= g.cols || d.y >= g.rows || d.x + d.w > g.cols || d.y + d.h > g.rows))
        die("the damage left the grid");

    const Cell *cells = screen_shown(t);
    for (size_t i = 0; i < size_t(g.cols) * g.rows; i++)
        if (cells[i].ch != u32(rune_safe(char32_t(cells[i].ch))))
            die("a cell holds a codepoint the renderer cannot draw");

    const ConnStats &s = conn_stats(c);
    if (s.replies + (conn_parked(c) ? 1 : 0) != s.frames)
        die("a frame was answered other than exactly once");
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (!term) {
        term = term_new();
        if (!term)
            die("the terminal would not allocate");
    }
    if (!srv) {
        srv = server_new(term);
        if (!srv)
            die("the server would not allocate");
    }

    // A geometry the input picks, and a connection per input: a client's whole
    // life is one input, which is what makes the claim's release reachable.
    u32 cols = size ? 1 + (data[0] % 24) : 8;
    u32 rows = size > 1 ? 1 + (data[1] % 8) : 4;
    screen_reset(*term);
    if (!screen_resize(*term, cols, rows))
        die("the grid would not allocate");

    Conn *c = conn_new(*srv);
    if (!c)
        die("the connection would not allocate");

    // Fed in pieces, because a frame split across reads is the case a server
    // gets wrong: the sizes come from the input, so libFuzzer can steer them.
    size_t at = 2;
    while (at < size && !conn_closed(*c)) {
        size_t take = 1 + (data[at] % 64);
        if (take > size - at)
            take = size - at;
        feed(*c, data + at, take);
        count_replies(*c);
        check(*c, *term);
        at += take;
    }

    // The events the window would raise, which is where -EINTR and the key
    // path live. They reply into a parked read and must obey C1 too.
    if (size > 3 && (data[2] & 1))
        server_key(*srv, data[3], data[2] >> 1);
    if (size > 5 && (data[4] & 1))
        server_resize(*srv, 1 + (data[5] % 40), 1 + (data[4] % 12));
    count_replies(*c);
    check(*c, *term);

    conn_eof(*c);
    conn_free(c); // the claims go back here, which the next input then sees
    return 0;
}

#ifdef KS_FUZZ_STANDALONE

namespace {

uint64_t rng_state;

uint32_t next_u32()
{
    rng_state = rng_state * 6364136223846793005ull + 1442695040888963407ull;
    return uint32_t(rng_state >> 33);
}

uint32_t pick(uint32_t n)
{
    return next_u32() % n;
}

void put(std::vector<uint8_t> &b, const void *p, size_t n)
{
    const uint8_t *q = static_cast<const uint8_t *>(p);
    b.insert(b.end(), q, q + n);
}

// Whole frames, mostly well-formed, because uniform noise never gets past the
// length check and would test the first `if` in feed() and nothing else.
void frame(std::vector<uint8_t> &b, u32 cols, u32 rows)
{
    uint32_t op  = pick(4) ? 1 + pick(KS_OP_MAX) : pick(70000);
    uint32_t seq = pick(8) ? 1 + pick(1000) : 0;
    uint16_t fl  = uint16_t(pick(4) ? 0 : pick(8));

    std::vector<uint8_t> body;
    if (op == KS_OP_HELLO) {
        ks_hello h{ pick(8) ? KS_MAGIC : next_u32(), pick(8) ? KS_ABI_VERSION : next_u32() };
        put(body, &h, sizeof(h));
    } else if (op == KS_OP_BLIT) {
        ks_blit bl{};
        bl.x         = pick(2) ? pick(cols + 2) : next_u32();
        bl.y         = pick(2) ? pick(rows + 2) : next_u32();
        bl.w         = pick(2) ? pick(4) : next_u32();
        bl.h         = pick(2) ? pick(3) : next_u32();
        bl.cursor_x  = pick(cols + 1);
        bl.cursor_y  = pick(rows + 1);
        bl.cursor_on = pick(2);
        bl.cols      = pick(4) ? cols : pick(40);
        bl.rows      = pick(4) ? rows : pick(12);
        bl.rsvd0     = pick(8) ? 0 : next_u32();
        put(body, &bl, sizeof(bl));
        // Usually the right number of cells, sometimes not.
        uint64_t want = uint64_t(bl.w) * bl.h;
        uint64_t have = pick(4) ? want : pick(8);
        for (uint64_t i = 0; i < have && body.size() < 4096; i++) {
            ks_cell cell{ next_u32(), uint8_t(pick(256)), uint8_t(pick(256)),
                          uint8_t(pick(256)), uint8_t(pick(2) ? 0 : pick(256)) };
            put(body, &cell, sizeof(cell));
        }
    } else if (op == KS_OP_ECHO) {
        ks_echo e{};
        e.x    = pick(cols + 2);
        e.y    = pick(rows + 2);
        e.cur  = pick(40);
        e.runs = pick(4) ? pick(KS_ECHO_RUNS_MAX + 2) : next_u32();
        put(body, &e, sizeof(e));
        uint32_t runs = e.runs < 16 ? e.runs : 0;
        std::vector<uint32_t> lens(runs);
        for (uint32_t i = 0; i < runs; i++) {
            lens[i] = pick(4) ? pick(12) : next_u32();
            ks_run r{ pick(2) ? ks_style_pack(uint8_t(pick(16)), uint8_t(pick(16)),
                                              uint8_t(pick(8)))
                              : KS_STYLE_KEEP,
                      lens[i] };
            put(body, &r, sizeof(r));
        }
        for (uint32_t i = 0; i < runs; i++)
            for (uint32_t k = 0; k < lens[i] && k < 64; k++)
                body.push_back(uint8_t(pick(256)));
    } else if (op == KS_OP_CURSOR) {
        ks_cursor_req q{ pick(cols + 2), pick(rows + 2), pick(2), pick(8) ? 0u : next_u32() };
        put(body, &q, sizeof(q));
    } else if (op == KS_OP_STYLE) {
        ks_style st{ pick(2) ? ks_style_pack(uint8_t(pick(16)), uint8_t(pick(16)),
                                             uint8_t(pick(8)))
                             : KS_STYLE_KEEP,
                     pick(8) ? 0u : next_u32() };
        put(body, &st, sizeof(st));
    }
    while (body.size() % KS_ALIGN)
        body.push_back(0);

    ks_head h{};
    h.len   = uint32_t(sizeof(ks_head) + body.size());
    h.op    = uint16_t(op);
    h.flags = fl;
    h.seq   = seq;
    h.res   = pick(16) ? 0 : int32_t(next_u32());
    if (!pick(8)) // and sometimes a length that is simply wrong
        h.len = next_u32() % 200;
    put(b, &h, sizeof(h));
    b.insert(b.end(), body.begin(), body.end());
}

} // namespace

int main()
{
    const char *seed_env = getenv("KS_SEED");
    const char *iter_env = getenv("KS_ITERS");
    uint64_t seed        = seed_env ? strtoull(seed_env, nullptr, 10) : 20260913;
    unsigned long iters  = iter_env ? strtoul(iter_env, nullptr, 10) : 20000;
    rng_state            = seed;
    printf("ks_proto_rand: seed %llu, %lu inputs\n", (unsigned long long)seed, iters);

    for (unsigned long i = 0; i < iters; i++) {
        std::vector<uint8_t> b;
        b.push_back(uint8_t(pick(256))); // the geometry
        b.push_back(uint8_t(pick(256)));
        u32 cols = 1 + (b[0] % 24), rows = 1 + (b[1] % 8);
        // The handshake first most of the time, so the ops past it are reached.
        if (pick(4)) {
            ks_hello hello{ KS_MAGIC, KS_ABI_VERSION };
            ks_head h{};
            h.len = uint32_t(sizeof(h) + sizeof(hello));
            h.op  = KS_OP_HELLO;
            h.seq = 1;
            put(b, &h, sizeof(h));
            put(b, &hello, sizeof(hello));
        }
        for (uint32_t k = 1 + pick(6); k-- > 0;)
            frame(b, cols, rows);
        LLVMFuzzerTestOneInput(b.data(), b.size());
    }

    server_free(srv);
    srv = nullptr;
    term_free(term);
    term = nullptr;
    printf("ks_proto_rand: OK\n");
    return 0;
}

#endif
