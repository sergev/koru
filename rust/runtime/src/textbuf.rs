// SPDX-License-Identifier: MIT

//! Braam's `ui/textbuf.h` and `ui/view.h`: a buffer of logical lines, and a
//! window onto it. It knows nothing about the screen and nothing about the
//! filesystem — the program does its own I/O and hands the bytes over.
//!
//! A line holds UTF-8 and a cursor is a byte offset stepped by whole
//! codepoints, so nothing here has to keep a decoded copy. Rust's `String` is
//! UTF-8 by type, so the `is_cont` walk Braam needs is `char_indices` here and
//! the offsets mean the same thing.

use crate::grid::{Grid, Pane};
use crate::vocab::Str;

/// A buffer of logical lines. Always at least one: a file has somewhere to
/// type even when it is empty.
#[derive(Clone, Default, Debug)]
pub struct TextBuf {
    lines: Vec<String>,
    modified: bool,
}

impl TextBuf {
    /// Splits on `\n`. A trailing newline does not make an extra empty line;
    /// an empty input is one empty line.
    pub fn load(&mut self, utf8: Str<'_>) {
        self.lines.clear();
        let mut at = 0;
        loop {
            match utf8[at..].find('\n') {
                Some(nl) => {
                    self.lines.push(utf8[at..at + nl].to_string());
                    at += nl + 1;
                    if at == utf8.len() {
                        break;
                    }
                }
                None => {
                    self.lines.push(utf8[at..].to_string());
                    break;
                }
            }
        }
        if self.lines.is_empty() {
            self.lines.push(String::new());
        }
        self.modified = false;
    }

    /// One more line, for a reader that arrives a line at a time — the pager,
    /// which cannot hold the whole input twice.
    pub fn add(&mut self, line: Str<'_>) {
        self.lines.push(line.to_string());
    }

    /// Every line, newline-terminated. What an editor writes back out.
    pub fn serialize(&self) -> String {
        let mut out = String::new();
        for l in &self.lines {
            out.push_str(l);
            out.push('\n');
        }
        out
    }

    pub fn lines(&self) -> usize {
        self.lines.len().max(1)
    }

    pub fn line(&self, i: usize) -> Str<'_> {
        self.lines.get(i).map_or("", String::as_str)
    }

    pub fn modified(&self) -> bool {
        self.modified
    }

    pub fn clear_modified(&mut self) {
        self.modified = false;
    }

    fn ensure(&mut self, row: usize) {
        while self.lines.len() <= row {
            self.lines.push(String::new());
        }
    }

    /// All of these clamp: an out-of-range line or offset does nothing.
    pub fn insert(&mut self, row: usize, at: usize, utf8: Str<'_>) {
        self.ensure(row);
        let at = self.clamp(row, at);
        self.lines[row].insert_str(at, utf8);
        self.modified = true;
    }

    /// Removes the codepoint starting at `at`. Returns its length in bytes.
    pub fn erase(&mut self, row: usize, at: usize) -> usize {
        if row >= self.lines.len() || at >= self.lines[row].len() {
            return 0;
        }
        let end = self.next(row, at);
        self.lines[row].replace_range(at..end, "");
        self.modified = true;
        end - at
    }

    /// Splits `row` at `at`, so the tail becomes row + 1.
    pub fn split(&mut self, row: usize, at: usize) {
        self.ensure(row);
        let at = self.clamp(row, at);
        let tail = self.lines[row].split_off(at);
        self.lines.insert(row + 1, tail);
        self.modified = true;
    }

    /// Appends row + 1 to `row`. Returns where the join happened.
    pub fn join(&mut self, row: usize) -> Option<usize> {
        if row + 1 >= self.lines.len() {
            return None;
        }
        let at = self.lines[row].len();
        let tail = self.lines.remove(row + 1);
        self.lines[row].push_str(&tail);
        self.modified = true;
        Some(at)
    }

    /// The byte offsets of the previous and next codepoint boundary in `row`.
    pub fn prev(&self, row: usize, at: usize) -> usize {
        let s = self.line(row);
        let mut at = at.min(s.len());
        while at > 0 {
            at -= 1;
            if s.is_char_boundary(at) {
                break;
            }
        }
        at
    }

    pub fn next(&self, row: usize, at: usize) -> usize {
        let s = self.line(row);
        if at >= s.len() {
            return s.len();
        }
        let mut at = at + 1;
        while at < s.len() && !s.is_char_boundary(at) {
            at += 1;
        }
        at
    }

    /// Columns to the left of `at` — codepoints, not bytes, which is what a
    /// status line and a horizontal scroll both want.
    pub fn column(&self, row: usize, at: usize) -> usize {
        let s = self.line(row);
        s[..at.min(s.len())].chars().count()
    }

    /// The inverse: the byte offset `col` codepoints into the row.
    pub fn offset(&self, row: usize, col: usize) -> usize {
        let s = self.line(row);
        s.char_indices().nth(col).map_or(s.len(), |(i, _)| i)
    }

    /// A byte offset that is inside the row and on a boundary.
    fn clamp(&self, row: usize, at: usize) -> usize {
        let s = self.line(row);
        let at = at.min(s.len());
        if s.is_char_boundary(at) {
            at
        } else {
            self.prev(row, at)
        }
    }
}

