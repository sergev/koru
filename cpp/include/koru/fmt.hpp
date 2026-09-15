// SPDX-License-Identifier: MIT
//
// Braam's `kernel/fmt.h`: a fixed-capacity text builder. Overflow truncates
// and sets a flag rather than allocating, which is what makes it safe on a
// panic path; nothing here is a syscall and nothing here allocates.
//
// The overload set differs from Braam's by necessity. There `usize` is 32 bits
// and `u64` is `unsigned long long`, so `put(u32)`, `put(usize)` and `put(u64)`
// are three types; here `usize` and `u64` are one, and `long` and `long long`
// are two. So the set is spelled in the built-in types rather than in the
// aliases, and covers every integer a program can hand it.

#ifndef KORU_FMT_HPP
#define KORU_FMT_HPP

#include <koru/text.hpp>
#include <koru/vocab.hpp>

namespace koru {

template <size_t N>
struct Buf {
    Buf &put(Str s)
    {
        for (size_t i = 0; i < s.size(); i++)
            put(s[i]);
        return *this;
    }

    Buf &put(char c)
    {
        if (n_ < N)
            b_[n_++] = c;
        else
            full_ = true;
        return *this;
    }

    /// A rune, encoded. Braam's `Opt::name` is a byte and this one is a
    /// codepoint, so the option parsers' diagnostics agree.
    Buf &put(char32_t c)
    {
        char t[4];
        size_t k = utf8_encode(c, t);
        for (size_t i = 0; i < k; i++)
            put(t[i]);
        return *this;
    }

    Buf &put(unsigned int v) { return digits(v); }
    Buf &put(unsigned long v) { return digits(v); }
    Buf &put(unsigned long long v) { return digits(v); }

    Buf &put(int v) { return signed_(v); }
    Buf &put(long v) { return signed_(v); }
    Buf &put(long long v) { return signed_(v); }

    /// Columns: padded to `w`, and something wider is written whole.
    Buf &put_right(Str s, size_t w)
    {
        for (size_t i = s.size(); i < w; i++)
            put(' ');
        return put(s);
    }

    Buf &put_left(Str s, size_t w)
    {
        put(s);
        for (size_t i = s.size(); i < w; i++)
            put(' ');
        return *this;
    }

    Buf &put_right(u64 v, size_t w)
    {
        char t[20];
        size_t k = 0;
        do {
            t[k++] = char('0' + u32(v % 10));
            v /= 10;
        } while (v);
        for (size_t i = k; i < w; i++)
            put(' ');
        while (k)
            put(t[--k]);
        return *this;
    }

    Buf &put_hex(u32 v)
    {
        put("0x");
        for (int shift = 28; shift >= 0; shift -= 4)
            put("0123456789abcdef"[(v >> shift) & 0xF]);
        return *this;
    }

    Str str() const { return Str(b_, n_); }

    bool overflowed() const { return full_; }

    void clear()
    {
        n_    = 0;
        full_ = false;
    }

private:
    template <class U>
    Buf &digits(U v)
    {
        char t[20];
        size_t k = 0;
        do {
            t[k++] = char('0' + unsigned(v % 10));
            v /= 10;
        } while (v);
        while (k)
            put(t[--k]);
        return *this;
    }

    template <class S>
    Buf &signed_(S v)
    {
        using U = std::make_unsigned_t<S>;
        if (v < 0) {
            put('-');
            return digits(U(-(v + 1)) + 1);
        }
        return digits(U(v));
    }

    char b_[N];
    size_t n_  = 0;
    bool full_ = false;
};

} // namespace koru

#endif // KORU_FMT_HPP
