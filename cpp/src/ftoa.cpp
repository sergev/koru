// SPDX-License-Identifier: MIT
//
// Braam's `math/ftoa.h`, over the host's libc. See the header for why this one
// is a wrapper where everything else in this binding is a port.

#include <koru/ftoa.hpp>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace koru {

namespace {

// A printf conversion built from the pieces Braam passes separately. `flags`
// is filtered to the five printf takes, so nothing a caller puts there can
// reach the format string.
void build(char *fmt, size_t cap, i32 prec, char style, i32 width, Str flags)
{
    char *p   = fmt;
    char *end = fmt + cap - 1;
    *p++      = '%';
    for (size_t i = 0; i < flags.size() && p < end; i++)
        if (strchr("#0- +", flags[i]) && flags[i] != '\0')
            *p++ = flags[i];
    if (width > 0)
        p += size_t(snprintf(p, size_t(end - p), "%d", width));
    if (prec >= 0)
        p += size_t(snprintf(p, size_t(end - p), ".%d", prec));
    if (p < end)
        *p++ = strchr("feEgGaA", style) ? style : 'g';
    *p = '\0';
}

Str convert(char *out, size_t cap, f64 v, const char *fmt)
{
    if (cap == 0)
        return Str();
    int n = snprintf(out, cap, fmt, v);
    if (n < 0)
        return Str();
    size_t got = size_t(n);
    return Str(out, got < cap ? got : cap - 1); // truncated, as Buf is
}

} // namespace

Str fmt_f64(char *out, size_t cap, f64 v, i32 prec, char style)
{
    char fmt[32];
    build(fmt, sizeof fmt, prec, style, 0, Str());
    return convert(out, cap, v, fmt);
}

Str fmt_f64_padded(char *out, size_t cap, f64 v, i32 prec, char style, i32 width, Str flags)
{
    char fmt[48];
    build(fmt, sizeof fmt, prec, style, width, flags);
    return convert(out, cap, v, fmt);
}

Str fmt_f64_shortest(char *out, size_t cap, f64 v)
{
    for (int prec = 1; prec < 17; prec++) {
        char fmt[16];
        snprintf(fmt, sizeof fmt, "%%.%dg", prec);
        Str got = convert(out, cap, v, fmt);
        if (got.size() < cap - 1 && strtod(out, nullptr) == v)
            return got;
    }
    return fmt_f64(out, cap, v, 17, 'g');
}

Option<f64> scan_f64(Str s, size_t &used, i32 *err)
{
    String z(s); // strtod wants a terminator; a Str has none
    errno       = 0;
    char *stop  = nullptr;
    double v    = strtod(z.c_str(), &stop);
    used        = size_t(stop - z.c_str());
    if (err)
        *err = errno == ERANGE ? ERANGE : 0;
    if (used == 0)
        return None;
    return Option<f64>(v);
}

Option<float> scan_f32(Str s, size_t &used, i32 *err)
{
    String z(s);
    errno      = 0;
    char *stop = nullptr;
    float v    = strtof(z.c_str(), &stop);
    used       = size_t(stop - z.c_str());
    if (err)
        *err = errno == ERANGE ? ERANGE : 0;
    if (used == 0)
        return None;
    return Option<float>(v);
}

Option<f64> parse_f64(Str s)
{
    size_t used     = 0;
    Option<f64> got = scan_f64(s, used, nullptr);
    if (!got.has_value() || used != s.size())
        return None;
    return got;
}

} // namespace koru
