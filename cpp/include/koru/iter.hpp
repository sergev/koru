// SPDX-License-Identifier: MIT
//
// The three iterators Braam's programs read through: the files named on a
// command line as one stream, that stream split into lines, and a directory
// tree walked pre-order. The C++ half of rust/runtime/src/iter.rs.
//
// `LineReader` and `TreeWalk` have the same fast-path shape a `File` does: a
// line already in the buffer, or an entry the walk already listed, must not
// cost a syscall.

#ifndef KORU_ITER_HPP
#define KORU_ITER_HPP

#include <koru/args.hpp>
#include <koru/ops.hpp>
#include <koru/task.hpp>
#include <koru/vocab.hpp>

#include <vector>

namespace koru {

/// The files named on a command line, read end to end as one stream — `wc a b`.
/// One is open at a time: each is opened when the read reaches it and closed
/// before the next.
///
/// `who` names this program in the diagnostic a failed open prints, and is
/// unused where no path was named.
class Input {
public:
    Input() = default;
    Input(Args paths, Handle fallback, Str who);

    Input(const Input &)            = delete;
    Input &operator=(const Input &) = delete;
    Input(Input &&)                 = default;
    Input &operator=(Input &&)      = default;

    /// The next chunk of the concatenation, or `Closed` at the end of it. A
    /// file that will not open is reported here, on stderr, and comes back as
    /// its own error — so a caller's Cancelled-is-130 mapping still holds.
    task<result<String>> read();

private:
    Args paths_;
    String who_;
    size_t at_ = 0;
    /// The file `at_` names, once opened. 0 where none is.
    Handle cur_ = 0;
    /// The fallback, where no path was named.
    Handle fd_ = 0;
    bool own_  = false;
};

/// Splits an [`Input`] into lines. A line may span any number of chunks, and a
/// final fragment with no newline is a line.
class LineReader {
public:
    explicit LineReader(Input &&src) : src_(std::move(src)) {}

    /// Braam's `LineReader(Input &)`, which keeps a pointer and says in its
    /// header that the `Input` must outlive it. This **moves** it in, so that
    /// lifetime rule is enforced rather than documented — the same trade
    /// `File(Input &)` makes. Two constructors and not one by value: an lvalue
    /// matches both, and that is ambiguous.
    explicit LineReader(Input &src) : src_(std::move(src)) {}

    /// `out` holds the next line, without its newline. `false` past the last
    /// one.
    task<result<bool>> next(String &out);

private:
    friend struct LineReaderPeek;

    /// What the bytes in hand answer: a whole line, or the last fragment once
    /// the input is spent. False means "read more".
    bool take(String &out);

    Input src_;
    String buf_;
    /// Consumed prefix of `buf_`, compacted when it refills.
    size_t pos_ = 0;
    bool eof_   = false;
};

/// Everything under `root`, pre-order, with an explicit stack rather than
/// recursion: a deep tree must not be a deep chain of coroutine frames.
///
/// Descends on a directory alone, so a link is handed over rather than
/// followed and no cycle guard is needed. `root` itself is not reported — a
/// caller that wants it stats it.
class TreeWalk {
public:
    explicit TreeWalk(Str root);

    /// How much of a reported path is the root: everything past `root_len()`
    /// is what lies under it, leading `/` and all.
    size_t root_len() const { return root_ == "/" ? 0 : root_.size(); }

    /// Whichever directory the last error was about.
    Str at() const { return at_; }

    /// `true` with `path` the whole path from the root and `e` the entry;
    /// `false` past the last one. A directory that will not list is an error
    /// naming it in [`at`](Self::at), and that level is dropped — so a caller
    /// that reports and calls again walks the rest.
    task<result<bool>> next(String &path, DirEntry &e);

private:
    struct Level {
        std::vector<DirEntry> ents;
        size_t at = 0;
        String path;
    };

    /// One entry off the levels in hand. Deepest first, and a level with
    /// nothing left is popped.
    bool report(String &path, DirEntry &out);

    String root_;
    std::vector<Level> levels_;
    /// The directory reported last, listed on the next call.
    Option<String> pending_;
    /// Whichever directory the last error was about.
    String at_;
    bool began_ = false;
};

} // namespace koru

#endif // KORU_ITER_HPP
