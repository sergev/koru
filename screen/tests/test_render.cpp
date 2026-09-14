// SPDX-License-Identifier: MIT
//
// T35's done test: five oracles that do not depend on what the font looks like.
// A golden image breaks on every glyph change and nobody regenerates one
// honestly, so none of these knows a single pixel's value in advance.
//
// Every one reads the frame back through SDL_RenderReadPixels on the offscreen
// driver, so the texture upload and the present are inside what is tested —
// a renderer that draws perfectly into its own buffer and uploads the wrong
// pitch would pass an oracle that read `Render::px` directly.

#include "harness.h"
#include "window.h"

#include <cstdio>
#include <cstdlib>
#include <map>

namespace {

Win w;
std::vector<uint32_t> frame;

u32 cell_w()
{
    return w.r.cw;
}

u32 cell_h()
{
    return w.r.ch;
}

uint32_t px(u32 x, u32 y)
{
    return (x < w.r.w && y < w.r.h) ? frame[size_t(y) * w.r.w + x] : 0;
}

// A frame: what the terminal has damaged, drawn, uploaded, presented and read
// back. The oracles never call anything else.
void present()
{
    win_present(w, t0());
    CHECK(win_read(w, frame));
}

// The modal colour of one cell, which is what "this cell is red" means when a
// glyph is drawn on top of it.
uint32_t modal_cell(u32 cx, u32 cy)
{
    std::map<uint32_t, u32> count;
    for (u32 y = 0; y < cell_h(); y++)
        for (u32 x = 0; x < cell_w(); x++)
            count[px(cx * cell_w() + x, cy * cell_h() + y)]++;
    uint32_t best = 0;
    u32 most      = 0;
    for (const auto &kv : count)
        if (kv.second > most) {
            most = kv.second;
            best = kv.first;
        }
    return best;
}

u32 ink_in_cell(u32 cx, u32 cy, uint32_t background)
{
    u32 n = 0;
    for (u32 y = 0; y < cell_h(); y++)
        for (u32 x = 0; x < cell_w(); x++)
            if (px(cx * cell_w() + x, cy * cell_h() + y) != background)
                n++;
    return n;
}

void fresh(u32 cols, u32 rows)
{
    screen_reset(t0());
    CHECK(screen_resize(t0(), cols, rows));
    CHECK(win_resize(w, cols, rows));
    screen_cursor(t0(), false); // the cursor is a reverse cell and would be ink
    present();
}

} // namespace

