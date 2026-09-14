// SPDX-License-Identifier: MIT
//
// A buffered stream over a handle, in place of stdio: a buffer, so a character
// is not a syscall; runes, so `get` is a codepoint and not a byte; and a
// sticky error, checked once rather than per character. The C++ half of
// rust/runtime/src/file.rs.
//
// **Destroying a `File` neither flushes nor closes.** A destructor cannot
// await, so flushing is `flush`, `close`, or the at-exit hook the standard
// streams install. That is Braam's documented behaviour, and the reason is the
// same one.
//
// This is where the fast path earns its keep, and it earns more here than on
// Braam: a miss costs an `ENTER` syscall rather than a scheduler step. Every
// operation answers out of the buffer without suspending where it can — and a
// C++ coroutine that reaches no `co_await` still allocates a frame, so the
// fast halves below are ordinary functions the coroutine calls first.
//
// The three standard streams are `File &`, as Braam's are: C++ can hold a
// reference across a suspension point, which is the thing Rust will not do and
// the reason the Rust binding has a `Std` handle where this has none.

#ifndef KORU_FILE_HPP
#define KORU_FILE_HPP

#include <koru/filebuf.hpp>
#include <koru/iter.hpp>
#include <koru/ops.hpp>
#include <koru/task.hpp>
#include <koru/vocab.hpp>

namespace koru {

/// What `open` asks the filesystem for.
enum class FileMode {
    Read,
    /// Truncating, creating.
    Write,
    /// Creating, positioned at the end.
    Append,
    /// Read and write; a direction change costs a flush or a seek.
    Update,
};

enum class Buffering {
    /// Every operation is a syscall.
    None,
    /// Flushed when what was written holds a newline.
    Line,
    /// Flushed when the buffer fills.
    Full,
    /// `Line` on the console, `Full` otherwise; decided on the first flush.
    Auto,
};

/// The block a `File` takes when nothing asks for more.
inline constexpr size_t FILE_BUF = 512;

class File {
public:
    File() = default;
    ~File() = default; // neither flushes nor closes, on purpose

    File(const File &)            = delete;
    File &operator=(const File &) = delete;
    File(File &&)                 = default;
    File &operator=(File &&)      = default;

    static task<result<File>> open(Str path, FileMode m);

    /// Wraps a handle this `File` does not own and will not close.
    static File of(Handle fd, FileMode m);

    /// Reads the concatenation an `Input` names. Braam's takes `Input&` and
    /// says it must outlive the `File`; owning it says the same thing and
    /// cannot be got wrong.
    static File over(Input src);

    /// The three standard streams. `in`, `out` and `err` rather than Braam's
    /// `stdin`, `stdout` and `stderr`, which are macros in `<cstdio>` and so
    /// cannot be identifiers here.
    static File &in();
    static File &out();
    static File &err();

    // ----------------------------------------------------------------- state

    Handle fd() const { return fd_; }

    /// The first error this stream met. Braam's sentinel is `Error(0)`; this
    /// is an `Option`, as the Rust binding's is.
    Option<Error> error() const { return err_; }

    bool clean() const { return !err_.has_value(); }
    /// An end of input is not a failure.
    bool eof() const { return err_.has_value() && err_->is(Kind::Closed); }
    bool failed() const { return !clean() && !eof(); }
    void clear_err() { err_.reset(); }

    void set_buffering(Buffering b) { how_ = b; }

    /// Asks for a larger block than [`FILE_BUF`].
    void reserve(size_t n);

    /// Puts a rune back, in front of what is buffered, so `read` and `getline`
    /// see it too. False where there is no room in front.
    bool unget(u32 c);

    // ----------------------------------------------------------------- input

    /// One rune. `Closed` at end of input.
    task<result<u32>> get();

    /// As many bytes as are there, never more than the span. `Closed` at end
    /// of input, so a short read is never mistaken for one.
    task<result<size_t>> read(SpanMut<u8> into);

