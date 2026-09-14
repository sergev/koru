// SPDX-License-Identifier: MIT
//
// UTF-8, as much of Braam's kernel/text.h as the grid needs. Ported verbatim:
// the decoder's error behaviour is what test_ansi.cpp asserts.
#pragma once

#include <cstdint>
#include <string_view>

using Str = std::string_view;

// A codepoint the renderer can draw, which is what a cell may hold: a surrogate
// or a value past U+10FFFF becomes U+FFFD. Every writer to the grid goes
// through this.
constexpr char32_t rune_safe(char32_t c)
{
    uint32_t v = uint32_t(c);
    return (v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff)) ? char32_t(0xfffd) : c;
}

// Decodes the codepoint at s[at]. Returns the bytes consumed, or 0 when the
// sequence runs past the end. Every malformed sequence — a stray continuation
// byte, a lead that cannot start one, a missing continuation, an overlong, a
// surrogate, a value past U+10FFFF — yields U+FFFD, so bad input is visible
// rather than silently dropped.
size_t utf8_decode(Str s, size_t at, char32_t &out);

// Encodes one codepoint into `out`, which must hold four bytes, and returns the
// length. rune_safe first.
size_t utf8_encode(char32_t ch, char *out);
