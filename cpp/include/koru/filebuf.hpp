// SPDX-License-Identifier: MIT
//
// The half of a buffered stream that performs no syscall: the bytes in hand,
// the rune boundaries in them, and the room left. A [`File`] owns one, and
// this is where its fast path lives. The C++ half of
// rust/runtime/src/filebuf.rs.
//
// One buffer serves both directions: `held` is what a reader has not taken or
// a writer has not sent, `room` what may follow. Braam lends the block from
// its allocator; here the buffer owns a `String`, and an `Input` chunk is
// **moved in** rather than copied, which is the same zero-copy the lending
// buys.

#ifndef KORU_FILEBUF_HPP
#define KORU_FILEBUF_HPP

#include <koru/vocab.hpp>

namespace koru {

/// What [`FileBuf::take`] did.
enum class RuneStep {
    /// `out` holds a codepoint.
    Got,
    /// The bytes in hand do not finish one.
    Need,
};

/// What [`FileBuf::take_line`] did.
enum class LineStep {
    /// A newline was found and `out` holds the line.
    Done,
    /// `out` holds a fragment; there are more bytes to come.
    Need,
};

/// What a malformed sequence decodes to, and what `take_broken` yields.
inline constexpr u32 RUNE_REPLACEMENT = 0xfffd;

/// The bytes the sequence at the front of `s` takes, or 0 where it runs past
/// the end. **Every malformed sequence is one byte and U+FFFD**, so bad input
/// is visible rather than silently dropped — Braam's `utf8_decode`'s rule.
size_t utf8_decode(Span<u8> s);

/// The codepoint `utf8_decode` measured. Separate so the measuring is pure.
u32 utf8_rune(Span<u8> s);

/// `c` encoded into `out`, which must hold four bytes. Returns the length.
size_t utf8_encode(u32 c, u8 *out);

class FileBuf {
public:
    FileBuf() = default;

    /// A block of `cap` bytes, empty.
    static FileBuf with_capacity(size_t cap);

    /// Takes `v` whole, every byte of it held. There is no room to append
    /// after this: it is a view of somebody else's chunk, as Braam's is.
    void adopt(String v);

    /// Grow to `cap`, keeping what is held. False where it already is that big.
    bool regrow(size_t cap);

    bool ready() const { return !b_.empty(); }
    size_t size() const { return len_ - pos_; }
    bool is_empty() const { return pos_ == len_; }

    /// What a reader has not taken, or a writer has not sent.
    Span<u8> held() const
    {
        return Span<u8>(reinterpret_cast<const u8 *>(b_.data()) + pos_, len_ - pos_);
    }

    void consume(size_t n);

    /// Moves the held bytes to the front.
    void compact();

    /// Where the next fill lands, and how much of it there is room for.
    SpanMut<u8> tail()
    {
        return SpanMut<u8>(reinterpret_cast<u8 *>(b_.data()) + len_, b_.size() - len_);
    }

    size_t room() const { return b_.size() - len_; }
    void filled(size_t n) { len_ += n; }
    void reset() { pos_ = len_ = 0; }

    // ----------------------------------------------------------------- read

    RuneStep take(u32 &out);

    /// Consumes one byte and yields U+FFFD: a sequence end of input cut short.
    u32 take_broken();

    /// Puts `c` back, in front of the held bytes. False where there is no room.
    bool unget(u32 c);

    /// Appends the held bytes up to a newline, consuming what it appended.
    LineStep take_line(String &out, bool keep_nl);

    /// Whether a whole line is in hand: what `take_line` answers `Done` to.
    bool has_line() const;

    // ---------------------------------------------------------------- write

    bool append(Span<u8> s);

    /// The bytes the encoding took, or 0 where it would not fit.
    size_t append_rune(u32 c);

private:
    String b_;         // the block
    size_t pos_ = 0;   // taken prefix of the block
    size_t len_ = 0;   // one past the last byte held
};

} // namespace koru

#endif // KORU_FILEBUF_HPP