    /// One line, without its newline unless `keep_nl`. `false` at end of
    /// input; a final fragment with no newline is a line.
    task<result<bool>> getline(String &out, bool keep_nl);

    // ---------------------------------------------------------------- output

    /// One rune, encoded.
    task<result<void>> put(u32 c);

    /// Bytes, not runes: a UTF-8 sequence may straddle two calls.
    task<result<void>> write(Str s);
    task<result<void>> write_bytes(Span<u8> s);

    task<result<void>> flush();

    /// Discards the buffer.
    task<result<u64>> seek(i64 off, u32 whence);

    /// Flushes, then closes if this `File` opened the handle.
    task<result<void>> close();

    /// Gives the handle back. Unread bytes are wound off a seekable stream,
    /// and are `Unsupported` on one that is not.
    task<result<Handle>> detach();

    // -------------------------------------------------------------- scanning
    //
    // `scanf`'s conversions over a stream: one function each rather than a
    // format string, because nothing here takes `...` and a format defeats
    // every check the compiler could make.
    //
    // Two rules, both places where `scanf` is vague. **Leading whitespace
    // follows scanf**: the numeric ones and `scan_token` skip it, `scan_until`
    // does not. And these are **one pass, with no backtracking**.

    task<result<void>> skip_space();

    /// The next byte must be `c`; `false` and nothing taken where it is not.
    task<result<bool>> scan_lit(u8 c);

    /// `scanf`'s `%s`. `false` at end of input.
    task<result<bool>> scan_token(String &out, size_t width);

    /// `scanf`'s `%[^set]`. `false` where the first byte is already in `stop`.
    task<result<bool>> scan_until(String &out, Str stop, size_t width);

    /// `scanf`'s `%d %i %u %o %x`; `base` 0 is C's prefix rules.
    task<result<i64>> scan_i64(u32 base, size_t width);
    task<result<u64>> scan_u64(u32 base, size_t width);

private:
    static File bare(Handle fd, FileMode m);

    /// The first error sticks; a later one does not overwrite it.
    Error fail_with(Error e);

    bool readable() const;
    bool writable() const;
    void block_ready();
    bool buffers_nothing() const { return how_ == Buffering::None || how_ == Buffering::Auto; }

    // The fast halves: what the buffer alone answers, with no suspension on
    // any path. `true` means the answer is in `out`.
    bool take_fast(result<u32> &out);
    bool read_fast(SpanMut<u8> into, result<size_t> &out);
    bool put_fast(u32 c, result<void> &out);
    bool write_fast(Span<u8> s, result<void> &out);
    bool line_fast(String &line, bool keep_nl, result<bool> &out);

    task<result<size_t>> fill();
    task<result<void>> drain();
    task<void> probe();
    /// Turn the buffer around, flushing or winding back what was in it.
    task<result<void>> settle(bool to_write);
    /// The next byte without consuming it. `Closed` at end of input.
    task<result<u8>> peek();
    /// The run both token scanners are: bytes while `keep` accepts them.
    task<result<bool>> scan_run(String &out, size_t width, bool (*keep)(u8, Str), Str arg);
    task<result<std::pair<u64, bool>>> scan_number(u32 base, size_t width);

    FileBuf buf_;
    size_t want_ = FILE_BUF;
    Option<Input> src_;
    Handle fd_    = 0;
    FileMode mode_ = FileMode::Read;
    Buffering how_ = Buffering::Full;
    Option<Error> err_;
    bool own_fd_ = false;
    bool closed_ = false;
    /// The buffer holds output rather than input.
    bool writing_ = false;
    /// Flushed before this one refills: a prompt is out before what answers it
    /// is read. Only `in()` sets it, and only to `out()`.
    bool tie_out_ = false;
};

/// For a port that had `getchar` and `putchar`.
task<result<u32>> get_rune();
task<result<void>> put_rune(u32 c);
task<result<void>> write_out(Str s);
task<result<void>> write_err(Str s);

} // namespace koru

#endif // KORU_FILE_HPP
