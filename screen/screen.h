// SPDX-License-Identifier: MIT
//
// The screen: a grid of cells in linear memory, which the renderer reads
// directly. Colours are fields and cursor addressing is indexing; an escape
// sequence is one encoding into that grid rather than the model, parsed by
// ansi.h on the way in.
//
// Braam's kernel/screen.h, ported. Three things are different, and doc/Notes.md
// says why each is forced:
//
//   - the browser-canvas descriptor and its magic do not port: the daemon owns
//     its grid, so `Screen` is the geometry alone and the cells are reached
//     with screen_cells() and screen_shown();
//   - there is no terminal registry, because KS_OP_TERM_OPEN answers -ENOSYS
//     until multiplexing exists: a `Term` is an object the daemon owns;
//   - `screen_flush` takes one terminal and hands back the damage it folded
//     the cursor into, where Braam's walks every terminal and calls the host.
//
// The cell is the protocol's, not a second one: that is what lets a blit be
// read in place.
#pragma once

#include "text.h"

#include <ks_abi.h>

#include <cstdint>

using u8  = uint8_t;
using u32 = uint32_t;
using i32 = int32_t;
using u64 = uint64_t;

// Braam's names for the protocol's values, so the port reads as its original.
using Cell = ks_cell;

enum : u8 {
    ATTR_BOLD      = KS_ATTR_BOLD,
    ATTR_UNDERLINE = KS_ATTR_UNDERLINE,
    ATTR_REVERSE   = KS_ATTR_REVERSE,
};

enum : u8 {
    COLOR_BLACK   = KS_COLOR_BLACK,
    COLOR_RED     = KS_COLOR_RED,
    COLOR_GREEN   = KS_COLOR_GREEN,
    COLOR_YELLOW  = KS_COLOR_YELLOW,
    COLOR_BLUE    = KS_COLOR_BLUE,
    COLOR_MAGENTA = KS_COLOR_MAGENTA,
    COLOR_CYAN    = KS_COLOR_CYAN,
    COLOR_WHITE   = KS_COLOR_WHITE,
    COLOR_BRIGHT  = KS_COLOR_BRIGHT,
};

// The grid is clamped to these, so a bad geometry cannot overflow the size
// computation or exhaust the heap. 1 MiB of cells at the maximum.
enum : u32 {
    SCREEN_MAX_COLS = KS_MAX_COLS,
    SCREEN_MAX_ROWS = KS_MAX_ROWS,
};

// Rows of scrollback. A row costs cols * 8 bytes; the ring is allocated on the
// first scroll, and without it there is simply no scrollback.
enum : u32 { SCREEN_SCROLLBACK = 512 };

// The geometry, which the renderer needs and a reply carries.
struct Screen {
    u32 cols = 0, rows = 0;
    u32 cursor_x = 0, cursor_y = 0; // cursor_x may equal cols: the wrap is deferred
    u32 cursor_on = 0;
};

struct Rect {
    u32 x = 0, y = 0, w = 0, h = 0;
};

struct Ansi;

// A terminal's grid and its sticky state. Copying one would copy two owned
// blocks, so it does not.
struct Term;

Term *term_new();
void term_free(Term *t);

// Reallocates the grid, keeping the rows in use and dropping from the top when
// they no longer fit, and clamps the geometry to the limits above — so a caller
// reads cols and rows back rather than assuming it got what it asked for.
// False when the grid could not be allocated, in which case the existing grid
// is left untouched.
bool screen_resize(Term &t, u32 cols, u32 rows);

const Screen &screen(const Term &t);

Cell *screen_cells(Term &t);

// What the renderer paints: the composed block while a scrollback view is up,
// and the live grid otherwise.
const Cell *screen_shown(const Term &t);

// Rows the grid has moved up, ever — a scroll's, and a resize's drop from the
// top. Nothing else counts them, so a writer holding an anchor row takes the
// difference across whatever it did to learn how far the anchor went.
u64 screen_scrolled(const Term &t);

// The scrollback view. While one is up the live grid is untouched underneath
// and screen_shown() points at a composed block instead. Negative pages back,
// clamped to the history there is; returns the resulting offset.
u32 screen_view_scroll(Term &t, i32 delta);

