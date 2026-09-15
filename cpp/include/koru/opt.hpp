// SPDX-License-Identifier: MIT
//
// Braam's `proc/opt.h`: bundled short flags, `--` to end them, and a flag that
// takes a value. Options come first and the first operand ends them. The C++
// half of rust/runtime/src/opt.rs.
//
// Allocation-free — every value views the `Args` handed in, which is why the
// parser borrows one where Braam's holds a span by value.

#ifndef KORU_OPT_HPP
#define KORU_OPT_HPP

#include <koru/args.hpp>
#include <koru/vocab.hpp>

#include <utility>

namespace koru {

/// What a program declares: the letters it takes, and which of those consume a
/// value. A valued letter need not appear in `flags`.
struct Opts {
    Str flags;  // "1CRSdhlr"
    Str valued; // "n" — takes the rest of the word, or the next one
};

/// One flag. `value` is empty unless the letter is in [`Opts::valued`].
///
/// `name` is a **byte**, as Braam's is — five of its programs take its address
/// as a one-byte `Str`. The parser still advances by whole runes, which is
/// what argv being attacker-supplied demands; a multi-byte letter reports its
/// lead byte here and the whole rune in [`OptError`].
struct Opt {
    char name = 0;
    Str value;
};

/// A bad command line, and the letter at fault. Braam puts the letter in the
/// out-parameter; here the error carries it, and `TRY` still yields [`Error`].
struct OptError {
    /// The whole rune, where [`Opt::name`] has room for one byte of it.
    char32_t name = 0;
    Error error;

    /// Braam's `next` answers `Result<bool>` and puts the letter in its
    /// out-parameter, so a program declares one. koru's does both.
    operator Error() const { return error; }
};

/// So `TRY` and `CO_TRY` convert an `OptError` like any other.
inline Error as_error(OptError e)
{
    return e.error;
}

/// `-h` or `--help` as the whole command line.
bool help_asked(const Args &args);

/// A cursor over argv, starting at argv[1]. Both the argument vector and the
/// spec are held **by value**, as Braam's are: `OptParse p(Args{ args.v.
/// subspan(n) }, ...)` is how three of its programs hand on the rest of a
/// command line, and a reference to that temporary dangles. An `Args` is a
/// counted handle, so holding one keeps the words alive as well.
class OptParse {
public:
    OptParse(Args args, Opts spec) : args_(std::move(args)), spec_(spec) {}

    /// The next flag: `true` with `out` filled, `false` once the operands
    /// begin. `Invalid` is a letter the program does not take, `NotFound` a
    /// valued letter with nothing after it.
    result<bool, OptError> next(Opt &out);

    /// The operands, once `next` has reported `false`.
    Args rest() const { return args_.skip(at_); }

private:
    Args args_;
    Opts spec_;
    size_t at_ = 1; // the word being read
    size_t in_ = 0; // how far into that word's bundle, in bytes; 0 = not started
};

} // namespace koru

#endif // KORU_OPT_HPP