/// A window onto a [`TextBuf`]: which line is at the top of the pane, how many
/// columns are scrolled off to the left, and the arithmetic that keeps a point
/// visible. A pager and an editor differ in what they do with keys, not in how
/// they scroll, so all of it is here and tested once.
#[derive(Copy, Clone, Default, Debug)]
pub struct TextView {
    top: usize,
    left: usize,
}

impl TextView {
    pub fn top(&self) -> usize {
        self.top
    }

    pub fn left(&self) -> usize {
        self.left
    }

    /// Moves the window by `delta` lines, clamped so the last line is never
    /// scrolled past the top of the pane.
    pub fn scroll(&mut self, delta: i32, lines: usize, height: u32) {
        let most = lines.saturating_sub(height as usize);
        if delta < 0 {
            self.top = self.top.saturating_sub(delta.unsigned_abs() as usize);
        } else {
            self.top += delta as usize;
        }
        self.top = self.top.min(most);
    }

    /// Puts `row` at the top of the pane, under the same clamp.
    pub fn to(&mut self, row: usize, lines: usize, height: u32) {
        self.top = row.min(lines.saturating_sub(height as usize));
    }

    /// Moves the window as little as it takes to show (row, col).
    pub fn follow(&mut self, row: usize, col: usize, height: u32, width: u32) {
        if height == 0 || width == 0 {
            return;
        }
        if row < self.top {
            self.top = row;
        } else if row >= self.top + height as usize {
            self.top = row - height as usize + 1;
        }
        if col < self.left {
            self.left = col;
        } else if col >= self.left + width as usize {
            self.left = col - width as usize + 1;
        }
    }

