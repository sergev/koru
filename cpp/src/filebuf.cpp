// SPDX-License-Identifier: MIT

#include <koru/filebuf.hpp>

#include <cstring>

namespace koru {

namespace {

bool is_cont(u8 b)
{
    return (b & 0xc0) == 0x80;
}

} // namespace

// ---------------------------------------------------------------------------
// UTF-8
// ---------------------------------------------------------------------------
//
// Hand-written, where the Rust half hands the candidate to `str::from_utf8`.
// The rules that check does for free are the four written out below: a
// continuation byte where a lead belongs, an overlong encoding, a surrogate,
// and anything past U+10FFFF.

size_t utf8_decode(Span<u8> s)
{
    if (s.empty())
        return 0;
    u8 lead    = s[0];
    size_t want = 0;
    if (lead < 0x80)
        return 1;
    else if (lead >= 0xc2 && lead <= 0xdf)
        want = 2;
    else if (lead >= 0xe0 && lead <= 0xef)
        want = 3;
    else if (lead >= 0xf0 && lead <= 0xf4)
        want = 4;
    else
        return 1; // a stray continuation, or a lead that cannot start one

    if (s.size() < want) {
        // Only a *prefix* of a valid sequence may ask for more bytes; a
        // truncated run whose continuations are already wrong is malformed now.
        for (size_t i = 1; i < s.size(); i++)
            if (!is_cont(s[i]))
                return 1;
        return 0;
    }
    for (size_t i = 1; i < want; i++)
        if (!is_cont(s[i]))
            return 1;
    // Overlong, surrogate, and past U+10FFFF, each a range on the second byte.
    if (want == 3 && lead == 0xe0 && s[1] < 0xa0)
        return 1;
    if (want == 3 && lead == 0xed && s[1] > 0x9f)
        return 1;
    if (want == 4 && lead == 0xf0 && s[1] < 0x90)
        return 1;
    if (want == 4 && lead == 0xf4 && s[1] > 0x8f)
        return 1;
    return want;
}

u32 utf8_rune(Span<u8> s)
{
    size_t n = utf8_decode(s);
    if (n == 0 || n == 1)
        return n == 1 && s[0] < 0x80 ? u32(s[0]) : RUNE_REPLACEMENT;

    u32 c = u32(s[0]) & (0xffu >> (n + 1));
    for (size_t i = 1; i < n; i++)
        c = (c << 6) | (u32(s[i]) & 0x3f);
    return c;
}

size_t utf8_encode(u32 c, u8 *out)
{
    if (c < 0x80) {
        out[0] = u8(c);
        return 1;
    }
    if (c < 0x800) {
        out[0] = u8(0xc0 | (c >> 6));
        out[1] = u8(0x80 | (c & 0x3f));
        return 2;
    }
    if (c < 0x10000) {
        out[0] = u8(0xe0 | (c >> 12));
        out[1] = u8(0x80 | ((c >> 6) & 0x3f));
        out[2] = u8(0x80 | (c & 0x3f));
        return 3;
    }
    out[0] = u8(0xf0 | (c >> 18));
    out[1] = u8(0x80 | ((c >> 12) & 0x3f));
    out[2] = u8(0x80 | ((c >> 6) & 0x3f));
    out[3] = u8(0x80 | (c & 0x3f));
    return 4;
}

// ---------------------------------------------------------------------------
// FileBuf
// ---------------------------------------------------------------------------

FileBuf FileBuf::with_capacity(size_t cap)
{
    FileBuf b;
    b.b_.assign(cap, '\0');
    return b;
}

void FileBuf::adopt(String v)
{
    len_ = v.size();
    b_   = std::move(v);
    pos_ = 0;
}

bool FileBuf::regrow(size_t cap)
{
    if (b_.size() >= cap)
        return false;
    compact();
    b_.resize(cap, '\0');
    return true;
}

void FileBuf::consume(size_t n)
{
    pos_ += n < size() ? n : size();
    if (pos_ == len_)
        pos_ = len_ = 0;
}

void FileBuf::compact()
{
    if (pos_ == 0)
        return;
    memmove(b_.data(), b_.data() + pos_, len_ - pos_);
    len_ -= pos_;
    pos_ = 0;
}

RuneStep FileBuf::take(u32 &out)
{
    Span<u8> h = held();
    size_t n   = utf8_decode(h);
    if (n == 0)
        return RuneStep::Need;
    out = utf8_rune(h);
    consume(n);
    return RuneStep::Got;
}

u32 FileBuf::take_broken()
{
    if (!is_empty())
        consume(1);
    return RUNE_REPLACEMENT;
}

bool FileBuf::unget(u32 c)
{
    u8 e[4];
    size_t n = utf8_encode(c, e);

    // Room in front, made by moving the held bytes along.
    if (pos_ < n) {
        if (size() + n > b_.size())
            return false;
        memmove(b_.data() + n, b_.data() + pos_, len_ - pos_);
        len_ = size() + n;
        pos_ = n;
    }
    pos_ -= n;
    memcpy(b_.data() + pos_, e, n);
    return true;
}

LineStep FileBuf::take_line(String &out, bool keep_nl)
{
    Span<u8> h = held();
    size_t nl  = 0;
    bool found = false;
    for (; nl < h.size(); nl++)
        if (h[nl] == '\n') {
            found = true;
            break;
        }
    if (!found) {
        out.append(reinterpret_cast<const char *>(h.data()), h.size());
        consume(h.size());
        return LineStep::Need;
    }
    size_t upto = keep_nl ? nl + 1 : nl;
    out.append(reinterpret_cast<const char *>(h.data()), upto);
    consume(nl + 1);
    return LineStep::Done;
}

bool FileBuf::has_line() const
{
    if (!ready())
        return false;
    Span<u8> h = held();
    for (size_t i = 0; i < h.size(); i++)
        if (h[i] == '\n')
            return true;
    return false;
}

bool FileBuf::append(Span<u8> s)
{
    if (s.size() > room())
        return false;
    memcpy(b_.data() + len_, s.data(), s.size());
    len_ += s.size();
    return true;
}

size_t FileBuf::append_rune(u32 c)
{
    u8 e[4];
    size_t n = utf8_encode(c, e);
    return append(Span<u8>(e, n)) ? n : 0;
}

} // namespace koru
