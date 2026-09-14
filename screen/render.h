// SPDX-License-Identifier: MIT
//
// Cells to pixels. Pure: it owns a pixel buffer and reads a Term, and it names
// no SDL — window.cpp uploads what this draws. That split is what lets the
// pixel oracles inspect a frame without a window, and what keeps the damage
// arithmetic testable.
//
// A cell is KS_FONT_W by KS_FONT_H plus the leading below it, times `scale`.
#pragma once

#include "screen.h"

#include <cstdint>
#include <vector>

// Rows of blank below a glyph, before the scale. A terminal cell is taller
// than it is wide, and an 8x8 glyph on its own is not.
#define KS_CELL_LEAD 2

struct Render {
    std::vector<uint32_t> px; // ARGB8888, row-major, w * h
    u32 w = 0, h = 0;         // pixels
    u32 cols = 0, rows = 0;   // cells
    u32 cw = 0, ch = 0;       // one cell, in pixels
    u32 scale = 1;
};

// The sixteen palette entries, ARGB8888. Index is KS_COLOR_* plus KS_COLOR_BRIGHT.
uint32_t render_color(u8 index);

// Sizes the buffer to this grid. Everything is damaged afterwards, since
// nothing in the new buffer was ever drawn.
bool render_resize(Render &r, u32 cols, u32 rows, u32 scale);

// Draws the cells of `d` out of the terminal, and the cursor if it falls in
// them. Nothing outside `d` is touched — which is the whole point, and what
// the damage oracle checks.
void render_damage(Render &r, const Term &t, Rect d);

// The pixel at (x, y), for the oracles.
uint32_t render_at(const Render &r, u32 x, u32 y);
