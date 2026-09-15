// SPDX-License-Identifier: MIT
//
// Braam's `proc/size.h`: `truncate(1)`'s SIZE operand. Pure — no syscall and
// nothing that could not be compiled into a unit test.

#ifndef KORU_SIZE_HPP
#define KORU_SIZE_HPP

#include <koru/vocab.hpp>

namespace koru {

/// What the leading punctuation means. `Set` is no modifier at all.
enum class SizeMod {
    Set,       // the size named
    Plus,      // + — longer by
    Minus,     // - — shorter by, floored at 0
    AtMost,    // < — shrink to it, or leave it alone
    AtLeast,   // > — grow to it, or leave it alone
    RoundDown, // / — down to a multiple of it
    RoundUp,   // % — up to a multiple of it
};

struct SizeSpec {
    SizeMod mod = SizeMod::Set;
    u64 n       = 0;
};

/// What `-o` counts in.
inline constexpr u64 SIZE_BLOCK = 512;

/// "100", "+1K", "%512". K, M, G and T are 1024; KB and its kin are 1000.
result<SizeSpec> parse_size(Str s);

/// The spec against a size. `Invalid` on a rounding to a multiple of zero, and
/// on an overflow past the largest offset there is.
result<u64> size_apply(SizeSpec spec, u64 cur);

} // namespace koru

#endif // KORU_SIZE_HPP
