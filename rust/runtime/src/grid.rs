// SPDX-License-Identifier: MIT

//! Braam's `ui/grid.h` and `ui/pane.h`: a rectangle of cells, the damage done
//! to it, and the panes a program composes its screen out of.
//!
//! Pure — no ring, no socket. A program paints its own buffer and blits the
//! damage across, which is what `ProcScreen::flush` does with this.
//!
//! One thing is not Braam's, and Rust forces it: **a `Pane` does not hold the
//! grid.** Braam's holds a pointer, so a body pane and a status pane write
//! through two aliases of one buffer — which is exactly what the borrow
//! checker exists to refuse. A `Pane` here is the rectangle, the style and the
//! cursor; every write takes the grid. doc/Notes.md has the reasoning.

use crate::ks_abi::{KS_COLOR_BLACK, KS_COLOR_WHITE, KsCell};
use crate::vocab::Str;

/// Braam's `Rect`: a damage rectangle, `w == 0` when nothing is damaged.
#[derive(Copy, Clone, Default, PartialEq, Eq, Debug)]
pub struct Rect {
    pub x: u32,
    pub y: u32,
    pub w: u32,
    pub h: u32,
}

/// A rectangle of cells and the damage done to it. Deliberately *not* the
/// daemon's grid: this one is a program's own, and the damage is what travels.
#[derive(Clone, Default, Debug)]
pub struct Grid {
    cells: Vec<KsCell>,
    cols: u32,
    rows: u32,
    /// Where the real cursor goes; sent with the next blit.
    pub cursor_x: u32,
    pub cursor_y: u32,
    pub cursor_on: bool,
    damage: Rect,
}

impl Grid {
    pub fn cols(&self) -> u32 {
        self.cols
    }

    pub fn rows(&self) -> u32 {
        self.rows
    }

    pub fn cells(&self) -> &[KsCell] {
        &self.cells
    }

    /// Sizes the grid, blanks it and marks all of it damaged: everything is
    /// new, so the next flush repaints.
    pub fn resize(&mut self, cols: u32, rows: u32) {
        self.cells = vec![KsCell::default(); cols as usize * rows as usize];
        self.cols = cols;
        self.rows = rows;
        self.cursor_x = self.cursor_x.min(cols.saturating_sub(1));
        self.cursor_y = self.cursor_y.min(rows.saturating_sub(1));
        self.damage = Rect::default();
        self.touch(0, 0, cols, rows);
    }

    pub fn at(&self, x: u32, y: u32) -> Option<&KsCell> {
        if x < self.cols && y < self.rows {
            self.cells.get((y * self.cols + x) as usize)
        } else {
            None
        }
    }

    pub fn at_mut(&mut self, x: u32, y: u32) -> Option<&mut KsCell> {
        if x < self.cols && y < self.rows {
            self.cells.get_mut((y * self.cols + x) as usize)
        } else {
            None
        }
    }

    pub fn touch(&mut self, x: u32, y: u32, w: u32, h: u32) {
        if self.cells.is_empty() || w == 0 || h == 0 || x >= self.cols || y >= self.rows {
            return;
        }
        let x1 = (x + w).min(self.cols);
        let y1 = (y + h).min(self.rows);
        if self.damage.w == 0 {
            self.damage = Rect {
                x,
                y,
                w: x1 - x,
                h: y1 - y,
            };
            return;
        }
        let dx1 = (self.damage.x + self.damage.w).max(x1);
        let dy1 = (self.damage.y + self.damage.h).max(y1);
        self.damage.x = self.damage.x.min(x);
        self.damage.y = self.damage.y.min(y);
        self.damage.w = dx1 - self.damage.x;
        self.damage.h = dy1 - self.damage.y;
    }

    pub fn damage(&self) -> Rect {
        self.damage
    }

    /// The damage, and forgets it.
    pub fn take_damage(&mut self) -> Rect {
        std::mem::take(&mut self.damage)
    }
}

/// A rectangle of a grid with its own coordinates, style and cursor. It never
/// scrolls: scrolling moves the whole screen, which is what a full-screen
/// program must not do.
#[derive(Copy, Clone, Debug)]
pub struct Pane {
    x: u32,
    y: u32,
    w: u32,
    h: u32,
    cx: u32,
    cy: u32,
    fg: u8,
    bg: u8,
    attrs: u8,
}

