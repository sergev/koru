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

namespace koru {

/// What a program declares: the letters it takes, and which of those consume a
/// value. A valued letter need not appear in `flags`.
struct Opts {
    Str flags;  // "1CRSdhlr"
    Str valued; // "n" — takes the rest of the word, or the next one
};

/// One flag. `value` is empty unless the letter is in [`Opts::valued`]. The
/// name is a **rune**, not a byte: argv is attacker-supplied and slicing a
/// value out of mid-sequence would cut a codepoint in half.
struct Opt {
    u32 name  = 0;
    Str value;
};

/// A bad command line, and the letter at fault. Braam puts the letter in the
/// out-parameter; here the error carries it, and `TRY` still yields [`Error`].
struct OptError {
    u32 name = 0;
    Error error;
};

/// So `TRY` and `CO_TRY` convert an `OptError` like any other.
inline Error as_error(OptError e)
{
    return e.error;
}

/// `-h` or `--help` as the whole command line.
bool help_asked(const Args &args);

/// A cursor over argv, starting at argv[1]. The spec is held by value.
class OptParse {
public:
    OptParse(const Args &args, Opts spec) : args_(&args), spec_(spec) {}

    /// The next flag: `true` with `out` filled, `false` once the operands
    /// begin. `Invalid` is a letter the program does not take, `NotFound` a
    /// valued letter with nothing after it.
    result<bool, OptError> next(Opt &out);

    /// The operands, once `next` has reported `false`.
    Args rest() const { return args_->skip(at_); }

private:
    const Args *args_;
    Opts spec_;
    size_t at_ = 1; // the word being read
    size_t in_ = 0; // how far into that word's bundle, in bytes; 0 = not started
};

} // namespace koru

#endif // KORU_OPT_HPP
