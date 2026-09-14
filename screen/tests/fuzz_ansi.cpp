// SPDX-License-Identifier: MIT
//
// The parser's fuzz oracle. Not "it did not crash" — ASan says that — but that
// after every write the cursor, the damage rectangle and the region margins are
// all inside the grid.
//
// Two drivers over one `LLVMFuzzerTestOneInput`. The standalone one is a PRNG
// and needs no clang runtime, so it is what the everyday gate runs:
//
//   scripts/screen.sh                       # ks_ansi_rand, under ASan and UBSan
//   KS_SEED=12345 ./build-asan/ks_ansi_rand # replay a failure
//
//   cmake -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DKORU_FUZZ=ON
//   ./build-fuzz/ks_ansi_fuzz -max_total_time=30

#include "ansi.h"
#include "screen.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

Term *term;


void die(const char *what)
{
    fprintf(stderr, "ks_ansi_fuzz: %s\n", what);
    abort();
}

// A damage rectangle, wherever it came from. Checked separately because a
// flush's is in its *return*: reading the terminal after one sees the cleared
// state, which is how the first version of this oracle missed the cursor fold
// entirely.
void check_rect(const Term &t, Rect d)
{
    const Screen &g = screen(t);
    if (!d.w)
        return;
    if (d.x >= g.cols || d.y >= g.rows || d.x + d.w > g.cols || d.y + d.h > g.rows)
        die("the damage left the grid");
    if (!d.h)
        die("a damage rectangle with width and no height");
}

