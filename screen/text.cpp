// SPDX-License-Identifier: MIT
//
// Braam's kernel/text.cpp, the two UTF-8 halves of it, with `usize` spelled
// `size_t` and `u8`/`u32` spelled out.

#include "text.h"

size_t utf8_encode(char32_t ch, char *out)
{
    uint32_t v = uint32_t(rune_safe(ch));

    if (v < 0x80) {
        out[0] = char(v);
        return 1;
    }
    if (v < 0x800) {
        out[0] = char(0xc0 | (v >> 6));
        out[1] = char(0x80 | (v & 0x3f));
        return 2;
    }
    if (v < 0x10000) {
        out[0] = char(0xe0 | (v >> 12));
        out[1] = char(0x80 | ((v >> 6) & 0x3f));
        out[2] = char(0x80 | (v & 0x3f));
        return 3;
    }
    out[0] = char(0xf0 | (v >> 18));
    out[1] = char(0x80 | ((v >> 12) & 0x3f));
    out[2] = char(0x80 | ((v >> 6) & 0x3f));
    out[3] = char(0x80 | (v & 0x3f));
    return 4;
}

size_t utf8_decode(Str s, size_t at, char32_t &out)
{
    if (at >= s.size())
        return 0;

    uint8_t c = uint8_t(s[at]);
    char32_t ch;
    size_t len;
    char32_t least; // the smallest value this length may spell
    if (c < 0x80) {
        out = c;
        return 1;
    } else if ((c & 0xe0) == 0xc0 && c >= 0xc2) { // c0 and c1 are overlong
        ch    = c & 0x1f;
        len   = 2;
        least = 0x80;
    } else if ((c & 0xf0) == 0xe0) {
        ch    = c & 0x0f;
        len   = 3;
        least = 0x800;
    } else if ((c & 0xf8) == 0xf0 && c <= 0xf4) { // f5 and up are past U+10FFFF
        ch    = c & 0x07;
        len   = 4;
        least = 0x10000;
    } else {
        // A stray continuation byte, or a lead that cannot start one. Before
        // the length check: bad input, not short input.
        out = 0xfffd;
        return 1;
    }

    if (at + len > s.size())
        return 0;

    // One byte on a bad continuation, so the next lead byte resynchronises.
    for (size_t k = 1; k < len; k++) {
        if ((uint8_t(s[at + k]) & 0xc0) != 0x80) {
            out = 0xfffd;
            return 1;
        }
        ch = (ch << 6) | (uint8_t(s[at + k]) & 0x3f);
    }

    // The shape was right, so all of it goes: one U+FFFD, not four.
    out = ch < least ? char32_t(0xfffd) : rune_safe(ch);
    return len;
}
