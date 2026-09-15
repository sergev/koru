// SPDX-License-Identifier: MIT
//
// T48's host half: the grid, the panes, the text buffer and the view. All of
// it is pure — no ring, no socket — so ctest runs it with no VM. The client
// over a socket is the device half, in cpp/tests/screen.cpp.
//
// The cases are rust/runtime/src/{grid,textbuf}.rs's own, assertion for
// assertion.

#include "harness.hpp"

#include <koru/grid.hpp>
#include <koru/screen.hpp>
#include <koru/textbuf.hpp>

#include <cstring>

using namespace koru;

namespace {

Grid grid_of(u32 cols, u32 rows)
{
    Grid g;
    g.resize(cols, rows);
    g.take_damage(); // a resize damages everything; start from quiet
    return g;
}

u32 ch_at(const Grid &g, u32 x, u32 y)
{
    const ks_cell *c = g.at(x, y);
    return c ? c->ch : 0xffffffffu;
}

} // namespace

// ---------------------------------------------------------------------------
// Grid and Pane
// ---------------------------------------------------------------------------

CASE(grid_a_write_lands_where_the_pane_says_and_damages_only_that)
{
    Grid g = grid_of(8, 3);
    Pane p = Pane::of(g).sub(2, 1, 4, 1);
    p.write(g, "ab");
    CHECK_EQ(ch_at(g, 2, 1), u32('a'));
    CHECK_EQ(ch_at(g, 3, 1), u32('b'));
    CHECK_EQ(ch_at(g, 1, 1), 0);
    CHECK(g.damage() == (Rect{ 2, 1, 2, 1 }));
}

CASE(grid_a_write_past_the_edge_is_dropped_rather_than_wrapped)
{
    Grid g = grid_of(8, 2);
    Pane p = Pane::of(g).sub(0, 0, 3, 1);
    p.write(g, "abcdef");
    CHECK_EQ(ch_at(g, 2, 0), u32('c'));
    CHECK_EQ(ch_at(g, 3, 0), 0); // it did not run past the pane
    CHECK_EQ(ch_at(g, 0, 1), 0); // and it did not wrap
}

CASE(grid_a_sub_pane_is_clipped_to_its_parent)
{
    Grid g    = grid_of(8, 4);
    Pane root = Pane::of(g);
    Pane in   = root.sub(6, 3, 10, 10);
    CHECK_EQ(in.width(), 2);
    CHECK_EQ(in.height(), 1);
    Pane out = root.sub(9, 9, 2, 2);
    CHECK_EQ(out.width(), 0);
    CHECK_EQ(out.height(), 0);
}

CASE(grid_the_body_and_the_status_line_divide_the_screen)
{
    Grid g      = grid_of(8, 4);
    Pane root   = Pane::of(g);
    Pane body   = root.top(root.height() - 1);
    Pane status = root.bottom(1);
    CHECK_EQ(body.height(), 3);
    CHECK_EQ(status.height(), 1);

    // They do not overlap, which is the whole point of a pane.
    Grid g2 = grid_of(8, 4);
    Pane b  = body;
    Pane s  = status;
    b.write_at(g2, 0, 2, "body");
    s.write_at(g2, 0, 0, "stat");
    CHECK_EQ(ch_at(g2, 0, 2), u32('b'));
    CHECK_EQ(ch_at(g2, 0, 3), u32('s'));
}

CASE(grid_fill_row_blanks_in_the_panes_colours)
{
    Grid g = grid_of(4, 1);
    Pane p = Pane::of(g);
    p.style(KS_COLOR_RED, KS_COLOR_BLUE, 0);
    p.write(g, "x");
    p.fill_row(g);
    const ks_cell *c = g.at(3, 0);
    REQUIRE(c != nullptr);
    CHECK_EQ(c->ch, 0); // a blank cell, not a space
    CHECK_EQ(c->bg, KS_COLOR_BLUE);
}

CASE(grid_the_damage_is_one_rectangle_over_everything_written)
{
    Grid g = grid_of(8, 4);
    Pane p = Pane::of(g);
    p.write_at(g, 1, 1, "a");
    p.write_at(g, 5, 3, "b");
    CHECK(g.take_damage() == (Rect{ 1, 1, 5, 3 }));
    CHECK_EQ(g.damage().w, 0); // and taking it forgets it
}

