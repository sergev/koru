// SPDX-License-Identifier: MIT
//
// Braam's `ui/textbuf.h` and `ui/view.h`: a buffer of logical lines, and a
// window onto it. It knows nothing about the screen and nothing about the
// filesystem — the program does its own I/O and hands the bytes over. The C++
// half of rust/runtime/src/textbuf.rs.
//
// A line holds UTF-8 and a cursor is a byte offset stepped by whole
// codepoints, so nothing here has to keep a decoded copy.

#ifndef KORU_TEXTBUF_HPP
#define KORU_TEXTBUF_HPP

#include <koru/grid.hpp>
#include <koru/vocab.hpp>

#include <vector>

namespace koru {

/// A buffer of logical lines. Always at least one: a file has somewhere to
/// type even when it is empty.
class TextBuf {
public:
    /// Splits on `\n`. A trailing newline does not make an extra empty line;
    /// an empty input is one empty line.
    result<void> load(Str utf8);

    /// One more line, for a reader that arrives a line at a time — the pager,
    /// which cannot hold the whole input twice.
    result<void> add(Str line)
    {
        lines_.emplace_back(line);
        return {};
    }

    /// Every line, newline-terminated, appended to `out`. What an editor
    /// writes back out.
    ///
    /// Everything that mutates a `TextBuf` answers a `result`, as Braam's
    /// does: there the allocator returns null and a full disk is a value, and
    /// an editor has to say so rather than lose the text. Here the allocator
    /// throws and none of them can fail — but a `src/cmd` source is written
    /// against the shape, and dropping it would be a source change.
    result<void> serialize(String &out) const;

    size_t lines() const { return lines_.empty() ? 1 : lines_.size(); }
    Str line(size_t i) const { return i < lines_.size() ? Str(lines_[i]) : Str(); }

    bool modified() const { return modified_; }
    void clear_modified() { modified_ = false; }

    // All of these clamp: an out-of-range line or offset does nothing.

    result<void> insert(size_t row, size_t at, Str utf8);

    /// Removes the codepoint starting at `at`. Returns its length in bytes.
    size_t erase(size_t row, size_t at);

    /// Splits `row` at `at`, so the tail becomes row + 1.
    result<void> split(size_t row, size_t at);

    /// Appends row + 1 to `row`. Where the join happened, or `Invalid` when
    /// there is no line after this one.
    result<size_t> join(size_t row);

    /// The byte offsets of the previous and next codepoint boundary in `row`.
    size_t prev(size_t row, size_t at) const;
    size_t next(size_t row, size_t at) const;

    /// Columns to the left of `at` — codepoints, not bytes, which is what a
    /// status line and a horizontal scroll both want.
    size_t column(size_t row, size_t at) const;

    /// The inverse: the byte offset `col` codepoints into the row.
    size_t offset(size_t row, size_t col) const;

private:
    void ensure(size_t row);
    /// A byte offset that is inside the row and on a boundary.
    size_t clamp(size_t row, size_t at) const;

    std::vector<String> lines_;
    bool modified_ = false;
};

/// A window onto a [`TextBuf`]: which line is at the top of the pane, how many
/// columns are scrolled off to the left, and the arithmetic that keeps a point
/// visible. A pager and an editor differ in what they do with keys, not in how
/// they scroll, so all of it is here and tested once.
class TextView {
public:
    size_t top() const { return top_; }
    size_t left() const { return left_; }

    /// Moves the window by `delta` lines, clamped so the last line is never
    /// scrolled past the top of the pane.
    void scroll(i32 delta, size_t lines, u32 height);

    /// Puts `row` at the top of the pane, under the same clamp.
    void to(size_t row, size_t lines, u32 height);

    /// Moves the window as little as it takes to show (row, col).
    void follow(size_t row, size_t col, u32 height, u32 width);

    /// Repaints every row of the pane, blanking those past the end of the text.
    void paint(Pane &p, Grid &g, const TextBuf &b) const;

    /// Braam's, over a pane that carries its grid.
    void paint(GridPane &p, const TextBuf &b) const { paint(p.pane(), p.grid(), b); }

private:
    size_t top_  = 0;
    size_t left_ = 0;
};

} // namespace koru

#endif // KORU_TEXTBUF_HPP
