// SPDX-License-Identifier: MIT
//
// Braam's `kernel/text.h`: character classes, UTF-8 both ways, integer parsing
// and the scanners a tokeniser needs. Pure — no ring, no syscall, no libc
// locale.
//
// `utf8_encode` and `utf8_decode` are overloads beside filebuf.hpp's, not
// replacements: that pair measures a `Span<u8>` for the buffered stream, this
// one is Braam's `(char32_t, char *)` and `(Str, at, char32_t &)`.

#ifndef KORU_TEXT_HPP
#define KORU_TEXT_HPP

#include <koru/vocab.hpp>

namespace koru {

constexpr bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

constexpr bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

/// A codepoint the host can render: a surrogate or a value past U+10FFFF
/// becomes U+FFFD.
constexpr char32_t rune_safe(char32_t c)
{
    u32 v = u32(c);
    return (v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff)) ? char32_t(0xfffd) : c;
}

/// One codepoint into `out`, which must hold four bytes; returns the length.
size_t utf8_encode(char32_t ch, char *out);

/// The codepoint at `s[at]`, or 0 bytes where the sequence runs past the end.
/// Every malformed sequence yields U+FFFD, so bad input is visible.
size_t utf8_decode(Str s, size_t at, char32_t &out);

/// The other case of `c`, or `c` where there is none. ASCII, Latin-1, Latin
/// Extended-A, Greek and Cyrillic, by range.
char32_t rune_lower(char32_t c);
char32_t rune_upper(char32_t c);

/// Decimal, no sign, no leading space, all digits. None past 2^32 - 1.
Option<u32> parse_u32(Str s);

// --------------------------------------------------------- scanning a Str
//
// `scanf`'s conversions, one function each. `used` is `strtod`'s endptr in
// this tree's spelling, and is 0 on no match. Leading whitespace follows
// scanf: the numeric ones and `scan_token` skip it, `scan_until` does not.

size_t scan_space(Str s);

Option<i64> scan_i64(Str s, size_t &used, u32 base = 10, size_t width = 0);
Option<u64> scan_u64(Str s, size_t &used, u32 base = 10, size_t width = 0);

/// `scanf`'s `%s`. Empty, with `used` 0, at end of input.
Str scan_token(Str s, size_t &used, size_t width = 0);

/// `scanf`'s `%[^set]`. Empty, with `used` 0, when the first byte stops it.
Str scan_until(Str s, Str stop, size_t &used, size_t width = 0);

} // namespace koru

#endif // KORU_TEXT_HPP