    /// Repaints every row of the pane, blanking those past the end of the text.
    pub fn paint(&self, p: &mut Pane, g: &mut Grid, b: &TextBuf) {
        let lines = b.lines();
        for y in 0..p.height() {
            p.move_to(0, y);
            let row = self.top + y as usize;
            if row < lines {
                let s = b.line(row);
                p.write(g, &s[b.offset(row, self.left).min(s.len())..]);
            }
            p.fill_row(g);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn loaded(text: &str) -> TextBuf {
        let mut b = TextBuf::default();
        b.load(text);
        b
    }

    #[test]
    fn a_trailing_newline_ends_the_last_line_and_does_not_add_one() {
        let b = loaded("one\ntwo\n");
        assert_eq!(b.lines(), 2);
        assert_eq!(b.line(0), "one");
        assert_eq!(b.line(1), "two");

        let b = loaded("one\ntwo");
        assert_eq!(b.lines(), 2);

        // An empty input is one empty line: a file always has somewhere to type.
        let b = loaded("");
        assert_eq!(b.lines(), 1);
        assert_eq!(b.line(0), "");

        // And a lone newline is two.
        let b = loaded("\n\n");
        assert_eq!(b.lines(), 2);
    }

    #[test]
    fn editing_marks_the_buffer_and_serialising_puts_it_back() {
        let mut b = loaded("ab\ncd");
        assert!(!b.modified());
        b.insert(0, 1, "X");
        assert_eq!(b.line(0), "aXb");
        assert!(b.modified());
        assert_eq!(b.erase(0, 1), 1);
        assert_eq!(b.line(0), "ab");
        assert_eq!(b.serialize(), "ab\ncd\n");
    }

    #[test]
    fn a_split_and_a_join_are_inverses() {
        let mut b = loaded("abcd");
        b.split(0, 2);
        assert_eq!((b.line(0), b.line(1)), ("ab", "cd"));
        assert_eq!(b.join(0), Some(2));
        assert_eq!(b.line(0), "abcd");
        assert_eq!(b.lines(), 1);
        assert_eq!(b.join(0), None, "there is no line after the last");
    }

    /// The offsets are bytes and the columns are codepoints, which is the
    /// distinction the whole file exists to keep.
    #[test]
    fn a_cursor_steps_by_whole_codepoints() {
        let b = loaded("aé☃b"); // 1 + 2 + 3 + 1 bytes
        assert_eq!(b.line(0).len(), 7);
        assert_eq!(b.next(0, 0), 1);
        assert_eq!(b.next(0, 1), 3);
        assert_eq!(b.next(0, 3), 6);
        assert_eq!(b.prev(0, 6), 3);
        assert_eq!(b.prev(0, 3), 1);
        assert_eq!(b.column(0, 6), 3);
        assert_eq!(b.offset(0, 3), 6);
        assert_eq!(b.offset(0, 99), 7, "past the end is the end");
        assert_eq!(b.next(0, 99), 7);
    }

    #[test]
    fn erasing_a_multibyte_codepoint_takes_all_of_it() {
        let mut b = loaded("aé☃");
        assert_eq!(b.erase(0, 1), 2);
        assert_eq!(b.line(0), "a☃");
        assert_eq!(b.erase(0, 1), 3);
        assert_eq!(b.line(0), "a");
    }

    #[test]
    fn the_view_never_scrolls_into_blankness() {
        let mut v = TextView::default();
        v.scroll(100, 10, 4); // ten lines in a four-row pane
        assert_eq!(v.top(), 6, "the last line stays reachable");
        v.scroll(-100, 10, 4);
        assert_eq!(v.top(), 0);

        // A text shorter than the pane never scrolls at all.
        let mut v = TextView::default();
        v.scroll(5, 3, 10);
        assert_eq!(v.top(), 0);
    }

    #[test]
    fn follow_moves_the_window_as_little_as_it_takes() {
        let mut v = TextView::default();
        v.follow(10, 0, 4, 20);
        assert_eq!(v.top(), 7);
        v.follow(7, 0, 4, 20);
        assert_eq!(v.top(), 7, "already visible: nothing moves");
        v.follow(2, 0, 4, 20);
        assert_eq!(v.top(), 2);

        v.follow(2, 30, 4, 20);
        assert_eq!(v.left(), 11);
        v.follow(2, 0, 4, 20);
        assert_eq!(v.left(), 0);
    }

    #[test]
    fn painting_blanks_the_rows_past_the_end_of_the_text() {
        let mut g = Grid::default();
        g.resize(6, 3);
        let b = loaded("one\ntwo");
        let mut p = Pane::of(&g);
        TextView::default().paint(&mut p, &mut g, &b);
        assert_eq!(g.at(0, 0).unwrap().ch, u32::from('o'));
        assert_eq!(g.at(0, 1).unwrap().ch, u32::from('t'));
        assert_eq!(g.at(0, 2).unwrap().ch, 0, "past the text, and blanked");
    }

    #[test]
    fn a_horizontal_scroll_starts_the_row_at_a_codepoint_boundary() {
        let mut g = Grid::default();
        g.resize(4, 1);
        let b = loaded("aé☃bcd");
        let mut v = TextView::default();
        v.follow(0, 4, 1, 4); // column 4 of six
        let mut p = Pane::of(&g);
        v.paint(&mut p, &mut g, &b);
        assert_eq!(v.left(), 1);
        assert_eq!(g.at(0, 0).unwrap().ch, u32::from('é'));
    }
}