impl Pane {
    /// The whole of a grid. Empty before it has been sized.
    pub fn of(g: &Grid) -> Pane {
        Pane::new(0, 0, g.cols(), g.rows())
    }

    pub fn new(x: u32, y: u32, w: u32, h: u32) -> Pane {
        Pane {
            x,
            y,
            w,
            h,
            cx: 0,
            cy: 0,
            fg: KS_COLOR_WHITE,
            bg: KS_COLOR_BLACK,
            attrs: 0,
        }
    }

    pub fn width(&self) -> u32 {
        self.w
    }

    pub fn height(&self) -> u32 {
        self.h
    }

    /// A sub-rectangle, in this pane's coordinates, clipped to it.
    pub fn sub(&self, x: u32, y: u32, w: u32, h: u32) -> Pane {
        if x >= self.w || y >= self.h {
            return Pane::new(self.x + self.w, self.y + self.h, 0, 0);
        }
        Pane::new(self.x + x, self.y + y, w.min(self.w - x), h.min(self.h - y))
    }

    /// The rows above `n` from the bottom, and those bottom `n` rows.
    pub fn top(&self, rows: u32) -> Pane {
        self.sub(0, 0, self.w, rows)
    }

    pub fn bottom(&self, rows: u32) -> Pane {
        let n = rows.min(self.h);
        self.sub(0, self.h - n, self.w, n)
    }

    pub fn style(&mut self, fg: u8, bg: u8, attrs: u8) {
        self.fg = fg;
        self.bg = bg;
        self.attrs = attrs;
    }

    /// Where the next `put` lands. Clamped, so a pane's cursor never leaves it.
    pub fn move_to(&mut self, x: u32, y: u32) {
        self.cx = if self.w > 0 { x.min(self.w - 1) } else { 0 };
        self.cy = if self.h > 0 { y.min(self.h - 1) } else { 0 };
    }

    pub fn cursor_x(&self) -> u32 {
        self.cx
    }

    pub fn cursor_y(&self) -> u32 {
        self.cy
    }

    /// One codepoint at the cursor, which advances. A write past the right
    /// edge is dropped rather than wrapped: a pane is a rectangle, not a
    /// stream.
    pub fn put(&mut self, g: &mut Grid, ch: char) {
        if self.cx >= self.w {
            return;
        }
        self.put_raw(g, u32::from(ch));
    }

    fn put_raw(&mut self, g: &mut Grid, ch: u32) {
        if self.cx >= self.w || self.cy >= self.h {
            return;
        }
        let (gx, gy) = (self.x + self.cx, self.y + self.cy);
        if let Some(c) = g.at_mut(gx, gy) {
            *c = KsCell {
                ch,
                fg: self.fg,
                bg: self.bg,
                attrs: self.attrs,
                rsvd0: 0,
            };
            g.touch(gx, gy, 1, 1);
        }
        self.cx += 1;
    }

    /// UTF-8, a codepoint at a time. A pane has no line discipline, so a
    /// newline shows as a space.
    pub fn write(&mut self, g: &mut Grid, text: Str<'_>) {
        for ch in text.chars() {
            self.put(g, if ch == '\n' { ' ' } else { ch });
        }
    }

    /// Writes at (x, y) and leaves the cursor after it.
    pub fn write_at(&mut self, g: &mut Grid, x: u32, y: u32, text: Str<'_>) {
        if x >= self.w || y >= self.h {
            return;
        }
        self.cx = x;
        self.cy = y;
        self.write(g, text);
    }

    /// Pads to the right edge with blanks in the current style. A blank cell,
    /// not a space: 0 is what the grid means by empty, and it still carries
    /// the colours, which is what a status line is made of.
    pub fn fill_row(&mut self, g: &mut Grid) {
        while self.cx < self.w {
            self.put_raw(g, 0);
        }
    }

    pub fn clear(&mut self, g: &mut Grid) {
        for y in 0..self.h {
            self.cx = 0;
            self.cy = y;
            self.fill_row(g);
        }
        self.move_to(0, 0);
    }