// Back to the live screen, and nothing when already there.
void screen_view_home(Term &t);

// Forgets the scrollback, bringing a view down first.
void screen_history_drop(Term &t);

// Rows the view sits above the live screen, and rows there are to page over.
u32 screen_view(const Term &t);
u32 screen_history(const Term &t);

// The cursor as the live screen has it; a view hides it.
bool screen_cursor_on(const Term &t);

void screen_style(Term &t, u8 fg, u8 bg, u8 attrs);

void screen_put(Term &t, char32_t ch);
void screen_write(Term &t, Str utf8);
void screen_newline(Term &t);
void screen_return(Term &t);

// One column back, erasing nothing; screen_backspace also blanks the cell.
void screen_left(Term &t);
void screen_backspace(Term &t);

void screen_move(Term &t, u32 x, u32 y);
void screen_cursor(Term &t, bool on);

void screen_clear(Term &t);

// Performs the deferred wrap if one is pending.
void screen_wrap(Term &t);

// The style the next put paints with.
void screen_style_get(const Term &t, u8 &fg, u8 &bg, u8 &attrs);

// ------------------------------------------------------- the scrolling region
//
// Rows top..bot, 0-origin and inclusive. Everything that scrolls moves rows
// inside it and leaves the rest of the grid alone. Inverted or out-of-range
// margins are refused, and a resize puts the whole screen back.

void screen_region(Term &t, u32 top, u32 bot);
u32 screen_region_top(const Term &t);
u32 screen_region_bot(const Term &t);

// The margins as *stored*, before the clamp the two accessors apply. For the
// fuzz oracle, which has to see the state rather than the guard over it:
// checking the clamped pair proves only that min() works.
void screen_region_stored(const Term &t, u32 &top, u32 &bot);

// Down one row, scrolling the region up at the bottom margin; up one row,
// scrolling it down at the top. The column is unchanged.
void screen_index(Term &t);
void screen_reverse_index(Term &t);

// The region's rows, n at a time. Rows leaving the top of a whole-screen region
// become scrollback and count in screen_scrolled(); a partial region's do
// neither. The cursor does not move.
void screen_scroll_up(Term &t, u32 n);
void screen_scroll_down(Term &t, u32 n);

// Rows at the cursor, within the region; nothing when the cursor is outside it.
void screen_insert_rows(Term &t, u32 n);
void screen_delete_rows(Term &t, u32 n);

// ------------------------------------------------------- cells and rows
//
// Every one of these writes blanks in the current background colour.

// Cells at the cursor, within its row; the row's tail falls off the end.
void screen_insert_cells(Term &t, u32 n);
void screen_delete_cells(Term &t, u32 n);

// n cells blank from the cursor, moving nothing.
void screen_erase_cells(Term &t, u32 n);

// 0 to the end of the row, 1 from its start, 2 the whole row.
void screen_erase_line(Term &t, u32 mode);

// 0 to the end of the screen, 1 from its start, 2 all of it. The cursor stays
// where it is, which is what tells this from screen_clear.
void screen_erase_display(Term &t, u32 mode);

// The cells changed since the last flush; w is 0 when none have. The cursor's
// own move is folded in by screen_flush, so it is not counted here.
Rect screen_damage(const Term &t);

// Marks a rectangle damaged, clipped to the grid, and makes every cell in it
// drawable. For a writer that fills cells through screen_cells() rather than
// through screen_put — a blit.
void screen_touch(Term &t, u32 x, u32 y, u32 w, u32 h);

// Folds the cursor's own move into the damage, hands the rectangle back and
// forgets it. One call per frame, by whoever is about to paint.
Rect screen_flush(Term &t);

// Margins, modes, tab stops and the saved cursor, as ESC c leaves them; the
// grid and the style are untouched.
void screen_ansi_reset(Term &t);

// Drops the grid. For tests, so a case starts from nothing.
void screen_reset(Term &t);

// Cell bytes this model holds: the grid, the ring and the view. For the test
// that asserts reset gives all three back; Braam asks its allocator.
size_t screen_bytes_in_use();

// The parser's state, which a write goes through. Declared here because a Term
// owns one and ansi.cpp is the only thing that looks inside it.
Ansi &screen_ansi(Term &t);