void test_render()
{
    test_begin("render");

    // The offscreen driver, at scale 1 so a cell is eight pixels by ten and
    // the arithmetic below is readable.
    setenv("SDL_VIDEODRIVER", "offscreen", 1);
    if (!win_open(w, "koru-screen test", 8, 4, 1)) {
        printf("FAIL render: SDL would not open a window: %s\n", SDL_GetError());
        CHECK(false);
        return;
    }
    printf("   driver %s, renderer %s, cell %ux%u\n", SDL_GetCurrentVideoDriver(),
           SDL_GetRendererName(w.renderer), cell_w(), cell_h());

    // ---------------------------------------------------------------- geometry
    //
    // The surface is exactly the cell size times the grid, and it follows a
    // resize. Nothing else here means anything if this is wrong.
    fresh(8, 4);
    CHECK_EQ(frame.size(), size_t(w.r.w) * w.r.h);
    CHECK_EQ(w.r.w, 8 * cell_w());
    CHECK_EQ(w.r.h, 4 * cell_h());
    fresh(20, 6);
    CHECK_EQ(w.r.w, 20 * cell_w());
    CHECK_EQ(w.r.h, 6 * cell_h());
    CHECK_EQ(frame.size(), size_t(20) * cell_w() * 6 * cell_h());

    // -------------------------------------------------------------------- ink
    //
    // A glyph's cell holds a pixel that is not the background and a blank cell
    // holds none. This is the only one that catches nothing being drawn at all.
    fresh(8, 2);
    uint32_t back = render_color(KS_COLOR_BLACK);
    screen_write(t0(), "A");
    present();
    CHECK(ink_in_cell(0, 0, back) > 0);
    CHECK_EQ(ink_in_cell(1, 0, back), 0u);
    CHECK_EQ(ink_in_cell(0, 1, back), 0u);

    // A space is blank, and a codepoint outside the font is not: "no glyph"
    // must never look like "nothing there".
    fresh(8, 2);
    screen_write(t0(), " ");
    present();
    CHECK_EQ(ink_in_cell(0, 0, back), 0u);
    screen_move(t0(), 1, 0);
    screen_put(t0(), 0x4e2d); // a han character, which this font does not have
    present();
    CHECK(ink_in_cell(1, 0, back) > 0);

    // ------------------------------------------------------------- isolation
    //
    // One cell's pixels are exactly its own rectangle. This one is absolute,
    // not a difference between two frames: a cell origin off by one pixel
    // shifts *every* cell the same way, so a before-and-after comparison sees
    // nothing at all. Asking where one background colour ends is what catches
    // it.
    fresh(8, 2);
    screen_style(t0(), KS_COLOR_WHITE, KS_COLOR_RED, 0);
    screen_write(t0(), "        \r\n        ");
    screen_move(t0(), 3, 0);
    screen_style(t0(), KS_COLOR_WHITE, KS_COLOR_BLUE, 0);
    screen_put(t0(), ' '); // a blank cell, so the whole rectangle is one colour
    present();

    uint32_t blue = render_color(KS_COLOR_BLUE);
    u32 blue_in = 0, blue_out = 0, wrong_in = 0;
    for (u32 y = 0; y < w.r.h; y++)
        for (u32 x = 0; x < w.r.w; x++) {
            bool in = x >= 3 * cell_w() && x < 4 * cell_w() && y < cell_h();
            if (px(x, y) == blue) {
                if (in)
                    blue_in++;
                else
                    blue_out++;
            } else if (in) {
                wrong_in++;
            }
        }
    CHECK_EQ(blue_in, cell_w() * cell_h());
    CHECK_EQ(blue_out, 0u);
    CHECK_EQ(wrong_in, 0u);

    // And writing one cell changes no other: the same claim, from the other
    // direction, which is what catches a draw that runs past its cell.
    fresh(8, 2);
    screen_style(t0(), KS_COLOR_WHITE, KS_COLOR_BLUE, 0);
    screen_write(t0(), "xxxxxxxx");
    present();
    std::vector<uint32_t> before = frame;

    screen_move(t0(), 3, 0);
    screen_put(t0(), 'W');
    present();
    u32 inside = 0, outside = 0;
    for (u32 y = 0; y < w.r.h; y++)
        for (u32 x = 0; x < w.r.w; x++) {
            if (before[size_t(y) * w.r.w + x] == px(x, y))
                continue;
            bool in = x >= 3 * cell_w() && x < 4 * cell_w() && y < cell_h();
            if (in)
                inside++;
            else
                outside++;
        }
    CHECK(inside > 0);
    CHECK_EQ(outside, 0u);

    // ----------------------------------------------------------------- colour
    //
    // A red-background cell's modal pixel is the palette's red, and a red
    // foreground leaves the background alone. Swapping fg and bg fails this.
    fresh(8, 2);
    screen_style(t0(), KS_COLOR_WHITE, KS_COLOR_RED, 0);
    screen_write(t0(), "M");
    present();
    CHECK_EQ(modal_cell(0, 0), render_color(KS_COLOR_RED));
    CHECK(ink_in_cell(0, 0, render_color(KS_COLOR_RED)) > 0);

    fresh(8, 2);
    screen_style(t0(), KS_COLOR_RED, KS_COLOR_BLACK, 0);
    screen_write(t0(), "M");
    present();
    CHECK_EQ(modal_cell(0, 0), render_color(KS_COLOR_BLACK));

    // ----------------------------------------------------------------- damage
    //
    // A blit in the middle leaves every pixel outside it byte-identical to the
    // previous frame — and the cells outside it are *not* the same as the grid
    // holds, because this stages a second change and does not report it. That
    // is what separates a renderer that repaints the damage from one that
    // repaints everything: comparing two frames drawn from identical cells
    // would pass either way.
    fresh(8, 3);
    screen_style(t0(), KS_COLOR_WHITE, KS_COLOR_BLACK, 0);
    screen_write(t0(), "abcdefgh\nijklmnop\nqrstuvwx");
    present();
    before = frame;

    // Stage cells the way a blit does — straight into the grid — and say so
    // for one of the two. Nothing else tells the renderer anything changed.
    Cell *cells = screen_cells(t0());
    for (u32 x = 2; x < 5; x++)
        cells[1 * 8 + x] = Cell{ '#', KS_COLOR_WHITE, KS_COLOR_BLUE, 0, 0 };
    cells[2 * 8 + 7] = Cell{ '@', KS_COLOR_WHITE, KS_COLOR_RED, 0, 0 }; // unreported
    screen_touch(t0(), 2, 1, 3, 1);
    present();

    u32 changed_outside = 0, changed_inside = 0;
    for (u32 y = 0; y < w.r.h; y++)
        for (u32 x = 0; x < w.r.w; x++) {
            if (before[size_t(y) * w.r.w + x] == px(x, y))
                continue;
            bool in = x >= 2 * cell_w() && x < 5 * cell_w() && y >= cell_h() && y < 2 * cell_h();
            if (in)
                changed_inside++;
            else
                changed_outside++;
        }
    CHECK(changed_inside > 0);
    CHECK_EQ(changed_outside, 0u);

    // The snapshot the plan asks for, which is also how a human looks at a
    // failure. Written where $KORU_SCREEN_SNAP says, and nowhere by default.
    if (const char *path = getenv("KORU_SCREEN_SNAP")) {
        CHECK(win_snapshot(w, path));
        printf("   snapshot: %s\n", path);
    }

    // ------------------------------------------------------------- the premise
    //
    // A synthesised ctrl-C arrives as {'c', MOD_CTRL}, never as byte 0x03.
    // Everything above is about pixels; this is about why the grid exists.
    SDL_KeyboardEvent e{};
    e.type     = SDL_EVENT_KEY_DOWN;
    e.key      = SDLK_C;
    e.scancode = SDL_SCANCODE_C;
    e.mod      = SDL_KMOD_LCTRL;
    KsKey k;
    CHECK(win_key(e, k));
    CHECK_EQ(k.code, u32('c'));
    CHECK_EQ(k.mods, u32(KS_MOD_CTRL));
    CHECK(k.code != 3);

    // The same key with no modifier is the same code, which is what "there are
    // no control characters" means.
    e.mod = SDL_KMOD_NONE;
    CHECK(win_key(e, k));
    CHECK_EQ(k.code, u32('c'));
    CHECK_EQ(k.mods, 0u);

    // A named key is not a codepoint, and cannot collide with one.
    e.key      = SDLK_UP;
    e.scancode = SDL_SCANCODE_UP;
    CHECK(win_key(e, k));
    CHECK_EQ(k.code, u32(KS_KEY_UP));
    CHECK(k.code >= KS_KEY_NAMED);

    // Escape is a named key too, so a program never has to time a byte to tell
    // it from the start of a sequence.
    e.key      = SDLK_ESCAPE;
    e.scancode = SDL_SCANCODE_ESCAPE;
    CHECK(win_key(e, k));
    CHECK_EQ(k.code, u32(KS_KEY_ESCAPE));

    // A bare modifier carries nothing.
    e.key      = SDLK_LCTRL;
    e.scancode = SDL_SCANCODE_LCTRL;
    e.mod      = SDL_KMOD_LCTRL;
    CHECK(!win_key(e, k));

    // ------------------------------------------------------------- the resize
    //
    // A window of so many pixels is a grid of so many cells, and never zero.
    u32 cols = 0, rows = 0;
    win_grid_of(w, int(20 * cell_w()), int(6 * cell_h()), cols, rows);
    CHECK_EQ(cols, 20u);
    CHECK_EQ(rows, 6u);
    win_grid_of(w, int(20 * cell_w() + cell_w() / 2), int(6 * cell_h() - 1), cols, rows);
    CHECK_EQ(cols, 20u);
    CHECK_EQ(rows, 5u);
    win_grid_of(w, 1, 1, cols, rows);
    CHECK_EQ(cols, 1u);
    CHECK_EQ(rows, 1u);

    win_close(w);
}
