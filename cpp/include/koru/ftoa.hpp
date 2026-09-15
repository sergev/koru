// SPDX-License-Identifier: MIT
//
// Braam's `math/ftoa.h`: floating point to text and back.
//
// **This is the one place the C++ binding stands on libc rather than porting.**
// Braam carries musl's `strtod` and printf engines because it has no libc at
// all; koru has one, and a second copy of `%g` would be a worse thing to own
// than a wrapper over the one already linked. The surface is Braam's; the
// engine underneath it is the host's.

#ifndef KORU_FTOA_HPP
#define KORU_FTOA_HPP

#include <koru/fmt.hpp>
#include <koru/vocab.hpp>

#include <cmath>

namespace koru {

/// Style is one of `f e g a` and their capitals, precision as printf's; -1 is
/// printf's default of 6. A longer conversion is truncated, as `Buf`'s is.
Str fmt_f64(char *out, size_t cap, f64 v, i32 prec = -1, char style = 'g');

/// The same, with printf's field width and flags — "#0- +" in any order.
Str fmt_f64_padded(char *out, size_t cap, f64 v, i32 prec, char style, i32 width, Str flags);

/// The fewest significant digits that parse back to `v` exactly.
Str fmt_f64_shortest(char *out, size_t cap, f64 v);

/// `strtod`'s grammar. None on a string that is not wholly one number.
Option<f64> parse_f64(Str s);

/// The same, stopping at the first character that cannot continue: `used` is
/// `strtod`'s endptr, 0 for none. `err` takes `ERANGE` where it did not fit.
Option<f64> scan_f64(Str s, size_t &used, i32 *err = nullptr);

/// The same at single precision, so it rounds once and not twice.
Option<float> scan_f32(Str s, size_t &used, i32 *err = nullptr);

/// 64 characters, so `%e` and `%g` always fit and `%f` does below 1e40.
template <size_t N>
Buf<N> &put_f64(Buf<N> &b, f64 v, i32 prec = -1, char style = 'g')
{
    char t[64];
    return b.put(fmt_f64(t, sizeof t, v, prec, style));
}

} // namespace koru

#endif // KORU_FTOA_HPP