CASE(grid_a_resize_damages_everything)
{
    Grid g;
    g.resize(4, 2);
    CHECK(g.damage() == (Rect{ 0, 0, 4, 2 }));
    CHECK_EQ(g.cells().size(), 8);
}

CASE(grid_the_cursor_is_the_grids_and_a_pane_places_it_in_its_own_coordinates)
{
    Grid g = grid_of(8, 4);
    Pane p = Pane::of(g).sub(2, 1, 4, 2);
    p.place_cursor(g, 1, 1);
    CHECK_EQ(g.cursor_x, 3);
    CHECK_EQ(g.cursor_y, 2);
    CHECK(g.cursor_on);
    p.place_cursor(g, 9, 9); // outside the pane: nothing moves
    CHECK_EQ(g.cursor_x, 3);
    CHECK_EQ(g.cursor_y, 2);
}

/// The cell **is** `ks_cell`, so a blit's payload is the grid's own bytes with
/// a header in front.
CASE(grid_a_blit_payload_is_the_header_and_then_the_cells)
{
    Grid g = grid_of(4, 2);
    Pane p = Pane::of(g);
    p.style(KS_COLOR_RED, KS_COLOR_BLUE, KS_ATTR_BOLD);
    p.write_at(g, 1, 0, "ab");

    String f = pack_blit(g, Rect{ 1, 0, 2, 1 });
    CHECK_EQ(f.size(), 40 + 2 * 8);
    const u8 *b = reinterpret_cast<const u8 *>(f.data());
    auto word   = [&](size_t i) { return u32(b[i * 4]) | (u32(b[i * 4 + 1]) << 8) |
                                       (u32(b[i * 4 + 2]) << 16) | (u32(b[i * 4 + 3]) << 24); };
    CHECK_EQ(word(0), 1); // x
    CHECK_EQ(word(1), 0); // y
    CHECK_EQ(word(2), 2); // w
    CHECK_EQ(word(3), 1); // h
    CHECK_EQ(word(7), 4); // cols
    CHECK_EQ(word(8), 2); // rows
    CHECK_EQ(word(9), 0); // the padding word the ABI reserves
    CHECK_EQ(word(10), u32('a'));
    CHECK_EQ(b[44], KS_COLOR_RED);
    CHECK_EQ(b[45], KS_COLOR_BLUE);
    CHECK_EQ(b[46], KS_ATTR_BOLD);
    CHECK_EQ(b[47], 0);
}

// ---------------------------------------------------------------------------
// TextBuf and TextView
// ---------------------------------------------------------------------------

namespace {

TextBuf loaded(Str text)
{
    TextBuf b;
    b.load(text);
    return b;
}

} // namespace

CASE(textbuf_a_trailing_newline_ends_the_last_line_and_does_not_add_one)
{
    TextBuf b = loaded("one\ntwo\n");
    CHECK_EQ(b.lines(), 2);
    CHECK(b.line(0) == "one");
    CHECK(b.line(1) == "two");

    CHECK_EQ(loaded("one\ntwo").lines(), 2);
    // An empty input is one empty line: a file always has somewhere to type.
    TextBuf e = loaded("");
    CHECK_EQ(e.lines(), 1);
    CHECK(e.line(0) == "");
    // And a lone newline is two.
    CHECK_EQ(loaded("\n\n").lines(), 2);
}

CASE(textbuf_editing_marks_the_buffer_and_serialising_puts_it_back)
{
    TextBuf b = loaded("ab\ncd");
    CHECK(!b.modified());
    b.insert(0, 1, "X");
    CHECK(b.line(0) == "aXb");
    CHECK(b.modified());
    CHECK_EQ(b.erase(0, 1), 1);
    CHECK(b.line(0) == "ab");
    String out;
    CHECK(b.serialize(out).ok() && out == "ab\ncd\n");
}

CASE(textbuf_a_split_and_a_join_are_inverses)
{
    TextBuf b = loaded("abcd");
    b.split(0, 2);
    CHECK(b.line(0) == "ab");
    CHECK(b.line(1) == "cd");
    CHECK_EQ(b.join(0).value(), size_t(2));
    CHECK(b.line(0) == "abcd");
    CHECK_EQ(b.lines(), 1);
    CHECK(b.join(0).error() == Kind::Invalid); // there is no line after the last
}

