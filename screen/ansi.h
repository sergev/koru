// SPDX-License-Identifier: MIT
//
// The escape-sequence parser: bytes in, grid operations out. It drives the
// screen through the screen_* calls and reaches nothing else, so the grid stays
// the model and an escape is one encoding into it.
//
// Braam's kernel/ansi.h. The state is a Term's, since a sequence may be split
// across two writes.
#pragma once

#include "screen.h"
#include "text.h"

#include <cstdint>

// Parameters a CSI sequence may carry; the rest are dropped, not an error.
enum : u32 { ANSI_PARAMS = 16 };

// Where the state machine is. Ground decodes UTF-8 and paints; the rest are one
// byte at a time.
enum : u8 {
    ANSI_GROUND,
    ANSI_ESC,
    ANSI_CSI,
    ANSI_STR,     // OSC, DCS, APC, PM: discarded up to BEL or ST
    ANSI_STR_ESC, // an ESC inside a string, which ST completes
};

struct Ansi {
    u8 state    = 0;
    u8 nparam   = 0;
    bool priv   = false; // a leading '?', '<', '=' or '>'
    bool inter  = false; // an intermediate byte: the sequence dispatches nothing
    bool bad    = false; // a runaway: consumed to its final byte, then dropped
    uint16_t len = 0;    // bytes in this sequence
    uint16_t param[ANSI_PARAMS] = {};

    // A rune split across two writes, held until the rest of it arrives.
    u8 pend[4] = {};
    u8 pend_n  = 0;

    bool lnm = false; // LF is a full new line; set at boot
    bool irm = false; // a glyph pushes the rest of the line right
    bool awm = false; // autowrap; set at boot

    // Italic is a foreground colour, and the one it replaced comes back.
    bool italic  = false;
    u8 italic_fg = 0;

    // DECSC: the cursor and the style, together.
    u32 save_x = 0, save_y = 0;
    u8 save_fg = 0, save_bg = 0, save_attrs = 0;
    bool save_italic  = false;
    u8 save_italic_fg = 0;

    u32 tabs[SCREEN_MAX_COLS / 32] = {};
};

// Ground, no parameters, LNM and DECAWM set, a tab stop every 8 columns.
void ansi_reset(Ansi &a);

// Bytes into the grid.
void ansi_write(Term &t, Ansi &a, Str utf8);
