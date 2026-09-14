// SPDX-License-Identifier: MIT

#include "render.h"

#include "font8x8.h"

#include <algorithm>
#include <utility>

using std::min;

namespace {

// The sixteen, dim then bright. Braam's palette is the browser's CSS names;
// these are the same colours as ARGB.
const uint32_t PALETTE[KS_COLORS] = {
    0xff000000, 0xffcd0000, 0xff00cd00, 0xffcdcd00, 0xff0000ee, 0xffcd00cd,
    0xff00cdcd, 0xffe5e5e5, 0xff7f7f7f, 0xffff0000, 0xff00ff00, 0xffffff00,
    0xff5c5cff, 0xffff00ff, 0xff00ffff, 0xffffffff,
};

const uint8_t *glyph_of(uint32_t ch)
{
    if (ch == 0)
        return nullptr; // a blank cell draws background and nothing else
    if (ch >= KS_FONT_FIRST && ch <= KS_FONT_LAST)
        return KS_FONT[ch - KS_FONT_FIRST];
    return KS_FONT_MISSING;
}

// One cell: the background, then the glyph's ink, then the attributes that are
// drawn rather than coloured.
void draw_cell(Render &r, const Cell &c, u32 cx, u32 cy, bool cursor)
{
    u8 fg = c.fg, bg = c.bg;
    bool reverse = (c.attrs & KS_ATTR_REVERSE) != 0;
    if (reverse != cursor) // the cursor is a reverse of whatever is under it
        std::swap(fg, bg);
    if (c.attrs & KS_ATTR_BOLD)
        fg = u8(fg | KS_COLOR_BRIGHT);

    uint32_t ink = render_color(fg), back = render_color(bg);
    const uint8_t *g = glyph_of(c.ch);
    u32 x0 = cx * r.cw, y0 = cy * r.ch;

    for (u32 y = 0; y < r.ch; y++) {
        uint32_t *row = &r.px[size_t(y0 + y) * r.w + x0];
        u32 gy = y / r.scale;
        // The underline is the last drawn row of the glyph box, not the
        // leading: an underlined cell must not touch the cell below it.
        bool under = (c.attrs & KS_ATTR_UNDERLINE) && gy == KS_FONT_H - 1;
        uint8_t bits = (g && gy < KS_FONT_H) ? g[gy] : 0;
        for (u32 x = 0; x < r.cw; x++) {
            u32 gx = x / r.scale;
            bool on = under || (gx < KS_FONT_W && (bits >> gx & 1));
            row[x] = on ? ink : back;
        }
    }
}

} // namespace

uint32_t render_color(u8 index)
{
    return PALETTE[index % KS_COLORS];
}

bool render_resize(Render &r, u32 cols, u32 rows, u32 scale)
{
    if (!cols || !rows || !scale)
        return false;
    r.scale = scale;
    r.cw    = KS_FONT_W * scale;
    r.ch    = (KS_FONT_H + KS_CELL_LEAD) * scale;
    r.cols  = cols;
    r.rows  = rows;
    r.w     = cols * r.cw;
    r.h     = rows * r.ch;
    r.px.assign(size_t(r.w) * r.h, render_color(KS_COLOR_BLACK));
    return true;
}

void render_damage(Render &r, const Term &t, Rect d)
{
    if (!d.w || !d.h || r.px.empty())
        return;
    const Screen &g = screen(t);
    const Cell *cells = screen_shown(t);
    if (!cells || g.cols != r.cols || g.rows != r.rows)
        return; // a frame from a geometry that is not the one sized for

    u32 x1 = min(d.x + d.w, r.cols), y1 = min(d.y + d.h, r.rows);
    // The cursor is drawn, not stored, so it is a property of the cell under it.
    u32 curx = min(g.cursor_x, g.cols - 1), cury = g.cursor_y;

    for (u32 y = d.y; y < y1; y++)
        for (u32 x = d.x; x < x1; x++) {
            bool cursor = g.cursor_on && x == curx && y == cury;
            draw_cell(r, cells[size_t(y) * g.cols + x], x, y, cursor);
        }
}

uint32_t render_at(const Render &r, u32 x, u32 y)
{
    if (x >= r.w || y >= r.h)
        return 0;
    return r.px[size_t(y) * r.w + x];
}
