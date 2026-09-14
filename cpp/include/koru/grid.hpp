// SPDX-License-Identifier: MIT
//
// Braam's `ui/grid.h` and `ui/pane.h`: a rectangle of cells, the damage done
// to it, and the panes a program composes its screen out of. The C++ half of
// rust/runtime/src/grid.rs.
//
// Pure — no ring, no socket. A program paints its own buffer and blits the
// damage across, which is what `Screen::flush` does with this.
//
// **A `Pane` does not hold the grid**, here as in the Rust binding: two panes
// over one buffer would be two aliases of it, and every write taking the grid
// is what says so in a language with no borrow checker either. doc/Notes.md
// has the reasoning.

#ifndef KORU_GRID_HPP
#define KORU_GRID_HPP

#include <koru/vocab.hpp>

#include <ks_abi.h>

#include <vector>

namespace koru {

/// Braam's `Rect`: a damage rectangle, `w == 0` when nothing is damaged.
struct Rect {
    u32 x = 0, y = 0, w = 0, h = 0;

    friend bool operator==(const Rect &a, const Rect &b)
    {
        return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
    }
};

/// A rectangle of cells and the damage done to it. Deliberately *not* the
/// daemon's grid: this one is a program's own, and the damage is what travels.
///
/// The cell **is** `ks_cell`, which is what makes the protocol's
/// read-a-blit-in-place claim true.
class Grid {
public:
    u32 cols() const { return cols_; }
    u32 rows() const { return rows_; }
    Span<ks_cell> cells() const { return Span<ks_cell>(cells_.data(), cells_.size()); }

    /// Sizes the grid, blanks it and marks all of it damaged: everything is
    /// new, so the next flush repaints.
    void resize(u32 cols, u32 rows);

    const ks_cell *at(u32 x, u32 y) const;
    ks_cell *at_mut(u32 x, u32 y);

    void touch(u32 x, u32 y, u32 w, u32 h);

    Rect damage() const { return damage_; }

    /// The damage, and forgets it.
    Rect take_damage();

    /// Where the real cursor goes; sent with the next blit.
    u32 cursor_x   = 0;
    u32 cursor_y   = 0;
    bool cursor_on = false;

private:
    std::vector<ks_cell> cells_;
    u32 cols_ = 0;
    u32 rows_ = 0;
    Rect damage_;
};

/// A rectangle of a grid with its own coordinates, style and cursor. It never
/// scrolls: scrolling moves the whole screen, which is what a full-screen
/// program must not do.
class Pane {
public:
    Pane() = default;
    Pane(u32 x, u32 y, u32 w, u32 h);

    /// The whole of a grid. Empty before it has been sized.
    static Pane of(const Grid &g) { return Pane(0, 0, g.cols(), g.rows()); }

    u32 width() const { return w_; }
    u32 height() const { return h_; }

    /// A sub-rectangle, in this pane's coordinates, clipped to it.
    Pane sub(u32 x, u32 y, u32 w, u32 h) const;

    /// The rows above `n` from the bottom, and those bottom `n` rows.
    Pane top(u32 rows) const { return sub(0, 0, w_, rows); }
    Pane bottom(u32 rows) const;

    void style(u8 fg, u8 bg, u8 attrs);

    /// Where the next `put` lands. Clamped, so a pane's cursor never leaves it.
    void move_to(u32 x, u32 y);

    u32 cursor_x() const { return cx_; }
    u32 cursor_y() const { return cy_; }

    /// One codepoint at the cursor, which advances. A write past the right
    /// edge is dropped rather than wrapped: a pane is a rectangle, not a
    /// stream.
    void put(Grid &g, u32 ch);

    /// UTF-8, a codepoint at a time. A pane has no line discipline, so a
    /// newline shows as a space.
    void write(Grid &g, Str text);

    /// Writes at (x, y) and leaves the cursor after it.
    void write_at(Grid &g, u32 x, u32 y, Str text);

    /// Pads to the right edge with blanks in the current style. A blank cell,
    /// not a space: 0 is what the grid means by empty, and it still carries
    /// the colours, which is what a status line is made of.
    void fill_row(Grid &g);

    void clear(Grid &g);

    /// Puts the real cursor here, in pane coordinates, so the daemon draws it.
    void place_cursor(Grid &g, u32 x, u32 y) const;

private:
    void put_raw(Grid &g, u32 ch);

    u32 x_ = 0, y_ = 0, w_ = 0, h_ = 0;
    u32 cx_ = 0, cy_ = 0;
    u8 fg_    = KS_COLOR_WHITE;
    u8 bg_    = KS_COLOR_BLACK;
    u8 attrs_ = 0;
};

} // namespace koru

#endif // KORU_GRID_HPP
