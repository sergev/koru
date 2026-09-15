// SPDX-License-Identifier: MIT

#include <koru/opt.hpp>

#include <koru/filebuf.hpp>

#include <cerrno>

namespace koru {

namespace {

Span<u8> bytes_of(Str s)
{
    return Span<u8>(reinterpret_cast<const u8 *>(s.data()), s.size());
}

/// Whether the rune `c` appears in `set`. A byte scan would say yes to the
/// second byte of a two-byte letter.
bool holds(Str set, u32 c)
{
    Span<u8> b = bytes_of(set);
    size_t at  = 0;
    while (at < b.size()) {
        Span<u8> rest = b.subspan(at);
        size_t n      = utf8_decode(rest);
        if (n == 0)
            return false;
        if (utf8_rune(rest) == c)
            return true;
        at += n;
    }
    return false;
}

} // namespace

bool help_asked(const Args &args)
{
    return args.size() == 2 && (args[1] == "-h" || args[1] == "--help");
}

result<bool, OptError> OptParse::next(Opt &out)
{
    const Args &args = args_;
    if (at_ >= args.size())
        return false;

    Str w = args[at_];
    if (in_ == 0) {
        // Anything that is not a flag ends the options, `-` alone included.
        if (w.size() < 2 || w[0] != '-')
            return false;
        if (w == "--") {
            at_++;
            return false;
        }
        in_ = 1;
    }

    // A whole rune: a byte, as Braam reads, would slice a value out of
    // mid-sequence.
    Span<u8> rest = bytes_of(w).subspan(in_);
    size_t n      = utf8_decode(rest);
    if (n == 0)
        return false;
    char32_t c = char32_t(utf8_rune(rest));
    // Braam's `Opt::name` is one byte; the whole rune goes to the error.
    char lead = w[in_];
    in_ += n;
    bool last = in_ >= w.size();

    // A valued letter takes the rest of its word, or the next word. Either way
    // it ends the bundle.
    if (holds(spec_.valued, c)) {
        Str value;
        if (!last) {
            value = w.substr(in_);
            at_++;
        } else if (at_ + 1 < args.size()) {
            at_ += 2;
            value = args[at_ - 1];
        } else {
            at_++;
            in_ = 0;
            out = Opt{ lead, Str() }; // Braam reads the letter out of `out`
            return OptError{ c, Error::from_errno(Errno(ENOENT)) };
        }
        in_  = 0;
        out  = Opt{ lead, value };
        return true;
    }

    if (last) {
        at_++;
        in_ = 0;
    }
    // Advanced first, so a caller that carries on does not loop on the letter.
    if (!holds(spec_.flags, c)) {
        out = Opt{ lead, Str() };
        return OptError{ c, Error::from_errno(Errno(EINVAL)) };
    }
    out = Opt{ lead, Str() };
    return true;
}

} // namespace koru