    /// Puts the real cursor here, in pane coordinates, so the daemon draws it.
    pub fn place_cursor(&self, g: &mut Grid, x: u32, y: u32) {
        if x >= self.w || y >= self.h {
            return;
        }
        g.cursor_x = self.x + x;
        g.cursor_y = self.y + y;
        g.cursor_on = true;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ks_abi::{KS_COLOR_BLUE, KS_COLOR_RED};

    fn grid(cols: u32, rows: u32) -> Grid {
        let mut g = Grid::default();
        g.resize(cols, rows);
        g.take_damage(); // a resize damages everything; start from quiet
        g
    }

    #[test]
    fn a_write_lands_where_the_pane_says_and_damages_only_that() {
        let mut g = grid(8, 3);
        let mut p = Pane::of(&g).sub(2, 1, 4, 1);
        p.write(&mut g, "ab");
        assert_eq!(g.at(2, 1).unwrap().ch, u32::from('a'));
        assert_eq!(g.at(3, 1).unwrap().ch, u32::from('b'));
        assert_eq!(g.at(1, 1).unwrap().ch, 0);
        assert_eq!(
            g.damage(),
            Rect {
                x: 2,
                y: 1,
                w: 2,
                h: 1
            }
        );
    }

    #[test]
    fn a_write_past_the_edge_is_dropped_rather_than_wrapped() {
        let mut g = grid(8, 2);
        let mut p = Pane::of(&g).sub(0, 0, 3, 1);
        p.write(&mut g, "abcdef");
        assert_eq!(g.at(2, 0).unwrap().ch, u32::from('c'));
        assert_eq!(g.at(3, 0).unwrap().ch, 0, "it did not run past the pane");
        assert_eq!(g.at(0, 1).unwrap().ch, 0, "and it did not wrap");
    }

    #[test]
    fn a_sub_pane_is_clipped_to_its_parent() {
        let g = grid(8, 4);
        let root = Pane::of(&g);
        let inner = root.sub(6, 3, 10, 10);
        assert_eq!((inner.width(), inner.height()), (2, 1));
        let outside = root.sub(9, 9, 2, 2);
        assert_eq!((outside.width(), outside.height()), (0, 0));
    }

    #[test]
    fn the_body_and_the_status_line_divide_the_screen() {
        let g = grid(8, 4);
        let root = Pane::of(&g);
        let body = root.top(root.height() - 1);
        let status = root.bottom(1);
        assert_eq!(body.height(), 3);
        assert_eq!(status.height(), 1);

        // They do not overlap, which is the whole point of a pane.
        let mut g2 = grid(8, 4);
        let mut b = body;
        let mut s = status;
        b.write_at(&mut g2, 0, 2, "body");
        s.write_at(&mut g2, 0, 0, "stat");
        assert_eq!(g2.at(0, 2).unwrap().ch, u32::from('b'));
        assert_eq!(g2.at(0, 3).unwrap().ch, u32::from('s'));
    }

    #[test]
    fn fill_row_blanks_in_the_panes_colours() {
        let mut g = grid(4, 1);
        let mut p = Pane::of(&g);
        p.style(KS_COLOR_RED, KS_COLOR_BLUE, 0);
        p.write(&mut g, "x");
        p.fill_row(&mut g);
        let c = g.at(3, 0).unwrap();
        assert_eq!(c.ch, 0, "a blank cell, not a space");
        assert_eq!(c.bg, KS_COLOR_BLUE);
    }

    #[test]
    fn the_damage_is_one_rectangle_over_everything_written() {
        let mut g = grid(8, 4);
        let mut p = Pane::of(&g);
        p.write_at(&mut g, 1, 1, "a");
        p.write_at(&mut g, 5, 3, "b");
        assert_eq!(
            g.take_damage(),
            Rect {
                x: 1,
                y: 1,
                w: 5,
                h: 3
            }
        );
        assert_eq!(g.damage().w, 0, "and taking it forgets it");
    }

    #[test]
    fn a_resize_damages_everything() {
        let mut g = Grid::default();
        g.resize(4, 2);
        assert_eq!(
            g.damage(),
            Rect {
                x: 0,
                y: 0,
                w: 4,
                h: 2
            }
        );
        assert_eq!(g.cells().len(), 8);
    }

    #[test]
    fn the_cursor_is_the_grids_and_a_pane_places_it_in_its_own_coordinates() {
        let mut g = grid(8, 4);
        let p = Pane::of(&g).sub(2, 1, 4, 2);
        p.place_cursor(&mut g, 1, 1);
        assert_eq!((g.cursor_x, g.cursor_y), (3, 2));
        assert!(g.cursor_on);
        p.place_cursor(&mut g, 9, 9); // outside the pane: nothing moves
        assert_eq!((g.cursor_x, g.cursor_y), (3, 2));
    }
}