// Every invariant the grid must hold, whatever bytes went in.
void check(Term &t)
{
    const Screen &g = screen(t);
    if (!g.cols || !g.rows || g.cols > SCREEN_MAX_COLS || g.rows > SCREEN_MAX_ROWS)
        die("the geometry left its bounds");
    // cursor_x may equal cols: the wrap is deferred.
    if (g.cursor_x > g.cols || g.cursor_y >= g.rows)
        die("the cursor left the grid");
    // The stored margins, not the clamped accessors: a clamp always passes.
    u32 rtop, rbot;
    screen_region_stored(t, rtop, rbot);
    if (rtop > rbot || rbot >= g.rows)
        die("the region left the grid");
    if (screen_region_top(t) > screen_region_bot(t) || screen_region_bot(t) >= g.rows)
        die("the clamped region left the grid");

    check_rect(t, screen_damage(t));
    if (screen_view(t) > screen_history(t))
        die("the view is past the history");

    // Every cell is drawable: the parser is the only writer here, and a
    // surrogate or a value past U+10FFFF would reach the renderer.
    const Cell *cells = screen_shown(t);
    for (size_t i = 0; i < size_t(g.cols) * g.rows; i++) {
        u32 ch = cells[i].ch;
        if (ch != u32(rune_safe(char32_t(ch))))
            die("a cell holds a codepoint the renderer cannot draw");
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (!term) {
        term = term_new();
        if (!term)
            die("the terminal would not allocate");
    }

    // A fresh grid per input, in a geometry the input itself picks: the first
    // byte is the width and the second the height, so a case that only matters
    // on a 1x1 grid is reachable.
    u32 cols = size ? 1 + (data[0] % 24) : 8;
    u32 rows = size > 1 ? 1 + (data[1] % 8) : 4;
    screen_reset(*term);
    if (!screen_resize(*term, cols, rows))
        die("the grid would not allocate");

    // Byte at a time, because a sequence split across writes is the state
    // machine's hardest case and the corpus should reach it. Flushed often,
    // because the cursor's own damage is folded in against where the *last*
    // flush left it: one flush per input never exercises that, and the cell a
    // parked cursor sits on is exactly where an off-by-one hides.
    for (size_t i = 2; i < size; i++) {
        screen_write(*term, Str(reinterpret_cast<const char *>(data) + i, 1));
        check(*term);
        // The byte itself decides, so libFuzzer's input stays deterministic.
        if ((data[i] & 3) == 0) {
            check_rect(*term, screen_flush(*term));
            check(*term);
        }
    }
    check_rect(*term, screen_flush(*term));
    check(*term);
    return 0;
}

#ifdef KS_FUZZ_STANDALONE

// A PRNG driver, so the oracle runs where libFuzzer's runtime is not
// installed. It emits whole *sequences* rather than bytes from an interesting
// set: uniform noise forms `ESC [ 1 ; 9 9 r` about once in 10^10 positions, so
// a byte-level generator tests the state machine and nothing the state machine
// dispatches to. libFuzzer's coverage feedback is what finds these on its own.
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

void put(uint8_t *buf, size_t cap, size_t &n, const char *s)
{
    for (; *s && n < cap; s++)
        buf[n++] = uint8_t(*s);
}

void put_num(uint8_t *buf, size_t cap, size_t &n, uint32_t v)
{
    char tmp[12];
    int k = snprintf(tmp, sizeof(tmp), "%u", v);
    for (int i = 0; i < k && n < cap; i++)
        buf[n++] = uint8_t(tmp[i]);
}

// A parameter value. One in three is drawn from the geometry's own edges,
// which is where an off-by-one lives: a uniform value reaches `rows + 1` about
// once in a thousand, and the guard it tests is two sequences deep.
uint32_t param_value(u32 cols, u32 rows)
{
    if (pick(3))
        return pick(4) ? pick(40) : pick(70000);
    const uint32_t edges[] = { 0, 1, cols - 1, cols, cols + 1, rows - 1, rows, rows + 1, 65535 };
    return edges[pick(sizeof(edges) / sizeof(edges[0]))];
}

// One token: text, a C0 byte, a two-byte escape, a CSI with parameters, or a
// string.
void token(uint8_t *buf, size_t cap, size_t &n, u32 cols, u32 rows)
{
    static const char finals[] = "@ABCDEFGHIJKLMPSTXZdfghlmr`su";
    switch (pick(10)) {
    case 0:
    case 1:
    case 2: // text, including UTF-8 leads and continuations
        for (uint32_t i = pick(6); i-- > 0 && n < cap;)
            buf[n++] = uint8_t(pick(256));
        return;
    case 3: // a C0 byte
        if (n < cap)
            buf[n++] = uint8_t("\b\t\n\r\f\v\a"[pick(7)]);
        return;
    case 4: // ESC and one final
        put(buf, cap, n, "\033");
        if (n < cap)
            buf[n++] = uint8_t("78DEMHc()"[pick(9)]);
        return;
    case 5: // a string, ended one way or the other
        put(buf, cap, n, "\033]0;x");
        put(buf, cap, n, pick(2) ? "\007" : "\033\\");
        return;
    default: { // CSI
        put(buf, cap, n, "\033[");
        if (!pick(4))
            put(buf, cap, n, "?");
        for (uint32_t i = 0, params = pick(4); i < params; i++) {
            if (i)
                put(buf, cap, n, ";");
            put_num(buf, cap, n, param_value(cols, rows));
        }
        if (n < cap)
            buf[n++] = uint8_t(finals[pick(sizeof(finals) - 1)]);
        return;
    }
    }
}

} // namespace

int main()
{
    const char *seed_env  = getenv("KS_SEED");
    const char *iter_env  = getenv("KS_ITERS");
    uint64_t seed = seed_env ? strtoull(seed_env, nullptr, 10) : 20260913;
    unsigned long iters = iter_env ? strtoul(iter_env, nullptr, 10) : 20000;
    rng_state = seed;
    printf("ks_ansi_rand: seed %llu, %lu inputs\n", (unsigned long long)seed, iters);

    uint8_t buf[512];
    for (unsigned long i = 0; i < iters; i++) {
        size_t n = 0;
        // Half the inputs take a tiny grid: filling a row is what parks the
        // cursor past the last column, and a 24-column grid is rarely filled.
        buf[n++] = uint8_t(pick(2) ? pick(4) : pick(256));
        buf[n++] = uint8_t(pick(2) ? pick(3) : pick(256));
        u32 cols = 1 + (buf[0] % 24), rows = 1 + (buf[1] % 8);
        for (uint32_t k = 1 + pick(12); k-- > 0;)
            token(buf, sizeof(buf), n, cols, rows);
        LLVMFuzzerTestOneInput(buf, n);
    }
    term_free(term);
    term = nullptr;
    printf("ks_ansi_rand: OK\n");
    return 0;
}

#endif
