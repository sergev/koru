// SPDX-License-Identifier: MIT

#include <koru/grid.hpp>

#include <koru/filebuf.hpp>

namespace koru {

namespace {

u32 min_of(u32 a, u32 b)
{
    return a < b ? a : b;
}

u32 max_of(u32 a, u32 b)
{
    return a > b ? a : b;
}

} // namespace

// ---------------------------------------------------------------------------
// Grid
// ---------------------------------------------------------------------------

void Grid::resize(u32 cols, u32 rows)
{
    cells_.assign(size_t(cols) * rows, ks_cell{});
    cols_ = cols;
    rows_ = rows;
    if (cols == 0 || cursor_x >= cols)
        cursor_x = cols ? cols - 1 : 0;
    if (rows == 0 || cursor_y >= rows)
        cursor_y = rows ? rows - 1 : 0;
    damage_ = Rect{};
    touch(0, 0, cols, rows);
}

const ks_cell *Grid::at(u32 x, u32 y) const
{
    if (x >= cols_ || y >= rows_)
        return nullptr;
    return &cells_[size_t(y) * cols_ + x];
}

ks_cell *Grid::at_mut(u32 x, u32 y)
{
    if (x >= cols_ || y >= rows_)
        return nullptr;
    return &cells_[size_t(y) * cols_ + x];
}

void Grid::touch(u32 x, u32 y, u32 w, u32 h)
{
    if (cells_.empty() || w == 0 || h == 0 || x >= cols_ || y >= rows_)
        return;
    u32 x1 = min_of(x + w, cols_);
    u32 y1 = min_of(y + h, rows_);
    if (damage_.w == 0) {
        damage_ = Rect{ x, y, x1 - x, y1 - y };
        return;
    }
    u32 dx1   = max_of(damage_.x + damage_.w, x1);
    u32 dy1   = max_of(damage_.y + damage_.h, y1);
    damage_.x = min_of(damage_.x, x);
    damage_.y = min_of(damage_.y, y);
    damage_.w = dx1 - damage_.x;
    damage_.h = dy1 - damage_.y;
}

Rect Grid::take_damage()
{
    Rect d  = damage_;
    damage_ = Rect{};
    return d;
}

// ---------------------------------------------------------------------------
// Pane
// ---------------------------------------------------------------------------

Pane::Pane(u32 x, u32 y, u32 w, u32 h) : x_(x), y_(y), w_(w), h_(h) {}

Pane Pane::sub(u32 x, u32 y, u32 w, u32 h) const
{
    if (x >= w_ || y >= h_)
        return Pane(x_ + w_, y_ + h_, 0, 0);
    return Pane(x_ + x, y_ + y, min_of(w, w_ - x), min_of(h, h_ - y));
}

Pane Pane::bottom(u32 rows) const
{
    u32 n = min_of(rows, h_);
    return sub(0, h_ - n, w_, n);
}

void Pane::style(u8 fg, u8 bg, u8 attrs)
{
    fg_    = fg;
    bg_    = bg;
    attrs_ = attrs;
}

void Pane::move_to(u32 x, u32 y)
{
    cx_ = w_ > 0 ? min_of(x, w_ - 1) : 0;
    cy_ = h_ > 0 ? min_of(y, h_ - 1) : 0;
}

void Pane::put(Grid &g, u32 ch)
{
    if (cx_ >= w_)
        return;
    put_raw(g, ch);
}

void Pane::put_raw(Grid &g, u32 ch)
{
    if (cx_ >= w_ || cy_ >= h_)
        return;
    u32 gx = x_ + cx_, gy = y_ + cy_;
    if (ks_cell *c = g.at_mut(gx, gy)) {
        c->ch    = ch;
        c->fg    = fg_;
        c->bg    = bg_;
        c->attrs = attrs_;
        c->rsvd0 = 0;
        g.touch(gx, gy, 1, 1);
    }
    cx_++;
}

void Pane::write(Grid &g, Str text)
{
    Span<u8> b = Span<u8>(reinterpret_cast<const u8 *>(text.data()), text.size());
    size_t at  = 0;
    while (at < b.size()) {
        Span<u8> rest = b.subspan(at);
        size_t n      = utf8_decode(rest);
        u32 ch        = utf8_rune(rest);
        if (n == 0) {
            // A sequence the text ends in the middle of: one replacement.
            put(g, RUNE_REPLACEMENT);
            break;
        }
        put(g, ch == '\n' ? u32(' ') : ch);
        at += n;
    }
}

void Pane::write_at(Grid &g, u32 x, u32 y, Str text)
{
    if (x >= w_ || y >= h_)
        return;
    cx_ = x;
    cy_ = y;
    write(g, text);
}

void Pane::fill_row(Grid &g)
{
    while (cx_ < w_)
        put_raw(g, 0);
}

void Pane::clear(Grid &g)
{
    for (u32 y = 0; y < h_; y++) {
        cx_ = 0;
        cy_ = y;
        fill_row(g);
    }
    move_to(0, 0);
}

void Pane::place_cursor(Grid &g, u32 x, u32 y) const
{
    if (x >= w_ || y >= h_)
        return;
    g.cursor_x  = x_ + x;
    g.cursor_y  = y_ + y;
    g.cursor_on = true;
}

} // namespace koru