/// The offsets are bytes and the columns are codepoints, which is the
/// distinction the whole file exists to keep.
CASE(textbuf_a_cursor_steps_by_whole_codepoints)
{
    TextBuf b = loaded("a\xc3\xa9\xe2\x98\x83" "b"); // 1 + 2 + 3 + 1 bytes
    CHECK_EQ(b.line(0).size(), 7);
    CHECK_EQ(b.next(0, 0), 1);
    CHECK_EQ(b.next(0, 1), 3);
    CHECK_EQ(b.next(0, 3), 6);
    CHECK_EQ(b.prev(0, 6), 3);
    CHECK_EQ(b.prev(0, 3), 1);
    CHECK_EQ(b.column(0, 6), 3);
    CHECK_EQ(b.offset(0, 3), 6);
    CHECK_EQ(b.offset(0, 99), 7); // past the end is the end
    CHECK_EQ(b.next(0, 99), 7);
}

CASE(textbuf_erasing_a_multibyte_codepoint_takes_all_of_it)
{
    TextBuf b = loaded("a\xc3\xa9\xe2\x98\x83");
    CHECK_EQ(b.erase(0, 1), 2);
    CHECK(b.line(0) == "a\xe2\x98\x83");
    CHECK_EQ(b.erase(0, 1), 3);
    CHECK(b.line(0) == "a");
}

CASE(textview_never_scrolls_into_blankness)
{
    TextView v;
    v.scroll(100, 10, 4); // ten lines in a four-row pane
    CHECK_EQ(v.top(), 6); // the last line stays reachable
    v.scroll(-100, 10, 4);
    CHECK_EQ(v.top(), 0);

    // A text shorter than the pane never scrolls at all.
    TextView w;
    w.scroll(5, 3, 10);
    CHECK_EQ(w.top(), 0);
}

CASE(textview_follow_moves_the_window_as_little_as_it_takes)
{
    TextView v;
    v.follow(10, 0, 4, 20);
    CHECK_EQ(v.top(), 7);
    v.follow(7, 0, 4, 20);
    CHECK_EQ(v.top(), 7); // already visible: nothing moves
    v.follow(2, 0, 4, 20);
    CHECK_EQ(v.top(), 2);

    v.follow(2, 30, 4, 20);
    CHECK_EQ(v.left(), 11);
    v.follow(2, 0, 4, 20);
    CHECK_EQ(v.left(), 0);
}

CASE(textview_painting_blanks_the_rows_past_the_end_of_the_text)
{
    Grid g;
    g.resize(6, 3);
    TextBuf b = loaded("one\ntwo");
    Pane p    = Pane::of(g);
    TextView().paint(p, g, b);
    CHECK_EQ(ch_at(g, 0, 0), u32('o'));
    CHECK_EQ(ch_at(g, 0, 1), u32('t'));
    CHECK_EQ(ch_at(g, 0, 2), 0); // past the text, and blanked
}

CASE(textview_a_horizontal_scroll_starts_the_row_at_a_codepoint_boundary)
{
    Grid g;
    g.resize(4, 1);
    TextBuf b = loaded("a\xc3\xa9\xe2\x98\x83" "bcd");
    TextView v;
    v.follow(0, 4, 1, 4); // column 4 of six
    Pane p = Pane::of(g);
    v.paint(p, g, b);
    CHECK_EQ(v.left(), 1);
    CHECK_EQ(ch_at(g, 0, 0), 0xe9);
}

// ---------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------

CASE(screen_a_key_is_printable_only_without_a_command_modifier)
{
    CHECK((Key{ u32('a'), 0 }).printable());
    // Shift is part of the character, not a command.
    CHECK((Key{ u32('A'), MOD_SHIFT }).printable());
    CHECK(!(Key{ u32('c'), MOD_CTRL }).printable());
    CHECK(!(Key{ KEY_UP, 0 }).printable());
    CHECK(!(Key{ 0x7f, 0 }).printable());
}

CASE(screen_the_socket_path_prefers_the_environments_own)
{
    // Not a race with other cases: both variables are read, never written.
    const char *named = getenv("KORU_SCREEN_SOCK");
    String got        = sock_path();
    if (named && *named)
        CHECK(got == named);
    else
        CHECK(got.size() > strlen(KS_SOCK_NAME) &&
              got.compare(got.size() - strlen(KS_SOCK_NAME), String::npos, KS_SOCK_NAME) == 0);
}
